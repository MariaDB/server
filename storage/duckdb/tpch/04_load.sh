#!/usr/bin/env bash
# Populate $SCHEMA.* from the generated Parquet files. Loading runs on the
# embedded DuckDB via run_in_duckdb (the duck helper) with read_parquet():
# the ENGINE=DUCKDB tables created in step 3 are addressable inside DuckDB as
# <database>.<table>, so INSERT ... SELECT * FROM read_parquet() fills them
# server-side without round-tripping the data through the MariaDB client.
# Times each table and prints row counts.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

echo "== read_parquet load from $DATA_DIR into database '$SCHEMA' (wall clock incl. client) =="
total=0
for t in "${TABLES[@]}"; do
  f="$DATA_DIR/$t.parquet"
  [ -f "$f" ] || { echo "ERROR: missing $f (run ./02_generate.sh)" >&2; exit 1; }
  duck "TRUNCATE $SCHEMA.$t" >/dev/null 2>&1 || true
  start=$(date +%s.%N)
  duck "INSERT INTO $SCHEMA.$t SELECT * FROM read_parquet('$f')" >/dev/null
  end=$(date +%s.%N)
  total=$(awk -v a="$total" -v s="$start" -v e="$end" 'BEGIN{print a+(e-s)}')
  awk -v s="$start" -v e="$end" -v t="$t" 'BEGIN{printf "%-10s %9.3f s\n", t, e-s}'
done
awk -v a="$total" 'BEGIN{printf "%-10s %9.3f s\n", "TOTAL", a}'

echo "== row counts =="
for t in "${TABLES[@]}"; do
  printf "%-10s " "$t"
  mdb_db "SELECT count(*) FROM $t" | grep -Eo '^[0-9]+$' | tail -1
done
