# DuckDB Temporary Table Support Design

## Status

Proposed design for supporting MariaDB user temporary tables in the DuckDB storage engine.

The immediate motivation is MDEV-40379. A debug build currently aborts when executing:

```sql
CREATE TEMPORARY TABLE t (c INT KEY) ENGINE=DuckDB;
```

The failure is caused by an assertion in `CreateTableConvertor::translate()` that rejects `HA_LEX_CREATE_TMP_TABLE`. Release builds skip the assertion and accidentally create a persistent DuckDB table under a generated physical name. Removing the assertion or adding `TEMPORARY` to the generated SQL is not sufficient for complete support because MariaDB and DuckDB differ in naming, transaction, and lifecycle semantics.

## Goals

The implementation must provide the following MariaDB semantics:

- A temporary table is visible only to the MariaDB session that created it.
- Sessions may create temporary tables with identical logical names without conflicts.
- A temporary table may shadow a permanent table with the same logical name.
- Temporary tables in different MariaDB databases remain distinct.
- An explicit `DROP TEMPORARY TABLE` removes the DuckDB object immediately.
- Remaining temporary tables are removed when the MariaDB session ends.
- DML against transactional temporary tables participates in transaction commit and rollback.
- `CREATE`, `DROP`, and `ALTER` of temporary tables do not cause an implicit commit and are not undone by `ROLLBACK`.
- MariaDB internal `HA_CREATE_TMP_ALTER` tables remain distinct from user temporary tables.
- Debug and release builds have identical behavior.

## Non-goals for the first milestone

The first milestone does not need to provide:

- Parallel replication support for DuckDB temporary tables.
- Pushdown of statements containing DuckDB temporary tables.
- Direct UPDATE or DELETE execution for DuckDB temporary tables.
- Every `ALTER TABLE` variant.

These paths must fail safely or fall back to the regular handler implementation until they are explicitly supported.

## Existing behavior

MariaDB calls `ha_duckdb::create()` with a generated physical path for a user temporary table. The basename is normally similar to:

```text
#sql-temptable-95fc2-4-0
```

`DatabaseTableNames` extracts a schema-like directory component and the physical basename from this path. The current create converter then produces SQL equivalent to:

```sql
CREATE SCHEMA IF NOT EXISTS "tmp";
USE "tmp";
CREATE TABLE IF NOT EXISTS "#sql-temptable-95fc2-4-0" (...);
```

This object is persistent in DuckDB. It is not a valid implementation of MariaDB temporary-table semantics and may survive an abnormal server termination.

The assertion that exposes this unsupported path is intentional:

```cpp
assert((m_create_info->options & HA_LEX_CREATE_TMP_TABLE) == 0);
```

The current handlerton does not set `HTON_TEMPORARY_NOT_SUPPORTED`, so MariaDB allows the request to reach the assertion.

## Proposed physical representation

A MariaDB user temporary table will be represented by a native DuckDB temporary table in the per-THD DuckDB connection:

```text
MariaDB logical name: db1.t
MariaDB physical path: .../tmp/#sql-temptable-95fc2-4-0
DuckDB object:         temp.main."#sql-temptable-95fc2-4-0"
```

The MariaDB-generated physical basename must be retained. It already provides session-safe uniqueness and avoids collisions between:

- `db1.t` and `db2.t` in one session;
- identically named temporary tables in separate sessions;
- a temporary table and a permanent table with the same logical name.

The engine must not use the logical `db.table` name as the DuckDB temporary object name.

## Unified physical table reference

Table-name construction is currently duplicated across DDL, DML, scans, truncation, and appenders. Temporary-table support requires one authoritative resolver.

Introduce a small value type, provisionally named `DuckdbTableRef`, containing:

```text
catalog
schema
table
temporary
```

The type must support construction from:

- `TABLE *` for open-table operations;
- a handler path and `HA_CREATE_INFO` for CREATE;
- a handler path plus the per-THD temporary-table registry for DROP and RENAME.

Resolution rules are:

```text
User temporary table:
  catalog   = temp
  schema    = main
  table     = physical basename from the normalized handler path
  temporary = true

Regular table or HA_CREATE_TMP_ALTER table:
  catalog   = the normal DuckDB catalog
  schema    = DatabaseTableNames.db_name
  table     = DatabaseTableNames.table_name
  temporary = false
```

A user temporary table is detected during CREATE using:

```cpp
create_info->options & HA_LEX_CREATE_TMP_TABLE
```

