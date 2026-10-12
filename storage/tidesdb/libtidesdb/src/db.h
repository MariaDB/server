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
#ifndef __TIDESDB_DB_H__
#define __TIDESDB_DB_H__

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/**
 * tidesdb_objstore_backend_t
 * identifies the object store backend in use
 */
typedef enum
{
    TDB_BACKEND_FS = 0,
    TDB_BACKEND_S3 = 1,
    TDB_BACKEND_UNKNOWN = 99
} tidesdb_objstore_backend_t;

/** opaque types for FFI bindings (Java, etc.) */
struct tidesdb_t
{
    int _opaque;
};
struct tidesdb_column_family_t
{
    int _opaque;
};
struct tidesdb_txn_t
{
    int _opaque;
};
struct tidesdb_iter_t
{
    int _opaque;
};
struct tidesdb_objstore_t
{
    int _opaque;
};

typedef struct tidesdb_t tidesdb_t;
typedef struct tidesdb_column_family_t tidesdb_column_family_t;
typedef struct tidesdb_txn_t tidesdb_txn_t;
typedef struct tidesdb_iter_t tidesdb_iter_t;
typedef struct tidesdb_objstore_t tidesdb_objstore_t;

/**
 * tidesdb_objstore_config_t
 * configuration for object store mode behavior
 * @param local_cache_path local directory for cached sstable files (NULL = use db_path)
 * @param local_cache_max_bytes max local cache size in bytes (0 = unlimited)
 * @param cache_on_read cache downloaded files locally (default 1)
 * @param cache_on_write keep local copy after upload (default 1)
 * @param max_concurrent_uploads parallel upload threads (default 4)
 * @param max_concurrent_downloads parallel download threads (default 8)
 * @param multipart_threshold use multipart upload above this size (default 64MB)
 * @param multipart_part_size multipart chunk size (default 8MB)
 * @param sync_manifest_to_object upload MANIFEST after each compaction (default 1)
 * @param replicate_wal upload closed WAL segments for node-failure recovery (default 1)
 * @param wal_upload_sync 0 = background WAL upload (default), 1 = block flush until uploaded
 * @param wal_sync_threshold_bytes sync active WAL when it grows by this many bytes (default 1MB, 0
 * = off)
 * @param wal_sync_on_commit upload WAL after every txn commit for RPO=0 replication (default 0)
 * @param replica_mode enable read-only replica mode (default 0)
 * @param replica_sync_interval_us MANIFEST poll interval in microseconds (default 5000000)
 * @param replica_replay_wal replay WAL for near-real-time reads on replicas (default 1)
 */
typedef struct
{
    const char *local_cache_path;
    size_t local_cache_max_bytes;
    int cache_on_read;
    int cache_on_write;
    int max_concurrent_uploads;
    int max_concurrent_downloads;
    size_t multipart_threshold;
    size_t multipart_part_size;
    int sync_manifest_to_object;
    int replicate_wal;
    int wal_upload_sync;
    size_t wal_sync_threshold_bytes;
    int wal_sync_on_commit;
    int replica_mode;
    uint64_t replica_sync_interval_us;
    int replica_replay_wal;
} tidesdb_objstore_config_t;

tidesdb_objstore_config_t tidesdb_objstore_default_config(void);

/** debug logging levels */
typedef enum
{
    TDB_LOG_DEBUG = 0,
    TDB_LOG_INFO = 1,
    TDB_LOG_WARN = 2,
    TDB_LOG_ERROR = 3,
    TDB_LOG_FATAL = 4,
    TDB_LOG_NONE = 99
} tidesdb_log_level_t;

/** txn isolation levels */
typedef enum
{
    TDB_ISOLATION_READ_UNCOMMITTED = 0,
    TDB_ISOLATION_READ_COMMITTED = 1,
    TDB_ISOLATION_REPEATABLE_READ = 2,
    TDB_ISOLATION_SNAPSHOT = 3,
    TDB_ISOLATION_SERIALIZABLE = 4
} tidesdb_isolation_level_t;

