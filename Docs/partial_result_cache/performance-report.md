# Partial Result Cache for MariaDB Nested Loop Joins - Performance Report

## Scope

This report records local engineering performance validation for the MariaDB
join-only Partial Result Cache prototype. It is not an official TPC-H benchmark
result.

The measured case is derived from the MySQL PTRC nested-loop join validation
case documented at:

```text
/Users/zhuqingping/Work/Database/MySQL/taurusdbondstore/Docs/ptrc_tpch_nlj_cache_cases_20260624.md
```

The goal is to validate whether MariaDB benefits from a nested-loop join
per-key row-batch cache despite already having `subquery_cache`.

## Environment

Repository:

```text
/Users/zhuqingping/Work/Database/MariaDB/server
```

Build:

```text
build_release
13.1.0-MariaDB
```

Build command:

```bash
cmake --build build_release --target mariadbd --parallel 16
```

Server configuration used for the measurement on June 25, 2026:

| Setting | Value |
|---|---|
| Socket | `/tmp/mariadb-ptrc-stable-20260625-111204/mariadb.sock` |
| Network | `--skip-networking` |
| `local_infile` | `1` |
| `secure_file_priv` | empty string |
| Buffer pool | `1G` |

Raw artifacts from this run are under:

```text
/tmp/mariadb-ptrc-stable-20260625-111204/bench-results-fixed
```

## Dataset

TPC-H SF0.1 data was generated with dbgen and loaded into InnoDB.

Generated data files were under:

```text
/tmp/mariadb-ptrc-stable-20260625-111204/data
```

Loaded row counts:

| Table | Rows |
|---|---:|
| `part` | `20000` |
| `partsupp` | `80000` |
| `lineitem` | `600572` |

Relevant indexes:

```sql
partsupp:
  PRIMARY KEY(ps_partkey, ps_suppkey)
  KEY partsupp_fk1(ps_suppkey)
  KEY partsupp_fk2(ps_partkey)

lineitem:
  PRIMARY KEY(l_orderkey, l_linenumber)
  KEY lineitem_fk1(l_orderkey)
  KEY lineitem_fk2(l_partkey)
  KEY lineitem_fk3(l_suppkey)
  KEY lineitem_fk4(l_partkey, l_suppkey)
```

## Query

The measured query amplifies repeated `lineitem` probes by enumerating two
supplier-alternative dimensions for each part before joining `lineitem`:

```sql
SELECT SUM(cnt) AS total_rows
FROM (
  SELECT ps1.ps_partkey, COUNT(*) AS cnt
  FROM partsupp AS ps1
  JOIN partsupp AS ps2
    ON ps2.ps_partkey = ps1.ps_partkey
  JOIN partsupp AS ps3
    ON ps3.ps_partkey = ps2.ps_partkey
  JOIN lineitem AS l
    ON l.l_partkey = ps3.ps_partkey
  GROUP BY ps1.ps_partkey
) AS q;
```

All measured runs returned:

```text
38436608
```

## Method

Settings common to both modes:

```sql
SET query_cache_type=OFF;
SET join_cache_level=0;
SET rds_partial_result_cache_max_mem_size=1073741824;
```

The workload SQL does not use `SQL_NO_CACHE`. Query-cache behavior is fixed by
the session setting above so the measured SQL text stays identical when only
PTRC and `subquery_cache` switches are changed.

Only this optimizer switch changed:

```sql
SET optimizer_switch='partial_result_cache=off';
SET optimizer_switch='partial_result_cache=on';
```

`join_cache_level=0` was intentional. It isolates the new nested-loop/ref cache
from MariaDB's existing join-buffer framework. The current prototype rejects
join-buffer/BKA plans.

The benchmark runner applies the session setup and the measured query through
the same client connection for every warmup and measured round. This is
required because `optimizer_switch` is session scoped.

## Reproducibility Protocol

Use the benchmark package under:

```text
Docs/partial_result_cache/benchmarks
```

Required performance workload:

- TPCH-derived repeated nested-loop join Case 4.

The package also includes standard TPC-H Q17 as a correlated-subquery sanity
check for the switch matrix. Q17 is not used as PTRC join-cache performance
evidence.

Required matrix:

| Mode | `subquery_cache` | `partial_result_cache` |
|---|---|---|
| `sqoff_ptrcoff` | off | off |
| `sqoff_ptrcon` | off | on |
| `sqon_ptrcoff` | on | off |
| `sqon_ptrcon` | on | on |

For every workload and mode, capture:

- release build and server configuration;
- result hash;
- one or more warmup rounds and at least five measured rounds;
- `EXPLAIN`, `EXPLAIN FORMAT=JSON`, and `ANALYZE FORMAT=JSON`;
- `Partial_result_cache%`, `Subquery_cache%`, and `Handler_read%` deltas;
- optimizer trace for plan-choice causes.

The status variables are global cumulative counters, so reports must compute
deltas from before and after snapshots.

## Timing Results

Wall-clock times were reported from `/usr/bin/time -p`. Treat these numbers as
local engineering evidence, not an official benchmark result.

### Q17 Sanity Check

Standard TPC-H Q17 returned the same result hash in all modes:

```text
02b01b60f4c735118cfd9f876501c0e596bc48a09d161b1d3c334edaa0f432c7
```

At SF0.1 the query completed in `0.01s` to `0.02s`. This is only correctness
coverage for the switch matrix, not a PTRC join-cache performance claim.

### TPCH-derived NLJ Case 4

The join-only NLJ case returned the same result hash in all modes:

```text
7dfa9537230f354915d48860ea18f550562c2e9ca7958a4afd227976d7e58fab
```

Measured times:

| Mode | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 |
|---|---:|---:|---:|---:|---:|
| `sqoff_ptrcoff` | `2.34s` | `2.34s` | `2.31s` | `2.32s` | `2.32s` |
| `sqoff_ptrcon` | `0.71s` | `0.71s` | `0.72s` | `0.72s` | `0.74s` |
| `sqon_ptrcoff` | `2.37s` | `2.34s` | `2.32s` | `2.36s` | `2.40s` |
| `sqon_ptrcon` | `0.71s` | `0.72s` | `0.72s` | `0.75s` | `0.73s` |

Warm-run averages and sample standard deviation:

| Mode | Average | Stddev |
|---|---:|---:|
| `sqoff_ptrcoff` | `2.326s` | `0.012s` |
| `sqoff_ptrcon` | `0.720s` | `0.011s` |
| `sqon_ptrcoff` | `2.358s` | `0.027s` |
| `sqon_ptrcon` | `0.726s` | `0.014s` |

Reported local speedup:

| Comparison | Speedup |
|---|---:|
| `subquery_cache=off`, PTRC off vs on | `3.23x` |
| `subquery_cache=on`, PTRC off vs on | `3.25x` |

## Status Counters

Five-run cumulative status deltas for the NLJ case:

| Status | `sqoff_ptrcoff` | `sqoff_ptrcon` | `sqon_ptrcoff` | `sqon_ptrcon` |
|---|---:|---:|---:|---:|
| `Partial_result_cache_bypass` | `0` | `0` | `0` | `0` |
| `Partial_result_cache_hit` | `0` | `8100000` | `0` | `8100000` |
| `Partial_result_cache_miss` | `0` | `300000` | `0` | `300000` |
| `Partial_result_cache_rows_cached` | `0` | `3802860` | `0` | `3802860` |
| `Partial_result_cache_rows_replayed` | `0` | `196380180` | `0` | `196380180` |

Interpretation:

- repeated ref keys are frequent;
- no memory bypass occurred with the 1GB test cap;
- most output work for the inner side was served by row replay rather than
  repeated handler probes.

## ANALYZE FORMAT=JSON

The plan shape did not change. Both modes used:

| Table | Access |
|---|---|
| `ps1` | index scan on `partsupp_fk2` |
| `ps2` | `ref` on `partsupp_fk2` |
| `ps3` | `ref` on `partsupp_fk2` |
| `l` | `ref` on `lineitem_fk4` |

Total runtime from `ANALYZE FORMAT=JSON`:

| Mode | `r_total_time_ms` |
|---|---:|
| `sqoff_ptrcoff` | `2397.48` |
| `sqoff_ptrcon` | `739.96` |
| `sqon_ptrcoff` | `2433.60` |
| `sqon_ptrcon` | `749.05` |

Key access deltas:

| Table | Metric | PTRC off | PTRC on |
|---|---|---:|---:|
| `ps2` | `pages_accessed` | `160328` | `40082` |
| `ps3` | `pages_accessed` | `641312` | `40082` |
| `l` | `pages_accessed` | `2602240` | `40660` |

The `lineitem` engine page access reduction is approximately:

```text
2602240 / 40660 = 64.0x
```

## Conclusion

MariaDB's existing `subquery_cache` does not address this join-only repeated
inner-ref pattern. In the targeted TPCH-derived nested-loop join case, the
prototype reduced wall-clock time from about `2.33s` to about `0.72s`, and the
same result was reproduced with `subquery_cache` both off and on.

This evidence supports continuing the conservative join-only PRC patch. The
result should be presented as local engineering evidence unless the community
submission also archives the raw run artifacts and exact reproduction steps.
