#!/usr/bin/env bash
# Create and fill the tpcds schema in the current database from the TPC-DS kit
# (github.com/dimoffon/TPC-DS: dsdgen/dsqgen, the Greengage DDL with its
# distribution keys and partitioning, the load statements and the query
# templates), and generate the queries for that scale into queries/.
#   load.sh [-f] [-r ROUNDS] [SF]
# SF is the scale factor in GB (1 = 2.9M store_sales rows); -f regenerates
# flat files that already exist for that scale; -r generates the flat files
# in ROUNDS rounds of PARALLEL/ROUNDS dsdgen chunks, loading and deleting each
# round's files before the next, so only a fraction of them is ever on disk
# (a scale whose flat files would not fit next to the tables).  Environment:
#   TPCDS_KIT     kit checkout with 00_compile_tpcds/tools built (~/ws/TPC-DS)
#   TPCDS_DATA    where dsdgen writes its flat files ($TPCDS_KIT/data/sf$SF)
#   STORAGE       table storage options (the kit's: AO column, zstd 1)
#   GPFDIST_PORT  port of the single gpfdist that serves TPCDS_DATA (8081)
#   PARALLEL      dsdgen chunks generated at once (the number of primaries)
# Connection settings come from the PG* environment (PGDATABASE, PGPORT...).
set -euo pipefail
export LC_NUMERIC=C	# psql, bc and printf agree on the decimal point
force=0
rounds=1
while getopts "fr:" opt; do
	case $opt in
		f) force=1 ;;
		r) rounds=$OPTARG ;;
		*) echo "usage: load.sh [-f] [-r ROUNDS] [SF]" >&2; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
sf=${1:-1}
here=$(cd "$(dirname "$0")" && pwd)
kit=${TPCDS_KIT:-$HOME/ws/TPC-DS}
data=${TPCDS_DATA:-$kit/data/sf$sf}
storage=${STORAGE:-appendonly=true, orientation=column, compresstype=zstd, compresslevel=1}
port=${GPFDIST_PORT:-8081}
tools=$kit/00_compile_tpcds/tools
ddl=$kit/03_ddl
[ -x "$tools/dsdgen" ] && [ -x "$tools/dsqgen" ] || { echo "no dsdgen/dsqgen in $tools (make -C $tools)" >&2; exit 1; }

tables="call_center catalog_page catalog_returns catalog_sales customer customer_address
customer_demographics date_dim household_demographics income_band inventory item promotion
reason ship_mode store store_returns store_sales time_dim warehouse web_page web_returns
web_sales web_site"

sql() { psql -X -q -At -v ON_ERROR_STOP=1 "$@"; }

