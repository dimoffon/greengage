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

# Frozen-tuple seed (M1.5) + recovery-start preservation (M4), BEFORE arming
# recovery.  The single-user seed consumes backup_label and advances pg_control
# past the backup LSN, which would fork production's timeline on resume.  We save
# the recovery-start state (backup_label + pg_control) before the seed and restore
# it after: recovery then resumes from the backup checkpoint and refetches
# production's WAL from the archive (restore_command), while the seed's frozen
# gp_segment_configuration pages -- protected, hence filtered -- persist.
if [ "$DR_SEED" = 1 ]; then
	log "dr: M4 seed: preserving recovery-start state, then frozen-seeding DR-local topology"
	have_label=
	[ -f "$COORD/backup_label" ] && { cp -p "$COORD/backup_label" "$COORD/backup_label.drsave"; have_label=1; }
	cp -p "$COORD/global/pg_control" "$COORD/pg_control.drsave"
	if "$SRC/gpMgmt/bin/gpseed_dr_topology" "$COORD" "$TOPO_FILE"; then
		[ -n "$have_label" ] && mv "$COORD/backup_label.drsave" "$COORD/backup_label"
		mv "$COORD/pg_control.drsave" "$COORD/global/pg_control"
		log "dr: M4 seed: done; recovery-start state restored (will resume production WAL from the archive)"
	else
		log "dr: WARNING gpseed_dr_topology failed; leaving node unseeded"
		rm -f "$COORD/backup_label.drsave" "$COORD/pg_control.drsave"
	fi
fi

log "dr: waiting for production to create + archive the restore point ..."
for _ in $(seq 1 300); do [ -f "$ARCHIVE/restorepoint_ready" ] && break; sleep 2; done

# Arm DR mode + continuous archive recovery and bring the coordinator up as a
# LIVE hot-standby that serves read-only queries while permanently in recovery.
# hot_standby is enabled automatically by DR mode (gp_dr_replica +
# dr_replica.signal) in the postmaster, so we deliberately do NOT set it here --
# that exercises the M2 auto-enable path.  The redo filter stays active, so
# production's gp_segment_configuration change is skipped during replay.
: > "$COORD/postgresql.auto.conf"             # drop production sync-rep settings
cat >> "$COORD/postgresql.conf" <<EOF

# --- GREENGAGE DR REPLICA ---
gp_dr_replica = on
restore_command = 'cp $WAL_ARCHIVE/seg%c/%f %p'
recovery_target_timeline = 'current'
EOF
touch "$COORD/dr_replica.signal"              # arms IsDRReplicaMode() + redo filter + hot_standby auto-enable
touch "$COORD/standby.signal"                 # continuous archive recovery (stays in recovery)

source "$ARCHIVE/primary_expected.env"
q()     { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null; }
# Returns 0 even when the statement errors (that is the expected case here), so
# the command substitution does not trip `set -e`; the error text is captured.
qfail() { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -v ON_ERROR_STOP=0 -qtAc "$1" 2>&1 || true; }

log "dr: starting DR coordinator in continuous hot-standby recovery (hot_standby auto-enabled by DR mode) ..."
# -W: do not wait. A continuous hot-standby never reports "ready" to pg_ctl's
# liveness wait the way a normal server does, so we launch and poll ourselves.
pg_ctl -D "$COORD" -l "$COORD/startup.log" -W -o "-c gp_role=dispatch" start

