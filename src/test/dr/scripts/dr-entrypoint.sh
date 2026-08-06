#!/bin/bash
# dr-entrypoint.sh -- build the DR cluster with `gg_recovery create-replica`
# (restore base backups + frozen-seed DR-local topology + arm DR mode + start every
# node in continuous archive recovery), then run the milestone regression suite:
# M1 (redo filter) / M2 (read-only) / M2' (distributed read) / M3 (stop-and-go) /
# M5 (observability), the gg_recovery switch/pause/stats utility test, and finally
# the SQL recovery-control test (gp_dr_switch / gp_dr_promote -> online read-write).
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin   # re-execs as gpadmin, sources greengage_path.sh

log "dr: waiting for primary to publish base backups ..."
for _ in $(seq 1 600); do [ -f "$READY_MARKER" ] && break; sleep 2; done
[ -f "$READY_MARKER" ] || die "timed out waiting for $READY_MARKER"
log "dr: primary ready; building DR cluster"

# Derive the DR layout (content -> datadir, content -> port) from the published
# topology, so the fixture scales with the segment count (coordinator + N segments).
declare -A DR_DATADIR DR_PORT
while IFS=$'\t' read -r dbid content role prefrole mode status port hostname address datadir; do
	[ -n "${content:-}" ] || continue
	DR_DATADIR[$content]=$datadir
	DR_PORT[$content]=$port
done < "$TOPO_FILE"
ALL_CONTENTS=$(printf '%s\n' "${!DR_DATADIR[@]}" | sort -n)             # -1 0 1 ...
SEG_CONTENTS=$(printf '%s\n' "${!DR_DATADIR[@]}" | sort -n | grep -v '^-')  # 0 1 ...
COORD=${DR_DATADIR[-1]}

# gpstart/gpstop resolve the coordinator datadir from $COORDINATOR_DATA_DIRECTORY
# (or the legacy $MASTER_DATA_DIRECTORY), else they abort with "Environment Variable
# COORDINATOR_DATA_DIRECTORY not set!".  The primary gets this from gpdemo-env.sh, but
# the DR cluster is built by gg_recovery (not gpdemo), so no such file exists here.
# Export it (so this script's own gpstart/gpstop work) AND persist it as a sourceable
# env file + ~/.bashrc line (so a fresh `docker exec dr bash` shell has it too) -- e.g.
# to gpstop the promoted cluster before rebuilding the replica with create-replica --force.
export COORDINATOR_DATA_DIRECTORY="$COORD"
export MASTER_DATA_DIRECTORY="$COORD"
export PGPORT="$PORT_BASE"
{
	echo "export COORDINATOR_DATA_DIRECTORY='$COORD'"
	echo "export MASTER_DATA_DIRECTORY='$COORD'"
	echo "export PGPORT='$PORT_BASE'"
} > "$DEMO/gpdemo-env.sh"
grep -q 'source .*gpdemo-env.sh' ~/.bashrc 2>/dev/null || \
	echo "source '$DEMO/gpdemo-env.sh' 2>/dev/null || true" >> ~/.bashrc
log "dr: exported COORDINATOR_DATA_DIRECTORY=$COORD (gpstart/gpstop usable; also in $DEMO/gpdemo-env.sh)"

# The full node list (coordinator + every segment) as "datadir:port", used by the
# M3 pause/advance helpers and the promote test so they act on the whole cluster
# regardless of segment count.  Coordinator first (ALL_CONTENTS sorts -1 ahead).
NODES=()
for content in $ALL_CONTENTS; do NODES+=("${DR_DATADIR[$content]}:${DR_PORT[$content]}"); done

log "dr: waiting for production to create + archive the restore point ..."
for _ in $(seq 1 300); do [ -f "$ARCHIVE/restorepoint_ready" ] && break; sleep 2; done

# --- M4: build the whole DR cluster in one shot via the gg_recovery utility.
#     create-replica restores each instance's base backup, frozen-seeds the
#     DR-local topology into the coordinator (hostname 'dr'), gates on the WAL
#     archive being complete enough to reach consistency, arms DR mode
#     (hot_standby + restore_command + standby.signal) on
#     every node with the M3 pause target dr_rp1, and starts each in archive
#     recovery.  This exercises the SAME code path an operator would run, instead
#     of open-coding the restore/seed/arm/start inline. ---
GG="python3 $SRC/gpMgmt/bin/gg_recovery"
export PGPORT="$PORT_BASE" PGDATABASE=postgres

