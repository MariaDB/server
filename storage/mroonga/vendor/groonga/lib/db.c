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
#include "grn.h"
#include "grn_config.h"
#include "grn_db.h"
#include "grn_obj.h"
#include "grn_hash.h"
#include "grn_pat.h"
#include "grn_dat.h"
#include "grn_ii.h"
#include "grn_index_column.h"
#include "grn_ctx_impl.h"
#include "grn_token_cursor.h"
#include "grn_tokenizers.h"
#include "grn_proc.h"
#include "grn_plugin.h"
#include "grn_geo.h"
#include "grn_scorers.h"
#include "grn_snip.h"
#include "grn_string.h"
#include "grn_normalizer.h"
#include "grn_report.h"
#include "grn_util.h"
#include "grn_cache.h"
#include "grn_window_functions.h"
#include <string.h>
#include <math.h>

typedef struct {
  grn_id id;
  unsigned int weight;
} weight_uvector_entry;

#define IS_WEIGHT_UVECTOR(obj) ((obj)->header.flags & GRN_OBJ_WITH_WEIGHT)

#define GRN_TABLE_GROUPED (0x01<<0)
#define GRN_TABLE_IS_GROUPED(table)\
  ((table)->header.impl_flags & GRN_TABLE_GROUPED)
#define GRN_TABLE_GROUPED_ON(table)\
  ((table)->header.impl_flags |= GRN_TABLE_GROUPED)
#define GRN_TABLE_IS_MULTI_KEYS_GROUPED(table)\
  (GRN_TABLE_IS_GROUPED(table) &&\
   table->header.domain == GRN_ID_NIL)

#define WITH_NORMALIZE(table,key,key_size,block) do {\
  if ((table)->normalizer && key && key_size > 0) {\
    grn_obj *nstr;\
    if ((nstr = grn_string_open(ctx, key, key_size,\
                                (table)->normalizer, 0))) {\
      const char *key;\
      unsigned int key_size;\
      grn_string_get_normalized(ctx, nstr, &key, &key_size, NULL);\
      block\
      grn_obj_close(ctx, nstr);\
    }\
  } else {\
    block\
  }\
} while (0)

inline static grn_id
grn_table_add_v_inline(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                       void **value, int *added);
inline static void
grn_table_add_subrec_inline(grn_obj *table, grn_rset_recinfo *ri, double score,
                            grn_rset_posinfo *pi, int dir);
inline static grn_id
grn_table_cursor_next_inline(grn_ctx *ctx, grn_table_cursor *tc);
inline static int
grn_table_cursor_get_value_inline(grn_ctx *ctx, grn_table_cursor *tc, void **value);

static void grn_obj_ensure_bulk(grn_ctx *ctx, grn_obj *obj);
static void grn_obj_ensure_vector(grn_ctx *ctx, grn_obj *obj);

inline static void
grn_obj_get_range_info(grn_ctx *ctx, grn_obj *obj,
                       grn_id *range_id, grn_obj_flags *range_flags);

static char grn_db_key[GRN_ENV_BUFFER_SIZE];

void
grn_db_init_from_env(void)
{
  grn_getenv("GRN_DB_KEY",
             grn_db_key,
             GRN_ENV_BUFFER_SIZE);
}

inline static void
gen_pathname(const char *path, char *buffer, int fno)
{
  size_t len = strlen(path);
  grn_memcpy(buffer, path, len);
  if (fno >= 0) {
    buffer[len] = '.';
    grn_itoh(fno, buffer + len + 1, 7);
    buffer[len + 8] = '\0';
  } else {
    buffer[len] = '\0';
  }
}

void
grn_db_generate_pathname(grn_ctx *ctx, grn_obj *db, grn_id id, char *buffer)
{
  gen_pathname(grn_obj_get_io(ctx, db)->path, buffer, id);
}

typedef struct {
  grn_obj *ptr;
  uint32_t lock;
  uint32_t done;
} db_value;

static const char *GRN_DB_CONFIG_PATH_FORMAT = "%s.conf";

static grn_bool
grn_db_config_create(grn_ctx *ctx, grn_db *s, const char *path,
                     const char *context_tag)
{
  char *config_path;
  char config_path_buffer[PATH_MAX];
  uint32_t flags = GRN_OBJ_KEY_VAR_SIZE;

  if (path) {
    grn_snprintf(config_path_buffer, PATH_MAX, PATH_MAX,
                 GRN_DB_CONFIG_PATH_FORMAT, path);
    config_path = config_path_buffer;
  } else {
    config_path = NULL;
  }
  s->config = grn_hash_create(ctx, config_path,
                              GRN_CONFIG_MAX_KEY_SIZE,
                              GRN_CONFIG_VALUE_SPACE_SIZE,
                              flags);
  if (!s->config) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "%s failed to create data store for configuration: <%s>",
        context_tag,
        config_path ? config_path : "(temporary)");
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static grn_bool
grn_db_config_open(grn_ctx *ctx, grn_db *s, const char *path)
{
  char config_path[PATH_MAX];

  grn_snprintf(config_path, PATH_MAX, PATH_MAX, GRN_DB_CONFIG_PATH_FORMAT, path);
  if (grn_path_exist(config_path)) {
    s->config = grn_hash_open(ctx, config_path);
    if (!s->config) {
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[db][open] failed to open data store for configuration: <%s>",
          config_path);
      return GRN_FALSE;
    }
    return GRN_TRUE;
  } else {
    return grn_db_config_create(ctx, s, path, "[db][open]");
  }
}

static grn_rc
grn_db_config_remove(grn_ctx *ctx, const char *path)
{
  char config_path[PATH_MAX];

  grn_snprintf(config_path, PATH_MAX, PATH_MAX, GRN_DB_CONFIG_PATH_FORMAT, path);
  return grn_hash_remove(ctx, config_path);
}

grn_obj *
grn_db_create(grn_ctx *ctx, const char *path, grn_db_create_optarg *optarg)
{
  grn_db *s = NULL;

  GRN_API_ENTER;

  if (path && strlen(path) > PATH_MAX - 14) {
    ERR(GRN_INVALID_ARGUMENT, "too long path");
    goto exit;
  }

  s = GRN_MALLOC(sizeof(grn_db));
  if (!s) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "grn_db alloc failed");
    goto exit;
  }

  CRITICAL_SECTION_INIT(s->lock);
  grn_tiny_array_init(ctx, &s->values, sizeof(db_value),
                      GRN_TINY_ARRAY_CLEAR|
                      GRN_TINY_ARRAY_THREADSAFE|
                      GRN_TINY_ARRAY_USE_MALLOC);
  s->keys = NULL;
  s->specs = NULL;
  s->config = NULL;

  {
    grn_bool use_default_db_key = GRN_TRUE;
    grn_bool use_pat_as_db_keys = GRN_FALSE;
    if (grn_db_key[0]) {
      if (!strcmp(grn_db_key, "pat")) {
        use_default_db_key = GRN_FALSE;
        use_pat_as_db_keys = GRN_TRUE;
      } else if (!strcmp(grn_db_key, "dat")) {
        use_default_db_key = GRN_FALSE;
      }
    }

    if (use_default_db_key && !strcmp(GRN_DEFAULT_DB_KEY, "pat")) {
      use_pat_as_db_keys = GRN_TRUE;
    }
    if (use_pat_as_db_keys) {
      s->keys = (grn_obj *)grn_pat_create(ctx, path, GRN_TABLE_MAX_KEY_SIZE,
                                          0, GRN_OBJ_KEY_VAR_SIZE);
    } else {
      s->keys = (grn_obj *)grn_dat_create(ctx, path, GRN_TABLE_MAX_KEY_SIZE,
                                          0, GRN_OBJ_KEY_VAR_SIZE);
    }
  }

  if (!s->keys) {
    goto exit;
  }

  GRN_DB_OBJ_SET_TYPE(s, GRN_DB);
  s->obj.db = (grn_obj *)s;
  s->obj.header.domain = GRN_ID_NIL;
  DB_OBJ(&s->obj)->range = GRN_ID_NIL;
  /* prepare builtin classes and load builtin plugins. */
  if (path) {
    {
      char specs_path[PATH_MAX];
      gen_pathname(path, specs_path, 0);
      s->specs = grn_ja_create(ctx, specs_path, 65536, 0);
      if (!s->specs) {
        ERR(GRN_NO_MEMORY_AVAILABLE,
            "failed to create specs: <%s>", specs_path);
        goto exit;
      }
    }
    if (!grn_db_config_create(ctx, s, path, "[db][create]")) {
      goto exit;
    }
    grn_ctx_use(ctx, (grn_obj *)s);
    grn_db_init_builtin_types(ctx);
    grn_obj_flush(ctx, (grn_obj *)s);
    GRN_API_RETURN((grn_obj *)s);
  } else {
    if (!grn_db_config_create(ctx, s, NULL, "[db][create]")) {
      goto exit;
    }
    grn_ctx_use(ctx, (grn_obj *)s);
    grn_db_init_builtin_types(ctx);
    GRN_API_RETURN((grn_obj *)s);
  }

exit:
  if (s) {
    if (s->keys) {
      if (s->keys->header.type == GRN_TABLE_PAT_KEY) {
        grn_pat_close(ctx, (grn_pat *)s->keys);
        grn_pat_remove(ctx, path);
      } else {
        grn_dat_close(ctx, (grn_dat *)s->keys);
        grn_dat_remove(ctx, path);
      }
    }
    if (s->specs) {
      const char *specs_path;
      specs_path = grn_obj_path(ctx, (grn_obj *)(s->specs));
      grn_ja_close(ctx, s->specs);
      grn_ja_remove(ctx, specs_path);
    }
    grn_tiny_array_fin(&s->values);
    CRITICAL_SECTION_FIN(s->lock);
    GRN_FREE(s);
  }

  GRN_API_RETURN(NULL);
}

grn_obj *
grn_db_open(grn_ctx *ctx, const char *path)
{
  grn_db *s = NULL;

  GRN_API_ENTER;

  if (!path) {
    ERR(GRN_INVALID_ARGUMENT, "[db][open] path is missing");
    goto exit;
  }

  if (strlen(path) > PATH_MAX - 14) {
    ERR(GRN_INVALID_ARGUMENT, "inappropriate path");
    goto exit;
  }

  s = GRN_MALLOC(sizeof(grn_db));
  if (!s) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "grn_db alloc failed");
    goto exit;
  }

  CRITICAL_SECTION_INIT(s->lock);
  grn_tiny_array_init(ctx, &s->values, sizeof(db_value),
                      GRN_TINY_ARRAY_CLEAR|
                      GRN_TINY_ARRAY_THREADSAFE|
                      GRN_TINY_ARRAY_USE_MALLOC);
  s->keys = NULL;
  s->specs = NULL;
  s->config = NULL;

  {
    uint32_t type = grn_io_detect_type(ctx, path);
    switch (type) {
    case GRN_TABLE_PAT_KEY :
      s->keys = (grn_obj *)grn_pat_open(ctx, path);
      break;
    case GRN_TABLE_DAT_KEY :
      s->keys = (grn_obj *)grn_dat_open(ctx, path);
      break;
    default :
      s->keys = NULL;
      if (ctx->rc == GRN_SUCCESS) {
        ERR(GRN_INVALID_ARGUMENT,
            "[db][open] invalid keys table's type: %#x", type);
        goto exit;
      }
      break;
    }
  }

  if (!s->keys) {
    goto exit;
  }

  {
    char specs_path[PATH_MAX];
    gen_pathname(path, specs_path, 0);
    s->specs = grn_ja_open(ctx, specs_path);
    if (!s->specs) {
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[db][open] failed to open specs: <%s>", specs_path);
      goto exit;
    }
  }
  if (!grn_db_config_open(ctx, s, path)) {
    goto exit;
  }

  GRN_DB_OBJ_SET_TYPE(s, GRN_DB);
  s->obj.db = (grn_obj *)s;
  s->obj.header.domain = GRN_ID_NIL;
  DB_OBJ(&s->obj)->range = GRN_ID_NIL;
  grn_ctx_use(ctx, (grn_obj *)s);
  {
    unsigned int n_records;

    n_records = grn_table_size(ctx, (grn_obj *)s);
#ifdef GRN_WITH_MECAB
    if (grn_db_init_mecab_tokenizer(ctx)) {
      ERRCLR(ctx);
    }
#endif
    grn_db_init_builtin_tokenizers(ctx);
    grn_db_init_builtin_normalizers(ctx);
    grn_db_init_builtin_scorers(ctx);
    grn_db_init_builtin_commands(ctx);
    grn_db_init_builtin_window_functions(ctx);

    if (grn_table_size(ctx, (grn_obj *)s) > n_records) {
      grn_obj_flush(ctx, (grn_obj *)s);
    }
  }
  GRN_API_RETURN((grn_obj *)s);

exit:
  if (s) {
    if (s->specs) {
      grn_ja_close(ctx, s->specs);
    }
    if (s->keys) {
      if (s->keys->header.type == GRN_TABLE_PAT_KEY) {
        grn_pat_close(ctx, (grn_pat *)s->keys);
      } else {
        grn_dat_close(ctx, (grn_dat *)s->keys);
      }
    }
    grn_tiny_array_fin(&s->values);
    CRITICAL_SECTION_FIN(s->lock);
    GRN_FREE(s);
  }

  GRN_API_RETURN(NULL);
}

static grn_id
grn_db_curr_id(grn_ctx *ctx, grn_obj *db)
{
  grn_id curr_id = GRN_ID_NIL;
  grn_db *s = (grn_db *)db;
  switch (s->keys->header.type) {
  case GRN_TABLE_PAT_KEY :
    curr_id = grn_pat_curr_id(ctx, (grn_pat *)s->keys);
    break;
  case GRN_TABLE_DAT_KEY :
    curr_id = grn_dat_curr_id(ctx, (grn_dat *)s->keys);
    break;
  }
  return curr_id;
}

/* s must be validated by caller */
grn_rc
grn_db_close(grn_ctx *ctx, grn_obj *db)
{
  grn_id id;
  db_value *vp;
  grn_db *s = (grn_db *)db;
  grn_bool ctx_used_db;
  if (!s) { return GRN_INVALID_ARGUMENT; }
  GRN_API_ENTER;

  ctx_used_db = ctx->impl && ctx->impl->db == db;
  if (ctx_used_db) {
#ifdef GRN_WITH_MECAB
    grn_db_fin_mecab_tokenizer(ctx);
#endif
    grn_ctx_loader_clear(ctx);
    if (ctx->impl->parser) {
      grn_expr_parser_close(ctx);
    }
  }

  GRN_TINY_ARRAY_EACH(&s->values, 1, grn_db_curr_id(ctx, db), id, vp, {
    if (vp->ptr) { grn_obj_close(ctx, vp->ptr); }
  });

  if (ctx_used_db) {
    if (ctx->impl->values) {
      grn_db_obj *o;
      GRN_ARRAY_EACH(ctx, ctx->impl->values, 0, 0, id, &o, {
        grn_obj_close(ctx, *((grn_obj **)o));
      });
      grn_array_truncate(ctx, ctx->impl->values);
    }
  }

/* grn_tiny_array_fin should be refined.. */
#ifdef WIN32
  {
    grn_tiny_array *a = &s->values;
    CRITICAL_SECTION_FIN(a->lock);
  }
#endif
  grn_tiny_array_fin(&s->values);

  switch (s->keys->header.type) {
  case GRN_TABLE_PAT_KEY :
    grn_pat_close(ctx, (grn_pat *)s->keys);
    break;
  case GRN_TABLE_DAT_KEY :
    grn_dat_close(ctx, (grn_dat *)s->keys);
    break;
  }
  CRITICAL_SECTION_FIN(s->lock);
  if (s->specs) { grn_ja_close(ctx, s->specs); }
  grn_hash_close(ctx, s->config);
  GRN_FREE(s);

  if (ctx_used_db) {
    grn_cache *cache;
    cache = grn_cache_current_get(ctx);
    if (cache) {
      grn_cache_expire(cache, -1);
    }
    ctx->impl->db = NULL;
  }

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_obj *
grn_ctx_get(grn_ctx *ctx, const char *name, int name_size)
{
  grn_obj *obj = NULL;
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    return NULL;
  }
  GRN_API_ENTER;
  if (GRN_DB_P(db)) {
    grn_db *s = (grn_db *)db;
    grn_obj *alias_table = NULL;
    grn_obj *alias_column = NULL;
    grn_obj alias_name_buffer;

    if (name_size < 0) {
      name_size = strlen(name);
    }
    GRN_TEXT_INIT(&alias_name_buffer, 0);
    while (GRN_TRUE) {
      grn_id id;

      id = grn_table_get(ctx, s->keys, name, name_size);
      if (id) {
        obj = grn_ctx_at(ctx, id);
        break;
      }

      if (!alias_column) {
        grn_id alias_column_id;
        const char *alias_column_name;
        uint32_t alias_column_name_size;

        grn_config_get(ctx,
                       "alias.column", -1,
                       &alias_column_name, &alias_column_name_size);
        if (!alias_column_name) {
          break;
        }
        alias_column_id = grn_table_get(ctx,
                                        s->keys,
                                        alias_column_name,
                                        alias_column_name_size);
        if (!alias_column_id) {
          break;
        }
        alias_column = grn_ctx_at(ctx, alias_column_id);
        if (alias_column->header.type != GRN_COLUMN_VAR_SIZE) {
          break;
        }
        if (alias_column->header.flags & GRN_OBJ_VECTOR) {
          break;
        }
        if (DB_OBJ(alias_column)->range != GRN_DB_SHORT_TEXT) {
          break;
        }
        alias_table = grn_ctx_at(ctx, alias_column->header.domain);
        if (alias_table->header.type == GRN_TABLE_NO_KEY) {
          break;
        }
      }

      {
        grn_id alias_id;
        alias_id = grn_table_get(ctx, alias_table, name, name_size);
        if (!alias_id) {
          break;
        }
        GRN_BULK_REWIND(&alias_name_buffer);
        grn_obj_get_value(ctx, alias_column, alias_id, &alias_name_buffer);
        name = GRN_TEXT_VALUE(&alias_name_buffer);
        name_size = GRN_TEXT_LEN(&alias_name_buffer);
      }
    }
    GRN_OBJ_FIN(ctx, &alias_name_buffer);
  }
  GRN_API_RETURN(obj);
}

grn_obj *
grn_ctx_db(grn_ctx *ctx)
{
  return (ctx && ctx->impl) ? ctx->impl->db : NULL;
}

grn_obj *
grn_db_keys(grn_obj *s)
{
  return (grn_obj *)(((grn_db *)s)->keys);
}

uint32_t
grn_obj_get_last_modified(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return 0;
  }

  return grn_obj_get_io(ctx, obj)->header->last_modified;
}

grn_bool
grn_obj_is_dirty(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  switch (obj->header.type) {
  case GRN_DB :
    return grn_db_is_dirty(ctx, obj);
  case GRN_TABLE_PAT_KEY :
    return grn_pat_is_dirty(ctx, (grn_pat *)obj);
  case GRN_TABLE_DAT_KEY :
    return grn_dat_is_dirty(ctx, (grn_dat *)obj);
  default :
    return GRN_FALSE;
  }
}

uint32_t
grn_db_get_last_modified(grn_ctx *ctx, grn_obj *db)
{
  return grn_obj_get_last_modified(ctx, db);
}

grn_bool
grn_db_is_dirty(grn_ctx *ctx, grn_obj *db)
{
  grn_obj *keys;

  if (!db) {
    return GRN_FALSE;
  }

  keys = ((grn_db *)db)->keys;
  return grn_obj_is_dirty(ctx, keys);
}

static grn_rc
grn_db_dirty(grn_ctx *ctx, grn_obj *db)
{
  grn_obj *keys;

  if (!db) {
    return GRN_SUCCESS;
  }

  keys = ((grn_db *)db)->keys;
  switch (keys->header.type) {
  case GRN_TABLE_PAT_KEY :
    return grn_pat_dirty(ctx, (grn_pat *)keys);
  case GRN_TABLE_DAT_KEY :
    return grn_dat_dirty(ctx, (grn_dat *)keys);
  default :
    return GRN_SUCCESS;
  }
}

static grn_rc
grn_db_clean(grn_ctx *ctx, grn_obj *db)
{
  grn_obj *keys;

  if (!db) {
    return GRN_SUCCESS;
  }

  keys = ((grn_db *)db)->keys;
  switch (keys->header.type) {
  case GRN_TABLE_PAT_KEY :
    return grn_pat_clean(ctx, (grn_pat *)keys);
  case GRN_TABLE_DAT_KEY :
    return grn_dat_clean(ctx, (grn_dat *)keys);
  default :
    return GRN_SUCCESS;
  }
}

static grn_rc
grn_db_clear_dirty(grn_ctx *ctx, grn_obj *db)
{
  grn_obj *keys;

  if (!db) {
    return GRN_SUCCESS;
  }

  keys = ((grn_db *)db)->keys;
  switch (keys->header.type) {
  case GRN_TABLE_PAT_KEY :
    return grn_pat_clear_dirty(ctx, (grn_pat *)keys);
  case GRN_TABLE_DAT_KEY :
    return grn_dat_clear_dirty(ctx, (grn_dat *)keys);
  default :
    return GRN_SUCCESS;
  }
}

void
grn_db_touch(grn_ctx *ctx, grn_obj *s)
{
  grn_obj_touch(ctx, s, NULL);
}

grn_bool
grn_obj_is_corrupt(grn_ctx *ctx, grn_obj *obj)
{
  grn_bool is_corrupt = GRN_FALSE;

  GRN_API_ENTER;

  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT, "[object][corrupt] object must not be NULL");
    GRN_API_RETURN(GRN_FALSE);
  }

  switch (obj->header.type) {
  case GRN_DB :
    is_corrupt = grn_io_is_corrupt(ctx, grn_obj_get_io(ctx, obj));
    if (!is_corrupt) {
      is_corrupt = grn_io_is_corrupt(ctx, ((grn_db *)obj)->specs->io);
    }
    if (!is_corrupt) {
      is_corrupt = grn_io_is_corrupt(ctx, ((grn_db *)obj)->config->io);
    }
    break;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
    is_corrupt = grn_io_is_corrupt(ctx, grn_obj_get_io(ctx, obj));
    break;
  case GRN_TABLE_DAT_KEY :
    is_corrupt = grn_dat_is_corrupt(ctx, (grn_dat *)obj);
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    is_corrupt = grn_io_is_corrupt(ctx, grn_obj_get_io(ctx, obj));
    break;
  case GRN_COLUMN_INDEX :
    is_corrupt = grn_io_is_corrupt(ctx, ((grn_ii *)obj)->seg);
    if (!is_corrupt) {
      is_corrupt = grn_io_is_corrupt(ctx, ((grn_ii *)obj)->chunk);
    }
    break;
  default :
    break;
  }

  GRN_API_RETURN(is_corrupt);
}

#define IS_TEMP(obj) (DB_OBJ(obj)->id & GRN_OBJ_TMP_OBJECT)

static inline void
grn_obj_touch_db(grn_ctx *ctx, grn_obj *obj, grn_timeval *tv)
{
  grn_obj_get_io(ctx, obj)->header->last_modified = tv->tv_sec;
  grn_db_dirty(ctx, obj);
}

void
grn_obj_touch(grn_ctx *ctx, grn_obj *obj, grn_timeval *tv)
{
  grn_timeval tv_;
  if (!tv) {
    grn_timeval_now(ctx, &tv_);
    tv = &tv_;
  }
  if (obj) {
    switch (obj->header.type) {
    case GRN_DB :
      grn_obj_touch_db(ctx, obj, tv);
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_INDEX :
      if (!IS_TEMP(obj)) {
        grn_obj_get_io(ctx, obj)->header->last_modified = tv->tv_sec;
        grn_obj_touch(ctx, DB_OBJ(obj)->db, tv);
      }
      break;
    }
  }
}

grn_rc
grn_db_check_name(grn_ctx *ctx, const char *name, unsigned int name_size)
{
  int len;
  const char *name_end = name + name_size;
  if (name_size > 0 &&
      *name == GRN_DB_PSEUDO_COLUMN_PREFIX) {
    return GRN_INVALID_ARGUMENT;
  }
  while (name < name_end) {
    char c = *name;
    if ((unsigned int)((c | 0x20) - 'a') >= 26u &&
        (unsigned int)(c - '0') >= 10u &&
        c != '_' &&
        c != '-' &&
        c != '#' &&
        c != '@') {
      return GRN_INVALID_ARGUMENT;
    }
    if (!(len = grn_charlen(ctx, name, name_end))) { break; }
    name += len;
  }
  return GRN_SUCCESS;
}

static grn_obj *
grn_type_open(grn_ctx *ctx, grn_obj_spec *spec)
{
  struct _grn_type *res;
  res = GRN_MALLOC(sizeof(struct _grn_type));
  if (res) {
    GRN_DB_OBJ_SET_TYPE(res, GRN_TYPE);
    res->obj.header = spec->header;
    GRN_TYPE_SIZE(&res->obj) = GRN_TYPE_SIZE(spec);
  }
  return (grn_obj *)res;
}

grn_obj *
grn_proc_create(grn_ctx *ctx, const char *name, int name_size, grn_proc_type type,
                grn_proc_func *init, grn_proc_func *next, grn_proc_func *fin,
                unsigned int nvars, grn_expr_var *vars)
{
  grn_proc *res = NULL;
  grn_id id = GRN_ID_NIL;
  grn_id range = GRN_ID_NIL;
  int added = 0;
  grn_obj *db;
  const char *path;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
  path = ctx->impl->plugin_path;
  if (path) {
    range = grn_plugin_reference(ctx, path);
  }
  if (name_size < 0) {
    name_size = strlen(name);
  }
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[proc][create]", name, name_size);
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    GRN_API_RETURN(NULL);
  }
  if (name && name_size) {
    grn_db *s = (grn_db *)db;
    if (!(id = grn_table_get(ctx, s->keys, name, name_size))) {
      if (!(id = grn_table_add(ctx, s->keys, name, name_size, &added))) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "grn_table_add failed");
        GRN_API_RETURN(NULL);
      }
    }
    if (!added) {
      db_value *vp;
      if ((vp = grn_tiny_array_at(&s->values, id)) && (res = (grn_proc *)vp->ptr)) {
        /* TODO: Do more robust check. */
        if (res->funcs[PROC_INIT] ||
            res->funcs[PROC_NEXT] ||
            res->funcs[PROC_FIN]) {
          ERR(GRN_INVALID_ARGUMENT, "already used name");
          GRN_API_RETURN(NULL);
        }
        if (range != GRN_ID_NIL) {
          grn_plugin_close(ctx, range);
        }
        GRN_API_RETURN((grn_obj *)res);
      } else {
        added = 1;
      }
    }
  } else if (ctx->impl && ctx->impl->values) {
    id = grn_array_add(ctx, ctx->impl->values, NULL) | GRN_OBJ_TMP_OBJECT;
    added = 1;
  }
  if (!res) { res = GRN_MALLOCN(grn_proc, 1); }
  if (res) {
    GRN_DB_OBJ_SET_TYPE(res, GRN_PROC);
    res->obj.db = db;
    res->obj.id = id;
    res->obj.header.domain = GRN_ID_NIL;
    res->obj.header.flags = path ? GRN_OBJ_CUSTOM_NAME : 0;
    res->obj.range = range;
    res->type = type;
    res->funcs[PROC_INIT] = init;
    res->funcs[PROC_NEXT] = next;
    res->funcs[PROC_FIN] = fin;
    memset(&(res->callbacks), 0, sizeof(res->callbacks));
    res->callbacks.function.selector_op = GRN_OP_NOP;
    res->callbacks.function.is_stable = GRN_TRUE;
    GRN_TEXT_INIT(&res->name_buf, 0);
    res->vars = NULL;
    res->nvars = 0;
    if (added) {
      if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
        // grn_obj_delete(ctx, db, id);
        GRN_FREE(res);
        GRN_API_RETURN(NULL);
      }
    }
    while (nvars--) {
      grn_obj *v = grn_expr_add_var(ctx, (grn_obj *)res, vars->name, vars->name_size);
      GRN_OBJ_INIT(v, vars->value.header.type, 0, vars->value.header.domain);
      GRN_TEXT_PUT(ctx, v, GRN_TEXT_VALUE(&vars->value), GRN_TEXT_LEN(&vars->value));
      vars++;
    }
  }
  GRN_API_RETURN((grn_obj *)res);
}

/* grn_table */

static void
calc_rec_size(grn_table_flags flags, uint32_t max_n_subrecs, uint32_t range_size,
              uint32_t additional_value_size,
              uint8_t *subrec_size, uint8_t *subrec_offset,
              uint32_t *key_size, uint32_t *value_size)
{
  *subrec_size = 0;
  *subrec_offset = 0;
  if (flags & GRN_OBJ_WITH_SUBREC) {
    switch (flags & GRN_OBJ_UNIT_MASK) {
    case GRN_OBJ_UNIT_DOCUMENT_NONE :
      break;
    case GRN_OBJ_UNIT_DOCUMENT_SECTION :
      *subrec_offset = sizeof(grn_id);
      *subrec_size = sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_DOCUMENT_POSITION :
      *subrec_offset = sizeof(grn_id);
      *subrec_size = sizeof(uint32_t) + sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_SECTION_NONE :
      *key_size += sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_SECTION_POSITION :
      *key_size += sizeof(uint32_t);
      *subrec_offset = sizeof(grn_id) + sizeof(uint32_t);
      *subrec_size = sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_POSITION_NONE :
      *key_size += sizeof(uint32_t) + sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_USERDEF_DOCUMENT :
      *subrec_size = range_size;
      break;
    case GRN_OBJ_UNIT_USERDEF_SECTION :
      *subrec_size = range_size + sizeof(uint32_t);
      break;
    case GRN_OBJ_UNIT_USERDEF_POSITION :
      *subrec_size = range_size + sizeof(uint32_t) + sizeof(uint32_t);
      break;
    }
    *value_size = (uintptr_t)GRN_RSET_SUBRECS_NTH((((grn_rset_recinfo *)0)->subrecs),
                                                  *subrec_size, max_n_subrecs);
  } else {
    *value_size = range_size;
  }
  *value_size += additional_value_size;
}

static grn_rc _grn_obj_remove(grn_ctx *ctx, grn_obj *obj, grn_bool dependent);

static grn_rc
grn_table_create_validate(grn_ctx *ctx, const char *name, unsigned int name_size,
                          const char *path, grn_table_flags flags,
                          grn_obj *key_type, grn_obj *value_type)
{
  grn_table_flags table_type;
  const char *table_type_name = NULL;

  table_type = (flags & GRN_OBJ_TABLE_TYPE_MASK);
  switch (table_type) {
  case GRN_OBJ_TABLE_HASH_KEY :
    table_type_name = "TABLE_HASH_KEY";
    break;
  case GRN_OBJ_TABLE_PAT_KEY :
    table_type_name = "TABLE_PAT_KEY";
    break;
  case GRN_OBJ_TABLE_DAT_KEY :
    table_type_name = "TABLE_DAT_KEY";
    break;
  case GRN_OBJ_TABLE_NO_KEY :
    table_type_name = "TABLE_NO_KEY";
    break;
  default :
    table_type_name = "unknown";
    break;
  }

  if (!key_type && table_type != GRN_OBJ_TABLE_NO_KEY &&
      !(flags & GRN_OBJ_KEY_VAR_SIZE)) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] "
        "key type is required for TABLE_HASH_KEY, TABLE_PAT_KEY or "
        "TABLE_DAT_KEY: <%.*s>", name_size, name);
    return ctx->rc;
  }

  if (key_type && table_type == GRN_OBJ_TABLE_NO_KEY) {
    int key_name_size;
    char key_name[GRN_TABLE_MAX_KEY_SIZE];
    key_name_size = grn_obj_name(ctx, key_type, key_name,
                                 GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] "
        "key isn't available for TABLE_NO_KEY table: <%.*s> (%.*s)",
        name_size, name, key_name_size, key_name);
    return ctx->rc;
  }

  if ((flags & GRN_OBJ_KEY_WITH_SIS) &&
      table_type != GRN_OBJ_TABLE_PAT_KEY) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] "
        "key with SIS is available only for TABLE_PAT_KEY table: "
        "<%.*s>(%s)",
        name_size, name,
        table_type_name);
    return ctx->rc;
  }

  if ((flags & GRN_OBJ_KEY_NORMALIZE) &&
      table_type == GRN_OBJ_TABLE_NO_KEY) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] "
        "key normalization isn't available for TABLE_NO_KEY table: <%.*s>",
        name_size, name);
    return ctx->rc;
  }

  if ((flags & GRN_OBJ_KEY_LARGE) &&
      table_type != GRN_OBJ_TABLE_HASH_KEY) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] "
        "large key support is available only for TABLE_HASH_KEY key table: "
        "<%.*s>(%s)",
        name_size, name,
        table_type_name);
    return ctx->rc;
  }

  return ctx->rc;
}

static grn_obj *
grn_table_create_with_max_n_subrecs(grn_ctx *ctx, const char *name,
                                    unsigned int name_size, const char *path,
                                    grn_table_flags flags, grn_obj *key_type,
                                    grn_obj *value_type,
                                    uint32_t max_n_subrecs,
                                    uint32_t additional_value_size)
{
  grn_id id;
  grn_id domain = GRN_ID_NIL, range = GRN_ID_NIL;
  uint32_t key_size, value_size = 0, range_size = 0;
  uint8_t subrec_size, subrec_offset;
  grn_obj *res = NULL;
  grn_obj *db;
  char buffer[PATH_MAX];
  if (!ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "[table][create] db not initialized");
    return NULL;
  }
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[table][create]", name, name_size);
    return NULL;
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "[table][create] invalid db assigned");
    return NULL;
  }
  if (grn_table_create_validate(ctx, name, name_size, path, flags,
                                key_type, value_type)) {
    return NULL;
  }
  if (key_type) {
    domain = DB_OBJ(key_type)->id;
    switch (key_type->header.type) {
    case GRN_TYPE :
      {
        grn_db_obj *t = (grn_db_obj *)key_type;
        flags |= t->header.flags;
        key_size = GRN_TYPE_SIZE(t);
        if (key_size > GRN_TABLE_MAX_KEY_SIZE) {
          int type_name_size;
          char type_name[GRN_TABLE_MAX_KEY_SIZE];
          type_name_size = grn_obj_name(ctx, key_type, type_name,
                                        GRN_TABLE_MAX_KEY_SIZE);
          ERR(GRN_INVALID_ARGUMENT,
              "[table][create] key size too big: <%.*s> <%.*s>(%u) (max:%u)",
              name_size, name,
              type_name_size, type_name,
              key_size, GRN_TABLE_MAX_KEY_SIZE);
          return NULL;
        }
      }
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
      key_size = sizeof(grn_id);
      break;
    default :
      {
        int key_name_size;
        char key_name[GRN_TABLE_MAX_KEY_SIZE];
        key_name_size = grn_obj_name(ctx, key_type, key_name,
                                     GRN_TABLE_MAX_KEY_SIZE);
        ERR(GRN_INVALID_ARGUMENT,
            "[table][create] key type must be type or table: <%.*s> (%.*s)",
            name_size, name, key_name_size, key_name);
        return NULL;
      }
      break;
    }
  } else {
    key_size = (flags & GRN_OBJ_KEY_VAR_SIZE) ? GRN_TABLE_MAX_KEY_SIZE : sizeof(grn_id);
  }
  if (value_type) {
    range = DB_OBJ(value_type)->id;
    switch (value_type->header.type) {
    case GRN_TYPE :
      {
        grn_db_obj *t = (grn_db_obj *)value_type;
        if (t->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
          int type_name_size;
          char type_name[GRN_TABLE_MAX_KEY_SIZE];
          type_name_size = grn_obj_name(ctx, value_type, type_name,
                                        GRN_TABLE_MAX_KEY_SIZE);
          ERR(GRN_INVALID_ARGUMENT,
              "[table][create] value type must be fixed size: <%.*s> (%.*s)",
              name_size, name, type_name_size, type_name);
          return NULL;
        }
        range_size = GRN_TYPE_SIZE(t);
      }
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
      range_size = sizeof(grn_id);
      break;
    default :
      {
        int value_name_size;
        char value_name[GRN_TABLE_MAX_KEY_SIZE];
        value_name_size = grn_obj_name(ctx, value_type, value_name,
                                       GRN_TABLE_MAX_KEY_SIZE);
        ERR(GRN_INVALID_ARGUMENT,
            "[table][create] value type must be type or table: <%.*s> (%.*s)",
            name_size, name, value_name_size, value_name);
        return NULL;
      }
      break;
    }
  }

  id = grn_obj_register(ctx, db, name, name_size);
  if (ERRP(ctx, GRN_ERROR)) { return NULL;  }
  if (GRN_OBJ_PERSISTENT & flags) {
    GRN_LOG(ctx, GRN_LOG_NOTICE,
            "DDL:%u:table_create %.*s", id, name_size, name);
    if (!path) {
      if (GRN_DB_PERSISTENT_P(db)) {
        grn_db_generate_pathname(ctx, db, id, buffer);
        path = buffer;
      } else {
        ERR(GRN_INVALID_ARGUMENT, "path not assigned for persistent table");
        grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
        return NULL;
      }
    } else {
      flags |= GRN_OBJ_CUSTOM_NAME;
    }
  } else {
    if (path) {
      ERR(GRN_INVALID_ARGUMENT, "path assigned for temporary table");
      grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
      return NULL;
    }
    if (GRN_DB_PERSISTENT_P(db) && name && name_size) {
      ERR(GRN_INVALID_ARGUMENT, "name assigned for temporary table");
      grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
      return NULL;
    }
  }
  calc_rec_size(flags, max_n_subrecs, range_size, additional_value_size,
                &subrec_size, &subrec_offset, &key_size, &value_size);
  switch (flags & GRN_OBJ_TABLE_TYPE_MASK) {
  case GRN_OBJ_TABLE_HASH_KEY :
    res = (grn_obj *)grn_hash_create(ctx, path, key_size, value_size, flags);
    break;
  case GRN_OBJ_TABLE_PAT_KEY :
    res = (grn_obj *)grn_pat_create(ctx, path, key_size, value_size, flags);
    break;
  case GRN_OBJ_TABLE_DAT_KEY :
    res = (grn_obj *)grn_dat_create(ctx, path, key_size, value_size, flags);
    break;
  case GRN_OBJ_TABLE_NO_KEY :
    domain = range;
    res = (grn_obj *)grn_array_create(ctx, path, value_size, flags);
    break;
  }
  if (res) {
    DB_OBJ(res)->header.impl_flags = 0;
    DB_OBJ(res)->header.domain = domain;
    DB_OBJ(res)->range = range;
    DB_OBJ(res)->max_n_subrecs = max_n_subrecs;
    DB_OBJ(res)->subrec_size = subrec_size;
    DB_OBJ(res)->subrec_offset = subrec_offset;
    DB_OBJ(res)->flags.group = 0;
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      _grn_obj_remove(ctx, res, GRN_FALSE);
      res = NULL;
    }
  } else {
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
  }
  return res;
}

grn_obj *
grn_table_create(grn_ctx *ctx, const char *name, unsigned int name_size,
                 const char *path, grn_table_flags flags,
                 grn_obj *key_type, grn_obj *value_type)
{
  grn_obj *res;
  GRN_API_ENTER;
  res = grn_table_create_with_max_n_subrecs(ctx, name, name_size, path,
                                            flags, key_type, value_type,
                                            0, 0);
  GRN_API_RETURN(res);
}

grn_obj *
grn_table_create_for_group(grn_ctx *ctx, const char *name,
                           unsigned int name_size, const char *path,
                           grn_obj *group_key, grn_obj *value_type,
                           unsigned int max_n_subrecs)
{
  grn_obj *res = NULL;
  GRN_API_ENTER;
  if (group_key) {
    grn_obj *key_type;
    key_type = grn_ctx_at(ctx, grn_obj_get_range(ctx, group_key));
    if (key_type) {
      res = grn_table_create_with_max_n_subrecs(ctx, name, name_size, path,
                                                GRN_TABLE_HASH_KEY|
                                                GRN_OBJ_WITH_SUBREC|
                                                GRN_OBJ_UNIT_USERDEF_DOCUMENT,
                                                key_type, value_type,
                                                max_n_subrecs, 0);
      grn_obj_unlink(ctx, key_type);
    }
  } else {
    res = grn_table_create_with_max_n_subrecs(ctx, name, name_size, path,
                                              GRN_TABLE_HASH_KEY|
                                              GRN_OBJ_KEY_VAR_SIZE|
                                              GRN_OBJ_WITH_SUBREC|
                                              GRN_OBJ_UNIT_USERDEF_DOCUMENT,
                                              NULL, value_type,
                                              max_n_subrecs, 0);
  }
  GRN_API_RETURN(res);
}

unsigned int
grn_table_get_subrecs(grn_ctx *ctx, grn_obj *table, grn_id id,
                      grn_id *subrecbuf, int *scorebuf, int buf_size)
{
  unsigned int count = 0;
  GRN_API_ENTER;
  if (GRN_OBJ_TABLEP(table)) {
    uint32_t value_size;
    grn_rset_recinfo *ri;
    uint32_t subrec_size = DB_OBJ(table)->subrec_size;
    uint32_t max_n_subrecs = DB_OBJ(table)->max_n_subrecs;
    if (subrec_size < sizeof(grn_id)) { goto exit; }
    if (!max_n_subrecs) { goto exit; }
    ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, table, id, &value_size);
    if (ri) {
      byte *psubrec = (byte *)ri->subrecs;
      uint32_t n_subrecs = (uint32_t)GRN_RSET_N_SUBRECS(ri);
      uint32_t limit = value_size / (GRN_RSET_SCORE_SIZE + subrec_size);
      if ((int) limit > buf_size) {
        limit = buf_size;
      }
      if (limit > n_subrecs) {
        limit = n_subrecs;
      }
      if (limit > max_n_subrecs) {
        limit = max_n_subrecs;
      }
      for (; count < limit; count++) {
        if (scorebuf) {
          scorebuf[count] = *((double *)psubrec);
        }
        psubrec += GRN_RSET_SCORE_SIZE;
        if (subrecbuf) {
          subrecbuf[count] = *((grn_id *)psubrec);
        }
        psubrec += subrec_size;
      }
    }
  }
exit :
  GRN_API_RETURN(count);
}

grn_obj *
grn_table_open(grn_ctx *ctx, const char *name, unsigned int name_size, const char *path)
{
  grn_obj *db;
  if (!ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    GRN_API_RETURN(NULL);
  } else {
    grn_obj *res = grn_ctx_get(ctx, name, name_size);
    if (res) {
      const char *path2 = grn_obj_path(ctx, res);
      if (path && (!path2 || strcmp(path, path2))) {
        ERR(GRN_INVALID_ARGUMENT, "path unmatch");
        GRN_API_RETURN(NULL);
      }
    } else if (path) {
      uint32_t type = grn_io_detect_type(ctx, path);
      if (!type) { GRN_API_RETURN(NULL); }
      switch (type) {
      case GRN_TABLE_HASH_KEY :
        res = (grn_obj *)grn_hash_open(ctx, path);
        break;
      case GRN_TABLE_PAT_KEY :
        res = (grn_obj *)grn_pat_open(ctx, path);
        break;
      case GRN_TABLE_DAT_KEY :
        res = (grn_obj *)grn_dat_open(ctx, path);
        break;
      case GRN_TABLE_NO_KEY :
        res = (grn_obj *)grn_array_open(ctx, path);
        break;
      }
      if (res) {
        grn_id id = grn_obj_register(ctx, db, name, name_size);
        res->header.flags |= GRN_OBJ_CUSTOM_NAME;
        res->header.domain = GRN_ID_NIL; /* unknown */
        DB_OBJ(res)->range = GRN_ID_NIL; /* unknown */
        grn_db_obj_init(ctx, db, id, DB_OBJ(res));
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT, "path is missing");
    }
    GRN_API_RETURN(res);
  }
}

grn_id
grn_table_lcp_search(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size)
{
  grn_id id = GRN_ID_NIL;
  GRN_API_ENTER;
  switch (table->header.type) {
  case GRN_TABLE_PAT_KEY :
    {
      grn_pat *pat = (grn_pat *)table;
      WITH_NORMALIZE(pat, key, key_size, {
        id = grn_pat_lcp_search(ctx, pat, key, key_size);
      });
    }
    break;
  case GRN_TABLE_DAT_KEY :
    {
      grn_dat *dat = (grn_dat *)table;
      WITH_NORMALIZE(dat, key, key_size, {
        id = grn_dat_lcp_search(ctx, dat, key, key_size);
      });
    }
    break;
  case GRN_TABLE_HASH_KEY :
    {
      grn_hash *hash = (grn_hash *)table;
      WITH_NORMALIZE(hash, key, key_size, {
        id = grn_hash_get(ctx, hash, key, key_size, NULL);
      });
    }
    break;
  }
  GRN_API_RETURN(id);
}

grn_obj *
grn_obj_default_set_value_hook(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  if (!pctx) {
    ERR(GRN_INVALID_ARGUMENT, "default_set_value_hook failed");
  } else {
    grn_obj *flags = grn_ctx_pop(ctx);
    grn_obj *newvalue = grn_ctx_pop(ctx);
    grn_obj *oldvalue = grn_ctx_pop(ctx);
    grn_obj *id = grn_ctx_pop(ctx);
    grn_hook *h = pctx->currh;
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(h);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    int section = data->section;
    if (flags) { /* todo */ }
    if (target) {
      switch (target->header.type) {
      case GRN_COLUMN_INDEX :
        grn_ii_column_update(ctx, (grn_ii *)target,
                             GRN_UINT32_VALUE(id),
                             section, oldvalue, newvalue, NULL);
      }
    }
  }
  return NULL;
}

grn_id
grn_table_add(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size, int *added)
{
  grn_id id = GRN_ID_NIL;
  GRN_API_ENTER;
  if (table) {
    int added_ = 0;
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      {
        grn_pat *pat = (grn_pat *)table;
        WITH_NORMALIZE(pat, key, key_size, {
          if (pat->io && !(pat->io->flags & GRN_IO_TEMPORARY)) {
            if (grn_io_lock(ctx, pat->io, grn_lock_timeout)) {
              id = GRN_ID_NIL;
            } else {
              id = grn_pat_add(ctx, pat, key, key_size, NULL, &added_);
              grn_io_unlock(pat->io);
            }
          } else {
            id = grn_pat_add(ctx, pat, key, key_size, NULL, &added_);
          }
        });
        if (added) { *added = added_; }
      }
      break;
    case GRN_TABLE_DAT_KEY :
      {
        grn_dat *dat = (grn_dat *)table;
        WITH_NORMALIZE(dat, key, key_size, {
          if (dat->io && !(dat->io->flags & GRN_IO_TEMPORARY)) {
            if (grn_io_lock(ctx, dat->io, grn_lock_timeout)) {
              id = GRN_ID_NIL;
            } else {
              id = grn_dat_add(ctx, dat, key, key_size, NULL, &added_);
              grn_io_unlock(dat->io);
            }
          } else {
            id = grn_dat_add(ctx, dat, key, key_size, NULL, &added_);
          }
        });
        if (added) { *added = added_; }
      }
      break;
    case GRN_TABLE_HASH_KEY :
      {
        grn_hash *hash = (grn_hash *)table;
        WITH_NORMALIZE(hash, key, key_size, {
          if (hash->io && !(hash->io->flags & GRN_IO_TEMPORARY)) {
            if (grn_io_lock(ctx, hash->io, grn_lock_timeout)) {
              id = GRN_ID_NIL;
            } else {
              id = grn_hash_add(ctx, hash, key, key_size, NULL, &added_);
              grn_io_unlock(hash->io);
            }
          } else {
            id = grn_hash_add(ctx, hash, key, key_size, NULL, &added_);
          }
        });
        if (added) { *added = added_; }
      }
      break;
    case GRN_TABLE_NO_KEY :
      {
        grn_array *array = (grn_array *)table;
        if (array->io && !(array->io->flags & GRN_IO_TEMPORARY)) {
          if (grn_io_lock(ctx, array->io, grn_lock_timeout)) {
            id = GRN_ID_NIL;
          } else {
            id = grn_array_add(ctx, array, NULL);
            grn_io_unlock(array->io);
          }
        } else {
          id = grn_array_add(ctx, array, NULL);
        }
        added_ = id ? 1 : 0;
        if (added) { *added = added_; }
      }
      break;
    }
    if (added_) {
      grn_hook *hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT];
      if (hooks) {
        // todo : grn_proc_ctx_open()
        grn_obj id_, flags_, oldvalue_, value_;
        grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4, {{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0}}};
        GRN_UINT32_INIT(&id_, 0);
        GRN_UINT32_INIT(&flags_, 0);
        GRN_TEXT_INIT(&oldvalue_, 0);
        GRN_TEXT_INIT(&value_, GRN_OBJ_DO_SHALLOW_COPY);
        GRN_TEXT_SET_REF(&value_, key, key_size);
        GRN_UINT32_SET(ctx, &id_, id);
        GRN_UINT32_SET(ctx, &flags_, GRN_OBJ_SET);
        while (hooks) {
          grn_ctx_push(ctx, &id_);
          grn_ctx_push(ctx, &oldvalue_);
          grn_ctx_push(ctx, &value_);
          grn_ctx_push(ctx, &flags_);
          pctx.caller = NULL;
          pctx.currh = hooks;
          if (hooks->proc) {
            hooks->proc->funcs[PROC_INIT](ctx, 1, &table, &pctx.user_data);
          } else {
            grn_obj_default_set_value_hook(ctx, 1, &table, &pctx.user_data);
          }
          if (ctx->rc) { break; }
          hooks = hooks->next;
          pctx.offset++;
        }
      }
    }
  }
  GRN_API_RETURN(id);
}

grn_id
grn_table_get_by_key(grn_ctx *ctx, grn_obj *table, grn_obj *key)
{
  grn_id id = GRN_ID_NIL;
  if (table->header.domain == key->header.domain) {
    id = grn_table_get(ctx, table, GRN_TEXT_VALUE(key), GRN_TEXT_LEN(key));
  } else {
    grn_rc rc;
    grn_obj buf;
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, table->header.domain);
    if ((rc = grn_obj_cast(ctx, key, &buf, GRN_TRUE))) {
      grn_obj *domain = grn_ctx_at(ctx, table->header.domain);
      ERR_CAST(table, domain, key);
    } else {
      id = grn_table_get(ctx, table, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
    }
    GRN_OBJ_FIN(ctx, &buf);
  }
  return id;
}

grn_id
grn_table_add_by_key(grn_ctx *ctx, grn_obj *table, grn_obj *key, int *added)
{
  grn_id id = GRN_ID_NIL;
  if (table->header.domain == key->header.domain) {
    id = grn_table_add(ctx, table, GRN_TEXT_VALUE(key), GRN_TEXT_LEN(key), added);
  } else {
    grn_rc rc;
    grn_obj buf;
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, table->header.domain);
    if ((rc = grn_obj_cast(ctx, key, &buf, GRN_TRUE))) {
      grn_obj *domain = grn_ctx_at(ctx, table->header.domain);
      ERR_CAST(table, domain, key);
    } else {
      id = grn_table_add(ctx, table, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf), added);
    }
    GRN_OBJ_FIN(ctx, &buf);
  }
  return id;
}

grn_id
grn_table_get(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size)
{
  grn_id id = GRN_ID_NIL;
  GRN_API_ENTER;
  if (table) {
    if (table->header.type == GRN_DB) {
      grn_db *db = (grn_db *)table;
      table = db->keys;
    }
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      WITH_NORMALIZE((grn_pat *)table, key, key_size, {
        id = grn_pat_get(ctx, (grn_pat *)table, key, key_size, NULL);
      });
      break;
    case GRN_TABLE_DAT_KEY :
      WITH_NORMALIZE((grn_dat *)table, key, key_size, {
        id = grn_dat_get(ctx, (grn_dat *)table, key, key_size, NULL);
      });
      break;
    case GRN_TABLE_HASH_KEY :
      WITH_NORMALIZE((grn_hash *)table, key, key_size, {
        id = grn_hash_get(ctx, (grn_hash *)table, key, key_size, NULL);
      });
      break;
    }
  }
  GRN_API_RETURN(id);
}

grn_id
grn_table_at(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_DB :
      {
        grn_db *db = (grn_db *)table;
        id = grn_table_at(ctx, db->keys, id);
      }
      break;
    case GRN_TABLE_PAT_KEY :
      id = grn_pat_at(ctx, (grn_pat *)table, id);
      break;
    case GRN_TABLE_DAT_KEY :
      id = grn_dat_at(ctx, (grn_dat *)table, id);
      break;
    case GRN_TABLE_HASH_KEY :
      id = grn_hash_at(ctx, (grn_hash *)table, id);
      break;
    case GRN_TABLE_NO_KEY :
      id = grn_array_at(ctx, (grn_array *)table, id);
      break;
    default :
      id = GRN_ID_NIL;
    }
  }
  GRN_API_RETURN(id);
}

inline static grn_id
grn_table_add_v_inline(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                       void **value, int *added)
{
  grn_id id = GRN_ID_NIL;
  if (!key || !key_size) { return GRN_ID_NIL; }
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      WITH_NORMALIZE((grn_pat *)table, key, key_size, {
        id = grn_pat_add(ctx, (grn_pat *)table, key, key_size, value, added);
      });
      break;
    case GRN_TABLE_DAT_KEY :
      WITH_NORMALIZE((grn_dat *)table, key, key_size, {
        id = grn_dat_add(ctx, (grn_dat *)table, key, key_size, value, added);
      });
      break;
    case GRN_TABLE_HASH_KEY :
      WITH_NORMALIZE((grn_hash *)table, key, key_size, {
        id = grn_hash_add(ctx, (grn_hash *)table, key, key_size, value, added);
      });
      break;
    case GRN_TABLE_NO_KEY :
      id = grn_array_add(ctx, (grn_array *)table, value);
      if (added) { *added = id ? 1 : 0; }
      break;
    }
  }
  return id;
}

grn_id
grn_table_add_v(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                void **value, int *added) {
  grn_id id;
  GRN_API_ENTER;
  id = grn_table_add_v_inline(ctx, table, key, key_size, value, added);
  GRN_API_RETURN(id);
}

grn_id
grn_table_get_v(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                void **value)
{
  grn_id id = GRN_ID_NIL;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      WITH_NORMALIZE((grn_pat *)table, key, key_size, {
        id = grn_pat_get(ctx, (grn_pat *)table, key, key_size, value);
      });
      break;
    case GRN_TABLE_DAT_KEY :
      WITH_NORMALIZE((grn_dat *)table, key, key_size, {
        id = grn_dat_get(ctx, (grn_dat *)table, key, key_size, value);
      });
      break;
    case GRN_TABLE_HASH_KEY :
      WITH_NORMALIZE((grn_hash *)table, key, key_size, {
        id = grn_hash_get(ctx, (grn_hash *)table, key, key_size, value);
      });
      break;
    }
  }
  GRN_API_RETURN(id);
}

int
grn_table_get_key(grn_ctx *ctx, grn_obj *table, grn_id id, void *keybuf, int buf_size)
{
  int r = 0;
  GRN_API_ENTER;
  if (table) {
    if (table->header.type == GRN_DB) {
      table = ((grn_db *)table)->keys;
    }
    switch (table->header.type) {
    case GRN_TABLE_HASH_KEY :
      r = grn_hash_get_key(ctx, (grn_hash *)table, id, keybuf, buf_size);
      break;
    case GRN_TABLE_PAT_KEY :
      r = grn_pat_get_key(ctx, (grn_pat *)table, id, keybuf, buf_size);
      break;
    case GRN_TABLE_DAT_KEY :
      r = grn_dat_get_key(ctx, (grn_dat *)table, id, keybuf, buf_size);
      break;
    case GRN_TABLE_NO_KEY :
      {
        grn_array *a = (grn_array *)table;
        if (a->obj.header.domain) {
          if ((unsigned int) buf_size >= a->value_size) {
            r = grn_array_get_value(ctx, a, id, keybuf);
          } else {
            r = a->value_size;
          }
        }
      }
      break;
    }
  }
  GRN_API_RETURN(r);
}

int
grn_table_get_key2(grn_ctx *ctx, grn_obj *table, grn_id id, grn_obj *bulk)
{
  int r = 0;
  GRN_API_ENTER;
  if (table) {
    if (table->header.type == GRN_DB) {
      table = ((grn_db *)table)->keys;
    }
    switch (table->header.type) {
    case GRN_TABLE_HASH_KEY :
      r = grn_hash_get_key2(ctx, (grn_hash *)table, id, bulk);
      break;
    case GRN_TABLE_PAT_KEY :
      r = grn_pat_get_key2(ctx, (grn_pat *)table, id, bulk);
      break;
    case GRN_TABLE_DAT_KEY :
      r = grn_dat_get_key2(ctx, (grn_dat *)table, id, bulk);
      break;
    case GRN_TABLE_NO_KEY :
      {
        grn_array *a = (grn_array *)table;
        if (a->obj.header.domain) {
          if (!grn_bulk_space(ctx, bulk, a->value_size)) {
            char *curr = GRN_BULK_CURR(bulk);
            r = grn_array_get_value(ctx, a, id, curr - a->value_size);
          }
        }
      }
      break;
    }
  }
  GRN_API_RETURN(r);
}

static grn_rc
grn_obj_clear_value(grn_ctx *ctx, grn_obj *obj, grn_id id)
{
  grn_rc rc = GRN_SUCCESS;
  if (GRN_DB_OBJP(obj)) {
    grn_obj buf;
    grn_id range = DB_OBJ(obj)->range;
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
    switch (obj->header.type) {
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_FIX_SIZE :
      rc = grn_obj_set_value(ctx, obj, id, &buf, GRN_OBJ_SET);
      break;
    }
    GRN_OBJ_FIN(ctx, &buf);
  }
  return rc;
}

static void
call_delete_hook(grn_ctx *ctx, grn_obj *table, grn_id rid, const void *key, unsigned int key_size)
{
  if (rid) {
    grn_hook *hooks = DB_OBJ(table)->hooks[GRN_HOOK_DELETE];
    if (hooks) {
      // todo : grn_proc_ctx_open()
      grn_obj id_, flags_, oldvalue_, value_;
      grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4,  {{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0}}};
      GRN_UINT32_INIT(&id_, 0);
      GRN_UINT32_INIT(&flags_, 0);
      GRN_TEXT_INIT(&oldvalue_, GRN_OBJ_DO_SHALLOW_COPY);
      GRN_TEXT_INIT(&value_, 0);
      GRN_TEXT_SET_REF(&oldvalue_, key, key_size);
      GRN_UINT32_SET(ctx, &id_, rid);
      GRN_UINT32_SET(ctx, &flags_, GRN_OBJ_SET);
      while (hooks) {
        grn_ctx_push(ctx, &id_);
        grn_ctx_push(ctx, &oldvalue_);
        grn_ctx_push(ctx, &value_);
        grn_ctx_push(ctx, &flags_);
        pctx.caller = NULL;
        pctx.currh = hooks;
        if (hooks->proc) {
          hooks->proc->funcs[PROC_INIT](ctx, 1, &table, &pctx.user_data);
        } else {
          grn_obj_default_set_value_hook(ctx, 1, &table, &pctx.user_data);
        }
        if (ctx->rc) { break; }
        hooks = hooks->next;
        pctx.offset++;
      }
    }
  }
}

static void
clear_column_values(grn_ctx *ctx, grn_obj *table, grn_id rid)
{
  if (rid) {
    grn_hash *cols;
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      if (grn_table_columns(ctx, table, "", 0, (grn_obj *)cols)) {
        grn_id *key;
        GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
          grn_obj *col = grn_ctx_at(ctx, *key);
          if (col) { grn_obj_clear_value(ctx, col, rid); }
        });
      }
      grn_hash_close(ctx, cols);
    }
  }
}

static void
delete_reference_records_in_index(grn_ctx *ctx, grn_obj *table, grn_id id,
                                  grn_obj *index)
{
  grn_ii *ii = (grn_ii *)index;
  grn_ii_cursor *ii_cursor = NULL;
  grn_posting *posting;
  grn_obj source_ids;
  unsigned int i, n_ids;
  grn_obj sources;
  grn_bool have_reference_source = GRN_FALSE;

  GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
  GRN_PTR_INIT(&sources, GRN_OBJ_VECTOR, 0);

  grn_obj_get_info(ctx, index, GRN_INFO_SOURCE, &source_ids);
  n_ids = GRN_BULK_VSIZE(&source_ids) / sizeof(grn_id);
  if (n_ids == 0) {
    goto exit;
  }

  for (i = 0; i < n_ids; i++) {
    grn_id source_id;
    grn_obj *source;

    source_id = GRN_UINT32_VALUE_AT(&source_ids, i);
    source = grn_ctx_at(ctx, source_id);
    if (grn_obj_get_range(ctx, source) == index->header.domain) {
      GRN_PTR_PUT(ctx, &sources, source);
      have_reference_source = GRN_TRUE;
    } else {
      grn_obj_unlink(ctx, source);
      GRN_PTR_PUT(ctx, &sources, NULL);
    }
  }

  if (!have_reference_source) {
    goto exit;
  }

  ii_cursor = grn_ii_cursor_open(ctx, ii, id, GRN_ID_NIL, GRN_ID_MAX,
                                 ii->n_elements, 0);
  if (!ii_cursor) {
    goto exit;
  }

  while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
    grn_obj *source = GRN_PTR_VALUE_AT(&sources, posting->sid - 1);
    if (!source) {
      continue;
    }
    switch (source->header.type) {
    case GRN_COLUMN_VAR_SIZE :
      switch (source->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
      case GRN_OBJ_COLUMN_SCALAR :
        grn_obj_clear_value(ctx, source, posting->rid);
        break;
      case GRN_OBJ_COLUMN_VECTOR :
        {
          grn_obj value;
          grn_obj new_value;
          GRN_TEXT_INIT(&value, 0);
          grn_obj_get_value(ctx, source, posting->rid, &value);
          if (value.header.type == GRN_UVECTOR) {
            int i, n_ids;
            GRN_RECORD_INIT(&new_value, GRN_OBJ_VECTOR, value.header.domain);
            n_ids = GRN_BULK_VSIZE(&value) / sizeof(grn_id);
            for (i = 0; i < n_ids; i++) {
              grn_id reference_id = GRN_RECORD_VALUE_AT(&value, i);
              if (reference_id == id) {
                continue;
              }
              GRN_RECORD_PUT(ctx, &new_value, reference_id);
            }
          } else {
            unsigned int i, n_elements;
            GRN_TEXT_INIT(&new_value, GRN_OBJ_VECTOR);
            n_elements = grn_vector_size(ctx, &value);
            for (i = 0; i < n_elements; i++) {
              const char *content;
              unsigned int content_length;
              unsigned int weight;
              grn_id domain;
              content_length =
                grn_vector_get_element(ctx, &value, i,
                                       &content, &weight, &domain);
              if (grn_table_get(ctx, table, content, content_length) == id) {
                continue;
              }
              grn_vector_add_element(ctx, &new_value, content, content_length,
                                     weight, domain);
            }
          }
          grn_obj_set_value(ctx, source, posting->rid, &new_value,
                            GRN_OBJ_SET);
          GRN_OBJ_FIN(ctx, &new_value);
          GRN_OBJ_FIN(ctx, &value);
        }
        break;
      }
      break;
    case GRN_COLUMN_FIX_SIZE :
      grn_obj_clear_value(ctx, source, posting->rid);
      break;
    }
  }

exit:
  if (ii_cursor) {
    grn_ii_cursor_close(ctx, ii_cursor);
  }
  grn_obj_unlink(ctx, &source_ids);
  {
    int i, n_sources;
    n_sources = GRN_BULK_VSIZE(&sources) / sizeof(grn_obj *);
    for (i = 0; i < n_sources; i++) {
      grn_obj *source = GRN_PTR_VALUE_AT(&sources, i);
      grn_obj_unlink(ctx, source);
    }
    grn_obj_unlink(ctx, &sources);
  }
}

static grn_rc
delete_reference_records(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  grn_hash *cols;
  grn_id *key;

  cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                         GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
  if (!cols) {
    return ctx->rc;
  }

  if (!grn_table_columns(ctx, table, "", 0, (grn_obj *)cols)) {
    grn_hash_close(ctx, cols);
    return ctx->rc;
  }

  GRN_HASH_EACH(ctx, cols, tid, &key, NULL, NULL, {
    grn_obj *col = grn_ctx_at(ctx, *key);
    if (!col) {
      continue;
    }
    if (col->header.type != GRN_COLUMN_INDEX) {
      continue;
    }
    delete_reference_records_in_index(ctx, table, id, col);
    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  });

  grn_hash_close(ctx, cols);

  return ctx->rc;
}

static grn_rc
grn_table_delete_prepare(grn_ctx *ctx, grn_obj *table,
                         grn_id id, const void *key, unsigned int key_size)
{
  grn_rc rc;

  rc = delete_reference_records(ctx, table, id);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  call_delete_hook(ctx, table, id, key, key_size);
  clear_column_values(ctx, table, id);

  return rc;
}

grn_rc
grn_table_delete(grn_ctx *ctx, grn_obj *table, const void *key, unsigned int key_size)
{
  grn_id rid = GRN_ID_NIL;
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (table) {
    if (key && key_size) { rid = grn_table_get(ctx, table, key, key_size); }
    if (rid) {
      rc = grn_table_delete_prepare(ctx, table, rid, key, key_size);
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      switch (table->header.type) {
      case GRN_DB :
        /* todo : delete tables and columns from db */
        break;
      case GRN_TABLE_PAT_KEY :
        WITH_NORMALIZE((grn_pat *)table, key, key_size, {
          grn_pat *pat = (grn_pat *)table;
          if (pat->io && !(pat->io->flags & GRN_IO_TEMPORARY)) {
            if (!(rc = grn_io_lock(ctx, pat->io, grn_lock_timeout))) {
              rc = grn_pat_delete(ctx, pat, key, key_size, NULL);
              grn_io_unlock(pat->io);
            }
          } else {
            rc = grn_pat_delete(ctx, pat, key, key_size, NULL);
          }
        });
        break;
      case GRN_TABLE_DAT_KEY :
        WITH_NORMALIZE((grn_dat *)table, key, key_size, {
          grn_dat *dat = (grn_dat *)table;
          if (dat->io && !(dat->io->flags & GRN_IO_TEMPORARY)) {
            if (!(rc = grn_io_lock(ctx, dat->io, grn_lock_timeout))) {
              rc = grn_dat_delete(ctx, dat, key, key_size, NULL);
              grn_io_unlock(dat->io);
            }
          } else {
            rc = grn_dat_delete(ctx, dat, key, key_size, NULL);
          }
        });
        break;
      case GRN_TABLE_HASH_KEY :
        WITH_NORMALIZE((grn_hash *)table, key, key_size, {
          grn_hash *hash = (grn_hash *)table;
          if (hash->io && !(hash->io->flags & GRN_IO_TEMPORARY)) {
            if (!(rc = grn_io_lock(ctx, hash->io, grn_lock_timeout))) {
              rc = grn_hash_delete(ctx, hash, key, key_size, NULL);
              grn_io_unlock(hash->io);
            }
          } else {
            rc = grn_hash_delete(ctx, hash, key, key_size, NULL);
          }
        });
        break;
      }
      if (rc == GRN_SUCCESS) {
        grn_obj_touch(ctx, table, NULL);
      }
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
_grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id,
                       grn_table_delete_optarg *optarg)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  if (table) {
    if (id) {
      const void *key = NULL;
      unsigned int key_size = 0;

      if (table->header.type != GRN_TABLE_NO_KEY) {
        key = _grn_table_key(ctx, table, id, &key_size);
      }
      rc = grn_table_delete_prepare(ctx, table, id, key, key_size);
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      // todo : support optarg
      switch (table->header.type) {
      case GRN_TABLE_PAT_KEY :
        rc = grn_pat_delete_by_id(ctx, (grn_pat *)table, id, optarg);
        break;
      case GRN_TABLE_DAT_KEY :
        rc = grn_dat_delete_by_id(ctx, (grn_dat *)table, id, optarg);
        break;
      case GRN_TABLE_HASH_KEY :
        rc = grn_hash_delete_by_id(ctx, (grn_hash *)table, id, optarg);
        break;
      case GRN_TABLE_NO_KEY :
        rc = grn_array_delete_by_id(ctx, (grn_array *)table, id, optarg);
        break;
      }
    }
  }
exit :
  return rc;
}

grn_rc
grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  grn_rc rc;
  grn_io *io;
  GRN_API_ENTER;
  if ((io = grn_obj_get_io(ctx, table)) && !(io->flags & GRN_IO_TEMPORARY)) {
    if (!(rc = grn_io_lock(ctx, io, grn_lock_timeout))) {
      rc = _grn_table_delete_by_id(ctx, table, id, NULL);
      grn_io_unlock(io);
    }
  } else {
    rc = _grn_table_delete_by_id(ctx, table, id, NULL);
  }
  if (rc == GRN_SUCCESS) {
    grn_obj_touch(ctx, table, NULL);
  }
  GRN_API_RETURN(rc);
}

grn_rc grn_ja_truncate(grn_ctx *ctx, grn_ja *ja);
grn_rc grn_ra_truncate(grn_ctx *ctx, grn_ra *ra);

grn_rc
grn_column_truncate(grn_ctx *ctx, grn_obj *column)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (column) {
    grn_hook *hooks;
    switch (column->header.type) {
    case GRN_COLUMN_INDEX :
      rc = grn_ii_truncate(ctx, (grn_ii *)column);
      break;
    case GRN_COLUMN_VAR_SIZE :
      for (hooks = DB_OBJ(column)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
        grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_ja_truncate(ctx, (grn_ja *)column);
      break;
    case GRN_COLUMN_FIX_SIZE :
      for (hooks = DB_OBJ(column)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
        grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_ra_truncate(ctx, (grn_ra *)column);
      break;
    }
    if (rc == GRN_SUCCESS) {
      grn_obj_touch(ctx, column, NULL);
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_truncate(grn_ctx *ctx, grn_obj *table)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (table) {
    grn_hook *hooks;
    grn_hash *cols;
    grn_obj *tokenizer;
    grn_obj *normalizer;
    grn_obj token_filters;
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      if (grn_table_columns(ctx, table, "", 0, (grn_obj *)cols)) {
        grn_id *key;
        GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
          grn_obj *col = grn_ctx_at(ctx, *key);
          if (col) { grn_column_truncate(ctx, col); }
        });
      }
      grn_hash_close(ctx, cols);
    }
    if (table->header.type != GRN_TABLE_NO_KEY) {
      grn_table_get_info(ctx, table, NULL, NULL, &tokenizer, &normalizer, NULL);
      GRN_PTR_INIT(&token_filters, GRN_OBJ_VECTOR, GRN_ID_NIL);
      grn_obj_get_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
    }
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_pat_truncate(ctx, (grn_pat *)table);
      break;
    case GRN_TABLE_DAT_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_dat_truncate(ctx, (grn_dat *)table);
      break;
    case GRN_TABLE_HASH_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_hash_truncate(ctx, (grn_hash *)table);
      break;
    case GRN_TABLE_NO_KEY :
      rc = grn_array_truncate(ctx, (grn_array *)table);
      break;
    }
    if (table->header.type != GRN_TABLE_NO_KEY) {
      grn_obj_set_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
      grn_obj_set_info(ctx, table, GRN_INFO_NORMALIZER, normalizer);
      grn_obj_set_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
      GRN_OBJ_FIN(ctx, &token_filters);
    }
    if (rc == GRN_SUCCESS) {
      grn_obj_touch(ctx, table, NULL);
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_get_info(grn_ctx *ctx, grn_obj *table, grn_table_flags *flags,
                   grn_encoding *encoding, grn_obj **tokenizer,
                   grn_obj **normalizer,
                   grn_obj **token_filters)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      if (flags) { *flags = ((grn_pat *)table)->header->flags; }
      if (encoding) { *encoding = ((grn_pat *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_pat *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_pat *)table)->normalizer; }
      if (token_filters) { *token_filters = &(((grn_pat *)table)->token_filters); }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_DAT_KEY :
      if (flags) { *flags = ((grn_dat *)table)->header->flags; }
      if (encoding) { *encoding = ((grn_dat *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_dat *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_dat *)table)->normalizer; }
      if (token_filters) { *token_filters = &(((grn_dat *)table)->token_filters); }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_HASH_KEY :
      if (flags) { *flags = ((grn_hash *)table)->header.common->flags; }
      if (encoding) { *encoding = ((grn_hash *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_hash *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_hash *)table)->normalizer; }
      if (token_filters) { *token_filters = &(((grn_hash *)table)->token_filters); }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_NO_KEY :
      if (flags) { *flags = grn_array_get_flags(ctx, ((grn_array *)table)); }
      if (encoding) { *encoding = GRN_ENC_NONE; }
      if (tokenizer) { *tokenizer = NULL; }
      if (normalizer) { *normalizer = NULL; }
      if (token_filters) { *token_filters = NULL; }
      rc = GRN_SUCCESS;
      break;
    }
  }
  GRN_API_RETURN(rc);
}

unsigned int
grn_table_size(grn_ctx *ctx, grn_obj *table)
{
  unsigned int n = 0;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_DB :
      n = grn_table_size(ctx, ((grn_db *)table)->keys);
      break;
    case GRN_TABLE_PAT_KEY :
      n = grn_pat_size(ctx, (grn_pat *)table);
      break;
    case GRN_TABLE_DAT_KEY :
      n = grn_dat_size(ctx, (grn_dat *)table);
      break;
    case GRN_TABLE_HASH_KEY :
      n = grn_hash_size(ctx, (grn_hash *)table);
      break;
    case GRN_TABLE_NO_KEY :
      n = grn_array_size(ctx, (grn_array *)table);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "not supported");
      break;
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid table assigned");
  }
  GRN_API_RETURN(n);
}

inline static void
subrecs_push(byte *subrecs, int size, int n_subrecs, double score, void *body, int dir)
{
  byte *v;
  double *c2;
  int n = n_subrecs - 1, n2;
  while (n) {
    n2 = (n - 1) >> 1;
    c2 = GRN_RSET_SUBRECS_NTH(subrecs,size,n2);
    if (GRN_RSET_SUBRECS_CMP(score, *c2, dir) >= 0) { break; }
    GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
    n = n2;
  }
  v = subrecs + n * (GRN_RSET_SCORE_SIZE + size);
  *((double *)v) = score;
  grn_memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

inline static void
subrecs_replace_min(byte *subrecs, int size, int n_subrecs, double score, void *body, int dir)
{
  byte *v;
  int n = 0, n1, n2;
  double *c1, *c2;
  for (;;) {
    n1 = n * 2 + 1;
    n2 = n1 + 1;
    c1 = n1 < n_subrecs ? GRN_RSET_SUBRECS_NTH(subrecs,size,n1) : NULL;
    c2 = n2 < n_subrecs ? GRN_RSET_SUBRECS_NTH(subrecs,size,n2) : NULL;
    if (c1 && GRN_RSET_SUBRECS_CMP(score, *c1, dir) > 0) {
      if (c2 &&
          GRN_RSET_SUBRECS_CMP(score, *c2, dir) > 0 &&
          GRN_RSET_SUBRECS_CMP(*c1, *c2, dir) > 0) {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
        n = n2;
      } else {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c1);
        n = n1;
      }
    } else {
      if (c2 && GRN_RSET_SUBRECS_CMP(score, *c2, dir) > 0) {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
        n = n2;
      } else {
        break;
      }
    }
  }
  v = subrecs + n * (GRN_RSET_SCORE_SIZE + size);
  grn_memcpy(v, &score, GRN_RSET_SCORE_SIZE);
  grn_memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

inline static void
grn_table_add_subrec_inline(grn_obj *table, grn_rset_recinfo *ri, double score,
                            grn_rset_posinfo *pi, int dir)
{
  if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
    int limit = DB_OBJ(table)->max_n_subrecs;
    ri->score += score;
    ri->n_subrecs += 1;
    if (limit) {
      int subrec_size = DB_OBJ(table)->subrec_size;
      int n_subrecs = GRN_RSET_N_SUBRECS(ri);
      if (pi) {
        byte *body = (byte *)pi + DB_OBJ(table)->subrec_offset;
        if (limit < n_subrecs) {
          if (GRN_RSET_SUBRECS_CMP(score, *((double *)(ri->subrecs)), dir) > 0) {
            subrecs_replace_min((byte *)ri->subrecs, subrec_size, limit, score, body, dir);
          }
        } else {
          subrecs_push((byte *)ri->subrecs, subrec_size, n_subrecs, score, body, dir);
        }
      }
    }
  }
}

void
grn_table_add_subrec(grn_obj *table, grn_rset_recinfo *ri, double score,
                     grn_rset_posinfo *pi, int dir)
{
  grn_table_add_subrec_inline(table, ri, score, pi, dir);
}

grn_table_cursor *
grn_table_cursor_open(grn_ctx *ctx, grn_obj *table,
                      const void *min, unsigned int min_size,
                      const void *max, unsigned int max_size,
                      int offset, int limit, int flags)
{
  grn_rc rc;
  grn_table_cursor *tc = NULL;
  unsigned int table_size;
  if (!table) { return tc; }
  GRN_API_ENTER;
  table_size = grn_table_size(ctx, table);
  if (flags & GRN_CURSOR_PREFIX) {
    if (offset < 0) {
      ERR(GRN_TOO_SMALL_OFFSET,
          "can't use negative offset with GRN_CURSOR_PREFIX: %d", offset);
    } else if (offset != 0 && offset >= (int) table_size) {
      ERR(GRN_TOO_LARGE_OFFSET,
          "offset is not less than table size: offset:%d, table_size:%d",
          offset, table_size);
    } else {
      if (limit < -1) {
        ERR(GRN_TOO_SMALL_LIMIT,
            "can't use smaller limit than -1 with GRN_CURSOR_PREFIX: %d",
            limit);
      } else if (limit == -1) {
        limit = table_size;
      }
    }
  } else {
    rc = grn_normalize_offset_and_limit(ctx, table_size, &offset, &limit);
    if (rc) {
      ERR(rc, "grn_normalize_offset_and_limit failed");
    }
  }
  if (!ctx->rc) {
    if (table->header.type == GRN_DB) { table = ((grn_db *)table)->keys; }
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      {
        grn_pat *pat = (grn_pat *)table;
        WITH_NORMALIZE(pat, min, min_size, {
          WITH_NORMALIZE(pat, max, max_size, {
            grn_pat_cursor *pat_cursor;
            pat_cursor = grn_pat_cursor_open(ctx, pat,
                                             min, min_size,
                                             max, max_size,
                                             offset, limit, flags);
            tc = (grn_table_cursor *)pat_cursor;
          });
        });
      }
      break;
    case GRN_TABLE_DAT_KEY :
      {
        grn_dat *dat = (grn_dat *)table;
        WITH_NORMALIZE(dat, min, min_size, {
          WITH_NORMALIZE(dat, max, max_size, {
            grn_dat_cursor *dat_cursor;
            dat_cursor = grn_dat_cursor_open(ctx, dat,
                                             min, min_size,
                                             max, max_size,
                                             offset, limit, flags);
            tc = (grn_table_cursor *)dat_cursor;
          });
        });
      }
      break;
    case GRN_TABLE_HASH_KEY :
      {
        grn_hash *hash = (grn_hash *)table;
        WITH_NORMALIZE(hash, min, min_size, {
          WITH_NORMALIZE(hash, max, max_size, {
            grn_hash_cursor *hash_cursor;
            hash_cursor = grn_hash_cursor_open(ctx, hash,
                                               min, min_size,
                                               max, max_size,
                                               offset, limit, flags);
            tc = (grn_table_cursor *)hash_cursor;
          });
        });
      }
      break;
    case GRN_TABLE_NO_KEY :
      tc = (grn_table_cursor *)grn_array_cursor_open(ctx, (grn_array *)table,
                                                     GRN_ID_NIL, GRN_ID_NIL,
                                                     offset, limit, flags);
      break;
    }
  }
  if (tc) {
    grn_id id = grn_obj_register(ctx, ctx->impl->db, NULL, 0);
    DB_OBJ(tc)->header.domain = GRN_ID_NIL;
    DB_OBJ(tc)->range = GRN_ID_NIL;
    grn_db_obj_init(ctx, ctx->impl->db, id, DB_OBJ(tc));
  }
  GRN_API_RETURN(tc);
}

grn_table_cursor *
grn_table_cursor_open_by_id(grn_ctx *ctx, grn_obj *table,
                            grn_id min, grn_id max, int flags)
{
  grn_table_cursor *tc = NULL;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      tc = (grn_table_cursor *)grn_pat_cursor_open(ctx, (grn_pat *)table,
                                                   NULL, 0, NULL, 0, 0, -1, flags);
      break;
    case GRN_TABLE_DAT_KEY :
      tc = (grn_table_cursor *)grn_dat_cursor_open(ctx, (grn_dat *)table,
                                                   NULL, 0, NULL, 0, 0, -1, flags);
      break;
    case GRN_TABLE_HASH_KEY :
      tc = (grn_table_cursor *)grn_hash_cursor_open(ctx, (grn_hash *)table,
                                                    NULL, 0, NULL, 0, 0, -1, flags);
      break;
    case GRN_TABLE_NO_KEY :
      tc = (grn_table_cursor *)grn_array_cursor_open(ctx, (grn_array *)table,
                                                     min, max, 0, -1, flags);
      break;
    }
  }
  GRN_API_RETURN(tc);
}

grn_rc
grn_table_cursor_close(grn_ctx *ctx, grn_table_cursor *tc)
{
  const char *tag = "[table][cursor][close]";
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (!tc) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "%s invalid cursor", tag);
  } else {
    {
      if (DB_OBJ(tc)->finalizer) {
        DB_OBJ(tc)->finalizer(ctx, 1, (grn_obj **)&tc, &DB_OBJ(tc)->user_data);
      }
      if (DB_OBJ(tc)->source) {
        GRN_FREE(DB_OBJ(tc)->source);
      }
      /*
      grn_hook_entry entry;
      for (entry = 0; entry < N_HOOK_ENTRIES; entry++) {
        grn_hook_free(ctx, DB_OBJ(tc)->hooks[entry]);
      }
      */
      grn_obj_delete_by_id(ctx, DB_OBJ(tc)->db, DB_OBJ(tc)->id, GRN_FALSE);
    }
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      grn_pat_cursor_close(ctx, (grn_pat_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      grn_dat_cursor_close(ctx, (grn_dat_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      grn_hash_cursor_close(ctx, (grn_hash_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      grn_array_cursor_close(ctx, (grn_array_cursor *)tc);
      break;
    default :
      rc = GRN_INVALID_ARGUMENT;
      ERR(rc, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

inline static grn_id
grn_table_cursor_next_inline(grn_ctx *ctx, grn_table_cursor *tc)
{
  const char *tag = "[table][cursor][next]";
  grn_id id = GRN_ID_NIL;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      id = grn_pat_cursor_next(ctx, (grn_pat_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      id = grn_dat_cursor_next(ctx, (grn_dat_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      id = grn_hash_cursor_next(ctx, (grn_hash_cursor *)tc);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      id = grn_array_cursor_next(ctx, (grn_array_cursor *)tc);
      break;
    case GRN_CURSOR_COLUMN_INDEX :
      {
        grn_posting *ip = grn_index_cursor_next(ctx, (grn_obj *)tc, NULL);
        if (ip) { id = ip->rid; }
      }
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  return id;
}

grn_id
grn_table_cursor_next(grn_ctx *ctx, grn_table_cursor *tc)
{
  grn_id id;
  GRN_API_ENTER;
  id = grn_table_cursor_next_inline(ctx, tc);
  GRN_API_RETURN(id);
}

int
grn_table_cursor_get_key(grn_ctx *ctx, grn_table_cursor *tc, void **key)
{
  const char *tag = "[table][cursor][get-key]";
  int len = 0;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      len = grn_pat_cursor_get_key(ctx, (grn_pat_cursor *)tc, key);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      len = grn_dat_cursor_get_key(ctx, (grn_dat_cursor *)tc, (const void **)key);
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      len = grn_hash_cursor_get_key(ctx, (grn_hash_cursor *)tc, key);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(len);
}

inline static int
grn_table_cursor_get_value_inline(grn_ctx *ctx, grn_table_cursor *tc, void **value)
{
  const char *tag = "[table][cursor][get-value]";
  int len = 0;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      len = grn_pat_cursor_get_value(ctx, (grn_pat_cursor *)tc, value);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      *value = NULL;
      len = 0;
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      len = grn_hash_cursor_get_value(ctx, (grn_hash_cursor *)tc, value);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      len = grn_array_cursor_get_value(ctx, (grn_array_cursor *)tc, value);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  return len;
}

int
grn_table_cursor_get_value(grn_ctx *ctx, grn_table_cursor *tc, void **value)
{
  int len;
  GRN_API_ENTER;
  len = grn_table_cursor_get_value_inline(ctx, tc, value);
  GRN_API_RETURN(len);
}

grn_rc
grn_table_cursor_set_value(grn_ctx *ctx, grn_table_cursor *tc,
                           const void *value, int flags)
{
  const char *tag = "[table][cursor][set-value]";
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      rc = grn_pat_cursor_set_value(ctx, (grn_pat_cursor *)tc, value, flags);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      rc = GRN_OPERATION_NOT_SUPPORTED;
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      rc = grn_hash_cursor_set_value(ctx, (grn_hash_cursor *)tc, value, flags);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      rc = grn_array_cursor_set_value(ctx, (grn_array_cursor *)tc, value, flags);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_cursor_delete(grn_ctx *ctx, grn_table_cursor *tc)
{
  const char *tag = "[table][cursor][delete]";
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    grn_id id;
    grn_obj *table;
    const void *key = NULL;
    unsigned int key_size = 0;
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      {
        grn_pat_cursor *pc = (grn_pat_cursor *)tc;
        id = pc->curr_rec;
        table = (grn_obj *)(pc->pat);
        key = _grn_pat_key(ctx, pc->pat, id, &key_size);
        rc = grn_table_delete_prepare(ctx, table, id, key, key_size);
        if (rc != GRN_SUCCESS) {
          goto exit;
        }
        rc = grn_pat_cursor_delete(ctx, pc, NULL);
      }
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      rc = GRN_OPERATION_NOT_SUPPORTED;
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      {
        grn_hash_cursor *hc = (grn_hash_cursor *)tc;
        id = hc->curr_rec;
        table = (grn_obj *)(hc->hash);
        key = _grn_hash_key(ctx, hc->hash, id, &key_size);
        rc = grn_table_delete_prepare(ctx, table, id, key, key_size);
        if (rc != GRN_SUCCESS) {
          goto exit;
        }
        rc = grn_hash_cursor_delete(ctx, hc, NULL);
      }
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      {
        grn_array_cursor *ac = (grn_array_cursor *)tc;
        id = ac->curr_rec;
        table = (grn_obj *)(ac->array);
        rc = grn_table_delete_prepare(ctx, table, id, key, key_size);
        if (rc != GRN_SUCCESS) {
          goto exit;
        }
        rc = grn_array_cursor_delete(ctx, ac, NULL);
      }
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_obj *
grn_table_cursor_table(grn_ctx *ctx, grn_table_cursor *tc)
{
  const char *tag = "[table][cursor][table]";
  grn_obj *obj = NULL;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "%s invalid cursor", tag);
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      obj = (grn_obj *)(((grn_pat_cursor *)tc)->pat);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      obj = (grn_obj *)(((grn_dat_cursor *)tc)->dat);
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      obj = (grn_obj *)(((grn_hash_cursor *)tc)->hash);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      obj = (grn_obj *)(((grn_array_cursor *)tc)->array);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "%s invalid type %d", tag, tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(obj);
}

typedef struct {
  grn_db_obj obj;
  grn_obj *index;
  grn_table_cursor *tc;
  grn_ii_cursor *iic;
  grn_id tid;
  grn_id rid_min;
  grn_id rid_max;
  int flags;
} grn_index_cursor;

grn_obj *
grn_index_cursor_open(grn_ctx *ctx, grn_table_cursor *tc,
                      grn_obj *index, grn_id rid_min, grn_id rid_max, int flags)
{
  grn_index_cursor *ic = NULL;
  GRN_API_ENTER;
  if (tc && (ic = GRN_MALLOCN(grn_index_cursor, 1))) {
    ic->tc = tc;
    ic->index = index;
    ic->iic = NULL;
    ic->tid = GRN_ID_NIL;
    ic->rid_min = rid_min;
    ic->rid_max = rid_max;
    ic->flags = flags;
    GRN_DB_OBJ_SET_TYPE(ic, GRN_CURSOR_COLUMN_INDEX);
    {
      grn_id id = grn_obj_register(ctx, ctx->impl->db, NULL, 0);
      DB_OBJ(ic)->header.domain = GRN_ID_NIL;
      DB_OBJ(ic)->range = GRN_ID_NIL;
      grn_db_obj_init(ctx, ctx->impl->db, id, DB_OBJ(ic));
    }
  }
  GRN_API_RETURN((grn_obj *)ic);
}

grn_posting *
grn_index_cursor_next(grn_ctx *ctx, grn_obj *c, grn_id *tid)
{
  grn_posting *ip = NULL;
  grn_index_cursor *ic = (grn_index_cursor *)c;
  GRN_API_ENTER;
  if (ic->iic) {
    if (ic->flags & GRN_OBJ_WITH_POSITION) {
      ip = grn_ii_cursor_next_pos(ctx, ic->iic);
      while (!ip && grn_ii_cursor_next(ctx, ic->iic)) {
        ip = grn_ii_cursor_next_pos(ctx, ic->iic);
        break;
      }
    } else {
      ip = grn_ii_cursor_next(ctx, ic->iic);
    }
  }
  if (!ip) {
    while ((ic->tid = grn_table_cursor_next_inline(ctx, ic->tc))) {
      grn_ii *ii = (grn_ii *)ic->index;
      if (ic->iic) { grn_ii_cursor_close(ctx, ic->iic); }
      if ((ic->iic = grn_ii_cursor_open(ctx, ii, ic->tid,
                                        ic->rid_min, ic->rid_max,
                                        ii->n_elements, ic->flags))) {
        ip = grn_ii_cursor_next(ctx, ic->iic);
        if (ip && ic->flags & GRN_OBJ_WITH_POSITION) {
          ip = grn_ii_cursor_next_pos(ctx, ic->iic);
        }
        if (ip) {
          break;
        }
      }
    }
  }
  if (tid) { *tid = ic->tid; }
  GRN_API_RETURN((grn_posting *)ip);
}

grn_rc
grn_table_search(grn_ctx *ctx, grn_obj *table, const void *key, uint32_t key_size,
                 grn_operator mode, grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  switch (table->header.type) {
  case GRN_TABLE_PAT_KEY :
    {
      grn_pat *pat = (grn_pat *)table;
      WITH_NORMALIZE(pat, key, key_size, {
        switch (mode) {
        case GRN_OP_EXACT :
          {
            grn_id id = grn_pat_get(ctx, pat, key, key_size, NULL);
            if (id) { grn_table_add(ctx, res, &id, sizeof(grn_id), NULL); }
          }
          // todo : support op;
          break;
        case GRN_OP_LCP :
          {
            grn_id id = grn_pat_lcp_search(ctx, pat, key, key_size);
            if (id) { grn_table_add(ctx, res, &id, sizeof(grn_id), NULL); }
          }
          // todo : support op;
          break;
        case GRN_OP_SUFFIX :
          rc = grn_pat_suffix_search(ctx, pat, key, key_size, (grn_hash *)res);
          // todo : support op;
          break;
        case GRN_OP_PREFIX :
          rc = grn_pat_prefix_search(ctx, pat, key, key_size, (grn_hash *)res);
          // todo : support op;
          break;
        case GRN_OP_TERM_EXTRACT :
          {
            int len;
            grn_id tid;
            const char *sp = key;
            const char *se = sp + key_size;
            for (; sp < se; sp += len) {
              if ((tid = grn_pat_lcp_search(ctx, pat, sp, se - sp))) {
                grn_table_add(ctx, res, &tid, sizeof(grn_id), NULL);
                /* todo : nsubrec++ if GRN_OBJ_TABLE_SUBSET assigned */
              }
              if (!(len = grn_charlen(ctx, sp, se))) { break; }
            }
          }
          // todo : support op;
          break;
        default :
          rc = GRN_INVALID_ARGUMENT;
          ERR(rc, "invalid mode %d", mode);
        }
      });
    }
    break;
  case GRN_TABLE_DAT_KEY :
    {
      grn_dat *dat = (grn_dat *)table;
      WITH_NORMALIZE(dat, key, key_size, {
        switch (mode) {
        case GRN_OP_EXACT :
          {
            grn_id id = grn_dat_get(ctx, dat, key, key_size, NULL);
            if (id) { grn_table_add(ctx, res, &id, sizeof(grn_id), NULL); }
          }
          break;
        case GRN_OP_PREFIX :
          {
            grn_dat_cursor *dc = grn_dat_cursor_open(ctx, dat, key, key_size, NULL, 0,
                                                     0, -1, GRN_CURSOR_PREFIX);
            if (dc) {
              grn_id id;
              while ((id = grn_dat_cursor_next(ctx, dc))) {
                grn_table_add(ctx, res, &id, sizeof(grn_id), NULL);
              }
              grn_dat_cursor_close(ctx, dc);
            }
          }
          break;
        case GRN_OP_LCP :
          {
            grn_id id = grn_dat_lcp_search(ctx, dat, key, key_size);
            if (id) { grn_table_add(ctx, res, &id, sizeof(grn_id), NULL); }
          }
          break;
        case GRN_OP_TERM_EXTRACT :
          {
            int len;
            grn_id tid;
            const char *sp = key;
            const char *se = sp + key_size;
            for (; sp < se; sp += len) {
              if ((tid = grn_dat_lcp_search(ctx, dat, sp, se - sp))) {
                grn_table_add(ctx, res, &tid, sizeof(grn_id), NULL);
                /* todo : nsubrec++ if GRN_OBJ_TABLE_SUBSET assigned */
              }
              if (!(len = grn_charlen(ctx, sp, se))) { break; }
            }
          }
          // todo : support op;
          break;
        default :
          rc = GRN_INVALID_ARGUMENT;
          ERR(rc, "invalid mode %d", mode);
        }
      });
    }
    break;
  case GRN_TABLE_HASH_KEY :
    {
      grn_hash *hash = (grn_hash *)table;
      grn_id id = GRN_ID_NIL;
      WITH_NORMALIZE(hash, key, key_size, {
        id = grn_hash_get(ctx, hash, key, key_size, NULL);
      });
      if (id) { grn_table_add(ctx, res, &id, sizeof(grn_id), NULL); }
    }
    break;
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_fuzzy_search(grn_ctx *ctx, grn_obj *table, const void *key, uint32_t key_size,
                       grn_fuzzy_search_optarg *args, grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  switch (table->header.type) {
  case GRN_TABLE_PAT_KEY :
    {
      grn_pat *pat = (grn_pat *)table;
      if (!grn_table_size(ctx, res) && op == GRN_OP_OR) {
        WITH_NORMALIZE(pat, key, key_size, {
          rc = grn_pat_fuzzy_search(ctx, pat, key, key_size,
                                    args, (grn_hash *)res);
        });
      } else {
        grn_obj *hash;
        hash = grn_table_create(ctx, NULL, 0, NULL,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                table, NULL);
        WITH_NORMALIZE(pat, key, key_size, {
          rc = grn_pat_fuzzy_search(ctx, pat, key, key_size,
                                    args, (grn_hash *)hash);
        });
        if (rc == GRN_SUCCESS) {
          rc = grn_table_setoperation(ctx, res, hash, res, op);
        }
        grn_obj_unlink(ctx, hash);
      }
    }
    break;
  default :
    rc = GRN_OPERATION_NOT_SUPPORTED;
    break;
  }
  GRN_API_RETURN(rc);
}

grn_id
grn_table_next(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  grn_id r = GRN_ID_NIL;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      r = grn_pat_next(ctx, (grn_pat *)table, id);
      break;
    case GRN_TABLE_DAT_KEY :
      r = grn_dat_next(ctx, (grn_dat *)table, id);
      break;
    case GRN_TABLE_HASH_KEY :
      r = grn_hash_next(ctx, (grn_hash *)table, id);
      break;
    case GRN_TABLE_NO_KEY :
      r = grn_array_next(ctx, (grn_array *)table, id);
      break;
    }
  }
  GRN_API_RETURN(r);
}

static grn_rc
grn_accessor_resolve_one_index_column(grn_ctx *ctx, grn_accessor *accessor,
                                      grn_obj *current_res, grn_obj **next_res)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *column = NULL;
  grn_id next_res_domain_id = GRN_ID_NIL;

  {
    grn_obj *index;
    grn_obj source_ids;
    unsigned int i, n_ids;

    index = accessor->obj;
    next_res_domain_id = index->header.domain;

    GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
    grn_obj_get_info(ctx, index, GRN_INFO_SOURCE, &source_ids);
    n_ids = GRN_BULK_VSIZE(&source_ids) / sizeof(grn_id);
    for (i = 0; i < n_ids; i++) {
      grn_id source_id;
      grn_obj *source;

      source_id = GRN_UINT32_VALUE_AT(&source_ids, i);
      source = grn_ctx_at(ctx, source_id);
      if (DB_OBJ(source)->range == next_res_domain_id) {
        column = source;
        break;
      }
      grn_obj_unlink(ctx, source);
    }

    if (!column) {
      return GRN_INVALID_ARGUMENT;
    }
  }

  {
    grn_rc rc;
    grn_obj *next_res_domain = grn_ctx_at(ctx, next_res_domain_id);
    *next_res = grn_table_create(ctx, NULL, 0, NULL,
                                 GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                 next_res_domain, NULL);
    rc = ctx->rc;
    grn_obj_unlink(ctx, next_res_domain);
    if (!*next_res) {
      return rc;
    }
  }

  {
    grn_obj_flags column_value_flags = 0;
    grn_obj column_value;
    grn_posting add_posting;
    grn_id *tid;
    grn_rset_recinfo *recinfo;

    if (column->header.type == GRN_COLUMN_VAR_SIZE) {
      column_value_flags |= GRN_OBJ_VECTOR;
    }
    GRN_VALUE_FIX_SIZE_INIT(&column_value,
                            column_value_flags,
                            next_res_domain_id);

    add_posting.sid = 0;
    add_posting.pos = 0;
    add_posting.weight = 0;

    GRN_HASH_EACH(ctx, (grn_hash *)current_res, id, &tid, NULL, &recinfo, {
      int i;
      int n_elements;

      add_posting.weight = recinfo->score - 1;

      GRN_BULK_REWIND(&column_value);
      grn_obj_get_value(ctx, column, *tid, &column_value);

      n_elements = GRN_BULK_VSIZE(&column_value) / sizeof(grn_id);
      for (i = 0; i < n_elements; i++) {
        add_posting.rid = GRN_RECORD_VALUE_AT(&column_value, i);
        rc = grn_ii_posting_add(ctx,
                                &add_posting,
                                (grn_hash *)*next_res,
                                GRN_OP_OR);
        if (rc != GRN_SUCCESS) {
          break;
        }
      }
      if (rc != GRN_SUCCESS) {
        break;
      }
    });

    GRN_OBJ_FIN(ctx, &column_value);
  }

  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, *next_res);
  }

  return rc;
}

static grn_rc
grn_accessor_resolve_one_table(grn_ctx *ctx, grn_accessor *accessor,
                               grn_obj *current_res, grn_obj **next_res)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *table;

  table = accessor->obj;
  *next_res = grn_table_create(ctx, NULL, 0, NULL,
                               GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                               table, NULL);
  if (!*next_res) {
    return ctx->rc;
  }

  grn_report_table(ctx,
                   "[accessor][resolve]",
                   "",
                   table);

  {
    grn_posting posting;

    memset(&posting, 0, sizeof(posting));
    GRN_HASH_EACH_BEGIN(ctx, (grn_hash *)current_res, cursor, id) {
      void *key;
      void *value;
      grn_id *record_id;
      grn_rset_recinfo *recinfo;
      grn_id next_record_id;

      grn_hash_cursor_get_key_value(ctx, cursor, &key, NULL, &value);
      record_id = key;
      recinfo = value;
      next_record_id = grn_table_get(ctx,
                                     table,
                                     record_id,
                                     sizeof(grn_id));
      if (next_record_id == GRN_ID_NIL) {
        continue;
      }

      posting.rid = next_record_id;
      posting.weight = recinfo->score;
      rc = grn_ii_posting_add(ctx,
                              &posting,
                              (grn_hash *)*next_res,
                              GRN_OP_OR);
      if (rc != GRN_SUCCESS) {
        break;
      }
    } GRN_HASH_EACH_END(ctx, cursor);
  }

  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, *next_res);
  }

  return rc;
}

static grn_rc
grn_accessor_resolve_one_data_column(grn_ctx *ctx, grn_accessor *accessor,
                                     grn_obj *current_res, grn_obj **next_res)
{
  grn_rc rc = GRN_SUCCESS;
  grn_index_datum index_datum;
  unsigned int n_index_data;
  grn_id next_res_domain_id = GRN_ID_NIL;

  n_index_data = grn_column_get_all_index_data(ctx,
                                               accessor->obj,
                                               &index_datum,
                                               1);
  if (n_index_data == 0) {
    return GRN_INVALID_ARGUMENT;
  }

  {
    grn_obj *lexicon;
    lexicon = grn_ctx_at(ctx, index_datum.index->header.domain);
    if (grn_obj_id(ctx, lexicon) != current_res->header.domain) {
      char index_name[GRN_TABLE_MAX_KEY_SIZE];
      int index_name_size;
      grn_obj *expected;
      char expected_name[GRN_TABLE_MAX_KEY_SIZE];
      int expected_name_size;

      index_name_size = grn_obj_name(ctx,
                                     index_datum.index,
                                     index_name,
                                     GRN_TABLE_MAX_KEY_SIZE);
      expected = grn_ctx_at(ctx, current_res->header.domain);
      expected_name_size = grn_obj_name(ctx,
                                        expected,
                                        expected_name,
                                        GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "[accessor][resolve][data-column] lexicon mismatch index: "
          "<%.*s> "
          "expected:<%.*s>",
          index_name_size,
          index_name,
          expected_name_size,
          expected_name);
      return ctx->rc;
    }
  }

  next_res_domain_id = DB_OBJ(index_datum.index)->range;

  grn_report_index(ctx,
                   "[accessor][resolve][data-column]",
                   "",
                   index_datum.index);
  {
    grn_rc rc;
    grn_obj *next_res_domain = grn_ctx_at(ctx, next_res_domain_id);
    *next_res = grn_table_create(ctx, NULL, 0, NULL,
                                 GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                 next_res_domain, NULL);
    rc = ctx->rc;
    grn_obj_unlink(ctx, next_res_domain);
    if (!*next_res) {
      return rc;
    }
  }

  {
    grn_id *tid;
    grn_rset_recinfo *recinfo;

    GRN_HASH_EACH(ctx, (grn_hash *)current_res, id, &tid, NULL, &recinfo, {
      grn_ii *ii = (grn_ii *)(index_datum.index);
      grn_ii_cursor *ii_cursor;
      grn_posting *posting;

      ii_cursor = grn_ii_cursor_open(ctx, ii, *tid,
                                     GRN_ID_NIL, GRN_ID_MAX,
                                     ii->n_elements,
                                     0);
      if (!ii_cursor) {
        continue;
      }

      while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
        grn_posting add_posting;

        if (index_datum.section > 0 && posting->sid != index_datum.section) {
          continue;
        }

        add_posting = *posting;
        add_posting.weight += recinfo->score - 1;
        rc = grn_ii_posting_add(ctx,
                                &add_posting,
                                (grn_hash *)*next_res,
                                GRN_OP_OR);
        if (rc != GRN_SUCCESS) {
          break;
        }
      }
      grn_ii_cursor_close(ctx, ii_cursor);

      if (rc != GRN_SUCCESS) {
        break;
      }
    });
  }

  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, *next_res);
  }

  return rc;
}

grn_rc
grn_accessor_resolve(grn_ctx *ctx, grn_obj *accessor, int deep,
                     grn_obj *base_res, grn_obj *res,
                     grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_accessor *a;
  grn_obj accessor_stack;
  int i, n_accessors;
  grn_obj *current_res = base_res;

  GRN_PTR_INIT(&accessor_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
  n_accessors = 0;
  for (a = (grn_accessor *)accessor; a; a = a->next) {
    if (deep == n_accessors) {
      break;
    }
    GRN_PTR_PUT(ctx, &accessor_stack, a);
    n_accessors++;
  }

  for (i = n_accessors; i > 0; i--) {
    grn_obj *next_res = NULL;

    a = (grn_accessor *)GRN_PTR_VALUE_AT(&accessor_stack, i - 1);
    if (a->obj->header.type == GRN_COLUMN_INDEX) {
      rc = grn_accessor_resolve_one_index_column(ctx, a,
                                                 current_res, &next_res);
    } else if (grn_obj_is_table(ctx, a->obj)) {
      rc = grn_accessor_resolve_one_table(ctx, a,
                                          current_res, &next_res);
    } else {
      rc = grn_accessor_resolve_one_data_column(ctx, a,
                                                current_res, &next_res);
    }

    if (current_res != base_res) {
      grn_obj_unlink(ctx, current_res);
    }

    if (rc != GRN_SUCCESS) {
      break;
    }

    current_res = next_res;
  }

  if (rc == GRN_SUCCESS && current_res != base_res) {
    grn_id *record_id;
    grn_rset_recinfo *recinfo;
    GRN_HASH_EACH(ctx, (grn_hash *)current_res, id, &record_id, NULL, &recinfo, {
      grn_posting posting;
      posting.rid = *record_id;
      posting.sid = 1;
      posting.pos = 0;
      posting.weight = recinfo->score - 1;
      grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
    });
    grn_obj_unlink(ctx, current_res);
    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  } else {
    if (rc == GRN_SUCCESS) {
      rc = GRN_INVALID_ARGUMENT;
    }
  }

  GRN_OBJ_FIN(ctx, &accessor_stack);
  return rc;
}

static inline void
grn_obj_search_index_report(grn_ctx *ctx, const char *tag, grn_obj *index)
{
  grn_report_index(ctx, "[object][search]", tag, index);
}

static inline grn_rc
grn_obj_search_accessor(grn_ctx *ctx, grn_obj *obj, grn_obj *query,
                        grn_obj *res, grn_operator op, grn_search_optarg *optarg)
{
  grn_rc rc = GRN_SUCCESS;
  grn_accessor *a;
  grn_obj *last_obj = NULL;
  int n_accessors;

  for (a = (grn_accessor *)obj; a; a = a->next) {
    if (!a->next) {
      last_obj = a->obj;
    }
  }
  n_accessors = 0;
  for (a = (grn_accessor *)obj; a; a = a->next) {
    n_accessors++;
    if (GRN_OBJ_INDEX_COLUMNP(a->obj)) {
      break;
    }
  }

  {
    grn_obj *index;
    grn_operator index_op = GRN_OP_MATCH;
    if (optarg && optarg->mode != GRN_OP_EXACT) {
      index_op = optarg->mode;
    }
    if (grn_column_index(ctx, last_obj, index_op, &index, 1, NULL) == 0) {
      rc = GRN_INVALID_ARGUMENT;
      goto exit;
    }

    if (n_accessors == 1) {
      rc = grn_obj_search(ctx, index, query, res, op, optarg);
    } else {
      grn_obj *base_res;
      grn_obj *range = grn_ctx_at(ctx, DB_OBJ(index)->range);
      base_res = grn_table_create(ctx, NULL, 0, NULL,
                                  GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                  range,
                                  NULL);
      rc = ctx->rc;
      grn_obj_unlink(ctx, range);
      if (!base_res) {
        goto exit;
      }
      if (optarg) {
        optarg->match_info.min = GRN_ID_NIL;
      }
      rc = grn_obj_search(ctx, index, query, base_res, GRN_OP_OR, optarg);
      if (rc != GRN_SUCCESS) {
        grn_obj_unlink(ctx, base_res);
        goto exit;
      }
      rc = grn_accessor_resolve(ctx, obj, n_accessors - 1, base_res, res, op);
      grn_obj_unlink(ctx, base_res);
    }
  }

exit :
  return rc;
}

static grn_rc
grn_obj_search_column_index_by_id(grn_ctx *ctx, grn_obj *obj,
                                  grn_id tid,
                                  grn_obj *res, grn_operator op,
                                  grn_search_optarg *optarg)
{
  grn_ii_cursor *c;

  grn_obj_search_index_report(ctx, "[id]", obj);

  c = grn_ii_cursor_open(ctx, (grn_ii *)obj, tid,
                         GRN_ID_NIL, GRN_ID_MAX, 1, 0);
  if (c) {
    grn_posting *pos;
    grn_hash *s = (grn_hash *)res;
    while ((pos = grn_ii_cursor_next(ctx, c))) {
      /* todo: support orgarg(op)
         res_add(ctx, s, (grn_rset_posinfo *) pos,
         get_weight(ctx, s, pos->rid, pos->sid, wvm, optarg), op);
      */
      grn_hash_add(ctx, s, pos, s->key_size, NULL, NULL);
    }
    grn_ii_cursor_close(ctx, c);
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_obj_search_column_index_by_key(grn_ctx *ctx, grn_obj *obj,
                                   grn_obj *query,
                                   grn_obj *res, grn_operator op,
                                   grn_search_optarg *optarg)
{
  grn_rc rc;
  unsigned int key_type = GRN_ID_NIL;
  const char *key;
  unsigned int key_len;
  grn_obj *table;
  grn_obj casted_query;
  grn_bool need_cast = GRN_FALSE;

  table = grn_ctx_at(ctx, obj->header.domain);
  if (table) {
    key_type = table->header.domain;
    need_cast = (query->header.domain != key_type);
    grn_obj_unlink(ctx, table);
  }
  if (need_cast) {
    GRN_OBJ_INIT(&casted_query, GRN_BULK, 0, key_type);
    rc = grn_obj_cast(ctx, query, &casted_query, GRN_FALSE);
    if (rc == GRN_SUCCESS) {
      key = GRN_BULK_HEAD(&casted_query);
      key_len = GRN_BULK_VSIZE(&casted_query);
    }
  } else {
    rc = GRN_SUCCESS;
    key = GRN_BULK_HEAD(query);
    key_len = GRN_BULK_VSIZE(query);
  }
  if (rc == GRN_SUCCESS) {
    if (grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
      const char *tag;
      if (optarg) {
        switch (optarg->mode) {
        case GRN_OP_MATCH :
          tag = "[key][match]";
          break;
        case GRN_OP_EXACT :
          tag = "[key][exact]";
          break;
        case GRN_OP_NEAR :
          tag = "[key][near]";
          break;
        case GRN_OP_NEAR2 :
          tag = "[key][near2]";
          break;
        case GRN_OP_SIMILAR :
          tag = "[key][similar]";
          break;
        case GRN_OP_REGEXP :
          tag = "[key][regexp]";
          break;
        case GRN_OP_FUZZY :
          tag = "[key][fuzzy]";
          break;
        default :
          tag = "[key][unknown]";
          break;
        }
      } else {
        tag = "[key][exact]";
      }
      grn_obj_search_index_report(ctx, tag, obj);
    }
    rc = grn_ii_sel(ctx, (grn_ii *)obj, key, key_len,
                    (grn_hash *)res, op, optarg);
  }
  if (need_cast) {
    GRN_OBJ_FIN(ctx, &casted_query);
  }

  return rc;
}

static grn_rc
grn_obj_search_column_index(grn_ctx *ctx, grn_obj *obj, grn_obj *query,
                            grn_obj *res, grn_operator op,
                            grn_search_optarg *optarg)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;

  if (DB_OBJ(obj)->range == res->header.domain) {
    switch (query->header.type) {
    case GRN_BULK :
      if (query->header.domain == obj->header.domain &&
          GRN_BULK_VSIZE(query) == sizeof(grn_id)) {
        grn_id tid = GRN_RECORD_VALUE(query);
        rc = grn_obj_search_column_index_by_id(ctx, obj, tid, res, op, optarg);
      } else {
        rc = grn_obj_search_column_index_by_key(ctx, obj, query,
                                                res, op, optarg);
      }
      break;
    case GRN_QUERY :
      rc = GRN_FUNCTION_NOT_IMPLEMENTED;
      break;
    }
  }

  return rc;
}

grn_rc
grn_obj_search(grn_ctx *ctx, grn_obj *obj, grn_obj *query,
               grn_obj *res, grn_operator op, grn_search_optarg *optarg)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (GRN_ACCESSORP(obj)) {
    rc = grn_obj_search_accessor(ctx, obj, query, res, op, optarg);
  } else if (GRN_DB_OBJP(obj)) {
    switch (obj->header.type) {
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_HASH_KEY :
      {
        const void *key = GRN_BULK_HEAD(query);
        uint32_t key_size = GRN_BULK_VSIZE(query);
        grn_operator mode = optarg ? optarg->mode : GRN_OP_EXACT;
        if (key && key_size) {
          if (grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
            const char *tag;
            if (optarg) {
              switch (optarg->mode) {
              case GRN_OP_EXACT :
                tag = "[table][exact]";
                break;
              case GRN_OP_LCP :
                tag = "[table][lcp]";
                break;
              case GRN_OP_SUFFIX :
                tag = "[table][suffix]";
                break;
              case GRN_OP_PREFIX :
                tag = "[table][prefix]";
                break;
              case GRN_OP_TERM_EXTRACT :
                tag = "[table][term-extract]";
                break;
              case GRN_OP_FUZZY :
                tag = "[table][fuzzy]";
                break;
              default :
                tag = "[table][unknown]";
                break;
              }
            } else {
              tag = "[table][exact]";
            }
            grn_obj_search_index_report(ctx, tag, obj);
          }
          if (optarg && optarg->mode == GRN_OP_FUZZY) {
            rc = grn_table_fuzzy_search(ctx, obj, key, key_size,
                                        &(optarg->fuzzy), res, op);
          } else {
            rc = grn_table_search(ctx, obj, key, key_size, mode, res, op);
          }
        }
      }
      break;
    case GRN_COLUMN_INDEX :
      rc = grn_obj_search_column_index(ctx, obj, query, res, op, optarg);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

#define GRN_TABLE_GROUP_BY_KEY           0
#define GRN_TABLE_GROUP_BY_VALUE         1
#define GRN_TABLE_GROUP_BY_COLUMN_VALUE  2

#define GRN_TABLE_GROUP_FILTER_PREFIX    0
#define GRN_TABLE_GROUP_FILTER_SUFFIX    (1L<<2)

inline static void
grn_table_group_add_subrec(grn_ctx *ctx,
                           grn_obj *table,
                           grn_rset_recinfo *ri, double score,
                           grn_rset_posinfo *pi, int dir,
                           grn_obj *calc_target,
                           grn_obj *value_buffer)
{
  grn_table_group_flags flags;

  if (!(DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC)) {
    return;
  }

  grn_table_add_subrec_inline(table, ri, score, pi, dir);

  flags = DB_OBJ(table)->flags.group;

  if (!(flags & (GRN_TABLE_GROUP_CALC_MAX |
                 GRN_TABLE_GROUP_CALC_MIN |
                 GRN_TABLE_GROUP_CALC_SUM |
                 GRN_TABLE_GROUP_CALC_AVG))) {
    return;
  }

  GRN_BULK_REWIND(value_buffer);
  grn_obj_get_value(ctx, calc_target, pi->rid, value_buffer);
  grn_rset_recinfo_update_calc_values(ctx, ri, table, value_buffer);
}

static grn_bool
accelerated_table_group(grn_ctx *ctx, grn_obj *table, grn_obj *key,
                        grn_table_group_result *result)
{
  grn_obj *res = result->table;
  grn_obj *calc_target = result->calc_target;
  if (key->header.type == GRN_ACCESSOR) {
    grn_accessor *a = (grn_accessor *)key;
    if (a->action == GRN_ACCESSOR_GET_KEY &&
        a->next && a->next->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        a->next->obj && !a->next->next) {
      grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, key));
      int idp = GRN_OBJ_TABLEP(range);
      grn_table_cursor *tc;
      if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
        grn_bool processed = GRN_TRUE;
        grn_obj value_buffer;
        GRN_VOID_INIT(&value_buffer);
        switch (a->next->obj->header.type) {
        case GRN_COLUMN_FIX_SIZE :
          {
            grn_id id;
            grn_ra *ra = (grn_ra *)a->next->obj;
            unsigned int element_size = (ra)->header->element_size;
            grn_ra_cache cache;
            GRN_RA_CACHE_INIT(ra, &cache);
            while ((id = grn_table_cursor_next_inline(ctx, tc))) {
              void *v, *value;
              grn_id *id_;
              uint32_t key_size;
              grn_rset_recinfo *ri = NULL;
              if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
                grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
              }
              id_ = (grn_id *)_grn_table_key(ctx, table, id, &key_size);
              v = grn_ra_ref_cache(ctx, ra, *id_, &cache);
              if (idp && *((grn_id *)v) &&
                  grn_table_at(ctx, range, *((grn_id *)v)) == GRN_ID_NIL) {
                continue;
              }
              if ((!idp || *((grn_id *)v)) &&
                  grn_table_add_v_inline(ctx, res, v, element_size, &value, NULL)) {
                grn_table_group_add_subrec(ctx, res, value,
                                           ri ? ri->score : 0,
                                           (grn_rset_posinfo *)&id, 0,
                                           calc_target,
                                           &value_buffer);
              }
            }
            GRN_RA_CACHE_FIN(ra, &cache);
          }
          break;
        case GRN_COLUMN_VAR_SIZE :
          if (idp) { /* todo : support other type */
            grn_id id;
            grn_ja *ja = (grn_ja *)a->next->obj;
            while ((id = grn_table_cursor_next_inline(ctx, tc))) {
              grn_io_win jw;
              unsigned int len = 0;
              void *value;
              grn_id *v, *id_;
              uint32_t key_size;
              grn_rset_recinfo *ri = NULL;
              if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
                grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
              }
              id_ = (grn_id *)_grn_table_key(ctx, table, id, &key_size);
              if ((v = grn_ja_ref(ctx, ja, *id_, &jw, &len))) {
                while (len) {
                  if ((*v != GRN_ID_NIL) &&
                      grn_table_add_v_inline(ctx, res, v, sizeof(grn_id), &value, NULL)) {
                    grn_table_group_add_subrec(ctx, res, value,
                                               ri ? ri->score : 0,
                                               (grn_rset_posinfo *)&id, 0,
                                               calc_target,
                                               &value_buffer);
                  }
                  v++;
                  len -= sizeof(grn_id);
                }
                grn_ja_unref(ctx, &jw);
              }
            }
          } else {
            processed = GRN_FALSE;
          }
          break;
        default :
          processed = GRN_FALSE;
          break;
        }
        GRN_OBJ_FIN(ctx, &value_buffer);
        grn_table_cursor_close(ctx, tc);
        return processed;
      }
    }
  }
  return GRN_FALSE;
}

static void
grn_table_group_single_key_records(grn_ctx *ctx, grn_obj *table,
                                   grn_obj *key, grn_table_group_result *result)
{
  grn_obj bulk;
  grn_obj value_buffer;
  grn_table_cursor *tc;
  grn_obj *res = result->table;
  grn_obj *calc_target = result->calc_target;

  GRN_TEXT_INIT(&bulk, 0);
  GRN_VOID_INIT(&value_buffer);
  if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, key));
    int idp = GRN_OBJ_TABLEP(range);
    while ((id = grn_table_cursor_next_inline(ctx, tc))) {
      void *value;
      grn_rset_recinfo *ri = NULL;
      GRN_BULK_REWIND(&bulk);
      if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
        grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
      }
      grn_obj_get_value(ctx, key, id, &bulk);
      switch (bulk.header.type) {
      case GRN_UVECTOR :
        {
          grn_bool is_reference;
          unsigned int element_size;
          uint8_t *elements;
          int i, n_elements;

          is_reference = !grn_type_id_is_builtin(ctx, bulk.header.type);

          element_size = grn_uvector_element_size(ctx, &bulk);
          elements = GRN_BULK_HEAD(&bulk);
          n_elements = GRN_BULK_VSIZE(&bulk) / element_size;
          for (i = 0; i < n_elements; i++) {
            uint8_t *element = elements + (element_size * i);

            if (is_reference) {
              grn_id id = *((grn_id *)element);
              if (id == GRN_ID_NIL) {
                continue;
              }
            }

            if (!grn_table_add_v_inline(ctx, res, element, element_size,
                                        &value, NULL)) {
              continue;
            }

            grn_table_group_add_subrec(ctx, res, value,
                                       ri ? ri->score : 0,
                                       (grn_rset_posinfo *)&id, 0,
                                       calc_target,
                                       &value_buffer);
          }
        }
        break;
      case GRN_VECTOR :
        {
          unsigned int i, n_elements;
          n_elements = grn_vector_size(ctx, &bulk);
          for (i = 0; i < n_elements; i++) {
            const char *content;
            unsigned int content_length;
            content_length = grn_vector_get_element(ctx, &bulk, i,
                                                    &content, NULL, NULL);
            if (grn_table_add_v_inline(ctx, res,
                                       content, content_length,
                                       &value, NULL)) {
              grn_table_group_add_subrec(ctx, res, value,
                                         ri ? ri->score : 0,
                                         (grn_rset_posinfo *)&id, 0,
                                         calc_target,
                                         &value_buffer);
            }
          }
        }
        break;
      case GRN_BULK :
        {
          if ((!idp || *((grn_id *)GRN_BULK_HEAD(&bulk))) &&
              grn_table_add_v_inline(ctx, res,
                                     GRN_BULK_HEAD(&bulk), GRN_BULK_VSIZE(&bulk),
                                     &value, NULL)) {
            grn_table_group_add_subrec(ctx, res, value,
                                       ri ? ri->score : 0,
                                       (grn_rset_posinfo *)&id, 0,
                                       calc_target,
                                       &value_buffer);
          }
        }
        break;
      default :
        ERR(GRN_INVALID_ARGUMENT, "invalid column");
        break;
      }
    }
    grn_table_cursor_close(ctx, tc);
  }
  GRN_OBJ_FIN(ctx, &value_buffer);
  GRN_OBJ_FIN(ctx, &bulk);
}

#define GRN_TABLE_GROUP_ALL_NAME     "_all"
#define GRN_TABLE_GROUP_ALL_NAME_LEN (sizeof(GRN_TABLE_GROUP_ALL_NAME) - 1)

static void
grn_table_group_all_records(grn_ctx *ctx, grn_obj *table,
                            grn_table_group_result *result)
{
  grn_obj value_buffer;
  grn_table_cursor *tc;
  grn_obj *res = result->table;
  grn_obj *calc_target = result->calc_target;

  GRN_VOID_INIT(&value_buffer);
  if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    void *value;
    if (grn_table_add_v_inline(ctx, res,
                               GRN_TABLE_GROUP_ALL_NAME,
                               GRN_TABLE_GROUP_ALL_NAME_LEN,
                               &value, NULL)) {
      while ((id = grn_table_cursor_next_inline(ctx, tc))) {
        grn_rset_recinfo *ri = NULL;
        if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
          grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
        }
        grn_table_group_add_subrec(ctx, res, value,
                                   ri ? ri->score : 0,
                                   (grn_rset_posinfo *)&id, 0,
                                   calc_target,
                                   &value_buffer);
      }
    }
    grn_table_cursor_close(ctx, tc);
  }
  GRN_OBJ_FIN(ctx, &value_buffer);
}

grn_rc
grn_table_group_with_range_gap(grn_ctx *ctx, grn_obj *table,
                               grn_table_sort_key *group_key,
                               grn_obj *res, uint32_t range_gap)
{
  grn_obj *key = group_key->key;
  if (key->header.type == GRN_ACCESSOR) {
    grn_accessor *a = (grn_accessor *)key;
    if (a->action == GRN_ACCESSOR_GET_KEY &&
        a->next && a->next->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        a->next->obj && !a->next->next) {
      grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, key));
      int idp = GRN_OBJ_TABLEP(range);
      grn_table_cursor *tc;
      if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL,
                                      0, 0, -1, 0))) {
        switch (a->next->obj->header.type) {
        case GRN_COLUMN_FIX_SIZE :
          {
            grn_id id;
            grn_ra *ra = (grn_ra *)a->next->obj;
            unsigned int element_size = (ra)->header->element_size;
            grn_ra_cache cache;
            GRN_RA_CACHE_INIT(ra, &cache);
            while ((id = grn_table_cursor_next_inline(ctx, tc))) {
              void *v, *value;
              grn_id *id_;
              uint32_t key_size;
              grn_rset_recinfo *ri = NULL;
              if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
                grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
              }
              id_ = (grn_id *)_grn_table_key(ctx, table, id, &key_size);
              v = grn_ra_ref_cache(ctx, ra, *id_, &cache);
              if (idp && *((grn_id *)v) &&
                  grn_table_at(ctx, range, *((grn_id *)v)) == GRN_ID_NIL) {
                continue;
              }
              if ((!idp || *((grn_id *)v))) {
                grn_id id;
                if (element_size == sizeof(uint32_t)) {
                  uint32_t quantized = (*(uint32_t *)v);
                  quantized -= quantized % range_gap;
                  id = grn_table_add_v_inline(ctx, res, &quantized,
                                              element_size, &value, NULL);
                } else {
                  id = grn_table_add_v_inline(ctx, res, v,
                                              element_size, &value, NULL);
                }
                if (id) {
                  grn_table_add_subrec_inline(res, value,
                                              ri ? ri->score : 0,
                                              (grn_rset_posinfo *)&id, 0);
                }
              }
            }
            GRN_RA_CACHE_FIN(ra, &cache);
          }
          break;
        case GRN_COLUMN_VAR_SIZE :
          if (idp) { /* todo : support other type */
            grn_id id;
            grn_ja *ja = (grn_ja *)a->next->obj;
            while ((id = grn_table_cursor_next_inline(ctx, tc))) {
              grn_io_win jw;
              unsigned int len = 0;
              void *value;
              grn_id *v, *id_;
              uint32_t key_size;
              grn_rset_recinfo *ri = NULL;
              if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
                grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
              }
              id_ = (grn_id *)_grn_table_key(ctx, table, id, &key_size);
              if ((v = grn_ja_ref(ctx, ja, *id_, &jw, &len))) {
                while (len) {
                  if ((*v != GRN_ID_NIL) &&
                      grn_table_add_v_inline(ctx, res, v, sizeof(grn_id), &value, NULL)) {
                    grn_table_add_subrec_inline(res, value, ri ? ri->score : 0,
                                                (grn_rset_posinfo *)&id, 0);
                  }
                  v++;
                  len -= sizeof(grn_id);
                }
                grn_ja_unref(ctx, &jw);
              }
            }
          } else {
            return 0;
          }
          break;
        default :
          return 0;
        }
        grn_table_cursor_close(ctx, tc);
        GRN_TABLE_GROUPED_ON(res);
        return 1;
      }
    }
  }
  return 0;
}

static inline void
grn_table_group_multi_keys_add_record(grn_ctx *ctx,
                                      grn_table_sort_key *keys,
                                      int n_keys,
                                      grn_table_group_result *results,
                                      int n_results,
                                      grn_id id,
                                      grn_rset_recinfo *ri,
                                      grn_obj *vector,
                                      grn_obj *bulk)
{
  int r;
  grn_table_group_result *rp;

  for (r = 0, rp = results; r < n_results; r++, rp++) {
    void *value;
    int i;
    int end;

    if (rp->key_end > n_keys) {
      end = n_keys;
    } else {
      end = rp->key_end + 1;
    }
    GRN_BULK_REWIND(bulk);
    grn_text_benc(ctx, bulk, end - rp->key_begin);
    for (i = rp->key_begin; i < end; i++) {
      grn_section section = vector->u.v.sections[i];
      grn_text_benc(ctx, bulk, section.length);
    }
    {
      grn_obj *body = vector->u.v.body;
      if (body) {
        GRN_TEXT_PUT(ctx, bulk, GRN_BULK_HEAD(body), GRN_BULK_VSIZE(body));
      }
    }
    for (i = rp->key_begin; i < end; i++) {
      grn_section section = vector->u.v.sections[i];
      grn_text_benc(ctx, bulk, section.weight);
      grn_text_benc(ctx, bulk, section.domain);
    }

    // todo : cut off GRN_ID_NIL
    if (grn_table_add_v_inline(ctx, rp->table,
                               GRN_BULK_HEAD(bulk), GRN_BULK_VSIZE(bulk),
                               &value, NULL)) {
      grn_table_group_add_subrec(ctx, rp->table, value,
                                 ri ? ri->score : 0,
                                 (grn_rset_posinfo *)&id, 0,
                                 rp->calc_target,
                                 bulk);
    }
  }
}

static void
grn_table_group_multi_keys_scalar_records(grn_ctx *ctx,
                                          grn_obj *table,
                                          grn_table_sort_key *keys,
                                          int n_keys,
                                          grn_table_group_result *results,
                                          int n_results)
{
  grn_id id;
  grn_table_cursor *tc;
  grn_obj bulk;
  grn_obj vector;

  tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0);
  if (!tc) {
    return;
  }

  GRN_TEXT_INIT(&bulk, 0);
  GRN_OBJ_INIT(&vector, GRN_VECTOR, 0, GRN_DB_VOID);
  while ((id = grn_table_cursor_next_inline(ctx, tc))) {
    int k;
    grn_table_sort_key *kp;
    grn_rset_recinfo *ri = NULL;

    if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
      grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
    }

    GRN_BULK_REWIND(&vector);
    for (k = 0, kp = keys; k < n_keys; k++, kp++) {
      GRN_BULK_REWIND(&bulk);
      grn_obj_get_value(ctx, kp->key, id, &bulk);
      grn_vector_add_element(ctx, &vector,
                             GRN_BULK_HEAD(&bulk), GRN_BULK_VSIZE(&bulk),
                             0,
                             bulk.header.domain);
    }

    grn_table_group_multi_keys_add_record(ctx, keys, n_keys, results, n_results,
                                          id, ri, &vector, &bulk);
  }
  GRN_OBJ_FIN(ctx, &vector);
  GRN_OBJ_FIN(ctx, &bulk);
  grn_table_cursor_close(ctx, tc);
}

static inline void
grn_table_group_multi_keys_vector_record(grn_ctx *ctx,
                                         grn_table_sort_key *keys,
                                         grn_obj *key_buffers,
                                         int nth_key,
                                         int n_keys,
                                         grn_table_group_result *results,
                                         int n_results,
                                         grn_id id,
                                         grn_rset_recinfo *ri,
                                         grn_obj *vector,
                                         grn_obj *bulk)
{
  int k;
  grn_table_sort_key *kp;

  for (k = nth_key, kp = &(keys[nth_key]); k < n_keys; k++, kp++) {
    grn_obj *key_buffer = &(key_buffers[k]);
    switch (key_buffer->header.type) {
    case GRN_UVECTOR :
      {
        unsigned int n_vector_elements;
        grn_id domain;
        grn_id *ids;
        unsigned int i, n_ids;

        n_vector_elements = grn_vector_size(ctx, vector);
        domain = key_buffer->header.domain;
        ids = (grn_id *)GRN_BULK_HEAD(key_buffer);
        n_ids = GRN_BULK_VSIZE(key_buffer) / sizeof(grn_id);
        for (i = 0; i < n_ids; i++) {
          grn_id element_id = ids[i];
          grn_vector_add_element(ctx, vector,
                                 (const char *)(&element_id), sizeof(grn_id),
                                 0,
                                 domain);
          grn_table_group_multi_keys_vector_record(ctx,
                                                   keys, key_buffers,
                                                   k + 1, n_keys,
                                                   results, n_results,
                                                   id, ri, vector, bulk);
          while (grn_vector_size(ctx, vector) != n_vector_elements) {
            const char *content;
            grn_vector_pop_element(ctx, vector, &content, NULL, NULL);
          }
        }
        return;
      }
      break;
    case GRN_VECTOR :
      {
        unsigned int n_vector_elements;
        unsigned int i, n_key_elements;

        n_vector_elements = grn_vector_size(ctx, vector);
        n_key_elements = grn_vector_size(ctx, key_buffer);
        for (i = 0; i < n_key_elements; i++) {
          const char *content;
          unsigned int content_length;
          grn_id domain;
          content_length = grn_vector_get_element(ctx, key_buffer, i,
                                                  &content, NULL, &domain);
          grn_vector_add_element(ctx, vector,
                                 content, content_length,
                                 0,
                                 domain);
          grn_table_group_multi_keys_vector_record(ctx,
                                                   keys, key_buffers,
                                                   k + 1, n_keys,
                                                   results, n_results,
                                                   id, ri, vector, bulk);
          while (grn_vector_size(ctx, vector) != n_vector_elements) {
            grn_vector_pop_element(ctx, vector, &content, NULL, NULL);
          }
        }
        return;
      }
      break;
    default :
      grn_vector_add_element(ctx, vector,
                             GRN_BULK_HEAD(key_buffer),
                             GRN_BULK_VSIZE(key_buffer),
                             0,
                             key_buffer->header.domain);
    }
  }

  if (k == n_keys) {
    grn_table_group_multi_keys_add_record(ctx,
                                          keys, n_keys,
                                          results, n_results,
                                          id, ri, vector, bulk);
  }
}

static void
grn_table_group_multi_keys_vector_records(grn_ctx *ctx,
                                          grn_obj *table,
                                          grn_table_sort_key *keys,
                                          int n_keys,
                                          grn_table_group_result *results,
                                          int n_results)
{
  grn_id id;
  grn_table_cursor *tc;
  grn_obj bulk;
  grn_obj vector;
  grn_obj *key_buffers;
  int k;

  tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0);
  if (!tc) {
    return;
  }

  key_buffers = GRN_MALLOCN(grn_obj, n_keys);
  if (!key_buffers) {
    grn_table_cursor_close(ctx, tc);
    return;
  }

  GRN_TEXT_INIT(&bulk, 0);
  GRN_OBJ_INIT(&vector, GRN_VECTOR, 0, GRN_DB_VOID);
  for (k = 0; k < n_keys; k++) {
    GRN_VOID_INIT(&(key_buffers[k]));
  }
  while ((id = grn_table_cursor_next_inline(ctx, tc))) {
    grn_table_sort_key *kp;
    grn_rset_recinfo *ri = NULL;

    if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
      grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
    }

    for (k = 0, kp = keys; k < n_keys; k++, kp++) {
      grn_obj *key_buffer = &(key_buffers[k]);
      GRN_BULK_REWIND(key_buffer);
      grn_obj_get_value(ctx, kp->key, id, key_buffer);
    }

    GRN_BULK_REWIND(&vector);
    grn_table_group_multi_keys_vector_record(ctx,
                                             keys, key_buffers, 0, n_keys,
                                             results, n_results,
                                             id, ri, &vector, &bulk);
  }
  for (k = 0; k < n_keys; k++) {
    GRN_OBJ_FIN(ctx, &(key_buffers[k]));
  }
  GRN_FREE(key_buffers);
  GRN_OBJ_FIN(ctx, &vector);
  GRN_OBJ_FIN(ctx, &bulk);
  grn_table_cursor_close(ctx, tc);
}

grn_rc
grn_table_group(grn_ctx *ctx, grn_obj *table,
                grn_table_sort_key *keys, int n_keys,
                grn_table_group_result *results, int n_results)
{
  grn_rc rc = GRN_SUCCESS;
  grn_bool group_by_all_records = GRN_FALSE;
  if (n_keys == 0 && n_results == 1) {
    group_by_all_records = GRN_TRUE;
  } else if (!table || !n_keys || !n_results) {
    ERR(GRN_INVALID_ARGUMENT, "table or n_keys or n_results is void");
    return GRN_INVALID_ARGUMENT;
  }
  GRN_API_ENTER;
  {
    int k, r;
    grn_table_sort_key *kp;
    grn_table_group_result *rp;
    for (k = 0, kp = keys; k < n_keys; k++, kp++) {
      if ((kp->flags & GRN_TABLE_GROUP_BY_COLUMN_VALUE) && !kp->key) {
        ERR(GRN_INVALID_ARGUMENT, "column missing in (%d)", k);
        goto exit;
      }
    }
    for (r = 0, rp = results; r < n_results; r++, rp++) {
      if (!rp->table) {
        grn_table_flags flags;
        grn_obj *key_type = NULL;
        uint32_t additional_value_size;

        flags = GRN_TABLE_HASH_KEY|
          GRN_OBJ_WITH_SUBREC|
          GRN_OBJ_UNIT_USERDEF_DOCUMENT;
        if (group_by_all_records) {
          key_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
        } else if (n_keys == 1) {
          key_type = grn_ctx_at(ctx, grn_obj_get_range(ctx, keys[0].key));
        } else {
          flags |= GRN_OBJ_KEY_VAR_SIZE;
        }
        additional_value_size = grn_rset_recinfo_calc_values_size(ctx,
                                                                  rp->flags);
        rp->table = grn_table_create_with_max_n_subrecs(ctx, NULL, 0, NULL,
                                                        flags,
                                                        key_type, table,
                                                        rp->max_n_subrecs,
                                                        additional_value_size);
        if (key_type) {
          grn_obj_unlink(ctx, key_type);
        }
        if (!rp->table) {
          goto exit;
        }
        DB_OBJ(rp->table)->flags.group = rp->flags;
      }
    }
    if (group_by_all_records) {
      grn_table_group_all_records(ctx, table, results);
    } else if (n_keys == 1 && n_results == 1) {
      if (!accelerated_table_group(ctx, table, keys->key, results)) {
        grn_table_group_single_key_records(ctx, table, keys->key, results);
      }
    } else {
      grn_bool have_vector = GRN_FALSE;
      for (k = 0, kp = keys; k < n_keys; k++, kp++) {
        grn_id range_id;
        grn_obj_flags range_flags = 0;
        grn_obj_get_range_info(ctx, kp->key, &range_id, &range_flags);
        if (range_flags == GRN_OBJ_VECTOR) {
          have_vector = GRN_TRUE;
          break;
        }
      }
      if (have_vector) {
        grn_table_group_multi_keys_vector_records(ctx, table,
                                                  keys, n_keys,
                                                  results, n_results);
      } else {
        grn_table_group_multi_keys_scalar_records(ctx, table,
                                                  keys, n_keys,
                                                  results, n_results);
      }
    }
    for (r = 0, rp = results; r < n_results; r++, rp++) {
      GRN_TABLE_GROUPED_ON(rp->table);
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_setoperation(grn_ctx *ctx, grn_obj *table1, grn_obj *table2, grn_obj *res,
                       grn_operator op)
{
  void *key = NULL, *value1 = NULL, *value2 = NULL;
  uint32_t value_size = 0;
  uint32_t key_size = 0;
  grn_bool have_subrec;

  GRN_API_ENTER;
  if (!table1) {
    ERR(GRN_INVALID_ARGUMENT, "[table][setoperation] table1 is NULL");
    GRN_API_RETURN(ctx->rc);
  }
  if (!table2) {
    ERR(GRN_INVALID_ARGUMENT, "[table][setoperation] table2 is NULL");
    GRN_API_RETURN(ctx->rc);
  }
  if (!res) {
    ERR(GRN_INVALID_ARGUMENT, "[table][setoperation] result table is NULL");
    GRN_API_RETURN(ctx->rc);
  }

  if (table1 != res) {
    if (table2 == res) {
      grn_obj *t = table1;
      table1 = table2;
      table2 = t;
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][setoperation] table1 or table2 must be result table");
      GRN_API_RETURN(ctx->rc);
    }
  }
  have_subrec = ((DB_OBJ(table1)->header.flags & GRN_OBJ_WITH_SUBREC) &&
                 (DB_OBJ(table2)->header.flags & GRN_OBJ_WITH_SUBREC));
  switch (table1->header.type) {
  case GRN_TABLE_HASH_KEY :
    value_size = ((grn_hash *)table1)->value_size;
    break;
  case GRN_TABLE_PAT_KEY :
    value_size = ((grn_pat *)table1)->value_size;
    break;
  case GRN_TABLE_DAT_KEY :
    value_size = 0;
    break;
  case GRN_TABLE_NO_KEY :
    value_size = ((grn_array *)table1)->value_size;
    break;
  }
  switch (table2->header.type) {
  case GRN_TABLE_HASH_KEY :
    if (value_size < ((grn_hash *)table2)->value_size) {
      value_size = ((grn_hash *)table2)->value_size;
    }
    break;
  case GRN_TABLE_PAT_KEY :
    if (value_size < ((grn_pat *)table2)->value_size) {
      value_size = ((grn_pat *)table2)->value_size;
    }
    break;
  case GRN_TABLE_DAT_KEY :
    value_size = 0;
    break;
  case GRN_TABLE_NO_KEY :
    if (value_size < ((grn_array *)table2)->value_size) {
      value_size = ((grn_array *)table2)->value_size;
    }
    break;
  }
  switch (op) {
  case GRN_OP_OR :
    if (have_subrec) {
      int added;
      GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
        if (grn_table_add_v_inline(ctx, table1, key, key_size, &value1, &added)) {
          if (added) {
            grn_memcpy(value1, value2, value_size);
          } else {
            grn_rset_recinfo *ri1 = value1;
            grn_rset_recinfo *ri2 = value2;
            grn_table_add_subrec_inline(table1, ri1, ri2->score, NULL, 0);
          }
        }
      });
    } else {
      GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
        if (grn_table_add_v_inline(ctx, table1, key, key_size, &value1, NULL)) {
          grn_memcpy(value1, value2, value_size);
        }
      });
    }
    break;
  case GRN_OP_AND :
    if (have_subrec) {
      GRN_TABLE_EACH(ctx, table1, 0, 0, id, &key, &key_size, &value1, {
        if (grn_table_get_v(ctx, table2, key, key_size, &value2)) {
          grn_rset_recinfo *ri1 = value1;
          grn_rset_recinfo *ri2 = value2;
          ri1->score += ri2->score;
        } else {
          _grn_table_delete_by_id(ctx, table1, id, NULL);
        }
      });
    } else {
      GRN_TABLE_EACH(ctx, table1, 0, 0, id, &key, &key_size, &value1, {
        if (!grn_table_get_v(ctx, table2, key, key_size, &value2)) {
          _grn_table_delete_by_id(ctx, table1, id, NULL);
        }
      });
    }
    break;
  case GRN_OP_AND_NOT :
    GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
      grn_table_delete(ctx, table1, key, key_size);
    });
    break;
  case GRN_OP_ADJUST :
    if (have_subrec) {
      GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
        if (grn_table_get_v(ctx, table1, key, key_size, &value1)) {
          grn_rset_recinfo *ri1 = value1;
          grn_rset_recinfo *ri2 = value2;
          ri1->score += ri2->score;
        }
      });
    } else {
      GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
        if (grn_table_get_v(ctx, table1, key, key_size, &value1)) {
          grn_memcpy(value1, value2, value_size);
        }
      });
    }
    break;
  default :
    break;
  }
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_table_difference(grn_ctx *ctx, grn_obj *table1, grn_obj *table2,
                     grn_obj *res1, grn_obj *res2)
{
  void *key = NULL;
  uint32_t key_size = 0;
  if (table1 != res1 || table2 != res2) { return GRN_INVALID_ARGUMENT; }
  if (grn_table_size(ctx, table1) > grn_table_size(ctx, table2)) {
    GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, NULL, {
      grn_id id1;
      if ((id1 = grn_table_get(ctx, table1, key, key_size))) {
        _grn_table_delete_by_id(ctx, table1, id1, NULL);
        _grn_table_delete_by_id(ctx, table2, id, NULL);
      }
    });
  } else {
    GRN_TABLE_EACH(ctx, table1, 0, 0, id, &key, &key_size, NULL, {
      grn_id id2;
      if ((id2 = grn_table_get(ctx, table2, key, key_size))) {
        _grn_table_delete_by_id(ctx, table1, id, NULL);
        _grn_table_delete_by_id(ctx, table2, id2, NULL);
      }
    });
  }
  return GRN_SUCCESS;
}

static grn_obj *grn_obj_get_accessor(grn_ctx *ctx, grn_obj *obj,
                                     const char *name, unsigned int name_size);

static grn_obj *
grn_obj_column_(grn_ctx *ctx, grn_obj *table, const char *name, unsigned int name_size)
{
  grn_id table_id = DB_OBJ(table)->id;
  grn_obj *column = NULL;

  if (table_id & GRN_OBJ_TMP_OBJECT) {
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    void *value = NULL;
    grn_snprintf(column_name, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
                 "%u%c%.*s", table_id, GRN_DB_DELIMITER, name_size, name);
    grn_pat_get(ctx, ctx->impl->temporary_columns,
                column_name, strlen(column_name),
                &value);
    if (value) {
      column = *((grn_obj **)value);
    }
  } else {
    char buf[GRN_TABLE_MAX_KEY_SIZE];
    int len = grn_obj_name(ctx, table, buf, GRN_TABLE_MAX_KEY_SIZE);
    if (len) {
      buf[len++] = GRN_DB_DELIMITER;
      if (len + name_size <= GRN_TABLE_MAX_KEY_SIZE) {
        grn_memcpy(buf + len, name, name_size);
        column = grn_ctx_get(ctx, buf, len + name_size);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "name is too long");
      }
    }
  }

  return column;
}

grn_obj *
grn_obj_column(grn_ctx *ctx, grn_obj *table, const char *name, unsigned int name_size)
{
  grn_obj *column = NULL;
  GRN_API_ENTER;
  if (GRN_OBJ_TABLEP(table)) {
    if (grn_db_check_name(ctx, name, name_size) ||
        !(column = grn_obj_column_(ctx, table, name, name_size))) {
      column = grn_obj_get_accessor(ctx, table, name, name_size);
    }
  } else if (GRN_ACCESSORP(table)) {
    column = grn_obj_get_accessor(ctx, table, name, name_size);
  }
  GRN_API_RETURN(column);
}

int
grn_table_columns(grn_ctx *ctx, grn_obj *table, const char *name, unsigned int name_size,
                  grn_obj *res)
{
  int n = 0;
  grn_id id;

  GRN_API_ENTER;

  if (!GRN_OBJ_TABLEP(table)) {
    GRN_API_RETURN(n);
  }

  id = DB_OBJ(table)->id;

  if (id == GRN_ID_NIL) {
    GRN_API_RETURN(n);
  }

  if (id & GRN_OBJ_TMP_OBJECT) {
    char search_key[GRN_TABLE_MAX_KEY_SIZE];
    grn_pat_cursor *cursor;
    grn_snprintf(search_key, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
                 "%u%c%.*s", id, GRN_DB_DELIMITER, name_size, name);
    cursor = grn_pat_cursor_open(ctx, ctx->impl->temporary_columns,
                                 search_key, strlen(search_key),
                                 NULL, 0,
                                 0, -1, GRN_CURSOR_PREFIX);
    if (cursor) {
      grn_id column_id;
      while ((column_id = grn_pat_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
        column_id |= GRN_OBJ_TMP_OBJECT | GRN_OBJ_TMP_COLUMN;
        grn_hash_add(ctx, (grn_hash *)res,
                     &column_id, sizeof(grn_id),
                     NULL, NULL);
        n++;
      }
      grn_pat_cursor_close(ctx, cursor);
    }
  } else {
    grn_db *s = (grn_db *)DB_OBJ(table)->db;
    if (s->keys) {
      grn_obj bulk;
      GRN_TEXT_INIT(&bulk, 0);
      grn_table_get_key2(ctx, s->keys, id, &bulk);
      GRN_TEXT_PUTC(ctx, &bulk, GRN_DB_DELIMITER);
      grn_bulk_write(ctx, &bulk, name, name_size);
      grn_table_search(ctx, s->keys, GRN_BULK_HEAD(&bulk), GRN_BULK_VSIZE(&bulk),
                       GRN_OP_PREFIX, res, GRN_OP_OR);
      grn_obj_close(ctx, &bulk);
      n = grn_table_size(ctx, res);
    }
  }

  GRN_API_RETURN(n);
}

const char *
_grn_table_key(grn_ctx *ctx, grn_obj *table, grn_id id, uint32_t *key_size)
{
  GRN_ASSERT(table);
  if (table->header.type == GRN_DB) { table = ((grn_db *)table)->keys; }
  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY :
    return _grn_hash_key(ctx, (grn_hash *)table, id, key_size);
  case GRN_TABLE_PAT_KEY :
    return _grn_pat_key(ctx, (grn_pat *)table, id, key_size);
  case GRN_TABLE_DAT_KEY :
    return _grn_dat_key(ctx, (grn_dat *)table, id, key_size);
  case GRN_TABLE_NO_KEY :
    {
      grn_array *a = (grn_array *)table;
      const char *v;
      if (a->obj.header.domain && a->value_size &&
          (v = _grn_array_get_value(ctx, a, id))) {
        *key_size = a->value_size;
        return v;
      } else {
        *key_size = 0;
      }
    }
    break;
  }
  return NULL;
}

/* column */

grn_obj *
grn_column_create(grn_ctx *ctx, grn_obj *table,
                  const char *name, unsigned int name_size,
                  const char *path, grn_column_flags flags, grn_obj *type)
{
  grn_db *s;
  uint32_t value_size;
  grn_obj *db= NULL, *res = NULL;
  grn_id id = GRN_ID_NIL;
  grn_id range = GRN_ID_NIL;
  grn_id domain = GRN_ID_NIL;
  grn_bool is_persistent_table;
  char fullname[GRN_TABLE_MAX_KEY_SIZE];
  unsigned int fullname_size;
  char buffer[PATH_MAX];

  GRN_API_ENTER;
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT, "[column][create] table is missing");
    goto exit;
  }
  if (!type) {
    ERR(GRN_INVALID_ARGUMENT, "[column][create] type is missing");
    goto exit;
  }
  if (!name || !name_size) {
    ERR(GRN_INVALID_ARGUMENT, "[column][create] name is missing");
    goto exit;
  }
  db = DB_OBJ(table)->db;
  s = (grn_db *)db;
  if (!GRN_DB_P(s)) {
    int table_name_len;
    char table_name[GRN_TABLE_MAX_KEY_SIZE];
    table_name_len = grn_obj_name(ctx, table, table_name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] invalid db assigned: <%.*s>.<%.*s>",
        table_name_len, table_name, name_size, name);
    goto exit;
  }

  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[column][create]", name, name_size);
    goto exit;
  }

  domain = DB_OBJ(table)->id;
  is_persistent_table = !(domain & GRN_OBJ_TMP_OBJECT);

  if (!domain) {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "[column][create] [todo] table-less column isn't supported yet");
    goto exit;
  }

  {
    int table_name_len;
    if (is_persistent_table) {
      table_name_len = grn_table_get_key(ctx, s->keys, domain,
                                         fullname, GRN_TABLE_MAX_KEY_SIZE);
    } else {
      grn_snprintf(fullname, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
                   "%u", domain);
      table_name_len = strlen(fullname);
    }
    if (name_size + 1 + table_name_len > GRN_TABLE_MAX_KEY_SIZE) {
      ERR(GRN_INVALID_ARGUMENT,
          "[column][create] too long column name: required name_size(%d) < %d"
          ": <%.*s>.<%.*s>",
          name_size, GRN_TABLE_MAX_KEY_SIZE - 1 - table_name_len,
          table_name_len, fullname, name_size, name);
      goto exit;
    }
    fullname[table_name_len] = GRN_DB_DELIMITER;
    grn_memcpy(fullname + table_name_len + 1, name, name_size);
    fullname_size = table_name_len + 1 + name_size;
  }

  range = DB_OBJ(type)->id;
  switch (type->header.type) {
  case GRN_TYPE :
    {
      grn_db_obj *t = (grn_db_obj *)type;
      flags |= t->header.flags & ~GRN_OBJ_KEY_MASK;
      value_size = GRN_TYPE_SIZE(t);
    }
    break;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    value_size = sizeof(grn_id);
    break;
  default :
    /*
    if (type == grn_type_any) {
      value_size = sizeof(grn_id) + sizeof(grn_id);
    }
    */
    value_size = sizeof(grn_id);
  }

  if (is_persistent_table) {
    id = grn_obj_register(ctx, db, fullname, fullname_size);
    if (ERRP(ctx, GRN_ERROR)) { goto exit; }

    {
      uint32_t table_name_size = 0;
      const char *table_name;
      table_name = _grn_table_key(ctx, ctx->impl->db, domain, &table_name_size);
      GRN_LOG(ctx, GRN_LOG_NOTICE,
              "DDL:%u:column_create %.*s %.*s",
              id,
              table_name_size, table_name,
              name_size, name);
    }
  } else {
    int added;
    id = grn_pat_add(ctx, ctx->impl->temporary_columns,
                     fullname, fullname_size, NULL,
                     &added);
    if (!id) {
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[column][create][temporary] "
          "failed to register temporary column name: <%.*s>",
          fullname_size, fullname);
      goto exit;
    } else if (!added) {
      id = GRN_ID_NIL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[column][create][temporary] already used name was assigned: <%.*s>",
          fullname_size, fullname);
      goto exit;
    }
    id |= GRN_OBJ_TMP_OBJECT | GRN_OBJ_TMP_COLUMN;
  }

  if (is_persistent_table && flags & GRN_OBJ_PERSISTENT) {
    if (!path) {
      if (GRN_DB_PERSISTENT_P(db)) {
        grn_db_generate_pathname(ctx, db, id, buffer);
        path = buffer;
      } else {
        int table_name_len;
        char table_name[GRN_TABLE_MAX_KEY_SIZE];
        table_name_len = grn_obj_name(ctx, table, table_name,
                                      GRN_TABLE_MAX_KEY_SIZE);
        ERR(GRN_INVALID_ARGUMENT,
            "[column][create] path not assigned for persistent column"
            ": <%.*s>.<%.*s>",
            table_name_len, table_name, name_size, name);
        goto exit;
      }
    } else {
      flags |= GRN_OBJ_CUSTOM_NAME;
    }
  } else {
    if (path) {
      int table_name_len;
      char table_name[GRN_TABLE_MAX_KEY_SIZE];
      table_name_len = grn_obj_name(ctx, table, table_name,
                                    GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "[column][create] path assigned for temporary column"
          ": <%.*s>.<%.*s>",
          table_name_len, table_name, name_size, name);
      goto exit;
    }
  }
  switch (flags & GRN_OBJ_COLUMN_TYPE_MASK) {
  case GRN_OBJ_COLUMN_SCALAR :
    if ((flags & GRN_OBJ_KEY_VAR_SIZE) || value_size > sizeof(int64_t)) {
      res = (grn_obj *)grn_ja_create(ctx, path, value_size, flags);
    } else {
      res = (grn_obj *)grn_ra_create(ctx, path, value_size);
    }
    break;
  case GRN_OBJ_COLUMN_VECTOR :
    res = (grn_obj *)grn_ja_create(ctx, path, value_size * 30/*todo*/, flags);
    //todo : zlib support
    break;
  case GRN_OBJ_COLUMN_INDEX :
    res = (grn_obj *)grn_ii_create(ctx, path, table, flags); //todo : ii layout support
    break;
  }
  if (res) {
    DB_OBJ(res)->header.domain = domain;
    DB_OBJ(res)->header.impl_flags = 0;
    DB_OBJ(res)->range = range;
    DB_OBJ(res)->header.flags = flags;
    res->header.flags = flags;
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      _grn_obj_remove(ctx, res, GRN_FALSE);
      res = NULL;
    } else {
      grn_obj_touch(ctx, res, NULL);
    }
  }
exit :
  if (!res && id) { grn_obj_delete_by_id(ctx, db, id, GRN_TRUE); }
  GRN_API_RETURN(res);
}

grn_obj *
grn_column_open(grn_ctx *ctx, grn_obj *table,
                const char *name, unsigned int name_size,
                const char *path, grn_obj *type)
{
  grn_id domain;
  grn_obj *res = NULL;
  grn_db *s;
  char fullname[GRN_TABLE_MAX_KEY_SIZE];
  GRN_API_ENTER;
  if (!table || !type || !name || !name_size) {
    ERR(GRN_INVALID_ARGUMENT, "missing type or name");
    goto exit;
  }
  s = (grn_db *)DB_OBJ(table)->db;
  if (!GRN_DB_P(s)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    goto exit;
  }
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[column][open]", name, name_size);
    goto exit;
  }
  if ((domain = DB_OBJ(table)->id)) {
    int len = grn_table_get_key(ctx, s->keys, domain, fullname, GRN_TABLE_MAX_KEY_SIZE);
    if (name_size + 1 + len > GRN_TABLE_MAX_KEY_SIZE) {
      ERR(GRN_INVALID_ARGUMENT, "too long column name");
      goto exit;
    }
    fullname[len] = GRN_DB_DELIMITER;
    grn_memcpy(fullname + len + 1, name, name_size);
    name_size += len + 1;
  } else {
    ERR(GRN_INVALID_ARGUMENT, "todo : not supported yet");
    goto exit;
  }
  res = grn_ctx_get(ctx, fullname, name_size);
  if (res) {
    const char *path2 = grn_obj_path(ctx, res);
    if (path && (!path2 || strcmp(path, path2))) { goto exit; }
  } else if (path) {
    uint32_t dbtype = grn_io_detect_type(ctx, path);
    if (!dbtype) { goto exit; }
    switch (dbtype) {
    case GRN_COLUMN_VAR_SIZE :
      res = (grn_obj *)grn_ja_open(ctx, path);
      break;
    case GRN_COLUMN_FIX_SIZE :
      res = (grn_obj *)grn_ra_open(ctx, path);
      break;
    case GRN_COLUMN_INDEX :
      res = (grn_obj *)grn_ii_open(ctx, path, table);
      break;
    }
    if (res) {
      grn_id id = grn_obj_register(ctx, (grn_obj *)s, fullname, name_size);
      DB_OBJ(res)->header.domain = domain;
      DB_OBJ(res)->range = DB_OBJ(type)->id;
      res->header.flags |= GRN_OBJ_CUSTOM_NAME;
      grn_db_obj_init(ctx, (grn_obj *)s, id, DB_OBJ(res));
    }
  }
exit :
  GRN_API_RETURN(res);
}

/*
typedef struct {
  grn_id id;
  int flags;
} grn_column_set_value_arg;

static grn_rc
default_column_set_value(grn_ctx *ctx, grn_proc_ctx *pctx, grn_obj *in, grn_obj *out)
{
  grn_user_data *data = grn_proc_ctx_get_local_data(pctx);
  if (data) {
    grn_column_set_value_arg *arg = data->ptr;
    unsigned int value_size = in->u.p.size; //todo
    if (!pctx->obj) { return GRN_ID_NIL; }
    switch (pctx->obj->header.type) {
    case GRN_COLUMN_VAR_SIZE :
      return grn_ja_put(ctx, (grn_ja *)pctx->obj, arg->id,
                        in->u.p.ptr, value_size, 0, NULL); // todo type->flag
    case GRN_COLUMN_FIX_SIZE :
      if (((grn_ra *)pctx->obj)->header->element_size < value_size) {
        ERR(GRN_INVALID_ARGUMENT, "too long value (%d)", value_size);
        return GRN_INVALID_ARGUMENT;
      } else {
        void *v = grn_ra_ref(ctx, (grn_ra *)pctx->obj, arg->id);
        if (!v) {
          ERR(GRN_NO_MEMORY_AVAILABLE, "ra get failed");
          return GRN_NO_MEMORY_AVAILABLE;
        }
        grn_memcpy(v, in->u.p.ptr, value_size);
        grn_ra_unref(ctx, (grn_ra *)pctx->obj, arg->id);
      }
      break;
    case GRN_COLUMN_INDEX :
      // todo : how??
      break;
    }
    return GRN_SUCCESS;
  } else {
    ERR(GRN_OBJECT_CORRUPT, "grn_proc_ctx_get_local_data failed");
    return ctx->rc;
  }
}
*/

/**** grn_vector ****/

//#define VECTOR(obj) ((grn_vector *)obj)

/*
#define INITIAL_VECTOR_SIZE 256

int
grn_vector_delimit(grn_ctx *ctx, grn_obj *vector)
{
  grn_vector *v = VECTOR(vector);
  uint32_t *offsets;
  if (!(v->n_entries & (INITIAL_VECTOR_SIZE - 1))) {
    offsets = GRN_REALLOC(v->offsets, sizeof(uint32_t) *
                          (v->n_entries + INITIAL_VECTOR_SIZE));
    if (!offsets) { return -1; }
    v->offsets = offsets;
  }
  v->offsets[v->n_entries] = GRN_BULK_VSIZE(vector);
  return ++(v->n_entries);
}
*/

static unsigned int
grn_uvector_element_size_internal(grn_ctx *ctx, grn_obj *uvector)
{
  unsigned int element_size;

  if (IS_WEIGHT_UVECTOR(uvector)) {
    element_size = sizeof(weight_uvector_entry);
  } else {
    switch (uvector->header.domain) {
    case GRN_DB_BOOL :
      element_size = sizeof(grn_bool);
      break;
    case GRN_DB_INT8 :
      element_size = sizeof(int8_t);
      break;
    case GRN_DB_UINT8 :
      element_size = sizeof(uint8_t);
      break;
    case GRN_DB_INT16 :
      element_size = sizeof(int16_t);
      break;
    case GRN_DB_UINT16 :
      element_size = sizeof(uint16_t);
      break;
    case GRN_DB_INT32 :
      element_size = sizeof(int32_t);
      break;
    case GRN_DB_UINT32 :
      element_size = sizeof(uint32_t);
      break;
    case GRN_DB_INT64 :
      element_size = sizeof(int64_t);
      break;
    case GRN_DB_UINT64 :
      element_size = sizeof(uint64_t);
      break;
    case GRN_DB_FLOAT :
      element_size = sizeof(double);
      break;
    case GRN_DB_TIME :
      element_size = sizeof(int64_t);
      break;
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      element_size = sizeof(grn_geo_point);
      break;
    default :
      element_size = sizeof(grn_id);
      break;
    }
  }

  return element_size;
}

static unsigned int
grn_uvector_size_internal(grn_ctx *ctx, grn_obj *uvector)
{
  unsigned int element_size;

  element_size = grn_uvector_element_size_internal(ctx, uvector);
  return GRN_BULK_VSIZE(uvector) / element_size;
}

unsigned int
grn_vector_size(grn_ctx *ctx, grn_obj *vector)
{
  unsigned int size;
  if (!vector) {
    ERR(GRN_INVALID_ARGUMENT, "vector is null");
    return 0;
  }
  GRN_API_ENTER;
  switch (vector->header.type) {
  case GRN_BULK :
    size = GRN_BULK_VSIZE(vector);
    break;
  case GRN_UVECTOR :
    size = grn_uvector_size_internal(ctx, vector);
    break;
  case GRN_VECTOR :
    size = vector->u.v.n_sections;
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT, "not vector");
    size = 0;
    break;
  }
  GRN_API_RETURN(size);
}

static grn_obj *
grn_vector_body(grn_ctx *ctx, grn_obj *v)
{
  if (!v) {
    ERR(GRN_INVALID_ARGUMENT, "invalid argument");
    return NULL;
  }
  switch (v->header.type) {
  case GRN_VECTOR :
    if (!v->u.v.body) {
      v->u.v.body = grn_obj_open(ctx, GRN_BULK, 0, v->header.domain);
    }
    return v->u.v.body;
  case GRN_BULK :
  case GRN_UVECTOR :
    return v;
  default :
    return NULL;
  }
}

unsigned int
grn_vector_get_element(grn_ctx *ctx, grn_obj *vector,
                       unsigned int offset, const char **str,
                       unsigned int *weight, grn_id *domain)
{
  unsigned int length = 0;
  GRN_API_ENTER;
  if (!vector || vector->header.type != GRN_VECTOR) {
    ERR(GRN_INVALID_ARGUMENT, "invalid vector");
    goto exit;
  }
  if ((unsigned int) vector->u.v.n_sections <= offset) {
    ERR(GRN_RANGE_ERROR, "offset out of range");
    goto exit;
  }
  {
    grn_section *vp = &vector->u.v.sections[offset];
    grn_obj *body = grn_vector_body(ctx, vector);
    *str = GRN_BULK_HEAD(body) + vp->offset;
    if (weight) { *weight = vp->weight; }
    if (domain) { *domain = vp->domain; }
    length = vp->length;
  }
exit :
  GRN_API_RETURN(length);
}

unsigned int
grn_vector_pop_element(grn_ctx *ctx, grn_obj *vector,
                       const char **str, unsigned int *weight, grn_id *domain)
{
  unsigned int offset, length = 0;
  GRN_API_ENTER;
  if (!vector || vector->header.type != GRN_VECTOR) {
    ERR(GRN_INVALID_ARGUMENT, "invalid vector");
    goto exit;
  }
  if (!vector->u.v.n_sections) {
    ERR(GRN_RANGE_ERROR, "offset out of range");
    goto exit;
  }
  offset = --vector->u.v.n_sections;
  {
    grn_section *vp = &vector->u.v.sections[offset];
    grn_obj *body = grn_vector_body(ctx, vector);
    *str = GRN_BULK_HEAD(body) + vp->offset;
    if (weight) { *weight = vp->weight; }
    if (domain) { *domain = vp->domain; }
    length = vp->length;
    grn_bulk_truncate(ctx, body, vp->offset);
  }
exit :
  GRN_API_RETURN(length);
}

#define W_SECTIONS_UNIT 8
#define S_SECTIONS_UNIT (1 << W_SECTIONS_UNIT)
#define M_SECTIONS_UNIT (S_SECTIONS_UNIT - 1)

grn_rc
grn_vector_delimit(grn_ctx *ctx, grn_obj *v, unsigned int weight, grn_id domain)
{
  if (v->header.type != GRN_VECTOR) { return GRN_INVALID_ARGUMENT; }
  if (!(v->u.v.n_sections & M_SECTIONS_UNIT)) {
    grn_section *vp = GRN_REALLOC(v->u.v.sections, sizeof(grn_section) *
                                  (v->u.v.n_sections + S_SECTIONS_UNIT));
    if (!vp) { return GRN_NO_MEMORY_AVAILABLE; }
    v->u.v.sections = vp;
  }
  {
    grn_obj *body = grn_vector_body(ctx, v);
    grn_section *vp = &v->u.v.sections[v->u.v.n_sections];
    vp->offset = v->u.v.n_sections ? vp[-1].offset + vp[-1].length : 0;
    vp->length = GRN_BULK_VSIZE(body) - vp->offset;
    vp->weight = weight;
    vp->domain = domain;
  }
  v->u.v.n_sections++;
  return GRN_SUCCESS;
}

grn_rc
grn_vector_decode(grn_ctx *ctx, grn_obj *v, const char *data, uint32_t data_size)
{
  uint8_t *p = (uint8_t *)data;
  uint8_t *pe = p + data_size;
  uint32_t n, n0 = v->u.v.n_sections;
  GRN_B_DEC(n, p);
  if (((n0 + M_SECTIONS_UNIT) >> W_SECTIONS_UNIT) !=
      ((n0 + n + M_SECTIONS_UNIT) >> W_SECTIONS_UNIT)) {
    grn_section *vp = GRN_REALLOC(v->u.v.sections, sizeof(grn_section) *
                                  ((n0 + n + M_SECTIONS_UNIT) & ~M_SECTIONS_UNIT));
    if (!vp) { return GRN_NO_MEMORY_AVAILABLE; }
    v->u.v.sections = vp;
  }
  {
    grn_section *vp;
    grn_obj *body = grn_vector_body(ctx, v);
    uint32_t offset = GRN_BULK_VSIZE(body);
    uint32_t o = 0, l, i;
    for (i = n, vp = v->u.v.sections + n0; i; i--, vp++) {
      if (pe <= p) { return GRN_INVALID_ARGUMENT; }
      GRN_B_DEC(l, p);
      vp->length = l;
      vp->offset = offset + o;
      vp->weight = 0;
      vp->domain = 0;
      o += l;
    }
    if (pe < p + o) { return GRN_INVALID_ARGUMENT; }
    grn_bulk_write(ctx, body, (char *)p, o);
    p += o;
    if (p < pe) {
      for (i = n, vp = v->u.v.sections + n0; i; i--, vp++) {
        if (pe <= p) { return GRN_INVALID_ARGUMENT; }
        GRN_B_DEC(vp->weight, p);
        GRN_B_DEC(vp->domain, p);
      }
    }
  }
  v->u.v.n_sections += n;
  return GRN_SUCCESS;
}

grn_rc
grn_vector_add_element(grn_ctx *ctx, grn_obj *vector,
                       const char *str, unsigned int str_len,
                       unsigned int weight, grn_id domain)
{
  grn_obj *body;
  GRN_API_ENTER;
  if (!vector) {
    ERR(GRN_INVALID_ARGUMENT, "vector is null");
    goto exit;
  }
  if ((body = grn_vector_body(ctx, vector))) {
    grn_bulk_write(ctx, body, str, str_len);
    grn_vector_delimit(ctx, vector, weight, domain);
  }
exit :
  GRN_API_RETURN(ctx->rc);
}

/*
grn_obj *
grn_sections_to_vector(grn_ctx *ctx, grn_obj *sections)
{
  grn_obj *vector = grn_vector_open(ctx, 0);
  if (vector) {
    grn_section *vp;
    int i;
    for (i = sections->u.v.n_sections, vp = sections->u.v.sections; i; i--, vp++) {
      grn_text_benc(ctx, vector, vp->weight);
      grn_text_benc(ctx, vector, vp->domain);
      grn_bulk_write(ctx, vector, vp->str, vp->str_len);
      grn_vector_delimit(ctx, vector);
    }
  }
  return vector;
}

grn_obj *
grn_vector_to_sections(grn_ctx *ctx, grn_obj *vector, grn_obj *sections)
{
  if (!sections) {
    sections = grn_obj_open(ctx, GRN_VECTOR, GRN_OBJ_DO_SHALLOW_COPY, 0);
  }
  if (sections) {
    int i, n = grn_vector_size(ctx, vector);
    sections->u.v.src = vector;
    for (i = 0; i < n; i++) {
      unsigned int size;
      const uint8_t *pe, *p = (uint8_t *)grn_vector_fetch(ctx, vector, i, &size);
      if (p) {
        grn_id domain;
        unsigned int weight;
        pe = p + size;
        if (p < pe) {
          GRN_B_DEC(weight, p);
          if (p < pe) {
            GRN_B_DEC(domain, p);
            if (p <= pe) {
              grn_vector_add(ctx, sections, (char *)p, pe - p, weight, domain);
            }
          }
        }
      }
    }
  }
  return sections;
}
*/

/**** uvector ****/

unsigned int
grn_uvector_size(grn_ctx *ctx, grn_obj *uvector)
{
  unsigned int size;

  if (!uvector) {
    ERR(GRN_INVALID_ARGUMENT, "uvector must not be NULL");
    return 0;
  }

  if (uvector->header.type != GRN_UVECTOR) {
    grn_obj type_name;
    GRN_TEXT_INIT(&type_name, 0);
    grn_inspect_type(ctx, &type_name, uvector->header.type);
    ERR(GRN_INVALID_ARGUMENT, "must be GRN_UVECTOR: %.*s",
        (int)GRN_TEXT_LEN(&type_name), GRN_TEXT_VALUE(&type_name));
    GRN_OBJ_FIN(ctx, &type_name);
    return 0;
  }

  GRN_API_ENTER;
  size = grn_uvector_size_internal(ctx, uvector);
  GRN_API_RETURN(size);
}

unsigned int
grn_uvector_element_size(grn_ctx *ctx, grn_obj *uvector)
{
  unsigned int element_size;

  if (!uvector) {
    ERR(GRN_INVALID_ARGUMENT, "uvector must not be NULL");
    return 0;
  }

  if (uvector->header.type != GRN_UVECTOR) {
    grn_obj type_name;
    GRN_TEXT_INIT(&type_name, 0);
    grn_inspect_type(ctx, &type_name, uvector->header.type);
    ERR(GRN_INVALID_ARGUMENT, "must be GRN_UVECTOR: %.*s",
        (int)GRN_TEXT_LEN(&type_name), GRN_TEXT_VALUE(&type_name));
    GRN_OBJ_FIN(ctx, &type_name);
    return 0;
  }

  GRN_API_ENTER;
  element_size = grn_uvector_element_size_internal(ctx, uvector);
  GRN_API_RETURN(element_size);
}

grn_rc
grn_uvector_add_element(grn_ctx *ctx, grn_obj *uvector,
                        grn_id id, unsigned int weight)
{
  GRN_API_ENTER;
  if (!uvector) {
    ERR(GRN_INVALID_ARGUMENT, "uvector is null");
    goto exit;
  }
  if (IS_WEIGHT_UVECTOR(uvector)) {
    weight_uvector_entry entry;
    entry.id = id;
    entry.weight = weight;
    grn_bulk_write(ctx, uvector,
                   (const char *)&entry, sizeof(weight_uvector_entry));
  } else {
    grn_bulk_write(ctx, uvector,
                   (const char *)&id, sizeof(grn_id));
  }
exit :
  GRN_API_RETURN(ctx->rc);
}

grn_id
grn_uvector_get_element(grn_ctx *ctx, grn_obj *uvector,
                        unsigned int offset, unsigned int *weight)
{
  grn_id id = GRN_ID_NIL;

  GRN_API_ENTER;
  if (!uvector || uvector->header.type != GRN_UVECTOR) {
    ERR(GRN_INVALID_ARGUMENT, "invalid uvector");
    goto exit;
  }

  if (IS_WEIGHT_UVECTOR(uvector)) {
    const weight_uvector_entry *entry;
    const weight_uvector_entry *entries_start;
    const weight_uvector_entry *entries_end;

    entries_start = (const weight_uvector_entry *)GRN_BULK_HEAD(uvector);
    entries_end = (const weight_uvector_entry *)GRN_BULK_CURR(uvector);
    if (offset > entries_end - entries_start) {
      ERR(GRN_RANGE_ERROR, "offset out of range");
      goto exit;
    }

    entry = entries_start + offset;
    id = entry->id;
    if (weight) { *weight = entry->weight; }
  } else {
    const grn_id *ids_start;
    const grn_id *ids_end;

    ids_start = (const grn_id *)GRN_BULK_HEAD(uvector);
    ids_end = (const grn_id *)GRN_BULK_CURR(uvector);
    if (offset > ids_end - ids_start) {
      ERR(GRN_RANGE_ERROR, "offset out of range");
      goto exit;
    }
    id = ids_start[offset];
    if (weight) { *weight = 0; }
  }
exit :
  GRN_API_RETURN(id);
}

/**** accessor ****/

static grn_accessor *
accessor_new(grn_ctx *ctx)
{
  grn_accessor *res = GRN_MALLOCN(grn_accessor, 1);
  if (res) {
    res->header.type = GRN_ACCESSOR;
    res->header.impl_flags = GRN_OBJ_ALLOCATED;
    res->header.flags = 0;
    res->header.domain = GRN_ID_NIL;
    res->range = GRN_ID_NIL;
    res->action = GRN_ACCESSOR_VOID;
    res->offset = 0;
    res->obj = NULL;
    res->next = NULL;
  }
  return res;
}

inline static grn_bool
grn_obj_get_accessor_rset_value(grn_ctx *ctx, grn_obj *obj,
                                grn_accessor **res, uint8_t action)
{
  grn_bool succeeded = GRN_FALSE;
  grn_accessor **rp;

  for (rp = res; GRN_TRUE; rp = &(*rp)->next) {
    *rp = accessor_new(ctx);
    (*rp)->obj = obj;

#define CHECK_GROUP_CALC_FLAG(flag) do {   \
      if (GRN_TABLE_IS_GROUPED(obj)) {     \
        grn_table_group_flags flags;       \
        flags = DB_OBJ(obj)->flags.group;  \
        if (flags & flag) {                \
          succeeded = GRN_TRUE;            \
          (*rp)->action = action;          \
          goto exit;                       \
        }                                  \
      }                                    \
    } while(GRN_FALSE)
    switch (action) {
    case GRN_ACCESSOR_GET_SCORE :
      if (DB_OBJ(obj)->header.flags & GRN_OBJ_WITH_SUBREC) {
        (*rp)->action = action;
        succeeded = GRN_TRUE;
        goto exit;
      }
      break;
    case GRN_ACCESSOR_GET_MAX :
      CHECK_GROUP_CALC_FLAG(GRN_TABLE_GROUP_CALC_MAX);
      break;
    case GRN_ACCESSOR_GET_MIN :
      CHECK_GROUP_CALC_FLAG(GRN_TABLE_GROUP_CALC_MIN);
      break;
    case GRN_ACCESSOR_GET_SUM :
      CHECK_GROUP_CALC_FLAG(GRN_TABLE_GROUP_CALC_SUM);
      break;
    case GRN_ACCESSOR_GET_AVG :
      CHECK_GROUP_CALC_FLAG(GRN_TABLE_GROUP_CALC_AVG);
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      if (GRN_TABLE_IS_GROUPED(obj)) {
        (*rp)->action = action;
        succeeded = GRN_TRUE;
        goto exit;
      }
      break;
    }
#undef CHECK_GROUP_CALC_FLAG

    switch (obj->header.type) {
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_HASH_KEY :
      (*rp)->action = GRN_ACCESSOR_GET_KEY;
      break;
    case GRN_TABLE_NO_KEY :
      if (!obj->header.domain) {
        goto exit;
      }
      (*rp)->action = GRN_ACCESSOR_GET_VALUE;
      break;
    default :
      /* lookup failed */
      goto exit;
    }
    if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
      goto exit;
    }
  }

exit :
  if (!succeeded) {
    grn_obj_close(ctx, (grn_obj *)*res);
    *res = NULL;
  }

  return succeeded;
}

static grn_obj *
grn_obj_get_accessor(grn_ctx *ctx, grn_obj *obj, const char *name, unsigned int name_size)
{
  grn_accessor *res = NULL, **rp = NULL, **rp0 = NULL;
  grn_bool is_chained = GRN_FALSE;
  if (!obj) { return NULL; }
  GRN_API_ENTER;
  if (obj->header.type == GRN_ACCESSOR) {
    is_chained = GRN_TRUE;
    for (rp0 = (grn_accessor **)&obj; *rp0; rp0 = &(*rp0)->next) {
      res = *rp0;
    }
    switch (res->action) {
    case GRN_ACCESSOR_GET_KEY :
      obj = grn_ctx_at(ctx, res->obj->header.domain);
      break;
    case GRN_ACCESSOR_GET_VALUE :
    case GRN_ACCESSOR_GET_SCORE :
    case GRN_ACCESSOR_GET_NSUBRECS :
    case GRN_ACCESSOR_GET_MAX :
    case GRN_ACCESSOR_GET_MIN :
    case GRN_ACCESSOR_GET_SUM :
    case GRN_ACCESSOR_GET_AVG :
      obj = grn_ctx_at(ctx, DB_OBJ(res->obj)->range);
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      obj = grn_ctx_at(ctx, DB_OBJ(res->obj)->range);
      break;
    case GRN_ACCESSOR_LOOKUP :
      /* todo */
      break;
    case GRN_ACCESSOR_FUNCALL :
      /* todo */
      break;
    }
  }
  if (!obj) {
    res = NULL;
    goto exit;
  }
  {
    size_t len;
    const char *sp, *se = name + name_size;
    if (*name == GRN_DB_DELIMITER) { name++; }
    for (sp = name; (len = grn_charlen(ctx, sp, se)); sp += len) {
      if (*sp == GRN_DB_DELIMITER) { break; }
    }
    if (!(len = sp - name)) { goto exit; }
    if (*name == GRN_DB_PSEUDO_COLUMN_PREFIX) { /* pseudo column */
      int done = 0;
      if (len < 2) { goto exit; }
      switch (name[1]) {
      case 'k' : /* key */
        if (len != GRN_COLUMN_NAME_KEY_LEN ||
            memcmp(name, GRN_COLUMN_NAME_KEY, GRN_COLUMN_NAME_KEY_LEN)) {
          goto exit;
        }
        for (rp = &res; !done; rp = &(*rp)->next) {
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (GRN_TABLE_IS_MULTI_KEYS_GROUPED(obj)) {
            (*rp)->action = GRN_ACCESSOR_GET_KEY;
            done++;
            break;
          }
          if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
            grn_obj_close(ctx, (grn_obj *)res);
            res = NULL;
            goto exit;
          }
          switch (obj->header.type) {
          case GRN_DB :
            (*rp)->action = GRN_ACCESSOR_GET_KEY;
            rp = &(*rp)->next;
            *rp = accessor_new(ctx);
            (*rp)->obj = obj;
            (*rp)->action = GRN_ACCESSOR_GET_DB_OBJ;
            done++;
            break;
          case GRN_TYPE :
            (*rp)->action = GRN_ACCESSOR_GET_KEY;
            done++;
            break;
          case GRN_TABLE_PAT_KEY :
          case GRN_TABLE_DAT_KEY :
          case GRN_TABLE_HASH_KEY :
            (*rp)->action = GRN_ACCESSOR_GET_KEY;
            break;
          case GRN_TABLE_NO_KEY :
            if (obj->header.domain) {
              (*rp)->action = GRN_ACCESSOR_GET_VALUE;
              break;
            }
            /* fallthru */
          default :
            /* lookup failed */
            grn_obj_close(ctx, (grn_obj *)res);
            res = NULL;
            goto exit;
          }
        }
        break;
      case 'i' : /* id */
        if (len != GRN_COLUMN_NAME_ID_LEN ||
            memcmp(name, GRN_COLUMN_NAME_ID, GRN_COLUMN_NAME_ID_LEN)) {
          goto exit;
        }
        for (rp = &res; !done; rp = &(*rp)->next) {
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (!obj->header.domain) {
            (*rp)->action = GRN_ACCESSOR_GET_ID;
            done++;
          } else {
            if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
            switch (obj->header.type) {
            case GRN_DB :
            case GRN_TYPE :
              (*rp)->action = GRN_ACCESSOR_GET_ID;
              done++;
              break;
            case GRN_TABLE_PAT_KEY :
            case GRN_TABLE_DAT_KEY :
            case GRN_TABLE_HASH_KEY :
            case GRN_TABLE_NO_KEY :
              (*rp)->action = GRN_ACCESSOR_GET_KEY;
              break;
            default :
              /* lookup failed */
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
          }
        }
        break;
      case 'v' : /* value */
        if (len != GRN_COLUMN_NAME_VALUE_LEN ||
            memcmp(name, GRN_COLUMN_NAME_VALUE, GRN_COLUMN_NAME_VALUE_LEN)) {
          goto exit;
        }
        for (rp = &res; !done; rp = &(*rp)->next) {
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (!obj->header.domain) {
            if (DB_OBJ((*rp)->obj)->range) {
              (*rp)->action = GRN_ACCESSOR_GET_VALUE;
              done++;
            } else {
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
            done++;
          } else {
            if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
            switch (obj->header.type) {
            case GRN_DB :
            case GRN_TYPE :
              if (DB_OBJ((*rp)->obj)->range) {
                (*rp)->action = GRN_ACCESSOR_GET_VALUE;
                done++;
              } else {
                grn_obj_close(ctx, (grn_obj *)res);
                res = NULL;
                goto exit;
              }
              break;
            case GRN_TABLE_PAT_KEY :
            case GRN_TABLE_DAT_KEY :
            case GRN_TABLE_HASH_KEY :
            case GRN_TABLE_NO_KEY :
             (*rp)->action = GRN_ACCESSOR_GET_KEY;
              break;
            default :
              /* lookup failed */
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
          }
        }
        break;
      case 's' : /* score, sum */
        if (len == GRN_COLUMN_NAME_SCORE_LEN &&
            memcmp(name, GRN_COLUMN_NAME_SCORE, GRN_COLUMN_NAME_SCORE_LEN) == 0) {
          if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                               GRN_ACCESSOR_GET_SCORE)) {
            goto exit;
          }
        } else if (len == GRN_COLUMN_NAME_SUM_LEN &&
                   memcmp(name,
                          GRN_COLUMN_NAME_SUM,
                          GRN_COLUMN_NAME_SUM_LEN) == 0) {
          if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                               GRN_ACCESSOR_GET_SUM)) {
            goto exit;
          }
        } else {
          goto exit;
        }
        break;
      case 'n' : /* nsubrecs */
        if (len != GRN_COLUMN_NAME_NSUBRECS_LEN ||
            memcmp(name,
                   GRN_COLUMN_NAME_NSUBRECS,
                   GRN_COLUMN_NAME_NSUBRECS_LEN)) {
          goto exit;
        }
        if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                             GRN_ACCESSOR_GET_NSUBRECS)) {
          goto exit;
        }
        break;
      case 'm' : /* max, min */
        if (len == GRN_COLUMN_NAME_MAX_LEN &&
            memcmp(name,
                   GRN_COLUMN_NAME_MAX,
                   GRN_COLUMN_NAME_MAX_LEN) == 0) {
          if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                               GRN_ACCESSOR_GET_MAX)) {
            goto exit;
          }
        } else if (len == GRN_COLUMN_NAME_MIN_LEN &&
                   memcmp(name,
                          GRN_COLUMN_NAME_MIN,
                          GRN_COLUMN_NAME_MIN_LEN) == 0) {
          if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                               GRN_ACCESSOR_GET_MIN)) {
            goto exit;
          }
        } else {
          goto exit;
        }
        break;
      case 'a' : /* avg */
        if (len == GRN_COLUMN_NAME_AVG_LEN &&
            memcmp(name,
                   GRN_COLUMN_NAME_AVG,
                   GRN_COLUMN_NAME_AVG_LEN) == 0) {
          if (!grn_obj_get_accessor_rset_value(ctx, obj, &res,
                                               GRN_ACCESSOR_GET_AVG)) {
            goto exit;
          }
        } else {
          goto exit;
        }
        break;
      default :
        res = NULL;
        goto exit;
      }
    } else {
      /* if obj->header.type == GRN_TYPE ... lookup table */
      for (rp = &res; ; rp = &(*rp)->next) {
        grn_obj *column = grn_obj_column_(ctx, obj, name, len);
        if (column) {
          *rp = accessor_new(ctx);
          (*rp)->obj = column;
          /*
          switch (column->header.type) {
          case GRN_COLUMN_VAR_SIZE :
            break;
          case GRN_COLUMN_FIX_SIZE :
            break;
          case GRN_COLUMN_INDEX :
            break;
          }
          */
          (*rp)->action = GRN_ACCESSOR_GET_COLUMN_VALUE;
          break;
        } else {
          grn_id next_obj_id;
          next_obj_id = obj->header.domain;
          if (!next_obj_id) {
            // ERR(GRN_INVALID_ARGUMENT, "no such column: <%s>", name);
            if (!is_chained) {
              grn_obj_close(ctx, (grn_obj *)res);
            }
            res = NULL;
            goto exit;
          }
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          obj = grn_ctx_at(ctx, next_obj_id);
          if (!obj) {
            grn_obj_close(ctx, (grn_obj *)res);
            res = NULL;
            goto exit;
          }
          switch (obj->header.type) {
          case GRN_TABLE_PAT_KEY :
          case GRN_TABLE_DAT_KEY :
          case GRN_TABLE_HASH_KEY :
          case GRN_TABLE_NO_KEY :
            (*rp)->action = GRN_ACCESSOR_GET_KEY;
            break;
          default :
            /* lookup failed */
            grn_obj_close(ctx, (grn_obj *)res);
            res = NULL;
            goto exit;
          }
        }
      }
    }
    if (sp != se) {
      if (!grn_obj_get_accessor(ctx, (grn_obj *)res, sp, se - sp)) {
        if (!is_chained) {
          grn_obj_close(ctx, (grn_obj *)res);
          res = NULL;
          goto exit;
        }
      }
    }
  }
  if (rp0) { *rp0 = res; }
 exit :
  GRN_API_RETURN((grn_obj *)res);
}

inline static grn_bool
grn_column_is_vector(grn_ctx *ctx, grn_obj *column)
{
  grn_obj_flags type;

  if (column->header.type != GRN_COLUMN_VAR_SIZE) {
    return GRN_FALSE;
  }

  type = column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK;
  return type == GRN_OBJ_COLUMN_VECTOR;
}

inline static grn_bool
grn_column_is_index(grn_ctx *ctx, grn_obj *column)
{
  grn_obj_flags type;

  if (column->header.type == GRN_ACCESSOR) {
    grn_accessor *a;
    for (a = (grn_accessor *)column; a; a = a->next) {
      if (a->next) {
        continue;
      }
      if (a->action != GRN_ACCESSOR_GET_COLUMN_VALUE) {
        return GRN_FALSE;
      }

      column = a->obj;
    }
  }

  if (column->header.type != GRN_COLUMN_INDEX) {
    return GRN_FALSE;
  }

  type = column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK;
  return type == GRN_OBJ_COLUMN_INDEX;
}

inline static void
grn_obj_get_range_info(grn_ctx *ctx, grn_obj *obj,
                       grn_id *range_id, grn_obj_flags *range_flags)
{
  if (!obj) {
    *range_id = GRN_ID_NIL;
  } else if (grn_obj_is_proc(ctx, obj)) {
    /* TODO */
    *range_id = GRN_ID_NIL;
  } else if (GRN_DB_OBJP(obj)) {
    *range_id = DB_OBJ(obj)->range;
    if (grn_column_is_vector(ctx, obj)) {
      *range_flags = GRN_OBJ_VECTOR;
    }
  } else if (obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a;
    for (a = (grn_accessor *)obj; a; a = a->next) {
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        *range_id = GRN_DB_UINT32;
        break;
      case GRN_ACCESSOR_GET_VALUE :
        if (GRN_DB_OBJP(a->obj)) {
          *range_id = DB_OBJ(a->obj)->range;
        }
        break;
      case GRN_ACCESSOR_GET_SCORE :
        *range_id = GRN_DB_FLOAT;
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        *range_id = GRN_DB_INT32;
        break;
      case GRN_ACCESSOR_GET_MAX :
      case GRN_ACCESSOR_GET_MIN :
      case GRN_ACCESSOR_GET_SUM :
        *range_id = GRN_DB_INT64;
        break;
      case GRN_ACCESSOR_GET_AVG :
        *range_id = GRN_DB_FLOAT;
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        grn_obj_get_range_info(ctx, a->obj, range_id, range_flags);
        break;
      case GRN_ACCESSOR_GET_KEY :
        if (GRN_DB_OBJP(a->obj)) { *range_id = DB_OBJ(a->obj)->header.domain; }
        break;
      default :
        if (GRN_DB_OBJP(a->obj)) { *range_id = DB_OBJ(a->obj)->range; }
        break;
      }
    }
  }
}

grn_id
grn_obj_get_range(grn_ctx *ctx, grn_obj *obj)
{
  grn_id range_id = GRN_ID_NIL;
  grn_obj_flags range_flags = 0;

  grn_obj_get_range_info(ctx, obj, &range_id, &range_flags);

  return range_id;
}

int
grn_obj_is_persistent(grn_ctx *ctx, grn_obj *obj)
{
  int res = 0;
  if (GRN_DB_OBJP(obj)) {
    res = IS_TEMP(obj) ? 0 : 1;
  } else if (obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a;
    for (a = (grn_accessor *)obj; a; a = a->next) {
      switch (a->action) {
      case GRN_ACCESSOR_GET_SCORE :
      case GRN_ACCESSOR_GET_NSUBRECS :
      case GRN_ACCESSOR_GET_MAX :
      case GRN_ACCESSOR_GET_MIN :
      case GRN_ACCESSOR_GET_SUM :
      case GRN_ACCESSOR_GET_AVG :
        res = 0;
        break;
      case GRN_ACCESSOR_GET_ID :
      case GRN_ACCESSOR_GET_VALUE :
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
      case GRN_ACCESSOR_GET_KEY :
        if (GRN_DB_OBJP(a->obj)) { res = IS_TEMP(obj) ? 0 : 1; }
        break;
      default :
        if (GRN_DB_OBJP(a->obj)) { res = IS_TEMP(obj) ? 0 : 1; }
        break;
      }
    }
  }
  return res;
}

#define SRC2RECORD() do {\
  grn_obj *table = grn_ctx_at(ctx, dest->header.domain);\
  if (GRN_OBJ_TABLEP(table)) {\
    grn_obj *p_key = src;\
    grn_id id;\
    if (table->header.type != GRN_TABLE_NO_KEY) {\
      grn_obj key;\
      GRN_OBJ_INIT(&key, GRN_BULK, 0, table->header.domain);\
      if (src->header.domain != table->header.domain) {\
        rc = grn_obj_cast(ctx, src, &key, GRN_TRUE);\
        p_key = &key;\
      }\
      if (!rc) {\
        if (GRN_BULK_VSIZE(p_key)) {\
          if (add_record_if_not_exist) {\
            id = grn_table_add_by_key(ctx, table, p_key, NULL);\
          } else {\
            id = grn_table_get_by_key(ctx, table, p_key);\
          }\
          if (id) {\
            GRN_RECORD_SET(ctx, dest, id);\
          } else {\
            rc = GRN_INVALID_ARGUMENT;\
          }\
        } else {\
          GRN_RECORD_SET(ctx, dest, GRN_ID_NIL);\
        }\
      }\
      GRN_OBJ_FIN(ctx, &key);\
    } else {\
      grn_obj record_id;\
      GRN_UINT32_INIT(&record_id, 0);\
      rc = grn_obj_cast(ctx, src, &record_id, GRN_TRUE);\
      if (!rc) {\
        id = GRN_UINT32_VALUE(&record_id);\
        if (id) {\
          GRN_RECORD_SET(ctx, dest, id);\
        } else {\
          rc = GRN_INVALID_ARGUMENT;\
        }\
      }\
    }\
  } else {\
    rc = GRN_FUNCTION_NOT_IMPLEMENTED;\
  }\
} while (0)

inline static grn_rc
grn_obj_cast_bool(grn_ctx *ctx, grn_obj *src, grn_obj *dest,
                  grn_bool add_record_if_not_exist)
{
  grn_rc rc = GRN_SUCCESS;

  switch (dest->header.domain) {
  case GRN_DB_BOOL :
    GRN_BOOL_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_INT8 :
    GRN_INT8_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_UINT8 :
    GRN_UINT8_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_INT16 :
    GRN_INT16_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_UINT16 :
    GRN_UINT16_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_INT32 :
    GRN_INT32_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_UINT32 :
    GRN_UINT32_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_INT64 :
    GRN_INT64_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_UINT64 :
    GRN_UINT64_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_FLOAT :
    GRN_FLOAT_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_TIME :
    GRN_TIME_SET(ctx, dest, GRN_BOOL_VALUE(src));
    break;
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    {
      const char *bool_text;
      bool_text = GRN_BOOL_VALUE(src) ? "true" : "false";
      GRN_TEXT_PUTS(ctx, dest, bool_text);
    }
    break;
  case GRN_DB_TOKYO_GEO_POINT :
  case GRN_DB_WGS84_GEO_POINT :
    rc = GRN_INVALID_ARGUMENT;
    break;
  default :
    SRC2RECORD();
    break;
  }
  return rc;
}

#define NUM2DEST(getvalue,totext,tobool,totime,tofloat)\
  switch (dest->header.domain) {\
  case GRN_DB_BOOL :\
    tobool(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_INT8 :\
    GRN_INT8_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_UINT8 :\
    GRN_UINT8_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_INT16 :\
    GRN_INT16_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_UINT16 :\
    GRN_UINT16_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_INT32 :\
    GRN_INT32_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_UINT32 :\
    GRN_UINT32_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_TIME :\
    totime(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_INT64 :\
    GRN_INT64_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_UINT64 :\
    GRN_UINT64_SET(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_FLOAT :\
    tofloat(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    totext(ctx, dest, getvalue(src));\
    break;\
  case GRN_DB_TOKYO_GEO_POINT :\
  case GRN_DB_WGS84_GEO_POINT :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  default :\
    SRC2RECORD();\
    break;\
  }

#define TEXT2DEST(type,tonum,setvalue) do {\
  const char *cur, *str = GRN_TEXT_VALUE(src);\
  const char *str_end = GRN_BULK_CURR(src);\
  type i = tonum(str, str_end, &cur);\
  if (cur == str_end) {\
    setvalue(ctx, dest, i);\
  } else if (cur != str) {\
    const char *rest;\
    grn_obj buf;\
    GRN_VOID_INIT(&buf);\
    rc = grn_aton(ctx, str, str_end, &rest, &buf);\
    if (!rc) {\
      rc = grn_obj_cast(ctx, &buf, dest, add_record_if_not_exist);\
    }\
    GRN_OBJ_FIN(ctx, &buf);\
  } else {\
    rc = GRN_INVALID_ARGUMENT;\
  }\
} while (0)

#define NUM2BOOL(ctx, dest, value) GRN_BOOL_SET(ctx, dest, value != 0)
#define FLOAT2BOOL(ctx, dest, value) do {\
  double value_ = value;\
  GRN_BOOL_SET(ctx, dest, value_ < -DBL_EPSILON || DBL_EPSILON < value_);\
} while (0)

#define NUM2TIME(ctx, dest, value)\
  GRN_TIME_SET(ctx, dest, (long long int)(value) * GRN_TIME_USEC_PER_SEC);
#define TIME2TIME(ctx, dest, value)\
  GRN_TIME_SET(ctx, dest, value);
#define FLOAT2TIME(ctx, dest, value) do {\
  int64_t usec = llround(value * GRN_TIME_USEC_PER_SEC);\
  GRN_TIME_SET(ctx, dest, usec);\
} while (0)

#define NUM2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, value);
#define TIME2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, (double)(value) / GRN_TIME_USEC_PER_SEC);
#define FLOAT2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, value);

static grn_rc
grn_obj_cast_record(grn_ctx *ctx,
                    grn_obj *src,
                    grn_obj *dest,
                    grn_bool add_record_if_not_exist)
{
  grn_obj *src_table;
  grn_obj *dest_table;
  const char *key;
  uint32_t key_size;
  grn_id dest_id;

  if (src->header.domain == dest->header.domain) {
    GRN_RECORD_SET(ctx, dest, GRN_RECORD_VALUE(src));
    return GRN_SUCCESS;
  }

  src_table = grn_ctx_at(ctx, src->header.domain);
  if (!src_table) {
    return GRN_INVALID_ARGUMENT;
  }
  if (src_table->header.type == GRN_TABLE_NO_KEY) {
    return GRN_INVALID_ARGUMENT;
  }

  dest_table = grn_ctx_at(ctx, dest->header.domain);
  if (!dest_table) {
    return GRN_INVALID_ARGUMENT;
  }
  switch (dest_table->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    break;
  default :
    return GRN_INVALID_ARGUMENT;
  }

  if (GRN_RECORD_VALUE(src) == GRN_ID_NIL) {
    GRN_RECORD_SET(ctx, dest, GRN_RECORD_VALUE(src));
    return GRN_SUCCESS;
  }

  key = _grn_table_key(ctx, src_table, GRN_RECORD_VALUE(src), &key_size);
  if (add_record_if_not_exist) {
    dest_id = grn_table_add(ctx, dest_table, key, key_size, NULL);
  } else {
    dest_id = grn_table_get(ctx, dest_table, key, key_size);
  }
  GRN_RECORD_SET(ctx, dest, dest_id);
  return GRN_SUCCESS;
}

grn_rc
grn_obj_cast(grn_ctx *ctx, grn_obj *src, grn_obj *dest,
             grn_bool add_record_if_not_exist)
{
  grn_rc rc = GRN_SUCCESS;
  switch (src->header.domain) {
  case GRN_DB_BOOL :
    rc = grn_obj_cast_bool(ctx, src, dest, add_record_if_not_exist);
    break;
  case GRN_DB_INT8 :
    NUM2DEST(GRN_INT8_VALUE, grn_text_itoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_UINT8 :
    NUM2DEST(GRN_UINT8_VALUE, grn_text_lltoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_INT16 :
    NUM2DEST(GRN_INT16_VALUE, grn_text_itoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_UINT16 :
    NUM2DEST(GRN_UINT16_VALUE, grn_text_lltoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_INT32 :
    NUM2DEST(GRN_INT32_VALUE, grn_text_itoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_UINT32 :
    NUM2DEST(GRN_UINT32_VALUE, grn_text_lltoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_INT64 :
    NUM2DEST(GRN_INT64_VALUE, grn_text_lltoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_TIME :
    NUM2DEST(GRN_TIME_VALUE, grn_text_lltoa, NUM2BOOL, TIME2TIME, TIME2FLOAT);
    break;
  case GRN_DB_UINT64 :
    NUM2DEST(GRN_UINT64_VALUE, grn_text_lltoa, NUM2BOOL, NUM2TIME, NUM2FLOAT);
    break;
  case GRN_DB_FLOAT :
    NUM2DEST(GRN_FLOAT_VALUE, grn_text_ftoa, FLOAT2BOOL, FLOAT2TIME,
             FLOAT2FLOAT);
    break;
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    switch (dest->header.domain) {
    case GRN_DB_BOOL :
      GRN_BOOL_SET(ctx, dest, GRN_TEXT_LEN(src) > 0);
      break;
    case GRN_DB_INT8 :
      TEXT2DEST(int8_t, grn_atoi8, GRN_INT8_SET);
      break;
    case GRN_DB_UINT8 :
      TEXT2DEST(uint8_t, grn_atoui8, GRN_UINT8_SET);
      break;
    case GRN_DB_INT16 :
      TEXT2DEST(int16_t, grn_atoi16, GRN_INT16_SET);
      break;
    case GRN_DB_UINT16 :
      TEXT2DEST(uint16_t, grn_atoui16, GRN_UINT16_SET);
      break;
    case GRN_DB_INT32 :
      TEXT2DEST(int32_t, grn_atoi, GRN_INT32_SET);
      break;
    case GRN_DB_UINT32 :
      TEXT2DEST(uint32_t, grn_atoui, GRN_UINT32_SET);
      break;
    case GRN_DB_TIME :
      {
        grn_timeval v;
        int len = GRN_TEXT_LEN(src);
        char *str = GRN_TEXT_VALUE(src);
        if (grn_str2timeval(str, len, &v)) {
          double d;
          char *end;
          grn_obj buf;
          GRN_TEXT_INIT(&buf, 0);
          GRN_TEXT_PUT(ctx, &buf, str, len);
          GRN_TEXT_PUTC(ctx, &buf, '\0');
          errno = 0;
          d = strtod(GRN_TEXT_VALUE(&buf), &end);
          if (!errno && end + 1 == GRN_BULK_CURR(&buf)) {
            v.tv_sec = d;
            v.tv_nsec = ((d - v.tv_sec) * GRN_TIME_NSEC_PER_SEC);
          } else {
            rc = GRN_INVALID_ARGUMENT;
          }
          GRN_OBJ_FIN(ctx, &buf);
        }
        GRN_TIME_SET(ctx, dest,
                     GRN_TIME_PACK((int64_t)v.tv_sec,
                                   GRN_TIME_NSEC_TO_USEC(v.tv_nsec)));
      }
      break;
    case GRN_DB_INT64 :
      TEXT2DEST(int64_t, grn_atoll, GRN_INT64_SET);
      break;
    case GRN_DB_UINT64 :
      TEXT2DEST(int64_t, grn_atoll, GRN_UINT64_SET);
      break;
    case GRN_DB_FLOAT :
      {
        double d;
        char *end;
        grn_obj buf;
        GRN_TEXT_INIT(&buf, 0);
        GRN_TEXT_PUT(ctx, &buf, GRN_TEXT_VALUE(src), GRN_TEXT_LEN(src));
        GRN_TEXT_PUTC(ctx, &buf, '\0');
        errno = 0;
        d = strtod(GRN_TEXT_VALUE(&buf), &end);
        if (!errno && end + 1 == GRN_BULK_CURR(&buf)) {
          GRN_FLOAT_SET(ctx, dest, d);
        } else {
          rc = GRN_INVALID_ARGUMENT;
        }
        GRN_OBJ_FIN(ctx, &buf);
      }
      break;
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      GRN_TEXT_PUT(ctx, dest, GRN_TEXT_VALUE(src), GRN_TEXT_LEN(src));
      break;
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      {
        int latitude, longitude;
        double degree;
        const char *cur, *str = GRN_TEXT_VALUE(src);
        const char *str_end = GRN_BULK_CURR(src);
        if (str == str_end) {
          GRN_GEO_POINT_SET(ctx, dest, 0, 0);
        } else {
          char *end;
          grn_obj buf, *buf_p = NULL;
          latitude = grn_atoi(str, str_end, &cur);
          if (cur < str_end && cur[0] == '.') {
            GRN_TEXT_INIT(&buf, 0);
            GRN_TEXT_PUT(ctx, &buf, str, GRN_TEXT_LEN(src));
            GRN_TEXT_PUTC(ctx, &buf, '\0');
            buf_p = &buf;
            errno = 0;
            degree = strtod(GRN_TEXT_VALUE(buf_p), &end);
            if (errno) {
              rc = GRN_INVALID_ARGUMENT;
            } else {
              latitude = GRN_GEO_DEGREE2MSEC(degree);
              cur = str + (end - GRN_TEXT_VALUE(buf_p));
            }
          }
          if (!rc && (cur[0] == 'x' || cur[0] == ',') && cur + 1 < str_end) {
            const char *c = cur + 1;
            longitude = grn_atoi(c, str_end, &cur);
            if (cur < str_end && cur[0] == '.') {
              if (!buf_p) {
                GRN_TEXT_INIT(&buf, 0);
                GRN_TEXT_PUT(ctx, &buf, str, GRN_TEXT_LEN(src));
                GRN_TEXT_PUTC(ctx, &buf, '\0');
                buf_p = &buf;
              }
              errno = 0;
              degree = strtod(GRN_TEXT_VALUE(buf_p) + (c - str), &end);
              if (errno) {
                rc = GRN_INVALID_ARGUMENT;
              } else {
                longitude = GRN_GEO_DEGREE2MSEC(degree);
                cur = str + (end - GRN_TEXT_VALUE(buf_p));
              }
            }
            if (!rc && cur == str_end) {
              if ((GRN_GEO_MIN_LATITUDE <= latitude &&
                   latitude <= GRN_GEO_MAX_LATITUDE) &&
                  (GRN_GEO_MIN_LONGITUDE <= longitude &&
                   longitude <= GRN_GEO_MAX_LONGITUDE)) {
                GRN_GEO_POINT_SET(ctx, dest, latitude, longitude);
              } else {
                rc = GRN_INVALID_ARGUMENT;
              }
            } else {
              rc = GRN_INVALID_ARGUMENT;
            }
          } else {
            rc = GRN_INVALID_ARGUMENT;
          }
          if (buf_p) { GRN_OBJ_FIN(ctx, buf_p); }
        }
      }
      break;
    default :
      SRC2RECORD();
      break;
    }
    break;
  case GRN_DB_TOKYO_GEO_POINT :
  case GRN_DB_WGS84_GEO_POINT :
    if (src->header.domain == dest->header.domain) {
      GRN_TEXT_PUT(ctx, dest, GRN_TEXT_VALUE(src), GRN_TEXT_LEN(src));
    } else {
      int latitude, longitude;
      double latitude_in_degree, longitude_in_degree;
      GRN_GEO_POINT_VALUE(src, latitude, longitude);
      latitude_in_degree = GRN_GEO_MSEC2DEGREE(latitude);
      longitude_in_degree = GRN_GEO_MSEC2DEGREE(longitude);
      /* TokyoGeoPoint <-> WGS84GeoPoint is based on
         http://www.jalan.net/jw/jwp0200/jww0203.do

         jx: longitude in degree in Tokyo Geodetic System.
         jy: latitude in degree in Tokyo Geodetic System.
         wx: longitude in degree in WGS 84.
         wy: latitude in degree in WGS 84.

         jy = wy * 1.000106961 - wx * 0.000017467 - 0.004602017
         jx = wx * 1.000083049 + wy * 0.000046047 - 0.010041046

         wy = jy - jy * 0.00010695 + jx * 0.000017464 + 0.0046017
         wx = jx - jy * 0.000046038 - jx * 0.000083043 + 0.010040
      */
      if (dest->header.domain == GRN_DB_TOKYO_GEO_POINT) {
        double wgs84_latitude_in_degree = latitude_in_degree;
        double wgs84_longitude_in_degree = longitude_in_degree;
        int tokyo_latitude, tokyo_longitude;
        double tokyo_latitude_in_degree, tokyo_longitude_in_degree;
        tokyo_latitude_in_degree =
          wgs84_latitude_in_degree * 1.000106961 -
          wgs84_longitude_in_degree * 0.000017467 -
          0.004602017;
        tokyo_longitude_in_degree =
          wgs84_longitude_in_degree * 1.000083049 +
          wgs84_latitude_in_degree  * 0.000046047 -
          0.010041046;
        tokyo_latitude = GRN_GEO_DEGREE2MSEC(tokyo_latitude_in_degree);
        tokyo_longitude = GRN_GEO_DEGREE2MSEC(tokyo_longitude_in_degree);
        GRN_GEO_POINT_SET(ctx, dest, tokyo_latitude, tokyo_longitude);
      } else {
        double tokyo_latitude_in_degree = latitude_in_degree;
        double tokyo_longitude_in_degree = longitude_in_degree;
        int wgs84_latitude, wgs84_longitude;
        double wgs84_latitude_in_degree, wgs84_longitude_in_degree;
        wgs84_latitude_in_degree =
          tokyo_latitude_in_degree -
          tokyo_latitude_in_degree * 0.00010695 +
          tokyo_longitude_in_degree * 0.000017464 +
          0.0046017;
        wgs84_longitude_in_degree =
          tokyo_longitude_in_degree -
          tokyo_latitude_in_degree * 0.000046038 -
          tokyo_longitude_in_degree * 0.000083043 +
          0.010040;
        wgs84_latitude = GRN_GEO_DEGREE2MSEC(wgs84_latitude_in_degree);
        wgs84_longitude = GRN_GEO_DEGREE2MSEC(wgs84_longitude_in_degree);
        GRN_GEO_POINT_SET(ctx, dest, wgs84_latitude, wgs84_longitude);
      }
    }
    break;
  case GRN_VOID :
    rc = grn_obj_reinit(ctx, dest, dest->header.domain, dest->header.flags);
    break;
  default :
    if (src->header.domain >= GRN_N_RESERVED_TYPES) {
      grn_obj *table;
      table = grn_ctx_at(ctx, src->header.domain);
      switch (table->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
      case GRN_TABLE_NO_KEY :
        rc = grn_obj_cast_record(ctx, src, dest, add_record_if_not_exist);
        break;
      default :
        rc = GRN_FUNCTION_NOT_IMPLEMENTED;
        break;
      }
    } else {
      rc = GRN_FUNCTION_NOT_IMPLEMENTED;
    }
    break;
  }
  return rc;
}

const char *
grn_accessor_get_value_(grn_ctx *ctx, grn_accessor *a, grn_id id, uint32_t *size)
{
  const char *value = NULL;
  for (;;) {
    switch (a->action) {
    case GRN_ACCESSOR_GET_ID :
      value = (const char *)(uintptr_t)id;
      *size = GRN_OBJ_GET_VALUE_IMD;
      break;
    case GRN_ACCESSOR_GET_KEY :
      value = _grn_table_key(ctx, a->obj, id, size);
      break;
    case GRN_ACCESSOR_GET_VALUE :
      value = grn_obj_get_value_(ctx, a->obj, id, size);
      break;
    case GRN_ACCESSOR_GET_SCORE :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value = (const char *)&((grn_rset_recinfo *)value)->score;
        *size = sizeof(double);
      }
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value = (const char *)&((grn_rset_recinfo *)value)->n_subrecs;
        *size = sizeof(int);
      }
      break;
    case GRN_ACCESSOR_GET_MAX :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value =
          (const char *)grn_rset_recinfo_get_max_(ctx,
                                                  (grn_rset_recinfo *)value,
                                                  a->obj);
        *size = GRN_RSET_MAX_SIZE;
      }
      break;
    case GRN_ACCESSOR_GET_MIN :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value =
          (const char *)grn_rset_recinfo_get_min_(ctx,
                                                  (grn_rset_recinfo *)value,
                                                  a->obj);
        *size = GRN_RSET_MIN_SIZE;
      }
      break;
    case GRN_ACCESSOR_GET_SUM :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value =
          (const char *)grn_rset_recinfo_get_sum_(ctx,
                                                  (grn_rset_recinfo *)value,
                                                  a->obj);
        *size = GRN_RSET_SUM_SIZE;
      }
      break;
    case GRN_ACCESSOR_GET_AVG :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value =
          (const char *)grn_rset_recinfo_get_avg_(ctx,
                                                  (grn_rset_recinfo *)value,
                                                  a->obj);
        *size = GRN_RSET_AVG_SIZE;
      }
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      /* todo : support vector */
      value = grn_obj_get_value_(ctx, a->obj, id, size);
      break;
    case GRN_ACCESSOR_GET_DB_OBJ :
      value = _grn_table_key(ctx, ((grn_db *)ctx->impl->db)->keys, id, size);
      break;
    case GRN_ACCESSOR_LOOKUP :
      /* todo */
      break;
    case GRN_ACCESSOR_FUNCALL :
      /* todo */
      break;
    }
    if (value && (a = a->next)) {
      id = *((grn_id *)value);
    } else {
      break;
    }
  }
  return value;
}

static grn_obj *
grn_accessor_get_value(grn_ctx *ctx, grn_accessor *a, grn_id id, grn_obj *value)
{
  uint32_t vs = 0;
  uint32_t size0;
  void *vp = NULL;
  if (!value) {
    if (!(value = grn_obj_open(ctx, GRN_BULK, 0, 0))) { return NULL; }
  } else {
    value->header.type = GRN_BULK;
  }
  size0 = GRN_BULK_VSIZE(value);
  for (;;) {
    grn_bulk_truncate(ctx, value, size0);
    switch (a->action) {
    case GRN_ACCESSOR_GET_ID :
      GRN_UINT32_PUT(ctx, value, id);
      value->header.domain = GRN_DB_UINT32;
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_KEY :
      if (!a->next && GRN_TABLE_IS_MULTI_KEYS_GROUPED(a->obj)) {
        grn_obj_ensure_vector(ctx, value);
        if (id) {
          grn_obj raw_vector;
          GRN_TEXT_INIT(&raw_vector, 0);
          grn_table_get_key2(ctx, a->obj, id, &raw_vector);
          grn_vector_decode(ctx, value,
                            GRN_BULK_HEAD(&raw_vector),
                            GRN_BULK_VSIZE(&raw_vector));
          GRN_OBJ_FIN(ctx, &raw_vector);
        }
        vp = NULL;
        vs = 0;
      } else {
        if (id) {
          grn_table_get_key2(ctx, a->obj, id, value);
          vp = GRN_BULK_HEAD(value) + size0;
          vs = GRN_BULK_VSIZE(value) - size0;
        } else {
          vp = NULL;
          vs = 0;
        }
        value->header.domain = a->obj->header.domain;
      }
      break;
    case GRN_ACCESSOR_GET_VALUE :
      grn_obj_get_value(ctx, a->obj, id, value);
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_SCORE :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        GRN_FLOAT_PUT(ctx, value, ri->score);
      } else {
        GRN_FLOAT_PUT(ctx, value, 0.0);
      }
      value->header.domain = GRN_DB_FLOAT;
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        GRN_INT32_PUT(ctx, value, ri->n_subrecs);
      } else {
        GRN_INT32_PUT(ctx, value, 0);
      }
      value->header.domain = GRN_DB_INT32;
      break;
    case GRN_ACCESSOR_GET_MAX :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        int64_t max;
        max = grn_rset_recinfo_get_max(ctx, ri, a->obj);
        GRN_INT64_PUT(ctx, value, max);
      } else {
        GRN_INT64_PUT(ctx, value, 0);
      }
      value->header.domain = GRN_DB_INT64;
      break;
    case GRN_ACCESSOR_GET_MIN :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        int64_t min;
        min = grn_rset_recinfo_get_min(ctx, ri, a->obj);
        GRN_INT64_PUT(ctx, value, min);
      } else {
        GRN_INT64_PUT(ctx, value, 0);
      }
      value->header.domain = GRN_DB_INT64;
      break;
    case GRN_ACCESSOR_GET_SUM :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        int64_t sum;
        sum = grn_rset_recinfo_get_sum(ctx, ri, a->obj);
        GRN_INT64_PUT(ctx, value, sum);
      } else {
        GRN_INT64_PUT(ctx, value, 0);
      }
      value->header.domain = GRN_DB_INT64;
      break;
    case GRN_ACCESSOR_GET_AVG :
      if (id) {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        double avg;
        avg = grn_rset_recinfo_get_avg(ctx, ri, a->obj);
        GRN_FLOAT_PUT(ctx, value, avg);
      } else {
        GRN_FLOAT_PUT(ctx, value, 0.0);
      }
      value->header.domain = GRN_DB_FLOAT;
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      /* todo : support vector */
      grn_obj_get_value(ctx, a->obj, id, value);
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_DB_OBJ :
      value = grn_ctx_at(ctx, id);
      grn_obj_close(ctx, value);
      return value;
      break;
    case GRN_ACCESSOR_LOOKUP :
      /* todo */
      break;
    case GRN_ACCESSOR_FUNCALL :
      /* todo */
      break;
    }
    if ((a = a->next)) {
      if (vs > 0) {
        id = *((grn_id *)vp);
      } else {
        id = GRN_ID_NIL;
      }
    } else {
      break;
    }
  }
  return value;
}

static grn_rc
grn_accessor_set_value(grn_ctx *ctx, grn_accessor *a, grn_id id,
                       grn_obj *value, int flags)
{
  grn_rc rc = GRN_SUCCESS;
  if (!value) { value = grn_obj_open(ctx, GRN_BULK, 0, 0); }
  if (value) {
    grn_obj buf;
    void *vp = NULL;
    GRN_TEXT_INIT(&buf, 0);
    for (;;) {
      GRN_BULK_REWIND(&buf);
      switch (a->action) {
      case GRN_ACCESSOR_GET_KEY :
        grn_table_get_key2(ctx, a->obj, id, &buf);
        vp = GRN_BULK_HEAD(&buf);
        break;
      case GRN_ACCESSOR_GET_VALUE :
        if (a->next) {
          grn_obj_get_value(ctx, a->obj, id, &buf);
          vp = GRN_BULK_HEAD(&buf);
        } else {
          rc = grn_obj_set_value(ctx, a->obj, id, value, flags);
        }
        break;
      case GRN_ACCESSOR_GET_SCORE :
        {
          grn_rset_recinfo *ri;
          if (a->next) {
            grn_obj_get_value(ctx, a->obj, id, &buf);
            ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
            vp = &ri->score;
          } else {
            uint32_t size;
            if ((ri = (grn_rset_recinfo *) grn_obj_get_value_(ctx, a->obj, id, &size))) {
              // todo : flags support
              if (value->header.domain == GRN_DB_FLOAT) {
                ri->score = GRN_FLOAT_VALUE(value);
              } else {
                grn_obj buf;
                GRN_FLOAT_INIT(&buf, 0);
                grn_obj_cast(ctx, value, &buf, GRN_FALSE);
                ri->score = GRN_FLOAT_VALUE(&buf);
                GRN_OBJ_FIN(ctx, &buf);
              }
            }
          }
        }
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
          vp = &ri->n_subrecs;
        }
        break;
      case GRN_ACCESSOR_GET_MAX :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
          if (value->header.type == GRN_DB_INT64) {
            grn_rset_recinfo_set_max(ctx, ri, a->obj, GRN_INT64_VALUE(value));
          } else {
            grn_obj value_int64;
            GRN_INT64_INIT(&value_int64, 0);
            if (!grn_obj_cast(ctx, value, &value_int64, GRN_FALSE)) {
              grn_rset_recinfo_set_max(ctx, ri, a->obj,
                                       GRN_INT64_VALUE(&value_int64));
            }
            GRN_OBJ_FIN(ctx, &value_int64);
          }
        }
        break;
      case GRN_ACCESSOR_GET_MIN :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
          if (value->header.type == GRN_DB_INT64) {
            grn_rset_recinfo_set_min(ctx, ri, a->obj, GRN_INT64_VALUE(value));
          } else {
            grn_obj value_int64;
            GRN_INT64_INIT(&value_int64, 0);
            if (!grn_obj_cast(ctx, value, &value_int64, GRN_FALSE)) {
              grn_rset_recinfo_set_min(ctx, ri, a->obj,
                                       GRN_INT64_VALUE(&value_int64));
            }
            GRN_OBJ_FIN(ctx, &value_int64);
          }
        }
        break;
      case GRN_ACCESSOR_GET_SUM :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
          if (value->header.type == GRN_DB_INT64) {
            grn_rset_recinfo_set_sum(ctx, ri, a->obj, GRN_INT64_VALUE(value));
          } else {
            grn_obj value_int64;
            GRN_INT64_INIT(&value_int64, 0);
            if (!grn_obj_cast(ctx, value, &value_int64, GRN_FALSE)) {
              grn_rset_recinfo_set_sum(ctx, ri, a->obj,
                                       GRN_INT64_VALUE(&value_int64));
            }
            GRN_OBJ_FIN(ctx, &value_int64);
          }
        }
        break;
      case GRN_ACCESSOR_GET_AVG :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)GRN_BULK_HEAD(&buf);
          if (value->header.type == GRN_DB_FLOAT) {
            grn_rset_recinfo_set_avg(ctx, ri, a->obj, GRN_FLOAT_VALUE(value));
          } else {
            grn_obj value_float;
            GRN_FLOAT_INIT(&value_float, 0);
            if (!grn_obj_cast(ctx, value, &value_float, GRN_FALSE)) {
              grn_rset_recinfo_set_avg(ctx, ri, a->obj,
                                       GRN_FLOAT_VALUE(&value_float));
            }
            GRN_OBJ_FIN(ctx, &value_float);
          }
        }
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        /* todo : support vector */
        if (a->next) {
          grn_obj_get_value(ctx, a->obj, id, &buf);
          vp = GRN_BULK_HEAD(&buf);
        } else {
          rc = grn_obj_set_value(ctx, a->obj, id, value, flags);
        }
        break;
      case GRN_ACCESSOR_LOOKUP :
        /* todo */
        break;
      case GRN_ACCESSOR_FUNCALL :
        /* todo */
        break;
      }
      if ((a = a->next)) {
        id = *((grn_id *)vp);
      } else {
        break;
      }
    }
    grn_obj_close(ctx, &buf);
  }
  return rc;
}

#define INCRDECR(op) \
  switch (DB_OBJ(obj)->range) {\
  case GRN_DB_INT8 :\
    if (s == sizeof(int8_t)) {\
      int8_t *vp = (int8_t *)p;\
      *vp op *(int8_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_UINT8 :\
    if (s == sizeof(uint8_t)) {\
      uint8_t *vp = (uint8_t *)p;\
      *vp op *(int8_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_INT16 :\
    if (s == sizeof(int16_t)) {\
      int16_t *vp = (int16_t *)p;\
      *vp op *(int16_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_UINT16 :\
    if (s == sizeof(uint16_t)) {\
      uint16_t *vp = (uint16_t *)p;\
      *vp op *(int16_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_INT32 :\
    if (s == sizeof(int32_t)) {\
      int32_t *vp = (int32_t *)p;\
      *vp op *(int32_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_UINT32 :\
    if (s == sizeof(uint32_t)) {\
      uint32_t *vp = (uint32_t *)p;\
      *vp op *(int32_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_INT64 :\
  case GRN_DB_TIME :\
    if (s == sizeof(int64_t)) {\
      int64_t *vp = (int64_t *)p;\
      *vp op *(int64_t *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  case GRN_DB_FLOAT :\
    if (s == sizeof(double)) {\
      double *vp = (double *)p;\
      *vp op *(double *)v;\
      rc = GRN_SUCCESS;\
    } else {\
      rc = GRN_INVALID_ARGUMENT;\
    }\
    break;\
  default :\
    rc = GRN_OPERATION_NOT_SUPPORTED;\
    break;\
  }

uint32_t
grn_obj_size(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) { return 0; }
  switch (obj->header.type) {
  case GRN_VOID :
  case GRN_BULK :
  case GRN_PTR :
  case GRN_UVECTOR :
  case GRN_PVECTOR :
  case GRN_MSG :
    return GRN_BULK_VSIZE(obj);
  case GRN_VECTOR :
    return obj->u.v.body ? GRN_BULK_VSIZE(obj->u.v.body) : 0;
  default :
    return 0;
  }
}

inline static int
call_hook(grn_ctx *ctx, grn_obj *obj, grn_id id, grn_obj *value, int flags)
{
  grn_hook *hooks = DB_OBJ(obj)->hooks[GRN_HOOK_SET];
  void *v = GRN_BULK_HEAD(value);
  unsigned int s = grn_obj_size(ctx, value);
  if (hooks || obj->header.type == GRN_COLUMN_VAR_SIZE) {
    grn_obj oldbuf, *oldvalue;
    GRN_TEXT_INIT(&oldbuf, 0);
    oldvalue = grn_obj_get_value(ctx, obj, id, &oldbuf);
    if (flags & GRN_OBJ_SET) {
      void *ov;
      unsigned int os;
      ov = GRN_BULK_HEAD(oldvalue);
      os = grn_obj_size(ctx, oldvalue);
      if ((ov && v && os == s && !memcmp(ov, v, s)) &&
          !(obj->header.type == GRN_COLUMN_FIX_SIZE &&
            grn_bulk_is_zero(ctx, value))) {
        grn_obj_close(ctx, oldvalue);
        return 0;
      }
    }
    if (hooks) {
      // todo : grn_proc_ctx_open()
      grn_obj id_, flags_;
      grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4, {{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0}}};
      GRN_UINT32_INIT(&id_, 0);
      GRN_UINT32_INIT(&flags_, 0);
      GRN_UINT32_SET(ctx, &id_, id);
      GRN_UINT32_SET(ctx, &flags_, flags);
      while (hooks) {
        grn_ctx_push(ctx, &id_);
        grn_ctx_push(ctx, oldvalue);
        grn_ctx_push(ctx, value);
        grn_ctx_push(ctx, &flags_);
        pctx.caller = NULL;
        pctx.currh = hooks;
        if (hooks->proc) {
          hooks->proc->funcs[PROC_INIT](ctx, 1, &obj, &pctx.user_data);
        } else {
          grn_obj_default_set_value_hook(ctx, 1, &obj, &pctx.user_data);
        }
        if (ctx->rc) {
          grn_obj_close(ctx, oldvalue);
          return 1;
        }
        hooks = hooks->next;
        pctx.offset++;
      }
    }
    grn_obj_close(ctx, oldvalue);
  }
  return 0;
}

static grn_rc
grn_obj_set_value_table_pat_key(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  grn_obj buf;

  if (call_hook(ctx, obj, id, value, flags)) {
    if (ctx->rc) {
      rc = ctx->rc;
    }
    return rc;
  }

  if (range != value->header.domain) {
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
    if (grn_obj_cast(ctx, value, &buf, GRN_TRUE) == GRN_SUCCESS) {
      v = GRN_BULK_HEAD(&buf);
    }
  }
  rc = grn_pat_set_value(ctx, (grn_pat *)obj, id, v, flags);
  if (range != value->header.domain) {
    grn_obj_close(ctx, &buf);
  }

  return rc;
}

static grn_rc
grn_obj_set_value_table_hash_key(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                 grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  grn_obj buf;

  if (call_hook(ctx, obj, id, value, flags)) {
    if (ctx->rc) {
      rc = ctx->rc;
    }
    return rc;
  }

  if (range != value->header.domain) {
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
    if (grn_obj_cast(ctx, value, &buf, GRN_TRUE) == GRN_SUCCESS) {
      v = GRN_BULK_HEAD(&buf);
    }
  }
  rc = grn_hash_set_value(ctx, (grn_hash *)obj, id, v, flags);
  if (range != value->header.domain) {
    grn_obj_close(ctx, &buf);
  }

  return rc;
}

static grn_rc
grn_obj_set_value_table_no_key(grn_ctx *ctx, grn_obj *obj, grn_id id,
                               grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  grn_obj buf;

  if (call_hook(ctx, obj, id, value, flags)) {
    if (ctx->rc) {
      rc = ctx->rc;
    }
    return rc;
  }

  if (range != value->header.domain) {
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
    if (grn_obj_cast(ctx, value, &buf, GRN_TRUE) == GRN_SUCCESS) {
      v = GRN_BULK_HEAD(&buf);
    }
  }
  rc = grn_array_set_value(ctx, (grn_array *)obj, id, v, flags);
  if (range != value->header.domain) {
    grn_obj_close(ctx, &buf);
  }

  return rc;
}

static grn_rc
grn_obj_set_value_column_var_size_scalar(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                         grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  unsigned int s = grn_obj_size(ctx, value);
  grn_obj buf;
  grn_id buf_domain = GRN_DB_VOID;

  if (call_hook(ctx, obj, id, value, flags)) {
    if (ctx->rc) {
      rc = ctx->rc;
    }
    return rc;
  }

  switch (flags & GRN_OBJ_SET_MASK) {
  case GRN_OBJ_INCR :
  case GRN_OBJ_DECR :
    if (value->header.domain == GRN_DB_INT32 ||
        value->header.domain == GRN_DB_INT64) {
      /* do nothing */
    } else if (GRN_DB_INT8 <= value->header.domain &&
               value->header.domain < GRN_DB_INT32) {
      buf_domain = GRN_DB_INT32;
    } else {
      buf_domain = GRN_DB_INT64;
    }
    break;
  default :
    if (range != value->header.domain) {
      buf_domain = range;
    }
    break;
  }

  if (buf_domain != GRN_DB_VOID) {
    GRN_OBJ_INIT(&buf, GRN_BULK, 0, buf_domain);
    if (grn_obj_cast(ctx, value, &buf, GRN_TRUE) == GRN_SUCCESS) {
      v = GRN_BULK_HEAD(&buf);
      s = GRN_BULK_VSIZE(&buf);
    }
  }

  rc = grn_ja_put(ctx, (grn_ja *)obj, id, v, s, flags, NULL);

  if (buf_domain != GRN_DB_VOID) {
    grn_obj_close(ctx, &buf);
  }

  return rc;
}

static grn_rc
grn_obj_set_value_column_var_size_vector_uvector(grn_ctx *ctx, grn_obj *column,
                                                 grn_id id, grn_obj *value,
                                                 int flags)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj uvector;
  grn_obj_flags uvector_flags = 0;
  grn_bool need_convert = GRN_FALSE;
  grn_bool need_cast = GRN_FALSE;
  grn_id column_range_id;
  void *raw_value;
  unsigned int size;

  if (column->header.flags & GRN_OBJ_WITH_WEIGHT) {
    if (!IS_WEIGHT_UVECTOR(value)) {
      need_convert = GRN_TRUE;
    }
  } else {
    if (IS_WEIGHT_UVECTOR(value)) {
      need_convert = GRN_TRUE;
      uvector_flags = GRN_OBJ_WITH_WEIGHT;
    }
  }
  column_range_id = DB_OBJ(column)->range;
  if (column_range_id != value->header.domain) {
    need_convert = GRN_TRUE;
    need_cast = GRN_TRUE;
  }

  if (need_convert) {
    unsigned int i, n;

    GRN_VALUE_FIX_SIZE_INIT(&uvector, GRN_OBJ_VECTOR, column_range_id);
    uvector.header.flags |= uvector_flags;
    n = grn_uvector_size(ctx, value);
    if (need_cast) {
      grn_obj value_record;
      grn_obj casted_record;

      GRN_VALUE_FIX_SIZE_INIT(&value_record, 0, value->header.domain);
      GRN_VALUE_FIX_SIZE_INIT(&casted_record, 0, column_range_id);
      for (i = 0; i < n; i++) {
        grn_id id;
        grn_id casted_id;
        unsigned int weight = 0;

        GRN_BULK_REWIND(&value_record);
        GRN_BULK_REWIND(&casted_record);

        id = grn_uvector_get_element(ctx, value, i, NULL);
        GRN_RECORD_SET(ctx, &value_record, id);
        rc = grn_obj_cast(ctx, &value_record, &casted_record, GRN_TRUE);
        if (rc != GRN_SUCCESS) {
          char column_name[GRN_TABLE_MAX_KEY_SIZE];
          int column_name_size;
          grn_obj inspected;
          column_name_size = grn_obj_name(ctx,
                                          column,
                                          column_name,
                                          GRN_TABLE_MAX_KEY_SIZE);
          GRN_TEXT_INIT(&inspected, 0);
          grn_inspect(ctx, &inspected, &value_record);
          ERR(rc,
              "[column][set-value] failed to cast: <%.*s>: <%.*s>",
              column_name_size,
              column_name,
              (int)GRN_TEXT_LEN(&inspected),
              GRN_TEXT_VALUE(&inspected));
          GRN_OBJ_FIN(ctx, &inspected);
          break;
        }
        casted_id = GRN_RECORD_VALUE(&casted_record);
        grn_uvector_add_element(ctx, &uvector, casted_id, weight);
      }

      GRN_OBJ_FIN(ctx, &value_record);
      GRN_OBJ_FIN(ctx, &casted_record);
    } else {
      for (i = 0; i < n; i++) {
        grn_id id;
        unsigned int weight = 0;
        id = grn_uvector_get_element(ctx, value, i, NULL);
        grn_uvector_add_element(ctx, &uvector, id, weight);
      }
    }
    raw_value = GRN_BULK_HEAD(&uvector);
    size = GRN_BULK_VSIZE(&uvector);
  } else {
    raw_value = GRN_BULK_HEAD(value);
    size = GRN_BULK_VSIZE(value);
  }

  if (rc == GRN_SUCCESS) {
    rc = grn_ja_put(ctx, (grn_ja *)column, id, raw_value, size, flags, NULL);
  }

  if (need_convert) {
    GRN_OBJ_FIN(ctx, &uvector);
  }

  return rc;
}

static grn_rc
grn_obj_set_value_column_var_size_vector(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                         grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  unsigned int s = grn_obj_size(ctx, value);
  grn_obj *lexicon = grn_ctx_at(ctx, range);

  if (call_hook(ctx, obj, id, value, flags)) {
    if (ctx->rc) {
      rc = ctx->rc;
    }
    return rc;
  }

  if (value->header.type == GRN_UVECTOR) {
    rc = grn_obj_set_value_column_var_size_vector_uvector(ctx, obj,
                                                          id, value,
                                                          flags);
    return rc;
  }

  if (GRN_OBJ_TABLEP(lexicon)) {
    grn_obj uvector;
    GRN_RECORD_INIT(&uvector, GRN_OBJ_VECTOR, range);
    if (obj->header.flags & GRN_OBJ_WITH_WEIGHT) {
      uvector.header.flags |= GRN_OBJ_WITH_WEIGHT;
    }
    switch (value->header.type) {
    case GRN_BULK :
      {
        unsigned int token_flags = 0;
        grn_token_cursor *token_cursor;
        if (v && s &&
            (token_cursor = grn_token_cursor_open(ctx, lexicon, v, s,
                                                  GRN_TOKEN_ADD, token_flags))) {
          while (token_cursor->status == GRN_TOKEN_CURSOR_DOING) {
            grn_id tid = grn_token_cursor_next(ctx, token_cursor);
            grn_uvector_add_element(ctx, &uvector, tid, 0);
          }
          grn_token_cursor_close(ctx, token_cursor);
        }
        rc = grn_ja_put(ctx, (grn_ja *)obj, id,
                        GRN_BULK_HEAD(&uvector), GRN_BULK_VSIZE(&uvector),
                        flags, NULL);
      }
      break;
    case GRN_VECTOR :
      {
        unsigned int n;
        n = grn_vector_size(ctx, value);
        if (n > 0) {
          unsigned int i;
          grn_obj value_buf, cast_buf;
          GRN_OBJ_INIT(&value_buf, GRN_BULK, 0, GRN_DB_VOID);
          GRN_OBJ_INIT(&cast_buf, GRN_BULK, 0, lexicon->header.domain);
          for (i = 0; i < n; i++) {
            grn_id tid;
            const char *element;
            unsigned int element_length;
            unsigned int weight;
            grn_id element_domain;

            element_length = grn_vector_get_element(ctx, value, i,
                                                    &element, &weight,
                                                    &element_domain);
            if (element_domain != lexicon->header.domain) {
              GRN_BULK_REWIND(&cast_buf);
              GRN_BULK_REWIND(&value_buf);
              grn_bulk_write(ctx, &value_buf, element, element_length);
              value_buf.header.domain = element_domain;
              rc = grn_obj_cast(ctx, &value_buf, &cast_buf, GRN_TRUE);
              if (rc) {
                grn_obj *range_obj;
                range_obj = grn_ctx_at(ctx, range);
                ERR_CAST(obj, range_obj, &value_buf);
                grn_obj_unlink(ctx, range_obj);
              } else {
                element = GRN_BULK_HEAD(&cast_buf);
                element_length = GRN_BULK_VSIZE(&cast_buf);
              }
            } else {
              rc = GRN_SUCCESS;
            }
            if (rc) {
              continue;
            }
            tid = grn_table_add(ctx, lexicon, element, element_length, NULL);
            grn_uvector_add_element(ctx, &uvector, tid, weight);
          }
          GRN_OBJ_FIN(ctx, &value_buf);
          GRN_OBJ_FIN(ctx, &cast_buf);
        }
      }
      rc = grn_ja_put(ctx, (grn_ja *)obj, id,
                      GRN_BULK_HEAD(&uvector), GRN_BULK_VSIZE(&uvector),
                      flags, NULL);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "vector, uvector or bulk required");
      break;
    }
    grn_obj_close(ctx, &uvector);
  } else {
    switch (value->header.type) {
    case GRN_BULK :
      if (!GRN_BULK_VSIZE(value)) {
        rc = grn_ja_put(ctx, (grn_ja *)obj, id, NULL, 0, flags, NULL);
      } else {
        grn_obj v;
        GRN_OBJ_INIT(&v, GRN_VECTOR, GRN_OBJ_DO_SHALLOW_COPY, GRN_DB_TEXT);
        v.u.v.body = value;
        grn_vector_delimit(ctx, &v, 0, GRN_ID_NIL);
        rc = grn_ja_putv(ctx, (grn_ja *)obj, id, &v, 0);
        grn_obj_close(ctx, &v);
      }
      break;
    case GRN_VECTOR :
      rc = grn_ja_putv(ctx, (grn_ja *)obj, id, value, 0);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "vector or bulk required");
      break;
    }
  }
  return rc;
}

static grn_rc
grn_obj_set_value_column_fix_size(grn_ctx *ctx, grn_obj *obj, grn_id id,
                                  grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_id range = DB_OBJ(obj)->range;
  void *v = GRN_BULK_HEAD(value);
  unsigned int s = grn_obj_size(ctx, value);
  grn_obj buf, *value_ = value;
  uint32_t element_size = ((grn_ra *)obj)->header->element_size;
  GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
  if (range != value->header.domain) {
    rc = grn_obj_cast(ctx, value, &buf, GRN_TRUE);
    if (rc) {
      grn_obj *range_obj;
      range_obj = grn_ctx_at(ctx, range);
      ERR_CAST(obj, range_obj, value);
      grn_obj_unlink(ctx, range_obj);
    } else {
      value_ = &buf;
      v = GRN_BULK_HEAD(&buf);
      s = GRN_BULK_VSIZE(&buf);
    }
  } else {
    rc = GRN_SUCCESS;
  }
  if (rc) {
    /* do nothing because it already has error. */
  } else if (element_size < s) {
    ERR(GRN_INVALID_ARGUMENT, "too long value (%d)", s);
  } else {
    void *p = grn_ra_ref(ctx, (grn_ra *)obj, id);
    if (!p) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "ra get failed");
      rc = GRN_NO_MEMORY_AVAILABLE;
      return rc;
    }
    switch (flags & GRN_OBJ_SET_MASK) {
    case GRN_OBJ_SET :
      if (call_hook(ctx, obj, id, value_, flags)) {
        if (ctx->rc) {
          rc = ctx->rc;
        }
        GRN_OBJ_FIN(ctx, &buf);
        grn_ra_unref(ctx, (grn_ra *)obj, id);
        return rc;
      }
      if (element_size != s) {
        if (!s) {
          memset(p, 0, element_size);
        } else {
          void *b;
          if ((b = GRN_CALLOC(element_size))) {
            grn_memcpy(b, v, s);
            grn_memcpy(p, b, element_size);
            GRN_FREE(b);
          }
        }
      } else {
        grn_memcpy(p, v, s);
      }
      rc = GRN_SUCCESS;
      break;
    case GRN_OBJ_INCR :
      /* todo : support hook */
      INCRDECR(+=);
      break;
    case GRN_OBJ_DECR :
      /* todo : support hook */
      INCRDECR(-=);
      break;
    default :
      rc = GRN_OPERATION_NOT_SUPPORTED;
      break;
    }
    grn_ra_unref(ctx, (grn_ra *)obj, id);
  }
  GRN_OBJ_FIN(ctx, &buf);
  return rc;
}

static grn_rc
grn_obj_set_value_column_index(grn_ctx *ctx, grn_obj *obj, grn_id id,
                               grn_obj *value, int flags)
{
  char column_name[GRN_TABLE_MAX_KEY_SIZE];
  int column_name_size;
  column_name_size = grn_obj_name(ctx, obj, column_name,
                                  GRN_TABLE_MAX_KEY_SIZE);
  ERR(GRN_INVALID_ARGUMENT,
      "can't set value to index column directly: <%.*s>",
      column_name_size, column_name);
  return ctx->rc;
}

grn_rc
grn_obj_set_value(grn_ctx *ctx, grn_obj *obj, grn_id id,
                  grn_obj *value, int flags)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!GRN_DB_OBJP(obj)) {
    if (obj->header.type == GRN_ACCESSOR) {
      rc = grn_accessor_set_value(ctx, (grn_accessor *)obj, id, value, flags);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "not db_obj");
    }
  } else {
    switch (obj->header.type) {
    case GRN_TABLE_PAT_KEY :
      rc = grn_obj_set_value_table_pat_key(ctx, obj, id, value, flags);
      break;
    case GRN_TABLE_DAT_KEY :
      rc = GRN_OPERATION_NOT_SUPPORTED;
      break;
    case GRN_TABLE_HASH_KEY :
      rc = grn_obj_set_value_table_hash_key(ctx, obj, id, value, flags);
      break;
    case GRN_TABLE_NO_KEY :
      rc = grn_obj_set_value_table_no_key(ctx, obj, id, value, flags);
      break;
    case GRN_COLUMN_VAR_SIZE :
      switch (obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
      case GRN_OBJ_COLUMN_SCALAR :
        rc = grn_obj_set_value_column_var_size_scalar(ctx, obj, id, value,
                                                      flags);
        break;
      case GRN_OBJ_COLUMN_VECTOR :
        rc = grn_obj_set_value_column_var_size_vector(ctx, obj, id, value,
                                                      flags);
        break;
      default :
        ERR(GRN_FILE_CORRUPT, "invalid GRN_OBJ_COLUMN_TYPE");
        break;
      }
      break;
    case GRN_COLUMN_FIX_SIZE :
      rc = grn_obj_set_value_column_fix_size(ctx, obj, id, value, flags);
      break;
    case GRN_COLUMN_INDEX :
      rc = grn_obj_set_value_column_index(ctx, obj, id, value, flags);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

const char *
grn_obj_get_value_(grn_ctx *ctx, grn_obj *obj, grn_id id, uint32_t *size)
{
  const char *value = NULL;
  *size = 0;
  switch (obj->header.type) {
  case GRN_ACCESSOR :
    value = grn_accessor_get_value_(ctx, (grn_accessor *)obj, id, size);
    break;
  case GRN_TABLE_PAT_KEY :
    value = grn_pat_get_value_(ctx, (grn_pat *)obj, id, size);
    break;
  case GRN_TABLE_DAT_KEY :
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "GRN_TABLE_DAT_KEY not supported");
    break;
  case GRN_TABLE_HASH_KEY :
    value = grn_hash_get_value_(ctx, (grn_hash *)obj, id, size);
    break;
  case GRN_TABLE_NO_KEY :
    if ((value = _grn_array_get_value(ctx, (grn_array *)obj, id))) {
      *size = ((grn_array *)obj)->value_size;
    }
    break;
  case GRN_COLUMN_VAR_SIZE :
    {
      grn_io_win jw;
      if ((value = grn_ja_ref(ctx, (grn_ja *)obj, id, &jw, size))) {
        grn_ja_unref(ctx, &jw);
      }
    }
    break;
  case GRN_COLUMN_FIX_SIZE :
    if ((value = grn_ra_ref(ctx, (grn_ra *)obj, id))) {
      grn_ra_unref(ctx, (grn_ra *)obj, id);
      *size = ((grn_ra *)obj)->header->element_size;
    }
    break;
  case GRN_COLUMN_INDEX :
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "todo: GRN_COLUMN_INDEX");
    break;
  }
  return value;
}

static void
grn_obj_get_value_expr(grn_ctx *ctx, grn_obj *expr, grn_id id, grn_obj *value)
{
  grn_expr *e = (grn_expr *)expr;
  grn_expr_code *code;

  if (e->codes_curr != 1) {
    return;
  }

  code = e->codes;
  if (code->op != GRN_OP_GET_VALUE) {
    return;
  }

  if (!code->value) {
    return;
  }

  switch (code->value->header.type) {
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_FIX_SIZE :
    grn_obj_get_value(ctx, code->value, id, value);
    break;
  default :
    break;
  }
}

static void
grn_obj_get_value_column_index(grn_ctx *ctx, grn_obj *index_column,
                               grn_id id, grn_obj *value)
{
  grn_ii *ii = (grn_ii *)index_column;
  grn_obj_ensure_bulk(ctx, value);
  if (id) {
    GRN_UINT32_SET(ctx, value, grn_ii_estimate_size(ctx, ii, id));
  } else {
    GRN_UINT32_SET(ctx, value, 0);
  }
  value->header.domain = GRN_DB_UINT32;
}

static grn_obj *
grn_obj_get_value_column_vector(grn_ctx *ctx, grn_obj *obj,
                                grn_id id, grn_obj *value)
{
  grn_obj *lexicon;

  lexicon = grn_ctx_at(ctx, DB_OBJ(obj)->range);
  if (lexicon && !GRN_OBJ_TABLEP(lexicon) &&
      (lexicon->header.flags & GRN_OBJ_KEY_VAR_SIZE)) {
    grn_obj_ensure_vector(ctx, value);
    if (id) {
      grn_obj v_;
      GRN_TEXT_INIT(&v_, 0);
      grn_ja_get_value(ctx, (grn_ja *)obj, id, &v_);
      grn_vector_decode(ctx, value, GRN_TEXT_VALUE(&v_), GRN_TEXT_LEN(&v_));
      GRN_OBJ_FIN(ctx, &v_);
    }
  } else {
    grn_obj_ensure_bulk(ctx, value);
    if (id) {
      grn_ja_get_value(ctx, (grn_ja *)obj, id, value);
    }
    value->header.type = GRN_UVECTOR;
    if (obj->header.flags & GRN_OBJ_WITH_WEIGHT) {
      value->header.flags |= GRN_OBJ_WITH_WEIGHT;
    } else {
      value->header.flags &= ~GRN_OBJ_WITH_WEIGHT;
    }
  }

  return value;
}

grn_obj *
grn_obj_get_value(grn_ctx *ctx, grn_obj *obj, grn_id id, grn_obj *value)
{
  GRN_API_ENTER;
  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_value failed");
    goto exit;
  }
  if (!value) {
    if (!(value = grn_obj_open(ctx, GRN_BULK, 0, 0))) {
      ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_value failed");
      goto exit;
    }
  }
  switch (value->header.type) {
  case GRN_VOID :
    grn_obj_reinit(ctx, value, GRN_DB_TEXT, 0);
    break;
  case GRN_BULK :
  case GRN_VECTOR :
  case GRN_UVECTOR :
  case GRN_MSG :
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_value failed");
    goto exit;
  }
  switch (obj->header.type) {
  case GRN_ACCESSOR :
    grn_obj_ensure_bulk(ctx, value);
    value = grn_accessor_get_value(ctx, (grn_accessor *)obj, id, value);
    break;
  case GRN_EXPR :
    grn_obj_get_value_expr(ctx, obj, id, value);
    break;
  case GRN_TABLE_PAT_KEY :
    {
      grn_pat *pat = (grn_pat *)obj;
      uint32_t size = pat->value_size;
      grn_obj_ensure_bulk(ctx, value);
      if (id) {
        if (grn_bulk_space(ctx, value, size)) {
          MERR("grn_bulk_space failed");
          goto exit;
        }
        {
          char *curr = GRN_BULK_CURR(value);
          grn_pat_get_value(ctx, pat, id, curr - size);
        }
      }
      value->header.type = GRN_BULK;
      value->header.domain = grn_obj_get_range(ctx, obj);
    }
    break;
  case GRN_TABLE_DAT_KEY :
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "GRN_TABLE_DAT_KEY not supported");
    break;
  case GRN_TABLE_HASH_KEY :
    {
      grn_bool processed = GRN_FALSE;
      grn_obj_ensure_bulk(ctx, value);
      value->header.domain = grn_obj_get_range(ctx, obj);
      if (id) {
        if (GRN_TABLE_IS_MULTI_KEYS_GROUPED(obj)) {
          grn_obj *domain;
          domain = grn_ctx_at(ctx, value->header.domain);
          if (GRN_OBJ_TABLEP(domain)) {
            grn_id subrec_id;
            if (grn_table_get_subrecs(ctx, obj, id, &subrec_id, NULL, 1) == 1) {
              GRN_RECORD_SET(ctx, value, subrec_id);
              processed = GRN_TRUE;
            }
          }
        }
        if (!processed) {
          grn_hash *hash = (grn_hash *)obj;
          uint32_t size = hash->value_size;
          if (grn_bulk_space(ctx, value, size)) {
            MERR("grn_bulk_space failed");
            goto exit;
          }
          {
            char *curr = GRN_BULK_CURR(value);
            grn_hash_get_value(ctx, hash, id, curr - size);
          }
        }
      }
    }
    break;
  case GRN_TABLE_NO_KEY :
    {
      grn_array *array = (grn_array *)obj;
      uint32_t size = array->value_size;
      grn_obj_ensure_bulk(ctx, value);
      if (id) {
        if (grn_bulk_space(ctx, value, size)) {
          MERR("grn_bulk_space failed");
          goto exit;
        }
        {
          char *curr = GRN_BULK_CURR(value);
          grn_array_get_value(ctx, array, id, curr - size);
        }
      }
      value->header.type = GRN_BULK;
      value->header.domain = grn_obj_get_range(ctx, obj);
    }
    break;
  case GRN_COLUMN_VAR_SIZE :
    switch (obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
    case GRN_OBJ_COLUMN_VECTOR :
      grn_obj_get_value_column_vector(ctx, obj, id, value);
      break;
    case GRN_OBJ_COLUMN_SCALAR :
      grn_obj_ensure_bulk(ctx, value);
      if (id) {
        grn_ja_get_value(ctx, (grn_ja *)obj, id, value);
      }
      value->header.type = GRN_BULK;
      break;
    default :
      ERR(GRN_FILE_CORRUPT, "invalid GRN_OBJ_COLUMN_TYPE");
      break;
    }
    value->header.domain = grn_obj_get_range(ctx, obj);
    break;
  case GRN_COLUMN_FIX_SIZE :
    grn_obj_ensure_bulk(ctx, value);
    value->header.type = GRN_BULK;
    value->header.domain = grn_obj_get_range(ctx, obj);
    if (id) {
      unsigned int element_size;
      void *v = grn_ra_ref(ctx, (grn_ra *)obj, id);
      if (v) {
        element_size = ((grn_ra *)obj)->header->element_size;
        grn_bulk_write(ctx, value, v, element_size);
        grn_ra_unref(ctx, (grn_ra *)obj, id);
      }
    }
    break;
  case GRN_COLUMN_INDEX :
    grn_obj_get_value_column_index(ctx, obj, id, value);
    break;
  }
exit :
  GRN_API_RETURN(value);
}

int
grn_obj_get_values(grn_ctx *ctx, grn_obj *obj, grn_id offset, void **values)
{
  int nrecords = -1;
  GRN_API_ENTER;
  if (obj->header.type == GRN_COLUMN_FIX_SIZE) {
    grn_obj *domain = grn_column_table(ctx, obj);
    if (domain) {
      int table_size = (int)grn_table_size(ctx, domain);
      if (0 < offset && offset <= (grn_id) table_size) {
        grn_ra *ra = (grn_ra *)obj;
        void *p = grn_ra_ref(ctx, ra, offset);
        if (p) {
          if ((offset >> ra->element_width) == ((unsigned int) table_size >> ra->element_width)) {
            nrecords = (table_size & ra->element_mask) + 1 - (offset & ra->element_mask);
          } else {
            nrecords = ra->element_mask + 1 - (offset & ra->element_mask);
          }
          if (values) { *values = p; }
          grn_ra_unref(ctx, ra, offset);
        } else {
          ERR(GRN_NO_MEMORY_AVAILABLE, "ra get failed");
        }
      } else {
        nrecords = 0;
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT, "no domain found");
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "obj is not a fix sized column");
  }
  GRN_API_RETURN(nrecords);
}

grn_rc
grn_column_index_update(grn_ctx *ctx, grn_obj *column,
                        grn_id id, unsigned int section,
                        grn_obj *oldvalue, grn_obj *newvalue)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (column->header.type != GRN_COLUMN_INDEX) {
    ERR(GRN_INVALID_ARGUMENT, "invalid column assigned");
  } else {
    rc = grn_ii_column_update(ctx, (grn_ii *)column, id, section, oldvalue, newvalue, NULL);
  }
  GRN_API_RETURN(rc);
}

grn_obj *
grn_column_table(grn_ctx *ctx, grn_obj *column)
{
  grn_obj *obj = NULL;
  grn_db_obj *col = DB_OBJ(column);
  GRN_API_ENTER;
  if (col) {
    obj = grn_ctx_at(ctx, col->header.domain);
  }
  GRN_API_RETURN(obj);
}

grn_obj *
grn_obj_get_info(grn_ctx *ctx, grn_obj *obj, grn_info_type type, grn_obj *valuebuf)
{
  GRN_API_ENTER;
  switch (type) {
  case GRN_INFO_SUPPORT_ZLIB :
    if (!valuebuf && !(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_BOOL))) {
      ERR(GRN_INVALID_ARGUMENT,
          "failed to open value buffer for GRN_INFO_ZLIB_SUPPORT");
      goto exit;
    }
#ifdef GRN_WITH_ZLIB
    GRN_BOOL_PUT(ctx, valuebuf, GRN_TRUE);
#else
    GRN_BOOL_PUT(ctx, valuebuf, GRN_FALSE);
#endif
    break;
  case GRN_INFO_SUPPORT_LZ4 :
    if (!valuebuf && !(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_BOOL))) {
      ERR(GRN_INVALID_ARGUMENT,
          "failed to open value buffer for GRN_INFO_LZ4_SUPPORT");
      goto exit;
    }
#ifdef GRN_WITH_LZ4
    GRN_BOOL_PUT(ctx, valuebuf, GRN_TRUE);
#else /* GRN_WITH_LZ4 */
    GRN_BOOL_PUT(ctx, valuebuf, GRN_FALSE);
#endif /* GRN_WITH_LZ4 */
    break;
  case GRN_INFO_SUPPORT_ZSTD :
    if (!valuebuf && !(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_BOOL))) {
      ERR(GRN_INVALID_ARGUMENT,
          "failed to open value buffer for GRN_INFO_ZSTD_SUPPORT");
      goto exit;
    }
#ifdef GRN_WITH_ZSTD
    GRN_BOOL_PUT(ctx, valuebuf, GRN_TRUE);
#else /* GRN_WITH_ZSTD */
    GRN_BOOL_PUT(ctx, valuebuf, GRN_FALSE);
#endif /* GRN_WITH_ZSTD */
    break;
  case GRN_INFO_SUPPORT_ARROW :
    if (!valuebuf && !(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_BOOL))) {
      ERR(GRN_INVALID_ARGUMENT,
          "failed to open value buffer for GRN_INFO_ARROW_SUPPORT");
      goto exit;
    }
#ifdef GRN_WITH_ARROW
    GRN_BOOL_PUT(ctx, valuebuf, GRN_TRUE);
#else /* GRN_WITH_ARROW */
    GRN_BOOL_PUT(ctx, valuebuf, GRN_FALSE);
#endif /* GRN_WITH_ARROW */
    break;
  default :
    if (!obj) {
      ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_info failed");
      goto exit;
    }
    switch (type) {
    case GRN_INFO_ENCODING :
      if (!valuebuf) {
        if (!(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, 0))) {
          ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_info failed");
          goto exit;
        }
      }
      {
        grn_encoding enc;
        if (obj->header.type == GRN_DB) { obj = ((grn_db *)obj)->keys; }
        switch (obj->header.type) {
        case GRN_TABLE_PAT_KEY :
          enc = ((grn_pat *)obj)->encoding;
          grn_bulk_write(ctx, valuebuf, (const char *)&enc, sizeof(grn_encoding));
          break;
        case GRN_TABLE_DAT_KEY :
          enc = ((grn_dat *)obj)->encoding;
          grn_bulk_write(ctx, valuebuf, (const char *)&enc, sizeof(grn_encoding));
          break;
        case GRN_TABLE_HASH_KEY :
          enc = ((grn_hash *)obj)->encoding;
          grn_bulk_write(ctx, valuebuf, (const char *)&enc, sizeof(grn_encoding));
          break;
        default :
          ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_info failed");
        }
      }
      break;
    case GRN_INFO_SOURCE :
      if (!valuebuf) {
        if (!(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, 0))) {
          ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_info failed");
          goto exit;
        }
      }
      if (!GRN_DB_OBJP(obj)) {
        ERR(GRN_INVALID_ARGUMENT, "only db_obj can accept GRN_INFO_SOURCE");
        goto exit;
      }
      grn_bulk_write(ctx, valuebuf, DB_OBJ(obj)->source, DB_OBJ(obj)->source_size);
      break;
    case GRN_INFO_DEFAULT_TOKENIZER :
      switch (DB_OBJ(obj)->header.type) {
      case GRN_TABLE_HASH_KEY :
        valuebuf = ((grn_hash *)obj)->tokenizer;
        break;
      case GRN_TABLE_PAT_KEY :
        valuebuf = ((grn_pat *)obj)->tokenizer;
        break;
      case GRN_TABLE_DAT_KEY :
        valuebuf = ((grn_dat *)obj)->tokenizer;
        break;
      }
      break;
    case GRN_INFO_NORMALIZER :
      switch (DB_OBJ(obj)->header.type) {
      case GRN_TABLE_HASH_KEY :
        valuebuf = ((grn_hash *)obj)->normalizer;
        break;
      case GRN_TABLE_PAT_KEY :
        valuebuf = ((grn_pat *)obj)->normalizer;
        break;
      case GRN_TABLE_DAT_KEY :
        valuebuf = ((grn_dat *)obj)->normalizer;
        break;
      }
      break;
    case GRN_INFO_TOKEN_FILTERS :
      if (!valuebuf) {
        if (!(valuebuf = grn_obj_open(ctx, GRN_PVECTOR, 0, 0))) {
          ERR(GRN_NO_MEMORY_AVAILABLE,
              "grn_obj_get_info: failed to allocate value buffer");
          goto exit;
        }
      }
      {
        grn_obj *token_filters = NULL;
        switch (obj->header.type) {
        case GRN_TABLE_HASH_KEY :
          token_filters = &(((grn_hash *)obj)->token_filters);
          break;
        case GRN_TABLE_PAT_KEY :
          token_filters = &(((grn_pat *)obj)->token_filters);
          break;
        case GRN_TABLE_DAT_KEY :
          token_filters = &(((grn_dat *)obj)->token_filters);
          break;
        default :
          ERR(GRN_INVALID_ARGUMENT,
              /* TODO: Show type name instead of type ID */
              "[info][get][token-filters] target object must be one of "
              "GRN_TABLE_HASH_KEY, GRN_TABLE_PAT_KEY and GRN_TABLE_DAT_KEY: %d",
              obj->header.type);
          break;
        }
        if (token_filters) {
          grn_bulk_write(ctx,
                         valuebuf,
                         GRN_BULK_HEAD(token_filters),
                         GRN_BULK_VSIZE(token_filters));
        }
      }
      break;
    default :
      /* todo */
      break;
    }
  }
exit :
  GRN_API_RETURN(valuebuf);
}

static void
update_source_hook(grn_ctx *ctx, grn_obj *obj)
{
  grn_id *s = DB_OBJ(obj)->source;
  int i, n = DB_OBJ(obj)->source_size / sizeof(grn_id);
  grn_obj_default_set_value_hook_data hook_data = { DB_OBJ(obj)->id, 0 };
  grn_obj *source, data;
  GRN_TEXT_INIT(&data, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET_REF(&data, &hook_data, sizeof(hook_data));
  for (i = 1; i <= n; i++, s++) {
    hook_data.section = i;
    if ((source = grn_ctx_at(ctx, *s))) {
      switch (source->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        grn_obj_add_hook(ctx, source, GRN_HOOK_INSERT, 0, NULL, &data);
        grn_obj_add_hook(ctx, source, GRN_HOOK_DELETE, 0, NULL, &data);
        break;
      case GRN_COLUMN_FIX_SIZE :
      case GRN_COLUMN_VAR_SIZE :
      case GRN_COLUMN_INDEX :
        grn_obj_add_hook(ctx, source, GRN_HOOK_SET, 0, NULL, &data);
        break;
      default :
        /* invalid target */
        break;
      }
    }
  }
  grn_obj_close(ctx, &data);
}

static void
del_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry, grn_obj *hld)
{
  int i;
  void *hld_value = NULL;
  uint32_t hld_size = 0;
  grn_hook **last;
  hld_value = GRN_BULK_HEAD(hld);
  hld_size = GRN_BULK_VSIZE(hld);
  if (!hld_size) { return; }
  for (i = 0, last = &DB_OBJ(obj)->hooks[entry]; *last; i++, last = &(*last)->next) {
    if (!memcmp(GRN_NEXT_ADDR(*last), hld_value, hld_size)) {
      grn_obj_delete_hook(ctx, obj, entry, i);
      return;
    }
  }
}

static void
delete_source_hook(grn_ctx *ctx, grn_obj *obj)
{
  grn_id *s = DB_OBJ(obj)->source;
  int i, n = DB_OBJ(obj)->source_size / sizeof(grn_id);
  grn_obj_default_set_value_hook_data hook_data = { DB_OBJ(obj)->id, 0 };
  grn_obj *source, data;
  GRN_TEXT_INIT(&data, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET_REF(&data, &hook_data, sizeof(hook_data));
  for (i = 1; i <= n; i++, s++) {
    hook_data.section = i;

    source = grn_ctx_at(ctx, *s);
    if (!source) {
      ERRCLR(ctx);
      continue;
    }

    switch (source->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
      del_hook(ctx, source, GRN_HOOK_INSERT, &data);
      del_hook(ctx, source, GRN_HOOK_DELETE, &data);
      break;
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_VAR_SIZE :
      del_hook(ctx, source, GRN_HOOK_SET, &data);
      break;
    default :
      /* invalid target */
      break;
    }
  }
  grn_obj_close(ctx, &data);
}

#define N_HOOK_ENTRIES 5

grn_rc
grn_hook_pack(grn_ctx *ctx, grn_db_obj *obj, grn_obj *buf)
{
  grn_rc rc;
  grn_hook_entry e;
  for (e = 0; e < N_HOOK_ENTRIES; e++) {
    grn_hook *hooks;
    for (hooks = obj->hooks[e]; hooks; hooks = hooks->next) {
      grn_id id = hooks->proc ? hooks->proc->obj.id : 0;
      if ((rc = grn_text_benc(ctx, buf, id + 1))) { goto exit; }
      if ((rc = grn_text_benc(ctx, buf, hooks->hld_size))) { goto exit; }
      if ((rc = grn_bulk_write(ctx, buf, (char *)GRN_NEXT_ADDR(hooks), hooks->hld_size))) { goto exit; }
    }
    if ((rc = grn_text_benc(ctx, buf, 0))) { goto exit; }
  }
exit :
  return rc;
}

static grn_rc
grn_hook_unpack(grn_ctx *ctx, grn_db_obj *obj, const char *buf, uint32_t buf_size)
{
  grn_hook_entry e;
  const uint8_t *p = (uint8_t *)buf, *pe = p + buf_size;
  for (e = 0; e < N_HOOK_ENTRIES; e++) {
    grn_hook *new, **last = &obj->hooks[e];
    for (;;) {
      grn_id id;
      uint32_t hld_size;
      GRN_B_DEC(id, p);
      if (!id--) { break; }
      if (p >= pe) { return GRN_FILE_CORRUPT; }
      GRN_B_DEC(hld_size, p);
      if (p >= pe) { return GRN_FILE_CORRUPT; }
      if (!(new = GRN_MALLOC(sizeof(grn_hook) + hld_size))) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      if (id) {
        new->proc = (grn_proc *)grn_ctx_at(ctx, id);
        if (!new->proc) {
          GRN_FREE(new);
          return ctx->rc;
        }
      } else {
        new->proc = NULL;
      }
      if ((new->hld_size = hld_size)) {
        grn_memcpy(GRN_NEXT_ADDR(new), p, hld_size);
        p += hld_size;
      }
      *last = new;
      last = &new->next;
      if (p >= pe) { return GRN_FILE_CORRUPT; }
    }
    *last = NULL;
  }
  return GRN_SUCCESS;
}

static void
grn_token_filters_pack(grn_ctx *ctx,
                       grn_obj *token_filters,
                       grn_obj *buffer)
{
  unsigned int i, n_token_filters;

  n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);
  for (i = 0; i < n_token_filters; i++) {
    grn_obj *token_filter = GRN_PTR_VALUE_AT(token_filters, i);
    grn_id token_filter_id;

    token_filter_id = grn_obj_id(ctx, token_filter);
    GRN_RECORD_PUT(ctx, buffer, token_filter_id);
  }
}

static grn_bool
grn_obj_encoded_spec_equal(grn_ctx *ctx,
                           grn_obj *encoded_spec1,
                           grn_obj *encoded_spec2)
{
  unsigned int i, n_elements;

  if (encoded_spec1->header.type != GRN_VECTOR) {
    return GRN_FALSE;
  }

  if (encoded_spec1->header.type != encoded_spec2->header.type) {
    return GRN_FALSE;
  }

  n_elements = grn_vector_size(ctx, encoded_spec1);
  if (grn_vector_size(ctx, encoded_spec2) != n_elements) {
    return GRN_FALSE;
  }

  for (i = 0; i < n_elements; i++) {
    const char *content1;
    const char *content2;
    unsigned int content_size1;
    unsigned int content_size2;
    unsigned int weight1;
    unsigned int weight2;
    grn_id domain1;
    grn_id domain2;

    content_size1 = grn_vector_get_element(ctx,
                                           encoded_spec1,
                                           i,
                                           &content1,
                                           &weight1,
                                           &domain1);
    content_size2 = grn_vector_get_element(ctx,
                                           encoded_spec2,
                                           i,
                                           &content2,
                                           &weight2,
                                           &domain2);
    if (content_size1 != content_size2) {
      return GRN_FALSE;
    }
    if (memcmp(content1, content2, content_size1) != 0) {
      return GRN_FALSE;
    }
    if (weight1 != weight2) {
      return GRN_FALSE;
    }
    if (domain1 != domain2) {
      return GRN_FALSE;
    }
  }

  return GRN_TRUE;
}

void
grn_obj_spec_save(grn_ctx *ctx, grn_db_obj *obj)
{
  grn_db *s;
  grn_obj v, *b;
  grn_obj_spec spec;
  grn_bool need_update = GRN_TRUE;

  if (obj->id & GRN_OBJ_TMP_OBJECT) { return; }
  if (!ctx->impl || !GRN_DB_OBJP(obj)) { return; }
  if (!(s = (grn_db *)ctx->impl->db) || !s->specs) { return; }
  if (obj->header.type == GRN_PROC && obj->range == GRN_ID_NIL) {
    return;
  }
  GRN_OBJ_INIT(&v, GRN_VECTOR, 0, GRN_DB_TEXT);
  if (!(b = grn_vector_body(ctx, &v))) { return; }
  spec.header = obj->header;
  spec.range = obj->range;
  grn_bulk_write(ctx, b, (void *)&spec, sizeof(grn_obj_spec));
  grn_vector_delimit(ctx, &v, 0, 0);
  if (obj->header.flags & GRN_OBJ_CUSTOM_NAME) {
    GRN_TEXT_PUTS(ctx, b, grn_obj_path(ctx, (grn_obj *)obj));
  }
  grn_vector_delimit(ctx, &v, 0, 0);
  grn_bulk_write(ctx, b, obj->source, obj->source_size);
  grn_vector_delimit(ctx, &v, 0, 0);
  grn_hook_pack(ctx, obj, b);
  grn_vector_delimit(ctx, &v, 0, 0);
  switch (obj->header.type) {
  case GRN_TABLE_HASH_KEY :
    grn_token_filters_pack(ctx, &(((grn_hash *)obj)->token_filters), b);
    grn_vector_delimit(ctx, &v, 0, 0);
    break;
  case GRN_TABLE_PAT_KEY :
    grn_token_filters_pack(ctx, &(((grn_pat *)obj)->token_filters), b);
    grn_vector_delimit(ctx, &v, 0, 0);
    break;
  case GRN_TABLE_DAT_KEY :
    grn_token_filters_pack(ctx, &(((grn_dat *)obj)->token_filters), b);
    grn_vector_delimit(ctx, &v, 0, 0);
    break;
  case GRN_EXPR :
    grn_expr_pack(ctx, b, (grn_obj *)obj);
    grn_vector_delimit(ctx, &v, 0, 0);
    break;
  }

  {
    grn_io_win jw;
    uint32_t current_spec_raw_len;
    char *current_spec_raw;

    current_spec_raw = grn_ja_ref(ctx,
                                  s->specs,
                                  obj->id,
                                  &jw,
                                  &current_spec_raw_len);
    if (current_spec_raw) {
      grn_rc rc;
      grn_obj current_spec;

      GRN_OBJ_INIT(&current_spec, GRN_VECTOR, 0, GRN_DB_TEXT);
      rc = grn_vector_decode(ctx,
                             &current_spec,
                             current_spec_raw,
                             current_spec_raw_len);
      if (rc == GRN_SUCCESS) {
        need_update = !grn_obj_encoded_spec_equal(ctx, &v, &current_spec);
      }
      GRN_OBJ_FIN(ctx, &current_spec);
      grn_ja_unref(ctx, &jw);
    }
  }

  if (!need_update) {
    grn_obj_close(ctx, &v);
    return;
  }

  {
    const char *name;
    uint32_t name_size = 0;
    const char *range_name = NULL;
    uint32_t range_name_size = 0;

    name = _grn_table_key(ctx, s->keys, obj->id, &name_size);
    switch (obj->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_INDEX :
      if (obj->range != GRN_ID_NIL) {
        range_name = _grn_table_key(ctx, s->keys, obj->range, &range_name_size);
      }
      break;
    default :
      break;
    }
    /* TODO: reduce log level. */
    GRN_LOG(ctx, GRN_LOG_NOTICE,
            "spec:%u:update:%.*s:%u(%s):%u%s%.*s%s",
            obj->id,
            name_size, name,
            obj->header.type,
            grn_obj_type_to_string(obj->header.type),
            obj->range,
            range_name_size == 0 ? "" : "(",
            range_name_size, range_name,
            range_name_size == 0 ? "" : ")");
  }
  grn_ja_putv(ctx, s->specs, obj->id, &v, 0);
  grn_obj_close(ctx, &v);
}

inline static void
grn_obj_set_info_source_invalid_lexicon_error(grn_ctx *ctx,
                                              const char *message,
                                              grn_obj *actual_type,
                                              grn_obj *expected_type,
                                              grn_obj *index_column,
                                              grn_obj *source)
{
  char actual_type_name[GRN_TABLE_MAX_KEY_SIZE];
  int actual_type_name_size;
  char expected_type_name[GRN_TABLE_MAX_KEY_SIZE];
  int expected_type_name_size;
  char index_column_name[GRN_TABLE_MAX_KEY_SIZE];
  int index_column_name_size;
  char source_name[GRN_TABLE_MAX_KEY_SIZE];
  int source_name_size;

  actual_type_name_size = grn_obj_name(ctx, actual_type,
                                       actual_type_name,
                                       GRN_TABLE_MAX_KEY_SIZE);
  expected_type_name_size = grn_obj_name(ctx, expected_type,
                                         expected_type_name,
                                         GRN_TABLE_MAX_KEY_SIZE);
  index_column_name_size = grn_obj_name(ctx, index_column,
                                        index_column_name,
                                        GRN_TABLE_MAX_KEY_SIZE);

  source_name_size = grn_obj_name(ctx, source,
                                  source_name, GRN_TABLE_MAX_KEY_SIZE);
  if (grn_obj_is_table(ctx, source)) {
    source_name[source_name_size] = '\0';
    grn_strncat(source_name,
                GRN_TABLE_MAX_KEY_SIZE,
                "._key",
                GRN_TABLE_MAX_KEY_SIZE - source_name_size - 1);
    source_name_size = strlen(source_name);
  }

  ERR(GRN_INVALID_ARGUMENT,
      "[column][index][source] %s: "
      "<%.*s> -> <%.*s>: "
      "index-column:<%.*s> "
      "source:<%.*s>",
      message,
      actual_type_name_size, actual_type_name,
      expected_type_name_size, expected_type_name,
      index_column_name_size, index_column_name,
      source_name_size, source_name);
}

inline static grn_rc
grn_obj_set_info_source_validate(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  grn_id lexicon_id;
  grn_obj *lexicon = NULL;
  grn_id lexicon_domain_id;
  grn_obj *lexicon_domain = NULL;
  grn_bool lexicon_domain_is_table;
  grn_bool lexicon_have_tokenizer;
  grn_id *source_ids;
  int i, n_source_ids;

  lexicon_id = obj->header.domain;
  lexicon = grn_ctx_at(ctx, lexicon_id);
  if (!lexicon) {
    goto exit;
  }

  lexicon_domain_id = lexicon->header.domain;
  lexicon_domain = grn_ctx_at(ctx, lexicon_domain_id);
  if (!lexicon_domain) {
    goto exit;
  }

  source_ids = (grn_id *)GRN_BULK_HEAD(value);
  n_source_ids = GRN_BULK_VSIZE(value) / sizeof(grn_id);
  if (n_source_ids > 1 && !(obj->header.flags & GRN_OBJ_WITH_SECTION)) {
    char index_name[GRN_TABLE_MAX_KEY_SIZE];
    int index_name_size;
    index_name_size = grn_obj_name(ctx, obj,
                                   index_name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_INVALID_ARGUMENT,
        "grn_obj_set_info(): GRN_INFO_SOURCE: "
        "multi column index must be created with WITH_SECTION flag: <%.*s>",
        index_name_size, index_name);
    goto exit;
  }

  lexicon_domain_is_table = grn_obj_is_table(ctx, lexicon_domain);
  {
    grn_obj *tokenizer;
    grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL, NULL);
    lexicon_have_tokenizer = (tokenizer != NULL);
  }

  for (i = 0; i < n_source_ids; i++) {
    grn_id source_id = source_ids[i];
    grn_obj *source;
    grn_id source_type_id;
    grn_obj *source_type;

    source = grn_ctx_at(ctx, source_id);
    if (!source) {
      continue;
    }
    if (grn_obj_is_table(ctx, source)) {
      source_type_id = source->header.domain;
    } else {
      source_type_id = DB_OBJ(source)->range;
    }
    source_type = grn_ctx_at(ctx, source_type_id);
    if (!lexicon_have_tokenizer) {
      if (grn_obj_is_table(ctx, source_type)) {
        if (lexicon_id != source_type_id) {
          grn_obj_set_info_source_invalid_lexicon_error(
            ctx,
            "index table must equal to source type",
            lexicon,
            source_type,
            obj,
            source);
        }
      } else {
        if (!(lexicon_domain_id == source_type_id ||
              (grn_type_id_is_text_family(ctx, lexicon_domain_id) &&
               grn_type_id_is_text_family(ctx, source_type_id)))) {
          grn_obj_set_info_source_invalid_lexicon_error(
            ctx,
            "index table's key must equal source type",
            lexicon_domain,
            source_type,
            obj,
            source);
        }
      }
    }
    grn_obj_unlink(ctx, source);
    if (ctx->rc != GRN_SUCCESS) {
      goto exit;
    }
  }

exit:
  if (lexicon) {
    grn_obj_unlink(ctx, lexicon);
  }
  if (lexicon_domain) {
    grn_obj_unlink(ctx, lexicon_domain);
  }
  return ctx->rc;
}

inline static void
grn_obj_set_info_source_log(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  grn_obj buf;
  grn_id *vp = (grn_id *)GRN_BULK_HEAD(value);
  uint32_t vs = GRN_BULK_VSIZE(value), s = 0;
  grn_id id;
  const char *n;

  id = DB_OBJ(obj)->id;
  n = _grn_table_key(ctx, ctx->impl->db, id, &s);
  GRN_TEXT_INIT(&buf, 0);
  GRN_TEXT_PUT(ctx, &buf, n, s);
  GRN_TEXT_PUTC(ctx, &buf, ' ');
  while (vs) {
    n = _grn_table_key(ctx, ctx->impl->db, *vp++, &s);
    GRN_TEXT_PUT(ctx, &buf, n, s);
    vs -= sizeof(grn_id);
    if (vs) { GRN_TEXT_PUTC(ctx, &buf, ','); }
  }
  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "DDL:%u:set_source %.*s",
          id,
          (int)GRN_BULK_VSIZE(&buf), GRN_BULK_HEAD(&buf));
  GRN_OBJ_FIN(ctx, &buf);
}

inline static grn_rc
grn_obj_set_info_source_update(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  void *v = GRN_BULK_HEAD(value);
  uint32_t s = GRN_BULK_VSIZE(value);
  if (s) {
    void *v2 = GRN_MALLOC(s);
    if (!v2) {
      return ctx->rc;
    }
    grn_memcpy(v2, v, s);
    if (DB_OBJ(obj)->source) { GRN_FREE(DB_OBJ(obj)->source); }
    DB_OBJ(obj)->source = v2;
    DB_OBJ(obj)->source_size = s;

    if (obj->header.type == GRN_COLUMN_INDEX) {
      update_source_hook(ctx, obj);
      grn_index_column_build(ctx, obj);
    }
  } else {
    DB_OBJ(obj)->source = NULL;
    DB_OBJ(obj)->source_size = 0;
  }

  return GRN_SUCCESS;
}

inline static grn_rc
grn_obj_set_info_source(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  grn_rc rc;

  rc = grn_obj_set_info_source_validate(ctx, obj, value);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  grn_obj_set_info_source_log(ctx, obj, value);
  rc = grn_obj_set_info_source_update(ctx, obj, value);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  grn_obj_spec_save(ctx, DB_OBJ(obj));

  return rc;
}

static grn_rc
grn_obj_set_info_token_filters(grn_ctx *ctx,
                               grn_obj *table,
                               grn_obj *token_filters)
{
  grn_obj *current_token_filters;
  unsigned int i, n_current_token_filters, n_token_filters;
  grn_obj token_filter_names;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY :
    current_token_filters = &(((grn_hash *)table)->token_filters);
    break;
  case GRN_TABLE_PAT_KEY :
    current_token_filters = &(((grn_pat *)table)->token_filters);
    break;
  case GRN_TABLE_DAT_KEY :
    current_token_filters = &(((grn_dat *)table)->token_filters);
    break;
  default :
    /* TODO: Show type name instead of type ID */
    ERR(GRN_INVALID_ARGUMENT,
        "[info][set][token-filters] target object must be one of "
        "GRN_TABLE_HASH_KEY, GRN_TABLE_PAT_KEY and GRN_TABLE_DAT_KEY: %d",
        table->header.type);
    return ctx->rc;
  }

  n_current_token_filters =
    GRN_BULK_VSIZE(current_token_filters) / sizeof(grn_obj *);
  n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);

  GRN_TEXT_INIT(&token_filter_names, 0);
  GRN_BULK_REWIND(current_token_filters);
  for (i = 0; i < n_token_filters; i++) {
    grn_obj *token_filter = GRN_PTR_VALUE_AT(token_filters, i);
    char token_filter_name[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int token_filter_name_size;

    GRN_PTR_PUT(ctx, current_token_filters, token_filter);

    if (i > 0) {
      GRN_TEXT_PUTC(ctx, &token_filter_names, ',');
    }
    token_filter_name_size = grn_obj_name(ctx,
                                          token_filter,
                                          token_filter_name,
                                          GRN_TABLE_MAX_KEY_SIZE);
    GRN_TEXT_PUT(ctx,
                 &token_filter_names,
                 token_filter_name,
                 token_filter_name_size);
  }
  if (n_token_filters > 0 || n_token_filters != n_current_token_filters) {
    GRN_LOG(ctx, GRN_LOG_NOTICE, "DDL:%u:set_token_filters %.*s",
            DB_OBJ(table)->id,
            (int)GRN_BULK_VSIZE(&token_filter_names),
            GRN_BULK_HEAD(&token_filter_names));
  }
  GRN_OBJ_FIN(ctx, &token_filter_names);
  grn_obj_spec_save(ctx, DB_OBJ(table));

  return GRN_SUCCESS;
}

grn_rc
grn_obj_set_info(grn_ctx *ctx, grn_obj *obj, grn_info_type type, grn_obj *value)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT, "grn_obj_set_info failed");
    goto exit;
  }
  switch (type) {
  case GRN_INFO_SOURCE :
    if (!GRN_DB_OBJP(obj)) {
      ERR(GRN_INVALID_ARGUMENT, "only db_obj can accept GRN_INFO_SOURCE");
      goto exit;
    }
    rc = grn_obj_set_info_source(ctx, obj, value);
    break;
  case GRN_INFO_DEFAULT_TOKENIZER :
    if (!value || DB_OBJ(value)->header.type == GRN_PROC) {
      switch (DB_OBJ(obj)->header.type) {
      case GRN_TABLE_HASH_KEY :
        ((grn_hash *)obj)->tokenizer = value;
        ((grn_hash *)obj)->header.common->tokenizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      case GRN_TABLE_PAT_KEY :
        ((grn_pat *)obj)->tokenizer = value;
        ((grn_pat *)obj)->header->tokenizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      case GRN_TABLE_DAT_KEY :
        ((grn_dat *)obj)->tokenizer = value;
        ((grn_dat *)obj)->header->tokenizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      }
    }
    break;
  case GRN_INFO_NORMALIZER :
    if (!value || DB_OBJ(value)->header.type == GRN_PROC) {
      switch (DB_OBJ(obj)->header.type) {
      case GRN_TABLE_HASH_KEY :
        ((grn_hash *)obj)->normalizer = value;
        ((grn_hash *)obj)->header.common->normalizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      case GRN_TABLE_PAT_KEY :
        ((grn_pat *)obj)->normalizer = value;
        ((grn_pat *)obj)->header->normalizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      case GRN_TABLE_DAT_KEY :
        ((grn_dat *)obj)->normalizer = value;
        ((grn_dat *)obj)->header->normalizer = grn_obj_id(ctx, value);
        rc = GRN_SUCCESS;
        break;
      }
    }
    break;
  case GRN_INFO_TOKEN_FILTERS :
    rc = grn_obj_set_info_token_filters(ctx, obj, value);
    break;
  default :
    /* todo */
    break;
  }
exit :
  GRN_API_RETURN(rc);
}

grn_obj *
grn_obj_get_element_info(grn_ctx *ctx, grn_obj *obj, grn_id id,
                         grn_info_type type, grn_obj *valuebuf)
{
  GRN_API_ENTER;
  GRN_API_RETURN(valuebuf);
}

grn_rc
grn_obj_set_element_info(grn_ctx *ctx, grn_obj *obj, grn_id id,
                         grn_info_type type, grn_obj *value)
{
  GRN_API_ENTER;
  GRN_API_RETURN(GRN_SUCCESS);
}

static void
grn_hook_free(grn_ctx *ctx, grn_hook *h)
{
  grn_hook *curr, *next;
  for (curr = h; curr; curr = next) {
    next = curr->next;
    GRN_FREE(curr);
  }
}

grn_rc
grn_obj_add_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry,
                 int offset, grn_obj *proc, grn_obj *hld)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (!GRN_DB_OBJP(obj)) {
    rc = GRN_INVALID_ARGUMENT;
  } else {
    int i;
    void *hld_value = NULL;
    uint32_t hld_size = 0;
    grn_hook *new, **last = &DB_OBJ(obj)->hooks[entry];
    if (hld) {
      hld_value = GRN_BULK_HEAD(hld);
      hld_size = GRN_BULK_VSIZE(hld);
    }
    if (!(new = GRN_MALLOC(sizeof(grn_hook) + hld_size))) {
      rc = GRN_NO_MEMORY_AVAILABLE;
      goto exit;
    }
    new->proc = (grn_proc *)proc;
    new->hld_size = hld_size;
    if (hld_size) {
      grn_memcpy(GRN_NEXT_ADDR(new), hld_value, hld_size);
    }
    for (i = 0; i != offset && *last; i++) { last = &(*last)->next; }
    new->next = *last;
    *last = new;
    grn_obj_spec_save(ctx, DB_OBJ(obj));
  }
exit :
  GRN_API_RETURN(rc);
}

int
grn_obj_get_nhooks(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry)
{
  int res = 0;
  GRN_API_ENTER;
  {
    grn_hook *hook = DB_OBJ(obj)->hooks[entry];
    while (hook) {
      res++;
      hook = hook->next;
    }
  }
  GRN_API_RETURN(res);
}

grn_obj *
grn_obj_get_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry,
                      int offset, grn_obj *hldbuf)
{
  grn_obj *res = NULL;
  GRN_API_ENTER;
  {
    int i;
    grn_hook *hook = DB_OBJ(obj)->hooks[entry];
    for (i = 0; i < offset; i++) {
      hook = hook->next;
      if (!hook) { return NULL; }
    }
    res = (grn_obj *)hook->proc;
    grn_bulk_write(ctx, hldbuf, (char *)GRN_NEXT_ADDR(hook), hook->hld_size);
  }
  GRN_API_RETURN(res);
}

grn_rc
grn_obj_delete_hook(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry, int offset)
{
  GRN_API_ENTER;
  {
    int i = 0;
    grn_hook *h, **last = &DB_OBJ(obj)->hooks[entry];
    for (;;) {
      if (!(h = *last)) { return GRN_INVALID_ARGUMENT; }
      if (++i > offset) { break; }
      last = &h->next;
    }
    *last = h->next;
    GRN_FREE(h);
  }
  grn_obj_spec_save(ctx, DB_OBJ(obj));
  GRN_API_RETURN(GRN_SUCCESS);
}

static grn_rc
remove_index(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry)
{
  grn_rc rc = GRN_SUCCESS;
  grn_hook *h0, *hooks = DB_OBJ(obj)->hooks[entry];
  DB_OBJ(obj)->hooks[entry] = NULL; /* avoid mutual recursive call */
  while (hooks) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (!target) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int length;
      char hook_name[GRN_TABLE_MAX_KEY_SIZE];
      int hook_name_length;

      length = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
      hook_name_length = grn_table_get_key(ctx,
                                           ctx->impl->db,
                                           data->target,
                                           hook_name,
                                           GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_OBJECT_CORRUPT,
          "[column][remove][index] "
          "hook has a dangling reference: <%.*s> -> <%.*s>",
          length, name,
          hook_name_length, hook_name);
      rc = ctx->rc;
    } else if (target->header.type == GRN_COLUMN_INDEX) {
      //TODO: multicolumn  MULTI_COLUMN_INDEXP
      rc = _grn_obj_remove(ctx, target, GRN_FALSE);
    } else {
      //TODO: err
      char fn[GRN_TABLE_MAX_KEY_SIZE];
      int flen;
      flen = grn_obj_name(ctx, target, fn, GRN_TABLE_MAX_KEY_SIZE);
      fn[flen] = '\0';
      ERR(GRN_UNKNOWN_ERROR, "column has unsupported hooks, col=%s",fn);
      rc = ctx->rc;
    }
    if (rc != GRN_SUCCESS) {
      DB_OBJ(obj)->hooks[entry] = hooks;
      break;
    }
    h0 = hooks;
    hooks = hooks->next;
    GRN_FREE(h0);
  }
  return rc;
}

static grn_rc
remove_columns(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;
  grn_hash *cols;
  if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                              GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
    if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)cols)) {
      GRN_HASH_EACH_BEGIN(ctx, cols, cursor, id) {
        grn_id *key;
        grn_obj *col;

        grn_hash_cursor_get_key(ctx, cursor, (void **)&key);
        col = grn_ctx_at(ctx, *key);

        if (!col) {
          char name[GRN_TABLE_MAX_KEY_SIZE];
          int name_size;
          name_size = grn_table_get_key(ctx, ctx->impl->db, *key,
                                        name, GRN_TABLE_MAX_KEY_SIZE);
          if (ctx->rc == GRN_SUCCESS) {
            ERR(GRN_INVALID_ARGUMENT,
                "[object][remove] column is broken: <%.*s>",
                name_size, name);
          } else {
            ERR(ctx->rc,
                "[object][remove] column is broken: <%.*s>: %s",
                name_size, name,
                ctx->errbuf);
          }
          rc = ctx->rc;
          break;
        }

        rc = _grn_obj_remove(ctx, col, GRN_FALSE);
        if (rc != GRN_SUCCESS) {
          grn_obj_unlink(ctx, col);
          break;
        }
      } GRN_HASH_EACH_END(ctx, cursor);
    }
    grn_hash_close(ctx, cols);
  }
  return rc;
}

static grn_rc
_grn_obj_remove_db_index_columns(grn_ctx *ctx, grn_obj *db)
{
  grn_rc rc = GRN_SUCCESS;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *obj = grn_ctx_at(ctx, id);
      if (obj && obj->header.type == GRN_COLUMN_INDEX) {
        rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
        if (rc != GRN_SUCCESS) {
          grn_obj_unlink(ctx, obj);
          break;
        }
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
  return rc;
}

static grn_rc
_grn_obj_remove_db_reference_columns(grn_ctx *ctx, grn_obj *db)
{
  grn_rc rc = GRN_SUCCESS;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *obj = grn_ctx_at(ctx, id);
      grn_obj *range = NULL;

      if (!obj) {
        continue;
      }

      switch (obj->header.type) {
      case GRN_COLUMN_FIX_SIZE :
      case GRN_COLUMN_VAR_SIZE :
        if (!DB_OBJ(obj)->range) {
          break;
        }

        range = grn_ctx_at(ctx, DB_OBJ(obj)->range);
        if (!range) {
          break;
        }

        switch (range->header.type) {
        case GRN_TABLE_NO_KEY :
        case GRN_TABLE_HASH_KEY :
        case GRN_TABLE_PAT_KEY :
        case GRN_TABLE_DAT_KEY :
          rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
          break;
        }
        break;
      }

      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
  return rc;
}

static grn_rc
_grn_obj_remove_db_reference_tables(grn_ctx *ctx, grn_obj *db)
{
  grn_rc rc = GRN_SUCCESS;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *obj = grn_ctx_at(ctx, id);
      grn_obj *domain = NULL;

      if (!obj) {
        continue;
      }

      switch (obj->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        if (!obj->header.domain) {
          break;
        }

        domain = grn_ctx_at(ctx, obj->header.domain);
        if (!domain) {
          break;
        }

        switch (domain->header.type) {
        case GRN_TABLE_NO_KEY :
        case GRN_TABLE_HASH_KEY :
        case GRN_TABLE_PAT_KEY :
        case GRN_TABLE_DAT_KEY :
          rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
          break;
        }
        break;
      }

      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
  return rc;
}

static grn_rc
_grn_obj_remove_db_all_tables(grn_ctx *ctx, grn_obj *db)
{
  grn_rc rc = GRN_SUCCESS;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *obj = grn_ctx_at(ctx, id);

      if (!obj) {
        continue;
      }

      switch (obj->header.type) {
      case GRN_TABLE_NO_KEY :
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
        break;
      }

      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
  return rc;
}

static grn_rc
_grn_obj_remove_db(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  grn_rc rc = GRN_SUCCESS;
  const char *io_spath;
  char *spath;
  grn_db *s = (grn_db *)db;
  unsigned char key_type;

  rc = _grn_obj_remove_db_index_columns(ctx, db);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = _grn_obj_remove_db_reference_columns(ctx, db);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = _grn_obj_remove_db_reference_tables(ctx, db);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = _grn_obj_remove_db_all_tables(ctx, db);
  if (rc != GRN_SUCCESS) { return rc; }

  if (s->specs &&
      (io_spath = grn_obj_path(ctx, (grn_obj *)s->specs)) && *io_spath != '\0') {
    if (!(spath = GRN_STRDUP(io_spath))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_spath);
      return ctx->rc;
    }
  } else {
    spath = NULL;
  }

  key_type = s->keys->header.type;

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) {
    if (spath) {
      GRN_FREE(spath);
    }
    return rc;
  }

  if (spath) {
    rc = grn_ja_remove(ctx, spath);
    GRN_FREE(spath);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  if (path) {
    switch (key_type) {
    case GRN_TABLE_PAT_KEY :
      rc = grn_pat_remove(ctx, path);
      break;
    case GRN_TABLE_DAT_KEY :
      rc = grn_dat_remove(ctx, path);
      break;
    }
    if (rc == GRN_SUCCESS) {
      rc = grn_db_config_remove(ctx, path);
    } else {
      grn_db_config_remove(ctx, path);
    }
  }

  return rc;
}

static grn_rc
remove_reference_tables(grn_ctx *ctx, grn_obj *table, grn_obj *db)
{
  grn_rc rc = GRN_SUCCESS;
  grn_bool is_close_opened_object_mode = GRN_FALSE;
  grn_id table_id;
  char table_name[GRN_TABLE_MAX_KEY_SIZE];
  int table_name_size;
  grn_table_cursor *cursor;

  if (grn_thread_get_limit() == 1) {
    is_close_opened_object_mode = GRN_TRUE;
  }

  table_id = DB_OBJ(table)->id;
  table_name_size = grn_obj_name(ctx, table, table_name, GRN_TABLE_MAX_KEY_SIZE);
  if ((cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                      GRN_CURSOR_BY_ID))) {
    grn_id id;
    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_obj *object;
      grn_bool is_removed = GRN_FALSE;

      if (is_close_opened_object_mode) {
        grn_ctx_push_temporary_open_space(ctx);
      }

      object = grn_ctx_at(ctx, id);
      if (!object) {
        ERRCLR(ctx);
        if (is_close_opened_object_mode) {
          grn_ctx_pop_temporary_open_space(ctx);
        }
        continue;
      }

      switch (object->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        if (DB_OBJ(object)->id == table_id) {
          break;
        }

        if (object->header.domain == table_id) {
          rc = _grn_obj_remove(ctx, object, GRN_TRUE);
          is_removed = (grn_table_at(ctx, db, id) == GRN_ID_NIL);
        }
        break;
      case GRN_TABLE_NO_KEY :
        break;
      case GRN_COLUMN_VAR_SIZE :
      case GRN_COLUMN_FIX_SIZE :
        if (object->header.domain == table_id) {
          break;
        }
        if (DB_OBJ(object)->range == table_id) {
          rc = _grn_obj_remove(ctx, object, GRN_FALSE);
          is_removed = (grn_table_at(ctx, db, id) == GRN_ID_NIL);
        }
        break;
      case GRN_COLUMN_INDEX :
        break;
      default:
        break;
      }

      if (!is_removed) {
        grn_obj_unlink(ctx, object);
      }

      if (is_close_opened_object_mode) {
        grn_ctx_pop_temporary_open_space(ctx);
      }

      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    grn_table_cursor_close(ctx, cursor);
  }

  return rc;
}

static grn_bool
is_removable_table(grn_ctx *ctx, grn_obj *table, grn_obj *db)
{
  grn_id table_id;
  grn_id reference_object_id;

  table_id = DB_OBJ(table)->id;
  if (table_id & GRN_OBJ_TMP_OBJECT) {
    return GRN_TRUE;
  }

  reference_object_id = grn_table_find_reference_object(ctx, table);
  if (reference_object_id == GRN_ID_NIL) {
    return GRN_TRUE;
  }

  {
    grn_obj *db;
    const char *table_name;
    int table_name_size;
    grn_obj *reference_object;
    const char *reference_object_name;
    int reference_object_name_size;

    db = grn_ctx_db(ctx);

    table_name = _grn_table_key(ctx, db, table_id,&table_name_size);

    reference_object = grn_ctx_at(ctx, reference_object_id);
    reference_object_name = _grn_table_key(ctx,
                                           db,
                                           reference_object_id,
                                           &reference_object_name_size);
    if (reference_object) {
      if (grn_obj_is_table(ctx, reference_object)) {
        ERR(GRN_OPERATION_NOT_PERMITTED,
            "[table][remove] a table that references the table exists: "
            "<%.*s._key> -> <%.*s>",
            reference_object_name_size, reference_object_name,
            table_name_size, table_name);
      } else {
        ERR(GRN_OPERATION_NOT_PERMITTED,
            "[table][remove] a column that references the table exists: "
            "<%.*s> -> <%.*s>",
            reference_object_name_size, reference_object_name,
            table_name_size, table_name);
      }
    } else {
      ERR(GRN_OPERATION_NOT_PERMITTED,
          "[table][remove] a dangling object that references the table exists: "
          "<%.*s(%u)> -> <%.*s>",
          reference_object_name_size,
          reference_object_name,
          reference_object_id,
          table_name_size, table_name);
    }
  }

  return GRN_FALSE;
}

static inline grn_rc
_grn_obj_remove_spec(grn_ctx *ctx, grn_obj *db, grn_id id, uint8_t type)
{
  const char *name;
  uint32_t name_size = 0;

  name = _grn_table_key(ctx, db, id, &name_size);
  /* TODO: reduce log level. */
  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "spec:%u:remove:%.*s:%u(%s)",
          id,
          name_size, name,
          type,
          grn_obj_type_to_string(type));

  return grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
}

static grn_rc
_grn_obj_remove_pat(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                    const char *path, grn_bool dependent)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  if (dependent) {
    rc = remove_reference_tables(ctx, obj, db);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  } else {
    if (!is_removable_table(ctx, obj, db)) {
      return ctx->rc;
    }
  }

  rc = remove_index(ctx, obj, GRN_HOOK_INSERT);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = remove_columns(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_pat_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_dat(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                    const char *path, grn_bool dependent)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  if (dependent) {
    rc = remove_reference_tables(ctx, obj, db);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  } else {
    if (!is_removable_table(ctx, obj, db)) {
      return ctx->rc;
    }
  }

  rc = remove_index(ctx, obj, GRN_HOOK_INSERT);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = remove_columns(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_dat_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_hash(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                     const char *path, grn_bool dependent)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  if (dependent) {
    rc = remove_reference_tables(ctx, obj, db);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  } else {
    if (!is_removable_table(ctx, obj, db)) {
      return ctx->rc;
    }
  }

  rc = remove_index(ctx, obj, GRN_HOOK_INSERT);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = remove_columns(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_hash_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_array(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path, grn_bool dependent)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  if (dependent) {
    rc = remove_reference_tables(ctx, obj, db);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  } else {
    if (!is_removable_table(ctx, obj, db)) {
      return ctx->rc;
    }
  }

  rc = remove_columns(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_array_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_ja(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  rc = remove_index(ctx, obj, GRN_HOOK_SET);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_ja_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_ra(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  rc = remove_index(ctx, obj, GRN_HOOK_SET);
  if (rc != GRN_SUCCESS) { return rc; }
  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_ra_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }
  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_index(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  delete_source_hook(ctx, obj);
  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_ii_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_db_obj(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                       const char *path)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t type;

  type = obj->header.type;

  rc = grn_obj_close(ctx, obj);
  if (rc != GRN_SUCCESS) { return rc; }

  if (path) {
    rc = grn_io_remove(ctx, path);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  if (!(id & GRN_OBJ_TMP_OBJECT)) {
    rc = _grn_obj_remove_spec(ctx, db, id, type);
    if (rc != GRN_SUCCESS) { return rc; }
    rc = grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    if (rc != GRN_SUCCESS) { return rc; }
  }

  grn_obj_touch(ctx, db, NULL);

  return rc;
}

static grn_rc
_grn_obj_remove_other(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path)
{
  return grn_obj_close(ctx, obj);
}

static grn_rc
_grn_obj_remove(grn_ctx *ctx, grn_obj *obj, grn_bool dependent)
{
  grn_rc rc = GRN_SUCCESS;
  grn_id id = GRN_ID_NIL;
  grn_obj *db = NULL;
  const char *io_path;
  char *path;
  grn_bool is_temporary_open_target = GRN_FALSE;

  if (ctx->impl && ctx->impl->db) {
    grn_id id;
    uint32_t s = 0;
    const char *n;

    id = DB_OBJ(obj)->id;
    n = _grn_table_key(ctx, ctx->impl->db, id, &s);
    if (s > 0) {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "DDL:%u:obj_remove %.*s", id, s, n);
    }
  }
  if (obj->header.type != GRN_PROC &&
      (io_path = grn_obj_path(ctx, obj)) && *io_path != '\0') {
    if (!(path = GRN_STRDUP(io_path))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
      return ctx->rc;
    }
  } else {
    path = NULL;
  }
  if (GRN_DB_OBJP(obj)) {
    id = DB_OBJ(obj)->id;
    db = DB_OBJ(obj)->db;
  }
  switch (obj->header.type) {
  case GRN_DB :
    rc = _grn_obj_remove_db(ctx, obj, db, id, path);
    break;
  case GRN_TABLE_PAT_KEY :
    rc = _grn_obj_remove_pat(ctx, obj, db, id, path, dependent);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_TABLE_DAT_KEY :
    rc = _grn_obj_remove_dat(ctx, obj, db, id, path, dependent);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_TABLE_HASH_KEY :
    rc = _grn_obj_remove_hash(ctx, obj, db, id, path, dependent);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_TABLE_NO_KEY :
    rc = _grn_obj_remove_array(ctx, obj, db, id, path, dependent);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_COLUMN_VAR_SIZE :
    rc = _grn_obj_remove_ja(ctx, obj, db, id, path);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_COLUMN_FIX_SIZE :
    rc = _grn_obj_remove_ra(ctx, obj, db, id, path);
    is_temporary_open_target = GRN_TRUE;
    break;
  case GRN_COLUMN_INDEX :
    rc = _grn_obj_remove_index(ctx, obj, db, id, path);
    is_temporary_open_target = GRN_TRUE;
    break;
  default :
    if (GRN_DB_OBJP(obj)) {
      rc = _grn_obj_remove_db_obj(ctx, obj, db, id, path);
    } else {
      rc = _grn_obj_remove_other(ctx, obj, db, id, path);
    }
  }
  if (path) {
    GRN_FREE(path);
  } else {
    is_temporary_open_target = GRN_FALSE;
  }

  if (is_temporary_open_target && rc == GRN_SUCCESS) {
    grn_obj *space;
    space = ctx->impl->temporary_open_spaces.current;
    if (space) {
      unsigned int i, n_elements;
      n_elements = GRN_BULK_VSIZE(space) / sizeof(grn_obj *);
      for (i = 0; i < n_elements; i++) {
        if (GRN_PTR_VALUE_AT(space, i) == obj) {
          GRN_PTR_SET_AT(ctx, space, i, NULL);
        }
      }
    }
  }

  return rc;
}

grn_rc
grn_obj_remove(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (ctx->impl && ctx->impl->db && ctx->impl->db != obj) {
    grn_io *io = grn_obj_get_io(ctx, ctx->impl->db);
    rc = grn_io_lock(ctx, io, grn_lock_timeout);
    if (rc == GRN_SUCCESS) {
      rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
      grn_io_unlock(io);
    }
  } else {
    rc = _grn_obj_remove(ctx, obj, GRN_FALSE);
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_remove_dependent(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (ctx->impl && ctx->impl->db && ctx->impl->db != obj) {
    grn_io *io = grn_obj_get_io(ctx, ctx->impl->db);
    rc = grn_io_lock(ctx, io, grn_lock_timeout);
    if (rc == GRN_SUCCESS) {
      rc = _grn_obj_remove(ctx, obj, GRN_TRUE);
      grn_io_unlock(io);
    }
  } else {
    rc = _grn_obj_remove(ctx, obj, GRN_TRUE);
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_remove_force(grn_ctx *ctx, const char *name, int name_size)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *db;
  grn_id obj_id;
  char path[PATH_MAX];

  GRN_API_ENTER;

  if (!(ctx->impl && ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT,
        "[object][remove][force] database isn't initialized");
    rc = ctx->rc;
    goto exit;
  }

  db = ctx->impl->db;
  if (name_size == -1) {
    name_size = strlen(name);
  }
  obj_id = grn_table_get(ctx, db, name, name_size);
  if (obj_id == GRN_ID_NIL) {
    ERR(GRN_INVALID_ARGUMENT,
        "[object][remove][force] nonexistent object: <%.*s>",
        name_size, name);
    rc = ctx->rc;
    goto exit;
  }

  grn_obj_delete_by_id(ctx, db, obj_id, GRN_TRUE);
  grn_obj_path_by_id(ctx, db, obj_id, path);
  grn_io_remove_if_exist(ctx, path);
  grn_strcat(path, PATH_MAX, ".c");
  grn_io_remove_if_exist(ctx, path);

exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_update_by_id(grn_ctx *ctx, grn_obj *table, grn_id id,
                       const void *dest_key, unsigned int dest_key_size)
{
  grn_rc rc = GRN_OPERATION_NOT_SUPPORTED;
  GRN_API_ENTER;
  if (table->header.type == GRN_TABLE_DAT_KEY) {
    grn_dat *dat = (grn_dat *)table;
    if (dat->io && !(dat->io->flags & GRN_IO_TEMPORARY)) {
      if (grn_io_lock(ctx, dat->io, grn_lock_timeout)) {
        rc = ctx->rc;
      } else {
        rc = grn_dat_update_by_id(ctx, dat, id, dest_key, dest_key_size);
        grn_io_unlock(dat->io);
      }
    } else {
      rc = grn_dat_update_by_id(ctx, dat, id, dest_key, dest_key_size);
    }
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_update(grn_ctx *ctx, grn_obj *table,
                 const void *src_key, unsigned int src_key_size,
                 const void *dest_key, unsigned int dest_key_size)
{
  grn_rc rc = GRN_OPERATION_NOT_SUPPORTED;
  GRN_API_ENTER;
  if (table->header.type == GRN_TABLE_DAT_KEY) {
    rc = grn_dat_update(ctx, (grn_dat *)table,
                        src_key, src_key_size,
                        dest_key, dest_key_size);
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_rename(grn_ctx *ctx, grn_obj *obj, const char *name, unsigned int name_size)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (ctx && ctx->impl && GRN_DB_P(ctx->impl->db) && GRN_DB_OBJP(obj) && !IS_TEMP(obj)) {
    grn_db *s = (grn_db *)ctx->impl->db;
    grn_obj *keys = (grn_obj *)s->keys;
    rc = grn_table_update_by_id(ctx, keys, DB_OBJ(obj)->id, name, name_size);
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_rename(grn_ctx *ctx, grn_obj *table, const char *name, unsigned int name_size)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_hash *cols;

  GRN_API_ENTER;

  if (!GRN_OBJ_TABLEP(table)) {
    char table_name[GRN_TABLE_MAX_KEY_SIZE];
    int table_name_size;
    table_name_size = grn_obj_name(ctx, table, table_name,
                                   GRN_TABLE_MAX_KEY_SIZE);
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][rename] isn't table: <%.*s> -> <%.*s>",
        table_name_size, table_name,
        name_size, name);
    goto exit;
  }
  if (IS_TEMP(table)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][rename] temporary table doesn't have name: "
        "(anonymous) -> <%.*s>",
        name_size, name);
    goto exit;
  }

  if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                              GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
    grn_table_columns(ctx, table, "", 0, (grn_obj *)cols);
    if (!(rc = grn_obj_rename(ctx, table, name, name_size))) {
      grn_id *key;
      char fullname[GRN_TABLE_MAX_KEY_SIZE];
      grn_memcpy(fullname, name, name_size);
      fullname[name_size] = GRN_DB_DELIMITER;
      GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
        grn_obj *col = grn_ctx_at(ctx, *key);
        if (col) {
          int colname_len = grn_column_name(ctx, col, fullname + name_size + 1,
                                            GRN_TABLE_MAX_KEY_SIZE - name_size - 1);
          if (colname_len) {
            if ((rc = grn_obj_rename(ctx, col, fullname,
                                     name_size + 1 + colname_len))) {
              break;
            }
          }
        }
      });
    }
    grn_hash_close(ctx, cols);
  }
exit:
  GRN_API_RETURN(rc);
}

grn_rc
grn_column_rename(grn_ctx *ctx, grn_obj *column, const char *name, unsigned int name_size)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(column)) {
    char fullname[GRN_TABLE_MAX_KEY_SIZE];
    grn_db *s = (grn_db *)DB_OBJ(column)->db;
    int len = grn_table_get_key(ctx, s->keys, DB_OBJ(column)->header.domain,
                                fullname, GRN_TABLE_MAX_KEY_SIZE);
    if (name_size + 1 + len > GRN_TABLE_MAX_KEY_SIZE) {
      ERR(GRN_INVALID_ARGUMENT,
          "[column][rename] too long column name: required name_size(%d) < %d"
          ": <%.*s>.<%.*s>",
          name_size, GRN_TABLE_MAX_KEY_SIZE - 1 - len,
          len, fullname, name_size, name);
      goto exit;
    }
    fullname[len] = GRN_DB_DELIMITER;
    grn_memcpy(fullname + len + 1, name, name_size);
    name_size += len + 1;
    rc = grn_obj_rename(ctx, column, fullname, name_size);
    if (rc == GRN_SUCCESS) {
      grn_obj_touch(ctx, column, NULL);
    }
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_path_rename(grn_ctx *ctx, const char *old_path, const char *new_path)
{
  GRN_API_ENTER;
  GRN_API_RETURN(GRN_SUCCESS);
}

/* db must be validated by caller */
grn_id
grn_obj_register(grn_ctx *ctx, grn_obj *db, const char *name, unsigned int name_size)
{
  grn_id id = GRN_ID_NIL;
  if (name && name_size) {
    grn_db *s = (grn_db *)db;
    int added;
    if (!(id = grn_table_add(ctx, s->keys, name, name_size, &added))) {
      grn_rc rc;
      rc = ctx->rc;
      if (rc == GRN_SUCCESS) {
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
      ERR(rc,
          "[object][register] failed to register a name: <%.*s>%s%s%s",
          name_size, name,
          ctx->rc == GRN_SUCCESS ? "" : ": <",
          ctx->rc == GRN_SUCCESS ? "" : ctx->errbuf,
          ctx->rc == GRN_SUCCESS ? "" : ">");
    } else if (!added) {
      ERR(GRN_INVALID_ARGUMENT,
          "[object][register] already used name was assigned: <%.*s>",
          name_size, name);
      id = GRN_ID_NIL;
    }
  } else if (ctx->impl && ctx->impl->values) {
    id = grn_array_add(ctx, ctx->impl->values, NULL) | GRN_OBJ_TMP_OBJECT;
  }
  return id;
}

grn_rc
grn_obj_delete_by_id(grn_ctx *ctx, grn_obj *db, grn_id id, grn_bool removep)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (id) {
    if (id & GRN_OBJ_TMP_OBJECT) {
      if (ctx->impl) {
        if (id & GRN_OBJ_TMP_COLUMN) {
          if (ctx->impl->temporary_columns) {
            rc = grn_pat_delete_by_id(ctx, ctx->impl->temporary_columns,
                                      id & ~(GRN_OBJ_TMP_COLUMN | GRN_OBJ_TMP_OBJECT),
                                      NULL);
          }
        } else {
          if (ctx->impl->values) {
            rc = grn_array_delete_by_id(ctx, ctx->impl->values,
                                        id & ~GRN_OBJ_TMP_OBJECT, NULL);
          }
        }
      }
    } else {
      db_value *vp;
      grn_db *s = (grn_db *)db;
      if ((vp = grn_tiny_array_at(&s->values, id))) {
        GRN_ASSERT(!vp->lock);
        vp->lock = 0;
        vp->ptr = NULL;
        vp->done = 0;
      }
      if (removep) {
        switch (s->keys->header.type) {
        case GRN_TABLE_PAT_KEY :
          rc = grn_pat_delete_by_id(ctx, (grn_pat *)s->keys, id, NULL);
          break;
        case GRN_TABLE_DAT_KEY :
          rc = grn_dat_delete_by_id(ctx, (grn_dat *)s->keys, id, NULL);
          break;
        }
      } else {
        rc = GRN_SUCCESS;
      }
    }
  }
  GRN_API_RETURN(rc);
}


grn_rc
grn_obj_path_by_id(grn_ctx *ctx, grn_obj *db, grn_id id, char *buffer)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (!GRN_DB_P(db) || !buffer) {
    rc = GRN_INVALID_ARGUMENT;
  } else {
    grn_db_generate_pathname(ctx, db, id, buffer);
  }
  GRN_API_RETURN(rc);
}

/* db must be validated by caller */
grn_rc
grn_db_obj_init(grn_ctx *ctx, grn_obj *db, grn_id id, grn_db_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;
  if (id) {
    if (id & GRN_OBJ_TMP_OBJECT) {
      if (id & GRN_OBJ_TMP_COLUMN) {
        if (ctx->impl && ctx->impl->temporary_columns) {
          grn_id real_id = id & ~(GRN_OBJ_TMP_COLUMN | GRN_OBJ_TMP_OBJECT);
          rc = grn_pat_set_value(ctx, ctx->impl->temporary_columns,
                                 real_id, &obj, GRN_OBJ_SET);
        }
      } else {
        if (ctx->impl && ctx->impl->values) {
          rc = grn_array_set_value(ctx, ctx->impl->values,
                                   id & ~GRN_OBJ_TMP_OBJECT, &obj, GRN_OBJ_SET);
        }
      }
    } else {
      db_value *vp;
      vp = grn_tiny_array_at(&((grn_db *)db)->values, id);
      if (!vp) {
        rc = GRN_NO_MEMORY_AVAILABLE;
        ERR(rc, "grn_tiny_array_at failed (%d)", id);
        return rc;
      }
      vp->lock = 1;
      vp->ptr = (grn_obj *)obj;
    }
  }
  obj->id = id;
  obj->db = db;
  obj->source = NULL;
  obj->source_size = 0;
  {
    grn_hook_entry entry;
    for (entry = 0; entry < N_HOOK_ENTRIES; entry++) {
      obj->hooks[entry] = NULL;
    }
  }
  grn_obj_spec_save(ctx, obj);
  return rc;
}

#define GET_PATH(spec,decoded_spec,buffer,s,id) do {\
  if (spec->header.flags & GRN_OBJ_CUSTOM_NAME) {\
    const char *path;\
    unsigned int size = grn_vector_get_element(ctx,\
                                               decoded_spec,\
                                               GRN_SERIALIZED_SPEC_INDEX_PATH,\
                                               &path,\
                                               NULL,\
                                               NULL);\
    if (size > PATH_MAX) { ERR(GRN_FILENAME_TOO_LONG, "too long path"); }\
    grn_memcpy(buffer, path, size);\
    buffer[size] = '\0';\
  } else {\
    grn_db_generate_pathname(ctx, (grn_obj *)s, id, buffer);\
  }\
} while (0)

#define UNPACK_INFO(spec,decoded_spec) do {\
  if (vp->ptr) {\
    const char *p;\
    uint32_t size;\
    grn_db_obj *r = DB_OBJ(vp->ptr);\
    r->header = spec->header;\
    r->id = id;\
    r->range = spec->range;\
    r->db = (grn_obj *)s;\
    size = grn_vector_get_element(ctx,\
                                  decoded_spec,\
                                  GRN_SERIALIZED_SPEC_INDEX_SOURCE,\
                                  &p,\
                                  NULL,\
                                  NULL);\
    if (size) {\
      if ((r->source = GRN_MALLOC(size))) {\
        grn_memcpy(r->source, p, size);\
        r->source_size = size;\
      }\
    }\
    size = grn_vector_get_element(ctx,\
                                  decoded_spec,\
                                  GRN_SERIALIZED_SPEC_INDEX_HOOK,\
                                  &p,\
                                  NULL,\
                                  NULL);\
    grn_hook_unpack(ctx, r, p, size);\
  }\
} while (0)

static void
grn_token_filters_unpack(grn_ctx *ctx,
                         grn_obj *token_filters,
                         grn_obj *spec_vector)
{
  grn_id *token_filter_ids;
  unsigned int element_size;
  unsigned int i, n_token_filter_ids;

  if (grn_vector_size(ctx, spec_vector) <= GRN_SERIALIZED_SPEC_INDEX_TOKEN_FILTERS) {
    return;
  }

  element_size = grn_vector_get_element(ctx,
                                        spec_vector,
                                        GRN_SERIALIZED_SPEC_INDEX_TOKEN_FILTERS,
                                        (const char **)(&token_filter_ids),
                                        NULL,
                                        NULL);
  n_token_filter_ids = element_size / sizeof(grn_id);
  for (i = 0; i < n_token_filter_ids; i++) {
    grn_id token_filter_id = token_filter_ids[i];
    grn_obj *token_filter;

    token_filter = grn_ctx_at(ctx, token_filter_id);
    if (!token_filter) {
      ERR(GRN_INVALID_ARGUMENT,
          "nonexistent token filter ID: %d", token_filter_id);
      return;
    }
    GRN_PTR_PUT(ctx, token_filters, token_filter);
  }
}

grn_bool
grn_db_spec_unpack(grn_ctx *ctx,
                   grn_id id,
                   void *encoded_spec,
                   uint32_t encoded_spec_size,
                   grn_obj_spec **spec,
                   grn_obj *decoded_spec,
                   const char *error_message_tag)
{
  grn_obj *db;
  grn_db *db_raw;
  grn_rc rc;
  uint32_t spec_size;

  db = ctx->impl->db;
  db_raw = (grn_db *)db;

  rc = grn_vector_decode(ctx,
                         decoded_spec,
                         encoded_spec,
                         encoded_spec_size);
  if (rc != GRN_SUCCESS) {
    const char *name;
    uint32_t name_size;
    name = _grn_table_key(ctx, db, id, &name_size);
    GRN_LOG((ctx), GRN_LOG_ERROR,
            "%s: failed to decode spec: <%u>(<%.*s>):<%u>: %s",
            error_message_tag,
            id,
            name_size, name,
            encoded_spec_size,
            grn_rc_to_string(rc));
    return GRN_FALSE;
  }

  spec_size = grn_vector_get_element(ctx,
                                     decoded_spec,
                                     GRN_SERIALIZED_SPEC_INDEX_SPEC,
                                     (const char **)spec,
                                     NULL,
                                     NULL);
  if (spec_size == 0) {
    const char *name;
    uint32_t name_size;
    name = _grn_table_key(ctx, db, id, &name_size);
    GRN_LOG(ctx, GRN_LOG_ERROR,
            "%s: spec value is empty: <%u>(<%.*s>)",
            error_message_tag,
            id,
            name_size, name);
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

grn_obj *
grn_ctx_at(grn_ctx *ctx, grn_id id)
{
  grn_obj *res = NULL;
  if (!ctx || !ctx->impl || !id) { return res; }
  GRN_API_ENTER;
  if (id & GRN_OBJ_TMP_OBJECT) {
    if (id & GRN_OBJ_TMP_COLUMN) {
      if (ctx->impl->temporary_columns) {
        grn_id real_id = id & ~(GRN_OBJ_TMP_COLUMN | GRN_OBJ_TMP_OBJECT);
        grn_obj **tmp_obj;
        uint32_t size;
        tmp_obj = (grn_obj **)grn_pat_get_value_(ctx,
                                                 ctx->impl->temporary_columns,
                                                 real_id,
                                                 &size);
        if (tmp_obj) {
          res = *tmp_obj;
        }
      }
    } else {
      if (ctx->impl->values) {
        grn_obj **tmp_obj;
        tmp_obj = _grn_array_get_value(ctx, ctx->impl->values,
                                       id & ~GRN_OBJ_TMP_OBJECT);
        if (tmp_obj) {
          res = *tmp_obj;
        }
      }
    }
  } else {
    grn_db *s = (grn_db *)ctx->impl->db;
    if (s) {
      db_value *vp;
      uint32_t l, *pl, ntrial;
      if (!(vp = grn_tiny_array_at(&s->values, id))) { goto exit; }
#ifdef USE_NREF
      pl = &vp->lock;
      for (ntrial = 0;; ntrial++) {
        GRN_ATOMIC_ADD_EX(pl, 1, l);
        if (l < GRN_IO_MAX_REF) { break; }
        if (ntrial >= 10) {
          GRN_LOG(ctx, GRN_LOG_NOTICE, "max trial in ctx_at(%p,%d)", vp->ptr, vp->lock);
          break;
        }
        GRN_ATOMIC_ADD_EX(pl, -1, l);
        GRN_FUTEX_WAIT(pl);
      }
#endif /* USE_NREF */
      if (s->specs && !vp->ptr /* && !vp->done */) {
#ifndef USE_NREF
        pl = &vp->lock;
        for (ntrial = 0;; ntrial++) {
          GRN_ATOMIC_ADD_EX(pl, 1, l);
          if (l < GRN_IO_MAX_REF) { break; }
          if (ntrial >= 10) {
            GRN_LOG(ctx, GRN_LOG_NOTICE, "max trial in ctx_at(%p,%d)", vp->ptr, vp->lock);
            break;
          }
          GRN_ATOMIC_ADD_EX(pl, -1, l);
          GRN_FUTEX_WAIT(pl);
        }
#endif /* USE_NREF */
        if (!l) {
          grn_io_win iw;
          uint32_t encoded_spec_size;
          void *encoded_spec;

          encoded_spec = grn_ja_ref(ctx, s->specs, id, &iw, &encoded_spec_size);
          if (encoded_spec) {
            grn_bool success;
            grn_obj_spec *spec;
            grn_obj decoded_spec;

            GRN_OBJ_INIT(&decoded_spec, GRN_VECTOR, 0, GRN_DB_TEXT);
            success = grn_db_spec_unpack(ctx,
                                         id,
                                         encoded_spec,
                                         encoded_spec_size,
                                         &spec,
                                         &decoded_spec,
                                         "grn_ctx_at");
            if (success) {
              char buffer[PATH_MAX];
              switch (spec->header.type) {
              case GRN_TYPE :
                vp->ptr = (grn_obj *)grn_type_open(ctx, spec);
                UNPACK_INFO(spec, &decoded_spec);
                break;
              case GRN_TABLE_HASH_KEY :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_hash_open(ctx, buffer);
                if (vp->ptr) {
                  grn_hash *hash = (grn_hash *)(vp->ptr);
                  grn_obj_flags flags = vp->ptr->header.flags;
                  UNPACK_INFO(spec, &decoded_spec);
                  vp->ptr->header.flags = flags;
                  grn_token_filters_unpack(ctx,
                                           &(hash->token_filters),
                                           &decoded_spec);
                }
                break;
              case GRN_TABLE_PAT_KEY :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_pat_open(ctx, buffer);
                if (vp->ptr) {
                  grn_pat *pat = (grn_pat *)(vp->ptr);
                  grn_obj_flags flags = vp->ptr->header.flags;
                  UNPACK_INFO(spec, &decoded_spec);
                  vp->ptr->header.flags = flags;
                  grn_token_filters_unpack(ctx,
                                           &(pat->token_filters),
                                           &decoded_spec);
                }
                break;
              case GRN_TABLE_DAT_KEY :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_dat_open(ctx, buffer);
                if (vp->ptr) {
                  grn_dat *dat = (grn_dat *)(vp->ptr);
                  grn_obj_flags flags = vp->ptr->header.flags;
                  UNPACK_INFO(spec, &decoded_spec);
                  vp->ptr->header.flags = flags;
                  grn_token_filters_unpack(ctx,
                                           &(dat->token_filters),
                                           &decoded_spec);
                }
                break;
              case GRN_TABLE_NO_KEY :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_array_open(ctx, buffer);
                UNPACK_INFO(spec, &decoded_spec);
                break;
              case GRN_COLUMN_VAR_SIZE :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_ja_open(ctx, buffer);
                UNPACK_INFO(spec, &decoded_spec);
                break;
              case GRN_COLUMN_FIX_SIZE :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                vp->ptr = (grn_obj *)grn_ra_open(ctx, buffer);
                UNPACK_INFO(spec, &decoded_spec);
                break;
              case GRN_COLUMN_INDEX :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                {
                  grn_obj *table = grn_ctx_at(ctx, spec->header.domain);
                  vp->ptr = (grn_obj *)grn_ii_open(ctx, buffer, table);
                }
                UNPACK_INFO(spec, &decoded_spec);
                break;
              case GRN_PROC :
                GET_PATH(spec, &decoded_spec, buffer, s, id);
                grn_plugin_register(ctx, buffer);
                break;
              case GRN_EXPR :
                {
                  const char *p;
                  uint32_t size;
                  uint8_t *u;
                  size = grn_vector_get_element(ctx,
                                                &decoded_spec,
                                                GRN_SERIALIZED_SPEC_INDEX_EXPR,
                                                &p,
                                                NULL,
                                                NULL);
                  u = (uint8_t *)p;
                  vp->ptr = grn_expr_open(ctx, spec, u, u + size);
                }
                break;
              }
              if (!vp->ptr) {
                const char *name;
                uint32_t name_size = 0;
                name = _grn_table_key(ctx, (grn_obj *)s, id, &name_size);
                GRN_LOG(ctx, GRN_LOG_ERROR,
                        "grn_ctx_at: failed to open object: "
                        "<%u>(<%.*s>):<%u>(<%s>)",
                        id,
                        name_size, name,
                        spec->header.type,
                        grn_obj_type_to_string(spec->header.type));
              }
            }
            GRN_OBJ_FIN(ctx, &decoded_spec);
            grn_ja_unref(ctx, &iw);
          }
#ifndef USE_NREF
          GRN_ATOMIC_ADD_EX(pl, -1, l);
#endif /* USE_NREF */
          vp->done = 1;
          GRN_FUTEX_WAKE(&vp->ptr);
        } else {
          for (ntrial = 0; !vp->ptr; ntrial++) {
            if (ntrial >= 1000) {
              GRN_LOG(ctx, GRN_LOG_NOTICE, "max trial in ctx_at(%d,%p,%d)!", id, vp->ptr, vp->lock);
              break;
            }
            GRN_FUTEX_WAIT(&vp->ptr);
          }
        }
        if (vp->ptr) {
          switch (vp->ptr->header.type) {
          case GRN_TABLE_HASH_KEY :
          case GRN_TABLE_PAT_KEY :
          case GRN_TABLE_DAT_KEY :
          case GRN_TABLE_NO_KEY :
          case GRN_COLUMN_FIX_SIZE :
          case GRN_COLUMN_VAR_SIZE :
          case GRN_COLUMN_INDEX :
            {
              grn_obj *space;
              space = ctx->impl->temporary_open_spaces.current;
              if (space) {
                GRN_PTR_PUT(ctx, space, vp->ptr);
              }
            }
            break;
          }
        }
      }
      res = vp->ptr;
      if (res && res->header.type == GRN_PROC) {
        grn_plugin_ensure_registered(ctx, res);
      }
    }
  }
exit :
  GRN_API_RETURN(res);
}

grn_bool
grn_ctx_is_opened(grn_ctx *ctx, grn_id id)
{
  grn_bool is_opened = GRN_FALSE;

  if (!ctx || !ctx->impl || !id) {
    return GRN_FALSE;
  }

  GRN_API_ENTER;
  if (id & GRN_OBJ_TMP_OBJECT) {
    if (ctx->impl->values) {
      grn_obj **tmp_obj;
      tmp_obj = _grn_array_get_value(ctx, ctx->impl->values,
                                     id & ~GRN_OBJ_TMP_OBJECT);
      if (tmp_obj) {
        is_opened = GRN_TRUE;
      }
    }
  } else {
    grn_db *s = (grn_db *)ctx->impl->db;
    if (s) {
      db_value *vp;
      vp = grn_tiny_array_at(&s->values, id);
      if (vp && vp->ptr) {
        is_opened = GRN_TRUE;
      }
    }
  }
  GRN_API_RETURN(is_opened);
}

grn_obj *
grn_obj_open(grn_ctx *ctx, unsigned char type, grn_obj_flags flags, grn_id domain)
{
  grn_obj *obj = GRN_MALLOCN(grn_obj, 1);
  if (obj) {
    GRN_OBJ_INIT(obj, type, flags, domain);
    obj->header.impl_flags |= GRN_OBJ_ALLOCATED;
  }
  return obj;
}

grn_obj *
grn_obj_graft(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj *new = grn_obj_open(ctx, obj->header.type, obj->header.impl_flags, obj->header.domain);
  if (new) {
    /* todo : deep copy if (obj->header.impl_flags & GRN_OBJ_DO_SHALLOW_COPY) */
    new->u.b.head = obj->u.b.head;
    new->u.b.curr = obj->u.b.curr;
    new->u.b.tail = obj->u.b.tail;
    obj->u.b.head = NULL;
    obj->u.b.curr = NULL;
    obj->u.b.tail = NULL;
  }
  return new;
}

grn_rc
grn_pvector_fin(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc;
  if (obj->header.impl_flags & GRN_OBJ_OWN) {
    /*
     * Note that GRN_OBJ_OWN should not be used outside the DB API function
     * because grn_obj_close is a DB API function.
     */
    unsigned int i, n_elements;
    n_elements = GRN_BULK_VSIZE(obj) / sizeof(grn_obj *);
    for (i = 0; i < n_elements; i++) {
      grn_obj *element = GRN_PTR_VALUE_AT(obj, n_elements - i - 1);
      if (element) {
        grn_obj_close(ctx, element);
      }
    }
  }
  obj->header.type = GRN_VOID;
  rc = grn_bulk_fin(ctx, obj);
  if (obj->header.impl_flags & GRN_OBJ_ALLOCATED) {
    GRN_FREE(obj);
  }
  return rc;
}

static void
grn_table_close_columns(grn_ctx *ctx, grn_obj *table)
{
  grn_hash *columns;
  int n_columns;

  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
  if (!columns) {
    return;
  }

  n_columns = grn_table_columns(ctx, table, "", 0, (grn_obj *)columns);
  if (n_columns > 0) {
    grn_hash_cursor *cursor;
    cursor = grn_hash_cursor_open(ctx, columns, NULL, 0, NULL, 0, 0, -1, 0);
    if (cursor) {
      while (grn_hash_cursor_next(ctx, cursor) != GRN_ID_NIL) {
        grn_id *id;
        grn_obj *column;

        grn_hash_cursor_get_key(ctx, cursor, (void **)&id);
        column = grn_ctx_at(ctx, *id);
        if (column) {
          grn_obj_close(ctx, column);
        }
      }
      grn_hash_cursor_close(ctx, cursor);
    }
  }

  grn_hash_close(ctx, columns);
}

grn_rc
grn_obj_close(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (obj) {
    if (grn_obj_is_table(ctx, obj) &&
        (DB_OBJ(obj)->id & GRN_OBJ_TMP_OBJECT)) {
      grn_table_close_columns(ctx, obj);
    }
    if (GRN_DB_OBJP(obj)) {
      grn_hook_entry entry;
      if (DB_OBJ(obj)->finalizer) {
        DB_OBJ(obj)->finalizer(ctx, 1, &obj, &DB_OBJ(obj)->user_data);
      }
      if (DB_OBJ(obj)->source) {
        GRN_FREE(DB_OBJ(obj)->source);
      }
      for (entry = 0; entry < N_HOOK_ENTRIES; entry++) {
        grn_hook_free(ctx, DB_OBJ(obj)->hooks[entry]);
      }
      grn_obj_delete_by_id(ctx, DB_OBJ(obj)->db, DB_OBJ(obj)->id, GRN_FALSE);
    }
    switch (obj->header.type) {
    case GRN_VECTOR :
      if (obj->u.v.body && !(obj->header.impl_flags & GRN_OBJ_REFER)) {
        grn_obj_close(ctx, obj->u.v.body);
      }
      if (obj->u.v.sections) { GRN_FREE(obj->u.v.sections); }
      if (obj->header.impl_flags & GRN_OBJ_ALLOCATED) { GRN_FREE(obj); }
      rc = GRN_SUCCESS;
      break;
    case GRN_VOID :
    case GRN_BULK :
    case GRN_UVECTOR :
    case GRN_MSG :
      obj->header.type = GRN_VOID;
      rc = grn_bulk_fin(ctx, obj);
      if (obj->header.impl_flags & GRN_OBJ_ALLOCATED) { GRN_FREE(obj); }
      break;
    case GRN_PTR :
      if (obj->header.impl_flags & GRN_OBJ_OWN) {
        if (GRN_BULK_VSIZE(obj) == sizeof(grn_obj *)) {
          grn_obj_close(ctx, GRN_PTR_VALUE(obj));
        }
      }
      obj->header.type = GRN_VOID;
      rc = grn_bulk_fin(ctx, obj);
      if (obj->header.impl_flags & GRN_OBJ_ALLOCATED) { GRN_FREE(obj); }
      break;
    case GRN_PVECTOR :
      rc = grn_pvector_fin(ctx, obj);
      break;
    case GRN_ACCESSOR :
      {
        grn_accessor *p, *n;
        for (p = (grn_accessor *)obj; p; p = n) {
          n = p->next;
          GRN_FREE(p);
        }
      }
      rc = GRN_SUCCESS;
      break;
    case GRN_SNIP :
      rc = grn_snip_close(ctx, (grn_snip *)obj);
      break;
    case GRN_STRING :
      rc = grn_string_close(ctx, obj);
      break;
    case GRN_CURSOR_TABLE_PAT_KEY :
      grn_pat_cursor_close(ctx, (grn_pat_cursor *)obj);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      grn_dat_cursor_close(ctx, (grn_dat_cursor *)obj);
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      grn_hash_cursor_close(ctx, (grn_hash_cursor *)obj);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      grn_array_cursor_close(ctx, (grn_array_cursor *)obj);
      break;
    case GRN_CURSOR_COLUMN_INDEX :
      {
        grn_index_cursor *ic = (grn_index_cursor *)obj;
        if (ic->iic) { grn_ii_cursor_close(ctx, ic->iic); }
        GRN_FREE(ic);
      }
      break;
    case GRN_CURSOR_COLUMN_GEO_INDEX :
      grn_geo_cursor_close(ctx, obj);
      break;
    case GRN_CURSOR_CONFIG :
      grn_config_cursor_close(ctx, (grn_config_cursor *)obj);
      break;
    case GRN_TYPE :
      GRN_FREE(obj);
      rc = GRN_SUCCESS;
      break;
    case GRN_DB :
      rc = grn_db_close(ctx, obj);
      break;
    case GRN_TABLE_PAT_KEY :
      rc = grn_pat_close(ctx, (grn_pat *)obj);
      break;
    case GRN_TABLE_DAT_KEY :
      rc = grn_dat_close(ctx, (grn_dat *)obj);
      break;
    case GRN_TABLE_HASH_KEY :
      rc = grn_hash_close(ctx, (grn_hash *)obj);
      break;
    case GRN_TABLE_NO_KEY :
      rc = grn_array_close(ctx, (grn_array *)obj);
      break;
    case GRN_COLUMN_VAR_SIZE :
      rc = grn_ja_close(ctx, (grn_ja *)obj);
      break;
    case GRN_COLUMN_FIX_SIZE :
      rc = grn_ra_close(ctx, (grn_ra *)obj);
      break;
    case GRN_COLUMN_INDEX :
      rc = grn_ii_close(ctx, (grn_ii *)obj);
      break;
    case GRN_PROC :
      {
        uint32_t i;
        grn_proc *p = (grn_proc *)obj;
        /*
        if (obj->header.domain) {
          grn_hash_delete(ctx, ctx->impl->qe, &obj->header.domain, sizeof(grn_id), NULL);
        }
        */
        for (i = 0; i < p->nvars; i++) {
          grn_obj_close(ctx, &p->vars[i].value);
        }
        GRN_REALLOC(p->vars, 0);
        grn_obj_close(ctx, &p->name_buf);
        if (p->obj.range != GRN_ID_NIL) {
          grn_plugin_close(ctx, p->obj.range);
        }
        GRN_FREE(obj);
        rc = GRN_SUCCESS;
      }
      break;
    case GRN_EXPR :
      rc = grn_expr_close(ctx, obj);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

void
grn_obj_unlink(grn_ctx *ctx, grn_obj *obj)
{
  if (obj &&
      (!GRN_DB_OBJP(obj) ||
       (((grn_db_obj *)obj)->id & GRN_OBJ_TMP_OBJECT) ||
       (((grn_db_obj *)obj)->id == GRN_ID_NIL) ||
       obj->header.type == GRN_DB)) {
    grn_obj_close(ctx, obj);
  } else if (GRN_DB_OBJP(obj)) {
#ifdef USE_NREF
    grn_db_obj *dob = DB_OBJ(obj);
    grn_db *s = (grn_db *)dob->db;
    db_value *vp = grn_tiny_array_at(&s->values, dob->id);
    if (vp) {
      uint32_t l, *pl = &vp->lock;
      if (!vp->lock) {
        GRN_LOG(ctx, GRN_LOG_ERROR, "invalid unlink(%p,%d)", obj, vp->lock);
        return;
      }
      GRN_ATOMIC_ADD_EX(pl, -1, l);
      if (l == 1) {
        GRN_ATOMIC_ADD_EX(pl, GRN_IO_MAX_REF, l);
        if (l == GRN_IO_MAX_REF) {
#ifdef CALL_FINALIZER
          grn_obj_close(ctx, obj);
          vp->done = 0;
          if (dob->finalizer) {
            dob->finalizer(ctx, 1, &obj, &dob->user_data);
            dob->finalizer = NULL;
            dob->user_data.ptr = NULL;
          }
#endif /* CALL_FINALIZER */
        }
        GRN_ATOMIC_ADD_EX(pl, -GRN_IO_MAX_REF, l);
        GRN_FUTEX_WAKE(pl);
      }
    }
#endif /* USE_NREF */
  }
}

#define VECTOR_CLEAR(ctx,obj) do {\
  if ((obj)->u.v.body && !((obj)->header.impl_flags & GRN_OBJ_REFER)) {\
    grn_obj_close((ctx), (obj)->u.v.body);\
  }\
  if ((obj)->u.v.sections) { GRN_FREE((obj)->u.v.sections); }\
  (obj)->header.impl_flags &= ~GRN_OBJ_DO_SHALLOW_COPY;\
  (obj)->u.b.head = NULL;\
  (obj)->u.b.curr = NULL;\
  (obj)->u.b.tail = NULL;\
} while (0)

static void
grn_obj_ensure_vector(grn_ctx *ctx, grn_obj *obj)
{
  if (obj->header.type != GRN_VECTOR) { grn_bulk_fin(ctx, obj); }
  obj->header.type = GRN_VECTOR;
  obj->header.flags &= ~GRN_OBJ_WITH_WEIGHT;
}

static void
grn_obj_ensure_bulk(grn_ctx *ctx, grn_obj *obj)
{
  if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
  obj->header.type = GRN_BULK;
  obj->header.flags &= ~GRN_OBJ_WITH_WEIGHT;
}

grn_rc
grn_obj_reinit(grn_ctx *ctx, grn_obj *obj, grn_id domain, unsigned char flags)
{
  if (!GRN_OBJ_MUTABLE(obj)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid obj assigned");
  } else {
    switch (obj->header.type) {
    case GRN_PTR :
      if (obj->header.impl_flags & GRN_OBJ_OWN) {
        if (GRN_BULK_VSIZE(obj) == sizeof(grn_obj *)) {
          grn_obj_close(ctx, GRN_PTR_VALUE(obj));
        }
        obj->header.impl_flags &= ~GRN_OBJ_OWN;
      }
      break;
    case GRN_PVECTOR :
      if (obj->header.impl_flags & GRN_OBJ_OWN) {
        unsigned int i, n_elements;
        n_elements = GRN_BULK_VSIZE(obj) / sizeof(grn_obj *);
        for (i = 0; i < n_elements; i++) {
          grn_obj *element = GRN_PTR_VALUE_AT(obj, i);
          grn_obj_close(ctx, element);
        }
        obj->header.impl_flags &= ~GRN_OBJ_OWN;
      }
      break;
    default :
      break;
    }

    switch (domain) {
    case GRN_DB_VOID :
      if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
      obj->header.type = GRN_VOID;
      obj->header.domain = domain;
      GRN_BULK_REWIND(obj);
      break;
    case GRN_DB_OBJECT :
    case GRN_DB_BOOL :
    case GRN_DB_INT8 :
    case GRN_DB_UINT8 :
    case GRN_DB_INT16 :
    case GRN_DB_UINT16 :
    case GRN_DB_INT32 :
    case GRN_DB_UINT32 :
    case GRN_DB_INT64 :
    case GRN_DB_UINT64 :
    case GRN_DB_FLOAT :
    case GRN_DB_TIME :
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
      obj->header.type = (flags & GRN_OBJ_VECTOR) ? GRN_UVECTOR : GRN_BULK;
      obj->header.domain = domain;
      GRN_BULK_REWIND(obj);
      break;
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      if (flags & GRN_OBJ_VECTOR) {
        if (obj->header.type != GRN_VECTOR) { grn_bulk_fin(ctx, obj); }
        obj->header.type = GRN_VECTOR;
        if (obj->u.v.body) {
          grn_obj_reinit(ctx, obj->u.v.body, domain, 0);
        }
        obj->u.v.n_sections = 0;
      } else {
        if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
        obj->header.type = GRN_BULK;
      }
      obj->header.domain = domain;
      GRN_BULK_REWIND(obj);
      break;
    default :
      {
        grn_obj *d = grn_ctx_at(ctx, domain);
        if (!d) {
          ERR(GRN_INVALID_ARGUMENT, "invalid domain assigned");
        } else {
          if (d->header.type == GRN_TYPE && (d->header.flags & GRN_OBJ_KEY_VAR_SIZE)) {
            if (flags & GRN_OBJ_VECTOR) {
              if (obj->header.type != GRN_VECTOR) { grn_bulk_fin(ctx, obj); }
              obj->header.type = GRN_VECTOR;
            } else {
              if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
              obj->header.type = GRN_BULK;
            }
          } else {
            if (obj->header.type == GRN_VECTOR) { VECTOR_CLEAR(ctx, obj); }
            obj->header.type = (flags & GRN_OBJ_VECTOR) ? GRN_UVECTOR : GRN_BULK;
          }
          obj->header.domain = domain;
          GRN_BULK_REWIND(obj);
        }
      }
      break;
    }
  }
  return ctx->rc;
}

grn_rc
grn_obj_reinit_for(grn_ctx *ctx, grn_obj *obj, grn_obj *domain_obj)
{
  grn_id domain = GRN_ID_NIL;
  grn_obj_flags flags = 0;

  if (!GRN_DB_OBJP(domain_obj) && domain_obj->header.type != GRN_ACCESSOR) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect_limited(ctx, &inspected, domain_obj);
    ERR(GRN_INVALID_ARGUMENT,
        "[reinit] invalid domain object: <%.*s>",
        (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return ctx->rc;
  }

  if (grn_column_is_index(ctx, domain_obj)) {
    domain = GRN_DB_UINT32;
  } else {
    grn_obj_get_range_info(ctx, domain_obj, &domain, &flags);
    if (GRN_OBJ_TABLEP(domain_obj) &&
        domain_obj->header.type != GRN_TABLE_NO_KEY) {
      domain = domain_obj->header.domain;
    }
  }
  return grn_obj_reinit(ctx, obj, domain, flags);
}

const char *
grn_obj_path(grn_ctx *ctx, grn_obj *obj)
{
  grn_io *io;
  const char *path = NULL;
  GRN_API_ENTER;
  if (obj->header.type == GRN_PROC) {
    path = grn_plugin_path(ctx, DB_OBJ(obj)->range);
    GRN_API_RETURN(path);
  }
  io = grn_obj_get_io(ctx, obj);
  if (io && !(io->flags & GRN_IO_TEMPORARY)) { path = io->path; }
  GRN_API_RETURN(path);
}

int
grn_obj_name(grn_ctx *ctx, grn_obj *obj, char *namebuf, int buf_size)
{
  int len = 0;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    if (DB_OBJ(obj)->id) {
      grn_db *s = (grn_db *)DB_OBJ(obj)->db;
      grn_id id = DB_OBJ(obj)->id;
      if (id & GRN_OBJ_TMP_OBJECT) {
        if (id & GRN_OBJ_TMP_COLUMN) {
          grn_id real_id = id & ~(GRN_OBJ_TMP_OBJECT | GRN_OBJ_TMP_COLUMN);
          len = grn_pat_get_key(ctx, ctx->impl->temporary_columns,
                                real_id, namebuf, buf_size);
        }
      } else {
        len = grn_table_get_key(ctx, s->keys, id, namebuf, buf_size);
      }
    }
  }
  GRN_API_RETURN(len);
}

int
grn_column_name(grn_ctx *ctx, grn_obj *obj, char *namebuf, int buf_size)
{
  int len = 0;
  char buf[GRN_TABLE_MAX_KEY_SIZE];
  if (!obj) { return len; }
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    grn_id id = DB_OBJ(obj)->id;
    if (id & GRN_OBJ_TMP_OBJECT) {
      if (id & GRN_OBJ_TMP_COLUMN) {
        grn_id real_id = id & ~(GRN_OBJ_TMP_OBJECT | GRN_OBJ_TMP_COLUMN);
        len = grn_pat_get_key(ctx, ctx->impl->temporary_columns,
                              real_id, buf, GRN_TABLE_MAX_KEY_SIZE);
      }
    } else if (id && id < GRN_ID_MAX) {
      grn_db *s = (grn_db *)DB_OBJ(obj)->db;
      len = grn_table_get_key(ctx, s->keys, id, buf, GRN_TABLE_MAX_KEY_SIZE);
    }
    if (len) {
      int cl;
      char *p = buf, *p0 = p, *pe = p + len;
      for (; p < pe && (cl = grn_charlen(ctx, p, pe)); p += cl) {
        if (*p == GRN_DB_DELIMITER && cl == 1) { p0 = p + cl; }
      }
      len = pe - p0;
      if (len && len <= buf_size) {
        grn_memcpy(namebuf, p0, len);
      }
    }
  } else if (obj->header.type == GRN_ACCESSOR) {
    grn_obj name;
    grn_accessor *a;

    GRN_TEXT_INIT(&name, 0);

#define ADD_DELMITER() do {                             \
      if (GRN_TEXT_LEN(&name) > 0) {                    \
        GRN_TEXT_PUTC(ctx, &name, GRN_DB_DELIMITER);    \
      }                                                 \
    } while (GRN_FALSE)

    for (a = (grn_accessor *)obj; a; a = a->next) {
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_ID);
        break;
      case GRN_ACCESSOR_GET_KEY :
        if (!a->next) {
          ADD_DELMITER();
          GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_KEY);
        }
        break;
      case GRN_ACCESSOR_GET_VALUE :
        if (!a->next) {
          ADD_DELMITER();
          GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_VALUE);
        }
        break;
      case GRN_ACCESSOR_GET_SCORE :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_SCORE);
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_NSUBRECS);
        break;
      case GRN_ACCESSOR_GET_MAX :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_MAX);
        break;
      case GRN_ACCESSOR_GET_MIN :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_MIN);
        break;
      case GRN_ACCESSOR_GET_SUM :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_SUM);
        break;
      case GRN_ACCESSOR_GET_AVG :
        ADD_DELMITER();
        GRN_TEXT_PUTS(ctx, &name, GRN_COLUMN_NAME_AVG);
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        ADD_DELMITER();
        {
          char column_name[GRN_TABLE_MAX_KEY_SIZE];
          int column_name_size;
          column_name_size = grn_column_name(ctx, a->obj,
                                             column_name,
                                             GRN_TABLE_MAX_KEY_SIZE);
          GRN_TEXT_PUT(ctx, &name, column_name, column_name_size);
        }
        break;
      case GRN_ACCESSOR_GET_DB_OBJ :
      case GRN_ACCESSOR_LOOKUP :
      case GRN_ACCESSOR_FUNCALL :
        break;
      }
    }
#undef ADD_DELIMITER

    len = GRN_TEXT_LEN(&name);
    if (len > 0 && len <= buf_size) {
      grn_memcpy(namebuf, GRN_TEXT_VALUE(&name), len);
    }

    GRN_OBJ_FIN(ctx, &name);
  }
  GRN_API_RETURN(len);
}

grn_rc
grn_column_name_(grn_ctx *ctx, grn_obj *obj, grn_obj *buf)
{
  if (GRN_DB_OBJP(obj)) {
    uint32_t len = 0;
    const char *p = NULL;
    grn_id id = DB_OBJ(obj)->id;
    if (id & GRN_OBJ_TMP_OBJECT) {
      if (id & GRN_OBJ_TMP_COLUMN) {
        grn_id real_id = id & ~(GRN_OBJ_TMP_OBJECT | GRN_OBJ_TMP_COLUMN);
        p = _grn_pat_key(ctx, ctx->impl->temporary_columns, real_id, &len);
      }
    } else if (id && id < GRN_ID_MAX) {
      grn_db *s = (grn_db *)DB_OBJ(obj)->db;
      p = _grn_table_key(ctx, s->keys, id, &len);
    }
    if (len) {
      int cl;
      const char *p0 = p, *pe = p + len;
      for (; p < pe && (cl = grn_charlen(ctx, p, pe)); p += cl) {
        if (*p == GRN_DB_DELIMITER && cl == 1) { p0 = p + cl; }
      }
      GRN_TEXT_PUT(ctx, buf, p0, pe - p0);
    }
  } else if (obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a;
    for (a = (grn_accessor *)obj; a; a = a->next) {
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        GRN_TEXT_PUT(ctx, buf, GRN_COLUMN_NAME_ID, GRN_COLUMN_NAME_ID_LEN);
        break;
      case GRN_ACCESSOR_GET_KEY :
        if (!a->next) {
          GRN_TEXT_PUT(ctx, buf, GRN_COLUMN_NAME_KEY, GRN_COLUMN_NAME_KEY_LEN);
        }
        break;
      case GRN_ACCESSOR_GET_VALUE :
        if (!a->next) {
          GRN_TEXT_PUT(ctx, buf,
                       GRN_COLUMN_NAME_VALUE,
                       GRN_COLUMN_NAME_VALUE_LEN);
        }
        break;
      case GRN_ACCESSOR_GET_SCORE :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_SCORE,
                     GRN_COLUMN_NAME_SCORE_LEN);
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_NSUBRECS,
                     GRN_COLUMN_NAME_NSUBRECS_LEN);
        break;
      case GRN_ACCESSOR_GET_MAX :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_MAX,
                     GRN_COLUMN_NAME_MAX_LEN);
        break;
      case GRN_ACCESSOR_GET_MIN :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_MIN,
                     GRN_COLUMN_NAME_MIN_LEN);
        break;
      case GRN_ACCESSOR_GET_SUM :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_SUM,
                     GRN_COLUMN_NAME_SUM_LEN);
        break;
      case GRN_ACCESSOR_GET_AVG :
        GRN_TEXT_PUT(ctx, buf,
                     GRN_COLUMN_NAME_AVG,
                     GRN_COLUMN_NAME_AVG_LEN);
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        grn_column_name_(ctx, a->obj, buf);
        if (a->next) { GRN_TEXT_PUTC(ctx, buf, '.'); }
        break;
      case GRN_ACCESSOR_GET_DB_OBJ :
      case GRN_ACCESSOR_LOOKUP :
      case GRN_ACCESSOR_FUNCALL :
        break;
      }
    }
  }
  return ctx->rc;
}

int
grn_obj_expire(grn_ctx *ctx, grn_obj *obj, int threshold)
{
  GRN_API_ENTER;
  GRN_API_RETURN(0);
}

int
grn_obj_check(grn_ctx *ctx, grn_obj *obj)
{
  GRN_API_ENTER;
  GRN_API_RETURN(0);
}

grn_rc
grn_obj_lock(grn_ctx *ctx, grn_obj *obj, grn_id id, int timeout)
{
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  rc = grn_io_lock(ctx, grn_obj_get_io(ctx, obj), timeout);
  if (rc == GRN_SUCCESS && obj && obj->header.type == GRN_COLUMN_INDEX) {
    rc = grn_io_lock(ctx, ((grn_ii *)obj)->chunk, timeout);
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_unlock(grn_ctx *ctx, grn_obj *obj, grn_id id)
{
  GRN_API_ENTER;
  if (obj && obj->header.type == GRN_COLUMN_INDEX) {
    grn_io_unlock(((grn_ii *)obj)->chunk);
  }
  grn_io_unlock(grn_obj_get_io(ctx, obj));
  GRN_API_RETURN(GRN_SUCCESS);
}

grn_user_data *
grn_obj_user_data(grn_ctx *ctx, grn_obj *obj)
{
  if (!GRN_DB_OBJP(obj)) { return NULL; }
  return &DB_OBJ(obj)->user_data;
}

grn_rc
grn_obj_set_finalizer(grn_ctx *ctx, grn_obj *obj, grn_proc_func *func)
{
  if (!GRN_DB_OBJP(obj)) { return GRN_INVALID_ARGUMENT; }
  DB_OBJ(obj)->finalizer = func;
  return GRN_SUCCESS;
}

grn_rc
grn_obj_clear_lock(grn_ctx *ctx, grn_obj *obj)
{
  GRN_API_ENTER;
  switch (obj->header.type) {
  case GRN_DB:
    {
      grn_table_cursor *cur;
      if ((cur = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0, 0, -1, 0))) {
        grn_id id;
        while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
          grn_obj *tbl = grn_ctx_at(ctx, id);
          if (tbl) {
            switch (tbl->header.type) {
            case GRN_TABLE_HASH_KEY :
            case GRN_TABLE_PAT_KEY:
            case GRN_TABLE_DAT_KEY:
            case GRN_TABLE_NO_KEY:
              grn_obj_clear_lock(ctx, tbl);
              break;
            }
          } else {
            if (ctx->rc != GRN_SUCCESS) {
              ERRCLR(ctx);
            }
          }
        }
        grn_table_cursor_close(ctx, cur);
      }
    }
    grn_io_clear_lock(grn_obj_get_io(ctx, obj));
    {
      grn_db *db = (grn_db *)obj;
      if (db->specs) {
        grn_obj_clear_lock(ctx, (grn_obj *)(db->specs));
      }
    }
    break;
  case GRN_TABLE_NO_KEY :
    grn_array_queue_lock_clear(ctx, (grn_array *)obj);
    /* fallthru */
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    {
      grn_hash *cols;
      if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                  GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
        if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)cols)) {
          grn_id *key;
          GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
            grn_obj *col = grn_ctx_at(ctx, *key);
            if (col) { grn_obj_clear_lock(ctx, col); }
          });
        }
        grn_hash_close(ctx, cols);
      }
      grn_io_clear_lock(grn_obj_get_io(ctx, obj));
    }
    break;
  case GRN_COLUMN_FIX_SIZE:
  case GRN_COLUMN_VAR_SIZE:
    grn_io_clear_lock(grn_obj_get_io(ctx, obj));
    break;
  case GRN_COLUMN_INDEX:
    grn_io_clear_lock(grn_obj_get_io(ctx, obj));
    if (obj) {
      grn_io_clear_lock(((grn_ii *)obj)->chunk);
    }
    break;
  }
  GRN_API_RETURN(GRN_SUCCESS);
}

unsigned int
grn_obj_is_locked(grn_ctx *ctx, grn_obj *obj)
{
  unsigned int res = 0;
  GRN_API_ENTER;
  res = grn_io_is_locked(grn_obj_get_io(ctx, obj));
  if (obj && obj->header.type == GRN_COLUMN_INDEX) {
    res += grn_io_is_locked(((grn_ii *)obj)->chunk);
  }
  GRN_API_RETURN(res);
}

grn_rc
grn_obj_flush(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;

  GRN_API_ENTER;

  switch (obj->header.type) {
  case GRN_DB :
    {
      grn_db *db = (grn_db *)obj;
      rc = grn_obj_flush(ctx, db->keys);
      if (rc == GRN_SUCCESS && db->specs) {
        rc = grn_obj_flush(ctx, (grn_obj *)(db->specs));
      }
      if (rc == GRN_SUCCESS) {
        rc = grn_obj_flush(ctx, (grn_obj *)(db->config));
      }
    }
    break;
  case GRN_TABLE_DAT_KEY :
    rc = grn_dat_flush(ctx, (grn_dat *)obj);
    break;
  case GRN_COLUMN_INDEX :
    rc = grn_ii_flush(ctx, (grn_ii *)obj);
    break;
  default :
    {
      grn_io *io;
      io = grn_obj_get_io(ctx, obj);
      if (io) {
        rc = grn_io_flush(ctx, io);
      }
    }
    break;
  }

  if (rc == GRN_SUCCESS &&
      GRN_DB_OBJP(obj) &&
      DB_OBJ(obj)->id != GRN_ID_NIL &&
      !IS_TEMP(obj)) {
    rc = grn_db_clean(ctx, DB_OBJ(obj)->db);
  }

  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_flush_recursive(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_SUCCESS;

  GRN_API_ENTER;
  switch (obj->header.type) {
  case GRN_DB :
    {
      grn_table_cursor *cursor;
      grn_id id;

      cursor = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0, 0, -1, 0);
      if (!cursor) {
        GRN_API_RETURN(ctx->rc);
      }

      while ((id = grn_table_cursor_next_inline(ctx, cursor)) != GRN_ID_NIL) {
        grn_obj *table = grn_ctx_at(ctx, id);
        rc = GRN_SUCCESS;
        if (table) {
          switch (table->header.type) {
          case GRN_TABLE_HASH_KEY :
          case GRN_TABLE_PAT_KEY:
          case GRN_TABLE_DAT_KEY:
          case GRN_TABLE_NO_KEY:
            rc = grn_obj_flush_recursive(ctx, table);
            break;
          }
        } else {
          if (ctx->rc != GRN_SUCCESS) {
            ERRCLR(ctx);
          }
        }
        if (rc != GRN_SUCCESS) {
          break;
        }
      }
      grn_table_cursor_close(ctx, cursor);
    }
    if (rc == GRN_SUCCESS) {
      rc = grn_obj_flush(ctx, obj);
    }
    break;
  case GRN_TABLE_NO_KEY :
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    {
      grn_hash *columns;
      columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
      if (!columns) {
        GRN_API_RETURN(ctx->rc);
      }

      if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)columns) > 0) {
        grn_id *key;
        GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
          grn_obj *column = grn_ctx_at(ctx, *key);
          if (column) {
            rc = grn_obj_flush(ctx, column);
            if (rc != GRN_SUCCESS) {
              break;
            }
          }
        });
      }
      grn_hash_close(ctx, columns);
    }

    if (rc == GRN_SUCCESS) {
      rc = grn_obj_flush(ctx, obj);
    }
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_INDEX :
    rc = grn_obj_flush(ctx, obj);
    break;
  default :
    {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, obj);
      ERR(GRN_INVALID_ARGUMENT,
          "[flush] object must be DB, table or column: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      rc = ctx->rc;
      GRN_OBJ_FIN(ctx, &inspected);
    }
    break;
  }

  GRN_API_RETURN(rc);
}

grn_obj *
grn_obj_db(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj *db = NULL;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) { db = DB_OBJ(obj)->db; }
  GRN_API_RETURN(db);
}

grn_id
grn_obj_id(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id = GRN_ID_NIL;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    id = DB_OBJ(obj)->id;
  }
  GRN_API_RETURN(id);
}

int
grn_obj_defrag(grn_ctx *ctx, grn_obj *obj, int threshold)
{
  int r = 0;
  GRN_API_ENTER;
  switch (obj->header.type) {
  case GRN_DB:
    {
      grn_table_cursor *cur;
      if ((cur = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0, 0, -1, 0))) {
        grn_id id;
        while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
          grn_obj *ja = grn_ctx_at(ctx, id);
          if (ja && ja->header.type == GRN_COLUMN_VAR_SIZE) {
            r += grn_ja_defrag(ctx, (grn_ja *)ja, threshold);
          }
        }
        grn_table_cursor_close(ctx, cur);
      }
    }
    break;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    {
      grn_hash *cols;
      if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                  GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
        if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)cols)) {
          grn_id *key;
          GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
            grn_obj *col = grn_ctx_at(ctx, *key);
            if (col) {
              r += grn_obj_defrag(ctx, col, threshold);
              grn_obj_unlink(ctx, col);
            }
          });
        }
        grn_hash_close(ctx, cols);
      }
    }
    break;
  case GRN_COLUMN_VAR_SIZE:
    r = grn_ja_defrag(ctx, (grn_ja *)obj, threshold);
    break;
  }
  GRN_API_RETURN(r);
}

/**** sort ****/

typedef struct {
  grn_id id;
  uint32_t size;
  const void *value;
} sort_reference_entry;

enum {
  KEY_ID = 0,
  KEY_BULK,
  KEY_INT8,
  KEY_INT16,
  KEY_INT32,
  KEY_INT64,
  KEY_UINT8,
  KEY_UINT16,
  KEY_UINT32,
  KEY_UINT64,
  KEY_FLOAT32,
  KEY_FLOAT64,
};

#define CMPNUM(type) do {\
  if (as) {\
    if (bs) {\
      type va = *((type *)(ap));\
      type vb = *((type *)(bp));\
      if (va != vb) { return va > vb; }\
    } else {\
      return 1;\
    }\
  } else {\
    if (bs) { return 0; }\
  }\
} while (0)

inline static int
compare_reference(grn_ctx *ctx,
                  sort_reference_entry *a, sort_reference_entry *b,
                  grn_table_sort_key *keys, int n_keys)
{
  int i;
  uint8_t type;
  uint32_t as, bs;
  const unsigned char *ap, *bp;
  for (i = 0; i < n_keys; i++, keys++) {
    if (i) {
      const char *ap_raw, *bp_raw;
      if (keys->flags & GRN_TABLE_SORT_DESC) {
        ap_raw = grn_obj_get_value_(ctx, keys->key, b->id, &as);
        bp_raw = grn_obj_get_value_(ctx, keys->key, a->id, &bs);
      } else {
        ap_raw = grn_obj_get_value_(ctx, keys->key, a->id, &as);
        bp_raw = grn_obj_get_value_(ctx, keys->key, b->id, &bs);
      }
      ap = (const unsigned char *)ap_raw;
      bp = (const unsigned char *)bp_raw;
    } else {
      if (keys->flags & GRN_TABLE_SORT_DESC) {
        ap = b->value; as = b->size;
        bp = a->value; bs = a->size;
      } else {
        ap = a->value; as = a->size;
        bp = b->value; bs = b->size;
      }
    }
    type = keys->offset;
    switch (type) {
    case KEY_ID :
      if (ap != bp) { return ap > bp; }
      break;
    case KEY_BULK :
      for (;; ap++, bp++, as--, bs--) {
        if (!as) { if (bs) { return 0; } else { break; } }
        if (!bs) { return 1; }
        if (*ap < *bp) { return 0; }
        if (*ap > *bp) { return 1; }
      }
      break;
    case KEY_INT8 :
      CMPNUM(int8_t);
      break;
    case KEY_INT16 :
      CMPNUM(int16_t);
      break;
    case KEY_INT32 :
      CMPNUM(int32_t);
      break;
    case KEY_INT64 :
      CMPNUM(int64_t);
      break;
    case KEY_UINT8 :
      CMPNUM(uint8_t);
      break;
    case KEY_UINT16 :
      CMPNUM(uint16_t);
      break;
    case KEY_UINT32 :
      CMPNUM(uint32_t);
      break;
    case KEY_UINT64 :
      CMPNUM(uint64_t);
      break;
    case KEY_FLOAT32 :
      if (as) {
        if (bs) {
          float va = *((float *)(ap));
          float vb = *((float *)(bp));
          if (va < vb || va > vb) { return va > vb; }
        } else {
          return 1;
        }
      } else {
        if (bs) { return 0; }
      }
      break;
    case KEY_FLOAT64 :
      if (as) {
        if (bs) {
          double va = *((double *)(ap));
          double vb = *((double *)(bp));
          if (va < vb || va > vb) { return va > vb; }
        } else {
          return 1;
        }
      } else {
        if (bs) { return 0; }
      }
      break;
    }
  }
  return 0;
}

inline static void
swap_reference(sort_reference_entry *a, sort_reference_entry *b)
{
  sort_reference_entry c_ = *a;
  *a = *b;
  *b = c_;
}

inline static sort_reference_entry *
part_reference(grn_ctx *ctx,
               sort_reference_entry *b, sort_reference_entry *e,
               grn_table_sort_key *keys, int n_keys)
{
  sort_reference_entry *c;
  intptr_t d = e - b;
  if (compare_reference(ctx, b, e, keys, n_keys)) {
    swap_reference(b, e);
  }
  if (d < 2) { return NULL; }
  c = b + (d >> 1);
  if (compare_reference(ctx, b, c, keys, n_keys)) {
    swap_reference(b, c);
  } else {
    if (compare_reference(ctx, c, e, keys, n_keys)) {
      swap_reference(c, e);
    }
  }
  if (d < 3) { return NULL; }
  b++;
  swap_reference(b, c);
  c = b;
  for (;;) {
    do {
      b++;
    } while (compare_reference(ctx, c, b, keys, n_keys));
    do {
      e--;
    } while (compare_reference(ctx, e, c, keys, n_keys));
    if (b >= e) { break; }
    swap_reference(b, e);
  }
  swap_reference(c, e);
  return e;
}

static void
sort_reference(grn_ctx *ctx,
               sort_reference_entry *head, sort_reference_entry *tail,
               int from, int to,
               grn_table_sort_key *keys, int n_keys)
{
  sort_reference_entry *c;
  if (head < tail && (c = part_reference(ctx, head, tail, keys, n_keys))) {
    intptr_t m = c - head + 1;
    if (from < m - 1) {
      sort_reference(ctx, head, c - 1, from, to, keys, n_keys);
    }
    if (m < to) {
      sort_reference(ctx, c + 1, tail, from - m, to - m, keys, n_keys);
    }
  }
}

static sort_reference_entry *
pack_reference(grn_ctx *ctx, grn_obj *table,
               sort_reference_entry *head, sort_reference_entry *tail,
               grn_table_sort_key *keys, int n_keys)
{
  int i = 0;
  sort_reference_entry e, c;
  grn_table_cursor *tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0);
  if (!tc) { return NULL; }
  if ((c.id = grn_table_cursor_next_inline(ctx, tc))) {
    c.value = grn_obj_get_value_(ctx, keys->key, c.id, &c.size);
    while ((e.id = grn_table_cursor_next_inline(ctx, tc))) {
      e.value = grn_obj_get_value_(ctx, keys->key, e.id, &e.size);
      if (compare_reference(ctx, &c, &e, keys, n_keys)) {
        *head++ = e;
      } else {
        *tail-- = e;
      }
      i++;
    }
    *head = c;
    i++;
  }
  grn_table_cursor_close(ctx, tc);
  return i > 2 ? head : NULL;
}

static int
grn_table_sort_reference(grn_ctx *ctx, grn_obj *table,
                         int offset, int limit,
                         grn_obj *result,
                         grn_table_sort_key *keys, int n_keys)
{
  int e, n;
  sort_reference_entry *array, *ep;
  e = offset + limit;
  n = grn_table_size(ctx, table);
  if (!(array = GRN_MALLOC(sizeof(sort_reference_entry) * n))) {
    return 0;
  }
  if ((ep = pack_reference(ctx, table, array, array + n - 1, keys, n_keys))) {
    intptr_t m = ep - array + 1;
    if (offset < m - 1) {
      sort_reference(ctx, array, ep - 1, offset, e, keys, n_keys);
    }
    if (m < e) {
      sort_reference(ctx, ep + 1, array + n - 1, offset - m, e - m, keys, n_keys);
    }
  }
  {
    int i;
    grn_id *v;
    for (i = 0, ep = array + offset; i < limit && ep < array + n; i++, ep++) {
      if (!grn_array_add(ctx, (grn_array *)result, (void **)&v)) { break; }
      *v = ep->id;
    }
    GRN_FREE(array);
    return i;
  }
}


typedef struct {
  grn_id id;
  grn_obj value;
} sort_value_entry;

inline static int
compare_value(grn_ctx *ctx,
              sort_value_entry *a, sort_value_entry *b,
              grn_table_sort_key *keys, int n_keys,
              grn_obj *a_buffer, grn_obj *b_buffer)
{
  int i;
  uint8_t type;
  uint32_t as, bs;
  const unsigned char *ap, *bp;
  for (i = 0; i < n_keys; i++, keys++) {
    if (i) {
      GRN_BULK_REWIND(a_buffer);
      GRN_BULK_REWIND(b_buffer);
      if (keys->flags & GRN_TABLE_SORT_DESC) {
        grn_obj_get_value(ctx, keys->key, b->id, a_buffer);
        grn_obj_get_value(ctx, keys->key, a->id, b_buffer);
      } else {
        grn_obj_get_value(ctx, keys->key, a->id, a_buffer);
        grn_obj_get_value(ctx, keys->key, b->id, b_buffer);
      }
      ap = (const unsigned char *)GRN_BULK_HEAD(a_buffer);
      as = GRN_BULK_VSIZE(a_buffer);
      bp = (const unsigned char *)GRN_BULK_HEAD(b_buffer);
      bs = GRN_BULK_VSIZE(b_buffer);
    } else {
      if (keys->flags & GRN_TABLE_SORT_DESC) {
        ap = (const unsigned char *)GRN_BULK_HEAD(&b->value);
        as = GRN_BULK_VSIZE(&b->value);
        bp = (const unsigned char *)GRN_BULK_HEAD(&a->value);
        bs = GRN_BULK_VSIZE(&a->value);
      } else {
        ap = (const unsigned char *)GRN_BULK_HEAD(&a->value);
        as = GRN_BULK_VSIZE(&a->value);
        bp = (const unsigned char *)GRN_BULK_HEAD(&b->value);
        bs = GRN_BULK_VSIZE(&b->value);
      }
    }
    type = keys->offset;
    switch (type) {
    case KEY_ID :
      if (ap != bp) { return ap > bp; }
      break;
    case KEY_BULK :
      for (;; ap++, bp++, as--, bs--) {
        if (!as) { if (bs) { return 0; } else { break; } }
        if (!bs) { return 1; }
        if (*ap < *bp) { return 0; }
        if (*ap > *bp) { return 1; }
      }
      break;
    case KEY_INT8 :
      CMPNUM(int8_t);
      break;
    case KEY_INT16 :
      CMPNUM(int16_t);
      break;
    case KEY_INT32 :
      CMPNUM(int32_t);
      break;
    case KEY_INT64 :
      CMPNUM(int64_t);
      break;
    case KEY_UINT8 :
      CMPNUM(uint8_t);
      break;
    case KEY_UINT16 :
      CMPNUM(uint16_t);
      break;
    case KEY_UINT32 :
      CMPNUM(uint32_t);
      break;
    case KEY_UINT64 :
      CMPNUM(uint64_t);
      break;
    case KEY_FLOAT32 :
      if (as) {
        if (bs) {
          float va = *((float *)(ap));
          float vb = *((float *)(bp));
          if (va < vb || va > vb) { return va > vb; }
        } else {
          return 1;
        }
      } else {
        if (bs) { return 0; }
      }
      break;
    case KEY_FLOAT64 :
      if (as) {
        if (bs) {
          double va = *((double *)(ap));
          double vb = *((double *)(bp));
          if (va < vb || va > vb) { return va > vb; }
        } else {
          return 1;
        }
      } else {
        if (bs) { return 0; }
      }
      break;
    }
  }
  return 0;
}

inline static void
swap_value(sort_value_entry *a, sort_value_entry *b)
{
  sort_value_entry c_ = *a;
  *a = *b;
  *b = c_;
}

inline static sort_value_entry *
part_value(grn_ctx *ctx,
           sort_value_entry *b, sort_value_entry *e,
           grn_table_sort_key *keys, int n_keys,
           grn_obj *a_buffer, grn_obj *b_buffer)
{
  sort_value_entry *c;
  intptr_t d = e - b;
  if (compare_value(ctx, b, e, keys, n_keys, a_buffer, b_buffer)) {
    swap_value(b, e);
  }
  if (d < 2) { return NULL; }
  c = b + (d >> 1);
  if (compare_value(ctx, b, c, keys, n_keys, a_buffer, b_buffer)) {
    swap_value(b, c);
  } else {
    if (compare_value(ctx, c, e, keys, n_keys, a_buffer, b_buffer)) {
      swap_value(c, e);
    }
  }
  if (d < 3) { return NULL; }
  b++;
  swap_value(b, c);
  c = b;
  for (;;) {
    do {
      b++;
    } while (compare_value(ctx, c, b, keys, n_keys, a_buffer, b_buffer));
    do {
      e--;
    } while (compare_value(ctx, e, c, keys, n_keys, a_buffer, b_buffer));
    if (b >= e) { break; }
    swap_value(b, e);
  }
  swap_value(c, e);
  return e;
}

static void
sort_value(grn_ctx *ctx,
           sort_value_entry *head, sort_value_entry *tail,
           int from, int to,
           grn_table_sort_key *keys, int n_keys,
           grn_obj *a_buffer, grn_obj *b_buffer)
{
  sort_value_entry *c;
  if (head < tail && (c = part_value(ctx, head, tail, keys, n_keys,
                                     a_buffer, b_buffer))) {
    intptr_t m = c - head + 1;
    if (from < m - 1) {
      sort_value(ctx, head, c - 1, from, to, keys, n_keys, a_buffer, b_buffer);
    }
    if (m < to) {
      sort_value(ctx, c + 1, tail, from - m, to - m, keys, n_keys,
                 a_buffer, b_buffer);
    }
  }
}

static sort_value_entry *
pack_value(grn_ctx *ctx, grn_obj *table,
           sort_value_entry *head, sort_value_entry *tail,
           grn_table_sort_key *keys, int n_keys,
           grn_obj *a_buffer, grn_obj *b_buffer)
{
  int i = 0;
  sort_value_entry e, c;
  grn_table_cursor *tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0);
  if (!tc) { return NULL; }
  if ((c.id = grn_table_cursor_next_inline(ctx, tc))) {
    GRN_TEXT_INIT(&c.value, 0);
    grn_obj_get_value(ctx, keys->key, c.id, &c.value);
    while ((e.id = grn_table_cursor_next_inline(ctx, tc))) {
      GRN_TEXT_INIT(&e.value, 0);
      grn_obj_get_value(ctx, keys->key, e.id, &e.value);
      if (compare_value(ctx, &c, &e, keys, n_keys, a_buffer, b_buffer)) {
        *head++ = e;
      } else {
        *tail-- = e;
      }
      i++;
    }
    *head = c;
    i++;
  }
  grn_table_cursor_close(ctx, tc);
  return i > 2 ? head : NULL;
}

static int
grn_table_sort_value(grn_ctx *ctx, grn_obj *table,
                     int offset, int limit,
                     grn_obj *result,
                     grn_table_sort_key *keys, int n_keys)
{
  int e, n;
  sort_value_entry *array, *ep;
  e = offset + limit;
  n = grn_table_size(ctx, table);
  if (!(array = GRN_MALLOC(sizeof(sort_value_entry) * n))) {
    return 0;
  }
  {
    grn_obj a_buffer;
    grn_obj b_buffer;
    GRN_TEXT_INIT(&a_buffer, 0);
    GRN_TEXT_INIT(&b_buffer, 0);
    if ((ep = pack_value(ctx, table, array, array + n - 1, keys, n_keys,
                         &a_buffer, &b_buffer))) {
      intptr_t m = ep - array + 1;
      if (offset < m - 1) {
        sort_value(ctx, array, ep - 1, offset, e, keys, n_keys,
                   &a_buffer, &b_buffer);
      }
      if (m < e) {
        sort_value(ctx, ep + 1, array + n - 1, offset - m, e - m, keys, n_keys,
                   &a_buffer, &b_buffer);
      }
    }
    GRN_OBJ_FIN(ctx, &a_buffer);
    GRN_OBJ_FIN(ctx, &b_buffer);
  }
  {
    int i;
    grn_id *v;
    for (i = 0, ep = array + offset; i < limit && ep < array + n; i++, ep++) {
      if (!grn_array_add(ctx, (grn_array *)result, (void **)&v)) { break; }
      *v = ep->id;
    }
    GRN_FREE(array);
    return i;
  }
}

static grn_bool
is_compressed_column(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj *target_obj;

  if (!obj) {
    return GRN_FALSE;
  }

  if (obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a = (grn_accessor *)obj;
    while (a->next) {
      a = a->next;
    }
    target_obj = a->obj;
  } else {
    target_obj = obj;
  }

  if (target_obj->header.type != GRN_COLUMN_VAR_SIZE) {
    return GRN_FALSE;
  }

  switch (target_obj->header.flags & GRN_OBJ_COMPRESS_MASK) {
  case GRN_OBJ_COMPRESS_ZLIB :
  case GRN_OBJ_COMPRESS_LZ4 :
  case GRN_OBJ_COMPRESS_ZSTD :
    return GRN_TRUE;
  default :
    return GRN_FALSE;
  }
}

static grn_bool
is_sub_record_accessor(grn_ctx *ctx, grn_obj *obj)
{
  grn_accessor *accessor;

  if (!obj) {
    return GRN_FALSE;
  }

  if (obj->header.type != GRN_ACCESSOR) {
    return GRN_FALSE;
  }

  for (accessor = (grn_accessor *)obj; accessor; accessor = accessor->next) {
    switch (accessor->action) {
    case GRN_ACCESSOR_GET_VALUE :
      if (GRN_TABLE_IS_MULTI_KEYS_GROUPED(accessor->obj)) {
        return GRN_TRUE;
      }
      break;
    default :
      break;
    }
  }

  return GRN_FALSE;
}

static grn_bool
is_encoded_pat_key_accessor(grn_ctx *ctx, grn_obj *obj)
{
  grn_accessor *accessor;

  if (!grn_obj_is_accessor(ctx, obj)) {
    return GRN_FALSE;
  }

  accessor = (grn_accessor *)obj;
  while (accessor->next) {
    accessor = accessor->next;
  }

  if (accessor->action != GRN_ACCESSOR_GET_KEY) {
    return GRN_FALSE;
  }

  if (accessor->obj->header.type != GRN_TABLE_PAT_KEY) {
    return GRN_FALSE;
  }

  return grn_pat_is_key_encoded(ctx, (grn_pat *)(accessor->obj));
}

static int
range_is_idp(grn_obj *obj)
{
  if (obj && obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a;
    for (a = (grn_accessor *)obj; a; a = a->next) {
      if (a->action == GRN_ACCESSOR_GET_ID) { return 1; }
    }
  }
  return 0;
}

int
grn_table_sort(grn_ctx *ctx, grn_obj *table, int offset, int limit,
               grn_obj *result, grn_table_sort_key *keys, int n_keys)
{
  grn_rc rc;
  grn_obj *index;
  int n, e, i = 0;
  GRN_API_ENTER;
  if (!n_keys || !keys) {
    WARN(GRN_INVALID_ARGUMENT, "keys is null");
    goto exit;
  }
  if (!table) {
    WARN(GRN_INVALID_ARGUMENT, "table is null");
    goto exit;
  }
  if (!(result && result->header.type == GRN_TABLE_NO_KEY)) {
    WARN(GRN_INVALID_ARGUMENT, "result is not a array");
    goto exit;
  }
  n = grn_table_size(ctx, table);
  if ((rc = grn_normalize_offset_and_limit(ctx, n, &offset, &limit))) {
    ERR(rc, "grn_normalize_offset_and_limit failed");
    goto exit;
  } else {
    e = offset + limit;
  }
  if (keys->flags & GRN_TABLE_SORT_GEO) {
    if (n_keys == 2) {
      i = grn_geo_table_sort(ctx, table, offset, limit, result,
                             keys[0].key, keys[1].key);
    } else {
      i = 0;
    }
    goto exit;
  }
  if (n_keys == 1 && !GRN_ACCESSORP(keys->key) &&
      grn_column_index(ctx, keys->key, GRN_OP_LESS, &index, 1, NULL)) {
    grn_id tid;
    grn_pat *lexicon = (grn_pat *)grn_ctx_at(ctx, index->header.domain);
    grn_pat_cursor *pc = grn_pat_cursor_open(ctx, lexicon, NULL, 0, NULL, 0,
                                             0 /* offset : can be used in unique index */,
                                             -1 /* limit : can be used in unique index */,
                                             (keys->flags & GRN_TABLE_SORT_DESC)
                                             ? GRN_CURSOR_DESCENDING
                                             : GRN_CURSOR_ASCENDING);
    if (pc) {
      while (i < e && (tid = grn_pat_cursor_next(ctx, pc))) {
        grn_ii_cursor *ic = grn_ii_cursor_open(ctx, (grn_ii *)index, tid, 0, 0, 1, 0);
        if (ic) {
          grn_posting *posting;
          while (i < e && (posting = grn_ii_cursor_next(ctx, ic))) {
            if (offset <= i) {
              grn_id *v;
              if (!grn_array_add(ctx, (grn_array *)result, (void **)&v)) { break; }
              *v = posting->rid;
            }
            i++;
          }
          grn_ii_cursor_close(ctx, ic);
        }
      }
      grn_pat_cursor_close(ctx, pc);
    }
  } else {
    int j;
    grn_bool have_compressed_column = GRN_FALSE;
    grn_bool have_sub_record_accessor = GRN_FALSE;
    grn_bool have_encoded_pat_key_accessor = GRN_FALSE;
    grn_bool have_index_value_get = GRN_FALSE;
    grn_table_sort_key *kp;
    for (kp = keys, j = n_keys; j; kp++, j--) {
      if (is_compressed_column(ctx, kp->key)) {
        have_compressed_column = GRN_TRUE;
      }
      if (is_sub_record_accessor(ctx, kp->key)) {
        have_sub_record_accessor = GRN_TRUE;
      }
      if (is_encoded_pat_key_accessor(ctx, kp->key)) {
        have_encoded_pat_key_accessor = GRN_TRUE;
      }
      if (range_is_idp(kp->key)) {
        kp->offset = KEY_ID;
      } else {
        grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, kp->key));
        if (range->header.type == GRN_TYPE) {
          if (range->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
            kp->offset = KEY_BULK;
          } else {
            uint8_t key_type = range->header.flags & GRN_OBJ_KEY_MASK;
            switch (key_type) {
            case GRN_OBJ_KEY_UINT :
            case GRN_OBJ_KEY_GEO_POINT :
              switch (GRN_TYPE_SIZE(DB_OBJ(range))) {
              case 1 :
                kp->offset = KEY_UINT8;
                break;
              case 2 :
                kp->offset = KEY_UINT16;
                break;
              case 4 :
                kp->offset = KEY_UINT32;
                break;
              case 8 :
                kp->offset = KEY_UINT64;
                break;
              default :
                ERR(GRN_INVALID_ARGUMENT, "unsupported uint value");
                goto exit;
              }
              break;
            case GRN_OBJ_KEY_INT :
              switch (GRN_TYPE_SIZE(DB_OBJ(range))) {
              case 1 :
                kp->offset = KEY_INT8;
                break;
              case 2 :
                kp->offset = KEY_INT16;
                break;
              case 4 :
                kp->offset = KEY_INT32;
                break;
              case 8 :
                kp->offset = KEY_INT64;
                break;
              default :
                ERR(GRN_INVALID_ARGUMENT, "unsupported int value");
                goto exit;
              }
              break;
            case GRN_OBJ_KEY_FLOAT :
              switch (GRN_TYPE_SIZE(DB_OBJ(range))) {
              case 4 :
                kp->offset = KEY_FLOAT32;
                break;
              case 8 :
                kp->offset = KEY_FLOAT64;
                break;
              default :
                ERR(GRN_INVALID_ARGUMENT, "unsupported float value");
                goto exit;
              }
              break;
            }
          }
        } else {
          if (kp->key->header.type == GRN_COLUMN_INDEX) {
            have_index_value_get = GRN_TRUE;
          }
          kp->offset = KEY_UINT32;
        }
      }
    }
    if (have_compressed_column ||
        have_sub_record_accessor ||
        have_encoded_pat_key_accessor ||
        have_index_value_get) {
      i = grn_table_sort_value(ctx, table, offset, limit, result,
                               keys, n_keys);
    } else {
      i = grn_table_sort_reference(ctx, table, offset, limit, result,
                                   keys, n_keys);
    }
  }
exit :
  GRN_API_RETURN(i);
}

static grn_obj *
deftype(grn_ctx *ctx, const char *name,
        grn_obj_flags flags,  unsigned int size)
{
  grn_obj *o = grn_ctx_get(ctx, name, strlen(name));
  if (!o) { o = grn_type_create(ctx, name, strlen(name), flags, size); }
  return o;
}

grn_rc
grn_db_init_builtin_types(grn_ctx *ctx)
{
  grn_id id;
  grn_obj *obj, *db = ctx->impl->db;
  char buf[] = "Sys00";
  grn_obj_register(ctx, db, buf, 5);
  obj = deftype(ctx, "Object",
                GRN_OBJ_KEY_UINT, sizeof(uint64_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_OBJECT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Bool",
                GRN_OBJ_KEY_UINT, sizeof(uint8_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_BOOL) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Int8",
                GRN_OBJ_KEY_INT, sizeof(int8_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_INT8) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "UInt8",
                GRN_OBJ_KEY_UINT, sizeof(uint8_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_UINT8) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Int16",
                GRN_OBJ_KEY_INT, sizeof(int16_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_INT16) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "UInt16",
                GRN_OBJ_KEY_UINT, sizeof(uint16_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_UINT16) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Int32",
                GRN_OBJ_KEY_INT, sizeof(int32_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_INT32) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "UInt32",
                GRN_OBJ_KEY_UINT, sizeof(uint32_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_UINT32) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Int64",
                GRN_OBJ_KEY_INT, sizeof(int64_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_INT64) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "UInt64",
                GRN_OBJ_KEY_UINT, sizeof(uint64_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_UINT64) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Float",
                GRN_OBJ_KEY_FLOAT, sizeof(double));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_FLOAT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Time",
                GRN_OBJ_KEY_INT, sizeof(int64_t));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_TIME) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "ShortText",
                GRN_OBJ_KEY_VAR_SIZE, GRN_TABLE_MAX_KEY_SIZE);
  if (!obj || DB_OBJ(obj)->id != GRN_DB_SHORT_TEXT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "Text",
                GRN_OBJ_KEY_VAR_SIZE, 1 << 16);
  if (!obj || DB_OBJ(obj)->id != GRN_DB_TEXT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "LongText",
                GRN_OBJ_KEY_VAR_SIZE, 1U << 31);
  if (!obj || DB_OBJ(obj)->id != GRN_DB_LONG_TEXT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "TokyoGeoPoint",
                GRN_OBJ_KEY_GEO_POINT, sizeof(grn_geo_point));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_TOKYO_GEO_POINT) { return GRN_FILE_CORRUPT; }
  obj = deftype(ctx, "WGS84GeoPoint",
                GRN_OBJ_KEY_GEO_POINT, sizeof(grn_geo_point));
  if (!obj || DB_OBJ(obj)->id != GRN_DB_WGS84_GEO_POINT) { return GRN_FILE_CORRUPT; }
  for (id = grn_db_curr_id(ctx, db) + 1; id < GRN_DB_MECAB; id++) {
    grn_itoh(id, buf + 3, 2);
    grn_obj_register(ctx, db, buf, 5);
  }
#ifdef GRN_WITH_MECAB
  if (grn_db_init_mecab_tokenizer(ctx)) {
    ERRCLR(ctx);
#endif
    grn_obj_register(ctx, db, "TokenMecab", 10);
#ifdef GRN_WITH_MECAB
  }
#endif
  grn_db_init_builtin_tokenizers(ctx);
  grn_db_init_builtin_normalizers(ctx);
  grn_db_init_builtin_scorers(ctx);
  for (id = grn_db_curr_id(ctx, db) + 1; id < 128; id++) {
    grn_itoh(id, buf + 3, 2);
    grn_obj_register(ctx, db, buf, 5);
  }
  grn_db_init_builtin_commands(ctx);
  grn_db_init_builtin_window_functions(ctx);
  for (id = grn_db_curr_id(ctx, db) + 1; id < GRN_N_RESERVED_TYPES; id++) {
    grn_itoh(id, buf + 3, 2);
    grn_obj_register(ctx, db, buf, 5);
  }
  return ctx->rc;
}

#define MULTI_COLUMN_INDEXP(i) (DB_OBJ(i)->source_size > sizeof(grn_id))

static grn_obj *
grn_index_column_get_tokenizer(grn_ctx *ctx, grn_obj *index_column)
{
  grn_obj *tokenizer;
  grn_obj *lexicon;

  lexicon = grn_ctx_at(ctx, index_column->header.domain);
  if (!lexicon) {
    return NULL;
  }

  grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL, NULL);
  return tokenizer;
}

static grn_bool
is_full_text_searchable_index(grn_ctx *ctx, grn_obj *index_column)
{
  grn_obj *tokenizer;

  tokenizer = grn_index_column_get_tokenizer(ctx, index_column);
  return tokenizer != NULL;
}

static int
grn_column_find_index_data_column_equal(grn_ctx *ctx, grn_obj *obj,
                                        grn_operator op,
                                        grn_index_datum *index_data,
                                        unsigned int n_index_data,
                                        grn_obj **index_buf, int buf_size,
                                        int *section_buf)
{
  int n = 0;
  grn_obj **ip = index_buf;
  grn_hook *hooks;

  for (hooks = DB_OBJ(obj)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    int section;
    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    if (obj->header.type != GRN_COLUMN_FIX_SIZE) {
      if (is_full_text_searchable_index(ctx, target)) { continue; }
    }
    section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
    if (section_buf) { *section_buf = section; }
    if (n < buf_size) {
      *ip++ = target;
    }
    if ((unsigned int) n < n_index_data) {
      index_data[n].index = target;
      index_data[n].section = section;
    }
    n++;
  }

  return n;
}

static grn_bool
is_valid_regexp_index(grn_ctx *ctx, grn_obj *index_column)
{
  grn_obj *tokenizer;

  tokenizer = grn_index_column_get_tokenizer(ctx, index_column);
  /* TODO: Restrict to TokenRegexp? */
  return tokenizer != NULL;
}

static int
grn_column_find_index_data_column_match(grn_ctx *ctx, grn_obj *obj,
                                        grn_operator op,
                                        grn_index_datum *index_data,
                                        unsigned int n_index_data,
                                        grn_obj **index_buf, int buf_size,
                                        int *section_buf)
{
  int n = 0;
  grn_obj **ip = index_buf;
  grn_hook_entry hook_entry;
  grn_hook *hooks;
  grn_bool prefer_full_text_search_index = GRN_FALSE;

  switch (obj->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    hook_entry = GRN_HOOK_INSERT;
    break;
  default :
    hook_entry = GRN_HOOK_SET;
    break;
  }

  if (op != GRN_OP_REGEXP && !grn_column_is_vector(ctx, obj)) {
    prefer_full_text_search_index = GRN_TRUE;
  }

  if (prefer_full_text_search_index) {
    for (hooks = DB_OBJ(obj)->hooks[hook_entry]; hooks; hooks = hooks->next) {
      grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
      grn_obj *target = grn_ctx_at(ctx, data->target);
      int section;
      if (target->header.type != GRN_COLUMN_INDEX) { continue; }
      if (!is_full_text_searchable_index(ctx, target)) { continue; }
      section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
      if (section_buf) { *section_buf = section; }
      if (n < buf_size) {
        *ip++ = target;
      }
      if ((unsigned int) n < n_index_data) {
        index_data[n].index = target;
        index_data[n].section = section;
      }
      n++;
    }
  }

  for (hooks = DB_OBJ(obj)->hooks[hook_entry]; hooks; hooks = hooks->next) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    int section;

    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    if (op == GRN_OP_REGEXP && !is_valid_regexp_index(ctx, target)) {
      continue;
    }

    if (prefer_full_text_search_index) {
      if (is_full_text_searchable_index(ctx, target)) { continue; }
    }

    section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
    if (section_buf) { *section_buf = section; }
    if (n < buf_size) {
      *ip++ = target;
    }
    if ((unsigned int) n < n_index_data) {
      index_data[n].index = target;
      index_data[n].section = section;
    }
    n++;
  }

  return n;
}

static int
grn_column_find_index_data_column_range(grn_ctx *ctx, grn_obj *obj,
                                        grn_operator op,
                                        grn_index_datum *index_data,
                                        unsigned int n_index_data,
                                        grn_obj **index_buf, int buf_size,
                                        int *section_buf)
{
  int n = 0;
  grn_obj **ip = index_buf;
  grn_hook_entry hook_entry;
  grn_hook *hooks;

  switch (obj->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    hook_entry = GRN_HOOK_INSERT;
    break;
  default :
    hook_entry = GRN_HOOK_SET;
    break;
  }

  for (hooks = DB_OBJ(obj)->hooks[hook_entry]; hooks; hooks = hooks->next) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    int section;
    if (!target) { continue; }
    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
    if (section_buf) { *section_buf = section; }
    {
      grn_obj *tokenizer, *lexicon = grn_ctx_at(ctx, target->header.domain);
      if (!lexicon) { continue; }
      if (lexicon->header.type != GRN_TABLE_PAT_KEY) { continue; }
      /* FIXME: GRN_TABLE_DAT_KEY should be supported */
      grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL, NULL);
      if (tokenizer) { continue; }
    }
    if (n < buf_size) {
      *ip++ = target;
    }
    if ((unsigned int) n < n_index_data) {
      index_data[n].index = target;
      index_data[n].section = section;
    }
    n++;
  }

  return n;
}

static grn_bool
is_valid_match_index(grn_ctx *ctx, grn_obj *index_column)
{
  return GRN_TRUE;
}

static grn_bool
is_valid_range_index(grn_ctx *ctx, grn_obj *index_column)
{
  grn_obj *tokenizer;
  grn_obj *lexicon;

  lexicon = grn_ctx_at(ctx, index_column->header.domain);
  if (!lexicon) { return GRN_FALSE; }
  /* FIXME: GRN_TABLE_DAT_KEY should be supported */
  if (lexicon->header.type != GRN_TABLE_PAT_KEY) {
    grn_obj_unlink(ctx, lexicon);
    return GRN_FALSE;
  }

  grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL, NULL);
  grn_obj_unlink(ctx, lexicon);
  if (tokenizer) { return GRN_FALSE; }

  return GRN_TRUE;
}

static grn_bool
is_valid_index(grn_ctx *ctx, grn_obj *index_column, grn_operator op)
{
  switch (op) {
  case GRN_OP_MATCH :
  case GRN_OP_NEAR :
  case GRN_OP_NEAR2 :
  case GRN_OP_SIMILAR :
    return is_valid_match_index(ctx, index_column);
    break;
  case GRN_OP_LESS :
  case GRN_OP_GREATER :
  case GRN_OP_LESS_EQUAL :
  case GRN_OP_GREATER_EQUAL :
  case GRN_OP_CALL :
    return is_valid_range_index(ctx, index_column);
    break;
  case GRN_OP_REGEXP :
    return is_valid_regexp_index(ctx, index_column);
    break;
  default :
    return GRN_FALSE;
    break;
  }
}

static int
find_section(grn_ctx *ctx, grn_obj *index_column, grn_obj *indexed_column)
{
  int section = 0;
  grn_id indexed_column_id;
  grn_id *source_ids;
  int i, n_source_ids;

  indexed_column_id = DB_OBJ(indexed_column)->id;

  source_ids = DB_OBJ(index_column)->source;
  n_source_ids = DB_OBJ(index_column)->source_size / sizeof(grn_id);
  for (i = 0; i < n_source_ids; i++) {
    grn_id source_id = source_ids[i];
    if (source_id == indexed_column_id) {
      section = i + 1;
      break;
    }
  }

  return section;
}

static int
grn_column_find_index_data_accessor_index_column(grn_ctx *ctx, grn_accessor *a,
                                                 grn_operator op,
                                                 grn_index_datum *index_data,
                                                 unsigned int n_index_data,
                                                 grn_obj **index_buf,
                                                 int buf_size,
                                                 int *section_buf)
{
  grn_obj *index_column = a->obj;
  int section = 0;

  if (!is_valid_index(ctx, index_column, op)) {
    return 0;
  }

  if (a->next) {
    int specified_section;
    grn_bool is_invalid_section;
    if (a->next->next) {
      return 0;
    }
    specified_section = find_section(ctx, index_column, a->next->obj);
    is_invalid_section = (specified_section == 0);
    if (is_invalid_section) {
      return 0;
    }
    section = specified_section;
    if (section_buf) {
      *section_buf = section;
    }
  }
  if (buf_size > 0) {
    *index_buf = index_column;
  }
  if (n_index_data > 0) {
    index_data[0].index = index_column;
    index_data[0].section = section;
  }

  return 1;
}

static grn_bool
grn_column_find_index_data_accessor_is_key_search(grn_ctx *ctx,
                                                  grn_accessor *accessor,
                                                  grn_operator op)
{
  if (accessor->next) {
    return GRN_FALSE;
  }

  if (accessor->action != GRN_ACCESSOR_GET_KEY) {
    return GRN_FALSE;
  }

  if (!grn_obj_is_table(ctx, accessor->obj)) {
    return GRN_FALSE;
  }

  switch (op) {
  case GRN_OP_LESS :
  case GRN_OP_GREATER :
  case GRN_OP_LESS_EQUAL :
  case GRN_OP_GREATER_EQUAL :
    switch (accessor->obj->header.type) {
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
      return GRN_TRUE;
    default :
      return GRN_FALSE;
    }
  case GRN_OP_EQUAL :
    switch (accessor->obj->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
      return GRN_TRUE;
    default :
      return GRN_FALSE;
    }
  default :
    return GRN_FALSE;
  }
}

static int
grn_column_find_index_data_accessor_match(grn_ctx *ctx, grn_obj *obj,
                                          grn_operator op,
                                          grn_index_datum *index_data,
                                          unsigned n_index_data,
                                          grn_obj **index_buf, int buf_size,
                                          int *section_buf)
{
  int n = 0;
  grn_obj **ip = index_buf;
  grn_accessor *a = (grn_accessor *)obj;

  while (a) {
    grn_hook *hooks;
    grn_bool found = GRN_FALSE;
    grn_hook_entry entry = (grn_hook_entry)-1;

    if (a->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        GRN_OBJ_INDEX_COLUMNP(a->obj)) {
      return grn_column_find_index_data_accessor_index_column(ctx, a, op,
                                                              index_data,
                                                              n_index_data,
                                                              index_buf,
                                                              buf_size,
                                                              section_buf);
    }

    switch (a->action) {
    case GRN_ACCESSOR_GET_KEY :
      entry = GRN_HOOK_INSERT;
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      entry = GRN_HOOK_SET;
      break;
    default :
      break;
    }

    if (entry == (grn_hook_entry)-1) {
      break;
    }

    for (hooks = DB_OBJ(a->obj)->hooks[entry]; hooks; hooks = hooks->next) {
      grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
      grn_obj *target = grn_ctx_at(ctx, data->target);

      if (target->header.type != GRN_COLUMN_INDEX) { continue; }

      found = GRN_TRUE;
      if (!a->next) {
        int section;

        if (!is_valid_index(ctx, target, op)) {
          continue;
        }

        section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
        if (section_buf) {
          *section_buf = section;
        }
        if (n < buf_size) {
          *ip++ = target;
        }
        if ((unsigned int) n < n_index_data) {
          index_data[n].index = target;
          index_data[n].section = section;
        }
        n++;
      }
    }

    if (!found &&
        grn_column_find_index_data_accessor_is_key_search(ctx, a, op)) {
      grn_obj *index;
      int section = 0;

      if ((grn_obj *)a == obj) {
        index = a->obj;
      } else {
        index = (grn_obj *)a;
      }

      found = GRN_TRUE;
      if (section_buf) {
        *section_buf = section;
      }
      if (n < buf_size) {
        *ip++ = index;
      }
      if ((unsigned int) n < n_index_data) {
        index_data[n].index = index;
        index_data[n].section = section;
      }
      n++;
    }

    if (!found &&
        a->next &&
        grn_obj_is_table(ctx, a->obj) &&
        a->obj->header.domain == a->next->obj->header.domain) {
      grn_obj *index = (grn_obj *)a;
      int section = 0;

      found = GRN_TRUE;
      if (section_buf) {
        *section_buf = section;
      }
      if (n < buf_size) {
        *ip++ = index;
      }
      if ((unsigned int) n < n_index_data) {
        index_data[n].index = index;
        index_data[n].section = section;
      }
      n++;
    }

    if (!found) {
      break;
    }
    a = a->next;
  }

  return n;
}

static int
grn_column_find_index_data_accessor(grn_ctx *ctx, grn_obj *obj,
                                    grn_operator op,
                                    grn_index_datum *index_data,
                                    unsigned n_index_data,
                                    grn_obj **index_buf, int buf_size,
                                    int *section_buf)
{
  int n = 0;

  if (section_buf) {
    *section_buf = 0;
  }
  switch (op) {
  case GRN_OP_EQUAL :
  case GRN_OP_NOT_EQUAL :
  case GRN_OP_TERM_EXTRACT :
    if (buf_size > 0) {
      index_buf[n] = obj;
    }
    if (n_index_data > 0) {
      index_data[n].index   = obj;
      index_data[n].section = 0;
    }
    n++;
    break;
  case GRN_OP_PREFIX :
    {
      grn_accessor *a = (grn_accessor *)obj;
      if (a->action == GRN_ACCESSOR_GET_KEY) {
        if (a->obj->header.type == GRN_TABLE_PAT_KEY) {
          if (buf_size > 0) {
            index_buf[n] = obj;
          }
          if (n_index_data > 0) {
            index_data[n].index   = obj;
            index_data[n].section = 0;
          }
          n++;
        }
        /* FIXME: GRN_TABLE_DAT_KEY should be supported */
      }
    }
    break;
  case GRN_OP_SUFFIX :
    {
      grn_accessor *a = (grn_accessor *)obj;
      if (a->action == GRN_ACCESSOR_GET_KEY) {
        if (a->obj->header.type == GRN_TABLE_PAT_KEY &&
            a->obj->header.flags & GRN_OBJ_KEY_WITH_SIS) {
          if (buf_size > 0) {
            index_buf[n]         = obj;
          }
          if (n_index_data > 0) {
            index_data[n].index   = obj;
            index_data[n].section = 0;
          }
          n++;
        }
      }
    }
    break;
  case GRN_OP_MATCH :
  case GRN_OP_NEAR :
  case GRN_OP_NEAR2 :
  case GRN_OP_SIMILAR :
  case GRN_OP_LESS :
  case GRN_OP_GREATER :
  case GRN_OP_LESS_EQUAL :
  case GRN_OP_GREATER_EQUAL :
  case GRN_OP_CALL :
  case GRN_OP_REGEXP :
  case GRN_OP_FUZZY :
    n = grn_column_find_index_data_accessor_match(ctx, obj, op,
                                                  index_data, n_index_data,
                                                  index_buf, buf_size,
                                                  section_buf);
    break;
  default :
    break;
  }

  return n;
}

int
grn_column_index(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                 grn_obj **index_buf, int buf_size, int *section_buf)
{
  int n = 0;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    switch (op) {
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
      n = grn_column_find_index_data_column_equal(ctx, obj, op,
                                                  NULL, 0,
                                                  index_buf, buf_size,
                                                  section_buf);
      break;
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_REGEXP :
    case GRN_OP_FUZZY :
      n = grn_column_find_index_data_column_match(ctx, obj, op,
                                                  NULL, 0,
                                                  index_buf, buf_size,
                                                  section_buf);
      break;
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_CALL :
      n = grn_column_find_index_data_column_range(ctx, obj, op,
                                                  NULL, 0,
                                                  index_buf, buf_size,
                                                  section_buf);
      break;
    default :
      break;
    }
  } else if (GRN_ACCESSORP(obj)) {
    n = grn_column_find_index_data_accessor(ctx, obj, op,
                                            NULL, 0,
                                            index_buf, buf_size,
                                            section_buf);
  }
  GRN_API_RETURN(n);
}

unsigned int
grn_column_find_index_data(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                           grn_index_datum *index_data,
                           unsigned int n_index_data)
{
  unsigned int n = 0;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    switch (op) {
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
      n = grn_column_find_index_data_column_equal(ctx, obj, op,
                                                  index_data, n_index_data,
                                                  NULL, 0, NULL);
      break;
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_REGEXP :
    case GRN_OP_FUZZY :
      n = grn_column_find_index_data_column_match(ctx, obj, op,
                                                  index_data, n_index_data,
                                                  NULL, 0, NULL);
      break;
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_CALL :
      n = grn_column_find_index_data_column_range(ctx, obj, op,
                                                  index_data, n_index_data,
                                                  NULL, 0, NULL);
      break;
    default :
      break;
    }
  } else if (GRN_ACCESSORP(obj)) {
    n = grn_column_find_index_data_accessor(ctx, obj, op,
                                            index_data, n_index_data,
                                            NULL, 0, NULL);
  }
  GRN_API_RETURN(n);
}

static uint32_t
grn_column_get_all_index_data_column(grn_ctx *ctx,
                                     grn_obj *obj,
                                     grn_index_datum *index_data,
                                     uint32_t n_index_data)
{
  uint32_t n = 0;
  grn_hook_entry hook_entry;
  grn_hook *hooks;

  switch (obj->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    hook_entry = GRN_HOOK_INSERT;
    break;
  default :
    hook_entry = GRN_HOOK_SET;
    break;
  }

  for (hooks = DB_OBJ(obj)->hooks[hook_entry]; hooks; hooks = hooks->next) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    int section = 0;
    if (!target) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int length;
      char hook_name[GRN_TABLE_MAX_KEY_SIZE];
      int hook_name_length;

      length = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
      hook_name_length = grn_table_get_key(ctx,
                                           ctx->impl->db,
                                           data->target,
                                           hook_name,
                                           GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_OBJECT_CORRUPT,
          "[column][indexes][all] "
          "hook has a dangling reference: <%.*s> -> <%.*s>",
          length, name,
          hook_name_length, hook_name);
      continue;
    }
    if (target->header.type != GRN_COLUMN_INDEX) {
      continue;
    }
    if (MULTI_COLUMN_INDEXP(target)) {
      section = data->section;
    }
    if (n < n_index_data) {
      index_data[n].index = target;
      index_data[n].section = section;
    }
    n++;
  }

  return n;
}

static uint32_t
grn_column_get_all_index_data_accessor_index_column(grn_ctx *ctx,
                                                    grn_accessor *a,
                                                    grn_index_datum *index_data,
                                                    uint32_t n_index_data)
{
  grn_obj *index_column = a->obj;
  int section = 0;

  if (a->next) {
    int specified_section;
    grn_bool is_invalid_section;
    if (a->next->next) {
      return 0;
    }
    specified_section = find_section(ctx, index_column, a->next->obj);
    is_invalid_section = (specified_section == 0);
    if (is_invalid_section) {
      return 0;
    }
    section = specified_section;
  }
  if (n_index_data > 0) {
    index_data[0].index = index_column;
    index_data[0].section = section;
  }

  return 1;
}

static uint32_t
grn_column_get_all_index_data_accessor(grn_ctx *ctx,
                                       grn_obj *obj,
                                       grn_index_datum *index_data,
                                       uint32_t n_index_data)
{
  uint32_t n = 0;
  grn_accessor *a = (grn_accessor *)obj;

  while (a) {
    grn_hook *hooks;
    grn_bool found = GRN_FALSE;
    grn_hook_entry entry = (grn_hook_entry)-1;

    if (a->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        GRN_OBJ_INDEX_COLUMNP(a->obj)) {
      return grn_column_get_all_index_data_accessor_index_column(ctx,
                                                                 a,
                                                                 index_data,
                                                                 n_index_data);
    }

    switch (a->action) {
    case GRN_ACCESSOR_GET_KEY :
      entry = GRN_HOOK_INSERT;
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      entry = GRN_HOOK_SET;
      break;
    default :
      break;
    }

    if (entry == (grn_hook_entry)-1) {
      break;
    }

    for (hooks = DB_OBJ(a->obj)->hooks[entry]; hooks; hooks = hooks->next) {
      grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
      grn_obj *target = grn_ctx_at(ctx, data->target);

      if (target->header.type != GRN_COLUMN_INDEX) {
        continue;
      }

      found = GRN_TRUE;
      if (!a->next) {
        int section = 0;

        if (MULTI_COLUMN_INDEXP(target)) {
          section = data->section;
        }
        if (n < n_index_data) {
          index_data[n].index = target;
          index_data[n].section = section;
        }
        n++;
      }
    }

    if (!found) {
      break;
    }
    a = a->next;
  }

  return n;
}

uint32_t
grn_column_get_all_index_data(grn_ctx *ctx,
                              grn_obj *obj,
                              grn_index_datum *index_data,
                              uint32_t n_index_data)
{
  uint32_t n = 0;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    n = grn_column_get_all_index_data_column(ctx, obj,
                                             index_data, n_index_data);
  } else if (GRN_ACCESSORP(obj)) {
    n = grn_column_get_all_index_data_accessor(ctx, obj,
                                               index_data, n_index_data);
  }
  GRN_API_RETURN(n);
}

grn_rc
grn_obj_columns(grn_ctx *ctx, grn_obj *table,
                const char *str, unsigned int str_size, grn_obj *res)
{
  grn_obj *col;
  const char *p = (char *)str, *q, *r, *pe = p + str_size, *tokbuf[256];
  while (p < pe) {
    int i, n = grn_tokenize(p, pe - p, tokbuf, 256, &q);
    for (i = 0; i < n; i++) {
      r = tokbuf[i];
      while (p < r && (' ' == *p || ',' == *p)) { p++; }
      if (p < r) {
        if (r[-1] == '*') {
          grn_hash *cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                           GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
          if (cols) {
            grn_id *key;
            grn_table_columns(ctx, table, p, r - p - 1, (grn_obj *)cols);
            GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
              if ((col = grn_ctx_at(ctx, *key))) { GRN_PTR_PUT(ctx, res, col); }
            });
            grn_hash_close(ctx, cols);
          }
          {
            grn_obj *type = grn_ctx_at(ctx, table->header.domain);
            if (GRN_OBJ_TABLEP(type)) {
              grn_obj *ai = grn_obj_column(ctx, table,
                                           GRN_COLUMN_NAME_ID,
                                           GRN_COLUMN_NAME_ID_LEN);
              if (ai) {
                if (ai->header.type == GRN_ACCESSOR) {
                  grn_id *key;
                  grn_accessor *id_accessor;
                  for (id_accessor = ((grn_accessor *)ai)->next;
                       id_accessor;
                       id_accessor = id_accessor->next) {
                    grn_obj *target_table = id_accessor->obj;

                    cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                           GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
                    if (!cols) {
                      continue;
                    }
                    grn_table_columns(ctx, target_table,
                                      p, r - p - 1, (grn_obj *)cols);
                    GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
                      if ((col = grn_ctx_at(ctx, *key))) {
                        grn_accessor *a;
                        grn_accessor *ac;
                        ac = accessor_new(ctx);
                        GRN_PTR_PUT(ctx, res, (grn_obj *)ac);
                        for (a = (grn_accessor *)ai; a; a = a->next) {
                          if (a->action != GRN_ACCESSOR_GET_ID) {
                            ac->action = a->action;
                            ac->obj = a->obj;
                            ac->next = accessor_new(ctx);
                            if (!(ac = ac->next)) { break; }
                          } else {
                            ac->action = GRN_ACCESSOR_GET_COLUMN_VALUE;
                            ac->obj = col;
                            ac->next = NULL;
                            break;
                          }
                        }
                      }
                    });
                    grn_hash_close(ctx, cols);
                  }
                }
                grn_obj_unlink(ctx, ai);
              }
            }
          }
        } else if ((col = grn_obj_column(ctx, table, p, r - p))) {
          GRN_PTR_PUT(ctx, res, col);
        }
      }
      p = r;
    }
    p = q;
  }
  return ctx->rc;
}

static grn_table_sort_key *
grn_table_sort_key_from_str_geo(grn_ctx *ctx, const char *str, unsigned int str_size,
                                grn_obj *table, unsigned int *nkeys)
{
  const char **tokbuf;
  const char *p = str, *pe = str + str_size;
  grn_table_sort_key *keys = NULL, *k = NULL;
  while ((*p++ != '(')) { if (p == pe) { return NULL; } }
  str = p;
  while ((*p != ')')) { if (++p == pe) { return NULL; } }
  str_size = p - str;
  p = str;
  if ((tokbuf = GRN_MALLOCN(const char *, str_size))) {
    grn_id domain = GRN_ID_NIL;
    int i, n = grn_tokenize(str, str_size, tokbuf, str_size, NULL);
    if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
      k = keys;
      for (i = 0; i < n; i++) {
        const char *r = tokbuf[i];
        while (p < r && (' ' == *p || ',' == *p)) { p++; }
        if (p < r) {
          k->flags = GRN_TABLE_SORT_ASC;
          k->offset = 0;
          if (*p == '+') {
            p++;
          } else if (*p == '-') {
            k->flags = GRN_TABLE_SORT_DESC;
            p++;
          }
          if (k == keys) {
            if (!(k->key = grn_obj_column(ctx, table, p, r - p))) {
              WARN(GRN_INVALID_ARGUMENT, "invalid sort key: <%.*s>(<%.*s>)",
                   (int)(tokbuf[i] - p), p, str_size, str);
              break;
            }
            domain = grn_obj_get_range(ctx, k->key);
          } else {
            grn_obj buf;
            GRN_TEXT_INIT(&buf, GRN_OBJ_DO_SHALLOW_COPY);
            GRN_TEXT_SET(ctx, &buf, p + 1, r - p - 2); /* should be quoted */
            k->key = grn_obj_open(ctx, GRN_BULK, 0, domain);
            grn_obj_cast(ctx, &buf, k->key, GRN_FALSE);
            GRN_OBJ_FIN(ctx, &buf);
          }
          k->flags |= GRN_TABLE_SORT_GEO;
          k++;
        }
        p = r;
      }
    }
    GRN_FREE(tokbuf);
  }
  if (!ctx->rc && k - keys > 0) {
    *nkeys = k - keys;
  } else {
    grn_table_sort_key_close(ctx, keys, k - keys);
    *nkeys = 0;
    keys = NULL;
  }
  return keys;
}

grn_table_sort_key *
grn_table_sort_key_from_str(grn_ctx *ctx, const char *str, unsigned int str_size,
                            grn_obj *table, unsigned int *nkeys)
{
  const char *p = str;
  const char **tokbuf;
  grn_table_sort_key *keys = NULL, *k = NULL;

  if (str_size == 0) {
    return NULL;
  }

  if ((keys = grn_table_sort_key_from_str_geo(ctx, str, str_size, table, nkeys))) {
    return keys;
  }
  if ((tokbuf = GRN_MALLOCN(const char *, str_size))) {
    int i, n = grn_tokenize(str, str_size, tokbuf, str_size, NULL);
    if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
      k = keys;
      for (i = 0; i < n; i++) {
        const char *r = tokbuf[i];
        while (p < r && (' ' == *p || ',' == *p)) { p++; }
        if (p < r) {
          k->flags = GRN_TABLE_SORT_ASC;
          k->offset = 0;
          if (*p == '+') {
            p++;
          } else if (*p == '-') {
            k->flags = GRN_TABLE_SORT_DESC;
            p++;
          }
          if ((k->key = grn_obj_column(ctx, table, p, r - p))) {
            k++;
          } else {
            if (r - p == GRN_COLUMN_NAME_SCORE_LEN &&
                memcmp(p, GRN_COLUMN_NAME_SCORE, GRN_COLUMN_NAME_SCORE_LEN) == 0) {
              char table_name[GRN_TABLE_MAX_KEY_SIZE];
              int table_name_size;
              table_name_size = grn_obj_name(ctx, table,
                                             table_name,
                                             GRN_TABLE_MAX_KEY_SIZE);
              if (table_name_size == 0) {
                grn_strcpy(table_name, GRN_TABLE_MAX_KEY_SIZE, "(anonymous)");
                table_name_size = strlen(table_name);
              }
              GRN_LOG(ctx, GRN_WARN,
                      "ignore invalid sort key: <%.*s>: "
                      "table:<%*.s> keys:<%.*s>",
                      (int)(r - p), p,
                      table_name_size, table_name,
                      str_size, str);
            } else {
              char table_name[GRN_TABLE_MAX_KEY_SIZE];
              int table_name_size;
              table_name_size = grn_obj_name(ctx, table,
                                             table_name,
                                             GRN_TABLE_MAX_KEY_SIZE);
              if (table_name_size == 0) {
                grn_strcpy(table_name, GRN_TABLE_MAX_KEY_SIZE, "(anonymous)");
                table_name_size = strlen(table_name);
              }
              WARN(GRN_INVALID_ARGUMENT,
                   "invalid sort key: <%.*s>: "
                   "table:<%.*s> keys:<%.*s>",
                   (int)(r - p), p,
                   table_name_size, table_name,
                   str_size, str);
              break;
            }
          }
        }
        p = r;
      }
    }
    GRN_FREE(tokbuf);
  }
  if (!ctx->rc && k - keys > 0) {
    *nkeys = k - keys;
  } else {
    grn_table_sort_key_close(ctx, keys, k - keys);
    *nkeys = 0;
    keys = NULL;
  }
  return keys;
}

grn_rc
grn_table_sort_key_close(grn_ctx *ctx, grn_table_sort_key *keys, unsigned int nkeys)
{
  unsigned int i;
  if (keys) {
    for (i = 0; i < nkeys; i++) {
      grn_obj *key = keys[i].key;
      if (!grn_obj_is_column(ctx, key)) {
        grn_obj_unlink(ctx, key);
      }
    }
    GRN_FREE(keys);
  }
  return ctx->rc;
}

grn_bool
grn_table_is_grouped(grn_ctx *ctx, grn_obj *table)
{
  if (GRN_OBJ_TABLEP(table) && GRN_TABLE_IS_GROUPED(table)) {
    return GRN_TRUE;
  }
  return GRN_FALSE;
}

unsigned int
grn_table_max_n_subrecs(grn_ctx *ctx, grn_obj *table)
{
  if (GRN_OBJ_TABLEP(table)) {
    return DB_OBJ(table)->max_n_subrecs;
  }
  return 0;
}

grn_obj *
grn_table_tokenize(grn_ctx *ctx, grn_obj *table,
                   const char *str, unsigned int str_len,
                   grn_obj *buf, grn_bool addp)
{
  grn_token_cursor *token_cursor = NULL;
  grn_tokenize_mode mode = addp ? GRN_TOKENIZE_ADD : GRN_TOKENIZE_GET;
  GRN_API_ENTER;
  if (!(token_cursor = grn_token_cursor_open(ctx, table, str, str_len, mode, 0))) {
    goto exit;
  }
  if (buf) {
    GRN_BULK_REWIND(buf);
  } else {
    if (!(buf = grn_obj_open(ctx, GRN_UVECTOR, 0, DB_OBJ(table)->id))) {
      goto exit;
    }
  }
  while (token_cursor->status != GRN_TOKEN_CURSOR_DONE && token_cursor->status != GRN_TOKEN_CURSOR_DONE_SKIP) {
    grn_id tid;
    if ((tid = grn_token_cursor_next(ctx, token_cursor))) {
      GRN_RECORD_PUT(ctx, buf, tid);
    }
  }
exit :
  if (token_cursor) {
    grn_token_cursor_close(ctx, token_cursor);
  }
  GRN_API_RETURN(buf);
}

static void
grn_db_recover_database_remove_orphan_inspect(grn_ctx *ctx, grn_obj *db)
{
  GRN_TABLE_EACH_BEGIN_FLAGS(ctx, db, cursor, id, GRN_CURSOR_BY_ID) {
    void *key;
    int key_size;

    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
#define INSPECT     "inspect"
#define INSPECT_LEN (sizeof(INSPECT) - 1)
    if (key_size == INSPECT_LEN && memcmp(key, INSPECT, INSPECT_LEN) == 0) {
      if (!grn_ctx_at(ctx, id)) {
        ERRCLR(ctx);
        grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
      }
      break;
    }
#undef INSPECT
#undef INSPECT_LEN
  } GRN_TABLE_EACH_END(ctx, cursor);
}

static void
grn_db_recover_database(grn_ctx *ctx, grn_obj *db)
{
  if (grn_obj_is_locked(ctx, db)) {
    ERR(GRN_OBJECT_CORRUPT,
        "[db][recover] database may be broken. Please re-create the database");
    return;
  }

  grn_db_clear_dirty(ctx, db);
  grn_db_recover_database_remove_orphan_inspect(ctx, db);
}

static void
grn_db_recover_table(grn_ctx *ctx, grn_obj *table)
{
  if (!grn_obj_is_locked(ctx, table)) {
    return;
  }

  {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int name_size;
    name_size = grn_obj_name(ctx, table, name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_OBJECT_CORRUPT,
        "[db][recover] table may be broken: <%.*s>: "
        "please truncate the table (or clear lock of the table) "
        "and load data again",
        (int)name_size, name);
  }
}

static void
grn_db_recover_data_column(grn_ctx *ctx, grn_obj *data_column)
{
  if (!grn_obj_is_locked(ctx, data_column)) {
    return;
  }

  {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int name_size;
    name_size = grn_obj_name(ctx, data_column, name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_OBJECT_CORRUPT,
        "[db][recover] column may be broken: <%.*s>: "
        "please truncate the column (or clear lock of the column) "
        "and load data again",
        (int)name_size, name);
  }
}

static void
grn_db_recover_index_column(grn_ctx *ctx, grn_obj *index_column)
{
  if (!grn_obj_is_locked(ctx, index_column)) {
    return;
  }

  grn_index_column_rebuild(ctx, index_column);
}

static grn_bool
grn_db_recover_is_builtin(grn_ctx *ctx, grn_id id, grn_table_cursor *cursor)
{
  void *key;
  const char *name;
  int name_size;

  if (id < GRN_N_RESERVED_TYPES) {
    return GRN_TRUE;
  }

  name_size = grn_table_cursor_get_key(ctx, cursor, &key);
  name = key;

#define NAME_EQUAL(value)                       \
  (name_size == strlen(value) && memcmp(name, value, strlen(value)) == 0)

  if (NAME_EQUAL("inspect")) {
    /* Just for compatibility. It's needed for users who used
       Groonga master at between 2016-02-03 and 2016-02-26. */
    return GRN_TRUE;
  }

#undef NAME_EQUAL

  return GRN_FALSE;
}

grn_rc
grn_db_recover(grn_ctx *ctx, grn_obj *db)
{
  grn_table_cursor *cursor;
  grn_id id;
  grn_bool is_close_opened_object_mode;

  GRN_API_ENTER;

  is_close_opened_object_mode = (grn_thread_get_limit() == 1);

  grn_db_recover_database(ctx, db);
  if (ctx->rc != GRN_SUCCESS) {
    GRN_API_RETURN(ctx->rc);
  }

  cursor = grn_table_cursor_open(ctx, db,
                                 NULL, 0, NULL, 0,
                                 0, -1,
                                 GRN_CURSOR_BY_ID);
  if (!cursor) {
    GRN_API_RETURN(ctx->rc);
  }

  while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    grn_obj *object;

    if (is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    if ((object = grn_ctx_at(ctx, id))) {
      switch (object->header.type) {
      case GRN_TABLE_NO_KEY :
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        grn_db_recover_table(ctx, object);
        break;
      case GRN_COLUMN_FIX_SIZE :
      case GRN_COLUMN_VAR_SIZE :
        grn_db_recover_data_column(ctx, object);
        break;
      case GRN_COLUMN_INDEX :
        grn_db_recover_index_column(ctx, object);
        break;
      default:
        break;
      }
      grn_obj_unlink(ctx, object);
    } else {
      if (grn_db_recover_is_builtin(ctx, id, cursor)) {
        ERRCLR(ctx);
      }
    }

    if (is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }

    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  }
  grn_table_cursor_close(ctx, cursor);

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_db_unmap(grn_ctx *ctx, grn_obj *db)
{
  grn_id id;
  db_value *vp;
  grn_db *s = (grn_db *)db;

  GRN_API_ENTER;

  GRN_TINY_ARRAY_EACH(&s->values, 1, grn_db_curr_id(ctx, db), id, vp, {
    grn_obj *obj = vp->ptr;

    if (obj) {
      switch (obj->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
      case GRN_TABLE_NO_KEY :
      case GRN_COLUMN_FIX_SIZE :
      case GRN_COLUMN_VAR_SIZE :
      case GRN_COLUMN_INDEX :
        grn_obj_close(ctx, obj);
        break;
      }
    }
  });

  GRN_API_RETURN(ctx->rc);
}

static grn_rc
grn_ctx_get_all_objects(grn_ctx *ctx, grn_obj *objects_buffer,
                        grn_bool (*predicate)(grn_ctx *ctx, grn_obj *object))
{
  grn_obj *db;
  grn_table_cursor *cursor;
  grn_id id;

  GRN_API_ENTER;

  db = ctx->impl->db;
  if (!db) {
    ERR(GRN_INVALID_ARGUMENT, "DB isn't associated");
    GRN_API_RETURN(ctx->rc);
  }

  cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    GRN_API_RETURN(ctx->rc);
  }

  while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    grn_obj *object;

    if ((object = grn_ctx_at(ctx, id))) {
      if (predicate(ctx, object)) {
        GRN_PTR_PUT(ctx, objects_buffer, object);
      } else {
        grn_obj_unlink(ctx, object);
      }
    } else {
      if (ctx->rc != GRN_SUCCESS) {
        ERRCLR(ctx);
      }
    }
  }
  grn_table_cursor_close(ctx, cursor);

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_ctx_get_all_tables(grn_ctx *ctx, grn_obj *tables_buffer)
{
  return grn_ctx_get_all_objects(ctx, tables_buffer, grn_obj_is_table);
}

grn_rc
grn_ctx_get_all_types(grn_ctx *ctx, grn_obj *types_buffer)
{
  return grn_ctx_get_all_objects(ctx, types_buffer, grn_obj_is_type);
}

grn_rc
grn_ctx_get_all_tokenizers(grn_ctx *ctx, grn_obj *tokenizers_buffer)
{
  return grn_ctx_get_all_objects(ctx, tokenizers_buffer,
                                 grn_obj_is_tokenizer_proc);
}

grn_rc
grn_ctx_get_all_normalizers(grn_ctx *ctx, grn_obj *normalizers_buffer)
{
  return grn_ctx_get_all_objects(ctx, normalizers_buffer,
                                 grn_obj_is_normalizer_proc);
}

grn_rc
grn_ctx_get_all_token_filters(grn_ctx *ctx, grn_obj *token_filters_buffer)
{
  return grn_ctx_get_all_objects(ctx, token_filters_buffer,
                                 grn_obj_is_token_filter_proc);
}

grn_rc
grn_ctx_push_temporary_open_space(grn_ctx *ctx)
{
  grn_obj *stack;
  grn_obj *space;
  grn_obj buffer;

  GRN_API_ENTER;

  stack = &(ctx->impl->temporary_open_spaces.stack);
  GRN_VOID_INIT(&buffer);
  grn_bulk_write(ctx, stack, (const char *)&buffer, sizeof(grn_obj));
  space = ((grn_obj *)GRN_BULK_CURR(stack)) - 1;
  GRN_PTR_INIT(space, GRN_OBJ_VECTOR | GRN_OBJ_OWN, GRN_ID_NIL);

  ctx->impl->temporary_open_spaces.current = space;

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_ctx_pop_temporary_open_space(grn_ctx *ctx)
{
  grn_obj *stack;
  grn_obj *space;

  GRN_API_ENTER;

  stack = &(ctx->impl->temporary_open_spaces.stack);
  if (GRN_BULK_EMPTYP(stack)) {
    ERR(GRN_INVALID_ARGUMENT,
        "[ctx][temporary-open-spaces][pop] too much pop");
    GRN_API_RETURN(ctx->rc);
  }

  space = ctx->impl->temporary_open_spaces.current;
  GRN_OBJ_FIN(ctx, space);
  grn_bulk_truncate(ctx, stack, GRN_BULK_VSIZE(stack) - sizeof(grn_obj));

  if (GRN_BULK_EMPTYP(stack)) {
    space = NULL;
  } else {
    space = ((grn_obj *)GRN_BULK_CURR(stack)) - 1;
  }
  ctx->impl->temporary_open_spaces.current = space;

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_ctx_merge_temporary_open_space(grn_ctx *ctx)
{
  grn_obj *stack;
  grn_obj *space;
  grn_obj *next_space;

  GRN_API_ENTER;

  stack = &(ctx->impl->temporary_open_spaces.stack);
  if ((unsigned long) GRN_BULK_VSIZE(stack) < (unsigned long) sizeof(grn_obj) * 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "[ctx][temporary-open-spaces][merge] "
        "merge requires at least two spaces");
    GRN_API_RETURN(ctx->rc);
  }

  space = ctx->impl->temporary_open_spaces.current;
  next_space = ctx->impl->temporary_open_spaces.current - 1;
  {
    unsigned int i, n_elements;
    n_elements = GRN_BULK_VSIZE(space) / sizeof(grn_obj *);
    for (i = 0; i < n_elements; i++) {
      grn_obj *element = GRN_PTR_VALUE_AT(space, i);
      GRN_PTR_PUT(ctx, next_space, element);
    }
  }
  GRN_BULK_REWIND(space);
  GRN_OBJ_FIN(ctx, space);
  grn_bulk_truncate(ctx, stack, GRN_BULK_VSIZE(stack) - sizeof(grn_obj));

  if (GRN_BULK_EMPTYP(stack)) {
    space = NULL;
  } else {
    space = ((grn_obj *)GRN_BULK_CURR(stack)) - 1;
  }
  ctx->impl->temporary_open_spaces.current = space;

  GRN_API_RETURN(ctx->rc);
}
