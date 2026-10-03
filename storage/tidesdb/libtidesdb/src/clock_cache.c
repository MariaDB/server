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

#include "clock_cache.h"

#include "../external/xxhash.h"

#define CLOCK_CACHE_PARTITION_FULL_THRESHOLD 85
#define CLOCK_CACHE_REF_BIT                  1u
#define CLOCK_CACHE_READER_INC               2u
#define CLOCK_CACHE_REF_MASK                 ((uint8_t)(~1u & 0xFFu))
#define CLOCK_CACHE_HAS_READERS(ref)         (((ref)&CLOCK_CACHE_REF_MASK) != 0)
/* reader field (bits 1-7) is saturated, all reader bits set == 127 concurrent readers.
 * one more READER_INC would carry out of the byte and wrap the field to 0, which would make
 * HAS_READERS read false while readers are live -- free_entry would then free under them. */
#define CLOCK_CACHE_READERS_SATURATED(ref) (((ref)&CLOCK_CACHE_REF_MASK) == CLOCK_CACHE_REF_MASK)
#define CLOCK_CACHE_PAYLOAD_ALIGN          8 /* align payload for safe typed access */
#define CLOCK_CACHE_ALIGN_UP(x, a)         (((x) + ((a)-1)) & ~((size_t)(a)-1))
#define CLOCK_CACHE_MAX_CPUS               1024

/* upper bound on the number of distinct L3 groups detect_l3_groups will track and the
 * sysfs path buffer used to read each cpu's shared-cache id */
#define CLOCK_CACHE_MAX_L3_GROUPS  64
#define CLOCK_CACHE_SYSFS_PATH_MAX 128

/* clock-evict scan limit -- we visit at most occupied * MULT slots per pass with a floor
 * of MIN, so sparsely populated partitions do not waste iterations on empty slots */
#define CLOCK_CACHE_EVICT_SCAN_MULT 3
#define CLOCK_CACHE_EVICT_SCAN_MIN  64

/**
 * detect_l3_groups
 * detect L3 cache topology by reading sysfs on Linux
 * groups CPUs by shared L3 cache (CCX on AMD, monolithic on Intel)
 * @param num_cpus number of CPUs to probe
 * @param cpu_to_group output array mapping CPU ID -> group ID
 * @return number of L3 groups (power of 2), or 1 if detection fails
 */
static int detect_l3_groups(int num_cpus, uint8_t *cpu_to_group)
{
    memset(cpu_to_group, 0, (size_t)num_cpus);

#if defined(__linux__)
    int seen_ids[CLOCK_CACHE_MAX_L3_GROUPS];
    int num_groups = 0;

    for (int cpu = 0; cpu < num_cpus; cpu++)
    {
        char path[CLOCK_CACHE_SYSFS_PATH_MAX];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index3/id", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        int id = -1;
        if (fscanf(f, "%d", &id) != 1) id = -1;
        fclose(f);
        if (id < 0) continue;

        int g;
        for (g = 0; g < num_groups; g++)
        {
            if (seen_ids[g] == id) break;
        }
        if (g == num_groups && num_groups < CLOCK_CACHE_MAX_L3_GROUPS)
        {
            seen_ids[num_groups++] = id;
        }
        if (cpu < num_cpus) cpu_to_group[cpu] = (uint8_t)g;
    }

    if (num_groups > 1)
    {
        /* we round down to power of 2 for masking */
        int p = 1;
        while (p * 2 <= num_groups) p <<= 1;
        if (p < num_groups)
        {
            for (int cpu = 0; cpu < num_cpus; cpu++)
                cpu_to_group[cpu] = cpu_to_group[cpu] % (uint8_t)p;
            return p;
        }
        return num_groups;
    }
#else
    (void)num_cpus;
    (void)cpu_to_group;
#endif
    return 1;
}

/**
 * get_local_partition
 * NUMA-aware partition selection -- routes threads to CCX-local partitions
 * on monolithic dies (num_groups=1), this is identical to hash & partition_mask
 * @param cache the cache
 * @param hash the key hash
 * @return partition index local to the calling thread's L3 group
 */
/** re-probe interval for thread migration detection.
 * every N calls we re-read the CPU ID to catch OS thread migrations
 * across CCX/NUMA boundaries. 4096 keeps the amortized cost negligible
 * (~one getcpu every few thousand cache ops) while catching migrations
 * within seconds under normal access rates. */
#define CLOCK_CACHE_GROUP_REPROBE_INTERVAL 4096

static inline size_t get_local_partition(const clock_cache_t *cache, uint64_t hash)
{
    if (cache->num_groups <= 1)
    {
        return (size_t)(hash & cache->partition_mask);
    }

    static THREAD_LOCAL int cached_group = -1;
    static THREAD_LOCAL unsigned int reprobe_counter = 0;
    int group = cached_group;
    if (TDB_UNLIKELY(group < 0 || ++reprobe_counter >= CLOCK_CACHE_GROUP_REPROBE_INTERVAL))
    {
        reprobe_counter = 0;
        const int cpu = tdb_get_cpu_id();
        group = (cpu >= 0 && cpu < cache->max_cpus) ? (int)cache->cpu_to_group[cpu] : 0;
        cached_group = group;
    }

    const size_t local_idx = (size_t)(hash & cache->local_partition_mask);
    return (size_t)group * cache->partitions_per_group + local_idx;
}

/**
 * clock_cache_sum_bytes
 * compute total bytes across all partitions by summing per-partition counters.
 * avoids contention on a single global atomic in the put/evict hot paths.
 * @param cache the cache
 * @return total bytes used
 */
static inline size_t clock_cache_sum_bytes(const clock_cache_t *cache)
{
    size_t total = 0;
    for (size_t i = 0; i < cache->num_partitions; i++)
    {
        total += atomic_load_explicit(&cache->partitions[i].bytes_used, memory_order_relaxed);
    }
    return total;
}

/**
 * entry_size
 * compute total entry size
 * @param key_len key length
 * @param payload_len payload length
 * @return total entry size
 */
static inline size_t entry_size(const size_t key_len, const size_t payload_len)
{
    return CLOCK_CACHE_ALIGN_UP(key_len, CLOCK_CACHE_PAYLOAD_ALIGN) + payload_len +
           sizeof(clock_cache_entry_t);
}

/**
 * compute_hash
 * compute full hash for key
 * @param key the key
 * @param key_len the key length
 * @return hash
 */
static inline uint64_t compute_hash(const char *key, const size_t key_len)
{
    return XXH3_64bits(key, key_len);
}

/**
 * hash_table_insert
 * insert slot into hash index with linear probing
 * @param partition the partition
 * @param hash the hash
 * @param slot_idx the slot index
 */
static void hash_table_insert(clock_cache_partition_t *partition, uint64_t hash,
                              const size_t slot_idx)
{
    clock_cache_entry_t *slot = &partition->slots[slot_idx];

    /* we store hash in entry for verification */
    atomic_store_explicit(&slot->cached_hash, hash, memory_order_release);

    /* we insert into hash index with linear probing */
    const size_t idx = hash & partition->hash_mask;
    const size_t max_probe = (partition->hash_index_size < CLOCK_CACHE_MAX_HASH_PROBE)
                                 ? partition->hash_index_size
                                 : CLOCK_CACHE_MAX_HASH_PROBE;
    for (size_t probe = 0; probe < max_probe; probe++)
    {
        const size_t pos = (idx + probe) & partition->hash_mask;
        int32_t expected = -1;

        /* we try to claim this hash index slot
         * weak CAS is sufficient since we're in a probe loop -- spurious failure
         * just advances to the next probe position */
        if (atomic_compare_exchange_weak(&partition->hash_index[pos], &expected, (int32_t)slot_idx))
        {
            return;
        }

        /* CAS failed; expected now holds the current value (CAS updates it on failure).
         * we check if this slot already points to our entry (reuse case) */
        if (expected == (int32_t)slot_idx)
        {
            return; /* already indexed */
        }
    }
}

/**
 * hash_table_remove
 * remove slot from hash index
 * @param partition the partition
 * @param hash the hash
 * @param slot_idx the slot index
 */
static void hash_table_remove(clock_cache_partition_t *partition, const uint64_t hash,
                              const size_t slot_idx)
{
    const size_t idx = hash & partition->hash_mask;
    const size_t max_probe = (partition->hash_index_size < CLOCK_CACHE_MAX_HASH_PROBE)
                                 ? partition->hash_index_size
                                 : CLOCK_CACHE_MAX_HASH_PROBE;
    size_t removed_pos = SIZE_MAX;

    /* we find the entry to remove */
    for (size_t probe = 0; probe < max_probe; probe++)
    {
        const size_t pos = (idx + probe) & partition->hash_mask;
        int32_t current = atomic_load_explicit(&partition->hash_index[pos], memory_order_acquire);

        if (current == (int32_t)slot_idx)
        {
            removed_pos = pos;
            break;
        }

        if (current == -1)
        {
            return; /* entry not in index */
        }
    }

    if (removed_pos == SIZE_MAX) return;

    /* backward-shift deletion, we shift subsequent entries back to fill the gap
     * this preserves the linear probing chain so lookups don't break */
    size_t empty = removed_pos;
    for (size_t step = 1; step < max_probe; step++)
    {
        const size_t candidate = (removed_pos + step) & partition->hash_mask;
        int32_t cand_slot =
            atomic_load_explicit(&partition->hash_index[candidate], memory_order_acquire);

        if (cand_slot == -1)
        {
            break; /* end of cluster */
        }

        /* we check if this entry's ideal position is at or before the empty slot
         * if so, shift it back to fill the gap */
        uint64_t cand_hash =
            atomic_load_explicit(&partition->slots[cand_slot].cached_hash, memory_order_relaxed);
        const size_t cand_ideal = cand_hash & partition->hash_mask;

        /* entry belongs at or before the empty slot if moving it would bring it
         * closer to (or keep it at) its ideal position.
         * with wrapping -- entry is displaced if its ideal position is in the range
         * (empty, candidate] on the circular index, i.e., it does not need to pass
         * through the empty slot to reach candidate from ideal position */
        int displaced;
        if (empty <= candidate)
            displaced = (cand_ideal <= empty || cand_ideal > candidate);
        else
            displaced = (cand_ideal <= empty && cand_ideal > candidate);

        if (displaced)
        {
            atomic_store_explicit(&partition->hash_index[empty], cand_slot, memory_order_release);
            empty = candidate;
        }
    }

    /* we clear the final empty position */
    atomic_store_explicit(&partition->hash_index[empty], -1, memory_order_release);
}

/**
 * try_match_entry
 * @param entry the entry
 * @param key the key
 * @param key_len the key length
 * @param target_hash the target hash
 * @return the entry or NULL if not found
 */
/* acquire a reader ref, refusing at saturation (see CLOCK_CACHE_READERS_SATURATED).
 * returns 1 with a ref held, 0 if already at max readers */
static inline int cc_try_pin_reader(clock_cache_entry_t *entry)
{
    uint8_t cur = atomic_load_explicit(&entry->ref_bit, memory_order_relaxed);
    for (;;)
    {
        if (CLOCK_CACHE_READERS_SATURATED(cur)) return 0;
        const uint8_t desired = (uint8_t)(cur + CLOCK_CACHE_READER_INC);
        if (atomic_compare_exchange_weak_explicit(&entry->ref_bit, &cur, desired,
                                                  memory_order_acq_rel, memory_order_relaxed))
            return 1;
        /* cur was reloaded with the current value on CAS failure -- retry */
    }
}

static clock_cache_entry_t *try_match_entry(clock_cache_entry_t *entry, const char *key,
                                            size_t key_len, uint64_t target_hash)
{
    uint8_t state = atomic_load_explicit(&entry->state, memory_order_relaxed);
    if (state != ENTRY_VALID) return NULL;

    uint64_t entry_hash = atomic_load_explicit(&entry->cached_hash, memory_order_relaxed);
    if (entry_hash != target_hash) return NULL;

    size_t entry_key_len = atomic_load_explicit(&entry->key_len, memory_order_relaxed);
    if (entry_key_len != key_len) return NULL;

    if (!cc_try_pin_reader(entry)) return NULL;

    /* we re-validate state after acquiring ref (entry may have been evicted between
     * our pre-checks and the ref acquisition) */
    state = atomic_load_explicit(&entry->state, memory_order_acquire);
    if (state != ENTRY_VALID)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    char *entry_key = atomic_load_explicit(&entry->key, memory_order_acquire);
    if (!entry_key)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    if (memcmp(entry_key, key, key_len) != 0)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    /* match! return with reader ref HELD -- caller must release via
     * atomic_fetch_sub(ref_bit, CLOCK_CACHE_READER_INC) when done */
    return entry;
}

