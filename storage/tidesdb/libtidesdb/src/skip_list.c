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

#include "skip_list.h"

/* thread-local cache for arena slot assignment
 * each thread caches its slot for one arena at a time
 * if the arena changes, we must get a new slot from that arena */
static _Thread_local skip_list_arena_t *tl_cached_arena = NULL;
static _Thread_local int tl_arena_slot = -1;

/**
 * skip_list_arena_create_block
 * creates a new arena block with the given capacity
 * @param capacity size in bytes for the block
 * @return pointer to block, or NULL on failure
 */
static skip_list_arena_block_t *skip_list_arena_create_block(const size_t capacity)
{
    skip_list_arena_block_t *block = malloc(sizeof(skip_list_arena_block_t));
    if (block == NULL) return NULL;

    block->data = malloc(capacity);
    if (block->data == NULL)
    {
        free(block);
        return NULL;
    }

    atomic_init(&block->used, 0);
    block->capacity = capacity;
    block->prev = NULL;

    return block;
}

/**
 * skip_list_arena_register_block
 * adds a block to the arena's all_blocks_head list for later destruction
 * @param arena the arena
 * @param block the block to register
 */
static void skip_list_arena_register_block(skip_list_arena_t *arena, skip_list_arena_block_t *block)
{
    skip_list_arena_block_t *head;
    do
    {
        head = atomic_load_explicit(&arena->all_blocks_head, memory_order_acquire);
        block->prev = head;
    } while (!atomic_compare_exchange_weak_explicit(&arena->all_blocks_head, &head, block,
                                                    memory_order_release, memory_order_acquire));
}

/**
 * skip_list_arena_create
 * creates a new arena with an initial block of the given capacity
 * @param initial_capacity size in bytes for the first block
 * @return pointer to arena, or NULL on failure
 */
static skip_list_arena_t *skip_list_arena_create(const size_t initial_capacity)
{
    skip_list_arena_t *arena = malloc(sizeof(skip_list_arena_t));
    if (arena == NULL) return NULL;

    skip_list_arena_block_t *block = skip_list_arena_create_block(initial_capacity);
    if (block == NULL)
    {
        free(arena);
        return NULL;
    }

    atomic_init(&arena->current_block, block);
    arena->block_size = initial_capacity;
    atomic_init(&arena->tl_slot_counter, 0);
    atomic_init(&arena->all_blocks_head, block);

    for (int i = 0; i < SKIP_LIST_ARENA_MAX_THREADS; i++)
    {
        atomic_init(&arena->tl_blocks[i], NULL);
    }

    return arena;
}

/**
 * skip_list_arena_get_slot
 * gets or assigns a thread-local slot for this thread and arena
 * the slot is cached per-thread but invalidated when switching arenas
 * @param arena the arena
 * @return slot index (0 to SKIP_LIST_ARENA_MAX_THREADS-1), or -1 if slots exhausted
 */
static inline int skip_list_arena_get_slot(skip_list_arena_t *arena)
{
    /* fast path -- cached slot for this arena */
    if (SKIP_LIST_LIKELY(tl_cached_arena == arena && tl_arena_slot >= 0))
    {
        return tl_arena_slot;
    }

    /* different arena or first allocation -- get a new slot */
    int slot = atomic_fetch_add_explicit(&arena->tl_slot_counter, 1, memory_order_relaxed);
    if (slot >= SKIP_LIST_ARENA_MAX_THREADS)
    {
        return -1;
    }

    tl_cached_arena = arena;
    tl_arena_slot = slot;
    return slot;
}

/**
 * skip_list_arena_alloc
 * thread-local bump allocation from the arena
 * each thread gets its own block -- no atomic contention on the fast path
 * only block allocation requires synchronization (rare)
 * @param arena the arena
 * @param size number of bytes to allocate
 * @return pointer to aligned memory, or NULL on failure
 */
static void *skip_list_arena_alloc(skip_list_arena_t *arena, size_t size)
{
    /* align up to SKIP_LIST_ARENA_ALIGNMENT */
    size = (size + (SKIP_LIST_ARENA_ALIGNMENT - 1)) & ~(size_t)(SKIP_LIST_ARENA_ALIGNMENT - 1);

    int slot = skip_list_arena_get_slot(arena);

    if (SKIP_LIST_LIKELY(slot >= 0))
    {
        /* fast path -- thread-local block with no atomic contention */
        skip_list_arena_block_t *block =
            atomic_load_explicit(&arena->tl_blocks[slot], memory_order_relaxed);

        if (SKIP_LIST_LIKELY(block != NULL))
        {
            /* a thread-local block is owned by exactly one thread (this slot) and its
             * `used` is never read by arena destroy, so relaxed is sufficient -- this
             * drops two seq_cst fences from the hottest allocation path */
            size_t used = atomic_load_explicit(&block->used, memory_order_relaxed);
            if (SKIP_LIST_LIKELY(used + size <= block->capacity))
            {
                atomic_store_explicit(&block->used, used + size, memory_order_relaxed);
                return block->data + used;
            }
        }

        /* thread-local block is NULL or full -- allocate a new one
         * use smaller blocks for thread-local slots to save memory on multi-threaded systems */
        size_t new_cap = SKIP_LIST_ARENA_TL_BLOCK_SIZE;
        if (size > new_cap) new_cap = size;

        skip_list_arena_block_t *new_block = skip_list_arena_create_block(new_cap);
        if (new_block == NULL) return NULL;

        atomic_store_explicit(&new_block->used, size, memory_order_relaxed);
        atomic_store_explicit(&arena->tl_blocks[slot], new_block, memory_order_relaxed);
        skip_list_arena_register_block(arena, new_block);

        return new_block->data;
    }

    /* fallback -- too many threads, use shared block with atomic contention */
    while (1)
    {
        skip_list_arena_block_t *block =
            atomic_load_explicit(&arena->current_block, memory_order_acquire);
        size_t offset = atomic_fetch_add_explicit(&block->used, size, memory_order_relaxed);

        if (SKIP_LIST_LIKELY(offset + size <= block->capacity))
        {
            return block->data + offset;
        }

        /* block full -- allocate a new shared block */
        size_t new_cap = arena->block_size;
        if (size > new_cap) new_cap = size;

        skip_list_arena_block_t *new_block = skip_list_arena_create_block(new_cap);
        if (new_block == NULL) return NULL;

        if (!atomic_compare_exchange_strong_explicit(&arena->current_block, &block, new_block,
                                                     memory_order_release, memory_order_acquire))
        {
            free(new_block->data);
            free(new_block);
        }
        else
        {
            skip_list_arena_register_block(arena, new_block);
        }
    }
}

/**
 * skip_list_arena_destroy
 * frees the arena and all its blocks
 * @param arena the arena to destroy
 */
static void skip_list_arena_destroy(skip_list_arena_t *arena)
{
    if (arena == NULL) return;

    /* free all blocks from the all_blocks_head list */
    skip_list_arena_block_t *block =
        atomic_load_explicit(&arena->all_blocks_head, memory_order_relaxed);
    while (block != NULL)
    {
        skip_list_arena_block_t *prev = block->prev;
        free(block->data);
        free(block);
        block = prev;
    }
    free(arena);
}

/**
 * skip_list_alloc
 * allocates memory from the arena if present, otherwise from malloc
 * @param list skip list (used to check for arena)
 * @param size number of bytes
 * @return pointer to memory, or NULL on failure
 */
static inline void *skip_list_alloc(const skip_list_t *list, size_t size)
{
    if (list != NULL && list->arena != NULL)
    {
        return skip_list_arena_alloc(list->arena, size);
    }
    return malloc(size);
}

/**
 * skip_list_dealloc
 * frees memory -- no-op when arena is active (bulk free on arena destroy)
 * @param list skip list (used to check for arena)
 * @param ptr pointer to free
 */
static inline void skip_list_dealloc(const skip_list_t *list, void *ptr)
{
    if (list != NULL && list->arena != NULL) return; /* no-op */
    free(ptr);
}

/**
 * skip_list_compare_keys_numeric_inline
 * fast inline comparison for 8-byte numeric keys
 * @param key1 first key
 * @param key2 second key
 * @return negative if key1 < key2, 0 if equal, positive if key1 > key2
 */
static inline int skip_list_compare_keys_numeric_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint64_t v1, v2;
    memcpy(&v1, key1, sizeof(uint64_t));
    memcpy(&v2, key2, sizeof(uint64_t));
    return (v1 < v2) ? -1 : (v1 > v2);
}

/* portable byte-swap for lexicographic integer comparison on little-endian.
 * memcmp compares bytes left-to-right (big-endian order), so we byte-swap
 * before integer comparison to match memcmp semantics on little-endian. */
#if defined(__GNUC__) || defined(__clang__)
#define SKIP_LIST_BSWAP32(x) __builtin_bswap32(x)
#define SKIP_LIST_BSWAP64(x) __builtin_bswap64(x)
#elif defined(_MSC_VER)
#define SKIP_LIST_BSWAP32(x) _byteswap_ulong(x)
#define SKIP_LIST_BSWAP64(x) _byteswap_uint64(x)
#else
static inline uint32_t SKIP_LIST_BSWAP32(uint32_t x)
{
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) |
           ((x << 24) & 0xFF000000);
}
static inline uint64_t SKIP_LIST_BSWAP64(uint64_t x)
{
    return ((uint64_t)SKIP_LIST_BSWAP32((uint32_t)x) << 32) |
           SKIP_LIST_BSWAP32((uint32_t)(x >> 32));
}
#endif

/* detect endianness at compile time */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SKIP_LIST_IS_BIG_ENDIAN 1
#else
#define SKIP_LIST_IS_BIG_ENDIAN 0
#endif

/* stack-allocated update array size for the batch/put paths; lists taller than this
 * fall back to a heap update array. file-scope so it is defined exactly once. */
#define SKIP_LIST_STACK_UPDATE_SIZE 64

/**
 * skip_list_compare_keys_4_inline
 * fast inline lexicographic comparison for 4-byte keys
 * uses byte-swapped integer comparison to avoid memcmp function call
 */
static inline int skip_list_compare_keys_4_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint32_t a, b;
    memcpy(&a, key1, 4);
    memcpy(&b, key2, 4);
#if !SKIP_LIST_IS_BIG_ENDIAN
    a = SKIP_LIST_BSWAP32(a);
    b = SKIP_LIST_BSWAP32(b);
#endif
    return (a < b) ? -1 : (a > b);
}

/**
 * skip_list_compare_keys_8_inline
 * fast inline lexicographic comparison for 8-byte keys
 * uses byte-swapped integer comparison to avoid memcmp function call
 */
static inline int skip_list_compare_keys_8_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint64_t a, b;
    memcpy(&a, key1, 8);
    memcpy(&b, key2, 8);
