# gg_duckdb — DuckDB auxiliary executor for Greengage

`gg_duckdb` embeds [DuckDB](https://duckdb.org) in every Greengage backend as a
second executor for segment-local, Motion-free parts of a plan. This directory
is at **milestone M1**: the library builds into the tree, loads on the
coordinator and on every segment, opens a per-backend DuckDB instance lazily on
the backend thread, exposes `gg_duckdb.query()`, `gg_duckdb.status()` and
`gg_duckdb.version()`, and ships the region node end to end: with
`gg_duckdb.mode = force` and `gg_duckdb.debug_wrap = scans` every SeqScan of a
segment slice becomes an identity region (`SELECT c1..cN FROM gg_leaf(0)`)
whose leaf is still the SeqScan, pulled by DuckDB on the backend thread. The
deparser and the real planner pass come with M2; the design is in
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

GUCs (all `gg_duckdb.*`): `mode` (off/auto/force; off by default), `max_memory`
(per-backend cap of DuckDB's memory limit, 512MB), `temp_directory` (default
`base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid>` under each node's data directory),
`max_temp_directory_size`, `release_instance_at_end`. The core GUC
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
cancellation by `statement_timeout`.

## Measured on the development host (DuckDB 1.5.5, 2026-09-09)

Opening the instance costs about 30 ms and 15–20 MB of RSS per backend
(a segment QE grows from ~21 MB to ~40 MB); a 2M-row group-by adds ~4 MB;
the thread count of a QE stays at two (main + interconnect receiver).
