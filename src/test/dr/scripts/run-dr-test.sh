#!/bin/bash
# run-dr-test.sh -- the M1 assertion: production changed gp_segment_configuration
# (seg0 port +1000) AFTER the base backup; the DR replica must NOT pick that up,
# because the apply-time redo filter (gp_dr_replica) skips production's WAL for
# the protected topology catalogs.  If frozen-seeded, the DR coordinator's
# hostname should also be the DR-local 'dr'.
set -uo pipefail
source /dr/scripts/lib.sh
source "$GPHOME/greengage_path.sh" 2>/dev/null || true
source "$ARCHIVE/primary_expected.env"   # PRODUCTION_SEG0_PORT

q() { PGOPTIONS='-c gp_role=utility' psql -p "$PORT_BASE" -d postgres -Atc "$1" 2>/dev/null; }

log "dr-test: waiting for the promoted DR coordinator to accept connections ..."
for _ in $(seq 1 60); do q "select 1" >/dev/null && break; sleep 2; done
q "select 1" >/dev/null || die "DR coordinator never accepted connections (see startup.log)"

echo "================ DR coordinator gp_segment_configuration ================"
q "select dbid,content,role,port,hostname from gp_segment_configuration order by content;"
echo "========================================================================="

dr_seg0_port=$(q "select port from gp_segment_configuration where content=0;")
dr_coord_host=$(q "select hostname from gp_segment_configuration where content=-1;")

log "dr-test: production changed seg0 port to : $PRODUCTION_SEG0_PORT"
log "dr-test: DR coordinator sees seg0 port   : ${dr_seg0_port:-<none>}"
log "dr-test: DR coordinator hostname         : ${dr_coord_host:-<none>}"

fail=0
if [ -z "$dr_seg0_port" ]; then
	log "dr-test: FAIL -- could not read DR gp_segment_configuration"
	fail=1
elif [ "$dr_seg0_port" = "$PRODUCTION_SEG0_PORT" ]; then
	log "dr-test: FAIL -- DR applied production's seg0 port change; the redo filter did NOT work"
	fail=1
else
	log "dr-test: PASS -- DR did NOT apply production's seg0 port change (redo filter works)"
fi

if [ "${DR_SEED:-1}" = 1 ]; then
	[ "$dr_coord_host" = dr ] \
		&& log "dr-test: PASS -- DR coordinator hostname is DR-local ('dr'); frozen seed survived replay" \
		|| log "dr-test: NOTE -- DR coordinator hostname is '${dr_coord_host:-<none>}', expected 'dr' (frozen seed not reflected)"
fi

if [ "$fail" = 0 ]; then
	log "================ DR FILTER TEST: PASS ================"
else
	log "================ DR FILTER TEST: FAIL ================"
fi
log "dr-test: containers stay up for inspection (e.g. docker compose exec dr bash)"
exec sleep infinity
