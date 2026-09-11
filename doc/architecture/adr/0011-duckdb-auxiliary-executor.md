# ADR-0011: DuckDB as an Auxiliary Executor for Segment-Local Plan Regions

- **Status:** Accepted — implemented on branch `7.x-duck` as the extension
  `gpcontrib/gg_duckdb` (milestones M0–M7; latest commits 48cff318611 and 656334dcd30).
- **Date:** 2026-09-11
- **Companion document:** `doc/architecture/gg-duckdb-executor.md` holds the design
  record with the per-milestone findings, measurements and traps. This ADR is the
  self-contained statement of *what* was decided, *how* a plan is rewritten and executed,
  and *what is and is not supported*. Where the two disagree, the code wins and this
  document is what to fix.
- **Numbering:** continues the DR series of `7.x-dr` (ADR-0001 to ADR-0010) so that the
  two branches can meet without renumbering.

---

## Context

Greengage runs every slice of a plan with the row-at-a-time PostgreSQL executor plus
Motion nodes for data movement. On analytical workloads most of the time goes into the
*segment-local* part of a slice: hash joins, hash aggregates, sorts and projections over
local scans, all of which a vectorised engine does several times faster. DuckDB is such an
engine, embeddable as a library, with a stable C API and a SQL dialect close enough to
PostgreSQL's that a plan fragment can be expressed as a DuckDB query.

The question was how to use it without giving up anything Greengage does that DuckDB
cannot: Motions, slices and gangs, dispatch, distributed transactions, DML, InitPlans and
correlated subplans, partition selection, resource groups and vmem accounting, both
optimizers (GPORCA and the Postgres planner), PostgreSQL's type and collation semantics,
and exact results. Four constraints shaped the answer:

1. **Results must equal the standard executor's** for every query that is rewritten:
   same types, same rounding, same NULL and error behaviour (a division by zero must
   raise, not return NULL), same text comparison semantics.
2. **No DuckDB-owned threads in a backend.** A Greengage host runs several primaries and
   their QEs; one thread per QE is the parallelism. DuckDB callbacks into the backend
   (reading rows from a PostgreSQL plan node) are only legal on the backend's thread.
3. **Memory must be accounted the Greengage way** (memquota shares, the vmem tracker,
   resource groups) even though the DuckDB C API exposes no allocator hook.
4. **Opt-in, behaviour-neutral when off**, and a small, reviewable footprint in the core.

---

## Decisions

### D1 — DuckDB is embedded in each backend through its C API, one instance per process, driven only by the backend thread

The extension is pure C against `libduckdb.so` (pinned: 1.5.5, built by
`gpcontrib/gg_duckdb/duckdb/build.sh` with the `icu`, `json`, `parquet`,
`core_functions` and, when asked, `httpfs`, `avro` and `iceberg` extensions statically
linked; `configure --with-duckdb=PREFIX`; the library is installed into `$GPHOME/lib`).
It must be in `shared_preload_libraries`: the region plan node is resolved by name on the
QE that receives the plan, so every process must know it.

Each process (coordinator and every QE) opens its own DuckDB instance lazily on first use
(`src/instance.c`) and keeps it for the session unless
`gg_duckdb.release_instance_at_end` is on. The instance is configured with `threads = 1`
and `external_threads = 1`, so DuckDB never owns a thread: every task, table-function
callback included, runs on the backend thread that calls
`duckdb_pending_execute_task()`. `memory_limit` is `gg_duckdb.max_memory` (512 MB;
changed with `SET GLOBAL` when the GUC changes), `temp_directory` is
`base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid>` under the node's data directory unless
`gg_duckdb.temp_directory` says otherwise, `max_temp_directory_size` is passed through,
external file access is off except for `gg_duckdb.data_directories` (applied as
`allowed_directories`; a remote prefix such as `s3://` keeps external access on), and the
HTTP proxy comes from `gg_duckdb.http_proxy`, never from the process environment. An
instance that DuckDB invalidates after an INTERNAL error is reopened on next use.

**Why the C API.** It is DuckDB's stable client surface across minor versions and the
announced 2.0 ABI freeze, works with a prebuilt shared library, and keeps the extension in
C. **Accepted costs:** the C++-only allocator hook is out of reach, so memory is budgeted
rather than tracked per allocation (D6); and a DuckDB call that allocates can throw a C++
exception under the memory limit, which must never unwind through a `PG_TRY` block — the
leaf callback is therefore two-phase (rows are pulled and converted inside the barrier,
strings are handed to DuckDB outside it), and this rule is load-bearing (a crash was found
and fixed in M6).

