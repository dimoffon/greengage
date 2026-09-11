#!/usr/bin/env bash
# Time every TPC-DS query in queries/ (or those matching a glob) under the
# gg_duckdb modes given (off and auto by default), compare the modes' result
# sets, and report medians, the number of DuckDB regions in the auto and in
# the forced plan, and the speedup of every other mode over the first.
#   run.sh [-n RUNS] [-q GLOB] [-m "MODE ..."] [-o DIR] [-t SECONDS] [-s SQL]
#   -n 0   plan only (no timing)
#   -s SQL statements run in every session before the query, e.g.
#          -s "SET gg_duckdb.min_memory = '256MB';"
#   -o DIR where per-query outputs, errors and results.csv go
#          (default results/<timestamp>)
#   -t     statement_timeout of every statement, in seconds (default 600)
# The runs of the modes are interleaved (off, auto, off, auto, ...) so drift
# on the host affects them alike; a mode that fails a query is not retried.
# Connection settings come from the PG* environment; PGOPTIONS may select the
# optimizer (PGOPTIONS='-c optimizer=on').
set -uo pipefail
export LC_NUMERIC=C	# psql, bc, sort -n and printf all agree on the decimal point
runs=3
glob='q*.sql'
modes='off auto'
out=''
timeout=600
settings=''
while getopts "n:q:m:o:t:s:" opt; do
	case $opt in
		n) runs=$OPTARG ;;
		q) glob=$OPTARG ;;
		m) modes=$OPTARG ;;
		o) out=$OPTARG ;;
		t) timeout=$OPTARG ;;
		s) settings=$OPTARG ;;
		*) echo "usage: run.sh [-n RUNS] [-q GLOB] [-m \"MODE ...\"] [-o DIR] [-t SECONDS] [-s SQL]" >&2; exit 1 ;;
	esac
