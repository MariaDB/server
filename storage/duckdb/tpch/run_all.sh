#!/usr/bin/env bash
# End-to-end: install generator, generate data, create schema, COPY-load, run queries.
# Each step is idempotent; re-running skips generation when data already exists.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$DIR/01_install.sh"
"$DIR/02_generate.sh"
"$DIR/03_schema.sh"
"$DIR/04_load.sh"
"$DIR/05_run_queries.sh"
