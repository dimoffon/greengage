-- An Iceberg table partitioned by day(sale_date) and by region, reached
-- through its REST catalog.  Iceberg keeps the partitioning in the table's
-- own metadata (its partition spec), so the Greengage side is one foreign
-- table: writes go into the spec's partitions (DuckDB writes one data file
-- per partition it touches), and reads that filter on sale_date or region
-- skip the data files of the other partitions before any of them is opened.
--
-- The table demo.sales is created with its partition spec by the catalog's
-- tooling (test/external/init/partitioned.py, pyiceberg: PartitionSpec of
-- day(sale_date) and identity(region)); DuckDB, and so gg_duckdb, cannot
-- declare a spec, only write into one.
--
-- Runs as a superuser against the Compose stack of test/external (MinIO on
-- localhost:9100, the REST catalog on localhost:8181):
--   psql -d postgres -f samples/iceberg_partitioned.sql
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
SET gg_duckdb.data_directories = 's3://ggduck/';   -- the catalog's files are on S3

CREATE SERVER lakehouse FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (s3_endpoint 'localhost:9100', s3_url_style 'path', s3_use_ssl 'false', s3_region 'us-east-1',
             iceberg_endpoint 'http://localhost:8181', iceberg_auth 'none');
CREATE USER MAPPING FOR CURRENT_USER SERVER lakehouse
    OPTIONS (s3_access_key_id 'ggadmin', s3_secret_access_key 'gg-secret-1');

-- ---------------------------------------------------------------- the table
-- namespace.table names the catalog's table.  The writer must be one
-- process, the coordinator: DuckDB's Iceberg commits do not detect each
-- other, so the wrapper refuses inserts from the segments.
CREATE FOREIGN TABLE ice_sales (
    id        bigint,
    sale_date date,
    region    text,
    product   text,
    qty       bigint,
    amount    numeric(12,2),
    sold_at   timestamp
) SERVER lakehouse OPTIONS (location 'demo.sales', format 'iceberg', mpp_execute 'coordinator');

-- ---------------------------------------------------------------- writes
-- one snapshot per transaction; the rows are gathered to the coordinator and
-- DuckDB writes one Parquet file per (day, region) partition the rows touch
INSERT INTO ice_sales
SELECT i, date '2024-03-01' + (i % 7), (ARRAY['eu', 'us', 'apac'])[1 + i % 3],
       'p' || (i % 20), 1 + i % 5, ((i % 1000) / 4.0)::numeric(12,2),
       timestamp '2024-03-01' + i * interval '3 minutes'
FROM generate_series(1, 21000) i;

-- a transaction: no snapshot until it commits
BEGIN;
INSERT INTO ice_sales VALUES (100001, '2024-03-08', 'eu', 'p1', 1, 1.00, '2024-03-08 10:00:00');
ROLLBACK;
INSERT INTO ice_sales VALUES (100001, '2024-03-08', 'eu', 'p1', 1, 1.00, '2024-03-08 10:00:00');

-- the snapshots and the data files, per partition
SELECT gg_duckdb.query($q$ SET GLOBAL unsafe_enable_version_guessing = true $q$);
SELECT gg_duckdb.query($q$
    SELECT 'snapshots: ' || count(*) FROM iceberg_snapshots('s3://ggduck/warehouse/demo/sales') $q$);
SELECT gg_duckdb.query($q$
    SELECT regexp_extract(file_path, 'data/(.*)/[^/]*$', 1) || ': ' || record_count || ' rows'
    FROM iceberg_metadata('s3://ggduck/warehouse/demo/sales') WHERE manifest_content = 'DATA' ORDER BY 1 $q$);

-- ---------------------------------------------------------------- reads
-- filters on the partition columns are pushed to DuckDB's Iceberg scan, which
-- reads only the data files of the matching partitions (the manifests carry
-- the partition values and the column bounds)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*), sum(amount) FROM ice_sales WHERE sale_date = '2024-03-02' AND region = 'us';
SELECT count(*), sum(amount) FROM ice_sales WHERE sale_date = '2024-03-02' AND region = 'us';
SELECT sale_date, region, count(*), sum(amount) FROM ice_sales
WHERE sale_date BETWEEN '2024-03-02' AND '2024-03-03' GROUP BY 1, 2 ORDER BY 1, 2;

-- an aggregate with DuckDB regions: the scan runs on the coordinator (the
-- table's mpp_execute), so the region needs gg_duckdb.on_coordinator; the
-- Iceberg scan is then a native reader of the region, no rows are converted
SET gg_duckdb.mode = force;
SET gg_duckdb.on_coordinator = on;
EXPLAIN (COSTS OFF)
SELECT region, count(*), sum(qty), sum(amount) FROM ice_sales GROUP BY region;
SELECT region, count(*), sum(qty), sum(amount) FROM ice_sales GROUP BY region ORDER BY region;
RESET gg_duckdb.on_coordinator;
RESET gg_duckdb.mode;

-- a join with a distributed heap table: the coordinator reads the Iceberg
-- table, the join runs on the segments after a motion
CREATE TABLE products (product text, category text) DISTRIBUTED REPLICATED;
INSERT INTO products SELECT 'p' || i, CASE WHEN i < 10 THEN 'food' ELSE 'tools' END FROM generate_series(0, 19) i;
SELECT p.category, count(*), sum(s.amount)
FROM ice_sales s JOIN products p USING (product)
WHERE s.region = 'eu' GROUP BY 1 ORDER BY 1;

-- every table of the namespace as a foreign table, columns from the catalog
CREATE SCHEMA lakehouse;
IMPORT FOREIGN SCHEMA "demo" FROM SERVER lakehouse INTO lakehouse;
SELECT foreign_table_name FROM information_schema.foreign_tables WHERE foreign_table_schema = 'lakehouse' ORDER BY 1;
SELECT count(*) FROM lakehouse.sales;

-- clean up
-- DROP SCHEMA lakehouse CASCADE; DROP TABLE products; DROP SERVER lakehouse CASCADE;
