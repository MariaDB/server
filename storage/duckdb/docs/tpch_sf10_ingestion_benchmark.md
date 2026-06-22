# TPC-H SF10 Ingestion Benchmark — DuckDB vs MariaDB DuckDB Engine

Comparison of loading the **same** TPC-H SF10 CSV dataset (~11 GB, 86.6M rows)
into DuckDB through three different paths.

## Hardware

| Component | Value |
|---|---|
| CPU | 13th Gen Intel Core i7-13700H (14 cores: 6 P + 8 E, 20 threads) |
| Architecture | x86_64 |
| Logical CPUs | 20 (matches DuckDB's `threads=20`) |
| RAM | 64 GB (65,501,828 kB total) |
| Storage | NVMe SSD (`/dev/nvme0n1p2`), ext4, 833 GB volume |
| Data location | `/git` (same NVMe volume) |

## Environment

| Component | Value |
|---|---|
| Data | TPC-H SF10, CSV, header row, `"`-quoted text fields |
| Generator | `tpchgen-cli` v2.0.2 (Apache DataFusion `tpchgen-rs`, pure Rust) |
| DuckDB | v1.5.2 (Variegata), CLI binary at `/duckdb` |
| MariaDB | 11.4.13, embedded DuckDB engine (`ENGINE=DUCKDB`) |
| DuckDB threads | 20 |
| DuckDB `memory_limit` | 1.0 GiB (embedded default) / 8 GB (tuned) / ~80% RAM (standalone CLI) |
| Data dir | `/git/tpch/` |

## Row counts (all paths, verified)

| Table | Rows |
|---|---:|
| lineitem | 59,986,052 |
| orders | 15,000,000 |
| partsupp | 8,000,000 |
| part | 2,000,000 |
| customer | 1,500,000 |
| supplier | 100,000 |
| nation | 25 |
| region | 5 |
| **TOTAL** | **86,586,082** |

## Methods

1. **Standalone CLI** — `/duckdb tpch_sf10.duckdb < ingest_sf10.sql`; native parallel
   `COPY ... FROM '*.csv'` into a persistent DB file. Script: `ingest_sf10.sql`.
2. **In-engine COPY** — `SELECT run_in_duckdb('COPY ... FROM ...')` runs DuckDB's
   native CSV reader *inside* the MariaDB server process. Script: `bench_duckdb_engine.sh`.
3. **LOAD DATA LOCAL INFILE** — standard MariaDB client path into `ENGINE=DUCKDB`
   tables. Script: `load_data_infile.sh`.

## Results — per-table wall-clock (seconds)

| Table | Rows | Standalone CLI | In-engine COPY (8 GB) | LOAD DATA LOCAL INFILE |
|---|---:|---:|---:|---:|
| lineitem | 59,986,052 | 16.88 | 12.66 | 299.02 |
| orders | 15,000,000 | 8.62 | 9.70 | 55.63 |
| partsupp | 8,000,000 | 6.82 | 5.26 | 28.51 |
| part | 2,000,000 | 2.62 | 2.70 | 8.54 |
| customer | 1,500,000 | 2.85 | 2.16 | 7.73 |
| supplier | 100,000 | 0.15 | 0.13 | 0.43 |
| nation | 25 | 0.004 | 0.04 | 0.05 |
| region | 5 | 0.006 | 0.04 | 0.05 |
| **TOTAL** | **86,586,082** | **38.4** | **32.69** | **~400 (6.7 min)** |

> In-engine COPY numbers use the tuned `memory_limit=8GB` (warm page cache). The 1 GiB
> default totals 52.0 s (see sub-table). Run-to-run variance is non-trivial for `lineitem`
> (measured 12.7–19.5 s at 8 GB across runs).

### `memory_limit` effect on `lineitem` (in-engine COPY)

| `memory_limit` | lineitem load | vs standalone (16.88 s) |
|---|---:|---:|
| 1.0 GiB (default) | 34.37 s | 2.0× slower |
| 8 GB (tuned) | 12.7–19.5 s | ~0.75–1.16× |

## Observations

- **COPY is the fast path.** DuckDB's native CSV reader parses the file directly with
  all 20 threads (vectorized, parallel). This holds whether run from the standalone CLI
  or in-process via `run_in_duckdb`.

- **The embedded `memory_limit=1.0 GiB` is the main in-engine COPY bottleneck.** Verified,
  not assumed: raising it to 8 GB cut `lineitem` from 34.4 s to ~12.7 s (full re-run) — on
  par with or faster than the standalone CLI's 16.9 s. At 8 GB the whole in-engine load
  (32.7 s) even edges out the standalone CLI (38.4 s) under a warm cache. The cap mainly
  hurts the largest table; smaller tables are unaffected and a couple are marginally faster
  in-engine. `memory_limit` set via `run_in_duckdb` is instance-level and persists
  across connections.

- **`LOAD DATA LOCAL INFILE` is ~12× slower than in-engine COPY** (totals: ~400 s vs 32.7 s;
  `lineitem`: 299 s vs 12.7 s ≈ 24×). This is a **structural code-path difference**, not a
  tuning issue: `LOAD DATA` routes every row through MariaDB's SQL/handler layer
  (`handler::write_row` → engine append), serializing what COPY does in parallel, and
  bypassing DuckDB's vectorized CSV parser entirely.

- **DuckDB-engine table requirements (discovered):** the engine **rejects non-UTF8
  charsets** and **requires a PRIMARY KEY**. TPC-H natural keys were used
  (`partsupp` and `lineitem` get composite PKs). Per-row PK index maintenance adds
  further overhead to the `LOAD DATA` path.

- **CSV quoting matters.** tpchgen emits `"`-quoted text fields because comments contain
  commas; `LOAD DATA` needs `OPTIONALLY ENCLOSED BY '"'` and `IGNORE 1 LINES` for the header.

- **Binary logging:** the server already ran with `@@log_bin = 0`; `SET SESSION
  sql_log_bin=0` was also issued, so binlog overhead is excluded from all `LOAD DATA` numbers.

## Recommendation

For bulk-loading external files into the MariaDB DuckDB engine, prefer the **in-engine
`COPY` via `run_in_duckdb`** over `LOAD DATA LOCAL INFILE`, and raise the embedded
DuckDB `memory_limit` for large tables. Reserve `LOAD DATA` for cases where data must
flow through the standard MariaDB SQL path.
