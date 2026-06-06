/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __TIDESDB_H__
#define __TIDESDB_H__

#include "alloc.h"
#include "block_manager.h"
#include "bloom_filter.h"
#include "btree.h"
#include "clock_cache.h"
#include "compat.h"
#include "compress.h"
#include "ini.h"
#include "local_cache.h"
#include "manifest.h"
#include "objstore.h"
#include "queue.h"
#include "skip_list.h"

/* logging levels for TDB_DEBUG_LOG */
typedef enum
{
    TDB_LOG_DEBUG = 0, /* general debugging info (most verbose) */
    TDB_LOG_INFO = 1,  /* informational messages */
    TDB_LOG_WARN = 2,  /* warnings (e.g., "Retry attempt N"..) */
    TDB_LOG_ERROR = 3, /* errors (e.g., "Failed to open file", "Invalid checksum") */
    TDB_LOG_FATAL = 4, /* fatal errors (e.g., "Corruption detected", "Out of memory") */
    TDB_LOG_NONE = 99  /* disable all logging */
} tidesdb_log_level_t;

extern _Atomic(int) _tidesdb_log_level; /* minimum level to log (default is TDB_LOG_DEBUG);
                                         * atomic -- the TDB_DEBUG_LOG macro gates on it
                                         * lock-free while tidesdb_open may rewrite it */
extern FILE *_tidesdb_log_file;         /* log file pointer (NULL = stderr, non-NULL = file) */
extern size_t _tidesdb_log_truncate;    /* truncate log file at this size (0 = no truncation) */
extern char _tidesdb_log_path[MAX_FILE_PATH_LENGTH]; /* path to log file for truncation */

/**
 * tidesdb_log_write
 * writes a log message to the configured log output (stderr or log file)
 * @param level log level (TDB_LOG_DEBUG, TDB_LOG_INFO, TDB_LOG_WARN, TDB_LOG_ERROR, TDB_LOG_FATAL)
 * @param file source file name (typically __FILE__)
 * @param line source line number (typically __LINE__)
 * @param fmt printf-style format string
 * @param ... format arguments
 */
void tidesdb_log_write(int level, const char *file, int line, const char *fmt, ...);

#define TDB_DEBUG_LOG(level, fmt, ...)                                           \
    do                                                                           \
    {                                                                            \
        if ((level) >= _tidesdb_log_level && _tidesdb_log_level != TDB_LOG_NONE) \
            tidesdb_log_write((level), __FILE__, __LINE__, fmt, ##__VA_ARGS__);  \
    } while (0)

/**
 * tidesdb_isolation_level_t
 * isolation levels for transactions
 *
 * tdb_isolation_read_uncommitted (0)
 *   -- sees all versions including uncommitted changes (dirty reads)
 *   -- no snapshot isolation, uses uint64_max to bypass filtering
 *   -- fastest but allows dirty reads, non-repeatable reads, and phantom reads
 *   -- no conflict detection
 *   -- good for analytics on non-critical data where performance is paramount
 *
 * tdb_isolation_read_committed (1)
 *   -- refreshes snapshot on each read operation
 *   -- prevents dirty reads by only seeing committed data
 *   -- allows non-repeatable reads (same key may return different values)
 *   -- allows phantom reads (range queries may see different rows)
 *   -- no conflict detection
 *   -- good default for most applications, good balance of consistency and performance
 *
 * tdb_isolation_repeatable_read (2)
 *   -- consistent snapshot taken at transaction start
 *   -- prevents dirty reads and non-repeatable reads for point reads
 *   -- allows phantom reads (new rows can appear in range queries)
 *   -- uses read-write conflict detection only
 *   -- aborts if a read key was modified by another transaction
 *   -- good for applications requiring consistent reads but tolerating some write conflicts
 *
 * tdb_isolation_snapshot (3)
 *   -- consistent snapshot with first-committer-wins semantics
 *   -- prevents dirty reads and non-repeatable reads
 *   -- prevents lost updates via write-write conflict detection
 *   -- allows write skew anomaly (two txns read overlapping data and write disjoint sets)
 *   -- no read set tracking, only write-write conflict detection
 *   -- aborts only on write-write conflict
 *   -- good for financial transactions, inventory management
 *
 * tdb_isolation_serializable (4)
 *   -- full serializability using ssi (serializable snapshot isolation)
 *   -- prevents dirty reads, non-repeatable reads, and phantom reads
 *   -- uses read-write, write-write, and rw-antidependency conflict detection
 *   -- tracks active transactions for dangerous structure detection
 *   -- highest isolation but lowest concurrency
 *   -- great for critical transactions requiring full acid guarantees
 */
typedef enum
{
    TDB_ISOLATION_READ_UNCOMMITTED = 0,
    TDB_ISOLATION_READ_COMMITTED = 1,
    TDB_ISOLATION_REPEATABLE_READ = 2,
    TDB_ISOLATION_SNAPSHOT = 3,
    TDB_ISOLATION_SERIALIZABLE = 4
} tidesdb_isolation_level_t;

/* error codes */
#define TDB_SUCCESS          0
#define TDB_ERR_MEMORY       -1
#define TDB_ERR_INVALID_ARGS -2
#define TDB_ERR_NOT_FOUND    -3
#define TDB_ERR_IO           -4
#define TDB_ERR_CORRUPTION   -5
#define TDB_ERR_EXISTS       -6
#define TDB_ERR_CONFLICT     -7
#define TDB_ERR_TOO_LARGE    -8
#define TDB_ERR_MEMORY_LIMIT -9
#define TDB_ERR_INVALID_DB   -10
#define TDB_ERR_UNKNOWN      -11
#define TDB_ERR_LOCKED       -12
#define TDB_ERR_READONLY     -13
/* system is at capacity and the operation gave up after the backpressure
 * stall hit its no-progress budget. transient; callers should retry */
#define TDB_ERR_BUSY -14

#ifdef TDB_ENABLE_READ_PROFILING
/**
 * tidesdb_read_stats_t
 * read profiling statistics (only available when TDB_ENABLE_READ_PROFILING is defined)
 * @param total_reads total number of read operations
 * @param memtable_hits reads satisfied from active memtable
 * @param immutable_hits reads satisfied from immutable memtables
 * @param sstable_hits reads satisfied from sstables on disk
 * @param levels_searched total levels searched across all reads
 * @param sstables_checked total sstables checked across all reads
 * @param bloom_checks total bloom filter checks performed
 * @param bloom_hits bloom filter checks that returned positive
 * @param blocks_read total klog blocks read from disk or cache
 * @param cache_block_hits block reads satisfied from block cache
 * @param cache_block_misses block reads that missed the cache
 * @param disk_reads total raw disk reads performed
 */
typedef struct
{
    _Atomic(uint64_t) total_reads;
    _Atomic(uint64_t) memtable_hits;
    _Atomic(uint64_t) immutable_hits;
    _Atomic(uint64_t) sstable_hits;
    _Atomic(uint64_t) levels_searched;
    _Atomic(uint64_t) sstables_checked;
    _Atomic(uint64_t) bloom_checks;
    _Atomic(uint64_t) bloom_hits;
    _Atomic(uint64_t) blocks_read;
    _Atomic(uint64_t) cache_block_hits;
    _Atomic(uint64_t) cache_block_misses;
    _Atomic(uint64_t) disk_reads;
} tidesdb_read_stats_t;
#endif

/* similar to relational database systems like oracle, where table and column names are limited to
 * 128 characters */
#define TDB_MAX_CF_NAME_LEN 128

/**
 * tidesdb_sync_mode_t
 * synchronization modes
 */
typedef enum
{
    TDB_SYNC_NONE, /* writes are not synced on every write, only once say sstable files are
                      completed */
    TDB_SYNC_FULL, /* writes are synced on every write, background and foreground wal and sstable
                      files */
    TDB_SYNC_INTERVAL, /* writes are synced on every write (background) all files,
    foreground wal syncs are done through sync worker */
} tidesdb_sync_mode_t;

/* default configuration values */
#define TDB_DEFAULT_WRITE_BUFFER_SIZE (64 * 1024 * 1024)
#define TDB_DEFAULT_LEVEL_SIZE_RATIO  10
/* cf trees grows organically -- L = log_T(N/B). starts with one disk
 * level and let add_level deepen it, rather than pre-allocating empty levels */
#define TDB_DEFAULT_MIN_LEVELS 1
/* spooky generalized Spooky sets the dividing level X to L-2; with
 * X = num_active_levels - 1 - offset that means offset = 1 */
#define TDB_DEFAULT_DIVIDING_LEVEL_OFFSET       1
#define TDB_DEFAULT_COMPACTION_THREAD_POOL_SIZE 2
/* fallback flush pool size when cpu detection fails at open */
#define TDB_DEFAULT_FLUSH_THREAD_POOL_SIZE 2
/* default config leaves num_flush_threads at 0 (auto); open resolves it to
 * min(cpu_count, TDB_FLUSH_THREADS_AUTO_CAP). a single shared flush pool feeds every CF, so a few
 * threads keeps multi-CF flushes from serializing without oversubscribing on large core counts */
#define TDB_FLUSH_THREADS_AUTO_CAP 4
/* pinned to the flush pool size tidesdb_open clamps max_concurrent_flushes to
 * num_flush_threads and warns when they differ, so the canonical default open
 * (default_config + open) must already agree or it warns on every startup */
#define TDB_DEFAULT_MAX_CONCURRENT_FLUSHES TDB_DEFAULT_FLUSH_THREAD_POOL_SIZE
#define TDB_DEFAULT_BLOOM_FPR              0.01
#define TDB_DEFAULT_KLOG_VALUE_THRESHOLD   512
#define TDB_DEFAULT_INDEX_SAMPLE_RATIO     1
#define TDB_DEFAULT_BLOCK_INDEX_PREFIX_LEN 16
#define TDB_DEFAULT_MIN_DISK_SPACE         (100 * 1024 * 1024)
#if defined(__OpenBSD__)
#define TDB_DEFAULT_MAX_OPEN_SSTABLES 64 /* x2 OpenBSD has lower default fd limits */
#else
#define TDB_DEFAULT_MAX_OPEN_SSTABLES 256 /* x2 each sstable has 2 fds, so really 512 */
#endif
#define TDB_DEFAULT_BLOCK_CACHE_SIZE    (64 * 1024 * 1024)
#define TDB_DEFAULT_SYNC_INTERVAL_US    128000
#define TDB_DEFAULT_LOG_FILE_TRUNCATION 24 * (1024 * 1024)

#define TDB_SKIP_LIST_MAX_LEVEL   12
#define TDB_SKIP_LIST_PROBABILITY 0.25f

/* configuration limits */
#define TDB_MAX_COMPARATOR_NAME 64
#define TDB_MAX_COMPARATOR_CTX  256

/* file system permissions */
#define TDB_DIR_PERMISSIONS 0755

/**
 * tidesdb_comparator_fn
 * comparator function type for custom key ordering
 * @param key1 first key to compare
 * @param key1_size size of first key in bytes
 * @param key2 second key to compare
 * @param key2_size size of second key in bytes
 * @param ctx user-provided context pointer
 * @return < 0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
typedef int (*tidesdb_comparator_fn)(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                     size_t key2_size, void *ctx);

/**
 * tidesdb_commit_op_t
 * represents a single operation in a committed transaction batch
 * @param key pointer to the key data
 * @param key_size size of the key in bytes
 * @param value pointer to the value data (NULL for deletes)
 * @param value_size size of the value in bytes (0 for deletes)
 * @param ttl time-to-live in seconds (0 = no expiration)
 * @param is_delete 1 if this is a delete operation, 0 for put
 */
typedef struct tidesdb_commit_op_t
{
    const uint8_t *key;
    size_t key_size;
    const uint8_t *value;
    size_t value_size;
    time_t ttl;
    int is_delete;
} tidesdb_commit_op_t;

/**
 * tidesdb_commit_hook_fn
 * callback invoked synchronously after a transaction commits to a column family
 * @param ops array of commit operations
 * @param num_ops number of operations in the array
 * @param commit_seq commit sequence number
 * @param ctx user-provided context
 */
typedef int (*tidesdb_commit_hook_fn)(const tidesdb_commit_op_t *ops, int num_ops,
                                      uint64_t commit_seq, void *ctx);

/* forward declarations for internal types */
#define TDB_MAX_LEVELS     32
#define TDB_IMM_SNAP_SLOTS 2 /* double-buffered RCU snapshot slots (one read, one rebuilt) */

typedef struct tidesdb_txn_op_t tidesdb_txn_op_t;
typedef struct tidesdb_merge_heap_t tidesdb_merge_heap_t;
typedef struct tidesdb_kv_pair_t tidesdb_kv_pair_t;
typedef struct tidesdb_commit_status_t tidesdb_commit_status_t;
typedef struct tidesdb_level_t tidesdb_level_t;
typedef struct tidesdb_sstable_t tidesdb_sstable_t;
typedef struct tidesdb_block_index_t tidesdb_block_index_t;
typedef struct tidesdb_memtable_t tidesdb_memtable_t;
typedef struct tidesdb_deferred_free_node_t tidesdb_deferred_free_node_t;
typedef struct tidesdb_t tidesdb_t;
typedef struct tidesdb_column_family_t tidesdb_column_family_t;

/* lock-free immutable memtable snapshot slot
 * part of a double-buffered RCU scheme; writers build in inactive slot,
 * swap the active index, then wait for old-slot readers to drain.
 * items is heap-allocated and grown lazily by the publisher to fit the queue
 * depth, so the snapshot never silently truncates -- the immutable queue is
 * bounded only by the configured l0_queue_stall_threshold, never by this array.
 * @param items heap array of immutable memtables (capacity = cap)
 * @param cap allocated capacity of items, in slots
 * @param count number of valid items in the array
 * @param readers number of active readers on this slot
 */
typedef struct
{
    tidesdb_memtable_t **items;
    size_t cap;
    _Atomic(size_t) count;
    _Atomic(int32_t) readers;
} tidesdb_imm_snap_t;

/* one column family's persisted unified memtable index
 * mirrors a line of the UNIMAP file. the index prefixes every key the cf
 * writes into the shared unified skip_list and wal, so it must stay stable
 * across reopen -- it is keyed on the cf name, the only cf identity that
 * survives a crash
 * @param name column family name
 * @param index the unified_cf_index permanently assigned to that name
 */
typedef struct
{
    char name[TDB_MAX_CF_NAME_LEN];
    uint32_t index;
} tidesdb_unified_cf_index_entry_t;

typedef struct tidesdb_txn_t tidesdb_txn_t;
typedef struct tidesdb_iter_t tidesdb_iter_t;
typedef struct tidesdb_stats_t tidesdb_stats_t;

/**
 * tidesdb_column_family_config_t
 * configuration for a column family
 * @param name column family name (set automatically when CF is created/loaded)
 * @param write_buffer_size size of write buffer
 * @param level_size_ratio ratio of level sizes
 * @param min_levels minimum number of levels
 * @param dividing_level_offset selects spooky's dividing level X via
 *                              X = num_levels - 1 - offset (X clamped to >= 1).
 *                              offset=0 means X=L-1 (the second-largest level)
 *                              and gives the 2L-spooky variant from the paper
 *                              with transient space-amp bounded by 1/T but the
 *                              highest write-amp. offset=1 means X=L-2 and is
 *                              the paper's recommended generalized tuning,
 *                              trading some ingest throughput for noticeably
 *                              lower compaction write-amp. higher offsets push
 *                              X further up the tree, reducing write-amp again
 *                              but multiplying the number of open files per
 *                              spooky equation 12. default is 1 (X=L-2, the paper's
 *                              generalized tuning, per TDB_DEFAULT_DIVIDING_LEVEL_OFFSET);
 *                              set to 0 (X=L-1) to favor ingest throughput at higher
 *                              write-amp.
 * @param klog_value_threshold threshold for klog value
 * @param compression_algorithm compression algorithm
 * @param enable_bloom_filter enable bloom filter
 * @param bloom_fpr bloom filter false positive rate
 * @param enable_block_indexes enable block indexes
 * @param index_sample_ratio index sample ratio
 * @param block_index_prefix_len block index prefix length
 * @param sync_mode sync mode
 * @param sync_interval_us sync interval in microseconds
 * @param comparator_name name of comparator
 * @param comparator_ctx_str comparator context string
 * @param comparator_fn_cached cached comparator function
 * @param comparator_ctx_cached cached comparator context
 * @param skip_list_max_level skip list max level
 * @param skip_list_probability skip list probability
 * @param default_isolation_level default isolation level
 * @param min_disk_space minimum free disk space required (bytes)
 * @param l1_file_count_trigger trigger for L1 file count, utilized for compaction triggering
 * @param l0_queue_stall_threshold threshold for L0 queue stall, utilized for backpressure
 * @param tombstone_density_trigger ratio in [0.0, 1.0] above which any single sstable's
 *                                  tombstone density (tombstone_count / num_entries) escalates
 *                                  compaction priority; 0.0 disables the check (default).
 *                                  sstables with fewer than tombstone_density_min_entries are
 *                                  ignored to prevent tiny-sstable noise.
 * @param tombstone_density_min_entries minimum entry count for an sstable to be considered by
 *                                      the density trigger; 0 falls back to the default
 * @param use_btree use btree for klog, faster reads depending on workload
 * @param commit_hook_fn optional commit hook callback (NULL = disabled, runtime-only)
 * @param commit_hook_ctx optional user context passed to commit hook (runtime-only)
 * @param object_target_file_size reserved for API compatibility, not used (file_max is derived from
 * level geometry per spooky algorithm 2)
 * @param object_lazy_compaction lazy compaction flag (1 = less aggressive, 0 = aggressive)
 * @param object_prefetch_compaction prefetch compaction flag (1 = download all inputs before merge,
 * 0 = stream)
 */
typedef struct tidesdb_column_family_config_t
{
    char name[TDB_MAX_CF_NAME_LEN];
    size_t write_buffer_size;
    size_t level_size_ratio;
    int min_levels;
    int dividing_level_offset;
    size_t klog_value_threshold;
    compression_algorithm compression_algorithm;
    int enable_bloom_filter;
    double bloom_fpr;
    int enable_block_indexes;
    int index_sample_ratio;
    int block_index_prefix_len;
    int sync_mode;
    uint64_t sync_interval_us;
    char comparator_name[TDB_MAX_COMPARATOR_NAME];
    char comparator_ctx_str[TDB_MAX_COMPARATOR_CTX];
    skip_list_comparator_fn comparator_fn_cached;
    void *comparator_ctx_cached;
    int skip_list_max_level;
    float skip_list_probability;
    tidesdb_isolation_level_t default_isolation_level;
    uint64_t min_disk_space;
    int l1_file_count_trigger;
    int l0_queue_stall_threshold;
    double tombstone_density_trigger;
    uint64_t tombstone_density_min_entries;
    int use_btree;
    tidesdb_commit_hook_fn commit_hook_fn;
    void *commit_hook_ctx;
    size_t object_target_file_size; /* reserved, not used */
    int object_lazy_compaction;
    int object_prefetch_compaction;
} tidesdb_column_family_config_t;

/**
 * tidesdb_comparator_entry_t
 * comparator registry entry
 * @param name unique name for the comparator
 * @param fn comparator function pointer
 * @param ctx_str optional context string (for serialization)
 * @param ctx runtime context pointer (reconstructed from ctx_str or set at registration)
 */
typedef struct tidesdb_comparator_entry_t
{
    char name[TDB_MAX_COMPARATOR_NAME];
    tidesdb_comparator_fn fn;
    char ctx_str[TDB_MAX_COMPARATOR_CTX];
    void *ctx;
} tidesdb_comparator_entry_t;

/**
 * tidesdb_config_t
 * configuration for the database
 * @param db_path path to the database
 * @param num_flush_threads number of flush threads
 * @param num_compaction_threads number of compaction threads
 * @param log_level minimum log level to display (TDB_LOG_DEBUG, TDB_LOG_INFO, TDB_LOG_WARN,
 * TDB_LOG_ERROR, TDB_LOG_FATAL, TDB_LOG_NONE)
 * @param block_cache_size size of clock cache for hot sstable blocks
 * @param max_open_sstables maximum number of resident open sstables (default 256); 0 = unlimited,
 * bounded only by the process open-file limit
 * @param log_to_file flag to determine if debug logging should be written to a file
 * @param log_truncation_at size in bytes at which to truncate the log file, 0 = no truncation
 * @param max_memory_usage maximum memory usage for the database
 * @param unified_memtable flag to determine if unified memtable should be used
 * @param unified_memtable_write_buffer_size write buffer size for unified memtable (0 = auto)
 * @param unified_memtable_skip_list_max_level skip list max level for unified memtable (0 = default
 * 12)
 * @param unified_memtable_skip_list_probability skip list probability (0 = default 0.25)
 * @param unified_memtable_sync_mode sync mode for unified WAL (default TDB_SYNC_NONE)
 * @param unified_memtable_sync_interval_us sync interval for unified WAL (0 = default)
 * @param object_store object store instance (NULL = local only, default)
 * @param object_store_config object store configuration (NULL = use defaults)
 * @param max_concurrent_flushes global semaphore on the number of in-flight memtable flushes
 *                               across all column families. bounds total transient memory and
 *                               work-queue depth when many column families flush at once.
 *                               pinned 1:1 to num_flush_threads at open -- a higher cap is
 *                               meaningless because the pool size is the upper bound, a lower
 *                               cap leaves workers idle. 0 means "match num_flush_threads",
 *                               any other mismatch is corrected with a warning.
 */
typedef struct tidesdb_config_t
{
    char *db_path;
    int num_flush_threads;
    int num_compaction_threads;
    tidesdb_log_level_t log_level;
    size_t block_cache_size;
    size_t max_open_sstables;
    int log_to_file;
    size_t log_truncation_at;
    size_t max_memory_usage;
    int unified_memtable;
    size_t unified_memtable_write_buffer_size;
    int unified_memtable_skip_list_max_level;
    float unified_memtable_skip_list_probability;
    int unified_memtable_sync_mode;
    uint64_t unified_memtable_sync_interval_us;
    tidesdb_objstore_t *object_store;
    tidesdb_objstore_config_t *object_store_config;
    int max_concurrent_flushes;
} tidesdb_config_t;

/**
 * tidesdb_memtable_t
 * pairs a skip list and WAL together for better isolation and rotation
 * @param skip_list the skip list data structure
 * @param wal associated write-ahead log
 * @param id unique identifier for this memtable
 * @param generation generation counter for memtable rotation
 * @param refcount reference count for safe concurrent access
 * @param writers count of commit-path writers actively mutating the WAL and skip list
 * @param flushed flag indicating if memtable has been flushed to disk
 */
struct tidesdb_memtable_t
{
    skip_list_t *skip_list;
    /* _Atomic -- a flush worker closes a rotated memtable's wal and clears this
     * while the reaper and sync worker may still read it on the active one */
    _Atomic(block_manager_t *) wal;
    uint64_t id;
    uint64_t generation;
    _Atomic(int) refcount;
    _Atomic(int) writers;
    _Atomic(int) flushed;
};

/**
 * tidesdb_column_family_t
 * a column family is an independent key-value storage with its own config, memtables, WALs, etc.
 * @param name name of column family
 * @param directory directory for column family
 * @param config column family configuration
 * @param active_memtable active memtable (paired skip list and WAL)
 * @param immutable_memtables queue of immutable memtables being flushed
 * @param immutable_bytes sum of actual immutable skip-list sizes, refreshed on each snapshot
 * publish, read by the reaper for memory-pressure accounting
 * @param pending_commits count of in-flight commits
 * @param levels fixed array of disk levels
 * @param num_active_levels number of currently active disk levels
 * @param next_sstable_id next sstable id
 * @param sstable_layout_version monotonic version for sstable layout changes
 * @param is_compacting atomic flag indicating compaction is queued
 * @param is_flushing atomic flag indicating flush is queued
 * @param flush_pending_count per-CF count of queued + in-flight flush work items
 * @param flush_deferred flag set when a flush was skipped at the global concurrent-flush cap
 * @param compaction_pending_count per-CF count of queued + in-flight compaction work items
 * @param compaction_armed flag set when an enqueue was skipped because is_compacting was 1; the
 * worker drains this when its current job ends and self-enqueues a follow-up
 * @param immutable_cleanup_counter counter for batched immutable cleanup
 * @param marked_for_deletion flag indicating column family is marked for deletion
 * @param manifest manifest for column family
 * @param db parent database reference
 * @param imm_snaps double-buffered lock-free immutable memtable snapshot slots
 * @param imm_snap_active index (0 or 1) of the currently active snapshot slot
 * @param imm_snap_publish_lock serializes concurrent snapshot publishers
 * @param unified_cf_index unified memtable column family index (4-byte big-endian prefix)
 */
struct tidesdb_column_family_t
{
    char *name;
    char *directory;
    tidesdb_column_family_config_t config;
    _Atomic(tidesdb_memtable_t *) active_memtable;
    queue_t *immutable_memtables;
    _Atomic(int64_t) immutable_bytes;
    _Atomic(uint64_t) pending_commits;
    tidesdb_level_t *levels[TDB_MAX_LEVELS];
    _Atomic(int) num_active_levels;
    _Atomic(uint64_t) next_sstable_id;
    _Atomic(uint64_t) sstable_layout_version;
    _Atomic(int) is_compacting;
    _Atomic(int) is_flushing;
    _Atomic(int) flush_pending_count;
    _Atomic(int) flush_deferred;
    _Atomic(int) compaction_pending_count;
    _Atomic(int) compaction_armed;
    _Atomic(int) immutable_cleanup_counter;
    _Atomic(int) marked_for_deletion;
    tidesdb_manifest_t *manifest;
    tidesdb_t *db;

    /* lock-free immutable memtable snapshot (double-buffered RCU)
     * readers acquire active slot, use items, release when done
     * writers rebuild in inactive slot, swap active, wait for old readers */
    tidesdb_imm_snap_t imm_snaps[TDB_IMM_SNAP_SLOTS];
    _Atomic(int) imm_snap_active; /* 0 or 1, index of current snapshot */

    /* publishers rebuild the inactive slot then swap -- the RCU design tolerates
     * many readers but only one writer, so concurrent publishers (flush worker
     * cleanup vs compaction-triggered flush) must serialize on this lock */
    pthread_mutex_t imm_snap_publish_lock;

    /* a single compaction round (serialized per CF by is_compacting) may run its
     * partition sub-merges across multiple sub-compaction threads; this serializes the
     * per-partition commit section (level add + manifest commit + layout bump) so the
     * heavy merge work parallelizes while shared-state mutation stays single-threaded */
    pthread_mutex_t compaction_commit_lock;

    /* read-side epoch for the active_memtable slot. a reader bumps this before
     * loading active_memtable + try_ref'ing the loaded pointer, drops it once
     * try_ref has finished (success means refcount is now pinned, failure means
     * we never touched the struct after the cas). the immutable cleanup loop
     * drains this counter to 0 before free()ing a memtable struct so a reader
     * holding a stale active_memtable pointer cannot UAF on try_ref's refcount
     * read. mirrors imm_snap_t.readers but for the direct-active read path */
    _Atomic(int) active_mt_readers;

    /* unified memtable mode -- 4-byte big-endian CF prefix for keys in the shared skip list */
    uint32_t unified_cf_index;

    /* write-amplification instrumentation -- lifetime, relaxed, observe-only. byte counters
     * are on-disk (framed) totals-- wal counts framed WAL appends (stays zero in unified mode,
     * where the shared uwal counter on tidesdb_t carries it), flush and compaction count
     * finished sstable file sizes (klog_size + vlog_size), user counts logical key+value bytes
     * committed (incremented on commit apply and on WAL replay so the denominator matches the
     * data actually flushed). the *_count fields count output sstables, not logical runs -- a
     * single triggered compaction can produce several. zero-initialized by the cf calloc */
    _Atomic(uint64_t) wal_bytes_written;
    _Atomic(uint64_t) flush_bytes_written;
    _Atomic(uint64_t) compaction_bytes_written;
    _Atomic(uint64_t) compaction_bytes_read;
    _Atomic(uint64_t) user_bytes_written;
    _Atomic(uint64_t) flush_count;
    _Atomic(uint64_t) compaction_count;

    /* last-emit timestamps (seconds) for throttled backpressure warnings -- see tdb_log_throttle.
     * zero-initialized by calloc, so the first event in each category logs immediately. */
    _Atomic(time_t) last_ceiling_stall_log_sec;
    _Atomic(time_t) last_imm_critical_log_sec;
    _Atomic(time_t) last_backpressure_log_sec;
};

/**
 * tidesdb_sstable_t
 * an immutable sorted string table on disk
 * consists of two files a .klog (keys + metadata) and .vlog (large values)
 * @param id unique identifier
 * @param klog_path path to .klog file
 * @param klog_filename cached pointer into klog_path past the last path separator
 * @param vlog_path path to .vlog file
 * @param cf_name cached column family name for block cache lookups
 * @param min_key minimum key in this sstable
 * @param min_key_size size of minimum key
 * @param max_key maximum key in this sstable
 * @param max_key_size size of maximum key
 * @param num_entries total number of keys
 * @param tombstone_count count of tombstone entries (TDB_KV_FLAG_TOMBSTONE) in this sstable.
 *                       TDB_TOMBSTONE_COUNT_UNKNOWN means a legacy footer pre-dating the field.
 * @param num_klog_blocks number of blocks in klog
 * @param num_vlog_blocks number of blocks in vlog
 * @param klog_data_end_offset offset where data ends in klog (before footer)
 * @param klog_size total size of klog file
 * @param vlog_size total size of vlog file
 * @param max_seq maximum sequence number in this sstable
 * @param bloom_filter bloom filter for key existence checks
 * @param block_indexes block indexes for fast key lookup
 * @param refcount reference count for safe concurrent access
 * @param klog_bm klog block manager
 * @param vlog_bm vlog block manager
 * @param config column family configuration
 * @param marked_for_deletion flag indicating sstable is marked for deletion
 * @param last_access_time last access time for lru eviction
 * @param db database handle (for resolving comparators from registry)
 * @param use_btree flag indicating sstable uses btree format
 * @param btree_root_offset root node offset for btree
 * @param btree_first_leaf first leaf offset for btree forward iteration
 * @param btree_last_leaf last leaf offset for btree backward iteration
 * @param btree_node_count total number of nodes in btree
 * @param btree_height height of btree
 * @param cached_comparator_fn cached comparator function for fast iteration
 * @param cached_comparator_ctx cached comparator context for fast iteration
 * @param is_reverse flag indicating sstable is reverse sorted
 * @param cache_key_prefix globally unique prefix for btree node cache keys
 */
struct tidesdb_sstable_t
{
    uint64_t id;
    char *klog_path;
    const char *klog_filename;
    char *vlog_path;
    char cf_name[TDB_MAX_CF_NAME_LEN];
    uint8_t *min_key;
    size_t min_key_size;
    uint8_t *max_key;
    size_t max_key_size;
    uint64_t num_entries;
    uint64_t tombstone_count;
    uint64_t num_klog_blocks;
    uint64_t num_vlog_blocks;
    uint64_t klog_data_end_offset;
    uint64_t klog_size;
    uint64_t vlog_size;
    uint64_t max_seq;
    bloom_filter_t *bloom_filter;
    tidesdb_block_index_t *block_indexes;
    _Atomic(int) refcount;
    /* opened lazily by tidesdb_sstable_ensure_open and published by CAS, so the
     * pointers are _Atomic -- readers acquire-load them and so observe the fully
     * initialized block_manager the opener built before the publishing CAS */
    _Atomic(block_manager_t *) klog_bm;
    _Atomic(block_manager_t *) vlog_bm;
    tidesdb_column_family_config_t *config;
    _Atomic(int) marked_for_deletion;
    _Atomic(time_t) last_access_time;
    tidesdb_t *db;
    int use_btree;
    int64_t btree_root_offset;
    int64_t btree_first_leaf;
    int64_t btree_last_leaf;
    uint64_t btree_node_count;
    uint32_t btree_height;
    skip_list_comparator_fn cached_comparator_fn;
    void *cached_comparator_ctx;
    int is_reverse;
    uint64_t cache_key_prefix;
    /* chunked footer aux blobs -- when a bloom filter or block index footer blob
     * exceeds the single-block chunk size it is written as multiple consecutive
     * blocks and located by explicit offset+size instead of trailing-block
     * navigation. aux_chunked is set (and the offsets persisted in metadata) only
     * for such sstables; legacy/small sstables leave it 0 and use the original
     * trailing-block read path. */
    int aux_chunked;
    uint64_t bloom_blob_offset;
    uint64_t bloom_blob_size;
    uint64_t index_blob_offset;
    uint64_t index_blob_size;
};

/**
 * tidesdb_level_t
 * a level in the lsm tree within a column family
 * @param level_num level number
 * @param capacity capacity of level in bytes
 * @param current_size current size of level in bytes
 * @param sstables array of sstable pointers (copy-on-write)
 * @param num_sstables number of sstables in array
 * @param sstables_capacity capacity of sstables array
 * @param file_boundaries file boundaries for partitioning
 * @param boundary_sizes sizes of boundary keys
 * @param num_boundaries number of boundaries
 * @param retired_sstables_arr array of retired sstables (mainly TOCTOU protection)
 * @param array_readers count of concurrent readers accessing sstable array
 */
struct tidesdb_level_t
{
    int level_num;
    _Atomic(size_t) capacity;
    _Atomic(size_t) current_size;
    _Atomic(tidesdb_sstable_t **) sstables;
    _Atomic(int) num_sstables;
    _Atomic(int) sstables_capacity;
    _Atomic(uint8_t **) file_boundaries;
    _Atomic(size_t *) boundary_sizes;
    _Atomic(int) num_boundaries;
    _Atomic(tidesdb_sstable_t **) retired_sstables_arr;
    _Atomic(int) array_readers;
};

/**
 * tidesdb_t
 * main database handle
 * @param db_path path to database directory
 * @param config database configuration
 * @param column_families array of column families
 * @param num_column_families number of column families
 * @param cf_capacity capacity of column families array
 * @param is_open atomic flag indicating database is fully open and ready for operations
 * @param is_recovering flag to determine if system is recovering
 * @param comparators atomic pointer to comparators array (lock-free COW)
 * @param num_comparators atomic count of registered comparators
 * @param comparators_capacity atomic capacity of comparators array
 * @param flush_threads array of flush threads
 * @param flush_queue queue of flush work items
 * @param compaction_threads array of compaction threads
 * @param compaction_queue queue of compaction work items
 * @param sync_thread background thread for interval syncing
 * @param sync_thread_active atomic flag indicating if sync thread is active
 * @param sync_thread_mutex mutex for sync thread
 * @param sync_thread_cond condition variable for sync thread
 * @param reaper_thread background thread for housekeeping
 * @param reaper_active atomic flag indicating if reaper thread is active
 * @param reaper_thread_mutex mutex for reaper thread
 * @param reaper_thread_cond condition variable for reaper thread
 * @param clock_cache clock cache for hot sstable blocks
 * @param btree_node_cache clock cache for hot btree nodes, created lazily on the
 *                         first btree column family so a database with no btree
 *                         column family does not pay for it
 * @param btree_cache_lock guards the one time lazy creation of btree_node_cache
 * @param resolved_block_cache_size block cache size after clamping, reused when
 *                                  btree_node_cache is created lazily
 * @param num_open_sstables global counter for open sstables
 * @param next_txn_id global transaction id counter
 * @param global_seq global sequence counter for snapshots and commits
 * @param commit_status tracks which sequences are committed
 * @param active_txns_lock rwlock for active transactions list
 * @param active_txns array of active serializable transactions
 * @param num_active_txns number of active transactions
 * @param active_txns_capacity capacity of active transactions array
 * @param cached_available_disk_space cached available disk space in bytes
 * @param last_disk_space_check timestamp of last disk space check
 * @param cached_current_time cached current time updated by reaper thread to avoid syscalls
 * @param available_memory available system memory in bytes
 * @param total_memory total system memory in bytes
 * @param resolved_memory_limit resolved global memory limit in bytes
 * @param cached_memtable_bytes cached total memtable + cache memory (updated by reaper)
 * @param sstable_aux_memory_bytes running total of bloom filter + block index
 *                                 memory across every sstable currently in a
 *                                 level, maintained at level add and remove so
 *                                 the reaper does not rescan every sstable
 * @param memory_pressure_level cached pressure level 0=normal 1=elevated 2=high 3=critical
 * @param txn_memory_bytes bytes held by in-flight transactions
 * @param flush_pending_count number of pending flush operations (queued + in-flight)
 * @param active_flushes global semaphore counter for in-flight flushes across all column
 *                       families. capped by config.max_concurrent_flushes.
 * @param flush_heartbeat monotonic counter bumped by flush workers as they make progress;
 *                        backpressure reads it to distinguish a slow flush from a wedged one
 * @param os_check_counter counter for periodic os-level memory checks
 * @param cf_list_lock rwlock for cf list modifications
 * @param deferred_free_list lock-free singly-linked list of deferred free nodes for retired arrays
 * @param lock_fd file descriptor for lock file
 * @param log_file file descriptor for log file
 * @param read_stats read profiling statistics (only when TDB_ENABLE_READ_PROFILING is defined)
 * @param object_store active object store connector (NULL = local only)
 * @param local_cache local file cache manager for object store mode
 * @param upload_threads background upload thread pool for async sstable uploads
 * @param num_upload_threads number of upload threads
 * @param upload_queue queue of upload jobs (tdb_upload_job_t)
 * @param last_uploaded_gen highest WAL generation confirmed uploaded to object store
 * @param total_uploads lifetime count of objects uploaded to object store
 * @param total_upload_failures lifetime count of permanently failed uploads (after all retries)
 * @param replica_mode 1 if running as read-only replica, 0 if primary
 * @param replica_sync_thread_active 1 while the dedicated replica sync thread runs
 */
struct tidesdb_t
{
    char *db_path;
    tidesdb_config_t config;
    tidesdb_column_family_t **column_families;
    /* _Atomic -- written under cf_list_lock on cf create/drop but read
     * lock-free by tdb_cf_effective_stall on the backpressure hot path */
    _Atomic(int) num_column_families;
    int cf_capacity;
    _Atomic(int) is_open;
    _Atomic(int) is_recovering;
    /* set by tidesdb_cancel_background_work -- when non-zero, in-flight compactions
     * bail at their next checkpoint and queued compaction work items are skipped.
     * compaction-only, flushes are unaffected so durability is preserved. sticky for
     * the db session, reset to 0 on open. */
    _Atomic(int) cancel_compaction;
    _Atomic(tidesdb_comparator_entry_t *) comparators;
    _Atomic(int) num_comparators;
    _Atomic(int) comparators_capacity;
    pthread_t *flush_threads;
    queue_t *flush_queue;
    pthread_t *compaction_threads;
    queue_t *compaction_queue;
    /* budget of ephemeral sub-compaction helper threads a compaction round may spawn,
     * initialized to num_compaction_threads at open. bounds total concurrent sub-merge
     * threads across all CFs so parallel compaction never oversubscribes the pool. */
    _Atomic(int) compaction_helper_budget;
    pthread_t sync_thread;
    _Atomic(int) sync_thread_active;
    pthread_mutex_t sync_thread_mutex;
    pthread_cond_t sync_thread_cond;
    pthread_t reaper_thread;
    _Atomic(int) reaper_active;
    pthread_mutex_t reaper_thread_mutex;
    pthread_cond_t reaper_thread_cond;
    clock_cache_t *clock_cache;
    /* created lazily after worker threads are running, so the pointer is
     * _Atomic -- btree_cache_lock still serializes the one-time creation */
    _Atomic(clock_cache_t *) btree_node_cache;
    pthread_mutex_t btree_cache_lock;
    size_t resolved_block_cache_size;
    _Atomic(int) num_open_sstables;
    /* last-emit timestamp (seconds) for the throttled open-failure (EMFILE) diagnostic, so a
     * descriptor-exhaustion storm logs one legible line per second instead of flooding */
    _Atomic(time_t) last_open_fail_log_sec;
    _Atomic(uint64_t) next_txn_id;
    _Atomic(uint64_t) global_seq;
    tidesdb_commit_status_t *commit_status;
    pthread_rwlock_t active_txns_lock;
    tidesdb_txn_t **active_txns;
    int num_active_txns;
    int active_txns_capacity;
    _Atomic(uint64_t) cached_available_disk_space;
    _Atomic(time_t) last_disk_space_check;
    _Atomic(time_t) cached_current_time;
    uint64_t available_memory;
    uint64_t total_memory;
    _Atomic(size_t) resolved_memory_limit;
    _Atomic(int64_t) cached_memtable_bytes;
    _Atomic(int64_t) sstable_aux_memory_bytes;
    _Atomic(int64_t) txn_memory_bytes;
    _Atomic(int) memory_pressure_level;
    _Atomic(int) flush_pending_count;
    _Atomic(int) active_flushes;
    _Atomic(uint64_t) flush_heartbeat;
    /* write-amplification instrumentation -- lifetime, relaxed, observe-only. uwal is
     * db-scoped because the unified WAL is shared across CFs; the per-cf flush, compaction
     * and wal byte counters live on tidesdb_column_family_t and their db-level sums are
     * folded across CFs in tidesdb_get_db_stats */
    _Atomic(uint64_t) uwal_bytes_written;
    int os_check_counter;
    pthread_rwlock_t cf_list_lock;
    _Atomic(tidesdb_deferred_free_node_t *) deferred_free_list;
    int lock_fd;
    FILE *log_file;
#ifdef TDB_ENABLE_READ_PROFILING
    tidesdb_read_stats_t read_stats;
#endif

    /* unified memtable mode -- single skip_list + single WAL for all CFs */
    struct
    {
        int enabled;                          /* 1 when unified memtable mode is active */
        _Atomic(tidesdb_memtable_t *) active; /* current active unified memtable */
        /* read-side epoch for the unified active slot. see the analogous
         * cf->active_mt_readers field for the protocol */
        _Atomic(int) active_mt_readers;
        queue_t *immutables;                    /* rotated unified memtables awaiting flush */
        _Atomic(int) is_flushing;               /* 1 while a rotation/flush is in progress */
        _Atomic(int) immutable_cleanup_counter; /* batched immutable cleanup counter */
        size_t write_buffer_size;               /* rotation threshold for the unified memtable */
        _Atomic(uint32_t) next_cf_index;        /* next CF prefix index to assign */
        _Atomic(uint64_t) wal_generation;       /* current unified WAL generation */
        tidesdb_unified_cf_index_entry_t *cf_index_map; /* name -> index, mirrors UNIMAP file */
        int cf_index_map_count;                         /* live entries in cf_index_map */
        int cf_index_map_capacity;                      /* allocated capacity of cf_index_map */
        pthread_mutex_t cf_index_map_lock;              /* guards cf_index_map mutation */
        pthread_mutex_t wal_group_sync_lock; /* coordinates group-commit fsync on the unified WAL */
        pthread_cond_t wal_group_sync_cond;
        /* last-emit timestamp (seconds) for the throttled unified ceiling-stall warning */
        _Atomic(time_t) last_ceiling_stall_log_sec;
    } unified_mt;

    /* object store mode runtime state */
    tidesdb_objstore_t *object_store;        /* active connector (NULL = local only) */
    tdb_local_cache_t *local_cache;          /* local file cache manager */
    pthread_t *upload_threads;               /* background upload thread pool */
    int num_upload_threads;                  /* number of upload threads */
    queue_t *upload_queue;                   /* queue of tdb_upload_job_t */
    _Atomic(uint64_t) last_uploaded_gen;     /* highest WAL gen confirmed uploaded */
    _Atomic(uint64_t) total_uploads;         /* lifetime upload count */
    _Atomic(uint64_t) total_upload_failures; /* lifetime failed upload count */
    _Atomic(uint64_t) last_wal_sync_size;    /* WAL file size at last object store sync;
                                              * _Atomic -- reaper writes it, open seeds it */

    /* replica mode runtime state */
    _Atomic(int) replica_mode;               /* 1 = read-only replica, 0 = primary */
    pthread_t replica_sync_thread;           /* dedicated replica MANIFEST/WAL sync thread */
    _Atomic(int) replica_sync_thread_active; /* 1 while the replica sync thread runs */

    /* compaction pause gate -- tidesdb_backup holds this across its file copy
     * so the copy cannot race a compaction rewriting the manifest + sstable set */
    pthread_mutex_t compaction_gate_lock;
    int compaction_paused;           /* guarded by compaction_gate_lock */
    _Atomic(int) active_compactions; /* compactions past the gate, in flight */
};

/**
 * tidesdb_txn_t
 * transaction handle for batched operations with acid guarantees
 *
 * supports multiple isolation levels:
 * -- read_uncommitted  sees all versions including uncommitted (dirty reads allowed)
 * -- read_committed    refreshes snapshot on each read (prevents dirty reads)
 * -- repeatable_read   consistent snapshot, read-write conflict detection
 * -- snapshot          consistent snapshot, write-write conflict detection only
 * -- serializable      full ssi with dangerous structure detection (prevents all anomalies)
 *
 * snapshot isolation semantics:
 * -- snapshot captured at begin (all committed txns with seq <= snapshot_seq are visible)
 * -- conflict detection at commit (isolation level dependent)
 * -- commit sequence acquired after conflict detection
 * -- no retries -- conflicts cause immediate abort
 * -- works across multiple column families
 *
 * @param db database handle
 * @param txn_id transaction id
 * @param snapshot_seq snapshot sequence captured at begin
 * @param commit_seq commit sequence (0 until commit)
 * @param ops array of operations
 * @param num_ops number of operations
 * @param ops_capacity capacity of operations array
 * @param read_keys array of read keys for conflict detection
 * @param read_key_sizes array of read key sizes
 * @param read_seqs array of read sequence numbers
 * @param read_cfs array of column families for each read key
 * @param read_set_count number of read keys
 * @param read_set_capacity capacity of read keys array
 * @param read_key_arenas array of read key arenas
 * @param read_key_arena_count number of read key arenas
 * @param read_key_arena_used bytes used in current read key arena
 * @param write_set_hash hash table for O(1) write set lookup (NULL if num_ops <
 * TDB_TXN_WRITE_HASH_THRESHOLD)
 * @param read_set_hash hash table for O(1) read set lookup (NULL if read_set_count <
 * TDB_TXN_READ_HASH_THRESHOLD)
 * @param cfs array of column families involved in transaction
 * @param num_cfs number of column families
 * @param cf_capacity capacity of column families array
 * @param last_cf cached last-used column family for O(1) single-CF lookup
 * @param last_cf_index cached index of last-used column family
 * @param savepoint_op_counts per-savepoint snapshot of num_ops -- the op-array length to truncate
 * back to on rollback to that savepoint
 * @param savepoint_cf_counts per-savepoint snapshot of num_cfs -- the cf-array length to truncate
 * back to on rollback to that savepoint
 * @param savepoint_names array of savepoint names
 * @param num_savepoints number of savepoints
 * @param savepoints_capacity capacity of savepoints array
 * @param is_committed flag indicating if transaction is committed
 * @param is_aborted flag indicating if transaction is aborted
 * @param isolation_level isolation level for this transaction
 * @param has_rw_conflict_in flag indicating rw-conflict-in (another txn read our writes)
 * @param has_rw_conflict_out flag indicating rw-conflict-out (we read another txn's writes)
 * @param mem_bytes running total of this txn's op buffer + read-key arena bytes (owned by the
 *                  committing thread, so plain non-atomic accounting)
 * @param mem_published amount of mem_bytes already reflected in db->txn_memory_bytes; the delta
 *                      is flushed to the global counter in threshold-sized batches
 */
struct tidesdb_txn_t
{
    tidesdb_t *db;
    uint64_t txn_id;
    uint64_t snapshot_seq;
    uint64_t commit_seq;
    tidesdb_txn_op_t *ops;
    int num_ops;
    int ops_capacity;
    uint8_t **read_keys;
    size_t *read_key_sizes;
    uint64_t *read_seqs;
    tidesdb_column_family_t **read_cfs;
    int read_set_count;
    int read_set_capacity;
    uint8_t **read_key_arenas;
    int read_key_arena_count;
    size_t read_key_arena_used;
    void *write_set_hash;
    void *read_set_hash;
    tidesdb_column_family_t **cfs;
    int num_cfs;
    int cf_capacity;
    tidesdb_column_family_t *last_cf;
    int last_cf_index;
    int *savepoint_op_counts;
    int *savepoint_cf_counts;
    char **savepoint_names;
    int num_savepoints;
    int savepoints_capacity;
    /* these flags are read cross-txn by tidesdb_txn_check_ssi_conflicts while
     * the owning txn writes them on commit/abort, so they are _Atomic */
    _Atomic(int) is_committed;
    _Atomic(int) is_aborted;
    tidesdb_isolation_level_t isolation_level;
    _Atomic(int) has_rw_conflict_in;
    _Atomic(int) has_rw_conflict_out;
    int64_t mem_bytes;
    int64_t mem_published;
};

/**
 * tidesdb_iter_t
 * iterator for database
 * @param cf column family (for single-cf iteration)
 * @param txn transaction (for isolation and multi-cf iteration)
 * @param heap merge heap
 * @param current current key-value pair
 * @param valid validity flag
 * @param direction direction of iteration (1=forward, -n=backward)
 * @param snapshot_time snapshot time for ttl checks
 * @param cf_snapshot snapshot sequence for visibility checks
 * @param cached_sources cached sst sources for reuse across seeks
 * @param num_cached_sources number of cached sources
 * @param cached_sources_capacity capacity of cached sources array
 * @param cached_mt_sources cached memtable sources for reuse across seeks
 * @param num_cached_mt_sources number of cached memtable sources
 * @param temp_sources pre-allocated temporary source array for seek operations
 * @param temp_sources_capacity capacity of temp_sources array
 */
struct tidesdb_iter_t
{
    tidesdb_column_family_t *cf;
    tidesdb_txn_t *txn;
    tidesdb_merge_heap_t *heap;
    tidesdb_kv_pair_t *current;
    int valid;
    int direction;
    time_t snapshot_time;
    uint64_t cf_snapshot;
    void **cached_sources;
    int num_cached_sources;
    int cached_sources_capacity;
    void **cached_mt_sources;
    int num_cached_mt_sources;
    void **temp_sources;
    int temp_sources_capacity;
};

/**
 * tidesdb_stats_t
 * statistics for database column family
 * @param num_levels number of levels
 * @param memtable_size size of memtable
 * @param level_sizes sizes of each level
 * @param level_num_sstables number of sstables in each level
 * @param config column family configuration
 * @param total_keys total number of keys across memtable and all sstables
 * @param total_data_size total data size (klog + vlog) across all sstables
 * @param avg_key_size average key size in bytes
 * @param avg_value_size average value size in bytes
 * @param level_key_counts number of keys per level
 * @param read_amp read amplification (point lookup cost multiplier)
 * @param hit_rate cache hit rate (0.0 if cache disabled)
 * @param use_btree whether column family uses b+tree klog format
 * @param btree_total_nodes total b+tree nodes across all sstables
 * @param btree_max_height maximum tree height across all sstables
 * @param btree_avg_height average tree height across all sstables
 * @param total_tombstones sum of tombstone_count across every sstable in the cf
 * @param tombstone_ratio total_tombstones / total_keys (0.0 if total_keys is 0)
 * @param level_tombstone_counts tombstone count per level (parallels level_key_counts)
 * @param max_sst_density worst per-sstable tombstone density observed in the cf
 * @param max_sst_density_level 1-based level where max_sst_density was observed (0 if none)
 * @param wal_bytes_written framed bytes appended to this cf's WAL (0 in unified mode)
 * @param flush_bytes_written on-disk bytes this cf's flushes wrote to L0 sstables
 * @param compaction_bytes_written on-disk bytes this cf's compactions wrote
 * @param compaction_bytes_read on-disk bytes this cf's compactions read as input
 * @param user_bytes_written logical key+value bytes committed to this cf (WA denominator)
 * @param flush_count flushed sstables produced by this cf
 * @param compaction_count compaction output sstables produced by this cf
 */
struct tidesdb_stats_t
{
    int num_levels;
    size_t memtable_size;
    size_t *level_sizes;
    int *level_num_sstables;
    tidesdb_column_family_config_t *config;
    uint64_t total_keys;
    uint64_t total_data_size;
    double avg_key_size;
    double avg_value_size;
    uint64_t *level_key_counts;
    double read_amp;
    double hit_rate;
    /* btree stats (only populated if use_btree=1) */
    int use_btree;
    uint64_t btree_total_nodes;
    uint32_t btree_max_height;
    double btree_avg_height;
    /* tombstone observability */
    uint64_t total_tombstones;
    double tombstone_ratio;
    uint64_t *level_tombstone_counts;
    double max_sst_density;
    int max_sst_density_level;
    /* write-amplification counters (lifetime since open, on-disk framed bytes). divide the
     * write totals by user_bytes_written for this cf's write amplification. wal_bytes_written
     * is zero in unified mode -- the shared WAL volume is reported db-wide in
     * tidesdb_db_stats_t.uwal_bytes_written. the *_count fields count output sstables. */
    uint64_t wal_bytes_written;
    uint64_t flush_bytes_written;
    uint64_t compaction_bytes_written;
    uint64_t compaction_bytes_read;
    uint64_t user_bytes_written;
    uint64_t flush_count;
    uint64_t compaction_count;
};

/**
 * tidesdb_cache_stats_t
 * statistics for database block cache
 * @param enabled whether block cache is enabled
 * @param total_entries total number of cached entries
 * @param total_bytes total bytes used by cache
 * @param hits cache hits
 * @param misses cache misses
 * @param hit_rate hit rate (hits / (hits + misses))
 * @param num_partitions number of cache partitions
 */
typedef struct tidesdb_cache_stats_t
{
    int enabled;
    size_t total_entries;
    size_t total_bytes;
    uint64_t hits;
    uint64_t misses;
    double hit_rate;
    size_t num_partitions;
} tidesdb_cache_stats_t;

/**
 * tidesdb_db_stats_t
 * database-level statistics
 * @param num_column_families number of column families
 * @param total_memory system total memory
 * @param available_memory system available memory at open
 * @param resolved_memory_limit resolved memory limit
 * @param memory_pressure_level current memory pressure level (0=normal, 1=elevated, 2=high,
 * 3=critical)
 * @param flush_pending_count number of pending flush operations (queued + in-flight)
 * @param total_memtable_bytes total bytes in active memtables across all CFs
 * @param total_immutable_count total immutable memtables across all CFs
 * @param total_sstable_count total sstables across all CFs and levels
 * @param total_data_size_bytes total data size across all CFs
 * @param num_open_sstables number of currently open sstable file handles
 * @param global_seq current global sequence number
 * @param txn_memory_bytes bytes held by in-flight transactions
 * @param compaction_queue_size number of pending compaction tasks
 * @param flush_queue_size number of pending flush tasks in queue
 * @param unified_memtable_enabled whether unified memtable mode is active
 * @param unified_memtable_bytes bytes in unified active memtable
 * @param unified_immutable_count number of unified immutable memtables
 * @param unified_is_flushing whether unified memtable is currently flushing/rotating
 * @param unified_next_cf_index next CF index to be assigned in unified mode
 * @param unified_wal_generation current unified WAL generation counter
 * @param object_store_enabled whether object store mode is active
 * @param object_store_connector connector name ("s3", "gcs", "fs", etc.)
 * @param local_cache_bytes_used current local file cache usage in bytes
 * @param local_cache_bytes_max configured maximum local cache size in bytes
 * @param local_cache_num_files number of files tracked in local cache
 * @param last_uploaded_generation highest WAL generation confirmed uploaded
 * @param upload_queue_depth number of pending upload jobs in the queue
 * @param total_uploads lifetime count of objects uploaded to object store
 * @param total_upload_failures lifetime count of permanently failed uploads (after all retries)
 * @param replica_mode whether running in read-only replica mode
 * @param uwal_bytes_written framed bytes appended to the shared unified WAL (0 if unified off)
 * @param wal_bytes_written per-cf WAL bytes summed across all column families
 * @param flush_bytes_written flush output bytes summed across all column families
 * @param compaction_bytes_written compaction output bytes summed across all column families
 * @param compaction_bytes_read compaction input bytes summed across all column families
 * @param user_bytes_written logical committed bytes summed across all column families
 * @param flush_count flushed sstables summed across all column families
 * @param compaction_count compaction output sstables summed across all column families
 */
typedef struct tidesdb_db_stats_t
{
    int num_column_families;
    uint64_t total_memory;
    uint64_t available_memory;
    size_t resolved_memory_limit;
    int memory_pressure_level;
    int flush_pending_count;
    int64_t total_memtable_bytes;
    int total_immutable_count;
    int total_sstable_count;
    uint64_t total_data_size_bytes;
    int num_open_sstables;
    uint64_t global_seq;
    int64_t txn_memory_bytes;
    size_t compaction_queue_size;
    size_t flush_queue_size;
    int unified_memtable_enabled;
    int64_t unified_memtable_bytes;
    int unified_immutable_count;
    int unified_is_flushing;
    uint32_t unified_next_cf_index;
    uint64_t unified_wal_generation;
    int object_store_enabled;
    const char *object_store_connector;
    size_t local_cache_bytes_used;
    size_t local_cache_bytes_max;
    int local_cache_num_files;
    uint64_t last_uploaded_generation;
    size_t upload_queue_depth;
    uint64_t total_uploads;
    uint64_t total_upload_failures;
    int replica_mode;
    /* write-amplification counters (lifetime since open, on-disk framed bytes). uwal is the
     * shared unified WAL volume (zero when unified mode is off); the remaining fields are
     * summed across all column families. db-wide WA = (uwal + wal + flush + compaction) /
     * user bytes. the *_count fields count output sstables, not logical runs. */
    uint64_t uwal_bytes_written;
    uint64_t wal_bytes_written;
    uint64_t flush_bytes_written;
    uint64_t compaction_bytes_written;
    uint64_t compaction_bytes_read;
    uint64_t user_bytes_written;
    uint64_t flush_count;
    uint64_t compaction_count;
} tidesdb_db_stats_t;

/**
 * tidesdb_default_column_family_config
 * @return default configuration for column family
 */
tidesdb_column_family_config_t tidesdb_default_column_family_config(void);

/**
 * tidesdb_default_config
 * @return default configuration for a database
 */
tidesdb_config_t tidesdb_default_config(void);

/**
 * tidesdb_open
 * opens an existing database or creates a new one
 * @param config database configuration
 * @param db output parameter for database handle
 * @return 0 on success, -n on failure
 */
int tidesdb_open(const tidesdb_config_t *config, tidesdb_t **db);

/**
 * tidesdb_raise_open_file_limit
 * raise this process's open-file ceiling toward `desired` descriptors so a database can keep more
 * sstables open -- the engine sizes max_open_sstables to fit this at open time, so call it BEFORE
 * tidesdb_open. an explicit, opt-in operator action, tidesdb never raises the limit itself. POSIX
 * raises the RLIMIT_NOFILE soft limit toward the hard limit; Windows raises the CRT stdio cap
 * (max 8192). a failed or partial raise is non-fatal -- the prior ceiling stands.
 * @param desired target descriptor count; <= 0 just reports the current ceiling
 * @return the open-file ceiling in effect after the attempt
 */
long tidesdb_raise_open_file_limit(long desired);

/**
 * tidesdb_register_comparator
 * registers a custom comparator function
 * @param db database handle
 * @param name unique name for the comparator (max 63 chars)
 * @param fn comparator function pointer
 * @param ctx_str optional context string for serialization (can be NULL)
 * @param ctx optional runtime context pointer (can be NULL)
 * @return 0 on success, -n on failure (duplicate name, invalid args, etc.)
 */
int tidesdb_register_comparator(tidesdb_t *db, const char *name, skip_list_comparator_fn fn,
                                const char *ctx_str, void *ctx);

/**
 * tidesdb_get_comparator
 * retrieves a registered comparator by name
 * @param db database handle
 * @param name comparator name
 * @param fn output parameter for comparator function (can be NULL)
 * @param ctx output parameter for runtime context pointer (can be NULL)
 * @return 0 on success, -n if not found
 */
int tidesdb_get_comparator(tidesdb_t *db, const char *name, skip_list_comparator_fn *fn,
                           void **ctx);

/**
 * tidesdb_close
 * closes a database
 * @param db database handle
 * @return 0 on success, -n on failure
 */
int tidesdb_close(tidesdb_t *db);

/**
 * tidesdb_promote_to_primary
 * switch a read-only replica to primary mode. performs a final WAL replay
 * and MANIFEST sync, then enables write acceptance.
 * @param db database handle in replica mode
 * @return TDB_SUCCESS on success, TDB_ERR_INVALID_ARGS if not a replica
 */
int tidesdb_promote_to_primary(tidesdb_t *db);

#ifdef TDB_ENABLE_READ_PROFILING
/**
 * tidesdb_get_read_stats
 * gets read profiling statistics
 * @param db the database
 * @param stats output statistics structure
 * @return TDB_SUCCESS on success, error code on failure
 */
int tidesdb_get_read_stats(tidesdb_t *db, tidesdb_read_stats_t *stats);

/**
 * tidesdb_print_read_stats
 * prints read profiling statistics to stdout
 * @param db the database
 */
void tidesdb_print_read_stats(tidesdb_t *db);

/**
 * tidesdb_reset_read_stats
 * resets read profiling statistics
 * @param db the database
 */
void tidesdb_reset_read_stats(tidesdb_t *db);
#endif

/**
 * tidesdb_create_column_family
 * creates a new column family with specified configuration
 * @param db database handle
 * @param name name of column family
 * @param config configuration for column family
 * @return 0 on success, -n on failure
 */
int tidesdb_create_column_family(tidesdb_t *db, const char *name,
                                 const tidesdb_column_family_config_t *config);

/**
 * tidesdb_drop_column_family
 * drops a column family
 * @param db database handle
 * @param name name of column family
 * @return 0 on success, -n on failure
 */
int tidesdb_drop_column_family(tidesdb_t *db, const char *name);

/**
 * tidesdb_delete_column_family
 * drops a column family passing pointer instead of string
 * @param db database handle
 * @param cf column family to drop
 * @return 0 on success, -n on failure
 */
int tidesdb_delete_column_family(tidesdb_t *db, tidesdb_column_family_t *cf);

/**
 * tidesdb_rename_column_family
 * renames a column family safely (flushes pending data first)
 * @param db database handle
 * @param old_name current name of column family
 * @param new_name new name for column family
 * @return 0 on success, -n on failure
 */
int tidesdb_rename_column_family(tidesdb_t *db, const char *old_name, const char *new_name);

/**
 * tidesdb_get_column_family
 * gets a column family from a database
 * @param db database handle
 * @param name name of column family
 * @return pointer to column family, NULL on failure
 */
tidesdb_column_family_t *tidesdb_get_column_family(tidesdb_t *db, const char *name);

/**
 * tidesdb_list_column_families
 * lists all column families in requested database
 * @param db database handle
 * @param names pointer to array of column family names (caller must free each name and the array)
 * @param count pointer to store the number of column families
 * @return 0 on success, -n on failure
 */
int tidesdb_list_column_families(tidesdb_t *db, char ***names, int *count);

/**
 * tidesdb_txn_begin
 * begins a transaction with default isolation level (READ_COMMITTED)
 * @param db database handle
 * @param txn pointer to transaction handle
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_begin(tidesdb_t *db, tidesdb_txn_t **txn);

/**
 * tidesdb_txn_begin_with_isolation
 * begins a transaction with specified isolation level
 * @param db database handle
 * @param isolation isolation level
 * @param txn pointer to transaction handle
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_begin_with_isolation(tidesdb_t *db, tidesdb_isolation_level_t isolation,
                                     tidesdb_txn_t **txn);

/**
 * tidesdb_txn_put
 * adds a write operation to a transaction
 * @param txn transaction handle
 * @param cf column family to put into
 * @param key key to put
 * @param key_size size of key
 * @param value value to put
 * @param value_size size of value
 * @param ttl time-to-live for key-value pair
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_put(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                    size_t key_size, const uint8_t *value, size_t value_size, time_t ttl);

/**
 * tidesdb_txn_get
 * gets a value from a transaction
 * @param txn transaction handle
 * @param cf column family to get from
 * @param key key to get
 * @param key_size size of key
 * @param value pointer to value
 * @param value_size pointer to size of value
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_get(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                    size_t key_size, uint8_t **value, size_t *value_size);

/**
 * tidesdb_txn_delete
 * adds a delete operation to a transaction
 * @param txn transaction handle
 * @param cf column family to delete from
 * @param key key to delete
 * @param key_size size of key
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_delete(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                       size_t key_size);

/**
 * tidesdb_txn_single_delete
 * adds a single-delete operation to a transaction
 *
 * the caller promises that for this key there is at most one put between this
 * single-delete and the previous single-delete (or the beginning). with that
 * promise compaction is free to drop the put and the single-delete together
 * the first merge that sees both, instead of carrying the tombstone forward
 * until the largest level. this dramatically reduces tombstone accumulation
 * for insert-once delete-once workloads and for secondary index maintenance.
 *
 * calling single-delete on a key that has been put more than once since the
 * last single-delete is a contract violation and may expose older values.
 * when in doubt, use tidesdb_txn_delete.
 *
 * for visibility and normal read semantics a single-delete behaves exactly
 * like tidesdb_txn_delete.
 *
 * @param txn transaction handle
 * @param cf column family to delete from
 * @param key key to delete
 * @param key_size size of key
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_single_delete(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                              size_t key_size);

/**
 * tidesdb_txn_rollback
 * rolls back a transaction
 * @param txn transaction handle
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_rollback(tidesdb_txn_t *txn);

/**
 * tidesdb_txn_commit
 * commits a transaction to the database
 *
 * multi-CF atomicity at runtime a transaction is all-or-nothing across all its column
 * families -- a single commit sequence gates visibility, so nothing is visible until the one
 * commit point. crash/failure atomicity differs by memtable mode, UNIFIED mode is crash-atomic
 * across CFs (the whole transaction is one atomic WAL batch), whereas per-CF mode writes a
 * separate WAL per CF, so a crash or IO/OOM failure mid-commit can leave a partially-applied
 * prefix (the CFs written before the failure) that recovery treats as committed. use unified
 * memtable mode when you need crash-atomic multi-CF transactions.
 *
 * @param txn transaction handle
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_commit(tidesdb_txn_t *txn);

/**
 * tidesdb_txn_free
 * frees the transaction
 * @param txn transaction handle
 */
void tidesdb_txn_free(tidesdb_txn_t *txn);

/**
 * tidesdb_txn_reset
 * resets a committed or aborted transaction for reuse without freeing/reallocating buffers
 * keeps the ops array, read set arrays, arenas, cfs array, and savepoints array allocated
 * frees op key/value data, resets read set counts, clears hash tables, frees savepoint children
 * assigns a fresh txn_id and snapshot_seq based on the new isolation level
 * @param txn transaction handle (must be committed or aborted)
 * @param isolation new isolation level for the reset transaction
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_reset(tidesdb_txn_t *txn, tidesdb_isolation_level_t isolation);

/**
 * tidesdb_txn_savepoint
 * creates a savepoint in the transaction
 * @param txn transaction handle
 * @param name name of savepoint
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_savepoint(tidesdb_txn_t *txn, const char *name);

/**
 * tidesdb_txn_rollback_to_savepoint
 * rolls back transaction to a savepoint
 * @param txn transaction handle
 * @param name name of savepoint
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_rollback_to_savepoint(tidesdb_txn_t *txn, const char *name);

/**
 * tidesdb_txn_release_savepoint
 * releases a savepoint without rolling back
 * @param txn transaction handle
 * @param name name of savepoint
 * @return 0 on success, -n on failure
 */
int tidesdb_txn_release_savepoint(tidesdb_txn_t *txn, const char *name);

/**
 * tidesdb_iter_new
 * creates a new iterator for a specific cf in the transaction
 * @param txn transaction handle
 * @param cf column family to iterate
 * @param iter pointer to iterator handle
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_new(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, tidesdb_iter_t **iter);

/**
 * tidesdb_iter_seek
 * seeks to a key in the iterator
 * @param iter iterator handle
 * @param key key to seek to
 * @param key_size size of key
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_seek(tidesdb_iter_t *iter, const uint8_t *key, size_t key_size);

/**
 * tidesdb_iter_seek_for_prev
 * seeks to a previous key in the iterator
 * @param iter iterator handle
 * @param key key to seek to
 * @param key_size size of key
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_seek_for_prev(tidesdb_iter_t *iter, const uint8_t *key, size_t key_size);

/**
 * tidesdb_iter_seek_to_first
 * seeks to the first key in the iterator
 * @param iter iterator handle
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_seek_to_first(tidesdb_iter_t *iter);

/**
 * tidesdb_iter_seek_to_last
 * seeks to the last key in the iterator
 * @param iter iterator handle
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_seek_to_last(tidesdb_iter_t *iter);

/**
 * tidesdb_iter_next
 * seeks to a next key in the iterator
 * @param iter iterator handle
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_next(tidesdb_iter_t *iter);

/**
 * tidesdb_iter_prev
 * seeks to a previous key in the iterator
 * @param iter iterator handle
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_prev(tidesdb_iter_t *iter);

/**
 * tidesdb_iter_valid
 * checks if an iterator is valid
 * @param iter iterator handle
 * @return non-zero if valid, 0 if invalid
 */
int tidesdb_iter_valid(tidesdb_iter_t *iter);

/**
 * tidesdb_iter_key
 * gets a key from an iterator
 * @param iter iterator handle
 * @param key pointer to key
 * @param key_size pointer to size of key
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_key(tidesdb_iter_t *iter, uint8_t **key, size_t *key_size);

/**
 * tidesdb_iter_value
 * gets a value from an iterator
 * @param iter iterator handle
 * @param value pointer to value
 * @param value_size pointer to size of value
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_value(tidesdb_iter_t *iter, uint8_t **value, size_t *value_size);

/**
 * tidesdb_iter_key_value
 * gets both key and value from an iterator in a single call
 * @param iter iterator handle
 * @param key pointer to key
 * @param key_size pointer to size of key
 * @param value pointer to value
 * @param value_size pointer to size of value
 * @return 0 on success, -n on failure
 */
int tidesdb_iter_key_value(tidesdb_iter_t *iter, uint8_t **key, size_t *key_size, uint8_t **value,
                           size_t *value_size);

/**
 * tidesdb_iter_free
 * frees an iterator
 * @param iter iterator handle
 */
void tidesdb_iter_free(tidesdb_iter_t *iter);

/**
 * tidesdb_comparator_memcmp
 * binary comparison using memcmp (default)
 * compares keys byte-by-byte
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx unused context
 * @return <0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
int tidesdb_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                              size_t key2_size, void *ctx);

/**
 * tidesdb_comparator_lexicographic
 * lexicographic string comparison
 * treats keys as null-terminated strings
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx unused context
 * @return <0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
int tidesdb_comparator_lexicographic(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                     size_t key2_size, void *ctx);

/**
 * tidesdb_comparator_uint64
 * compares keys as 64-bit unsigned integers (little-endian)
 * keys must be exactly 8 bytes
 * @param key1 first key (8 bytes)
 * @param key1_size size of first key (must be 8)
 * @param key2 second key (8 bytes)
 * @param key2_size size of second key (must be 8)
 * @param ctx unused context
 * @return <0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
int tidesdb_comparator_uint64(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                              size_t key2_size, void *ctx);

/**
 * tidesdb_comparator_int64
 * compares keys as 64-bit signed integers (little-endian)
 * keys must be exactly 8 bytes
 * @param key1 first key (8 bytes)
 * @param key1_size size of first key (must be 8)
 * @param key2 second key (8 bytes)
 * @param key2_size size of second key (must be 8)
 * @param ctx unused context
 * @return <0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
int tidesdb_comparator_int64(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                             size_t key2_size, void *ctx);

/**
 * tidesdb_comparator_reverse_memcmp
 * reverse binary comparison (descending order)
 * useful for reverse-sorted indexes
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx unused context
 * @return >0 if key1 < key2, 0 if equal, <0 if key1 > key2
 */
int tidesdb_comparator_reverse_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                      size_t key2_size, void *ctx);

