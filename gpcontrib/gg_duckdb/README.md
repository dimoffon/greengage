# gg_duckdb — DuckDB auxiliary executor for Greengage

`gg_duckdb` embeds [DuckDB](https://duckdb.org) in every Greengage backend as a
second executor for segment-local, Motion-free parts of a plan. This directory
is at **milestone M7**: the library builds into the tree, loads on the
coordinator and on every segment, opens a per-backend DuckDB instance lazily on
the backend thread, and with `gg_duckdb.mode = auto|force` a planner pass turns
eligible segment-local subtrees into DuckDB regions: hash, merge and nested-loop
joins (inner, left, right, full, semi, anti), `UNION ALL` appends, plain,
hashed and sorted aggregates (`count`, `sum`, `min`, `max`, `bool_and`,
`bool_or`, with DISTINCT and FILTER) including the partial and final phases of
a two-phase aggregate, projections, and top chains of `ORDER BY`, `DISTINCT`
and `LIMIT/OFFSET` over a whitelist of types, operators and functions whose
DuckDB semantics equal PostgreSQL's (arithmetic, comparisons, `LIKE`, `CASE`,
`COALESCE`, widening casts, `abs`, `length`, and `date_trunc` and `extract`
over timestamps for the units both engines truncate and count alike). Leaves (scans, Motion receivers, shared
scans, subquery scans, joins DuckDB may not run, anything else) stay ordinary
plan nodes pulled by DuckDB on the backend thread, and the subtree under a leaf
gets regions of its own; a projection DuckDB cannot compute stays with the
executor as a leaf and the region goes on above it. A sequential scan over a
heap or append-optimized table is the exception: when the region can compute
its target list and filter, it reads the table itself through `gg_rel(i)` (the
scan's snapshot and visibility, no scan node, only the attributes DuckDB asks
for; heap pages directly, append-optimized row and column tables through their
table AM's projected scan, and the partitions under GPORCA's Dynamic Seq Scan
in turn), and the scan's filter runs in DuckDB. Every region query is prepared on the coordinator
before it is used, so an ineligible or unbindable subtree simply stays on the
standard executor. Partial `sum` and `avg` phases over numerics and bigints,
and partial `avg` phases over integers and smallints, run in DuckDB too: the
region packs DuckDB's exact sum and count into the transition state the
executor's final phase expects (a final `avg` stays with the executor, whose
division sets the result's scale). `DuckDB()`/`NoDuckDB()` hints through
pg_hint_plan force or forbid regions, and under `mode = auto` a cost gate
compares the standard executor's estimate for the interior operators with
DuckDB's, conversion included, and draws the region's boundary by the same
comparison: a subtree whose output is cheaper to convert than its inputs, a
join filtering a large table through a small one typically, stays with the
executor as a leaf and only the operators above it go to DuckDB. The `gg_duckdb` foreign data wrapper reads
Parquet, CSV and JSON files through DuckDB on every segment, each file by
exactly one segment, and the pass inlines such a scan into a region as a
native reader, so joins and aggregates above it see no row conversion on the
way in. Executor parameters (an InitPlan's result, a correlated subquery's
outer values, a prepared statement's or PL/pgSQL generic plan's arguments) are
bound as DuckDB parameters, regions form inside InitPlans and correlated
subqueries too, and a rescanned region binds the new values into its kept
prepared statement. Foreign tables can be written: `INSERT` buffers the rows
in DuckDB for the transaction and writes them at commit, one Parquet, CSV or
JSON file per segment under the table's location pattern, or one snapshot of
an Iceberg table reached through its REST catalog. The design is in
`doc/architecture/gg-duckdb-executor.md`.

## Design in one paragraph

