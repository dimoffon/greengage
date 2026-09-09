-- M4: DuckDB(t1 t2 ...) and NoDuckDB(t1 t2 ...) hints through pg_hint_plan.
-- A hint forces or forbids regions by the aliases of their scan leaves; it
-- beats the cost gate, never makes an ineligible subtree eligible, loses to
-- NoDuckDB, and is ignored under gg_duckdb.mode = off.
LOAD 'pg_hint_plan';
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
CREATE TABLE h_a (id int, k int, v text) DISTRIBUTED BY (k);
CREATE TABLE h_b (id int, k int, w int8) DISTRIBUTED BY (k);
INSERT INTO h_a SELECT i, i % 500, 'v' || (i % 37) FROM generate_series(1, 20000) i;
INSERT INTO h_b SELECT i, i % 700, i::int8 * 3 FROM generate_series(1, 20000) i;
ANALYZE h_a; ANALYZE h_b;
-- a hint keyword ORCA does not know must not make it fall back
SET optimizer_trace_fallback = on;
SET gg_duckdb.mode = auto;
SET gg_duckdb.explain_decisions = on;
-- without a hint the gate declines this small input
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
-- DuckDB(a b) forces the maximal eligible region covering both relations
/*+ DuckDB(a b) */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
-- naming one relation is enough: the region rooted at the Agg covers it
/*+ DuckDB(b) */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
-- DuckDB() addresses the whole query
/*+ DuckDB() */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
-- NoDuckDB wins over DuckDB and over gg_duckdb.mode = force
SET gg_duckdb.mode = force;
/*+ DuckDB(a b) NoDuckDB(b) */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
/*+ NoDuckDB() */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
SET gg_duckdb.mode = auto;
-- a hint cannot make an ineligible subtree eligible: upper() is not carried,
-- and a bare scan has nothing for DuckDB to do
/*+ DuckDB(a) */
EXPLAIN (COSTS OFF)
SELECT upper(v), count(*) FROM h_a a GROUP BY 1;
/*+ DuckDB(a) */
EXPLAIN (COSTS OFF)
SELECT * FROM h_a a WHERE id < 10;
-- an alias the query does not have
/*+ DuckDB(zzz) */
EXPLAIN (COSTS OFF)
SELECT k, count(*) FROM h_a a GROUP BY k;
-- gg_duckdb.strict makes an unhonoured hint an error
SET gg_duckdb.strict = on;
/*+ DuckDB(zzz) */
EXPLAIN (COSTS OFF)
SELECT k, count(*) FROM h_a a GROUP BY k;
/*+ DuckDB(a) */
EXPLAIN (COSTS OFF)
SELECT * FROM h_a a WHERE id < 10;
RESET gg_duckdb.strict;
-- gg_duckdb.mode = off ignores hints
SET gg_duckdb.mode = off;
/*+ DuckDB(a b) */
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
SET gg_duckdb.explain_decisions = off;
-- the forced region returns what the standard executor returns
SET gg_duckdb.mode = auto;
CREATE TEMP TABLE h_base AS
SELECT a.k, count(*) AS n, sum(b.w) AS sw FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
/*+ DuckDB(a b) */
CREATE TEMP TABLE h_duck AS
SELECT a.k, count(*) AS n, sum(b.w) AS sw FROM h_a a JOIN h_b b ON a.k = b.k GROUP BY a.k;
SELECT count(*) AS missing FROM (TABLE h_base EXCEPT ALL TABLE h_duck) x;
SELECT count(*) AS extra FROM (TABLE h_duck EXCEPT ALL TABLE h_base) x;
DROP TABLE h_base; DROP TABLE h_duck;
RESET gg_duckdb.mode;
RESET optimizer_trace_fallback;
DROP TABLE h_a; DROP TABLE h_b;
