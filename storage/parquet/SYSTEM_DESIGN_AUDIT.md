# Parquet Storage Engine Audit Against System Design

## Scope
- This is a static source review of the current working tree under `storage/parquet`.
- Files reviewed: `storage/parquet/CMakeLists.txt`, `storage/parquet/ha_parquet.h`, `storage/parquet/ha_parquet.cc`, and `storage/parquet/parquet_test.cc`.
- This document compares the current implementation to the provided design. It does not claim end-to-end runtime validation.

## Overall Assessment
- The current code is a prototype of a DuckDB-backed local Parquet handler.
- It is not yet the S3 + Iceberg + LakeKeeper storage engine described in the system design.
- The biggest gaps are the missing handlerton transaction callbacks, missing select handler, and missing catalog/object-store integration.

## Unimplemented
- Handlerton transaction lifecycle is missing. `ha_parquet_init()` only assigns `create`; it does not wire `commit`, `rollback`, `prepare`, `recover`, `commit_by_xid`, or `rollback_by_xid` (`storage/parquet/ha_parquet.cc:631-636`).
- The custom select handler is missing. `storage/parquet/CMakeLists.txt:4-9` references `ha_parquet_pushdown.cc`, but that file does not exist and there are no implementations of `create_duckdb_select_handler()`, `init_scan()`, `next_row()`, `end_scan()`, or select-handler `prepare()`.
- LakeKeeper / Iceberg catalog management is missing. There is no REST client code, no table registration, no snapshot commit logic, no optimistic concurrency control, and no time-travel support anywhere under `storage/parquet`.
- Engine options and defaults are missing. There is no handling for `CONNECTION`, `CATALOG`, `PARQUET_VERSION`, `BLOCK_SIZE`, or `COMPRESSION_CODEC`, and no code that applies the design defaults.
- `update_row()` and `delete_row()` are still stubbed out with `HA_ERR_WRONG_COMMAND` (`storage/parquet/ha_parquet.cc:462-469`).
- `alter_table()` is not implemented at all; there is no override in `storage/parquet/ha_parquet.h`.
- Iceberg append/delete/merge logic is missing. There is no equality delete generation, predicate delete files, snapshot append, merge/upsert rewrite, file pruning, or compaction code in the current implementation.
- Cross-engine joins are effectively unimplemented because the select-handler path and external-table registry described in the design do not exist.
- The small unit test in `storage/parquet/parquet_test.cc` is not wired into `storage/parquet/CMakeLists.txt`, so there is no implemented automated verification path for the plugin logic reviewed here.

