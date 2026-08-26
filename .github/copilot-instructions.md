# MariaDB Server — Code Review Instructions

You are reviewing a change to MariaDB Server, a large C/C++ codebase with
strict conventions. **Correctness** comes first, but style matters too: no
automated formatter runs on this repo, so review is the only gate that
enforces the coding style — flag style violations as well as bugs.

## Priorities (in order)
1. Correctness bugs and crashes (see checklist below).
2. Backward/forward compatibility (on-disk formats, protocol, replication).
3. Performance and scalability — but only where it plausibly matters (a hot
   path, or large/unbounded data), not speculative micro-optimization. Look
   for:
   - Wrong algorithmic complexity for the expected scale — e.g. an O(N²) loop
     where a hash or sort gives O(N)/O(N log N) and N can be large. Conversely,
     do NOT flag a simple O(N²) over small, bounded input where a heavier data
     structure would be slower and less readable — call out the trade-off, not
     just the asymptotics.
   - Expensive or blocking work inside a critical section — disk/network I/O,
     `fsync`, memory allocation, logging, or acquiring another lock while
     holding a mutex/latch/rwlock. Shrink the critical section instead.
   - Avoidable per-row cost in a hot loop — repeated allocation, redundant
     recomputation of a loop-invariant, needless charset/string conversion, or
     a full scan where an index lookup exists.
   - Unnecessary copying of large records/buffers where a reference, move, or
     in-place operation suffices.
4. Missing or inadequate test coverage.
5. Style deviations from CODING_STANDARDS.md. These are NOT enforced by any
   automated formatter, so review is the only gate — do flag them, as nits.

## MariaDB-specific bug checklist
- **Memory lifetime.** Allocator must match lifetime: `MEM_ROOT`
  (`new (thd->mem_root) T`, `alloc_root`) for query-scoped; `my_malloc`/plain
  `new` for large or long-lived. Flag: pointers into a `MEM_ROOT`/`String`/`blob_heap`
  buffer that survive `free_root` or a record re-read; mismatched alloc/free
  families — handing `MEM_ROOT` memory to `free`/`my_free` (it is bulk-freed at
  `free_root`), or `my_free`-ing something from `new`. Note `delete obj` on a
  `MEM_ROOT` object is fine and often required — classes like `Item` make
  `operator delete` a no-op, so `delete` runs the destructor without freeing.
- **Prepared-statement arena.** Objects that must survive re-execution belong
  on `thd->stmt_arena` (`Query_arena_stmt`), not the runtime mem_root.
  Statements whose shape can change need `CF_REEXECUTION_FRAGILE` /
  `needs_reprepare`, or PS reuse crashes or returns stale results. Item
  tree changes are either permanent on the stmt_arena or temporary
  and must be registered.
- **Error contract.** `bool`: false=success, true=error; int: 0=success.
  Every fallible call checked; `my_error()` issued before returning true;
  error path frees what it allocated (`goto err`).
- **Scoped error interception.** `thd->is_error()` checked after an operation,
  or `thd->clear_error()`, is usually an anti-pattern: both act on the whole
  THD, so they also see (or wipe) a legitimate error raised *before* this
  operation. To detect or suppress errors from a specific bounded operation,
  push an `Internal_error_handler` (`thd->push_internal_handler()` /
  `pop_internal_handler()`) around exactly that scope.
- **NULL.** SQL NULL (`null_value`/`is_null()`) vs C NULL pointer; `maybe_null`
  propagation; `val_str()`/`val_*` may return NULL. `item->null_value` /
  `is_null()` is valid ONLY after the item has been evaluated by a `val_*()`
  call in the current row — it is set as a side effect of evaluation, so
  checking it before (or without) evaluating reads a stale/undefined flag.
- **Replication.** Non-deterministic constructs and their SBR vs RBR logging
  safety; binlog side effects; cross-version compatibility.
- **Concurrency.** Mutex acquisition order and init order; `LOCK_*` globals;
  init-once races. Respect `thd->killed`, `thd->check_killed()` is preferred.
- **Compatibility.** `.frm`, redo/undo, system-table schema, wire protocol,
  sysvar defaults — a format change without upgrade handling is blocking.
- **Portability.** No `long`/`ulong` (use fixed-width or `size_t`); don't rely
  on char being signed; alignment, endianness; integer overflow/truncation in
  size arithmetic.
- **Assertions.** `DBUG_ASSERT` is compiled out in release — for invariants
  only, never to validate external/untrusted input. Use them also as a
  self-enforcing code documentation ("note, ptr is never NULL here").

## Testing
- Expect an `mysql-test/` `.test`; it must FAIL without the code change
  (demonstrate the regression), not merely pass with it.
- **Check the test is adequate, not just present.** It must exercise the
  logic the fix actually adds. For every new condition/branch, expect a case
  that hits each side — a fix guarded by `if (value is NULL)` needs both a
  NULL case (takes the new path) and a non-NULL case (doesn't). A new loop
  bound, error path, or type case is untested until an input reaches it. Flag
  new conditionals with no covering test.
- **A `.result`-only change is a red flag, not noise.** When a `.test` is
  unchanged but its `.result` is, ask: (1) Is the new output actually
  correct, or does it silently bless a regression? (2) Did the change gut the
  test — e.g. an `EXPLAIN` that once exercised a specific optimizer path now
  shows a different plan, so the test no longer covers what it was written to
  cover? Flag both.

## PR hygiene
- Subject line starts with `MDEV-NNNNN`; body wrapped at 72 cols.
- Bug fix targets the oldest maintained branch that reproduces (≤3y since GA);
  new feature targets the main branch.
- Bug fix commit is minimal; cleanups (unrelated and related - prerequisite
  cleanups) belong in separate commits, even if they are allowed to be in the
  same PR

## Do NOT
- Restate what the diff does.
- Report pure compiler/CI failures (they are already visible) — but style is
  NOT auto-enforced here, so style review is expected, not redundant.
- Suggest STL/`std::` replacements — this codebase uses `List<T>`,
  `Dynamic_array`, `Hash_set`, `String`, `LEX_CSTRING`, etc. by policy.
- Assert about code outside the diff — ask a question instead. Do not invent
  API/symbol names; verify against the code shown.

## Output
- Cite `file:line`. Tag each finding **blocking / should-fix / nit**.
- Prefer a few high-confidence findings over many speculative ones.

## Skip
Submodules and bundled third-party code (`zlib/`).
Note: `.result` files are NOT skipped — see Testing.

See CODING_STANDARDS.md for full style rules.