### D2 — Integration is a post-planner pass that replaces Motion-free, segment-local subtrees with a custom plan node; the optimizer's plan shape is not changed

One hook, `post_planner_hook`, fires at the end of `standard_planner()` on both the ORCA
branch and the Postgres-planner branch, at a point where both deliver the same state:
slices numbered, `plan_node_id`s assigned, Vars in setrefs form, pg_hint_plan's `Set()`
hints live. The pass (`src/pass.c`) walks the finished `PlannedStmt` and replaces eligible
subtrees with a `CustomScan` named `GGDuckDBRegion` — a *region* — whose leaves are the
original plan nodes at its boundary. Everything Greengage-specific stays with the standard
executor: Motion senders and receivers, slices, dispatch, DML, InitPlans, partition
selectors, ShareInputScans.

**Why here and not in the optimizer.** One site covers both optimizers with identical
semantics; ORCA's plan is taken as the best MPP shape and only its local execution changes;
nothing in the optimizers had to learn a new operator. **Accepted costs:** ORCA cannot
prefer a plan shape because DuckDB would run it cheaper (memo-level DuckDB operators are a
deferred milestone with measured preconditions), and a region can only be as large as the
optimizer's segment-local subtree — its leaves are where the executor's rows are converted,
which is the dominant cost of the whole design.

### D3 — Eligibility is deparsability: one walk both decides and emits the DuckDB SQL, from a whitelist whose semantics are known to equal PostgreSQL's

The deparser (`src/deparse.c`) turns a candidate subtree into one DuckDB `SELECT`. It
knows a fixed set of plan node kinds, types, operators, functions and aggregates; every
generated expression is wrapped in a `CAST` to the DuckDB shape of its PostgreSQL type;
constants become bound parameters; a node it cannot express becomes a leaf (an ordinary
plan node pulled through a table function) or rejects the region. There is no separate
"is this supported" analysis that could drift from the generator. Before a region is
planned its query is prepared on the coordinator's DuckDB
(`gg_duckdb.validate_at_plan_time`, on by default) and its result columns checked against
the expected types, so a query DuckDB cannot bind never leaves the planner: the subtree
stays on the standard executor with a reason (`gg_duckdb.explain_decisions`).

### D4 — Leaves are PostgreSQL plan nodes pulled through a DuckDB table function; native readers are the one exception

A region's leaves (`cs->custom_plans`) are initialised as ordinary child PlanStates and
read by the DuckDB table function `gg_leaf(i)` (`src/leaf.c`), which pulls up to one
vector (2048 rows) per call with `ExecProcNode`, converts fixed-width values straight into
the vectors and stages strings for DuckDB, and declares projection pushdown so only the
columns the region's SQL references are converted. This keeps every executor feature at a
leaf for free (scans of every kind, partition pruning, Motion receivers, index scans,
foreign scans) at the price of converting rows. The exception is a scan of a `gg_duckdb`
foreign table (D7) without local quals: the deparser inlines it as a DuckDB reader call
(`read_parquet(...)`, `read_csv(...)`, `read_json(...)`, `iceberg_scan(...)`) over the
files the executing segment owns, so such leaves convert nothing.

### D5 — A cost gate with measured constants decides, per node, what goes to DuckDB

`gg_duckdb.mode` is `off` (the pass does nothing), `force` (every eligible region, whole —
the setting for tests) or `auto`. Under `auto` a PostgreSQL-unit cost model compares the
standard executor's estimate of the operators a region would take over with DuckDB's
estimate of the same plus the conversion of the rows crossing the boundary, and the same
model chooses *where the boundary lies*: every interior-capable node is tried as an
interior and kept by the executor instead when converting its output is cheaper than
converting its inputs plus what DuckDB saves on its operators. The constants are measured
on the development host (identity regions and aggregate probes; the numbers sit next to
the constants in `deparse.c`) and scaled by GUCs; `DuckDB()`/`NoDuckDB()` hints beat the
gate. The details are in "How the plan is rewritten" below. **Why measured rather than
fitted:** the first, fitted constants separated 13 benchmark queries perfectly and still
mispredicted the next run; per-value and per-operator measurements transfer.

### D6 — Memory is budgeted up front, not tracked per allocation

