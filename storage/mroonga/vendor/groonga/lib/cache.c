/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "grn_cache.h"
#include "grn_ctx.h"
#include "grn_ctx_impl.h"
#include "grn_hash.h"
#include "grn_pat.h"
#include "grn_store.h"
#include "grn_db.h"
#include "grn_file_lock.h"

#include <sys/stat.h>

typedef struct _grn_cache_entry_memory grn_cache_entry_memory;

struct _grn_cache_entry_memory {
  grn_cache_entry_memory *next;
  grn_cache_entry_memory *prev;
  grn_obj *value;
  grn_timeval tv;
  grn_id id;
};

typedef struct _grn_cache_entry_persistent_data {
  grn_id next;
  grn_id prev;
  grn_timeval modified_time;
} grn_cache_entry_persistent_data;

/*
  sizeof(grn_cache_entry_persistent_metadata) should be equal or smaller
  than sizeof(grn_cache_entry_persistent_data).
 */
typedef struct _grn_cache_entry_persistent_metadata {
  uint32_t max_nentries;
  uint32_t nfetches;
  uint32_t nhits;
} grn_cache_entry_persistent_metadata;

typedef union _grn_cache_entry_persistent {
  grn_cache_entry_persistent_data data;
  grn_cache_entry_persistent_metadata metadata;
} grn_cache_entry_persistent;

struct _grn_cache {
  union {
    struct {
      grn_cache_entry_memory *next;
      grn_cache_entry_memory *prev;
      grn_hash *hash;
      grn_mutex mutex;
      uint32_t max_nentries;
      uint32_t nfetches;
      uint32_t nhits;
    } memory;
    struct {
      grn_hash *keys;
      grn_ja *values;
      int timeout;
    } persistent;
  } impl;
  grn_bool is_memory;
  grn_ctx *ctx;
};

#define GRN_CACHE_PERSISTENT_ROOT_ID 1
#define GRN_CACHE_PERSISTENT_ROOT_KEY "\0"
#define GRN_CACHE_PERSISTENT_ROOT_KEY_LEN \
  (sizeof(GRN_CACHE_PERSISTENT_ROOT_KEY) - 1)
#define GRN_CACHE_PERSISTENT_METADATA_ID 2
#define GRN_CACHE_PERSISTENT_METADATA_KEY "\1"
#define GRN_CACHE_PERSISTENT_METADATA_KEY_LEN \
  (sizeof(GRN_CACHE_PERSISTENT_METADATA_KEY) - 1)

static grn_ctx grn_cache_ctx;
static grn_cache *grn_cache_current = NULL;
static grn_cache *grn_cache_default = NULL;
static char grn_cache_default_base_path[PATH_MAX];

void
grn_set_default_cache_base_path(const char *base_path)
{
  if (base_path) {
    grn_strcpy(grn_cache_default_base_path,
               PATH_MAX,
               base_path);
  } else {
    grn_cache_default_base_path[0] = '\0';
  }
}

const char *
grn_get_default_cache_base_path(void)
{
  if (grn_cache_default_base_path[0] == '\0') {
    return NULL;
  } else {
    return grn_cache_default_base_path;
  }
}

static void
grn_cache_open_memory(grn_ctx *ctx, grn_cache *cache)
{
  cache->impl.memory.next = (grn_cache_entry_memory *)cache;
  cache->impl.memory.prev = (grn_cache_entry_memory *)cache;
  cache->impl.memory.hash = grn_hash_create(cache->ctx,
                                            NULL,
                                            GRN_CACHE_MAX_KEY_SIZE,
                                            sizeof(grn_cache_entry_memory),
                                            GRN_OBJ_KEY_VAR_SIZE);
  if (!cache->impl.memory.hash) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "[cache] failed to create hash table");
    return;
  }
  MUTEX_INIT(cache->impl.memory.mutex);

  cache->impl.memory.max_nentries = GRN_CACHE_DEFAULT_MAX_N_ENTRIES;
  cache->impl.memory.nfetches = 0;
  cache->impl.memory.nhits = 0;
}

