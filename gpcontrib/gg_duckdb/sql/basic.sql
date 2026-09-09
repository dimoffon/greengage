-- M0 smoke tests: the library is preloaded everywhere, every node can run a
-- DuckDB statement on the backend thread, errors and cancel behave.
CREATE EXTENSION gg_duckdb;

-- preloaded on the coordinator and on every segment
SELECT -1 AS segment, preloaded, instance_initialized FROM gg_duckdb.status()
UNION ALL
SELECT gp_segment_id, (s).preloaded, (s).instance_initialized
  FROM (SELECT gp_segment_id, gg_duckdb.status() AS s FROM gp_dist_random('gp_id')) x
ORDER BY 1;

-- a query runs on the coordinator ...
SELECT gg_duckdb.query('select 42 as a, ''duck'' as b, null::int as c');

-- ... and on every segment, in each segment's own instance
SELECT gp_segment_id, gg_duckdb.query('select 6 * 7') FROM gp_dist_random('gp_id') ORDER BY 1;

-- several rows, several types
SELECT gg_duckdb.query('select i, i * 1.5, ''x'' || i from range(3) t(i) order by i');

-- the instance exists now and reports its settings
SELECT instance_initialized, threads, memory_limit_bytes,
       temp_directory LIKE '%pgsql_tmp_gg_duckdb_%' AS default_temp_dir,
       temp_dir_writable, queries_executed > 0 AS ran, errors
  FROM gg_duckdb.status();

-- a DuckDB error is a PG error, and the instance survives it
SELECT gg_duckdb.query('select * from no_such_table');
SELECT gg_duckdb.query('select 1');
SELECT errors, last_error FROM gg_duckdb.status();

-- syntax errors too
SELECT gg_duckdb.query('selec 1');

-- early stop of a set-returning call releases the result (no crash, no leak error)
SELECT gg_duckdb.query('select i from range(100000) t(i)') LIMIT 2;

-- a bigger query exercises the task loop and spilling settings
SELECT gg_duckdb.query('select count(*), sum(i) from range(1000000) t(i)');

-- the GUCs exist and reach the segments
SET gg_duckdb.max_memory = '256MB';
SELECT gp_segment_id, current_setting('gg_duckdb.max_memory') FROM gp_dist_random('gp_id') ORDER BY 1;
SELECT gp_segment_id, (gg_duckdb.status()).memory_limit_bytes FROM gp_dist_random('gp_id') ORDER BY 1;
RESET gg_duckdb.max_memory;
SHOW gg_duckdb.mode;
SHOW optimizer_enable_duckdb;

-- version() matches the pinned build
SELECT gg_duckdb.version() = (SELECT library_version FROM gg_duckdb.status()) AS same,
       (SELECT library_version = pinned_version FROM gg_duckdb.status()) AS pinned;

DROP EXTENSION gg_duckdb;
