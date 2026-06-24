# Partial Result Cache for MariaDB Nested Loop Joins - Test Report

## Scope

This report summarizes validation completed for the MariaDB join-only Partial
Result Cache prototype.

The validation scope is intentionally focused:

- configuration surface;
- `JT_REF` nested-loop join correctness;
- EXPLAIN and optimizer trace visibility;
- minimum cost-threshold selection;
- status counters;
- negative-entry replay;
- runtime low-hit-ratio bypass;
- release build;
- reported TPCH-derived nested-loop performance case.

It does not claim coverage for BKA/BKAH, outer join, semi join, or BLOB row
caching. Correlated subquery result caching is intentionally left to MariaDB's
existing `subquery_cache`.

## Build Validation

| Gate | Command | Result |
|---|---|---|
| Release server build | `cmake --build build_release --target mariadbd --parallel 16` | Passed |

Latest local validation command:

```bash
cd /Users/zhuqingping/Work/Database/MariaDB/server
cmake --build build_release --target mariadbd --parallel 16
```

Result: passed.

## Focused MTR

Focused tests:

- `sys_vars.partial_result_cache_basic`
- `main.partial_result_cache_join`

Canonical command:

```bash
cd /Users/zhuqingping/Work/Database/MariaDB/server/build_release/mysql-test
TMPDIR=/tmp ./mtr \
  sys_vars.partial_result_cache_basic \
  main.partial_result_cache_join \
  --parallel=1 \
  --vardir=/tmp/mariadb-ptrc-final-var \
  --tmpdir=/tmp/mariadb-ptrc-final-tmp \
  --force
```

Latest result:

```text
main.partial_result_cache_join           [ pass ]      3
sys_vars.partial_result_cache_basic      [ pass ]      2
Completed: All 2 tests were successful.
```

## Test Coverage Matrix

| Area | Test | Coverage |
|---|---|---|
| Optimizer switch registration | `sys_vars.partial_result_cache_basic`, `optimizer_switch_basic.result` | Confirms `partial_result_cache=off/on` can be displayed and toggled. |
| Session variables | `sys_vars.partial_result_cache_basic` | Confirms default values and session assignment for four `rds_partial_result_cache_*` variables. |
| Disabled-by-default behavior | `sys_vars.partial_result_cache_basic` | Confirms default optimizer switch includes `partial_result_cache=off`. |
| Join result correctness | `main.partial_result_cache_join` | Runs the same repeated-key join with PRC off and on; result rows match expected output. |
| Multi-row ref key replay | `main.partial_result_cache_join` | Uses key `1` with two inner rows and three outer rows, requiring cached multi-row replay. |
| Status counters | `main.partial_result_cache_join` | Expects 3 hits, 3 misses, 3 cached rows, and 4 replayed rows. |
| Negative cache entry replay | `main.partial_result_cache_join` | Repeats a no-match key and verifies it increments hit count without replaying rows. |
| VARCHAR and NULL row replay | `main.partial_result_cache_join` | Selects cached `VARCHAR` and nullable integer columns from `record[0]`. |
| Join-buffer isolation | `main.partial_result_cache_join` | Sets `join_cache_level=0` so the test covers normal nested-loop/ref execution. |
| EXPLAIN visibility | `main.partial_result_cache_join` | Confirms traditional EXPLAIN prints `Using partial result cache` for the selected inner ref table. |
| ANALYZE FORMAT=JSON counters | `main.partial_result_cache_join` | Confirms PTRC runtime hit, miss, cached-row, replayed-row, bypass, and memory counters on the selected inner ref table. |
| Optimizer trace visibility | `main.partial_result_cache_join` | Confirms trace contains `partial_result_cache`, `chosen: true`, and `cause: chosen by cost`. |
| Cost threshold rejection | `main.partial_result_cache_join` | Sets `rds_partial_result_cache_cost_threshold=1` and confirms EXPLAIN no longer marks PTRC while trace reports `cost below threshold`. |
| Runtime low-hit-ratio bypass | `main.partial_result_cache_join` | Sets `min_hit_ratio=0.9` and `hit_ratio_frequency=2` on a unique-key workload and verifies `Partial_result_cache_bypass=1`. |
| Unsupported shapes | `main.partial_result_cache_join` | Covers zero memory, BLOB row, and outer-join inner-table rejection through optimizer trace causes. |

## MTR Expected Counter Values

For the focused join test:

```text
Partial_result_cache_bypass          0
Partial_result_cache_hit             3
Partial_result_cache_miss            3
Partial_result_cache_rows_cached     3
Partial_result_cache_rows_replayed   4
```

The test data is:

```sql
CREATE TABLE t1 (a INT NOT NULL, b INT NOT NULL, KEY(a));
CREATE TABLE t2 (
  a INT NOT NULL,
  c INT NOT NULL,
  v VARCHAR(10),
  d INT,
  KEY(a)
);

INSERT INTO t1 VALUES (1,10),(1,11),(2,20),(3,30),(1,12),(3,31);
INSERT INTO t2 VALUES
  (1,100,'aa',NULL),(1,101,'bb',7),(2,200,'cc',NULL),(4,400,'dd',9);
```