static void
grn_cache_open_persistent(grn_ctx *ctx,
                          grn_cache *cache,
                          const char *base_path)
{
  grn_file_lock file_lock;
  char *keys_path = NULL;
  char *values_path = NULL;
  char lock_path_buffer[PATH_MAX];
  char keys_path_buffer[PATH_MAX];
  char values_path_buffer[PATH_MAX];

  cache->impl.persistent.timeout = 1000;

  if (base_path) {
    grn_snprintf(lock_path_buffer, PATH_MAX, PATH_MAX, "%s.lock", base_path);
    grn_file_lock_init(ctx, &file_lock, lock_path_buffer);
  } else {
    grn_file_lock_init(ctx, &file_lock, NULL);
  }

  if (base_path) {
    struct stat stat_buffer;

    grn_snprintf(keys_path_buffer, PATH_MAX, PATH_MAX, "%s.keys", base_path);
    grn_snprintf(values_path_buffer, PATH_MAX, PATH_MAX, "%s.values", base_path);
    keys_path = keys_path_buffer;
    values_path = values_path_buffer;

    if (!grn_file_lock_acquire(ctx,
                               &file_lock,
                               cache->impl.persistent.timeout,
                               "[cache][persistent][open]")) {
      goto exit;
    }

    if (stat(keys_path, &stat_buffer) == 0) {
      cache->impl.persistent.keys = grn_hash_open(ctx, keys_path);
      if (cache->impl.persistent.keys) {
        cache->impl.persistent.values = grn_ja_open(ctx, values_path);
      }
    }
    if (!cache->impl.persistent.keys) {
      if (cache->impl.persistent.values) {
        grn_ja_close(ctx, cache->impl.persistent.values);
        cache->impl.persistent.values = NULL;
      }
      if (stat(keys_path, &stat_buffer) == 0) {
        if (grn_hash_remove(ctx, keys_path) != GRN_SUCCESS) {
          ERRNO_ERR("[cache][persistent] "
                    "failed to remove path for cache keys: <%s>",
                    keys_path);
          goto exit;
        }
      }
      if (stat(values_path, &stat_buffer) == 0) {
        if (grn_ja_remove(ctx, values_path) != GRN_SUCCESS) {
          ERRNO_ERR("[cache][persistent] "
                    "failed to remove path for cache values: <%s>",
                    values_path);
          goto exit;
        }
      }
    }
  }

  if (!cache->impl.persistent.keys) {
    cache->impl.persistent.keys =
      grn_hash_create(ctx,
                      keys_path,
                      GRN_CACHE_MAX_KEY_SIZE,
                      sizeof(grn_cache_entry_persistent),
                      GRN_OBJ_KEY_VAR_SIZE);
    if (!cache->impl.persistent.keys) {
      ERR(ctx->rc == GRN_SUCCESS ? GRN_FILE_CORRUPT : ctx->rc,
          "[cache][persistent] failed to create cache keys storage: <%s>",
          keys_path ? keys_path : "(memory)");
      goto exit;
    }
    cache->impl.persistent.values =
      grn_ja_create(ctx,
                    values_path,
                    1 << 16,
                    0);
    if (!cache->impl.persistent.values) {
      grn_hash_close(ctx, cache->impl.persistent.keys);
      ERR(ctx->rc == GRN_SUCCESS ? GRN_FILE_CORRUPT : ctx->rc,
          "[cache][persistent] failed to create cache values storage: <%s>",
          values_path ? values_path : "(memory)");
      goto exit;
    }
  }

  {
    grn_cache_entry_persistent *entry;
    grn_id root_id;
    int added;

    root_id = grn_hash_add(ctx,
                           cache->impl.persistent.keys,
                           GRN_CACHE_PERSISTENT_ROOT_KEY,
                           GRN_CACHE_PERSISTENT_ROOT_KEY_LEN,
                           (void **)&entry,
                           &added);
    if (root_id != GRN_CACHE_PERSISTENT_ROOT_ID) {
      grn_ja_close(ctx, cache->impl.persistent.values);
      grn_hash_close(ctx, cache->impl.persistent.keys);
      if (values_path) {
        grn_ja_remove(ctx, values_path);
      }
      if (keys_path) {
        grn_hash_remove(ctx, keys_path);
      }
      ERR(ctx->rc == GRN_SUCCESS ? GRN_FILE_CORRUPT : ctx->rc,
          "[cache][persistent] broken cache keys storage: broken root: <%s>",
          keys_path ? keys_path : "(memory)");
      return;
    }

    if (added) {
      entry->data.next = root_id;
      entry->data.prev = root_id;
      entry->data.modified_time.tv_sec = 0;
      entry->data.modified_time.tv_nsec = 0;
    }
  }

  {
    grn_cache_entry_persistent *entry;
    grn_id metadata_id;
    int added;

    metadata_id = grn_hash_add(ctx,
                               cache->impl.persistent.keys,
                               GRN_CACHE_PERSISTENT_METADATA_KEY,
                               GRN_CACHE_PERSISTENT_METADATA_KEY_LEN,
                               (void **)&entry,
                               &added);
    if (metadata_id != GRN_CACHE_PERSISTENT_METADATA_ID) {
      grn_ja_close(ctx, cache->impl.persistent.values);
      grn_hash_close(ctx, cache->impl.persistent.keys);
      if (values_path) {
        grn_ja_remove(ctx, values_path);
      }
      if (keys_path) {
        grn_hash_remove(ctx, keys_path);
      }
      ERR(ctx->rc == GRN_SUCCESS ? GRN_FILE_CORRUPT : ctx->rc,
          "[cache][persistent] broken cache keys storage: broken metadata: <%s>",
          keys_path ? keys_path : "(memory)");
      goto exit;
    }

    if (added) {
      entry->metadata.max_nentries = GRN_CACHE_DEFAULT_MAX_N_ENTRIES;
      entry->metadata.nfetches = 0;
      entry->metadata.nhits = 0;
    }
  }

exit :
  grn_file_lock_release(ctx, &file_lock);
  grn_file_lock_fin(ctx, &file_lock);
}

