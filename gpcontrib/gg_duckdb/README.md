# gg_duckdb — DuckDB auxiliary executor for Greengage

`gg_duckdb` embeds [DuckDB](https://duckdb.org) in every Greengage backend as a
second executor for segment-local, Motion-free parts of a plan. This directory
is at **milestone M5**: the library builds into the tree, loads on the
coordinator and on every segment, opens a per-backend DuckDB instance lazily on
the backend thread, and with `gg_duckdb.mode = auto|force` a planner pass turns
eligible segment-local subtrees into DuckDB regions: hash, merge and nested-loop
joins (inner, left, right, full, semi, anti), `UNION ALL` appends, plain,
hashed and sorted aggregates (`count`, `sum`, `min`, `max`, `bool_and`,
`bool_or`, with DISTINCT and FILTER) including the partial and final phases of
a two-phase aggregate, projections, and top chains of `ORDER BY`, `DISTINCT`
and `LIMIT/OFFSET` over a whitelist of types, operators and functions whose
DuckDB semantics equal PostgreSQL's. Leaves (scans, Motion receivers, shared
scans, subquery scans, joins DuckDB may not run, anything else) stay ordinary
plan nodes pulled by DuckDB on the backend thread, and the subtree under a leaf
gets regions of its own. Every region query is prepared on the coordinator
before it is used, so an ineligible or unbindable subtree simply stays on the
standard executor. Partial `sum(numeric)` and `avg(numeric)` phases run in
DuckDB too: the region packs DuckDB's exact sum and count into the aggregate
state the final phase expects. `DuckDB()`/`NoDuckDB()` hints through
pg_hint_plan force or forbid regions, and under `mode = auto` a cost gate
compares the standard executor's estimate for the interior operators with
DuckDB's, conversion included. The `gg_duckdb` foreign data wrapper reads
Parquet, CSV and JSON files through DuckDB on every segment, each file by
exactly one segment, and the pass inlines such a scan into a region as a
native reader, so joins and aggregates above it see no row conversion on the
way in. The design is in `doc/architecture/gg-duckdb-executor.md`.

## Design in one paragraph

DuckDB is opened with `threads = 1` and `external_threads = 1`, so it owns no
thread of its own: every unit of DuckDB work runs inside
`duckdb_pending_execute_task()` on the backend thread. That is what makes it
legal for DuckDB callbacks (the future leaf table functions) to call back into
the backend, whose buffer manager, memory contexts, error handling and vmem
tracker are all main-thread only. Only DuckDB's C API (`duckdb.h`) is used: it is
the stable client surface across DuckDB minors and the coming 2.0 ABI freeze,
and it keeps the extension in C.

## Building

DuckDB is built once, from the release pinned in `duckdb/duckdb.version`, into a
prefix, and the tree is configured against that prefix:

```sh
gpcontrib/gg_duckdb/duckdb/build.sh $HOME/duckdb-1.5.5      # cmake + make/ninja, ~15 min
./configure ... --with-duckdb=$HOME/duckdb-1.5.5
make && make install                                       # copies libduckdb.so into $GPHOME/lib
```

Without `--with-duckdb` the directory is skipped entirely. `build.sh` builds the
`core_functions`, `parquet`, `icu` and `json` extensions statically, disables
runtime extension loading, sanitizers and `OVERRIDE_NEW_DELETE`, and keeps the
DuckDB CLI in `PREFIX/bin` for data generation. For S3 and Apache Iceberg
add DuckDB's `httpfs`, `avro` and `iceberg` extensions at the commits 1.5.5
pins (`duckdb/extensions.cmake`); their native dependencies come from vcpkg
at the tag DuckDB pins, and the static archives' symbols are hidden so the
bundled OpenSSL and curl never meet the backend's own:

```sh
git clone --branch 2025.12.12 https://github.com/microsoft/vcpkg $HOME/vcpkg && $HOME/vcpkg/bootstrap-vcpkg.sh -disableMetrics
DUCKDB_REMOTE_EXTENSIONS=1 DUCKDB_VCPKG=$HOME/vcpkg gpcontrib/gg_duckdb/duckdb/build.sh $HOME/duckdb-1.5.5   # ~15 min more
```

## Running

The plan node is resolved by name on the segment that receives the plan, so the
library must be in `shared_preload_libraries` on every node:

```sh
gpconfig -c shared_preload_libraries -v gg_duckdb
gpstop -raq -M fast
psql -c 'CREATE EXTENSION gg_duckdb'
psql -c "SELECT gp_segment_id, gg_duckdb.query('select 42') FROM gp_dist_random('gp_id')"
psql -c "SELECT gp_segment_id, (gg_duckdb.status()).* FROM gp_dist_random('gp_id')"
```

