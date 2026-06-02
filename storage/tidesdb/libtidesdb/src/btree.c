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

#include "btree.h"

#include <inttypes.h>

#include "compress.h"
#include "xxhash.h"

/* arena alignment in bytes -- every allocation is rounded up so unaligned typed access
 * inside an arena slot is safe on platforms that fault on misaligned uint64_t loads */
#define BTREE_ARENA_ALIGNMENT 8

/* upper bound on hex digits for a uint64 -- 16 nibbles, used as the local stack buffer
 * by btree_u64_to_hex when building a cache key */
#define BTREE_U64_HEX_MAX 16

/* initial entry capacity of a pending leaf during btree construction; the array doubles
 * on overflow so this only sets the smallest meaningful allocation */
#define BTREE_PENDING_LEAF_INITIAL_CAP 64

/* small malloc safety pads added on top of the precomputed est_size in the leaf and
 * internal-node serializers, to absorb any conservative undercount without realloc */
#define BTREE_LEAF_SERIALIZE_SAFETY_PAD     64
#define BTREE_INTERNAL_SERIALIZE_SAFETY_PAD 32

/* fixed-size empty-leaf encoding -- type byte, num_entries=0 varint, prev/next int64 */
#define BTREE_LEAF_EMPTY_BUF_SIZE 32

/* suffix for the temp file uncompressed leaves are staged into before compression */
#define BTREE_LEAF_STAGE_SUFFIX ".lstmp"

/* compressed-node block layout written by btree_node_serialize_with_compression and read
 * back by btree_node_read_with_compression. format is
 * [original_size:u32][prev_offset:i64][next_offset:i64][compressed_data] */
#define BTREE_COMPRESSED_NODE_PREV_OFF    4
#define BTREE_COMPRESSED_NODE_NEXT_OFF    12
#define BTREE_COMPRESSED_NODE_HEADER_SIZE 20

/**
 * varint encoding utilities
 * uses LEB128-style encoding -- 7 bits per byte, high bit = continuation
 */

/**
 * btree_varint_size
 * returns the size of a varint encoding for a given value
 * @param val the value to encode
 * @return the size of the varint encoding
 */
static inline size_t btree_varint_size(const uint64_t val)
{
    if (val < (1ULL << 7)) return 1;
    if (val < (1ULL << 14)) return 2;
    if (val < (1ULL << 21)) return 3;
    if (val < (1ULL << 28)) return 4;
    if (val < (1ULL << 35)) return 5;
    if (val < (1ULL << 42)) return 6;
    if (val < (1ULL << 49)) return 7;
    if (val < (1ULL << 56)) return 8;
    if (val < (1ULL << 63)) return 9;
    return 10;
}

/**
 * btree_varint_encode
 * encodes a varint value into a buffer
 * @param buf the buffer to encode into
 * @param val the value to encode
 * @return the number of bytes encoded
 */
static inline size_t btree_varint_encode(uint8_t *buf, uint64_t val)
{
    size_t i = 0;
    while (val >= 0x80)
    {
        buf[i++] = (uint8_t)(val | 0x80);
        val >>= 7;
    }
    buf[i++] = (uint8_t)val;
    return i;
}

/**
 * btree_varint_decode
 * decodes a varint value from a buffer
 * @param buf the buffer to decode from
 * @param val the value to decode
 * @return the number of bytes decoded
 */
static inline size_t btree_varint_decode(const uint8_t *buf, uint64_t *val)
{
    uint64_t result = 0;
    size_t shift = 0;
    size_t i = 0;
    while (buf[i] & 0x80)
    {
        result |= (uint64_t)(buf[i] & 0x7F) << shift;
        shift += 7;
        i++;
        if (i >= 10) break;
    }
    result |= (uint64_t)buf[i] << shift;
    *val = result;
    return i + 1;
}

/**
 * btree_signed_varint_encode
 * encodes a signed integer using zigzag encoding then varint
 * @param buf the buffer to encode into
 * @param val the signed value to encode
 * @return the number of bytes encoded
 */
static inline size_t btree_signed_varint_encode(uint8_t *buf, const int64_t val)
{
    const uint64_t uval = ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
    return btree_varint_encode(buf, uval);
}

/**
 * btree_signed_varint_decode
 * decodes a zigzag-encoded signed varint from a buffer
 * @param buf the buffer to decode from
 * @param val output parameter for the decoded signed value
 * @return the number of bytes decoded
 */
static inline size_t btree_signed_varint_decode(const uint8_t *buf, int64_t *val)
{
    uint64_t uval;
    const size_t n = btree_varint_decode(buf, &uval);
    *val = (int64_t)((uval >> 1) ^ (~(uval & 1) + 1));
    return n;
}

/* bounded LEB128 decode for parsing on-disk (untrusted) node bytes in which reads at most
 * the bytes remaining before `end` and at most 10. returns bytes consumed, or 0 on
 * truncation / overlong encoding so the caller can reject a malformed node. */
static inline size_t btree_varint_decode_bounded(const uint8_t *buf, const uint8_t *end,
                                                 uint64_t *val)
{
    uint64_t result = 0;
    size_t shift = 0;
    for (size_t i = 0; i < 10; i++)
    {
        if (buf + i >= end) return 0;
        const uint8_t b = buf[i];
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
        {
            *val = result;
            return i + 1;
        }
        shift += 7;
    }
    return 0;
}

static inline size_t btree_signed_varint_decode_bounded(const uint8_t *buf, const uint8_t *end,
                                                        int64_t *val)
{
    uint64_t uval;
    const size_t n = btree_varint_decode_bounded(buf, end, &uval);
    if (n == 0) return 0;
    *val = (int64_t)((uval >> 1) ^ (~(uval & 1) + 1));
    return n;
}

/**
 * btree_compute_prefix_len
 * computes the common prefix length between two keys
 * @param key1 first key data
 * @param len1 length of first key
 * @param key2 second key data
 * @param len2 length of second key
 * @return the number of common prefix bytes
 */
static inline size_t btree_compute_prefix_len(const uint8_t *key1, const size_t len1,
                                              const uint8_t *key2, const size_t len2)
{
    const size_t min_len = (len1 < len2) ? len1 : len2;
    size_t prefix_len = 0;
    while (prefix_len < min_len && key1[prefix_len] == key2[prefix_len])
    {
        prefix_len++;
    }
    return prefix_len;
}

/**
 * btree_arena_create
 * creates a new arena allocator for bulk memory management
 * @return new arena or NULL on failure
 */
btree_arena_t *btree_arena_create(void)
{
    btree_arena_t *arena = calloc(1, sizeof(btree_arena_t));
    if (!arena) return NULL;

    btree_arena_block_t *block = calloc(1, sizeof(btree_arena_block_t));
    if (!block)
    {
        free(arena);
        return NULL;
    }

    block->data = malloc(BTREE_ARENA_BLOCK_SIZE);
    if (!block->data)
    {
        free(block);
        free(arena);
        return NULL;
    }

    block->size = BTREE_ARENA_BLOCK_SIZE;
    block->used = 0;
    block->next = NULL;

    arena->current = block;
    arena->blocks = block;
    arena->total_allocated = BTREE_ARENA_BLOCK_SIZE;

    return arena;
}

btree_arena_t *btree_arena_create_sized(size_t initial_capacity)
{
    if (initial_capacity < BTREE_ARENA_MIN_BLOCK_SIZE)
        initial_capacity = BTREE_ARENA_MIN_BLOCK_SIZE;

    initial_capacity = (initial_capacity + 7) & ~(size_t)7;

    btree_arena_t *arena = malloc(sizeof(btree_arena_t));
    if (!arena) return NULL;

    btree_arena_block_t *block = malloc(sizeof(btree_arena_block_t));
    if (!block)
    {
        free(arena);
        return NULL;
    }

    block->data = malloc(initial_capacity);
    if (!block->data)
    {
        free(block);
        free(arena);
        return NULL;
    }

    block->size = initial_capacity;
    block->used = 0;
    block->next = NULL;

    arena->current = block;
    arena->blocks = block;
    arena->total_allocated = initial_capacity;

    return arena;
}

/**
 * btree_arena_alloc
 * allocates memory from the arena with 8-byte alignment
 * @param arena the arena to allocate from
 * @param size number of bytes to allocate
 * @return pointer to allocated memory or NULL on failure
 */
void *btree_arena_alloc(btree_arena_t *arena, size_t size)
{
    if (!arena || size == 0) return NULL;

    size = (size + (BTREE_ARENA_ALIGNMENT - 1)) & ~(size_t)(BTREE_ARENA_ALIGNMENT - 1);

    /* we check if current block has space */
    if (arena->current->used + size <= arena->current->size)
    {
        void *ptr = arena->current->data + arena->current->used;
        arena->current->used += size;
        return ptr;
    }

    /* we need new block thus we allocate at least BTREE_ARENA_BLOCK_SIZE or size if larger */
    const size_t block_size = (size > BTREE_ARENA_BLOCK_SIZE) ? size : BTREE_ARENA_BLOCK_SIZE;

    btree_arena_block_t *block = calloc(1, sizeof(btree_arena_block_t));
    if (!block) return NULL;

    block->data = malloc(block_size);
    if (!block->data)
    {
        free(block);
        return NULL;
    }

    block->size = block_size;
    block->used = size;
    block->next = arena->blocks;
    arena->blocks = block;
    arena->current = block;
    arena->total_allocated += block_size;

    return block->data;
}

/**
 * btree_arena_destroy
 * destroys an arena and frees all associated memory
 * @param arena the arena to destroy
 */
void btree_arena_destroy(btree_arena_t *arena)
{
    if (!arena) return;

    btree_arena_block_t *block = arena->blocks;
    while (block)
    {
        btree_arena_block_t *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }

    free(arena);
}

/**
 * btree_arena_reset
 * resets an arena for reuse without freeing memory
 * @param arena the arena to reset
 */
void btree_arena_reset(btree_arena_t *arena)
{
    if (!arena) return;

    btree_arena_block_t *block = arena->blocks;
    while (block)
    {
        block->used = 0;
        block = block->next;
    }

    arena->current = arena->blocks;
}

/**
 * btree_compare_keys_numeric_inline
 * fast inline comparison for 8-byte numeric keys
 * @param key1 first key (8 bytes)
 * @param key2 second key (8 bytes)
 * @return -1 if key1 < key2, 1 if key1 > key2, 0 if equal
 */
static inline int btree_compare_keys_numeric_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint64_t v1, v2;
    memcpy(&v1, key1, sizeof(uint64_t));
    memcpy(&v2, key2, sizeof(uint64_t));
    return (v1 < v2) ? -1 : (v1 > v2);
}

#if defined(__GNUC__) || defined(__clang__)
#define BTREE_BSWAP64(x) __builtin_bswap64(x)
#elif defined(_MSC_VER)
#define BTREE_BSWAP64(x) _byteswap_uint64(x)
#else
static inline uint64_t BTREE_BSWAP64(uint64_t x)
{
    return ((x & 0xFF00000000000000ULL) >> 56) | ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0x0000FF0000000000ULL) >> 24) | ((x & 0x000000FF00000000ULL) >> 8) |
           ((x & 0x00000000FF000000ULL) << 8) | ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x000000000000FF00ULL) << 40) | ((x & 0x00000000000000FFULL) << 56);
}
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BTREE_IS_BIG_ENDIAN 1
#else
#define BTREE_IS_BIG_ENDIAN 0
#endif

/* lexicographic (memcmp-order) compare of two 8-byte keys via byte-swapped integer
 * compare -- matches memcmp and the skip_list 8-byte path. distinct from
 * btree_compare_keys_numeric_inline, which is native-endian for CMP_NUMERIC. */
static inline int btree_compare_keys_8_memcmp_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint64_t a, b;
    memcpy(&a, key1, sizeof(uint64_t));
    memcpy(&b, key2, sizeof(uint64_t));
#if !BTREE_IS_BIG_ENDIAN
    a = BTREE_BSWAP64(a);
    b = BTREE_BSWAP64(b);
#endif
    return (a < b) ? -1 : (a > b);
}

/**
 * btree_compare_keys_inline
 * inline comparator for hot paths
 * @param config btree configuration containing comparator settings
 * @param key1 first key
 * @param key1_size size of first key
 * @param key2 second key
 * @param key2_size size of second key
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
static inline int btree_compare_keys_inline(const btree_config_t *config, const uint8_t *key1,
                                            const size_t key1_size, const uint8_t *key2,
                                            const size_t key2_size)
{
    if (BTREE_LIKELY(config->cmp_type == BTREE_CMP_MEMCMP))
    {
        if (BTREE_LIKELY(key1_size == key2_size))
        {
            if (key1_size == 8)
            {
                return btree_compare_keys_8_memcmp_inline(key1, key2);
            }
            const int cmp = memcmp(key1, key2, key1_size);
            return (cmp == 0) ? 0 : ((cmp < 0) ? -1 : 1);
        }
        return btree_comparator_memcmp(key1, key1_size, key2, key2_size, NULL);
    }

    switch (config->cmp_type)
    {
        case BTREE_CMP_NUMERIC:
            return btree_compare_keys_numeric_inline(key1, key2);
        case BTREE_CMP_STRING:
            return btree_comparator_string(key1, key1_size, key2, key2_size, NULL);
        case BTREE_CMP_CUSTOM:
        default:
            return config->comparator(key1, key1_size, key2, key2_size, config->comparator_ctx);
    }
}

int btree_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                            size_t key2_size, void *ctx)
{
    (void)ctx;
    const size_t min_size = key1_size < key2_size ? key1_size : key2_size;
    const int cmp = memcmp(key1, key2, min_size);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    return (key1_size < key2_size) ? -1 : (key1_size > key2_size) ? 1 : 0;
}

int btree_comparator_string(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                            size_t key2_size, void *ctx)
{
    (void)ctx;
    /* length-bounded compare, keys are byte buffers, not guaranteed NUL-terminated.
     * strcmp here would read past the buffer on a non-terminated key. memcmp over the
     * shorter length plus a length tie-break gives the same order as strcmp for
     * well-formed C-string keys while staying in bounds. */
    const size_t min_size = key1_size < key2_size ? key1_size : key2_size;
    const int cmp = memcmp(key1, key2, min_size);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    if (key1_size < key2_size) return -1;
    if (key1_size > key2_size) return 1;
    return 0;
}

int btree_comparator_numeric(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                             size_t key2_size, void *ctx)
{
    (void)key1_size;
    (void)key2_size;
    (void)ctx;
    uint64_t val1, val2;
    memcpy(&val1, key1, sizeof(uint64_t));
    memcpy(&val2, key2, sizeof(uint64_t));
    if (val1 < val2) return -1;
    if (val1 > val2) return 1;
    return 0;
}

/**
 * btree_pending_leaf_t
 * a leaf node being built during tree construction
 * @param entries array of entry metadata
 * @param keys array of key pointers
 * @param values array of value pointers
 * @param num_entries current number of entries
 * @param capacity maximum capacity of arrays
 * @param current_size current serialized size estimate
 * @param first_key first key in this leaf (for separator)
 * @param first_key_size size of first key
 * @param last_key last key in this leaf
 * @param last_key_size size of last key
 */
typedef struct btree_pending_leaf_t
{
    btree_entry_t *entries;
    uint8_t **keys;
    uint8_t **values;
    uint32_t num_entries;
    uint32_t capacity;
    size_t current_size;
    uint8_t *first_key;
    size_t first_key_size;
    uint8_t *last_key;
    size_t last_key_size;
} btree_pending_leaf_t;

/**
 * btree_level_entry_t
 * entry for building internal nodes (separator key + child offset)
 * @param key separator key data
 * @param key_size size of separator key
 * @param child_offset offset of child node in storage
 */
typedef struct btree_level_entry_t
{
    uint8_t *key;
    size_t key_size;
    int64_t child_offset;
} btree_level_entry_t;

/**
 * btree_builder_t
 * builder state for constructing B+tree from sorted data
 * @param bm block manager for storage
 * @param config btree configuration
 * @param current_leaf leaf node currently being built
 * @param first_leaf_offset offset of first leaf in tree
 * @param last_leaf_offset offset of last leaf in tree
 * @param prev_leaf_offset offset of previously written leaf
 * @param leaf_offsets array of all leaf offsets for backpatching
 * @param num_leaf_offsets number of leaf offsets
 * @param leaf_offsets_capacity capacity of leaf_offsets array
 * @param level_entries entries for building internal nodes
 * @param num_level_entries number of level entries
 * @param level_entries_capacity capacity of level_entries array
 * @param entry_count total number of entries added
 * @param node_count total number of nodes written
 * @param max_seq maximum sequence number seen
 * @param min_key minimum key in tree
 * @param min_key_size size of minimum key
 * @param max_key maximum key in tree
 * @param max_key_size size of maximum key
 */
struct btree_builder_t
{
    block_manager_t *bm;
    block_manager_t *leaf_bm; /* uncompressed leaves stage here -- a temp file
                               * when compression is on, so the real klog never
                               * keeps the discarded pre-compression copies */
    btree_config_t config;

    btree_pending_leaf_t *current_leaf;
    int64_t first_leaf_offset;
    int64_t last_leaf_offset;
    int64_t prev_leaf_offset;

    int64_t *leaf_offsets;
    uint32_t num_leaf_offsets;
    uint32_t leaf_offsets_capacity;

    btree_level_entry_t *level_entries;
    uint32_t num_level_entries;
    uint32_t level_entries_capacity;

    uint64_t entry_count;
    uint64_t node_count;
    uint64_t max_seq;
    uint32_t height;

    uint8_t *min_key;
    size_t min_key_size;
    uint8_t *max_key;
    size_t max_key_size;
};

/**
 * btree_leaf_serialize
 * serializes a leaf node with optimized format:
 * -- varint encoding for sizes and metadata
 * -- prefix compression for keys
 * -- key indirection table for O(1) access
 * -- delta encoding for sequence numbers
 *
 * format:
 * [type:1][num_entries:varint][prev_offset:8][next_offset:8]
 * [key_offsets_table: num_entries * 2 bytes] -- offset from keys_start to each key
 * [base_seq:varint][entries: prefix_len:varint, suffix_len:varint, value_size:varint,
 *                           vlog_offset:varint, seq_delta:signed_varint, ttl:signed_varint,
 * flags:1] [keys: prefix-compressed][values]
 *
 * @param leaf the pending leaf to serialize
 * @param prev_offset offset of previous leaf node (-1 if first)
 * @param next_offset offset of next leaf node (-1 if last)
 * @param out output buffer (caller must free)
 * @param out_size output size of serialized data
 * @return 0 on success, -1 on failure
 */
static int btree_leaf_serialize(const btree_pending_leaf_t *leaf, const int64_t prev_offset,
                                const int64_t next_offset, uint8_t **out, size_t *out_size)
{
    if (!leaf || !out || !out_size) return -1;
    if (leaf->num_entries == 0)
    {
        /* empty leaf -- minimal format */
        uint8_t *buffer = malloc(BTREE_LEAF_EMPTY_BUF_SIZE);
        if (!buffer) return -1;
        size_t off = 0;
        buffer[off++] = BTREE_NODE_LEAF;
        off += btree_varint_encode(buffer + off, 0);
        encode_int64_le_compat(buffer + off, prev_offset);
        off += 8;
        encode_int64_le_compat(buffer + off, next_offset);
        off += 8;
        *out = buffer;
        *out_size = off;
        return 0;
    }

    /* we compute prefix lengths and compressed key sizes */
    size_t *prefix_lens = malloc(leaf->num_entries * sizeof(size_t));
    size_t *suffix_lens = malloc(leaf->num_entries * sizeof(size_t));
    if (!prefix_lens || !suffix_lens)
    {
        free(prefix_lens);
        free(suffix_lens);
        return -1;
    }

    /* first key has no prefix compression */
    prefix_lens[0] = 0;
    suffix_lens[0] = leaf->entries[0].key_size;

    for (uint32_t i = 1; i < leaf->num_entries; i++)
    {
        prefix_lens[i] = btree_compute_prefix_len(leaf->keys[i - 1], leaf->entries[i - 1].key_size,
                                                  leaf->keys[i], leaf->entries[i].key_size);
        suffix_lens[i] = leaf->entries[i].key_size - prefix_lens[i];
    }

    /* we find base sequence number (minimum) for delta encoding */
    uint64_t base_seq = leaf->entries[0].seq;
    for (uint32_t i = 1; i < leaf->num_entries; i++)
    {
        if (leaf->entries[i].seq < base_seq) base_seq = leaf->entries[i].seq;
    }

    /* we calculate total size needed */
    size_t est_size = 1;                              /* type */
    est_size += btree_varint_size(leaf->num_entries); /* num_entries */
    est_size += 16;                                   /* prev/next offsets */
    est_size += leaf->num_entries * 2;                /* key indirection table */
    est_size += btree_varint_size(base_seq);          /* base_seq */

    size_t keys_total = 0;
    size_t values_total = 0;
    for (uint32_t i = 0; i < leaf->num_entries; i++)
    {
        est_size += btree_varint_size(prefix_lens[i]);
        est_size += btree_varint_size(suffix_lens[i]);
        est_size += btree_varint_size(leaf->entries[i].value_size);
        est_size += btree_varint_size(leaf->entries[i].vlog_offset);
        const int64_t seq_delta = (int64_t)(leaf->entries[i].seq - base_seq);
        est_size += btree_varint_size(((uint64_t)seq_delta << 1) ^ (uint64_t)(seq_delta >> 63));
        est_size += btree_varint_size(((uint64_t)leaf->entries[i].ttl << 1) ^
                                      (uint64_t)(leaf->entries[i].ttl >> 63));
        est_size += 1; /* flags */
        keys_total += suffix_lens[i];
        if (leaf->entries[i].vlog_offset == 0 && leaf->values[i])
        {
            values_total += leaf->entries[i].value_size;
        }
    }
    est_size += keys_total + values_total;

    uint8_t *buffer = malloc(est_size + BTREE_LEAF_SERIALIZE_SAFETY_PAD);
    if (!buffer)
    {
        free(prefix_lens);
        free(suffix_lens);
        return -1;
    }

    size_t off = 0;

    /* header */
    buffer[off++] = BTREE_NODE_LEAF;
    off += btree_varint_encode(buffer + off, leaf->num_entries);
    encode_int64_le_compat(buffer + off, prev_offset);
    off += 8;
    encode_int64_le_compat(buffer + off, next_offset);
    off += 8;

    /* key indirection table placeholder -- we'll fill this after writing keys */
    const size_t indirection_table_pos = off;
    off += leaf->num_entries * 2;

    /* base sequence number */
    off += btree_varint_encode(buffer + off, base_seq);

    /* entry metadata (varint encoded) */
    for (uint32_t i = 0; i < leaf->num_entries; i++)
    {
        off += btree_varint_encode(buffer + off, prefix_lens[i]);
        off += btree_varint_encode(buffer + off, suffix_lens[i]);
        off += btree_varint_encode(buffer + off, leaf->entries[i].value_size);
        off += btree_varint_encode(buffer + off, leaf->entries[i].vlog_offset);
        int64_t seq_delta = (int64_t)(leaf->entries[i].seq - base_seq);
        off += btree_signed_varint_encode(buffer + off, seq_delta);
        off += btree_signed_varint_encode(buffer + off, leaf->entries[i].ttl);
        buffer[off++] = leaf->entries[i].flags;
    }

    /* keys (prefix-compressed -- only suffix stored) */
    size_t keys_start = off;
    for (uint32_t i = 0; i < leaf->num_entries; i++)
    {
        /* we write key offset as little-endian uint16. if the keys section exceeds
         * 64KB the offset wraps and deserialization will read garbage. */
        const size_t raw_off = off - keys_start;
        if (raw_off > UINT16_MAX)
        {
            free(prefix_lens);
            free(suffix_lens);
            return -1;
        }
        const uint16_t key_off = (uint16_t)raw_off;
        buffer[indirection_table_pos + i * 2] = (uint8_t)(key_off & 0xFF);
        buffer[indirection_table_pos + i * 2 + 1] = (uint8_t)((key_off >> 8) & 0xFF);
        memcpy(buffer + off, leaf->keys[i] + prefix_lens[i], suffix_lens[i]);
        off += suffix_lens[i];
    }

    /* values (inline only) */
    for (uint32_t i = 0; i < leaf->num_entries; i++)
    {
        if (leaf->entries[i].vlog_offset == 0 && leaf->values[i])
        {
            memcpy(buffer + off, leaf->values[i], leaf->entries[i].value_size);
            off += leaf->entries[i].value_size;
        }
    }

    free(prefix_lens);
    free(suffix_lens);

    *out = buffer;
    *out_size = off;
    return 0;
}

/**
 * btree_internal_serialize
 * serializes an internal node with optimized format:
 * -- varint encoding for counts and key sizes
 * -- delta encoding for child offsets
 * -- prefix compression for separator keys
 *
 * format:
 * [type:1][num_keys:varint][base_offset:8][child_offset_deltas:signed_varint*N]
 * [key_sizes:varint*(N-1)][keys:prefix-compressed]
 *
 * @param entries internal node entries
 * @param num_entries number of entries
 * @param out output parameter for serialized node
 * @param out_size output parameter for serialized node size
 * @return 0 on success, -1 on failure
 */
static int btree_internal_serialize(const btree_level_entry_t *entries, const uint32_t num_entries,
                                    uint8_t **out, size_t *out_size)
{
    if (!entries || num_entries == 0 || !out || !out_size) return -1;

    const uint32_t num_keys = (num_entries > 1) ? num_entries - 1 : 0;
    const uint32_t num_children = num_entries;

    /* we estimate size needed */
    size_t est_size = 1;                     /* type */
    est_size += btree_varint_size(num_keys); /* num_keys */
    est_size += 8;                           /* base_offset */
    est_size += num_children * 10;           /* child offset deltas (worst case) */

    size_t keys_size = 0;
    for (uint32_t i = 1; i < num_entries; i++)
    {
        est_size += btree_varint_size(entries[i].key_size);
        keys_size += entries[i].key_size;
    }
    est_size += keys_size;

    uint8_t *buffer = malloc(est_size + BTREE_INTERNAL_SERIALIZE_SAFETY_PAD);
    if (!buffer) return -1;

    size_t off = 0;

    buffer[off++] = BTREE_NODE_INTERNAL;
    off += btree_varint_encode(buffer + off, num_keys);

    /* we base offset is the first child offset */
    const int64_t base_offset = entries[0].child_offset;
    encode_int64_le_compat(buffer + off, base_offset);
    off += 8;

    /* child offset deltas */
    int64_t prev_offset = base_offset;
    for (uint32_t i = 0; i < num_children; i++)
    {
        const int64_t delta = entries[i].child_offset - prev_offset;
        off += btree_signed_varint_encode(buffer + off, delta);
        prev_offset = entries[i].child_offset;
    }

    /* we separator key sizes (varint) */
    for (uint32_t i = 1; i < num_entries; i++)
    {
        off += btree_varint_encode(buffer + off, entries[i].key_size);
    }

    for (uint32_t i = 1; i < num_entries; i++)
    {
        memcpy(buffer + off, entries[i].key, entries[i].key_size);
        off += entries[i].key_size;
    }

    *out = buffer;
    *out_size = off;
    return 0;
}

/**
 * btree_node_deserialize_arena
 * deserializes a node from optimized format using arena allocation
 * all memory is allocated from the arena for O(1) bulk deallocation
 * @param data node bytes
 * @param data_size node size
 * @param node output parameter for deserialized node
 * @param arena arena allocator to use
 * @return 0 on success, -1 on failure
 */
static int btree_node_deserialize_arena(const uint8_t *data, const size_t data_size,
                                        btree_node_t **node, btree_arena_t *arena)
{
    if (!data || data_size < 2 || !node || !arena) return -1;

    const uint8_t *const end = data + data_size;

    btree_node_t *n = btree_arena_alloc(arena, sizeof(btree_node_t));
    if (!n) return -1;
    memset(n, 0, sizeof(btree_node_t));
    n->arena = arena;

    size_t off = 0;
    n->type = data[off++]; /* data_size >= 2 guarantees this byte */

    /* every read below is bounds-checked against data_size -- on-disk node bytes are
     * untrusted (a malformed/truncated node must be rejected, never over-read). on a
     * violation the caller destroys the arena, so we just return -1. */
#define BT_NEED(want)                                                       \
    do                                                                      \
    {                                                                       \
        if (off > data_size || (size_t)(want) > data_size - off) return -1; \
    } while (0)
#define BT_VARINT(dst)                                                           \
    do                                                                           \
    {                                                                            \
        const size_t _vn = btree_varint_decode_bounded(data + off, end, &(dst)); \
        if (_vn == 0) return -1;                                                 \
        off += _vn;                                                              \
    } while (0)
#define BT_SVARINT(dst)                                                                 \
    do                                                                                  \
    {                                                                                   \
        const size_t _vn = btree_signed_varint_decode_bounded(data + off, end, &(dst)); \
        if (_vn == 0) return -1;                                                        \
        off += _vn;                                                                     \
    } while (0)

    uint64_t num_entries_u64;
    BT_VARINT(num_entries_u64);
    if (num_entries_u64 > UINT32_MAX) return -1;
    n->num_entries = (uint32_t)num_entries_u64;

    if (n->type == BTREE_NODE_LEAF)
    {
        BT_NEED(16);
        n->prev_offset = decode_int64_le_compat(data + off);
        off += 8;
        n->next_offset = decode_int64_le_compat(data + off);
        off += 8;

        if (n->num_entries > 0)
        {
            const uint32_t ne = n->num_entries;

            /* the indirection table alone needs ne*2 bytes -- reject an ne that can't
             * fit before allocating ne-sized arrays */
            BT_NEED((size_t)ne * 2);

            /* single arena alloc for all 4 metadata arrays */
            const size_t entries_sz = ne * sizeof(btree_entry_t);
            const size_t keys_ptr_sz = ne * sizeof(uint8_t *);
            const size_t key_sizes_sz = ne * sizeof(size_t);
            const size_t values_ptr_sz = ne * sizeof(uint8_t *);
            const size_t meta_total = entries_sz + keys_ptr_sz + key_sizes_sz + values_ptr_sz;
            uint8_t *meta_buf = btree_arena_alloc(arena, meta_total);
            if (!meta_buf) return -1;

            n->entries = (btree_entry_t *)meta_buf;
            n->keys = (uint8_t **)(meta_buf + entries_sz);
            n->key_sizes = (size_t *)(meta_buf + entries_sz + keys_ptr_sz);
            n->values = (uint8_t **)(meta_buf + entries_sz + keys_ptr_sz + key_sizes_sz);

            /* only values needs zeroing (sparse -- vlog entries have no inline value) */
            memset(n->values, 0, values_ptr_sz);

            /* single arena alloc for all 3 temp arrays (align offsets_sz so size_t arrays
             * start on an 8-byte boundary) */
            const size_t offsets_sz = ((ne * sizeof(uint16_t)) + 7) & ~(size_t)7;
            const size_t lens_sz = ne * sizeof(size_t);
            const size_t temp_total = offsets_sz + lens_sz + lens_sz;
            uint8_t *temp_buf = btree_arena_alloc(arena, temp_total);
            if (!temp_buf) return -1;

            uint16_t *key_offsets = (uint16_t *)temp_buf;
            size_t *prefix_lens = (size_t *)(temp_buf + offsets_sz);
            size_t *suffix_lens = (size_t *)(temp_buf + offsets_sz + lens_sz);

            /* we read key indirection table (stored as little-endian uint16) -- bounded
             * by the BT_NEED(ne*2) above */
            for (uint32_t i = 0; i < ne; i++)
            {
                key_offsets[i] = (uint16_t)(data[off] | (data[off + 1] << 8));
                off += 2;
            }

            /* we read base sequence number */
            uint64_t base_seq;
            BT_VARINT(base_seq);

            /* we read entry metadata */
            for (uint32_t i = 0; i < ne; i++)
            {
                uint64_t prefix_len, suffix_len, value_size, vlog_offset;
                int64_t seq_delta, ttl;

                BT_VARINT(prefix_len);
                BT_VARINT(suffix_len);
                BT_VARINT(value_size);
                BT_VARINT(vlog_offset);
                BT_SVARINT(seq_delta);
                BT_SVARINT(ttl);
                BT_NEED(1); /* flags byte */

                /* key_size must fit uint32; prefix can't exceed the previous key's
                 * length (the prefix is copied from it during reconstruction) */
                const uint64_t key_size = prefix_len + suffix_len;
                if (key_size > UINT32_MAX) return -1;
                if (i == 0 ? (prefix_len != 0) : (prefix_len > n->entries[i - 1].key_size))
                    return -1;

                prefix_lens[i] = (size_t)prefix_len;
                suffix_lens[i] = (size_t)suffix_len;
                n->entries[i].key_size = (uint32_t)key_size;
                n->entries[i].value_size = (uint32_t)value_size;
                n->entries[i].vlog_offset = vlog_offset;
                n->entries[i].seq = base_seq + (uint64_t)seq_delta;
                n->entries[i].ttl = ttl;
                n->entries[i].flags = data[off++];
                n->key_sizes[i] = n->entries[i].key_size;
            }

            /* single arena alloc for all key data, then carve up with pointers */
            size_t total_key_bytes = 0;
            for (uint32_t i = 0; i < ne; i++)
            {
                total_key_bytes += ((size_t)n->entries[i].key_size + 7) & ~(size_t)7;
            }

            uint8_t *key_buf = btree_arena_alloc(arena, total_key_bytes);
            if (!key_buf) return -1;

            /* we reconstruct keys from prefix-compressed format */
            const size_t keys_start = off;
            size_t key_buf_off = 0;
            for (uint32_t i = 0; i < ne; i++)
            {
                n->keys[i] = key_buf + key_buf_off;

                /* we copy prefix from previous key (prefix_len validated <= prev key_size) */
                if (i > 0 && prefix_lens[i] > 0)
                {
                    memcpy(n->keys[i], n->keys[i - 1], prefix_lens[i]);
                }

                /* we copy suffix from serialized data -- the suffix region must lie
                 * entirely within the node */
                const size_t suffix_pos = keys_start + key_offsets[i];
                if (suffix_pos > data_size || suffix_lens[i] > data_size - suffix_pos) return -1;
                memcpy(n->keys[i] + prefix_lens[i], data + suffix_pos, suffix_lens[i]);

                key_buf_off += ((size_t)n->entries[i].key_size + 7) & ~(size_t)7;
            }

            /* we advance past all key data */
            for (uint32_t i = 0; i < ne; i++)
            {
                off += suffix_lens[i];
            }
            if (off > data_size) return -1; /* keys section overran the node */

            /* single arena alloc for all inline values, then point each into it */
            size_t total_inline_bytes = 0;
            for (uint32_t i = 0; i < ne; i++)
            {
                if (n->entries[i].vlog_offset == 0 && n->entries[i].value_size > 0)
                {
                    total_inline_bytes += n->entries[i].value_size;
                    if (total_inline_bytes > data_size) return -1; /* cap + overflow guard */
                }
            }

            if (total_inline_bytes > 0)
            {
                BT_NEED(total_inline_bytes);
                uint8_t *val_buf = btree_arena_alloc(arena, total_inline_bytes);
                if (!val_buf) return -1;
                memcpy(val_buf, data + off, total_inline_bytes);

                size_t val_off = 0;
                for (uint32_t i = 0; i < ne; i++)
                {
                    if (n->entries[i].vlog_offset == 0 && n->entries[i].value_size > 0)
                    {
                        n->values[i] = val_buf + val_off;
                        val_off += n->entries[i].value_size;
                    }
                }
            }
            off += total_inline_bytes;
        }
    }
    else if (n->type == BTREE_NODE_INTERNAL)
    {
        const uint32_t num_keys = n->num_entries;
        const uint32_t num_children = num_keys + 1;

        /* single arena alloc for child_offsets + keys ptrs + key_sizes */
        const size_t child_sz = num_children * sizeof(int64_t);
        const size_t ikeys_ptr_sz = num_keys * sizeof(uint8_t *);
        const size_t ikey_sizes_sz = num_keys * sizeof(size_t);
        const size_t internal_total = child_sz + ikeys_ptr_sz + ikey_sizes_sz;
        uint8_t *ibuf = btree_arena_alloc(arena, internal_total);
        if (!ibuf) return -1;

        n->child_offsets = (int64_t *)ibuf;
        n->keys = (num_keys > 0) ? (uint8_t **)(ibuf + child_sz) : NULL;
        n->key_sizes = (num_keys > 0) ? (size_t *)(ibuf + child_sz + ikeys_ptr_sz) : NULL;

        BT_NEED(8);
        int64_t base_offset = decode_int64_le_compat(data + off);
        off += 8;

        /* we decode delta-encoded child offsets */
        int64_t prev_offset = base_offset;
        for (uint32_t i = 0; i < num_children; i++)
        {
            int64_t delta;
            BT_SVARINT(delta);
            n->child_offsets[i] = prev_offset + delta;
            prev_offset = n->child_offsets[i];
        }

        /* we read key sizes (varint) */
        for (uint32_t i = 0; i < num_keys; i++)
        {
            uint64_t key_size;
            BT_VARINT(key_size);
            if (key_size > UINT32_MAX) return -1;
            n->key_sizes[i] = (size_t)key_size;
        }

        /* single arena alloc for all separator key data */
        size_t total_ikey_bytes = 0;
        for (uint32_t i = 0; i < num_keys; i++)
        {
            total_ikey_bytes += (n->key_sizes[i] + 7) & ~(size_t)7;
        }

        if (total_ikey_bytes > 0)
        {
            uint8_t *ikey_buf = btree_arena_alloc(arena, total_ikey_bytes);
            if (!ikey_buf) return -1;

            size_t ikey_off = 0;
            for (uint32_t i = 0; i < num_keys; i++)
            {
                n->keys[i] = ikey_buf + ikey_off;
                BT_NEED(n->key_sizes[i]);
                memcpy(n->keys[i], data + off, n->key_sizes[i]);
                off += n->key_sizes[i];
                ikey_off += (n->key_sizes[i] + 7) & ~(size_t)7;
            }
        }
    }

#undef BT_NEED
#undef BT_VARINT
#undef BT_SVARINT

    *node = n;
    return 0;
}

