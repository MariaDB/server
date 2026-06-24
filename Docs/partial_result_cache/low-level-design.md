# Partial Result Cache for MariaDB Nested Loop Joins - Low Level Design

## Source Layout

Product source changes:

- `sql/sql_priv.h`
  - Adds `OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE`.
- `sql/sql_class.h`
  - Adds session variables under `system_variables`.
- `sql/sys_vars.cc`
  - Registers `optimizer_switch=partial_result_cache`.
  - Registers `rds_partial_result_cache_*` variables.
- `sql/sql_select.h`
  - Adds `Partial_result_cache` forward declaration.
  - Adds `JOIN_TAB::partial_result_cache_eligible`.
  - Adds `JOIN_TAB::partial_result_cache`.
- `sql/sql_select.cc`
  - Calls the PTRC setup hook after the normal access method is selected.
  - Frees cache state from `JOIN_TAB::cleanup()`.
- `sql/partial_result_cache.h`
  - Declares status counters and PTRC setup/cleanup interfaces.
- `sql/partial_result_cache.cc`
  - Implements eligibility, optimizer trace, runtime bypass, row replay, and
    the cache object.
- `sql/mysqld.cc`
  - Exposes `Partial_result_cache_*` status variables.
  - Initializes counters during server variable initialization.

Tests:

- `mysql-test/suite/sys_vars/t/partial_result_cache_basic.test`
- `mysql-test/suite/sys_vars/r/partial_result_cache_basic.result`
- `mysql-test/suite/sys_vars/r/optimizer_switch_basic.result`
- `mysql-test/main/partial_result_cache_join.test`
- `mysql-test/main/partial_result_cache_join.result`

Docs:

- `Docs/partial_result_cache/*`

## Optimizer Switch And Variables

`OPTIMIZER_SWITCH_PARTIAL_RESULT_CACHE` uses bit `1ULL << 40`, following the
last existing optimizer switch bit in this MariaDB tree.

The optimizer switch name is appended to `optimizer_switch_names[]`:

```c++
"partial_result_cache",
```

Four session variables are registered for compatibility with the MySQL PTRC
control surface:

```text
rds_partial_result_cache_max_mem_size
rds_partial_result_cache_cost_threshold
rds_partial_result_cache_min_hit_ratio
rds_partial_result_cache_hit_ratio_frequency
```

`rds_partial_result_cache_max_mem_size` is active in execution.
`rds_partial_result_cache_cost_threshold` is active during plan refinement as
the minimum estimated cache hit ratio. The hit-ratio frequency and minimum
runtime hit-ratio variables are active during execution and can disable cache
maintenance when observed hit ratio is too low.

## Executor Hook

MariaDB's normal nested-loop executor is driven by `JOIN_TAB`:

- `make_join_readinfo()` sets up each table.
- `pick_table_access_method()` chooses table read functions.
- `sub_select()` calls `JOIN_TAB::read_first_record`.
- The same loop calls `JOIN_TAB::read_record.read_record_func`.
- `evaluate_join_record()` handles predicates and upper nested-loop work.

The PRC hook is installed immediately after `pick_table_access_method()`:

```c++
if (setup_partial_result_cache(tab, first_tab, jcl))
  return TRUE;
```

When the table is not eligible, any existing sidecar cache is freed and the
normal access functions remain in place.

## Eligibility Function

`partial_result_cache_is_eligible()` checks:

```c++
optimizer_switch.partial_result_cache
max_mem_size != 0
tab != first_tab
tab->type == JT_REF
join_cache_level == 0
!tab->bush_children
!tab->is_inner_table_of_outer_join()
!tab->is_inner_table_of_semijoin()
!tab->ref.disable_cache
!tab->ref.is_access_triggered()
tab->ref.key_length != 0
table has no BLOB fields
lock_type <= TL_READ_HIGH_PRIORITY
estimated_hit_ratio >= rds_partial_result_cache_cost_threshold
```

`join_cache_level` here is the selected cache level for the table, not the
global variable. This ensures the first patch does not conflict with MariaDB's
existing join buffer machinery.

The cost check estimates repeated probes from
`JOIN_TAB::partial_join_cardinality` and `JOIN_TAB::records_read`, then
multiplies the estimated hit ratio by `JOIN_TAB::join_read_time` for the saved
cost reported in trace and JSON EXPLAIN. A threshold of `0` keeps every
otherwise safe candidate eligible; a threshold of `1` rejects the current
implementation because it cannot prove a perfect hit ratio.

## EXPLAIN And Optimizer Trace

When PTRC is selected, traditional EXPLAIN prints:

```text
Using partial result cache
```

`EXPLAIN FORMAT=JSON` prints:

```json
"using_partial_result_cache": true,
"partial_result_cache_estimated_hit_ratio": ...,
"partial_result_cache_estimated_saved_cost": ...
```

`ANALYZE FORMAT=JSON` also prints per-table runtime counters for PTRC-selected
inner ref tables:

```json
"r_partial_result_cache_hits": ...,
"r_partial_result_cache_misses": ...,
"r_partial_result_cache_rows_cached": ...,
"r_partial_result_cache_rows_replayed": ...,
"r_partial_result_cache_bypass": ...,
"r_partial_result_cache_mem_used": ...
```

Optimizer trace records both chosen and rejected candidates:

```json
"partial_result_cache": {
  "table": "...",
  "chosen": true,
  "cause": "chosen by cost",
  "estimated_hit_ratio": ...,
  "estimated_saved_cost": ...
}
```

Rejected candidates use causes such as `disabled by optimizer_switch`,
`join buffer is selected`, `BLOB row is not supported`, and
`cost below threshold`.

## Cache Object

`Partial_result_cache` is implemented in `sql/partial_result_cache.cc`. It
stores:

```c++
Entry **m_buckets;
size_t m_bucket_count;
size_t m_bucket_bytes;
Entry *m_current_entry;
Cached_row *m_current_row;
size_t m_mem_used;
ulong m_rec_length;
ulong m_hit;
ulong m_miss;
ulong m_rows_cached;
ulong m_rows_replayed;
ulong m_bypass;
bool m_replaying;
bool m_disabled;
```

`Entry` stores:

```c++
Entry *next;
bool found;
uint key_length;
uchar *key;
Cached_row *rows;
Cached_row *last_row;
```

The key is a byte copy of `tab->ref.key_buff` with length
`tab->ref.key_length`. Entries are stored in a fixed bucket array with a simple
byte hash. Rows are stored as linked `Cached_row` nodes allocated with
`my_malloc()` and released with `my_free()`.

The row payload is a byte copy of:

```c++
table->record[0] ... table->record[0] + table->s->reclength
```

Because this is a raw fixed-record copy, tables with BLOB fields are rejected.

## Read Path

### First Row

`join_read_partial_result_cache_key()` calls:

```c++
tab->partial_result_cache->read_first(tab)
```

`read_first()`:

1. Clears current replay state.
2. If disabled, delegates to `join_read_always_key(tab)`.
3. Initializes the handler index if needed.
4. Calls `cp_buffer_from_ref()` to materialize `tab->ref.key_buff`.
5. Calls `prepare_index_key_scan_map()`.
6. Looks up the key in `m_entries`.
7. On hit:
   - increments `m_hit`;
   - marks replay mode;
   - restores the first cached row.
8. On miss:
   - increments `m_miss`;
   - calls `ha_index_read_map()`;
   - stores a negative entry if no row is found;
   - stores the first row if found.

### Next Rows

`join_read_partial_result_cache_next_same()` calls:

```c++
tab->partial_result_cache->read_next(info)
```

`read_next()`:

- if replaying, restores the next cached row;
- otherwise delegates to `join_read_next_same(info)`;
- every successful delegated read is appended to the current cache entry.

When replay reaches the end of a positive entry, `table->status` is set to
`STATUS_GARBAGE` and `-1` is returned, matching normal end-of-ref behavior.

## Memory Accounting And Bypass

The memory cap is read from:

```c++
thd->variables.partial_result_cache_max_mem_size
```

The prototype accounts:

- bucket array bytes;
- key bytes;
- `sizeof(Entry)`;
- copied fixed-row bytes.

If the cap is exceeded:

1. all cache entries are cleared;
2. current replay state is reset;
3. `m_disabled` is set;
4. `m_bypass` is incremented;
5. future reads delegate to normal ref access.

Runtime `my_malloc()` failure follows the same disable-and-continue path. The
bucket array is allocated during setup; a setup-stage allocation failure is
reported as setup failure, matching surrounding executor initialization code.

The runtime hit-ratio bypass checks every
`rds_partial_result_cache_hit_ratio_frequency` misses. If
`hit / (hit + miss)` is lower than
`rds_partial_result_cache_min_hit_ratio`, the cache is cleared and disabled for
the rest of that statement execution.

## Status Counters

The cache object accumulates statement-local counters. During `ANALYZE
FORMAT=JSON`, these counters are mirrored into `Explain_table_access` so the
table node can print `r_partial_result_cache_*` members after execution. The
same counters are merged into global status counters in the cache destructor:

```c++
statistic_add(partial_result_cache_hit, m_hit, &LOCK_status);
statistic_add(partial_result_cache_miss, m_miss, &LOCK_status);
...
```

The counters are exposed through `SHOW STATUS LIKE 'Partial_result_cache%'` and
through the PTRC-selected table node in `ANALYZE FORMAT=JSON`.

## Cleanup

`JOIN_TAB::cleanup()` calls:

```c++
free_partial_result_cache(partial_result_cache);
partial_result_cache= NULL;
```

This makes cache lifetime match the statement execution plan lifetime.

## Error And Fallback Behavior

- Cache bucket allocation failure makes `make_join_readinfo()` return `TRUE`,
  matching the surrounding setup-stage OOM behavior.
- Runtime entry, key, or row allocation failure disables the cache and returns
  to normal ref reads.
- Handler errors from `ha_index_init()`, `prepare_index_key_scan_map()`,
  `ha_index_read_map()`, and `ha_index_next_same()` are reported through the
  existing helper paths.
- Key-not-found and end-of-file are cached as negative entries.
- Memory-limit bypass does not report an error; it returns to normal ref reads.

## Review Notes

The first patch intentionally avoids these areas:

- `JT_REF_OR_NULL`
- `JT_EQ_REF`
- outer join null-complemented rows
- semi-join FirstMatch/LooseScan/Duplicate Weedout
- BKA/BKAH MRR access
- BLOB/TEXT rows
- precise outer-key distinct estimates for costing

Those should be separate reviewable changes after the community accepts the
basic execution hook and correctness model.
