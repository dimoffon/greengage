-- A PostgreSQL error raised inside a leaf on the third batch of one segment
-- comes back as that error; the backend and its DuckDB are fine afterwards.
1: SET gg_duckdb.mode = force;
SELECT gp_inject_fault('gg_duckdb_before_batch', 'error', '', '', '', 3, 3, 0, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 2;
1: SELECT k, count(*) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
SELECT gp_inject_fault('gg_duckdb_before_batch', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 2;
1: SELECT k, count(*) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
1: SELECT gp_segment_id, (gg_duckdb.status()).errors, (gg_duckdb.status()).reopens FROM gp_dist_random('gp_id') ORDER BY 1;
1q:
