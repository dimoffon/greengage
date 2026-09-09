-- M2: the deparser and the real pass.  Segment-local Result/Agg subtrees and
-- top chains of Limit/Unique/Sort become DuckDB regions; every case is
-- compared against the plain plan and prints 0 | 0 regardless of shape.
CREATE EXTENSION IF NOT EXISTS gg_duckdb;

CREATE TABLE d_orders (
    id int, cust int, qty int, amount numeric(12,2), ts timestamp, d date,
    note text, flag bool, f8 float8, big int8
) DISTRIBUTED BY (id);
INSERT INTO d_orders
SELECT i, i % 97, (i * 7) % 13, (i % 1000) / 4.0, timestamp '2024-01-01' + (i % 500) * interval '1 hour',
       date '2024-01-01' + (i % 365), 'note ' || (i % 50), (i % 3) = 0, i / 3.0, i::int8 * 1000000000
FROM generate_series(1, 60000) i;
INSERT INTO d_orders VALUES (60001, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
ANALYZE d_orders;

-- what the pass considers, and why it declines
SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;
SET gp_enable_multiphase_agg = off;

-- grouped aggregate on the distribution key: a single-phase HashAgg on the segments
EXPLAIN (COSTS OFF)
SELECT id, count(*), sum(qty), min(amount), max(ts), bool_and(flag)
  FROM d_orders WHERE id < 3000 GROUP BY id;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT id, count(*) AS n FROM d_orders WHERE id < 30 GROUP BY id;

-- ORDER BY + LIMIT pushed to the segments
EXPLAIN (COSTS OFF)
SELECT id, amount FROM d_orders ORDER BY amount DESC, id LIMIT 5;

-- text ordering is refused under the database collation, accepted under "C"
EXPLAIN (COSTS OFF)
SELECT note, id FROM d_orders ORDER BY note, id LIMIT 3;
EXPLAIN (COSTS OFF)
SELECT note, id FROM d_orders ORDER BY note COLLATE "C", id LIMIT 3;

SET gg_duckdb.explain_decisions = off;

-- results equal the plain plan: materialise both and compare both ways
CREATE OR REPLACE FUNCTION d_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE missing bigint; extra bigint;
BEGIN
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'CREATE TEMP TABLE d_base AS ' || q;
  SET LOCAL gg_duckdb.mode = force;
  EXECUTE 'CREATE TEMP TABLE d_duck AS ' || q;
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'SELECT count(*) FROM (TABLE d_base EXCEPT ALL TABLE d_duck) x' INTO missing;
  EXECUTE 'SELECT count(*) FROM (TABLE d_duck EXCEPT ALL TABLE d_base) x' INTO extra;
  DROP TABLE d_base; DROP TABLE d_duck;
  RETURN missing || ' | ' || extra;
END $$;

SELECT d_check($q$ SELECT id, count(*) AS n, sum(qty) AS sq, min(amount) AS mina, max(ts) AS maxts, max(d) AS maxd,
                          bool_and(flag) AS ba, count(note) AS cn
                   FROM d_orders GROUP BY id $q$);
SELECT d_check($q$ SELECT id, sum(qty * 2 + 1) AS s1, max(CASE WHEN flag THEN amount ELSE -amount END) AS m1,
                          count(*) FILTER (WHERE note LIKE 'note 1%') AS c1, sum(big) AS s2, sum(amount) AS s3,
                          min(note COLLATE "C") AS m2, count(DISTINCT qty) AS c2
                   FROM d_orders GROUP BY id HAVING count(*) > 0 $q$);
SELECT d_check($q$ SELECT id, qty FROM d_orders WHERE qty IN (1, 2, 3) AND coalesce(flag, false)
                   ORDER BY id LIMIT 100 $q$);
SELECT d_check($q$ SELECT id, amount FROM d_orders ORDER BY amount DESC NULLS FIRST, id LIMIT 7 $q$);
SELECT d_check($q$ SELECT id, amount FROM d_orders ORDER BY amount ASC NULLS LAST, id LIMIT 7 OFFSET 3 $q$);
SELECT d_check($q$ SELECT DISTINCT qty FROM d_orders $q$);
SELECT d_check($q$ SELECT id, length(note) AS e1, note || '!' AS e2, qty % 4 AS e3, -qty AS e4, qty / 3 AS e5,
                          f8 / 2.0 AS e6, abs(qty - 6) AS e7, qty::int8 + big AS e8, d + 1 AS e9,
                          ts - interval '1 day' AS e10, nullif(qty, 0) AS e11, qty IS NULL AS e12
                   FROM d_orders WHERE id < 500 AND (qty > 3 OR flag IS NOT TRUE) ORDER BY id LIMIT 200 $q$);
-- a Motion leaf: the aggregate above a Redistribute Motion, single phase
SELECT d_check($q$ SELECT cust, count(*), sum(amount), max(id) FROM d_orders GROUP BY cust $q$);
SELECT d_check($q$ SELECT cust, count(*) FROM d_orders GROUP BY cust ORDER BY cust LIMIT 10 $q$);

-- division by zero is an error in both executors
SET gg_duckdb.mode = force;
SELECT id, sum(qty / (id - 7)) FROM d_orders WHERE id < 10 GROUP BY id ORDER BY id;
SET gg_duckdb.mode = off;
SELECT id, sum(qty / (id - 7)) FROM d_orders WHERE id < 10 GROUP BY id ORDER BY id;

-- the gate: auto mode needs gg_duckdb.min_rows estimated input rows
SET gg_duckdb.mode = auto;
SET gg_duckdb.min_rows = 1000000;
EXPLAIN (COSTS OFF) SELECT id, count(*) FROM d_orders GROUP BY id;
SET gg_duckdb.min_rows = 0;
-- ... and the cost gate must favour DuckDB; for a tiny input it does not
EXPLAIN (COSTS OFF) SELECT id, count(*) FROM d_orders WHERE id < 30 GROUP BY id;
SET gg_duckdb.cost_fixed = 0; SET gg_duckdb.cost_convert_row = 0; SET gg_duckdb.cost_convert_byte = 0;
EXPLAIN (COSTS OFF) SELECT id, count(*) FROM d_orders GROUP BY id;
RESET gg_duckdb.min_rows;
RESET gg_duckdb.cost_fixed; RESET gg_duckdb.cost_convert_row; RESET gg_duckdb.cost_convert_byte;

-- a bad query never leaves the planner: plan-time validation declines it
SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;
SET gg_duckdb.validate_at_plan_time = on;
EXPLAIN (COSTS OFF) SELECT id, sum(f8) FROM d_orders GROUP BY id;   -- float sum: not eligible

-- a projection DuckDB cannot compute (upper) is computed by the executor in
-- the leaf; the aggregates above it are regions
EXPLAIN (COSTS OFF) SELECT upper(note) AS u, count(*), sum(qty) FROM d_orders GROUP BY 1;
SET gg_duckdb.explain_decisions = off;
SELECT d_check($q$ SELECT upper(note) AS u, count(*) AS n, sum(qty) AS s FROM d_orders GROUP BY 1 $q$);

RESET gg_duckdb.explain_decisions;
RESET gg_duckdb.validate_at_plan_time;
RESET gp_enable_multiphase_agg;
RESET gg_duckdb.mode;
DROP FUNCTION d_check(text);
DROP TABLE d_orders;
