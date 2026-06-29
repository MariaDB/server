# MariaDB ↔ DuckDB Storage Engine: Architecture

## Overview

DuckDB is embedded into MariaDB as a **storage engine plugin** (`ha_duckdb`). MariaDB owns metadata (`.frm` files), SQL parsing, optimization, and the client protocol. DuckDB owns data storage, columnar analytical execution, and MVCC transactions. The two systems communicate through MariaDB's standard handler API and the `select_handler` interface for query pushdown.

```
┌───────────────────────────────────────────────────────────┐
│                     MariaDB Server                        │
│                                                           │
│  SQL Parser → Optimizer → Executor                        │
│       │            │          │                            │
│       │     ┌──────┘          │                            │
│       │     │  select_handler │  handler API               │
│       │     │  (pushdown)     │  (row-by-row)              │
│       ▼     ▼                 ▼                            │
│  ┌─────────────────────────────────────────┐              │
│  │        ha_duckdb  (storage engine)      │              │
│  │                                         │              │
│  │  DDL convertors ──┐                     │              │
│  │  DML convertors ──┼── SQL generation    │              │
│  │  select_handler ──┘                     │              │
│  │                                         │              │
│  │  DuckdbThdContext (per THD)             │              │
│  │   └─ duckdb::Connection                 │              │
│  │       └─ transaction, session state     │              │
│  │                                         │              │
│  │  DuckdbManager (singleton)              │              │
│  │   └─ duckdb::DuckDB instance            │              │
│  │       └─ catalog, buffer pool, WAL      │              │
│  └─────────────────────────────────────────┘              │
└───────────────────────────────────────────────────────────┘
```

## Plugin Lifecycle

### Initialization (`duckdb_init_func`)

1. Registers handlerton callbacks: `create`, `commit`, `rollback`, `close_connection`, `drop_database`, `create_select`, `create_unit`.
2. `DuckdbManager::CreateInstance()` opens a single `duckdb.db` file in the MariaDB datadir, creating the `duckdb::DuckDB` instance.
3. Registers SQL macros for MySQL compatibility (`adddate`, `datediff`, `curdate`, `convert_tz`, `substring_index`, `strcmp`, etc.).
4. Registers C++ UDFs via `register_mysql_compat_functions()` — `mid`, `oct`, `bin`, `hex`, `locate`, `addtime`, `subtime`, `regexp_replace`, etc.
5. Registers cross-engine scan infrastructure: `_mdb_scan` table function + replacement scan callback.

### Shutdown (`duckdb_deinit_func`)

`DuckdbManager::Cleanup()` destroys the DuckDB instance and closes the database file.

---

## Query Processing Paths

### Path 1: SELECT Pushdown (`select_handler`)

Primary path for analytical queries. MariaDB hands the **entire SELECT** to DuckDB.

```
MariaDB parser
  → optimizer calls hton->create_select / hton->create_unit
    → can_pushdown_to_duckdb(): at least one DuckDB table?
      → creates ha_duckdb_select_handler
        → init_scan():
           1. Takes original SQL text from THD::query()
           2. Rewrites MariaDB-specific syntax for DuckDB:
              • GROUP BY ... WITH ROLLUP  →  GROUP BY ROLLUP(...)
              • CONVERT(expr, TYPE)       →  CAST(expr AS TYPE)
              • CURRENT_TIME()            →  current_time
              • STRAIGHT_JOIN             →  JOIN / CROSS JOIN
              • REGEXP / RLIKE            →  ~ / !~
              • LIMIT offset,count        →  LIMIT count OFFSET offset
              • Strips hints: HIGH_PRIORITY, SQL_NO_CACHE, FORCE INDEX, etc.
           3. Sends rewritten SQL to duckdb::Connection::Query()
        → next_row(): reads DataChunk row-by-row, converts Value → Field
        → end_scan(): releases result
```

### Path 1a: Cross-Engine Queries