For an open table, it is detected from the corresponding `TABLE_SHARE` temporary-table state. This must not be inferred from a `#sql-*` prefix because internal ALTER tables use similar generated names.

The resolver must also provide one identifier-quoting implementation. Callers must not assemble `"schema"."table"` independently.

## Per-THD temporary-table registry

`handler::delete_table(const char *name)` does not receive a `TABLE` or `HA_CREATE_INFO`. It therefore cannot reliably distinguish a user temporary table from an internal ALTER table by inspecting the path alone.

Extend `DuckdbThdContext` with a registry keyed by the normalized handler path. Each entry should contain at least:

```text
physical DuckdbTableRef
CREATE replay SQL or equivalent table definition
lifecycle state needed by the transaction journal
```

An entry is inserted only after DuckDB successfully creates the table. It is removed only after the corresponding DROP succeeds.

The registry also provides an unambiguous lookup for explicit DROP, MariaDB session cleanup, appender invalidation, and transaction replay.

## CREATE implementation

`CreateTableConvertor::translate()` must have separate regular and user-temporary paths.

For a user temporary table it should generate SQL equivalent to:

```sql
CREATE TEMPORARY TABLE IF NOT EXISTS
  temp.main."#sql-temptable-95fc2-4-0" (...);
```

The temporary path must not execute:

```sql
CREATE SCHEMA "tmp";
USE "tmp";
```

After successful execution, `ha_duckdb::create()` records the table in the per-THD registry.

`HA_CREATE_TMP_ALTER` alone must continue through the regular-table path. Only `HA_LEX_CREATE_TMP_TABLE` selects native DuckDB temporary-table behavior.

## DROP and connection cleanup

`ha_duckdb::delete_table()` must first resolve the handler path through the per-THD registry.

For a registered temporary table it executes:

```sql
DROP TABLE IF EXISTS temp.main."<physical-name>";
```

Before DROP, all appenders associated with the physical table must be flushed or discarded according to the operation's semantics. After successful DROP, the appender and registry entry are removed.

MariaDB invokes the storage engine's delete path for explicit `DROP TEMPORARY TABLE` and while closing a session. DuckDB also automatically removes native temporary tables when its connection is destroyed. Explicit DROP is still required so that MariaDB and DuckDB state remain synchronized during the lifetime of the session.

Destroying `DuckdbThdContext` remains the final cleanup safety net.

## Row DML and handler scans

The following operations must use `DuckdbTableRef` instead of constructing identifiers independently:

- `InsertConvertor`;
- `UpdateConvertor`;
- `DeleteConvertor`;
- `ha_duckdb::rnd_init()`;
- `ha_duckdb::delete_all_rows()`;
- `ha_duckdb::truncate()`;
- appender lookup and invalidation.

For a temporary table, generated SQL must reference:

```sql
temp.main."<physical-name>"
```

The existing `rnd_init()` path uses `table->s->db` and `table->s->table_name`, which are logical MariaDB names. That path must be converted before temporary tables can be read reliably.

The first implementation should verify non-batched row operations before enabling batched ingestion.

## Appender integration

`DeltaAppender` currently identifies a base table by a schema/table pair. It must instead receive a complete physical table reference.

DuckDB provides an Appender constructor that accepts catalog, schema, and table. For a user temporary base table, the Appender must target:

```text
catalog = temp
schema  = main
table   = MariaDB-generated physical basename
```

The mixed-DML batch path already creates a separate native DuckDB temporary staging table. Temporary-table support must distinguish:

- the user temporary base table in `temp.main`;
- the internal delta staging table in `temp.main`.

All generated `CREATE ... AS FROM`, `INSERT INTO`, `DELETE FROM`, merge, and cleanup statements must use the resolved base-table reference.

The `DeltaAppenders` map should be keyed by the full physical identity rather than only a schema/table pair.

## Transaction semantics

### Semantic difference

MariaDB temporary-table DDL does not cause an implicit commit, but the DDL action itself cannot be rolled back. For example:

```sql
BEGIN;
CREATE TEMPORARY TABLE t (c INT) ENGINE=DuckDB;
INSERT INTO t VALUES (1);
ROLLBACK;
```

After rollback:

- the table definition must still exist;
- the inserted row must not exist.

DuckDB transactional DDL behaves differently. Rolling back the DuckDB transaction removes a temporary table created by that transaction.

The implementation must not solve this by committing the active DuckDB transaction before temporary DDL. Doing so would incorrectly commit unrelated DML. A second DuckDB connection is also unsuitable because DuckDB temporary objects are connection-local and would not participate in queries executed by the primary connection.

