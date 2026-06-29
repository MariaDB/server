# Shared Resources Between MariaDB Client Connections (DuckDB Engine)

## Architecture Overview

All MariaDB threads sharing DuckDB tables operate against a **single `duckdb::DuckDB` instance** managed by the `DuckdbManager` singleton. Each thread (THD) receives its own `duckdb::Connection` with an independent transaction context.

```
┌─────────────────────────────────────────────────┐
│          DuckdbManager  (singleton)              │
│  ┌───────────────────────────────────────────┐  │
│  │  duckdb::DuckDB                           │  │
│  │  • catalog (schemas, tables, functions)   │  │
│  │  • buffer pool / block manager            │  │
│  │  • WAL                                    │  │
│  │  • temp directory (swap)                  │  │
│  └─────────────────────┬─────────────────────┘  │
└────────────────────────┼────────────────────────┘
                         │ CreateConnection()
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
  Connection #1    Connection #2    Connection #3
  (THD A)          (THD B)          (THD C)
```

## Shared (global)

| Resource | Location | Notes |
|----------|----------|-------|
| `duckdb::DuckDB` instance | `DuckdbManager::m_database` | Single database file `duckdb.db` |
| Persistent catalog (schemas, tables) | inside DuckDB | All connections read/write the same tables |
| Buffer pool / memory allocator | inside DuckDB | Bounded by `global_memory_limit` |
| WAL (write-ahead log) | on disk | One WAL per instance |
| Temp/swap directory | on disk | Shared spill area |
| SQL macros (adddate, datediff, …) | DuckDB catalog | Registered once at init |
| C++ UDFs (`register_mysql_compat_functions`) | DuckDB catalog | Registered once at init |
| Table function `_mdb_scan` + replacement scan | DuckDB catalog | Cross-engine scan infrastructure |
| `Duckdb_share` (per TABLE_SHARE) | `Handler_share` | Contains `THR_LOCK` for MariaDB table-level locking |
| Global sysvar values | `duckdb_config.h` globals | `memory_limit`, `max_threads`, `checkpoint_threshold`, etc. |

## Per-connection (isolated)

| Resource | Location | Notes |
|----------|----------|-------|
| `duckdb::Connection` | `DuckdbThdContext::m_con` | Own `ClientContext` + transaction state |
| Transaction (BEGIN/COMMIT/ROLLBACK) | inside Connection | MVCC-isolated snapshots |
| Temporary tables (`CREATE TEMPORARY TABLE`) | Connection's temp catalog | Invisible to other connections |
| `DeltaAppenders` (batch DML buffers) | `DuckdbThdContext::m_appenders` | Temp tables + Appender per target table |
| Session settings (timezone, collation, optimizers) | `DuckdbThdContext` cached fields | Propagated to DuckDB via `SET` on change |
| `BatchState` | `DuckdbThdContext::batch_state` | Insert-only vs mixed batch mode |
| Scan state (`QueryResult`, `DataChunk`) | `ha_duckdb` instance fields | Per-open-table cursor |
| Thread-local external table registry | `tls_external_tables` | Cross-engine scan; set/cleared per query |

## Concurrency Implications

- **Reads** are MVCC-isolated: each connection sees a consistent snapshot.
- **Writes** to the same persistent table from multiple connections are serialized by DuckDB's internal write lock (single-writer model in DuckDB).
- **Temporary tables** are fully private — no naming collisions between sessions possible.
- **MariaDB THR_LOCK** in `Duckdb_share` gates concurrent access at the MariaDB handler level (read/write lock per table).
- **Buffer pool pressure** is shared: one connection doing a large scan can evict pages needed by another.