# 1. Flat files: dsdgen chunks (one per primary segment unless PARALLEL says
#    otherwise) generated in parallel; with -r, a round's worth at a time.
nseg=$(sql -c "select count(*) from gp_segment_configuration where role = 'p' and content >= 0")
par=${PARALLEL:-$nseg}
[ "$par" -ge 2 ] || par=2	# dsdgen wants at least two chunks
generate() {	# chunk... -> the flat files of those chunks in $data
	local pids=() c t
	mkdir -p "$data"
	echo "generating SF$sf chunks $* of $par into $data"
	for c in "$@"; do
		(cd "$tools" && ./dsdgen -scale "$sf" -dir "$data" -parallel "$par" -child "$c" -terminate n -force > "$data/dsdgen.$c.log" 2>&1) &
		pids+=($!)
	done
	for p in "${pids[@]}"; do wait "$p" || { echo "dsdgen failed, see $data/dsdgen.*.log" >&2; exit 1; }; done
	for t in $tables; do
		for c in "$@"; do touch "$data/${t}_${c}_${par}.dat"; done	# every glob matches
	done
	du -sh "$data" | sed 's/^/flat files: /'
}
if [ "$rounds" -gt 1 ]; then
	rm -f "$data"/*.dat
elif [ "$force" = 1 ] || ! ls "$data"/*.dat > /dev/null 2>&1; then
	rm -f "$data"/*.dat
	generate $(seq 1 "$par")
else
	echo "reusing the flat files in $data"
fi

# 2. Schema and tables: the kit's Greengage DDL, its distribution keys and partitioning.
sql -f "$ddl/000.gpdb.tpcds.sql" 2>&1 | grep -v "^NOTICE\|^psql:.*NOTICE" || true
for f in "$ddl"/*.gpdb.*.sql; do
	t=$(basename "$f" .sql); t=${t#*.gpdb.}
	[ "$t" = tpcds ] && continue
	dist=$(awk -F'|' -v t="$t" '$2 == t { print $3 }' "$ddl/distribution.txt")
	sql -f "$f" -v SMALL_STORAGE="$storage" -v MEDIUM_STORAGE="$storage" -v LARGE_STORAGE="$storage" \
		-v DISTRIBUTED_BY="DISTRIBUTED BY ($dist)" -v EVERY=180
done

# 3. External tables over one gpfdist serving the flat files.
host=$(hostname)
for f in "$ddl"/*.ext_tpcds.*.sql; do
	t=$(basename "$f" .sql); t=${t#*.ext_tpcds.}
	sql -f "$f" -v LOCATION="'gpfdist://$host:$port/${t}_[0-9]*_[0-9]*.dat'"
done

# 4. Load.  An append-only column insert keeps a compressor per column of every
#    partition it touches, too much for a fact table's 220 partitions in one
#    statement on a host that limits memory commitment, so a partitioned table
#    is filled one band of its partition key at a time.
band=100
insert() {	# table predicate -> rows
	psql -X -At -v ON_ERROR_STOP=1 -c "INSERT INTO tpcds.$1 SELECT * FROM ext_tpcds.$1 WHERE $2" | sed -n 's/^INSERT 0 //p'
}
mkdir -p "$data"
gpfdist -d "$data" -p "$port" > "$data/gpfdist.log" 2>&1 &
gpfdist_pid=$!
trap 'kill $gpfdist_pid 2> /dev/null || true' EXIT
sleep 1
kill -0 "$gpfdist_pid" || { echo "gpfdist did not start, see $data/gpfdist.log" >&2; exit 1; }
for t in $tables; do sql -c "TRUNCATE tpcds.$t"; done
total=0
load_round() {	# -> appends every table's flat files present in $data
	local f t start key rows n pred
	for f in "$kit"/04_load/*.gpdb.*.sql; do
		t=$(basename "$f" .sql); t=${t#*.gpdb.}
		start=$(date +%s.%N)
		key=$(sql -c "select a.attname from pg_partitioned_table p join pg_attribute a on a.attrelid = p.partrelid and a.attnum = p.partattrs[0] where p.partrelid = 'tpcds.$t'::regclass")
		if [ -z "$key" ]; then
			rows=$(insert "$t" "true")
		else
			rows=0
			while IFS= read -r pred; do
				n=$(insert "$t" "$pred")
				rows=$((rows + n))
			done < <(sql -c "with b as (select regexp_match(pg_get_expr(c.relpartbound, c.oid), 'FROM \((\d+)\) TO \((\d+)\)') m
			                            from pg_class c where c.oid in (select relid from pg_partition_tree('tpcds.$t') where isleaf)),
			                      r as (select min(m[1]::int) lo, max(m[2]::int) hi from b where m is not null)
			                 select format('$key is null or $key < %s', lo) from r
			                 union all select format('$key >= %s and $key < %s', g, least(g + $band, hi)) from r, generate_series(lo, hi - 1, $band) g
			                 union all select format('$key >= %s', hi) from r")
		fi
		printf "%-22s %12s rows %7.1f s\n" "$t" "$rows" "$(echo "$(date +%s.%N) - $start" | bc)"
		total=$((total + rows))
	done
}
if [ "$rounds" -gt 1 ]; then
	per=$(( (par + rounds - 1) / rounds ))
	for ((first = 1; first <= par; first += per)); do
		last=$((first + per - 1)); [ "$last" -le "$par" ] || last=$par
		generate $(seq "$first" "$last")
		load_round
		rm -f "$data"/*.dat
	done
else
	load_round
fi
echo "loaded $total rows"

# 5. Statistics: ANALYZE of a partitioned root covers the root and every leaf.
start=$(date +%s.%N)
for t in $tables; do sql -c "ANALYZE tpcds.$t"; done
printf "analyzed in %.1f s\n" "$(echo "$(date +%s.%N) - $start" | bc)"

# 6. The 99 queries for this scale, one file per template (q14, q23, q24 and
#    q39 hold two statements each), from the kit's templates in its Greengage
#    dialect and with dsqgen's default seed.  dsqgen writes query_0.sql into
#    the current directory whatever -output says.
qdir=$here/queries
tpl=$kit/00_compile_tpcds/query_templates
rm -rf "$qdir"
mkdir -p "$qdir"
(cd "$qdir" && "$tools/dsqgen" -input "$tpl/templates.lst" -directory "$tpl" -dialect pivotal -scale "$sf" -distributions "$tools/tpcds.idx" > dsqgen.log 2>&1)
awk -v dir="$qdir" '
	/^-- start query .* using template query[0-9]+\.tpl/ { match($0, /query[0-9]+\.tpl/); n = substr($0, RSTART + 5, RLENGTH - 9); f = sprintf("%s/q%02d.sql", dir, n); out = 1; next }
	/^-- end query/ { if (out) close(f); out = 0; next }
	out { print > f }' "$qdir/query_0.sql"
rm -f "$qdir/query_0.sql"
echo "$(ls "$qdir"/q*.sql | wc -l) query files in $qdir"
