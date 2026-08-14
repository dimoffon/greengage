#!/bin/bash
#
# The file-backed cluster topology store, end to end.
#
# Driven with initdb, gg_topology, psql and a postmaster started directly,
# rather than with gpMgmt.  gg_topology_source is PGC_POSTMASTER, so it has to
# be in each node's postgresql.conf before anything starts -- gpstop -ar will
# not add it -- and starting nodes by hand is the only way to arm it in a test.
# gpMgmt itself works under any provider now that gp_segment_configuration is a
# view over the active store.
#
# Usage:  ./run.sh [BINDIR]
#
# BINDIR defaults to the directory holding pg_config on PATH.  A scratch data
# directory is created under ${TMPDIR:-/tmp} and removed on exit.
#
set -u

BINDIR=${1:-}
if [ -z "$BINDIR" ]; then
	BINDIR=$(dirname "$(command -v pg_config 2>/dev/null)" 2>/dev/null)
fi
if [ ! -x "$BINDIR/initdb" ]; then
	echo "usage: $0 BINDIR   (a Greengage bin directory containing initdb)" >&2
	exit 2
fi

INITDB=$BINDIR/initdb
POSTGRES=$BINDIR/postgres
PSQL=$BINDIR/psql
GGTOPO=$BINDIR/gg_topology
CONTROLDATA=$BINDIR/pg_controldata

TMPROOT=$(mktemp -d "${TMPDIR:-/tmp}/gg_topology_test.XXXXXX")
PORT=${PGPORT_BASE:-7810}
pass=0
fail=0

cleanup() {
	pkill -TERM -f "postgres -D $TMPROOT" >/dev/null 2>&1
	sleep 1
	pkill -KILL -f "postgres -D $TMPROOT" >/dev/null 2>&1
	rm -rf "$TMPROOT"
}
trap cleanup EXIT

ok()   { printf '%-56s PASS\n' "$1"; pass=$((pass+1)); }
no()   { printf '%-56s FAIL  %s\n' "$1" "$2"; fail=$((fail+1)); }
chk()  { if [ "$2" = "$3" ]; then ok "$1"; else no "$1" "expected [$2] got [$3]"; fi; }

# ----------------------------------------------------------------------------
# the format, through gg_topology
# ----------------------------------------------------------------------------
echo "== format =="

D=$TMPROOT/fmt
$INITDB -D "$D" -N --no-locale -E UTF8 >/dev/null 2>&1 || { echo "initdb failed"; exit 1; }

SYSID=$($CONTROLDATA "$D" | sed -n 's/^Database system identifier: *//p')

chk "initdb writes a store"            "1"      "$(test -f "$D/gg_topology" && echo 1 || echo 0)"
chk "  at generation 0"                "0"      "$($GGTOPO dump -D "$D" | awk '/^generation/{print $2}')"
chk "  with no entries"                "0"      "$($GGTOPO dump -D "$D" | awk '/^nentries/{print $2}')"
chk "  recording this cluster"         "$SYSID" "$($GGTOPO dump -D "$D" | awk '/^system_identifier/{print $2}')"

TOPO3=$(printf '1 -1 p p s u 6000 cdw cdw /d/coord\n2 0 p p s u 6001 sdw1 sdw1 /d/seg0\n3 1 p p s u 6002 sdw2 sdw2 /d/seg1\n')

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
chk "write records three entries"      "3" "$($GGTOPO dump -D "$D" | awk '/^nentries/{print $2}')"
chk "  and advances the generation"    "1" "$($GGTOPO dump -D "$D" | awk '/^generation/{print $2}')"
echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
chk "  again on the next write"        "2" "$($GGTOPO dump -D "$D" | awk '/^generation/{print $2}')"

printf '1 -1 p p s u 6000 cdw cdw /d/has a space\n' | $GGTOPO write -D "$D" >/dev/null 2>&1
chk "a space in a datadir is refused"  "1" "$?"

printf '1 -1 p p s u 6000 cdw cdw /d/c\n9 -1 m m s u 6009 sby sby /d/s\n' \
	| $GGTOPO write -D "$D" >/dev/null 2>&1
chk "a standby coordinator is refused" "1" "$?"

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
sed -i 's#/d/seg0#/d/segX#' "$D/gg_topology"
$GGTOPO dump -D "$D" >/dev/null 2>&1
chk "a flipped byte fails the checksum" "1" "$?"

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
head -n -1 "$D/gg_topology" > "$D/x" && mv "$D/x" "$D/gg_topology"
$GGTOPO dump -D "$D" >/dev/null 2>&1
chk "a missing checksum line is caught" "1" "$?"

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
$GGTOPO bootstrap -D "$D" >/dev/null 2>&1
chk "bootstrap will not reset a live store" "1" "$?"

touch "$D/postmaster.pid"
echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null 2>&1
chk "write refuses while a server owns it" "1" "$?"
rm -f "$D/postmaster.pid"

chk "no temp file is left behind"      "0" "$(ls "$D" | grep -c 'gg_topology\.tmp')"

# ----------------------------------------------------------------------------
# what the postmaster does with each state of the store
# ----------------------------------------------------------------------------
echo
echo "== startup =="

