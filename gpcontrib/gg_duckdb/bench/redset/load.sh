#!/usr/bin/env bash
# Fetch the Redset query logs (github.com/amazon-science/redset) and load them
# into the redset schema of the current database as append-only column tables
# partitioned by week, read from the Parquet files through the gg_duckdb
# foreign data wrapper; the foreign tables stay in redset_pq so the same
# queries can run over the files.
#   load.sh [-a | -m MOD -r REM] [-f]
# Redset holds three months of per-statement statistics of 200 provisioned and
# 200 serverless Redshift clusters, one Parquet file per cluster (provisioned:
# 433M rows, 17.7 GB; serverless: 8.2M rows, 0.3 GB).  The serverless tier is
# loaded whole; of the provisioned tier the clusters whose id % MOD = REM are
# loaded (4 and 2 by default: 50 clusters, 3.3 GB of Parquet, about 80M rows),
# or every cluster with -a.  Files already present are not fetched again; -f
# reloads tables that exist.  Environment:
#   REDSET_DATA  where the files go ($HOME/ws/redset/data); it must lie under
#                gg_duckdb.data_directories (gpconfig -c gg_duckdb.data_directories -v DIR; gpstop -u)
#   STORAGE      table storage options (the TPC-DS harness's: append-only column, zstd 1)
# Connection settings come from the PG* environment (PGDATABASE, PGPORT...).
set -euo pipefail
export LC_NUMERIC=C
mod=4
rem=2
force=0
while getopts "am:r:f" opt; do
	case $opt in
		a) mod=1; rem=0 ;;
		m) mod=$OPTARG ;;
		r) rem=$OPTARG ;;
		f) force=1 ;;
		*) echo "usage: load.sh [-a | -m MOD -r REM] [-f]" >&2; exit 1 ;;
	esac
done
data=${REDSET_DATA:-$HOME/ws/redset/data}
storage=${STORAGE:-appendonly=true, orientation=column, compresstype=zstd, compresslevel=1}
url=https://s3.amazonaws.com/redshift-downloads/redset
sql() { psql -X -q -At -v ON_ERROR_STOP=1 "$@"; }

# 1. The files: one per cluster, ids 0 to 199 in both tiers.
fetch() {	# tier id...
	local tier=$1 id; shift
	mkdir -p "$data/$tier"
	for id in "$@"; do
		[ -s "$data/$tier/$id.parquet" ] && continue
		wget -q -O "$data/$tier/$id.parquet" "$url/$tier/parts/$id.parquet" || { rm -f "$data/$tier/$id.parquet"; echo "fetching $tier/$id.parquet failed" >&2; exit 1; }
	done
	echo "$tier: $(ls "$data/$tier"/*.parquet | wc -l) files, $(du -sh "$data/$tier" | cut -f1)"
}
fetch serverless $(seq 0 199)
fetch provisioned $(seq 0 199 | awk -v m="$mod" -v r="$rem" '$1 % m == r')

# 2. DuckDB may only read under gg_duckdb.data_directories.
dirs=$(sql -c "show gg_duckdb.data_directories")
case ",$dirs," in
	*",$data,"* | *",${data%/},"*) ;;
	*) echo "gg_duckdb.data_directories is '$dirs': it must list $data (gpconfig -c gg_duckdb.data_directories -v $data; gpstop -u)" >&2; exit 1 ;;
esac

# 3. The tables: the published columns with the types a warehouse would give
#    them (the files hold counts as doubles and flags as integers; DuckDB casts
#    on the way in), distributed by the statement's key, partitioned by week.
columns="instance_id integer NOT NULL, cluster_size smallint, user_id bigint, database_id bigint, query_id bigint NOT NULL,
	arrival_timestamp timestamp NOT NULL, compile_duration_ms bigint, queue_duration_ms bigint, execution_duration_ms bigint,
	feature_fingerprint varchar(64), was_aborted boolean, was_cached boolean, cache_source_query_id bigint, query_type varchar(16),
	num_permanent_tables_accessed integer, num_external_tables_accessed integer, num_system_tables_accessed integer,
	read_table_ids text, write_table_ids text, mbytes_scanned bigint, mbytes_spilled bigint,
	num_joins integer, num_scans integer, num_aggregations integer"
sql <<-SQL
	CREATE SCHEMA IF NOT EXISTS redset;
	CREATE SCHEMA IF NOT EXISTS redset_pq;
	DO \$\$ BEGIN
		IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'redset_files') THEN
			CREATE SERVER redset_files FOREIGN DATA WRAPPER gg_duckdb;
		END IF;
	END \$\$;
SQL
total=0
for tier in serverless provisioned; do
	sql <<-SQL
		DROP FOREIGN TABLE IF EXISTS redset_pq.$tier;
		CREATE FOREIGN TABLE redset_pq.$tier ($columns)
			SERVER redset_files OPTIONS (location '$data/$tier/*.parquet');
	SQL
	if [ "$force" = 1 ] || [ "$(sql -c "select count(*) from pg_class where oid = to_regclass('redset.$tier')")" = 0 ]; then
		start=$(date +%s.%N)
		sql <<-SQL
			DROP TABLE IF EXISTS redset.$tier;
			CREATE TABLE redset.$tier ($columns)
				WITH ($storage)
				DISTRIBUTED BY (instance_id, query_id)
				PARTITION BY RANGE (arrival_timestamp)
					(START ('2024-02-26') END ('2024-06-03') EVERY (INTERVAL '1 week'));
			INSERT INTO redset.$tier SELECT * FROM redset_pq.$tier;
			ANALYZE redset.$tier;
		SQL
		rows=$(sql -c "select count(*) from redset.$tier")
		size=$(sql -c "select pg_size_pretty(sum(pg_total_relation_size(relid))) from pg_partition_tree('redset.$tier') where isleaf")
		printf "%-12s %12s rows %8s %7.1f s\n" "$tier" "$rows" "$size" "$(echo "$(date +%s.%N) - $start" | bc)"
	else
		rows=$(sql -c "select count(*) from redset.$tier")
		printf "%-12s %12s rows (kept; -f reloads)\n" "$tier" "$rows"
	fi
	sql -c "ANALYZE redset_pq.$tier"
	total=$((total + rows))
done

# 4. The statement classes of the paper's Table 2, a replicated dimension.
sql <<-SQL
	DROP TABLE IF EXISTS redset.query_types;
	CREATE TABLE redset.query_types AS
		SELECT query_type,
		       CASE query_type WHEN 'select' THEN 'RO'
		            WHEN 'insert' THEN 'RW' WHEN 'copy' THEN 'RW' WHEN 'delete' THEN 'RW'
		            WHEN 'update' THEN 'RW' WHEN 'ctas' THEN 'RW' WHEN 'unload' THEN 'RW'
		            WHEN 'analyze' THEN 'Sys' WHEN 'vacuum' THEN 'Sys'
		            ELSE 'Other' END::varchar(8) AS query_class
		FROM (SELECT query_type FROM redset.provisioned UNION SELECT query_type FROM redset.serverless) t
		DISTRIBUTED REPLICATED;
	ANALYZE redset.query_types;
SQL
echo "loaded $total rows; $(sql -c "select count(*) from redset.query_types") statement types"