log "dr: waiting for the in-recovery coordinator to accept read connections ..."
ok=
for _ in $(seq 1 80); do q "select 1" >/dev/null && { ok=1; break; }; sleep 3; done
if [ -z "$ok" ]; then
	log "dr: coordinator did not accept connections; startup + server logs:"
	tail -30 "$COORD/startup.log" >&2 2>/dev/null || true
	cat "$COORD"/log/*.csv 2>/dev/null | tail -30 >&2 || true
	exec sleep infinity
fi
log "dr: coordinator is LIVE in recovery and accepting read connections"

log "dr: waiting to replay past production's change (LSN ${PRODUCTION_CHANGE_LSN}) ..."
for _ in $(seq 1 60); do
	[ "$(q "select pg_last_wal_replay_lsn() >= '${PRODUCTION_CHANGE_LSN}'::pg_lsn")" = t ] && break
	sleep 2
done

pass=0; fail=0
ok_() { log "dr-test: PASS  $1"; pass=$((pass+1)); }
no_() { log "dr-test: FAIL  $1"; fail=$((fail+1)); }
refused() {  # $1=label  $2=sql -- assert the statement is refused on the DR replica
	local out; out=$(qfail "$2")
	if echo "$out" | grep -qiE "read-only|disaster-recovery replica|cannot execute|during recovery"; then
		ok_ "$1 refused -> $(echo "$out" | grep -iE 'ERROR' | head -1)"
	else
		no_ "$1 NOT refused -> ${out:-<no output>}"
	fi
}

echo "================ M1 + M2 assertions on the LIVE DR coordinator (in recovery) ================"
# M1 (redo filter): production changed seg0 hostname to $PRODUCTION_SEG0_HOSTNAME; DR must not show it.
drhost=$(q "select hostname from gp_segment_configuration where content=0;")
if [ -n "$drhost" ] && [ "$drhost" != "$PRODUCTION_SEG0_HOSTNAME" ]; then
	ok_ "M1 redo filter: DR seg0 hostname still '$drhost' (production changed it to '$PRODUCTION_SEG0_HOSTNAME')"
else
	no_ "M1 redo filter: DR seg0 hostname='$drhost', production='$PRODUCTION_SEG0_HOSTNAME'"
fi
# M4 (seed): when seeded, the DR carries genuinely DR-LOCAL topology ('dr'),
# proving topology independence -- not merely "ignored production's change".
if [ "$DR_SEED" = 1 ]; then
	if [ "$drhost" = "dr" ]; then
		ok_ "M4 seed: DR seg0 has DR-LOCAL hostname 'dr' (independent of production)"
	else
		no_ "M4 seed: DR seg0 hostname='$drhost' (expected DR-local 'dr')"
	fi
fi
# M1 (selective): a user table created on production AFTER the base backup must
# appear on the DR via WAL replay -- the filter only skips protected catalogs,
# it applies everything else.
if [ "$(q "select count(*) from pg_class where relname = 'dr_wal_applied';")" = 1 ]; then
	ok_ "M1 selective: user table 'dr_wal_applied' (created post-backup on production) replayed onto the DR catalog"
else
	no_ "M1 selective: user table 'dr_wal_applied' NOT present on the DR (valid change not applied)"
fi
# M2 (reads work): coordinator-only catalog read while in recovery.
if [ "$(q "select count(*) > 0 from gp_segment_configuration;")" = t ]; then
	ok_ "M2 coordinator-only read works (count(gp_segment_configuration) > 0)"
else
	no_ "M2 coordinator-only read FAILED"
fi
# M2 (writes refused): FOR UPDATE + DML go through ExecCheckXactReadOnly (DR message);
# DDL goes through check_xact_readonly (stock read-only message). All must fail.
refused "M2 SELECT ... FOR UPDATE" "select port from gp_segment_configuration where content = 0 for update;"
refused "M2 DML (INSERT)"          "insert into dr_marker values (99, 'must be refused');"
refused "M2 DDL (CREATE TABLE)"    "create table dr_should_not_exist (x int);"
echo "============================================================================================"
if [ "$fail" -eq 0 ]; then
	log "================ DR M1+M2 TEST: PASS ($pass/$((pass+fail)) checks) ================"
else
	log "================ DR M1+M2 TEST: FAIL ($fail of $((pass+fail)) failed) ================"
fi
log "dr: DR coordinator left RUNNING in recovery (utility-mode reads). Connect with:"
log "dr:   sudo docker-compose -f src/test/dr/docker-compose.yml exec dr \\"
log "dr:        env PGOPTIONS='-c gp_role=utility' psql -p ${PORT_BASE} postgres"

# --- M2' I-0: bring the DR SEGMENT (content 0) up in recovery too, so the
#     coordinator can (after SR-1 + SR-2) dispatch a read-only distributed query
#     to it.  For now this just smoke-tests that the segment serves reads while in
#     recovery -- the dispatch path itself is M2' SR-1/SR-2 (not yet implemented).
SEG0=${DR_DATADIR[0]}
: > "$SEG0/postgresql.auto.conf"
cat >> "$SEG0/postgresql.conf" <<EOF

# --- GREENGAGE DR REPLICA (segment) ---
gp_dr_replica = on
restore_command = 'cp $WAL_ARCHIVE/seg%c/%f %p'
recovery_target_timeline = 'current'
EOF
touch "$SEG0/dr_replica.signal"               # filter + read-only enforcement + hot_standby auto-enable
touch "$SEG0/standby.signal"                  # continuous archive recovery (%c resolves to 0 on the segment)

log "dr: M2' I-0: starting DR segment (content 0) in continuous hot-standby recovery (gp_role=execute) ..."
pg_ctl -D "$SEG0" -l "$SEG0/startup.log" -W -o "-c gp_role=execute" start
sok=
for _ in $(seq 1 80); do PGOPTIONS='-c gp_role=utility' psql -p 7002 -d postgres -Atc 'select 1' >/dev/null 2>&1 && { sok=1; break; }; sleep 3; done
if [ -n "$sok" ]; then
	scount=$(PGOPTIONS='-c gp_role=utility' psql -p 7002 -d postgres -Atc 'select count(*) from dr_marker' 2>/dev/null)
	log "dr: M2' I-0: PASS -- DR segment is LIVE in recovery; segment-local 'select count(*) from dr_marker' = ${scount:-<none>}"
else
	log "dr: M2' I-0: FAIL -- DR segment did not accept connections; logs:"
	tail -20 "$SEG0/startup.log" >&2 2>/dev/null || true
	cat "$SEG0"/log/*.csv 2>/dev/null | tail -20 >&2 || true
fi

# --- M2' SR-1: dispatched read-only DISTRIBUTED queries.  The coordinator (in
#     recovery) enters DTX_CONTEXT_QD_STANDBY_READER -- no gxid -- and dispatches
#     to the segment (also in recovery), which serves as a non-writing reader.
#     Default psql on the coordinator is dispatch mode (utility is the override). ---
if [ -n "$sok" ]; then
	# Returns 0 even when the statement errors (INSERT is expected to be refused),
	# so the command substitution does not trip `set -e`; the output is captured.
	dsp() { psql -p "$PORT_BASE" -d postgres -v ON_ERROR_STOP=0 -qtAc "$1" 2>&1 || true; }
	p2=0; f2=0
	okp() { log "dr-test: PASS  $1"; p2=$((p2+1)); }
	nop() { log "dr-test: FAIL  $1"; f2=$((f2+1)); }
	for _ in $(seq 1 20); do [ "$(dsp 'select 1')" = 1 ] && break; sleep 2; done
	echo "========== M2' distributed-read assertions (dispatched; coordinator + segment in recovery) =========="
	r=$(dsp 'select count(*) from dr_marker;')
	[ "$r" = 1 ] && okp "M2' dispatch: 'select count(*) from dr_marker' = 1 (aggregated from the segment)" \
				 || nop "M2' dispatch count = '$r'"
	r=$(dsp 'select gp_segment_id from dr_marker;')
	[ "$r" = 0 ] && okp "M2' dispatch: row served by gp_segment_id=0 (real dispatch to the DR segment)" \
				 || nop "M2' dispatch gp_segment_id = '$r'"
	r=$(dsp 'select (select count(*) from dr_marker);')
	[ "$r" = 1 ] && okp "M2' dispatch: InitPlan-bearing read works (no gxid, no DTX backstop)" \
				 || nop "M2' dispatch InitPlan read = '$r'"
	r=$(dsp 'select count(*) from dr_marker a join dr_marker b using (id);')
	[ "$r" = 1 ] && okp "M2' dispatch: multi-slice JOIN works (exercises the QE_READER reader-consume path)" \
				 || nop "M2' dispatch JOIN = '$r'"
	r=$(dsp "insert into dr_marker values (2, 'must be refused');")
	echo "$r" | grep -qiE "read-only|disaster-recovery replica" \
		&& okp "M2' dispatch: INSERT still refused ($(echo "$r" | grep -iE 'ERROR' | head -1))" \
		|| nop "M2' dispatch INSERT not refused -> $r"
	echo "===================================================================================================="
	if [ "$f2" -eq 0 ]; then
		log "================ M2' DISTRIBUTED-READ TEST: PASS ($p2/$((p2+f2))) ================"
	else
		log "================ M2' DISTRIBUTED-READ TEST: FAIL ($f2 of $((p2+f2)) failed) ================"
	fi
fi

exec sleep infinity