void btree_node_free(btree_node_t *node)
{
    if (!node) return;

    /* for arena-allocated nodes we destroy arena for O(1) bulk deallocation
     * for uncached nodes only -- cached nodes use btree_cached_node_release */
    if (node->arena)
    {
        btree_arena_destroy(node->arena);
        return;
    }

    if (node->keys)
    {
        for (uint32_t i = 0; i < node->num_entries; i++)
        {
            free(node->keys[i]);
        }
        free(node->keys);
    }

    if (node->values)
    {
        for (uint32_t i = 0; i < node->num_entries; i++)
        {
            free(node->values[i]);
        }
        free(node->values);
    }

    free(node->entries);
    free(node->key_sizes);
    free(node->child_offsets);
    free(node);
}

/**
 * btree_cached_node_release
 * release a reference to a cached btree node
 * frees the node when the last reference is released
 * @param node the cached node to release
 */
static void btree_cached_node_release(btree_node_t *node)
{
    if (!node) return;
    if (atomic_fetch_sub_explicit(&node->rc_count, 1, memory_order_acq_rel) == 1)
    {
        if (node->arena)
        {
            btree_arena_destroy(node->arena);
        }
        else
        {
            btree_node_free(node);
        }
    }
}

/**
 * btree_node_done
 * release a node returned by btree_node_read_cached
 * handles both cached (ref-counted) and non-cached (direct free) nodes
 * @param node the node to release
 * @param cached 1 if node came from cache, 0 if direct read
 */
static inline void btree_node_done(btree_node_t *node, const int cached)
{
    if (!node) return;
    if (cached)
        btree_cached_node_release(node);
    else
        btree_node_free(node);
}

static void btree_node_cache_evict_callback(void *payload, size_t payload_len)
{
    if (payload && payload_len == sizeof(btree_node_t *))
    {
        btree_node_t *node;
        memcpy(&node, payload, sizeof(btree_node_t *));
        if (node) btree_cached_node_release(node);
    }
}

int btree_node_read(block_manager_t *bm, const int64_t offset, btree_node_t **node)
{
    return btree_node_read_with_compression(bm, offset, node, TDB_COMPRESS_NONE);
}

int btree_node_read_with_compression(block_manager_t *bm, const int64_t offset, btree_node_t **node,
                                     const int compression_algo)
{
    if (!bm || offset < 0 || !node) return -1;

    block_manager_cursor_t cursor;
    if (block_manager_cursor_init_stack(&cursor, bm) != 0) return -1;

    if (block_manager_cursor_goto(&cursor, (uint64_t)offset) != 0) return -1;

    block_manager_block_t *block = block_manager_cursor_read(&cursor);
    if (!block) return -1;

    /* we decompress if compression is enabled
     * format -- [original_size:4][prev_offset:8][next_offset:8][compressed_data] */
    const uint8_t *data = block->data;
    size_t data_size = block->size;
    uint8_t *decompressed = NULL;

    if (compression_algo != TDB_COMPRESS_NONE && block->size > BTREE_COMPRESSED_NODE_HEADER_SIZE)
    {
        const uint8_t *block_data = (const uint8_t *)block->data;
        const uint32_t original_size = decode_uint32_le_compat(block_data);
        const int64_t header_prev_offset =
            decode_int64_le_compat(block_data + BTREE_COMPRESSED_NODE_PREV_OFF);
        const int64_t header_next_offset =
            decode_int64_le_compat(block_data + BTREE_COMPRESSED_NODE_NEXT_OFF);
        const uint8_t *compressed_data = block_data + BTREE_COMPRESSED_NODE_HEADER_SIZE;
        const size_t compressed_size = block->size - BTREE_COMPRESSED_NODE_HEADER_SIZE;

        size_t decompressed_size;
        decompressed = decompress_data(compressed_data, compressed_size, &decompressed_size,
                                       (compression_algorithm)compression_algo);
        if (decompressed && decompressed_size == original_size)
        {
            /* we only patch prev_offset and next_offset for leaf nodes, not internal nodes */
            if (decompressed[0] == BTREE_NODE_LEAF)
            {
                /* we calculate position -- type(1) + num_entries(varint) */
                size_t pos = 1;
                uint64_t num_entries;
                pos += btree_varint_decode(decompressed + pos, &num_entries);
                /* now pos points to prev_offset -- we write in little-endian format */
                encode_int64_le_compat(decompressed + pos, header_prev_offset);
                encode_int64_le_compat(decompressed + pos + 8, header_next_offset);
            }
            data = decompressed;
            data_size = decompressed_size;
        }
        else
        {
            free(decompressed);
            block_manager_block_free(block);
            return -1;
        }
    }

    /* we use arena allocation to eliminate N+7 individual malloc/free per node read
     * btree_node_free will destroy the arena via O(1) bulk deallocation.
     * we size the arena to data_size * 2 since deserialized form (pointers, arrays)
     * is typically 1-2x the serialized size. avoids 64KB default for small nodes. */
    btree_arena_t *arena = btree_arena_create_sized(data_size * 2);
    if (!arena)
    {
        free(decompressed);
        block_manager_block_free(block);
        return -1;
    }

    const int result = btree_node_deserialize_arena(data, data_size, node, arena);
    if (result == 0)
    {
        (*node)->block_offset = offset;
    }
    else
    {
        btree_arena_destroy(arena);
    }

    free(decompressed);
    block_manager_block_free(block);
    return result;
}

/**
 * btree_u64_to_hex
 * fast uint64 to hex string conversion (avoids snprintf overhead)
 * @param val value to convert
 * @param buf output buffer (must be at least 17 bytes)
 * @return number of characters written
 */
static inline int btree_u64_to_hex(uint64_t val, char *buf)
{
    static const char hex_chars[] = "0123456789abcdef";
    if (val == 0)
    {
        buf[0] = '0';
        return 1;
    }
    char tmp[BTREE_U64_HEX_MAX];
    int len = 0;
    while (val > 0)
    {
        tmp[len++] = hex_chars[val & 0xF];
        val >>= 4;
    }
    for (int i = 0; i < len; i++)
    {
        buf[i] = tmp[len - 1 - i];
    }
    return len;
}

int btree_format_cache_key_prefix(const uint64_t cache_key_prefix, char *out)
{
    if (!out) return 0;
    int len = btree_u64_to_hex(cache_key_prefix, out);
    out[len++] = BTREE_CACHE_KEY_SEPARATOR;
    return len;
}

/**
 * btree_node_read_cached
 * reads a node with caching support
 * caches deserialized nodes directly for maximum performance
 * if cache hit, returns pointer to cached node (caller must not free)
 * if cache miss, reads from disk, deserializes, and caches
 * @param tree btree instance
 * @param offset node offset
 * @param node output parameter for deserialized node
 * @return 0 on success, -1 on failure
 */
static int btree_node_read_cached(btree_t *tree, const int64_t offset, btree_node_t **node)
{
    if (!tree || !tree->bm || offset < 0 || !node) return -1;

    /* if no cache, we fall back to direct read with compression */
    if (!tree->node_cache)
    {
        return btree_node_read_with_compression(tree->bm, offset, node,
                                                tree->config.compression_algo);
    }

    char cache_key[BTREE_CACHE_KEY_SIZE];
    int key_len = btree_format_cache_key_prefix(tree->cache_key_prefix, cache_key);
    key_len += btree_u64_to_hex((uint64_t)offset, cache_key + key_len);

    size_t cached_size = 0;
    clock_cache_entry_t *entry = NULL;
    const uint8_t *cached_ptr = clock_cache_get_zero_copy(tree->node_cache, cache_key,
                                                          (size_t)key_len, &cached_size, &entry);

    if (cached_ptr && cached_size == sizeof(btree_node_t *))
    {
        /* cache hit -- acquire caller ref before releasing cache entry
         * this prevents eviction from freeing the node while we use it */
        btree_node_t *cached_node;
        memcpy(&cached_node, cached_ptr, sizeof(btree_node_t *));
        atomic_fetch_add_explicit(&cached_node->rc_count, 1, memory_order_relaxed);
        clock_cache_release(entry);
        *node = cached_node;
        return 0;
    }

    if (entry) clock_cache_release(entry);

    /* cache miss! we read from disk (block manager handles checksum verification) */
    block_manager_cursor_t cursor;
    if (block_manager_cursor_init_stack(&cursor, tree->bm) != 0) return -1;

    if (block_manager_cursor_goto(&cursor, (uint64_t)offset) != 0) return -1;

    block_manager_block_t *block = block_manager_cursor_read(&cursor);
    if (!block) return -1;

    /* we decompress if compression is enabled
     * format -- [original_size:4][prev_offset:8][next_offset:8][compressed_data] */
    const uint8_t *data = block->data;
    size_t data_size = block->size;
    uint8_t *decompressed = NULL;

    if (tree->config.compression_algo != TDB_COMPRESS_NONE && block->size > 20)
    {
        const uint8_t *block_data = (const uint8_t *)block->data;
        const uint32_t original_size = decode_uint32_le_compat(block_data);
        int64_t header_prev_offset = decode_int64_le_compat(block_data + 4);
        int64_t header_next_offset = decode_int64_le_compat(block_data + 12);
        const uint8_t *compressed_data = block_data + 20;
        const size_t compressed_size = block->size - 20;

        size_t decompressed_size;
        decompressed = decompress_data(compressed_data, compressed_size, &decompressed_size,
                                       (compression_algorithm)tree->config.compression_algo);
        if (decompressed && decompressed_size == original_size)
        {
            /* we only patch prev_offset and next_offset for leaf nodes, not internal nodes */
            if (decompressed[0] == BTREE_NODE_LEAF)
            {
                /* we calculate position, type(1) + num_entries(varint) */
                size_t pos = 1;
                uint64_t num_entries;
                pos += btree_varint_decode(decompressed + pos, &num_entries);
                /* now pos points to prev_offset - write in little-endian format */
                encode_int64_le_compat(decompressed + pos, header_prev_offset);
                encode_int64_le_compat(decompressed + pos + 8, header_next_offset);
            }
            data = decompressed;
            data_size = decompressed_size;
        }
        else
        {
            free(decompressed);
            block_manager_block_free(block);
            return -1;
        }
    }

    btree_node_t *new_node = NULL;
    btree_arena_t *node_arena = btree_arena_create_sized(data_size * 2);
    if (!node_arena)
    {
        free(decompressed);
        block_manager_block_free(block);
        return -1;
    }

    const int result = btree_node_deserialize_arena(data, data_size, &new_node, node_arena);
    free(decompressed);
    block_manager_block_free(block);

    if (result != 0)
    {
        btree_arena_destroy(node_arena);
        return -1;
    }

    new_node->block_offset = offset;
    new_node->arena = node_arena;

    /* rc_count = 2, 1 for cache ownership + 1 for caller */
    atomic_store_explicit(&new_node->rc_count, 2, memory_order_relaxed);

    /* we account for actual memory cost i.e node struct + arena allocations.
     * without this the cache treats every node as 0 bytes and never evicts,
     * causing unbounded memory growth under btree workloads. */
    const size_t node_cost = sizeof(btree_node_t) + node_arena->total_allocated;
    clock_cache_put(tree->node_cache, cache_key, (size_t)key_len, &new_node, sizeof(btree_node_t *),
                    node_cost);

    *node = new_node;
    return 0;
}

/**
 * btree_pending_leaf_create
 * creates a new pending leaf for building during tree construction
 * @return new pending leaf or NULL on failure
 */
static btree_pending_leaf_t *btree_pending_leaf_create(void)
{
    btree_pending_leaf_t *leaf = calloc(1, sizeof(btree_pending_leaf_t));
    if (!leaf) return NULL;

    leaf->capacity = BTREE_PENDING_LEAF_INITIAL_CAP;
    leaf->entries = calloc(leaf->capacity, sizeof(btree_entry_t));
    leaf->keys = calloc(leaf->capacity, sizeof(uint8_t *));
    leaf->values = calloc(leaf->capacity, sizeof(uint8_t *));

    if (!leaf->entries || !leaf->keys || !leaf->values)
    {
        free(leaf->entries);
        free(leaf->keys);
        free(leaf->values);
        free(leaf);
        return NULL;
    }

    return leaf;
}

/**
 * btree_pending_leaf_free
 * frees a pending leaf and all associated memory
 * @param leaf the pending leaf to free
 */
static void btree_pending_leaf_free(btree_pending_leaf_t *leaf)
{
    if (!leaf) return;

    for (uint32_t i = 0; i < leaf->num_entries; i++)
    {
        free(leaf->keys[i]);
        free(leaf->values[i]);
    }

    free(leaf->entries);
    free(leaf->keys);
    free(leaf->values);
    free(leaf->first_key);
    free(leaf->last_key);
    free(leaf);
}

/**
 * btree_pending_leaf_add
 * adds an entry to a pending leaf during tree construction
 * @param leaf the pending leaf to add to
 * @param key key data
 * @param key_size size of key
 * @param value value data (may be NULL if vlog_offset > 0)
 * @param value_size size of value
 * @param vlog_offset offset in value log (0 for inline values)
 * @param seq sequence number
 * @param ttl time-to-live (-1 for no expiry)
 * @param flags entry flags (tombstone, etc.)
 * @return 0 on success, -1 on failure
 */
static int btree_pending_leaf_add(btree_pending_leaf_t *leaf, const uint8_t *key,
                                  const size_t key_size, const uint8_t *value,
                                  const size_t value_size, const uint64_t vlog_offset,
                                  const uint64_t seq, const int64_t ttl, const uint8_t flags)
{
    if (leaf->num_entries >= leaf->capacity)
    {
        const uint32_t new_capacity = leaf->capacity * 2;
        btree_entry_t *new_entries = realloc(leaf->entries, new_capacity * sizeof(btree_entry_t));
        uint8_t **new_keys = realloc(leaf->keys, new_capacity * sizeof(uint8_t *));
        uint8_t **new_values = realloc(leaf->values, new_capacity * sizeof(uint8_t *));

        if (!new_entries || !new_keys || !new_values)
        {
            return -1;
        }

        leaf->entries = new_entries;
        leaf->keys = new_keys;
        leaf->values = new_values;
        leaf->capacity = new_capacity;

        for (uint32_t i = leaf->num_entries; i < new_capacity; i++)
        {
            leaf->keys[i] = NULL;
            leaf->values[i] = NULL;
        }
    }

    const uint32_t idx = leaf->num_entries;

    leaf->keys[idx] = malloc(key_size);
    if (!leaf->keys[idx]) return -1;
    memcpy(leaf->keys[idx], key, key_size);

    if (vlog_offset == 0 && value && value_size > 0)
    {
        leaf->values[idx] = malloc(value_size);
        if (!leaf->values[idx])
        {
            free(leaf->keys[idx]);
            leaf->keys[idx] = NULL;
            return -1;
        }
        memcpy(leaf->values[idx], value, value_size);
    }
    else
    {
        leaf->values[idx] = NULL;
    }

    leaf->entries[idx].key_size = (uint32_t)key_size;
    leaf->entries[idx].value_size = (uint32_t)value_size;
    leaf->entries[idx].vlog_offset = vlog_offset;
    leaf->entries[idx].seq = seq;
    leaf->entries[idx].ttl = ttl;
    leaf->entries[idx].flags = flags;

    if (leaf->num_entries == 0)
    {
        leaf->first_key = malloc(key_size);
        if (leaf->first_key)
        {
            memcpy(leaf->first_key, key, key_size);
            leaf->first_key_size = key_size;
        }
    }

    free(leaf->last_key);
    leaf->last_key = malloc(key_size);
    if (leaf->last_key)
    {
        memcpy(leaf->last_key, key, key_size);
        leaf->last_key_size = key_size;
    }

    leaf->current_size += key_size + (vlog_offset == 0 ? value_size : 0) + sizeof(btree_entry_t);
    leaf->num_entries++;

    return 0;
}

int btree_builder_new(btree_builder_t **builder, block_manager_t *bm, const btree_config_t *config)
{
    if (!builder || !bm || !config) return -1;

    btree_builder_t *b = calloc(1, sizeof(btree_builder_t));
    if (!b) return -1;

    b->bm = bm;
    b->config = *config;

    if (!b->config.comparator)
    {
        b->config.comparator = btree_comparator_memcmp;
        b->config.cmp_type = BTREE_CMP_MEMCMP;
    }

    if (b->config.target_node_size == 0)
    {
        b->config.target_node_size = BTREE_DEFAULT_NODE_SIZE;
    }

    b->current_leaf = btree_pending_leaf_create();
    if (!b->current_leaf)
    {
        free(b);
        return -1;
    }

    b->first_leaf_offset = -1;
    b->last_leaf_offset = -1;
    b->prev_leaf_offset = -1;

    b->leaf_offsets_capacity = 256;
    b->leaf_offsets = calloc(b->leaf_offsets_capacity, sizeof(int64_t));
    if (!b->leaf_offsets)
    {
        btree_pending_leaf_free(b->current_leaf);
        free(b);
        return -1;
    }

    b->level_entries_capacity = 256;
    b->level_entries = calloc(b->level_entries_capacity, sizeof(btree_level_entry_t));
    if (!b->level_entries)
    {
        free(b->leaf_offsets);
        btree_pending_leaf_free(b->current_leaf);
        free(b);
        return -1;
    }

    /* uncompressed leaves are staged before compression. with compression on,
     * stage them in a temp file so the klog receives only the final compressed
     * leaves -- staging them in the klog would leave the discarded uncompressed
     * copies behind as permanent dead weight. with compression off the first
     * write is already final, so stage straight into the klog. */
    b->leaf_bm = bm;
    if (b->config.compression_algo != TDB_COMPRESS_NONE)
    {
        /* sizeof the suffix literal already includes its null terminator, so this
         * holds a full-length file_path plus the suffix without truncation */
        char tmp_path[MAX_FILE_PATH_LENGTH + sizeof(BTREE_LEAF_STAGE_SUFFIX)];
        snprintf(tmp_path, sizeof(tmp_path), "%s" BTREE_LEAF_STAGE_SUFFIX, bm->file_path);
        block_manager_t *tmp_bm = NULL;
        if (block_manager_open(&tmp_bm, tmp_path, BLOCK_MANAGER_SYNC_NONE) == 0 &&
            block_manager_truncate(tmp_bm) == 0)
        {
            b->leaf_bm = tmp_bm;
        }
        else if (tmp_bm)
        {
            /* temp file unavailable -- fall back to staging in the klog so the
             * build still succeeds (correctness over space) */
            block_manager_close(tmp_bm);
        }
    }

    *builder = b;
    return 0;
}

/**
 * btree_builder_flush_leaf
 * flushes the current pending leaf to storage
 * @param builder the builder instance
 * @return 0 on success, -1 on failure
 */
static int btree_builder_flush_leaf(btree_builder_t *builder)
{
    if (!builder || !builder->current_leaf || builder->current_leaf->num_entries == 0)
    {
        return 0;
    }

    uint8_t *serialized = NULL;
    size_t serialized_size = 0;

    if (btree_leaf_serialize(builder->current_leaf, builder->prev_leaf_offset, -1, &serialized,
                             &serialized_size) != 0)
    {
        return -1;
    }

    /**** leaf nodes are written without compression during build phase
     ***  because we need to backpatch next_offset links after all leaves are written.
     **   compression is applied during the backpatch phase after patching.
     *    we use from_buffer to transfer ownership and avoid redundant malloc+memcpy */
    block_manager_block_t *block =
        block_manager_block_create_from_buffer(serialized_size, serialized);

    if (!block) return -1;

    const int64_t offset = block_manager_block_write(builder->leaf_bm, block);
    block_manager_block_free(block);

    if (offset < 0) return -1;

    /* we track leaf offset for bidirectional linking */
    if (builder->num_leaf_offsets >= builder->leaf_offsets_capacity)
    {
        const uint32_t new_cap = builder->leaf_offsets_capacity * 2;
        int64_t *new_offsets = realloc(builder->leaf_offsets, new_cap * sizeof(int64_t));
        if (!new_offsets) return -1;
        builder->leaf_offsets = new_offsets;
        builder->leaf_offsets_capacity = new_cap;
    }
    builder->leaf_offsets[builder->num_leaf_offsets++] = offset;

    if (builder->first_leaf_offset < 0)
    {
        builder->first_leaf_offset = offset;
    }
    builder->last_leaf_offset = offset;

    if (builder->num_level_entries >= builder->level_entries_capacity)
    {
        const uint32_t new_cap = builder->level_entries_capacity * 2;
        btree_level_entry_t *new_entries =
            realloc(builder->level_entries, new_cap * sizeof(btree_level_entry_t));
        if (!new_entries) return -1;
        builder->level_entries = new_entries;
        builder->level_entries_capacity = new_cap;
    }

    btree_level_entry_t *entry = &builder->level_entries[builder->num_level_entries];
    entry->key = malloc(builder->current_leaf->first_key_size);
    if (!entry->key) return -1;
    memcpy(entry->key, builder->current_leaf->first_key, builder->current_leaf->first_key_size);
    entry->key_size = builder->current_leaf->first_key_size;
    entry->child_offset = offset;
    builder->num_level_entries++;

    builder->prev_leaf_offset = offset;
    builder->node_count++;

    btree_pending_leaf_free(builder->current_leaf);
    builder->current_leaf = btree_pending_leaf_create();

    return builder->current_leaf ? 0 : -1;
}

