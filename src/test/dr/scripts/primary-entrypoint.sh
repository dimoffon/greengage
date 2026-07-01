#!/bin/bash
# primary-entrypoint.sh -- bring up the production cluster, enable per-segment
# WAL archiving to the shared volume, base-backup each instance for DR seeding,
# publish the DR-local topology, then keep running (and archiving).
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin   # re-execs as gpadmin, sources greengage_path.sh

cd "$DEMO"

# Remove any gpdemo-env.sh baked into the image from a host build, then create.
rm -f "$DEMO/gpdemo-env.sh"
log "primary: creating demo cluster (coordinator + 2 segments, no mirrors) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=2 WITH_MIRRORS=false 2>&1 | tail -25
source "$DEMO/gpdemo-env.sh"

log "primary: instances:"
list_instances | sed 's/^/    /'

# Allow local replication connections for pg_basebackup (-h localhost).
COORD_DATADIR=$(psql -p "$PORT_BASE" -d postgres -Atc \
	"select datadir from gp_segment_configuration where content=-1;")
grep -q 'replication.*trust' "$COORD_DATADIR/pg_hba.conf" || \
	echo 'host replication all 0.0.0.0/0 trust' >> "$COORD_DATADIR/pg_hba.conf"

log "primary: enabling per-instance WAL archiving to $WAL_ARCHIVE"
while IFS=$'\t' read -r datadir port dbid content; do
	mkdir -p "$WAL_ARCHIVE/seg$content"
	if ! grep -q 'GREENGAGE DR ARCHIVE' "$datadir/postgresql.conf"; then
		cat >> "$datadir/postgresql.conf" <<EOF

# --- GREENGAGE DR ARCHIVE ---
wal_level = replica
archive_mode = on
archive_timeout = 30
archive_command = 'test ! -f $WAL_ARCHIVE/seg%c/%f && cp %p $WAL_ARCHIVE/seg%c/%f'
EOF
	fi
done < <(list_instances)

# archive_mode requires a full restart.
gpstop -ar -q

# A marker table so there is user data to (eventually) read on DR.
psql -p "$PORT_BASE" -d postgres -q -c \
	"drop table if exists dr_marker; create table dr_marker(id int, note text) distributed by (id);
	 insert into dr_marker values (1,'created on production');" || true

log "primary: base-backup each instance to $BASEBACKUP (for DR seeding)"
while IFS=$'\t' read -r datadir port dbid content; do
	rm -rf "$BASEBACKUP/seg$content"
	pg_basebackup -h localhost -p "$port" -X stream --no-sync \
		-D "$BASEBACKUP/seg$content" --target-gp-dbid "$dbid"
	log "primary:   content $content (dbid $dbid, port $port) -> $BASEBACKUP/seg$content"
done < <(list_instances)

# Publish the DR-local topology: the primary's own layout (same dbids/ports/
# datadirs -- the dr container mirrors the demo layout under the same $DEMO path)
# but with hostname + address forced to 'dr', so the seeded DR catalog is
# genuinely DR-local.  Generated from the live config, so it scales with the
# segment count (coordinator + N segments) automatically.
psql -p "$PORT_BASE" -d postgres -Atc \
	"select dbid||E'\t'||content||E'\t'||role||E'\t'||preferred_role||E'\t'||mode||E'\t'||status||E'\t'||port||E'\tdr\tdr\t'||datadir
	   from gp_segment_configuration order by content;" > "$TOPO_FILE"

touch "$READY_MARKER"
log "primary: ready (base backups + topology published, archiving live)"

# Create a user table AFTER the base backup, so it exists only in the WAL the DR
# replays -- proving the redo filter is SELECTIVE: valid (non-topology) changes
# ARE applied on the DR; only the protected topology catalogs are skipped.
sleep 3
psql -p "$PORT_BASE" -d postgres -q -c \
	"create table dr_wal_applied (id int, note text) distributed by (id);
	 insert into dr_wal_applied values (1, 'created on production AFTER the base backup');" || true
log "primary: created table dr_wal_applied (post-base-backup; DR must replay it via WAL)"

# --- generate WAL that changes the protected topology catalog, to exercise the
#     DR redo filter. The DR replica must NOT pick this change up. ---
#
# We change seg0's *hostname*, NOT its port/address.  The coordinator connects to
# a segment by `address` (resolved to hostaddr) + `port` (cdbconn.c); `hostname`
# is only a DNS fallback (cdbutil.c).  So this keeps production fully dispatchable
# -- an earlier version bumped the port, which pointed the coordinator at a port
# the segment wasn't listening on and broke production's own query dispatch.
sleep 5
log "primary: changing gp_segment_configuration (seg0 hostname) to test the DR filter"
PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -q -c \
	"update gp_segment_configuration set hostname = 'prod-seg0-CHANGED' where content = 0;" || true
{
	echo "PRODUCTION_SEG0_HOSTNAME=$(psql -p "$PORT_BASE" -d postgres -Atc "select hostname from gp_segment_configuration where content=0;")"
	echo "PRODUCTION_CHANGE_LSN=$(psql -p "$PORT_BASE" -d postgres -Atc "select pg_current_wal_lsn();")"
} > "$ARCHIVE/primary_expected.env"

