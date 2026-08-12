#!/bin/bash
# maint-primary-entrypoint.sh -- production side of the V-13' fixture.
#
# V-13' asks the inverse of the question the dense/V-20 fixture asked.  That one
# tested whether a DR replica's frozen-seeded topology survived production
# truncating the shared catalog it lived in.  The replica's topology is not in
# that catalog any more -- it is in $PGDATA/gp_topology, which production's WAL
# cannot reach -- so the question is now simply: can production run ordinary
# maintenance on its topology catalog while a replica is attached?
#
# Under the old design the answer was no.  VACUUM FULL / CLUSTER / REINDEX /
# TRUNCATE rewrite a mapped shared catalog's relfilenode and emit a shared relmap
# update, and DRRejectForbiddenRemap() halted the replica with FATAL on exactly
# that record ("re-seed this node").  That guard is deleted; this fixture is what
# says so.
#
# Production runs four operations, each followed by its own restore point so the
# DR advances across them one at a time and a failure attributes to one:
#
#   1. VACUUM             -- truncates trailing pages: XLOG_SMGR_TRUNCATE
#   2. VACUUM FULL        -- rewrites the relation: new relfilenode + relmap update
#   3. REINDEX            -- rewrites the indexes: new relfilenodes + relmap update
#   4. TRUNCATE + refill  -- rewrites the relation again, inside one transaction
#                            so no other backend ever sees an empty topology
#
# The densify step survives from the old fixture for one reason: without dead
# tuples to reclaim, VACUUM would not truncate and operation 1 would be a no-op.
#
# Each step publishes its post-op page count and the relation's relfilenode, so
# the DR side can assert the rewrite actually happened rather than passing on a
# fixture that quietly did nothing.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin
reset_archive    # this run's /archive starts empty; the volume outlives `down`

FILLER_ROWS=${FILLER_ROWS:-5000}
MIN_PAGES=${MIN_PAGES:-3}
STATE=$ARCHIVE/maint_prod_state.env

cd "$DEMO"
rm -f "$DEMO/gpdemo-env.sh"
log "maint-primary: creating demo cluster (coordinator + 2 segments, no mirrors) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=2 WITH_MIRRORS=false 2>&1 | tail -12
source "$DEMO/gpdemo-env.sh"

COORD_DATADIR=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir from gp_segment_configuration where content=-1;")
grep -q 'replication.*trust' "$COORD_DATADIR/pg_hba.conf" || \
	echo 'host replication all 0.0.0.0/0 trust' >> "$COORD_DATADIR/pg_hba.conf"

log "maint-primary: enabling per-instance WAL archiving to $WAL_ARCHIVE"
while IFS=$'\t' read -r datadir port dbid content; do
	mkdir -p "$WAL_ARCHIVE/seg$content"
	if ! grep -q 'GREENGAGE DR ARCHIVE' "$datadir/postgresql.conf"; then
		cat >> "$datadir/postgresql.conf" <<EOF

# --- GREENGAGE DR ARCHIVE ---
wal_level = replica
archive_mode = on
archive_timeout = 30
archive_command = 'test ! -f $WAL_ARCHIVE/seg%c/%f && cp %p $WAL_ARCHIVE/seg%c/%f'
wal_consistency_checking = '${WAL_CONSISTENCY_CHECKING:-all}'
# Vacuum timing is the experiment, so it must be ours alone: autovacuum could
# reclaim the fillers (and truncate) before the base backup, turning operation 1
# into a no-op without saying so.
autovacuum = off
EOF
	fi
done < <(list_instances)
gpstop -ar -q

# User data the DR can read -- distributed, so the DR side's read has to dispatch
# to a segment rather than answer from the coordinator.
psql -p "$PORT_BASE" -d postgres -q -c \
	"drop table if exists maint_marker;
	 create table maint_marker(id int, note text) distributed by (id);
	 insert into maint_marker select g, 'row '||g from generate_series(1,100) g;" || true

u() { PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -Atc "$1"; }

BLKSZ=$(u "select current_setting('block_size')::int;")
pages()      { u "select pg_relation_size('gp_segment_configuration_internal') / $BLKSZ;"; }
# pg_relation_filenode(), NOT pg_class.relfilenode: these are MAPPED shared
# catalogs, so pg_class.relfilenode is 0 for them and the real filenode lives in
# global/pg_filenode.map.  That mapping is exactly what a rewrite changes, and
# what the relmap update record carries.
relfilenode(){ u "select pg_relation_filenode('gp_segment_configuration_internal'::regclass);"; }
# REINDEX rewrites the INDEXES, not the heap, so the heap's filenode is unchanged
# by it -- the index's is what proves that operation did anything.
idxrelfilenode(){ u "select pg_relation_filenode('gp_segment_config_dbid_index'::regclass);"; }
liverows()   { u "select count(*) from gp_segment_configuration_internal;"; }

# --- densify, so operation 1 has something to reclaim ------------------------
log "maint-primary: catalog before densify: $(pages) page(s) (block_size=$BLKSZ)"
log "maint-primary: inserting + deleting $FILLER_ROWS filler rows in ONE transaction ..."
PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -q <<SQL
BEGIN;
INSERT INTO gp_segment_configuration_internal
       (dbid, content, role, preferred_role, mode, status, port, hostname, address, datadir)
SELECT 10000 + g, 10000 + g, 'm', 'm', 'n', 'd', 40000 + g,
       'filler', 'filler', '/nonexistent/filler'
  FROM generate_series(1, $FILLER_ROWS) g;
DELETE FROM gp_segment_configuration_internal WHERE content >= 10000;
COMMIT;
SQL

PAGES_DENSE=$(pages)
LIVE=$(liverows)
log "maint-primary: catalog is now $PAGES_DENSE page(s); $LIVE live row(s)"
[ "$PAGES_DENSE" -ge "$MIN_PAGES" ] || \
	die "densify produced only $PAGES_DENSE page(s); need >= $MIN_PAGES (raise FILLER_ROWS)"
[ "$LIVE" = 3 ] || die "expected 3 live rows after densify, got $LIVE"

# --- base backup + published topology ----------------------------------------
log "maint-primary: base-backup each instance to $BASEBACKUP"
while IFS=$'\t' read -r datadir port dbid content; do
	rm -rf "$BASEBACKUP/seg$content"
	pg_basebackup -h localhost -p "$port" -X stream --no-sync \
		-D "$BASEBACKUP/seg$content" --target-gp-dbid "$dbid"
	log "maint-primary:   content $content (dbid $dbid) -> $BASEBACKUP/seg$content"
done < <(list_instances)

psql -p "$PORT_BASE" -d postgres -Atc \
	"select dbid||E'\t'||content||E'\t'||role||E'\t'||preferred_role||E'\t'||mode||E'\t'||status||E'\t'||port||E'\tdr\tdr\t'||datadir
	   from gp_segment_configuration order by content;" > "$TOPO_FILE"

# archive_and_rp <restore point name> -- create it and force it to the archive.
archive_and_rp() {
	psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('$1');" >/dev/null 2>&1 || true
	psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
	for _ in 1 2 3; do
		psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true
		sleep 2
	done
	log "maint-primary: restore point '$1' created and archived"
}

RFN_PRE=$(relfilenode)
{
	echo "PAGES_DENSE=$PAGES_DENSE"
	echo "FILLER_ROWS=$FILLER_ROWS"
	echo "RFN_PRE=$RFN_PRE"
	echo "IRFN_PRE=$(idxrelfilenode)"
} > "$STATE"
archive_and_rp maint_rp_pre
touch "$READY_MARKER"
log "maint-primary: ready (base backups + topology published; catalog relfilenode $RFN_PRE)"