int btree_builder_add(btree_builder_t *builder, const uint8_t *key, const size_t key_size,
                      const uint8_t *value, const size_t value_size, const uint64_t vlog_offset,
                      const uint64_t seq, const int64_t ttl, const uint8_t entry_flags)
{
    if (!builder || !key || key_size == 0) return -1;

    uint8_t flags = entry_flags & (BTREE_ENTRY_FLAG_TOMBSTONE | BTREE_ENTRY_FLAG_SINGLE_DELETE);
    if (ttl != 0) flags |= BTREE_ENTRY_FLAG_HAS_TTL;
    if (vlog_offset > 0) flags |= BTREE_ENTRY_FLAG_VLOG_REF;

    /* we flush the full leaf before adding -- but never across a run of entries
     * that share a key. a key's versions must all stay within one leaf so
     * internal-node routing lands on the single leaf holding them and btree_get
     * can resolve the whole run. */
    if (builder->current_leaf->current_size >= builder->config.target_node_size &&
        builder->current_leaf->num_entries >= BTREE_MIN_ENTRIES_PER_LEAF)
    {
        const btree_pending_leaf_t *cur = builder->current_leaf;
        const int same_key_as_last = cur->last_key != NULL && cur->last_key_size == key_size &&
                                     memcmp(cur->last_key, key, key_size) == 0;
        if (!same_key_as_last && btree_builder_flush_leaf(builder) != 0)
        {
            return -1;
        }
    }

    if (btree_pending_leaf_add(builder->current_leaf, key, key_size, value, value_size, vlog_offset,
                               seq, ttl, flags) != 0)
    {
        return -1;
    }

    if (builder->min_key == NULL)
    {
        builder->min_key = malloc(key_size);
        if (builder->min_key)
        {
            memcpy(builder->min_key, key, key_size);
            builder->min_key_size = key_size;
        }
    }

    free(builder->max_key);
    builder->max_key = malloc(key_size);
    if (builder->max_key)
    {
        memcpy(builder->max_key, key, key_size);
        builder->max_key_size = key_size;
    }

    if (seq > builder->max_seq)
    {
        builder->max_seq = seq;
    }

    builder->entry_count++;
    return 0;
}

/**
 * btree_builder_build_internal_levels
 * builds internal node levels from leaf level entries
 * @param builder the builder instance
 * @param root_offset output parameter for the root node offset
 * @return 0 on success, -1 on failure
 */
static int btree_builder_build_internal_levels(btree_builder_t *builder, int64_t *root_offset)
{
    if (builder->num_level_entries == 0)
    {
        builder->height = 1;
        *root_offset = -1;
        return 0;
    }

    if (builder->num_level_entries == 1)
    {
        builder->height = 1; /* a single leaf is the whole tree */
        *root_offset = builder->level_entries[0].child_offset;
        return 0;
    }

    btree_level_entry_t *current_level = builder->level_entries;
    uint32_t current_count = builder->num_level_entries;

    /* each pass of the loop builds one internal level above the leaf level */
    uint32_t internal_levels = 0;

    while (current_count > 1)
    {
        const uint32_t next_capacity = (current_count / BTREE_DEFAULT_FANOUT) + 1;
        btree_level_entry_t *next_level = calloc(next_capacity, sizeof(btree_level_entry_t));
        if (!next_level) return -1;

        uint32_t next_count = 0;
        uint32_t i = 0;

        while (i < current_count)
        {
            uint32_t node_entries = BTREE_DEFAULT_FANOUT;
            if (i + node_entries > current_count)
            {
                node_entries = current_count - i;
            }

            uint8_t *serialized = NULL;
            size_t serialized_size = 0;

            if (btree_internal_serialize(&current_level[i], node_entries, &serialized,
                                         &serialized_size) != 0)
            {
                for (uint32_t j = 0; j < next_count; j++)
                {
                    free(next_level[j].key);
                }
                free(next_level);
                return -1;
            }

            /**** we compress if compression is enabled
             ***  format -- [original_size:4][prev_offset:8][next_offset:8][compressed_data]
             **   internal nodes use prev_offset=-1 and next_offset=-1 (unused) for consistent
             *format
             */
            const uint8_t *final_data = serialized;
            size_t final_size = serialized_size;
            uint8_t *block_with_header = NULL;

            if (builder->config.compression_algo != TDB_COMPRESS_NONE)
            {
                size_t compressed_size;
                uint8_t *compressed =
                    compress_data(serialized, serialized_size, &compressed_size,
                                  (compression_algorithm)builder->config.compression_algo);
                if (compressed)
                {
                    /** we create block with header:
                     *  [original_size:4][prev_offset:8][next_offset:8][compressed_data] */
                    const size_t header_size = 4 + 8 + 8;
                    final_size = header_size + compressed_size;
                    block_with_header = malloc(final_size);
                    if (block_with_header)
                    {
                        encode_uint32_le_compat(block_with_header, (uint32_t)serialized_size);
                        int64_t unused_prev = -1;
                        int64_t unused_next = -1;
                        encode_int64_le_compat(block_with_header + 4, unused_prev);
                        encode_int64_le_compat(block_with_header + 12, unused_next);
                        memcpy(block_with_header + header_size, compressed, compressed_size);
                        final_data = block_with_header;
                    }
                    free(compressed);
                }
            }

            block_manager_block_t *block = block_manager_block_create(final_size, final_data);
            free(serialized);
            free(block_with_header);

            if (!block)
            {
                for (uint32_t j = 0; j < next_count; j++)
                {
                    free(next_level[j].key);
                }
                free(next_level);
                return -1;
            }

            const int64_t offset = block_manager_block_write(builder->bm, block);
            block_manager_block_free(block);

            if (offset < 0)
            {
                for (uint32_t j = 0; j < next_count; j++)
                {
                    free(next_level[j].key);
                }
                free(next_level);
                return -1;
            }

            next_level[next_count].key = malloc(current_level[i].key_size);
            if (next_level[next_count].key)
            {
                memcpy(next_level[next_count].key, current_level[i].key, current_level[i].key_size);
                next_level[next_count].key_size = current_level[i].key_size;
            }
            next_level[next_count].child_offset = offset;
            next_count++;

            builder->node_count++;
            i += node_entries;
        }

        if (current_level != builder->level_entries)
        {
            for (uint32_t j = 0; j < current_count; j++)
            {
                free(current_level[j].key);
            }
            free(current_level);
        }

        current_level = next_level;
        current_count = next_count;
        internal_levels++;
    }

    builder->height = 1 + internal_levels;
    *root_offset = current_level[0].child_offset;

    if (current_level != builder->level_entries)
    {
        for (uint32_t j = 0; j < current_count; j++)
        {
            free(current_level[j].key);
        }
        free(current_level);
    }

    return 0;
}

/**
 * btree_builder_backpatch_leaf_links
 * patches next_offset in each leaf to point to the next leaf
 * this enables O(1) forward iteration through leaves
 *
 * block format -- [size(4)][checksum(4)][data][size(4)][magic(4)]
 * leaf data format -- [type:1][num_entries:varint][prev_offset:8][next_offset:8]...
 *
 * @param builder the builder instance
 * @return 0 on success, -1 on failure
 */
static int btree_builder_backpatch_leaf_links(btree_builder_t *builder)
{
    if (!builder || builder->num_leaf_offsets == 0) return 0;

    /* block header -- [size(4)][checksum(4)] = 8 bytes before data */
    const size_t block_header_size = BLOCK_MANAGER_BLOCK_HEADER_SIZE;

    /* we backpatch all leaves in place (theyre uncompressed at this point)
     * only needed if there are 2+ leaves */
    for (uint32_t i = 0; i + 1 < builder->num_leaf_offsets; i++)
    {
        const int64_t leaf_offset = builder->leaf_offsets[i];
        int64_t next_leaf_offset = builder->leaf_offsets[i + 1];

        block_manager_cursor_t cursor;
        cursor.bm = builder->leaf_bm;
        cursor.current_pos = leaf_offset;
        cursor.block_size_valid = 0;

        block_manager_block_t *block = block_manager_cursor_read(&cursor);
        if (!block) return -1;

        /* we calculate next_offset position type(1) + num_entries(varint) + prev_offset(8) */
        uint8_t *block_data = (uint8_t *)block->data;
        size_t off = 1; /* skip type byte */
        uint64_t num_entries;
        off += btree_varint_decode(block_data + off, &num_entries);
        off += 8; /* skip prev_offset, now at next_offset position */

        memcpy(block_data + off, &next_leaf_offset, sizeof(int64_t));

        const uint32_t new_checksum = XXH32(block->data, block->size, 0);

        uint8_t checksum_bytes[4];
        encode_uint32_le_compat(checksum_bytes, new_checksum);
        if (block_manager_write_at(builder->leaf_bm, leaf_offset + BLOCK_MANAGER_SIZE_FIELD_SIZE,
                                   checksum_bytes, 4) != 0)
        {
            block_manager_block_free(block);
            return -1;
        }

        if (block_manager_write_at(builder->leaf_bm, leaf_offset + block_header_size + off,
                                   (uint8_t *)&next_leaf_offset, sizeof(int64_t)) != 0)
        {
            block_manager_block_free(block);
            return -1;
        }

        block_manager_block_free(block);
    }

    /* if compression enabled, compress all leaves and write to new locations
     * format is [original_size:4][next_offset:8][compressed_data] stored in block
     * next_offset is stored in header so it can be patched without decompression */
    if (builder->config.compression_algo != TDB_COMPRESS_NONE)
    {
        int64_t *new_offsets = malloc(builder->num_leaf_offsets * sizeof(int64_t));
        if (!new_offsets) return -1;

        /* we compress and write all leaves with placeholder next_offset=-1 */
        for (uint32_t i = 0; i < builder->num_leaf_offsets; i++)
        {
            block_manager_cursor_t cursor;
            cursor.bm = builder->leaf_bm;
            cursor.current_pos = builder->leaf_offsets[i];
            cursor.block_size_valid = 0;

            block_manager_block_t *block = block_manager_cursor_read(&cursor);
            if (!block)
            {
                free(new_offsets);
                return -1;
            }

            /* we compress data (includes next_offset in the serialized leaf data) */
            size_t compressed_size;
            uint8_t *compressed =
                compress_data(block->data, block->size, &compressed_size,
                              (compression_algorithm)builder->config.compression_algo);
            const uint32_t original_size = (uint32_t)block->size;
            block_manager_block_free(block);

            if (!compressed)
            {
                free(new_offsets);
                return -1;
            }

            /* we create block with header:
             * [original_size:4][prev_offset:8][next_offset:8][compressed_data] */
            const size_t header_size = 4 + 8 + 8; /* original_size + prev_offset + next_offset */
            const size_t total_size = header_size + compressed_size;
            uint8_t *block_data = malloc(total_size);
            if (!block_data)
            {
                free(compressed);
                free(new_offsets);
                return -1;
            }
            encode_uint32_le_compat(block_data, original_size);
            int64_t placeholder_prev = -1;
            int64_t placeholder_next = -1;
            encode_int64_le_compat(block_data + 4, placeholder_prev);
            encode_int64_le_compat(block_data + 12, placeholder_next);
            memcpy(block_data + header_size, compressed, compressed_size);
            free(compressed);

            block_manager_block_t *new_block = block_manager_block_create(total_size, block_data);
            free(block_data);

            if (!new_block)
            {
                free(new_offsets);
                return -1;
            }

            const int64_t new_offset = block_manager_block_write(builder->bm, new_block);
            block_manager_block_free(new_block);

            if (new_offset < 0)
            {
                free(new_offsets);
                return -1;
            }

            new_offsets[i] = new_offset;
        }

        /* we patch prev_offset and next_offset in header and update checksum */
        for (uint32_t i = 0; i < builder->num_leaf_offsets; i++)
        {
            /* header format -- [original_size:4][prev_offset:8][next_offset:8][compressed_data] */
            /* block format  -- [block_size:4][checksum:4][data...] where data starts with our
             * header
             */
            const int64_t prev_patch_offset = new_offsets[i] + BLOCK_MANAGER_BLOCK_HEADER_SIZE + 4;
            const int64_t next_patch_offset = new_offsets[i] + BLOCK_MANAGER_BLOCK_HEADER_SIZE + 12;

            /* we patch prev_offset (first leaf has prev=-1, others point to previous new offset) */
            int64_t prev_leaf_offset = (i == 0) ? -1 : new_offsets[i - 1];
            if (block_manager_write_at(builder->bm, prev_patch_offset, (uint8_t *)&prev_leaf_offset,
                                       8) != 0)
            {
                free(new_offsets);
                return -1;
            }

            /* we patch next_offset (last leaf has next=-1, others point to next new offset) */
            int64_t next_leaf_offset =
                (i + 1 < builder->num_leaf_offsets) ? new_offsets[i + 1] : -1;
            if (block_manager_write_at(builder->bm, next_patch_offset, (uint8_t *)&next_leaf_offset,
                                       8) != 0)
            {
                free(new_offsets);
                return -1;
            }

            /* we update checksum after patching the block data */
            if (block_manager_update_checksum(builder->bm, new_offsets[i]) != 0)
            {
                free(new_offsets);
                return -1;
            }
        }

        /* we must update leaf_offsets and level_entries with new locations */
        for (uint32_t i = 0; i < builder->num_leaf_offsets; i++)
        {
            builder->leaf_offsets[i] = new_offsets[i];
        }
        for (uint32_t i = 0; i < builder->num_level_entries && i < builder->num_leaf_offsets; i++)
        {
            builder->level_entries[i].child_offset = new_offsets[i];
        }

        builder->first_leaf_offset = new_offsets[0];
        builder->last_leaf_offset = new_offsets[builder->num_leaf_offsets - 1];

        free(new_offsets);
    }

    return 0;
}