A region carries `CUSTOMSCAN_GP_MEMORY_INTENSIVE` and `CUSTOMSCAN_GP_BLOCKING` when it
contains an aggregate, sort or distinct, so memquota gives it a Sort-like share of the
query's memory. At start the region takes that share (`PlanStateOperatorMemKB`), raised
to `gg_duckdb.min_memory` (64 MB) and capped by `gg_duckdb.max_memory` (512 MB), reserves
it with the vmem tracker (`gg_duckdb.reserve_memory`) and releases it at the end; the
instance's `memory_limit` bounds DuckDB's buffer pool, and spills go to the per-node
temporary directory. **Accepted cost:** DuckDB documents that some intermediates escape
its buffer pool, so the accounting is honest but coarse; per-allocation accounting waits
for a C++ allocator hook (DuckDB 2.0).

### D7 — Lake data comes in through a foreign data wrapper executed on all segments

`CREATE FOREIGN DATA WRAPPER gg_duckdb ... OPTIONS (mpp_execute 'all segments')` is in the
extension script. A foreign table names files (`location`: a path, a glob, a list, or an
S3 URL; `format` parquet, csv, json or iceberg; for an Iceberg REST catalog `location
'namespace.table'` with `iceberg_endpoint`/`iceberg_warehouse`/`iceberg_auth` on the
server). Each file of an all-segments table is read by exactly one segment
(`hash(file) % nseg`, decided by DuckDB inside the reader query), deparsable conjuncts are
pushed into the reader SQL (DuckDB then skips Parquet row groups by their statistics and
prunes Iceberg data files by manifest bounds), `ANALYZE` samples through
`gp_acquire_sample_rows_func`, `IMPORT FOREIGN SCHEMA "<directory>"` makes one table per
file or subdirectory (or per catalog table). Credentials are `CREATE USER MAPPING` options
(`s3_access_key_id`, `s3_secret_access_key`, `s3_session_token`, `iceberg_token`,
`iceberg_client_id`, `iceberg_client_secret`) turned into DuckDB temporary secrets per
backend. Inserts are buffered in the process's DuckDB and written at commit or prepare:
one file per segment and transaction for file tables (the location must be a pattern with
one `*`), one `INSERT` and one snapshot from the coordinator for Iceberg catalog tables,
which are therefore `mpp_execute 'coordinator'` and locked for the transaction, because
DuckDB's Iceberg commits do not detect concurrent writers.

### D8 — What stays out

DuckDB tables, DDL or writes through DuckDB, MotherDuck, runtime fallback to the original
subtree once a region has started, direct reading of heap or append-optimized pages by
DuckDB, DuckDB worker threads (`threads > 1` is designed, not built), and ORCA memo-level
operators (gated on a measured ≥2x from the pass and one documented query where a
different shape would win).

---

## How the plan is rewritten

All of this is `src/pass.c` (the walk, the gate, the region node) and `src/deparse.c`
(eligibility and SQL), with `src/hints.c` for hints.

1. **When.** The hook runs on the coordinator only (`Gp_role == GP_ROLE_DISPATCH`), for
   plans whose command is `SELECT`, when `gg_duckdb.mode` is not `off`. Plans of
   `INSERT`/`UPDATE`/`DELETE`, including their SELECT parts, are not rewritten. (The core
   GUC `optimizer_enable_duckdb` exists for the deferred ORCA milestone and is not yet
   consulted by the pass.)

2. **Where.** The pass walks the plan top-down with `plan_tree_mutator`, tracking the
   slice through `Motion.motionID`; the statement's subplans (InitPlans and correlated
   subqueries) are walked the same way in the slices `subplan_sliceIds` names. A node is a
   candidate root only in a slice that runs on segments (primary reader/writer or
   singleton reader gangs); coordinator slices, and subplans that are never dispatched,
   only under `gg_duckdb.on_coordinator` (off). Only node kinds the deparser can make
   interior are tried as roots: `Agg`, `Sort`, `Unique`, `Limit`, `Result`, `Material`,
   `HashJoin`, `MergeJoin`, `NestLoop`, `Append`.

