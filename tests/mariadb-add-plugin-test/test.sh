#!/usr/bin/env bash
# Usage: test.sh <mariadb-package.tar.gz>
#
# Builds the plugins in this directory against the given MariaDB package
# (an unpacked-tarball layout, e.g. mariadb-X.Y.Z-linux-x86_64.tar.gz)
# using nothing but find_package(mariadb-plugin) + MARIADB_ADD_PLUGIN(),
# the way an external plugin author would - then verifies mariadbd can
# actually load the resulting server plugins (MDEV-40608).
#
# NOTE: mariadbd's --plugin-load-add does not fail the process on a bad
# plugin - sql_plugin.cc discards plugin_load_list()'s return value at
# the call site in plugin_init(), so a failed load is only ever logged,
# never reflected in the exit code. This script therefore greps output
# for the failure message instead of trusting the exit code.

set -euxo pipefail

if [ $# -ne 1 ]; then
  echo "usage: $0 <mariadb-package.tar.gz>" >&2
  exit 2
fi

ARCHIVE_SRC=$(readlink -f "$1")
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ ! -f "$ARCHIVE_SRC" ]; then
  echo "error: no such file: $ARCHIVE_SRC" >&2
  exit 2
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT
echo "== work dir: $WORKDIR"

# Unpack into a fresh temp dir unrelated to wherever the archive was
# built/downloaded, so the test can't accidentally depend on that
# location - only on paths computed from the unpacked tree itself
# (MDEV-40608's whole point).
mkdir -p "$WORKDIR/unpacked"
tar xzf "$ARCHIVE_SRC" -C "$WORKDIR/unpacked"

INSTALL_ROOT=$(find "$WORKDIR/unpacked" -mindepth 1 -maxdepth 1 -type d)
if [ -z "$INSTALL_ROOT" ]; then
  echo "error: unpacked archive has no top-level directory" >&2
  exit 1
fi
echo "== install root: $INSTALL_ROOT"

BUILD_DIR="$WORKDIR/build"
mkdir -p "$BUILD_DIR"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DCMAKE_PREFIX_PATH="$INSTALL_ROOT"
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"

MARIADBD="$INSTALL_ROOT/bin/mariadbd"
PLUGINDIR="$INSTALL_ROOT/lib/plugin"
if [ ! -x "$MARIADBD" ]; then
  echo "error: $MARIADBD not found or not executable" >&2
  exit 1
fi

check_plugin_loads() {
  local lib="$1"
  local expect="$2"
  local outfile="$WORKDIR/help-$lib.log"
  echo "== checking $lib loads into mariadbd"
  # Redirect to a file rather than capturing via $(...): --help --verbose
  # output is large, and this avoids any command-substitution buffering/
  # truncation surprises with it.
  "$MARIADBD" --no-defaults --plugin-dir="$PLUGINDIR" \
    --plugin-load-add="$lib" --help --verbose > "$outfile" 2>&1 || true
  if grep -qiE "couldn't load plugin|\[ERROR\]" "$outfile"; then
    echo "FAIL: $lib produced a load error:" >&2
    grep -iE "couldn't load plugin|\[ERROR\]" "$outfile" >&2
    exit 1
  fi
  if ! grep -q -- "$expect" "$outfile"; then
    echo "FAIL: $lib loaded without a logged error, but its option ('$expect') never appeared in --help output" >&2
    exit 1
  fi
  echo "OK: $lib"
}

check_plugin_loads "dummy_auth.so" "dummy-auth"
check_plugin_loads "ha_dummy_storage_engine.so" "dummy-se"

echo "== dummy_client_auth.so built and installed - client plugins only"
echo "   load during a connection handshake, not checked by this script"
test -f "$PLUGINDIR/dummy_client_auth.so"

echo "ALL OK"