When a SELECT mixes tables from **different engines** (DuckDB + InnoDB, etc.):

```
ha_duckdb_select_handler::init_scan()
  → registers non-DuckDB tables in a thread-local registry
  → sends SQL to DuckDB
    → DuckDB cannot find the table in its catalog
      → replacement scan callback redirects to _mdb_scan('table_name')
        → mdb_scan_function():
           • creates FiberScanState
           • in fiber: new THD, synthetic SELECT * FROM table WHERE …,
             mysql_execute_command()
           • MariaDB result rows → DataChunk via select_to_duckdb interceptor
           • yield() when DataChunk is full, continue() for next chunk
```

The fiber runs on the same OS thread as DuckDB. TLS (`current_thd`, `THR_KEY_mysys`) is swapped around fiber spawn/continue calls.

### Path 2: DDL via Handler API

```
MariaDB executor calls handler method
  → ha_duckdb::create() / delete_table() / rename_table() / truncate()
    → DDL convertor generates SQL for DuckDB
      (CreateTableConvertor, RenameTableConvertor, etc.)
    → SQL sent to DuckDB Connection
```

ALTER TABLE uses inplace ALTER:
```
check_if_supported_inplace_alter() → HA_ALTER_INPLACE_NO_LOCK
commit_inplace_alter_table()
  → AddColumnConvertor / DropColumnConvertor / ChangeColumnConvertor /
    ChangeColumnDefaultConvertor / ChangeColumnForPrimaryKeyConvertor
  → each operation executes in a separate auto-commit context
    (DuckDB v1.5+ disallows compound DDL mixing structural + constraint changes)
```

DROP DATABASE: `duckdb_drop_database()` → `DROP SCHEMA IF EXISTS "db"`.

### Path 3: Row-by-Row DML

```
ha_duckdb::write_row() / update_row() / delete_row()
  → duckdb_register_trx(thd)
  → if BatchState == NOT_IN_BATCH:
      InsertConvertor / UpdateConvertor / DeleteConvertor
        → generates SQL: INSERT INTO … VALUES (…) / UPDATE … / DELETE …
        → duckdb_query(connection, sql)
  → if BatchState == IN_INSERT_ONLY_BATCH or IN_MIX_BATCH:
      DeltaAppender::append_row_insert/update/delete()
        → DuckDB Appender API → temp table → flushed at commit
```

### Path 4: Direct UPDATE/DELETE (Statement Pushdown)

For single-table statements with simple WHERE (`HA_CAN_DIRECT_UPDATE_AND_DELETE`):
```
ha_duckdb::direct_delete_rows() / direct_update_rows()
  → flush_appenders()          // ensure DuckDB sees consistent data
  → take original SQL from thd_query_string()
  → send entire statement to DuckDB
  → read affected-row count from result
```

### Path 5: Table Scan Fallback (rnd_*)

When `select_handler` is not applicable:
```
rnd_init() → SELECT * FROM "schema"."table" → sent to DuckDB
rnd_next() → reads DataChunk chunk-by-chunk, converts values
rnd_end()  → releases QueryResult
```

---

## Transaction Coordination

```
MariaDB THD
  │
  ├─ external_lock(F_WRLCK/F_RDLCK)
  │    └─ duckdb_register_trx()
  │         ├─ trans_register_ha(stmt)   // register with MariaDB TC
  │         ├─ trans_register_ha(all)    // if explicit transaction
  │         └─ DuckdbThdContext::duckdb_trans_begin()  // BEGIN in DuckDB
  │
  ├─ write_row / update_row / …
  │
  ├─ duckdb_prepare()                   // flush appenders
  │
  ├─ duckdb_commit()
  │    ├─ flush_appenders()             // safety net if prepare was skipped
  │    └─ DuckdbThdContext::duckdb_trans_commit()
  │
  └─ duckdb_rollback()
       └─ DuckdbThdContext::duckdb_trans_rollback()
```

XA transactions are rejected (`reject_xa_if_active`).