/**
 * tidesdb_comparator_case_insensitive
 * case-insensitive string comparison
 * treats keys as ASCII strings
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx unused context
 * @return <0 if key1 < key2, 0 if equal, >0 if key1 > key2
 */
int tidesdb_comparator_case_insensitive(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                        size_t key2_size, void *ctx);

/**
 * tidesdb_cf_set_commit_hook
 * sets or clears the commit hook for a column family at runtime
 * pass NULL for fn to disable the hook
 * @param cf column family handle
 * @param fn commit hook callback (or NULL to disable)
 * @param ctx user-provided context passed to the callback
 * @return TDB_SUCCESS on success, TDB_ERR_INVALID_ARGS if cf is NULL
 */
int tidesdb_cf_set_commit_hook(tidesdb_column_family_t *cf, tidesdb_commit_hook_fn fn, void *ctx);

/**
 * tidesdb_compact
 * runs a full compaction on a column family. every active level is merged
 * into the largest so all garbage (tombstones, single-delete pairs,
 * superseded puts) is reclaimed; with a single disk level the merge is a
 * self-rewrite of that level. blocks until the work item has been
 * serviced, including any compaction already in flight on this cf
 * @param cf column family handle
 * @return 0 on success, -n on failure
 */
int tidesdb_compact(tidesdb_column_family_t *cf);

