# TPC-H kit — DuckDB storage engine (MariaDB)

Reproducible TPC-H pipeline for the DuckDB storage engine: install a generator,
generate Parquet data, create the `ENGINE=DUCKDB` tables, load them with
`read_parquet()`, and run the 22 queries directly through the `mariadb` client.

## Pipeline

| Step | Script | What it does |
|---|---|---|
| 1 | `01_install.sh` | Install `tpchgen-cli` (pip / uv / cargo). |
| 2 | `02_generate.sh` | Generate Parquet data at scale factor `$SF` into `$DATA_DIR`. |
| 3 | `03_schema.sh` | Create database `$SCHEMA` + 8 `ENGINE=DUCKDB` tables. |
| 4 | `04_load.sh` | `INSERT ... SELECT * FROM read_parquet()` each `.parquet` into `$SCHEMA.*` (via `run_in_duckdb`); prints per-table timings + row counts. |
| 5 | `05_run_queries.sh` | Run the 22 queries from `$TPCH_SQL`; writes `query_timings.tsv`. |

Run everything: `./run_all.sh` (steps are idempotent; generation is skipped if data exists).

## Configuration

All knobs live in `config.sh` and are overridable via environment:

```bash
SF=1 ./run_all.sh                      # scale factor 1
DATA_DIR=/data/tpch SF=10 ./run_all.sh # custom data location
SCHEMA=tpch_bench ./03_schema.sh       # custom MariaDB database
```

| Var | Default | Meaning |
|---|---|---|
| `SF` | `10` | TPC-H scale factor |
| `DATA_DIR` | `/git/tpch/sf<SF>` | where `.parquet` files are generated/read |
| `SCHEMA` | `bench` | MariaDB database holding the `ENGINE=DUCKDB` tables |
| `TPCH_SQL` | `/tpch.sql` | source of the 22 (MariaDB-dialect) queries |
| `MARIADB` | `mariadb` | client command |

## Prerequisites

- A running MariaDB server with the DuckDB storage engine loaded
  (`ENGINE=DUCKDB` available) and the `run_in_duckdb` function installed
  (used only for the Parquet load).
- `pip`, `uv`, or `cargo` to install the generator; `tpchgen-cli` on `PATH`
  afterwards (pip user installs land in `~/.local/bin`).

## How it works / caveats

- **Generator:** `tpchgen-cli -s <SF> --format=parquet --output-dir <DATA_DIR>`
  emits one `.parquet` file per table.
- **Load target:** data goes into `ENGINE=DUCKDB` tables in a regular MariaDB
  database (`bench`). The load runs server-side on the embedded DuckDB via
  `run_in_duckdb`: `INSERT INTO <db>.<table> SELECT * FROM read_parquet(...)`.
  ENGINE=DUCKDB tables are addressable inside DuckDB as `<db>.<table>`, so the
  Parquet data never round-trips through the MariaDB client. The TPC-H Parquet
  column order/types match the table definitions, so a plain `SELECT *` works.
- **Queries:** taken from `$TPCH_SQL` (MariaDB dialect) and executed directly
  through the `mariadb` client with `$SCHEMA` as the default database. Any
  query that errors out is reported as `ERR` in the timings.
