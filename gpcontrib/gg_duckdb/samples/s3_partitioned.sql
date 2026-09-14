-- A Parquet lake on S3 as a Greengage table partitioned by month (RANGE on
-- sale_date) and, inside each month, by region (LIST), every leaf a gg_duckdb
-- foreign table over its own directory:
--
--   s3://ggduck/lake/sales/2024-03/eu/part_*.parquet
--   s3://ggduck/lake/sales/2024-03/us/part_*.parquet
--   s3://ggduck/lake/sales/2024-03/other/part_*.parquet     (the DEFAULT leaf)
--   s3://ggduck/lake/sales/2024-04/...
--
-- Writes go through the parent: the partition tree routes every row to its
-- leaf, and each segment writes one Parquet file per leaf and transaction
-- when the transaction commits (part_gg-<transaction>-<segment>.parquet).
-- Reads through the parent are pruned by the planner to the leaves the WHERE
-- clause allows, and inside a leaf DuckDB skips the Parquet row groups whose
-- statistics exclude the filter.
--
-- Runs as a superuser against the Compose stack of test/external (MinIO on
-- localhost:9100, bucket ggduck, user ggadmin / gg-secret-1):
--   psql -d postgres -f samples/s3_partitioned.sql
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
SET gg_duckdb.data_directories = 's3://ggduck/';   -- or cluster-wide: gpconfig -c gg_duckdb.data_directories -v 's3://ggduck/'

CREATE SERVER lake FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (s3_endpoint 'localhost:9100', s3_url_style 'path', s3_use_ssl 'false', s3_region 'us-east-1');
CREATE USER MAPPING FOR CURRENT_USER SERVER lake
    OPTIONS (s3_access_key_id 'ggadmin', s3_secret_access_key 'gg-secret-1');

-- ---------------------------------------------------------------- the table
CREATE TABLE sales (
    id        bigint,
    sale_date date,
    region    text,
    product   text,
    qty       bigint,
    amount    numeric(12,2),
    sold_at   timestamp
) PARTITION BY RANGE (sale_date) DISTRIBUTED BY (id);

-- one month, subpartitioned by region: the leaves are foreign tables
CREATE TABLE sales_2024_03 PARTITION OF sales
    FOR VALUES FROM ('2024-03-01') TO ('2024-04-01') PARTITION BY LIST (region);
CREATE FOREIGN TABLE sales_2024_03_eu PARTITION OF sales_2024_03 FOR VALUES IN ('eu', 'uk')
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-03/eu/part_*.parquet');
CREATE FOREIGN TABLE sales_2024_03_us PARTITION OF sales_2024_03 FOR VALUES IN ('us')
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-03/us/part_*.parquet');
CREATE FOREIGN TABLE sales_2024_03_other PARTITION OF sales_2024_03 DEFAULT
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-03/other/part_*.parquet');

CREATE TABLE sales_2024_04 PARTITION OF sales
    FOR VALUES FROM ('2024-04-01') TO ('2024-05-01') PARTITION BY LIST (region);
CREATE FOREIGN TABLE sales_2024_04_eu PARTITION OF sales_2024_04 FOR VALUES IN ('eu', 'uk')
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-04/eu/part_*.parquet');
CREATE FOREIGN TABLE sales_2024_04_us PARTITION OF sales_2024_04 FOR VALUES IN ('us')
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-04/us/part_*.parquet');
CREATE FOREIGN TABLE sales_2024_04_other PARTITION OF sales_2024_04 DEFAULT
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-04/other/part_*.parquet');

-- The same tree in Greengage's classic syntax, with heap leaves exchanged for
-- foreign tables afterwards, works too:
--   CREATE TABLE sales (...) DISTRIBUTED BY (id)
--   PARTITION BY RANGE (sale_date) SUBPARTITION BY LIST (region)
--   SUBPARTITION TEMPLATE (SUBPARTITION eu VALUES ('eu', 'uk'), SUBPARTITION us VALUES ('us'), DEFAULT SUBPARTITION other)
--   (START ('2024-03-01') END ('2024-05-01') EVERY (INTERVAL '1 month'));
--   CREATE FOREIGN TABLE sales_2024_03_eu (LIKE sales) SERVER lake OPTIONS (location '...');
--   ALTER TABLE sales ALTER PARTITION FOR ('2024-03-01') EXCHANGE PARTITION FOR ('eu') WITH TABLE sales_2024_03_eu;

-- ---------------------------------------------------------------- writes
-- INSERT through the parent: rows are routed to their leaf; every segment
-- writes one file per leaf it received rows for, at commit.
INSERT INTO sales
SELECT i, date '2024-03-01' + (i % 61), (ARRAY['eu', 'us', 'uk', 'apac'])[1 + i % 4],
       'p' || (i % 20), 1 + i % 5, ((i % 1000) / 4.0)::numeric(12,2),
       timestamp '2024-03-01' + i * interval '3 minutes'
FROM generate_series(1, 100000) i;

-- the same from a file: COPY routes through the parent as well
COPY sales FROM STDIN WITH (FORMAT csv);
100001,2024-04-02,eu,p1,3,10.00,2024-04-02 09:00:00
100002,2024-04-02,us,p2,1,20.00,2024-04-02 09:05:00
\.

-- a transaction: nothing is written until it commits; a rollback writes nothing
BEGIN;
INSERT INTO sales VALUES (200001, '2024-04-10', 'uk', 'p3', 2, 5.50, '2024-04-10 12:00:00');
ROLLBACK;

-- which leaf got what, and the files behind one leaf (one per segment and transaction)
SELECT tableoid::regclass AS leaf, count(*) FROM sales GROUP BY 1 ORDER BY 1;
SELECT segment, regexp_replace(file, '.*/', '') AS file FROM gg_duckdb.foreign_files('sales_2024_03_eu') ORDER BY 2;

-- ---------------------------------------------------------------- reads
-- the planner keeps only the leaves the filters allow: one month, one region
EXPLAIN (COSTS OFF)
SELECT count(*), sum(amount) FROM sales
WHERE sale_date BETWEEN '2024-03-10' AND '2024-03-20' AND region = 'eu';
SELECT count(*), sum(amount) FROM sales
WHERE sale_date BETWEEN '2024-03-10' AND '2024-03-20' AND region = 'eu';

-- a filter on a non-partition column is pushed into DuckDB's Parquet reader,
-- which skips row groups by their statistics (visible as the WHERE of the
-- DuckDB SQL of each leaf)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, amount FROM sales WHERE sale_date >= '2024-04-01' AND region IN ('eu', 'uk') AND id > 99990;

-- an aggregate over a month: with DuckDB regions on, the readers are inlined
-- into the region and the aggregate runs in DuckDB without row conversion
SET gg_duckdb.mode = force;
EXPLAIN (COSTS OFF)
SELECT region, product, count(*), sum(amount) FROM sales WHERE sale_date >= '2024-04-01' GROUP BY 1, 2;
SELECT region, count(*), sum(qty), sum(amount) FROM sales WHERE sale_date >= '2024-04-01' GROUP BY region ORDER BY region;

-- a join with a distributed heap table, both months
CREATE TABLE products (product text, category text) DISTRIBUTED REPLICATED;
INSERT INTO products SELECT 'p' || i, CASE WHEN i < 10 THEN 'food' ELSE 'tools' END FROM generate_series(0, 19) i;
SELECT p.category, count(*), sum(s.amount)
FROM sales s JOIN products p USING (product)
WHERE s.sale_date >= '2024-03-15' AND s.region <> 'apac'
GROUP BY 1 ORDER BY 1;
RESET gg_duckdb.mode;

-- ---------------------------------------------------------------- maintenance
-- a new month: its leaves are foreign tables over directories that do not
-- need to exist yet; files appear with the first commit
CREATE TABLE sales_2024_05 PARTITION OF sales
    FOR VALUES FROM ('2024-05-01') TO ('2024-06-01') PARTITION BY LIST (region);
CREATE FOREIGN TABLE sales_2024_05_eu PARTITION OF sales_2024_05 FOR VALUES IN ('eu', 'uk')
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-05/eu/part_*.parquet');
CREATE FOREIGN TABLE sales_2024_05_other PARTITION OF sales_2024_05 DEFAULT
    SERVER lake OPTIONS (location 's3://ggduck/lake/sales/2024-05/other/part_*.parquet');
INSERT INTO sales VALUES (300001, '2024-05-01', 'eu', 'p1', 1, 1.00, '2024-05-01 10:00:00');
SELECT count(*) FROM sales_2024_05_eu;

-- statistics for the planner come from the Parquet metadata
ANALYZE sales_2024_03_eu;
SELECT relname, reltuples FROM pg_class WHERE relname = 'sales_2024_03_eu';

-- clean up
-- DROP TABLE sales; DROP TABLE products; DROP SERVER lake CASCADE;
