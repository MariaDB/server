#!/usr/bin/env bash
# Populate $SCHEMA.* with COPY from the generated .tbl files (DuckDB reads the
# pipe-delimited, header-less, trailing-'|' tbl format with DELIMITER '|').
# Times each table and prints row counts.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

echo "== COPY load from $DATA_DIR into schema '$SCHEMA' (wall clock incl. client) =="
total=0
for t in "${TABLES[@]}"; do
  f="$DATA_DIR/$t.tbl"
  [ -f "$f" ] || { echo "ERROR: missing $f (run ./02_generate.sh)" >&2; exit 1; }
  duck "TRUNCATE $SCHEMA.$t" >/dev/null 2>&1 || true
  start=$(date +%s.%N)
  duck "COPY $SCHEMA.$t FROM '$f' (DELIMITER '|')" >/dev/null
  end=$(date +%s.%N)
  total=$(awk -v a="$total" -v s="$start" -v e="$end" 'BEGIN{print a+(e-s)}')
  awk -v s="$start" -v e="$end" -v t="$t" 'BEGIN{printf "%-10s %9.3f s\n", t, e-s}'
done
awk -v a="$total" 'BEGIN{printf "%-10s %9.3f s\n", "TOTAL", a}'

echo "== row counts =="
for t in "${TABLES[@]}"; do
  printf "%-10s " "$t"
  duck "SELECT count(*) FROM $SCHEMA.$t" | grep -Eo '^[0-9]+$' | tail -1
done
