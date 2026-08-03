#!/usr/bin/env bash
# Shared configuration for the TPC-H DuckDB-engine benchmark kit.
# Every statement runs directly through the mariadb client against
# ENGINE=DUCKDB tables (no run_in_duckdb UDF).
# Override any value via environment, e.g.  SF=1 ./04_load.sh
#
#   SF         TPC-H scale factor                          (default 10)
#   DATA_DIR   where .tbl files are generated/loaded from (default /git/tpch/sf<SF>)
#   SCHEMA     MariaDB database holding the ENGINE=DUCKDB tables (default bench)
#   TPCH_SQL   MariaDB-dialect query file (source of the 22 queries)
#   MARIADB    mariadb client command

CONFIG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SF="${SF:-10}"
DATA_DIR="${DATA_DIR:-/git/tpch/sf${SF}}"
SCHEMA="${SCHEMA:-bench}"
TPCH_SQL="${TPCH_SQL:-$CONFIG_DIR/tpch.sql}"
MARIADB="${MARIADB:-mariadb}"

TABLES=(region nation supplier customer part partsupp orders lineitem)

# The DuckDB engine only accepts utf8mb3/utf8mb4/ascii column charsets, so the
# client connection must use utf8mb4 (otherwise CREATE TABLE inherits the
# server's default, e.g. latin1, and fails with "non-utf8 charset").
CHARSET="${CHARSET:-utf8mb4}"

# Run one SQL statement directly through the mariadb client, server-wide
# (no default database). Use for CREATE DATABASE and other global DDL.
mdb() {
  "$MARIADB" --default-character-set="$CHARSET" -N -e "$1"
}

# Run one SQL statement directly through the mariadb client within $SCHEMA.
mdb_db() {
  "$MARIADB" --default-character-set="$CHARSET" -N -D "$SCHEMA" -e "$1"
}

# Run one statement on the embedded DuckDB through MariaDB's run_in_duckdb.
# Single quotes in $1 are escaped so SQL string/identifier literals survive.
# Used for DuckDB-native SQL (e.g. read_parquet()) against ENGINE=DUCKDB tables,
# which are addressable inside DuckDB as <database>.<table>.
# Prints the UDF result text (callers discard it when not needed).
duck() {
  local esc
  esc=$(printf '%s' "$1" | sed "s/'/''/g")
  "$MARIADB" --default-character-set="$CHARSET" -N -e "SELECT run_in_duckdb('$esc')"
}
