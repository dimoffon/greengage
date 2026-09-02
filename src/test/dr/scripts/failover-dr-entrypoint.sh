#!/bin/bash
# failover-dr-entrypoint.sh -- DR side of the mirror-failover fixture.
#
# Production loses the primary of one content and FTS promotes its mirror (see
# failover-primary-entrypoint.sh).  The promoted node starts timeline 2, so from
# the archive's point of view that content's WAL forks: fo_rp_b and the rows
# before it exist only on the new timeline.
#
# The replica is stopped at fo_rp_a when the failover happens, so the advance
# fo_rp_a -> fo_rp_b is the one that has to cross the fork.  This script asserts:
#
#   * the advance completes at all       -- a replica that will not follow the
#                                           switch never reaches fo_rp_b, and it
#                                           fails by WAITING, not by erroring:
#                                           the switch is wrapped in a timeout so
#                                           that shows up as a failed check
#                                           rather than a hung container
#   * the forked node followed it        -- it restored 00000002.history and its
#                                           log says "new target timeline is 2"
#   * the OTHER nodes did not            -- the fork is one content's, not the
#                                           cluster's
#   * the post-failover rows are there   -- and specifically that the forked
#                                           content's share of them grew, so the
#                                           rows read are the ones that only ever
#                                           existed on the new timeline
#   * nothing restarted, nothing FATALed
#   * the served topology never moved    -- while the replica's CATALOG does
#                                           carry production's failover, which is
#                                           the P6/P7 property at its sharpest:
#                                           FTS rewrote the topology catalog and
#                                           the replica replayed that rewrite
#                                           without it changing what it serves
#
# and finishes by promoting the replica, because a cluster that crossed a fork
# during recovery still has to be promotable afterwards.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin

# `set -e` exits silently, which in a fixture is indistinguishable from a hang.
# `set -E` is required, not decoration: without errtrace an ERR trap is NOT
# inherited by shell functions.
set -E
trap 'rc=$?; echo "fo-dr: ABORT line $LINENO: [$BASH_COMMAND] exited $rc" >&2; exit $rc' ERR

# /archive outlives `down`, so only a marker from THIS run will do.
DR_START=$(date +%s)
log "fo-dr: waiting for primary to publish base backups ..."
marker_is_fresh() {
	[ -f "$READY_MARKER" ] || return 1
	[ "$(stat -c %Y "$READY_MARKER" 2>/dev/null || echo 0)" -ge "$DR_START" ]
}
warned=
for _ in $(seq 1 900); do
	marker_is_fresh && break
	if [ -z "$warned" ] && [ -f "$READY_MARKER" ]; then
		warned=1
		log "fo-dr: NOTE $READY_MARKER predates this container -- ignoring it and waiting"
		log "fo-dr:      for one from this run.  Restart both, or 'down -v' and up."
	fi
	sleep 2
done
marker_is_fresh || die "no $READY_MARKER from this run; restart both containers, or 'docker-compose down -v' first"
source "$ARCHIVE/failover_prod_state.env"
log "fo-dr: production will fail content $FO_CONTENT over from dbid $FO_OLD_DBID to dbid $FO_NEW_DBID"

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
log "fo-dr: building the replica (paused at fo_rp_pre, before the failover) ..."
set +e
$GG create \
	--topology "$TOPO_FILE" \
	--backup "$BASEBACKUP" \
	--wal "$WAL_ARCHIVE" \
	--pause-at fo_rp_pre \
	--force 2>&1 | sed 's/^/    ggdr: /'
set -e

# Both swallow failure on purpose: this runs under `set -e`, and a query that
# errors is a thing an assertion should REPORT, not a reason to die silently.
q()   { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }
qn()  { PGOPTIONS='-c gp_role=utility' psql -p "$1" -d postgres -Atc "$2" 2>/dev/null || true; }
dsp() { psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null || true; }