#if !SKIP_LIST_IS_BIG_ENDIAN
    a = SKIP_LIST_BSWAP64(a);
    b = SKIP_LIST_BSWAP64(b);
#endif
    return (a < b) ? -1 : (a > b);
}

/**
 * skip_list_compare_keys_16_inline
 * fast inline lexicographic comparison for 16-byte keys
 * compares first 8 bytes with early exit, avoiding second half when keys diverge early
 */
static inline int skip_list_compare_keys_16_inline(const uint8_t *key1, const uint8_t *key2)
{
    uint64_t a, b;
    memcpy(&a, key1, 8);
    memcpy(&b, key2, 8);
#if !SKIP_LIST_IS_BIG_ENDIAN
    a = SKIP_LIST_BSWAP64(a);
    b = SKIP_LIST_BSWAP64(b);
#endif
    if (a != b) return (a < b) ? -1 : 1;

    memcpy(&a, key1 + 8, 8);
    memcpy(&b, key2 + 8, 8);
#if !SKIP_LIST_IS_BIG_ENDIAN
    a = SKIP_LIST_BSWAP64(a);
    b = SKIP_LIST_BSWAP64(b);
#endif
    return (a < b) ? -1 : (a > b);
}

/**
 * skip_list_compare_keys_32_inline
 * fast inline lexicographic comparison for 32-byte keys
 * compares in 8-byte chunks with early exit
 */
static inline int skip_list_compare_keys_32_inline(const uint8_t *key1, const uint8_t *key2)
{
    for (int i = 0; i < 32; i += 8)
    {
        uint64_t a, b;
        memcpy(&a, key1 + i, 8);
        memcpy(&b, key2 + i, 8);
#if !SKIP_LIST_IS_BIG_ENDIAN
        a = SKIP_LIST_BSWAP64(a);
        b = SKIP_LIST_BSWAP64(b);
#endif
        if (a != b) return (a < b) ? -1 : 1;
    }
    return 0;
}

/**
 * skip_list_get_latest_valid_version
 * fast path for accessing the latest valid version
 * @param version version to check
 * @param current_time current time for TTL validation
 * @return latest valid version, or NULL if none
 */
static inline int skip_list_version_is_invalid_with_time(skip_list_version_t *version,
                                                         int64_t current_time);

static inline skip_list_version_t *skip_list_get_latest_valid_version(skip_list_node_t *node,
                                                                      const int64_t current_time)
{
    skip_list_version_t *version = atomic_load_explicit(&node->versions, memory_order_acquire);

    if (SKIP_LIST_UNLIKELY(version == NULL)) return NULL;
    skip_list_version_t *next = atomic_load_explicit(&version->next, memory_order_relaxed);
    if (SKIP_LIST_LIKELY(next == NULL))
    {
        if (!skip_list_version_is_invalid_with_time(version, current_time))
        {
            return version;
        }
        return NULL;
    }

    while (version != NULL)
    {
        if (!skip_list_version_is_invalid_with_time(version, current_time))
        {
            return version;
        }
        version = atomic_load_explicit(&version->next, memory_order_acquire);
    }

    return NULL;
}

/**
 * skip_list_free_version
 * frees a single version
 * @param list skip list (used to check for arena)
 * @param version version to free
 */
static void skip_list_free_version(const skip_list_t *list, skip_list_version_t *version);

/**
 * skip_list_compare_keys_with_type
 * hot-path comparator that accepts cmp_type as a register parameter
 * avoids reloading list->cmp_type from memory across function-call barriers (memcmp etc.)
 * callers in traversal loops should cache list->cmp_type in a local and use this variant
 */
static inline int skip_list_compare_keys_with_type(const skip_list_cmp_type_t cmp_type,
                                                   const skip_list_t *list, const uint8_t *key1,
                                                   const size_t key1_size, const uint8_t *key2,
                                                   const size_t key2_size)
{
    /* fast path for most common case -- memcmp with equal-sized keys */
    if (SKIP_LIST_LIKELY(cmp_type == SKIP_LIST_CMP_MEMCMP))
    {
        if (SKIP_LIST_LIKELY(key1_size == key2_size))
        {
            /* we use switch for common key sizes to avoid memcmp function call overhead.
             * 4/8 byte keys use byte-swapped integer comparison (no function call).
             * 16/32 byte keys use chunked comparison with early exit. */
            switch (key1_size)
            {
                case 4:
                    return skip_list_compare_keys_4_inline(key1, key2);
                case 8:
                    return skip_list_compare_keys_8_inline(key1, key2);
                case 16:
                    return skip_list_compare_keys_16_inline(key1, key2);
                case 32:
                    return skip_list_compare_keys_32_inline(key1, key2);
                default:
                {
                    const int cmp = memcmp(key1, key2, key1_size);
                    return (cmp == 0) ? 0 : ((cmp < 0) ? -1 : 1);
                }
            }
        }
        return skip_list_comparator_memcmp(key1, key1_size, key2, key2_size, NULL);
    }

    /* slow path for other comparator types */
    switch (cmp_type)
    {
        case SKIP_LIST_CMP_NUMERIC:
            return skip_list_compare_keys_numeric_inline(key1, key2);

        case SKIP_LIST_CMP_STRING:
            return skip_list_comparator_string(key1, key1_size, key2, key2_size, NULL);

        case SKIP_LIST_CMP_CUSTOM:
        default:
            return list->comparator(key1, key1_size, key2, key2_size, list->comparator_ctx);
    }
}

/**
 * skip_list_get_current_time
 * gets current time using cached time if available, otherwise syscall
 * @param list skip list (may be NULL)
 * @return current time as int64_t for consistent 64-bit handling
 */
static inline time_t skip_list_get_current_time(const skip_list_t *list)
{
#if defined(__MINGW32__) && !defined(__MINGW64__)
    /* on MinGW x86, cached time has visibility issues across threads, it seems to be a compiler bug
     ********
     */
    (void)list;
    return time(NULL);
#else
    if (list != NULL && list->cached_time != NULL)
    {
        return atomic_load_explicit(list->cached_time, memory_order_relaxed);
    }
    return time(NULL);
#endif
}

/**
 * skip_list_version_is_invalid_with_time
 * checks if version is expired or deleted using provided time
 * @param version version to check
 * @param current_time current time to use for TTL check
 * @return 1 if invalid, 0 if valid
 */
static inline int skip_list_version_is_invalid_with_time(skip_list_version_t *version,
                                                         const int64_t current_time)
{
    if (version == NULL) return 1;
    if (VERSION_IS_DELETED(version)) return 1;
    if (version->ttl > 0 && version->ttl < current_time) return 1;
    return 0;
}

/**
 * skip_list_validate_sequence
 * validates that new sequence number does not duplicate an existing version
 * @param existing_version existing version to check against
 * @param new_seq new sequence number
 * @return 0 if valid (new_seq != existing), -1 if duplicate
 */
static inline int skip_list_validate_sequence(skip_list_version_t *existing_version,
                                              uint64_t new_seq)
{
    if (existing_version != NULL)
    {
        uint64_t existing_seq = atomic_load_explicit(&existing_version->seq, memory_order_acquire);
        if (new_seq == existing_seq) return -1;
    }
    return 0;
}

/**
 * skip_list_insert_version_cas
 * inserts a new version into a version chain maintaining descending seq order
 * handles out-of-order arrivals from concurrent transaction commits by inserting
 * at the correct position in the chain rather than only at the head
 * @param versions_ptr pointer to atomic version list head
 * @param new_version version to insert
 * @param seq sequence number (for validation)
 * @param list skip list (for total_size update)
 * @param value_size size of new value
 * @return 0 on success, -1 on failure (duplicate seq)
 */
static int skip_list_insert_version_cas(_Atomic(skip_list_version_t *) *versions_ptr,
                                        skip_list_version_t *new_version, const uint64_t seq,
                                        skip_list_t *list, size_t value_size)
{
    skip_list_version_t *old_head;
    while (1)
    {
        old_head = atomic_load_explicit(versions_ptr, memory_order_acquire);

        if (old_head == NULL || seq > atomic_load_explicit(&old_head->seq, memory_order_acquire))
        {
            /* normal case -- new version is newest, prepend at head */
            atomic_store_explicit(&new_version->next, old_head, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(versions_ptr, &old_head, new_version,
                                                      memory_order_release, memory_order_acquire))
            {
                /* head prepend succeeded -- update total_size, subtract old head, add new */
                if (old_head && old_head->value_size > 0)
                {
                    atomic_fetch_sub_explicit(&list->total_size, old_head->value_size,
                                              memory_order_relaxed);
                }
                atomic_fetch_add_explicit(&list->total_size, value_size, memory_order_relaxed);
                return 0;
            }
            /* CAS failed, retry from top */
            continue;
        }

        uint64_t head_seq = atomic_load_explicit(&old_head->seq, memory_order_acquire);
        if (seq == head_seq)
        {
            /* duplicate sequence -- reject */
            skip_list_free_version(list, new_version);
            return -1;
        }

        /* out-of-order arrival -- walk chain to find correct insertion point
         * chain is descending by seq, so find first node where next->seq < seq
         * then insert between current and next.
         * for out-of-order inserts we cannot use head CAS, so we retry from the top
         * if the head changed. we insert by splicing into the chain. */
        skip_list_version_t *prev = old_head;
        skip_list_version_t *curr = atomic_load_explicit(&prev->next, memory_order_acquire);

        while (curr != NULL)
        {
            uint64_t curr_seq = atomic_load_explicit(&curr->seq, memory_order_acquire);
            if (seq == curr_seq)
            {
                /* duplicate in chain */
                skip_list_free_version(list, new_version);
                return -1;
            }
            if (seq > curr_seq)
            {
                break; /* insert between prev and curr */
            }
            prev = curr;
            curr = atomic_load_explicit(&prev->next, memory_order_acquire);
        }

        /* splice new_version between prev and curr */
        atomic_store_explicit(&new_version->next, curr, memory_order_relaxed);
        skip_list_version_t *expected_curr = curr;
        if (!atomic_compare_exchange_strong_explicit(&prev->next, &expected_curr, new_version,
                                                     memory_order_release, memory_order_acquire))
        {
            /* chain was modified concurrently, retry from top */
            continue;
        }

        /* successfully inserted in middle/tail -- we update total_size */
        atomic_fetch_add_explicit(&list->total_size, value_size, memory_order_relaxed);
        return 0;
    }
}

int skip_list_comparator_memcmp(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                size_t key2_size, void *ctx)
{
    (void)ctx;
    size_t min_size = key1_size < key2_size ? key1_size : key2_size;
    const int cmp = memcmp(key1, key2, min_size);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    return (key1_size < key2_size) ? -1 : (key1_size > key2_size) ? 1 : 0;
}

