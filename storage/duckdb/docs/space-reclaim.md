# DuckDB Space Reclaim

When tables are dropped or large amounts of data are deleted, the DuckDB database
file does not automatically shrink. Deleted blocks are marked as free but remain
in the file until explicitly reclaimed.

## Methods

### 1. CHECKPOINT

Forces a write-ahead log (WAL) flush to the main database file and marks deleted
blocks as reusable.

```sql
SELECT run_in_duckdb('CHECKPOINT');
-- or force even when WAL is empty:
SELECT run_in_duckdb('FORCE CHECKPOINT');
```

**Effect**: Freed blocks become available for new data. The file size does **not**
decrease — space is reused internally.

**When to use**: After bulk deletes or DROP TABLE, before inserting new data into
other tables. Low cost, safe to run frequently.

### 2. VACUUM

Compacts the database file by relocating live data and truncating unused blocks
at the end of the file.

```sql
SELECT run_in_duckdb('VACUUM');
```

**Effect**: The `.duckdb` file **shrinks on disk**. This is a heavier operation
that rewrites portions of the file.

**When to use**: After dropping large tables or deleting significant portions of
data when actual disk space recovery is needed.

**Requirements** (DuckDB v1.1+):
- No active transactions on the database.
- Sufficient temporary disk space for block relocation.

### 3. Export / Reimport (nuclear option)

For maximum compaction or to reset fragmentation entirely:

```sql
SELECT run_in_duckdb('EXPORT DATABASE ''/tmp/duckdb_export''');
```

Then stop MariaDB, delete the old `.duckdb` file, restart, and import:

```sql
SELECT run_in_duckdb('IMPORT DATABASE ''/tmp/duckdb_export''');
```

**Effect**: Produces a perfectly compact file with zero fragmentation.

**When to use**: Rare maintenance scenarios where VACUUM is insufficient or the
database has accumulated significant fragmentation over time.

## Configuration

The `duckdb_checkpoint_threshold` variable (default 256 MB) controls how large
the WAL can grow before an automatic checkpoint is triggered:

```
loose-duckdb-checkpoint-threshold=268435456
```

A smaller threshold means more frequent automatic checkpoints but less WAL
accumulation.

## Behavior After DROP TABLE

When `ha_duckdb::delete_table()` executes `DROP TABLE` in DuckDB:

1. The table metadata and data blocks are marked as deleted.
2. If the WAL exceeds `checkpoint_threshold`, an automatic checkpoint occurs.
3. Otherwise, blocks remain in the WAL until the next checkpoint.
4. To immediately reclaim disk space, run `VACUUM` after the drop.

## Summary

| Method             | File shrinks | Cost    | Availability |
|--------------------|--------------|---------|--------------|
| CHECKPOINT         | No           | Low     | Always       |
| VACUUM             | Yes          | Medium  | DuckDB 1.1+  |
| Export + Reimport  | Yes          | High    | Always       |
