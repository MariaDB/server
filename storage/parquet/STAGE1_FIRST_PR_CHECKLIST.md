# Stage 1 First PR Checklist

## First PR objective
- Land the smallest reviewable slice that makes the Parquet plugin safer and easier to extend.
- Focus on build consistency, safe Stage 1 fallback behavior, resource cleanup, and scaffolding.
- Do not try to finish full Stage 1 in this PR.

## Explicit non-goals for this PR
- No full LakeKeeper table creation yet.
- No real Iceberg snapshot commit yet.
- No real object-store upload/delete yet.
- No working DuckDB SELECT handler pushdown yet.
- No implementation of Stage 3 hooks or Stage 3 DML/schema operations.

## What this PR should accomplish
- The plugin source tree and build files are internally consistent.
- The current prototype no longer claims features it does not implement safely.
- The handler stops doing obviously incorrect things:
  - leaking DuckDB resources
  - claiming condition pushdown when it is not actually applied
  - using mismatched local read/write Parquet paths
- Shared metadata/helper scaffolding exists so follow-on PRs can implement catalog/object-store logic cleanly.

## File-by-file checklist

### [storage/parquet/CMakeLists.txt](/Users/ryanzhou/Desktop/server/storage/parquet/CMakeLists.txt)
- [ ] Keep the target source list consistent with the files that actually exist.
- [ ] Add a real `ha_parquet_pushdown.cc` stub instead of referencing a missing file.
- [ ] Add `parquet_metadata.cc` if that helper is introduced in this PR.
- [ ] Do not add catalog/object-store sources yet unless they contain real code used by this PR.
- [ ] Keep DuckDB/CURL linkage unchanged unless cleanup is necessary for compilation.

### [storage/parquet/ha_parquet.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.h)
- [ ] Introduce small shared structs for future Stage 1 work, but keep them minimal:
  - `ParquetTableOptions`
  - `ParquetLocalPaths`
  - optional staged-file placeholder struct
- [ ] Add helper method declarations for:
  - resolving the local prototype Parquet path consistently
  - initializing / tearing down DuckDB state
  - resolving Stage 1 defaults in one place
- [ ] Remove handler state that should not be active yet for condition pushdown.
- [ ] Keep the header narrow; do not add full REST/object-store interfaces here.

### [storage/parquet/ha_parquet.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc)

#### Build and lifecycle cleanup
- [ ] Remove duplicate includes and any dead prototype code that is no longer used.
- [ ] Implement real cleanup in `close()`:
  - free `scan_result`
  - delete/destroy DuckDB connection and database objects
  - reset handler-owned state
- [ ] Make `open()` initialize all runtime state explicitly and idempotently.

#### Safe Stage 1 fallback behavior
- [ ] Change `cond_push()` to return the original condition instead of `nullptr`.
- [ ] Make `cond_pop()` a no-op.
- [ ] Stop pretending Stage 2 pushdown exists in Stage 1.

#### Path consistency
- [ ] Introduce one shared helper for local prototype Parquet path resolution.
- [ ] Make `create()` and `rnd_init()` use the same helper.
- [ ] Remove the current mismatch between `name + ".parquet"` and `table->s->table_name.str + ".parquet"`.

#### Write-path safety cleanup
- [ ] Fix the reversed `s3_path.empty()` condition in `external_lock()`.
- [ ] Keep mailbox writes only on successful flushes.
- [ ] Leave full commit/rollback integration for the next PR.

#### Scope control
- [ ] Do not implement real LakeKeeper commit logic in this file yet.
- [ ] Do not implement full object-store upload yet.
- [ ] Do not implement full typed ingestion rewrite yet unless needed for compilation or a small correctness fix.

### [storage/parquet/ha_parquet_pushdown.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.h)
- [ ] Create this header.
- [ ] Declare a minimal select-handler factory:
  - `create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)`
- [ ] If helpful, declare a tiny stub `ha_parquet_select_handler` class, but keep it non-functional for now.
- [ ] Add clear comments that full Stage 1 pushdown will be implemented in a follow-up PR.

### [storage/parquet/ha_parquet_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc)
- [ ] Create this file so the plugin builds cleanly.
- [ ] Implement `create_duckdb_select_handler(...)` as a safe stub.
- [ ] Return `nullptr` unconditionally, or gate it behind a hard-disabled path, so MariaDB falls back to normal execution.
- [ ] Do not attempt partial pushdown in this first PR.

### [storage/parquet/parquet_metadata.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_metadata.h)
- [ ] Create this header.
- [ ] Define minimal metadata/default-resolution types only:
  - block size
  - compression codec
  - Parquet version
  - local prototype path info
- [ ] Keep this file free of network or object-store concerns.

### [storage/parquet/parquet_metadata.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_metadata.cc)
- [ ] Create this file.
- [ ] Implement helper functions for:
  - resolving Stage 1 defaults
  - resolving consistent local prototype paths
- [ ] Keep the implementation local and pure so it is easy to unit test.

### [storage/parquet/parquet_test.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_test.cc)
- [ ] Keep the existing schema-mapping tests.
- [ ] Add focused tests for any new pure helpers from `parquet_metadata.cc`, especially:
  - default option resolution
  - local path resolution consistency
- [ ] Avoid adding tests that depend on live network/object-store/catalog behavior.

## Nice-to-have, only if still small
- [ ] Add one regression test for the `cond_push()` safe fallback behavior.
- [ ] Add one regression test for local path consistency between create/read code paths.

## Definition of done for the first PR
- [ ] The plugin no longer references missing source files.
- [ ] A stub `ha_parquet_pushdown.cc` exists and compiles.
- [ ] `close()` releases DuckDB-owned resources.
- [ ] `cond_push()` no longer risks wrong results by claiming unimplemented pushdown.
- [ ] The local prototype uses one consistent Parquet-path helper for create/read.
- [ ] The `external_lock()` mailbox condition is fixed.
- [ ] Any new helper logic added in this PR has small unit-test coverage.

## Follow-up PRs after this one
1. Handlerton `commit()` / `rollback()` plus THD mailbox redesign.
2. Real object-store flush/upload and rollback deletion.
3. Real LakeKeeper/Iceberg table creation and active-file lookup.
4. Real DuckDB SELECT handler implementation.
5. Full Stage 1 scan/write path rework and MTR coverage.