# --- auxiliary-tooling gate: gg_walfilter black-box tests (against the
#     installed binary; no cluster needed).  The in-backend redo filter is
#     what protects the DR topology, but the shipped filtering tool must
#     still pass its own suite before we bless the build. ---
log "dr: running the gg_walfilter tests (black-box, installed binary) ..."
if GG_WALFILTER="$(command -v gg_walfilter)" \
	python3 "$SRC/src/test/dr/test_gg_walfilter.py" >/tmp/walfilter-test.log 2>&1; then
	tail -2 /tmp/walfilter-test.log | sed 's/^/    walfilter-test: /'
	log "dr: gg_walfilter tests PASSED"
else
	tail -20 /tmp/walfilter-test.log | sed 's/^/    walfilter-test: /'
	die "gg_walfilter tests FAILED; refusing to build the DR cluster"
fi

log "dr: building the DR replica via 'gg_recovery create-replica' (M4) ..."
set +e
$GG create-replica \
	--topology "$TOPO_FILE" \
	--basebackup-dir "$BASEBACKUP" \
	--wal-archive "$WAL_ARCHIVE" \
	--pause-at dr_rp1 \
	--force 2>&1 | sed 's/^/    gg_recovery: /'
cr_rc=${PIPESTATUS[0]}
set -e
[ "$cr_rc" = 0 ] || log "dr: WARNING create-replica exit=$cr_rc (continuing; assertions below will show the real state)"

q()     { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null; }
# Returns 0 even when the statement errors (that is the expected case here), so
# the command substitution does not trip `set -e`; the error text is captured.
qfail() { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -v ON_ERROR_STOP=0 -qtAc "$1" 2>&1 || true; }

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

source "$ARCHIVE/primary_expected.env"
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
# M4 (seed): create-replica frozen-seeds the DR-local topology, so the DR carries a
# genuinely DR-LOCAL hostname ('dr') -- proving topology independence, not merely
# "ignored production's change".
if [ "$drhost" = "dr" ]; then
	ok_ "M4 seed: DR seg0 has DR-LOCAL hostname 'dr' (independent of production)"
else
	no_ "M4 seed: DR seg0 hostname='$drhost' (expected DR-local 'dr')"
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