log "maint-primary: waiting for the DR to finish building ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/maint_dr_built" ] && break; sleep 2; done
[ -f "$ARCHIVE/maint_dr_built" ] || die "timed out waiting for the DR to build"

# --- the four maintenance operations -----------------------------------------
# publish <tag> -- record this operation's page count and relfilenode, then take
# its restore point.  Recording BEFORE the restore point matters: the DR reads
# this file after advancing to that point, so it must already describe it.
publish() {
	local tag=$1 p r i
	p=$(pages); r=$(relfilenode); i=$(idxrelfilenode)
	{
		echo "PAGES_$tag=$p"
		echo "RFN_$tag=$r"
		echo "IRFN_$tag=$i"
	} >> "$STATE"
	log "maint-primary: after $tag: $p page(s), heap relfilenode $r, index relfilenode $i"
	archive_and_rp "maint_rp_$(echo "$tag" | tr 'A-Z' 'a-z')"
}

log "maint-primary: [1/4] VACUUM gp_segment_configuration_internal ..."
u "vacuum gp_segment_configuration_internal;" >/dev/null
PAGES_VACUUM=$(pages)
[ "$PAGES_VACUUM" -lt "$PAGES_DENSE" ] || \
	die "VACUUM did not truncate ($PAGES_DENSE -> $PAGES_VACUUM); operation 1 would be vacuous"
log "maint-primary: relation shrank $PAGES_DENSE -> $PAGES_VACUUM, so XLOG_SMGR_TRUNCATE was emitted"
publish VACUUM

log "maint-primary: [2/4] VACUUM FULL gp_segment_configuration_internal ..."
u "vacuum full gp_segment_configuration_internal;" >/dev/null
RFN_NOW=$(relfilenode)
[ "$RFN_NOW" != "$RFN_PRE" ] || \
	die "VACUUM FULL did not change the relfilenode ($RFN_NOW); no relmap update was emitted"
log "maint-primary: relfilenode $RFN_PRE -> $RFN_NOW, so a shared relmap update was emitted"
publish VACUUMFULL

log "maint-primary: [3/4] REINDEX TABLE gp_segment_configuration_internal ..."
IRFN_BEFORE=$(idxrelfilenode)
u "reindex table gp_segment_configuration_internal;" >/dev/null
IRFN_AFTER=$(idxrelfilenode)
[ "$IRFN_AFTER" != "$IRFN_BEFORE" ] || \
	die "REINDEX did not change the index relfilenode ($IRFN_AFTER); no relmap update was emitted"
log "maint-primary: index relfilenode $IRFN_BEFORE -> $IRFN_AFTER, so a shared relmap update was emitted"
publish REINDEX

# Operation 4 rewrites the relation one more time.  All in ONE transaction: a
# TRUNCATE that committed on its own would leave production briefly describing no
# cluster, and any backend building a gang in that window would fail.
log "maint-primary: [4/4] TRUNCATE + re-insert gp_segment_configuration_internal ..."
PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -q <<'SQL'
BEGIN;
CREATE TEMP TABLE maint_topo_save AS
       SELECT * FROM gp_segment_configuration_internal;
TRUNCATE gp_segment_configuration_internal;
INSERT INTO gp_segment_configuration_internal SELECT * FROM maint_topo_save;
DROP TABLE maint_topo_save;
COMMIT;
SQL
LIVE_AFTER=$(liverows)
[ "$LIVE_AFTER" = 3 ] || die "after TRUNCATE + re-insert the catalog has $LIVE_AFTER rows, expected 3"
publish TRUNCATE

# Production must still be a working cluster after all four.
if psql -p "$PORT_BASE" -d postgres -Atc "select count(*) from maint_marker;" >/dev/null 2>&1; then
	log "maint-primary: OK -- distributed query still dispatches after all four operations"
else
	die "production stopped dispatching after the maintenance operations"
fi

touch "$ARCHIVE/maint_ops_done"
log "maint-primary: all four operations archived; done, idling so WAL keeps flowing"
exec sleep infinity