static clock_cache_entry_t *find_entry_with_hash(clock_cache_partition_t *partition,
                                                 const char *key, const size_t key_len,
                                                 const uint64_t target_hash)
{
    /* we cache immutable struct fields in registers to prevent reloads across
     * atomic barriers in try_match_entry (acq_rel on ref_bit acts as compiler barrier,
     * forcing the compiler to reload partition->hash_mask etc. from memory each iteration) */
    const size_t hash_mask = partition->hash_mask;
    _Atomic(int32_t) *const hash_index = partition->hash_index;
    clock_cache_entry_t *const slots = partition->slots;

    const size_t idx = target_hash & hash_mask;
    const size_t max_probe = (partition->hash_index_size < CLOCK_CACHE_MAX_HASH_PROBE)
                                 ? partition->hash_index_size
                                 : CLOCK_CACHE_MAX_HASH_PROBE;
    /* we prefetch the first hash index entry before the loop to warm the cache line */
    PREFETCH_READ(&hash_index[idx]);

    for (size_t probe = 0; probe < max_probe; probe++)
    {
        const size_t pos = (idx + probe) & hash_mask;
        int32_t slot_idx = atomic_load_explicit(&hash_index[pos], memory_order_relaxed);

        if (slot_idx == -1)
        {
            /* empty slot in index, entry not found */
            return NULL;
        }

        /* we prefetch slot data + next hash index entry simultaneously
         * this gives memory subsystem time to warm both cache lines
         * before the next iteration's hash_index load and this iteration's try_match */
        PREFETCH_READ(&slots[slot_idx]);
        if (probe + 1 < max_probe)
        {
            const size_t next_pos = (idx + probe + 1) & hash_mask;
            PREFETCH_READ(&hash_index[next_pos]);
        }

        clock_cache_entry_t *entry = &slots[slot_idx];
        clock_cache_entry_t *match = try_match_entry(entry, key, key_len, target_hash);
        if (match) return match;
    }

    return NULL;
}

/**
 * free_entry
 * free entry contents -- lock-free with atomic state transitions
 * @param cache the cache
 * @param partition the partition
 * @param entry the entry
 */
static void free_entry(clock_cache_t *cache, clock_cache_partition_t *partition,
                       clock_cache_entry_t *entry)
{
    /* we try to claim entry for deletion using CAS */
    uint8_t expected = ENTRY_VALID;
    if (!atomic_compare_exchange_strong(&entry->state, &expected, ENTRY_DELETING))
    {
        /* someone else is deleting or entry is already empty */
        return;
    }

    char *key = atomic_load_explicit(&entry->key, memory_order_acquire);
    void *payload = atomic_load_explicit(&entry->payload, memory_order_acquire);
    const size_t plen = atomic_load_explicit(&entry->payload_len, memory_order_acquire);
    const size_t klen = atomic_load_explicit(&entry->key_len, memory_order_acquire);

    if (!key || !payload)
    {
        /* invalid entry, just mark as empty */
        atomic_store_explicit(&entry->state, ENTRY_EMPTY, memory_order_release);
        return;
    }

    /* we check if entry is being read (upper bits indicate active readers) */
    uint8_t ref = atomic_load_explicit(&entry->ref_bit, memory_order_acquire);
    if (CLOCK_CACHE_HAS_READERS(ref))
    {
        /* entry is being read by active readers, revert state and abort */
        atomic_store_explicit(&entry->state, ENTRY_VALID, memory_order_release);
        return;
    }

    /* we mark hash entry as deleted (tombstone) -- but keep back-pointer for reuse
     * we use cached hash to avoid redundant XXH3 recomputation */
    const uint64_t hash = atomic_load_explicit(&entry->cached_hash, memory_order_relaxed);
    const size_t slot_idx = entry - partition->slots;
    hash_table_remove(partition, hash, slot_idx);

    atomic_store_explicit(&entry->key, NULL, memory_order_release);
    atomic_store_explicit(&entry->payload, NULL, memory_order_release);

    /* we must re-check ref_bit after clearing pointers, a reader may have snuck in
     * between our first check and clearing pointers
     * the release stores above + this acquire load form a release-acquire pair */
    ref = atomic_load_explicit(&entry->ref_bit, memory_order_acquire);
    if (CLOCK_CACHE_HAS_READERS(ref))
    {
        /* a reader incremented ref_bit after we started deleting
         * restore pointers and revert state, we must let the reader finish */
        atomic_store_explicit(&entry->key, key, memory_order_release);
        atomic_store_explicit(&entry->payload, payload, memory_order_release);
        atomic_store_explicit(&entry->state, ENTRY_VALID, memory_order_release);
        hash_table_insert(partition, hash, slot_idx);
        return;
    }

    if (cache->evict_callback)
    {
        cache->evict_callback(payload, plen);
    }

    /* payload is embedded in same allocation as key -- single free */
    free(key);
    atomic_store_explicit(&entry->key_len, 0, memory_order_release);
    atomic_store_explicit(&entry->payload_len, 0, memory_order_release);
    atomic_store_explicit(&entry->ref_bit, 0, memory_order_release);

    const size_t ext_bytes = atomic_load_explicit(&entry->external_bytes, memory_order_relaxed);

    const size_t freed_bytes = entry_size(klen, plen) + ext_bytes;
    atomic_fetch_sub_explicit(&partition->occupied_count, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&partition->bytes_used, freed_bytes, memory_order_relaxed);

    /* we transition to empty state */
    atomic_store_explicit(&entry->state, ENTRY_EMPTY, memory_order_release);
}

/**
 * evict_for_space
 * CLOCK eviction that targets VALID entries to free memory.
 * skips EMPTY slots (unlike clock_evict which returns them).
 * uses two passes -- first pass clears ref_bits, second pass evicts.
 * @param cache the cache
 * @param partition the partition
 * @return 1 if an entry was evicted, 0 if no evictable entry found
 */
