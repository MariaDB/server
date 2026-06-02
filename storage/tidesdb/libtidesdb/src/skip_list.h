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
#ifndef __SKIP_LIST_H__
#define __SKIP_LIST_H__
#include "compat.h"

/* branch prediction hints for hot paths */
#if defined(__GNUC__) || defined(__clang__)
#define SKIP_LIST_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SKIP_LIST_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SKIP_LIST_LIKELY(x)   (x)
#define SKIP_LIST_UNLIKELY(x) (x)
#endif

/* forward declarations */
typedef struct skip_list_node_t skip_list_node_t;
typedef struct skip_list_t skip_list_t;
typedef struct skip_list_version_t skip_list_version_t;
typedef struct skip_list_arena_block_t skip_list_arena_block_t;
typedef struct skip_list_arena_t skip_list_arena_t;

/* arena alignment for all allocations */
#define SKIP_LIST_ARENA_ALIGNMENT 8

/* maximum number of thread-local blocks for contention-free allocation */
#define SKIP_LIST_ARENA_MAX_THREADS 64

/* default size for thread-local blocks (smaller than shared block to save memory) */
#define SKIP_LIST_ARENA_TL_BLOCK_SIZE (64 * 1024)

/**
 * skip_list_arena_block_t
 * a single contiguous memory block in the arena's linked list
 * @param data pointer to the raw memory
 * @param used atomic bump pointer (bytes consumed so far)
 * @param capacity total bytes available in this block
 * @param prev previous block in the chain (for destruction)
 */
struct skip_list_arena_block_t
{
    uint8_t *data;
    _Atomic(size_t) used;
    size_t capacity;
    skip_list_arena_block_t *prev;
};

/**
 * skip_list_arena_t
 * lock-free bump allocator for skip list nodes and versions
 * uses thread-local blocks to eliminate atomic contention on the fast path
 * each thread gets its own block; only block allocation requires synchronization
 * individual frees are no-ops; all memory is reclaimed when the arena is destroyed
 * @param current_block atomic pointer to the shared fallback block (rarely used)
 * @param block_size default capacity for new blocks
 * @param tl_blocks thread-local block pointers indexed by thread slot
 * @param tl_slot_counter atomic counter for assigning thread slots
 * @param all_blocks_head atomic linked list of all blocks for destruction
 */
struct skip_list_arena_t
{
    _Atomic(skip_list_arena_block_t *) current_block;
    size_t block_size;
    _Atomic(skip_list_arena_block_t *) tl_blocks[SKIP_LIST_ARENA_MAX_THREADS];
    _Atomic(int) tl_slot_counter;
    _Atomic(skip_list_arena_block_t *) all_blocks_head;
};

/* skip_list_version_t flag bits */
#define SKIP_LIST_FLAG_DELETED 0x01 /* version is tombstone */
#define SKIP_LIST_FLAG_SINGLE_DELETE                         \
    0x02 /* tombstone subtype, always set together with      \
          * SKIP_LIST_FLAG_DELETED. caller promises the key  \
          * has been put at most once since the last         \
          * single-delete or start, so put+single-delete can \
          * be reaped together at compaction. */

/**
 * skip_list_cmp_type_t
 * comparator type enum
 */
typedef enum
{
    SKIP_LIST_CMP_MEMCMP = 0, /* default memcmp-based comparison */
    SKIP_LIST_CMP_STRING,     /* string-based comparison */
    SKIP_LIST_CMP_NUMERIC,    /* numeric comparison (8-byte keys) */
    SKIP_LIST_CMP_CUSTOM      /* custom comparator function */
} skip_list_cmp_type_t;

/* skip_list_node_t flag bits */
#define SKIP_LIST_NODE_FLAG_SENTINEL 0x01 /* node is a sentinel (header or tail) */

#define SKIP_LIST_MAX_CAS_ATTEMPTS 1000

/* helper macros for flag access */
#define VERSION_IS_DELETED(version) \
    (atomic_load_explicit(&(version)->flags, memory_order_acquire) & SKIP_LIST_FLAG_DELETED)

#define NODE_IS_SENTINEL(node) ((node)->node_flags & SKIP_LIST_NODE_FLAG_SENTINEL)

/**
 * skip_list_version_t
 * a single version of a key's value
 * @param seq sequence number for MVCC (monotonically increasing)
 * @param value value data
 * @param value_size size of value
 * @param ttl time-to-live
 * @param next next older version
 * @param flags version flags (deleted, etc)
 */
struct skip_list_version_t
{
    _Atomic(uint64_t) seq;
    uint8_t *value;
    size_t value_size;
    int64_t ttl;
    _Atomic(skip_list_version_t *) next;
    _Atomic(uint8_t) flags;
};

/**
 * skip_list_comparator_fn
 * comparator function type for custom key comparison
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
typedef int (*skip_list_comparator_fn)(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                       size_t key2_size, void *ctx);

/* macro to access backward pointers at a specific level */
#define BACKWARD_PTR(node, lvl, max_level) (node->forward[(max_level) + 1 + (lvl)])

/**
 * skip_list_node_t
 * a key in the skip list with multiple versions
 * @param level node level in skip list
 * @param node_flags node flags (sentinel, etc)
 * @param key key data (NULL for sentinel nodes)
 * @param key_size size of key (0 for sentinel nodes)
 * @param versions lock-free list of versions (newest first)
 * @param forward forward[0..level] forward pointers, forward[level+1..2*level+1] backward pointers
 */
struct skip_list_node_t
{
    uint8_t level;
    uint8_t node_flags;
    uint8_t *key;
    size_t key_size;
    _Atomic(skip_list_version_t *) versions;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4200)
#endif
    _Atomic(skip_list_node_t *) forward[];
#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

/**
 * skip_list_t
 * main skip list structure
 * @param level current maximum level
 * @param max_level maximum allowed level
 * @param probability probability for level generation
 * @param header sentinel header node (compares less than all keys)
 * @param tail sentinel tail node (compares greater than all keys)
 * @param total_size total size of all entries
 * @param entry_count track entry count atomically to avoid O(n) traversals
 * @param cmp_type comparator type enum (memcmp, string, numeric, custom)
 * @param comparator key comparison function
 * @param comparator_ctx context for comparator
 * @param cached_time pointer to external cached time (NULL = use time(NULL))
 * @param arena bump allocator for cache-friendly node allocation (NULL = use malloc/free)
 */
typedef struct skip_list_t
{
    _Atomic(int) level;
    int max_level;
    float probability;
    _Atomic(skip_list_node_t *) header;
    _Atomic(skip_list_node_t *) tail;
    _Atomic(size_t) total_size;
    _Atomic(int) entry_count;
    skip_list_cmp_type_t cmp_type;
    skip_list_comparator_fn comparator;
    void *comparator_ctx;
    _Atomic(time_t) *cached_time;
    skip_list_arena_t *arena;
} skip_list_t;

/**
 * skip_list_cursor_t
 * cursor structure for iterating through the skip list
 * @param list pointer to the skip list
 * @param current current node position
 * @param cached_header cached header sentinel for fast boundary checks
 * @param cached_tail cached tail sentinel for fast boundary checks
 * @param current_version current version on the current node; NULL means use head.
 *                        advanced by skip_list_cursor_advance_in_node and reset on
 *                        every cursor seek/next/prev
 */
typedef struct
{
    skip_list_t *list;
    skip_list_node_t *current;
    skip_list_node_t *cached_header;
    skip_list_node_t *cached_tail;
    skip_list_version_t *current_version;
} skip_list_cursor_t;

/**
 * skip_list_comparator_memcmp
 * default memcmp-based comparator
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int skip_list_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                size_t key2_size, void *ctx);

/**
 * skip_list_comparator_string
 * string-based comparator
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int skip_list_comparator_string(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                size_t key2_size, void *ctx);

/**
 * skip_list_comparator_numeric
 * numeric comparator
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int skip_list_comparator_numeric(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                 size_t key2_size, void *ctx);

/**
 * skip_list_create_node
 * creates a new skip list node
 * @param level level of the node
 * @param key key data
 * @param key_size size of key
 * @param value value data
 * @param value_size size of value
 * @param ttl time-to-live
 * @param flags version flags (bitmask of SKIP_LIST_FLAG_*)
 * @return pointer to new node, NULL on failure
 */
