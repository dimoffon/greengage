# TPC-DS for gg_duckdb

The 99 TPC-DS queries under `gg_duckdb.mode = off` and `auto`, timed and
their result sets compared, on the schema the Greengage TPC-DS harness
([dimoffon/TPC-DS](https://github.com/dimoffon/TPC-DS)) creates: append-only
column tables compressed with zstd, the fact tables partitioned by date, the
kit's distribution keys.

```sh
git clone https://github.com/dimoffon/TPC-DS.git ~/ws/TPC-DS
make -C ~/ws/TPC-DS/00_compile_tpcds/tools        # dsdgen, dsqgen (gcc >= 10 needs -fcommon in LINUX_CFLAGS)
source $GPHOME/greengage_path.sh; source gpdemo-env.sh
export PGDATABASE=postgres
bench/tpcds/load.sh 1                             # dsdgen SF 1 -> gpfdist -> tpcds schema, ANALYZE, queries/
PGOPTIONS='-c optimizer=on' bench/tpcds/run.sh -n 3
```

`load.sh` generates the flat files with one dsdgen chunk per primary segment,
creates the tables from the kit's DDL, loads them through one gpfdist, runs
ANALYZE, and writes the queries for that scale to `queries/` (dsqgen, the
kit's Greengage dialect, its default seed; q14, q23, q24 and q39 hold two
statements each).  `run.sh` runs every query under each mode in turn, three
times, interleaving the modes, and writes `results/<timestamp>/`: the first
run's result rows per query and mode, the errors, and `results.csv` with the
medians, the number of DuckDB regions under `auto` and under `force`, the
speedup off/auto, and whether the two modes' result sets are the `same`,
differ in `order` only, or `DIFF`.

## Results: direct scans (16 September 2026, GPORCA, medians of 3)

Two passes of every scale on the same build (`aa091e2b73c`: a region reads the
heap, append-optimized and partitioned tables under its scans itself instead of
pulling their rows through the executor), one with `gg_duckdb.direct_scans =
off` and one with it on, so only the leaf kind differs.

| Scale | Direct scans | off | auto | Ratio | With regions | Ratio on those |
|---|---|---|---|---|---|---|
| SF10 (99 queries) | off | 314.4 s | 282.5 s | 1.11x | 51 | 1.20x |
| SF10 | on | 314.4 s | 279.4 s | 1.13x | 57 | 1.22x |
| SF30 (98 queries) | off | 663.0 s | 557.9 s | 1.19x | 59 | 1.32x |
| SF30 | on | 641.3 s | 511.2 s | 1.25x | 65 | 1.39x |

The SF30 totals leave out q14, which the host failed to fork query executors
for in the first pass ("failed to acquire resources on one or more segments",
the commit-limit trap); it completed in the other three runs.  The `off`
totals themselves differ by 3% between the SF30 passes, which is this host's
drift over two hours, so the ratios compare and the seconds do not.

Direct scans give the gate more to take (SF30: 65 queries with regions against
59, SF10: 57 against 51) and make the regions it already had cheaper.  The
queries that moved at SF30 (auto, direct off -> on): q24 36.5 -> 12.0 s (no
region before, two after), q82 2.35 -> 1.71 s, q01 1.06 -> 0.76 s, q88 11.6 ->
8.6 s, q09 15.1 -> 11.3 s, q96 3.84 -> 3.02 s, q11 9.5 -> 8.0 s, q74 7.4 ->
6.3 s, q25 1.96 -> 1.66 s; at SF10 q88 1.24x, q96 1.25x, q09 1.16x (5 regions
-> 15), q90 and q80 gain their first regions.  Losses are small and within the
host's noise (SF30 q65 0.92x on a 3 s query; SF10 q97 0.87x with an identical
plan in both passes).  One real cost-model miss remains: SF10 q76 takes one
more join into the region with its dimensions read directly and lands at 0.95x
of the standard executor, where the pass without direct scans matched it.

q64's `DIFF` at SF30 appears in both passes: its `LIMIT 100` cuts different
tied rows, and without the limit both modes return the same 1500 rows.  Four
more queries differ only in the order of tied rows.
