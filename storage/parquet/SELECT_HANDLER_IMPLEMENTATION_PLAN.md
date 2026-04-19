# Parquet SELECT Handler Implementation Plan

## Goal

Implement a real Parquet `select_handler` so MariaDB can push eligible `SELECT` queries into DuckDB whenever at least one referenced table uses `ENGINE=PARQUET`.

This plan is based on:

- [ha_parquet_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc)
- [ha_parquet_pushdown.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.h)
- [select_handler.h](/Users/ryanzhou/Desktop/server/sql/select_handler.h)
- [select_handler.cc](/Users/ryanzhou/Desktop/server/sql/select_handler.cc)
- [ha_duckdb_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/duckdb/duckdb/ha_duckdb_pushdown.cc)
- [cross_engine_scan.h](/Users/ryanzhou/Desktop/server/storage/duckdb/duckdb/cross_engine_scan.h)

## Requirement adjustments

Some of the original requirements need to be tightened so the implementation is correct and realistic.

1. `prepare()` should usually use the base `select_handler::prepare()`.

The design says `prepare()` should create the structures and buffers to hold the result. That is already what the base class does in [select_handler.cc](/Users/ryanzhou/Desktop/server/sql/select_handler.cc). The Parquet handler should not start with a custom `prepare()` unless it has a concrete reason to override it. Phase 1 should reuse the base implementation.

2. `init_scan()` cannot only register non-Parquet tables.

If the handler executes the original SQL text in DuckDB, DuckDB must also be able to resolve Parquet table names. Registering only non-Parquet tables is not enough. The handler must make **every referenced Parquet table visible inside DuckDB** before executing the query.

The cleanest approach is:

- register non-Parquet tables in an external-table registry for replacement scans
- create a DuckDB temporary view for each Parquet table name using `read_parquet([...])`

3. `create_duckdb_select_handler()` should be conservative.

It should not push down every query that mentions a Parquet table. It should initially accept only cases we can execute correctly:

- `SQLCOM_SELECT`
- optionally `SQLCOM_INSERT_SELECT` after the pure `SELECT` path is working
- base tables only, no derived tables in the first slice
- no statement-prepare path
- no queries with side effects

4. Cross-engine joins should be Phase 1.5, not the very first line of code.

The first buildable slice should support:

- pure Parquet `SELECT`
- joins between Parquet tables

Then extend to:

- Parquet + InnoDB joins through the external-table registry

That sequencing makes debugging much easier.

5. UNION / EXCEPT / INTERSECT support should be planned explicitly.

The current requirement list only mentions `create_duckdb_select_handler()`, but MariaDB’s pushdown framework also has the concept of unit handlers. If we want set operations pushed down cleanly, we should add a Parquet `create_unit` path in a follow-up slice. It does not need to block the first `SELECT` handler PR, but it should be called out now.

## Recommended architecture

## Files

- [storage/parquet/ha_parquet_pushdown.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.h)
  - declare the Parquet select-handler class
  - declare the factory
  - optionally declare a future unit factory
- [storage/parquet/ha_parquet_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc)
  - factory logic
  - `ha_parquet_select_handler` implementation
- new [storage/parquet/parquet_cross_engine_scan.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_cross_engine_scan.h)
  - thread-local external-table registry API
  - replacement-scan declarations
- new [storage/parquet/parquet_cross_engine_scan.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_cross_engine_scan.cc)
  - registry implementation
  - DuckDB replacement-scan callback
- optionally new [storage/parquet/parquet_duckdb_value.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_duckdb_value.h)
  - helper to copy `duckdb::Value` into a MariaDB `Field`
- optionally new [storage/parquet/parquet_duckdb_value.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_duckdb_value.cc)
  - extracted field-conversion logic shared by `rnd_next()` and the select handler
- [storage/parquet/ha_parquet.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc)
  - wire `parquet_hton->create_select`
  - optionally wire a future `create_unit`
- [storage/parquet/CMakeLists.txt](/Users/ryanzhou/Desktop/server/storage/parquet/CMakeLists.txt)
  - add any new source files to the plugin build

## Runtime model

For each pushed-down query:

1. MariaDB chooses the Parquet `select_handler`.
2. The handler creates a fresh DuckDB connection for the query.
3. Each referenced Parquet table is exposed to DuckDB as a temporary view.
4. Each referenced non-Parquet table is registered in a thread-local external-table registry.
5. The handler executes the original SQL text inside DuckDB.
6. Result rows are streamed back into `table->record[0]`.
7. Query-local views, registries, and DuckDB result state are cleaned up.

## Implementation phases

## Phase 0: Wire the handlerton

### Changes

- In [ha_parquet.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc), set:
  - `parquet_hton->create_select = create_duckdb_select_handler;`

### Notes

