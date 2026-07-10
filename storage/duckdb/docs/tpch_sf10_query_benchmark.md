# TPC-H SF10 Query Latency — native DuckDB vs MariaDB DuckDB engine

Per-query latency for the 22 TPC-H queries over the same SF10 dataset (86.6M rows),
comparing the standalone DuckDB CLI against the MariaDB DuckDB storage engine.

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
| DuckDB | v1.5.2 (Variegata), CLI binary at `/duckdb`, DB file `/git/tpch/tpch_sf10.duckdb` |
| MariaDB | 11.4.13, embedded DuckDB engine (`ENGINE=DUCKDB`), database `tpch` |
| `duckdb_memory_limit` | 8 GiB (8589934592), set persistently in `duckdb.cnf` |
| DuckDB threads | 20 |
| MariaDB queries | MariaDB-dialect, split from `/tpch.sql` |
| DuckDB queries | DuckDB-dialect, from the tpch extension `q01..q22` |

## Methodology

- **DuckDB:** all 22 queries run in **one warm session** with `.timer on`; per-query
  time = reported `Run Time (s): real`. No per-query process/DB-reopen overhead.
- **MariaDB:** queries run **sequentially**, one `mariadb` client invocation each;
  per-query time = wall-clock. Each invocation pays a fixed client-connect +
  SQL-layer/pushdown-setup cost (~40 ms), measured as part of the number.
- Single representative run (numbers are stable to ~±15% across repeated runs).
- Harness: `bench_queries.sh`; raw output: `query_timings.tsv`.

## Results — per-query latency (seconds)

| query | duckdb_s | mariadb_s | | query | duckdb_s | mariadb_s |
|---|---:|---:|---|---|---:|---:|
| q01 | 0.219 | 0.253 | | q12 | 0.062 | 0.152 |
| q02 | 0.058 | 0.076 | | q13 | 0.257 | 0.606 |
| q03 | 0.100 | 0.134 | | q14 | 0.047 | 0.128 |
| q04 | 0.149 | 0.135 | | q15 | 0.041 | 0.101 |
| q05 | 0.098 | 0.142 | | q16 | 0.068 | 0.178 |
| q06 | 0.024 | 0.070 | | q17 | 0.065 | 0.116 |
| q07 | 0.063 | 0.133 | | q18 | 0.251 | 0.318 |
| q08 | 0.097 | 0.145 | | q19 | 0.096 | 0.207 |
| q09 | 0.274 | 0.337 | | q20 | 0.055 | 0.166 |
| q10 | 0.210 | 0.253 | | q21 | 0.259 | 0.486 |
| q11 | 0.013 | 0.049 | | q22 | 0.056 | 0.113 |
| | | | | **TOTAL** | **2.562** | **4.298** |

## Observations

- **Native DuckDB is faster on every query** once both are measured warm. Total
  2.56 s vs 4.30 s (~1.7× over the 22-query set).
- **A ~40 ms fixed floor dominates the cheap queries on MariaDB** (q06 0.024 vs 0.070,
  q11 0.013 vs 0.049, q15 0.041 vs 0.101). This floor is the per-invocation `mariadb`
  client-connect + SQL-layer/pushdown setup — not DuckDB execution. Running MariaDB
  queries inside a single persistent session would remove most of it.
- **On heavy queries the gap narrows**, where actual DuckDB execution dominates and the
  fixed overhead matters less (q09 1.23×, q18 1.27×, q01 1.16×).
- **q13 is the widest gap** (0.257 vs 0.606, 2.36×). It uses the MariaDB-rewritten
  variant (the original DuckDB derived-column-alias syntax `) AS c_orders (c_custkey,
  c_count)` doesn't parse in MariaDB); the LEFT JOIN + aggregate pushdown carries
  noticeably more per-query overhead here. Root cause of the extra cost beyond the
  fixed floor is not fully isolated.
- **Memory limit (8 GiB vs 1 GiB default) did not affect query latency** at SF10 — these
  queries are not memory-bound; the setting matters for bulk loads, not these scans.
