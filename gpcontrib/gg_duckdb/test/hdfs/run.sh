#!/usr/bin/env bash
# Check gg_duckdb against the HDFS and Hive Metastore stand up.sh brings up:
# Iceberg tables registered in the metastore, read from HDFS through the
# embedded JVM.  Runs against the cluster the PG* environment points at, as a
# superuser, with a Kerberos ticket cache in place (up.sh prints the kinit).
# Every check prints "0 | 0" (rows missing | rows extra against the reference
# the seed computed in Spark, in expected_ref.txt) or a value to read.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ccache=${GG_HDFS_CCACHE:-/tmp/krb5cc_$(id -u)}

[ -f "$ccache" ] || { echo "run.sh: no ticket cache at $ccache; see up.sh" >&2; exit 1; }
[ -s "$here/expected_ref.txt" ] || { echo "run.sh: no expected_ref.txt; run up.sh first" >&2; exit 1; }
echo "=== reference (from the Spark seed)"
cat "$here/expected_ref.txt"

# The JVM resolves the stand's names from the hosts file up.sh wrote and reads
# its Kerberos configuration from the copy beside it, so the host's /etc/hosts
# and /etc/krb5.conf are left alone.
jvm_opts="-Xrs -Xms16m -Xmx256m -Xss1m -XX:+UseSerialGC -XX:ReservedCodeCacheSize=32m"
jvm_opts="$jvm_opts -XX:MaxMetaspaceSize=128m -XX:TieredStopAtLevel=1 -XX:CICompilerCount=1"
jvm_opts="$jvm_opts -Djdk.net.hosts.file=$here/.run/jvm-hosts -Djava.security.krb5.conf=$here/.run/krb5.conf"

psql -X -v ON_ERROR_STOP=0 -d "${PGDATABASE:-postgres}" -v conf="$here/client-conf" \
     -v ccache="$ccache" -v jvm="$jvm_opts" <<'SQL'
\set QUIET on
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
-- Every node needs these: the JVM reads them when it starts, so a backend that
-- has already read HDFS keeps what it had.
SET gg_duckdb.jvm_options = :'jvm';
SET gg_duckdb.hadoop_conf_dir = :'conf';
SET gg_duckdb.kerberos_ccache = :'ccache';
SET gg_duckdb.data_directories = 'hdfs://nn.adh.local:8020/';
DROP SERVER IF EXISTS hms CASCADE;
CREATE SERVER hms FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (hms_uri 'thrift://ms.adh.local:9083',
             hms_conf 'metastore.sasl.enabled=true;metastore.kerberos.principal=hive/_HOST@ADH.LOCAL',
             hdfs_authentication 'kerberos');
\set QUIET off

\echo === the metastore database imports as foreign tables, non-Iceberg ones skipped
CREATE SCHEMA IF NOT EXISTS hdfs_gg;
IMPORT FOREIGN SCHEMA "gg" FROM SERVER hms INTO hdfs_gg;
SELECT foreign_table_name FROM information_schema.foreign_tables
 WHERE foreign_table_schema = 'hdfs_gg' ORDER BY 1;
\d hdfs_gg.events

\echo === events: both snapshots, under each mode and both engines
SET gg_duckdb.mode = off;
SELECT count(*) AS n, sum(amount) AS amount, count(DISTINCT day) AS days,
       min(id) AS lo, max(id) AS hi FROM hdfs_gg.events;
SELECT day, count(*) AS n FROM hdfs_gg.events GROUP BY day ORDER BY day;
SET gg_duckdb.mode = force;
SELECT count(*) AS n, sum(amount) AS amount, count(DISTINCT day) AS days,
       min(id) AS lo, max(id) AS hi FROM hdfs_gg.events;
SELECT region, count(*) AS n, sum(amount) AS amount
  FROM hdfs_gg.events GROUP BY region ORDER BY region;
\echo === the catalog leaf is inlined into the region, not pulled through the executor
EXPLAIN (COSTS OFF, VERBOSE) SELECT region, count(*) FROM hdfs_gg.events GROUP BY region;

\echo === merge-on-read deletes are honoured (the deleted rows must be gone)
SET gg_duckdb.mode = off;
SELECT count(*) AS n, count(*) FILTER (WHERE id % 7 = 0) AS should_be_zero FROM hdfs_gg.events_mor;
SET gg_duckdb.mode = force;
SELECT count(*) AS n, count(*) FILTER (WHERE id % 7 = 0) AS should_be_zero FROM hdfs_gg.events_mor;

\echo === which segments read the table
SELECT count(*) AS files, count(DISTINCT segment) AS segments FROM gg_duckdb.foreign_files('hdfs_gg.events');

\echo === the row estimate comes from the manifests, not from a scan
ANALYZE hdfs_gg.events;
SELECT reltuples::bigint AS estimate FROM pg_class WHERE oid = 'hdfs_gg.events'::regclass;

\echo === the demo database the stand ships
CREATE SCHEMA IF NOT EXISTS hdfs_demo;
IMPORT FOREIGN SCHEMA "demo_hdfs" FROM SERVER hms INTO hdfs_demo;
SET gg_duckdb.mode = off;
SELECT count(*) AS regions FROM hdfs_demo.regions;
SELECT count(*) AS sales FROM hdfs_demo.sales;
SET gg_duckdb.mode = auto;
SELECT sale_month, count(*) AS n FROM hdfs_demo.sales GROUP BY sale_month ORDER BY sale_month LIMIT 3;

\echo === writes are refused
INSERT INTO hdfs_gg.events VALUES (1, date '2026-01-01', 'east', 1.00, 'x');

\echo === done with the reads; the tables stay for the ticket check below
SQL

# A missing ticket must fail the query cleanly and leave the backends alive.
# It needs a session of its own: the JVM reads KRB5CCNAME once per process, so
# a backend that already holds one would ignore the changed setting.  A new
# session starts new segment processes, which have no JVM yet.
echo
echo "=== a missing ticket cache fails the query, not the backend"
psql -X -v ON_ERROR_STOP=0 -d "${PGDATABASE:-postgres}" -v conf="$here/client-conf" -v jvm="$jvm_opts" <<'SQL'
SET gg_duckdb.jvm_options = :'jvm';
SET gg_duckdb.hadoop_conf_dir = :'conf';
SET gg_duckdb.kerberos_ccache = '/nonexistent/krb5cc';
SET gg_duckdb.data_directories = 'hdfs://nn.adh.local:8020/';
SELECT count(*) FROM hdfs_demo.regions;
SELECT 'backend alive' AS state;
SQL

psql -X -q -v ON_ERROR_STOP=0 -d "${PGDATABASE:-postgres}" <<'SQL'
DROP SCHEMA IF EXISTS hdfs_gg CASCADE;
DROP SCHEMA IF EXISTS hdfs_demo CASCADE;
DROP SERVER IF EXISTS hms CASCADE;
SQL
echo "=== done"
