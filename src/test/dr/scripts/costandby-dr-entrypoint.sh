#!/bin/bash
# costandby-dr-entrypoint.sh -- DR side of the coordinator-standby activation.
#
# Production loses its coordinator and an operator activates the standby (see
# costandby-primary-entrypoint.sh).  Two things arrive here as a result:
#
#   1. content -1's WAL forks.  The activated node starts timeline 2, and
#      co_rp_b -- with the table created after the activation -- exists only
#      there.  The replica has to follow it, which is the same property the
#      mirror fixture tests, on the one content that has no FTS behind it.
#
#   2. production's topology catalog loses a row.  Activation DELETES the old
#      coordinator's row and promotes the standby's
#      (segment_config_activate_standby(), segadmin.c).  This replica's own
#      coordinator sits at that same content, and under the design this feature
#      replaced it would have been reading that very catalog.
#
# So the pair of assertions at the centre of this fixture is: the replica's
# CATALOG shows production's coordinator row gone, and the topology the replica
# SERVES still has it -- same run, same instant, one from the replayed WAL and
# one from $PGDATA/gg_topology.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin

# `set -E` is required, not decoration: without errtrace an ERR trap is NOT
# inherited by shell functions, so a failure inside one exits as silently as a
# hang.
set -E
trap 'rc=$?; echo "co-dr: ABORT line $LINENO: [$BASH_COMMAND] exited $rc" >&2; exit $rc' ERR

DR_START=$(date +%s)
log "co-dr: waiting for primary to publish base backups ..."
marker_is_fresh() {
	[ -f "$READY_MARKER" ] || return 1
	[ "$(stat -c %Y "$READY_MARKER" 2>/dev/null || echo 0)" -ge "$DR_START" ]
}
warned=
for _ in $(seq 1 900); do
	marker_is_fresh && break
	if [ -z "$warned" ] && [ -f "$READY_MARKER" ]; then
		warned=1
		log "co-dr: NOTE $READY_MARKER predates this container -- ignoring it and waiting"
		log "co-dr:      for one from this run.  Restart both, or 'down -v' and up."
	fi
	sleep 2
done
marker_is_fresh || die "no $READY_MARKER from this run; restart both containers, or 'docker-compose down -v' first"
source "$ARCHIVE/costandby_prod_state.env"
log "co-dr: production will activate its standby coordinator: dbid $CO_OLD_DBID -> dbid $CO_NEW_DBID"

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
log "co-dr: building the replica (paused at co_rp_pre, before the activation) ..."
set +e
$GG create \
	--topology "$TOPO_FILE" \
	--backup "$BASEBACKUP" \
	--wal "$WAL_ARCHIVE" \
	--pause-at co_rp_pre \
	--force 2>&1 | sed 's/^/    ggdr: /'
set -e

q()   { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }
qn()  { PGOPTIONS='-c gp_role=utility' psql -p "$1" -d postgres -Atc "$2" 2>/dev/null || true; }
dsp() { psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }

log "co-dr: waiting for the coordinator to accept read connections ..."
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

# Wrapped in `timeout`: gg_dr_switch() blocks until every node arrives and has no
# timeout of its own, and the failure this fixture exists to catch is a node that
# never does.  Without a bound that is a hung container, not a failed check.
ADVANCE_TIMEOUT=${ADVANCE_TIMEOUT:-420}
advance() {
	local out rc
	if out=$(timeout "$ADVANCE_TIMEOUT" psql -p "$PORT_BASE" -d postgres -Atc \
			"select gg_dr_switch('$1');" 2>&1); then rc=0; else rc=$?; fi
	if [ "$rc" = 124 ]; then
		log "co-dr: gg_dr_switch('$1') did not return within ${ADVANCE_TIMEOUT}s"
		return 1
	fi
	if [ "$rc" != 0 ] || [ "$out" != t ]; then
		log "co-dr: gg_dr_switch('$1') -> rc=$rc out='$out' (retrying once)"
		if out=$(timeout "$ADVANCE_TIMEOUT" psql -p "$PORT_BASE" -d postgres -Atc \
				"select gg_dr_switch('$1');" 2>&1); then rc=0; else rc=$?; fi
		log "co-dr: gg_dr_switch('$1') retry -> rc=$rc out='$out'"
		[ "$rc" = 0 ] && [ "$out" = t ] || return 1
	fi
	return 0
}