static grn_cache *
grn_cache_open_raw(grn_ctx *ctx,
                   grn_bool is_memory,
                   const char *base_path)
{
  grn_cache *cache = NULL;

  GRN_API_ENTER;
  cache = GRN_CALLOC(sizeof(grn_cache));
  if (!cache) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "[cache] failed to allocate grn_cache");
    goto exit;
  }

  cache->ctx = ctx;
  cache->is_memory = is_memory;
  if (cache->is_memory) {
    grn_cache_open_memory(ctx, cache);
  } else {
    grn_cache_open_persistent(ctx, cache, base_path);
  }
  if (ctx->rc != GRN_SUCCESS) {
    GRN_FREE(cache);
    cache = NULL;
    goto exit;
  }

exit :
  GRN_API_RETURN(cache);
}

grn_cache *
grn_cache_open(grn_ctx *ctx)
{
  const char *base_path = NULL;
  grn_bool is_memory;

  if (grn_cache_default_base_path[0] != '\0') {
    base_path = grn_cache_default_base_path;
  }

  if (base_path) {
    is_memory = GRN_FALSE;
  } else {
    char grn_cache_type_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_CACHE_TYPE", grn_cache_type_env, GRN_ENV_BUFFER_SIZE);
    if (strcmp(grn_cache_type_env, "persistent") == 0) {
      is_memory = GRN_FALSE;
    } else {
      is_memory = GRN_TRUE;
    }
  }

  return grn_cache_open_raw(ctx, is_memory, base_path);
}

grn_cache *
grn_persistent_cache_open(grn_ctx *ctx, const char *base_path)
{
  grn_bool is_memory = GRN_FALSE;
  return grn_cache_open_raw(ctx, is_memory, base_path);
}


