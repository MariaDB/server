# Stage 1 Task Checklist

## Goal
- Convert the Stage 1 implementation plan into an execution checklist with file-by-file work items.
- Keep scope limited to Stage 1:
  - implement/fix handlerton `commit()` and `rollback()`
  - implement/fix handler `store_lock()`, `external_lock()`, `create()`, `open()`, `write_row()`, `rnd_init()`, `rnd_next()`, `close()`
  - add the missing SELECT handler
- Do not implement Stage 3 XA/recovery hooks or Stage 3 DML/schema operations.

## Suggested work order
1. Fix build wiring and add missing source files.
2. Add shared metadata / REST / object-store helpers.
3. Rework handlerton transaction state and callbacks.
4. Rework per-table handler lifecycle and row staging.
5. Add SELECT handler pushdown path.
6. Add tests.

## File-by-file checklist

### [storage/parquet/CMakeLists.txt](/Users/ryanzhou/Desktop/server/storage/parquet/CMakeLists.txt)
- [ ] Keep the Parquet plugin source list consistent with the actual files in the directory.
- [ ] Add the new Stage 1 sources to the plugin target:
  - `ha_parquet_pushdown.cc`
  - `parquet_catalog.cc`
  - `parquet_object_store.cc`
  - `parquet_metadata.cc`
  - optional `parquet_duckdb_utils.cc`
- [ ] Add matching headers to include paths if any new private include directory is introduced.
- [ ] Remove any library dependency that is no longer needed after refactor.
- [ ] Keep DuckDB and CURL linkage explicit and local to this plugin.
- [ ] Add a unit-test target for `parquet_test.cc` if the project conventions allow local unit targets here.

### [storage/parquet/ha_parquet.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.h)
- [ ] Replace the current `parquet_trx_data` with a real Stage 1 transaction mailbox structure.
- [ ] Add structured staged-file metadata, for example:
  - catalog identifier
  - table identifier
  - object-store path
  - local temp file path
  - record count
  - file size
- [ ] Add per-table metadata state to the handler, for example:
  - catalog endpoint / namespace / table name
  - storage location / object-store prefix
  - resolved `BLOCK_SIZE`, `COMPRESSION_CODEC`, `PARQUET_VERSION`
  - active file list cache for scans
- [ ] Add declarations for Stage 1 helper methods such as:
  - metadata loading/parsing
  - buffer initialization
  - typed row append
  - flush-to-object-store
  - scan setup from active Iceberg files
- [ ] Add members for bounded write buffering:
  - estimated buffered bytes
  - buffered row count
  - flush threshold in bytes
  - appender / prepared ingestion state
- [ ] Remove or neutralize condition-pushdown-only state that should not be used in Stage 1.
- [ ] Add handlerton helper declarations if they remain in this file, otherwise move them into helper headers.

### [storage/parquet/ha_parquet.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc)

#### Handlerton section
- [ ] Implement `parquet_commit(handlerton *hton, THD *thd, bool all)`.
- [ ] Implement `parquet_rollback(handlerton *hton, THD *thd, bool all)`.
- [ ] Update `ha_parquet_init()` to wire:
  - `create`
  - `commit`
  - `rollback`
  - `create_select`
- [ ] Leave `prepare`, `recover`, `commit_by_xid`, and `rollback_by_xid` unset.
- [ ] Remove `HA_NO_TRANSACTIONS` from `table_flags()`.

#### Handler lifecycle
- [ ] Rework `create()` so it creates/registers the Iceberg table through LakeKeeper instead of writing a local placeholder Parquet file.
- [ ] Parse and validate Stage 1 engine configuration from supported MariaDB inputs.
- [ ] Apply Stage 1 defaults when optional settings are absent:
  - latest supported Parquet version
  - `16MB` block size
  - `Gzip` compression
- [ ] Rework `open()` to:
  - load persisted engine metadata
  - create a bounded in-memory DuckDB session
  - initialize scan/write state
  - avoid path guessing from `table->s->table_name.str`
- [ ] Rework `close()` to free all DuckDB objects and handler-owned state.

#### Locking / transaction registration
- [ ] Fix `store_lock()` to follow a sane Stage 1 MariaDB engine pattern.
- [ ] Rework `external_lock()` to:
  - call `trans_register_ha()` when appropriate
  - create the THD mailbox if missing
  - flush buffered rows on unlock
  - append staged files only on successful flush
