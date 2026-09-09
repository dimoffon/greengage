-- The library must be preloaded on every node; the fault injector too.
!\retcode gpconfig -c shared_preload_libraries -v "$(psql -At -c "SELECT array_to_string(array_append(array_remove(string_to_array(current_setting('shared_preload_libraries'), ','), 'gg_duckdb'), 'gg_duckdb'), ',')" postgres)";
!\retcode gpstop -raq -M fast;
CREATE EXTENSION IF NOT EXISTS gp_inject_fault;
CREATE EXTENSION IF NOT EXISTS gg_duckdb;
DROP TABLE IF EXISTS iso_t;
CREATE TABLE iso_t (id int, k int, v text, n numeric(10,2)) DISTRIBUTED BY (id);
INSERT INTO iso_t SELECT i, i % 1000, 'v' || (i % 37), (i % 500) / 4.0 FROM generate_series(1, 300000) i;
ANALYZE iso_t;