static void
grn_cache_close_memory(grn_ctx *ctx, grn_cache *cache)
{
  grn_cache_entry_memory *vp;

  GRN_HASH_EACH(ctx, cache->impl.memory.hash, id, NULL, NULL, &vp, {
    grn_obj_close(ctx, vp->value);
  });
  grn_hash_close(ctx, cache->impl.memory.hash);
  MUTEX_FIN(cache->impl.memory.mutex);
}

static void
grn_cache_close_persistent(grn_ctx *ctx, grn_cache *cache)
{
  grn_hash_close(ctx, cache->impl.persistent.keys);
  grn_ja_close(ctx, cache->impl.persistent.values);
}

grn_rc
grn_cache_close(grn_ctx *ctx_not_used, grn_cache *cache)
{
  grn_ctx *ctx = cache->ctx;

  GRN_API_ENTER;

  if (cache->is_memory) {
    grn_cache_close_memory(ctx, cache);
  } else {
    grn_cache_close_persistent(ctx, cache);
  }
  GRN_FREE(cache);

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_cache_current_set(grn_ctx *ctx, grn_cache *cache)
{
  grn_cache_current = cache;
  return GRN_SUCCESS;
}

grn_cache *
grn_cache_current_get(grn_ctx *ctx)
{
  return grn_cache_current;
}

void
grn_cache_init(void)
{
  grn_ctx *ctx = &grn_cache_ctx;

  grn_ctx_init(ctx, 0);

  grn_cache_default = grn_cache_open(ctx);
  grn_cache_current_set(ctx, grn_cache_default);
}

grn_rc
grn_cache_default_reopen(void)
{
  grn_ctx *ctx = &grn_cache_ctx;
  grn_cache *new_default;
  grn_bool default_is_current;

  GRN_API_ENTER;

  new_default = grn_cache_open(ctx);
  if (!new_default) {
    GRN_API_RETURN(ctx->rc);
  }

  default_is_current = (grn_cache_default == grn_cache_current_get(ctx));
  if (default_is_current) {
    grn_cache_current_set(ctx, new_default);
  }

  if (grn_cache_default) {
    grn_cache_close(ctx, grn_cache_default);
  }
  grn_cache_default = new_default;

  GRN_API_RETURN(ctx->rc);
}

static void
grn_cache_expire_entry_memory(grn_cache *cache, grn_cache_entry_memory *ce)
{
  ce->prev->next = ce->next;
  ce->next->prev = ce->prev;
  grn_obj_close(cache->ctx, ce->value);
  grn_hash_delete_by_id(cache->ctx, cache->impl.memory.hash, ce->id, NULL);
}

static void
grn_cache_entry_persistent_delete_link(grn_cache *cache,
                                       grn_cache_entry_persistent *entry)
{
  grn_ctx *ctx = cache->ctx;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *prev_entry;
  grn_cache_entry_persistent *next_entry;

  prev_entry =
    (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                      keys,
                                                      entry->data.prev,
                                                      NULL);
  next_entry =
    (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                      keys,
                                                      entry->data.next,
                                                      NULL);
  prev_entry->data.next = entry->data.next;
  next_entry->data.prev = entry->data.prev;
}

static void
grn_cache_entry_persistent_prepend_link(grn_cache *cache,
                                        grn_cache_entry_persistent *entry,
                                        grn_id entry_id,
                                        grn_cache_entry_persistent *head_entry,
                                        grn_id head_entry_id)
{
  grn_ctx *ctx = cache->ctx;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *head_next_entry;

  entry->data.next = head_entry->data.next;
  entry->data.prev = head_entry_id;
  head_next_entry =
    (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                      keys,
                                                      head_entry->data.next,
                                                      NULL);
  head_next_entry->data.prev = entry_id;
  head_entry->data.next = entry_id;
}

static void
grn_cache_expire_entry_persistent(grn_cache *cache,
                                  grn_cache_entry_persistent *entry,
                                  grn_id cache_id)
{
  grn_hash *keys = cache->impl.persistent.keys;
  grn_ja *values = cache->impl.persistent.values;

  grn_cache_entry_persistent_delete_link(cache, entry);
  grn_ja_put(cache->ctx, values, cache_id, NULL, 0, GRN_OBJ_SET, NULL);
  grn_hash_delete_by_id(cache->ctx, keys, cache_id, NULL);
}

static void
grn_cache_expire_memory_without_lock(grn_cache *cache, int32_t size)
{
  grn_cache_entry_memory *ce0 =
    (grn_cache_entry_memory *)(&(cache->impl.memory));
  while (ce0 != ce0->prev && size--) {
    grn_cache_expire_entry_memory(cache, ce0->prev);
  }
}

static void
grn_cache_expire_persistent_without_lock(grn_cache *cache, int32_t size)
{
  grn_ctx *ctx = cache->ctx;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *head_entry;

  head_entry =
    (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                      keys,
                                                      GRN_CACHE_PERSISTENT_ROOT_ID,
                                                      NULL);
  while (head_entry->data.prev != GRN_CACHE_PERSISTENT_ROOT_ID &&
         size > 0) {
    grn_cache_entry_persistent *tail_entry;
    tail_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        head_entry->data.prev,
                                                        NULL);
    grn_cache_expire_entry_persistent(cache, tail_entry, head_entry->data.prev);
    size--;
  }
}

