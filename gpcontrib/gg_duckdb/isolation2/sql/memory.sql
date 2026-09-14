-- A region whose hash aggregate needs more than its DuckDB memory limit fails
-- with DuckDB's out-of-memory error, cleanly: the backend and its instance go
-- on, and the same query runs under a larger limit.  (DuckDB raises the error
-- from an allocation inside the leaf callback: a C++ exception unwinding
-- through the extension's frames, which must not skip a PG_TRY block.)
1: SET gg_duckdb.mode = force;
1: SET gg_duckdb.max_memory = '16MB';
1: SET gg_duckdb.min_memory = '8MB';
1: SELECT count(*) FROM (SELECT id, repeat(v, 40) AS w, count(*) FROM iso_t GROUP BY 1, 2) x;
1: SELECT count(*) FROM (SELECT id, k, count(*) FROM iso_t GROUP BY 1, 2) x;
1: SET gg_duckdb.max_memory = '64MB';
1: SELECT count(*) FROM (SELECT id, repeat(v, 40) AS w, count(*) FROM iso_t GROUP BY 1, 2) x;
1: SELECT gp_segment_id, (gg_duckdb.status()).errors, (gg_duckdb.status()).reopens FROM gp_dist_random('gp_id') ORDER BY 1;
1q:
