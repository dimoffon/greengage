#!/bin/bash
# dense-dr-entrypoint.sh -- DR side of the V-20 regression fixture.
#
# Builds the replica from production's DENSE base backup (see
# dense-primary-entrypoint.sh), pausing at dense_rp_pre -- before production's
# VACUUM truncates the topology catalog.  Then:
#
#   PRECONDITION -- assert the DR is actually exposed: its live topology rows must
#     sit on a block at or above the block count production will truncate down to.
#     If they landed on block 0 the fixture is not dense enough and the run proves
#     nothing, so that is a hard failure, not a pass.
#
#   OUTCOME -- advance past production's truncation and report what happened to
#     the DR's topology.  With the smgr_redo guard the rows survive and the
#     coordinator logs a skip; without it they are discarded with the truncated
#     blocks.
#
# The script reports the observed behaviour either way; it is meant to be run
# against a guarded and an unguarded build to compare.
set -euo pipefail
source /dr/scripts/lib.sh
ensure_gpadmin

log "dense-dr: waiting for primary to publish the dense base backups ..."
for _ in $(seq 1 900); do [ -f "$READY_MARKER" ] && break; sleep 2; done
[ -f "$READY_MARKER" ] || die "timed out waiting for $READY_MARKER"
source "$ARCHIVE/dense_prod_state.env"
log "dense-dr: production catalog was densified to $PROD_PAGES_DENSE page(s) with $FILLER_ROWS filler rows"

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
log "dense-dr: building the replica (paused at dense_rp_pre, before the truncation) ..."
set +e
$GG create-replica \
	--topology "$TOPO_FILE" \
	--basebackup-dir "$BASEBACKUP" \
	--wal-archive "$WAL_ARCHIVE" \
	--pause-at dense_rp_pre \
	--force 2>&1 | sed 's/^/    ggdr: /'
set -e

# Utility-mode helper: the checks must keep working even if the topology catalog
# is emptied, which is precisely the failure this test looks for -- utility mode
# needs no segment array, dispatch mode would not survive it.
q() { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null; }

log "dense-dr: waiting for the coordinator to accept read connections ..."
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
advance() {   # $1=to -- move the whole cluster to a new restore point AND serve it
	# gg_dr_switch(), not a per-node rearm+resume: the step that makes a node start
	# serving the new point may only run once every node has arrived, so only the
	# coordinator can drive it (ADR-0005).
	psql -p "$PORT_BASE" -d postgres -Atc "select gg_dr_switch('$1');" >/dev/null 2>&1
}

BLKSZ=$(q "select current_setting('block_size')::int;")
dr_pages()   { q "select pg_relation_size('gp_segment_configuration') / $BLKSZ;"; }
dr_rows()    { q "select count(*) from gp_segment_configuration;"; }
dr_max_blk() { q "select coalesce(max(substring(ctid::text from '\((\d+),')::int), -1) from gp_segment_configuration;"; }

log "dense-dr: waiting for all ${#NODES[@]} nodes to pause at dense_rp_pre ..."
for _ in $(seq 1 120); do all_paused && break; sleep 2; done
all_paused || die "nodes did not reach the dense_rp_pre pause"

pass=0; fail=0
ok_() { log "dense-dr: PASS  $1"; pass=$((pass+1)); }
no_() { log "dense-dr: FAIL  $1"; fail=$((fail+1)); }

echo "================ V-20 PRECONDITION (DR built from a dense catalog, pre-truncate) ================"
PRE_ROWS=$(dr_rows); PRE_PAGES=$(dr_pages); PRE_MAX_BLK=$(dr_max_blk)
DR_HOST=$(q "select hostname from gp_segment_configuration where content = 0;")
log "dense-dr:   DR catalog: $PRE_ROWS live row(s), $PRE_PAGES page(s), highest live block $PRE_MAX_BLK"
log "dense-dr:   production:  was $PROD_PAGES_DENSE page(s), will truncate to ${PROD_PAGES_AFTER:-1}"

