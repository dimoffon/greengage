-- A LIMIT above the Gather Motion is satisfied by the first segments that
-- answer; the query then stops the regions still running elsewhere.  Segment 1
-- is held inside its first leaf batch until the coordinator has its row, then
-- released: its region is squelched (the DuckDB query dropped, the leaf
-- stopped) and the query completes.  The second query has a Motion leaf, so
-- the squelch has to reach the sending slice as well.
1: SET gg_duckdb.mode = force;
SELECT gp_inject_fault_infinite('gg_duckdb_before_batch', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1&: SELECT count(*) FROM (SELECT k, count(*) FROM iso_t GROUP BY k LIMIT 1) x;
SELECT gp_wait_until_triggered_fault('gg_duckdb_before_batch', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
SELECT pg_sleep(1);
SELECT gp_inject_fault('gg_duckdb_before_batch', 'resume', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1<:
SELECT gp_inject_fault('gg_duckdb_before_batch', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
SELECT gp_inject_fault_infinite('gg_duckdb_before_batch', 'suspend', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1&: SELECT count(*) FROM (SELECT id % 7 AS g, count(*) FROM iso_t GROUP BY id % 7 LIMIT 1) x;
SELECT gp_wait_until_triggered_fault('gg_duckdb_before_batch', 1, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
SELECT pg_sleep(1);
SELECT gp_inject_fault('gg_duckdb_before_batch', 'resume', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1<:
SELECT gp_inject_fault('gg_duckdb_before_batch', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1: SELECT count(*) FROM iso_t;
1: SELECT gp_segment_id, (gg_duckdb.status()).errors, (gg_duckdb.status()).reopens FROM gp_dist_random('gp_id') ORDER BY 1;
1q:
