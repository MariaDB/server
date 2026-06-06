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
#ifndef __LOCAL_CACHE_H__
#define __LOCAL_CACHE_H__

#include "compat.h"

#define TDB_LOCAL_CACHE_MAX_PATH     4096
#define TDB_LOCAL_CACHE_HASH_BUCKETS 256 /* power of 2 for bitmask lookup */

/**
 * tdb_cache_entry_t
 * doubly-linked LRU list node tracking a cached file, also chained in a hash bucket
 * @param path file path of the cached file
 * @param size size of the cached file in bytes
 * @param prev pointer to the previous entry in the LRU list
 * @param next pointer to the next entry in the LRU list
 * @param hash_next pointer to the next entry in the same hash bucket
 * @param hash value of the path hash (cached to avoid recomputation on remove)
 */
typedef struct tdb_cache_entry
{
    char path[TDB_LOCAL_CACHE_MAX_PATH];
    size_t size;
    struct tdb_cache_entry *prev;
    struct tdb_cache_entry *next;
    struct tdb_cache_entry *hash_next;
    uint32_t hash;
} tdb_cache_entry_t;

/**
 * tdb_local_cache_t
 * local file cache manager with hash-indexed LRU eviction for object store mode.
 * tracks which sstable files are cached locally and evicts cold files
 * when the cache exceeds max_bytes. uses a hash table for O(1) lookups
 * and a doubly-linked LRU list for eviction ordering.
 * @param cache_dir directory path for cached files
 * @param max_bytes maximum cache size in bytes (0 = unlimited)
 * @param current_bytes atomic counter of current cache size in bytes
 * @param lock mutex protecting the LRU list and hash table
 * @param lru_head pointer to the most recently used entry
 * @param lru_tail pointer to the least recently used entry (eviction candidate)
 * @param num_entries atomic counter of entries currently in the cache
 * @param buckets hash table buckets for O(1) path lookups
 */
typedef struct
{
    char cache_dir[TDB_LOCAL_CACHE_MAX_PATH];
    size_t max_bytes; /* 0 = unlimited */
    _Atomic(size_t) current_bytes;
    pthread_mutex_t lock;
    tdb_cache_entry_t *lru_head;
    tdb_cache_entry_t *lru_tail;
    _Atomic(int) num_entries;
    tdb_cache_entry_t *buckets[TDB_LOCAL_CACHE_HASH_BUCKETS];
} tdb_local_cache_t;

/**
 * tdb_local_cache_init
 * initialize the local file cache manager
 * @param cache     cache struct to initialize
 * @param cache_dir local directory for cached files
 * @param max_bytes maximum cache size in bytes (0 = unlimited)
 * @return 0 on success, -1 on error
 */
int tdb_local_cache_init(tdb_local_cache_t *cache, const char *cache_dir, size_t max_bytes);

/**
 * tdb_local_cache_destroy
 * free all tracking entries and destroy mutex.
 * does not delete cached files from disk (they persist for next startup).
 * @param cache cache to destroy
 */
void tdb_local_cache_destroy(tdb_local_cache_t *cache);

/**
 * tdb_local_cache_track
 * register a file in the cache. stats the file for size, adds to LRU head,
 * and triggers eviction if the cache is over its size limit. if the file is
 * already tracked it is simply moved to the LRU head (no duplicate entry and
 * no eviction) and 0 is returned, so re-tracking is safe and acts as a touch.
 * @param cache      cache manager
 * @param local_path path to the cached file
 * @return 0 on success, -1 on error
 */
int tdb_local_cache_track(tdb_local_cache_t *cache, const char *local_path);

/**
 * tdb_local_cache_touch
 * move an existing cached file to the head of the LRU list (mark as recently used).
 * no-op if the file is not tracked.
 * @param cache      cache manager
 * @param local_path path to the cached file
 */
void tdb_local_cache_touch(tdb_local_cache_t *cache, const char *local_path);

/**
 * tdb_local_cache_remove
 * remove a file from cache tracking. does not delete the file from disk.
 * @param cache      cache manager
 * @param local_path path to remove
 */
void tdb_local_cache_remove(tdb_local_cache_t *cache, const char *local_path);

#endif /* __LOCAL_CACHE_H__ */
