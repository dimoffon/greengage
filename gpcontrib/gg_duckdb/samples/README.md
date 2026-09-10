# gg_duckdb samples

Two scripts against the Compose stack of `test/external` (MinIO on
`localhost:9100`, an Iceberg REST catalog on `localhost:8181`; `docker-compose
up -d` there and wait for the `init` job), run as a superuser on the demo
cluster:

```sh
psql -d postgres -f samples/s3_partitioned.sql
psql -d postgres -f samples/iceberg_partitioned.sql
```

`s3_partitioned.sql`: a Parquet lake on S3 as a Greengage table partitioned
by month (RANGE on `sale_date`) and by region (LIST), every leaf a
`gg_duckdb` foreign table over its own directory. INSERT and COPY through
the parent route each row to its leaf, and each segment writes one Parquet
file per leaf and transaction at commit; reads through the parent are pruned
to the leaves the filters allow, filters on other columns are pushed into
the Parquet reader, and with DuckDB regions the readers are inlined into the
aggregate. The same tree in the classic `SUBPARTITION BY LIST` syntax with
`EXCHANGE PARTITION` is shown in a comment.

`iceberg_partitioned.sql`: an Iceberg table partitioned by `day(sale_date)`
and `region` in its own partition spec (created by
`test/external/init/partitioned.py`, since DuckDB cannot declare a spec),
reached through the REST catalog as one foreign table written by the
coordinator: every transaction is one snapshot, DuckDB writes one data file
per partition it touches, and reads that filter on the partition columns
open only the matching data files.
