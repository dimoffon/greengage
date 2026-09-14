#!/usr/bin/env bash
# Create and fill the tpch schema in the current database.
#   load.sh [SF] [TABLE_OPTS]
# SF scales the TPC-H row counts (1 = 6M lineitem rows); TABLE_OPTS are
# appended to every CREATE TABLE, e.g.
#   "WITH (appendonly=true, orientation=column, compresstype=zstd)".
# Connection settings come from the PG* environment (PGDATABASE, PGPORT...).
set -euo pipefail
sf=${1:-1}
opts=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
psql -X -v ON_ERROR_STOP=1 -v opts="$opts" -f "$here/schema.sql"
psql -X -v ON_ERROR_STOP=1 -v sf="$sf" -f "$here/gen.sql"
