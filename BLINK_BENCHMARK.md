# B-link benchmark record

## Canonical B-link point

The canonical proposal-scale B-link point uses a clean datadir, all 20 online CPUs, a 24 GiB buffer pool, an 8 GiB redo log, and `innodb_flush_log_at_trx_commit=2`.

```text
preload rows                         2,500,000
measured inserts                       400,000
payload bytes                            2,500
threads                                     32
buffer pool                              24 GiB
redo log                                  8 GiB
leaf/internal pool                   450000/512
CPU affinity                              0-19
```

Result artifact:

```text
/tmp/blink-split-heavy-results/blink-proposal-flush2-t32-20260903T090949Z
```

Result:

```text
TPS                                  102,837.80
total time                               3.8886 s
average latency                            0.31 ms
p95 latency                                0.56 ms
maximum latency                          220.07 ms
ignored errors                                0
```

Measured B-link deltas:

```text
optimistic inserts                      400000
leaf splits                             396455
internal splits                              1
parent installs                         396456
cascade levels                          396456
right moves                                  0
incomplete retries                           0
pool-empty retries                           0
pool refills                                 0
normal X(index)                              0
leaf pool depth                    450000 -> 53545
internal pool depth                    512 -> 511
```

Correctness:

```text
row count                              2,900,000
MIN(id)                                1,000,000
MAX(id)                        2,500,000,000,000
forward scan                                  OK
backward scan                                 OK
CHECK TABLE                                   OK
```

Measured I/O and redo:

```text
physical buffer-pool reads                     0
data reads                                     0
data writes                                    0
pages flushed                                  0
buffer-pool wait-free events                   0
redo written                             4.48 GB
log writes                               144,175
fsyncs                                        35
```

The server reported source revision `0f4fa0590289133b3a1fb25c539022671acbd46e`; the repository checkpoint containing the configurable pool and scheduler work is `185ead75656ac8e346000b0f71120b9cff4fdd78`.

## Other B-link points

| Workload | Configuration | TPS | Avg | P95 | Max | Artifact |
|---|---|---:|---:|---:|---:|---|
| 20k/3.2k, t2 | prefilled pool, flush=1 | 3,533.63 | 0.56 ms | 0.63 ms | 28.95 ms | `blink-prefill2-20k-phase16-t2-20260902T153003Z` |
| 20k/3.2k, t32 | prefilled 4096/256, flush=1 | 17,606.31 | 1.76 ms | 4.49 ms | 35.16 ms | `blink-prefill-clean-20k-phase16-t32-20260902T161627Z` |
| 2.5M/400k, t32 | 128 MiB BP, 100 MiB redo, flush=1 | 2,257.12 | 14.18 ms | 27.17 ms | 492.66 ms | `blink-proposal-prefill-t32-20260902T201357Z` |
| 2.5M/400k, t32 | 24 GiB BP, 100 MiB redo, dirty prefill, flush=1 | 2,061.40 | 15.52 ms | 68.05 ms | 2404.92 ms | `blink-proposal-memory-t32-20260902T204619Z` |
| 2.5M/400k, t32 | 24 GiB BP, 8 GiB redo, clean prefill, flush=1 | 8,695.30 | 3.68 ms | 6.43 ms | 423.21 ms | `blink-proposal-memory-redo8g-t32-20260902T213232Z` |
| 2.5M/400k, t32 | 24 GiB BP, 8 GiB redo, clean prefill, flush=2 | 102,837.80 | 0.31 ms | 0.56 ms | 220.07 ms | `blink-proposal-flush2-t32-20260903T090949Z` |

`flush=2` is not durability-equivalent to `flush=1`: an operating-system or power failure can lose approximately the latest second of committed transactions.

## Canonical vanilla point and direct comparison

The matching vanilla run used MariaDB `main` revision `bab03b0fc44e7aeac869b20b0055c6a8f2083eb6`, a clean datadir, all CPUs `0-19`, and the same workload and server settings as the canonical B-link point. The only intrinsic methodology difference was the absence of the B-link page pool and its prefill step.

Vanilla result artifact:

```text
/tmp/blink-split-heavy-results/vanilla-proposal-t32-20260903T093435Z
```

Vanilla result:

```text
TPS                                   19,675.94
total time                               20.3285 s
average latency                             1.63 ms
p95 latency                                 8.28 ms
maximum latency                           401.29 ms
ignored errors                                 0
page splits                               392893
```

Direct comparison:

| Metric | Vanilla | B-link | B-link change |
|---|---:|---:|---:|
| TPS | 19,675.94 | 102,837.80 | 5.23x / +422.7% |
| Total time | 20.3285 s | 3.8886 s | -80.9% |
| Average latency | 1.63 ms | 0.31 ms | -81.0% |
| P95 latency | 8.28 ms | 0.56 ms | -93.2% |
| Maximum latency | 401.29 ms | 220.07 ms | -45.2% |
| Structural splits | 392893 | 396455 leaf + 1 internal | comparable |
| Redo written | 3.76 GB | 4.48 GB | +19.1% |
| Log writes | 398917 | 144175 | -63.9% |
| Data fsyncs | 3277 | 35 | -98.9% |

Both measured phases satisfied the common admission conditions:

```text
physical buffer-pool reads              0
data reads                              0
data writes                             0
pages flushed                           0
buffer-pool wait-free events            0
ignored errors                          0
final row count                 2,900,000
CHECK TABLE                            OK
```

Vanilla preparation measurements:

```text
preload                              28.538 s
dirty pages                    394688 -> 910
dirty drain                         51.386 s
warm scan                             0.687 s
warm physical reads delta                 0
```

B-link preparation measurements recorded for the matching canonical run:

```text
pool prefill                           5.205 s
dirty pages                    839159 -> 743
dirty drain                         61.622 s
warm scan                             0.838 s
warm physical reads delta                 0
```

The B-link preload duration was not instrumented in the canonical run, so total preparation durations must not be compared. The measured-phase comparison is valid because both sides started with zero dirty pages, a resident dataset, the same 24 GiB buffer pool, the same 8 GiB redo log, `innodb_flush_log_at_trx_commit=2`, AHI disabled, no CPU affinity restriction, and no measured data-page I/O.

The observed difference in log writes and fsyncs is substantial, but this record does not assign a root cause without a dedicated redo/latch profile.

## Canonical B-link methodology

### 1. Reset and start

Use a fresh datadir for every implementation being compared.

```bash
./mdb-dev stop
./mdb-dev reset-data
./mdb-dev start
```

Do not pin the server. Verify that it can use every online CPU:

```bash
pid=$(pgrep -xo mariadbd)
taskset -pc "$pid"
getconf _NPROCESSORS_ONLN
```

The recorded run used CPUs `0-19`.

### 2. Configure the server

```sql
SET GLOBAL innodb_buffer_pool_size=25769803776;
SET GLOBAL innodb_log_file_size=8589934592;
SET GLOBAL innodb_flush_log_at_trx_commit=2;
SET GLOBAL innodb_blink_leaf_pool_high=256;
SET GLOBAL innodb_blink_leaf_pool_low=64;
SET GLOBAL innodb_blink_internal_pool_high=32;
SET GLOBAL innodb_blink_internal_pool_low=8;
```

Verify the effective values before loading data. Keep adaptive hash disabled for both implementations.

```sql
SELECT @@innodb_buffer_pool_size,
       @@innodb_log_file_size,
       @@innodb_flush_log_at_trx_commit,
       @@innodb_adaptive_hash_index;
```

### 3. Prepare the proposal dataset

```bash
DATABASE=blink_proposal_t32 \
SOCKET=/var/run/mysqld/mysqld.sock \
BLINK=ON \
THREADS=32 \
PRELOAD_ROWS=2500000 \
MEASURED_ROWS=400000 \
PAYLOAD_SIZE=2500 \
BATCH_SIZE=250 \
LABEL=blink-proposal \
RESULT_ROOT=/tmp/blink-split-heavy-results \
./scripts/run_blink_split_heavy.sh prepare
```

Validate:

```sql
SELECT COUNT(*), MIN(id), MAX(id)
FROM blink_proposal_t32.split_heavy;
```

Expected preload count is `2,500,000`.

### 4. Prefill the B-link page pool

Force the only registered benchmark pool to its measured budget:

```sql
SET GLOBAL innodb_blink_leaf_pool_high=450000;
SET GLOBAL innodb_blink_leaf_pool_low=450000;
SET GLOBAL innodb_blink_internal_pool_high=512;
SET GLOBAL innodb_blink_internal_pool_low=512;
```

Wait for exact status depths:

```sql
SHOW GLOBAL STATUS LIKE 'Innodb_blink_pool_depth%';
```

Required values:

```text
Innodb_blink_pool_depth_leaf       450000
Innodb_blink_pool_depth_internal      512
```

On the recorded clean run this prefill took 5.205 seconds. Do not infer readiness from a stable refill counter; only the depth counters are authoritative.

### 5. Drain preparation dirty pages

The preload and prefill create roughly 800k dirty pages. They must not be flushed during the measured phase.

Save the current settings, then accelerate the preparation-only drain:

```sql
SET GLOBAL innodb_io_capacity_max=40000;
SET GLOBAL innodb_io_capacity=20000;
SET GLOBAL innodb_max_dirty_pages_pct=0;
```

Wait until:

```sql
SHOW GLOBAL STATUS LIKE 'Innodb_buffer_pool_pages_dirty';
```

reports at most 1,000 pages. Restore the normal settings:

```sql
SET GLOBAL innodb_max_dirty_pages_pct=90;
SET GLOBAL innodb_io_capacity=200;
SET GLOBAL innodb_io_capacity_max=2000;
```

The recorded preparation drained `839159 -> 743` dirty pages in 61.622 seconds and reached zero before the measured run.

### 6. Warm the dataset

Capture `Innodb_buffer_pool_reads`, run a full scan, and capture it again:

```sql
SELECT COUNT(*), SUM(OCTET_LENGTH(pad))
FROM blink_proposal_t32.split_heavy;
```

Admission requirement:

```text
warm-scan physical reads delta = 0
```

The recorded warm scan took 838 ms and performed zero physical reads.

### 7. Disable measured-phase refill

The pool already covers the full measured budget. Lowering the trigger prevents temporary cascade stash pops from starting background refill:

```sql
SET GLOBAL innodb_blink_leaf_pool_low=1;
SET GLOBAL innodb_blink_internal_pool_low=1;
```

Capture all global status counters immediately before the run.

### 8. Run the measured phase

```bash
DATABASE=blink_proposal_t32 \
SOCKET=/var/run/mysqld/mysqld.sock \
BLINK=ON \
THREADS=32 \
PRELOAD_ROWS=2500000 \
MEASURED_ROWS=400000 \
PAYLOAD_SIZE=2500 \
BATCH_SIZE=250 \
LABEL=blink-proposal \
RESULT_ROOT=/tmp/blink-split-heavy-results \
./scripts/run_blink_split_heavy.sh run
```

### 9. Admission and correctness checks

Required measured deltas:

```text
Innodb_blink_pool_refills             0
Innodb_blink_pool_empty_retries       0
Innodb_blink_incomplete_retries       0
Innodb_blink_normal_x_index           0
Innodb_buffer_pool_reads              0
Innodb_data_reads                     0
Innodb_data_writes                    0
Innodb_buffer_pool_pages_flushed      0
```

Structural invariant:

```text
leaf_splits + internal_splits = parent_installs
```

Validate the table:

```sql
SELECT COUNT(*), MIN(id), MAX(id)
FROM blink_proposal_t32.split_heavy;
SELECT id FROM blink_proposal_t32.split_heavy ORDER BY id LIMIT 5;
SELECT id FROM blink_proposal_t32.split_heavy ORDER BY id DESC LIMIT 5;
CHECK TABLE blink_proposal_t32.split_heavy;
```

Expected final count is `2,900,000`.

## Vanilla MariaDB methodology

Use a separate fresh datadir and the same binary build settings, CPU set, buffer pool, redo size, durability mode, AHI setting, workload script, and sysbench version.

Configure:

```sql
SET GLOBAL innodb_buffer_pool_size=25769803776;
SET GLOBAL innodb_log_file_size=8589934592;
SET GLOBAL innodb_flush_log_at_trx_commit=2;
```

If the vanilla binary exposes `innodb_blink_enabled`, set it to `OFF`; an unmodified vanilla build will not expose it.

Prepare with the same command except:

```text
BLINK=OFF
DATABASE=vanilla_proposal_t32
LABEL=vanilla-proposal
```

Vanilla has no page pool. Therefore omit the complete pool-prefill step. After preload, perform the same dirty-page drain, restore normal I/O capacity, run the same full warm scan, verify zero physical reads, capture status, and execute the same 400k/t32 measured phase.

For a valid comparison both runs must satisfy:

```text
same online CPUs and no affinity restriction
same 24 GiB buffer pool
same 8 GiB redo log
same flush_log_at_trx_commit=2
same AHI setting
same dataset shape and row counts
zero measured physical reads
zero measured data-page writes/flushes
zero ignored errors
CHECK TABLE OK
```

## Vanilla preparation cost

Vanilla MariaDB has no B-link page pool, no pool-size variables, and no preallocator thread. Consequently it does not pay pool-prefill time and is not affected by the allocator's historical 100 ms sleep.

Its preparation consists only of:

1. loading 2.5M rows;
2. draining preload dirty pages;
3. warming the dataset.

The actual duration depends on the vanilla insert path and storage throughput and must be measured rather than predicted. Record each stage with millisecond timestamps or `/usr/bin/time`; do not compare total preparation time unless preload, dirty drain, and warmup are reported separately.

## Possible optimization: concurrent pessimistic updates

The main B-link insert path performs leaf splits and parent cascades concurrently under `S(index)`. Some uncommon DML and rollback paths cannot currently use this protocol safely and require a synchronous fallback under `X(index)`:

- row-growing pessimistic `UPDATE`;
- insertion into a delete-marked record when the replacement does not fit;
- insert-by-modify overflow;
- rollback of a row-growing update;
- selected operations involving externally stored fields.

Here, synchronous means that the high-key-aware leaf split, parent installation, possible parent splits, and root raise complete in one mini-transaction before control returns to the caller. It does not imply a synchronous disk flush.

The fallback temporarily blocks normal B-link writers on the same index because their operations hold `S(index)`. This does not affect the canonical split-heavy benchmark: it inserts new keys only, and its measured `Innodb_blink_normal_x_index` delta was zero. Workloads dominated by record-growing updates can receive less benefit because a larger fraction of their structural changes will use the serialized path.

The synchronous fallback is required by the current update and undo architecture. Before the replacement insert requests a split, the operation may already have changed or delete-marked the old record, written undo information, transferred record locks, prepared externally stored fields, and established cursor-lifetime requirements. Publishing a split and returning a normal retry at this point would not be equivalent to retrying a clean insert.

A future fully concurrent update protocol could separate the operation into recoverable phases:

```text
prepare replacement
publish update intent
publish page split
install replacement
complete parent cascade
commit update intent
```

Such a design would require new persistent state, compensation and rollback rules, lock-transfer semantics, and crash-recovery support. It should therefore be treated as a separate optimization project rather than an extension of the normal concurrent insert split.

Future performance work should expose or retain a dedicated counter for synchronous B-link structural operations and report:

```text
sync_x_splits / total_splits
```

A near-zero ratio means that the workload uses the concurrent path almost exclusively. A high ratio identifies workloads where concurrent pessimistic updates could provide an additional scalability improvement.
