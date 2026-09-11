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
source instead of being pulled. A `Result` whose projection or filter DuckDB cannot
compute (a function outside the whitelist) becomes a leaf too, so the region goes on
above it; ORCA emits such a `Result` for most computed expressions.

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
barrier holds only PostgreSQL work: no DuckDB call that can allocate runs inside it,
because DuckDB reports an allocation failure under its memory limit as a C++ exception
that unwinds through the extension's C frames to DuckDB's own handler, and one that
crossed a `PG_TRY` block would leave the backend's exception stack pointing into a dead
frame (M6 findings). The C API is the stable client surface across DuckDB minors and the 2.0 ABI freeze, works
with a prebuilt `libduckdb.so`, and keeps the extension in C. It costs the custom
allocator hook (C++ only), which is why memory is budgeted rather than tracked per
allocation (D5). In an MPP with several primaries per host, one thread per QE is the
right default; multi-threaded regions are a later opt-in.

**D4 — A post-planner pass called from one core hook.** `post_planner_hook` fires at the
end of `standard_planner()` on both the ORCA branch and the PostgreSQL-planner branch.
Both deliver the same state there: slices numbered, plan node ids assigned, setrefs-form
Vars, pg_hint_plan's `Set()` hints live. The pass walks with `plan_tree_mutator`, tracks
the slice through `Motion.motionID`, and replaces each maximal eligible region that
passes the gate; the statement's subplans (InitPlans and correlated subqueries) are
walked the same way in the slices `subplan_sliceIds` names. Boundary rules that matter
for correctness: a join whose inner subtree holds a PartitionSelector feeding a
`Dynamic*Scan` on the outer side is never interior (DuckDB gives no build-side-first
guarantee); a Material that shields a Motion from rescans stays a boundary, and so does
any Material over a Motion when the region has external parameters, since the region
will be rescanned and the executor's Material is what buffers the Motion's rows; a
Motion leaf is allowed only when the region has no external params. Executor parameters
(an InitPlan's result, a correlated subquery's outer values, a generic plan's arguments)
are bound as DuckDB parameters when the query starts, and a rescan binds them again
into the kept prepared statement. The gate needs at least one join/aggregate/sort/unique operator, a minimum row
count, and a PostgreSQL-unit cost model over per-segment row estimates (ORCA's
`total_cost` is in ORCA units and is not compared); the same model draws the region's
boundary, each node of the candidate subtree choosing between being an interior operator
and a leaf the executor runs (see the boundary findings); `gg_duckdb.mode = force`
bypasses the gate, never eligibility, and takes eligible regions whole.

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
skipping. Files are sharded deterministically by `hash(path) % numsegments`. The same
tables are written: an `INSERT` buffers each process's rows in its DuckDB instance for
the transaction and DuckDB's `COPY` writes them as one file per segment and transaction
when the transaction commits or prepares there (D9); an Iceberg table reached through
its REST catalog (`location 'namespace.table'` on a server with `iceberg_endpoint`) is
read and written through DuckDB's `ATTACH`, written by the coordinator alone.

**D9 — Writes are buffered in DuckDB and published at commit.** The FDW modify callbacks
append rows through the C API appender into a DuckDB table of the process's instance,
tagged with the subtransaction that inserted them; a savepoint that rolls back deletes
its rows, one that commits hands them to its parent; the transaction's `PRE_COMMIT` or
`PRE_PREPARE` event runs the `COPY` (or the Iceberg `INSERT`) and an abort drops the
buffer. Chosen over writing at statement end because a rolled-back transaction then
writes nothing at all, and over a temp-name-and-rename scheme because object stores have
no rename. What it cannot give: a segment publishes when its part of the distributed
transaction prepares and never sees the coordinator's decision after that, so a rollback
after every segment prepared, or a crash there, leaves files (named with the distributed
transaction id); and the rows of an open transaction are invisible to its own reads.
Iceberg tables are written by one process because DuckDB's Iceberg commits do not detect
each other (three concurrent inserts from three processes left one snapshot and no
error), so the table is `mpp_execute 'coordinator'` and locked for the transaction.

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

M0 foundation — M1 identity region end to end — M2 deparser, plan-time
validation, the pass and its GUC gate — M3 joins, Append, two-phase aggregates,
Motion/ShareInputScan leaves — M4 hints, cost calibration, benchmarks — M5 native readers
(FDW) — M6 hardening and opt-ins (executor parameters, subplans, float aggregates, the
isolation2 suite; threads > 1, per-allocation accounting and DuckDB 2.0 assessed and
deferred) — M7 writes (INSERT into file tables and Iceberg catalog tables). Out of scope: DuckDB tables, DDL or writes
through DuckDB, MotherDuck, runtime fallback to the original subtree, direct heap/AO
page reading.

## Boundary and calibration findings (2026-09-11)

- **The region's boundary is chosen by cost.** The gate used to judge each candidate
  subtree as a whole: the deparser built the maximal region rooted at a node and the pass
  accepted or declined it, so a join that filters a large table through a small one (Q3,
  Q5, Q10, the local top-N join) dragged the aggregate above it down with it, and `auto`
  fell back to the standard executor for the whole subtree. Now, under `mode = auto`, the
  deparser tries every interior-capable node as an interior first and then asks whether
  the executor should keep it instead: the attempt's cost is the conversion of the leaves
  it took plus the balance of the operators it moved to DuckDB (each operator counts
  `op * ((1 + margin) * factor - 1)`, negative when DuckDB is cheaper), the alternative is
  the conversion of the node's own output; when the leaf is cheaper the attempt is undone
  (the same undo the Result fallback of M6 used, generalised: leaves, parameters, readers,
  aliases, estimates) and the node becomes a leaf the executor runs. The objective is
  additive over the tree, so the bottom-up choice is the optimum of the model, and with
  no cut the decision reduces exactly to the old gate. The base of a region is never a
  leaf unless a Sort or Unique above it keeps the region worth having; hints see the
  maximal shape (a `DuckDB()` hint forces it whole, a `NoDuckDB()` hint forbids by its
  scans), so under a hint nothing changes. `gg_duckdb.cost_boundary` (on) switches it;
  `explain_decisions` reports each kept subtree, and the two costs behind the choice go
  to the DEBUG1 log with the gate's other estimates (`bench/run.sh -c` prints them per
  candidate). `min_rows` drops to 10000: a region over a join's output is
  legitimately smaller than the join's input, and the fixed cost is modelled.