int skip_list_comparator_string(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
                                size_t key2_size, void *ctx)
{
    (void)ctx;
    /* length-bounded compare keys are byte buffers, not guaranteed NUL-terminated.
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

int skip_list_comparator_numeric(const uint8_t *key1, size_t key1_size, const uint8_t *key2,
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
 * skip_list_create_version
 * creates a new version for a key
 * @param list skip list (for arena allocation)
 * @param value value data
 * @param value_size size of value
 * @param ttl time-to-live
 * @param flags version flags (bitmask of SKIP_LIST_FLAG_*)
 * @param seq sequence number for MVCC
 * @return pointer to new version, NULL on failure
 */
static skip_list_version_t *skip_list_create_version(const skip_list_t *list, const uint8_t *value,
                                                     const size_t value_size, const int64_t ttl,
                                                     const uint8_t flags, uint64_t seq)
{
    /* we combine version struct + value data into a single allocation
     * this halves malloc calls and improves cache locality */
    const size_t alloc_size =
        sizeof(skip_list_version_t) + ((value != NULL && value_size > 0) ? value_size : 0);
    skip_list_version_t *version = (skip_list_version_t *)skip_list_alloc(list, alloc_size);
    if (version == NULL) return NULL;

    if (value != NULL && value_size > 0)
    {
        version->value = (uint8_t *)(version + 1); /* value follows struct in same allocation */
        memcpy(version->value, value, value_size);
        version->value_size = value_size;
    }
    else
    {
        version->value = NULL;
        version->value_size = 0;
    }

    atomic_init(&version->flags, flags);
    atomic_init(&version->seq, seq);
    version->ttl = ttl;
    atomic_init(&version->next, NULL);
    return version;
}

/**
 * skip_list_free_version
 * frees a single version
 * @param list skip list (for arena deallocation)
 * @param version version to free
 */
static void skip_list_free_version(const skip_list_t *list, skip_list_version_t *version)
{
    if (version == NULL) return;
    /* value is embedded in same allocation as version struct -- single free */
    skip_list_dealloc(list, version);
}

/**
 * skip_list_free_version_list
 * frees a linked list of versions
 * @param list skip list (for arena deallocation)
 * @param head head of version list
 */
static void skip_list_free_version_list(const skip_list_t *list, skip_list_version_t *head)
{
    while (head != NULL)
    {
        skip_list_version_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
        skip_list_free_version(list, head);
        head = next;
    }
}

/**
 * skip_list_create_sentinel
 * creates a sentinel node (header or tail)
 * @param level level of the node
 * @return pointer to new sentinel node, NULL on failure
 */
static skip_list_node_t *skip_list_create_sentinel(const int level)
{
    size_t pointers_size = (level + 1) * 2 * sizeof(_Atomic(skip_list_node_t *));
    skip_list_node_t *node = (skip_list_node_t *)malloc(sizeof(skip_list_node_t) + pointers_size);
    if (node == NULL) return NULL;

    node->key = NULL;
    node->key_size = 0;
    node->level = (uint8_t)level;
    node->node_flags = SKIP_LIST_NODE_FLAG_SENTINEL;
    atomic_init(&node->versions, NULL);

    for (int i = 0; i <= level; i++)
    {
        atomic_init(&node->forward[i], NULL);
        atomic_init(&BACKWARD_PTR(node, i, level), NULL);
    }

    return node;
}

skip_list_node_t *skip_list_create_node(const int level, const uint8_t *key, size_t key_size,
                                        const uint8_t *value, const size_t value_size,
                                        const int64_t ttl, const uint8_t flags)
{
    if (key == NULL || key_size == 0) return NULL;

    /* we combine node struct + forward/backward pointers + key into a single allocation
     * this eliminates one malloc per node and co-locates key data for cache locality */
    size_t pointers_size = (level + 1) * 2 * sizeof(_Atomic(skip_list_node_t *));
    skip_list_node_t *node =
        (skip_list_node_t *)malloc(sizeof(skip_list_node_t) + pointers_size + key_size);
    if (node == NULL) return NULL;

    node->key = (uint8_t *)node + sizeof(skip_list_node_t) + pointers_size;
    memcpy(node->key, key, key_size);
    node->key_size = key_size;
    node->level = (uint8_t)level;
    node->node_flags = 0; /* not a sentinel */

    const int is_tombstone = (flags & SKIP_LIST_FLAG_DELETED) != 0;
    skip_list_version_t *initial_version = NULL;
    if (value != NULL || is_tombstone)
    {
        initial_version = skip_list_create_version(NULL, value, value_size, ttl, flags, 0);
        if (initial_version == NULL)
        {
            /* for non-tombstones, version creation failure is fatal
             * for tombstones, NULL version is acceptable */
            if (!is_tombstone)
            {
                free(node);
                return NULL;
            }
        }
    }
    atomic_init(&node->versions, initial_version);

    for (int i = 0; i <= level; i++)
    {
        atomic_init(&node->forward[i], NULL);
        atomic_init(&BACKWARD_PTR(node, i, level), NULL);
    }

    return node;
}

/**
 * skip_list_free_node_internal
 * arena-aware node free -- simply no-op when arena is active
 */
static int skip_list_free_node_internal(const skip_list_t *list, skip_list_node_t *node)
{
    if (node == NULL) return -1;
    skip_list_version_t *versions = atomic_load_explicit(&node->versions, memory_order_acquire);
    skip_list_free_version_list(list, versions);
    /* key is embedded in same allocation as node -- single free */
    skip_list_dealloc(list, node);
    return 0;
}

int skip_list_free_node(skip_list_node_t *node)
{
    if (node == NULL) return -1;
    skip_list_version_t *versions = atomic_load_explicit(&node->versions, memory_order_acquire);

    while (versions != NULL)
    {
        skip_list_version_t *next = atomic_load_explicit(&versions->next, memory_order_acquire);
        free(versions);
        versions = next;
    }
    free(node);
    return 0;
}

int skip_list_new(skip_list_t **list, const int max_level, const float probability)
{
    return skip_list_new_with_comparator(list, max_level, probability, skip_list_comparator_memcmp,
                                         NULL);
}

int skip_list_new_with_comparator(skip_list_t **list, int max_level, float probability,
                                  skip_list_comparator_fn comparator, void *comparator_ctx)
{
    return skip_list_new_with_comparator_and_cached_time(list, max_level, probability, comparator,
                                                         comparator_ctx, NULL);
}

int skip_list_new_with_comparator_and_cached_time(skip_list_t **list, const int max_level,
                                                  const float probability,
                                                  skip_list_comparator_fn comparator,
                                                  void *comparator_ctx,
                                                  _Atomic(time_t) *cached_time)
{
    if (list == NULL || max_level <= 0 || probability <= 0.0f || probability >= 1.0f) return -1;

    skip_list_t *new_list = (skip_list_t *)malloc(sizeof(skip_list_t));
    if (new_list == NULL) return -1;

    atomic_init(&new_list->level, 0);
    new_list->max_level = max_level;
    new_list->probability = probability;

    /* we determine comparator typen */
    if (comparator == skip_list_comparator_memcmp)
    {
        new_list->cmp_type = SKIP_LIST_CMP_MEMCMP;
    }
    else if (comparator == skip_list_comparator_string)
    {
        new_list->cmp_type = SKIP_LIST_CMP_STRING;
    }
    else if (comparator == skip_list_comparator_numeric)
    {
        new_list->cmp_type = SKIP_LIST_CMP_NUMERIC;
    }
    else
    {
        new_list->cmp_type = SKIP_LIST_CMP_CUSTOM;
    }

    new_list->comparator = comparator;
    new_list->comparator_ctx = comparator_ctx;
    new_list->cached_time = cached_time;
    new_list->arena = NULL;

    if (cached_time != NULL)
    {
        atomic_store_explicit(cached_time, tdb_get_current_time(), memory_order_seq_cst);
    }

    atomic_init(&new_list->total_size, 0);
    atomic_init(&new_list->entry_count, 0);
    atomic_init(&new_list->min_seq, UINT64_MAX);

    /* we create sentinel nodes with no keys -- they are identified by the sentinel flag */
    skip_list_node_t *header = skip_list_create_sentinel(max_level);
    skip_list_node_t *tail = skip_list_create_sentinel(max_level);

    if (header == NULL || tail == NULL)
    {
        if (header) skip_list_free_node(header);
        if (tail) skip_list_free_node(tail);
        free(new_list);
        return -1;
    }

    for (int i = 0; i <= max_level; i++)
    {
        atomic_store_explicit(&header->forward[i], tail, memory_order_relaxed);
        atomic_store_explicit(&BACKWARD_PTR(tail, i, max_level), header, memory_order_relaxed);
    }

    atomic_init(&new_list->header, header);
    atomic_init(&new_list->tail, tail);

    *list = new_list;
    return 0;
}

int skip_list_new_with_arena(skip_list_t **list, const int max_level, const float probability,
                             skip_list_comparator_fn comparator, void *comparator_ctx,
                             _Atomic(time_t) *cached_time, const size_t arena_initial_capacity)
{
    if (arena_initial_capacity == 0)
    {
        return skip_list_new_with_comparator_and_cached_time(
            list, max_level, probability, comparator, comparator_ctx, cached_time);
    }

    int rc = skip_list_new_with_comparator_and_cached_time(list, max_level, probability, comparator,
                                                           comparator_ctx, cached_time);
    if (rc != 0) return rc;

    (*list)->arena = skip_list_arena_create(arena_initial_capacity);
    if ((*list)->arena == NULL)
    {
        skip_list_free(*list);
        *list = NULL;
        return -1;
    }

    return 0;
}

/**
 * skip_list_xorshift64star
 * fast thread-local RNG for skip list level selection using xorshift64* algorithm
 * @param state pointer to thread-local RNG state
 * @return pseudo-random 64-bit value
 */
static inline uint64_t skip_list_xorshift64star(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

int skip_list_random_level(const skip_list_t *list)
{
    if (list == NULL) return -1;

    /* thread-local RNG state */
    static _Thread_local uint64_t rng_state = 0;
    if (SKIP_LIST_UNLIKELY(rng_state == 0))
    {
        /** we init with thread ID + address entropy for uniqueness
         * avoids time() syscall on hot path */
        rng_state = (uint64_t)TDB_THREAD_ID() ^ ((uintptr_t)&rng_state >> 3);
        if (rng_state == 0) rng_state = 1; /* ensure non-zero */
    }

    /* geometric level distribution for the configured probability where we promote a level
     * while a fresh uniform draw stays below p. averages ~1/(1-p) draws (~1.33 at
     * p=0.25), each a cheap xorshift + compare. */
    const double p = (double)list->probability;
    int level = 0;
    while (level < list->max_level)
    {
        const uint64_t rnd = skip_list_xorshift64star(&rng_state);
        /* top 53 bits -> uniform double in [0, 1) */
        const double u = (double)(rnd >> 11) * (1.0 / 9007199254740992.0);
        if (u >= p) break;
        level++;
    }

    return level;
}

int skip_list_compare_keys(const skip_list_t *list, const uint8_t *key1, size_t key1_size,
                           const uint8_t *key2, size_t key2_size)
{
    if (list == NULL || key1 == NULL || key2 == NULL) return 0;
    return list->comparator(key1, key1_size, key2, key2_size, list->comparator_ctx);
}

int skip_list_check_and_update_ttl(const skip_list_t *list, skip_list_node_t *node)
{
    if (node == NULL) return -1;
    skip_list_version_t *version = atomic_load_explicit(&node->versions, memory_order_acquire);
    if (version != NULL && version->ttl > 0 && version->ttl <= skip_list_get_current_time(list))
    {
        return 1;
    }
    return 0;
}

int skip_list_get(skip_list_t *list, const uint8_t *key, const size_t key_size, uint8_t **value,
                  size_t *value_size, int64_t *ttl, uint8_t *deleted)
{
    if (list == NULL || key == NULL || key_size == 0 || value == NULL || value_size == NULL)
        return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level =
        atomic_load_explicit(&list->level, memory_order_acquire); /* cache level */
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    /* we track if we found exact match at level 0 to avoid redundant comparison */
    int found_exact = 0;
    skip_list_node_t *candidate = NULL;

    /* we search from top level down with prefetching
     * use relaxed loads during traversal, acquire only at level 0 for final target
     * prefetch fires before sentinel check so cache line is warming during condition eval */
    /* on x86 (TSO), relaxed and acquire loads compile identically.
     * we use acquire uniformly to avoid a per-iteration branch */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

        /* we prefetch before touching any fields -- this gives memory subsystem head start */
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        /* non-sentinel nodes always have key != NULL, so sentinel check is sufficient */
        while (SKIP_LIST_LIKELY(next != NULL && !NODE_IS_SENTINEL(next)))
        {
            const int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key,
                                                             next->key_size, key, key_size);
            if (cmp > 0) break;
            if (cmp == 0)
            {
                /* exact match found -- at level 0 we can skip final comparison */
                if (i == 0)
                {
                    found_exact = 1;
                    candidate = next;
                }
                break;
            }
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

            /* prefetch immediately after loading pointer, before next iteration's sentinel check */
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target;
    if (found_exact)
    {
        target = candidate;
    }
    else
    {
        target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
        if (SKIP_LIST_UNLIKELY(target == NULL || NODE_IS_SENTINEL(target) || target->key == NULL))
            return -1;

        const int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key,
                                                         target->key_size, key, key_size);
        if (SKIP_LIST_UNLIKELY(cmp != 0)) return -1;
    }

    skip_list_version_t *head_version =
        atomic_load_explicit(&target->versions, memory_order_acquire);
    if (head_version == NULL) return -1;

    const int64_t current_time = skip_list_get_current_time(list);
    int head_invalid = skip_list_version_is_invalid_with_time(head_version, current_time);

    if (head_invalid && VERSION_IS_DELETED(head_version))
    {
        if (ttl != NULL) *ttl = head_version->ttl;
        if (deleted != NULL) *deleted = 1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    skip_list_version_t *version =
        head_invalid ? skip_list_get_latest_valid_version(target, current_time) : head_version;

    if (version == NULL)
    {
        if (deleted != NULL) *deleted = 1;
        if (ttl != NULL) *ttl = -1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    if (ttl != NULL) *ttl = version->ttl;
    if (deleted != NULL) *deleted = 0;

    if (version->value_size > 0 && version->value != NULL)
    {
        *value = (uint8_t *)malloc(version->value_size);
        if (*value == NULL) return -1;
        memcpy(*value, version->value, version->value_size);
        *value_size = version->value_size;
    }
    else
    {
        *value = NULL;
        *value_size = 0;
    }
    return 0;
}

int skip_list_get_ref(skip_list_t *list, const uint8_t *key, const size_t key_size,
                      const uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted)
{
    if (list == NULL || key == NULL || key_size == 0 || value == NULL || value_size == NULL)
        return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    int found_exact = 0;
    skip_list_node_t *candidate = NULL;

    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (SKIP_LIST_LIKELY(next != NULL && !NODE_IS_SENTINEL(next)))
        {
            const int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key,
                                                             next->key_size, key, key_size);
            if (cmp > 0) break;
            if (cmp == 0)
            {
                if (i == 0)
                {
                    found_exact = 1;
                    candidate = next;
                }
                break;
            }
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target;
    if (found_exact)
    {
        target = candidate;
    }
    else
    {
        target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
        if (SKIP_LIST_UNLIKELY(target == NULL || NODE_IS_SENTINEL(target) || target->key == NULL))
            return -1;

        const int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key,
                                                         target->key_size, key, key_size);
        if (SKIP_LIST_UNLIKELY(cmp != 0)) return -1;
    }

    skip_list_version_t *head_version =
        atomic_load_explicit(&target->versions, memory_order_acquire);
    if (head_version == NULL) return -1;

    const int64_t current_time = skip_list_get_current_time(list);
    int head_invalid = skip_list_version_is_invalid_with_time(head_version, current_time);

    if (head_invalid && VERSION_IS_DELETED(head_version))
    {
        if (ttl != NULL) *ttl = head_version->ttl;
        if (deleted != NULL) *deleted = 1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    skip_list_version_t *version =
        head_invalid ? skip_list_get_latest_valid_version(target, current_time) : head_version;

    if (version == NULL)
    {
        if (deleted != NULL) *deleted = 1;
        if (ttl != NULL) *ttl = -1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    if (ttl != NULL) *ttl = version->ttl;
    if (deleted != NULL) *deleted = 0;

    /* zero-copy -- we simply return direct pointer into version data */
    *value = version->value;
    *value_size = version->value_size;
    return 0;
}

int skip_list_delete(skip_list_t *list, const uint8_t *key, const size_t key_size,
                     const uint64_t seq)
{
    if (list == NULL || key == NULL || key_size == 0) return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    /* we traverse with prefetching -- prefetch before sentinel check */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key, next->key_size,
                                                       key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    if (target == NULL || NODE_IS_SENTINEL(target)) return 0;

    int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key, target->key_size, key,
                                               key_size);
    if (cmp != 0) return 0;

    skip_list_version_t *tombstone = skip_list_create_version(list, NULL, 0, -1, 1, seq);
    if (tombstone == NULL) return -1;

    if (skip_list_insert_version_cas(&target->versions, tombstone, seq, list, 0) != 0)
    {
        return -1;
    }
    return 0;
}

int skip_list_clear(skip_list_t *list)
{
    if (list == NULL) return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *tail = atomic_load_explicit(&list->tail, memory_order_acquire);

    if (list->arena == NULL)
    {
        /* no arena -- we must walk and free each node individually */
        skip_list_node_t *current = atomic_load_explicit(&header->forward[0], memory_order_acquire);
        while (current != NULL && !NODE_IS_SENTINEL(current))
        {
            skip_list_node_t *next =
                atomic_load_explicit(&current->forward[0], memory_order_acquire);
            skip_list_free_node(current);
            current = next;
        }
    }
    /* with arena, nodes are freed in bulk when arena is destroyed */

    const int max_level = list->max_level;
    for (int i = 0; i <= max_level; i++)
    {
        atomic_store_explicit(&header->forward[i], tail, memory_order_release);
        atomic_store_explicit(&BACKWARD_PTR(tail, i, max_level), header, memory_order_release);
    }

    atomic_store_explicit(&list->level, 0, memory_order_release);
    atomic_store_explicit(&list->total_size, 0, memory_order_release);
    atomic_store_explicit(&list->entry_count, 0, memory_order_release);

    return 0;
}

void skip_list_free(skip_list_t *list)
{
    if (list == NULL) return;

    if (list->arena != NULL)
    {
        /* arena path -- we simply destroy arena (frees all nodes+versions in bulk),
         * then free sentinels which were malloc'd before arena existed */
        skip_list_arena_destroy(list->arena);
        list->arena = NULL;

        skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
        skip_list_node_t *tail = atomic_load_explicit(&list->tail, memory_order_acquire);
        skip_list_free_node(header);
        skip_list_free_node(tail);
    }
    else
    {
        /* no arena -- we walk and free each node individually */
        skip_list_clear(list);

        skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
        skip_list_node_t *tail = atomic_load_explicit(&list->tail, memory_order_acquire);
        skip_list_free_node(header);
        skip_list_free_node(tail);
    }

    free(list);
}

size_t skip_list_get_size(skip_list_t *list)
{
    if (list == NULL) return 0;
    return atomic_load_explicit(&list->total_size, memory_order_acquire);
}

int skip_list_count_entries(skip_list_t *list)
{
    if (list == NULL) return -1;
    return atomic_load_explicit(&list->entry_count, memory_order_acquire);
}

int skip_list_get_min_key(skip_list_t *list, uint8_t **key, size_t *key_size)
{
    if (list == NULL || key == NULL || key_size == NULL) return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *first = atomic_load_explicit(&header->forward[0], memory_order_acquire);

    if (first == NULL || NODE_IS_SENTINEL(first)) return -1;

    /* we find first valid (non-deleted, non-expired) entry */
    const int64_t current_time = skip_list_get_current_time(list);
    skip_list_node_t *current = first;
    while (current != NULL && !NODE_IS_SENTINEL(current))
    {
        skip_list_version_t *version =
            atomic_load_explicit(&current->versions, memory_order_acquire);
        if (!skip_list_version_is_invalid_with_time(version, current_time))
        {
            first = current;
            break;
        }
        current = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    }

    if (current == NULL || NODE_IS_SENTINEL(current)) return -1;

    *key = (uint8_t *)malloc(first->key_size);
    if (*key == NULL) return -1;
    memcpy(*key, first->key, first->key_size);
    *key_size = first->key_size;
    return 0;
}

static skip_list_node_t *skip_list_predecessor(const skip_list_t *list, skip_list_node_t *header,
                                               const uint8_t *key, size_t key_size);

int skip_list_get_max_key(skip_list_t *list, uint8_t **key, size_t *key_size)
{
    if (list == NULL || key == NULL || key_size == NULL) return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);

    /* forward-reseek the last node, then step back via forward search (not the
     * stale-prone backward pointers) until a valid (non-deleted, non-expired)
     * entry or the header */
    const int64_t current_time = skip_list_get_current_time(list);
    skip_list_node_t *current = skip_list_predecessor(list, header, NULL, 0);
    while (current != header && !NODE_IS_SENTINEL(current))
    {
        skip_list_version_t *version =
            atomic_load_explicit(&current->versions, memory_order_acquire);
        if (!skip_list_version_is_invalid_with_time(version, current_time))
        {
            *key = (uint8_t *)malloc(current->key_size);
            if (*key == NULL) return -1;
            memcpy(*key, current->key, current->key_size);
            *key_size = current->key_size;
            return 0;
        }
        current = skip_list_predecessor(list, header, current->key, current->key_size);
    }

    return -1;
}

int skip_list_cursor_init(skip_list_cursor_t **cursor, skip_list_t *list)
{
    if (cursor == NULL || list == NULL) return -1;

    *cursor = (skip_list_cursor_t *)malloc(sizeof(skip_list_cursor_t));
    if (*cursor == NULL) return -1;

    (*cursor)->list = list;
    (*cursor)->cached_header = atomic_load_explicit(&list->header, memory_order_acquire);
    (*cursor)->cached_tail = atomic_load_explicit(&list->tail, memory_order_acquire);
    (*cursor)->current =
        atomic_load_explicit(&(*cursor)->cached_header->forward[0], memory_order_acquire);
    (*cursor)->current_version = NULL;
    return 0;
}

void skip_list_cursor_free(skip_list_cursor_t *cursor)
{
    if (cursor != NULL) free(cursor);
}

int skip_list_cursor_valid(const skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    return (cursor->current != cursor->cached_header && cursor->current != cursor->cached_tail) ? 1
                                                                                                : 0;
}

int skip_list_cursor_next(skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_tail) return -1;

    cursor->current = atomic_load_explicit(&cursor->current->forward[0], memory_order_acquire);
    cursor->current_version = NULL;
    if (cursor->current == NULL || cursor->current == cursor->cached_tail) return -1;

    /* we prefetch next node, its key, and its version to hide memory latency.
     * acquire (not relaxed) -- next is dereferenced below (NODE_IS_SENTINEL,
     * ->key) so it must synchronize with the release-CAS that published it */
    skip_list_node_t *next =
        atomic_load_explicit(&cursor->current->forward[0], memory_order_acquire);
    if (next && !NODE_IS_SENTINEL(next))
    {
        PREFETCH_READ(next);
        PREFETCH_READ(next->key);
    }
    /* we prefetch version for the current node -- cursor_get will need it */
    PREFETCH_READ(&cursor->current->versions);

    return 0;
}

/**
 * skip_list_predecessor
 * forward-searches for the last node whose key is strictly less than `key`, or for
 * the last node in the list when key == NULL. used for reverse navigation unlike
 * the per-node backward pointers (which are maintained best-effort and can be left
 * stale by concurrent inserts, so a backward walk may skip nodes), forward[0] is the
 * linearizable structure, so this is always complete.
 * @return the predecessor node, or the header sentinel when none exists
 */
static skip_list_node_t *skip_list_predecessor(const skip_list_t *list, skip_list_node_t *header,
                                               const uint8_t *key, const size_t key_size)
{
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;
    skip_list_node_t *pred = header;
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&pred->forward[i], memory_order_acquire);
        while (next != NULL && !NODE_IS_SENTINEL(next) &&
               (key == NULL || skip_list_compare_keys_with_type(cmp_type, list, next->key,
                                                                next->key_size, key, key_size) < 0))
        {
            pred = next;
            next = atomic_load_explicit(&pred->forward[i], memory_order_acquire);
        }
    }
    return pred;
}