# Prove production still dispatches after the change (the point of using hostname).
if psql -p "$PORT_BASE" -d postgres -Atc "select count(*) from dr_marker;" >/dev/null 2>&1; then
	log "primary: OK -- distributed query still dispatches after the change (uses address, not hostname)"
else
	log "primary: WARNING -- distributed query failed after the change"
fi

# Create a restore point AFTER the change on the coordinator, then force its WAL
# to the archive, so the DR replica can recover up to the restore point (replaying
# -- and filtering -- the change) and promote there.
PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -q -c \
	"select pg_create_restore_point('dr_filter_test');" || true
psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
for _ in 1 2 3; do
	psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true
	sleep 2
done
touch "$ARCHIVE/restorepoint_ready"
log "primary: restore point 'dr_filter_test' created and archived"

# --- M3 (stop-and-go): two DISTRIBUTED restore points with data between them, so
#     the DR can pause at dr_rp1 (as-of rp1: dr_m3 has 1 row) and later advance to
#     dr_rp2 (as-of rp2: dr_m3 has 2 rows).  gp_create_restore_point dispatches to
#     every segment under TwophaseCommitLock, so each DR node gets its own
#     restore-point record to pause at. ---
psql -p "$PORT_BASE" -d postgres -q -c \
	"create table dr_m3 (id int) distributed by (id); insert into dr_m3 values (1);" || true
psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dr_rp1');" >/dev/null 2>&1 || true
psql -p "$PORT_BASE" -d postgres -q -c "insert into dr_m3 values (2);" || true
psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dr_rp2');" >/dev/null 2>&1 || true

# --- M3 STRADDLE: a distributed txn T that spans BOTH segments (real 2PC) whose QD
#     DISTRIBUTED_COMMIT lands BEFORE dr_rp_straddle but whose commit-prepared
#     broadcast + forget land AFTER.  The dtm_broadcast_commit_prepared fault
#     (cdbtm.c) suspends T after its DISTRIBUTED_COMMIT and before it takes
#     TwophaseCommitLock SHARED, so the restore point's EXCLUSIVE lock slips in
#     between.  At the DR cut, T is committed on the coordinator (shmCommittedGxidArray
#     != 0) but prepared-only on the segments -- exactly what the DR-standby snapshot
#     builder must EXCLUDE.  dr_straddle: 10 baseline rows + T's 20 rows (=30 total). ---
CDBID=$(psql -p "$PORT_BASE" -d postgres -Atc "select dbid from gp_segment_configuration where content=-1;")
psql -p "$PORT_BASE" -d postgres -q -c \
	"create table dr_straddle (id int) distributed by (id); insert into dr_straddle select generate_series(1,10);" || true
psql -p "$PORT_BASE" -d postgres -Atc \
	"select gp_inject_fault_infinite('dtm_broadcast_commit_prepared', 'suspend', $CDBID);" >/dev/null 2>&1 || true
( psql -p "$PORT_BASE" -d postgres -c "insert into dr_straddle select generate_series(101,120);" >/dev/null 2>&1 ) &
TPID=$!
psql -p "$PORT_BASE" -d postgres -Atc \
	"select gp_wait_until_triggered_fault('dtm_broadcast_commit_prepared', 1, $CDBID);" >/dev/null 2>&1 || true
psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dr_rp_straddle');" >/dev/null 2>&1 || true
psql -p "$PORT_BASE" -d postgres -Atc \
	"select gp_inject_fault('dtm_broadcast_commit_prepared', 'reset', $CDBID);" >/dev/null 2>&1 || true
wait "$TPID" 2>/dev/null || true
psql -p "$PORT_BASE" -d postgres -q -c "select gp_create_restore_point('dr_rp_straddle_done');" >/dev/null 2>&1 || true

psql -p "$PORT_BASE" -d postgres -q -c "checkpoint;" >/dev/null 2>&1 || true
for _ in 1 2 3; do psql -p "$PORT_BASE" -d postgres -q -c "select pg_switch_wal();" >/dev/null 2>&1 || true; sleep 2; done
touch "$ARCHIVE/m3_ready"
log "primary: M3 restore points (dr_rp1/dr_rp2) + straddle (dr_rp_straddle/_done, coord dbid=${CDBID:-?}) created + archived"

log "primary: done; idling so the cluster keeps archiving WAL"
exec sleep infinity
