# Partial Result Cache for MariaDB Nested Loop Joins - High Level Design

## Summary

Partial Result Cache (PRC/PTRC) accelerates nested-loop joins that repeatedly
execute the same inner `ref` lookup for duplicate outer key values.

MariaDB already has `subquery_cache` for correlated subquery expression caching
and a separate `JOIN_CACHE` framework for BNL/BKA/BKAH join buffering. This
design does not replace either subsystem. It adds a conservative, statement
local memoization layer for ordinary nested-loop `JT_REF` table access.

The first implementation is a guarded prototype suitable for community review:

- disabled by default;
- isolated to `JT_REF`;
- controllable with `PRC_JOIN` and `NO_PRC_JOIN` optimizer hints;
- rejected for outer joins, semi joins, BLOB tables, join-buffer plans, and
  `TABLE_REF::disable_cache`;
- verified with focused MTR and a TPCH-derived nested-loop join case.

## Problem

Nested-loop joins can produce repeated outer parameter values. Without a
per-key result cache, each duplicate key causes another handler lookup and
another scan of the matching inner rows.

MariaDB's existing mechanisms do not cover this exact case:

| Mechanism | Covers | Gap for repeated `JT_REF` row batches |
|---|---|---|
| `subquery_cache` | Correlated subquery expression results | Does not cache ordinary join inner table rows. |
| `JOIN_CACHE` / BNL / BKA / BKAH | Batched join execution | Does not memoize and replay one ref-key row batch. |
| `eq_ref` one-record cache | At-most-one-row inner lookups | Does not support multi-row `JT_REF` access. |
| PTRC join cache | Statement-local ref-key row batches | New feature, intentionally narrow in phase 1. |

User-visible impact is repeated handler probes, page accesses, and CPU work
when duplicate outer keys repeatedly read the same inner rows. The target
workloads are join-only plans with high duplicate-key fanout, not general
TPC-H acceleration.

## Goals

- Cache and replay result rows for repeated inner `JT_REF` lookups.
- Keep cache lifetime statement-local.
- Preserve existing nested-loop join semantics by reusing MariaDB's normal row
  evaluation path after restoring a cached row.
- Keep unsupported or risky shapes on the existing execution path.
- Expose basic status counters for validation and diagnosis.
- Provide focused tests and TPCH-derived repeated-NLJ performance evidence.

## Non-Goals

- Leave subquery-result caching to MariaDB's existing `subquery_cache`; do not
  modify that subsystem.
- Do not implement `AccessPath::PARTIAL_RESULT_CACHE`; MariaDB's executor does
  not use the MySQL 8.0 AccessPath/RowIterator pipeline.
- Do not cache across statements, sessions, transactions, or stored procedure
  statements.
- Do not support BNL/BKA/BKAH in the first patch.
- Do not claim broad TPC-H performance improvement or official benchmark
  results.

## User Interface

Optimizer switch:

```sql
SET optimizer_switch='partial_result_cache=on';
SET optimizer_switch='partial_result_cache=off';
```

Default: `off`.

Optimizer hints:

```sql
SELECT /*+ PRC_JOIN(inner_tbl) */ ...
SELECT /*+ NO_PRC_JOIN(inner_tbl) */ ...
SELECT /*+ PRC_JOIN() */ ...
SELECT /*+ NO_PRC_JOIN() */ ...
```

`PRC_JOIN` can force consideration of a safe nested-loop ref table even when
the switch is off or the estimated hit ratio is below
`rds_partial_result_cache_cost_threshold`. It cannot bypass safety checks such
as BLOB rows, outer/semi join inner tables, join-buffer plans, or a zero memory
limit. `NO_PRC_JOIN` disables PTRC for the matched table or query block.

Session variables:

| Variable | Default | Meaning |
|---|---:|---|
| `rds_partial_result_cache_max_mem_size` | `16777216` | Maximum per-statement cache memory for this prototype. `0` disables eligibility. |
| `rds_partial_result_cache_cost_threshold` | `0.5` | Minimum estimated hit ratio required before installing the cache. `0` allows every otherwise safe candidate. |
| `rds_partial_result_cache_min_hit_ratio` | `0.2` | Minimum runtime hit ratio before cache maintenance enters bypass mode. |
| `rds_partial_result_cache_hit_ratio_frequency` | `200` | Miss-count interval for checking the runtime hit ratio. |

Status variables:

| Status | Meaning |
|---|---|
| `Partial_result_cache_hit` | Number of cache-key hits. |
| `Partial_result_cache_miss` | Number of cache-key misses. |
| `Partial_result_cache_rows_cached` | Number of inner rows copied into cache entries. |
| `Partial_result_cache_rows_replayed` | Number of cached rows restored into table buffers. |
| `Partial_result_cache_bypass` | Number of times a cache instance entered bypass mode. |

The status variables are global cumulative counters, matching MariaDB's
existing `Subquery_cache_hit` / `Subquery_cache_miss` pattern. Performance
reports should use deltas between snapshots.

Plan visibility:

- Traditional `EXPLAIN` adds `Using partial result cache` to the inner ref
  table when PTRC is selected.
- `EXPLAIN FORMAT=JSON` adds `using_partial_result_cache`,
  `partial_result_cache_estimated_hit_ratio`, and
  `partial_result_cache_estimated_saved_cost`.
