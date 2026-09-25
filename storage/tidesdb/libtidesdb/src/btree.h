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
#ifndef __BTREE_H__
#define __BTREE_H__

#include "block_manager.h"
#include "clock_cache.h"
#include "compat.h"

/* branch prediction hints */
#if defined(__GNUC__) || defined(__clang__)
#define BTREE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define BTREE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BTREE_LIKELY(x)   (x)
#define BTREE_UNLIKELY(x) (x)
#endif

/* magic number "BTR+" in hex */
#define BTREE_MAGIC   0x4254522B
#define BTREE_VERSION 1

/* btree clock-cache key layout. each key is "<cache_key_prefix><sep><offset_hex>" where
 * cache_key_prefix is set by tidesdb_sstable_create. exposed in the header so cache
 * invalidation paths in tidesdb.c can build matching prefixes. */
#define BTREE_CACHE_KEY_SIZE      32
#define BTREE_CACHE_KEY_SEPARATOR ':'

/* node type flags */
#define BTREE_NODE_LEAF     0x01
#define BTREE_NODE_INTERNAL 0x02

/* entry flags (matching TidesDB kv flags) */
#define BTREE_ENTRY_FLAG_TOMBSTONE 0x01
#define BTREE_ENTRY_FLAG_HAS_TTL   0x02
#define BTREE_ENTRY_FLAG_VLOG_REF  0x04 /* value is in vlog, not inline */
#define BTREE_ENTRY_FLAG_SINGLE_DELETE       \
    0x10 /* single-delete tombstone subtype, \
          * always set alongside             \
          * BTREE_ENTRY_FLAG_TOMBSTONE */

/* default configuration */
#define BTREE_DEFAULT_NODE_SIZE    (64 * 1024) /* 64KB target node size */
#define BTREE_DEFAULT_FANOUT       256         /* target keys per internal node */
#define BTREE_MIN_ENTRIES_PER_LEAF 2

/* block types for metadata */
#define BTREE_BLOCK_TYPE_META     0x00
#define BTREE_BLOCK_TYPE_LEAF     0x01
#define BTREE_BLOCK_TYPE_INTERNAL 0x02

/* forward declarations */
typedef struct btree_t btree_t;
typedef struct btree_builder_t btree_builder_t;
typedef struct btree_cursor_t btree_cursor_t;
typedef struct btree_node_t btree_node_t;
typedef struct btree_entry_t btree_entry_t;
typedef struct btree_arena_t btree_arena_t;

#define BTREE_ARENA_BLOCK_SIZE     (64 * 1024) /* 64KB blocks */
#define BTREE_ARENA_MIN_BLOCK_SIZE 256         /* minimum arena block size */

/**
 * btree_arena_block_t
 * one bump-allocated block in a btree_arena_t's block chain
 * @param data the block's memory buffer
 * @param size total capacity of the block in bytes
 * @param used bytes consumed so far (bump pointer offset into data)
 * @param next next block in the arena's linked list
 */
typedef struct btree_arena_block_t
{
    uint8_t *data;
    size_t size;
    size_t used;
    struct btree_arena_block_t *next;
} btree_arena_block_t;

/*
 * btree_arena_t
 * simple arena allocator for btree nodes to reduce malloc/free overhead
 * allocations are bump-pointer style, freed all at once when arena is destroyed
 * @param current current block
 * @param blocks linked list of blocks
 * @param total_allocated total bytes allocated
 */
struct btree_arena_t
{
    btree_arena_block_t *current;
    btree_arena_block_t *blocks;
    size_t total_allocated;
};

/**
 * btree_arena_create
 * creates a new arena allocator with default block size (64KB)
 * @return new arena or NULL on failure
 */
btree_arena_t *btree_arena_create(void);

/**
 * btree_arena_create_sized
 * creates a new arena allocator with a specific initial capacity
 * used to right-size arenas for deserialized nodes to reduce memory waste
 * @param initial_capacity initial block size in bytes (clamped to minimum 256)
 * @return new arena or NULL on failure
 */
btree_arena_t *btree_arena_create_sized(size_t initial_capacity);

/**
 * btree_arena_alloc
 * allocates memory from the arena (8-byte aligned)
 * @param arena the arena
 * @param size bytes to allocate
 * @return pointer to allocated memory or NULL on failure
 */
void *btree_arena_alloc(btree_arena_t *arena, size_t size);

/**
 * btree_arena_destroy
 * destroys the arena and frees all memory
 * @param arena the arena to destroy
 */
void btree_arena_destroy(btree_arena_t *arena);

