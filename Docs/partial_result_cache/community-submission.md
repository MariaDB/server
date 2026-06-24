# Partial Result Cache for MariaDB Nested Loop Joins - Community Submission Notes

## Recommended Patch Scope

Include:

- Product source changes under `sql/`:
  - `sql/sql_priv.h`
  - `sql/sql_class.h`
  - `sql/sys_vars.cc`
  - `sql/partial_result_cache.h`
  - `sql/partial_result_cache.cc`
  - `sql/opt_hints_structs.h`
  - `sql/opt_hints.cc`
  - `sql/opt_hints_parser.h`
  - `sql/opt_hints_parser.cc`
  - `sql/sql_select.h`
  - `sql/sql_select.cc`
  - `sql/mysqld.cc`
- Focused MTR tests and result files:
  - `mysql-test/suite/sys_vars/t/partial_result_cache_basic.test`
  - `mysql-test/suite/sys_vars/r/partial_result_cache_basic.result`
  - `mysql-test/suite/sys_vars/r/optimizer_switch_basic.result`
  - `mysql-test/main/partial_result_cache_join.test`
  - `mysql-test/main/partial_result_cache_join.result`
- Community-facing docs under `Docs/partial_result_cache/`.
- Benchmark reproduction scripts under `Docs/partial_result_cache/benchmarks/`.

Usually exclude from an upstream community patch:

- `build_debug/`
- `build_release/`
- generated benchmark data and `/tmp` logs
- `.DS_Store`
- local agent files such as `AGENTS.md` and `CLAUDE.md` unless the target
  branch explicitly wants them
- internal task-board docs unless maintainers want the development history
  included

## Current Community Commit Series

The `support_ptrc` branch is organized as a short community-facing review
series:

```text
7e3afa516d2 MDEV-PTRC: Cache repeated nested-loop ref rows
af4390d1f1c MDEV-PTRC: Test nested-loop ref row replay
HEAD        MDEV-PTRC: Document nested-loop ref cache design
```

This split mirrors the MySQL PTRC contribution style: product runtime changes
are separated from focused MTR coverage and public documentation, while keeping
the series short enough for review.

## Proposed Community Commit Message

```text
MDEV-PTRC: Add partial result cache for nested-loop ref joins

Problem:
MariaDB has subquery_cache and join-buffer based BNL/BKA execution, but neither
memoizes the full row batch for an ordinary nested-loop join inner ref lookup.
When the outer side repeats the same key many times, the executor repeatedly
performs the same handler probe and scans the same matching inner rows.

This pattern appears in join-only plans such as a partsupp self-join followed
by lineitem lookup in TPCH-derived tests. With join_cache_level=0, MariaDB's
normal nested-loop executor probes lineitem 1,280,000 times for the tested
SF0.1 case.

Solution:
Add an experimental statement-local Partial Result Cache for safe JT_REF join
tabs, controlled by optimizer_switch=partial_result_cache and disabled by
default.

The cache is installed after pick_table_access_method() in make_join_readinfo().
For eligible JT_REF tables, it wraps join_read_always_key() and
join_read_next_same(). On a miss, the original handler ref lookup is executed
and the complete inner row batch is copied from TABLE::record[0]. On a later
hit for the same ref key, cached rows are restored into TABLE::record[0], and
the existing evaluate_join_record() path performs normal predicate and upper
join processing.

The first patch deliberately rejects risky shapes:
- first join table;
- non-JT_REF access;
- selected join-buffer/BKA paths;
- outer join and semi-join inner tables;
- TABLE_REF::disable_cache;
- trigger-guarded ref access;
- zero-length ref keys;
- BLOB tables;
- stronger-than-read lock types.

The cache is statement-local and freed from JOIN_TAB::cleanup(). If the
configured memory cap is exceeded, allocation fails at runtime, or the observed
hit ratio falls below the configured threshold, it clears its contents and uses
normal ref access for the rest of the statement.

The patch exposes the choice in traditional EXPLAIN, JSON EXPLAIN, and
optimizer trace. It also uses `rds_partial_result_cache_cost_threshold` as a
minimum estimated hit-ratio gate; candidates below the threshold remain on the
normal ref access path and report `cost below threshold` in optimizer trace.

`PRC_JOIN` and `NO_PRC_JOIN` provide reviewer-controlled table/query-block
selection. `PRC_JOIN` can override the optimizer switch and cost threshold for
safe nested-loop ref candidates. It cannot bypass safety checks. `NO_PRC_JOIN`
disables PTRC for the matched table or query block.

User-visible interface:
- optimizer_switch=partial_result_cache
- rds_partial_result_cache_max_mem_size
- rds_partial_result_cache_cost_threshold
- rds_partial_result_cache_min_hit_ratio
- rds_partial_result_cache_hit_ratio_frequency
- Partial_result_cache_hit
- Partial_result_cache_miss
- Partial_result_cache_rows_cached
- Partial_result_cache_rows_replayed
- Partial_result_cache_bypass
- PRC_JOIN / NO_PRC_JOIN optimizer hints

All four `rds_partial_result_cache_*` variables are active in this patch.
`max_mem_size` and `cost_threshold` control eligibility. `min_hit_ratio` and
`hit_ratio_frequency` control runtime bypass for low-hit workloads.

Compatibility:
The feature is disabled by default. Unsupported shapes use the existing
execution path. It does not change MariaDB subquery_cache behavior and does not
share cached rows across statements, sessions, or transactions.

Test:
Built release server:
  cmake --build build_release --target mariadbd --parallel 16

Ran focused MTR:
  cd build_release/mysql-test
  TMPDIR=/tmp ./mtr sys_vars.partial_result_cache_basic \
    main.partial_result_cache_join \
    --parallel=1 \
    --vardir=/tmp/mariadb-ptrc-final-var \
    --tmpdir=/tmp/mariadb-ptrc-final-tmp \
    --force

Result:
  main.partial_result_cache_join           [ pass ]
  sys_vars.partial_result_cache_basic      [ pass ]
  Completed: All 2 tests were successful.

Performance:
Reported local engineering validation, not an official TPC-H result. Archive or
regenerate raw logs before using these numbers as a reviewed performance claim.

TPC-H SF0.1 derived join-only Case 4, InnoDB, release build,
join_cache_level=0, max_mem_size=1G, five measured rounds:
- subquery_cache=off, PTRC off average: 2.326s
- subquery_cache=off, PTRC on average:  0.720s
- subquery_cache=off speedup:           3.23x
- subquery_cache=on, PTRC off average:  2.358s
- subquery_cache=on, PTRC on average:   0.726s
- subquery_cache=on speedup:            3.25x

Five-run PTRC-on status delta in each subquery_cache mode:
- Partial_result_cache_hit: 8100000
- Partial_result_cache_miss: 300000
- Partial_result_cache_rows_cached: 3802860
- Partial_result_cache_rows_replayed: 196380180
- Partial_result_cache_bypass: 0

ANALYZE FORMAT=JSON reportedly showed unchanged plan shape. lineitem
pages_accessed dropped from 2,602,240 to 40,660 in that local run.
```

