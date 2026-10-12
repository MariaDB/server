#!/usr/bin/env bash
set -euo pipefail

command_name=${1:-}
case "$command_name" in
  prepare|run|cleanup|smoke) ;;
  *) echo "usage: $0 {prepare|run|cleanup|smoke}" >&2; exit 2 ;;
esac

socket=${SOCKET:-/var/run/mysqld/mysqld.sock}
user=${DB_USER:-root}
database=${DATABASE:-blink_bench}
threads=${THREADS:-32}
preload_rows=${PRELOAD_ROWS:-2500000}
measured_rows=${MEASURED_ROWS:-400000}
payload_size=${PAYLOAD_SIZE:-2500}
batch_size=${BATCH_SIZE:-250}
blink=${BLINK:-OFF}
label=${LABEL:-baseline}
client=${CLIENT:-mariadb}
sysbench_bin=${SYSBENCH:-sysbench}
script_dir=$(cd "$(dirname "$0")" && pwd)
workload="$script_dir/blink_split_heavy.lua"
result_root=${RESULT_ROOT:-/tmp/blink-split-heavy-results}

mysql() {
  "$client" --no-defaults --socket="$socket" -u"$user" "$@"
}

sysbench_command() {
  "$sysbench_bin" "$workload" \
    --mysql-socket="$socket" \
    --mysql-user="$user" \
    --mysql-db="$database" \
    --mysql-ssl=off \
    --threads="$threads" \
    --preload_rows="$preload_rows" \
    --measured_rows="$measured_rows" \
    --payload_size="$payload_size" \
    --batch_size="$batch_size" \
    "$@"
}

set_blink() {
  if mysql -NBe "SHOW GLOBAL VARIABLES LIKE 'innodb_blink_enabled'" |
      grep -q '^innodb_blink_enabled'; then
    mysql -e "SET GLOBAL innodb_blink_enabled='$blink'"
  elif [[ "$blink" == ON ]]; then
    echo "innodb_blink_enabled is unavailable on $socket" >&2
    exit 1
  fi
}

prepare() {
  mysql -e "CREATE DATABASE IF NOT EXISTS \`$database\`"
  set_blink
  sysbench_command --threads=1 prepare
  mysql "$database" -e \
    "SELECT COUNT(*) AS rows_loaded, MIN(id) AS min_id, MAX(id) AS max_id FROM split_heavy"
}

run() {
  local timestamp result_dir before_splits after_splits
  timestamp=$(date -u +%Y%m%dT%H%M%SZ)
  result_dir="$result_root/${label}-t${threads}-${timestamp}"
  mkdir -p "$result_dir"

  mysql -NBe "SELECT @@version,@@version_source_revision,@@socket,@@port" \
    > "$result_dir/server.tsv"
  mysql -NBe "SHOW GLOBAL VARIABLES WHERE Variable_name IN
    ('innodb_buffer_pool_size','innodb_log_file_size',
     'innodb_flush_log_at_trx_commit','innodb_adaptive_hash_index',
     'performance_schema','innodb_blink_enabled')" \
    > "$result_dir/variables.tsv"
  mysql "$database" -NBe "SELECT COUNT(*),MIN(id),MAX(id) FROM split_heavy" \
    > "$result_dir/table-before.tsv"
  before_splits=$(mysql -NBe \
    "SHOW GLOBAL STATUS LIKE 'Innodb_buffer_pool_pages_split'" | awk '{print $2}')
  mysql -NBe "SHOW GLOBAL STATUS" > "$result_dir/status-before.tsv"

  sysbench_command \
    --events="$measured_rows" \
    --time=0 \
    --report-interval=1 \
    --histogram=on \
    run | tee "$result_dir/sysbench.txt"

  after_splits=$(mysql -NBe \
    "SHOW GLOBAL STATUS LIKE 'Innodb_buffer_pool_pages_split'" | awk '{print $2}')
  mysql -NBe "SHOW GLOBAL STATUS" > "$result_dir/status-after.tsv"
  mysql "$database" -NBe "SELECT COUNT(*),MIN(id),MAX(id) FROM split_heavy" \
    > "$result_dir/table-after.tsv"
  printf 'page_splits_before\t%s\npage_splits_after\t%s\npage_splits_delta\t%s\n' \
    "$before_splits" "$after_splits" "$((after_splits - before_splits))" \
    > "$result_dir/deltas.tsv"

  if mysql -NBe "SELECT @@performance_schema" | grep -qx 1; then
    mysql -NBe "SELECT NAME,COUNT_STAR,SUM_TIMER_WAIT,AVG_TIMER_WAIT,MAX_TIMER_WAIT
      FROM performance_schema.events_waits_summary_global_by_event_name
      WHERE NAME='wait/synch/sxlock/innodb/index_tree_rw_lock'" \
      > "$result_dir/index-latch.tsv"
  fi

  echo "$result_dir"
}

cleanup() {
  sysbench_command --threads=1 cleanup
}

if [[ "$command_name" == smoke ]]; then
  threads=${THREADS:-4}
  preload_rows=${PRELOAD_ROWS:-20000}
  measured_rows=${MEASURED_ROWS:-3200}
  batch_size=${BATCH_SIZE:-200}
  prepare
  run
else
  "$command_name"
fi