skip_list_node_t *skip_list_create_node(int level, const uint8_t *key, size_t key_size,
                                        const uint8_t *value, size_t value_size, int64_t ttl,
                                        uint8_t flags);

/**
 * skip_list_free_node
 * frees a skip list node
 * @param node node to free
 * @return 0 on success, -1 on failure
 */
int skip_list_free_node(skip_list_node_t *node);

/**
 * skip_list_new
 * creates a new skip list with default memcmp comparator
 * @param list pointer to skip list pointer
 * @param max_level maximum level
 * @param probability probability for level generation
 * @return 0 on success, -1 on failure
 */
int skip_list_new(skip_list_t **list, int max_level, float probability);

/**
 * skip_list_new_with_comparator
 * creates a new skip list with custom comparator
 * @param list pointer to skip list pointer
 * @param max_level maximum level
 * @param probability probability for level generation
 * @param comparator custom key comparison function
 * @param comparator_ctx context for comparator
 * @return 0 on success, -1 on failure
 */
int skip_list_new_with_comparator(skip_list_t **list, int max_level, float probability,
                                  skip_list_comparator_fn comparator, void *comparator_ctx);

/**
 * skip_list_new_with_comparator_and_cached_time
 * creates a new skip list with custom comparator and cached time pointer
 * @param list pointer to skip list pointer
 * @param max_level maximum level
 * @param probability probability for level generation
 * @param comparator custom key comparison function
 * @param comparator_ctx context for comparator
 * @param cached_time pointer to external cached time (avoids time() syscalls)
 * @return 0 on success, -1 on failure
 */
int skip_list_new_with_comparator_and_cached_time(skip_list_t **list, int max_level,
                                                  float probability,
                                                  skip_list_comparator_fn comparator,
                                                  void *comparator_ctx,
                                                  _Atomic(time_t) *cached_time);

/**
 * skip_list_new_with_arena
 * creates a new skip list backed by a bump arena for cache-friendly node allocation
 * all node and version memory is allocated from contiguous blocks, improving spatial
 * locality during traversal. individual frees are no-ops; memory is reclaimed when
 * the skip list is freed. ideal for memtable skip lists that are filled then freed whole.
 * @param list pointer to skip list pointer
 * @param max_level maximum level
 * @param probability probability for level generation
 * @param comparator custom key comparison function
 * @param comparator_ctx context for comparator
 * @param cached_time pointer to external cached time (avoids time() syscalls)
 * @param arena_initial_capacity initial arena block size in bytes (0 = no arena)
 * @return 0 on success, -1 on failure
 */
int skip_list_new_with_arena(skip_list_t **list, int max_level, float probability,
                             skip_list_comparator_fn comparator, void *comparator_ctx,
                             _Atomic(time_t) *cached_time, size_t arena_initial_capacity);

/**
 * skip_list_random_level
 * generates a random level for a new node
 * @param list skip list
 * @return random level
 */
int skip_list_random_level(const skip_list_t *list);

/**
 * skip_list_compare_keys
 * compares two keys using the skip list's comparator
 * @param list skip list
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int skip_list_compare_keys(const skip_list_t *list, const uint8_t *key1, size_t key1_size,
                           const uint8_t *key2, size_t key2_size);

/**
 * skip_list_put_with_seq
 * inserts or updates a key-value pair with a specific sequence number
 * @param list skip list
 * @param key key
 * @param key_size key size
 * @param value value
 * @param value_size value size
 * @param ttl time-to-live
 * @param seq sequence number for MVCC
 * @param flags bitmask of SKIP_LIST_FLAG_*; 0 means a live put, SKIP_LIST_FLAG_DELETED
 *              means a tombstone, optionally OR'd with SKIP_LIST_FLAG_SINGLE_DELETE.
 *              passing 1 for a regular tombstone remains valid because the value 1
 *              equals SKIP_LIST_FLAG_DELETED.
 * @return 0 on success, -1 on failure
 */
int skip_list_put_with_seq(skip_list_t *list, const uint8_t *key, size_t key_size,
                           const uint8_t *value, size_t value_size, int64_t ttl, uint64_t seq,
                           uint8_t flags);

/**
 * skip_list_delete
 * deletes a key (creates tombstone) with a specific sequence number
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param seq sequence number for the deletion (must be greater than existing versions)
 * @return 0 on success, -1 on failure (including if seq <= existing version seq)
 */
int skip_list_delete(skip_list_t *list, const uint8_t *key, size_t key_size, uint64_t seq);

/**
 * skip_list_batch_entry_t
 * entry for batch put operations
 *
 * flags is a bitmask of SKIP_LIST_FLAG_*. a live put leaves flags = 0; a regular
 * tombstone sets SKIP_LIST_FLAG_DELETED; a single-delete tombstone also sets
 * SKIP_LIST_FLAG_SINGLE_DELETE on top. callers that previously set deleted = 1
 * continue to work unchanged because the value 1 equals SKIP_LIST_FLAG_DELETED.
 */
typedef struct
{
    const uint8_t *key;
    size_t key_size;
    const uint8_t *value;
    size_t value_size;
    uint64_t seq;
    int64_t ttl;
    uint8_t flags;
} skip_list_batch_entry_t;

/**
 * skip_list_put_batch
 * inserts multiple key-value pairs in a batch for better performance
 * entries should ideally be sorted by key for optimal performance
 * @param list skip list
 * @param entries array of batch entries
 * @param count number of entries
 * @return number of successfully inserted entries; this MAY be less than count when
 *         individual entries are skipped (e.g. duplicate (key,seq) or a per-entry
 *         allocation failure) -- compare the result against count to detect a partial
 *         batch. returns -1 only on a critical failure that inserts nothing, NULL list/
 *         entries, count == 0, or the update-array allocation failing.
 */
int skip_list_put_batch(skip_list_t *list, const skip_list_batch_entry_t *entries, size_t count);

/**
 * skip_list_get
 * retrieves a value by key
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param value pointer to value pointer (caller must free)
 * @param value_size pointer to value size
 * @param deleted pointer to deleted flag
 * @param ttl pointer to ttl
 * @return 0 on success, -1 on failure
 */
int skip_list_get(skip_list_t *list, const uint8_t *key, size_t key_size, uint8_t **value,
                  size_t *value_size, int64_t *ttl, uint8_t *deleted);

/**
 * skip_list_get_ref
 * zero-copy get that returns a direct pointer into the version data
 * the returned pointers are only valid while the caller holds a reference
 * to the skip list (e.g. memtable refcount). caller must not free the value.
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param value pointer to value pointer (do not free)
 * @param value_size pointer to value size
 * @param ttl pointer to ttl
 * @param deleted pointer to deleted flag
 * @return 0 on success, -1 on failure
 */
int skip_list_get_ref(skip_list_t *list, const uint8_t *key, size_t key_size, const uint8_t **value,
                      size_t *value_size, int64_t *ttl, uint8_t *deleted);

/**
 * skip_list_visibility_check_fn
 * Callback function to check if a sequence is visible
 * @param opaque_ctx opaque context pointer (e.g., commit_status)
 * @param seq sequence number to check
 * @return 1 if visible, 0 if not
 */
typedef int (*skip_list_visibility_check_fn)(void *opaque_ctx, uint64_t seq);

/**
 * skip_list_get_with_seq
 * retrieves a value by key with sequence number for MVCC snapshot reads
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param value pointer to value pointer (caller must free)
 * @param value_size pointer to value size
 * @param ttl pointer to ttl
 * @param deleted pointer to deleted flag
 * @param seq pointer to sequence number (output)
 * @param snapshot_seq snapshot sequence number. UINT64_MAX reads the latest version with no
 *                     snapshot filtering; any other value reads the newest version with seq <=
 *                     snapshot_seq, so 0 matches nothing because sequence numbers start at 1
 * @param visibility_check callback to check if a sequence is committed (NULL = skip check)
 * @param visibility_ctx context for visibility check callback
 * @return 0 on success, -1 on failure
 */
int skip_list_get_with_seq(skip_list_t *list, const uint8_t *key, size_t key_size, uint8_t **value,
                           size_t *value_size, int64_t *ttl, uint8_t *deleted, uint64_t *seq,
                           uint64_t snapshot_seq, skip_list_visibility_check_fn visibility_check,
                           void *visibility_ctx);

