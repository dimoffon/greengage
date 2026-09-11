# gg_duckdb benchmark

A TPC-H-shaped workload for measuring what DuckDB regions buy on a Greengage
cluster and for calibrating the cost gate (`gg_duckdb.cost_*`).

```sh
source $GPHOME/greengage_path.sh; source gpdemo-env.sh
export PGDATABASE=postgres
bench/load.sh 1                                    # tpch schema, SF 1: 6M lineitem rows, heap
bench/load.sh 1 "WITH (appendonly=true, orientation=column, compresstype=zstd)"
PGOPTIONS='-c optimizer=on' bench/run.sh -n 3 -o /tmp/bench.csv
bench/run.sh -q 'q0*' -c                           # cost gate estimates per candidate
```

The data is generated inside the database (`gen.sql`), deterministically, with
the spec's row counts, value ranges and date distributions but not dbgen's
values, so results are comparable between runs on the same host and not with
published TPC-H numbers.  Decimal columns are narrow (`numeric(12,2)` prices,
`numeric(4,2)` rates) so the products the queries form stay within 38 digits.

`queries/` holds TPC-H Q1, Q3, Q4, Q5, Q6, Q10, Q12, Q13, Q14, Q18 and Q19 with
the spec's validation parameters (Q18's threshold lowered for the generator's
quantities), and two local queries over the co-located `orders`/`lineitem`
pair.  Each is run under `gg_duckdb.mode = off`, `force`, `auto` and `auto`
with `gg_duckdb.cost_boundary = off` (`whole_ms`: the gate judging every
eligible region whole, as it did before the boundary was cost-chosen); the
report shows the median of the runs, the number of DuckDB regions in the
forced plan, and the speedups off/force and off/auto.  With `-c` the gate's
estimates follow each query, one line per candidate and one per subtree the
executor keeps under it.
