#!/bin/bash
# dr-entrypoint.sh -- build the DR cluster with `ggdr create-replica`
# (restore base backups + write the DR-local topology store + arm DR mode + start
# every node in continuous archive recovery), then run the milestone regression suite:
# T1-T6 (topology store + unrestricted replay) / M2 (read-only) / M2' (distributed
# read) / M3 (stop-and-go) /
# M5 (observability), the ggdr switch/pause/stats utility test, and finally
# the SQL recovery-control test (gg_dr_switch / gg_dr_promote -> online read-write).
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin   # re-execs as gpadmin, sources greengage_path.sh

# Wait for a marker from THIS run.  /archive is a named volume that survives
# `docker-compose down`, so a marker left by the previous run would send this
# container off to build a replica from last run's base backups -- green, and
# meaningless.  The primary wipes /archive at its own start; requiring the marker
# to be newer than this container closes the gap between the two.
DR_START=$(date +%s)
log "dr: waiting for primary to publish base backups ..."
marker_is_fresh() {
	[ -f "$READY_MARKER" ] || return 1
	[ "$(stat -c %Y "$READY_MARKER" 2>/dev/null || echo 0)" -ge "$DR_START" ]
}
warned=
for _ in $(seq 1 600); do
	marker_is_fresh && break
	if [ -z "$warned" ] && [ -f "$READY_MARKER" ]; then
		warned=1
		log "dr: NOTE $READY_MARKER predates this container -- ignoring it and waiting"
		log "dr:      for one from this run.  If the primary published BEFORE this"
		log "dr:      container started (e.g. the DR was restarted on its own), it"
		log "dr:      will not publish again: restart both, or 'down -v' and up."
	fi
	sleep 2
done
marker_is_fresh || die "no $READY_MARKER from this run; restart both containers, or 'docker-compose down -v' first"
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
# the DR cluster is built by ggdr (not gpdemo), so no such file exists here.
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

# --- M4: build the whole DR cluster in one shot via the ggdr utility.
#     create-replica restores each instance's base backup, writes the DR-local
#     topology (hostname 'dr') into every node's own $PGDATA/gp_topology, gates on
#     the WAL archive being complete enough to reach consistency, arms DR mode
#     (gp_topology_source=file + hot_standby + restore_command + standby.signal) on
#     every node with the M3 pause target dr_rp1, and starts each in archive
#     recovery.  This exercises the SAME code path an operator would run, instead
#     of open-coding the restore/write/arm/start inline. ---
GG="python3 $SRC/gpMgmt/bin/ggdr"
export PGPORT="$PORT_BASE" PGDATABASE=postgres

# --- auxiliary-tooling gate: gg_walfilter black-box tests (against the
#     installed binary; no cluster needed).  The DR's topology is protected by
#     living outside the WAL now, not by the filter, but the shipped filtering
#     tool must still pass its own suite before we bless the build. ---
log "dr: running the gg_walfilter tests (black-box, installed binary) ..."
if GG_WALFILTER="$(command -v gg_walfilter)" \
	python3 "$SRC/src/test/dr/test_gg_walfilter.py" >/tmp/walfilter-test.log 2>&1; then
	tail -2 /tmp/walfilter-test.log | sed 's/^/    walfilter-test: /'
	log "dr: gg_walfilter tests PASSED"
else
	tail -20 /tmp/walfilter-test.log | sed 's/^/    walfilter-test: /'
	die "gg_walfilter tests FAILED; refusing to build the DR cluster"
fi

log "dr: building the DR replica via 'ggdr create-replica' (M4) ..."
set +e
$GG create-replica \
	--topology "$TOPO_FILE" \
	--basebackup-dir "$BASEBACKUP" \
	--wal-archive "$WAL_ARCHIVE" \
	--pause-at dr_rp1 \
	--force 2>&1 | sed 's/^/    ggdr: /'
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

echo "================ T1-T6 + M2 assertions on the LIVE DR coordinator (in recovery) ================"
# T1 topology independence: production changed seg0's hostname to
# $PRODUCTION_SEG0_HOSTNAME after the base backup.  The DR must not show it -- not
# because anything filters that record (nothing does, as of P7), but because the
# topology the DR serves lives in $PGDATA/gp_topology, which production's WAL
# cannot reach.
drhost=$(q "select hostname from gp_segment_configuration where content=0;")
if [ -n "$drhost" ] && [ "$drhost" != "$PRODUCTION_SEG0_HOSTNAME" ]; then
	ok_ "T1 topology independence: DR seg0 hostname still '$drhost' (production changed it to '$PRODUCTION_SEG0_HOSTNAME')"