start_expect() {   # start_expect <label> <START|REFUSE> <extra args...>
	local label="$1" expect="$2"; shift 2
	PORT=$((PORT+1))
	rm -f "$D"/log/*.csv 2>/dev/null
	timeout 12 $POSTGRES -D "$D" -p $PORT -c gp_dbid=1 -c gp_contentid=-1 "$@" \
		> "$TMPROOT/startup.log" 2>&1 &
	local pid=$!
	sleep 7
	local got
	if kill -0 $pid 2>/dev/null; then
		got=START
		kill -TERM $pid 2>/dev/null
	else
		got=REFUSE
	fi
	wait $pid 2>/dev/null
	if [ "$got" = "$expect" ]; then ok "$label"; else no "$label" "expected $expect, got $got"; fi
	sleep 1
}

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
start_expect "catalog mode ignores the store"   START  -c gp_role=dispatch
start_expect "a valid store serves a dispatcher" START -c gp_role=dispatch -c gg_topology_source=file
start_expect "and a utility-mode node"           START -c gp_role=utility  -c gg_topology_source=file

$GGTOPO bootstrap -D "$D" --force >/dev/null
start_expect "generation 0 starts in utility mode"  START  -c gp_role=utility  -c gg_topology_source=file
start_expect "generation 0 refuses a dispatcher"    REFUSE -c gp_role=dispatch -c gg_topology_source=file

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
sed -i 's#/d/seg0#/d/segX#' "$D/gg_topology"
start_expect "a corrupt store refuses to start"     REFUSE -c gp_role=dispatch -c gg_topology_source=file

echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
rm -f "$D/gg_topology"
start_expect "a deleted store refuses to start"     REFUSE -c gp_role=dispatch -c gg_topology_source=file

# a standby coordinator has to be planted past gg_topology, which refuses it
echo "$TOPO3" | $GGTOPO write -D "$D" >/dev/null
sed -i 's#^1 -1 p p #1 -1 p m #' "$D/gg_topology"
python3 - "$D/gg_topology" <<'PY' 2>/dev/null || true
import sys, binascii
p = sys.argv[1]
data = open(p, 'rb').read()
body = data[:data.rindex(b'\ncrc32c ') + 1]
# recompute CRC-32C (Castagnoli) so the file is well-formed but wrong-shaped
poly, crc = 0x82F63B78, 0xFFFFFFFF
for b in body:
    crc ^= b
    for _ in range(8):
        crc = (crc >> 1) ^ (poly if crc & 1 else 0)
crc ^= 0xFFFFFFFF
open(p, 'wb').write(body + ('crc32c %08x\n' % crc).encode())
PY
start_expect "a standby coordinator refuses to start" REFUSE -c gp_role=dispatch -c gg_topology_source=file

# ----------------------------------------------------------------------------
# writing the store from the backend
# ----------------------------------------------------------------------------
echo
echo "== writes =="

W=$TMPROOT/wr
PORT=$((PORT+1))
$INITDB -D "$W" -N --no-locale -E UTF8 >/dev/null 2>&1
printf '1 -1 p p s u %d cdw cdw %s\n' "$PORT" "$W" | $GGTOPO write -D "$W" >/dev/null

$POSTGRES -D "$W" -p $PORT -c gp_dbid=1 -c gp_contentid=-1 \
	-c gp_role=utility -c gg_topology_source=file > "$TMPROOT/wr.log" 2>&1 &
WPM=$!
sleep 7

q() { PGOPTIONS="-c gp_role=utility" $PSQL -p $PORT -d postgres -Atc "$1" 2>&1; }
gen() { $GGTOPO dump -D "$W" | awk '/^generation/{print $2}'; }

chk "gp_add_segment_primary returns a dbid" "2" "$(q "select gp_add_segment_primary('sdw1','sdw1',6100,'/d/s0')")"
chk "  the store advanced"                  "2" "$(gen)"
chk "  the entry is in the store"           "1" "$($GGTOPO dump -D "$W" | grep -c '^2 0 p p n u 6100 sdw1 sdw1 /d/s0$')"
chk "  and the catalog was not touched"     "0" "$(q "select count(*) from gp_segment_configuration_internal")"
chk "  but the view shows the store"        "2" "$(q "select count(*) from gp_segment_configuration")"

chk "a second primary"                      "3" "$(q "select gp_add_segment_primary('sdw2','sdw2',6101,'/d/s1')")"
chk "  takes the next content"              "1" "$($GGTOPO dump -D "$W" | grep -c '^3 1 p p n u 6101')"

# The file provider renames at persist() time, not at commit.  That is a real
# divergence from the catalog provider and is asserted, not glossed over.
q "begin; select gp_add_segment_primary('sdw3','sdw3',6102,'/d/s2'); rollback;" >/dev/null
chk "a rolled-back write still advanced it" "4" "$(gen)"

chk "gp_remove_segment"                     "t" "$(q "select gp_remove_segment(4::smallint)")"
chk "  removed it from the store"           "0" "$($GGTOPO dump -D "$W" | grep -c 'sdw3')"
chk "no temp files after all that"          "0" "$(ls "$W" | grep -c 'gg_topology\.tmp')"
$GGTOPO dump -D "$W" >/dev/null 2>&1
chk "the store still parses"                "0" "$?"

kill -TERM $WPM 2>/dev/null
wait $WPM 2>/dev/null

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