/**
 * btree_arena_reset
 * resets the arena for reuse (keeps allocated blocks)
 * @param arena the arena to reset
 */
void btree_arena_reset(btree_arena_t *arena);

/**
 * btree_cmp_type_t
 * comparator type enum (mirrors skip_list)
 */
typedef enum
{
    BTREE_CMP_MEMCMP = 0, /* default memcmp-based comparison */
    BTREE_CMP_STRING,     /* string-based comparison */
    BTREE_CMP_NUMERIC,    /* numeric comparison (8-byte keys) */
    BTREE_CMP_CUSTOM      /* custom comparator function */
} btree_cmp_type_t;

/**
 * btree_comparator_fn
 * comparator function type (same signature as skip_list)
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
typedef int (*btree_comparator_fn)(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                   size_t key2_size, void *ctx);

/**
 * btree_entry_t
 * a single key-value entry in a leaf node
 * @param key_size size of key
 * @param value_size size of value (inline or in vlog)
 * @param vlog_offset offset in vlog if value is external (0 = inline)
 * @param seq sequence number
 * @param ttl time-to-live (0 = no expiry)
 * @param flags entry flags (tombstone, has_ttl, vlog_ref)
 */
struct btree_entry_t
{
    uint32_t key_size;
    uint32_t value_size;
    uint64_t vlog_offset;
    uint64_t seq;
    int64_t ttl;
    uint8_t flags;
};

/**
 * btree_node_t
 * in-memory representation of a B+tree node
 * @param type node type (leaf or internal)
 * @param num_entries number of entries/children
 * @param entries array of entries (leaf nodes only)
 * @param keys array of key pointers
 * @param key_sizes array of key sizes
 * @param values array of inline value pointers (leaf nodes only)
 * @param child_offsets array of child block offsets (internal nodes only)
 * @param prev_offset offset of previous sibling (leaf nodes, for backward scan)
 * @param next_offset offset of next sibling (leaf nodes, for forward scan)
 * @param block_offset this node's offset in the file
 * @param arena arena for cached node allocations (owned by btree, created with cache)
 * @param rc_count reference count for cached nodes (0 = not ref-counted)
 */
struct btree_node_t
{
    uint8_t type;
    uint32_t num_entries;
    btree_entry_t *entries;
    uint8_t **keys;
    size_t *key_sizes;
    uint8_t **values;
    int64_t *child_offsets;
    int64_t prev_offset;
    int64_t next_offset;
    int64_t block_offset;
    btree_arena_t *arena;
    atomic_int rc_count;
};

/**
 * btree_config_t
 * configuration for B+tree construction
 * @param target_node_size target size for nodes in bytes
 * @param value_threshold values >= this size go to vlog
 * @param comparator comparator function
 * @param comparator_ctx comparator context
 * @param cmp_type comparator type
 * @param compression_algo compression algorithm (0=none, 2=lz4, 3=zstd, 4=lz4_fast)
 */
typedef struct
{
    size_t target_node_size;
    size_t value_threshold;
    btree_comparator_fn comparator;
    void *comparator_ctx;
    btree_cmp_type_t cmp_type;
    int compression_algo;
} btree_config_t;

/**
 * btree_t
 * immutable B+tree structure (read-only after construction)
 * @param bm block manager for storage
 * @param root_offset offset of root node
 * @param first_leaf_offset offset of first leaf (for forward iteration)
 * @param last_leaf_offset offset of last leaf (for backward iteration)
 * @param entry_count total number of entries
 * @param node_count total number of nodes
 * @param height tree height
 * @param config configuration
 * @param min_key minimum key in tree
 * @param min_key_size size of minimum key
 * @param max_key maximum key in tree
 * @param max_key_size size of maximum key
 * @param max_seq maximum sequence number
 * @param node_cache node cache for fast lookups (optional, can be NULL)
 * @param node_arena arena for cached node allocations (owned by btree, created with cache)
 * @param cache_key_prefix precomputed cache key prefix for this btree's node cache entries
 */
struct btree_t
{
    block_manager_t *bm;
    int64_t root_offset;
    int64_t first_leaf_offset;
    int64_t last_leaf_offset;
    uint64_t entry_count;
    uint64_t node_count;
    uint32_t height;
    btree_config_t config;
    uint8_t *min_key;
    size_t min_key_size;
    uint8_t *max_key;
    size_t max_key_size;
    uint64_t max_seq;
    clock_cache_t *node_cache;
    btree_arena_t *node_arena;
    uint64_t cache_key_prefix;
};