- [ ] Fix the current reversed `s3_path.empty()` logic.

#### Write path
- [ ] Replace SQL-string-based row insertion with typed DuckDB ingestion.
- [ ] Stop relying on `Field::val_str()` for every column.
- [ ] Preserve:
  - NULLs
  - binary data
  - temporal values
  - decimal precision
- [ ] Track flush threshold by bytes, not only row count.
- [ ] Export buffered data to a local temp Parquet file on flush.
- [ ] Upload or stream the temp file to object storage.
- [ ] Record staged file metadata into the THD mailbox.
- [ ] Clear the in-memory buffer after a successful flush.

#### Read path
- [ ] Rework `rnd_init()` to fetch active data files from LakeKeeper/Iceberg metadata.
- [ ] Build the DuckDB scan from the active file list, not from a guessed local filename.
- [ ] Return an empty result cleanly when the table has no active files.
- [ ] Rework `rnd_next()` to support full Stage 1 type round-tripping:
  - integer families
  - floating-point
  - decimal
  - string/text
  - binary/blob
  - date/time/timestamp
  - boolean/bit
- [ ] Centralize type conversion helpers instead of keeping ad-hoc inline conversions.

#### Condition handling
- [ ] Make `cond_push()` a safe Stage 1 fallback by returning the original condition.
- [ ] Make `cond_pop()` a no-op.
- [ ] Remove any logic that makes MariaDB think filtering was pushed down when it was not.

### [storage/parquet/ha_parquet_pushdown.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.h)
- [ ] Create this header.
- [ ] Declare the SELECT handler class for Stage 1 DuckDB pushdown.
- [ ] Declare the handlerton factory:
  - `create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)`
- [ ] Define the private state needed for per-query execution:
  - per-query DuckDB database / connection
  - printed SQL text
  - result handle
  - prepared-state guard
  - external-table registry

### [storage/parquet/ha_parquet_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc)
- [ ] Create this file so the plugin build matches `CMakeLists.txt`.
- [ ] Implement the `create_duckdb_select_handler(...)` factory.
- [ ] Return `nullptr` unless at least one referenced table is a Parquet-engine table.
- [ ] Gate Stage 1 support to safe query shapes only.
- [ ] Implement the Stage 1 select-handler class:
  - [ ] constructor(s)
  - [ ] `prepare()`
  - [ ] `init_scan()`
  - [ ] `next_row()`
  - [ ] `end_scan()`
- [ ] In `prepare()`, follow MariaDB `select_handler` conventions and create the temp output table once.
- [ ] In `init_scan()`:
  - print the query text
  - create a fresh DuckDB session
  - register Parquet tables from active Iceberg file lists
  - register non-Parquet tables using a bounded Stage 1 strategy
  - execute the query in DuckDB
- [ ] In `next_row()`, copy each DuckDB result row into `table->record[0]`.
- [ ] In `end_scan()`, free query result state and tear down the per-query DuckDB session.
- [ ] Add clean failure handling so unsupported pushdown returns control safely.

### [storage/parquet/parquet_metadata.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_metadata.h)
- [ ] Create this header for Stage 1 metadata structures and parsing helpers.
- [ ] Define shared structs for:
  - engine table metadata
  - staged file metadata
  - active file descriptors for scans
- [ ] Define helper interfaces for loading/saving engine-private metadata.

### [storage/parquet/parquet_metadata.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_metadata.cc)
- [ ] Create this file.
- [ ] Implement metadata parsing from MariaDB create/open context.
- [ ] Implement Stage 1 default resolution for:
  - Parquet version
  - block size
  - compression codec
- [ ] Implement load/save helpers for any persistent engine metadata needed by `open()`.

### [storage/parquet/parquet_catalog.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_catalog.h)
- [ ] Create this header for LakeKeeper / Iceberg REST operations.
- [ ] Declare helpers for:
  - create table
  - fetch active snapshot metadata
  - append staged Parquet files into a new snapshot

### [storage/parquet/parquet_catalog.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_catalog.cc)
- [ ] Create this file.
- [ ] Implement Stage 1 REST calls to LakeKeeper using CURL.
- [ ] Implement table creation.
- [ ] Implement active file-list fetch for scans.
- [ ] Implement append commit for staged Parquet files.
- [ ] Return structured success/failure information instead of raw strings.

### [storage/parquet/parquet_object_store.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_object_store.h)
- [ ] Create this header for Stage 1 object-store operations.
- [ ] Declare helpers for:
  - upload/stream local Parquet files
  - delete staged files on rollback
  - inspect uploaded object size if needed