## Implemented Wrong
- The build configuration is inconsistent with the source tree. `storage/parquet/CMakeLists.txt:4-9` requires `ha_parquet_pushdown.cc`, but that file is missing.
- `table_flags()` advertises `HA_NO_TRANSACTIONS` (`storage/parquet/ha_parquet.cc:24-27`), which conflicts with the design's transaction model and even with stage 1's required `commit()` / `rollback()` support.
- `open()` only creates a DuckDB connection and installs/loads the Iceberg extension (`storage/parquet/ha_parquet.cc:34-65`). It never creates the Iceberg view described in the design, never uses catalog metadata, and does not check query results for failure.
- `close()` does not release any resources (`storage/parquet/ha_parquet.cc:69-72`). The heap allocations declared in `storage/parquet/ha_parquet.h:62-72` are never deleted.
- `create()` writes a local `*.parquet` file and a local `duckdb_helper.duckdb` helper database (`storage/parquet/ha_parquet.cc:277-335`) instead of asking LakeKeeper to create Iceberg metadata and return an S3 storage path.
- The read and write paths disagree about file location. `create()` writes to `std::string(name) + ".parquet"` (`storage/parquet/ha_parquet.cc:295-296`), but `rnd_init()` reads `table->s->table_name.str + ".parquet"` (`storage/parquet/ha_parquet.cc:482-483`), which is usually just the bare table name.
- Block-size-based flushing is effectively disabled. `flush_threshold` is an unsigned `uint64_t` (`storage/parquet/ha_parquet.h:57-58`) but `open()` sets it to `-1` (`storage/parquet/ha_parquet.cc:39-40`), so `write_row()` almost never reaches the flush condition (`storage/parquet/ha_parquet.cc:448-456`).
- `flush_remaining_rows_to_s3()` is not real S3 integration. It hard-codes an `s3://parquet-bucket/...` path and issues a DuckDB `COPY` directly to it (`storage/parquet/ha_parquet.cc:364-385`) without object-store configuration, credential handling, or MariaDB/external S3 internals.
- The unlock path stores mailbox data only when the flush path is empty. In `external_lock()`, the condition is reversed: `if (s3_path.empty() == true) { ... push_back(s3_path); }` (`storage/parquet/ha_parquet.cc:596-603`). That means successful flushes are not recorded, while empty paths are.
- `cond_push()` claims full condition pushdown by returning `nullptr` (`storage/parquet/ha_parquet.cc:125-140`), but the pushed condition is never applied in `rnd_init()` or `rnd_next()` (`storage/parquet/ha_parquet.cc:472-549`). That can produce wrong query results because MariaDB may believe the storage engine handled the filter.
- `rnd_init()` does not do the fallback flow from the design. It does not ask LakeKeeper for active Parquet files and does not filter in memory; it just reads a single file via `read_parquet()` (`storage/parquet/ha_parquet.cc:482-493`).
- Type round-tripping is only partially implemented. `mariadb_type_to_duckdb()` maps many MariaDB types (`storage/parquet/ha_parquet.cc:150-218`), but `rnd_next()` only has explicit handling for a few numeric families and coerces everything else through `ToString()` (`storage/parquet/ha_parquet.cc:523-545`). That is not a faithful implementation of the mapped schema.
- Compression/version/block-size defaults from the design are not honored. The Parquet export helper never sets compression codec, Parquet version, or block size (`storage/parquet/ha_parquet.cc:269-274`).

## Implemented Correctly
- The implementation is currently contained in the plugin area rather than requiring core SQL-parser changes. The active worktree changes are confined to `storage/parquet/*`, which matches the design requirement to avoid core MariaDB changes.
- The storage engine is registered under the name `PARQUET`, and a handler factory is wired through the handlerton (`storage/parquet/ha_parquet.cc:624-665`).
- The handler skeleton covers the main stage-1 read/write entry points described in the design: `open`, `close`, `create`, `write_row`, `rnd_init`, `rnd_next`, `store_lock`, and `external_lock` are all declared and implemented (`storage/parquet/ha_parquet.h:30-52`, `storage/parquet/ha_parquet.cc:34-65`, `69-72`, `277-335`, `389-459`, `472-549`, `573-621`).
- DuckDB is used as the internal execution/buffering engine, which matches the architecture. `open()` creates an in-memory DuckDB instance and sets a session memory limit (`storage/parquet/ha_parquet.cc:49-55`).
- The type-mapping helper is a correct starting point for MariaDB-to-DuckDB/Parquet schema translation for common scalar types (`storage/parquet/ha_parquet.cc:150-218`).
- Parquet file generation is delegated to DuckDB's `COPY ... (FORMAT PARQUET)` mechanism (`storage/parquet/ha_parquet.cc:269-274`), which is the right primitive for producing valid Parquet data files.
- `write_row()` follows the intended buffering pattern at a high level: rows are inserted into an in-memory DuckDB table before export (`storage/parquet/ha_parquet.cc:389-459`).
- `rnd_next()` does perform the basic handler responsibility of pulling values from DuckDB and storing them back into MariaDB fields (`storage/parquet/ha_parquet.cc:505-549`).
- There is at least minimal unit-test coverage for the schema-mapping helper in `storage/parquet/parquet_test.cc:16-64`, even though the test is not wired into the build.

## Conclusion
- The current code is best described as an early local DuckDB/Parquet prototype.
- The S3 streaming, Iceberg metadata management, LakeKeeper catalog integration, transaction lifecycle, and select-handler pushdown architecture described in the design are still mostly absent.
- The most important correctness issues to fix before adding more features are: remove `HA_NO_TRANSACTIONS`, fix `cond_push()` semantics, fix the mailbox/unlock bug, align read/write paths, implement real cleanup in `close()`, and either add or remove the missing `ha_parquet_pushdown.cc` build dependency.