# --- M2' I-0: every DR SEGMENT is already up in recovery (create-replica started
#     the whole cluster).  Verify each accepts utility reads so the coordinator can
#     dispatch a read-only distributed query to them. ---
sok=1
for c in $SEG_CONTENTS; do
	sd=${DR_DATADIR[$c]}; sp=${DR_PORT[$c]}; up=
	for _ in $(seq 1 80); do PGOPTIONS='-c gp_role=utility' psql -p "$sp" -d postgres -Atc 'select 1' >/dev/null 2>&1 && { up=1; break; }; sleep 3; done
	if [ -n "$up" ]; then
		scount=$(PGOPTIONS='-c gp_role=utility' psql -p "$sp" -d postgres -Atc 'select count(*) from dr_marker' 2>/dev/null)
		log "dr: M2' I-0: PASS -- DR segment content $c LIVE in recovery; segment-local count(dr_marker)=${scount:-<none>}"
	else
		sok=
		log "dr: M2' I-0: FAIL -- DR segment content $c did not accept connections; logs:"
		tail -20 "$sd/startup.log" >&2 2>/dev/null || true
		cat "$sd"/log/*.csv 2>/dev/null | tail -20 >&2 || true
	fi
done

# --- M2' SR-1: dispatched read-only DISTRIBUTED queries.  The coordinator (in
#     recovery) enters DTX_CONTEXT_QD_STANDBY_READER -- no gxid -- and dispatches
#     to the segment (also in recovery), which serves as a non-writing reader.
#     Default psql on the coordinator is dispatch mode (utility is the override). ---
if [ -n "$sok" ]; then
	# Returns 0 even when the statement errors (INSERT is expected to be refused),
	# so the command substitution does not trip `set -e`; the output is captured.
	dsp() { psql -p "$PORT_BASE" -d postgres -v ON_ERROR_STOP=0 -qtAc "$1" 2>&1 || true; }
	# Multi-statement variant.  `psql -c` prints only the LAST statement's result,
	# so a script whose interesting value is not last (e.g. one wrapped in
	# begin/commit) has to go through stdin instead.
	dsps() { psql -p "$PORT_BASE" -d postgres -v ON_ERROR_STOP=0 -qtA 2>&1 <<<"$1" || true; }
	p2=0; f2=0
	okp() { log "dr-test: PASS  $1"; p2=$((p2+1)); }
	nop() { log "dr-test: FAIL  $1"; f2=$((f2+1)); }
	for _ in $(seq 1 20); do [ "$(dsp 'select 1')" = 1 ] && break; sleep 2; done
	echo "========== M2' distributed-read assertions (dispatched; coordinator + segment in recovery) =========="
	r=$(dsp 'select count(*) from dr_marker;')
	[ "$r" = 1 ] && okp "M2' dispatch: 'select count(*) from dr_marker' = 1 (aggregated from the segment)" \
				 || nop "M2' dispatch count = '$r'"
	r=$(dsp 'select gp_segment_id from dr_marker;')
	echo "$r" | grep -qE '^[0-9]+$' && okp "M2' dispatch: row served by gp_segment_id=$r (real dispatch to a DR segment)" \
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

	# --- M3 (stop-and-go): the DR is armed to PAUSE replay at restore point
	#     dr_rp1.  Show reads are STABLE as-of dr_rp1 (dr_m3 has 1 row), then after
	#     advancing to dr_rp2 they reflect as-of dr_rp2 (dr_m3 has 2 rows).  While
	#     paused there is zero replay, hence zero recovery-conflict cancellation. ---
	advance() {  # $1=from $2=to : rearm the pause GUC (SIGHUP) + resume replay on ALL nodes
		local nd dd pp
		for nd in "${NODES[@]}"; do
			dd=${nd%:*}; pp=${nd##*:}
			sed -i "s/gp_pause_on_restore_point_replay = '$1'/gp_pause_on_restore_point_replay = '$2'/" "$dd/postgresql.conf"
			PGOPTIONS='-c gp_role=utility' psql -p "$pp" -d postgres -Atc 'select pg_reload_conf();'       >/dev/null 2>&1 || true
			PGOPTIONS='-c gp_role=utility' psql -p "$pp" -d postgres -Atc 'select pg_wal_replay_resume();' >/dev/null 2>&1 || true
		done
	}
	all_paused() {  # true iff EVERY node (coordinator + all segments) has replay paused
		local nd pp
		for nd in "${NODES[@]}"; do
			pp=${nd##*:}
			[ "$(PGOPTIONS='-c gp_role=utility' psql -p "$pp" -d postgres -Atc 'select pg_is_wal_replay_paused();' 2>/dev/null)" = t ] || return 1
		done
		return 0
	}
	p3=0; f3=0
	ok3() { log "dr-test: PASS  $1"; p3=$((p3+1)); }
	no3() { log "dr-test: FAIL  $1"; f3=$((f3+1)); }
	echo "================ M3 stop-and-go: reads gated by restore point (dr_rp1 -> dr_rp2) ================"
	for _ in $(seq 1 90); do all_paused && break; sleep 2; done
	if all_paused; then
		ok3 "M3: all ${#NODES[@]} DR nodes PAUSED at restore point dr_rp1 (replay stopped)"
		r=$(dsp 'select count(*) from dr_m3;')
		[ "$r" = 1 ] && ok3 "M3: as-of dr_rp1, dr_m3 count = 1 (the 2nd insert is beyond rp1)" \
					 || no3 "M3: as-of dr_rp1 dr_m3 = '$r' (expected 1)"
		r=$(dsp 'select count(*) from dr_m3;')
		[ "$r" = 1 ] && ok3 "M3: repeated read STABLE at dr_rp1 (=1; paused => no replay, no cancellation)" \
					 || no3 "M3: repeated read changed = '$r'"
		log "dr-test: M3 advancing dr_rp1 -> dr_rp2 (rearm GUC + SIGHUP + resume on both nodes) ..."
		advance dr_rp1 dr_rp2
		for _ in $(seq 1 90); do [ "$(dsp 'select count(*) from dr_m3;')" = 2 ] && break; sleep 2; done
		r=$(dsp 'select count(*) from dr_m3;')
		[ "$r" = 2 ] && ok3 "M3: after advancing to dr_rp2, dr_m3 count = 2 (new data now visible as-of rp2)" \
					 || no3 "M3: as-of dr_rp2 dr_m3 = '$r' (expected 2)"

		# --- M3 STRADDLE: advance to dr_rp_straddle, where a distributed 2PC txn T
		#     spanning both segments straddles the cut -- its DISTRIBUTED_COMMIT is
		#     replayed on the coordinator (shmCommittedGxidArray != 0) but it is
		#     prepared-only on the segments.  CreateDRStandbyDistributedSnapshot must
		#     EXCLUDE T (in-doubt) and NOT error, so dr_straddle shows only the 10
		#     baseline rows; after advancing past T's forget, T's 20 rows appear (30). ---
		log "dr-test: M3 straddle: advancing dr_rp2 -> dr_rp_straddle (the straddle cut) ..."
		advance dr_rp2 dr_rp_straddle
		for _ in $(seq 1 90); do all_paused && break; sleep 2; done
		r=$(dsp 'select count(*) from dr_straddle;')
		if [ "$r" = 10 ]; then
			ok3 "M3 straddle: at the cut dr_straddle count = 10 (in-doubt 2PC txn excluded; no shmNumCommittedGxacts error)"
		else
			no3 "M3 straddle: at the cut dr_straddle = '$r' (expected 10; $(echo "$r" | grep -iE 'ERROR' | head -1))"
		fi
		log "dr-test: M3 straddle: advancing dr_rp_straddle -> dr_rp_straddle_done (past T's forget) ..."
		advance dr_rp_straddle dr_rp_straddle_done
		for _ in $(seq 1 90); do [ "$(dsp 'select count(*) from dr_straddle;')" = 30 ] && break; sleep 2; done
		r=$(dsp 'select count(*) from dr_straddle;')
		[ "$r" = 30 ] && ok3 "M3 straddle: after advancing past the forget, dr_straddle count = 30 (T now visible)" \
					 || no3 "M3 straddle: past-forget dr_straddle = '$r' (expected 30)"
	else
		no3 "M3: not all ${#NODES[@]} DR nodes paused at dr_rp1"
	fi
	echo "==============================================================================================="
	if [ "$f3" -eq 0 ]; then
		log "================ M3 STOP-AND-GO TEST: PASS ($p3/$((p3+f3))) ================"
	else
		log "================ M3 STOP-AND-GO TEST: FAIL ($f3 of $((p3+f3)) failed) ================"
	fi

	# --- M5: DR observability.  The cluster is now paused at dr_rp_straddle_done,
	#     so gp_stat_dr_replica / _summary should report all 3 nodes in recovery,
	#     paused, at that same consistent restore point. ---
	echo "================ M5: gp_stat_dr_replica observability (paused at dr_rp_straddle_done) ================"
	p5=0; f5=0
	ok5() { log "dr-test: PASS  $1"; p5=$((p5+1)); }
	no5() { log "dr-test: FAIL  $1"; f5=$((f5+1)); }
	rows=$(dsp "select count(*) from gp_stat_dr_replica;")
	[ "$rows" = 3 ] && ok5 "M5: gp_stat_dr_replica has 3 rows (coordinator + 2 segments)" \
					|| no5 "M5: gp_stat_dr_replica rows = '$rows' (expected 3)"
	r=$(dsp "select count(*) from gp_stat_dr_replica where dr_replica and in_recovery;")
	[ "$r" = 3 ] && ok5 "M5: all 3 nodes report dr_replica=t and in_recovery=t" \
				 || no5 "M5: dr_replica/in_recovery nodes = '$r' (expected 3)"
	r=$(dsp "select count(*) from gp_stat_dr_replica where restore_point = 'dr_rp_straddle_done';")
	[ "$r" = 3 ] && ok5 "M5: all 3 nodes report restore_point='dr_rp_straddle_done' (per-node served N via new C accessor)" \
				 || no5 "M5: nodes at dr_rp_straddle_done = '$r' (expected 3)"
	r=$(dsp "select node_count||'|'||all_in_recovery||'|'||all_paused||'|'||coalesce(consistent_restore_point,'<null>') from gp_stat_dr_replica_summary;")
	[ "$r" = "3|true|true|dr_rp_straddle_done" ] \
		&& ok5 "M5: summary = 3 nodes, all_in_recovery, all_paused, consistent_restore_point=dr_rp_straddle_done" \
		|| no5 "M5: summary = '$r' (expected 3|true|true|dr_rp_straddle_done)"
	r=$(dsp "select rpo_seconds >= 0 from gp_stat_dr_replica_summary;")
	[ "$r" = t ] && ok5 "M5: summary rpo_seconds is a finite, non-negative RPO" \
				 || no5 "M5: rpo_seconds check = '$r'"
	# Faithful served-N: re-point the pause GUC to a name never reached, WITHOUT
	# resuming.  The view must still report the ACTUALLY-reached restore point
	# (pg_last_paused_restore_point / XLogCtl), proving it is the C accessor and
	# not the configured GUC -- the correctness gap the M5 plan flagged.
	for nd in "${NODES[@]}"; do
		sed -i "s/gp_pause_on_restore_point_replay = 'dr_rp_straddle_done'/gp_pause_on_restore_point_replay = 'never_reached'/" "${nd%:*}/postgresql.conf"
		PGOPTIONS='-c gp_role=utility' psql -p "${nd##*:}" -d postgres -Atc 'select pg_reload_conf();' >/dev/null 2>&1 || true
	done
	sleep 3
	r=$(dsp "select coalesce(consistent_restore_point,'<null>') from gp_stat_dr_replica_summary;")
	[ "$r" = "dr_rp_straddle_done" ] \
		&& ok5 "M5: served-N is FAITHFUL -- view still reports dr_rp_straddle_done after GUC re-pointed to 'never_reached' (C accessor, not GUC)" \
		|| no5 "M5: served-N = '$r' after GUC re-point (expected dr_rp_straddle_done; a GUC-based served-N would wrongly show 'never_reached')"
	echo "===================================================================================================="
	if [ "$f5" -eq 0 ]; then
		log "================ M5 OBSERVABILITY TEST: PASS ($p5/$((p5+f5))) ================"
	else
		log "================ M5 OBSERVABILITY TEST: FAIL ($f5 of $((p5+f5)) failed) ================"
	fi

	# --- gg_recovery: the DR recovery-control utility (switch / pause / stats).
	#     The cluster is paused at dr_rp_straddle_done; drive it forward to
	#     dr_rp_switch.  There is no 'follow' mode: a replica only ever serves
	#     while paused at a restore point, which is the one cut with a
	#     cross-node guarantee. ---
	echo "================ gg_recovery: DR recovery-control utility ================"
	p6=0; f6=0
	ok6() { log "dr-test: PASS  $1"; p6=$((p6+1)); }
	no6() { log "dr-test: FAIL  $1"; f6=$((f6+1)); }
	for _ in $(seq 1 60); do [ -f "$ARCHIVE/gg_ready" ] && break; sleep 2; done
	if $GG stats 2>&1 | grep -q "consistent serve point: dr_rp_straddle_done"; then
		ok6 "gg_recovery stats: consistent serve point = dr_rp_straddle_done"
	else
		no6 "gg_recovery stats: did not report dr_rp_straddle_done"; $GG stats 2>&1 | tail -6 >&2
	fi
	if $GG switch dr_rp_switch 2>&1 | grep -q "all 3 node(s) paused at 'dr_rp_switch'"; then
		ok6 "gg_recovery switch dr_rp_switch: all 3 nodes advanced to the new restore point"
	else
		no6 "gg_recovery switch dr_rp_switch: did not reach on all nodes"
	fi
	r=$(dsp "select count(*) from gg_switch;")
	[ "$r" = 5 ] && ok6 "gg_recovery switch: gg_switch=5 now visible (data advanced to dr_rp_switch)" \
				 || no6 "gg_recovery switch: gg_switch = '$r' (expected 5)"
	if $GG stats 2>&1 | grep -q "consistent serve point: dr_rp_switch"; then
		ok6 "gg_recovery stats: consistent serve point moved to dr_rp_switch"
	else
		no6 "gg_recovery stats: did not report dr_rp_switch after the switch"
	fi
	echo "========================================================================="
	if [ "$f6" -eq 0 ]; then
		log "================ gg_recovery TEST: PASS ($p6/$((p6+f6))) ================"
	else
		log "================ gg_recovery TEST: FAIL ($f6 of $((p6+f6)) failed) ================"
	fi

	# --- SQL recovery control: gp_dr_switch() / gp_dr_promote().
	#     The same cluster-wide steps gg_recovery performs, but dispatched from
	#     the coordinator, so a client that can only reach the coordinator can
	#     drive recovery.  Both run here against the live DR: switch advances the
	#     whole cluster to a fresh restore point, then promote cuts it there.
	#     Promote is last because it is irreversible. ---
	echo "================ SQL recovery control: gp_dr_switch / gp_dr_promote ================"
	p7=0; f7=0
	ok7() { log "dr-test: PASS  $1"; p7=$((p7+1)); }
	no7() { log "dr-test: FAIL  $1"; f7=$((f7+1)); }

	# ask production for a fresh restore point beyond dr_rp_switch, and wait for it
	touch "$ARCHIVE/dr_wants_promote_rp"
	for _ in $(seq 1 300); do [ -f "$ARCHIVE/promote_rp_ready" ] && break; sleep 2; done

	# Inside an explicit transaction block on purpose: the segment leg used to be a
	# dispatched `ALTER SYSTEM`, which PreventInTransactionBlock rejects whenever the
	# QE runs it inside the dispatched transaction.  It is a dispatched function call
	# now, which is never subject to that check -- assert the usage stays legal.
	r=$(dsps "begin;
select gp_dr_switch('dr_rp_promote', 300);
commit;")
	echo "$r" | grep -qx t && ok7 "gp_dr_switch('dr_rp_promote') returned true inside a transaction block (whole cluster advanced from the coordinator)" \
				 || no7 "gp_dr_switch returned '$r' (expected t)"
	r=$(dsp "select consistent_restore_point from gp_stat_dr_replica_summary;")
	[ "$r" = dr_rp_promote ] && ok7 "summary consistent_restore_point = dr_rp_promote after gp_dr_switch" \
						  || no7 "summary consistent_restore_point = '$r' (expected dr_rp_promote)"
	r=$(dsp "select count(*) from gg_promote;")
	[ "$r" = 7 ] && ok7 "data as-of dr_rp_promote is visible (gg_promote=7)" \
				 || no7 "gg_promote = '$r' (expected 7)"

	# promote refuses when asked for a point the cluster is not at
	r=$(dsp "select gp_dr_promote('not_this_one', 60);")
	echo "$r" | grep -qi "not \"not_this_one\"\|paused at restore point" \
		&& ok7 "gp_dr_promote refuses a mismatched --at ($(echo "$r" | grep -i ERROR | head -1))" \
		|| no7 "gp_dr_promote did not refuse a mismatched restore point -> $r"

	r=$(dsp "select gp_dr_promote(null, 300);")
	[ "$r" = t ] && ok7 "gp_dr_promote() returned true (cluster promoted at dr_rp_promote)" \
				 || no7 "gp_dr_promote returned '$r' (expected t)"

	# every node must now be OUT of recovery -- and DR mode lifts with it, no restart.
	# Poll: promotion is asynchronous per node, and the coordinator additionally
	# runs distributed-transaction recovery before it accepts connections again.
	outrec=0
	for _ in $(seq 1 60); do
		outrec=0
		for nd in "${NODES[@]}"; do
			[ "$(PGOPTIONS='-c gp_role=utility' psql -p "${nd##*:}" -d postgres -Atc 'select pg_is_in_recovery();' 2>/dev/null)" = f ] && outrec=$((outrec+1))
		done
		[ "$outrec" = "${#NODES[@]}" ] && break
		sleep 2
	done
	[ "$outrec" = "${#NODES[@]}" ] && ok7 "all ${#NODES[@]} nodes OUT of recovery (pg_is_in_recovery()=f)" \
								  || no7 "only $outrec/${#NODES[@]} nodes out of recovery"
	for _ in $(seq 1 30); do r=$(dsp "select dr_replica from gp_stat_dr_replica where gp_segment_id = -1;"); [ "$r" = f ] && break; sleep 2; done
	[ "$r" = f ] && ok7 "dr_replica=f without any restart (mode is keyed on recovery, not a GUC)" \
				 || no7 "dr_replica = '$r' after promotion (expected f)"

	# the promoted cluster must accept a DISTRIBUTED write; FTS needs a probe cycle first.
	wok=
	for _ in $(seq 1 30); do
		if psql -p "$PORT_BASE" -d postgres -q -c \
			"create table gg_online_write (x int) distributed by (x); insert into gg_online_write values (1);" >/dev/null 2>&1; then wok=1; break; fi
		sleep 2
	done
	if [ -n "$wok" ] && [ "$(dsp 'select count(*) from gg_online_write;')" = 1 ]; then
		ok7 "distributed WRITE succeeds on the promoted cluster (online read-write)"
	else
		no7 "distributed write did NOT succeed after promote"
	fi
	echo "==================================================================================="
	if [ "$f7" -eq 0 ]; then
		log "================ SQL RECOVERY-CONTROL TEST: PASS ($p7/$((p7+f7))) ================"
	else
		log "================ SQL RECOVERY-CONTROL TEST: FAIL ($f7 of $((p7+f7)) failed) ================"
	fi
fi

exec sleep infinity
