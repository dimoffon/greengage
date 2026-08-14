#!/bin/bash
# maint-dr-entrypoint.sh -- DR side of the V-13' fixture.
#
# Production runs VACUUM, VACUUM FULL, REINDEX and TRUNCATE on its topology
# catalog while this replica is attached (see maint-primary-entrypoint.sh).  The
# last three rewrite a mapped shared catalog's relfilenode and emit a shared
# relmap update -- the record the old design halted on with FATAL.
#
# This script advances the replica across each operation in turn and asserts, per
# advance, that the replica did not notice:
#
#   * same postmaster PID as before        (it did not crash and restart)
#   * seg0 hostname still 'dr'             (its topology is still its own)
#   * node count unchanged                 (and still complete)
#   * a DISTRIBUTED read still dispatches  (the topology is usable, not merely
#                                           present -- the fixture this replaces
#                                           only ever read in utility mode,
#                                           because it expected the catalog might
#                                           be destroyed)
#   * no FATAL in the log
#   * the old guard's log line appears ZERO times
#
# and that production's rewrite really happened (relfilenode changed), so a run
# that quietly did nothing cannot pass.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin

# `set -e` exits silently, which in a fixture is indistinguishable from a hang:
# the container just stops mid-run with no line telling you which command failed.
# Say it.  `set -E` is required, not decoration -- without errtrace an ERR trap is
# NOT inherited by shell functions, so a failure inside one exits just as quietly
# as before while the trap looks installed.
set -E
trap 'rc=$?; echo "maint-dr: ABORT line $LINENO: [$BASH_COMMAND] exited $rc" >&2; exit $rc' ERR

# See dr-entrypoint.sh: /archive outlives `down`, so only a marker from THIS run
# will do.
DR_START=$(date +%s)
log "maint-dr: waiting for primary to publish base backups ..."
marker_is_fresh() {
	[ -f "$READY_MARKER" ] || return 1
	[ "$(stat -c %Y "$READY_MARKER" 2>/dev/null || echo 0)" -ge "$DR_START" ]
}
warned=
for _ in $(seq 1 900); do
	marker_is_fresh && break
	if [ -z "$warned" ] && [ -f "$READY_MARKER" ]; then
		warned=1
		log "maint-dr: NOTE $READY_MARKER predates this container -- ignoring it and waiting"
		log "maint-dr:      for one from this run.  If the primary published BEFORE this"
		log "maint-dr:      container started (e.g. the DR was restarted on its own), it"
		log "maint-dr:      will not publish again: restart both, or 'down -v' and up."
	fi
	sleep 2
done
marker_is_fresh || die "no $READY_MARKER from this run; restart both containers, or 'docker-compose down -v' first"
source "$ARCHIVE/maint_prod_state.env"
log "maint-dr: production catalog densified to $PAGES_DENSE page(s); relfilenode $RFN_PRE"

declare -A DR_DATADIR DR_PORT
while IFS=$'\t' read -r dbid content role prefrole mode status port hostname address datadir; do
	[ -n "${content:-}" ] || continue
	DR_DATADIR[$content]=$datadir
	DR_PORT[$content]=$port
done < "$TOPO_FILE"
ALL_CONTENTS=$(printf '%s\n' "${!DR_DATADIR[@]}" | sort -n)
COORD=${DR_DATADIR[-1]}
export COORDINATOR_DATA_DIRECTORY="$COORD" MASTER_DATA_DIRECTORY="$COORD" PGPORT="$PORT_BASE" PGDATABASE=postgres

NODES=()
for content in $ALL_CONTENTS; do NODES+=("${DR_DATADIR[$content]}:${DR_PORT[$content]}"); done

GG="python3 $SRC/gpMgmt/bin/ggdr"
log "maint-dr: building the replica (paused at maint_rp_pre, before any maintenance) ..."
set +e
$GG create \
	--topology "$TOPO_FILE" \
	--backup "$BASEBACKUP" \
	--wal "$WAL_ARCHIVE" \
	--pause-at maint_rp_pre \
	--force 2>&1 | sed 's/^/    ggdr: /'