static int evict_for_space(clock_cache_t *cache, clock_cache_partition_t *partition)
{
    const size_t slots_mask = partition->slots_mask;
    const size_t start =
        atomic_fetch_add_explicit(&partition->clock_hand, 1, memory_order_relaxed) & slots_mask;

    /* we limit scan distance based on occupied count rather than total slots.
     * when the partition is sparsely populated (e.g., 128 entries in 8192 slots),
     * scanning all slots wastes 98%+ of iterations on EMPTY slots.
     * we scan at most occupied_count * CLOCK_CACHE_EVICT_SCAN_MULT slots (gives high
     * probability of finding a victim even with clustering) with a minimum of
     * CLOCK_CACHE_EVICT_SCAN_MIN. */
    const size_t occupied = atomic_load_explicit(&partition->occupied_count, memory_order_relaxed);
    size_t scan_limit = occupied * CLOCK_CACHE_EVICT_SCAN_MULT;
    if (scan_limit < CLOCK_CACHE_EVICT_SCAN_MIN) scan_limit = CLOCK_CACHE_EVICT_SCAN_MIN;
    if (scan_limit > partition->num_slots) scan_limit = partition->num_slots;

    /* pass 0 clears ref_bits, pass 1 evicts entries with ref_bit=0 */
    for (int pass = 0; pass < 2; pass++)
    {
        for (size_t i = 0; i < scan_limit; i++)
        {
            const size_t hand = (start + i) & slots_mask;
            clock_cache_entry_t *entry = &partition->slots[hand];

            const uint8_t state = atomic_load_explicit(&entry->state, memory_order_acquire);
            if (state != ENTRY_VALID) continue;

            const uint8_t ref = atomic_load_explicit(&entry->ref_bit, memory_order_acquire);
            if (CLOCK_CACHE_HAS_READERS(ref)) continue;

            if ((ref & CLOCK_CACHE_REF_BIT) == 0)
            {
                /* no ref_bit, no readers -- evict */
                free_entry(cache, partition, entry);
                atomic_store_explicit(&partition->clock_hand, hand + 1, memory_order_relaxed);
                return 1;
            }

            /* ref_bit set -- clear it (second chance), will evict on next pass */
            atomic_fetch_and_explicit(&entry->ref_bit, CLOCK_CACHE_REF_MASK, memory_order_relaxed);
        }
    }

    return 0;
}

/**
 * clock_evict
 * CLOCK second-chance eviction -- finds or frees a slot for new entry insertion
 * @param cache the cache
 * @param partition the partition
 * @return slot index of an available (empty or just-evicted) entry
 */
static size_t clock_evict(clock_cache_t *cache, clock_cache_partition_t *partition)
{
    size_t iterations = 0;
    const size_t max_iterations = partition->num_slots;

    /* we start from thread-local position to reduce contention on clock_hand */
    static THREAD_LOCAL size_t thread_hand = 0;
    if (thread_hand == 0)
    {
        thread_hand = (size_t)TDB_THREAD_ID();
        if (thread_hand == 0) thread_hand = 1; /* we ensure non-zero */
    }
    const size_t slots_mask = partition->slots_mask;
    const size_t start_pos = thread_hand & slots_mask;

    while (iterations < max_iterations)
    {
        /* we use local counter with occasional sync to global clock_hand */
        const size_t hand = (start_pos + iterations) & slots_mask;
        clock_cache_entry_t *entry = &partition->slots[hand];

        /* we prefetch 2 entries ahead to overlap memory latency with eviction logic */
        const size_t pf1 = (hand + 1) & slots_mask;
        const size_t pf2 = (hand + 2) & slots_mask;
        PREFETCH_READ(&partition->slots[pf1]);
        PREFETCH_READ(&partition->slots[pf2]);

        /* we check state atomically */
        uint8_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

        if (state == ENTRY_EMPTY)
        {
            /* found empty slot -- we update thread position for next time */
            thread_hand = hand + 1;
            return hand;
        }

        if (state != ENTRY_VALID)
        {
            iterations++;
            continue;
        }

        /* we check reference bit and active readers */
        uint8_t ref = atomic_load_explicit(&entry->ref_bit, memory_order_acquire);
        if (CLOCK_CACHE_HAS_READERS(ref))
        {
            if (ref & CLOCK_CACHE_REF_BIT)
            {
                atomic_fetch_and_explicit(&entry->ref_bit, CLOCK_CACHE_REF_MASK,
                                          memory_order_relaxed);
            }
            iterations++;
            continue;
        }

        if ((ref & CLOCK_CACHE_REF_BIT) == 0)
        {
            /* found victim -- try to evict */
            PREFETCH_WRITE(entry);
            free_entry(cache, partition, entry);

            /* we update thread position for next time */
            thread_hand = hand + 1;
            return hand;
        }

        atomic_fetch_and_explicit(&entry->ref_bit, CLOCK_CACHE_REF_MASK, memory_order_relaxed);

        iterations++;
    }

    /* we try to evict at current position as a fallback*/
    size_t hand = atomic_load_explicit(&partition->clock_hand, memory_order_acquire) & slots_mask;
    clock_cache_entry_t *entry = &partition->slots[hand];
    PREFETCH_WRITE(entry);
    uint8_t state = atomic_load_explicit(&entry->state, memory_order_acquire);

    if (state == ENTRY_VALID)
    {
        free_entry(cache, partition, entry);
    }

    return hand;
}

/**
 * ensure_space
 * ensure space in partition
 * @param cache the cache
 * @param partition the partition
 * @param required_bytes the required bytes
 * @return 0 on success, -1 on failure
 */
static int ensure_space(clock_cache_t *cache, clock_cache_partition_t *partition,
                        const size_t required_bytes)
{
    const size_t occupied = atomic_load_explicit(&partition->occupied_count, memory_order_relaxed);

    /* we check global byte budget (not per-partition) to avoid premature eviction
     * when hash distribution is uneven. a hot partition can use more than its "fair share"
     * as long as the total cache stays within budget.
     * we sum per-partition bytes_used instead of reading a single global atomic
     * to eliminate contention on the put/evict hot paths at high core counts. */
    const size_t global_bytes = clock_cache_sum_bytes(cache);
    if (global_bytes + required_bytes <= cache->max_bytes && occupied < partition->evict_threshold)
    {
        return 0;
    }

    /* byte-based eviction -- we enforce global byte budget via local eviction.
     * we evict from this partition to reduce global pressure. */
    if (cache->max_bytes > 0 && global_bytes + required_bytes > cache->max_bytes)
    {
        size_t cur_global = global_bytes;
        size_t evict_rounds = 0;
        const size_t max_evictions = occupied;
        while (cur_global + required_bytes > cache->max_bytes && evict_rounds < max_evictions)
        {
            if (!evict_for_space(cache, partition)) break;
            cur_global = clock_cache_sum_bytes(cache);
            evict_rounds++;
        }

        /* if local partition eviction wasn't enough, try other partitions.
         * this handles the case where entries are spread across many partitions
         * and a single partition can't free enough to meet the global byte budget
         * (common with large external_bytes like btree nodes). */
        if (cur_global + required_bytes > cache->max_bytes)
        {
            const size_t local_idx = (size_t)(partition - cache->partitions);
            for (size_t p = 1; p < cache->num_partitions; p++)
            {
                const size_t other_idx = (local_idx + p) & (cache->num_partitions - 1);
                clock_cache_partition_t *other = &cache->partitions[other_idx];
                const size_t other_occ =
                    atomic_load_explicit(&other->occupied_count, memory_order_relaxed);
                if (other_occ == 0) continue;

                size_t rounds = 0;
                while (rounds < other_occ)
                {
                    if (!evict_for_space(cache, other)) break;
                    rounds++;
                    const size_t now = clock_cache_sum_bytes(cache);
                    if (now + required_bytes <= cache->max_bytes) goto eviction_done;
                }
            }
        eviction_done:;
        }
    }

    /* slot-count-based eviction -- prevent hash table overload */
    if (occupied >= partition->evict_threshold)
    {
        clock_evict(cache, partition);
    }

    return 0;
}