else
	no_ "T1 topology independence: DR seg0 hostname='$drhost', production='$PRODUCTION_SEG0_HOSTNAME'"
fi
# T2 topology is DR-local: not merely "ignored production's change" -- the value is
# one only this cluster ever had.
if [ "$drhost" = "dr" ]; then
	ok_ "T2 topology is DR-local: DR seg0 hostname is 'dr'"
else
	no_ "T2 topology is DR-local: DR seg0 hostname='$drhost' (expected 'dr')"
fi
# T3 replay is unrestricted, part 1: the catalog production's WAL writes is the one
# the DR now ignores, so it holds PRODUCTION's rows -- including, as of P7, the
# change T1 just proved the DR does not serve.  Both statements are true at once,
# and that is the whole point: replay is complete, and topology is separate.
cathost=$(q "select hostname from gp_segment_configuration_internal where content=0;")
if [ "$cathost" = "$PRODUCTION_SEG0_HOSTNAME" ]; then
	ok_ "T3 replay is unrestricted: the catalog replayed production's change to '$cathost' while the replica serves '$drhost'"
elif [ -n "$cathost" ] && [ "$cathost" != "dr" ]; then
	no_ "T3 replay is unrestricted: catalog seg0 hostname='$cathost', expected production's '$PRODUCTION_SEG0_HOSTNAME' (was the record filtered?)"
else
	no_ "T3 replay is unrestricted: catalog seg0 hostname='$cathost' (expected production's, not 'dr')"
fi
# T3, part 2: a user table created on production AFTER the base backup must appear
# on the DR via WAL replay.
if [ "$(q "select count(*) from pg_class where relname = 'dr_wal_applied';")" = 1 ]; then
	ok_ "T3 replay is unrestricted: user table 'dr_wal_applied' (created post-backup) replayed onto the DR"
else
	no_ "T3 replay is unrestricted: user table 'dr_wal_applied' NOT present on the DR"
fi
# T4: every node -- not just the coordinator -- runs the file provider.  A node left
# on 'catalog' would describe production, and gp_topology_source is exempt from the
# QD/QE GUC-sync check, so nothing else would say so.
t4_bad=
for content in $ALL_CONTENTS; do
	src=$(PGOPTIONS='-c gp_role=utility' psql -p "${DR_PORT[$content]}" -d postgres -Atc "show gp_topology_source;" 2>/dev/null)
	[ "$src" = "file" ] || t4_bad="$t4_bad content=$content:'${src:-<unreachable>}'"
done
if [ -z "$t4_bad" ]; then
	ok_ "T4 every node reports gp_topology_source=file"
else
	no_ "T4 nodes not on the file provider:$t4_bad"
fi
# T5: gp_configuration_history IS replayed now.  It used to be protected alongside
# gp_segment_configuration, and nothing needs it to be: the DR does not read it, and
# keeping production's history is the more useful behaviour.  Asserting it here
# catches anyone re-adding protection out of habit.
if [ "$(q "select count(*) from gp_configuration_history where description like 'T5 marker:%';")" = 1 ]; then
	t5_rows=$(q "select count(*) from gp_configuration_history;")
	ok_ "T5 gp_configuration_history is replayed, not protected (production's marker row is here; $t5_rows row(s) total)"
else
	no_ "T5 production's gp_configuration_history marker row did NOT reach the DR (is something still filtering it?)"