/** compression algorithms */
/* ABI/on-disk contract, these numeric values are persisted in sstable/vlog metadata and are
 * duplicated in compress.h. the two copies MUST stay identical -- compress.c _Static_asserts the
 * compress.h copy; keep this copy in lockstep. every enumerator is always defined; whether a
 * backend can actually be used is a build-time choice (the -DTIDESDB_WITH_* options), queryable at
 * runtime via tidesdb_compression_available. */
typedef enum
{
    TDB_COMPRESS_NONE = 0,
    TDB_COMPRESS_SNAPPY = 1,
    TDB_COMPRESS_LZ4 = 2,
    TDB_COMPRESS_ZSTD = 3,
    TDB_COMPRESS_LZ4_FAST = 4
} compression_algorithm;

/** column family sync modes */
typedef enum
{
    TDB_SYNC_NONE = 0,
    TDB_SYNC_FULL = 1,
    TDB_SYNC_INTERVAL = 2
} tidesdb_sync_mode_t;

/** system error codes */
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
#define TDB_ERR_BUSY         -14

/** configuration limits */
#define TDB_MAX_COMPARATOR_NAME 64
#define TDB_MAX_COMPARATOR_CTX  256
#define TDB_MAX_CF_NAME_LEN     128

/** comparator function type.
 * ABI contract -- this MUST stay structurally identical to skip_list_comparator_fn in
 * skip_list.h. tidesdb_register_comparator / tidesdb_get_comparator are declared with this
 * type in the FFI header and with skip_list_comparator_fn internally, so the two function
 * pointer signatures have to match exactly or an FFI caller passes an incompatible callback. */
typedef int (*tidesdb_comparator_fn)(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                     size_t key2_size, void *ctx);

/**
 * tidesdb_commit_op_t
 * represents a single operation in a committed transaction batch
 * passed to the commit hook callback
 * @param key pointer to key data (valid only during callback invocation)
 * @param key_size size of key in bytes
 * @param value pointer to value data (NULL for deletes, valid only during callback invocation)
 * @param value_size size of value in bytes (0 for deletes)
 * @param ttl time-to-live for the key-value pair (0 = no expiry)
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
 * callback function invoked synchronously after a transaction commits to a column family
 * the callback receives the full batch of operations for that CF atomically
 * the hook fires after WAL write, memtable apply, and commit status marking are complete
 * hook failure is logged but does not roll back the commit (data is already durable)
 *
 * @param ops array of committed operations (valid only during callback invocation)
 * @param num_ops number of operations in the array
 * @param commit_seq monotonic commit sequence number
 * @param ctx user-provided context pointer
 * @return 0 on success, non-zero on failure (logged as warning)
 */
typedef int (*tidesdb_commit_hook_fn)(const tidesdb_commit_op_t *ops, int num_ops,
                                      uint64_t commit_seq, void *ctx);

/**
 * tidesdb_column_family_config_t
 * configuration for a column family
 * @param name name of column family
 * @param write_buffer_size size of write buffer
 * @param level_size_ratio ratio of level sizes
 * @param min_levels minimum number of levels
 * @param dividing_level_offset offset for dividing level
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
 * @param use_btree whether btree is used
 * @param commit_hook_fn optional commit hook callback (NULL = disabled, runtime-only)
 * @param commit_hook_ctx optional user context passed to commit hook (runtime-only)
 * @param object_target_file_size reserved for API compatibility, not used.. will be retired
 * completely
 * @param object_lazy_compaction 1 = compact less aggressively in object store mode (default 0)
 * @param object_prefetch_compaction 1 = download all inputs before merge (default 1)
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
    void *comparator_fn_cached;
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
 * tidesdb_config_t
 * configuration for the database
 * @param db_path path to the database
 * @param num_flush_threads number of flush threads
 * @param num_compaction_threads number of compaction threads
 * @param log_level minimum log level to display (TDB_LOG_DEBUG, TDB_LOG_INFO, TDB_LOG_WARN,
 * TDB_LOG_ERROR, TDB_LOG_FATAL, TDB_LOG_NONE)
 * @param block_cache_size size of clock cache for hot sstable blocks
 * @param max_open_sstables maximum number of open sstables
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
 * @param object_store pluggable object store connector (NULL = local only, default)
 * @param object_store_config object store behavior configuration (NULL = use defaults)
 * @param max_concurrent_flushes global semaphore on the number of in-flight memtable flushes
 *                               across all column families. bounds total transient memory and
 *                               work-queue depth when many column families flush at once.
 *                               0 falls back to TDB_DEFAULT_MAX_CONCURRENT_FLUSHES.
 * @param finish_compactions_on_close close behavior. 0 (default) cancels in-flight compactions at
 *                               their next checkpoint for a fast shutdown -- the merge discards its
 *                               uncommitted output and leaves inputs intact, so no data is lost
 *                               (recovery handles a mid-merge state the same way). 1 lets in-flight
 *                               compactions run to completion before close returns.
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
    int finish_compactions_on_close;
} tidesdb_config_t;

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
 * @param use_btree whether btree is used
 * @param btree_total_nodes total number of nodes in btree
 * @param btree_max_height maximum height of btree
 * @param btree_avg_height average height of btree
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
typedef struct tidesdb_stats_t
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
    int use_btree;
    uint64_t btree_total_nodes;
    uint32_t btree_max_height;
    double btree_avg_height;
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
} tidesdb_stats_t;

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

/**** system default configuration functions */
tidesdb_column_family_config_t tidesdb_default_column_family_config(void);
tidesdb_config_t tidesdb_default_config(void);