3. **The shape of a region.** From a root, the deparser peels a *top chain* — `[Limit]
   [Unique] [Sort]`, Materials in between transparent, each node merely passing columns
   through — and then a *base*: `Result(base)`, `Agg(base)`, `Material(base)`,
   `Join(base, base)`, `Append(base, ...)`, or a leaf. The top chain becomes `SELECT
   [DISTINCT] * FROM (base) ORDER BY ... LIMIT n OFFSET m` (constant limits only, `Unique`
   over all columns only); every interior node becomes a subquery `SELECT <expressions> AS
   c1..cN FROM ...` with `WHERE`/`GROUP BY`/`HAVING`, joins as `JOIN`/`LEFT JOIN`/`RIGHT
   JOIN`/`FULL OUTER JOIN`/`SEMI JOIN`/`ANTI JOIN ... ON <clauses>`, an `Append` as `UNION
   ALL`. The `Hash` node of a hash join and the `Sort` children of a merge join or of a
   sorted aggregate are dropped: DuckDB picks its own join and grouping strategy. A sorted
   `Agg` whose output order the query still needs is given an `ORDER BY` over its output
   when every sort key is among the output columns; otherwise the region is unordered and
   is declined wherever the parent needs the order (`order_required`, propagated from the
   query's `ORDER BY` and per node kind).

4. **Interior or leaf.** A node is interior only when its rules hold: a join must have
   two inputs, no InitPlan, a join type among inner/left/right/full/semi/anti (never `NOT
   IN` nor the deduplicating variants), a nested loop must have no parameters and no
   shared or singleton outer, a hash join must have plain hash clauses (`IS NOT DISTINCT
   FROM` joins stay out), and a join whose inner side holds a PartitionSelector that
   prunes a dynamic scan on its outer side is never interior (DuckDB gives no
   build-side-first guarantee). An `Agg` must be simple, partial (`INITIAL_SERIAL`) or
   final (`FINAL_DESERIAL`), without grouping sets, chains or aggregate parameters, plain
   or hashed, or sorted over the `Sort` that groups it. An `Append` must have no InitPlan
   and no partition-pruning information. A `Result` must have a child, no hash filter and
   no InitPlan. A `Material` is transparent unless it is strict, shields its child from
   rescans, or sits over a Motion in a region that will be rescanned (then the executor's
   Material must keep buffering). Everything else — every scan, Motion receiver,
   ShareInputScan, SubqueryScan, Sequence, PartitionSelector, WindowAgg, MergeAppend,
   ForeignScan of another wrapper, any node with an InitPlan — is a leaf. A `Result` whose
   projection DuckDB cannot compute becomes a leaf as well (the attempt is undone); any
   other node that fails to deparse rejects the candidate, and the pass tries the
   children as roots.

5. **Expressions.** Vars resolve through the child subqueries' column aliases; constants
   become `$n` parameters bound on the QE (a numeric `NaN` constant rejects); executor
   parameters (`PARAM_EXEC`: an InitPlan's result or a correlated subquery's outer values;
   `PARAM_EXTERN`: a prepared statement's or PL/pgSQL plan's arguments) are bound as DuckDB
   parameters when the region starts and again on every rescan; `SubPlan` expressions
   reject (a correlated subquery therefore stays with the executor, and a region *inside*
   a correlated subquery keeps its connection and prepared statement across rescans).
   Numeric arithmetic is deparsed with the DECIMAL shape DuckDB derives (a product wider
   than 38 digits rejects); division and modulo are guarded so that a zero divisor raises
   `division by zero` as PostgreSQL does; `LIKE` gets an explicit `ESCAPE '\'`; text
   ordering comparisons, `min`/`max` of text and sorting by text are accepted only under
   the `C`/`POSIX` collation, because DuckDB compares bytes (equality is bytewise under
   every deterministic collation in PostgreSQL 12, so grouping and `=` are fine).

6. **The gate** (`mode = auto`). A region needs at least one join, aggregate, sort or
   distinct (a bare scan or projection is never wrapped), at least `gg_duckdb.min_rows`
   (10000) estimated input rows, and must win on cost:

   ```
   pg   = Σ over the interior operators of the executor's estimate
   duck = cost_fixed + pg * cost_op_factor + conv_in + conv_out
   accept iff duck * (1 + cost_margin) < pg
   ```

   The executor's estimate follows `costsize.c` (`cpu_operator_cost` per aggregate and
   group key per input row, per hash or merge clause per row, `cpu_tuple_cost` per output
   row, comparisons for sorts) with measured corrections: numeric operators weigh 3.5,
   numeric aggregates 1.2 in a plain aggregate rising to 6 at 2^19 groups, group keys 1
   rising to 4, every deparsed expression's operators are counted (aggregate arguments per
   input row), and a bounded top-N sort costs `n + N ln(n/N) log2 N` comparisons, not
   `n log2 2N`. Conversion is charged per value and direction, only for the leaf columns
   the region references: about 10–17 ns for a fixed-width value, 21/130 ns in/out for a
   text, 25/20 ns for a numeric, 6 ns per row, all in planner units at ~15 µs per unit
   and scaled by `gg_duckdb.cost_convert_factor`. `cost_fixed` (200 units, about 3–4 ms)
   is a region's measured fixed cost in a warm session, `cost_op_factor` (0.25) DuckDB's
   cost of an operator relative to the executor's, `cost_margin` (0.25) the fraction by
   which DuckDB must win.

   **The boundary.** With `gg_duckdb.cost_boundary` (on), the deparser tries every
   interior-capable node as an interior and then asks whether the executor should keep it:
   the attempt's cost is the conversion of the leaves it took plus the balance of the
   operators it moved to DuckDB (`op * ((1 + margin) * factor - 1)`, negative when DuckDB
   is cheaper), the alternative is the conversion of the node's own output; when the leaf
   is cheaper the attempt is undone and the node becomes a leaf the executor runs. The
   objective is additive over the tree, so the bottom-up choice is the model's optimum and
   reduces to the whole-region decision when nothing is cut. The base of a region is never
   made a leaf unless a Sort or Unique above it keeps the region worth having. This is
   what lets a join that filters a large table through a small one stay with the executor
   while the aggregate above it goes to DuckDB.

   **Hints.** `/*+ DuckDB(a b) */` forces every eligible region whose scan aliases cover
   the set (in a top-down pass, the maximal one, taken whole), `/*+ NoDuckDB(a) */`
   forbids every region touching the alias, an empty list means the whole query,
   `NoDuckDB` wins, hints never make an ineligible subtree eligible and do nothing under
   `mode = off`; an unhonoured `DuckDB` hint is a NOTICE, or an ERROR under
   `gg_duckdb.strict`. The hint type lives in pg_hint_plan (`HINT_TYPE_DUCKDB`); ORCA
   walks only the hint types it knows and does not fall back.

