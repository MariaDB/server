#!/usr/bin/env bash
set -euo pipefail

MYSQL="mariadb"
SOCKET=""
DATABASE=""
OUT=""
RUNS=5
WARMUP=1

usage() {
  cat <<'USAGE'
Usage: run_ptrc_benchmark.sh --socket SOCKET --database DB [options]

Options:
  --mysql PATH       MariaDB client binary. Default: mariadb
  --out DIR          Output directory. Default: /tmp/mariadb-ptrc-bench-<ts>
  --runs N           Measured rounds per mode. Default: 5
  --warmup N         Warmup rounds per mode. Default: 1
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --mysql) MYSQL="$2"; shift 2 ;;
    --socket) SOCKET="$2"; shift 2 ;;
    --database) DATABASE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ -z "$SOCKET" ] || [ -z "$DATABASE" ]; then
  usage >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-/tmp/mariadb-ptrc-bench-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"/{plans,status,results,tmp}

mysql_base=("$MYSQL" --no-defaults --socket="$SOCKET" "$DATABASE")
mysql_batch=("${mysql_base[@]}" --batch --raw --skip-column-names)

run_sql() {
  local sql_file="$1"
  "${mysql_base[@]}" < "$sql_file"
}

run_inline() {
  local sql="$1"
  printf '%s\n' "$sql" | "${mysql_base[@]}"
}

hash_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

write_env() {
  {
    date
    echo "mysql=$MYSQL"
    echo "socket=$SOCKET"
    echo "database=$DATABASE"
    "$MYSQL" --no-defaults --socket="$SOCKET" \
      --batch --raw --skip-column-names \
      -e "SELECT VERSION(); SHOW VARIABLES LIKE 'optimizer_switch';"
  } > "$OUT/environment.txt"
}

mode_setup() {
  local sq="$1"
  local ptrc="$2"
  cat "$SCRIPT_DIR/sql/session_setup.sql"
  printf "SET optimizer_switch='subquery_cache=%s,partial_result_cache=%s';\n" \
    "$sq" "$ptrc"
}

capture_plan() {
  local workload="$1"
  local mode="$2"
  local query_file="$3"
  local setup_file="$OUT/tmp/setup_${mode}.sql"
  local query
  query="$(cat "$query_file")"

  {
    cat "$setup_file"
    printf 'EXPLAIN %s\n' "$query"
    printf 'EXPLAIN FORMAT=JSON %s\n' "$query"
    printf 'ANALYZE FORMAT=JSON %s\n' "$query"
  } | "${mysql_base[@]}" > "$OUT/plans/${workload}_${mode}.txt"
}

capture_status() {
  local workload="$1"
  local mode="$2"
  local phase="$3"
  run_sql "$SCRIPT_DIR/sql/capture_status.sql" \
    > "$OUT/status/${workload}_${mode}_${phase}.txt"
}

run_query_once() {
  local setup_file="$1"
  local query_file="$2"
  local output_file="$3"
  { cat "$setup_file"; cat "$query_file"; } | "${mysql_batch[@]}" > "$output_file"
}

measure_query() {
  local workload="$1"
  local mode="$2"
  local round="$3"
  local setup_file="$4"
  local query_file="$5"
  local result_file="$OUT/results/${workload}_${mode}_${round}.txt"
  local time_file="$OUT/tmp/time_${workload}_${mode}_${round}.txt"

  /usr/bin/time -p -o "$time_file" \
    bash -c 'cat "$1" "$2" | "$3" --no-defaults --socket="$4" --batch --raw --skip-column-names "$5"' \
    _ "$setup_file" "$query_file" "$MYSQL" "$SOCKET" "$DATABASE" > "$result_file"

  local seconds
  seconds="$(awk '/^real / {print $2}' "$time_file")"
  local digest
  digest="$(hash_file "$result_file")"
  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$workload" "$mode" "$round" "$seconds" "$digest" >> "$OUT/summary.tsv"
}

write_env
printf 'workload\tmode\tround\tseconds\tresult_sha256\n' > "$OUT/summary.tsv"

for workload in q17 nlj_case4; do
  query_file="$SCRIPT_DIR/sql/${workload}.sql"
  for mode in sqoff_ptrcoff sqoff_ptrcon sqon_ptrcoff sqon_ptrcon; do
    case "$mode" in
      sqoff_ptrcoff) sq=off; ptrc=off ;;
      sqoff_ptrcon) sq=off; ptrc=on ;;
      sqon_ptrcoff) sq=on; ptrc=off ;;
      sqon_ptrcon) sq=on; ptrc=on ;;
    esac

    setup_file="$OUT/tmp/setup_${mode}.sql"
    mode_setup "$sq" "$ptrc" > "$setup_file"
    run_sql "$setup_file"
    capture_plan "$workload" "$mode" "$query_file"

    for i in $(seq 1 "$WARMUP"); do
      run_query_once "$setup_file" "$query_file" \
        "$OUT/results/${workload}_${mode}_warmup_${i}.txt"
    done

    capture_status "$workload" "$mode" "before"
    for i in $(seq 1 "$RUNS"); do
      measure_query "$workload" "$mode" "$i" "$setup_file" "$query_file"
    done
    capture_status "$workload" "$mode" "after"
  done
done

echo "Wrote benchmark artifacts to $OUT"
