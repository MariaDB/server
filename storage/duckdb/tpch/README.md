# TPC-H kit — DuckDB storage engine (MariaDB)

Reproducible TPC-H pipeline for the embedded DuckDB engine: install a generator,
generate data, create the schema, COPY-load it, and run the 22 queries — all
through MariaDB's `run_in_duckdb`.

## Pipeline

| Step | Script | What it does |
|---|---|---|
| 1 | `01_install.sh` | Install `tpchgen-cli` (pip / uv / cargo). |
| 2 | `02_generate.sh` | Generate `.tbl` data at scale factor `$SF` into `$DATA_DIR`. |
| 3 | `03_schema.sh` | Create schema `$SCHEMA` + 8 tables in the embedded DuckDB. |
| 4 | `04_load.sh` | `COPY` each `.tbl` into `$SCHEMA.*`; prints per-table timings + row counts. |
| 5 | `05_run_queries.sh` | Run the 22 queries from `$TPCH_SQL`; writes `query_timings.tsv`. |

Run everything: `./run_all.sh` (steps are idempotent; generation is skipped if data exists).

## Configuration

All knobs live in `config.sh` and are overridable via environment:

```bash
SF=1 ./run_all.sh                      # scale factor 1
DATA_DIR=/data/tpch SF=10 ./run_all.sh # custom data location
SCHEMA=tpch_bench ./03_schema.sh       # custom DuckDB schema
```

| Var | Default | Meaning |
|---|---|---|
| `SF` | `10` | TPC-H scale factor |
| `DATA_DIR` | `/git/tpch/sf<SF>` | where `.tbl` files are generated/read |
| `SCHEMA` | `bench` | DuckDB schema populated via the UDF |
| `TPCH_SQL` | `/tpch.sql` | source of the 22 (MariaDB-dialect) queries |
| `MARIADB` | `mariadb` | client command |

## Prerequisites

- A running MariaDB server with the DuckDB engine loaded and the
  `run_in_duckdb` function available.
- `pip`, `uv`, or `cargo` to install the generator; `tpchgen-cli` on `PATH`
  afterwards (pip user installs land in `~/.local/bin`).

## How it works / caveats

- **Generator:** `tpchgen-cli -s <SF> --output-dir <DATA_DIR>` emits classic
  `.tbl` files (pipe-delimited, no header, trailing `|`). DuckDB loads these with
  `COPY ... (DELIMITER '|')` — the trailing delimiter is tolerated.
- **Load target:** data goes into a DuckDB-native schema (`bench`) inside the
  embedded instance via `run_in_duckdb`, not into `ENGINE=DUCKDB` MariaDB
  tables (which can't be `COPY`-loaded).
- **Queries:** taken from `/tpch.sql` (MariaDB dialect) and executed as
  `SET schema '<SCHEMA>'; <query>` through the UDF. The UDF receives the **raw**
  text — the engine's dialect rewrites only apply on pushdown, so any
  MariaDB-only syntax errors out and is reported as `ERR` in the timings.
