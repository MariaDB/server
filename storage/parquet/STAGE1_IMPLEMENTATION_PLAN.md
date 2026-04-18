# Stage 1 Implementation Plan

## Scope
- This plan covers only Stage 1 work.
- In scope: handlerton `commit()` / `rollback()`, handler `store_lock()`, `external_lock()`, `create()`, `open()`, `write_row()`, `rnd_init()`, `rnd_next()`, `close()`, and the missing SELECT handler.
- Out of scope: `prepare()`, `recover()`, `commit_by_xid()`, `rollback_by_xid()`, `delete_row()`, `update_row()`, `alter_table()`.
- Stage 2 condition pushdown is also out of scope. For Stage 1, `cond_push()` / `cond_pop()` should be made safe no-ops so the server keeps filtering rows itself.

## Handlerton

### Goals
- Make Stage 1 writes transactional at commit/rollback time.
- Register the SELECT handler entry point.
- Keep all Stage 3 XA/recovery hooks unimplemented.

### Functions to implement or fix
- `ha_parquet_init()`
- New handlerton callbacks:
  - `parquet_commit(handlerton *hton, THD *thd, bool all)`
  - `parquet_rollback(handlerton *hton, THD *thd, bool all)`
  - `create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)`

### Plan
1. Define a real per-THD transaction mailbox.
   - Replace the current `parquet_trx_data` vector-only shape with structured staged-write metadata.
   - Track, per table:
     - catalog endpoint / namespace / table identifier
     - object-store prefix
     - staged Parquet files
     - per-file record count
     - per-file size in bytes
     - optional local temp path for rollback cleanup
   - Keep this state attached with `thd_get_ha_data()` / `thd_set_ha_data()`.

2. Wire only Stage 1 callbacks in `ha_parquet_init()`.
   - Set `parquet_hton->commit = parquet_commit`.
   - Set `parquet_hton->rollback = parquet_rollback`.
   - Set `parquet_hton->create_select = create_duckdb_select_handler`.
   - Leave `prepare`, `recover`, `commit_by_xid`, and `rollback_by_xid` unset.
   - Do not advertise features the engine does not yet support.

3. Fix transaction capability signaling.
   - Remove `HA_NO_TRANSACTIONS` from `table_flags()`.
   - Keep only flags that are true for the Stage 1 engine.
   - Make sure the handler only registers with MariaDB transaction management once per THD/table access path.

4. Implement `commit()`.
   - Treat the THD mailbox as the source of truth for staged files.
   - Group staged files by Iceberg table.
   - For each table:
     - fetch current Iceberg table metadata from LakeKeeper
     - append staged Parquet files to a new snapshot
     - commit the new snapshot through the catalog REST API
   - On success:
     - clear staged file bookkeeping
     - delete any local temp artifacts
     - clear the THD mailbox entry
   - On failure:
     - return an engine error without silently dropping staged state
     - leave rollback cleanup possible

5. Implement `rollback()`.
   - Read all staged files from the THD mailbox.
   - Delete uploaded Parquet files from object storage.
   - Delete any local temp artifacts if they still exist.
   - Discard staged Iceberg commit state in memory.
   - Clear the THD mailbox entry even if object-store cleanup is best-effort.

6. Add a small REST/object-store helper layer.
   - Keep the handlerton callbacks thin.
   - Move LakeKeeper REST calls and S3 delete/upload helpers into reusable utilities under `storage/parquet`.
   - Return structured status codes instead of raw strings.

### Exit criteria
- `COMMIT` registers staged files in LakeKeeper and clears mailbox state.
- `ROLLBACK` removes staged files and clears mailbox state.
- The engine no longer claims `HA_NO_TRANSACTIONS`.
- The handlerton exposes a select-handler factory.

## Handler

### Goals
- Make the per-table handler behave like the Stage 1 design instead of a local-file prototype.
- Use DuckDB for bounded in-memory buffering and Parquet generation.
- Stage data during `write_row()` / unlock, but only publish on handlerton `commit()`.

### Functions to implement or fix
- `create()`
- `open()`
- `close()`
- `store_lock()`
- `external_lock()`
- `write_row()`
- `rnd_init()`
- `rnd_next()`
- Safe Stage 1 fallback for `cond_push()` / `cond_pop()`

### Plan
1. Introduce engine-private table metadata.
   - Add a metadata structure on the handler for:
     - catalog endpoint
     - namespace + table name
     - object-store base path
     - resolved Parquet settings
     - active snapshot/file list cache
   - Because core parser changes are disallowed, prefer parser-supported inputs first.
   - Recommended Stage 1 approach:
     - read `CONNECTION` from MariaDB's existing create-info path
     - parse catalog/object-store details from that connection string
     - apply internal defaults for `PARQUET_VERSION`, `BLOCK_SIZE`, and `COMPRESSION_CODEC` if those options are not already exposed without parser changes

