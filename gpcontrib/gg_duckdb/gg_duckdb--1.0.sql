/* gpcontrib/gg_duckdb/gg_duckdb--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gg_duckdb" to load this file. \quit

-- Version of the DuckDB library the backend loaded.
CREATE FUNCTION gg_duckdb.version()
RETURNS text
AS 'MODULE_PATHNAME', 'gg_duckdb_version'
LANGUAGE C STRICT STABLE;

-- Run a DuckDB SQL statement in this backend's embedded instance and return
-- each result row rendered as text (columns separated by '|').  A smoke and
-- experiment tool: `SELECT gp_segment_id, gg_duckdb.query('select 42')
-- FROM gp_dist_random('gp_id')` proves every segment can run DuckDB.
CREATE FUNCTION gg_duckdb.query(sql text)
RETURNS SETOF text
AS 'MODULE_PATHNAME', 'gg_duckdb_query'
LANGUAGE C STRICT VOLATILE;

-- Per-backend state of the embedded DuckDB; call it through
-- gp_dist_random('gp_id') to see every segment.
CREATE FUNCTION gg_duckdb.status(
    OUT preloaded boolean,
    OUT library_version text,
    OUT pinned_version text,
    OUT instance_initialized boolean,
    OUT threads integer,
    OUT memory_limit_bytes bigint,
    OUT temp_directory text,
    OUT temp_dir_writable boolean,
    OUT queries_executed bigint,
    OUT errors bigint,
    OUT last_error text)
RETURNS record
AS 'MODULE_PATHNAME', 'gg_duckdb_status'
LANGUAGE C STRICT VOLATILE;
