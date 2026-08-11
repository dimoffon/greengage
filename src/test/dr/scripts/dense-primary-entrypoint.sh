#!/bin/bash
# dense-primary-entrypoint.sh -- production side of the V-20 regression fixture.
#
# V-20: production's VACUUM truncating trailing pages of a protected topology
# catalog must NOT truncate the DR replica's copy of it.  The DR's copy holds
# frozen-seeded DR-local rows and legitimately has a different page count, so
# applying production's truncation point can discard the DR's entire topology.
#
# The default fixture cannot reproduce this: with 3 segment rows in a 32 KB page
# the DR's seed has ample room on block 0, so there is nothing above production's
# truncation point to lose.  This script builds the state a real, large cluster is
# in -- a DENSE catalog -- which is what pushes the seed's rows onto a trailing
# block:
#
#   1. fill gp_segment_configuration_internal with filler rows, then delete them in the
#      SAME transaction, so they are dead the moment they become visible (no
#      other backend, FTS included, ever sees them) but still occupy their pages;
#   2. base-backup in that state -- the DR inherits full pages and a free-space
#      map that reports them full, so its seed's INSERTs extend the relation and
#      its live rows land on a trailing block;
#   3. after the DR is built, VACUUM the catalog: the dead fillers go, the
#      trailing pages become empty, and production truncates -- emitting the
#      XLOG_SMGR_TRUNCATE record that is the subject of the test.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin

FILLER_ROWS=${FILLER_ROWS:-5000}
MIN_PAGES=${MIN_PAGES:-3}

cd "$DEMO"
rm -f "$DEMO/gpdemo-env.sh"
log "dense-primary: creating demo cluster (coordinator + 2 segments, no mirrors) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=2 WITH_MIRRORS=false 2>&1 | tail -12
source "$DEMO/gpdemo-env.sh"

COORD_DATADIR=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir from gp_segment_configuration where content=-1;")
grep -q 'replication.*trust' "$COORD_DATADIR/pg_hba.conf" || \
	echo 'host replication all 0.0.0.0/0 trust' >> "$COORD_DATADIR/pg_hba.conf"

log "dense-primary: enabling per-instance WAL archiving to $WAL_ARCHIVE"
while IFS=$'\t' read -r datadir port dbid content; do
	mkdir -p "$WAL_ARCHIVE/seg$content"
	if ! grep -q 'GREENGAGE DR ARCHIVE' "$datadir/postgresql.conf"; then
		cat >> "$datadir/postgresql.conf" <<EOF

# --- GREENGAGE DR ARCHIVE ---
wal_level = replica
archive_mode = on
archive_timeout = 30
archive_command = 'test ! -f $WAL_ARCHIVE/seg%c/%f && cp %p $WAL_ARCHIVE/seg%c/%f'
# V-20 fixture: vacuum timing is the whole experiment, so it must be ours alone.
# With autovacuum on, the filler tuples could be reclaimed (and the relation
# truncated) before the base backup, quietly turning this back into the easy case.
autovacuum = off
EOF
	fi
done < <(list_instances)
gpstop -ar -q

# A marker table so the DR has user data to read.
psql -p "$PORT_BASE" -d postgres -q -c \
	"drop table if exists dense_marker;
	 create table dense_marker(id int, note text) distributed by (id);
	 insert into dense_marker values (1,'created on production');" || true

# --- densify the protected topology catalog ---------------------------------
u() { PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -Atc "$1"; }

BLKSZ=$(u "select current_setting('block_size')::int;")
pages() { u "select pg_relation_size('gp_segment_configuration_internal') / $BLKSZ;"; }

log "dense-primary: catalog before densify: $(pages) page(s) (block_size=$BLKSZ)"
log "dense-primary: inserting + deleting $FILLER_ROWS filler rows in ONE transaction ..."
# One transaction: the fillers are dead as soon as they are visible, so no other
# backend ever observes a phantom segment, but their pages stay occupied because
# nothing vacuums them.  Deletes do not update the free-space map, so the FSM
# keeps reporting these pages full -- which is exactly what makes the DR's seed
# extend the relation instead of reusing block 0.
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