int btree_builder_finish(btree_builder_t *builder, btree_t **tree)
{
    if (!builder || !tree) return -1;

    if (builder->current_leaf && builder->current_leaf->num_entries > 0)
    {
        if (btree_builder_flush_leaf(builder) != 0)
        {
            return -1;
        }
    }

    if (btree_builder_backpatch_leaf_links(builder) != 0)
    {
        return -1;
    }

    int64_t root_offset = -1;
    if (btree_builder_build_internal_levels(builder, &root_offset) != 0)
    {
        return -1;
    }

    btree_t *t = calloc(1, sizeof(btree_t));
    if (!t) return -1;

    t->bm = builder->bm;
    t->config = builder->config;
    t->root_offset = root_offset;
    t->first_leaf_offset = builder->first_leaf_offset;
    t->last_leaf_offset = builder->last_leaf_offset;
    t->entry_count = builder->entry_count;
    t->node_count = builder->node_count;
    t->max_seq = builder->max_seq;
    t->height = builder->height ? builder->height : 1;

    if (builder->min_key)
    {
        t->min_key = builder->min_key;
        t->min_key_size = builder->min_key_size;
        builder->min_key = NULL;
    }

    if (builder->max_key)
    {
        t->max_key = builder->max_key;
        t->max_key_size = builder->max_key_size;
        builder->max_key = NULL;
    }

    *tree = t;
    return 0;
}

void btree_builder_free(btree_builder_t *builder)
{
    if (!builder) return;

    /* drop the temp leaf-staging file (only created when compression is on) */
    if (builder->leaf_bm && builder->leaf_bm != builder->bm)
    {
        char tmp_path[MAX_FILE_PATH_LENGTH];
        snprintf(tmp_path, sizeof(tmp_path), "%s", builder->leaf_bm->file_path);
        block_manager_close(builder->leaf_bm);
        remove(tmp_path);
    }

    btree_pending_leaf_free(builder->current_leaf);

    free(builder->leaf_offsets);

    if (builder->level_entries)
    {
        for (uint32_t i = 0; i < builder->num_level_entries; i++)
        {
            free(builder->level_entries[i].key);
        }
        free(builder->level_entries);
    }

    free(builder->min_key);
    free(builder->max_key);
    free(builder);
}

int btree_open(btree_t **tree, block_manager_t *bm, const btree_config_t *config,
               const int64_t root_offset, const int64_t first_leaf_offset,
               const int64_t last_leaf_offset)
{
    if (!tree || !bm || !config) return -1;

    btree_t *t = calloc(1, sizeof(btree_t));
    if (!t) return -1;

    t->bm = bm;
    t->config = *config;
    t->root_offset = root_offset;
    t->first_leaf_offset = first_leaf_offset;
    t->last_leaf_offset = last_leaf_offset;

    if (!t->config.comparator)
    {
        t->config.comparator = btree_comparator_memcmp;
        t->config.cmp_type = BTREE_CMP_MEMCMP;
    }

    *tree = t;
    return 0;
}

int btree_get_at_seq(btree_t *tree, const uint8_t *key, const size_t key_size,
                     const uint64_t seq_ceiling, uint8_t **value, size_t *value_size,
                     uint64_t *vlog_offset, uint64_t *seq, int64_t *ttl, uint8_t *deleted)
{
    if (!tree || !key || key_size == 0) return -1;

    if (tree->root_offset < 0) return -1;

    const int using_cache = (tree->node_cache != NULL);

    btree_node_t *node = NULL;
    if (btree_node_read_cached(tree, tree->root_offset, &node) != 0)
    {
        return -1;
    }

    while (node->type == BTREE_NODE_INTERNAL)
    {
        /* we utilize binary search for child index in internal node
         * find the largest i where key >= keys[i], then child_idx = i + 1
         * if key < keys[0], child_idx = 0. separator keys are strictly
         * increasing -- the builder never splits a key's run across leaves --
         * so a key's whole run lives in the one child this routes to. */
        uint32_t child_idx = 0;
        if (node->num_entries > 0)
        {
            int32_t lo = 0;
            int32_t hi = (int32_t)node->num_entries - 1;
            while (lo <= hi)
            {
                const int32_t mid = lo + (hi - lo) / 2;
                const int cmp = btree_compare_keys_inline(&tree->config, key, key_size,
                                                          node->keys[mid], node->key_sizes[mid]);
                if (cmp < 0)
                {
                    hi = mid - 1;
                }
                else
                {
                    lo = mid + 1;
                }
            }
            child_idx = (uint32_t)lo;
        }

        const int64_t child_offset = node->child_offsets[child_idx];

        btree_node_done(node, using_cache);

        if (btree_node_read_cached(tree, child_offset, &node) != 0)
        {
            return -1;
        }
    }

    /* lower_bound -- leftmost index whose key is >= the search key */
    int32_t lo = 0;
    int32_t hi = (int32_t)node->num_entries;
    while (lo < hi)
    {
        const int32_t mid = lo + (hi - lo) / 2;
        const int cmp = btree_compare_keys_inline(&tree->config, key, key_size, node->keys[mid],
                                                  node->key_sizes[mid]);
        if (cmp <= 0)
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1;
        }
    }

    /* scan the run of entries that share the search key, keeping the highest
     * seq that does not exceed seq_ceiling. a key may have several versions --
     * a flush or compaction retains a version chain -- and they all live in
     * this one leaf, so the resolved version is the one visible at the
     * caller's snapshot. */
    int32_t found_idx = -1;
    for (int32_t i = lo; i < (int32_t)node->num_entries; i++)
    {
        if (btree_compare_keys_inline(&tree->config, key, key_size, node->keys[i],
                                      node->key_sizes[i]) != 0)
        {
            break;
        }
        const uint64_t entry_seq = node->entries[i].seq;
        if (entry_seq > seq_ceiling) continue;
        if (found_idx < 0 || entry_seq > node->entries[found_idx].seq)
        {
            found_idx = i;
        }
    }

    if (found_idx < 0)
    {
        btree_node_done(node, using_cache);
        return -1;
    }

    const btree_entry_t *entry = &node->entries[found_idx];

    if (value && value_size)
    {
        if (entry->vlog_offset == 0 && node->values[found_idx])
        {
            *value = malloc(entry->value_size);
            if (*value)
            {
                memcpy(*value, node->values[found_idx], entry->value_size);
            }
            *value_size = entry->value_size;
        }
        else
        {
            *value = NULL;
            *value_size = entry->value_size;
        }
    }

    if (vlog_offset) *vlog_offset = entry->vlog_offset;
    if (seq) *seq = entry->seq;
    if (ttl) *ttl = entry->ttl;
    /* deleted returns the persisted tombstone/single-delete bits so compaction
     * can distinguish single-delete from regular delete. the low bit still
     * equals BTREE_ENTRY_FLAG_TOMBSTONE, so callers that treat *deleted as a
     * bool keep working unchanged. */
    if (deleted)
        *deleted = entry->flags & (BTREE_ENTRY_FLAG_TOMBSTONE | BTREE_ENTRY_FLAG_SINGLE_DELETE);

    btree_node_done(node, using_cache);
    return 0;
}

int btree_get(btree_t *tree, const uint8_t *key, const size_t key_size, uint8_t **value,
              size_t *value_size, uint64_t *vlog_offset, uint64_t *seq, int64_t *ttl,
              uint8_t *deleted)
{
    return btree_get_at_seq(tree, key, key_size, UINT64_MAX, value, value_size, vlog_offset, seq,
                            ttl, deleted);
}

uint64_t btree_get_entry_count(const btree_t *tree)
{
    return tree ? tree->entry_count : 0;
}

int btree_get_min_key(btree_t *tree, uint8_t **key, size_t *key_size)
{
    if (!tree || !key || !key_size) return -1;
    if (!tree->min_key) return -1;

    *key = malloc(tree->min_key_size);
    if (!*key) return -1;
    memcpy(*key, tree->min_key, tree->min_key_size);
    *key_size = tree->min_key_size;
    return 0;
}

int btree_get_max_key(btree_t *tree, uint8_t **key, size_t *key_size)
{
    if (!tree || !key || !key_size) return -1;
    if (!tree->max_key) return -1;

    *key = malloc(tree->max_key_size);
    if (!*key) return -1;
    memcpy(*key, tree->max_key, tree->max_key_size);
    *key_size = tree->max_key_size;
    return 0;
}

uint64_t btree_get_max_seq(const btree_t *tree)
{
    return tree ? tree->max_seq : 0;
}

int btree_get_stats(const btree_t *tree, btree_stats_t *stats)
{
    if (!tree || !stats) return -1;

    stats->entry_count = tree->entry_count;
    stats->node_count = tree->node_count;
    stats->height = tree->height;

    /* we get serialized size from block manager if available */
    stats->serialized_size = 0;
    if (tree->bm)
    {
        uint64_t size;
        if (block_manager_get_size(tree->bm, &size) == 0)
        {
            stats->serialized_size = size;
        }
    }

    return 0;
}

void btree_free(btree_t *tree)
{
    if (!tree) return;
    free(tree->min_key);
    free(tree->max_key);
    if (tree->node_arena)
    {
        btree_arena_destroy(tree->node_arena);
    }
    free(tree);
}

void btree_set_node_cache(btree_t *tree, clock_cache_t *cache)
{
    if (tree)
    {
        tree->node_cache = cache;
    }
}

/**
 * btree_create_node_cache
 * creates a node cache with the proper eviction callback
 * @param max_bytes maximum cache size in bytes
 * @return new cache or NULL on failure
 */
clock_cache_t *btree_create_node_cache(const size_t max_bytes)
{
    cache_config_t config = {0};
    config.avg_entry_size = BTREE_DEFAULT_NODE_SIZE;
    clock_cache_compute_config(max_bytes, &config);
    config.evict_callback = btree_node_cache_evict_callback;
    return clock_cache_create(&config);
}

/**
 * btree_print_node
 * recursively prints a node and its children for debugging
 * @param tree the btree instance
 * @param offset node offset in storage
 * @param depth current depth for indentation
 */
static void btree_print_node(btree_t *tree, const int64_t offset, const int depth)
{
    if (offset < 0) return;

    btree_node_t *node = NULL;
    if (btree_node_read_with_compression(tree->bm, offset, &node, tree->config.compression_algo) !=
        0)
    {
        printf("%*s[ERROR reading node at offset %" PRId64 "]\n", depth * 2, "", offset);
        return;
    }

    if (node->type == BTREE_NODE_INTERNAL)
    {
        printf("%*sINTERNAL (offset=%" PRId64 ", keys=%u, children=%u)\n", depth * 2, "", offset,
               node->num_entries, node->num_entries + 1);

        for (uint32_t i = 0; i < node->num_entries; i++)
        {
            printf("%*s  key[%u]: \"%.20s%s\" (size=%zu)\n", depth * 2, "", i,
                   (char *)node->keys[i], node->key_sizes[i] > 20 ? "..." : "", node->key_sizes[i]);
        }

        for (uint32_t i = 0; i <= node->num_entries; i++)
        {
            printf("%*s  child[%u] -> offset %" PRId64 "\n", depth * 2, "", i,
                   node->child_offsets[i]);
            btree_print_node(tree, node->child_offsets[i], depth + 1);
        }
    }
    else
    {
        printf("%*sLEAF (offset=%" PRId64 ", entries=%u, prev=%" PRId64 ", next=%" PRId64 ")\n",
               depth * 2, "", offset, node->num_entries, node->prev_offset, node->next_offset);

        for (uint32_t i = 0; i < node->num_entries && i < 5; i++)
        {
            printf("%*s  [%u] key=\"%.20s%s\" seq=%" PRIu64 "\n", depth * 2, "", i,
                   (char *)node->keys[i], node->key_sizes[i] > 20 ? "..." : "",
                   node->entries[i].seq);
        }
        if (node->num_entries > 5)
        {
            printf("%*s  ... (%u more entries)\n", depth * 2, "", node->num_entries - 5);
        }
    }

    btree_node_free(node);
}

