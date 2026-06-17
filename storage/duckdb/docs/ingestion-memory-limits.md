# Limiting DuckDB RAM During Ingestion

This document records what DuckDB settings exist to constrain memory consumption
during data ingestion (large `COPY` / `INSERT ... SELECT` loads, e.g. via
`run_in_duckdb()`), and the one configuration gotcha that most often causes
apparently unbounded RAM growth.

> **Summary:** DuckDB has **no ingestion-specific memory budget**. The only hard
> ceiling is the instance-wide `memory_limit` (alias `max_memory`), and it can
> only be enforced if DuckDB has spill space (a `temp_directory`). Without a temp
> directory, DuckDB **explicitly cannot limit memory** and will grow unbounded.

## 1. Likely root cause: no spill space

DuckDB can only honor `memory_limit` for memory-intensive operators if it can
offload to a temporary directory. The temporary-memory manager is explicit:

```@/git/mdb-w-duckdb/storage/duckdb/third_parties/duckdb/src/storage/temporary_memory_manager.cpp:134-136
	} else if (!has_temporary_directory) {
		// We cannot offload, so we cannot limit memory usage. Set reservation equal to the remaining size
		SetReservation(temporary_memory_state, temporary_memory_state.GetRemainingSize());
```

Consequences:

- If the shared instance has **no `temp_directory`** (or no swap space), the
  memory limit cannot be enforced during heavy ingestion.
- `memory_limit` set to `-1` / `none` / `null` means **infinite**
  (`DBConfig::ParseMemoryLimit`, `config.cpp:633-637`).

## 2. Settings that affect ingestion memory

All are real, in-tree settings (registered in `config.cpp`, defined in
`settings.hpp`):

- **`write_buffer_row_group_count`** (GLOBAL, default `5`) — the most directly
  relevant. Its description: *"The amount of row groups to buffer in bulk
  ingestion prior to flushing them together. Reducing this setting can reduce
  memory consumption."* (`settings.hpp:1609-1618`). Closest thing to an
  ingestion-specific RAM control.
- **`memory_limit` / `max_memory`** (GLOBAL) — the instance-wide ceiling
  (`MaxMemorySetting`, `custom_settings.cpp:1295-1309`). Enforceable only with
  spill space (see section 1).
- **`temp_directory`** + **`max_temp_directory_size`** (GLOBAL) — set these so
  the limit can be enforced by spilling
  (`custom_settings.cpp:1314-1358`, `1606-1633`).
- **`preserve_insertion_order`** (GLOBAL, default `true`) — when `false`, the
  planner enables parallel/streaming insert and may reorder, reducing buffering.
  Gated in `PlanInsert` via
  `parallel_streaming_insert = !PreserveInsertionOrder(...)`
  (`plan_insert.cpp:102`).

For `COPY ... TO ... (PARTITION_BY ...)` (writing partitioned files):

- **`partitioned_write_flush_threshold`** (default `524288` rows) — flush a
  thread's partition buffer sooner (`settings.hpp:1257-1266`; used in
  `physical_copy_to_file.cpp:287,317`).
- **`partitioned_write_max_open_files`** (default `100`) — fewer simultaneously
  open files = less buffering (`settings.hpp:1268-1277`).

Not relevant: `streaming_buffer_size` controls only result streaming back to the
client, not ingestion (`client_config.hpp:82-83`).

## 3. Caveat: single shared instance

These are **GLOBAL** settings on the **single shared DuckDB instance** (the same
instance `run_in_duckdb()` uses — see `security-model.md`). A `SET GLOBAL ...`
issued through `run_in_duckdb()` changes behavior for **every** session; there is
no per-call or per-tenant memory isolation, and a caller can also raise
`memory_limit` or set it to unlimited.

## References

- `src/storage/temporary_memory_manager.cpp` — memory cannot be limited without
  a temp directory
- `src/main/config.cpp`, `src/main/settings/custom_settings.cpp`,
  `src/include/duckdb/main/settings.hpp` — setting definitions
- `src/execution/physical_plan/plan_insert.cpp` — insertion-order vs. parallel
  streaming insert
- `src/execution/operator/persistent/physical_copy_to_file.cpp` — partitioned
  write buffering
- `docs/security-model.md` — shared-instance / `run_in_duckdb()` context
