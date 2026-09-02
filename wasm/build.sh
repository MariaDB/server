#!/usr/bin/env bash
# Build host import_executables, then the WASM lite4mariadb package.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_BUILD="${NATIVE_BUILD:-$ROOT/build-native}"
WASM_BUILD="${WASM_BUILD:-$ROOT/build-wasm}"
EMSDK="${EMSDK_ROOT:-$HOME/emsdk}"

if [[ -f "$EMSDK/emsdk_env.sh" ]]; then
  # shellcheck disable=SC1091
  source "$EMSDK/emsdk_env.sh"
fi

if ! command -v emcmake >/dev/null 2>&1; then
  echo "emcmake not on PATH; install emsdk and source emsdk_env.sh" >&2
  exit 1
fi

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

echo "==> WASM configure in $WASM_BUILD"
emcmake cmake -S "$ROOT" -B "$WASM_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DIMPORT_EXECUTABLES="$NATIVE_BUILD/import_executables.cmake" \
  -DWITH_EMBEDDED_SERVER=ON \
  -DUPDATE_SUBMODULES=OFF

echo "==> WASM build lite4mariadb"
cmake --build "$WASM_BUILD" --target lite4mariadb -j"$JOBS"

echo "==> Artifacts in $ROOT/wasm/dist"
ls -la "$ROOT/wasm/dist"