7. **The region node.** `make_region()` builds a `CustomScan` with `scanrelid = 0`,
   `custom_plans` = the leaves in `gg_leaf` index order, `custom_scan_tlist` = Vars into a
   synthetic `RTE_NAMEDTUPLESTORE` entry appended to the range table (so `EXPLAIN VERBOSE`
   can name the columns; an output that carries an aggregate is described by a copy of that
   `Aggref` over display-only columns, which ruleutils insists on), `custom_private` = a
   flat `List` of `Const` (format version 4: the SQL, flags, leaf count, label, bound
   constants, output type descriptors, native readers — the only node kinds every plan
   walker accepts there), costs, rows, width, flow and parameters copied from the wrapped
   root, and a fresh `plan_node_id`. After a region is made the pass descends into its
   leaves, so a Motion leaf leads to the next slice and further regions.

8. **What the user sees.** `EXPLAIN` shows `Custom Scan (GGDuckDBRegion)` with a label
   such as `Limit Sort Agg over 1 leaf` and the leaves below it; `VERBOSE` adds the DuckDB
   SQL and the native readers; `ANALYZE` adds rows out, chunks and rows in per leaf from
   each QE. `gg_duckdb.explain_decisions = on` emits one NOTICE per candidate with the
   reason it was declined, its region SQL when accepted, and the subtrees the executor
   keeps; the gate's estimates go to the DEBUG1 log (`bench/run.sh -c` prints them).

---

## How a region executes

On the QE, `BeginCustomScan` (`src/node.c`) initialises the leaves as child PlanStates
(`custom_ps`), cross-checks every leaf column's type against the DuckDB shape recorded in
the plan, and prepares the output mapping. The first `ExecCustomScan` call takes the
memory budget (D6), connects to the process's DuckDB, registers `gg_leaf` once per
instance, resolves the native-reader tokens to reader calls over the segment's files (or to
an empty relation), prepares the statement, binds the constants, executor parameters and
file lists, and drives the pending result to completion on the backend thread, checking
for interrupts between tasks (`duckdb_interrupt` on cancel). Result chunks are streamed
and converted row by row into the scan slot (`src/query.c`, `src/types.c`); a
PostgreSQL error raised inside a leaf callback is captured, DuckDB is allowed to unwind,
and the error is re-raised in the backend; a DuckDB error becomes a PostgreSQL ERROR with
DuckDB's message. Rescans keep the connection and the prepared statement, rebind the
parameters and rescan the leaves. Squelch and shutdown stop the DuckDB query. The region is
also the execution engine of a foreign scan on a `gg_duckdb` table outside a region
(`src/fdw.c` drives the same query object).