/**
 * btree_stats_t
 * statistics for a single B+tree (per-sstable)
 * @param entry_count total number of entries
 * @param node_count total number of nodes
 * @param height tree height (1 = single leaf, 2+ = has internal nodes)
 * @param serialized_size total bytes on disk
 */
typedef struct
{
    uint64_t entry_count;
    uint64_t node_count;
    uint32_t height;
    uint64_t serialized_size;
} btree_stats_t;

/**
 * btree_cursor_t
 * cursor for iterating through the B+tree
 * uses tree traversal for leaf-to-leaf navigation (memory efficient)
 * @param tree pointer to the B+tree
 * @param current_node current leaf node
 * @param current_index index within current node
 * @param current_leaf_offset offset of current leaf node
 * @param at_end flag indicating cursor is past end
 * @param at_begin flag indicating cursor is before begin
 * @param using_cache flag indicating current node was loaded from cache
 * @param read_error set when a node load failed (vs a genuine end of tree) so callers can tell a
 *                   truncated traversal apart from a complete one and not treat lost entries as
 * gone
 */
struct btree_cursor_t
{
    btree_t *tree;
    btree_node_t *current_node;
    int32_t current_index;
    int64_t current_leaf_offset;
    int at_end;
    int at_begin;
    int using_cache;
    int read_error;
};

/**
 * btree_comparator_memcmp
 * default memcmp-based comparator
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int btree_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                            size_t key2_size, void *ctx);

/**
 * btree_comparator_string
 * string-based comparator
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int btree_comparator_string(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                            size_t key2_size, void *ctx);

/**
 * btree_comparator_numeric
 * numeric comparator for 8-byte keys
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @param ctx context pointer (unused)
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
int btree_comparator_numeric(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                             size_t key2_size, void *ctx);

/**
 * btree_builder_new
 * creates a new B+tree builder for sorted data insertion
 * @param builder output pointer to builder
 * @param bm block manager for storage
 * @param config configuration (comparator, node size, value threshold)
 * @return 0 on success, -1 on failure
 */
int btree_builder_new(btree_builder_t **builder, block_manager_t *bm, const btree_config_t *config);

/**
 * btree_builder_add
 * adds an entry to the B+tree (must be called in sorted key order)
 * @param builder the builder
 * @param key key data
 * @param key_size size of key
 * @param value value data (NULL for tombstones)
 * @param value_size size of value
 * @param vlog_offset vlog offset if value is external (0 = inline)
 * @param seq sequence number
 * @param ttl time-to-live (0 = no expiry)
 * @param entry_flags bitmask of BTREE_ENTRY_FLAG_* to persist on this entry
 *                    (TOMBSTONE, SINGLE_DELETE). HAS_TTL and VLOG_REF are
 *                    derived from ttl and vlog_offset. passing 1 (a bare
 *                    tombstone) stays valid because 1 == TOMBSTONE.
 * @return 0 on success, -1 on failure
 */
int btree_builder_add(btree_builder_t *builder, const uint8_t *key, size_t key_size,
                      const uint8_t *value, size_t value_size, uint64_t vlog_offset, uint64_t seq,
                      int64_t ttl, uint8_t entry_flags);

/**
 * btree_builder_finish
 * finalizes the B+tree construction
 * @param builder the builder
 * @param tree output pointer to completed tree
 * @return 0 on success, -1 on failure
 */
int btree_builder_finish(btree_builder_t *builder, btree_t **tree);

/**
 * btree_builder_free
 * frees builder resources (call after finish or on error)
 * @param builder the builder to free
 */
void btree_builder_free(btree_builder_t *builder);

/**
 * btree_open
 * opens an existing B+tree from storage
 * tidesdb core reads sstable metadata and passes offsets to btree
 * @param tree output pointer to tree
 * @param bm block manager containing the tree
 * @param config configuration (comparator must match what was used to build)
 * @param root_offset offset of root node (from sstable metadata)
 * @param first_leaf_offset offset of first leaf for forward iteration
 * @param last_leaf_offset offset of last leaf for backward iteration
 * @return 0 on success, -1 on failure
 */
int btree_open(btree_t **tree, block_manager_t *bm, const btree_config_t *config,
               int64_t root_offset, int64_t first_leaf_offset, int64_t last_leaf_offset);

