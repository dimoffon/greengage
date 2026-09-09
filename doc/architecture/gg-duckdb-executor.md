# DuckDB as an auxiliary executor for Greengage (`gg_duckdb`)

Status: milestones M0 (foundation) and M1 (identity region end to end) on branch `7.x-duck`. This document is the design
record; the milestone plan and the verified facts behind each decision live with it.

## What it is

Greengage executes every slice with the row-at-a-time PostgreSQL executor plus Motion
nodes. `gg_duckdb` embeds [DuckDB](https://duckdb.org) in each backend as a second,
vectorised executor for the *segment-local, Motion-free* parts of a plan: local joins,
aggregates, sorts and projections over local scans, and later DuckDB-native readers of
Parquet/CSV/JSON files. The standard executor keeps everything Greengage-specific:
Motion, slices, dispatch, DML, InitPlans, partition selection.

Whether a subtree goes to DuckDB is decided in the GPORCA pipeline (a pass over the
finished plan), gated by the core GUC `optimizer_enable_duckdb` and the extension GUC
`gg_duckdb.mode`, steerable by pg_hint_plan hints `DuckDB(...)` / `NoDuckDB(...)`, and
cost-based where both executors could run the subtree.

## Decisions

**D1 — A "DuckDB region" node with PG-executed leaves.** One `CustomScan`
(`GGDuckDBRegion`) replaces a Motion-free interior subtree of a slice. Its leaves stay
ordinary PG plan nodes kept in `custom_plans` (SeqScan, IndexScan, BitmapHeapScan,
DynamicSeqScan, ShareInputScan, SubqueryScan, a Motion *receiver*, anything ineligible)
and are pulled on the backend thread through `ExecProcNode`; its interior (Result
projection, HashJoin/MergeJoin/NestLoop without `nestParams`, Agg, Sort, Limit,
Material, Unique, Append) is executed by DuckDB. This needs no DuckDB catalog, no
`ruleutils` deparser, no direct storage reader; distributed-snapshot visibility and
AO/AOCO column projection come free from the leaves; and a region may sit directly above
a Redistribute/Broadcast receiver, which is where MPP joins and second-phase aggregates
run. A `gg_duckdb` foreign table (D8) is the one leaf that is inlined as a DuckDB-native
source instead of being pulled.

**D2 — Handover as SQL text from our own deparser; "eligible" means "generates".**
Each leaf `i` is a registered table function `gg_leaf(i)`; each interior node a nested
aliased subquery with positional columns `c1..cN`. Constants become prepared-statement
parameters. The deparser enforces the semantics firewall: whitelisted types (bool,
int2/4/8, float4/8, numeric with explicit typmod ≤ 38, text/varchar/bpchar, date,
timestamp, timestamptz, interval, bytea, uuid), a curated operator/function/aggregate map
with bit-exact semantics, explicit `NULLS FIRST/LAST` on every sort key, text ordering
comparisons only under `C`/`POSIX` collation, `LIKE` always with `ESCAPE '\'`, every
division or modulo guarded by `CASE WHEN divisor = 0 THEN error('division by zero')`
because DuckDB returns NULL or infinity where PostgreSQL raises an error. Anything
outside the whitelist makes the subtree ineligible; nothing is ever approximated.

**D3 — DuckDB's C API only, driven on the backend thread, `threads = 1`.** The instance is
opened with `threads = 1` and `external_threads = 1`, so DuckDB owns no thread and every
unit of work, table-function callbacks included, runs inside
`duckdb_pending_execute_task()` on the backend thread. Nothing in the backend is
callable off its main thread (buffer manager, memory contexts, elog, the vmem tracker),
and this is what makes the leaf callbacks legal. Every host callback is a `PG_TRY`
barrier: a PG error becomes `duckdb_function_set_error`, never a `longjmp` through DuckDB
frames; DuckDB errors surface from the task loop and are re-raised with `ereport`. The
C API is the stable client surface across DuckDB minors and the 2.0 ABI freeze, works
with a prebuilt `libduckdb.so`, and keeps the extension in C. It costs the custom
allocator hook (C++ only), which is why memory is budgeted rather than tracked per
allocation (D5). In an MPP with several primaries per host, one thread per QE is the
right default; multi-threaded regions are a later opt-in.

**D4 — A post-planner pass called from one core hook.** `post_planner_hook` fires at the
end of `standard_planner()` on both the ORCA branch and the PostgreSQL-planner branch.
Both deliver the same state there: slices numbered, plan node ids assigned, setrefs-form
Vars, pg_hint_plan's `Set()` hints live. The pass walks with `plan_tree_mutator`, tracks
the slice through `Motion.motionID`, and replaces each maximal eligible region that
passes the gate. Boundary rules that matter for correctness: a join whose inner subtree
holds a PartitionSelector feeding a `Dynamic*Scan` on the outer side is never interior
(DuckDB gives no build-side-first guarantee); a Material that shields a Motion from
rescans stays a boundary; a Motion leaf is allowed only when the region has no external
params. The gate needs at least one join/aggregate/sort/unique operator, a minimum row
count, and a PostgreSQL-unit cost model over per-segment row estimates (ORCA's
`total_cost` is in ORCA units and is not compared); `gg_duckdb.mode = force` bypasses
the gate, never eligibility.

**D5 — Memory budgeted up front.** The region is memory-intensive/blocking for
`memquota.c` through two new `CustomScan.flags` bits, so it receives a Sort-like share
of the query's memory; the executor reserves that budget with the vmem tracker at region
start and sets DuckDB's `memory_limit` to it. DuckDB documents that some intermediates
escape its buffer pool, so this is honest but coarse; per-allocation accounting waits
for the DuckDB 2.0 C++ layer. Spills go under each node's `base/pgsql_tmp`.

**D6 — Hints and GUCs.** `DuckDB(t1 t2)`, `NoDuckDB(t1)`, `DuckDB()`/`NoDuckDB()` are a new
hint *type* in pg_hint_plan (invisible to ORCA's own hint loops, so no fallback), read by
the pass through `plan_hint_hook`, which is now defined in `planner.c` and available
with or without ORCA. Extension GUCs that QEs must see carry `GUC_GPDB_NEED_SYNC`.

**D7 — GPORCA's plan shape is unchanged for now.** Memo-level DuckDB operators, so that
ORCA can choose a different shape because DuckDB is cheaper, are a later milestone
gated on a measured ≥2x from the pass phase and one documented query where a different
shape wins.

**D8 — DuckDB-native readers as foreign tables.** A `gg_duckdb` foreign data wrapper with
`mpp_execute 'all segments'` foreign tables over Parquet/CSV/JSON files: standalone, its
`ForeignScan` is a region with no PG leaves; inside a region it is inlined as
`read_parquet(...)` so filters, joins and aggregates push into DuckDB with row-group
skipping. Files are sharded deterministically by `hash(path) % numsegments`.

## Core changes (all small, behaviour-neutral without the extension)

- `execAmi.c`: `ExecSquelchNode` handles `T_CustomScanState` (shutdown callback, then
  the `custom_ps` children). Motion senders and Limit squelch their children; the old
  switch errored out on a custom scan.
- `cdbplan.c`: `plan_tree_mutator` mutates `custom_plans/custom_exprs/custom_private/
  custom_scan_tlist`, as `plan_tree_walker` already walked them.
- `execProcnode.c`: `planstate_walk_kids` visits `custom_ps` (EXPLAIN ANALYZE stats).
- `memquota.c` + `nodes/extensible.h`: `CUSTOMSCAN_GP_MEMORY_INTENSIVE`,
  `CUSTOMSCAN_GP_BLOCKING`.
- `planner.c`/`planner.h`: `post_planner_hook`; `plan_hint_hook` moved here from ORCA's
  `COptTasks.cpp` and its declaration in `orca.h` out of `#ifdef USE_ORCA`
  (`pg_hint_plan` installs it unconditionally).
- `guc_gp.c`: `optimizer_enable_duckdb` (unsynced; planning happens on the QD).
- Build: `configure --with-duckdb=PREFIX`, `gpcontrib/Makefile` skips the extension
  without it; `libduckdb.so` is installed into `$GPHOME/lib`.

## Milestones

M0 foundation (this) — M1 identity region end to end — M2 deparser, plan-time
validation, the pass and its GUC gate — M3 joins, Append, two-phase aggregates,
Motion/ShareInputScan leaves — M4 hints, cost calibration, benchmarks — M5 native readers
(FDW) — M6 hardening and opt-ins (InitPlan params, float aggregates, threads > 1,
per-allocation accounting, DuckDB 2.0). Out of scope: DuckDB tables, DDL or writes
through DuckDB, MotherDuck, runtime fallback to the original subtree, direct heap/AO
page reading.

## M1 findings (2026-09-09)

- The region node, `gg_leaf` and both conversions work end to end under both
  optimizers; the identity test round-trips every carried type with NULLs and
  extremes, including toasted text, `-0`, NaN, infinities of date/timestamp and
  38-digit numerics.
- The leaf must execute in the context its parent would have used: running
  `ExecProcNode` under the batch context freed the SeqScan's lazily allocated
  scan descriptor at the next batch reset and crashed segments under Sort and
  under a rescan. Only the value conversion runs in the batch context.
- The synthetic range-table entry for `custom_scan_tlist` is a named tuplestore
  with column lists, as in pg_duckdb; `RTE_RESULT` expands to no columns and
  breaks EXPLAIN VERBOSE.
- DuckDB reports errors raised inside a streaming fetch through the result's
  error state, so a runtime error (integer overflow) is an error, never a short
  result; cancellation through the leaf's `CHECK_FOR_INTERRUPTS` keeps the
  original message.

## M0 findings (2026-09-09, DuckDB 1.5.5)

- Opening an instance: ~30 ms, ~15–20 MB RSS per backend (a segment QE goes from ~21 MB
  to ~40 MB); a 2M-row group-by adds ~4 MB; the QE's thread count stays at two (main +
  interconnect receiver): DuckDB adds none. A 2M-row group-by is 32 cooperative tasks; a
  1M-row streaming fetch is 489 chunks of 2048 rows; no `NO_TASKS_AVAILABLE` stalls.
- Semantics matrix, PostgreSQL vs DuckDB: integer `/` and `%`, numeric rounding (half
  away from zero), `-0.0`/`0.0` and NaN equality, ordering and hash grouping, date +
  interval, NULL ordering, `count(DISTINCT)`, `bool_and` agree. Different, hence excluded
  or guarded: `round(float8)` (half-to-even vs half-away), `substring` with a negative
  start, `upper('ß')`, `LIKE` without explicit `ESCAPE`, text `<`/`min` under
  `en_US.utf8`, numeric `/` (DuckDB returns a double), division and modulo by zero
  (NULL / infinity vs error), float overflow (infinity vs error).
- Build traps: DuckDB's Makefile must receive `CMAKE_VARS`, `DUCKDB_EXTENSIONS` and
  `OVERRIDE_GIT_DESCRIBE` through the environment, not the make command line, or its own
  additions (`-DBUILD_EXTENSIONS`, the version stamp) are lost; strict overcommit on the
  build host limits parallel C++ jobs.