void clock_cache_compute_config(const size_t max_bytes, cache_config_t *config)
{
    if (!config) return;

    const int num_cpus = tdb_get_cpu_count();

    size_t num_partitions = (size_t)num_cpus * CLOCK_CACHE_PARTITIONS_PER_CPU;
    if (num_partitions < CLOCK_CACHE_MIN_PARTITIONS) num_partitions = CLOCK_CACHE_MIN_PARTITIONS;
    if (num_partitions > CLOCK_CACHE_MAX_PARTITIONS) num_partitions = CLOCK_CACHE_MAX_PARTITIONS;

    /* we round up to next power of 2 for efficient masking */
    size_t p = 1;
    while (p < num_partitions) p <<= 1;
    num_partitions = p;

    /* slot count is sized for hash table efficiency (low load factor), not for byte budget.
     * when caller specifies avg_entry_size (e.g., btree 64KB nodes), use it to avoid
     * creating vastly more slots than entries that will fit in the byte budget.
     * otherwise use a small default so that many small entries probe efficiently. */
    const size_t avg_entry_size =
        (config->avg_entry_size > 0) ? config->avg_entry_size : CLOCK_CACHE_AVG_ENTRY_SIZE;
    size_t total_entries = max_bytes / avg_entry_size;
    if (total_entries < num_partitions) total_entries = num_partitions;

    /* we distribute entries across partitions */
    size_t slots_per_partition = total_entries / num_partitions;

    /* we clamp to reasonable range -- 64-8192 slots per partition */
    if (slots_per_partition < CLOCK_CACHE_MIN_SLOTS_PER_PARTITION)
        slots_per_partition = CLOCK_CACHE_MIN_SLOTS_PER_PARTITION;
    if (slots_per_partition > CLOCK_CACHE_MAX_SLOTS_PER_PARTITION)
        slots_per_partition = CLOCK_CACHE_MAX_SLOTS_PER_PARTITION;

    /* we round up to next power of 2 for better memory alignment */
    size_t s = CLOCK_CACHE_MIN_SLOTS_PER_PARTITION;
    while (s < slots_per_partition) s <<= 1;
    slots_per_partition = s;

    config->max_bytes = max_bytes;
    config->num_partitions = num_partitions;
    config->slots_per_partition = slots_per_partition;
    config->evict_callback = NULL; /* no callback by default */
}

clock_cache_t *clock_cache_create(const cache_config_t *config)
{
    if (!config || config->num_partitions == 0 || config->slots_per_partition == 0)
    {
        return NULL;
    }

    clock_cache_t *cache = (clock_cache_t *)calloc(1, sizeof(clock_cache_t));
    if (!cache) return NULL;

    cache->num_partitions = config->num_partitions;
    cache->max_bytes = config->max_bytes;
    cache->partition_mask = config->num_partitions - 1; /* assumes power of 2 */
    cache->evict_callback = config->evict_callback;     /* store eviction callback */
    atomic_store_explicit(&cache->total_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&cache->hits, 0, memory_order_relaxed);
    atomic_store_explicit(&cache->misses, 0, memory_order_relaxed);
    atomic_store_explicit(&cache->shutdown, 0, memory_order_relaxed);

    /** we detect L3/CCX topology for NUMA-aware partition routing */
    cache->max_cpus = tdb_get_cpu_count();
    if (cache->max_cpus > CLOCK_CACHE_MAX_CPUS) cache->max_cpus = CLOCK_CACHE_MAX_CPUS;
    cache->cpu_to_group = (uint8_t *)calloc((size_t)cache->max_cpus, sizeof(uint8_t));
    if (!cache->cpu_to_group)
    {
        free(cache);
        return NULL;
    }

    cache->num_groups = (size_t)detect_l3_groups(cache->max_cpus, cache->cpu_to_group);
    if (cache->num_groups > config->num_partitions) cache->num_groups = 1;
    cache->partitions_per_group = config->num_partitions / cache->num_groups;
    cache->local_partition_mask = cache->partitions_per_group - 1;

    cache->partitions =
        (clock_cache_partition_t *)calloc(config->num_partitions, sizeof(clock_cache_partition_t));
    if (!cache->partitions)
    {
        free(cache->cpu_to_group);
        free(cache);
        return NULL;
    }

    for (size_t i = 0; i < config->num_partitions; i++)
    {
        clock_cache_partition_t *partition = &cache->partitions[i];
        partition->num_slots = config->slots_per_partition;
        partition->slots_mask = config->slots_per_partition - 1;
        partition->evict_threshold =
            (config->slots_per_partition * CLOCK_CACHE_PARTITION_FULL_THRESHOLD) / 100;
        atomic_store_explicit(&partition->clock_hand, 0, memory_order_relaxed);
        atomic_store_explicit(&partition->occupied_count, 0, memory_order_relaxed);
        atomic_store_explicit(&partition->bytes_used, 0, memory_order_relaxed);
        atomic_store_explicit(&partition->hits, 0, memory_order_relaxed);
        atomic_store_explicit(&partition->misses, 0, memory_order_relaxed);

        /* we calculate hash index size (2x slots for low collision rate) */
        partition->hash_index_size =
            (config->slots_per_partition * CLOCK_CACHE_HASH_INDEX_MULTIPLIER_NUM) /
            CLOCK_CACHE_HASH_INDEX_MULTIPLIER_DEN;
        /* we round up to next power of 2 */
        size_t size = 1;
        while (size < partition->hash_index_size) size <<= 1;
        partition->hash_index_size = size;
        partition->hash_mask = size - 1;

        partition->slots =
            (clock_cache_entry_t *)calloc(config->slots_per_partition, sizeof(clock_cache_entry_t));
        if (!partition->slots)
        {
            for (size_t j = 0; j < i; j++)
            {
                free((void *)cache->partitions[j].hash_index);
                free(cache->partitions[j].slots);
            }
            free(cache->partitions);
            free(cache->cpu_to_group);
            free(cache);
            return NULL;
        }

        partition->hash_index =
            (_Atomic(int32_t) *)calloc(partition->hash_index_size, sizeof(_Atomic(int32_t)));
        if (!partition->hash_index)
        {
            free(partition->slots);
            for (size_t j = 0; j < i; j++)
            {
                free((void *)cache->partitions[j].hash_index);
                free(cache->partitions[j].slots);
            }
            free(cache->partitions);
            free(cache->cpu_to_group);
            free(cache);
            return NULL;
        }

        /* we initialize hash index to -1 (which is empty) */
        for (size_t j = 0; j < partition->hash_index_size; j++)
        {
            atomic_store_explicit(&partition->hash_index[j], -1, memory_order_relaxed);
        }

        /* we initialize all entry states to EMPTY */
        for (size_t j = 0; j < partition->num_slots; j++)
        {
            atomic_store_explicit(&partition->slots[j].state, ENTRY_EMPTY, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].key, NULL, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].payload, NULL, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].key_len, 0, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].payload_len, 0, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].ref_bit, 0, memory_order_relaxed);
            atomic_store_explicit(&partition->slots[j].cached_hash, 0, memory_order_relaxed);
        }
    }

    return cache;
}

void clock_cache_destroy(clock_cache_t *cache)
{
    if (!cache) return;

    atomic_store_explicit(&cache->shutdown, 1, memory_order_release);

    /* mem fence, ensure all threads see shutdown flag */
    atomic_thread_fence(memory_order_seq_cst);

    for (size_t i = 0; i < cache->num_partitions; i++)
    {
        clock_cache_partition_t *partition = &cache->partitions[i];

        /* we mark all entries as deleting first to stop new accesses */
        for (size_t j = 0; j < partition->num_slots; j++)
        {
            uint8_t state = atomic_load_explicit(&partition->slots[j].state, memory_order_acquire);
            if (state == ENTRY_VALID || state == ENTRY_WRITING)
            {
                atomic_store_explicit(&partition->slots[j].state, ENTRY_DELETING,
                                      memory_order_release);
            }
        }

        /* mem fence -- ensure all readers see DELETING state */
        atomic_thread_fence(memory_order_seq_cst);

        for (size_t j = 0; j < partition->num_slots; j++)
        {
            char *key = atomic_load_explicit(&partition->slots[j].key, memory_order_acquire);
            void *payload =
                atomic_load_explicit(&partition->slots[j].payload, memory_order_acquire);
            const size_t payload_len =
                atomic_load_explicit(&partition->slots[j].payload_len, memory_order_acquire);

            if (payload && cache->evict_callback)
            {
                cache->evict_callback(payload, payload_len);
            }

            /* payload is embedded in same allocation as key -- single free */
            if (key) free(key);
        }

        free((void *)partition->hash_index);
        free(partition->slots);
    }

    free(cache->partitions);
    free(cache->cpu_to_group);
    free(cache);
}

int clock_cache_put(clock_cache_t *cache, const char *key, size_t key_len, const void *payload,
                    size_t payload_len, size_t external_bytes)
{
    if (!cache || !key || key_len == 0 || !payload) return -1;

    if (atomic_load_explicit(&cache->shutdown, memory_order_acquire)) return -1;

    const uint64_t hash = compute_hash(key, key_len);
    const size_t partition_idx = get_local_partition(cache, hash);
    clock_cache_partition_t *partition = &cache->partitions[partition_idx];
    const size_t entry_bytes = entry_size(key_len, payload_len) + external_bytes;

    /* we try to find and invalidate existing entry (best-effort update) */
    clock_cache_entry_t *old_entry = find_entry_with_hash(partition, key, key_len, hash);
    if (old_entry)
    {
        /* we release reader ref before free_entry (which checks for active readers) */
        atomic_fetch_sub_explicit(&old_entry->ref_bit, CLOCK_CACHE_READER_INC,
                                  memory_order_acq_rel);
        free_entry(cache, partition, old_entry);
    }

    /* we always ensure space to enforce max_bytes limit */
    ensure_space(cache, partition, entry_bytes);

    clock_cache_entry_t *entry = NULL;
    size_t slot_idx = 0;
    const int max_retries = CLOCK_CACHE_MAX_PUT_RETRIES;

    for (int retry = 0; retry < max_retries; retry++)
    {
        slot_idx = clock_evict(cache, partition);
        entry = &partition->slots[slot_idx];
        PREFETCH_WRITE(entry);

        /* we try to claim slot with CAS, EMPTY --> WRITING */
        uint8_t expected = ENTRY_EMPTY;
        if (atomic_compare_exchange_strong(&entry->state, &expected, ENTRY_WRITING))
        {
            /* got it */
            break;
        }

        /* someone else claimed it, try again */
        entry = NULL;
    }

    if (!entry)
    {
        /* failed to claim slot after retries */
        return -1;
    }

    /* we own the slot now, allocate key + payload in single allocation
     * payload is aligned to CLOCK_CACHE_PAYLOAD_ALIGN for safe typed access
     * this halves malloc calls and improves data locality */
    const size_t aligned_key_len = CLOCK_CACHE_ALIGN_UP(key_len, CLOCK_CACHE_PAYLOAD_ALIGN);
    char *new_buf = (char *)malloc(aligned_key_len + payload_len);
    if (!new_buf)
    {
        atomic_store_explicit(&entry->state, ENTRY_EMPTY, memory_order_release);
        return -1;
    }

    char *new_key = new_buf;
    void *new_payload = new_buf + aligned_key_len;
    memcpy(new_key, key, key_len);
    memcpy(new_payload, payload, payload_len);

    atomic_store_explicit(&entry->key, new_key, memory_order_release);
    atomic_store_explicit(&entry->payload, new_payload, memory_order_release);
    atomic_store_explicit(&entry->key_len, key_len, memory_order_release);
    atomic_store_explicit(&entry->payload_len, payload_len, memory_order_release);
    atomic_store_explicit(&entry->ref_bit, CLOCK_CACHE_REF_BIT, memory_order_release);
    atomic_store_explicit(&entry->external_bytes, external_bytes, memory_order_release);

    /* we transition to valid, entry is now visible */
    atomic_store_explicit(&entry->state, ENTRY_VALID, memory_order_release);

    atomic_fetch_add_explicit(&partition->occupied_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&partition->bytes_used, entry_bytes, memory_order_relaxed);

    hash_table_insert(partition, hash, slot_idx);

    return 0;
}