- **The first benchmark of the boundary was flat, and the reasons were in the model
  and in the harness, not in the mechanism.** With the M4 constants the boundary carved
  `Limit Sort Agg` regions over the executor's join for l02 and Q3 and they measured as
  a wash and a 4% loss. Three things were wrong. `bench/run.sh` timed each query in a
  fresh session, so every DuckDB run paid the ~30 ms opening of the instance on each QE
  that `off` never paid (the runner now warms the session first; warm, those two regions
  win 7% and 3%). The bounded-sort cost took cost_sort's `n * log2(2N)` comparisons,
  eight times what a top-10 heap over 185k rows does (now `n + N ln(n/N) log2 N`). And the
  conversion and aggregate constants were fitted on 13 queries rather than measured, which
  the next item corrects.
- **Per-value costs, measured** (identity regions over lineitem, 2M rows per segment,
  planner units at about 15 us each): converting a fixed-width value costs 10-17 ns in
  either direction, a text 21 ns in and 130 ns out, a numeric 75 ns in and 380 ns out
  (both go through `numeric_out`/`numeric_in` text: the obvious follow-up is a direct
  digit conversion), the row itself about 6 ns; a region's fixed cost in a warm session is
  3-4 ms (`cost_fixed` 200). On the executor's side, a numeric operator costs 130 ns
  (3.5x `cpu_operator_cost`), a numeric aggregate transition 46 ns in a plain aggregate
  and 250 ns in a hashed one with 500k groups (its state is reallocated per row), and a
  group key 45 ns at 10k groups to 140 ns at 500k. The deparser now charges conversion
  per column and direction, only for the leaf columns the region references (DuckDB
  projects only those into `gg_leaf`; ORCA's scans carry the whole physical target list),
  counts the operators of every expression it deparses (aggregate arguments per input
  row), and weights numeric aggregates and group keys by `log2(groups)`. The constants
  are in `deparse.c` next to the measurements; `cost_convert_factor` scales the
  conversion side for another host. Why it matters: a 2M-row hash aggregate with a
  numeric sum over 500k groups costs the executor 830 ms and DuckDB 250 ms including
  conversion (C5 in the notes), a 2.3x win the old model declined as a loss, which is
  exactly the Q18 miss under the Postgres planner.
- **Benchmark** (`bench/`, SF 1, 3 primaries, medians of 3, warm sessions, ms; `whole`
  is `auto` with `cost_boundary` off, the speedups are off/force and off/auto):

  | query | what the region covers | ORCA off | force | auto | whole | off/auto | planner off | force | auto | whole | off/auto |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | q01 | 8 numeric aggregates over 2M rows/segment | 1625 | 1163 | 1173 | 1173 | 1.39 | 1579 | 1119 | 1117 | 1107 | 1.41 |
  | q04 | semi join + count | 663 | 375 | 389 | 393 | 1.70 | 360 | 346 | 368 | 369 | 0.98 |
  | q13 | left join, count, count of counts | 383 | 218 | 482 | 219 | 0.79 | 336 | 196 | 194 | 195 | 1.73 |
  | q18 | join + large group-by (planner: two joins + the aggregate) | 1578 | 1084 | 953 | 948 | 1.66 | 1633 | 974 | 868 | 857 | 1.88 |
  | l01 | co-located join + 5 aggregates (agg over the executor's join) | 886 | 898 | 896 | 877 | 0.99 | 763 | 860 | 752 | 749 | 1.01 |
  | l02 | co-located join, 77k groups, top-50 (agg + top-N over the executor's join) | 402 | 583 | 388 | 406 | 1.04 | 331 | 547 | 316 | 341 | 1.04 |
  | q03 | 3-way join, top-10 (agg + top-N over the executor's join) | 462 | 549 | 462 | 439 | 1.00 | 408 | 485 | 405 | 393 | 1.01 |
  | q10 | 3-way join, 7 group columns, top-20 (cut region) | 450 | 484 | 447 | 428 | 1.01 | 333 | 440 | 342 | 360 | 0.98 |
  | q12 | join + CASE sums | 357 | 380 | 370 | 378 | 0.96 | 320 | 345 | 330 | 356 | 0.97 |
  | q05 | 6-way join, 9 groups (declined) | 470 | 784 | 495 | 471 | 0.95 | 344 | 651 | 341 | 349 | 1.01 |
  | q06 | filtered sum over 150k rows (declined) | 244 | 258 | 246 | 246 | 0.99 | 240 | 243 | 223 | 226 | 1.07 |
  | q14 | join under a CASE the region declines (declined) | 271 | 287 | 271 | 270 | 1.00 | 228 | 265 | 232 | 238 | 0.98 |
  | q19 | join with OR-of-IN predicates (declined) | 337 | 346 | 324 | 336 | 1.04 | 296 | 314 | 302 | 284 | 0.98 |

  The q13 `auto` outlier under ORCA (482) is a host hiccup: the plan is the same as under
  `whole`, and six consecutive warm runs right after measured 157-216 ms. Under the
  Postgres planner q04's region is a 20k-row aggregate over the executor's semi join,
  accepted by 7% of the estimate and a wash in practice.

- **What the boundary buys, honestly.** On this set the losing whole-join regions are
  declined as before; the boundary adds small wins where an aggregate or top-N sits over
  a selective join (l02, Q3, Q10, Q12) and prunes the rest faster. The large wins stay
  the aggregate-heavy and semi/anti-join regions, and the largest single change on this
  set comes from the measured aggregate costs (Q18 under the planner), not from the
  boundary. Regions that output many numeric or text rows stay expensive until the
  output conversion is fixed.

## M7 findings (2026-09-10)

- **The write path is short because the core already routes.** An `INSERT` into an
  all-segments foreign table sends every row to a segment (randomly, or by the columns
  with `insert_dist_by_key`) and calls `ExecForeignInsert` there, exactly as for writable
  external tables; the wrapper implements the four modify callbacks and the two `COPY
  FROM` ones. DuckDB has no push-style file writer in the C API, so the rows go through
  the appender into a buffer table (the leaf's value writers fill the chunks) and one
  `COPY` per table and transaction writes the file at commit; the buffer is DuckDB's, so
  it spills under the memory budget (100 MB of rows per segment under 64 MB, in the tests).
- **Two DuckDB memory traps in the writer.** The Parquet writer holds a whole row group
  before writing it and DuckDB's default is 122880 rows, which for wide rows is a 128 MB
  block: the row group size is derived from the measured average row width so that a
  group takes an eighth of the budget. And with `preserve_insertion_order` on, DuckDB
  buffers a COPY's batches in memory; the setting is switched off around the commit-time
  writes (the file's order is nobody's business) and restored after.
- **A third on the reader, found by the writer.** DuckDB detects a JSON file's layout
  with a 32 MB buffer it keeps per file, so a table of several JSON files, which every
  written table is, ran out of a 64 MB budget on read. `json_format` names the layout;
  a pattern location defaults to newline-delimited, the layout the writer produces.
- **Savepoints.** Rows carry the segment-local subtransaction id that inserted them
  (`GetCurrentSubTransactionId()` on the QE, whose numbering differs from the QD's but
  is consistent with the QE's own subtransaction events); a rolled-back savepoint's rows
  are deleted from the buffer, a released one's re-tagged to the parent. The first
  version decided whether to write a file from a row counter that a rollback had made
  meaningless, and one segment out of three skipped its file: the count now comes from
  the buffer itself.
- **Iceberg writes are single-writer, measured.** Three CLI processes inserting into one
  catalog table at the same time reported no error, wrote three data files and four
  metadata files, and left a current snapshot with one process's rows. DuckDB's REST
  commit does not carry the requirement that would make the catalog reject a stale
  update. So a catalog table is written by the coordinator only (`mpp_execute
  'coordinator'`; the segments refuse with that hint), with the foreign table locked for
  the transaction so that two sessions of the cluster never commit at once; a writer
  outside the cluster remains that risk. DuckDB's Iceberg writer sizes its Parquet row
  groups itself, so the coordinator's budget for such a write starts at 256 MB.
- **S3 uploads come in 80 MB parts.** The first write of the samples to S3 failed on
  every segment for a 76.5 MB block under the 62.5 MB budget: DuckDB's S3 writer
  uploads in parts of `s3_uploader_max_filesize` (800 GB) over
  `s3_uploader_max_parts_per_file` (10000), one buffer per part in flight (and up to
  50 uploader threads of its own). Writers to S3 set the file limit to 80 GB, so 8 MB
  parts, and two uploader threads; the same block had been misread as the Iceberg
  writer's row group before.
- **Reads through the catalog.** `ATTACH IF NOT EXISTS ... (TYPE iceberg, ENDPOINT,
  AUTHORIZATION_TYPE, TOKEN | CLIENT_ID/CLIENT_SECRET)` per server and backend, redone
  after a `DETACH` when the server's or the mapping's options change, forgotten with the
  instance; the reader is the attached table's name, so no file list is bound. The
  version-guessing setting is not needed there. `IMPORT FOREIGN SCHEMA "namespace"`
  lists the catalog's tables through `duckdb_tables()`.
- **A region must keep an order the query still needs.** The samples ran an
  `ORDER BY region` aggregate on the coordinator and got its groups unordered: ORCA had
  planned a sorted GroupAggregate over a Sort with no Sort above it, the query's ORDER
  BY being satisfied by the aggregate's output order, and the region replaced both with
  a hash aggregate whose order DuckDB could not restore (text under a non-C collation).
  On the segments the merge Gather Motion above had always declined such regions; at
  the top of a plan nothing had. The pass now carries an output-order requirement down
  the tree: the top node needs its order when the query has an ORDER BY, a Sort lifts
  it from its child, a hash join, a hashed aggregate or a plain Motion drop it, a merge
  join, a sorted aggregate, a merge Motion or a Unique impose it, and everything else
  passes it through; a region whose output is not ordered is declined where it holds.
- **`gg_duckdb.query()` is now superuser-only**, as the documents already claimed: with
  a remote prefix in `data_directories` DuckDB's external access is on for the whole
  backend, and the function ran arbitrary DuckDB SQL for any role.

## M6 findings (2026-09-10)

- **Executor parameters are DuckDB parameters.** A `Param` of kind `PARAM_EXEC` (an
  InitPlan's result, dispatched to the segments with the plan, or the outer row's values
  in a correlated subquery) or `PARAM_EXTERN` (a prepared statement's or a PL/pgSQL
  generic plan's argument) deparses as `CAST($n AS T)`, appended to the region's
  parameter list after the constants met so far; the query start reads it from
  `es_param_exec_vals` (running the InitPlan first when its plan sits in this process,
  as `ExecEvalParamExec` does) or from `es_param_list_info` through `paramFetch`, and
  checks the type against the plan's. Where the parameter lands decides what this buys:
  both optimizers push `id > (SELECT ...)` into the scan leaf, where it costs nothing;
  a parameter in a HAVING clause, a join condition or a projection sits in the region
  text. ORCA mostly plans an InitPlan away, into a Broadcast join or a `SubPlan`
  projected by the leaf scan, so the region sees a column. The `params` regress test
  covers the cases under both optimizers, custom and generic plans included.
- **Subplans are walked.** The pass only mutated `stmt->planTree`; InitPlans and
  correlated subqueries (`stmt->subplans`, each rooted in the slice `subplan_sliceIds`
  names) never got regions. They do now. Under the PostgreSQL planner a correlated
  subquery runs on the segments and its region is rescanned once per outer row; ORCA
  puts such subqueries on the coordinator, where regions need `on_coordinator`. For the
  rescan to be legal a Material over a Motion is a leaf whenever the region has external
  parameters (the executor's Material buffers the Motion's rows and rewinds; DuckDB's
  would ask the Motion again), and the region keeps its connection and its prepared
  statement across rescans, binding the new values only — before, every start opened a
  connection without closing the previous one and prepared again. Measured on a scalar
  subquery rescanned 1999 times (planner, 3 segments): 1.24 s with regions, 1.66 s
  without.
- **A crash under the memory limit, and the rule it taught.** A hash aggregate with wide
  string keys under a 16 MB limit segfaulted every QE. The core dump showed `longjmp`
  into a dead frame from inside error handling: DuckDB had thrown its
  `OutOfMemoryException` from the string allocation in the leaf callback
  (`duckdb_vector_assign_string_element_len` goes through the buffer manager's allocator,
  which enforces `memory_limit`), the exception unwound through the callback's `PG_TRY`
  block to DuckDB's handler, `PG_exception_stack` kept pointing at the abandoned
  `sigjmp_buf`, and the `ereport` that reported the DuckDB error jumped into it. The
  leaf callback now has two phases: inside the barrier the rows are pulled and
  converted, fixed-width values written straight into the vectors' memory and strings
  copied into the batch context; outside it the strings are handed to DuckDB. The same
  query now fails with DuckDB's `Out of Memory Error` and the backend goes on (the
  isolation2 `memory` test). The rule is recorded in D3. `max_temp_directory_size`
  does reach the segments' instances (`current_setting` shows it), but no region was
  found to spill at these sizes — a 21 MB sort under a 16 MB limit with a 1 MB cap
  completes — so the temp cap has no test.
- **Regions without columns.** `count(*)` above a subquery makes ORCA keep only the
  rows: the aggregate below projects nothing, and the region's `1 AS c1` placeholder
  was one column where the plan-time check expected none, so such subtrees were
  declined. The placeholder is now the region's one result column, discarded on the
  way out.
- **A projection DuckDB cannot compute is a leaf.** ORCA puts computed expressions
  into a `Result` above the scan, so one function outside the whitelist in a projection
  (`upper(v)` as a group key, say) used to make the whole subtree ineligible, while the
  PostgreSQL planner, which computes the same expression in the scan's target list,
  got a region. The deparser now tries a `Result` as interior and, when that fails,
  undoes the attempt (the leaves, parameters and readers it collected, the aliases and
  costs it took) and deparses the node as a leaf: the executor computes the projection,
  DuckDB does the aggregate or join above it. Under ORCA `upper(v), count(*), sum(id)
  ... GROUP BY 1` is now two regions, one per aggregate phase, with the `Result` as the
  lower one's leaf.
- **ORCA hands a foreign scan one conjunction.** The wrapper pushed a clause only when
  all of it deparsed, and ORCA gives the scan its quals as a single `AND`, so one
  function outside the whitelist kept every qual local under ORCA while the planner,
  whose clauses arrive one by one, pushed the rest. Each conjunct is now pushed or kept
  on its own. Found while verifying pushdown against MinIO: DuckDB pushes the pushed
  quals into the Parquet reader (row groups skipped by their statistics, only their
  byte ranges fetched: two range requests instead of eleven for a filter selecting one
  of ten row groups) and into the Iceberg scan (data files pruned by the manifests'
  bounds); CSV and JSON readers have nothing to push into and read whole files.
- **A walker that crashed on SubPlans.** The PartitionSelector-hazard walkers ran
  `plan_tree_walker` with a context that carried no plan base; the first SubPlan node
  in a join's inner side (ORCA attaches an InitPlan there) dereferenced it. They skip
  SubPlan nodes now: what a subplan selects is its own business.
- **Float aggregates** (`sum`, `avg`, `min`, `max` over float4/float8) are eligible only
  under `gg_duckdb.allow_float_aggregates`, off by default: DuckDB's summation order
  differs, so the last digits of a float sum can differ from the standard executor's;
  `min`/`max` are exact either way but ride on the same switch for simplicity.
- **isolation2.** `gpcontrib/gg_duckdb/isolation2/` drives the extension's fault points
  (`gg_duckdb_before_batch` at every leaf batch, `gg_duckdb_query_start`,
  `gg_duckdb_after_chunk`): `cancel` suspends a segment inside a leaf batch and cancels
  the session (the usual "canceling statement due to user request", instance intact,
  no reopen); `timeout` lets `statement_timeout` fire while a leaf sleeps; `squelch`
  holds one segment inside its first batch while the coordinator's LIMIT is satisfied by
  the others, then releases it, once with a scan leaf and once with a Motion leaf (the
  query completes and the session goes on); `memory` is the out-of-memory case above,
  followed by the same query under a larger limit; `leaf_error` raises a PostgreSQL
  error in the third batch of one segment and sees it verbatim on the client;
  `nested` runs a region on the coordinator whose leaf calls a function whose SPI query
  is a region itself (the backend's counter shows one outer and one inner query per
  row), then a segment leaf calling a function that runs a plain SPI query there. The
  suite restarts the cluster around itself like the regress suite does, and its
  answer files are the same under both optimizers.
- **DuckDB 2.0 has not shipped** (checked 2026-09-10: the newest release is 1.5.5 of
  2026-07-22; no 2.0 tag or release candidate). The pin stays at 1.5.5; the C-API
  surface in use is the table-function registration, prepared statements with bound
  values (LIST included), pending results, streaming chunks, vectors and validity, all
  of which are in the documented stable subset. Per-allocation memory accounting (D5)
  stays deferred with it.
- **`threads > 1` per QE: designed, not built.** DuckDB worker threads would run
  `gg_leaf` callbacks off the backend thread, which is illegal. The shape that works
  keeps every PostgreSQL call on the backend thread: the leaf callback becomes a
  consumer of a bounded queue of finished DuckDB chunks that the backend thread fills
  between its own `duckdb_pending_execute_task` calls (pull, convert, enqueue, with the
  signal-masking discipline of the interconnect receiver thread when a worker must
  wait), `duckdb_init_set_max_threads` opened for the leaf function, and the memory
  budget of D5 split across threads. It is worth building only where a QE owns a host,
  since a demo-style cluster runs several primaries per host and one thread per QE is
  already the parallelism; it stays an opt-in for later.
- **`mode` default.** Still `off`. The gate is calibrated on one host and one data
  set, and D5's accounting is coarse; both are what a default needs before it flips.
  `auto` is the evaluation setting, and every benchmark query of M4 runs within noise
  of the better engine under it. (The 2026-09-11 recalibration
  replaced the fitted constants with measured ones and made the boundary cost-chosen;
  see the boundary findings.)

## M5 findings (2026-09-09)

- **The wrapper.** `gg_duckdb` is a foreign data wrapper whose tables name files (`location`:
  a path, a glob or a list) in Parquet, CSV or JSON. The wrapper carries
  `mpp_execute 'all segments'` as its own option, so every table executes on all segments
  unless a server or table says otherwise; the core maps that to a random-distributed
  policy and a Strewn locus, and ORCA plans it natively through `BuildForeignScan`, which
  calls the wrapper's planning callbacks with a minimal `PlannerInfo`. File-level
  sharding: each executing segment expands the location with DuckDB's `glob()` and keeps
  the files with `hash(path) % segments = its index`; the coordinator or a single
  segment reads everything. `gg_duckdb.foreign_files(regclass)`, an `EXECUTE ON ALL
  SEGMENTS` function, shows the assignment.
- **File access.** DuckDB's external access stays off; `gg_duckdb.data_directories`
  (SUSET, synchronised) lists the directories it may read, applied as DuckDB's
  `allowed_directories` by SQL right after the instance opens (the configuration API
  takes no list, and the list cannot change once external access is off), so a changed
  setting takes effect by reopening the instance, which happens at the next connection
  while no query holds one. The validator refuses locations outside the list at DDL
  time, on the coordinator and on the segments it is dispatched to, and the scan checks
  again at run time; DuckDB itself refuses everything else.
- **One executor, two nodes.** The DuckDB query driver of the region (prepare, bind,
  task loop, streaming chunks, conversion into a slot) moved to `query.c` as
  `GGDuckQuery`, shared by the CustomScan region and the ForeignScan. A result column may
  map to any slot attribute (`colmap`), so a scan fetches only the attributes the query
  uses and leaves the rest NULL, as postgres_fdw does. Column types and counts are
  checked on the executed result, not the prepared statement: DuckDB binds a statement
  whose table function takes a parameter only when it executes.
- **The file list is a parameter.** The scan's SQL is `SELECT * FROM (SELECT CAST("col"
  AS T) AS c<attno>, ... FROM read_parquet($n, ...)) AS n0 [WHERE <pushed quals>]`; the
  executing node binds its file list as a `LIST(VARCHAR)` value after the constant
  parameters. An empty list is a DuckDB error, so a node with no files substitutes an
  empty relation of the same columns instead (or, for a standalone scan, returns
  nothing). The SQL is stored with a token in place of the reader call
  (`__GG_NATIVE_<i>__`), and plan-time validation prepares it with the empty relation.
- **Native leaves.** The region deparser treats a `gg_duckdb` ForeignScan without local
  quals as a native leaf: instead of `gg_leaf(i)` it inlines the scan's query (the quals
  it pushed re-deparsed with the region's parameter numbering, the target list projected
  over the fetched columns), records the reader and the empty relation in the region's
  private data (v4), and the region resolves the files at start on each segment. Such
  rows are not converted on the way in, and the cost gate does not charge them
  (`rows_native`). EXPLAIN shows `DuckDB readers: <table> (<format> <location>)` and the
  label counts them.
- **Statistics and import.** ANALYZE on an all-segments table dispatches
  `gp_acquire_sample_rows` (the mechanism file_fdw uses), and each segment samples its
  own files with DuckDB's reservoir sampling (`USING SAMPLE reservoir(n ROWS)`) and
  counts its rows; the coordinator merges. Without statistics the planner's row estimate
  comes from `parquet_file_metadata` when the coordinator can read the files, else a
  default. `IMPORT FOREIGN SCHEMA "<directory>"` makes a table per file in the directory
  and one per subdirectory (`<dir>/<name>/*.<format>`), with columns from DuckDB's
  `DESCRIBE` mapped to PostgreSQL types (DuckDB `TIMESTAMP` variants to `timestamp`,
  `HUGEINT` to `numeric(38,0)`, uncarried types skipped with a notice).
- **Verified on the demo cluster** under both optimizers: six Parquet parts of one table
  read by three segments, every file by exactly one; every carried type with NULLs equal
  to a PostgreSQL-generated reference through the standalone scan and through regions;
  pushed and local quals; the reader inlined into a partial aggregate region; correlated
  rescans; ANALYZE statistics; IMPORT; refused locations and formats; a table over a
  single file (two segments read nothing). Not in this milestone: row-group-level
  sharding of a single large file, writes through DuckDB.
- **S3 and Apache Iceberg.** `duckdb/build.sh` with `DUCKDB_REMOTE_EXTENSIONS=1` and
  `DUCKDB_VCPKG=<checkout>` links DuckDB's `httpfs`, `avro` and `iceberg` extensions at
  the commits 1.5.5 pins; their native dependencies (OpenSSL, curl, the AWS SDK, avro-c,
  roaring) come from vcpkg at the tag DuckDB pins, as in DuckDB's own extension builds
  (`httpfs` insists on a static OpenSSL, so vcpkg's 3.6 is linked in; the library is
  linked with `--exclude-libs,ALL`, which leaves the 1300 `duckdb_*` C API symbols
  exported and hides the 1600 OpenSSL and curl ones that would otherwise interpose on
  the backend's libssl/libcrypto 3.0). httpfs takes the process environment's
  `http_proxy` as its default and rejected this host's spelling of it at the first
  request; the instance clears it at open and `gg_duckdb.http_proxy` sets one explicitly.
  A server carries `s3_endpoint`, `s3_region`, `s3_url_style`, `s3_use_ssl`, a user
  mapping `s3_access_key_id`, `s3_secret_access_key`, `s3_session_token`; before a scan
  lists its files the wrapper issues `CREATE OR REPLACE TEMPORARY SECRET` per bucket of
  the table's locations in the backend's instance (secrets are instance-wide and stay in
  memory). DuckDB's `allowed_directories` sandbox normalises every entry to a local path,
  so a remote prefix in `gg_duckdb.data_directories` leaves `enable_external_access` on
  for that backend: the wrapper's validator and run-time checks are the guard, and
  `gg_duckdb.query()` is a superuser tool. `format 'iceberg'` reads the table's current
  snapshot with `iceberg_scan(location, allow_moved_paths=true)`; the "file list" of such
  a table is its one location, so on an all-segments table exactly one segment reads it
  and the others substitute the empty relation, while `mpp_execute 'coordinator'` reads
  it on the coordinator. A table read by its directory needs DuckDB's
  `unsafe_enable_version_guessing` (catalog-written tables have no `version-hint.text`;
  DuckDB picks the lexicographically newest `metadata/*.metadata.json`), which the scan's
  connection sets; a metadata file as the location reads exactly that version. Sharding
  an Iceberg table by data file (possible when its snapshot carries no delete files) and
  reading through a REST catalog (`ATTACH ... (TYPE ICEBERG)`) are later refinements.
  Verified against the stack under both optimizers: six Parquet parts read by three
  segments with none read twice, results equal to the reference through the scan and
  through regions, CSV and JSON, the Iceberg table's current snapshot (5900 rows after
  the delete) with quals and in regions, ANALYZE and IMPORT over S3. `test/external/` provides a
  Compose stack (MinIO on 9100, `tabulario/iceberg-rest` on 8181, a pyiceberg job that
  writes six Parquet parts, a CSV, a JSON and the table `demo.events` with two appends
  and a delete) and `run.sh` with the checks against a reference generated in
  PostgreSQL.
- **Trap found on the way.** A query state on the stack with a memory-context reset
  callback pointing at it crashes when the context is deleted after the function returned
  (ANALYZE's sampler): such states are palloc'd in the parent context.

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
- **Benchmark (bench/, TPC-H-shaped, SF 1: 6M lineitem rows, heap, 3 primaries on this
  20-core host, medians of 3, ms).** `off` is the standard executor, `force` every
  eligible region, `auto` the calibrated gate; the speedup is off/force.

  | query | what the region covers | ORCA off | force | auto | speedup | planner off | force | auto | speedup |
  |---|---|---|---|---|---|---|---|---|---|
  | q01 | 8 numeric aggregates over 2M rows/segment | 1630 | 1052 | 1051 | 1.55 | 1562 | 1018 | 1018 | 1.53 |
  | q04 | semi join + count | 675 | 391 | 387 | 1.73 | 351 | 341 | 349 | 1.03 |
  | q13 | left join, count, count of counts | 372 | 223 | 207 | 1.66 | 339 | 201 | 192 | 1.69 |
  | q18 | join + 800k-group sum (both phases) | 1527 | 954 | 899 | 1.60 | 1609 | 846 | 835 | 1.90 |
  | l01 | co-located join + 5 aggregates | 876 | 848 | 882 | 1.03 | 742 | 808 | 740 | 0.92 |
  | q14 | join under a CASE the region declines | 268 | 278 | 258 | 0.97 | 216 | 257 | 218 | 0.84 |
  | q12 | join + CASE sums | 355 | 373 | 347 | 0.95 | 308 | 341 | 303 | 0.90 |
  | q19 | join with OR-of-IN predicates | 322 | 351 | 333 | 0.92 | 280 | 301 | 283 | 0.93 |
  | q06 | filtered sum over 150k rows | 264 | 284 | 246 | 0.93 | 224 | 248 | 223 | 0.91 |
  | q10 | 3-way join, 7 group columns, top-20 | 439 | 488 | 432 | 0.90 | 339 | 430 | 352 | 0.79 |
  | q03 | 3-way join, 185k groups, top-10 | 444 | 550 | 436 | 0.81 | 375 | 507 | 374 | 0.74 |
  | l02 | co-located join, 77k groups, top-50 | 408 | 610 | 411 | 0.67 | 321 | 558 | 321 | 0.58 |
  | q05 | 6-way join, 9 groups | 471 | 748 | 472 | 0.63 | 319 | 653 | 312 | 0.49 |

  The first calibrated run had one misprediction, q03 under ORCA: the gate declined the
  `Limit Sort Join Join Agg` region and then accepted the `Sort Join Join Agg` region
  below the Limit, costing the executor's sort as a full sort while it is a bounded top-N
  sort under the Limit, and DuckDB had to sort and hand back every row. The gate now costs
  a Sort right under a constant Limit as bounded (`GGRegionSpec.parent_limit`), which
  declines it; the table shows the run after that fix. The pattern: DuckDB wins by 1.5-1.9x
  where the interior does much work per input row (many aggregates, semi/anti joins,
  large group-bys) and loses, by up to 2x, where a large input is converted for a join
  that produces little: converting rows into DuckDB (about 30-60 ns per column per row)
  costs as much as the executor's own hash join probe, so a region that only replaces
  joins over big inputs cannot pay for itself.
  This is the expected shape for a pass that leaves the scans to the executor; the
  native readers of M5 remove the conversion on the input side.
- **Calibration and the gate's constants.** For each top-level candidate the pass logs
  rows and bytes in and out and both estimates; the 13 queries above were fitted for a
  decision that accepts exactly the queries with a measured win (`bench/run.sh -c`
  prints the estimates). Perfect separation is reached by many constant sets; the
  shipped one is physically sensible: `cost_fixed 50`, `cost_convert_row 0.001`,
  `cost_convert_byte 0.0003`, `cost_op_factor 0.25`, `cost_margin 0.25` (one planner unit
  is about 15 µs on this host, so a region's fixed cost is under a millisecond and a byte
  converted costs about 4.5 ns). Under `auto` every query then runs within noise of the
  better engine under both optimizers. The constants are per-segment estimates of
  per-segment work, so they should transfer to other cluster sizes; a faster or slower
  host shifts `cost_op_factor` and the conversion constants together, so the decisions
  are less host-dependent than the constants.
- **`gg_duckdb.mode` default.** It stays `off` in this milestone: the gate is fitted on
  13 queries on one host, the executor path is four milestones old, and the hardening of
  M6 (isolation2 cancel/OOM tests, per-allocation memory accounting) has not landed.
  `auto` is the setting for evaluation and is what the numbers above recommend; the
  decision is revisited after M5 and M6.
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