Values cross the boundary through `src/types.c`: fixed-width types are memory writes,
strings are copied once, `bpchar` padding is stripped on the way in, dates and timestamps
are re-based between the epochs with infinities mapped, numerics are converted on their
base-10000 digits (mirroring numeric.c's storage layout, with a self-test against
`numeric_in`/`numeric_out` on first use), and a partial `sum(numeric)`/`avg(numeric)` runs
as `struct_pack(sum, count)` that the region packs into the serialised aggregate state the
final phase expects.

---

## What is supported

**Statements and placement.** `SELECT` plans (cursors and `CREATE TABLE AS` included) on
segment slices; coordinator slices with `gg_duckdb.on_coordinator`; regions in InitPlans
and correlated subqueries (rescanned); nested regions across slices (a Motion leaf's
sender side may hold its own region); regions above Motion receivers, ShareInputScans and
SubqueryScans.

**Plan nodes as interior.** `Agg` (plain, hashed, sorted-over-Sort; simple, partial and
final phases of two-phase aggregation), `HashJoin`/`MergeJoin`/`NestLoop` (inner, left,
right, full, semi, anti), `Append` (`UNION ALL`), `Result` (projection, one-time filter),
`Material` (transparent), and the top chain `Limit` (constant `LIMIT`/`OFFSET`), `Unique`
(over all columns: `DISTINCT`), `Sort`.

**Types carried across the boundary.** `bool`, `int2`, `int4`, `int8`, `float4`,
`float8`, `numeric(p,s)` with an explicit typmod and `p ≤ 38`, `text`, `varchar`,
`bpchar`, `bytea`, `date`, `timestamp`, `timestamptz`, `interval`, `uuid`; internally the
serialised numeric aggregate state.

**Expressions.** Column references, constants of the carried types, `AND`/`OR`/`NOT`,
`IS [NOT] NULL` on scalars, the boolean tests DuckDB has, searched `CASE`, `COALESCE`,
`NULLIF` on comparable types, `= ANY`/`<> ALL` against a constant, non-empty array of a
carried type (`IN` lists), executor and external parameters, `RelabelType` (binary
casts).

**Operators.** `=`, `<>`, `<`, `<=`, `>`, `>=` on operands of one type family (text
ordering only under `C`/`POSIX`); `+`, `-`, `*` on integers, floats and numerics (exact
DECIMAL arithmetic), `date + int`, `timestamp ± interval`, `interval ± interval`,
`timestamp - timestamp`; `/` and `%` on integers (integer division) and `/` on floats,
with PostgreSQL's division-by-zero error; unary `-` on integers and floats; `||`, `LIKE`
and `NOT LIKE` on text.

**Functions.** Widening casts `int2→int4`, `int2/int4→int8`, `int2/int4/int8/float4→float8`,
`int2/int4→float4`; `abs` on integers and floats; `length`/`char_length` on text; the
`text` cast of text kinds. Only immutable builtins are considered.

**Aggregates.** `count(*)`, `count(x)`, `sum` of integers and numerics (`sum(int8)` and
partial `sum(numeric)`/`avg(numeric)` through the packed state), `min`/`max` of integers,
floats, numerics, text (`C` collation), dates, timestamps and intervals, `bool_and`/
`bool_or`, `min`/`max` of floats, `FILTER (WHERE ...)`, `DISTINCT` in simple (non-split)
aggregates, and `sum`/`avg` of floats under `gg_duckdb.allow_float_aggregates` (off by
default: the last bits of a float sum depend on the order of summation).

**Foreign tables.** Reads of Parquet, CSV and JSON files on local directories under
`gg_duckdb.data_directories` or on S3 (`s3_endpoint`, `s3_region`, `s3_url_style`,
`s3_use_ssl` on the server), Iceberg tables by path and through a REST catalog, Hive
partitioning, `union_by_name`, reader options (`header`, `delim`, `quote`, `escape`,
`nullstr`, `skip`, `dateformat`, `timestampformat`, `compression`, `sample_size`,
`maximum_object_size`, `json_format`), per-column `column_name` mapping, the core's
`insert_dist_by_key` options, qual pushdown of deparsable conjuncts, `ANALYZE`, `IMPORT
FOREIGN SCHEMA`, inlining into regions, `INSERT` into file tables (Parquet, CSV, JSON,
one file per segment and transaction, savepoints honoured) and into Iceberg catalog tables
(coordinator only), `COPY ... FROM` through the insert path, partitioned tables with foreign
leaves (`CREATE FOREIGN TABLE ... PARTITION OF`, `EXCHANGE PARTITION`).

