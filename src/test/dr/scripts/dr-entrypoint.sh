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

# Frozen-tuple seed (M1.5), BEFORE arming recovery. Off by default: the
# standalone seed advances local WAL and currently diverges from the production
# archive (see the DR_SEED note in docker-compose.yml).
if [ "$DR_SEED" = 1 ]; then
	log "dr: frozen-seeding DR-local gp_segment_configuration (gpseed_dr_topology)"
	"$SRC/gpMgmt/bin/gpseed_dr_topology" "$COORD" "$TOPO_FILE" || \
		log "dr: WARNING gpseed_dr_topology failed (continuing)"
fi

log "dr: waiting for production to create + archive the restore point ..."
for _ in $(seq 1 300); do [ -f "$ARCHIVE/restorepoint_ready" ] && break; sleep 2; done

# Arm DR mode on the coordinator and recover up to the restore point, then
# promote. A GP coordinator kept in *continuous* recovery does not accept
# connections (that is the M2' standby-distributed-read milestone), so we
# recover-to-target + promote to verify -- exactly as test_gpdb_pitr.sh does.
# The redo filter (gp_dr_replica + dr_replica.signal) is active throughout, so
# production's gp_segment_configuration change is skipped during replay.
: > "$COORD/postgresql.auto.conf"             # drop production sync-rep settings
cat >> "$COORD/postgresql.conf" <<EOF

# --- GREENGAGE DR REPLICA ---
gp_dr_replica = on
restore_command = 'cp $WAL_ARCHIVE/seg%c/%f %p'
recovery_target_name = 'dr_filter_test'
recovery_target_action = 'promote'
recovery_target_timeline = 'current'
EOF
touch "$COORD/dr_replica.signal"              # arms IsDRReplicaMode() + the redo filter
touch "$COORD/recovery.signal"                # archive recovery to the restore point

# Recover to the restore point and read gp_segment_configuration in SINGLE-USER
# mode -- no interconnect / FTS / dispatch, so the DR coordinator's
# production-derived topology cannot make it try to act as a live MPP coordinator
# (which fails to bind the interconnect). StartupXLOG honours recovery.signal +
# restore_command, so this performs the same archive recovery, replaying -- and,
# with the filter armed, *skipping* -- production's gp_segment_configuration
# change before reaching the restore point.
log "dr: pre-flight: $(ls -l "$COORD/dr_replica.signal" 2>/dev/null || echo 'dr_replica.signal MISSING'); $(grep -i '^gp_dr_replica' "$COORD/postgresql.conf" 2>/dev/null || echo 'gp_dr_replica NOT in postgresql.conf')"
log "dr: recover-to-restore-point + read in single-user mode (DR redo filter active) ..."
postgres --single -D "$COORD" -c gp_role=utility template1 > "$ARCHIVE/dr_recover.log" 2>&1 <<'SQL'
SELECT 'DR_SEG0_PORT_IS_' || port FROM gp_segment_configuration WHERE content = 0;
SELECT 'DR_COORD_HOST_IS_' || hostname FROM gp_segment_configuration WHERE content = -1;
SQL
OUT=$(cat "$ARCHIVE/dr_recover.log")
echo "$OUT" | grep -iE "marker file \"|not a mapped relation|redo done|recovery stopping|DR_SEG0_PORT_IS_|DR_COORD_HOST_IS_|ERROR|FATAL|PANIC" | tail -25

source "$ARCHIVE/primary_expected.env"
dr_seg0_port=$(echo "$OUT" | grep -oE 'DR_SEG0_PORT_IS_[0-9]+' | head -1 | sed 's/DR_SEG0_PORT_IS_//')
reached_rp=$(echo "$OUT" | grep -ciE "restore point .?dr_filter_test|recovery stopping at restore point")

echo "======================================================================"
log "dr-test: production changed seg0 port to        : ${PRODUCTION_SEG0_PORT}"
log "dr-test: DR (after filtered replay) sees seg0    : ${dr_seg0_port:-<none>}"
log "dr-test: DR reached the post-change restore point: $([ "$reached_rp" -ge 1 ] && echo yes || echo no)"
if [ -n "$dr_seg0_port" ] && [ "$dr_seg0_port" != "$PRODUCTION_SEG0_PORT" ] && [ "$reached_rp" -ge 1 ]; then
	log "================ DR FILTER TEST: PASS ================"
	log "dr-test: DR replayed past production's gp_segment_configuration change but did NOT apply it -- redo filter works"
else
	log "================ DR FILTER TEST: FAIL ================"
fi
echo "======================================================================"
log "dr: idling for inspection (docker compose exec dr bash)"
exec sleep infinity