DuckDB is opened with `threads = 1` and `external_threads = 1`, so it owns no
thread of its own: every unit of DuckDB work runs inside
`duckdb_pending_execute_task()` on the backend thread. That is what makes it
legal for DuckDB callbacks (the future leaf table functions) to call back into
the backend, whose buffer manager, memory contexts, error handling and vmem
tracker are all main-thread only. Only DuckDB's C API (`duckdb.h`) is used: it is
the stable client surface across DuckDB minors and the coming 2.0 ABI freeze,
and it keeps the extension in C. The one rule those callbacks live by: the
`PG_TRY` barrier around the PostgreSQL work holds no DuckDB call that can
allocate, because DuckDB reports an allocation failure under its memory limit
as a C++ exception, and one unwinding through a `PG_TRY` block would leave the
backend's exception stack pointing into a dead frame.

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
a region needs under auto, 10000), the cost gate's constants `cost_fixed`
(planner cost units per region, 200: about 3 ms), `cost_convert_factor` (scales
the measured per-value conversion costs: 10-17 ns for a fixed-width value in
either direction, 21/130 ns for a text in/out, 25/20 ns for a numeric in/out,
1.0), `cost_op_factor` (DuckDB's cost of an interior operator relative to the
standard executor's, 0.25) and `cost_margin` (DuckDB must win by this fraction,
0.25), `cost_boundary` (let the gate end a region above a subtree whose output
is cheaper to convert than its inputs, on; off judges every eligible region
whole), `direct_scans` (let a region read the heap and append-optimized tables under
its sequential scans itself, computing their target lists and filters, on; every row the scan
visits is converted, so the gate charges conversion on the table's rows and
credits the skipped scan node), `explain_decisions` (NOTICE per candidate subtree with the reason it was
declined, or its DuckDB query and the subtrees the executor keeps under it; the
gate's estimates go to the DEBUG1 log).  The gate is also memory-aware: a
region's hash tables (the groups of an aggregate, the build side of a join,
the rows of a DISTINCT, at twice their planner width) must fit the memory the
region will have, the largest memquota share among the operators it replaces
within the `min_memory`/`max_memory` bounds; the boundary keeps a subtree with
the executor when it would not fit, and a region whose root would not fit is
declined (a hash table larger than DuckDB's memory limit fails at execution
rather than spilling, and nothing falls back).  For the estimate to hold, the
instance runs with DuckDB's join reordering and build-side swapping disabled: a
region's joins keep the plan's order and build sides, chosen with statistics
DuckDB does not have for the leaves.  Then
`strict` (an unhonoured `DuckDB()` hint is an error instead of a notice, off),
`on_coordinator` (regions in coordinator slices, off), `validate_at_plan_time`
(prepare every region query on the coordinator first, on), `max_memory` and
`min_memory` (bounds of a region's DuckDB memory limit, 512MB and 256MB),
`reserve_memory` (reserve the region's budget with the vmem tracker, on),
`temp_directory` (default `base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid>` under
each node's data directory), `max_temp_directory_size`,
`release_instance_at_end`, `allow_float_aggregates` (`sum`/`avg`/`min`/`max`
over float4/float8 inside regions, off: DuckDB's summation order differs, so
the last digits of a float sum can differ from the standard executor's),
`data_directories` and `http_proxy` (see the foreign data wrapper),
`jvm_options`, `hadoop_conf_dir` and `kerberos_ccache` (the embedded JVM that
reads HDFS, see Iceberg in a Hive Metastore), and the
development aids `debug_wrap` and `debug_region_sql`.

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
option `column_name` maps a column to a differently named file column.
`json_format` (`auto`, `newline_delimited`, `array`, `unstructured`) fixes a
JSON table's layout: DuckDB detects it with a 32 MB buffer it keeps per file,
which a table of several files does not get under the memory budget, so a
pattern location reads as newline-delimited (what the writer produces)
unless told otherwise, and a single file is detected. With
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

### Iceberg through a REST catalog

A server with `iceberg_endpoint` (and `iceberg_warehouse`, `iceberg_auth`
`none`/`bearer`/`oauth2` with `iceberg_token` or `iceberg_client_id` and
`iceberg_client_secret` on the user mapping, `iceberg_oauth2_server_uri`)
reaches an Iceberg REST catalog; a table whose `location` is
`namespace.table` with `format 'iceberg'` is that catalog's table, read
through DuckDB's `ATTACH` (no version guessing, the catalog knows the
current snapshot) and written through it. `IMPORT FOREIGN SCHEMA "namespace"
FROM SERVER s` creates one foreign table per table of the namespace. The
data files are wherever the catalog says, so `gg_duckdb.data_directories`
must hold a remote prefix, and the S3 secret of such a table has no bucket
scope.

### Iceberg in a Hive Metastore, on HDFS

A server with `hms_uri` reaches a Hive Metastore, and a table whose
`location` is `database.table` with `format 'iceberg'` is that metastore's
table. `hms_conf` passes settings to the metastore client verbatim
(`'metastore.sasl.enabled=true;metastore.kerberos.principal=hive/_HOST@REALM'`),
`hms_config_dir` overrides `gg_duckdb.hadoop_conf_dir` for that server, and
`hdfs_user`, `hdfs_authentication` (`simple` or `kerberos`) and `hdfs_conf`
become the HDFS identity, as a DuckDB secret scoped to the name node.
`IMPORT FOREIGN SCHEMA "database" FROM SERVER s` creates one foreign table
per **Iceberg** table and skips the rest with a notice: only Iceberg tables
are carried, not Hive Parquet, CSV or ORC ones. `gg_duckdb.data_directories`
must hold the name node's prefix (`hdfs://nn:8020/`). A metastore table is
read only; its row estimate comes from the current snapshot's manifests, not
from a scan. The metastore catalog caches for five seconds, so a table
committed elsewhere can take that long to appear.

```sql
CREATE SERVER hms FOREIGN DATA WRAPPER gg_duckdb
    OPTIONS (hms_uri 'thrift://ms.example:9083', hdfs_authentication 'kerberos',
             hms_conf 'metastore.sasl.enabled=true;metastore.kerberos.principal=hive/_HOST@EXAMPLE');
SET gg_duckdb.hadoop_conf_dir = '/etc/hadoop/conf';   -- core-site.xml, hdfs-site.xml
SET gg_duckdb.kerberos_ccache = '/tmp/krb5cc_gpadmin';
SET gg_duckdb.data_directories = 'hdfs://nn.example:8020/';
IMPORT FOREIGN SCHEMA "warehouse" FROM SERVER hms INTO lake;
```

HDFS is reached through JNI libhdfs, which starts a **JVM inside the
backend**: one per process, at the first HDFS or metastore access, never
destroyed, and every segment backend that reads HDFS gets one of its own.
It reads `gg_duckdb.jvm_options`, `gg_duckdb.hadoop_conf_dir` and
`gg_duckdb.kerberos_ccache` at that moment, so changing them affects
backends that have not read HDFS yet, not the ones that have. The default
`jvm_options` keep a JVM small (`-Xmx256m`, serial GC, one compiler thread)
and hand the signals PostgreSQL needs back to it (`-Xrs`); raise the heap
for large reads, and count it as memory outside `gg_duckdb.max_memory`,
which only bounds DuckDB. Kerberos works from a **ticket cache file** owned
by the server's OS user: the client cannot log in from a keytab, so
something outside the server has to keep the cache fresh. A backend waiting
on HDFS or the metastore cannot be interrupted, so a cancelled query returns
only when the client's own timeouts expire; set them in the Hadoop
configuration.

This needs a build with `DUCKDB_HDFS_DIR` (see `duckdb/build.sh`), which
links duckdb-hdfs into `libduckdb.so`, packages its JNI runtime beside it,
and replaces the upstream Iceberg extension with the fork that carries the
metastore catalog. `test/hdfs/` runs the checks against a cut-down ADH
stand.

## Writing: INSERT into foreign tables

```sql
CREATE FOREIGN TABLE sales (id bigint, day date, amount numeric(12,2))
    SERVER duck OPTIONS (location '/data/sales/part_*.parquet');
INSERT INTO sales SELECT ... ;                 -- one file per segment and transaction
COPY sales FROM '/tmp/sales.csv' CSV;
CREATE FOREIGN TABLE ice_sales (...) SERVER minio
    OPTIONS (location 'shop.sales', format 'iceberg', mpp_execute 'coordinator');
INSERT INTO ice_sales SELECT ... ;             -- one snapshot per transaction
```

A file table is writable when its one location's file name holds exactly
one `*`: each new file takes the wildcard's place
(`part_gg-<transaction>-<segment>.parquet`), so the reader's pattern finds
it. The rows a process receives (every segment of an all-segments table,
randomly or by the columns with the core's `insert_dist_by_key` option; the
coordinator otherwise) are buffered in a table of its DuckDB instance for the
transaction, under the memory budget and spilling beyond it, and written by
DuckDB's `COPY` when the transaction commits, or prepares on a segment of a
distributed transaction. A transaction that rolls back writes nothing; a
savepoint rolled back takes its rows out of the buffer; the rows of an open
transaction are not visible to its own reads. Parquet row groups shrink to
the budget when the rows are wide. CSV takes the `header`, `delim`, `quote`,
`escape`, `nullstr`, `dateformat`, `timestampformat` and `compression`
options, Parquet and JSON `compression`. Only `INSERT` (and `COPY FROM`):
files are immutable, so `UPDATE` and `DELETE` are refused.

An Iceberg catalog table is written by one process, the coordinator
(`mpp_execute 'coordinator'`; an all-segments table refuses the insert with
that hint), with the foreign table locked for the transaction: DuckDB's
Iceberg commits do not detect each other, three concurrent inserts from
separate processes left one snapshot and no error, so the wrapper never runs
two. A writer from outside the cluster is still that risk. The coordinator's
DuckDB budget for such a write starts at 256 MB, since DuckDB's Iceberg
writer sizes its Parquet row groups itself. Writers to S3 upload in parts of
8 MB (`s3_uploader_max_filesize` 80 GB, two uploader threads) instead of
DuckDB's default 80 MB parts, which would not fit a 64 MB budget.

What a commit cannot undo: a segment writes its file when its part of the
distributed transaction prepares, and does not see the coordinator's decision
after that, so a rollback after every segment prepared, or a crash there,
leaves the files; their names carry the distributed transaction id. An error
while writing aborts the transaction, but the files of the tables already
written by it remain. `gg_duckdb.query()` is a superuser function.

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
regions above Redistribute Motion leaves; integer aggregate states; date_trunc
and extract), division by zero, the auto-mode gate, and the collation,
float-sum and date-unit rejections. `joins` covers milestone M3
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
rescans, ANALYZE, IMPORT FOREIGN SCHEMA, and the refused locations. `params`
covers executor parameters inside regions (an InitPlan's result in a HAVING
clause and a join condition, a generic plan's arguments, a PL/pgSQL variable,
correlated subqueries rescanned with new values) and the float-aggregate
opt-in. `write` (an `input/*.source` test) covers `INSERT` into Parquet, CSV
and JSON tables: every carried type round-tripping, one file per segment,
rollback, savepoints, a failing insert, `COPY FROM`, the refused statements,
and a buffer larger than the memory budget. `test/external/run.sh` covers the
Iceberg catalog: reads by name, a coordinator-side insert against a
reference, rollback, the refusal from the segments, and the namespace
import. `bench/` holds the TPC-H-shaped benchmark (see its README), and
`samples/` two scripts for a lake partitioned by date and by region: Parquet
on S3 as a Greengage partition tree of foreign tables, and an Iceberg table
with a partition spec (see `samples/README.md`).

```sh
make -C gpcontrib/gg_duckdb/isolation2 installcheck
```

runs the fault-injection tests (a build with `--enable-debug-extensions`):
cancellation while a segment is suspended inside a leaf batch,
`statement_timeout`, a LIMIT stopping regions on other segments, the
out-of-memory error under a small DuckDB memory limit and the recovery after
it, a PostgreSQL error raised inside a leaf on one segment, and two regions
nested in one backend through SPI. The fault points are
`gg_duckdb_before_batch` (every leaf batch), `gg_duckdb_query_start` and
`gg_duckdb_after_chunk`. Its answer files hold under both optimizers.

## Measured on the development host (DuckDB 1.5.5, 2026-09-09)

Opening the instance costs about 30 ms and 15–20 MB of RSS per backend
(a segment QE grows from ~21 MB to ~40 MB); a 2M-row group-by adds ~4 MB;
the thread count of a QE stays at two (main + interconnect receiver).

On the TPC-H-shaped benchmark (`bench/`, SF 1, 3 primaries, medians of 3 in
warm sessions), the 13 queries take 8.3 s without DuckDB and 5.7 s under
`mode = auto` with ORCA (1.46x), 7.4 s and 5.0 s with the Postgres planner
(1.49x): regions win by 1.6–2.6x where the interior does much work per
input row (Q1's eight numeric aggregates 2.5x, Q18's large group-by
2.0–2.3x, the co-located join with five numeric aggregates 1.7x, Q13's left
join and counts and Q4's semi join 1.6–1.8x), and the rest run within a few
percent of the standard executor, the join-heavy queries (Q3, Q5, Q10)
either declined or cut down to the aggregate above the join. The gate's
constants are measured, not fitted: converting a value costs 10–17 ns for a
fixed-width type, 21/130 ns in/out for a text and 25/20 ns in/out for a
numeric (converted on its digits, not through text), while the executor's
numeric operators cost 130 ns and its numeric aggregates 46–250 ns per row
depending on the number of groups; and the gate
draws each region's boundary by the same model, so a join that filters a
large table through a small one stays with the executor and only the
aggregate or top-N above it goes to DuckDB. `mode` stays `off` by default; see
`doc/architecture/gg-duckdb-executor.md` for the numbers, the measurements
and the decision.
