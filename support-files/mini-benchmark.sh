#!/bin/bash
# Abort on errors
set -e

display_help() {
  echo "Usage: $(basename "$0") [-h] [--perf] [--perf-flamegraph]"
  echo
  echo "This is a very small and naive benchmark script designed to be suitable"
  echo "for running in a CI system on every commit to detect severe performance"
  echo "regressions."
  echo
  echo "optional arguments:"
  echo "  --perf             measure CPU cycles and instruction count in for "
  echo "                     sysbench runs"
  echo "  --perf-flamegraph  record performance counters in perf.data.* and"
  echo "                     generate flamegraphs automatically"
  echo "  -h, --help         display this help and exit"
}

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
    --perf)
      PERF=true
      shift
      ;;
    --perf-flamegraph)
      PERF_RECORD=true
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
MARIADB_SERVER_PID="$(echo "$(pgrep -f mariadbd || pgrep -f mysqld)" | tail -n 1)"

if [ -z "$MARIADB_SERVER_PID" ]
then
  echo "ERROR: Server 'mariadbd' or 'mysqld' is not running, please start the service"
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
    rpm -q --whatprovides --qf '%{name}' $x | cut -d : -f 1
  done | sort -u > mariadbd-dependencies.txt
  # shellcheck disable=SC2046
  debuginfo-install -y mariadb-server $(cat mariadbd-dependencies.txt)

  echo "Using 'perf' to record performance counters in perf.data files"
  PERF="perf record -g --freq=99 --output=perf.data --timestamp-filename --pid=$MARIADB_SERVER_PID --"

elif [ -e /usr/bin/perf ]
then
  # If flamegraphs were not requested, log normal perf counters if possible
  echo "Using 'perf' to log basic performance counters for benchmark"
fi
  PERF="perf stat -p $MARIADB_SERVER_PID --"

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
renice --priority -20 --pid "$MARIADB_SERVER_PID"

echo "Set CPU affinity 0 for MariaDB Server process ID $MARIADB_SERVER_PID"
taskset -cp 0 "$MARIADB_SERVER_PID"

mariadb -e "
  CREATE DATABASE IF NOT EXISTS sbtest;
  CREATE USER IF NOT EXISTS sbtest@localhost;
  GRANT ALL PRIVILEGES ON sbtest.* TO sbtest@localhost"

sysbench oltp_read_write prepare --tables=20 --table-size=100000 | tee sysbench-prepare.log
sync && sleep 1 # Ensure writes were propagated to disk

# Save results of this run in a subdirectory so that they are not overwritten by
# the next run
TIMESTAMP="$(date -Iseconds)"
mkdir "mini-benchmark-$TIMESTAMP"
cd "mini-benchmark-$TIMESTAMP" || exit 1

# Run benchmark with increasing thread counts. The MariaDB Server will be using
# around 300 MB of RAM and mostly reading and writing in RAM, so I/O usage is
# also low. The benchmark will most likely be CPU bound to due to the load
# profile, and also guaranteed to be CPU bound because of being limited to a
# single CPU with 'tasksel'.
for t in 1 2 4 8 16
do
  # Prepend command with perf if defined
  # Output stderr to stdout as perf outpus everything in stderr
  $PERF $TASKSET_SYSBENCH sysbench oltp_read_write run --threads=$t --time=60 --report-interval=10 2>&1 | tee sysbench-run-$t.log
done

sysbench oltp_read_write cleanup --tables=20 | tee sysbench-cleanup.log

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

  # Final verdict based on cpu cycle count
  RESULT="$(grep -h -e cycles sysbench-run-*.log | sort -k 1 | awk '{s+=$1}END{print s}')"
  if [ "$RESULT" -gt 850000000000 ]
  then
    echo # Newline improves readability
    echo "Benchmark exceeded 8.5 billion cpu cycles, performance most likely regressed!"
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
    perf script -i $f | stackcollapse-perf.pl | flamegraph.pl --width 3000 > $f.svg
  done
  echo "Flamegraphs stored in folder mini-benchmark-$TIMESTAMP/"
fi

# Fallback if CPU cycle count not availalbe: final verdict based on peak QPS
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