/**
 * tidesdb_raise_open_file_limit
 * raise this process's open-file ceiling toward `desired` descriptors so a database can keep more
 * sstables open -- the engine sizes max_open_sstables to fit this at open time, so call it BEFORE
 * tidesdb_open. an explicit, opt-in operator action, tidesdb never raises the limit itself. POSIX
 * (Linux, macOS, the BSDs, illumos) raises the RLIMIT_NOFILE soft limit toward the hard limit;
 * Windows raises the CRT stdio cap (max 8192). a failed or partial raise is non-fatal.
 * @param desired target descriptor count; <= 0 just reports the current ceiling
 * @return the open-file ceiling in effect after the attempt
 */
long tidesdb_raise_open_file_limit(long desired);

/**** initialization and custom allocator support */

/**
 * tidesdb_malloc_fn
 * function pointer type for malloc-like allocation
 * @param size number of bytes to allocate
 * @return pointer to allocated memory or NULL on failure
 */
typedef void *(*tidesdb_malloc_fn)(size_t size);

/**
 * tidesdb_calloc_fn
 * function pointer type for calloc-like allocation
 * @param count number of elements to allocate
 * @param size size of each element in bytes
 * @return pointer to zero-initialized memory or NULL on failure
 */
typedef void *(*tidesdb_calloc_fn)(size_t count, size_t size);

/**
 * tidesdb_realloc_fn
 * function pointer type for realloc-like reallocation
 * @param ptr pointer to previously allocated memory (or NULL)
 * @param size new size in bytes
 * @return pointer to reallocated memory or NULL on failure
 */
typedef void *(*tidesdb_realloc_fn)(void *ptr, size_t size);

/**
 * tidesdb_free_fn
 * function pointer type for free-like deallocation
 * @param ptr pointer to memory to free (may be NULL)
 */
typedef void (*tidesdb_free_fn)(void *ptr);

/**
 * tidesdb_init
 * initializes TidesDB with optional custom memory allocation functions
 * MUST be called exactly once before any other TidesDB function
 * pass NULL for any function to use the default system allocator
 *
 * Example (Redis module):
 *   tidesdb_init(RedisModule_Alloc, RedisModule_Calloc,
 *                RedisModule_Realloc, RedisModule_Free);
 *
 * Example (system allocator):
 *   tidesdb_init(NULL, NULL, NULL, NULL);
 *
 * @param malloc_fn custom malloc function (or NULL for system malloc)
 * @param calloc_fn custom calloc function (or NULL for system calloc)
 * @param realloc_fn custom realloc function (or NULL for system realloc)
 * @param free_fn custom free function (or NULL for system free)
 * @return 0 on success, -1 if already initialized
 */
int tidesdb_init(tidesdb_malloc_fn malloc_fn, tidesdb_calloc_fn calloc_fn,
                 tidesdb_realloc_fn realloc_fn, tidesdb_free_fn free_fn);

/**
 * tidesdb_finalize
 * finalizes TidesDB and resets the allocator
 * should be called after all TidesDB operations are complete
 * after calling this, tidesdb_init() can be called again
 */
