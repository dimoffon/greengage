#!/usr/bin/env bash
# Check the gg_duckdb foreign data wrapper against the Compose stack
# (docker-compose.yml): Parquet, CSV and JSON files in MinIO through S3, and
# an Apache Iceberg table through the REST catalog's warehouse in MinIO.
# Runs against the cluster the PG* environment points at, as a superuser;
# every check prints "0 | 0" (rows missing | rows extra against a reference
# generated in PostgreSQL) or an expected value.
set -uo pipefail
psql -X -v ON_ERROR_STOP=0 -d "${PGDATABASE:-postgres}" <<'SQL'
\set QUIET on
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
SET gg_duckdb.data_directories = 's3://ggduck/';
DROP SERVER IF EXISTS minio CASCADE;
CREATE SERVER minio FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (s3_endpoint 'localhost:9100', s3_url_style 'path', s3_use_ssl 'false', s3_region 'us-east-1');
CREATE USER MAPPING FOR CURRENT_USER SERVER minio
    OPTIONS (s3_access_key_id 'ggadmin', s3_secret_access_key 'gg-secret-1');
CREATE FOREIGN TABLE s3_events (id bigint, k bigint, v text, amt numeric(12,2), ts timestamp, flag boolean, nullable bigint)
    SERVER minio OPTIONS (location 's3://ggduck/files/events/*.parquet');
CREATE FOREIGN TABLE s3_items (id bigint, name text, price numeric(8,2))
    SERVER minio OPTIONS (location 's3://ggduck/files/items.csv', format 'csv', header 'true');
CREATE FOREIGN TABLE s3_tags (id bigint, tag text)
    SERVER minio OPTIONS (location 's3://ggduck/files/tags.json', format 'json');
CREATE FOREIGN TABLE ice_events (id bigint, k bigint, v text, amt numeric(12,2), ts timestamp, flag boolean, nullable bigint)
    SERVER minio OPTIONS (location 's3://ggduck/warehouse/demo/events', format 'iceberg');
DROP TABLE IF EXISTS ref_events;
CREATE TABLE ref_events AS
SELECT i::bigint AS id, (i % 7)::bigint AS k, 'v' || i AS v, (i * 1.25)::numeric(12,2) AS amt,
       (timestamp '2024-01-01' + (i % 100) * interval '1 day') AS ts, i % 3 = 0 AS flag,
       CASE WHEN i % 13 = 0 THEN NULL ELSE i::bigint END AS nullable
FROM generate_series(0, 5999) i DISTRIBUTED BY (id);
CREATE OR REPLACE FUNCTION x_check(q text, r text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE missing bigint; extra bigint;
BEGIN
  EXECUTE 'CREATE TEMP TABLE x_duck AS ' || q;
  EXECUTE 'CREATE TEMP TABLE x_base AS ' || r;
  EXECUTE 'SELECT count(*) FROM (TABLE x_base EXCEPT ALL TABLE x_duck) x' INTO missing;
  EXECUTE 'SELECT count(*) FROM (TABLE x_duck EXCEPT ALL TABLE x_base) x' INTO extra;
  DROP TABLE x_base; DROP TABLE x_duck;
  RETURN missing || ' | ' || extra;
END $$;
\set QUIET off
\echo === S3: six Parquet parts, each read by one segment
SELECT count(*) AS files, count(DISTINCT file) AS distinct_files, count(DISTINCT segment) AS segments FROM gg_duckdb.foreign_files('s3_events');
SET gg_duckdb.mode = off;
SELECT x_check('SELECT * FROM s3_events', 'SELECT * FROM ref_events') AS parquet_scan;
SELECT x_check('SELECT id, v, amt FROM s3_events WHERE k = 3 AND amt > 100 AND v LIKE ''v1%'' AND nullable IS NOT NULL',
               'SELECT id, v, amt FROM ref_events WHERE k = 3 AND amt > 100 AND v LIKE ''v1%'' AND nullable IS NOT NULL') AS parquet_quals;
SELECT count(*) AS csv_rows, sum(price) AS csv_sum FROM s3_items;
SELECT count(*) AS json_rows, min(tag) AS first_tag FROM s3_tags;
\echo === S3: the reader inlined into regions
SET gg_duckdb.mode = force;
SELECT x_check('SELECT k, count(*) AS n, sum(amt) AS sa, min(ts) AS mt, bool_and(flag) AS bf, count(nullable) AS cn FROM s3_events GROUP BY k',
               'SELECT k, count(*) AS n, sum(amt) AS sa, min(ts) AS mt, bool_and(flag) AS bf, count(nullable) AS cn FROM ref_events GROUP BY k') AS parquet_region;
SELECT x_check('SELECT e.k, i.name, count(*) AS n FROM s3_events e JOIN s3_items i ON e.k = i.id GROUP BY e.k, i.name',
               'SELECT e.k, i.name, count(*) AS n FROM ref_events e JOIN s3_items i ON e.k = i.id GROUP BY e.k, i.name') AS parquet_join_region;
SET optimizer_force_multistage_agg = on; SET gp_eager_two_phase_agg = on;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(amt) FROM s3_events GROUP BY k;
\echo === Iceberg: the current snapshot (two appends, then ids below 100 deleted)
SET gg_duckdb.mode = off;
SELECT * FROM gg_duckdb.foreign_files('ice_events');
SELECT count(*) AS rows, min(id) AS min_id, max(id) AS max_id FROM ice_events;
SELECT x_check('SELECT * FROM ice_events', 'SELECT * FROM ref_events WHERE id >= 100') AS iceberg_scan;
SELECT x_check('SELECT k, count(*) AS n, sum(amt) AS sa FROM ice_events WHERE flag GROUP BY k',
               'SELECT k, count(*) AS n, sum(amt) AS sa FROM ref_events WHERE flag AND id >= 100 GROUP BY k') AS iceberg_quals;
SET gg_duckdb.mode = force;
SELECT x_check('SELECT k, count(*) AS n, sum(amt) AS sa FROM ice_events GROUP BY k',
               'SELECT k, count(*) AS n, sum(amt) AS sa FROM ref_events WHERE id >= 100 GROUP BY k') AS iceberg_region;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM ice_events GROUP BY k;
RESET optimizer_force_multistage_agg; RESET gp_eager_two_phase_agg;
\echo === statistics and import over S3
ANALYZE s3_events;
SELECT reltuples FROM pg_class WHERE relname = 's3_events';
CREATE SCHEMA IF NOT EXISTS s3_imported;
IMPORT FOREIGN SCHEMA "s3://ggduck/files" FROM SERVER minio INTO s3_imported;
SELECT foreign_table_name FROM information_schema.foreign_tables WHERE foreign_table_schema = 's3_imported' ORDER BY 1;
SELECT count(*) FROM s3_imported.events;
\set QUIET on
RESET gg_duckdb.mode;
DROP SCHEMA s3_imported CASCADE;
DROP FUNCTION x_check(text, text);
DROP TABLE ref_events;
DROP SERVER minio CASCADE;
SQL