log "fo-dr: waiting for the coordinator to accept read connections ..."
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
# Wrapped in `timeout`, which is the difference between this fixture and the
# others.  gg_dr_switch() blocks until every node is paused at the target and has
# no timeout of its own -- correct for an operator, useless here: the failure
# this fixture exists to catch IS a node that never arrives, and without a bound
# it would present as a container that hangs with no output.  Returns non-zero
# when the target was not reached, and the caller asserts on that.
ADVANCE_TIMEOUT=${ADVANCE_TIMEOUT:-420}
advance() {
	local out rc
	if out=$(timeout "$ADVANCE_TIMEOUT" psql -p "$PORT_BASE" -d postgres -Atc \
			"call gg_dr_switch('$1');" 2>&1); then rc=0; else rc=$?; fi
	if [ "$rc" = 124 ]; then
		log "fo-dr: gg_dr_switch('$1') did not return within ${ADVANCE_TIMEOUT}s"
		return 1
	fi
	if [ "$rc" != 0 ] || echo "$out" | grep -qi "error"; then
		log "fo-dr: gg_dr_switch('$1') -> rc=$rc out='$out' (retrying once)"
		if out=$(timeout "$ADVANCE_TIMEOUT" psql -p "$PORT_BASE" -d postgres -Atc \
				"call gg_dr_switch('$1');" 2>&1); then rc=0; else rc=$?; fi
		log "fo-dr: gg_dr_switch('$1') retry -> rc=$rc out='$out'"
		[ "$rc" = 0 ] && ! echo "$out" | grep -qi "error" || return 1
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

# Did this node follow production onto timeline $1?  Two independent signals, so
# a pass cannot come from one of them being read wrongly:
#
#   * the timeline history file, restored from the archive under its real name.
#     (WAL segments are not evidence: archive recovery restores each one to
#     pg_wal/RECOVERYXLOG and reuses that name, so no 000000020000... ever
#     appears.  History files are the exception and keep their name.)
#   * the startup process saying so.
followed_timeline() {
	local dd=$1 tli=$2 hist=0 said=0
	[ -f "$dd/pg_wal/0000000$tli.history" ] && hist=1
	grep -qs "new target timeline is $tli" "$dd"/log/*.csv && said=1
	echo "$hist$said"
}

pass=0; fail=0
ok_() { log "fo-dr: PASS  $1"; pass=$((pass+1)); }
no_() { log "fo-dr: FAIL  $1"; fail=$((fail+1)); }

log "fo-dr: waiting for all ${#NODES[@]} nodes to pause at fo_rp_pre ..."
for _ in $(seq 1 120); do all_paused && break; sleep 2; done
all_paused || die "nodes did not reach the fo_rp_pre pause"

echo "================ PRECONDITION (replica built, production still whole) ================"
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
BASE_HOST=$(q "select hostname from gp_segment_configuration where content = 0;")
BASE_NODES=$(q "select count(*) from gp_segment_configuration;")
BASE_READ=$(dsp "select count(*) from fo_marker;")
[ "$BASE_HOST" = dr ] && ok_ "topology is DR-local before the failover" \
					  || no_ "seg0 hostname is '$BASE_HOST' (expected 'dr')"
[ "$BASE_READ" = "$ROWS_PRE" ] && ok_ "a distributed read works before the failover ($ROWS_PRE rows)" \
							   || no_ "distributed read returned '$BASE_READ' (expected $ROWS_PRE)"

# Nobody may be on a forked timeline yet; without this a later "it followed the
# fork" could be describing a history file that was there all along.
PRE_FORKED=
for content in $ALL_CONTENTS; do
	[ -f "${DR_DATADIR[$content]}/pg_wal/00000002.history" ] && PRE_FORKED="$PRE_FORKED $content"
done
[ -z "$PRE_FORKED" ] && ok_ "no node has a forked timeline before production fails over" \
					 || no_ "node(s)$PRE_FORKED already carry 00000002.history"
echo "======================================================================================"

touch "$ARCHIVE/failover_dr_built"
log "fo-dr: signalled the primary; waiting for fo_rp_a ..."

# --- advance 1: still on the original timeline --------------------------------
echo "================ ADVANCE 1: fo_rp_a (production still whole) ================"
if advance fo_rp_a; then
	ok_ "advanced to fo_rp_a"
else
	no_ "could not advance to fo_rp_a"
fi
for _ in $(seq 1 180); do all_paused && break; sleep 2; done
all_paused || no_ "nodes did not all pause at fo_rp_a"

log "fo-dr: waiting for production to fail over ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/failover_ops_done" ] && break; sleep 2; done
[ -f "$ARCHIVE/failover_ops_done" ] || die "timed out waiting for production's failover"
source "$ARCHIVE/failover_prod_state.env"
log "fo-dr: production promoted dbid $FO_NEW_DBID; its content $FO_CONTENT is on timeline $FO_NEW_TLI"

A_TOTAL=$(dsp "select count(*) from fo_after;")
A_SEG=$(dsp "select count(*) from fo_after where gp_segment_id = $FO_CONTENT;")
[ "$A_TOTAL" = "$TOTAL_A" ] && ok_ "fo_rp_a serves the pre-failover rows ($TOTAL_A)" \
							|| no_ "fo_rp_a served '$A_TOTAL' rows (expected $TOTAL_A)"
[ "$A_SEG" = "$SEG_A" ] && ok_ "content $FO_CONTENT holds $SEG_A of them" \
					   || no_ "content $FO_CONTENT holds '$A_SEG' (expected $SEG_A)"
echo "============================================================================="

# --- advance 2: across the fork -----------------------------------------------
echo "================ ADVANCE 2: fo_rp_b (ACROSS THE TIMELINE FORK) ================"
if advance fo_rp_b; then
	ok_ "advanced across the fork to fo_rp_b"
else
	no_ "could not advance to fo_rp_b -- the replica did not follow production's new timeline"
fi
for _ in $(seq 1 180); do all_paused && break; sleep 2; done
all_paused && ok_ "every node is paused at a consistent point again" \
			|| no_ "nodes did not all pause at fo_rp_b"

FORKED_DD=${DR_DATADIR[$FO_CONTENT]}
sig=$(followed_timeline "$FORKED_DD" "$FO_NEW_TLI")
case $sig in
	11) ok_ "content $FO_CONTENT followed production onto timeline $FO_NEW_TLI (history file + log line)" ;;
	10) no_ "content $FO_CONTENT restored 0000000$FO_NEW_TLI.history but never logged the switch" ;;
	01) no_ "content $FO_CONTENT logged the switch but has no 0000000$FO_NEW_TLI.history" ;;
	*)  no_ "content $FO_CONTENT shows no sign of following timeline $FO_NEW_TLI" ;;
esac

OTHERS=
for content in $ALL_CONTENTS; do
	[ "$content" = "$FO_CONTENT" ] && continue
	[ "$(followed_timeline "${DR_DATADIR[$content]}" "$FO_NEW_TLI")" != 00 ] && OTHERS="$OTHERS $content"
done
[ -z "$OTHERS" ] && ok_ "the other nodes stayed on their own timeline (the fork was one content's)" \
				 || no_ "node(s)$OTHERS also switched timelines, which production never asked for"

AFTER_PIDS=$(pids)
[ "$AFTER_PIDS" = "$BASE_PIDS" ] && ok_ "every postmaster still has its original PID" \
								 || no_ "a postmaster restarted ($BASE_PIDS -> $AFTER_PIDS)"

B_TOTAL=$(dsp "select count(*) from fo_after;")
B_SEG=$(dsp "select count(*) from fo_after where gp_segment_id = $FO_CONTENT;")
[ "$B_TOTAL" = "$TOTAL_B" ] && ok_ "fo_rp_b serves the post-failover rows ($TOTAL_B)" \
							|| no_ "fo_rp_b served '$B_TOTAL' rows (expected $TOTAL_B)"
# The one that can only be answered from the new timeline: these rows were
# written through the promoted mirror and exist nowhere on timeline 1.
if [ "$B_SEG" = "$SEG_B" ]; then
	ok_ "content $FO_CONTENT serves $SEG_B rows -- $((SEG_B-SEG_A)) of them written through the promoted mirror"
else
	no_ "content $FO_CONTENT served '$B_SEG' rows (expected $SEG_B)"
fi

# The topology halves, asserted together: production's failover reached the
# replica's catalog, and the topology it serves did not move.
HOST=$(q "select hostname from gp_segment_configuration where content = 0;")
NODES_NOW=$(q "select count(*) from gp_segment_configuration;")
[ "$HOST" = dr ] && ok_ "the served topology is still DR-local (seg0 'dr')" \
				 || no_ "seg0 hostname is now '$HOST'"
[ "$NODES_NOW" = "$BASE_NODES" ] && ok_ "still $NODES_NOW node(s) in the served topology" \
								 || no_ "node count changed $BASE_NODES -> '$NODES_NOW'"
REPLAYED=$(q "select count(*) from gp_segment_configuration_internal
			   where content = $FO_CONTENT and dbid = $FO_NEW_DBID and role = 'p';")
[ "$REPLAYED" = 1 ] && ok_ "the replica's CATALOG carries production's failover (dbid $FO_NEW_DBID is primary there)" \
					|| no_ "the replayed catalog does not show dbid $FO_NEW_DBID as primary for content $FO_CONTENT"

# Exclude the one benign FATAL the fixture provokes itself: the connection poll
# above hits each node while it is still starting recovery, and every attempt
# logs FATAL 57P03 "the database system is starting up".
FATALS=
for content in $ALL_CONTENTS; do
	n=$(cat "${DR_DATADIR[$content]}"/log/*.csv 2>/dev/null \
		| grep -E '"(FATAL|PANIC)"' \
		| grep -vc "the database system is starting up" || true)
	[ "${n:-0}" -eq 0 ] || FATALS="$FATALS content=$content:$n"
done
[ -z "$FATALS" ] && ok_ "no unexpected FATAL or PANIC on any node" \
				 || no_ "FATAL/PANIC lines found:$FATALS"
echo "==============================================================================="

# --- promotion ----------------------------------------------------------------
echo "================ PROMOTE the replica that crossed a fork ================"
set +e
$GG promote --yes 2>&1 | sed 's/^/    ggdr: /'
prom_rc=${PIPESTATUS[0]}
set -e
[ "$prom_rc" = 0 ] && ok_ "ggdr promote succeeded after following a timeline switch" \
				   || no_ "ggdr promote exited $prom_rc"

for _ in $(seq 1 60); do [ "$(q "select pg_is_in_recovery();")" = f ] && break; sleep 2; done
[ "$(q "select pg_is_in_recovery();")" = f ] && ok_ "coordinator is out of recovery" \
											 || no_ "coordinator is still in recovery"

PROM_HOST=$(dsp "select hostname from gp_segment_configuration where content = 0;")
[ "$PROM_HOST" = dr ] && ok_ "promoted cluster still serves its own topology (seg0 'dr')" \
					  || no_ "promoted cluster serves seg0 hostname '$PROM_HOST'"

# Timed out, unlike the other reads.  Production runs with mirrors, so its
# postgresql.auto.conf carries synchronous_standby_names = '*' and the base
# backup brings it along; ggdr create truncates that file, and if it ever stopped
# doing so this write would not fail, it would wait forever for a standby the
# replica does not have.
PROM_WRITE=$(timeout 120 psql -p "$PORT_BASE" -d postgres -Atc \
	"insert into fo_after values (999999,'written after promotion'); select count(*) from fo_after;" 2>/dev/null || true)
[ "$PROM_WRITE" = "$((TOTAL_B+1))" ] && ok_ "the promoted cluster accepts a distributed write" \
									 || no_ "distributed write after promotion gave '$PROM_WRITE' (expected $((TOTAL_B+1)))"

# The promoted replica must not have written into production's archive.  It has
# production's archive_command in its postgresql.conf -- the base backup brought
# it -- and the moment it leaves recovery it is a cluster that archives.  The
# damage is not the disk space: a replica follows the NEWEST timeline in the
# archive, so a history file left here by this promotion is the one the next
# replica built from this archive would choose, and it would recover onto this
# replica's timeline instead of production's.
#
# Checked on history files, which is where it shows up first and unambiguously:
# any timeline above the one production reached was invented on this side.
# -printf '%f', the basename: %P keeps the seg<content>/ prefix and the hex
# conversion below then chokes on it.  And the test lives in an `if`, not after
# an `&&`: as the loop's last command a false `&&` makes the loop exit 1, which
# under `set -e` aborts the run -- on the passing case, where the newest history
# file is production's.
STRAY=
for h in $(find "$WAL_ARCHIVE" -name '*.history' -printf '%f\n' 2>/dev/null | sort -u); do
	t=$((16#${h%.history}))
	if [ "$t" -gt "$FO_NEW_TLI" ]; then STRAY="$STRAY $h"; fi
done
[ -z "$STRAY" ] && ok_ "the promoted replica left production's archive alone (no timeline above $FO_NEW_TLI)" \
				|| no_ "the promoted replica archived into production's archive: $(echo $STRAY)"
echo "========================================================================="

if [ "$fail" -eq 0 ]; then
	log "================ FAILOVER VERDICT: THE REPLICA FOLLOWED PRODUCTION ================"
else
	log "================ FAILOVER VERDICT: FAILED ($fail of $((pass+fail)) checks) ================"
fi
log "fo-dr: $pass passed, $fail failed"
log "fo-dr: container left running for inspection"
exec sleep infinity
