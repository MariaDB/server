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
#ifndef __CLOCK_CACHE_H__
#define __CLOCK_CACHE_H__
#include "compat.h"

/* forward declarations */
typedef struct clock_cache_t clock_cache_t;
typedef struct clock_cache_partition_t clock_cache_partition_t;

/**
 * clock_cache_evict_fn
 * callback function for custom cleanup when cache entry is evicted
 * @param payload pointer to the payload being evicted
 * @param payload_len length of the payload
 */
typedef void (*clock_cache_evict_fn)(void *payload, size_t payload_len);

/**
 * cache_config_t
 * configuration for cache creation
 * @param max_bytes maximum total bytes across all partitions
 * @param num_partitions number of partitions (power of 2 recommended)
 * @param slots_per_partition initial slots per partition
 * @param evict_callback optional callback for custom cleanup on eviction (can be NULL)
 */
typedef struct
{
    size_t max_bytes;
    size_t num_partitions;
    size_t slots_per_partition;
    size_t avg_entry_size; /* expected average entry size in bytes (0 = use default 100) */
    clock_cache_evict_fn evict_callback;
} cache_config_t;

/**
 * clock_cache_entry_t
 * individual cache entry in a slot
 * lock-free design using atomic state machine
 * @param key atomic pointer to heap-allocated key
 * @param payload atomic pointer to heap-allocated payload
 * @param key_len atomic key length
 * @param payload_len atomic payload length
 * @param ref_bit atomic ref bit (LSB) plus reader count in upper bits
 * @param state atomic state -- 0=empty, 1=writing, 2=valid, 3=deleting
 * @param cached_hash cached hash value for this entry
 * @param external_bytes caller-declared memory cost of pointed-to data
 */
typedef struct
{
    _Atomic(char *) key;
    _Atomic(void *) payload;
    atomic_size_t key_len;
    atomic_size_t payload_len;
    _Atomic(uint8_t) ref_bit;
    _Atomic(uint8_t) state;
    atomic_uint64_t cached_hash;
    atomic_size_t external_bytes;
} clock_cache_entry_t;

/** entry states */
#define ENTRY_EMPTY    0
#define ENTRY_WRITING  1
#define ENTRY_VALID    2
#define ENTRY_DELETING 3

/** cache configuration constants */
#define CLOCK_CACHE_MAX_PUT_RETRIES         100  /* max retries for claiming a slot */
#define CLOCK_CACHE_MIN_PARTITIONS          4    /* minimum number of partitions */
#define CLOCK_CACHE_MAX_PARTITIONS          512  /* maximum number of partitions */
#define CLOCK_CACHE_PARTITIONS_PER_CPU      4    /* partitions per CPU core */
#define CLOCK_CACHE_MIN_SLOTS_PER_PARTITION 64   /* minimum slots per partition */
#define CLOCK_CACHE_MAX_SLOTS_PER_PARTITION 8192 /* maximum slots per partition */
#define CLOCK_CACHE_AVG_ENTRY_SIZE          100  /* estimated average entry size in bytes */
/* hash index size = slots * 2 (2x, low load factor for fast probing) */
#define CLOCK_CACHE_HASH_INDEX_MULTIPLIER_NUM 2
#define CLOCK_CACHE_HASH_INDEX_MULTIPLIER_DEN 1
#define CLOCK_CACHE_MAX_HASH_PROBE            128 /* max linear probing distance */

/**
 * clock_cache_partition_t
 * single partition
 * uses hybrid design -- hash table for O(1) lookup + circular array for CLOCK eviction
 * @param slots circular array of slots for CLOCK
 * @param hash_index fixed-size hash index, hash --> slot_idx (-1 = empty)
 * @param num_slots current number of slots (immutable after init)
 * @param hash_index_size hash index size (2x num_slots for low collisions)
 * @param hash_mask mask for fast modulo (immutable)
 * @param slots_mask mask for fast modulo (num_slots - 1, power of 2)
 * @param evict_threshold precomputed -- num_slots * 85 / 100
 * @param clock_hand atomic CLOCK hand position
 * @param occupied_count atomic count of occupied slots
 * @param bytes_used atomic bytes used in this partition
 * @param hits per-partition hit counter (avoids false sharing on global counter)
 * @param misses per-partition miss counter (avoids false sharing on global counter)
 */