int skip_list_cursor_prev(skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_header) return -1;

    skip_list_node_t *cur = cursor->current;

    /* the backward pointer is a HINT, trusted only when the forward
     * list confirms it -- H is cur's true predecessor iff H->forward[0] == cur, and
     * forward[0] is the linearizable source of truth. this keeps reverse steps O(1)
     * when the hint is fresh (the common case) while a stale/NULL backward pointer,
     * which a concurrent insert can leave behind, falls through to the reseek. */
    skip_list_node_t *hint =
        atomic_load_explicit(&BACKWARD_PTR(cur, 0, cur->level), memory_order_acquire);
    if (hint != NULL && atomic_load_explicit(&hint->forward[0], memory_order_acquire) == cur)
    {
        cursor->current = hint;
        cursor->current_version = NULL;
        if (hint == cursor->cached_header) return -1;
        PREFETCH_READ(&hint->versions);
        return 0;
    }

    /* slow path -- forward-reseek the predecessor (always complete). when cur is the
     * tail, the predecessor of "+infinity" is the last node (key == NULL). */
    skip_list_node_t *pred =
        (cur == cursor->cached_tail)
            ? skip_list_predecessor(cursor->list, cursor->cached_header, NULL, 0)
            : skip_list_predecessor(cursor->list, cursor->cached_header, cur->key, cur->key_size);

    cursor->current = pred;
    cursor->current_version = NULL;
    if (pred == cursor->cached_header) return -1;

    PREFETCH_READ(&pred->versions);
    return 0;
}