done
here=$(cd "$(dirname "$0")" && pwd)
out=${out:-$here/results/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
export PGOPTIONS="${PGOPTIONS:-}"
read -r -a mode_list <<< "$modes"
base=${mode_list[0]}

median() {
	sort -n | awk '{ a[NR] = $1 } END { if (NR == 0) print "-"; else if (NR % 2) print a[(NR + 1) / 2]; else print (a[NR / 2] + a[NR / 2 + 1]) / 2 }'
}

ratio() {	# a b -> a/b or -
	awk -v a="$1" -v b="$2" 'BEGIN { if (a + 0 > 0 && b + 0 > 0) printf "%.2f", a / b; else print "-" }'
}

# The session every measurement runs in: the schema, the timeout, and a tiny
# forced region first, as a region opens DuckDB on every QE and on the
# coordinator (about 30 ms once per session), which is not the query's cost.
session() {	# mode
	cat <<-SQL
		SET search_path = tpcds;
		SET statement_timeout = '${timeout}s';
		$settings
		\\o /dev/null
		SET gg_duckdb.mode = force; SET gg_duckdb.min_rows = 0;
		SELECT r_reason_desc, count(*) FROM reason GROUP BY 1;
		RESET gg_duckdb.min_rows;
		\\o
		SET gg_duckdb.mode = $1;
	SQL
}

run_once() {	# mode file outfile errfile -> ms of the file's statements together, or ERROR
	local t
	t=$( { session "$1"; printf '\\o %s\n\\timing on\n\\i %s\n' "$3" "$2"; } \
		| psql -X -q -At -v ON_ERROR_STOP=1 2> "$4" \
		| awk '/^Time:/ { sub(",", ".", $2); s += $2 } END { printf "%.3f", s }'; exit "${PIPESTATUS[1]}" )
	if [ $? -eq 0 ]; then echo "$t"; else echo ERROR; fi
}

regions() {	# mode file -> GGDuckDBRegion nodes in the plans of the file's statements
	{
		echo "SET search_path = tpcds; SET gg_duckdb.mode = $1; SET statement_timeout = '${timeout}s'; $settings"
		awk 'BEGIN { need = 1 }
		     need && /^[[:space:]]*(--.*)?$/ { print; next }
		     need { print "EXPLAIN (COSTS OFF)"; need = 0 }
		     { print }
		     /;[[:space:]]*$/ { need = 1 }' "$2"
	} | psql -X -q -At 2> /dev/null | grep -c GGDuckDBRegion
}

compare() {	# out1 out2 -> same | order | DIFF
	if cmp -s "$1" "$2"; then echo same
	elif cmp -s <(sort "$1") <(sort "$2"); then echo order
	else echo DIFF
	fi
}

csv=$out/results.csv
hdr="query,regions_auto,regions_force"
for m in "${mode_list[@]}"; do hdr+=",${m}_ms,${m}_rows"; done
hdr+=",speedup,result,error"
echo "$hdr" > "$csv"
printf "%-5s %6s %7s" query r_auto r_force
for m in "${mode_list[@]}"; do printf " %10s %7s" "${m}_ms" rows; done
printf " %7s %-6s %s\n" speedup result error

for f in "$here"/queries/$glob; do
	name=$(basename "$f" .sql)
	ra=$(regions auto "$f")
	rf=$(regions force "$f")
	unset samples errs rows med
	declare -A samples errs rows med
	for m in "${mode_list[@]}"; do samples[$m]=""; errs[$m]=""; rows[$m]="-"; done
	for ((i = 0; i < runs; i++)); do
		for m in "${mode_list[@]}"; do
			[ -n "${errs[$m]}" ] && continue
			if [ "$i" -eq 0 ]; then o=$out/$name.$m.out; else o=/dev/null; fi
			t=$(run_once "$m" "$f" "$o" "$out/$name.$m.err")
			if [ "$t" = ERROR ] && grep -q "failed to acquire resources" "$out/$name.$m.err"; then
				sleep 20	# the host refused to fork the QEs (memory commitment is transient here): once more
				t=$(run_once "$m" "$f" "$o" "$out/$name.$m.err")
			fi
			if [ "$t" = ERROR ]; then
				errs[$m]="$m: $(grep -m1 -o 'ERROR:.*' "$out/$name.$m.err" | head -c 100)"
				[ "${errs[$m]}" != "$m: " ] || errs[$m]="$m: failed, see $out/$name.$m.err"
			else
				samples[$m]+="$t"$'\n'
				[ "$i" -eq 0 ] && rows[$m]=$(wc -l < "$o")
			fi
		done
	done
	for m in "${mode_list[@]}"; do med[$m]=$(printf '%s' "${samples[$m]}" | median); done
	speedup=-
	result=-
	error=""
	for m in "${mode_list[@]}"; do [ -n "${errs[$m]}" ] && error+="${errs[$m]}; "; done
	if [ "${#mode_list[@]}" -gt 1 ]; then
		m=${mode_list[1]}
		speedup=$(ratio "${med[$base]}" "${med[$m]}")
		[ -z "${errs[$base]}" ] && [ -z "${errs[$m]}" ] && [ "$runs" -gt 0 ] && result=$(compare "$out/$name.$base.out" "$out/$name.$m.out")
	fi
	line="$name,$ra,$rf"
	printf "%-5s %6s %7s" "$name" "$ra" "$rf"
	for m in "${mode_list[@]}"; do
		line+=",${med[$m]},${rows[$m]}"
		printf " %10s %7s" "${med[$m]}" "${rows[$m]}"
	done
	line+=",$speedup,$result,\"${error% }\""
	printf " %7s %-6s %s\n" "$speedup" "$result" "${error% }"
	echo "$line" >> "$csv"
done

# Totals over the queries every mode completed.
[ "$runs" -gt 0 ] && [ "${#mode_list[@]}" -gt 1 ] && awk -F, -v n="${#mode_list[@]}" '
	NR == 1 { next }
	{ ok = 1; for (i = 0; i < n; i++) if ($(4 + 2 * i) == "-") ok = 0 }
	ok { q++; b += $4; a += $6; if ($2 > 0) r++; if ($(4 + 2 * n) + 0 > 1.05) w++; if ($(4 + 2 * n) + 0 < 0.95 && $(4 + 2 * n) != "-") l++ }
	$(5 + 2 * n) == "DIFF" { d++ }
	$(5 + 2 * n) == "order" { o++ }
	$(6 + 2 * n) != "\"\"" { e++ }
	END { printf "\n%d queries completed by every mode: %s %.0f ms, %s %.0f ms (%.2fx); %d with regions under auto; %d faster than 1.05x, %d slower than 0.95x; results: %d DIFF, %d order-only; %d queries with errors\n", q, "'"$base"'", b, "'"${mode_list[1]}"'", a, (a > 0 ? b / a : 0), r + 0, w + 0, l + 0, d + 0, o + 0, e + 0 }' "$csv"
echo "results in $out"
