#!/usr/bin/env bash
# Run the 22 TPC-H queries from $TPCH_SQL against $SCHEMA via run_in_duckdb,
# timing each (wall clock, one client invocation). Writes a TSV of timings.
# Queries get the raw MariaDB-dialect text (no pushdown rewrites): any query
# using MariaDB-only syntax is reported as ERR.
set -uo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/config.sh"

[ -f "$TPCH_SQL" ] || { echo "ERROR: query file $TPCH_SQL not found" >&2; exit 1; }
OUT="${OUT:-$DIR/query_timings.tsv}"

# Split $TPCH_SQL into per-query files: Q1 is the first block (no marker),
# each "-- Qn:" marker starts the next query.
MQ="${TMPDIR:-/tmp}/tpch_mq"
mkdir -p "$MQ"; rm -f "$MQ"/q*.sql
awk 'BEGIN{idx=1} /^-- Q[0-9]+:/{idx++} {print > sprintf("'"$MQ"'/q%02d.sql", idx)}' "$TPCH_SQL"

printf "query\ttime_s\n" | tee "$OUT"
for i in $(seq 1 22); do
  n=$(printf "%02d" "$i")
  f="$MQ/q$n.sql"
  [ -f "$f" ] || continue
  combined="SET schema '$SCHEMA'; $(cat "$f")"
  esc=$(printf '%s' "$combined" | sed "s/'/''/g")

  start=$(date +%s.%N)
  out=$("$MARIADB" -N -e "SELECT run_in_duckdb('$esc')" 2>&1)
  end=$(date +%s.%N)

  if printf '%s' "$out" | grep -qiE 'error'; then
    tt="ERR"
  else
    tt=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
  fi
  printf "q%s\t%s\n" "$n" "$tt" | tee -a "$OUT"
done
echo "Timings written to $OUT"
