# DuckDB Storage Engine for MariaDB

A pluggable storage engine that brings [DuckDB](https://github.com/duckdb/duckdb)'s columnar analytical engine inside [MariaDB Server](https://github.com/MariaDB/server).

Create a table with `ENGINE=DuckDB` and analytical queries against it are executed through DuckDB's columnar vectorized engine — no ETL pipelines, no separate cluster, no additional protocols. One server, one SQL interface, the familiar `mariadb` client.

## Use Cases

- **HTAP (Hybrid Transactional/Analytical Processing)** — InnoDB handles OLTP, DuckDB handles analytics, both in the same database.
- **Ad-hoc analytical queries** — complex joins, aggregations, subqueries, and window functions over large datasets without exporting data to a separate system.
- **Eliminating ETL complexity** — no need for a dedicated analytical cluster or data movement pipelines; the analytical engine runs in-process.

## Roadmap

- **Analytical GIS** — deliver geospatial analytics via DuckDB's Spatial extension.
- **Faster HTAP over InnoDB-only data** — run queries against InnoDB-only data through DuckDB's vectorized execution.
- **Partial query pushdown** — implement a derived handler to push parts of complex queries down into DuckDB.
- **Data lake access** — reach data lake protocols and formats via DuckDB extensions.

## Performance

TPC-H Scale Factor 10 (~11 GB raw data, 86.6M rows total, ~60M in `lineitem`).

**Hardware / environment:** Intel Core i7-13700H (14 cores / 20 threads), 64 GB RAM, NVMe SSD (ext4). MariaDB 11.4.13 with embedded DuckDB v1.5.2, `duckdb_memory_limit=8 GiB`, `threads=20`. Warm runs; numbers stable to ~±15%.

| Metric | Result |
|---|---|
| Data loading (in-engine `COPY`) | 33 seconds |
| All 22 TPC-H queries | **4.30 seconds** total |

### Per-query latency (seconds, warm)

| Query | Time | Query | Time | Query | Time |
|---|---:|---|---:|---|---:|
| q01 | 0.253 | q09 | 0.337 | q17 | 0.116 |
| q02 | 0.076 | q10 | 0.253 | q18 | 0.318 |
| q03 | 0.134 | q11 | 0.049 | q19 | 0.207 |
| q04 | 0.135 | q12 | 0.152 | q20 | 0.166 |
| q05 | 0.142 | q13 | 0.606 | q21 | 0.486 |
| q06 | 0.070 | q14 | 0.128 | q22 | 0.113 |
| q07 | 0.133 | q15 | 0.101 | **Total** | **4.30** |
| q08 | 0.145 | q16 | 0.178 | | |

Each query also pays a small fixed cost (~40 ms) for client connect and pushdown setup — noticeable on the cheapest queries, negligible on the heavy analytical ones. See [`docs/tpch_sf10_query_benchmark.md`](docs/tpch_sf10_query_benchmark.md) and [`docs/tpch_sf10_ingestion_benchmark.md`](docs/tpch_sf10_ingestion_benchmark.md) for full methodology.

## How It Works

DuckDB is an in-process analytical database. Its performance rests on three pillars:

- **Columnar storage** — minimizes unnecessary data reads.
- **Vectorized execution** — processes data in batches for maximum CPU cache efficiency.
- **Parallelism** — leverages all available processor cores.

Tables created with `ENGINE=DuckDB` store data in DuckDB's native format. Queries are translated and executed by the DuckDB engine. InnoDB and DuckDB tables coexist in the same database.

## Building

The engine is built as part of the MariaDB server tree. It lives under `storage/duckdb/` and uses `ExternalProject_Add` to build DuckDB v1.5.2 from source.

Clone the branch matching your target MariaDB version. The engine is already part of the tree under `storage/duckdb/`:

```bash
# Pick the branch matching your target MariaDB version
git clone --recurse-submodules -b 11.4 https://github.com/MariaDB/server.git mariadb-server
# or: -b 11.8
# or: -b 12.3
cd mariadb-server

# Install build dependencies (requires root)
./storage/duckdb/build.sh -D

# Build and install
./storage/duckdb/build.sh
```

### Build dependencies

Before building packages, install the build dependencies. This is a one-time setup that installs the system packages and toolchain required to compile the server and the DuckDB engine (requires root):

```bash
./storage/duckdb/build.sh -D
```

### Build packages

With the dependencies installed, build a distributable package for your platform.

**RPM** (Rocky/Fedora/Amazon Linux):

```bash
./storage/duckdb/build.sh -p
```

**DEB** (Debian/Ubuntu):

```bash
./storage/duckdb/build.sh -p
```

## Cross-Engine Queries

The engine supports cross-engine joins — a single `SELECT` can combine DuckDB tables with tables from other engines (e.g. InnoDB). When the query planner detects a mix of engines, the `select_handler` does the following:

1. Opens the non-DuckDB tables via MariaDB's handler API and registers them in a thread-local table registry.
2. Derives a per-table predicate from the query's `WHERE` (via `make_cond_for_table`) and registers it alongside each external table.
3. Pushes the **entire query** — including all `WHERE`, `JOIN`, `GROUP BY`, and `ORDER BY` clauses — down to DuckDB as a single SQL statement.
4. DuckDB's replacement scan callback transparently redirects references to non-DuckDB tables to the `_mdb_scan` table function.

The `_mdb_scan` table function does **not** loop over the handler directly. On its first call it lazily spawns a **cooperative fiber** (stack-switching runtime borrowed from `libmariadb`) running on a dedicated background `THD`. Inside the fiber it builds a synthetic, projection- and predicate-pushed
`SELECT <needed columns> FROM <db>.<table> [WHERE <pushed predicate>]`
and executes it through the **full MariaDB pipeline** (`lex_start` → `parse_sql` → `mysql_execute_command`). A custom `select_result_interceptor` (`select_to_duckdb`) converts each result row's `Item`s into a DuckDB `DataChunk`; when a chunk fills (`STANDARD_VECTOR_SIZE`) the fiber **yields** back to DuckDB, which consumes the chunk and resumes the fiber for the next batch.

Because the external scan runs through `mysql_execute_command`, it uses the **MariaDB optimizer and all available access paths** (index range/ref access, etc.), and the pushed `WHERE` predicate is evaluated by MariaDB *before* rows reach DuckDB. On the DuckDB side, the full query text lets DuckDB's optimizer reorder joins, build hash tables, and run all aggregation/sorting over the streamed rows. A direct `ha_rnd_next` loop is retained only as a fallback when no fiber is used.

This means queries like the following just work:

```sql
SELECT d.id, d.amount, i.name
  FROM analytics.orders d          -- ENGINE=DuckDB
  JOIN inventory.products i        -- ENGINE=InnoDB
    ON d.product_id = i.id
 WHERE d.amount > 1000;
```

DuckDB handles the join, aggregation, and sorting; InnoDB rows are produced on demand by a fiber-driven MariaDB query. No data copying or ETL is required.

## Current Limitations

- **DECIMAL precision > 38** — DuckDB supports up to 38 digits; wider MariaDB DECIMALs will fail on DDL conversion.
- **Some MariaDB functions are yet not pushdown-compatible** — `GROUP_CONCAT()`, `DATE_FORMAT()`, `JSON_CONTAINS()`, `FOUND_ROWS()`, `LAST_INSERT_ID()`, and a few others have no DuckDB equivalent or differ in syntax. Such queries fall back to MariaDB execution.
- **Strict GROUP BY** — DuckDB rejects `SELECT` columns not in `GROUP BY` and not aggregated, even when MariaDB's `sql_mode` allows it.
- **XA transactions** — `XA PREPARE` is not supported by the engine.
- **Collations** — MariaDB UCA-based collation rules are approximated via DuckDB's built-in `NOCASE`/`NOACCENT` collations for UTF-8 charsets; non-UTF8 charsets fall back to binary comparison. See [`docs/collation-mapping.md`](docs/collation-mapping.md) for the full mapping and known gaps.
- **Cross-engine scan is yet single-threaded** — each external (non-DuckDB) table is produced by a single fiber-driven MariaDB query (`_mdb_scan` reports `MaxThreads() == 1`); only the DuckDB side of the query is parallelized.
- **ALTER COLUMN DROP DEFAULT** — not propagated to DuckDB catalog.
- **Timezone propagation** — `TIMESTAMP` columns are stored as `TIMESTAMPTZ`; timezone must be set consistently between MariaDB and DuckDB contexts to avoid shifts.

See [`docs/mariadb-duckdb-incompatibilities.md`](docs/mariadb-duckdb-incompatibilities.md) for a detailed compatibility matrix.

## License

This project is licensed under the GNU General Public License v2. See [COPYING](COPYING) for details.

DuckDB itself is licensed under the MIT License.

## Acknowledgments

**Alibaba and the AliSQL Project** — A special thank you to Alibaba and their engineering team for open-sourcing [AliSQL](https://github.com/alibaba/AliSQL). AliSQL is a MySQL branch developed at Alibaba Group and extensively used in their production infrastructure. The December 2025 open-source release of AliSQL 8.0 included integration of DuckDB as a native storage engine, providing a valuable reference implementation and validating the viability of embedding DuckDB inside a MySQL-compatible server. The DuckDB Engine for MariaDB draws heavily on this experience.

**The DuckDB Project** — None of this would be possible without the remarkable work of the [DuckDB team](https://github.com/duckdb/duckdb). DuckDB has grown from an academic research project into one of the most impressive analytical engines available today — fast, embeddable, dependency-free, and released under the permissive MIT License. Its clean C++ codebase and well-defined embedding API are what make integrations like this one feasible.
