#!/bin/bash
# dr-entrypoint.sh -- seed the DR cluster from the primary's base backups, arm
# DR mode (gp_dr_replica + dr_replica.signal) + continuous restore, start the
# instances in recovery, then run the filter test.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin   # re-execs as gpadmin, sources greengage_path.sh

DR_SEED=${DR_SEED:-1}   # 1 = run the frozen-tuple topology seed (M1.5)

log "dr: waiting for primary to publish base backups ..."
for _ in $(seq 1 600); do [ -f "$READY_MARKER" ] && break; sleep 2; done
[ -f "$READY_MARKER" ] || die "timed out waiting for $READY_MARKER"
log "dr: primary ready; seeding DR cluster"

# Restore each instance's base backup into the DR demo layout.
declare -A DR_DATADIR=( [-1]="$DEMO/datadirs/qddir/demoDataDir-1"
                        [0]="$DEMO/datadirs/dbfast1/demoDataDir0" )
for content in -1 0; do
	dest=${DR_DATADIR[$content]}
	rm -rf "$dest"; mkdir -p "$(dirname "$dest")"
	cp -a "$BASEBACKUP/seg$content" "$dest"
	chmod 700 "$dest"
	log "dr:   restored content $content -> $dest"
done

COORD=${DR_DATADIR[-1]}

# Frozen-tuple seed of DR-local topology (M1.5), BEFORE arming standby mode.
if [ "$DR_SEED" = 1 ]; then
	log "dr: frozen-seeding DR-local gp_segment_configuration (gpseed_dr_topology)"
	"$SRC/gpMgmt/bin/gpseed_dr_topology" "$COORD" "$TOPO_FILE" || \
		log "dr: WARNING gpseed_dr_topology failed (continuing; filter test still valid)"
fi

# Arm DR mode + archive-based continuous restore on every instance.
for content in -1 0; do
	dest=${DR_DATADIR[$content]}
	: > "$dest/postgresql.auto.conf"          # drop production sync-rep settings
	cat >> "$dest/postgresql.conf" <<EOF

# --- GREENGAGE DR REPLICA ---
gp_dr_replica = on
hot_standby = on
restore_command = 'cp $WAL_ARCHIVE/seg%c/%f %p'
EOF
	touch "$dest/dr_replica.signal"           # arms IsDRReplicaMode() + the redo filter
	touch "$dest/standby.signal"              # archive-based continuous recovery
	pg_ctl -D "$dest" -l "$dest/startup.log" -w -t 120 start || \
		{ log "dr: content $content failed to start; tail of log:"; tail -30 "$dest/startup.log" >&2; die "start failed"; }
	log "dr:   content $content started in recovery (port from config)"
done

log "dr: instances started in recovery; replaying production WAL from $WAL_ARCHIVE"
sleep 10
exec /dr/scripts/run-dr-test.sh