struct clock_cache_partition_t
{
    /* cache line 0 -- cold, read-only after init */
    clock_cache_entry_t *slots;
    _Atomic(int32_t) *hash_index;
    size_t num_slots;
    size_t hash_index_size;
    size_t hash_mask;
    size_t slots_mask;
    size_t evict_threshold;
    char _pad_cold[8]; /* pad to 64 bytes (keeps the hot atomics off this line) */

    /* cache line 1 -- eviction path (writers/evictors only) */
    atomic_size_t clock_hand;
    atomic_size_t occupied_count;
    atomic_size_t bytes_used;
    char _pad_evict[40]; /* pad to 64 bytes */

    /* cache line 2 -- read-path stats (readers only) */
    atomic_uint64_t hits;
    atomic_uint64_t misses;
    char _pad_stats[48]; /* pad to 64 bytes */
};

/**
 * clock_cache_t
 * main cache structure with partitions
 *
 * * PERFORMANCE NOTES *****
 * -- uses hybrid design -- hash table for O(1) lookup + circular array for CLOCK eviction
 * -- hash table provides O(1) average-case lookups (with chaining for collisions)
 * -- CLOCK array enables efficient second-chance eviction without reordering
 * -- for high-performance workloads
 *    -- use 128-512 partitions for 16+ threads to minimize lock contention
 *    -- hash table size auto-scales to next power-of-2 >= slots_per_partition
 * @param partitions array of partitions
 * @param num_partitions number of partitions
 * @param partition_mask mask for fast modulo (num_partitions - 1)
 * @param max_bytes maximum total bytes
 * @param total_bytes total bytes across all partitions
 * @param hits cache hits
 * @param misses cache misses
 * @param shutdown shutdown flag -- prevents new operations
 * @param evict_callback optional callback for custom cleanup on eviction (can be NULL)
 * @param num_groups number of L3/CCX groups (1 on monolithic dies, 4 on Threadripper)
 * @param partitions_per_group partitions per L3 group
 * @param local_partition_mask partitions_per_group - 1 for fast modulo
 * @param cpu_to_group CPU ID to L3 group mapping table
 * @param max_cpus size of cpu_to_group table
 */
struct clock_cache_t
{
    clock_cache_partition_t *partitions;
    size_t num_partitions;
    size_t partition_mask;
    size_t max_bytes;
    atomic_size_t total_bytes;
    atomic_uint64_t hits;
    atomic_uint64_t misses;
    _Atomic(uint8_t) shutdown;
    clock_cache_evict_fn evict_callback;
    size_t num_groups;
    size_t partitions_per_group;
    size_t local_partition_mask;
    uint8_t *cpu_to_group;
    int max_cpus;
};

/**
 * clock_cache_stats_t
 * cache statistics
 * @param total_entries total number of entries
 * @param total_bytes total bytes used
 * @param hits cache hits
 * @param misses cache misses
 * @param hit_rate hit rate (hits / (hits + misses))
 * @param num_partitions number of partitions
 */
typedef struct
{
    size_t total_entries;
    size_t total_bytes;
    uint64_t hits;
    uint64_t misses;
    double hit_rate;
    size_t num_partitions;
} clock_cache_stats_t;

/**
 * clock_cache_compute_config
 * compute optimal cache configuration based on max_bytes and CPU count
 * uses heuristics -- 1 partition per CPU core (up to 128), ~512 slots per partition
 * @param max_bytes maximum total bytes for cache
 * @param config output parameter for computed configuration
 */
void clock_cache_compute_config(size_t max_bytes, cache_config_t *config);

/**
 * clock_cache_create
 * create a new cache with specified configuration
 * @param config cache configuration
 * @return pointer to new cache or NULL on failure
 */
clock_cache_t *clock_cache_create(const cache_config_t *config);

/**
 * clock_cache_destroy
 * destroy the cache and free all resources
 * @param cache the cache to destroy
 */
void clock_cache_destroy(clock_cache_t *cache);

/**
 * clock_cache_put
 * insert or update a key-value pair
 * @param cache the cache
 * @param key the key
 * @param key_len the key length
 * @param payload the payload (can be any pointer type)
 * @param payload_len the payload length
 * @param external_bytes caller-declared memory cost of data pointed to by payload
 *                       (e.g., heap-allocated block data). included in bytes_used accounting
 *                       and eviction decisions. pass 0 if payload is self-contained.
 * @return 0 on success, -1 on failure
 */
