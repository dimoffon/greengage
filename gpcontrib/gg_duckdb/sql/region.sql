-- M1: identity regions.  With gg_duckdb.debug_wrap = scans every SeqScan of a
-- segment slice is wrapped into a DuckDB region whose query is the identity
-- (SELECT c1..cN FROM gg_leaf(0)); the leaf is still the SeqScan, executed by
-- the standard executor and pulled on the backend thread.  Results must be
-- identical to the plain plan; the comparisons below print 0 | 0 regardless
-- of plan shape.
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
SET gg_duckdb.mode = force;

CREATE TABLE region_types (
    id int4,
    b bool, i2 int2, i8 int8, f4 float4, f8 float8,
    n numeric(12,3), n38 numeric(38,10),
    t text, v varchar(20), c char(6),
    d date, ts timestamp, tstz timestamptz, iv interval,
    by bytea, u uuid
) DISTRIBUTED BY (id);
INSERT INTO region_types VALUES
  (1, true,  -32768, -9223372036854775808, -1.5, 2.25, -123456789.125, -1234567890123456789012345678.0123456789,
   'text', 'varchar', 'ab', '2000-01-01', '2000-01-01 00:00:00', '2000-01-01 00:00:00+00', '1 month 2 days 3 seconds',
   '\x00ff10', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
  (2, false, 32767, 9223372036854775807, 3.4028235e38, 1.7976931348623157e308, 999999999.999, 9999999999999999999999999999.9999999999,
   '', '', '', '1969-12-31', '1969-12-31 23:59:59.999999', '1969-12-31 23:59:59.999999+00', '-1 year -2 days -00:00:00.000001',
   '', '00000000-0000-0000-0000-000000000000'),
  (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (4, true, 0, 0, 'NaN', '-0', 0.000, 0, repeat('x', 5000), 'trailing  ', 'pad', 'infinity', 'infinity', '-infinity', '0',
   '\xdeadbeef', 'ffffffff-ffff-ffff-ffff-ffffffffffff'),
  (5, false, 1, -1, 'Infinity', '-Infinity', -0.001, -0.0000000001, E'é中\U0001F600', 'uni', 'x', '-infinity', '-infinity', 'infinity', '178000000 years',
   '\x', '12345678-1234-5678-1234-567812345678');
ANALYZE region_types;

-- the plan has a region under the Gather Motion
SET gg_duckdb.debug_wrap = scans;
EXPLAIN (COSTS OFF) SELECT * FROM region_types;
EXPLAIN (COSTS OFF, VERBOSE) SELECT id, t FROM region_types WHERE id > 1;

-- identity: every value round-trips (the 5000-byte text is shown by its length: it is toasted)
SELECT id, b, i2, i8, f4, f8, n, n38, left(t, 12) AS t12, length(t) AS t_len, v, c, d, ts, tstz, iv, by, u
  FROM region_types ORDER BY id;
SELECT id, length(c) AS c_len, c = 'pad' AS c_eq, octet_length(c) AS c_bytes FROM region_types ORDER BY id;

-- same rows with and without regions, compared both ways
CREATE TEMP TABLE plain AS SELECT * FROM region_types;   -- temp tables live on the coordinator's slice too
SET gg_duckdb.debug_wrap = off;
CREATE TEMP TABLE base AS SELECT * FROM region_types;
SET gg_duckdb.debug_wrap = scans;
SELECT (SELECT count(*) FROM (TABLE base EXCEPT ALL TABLE plain) x) AS missing,
       (SELECT count(*) FROM (TABLE plain EXCEPT ALL TABLE base) x) AS extra;
SELECT (SELECT count(*) FROM (SELECT * FROM base EXCEPT ALL SELECT * FROM region_types) x) AS missing,
       (SELECT count(*) FROM (SELECT * FROM region_types EXCEPT ALL SELECT * FROM base) x) AS extra;

-- projection and quals stay on the leaf; the region carries what the leaf projects
SELECT id, i2 + 1 AS i2p, left(upper(t), 12) AS ut FROM region_types WHERE b ORDER BY id;

-- a bigger table: many chunks, aggregates above the region, LIMIT early stop
CREATE TABLE region_big AS SELECT i AS id, i % 7 AS grp, i::numeric(10,2) / 4 AS amount, 'row ' || i AS label
  FROM generate_series(1, 100000) i DISTRIBUTED BY (id);
ANALYZE region_big;
SELECT count(*), sum(id), min(amount), max(label) FROM region_big;
SELECT grp, count(*), sum(amount) FROM region_big GROUP BY grp ORDER BY grp;
SELECT id FROM region_big ORDER BY id LIMIT 3;
SELECT count(*) FROM (SELECT * FROM region_big LIMIT 10) x;

-- rescan: the region is the inner side of a nested loop
SET enable_hashjoin = off; SET enable_mergejoin = off; SET optimizer_enable_hashjoin = off;
SELECT count(*) FROM (SELECT DISTINCT grp FROM region_big) g JOIN region_types t ON t.id = g.grp;
RESET enable_hashjoin; RESET enable_mergejoin; RESET optimizer_enable_hashjoin;

-- a PostgreSQL error raised inside the leaf surfaces unchanged
SELECT id FROM region_types WHERE 1 / (id - 3) > 0 ORDER BY id;
-- and the session is still usable afterwards
SELECT count(*) FROM region_big;

-- a DuckDB-side error inside the region is an error, never a short result
SET gg_duckdb.debug_region_sql = 'SELECT c1 + 2147483644 AS c1, c2 FROM gg_leaf(0)';
SELECT id, grp FROM region_big WHERE id < 5 ORDER BY id;
SET gg_duckdb.debug_region_sql = 'SELECT c1::VARCHAR AS c1, c2 FROM gg_leaf(0)';
SELECT id, grp FROM region_big WHERE id < 5 ORDER BY id;
SET gg_duckdb.debug_region_sql = 'SELECT c1, c2 FROM gg_leaf(0) WHERE c2 = 3';
SELECT id, grp FROM region_big WHERE id < 20 ORDER BY id;
RESET gg_duckdb.debug_region_sql;

-- cancel: a slow leaf is interrupted by statement_timeout
CREATE FUNCTION region_slow(int) RETURNS bool LANGUAGE sql VOLATILE
  AS 'SELECT pg_sleep(0.01) IS NOT NULL';
SET statement_timeout = '500ms';
SELECT count(*) FROM region_big WHERE region_slow(id);
RESET statement_timeout;
DROP FUNCTION region_slow(int);
SELECT count(*) FROM region_big;

-- mode off: no regions
SET gg_duckdb.mode = off;
EXPLAIN (COSTS OFF) SELECT * FROM region_types;

RESET gg_duckdb.debug_wrap;
RESET gg_duckdb.mode;
DROP TABLE region_types;
DROP TABLE region_big;