set -e

# Both swallow failure on purpose.  This script runs under `set -e`, so a query
# that errors -- which is a thing an assertion should REPORT -- would otherwise
# abort the run silently and look like a hang.  An empty result fails the
# assertion that reads it, which is the behaviour a test wants.
q()   { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }
dsp() { psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }

log "maint-dr: waiting for the coordinator to accept read connections ..."
ok=
for _ in $(seq 1 80); do q "select 1" >/dev/null && { ok=1; break; }; sleep 3; done
[ -n "$ok" ] || { tail -30 "$COORD"/log/*.csv >&2 2>/dev/null; die "DR coordinator never came up"; }

all_paused() {
	local nd pp
	for nd in "${NODES[@]}"; do
		pp=${nd##*:}
		[ "$(PGOPTIONS='-c gp_role=utility' psql -p "$pp" -d postgres -Atc 'select pg_is_wal_replay_paused();' 2>/dev/null)" = t ] || return 1
	done
	return 0
}
# Advance the whole cluster to a restore point and start serving it.
#
# Deliberately tolerant: gg_dr_switch() can legitimately fail transiently (the
# point's WAL may not be archived yet), and this script runs under `set -e`, so a
# bare psql here would take the whole run down with no message -- which is exactly
# what it did the first time.  Report and let the per-advance assertions describe
# the resulting state.
advance() {
	local out rc
	# `out=$(cmd); rc=$?` does NOT work under set -e: the assignment itself is the
	# failing command, so the shell exits before rc=$? runs.  Putting the
	# assignment in an `if` condition is what makes the failure catchable.
	if out=$(psql -p "$PORT_BASE" -d postgres -Atc "select gg_dr_switch('$1');" 2>&1); then rc=0; else rc=$?; fi
	if [ "$rc" != 0 ] || [ "$out" != t ]; then
		log "maint-dr: gg_dr_switch('$1') -> rc=$rc out='$out' (retrying once)"
		sleep 10
		out=$(psql -p "$PORT_BASE" -d postgres -Atc "select gg_dr_switch('$1');" 2>&1) || true
		log "maint-dr: gg_dr_switch('$1') retry -> '$out'"
	fi
	return 0
}

# Every node's postmaster PID, so a crash-and-restart anywhere is visible.
pids() {
	local content out=
	for content in $ALL_CONTENTS; do
		out="$out $content:$(head -1 "${DR_DATADIR[$content]}/postmaster.pid" 2>/dev/null || echo gone)"
	done
	echo "$out"
}

pass=0; fail=0
ok_() { log "maint-dr: PASS  $1"; pass=$((pass+1)); }
no_() { log "maint-dr: FAIL  $1"; fail=$((fail+1)); }

log "maint-dr: waiting for all ${#NODES[@]} nodes to pause at maint_rp_pre ..."
for _ in $(seq 1 120); do all_paused && break; sleep 2; done
all_paused || die "nodes did not reach the maint_rp_pre pause"

echo "================ V-13' PRECONDITION (replica built, before any maintenance) ================"
# This fixture only means anything under the file provider: with the topology in
# the catalog, production's VACUUM FULL would rewrite the very relation the
# replica reads, and the question would be a different one.
BAD=
for content in $ALL_CONTENTS; do
	src=$(PGOPTIONS='-c gp_role=utility' psql -p "${DR_PORT[$content]}" -d postgres -Atc "show gg_topology_source;" 2>/dev/null)
	[ "$src" = file ] || BAD="$BAD content=$content:'${src:-<unreachable>}'"
done
if [ -z "$BAD" ]; then
	ok_ "every node runs the file topology provider"
else
	no_ "nodes not on the file provider:$BAD"
	die "V-13' is only meaningful against a file-mode replica; aborting"
fi

BASE_PIDS=$(pids)
BASE_HOST=$(q "select hostname from gp_segment_configuration where content = 0;")
BASE_NODES=$(q "select count(*) from gp_segment_configuration;")
BASE_READ=$(dsp "select count(*) from maint_marker;")
BASE_CATRFN=$(q "select pg_relation_filenode('gp_segment_configuration_internal'::regclass);")
log "maint-dr:   topology: seg0 hostname '$BASE_HOST', $BASE_NODES node(s); distributed read = $BASE_READ"
log "maint-dr:   replica's catalog relfilenode: $BASE_CATRFN (production's is $RFN_PRE)"
[ "$BASE_HOST" = dr ]  && ok_ "topology is DR-local before any maintenance" \
					   || no_ "seg0 hostname is '$BASE_HOST' (expected 'dr')"
[ "$BASE_READ" = 100 ] && ok_ "a distributed read works before any maintenance (100 rows)" \
					   || no_ "distributed read returned '$BASE_READ' (expected 100)"
echo "==========================================================================================="

touch "$ARCHIVE/maint_dr_built"
log "maint-dr: signalled the primary; waiting for it to run the four operations ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/maint_ops_done" ] && break; sleep 2; done
[ -f "$ARCHIVE/maint_ops_done" ] || die "timed out waiting for production's maintenance"
source "$ARCHIVE/maint_prod_state.env"

# check_advance <label> <restore point> <expected production relfilenode var>
#
# Advance the replica across one operation and assert it did not notice.  The
# relfilenode check is what stops this passing on a fixture that did nothing: for
# the three rewriting operations, production's relfilenode must have moved, and
# the replica must have followed it (it replays the relmap update like any other
# record now).
check_advance() {
	local label=$1 rp=$2 prod_rfn=$3 want_rewrite=$4 which=${5:-heap}
	local after host nodes nread fatals guards catrfn

	advance "$rp"
	for _ in $(seq 1 180); do all_paused && break; sleep 2; done
	all_paused || no_ "$label: nodes did not all pause at $rp"

	# Against the ORIGINAL pids, not the previous advance's: the invariant is that
	# no node ever restarted, and comparing pairwise would let a restart followed
	# by a stable pid look clean.
	after=$(pids)
	[ "$after" = "$BASE_PIDS" ] && ok_ "$label: every postmaster still has its original PID" \
								|| no_ "$label: a postmaster restarted ($BASE_PIDS -> $after)"

	host=$(q "select hostname from gp_segment_configuration where content = 0;")
	[ "$host" = dr ] && ok_ "$label: seg0 hostname still 'dr'" \
					 || no_ "$label: seg0 hostname is now '$host'"

	nodes=$(q "select count(*) from gp_segment_configuration;")
	[ "$nodes" = "$BASE_NODES" ] && ok_ "$label: still $nodes node(s) in the topology" \
								 || no_ "$label: node count changed $BASE_NODES -> '$nodes'"

	nread=$(dsp "select count(*) from maint_marker;")
	[ "$nread" = 100 ] && ok_ "$label: a distributed read still dispatches (100 rows)" \
					   || no_ "$label: distributed read returned '$nread' (expected 100)"

	# Exclude the one benign FATAL this fixture provokes itself: the connection
	# poll above hits the coordinator while it is still starting recovery, and each
	# attempt logs FATAL 57P03 "the database system is starting up".  A bare
	# FATAL sweep is guaranteed to false-positive here.
	fatals=$(cat "$COORD"/log/*.csv 2>/dev/null \
		| grep -E '"(FATAL|PANIC)"' \
		| grep -vc "the database system is starting up" || true)
	[ "${fatals:-0}" -eq 0 ] && ok_ "$label: no unexpected FATAL or PANIC in the coordinator log" \
							 || no_ "$label: $fatals FATAL/PANIC line(s) in the coordinator log"

	# The inverted grep.  This line came from the smgr_redo truncate guard; it is
	# deleted, so it must never appear.  If it does, the build under test still
	# carries the guard and the run says nothing about the new design.
	guards=$(cat "$COORD"/log/*.csv 2>/dev/null | grep -c "skipped truncation of protected topology catalog" || true)
	[ "${guards:-0}" -eq 0 ] && ok_ "$label: the old truncate guard did not fire (0 log lines)" \
							 || no_ "$label: the old guard fired $guards time(s) -- this build still has it"

	# The check that stops this passing on a fixture that did nothing: for the
	# rewriting operations, production's relfilenode must have moved AND the
	# replica must have followed it -- it replays the relmap update like any other
	# record now, where the old build halted on it.  REINDEX rewrites the index,
	# not the heap, so the relation to look at differs per operation.
	if [ "$want_rewrite" = yes ]; then
		# pg_relation_filenode() resolves the relmap; pg_class.relfilenode is 0 for
		# these mapped shared catalogs.
		if [ "$which" = index ]; then
			catrfn=$(q "select pg_relation_filenode('gp_segment_config_dbid_index'::regclass);")
		else
			catrfn=$(q "select pg_relation_filenode('gp_segment_configuration_internal'::regclass);")
		fi
		if [ -z "$prod_rfn" ]; then
			no_ "$label: production published no $which relfilenode for this operation"
		elif [ "$catrfn" != "$prod_rfn" ]; then
			no_ "$label: replica $which relfilenode is $catrfn, production's is $prod_rfn"
		else
			ok_ "$label: the replica followed production's $which rewrite (relfilenode $catrfn)"
		fi
	fi
}

echo "================ V-13' : production maintenance, one operation at a time ================"
check_advance "VACUUM"      maint_rp_vacuum     ""                     no
check_advance "VACUUM FULL" maint_rp_vacuumfull "${RFN_VACUUMFULL:-}"  yes heap
check_advance "REINDEX"     maint_rp_reindex    "${IRFN_REINDEX:-}"    yes index
check_advance "TRUNCATE"    maint_rp_truncate   "${RFN_TRUNCATE:-}"    yes heap
echo "========================================================================================"

# --- promotion: the replica becomes a cluster of its own ----------------------
echo "================ V-13' : promote the maintained replica ================"
set +e
$GG promote --yes 2>&1 | sed 's/^/    ggdr: /'
prom_rc=${PIPESTATUS[0]}
set -e
[ "$prom_rc" = 0 ] && ok_ "ggdr promote succeeded after four production rewrites" \
				   || no_ "ggdr promote exited $prom_rc"

for _ in $(seq 1 60); do [ "$(q "select pg_is_in_recovery();")" = f ] && break; sleep 2; done
[ "$(q "select pg_is_in_recovery();")" = f ] && ok_ "coordinator is out of recovery" \
											 || no_ "coordinator is still in recovery"

# FTS owns the store from here, and the topology it maintains must still be the
# replica's own.
PROM_HOST=$(dsp "select hostname from gp_segment_configuration where content = 0;")
[ "$PROM_HOST" = dr ] && ok_ "promoted cluster still serves its own topology (seg0 'dr')" \
					  || no_ "promoted cluster serves seg0 hostname '$PROM_HOST'"

PROM_WRITE=$(dsp "insert into maint_marker values (999,'written after promotion'); select count(*) from maint_marker;")
[ "$PROM_WRITE" = 101 ] && ok_ "the promoted cluster accepts a distributed write" \
					    || no_ "distributed write after promotion gave '$PROM_WRITE' (expected 101)"

# ggdr promote replaces production's replayed history with one row of its own.
HIST=$(dsp "select count(*) from gp_configuration_history;")
HIST_TXT=$(dsp "select description from gp_configuration_history limit 1;")
if [ "$HIST" = 1 ] && [ "${HIST_TXT#DR promoted at restore point}" != "$HIST_TXT" ]; then
	ok_ "gp_configuration_history holds exactly the fresh promotion row: '$HIST_TXT'"
else
	no_ "gp_configuration_history has $HIST row(s), first = '$HIST_TXT'"
fi
echo "======================================================================="

if [ "$fail" -eq 0 ]; then
	log "================ V-13' VERDICT: PRODUCTION MAINTENANCE SURVIVED ================"
else
	log "================ V-13' VERDICT: FAILED ($fail of $((pass+fail)) checks) ================"
fi
log "maint-dr: $pass passed, $fail failed"
log "maint-dr: container left running for inspection"
exec sleep infinity