**Operations.** `gg_duckdb.status()` (queries, errors, reopens, last error per process),
`gg_duckdb.version()`, `gg_duckdb.query(sql)` (superuser only; runs on the calling
process), `gg_duckdb.foreign_files(regclass)` (which segment reads which file), the
`explain_decisions` NOTICEs and the DEBUG1 estimates, the fault points
`gg_duckdb_before_batch`, `gg_duckdb_query_start` and `gg_duckdb_after_chunk` for tests.

**GUCs** (`gg_duckdb.*`): `mode` (off), `max_memory` (512MB), `min_memory` (64MB),
`temp_directory`, `max_temp_directory_size`, `release_instance_at_end` (off),
`min_rows` (10000), `cost_fixed` (200), `cost_convert_factor` (1.0), `cost_op_factor`
(0.25), `cost_margin` (0.25), `cost_boundary` (on), `explain_decisions` (off),
`validate_at_plan_time` (on), `on_coordinator` (off), `reserve_memory` (on), `strict`
(off), `data_directories`, `http_proxy`, `allow_float_aggregates` (off), and the
development aids `debug_wrap` (identity regions over every SeqScan) and
`debug_region_sql`. The ones a QE must see carry `GUC_GPDB_NEED_SYNC`.

---

## What is not supported

Unless noted, the effect is that the subtree stays on the standard executor, with the
reason visible under `explain_decisions`; nothing errors at plan time.

**Statements.** DML plans (`INSERT ... SELECT`, `UPDATE`, `DELETE` and their SELECT
parts); utility statements; `UPDATE`/`DELETE`/`TRUNCATE` on foreign tables (an error, as
for any read-only foreign table); a transaction reading back its own uncommitted inserts
into a foreign table (they are written at commit).

**Plan nodes.** `WindowAgg`, `MergeAppend`, `RecursiveUnion`, `SetOp` (`UNION`/`INTERSECT`/
`EXCEPT` other than `UNION ALL`), grouping sets and `ROLLUP`/`CUBE`, aggregates with
aggregate parameters, `NOT IN` joins (`JOIN_LASJ_NOTIN`), deduplicating semi joins,
parameterised nested loops (index nested loops), joins with a shared or singleton outer,
`IS NOT DISTINCT FROM` hash joins, a join whose inner PartitionSelector prunes its outer
dynamic scan, `Append` with partition pruning, `Result` with a hash filter, `Limit`/
`Unique`/`Sort` that change the column list, `Unique` over a subset of the columns,
non-constant `LIMIT`/`OFFSET`, any node with an InitPlan, Motion senders (a region never
spans slices), scans of every kind (leaves), regions in coordinator slices without
`on_coordinator`.

**Types.** Arrays (so no `float4[]` vectors yet), `json`/`jsonb`, `xml`, `time`/`timetz`,
`money`, network types, enums, composites, domains, ranges, `char`/`name`/`oid`,
unconstrained `numeric` columns (no DECIMAL width) and any numeric wider than 38 digits, a
numeric `NaN` value (an ERROR when met at run time, since DECIMAL has none), `bytea`
ordering.

**Expressions and functions.** Correlated `SubPlan`s in expressions, `CASE <expr> WHEN`
(simple case), `ROW(...) IS NULL`, `ANY`/`ALL` with a non-constant or empty array or an
operator other than `=`/`<>`, any operator or function outside the list above (including
every non-builtin, every volatile or stable function, string functions other than
`length`, date arithmetic other than the listed forms, casts other than the widening
ones), text comparisons, `min`/`max` and sorts under a non-`C` collation, upper/lower and
other locale-dependent functions, `ORDER BY` inside aggregates, ordered-set aggregates,
aggregates with more than one argument, non-builtin aggregates, `DISTINCT` inside a
partial or final aggregate, a simple (non-split) `avg(numeric)`, a partial `sum(int8)` or
any other partial aggregate whose transition state is internal, `avg` in the final phase
(it stays with the executor, whose scale rules it needs), float aggregates without the
opt-in.

**Foreign tables.** Formats other than Parquet, CSV, JSON and Iceberg; DuckDB-native
database files and attached databases as tables; Iceberg writes from more than one
process (by design: single writer); Iceberg `UPDATE`/`DELETE`; row-group or data-file
level sharding (sharding is per file, so one huge file is read by one segment); files
outside `gg_duckdb.data_directories`; HNSW/vector indexes (the `vss` extension is not
built and indexes only DuckDB-native tables).

