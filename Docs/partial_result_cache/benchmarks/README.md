# PTRC Benchmark Reproduction Package

This directory contains a reproducible local benchmark harness for the
MariaDB nested-loop join Partial Result Cache prototype.

The harness is intended to produce review artifacts, not official benchmark
claims. Attach the generated raw output to an MDEV or pull request before
quoting any speedup.

## Workloads

- `sql/q17.sql`: standard TPC-H Q17 correlated subquery workload.
- `sql/nlj_case4.sql`: TPCH-derived repeated nested-loop join case that
  stresses repeated `JT_REF` inner lookups.

Run both workloads with this 2x2 matrix:

| Mode | `subquery_cache` | `partial_result_cache` |
|---|---|---|
| `sqoff_ptrcoff` | off | off |
| `sqoff_ptrcon` | off | on |
| `sqon_ptrcoff` | on | off |
| `sqon_ptrcon` | on | on |

## Expected Schema

The SQL files assume the standard TPC-H table names and indexes used by the
local MariaDB PTRC validation:

- `part(p_partkey, p_brand, p_container)`
- `lineitem(l_partkey, l_quantity, l_extendedprice, ...)`
- `partsupp(ps_partkey, ps_suppkey, ...)`

Use a release build and a fixed, documented server configuration. For the NLJ
case, use `join_cache_level=0` to isolate PTRC from MariaDB join-buffer plans.

## Usage

```bash
Docs/partial_result_cache/benchmarks/run_ptrc_benchmark.sh \
  --mysql build_release/client/mariadb \
  --socket /tmp/mariadb-ptrc-release.sock \
  --database tpch_sf1 \
  --out /tmp/mariadb-ptrc-bench-$(date +%Y%m%d-%H%M%S)
```

The script writes:

- `summary.tsv`: workload, mode, round, seconds, and result hash.
- `environment.txt`: client/server variables and version.
- `plans/*.txt`: `EXPLAIN`, `EXPLAIN FORMAT=JSON`, and
  `ANALYZE FORMAT=JSON` output.
- `status/*.txt`: `Partial_result_cache%`, `Subquery_cache%`, and
  `Handler_read%` snapshots before and after measured statements.
- `results/*.txt`: raw query output used for the SHA-256 hash.

Use at least one warmup and five measured rounds for community-facing
evidence. Keep result hashes identical across modes before comparing timings.
