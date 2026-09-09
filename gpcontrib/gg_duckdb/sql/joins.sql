-- M3: joins, Append and two-phase aggregates as region interiors, regions
-- above Motion leaves, and the PartitionSelector boundary rule.  Every case
-- is compared against the plain plan and prints 0 | 0 regardless of shape.
CREATE EXTENSION IF NOT EXISTS gg_duckdb;

CREATE TABLE j_a (id int, k int, v text, n numeric(10,2), flag bool) DISTRIBUTED BY (k);
CREATE TABLE j_b (id int, k int, w int8, ts timestamp) DISTRIBUTED BY (k);
INSERT INTO j_a SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 500 END, 'v' || (i % 37), (i % 1000) / 8.0, i % 2 = 0
  FROM generate_series(1, 30000) i;
INSERT INTO j_b SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 700 END, i::int8 * 3, timestamp '2024-01-01' + (i % 100) * interval '1 day'
  FROM generate_series(1, 20000) i;
CREATE TABLE j_c (id int, k int, v text) DISTRIBUTED BY (id);
INSERT INTO j_c SELECT i, i % 500, 'c' || i FROM generate_series(1, 5000) i;
ANALYZE j_a; ANALYZE j_b; ANALYZE j_c;

CREATE OR REPLACE FUNCTION j_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE missing bigint; extra bigint;
BEGIN
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'CREATE TEMP TABLE j_base AS ' || q;
  SET LOCAL gg_duckdb.mode = force;
  EXECUTE 'CREATE TEMP TABLE j_duck AS ' || q;
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'SELECT count(*) FROM (TABLE j_base EXCEPT ALL TABLE j_duck) x' INTO missing;
  EXECUTE 'SELECT count(*) FROM (TABLE j_duck EXCEPT ALL TABLE j_base) x' INTO extra;
  DROP TABLE j_base; DROP TABLE j_duck;
  RETURN missing || ' | ' || extra;
END $$;

SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;

-- a co-located hash join with an aggregate above it: one region per segment
EXPLAIN (COSTS OFF)
SELECT a.k, count(*), sum(b.w) FROM j_a a JOIN j_b b ON a.k = b.k GROUP BY a.k;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT a.id, b.id FROM j_a a LEFT JOIN j_b b ON a.k = b.k AND b.id < 100 WHERE a.id < 10;

-- two-phase aggregate: the partial Agg on the segments and the final Agg above the Redistribute
SET optimizer_force_multistage_agg = on; SET gp_eager_two_phase_agg = on;
EXPLAIN (COSTS OFF)
SELECT id % 7 AS g, count(*), sum(k), min(id), bool_and(flag) FROM j_a GROUP BY id % 7;
-- a scalar aggregate: the partial Agg on the segments is a region, the final one on the coordinator is not
EXPLAIN (COSTS OFF)
SELECT count(*), max(k) FROM j_a;
-- sum(int8) serialises an internal state between the phases: neither phase is a region
EXPLAIN (COSTS OFF)
SELECT id % 7 AS g, sum(w) FROM j_b GROUP BY id % 7;
-- sum(numeric) and avg(numeric) too, but DuckDB packs their state: the partial
-- phase sums exactly and counts, the final phase adds the packed sums up
EXPLAIN (COSTS OFF, VERBOSE)
SELECT id % 7 AS g, sum(n), sum(n * 2 - 1), avg(n) FROM j_a GROUP BY id % 7;
RESET optimizer_force_multistage_agg; RESET gp_eager_two_phase_agg;
SET gg_duckdb.explain_decisions = off;

-- joins of every supported kind, NULL keys included
SELECT j_check($q$ SELECT a.id AS aid, b.id AS bid, a.v, b.w FROM j_a a JOIN j_b b ON a.k = b.k WHERE a.id < 3000 $q$);
SELECT j_check($q$ SELECT a.id AS aid, b.id AS bid FROM j_a a LEFT JOIN j_b b ON a.k = b.k AND b.id < 5000 WHERE a.id < 3000 $q$);
SELECT j_check($q$ SELECT a.id AS aid, b.id AS bid FROM j_a a RIGHT JOIN j_b b ON a.k = b.k WHERE b.id < 2000 $q$);
SELECT j_check($q$ SELECT a.id AS aid, b.id AS bid FROM j_a a FULL JOIN j_b b ON a.k = b.k WHERE coalesce(a.id, 0) < 800 AND coalesce(b.id, 0) < 800 $q$);
SELECT j_check($q$ SELECT a.id, a.k FROM j_a a WHERE EXISTS (SELECT 1 FROM j_b b WHERE b.k = a.k AND b.id < 4000) $q$);
SELECT j_check($q$ SELECT a.id, a.k FROM j_a a WHERE NOT EXISTS (SELECT 1 FROM j_b b WHERE b.k = a.k) $q$);
SELECT j_check($q$ SELECT a.k, count(*) AS n, sum(b.w) AS sw, max(b.ts) AS mts FROM j_a a JOIN j_b b ON a.k = b.k GROUP BY a.k $q$);
SELECT j_check($q$ SELECT a.k, count(*) AS n FROM j_a a JOIN j_b b ON a.k = b.k JOIN j_c c ON c.k = a.k GROUP BY a.k $q$);
-- a nested loop with an inequality
SET optimizer_enable_hashjoin = off; SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT j_check($q$ SELECT a.id AS aid, b.id AS bid FROM j_a a JOIN j_b b ON a.k = b.k AND a.id < b.id WHERE a.id < 200 AND b.id < 200 $q$);
RESET optimizer_enable_hashjoin; RESET enable_hashjoin; RESET enable_mergejoin;

