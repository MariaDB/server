#!/usr/bin/env bash
# Generate TPC-H data (.tbl, pipe-delimited) at scale factor $SF into $DATA_DIR.
# Skips generation if all .tbl files already exist (set FORCE=1 to regenerate).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

command -v tpchgen-cli >/dev/null 2>&1 || { echo "ERROR: run ./01_install.sh first" >&2; exit 1; }

mkdir -p "$DATA_DIR"

missing=0
for t in "${TABLES[@]}"; do [ -f "$DATA_DIR/$t.tbl" ] || missing=1; done
if [ "$missing" = 0 ] && [ "${FORCE:-0}" != 1 ]; then
  echo "All .tbl files already present in $DATA_DIR (set FORCE=1 to regenerate)."
  exit 0
fi

echo "Generating TPC-H SF$SF (.tbl) into $DATA_DIR ..."
tpchgen-cli -s "$SF" --output-dir "$DATA_DIR"
ls -la "$DATA_DIR"/*.tbl