2. Rework `create()`.
   - Validate that every column type is supported by the Stage 1 type-mapping layer.
   - Convert the MariaDB schema into the Iceberg schema expected by LakeKeeper.
   - Call the catalog REST API to create the Iceberg table and obtain its storage location.
   - Persist any engine-private metadata needed by `open()`.
   - Do not create a local `*.parquet` file just to satisfy `CREATE TABLE`.

3. Rework `open()`.
   - Load the engine-private metadata for the table.
   - Create a DuckDB in-memory session.
   - Set a bounded memory limit derived from the Stage 1 block-size target.
   - Load only the DuckDB extensions needed for Stage 1.
   - Initialize scan/write state cleanly:
     - reset batch byte counters
     - reset staged-result state
     - create no buffer tables until first write
   - Remove path guessing based on `table->s->table_name.str`.

4. Rework `close()`.
   - Free `scan_result`.
   - Destroy any appender/prepared statement state.
   - Destroy DuckDB connection and database objects.
   - Clear handler-owned strings and cached metadata.
   - Make `close()` safe to call repeatedly after partial setup failures.

5. Fix `store_lock()`.
   - Preserve MariaDB lock semantics instead of only storing the first lock type forever.
   - Follow a simple engine pattern similar to BLACKHOLE/TINA for Stage 1.
   - Avoid claiming row-level behavior the engine does not actually implement.

6. Rework `external_lock()`.
   - On read/write lock:
     - call `trans_register_ha()` exactly as needed for Stage 1
     - allocate the THD mailbox if missing
   - On unlock:
     - flush any buffered rows
     - append staged-file metadata to the mailbox only when a flush succeeds
   - Do not commit to LakeKeeper in `external_lock()`.
   - Fix the current reversed `s3_path.empty()` condition.

7. Rework `write_row()`.
   - Stop building SQL `INSERT` strings from `Field::val_str()`.
   - Use DuckDB's typed ingestion API, preferably `duckdb::Appender`, to preserve:
     - NULLs
     - binary values
     - temporal values
     - numeric precision
   - Replace row-count-only flushing with a byte-based threshold.
   - Track:
     - estimated buffered bytes
     - buffered row count
   - When the threshold is hit:
     - export the in-memory buffer to a local temporary Parquet file
     - upload or stream that file to object storage
     - measure record count and file size
     - add the staged-file metadata to the THD mailbox
     - clear the DuckDB buffer table
   - Apply Stage 1 defaults:
     - latest supported Parquet version
     - `16MB` block-size target
     - `Gzip` compression unless configured otherwise

8. Rework `rnd_init()`.
   - Do not guess a single `table_name.parquet` path.
   - Ask LakeKeeper for the active snapshot's data files for the table.
   - Build a DuckDB scan from the active file list.
   - For Stage 1, do not use condition pushdown here.
   - If no files exist yet, return an empty result set cleanly.

9. Rework `rnd_next()`.
   - Expand type conversion coverage so it matches the Stage 1 schema mapping:
     - integers
     - floating point
     - decimal
     - string/text
     - binary/blob
     - date/time/timestamp
     - boolean/bit
   - Ensure nullability is handled with `set_null()` / `set_notnull()`.
   - Keep conversion centralized in helper functions instead of embedding many ad-hoc cases inline.

10. Make Stage 2 condition hooks safe for Stage 1.
   - `cond_push()` should not claim pushdown if Stage 2 is not implemented.
   - Recommended Stage 1 behavior:
     - return the original `cond`
     - keep no pushed-condition state
   - `cond_pop()` should become a no-op.

### Exit criteria
- `CREATE TABLE` registers the Iceberg table instead of writing a local placeholder Parquet file.
- `INSERT` buffers rows in DuckDB, flushes to object storage by size, and stages files for commit.
- `SELECT` reads the active Iceberg snapshot's files rather than a guessed local path.
- `close()` releases all DuckDB resources.
- Stage 1 no longer performs unsafe pseudo-pushdown through `cond_push()`.

## SELECT Handler

### Goals
- Add the missing Stage 1 select-handler implementation referenced by CMake.
- Push eligible SELECT queries involving at least one Parquet table into DuckDB.
- Support cross-engine reads by making non-Parquet tables visible to DuckDB.