void tidesdb_finalize(void);

/**** database operations */
int tidesdb_open(const tidesdb_config_t *config, tidesdb_t **db);
int tidesdb_close(tidesdb_t *db);

/**** comparator operations */
int tidesdb_register_comparator(tidesdb_t *db, const char *name, tidesdb_comparator_fn fn,
                                const char *ctx_str, void *ctx);
int tidesdb_get_comparator(tidesdb_t *db, const char *name, tidesdb_comparator_fn *fn, void **ctx);

/**** column family operations */
int tidesdb_create_column_family(tidesdb_t *db, const char *name,
                                 const tidesdb_column_family_config_t *config);
int tidesdb_drop_column_family(tidesdb_t *db, const char *name);
int tidesdb_delete_column_family(tidesdb_t *db, tidesdb_column_family_t *cf);

/**
 * tidesdb_rename_column_family
 * atomically renames a column family and its underlying directory
 * waits for any in-progress flush/compaction to complete before renaming
 * @param db database handle
 * @param old_name current name of the column family
 * @param new_name new name for the column family
 * @return TDB_SUCCESS, TDB_ERR_NOT_FOUND, TDB_ERR_EXISTS, or TDB_ERR_IO
 */
int tidesdb_rename_column_family(tidesdb_t *db, const char *old_name, const char *new_name);
tidesdb_column_family_t *tidesdb_get_column_family(tidesdb_t *db, const char *name);
int tidesdb_list_column_families(tidesdb_t *db, char ***names, int *count);

/**** transaction operations */
int tidesdb_txn_begin(tidesdb_t *db, tidesdb_txn_t **txn);
int tidesdb_txn_begin_with_isolation(tidesdb_t *db, tidesdb_isolation_level_t isolation,
                                     tidesdb_txn_t **txn);
int tidesdb_txn_put(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                    size_t key_size, const uint8_t *value, size_t value_size, time_t ttl);
int tidesdb_txn_get(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                    size_t key_size, uint8_t **value, size_t *value_size);
int tidesdb_txn_delete(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                       size_t key_size);
int tidesdb_txn_single_delete(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, const uint8_t *key,
                              size_t key_size);
int tidesdb_txn_commit(tidesdb_txn_t *txn);
int tidesdb_txn_rollback(tidesdb_txn_t *txn);
int tidesdb_txn_reset(tidesdb_txn_t *txn, tidesdb_isolation_level_t isolation);
void tidesdb_txn_free(tidesdb_txn_t *txn);

/**** savepoint operations */
int tidesdb_txn_savepoint(tidesdb_txn_t *txn, const char *name);
int tidesdb_txn_rollback_to_savepoint(tidesdb_txn_t *txn, const char *name);
int tidesdb_txn_release_savepoint(tidesdb_txn_t *txn, const char *name);

/**** iterator operations */
int tidesdb_iter_new(tidesdb_txn_t *txn, tidesdb_column_family_t *cf, tidesdb_iter_t **iter);
int tidesdb_iter_seek(tidesdb_iter_t *iter, const uint8_t *key, size_t key_size);
int tidesdb_iter_seek_for_prev(tidesdb_iter_t *iter, const uint8_t *key, size_t key_size);
int tidesdb_iter_seek_to_first(tidesdb_iter_t *iter);
int tidesdb_iter_seek_to_last(tidesdb_iter_t *iter);
int tidesdb_iter_next(tidesdb_iter_t *iter);
int tidesdb_iter_prev(tidesdb_iter_t *iter);
int tidesdb_iter_valid(tidesdb_iter_t *iter);
int tidesdb_iter_key(tidesdb_iter_t *iter, uint8_t **key, size_t *key_size);
int tidesdb_iter_value(tidesdb_iter_t *iter, uint8_t **value, size_t *value_size);
void tidesdb_iter_free(tidesdb_iter_t *iter);

/**** comparator functions */
int tidesdb_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                              size_t key2_size, void *ctx);
int tidesdb_comparator_lexicographic(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                     size_t key2_size, void *ctx);
int tidesdb_comparator_uint64(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                              size_t key2_size, void *ctx);
int tidesdb_comparator_int64(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                             size_t key2_size, void *ctx);
int tidesdb_comparator_reverse_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                      size_t key2_size, void *ctx);
int tidesdb_comparator_case_insensitive(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                        size_t key2_size, void *ctx);

