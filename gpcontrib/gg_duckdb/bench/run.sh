#!/usr/bin/env bash
# Time every query in queries/ (or those matching a glob) under
# gg_duckdb.mode = off, force and auto, and report medians, the number of
# DuckDB regions in the forced plan, and the speedup.
#   run.sh [-n RUNS] [-q GLOB] [-o CSV] [-c]
#   -c  also print the cost gate's estimates (client_min_messages = debug1)
# Connection settings come from the PG* environment; PGOPTIONS may select the
# optimizer (PGOPTIONS='-c optimizer=on').
set -uo pipefail
runs=3
glob='*.sql'
csv=''
costs=0
while getopts "n:q:o:c" opt; do
	case $opt in
		n) runs=$OPTARG ;;
		q) glob=$OPTARG ;;
		o) csv=$OPTARG ;;
		c) costs=1 ;;
		*) echo "usage: run.sh [-n RUNS] [-q GLOB] [-o CSV] [-c]" >&2; exit 1 ;;
	esac
done
here=$(cd "$(dirname "$0")" && pwd)
export PGOPTIONS="${PGOPTIONS:-}"

median() {
	sort -n | awk '{ a[NR] = $1 } END { if (NR == 0) print "-"; else if (NR % 2) print a[(NR + 1) / 2]; else print (a[NR / 2] + a[NR / 2 + 1]) / 2 }'
}

time_query() {	# mode file -> median ms
	local mode=$1 file=$2 i t
	for ((i = 0; i < runs; i++)); do
		t=$(psql -X -q -At -v ON_ERROR_STOP=1 <<-SQL 2>&1 | grep '^Time:' | tail -1 | sed 's/Time: \([0-9.,]*\) ms.*/\1/; s/,/./'
			SET search_path = tpch;
			SET gg_duckdb.mode = $mode;
			\\timing on
			\\o /dev/null
			\\i $file
		SQL
		)
		echo "${t:-nan}"
	done | median
}

regions_in_plan() {	# file -> count of GGDuckDBRegion nodes under force
	psql -X -q -At -v ON_ERROR_STOP=1 <<-SQL 2>/dev/null | grep -c GGDuckDBRegion
		SET search_path = tpch;
		SET gg_duckdb.mode = force;
		EXPLAIN (COSTS OFF)
		\\i $file
	SQL
}

printf "%-8s %7s %10s %10s %10s %8s\n" query regions off_ms force_ms auto_ms speedup
[ -n "$csv" ] && echo "query,regions,off_ms,force_ms,auto_ms,speedup" > "$csv"
for f in "$here"/queries/$glob; do
	name=$(basename "$f" .sql)
	regions=$(regions_in_plan "$f")
	off=$(time_query off "$f")
	force=$(time_query force "$f")
	auto=$(time_query auto "$f")
	speedup=$(awk -v a="$off" -v b="$force" 'BEGIN { if (b > 0) printf "%.2f", a / b; else print "-" }')
	printf "%-8s %7s %10s %10s %10s %8s\n" "$name" "$regions" "$off" "$force" "$auto" "$speedup"
	[ -n "$csv" ] && echo "$name,$regions,$off,$force,$auto,$speedup" >> "$csv"
	if [ "$costs" = 1 ]; then
		psql -X -q -At <<-SQL 2>&1 | grep 'gg_duckdb: plan node' | sed 's/^/    /'
			SET search_path = tpch;
			SET gg_duckdb.mode = auto;
			SET client_min_messages = debug1;
			EXPLAIN (COSTS OFF)
			\\i $f
		SQL
	fi
done
