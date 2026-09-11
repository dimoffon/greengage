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