GUCs (all `gg_duckdb.*`): `mode` (off/auto/force; off by default: auto applies
the gate, force takes every eligible region), `min_rows` (estimated input rows
a region needs under auto, 100000), the cost gate's constants `cost_fixed`
(planner cost units per region, 50), `cost_convert_row` (0.001) and
`cost_convert_byte` (0.0003) per row and byte converted into or out of DuckDB,
`cost_op_factor` (DuckDB's cost of an interior operator relative to the
standard executor's, 0.25) and `cost_margin` (DuckDB must win by this fraction,
0.25), `explain_decisions` (NOTICE per candidate subtree with the reason it was
declined, or its DuckDB query; the gate's estimates go to the DEBUG1 log),
`strict` (an unhonoured `DuckDB()` hint is an error instead of a notice, off),
`on_coordinator` (regions in coordinator slices, off), `validate_at_plan_time`
(prepare every region query on the coordinator first, on), `max_memory` and
`min_memory` (bounds of a region's DuckDB memory limit, 512MB and 64MB),
`reserve_memory` (reserve the region's budget with the vmem tracker, on),
`temp_directory` (default `base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid>` under
each node's data directory), `max_temp_directory_size`,
`release_instance_at_end`, and the development aids `debug_wrap` and
`debug_region_sql`. The core GUC `optimizer_enable_duckdb` gates plans
produced by GPORCA.

## Hints

With pg_hint_plan loaded (`LOAD 'pg_hint_plan'` or preloaded), a query may
carry `DuckDB(t1 t2 ...)` and `NoDuckDB(t1 t2 ...)` hints naming relation
aliases, or `DuckDB()`/`NoDuckDB()` for the whole query:

```sql
/*+ DuckDB(a b) */ SELECT a.k, count(*) FROM t1 a JOIN t2 b ON a.k = b.k GROUP BY a.k;
```

`DuckDB(S)` forces the maximal eligible region whose scan leaves cover the
aliases in S, bypassing the cost gate; `NoDuckDB(S)` forbids every region
touching S and wins over `DuckDB`. Hints never make an ineligible subtree
eligible and do nothing under `gg_duckdb.mode = off`. A `DuckDB` hint that no
region honoured is reported as a NOTICE, or as an error under
`gg_duckdb.strict`. Aliases of base relations are addressable; subqueries the
optimizer flattens are not. ORCA does not fall back on these hints.

## Reading files: the foreign data wrapper

DuckDB may only touch files under `gg_duckdb.data_directories` (a
superuser setting, synchronised to the segments, comma-separated absolute
paths; empty allows none). Set it cluster-wide with `gpconfig`, or per
session as a superuser. Then:

```sql
CREATE SERVER duck FOREIGN DATA WRAPPER gg_duckdb;
CREATE FOREIGN TABLE lineitem (l_orderkey bigint, l_quantity numeric(10,2), l_shipdate date, ...)
    SERVER duck OPTIONS (location '/data/tpch/lineitem/*.parquet');
IMPORT FOREIGN SCHEMA "/data/tpch" FROM SERVER duck INTO tpch;   -- a table per file or subdirectory
ANALYZE lineitem;
SELECT * FROM gg_duckdb.foreign_files('lineitem');               -- which segment reads which file
```

Table options: `location` (a path, a glob, or a comma-separated list; every
entry must lie under `gg_duckdb.data_directories`), `format` (`parquet`,
the default, `csv` or `json`, also settable on the server or the wrapper),
`union_by_name`, `hive_partitioning`, and for CSV and JSON the reader
options `header`, `delim`, `quote`, `escape`, `nullstr`, `skip`,
`dateformat`, `timestampformat`, `compression`, `sample_size`,
`maximum_object_size`, passed to DuckDB's `read_csv`/`read_json`; a column
option `column_name` maps a column to a differently named file column. With
a DuckDB built with `DUCKDB_REMOTE_EXTENSIONS=1` (see `duckdb/build.sh`) the
location may be an S3 URL and the format `iceberg`:

```sql
CREATE SERVER minio FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (s3_endpoint 'localhost:9100', s3_url_style 'path', s3_use_ssl 'false', s3_region 'us-east-1');
CREATE USER MAPPING FOR CURRENT_USER SERVER minio
    OPTIONS (s3_access_key_id 'ggadmin', s3_secret_access_key 'gg-secret-1');
CREATE FOREIGN TABLE events (...) SERVER minio OPTIONS (location 's3://ggduck/files/events/*.parquet');
CREATE FOREIGN TABLE ice (...) SERVER minio OPTIONS (location 's3://ggduck/warehouse/demo/events', format 'iceberg');
```

The endpoint, region, URL style and TLS flag are server options, the keys
(and an optional `s3_session_token`) user mapping options; the wrapper turns
them into a DuckDB secret scoped to the table's buckets in each backend's
instance. An Iceberg location is the table's directory (its `metadata/`
inside) or a metadata file; the table's current snapshot is read by one
process (the segment its location falls to, or the coordinator with
`mpp_execute 'coordinator'`), with deletes applied by DuckDB's reader.
`gg_duckdb.data_directories` must list the remote prefix (`s3://ggduck/`); a
remote prefix keeps DuckDB's external access on for that backend, since
DuckDB's own sandbox knows only local directories, so the wrapper's checks
are the guard there and `gg_duckdb.query()` stays a superuser tool. The
process environment's `http_proxy` is ignored; `gg_duckdb.http_proxy`
(`host:port`, superuser) names one when remote files need it.
`test/external/` holds a Compose stack (MinIO, an Iceberg REST catalog, a
job that fills them) and `run.sh` with the checks. The
wrapper's default `mpp_execute` is `all segments`: every segment expands
the location and reads the files whose path hash falls to it, so a table
over N files is read by min(N, segments) segments in parallel; a table with
`mpp_execute 'coordinator'` is read by the coordinator alone. Files must be
visible under the same path on every node (a shared or network file
system); `IMPORT FOREIGN SCHEMA` and, without statistics, the planner's
row estimate read the files on the coordinator. Column types come from the
table definition; DuckDB casts the file's values to them, so a mismatch is
an error, not a silent conversion. Quals over carried types and operators
are evaluated inside DuckDB (`EXPLAIN VERBOSE` shows the DuckDB query),
the rest by the executor.

## Tests

```sh
make -C gpcontrib/gg_duckdb installcheck                             # ORCA answer files
PGOPTIONS='-c optimizer=off' make -C gpcontrib/gg_duckdb installcheck  # planner answer files
```

`setup` and `teardown` restart the cluster (teardown leaves the library out
of `shared_preload_libraries`; add it back for manual use). `region` covers every carried type
with NULLs and extremes, both differential comparisons (`0 | 0`), aggregates
above a region, LIMIT early stop, a nested-loop rescan, a PostgreSQL error
raised inside the leaf, DuckDB-side errors (overflow, type mismatch), and
cancellation by `statement_timeout`. `deparse` covers the pass and the
deparser: EXPLAIN shapes and decision notices, nine differential comparisons
(grouped aggregates with expressions, FILTER, DISTINCT, collated min, int8 and
numeric sums; sort-limit chains with NULLS FIRST/LAST and OFFSET; DISTINCT;
regions above Redistribute Motion leaves), division by zero, the auto-mode
gate, and the collation and float-sum rejections. `joins` covers milestone M3
with 19 differential comparisons: every join kind with NULL keys, three-way
joins, a nested loop with an inequality, `UNION ALL` under an aggregate,
two-phase aggregates across a Redistribute Motion (forced and as the
optimizers choose them, numeric sums and averages included), a shared CTE,
`NOT IN` and dynamic partition elimination (both stay leaves), and LIMIT early
stop across slices. `hints` covers the `DuckDB()`/`NoDuckDB()` hints under
both optimizers. `foreign` (an `input/*.source` test: it writes its Parquet,
CSV and JSON files into `data/` with the coordinator's DuckDB) covers the
foreign data wrapper: sharding, every carried type against a PostgreSQL
reference, pushed and local quals, the reader inlined into regions,
rescans, ANALYZE, IMPORT FOREIGN SCHEMA, and the refused locations.
`bench/` holds the TPC-H-shaped benchmark (see its README).

## Measured on the development host (DuckDB 1.5.5, 2026-09-09)

Opening the instance costs about 30 ms and 15–20 MB of RSS per backend
(a segment QE grows from ~21 MB to ~40 MB); a 2M-row group-by adds ~4 MB;
the thread count of a QE stays at two (main + interconnect receiver).

On the TPC-H-shaped benchmark (`bench/`, SF 1, 3 primaries, medians of 3),
regions win by 1.5–1.9x where the interior does much work per input row
(Q1's eight aggregates, Q4's semi join, Q13's left join and counts, Q18's
large group-by) and lose by up to 2x where a large input is converted for a
join that produces little (Q3, Q5, Q10, the local top-N join): converting a
row into DuckDB costs about as much as the executor's own hash join probe.
The cost gate is calibrated on those measurements, and under `mode = auto`
every query of the set runs within noise of the better engine under both
optimizers. `mode` stays `off` by default until the hardening milestone; see
`doc/architecture/gg-duckdb-executor.md` for the numbers and the decision.