int clock_cache_put(clock_cache_t *cache, const char *key, size_t key_len, const void *payload,
                    size_t payload_len, size_t external_bytes);

/**
 * clock_cache_put_new
 * insert a key-value pair that is known to not already exist in the cache
 * skips the existing-entry lookup (find_entry_with_hash) for better performance
 * on the cache-miss-then-populate path where we just confirmed the key is absent
 * @param cache the cache
 * @param key the key
 * @param key_len the key length
 * @param payload the payload
 * @param payload_len the payload length
 * @param external_bytes caller-declared memory cost of pointed-to data
 * @return 0 on success, -1 on failure
 */
int clock_cache_put_new(clock_cache_t *cache, const char *key, size_t key_len, const void *payload,
                        size_t payload_len, size_t external_bytes);

/**
 * clock_cache_get
 * retrieve a value by key (lock-free)
 * @param cache the cache
 * @param key the key
 * @param key_len the key length
 * @param payload_len output parameter for payload length
 * @return allocated payload copy (caller must free) or NULL if not found
 */
uint8_t *clock_cache_get(clock_cache_t *cache, const char *key, size_t key_len,
                         size_t *payload_len);

/**
 * clock_cache_get_zero_copy
 * retrieve a value by key without copying (zero-copy, lock-free)
 * caller must call clock_cache_release() when done to decrement ref_bit
 * @param cache the cache
 * @param key the key
 * @param key_len the key length
 * @param payload_len output parameter for payload length
 * @param entry_out output parameter for entry pointer (needed for release)
 * @return pointer to cached payload (**do not free**) or NULL if not found
 */
const uint8_t *clock_cache_get_zero_copy(clock_cache_t *cache, const char *key, size_t key_len,
                                         size_t *payload_len, clock_cache_entry_t **entry_out);

/**
 * clock_cache_release
 * release a zero-copy reference obtained from clock_cache_get_zero_copy
 * @param entry the entry pointer from clock_cache_get_zero_copy
 */
void clock_cache_release(clock_cache_entry_t *entry);

/**
 * clock_cache_delete
 * remove a key-value pair from cache
 * @param cache the cache
 * @param key the key
 * @param key_len the key length
 * @return 0 on success, -1 if not found
 */
int clock_cache_delete(clock_cache_t *cache, const char *key, size_t key_len);

/**
 * clock_cache_clear
 * remove all entries from cache
 * @param cache the cache
 */
void clock_cache_clear(clock_cache_t *cache);

/**
 * clock_cache_get_stats
 * get cache statistics
 * @param cache the cache
 * @param stats output parameter for statistics
 */
void clock_cache_get_stats(clock_cache_t *cache, clock_cache_stats_t *stats);

/**
 * clock_cache_foreach_callback_t
 * callback function for iterating over cache entries
 * @param key the entry key
 * @param key_len the key length
 * @param payload the entry payload
 * @param payload_len the payload length
 * @param user_data user data passed from caller
 * @return 0 to continue iteration, non-zero to stop
 */
typedef int (*clock_cache_foreach_callback_t)(const char *key, size_t key_len,
                                              const uint8_t *payload, size_t payload_len,
                                              void *user_data);

/**
 * clock_cache_foreach_prefix
 * iterate over all entries matching a key prefix
 * @param cache the cache
 * @param prefix the key prefix to match
 * @param prefix_len the prefix length
 * @param callback function to call for each matching entry (return 0 to continue, non-zero to stop)
 * @param user_data user data passed to callback
 * @return number of entries processed
 */
size_t clock_cache_foreach_prefix(clock_cache_t *cache, const char *prefix, size_t prefix_len,
                                  clock_cache_foreach_callback_t callback, void *user_data);

/**
 * clock_cache_delete_by_prefix
 * delete all entries matching a key prefix
 * unlike foreach_prefix + delete, this correctly releases reader refs before deletion
 * @param cache the cache
 * @param prefix the key prefix to match
 * @param prefix_len the prefix length
 * @return number of entries deleted
 */
size_t clock_cache_delete_by_prefix(clock_cache_t *cache, const char *prefix, size_t prefix_len);

#endif /* __CLOCK_CACHE_H__ */