### [storage/parquet/parquet_object_store.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_object_store.cc)
- [ ] Create this file.
- [ ] Implement Stage 1 upload helper for Parquet flushes.
- [ ] Implement rollback deletion helper.
- [ ] Keep object-store operations separate from handlerton/handler logic.

### [storage/parquet/parquet_duckdb_utils.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_duckdb_utils.h)
- [ ] Optional but recommended: create a small DuckDB utility header.
- [ ] Declare helpers for:
  - schema mapping
  - type conversion
  - temp table creation
  - appender setup
  - file-list scan query generation

### [storage/parquet/parquet_duckdb_utils.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_duckdb_utils.cc)
- [ ] Optional but recommended: move reusable DuckDB logic out of `ha_parquet.cc`.
- [ ] Implement shared MariaDB <-> DuckDB conversion helpers.
- [ ] Implement shared Parquet `COPY` option generation with Stage 1 defaults.

### [storage/parquet/parquet_test.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_test.cc)
- [ ] Keep the current schema-mapping tests.
- [ ] Add tests for Stage 1 helper logic that is easy to unit test:
  - metadata default resolution
  - connection/config parsing
  - type mapping
  - file-list-to-query generation
- [ ] Avoid coupling this unit test to live LakeKeeper or object-store access.

## Test files to add

### [mysql-test/storage_engine/parquet or similar suite path]
- [ ] Add MTR coverage for `CREATE TABLE ... ENGINE=PARQUET`.
- [ ] Add MTR coverage for `INSERT` followed by `COMMIT`.
- [ ] Add MTR coverage for `ROLLBACK` removing staged files.
- [ ] Add MTR coverage for `SELECT` from an empty Parquet table.
- [ ] Add MTR coverage for reading multiple flushed Parquet files in one table.
- [ ] Add MTR coverage for a join between one Parquet table and one non-Parquet table.
- [ ] Add a regression test ensuring Stage 1 `cond_push()` does not produce incorrect filtering behavior.

## Concrete implementation phases

### Phase 1: Build + skeletons
- [ ] Add `ha_parquet_pushdown.h/.cc`.
- [ ] Add helper headers/sources for metadata, catalog, and object store.
- [ ] Update `CMakeLists.txt` to compile everything.

### Phase 2: Shared helpers
- [ ] Implement metadata structs and parsing.
- [ ] Implement catalog REST helpers.
- [ ] Implement object-store upload/delete helpers.
- [ ] Implement DuckDB utility helpers.

### Phase 3: Handlerton
- [ ] Replace the current THD mailbox structure.
- [ ] Wire `commit()`, `rollback()`, and `create_select`.
- [ ] Remove incorrect transaction capability flags.

### Phase 4: Handler write path
- [ ] Rework `create()`.
- [ ] Rework `open()`.
- [ ] Rework `store_lock()` and `external_lock()`.
- [ ] Rework `write_row()` and flush staging.
- [ ] Rework `close()`.

### Phase 5: Handler read path
- [ ] Rework `rnd_init()` to use active Iceberg files.
- [ ] Rework `rnd_next()` conversions.
- [ ] Make `cond_push()` / `cond_pop()` safe Stage 1 fallbacks.

### Phase 6: SELECT handler
- [ ] Implement factory gating logic.
- [ ] Implement query printing and DuckDB query setup.
- [ ] Implement Parquet table registration from Iceberg metadata.
- [ ] Implement non-Parquet table registration strategy.
- [ ] Implement row streaming back through MariaDB `select_handler`.

### Phase 7: Tests
- [ ] Wire unit tests if supported.
- [ ] Add MTR tests for commit/rollback/read/join flows.
- [ ] Add regressions for the current prototype bugs.

## Minimum done definition for Stage 1
- [ ] Plugin builds successfully with all referenced sources present.
- [ ] `CREATE TABLE ... ENGINE=PARQUET` registers a real Iceberg table.
- [ ] `INSERT` stages Parquet files to object storage and commits them through the catalog on `COMMIT`.
- [ ] `ROLLBACK` deletes staged objects and does not publish files to the catalog.
- [ ] Table scans read the active Iceberg snapshot file list.
- [ ] The Stage 1 select handler runs eligible queries through DuckDB.
- [ ] Unsupported query shapes fail safely or fall back safely.