### Temporary DDL journal

Add a per-THD temporary DDL journal to `DuckdbThdContext`.

The journal records successful user temporary DDL operations executed in the current MariaDB transaction. On successful commit, the journal is cleared. After DuckDB rollback, the journal is replayed to restore MariaDB's non-rollback DDL semantics.

#### CREATE

Record the table definition or replayable CREATE SQL.

After DuckDB rollback removes the object, replay CREATE. The table is recreated empty, while transactional rows remain rolled back.

#### DROP

Record the physical DROP operation.

DuckDB rollback may restore the dropped table and its data. Replay DROP after rollback so that the MariaDB DROP remains effective.

#### ALTER

Record replayable ALTER operations in execution order.

After DuckDB rollback restores the previous definition, replay the ALTER operations so that the structural changes remain effective while transactional DML remains rolled back.

#### Coalescing

The journal should coalesce simple operation sequences:

```text
CREATE -> DROP: remove both entries
CREATE -> ALTER: update the replay definition or preserve ordered replay
ALTER  -> DROP: retain only the final DROP where safe
```

Correctness is more important than aggressive coalescing. Ordered replay is acceptable for the initial implementation.

Replay must happen after `duckdb_trans_rollback()` completes and must not leave an unintended active transaction.

Statement rollback, full transaction rollback, savepoints, and errors during CREATE TABLE AS SELECT must be characterized against InnoDB before finalizing journal boundaries.

## SELECT pushdown

The select-handler pushdown path sends a transformed form of the original MariaDB SQL to DuckDB. That SQL contains logical table names such as `db.t`, while the DuckDB object uses a generated physical name in `temp.main`.

For the first milestone, SELECT pushdown must be disabled when any participating DuckDB table is a user temporary table. The regular handler scan will provide correct behavior after `rnd_init()` uses `DuckdbTableRef`.

A later implementation may restore pushdown using an AST-aware table-reference mapping from each opened `TABLE_LIST` entry to its physical DuckDB object.

A textual search-and-replace is not acceptable because it cannot safely handle aliases, quoted identifiers, subqueries, repeated names, or permanent/temporary shadowing.

## Direct UPDATE and DELETE

Direct UPDATE and DELETE currently execute the original MariaDB SQL in DuckDB. They have the same logical-to-physical naming problem as SELECT pushdown.

Until an AST-aware mapping exists, direct UPDATE and DELETE must decline temporary-table statements and allow MariaDB to use row-based handler DML. The exact fallback contract of the MariaDB direct-DML API must be verified before implementation.

## ALTER and RENAME

ALTER and RENAME support should follow the basic CRUD milestone.

All DDL converters must accept a physical table reference instead of independent schema and table strings. COPY ALTER must preserve the distinction between:

- the user temporary table;
- MariaDB's internal `HA_CREATE_TMP_ALTER` shadow table.

After COPY ALTER completes, the resulting user table must remain a native DuckDB temporary table in `temp.main`. Internal shadow objects must not be accidentally registered as user temporary tables or left persistent.

Cross-database RENAME behavior must be characterized separately because MariaDB logical databases do not map directly to schemas for DuckDB temporary objects.

## Replication

MariaDB adds `HA_LEX_CREATE_GLOBAL_TMP_TABLE` when a replica thread creates a temporary table. DuckDB temporary tables are connection-local, so an object created by one replica THD is not automatically visible from another DuckDB connection.

The first milestone should reject this path explicitly rather than provide partially correct behavior.

Full replication support requires a separate design, potentially involving a replica-specific shared context or guaranteed affinity between the MariaDB temporary table and one DuckDB connection. Parallel replication serialization in MariaDB does not by itself make a DuckDB connection-local catalog visible across THDs.

## Error handling and invariants

The implementation must maintain these invariants:

1. A MariaDB temporary-table share never points to a missing DuckDB object after a successful statement, commit, or rollback.
2. A DuckDB temporary object is never registered until CREATE succeeds.
3. A registry entry is not removed until DROP succeeds.
4. Appenders never outlive the physical object they target.
5. User temporary tables never create persistent DuckDB schemas or tables.
6. Internal ALTER tables never enter the user temporary-table registry.
7. Unsupported pushdown, direct DML, ALTER, and replication paths fail or fall back without aborting the server.
8. Debug assertions validate internal invariants but never reject valid user input that reached the handler API.

If replay after rollback fails, the engine must return an error and mark the context unusable rather than continue with divergent MariaDB and DuckDB catalogs.

## Implementation sequence

### Patch 1: Physical table references