int skip_list_cursor_advance_in_node(skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_header || cursor->current == cursor->cached_tail)
        return -1;

    /* if no version was selected yet, the next-older sits behind the head; otherwise
     * walk the chain pointer from the version we are currently parked on */
    skip_list_version_t *cur =
        cursor->current_version
            ? cursor->current_version
            : atomic_load_explicit(&cursor->current->versions, memory_order_acquire);
    if (cur == NULL) return -1;

    skip_list_version_t *next_older = atomic_load_explicit(&cur->next, memory_order_acquire);
    if (next_older == NULL) return -1;

    cursor->current_version = next_older;
    return 0;
}

int skip_list_cursor_get(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                         uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted)
{
    if (cursor == NULL || cursor->current == NULL) return -1;

    if (cursor->current == cursor->cached_tail) return -1;

    *key = cursor->current->key;
    *key_size = cursor->current->key_size;

    skip_list_version_t *version =
        cursor->current_version
            ? cursor->current_version
            : atomic_load_explicit(&cursor->current->versions, memory_order_acquire);
    if (version == NULL) return -1;

    if (ttl != NULL) *ttl = version->ttl;

    /* we check if version is invalid (expired or deleted) */
    if (skip_list_version_is_invalid_with_time(version, skip_list_get_current_time(cursor->list)))
    {
        if (deleted != NULL) *deleted = 1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    if (deleted != NULL) *deleted = 0;
    *value = version->value;
    *value_size = version->value_size;
    return 0;
}

int skip_list_cursor_get_with_seq(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                                  uint8_t **value, size_t *value_size, int64_t *ttl,
                                  uint8_t *deleted, uint64_t *seq)
{
    if (cursor == NULL || cursor->current == NULL) return -1;

    if (cursor->current == cursor->cached_tail) return -1;

    *key = cursor->current->key;
    *key_size = cursor->current->key_size;

    skip_list_version_t *version =
        cursor->current_version
            ? cursor->current_version
            : atomic_load_explicit(&cursor->current->versions, memory_order_acquire);
    if (version == NULL) return -1;

    if (ttl != NULL) *ttl = version->ttl;
    if (seq != NULL) *seq = atomic_load_explicit(&version->seq, memory_order_acquire);

    /* *deleted returns the version flag bits (SKIP_LIST_FLAG_*) so callers can
     * see single-delete and not just plain tombstone.  the low bit is always set
     * when the caller should treat this entry as a tombstone (tombstone or
     * expired ttl), matching the old bool-like contract for existing callers. */
    const uint8_t version_flags = atomic_load_explicit(&version->flags, memory_order_acquire);

    /* we check if version is invalid (expired or deleted) */
    if (skip_list_version_is_invalid_with_time(version, skip_list_get_current_time(cursor->list)))
    {
        if (deleted != NULL)
        {
            *deleted = SKIP_LIST_FLAG_DELETED | (version_flags & SKIP_LIST_FLAG_SINGLE_DELETE);
        }
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    if (deleted != NULL) *deleted = 0;
    *value = version->value;
    *value_size = version->value_size;
    return 0;
}

int skip_list_cursor_next_get(skip_list_cursor_t *cursor, uint8_t **key, size_t *key_size,
                              uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_tail) return -1;

    /* we advance to next node */
    cursor->current = atomic_load_explicit(&cursor->current->forward[0], memory_order_acquire);
    cursor->current_version = NULL;
    if (cursor->current == NULL || cursor->current == cursor->cached_tail) return -1;

    /* we prefetch next node for the next call to this function.
     * acquire (not relaxed) -- next is dereferenced below so it must
     * synchronize with the release-CAS that published it */
    skip_list_node_t *next =
        atomic_load_explicit(&cursor->current->forward[0], memory_order_acquire);
    if (next && !NODE_IS_SENTINEL(next))
    {
        PREFETCH_READ(next);
        PREFETCH_READ(next->key);
        PREFETCH_READ(&next->versions);
    }

    /* inline get -- no redundant sentinel/NULL checks */
    *key = cursor->current->key;
    *key_size = cursor->current->key_size;

    skip_list_version_t *version =
        atomic_load_explicit(&cursor->current->versions, memory_order_acquire);
    if (version == NULL) return -1;

    if (ttl != NULL) *ttl = version->ttl;

    if (skip_list_version_is_invalid_with_time(version, skip_list_get_current_time(cursor->list)))
    {
        if (deleted != NULL) *deleted = 1;
        *value = NULL;
        *value_size = 0;
        return 0;
    }

    if (deleted != NULL) *deleted = 0;
    *value = version->value;
    *value_size = version->value_size;
    return 0;
}

int skip_list_cursor_at_start(skip_list_cursor_t *cursor)
{
    if (cursor == NULL) return -1;
    skip_list_node_t *first =
        atomic_load_explicit(&cursor->cached_header->forward[0], memory_order_acquire);
    return (cursor->current == first) ? 1 : 0;
}

int skip_list_cursor_at_end(const skip_list_cursor_t *cursor)
{
    if (cursor == NULL) return -1;
    return (cursor->current == cursor->cached_tail) ? 1 : 0;
}

int skip_list_cursor_has_next(skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_tail) return -1;
    skip_list_node_t *next =
        atomic_load_explicit(&cursor->current->forward[0], memory_order_acquire);
    return (next != NULL && next != cursor->cached_tail) ? 1 : 0;
}

int skip_list_cursor_has_prev(skip_list_cursor_t *cursor)
{
    if (cursor == NULL || cursor->current == NULL) return -1;
    if (cursor->current == cursor->cached_tail) return -1;
    skip_list_node_t *first =
        atomic_load_explicit(&cursor->cached_header->forward[0], memory_order_acquire);
    return (cursor->current != first && cursor->current != cursor->cached_header) ? 1 : 0;
}

int skip_list_cursor_goto_last(skip_list_cursor_t *cursor)
{
    if (cursor == NULL) return -1;

    /* fast verified hint where the last node L satisfies L->forward[0] == tail.
     * we trust the tail's backward pointer only when forward confirms it; otherwise
     * forward-reseek the last node (predecessor of "+infinity"). */
    skip_list_node_t *tail = cursor->cached_tail;
    skip_list_node_t *last =
        atomic_load_explicit(&BACKWARD_PTR(tail, 0, tail->level), memory_order_acquire);
    if (last == NULL || last == cursor->cached_header ||
        atomic_load_explicit(&last->forward[0], memory_order_acquire) != tail)
    {
        last = skip_list_predecessor(cursor->list, cursor->cached_header, NULL, 0);
    }

    if (last == cursor->cached_header || NODE_IS_SENTINEL(last)) return -1;

    cursor->current = last;
    cursor->current_version = NULL;
    return 0;
}

int skip_list_cursor_goto_first(skip_list_cursor_t *cursor)
{
    if (cursor == NULL) return -1;
    skip_list_node_t *first =
        atomic_load_explicit(&cursor->cached_header->forward[0], memory_order_acquire);
    if (first == NULL || NODE_IS_SENTINEL(first)) return -1;
    cursor->current = first;
    cursor->current_version = NULL;
    return 0;
}

/**
 * skip_list_cursor_seek
 * positions cursor at the node before the first key >= target
 * @param cursor the cursor to position
 * @param key the target key to seek to
 * @param key_size size of the target key
 * @return 0 on success, -1 on failure
 *
 * after calling this function, cursor->current points to the predecessor node.
 * callers must call skip_list_cursor_next() or similar to access the actual target key.
 * this behavior allows efficient insertion and supports both exact matches and range queries.
 */
int skip_list_cursor_seek(skip_list_cursor_t *cursor, const uint8_t *key, const size_t key_size)
{
    if (cursor == NULL || key == NULL || key_size == 0) return -1;

    skip_list_node_t *current = cursor->cached_header;
    const int max_level =
        atomic_load_explicit(&cursor->list->level, memory_order_acquire); /* cache level */
    const skip_list_cmp_type_t cmp_type = cursor->list->cmp_type;

    /* we find the node before the target key */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, cursor->list, next->key,
                                                       next->key_size, key, key_size);
            if (cmp >= 0) break; /* we stop before target or equal */
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    /* we position cursor at the node before target
     * caller must call skip_list_cursor_next() to access first key >= target */
    cursor->current = current;
    cursor->current_version = NULL;
    return 0;
}

int skip_list_cursor_seek_ge(skip_list_cursor_t *cursor, const uint8_t *key, const size_t key_size)
{
    if (cursor == NULL || key == NULL || key_size == 0) return -1;

    skip_list_node_t *current = cursor->cached_header;
    const int max_level = atomic_load_explicit(&cursor->list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = cursor->list->cmp_type;

    /* we find the node before target */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, cursor->list, next->key,
                                                       next->key_size, key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        }
    }

    /* we land directly on the first entry >= target rather than parking before it and
     * leaving a separate next() to read forward[0]. a concurrent put can splice a node
     * whose key is < target into forward[0] in that window, so a seek+next pair can
     * return a key below target; re-reading forward[0] until we pass target closes it.
     * once current points at a node >= target, later sub-target inserts splice in before
     * it and do not move the cursor. */
    for (;;)
    {
        skip_list_node_t *nx = atomic_load_explicit(&current->forward[0], memory_order_acquire);
        if (nx == NULL || NODE_IS_SENTINEL(nx))
        {
            cursor->current = nx;
            cursor->current_version = NULL;
            return -1;
        }
        if (skip_list_compare_keys_with_type(cmp_type, cursor->list, nx->key, nx->key_size, key,
                                             key_size) >= 0)
        {
            cursor->current = nx;
            cursor->current_version = NULL;
            return 0;
        }
        current = nx;
    }
}

int skip_list_cursor_seek_for_prev(skip_list_cursor_t *cursor, const uint8_t *key,
                                   const size_t key_size)
{
    if (cursor == NULL || key == NULL || key_size == 0) return -1;

    skip_list_node_t *current = cursor->cached_header;
    const int max_level =
        atomic_load_explicit(&cursor->list->level, memory_order_acquire); /* cache level */
    const skip_list_cmp_type_t cmp_type = cursor->list->cmp_type;

    /* we find the last node with key <= target */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, cursor->list, next->key,
                                                       next->key_size, key, key_size);
            if (cmp > 0) break; /* stop when key > target */
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    /* the current is now the last node with key <= target, or header if no such key */
    if (NODE_IS_SENTINEL(current))
    {
        /* no key <= target exists, cursor is invalid */
        cursor->current = current;
        cursor->current_version = NULL;
        return 0;
    }

    cursor->current = current;
    cursor->current_version = NULL;
    return 0;
}

int skip_list_put_with_seq(skip_list_t *list, const uint8_t *key, size_t key_size,
                           const uint8_t *value, size_t value_size, int64_t ttl, uint64_t seq,
                           uint8_t flags)
{
    const int is_tombstone = (flags & SKIP_LIST_FLAG_DELETED) != 0;
    if (list == NULL || key == NULL || key_size == 0 || (!is_tombstone && value == NULL)) return -1;

    /* track the smallest seq ever inserted -- a compaction reads this to learn the oldest unflushed
     * write still held in this memtable, so it never reaps a tombstone newer than it. */
    {
        uint64_t cur = atomic_load_explicit(&list->min_seq, memory_order_relaxed);
        while (seq < cur &&
               !atomic_compare_exchange_weak_explicit(&list->min_seq, &cur, seq,
                                                      memory_order_relaxed, memory_order_relaxed))
        {
        }
    }

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    /* we use stack allocation for update array (SKIP_LIST_STACK_UPDATE_SIZE is file-scope) */
    skip_list_node_t *stack_update[SKIP_LIST_STACK_UPDATE_SIZE];
    skip_list_node_t **update;
    const int use_stack = (list->max_level < SKIP_LIST_STACK_UPDATE_SIZE);

    if (use_stack)
    {
        update = stack_update;
    }
    else
    {
        update = malloc((list->max_level + 1) * sizeof(skip_list_node_t *));
        if (!update) return -1;
    }

    for (int i = 0; i <= list->max_level; i++)
    {
        update[i] = header;
    }

    skip_list_node_t *current = header;

    /* we traverse with prefetching -- prefetch before sentinel check */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key, next->key_size,
                                                       key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
        update[i] = current;
    }

    skip_list_node_t *existing = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    if (existing != NULL && !NODE_IS_SENTINEL(existing))
    {
        int cmp = skip_list_compare_keys_with_type(cmp_type, list, existing->key,
                                                   existing->key_size, key, key_size);
        if (cmp == 0)
        {
            /* the key exists, we validate sequence and add new version */
            skip_list_version_t *latest =
                atomic_load_explicit(&existing->versions, memory_order_acquire);
            if (skip_list_validate_sequence(latest, seq) != 0)
            {
                if (!use_stack) free(update);
                return -1;
            }

            skip_list_version_t *new_version =
                skip_list_create_version(list, value, value_size, ttl, flags, seq);
            if (new_version == NULL)
            {
                if (!use_stack) free(update);
                return -1;
            }

            if (skip_list_insert_version_cas(&existing->versions, new_version, seq, list,
                                             value_size) != 0)
            {
                if (!use_stack) free(update);
                return -1;
            }

            if (!use_stack) free(update);
            return 0;
        }
    }

    skip_list_node_t *recheck = atomic_load_explicit(&update[0]->forward[0], memory_order_acquire);
    if (recheck != existing && recheck != NULL && !NODE_IS_SENTINEL(recheck))
    {
        int cmp = skip_list_compare_keys_with_type(cmp_type, list, recheck->key, recheck->key_size,
                                                   key, key_size);
        if (cmp == 0)
        {
            skip_list_version_t *latest =
                atomic_load_explicit(&recheck->versions, memory_order_acquire);
            if (skip_list_validate_sequence(latest, seq) != 0)
            {
                if (!use_stack) free(update);
                return -1;
            }

            skip_list_version_t *new_version =
                skip_list_create_version(list, value, value_size, ttl, flags, seq);
            if (new_version == NULL)
            {
                if (!use_stack) free(update);
                return -1;
            }

            if (skip_list_insert_version_cas(&recheck->versions, new_version, seq, list,
                                             value_size) != 0)
            {
                if (!use_stack) free(update);
                return -1;
            }

            if (!use_stack) free(update);
            return 0;
        }
    }

    int new_level = skip_list_random_level(list);
    int current_level = atomic_load_explicit(&list->level, memory_order_acquire);

    if (new_level > current_level)
    {
        for (int i = current_level + 1; i <= new_level; i++)
        {
            update[i] = header;
        }
        atomic_store_explicit(&list->level, new_level, memory_order_release);
    }

    /* we combine node + pointers + key into single allocation for cache locality */
    const size_t pointers_size = (2 * (new_level + 1)) * sizeof(_Atomic(skip_list_node_t *));
    skip_list_node_t *new_node =
        skip_list_alloc(list, sizeof(skip_list_node_t) + pointers_size + key_size);
    if (new_node == NULL)
    {
        if (!use_stack) free(update);
        return -1;
    }

    new_node->key = (uint8_t *)new_node + sizeof(skip_list_node_t) + pointers_size;
    memcpy(new_node->key, key, key_size);
    new_node->key_size = key_size;
    new_node->level = (uint8_t)new_level;
    new_node->node_flags = 0;

    skip_list_version_t *initial_version =
        skip_list_create_version(list, value, value_size, ttl, flags, seq);
    if (initial_version == NULL)
    {
        skip_list_dealloc(list, new_node);
        if (!use_stack) free(update);
        return -1;
    }
    atomic_init(&new_node->versions, initial_version);

    for (int i = 0; i <= new_level; i++)
    {
        atomic_init(&new_node->forward[i], NULL);
        atomic_init(&BACKWARD_PTR(new_node, i, new_level), NULL);
    }

    skip_list_node_t *pred = update[0];
    skip_list_node_t *next_at_0;
    int cas_attempts = 0;

    while (1)
    {
        next_at_0 = atomic_load_explicit(&pred->forward[0], memory_order_acquire);

        if (next_at_0 != NULL && !NODE_IS_SENTINEL(next_at_0))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next_at_0->key,
                                                       next_at_0->key_size, key, key_size);
            if (cmp == 0)
            {
                skip_list_version_t *latest =
                    atomic_load_explicit(&next_at_0->versions, memory_order_acquire);
                if (skip_list_validate_sequence(latest, seq) != 0)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                skip_list_version_t *new_version =
                    skip_list_create_version(list, value, value_size, ttl, flags, seq);
                if (new_version == NULL)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                if (skip_list_insert_version_cas(&next_at_0->versions, new_version, seq, list,
                                                 value_size) != 0)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                skip_list_free_node_internal(list, new_node);
                if (!use_stack) free(update);
                return 0;
            }
            if (cmp < 0)
            {
                pred = next_at_0;
                continue;
            }
        }

        atomic_store_explicit(&new_node->forward[0], next_at_0, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(&pred->forward[0], &next_at_0, new_node,
                                                  memory_order_release, memory_order_acquire))
        {
            update[0] = pred;
            break;
        }

        if (next_at_0 != NULL && !NODE_IS_SENTINEL(next_at_0))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next_at_0->key,
                                                       next_at_0->key_size, key, key_size);
            if (cmp == 0)
            {
                skip_list_version_t *latest =
                    atomic_load_explicit(&next_at_0->versions, memory_order_acquire);
                if (skip_list_validate_sequence(latest, seq) != 0)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                skip_list_version_t *new_version =
                    skip_list_create_version(list, value, value_size, ttl, flags, seq);
                if (new_version == NULL)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                if (skip_list_insert_version_cas(&next_at_0->versions, new_version, seq, list,
                                                 value_size) != 0)
                {
                    skip_list_free_node_internal(list, new_node);
                    if (!use_stack) free(update);
                    return -1;
                }

                skip_list_free_node_internal(list, new_node);
                if (!use_stack) free(update);
                return 0;
            }
            if (cmp < 0)
            {
                pred = next_at_0;
                continue;
            }
        }

        cas_attempts++;
        if (cas_attempts > SKIP_LIST_MAX_CAS_ATTEMPTS)
        {
            skip_list_free_node_internal(list, new_node);
            if (!use_stack) free(update);
            return -1;
        }
    }

    atomic_store_explicit(&BACKWARD_PTR(new_node, 0, new_level), update[0], memory_order_release);
    skip_list_node_t *next_after_insert =
        atomic_load_explicit(&new_node->forward[0], memory_order_acquire);
    if (next_after_insert != NULL)
    {
        skip_list_node_t *expected = update[0];
        atomic_compare_exchange_strong_explicit(
            &BACKWARD_PTR(next_after_insert, 0, next_after_insert->level), &expected, new_node,
            memory_order_release, memory_order_acquire);
    }

    for (int i = 1; i <= new_level; i++)
    {
        skip_list_node_t *next;
        do
        {
            next = atomic_load_explicit(&update[i]->forward[i], memory_order_acquire);
            atomic_store_explicit(&new_node->forward[i], next, memory_order_relaxed);
        } while (!atomic_compare_exchange_weak_explicit(
            &update[i]->forward[i], &next, new_node, memory_order_release, memory_order_acquire));

        atomic_store_explicit(&BACKWARD_PTR(new_node, i, new_level), update[i],
                              memory_order_release);
        if (next != NULL)
        {
            skip_list_node_t *expected = update[i];
            atomic_compare_exchange_strong_explicit(&BACKWARD_PTR(next, i, next->level), &expected,
                                                    new_node, memory_order_release,
                                                    memory_order_acquire);
        }
    }

    atomic_fetch_add_explicit(&list->total_size, key_size + value_size, memory_order_relaxed);
    atomic_fetch_add_explicit(&list->entry_count, 1, memory_order_relaxed);

    if (!use_stack) free(update);
    return 0;
}

