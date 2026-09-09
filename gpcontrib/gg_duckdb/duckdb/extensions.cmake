# The extension set built into libduckdb.so when build.sh runs with
# DUCKDB_REMOTE_EXTENSIONS=1.  A configuration file replaces DuckDB's default
# list, so the in-tree ones every build needs come first; then httpfs (S3 and
# HTTP file systems), avro and iceberg (Apache Iceberg tables), out of tree
# at the commits DuckDB v1.5.5 pins in .github/config/extensions/*.cmake.
duckdb_extension_load(core_functions)
duckdb_extension_load(parquet)
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 827222fb45a043a7a852d1f7aae46901492a3cda
)
duckdb_extension_load(avro
    GIT_URL https://github.com/duckdb/duckdb-avro
    GIT_TAG f9d590297485f0318f480372c70bdd852826e258
)
duckdb_extension_load(iceberg
    GIT_URL https://github.com/duckdb/duckdb-iceberg
    GIT_TAG 45163a28e0ed6a2071a82a1bf1dd432d0216cf9c
)