/**
 * tidesdb_compact_range
 * synchronously compacts every sstable in the column family whose [min_key, max_key]
 * overlaps the caller supplied [start_key, end_key) range. output is merged toward the
 * largest level affected by the input set, so any tombstones in the range that meet
 * their dead puts are dropped during this pass. the caller blocks until the merge
 * completes. intended for bulk reclaim after large range deletes -- emit point
 * tombstones with tidesdb_txn_delete, then call this to physically merge them out.
 *
 * NULL start_key means unbounded low, NULL end_key means unbounded high. both NULL
 * is rejected with TDB_ERR_INVALID_ARGS so callers go through tidesdb_compact for
 * full cf compaction.
 *
 * @param cf column family handle
 * @param start_key inclusive range start (NULL = unbounded low)
 * @param start_key_size size of start_key in bytes (0 if start_key is NULL)
 * @param end_key exclusive range end (NULL = unbounded high)
 * @param end_key_size size of end_key in bytes (0 if end_key is NULL)
 * @return TDB_SUCCESS on success, TDB_ERR_INVALID_ARGS for bad args, TDB_ERR_LOCKED
 *         if another compaction is already running, or other error codes from the
 *         underlying merge
 */
int tidesdb_compact_range(tidesdb_column_family_t *cf, const uint8_t *start_key,
                          size_t start_key_size, const uint8_t *end_key, size_t end_key_size);

/**
 * tidesdb_flush_memtable
 * flushes a column family's memtable to disk (sorted run to level 1)
 * @param cf column family handle
 * @return 0 on success, -n on failure
 */
int tidesdb_flush_memtable(tidesdb_column_family_t *cf);

/**
 * tidesdb_is_flushing
 * checks if a column family is currently flushing
 * @param cf column family handle
 * @return 1 if flushing, 0 if not flushing
 */
int tidesdb_is_flushing(tidesdb_column_family_t *cf);

/**
 * tidesdb_is_compacting
 * checks if a column family is currently compacting
 * @param cf column family handle
 * @return 1 if compacting, 0 if not compacting
 */
int tidesdb_is_compacting(tidesdb_column_family_t *cf);

/**
 * tidesdb_cf_config_load_from_ini
 * loads the column family configuration from an INI file
 * @param ini_file INI file path
 * @param section_name section name in INI file
 * @param config pointer to column family configuration
 * @return 0 on success, -n on failure
 */
int tidesdb_cf_config_load_from_ini(const char *ini_file, const char *section_name,
                                    tidesdb_column_family_config_t *config);

/**
 * tidesdb_cf_config_save_to_ini
 * saves a column family configuration to an INI file (column family config)
 * @param ini_file INI file path
 * @param section_name section name in INI file
 * @param config pointer to column family configuration
 * @return 0 on success, -n on failure
 */
