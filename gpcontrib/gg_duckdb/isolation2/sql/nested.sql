-- Two DuckDB queries nested in one backend: on the coordinator, a region's
-- leaf calls a function whose SPI query is a region itself (the counter of
-- that backend shows the queries run).  On the segments, a leaf calls a
-- function that runs a plain SPI query there (one that touches no table:
-- a function on a segment may not).
1: SET gg_duckdb.mode = force;
1: SET gg_duckdb.on_coordinator = on;
1: CREATE OR REPLACE FUNCTION iso_inner(x int) RETURNS bigint LANGUAGE plpgsql STABLE AS $$ DECLARE r bigint; BEGIN SELECT count(*) INTO r FROM (SELECT g % 7 AS m, count(*) FROM generate_series(1, x * 50) g GROUP BY 1 HAVING count(*) > 3) s; RETURN r; END $$;
1: SELECT m, count(*) FROM (SELECT g % 5 AS m FROM generate_series(1, 300) g WHERE iso_inner(g) > 5) s GROUP BY m ORDER BY m;
1: SELECT queries_executed FROM gg_duckdb.status();
1: SET gg_duckdb.mode = off;
1: SELECT m, count(*) FROM (SELECT g % 5 AS m FROM generate_series(1, 300) g WHERE iso_inner(g) > 5) s GROUP BY m ORDER BY m;
1: SET gg_duckdb.mode = force;
1: RESET gg_duckdb.on_coordinator;
1: CREATE OR REPLACE FUNCTION iso_seg(x int) RETURNS bigint LANGUAGE plpgsql STABLE AS $$ DECLARE r bigint; BEGIN SELECT count(*) INTO r FROM generate_series(1, x % 100) g WHERE g % 3 = 0; RETURN r; END $$;
1: SELECT count(*) FROM (SELECT k, count(*) FROM iso_t WHERE id > 299000 AND iso_seg(id) > 10 GROUP BY k) x;
1: SET gg_duckdb.mode = off;
1: SELECT count(*) FROM (SELECT k, count(*) FROM iso_t WHERE id > 299000 AND iso_seg(id) > 10 GROUP BY k) x;
1: DROP FUNCTION iso_inner(int);
1: DROP FUNCTION iso_seg(int);
1q:
