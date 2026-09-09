# DuckDB as an auxiliary executor for Greengage (`gg_duckdb`)

Status: milestones M0 (foundation), M1 (identity region) and M2 (deparser and planner pass) on branch `7.x-duck`. This document is the design
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

## M4 findings (2026-09-09)

- **Hints.** `DuckDB(t1 t2 ...)` and `NoDuckDB(t1 t2 ...)` are a new hint type in
  pg_hint_plan (`HINT_TYPE_DUCKDB`, keywords `HINT_KEYWORD_DUCKDB`/`NODUCKDB` before
  `HINT_KEYWORD_UNRECOGNIZED`, `NUM_HINT_TYPE 7`, `DuckDBHint {base, nrels, relnames,
  negative}`, `hstate->duckdb_hints`), mirrored in `src/include/optimizer/hints.h`, which
  now compiles as C (`extern "C"` under `__cplusplus`) so the extension can include it.
  ORCA reads the per-type arrays it knows and never sees the new type: with
  `optimizer_trace_fallback = on` no fallback appears. The pass obtains the parsed hints by
  calling `plan_hint_hook(parse)` exactly as ORCA does. A region is addressed by the
  aliases of its scan leaves (`rte->eref->aliasname` through `scanrelid`); `DuckDB(S)`
  forces every eligible region covering S, which in a top-down pass is the maximal one,
  `NoDuckDB(S)` forbids every region touching S and wins, an empty list means the whole
  query. Hints beat the cost gate, never make an ineligible subtree eligible, and do
  nothing under `mode = off`; an unhonoured `DuckDB` hint is a NOTICE, or an ERROR under
  `gg_duckdb.strict`. The core `planhints`/`rowhints`/`joinhints` tests pass unchanged
  under both optimizers. pg_hint_plan's own suite does not pass in this tree on this host
  before or after the change (`UNIQUE index must contain all columns in the distribution
  key` in its `init` test), and its copied `make_join_rel.c` errors on `NOT IN` joins
  (`unrecognized join type: 6`) whenever a hint is present under the Postgres planner: a
  pre-existing limitation, so the hint tests avoid `NOT IN`.
- **Cost gate.** Under `mode = auto` the deparser accumulates a PostgreSQL-unit estimate
  of the interior operators the way costsize.c charges them (`cpu_operator_cost` per input
  row per aggregate or hash clause, `cpu_tuple_cost` per output row, `2·cpu_operator_cost·
  n·log2 n` for a sort) and the pass compares it with `cost_fixed + op_cost·cost_op_factor
  + (rows in + out)·cost_convert_row + (bytes in + out)·cost_convert_byte`; DuckDB wins
  when its estimate times `1 + cost_margin` is below the executor's. `min_rows` stays as a
  floor. The estimates are logged at DEBUG1 per candidate; `bench/run.sh -c` prints them.
- **Exact numeric arithmetic.** `+`, `-` and `*` on numeric operands are deparsed with the
  DECIMAL shape DuckDB derives (`w1+w2, s1+s2` for a product, `max(w1-s1, w2-s2)+max(s1,s2)+1`
  for a sum; an integer operand counts as DECIMAL of its digits) and an explicit CAST; a
  product wider than 38 digits is declined. Both engines compute these exactly, so only the
  shape had to be derived; the display scale of PostgreSQL's result equals the derived one.
- **Packed numeric aggregate states.** `sum(numeric)` and `avg(numeric)` serialise an
  `internal` state (`numeric_avg_serialize`: N, sumX as `numeric_send`, maxScale,
  maxScaleCount, NaNcount) between the phases. The partial phase in a region computes
  `struct_pack(s := CAST(sum(x) AS DECIMAL(38,scale)), n := count(x))`; the region packs
  the struct into that state on output (maxScale = the derived scale, maxScaleCount = N,
  NaNcount = 0; the sum NULL when N = 0), so PostgreSQL's final phase combines it as if
  PostgreSQL had produced it. A final `sum` in a region unpacks the states of its Motion
  leaf into the same struct and adds the sums; a final `avg` stays on the executor (its
  division has PostgreSQL's scale rules). The scale of a state column is derived from the
  partial Aggref's argument through the plan below the leaf, which a segment cannot
  repeat once that subtree has itself become a region: the region's `custom_private` (v3)
  therefore records every leaf column's DuckDB shape, and the segment only cross-checks
  them against the tuple descriptors. A NaN in a state is refused with an error.
- **EXPLAIN VERBOSE above a partial region.** ruleutils resolves a final-phase Aggref's
  argument down the plan and insists on finding the partial Aggref
  (`get_agg_combine_expr`). A region's `custom_scan_tlist` therefore describes an output
  that carries a simple or partial aggregate by a copy of that Aggref whose input Vars
  point at extra, display-only columns of the synthetic range table entry (named after
  the scanned attributes). EXPLAIN VERBOSE shows `Output: (PARTIAL sum(gg_duckdb_region.price
  ...))` for such regions.

## M3 findings (2026-09-09)

- The region grammar grows to `base := Join(base, base) | Append(base...) |
  Result(base) | Agg(base) | Material(base) | leaf`. A join is `SELECT <tlist> FROM
  (outer) AS a <kind> JOIN (inner) AS b ON <hash/merge clauses AND joinqual> [WHERE
  qual]`; the Hash node of a hash join and the Sorts of a merge join are dropped, so
  DuckDB picks its own build side and join order. SEMI and ANTI joins map onto DuckDB's
  `SEMI JOIN`/`ANTI JOIN`. Append is a `UNION ALL` of its children when their column
  lists agree in DuckDB type, width and scale.
- Both optimizers leave `HashJoin.hashqualclauses` empty for an equality join and fill
  it only for `IS NOT DISTINCT FROM` joins. A join stays a leaf when it is a `NOT IN`
  join, a deduplicating variant, a parameterised nested loop, or when a
  PartitionSelector on its inner side prunes dynamic scans on its outer side (the
  executor guarantees the build side runs first; DuckDB does not). A leaf join is still
  a leaf of the region above it, so `Agg` over such a join is a region.
- Two-phase aggregates: the partial phase (`AGGSPLIT_INITIAL_SERIAL`) is a region when
  every transition value is a plain value (`count`, `sum` of `int2/int4`, `min`, `max`,
  `bool_and/or`); the final phase (`AGGSPLIT_FINAL_DESERIAL`) above the Redistribute
  Motion is a region that sums the partial counts and sums and takes min/max of the
  partial extremes. The planner keeps `aggstar` set on a final `count(*)` whose single
  argument is the partial count; ORCA clears it. `sum(int8)`, `sum(numeric)` and `avg`
  serialise an `internal` state as `bytea` between the phases and stay on the executor.
- A leaf that projects nothing (a join or scan under `count(*)`) is given a placeholder
  BOOLEAN column: DuckDB requires a table function to return at least one column, and
  with projection pushdown it asks for column 0 when it needs none.
- DuckDB invalidates its whole database instance after an `INTERNAL` error during
  execution (`ClientContext::ExecuteTaskInternal`) but not during `duckdb_prepare`. The
  connection that follows any DuckDB error probes the instance with `SELECT 1` and
  reopens it when it was invalidated (`gg_duckdb.status()` counts `reopens`).
- After a region is made the pass visits every leaf: a Motion leads to another slice,
  a SubqueryScan, ShareInputScan or leaf join has a subtree of its own, and each may hold
  further regions. Decision notices are emitted only for nodes that can be a region root
  (joins, Agg, Sort, Unique, Limit, Result, Material, Append).
- Shared CTEs work in both optimizers with ShareInputScan producers and consumers as
  leaves; the planner's `gp_cte_sharing = on` shape puts the producer under a
  SubqueryScan leaf, where the aggregate below it becomes a region.

## M2 findings (2026-09-09)

- The region grammar that ships: `[Limit] [Unique] [Sort] base`, hoisted into one
  `SELECT [DISTINCT] * FROM (base) ORDER BY ... LIMIT ... OFFSET ...`, with `base :=
  Result(base) | Agg(base) | Material(base) | leaf`. A sorted Agg is absorbed as a hash
  `GROUP BY` and its output order restored by an outer `ORDER BY` when every sort key is
  projected; otherwise the region is unordered and a parent that needs order declines it.
- ORCA represents `count(*)` as an aggregate with an empty argument list and `aggstar`
  unset; both optimizers deliver interior Vars in setrefs form, which the deparser relies on.
- DuckDB binds a statement with parameters when it is first executed, not when it is
  prepared, so the leaf table function's bind context must stay in place from prepare
  through the start of execution.
- memquota grants a node that is not memory intensive about 100 KB; DuckDB cannot run
  under such a limit, hence `gg_duckdb.min_memory` (64 MB) as the floor of a region's
  memory limit, which is reserved with the vmem tracker while the region runs.
- gpdiff's `matchsubs` do not apply to NOTICE lines, so decision notices carry no row
  estimates (they go to the DEBUG1 log instead).
- Aggregate outputs of unconstrained `numeric` can be region outputs (DuckDB's DECIMAL
  shape is recorded in the plan) but not leaf columns: a subtree whose leaf yields such a
  column is declined. Numeric arithmetic, `avg`, float sums and `AGG_SORTED` without a
  covering Sort are declined in this milestone.

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