int tidesdb_cf_config_save_to_ini(const char *ini_file, const char *section_name,
                                  const tidesdb_column_family_config_t *config);

/**
 * tidesdb_cf_update_runtime_config
 * updates the runtime configuration of a column family
 * @param cf column family handle
 * @param new_config new configuration
 * @param persist_to_disk whether to persist the configuration to disk
 * @return 0 on success, -n on failure
 */
int tidesdb_cf_update_runtime_config(tidesdb_column_family_t *cf,
                                     const tidesdb_column_family_config_t *new_config,
                                     int persist_to_disk);

/**
 * tidesdb_get_stats
 * gets the statistics of a column family
 * @param cf column family handle
 * @param stats pointer to statistics
 * @return 0 on success, -n on failure
 */
int tidesdb_get_stats(tidesdb_column_family_t *cf, tidesdb_stats_t **stats);

/**
 * tidesdb_free_stats
 * frees the statistics of the column family
 * @param stats statistics
 */
void tidesdb_free_stats(tidesdb_stats_t *stats);

/**
 * tidesdb_get_db_stats
 * gets database-level statistics (memory, pressure, queues, totals across all CFs)
 * @param db database handle
 * @param stats output parameter for database statistics (caller provides pointer to struct)
 * @return 0 on success, -n on failure
 */
