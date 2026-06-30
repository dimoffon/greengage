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
log "primary: creating demo cluster (coordinator + 1 segment, no mirrors) ..."
LANG=en_US.UTF-8 make create-demo-cluster \
	PORT_BASE="$PORT_BASE" NUM_PRIMARY_MIRROR_PAIRS=1 WITH_MIRRORS=false 2>&1 | tail -25
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

# Publish the DR-local topology (same dbids/ports/layout, but hostname 'dr').
# datadirs mirror the primary's demo layout inside the dr container's $DEMO.
{
	echo -e "1\t-1\tp\tp\tn\tu\t7000\tdr\tdr\t$DEMO/datadirs/qddir/demoDataDir-1"
	echo -e "2\t0\tp\tp\ts\tu\t7002\tdr\tdr\t$DEMO/datadirs/dbfast1/demoDataDir0"
} > "$TOPO_FILE"

touch "$READY_MARKER"
log "primary: ready (base backups + topology published, archiving live)"

# --- generate WAL that changes the protected topology catalog, to exercise the
#     DR redo filter: production bumps the segment's port; the DR replica must
#     NOT pick this up (its frozen-seeded DR-local rows stay intact). ---
sleep 5
log "primary: changing gp_segment_configuration (segment port -> +1000) to test the DR filter"
PGOPTIONS='-c gp_role=utility -c allow_system_table_mods=on' \
	psql -p "$PORT_BASE" -d postgres -q -c \
	"update gp_segment_configuration set port = port + 1000 where content = 0;" || true
{
	echo "PRODUCTION_SEG0_PORT=$(psql -p "$PORT_BASE" -d postgres -Atc "select port from gp_segment_configuration where content=0;")"
	echo "PRODUCTION_CHANGE_LSN=$(psql -p "$PORT_BASE" -d postgres -Atc "select pg_current_wal_lsn();")"
} > "$ARCHIVE/primary_expected.env"

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

log "primary: done; idling so the cluster keeps archiving WAL"
exec sleep infinity