[ "$PRE_ROWS" = 3 ] && ok_ "DR carries its 3 seeded topology rows" \
					|| no_ "DR has $PRE_ROWS topology rows (expected 3)"
[ "$DR_HOST" = dr ] && ok_ "DR topology is DR-local (seg0 hostname 'dr')" \
					|| no_ "DR seg0 hostname is '$DR_HOST' (expected 'dr')"
# The whole point of the dense fixture: the seed's rows must NOT be on block 0,
# or production's truncation has nothing of ours to discard and the run is void.
if [ "$PRE_MAX_BLK" -ge 1 ]; then
	ok_ "EXPOSED as intended: DR live rows are on block $PRE_MAX_BLK, above production's truncation target"
else
	no_ "NOT EXPOSED: DR live rows are on block 0 -- the catalog was not dense enough; raise FILLER_ROWS"
	log "dense-dr: aborting: without exposure this run cannot tell a guarded build from an unguarded one"
	exec sleep infinity
fi
echo "================================================================================================="

touch "$ARCHIVE/dense_dr_built"
log "dense-dr: signalled the primary; waiting for it to VACUUM + truncate ..."
for _ in $(seq 1 900); do [ -f "$ARCHIVE/dense_truncate_done" ] && break; sleep 2; done
[ -f "$ARCHIVE/dense_truncate_done" ] || die "timed out waiting for production's truncation"
source "$ARCHIVE/dense_prod_state.env"
log "dense-dr: production truncated $PROD_PAGES_DENSE -> $PROD_PAGES_AFTER page(s); advancing the DR past it"

advance dense_rp_post
for _ in $(seq 1 180); do all_paused && break; sleep 2; done
all_paused || log "dense-dr: WARNING nodes did not all pause at dense_rp_post; reporting current state anyway"

echo "================ V-20 OUTCOME (DR replayed production's XLOG_SMGR_TRUNCATE) ================"
POST_ROWS=$(dr_rows); POST_PAGES=$(dr_pages); POST_MAX_BLK=$(dr_max_blk)
GUARD_HITS=$(cat "$COORD"/log/*.csv 2>/dev/null | grep -c "skipped truncation of protected topology catalog" || true)
log "dense-dr:   DR catalog now: ${POST_ROWS:-<unreadable>} live row(s), ${POST_PAGES:-?} page(s), highest live block ${POST_MAX_BLK:-?}"
log "dense-dr:   guard log lines in the coordinator log: $GUARD_HITS"

if [ "${POST_ROWS:-0}" = 3 ] && [ "$POST_PAGES" -gt "$PROD_PAGES_AFTER" ]; then
	ok_ "topology SURVIVED: 3 rows intact, DR still $POST_PAGES page(s) while production is $PROD_PAGES_AFTER"
	VERDICT="TOPOLOGY SURVIVED -- production's truncation was not applied to the DR"
elif [ "${POST_ROWS:-0}" = 3 ]; then
	no_ "rows intact but the DR relation shrank to $POST_PAGES page(s) -- unexpected, investigate"
	VERDICT="AMBIGUOUS -- rows present but the relation was truncated"
else
	no_ "topology DESTROYED: ${POST_ROWS:-0} live row(s) left (was $PRE_ROWS on block $PRE_MAX_BLK); DR relation $POST_PAGES page(s)"
	VERDICT="TOPOLOGY DESTROYED -- production's truncation discarded the DR's own rows"
fi
if [ "$GUARD_HITS" -gt 0 ]; then
	ok_ "smgr_redo guard fired on the real record ($GUARD_HITS log line(s))"
	cat "$COORD"/log/*.csv 2>/dev/null | grep -o "skipped truncation of protected topology catalog.*" | head -2 | sed 's/^/    guard: /'
else
	log "dense-dr: NOTE  no guard log line -- this build has no smgr_redo guard"
fi
echo "==========================================================================================="
log "================ V-20 VERDICT: $VERDICT ================"
log "================ dense fixture: $pass passed, $fail failed ================"
log "dense-dr: container left running for inspection"
exec sleep infinity