## Review Checklist

Before opening the community PR:

- Run `git diff --check`.
- Confirm no build outputs are staged.
- Confirm docs intended for public review are under `Docs/partial_result_cache/`.
- Confirm internal task-board material is excluded or intentionally included.
- Re-run focused MTR from the build tree, not source-tree `mysql-test/`.
- Add or run negative tests for rejected shapes if maintainers require them in
  the initial patch.
- Verify `PRC_JOIN` / `NO_PRC_JOIN` normalization and trace output.
- Archive the June 25, 2026 release-build benchmark artifacts or regenerate
  them before making community-facing performance claims.
- Decide whether to squash local commits into one feature commit.
- Decide whether the `rds_` variable prefix is acceptable for MariaDB
  community submission or should be renamed before upstream review.

## Known Review Questions

## Recommended MDEV/Jira Draft

```text
Title:
MDEV-xxxxx: Add partial result cache for repeated nested-loop ref joins

Problem:
MariaDB nested-loop joins can repeatedly probe the same inner JT_REF key when
outer rows contain duplicate join keys. Existing execution repeats handler
lookups and scans the same matching inner rows. subquery_cache handles
subquery expression results, not ordinary join table row batches. JOIN_CACHE
batches join execution, but does not memoize and replay the full row batch for
one inner ref key.

Motivation:
The feature targets join-only workloads where duplicate outer keys amplify
handler reads, page accesses, and CPU work. Local engineering evidence on a
TPCH-derived nested-loop case shows reduced handler work. Community-facing
performance claims should cite archived raw logs from the benchmark package in
this branch.

Proposed solution:
Add a statement-local partial result cache for eligible inner JT_REF join tabs.
On miss, execute the original ref lookup and cache copied TABLE::record[0]
rows for the ref key. On hit, replay cached rows into TABLE::record[0] and
continue through normal evaluate_join_record() processing.

Supported scope:
- optimizer_switch=partial_result_cache, default off
- PRC_JOIN / NO_PRC_JOIN table and query-block hints
- JT_REF inner table only
- no selected join buffer
- statement-local cache lifetime
- EXPLAIN, JSON EXPLAIN, optimizer trace visibility
- ANALYZE FORMAT=JSON runtime PTRC counters
- global cumulative status counters
- memory cap and runtime low-hit bypass

Non-goals:
- replacing MariaDB subquery_cache
- replacing JOIN_CACHE / BNL / BKA / BKAH
- cross-statement/session cache
- BLOB row caching
- outer join and semi join inner tables
- histogram-based cost model
- LRU eviction

Compatibility and risk:
The feature is disabled by default. Unsupported shapes fall back to the
existing executor path. Main risks are raw record-buffer replay assumptions,
costing false positives, memory pressure, and interaction with join-buffer,
outer-join, semi-join, and handler state.

Testing plan:
Run focused sys_vars and main MTR coverage, then archive or regenerate release
benchmark raw logs for the TPCH-derived repeated-NLJ case with subquery_cache
on/off crossed with PTRC on/off. Standard TPC-H Q17 may be run as a
correlated-subquery sanity check, but it is not PTRC performance evidence.
```

### Why not reuse MariaDB subquery_cache?

The target is ordinary nested-loop join inner table access, not expression or
subquery result caching. MariaDB `subquery_cache` does not memoize a row batch
for repeated `JT_REF` join keys.

### Why not extend JOIN_CACHE?

MariaDB `JOIN_CACHE` implements BNL/BKA/BKAH batching. The first PTRC patch is
per-key memoization for the normal nested-loop/ref executor path. It rejects
join-buffer paths so behavior remains isolated and reviewable.

### Why raw record copies?

The first target is a safe fixed-row `JT_REF` implementation. BLOB tables are
rejected because raw `record[0]` copies are not enough for pointer-owned BLOB
payloads. A later patch can use a more general row-packing abstraction.

### Why is the cost model intentionally small?

The first reviewable patch uses plan-level probe and ref-cost estimates to
avoid installing PTRC when the estimated hit ratio is below
`rds_partial_result_cache_cost_threshold`. It does not yet use histograms or a
precise distinct estimate for the outer ref expressions; those can be reviewed
as a separate costing improvement.

### Why disabled by default?

The first community patch is intentionally conservative. It provides correctness
tests, EXPLAIN/trace visibility, and a performance proof point while leaving
broader plan shapes and richer costing to separate reviews.
