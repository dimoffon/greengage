#!/bin/bash
# run-dr-test.sh -- the M1 assertion: production changed its topology (seg0 port
# +1000) AFTER the base backup; the DR replica must NOT pick that up, because the
# replica's topology lives in its own $PGDATA/gg_topology and production's WAL
# carries only the catalog.  The DR coordinator's hostname should likewise be the
# DR-local 'dr' that create wrote.
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
	log "dr-test: FAIL -- DR serves production's seg0 port; its topology is not its own"
	fail=1
else
	log "dr-test: PASS -- DR did NOT pick up production's seg0 port change"
fi

[ "$dr_coord_host" = dr ] \
	&& log "dr-test: PASS -- DR coordinator hostname is DR-local ('dr'), from its own topology store" \
	|| log "dr-test: NOTE -- DR coordinator hostname is '${dr_coord_host:-<none>}', expected 'dr'"

if [ "$fail" = 0 ]; then
	log "================ DR TOPOLOGY TEST: PASS ================"
else
	log "================ DR TOPOLOGY TEST: FAIL ================"
fi
log "dr-test: containers stay up for inspection (e.g. docker compose exec dr bash)"
exec sleep infinity