static grn_rc
grn_cache_set_max_n_entries_memory(grn_ctx *ctx,
                                   grn_cache *cache,
                                   unsigned int n)
{
  uint32_t current_max_n_entries;

  MUTEX_LOCK(cache->impl.memory.mutex);
  current_max_n_entries = cache->impl.memory.max_nentries;
  cache->impl.memory.max_nentries = n;
  if (n < current_max_n_entries) {
    grn_cache_expire_memory_without_lock(cache, current_max_n_entries - n);
  }
  MUTEX_UNLOCK(cache->impl.memory.mutex);

  return GRN_SUCCESS;
}

static grn_rc
grn_cache_set_max_n_entries_persistent(grn_ctx *ctx,
                                       grn_cache *cache,
                                       unsigned int n)
{
  grn_rc rc;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *metadata_entry;
  uint32_t current_max_n_entries;

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  metadata_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        GRN_CACHE_PERSISTENT_METADATA_ID,
                                                        NULL);

  current_max_n_entries = metadata_entry->metadata.max_nentries;
  metadata_entry->metadata.max_nentries = n;
  if (n < current_max_n_entries) {
    grn_cache_expire_persistent_without_lock(cache, current_max_n_entries - n);
  }
  grn_io_unlock(keys->io);

  return GRN_SUCCESS;
}

grn_rc
grn_cache_set_max_n_entries(grn_ctx *ctx, grn_cache *cache, unsigned int n)
{
  if (!cache) {
    return GRN_INVALID_ARGUMENT;
  }

  if (cache->is_memory) {
    return grn_cache_set_max_n_entries_memory(cache->ctx, cache, n);
  } else {
    return grn_cache_set_max_n_entries_persistent(cache->ctx, cache, n);
  }
}

static uint32_t
grn_cache_get_max_n_entries_memory(grn_ctx *ctx, grn_cache *cache)
{
  return cache->impl.memory.max_nentries;
}

static uint32_t
grn_cache_get_max_n_entries_persistent(grn_ctx *ctx, grn_cache *cache)
{
  grn_rc rc;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *metadata_entry;
  uint32_t current_max_n_entries;

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return 0;
  }

  metadata_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        GRN_CACHE_PERSISTENT_METADATA_ID,
                                                        NULL);
  current_max_n_entries = metadata_entry->metadata.max_nentries;
  grn_io_unlock(keys->io);

  return current_max_n_entries;
}