### Functions/classes to implement
- New source file(s), e.g.:
  - `ha_parquet_pushdown.h`
  - `ha_parquet_pushdown.cc`
- Factory:
  - `create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)`
- New select-handler class implementing:
  - `prepare()`
  - `init_scan()`
  - `next_row()`
  - `end_scan()`

### Plan
1. Fix the build first.
   - Add the missing `ha_parquet_pushdown.cc` file referenced by `storage/parquet/CMakeLists.txt`.
   - If needed, add a matching header.
   - Keep the plugin build self-consistent before wiring it into `create_select`.

2. Implement a Parquet select-handler class.
   - Derive from `select_handler`.
   - Store:
     - per-query DuckDB database/connection
     - printed SQL text
     - result handle
     - temporary registry for external tables
     - prepared-state guard

3. Implement the factory (`create_duckdb_select_handler`).
   - Return `nullptr` unless the query contains at least one `ENGINE=PARQUET` table.
   - Gate Stage 1 support to safe read-only query shapes first.
   - Recommended Stage 1 limitations:
     - support single SELECTs with joins
     - return `nullptr` for unsupported unit pushdown (`UNION` / `EXCEPT` / `INTERSECT`) until a `create_unit` path is added
     - return `nullptr` for side-effect or unsupported statement types

4. Implement `prepare()`.
   - Follow MariaDB's normal `select_handler::prepare()` pattern.
   - Create the temp table only once.
   - Populate `result_columns` from the temp table's item list.

5. Implement `init_scan()`.
   - Print the raw SQL query text from `SELECT_LEX` or its master unit.
   - Create a fresh DuckDB session for the query.
   - Register every referenced table in DuckDB:
     - Parquet tables:
       - resolve active data files from LakeKeeper
       - create DuckDB views or table functions over those files
     - Non-Parquet tables:
       - recommended Stage 1 approach: register a replacement-scan backed by MariaDB row iteration so DuckDB can read rows on demand
   - Execute the SQL inside DuckDB and store the result handle.

6. Implement `next_row()`.
   - Read the next DuckDB row.
   - Convert it into `table->record[0]`, not the handler's per-table record buffer.
   - Reuse the same type-conversion helpers as `rnd_next()` where possible.
   - Return `HA_ERR_END_OF_FILE` at end of stream.

7. Implement `end_scan()`.
   - Free the result handle.
   - Tear down external-table registry entries.
   - Destroy the per-query DuckDB session.
   - Leave the temp output table cleanup to the select-handler lifecycle.

8. Decide the Stage 1 cross-engine strategy explicitly.
   - Recommended path:
     - implement bounded replacement scans for non-Parquet tables
     - keep memory use bounded and avoid copying whole foreign tables into DuckDB
   - Temporary fallback if needed:
     - materialize small non-Parquet tables into DuckDB temp tables
     - only allow this under a clear memory bound
     - document that it is a fallback, not the final architecture

9. Add safe failure behavior.
   - If query printing, table registration, or DuckDB execution fails, abort select-handler execution cleanly.
   - Never return a handler that can silently produce partial or wrong results.
   - Prefer falling back to normal server execution only when the query has not yet started pushdown execution.

### Exit criteria
- The plugin builds with a real `ha_parquet_pushdown.cc`.
- SELECT queries involving at least one Parquet table can be executed through DuckDB.
- Queries that also reference non-Parquet tables have a defined Stage 1 execution path.
- Unsupported query shapes fall back safely instead of producing wrong results.

## Verification
- Add MTR coverage for:
  - `CREATE TABLE ... ENGINE=PARQUET`
  - autocommit `INSERT` + `COMMIT`
  - `ROLLBACK` removing staged files
  - `SELECT` from an empty table
  - `SELECT` after multiple flushes to multiple Parquet files
  - join between one Parquet table and one non-Parquet table
- Keep `parquet_test.cc` for helper-level unit tests, but add it to the build or move equivalent coverage into MTR.
- Add targeted tests for the current failure modes:
  - reversed unlock mailbox condition
  - mismatched read/write file paths
  - no-op `close()` leaks
  - unsafe `cond_push()` behavior

## Recommended implementation order
1. Fix build wiring and add the missing select-handler source file.
2. Fix transaction capability signaling and add handlerton `commit()` / `rollback()`.
3. Rework handler metadata flow (`create()` + `open()` + `close()`).
4. Rework write buffering/flush staging (`write_row()` + `external_lock()`).
5. Rework scan path (`rnd_init()` + `rnd_next()`).
6. Add the SELECT handler query path.
7. Add MTR tests and helper tests for the completed Stage 1 surface.