The expected counters prove:

- first key `1` is a miss and stores two rows;
- later key `1` occurrences are hits;
- key `2` is a miss and stores one row;
- key `3` is a miss and stores a negative result;
- the second key `3` probe is a negative-entry hit;
- four rows are replayed from cache for the two repeated key `1` probes.

## TPCH-Derived Release Validation

The following is a local engineering measurement from June 25, 2026. It is not
an official benchmark result and not a gate proven by MTR. Community-facing
performance claims should archive the raw artifact bundle or regenerate it with
the benchmark package in this branch.

- MariaDB release build: `13.1.0-MariaDB`
- Dataset: TPCH SF0.1, InnoDB
- Raw artifacts:
  `/tmp/mariadb-ptrc-stable-20260625-111204/bench-results-fixed`
- Query shape: three `partsupp` self-join dimensions before `lineitem`
- `join_cache_level=0`
- `rds_partial_result_cache_max_mem_size=1073741824`
- Matrix: `subquery_cache=off|on`, `partial_result_cache=off|on`
- Warmup: 1 round per mode
- Measurement: 5 rounds per mode

The benchmark runner applies the session setup and measured query through the
same client connection for every warmup and timed round. This is required
because `optimizer_switch` is session scoped.

The benchmark harness can also run standard TPCH Q17 as a correlated-subquery
sanity case. In this run it completed in `0.01s` to `0.02s` at SF0.1 and
returned the same result hash in all modes:

```text
02b01b60f4c735118cfd9f876501c0e596bc48a09d161b1d3c334edaa0f432c7
```

This Q17 result is not PTRC performance evidence. It only checks that the
switch matrix preserves results on a correlated-subquery workload.

The TPCH-derived nested-loop join case returned the same result hash in all
modes:

```text
7dfa9537230f354915d48860ea18f550562c2e9ca7958a4afd227976d7e58fab
```

Five measured rounds:

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

Five-run cumulative status deltas for the nested-loop join case:

| Status | `sqoff_ptrcoff` | `sqoff_ptrcon` | `sqon_ptrcoff` | `sqon_ptrcon` |
|---|---:|---:|---:|---:|
| `Partial_result_cache_bypass` | `0` | `0` | `0` | `0` |
| `Partial_result_cache_hit` | `0` | `8100000` | `0` | `8100000` |
| `Partial_result_cache_miss` | `0` | `300000` | `0` | `300000` |
| `Partial_result_cache_rows_cached` | `0` | `3802860` | `0` | `3802860` |
| `Partial_result_cache_rows_replayed` | `0` | `196380180` | `0` | `196380180` |

Traditional EXPLAIN showed `Using partial result cache` for `ps2`, `ps3`, and
`lineitem` when PTRC was on. JSON EXPLAIN showed
`using_partial_result_cache: true` for the same inner ref tables.

`ANALYZE FORMAT=JSON` showed unchanged plan shape and lower engine page
accesses:

| Table | Metric | PTRC off | PTRC on |
|---|---|---:|---:|
| `ps2` | `pages_accessed` | `160328` | `40082` |
| `ps3` | `pages_accessed` | `641312` | `40082` |
| `l` | `pages_accessed` | `2602240` | `40660` |

## Static And Packaging Checks

Current community-facing patch should include:

- product code under `sql/`;
- focused MTR tests and result files;
- `Docs/partial_result_cache/` public design and validation documents.

Current community-facing patch should exclude:

- `build_debug/`
- `build_release/`
- generated benchmark data and `/tmp` logs
- `.DS_Store`
- local agent context files unless the maintainer explicitly wants them
- internal task-board docs if preparing a minimal upstream patch

## Remaining Validation Before Upstream Submission

| Priority | Item | Status |
|---|---|---|
| Required | JSON EXPLAIN exposes PTRC fields | Covered by `partial_result_cache_join`. |
| Required | `PRC_JOIN` and `NO_PRC_JOIN` force/disable behavior | Covered by `partial_result_cache_join`. |
| Required | Runtime memory-cap bypass | Covered by `partial_result_cache_join`. |
| Required | Repeated negative key cache | Covered by `partial_result_cache_join`. |
| Required | BKA/BKAH rejection | Still needs a stable plan-shape regression. |
| Required | Semi-join rejection | Still needs a stable semi-join fixture. |
| Required | Reproducible benchmark artifacts | Harness added; release raw artifacts generated and should be archived before PR. |
| Optional | Debug build and focused debug MTR | Recommended before final PR. |
| Optional | Debug/fault injection for handler errors | Deferred unless maintainers request it. |
| Optional | Rename `rds_` variables | Product decision before final community PR. |

Additional useful gates:

- `git diff --check`.
- A broader MTR subset touching joins, optimizer switch output, status
  variables, and handler/read semantics.
- Broader costing tests with analyzed data and low-duplicate workloads.
- Optional sanitizer run if the community process expects it.