- `ANALYZE FORMAT=JSON` adds per-table runtime counters for selected PTRC
  inner ref tables: `r_partial_result_cache_hits`,
  `r_partial_result_cache_misses`,
  `r_partial_result_cache_rows_cached`,
  `r_partial_result_cache_rows_replayed`,
  `r_partial_result_cache_bypass`, and
  `r_partial_result_cache_mem_used`.
- Optimizer trace adds a `partial_result_cache` object per considered join tab
  with the table, `chosen`, `cause`, `estimated_hit_ratio`, and
  `estimated_saved_cost`.

## Supported Plan Shape

The first patch supports ordinary nested-loop joins where an inner table uses
`JT_REF` and no join buffer is selected:

```text
outer table(s)
  -> inner table, access_type=ref
```

The cache key is built from `JOIN_TAB::ref.key_buff` after
`cp_buffer_from_ref()`. The cache value is the full set of matching rows copied
from `TABLE::record[0]` for that key.

On a hit, cached rows are copied back into `TABLE::record[0]`. MariaDB then
continues through the existing nested-loop evaluator, so attached conditions,
grouping, aggregation, and upper join work use the same code path as a handler
read.

## Eligibility

The prototype rejects caching when any of the following is true:

- `optimizer_switch.partial_result_cache` is off and no matching `PRC_JOIN`
  hint is present;
- a matching `NO_PRC_JOIN` hint is present;
- `rds_partial_result_cache_max_mem_size` is zero;
- the table is the first non-const join table;
- the access type is not `JT_REF`;
- a MariaDB join buffer is selected for the table;
- the table belongs to an outer join or semi join inner nest;
- `TABLE_REF::disable_cache` is true;
- the ref access is trigger-guarded;
- the ref key length is zero;
- the table has BLOB fields;
- the table lock type is stronger than `TL_READ_HIGH_PRIORITY`.

These restrictions are intentional. They prioritize correctness and keep the
first community patch small enough to review.

## Memory And Lifetime

Each eligible `JOIN_TAB` owns a `Partial_result_cache` object. The object lives
only for the statement and is released from `JOIN_TAB::cleanup()`.

The cache stores:

- one hash entry per key;
- a negative marker for no-match keys;
- a linked list of copied `record[0]` rows for matched keys;
- local hit/miss/row counters merged into global status on destruction.

The implementation uses explicit `my_malloc()` / `my_free()` allocation instead
of STL containers so runtime allocation failure can disable the cache and
continue on the normal executor path. If the configured memory cap is exceeded
or the runtime hit ratio falls below `rds_partial_result_cache_min_hit_ratio`
at a configured miss-count checkpoint, the cache clears its contents and enters
bypass mode for the remaining statement execution. Bypass delegates to the
original `join_read_always_key()` and `join_read_next_same()` behavior.

## Correctness Model

Correctness is based on conservative eligibility and fallback:

- On miss, the original handler ref lookup is used.
- On hit, only the inner table record buffer is restored; normal join condition
  evaluation still runs.
- Existing ICP-sensitive cases are rejected through `TABLE_REF::disable_cache`.
- Outer join and semi-join state machines are rejected until they receive
  dedicated review.
- BLOB tables are rejected because raw `record[0]` copying is not sufficient
  for pointer-owned BLOB payloads.
- Locking reads above `TL_READ_HIGH_PRIORITY` are rejected to avoid row-lock
  lifetime changes.

## Performance Model

The cache helps when:

- the same ref key appears many times;
- the inner ref lookup returns multiple rows or touches many pages;
- the cached row batch fits in the memory cap;
- replaying copied rows is cheaper than repeated handler probes.

The cache can be neutral or slower when keys are mostly unique, the inner path
is cheap, or the memory cap is too small. The first cost model uses the chosen
plan's estimated probe count and ref read cost to compute a conservative
estimated hit ratio and saved cost. Candidates below
`rds_partial_result_cache_cost_threshold` are rejected with optimizer trace
cause `cost below threshold`.

## Current Limitations

- No BNL/BKA/BKAH integration.
- No BLOB table support.
- No outer join or semi-join support.
- No LRU eviction; memory overflow clears the cache and disables it for the
  rest of the statement.
- No debug fault-injection test for storage-engine error paths.
- Costing is intentionally minimal; it is visible in EXPLAIN JSON and trace,
  but it does not yet use column histograms or a precise outer-key distinct
  estimate.

The `rds_partial_result_cache_*` variable prefix is retained in this prototype
branch. Before a final MariaDB community PR, decide whether to rename these
variables to MariaDB-neutral `partial_result_cache_*` names.

Correlated subquery result caching is not listed as a gap for this patch.
MariaDB already provides `subquery_cache`; this feature intentionally targets
ordinary join inner row-batch replay instead.

## Benchmark Acceptance Plan

Before using performance numbers as a community-facing claim, regenerate raw
logs with `Docs/partial_result_cache/benchmarks/run_ptrc_benchmark.sh`:

- release build only;
- TPCH-derived repeated-NLJ case for performance evidence;
- optional standard TPC-H Q17 run only as a switch-matrix sanity check;
- `subquery_cache` on/off crossed with `partial_result_cache` on/off;
- one or more warmup rounds and at least five measured rounds;
- result hashes, `EXPLAIN`, `EXPLAIN FORMAT=JSON`,
  `ANALYZE FORMAT=JSON`, optimizer trace, and status deltas.

These limitations are acceptable for an initial reviewable patch, and are
listed as follow-up work rather than hidden behavior.
