#!/bin/bash
# Replicate buildbot/amd64-msan-clang-20: build with MSAN and run tests that failed on CI.
# Must be run on Linux (MSAN is not supported on macOS).
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$SRC/build-msan"
mkdir -p "$BUILD"
cd "$BUILD"

# Prefer clang-20 if present (matches buildbot), else clang
if command -v clang-20 &>/dev/null && command -v clang++-20 &>/dev/null; then
  export CC=clang-20 CXX=clang++-20
elif command -v clang &>/dev/null; then
  export CC=clang CXX=clang++
fi

cmake "$SRC" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWITH_MSAN=ON \
  -GNinja

ninja mariadbd

# Run the two tests that failed on amd64-msan-clang-20
perl "$SRC/mysql-test/mariadb-test-run.pl" --tmpdir=/tmp/mtr \
  main.tmp_space_usage \
  innodb.skip_locked_nowait

echo "MSAN test run finished successfully."