/**
 * skip_list_get_with_seq_ref
 * zero-copy MVCC get that returns a direct pointer into the version data
 * the returned pointer is only valid while the caller holds a reference
 * to the skip list (e.g. memtable refcount). caller must not free the value.
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param value pointer to const value pointer (do not free)
 * @param value_size pointer to value size
 * @param ttl pointer to ttl
 * @param deleted pointer to deleted flag
 * @param seq pointer to sequence number (output)
 * @param snapshot_seq snapshot sequence number (UINT64_MAX = latest; otherwise the newest version
 *                     with seq <= snapshot_seq, and 0 matches nothing since seqs start at 1)
 * @param visibility_check callback to check if a sequence is committed
 * @param visibility_ctx context for visibility check callback
 * @return 0 on success, -1 on failure
 */
int skip_list_get_with_seq_ref(skip_list_t *list, const uint8_t *key, size_t key_size,
                               const uint8_t **value, size_t *value_size, int64_t *ttl,
                               uint8_t *deleted, uint64_t *seq, uint64_t snapshot_seq,
                               skip_list_visibility_check_fn visibility_check,
                               void *visibility_ctx);

/**
 * skip_list_get_max_seq
 * retrieves only the maximum sequence number for a key without allocating value
 * optimized for conflict detection where only seq comparison is needed
 * @param list skip list
 * @param key key data
 * @param key_size size of key
 * @param out_seq output parameter for sequence number (set to 0 if not found)
 * @return 0 if key found, -1 if not found or error
 */
int skip_list_get_max_seq(skip_list_t *list, const uint8_t *key, size_t key_size,
                          uint64_t *out_seq);

/**
 * skip_list_cursor_init
 * initializes a new cursor
 * @param cursor pointer to cursor pointer
 * @param list skip list
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_init(skip_list_cursor_t **cursor, skip_list_t *list);

/**
 * skip_list_cursor_next
 * moves cursor to next entry
 * @param cursor cursor
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_next(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_prev
 * moves cursor to previous entry
 * @param cursor cursor
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_prev(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_get
 * gets key-value at current cursor position
 * @param cursor cursor
 * @param key pointer to key pointer
 * @param key_size pointer to key size
 * @param value pointer to value pointer
 * @param value_size pointer to value size
 * @param ttl pointer to ttl
 * @param deleted pointer to deleted flag
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_get(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                         uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted);

/**
 * skip_list_cursor_next_get
 * fused next + get in a single call, avoiding redundant sentinel checks
 * and enabling better prefetching. returns zero-copy pointers.
 * @param cursor cursor
 * @param key pointer to key pointer (do not free)
 * @param key_size pointer to key size
 * @param value pointer to value pointer (do not free)
 * @param value_size pointer to value size
 * @param ttl pointer to ttl
 * @param deleted pointer to deleted flag
 * @return 0 on success, -1 on failure (end of list)
 */
int skip_list_cursor_next_get(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                              uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted);

/**
 * skip_list_cursor_get_with_seq
 * get key-value pair at cursor position with sequence number
 * @param cursor cursor
 * @param key pointer to key
 * @param key_size pointer to key size
 * @param value pointer to value
 * @param value_size pointer to value size
 * @param ttl pointer to TTL
 * @param deleted pointer to deleted flag
 * @param seq pointer to sequence number
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_get_with_seq(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                                  uint8_t **value, size_t *value_size, int64_t *ttl,
                                  uint8_t *deleted, uint64_t *seq);

/**
 * skip_list_cursor_advance_in_node
 * advance the cursor to the next-older version on the current node without moving
 * to the next key. used by mvcc readers and flushers that need every version still
 * visible to an active snapshot, not just the latest. resets to head on the next
 * cursor seek/next/prev.
 * @param cursor cursor
 * @return 0 on success, -1 when the version chain on the current node is exhausted
 */