/**** commit hook operations */

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

/**** maintenance operations */
int tidesdb_compact(tidesdb_column_family_t *cf);

/**
 * tidesdb_compact_range
 * synchronously compacts every sstable whose key range overlaps [start_key, end_key).
 * output is merged toward the largest level affected. NULL endpoints are unbounded.
 * both NULL is rejected so callers go through tidesdb_compact for full cf compaction.
 * @return TDB_SUCCESS, TDB_ERR_INVALID_ARGS for bad args, TDB_ERR_LOCKED if another
 *         compaction is running, or other error codes from the underlying merge
 */
int tidesdb_compact_range(tidesdb_column_family_t *cf, const uint8_t *start_key,
                          size_t start_key_size, const uint8_t *end_key, size_t end_key_size);

int tidesdb_flush_memtable(tidesdb_column_family_t *cf);

/**
 * tidesdb_is_flushing
 * check if a column family has a flush operation in progress
 * @param cf column family handle
 * @return 1 if flushing, 0 otherwise
 */
int tidesdb_is_flushing(tidesdb_column_family_t *cf);

/**
 * tidesdb_is_compacting
 * check if a column family has a compaction operation in progress
 * @param cf column family handle
 * @return 1 if compacting, 0 otherwise
 */
int tidesdb_is_compacting(tidesdb_column_family_t *cf);
int tidesdb_backup(tidesdb_t *db, char *dir);
int tidesdb_checkpoint(tidesdb_t *db, const char *checkpoint_dir);

/**
 * tidesdb_clone_column_family
 * clones an existing column family to a new column family with a different name
 * @param db database handle
 * @param src_name name of the source column family to clone
 * @param dst_name name for the new cloned column family
 * @return TDB_SUCCESS, TDB_ERR_NOT_FOUND, TDB_ERR_EXISTS, or other error codes
 */
int tidesdb_clone_column_family(tidesdb_t *db, const char *src_name, const char *dst_name);

/**
 * tidesdb_purge_cf
 * forces a full flush of the active memtable and triggers aggressive compaction for a column
 * family. waits for all flush and compaction I/O to complete before returning.
 * @param cf column family handle
 * @return 0 on success, -n on failure
 */
int tidesdb_purge_cf(tidesdb_column_family_t *cf);

/**
 * tidesdb_purge
 * forces a full flush and aggressive compaction for all column families.
 * waits for all flush and compaction queues to fully drain before returning.
 * @param db database handle
 * @return 0 on success, first non-zero error code on failure
 */
int tidesdb_purge(tidesdb_t *db);

/**
 * tidesdb_cancel_background_work
 * cancels background compaction db-wide (in-flight merges bail safely, queued
 * compaction is skipped); flushes are unaffected so durability is preserved. blocks
 * (bounded) until compaction is idle. sticky for the session, reset on next open --
 * intended to be called right before tidesdb_close for a fast shutdown.
 * @param db database handle
 * @return TDB_SUCCESS, or TDB_ERR_INVALID_ARGS if db is NULL
 */
int tidesdb_cancel_background_work(tidesdb_t *db);

/**** configuration operations */
int tidesdb_cf_config_load_from_ini(const char *ini_file, const char *section_name,
                                    tidesdb_column_family_config_t *config);
int tidesdb_cf_config_save_to_ini(const char *ini_file, const char *section_name,
                                  const tidesdb_column_family_config_t *config);
int tidesdb_cf_update_runtime_config(tidesdb_column_family_t *cf,
                                     const tidesdb_column_family_config_t *new_config,
                                     int persist_to_disk);

/**** statistics operations */
int tidesdb_get_stats(tidesdb_column_family_t *cf, tidesdb_stats_t **stats);
void tidesdb_free_stats(tidesdb_stats_t *stats);
int tidesdb_get_db_stats(tidesdb_t *db, tidesdb_db_stats_t *stats);
int tidesdb_get_cache_stats(tidesdb_t *db, tidesdb_cache_stats_t *stats);

