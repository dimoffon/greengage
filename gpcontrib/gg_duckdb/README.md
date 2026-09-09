# gg_duckdb — DuckDB auxiliary executor for Greengage

`gg_duckdb` embeds [DuckDB](https://duckdb.org) in every Greengage backend as a
second executor for segment-local, Motion-free parts of a plan. This directory
is at **milestone M2**: the library builds into the tree, loads on the
coordinator and on every segment, opens a per-backend DuckDB instance lazily on
the backend thread, and with `gg_duckdb.mode = auto|force` a planner pass turns
eligible segment-local subtrees into DuckDB regions: plain, hashed and sorted
aggregates (`count`, `sum`, `min`, `max`, `bool_and`, `bool_or`, with DISTINCT
and FILTER), projections, and top chains of `ORDER BY`, `DISTINCT` and
`LIMIT/OFFSET` over a whitelist of types, operators and functions whose DuckDB
semantics equal PostgreSQL's. Leaves (scans, Motion receivers, anything else)
stay ordinary plan nodes pulled by DuckDB on the backend thread. Every region
query is prepared on the coordinator before it is used, so an ineligible or
unbindable subtree simply stays on the standard executor. Joins, Append and
two-phase aggregates come with M3; the design is in
`doc/architecture/gg-duckdb-executor.md`.

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
DuckDB CLI in `PREFIX/bin` for data generation.

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
a region needs under auto, 100000), `explain_decisions` (NOTICE per candidate
subtree with the reason it was declined, or its DuckDB query), `on_coordinator`
(regions in coordinator slices, off), `validate_at_plan_time` (prepare every
region query on the coordinator first, on), `max_memory` and `min_memory`
(bounds of a region's DuckDB memory limit, 512MB and 64MB), `reserve_memory`
(reserve the region's budget with the vmem tracker, on), `temp_directory`
(default `base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid>` under each node's data
directory), `max_temp_directory_size`, `release_instance_at_end`, and the
development aids `debug_wrap` and `debug_region_sql`. The core GUC
`optimizer_enable_duckdb` gates plans produced by GPORCA.

## Tests

```sh
make -C gpcontrib/gg_duckdb installcheck                             # ORCA answer files
PGOPTIONS='-c optimizer=off' make -C gpcontrib/gg_duckdb installcheck  # planner answer files
```

`setup` and `teardown` restart the cluster. `region` covers every carried type
with NULLs and extremes, both differential comparisons (`0 | 0`), aggregates
above a region, LIMIT early stop, a nested-loop rescan, a PostgreSQL error
raised inside the leaf, DuckDB-side errors (overflow, type mismatch), and
cancellation by `statement_timeout`. `deparse` covers the pass and the
deparser: EXPLAIN shapes and decision notices, nine differential comparisons
(grouped aggregates with expressions, FILTER, DISTINCT, collated min, int8 and
numeric sums; sort-limit chains with NULLS FIRST/LAST and OFFSET; DISTINCT;
regions above Redistribute Motion leaves), division by zero, the auto-mode
gate, and the collation and float-sum rejections.

## Measured on the development host (DuckDB 1.5.5, 2026-09-09)

Opening the instance costs about 30 ms and 15–20 MB of RSS per backend
(a segment QE grows from ~21 MB to ~40 MB); a 2M-row group-by adds ~4 MB;
the thread count of a QE stays at two (main + interconnect receiver).
