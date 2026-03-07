#!/bin/sh
# Run tests relevant to CALL / named params. Run from repo root.
# Usage: ./mysql-test/run_call_tests.sh [--quick|--main|--main-ps|--push]

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MTR="$ROOT/build/mysql-test/mtr"

if [ ! -x "$MTR" ]; then
  echo "Build or MTR not found. Run: cd build && ninja"
  exit 1
fi

cd "$ROOT/build/mysql-test"

case "${1:-}" in
  --quick)
    echo "=== Quick: call_named_param (default + ps-protocol) ==="
    ./mtr --force call_named_param
    ./mtr --force --ps-protocol call_named_param
    ;;
  --main)
    echo "=== Main suite ==="
    ./mtr --force --suite-timeout=120 --max-test-fail=10 --retry=3 --suite=main
    ;;
  --main-ps)
    echo "=== Main suite with --ps-protocol ==="
    ./mtr --force --ps-protocol --suite-timeout=120 --max-test-fail=10 --retry=3 --suite=main
    ;;
  --push)
    echo "=== default.push: n_mix ==="
    ./mtr --timer --force --parallel=auto --comment=n_mix --vardir=var-n_mix --mysqld=--binlog-format=mixed --skip-test-list=collections/disabled-per-push.list
    echo "=== default.push: ps_row ==="
    ./mtr --timer --force --parallel=auto --comment=ps_row --vardir=var-ps_row --ps-protocol --mysqld=--binlog-format=row --skip-test-list=collections/disabled-per-push.list
    ;;
  *)
    echo "Usage: $0 --quick | --main | --main-ps | --push"
    echo "  --quick   call_named_param only (default + ps-protocol)"
    echo "  --main    full main suite"
    echo "  --main-ps main suite with prepared statements"
    echo "  --push    first two default.push runs (n_mix, ps_row)"
    exit 1
    ;;
esac
