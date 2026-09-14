#!/usr/bin/env bash
# Time every query in queries/ (or those matching a glob) under
# gg_duckdb.mode = off, force, auto and auto with gg_duckdb.cost_boundary off
# (the gate taking eligible regions whole), and report medians, the number
# of DuckDB regions in the forced plan, and the speedups off/force and
# off/auto.
#   run.sh [-n RUNS] [-q GLOB] [-o CSV] [-c]
#   -n 0  plan only (no timing)
#   -c    also print the cost gate's estimates (client_min_messages = debug1)
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

time_query() {	# mode file [settings] -> median ms
	local mode=$1 file=$2 extra=${3:-} i t
	[ "$runs" -gt 0 ] || { echo "-"; return; }
	for ((i = 0; i < runs; i++)); do
		t=$(psql -X -q -At -v ON_ERROR_STOP=1 <<-SQL 2>&1 | grep '^Time:' | tail -1 | sed 's/Time: \([0-9.,]*\) ms.*/\1/; s/,/./'
			SET search_path = tpch;
			\\o /dev/null
			-- warm the session first: a region opens DuckDB on every QE and on the
			-- coordinator (about 30 ms once per session), which is not the query's cost
			SET gg_duckdb.mode = force; SET gg_duckdb.min_rows = 0;
			SELECT n_name, count(*) FROM nation GROUP BY 1;
			RESET gg_duckdb.min_rows;
			SET gg_duckdb.mode = $mode;
			$extra
			\\timing on
			\\i $file
		SQL
		)
		echo "${t:-nan}"
	done | median
}

regions_in_plan() {	# file -> count of GGDuckDBRegion nodes under force
	local file=$1
	psql -X -q -At -v ON_ERROR_STOP=1 <<-SQL 2>/dev/null | grep -c GGDuckDBRegion
		SET search_path = tpch;
		SET gg_duckdb.mode = force;
		EXPLAIN (COSTS OFF)
		$(cat "$file")
	SQL
}

ratio() {	# a b -> a/b or -
	awk -v a="$1" -v b="$2" 'BEGIN { if (a + 0 > 0 && b + 0 > 0) printf "%.2f", a / b; else print "-" }'
}

printf "%-8s %7s %10s %10s %10s %10s %8s %8s\n" query regions off_ms force_ms auto_ms whole_ms force_x auto_x
[ -n "$csv" ] && echo "query,regions,off_ms,force_ms,auto_ms,whole_ms,force_x,auto_x" > "$csv"
for f in "$here"/queries/$glob; do
	name=$(basename "$f" .sql)
	regions=$(regions_in_plan "$f")
	off=$(time_query off "$f")
	force=$(time_query force "$f")
	auto=$(time_query auto "$f")
	whole=$(time_query auto "$f" 'SET gg_duckdb.cost_boundary = off;')
	force_x=$(ratio "$off" "$force")
	auto_x=$(ratio "$off" "$auto")
	printf "%-8s %7s %10s %10s %10s %10s %8s %8s\n" "$name" "$regions" "$off" "$force" "$auto" "$whole" "$force_x" "$auto_x"
	[ -n "$csv" ] && echo "$name,$regions,$off,$force,$auto,$whole,$force_x,$auto_x" >> "$csv"
	if [ "$costs" = 1 ]; then
		psql -X -q -At <<-SQL 2>&1 | grep 'gg_duckdb: plan node' | sed 's/^/    /'
			SET search_path = tpch;
			SET gg_duckdb.mode = auto;
			SET client_min_messages = debug1;
			EXPLAIN (COSTS OFF)
			$(cat "$f")
		SQL
	fi
done
