# Redset for gg_duckdb

The workload of the Amazon Redshift fleet, as far as its published query
statistics let one write it: van Renen et al., *Why TPC Is Not Enough: An
Analysis of the Amazon Redshift Fleet* (VLDB 2024) describes how real
warehouse workloads differ from TPC-H/DS (writes everywhere, short and
repeating statements, one wide varchar-heavy fact per cluster, skew and long
tails) and publishes [Redset](https://github.com/amazon-science/redset): three
months of per-statement statistics of 200 provisioned and 200 serverless
clusters, one Parquet file per cluster (23 columns: ids, arrival timestamp,
compile/queue/execution durations, a fingerprint, the statement type, flags,
comma-separated table lists, bytes scanned and spilled, operator counts).

`queries/` asks the paper's questions of that data, in the SQL an analyst
would write, each file's header comment naming the table or figure it
reproduces: statement types and classes (q01, q15 across both tiers), run-time
buckets (q02), operators per statement (q03), percentiles (q04), daily and
hourly load and spikiness (q05, q06), the cluster profile and busyness score
(q07), repetition (q08, q09), the time between repeats (q10), common table
sets (q11), result-cache hits (q12), aborts (q13), spill by cluster size (q14),
where the time goes (q16), dashboard-style short statements (q17), writes per
table from the comma lists (q18) and the heavy users (q19).  So unlike TPC-DS
this is a single-fact workload of aggregates over integers, timestamps,
booleans and strings, with window functions, ordered-set aggregates,
`count(DISTINCT)`, `date_trunc`/`extract`, set-returning functions and a
self-join, and with short selective statements next to full scans.

```sh
source $GPHOME/greengage_path.sh; source gpdemo-env.sh
export PGDATABASE=postgres
gpconfig -c gg_duckdb.data_directories -v $HOME/ws/redset/data; gpstop -u
bench/redset/load.sh                              # fetch, load the redset schema, ANALYZE (about 25 minutes)
PGOPTIONS='-c optimizer=on' bench/redset/run.sh -n 3                          # append-only column tables
PGOPTIONS='-c optimizer=on' BENCH_SCHEMA=redset_pq bench/redset/run.sh -n 3   # the Parquet files, through the wrapper
```

`load.sh` fetches the per-cluster files (the serverless tier whole, and of the
provisioned tier, whose 433M rows would take 18 GB, the 50 clusters with
`instance_id % 4 = 2`: 3.3 GB, about 80M rows, the busiest 2.4 GB cluster left
out; `-a` or `-m MOD -r REM` change that), creates foreign tables over them in
`redset_pq`, and fills `redset.provisioned` and `redset.serverless` from those
foreign tables: append-only column tables compressed with zstd, distributed by
`(instance_id, query_id)`, partitioned by week, with the published columns typed
as a warehouse would (counts as integers, flags as booleans).  A replicated
`redset.query_types` dimension carries the paper's RO/RW/Sys classes.
`run.sh` is the TPC-DS runner (see `../tpcds/README.md`) run in the `redset`
schema, or in `redset_pq` with `BENCH_SCHEMA`, with the warm-up statement in
`warmup.sql`.

## Results (14 September 2026, 3 primaries on one laptop, medians of 3)

The 50-cluster subset (74.7M provisioned rows) and the serverless tier
(8.2M), every result set equal between the modes in all six passes.  "As
shipped" is commit 2a6f62be721; "fixed" adds the integer aggregate states
(partial `sum`/`avg` over bigints, `avg` over integers and smallints) and the
wrapper fix for pushed quals of a scan whose range-table index is settled
after GetForeignPlan (q09 failed on Parquet before it).

| Pass | Build | off | auto | Ratio | With regions | Ratio on those |
|---|---|---|---|---|---|---|
| Column tables, GPORCA | as shipped | 224.5 s | 213.4 s | 1.05x | 6 of 19 | 1.10x |
| Parquet via the wrapper, GPORCA | as shipped | 200.7 s | 183.1 s | 1.10x | 6 of 18 | 1.19x |
| Column tables, planner | as shipped | 228.9 s | 225.2 s | 1.02x | 4 of 19 | 1.03x |
| Column tables, GPORCA | fixed | 224.8 s | 205.8 s | 1.09x | 15 of 19 | 1.12x |
| Parquet via the wrapper, GPORCA | fixed | 218.3 s | 163.6 s | 1.33x | 16 of 19 | 1.43x |
| Column tables, planner | fixed | 224.0 s | 215.6 s | 1.04x | 13 of 19 | 1.07x |
| Column tables, GPORCA | direct scans | 222.2 s | 200.0 s | 1.11x | 15 of 19 | 1.16x |
| Parquet via the wrapper, GPORCA | direct scans | 220.5 s | 168.3 s | 1.31x | 16 of 19 | 1.39x |
| Column tables, planner | direct scans | 238.6 s | 221.7 s | 1.08x | 13 of 19 | 1.16x |

On the column tables the wins are the self-join (q12 1.7x), the statement
types (q01 1.6x) and the wide profiles (q07 1.25x, q16 1.18x, q14 1.15x);
narrow few-group aggregates gain nothing once their rows are converted.  On
the Parquet files the same aggregates run natively in DuckDB: q15 5.4x, q13
5.2x, q01 5.0x, q16 4.6x, q02 3.9x, q07 3.6x, q03 2.5x, q14 2.3x, q11 1.8x.
Out of reach by construction: ordered-set aggregates (q04, q10), window
functions (q05, q10), the planner's multi-stage `count(DISTINCT)` plan, whose
first stage passes non-grouped columns through (q19; DuckDB refuses the
deparsed SQL and plan-time validation keeps the query on the executor), the
`unnest` of the comma lists (q18, its aggregate above is a region), and top-N
by a text key under a non-C collation.  The memory gate declines the
distinct-fingerprint groupings on the column tables (q08 312 MB, q09 360 MB
against the 256 MB budget); q08 runs natively on Parquet at 1.47x.  `date_trunc`/`extract` are not
deparsed, so q05 and q06 convert their rows even on Parquet; q06 loses 5-11%
to a sort-only region the memory gate leaves behind (a planner estimate of
25M groups for 240 real ones).  Ten of the 19 queries use `FILTER`, on which
GPORCA falls back to the planner.

With `date_trunc` and `extract` deparsed (the next change, 14 September), q05
and q06 do not move (0.96-1.0x and 0.91-0.98x): both optimizers compute the
expression in the scan below the region, and the planner, to which GPORCA
falls back on FILTER, estimates 25M groups for `GROUP BY instance_id,
date_trunc('day', arrival_timestamp)` where there are 4,550 (240 for the
hours of q06).  That estimate picks a one-phase aggregate after the
redistribution, which is slower for the executor itself (q05 and q06 together
24.3 s, against 14.8 s with `gp_eager_two_phase_agg = on`), and it makes the
memory gate decline the partial aggregates (a 2.8 GB working set against the
256 MB budget).  With the two-phase plan forced and the budget raised, q06
runs at 2.10x on the column tables (12.1 s to 5.8 s); on Parquet its native
partial aggregate is still declined on a 52M-group estimate.  A bounded
estimate for such expressions (24 hours, 7 weekdays, the column's range in
days) is the lever for the load-over-time analyses.

With direct scans (15 September, commit aa091e2b73c: regions read heap,
append-optimized and partitioned tables themselves instead of pulling their
scans through the executor), the column-table queries whose regions sit on
the partition scans gain, alike under both optimizers: q02 3.7 to 1.9 s
(0.99x to 1.97x), q15 7.1 to 5.1 s, q13 5.2 to 3.8 s, q01 5.7 to 4.9 s, q16
5.0 to 4.3 s, q07 4.1 to 3.6 s.  The totals move less, from 1.09x to 1.11x
under GPORCA and 1.04x to 1.08x under the planner (1.12x to 1.16x and 1.07x
to 1.16x over the queries with regions), because the queries that take most
of the time (q04, q05, q08-q12, q18, q19) have no region or one above a
Motion, whose leaf is not a scan.  The planner pass ran while the host was
slower (its `off` total 6.5% up, queries without regions 7-15% slower under
`auto` too), so its ratios rather than its times compare.  q06's 0.92x is the
same sort-only region above the redistribution as before, its plan identical
with direct scans off.  The Parquet pass is the control: its scans already
were DuckDB's own readers, and it stays within noise (1.33x to 1.31x).  All
57 result sets equal.
