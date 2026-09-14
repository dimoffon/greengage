-- A region suspended inside a leaf callback on a segment is cancelled with
-- the usual message, and the backend's DuckDB is fine afterwards.
1: SET gg_duckdb.mode = force;
SELECT gp_inject_fault_infinite('gg_duckdb_before_batch', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1&: SELECT k, count(*), sum(n) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
SELECT gp_wait_until_triggered_fault('gg_duckdb_before_batch', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT k, count(*), sum(n) FROM iso_t%' AND pid <> pg_backend_pid();
1<:
SELECT gp_inject_fault('gg_duckdb_before_batch', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 0;
1: SELECT k, count(*), sum(n) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
1: SELECT gp_segment_id, (gg_duckdb.status()).errors, (gg_duckdb.status()).reopens FROM gp_dist_random('gp_id') ORDER BY 1;
1q:
