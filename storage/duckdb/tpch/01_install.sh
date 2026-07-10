#!/usr/bin/env bash
# Install tpchgen-cli (fast pure-Rust TPC-H data generator).
# Tries pip, then uv, then cargo. Idempotent.
set -euo pipefail

if command -v tpchgen-cli >/dev/null 2>&1; then
  echo "tpchgen-cli already installed: $(tpchgen-cli --version 2>/dev/null || echo present)"
  exit 0
fi

echo "Installing tpchgen-cli ..."
if command -v pip3 >/dev/null 2>&1; then
  pip3 install --user tpchgen-cli
elif command -v pip >/dev/null 2>&1; then
  pip install --user tpchgen-cli
elif command -v uv >/dev/null 2>&1; then
  uv tool install tpchgen-cli
elif command -v cargo >/dev/null 2>&1; then
  RUSTFLAGS='-C target-cpu=native' cargo install tpchgen-cli
else
  echo "ERROR: need one of pip3/pip, uv, or cargo to install tpchgen-cli" >&2
  exit 1
fi

command -v tpchgen-cli >/dev/null 2>&1 \
  || { echo "ERROR: tpchgen-cli not on PATH after install (check ~/.local/bin or ~/.cargo/bin)" >&2; exit 1; }
echo "Installed: $(tpchgen-cli --version 2>/dev/null || echo present)"
