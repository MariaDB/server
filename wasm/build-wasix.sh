#!/usr/bin/env bash
# Build the WASIX lite4mariadb module (wasixcc / Wasmer runtime).
# Reuses the native host tools from build.sh (build-native/).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_BUILD="${NATIVE_BUILD:-$ROOT/build-native}"
WASIX_BUILD="${WASIX_BUILD:-$ROOT/build-wasix}"

if [[ -f "$HOME/.wasixcc/env" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/.wasixcc/env"
fi

if ! command -v wasixcc >/dev/null 2>&1; then
  echo "wasixcc not on PATH; install wasixcc and source ~/.wasixcc/env" >&2
  exit 1
fi

# /tmp is a small tmpfs on some systems; keep compiler temps on disk.
export TMPDIR="${TMPDIR:-$ROOT/build-wasix-tmp}"
mkdir -p "$TMPDIR"

JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$NATIVE_BUILD/import_executables.cmake" ]]; then
  echo "==> Native host tools in $NATIVE_BUILD"
  cmake -S "$ROOT" -B "$NATIVE_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_EMBEDDED_SERVER=OFF \
    -DWITH_UNIT_TESTS=OFF \
    -DWITH_WSREP=OFF \
    -DWITH_SSL=system \
    -DWITH_ZLIB=system \
    -DWITH_PCRE=system \
    -DWITH_LIBFMT=bundled \
    -DUPDATE_SUBMODULES=OFF \
    -DWITH_SYSTEMD=no \
    -DPLUGIN_COLUMNSTORE=NO \
    -DPLUGIN_ROCKSDB=NO \
    -DPLUGIN_MROONGA=NO \
    -DPLUGIN_SPIDER=NO \
    -DPLUGIN_CONNECT=NO \
    -DPLUGIN_OQGRAPH=NO \
    -DPLUGIN_DUCKDB=NO \
    -DPLUGIN_S3=NO \
    -DPLUGIN_VIDEX=NO
  cmake --build "$NATIVE_BUILD" --target import_executables -j"$JOBS"
fi

echo "==> WASIX configure in $WASIX_BUILD"
cmake -S "$ROOT" -B "$WASIX_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/wasm/wasix-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DIMPORT_EXECUTABLES="$NATIVE_BUILD/import_executables.cmake" \
  -DWITH_EMBEDDED_SERVER=ON \
  -DUPDATE_SUBMODULES=OFF

echo "==> WASIX build lite4mariadb-wasix"
cmake --build "$WASIX_BUILD" --target lite4mariadb-wasix -j"$JOBS"

echo "==> Artifact:"
ls -la "$ROOT/wasm/dist/lite4mariadb.wasix.wasm"
