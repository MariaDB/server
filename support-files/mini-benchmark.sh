#!/bin/bash
# Abort on errors
set -ex

display_help() {
  echo "Usage: $(basename "$0") [-h] [--perf] [--perf-flamegraph]"
  echo
  echo "This is a very small and naive benchmark script designed to be suitable"
  echo "for running in a CI system on every commit to detect severe performance"
  echo "regressions."
  echo
  echo "optional arguments:"
  echo "  --name STRING      identifier for the benchmark, added to the "
  echo "                     folder name and (if --log is set) the log file "
  echo "  --threads \"STRING\" quoted string of space-separated integers "
  echo "                     representing the threads to run."
  echo "                     example: --threads \"1 32 64 128\""
  echo "                     default: \"1 2 4 8 16\""
  echo "  --duration INTEGER duration of each thread run in seconds"
  echo "                     default: 60"
  echo "  --workload STRING  sysbench workload to execute"
  echo "                     default: oltp_read_write"
  echo "  --log              logs the mini-benchmark stdout/stderr into the"
  echo "                     benchmark folder."
  echo "  --perf             measure CPU cycles and instruction count in for "
  echo "                     sysbench runs"
  echo "  --perf-flamegraph  record performance counters in perf.data.* and"
  echo "                     generate flamegraphs automatically"
  echo "  --cpu-limit        upper limit on the number of CPU cycles (in billions) used for the benchmark"
  echo "                     default: 750"
  echo "  -h, --help         display this help and exit"
}

# Default parameters
BENCHMARK_NAME='mini-benchmark'
THREADS='1 2 4 8 16'
DURATION=60
WORKLOAD='oltp_read_write'

while :
do
  case "$1" in
    -h | --help)
      display_help
      exit 0
      ;;
    --version)
      display_version
      exit 0
      ;;
    --name)
      shift
      BENCHMARK_NAME+='-'
      BENCHMARK_NAME+=$1
      shift
      ;;
    --threads)
      shift
      THREADS=$1
      shift
      ;;
    --duration)
      shift
      DURATION=$1
      shift
      ;;
    --workload)
      shift
      WORKLOAD=$1
      shift
      ;;
    --log)
      LOG=true
      shift
      ;;
    --perf)
      PERF=true
      shift
      ;;
    --perf-flamegraph)
      PERF_RECORD=true
      shift
      ;;
    --cpu-limit)
      shift
      CPU_CYCLE_LIMIT=$1
      shift
      ;;
    -*)
      echo "Error: Unknown option: $1" >&2
      ## or call function display_help
      exit 1
      ;;
    *)  # No more options
      break
      ;;
  esac
done

# Save results of this run in a subdirectory so that they are not overwritten by
# the next run
TIMESTAMP="$(date -Iseconds)"
mkdir "$BENCHMARK_NAME-$TIMESTAMP"
cd "$BENCHMARK_NAME-$TIMESTAMP" || exit 1

