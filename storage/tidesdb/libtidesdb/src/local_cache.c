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
#include "local_cache.h"

#include <string.h>
#include <sys/stat.h>

#include "xxhash.h"

#define TDB_LOCAL_CACHE_KLOG_EXT ".klog"
#define TDB_LOCAL_CACHE_VLOG_EXT ".vlog"
/* both partner extensions must be the same length for the swap-trick in
 * cache_evict_partner to produce a valid path */
#define TDB_LOCAL_CACHE_EXT_LEN (sizeof(TDB_LOCAL_CACHE_KLOG_EXT) - 1)

/**
 * cache_hash
 * XXH32 hash of a file path for bucket lookup
 * @param path file path to hash
 * @return hash value
 */
static uint32_t cache_hash(const char *path)
{
    return XXH32(path, strlen(path), 0);
}

/**
 * cache_bucket
 * return the bucket index for a hash value
 * @param h hash value
 * @return bucket index
 */
static inline uint32_t cache_bucket(uint32_t h)
{
    return h & (TDB_LOCAL_CACHE_HASH_BUCKETS - 1);
}

int tdb_local_cache_init(tdb_local_cache_t *cache, const char *cache_dir, size_t max_bytes)
{
    if (!cache || !cache_dir) return -1;

    memset(cache, 0, sizeof(*cache));
    snprintf(cache->cache_dir, sizeof(cache->cache_dir), "%s", cache_dir);
    cache->max_bytes = max_bytes;
    atomic_init(&cache->current_bytes, 0);
    pthread_mutex_init(&cache->lock, NULL);
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    atomic_init(&cache->num_entries, 0);
    memset(cache->buckets, 0, sizeof(cache->buckets));

    return 0;
}

void tdb_local_cache_destroy(tdb_local_cache_t *cache)
{
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);

    tdb_cache_entry_t *cur = cache->lru_head;
    while (cur)
    {
        tdb_cache_entry_t *next = cur->next;
        free(cur);
        cur = next;
    }
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    atomic_store(&cache->num_entries, 0);
    atomic_store(&cache->current_bytes, 0);
    memset(cache->buckets, 0, sizeof(cache->buckets));

    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
}

/**
 * lru_unlink
 * unlink an entry from the doubly-linked LRU list
 * @param cache the cache manager
 * @param entry entry to unlink (must be in the list)
 * caller must hold cache->lock
 */
static void lru_unlink(tdb_local_cache_t *cache, tdb_cache_entry_t *entry)
{
    if (entry->prev)
        entry->prev->next = entry->next;
    else
        cache->lru_head = entry->next;

    if (entry->next)
        entry->next->prev = entry->prev;
    else
        cache->lru_tail = entry->prev;

    entry->prev = NULL;
    entry->next = NULL;
}

/**
 * lru_push_head
 * insert an entry at the head (most recently used) of the LRU list
 * @param cache the cache manager
 * @param entry entry to insert
 * caller must hold cache->lock
 */
static void lru_push_head(tdb_local_cache_t *cache, tdb_cache_entry_t *entry)
{
    entry->prev = NULL;
    entry->next = cache->lru_head;
    if (cache->lru_head)
        cache->lru_head->prev = entry;
    else
        cache->lru_tail = entry;
    cache->lru_head = entry;
}

/**
 * hash_insert
 * insert an entry into the hash table
 * @param cache the cache manager
 * @param entry entry to insert
 * caller must hold cache->lock
 */
static void hash_insert(tdb_local_cache_t *cache, tdb_cache_entry_t *entry)
{
    uint32_t idx = cache_bucket(entry->hash);
    entry->hash_next = cache->buckets[idx];
    cache->buckets[idx] = entry;
}

/**
 * hash_remove
 * remove an entry from the hash table
 * @param cache the cache manager
 * @param entry entry to remove
 * caller must hold cache->lock
 */