int tidesdb_get_db_stats(tidesdb_t *db, tidesdb_db_stats_t *stats);

/**
 * tidesdb_get_cache_stats
 * gets block cache statistics for the database
 * @param db database handle
 * @param stats output parameter for cache statistics
 * @return 0 on success, -n on failure
 * @note if block cache is disabled, stats->enabled will be 0 and other fields will be zero
 */
int tidesdb_get_cache_stats(tidesdb_t *db, tidesdb_cache_stats_t *stats);

/**
 * tidesdb_backup
 * backup current database to a directory. this is a best effort backup that copies immutable files
 * first, then forces a sorted run, waits for the flush/compaction queues to drain, and performs a
 * final copy to pick up wal's and the manifest while skipping already copied sstable files.
 * @param db database handle
 * @param dir destination directory for the backup
 * @return 0 on success, -n on failure
 */
int tidesdb_backup(tidesdb_t *db, char *dir);

/**
 * tidesdb_checkpoint
 * creates a lightweight checkpoint of the database using hard links for sstable files.
 * this is much faster than a full backup since sstable files (which are immutable) are
 * hard-linked rather than copied. only small metadata files (manifest, config) are copied.
 *
 * the checkpoint is a fully openable tidesdb database directory.
 *
 * algorithm:
 *   1. for each column family -- we flush memtable, halt compactions
 *   2. hard link all live sstable files into the checkpoint directory
 *   3. copy manifest and config files
 *   4. resume compactions
 *
 * if hard linking fails (e.g., cross-filesystem), falls back to file copy.
 *
 * @param db database handle
 * @param checkpoint_dir destination directory for the checkpoint (must not exist or be empty)
 * @return 0 on success, -n on failure
 */