/**
 * btree_get_at_seq
 * retrieves the version of a key visible at a sequence ceiling. a key may have
 * several versions in one tree (a flush or compaction retains a version chain);
 * this returns the one with the highest seq that does not exceed seq_ceiling,
 * or -1 if the key has no version at or below it.
 * @param tree the B+tree
 * @param key key data
 * @param key_size size of key
 * @param seq_ceiling highest sequence number to consider (UINT64_MAX = newest)
 * @param value output pointer to value (caller must free)
 * @param value_size output value size
 * @param vlog_offset output vlog offset (0 if inline)
 * @param seq output sequence number
 * @param ttl output time-to-live
 * @param deleted output tombstone flag
 * @return 0 on success, -1 on not found or error
 */
int btree_get_at_seq(btree_t *tree, const uint8_t *key, size_t key_size, uint64_t seq_ceiling,
                     uint8_t **value, size_t *value_size, uint64_t *vlog_offset, uint64_t *seq,
                     int64_t *ttl, uint8_t *deleted);

/**
 * btree_get
 * retrieves the newest version of a key (equivalent to btree_get_at_seq with
 * seq_ceiling = UINT64_MAX)
 * @param tree the B+tree
 * @param key key data
 * @param key_size size of key
 * @param value output pointer to value (caller must free)
 * @param value_size output value size
 * @param vlog_offset output vlog offset (0 if inline)
 * @param seq output sequence number
 * @param ttl output time-to-live
 * @param deleted output tombstone flag
 * @return 0 on success, -1 on not found or error
 */
int btree_get(btree_t *tree, const uint8_t *key, size_t key_size, uint8_t **value,
              size_t *value_size, uint64_t *vlog_offset, uint64_t *seq, int64_t *ttl,
              uint8_t *deleted);

/**
 * btree_get_entry_count
 * returns total number of entries
 * @param tree the B+tree
 * @return total number of entries in the tree
 */
uint64_t btree_get_entry_count(const btree_t *tree);

/**
 * btree_get_min_key
 * gets the minimum key
 * @param tree the B+tree
 * @param key output pointer to key (caller must free)
 * @param key_size output key size
 * @return 0 on success, -1 on failure
 */
int btree_get_min_key(btree_t *tree, uint8_t **key, size_t *key_size);

/**
 * btree_get_max_key
 * gets the maximum key
 * @param tree the B+tree
 * @param key output pointer to key (caller must free)
 * @param key_size output key size
 * @return 0 on success, -1 on failure
 */
int btree_get_max_key(btree_t *tree, uint8_t **key, size_t *key_size);

/**
 * btree_get_max_seq
 * returns maximum sequence number in tree
 * @param tree the B+tree
 * @return maximum sequence number across all entries in the tree
 */
uint64_t btree_get_max_seq(const btree_t *tree);

/**
 * btree_get_stats
 * populates statistics for the B+tree
 * @param tree the B+tree
 * @param stats output statistics structure
 * @return 0 on success, -1 on failure
 */
int btree_get_stats(const btree_t *tree, btree_stats_t *stats);

/**
 * btree_free
 * frees B+tree resources
 * @param tree the tree to free
 */
void btree_free(btree_t *tree);

/**
 * btree_set_node_cache
 * sets the node cache for faster lookups (optional)
 * the cache is not owned by the btree -- caller must manage its lifetime
 * @param tree the B+tree
 * @param cache the clock cache to use (can be NULL to disable caching)
 */
void btree_set_node_cache(btree_t *tree, clock_cache_t *cache);

/**
 * btree_create_node_cache
 * creates a node cache with the proper eviction callback for btree nodes
 * caller owns the returned cache and must destroy it
 * @param max_bytes maximum cache size in bytes
 * @return new cache or NULL on failure
 */
clock_cache_t *btree_create_node_cache(size_t max_bytes);

/**
 * btree_print_tree
 * prints tree structure for debugging
 * @param tree the B+tree
 */
void btree_print_tree(btree_t *tree);

/**
 * btree_cursor_init
 * initializes a cursor positioned before first entry
 * @param cursor output pointer to cursor
 * @param tree the B+tree
 * @return 0 on success, -1 on failure
 */
int btree_cursor_init(btree_cursor_t **cursor, btree_t *tree);

/**
 * btree_cursor_next
 * moves cursor to next entry
 * @param cursor the cursor
 * @return 0 on success, -1 on failure or end
 */
int btree_cursor_next(btree_cursor_t *cursor);

/**
 * btree_cursor_prev
 * moves cursor to previous entry
 * @param cursor the cursor
 * @return 0 on success, -1 on failure or start
 */
int btree_cursor_prev(btree_cursor_t *cursor);

/**
 * btree_cursor_seek
 * positions cursor at first key >= target
 * @param cursor the cursor
 * @param key target key
 * @param key_size size of target key
 * @return 0 on success, -1 on failure
 */
