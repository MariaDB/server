# Parquet Engine Changes

This file summarizes the main changes made during the Parquet + LakeKeeper debugging and implementation work in this checkout.

## 1. LakeKeeper integration fixes

The Parquet engine now uses a consistent LakeKeeper warehouse path for:

- table creation
- table load during reads
- transaction commit

Earlier revisions were mixing warehouse IDs, which caused successful MariaDB statements to map to failed LakeKeeper operations. That led to empty reads because no valid snapshot was actually visible in the catalog.

## 2. Table creation and delete behavior

`ha_parquet::create()` now:

- creates the DuckDB buffer table
- writes the initial local Parquet artifact
- registers the table in LakeKeeper under namespace `default`

`ha_parquet::delete_table()` was added so `DROP TABLE` can remove the corresponding LakeKeeper table entry instead of leaving stale remote catalog state behind.

## 3. Snapshot flush path

`flush_remaining_rows_to_s3()` was updated so the write path can:

- discover the current snapshot data file from LakeKeeper
- write a new S3 Parquet object
- append buffered rows on top of the current snapshot contents when needed
- record file size and row-count information for commit

This moved the engine away from the earlier behavior where only the latest staged batch was visible.

## 4. Read path metadata handling

The read path now:

- fetches table metadata from LakeKeeper
- extracts `manifest-list` / S3 data-file paths from the response
- builds a DuckDB `read_parquet(...)` query from those paths

This made reads depend on real catalog state instead of a local-only assumption.

## 5. Row decode fix in `write_row()`

One early bug was that inserted rows were reaching Parquet as `NULL` values.

That was fixed by making `write_row()` read from the actual MariaDB record buffer passed in `buf`, rather than indirectly reading from the wrong state. After that change:

- MariaDB field decoding was correct
- DuckDB buffer inserts were correct
- freshly written Parquet files contained the expected values

## 6. Null-bit fix in `rnd_next()`

The final read-side correctness bug was in `rnd_next()`.

DuckDB was returning the correct values, and MariaDB field bytes were being stored correctly, but the field null bit was still set. That made rows display as `NULL` even though the data was present.

The minimal fix was:

- explicitly call `f->set_notnull()` on the non-null branch before storing the value

After that fix, a query like:

```sql
SELECT * FROM debug_t2;
```

returned:

```text
+------+-------+
| id   | name  |
+------+-------+
|    1 | alpha |
|    2 | beta  |
+------+-------+
```

## 7. Debugging instrumentation and cleanup

Temporary debug logging was added during diagnosis to inspect:

- MariaDB record-buffer decoding in `write_row()`
- DuckDB buffer contents before flush
- Parquet file contents immediately after flush
- LakeKeeper metadata responses in `rnd_init()`
- per-field value transfer in `rnd_next()`

That temporary instrumentation has now been removed. The permanent functional fix that remains from that work is the `f->set_notnull()` change in `rnd_next()`.

## 8. Runtime configuration moved to `.env`

The remaining LakeKeeper and S3 runtime values are now loaded from:

`/Users/ryanzhou/Desktop/server/.env`

The engine reads:

- `PARQUET_LAKEKEEPER_BASE_URL`
- `PARQUET_LAKEKEEPER_WAREHOUSE_ID`
- `PARQUET_LAKEKEEPER_NAMESPACE`
- `PARQUET_S3_BUCKET`
- `PARQUET_S3_DATA_PREFIX`
- `PARQUET_S3_REGION`
- `PARQUET_S3_ACCESS_KEY_ID`
- `PARQUET_S3_SECRET_ACCESS_KEY`

An optional `PARQUET_ENV_FILE` process environment variable can point the server at a different `.env` file.

This removes the need to edit `ha_parquet.cc` every time the local LakeKeeper or S3 settings change.

## 9. Operational lesson: use the build binary

One major source of confusion during debugging was that the server was sometimes started with:

```text
/Users/ryanzhou/Desktop/server/sql/mariadbd
```

instead of the rebuilt binary:

```text
/Users/ryanzhou/Desktop/server/build/sql/mariadbd
```

That made it look like fixes were not taking effect when the wrong executable was actually running.

For this checkout, the correct startup command is:

```bash
/Users/ryanzhou/Desktop/server/build/sql/mariadbd \
  --defaults-file=/Users/ryanzhou/Desktop/server/build/mariadb-parquet.cnf
```

## 10. Current status

What is working:

- `CREATE TABLE ... ENGINE=PARQUET`
- `INSERT`
- `SELECT`
- LakeKeeper-backed metadata lookup
- S3-backed Parquet read/write for the tested append flow

What is still not implemented or still limited:

- `UPDATE`
- `DELETE`
- broader transaction semantics
- more complete Iceberg manifest/planning behavior