int skip_list_put_batch(skip_list_t *list, const skip_list_batch_entry_t *entries,
                        const size_t count)
{
    if (list == NULL || entries == NULL || count == 0) return -1;

    /* track the smallest seq in this batch -- same min_seq bookkeeping as skip_list_put_with_seq,
     * which the batched commit path bypasses (a gap that lets a compaction reap a tombstone newer
     * than batched-but-unflushed data and resurrect the key). */
    {
        uint64_t batch_min = UINT64_MAX;
        for (size_t i = 0; i < count; i++)
            if (entries[i].seq < batch_min) batch_min = entries[i].seq;
        uint64_t cur = atomic_load_explicit(&list->min_seq, memory_order_relaxed);
        while (batch_min < cur &&
               !atomic_compare_exchange_weak_explicit(&list->min_seq, &cur, batch_min,
                                                      memory_order_relaxed, memory_order_relaxed))
        {
        }
    }

    int success_count = 0;

    /* we use a shared update array across batch entries for efficiency
     * this avoids repeated allocation/deallocation per entry */
    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);

    skip_list_node_t *stack_update[SKIP_LIST_STACK_UPDATE_SIZE];
    skip_list_node_t **update;
    const int use_stack = (list->max_level < SKIP_LIST_STACK_UPDATE_SIZE);

    if (use_stack)
    {
        update = stack_update;
    }
    else
    {
        update = malloc((list->max_level + 1) * sizeof(skip_list_node_t *));
        if (!update) return -1;
    }

    const skip_list_cmp_type_t cmp_type = list->cmp_type;
    const uint8_t *prev_key = NULL;
    size_t prev_key_size = 0;
    int prev_max_level = 0;
    size_t batch_total_size = 0;
    int batch_entry_count = 0;

    /* we initialize update array for the first entry */
    for (int i = 0; i <= list->max_level; i++)
    {
        update[i] = header;
    }

    for (size_t e = 0; e < count; e++)
    {
        const skip_list_batch_entry_t *entry = &entries[e];

        if (entry->key == NULL || entry->key_size == 0) continue;
        if (!(entry->flags & SKIP_LIST_FLAG_DELETED) && entry->value == NULL) continue;

        const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);

        /* sorted-key hint -- if this key >= previous key, reuse update[] positions
         * from previous iteration instead of restarting from header.
         * each update[i] has level >= i (set during traversal at that level)
         * so accessing update[i]->forward[i] is always safe. */
        int use_hint = 0;
        if (prev_key != NULL)
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, entry->key, entry->key_size,
                                                       prev_key, prev_key_size);
            use_hint = (cmp >= 0);
        }

        skip_list_node_t *current;
        if (!use_hint)
        {
            /* unsorted or first entry -- we reset to header */
            for (int i = 0; i <= list->max_level; i++)
            {
                update[i] = header;
            }
            current = header;
        }
        else
        {
            /* we init any new levels above prev_max_level to header */
            for (int i = prev_max_level + 1; i <= max_level; i++)
            {
                update[i] = header;
            }
            /* we start from the top-level hint, carry-down handles lower levels */
            current = update[max_level];
        }

        /* we traverse with prefetching -- prefetch before sentinel check */
        for (int i = max_level; i >= 0; i--)
        {
            skip_list_node_t *next =
                atomic_load_explicit(&current->forward[i], memory_order_acquire);

            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }

            while (next != NULL && !NODE_IS_SENTINEL(next))
            {
                int cmp = skip_list_compare_keys_with_type(
                    cmp_type, list, next->key, next->key_size, entry->key, entry->key_size);
                if (cmp >= 0) break;
                current = next;
                next = atomic_load_explicit(&current->forward[i], memory_order_acquire);

                if (SKIP_LIST_LIKELY(next != NULL))
                {
                    PREFETCH_READ(next);
                    PREFETCH_READ(next->key);
                }
            }
            update[i] = current;
        }

        prev_key = entry->key;
        prev_key_size = entry->key_size;
        prev_max_level = max_level;

        /* we check if key exists */
        skip_list_node_t *existing =
            atomic_load_explicit(&current->forward[0], memory_order_acquire);
        if (existing != NULL && !NODE_IS_SENTINEL(existing))
        {
            int cmp = skip_list_compare_keys_with_type(
                cmp_type, list, existing->key, existing->key_size, entry->key, entry->key_size);
            if (cmp == 0)
            {
                /* key exists, we add new version */
                skip_list_version_t *latest =
                    atomic_load_explicit(&existing->versions, memory_order_acquire);
                if (skip_list_validate_sequence(latest, entry->seq) != 0)
                {
                    continue; /* skip this entry */
                }

                skip_list_version_t *new_version = skip_list_create_version(
                    list, entry->value, entry->value_size, entry->ttl, entry->flags, entry->seq);
                if (new_version == NULL)
                {
                    continue;
                }

                if (skip_list_insert_version_cas(&existing->versions, new_version, entry->seq, list,
                                                 entry->value_size) == 0)
                {
                    success_count++;
                }
                continue;
            }
        }

        /* we create new node */
        int new_level = skip_list_random_level(list);
        int current_level = atomic_load_explicit(&list->level, memory_order_acquire);

        if (new_level > current_level)
        {
            for (int i = current_level + 1; i <= new_level; i++)
            {
                update[i] = header;
            }
            atomic_store_explicit(&list->level, new_level, memory_order_release);
        }

        /* we combine node + pointers + key into single allocation for cache locality */
        const size_t batch_ptrs_size = (2 * (new_level + 1)) * sizeof(_Atomic(skip_list_node_t *));
        skip_list_node_t *new_node =
            skip_list_alloc(list, sizeof(skip_list_node_t) + batch_ptrs_size + entry->key_size);
        if (new_node == NULL)
        {
            continue;
        }

        new_node->key = (uint8_t *)new_node + sizeof(skip_list_node_t) + batch_ptrs_size;
        memcpy(new_node->key, entry->key, entry->key_size);
        new_node->key_size = entry->key_size;
        new_node->level = (uint8_t)new_level;
        new_node->node_flags = 0;

        skip_list_version_t *initial_version = skip_list_create_version(
            list, entry->value, entry->value_size, entry->ttl, entry->flags, entry->seq);
        if (initial_version == NULL)
        {
            skip_list_dealloc(list, new_node);
            continue;
        }
        atomic_init(&new_node->versions, initial_version);

        for (int i = 0; i <= new_level; i++)
        {
            atomic_init(&new_node->forward[i], NULL);
            atomic_init(&BACKWARD_PTR(new_node, i, new_level), NULL);
        }

        /* we insert at level 0 with CAS */
        skip_list_node_t *pred = update[0];
        skip_list_node_t *next_at_0;
        int cas_attempts = 0;
        int inserted = 0;

        while (1)
        {
            next_at_0 = atomic_load_explicit(&pred->forward[0], memory_order_acquire);

            if (next_at_0 != NULL && !NODE_IS_SENTINEL(next_at_0))
            {
                int cmp = skip_list_compare_keys_with_type(cmp_type, list, next_at_0->key,
                                                           next_at_0->key_size, entry->key,
                                                           entry->key_size);
                if (cmp == 0)
                {
                    /* concurrent insert, we add version instead */
                    skip_list_version_t *latest =
                        atomic_load_explicit(&next_at_0->versions, memory_order_acquire);
                    if (skip_list_validate_sequence(latest, entry->seq) == 0)
                    {
                        skip_list_version_t *new_version =
                            skip_list_create_version(list, entry->value, entry->value_size,
                                                     entry->ttl, entry->flags, entry->seq);
                        if (new_version != NULL)
                        {
                            if (skip_list_insert_version_cas(&next_at_0->versions, new_version,
                                                             entry->seq, list,
                                                             entry->value_size) == 0)
                            {
                                success_count++;
                            }
                        }
                    }
                    skip_list_free_node_internal(list, new_node);
                    new_node = NULL; /* prevent use-after-free in higher level linking */
                    inserted = 1;
                    break;
                }
                if (cmp < 0)
                {
                    pred = next_at_0;
                    continue;
                }
            }

            atomic_store_explicit(&new_node->forward[0], next_at_0, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&pred->forward[0], &next_at_0, new_node,
                                                      memory_order_release, memory_order_acquire))
            {
                update[0] = pred;
                inserted = 1;
                break;
            }

            cas_attempts++;
            if (cas_attempts > SKIP_LIST_MAX_CAS_ATTEMPTS)
            {
                skip_list_free_node_internal(list, new_node);
                new_node = NULL; /* prevent use-after-free in higher level linking */
                inserted = 1;    /* mark as handled to avoid double-free */
                break;
            }
        }

        if (!inserted)
        {
            skip_list_free_node_internal(list, new_node);
            continue;
        }

        if (new_node != NULL && cas_attempts <= SKIP_LIST_MAX_CAS_ATTEMPTS && update[0] == pred)
        {
            /* we successfully inserted new node, link higher levels */
            atomic_store_explicit(&BACKWARD_PTR(new_node, 0, new_level), update[0],
                                  memory_order_release);
            skip_list_node_t *next_after_insert =
                atomic_load_explicit(&new_node->forward[0], memory_order_acquire);
            if (next_after_insert != NULL)
            {
                skip_list_node_t *expected = update[0];
                atomic_compare_exchange_strong_explicit(
                    &BACKWARD_PTR(next_after_insert, 0, next_after_insert->level), &expected,
                    new_node, memory_order_release, memory_order_acquire);
            }

            for (int i = 1; i <= new_level; i++)
            {
                skip_list_node_t *next;
                do
                {
                    next = atomic_load_explicit(&update[i]->forward[i], memory_order_acquire);
                    atomic_store_explicit(&new_node->forward[i], next, memory_order_relaxed);
                } while (!atomic_compare_exchange_weak_explicit(&update[i]->forward[i], &next,
                                                                new_node, memory_order_release,
                                                                memory_order_acquire));

                atomic_store_explicit(&BACKWARD_PTR(new_node, i, new_level), update[i],
                                      memory_order_release);
                if (next != NULL)
                {
                    skip_list_node_t *expected = update[i];
                    atomic_compare_exchange_strong_explicit(
                        &BACKWARD_PTR(next, i, next->level), &expected, new_node,
                        memory_order_release, memory_order_acquire);
                }
            }

            batch_total_size += entry->key_size + entry->value_size;
            batch_entry_count++;
            success_count++;
        }
    }

    /* we do a single atomic update for the entire batch instead of per-entry */
    if (batch_total_size > 0)
        atomic_fetch_add_explicit(&list->total_size, batch_total_size, memory_order_relaxed);
    if (batch_entry_count > 0)
        atomic_fetch_add_explicit(&list->entry_count, batch_entry_count, memory_order_relaxed);

    if (!use_stack) free(update);
    return success_count;
}