int clock_cache_put_new(clock_cache_t *cache, const char *key, size_t key_len, const void *payload,
                        size_t payload_len, size_t external_bytes)
{
    if (!cache || !key || key_len == 0 || !payload) return -1;

    if (atomic_load_explicit(&cache->shutdown, memory_order_acquire)) return -1;

    const uint64_t hash = compute_hash(key, key_len);
    const size_t partition_idx = get_local_partition(cache, hash);
    clock_cache_partition_t *partition = &cache->partitions[partition_idx];
    const size_t entry_bytes = entry_size(key_len, payload_len) + external_bytes;

    /* skip find_entry_with_hash -- caller guarantees key is not in cache
     * this saves a full hash table probe on the cache-miss-then-populate path */

    ensure_space(cache, partition, entry_bytes);

    clock_cache_entry_t *entry = NULL;
    size_t slot_idx = 0;
    const int max_retries = CLOCK_CACHE_MAX_PUT_RETRIES;

    for (int retry = 0; retry < max_retries; retry++)
    {
        slot_idx = clock_evict(cache, partition);
        entry = &partition->slots[slot_idx];
        PREFETCH_WRITE(entry);

        uint8_t expected = ENTRY_EMPTY;
        if (atomic_compare_exchange_strong(&entry->state, &expected, ENTRY_WRITING))
        {
            break;
        }

        entry = NULL;
    }

    if (!entry)
    {
        return -1;
    }

    const size_t aligned_key_len = CLOCK_CACHE_ALIGN_UP(key_len, CLOCK_CACHE_PAYLOAD_ALIGN);
    char *new_buf = (char *)malloc(aligned_key_len + payload_len);
    if (!new_buf)
    {
        atomic_store_explicit(&entry->state, ENTRY_EMPTY, memory_order_release);
        return -1;
    }

    char *new_key = new_buf;
    void *new_payload = new_buf + aligned_key_len;
    memcpy(new_key, key, key_len);
    memcpy(new_payload, payload, payload_len);

    atomic_store_explicit(&entry->key, new_key, memory_order_release);
    atomic_store_explicit(&entry->payload, new_payload, memory_order_release);
    atomic_store_explicit(&entry->key_len, key_len, memory_order_release);
    atomic_store_explicit(&entry->payload_len, payload_len, memory_order_release);
    atomic_store_explicit(&entry->ref_bit, CLOCK_CACHE_REF_BIT, memory_order_release);
    atomic_store_explicit(&entry->external_bytes, external_bytes, memory_order_release);

    atomic_store_explicit(&entry->state, ENTRY_VALID, memory_order_release);

    atomic_fetch_add_explicit(&partition->occupied_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&partition->bytes_used, entry_bytes, memory_order_relaxed);

    hash_table_insert(partition, hash, slot_idx);

    return 0;
}

uint8_t *clock_cache_get(clock_cache_t *cache, const char *key, const size_t key_len,
                         size_t *payload_len)
{
    if (!cache || !key || key_len == 0) return NULL;

    if (atomic_load_explicit(&cache->shutdown, memory_order_acquire)) return NULL;

    const uint64_t hash = compute_hash(key, key_len);
    const size_t partition_idx = get_local_partition(cache, hash);
    clock_cache_partition_t *partition = &cache->partitions[partition_idx];

    /* find_entry_with_hash returns entry with reader ref HELD (from try_match_entry) */
    clock_cache_entry_t *entry = find_entry_with_hash(partition, key, key_len, hash);

    if (!entry)
    {
        atomic_fetch_add_explicit(&partition->misses, 1, memory_order_relaxed);
        return NULL;
    }

    /* reader ref is held -- entry is protected from eviction */
    uint8_t *entry_payload = atomic_load_explicit(&entry->payload, memory_order_acquire);
    size_t entry_payload_len = atomic_load_explicit(&entry->payload_len, memory_order_acquire);

    if (!entry_payload || entry_payload_len == 0)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    uint8_t *result = (uint8_t *)malloc(entry_payload_len);
    if (!result)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    memcpy(result, entry_payload, entry_payload_len);

    /* we release reader ref and conditionally mark as recently used
     * combining two atomic RMWs into one when ref bit is already set (hot entries) */
    uint8_t old_ref =
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
    if (!(old_ref & CLOCK_CACHE_REF_BIT))
    {
        atomic_fetch_or_explicit(&entry->ref_bit, CLOCK_CACHE_REF_BIT, memory_order_relaxed);
    }

    if (payload_len) *payload_len = entry_payload_len;

    atomic_fetch_add_explicit(&partition->hits, 1, memory_order_relaxed);
    return result;
}

const uint8_t *clock_cache_get_zero_copy(clock_cache_t *cache, const char *key,
                                         const size_t key_len, size_t *payload_len,
                                         clock_cache_entry_t **entry_out)
{
    if (!cache || !key || key_len == 0) return NULL;

    if (atomic_load_explicit(&cache->shutdown, memory_order_acquire)) return NULL;

    const uint64_t hash = compute_hash(key, key_len);
    const size_t partition_idx = get_local_partition(cache, hash);
    clock_cache_partition_t *partition = &cache->partitions[partition_idx];

    /* find_entry_with_hash returns entry with reader ref HELD (from try_match_entry) */
    clock_cache_entry_t *entry = find_entry_with_hash(partition, key, key_len, hash);

    if (!entry)
    {
        atomic_fetch_add_explicit(&partition->misses, 1, memory_order_relaxed);
        return NULL;
    }

    /* reader ref is held -- entry is protected from eviction */
    uint8_t *entry_payload = atomic_load_explicit(&entry->payload, memory_order_acquire);
    size_t entry_payload_len = atomic_load_explicit(&entry->payload_len, memory_order_acquire);

    if (!entry_payload || entry_payload_len == 0)
    {
        atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
        return NULL;
    }

    if (payload_len) *payload_len = entry_payload_len;
    if (entry_out) *entry_out = entry;

    /* we conditionally mark as recently used -- skip atomic RMW when ref bit is already set
     * (hot entries); caller releases ref via clock_cache_release() */
    uint8_t cur_ref = atomic_load_explicit(&entry->ref_bit, memory_order_relaxed);
    if (!(cur_ref & CLOCK_CACHE_REF_BIT))
    {
        atomic_fetch_or_explicit(&entry->ref_bit, CLOCK_CACHE_REF_BIT, memory_order_relaxed);
    }

    atomic_fetch_add_explicit(&partition->hits, 1, memory_order_relaxed);
    return entry_payload;
}