(
# Check that the dependencies of this script are available
if [ ! -e /usr/bin/pgrep ]
then
  echo "ERROR: Command 'pgrep' missing, please install package 'psproc'"
  exit 1
fi

if [ ! -e /usr/bin/sysbench ]
then
  echo "ERROR: Command 'sysbench' missing, please install package 'sysbench'"
  exit 1
fi

# If there are multiple processes, assume the last one is the actual server and
# any potential other ones were just part of the service wrapper chain
# shellcheck disable=SC2005
MARIADB_SERVER_PID="$(echo "$(pgrep -f mariadbd || pgrep -f mysqld)" | tail -n 1)"

if [ -z "$MARIADB_SERVER_PID" ]
then
  echo "ERROR: Server 'mariadbd' or 'mysqld' is not running, please start the service"
  exit 1
fi

if [ "$PERF" == true ] && [ "$PERF_RECORD" == true ]
then
  echo "ERROR: Cannot select both --perf and --perf-flamegraph options simultaneously. Please choose one or the other."
  exit 1
fi

if [ "$PERF" == true ] || [ "$PERF_RECORD" == true ]
then
  if [ ! -e /usr/bin/perf ]
  then
    echo "ERROR: Command 'perf' missing, please install package 'perf'"
    exit 1
  fi
fi

if [ "$PERF_RECORD" == true ]
then
  if [ ! -e /usr/bin/flamegraph.pl ]
  then
    echo "ERROR: Command 'flamegraph.pl' missing, please install package 'flamegraph'"
    exit 1
  fi

  if [ ! -e /usr/bin/stackcollapse-perf.pl ]
  then
    echo "ERROR: Command 'stackcollapse-perf.pl' missing, please install package 'flamegraph-stackcollapse-perf'"
    exit 1
  fi

  if [ ! -e /usr/bin/debuginfo-install ]
  then
    echo "ERROR: Command 'debuginfo-install' missing, please install package 'dnf-utils'"
    exit 1
  fi

  echo "Ensure the MariaDB Server debug symbols are installed"
  for x in $(ldd /usr/sbin/mariadbd | grep -oE " /.* ")
  do
    rpm -q --whatprovides --qf '%{name}' "$x" | cut -d : -f 1
  done | sort -u > mariadbd-dependencies.txt
  # shellcheck disable=SC2046
  debuginfo-install -y mariadb-server $(cat mariadbd-dependencies.txt)

  if ! (perf record echo "testing perf") > /dev/null 2>&1
  then
    echo "perf does not have permission to run on this system. Skipping."
    PERF_COMMAND=""
  else
    echo "Using 'perf' to record performance counters in perf.data files"
    PERF_COMMAND="perf record -g --freq=99 --output=perf.data --timestamp-filename --pid=$MARIADB_SERVER_PID --"
  fi

elif [ "$PERF" == true ]
then
  # If flamegraphs were not requested, log normal perf counters if possible

  if ! (perf stat echo "testing perf") > /dev/null 2>&1
  then
    echo "perf does not have permission to run on this system. Skipping."
    PERF_COMMAND=""
  else
    echo "Using 'perf' to log basic performance counters for benchmark"
    PERF_COMMAND="perf stat -p $MARIADB_SERVER_PID --"
  fi
fi

# Run sysbench on another CPU if system has more than one available
if [ "$(nproc)" -gt 1 ]
then
  TASKSET_SYSBENCH='taskset -c 1'
else
  TASKSET_SYSBENCH=''
fi

echo "System hardware information:"
lscpu
free -m
df -h .
uname -a
echo

echo "Set highest priority for MariaDB Server process ID $MARIADB_SERVER_PID"
renice --priority -20 --pid "$MARIADB_SERVER_PID" || echo "renice failed. Not setting priority."

echo "Set CPU affinity 0 for MariaDB Server process ID $MARIADB_SERVER_PID"
taskset -cp 0 "$MARIADB_SERVER_PID" || echo "taskset failed. Not setting cpu affinity."

mariadb -e "
  CREATE DATABASE IF NOT EXISTS sbtest;
  CREATE USER IF NOT EXISTS sbtest@localhost;
  GRANT ALL PRIVILEGES ON sbtest.* TO sbtest@localhost"

sysbench "$WORKLOAD" prepare --tables=20 --table-size=100000 | tee sysbench-prepare.log
sync && sleep 1 # Ensure writes were propagated to disk

# Run benchmark with increasing thread counts. The MariaDB Server will be using
# around 300 MB of RAM and mostly reading and writing in RAM, so I/O usage is
# also low. The benchmark will most likely be CPU bound to due to the load
# profile, and also guaranteed to be CPU bound because of being limited to a
# single CPU with 'tasksel'.
for t in $THREADS
do
  # Prepend command with perf if defined
  # Output stderr to stdout as perf outputs everything in stderr
  # shellcheck disable=SC2086
  $PERF_COMMAND $TASKSET_SYSBENCH sysbench "$WORKLOAD" run --threads=$t --time=$DURATION --report-interval=10 2>&1 | tee sysbench-run-$t.log
done

sysbench "$WORKLOAD" cleanup --tables=20 | tee sysbench-cleanup.log

# Store results from 4 thread run in a Gitlab-CI compatible metrics file
grep -oE '[a-z]+:[ ]+[0-9.]+' sysbench-run-4.log | sed -r 's/\s+/ /g' | tail -n 15 > metrics.txt

echo # Newline improves readability
echo "== SUMMARY =="

# Print performance counter summary if they were logged
if grep --quiet cycles sysbench-run-*.log
then
  grep -e cycles sysbench-run-*.log | sort -k 2
  echo "Total: $(grep -h -e cycles sysbench-run-*.log | sort -k 1 | awk '{s+=$1}END{print s}')"
  echo # Newline improves readability
  grep -e instructions sysbench-run-*.log | sort -k 2
  echo "Total: $(grep -h -e instructions sysbench-run-*.log | sort -k 1 | awk '{s+=$1}END{print s}')"
  echo # Newline improves readability

  if [ -z "$CPU_CYCLE_LIMIT" ]
  then 
     # 04-04-2024: We found this to be an appropriate default limit after running a few benchmarks
     # Configure the limit with --cpu-limit if needed
    CPU_CYCLE_LIMIT=750
  fi
  CPU_CYCLE_LIMIT_LONG="${CPU_CYCLE_LIMIT}000000000"

  # Final verdict based on cpu cycle count
  RESULT="$(grep -h -e cycles sysbench-run-*.log | sort -k 1 | awk '{s+=$1}END{print s}')"
  if [ "$RESULT" -gt "$CPU_CYCLE_LIMIT_LONG" ]
  then
    echo # Newline improves readability
    echo "Benchmark exceeded the allowed limit of ${CPU_CYCLE_LIMIT} billion CPU cycles"
    echo "Performance most likely regressed!"
    exit 1
  fi
fi

# List all sysbench status lines at once
grep -h thds sysbench-run-*.log | sort -k 5 -h

echo # Newline improves readability
echo "Highest count for queries per second:"
sort -k 9 -h sysbench-run-*.log | tail -n 1

if [ "$PERF_RECORD" == true ]
then
  for f in perf.data.*
  do
    perf script -i "$f" | stackcollapse-perf.pl | flamegraph.pl --width 1800 > "$f".svg
  done
  echo "Flamegraphs stored in folder $BENCHMARK_NAME-$TIMESTAMP/"
fi

# Fallback if CPU cycle count not available: final verdict based on peak QPS
RESULT="$(sort -k 9 -h sysbench-run-*.log | tail -n 1 | grep -oE "qps: [0-9]+" | grep -oE "[0-9]+")"
case $RESULT in
  ''|*[!0-9]*)
    echo "ERROR: Benchmark result invalid, not an integer."
    exit 1
    ;;

  *)
    if [ "$RESULT" -lt 13000 ]
    then
      echo # Newline improves readability
      echo "Benchmark did not reach 13000+ qps, performance most likely regressed!"
      exit 1
    else
      echo "Banchmark passed with $RESULT queries per second as peak value"
    fi
    ;;
esac
# Record the output into the log file, if requested
) 2>&1 | ($LOG && tee "$BENCHMARK_NAME"-"$TIMESTAMP".log)
exit ${PIPESTATUS[0]} # Propagate errors in the sub-shell
