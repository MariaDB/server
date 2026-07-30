# DuckDB Space Reclaim

When tables are dropped or large amounts of data are deleted, the DuckDB database
file does not automatically shrink. Deleted blocks are marked as free and reused
for new writes, but the file size on disk remains unchanged unless specific
conditions are met.

## How DuckDB storage works

DuckDB uses a single-file storage format with 256 KB blocks. A `free_list`
tracks which blocks are available for reuse. Key internals
(`single_file_block_manager.cpp`):

- **`free_list`** — set of block IDs that are free for reuse.
- **`max_block`** — highest allocated block ID (determines logical file size).
- **`Truncate()`** — called during FULL_CHECKPOINT; scans `free_list` from the
  **end** and shrinks the file only while the **tail** blocks are contiguous and
  free. Stops at the first occupied block.
- **`TrimFreeBlocks()`** — calls `fallocate(FALLOC_FL_PUNCH_HOLE)` on freed
  blocks, returning physical space to the filesystem without changing logical
  file size. Controlled by `DBConfig::options.trim_free_blocks` (not exposed as
  a SQL setting in v1.5).

## What VACUUM actually does

`VACUUM` in DuckDB v1.5 does **NOT** shrink the file. The `PhysicalVacuum`
operator (`physical_vacuum.cpp`) only:

1. Scans table data to compute `DistinctStatistics` per column.
2. Calls `VacuumIndexes()` on the table storage.

It does **not** trigger a checkpoint, does not relocate blocks, and does not
call `Truncate()`. The SQL `VACUUM` command is essentially a statistics refresh.

## What CHECKPOINT does

`CHECKPOINT` / `FORCE CHECKPOINT` writes WAL contents to the main file and
calls `Truncate()`. However, file shrinking depends on:

1. **Checkpoint type** — only `FULL_CHECKPOINT` attempts truncation.
   `CONCURRENT_CHECKPOINT` (used when other transactions are active) skips it.
2. **Block layout** — `Truncate()` removes free blocks from the tail only:
   ```cpp
   for (auto entry = free_list.rbegin(); ...) {
       if (block_id + 1 != max_block) break;  // stop at first gap
       max_block--;
   }
   ```
   If even one metadata or catalog block sits near the end of the file, nothing
   is truncated.
3. **Vacuum lock** — `FULL_CHECKPOINT` requires an exclusive vacuum lock. If any
   transaction holds a shared vacuum lock (any INSERT/DELETE in progress), the
   checkpoint degrades to `CONCURRENT_CHECKPOINT`.
4. **Active transactions** — if `GetLastCommit() > LowestActiveStart()`, the
   checkpoint is forced to `CONCURRENT_CHECKPOINT` regardless.

### Conditions for successful file truncation

All must be true simultaneously:
- No other DuckDB connections (each MariaDB THD holds a persistent connection).
- No active transactions (`FORCE CHECKPOINT` waits for them).
- The free blocks at the end of the file form a contiguous range from `max_block`
  downward.

In practice, after a large table is dropped, metadata blocks (catalog, schemas)
often remain at high block IDs, preventing any truncation.

## Practical methods

### 1. FORCE CHECKPOINT (reuse space, may shrink)

```sql
SELECT run_in_duckdb('FORCE CHECKPOINT');
```

**Effect**: WAL flushed, free blocks marked for reuse. File **may** shrink if
tail blocks are all free. In most cases, the file size stays the same but new
inserts will reuse freed blocks without growing the file.

**Important**: Close all other MariaDB connections first (each holds a DuckDB
connection via `DuckdbThdContext`).

### 2. Export / Reimport (guaranteed shrink)

The only reliable way to reduce file size:

```sql
SELECT run_in_duckdb('EXPORT DATABASE ''/tmp/duckdb_export'' (FORMAT PARQUET)');
```

Then:
```bash
mysqladmin shutdown
rm /var/lib/mysql/duckdb.db
# restart MariaDB
```

```sql
SELECT run_in_duckdb('IMPORT DATABASE ''/tmp/duckdb_export''');
```

**Effect**: New file contains only live data, perfectly compacted.

### 3. Delete the file (when no data is needed)

If the database is empty or data can be reloaded:

```bash
mysqladmin shutdown
rm /var/lib/mysql/duckdb.db
# restart MariaDB — a fresh empty file is created
```

## Diagnostics

```sql
SELECT run_in_duckdb('SELECT * FROM pragma_database_size()');
```

Returns `total_blocks`, `used_blocks`, `free_blocks`, `wal_size`. If
`free_blocks` is high but the file hasn't shrunk, it means `Truncate()` could
not find a contiguous free tail.

Check actual disk usage vs logical size:
```bash
ls -lh /var/lib/mysql/duckdb.db   # logical size
du -sh /var/lib/mysql/duckdb.db   # actual disk usage (differs if hole-punched)
```

## Configuration

The `duckdb_checkpoint_threshold` variable (default 256 MB) controls how large
the WAL can grow before an automatic checkpoint is triggered:

```
loose-duckdb-checkpoint-threshold=268435456
```

A smaller threshold means more frequent automatic checkpoints but less WAL
accumulation.

## Summary

| Method             | File shrinks | Reuses space | Cost   | Reliability |
|--------------------|--------------|--------------|--------|-------------|
| FORCE CHECKPOINT   | Rarely       | Yes          | Low    | Always works for reuse |
| VACUUM             | No           | No           | Low    | Only updates stats |
| Export + Reimport  | Yes          | N/A          | High   | Always shrinks |
| Delete file        | Yes          | N/A          | None   | Loses all data |