int btree_cursor_seek(btree_cursor_t *cursor, const uint8_t *key, size_t key_size);

/**
 * btree_cursor_seek_for_prev
 * positions cursor at last key <= target
 * @param cursor the cursor
 * @param key target key
 * @param key_size size of target key
 * @return 0 on success, -1 on failure
 */
int btree_cursor_seek_for_prev(btree_cursor_t *cursor, const uint8_t *key, size_t key_size);

/**
 * btree_cursor_goto_first
 * moves cursor to first entry
 * @param cursor the cursor
 * @return 0 on success, -1 on failure
 */
int btree_cursor_goto_first(btree_cursor_t *cursor);

/**
 * btree_cursor_goto_last
 * moves cursor to last entry
 * @param cursor the cursor
 * @return 0 on success, -1 on failure
 */
int btree_cursor_goto_last(btree_cursor_t *cursor);

/**
 * btree_cursor_valid
 * checks if cursor is at a valid position
 * @param cursor the cursor
 * @return 1 if valid, 0 if not, -1 on error
 */
int btree_cursor_valid(btree_cursor_t *cursor);

/**
 * btree_cursor_read_failed
 * reports whether the cursor stopped because a node load failed rather than because it reached a
 * genuine end of the tree. lets a caller (e.g. a compaction merge source) tell a transiently
 * truncated scan apart from a complete one and abort instead of dropping the unread entries.
 * @param cursor the cursor
 * @return 1 if the last advance stopped on a node-load failure, 0 otherwise
 */
int btree_cursor_read_failed(const btree_cursor_t *cursor);

/**
 * btree_cursor_get
 * gets entry at current cursor position
 * @param cursor the cursor
 * @param key output key pointer (do not free, valid until cursor moves)
 * @param key_size output key size
 * @param value output value pointer (do not free, valid until cursor moves)
 * @param value_size output value size
 * @param vlog_offset output vlog offset (0 if inline)
 * @param seq output sequence number
 * @param ttl output time-to-live
 * @param deleted output tombstone flag
 * @return 0 on success, -1 on failure
 */
int btree_cursor_get(btree_cursor_t *cursor, uint8_t **key, size_t *key_size, uint8_t **value,
                     size_t *value_size, uint64_t *vlog_offset, uint64_t *seq, int64_t *ttl,
                     uint8_t *deleted);

/**
 * btree_cursor_has_next
 * checks if cursor has next entry
 * @param cursor the cursor
 * @return 1 if has next, 0 if not, -1 on error
 */
int btree_cursor_has_next(btree_cursor_t *cursor);

/**
 * btree_cursor_has_prev
 * checks if cursor has previous entry
 * @param cursor the cursor
 * @return 1 if has prev, 0 if not, -1 on error
 */
int btree_cursor_has_prev(btree_cursor_t *cursor);

/**
 * btree_cursor_free
 * frees cursor resources
 * @param cursor the cursor to free
 */
void btree_cursor_free(btree_cursor_t *cursor);

/**
 * btree_node_free
 * frees a node and its contents
 * @param node the node to free
 */
void btree_node_free(btree_node_t *node);

/**
 * btree_node_read
 * reads a node from storage
 * @param bm block manager
 * @param offset block offset
 * @param node output pointer to node
 * @return 0 on success, -1 on failure
 */
int btree_node_read(block_manager_t *bm, int64_t offset, btree_node_t **node);

/**
 * btree_node_read_with_compression
 * reads a node from storage with decompression support
 * @param bm block manager
 * @param offset node offset
 * @param node output pointer to node
 * @param compression_algo compression algorithm (0=none, 2=lz4, 3=zstd, 4=lz4_fast)
 * @return 0 on success, -1 on failure
 */
int btree_node_read_with_compression(block_manager_t *bm, int64_t offset, btree_node_t **node,
                                     int compression_algo);

/**
 * btree_format_cache_key_prefix
 * write the producer-side cache key prefix for a btree owned by an sstable whose
 * cache_key_prefix value is the given uint64. the bytes produced match what
 * btree_node_read_cached prepends before the per-node offset, so callers can pass them
 * to clock_cache_delete_by_prefix to invalidate every cache entry for one btree.
 *
 * @param cache_key_prefix the precomputed prefix value (see tidesdb_sstable_t)
 * @param out output buffer; must be at least BTREE_CACHE_KEY_SIZE bytes
 * @return number of bytes written (no trailing null)
 */
int btree_format_cache_key_prefix(uint64_t cache_key_prefix, char *out);

#endif /* __BTREE_H__ */