---

## Batch DML (DeltaAppender)

For bulk INSERT (and mixed INSERT/UPDATE/DELETE within a single transaction):

1. Creates a **temporary table** cloning the target schema + 3 auxiliary columns (`#mdb_delete_flag`, `#mdb_row_no`, `#mdb_trx_no`).
2. Writes rows via the `duckdb::Appender` API (much faster than per-row SQL INSERT).
3. On `flush()` (at commit/prepare): executes `INSERT INTO target SELECT … FROM tmp` and/or `DELETE FROM target WHERE pk IN (SELECT pk FROM tmp WHERE delete_flag)`.
4. The temp table lives in the connection-local temporary catalog — invisible to other sessions.

---

## SQL Translation Layer

### DDL Convertors (`convertor/ddl_convertor.cc`)
Translate MariaDB DDL → DuckDB DDL:
- Data type mapping (MariaDB types → DuckDB types)
- Expression defaults (read from `TABLE_SHARE::vcol_defs` binary blob, since the Item tree is invalid at `ha_duckdb::create()` time)
- PRIMARY KEY → NOT NULL constraints (DuckDB has no indexes)
- ADD/DROP/CHANGE COLUMN, RENAME TABLE

### DML Convertors (`convertor/dml_convertor.cc`)
Generate INSERT/UPDATE/DELETE SQL from MariaDB row buffers.

### MySQL Compat Functions (`runtime/duckdb_mysql_compat.cc`)
~1000 lines of C++ scalar UDFs registered in DuckDB for MariaDB SQL compatibility: `mid`, `oct`, `bin`, `hex`, `locate`, `addtime`, `subtime`, `regexp_replace`, `field`, `elt`, etc.

### DuckDB Value → MariaDB Field (`convertor/duckdb_select.cc`)
`store_duckdb_field_in_mysql_format()` converts DuckDB `Value` objects to MariaDB `Field::store()` calls when reading query results.

---

## Source Code Map

```
storage/duckdb/
├── ha_duckdb.h/cc                — handler class, handlerton, DDL/DML entry points
├── ha_duckdb_pushdown.h/cc       — select_handler: SELECT/UNION pushdown + SQL rewriting
├── duckdb_udf.cc                 — UDF plugin registration
│
├── convertor/
│   ├── ddl_convertor.h/cc        — CREATE/ALTER/DROP/RENAME SQL generation
│   ├── dml_convertor.h/cc        — INSERT/UPDATE/DELETE SQL generation
│   └── duckdb_select.h/cc        — DuckDB Value → MariaDB Field conversion
│
├── runtime/
│   ├── duckdb_manager.h/cc       — DuckdbManager singleton, DuckDB instance lifecycle
│   ├── duckdb_context.h/cc       — DuckdbThdContext: per-THD connection + transactions
│   ├── duckdb_query.h/cc         — duckdb_query() wrappers
│   ├── duckdb_mysql_compat.h/cc  — C++ UDFs for MySQL function compatibility
│   ├── cross_engine_scan.h/cc    — _mdb_scan table function + replacement scan
│   ├── fiber_context.h/c         — stackful coroutine (fiber) primitives
│   ├── fiber_scan.h/cc           — fiber-based MariaDB scan for cross-engine queries
│   └── delta_appender.h/cc       — batch DML via Appender API + temp tables
│
├── common/
│   ├── duckdb_config.h/cc        — global sysvar definitions
│   ├── duckdb_charset_collation.h/cc
│   ├── duckdb_timezone.h/cc
│   ├── duckdb_types.h/cc         — DatabaseTableNames, path parsing
│   ├── duckdb_log.h/cc           — logging control
│   ├── duckdb_handler_errors.h   — error code constants
│   └── row_helpers.h             — byte-level row reading utilities
│
├── cmake/                        — DuckDB build integration (ExternalProject_Add)
└── third_parties/duckdb/         — upstream DuckDB submodule (v1.5.2)
```