- Do not wire `create_unit` yet unless the handler already supports whole-unit execution.
- Keep the current row-level handler path untouched while the select handler is still incomplete.

## Phase 1: Add a buildable Parquet-only select handler

### Goal

Push down a single-table Parquet `SELECT` into DuckDB.

### Class design

Define `ha_parquet_select_handler : public select_handler` with fields similar to DuckDB’s implementation:

- `std::unique_ptr<duckdb::QueryResult> query_result`
- `std::unique_ptr<duckdb::DataChunk> current_chunk`
- `size_t current_row_index`
- `StringBuffer<4096> query_string`
- `bool has_cross_engine`
- `std::vector<std::string> external_table_names`
- query-local metadata for referenced Parquet tables if needed

### Constructors

Mirror the DuckDB plugin shape:

- constructor for `SELECT_LEX`
- optionally constructor for partial-unit pushdown later

For the first slice, a single constructor for plain `SELECT_LEX` is enough.

### Factory

Implement `create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *sel_unit)` to:

- return `nullptr` unless the command is `SQLCOM_SELECT`
- return `nullptr` if `sel_lex == nullptr`
- return `nullptr` if there are no Parquet tables
- return `nullptr` for derived tables in the initial slice
- return `nullptr` for statement-prepare mode
- return `nullptr` for `UNCACHEABLE_SIDEEFFECT`
- otherwise allocate `ha_parquet_select_handler`

### Table discovery

Add a helper like:

`can_pushdown_to_parquet(SELECT_LEX *sel_lex, std::vector<ParquetTableRef> &, std::vector<ExternalTableRef> &, bool &has_parquet_table)`

It should walk `TABLE_LIST` and partition tables into:

- Parquet tables
- non-Parquet external tables

### `prepare()`

For the first slice, do not override it. Let `select_handler::prepare()` allocate the temp result table and fill `result_columns`.

### `init_scan()`

For the first slice:

1. create a new DuckDB connection using the same engine-wide DuckDB instance or a dedicated per-query DB strategy
2. `INSTALL/LOAD parquet`
3. `INSTALL/LOAD httpfs`
4. apply S3 config from the Parquet runtime config
5. create a temporary DuckDB view for each referenced Parquet table:
   - view name should match the MariaDB table name used in the SQL
   - view body should be `SELECT * FROM read_parquet([...])`
   - file list should come from the same LakeKeeper metadata path the row-level read path uses
6. execute the original SQL text from `thd->query()`

### `next_row()`

Implement chunked fetching like [ha_duckdb_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/duckdb/duckdb/ha_duckdb_pushdown.cc):

- fetch a new chunk when the current chunk is exhausted
- return `HA_ERR_END_OF_FILE` when no rows remain
- copy each DuckDB column into `table->field[i]`

Do not duplicate the conversion logic already being debugged in the row handler. Extract the `duckdb::Value` -> `Field` code into a shared helper and use it from both:

- `ha_parquet::rnd_next()`
- `ha_parquet_select_handler::next_row()`

That avoids reintroducing the null-bit bug we just fixed.

### `end_scan()`

Clean up:

- current chunk
- query result
- current row index
- query-local DuckDB objects if owned by the handler
- temp table via `free_tmp_table(thd, table)` if following the DuckDB pattern

### Exit criteria

These queries should work through the Parquet select handler:

```sql
SELECT * FROM parquet_table;
SELECT * FROM parquet_table WHERE id >= 10;
SELECT col1, COUNT(*) FROM parquet_table GROUP BY col1;
```

## Phase 2: Parquet-to-Parquet joins

### Goal

Support joins where all referenced tables use `ENGINE=PARQUET`.

### Changes

- extend table discovery to collect all Parquet tables from `TABLE_LIST`
- in `init_scan()`, create one DuckDB temp view per Parquet table
- execute the original SQL unchanged

### Why this phase matters

This proves the query-level path independently of cross-engine registration and replacement scans.

### Exit criteria

```sql
SELECT a.id, b.name
FROM parquet_a AS a
JOIN parquet_b AS b ON a.id = b.id;
```

## Phase 3: Cross-engine registry and replacement scan

### Goal

Support joins that mix Parquet tables with InnoDB or other MariaDB engines.

### New helper module

Create `parquet_cross_engine_scan.h/.cc` modeled on [cross_engine_scan.h](/Users/ryanzhou/Desktop/server/storage/duckdb/duckdb/cross_engine_scan.h).

### API

Add:

- `register_external_table(const std::string &name, TABLE *table)`
- `clear_external_tables()`
- `TABLE *find_external_table(const std::string &name)`
- a DuckDB replacement-scan callback
- one registration function to install the replacement scan into the DuckDB instance

### Important requirement change

Do not register only non-Parquet tables and hope DuckDB will resolve the Parquet ones automatically. The plan must stay:

- Parquet tables => explicit DuckDB temp views
- non-Parquet tables => external-table registry + replacement scans

### `init_scan()` additions

When cross-engine tables are present:

- register each non-Parquet table in the thread-local registry
- ensure the replacement-scan callback is installed once
- then execute the original SQL text

### `end_scan()` additions

Always clear the external-table registry for the current query, even on errors.

### Exit criteria

```sql
SELECT p.order_id, c.customer_name
FROM parquet_orders AS p
JOIN innodb_customers AS c
  ON p.customer_id = c.customer_id;
```

## Phase 4: Pushdown gating and correctness rules

### Goal

Make the factory safe instead of optimistic.

### Rules for initial acceptance

Push down only if all of these are true:

- query is `SQLCOM_SELECT`
- at least one table is Parquet
- no derived tables
- no statement-prepare mode
- no side-effect expressions
- no unsupported temporary-table semantics

### Rules for initial fallback

Return `nullptr` for:

- `CREATE VIEW`
- unsupported unit queries
- `SELECT` with constructs the replacement-scan path cannot yet satisfy
- anything that depends on MariaDB-specific evaluation not mirrored in DuckDB

### Why this matters

Wrong pushdown is worse than no pushdown. The Parquet handler should fail closed and let MariaDB execute the query normally.

## Phase 5: Unit pushdown

### Goal

Support `UNION` / `EXCEPT` / `INTERSECT`.

### Recommendation

Add a separate Parquet unit factory later, mirroring DuckDB’s `create_duckdb_unit_handler(...)`.

This is worth planning now, but it should not block the single-`SELECT` pushdown slice.

## File-by-file checklist

## [storage/parquet/ha_parquet.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet.cc)

- wire `parquet_hton->create_select`
- optionally factor `duckdb::Value` to `Field` conversion into a shared helper
- optionally expose one helper that resolves active Parquet files for a named table

## [storage/parquet/ha_parquet_pushdown.h](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.h)

- include `select_handler.h`
- include `sql_class.h`
- declare `ha_parquet_select_handler`
- declare the factory
- optionally declare a future unit factory

## [storage/parquet/ha_parquet_pushdown.cc](/Users/ryanzhou/Desktop/server/storage/parquet/ha_parquet_pushdown.cc)

- implement table-discovery helpers
- implement the factory
- implement constructors
- implement `init_scan()`
- implement `next_row()`
- implement `end_scan()`

## new [storage/parquet/parquet_cross_engine_scan.h](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_cross_engine_scan.h)

- declare the thread-local registry and replacement-scan entry points

## new [storage/parquet/parquet_cross_engine_scan.cc](/Users/ryanzhou/Desktop/server/storage/parquet/parquet_cross_engine_scan.cc)

- implement registry storage
- implement replacement-scan lookup
- register `_mdb_scan` or equivalent table function integration

## [storage/parquet/CMakeLists.txt](/Users/ryanzhou/Desktop/server/storage/parquet/CMakeLists.txt)

- add any new `.cc` files
- ensure the pushdown code has the right include paths for MariaDB and DuckDB APIs

## Testing plan

## MTR coverage

Add a Parquet suite with these cases:

1. single Parquet table pushdown

```sql
SELECT * FROM parquet_t ORDER BY id;
```

2. Parquet filter pushdown

```sql
SELECT * FROM parquet_t WHERE id >= 2 ORDER BY id;
```

3. Parquet-to-Parquet join

```sql
SELECT a.id, b.name
FROM parquet_a a
JOIN parquet_b b ON a.id = b.id
ORDER BY a.id;
```

4. Parquet-to-InnoDB join

```sql
SELECT p.id, i.name
FROM parquet_p p
JOIN innodb_i i ON p.id = i.id
ORDER BY p.id;
```

5. fallback case

- use a query shape the factory should reject
- verify it still executes successfully through MariaDB’s normal path

6. explain/debug coverage

- if possible, verify from logs or explain output that the select handler was chosen

## Manual validation

Check both:

- result correctness
- cleanup correctness

After each query, ensure:

- no stale external-table registry remains
- temp views do not leak between queries
- repeated queries do not accumulate state in DuckDB

## Recommended PR slicing

1. Wire `create_select` plus a Parquet-only handler for single-table `SELECT`
2. Add Parquet-to-Parquet joins
3. Add external-table registry and Parquet-to-InnoDB joins
4. Add unit pushdown and broaden query acceptance

## Final recommendation

The most important practical change to the original requirement set is this:

**Do not implement cross-engine join pushdown by only registering non-Parquet tables.**

That would leave DuckDB unable to resolve the Parquet tables named in the original SQL. A correct SELECT-handler design for this engine must do both:

- make Parquet tables visible inside DuckDB
- make non-Parquet tables visible through replacement scans

That adjustment is the difference between a plausible plan and a working one.