int skip_list_cursor_advance_in_node(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_free
 * frees a cursor
 * @param cursor cursor to free
 */
void skip_list_cursor_free(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_at_start
 * checks if cursor is at start
 * @param cursor cursor
 * @return 1 if at start, 0 if not, -1 on error
 */
int skip_list_cursor_at_start(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_at_end
 * checks if cursor is at end
 * @param cursor cursor
 * @return 1 if at end, 0 if not, -1 on error
 */
int skip_list_cursor_at_end(const skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_has_next
 * checks if cursor has next entry
 * @param cursor cursor
 * @return 1 if has next, 0 if not
 */
int skip_list_cursor_has_next(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_has_prev
 * checks if cursor has previous entry
 * @param cursor cursor
 * @return 1 if has prev, 0 if not
 */
int skip_list_cursor_has_prev(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_goto_last
 * moves cursor to last entry
 * @param cursor cursor
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_goto_last(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_goto_first
 * moves cursor to first entry
 * @param cursor cursor
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_goto_first(skip_list_cursor_t *cursor);

/**
 * skip_list_cursor_seek
 * positions cursor at the node before the first key >= target
 * @param cursor cursor to position
 * @param key target key
 * @param key_size size of target key
 * @return 0 on success, -1 on failure
 *
 * after calling this function, cursor->current points to the predecessor node.
 * callers must call skip_list_cursor_next() to access the actual first key >= target.
 * this behavior allows efficient insertion and supports both exact matches and range queries.
 */
int skip_list_cursor_seek(skip_list_cursor_t *cursor, const uint8_t *key, size_t key_size);

/**
 * skip_list_cursor_seek_ge
 * seeks cursor directly to the first key >= target, positioning cursor->current on it.
 * unlike skip_list_cursor_seek (which parks on the predecessor and requires a separate
 * skip_list_cursor_next), this folds the advance in and re-reads forward[0] so a concurrent
 * skip_list_put that splices a node < target into the predecessor's forward[0] between the
 * descent and the advance cannot leave the cursor on a key below target.
 * @param cursor cursor
 * @param key target key
 * @param key_size size of target key
 * @return 0 if positioned on a key >= target, -1 if no such key exists (cursor at end)
 */
int skip_list_cursor_seek_ge(skip_list_cursor_t *cursor, const uint8_t *key, size_t key_size);

/**
 * skip_list_cursor_seek_for_prev
 * seeks cursor to last key <= target
 * @param cursor cursor
 * @param key target key
 * @param key_size size of target key
 * @return 0 on success, -1 on failure
 */
int skip_list_cursor_seek_for_prev(skip_list_cursor_t *cursor, const uint8_t *key, size_t key_size);

/**
 * skip_list_cursor_valid
 * checks if cursor is at a valid position (not at sentinel)
 * @param cursor cursor
 * @return 1 if valid, 0 if not, -1 on error
 */
int skip_list_cursor_valid(const skip_list_cursor_t *cursor);

/**
 * skip_list_clear
 * clears all entries from the skip list
 * @param list skip list
 * @return 0 on success, -1 on failure
 */
int skip_list_clear(skip_list_t *list);

/**
 * skip_list_free
 * frees the skip list and all its nodes
 * @param list skip list
 */
void skip_list_free(skip_list_t *list);

/**
 * skip_list_check_and_update_ttl
 * checks and updates TTL for a node
 * @param list skip list
 * @param node node to check
 * @return 0 on success, -1 on failure
 */
int skip_list_check_and_update_ttl(const skip_list_t *list, skip_list_node_t *node);

/**
 * skip_list_get_size
 * gets total size of all entries
 * @param list skip list
 * @return total size in bytes
 */
size_t skip_list_get_size(skip_list_t *list);

/**
 * skip_list_count_entries
 * counts number of entries in skip list
 * @param list skip list
 * @return number of entries
 */
int skip_list_count_entries(skip_list_t *list);

/**
 * skip_list_get_min_key
 * gets the minimum key in the skip list
 * @param list skip list
 * @param key pointer to key pointer
 * @param key_size pointer to key size
 * @return 0 on success, -1 on failure
 */
int skip_list_get_min_key(skip_list_t *list, uint8_t **key, size_t *key_size);

/**
 * skip_list_get_max_key
 * gets the maximum key in the skip list
 * @param list skip list
 * @param key pointer to key pointer
 * @param key_size pointer to key size
 * @return 0 on success, -1 on failure
 */
int skip_list_get_max_key(skip_list_t *list, uint8_t **key, size_t *key_size);

#endif /* __SKIP_LIST_H__ */