void clock_cache_release(clock_cache_entry_t *entry)
{
    if (!entry) return;
    atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
}

int clock_cache_delete(clock_cache_t *cache, const char *key, const size_t key_len)
{
    if (!cache || !key || key_len == 0) return -1;

    if (atomic_load_explicit(&cache->shutdown, memory_order_acquire)) return -1;

    const uint64_t hash = compute_hash(key, key_len);
    const size_t partition_idx = get_local_partition(cache, hash);
    clock_cache_partition_t *partition = &cache->partitions[partition_idx];

    clock_cache_entry_t *entry = find_entry_with_hash(partition, key, key_len, hash);

    if (!entry)
    {
        return -1;
    }

    /* we release reader ref before free_entry (which checks for active readers) */
    atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC, memory_order_acq_rel);
    free_entry(cache, partition, entry);

    return 0;
}

void clock_cache_clear(clock_cache_t *cache)
{
    if (!cache) return;

    for (size_t i = 0; i < cache->num_partitions; i++)
    {
        clock_cache_partition_t *partition = &cache->partitions[i];

        for (size_t j = 0; j < partition->num_slots; j++)
        {
            uint8_t state = atomic_load_explicit(&partition->slots[j].state, memory_order_acquire);
            if (state == ENTRY_VALID)
            {
                free_entry(cache, partition, &partition->slots[j]);
            }
        }
    }

    /* we reset per-partition byte counters (may have residual from reader-held entries) */
    for (size_t i = 0; i < cache->num_partitions; i++)
    {
        atomic_store_explicit(&cache->partitions[i].bytes_used, 0, memory_order_relaxed);
    }
}

void clock_cache_get_stats(clock_cache_t *cache, clock_cache_stats_t *stats)
{
    if (!cache || !stats) return;

    /* we use tracked per-partition counters instead of scanning all slots
     * this is O(num_partitions) instead of O(total_slots) */
    size_t total_bytes = 0;
    size_t total_entries = 0;
    uint64_t total_hits = 0;
    uint64_t total_misses = 0;
    for (size_t i = 0; i < cache->num_partitions; i++)
    {
        total_bytes += atomic_load_explicit(&cache->partitions[i].bytes_used, memory_order_relaxed);
        total_entries +=
            atomic_load_explicit(&cache->partitions[i].occupied_count, memory_order_relaxed);
        total_hits += atomic_load_explicit(&cache->partitions[i].hits, memory_order_relaxed);
        total_misses += atomic_load_explicit(&cache->partitions[i].misses, memory_order_relaxed);
    }

    stats->total_bytes = total_bytes;
    stats->total_entries = total_entries;
    stats->hits = total_hits;
    stats->misses = total_misses;
    stats->num_partitions = cache->num_partitions;

    const uint64_t total_accesses = stats->hits + stats->misses;
    stats->hit_rate = (total_accesses > 0) ? ((double)stats->hits / (double)total_accesses) : 0.0;
}

size_t clock_cache_delete_by_prefix(clock_cache_t *cache, const char *prefix,
                                    const size_t prefix_len)
{
    if (!cache || !prefix || prefix_len == 0) return 0;

    size_t count = 0;

    for (size_t p = 0; p < cache->num_partitions; p++)
    {
        clock_cache_partition_t *partition = &cache->partitions[p];

        for (size_t i = 0; i < partition->num_slots; i++)
        {
            clock_cache_entry_t *entry = &partition->slots[i];

            uint8_t state = atomic_load_explicit(&entry->state, memory_order_acquire);
            if (state != ENTRY_VALID) continue;

            if (!cc_try_pin_reader(entry)) continue;

            state = atomic_load_explicit(&entry->state, memory_order_acquire);
            if (state != ENTRY_VALID)
            {
                atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                          memory_order_release);
                continue;
            }

            char *key = atomic_load_explicit(&entry->key, memory_order_acquire);
            size_t key_len = atomic_load_explicit(&entry->key_len, memory_order_acquire);

            const int match =
                (key && key_len >= prefix_len && memcmp(key, prefix, prefix_len) == 0);

            /** we release reader ref before calling free_entry
             * free_entry checks for active readers and aborts if any are held */
            atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                      memory_order_acq_rel);

            if (match)
            {
                free_entry(cache, partition, entry);
                count++;
            }
        }
    }

    return count;
}

size_t clock_cache_foreach_prefix(clock_cache_t *cache, const char *prefix, size_t prefix_len,
                                  const clock_cache_foreach_callback_t callback, void *user_data)
{
    if (!cache || !prefix || prefix_len == 0 || !callback) return 0;

    size_t count = 0;

    for (size_t p = 0; p < cache->num_partitions; p++)
    {
        clock_cache_partition_t *partition = &cache->partitions[p];

        for (size_t i = 0; i < partition->num_slots; i++)
        {
            clock_cache_entry_t *entry = &partition->slots[i];

            /* we check if entry is valid */
            uint8_t state = atomic_load_explicit(&entry->state, memory_order_acquire);
            if (state != ENTRY_VALID) continue;

            if (!cc_try_pin_reader(entry)) continue;

            /* we re-verify state after incrementing ref_bit */
            state = atomic_load_explicit(&entry->state, memory_order_acquire);
            if (state != ENTRY_VALID)
            {
                atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                          memory_order_release);
                continue;
            }

            char *key_recheck = atomic_load_explicit(&entry->key, memory_order_acquire);
            size_t key_len = atomic_load_explicit(&entry->key_len, memory_order_acquire);
            if (!key_recheck || key_len < prefix_len)
            {
                atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                          memory_order_release);
                continue;
            }

            /* we check prefix match */
            if (memcmp(key_recheck, prefix, prefix_len) == 0)
            {
                const uint8_t *payload =
                    atomic_load_explicit(&entry->payload, memory_order_acquire);
                const size_t payload_len =
                    atomic_load_explicit(&entry->payload_len, memory_order_acquire);

                if (payload)
                {
                    atomic_fetch_or_explicit(&entry->ref_bit, CLOCK_CACHE_REF_BIT,
                                             memory_order_relaxed);
                    int result = callback(key_recheck, key_len, payload, payload_len, user_data);
                    count++;

                    atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                              memory_order_release);

                    if (result != 0) return count;
                }
                else
                {
                    atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                              memory_order_release);
                }
            }
            else
            {
                atomic_fetch_sub_explicit(&entry->ref_bit, CLOCK_CACHE_READER_INC,
                                          memory_order_release);
            }
        }
    }

    return count;
}