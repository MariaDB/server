#!/usr/bin/env bash
# Generate TPC-H data (Parquet) at scale factor $SF into $DATA_DIR.
# Skips generation if all .parquet files already exist (set FORCE=1 to regenerate).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

command -v tpchgen-cli >/dev/null 2>&1 || { echo "ERROR: run ./01_install.sh first" >&2; exit 1; }

mkdir -p "$DATA_DIR"

missing=0
for t in "${TABLES[@]}"; do [ -f "$DATA_DIR/$t.parquet" ] || missing=1; done
if [ "$missing" = 0 ] && [ "${FORCE:-0}" != 1 ]; then
  echo "All .parquet files already present in $DATA_DIR (set FORCE=1 to regenerate)."
  exit 0
fi

echo "Generating TPC-H SF$SF (Parquet) into $DATA_DIR ..."
tpchgen-cli -s "$SF" --format=parquet --output-dir "$DATA_DIR"
ls -la "$DATA_DIR"/*.parquet