**Runtime.** DuckDB worker threads (one thread per QE), per-allocation memory
accounting, runtime fallback to the original subtree after a region has started, region
execution when the library is not preloaded (the QE cannot resolve the node), a region
that outputs more rows than DuckDB can materialise under its memory limit without
spilling to the temp directory (it spills; the cap is `max_temp_directory_size`).

---

## Consequences

**What it buys.** On the TPC-H-shaped benchmark shipped in `gpcontrib/gg_duckdb/bench`
(scale factor 1, three primaries, warm sessions, medians of 3) the 13 queries take 8.3 s
without DuckDB and 5.7 s under `mode = auto` with ORCA (1.46x), 7.4 s and 5.0 s with the
Postgres planner (1.49x): q01 2.5x, q18 2.0–2.3x, the co-located join with five numeric
aggregates 1.7x, q13 and q04 1.6–1.8x, the join-heavy queries within a few percent of the
standard executor. Every region's result is checked against the plain plan in the regress
suites, under both optimizers.

**What it costs, stated plainly.**

- Rows are converted at every leaf and every region output. Fixed-width values are
  cheap, numerics are now cheap, texts cost about 130 ns on the way out. A region that
  only replaces joins over big inputs cannot pay for itself; the gate declines it.
- A region costs 3–4 ms fixed in a warm session, plus the plan-time preparation on the
  coordinator; the first region of a session opens DuckDB on every QE (about 30 ms and
  15–20 MB of RSS per process, kept for the session).
- Memory accounting is a budget, not a measurement (D6).
- Text ordering is byte order: under the usual `en_US.utf8` database collation, sorts,
  `min`/`max` and range comparisons on text stay with the executor.
- Float sums differ in their last bits and are opt-in.
- Estimates drive the gate: with stale or missing statistics the model decides on the
  optimizer's guesses, like the optimizer itself.
- The optimizers do not know DuckDB exists: a plan shape that would only pay under DuckDB
  is never chosen, and hints are the only steering beyond the gate.
- `gg_duckdb.mode` defaults to `off`. The gate is calibrated on one host and one data
  set; `auto` is the setting for evaluation.

**Operationally.** `shared_preload_libraries = 'gg_duckdb'` on every node, `libduckdb.so`
in `$GPHOME/lib`, one DuckDB instance per process on first use, spill files under each
node's `base/pgsql_tmp`, `gg_duckdb.data_directories` as the file allow-list, credentials
through user mappings.

---

## Alternatives considered

- **Execute whole queries in DuckDB with PostgreSQL tables as DuckDB scans** (the pg_duckdb
  approach). Rejected: it removes Motions and the MPP execution model, so every distributed
  query would need its own plan and data movement in DuckDB.
- **Teach GPORCA a DuckDB operator** so that it chooses plan shapes for DuckDB. Deferred:
  it needs a cost model for both engines inside the optimizer and ORCA-only coverage; the
  pass first, with measured preconditions for the memo-level step.
- **Use DuckDB's C++ API** for the allocator hook and per-allocation accounting. Rejected
  for now: no stable ABI across minors and a C++ extension; revisit at DuckDB 2.0.
- **Runtime fallback** when a region fails mid-query. Rejected: side effects and partial
  results are impossible to undo in general; plan-time validation makes the failing class
  small (an actual bind failure) and turns it into "no region".
- **Store data in DuckDB** (tables, database files). Out of scope: the extension is an
  executor and a reader, not a storage engine.

---

## Verification

- `gpcontrib/gg_duckdb/sql`: `setup`, `basic`, `region`, `deparse`, `joins`, `hints`,
  `foreign`, `params`, `write`, `teardown`, run under both optimizers (`make installcheck`
  with `PGOPTIONS='-c optimizer=on|off'`); every region case compares the plain plan's
  result with the region's.
- `gpcontrib/gg_duckdb/isolation2`: `cancel`, `timeout`, `squelch`, `memory`,
  `leaf_error`, `nested`, through the fault points.
- `gpcontrib/gg_duckdb/bench`: the TPC-H-shaped set with an in-database generator,
  `run.sh` timing `off`, `force`, `auto` and `auto` with the boundary off, `-c` for the
  gate's estimates.
- `gpcontrib/gg_duckdb/test/external`: a Compose stack (MinIO, an Iceberg REST catalog)
  for the S3 and Iceberg paths.