pids() {
	local content out=
	for content in $ALL_CONTENTS; do
		out="$out $content:$(head -1 "${DR_DATADIR[$content]}/postmaster.pid" 2>/dev/null || echo gone)"
	done
	echo "$out"
}

# Two independent signals, so a pass cannot rest on one being misread: the
# history file, restored from the archive under its real name (WAL segments are
# not evidence -- archive recovery restores each to pg_wal/RECOVERYXLOG and
# reuses that name), and the startup process saying it switched.
followed_timeline() {
	local dd=$1 tli=$2 hist=0 said=0
	[ -f "$dd/pg_wal/0000000$tli.history" ] && hist=1
	grep -qs "new target timeline is $tli" "$dd"/log/*.csv && said=1
	echo "$hist$said"
}

pass=0; fail=0
ok_() { log "co-dr: PASS  $1"; pass=$((pass+1)); }
no_() { log "co-dr: FAIL  $1"; fail=$((fail+1)); }

log "co-dr: waiting for all ${#NODES[@]} nodes to pause at co_rp_pre ..."
for _ in $(seq 1 120); do all_paused && break; sleep 2; done
all_paused || die "nodes did not reach the co_rp_pre pause"

echo "================ PRECONDITION (replica built, production's coordinator still original) ================"
BAD=
for content in $ALL_CONTENTS; do
	src=$(qn "${DR_PORT[$content]}" "show gg_topology_source;")
	[ "$src" = file ] || BAD="$BAD content=$content:'${src:-<unreachable>}'"
done
if [ -z "$BAD" ]; then
	ok_ "every node runs the file topology provider"
else
	no_ "nodes not on the file provider:$BAD"
	die "this fixture is only meaningful against a file-mode replica; aborting"
fi

BASE_PIDS=$(pids)
BASE_NODES=$(q "select count(*) from gp_segment_configuration;")
BASE_HOST=$(q "select hostname from gp_segment_configuration where content = -1;")
BASE_READ=$(dsp "select count(*) from co_marker;")
[ "$BASE_HOST" = dr ] && ok_ "the served coordinator row is DR-local before the activation" \
					  || no_ "coordinator hostname is '$BASE_HOST' (expected 'dr')"
[ "$BASE_READ" = "$ROWS_PRE" ] && ok_ "a distributed read works before the activation ($ROWS_PRE rows)" \
							   || no_ "distributed read returned '$BASE_READ' (expected $ROWS_PRE)"
# The replayed catalog still has production's original coordinator, so the
# disappearance asserted later is this fixture's doing and not a fixture that
# never had the row.
PRE_CAT=$(q "select count(*) from gp_segment_configuration_internal where dbid = $CO_OLD_DBID;")
[ "$PRE_CAT" = 1 ] && ok_ "the replayed catalog carries production's original coordinator (dbid $CO_OLD_DBID)" \
				   || no_ "the replayed catalog has $PRE_CAT row(s) for dbid $CO_OLD_DBID, expected 1"
PRE_FORKED=
for content in $ALL_CONTENTS; do
	[ -f "${DR_DATADIR[$content]}/pg_wal/00000002.history" ] && PRE_FORKED="$PRE_FORKED $content"
done
[ -z "$PRE_FORKED" ] && ok_ "no node has a forked timeline before the activation" \
					 || no_ "node(s)$PRE_FORKED already carry 00000002.history"
echo "======================================================================================================"

touch "$ARCHIVE/costandby_dr_built"
log "co-dr: signalled the primary; waiting for co_rp_a ..."

echo "================ ADVANCE 1: co_rp_a (coordinator still the original) ================"
if advance co_rp_a; then ok_ "advanced to co_rp_a"; else no_ "could not advance to co_rp_a"; fi
for _ in $(seq 1 180); do all_paused && break; sleep 2; done
all_paused || no_ "nodes did not all pause at co_rp_a"

log "co-dr: waiting for production to activate its standby ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/costandby_ops_done" ] && break; sleep 2; done
[ -f "$ARCHIVE/costandby_ops_done" ] || die "timed out waiting for production's activation"
source "$ARCHIVE/costandby_prod_state.env"
log "co-dr: production activated dbid $CO_NEW_DBID; content -1 is on timeline $CO_NEW_TLI"

A_TOTAL=$(dsp "select count(*) from co_after;")
[ "$A_TOTAL" = "$TOTAL_A" ] && ok_ "co_rp_a serves the pre-activation rows ($TOTAL_A)" \
							|| no_ "co_rp_a served '$A_TOTAL' rows (expected $TOTAL_A)"
echo "====================================================================================="

echo "================ ADVANCE 2: co_rp_b (ACROSS THE COORDINATOR'S FORK) ================"
if advance co_rp_b; then
	ok_ "advanced across the coordinator's fork to co_rp_b"
else
	no_ "could not advance to co_rp_b -- the replica did not follow the activated coordinator"
fi
for _ in $(seq 1 180); do all_paused && break; sleep 2; done
all_paused && ok_ "every node is paused at a consistent point again" \
			|| no_ "nodes did not all pause at co_rp_b"

sig=$(followed_timeline "$COORD" "$CO_NEW_TLI")
case $sig in
	11) ok_ "the coordinator followed production onto timeline $CO_NEW_TLI (history file + log line)" ;;
	10) no_ "the coordinator restored 0000000$CO_NEW_TLI.history but never logged the switch" ;;
	01) no_ "the coordinator logged the switch but has no 0000000$CO_NEW_TLI.history" ;;
	*)  no_ "the coordinator shows no sign of following timeline $CO_NEW_TLI" ;;
esac

OTHERS=
for content in $ALL_CONTENTS; do
	[ "$content" = "-1" ] && continue
	[ "$(followed_timeline "${DR_DATADIR[$content]}" "$CO_NEW_TLI")" != 00 ] && OTHERS="$OTHERS $content"
done
[ -z "$OTHERS" ] && ok_ "the segments stayed on their own timeline (only content -1 forked)" \
				 || no_ "segment(s)$OTHERS also switched timelines, which production never asked for"

AFTER_PIDS=$(pids)
[ "$AFTER_PIDS" = "$BASE_PIDS" ] && ok_ "every postmaster still has its original PID" \
								 || no_ "a postmaster restarted ($BASE_PIDS -> $AFTER_PIDS)"

# One dispatched SELECT that can only be answered past the fork: the table was
# created after the activation, so its pg_class row exists only in WAL the new
# coordinator wrote on the new timeline.  A replica still on timeline 1 does not
# know the name.
NEW_TAB=$(dsp "select count(*) from co_after_activation;")
[ "$NEW_TAB" = "$NEWTAB" ] && ok_ "a table created AFTER the activation is readable on the replica ($NEWTAB rows)" \
						   || no_ "co_after_activation returned '$NEW_TAB' (expected $NEWTAB)"
B_TOTAL=$(dsp "select count(*) from co_after;")
[ "$B_TOTAL" = "$TOTAL_B" ] && ok_ "co_rp_b serves the post-activation rows ($TOTAL_B)" \
							|| no_ "co_rp_b served '$B_TOTAL' rows (expected $TOTAL_B)"

# --- the pair this fixture exists for ----------------------------------------
# Production deleted the row describing content -1 and promoted another in its
# place.  The replica must have replayed exactly that, and must still be serving
# its own.
CAT_GONE=$(q "select count(*) from gp_segment_configuration_internal where dbid = $CO_OLD_DBID;")
CAT_NEW=$(q "select count(*) from gp_segment_configuration_internal
			  where content = -1 and role = 'p' and dbid = $CO_NEW_DBID;")
[ "$CAT_GONE" = 0 ] && ok_ "the replica's CATALOG lost production's old coordinator row (dbid $CO_OLD_DBID), as production did" \
					|| no_ "the replayed catalog still has $CAT_GONE row(s) for dbid $CO_OLD_DBID"
[ "$CAT_NEW" = 1 ] && ok_ "the replica's CATALOG shows dbid $CO_NEW_DBID as production's coordinator" \
				   || no_ "the replayed catalog does not show dbid $CO_NEW_DBID as coordinator"

SERVED_NODES=$(q "select count(*) from gp_segment_configuration;")
SERVED_DBID=$(q "select dbid from gp_segment_configuration where content = -1;")
SERVED_HOST=$(q "select hostname from gp_segment_configuration where content = -1;")
[ "$SERVED_NODES" = "$BASE_NODES" ] && ok_ "the SERVED topology still has all $SERVED_NODES node(s)" \
									 || no_ "served node count changed $BASE_NODES -> '$SERVED_NODES'"
[ "$SERVED_DBID" = "$CO_OLD_DBID" ] && ok_ "the SERVED coordinator is still dbid $CO_OLD_DBID -- the row production deleted" \
									|| no_ "the served coordinator is dbid '$SERVED_DBID', expected $CO_OLD_DBID"
[ "$SERVED_HOST" = dr ] && ok_ "and it is still DR-local ('dr')" \
						|| no_ "the served coordinator hostname is '$SERVED_HOST'"

FATALS=
for content in $ALL_CONTENTS; do
	n=$(cat "${DR_DATADIR[$content]}"/log/*.csv 2>/dev/null \
		| grep -E '"(FATAL|PANIC)"' \
		| grep -vc "the database system is starting up" || true)
	[ "${n:-0}" -eq 0 ] || FATALS="$FATALS content=$content:$n"
done
[ -z "$FATALS" ] && ok_ "no unexpected FATAL or PANIC on any node" \
				 || no_ "FATAL/PANIC lines found:$FATALS"
echo "===================================================================================="

# --- promotion ----------------------------------------------------------------
# The interesting one on this fixture.  StartupXLOG promotes a standby
# coordinator's catalog on the way out of recovery, and this replica's replayed
# catalog is now in the state where that would do the most damage: it no longer
# contains a row for this node's own dbid at content -1.  needToPromoteCatalog
# excludes a DR replica explicitly (P0) -- if it ever stopped doing so, this is
# the run that would notice.
echo "================ PROMOTE the replica that replayed an activation ================"
set +e
$GG promote --yes 2>&1 | sed 's/^/    ggdr: /'
prom_rc=${PIPESTATUS[0]}
set -e
[ "$prom_rc" = 0 ] && ok_ "ggdr promote succeeded after replaying production's standby activation" \
				   || no_ "ggdr promote exited $prom_rc"

for _ in $(seq 1 60); do [ "$(q "select pg_is_in_recovery();")" = f ] && break; sleep 2; done
[ "$(q "select pg_is_in_recovery();")" = f ] && ok_ "coordinator is out of recovery" \
											 || no_ "coordinator is still in recovery"

PROM_DBID=$(dsp "select dbid from gp_segment_configuration where content = -1;")
[ "$PROM_DBID" = "$CO_OLD_DBID" ] && ok_ "the promoted cluster still describes itself (coordinator dbid $CO_OLD_DBID)" \
								  || no_ "the promoted cluster's coordinator is dbid '$PROM_DBID'"

PROM_WRITE=$(timeout 120 psql -p "$PORT_BASE" -d postgres -Atc \
	"insert into co_after values (999999,'written after promotion'); select count(*) from co_after;" 2>/dev/null || true)
[ "$PROM_WRITE" = "$((TOTAL_B+1))" ] && ok_ "the promoted cluster accepts a distributed write" \
									 || no_ "distributed write after promotion gave '$PROM_WRITE' (expected $((TOTAL_B+1)))"

STRAY=
for h in $(find "$WAL_ARCHIVE" -name '*.history' -printf '%f\n' 2>/dev/null | sort -u); do
	t=$((16#${h%.history}))
	if [ "$t" -gt "$CO_NEW_TLI" ]; then STRAY="$STRAY $h"; fi
done
[ -z "$STRAY" ] && ok_ "the promoted replica left production's archive alone (no timeline above $CO_NEW_TLI)" \
				|| no_ "the promoted replica archived into production's archive: $(echo $STRAY)"
echo "================================================================================="

if [ "$fail" -eq 0 ]; then
	log "================ STANDBY-ACTIVATION VERDICT: THE REPLICA FOLLOWED PRODUCTION ================"
else
	log "================ STANDBY-ACTIVATION VERDICT: FAILED ($fail of $((pass+fail)) checks) ================"
fi
log "co-dr: $pass passed, $fail failed"
log "co-dr: container left running for inspection"
exec sleep infinity