int tidesdb_checkpoint(tidesdb_t *db, const char *checkpoint_dir);

/**
 * tidesdb_clone_column_family
 * clones an existing column family to a new column family with a different name.
 * flushes the source memtable, waits for background operations, copies all sstable files,
 * and creates a new column family structure with the copied data.
 * @param db database handle
 * @param src_name name of the source column family to clone
 * @param dst_name name for the new cloned column family
 * @return TDB_SUCCESS on success, TDB_ERR_NOT_FOUND if source doesn't exist,
 *         TDB_ERR_EXISTS if destination already exists, or other error codes on failure
 */
int tidesdb_clone_column_family(tidesdb_t *db, const char *src_name, const char *dst_name);

/**
 * tidesdb_purge_cf
 * forces a full flush of the active memtable and triggers aggressive compaction for a column
 * family. waits for all flush and compaction I/O to complete before returning. this is useful for
 * manual maintenance, pre-backup preparation, or reclaiming space after bulk deletes.
 * @param cf column family handle
 * @return 0 on success, -n on failure
 */
int tidesdb_purge_cf(tidesdb_column_family_t *cf);

/**
 * tidesdb_purge
 * forces a full flush and aggressive compaction for all column families.
 * waits for all flush and compaction queues to fully drain before returning.
 * @param db database handle
 * @return 0 on success, first non-zero error code on failure (continues processing remaining CFs)
 */
int tidesdb_purge(tidesdb_t *db);

/**
 * tidesdb_cancel_background_work
 * cancels background compaction db-wide this means in-flight merges bail at their next
 * checkpoint (uncommitted output is discarded, inputs left intact -- recovery-safe)
 * and queued compaction work is skipped. flushes are unaffected so durability is
 * preserved. blocks (bounded) until compaction is idle. the cancel is sticky for
 * this database session and is reset on the next tidesdb_open, so it is intended to
 * be called immediately before tidesdb_close for a fast shutdown when a large
 * compaction backlog would otherwise make close wait minutes to seconds.
 * @param db database handle
 * @return TDB_SUCCESS, or TDB_ERR_INVALID_ARGS if db is NULL
 */
int tidesdb_cancel_background_work(tidesdb_t *db);

/**
 * tidesdb_range_cost
 * estimate the computational cost of iterating between two keys in a column family.
 * the returned cost is an opaque double -- meaningful only for comparison with other
 * values from the same function. uses only in-memory metadata (block indexes, sstable
 * min/max keys, entry counts); performs no disk I/O and no iteration.
 *
 * when block indexes are enabled, cost is estimated via O(log B) binary search per
 * overlapping sstable. when block indexes are disabled, a byte-level key interpolation
 * fallback is used instead.
 *
 * @param cf column family
 * @param key_a first key (bound of range)
 * @param key_a_size size of first key
 * @param key_b second key (bound of range)
 * @param key_b_size size of second key
 * @param cost output -- estimated traversal cost (higher = more expensive)
 * @return TDB_SUCCESS on success, TDB_ERR_INVALID_ARGS on bad input
 */
int tidesdb_range_cost(tidesdb_column_family_t *cf, const uint8_t *key_a, size_t key_a_size,
                       const uint8_t *key_b, size_t key_b_size, double *cost);

/**
 * tidesdb_sync_wal
 * forces an fsync of the active WAL for a column family.
 * useful for explicit durability control when using TDB_SYNC_NONE or TDB_SYNC_INTERVAL modes.
 * @param cf column family handle
 * @return 0 on success, -n on failure
 */
int tidesdb_sync_wal(tidesdb_column_family_t *cf);

/**
 * tidesdb_free
 * frees a pointer allocated by TidesDB
 * @param ptr pointer to free
 */
void tidesdb_free(void *ptr);

#endif /* __TIDESDB_H__ */