static void hash_remove(tdb_local_cache_t *cache, tdb_cache_entry_t *entry)
{
    uint32_t idx = cache_bucket(entry->hash);
    tdb_cache_entry_t **pp = &cache->buckets[idx];
    while (*pp)
    {
        if (*pp == entry)
        {
            *pp = entry->hash_next;
            entry->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

/**
 * hash_find
 * find an entry by file path in the hash table (O(1) average)
 * @param cache the cache manager
 * @param path file path to search for
 * @param h precomputed hash of path
 * @return the entry if found, NULL otherwise
 * caller must hold cache->lock
 */
static tdb_cache_entry_t *hash_find(tdb_local_cache_t *cache, const char *path, uint32_t h)
{
    uint32_t idx = cache_bucket(h);
    tdb_cache_entry_t *cur = cache->buckets[idx];
    while (cur)
    {
        if (cur->hash == h && strcmp(cur->path, path) == 0) return cur;
        cur = cur->hash_next;
    }
    return NULL;
}

/**
 * cache_remove_entry
 * fully remove an entry from both hash table and LRU list, update accounting,
 * and optionally delete the file from disk
 * @param cache the cache manager
 * @param entry entry to remove
 * @param current pointer to running byte counter
 * @param delete_file 1 to unlink file from disk, 0 to just untrack
 * caller must hold cache->lock
 */
static void cache_remove_entry(tdb_local_cache_t *cache, tdb_cache_entry_t *entry, size_t *current,
                               int delete_file)
{
    lru_unlink(cache, entry);
    hash_remove(cache, entry);

    if (delete_file)
    {
        /* tdb_unlink clears the Windows read-only attribute that can otherwise block
         * deletion. surface a failure, a swallowed unlink error leaks the file on disk
         * while the byte counter below is decremented as if reclaimed. this leaf module has
         * no db log, so stderr is the available channel. the counter is still decremented
         * because the entry is being untracked regardless -- the leak is an OS-level issue
         * the operator must clear, not a tracker-accounting one. */
        if (tdb_unlink(entry->path) != 0)
        {
            fprintf(stderr, "tidesdb local_cache: failed to unlink %s; file leaked on disk\n",
                    entry->path);
        }
    }

    *current -= entry->size;
    atomic_store_explicit(&cache->current_bytes, *current, memory_order_relaxed);
    atomic_fetch_sub_explicit(&cache->num_entries, 1, memory_order_relaxed);
}

/**
 * cache_evict_partner
 * if the victim is a TDB_LOCAL_CACHE_KLOG_EXT or TDB_LOCAL_CACHE_VLOG_EXT file, find and evict its
 * partner so sstable file pairs are always evicted together
 * @param cache the cache manager
 * @param victim the entry being evicted
 * @param current pointer to the running byte counter
 * caller must hold cache->lock
 */
static void cache_evict_partner(tdb_local_cache_t *cache, const tdb_cache_entry_t *victim,
                                size_t *current)
{
    size_t vlen = strlen(victim->path);
    if (vlen < TDB_LOCAL_CACHE_EXT_LEN) return;

    const char *ext = victim->path + vlen - TDB_LOCAL_CACHE_EXT_LEN;
    const char *partner_ext = NULL;

    if (strcmp(ext, TDB_LOCAL_CACHE_KLOG_EXT) == 0)
        partner_ext = TDB_LOCAL_CACHE_VLOG_EXT;
    else if (strcmp(ext, TDB_LOCAL_CACHE_VLOG_EXT) == 0)
        partner_ext = TDB_LOCAL_CACHE_KLOG_EXT;

    if (!partner_ext) return;

    char partner_path[TDB_LOCAL_CACHE_MAX_PATH];
    memcpy(partner_path, victim->path, vlen - TDB_LOCAL_CACHE_EXT_LEN);
    memcpy(partner_path + vlen - TDB_LOCAL_CACHE_EXT_LEN, partner_ext, TDB_LOCAL_CACHE_EXT_LEN);
    partner_path[vlen] = '\0';

    uint32_t ph = cache_hash(partner_path);
    tdb_cache_entry_t *partner = hash_find(cache, partner_path, ph);
    if (!partner) return;

    cache_remove_entry(cache, partner, current, 1);
    free(partner);
}

/**
 * cache_evict
 * evict LRU entries (from tail) until enough space is available
 * @param cache the cache manager
 * @param bytes_needed number of bytes needed for the new entry
 * caller must hold cache->lock
 */
static void cache_evict(tdb_local_cache_t *cache, size_t bytes_needed)
{
    if (cache->max_bytes == 0) return; /* unlimited */

    size_t current = atomic_load_explicit(&cache->current_bytes, memory_order_relaxed);
    while (current + bytes_needed > cache->max_bytes && cache->lru_tail)
    {
        tdb_cache_entry_t *victim = cache->lru_tail;
        cache_remove_entry(cache, victim, &current, 1);

        /* we evict the klog/vlog partner so sstable pairs stay together */
        cache_evict_partner(cache, victim, &current);

        free(victim);
    }
}

int tdb_local_cache_track(tdb_local_cache_t *cache, const char *local_path)
{
    if (!cache || !local_path) return -1;

    struct stat st;
    if (stat(local_path, &st) != 0) return -1;

    size_t file_size = (size_t)st.st_size;
    uint32_t h = cache_hash(local_path);

    pthread_mutex_lock(&cache->lock);

    /* we check if already tracked via hash lookup (O(1)) */
    tdb_cache_entry_t *existing = hash_find(cache, local_path, h);
    if (existing)
    {
        /* we move to head (touch) */
        lru_unlink(cache, existing);
        lru_push_head(cache, existing);
        pthread_mutex_unlock(&cache->lock);
        return 0;
    }

    /* we evict if needed */
    cache_evict(cache, file_size);

    tdb_cache_entry_t *entry = calloc(1, sizeof(tdb_cache_entry_t));
    if (!entry)
    {
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }

    snprintf(entry->path, sizeof(entry->path), "%s", local_path);
    entry->size = file_size;
    entry->hash = h;
    lru_push_head(cache, entry);
    hash_insert(cache, entry);
    atomic_fetch_add_explicit(&cache->num_entries, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&cache->current_bytes, file_size, memory_order_relaxed);

    pthread_mutex_unlock(&cache->lock);
    return 0;
}

void tdb_local_cache_touch(tdb_local_cache_t *cache, const char *local_path)
{
    if (!cache || !local_path) return;

    uint32_t h = cache_hash(local_path);

    pthread_mutex_lock(&cache->lock);

    tdb_cache_entry_t *entry = hash_find(cache, local_path, h);
    if (entry)
    {
        lru_unlink(cache, entry);
        lru_push_head(cache, entry);
    }

    pthread_mutex_unlock(&cache->lock);
}

void tdb_local_cache_remove(tdb_local_cache_t *cache, const char *local_path)
{
    if (!cache || !local_path) return;

    uint32_t h = cache_hash(local_path);

    pthread_mutex_lock(&cache->lock);

    tdb_cache_entry_t *entry = hash_find(cache, local_path, h);
    if (entry)
    {
        size_t current = atomic_load_explicit(&cache->current_bytes, memory_order_relaxed);
        cache_remove_entry(cache, entry, &current, 0);
        free(entry);
    }

    pthread_mutex_unlock(&cache->lock);
}