void btree_print_tree(btree_t *tree)
{
    if (!tree)
    {
        printf("btree_print_tree: NULL tree\n");
        return;
    }

    printf("--- B+Tree Structure ---\n");
    printf("entry_count: %" PRIu64 "\n", tree->entry_count);
    printf("node_count: %" PRIu64 "\n", tree->node_count);
    printf("height: %u\n", tree->height);
    printf("root_offset: %" PRId64 "\n", tree->root_offset);
    printf("first_leaf_offset: %" PRId64 "\n", tree->first_leaf_offset);
    printf("last_leaf_offset: %" PRId64 "\n", tree->last_leaf_offset);

    if (tree->min_key)
    {
        printf("min_key: \"%.30s%s\"\n", (char *)tree->min_key,
               tree->min_key_size > 30 ? "..." : "");
    }
    if (tree->max_key)
    {
        printf("max_key: \"%.30s%s\"\n", (char *)tree->max_key,
               tree->max_key_size > 30 ? "..." : "");
    }

    printf("\nTree structure:\n");
    btree_print_node(tree, tree->root_offset, 0);
    printf("-----------------------\n");
}

int btree_cursor_init(btree_cursor_t **cursor, btree_t *tree)
{
    if (!cursor || !tree) return -1;

    btree_cursor_t *c = calloc(1, sizeof(btree_cursor_t));
    if (!c) return -1;

    c->tree = tree;
    c->current_node = NULL;
    c->current_index = -1;
    c->current_leaf_offset = -1;
    c->at_end = 0;
    c->at_begin = 0;
    c->using_cache = (tree->node_cache != NULL);

    *cursor = c;

    return btree_cursor_goto_first(c);
}

int btree_cursor_goto_first(btree_cursor_t *cursor)
{
    if (!cursor || !cursor->tree) return -1;

    if (cursor->current_node)
    {
        btree_node_done(cursor->current_node, cursor->using_cache);
        cursor->current_node = NULL;
    }

    if (cursor->tree->first_leaf_offset < 0)
    {
        cursor->at_end = 1;
        return -1;
    }

    cursor->current_leaf_offset = cursor->tree->first_leaf_offset;
    if (btree_node_read_cached(cursor->tree, cursor->current_leaf_offset, &cursor->current_node) !=
        0)
    {
        return -1;
    }

    cursor->current_index = 0;
    cursor->at_end = (cursor->current_node->num_entries == 0);
    cursor->at_begin = 0;
    return cursor->at_end ? -1 : 0;
}

int btree_cursor_goto_last(btree_cursor_t *cursor)
{
    if (!cursor || !cursor->tree) return -1;

    if (cursor->current_node)
    {
        btree_node_done(cursor->current_node, cursor->using_cache);
        cursor->current_node = NULL;
    }

    if (cursor->tree->last_leaf_offset < 0)
    {
        cursor->at_end = 1;
        return -1;
    }

    cursor->current_leaf_offset = cursor->tree->last_leaf_offset;
    if (btree_node_read_cached(cursor->tree, cursor->current_leaf_offset, &cursor->current_node) !=
        0)
    {
        return -1;
    }

    cursor->current_index = (int32_t)cursor->current_node->num_entries - 1;
    cursor->at_end = (cursor->current_index < 0);
    cursor->at_begin = 0;
    return cursor->at_end ? -1 : 0;
}

int btree_cursor_next(btree_cursor_t *cursor)
{
    if (!cursor || cursor->at_end) return -1;

    if (!cursor->current_node)
    {
        return btree_cursor_goto_first(cursor);
    }

    cursor->current_index++;

    if ((uint32_t)cursor->current_index >= cursor->current_node->num_entries)
    {
        const int64_t next_leaf_offset = cursor->current_node->next_offset;

        if (next_leaf_offset < 0)
        {
            cursor->at_end = 1;
            return -1;
        }

        btree_node_done(cursor->current_node, cursor->using_cache);
        cursor->current_node = NULL;

        cursor->current_leaf_offset = next_leaf_offset;
        if (btree_node_read_cached(cursor->tree, cursor->current_leaf_offset,
                                   &cursor->current_node) != 0)
        {
            cursor->at_end = 1;
            return -1;
        }

        cursor->current_index = 0;

        if (cursor->current_node->num_entries == 0)
        {
            cursor->at_end = 1;
            return -1;
        }
    }

    return 0;
}

int btree_cursor_prev(btree_cursor_t *cursor)
{
    if (!cursor) return -1;

    if (!cursor->current_node)
    {
        return btree_cursor_goto_last(cursor);
    }

    cursor->current_index--;

    if (cursor->current_index < 0)
    {
        const int64_t prev_leaf_offset = cursor->current_node->prev_offset;

        if (prev_leaf_offset < 0)
        {
            /* we reached beginning */
            cursor->current_index = -1;
            cursor->at_begin = 1;
            return -1;
        }

        btree_node_done(cursor->current_node, cursor->using_cache);
        cursor->current_node = NULL;

        cursor->current_leaf_offset = prev_leaf_offset;
        if (btree_node_read_cached(cursor->tree, cursor->current_leaf_offset,
                                   &cursor->current_node) != 0)
        {
            cursor->at_begin = 1;
            return -1;
        }

        cursor->current_index = (int32_t)cursor->current_node->num_entries - 1;

        if (cursor->current_index < 0)
        {
            cursor->at_begin = 1;
            return -1;
        }
    }

    return 0;
}

int btree_cursor_seek(btree_cursor_t *cursor, const uint8_t *key, const size_t key_size)
{
    if (!cursor || !cursor->tree || !key || key_size == 0) return -1;

    if (cursor->current_node)
    {
        btree_node_done(cursor->current_node, cursor->using_cache);
    }
    cursor->current_node = NULL;

    if (cursor->tree->root_offset < 0)
    {
        cursor->at_end = 1;
        return -1;
    }

    btree_node_t *node = NULL;
    if (btree_node_read_cached(cursor->tree, cursor->tree->root_offset, &node) != 0)
    {
        return -1;
    }

    while (node->type == BTREE_NODE_INTERNAL)
    {
        /* we utilize binary search for child index in internal node */
        uint32_t child_idx = 0;
        if (node->num_entries > 0)
        {
            int32_t lo = 0;
            int32_t hi = (int32_t)node->num_entries - 1;
            while (lo <= hi)
            {
                const int32_t mid = lo + (hi - lo) / 2;
                const int cmp = btree_compare_keys_inline(&cursor->tree->config, key, key_size,
                                                          node->keys[mid], node->key_sizes[mid]);
                if (cmp < 0)
                {
                    hi = mid - 1;
                }
                else
                {
                    lo = mid + 1;
                }
            }
            child_idx = (uint32_t)lo;
        }

        const int64_t child_offset = node->child_offsets[child_idx];
        btree_node_done(node, cursor->using_cache);

        if (btree_node_read_cached(cursor->tree, child_offset, &node) != 0)
        {
            return -1;
        }
    }

    int32_t left = 0;
    int32_t right = (int32_t)node->num_entries - 1;
    int32_t found_idx = -1;

    while (left <= right)
    {
        const int32_t mid = left + (right - left) / 2;
        const int cmp = btree_compare_keys_inline(&cursor->tree->config, key, key_size,
                                                  node->keys[mid], node->key_sizes[mid]);
        if (cmp == 0)
        {
            found_idx = mid;
            break;
        }
        if (cmp < 0)
        {
            right = mid - 1;
        }
        else
        {
            left = mid + 1;
        }
    }

    if (found_idx < 0)
    {
        found_idx = left;
    }

    if ((uint32_t)found_idx >= node->num_entries)
    {
        if (node->next_offset >= 0)
        {
            const int64_t next_off = node->next_offset;
            btree_node_done(node, cursor->using_cache);
            if (btree_node_read_cached(cursor->tree, next_off, &node) != 0)
            {
                cursor->at_end = 1;
                return -1;
            }
            found_idx = 0;
        }
        else
        {
            btree_node_done(node, cursor->using_cache);
            cursor->at_end = 1;
            return -1;
        }
    }

    cursor->current_node = node;
    cursor->current_index = found_idx;
    cursor->current_leaf_offset = node->block_offset;
    cursor->at_end = 0;
    cursor->at_begin = 0;
    return 0;
}

int btree_cursor_seek_for_prev(btree_cursor_t *cursor, const uint8_t *key, const size_t key_size)
{
    if (!cursor || !cursor->tree || !key || key_size == 0) return -1;

    if (btree_cursor_seek(cursor, key, key_size) != 0)
    {
        return btree_cursor_goto_last(cursor);
    }

    const int cmp = btree_compare_keys_inline(
        &cursor->tree->config, key, key_size, cursor->current_node->keys[cursor->current_index],
        cursor->current_node->key_sizes[cursor->current_index]);

    if (cmp < 0)
    {
        return btree_cursor_prev(cursor);
    }

    return 0;
}

int btree_cursor_valid(btree_cursor_t *cursor)
{
    if (!cursor) return -1;
    if (cursor->at_end) return 0;
    if (!cursor->current_node) return 0;
    if (cursor->current_index < 0) return 0;
    if ((uint32_t)cursor->current_index >= cursor->current_node->num_entries) return 0;
    return 1;
}

int btree_cursor_get(btree_cursor_t *cursor, uint8_t **key, size_t *key_size, uint8_t **value,
                     size_t *value_size, uint64_t *vlog_offset, uint64_t *seq, int64_t *ttl,
                     uint8_t *deleted)
{
    if (!cursor || !cursor->current_node) return -1;
    if (cursor->current_index < 0 ||
        (uint32_t)cursor->current_index >= cursor->current_node->num_entries)
    {
        return -1;
    }

    const uint32_t idx = (uint32_t)cursor->current_index;
    const btree_entry_t *entry = &cursor->current_node->entries[idx];

    if (key) *key = cursor->current_node->keys[idx];
    if (key_size) *key_size = cursor->current_node->key_sizes[idx];
    if (value) *value = cursor->current_node->values[idx];
    if (value_size) *value_size = entry->value_size;
    if (vlog_offset) *vlog_offset = entry->vlog_offset;
    if (seq) *seq = entry->seq;
    if (ttl) *ttl = entry->ttl;
    /* deleted returns the persisted tombstone/single-delete bits so compaction
     * can distinguish single-delete from regular delete. the low bit still
     * equals BTREE_ENTRY_FLAG_TOMBSTONE, so callers that treat *deleted as a
     * bool keep working unchanged. */
    if (deleted)
        *deleted = entry->flags & (BTREE_ENTRY_FLAG_TOMBSTONE | BTREE_ENTRY_FLAG_SINGLE_DELETE);

    return 0;
}

int btree_cursor_has_next(btree_cursor_t *cursor)
{
    if (!cursor) return -1;
    if (cursor->at_end) return 0;
    if (!cursor->current_node) return 1;

    if ((uint32_t)(cursor->current_index + 1) < cursor->current_node->num_entries)
    {
        return 1;
    }

    return (cursor->current_node->next_offset >= 0) ? 1 : 0;
}

int btree_cursor_has_prev(btree_cursor_t *cursor)
{
    if (!cursor) return -1;
    if (!cursor->current_node) return 0;

    if (cursor->current_index > 0)
    {
        return 1;
    }

    return (cursor->current_node->prev_offset >= 0) ? 1 : 0;
}

void btree_cursor_free(btree_cursor_t *cursor)
{
    if (!cursor) return;
    btree_node_done(cursor->current_node, cursor->using_cache);
    free(cursor);
}