-- Append: UNION ALL of two co-located scans under an aggregate
SELECT j_check($q$ SELECT k, count(*) AS n FROM (SELECT k FROM j_a UNION ALL SELECT k FROM j_b) u GROUP BY k $q$);

-- two-phase aggregates across the Redistribute Motion, forced and as the optimizers choose
SET optimizer_force_multistage_agg = on; SET gp_eager_two_phase_agg = on;
SELECT j_check($q$ SELECT id % 7 AS g, count(*) AS n, sum(k) AS sk, min(id) AS mi, max(n) AS mn, bool_and(flag) AS bf FROM j_a GROUP BY id % 7 $q$);
SELECT j_check($q$ SELECT id % 7 AS g, count(k) AS n, sum(w) AS sw, min(ts) AS mt FROM j_b GROUP BY id % 7 $q$);
SELECT j_check($q$ SELECT a.v, count(*) AS n, sum(b.w) AS sw FROM j_a a JOIN j_b b ON a.k = b.k GROUP BY a.v $q$);
-- numeric sums and averages across the phases, with NULLs, a FILTER, an
-- all-NULL group, an empty input and exact arithmetic on the way in
SELECT j_check($q$ SELECT id % 7 AS g, sum(n) AS sn, sum(n * (2 - n)) AS sx, avg(n) AS an, count(n) AS cn FROM j_a GROUP BY id % 7 $q$);
SELECT j_check($q$ SELECT id % 5 AS g, sum(n) FILTER (WHERE flag) AS sf, sum(n + 0.5) AS sh, min(n) AS mn FROM j_a GROUP BY id % 5 $q$);
SELECT j_check($q$ SELECT sum(n) AS sn, avg(n) AS an, sum(n * 3) AS s3 FROM j_a $q$);
SELECT j_check($q$ SELECT sum(n) AS sn FROM j_a WHERE id < 0 $q$);
SELECT j_check($q$ SELECT id % 3 AS g, sum(n) AS sn FROM j_a WHERE id % 11 = 0 GROUP BY id % 3 $q$);
RESET optimizer_force_multistage_agg; RESET gp_eager_two_phase_agg;
SELECT j_check($q$ SELECT id % 7 AS g, count(*) AS n, sum(k) AS sk, min(v COLLATE "C") AS mv, max(n) AS mn, bool_and(flag) AS bf FROM j_a GROUP BY id % 7 $q$);
SELECT j_check($q$ SELECT count(*) AS n, max(k) AS mk, min(id) AS mi FROM j_a $q$);
SELECT j_check($q$ SELECT k, sum(w) AS sw, count(*) AS n FROM j_b GROUP BY k $q$);
-- LIMIT above a region above a Motion leaf: early stop must propagate across slices
SELECT count(*) FROM (SELECT id % 7 AS g, count(*) FROM j_a GROUP BY id % 7 LIMIT 3) x;
SELECT count(*) FROM j_a;

-- NOT IN has its own NULL semantics (a left anti semi join not-in): never a region
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM j_a a WHERE a.k NOT IN (SELECT b.k FROM j_b b WHERE b.id < 100);
SET gg_duckdb.explain_decisions = off;
SELECT j_check($q$ SELECT count(*) AS n FROM j_a a WHERE a.k NOT IN (SELECT b.k FROM j_b b WHERE b.id < 100) $q$);

-- a shared CTE: ShareInputScan producers and consumers are leaves, and the
-- subtree under a leaf gets regions of its own
SET gp_cte_sharing = on;
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF)
WITH c AS (SELECT k, count(*) AS n FROM j_a GROUP BY k)
SELECT c1.k, c1.n + c2.n AS s FROM c c1 JOIN c c2 ON c1.k = c2.k WHERE c1.n > 50;
SET gg_duckdb.explain_decisions = off;
SELECT j_check($q$ WITH c AS (SELECT k, count(*) AS n FROM j_a GROUP BY k)
  SELECT c1.k, c1.n + c2.n AS s FROM c c1 JOIN c c2 ON c1.k = c2.k WHERE c1.n > 50 $q$);
RESET gp_cte_sharing;
-- a partitioned table joined with dynamic partition elimination: the join is never a region
CREATE TABLE j_part (id int, k int, d int) DISTRIBUTED BY (id)
  PARTITION BY RANGE (d) (START (0) END (100) EVERY (10));
INSERT INTO j_part SELECT i, i % 500, i % 100 FROM generate_series(1, 20000) i;
CREATE TABLE j_dim (d int, name text) DISTRIBUTED REPLICATED;
INSERT INTO j_dim SELECT i, 'd' || i FROM generate_series(0, 99) i WHERE i < 30;
ANALYZE j_part; ANALYZE j_dim;
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM j_part p JOIN j_dim m ON p.d = m.d;
SET gg_duckdb.explain_decisions = off;
SELECT j_check($q$ SELECT p.d, count(*) AS n FROM j_part p JOIN j_dim m ON p.d = m.d GROUP BY p.d $q$);

RESET gg_duckdb.mode;
DROP FUNCTION j_check(text);
DROP TABLE j_a; DROP TABLE j_b; DROP TABLE j_c; DROP TABLE j_part; DROP TABLE j_dim;