fi
# T6 is the acceptance test for deleting the redo filter: production runs with
# wal_consistency_checking, so every record carries a full-page image and redo
# compares the DR's own page against production's.  A mismatch is a PANIC naming the
# rmgr and block, so the proof is simply that recovery got this far without one.
# While the filter existed this could not be run: it skipped checkXLogConsistency()
# alongside redo, so silence proved nothing about the records it skipped.
#
# Match the failure precisely.  checkXLogConsistency() says "inconsistent page
# found, rel .../..., forknum N, blkno N" (xlog.c:1545) and a corrupt record is a
# PANIC.  A blanket FATAL sweep does NOT work here: the loop above polls the
# coordinator for connections while it is still starting, and every one of those
# attempts logs FATAL 57P03 "the database system is starting up".
t6_pat='inconsistent page found|PANIC'
t6_hits=$(cat "$COORD"/log/*.csv 2>/dev/null | grep -cE "$t6_pat" || true)
# It is PRODUCTION's setting that stamps the full-page images into the WAL; the
# replica checks whenever a record carries XLR_CHECK_CONSISTENCY, whatever its own
# GUC says.  So report what production was told to do, not what this node reports.
if [ "${t6_hits:-0}" -eq 0 ]; then
	ok_ "T6 wal_consistency_checking='${WAL_CONSISTENCY_CHECKING:-all}' on production: replayed with no page inconsistency"
else
	no_ "T6 wal_consistency_checking: $t6_hits page-inconsistency/PANIC line(s) on the DR coordinator"
	grep -hE "$t6_pat" "$COORD"/log/*.csv 2>/dev/null | head -3 | cut -c1-300 | sed 's/^/    T6: /'
fi
# M2 (reads work): coordinator-only catalog read while in recovery.
if [ "$(q "select count(*) > 0 from gp_segment_configuration;")" = t ]; then
	ok_ "M2 coordinator-only read works (count(gp_segment_configuration) > 0)"
else
	no_ "M2 coordinator-only read FAILED"
fi
# M2 (writes refused): FOR UPDATE + DML go through ExecCheckXactReadOnly (DR message);
# DDL goes through check_xact_readonly (stock read-only message). All must fail.
refused "M2 SELECT ... FOR UPDATE" "select id from dr_marker for update;"
refused "M2 DML (INSERT)"          "insert into dr_marker values (99, 'must be refused');"
refused "M2 DDL (CREATE TABLE)"    "create table dr_should_not_exist (x int);"
# The check above deliberately locks a real table: over a view there is no rowmark,
# so the DR executor gate never runs and the assertion would pass while testing
# nothing.  Assert separately that FOR UPDATE on the topology view fails at all --
# it does, but in the parser ("cannot be applied to a function"), which is a
# different guarantee and worth stating as one.
# M2'' a topology WRITE must be refused too, and this one is not covered by the
# read-only executor gate: the segadmin functions are plain CMD_SELECTs, and under
# the file provider persisting needs no XID, so nothing else would stop a superuser
# durably rewriting $PGDATA/gp_topology on a replica in recovery.  The catalog
# provider used to refuse it only as a side effect of needing a transaction id.
tw_out=$(qfail "select gp_update_segment_mode_status(2::int2, 'n'::\"char\", 'd'::\"char\");")
if echo "$tw_out" | grep -qi "during recovery"; then
	ok_ "M2'' topology write refused -> $(echo "$tw_out" | grep -iE 'ERROR' | head -1)"
else
	no_ "M2'' topology write NOT refused -> ${tw_out:-<no output>}"
fi
# and the store on disk is untouched by the attempt
tw_gen=$(gg_topology dump -D "$COORD" 2>/dev/null | awk '/^generation/{print $2}')
if [ "${tw_gen:-0}" = 1 ]; then
	ok_ "M2'' the topology store is still at generation 1 (the refused write left no trace)"
else
	no_ "M2'' the topology store is at generation '${tw_gen:-<unreadable>}' (expected 1)"
fi
fu_out=$(qfail "select dbid from gp_segment_configuration for update;")
if echo "$fu_out" | grep -qiE "ERROR"; then
	ok_ "M2' FOR UPDATE on the topology view is rejected -> $(echo "$fu_out" | grep -iE 'ERROR' | head -1)"
else
	no_ "M2' FOR UPDATE on the topology view was NOT rejected -> ${fu_out:-<no output>}"
fi
echo "============================================================================================"
if [ "$fail" -eq 0 ]; then
	log "================ DR T1-T6 + M2 TEST: PASS ($pass/$((pass+fail)) checks) ================"
else
	log "================ DR T1-T6 + M2 TEST: FAIL ($fail of $((pass+fail)) failed) ================"
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
	advance() {  # $1=to : move the whole cluster to a new restore point AND serve it
		# Deliberately gg_dr_switch() and not a per-node rearm+resume.  Rearming by
		# hand still gets every node paused at the right place, but reads would go
		# on answering as of the OLD point forever: what makes a node start serving
		# a new point is the publish step, and that is only safe once every node
		# has arrived, which only the coordinator can establish.
		psql -p "$PORT_BASE" -d postgres -Atc "select gg_dr_switch('$1');" >/dev/null 2>&1
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

		# --- M3 SKEW (C-12 regression): a read taken while the cluster is mid-advance
		#     must still answer as of the point it is SERVING, not as of wherever each
		#     node's replay happens to have got to.
		#
		#     Made deterministic rather than raced: drive the SEGMENTS to dr_rp2 by hand
		#     and leave the coordinator at dr_rp1.  That is exactly the state an advance
		#     passes through -- segments ahead, no new point published -- but it holds
		#     still instead of lasting milliseconds.  dr_m3's second row is replayed and
		#     locally committed on a segment throughout, so a live snapshot shows 2.  The
		#     frozen dr_rp1 image must keep showing 1 until gg_dr_switch() publishes.
		# 'ggdr switch --content' IS this primitive: arm the segments' pause target,
		# reload, resume, wait -- and deliberately not publish, which is precisely
		# what holds the coordinator serving dr_rp1 while the segments sit at
		# dr_rp2.  Doing it by hand here meant sed'ing postgresql.conf, which is not
		# where the target lives once anything has switched: gg_dr_switch() writes
		# it with ALTER SYSTEM, and postgresql.auto.conf overrides postgresql.conf.
		#
		# `timeout` because ggdr's wait has none by design (an operator Ctrl-Cs it),
		# and a test needs a bound.  Killing the wait leaves the target armed, so
		# the segments still get there -- segs_reached below is what decides.
		skew_segments_to() {  # $1=to : re-point the segments only, publishing nothing
			set +e
			timeout 300 $GG switch "$1" --content "${SEG_CONTENTS//$'\n'/,}" 2>&1 | sed 's/^/    ggdr: /'
			local rc=${PIPESTATUS[0]}
			set -e
			[ "$rc" = 0 ] || log "dr-test: M3 skew: 'ggdr switch --content' exited $rc (see above)"
		}
		segs_reached() {  # $1=restore point : true iff every SEGMENT has replayed to it
			local content pp
			for content in $SEG_CONTENTS; do
				pp=${DR_PORT[$content]}
				[ "$(PGOPTIONS='-c gp_role=utility' psql -p "$pp" -d postgres \
					-Atc "select coalesce(pg_last_paused_restore_point(),'');" 2>/dev/null)" = "$1" ] || return 1
			done
			return 0
		}
		log "dr-test: M3 skew: driving the SEGMENTS to dr_rp2 while the coordinator stays at dr_rp1 ..."
		skew_segments_to dr_rp2
		for _ in $(seq 1 90); do segs_reached dr_rp2 && break; sleep 2; done
		if segs_reached dr_rp2; then
			ok3 "M3 skew: segments replayed to dr_rp2, coordinator still at dr_rp1 (mid-advance state, held)"
			r=$(dsp 'select count(*) from dr_m3;')
			[ "$r" = 1 ] && ok3 "M3 skew: mid-advance read STILL = 1 (served as-of dr_rp1, not as-of replay-now)" \
						 || no3 "M3 skew: mid-advance read = '$r' (expected 1 -- a torn read of the advancing segments)"
			r=$(dsp 'select count(*) from dr_m3;')
			[ "$r" = 1 ] && ok3 "M3 skew: repeated mid-advance read STILL = 1 (stable, not drifting with replay)" \
						 || no3 "M3 skew: repeated mid-advance read = '$r' (expected 1)"

			# An index-only scan is the one path a frozen snapshot cannot reach by
			# itself: an all-visible page lets it answer from the index and skip
			# the heap, so no visibility check runs at all.  Production VACUUMed
			# dr_ios on both sides of dr_rp1, so the segments (now at dr_rp2) have
			# replayed XLOG_HEAP2_VISIBLE for the post-rp1 pages.  Forcing the plan
			# must still give the dr_rp1 answer, and must show heap fetches -- both
			# only hold because visibilitymap_get_status() reports nothing
			# all-visible in DR mode (ADR-0005 D4).
			r=$(dsp 'set enable_seqscan=off; set enable_bitmapscan=off; select count(k) from dr_ios;')
			[ "$r" = 200 ] && ok3 "M3 skew: index-only scan mid-advance = 200 (as-of dr_rp1; the post-rp1 rows are all-visible on the segments and still not returned)" \
						   || no3 "M3 skew: index-only scan mid-advance = '$r' (expected 200 -- the visibility-map fast path leaked replay-now rows)"
			# `|| true` because grep -c exits 1 when the count is 0 -- which is the
			# PASSING case here, and under `set -e` a failing command substitution
			# aborts the script.
			r=$(dsp 'set enable_seqscan=off; set enable_bitmapscan=off; explain (analyze, costs off, timing off, summary off) select count(k) from dr_ios;' | grep -ci 'Heap Fetches: 0$' || true)
			[ "$r" = 0 ] && ok3 "M3 skew: no zero-heap-fetch index-only scan on the replica (visibility-map fast path disabled)" \
						 || no3 "M3 skew: $r index-only scan node(s) reported 'Heap Fetches: 0' (the heap was skipped)"

			# Reported 2026-08-13: with a subset re-pointed, every node is still
			# SERVING the old point -- and status used to report only the point
			# replay had reached and call it the consistent serve point, which
			# describes this cluster wrongly.  The state under test right here is
			# exactly that one, so assert the two points are reported apart.
			r=$(dsp "select count(*) from gg_stat_dr_replica where served_restore_point = 'dr_rp1';")
			[ "$r" = 3 ] && ok3 "M3 skew: all 3 nodes report served_restore_point=dr_rp1 (what reads are answered as of)" \
						 || no3 "M3 skew: nodes serving dr_rp1 = '$r' (expected 3)"
			r=$(dsp "select count(*) from gg_stat_dr_replica where restore_point = 'dr_rp2';")
			[ "$r" = 2 ] && ok3 "M3 skew: 2 nodes report restore_point=dr_rp2 -- reached and served genuinely differ" \
						 || no3 "M3 skew: nodes stopped at dr_rp2 = '$r' (expected 2)"
			r=$(dsp "select coalesce(consistent_restore_point,'<null>')||'|'||coalesce(consistent_paused_point,'<null>') from gg_stat_dr_replica_summary;")
			[ "$r" = "dr_rp1|<null>" ] \
				&& ok3 "M3 skew: summary = serving dr_rp1, no single reached point (the skew is visible, not papered over)" \
				|| no3 "M3 skew: summary consistent_restore_point|consistent_paused_point = '$r' (expected 'dr_rp1|<null>')"
		else
			no3 "M3 skew: segments did not reach dr_rp2 (skew state not established)"
		fi

		log "dr-test: M3 advancing to dr_rp2 via gg_dr_switch (publishes the new point cluster-wide) ..."
		advance dr_rp2
		for _ in $(seq 1 90); do [ "$(dsp 'select count(*) from dr_m3;')" = 2 ] && break; sleep 2; done
		r=$(dsp 'select count(*) from dr_m3;')
		[ "$r" = 2 ] && ok3 "M3: after advancing to dr_rp2, dr_m3 count = 2 (new data now visible as-of rp2)" \
					 || no3 "M3: as-of dr_rp2 dr_m3 = '$r' (expected 2)"
		# The other half of the bug above: once the publish lands, the served point
		# is the one that moved.  Reads changing and the summary changing must be
		# the same event, or the status is lying in the other direction.
		r=$(dsp "select coalesce(consistent_restore_point,'<null>')||'|'||coalesce(consistent_paused_point,'<null>') from gg_stat_dr_replica_summary;")
		[ "$r" = "dr_rp2|dr_rp2" ] \
			&& ok3 "M3: after the publish, served and reached agree at dr_rp2 (summary and reads moved together)" \
			|| no3 "M3: summary consistent_restore_point|consistent_paused_point = '$r' (expected 'dr_rp2|dr_rp2')"

		# --- M3 STRADDLE: advance to dr_rp_straddle, where a distributed 2PC txn T
		#     spanning both segments straddles the cut -- its DISTRIBUTED_COMMIT is
		#     replayed on the coordinator (shmCommittedGxidArray != 0) but it is
		#     prepared-only on the segments.  CreateDRStandbyDistributedSnapshot must
		#     EXCLUDE T (in-doubt) and NOT error, so dr_straddle shows only the 10
		#     baseline rows; after advancing past T's forget, T's 20 rows appear (30). ---
		log "dr-test: M3 straddle: advancing dr_rp2 -> dr_rp_straddle (the straddle cut) ..."
		advance dr_rp_straddle
		for _ in $(seq 1 90); do all_paused && break; sleep 2; done
		r=$(dsp 'select count(*) from dr_straddle;')
		if [ "$r" = 10 ]; then
			ok3 "M3 straddle: at the cut dr_straddle count = 10 (in-doubt 2PC txn excluded; no shmNumCommittedGxacts error)"
		else
			no3 "M3 straddle: at the cut dr_straddle = '$r' (expected 10; $(echo "$r" | grep -iE 'ERROR' | head -1))"
		fi
		log "dr-test: M3 straddle: advancing dr_rp_straddle -> dr_rp_straddle_done (past T's forget) ..."
		advance dr_rp_straddle_done
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
	#     so gg_stat_dr_replica / _summary should report all 3 nodes in recovery,
	#     paused, at that same consistent restore point. ---
	echo "================ M5: gg_stat_dr_replica observability (paused at dr_rp_straddle_done) ================"
	p5=0; f5=0
	ok5() { log "dr-test: PASS  $1"; p5=$((p5+1)); }
	no5() { log "dr-test: FAIL  $1"; f5=$((f5+1)); }
	rows=$(dsp "select count(*) from gg_stat_dr_replica;")
	[ "$rows" = 3 ] && ok5 "M5: gg_stat_dr_replica has 3 rows (coordinator + 2 segments)" \
					|| no5 "M5: gg_stat_dr_replica rows = '$rows' (expected 3)"
	r=$(dsp "select count(*) from gg_stat_dr_replica where dr_replica and in_recovery;")
	[ "$r" = 3 ] && ok5 "M5: all 3 nodes report dr_replica=t and in_recovery=t" \
				 || no5 "M5: dr_replica/in_recovery nodes = '$r' (expected 3)"
	r=$(dsp "select count(*) from gg_stat_dr_replica where restore_point = 'dr_rp_straddle_done';")
	[ "$r" = 3 ] && ok5 "M5: all 3 nodes report restore_point='dr_rp_straddle_done' (per-node served N via new C accessor)" \
				 || no5 "M5: nodes at dr_rp_straddle_done = '$r' (expected 3)"
	r=$(dsp "select node_count||'|'||all_in_recovery||'|'||all_paused||'|'||coalesce(consistent_restore_point,'<null>') from gg_stat_dr_replica_summary;")
	[ "$r" = "3|true|true|dr_rp_straddle_done" ] \
		&& ok5 "M5: summary = 3 nodes, all_in_recovery, all_paused, consistent_restore_point=dr_rp_straddle_done" \
		|| no5 "M5: summary = '$r' (expected 3|true|true|dr_rp_straddle_done)"
	r=$(dsp "select rpo_seconds >= 0 from gg_stat_dr_replica_summary;")
	[ "$r" = t ] && ok5 "M5: summary rpo_seconds is a finite, non-negative RPO" \
				 || no5 "M5: rpo_seconds check = '$r'"
	# Faithful served-N: re-point the pause GUC to a name never reached, WITHOUT
	# resuming.  The view must still report the ACTUALLY-reached restore point
	# (pg_last_paused_restore_point / XLogCtl), proving it is the C accessor and
	# not the configured GUC -- the correctness gap the M5 plan flagged.
	# ALTER SYSTEM, not sed: gg_dr_switch() armed dr_rp_straddle_done through
	# AlterSystemSetConfigFile, so the live value is in postgresql.auto.conf --
	# which also overrides postgresql.conf.  The sed this replaces went looking
	# for it in postgresql.conf, matched nothing, and left the GUC exactly where
	# it was, so the check below used to pass without re-pointing anything.
	for nd in "${NODES[@]}"; do
		PGOPTIONS='-c gp_role=utility' psql -p "${nd##*:}" -d postgres \
			-Atc "alter system set gp_pause_on_restore_point_replay = 'never_reached';" >/dev/null 2>&1 || true
		PGOPTIONS='-c gp_role=utility' psql -p "${nd##*:}" -d postgres -Atc 'select pg_reload_conf();' >/dev/null 2>&1 || true
	done
	sleep 3
	# Assert the re-point landed before asserting what survives it: a re-point
	# that silently does nothing makes the served-N check below prove nothing.
	r=$(q "show gp_pause_on_restore_point_replay;")
	[ "$r" = "never_reached" ] \
		&& ok5 "M5: the pause GUC really is re-pointed to 'never_reached' (so the check below is not vacuous)" \
		|| no5 "M5: pause GUC = '$r' after the re-point (expected never_reached -- the served-N check below would prove nothing)"
	r=$(dsp "select coalesce(consistent_restore_point,'<null>') from gg_stat_dr_replica_summary;")
	[ "$r" = "dr_rp_straddle_done" ] \
		&& ok5 "M5: served-N is FAITHFUL -- view still reports dr_rp_straddle_done after GUC re-pointed to 'never_reached' (C accessor, not GUC)" \
		|| no5 "M5: served-N = '$r' after GUC re-point (expected dr_rp_straddle_done; a GUC-based served-N would wrongly show 'never_reached')"
	echo "===================================================================================================="
	if [ "$f5" -eq 0 ]; then
		log "================ M5 OBSERVABILITY TEST: PASS ($p5/$((p5+f5))) ================"
	else
		log "================ M5 OBSERVABILITY TEST: FAIL ($f5 of $((p5+f5)) failed) ================"
	fi

	# --- ggdr: the DR recovery-control utility (switch / pause / stats).
	#     The cluster is paused at dr_rp_straddle_done; drive it forward to
	#     dr_rp_switch.  There is no 'follow' mode: a replica only ever serves
	#     while paused at a restore point, which is the one cut with a
	#     cross-node guarantee. ---
	echo "================ ggdr: DR recovery-control utility ================"
	p6=0; f6=0
	ok6() { log "dr-test: PASS  $1"; p6=$((p6+1)); }
	no6() { log "dr-test: FAIL  $1"; f6=$((f6+1)); }
	for _ in $(seq 1 60); do [ -f "$ARCHIVE/gg_ready" ] && break; sleep 2; done
	# 'serving reads as of', not the old 'consistent serve point': stat reports the
	# served point and the reached point as two lines now, because they are two
	# facts and a cluster can hold them apart.
	if $GG stat 2>&1 | grep -q "serving reads as of: dr_rp_straddle_done"; then
		ok6 "ggdr stat: serving reads as of dr_rp_straddle_done"
	else
		no6 "ggdr stat: did not report dr_rp_straddle_done"; $GG stat 2>&1 | tail -8 >&2
	fi
	if $GG stat 2>&1 | grep -q "replay stopped at:   dr_rp_straddle_done$"; then
		ok6 "ggdr stat: reports the reached point separately, and it agrees after a whole-cluster switch"
	else
		no6 "ggdr stat: 'replay stopped at' did not report a clean dr_rp_straddle_done"; $GG stat 2>&1 | tail -8 >&2
	fi
	# rpo_seconds is the age of the SERVED CUT, so advancing to a LATER restore
	# point must lower it.  Production created dr_rp_switch well after
	# dr_rp_straddle_done and the switch itself takes a second or two, so the drop
	# is that gap less the switch.  Under the old definition -- age of the last
	# replayed commit -- this was free to go either way, and with production idle
	# between the two points it could only grow.
	rpo_before=$(dsp "select round(rpo_seconds)::int from gg_stat_dr_replica_summary;")
	if $GG switch dr_rp_switch 2>&1 | grep -q "all 3 node(s) paused at 'dr_rp_switch'"; then
		ok6 "ggdr switch dr_rp_switch: all 3 nodes advanced to the new restore point"
	else
		no6 "ggdr switch dr_rp_switch: did not reach on all nodes"
	fi
	rpo_after=$(dsp "select round(rpo_seconds)::int from gg_stat_dr_replica_summary;")
	if [ -n "$rpo_before" ] && [ -n "$rpo_after" ] && [ "$rpo_after" -lt "$rpo_before" ] 2>/dev/null; then
		ok6 "ggdr: rpo_seconds fell with the served cut ($rpo_before -> $rpo_after; it ages the cut, not the last commit)"
	else
		no6 "ggdr: rpo_seconds went '$rpo_before' -> '$rpo_after' (expected a drop; a newer cut is a younger recovery point)"
	fi
	r=$(dsp "select count(*) from gg_switch;")
	[ "$r" = 5 ] && ok6 "ggdr switch: gg_switch=5 now visible (data advanced to dr_rp_switch)" \
				 || no6 "ggdr switch: gg_switch = '$r' (expected 5)"
	if $GG stat 2>&1 | grep -q "serving reads as of: dr_rp_switch"; then
		ok6 "ggdr stat: the served point moved to dr_rp_switch"
	else
		no6 "ggdr stat: did not report dr_rp_switch after the switch"
	fi
	echo "========================================================================="
	if [ "$f6" -eq 0 ]; then
		log "================ ggdr TEST: PASS ($p6/$((p6+f6))) ================"
	else
		log "================ ggdr TEST: FAIL ($f6 of $((p6+f6)) failed) ================"
	fi

	# --- SQL recovery control: gg_dr_switch() / gg_dr_promote().
	#     The same cluster-wide steps ggdr performs, but dispatched from
	#     the coordinator, so a client that can only reach the coordinator can
	#     drive recovery.  Both run here against the live DR: switch advances the
	#     whole cluster to a fresh restore point, then promote cuts it there.
	#     Promote is last because it is irreversible. ---
	echo "================ SQL recovery control: gg_dr_switch / gg_dr_promote ================"
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
select gg_dr_switch('dr_rp_promote');
commit;")
	echo "$r" | grep -qx t && ok7 "gg_dr_switch('dr_rp_promote') returned true inside a transaction block (whole cluster advanced from the coordinator)" \
				 || no7 "gg_dr_switch returned '$r' (expected t)"
	r=$(dsp "select consistent_restore_point from gg_stat_dr_replica_summary;")
	[ "$r" = dr_rp_promote ] && ok7 "summary consistent_restore_point = dr_rp_promote after gg_dr_switch" \
						  || no7 "summary consistent_restore_point = '$r' (expected dr_rp_promote)"
	r=$(dsp "select count(*) from gg_promote;")
	[ "$r" = 7 ] && ok7 "data as-of dr_rp_promote is visible (gg_promote=7)" \
				 || no7 "gg_promote = '$r' (expected 7)"

	# dsp() folds stderr in so assertions can grep for ERROR, which means server
	# NOTICEs land in the value too.  gg_dr_promote() emits one (production's
	# gp_configuration_history is now replayed onto the replica, and this SQL path
	# -- unlike `ggdr promote` -- cannot tidy it up), so take the result line.
	r_raw=$(dsp "select gg_dr_promote();")
	r=$(printf '%s\n' "$r_raw" | grep -vE '^(NOTICE|HINT|DETAIL|CONTEXT|WARNING):' | tail -1)
	[ "$r" = t ] && ok7 "gg_dr_promote() returned true (cluster promoted at dr_rp_promote)" \
				 || no7 "gg_dr_promote returned '$r' (expected t)"
	printf '%s\n' "$r_raw" | grep -q 'gp_configuration_history still holds' \
		&& ok7 "gg_dr_promote() warns that gp_configuration_history came from production" \
		|| no7 "gg_dr_promote() did not warn about the replayed gp_configuration_history"

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
	for _ in $(seq 1 30); do r=$(dsp "select dr_replica from gg_stat_dr_replica where gp_segment_id = -1;"); [ "$r" = f ] && break; sleep 2; done
	[ "$r" = f ] && ok7 "dr_replica=f without any restart (mode is keyed on recovery, not a GUC)" \
				 || no7 "dr_replica = '$r' after promotion (expected f)"
	# Recovery-position columns must go NULL once out of recovery: they do not error
	# there, they keep reporting the last replayed position forever, which is stale
	# trivia on a production cluster -- and it made rpo_seconds grow without bound
	# for a cluster that has no RPO at all.
	r=$(dsp "select count(*) from gg_stat_dr_replica where replay_lsn is not null or replay_time is not null;")
	[ "$r" = 0 ] && ok7 "replay_lsn/replay_time are NULL on all nodes after promotion (no stale recovery position)" \
				 || no7 "$r node(s) still report replay_lsn/replay_time after promotion (expected 0)"
	r=$(dsp "select coalesce(rpo_seconds::text,'<null>') from gg_stat_dr_replica_summary;")
	[ "$r" = "<null>" ] && ok7 "summary rpo_seconds is NULL after promotion (a promoted cluster has no RPO)" \
					   || no7 "rpo_seconds = '$r' after promotion (expected NULL)"

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