int tidesdb_range_cost(tidesdb_column_family_t *cf, const uint8_t *key_a, size_t key_a_size,
                       const uint8_t *key_b, size_t key_b_size, double *cost);

void tidesdb_free(void *ptr);

int tidesdb_sync_wal(tidesdb_column_family_t *cf);

/**** object store connector factories */

/**
 * tidesdb_objstore_fs_create
 * create a filesystem-backed object store connector for testing and local replication
 * stores objects as files under root_dir mirroring the key path structure
 * @param root_dir directory to store objects in
 * @return connector handle or NULL on error
 */
tidesdb_objstore_t *tidesdb_objstore_fs_create(const char *root_dir);

/**
 * tidesdb_objstore_s3_create
 * create an S3-compatible object store connector (AWS S3, MinIO, etc.).
 * the library must have been built with TIDESDB_WITH_S3=ON; otherwise the
 * symbol is unresolved at link time.
 * @param endpoint       S3 endpoint (e.g. "s3.amazonaws.com" or "minio.local:9000")
 * @param bucket         bucket name
 * @param prefix         key prefix (e.g. "production/db1/"), may be NULL
 * @param access_key     AWS access key ID
 * @param secret_key     AWS secret access key
 * @param region         AWS region (e.g. "us-east-1"), NULL for MinIO
 * @param use_ssl        1 for HTTPS, 0 for HTTP
 * @param use_path_style 1 for path-style URLs (MinIO), 0 for virtual-hosted (AWS)
 * @return connector handle, or NULL on error
 */
tidesdb_objstore_t *tidesdb_objstore_s3_create(const char *endpoint, const char *bucket,
                                               const char *prefix, const char *access_key,
                                               const char *secret_key, const char *region,
                                               int use_ssl, int use_path_style);

/**
 * tidesdb_objstore_s3_config_t
 * full S3 connector configuration, including TLS and multipart tuning the positional
 * tidesdb_objstore_s3_create cannot express. zero-initialize and set what you need; the
 * all-zero defaults are secure (TLS verify on, no custom CA) and use the built-in multipart
 * sizes.
 * @param endpoint S3 endpoint (required)
 * @param bucket bucket name (required)
 * @param prefix key prefix, or NULL
 * @param access_key AWS access key ID (required)
 * @param secret_key AWS secret access key (required)
 * @param region AWS region, or NULL for the default
 * @param use_ssl 1 for HTTPS, 0 for HTTP
 * @param use_path_style 1 for path-style URLs (MinIO), 0 for virtual-hosted (AWS)
 * @param tls_ca_path custom CA bundle file path, or NULL for the system bundle
 * @param tls_insecure_skip_verify 1 disables TLS peer+host verification (test only, insecure);
 *                                 0 keeps verification on (default)
 * @param multipart_threshold object size at/above which multipart upload is used; 0 = default
 * @param multipart_part_size multipart chunk size in bytes; 0 = default
 */
typedef struct
{
    const char *endpoint;
    const char *bucket;
    const char *prefix;
    const char *access_key;
    const char *secret_key;
    const char *region;
    int use_ssl;
    int use_path_style;
    const char *tls_ca_path;
    int tls_insecure_skip_verify;
    size_t multipart_threshold;
    size_t multipart_part_size;
} tidesdb_objstore_s3_config_t;

/**
 * tidesdb_objstore_s3_create_config
 * create an S3-compatible connector from a full configuration struct (TLS + multipart).
 * tidesdb_objstore_s3_create is a thin wrapper over this with secure/default settings.
 * @param config connector configuration (fields are copied; need not outlive the call)
 * @return connector handle, or NULL on error
 */
tidesdb_objstore_t *tidesdb_objstore_s3_create_config(const tidesdb_objstore_s3_config_t *config);

/**
 * tidesdb_promote_to_primary
 * switch a read-only replica to primary mode
 * @param db database handle in replica mode
 * @return TDB_SUCCESS on success, TDB_ERR_INVALID_ARGS if not a replica
 */
int tidesdb_promote_to_primary(tidesdb_t *db);

int tidesdb_iter_key_value(tidesdb_iter_t *iter, uint8_t **key, size_t *key_size, uint8_t **value,
                           size_t *value_size);

#endif /* __TIDESDB_DB_H__ */