uint32_t
grn_cache_get_max_n_entries(grn_ctx *ctx, grn_cache *cache)
{
  if (!cache) {
    return 0;
  }

  if (cache->is_memory) {
    return grn_cache_get_max_n_entries_memory(cache->ctx, cache);
  } else {
    return grn_cache_get_max_n_entries_persistent(cache->ctx, cache);
  }
}

static void
grn_cache_get_statistics_memory(grn_ctx *ctx, grn_cache *cache,
                                grn_cache_statistics *statistics)
{
  MUTEX_LOCK(cache->impl.memory.mutex);
  statistics->nentries = GRN_HASH_SIZE(cache->impl.memory.hash);
  statistics->max_nentries = cache->impl.memory.max_nentries;
  statistics->nfetches = cache->impl.memory.nfetches;
  statistics->nhits = cache->impl.memory.nhits;
  MUTEX_UNLOCK(cache->impl.memory.mutex);
}

static void
grn_cache_get_statistics_persistent(grn_ctx *ctx, grn_cache *cache,
                                    grn_cache_statistics *statistics)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_cache_entry_persistent *metadata_entry;

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return;
  }

  metadata_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        GRN_CACHE_PERSISTENT_METADATA_ID,
                                                        NULL);

  statistics->nentries = GRN_HASH_SIZE(keys);
  statistics->max_nentries = metadata_entry->metadata.max_nentries;
  statistics->nfetches = metadata_entry->metadata.nfetches;
  statistics->nhits = metadata_entry->metadata.nhits;

  grn_io_unlock(keys->io);
}

void
grn_cache_get_statistics(grn_ctx *ctx, grn_cache *cache,
                         grn_cache_statistics *statistics)
{
  if (cache->is_memory) {
    return grn_cache_get_statistics_memory(ctx, cache, statistics);
  } else {
    return grn_cache_get_statistics_persistent(ctx, cache, statistics);
  }
}

static grn_rc
grn_cache_fetch_memory(grn_ctx *ctx, grn_cache *cache,
                       const char *key, uint32_t key_len,
                       grn_obj *output)
{
  /* TODO: How about GRN_NOT_FOUND? */
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_cache_entry_memory *ce;

  MUTEX_LOCK(cache->impl.memory.mutex);
  cache->impl.memory.nfetches++;
  if (grn_hash_get(cache->ctx, cache->impl.memory.hash, key, key_len,
                   (void **)&ce)) {
    if (ce->tv.tv_sec <= grn_db_get_last_modified(ctx, ctx->impl->db)) {
      grn_cache_expire_entry_memory(cache, ce);
      goto exit;
    }
    rc = GRN_SUCCESS;
    GRN_TEXT_PUT(ctx,
                 output,
                 GRN_TEXT_VALUE(ce->value),
                 GRN_TEXT_LEN(ce->value));
    ce->prev->next = ce->next;
    ce->next->prev = ce->prev;
    {
      grn_cache_entry_memory *ce0 =
        (grn_cache_entry_memory *)(&(cache->impl.memory));
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
    cache->impl.memory.nhits++;
  }
exit :
  MUTEX_UNLOCK(cache->impl.memory.mutex);
  return rc;
}

static grn_rc
grn_cache_fetch_persistent(grn_ctx *ctx, grn_cache *cache,
                           const char *key, uint32_t key_len,
                           grn_obj *output)
{
  /* TODO: How about GRN_NOT_FOUND? */
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_ja *values = cache->impl.persistent.values;
  grn_id cache_id;
  grn_cache_entry_persistent *entry;
  grn_cache_entry_persistent *metadata_entry;

  if (key_len == GRN_CACHE_PERSISTENT_ROOT_KEY_LEN &&
      memcmp(key,
             GRN_CACHE_PERSISTENT_ROOT_KEY,
             GRN_CACHE_PERSISTENT_ROOT_KEY_LEN) == 0) {
    return rc;
  }

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  /* TODO: How about GRN_NOT_FOUND? */
  rc = GRN_INVALID_ARGUMENT;

  metadata_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        GRN_CACHE_PERSISTENT_METADATA_ID,
                                                        NULL);
  metadata_entry->metadata.nfetches++;

  cache_id = grn_hash_get(cache->ctx, keys, key, key_len, (void **)&entry);
  if (cache_id == GRN_ID_NIL) {
    goto exit;
  }

  if (cache_id != GRN_ID_NIL) {
    if (entry->data.modified_time.tv_sec <=
        grn_db_get_last_modified(ctx, ctx->impl->db)) {
      grn_cache_expire_entry_persistent(cache, entry, cache_id);
      goto exit;
    }

    rc = GRN_SUCCESS;
    grn_ja_get_value(ctx, values, cache_id, output);
    grn_cache_entry_persistent_delete_link(cache, entry);
    {
      grn_cache_entry_persistent *head_entry;
      head_entry =
        (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                          keys,
                                                          GRN_CACHE_PERSISTENT_ROOT_ID,
                                                          NULL);
      grn_cache_entry_persistent_prepend_link(cache,
                                              entry,
                                              cache_id,
                                              head_entry,
                                              GRN_CACHE_PERSISTENT_ROOT_ID);
    }
    metadata_entry->metadata.nhits++;
  }

exit :
  grn_io_unlock(keys->io);

  return rc;
}

