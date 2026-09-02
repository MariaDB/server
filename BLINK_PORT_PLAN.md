# MariaDB B-link Tree Port Plan

## Scope

Reference implementation:

- MySQL repository: `/git/mysql-server`
- MySQL commit: `f22525583a694799964db2a1a2a44904a06cf00f`

MariaDB port:

- Repository: `/git/mdb-13`
- Branch: `poc/blink-tree-mariadb-2`
- Base commit: `bab03b0fc44e7aeac869b20b0055c6a8f2083eb6`

The implementation must be a systematic port of the MySQL B-link patch. Each phase must preserve the upstream latch, MTR, failure-boundary, and recovery contracts while adapting them to MariaDB APIs and page layout.

## Confirmed design decisions

### Index format marker

The persistent B-link format marker is stored only at index level:

```text
DICT_BLINK
```

`DICT_BLINK`:

- is persisted in the MariaDB dictionary;
- is restored when the index is opened;
- determines runtime B-link dispatch;
- is independent of the current sysvar value;
- cannot be disabled after high-key records have appeared.

There is no `PAGE_IS_BLINK` flag.

### Recovery marker

The incomplete-split state is encoded as an unused value in the low three bits of `PAGE_DIRECTION_B`:

```cpp
constexpr byte PAGE_LEFT             = 1;
constexpr byte PAGE_RIGHT            = 2;
constexpr byte PAGE_SAME_REC         = 3;
constexpr byte PAGE_SAME_PAGE        = 4;
constexpr byte PAGE_NO_DIRECTION     = 5;
constexpr byte PAGE_INCOMPLETE_SPLIT = 6;
```

Value `7` remains reserved.

The high five bits of `PAGE_DIRECTION_B` must always be preserved because they participate in MariaDB instant-column metadata.

### High-key representation

A record is a structural high key only when all conditions hold:

```text
index has DICT_BLINK
page.FIL_PAGE_NEXT != FIL_NULL
record is immediately before supremum
record status is REC_STATUS_NODE_PTR
child-page field is FIL_NULL
```

### Existing indexes

An existing multi-page B-tree is never converted in place.

Supported transitions are:

- a new physical index created as B-link from the beginning;
- atomic cutover of an index whose height is zero;
- a new physical tree created by COPY rebuild.

## Phase 0: Port matrix

For every function in the MySQL patch, record:

| Field | Meaning |
|---|---|
| MySQL source | File and line range |
| MariaDB target | Target file and function |
| Port type | Exact port, API adaptation, redesign, or not applicable |
| Latches | Entry and exit latch contract |
| MTR | Boundaries and ownership |
| Failure boundary | Permitted failures before and after structural mutation |
| Format | Persistent page or dictionary changes |
| Test | Corresponding upstream test |

Classify these groups before implementing them:

```text
metadata
page primitives
cursor operations
descent
MTR transfer
fit predicate
split
root raise
cascade
allocator
row integration
recovery
lifecycle
tests
```

**Exit criterion:** the relevant upstream implementation and every MariaDB-specific difference are understood before code is written.

## Phase 1: Persistent `DICT_BLINK`

Add and persist `DICT_BLINK`.

Support:

- dictionary write during index creation;
- dictionary read during index open;
- restart persistence;
- stamp preservation during TRUNCATE;
- unsupported-format validation;
- AHI disablement;
- merge disablement.

Runtime dispatch:

```cpp
bool use_blink_path(const dict_index_t *index)
{
  return (index->type & DICT_BLINK) && blink_index_shape_ok(index);
}
```

Do not add a runtime switch such as `innodb_blink_core_enabled`. The `innodb_blink_enabled` sysvar affects only stamping of new physical indexes.

Initial eligibility restrictions:

```text
ROW_FORMAT=COMPACT or DYNAMIC
not ROW_FORMAT=COMPRESSED
not TEMPORARY
not a system table
not FTS
not SPATIAL
not an online-DDL target
```

Instant columns remain unsupported until their metadata interactions are tested explicitly.

DDL behavior:

| Operation | Behavior |
|---|---|
| CREATE TABLE | Stamp supported new indexes |
| CREATE INDEX | Stamp only the newly created physical index when safe |
| Existing multi-page index | Never convert in place |
| Height-zero index | Allow atomic cutover |
| In-place rebuild of B-link table | Refuse and request COPY |
| COPY rebuild | New tree may be B-link |
| TRUNCATE | Preserve the previous stamp |
| IMPORT | Initially reject B-link tablespaces |

**Exit criteria:** restart preserves the stamp and toggling the sysvar never changes runtime interpretation of an existing index.

## Phase 2: `PAGE_DIRECTION_B` recovery state

Define:

```cpp
constexpr byte PAGE_INCOMPLETE_SPLIT = 6;
```

Read the marker through the existing low-three-bit direction accessor:

```cpp
bool page_has_incomplete_split(const page_t *page)
{
  return page_get_direction(page) == PAGE_INCOMPLETE_SPLIT;
}
```

Set the marker while preserving the instant-column bits:

```text
new value = (old value & 0xf8) | PAGE_INCOMPLETE_SPLIT
PAGE_N_DIRECTION = 0
```

Clear it as:

```text
new value = (old value & 0xf8) | PAGE_NO_DIRECTION
PAGE_N_DIRECTION = 0
```

All insert-direction update paths must leave direction value `6` unchanged. Ordinary direction accounting resumes only after parent installation clears the marker.

Page reorganization must:

1. remember whether the source page has direction `6`;
2. recreate and copy the page;
3. restore direction `6` before commit.

Validation must permit value `6` only when:

```text
index has DICT_BLINK
page is a valid non-rightmost page
page has a valid terminal high key
```

Value `6` on an ordinary index is corruption.

**Exit criteria:** the marker survives normal inserts, page reorganization, redo, and restart without changing the high instant-metadata bits.

## Phase 3: Structural high-key primitives

Port and adapt:

```text
blink_make_high_key_tuple()
blink_write_high_key_record()
blink_read_high_key_record()
blink_delete_high_key_record()
rec_is_high_key_structural()
```

The high-key predicate must accept `dict_index_t` and validate the `FIL_NULL` child sentinel. A position-only predicate is insufficient.

Audit MariaDB primitives that assume a node-pointer record cannot exist on a leaf page, including:

- `page_rec_get_prev_const()`;
- record-offset calculation;
- page validation;
- page directory walking;
- page copy and delete;
- page reorganization.

Required page invariants:

```text
rightmost page:
  FIL_PAGE_NEXT == FIL_NULL
  no high key

non-rightmost B-link page:
  FIL_PAGE_NEXT != FIL_NULL
  valid terminal high key

incomplete page:
  PAGE_DIRECTION_B low bits == 6
  valid terminal high key
```

**Exit criteria:** high keys survive reorganization and are never interpreted as user records.

## Phase 4: Cursor, validation, and statistics

Adapt every record traversal path to skip structural high keys:

- persistent cursor next and previous;
- cursor save and restore;
- row selection;
- purge;
- rollback;
- statistics sampling;
- `CHECK TABLE`;
- range estimation;
- online row logging;
- page validation.

A high key on a leaf page must never be decoded as:

```text
DB_TRX_ID
DB_ROLL_PTR
user columns
delete-marked user record
```

A high key on an internal page must never be followed as a child pointer.

**Exit criteria:** forward and backward scans, `COUNT(*)`, statistics, purge, rollback, and `CHECK TABLE` operate without returning high-key records or reporting false corruption.

## Phase 5: MTR latch transfer

Port and test:

```cpp
mtr_t::transfer_to()
```

Required contract:

```text
MTR1 owns S(index)
MTR2 is active
ownership moves MTR1 -> MTR2
MTR1 commit does not release S(index)
MTR2 commit releases S(index) exactly once
```

Tests must cover:

- S-latch transfer;
- source commit;
- destination commit;
- incorrect object or memo type;
- double-unlock prevention;
- reverse release ordering;
- active destination requirement.

**Exit criterion:** isolated MTR tests pass before cascade uses latch transfer.

## Phase 6: Non-coupled B-link descent

Port close to the MySQL implementation:

```text
blink_move_right_if_needed()
blink_child_identity_check()
blink_search_to_nth_level()
```

Latch protocol:

```text
S(index)
S(parent)
find child
release parent
S(child), or X(child) at the target level
```

Right-link correction must acquire the right sibling before releasing the current sibling.

Required behavior:

- arbitrary target level;
- `BTR_ALREADY_S_LATCHED`;
- root S-to-X upgrade;
- restart when root level changes;
- `PAGE_CUR_GE` to `PAGE_CUR_L` conversion on internal levels;
- `PAGE_CUR_G` to `PAGE_CUR_LE` conversion on internal levels;
- infimum handling;
- page type, index ID, and level validation;
- rejection of empty or high-key-only internal pages;
- malformed or absent required high key reported as corruption;
- no AHI path for a B-link index.

**Exit criteria:** descent, infimum, stale-parent/right-move, and concurrent root-change tests pass.

## Phase 7: Asynchronous allocator foundation

Implement before structural split code:

- per-index leaf pool;
- per-index non-leaf pool;
- global registry;
- index pin and reference count;
- background preallocator;
- low/high watermarks;
- refill hysteresis;
- round-robin refill;
- startup and shutdown integration;
- deferred page reclaim.

MariaDB allocation order:

```text
try S(index)
SX(root)
X(space)
btr_page_alloc()
btr_page_create()
commit
```

Never allow:

```text
X(space) -> wait for SX(root)
```

The DML path only pops pages from the pool. It never performs synchronous FSP allocation.

Empty pool returns `DB_BLINK_RETRY_POOL_EMPTY` before lock/undo and before structural mutation.

**Exit criteria:** pool fill, pop, retry, DROP, reclaim, and shutdown tests pass.

## Phase 8: Split fit predicate

Port:

```text
blink_max_high_key_record_size()
blink_page_insert_fits()
blink_split_choose_and_check_fit()
```

Before mutation prove that:

- the tuple fits;
- the new high key fits;
- the old high key fits on the new sibling;
- both page halves remain structurally valid;
- the selected split record is not a high key;
- internal pages retain real node pointers;
- append optimization is safe;
- page reorganization will provide sufficient space.

If the predicate rejects the operation:

```text
page bytes remain unchanged
no redo is generated
no incomplete marker is set
```

**Exit criterion:** upstream fit-predicate tests pass.

## Phase 9: Common leaf/internal split

Port a single implementation:

```cpp
blink_split_page_and_insert()
```

Do not maintain separate leaf and internal split algorithms.

Required MTR order:

```text
X(left)
X(new sibling)
X(old right sibling), if present
copy old high key
move records
publish prev/next links
write high key on left
restore high key on new sibling
update old-right.prev
insert tuple
set PAGE_DIRECTION_B = 6 on left
commit
```

Set direction `6` after operations that update insert-direction hints.

After structural mutation begins, normal recoverable error returns are forbidden. Any unexpected failure after this boundary is an invariant failure with durable recovery debt.

**Exit criteria:** first, append, middle, already-B-link, and internal splits pass, including old-high-key preservation and validation of both resulting pages.

## Phase 10: Root raise

Port:

```text
blink_root_raise_low()
blink_root_raise_and_insert()
```

Latch contract:

```text
S(index)
X(root)
X(preallocated old-root page)
X(preallocated sibling)
```

Normal root raise does not acquire `X(index)`.

Root invariants:

```text
no siblings
no high key
no persistent PAGE_INCOMPLETE_SPLIT state
```

Root raise is single-MTR atomic and must preserve:

- FSEG headers;
- root page number;
- `PAGE_ROOT_AUTO_INC`;
- instant metadata;
- record-lock state;
- AHI state;
- `REC_INFO_MIN_REC_FLAG`.

**Exit criteria:** leaf-root, internal-root, cascade-driven, and concurrent root raises pass, followed by restart and `CHECK TABLE`.

## Phase 11: Correct pessimistic-insert boundary

Port the MySQL operation order:

```text
check incomplete state on target
pre-pop the full cascade page budget
record-lock check
undo logging
big-record conversion
X-latch all required pool pages
leaf split or root raise
lock_update_insert
PAGE_MAX_TRX_ID update
leaf MTR commit
parent cascade
```

Before mutation, obtain:

```text
one leaf sibling
tree_height + slack non-leaf pages
additional pages required by root raise
```

If pages are unavailable, return `DB_BLINK_RETRY_POOL_EMPTY` while the tree remains unchanged.

**Exit criteria:** lock wait, empty pool, big record, rollback, and page-leak tests pass.

## Phase 12: Parent cascade

Port:

```cpp
blink_insert_into_level()
```

Per-level protocol:

```text
transfer S(index)
re-descend to parent level
X(parent)
if parent direction == 6:
  release, back off, and retry
attempt direct node-pointer insertion
reorganize and retry
if parent is full, perform an internal B-link split
clear the previous child marker in the same MTR
build the next separator
transfer S(index)
continue at level + 1
```

Required safeguards:

- idempotent lookup by child page number;
- `left.next == right` validation;
- `right.prev == left` validation;
- deep copy of separator data;
- interruption handling;
- bounded pause/yield/sleep backoff;
- pending completion queue.

**Exit criteria:** parent-with-space, parent split, recursive cascade, concurrent same-parent cascades, incomplete-parent retry, duplicate completion, and root cascade pass.

## Phase 13: Recovery and runtime completion

Implement an idempotent finisher based on:

```cpp
blink_finish_incomplete_split()
```

A recovery candidate is:

```text
index has DICT_BLINK
PAGE_DIRECTION_B low bits == 6
terminal high key is valid
right sibling is valid
```

The finisher must:

1. find the index;
2. validate left and right page identity;
3. reconstruct the separator;
4. detect an already-installed parent pointer;
5. install a missing parent pointer idempotently;
6. continue a recursive cascade;
7. clear direction from `6` to `PAGE_NO_DIRECTION`.

Runtime completion queues splits abandoned after leaf-MTR commit.

Crash injection points:

- after leaf split commit;
- before parent install;
- after parent pointer and before marker clear;
- after internal split;
- during root cascade.

**Exit criteria:** restart leaves no page with direction `6`, row count is correct, and `CHECK TABLE` succeeds.

## Phase 14: DDL and lifecycle

Implement or explicitly reject:

- CREATE TABLE;
- CREATE INDEX;
- height-zero cutover;
- COPY rebuild;
- in-place rebuild;
- TRUNCATE;
- DROP;
- DISCARD;
- IMPORT;
- online DDL;
- instant columns;
- compressed indexes.

All decisions use persistent `DICT_BLINK`.

Height-zero cutover under `X(index)`:

```text
verify height == 0
drop AHI entries
disable AHI
set DICT_BLINK
persist dictionary property
register allocator pool
```

No physical root-page conversion is needed.

## Phase 15: Tests

Port at least:

```text
blink_create_index_stamp
blink_descent_smoke
blink_descent_infimum_smoke
blink_optimistic_insert_smoke
blink_fit_predicate_smoke
blink_split_smoke
blink_split_append_hint
blink_split_recursive_smoke
blink_root_raise_smoke
blink_cascade_visibility
blink_cascade_nodeptr_stability
blink_cascade_root_raise
blink_is_on_target_retry
blink_pool_empty_retry
blink_pool_warmup
blink_recovery_finalize
blink_recovery_preflushed
blink_purge_empty_leaf
blink_check_table
blink_truncate_stamp
blink_discard_import
blink_reject_online_rebuild
```

Add MariaDB-specific tests:

```text
PAGE_DIRECTION_B high instant bits are preserved
direction 6 survives page reorganization
normal insert does not overwrite direction 6
direction 6 is rejected on a non-B-link index
root never retains direction 6
```

Each implementation phase includes its own tests instead of deferring all tests to the end.

## Phase 16: Functional workload

Run in this order:

```text
200 preload + 32 measured inserts, 2 threads
20k preload + 3.2k measured inserts, 2 threads
20k preload + 3.2k measured inserts, 4/8/16/32 threads
restart
COUNT(*)
CHECK TABLE
forward and backward scans
```

Admission criteria for performance testing:

```text
normal-path X(index) delta = 0
pool refill during measured phase = 0
pool-empty retries are bounded
leaf splits equal completed leaf cascades
no pages retain direction 6 after the run
row count is correct
CHECK TABLE succeeds
```

## Phase 17: Proposal-scale benchmark

Run only after all correctness gates pass:

```text
preload:  2,500,000 rows
measured:   400,000 inserts
payload:       2,500 bytes
threads:  1, 2, 4, 8, 16, 32
rounds:   at least 5
```

Restore the same prepared snapshot before every round.

Collect:

- TPS;
- average, p95, p99, and maximum latency;
- leaf, internal, and root splits;
- right moves;
- cascade depth;
- incomplete-split retries;
- pool retries and refills;
- redo volume;
- I/O counters;
- index-tree latch waits;
- `X(index)` acquisitions;
- row count;
- `CHECK TABLE` result.

## Recommended implementation commits

```text
1. DICT_BLINK metadata and DDL stamping tests
2. PAGE_DIRECTION_B state 6 helpers and tests
3. structural high-key primitives and page tests
4. cursor, validation, and statistics high-key support
5. MTR transfer and unit tests
6. non-coupled descent and tests
7. allocator pool and retry tests
8. fit predicate and tests
9. common leaf/internal split and tests
10. root raise and tests
11. pessimistic insert ordering
12. parent cascade and concurrency tests
13. recovery and runtime finisher
14. DDL and lifecycle restrictions
15. functional and performance verification
```

## Benchmark scripts

The split-heavy workload is stored in:

- `scripts/blink_split_heavy.lua`;
- `scripts/run_blink_split_heavy.sh`.

The runner was copied from the previous experimental branch and still contains the old `innodb_blink_core_enabled` workflow. That mechanism must be removed or replaced before performance results are considered valid, because persistent `DICT_BLINK` must be the sole runtime format selector.
