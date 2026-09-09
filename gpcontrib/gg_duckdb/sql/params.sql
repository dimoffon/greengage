-- M6: executor parameters inside regions (an InitPlan's result, a prepared
-- statement's argument, a PL/pgSQL variable) and the float-aggregate opt-in.
CREATE TABLE p_a (id int, k int, v text, f float8, n numeric(10,2)) DISTRIBUTED BY (k);
CREATE TABLE p_b (id int, k int) DISTRIBUTED BY (k);
INSERT INTO p_a SELECT i, i % 300, 'v' || (i % 17), i / 7.0, (i % 1000) / 4.0 FROM generate_series(1, 20000) i;
INSERT INTO p_b SELECT i, i % 300 FROM generate_series(1, 5000) i;
ANALYZE p_a; ANALYZE p_b;
CREATE OR REPLACE FUNCTION p_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE missing bigint; extra bigint;
BEGIN
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'CREATE TEMP TABLE p_base AS ' || q;
  SET LOCAL gg_duckdb.mode = force;
  EXECUTE 'CREATE TEMP TABLE p_duck AS ' || q;
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'SELECT count(*) FROM (TABLE p_base EXCEPT ALL TABLE p_duck) x' INTO missing;
  EXECUTE 'SELECT count(*) FROM (TABLE p_duck EXCEPT ALL TABLE p_base) x' INTO extra;
  DROP TABLE p_base; DROP TABLE p_duck;
  RETURN missing || ' | ' || extra;
END $$;
SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;
-- an InitPlan's result, evaluated on the coordinator and dispatched, is a $n
EXPLAIN (COSTS OFF, VERBOSE)
SELECT k, count(*) FROM p_a WHERE id > (SELECT max(id) - 3000 FROM p_b) GROUP BY k;
SET gg_duckdb.explain_decisions = off;
SELECT p_check($q$ SELECT k, count(*) AS n, sum(n) AS sn FROM p_a WHERE id > (SELECT max(id) - 3000 FROM p_b) GROUP BY k $q$);
SELECT p_check($q$ SELECT k, count(*) AS n FROM p_a WHERE v = (SELECT min(v) FROM p_a WHERE id < 10) AND id < (SELECT max(id) FROM p_b) GROUP BY k $q$);
-- a prepared statement's arguments
PREPARE p_grp(int, text) AS SELECT k, count(*) AS n, sum(n) AS sn FROM p_a WHERE id > $1 AND v <> $2 GROUP BY k ORDER BY k LIMIT 5;
EXECUTE p_grp(15000, 'v3');
EXECUTE p_grp(19990, 'v0');
SET gg_duckdb.mode = off;
EXECUTE p_grp(15000, 'v3');
EXECUTE p_grp(19990, 'v0');
DEALLOCATE p_grp;
-- a PL/pgSQL variable inside a region
CREATE OR REPLACE FUNCTION p_count_above(threshold int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r bigint;
BEGIN
  SELECT count(*) INTO r FROM p_a a JOIN p_b b ON a.k = b.k WHERE a.id > threshold;
  RETURN r;
END $$;
SET gg_duckdb.mode = force;
SELECT p_count_above(10000), p_count_above(19000);
SET gg_duckdb.mode = off;
SELECT p_count_above(10000), p_count_above(19000);
-- parameters inside the region itself, not in a leaf's qual: a HAVING
-- clause, a join condition, and a generic plan's arguments
SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF, VERBOSE)
SELECT k, count(*) FROM p_a GROUP BY k HAVING count(*) > (SELECT count(*) FROM p_b) / 100;
SET gg_duckdb.explain_decisions = off;
SELECT p_check($q$ SELECT k, count(*) AS n FROM p_a GROUP BY k HAVING count(*) > (SELECT count(*) FROM p_b) / 100 $q$);
SELECT p_check($q$ SELECT a.k, count(*) AS n FROM p_a a JOIN p_b b ON a.k = b.k AND a.id + b.id > (SELECT max(id) FROM p_b) GROUP BY a.k $q$);
SET plan_cache_mode = force_generic_plan;
PREPARE p_join(int) AS SELECT a.k, count(*) AS n FROM p_a a JOIN p_b b ON a.k = b.k AND a.id + b.id > $1 GROUP BY a.k ORDER BY a.k LIMIT 3;
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF) EXECUTE p_join(20000);
SET gg_duckdb.explain_decisions = off;
EXECUTE p_join(20000);
EXECUTE p_join(24000);
SET gg_duckdb.mode = off;
EXECUTE p_join(20000);
EXECUTE p_join(24000);
DEALLOCATE p_join;
SET gg_duckdb.mode = force;
-- the same through PL/pgSQL, where the variable is a parameter of a generic plan
CREATE OR REPLACE FUNCTION p_join_count(threshold int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r bigint;
BEGIN
  SELECT count(*) INTO r FROM p_a a JOIN p_b b ON a.k = b.k AND a.id + b.id > threshold;
  RETURN r;
END $$;
SELECT p_join_count(20000), p_join_count(24000);
SET gg_duckdb.mode = off;
SELECT p_join_count(20000), p_join_count(24000);
RESET plan_cache_mode;
-- a correlated subquery: the region inside it is rescanned with a new value
SET gg_duckdb.mode = force;
SELECT p_check($q$ SELECT b.id FROM p_b b WHERE b.id < 8 AND EXISTS (SELECT 1 FROM p_a a WHERE a.k = b.k GROUP BY a.v HAVING count(*) > b.id) $q$);
SELECT p_check($q$ SELECT b.id, (SELECT count(*) FROM p_a a WHERE a.k = b.k GROUP BY a.k HAVING sum(a.id) > b.id * 1000) AS n FROM p_b b WHERE b.id < 8 $q$);
DROP FUNCTION p_join_count(int);
-- float sums are declined unless opted in; min/max are fine
SET gg_duckdb.mode = force;
SET gg_duckdb.explain_decisions = on;
EXPLAIN (COSTS OFF) SELECT k, sum(f), avg(f), max(f) FROM p_a GROUP BY k;
SET gg_duckdb.allow_float_aggregates = on;
EXPLAIN (COSTS OFF) SELECT k, sum(f), avg(f), max(f) FROM p_a GROUP BY k;
SET gg_duckdb.explain_decisions = off;
-- within a relative tolerance: the order of summation differs
SELECT k, round(sum(f)::numeric, 6) AS s, round(avg(f)::numeric, 6) AS a, max(f) AS m FROM p_a WHERE k < 3 GROUP BY k ORDER BY k;
SET gg_duckdb.mode = off;
SELECT k, round(sum(f)::numeric, 6) AS s, round(avg(f)::numeric, 6) AS a, max(f) AS m FROM p_a WHERE k < 3 GROUP BY k ORDER BY k;
RESET gg_duckdb.allow_float_aggregates;
RESET gg_duckdb.mode;
DROP FUNCTION p_check(text); DROP FUNCTION p_count_above(int);
DROP TABLE p_a; DROP TABLE p_b;
