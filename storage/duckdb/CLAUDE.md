# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

DuckDB storage engine plugin for MariaDB. Creates tables with `ENGINE=DuckDB` that store data in DuckDB's columnar format and execute analytical queries through DuckDB's vectorized engine. Lives under `storage/duckdb/` in the MariaDB server tree.

## Build

```bash
# Build + install + start MariaDB (most common during development)
./build.sh -S -t Debug

# CI mode (build only, no install)
./build.sh -c -t Debug

# Build with packages
./build.sh -p -t RelWithDebInfo
```

The build directory is created as a sibling: `../DuckdbBuildOf_<source_dir_name>/`. DuckDB itself is built from source at `third_parties/duckdb/` via `ExternalProject_Add` and merged into a single `libduckdb_bundle.a`.

## Tests (MTR)

Tests use MariaDB's MTR (MySQL Test Runner) framework. Test files live in `mysql-test/duckdb/t/` with expected results in `mysql-test/duckdb/r/`.

```bash
# Run a single test
./run_mtr.sh create_table_column

# Run and record new expected output
./run_mtr.sh -r create_table_column

# Run all tests
./run_mtr.sh -a

# Run against an externally running MariaDB
./run_mtr.sh -e create_table_column

# Run all against extern server
./run_mtr.sh -a -e
```

## Architecture

All engine code is in the `myduck` namespace (except `ha_duckdb` which is in global scope per MariaDB handler convention).

### Key components

- **`ha_duckdb`** (`ha_duckdb.cc/h`) — MariaDB `handler` subclass. Entry point for all storage engine operations (open, close, read, write, DDL). Implements row-at-a-time interface for MariaDB, translating to DuckDB batch operations.

- **`DuckdbManager`** (`duckdb_manager.cc/h`) — Singleton owning the `duckdb::DuckDB` instance. Created once at plugin init, creates connections for each thread. Database file stored as `duckdb.db` in MariaDB data directory.

- **`DuckdbThdContext`** (`duckdb_context.cc/h`) — Per-thread context holding a `duckdb::Connection`, transaction state, and appenders. Attached to MariaDB's THD. Manages BEGIN/COMMIT/ROLLBACK lifecycle and session variable propagation (timezone, optimizer flags, collation).

- **DDL/DML Convertors** (`ddl_convertor.cc/h`, `dml_convertor.cc/h`) — Translate MariaDB SQL structures (TABLE, Field, Alter_info) into DuckDB-compatible SQL strings. Handle identifier quoting differences (MariaDB backticks → DuckDB double quotes) and type mapping.

- **`DeltaAppender`** (`delta_appender.cc/h`) — Batched write path. Accumulates INSERT/UPDATE/DELETE rows using DuckDB's Appender API into a temporary buffer table, then flushes as a single DuckDB DML statement at commit time. `DeltaAppenders` manages per-table appender instances.

- **Select handler / Query pushdown** (`ha_duckdb_pushdown.cc/h`) — Implements MariaDB's `select_handler` interface to push entire SELECT queries down to DuckDB. Registered via `hton->create_select` and `hton->create_unit`. Supports pure-DuckDB queries and cross-engine joins.

- **Cross-engine scan** (`cross_engine_scan.cc/h`) — Enables DuckDB to read from non-DuckDB tables (e.g. InnoDB) during cross-engine joins. Registers a `_mdb_scan` table function and a replacement scan callback in DuckDB. Uses a thread-local registry of external tables.

- **Type mapping** (`duckdb_types.cc/h`) — Converts between MariaDB field types and DuckDB types. `store_duckdb_field_in_mysql_format()` reads DuckDB values back into MariaDB row format.

### SQL generation conventions

All generated SQL must use **double quotes** for identifiers (DuckDB follows SQL standard), not backticks. The `SELECT_LEX::print()` output from MariaDB uses backticks and must be post-processed. See `docs/mariadb-duckdb-incompatibilities.md` for known function name rewrites and type mapping issues.

### DuckDB source and patches

DuckDB source is at `third_parties/duckdb/` (git submodule). No patches are applied — all compatibility is handled at runtime via `duckdb_mysql_compat.cc`. The build produces a static library; `_GLIBCXX_DEBUG` is explicitly undefined in CMakeLists.txt to avoid ABI mismatch with MariaDB's debug build.

### Configuration

`duckdb.cnf` — MariaDB config snippet that loads `ha_duckdb.so`. Installed to `/etc/my.cnf.d/`.