grn_rc
grn_cache_fetch(grn_ctx *ctx, grn_cache *cache,
                const char *key, uint32_t key_len,
                grn_obj *output)
{
  if (!ctx->impl || !ctx->impl->db) { return GRN_INVALID_ARGUMENT; }

  if (cache->is_memory) {
    return grn_cache_fetch_memory(ctx, cache, key, key_len, output);
  } else {
    return grn_cache_fetch_persistent(ctx, cache, key, key_len, output);
  }
}

static void
grn_cache_update_memory(grn_ctx *ctx, grn_cache *cache,
                        const char *key, uint32_t key_len,
                        grn_obj *value)
{
  grn_id id;
  int added = 0;
  grn_cache_entry_memory *ce;
  grn_rc rc = GRN_SUCCESS;
  grn_obj *old = NULL;
  grn_obj *obj = NULL;

  if (cache->impl.memory.max_nentries == 0) {
    return;
  }

  MUTEX_LOCK(cache->impl.memory.mutex);
  obj = grn_obj_open(cache->ctx, GRN_BULK, 0, GRN_DB_TEXT);
  if (!obj) {
    goto exit;
  }
  GRN_TEXT_PUT(cache->ctx, obj, GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
  id = grn_hash_add(cache->ctx, cache->impl.memory.hash, key, key_len,
                    (void **)&ce, &added);
  if (id) {
    if (!added) {
      old = ce->value;
      ce->prev->next = ce->next;
      ce->next->prev = ce->prev;
    }
    ce->id = id;
    ce->value = obj;
    ce->tv = ctx->impl->tv;
    {
      grn_cache_entry_memory *ce0 =
        (grn_cache_entry_memory *)(&(cache->impl.memory));
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
    if (GRN_HASH_SIZE(cache->impl.memory.hash) >
        cache->impl.memory.max_nentries) {
      grn_cache_expire_entry_memory(cache, cache->impl.memory.prev);
    }
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
exit :
  if (rc) { grn_obj_close(cache->ctx, obj); }
  if (old) { grn_obj_close(cache->ctx, old); }
  MUTEX_UNLOCK(cache->impl.memory.mutex);
}

static void
grn_cache_update_persistent(grn_ctx *ctx, grn_cache *cache,
                            const char *key, uint32_t key_len,
                            grn_obj *value)
{
  grn_rc rc;
  grn_hash *keys = cache->impl.persistent.keys;
  grn_ja *values = cache->impl.persistent.values;
  grn_cache_entry_persistent *metadata_entry;
  grn_id cache_id;
  grn_cache_entry_persistent *entry;
  int added;

  if (key_len == GRN_CACHE_PERSISTENT_ROOT_KEY_LEN &&
      memcmp(key,
             GRN_CACHE_PERSISTENT_ROOT_KEY,
             GRN_CACHE_PERSISTENT_ROOT_KEY_LEN) == 0) {
    return;
  }

  if (key_len == GRN_CACHE_PERSISTENT_METADATA_KEY_LEN &&
      memcmp(key,
             GRN_CACHE_PERSISTENT_METADATA_KEY,
             GRN_CACHE_PERSISTENT_METADATA_KEY_LEN) == 0) {
    return;
  }

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return;
  }

  metadata_entry =
    (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                      keys,
                                                      GRN_CACHE_PERSISTENT_METADATA_ID,
                                                      NULL);
  if (metadata_entry->metadata.max_nentries == 0) {
    goto exit;
  }

  cache_id = grn_hash_add(cache->ctx, keys, key, key_len, (void **)&entry,
                          &added);
  if (cache_id) {
    grn_cache_entry_persistent *head_entry;

    if (!added) {
      grn_cache_entry_persistent_delete_link(cache, entry);
    }
    entry->data.modified_time = ctx->impl->tv;

    grn_ja_put(cache->ctx, values, cache_id,
               GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value),
               GRN_OBJ_SET, NULL);

    head_entry =
      (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                        keys,
                                                        GRN_CACHE_PERSISTENT_ROOT_ID,
                                                        NULL);
    grn_cache_entry_persistent_prepend_link(cache,
                                            entry,
                                            cache_id,
                                            head_entry,
                                            GRN_CACHE_PERSISTENT_ROOT_ID);
    if (GRN_HASH_SIZE(keys) > metadata_entry->metadata.max_nentries) {
      grn_cache_entry_persistent *tail_entry;
      tail_entry =
        (grn_cache_entry_persistent *)grn_hash_get_value_(ctx,
                                                          keys,
                                                          head_entry->data.prev,
                                                          NULL);
      grn_cache_expire_entry_persistent(cache,
                                        tail_entry,
                                        head_entry->data.prev);
    }
  }

exit :
  grn_io_unlock(keys->io);
}

void
grn_cache_update(grn_ctx *ctx, grn_cache *cache,
                 const char *key, uint32_t key_len, grn_obj *value)
{
  if (!ctx->impl) { return; }

  if (cache->is_memory) {
    grn_cache_update_memory(ctx, cache, key, key_len, value);
  } else {
    grn_cache_update_persistent(ctx, cache, key, key_len, value);
  }
}

static void
grn_cache_expire_memory(grn_cache *cache, int32_t size)
{
  MUTEX_LOCK(cache->impl.memory.mutex);
  grn_cache_expire_memory_without_lock(cache, size);
  MUTEX_UNLOCK(cache->impl.memory.mutex);
}

static void
grn_cache_expire_persistent(grn_cache *cache, int32_t size)
{
  grn_rc rc;
  grn_ctx *ctx = cache->ctx;
  grn_hash *keys = cache->impl.persistent.keys;

  rc = grn_io_lock(ctx, keys->io, cache->impl.persistent.timeout);
  if (rc != GRN_SUCCESS) {
    return;
  }

  grn_cache_expire_persistent_without_lock(cache, size);

  grn_io_unlock(keys->io);
}

void
grn_cache_expire(grn_cache *cache, int32_t size)
{
  if (cache->is_memory) {
    grn_cache_expire_memory(cache, size);
  } else {
    grn_cache_expire_persistent(cache, size);
  }
}

void
grn_cache_fin(void)
{
  grn_ctx *ctx = &grn_cache_ctx;

  grn_cache_current_set(ctx, NULL);

  if (grn_cache_default) {
    grn_cache_close(ctx, grn_cache_default);
    grn_cache_default = NULL;
  }

  grn_ctx_fin(ctx);
}