- Add `DuckdbTableRef` and centralized identifier quoting.
- Resolve regular, user temporary, and `HA_CREATE_TMP_ALTER` objects correctly.
- Convert existing non-pushdown callers to use the resolver.
- Add focused unit or MTR coverage for physical-name selection.

### Patch 2: Basic CREATE, DROP, CRUD, and scans

- Generate native DuckDB `CREATE TEMPORARY TABLE`.
- Add the per-THD temporary-table registry.
- Implement explicit DROP and session cleanup.
- Support non-batched INSERT, SELECT, UPDATE, DELETE, and TRUNCATE.
- Disable pushdown and direct DML for user temporary tables.
- Make the MDEV-40379 regression test pass in debug and release builds.

### Patch 3: Appender support

- Pass full physical references to `DeltaAppender`.
- Support insert-only batching.
- Support mixed DML staging and merge.
- Invalidate appenders during DROP and TRUNCATE.

### Patch 4: Transaction DDL journal

- Replay CREATE after rollback.
- Replay DROP after rollback.
- Replay supported ALTER operations after rollback.
- Cover `autocommit=0`, explicit transactions, statement errors, and savepoint behavior.

### Patch 5: Pushdown and direct DML

- Add an AST-aware logical-to-physical table mapping.
- Re-enable SELECT pushdown for supported temporary-table queries.
- Re-enable direct UPDATE and DELETE after equivalent mapping is available.

### Patch 6: Extended DDL and replication

- Complete ALTER and COPY ALTER coverage.
- Add CREATE TABLE AS SELECT behavior.
- Define RENAME semantics.
- Design and implement replication-thread support.

## Test plan

### Basic regression

```sql
CREATE TEMPORARY TABLE t (c INT KEY) ENGINE=DuckDB;
DROP TEMPORARY TABLE t;
```

This must pass without assertion failures in debug builds.

### CRUD

Test INSERT, SELECT, UPDATE, DELETE, TRUNCATE, and DROP with batch mode both enabled and disabled.

### Session isolation

Two simultaneous connections create, populate, and read temporary tables with the same logical name. Each connection must see only its own data.

### Permanent-table shadowing

Create a permanent DuckDB table and a temporary DuckDB table with the same logical name. Unqualified statements must use the temporary table until it is dropped.

### Database namespaces

Create `db1.t` and `db2.t` as temporary DuckDB tables in one session. Both must coexist and retain independent data.

### Cleanup

Verify explicit DROP, normal disconnect, killed connection, and server shutdown. No persistent DuckDB object or schema may remain.

### Transactions

Cover at least:

```text
CREATE -> ROLLBACK
CREATE -> INSERT -> ROLLBACK
CREATE -> INSERT -> COMMIT
existing table -> INSERT -> ROLLBACK
DROP -> ROLLBACK
ALTER -> ROLLBACK
autocommit = 0
```

The table definition and DDL effects must follow MariaDB semantics while DML follows transaction semantics.

### CREATE TABLE AS SELECT

Test successful CTAS, source-query failure, conversion failure, statement rollback, and later transaction rollback.

### ALTER

Test supported inplace operations, COPY ALTER, failure cleanup, and distinction from `HA_CREATE_TMP_ALTER` objects.

### Pushdown and direct DML

Before physical-name rewriting is implemented, verify that these paths fall back safely. After implementation, test aliases, quoted identifiers, joins, subqueries, and permanent/temporary shadowing.

### Replication

Until supported, verify a stable documented error instead of a crash or silently incorrect execution.

## Risks

The highest-risk area is transaction synchronization, not DuckDB object creation. MariaDB metadata and DuckDB catalog state can diverge if rollback replay is incomplete or fails.

Other significant risks are:

- confusing user temporary tables with internal ALTER tables;
- stale appenders targeting dropped or recreated objects;
- raw SQL pushdown using logical names;
- CTAS statement errors crossing transaction boundaries;
- replication THDs using different DuckDB connections.

The implementation should therefore be delivered in small patches with fallback paths disabled until each capability has dedicated coverage.

## Recommended first milestone

The first usable milestone should include:

- native `temp.main` storage;
- physical-name resolution;
- per-THD registry;
- CREATE and DROP;
- non-batched CRUD and handler scans;
- session isolation and cleanup;
- the temporary DDL journal for CREATE and DROP rollback semantics;
- safe fallback from pushdown and direct DML;
- explicit rejection of unsupported replication use.

This milestone solves MDEV-40379 without claiming support for paths whose semantics are not yet correct.