PROD_PAGES_DENSE=$(pages)
PROD_LIVE=$(u "select count(*) from gp_segment_configuration_internal;")
PROD_MAX_BLK=$(u "select max(substring(ctid::text from '\((\d+),')::int) from gp_segment_configuration_internal;")
log "dense-primary: catalog is now $PROD_PAGES_DENSE page(s); $PROD_LIVE live row(s), highest live block $PROD_MAX_BLK"
[ "$PROD_PAGES_DENSE" -ge "$MIN_PAGES" ] || \
	die "densify produced only $PROD_PAGES_DENSE page(s); need >= $MIN_PAGES (raise FILLER_ROWS)"
[ "$PROD_LIVE" = 3 ] || die "expected 3 live rows after densify, got $PROD_LIVE"

# Production must still dispatch -- the fillers were never visible, so it should.
if psql -p "$PORT_BASE" -d postgres -Atc "select count(*) from dense_marker;" >/dev/null 2>&1; then
	log "dense-primary: OK -- distributed query still dispatches after densify"
else
	die "distributed query broke after densify"
fi

# --- base backup in the dense state -----------------------------------------
log "dense-primary: base-backup each instance to $BASEBACKUP (dense catalog)"
while IFS=$'\t' read -r datadir port dbid content; do
	rm -rf "$BASEBACKUP/seg$content"
	pg_basebackup -h localhost -p "$port" -X stream --no-sync \
		-D "$BASEBACKUP/seg$content" --target-gp-dbid "$dbid"
	log "dense-primary:   content $content (dbid $dbid) -> $BASEBACKUP/seg$content"
done < <(list_instances)

psql -p "$PORT_BASE" -d postgres -Atc \
	"select dbid||E'\t'||content||E'\t'||role||E'\t'||preferred_role||E'\t'||mode||E'\t'||status||E'\t'||port||E'\tdr\tdr\t'||datadir
	   from gp_segment_configuration order by content;" > "$TOPO_FILE"

# A restore point the DR can pause at BEFORE the truncation, so it can be
# inspected in the pre-truncate state.
psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dense_rp_pre');" >/dev/null 2>&1 || true
psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
for _ in 1 2 3; do psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true; sleep 2; done

{
	echo "PROD_PAGES_DENSE=$PROD_PAGES_DENSE"
	echo "PROD_MAX_LIVE_BLK=$PROD_MAX_BLK"
	echo "FILLER_ROWS=$FILLER_ROWS"
} > "$ARCHIVE/dense_prod_state.env"
touch "$READY_MARKER"
log "dense-primary: ready (dense base backups + topology published, dense_rp_pre archived)"

# --- wait for the DR, then truncate -----------------------------------------
log "dense-primary: waiting for the DR to finish building ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/dense_dr_built" ] && break; sleep 2; done
[ -f "$ARCHIVE/dense_dr_built" ] || die "timed out waiting for the DR to build"

log "dense-primary: VACUUM gp_segment_configuration_internal (reclaims the fillers -> truncates) ..."
u "vacuum gp_segment_configuration_internal;" >/dev/null
PROD_PAGES_AFTER=$(pages)
log "dense-primary: catalog after VACUUM: $PROD_PAGES_AFTER page(s) (was $PROD_PAGES_DENSE)"
if [ "$PROD_PAGES_AFTER" -lt "$PROD_PAGES_DENSE" ]; then
	log "dense-primary: OK -- relation shrank, so an XLOG_SMGR_TRUNCATE was emitted"
else
	die "VACUUM did not truncate ($PROD_PAGES_DENSE -> $PROD_PAGES_AFTER); the fixture cannot test V-20"
fi

psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dense_rp_post');" >/dev/null 2>&1 || true
psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
for _ in 1 2 3; do psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true; sleep 2; done

echo "PROD_PAGES_AFTER=$PROD_PAGES_AFTER" >> "$ARCHIVE/dense_prod_state.env"
touch "$ARCHIVE/dense_truncate_done"
log "dense-primary: truncation archived; dense_rp_post created"
log "dense-primary: done; idling so the cluster keeps archiving WAL"
exec sleep infinity
