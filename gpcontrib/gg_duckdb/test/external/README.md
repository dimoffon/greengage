# gg_duckdb against S3 and Apache Iceberg

A Compose stack with the external systems the foreign data wrapper can read
from, and a script that checks the wrapper against them from the demo cluster
on the host. Not part of `make installcheck`: it needs Docker and a DuckDB
built with `DUCKDB_REMOTE_EXTENSIONS=1` (httpfs, avro, iceberg).

```sh
cd gpcontrib/gg_duckdb/test/external
docker-compose up -d && docker-compose logs -f init      # until "done"
source $GPHOME/greengage_path.sh; source <demo>/gpdemo-env.sh
./run.sh                                                 # the checks
docker-compose down -v
```

What the stack provides:

| Service | Address on the host | Contents |
|---|---|---|
| MinIO (S3 API) | `localhost:9100`, console `localhost:9101`, user `ggadmin` / `gg-secret-1` | bucket `ggduck`: `files/events/part-*.parquet` (6 parts, 6000 rows), `files/items.csv`, `files/tags.json`, and the Iceberg warehouse under `warehouse/` |
| Iceberg REST catalog | `localhost:8181` | namespace `demo`, table `demo.events`: two appends and a delete, so the current snapshot is the third one and spans several data files |
| init job | one-shot | writes the files with pyarrow and the table with pyiceberg |

The rows follow the same formulas as `run.sh` generates in PostgreSQL, so the
checks compare the wrapper's results against a reference table.

The Iceberg table is read by its directory (`location 's3://ggduck/warehouse/demo/events'`,
`format 'iceberg'`). Catalog-written tables carry no `version-hint.text`, so
the wrapper lets DuckDB pick the newest `metadata/*.metadata.json`
(`SET unsafe_enable_version_guessing = true` on the scan's connection); a
concurrent writer's uncommitted metadata could be picked that way, which is
why DuckDB calls it unsafe. Naming the metadata file itself as the location
reads exactly that version.