uint64_t skip_list_get_min_seq(skip_list_t *list)
{
    if (list == NULL) return UINT64_MAX;
    return atomic_load_explicit(&list->min_seq, memory_order_acquire);
}

int skip_list_get_max_seq(skip_list_t *list, const uint8_t *key, const size_t key_size,
                          uint64_t *out_seq)
{
    if (list == NULL || key == NULL || key_size == 0 || out_seq == NULL) return -1;

    *out_seq = 0;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key, next->key_size,
                                                       key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    if (target == NULL || NODE_IS_SENTINEL(target)) return -1;

    int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key, target->key_size, key,
                                               key_size);
    if (cmp != 0) return -1;

    skip_list_version_t *version = atomic_load_explicit(&target->versions, memory_order_acquire);
    if (version == NULL) return -1;

    *out_seq = atomic_load_explicit(&version->seq, memory_order_acquire);
    return 0;
}

int skip_list_get_with_seq(skip_list_t *list, const uint8_t *key, const size_t key_size,
                           uint8_t **value, size_t *value_size, int64_t *ttl, uint8_t *deleted,
                           uint64_t *seq, uint64_t snapshot_seq,
                           const skip_list_visibility_check_fn visibility_check,
                           void *visibility_ctx)
{
    if (list == NULL || key == NULL || key_size == 0 || value == NULL || value_size == NULL)
        return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level =
        atomic_load_explicit(&list->level, memory_order_acquire); /* cache level */
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    /* we attempt to find the node */
    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key, next->key_size,
                                                       key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    if (target == NULL || NODE_IS_SENTINEL(target)) return -1;

    int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key, target->key_size, key,
                                               key_size);
    if (cmp != 0) return -1;

    /* we found the key, now we must find the appropriate version */
    skip_list_version_t *version = atomic_load_explicit(&target->versions, memory_order_acquire);

    if (snapshot_seq == UINT64_MAX)
    {
        if (version == NULL) return -1;
    }
    else
    {
        /**
         * we find the newest committed version with seq <= snapshot_seq.
         * version chain is ordered newest-to-oldest, so we return the first
         * version that passes both checks. */
        while (version != NULL)
        {
            uint64_t version_seq = atomic_load_explicit(&version->seq, memory_order_acquire);

            /* we check if version is within snapshot range */
            if (version_seq <= snapshot_seq)
            {
                /* if visibility check provided, we verify this version is committed */
                if (visibility_check != NULL)
                {
                    if (visibility_check(visibility_ctx, version_seq))
                    {
                        /* we found the newest committed version within snapshot -- we use it */
                        break;
                    }
                    /* this version is not committed yet -- thus we check older versions */
                }
                else
                {
                    /* no visibility check -- we assume committed (for recovery, etc.) */
                    break;
                }
            }
            /* version is too new or not committed -- we check next (older) version */
            version = atomic_load_explicit(&version->next, memory_order_acquire);
        }

        if (version == NULL) return -1; /* no visible version */
    }

    /* we always set ttl if provided */
    if (ttl != NULL) *ttl = version->ttl;

    if (version->ttl > 0)
    {
        if (version->ttl <= skip_list_get_current_time(list))
        {
            if (deleted != NULL) *deleted = 1;
            *value = NULL;
            *value_size = 0;
            if (seq != NULL) *seq = atomic_load_explicit(&version->seq, memory_order_acquire);
            return 0; /* return success but mark as expired/deleted */
        }
    }

    uint8_t is_deleted = VERSION_IS_DELETED(version);
    if (deleted != NULL) *deleted = is_deleted;

    /* we return the value (even for tombstones, caller checks deleted flag) */
    if (!is_deleted && version->value != NULL && version->value_size > 0)
    {
        *value = malloc(version->value_size);
        if (*value == NULL) return -1;
        memcpy(*value, version->value, version->value_size);
        *value_size = version->value_size;
    }
    else
    {
        *value = NULL;
        *value_size = 0;
    }

    if (seq != NULL) *seq = atomic_load_explicit(&version->seq, memory_order_acquire);

    return 0;
}

int skip_list_get_with_seq_ref(skip_list_t *list, const uint8_t *key, const size_t key_size,
                               const uint8_t **value, size_t *value_size, int64_t *ttl,
                               uint8_t *deleted, uint64_t *seq, uint64_t snapshot_seq,
                               const skip_list_visibility_check_fn visibility_check,
                               void *visibility_ctx)
{
    if (list == NULL || key == NULL || key_size == 0 || value == NULL || value_size == NULL)
        return -1;

    skip_list_node_t *header = atomic_load_explicit(&list->header, memory_order_acquire);
    skip_list_node_t *current = header;
    const int max_level = atomic_load_explicit(&list->level, memory_order_acquire);
    const skip_list_cmp_type_t cmp_type = list->cmp_type;

    for (int i = max_level; i >= 0; i--)
    {
        skip_list_node_t *next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
        if (SKIP_LIST_LIKELY(next != NULL))
        {
            PREFETCH_READ(next);
            PREFETCH_READ(next->key);
        }

        while (next != NULL && !NODE_IS_SENTINEL(next))
        {
            int cmp = skip_list_compare_keys_with_type(cmp_type, list, next->key, next->key_size,
                                                       key, key_size);
            if (cmp >= 0) break;
            current = next;
            next = atomic_load_explicit(&current->forward[i], memory_order_acquire);
            if (SKIP_LIST_LIKELY(next != NULL))
            {
                PREFETCH_READ(next);
                PREFETCH_READ(next->key);
            }
        }
    }

    skip_list_node_t *target = atomic_load_explicit(&current->forward[0], memory_order_acquire);
    if (target == NULL || NODE_IS_SENTINEL(target)) return -1;

    int cmp = skip_list_compare_keys_with_type(cmp_type, list, target->key, target->key_size, key,
                                               key_size);
    if (cmp != 0) return -1;

    skip_list_version_t *version = atomic_load_explicit(&target->versions, memory_order_acquire);

    if (snapshot_seq == UINT64_MAX)
    {
        if (version == NULL) return -1;
    }
    else
    {
        while (version != NULL)
        {
            uint64_t version_seq = atomic_load_explicit(&version->seq, memory_order_acquire);

            if (version_seq <= snapshot_seq)
            {
                if (visibility_check != NULL)
                {
                    if (visibility_check(visibility_ctx, version_seq))
                    {
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
            version = atomic_load_explicit(&version->next, memory_order_acquire);
        }

        if (version == NULL) return -1;
    }

    if (ttl != NULL) *ttl = version->ttl;

    if (version->ttl > 0)
    {
        if (version->ttl <= skip_list_get_current_time(list))
        {
            if (deleted != NULL) *deleted = 1;
            *value = NULL;
            *value_size = 0;
            if (seq != NULL) *seq = atomic_load_explicit(&version->seq, memory_order_acquire);
            return 0;
        }
    }

    uint8_t is_deleted = VERSION_IS_DELETED(version);
    if (deleted != NULL) *deleted = is_deleted;

    if (!is_deleted && version->value != NULL && version->value_size > 0)
    {
        *value = version->value;
        *value_size = version->value_size;
    }
    else
    {
        *value = NULL;
        *value_size = 0;
    }

    if (seq != NULL) *seq = atomic_load_explicit(&version->seq, memory_order_acquire);

    return 0;
}