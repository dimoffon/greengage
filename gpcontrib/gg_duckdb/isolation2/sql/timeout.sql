-- statement_timeout fires while a leaf sleeps inside a region.
1: SET gg_duckdb.mode = force;
1: SET statement_timeout = '2s';
SELECT gp_inject_fault('gg_duckdb_before_batch', 'sleep', '', '', '', 1, 3, 5, dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1: SELECT k, count(*) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
SELECT gp_inject_fault('gg_duckdb_before_batch', 'reset', dbid) FROM gp_segment_configuration WHERE role = 'p' AND content = 1;
1: RESET statement_timeout;
1: SELECT k, count(*) FROM iso_t GROUP BY k ORDER BY k LIMIT 3;
1q:
