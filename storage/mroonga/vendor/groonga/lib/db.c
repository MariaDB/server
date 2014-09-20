/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2014 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "groonga_in.h"
#include "db.h"
#include "hash.h"
#include "pat.h"
#include "dat.h"
#include "ii.h"
#include "ctx_impl.h"
#include "token.h"
#include "proc.h"
#include "plugin_in.h"
#include "geo.h"
#include "snip.h"
#include "string_in.h"
#include "normalizer_in.h"
#include "util.h"
#include <string.h>
#include <float.h>

typedef struct {
  grn_id id;
  unsigned int weight;
} weight_uvector_entry;

#define IS_WEIGHT_UVECTOR(obj) ((obj)->header.flags & GRN_OBJ_WITH_WEIGHT)

#define NEXT_ADDR(p) (((byte *)(p)) + sizeof(*(p)))

#define GRN_TABLE_GROUPED (0x01<<0)
#define GRN_TABLE_IS_GROUPED(table)\
  ((table)->header.impl_flags & GRN_TABLE_GROUPED)
#define GRN_TABLE_GROUPED_ON(table)\
  ((table)->header.impl_flags |= GRN_TABLE_GROUPED)

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
grn_table_add_subrec_inline(grn_obj *table, grn_rset_recinfo *ri, int score,
                            grn_rset_posinfo *pi, int dir);
inline static grn_id
grn_table_cursor_next_inline(grn_ctx *ctx, grn_table_cursor *tc);
inline static int
grn_table_cursor_get_value_inline(grn_ctx *ctx, grn_table_cursor *tc, void **value);

static void grn_obj_ensure_bulk(grn_ctx *ctx, grn_obj *obj);
static void grn_obj_ensure_vector(grn_ctx *ctx, grn_obj *obj);

inline static void
gen_pathname(const char *path, char *buffer, int fno)
{
  size_t len = strlen(path);
  memcpy(buffer, path, len);
  if (fno >= 0) {
    buffer[len] = '.';
    grn_itoh(fno, buffer + len + 1, 7);
    buffer[len + 8] = '\0';
  } else {
    buffer[len] = '\0';
  }
}

static grn_bool
is_text_object(grn_obj *object)
{
  if (!object) {
    return GRN_FALSE;
  }

  if (object->header.type != GRN_BULK) {
    return GRN_FALSE;
  }

  switch (object->header.domain) {
  case GRN_DB_SHORT_TEXT:
  case GRN_DB_TEXT:
  case GRN_DB_LONG_TEXT:
    return GRN_TRUE;
  default:
    return GRN_FALSE;
  }
}

static void
limited_size_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *object)
{
  unsigned int original_size = 0;
  unsigned int max_size = GRN_CTX_MSGSIZE / 2;

  if (object) {
    original_size = GRN_BULK_VSIZE(object);
  }

  if (original_size > max_size && is_text_object(object)) {
    grn_text_esc(ctx, buffer, GRN_TEXT_VALUE(object), max_size);
    GRN_TEXT_PUTS(ctx, buffer, "...(");
    grn_text_lltoa(ctx, buffer, original_size);
    GRN_TEXT_PUTS(ctx, buffer, ")");
  } else {
    grn_inspect(ctx, buffer, object);
  }
}

typedef struct {
  grn_obj *ptr;
  uint32_t lock;
  uint32_t done;
} db_value;

grn_obj *
grn_db_create(grn_ctx *ctx, const char *path, grn_db_create_optarg *optarg)
{
  grn_db *s;
  GRN_API_ENTER;
  if (!path || strlen(path) <= PATH_MAX - 14) {
    if ((s = GRN_MALLOC(sizeof(grn_db)))) {
      grn_bool use_default_db_key = GRN_TRUE;
      grn_bool use_pat_as_db_keys = GRN_FALSE;
      if (getenv("GRN_DB_KEY")) {
        if (!strcmp(getenv("GRN_DB_KEY"), "pat")) {
          use_default_db_key = GRN_FALSE;
          use_pat_as_db_keys = GRN_TRUE;
        } else if (!strcmp(getenv("GRN_DB_KEY"), "dat")) {
          use_default_db_key = GRN_FALSE;
        }
      }
      if (use_default_db_key && !strcmp(GRN_DEFAULT_DB_KEY, "pat")) {
        use_pat_as_db_keys = GRN_TRUE;
      }
      grn_tiny_array_init(ctx, &s->values, sizeof(db_value),
                          GRN_TINY_ARRAY_CLEAR|
                          GRN_TINY_ARRAY_THREADSAFE|
                          GRN_TINY_ARRAY_USE_MALLOC);
      if (use_pat_as_db_keys) {
        s->keys = (grn_obj *)grn_pat_create(ctx, path, GRN_TABLE_MAX_KEY_SIZE,
                                            0, GRN_OBJ_KEY_VAR_SIZE);
      } else {
        s->keys = (grn_obj *)grn_dat_create(ctx, path, GRN_TABLE_MAX_KEY_SIZE,
                                            0, GRN_OBJ_KEY_VAR_SIZE);
      }
      if (s->keys) {
        CRITICAL_SECTION_INIT(s->lock);
        GRN_DB_OBJ_SET_TYPE(s, GRN_DB);
        s->obj.db = (grn_obj *)s;
        s->obj.header.domain = GRN_ID_NIL;
        DB_OBJ(&s->obj)->range = GRN_ID_NIL;
        // prepare builtin classes and load builtin plugins.
        if (path) {
          char specs_path[PATH_MAX];
          gen_pathname(path, specs_path, 0);
          if ((s->specs = grn_ja_create(ctx, specs_path, 65536, 0))) {
            grn_ctx_use(ctx, (grn_obj *)s);
            grn_db_init_builtin_types(ctx);
            GRN_API_RETURN((grn_obj *)s);
          } else {
            ERR(GRN_NO_MEMORY_AVAILABLE,
                "failed to create specs: <%s>", specs_path);
          }
        } else {
          s->specs = NULL;
          grn_ctx_use(ctx, (grn_obj *)s);
          grn_db_init_builtin_types(ctx);
          GRN_API_RETURN((grn_obj *)s);
        }
        if (use_pat_as_db_keys) {
          grn_pat_close(ctx, (grn_pat *)s->keys);
          grn_pat_remove(ctx, path);
        } else {
          grn_dat_close(ctx, (grn_dat *)s->keys);
          grn_dat_remove(ctx, path);
        }
      }
      grn_tiny_array_fin(&s->values);
      GRN_FREE(s);
    } else {
      ERR(GRN_NO_MEMORY_AVAILABLE, "grn_db alloc failed");
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "too long path");
  }
  GRN_API_RETURN(NULL);
}

grn_obj *
grn_db_open(grn_ctx *ctx, const char *path)
{
  grn_db *s;
  GRN_API_ENTER;
  if (path && strlen(path) <= PATH_MAX - 14) {
    if ((s = GRN_MALLOC(sizeof(grn_db)))) {
      uint32_t type = grn_io_detect_type(ctx, path);
      grn_tiny_array_init(ctx, &s->values, sizeof(db_value),
                          GRN_TINY_ARRAY_CLEAR|
                          GRN_TINY_ARRAY_THREADSAFE|
                          GRN_TINY_ARRAY_USE_MALLOC);
      switch (type) {
      case GRN_TABLE_PAT_KEY :
        s->keys = (grn_obj *)grn_pat_open(ctx, path);
        break;
      case GRN_TABLE_DAT_KEY :
        s->keys = (grn_obj *)grn_dat_open(ctx, path);
        break;
      default :
        s->keys = NULL;
        break;
      }
      if (s->keys) {
        char specs_path[PATH_MAX];
        gen_pathname(path, specs_path, 0);
        if ((s->specs = grn_ja_open(ctx, specs_path))) {
          CRITICAL_SECTION_INIT(s->lock);
          GRN_DB_OBJ_SET_TYPE(s, GRN_DB);
          s->obj.db = (grn_obj *)s;
          s->obj.header.domain = GRN_ID_NIL;
          DB_OBJ(&s->obj)->range = GRN_ID_NIL;
          grn_ctx_use(ctx, (grn_obj *)s);
#ifdef GRN_WITH_MECAB
          if (grn_db_init_mecab_tokenizer(ctx)) {
            ERRCLR(ctx);
          }
#endif
          grn_db_init_builtin_tokenizers(ctx);
          grn_db_init_builtin_normalizers(ctx);
          grn_db_init_builtin_query(ctx);
          GRN_API_RETURN((grn_obj *)s);
        }
        switch (type) {
        case GRN_TABLE_PAT_KEY :
          grn_pat_close(ctx, (grn_pat *)s->keys);
          break;
        case GRN_TABLE_DAT_KEY :
          grn_dat_close(ctx, (grn_dat *)s->keys);
          break;
        }
      }
      grn_tiny_array_fin(&s->values);
      GRN_FREE(s);
    } else {
      ERR(GRN_NO_MEMORY_AVAILABLE, "grn_db alloc failed");
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "inappropriate path");
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
    grn_ctx_loader_clear(ctx);
    if (ctx->impl->parser) {
      grn_expr_parser_close(ctx);
    }
    if (ctx->impl->values) {
      grn_db_obj *o;
      GRN_ARRAY_EACH(ctx, ctx->impl->values, 0, 0, id, &o, {
        grn_obj_close(ctx, *((grn_obj **)o));
      });
      grn_array_truncate(ctx, ctx->impl->values);
    }
  }
  GRN_TINY_ARRAY_EACH(&s->values, 1, grn_db_curr_id(ctx, db), id, vp, {
    if (vp->ptr) { grn_obj_close(ctx, vp->ptr); }
  });
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
  grn_id id;
  grn_obj *obj = NULL;
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    return NULL;
  }
  GRN_API_ENTER;
  if (GRN_DB_P(db)) {
    grn_db *s = (grn_db *)db;
    if (name_size < 0) {
      name_size = strlen(name);
    }
    if ((id = grn_table_get(ctx, s->keys, name, name_size))) {
      obj = grn_ctx_at(ctx, id);
    }
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

static grn_io*
grn_obj_io(grn_obj *obj)
{
  grn_io *io = NULL;
  if (obj) {
    if (obj->header.type == GRN_DB) { obj = ((grn_db *)obj)->keys; }
    switch (obj->header.type) {
    case GRN_TABLE_PAT_KEY :
      io = ((grn_pat *)obj)->io;
      break;
    case GRN_TABLE_DAT_KEY :
      io = ((grn_dat *)obj)->io;
      break;
    case GRN_TABLE_HASH_KEY :
      io = ((grn_hash *)obj)->io;
      break;
    case GRN_TABLE_NO_KEY :
      io = ((grn_array *)obj)->io;
      break;
    case GRN_COLUMN_VAR_SIZE :
      io = ((grn_ja *)obj)->io;
      break;
    case GRN_COLUMN_FIX_SIZE :
      io = ((grn_ra *)obj)->io;
      break;
    case GRN_COLUMN_INDEX :
      io = ((grn_ii *)obj)->seg;
      break;
    }
  }
  return io;
}

uint32_t
grn_db_lastmod(grn_obj *s)
{
  return grn_obj_io(((grn_db *)s)->keys)->header->lastmod;
}

void
grn_db_touch(grn_ctx *ctx, grn_obj *s)
{
  grn_timeval tv;
  grn_timeval_now(ctx, &tv);
  grn_obj_io(s)->header->lastmod = tv.tv_sec;
}

#define IS_TEMP(obj) (DB_OBJ(obj)->id & GRN_OBJ_TMP_OBJECT)

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
      grn_obj_io(obj)->header->lastmod = tv->tv_sec;
      break;
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_INDEX :
      if (!IS_TEMP(obj)) {
        grn_obj_io(DB_OBJ(obj)->db)->header->lastmod = tv->tv_sec;
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

grn_obj *
grn_type_create(grn_ctx *ctx, const char *name, unsigned int name_size,
                grn_obj_flags flags, unsigned int size)
{
  grn_id id;
  struct _grn_type *res = NULL;
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[type][create]", name, name_size);
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    GRN_API_RETURN(NULL);
  }
  id = grn_obj_register(ctx, db, name, name_size);
  if (id && (res = GRN_MALLOC(sizeof(grn_db_obj)))) {
    GRN_DB_OBJ_SET_TYPE(res, GRN_TYPE);
    res->obj.header.flags = flags;
    res->obj.header.domain = GRN_ID_NIL;
    GRN_TYPE_SIZE(&res->obj) = size;
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      // grn_obj_delete(ctx, db, id);
      GRN_FREE(res);
      GRN_API_RETURN(NULL);
    }
  }
  GRN_API_RETURN((grn_obj *)res);
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
  const char *path = ctx->impl->plugin_path;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
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
        if (res->funcs[PROC_INIT] ||
            res->funcs[PROC_NEXT] ||
            res->funcs[PROC_FIN]) {
          ERR(GRN_INVALID_ARGUMENT, "already used name");
          GRN_API_RETURN(NULL);
        }
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
    res->selector = NULL;
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
calc_rec_size(grn_obj_flags flags, uint32_t max_n_subrecs, uint32_t range_size,
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
}

static void _grn_obj_remove(grn_ctx *ctx, grn_obj *obj);

static grn_rc
grn_table_create_validate(grn_ctx *ctx, const char *name, unsigned int name_size,
                          const char *path, grn_obj_flags flags,
                          grn_obj *key_type, grn_obj *value_type)
{
  switch (flags & GRN_OBJ_TABLE_TYPE_MASK) {
  case GRN_OBJ_TABLE_HASH_KEY :
    if (flags & GRN_OBJ_KEY_WITH_SIS) {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][create] "
          "key with SIS isn't available for hash table: <%.*s>",
          name_size, name);
    }
    break;
  case GRN_OBJ_TABLE_PAT_KEY :
    break;
  case GRN_OBJ_TABLE_DAT_KEY :
    break;
  case GRN_OBJ_TABLE_NO_KEY :
    if (key_type) {
      int key_name_size;
      char key_name[GRN_TABLE_MAX_KEY_SIZE];
      key_name_size = grn_obj_name(ctx, key_type, key_name,
                                   GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "[table][create] "
          "key isn't available for no key table: <%.*s> (%.*s)",
          name_size, name, key_name_size, key_name);
    } else if (flags & GRN_OBJ_KEY_WITH_SIS) {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][create] "
          "key with SIS isn't available for no key table: <%.*s>",
          name_size, name);
    } else if (flags & GRN_OBJ_KEY_NORMALIZE) {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][create] "
          "key normalization isn't available for no key table: <%.*s>",
          name_size, name);
    }
    break;
  }
  return ctx->rc;
}

static grn_obj *
grn_table_create_with_max_n_subrecs(grn_ctx *ctx, const char *name,
                                    unsigned int name_size, const char *path,
                                    grn_obj_flags flags, grn_obj *key_type,
                                    grn_obj *value_type, uint32_t max_n_subrecs)
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
    GRN_LOG(ctx, GRN_LOG_NOTICE, "DDL:table_create %.*s", name_size, name);
    if (!path) {
      if (GRN_DB_PERSISTENT_P(db)) {
        gen_pathname(grn_obj_io(db)->path, buffer, id);
        path = buffer;
      } else {
        ERR(GRN_INVALID_ARGUMENT, "path not assigned for persistent table");
        return NULL;
      }
    } else {
      flags |= GRN_OBJ_CUSTOM_NAME;
    }
  } else {
    if (path) {
      ERR(GRN_INVALID_ARGUMENT, "path assigned for temporary table");
      return NULL;
    }
    if (GRN_DB_PERSISTENT_P(db) && name && name_size) {
      ERR(GRN_INVALID_ARGUMENT, "name assigned for temporary table");
      return NULL;
    }
  }
  calc_rec_size(flags, max_n_subrecs, range_size, &subrec_size,
                &subrec_offset, &key_size, &value_size);
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
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      _grn_obj_remove(ctx, res);
      res = NULL;
    }
  } else {
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
  }
  return res;
}

grn_obj *
grn_table_create(grn_ctx *ctx, const char *name, unsigned int name_size,
                 const char *path, grn_obj_flags flags,
                 grn_obj *key_type, grn_obj *value_type)
{
  grn_obj *res;
  GRN_API_ENTER;
  res = grn_table_create_with_max_n_subrecs(ctx, name, name_size, path,
                                            flags, key_type, value_type, 0);
  GRN_API_RETURN(res);
}

grn_obj *
grn_table_create_for_group(grn_ctx *ctx, const char *name,
                           unsigned int name_size, const char *path,
                           grn_obj *group_key, grn_obj *value_type,
                           unsigned int max_n_subrecs)
{
  grn_obj *res = NULL;
  grn_obj *key_type;
  GRN_API_ENTER;
  key_type = grn_ctx_at(ctx, grn_obj_get_range(ctx, group_key));
  if (key_type) {
    res = grn_table_create_with_max_n_subrecs(ctx, name, name_size, path,
                                              GRN_TABLE_HASH_KEY|
                                              GRN_OBJ_WITH_SUBREC|
                                              GRN_OBJ_UNIT_USERDEF_DOCUMENT,
                                              key_type, value_type, max_n_subrecs);
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
      if (limit > buf_size) {
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
          scorebuf[count] = *((int *)psubrec);
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

typedef struct {
  grn_id target;
  unsigned int section;
} default_set_value_hook_data;

struct _grn_hook {
  grn_hook *next;
  grn_proc *proc;
  uint32_t hld_size;
};

static grn_obj *
default_set_value_hook(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
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
    default_set_value_hook_data *data = (void *)NEXT_ADDR(h);
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
        grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4};
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
            default_set_value_hook(ctx, 1, &table, &pctx.user_data);
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
      ERR(rc, "cast failed");
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
      ERR(rc, "cast failed");
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
          if (buf_size >= a->value_size) {
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
      grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4};
      GRN_UINT32_INIT(&id_, 0);
      GRN_UINT32_INIT(&flags_, 0);
      GRN_TEXT_INIT(&oldvalue_, GRN_OBJ_DO_SHALLOW_COPY);
      GRN_TEXT_INIT(&value_, 0);
      GRN_TEXT_SET_REF(&oldvalue_, key, key_size);
      GRN_UINT32_SET(ctx, &id_, rid);
      GRN_UINT32_SET(ctx, &flags_, GRN_OBJ_SET);
      grn_ctx_push(ctx, &id_);
      grn_ctx_push(ctx, &oldvalue_);
      grn_ctx_push(ctx, &value_);
      grn_ctx_push(ctx, &flags_);
      while (hooks) {
        pctx.caller = NULL;
        pctx.currh = hooks;
        if (hooks->proc) {
          hooks->proc->funcs[PROC_INIT](ctx, 1, &table, &pctx.user_data);
        } else {
          default_set_value_hook(ctx, 1, &table, &pctx.user_data);
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
  grn_ii_posting *posting;
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
      grn_obj_unlink(ctx, col);
      continue;
    }
    delete_reference_records_in_index(ctx, table, id, col);
    grn_obj_unlink(ctx, col);
    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  });

  grn_hash_close(ctx, cols);

  return ctx->rc;
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
      rc = delete_reference_records(ctx, table, rid);
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      call_delete_hook(ctx, table, rid, key, key_size);
      clear_column_values(ctx, table, rid);
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
      grn_obj_touch(ctx, table, NULL);
    }
  }
exit:
  GRN_API_RETURN(rc);
}

grn_rc
_grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id,
                       grn_table_delete_optarg *optarg)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  if (table) {
    const void *key;
    unsigned int key_size;
    if (id) {
      rc = delete_reference_records(ctx, table, id);
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      if ((key = _grn_table_key(ctx, table, id, &key_size))) {
        call_delete_hook(ctx, table, id, key, key_size);
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
      if (rc == GRN_SUCCESS) {
        clear_column_values(ctx, table, id);
      }
    }
  }
exit:
  return rc;
}

grn_rc
grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  grn_rc rc;
  grn_io *io;
  GRN_API_ENTER;
  if ((io = grn_obj_io(table)) && !(io->flags & GRN_IO_TEMPORARY)) {
    if (!(rc = grn_io_lock(ctx, io, grn_lock_timeout))) {
      rc = _grn_table_delete_by_id(ctx, table, id, NULL);
      grn_io_unlock(io);
    }
  } else {
    rc = _grn_table_delete_by_id(ctx, table, id, NULL);
  }
  grn_obj_touch(ctx, table, NULL);
  GRN_API_RETURN(rc);
}

grn_rc grn_ii_truncate(grn_ctx *ctx, grn_ii *ii);
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
        default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_ja_truncate(ctx, (grn_ja *)column);
      break;
    case GRN_COLUMN_FIX_SIZE :
      for (hooks = DB_OBJ(column)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
        default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_ra_truncate(ctx, (grn_ra *)column);
      break;
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
    grn_table_get_info(ctx, table, NULL, NULL, &tokenizer, &normalizer);
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_pat_truncate(ctx, (grn_pat *)table);
      break;
    case GRN_TABLE_DAT_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
        grn_obj *target = grn_ctx_at(ctx, data->target);
        if (target->header.type != GRN_COLUMN_INDEX) { continue; }
        if ((rc = grn_ii_truncate(ctx, (grn_ii *)target))) { goto exit; }
      }
      rc = grn_dat_truncate(ctx, (grn_dat *)table);
      break;
    case GRN_TABLE_HASH_KEY :
      for (hooks = DB_OBJ(table)->hooks[GRN_HOOK_INSERT]; hooks; hooks = hooks->next) {
        default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
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
    grn_obj_set_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
    grn_obj_set_info(ctx, table, GRN_INFO_NORMALIZER, normalizer);
    grn_obj_touch(ctx, table, NULL);
  }
exit :
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_get_info(grn_ctx *ctx, grn_obj *table, grn_obj_flags *flags,
                   grn_encoding *encoding, grn_obj **tokenizer,
                   grn_obj **normalizer)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_PAT_KEY :
      if (flags) { *flags = ((grn_pat *)table)->obj.header.flags; }
      if (encoding) { *encoding = ((grn_pat *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_pat *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_pat *)table)->normalizer; }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_DAT_KEY :
      if (flags) { *flags = ((grn_dat *)table)->obj.header.flags; }
      if (encoding) { *encoding = ((grn_dat *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_dat *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_dat *)table)->normalizer; }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_HASH_KEY :
      if (flags) { *flags = ((grn_hash *)table)->obj.header.flags; }
      if (encoding) { *encoding = ((grn_hash *)table)->encoding; }
      if (tokenizer) { *tokenizer = ((grn_hash *)table)->tokenizer; }
      if (normalizer) { *normalizer = ((grn_hash *)table)->normalizer; }
      rc = GRN_SUCCESS;
      break;
    case GRN_TABLE_NO_KEY :
      if (flags) { *flags = 0; }
      if (encoding) { *encoding = GRN_ENC_NONE; }
      if (tokenizer) { *tokenizer = grn_token_uvector; }
      if (normalizer) { *normalizer = NULL; }
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
      n = GRN_HASH_SIZE((grn_hash *)table);
      break;
    case GRN_TABLE_NO_KEY :
      n = GRN_ARRAY_SIZE((grn_array *)table);
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
subrecs_push(byte *subrecs, int size, int n_subrecs, int score, void *body, int dir)
{
  byte *v;
  int *c2;
  int n = n_subrecs - 1, n2;
  while (n) {
    n2 = (n - 1) >> 1;
    c2 = GRN_RSET_SUBRECS_NTH(subrecs,size,n2);
    if (GRN_RSET_SUBRECS_CMP(score, *c2, dir) >= 0) { break; }
    GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
    n = n2;
  }
  v = subrecs + n * (GRN_RSET_SCORE_SIZE + size);
  *((int *)v) = score;
  memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

inline static void
subrecs_replace_min(byte *subrecs, int size, int n_subrecs, int score, void *body, int dir)
{
  byte *v;
  int n = 0, n1, n2, *c1, *c2;
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
  memcpy(v, &score, GRN_RSET_SCORE_SIZE);
  memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

inline static void
grn_table_add_subrec_inline(grn_obj *table, grn_rset_recinfo *ri, int score,
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
          if (GRN_RSET_SUBRECS_CMP(score, *ri->subrecs, dir) > 0) {
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
grn_table_add_subrec(grn_obj *table, grn_rset_recinfo *ri, int score,
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
    } else if (offset != 0 && offset >= table_size) {
      ERR(GRN_TOO_LARGE_OFFSET,
          "offset is rather than table size: offset:%d, table_size:%d",
          offset, table_size);
    } else {
      if (limit < -1) {
        ERR(GRN_TOO_SMALL_LIMIT,
            "can't use small limit rather than -1 with GRN_CURSOR_PREFIX: %d",
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
  grn_rc rc = GRN_SUCCESS;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
    rc = GRN_INVALID_ARGUMENT;
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
      break;
    }
  }
  GRN_API_RETURN(rc);
}

inline static grn_id
grn_table_cursor_next_inline(grn_ctx *ctx, grn_table_cursor *tc)
{
  grn_id id = GRN_ID_NIL;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
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
  int len = 0;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
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
      ERR(GRN_INVALID_ARGUMENT, "invalid type %d", tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(len);
}

inline static int
grn_table_cursor_get_value_inline(grn_ctx *ctx, grn_table_cursor *tc, void **value)
{
  int len = 0;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
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
      ERR(GRN_INVALID_ARGUMENT, "invalid type %d", tc->header.type);
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
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
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
      ERR(GRN_INVALID_ARGUMENT, "invalid type %d", tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_table_cursor_delete(grn_ctx *ctx, grn_table_cursor *tc)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
  } else {
    switch (tc->header.type) {
    case GRN_CURSOR_TABLE_PAT_KEY :
      rc = grn_pat_cursor_delete(ctx, (grn_pat_cursor *)tc, NULL);
      break;
    case GRN_CURSOR_TABLE_DAT_KEY :
      rc = GRN_OPERATION_NOT_SUPPORTED;
      break;
    case GRN_CURSOR_TABLE_HASH_KEY :
      rc = grn_hash_cursor_delete(ctx, (grn_hash_cursor *)tc, NULL);
      break;
    case GRN_CURSOR_TABLE_NO_KEY :
      rc = grn_array_cursor_delete(ctx, (grn_array_cursor *)tc, NULL);
      break;
    default :
      ERR(GRN_INVALID_ARGUMENT, "invalid type %d", tc->header.type);
      break;
    }
  }
  GRN_API_RETURN(rc);
}

grn_obj *
grn_table_cursor_table(grn_ctx *ctx, grn_table_cursor *tc)
{
  grn_obj *obj = NULL;
  GRN_API_ENTER;
  if (!tc) {
    ERR(GRN_INVALID_ARGUMENT, "tc is null");
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
      ERR(GRN_INVALID_ARGUMENT, "invalid type %d", tc->header.type);
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
  grn_ii_posting *ip = NULL;
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

grn_rc
grn_accessor_resolve(grn_ctx *ctx, grn_obj *accessor, int deep,
                     grn_obj *base_res, grn_obj **res,
                     grn_search_optarg *optarg)
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
    grn_obj *index;
    grn_operator index_op = GRN_OP_MATCH;

    a = (grn_accessor *)GRN_PTR_VALUE_AT(&accessor_stack, i - 1);
    if (grn_column_index(ctx, a->obj, index_op, &index, 1, NULL) == 0) {
      rc = GRN_INVALID_ARGUMENT;
      break;
    }

    {
      grn_id *tid;
      grn_obj *domain;
      grn_obj *next_res;
      grn_search_optarg next_optarg;
      grn_rset_recinfo *recinfo;
      if (optarg) {
        next_optarg = *optarg;
        next_optarg.mode = GRN_OP_EXACT;
      } else {
        memset(&next_optarg, 0, sizeof(grn_search_optarg));
      }
      {
        grn_obj *range = grn_ctx_at(ctx, DB_OBJ(index)->range);
        next_res = grn_table_create(ctx, NULL, 0, NULL,
                                    GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                    range, NULL);
        rc = ctx->rc;
        grn_obj_unlink(ctx, range);
        if (!next_res) {
          if (current_res != base_res) {
            grn_obj_unlink(ctx, current_res);
          }
          break;
        }
      }
      domain = grn_ctx_at(ctx, index->header.domain);
      GRN_HASH_EACH(ctx, (grn_hash *)current_res, id, &tid, NULL, &recinfo, {
        next_optarg.weight_vector = NULL;
        next_optarg.vector_size = recinfo->score;
        if (domain->header.type == GRN_TABLE_NO_KEY) {
          rc = grn_ii_sel(ctx, (grn_ii *)index,
                          (const char *)tid, sizeof(grn_id),
                          (grn_hash *)next_res, GRN_OP_OR,
                          &next_optarg);
        } else {
          char key[GRN_TABLE_MAX_KEY_SIZE];
          int key_len;
          key_len = grn_table_get_key(ctx, domain, *tid,
                                      key, GRN_TABLE_MAX_KEY_SIZE);
          rc = grn_ii_sel(ctx, (grn_ii *)index, key, key_len,
                          (grn_hash *)next_res, GRN_OP_OR,
                          &next_optarg);
        }
        if (rc != GRN_SUCCESS) {
          break;
        }
      });
      grn_obj_unlink(ctx, domain);
      if (current_res != base_res) {
        grn_obj_unlink(ctx, current_res);
      }
      if (rc != GRN_SUCCESS) {
        grn_obj_unlink(ctx, next_res);
        break;
      }
      current_res = next_res;
    }
  }

  if (rc == GRN_SUCCESS && current_res != base_res) {
    *res = current_res;
  } else {
    *res = NULL;
    if (rc == GRN_SUCCESS) {
      rc = GRN_INVALID_ARGUMENT;
    }
  }

  GRN_OBJ_FIN(ctx, &accessor_stack);
  return rc;
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
      grn_obj *resolve_res = NULL;
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
      rc = grn_obj_search(ctx, index, query, base_res, GRN_OP_OR, optarg);
      if (rc != GRN_SUCCESS) {
        grn_obj_unlink(ctx, base_res);
        goto exit;
      }
      rc = grn_accessor_resolve(ctx, obj, n_accessors - 1, base_res,
                                &resolve_res, optarg);
      if (resolve_res) {
        grn_id *record_id;
        grn_rset_recinfo *recinfo;
        GRN_HASH_EACH(ctx, (grn_hash *)resolve_res, id, &record_id, NULL,
                      &recinfo, {
          grn_ii_posting posting;
          posting.rid = *record_id;
          posting.sid = 1;
          posting.pos = 0;
          posting.weight = recinfo->score - 1;
          grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
        });
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
        grn_obj_unlink(ctx, resolve_res);
      }
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
  grn_ii_cursor *c = grn_ii_cursor_open(ctx, (grn_ii *)obj, tid,
                                        GRN_ID_NIL, GRN_ID_MAX, 1, 0);
  if (c) {
    grn_ii_posting *pos;
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
          rc = grn_table_search(ctx, obj, key, key_size, mode, res, op);
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

static int
accelerated_table_group(grn_ctx *ctx, grn_obj *table, grn_obj *key, grn_obj *res)
{
  if (key->header.type == GRN_ACCESSOR) {
    grn_accessor *a = (grn_accessor *)key;
    if (a->action == GRN_ACCESSOR_GET_KEY &&
        a->next && a->next->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        a->next->obj && !a->next->next) {
      grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, key));
      int idp = GRN_OBJ_TABLEP(range);
      grn_table_cursor *tc;
      if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
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
                grn_table_add_subrec_inline(res, value, ri ? ri->score : 0,
                                            (grn_rset_posinfo *)&id, 0);
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
        return 1;
      }
    }
  }
  return 0;
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

grn_rc
grn_table_group(grn_ctx *ctx, grn_obj *table,
                grn_table_sort_key *keys, int n_keys,
                grn_table_group_result *results, int n_results)
{
  grn_rc rc = GRN_SUCCESS;
  if (!table || !n_keys || !n_results) {
    ERR(GRN_INVALID_ARGUMENT, "table or n_keys or n_results is void");
    return GRN_INVALID_ARGUMENT;
  }
  GRN_API_ENTER;
  {
    int k, r;
    void *key;
    grn_obj bulk;
    grn_table_cursor *tc;
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
        ERR(GRN_INVALID_ARGUMENT, "table missing in (%d)", r);
        goto exit;
      }
    }
    GRN_TEXT_INIT(&bulk, 0);
    if (n_keys == 1 && n_results == 1) {
      if (!accelerated_table_group(ctx, table, keys->key, results->table)) {
        if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
          grn_id id;
          grn_obj *range = grn_ctx_at(ctx, grn_obj_get_range(ctx, keys->key));
          int idp = GRN_OBJ_TABLEP(range);
          while ((id = grn_table_cursor_next_inline(ctx, tc))) {
            void *value;
            grn_rset_recinfo *ri = NULL;
            GRN_BULK_REWIND(&bulk);
            if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
              grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
            }
            grn_obj_get_value(ctx, keys->key, id, &bulk);
            switch (bulk.header.type) {
            case GRN_UVECTOR :
              {
                // todo : support objects except grn_id
                grn_id *v = (grn_id *)GRN_BULK_HEAD(&bulk);
                grn_id *ve = (grn_id *)GRN_BULK_CURR(&bulk);
                while (v < ve) {
                  if ((*v != GRN_ID_NIL) &&
                      grn_table_add_v_inline(ctx, results->table, v, sizeof(grn_id), &value, NULL)) {
                    grn_table_add_subrec_inline(results->table, value, ri ? ri->score : 0,
                                                (grn_rset_posinfo *)&id, 0);
                  }
                  v++;
                }
              }
              break;
            case GRN_VECTOR :
              ERR(GRN_OPERATION_NOT_SUPPORTED, "sorry.. not implemented yet");
              /* todo */
              break;
            case GRN_BULK :
              {
                if ((!idp || *((grn_id *)GRN_BULK_HEAD(&bulk))) &&
                    grn_table_add_v_inline(ctx, results->table,
                                           GRN_BULK_HEAD(&bulk), GRN_BULK_VSIZE(&bulk), &value, NULL)) {
                  grn_table_add_subrec_inline(results->table, value, ri ? ri->score : 0,
                                              (grn_rset_posinfo *)&id, 0);
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
      }
    } else {
      if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
        grn_id id;
        while ((id = grn_table_cursor_next_inline(ctx, tc))) {
          grn_rset_recinfo *ri = NULL;
          GRN_BULK_REWIND(&bulk);
          if (DB_OBJ(table)->header.flags & GRN_OBJ_WITH_SUBREC) {
            grn_table_cursor_get_value_inline(ctx, tc, (void **)&ri);
          }
          for (k = 0, kp = keys; k < n_keys; k++, kp++) {
            kp->offset = GRN_BULK_VSIZE(&bulk);
            grn_obj_get_value(ctx, kp->key, id, &bulk);
          }
          for (r = 0, rp = results; r < n_results; r++, rp++) {
            void *value;
            int begin = keys[rp->key_begin].offset;
            int end = rp->key_end >= n_keys
              ? GRN_BULK_VSIZE(&bulk)
              : keys[rp->key_end].offset;
            key = GRN_BULK_HEAD(&bulk) + begin;
            // todo : cut off GRN_ID_NIL
            if (grn_table_add_v_inline(ctx, rp->table, key, end - begin, &value, NULL)) {
              grn_table_add_subrec_inline(rp->table, value, ri ? ri->score : 0,
                                          (grn_rset_posinfo *)&id, 0);
            }
          }
        }
        grn_table_cursor_close(ctx, tc);
      }
    }
    grn_obj_close(ctx, &bulk);
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
  grn_rc rc = GRN_SUCCESS;
  void *key = NULL, *value1 = NULL, *value2 = NULL;
  uint32_t value_size = 0;
  uint32_t key_size = 0;
  grn_bool have_subrec;
  if (table1 != res) {
    if (table2 == res) {
      grn_obj *t = table1;
      table1 = table2;
      table2 = t;
    } else {
      return GRN_INVALID_ARGUMENT;
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
            memcpy(value1, value2, value_size);
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
          memcpy(value1, value2, value_size);
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
    GRN_TABLE_EACH(ctx, table2, 0, 0, id, &key, &key_size, &value2, {
      if (grn_table_get_v(ctx, table1, key, key_size, &value1)) {
        memcpy(value1, value2, value_size);
      }
    });
    break;
  default :
    break;
  }
  return rc;
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
  grn_obj *column = NULL;
  char buf[GRN_TABLE_MAX_KEY_SIZE];
  int len = grn_obj_name(ctx, table, buf, GRN_TABLE_MAX_KEY_SIZE);
  if (len) {
    buf[len++] = GRN_DB_DELIMITER;
    if (len + name_size <= GRN_TABLE_MAX_KEY_SIZE) {
      memcpy(buf + len, name, name_size);
      column = grn_ctx_get(ctx, buf, len + name_size);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "name is too long");
    }
  } else {
    /* todo : support temporary table */
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
  GRN_API_ENTER;
  if (GRN_OBJ_TABLEP(table) && DB_OBJ(table)->id &&
      !(DB_OBJ(table)->id & GRN_OBJ_TMP_OBJECT)) {
    grn_db *s = (grn_db *)DB_OBJ(table)->db;
    if (s->keys) {
      grn_obj bulk;
      GRN_TEXT_INIT(&bulk, 0);
      grn_table_get_key2(ctx, s->keys, DB_OBJ(table)->id, &bulk);
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
                  const char *path, grn_obj_flags flags, grn_obj *type)
{
  grn_db *s;
  uint32_t value_size;
  grn_obj *db, *res = NULL;
  grn_id id = GRN_ID_NIL;
  grn_id range = GRN_ID_NIL;
  grn_id domain = GRN_ID_NIL;
  char fullname[GRN_TABLE_MAX_KEY_SIZE];
  char buffer[PATH_MAX];
  grn_bool ja_p = GRN_FALSE;
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
  if (DB_OBJ(table)->id & GRN_OBJ_TMP_OBJECT) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] temporary table doesn't support column: <%.*s>",
        name_size, name);
    goto exit;
  }
  {
    uint32_t s = 0;
    const char *n = _grn_table_key(ctx, ctx->impl->db, DB_OBJ(table)->id, &s);
    GRN_LOG(ctx, GRN_LOG_NOTICE,
            "DDL:column_create %.*s %.*s", s, n, name_size, name);
  }
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[column][create]", name, name_size);
    goto exit;
  }
  if ((domain = DB_OBJ(table)->id)) {
    int len = grn_table_get_key(ctx, s->keys, domain, fullname, GRN_TABLE_MAX_KEY_SIZE);
    if (name_size + 1 + len > GRN_TABLE_MAX_KEY_SIZE) {
      ERR(GRN_INVALID_ARGUMENT,
          "[column][create] too long column name: required name_size(%d) < %d"
          ": <%.*s>.<%.*s>",
          name_size, GRN_TABLE_MAX_KEY_SIZE - 1 - len,
          len, fullname, name_size, name);
      goto exit;
    }
    fullname[len] = GRN_DB_DELIMITER;
    memcpy(fullname + len + 1, name, name_size);
    name_size += len + 1;
  } else {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "[column][create] [todo] table-less column isn't supported yet");
    goto exit;
  }
  range = DB_OBJ(type)->id;
  switch (type->header.type) {
  case GRN_TYPE :
    {
      grn_db_obj *t = (grn_db_obj *)type;
      flags |= t->header.flags;
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
  id = grn_obj_register(ctx, db, fullname, name_size);
  if (ERRP(ctx, GRN_ERROR)) { goto exit;  }
  if (GRN_OBJ_PERSISTENT & flags) {
    if (!path) {
      if (GRN_DB_PERSISTENT_P(db)) {
        gen_pathname(grn_obj_io(db)->path, buffer, id);
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
      ja_p = GRN_TRUE;
    } else {
      res = (grn_obj *)grn_ra_create(ctx, path, value_size);
    }
    break;
  case GRN_OBJ_COLUMN_VECTOR :
    res = (grn_obj *)grn_ja_create(ctx, path, value_size * 30/*todo*/, flags);
    ja_p = GRN_TRUE;
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
    if (ja_p) {
      grn_bool zlib_p = GRN_FALSE;
      grn_bool lzo_p = GRN_FALSE;
#ifdef GRN_WITH_ZLIB
      if (flags & GRN_OBJ_COMPRESS_ZLIB) {
        zlib_p = GRN_TRUE;
      }
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZO
      if (flags & GRN_OBJ_COMPRESS_LZO) {
        lzo_p = GRN_TRUE;
      }
#endif /* GRN_WITH_LZO */
      if (zlib_p || lzo_p) {
        int table_name_len;
        char table_name[GRN_TABLE_MAX_KEY_SIZE];
        table_name_len = grn_obj_name(ctx, table, table_name,
                                      GRN_TABLE_MAX_KEY_SIZE);
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "[column][create] "
                "%s compressed column will leaks memories: <%.*s>.<%.*s>",
                zlib_p ? "zlib" : "lzo",
                table_name_len, table_name, name_size, name);
      }
    }
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      _grn_obj_remove(ctx, res);
      res = NULL;
    }
    grn_obj_touch(ctx, res, NULL);
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
    memcpy(fullname + len + 1, name, name_size);
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
        memcpy(v, in->u.p.ptr, value_size);
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
grn_uvector_size_internal(grn_ctx *ctx, grn_obj *uvector)
{
  unsigned int size;

  if (IS_WEIGHT_UVECTOR(uvector)) {
    size = GRN_BULK_VSIZE(uvector) / sizeof(weight_uvector_entry);
  } else {
    size = GRN_BULK_VSIZE(uvector) / sizeof(grn_id);
  }

  return size;
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
  if (vector->u.v.n_sections <= offset) {
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
    uint32_t o = 0, l, i;
    for (i = n, vp = v->u.v.sections + n0; i; i--, vp++) {
      if (pe <= p) { return GRN_INVALID_ARGUMENT; }
      GRN_B_DEC(l, p);
      vp->length = l;
      vp->offset = o;
      vp->weight = 0;
      vp->domain = 0;
      o += l;
    }
    if (pe < p + o) { return GRN_INVALID_ARGUMENT; }
    {
      grn_obj *body = grn_vector_body(ctx, v);
      grn_bulk_write(ctx, body, (char *)p, o);
    }
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
    res->action = GRN_ACCESSOR_VOID;
    res->offset = 0;
    res->next = NULL;
  }
  return res;
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
      case 's' : /* score */
        if (len != GRN_COLUMN_NAME_SCORE_LEN ||
            memcmp(name, GRN_COLUMN_NAME_SCORE, GRN_COLUMN_NAME_SCORE_LEN)) {
          goto exit;
        }
        for (rp = &res; !done; rp = &(*rp)->next) {
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (DB_OBJ(obj)->header.flags & GRN_OBJ_WITH_SUBREC) {
            (*rp)->action = GRN_ACCESSOR_GET_SCORE;
            done++;
          } else {
            switch (obj->header.type) {
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
            if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
          }
        }
        break;
      case 'n' : /* nsubrecs */
        if (len != GRN_COLUMN_NAME_NSUBRECS_LEN ||
            memcmp(name,
                   GRN_COLUMN_NAME_NSUBRECS,
                   GRN_COLUMN_NAME_NSUBRECS_LEN)) {
          goto exit;
        }
        for (rp = &res; !done; rp = &(*rp)->next) {
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (GRN_TABLE_IS_GROUPED(obj)) {
            (*rp)->action = GRN_ACCESSOR_GET_NSUBRECS;
            done++;
          } else {
            switch (obj->header.type) {
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
            if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
              grn_obj_close(ctx, (grn_obj *)res);
              res = NULL;
              goto exit;
            }
          }
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
          if (!obj->header.domain) {
            // ERR(GRN_INVALID_ARGUMENT, "no such column: <%s>", name);
            if (!is_chained) {
              grn_obj_close(ctx, (grn_obj *)res);
            }
            res = NULL;
            goto exit;
          }
          *rp = accessor_new(ctx);
          (*rp)->obj = obj;
          if (!(obj = grn_ctx_at(ctx, obj->header.domain))) {
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

inline static void
grn_obj_get_range_info(grn_ctx *ctx, grn_obj *obj,
                       grn_id *range_id, grn_obj_flags *range_flags)
{
  if (GRN_DB_OBJP(obj)) {
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
      case GRN_ACCESSOR_GET_NSUBRECS :
        *range_id = GRN_DB_INT32;
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        if (GRN_DB_OBJP(a->obj)) {
          *range_id = DB_OBJ(a->obj)->range;
          if (grn_column_is_vector(ctx, a->obj)) {
            *range_flags = GRN_OBJ_VECTOR;
          }
        }
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
        grn_obj_cast(ctx, src, &key, GRN_TRUE);\
        p_key = &key;\
      }\
      if (GRN_BULK_VSIZE(p_key)) {\
        id = addp ? grn_table_add_by_key(ctx, table, p_key, NULL)\
                  : grn_table_get_by_key(ctx, table, p_key);\
        if (id) { GRN_RECORD_SET(ctx, dest, id); }\
      } else {\
        GRN_RECORD_SET(ctx, dest, GRN_ID_NIL);\
      }\
      GRN_OBJ_FIN(ctx, &key);\
    } else {\
      grn_obj record_id;\
      GRN_UINT32_INIT(&record_id, 0);\
      grn_obj_cast(ctx, src, &record_id, GRN_TRUE);\
      id = GRN_UINT32_VALUE(&record_id);\
      if (id) { GRN_RECORD_SET(ctx, dest, id); }\
    }\
  } else {\
    rc = GRN_FUNCTION_NOT_IMPLEMENTED;\
  }\
} while (0)

inline static grn_rc
grn_obj_cast_bool(grn_ctx *ctx, grn_obj *src, grn_obj *dest, grn_bool addp)
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
      rc = grn_obj_cast(ctx, &buf, dest, addp);\
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
#define FLOAT2TIME(ctx, dest, value)\
  GRN_TIME_SET(ctx, dest, (long long int)(value * GRN_TIME_USEC_PER_SEC));

#define NUM2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, value);
#define TIME2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, (double)(value) / GRN_TIME_USEC_PER_SEC);
#define FLOAT2FLOAT(ctx, dest, value)\
  GRN_FLOAT_SET(ctx, dest, value);

grn_rc
grn_obj_cast(grn_ctx *ctx, grn_obj *src, grn_obj *dest, grn_bool addp)
{
  grn_rc rc = GRN_SUCCESS;
  switch (src->header.domain) {
  case GRN_DB_BOOL :
    rc = grn_obj_cast_bool(ctx, src, dest, addp);
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
    rc = GRN_FUNCTION_NOT_IMPLEMENTED;
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
        *size = sizeof(int);
      }
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      if ((value = grn_obj_get_value_(ctx, a->obj, id, size))) {
        value = (const char *)&((grn_rset_recinfo *)value)->n_subrecs;
        *size = sizeof(int);
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
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_KEY :
      grn_table_get_key2(ctx, a->obj, id, value);
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_VALUE :
      grn_obj_get_value(ctx, a->obj, id, value);
      vp = GRN_BULK_HEAD(value) + size0;
      vs = GRN_BULK_VSIZE(value) - size0;
      break;
    case GRN_ACCESSOR_GET_SCORE :
      grn_obj_get_value(ctx, a->obj, id, value);
      {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        GRN_INT32_PUT(ctx, value, ri->score);
      }
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      {
        grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
        GRN_INT32_PUT(ctx, value, ri->n_subrecs);
      }
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
      id = *((grn_id *)vp);
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
              vp = &ri->score;
              // todo : flags support
              if (value->header.domain == GRN_DB_INT32) {
                memcpy(vp, GRN_BULK_HEAD(value), sizeof(int));
              } else {
                grn_obj buf;
                GRN_INT32_INIT(&buf, 0);
                grn_obj_cast(ctx, value, &buf, GRN_FALSE);
                memcpy(vp, GRN_BULK_HEAD(&buf), sizeof(int));
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
      grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4};
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
          default_set_value_hook(ctx, 1, &obj, &pctx.user_data);
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

inline static int
call_hook_for_build(grn_ctx *ctx, grn_obj *obj, grn_id id, grn_obj *value, int flags)
{
  grn_hook *hooks = DB_OBJ(obj)->hooks[GRN_HOOK_SET];

  if (hooks || obj->header.type == GRN_COLUMN_VAR_SIZE) {
    grn_obj oldvalue;
    GRN_TEXT_INIT(&oldvalue, 0);

    if (hooks) {
      // todo : grn_proc_ctx_open()
      grn_obj id_, flags_;
      grn_proc_ctx pctx = {{0}, hooks->proc, NULL, hooks, hooks, PROC_INIT, 4, 4};
      GRN_UINT32_INIT(&id_, 0);
      GRN_UINT32_INIT(&flags_, 0);
      GRN_UINT32_SET(ctx, &id_, id);
      GRN_UINT32_SET(ctx, &flags_, flags);
      while (hooks) {
        grn_ctx_push(ctx, &id_);
        grn_ctx_push(ctx, &oldvalue);
        grn_ctx_push(ctx, value);
        grn_ctx_push(ctx, &flags_);
        pctx.caller = NULL;
        pctx.currh = hooks;
        if (hooks->proc) {
          hooks->proc->funcs[PROC_INIT](ctx, 1, &obj, &pctx.user_data);
        } else {
          default_set_value_hook(ctx, 1, &obj, &pctx.user_data);
        }
        if (ctx->rc) {
          grn_obj_close(ctx, &oldvalue);
          return 1;
        }
        hooks = hooks->next;
        pctx.offset++;
      }
    }
    grn_obj_close(ctx, &oldvalue);
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
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_obj uvector;
  grn_obj_flags uvector_flags = 0;
  grn_bool need_convert = GRN_FALSE;
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

  if (need_convert) {
    unsigned int i, n;
    GRN_VALUE_FIX_SIZE_INIT(&uvector, GRN_OBJ_VECTOR, value->header.domain);
    uvector.header.flags |= uvector_flags;
    n = grn_uvector_size(ctx, value);
    for (i = 0; i < n; i++) {
      grn_id id;
      unsigned int weight = 0;
      id = grn_uvector_get_element(ctx, value, i, NULL);
      grn_uvector_add_element(ctx, &uvector, id, weight);
    }
    raw_value = GRN_BULK_HEAD(&uvector);
    size = GRN_BULK_VSIZE(&uvector);
  } else {
    raw_value = GRN_BULK_HEAD(value);
    size = GRN_BULK_VSIZE(value);
  }

  rc = grn_ja_put(ctx, (grn_ja *)column, id, raw_value, size, flags, NULL);

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
        grn_token *token;
        if (v && s &&
            (token = grn_token_open(ctx, lexicon, v, s,
                                    GRN_TOKEN_ADD, token_flags))) {
          while (!token->status) {
            grn_id tid = grn_token_next(ctx, token);
            grn_uvector_add_element(ctx, &uvector, tid, 0);
          }
          grn_token_close(ctx, token);
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
            memcpy(b, v, s);
            memcpy(p, b, element_size);
            GRN_FREE(b);
          }
        }
      } else {
        memcpy(p, v, s);
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
grn_obj_get_value_column_index(grn_ctx *ctx, grn_obj *index_column,
                               grn_id id, grn_obj *value)
{
  grn_ii *ii = (grn_ii *)index_column;
  grn_obj_ensure_bulk(ctx, value);
  GRN_UINT32_SET(ctx, value, grn_ii_estimate_size(ctx, ii, id));
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
    grn_obj v_;
    grn_obj_ensure_vector(ctx, value);
    GRN_TEXT_INIT(&v_, 0);
    grn_ja_get_value(ctx, (grn_ja *)obj, id, &v_);
    grn_vector_decode(ctx, value, GRN_TEXT_VALUE(&v_), GRN_TEXT_LEN(&v_));
    GRN_OBJ_FIN(ctx, &v_);
  } else {
    grn_obj_ensure_bulk(ctx, value);
    grn_ja_get_value(ctx, (grn_ja *)obj, id, value);
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
  if (!id) { goto exit; }
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
    GRN_TEXT_INIT(value, 0);
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
    value->header.domain = grn_obj_get_range(ctx, obj);
    break;
  case GRN_TABLE_PAT_KEY :
    {
      grn_pat *pat = (grn_pat *)obj;
      uint32_t size = pat->value_size;
      grn_obj_ensure_bulk(ctx, value);
      if (grn_bulk_space(ctx, value, size)) {
        MERR("grn_bulk_space failed");
        goto exit;
      }
      {
        char *curr = GRN_BULK_CURR(value);
        grn_pat_get_value(ctx, pat, id, curr - size);
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
      grn_hash *hash = (grn_hash *)obj;
      uint32_t size = hash->value_size;
      grn_obj_ensure_bulk(ctx, value);
      if (grn_bulk_space(ctx, value, size)) {
        MERR("grn_bulk_space failed");
        goto exit;
      }
      {
        char *curr = GRN_BULK_CURR(value);
        grn_hash_get_value(ctx, hash, id, curr - size);
      }
      value->header.type = GRN_BULK;
      value->header.domain = grn_obj_get_range(ctx, obj);
    }
    break;
  case GRN_TABLE_NO_KEY :
    {
      grn_array *array = (grn_array *)obj;
      uint32_t size = array->value_size;
      grn_obj_ensure_bulk(ctx, value);
      if (grn_bulk_space(ctx, value, size)) {
        MERR("grn_bulk_space failed");
        goto exit;
      }
      {
        char *curr = GRN_BULK_CURR(value);
        grn_array_get_value(ctx, array, id, curr - size);
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
      grn_ja_get_value(ctx, (grn_ja *)obj, id, value);
      value->header.type = GRN_BULK;
      break;
    default :
      ERR(GRN_FILE_CORRUPT, "invalid GRN_OBJ_COLUMN_TYPE");
      break;
    }
    value->header.domain = grn_obj_get_range(ctx, obj);
    break;
  case GRN_COLUMN_FIX_SIZE :
    {
      unsigned int element_size;
      void *v = grn_ra_ref(ctx, (grn_ra *)obj, id);
      grn_obj_ensure_bulk(ctx, value);
      value->header.type = GRN_BULK;
      value->header.domain = grn_obj_get_range(ctx, obj);
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
      if (0 < offset && offset <= table_size) {
        grn_ra *ra = (grn_ra *)obj;
        void *p = grn_ra_ref(ctx, ra, offset);
        if (p) {
          if ((offset >> ra->element_width) == (table_size >> ra->element_width)) {
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
  case GRN_INFO_SUPPORT_LZO :
    if (!valuebuf && !(valuebuf = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_BOOL))) {
      ERR(GRN_INVALID_ARGUMENT,
          "failed to open value buffer for GRN_INFO_LZO_SUPPORT");
      goto exit;
    }
#ifdef GRN_WITH_LZO
    GRN_BOOL_PUT(ctx, valuebuf, GRN_TRUE);
#else /* GRN_WITH_LZO */
    GRN_BOOL_PUT(ctx, valuebuf, GRN_FALSE);
#endif /* GRN_WITH_LZO */
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
    default :
      /* todo */
      break;
    }
  }
exit :
  GRN_API_RETURN(valuebuf);
}

static void
build_index(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj *src, **cp, **col, *target;
  grn_id *s = DB_OBJ(obj)->source;
  if (!(DB_OBJ(obj)->source_size) || !s) { return; }
  if ((src = grn_ctx_at(ctx, *s))) {
    target = GRN_OBJ_TABLEP(src) ? src : grn_ctx_at(ctx, src->header.domain);
    if (target) {
      int i, ncol = DB_OBJ(obj)->source_size / sizeof(grn_id);
      grn_obj_flags flags;
      grn_ii *ii = (grn_ii *)obj;
      grn_bool use_grn_ii_build;
      grn_table_get_info(ctx, ii->lexicon, &flags, NULL, NULL, NULL);
      switch (flags & GRN_OBJ_TABLE_TYPE_MASK) {
      case GRN_OBJ_TABLE_PAT_KEY :
      case GRN_OBJ_TABLE_DAT_KEY :
        use_grn_ii_build = GRN_TRUE;
        break;
      default :
        use_grn_ii_build = GRN_FALSE;
      }
      if ((ii->header->flags & GRN_OBJ_WITH_WEIGHT)) {
        use_grn_ii_build = GRN_FALSE;
      }
      if ((col = GRN_MALLOC(ncol * sizeof(grn_obj *)))) {
        for (cp = col, i = ncol; i; s++, cp++, i--) {
          if (!(*cp = grn_ctx_at(ctx, *s))) {
            ERR(GRN_INVALID_ARGUMENT, "source invalid, n=%d",i);
            GRN_FREE(col);
            return;
          }
          if (GRN_OBJ_TABLEP(grn_ctx_at(ctx, DB_OBJ(*cp)->range))) {
            use_grn_ii_build = GRN_FALSE;
          }
        }
        if (use_grn_ii_build) {
          uint64_t sparsity = 10;
          if (getenv("GRN_INDEX_SPARSITY")) {
            uint64_t v;
            errno = 0;
            v = strtoull(getenv("GRN_INDEX_SPARSITY"), NULL, 0);
            if (!errno) { sparsity = v; }
          }
          grn_ii_build(ctx, ii, sparsity);
        } else {
          grn_table_cursor  *tc;
          if ((tc = grn_table_cursor_open(ctx, target, NULL, 0, NULL, 0,
                                          0, -1, GRN_CURSOR_BY_ID))) {
            grn_id id;
            grn_obj rv;
            GRN_TEXT_INIT(&rv, 0);
            while ((id = grn_table_cursor_next_inline(ctx, tc)) != GRN_ID_NIL) {
              for (cp = col, i = ncol; i; i--, cp++) {
                GRN_BULK_REWIND(&rv);
                if (GRN_OBJ_TABLEP(*cp)) {
                  grn_table_get_key2(ctx, *cp, id, &rv);
                } else {
                  grn_obj_get_value(ctx, *cp, id, &rv);
                }
                call_hook_for_build(ctx, *cp, id, &rv, 0);
              }
            }
            GRN_OBJ_FIN(ctx, &rv);
            grn_table_cursor_close(ctx, tc);
          }
        }
        GRN_FREE(col);
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT, "invalid target");
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid source");
  }
}

static void
update_source_hook(grn_ctx *ctx, grn_obj *obj)
{
  grn_id *s = DB_OBJ(obj)->source;
  int i, n = DB_OBJ(obj)->source_size / sizeof(grn_id);
  default_set_value_hook_data hook_data = { DB_OBJ(obj)->id, 0 };
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
    if (!memcmp(NEXT_ADDR(*last), hld_value, hld_size)) {
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
  default_set_value_hook_data hook_data = { DB_OBJ(obj)->id, 0 };
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
      if ((rc = grn_bulk_write(ctx, buf, (char *)NEXT_ADDR(hooks), hooks->hld_size))) { goto exit; }
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
        memcpy(NEXT_ADDR(new), p, hld_size);
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

void
grn_obj_spec_save(grn_ctx *ctx, grn_db_obj *obj)
{
  grn_db *s;
  grn_obj v, *b;
  grn_obj_spec spec;
  if (obj->id & GRN_OBJ_TMP_OBJECT) { return; }
  if (!ctx->impl || !GRN_DB_OBJP(obj)) { return; }
  if (!(s = (grn_db *)ctx->impl->db) || !s->specs) { return; }
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
  case GRN_EXPR :
    grn_expr_pack(ctx, b, (grn_obj *)obj);
    grn_vector_delimit(ctx, &v, 0, 0);
    break;
  }
  grn_ja_putv(ctx, s->specs, obj->id, &v, 0);
  grn_obj_close(ctx, &v);
}

inline static grn_rc
grn_obj_set_info_source_validate_report_error(grn_ctx *ctx,
                                              grn_obj *column,
                                              grn_obj *table_domain,
                                              grn_obj *source,
                                              grn_id source_type_id)
{
  char column_name[GRN_TABLE_MAX_KEY_SIZE];
  char table_domain_name[GRN_TABLE_MAX_KEY_SIZE];
  char source_name[GRN_TABLE_MAX_KEY_SIZE];
  char source_type_name[GRN_TABLE_MAX_KEY_SIZE];
  int column_name_size;
  int table_domain_name_size;
  int source_name_size;
  int source_type_name_size;
  grn_obj *source_type;

  column_name_size = grn_obj_name(ctx, column,
                                  column_name, GRN_TABLE_MAX_KEY_SIZE);
  source_name_size = grn_obj_name(ctx, source,
                                  source_name, GRN_TABLE_MAX_KEY_SIZE);
  if (GRN_OBJ_TABLEP(source)) {
    source_name[source_name_size] = '\0';
    strncat(source_name, "._key",
            GRN_TABLE_MAX_KEY_SIZE - source_name_size - 1);
    source_name_size = strlen(source_name);
  }
  table_domain_name_size = grn_obj_name(ctx, table_domain,
                                        table_domain_name,
                                        GRN_TABLE_MAX_KEY_SIZE);
  source_type = grn_ctx_at(ctx, source_type_id);
  if (source_type) {
    source_type_name_size = grn_obj_name(ctx, source_type,
                                         source_type_name,
                                         GRN_TABLE_MAX_KEY_SIZE);
    grn_obj_unlink(ctx, source_type);
  } else {
    strncpy(source_type_name, "(nil)", GRN_TABLE_MAX_KEY_SIZE);
    source_type_name_size = strlen(source_type_name);
  }
  ERR(GRN_INVALID_ARGUMENT,
      "grn_obj_set_info(): GRN_INFO_SOURCE: "
      "source type must equal to index table's key type: "
      "source:<%.*s(%.*s)> index:<%.*s(%.*s)>",
      source_name_size, source_name,
      source_type_name_size, source_type_name,
      column_name_size, column_name,
      table_domain_name_size, table_domain_name);
  return ctx->rc;
}

inline static grn_rc
grn_obj_set_info_source_validate(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  grn_rc rc = GRN_SUCCESS;
  grn_id table_id;
  grn_obj *table = NULL;
  grn_id table_domain_id;
  grn_obj *table_domain = NULL;
  grn_id *source_ids;
  int i, n_source_ids;

  table_id = obj->header.domain;
  table = grn_ctx_at(ctx, table_id);
  if (!table) {
    goto exit;
  }

  table_domain_id = table->header.domain;
  table_domain = grn_ctx_at(ctx, table_domain_id);
  if (!table_domain) {
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

  if (!GRN_OBJ_TABLEP(table_domain)) {
    goto exit;
  }

  for (i = 0; i < n_source_ids; i++) {
    grn_id source_id = source_ids[i];
    grn_obj *source;
    grn_id source_type_id;

    source = grn_ctx_at(ctx, source_id);
    if (!source) {
      continue;
    }
    if (GRN_OBJ_TABLEP(source)) {
      source_type_id = source->header.domain;
    } else {
      source_type_id = DB_OBJ(source)->range;
    }
    if (table_domain_id != source_type_id) {
      rc = grn_obj_set_info_source_validate_report_error(ctx,
                                                         obj,
                                                         table_domain,
                                                         source,
                                                         source_type_id);
    }
    grn_obj_unlink(ctx, source);
    if (rc != GRN_SUCCESS) {
      goto exit;
    }
  }

exit:
  if (table) {
    grn_obj_unlink(ctx, table);
  }
  if (table_domain) {
    grn_obj_unlink(ctx, table_domain);
  }
  return ctx->rc;
}

inline static void
grn_obj_set_info_source_log(grn_ctx *ctx, grn_obj *obj, grn_obj *value)
{
  grn_obj buf;
  grn_id *vp = (grn_id *)GRN_BULK_HEAD(value);
  uint32_t vs = GRN_BULK_VSIZE(value), s = 0;
  const char *n = _grn_table_key(ctx, ctx->impl->db, DB_OBJ(obj)->id, &s);
  GRN_TEXT_INIT(&buf, 0);
  GRN_TEXT_PUT(ctx, &buf, n, s);
  GRN_TEXT_PUTC(ctx, &buf, ' ');
  while (vs) {
    n = _grn_table_key(ctx, ctx->impl->db, *vp++, &s);
    GRN_TEXT_PUT(ctx, &buf, n, s);
    vs -= sizeof(grn_id);
    if (vs) { GRN_TEXT_PUTC(ctx, &buf, ','); }
  }
  GRN_LOG(ctx, GRN_LOG_NOTICE, "DDL:set_source %.*s",
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
    memcpy(v2, v, s);
    if (DB_OBJ(obj)->source) { GRN_FREE(DB_OBJ(obj)->source); }
    DB_OBJ(obj)->source = v2;
    DB_OBJ(obj)->source_size = s;

    if (obj->header.type == GRN_COLUMN_INDEX) {
      update_source_hook(ctx, obj);
      build_index(ctx, obj);
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
        ((grn_hash *)obj)->header->tokenizer = grn_obj_id(ctx, value);
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
        ((grn_hash *)obj)->header->normalizer = grn_obj_id(ctx, value);
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

grn_bool
grn_obj_is_builtin(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id;

  if (!obj) { return GRN_FALSE; }

  id = grn_obj_id(ctx, obj);
  if (id == GRN_ID_NIL) {
    return GRN_FALSE;
  } else {
    return id < GRN_N_RESERVED_TYPES;
  }
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
      memcpy(NEXT_ADDR(new), hld_value, hld_size);
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
    grn_bulk_write(ctx, hldbuf, (char *)NEXT_ADDR(hook), hook->hld_size);
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

static void
remove_index(grn_ctx *ctx, grn_obj *obj, grn_hook_entry entry)
{
  grn_hook *h0, *hooks = DB_OBJ(obj)->hooks[entry];
  DB_OBJ(obj)->hooks[entry] = NULL; /* avoid mutual recursive call */
  while (hooks) {
    default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (!target) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int length;
      length = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_UNKNOWN_ERROR,
          "[column][remove][index] "
          "hook has a dangling reference: %.*s", length, name);
    } else if (target->header.type == GRN_COLUMN_INDEX) {
      //TODO: multicolumn  MULTI_COLUMN_INDEXP
      _grn_obj_remove(ctx, target);
    } else {
      //TODO: err
      char fn[GRN_TABLE_MAX_KEY_SIZE];
      int flen;
      flen = grn_obj_name(ctx, target, fn, GRN_TABLE_MAX_KEY_SIZE);
      fn[flen] = '\0';
      ERR(GRN_UNKNOWN_ERROR, "column has unsupported hooks, col=%s",fn);
    }
    h0 = hooks;
    hooks = hooks->next;
    GRN_FREE(h0);
  }
}

static void
remove_columns(grn_ctx *ctx, grn_obj *obj)
{
  grn_hash *cols;
  if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                              GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
    if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)cols)) {
      grn_id *key;
      GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
        grn_obj *col = grn_ctx_at(ctx, *key);
        if (col) { _grn_obj_remove(ctx, col); }
      });
    }
    grn_hash_close(ctx, cols);
  }
}

static void
_grn_obj_remove_db_index_columns(grn_ctx *ctx, grn_obj *db)
{
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1, 0))) {
    grn_id id;
    while ((id = grn_table_cursor_next_inline(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *obj = grn_ctx_at(ctx, id);
      if (obj && obj->header.type == GRN_COLUMN_INDEX) {
        _grn_obj_remove(ctx, obj);
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static void
_grn_obj_remove_db_reference_columns(grn_ctx *ctx, grn_obj *db)
{
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
            _grn_obj_remove(ctx, obj);
            break;
        }
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static void
_grn_obj_remove_db_reference_tables(grn_ctx *ctx, grn_obj *db)
{
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
            _grn_obj_remove(ctx, obj);
            break;
        }
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static void
_grn_obj_remove_db_all_tables(grn_ctx *ctx, grn_obj *db)
{
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
        _grn_obj_remove(ctx, obj);
        break;
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static void
_grn_obj_remove_db(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  const char *io_spath;
  char *spath;
  grn_db *s = (grn_db *)db;
  unsigned char key_type;

  if (s->specs &&
      (io_spath = grn_obj_path(ctx, (grn_obj *)s->specs)) && *io_spath != '\0') {
    if (!(spath = GRN_STRDUP(io_spath))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_spath);
      return;
    }
  } else {
    spath = NULL;
  }

  key_type = s->keys->header.type;

  _grn_obj_remove_db_index_columns(ctx, db);
  _grn_obj_remove_db_reference_columns(ctx, db);
  _grn_obj_remove_db_reference_tables(ctx, db);
  _grn_obj_remove_db_all_tables(ctx, db);

  grn_obj_close(ctx, obj);

  if (spath) {
    grn_ja_remove(ctx, spath);
    GRN_FREE(spath);
  }

  if (path) {
    switch (key_type) {
    case GRN_TABLE_PAT_KEY :
      grn_pat_remove(ctx, path);
      break;
    case GRN_TABLE_DAT_KEY :
      grn_dat_remove(ctx, path);
      break;
    }
  }
}

static grn_bool
is_removable_table(grn_ctx *ctx, grn_obj *table, grn_obj *db)
{
  grn_bool removable = GRN_TRUE;
  grn_id table_id;
  char table_name[GRN_TABLE_MAX_KEY_SIZE];
  int table_name_size;
  grn_table_cursor *cursor;

  table_id = DB_OBJ(table)->id;
  table_name_size = grn_obj_name(ctx, table, table_name, GRN_TABLE_MAX_KEY_SIZE);
  if ((cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                      GRN_CURSOR_BY_ID))) {
    grn_id id;
    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_obj *object;

      object = grn_ctx_at(ctx, id);
      if (!object) {
        ERRCLR(ctx);
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
          char reference_table_name[GRN_TABLE_MAX_KEY_SIZE];
          int reference_table_name_size;
          reference_table_name_size =
            grn_obj_name(ctx, object, reference_table_name,
                         GRN_TABLE_MAX_KEY_SIZE);
          ERR(GRN_OPERATION_NOT_PERMITTED,
              "[table][remove] a table that references the table exists: "
              "<%.*s._key> -> <%.*s>",
              reference_table_name_size, reference_table_name,
              table_name_size, table_name);
          removable = GRN_FALSE;
        }
        break;
      case GRN_COLUMN_VAR_SIZE :
      case GRN_COLUMN_FIX_SIZE :
        if (object->header.domain == table_id) {
          break;
        }
        if (DB_OBJ(object)->range == table_id) {
          char column_name[GRN_TABLE_MAX_KEY_SIZE];
          int column_name_size;
          column_name_size = grn_obj_name(ctx, object, column_name,
                                          GRN_TABLE_MAX_KEY_SIZE);
          ERR(GRN_OPERATION_NOT_PERMITTED,
              "[table][remove] a column that references the table exists: "
              "<%.*s> -> <%.*s>",
              column_name_size, column_name,
              table_name_size, table_name);
          removable = GRN_FALSE;
        }
        break;
      default:
        break;
      }
      grn_obj_unlink(ctx, object);

      if (!removable) {
        break;
      }
    }
    grn_table_cursor_close(ctx, cursor);
  }

  return removable;
}

static void
_grn_obj_remove_pat(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                    const char *path)
{
  if (!is_removable_table(ctx, obj, db)) {
    return;
  }
  remove_index(ctx, obj, GRN_HOOK_INSERT);
  remove_columns(ctx, obj);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_pat_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_dat(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                    const char *path)
{
  if (!is_removable_table(ctx, obj, db)) {
    return;
  }
  remove_index(ctx, obj, GRN_HOOK_INSERT);
  remove_columns(ctx, obj);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_dat_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_hash(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                     const char *path)
{
  if (!is_removable_table(ctx, obj, db)) {
    return;
  }
  remove_index(ctx, obj, GRN_HOOK_INSERT);
  remove_columns(ctx, obj);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_hash_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_array(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path)
{
  if (!is_removable_table(ctx, obj, db)) {
    return;
  }
  remove_columns(ctx, obj);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_array_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_ja(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  remove_index(ctx, obj, GRN_HOOK_SET);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_ja_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_ra(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                   const char *path)
{
  remove_index(ctx, obj, GRN_HOOK_SET);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_ra_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_index(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path)
{
  delete_source_hook(ctx, obj);
  grn_obj_close(ctx, obj);
  if (path) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
    grn_ii_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_db_obj(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                       const char *path)
{
  grn_obj_close(ctx, obj);
  if (!(id & GRN_OBJ_TMP_OBJECT)) {
    grn_ja_put(ctx, ((grn_db *)db)->specs, id, NULL, 0, GRN_OBJ_SET, NULL);
    grn_obj_delete_by_id(ctx, db, id, GRN_TRUE);
  }
  if (path) {
    grn_io_remove(ctx, path);
  }
  grn_obj_touch(ctx, db, NULL);
}

static void
_grn_obj_remove_other(grn_ctx *ctx, grn_obj *obj, grn_obj *db, grn_id id,
                      const char *path)
{
  grn_obj_close(ctx, obj);
}

static void
_grn_obj_remove(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id = GRN_ID_NIL;
  grn_obj *db = NULL;
  const char *io_path;
  char *path;
  if (ctx->impl && ctx->impl->db) {
    uint32_t s = 0;
    const char *n = _grn_table_key(ctx, ctx->impl->db, DB_OBJ(obj)->id, &s);
    GRN_LOG(ctx, GRN_LOG_NOTICE, "DDL:obj_remove %.*s", s, n);
  }
  if ((io_path = grn_obj_path(ctx, obj)) && *io_path != '\0') {
    if (!(path = GRN_STRDUP(io_path))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
      return;
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
    _grn_obj_remove_db(ctx, obj, db, id, path);
    break;
  case GRN_TABLE_PAT_KEY :
    _grn_obj_remove_pat(ctx, obj, db, id, path);
    break;
  case GRN_TABLE_DAT_KEY :
    _grn_obj_remove_dat(ctx, obj, db, id, path);
    break;
  case GRN_TABLE_HASH_KEY :
    _grn_obj_remove_hash(ctx, obj, db, id, path);
    break;
  case GRN_TABLE_NO_KEY :
    _grn_obj_remove_array(ctx, obj, db, id, path);
    break;
  case GRN_COLUMN_VAR_SIZE :
    _grn_obj_remove_ja(ctx, obj, db, id, path);
    break;
  case GRN_COLUMN_FIX_SIZE :
    _grn_obj_remove_ra(ctx, obj, db, id, path);
    break;
  case GRN_COLUMN_INDEX :
    _grn_obj_remove_index(ctx, obj, db, id, path);
    break;
  default :
    if (GRN_DB_OBJP(obj)) {
      _grn_obj_remove_db_obj(ctx, obj, db, id, path);
    } else {
      _grn_obj_remove_other(ctx, obj, db, id, path);
    }
  }
  if (path) { GRN_FREE(path); }
}

grn_rc
grn_obj_remove(grn_ctx *ctx, grn_obj *obj)
{
  GRN_API_ENTER;
  if (ctx->impl && ctx->impl->db && ctx->impl->db != obj) {
    grn_io *io = grn_obj_io(ctx->impl->db);
    if (!grn_io_lock(ctx, io, grn_lock_timeout)) {
      _grn_obj_remove(ctx, obj);
      grn_io_unlock(io);
    }
  } else {
    _grn_obj_remove(ctx, obj);
  }
  GRN_API_RETURN(ctx->rc);
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
      memcpy(fullname, name, name_size);
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
    memcpy(fullname + len + 1, name, name_size);
    name_size += len + 1;
    rc = grn_obj_rename(ctx, column, fullname, name_size);
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
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "grn_table_add failed: <%.*s>", name_size, name);
    } else if (!added) {
      ERR(GRN_INVALID_ARGUMENT,
          "already used name was assigned: <%.*s>", name_size, name);
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
      if (ctx->impl && ctx->impl->values) {
        rc = grn_array_delete_by_id(ctx, ctx->impl->values,
                                      id & ~GRN_OBJ_TMP_OBJECT, NULL);
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
    gen_pathname(grn_obj_io(db)->path, buffer, id);
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
      if (ctx->impl && ctx->impl->values) {
        rc = grn_array_set_value(ctx, ctx->impl->values,
                                 id & ~GRN_OBJ_TMP_OBJECT, &obj, GRN_OBJ_SET);
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

#define GET_PATH(spec,buffer,s,id) do {\
  if (spec->header.flags & GRN_OBJ_CUSTOM_NAME) {\
    const char *path;\
    unsigned int size = grn_vector_get_element(ctx, &v, 1, &path, NULL, NULL); \
    if (size > PATH_MAX) { ERR(GRN_FILENAME_TOO_LONG, "too long path"); }\
    memcpy(buffer, path, size);\
    buffer[size] = '\0';\
  } else {\
    gen_pathname(grn_obj_io(s->keys)->path, buffer, id);  \
  }\
} while (0)

#define UNPACK_INFO() do {\
  if (vp->ptr) {\
    grn_db_obj *r = DB_OBJ(vp->ptr);\
    r->header = spec->header;\
    r->id = id;\
    r->range = spec->range;\
    r->db = (grn_obj *)s;\
    size = grn_vector_get_element(ctx, &v, 2, &p, NULL, NULL);\
    if (size) {\
      if ((r->source = GRN_MALLOC(size))) {\
        memcpy(r->source, p, size);\
        r->source_size = size;\
      }\
    }\
    size = grn_vector_get_element(ctx, &v, 3, &p, NULL, NULL);\
    grn_hook_unpack(ctx, r, p, size);\
  }\
} while (0)

grn_obj *
grn_ctx_at(grn_ctx *ctx, grn_id id)
{
  grn_obj *res = NULL;
  if (!ctx || !ctx->impl || !id) { return res; }
  GRN_API_ENTER;
  if (id & GRN_OBJ_TMP_OBJECT) {
    if (ctx->impl->values) {
      grn_obj **tmp_obj;
      tmp_obj = _grn_array_get_value(ctx, ctx->impl->values, id & ~GRN_OBJ_TMP_OBJECT);
      if (tmp_obj) {
        res = *tmp_obj;
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
          grn_io_win jw;
          uint32_t value_len;
          char *value = grn_ja_ref(ctx, s->specs, id, &jw, &value_len);
          if (value) {
            grn_obj v;
            GRN_OBJ_INIT(&v, GRN_VECTOR, 0, GRN_DB_TEXT);
            if (!grn_vector_decode(ctx, &v, value, value_len)) {
              const char *p;
              uint32_t size;
              grn_obj_spec *spec;
              char buffer[PATH_MAX];
              size = grn_vector_get_element(ctx, &v, 0, (const char **)&spec, NULL, NULL);
              if (size) {
                switch (spec->header.type) {
                case GRN_TYPE :
                  vp->ptr = (grn_obj *)grn_type_open(ctx, spec);
                  UNPACK_INFO();
                  break;
                case GRN_TABLE_HASH_KEY :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_hash_open(ctx, buffer);
                  if (vp->ptr) {
                    grn_obj_flags flags = vp->ptr->header.flags;
                    UNPACK_INFO();
                    vp->ptr->header.flags = flags;
                  }
                  break;
                case GRN_TABLE_PAT_KEY :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_pat_open(ctx, buffer);
                  if (vp->ptr) {
                    grn_obj_flags flags = vp->ptr->header.flags;
                    UNPACK_INFO();
                    vp->ptr->header.flags = flags;
                  }
                  break;
                case GRN_TABLE_DAT_KEY :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_dat_open(ctx, buffer);
                  if (vp->ptr) {
                    grn_obj_flags flags = vp->ptr->header.flags;
                    UNPACK_INFO();
                    vp->ptr->header.flags = flags;
                  }
                  break;
                case GRN_TABLE_NO_KEY :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_array_open(ctx, buffer);
                  UNPACK_INFO();
                  break;
                case GRN_COLUMN_VAR_SIZE :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_ja_open(ctx, buffer);
                  UNPACK_INFO();
                  break;
                case GRN_COLUMN_FIX_SIZE :
                  GET_PATH(spec, buffer, s, id);
                  vp->ptr = (grn_obj *)grn_ra_open(ctx, buffer);
                  UNPACK_INFO();
                  break;
                case GRN_COLUMN_INDEX :
                  GET_PATH(spec, buffer, s, id);
                  {
                    grn_obj *table = grn_ctx_at(ctx, spec->header.domain);
                    vp->ptr = (grn_obj *)grn_ii_open(ctx, buffer, table);
                  }
                  UNPACK_INFO();
                  break;
                case GRN_PROC :
                  GET_PATH(spec, buffer, s, id);
                  grn_plugin_register(ctx, buffer);
                  break;
                case GRN_EXPR :
                  {
                    uint8_t *u;
                    size = grn_vector_get_element(ctx, &v, 4, &p, NULL, NULL);
                    u = (uint8_t *)p;
                    vp->ptr = grn_expr_open(ctx, spec, u, u + size);
                  }
                  break;
                }
              }
              grn_obj_close(ctx, &v);
            }
            grn_ja_unref(ctx, &jw);
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
      }
      res = vp->ptr;
    }
  }
exit :
  GRN_API_RETURN(res);
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
grn_obj_close(grn_ctx *ctx, grn_obj *obj)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  GRN_API_ENTER;
  if (obj) {
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
    case GRN_PTR :
    case GRN_UVECTOR :
    case GRN_PVECTOR :
    case GRN_MSG :
      obj->header.type = GRN_VOID;
      rc = grn_bulk_fin(ctx, obj);
      if (obj->header.impl_flags & GRN_OBJ_ALLOCATED) { GRN_FREE(obj); }
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
    limited_size_inspect(ctx, &inspected, domain_obj);
    ERR(GRN_INVALID_ARGUMENT,
        "[reinit] invalid domain object: <%.*s>",
        (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return ctx->rc;
  }

  grn_obj_get_range_info(ctx, domain_obj, &domain, &flags);
  if (GRN_OBJ_TABLEP(domain_obj) &&
      domain_obj->header.type != GRN_TABLE_NO_KEY) {
    domain = domain_obj->header.domain;
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
  io = grn_obj_io(obj);
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
      if (!(DB_OBJ(obj)->id & GRN_OBJ_TMP_OBJECT)) {
        len = grn_table_get_key(ctx, s->keys, DB_OBJ(obj)->id, namebuf, buf_size);
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
    if (DB_OBJ(obj)->id && DB_OBJ(obj)->id < GRN_ID_MAX) {
      grn_db *s = (grn_db *)DB_OBJ(obj)->db;
      len = grn_table_get_key(ctx, s->keys, DB_OBJ(obj)->id, buf, GRN_TABLE_MAX_KEY_SIZE);
      if (len) {
        int cl;
        char *p = buf, *p0 = p, *pe = p + len;
        for (; p < pe && (cl = grn_charlen(ctx, p, pe)); p += cl) {
          if (*p == GRN_DB_DELIMITER && cl == 1) { p0 = p + cl; }
        }
        len = pe - p0;
        if (len && len <= buf_size) {
          memcpy(namebuf, p0, len);
        }
      }
    }
  } else if (obj->header.type == GRN_ACCESSOR) {
    const char *name = NULL;
    grn_accessor *a;
    for (a = (grn_accessor *)obj; a; a = a->next) {
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        name = GRN_COLUMN_NAME_ID;
        break;
      case GRN_ACCESSOR_GET_KEY :
        name = GRN_COLUMN_NAME_KEY;
        break;
      case GRN_ACCESSOR_GET_VALUE :
        name = GRN_COLUMN_NAME_VALUE;
        break;
      case GRN_ACCESSOR_GET_SCORE :
        name = GRN_COLUMN_NAME_SCORE;
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        name = GRN_COLUMN_NAME_NSUBRECS;
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
      case GRN_ACCESSOR_GET_DB_OBJ :
      case GRN_ACCESSOR_LOOKUP :
      case GRN_ACCESSOR_FUNCALL :
        break;
      }
    }
    if (name) {
      len = strlen(name);
      if (len <= buf_size) {
        memcpy(namebuf, name, len);
      }
    }
  }
  GRN_API_RETURN(len);
}

grn_rc
grn_column_name_(grn_ctx *ctx, grn_obj *obj, grn_obj *buf)
{
  if (GRN_DB_OBJP(obj)) {
    if (DB_OBJ(obj)->id && DB_OBJ(obj)->id < GRN_ID_MAX) {
      uint32_t len;
      grn_db *s = (grn_db *)DB_OBJ(obj)->db;
      const char *p = _grn_table_key(ctx, s->keys, DB_OBJ(obj)->id, &len);
      if (len) {
        int cl;
        const char *p0 = p, *pe = p + len;
        for (; p < pe && (cl = grn_charlen(ctx, p, pe)); p += cl) {
          if (*p == GRN_DB_DELIMITER && cl == 1) { p0 = p + cl; }
        }
        GRN_TEXT_PUT(ctx, buf, p0, pe - p0);
      }
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
  rc = grn_io_lock(ctx, grn_obj_io(obj), timeout);
  GRN_API_RETURN(rc);
}

grn_rc
grn_obj_unlock(grn_ctx *ctx, grn_obj *obj, grn_id id)
{
  GRN_API_ENTER;
  grn_io_unlock(grn_obj_io(obj));
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
    grn_io_clear_lock(grn_obj_io(obj));
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
      grn_io_clear_lock(grn_obj_io(obj));
    }
    break;
  case GRN_COLUMN_FIX_SIZE:
  case GRN_COLUMN_VAR_SIZE:
  case GRN_COLUMN_INDEX:
    grn_io_clear_lock(grn_obj_io(obj));
    break;
  }
  GRN_API_RETURN(GRN_SUCCESS);
}

unsigned int
grn_obj_is_locked(grn_ctx *ctx, grn_obj *obj)
{
  unsigned int res = 0;
  GRN_API_ENTER;
  res = grn_io_is_locked(grn_obj_io(obj));
  GRN_API_RETURN(res);
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
} sort_entry;

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
compare_value(grn_ctx *ctx, sort_entry *a, sort_entry *b,
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
swap(sort_entry *a, sort_entry *b)
{
  sort_entry c_ = *a;
  *a = *b;
  *b = c_;
}

inline static sort_entry *
part(grn_ctx *ctx, sort_entry *b, sort_entry *e, grn_table_sort_key *keys, int n_keys)
{
  sort_entry *c;
  intptr_t d = e - b;
  if (compare_value(ctx, b, e, keys, n_keys)) {
    swap(b, e);
  }
  if (d < 2) { return NULL; }
  c = b + (d >> 1);
  if (compare_value(ctx, b, c, keys, n_keys)) {
    swap(b, c);
  } else {
    if (compare_value(ctx, c, e, keys, n_keys)) {
      swap(c, e);
    }
  }
  if (d < 3) { return NULL; }
  b++;
  swap(b, c);
  c = b;
  for (;;) {
    do {
      b++;
    } while (compare_value(ctx, c, b, keys, n_keys));
    do {
      e--;
    } while (compare_value(ctx, e, c, keys, n_keys));
    if (b >= e) { break; }
    swap(b, e);
  }
  swap(c, e);
  return e;
}

static void
_sort(grn_ctx *ctx, sort_entry *head, sort_entry *tail, int from, int to,
      grn_table_sort_key *keys, int n_keys)
{
  sort_entry *c;
  if (head < tail && (c = part(ctx, head, tail, keys, n_keys))) {
    intptr_t m = c - head + 1;
    if (from < m - 1) { _sort(ctx, head, c - 1, from, to, keys, n_keys); }
    if (m < to) { _sort(ctx, c + 1, tail, from - m, to - m, keys, n_keys); }
  }
}

static sort_entry *
pack(grn_ctx *ctx, grn_obj *table, sort_entry *head, sort_entry *tail,
     grn_table_sort_key *keys, int n_keys)
{
  int i = 0;
  sort_entry e, c;
  grn_table_cursor *tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0);
  if (!tc) { return NULL; }
  if ((c.id = grn_table_cursor_next_inline(ctx, tc))) {
    c.value = grn_obj_get_value_(ctx, keys->key, c.id, &c.size);
    while ((e.id = grn_table_cursor_next_inline(ctx, tc))) {
      e.value = grn_obj_get_value_(ctx, keys->key, e.id, &e.size);
      if (compare_value(ctx, &c, &e, keys, n_keys)) {
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
  sort_entry *array, *ep;
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
    i = grn_geo_table_sort(ctx, table, offset, limit, result, keys, n_keys);
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
          grn_ii_posting *posting;
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
    grn_table_sort_key *kp;
    for (kp = keys, j = n_keys; j; kp++, j--) {
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
          kp->offset = KEY_UINT32;
        }
      }
    }
    if (!(array = GRN_MALLOC(sizeof(sort_entry) * n))) {
      goto exit;
    }
    if ((ep = pack(ctx, table, array, array + n - 1, keys, n_keys))) {
      intptr_t m = ep - array + 1;
      if (offset < m - 1) { _sort(ctx, array, ep - 1, offset, e, keys, n_keys); }
      if (m < e) { _sort(ctx, ep + 1, array + n - 1, offset - m, e - m, keys, n_keys); }
    }
    {
      grn_id *v;
      for (i = 0, ep = array + offset; i < limit && ep < array + n; i++, ep++) {
        if (!grn_array_add(ctx, (grn_array *)result, (void **)&v)) { break; }
        *v = ep->id;
      }
      GRN_FREE(array);
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
                GRN_OBJ_KEY_VAR_SIZE, 1 << 31);
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
  for (id = grn_db_curr_id(ctx, db) + 1; id < 128; id++) {
    grn_itoh(id, buf + 3, 2);
    grn_obj_register(ctx, db, buf, 5);
  }
  grn_db_init_builtin_query(ctx);
  for (id = grn_db_curr_id(ctx, db) + 1; id < GRN_N_RESERVED_TYPES; id++) {
    grn_itoh(id, buf + 3, 2);
    grn_obj_register(ctx, db, buf, 5);
  }
  return ctx->rc;
}

#define MULTI_COLUMN_INDEXP(i) (DB_OBJ(i)->source_size > sizeof(grn_id))

static inline int
grn_column_index_column_equal(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                              grn_obj **indexbuf, int buf_size, int *section)
{
  int n = 0;
  grn_obj **ip = indexbuf;
  grn_hook *hooks;

  for (hooks = DB_OBJ(obj)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
    default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    if (section) { *section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0; }
    if (obj->header.type != GRN_COLUMN_FIX_SIZE) {
      grn_obj *tokenizer, *lexicon = grn_ctx_at(ctx, target->header.domain);
      if (!lexicon) { continue; }
      grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL);
      if (tokenizer) { continue; }
    }
    if (n < buf_size) {
      *ip++ = target;
    }
    n++;
  }

  return n;
}

static inline int
grn_column_index_column_match(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                              grn_obj **indexbuf, int buf_size, int *section)
{
  int n = 0;
  grn_obj **ip = indexbuf;
  grn_hook *hooks;

  for (hooks = DB_OBJ(obj)->hooks[GRN_HOOK_SET]; hooks; hooks = hooks->next) {
    default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    if (section) { *section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0; }
    if (n < buf_size) {
      *ip++ = target;
    }
    n++;
  }

  return n;
}

static inline int
grn_column_index_column_range(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                              grn_obj **indexbuf, int buf_size, int *section)
{
  int n = 0;
  grn_obj **ip = indexbuf;
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
    default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (target->header.type != GRN_COLUMN_INDEX) { continue; }
    if (section) { *section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0; }
    {
      grn_obj *tokenizer, *lexicon = grn_ctx_at(ctx, target->header.domain);
      if (!lexicon) { continue; }
      if (lexicon->header.type != GRN_TABLE_PAT_KEY) { continue; }
      /* FIXME: GRN_TABLE_DAT_KEY should be supported */
      grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL);
      if (tokenizer) { continue; }
    }
    if (n < buf_size) {
      *ip++ = target;
    }
    n++;
  }

  return n;
}

static inline grn_bool
is_valid_match_index(grn_ctx *ctx, grn_obj *index_column)
{
  return GRN_TRUE;
}

static inline grn_bool
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

  grn_table_get_info(ctx, lexicon, NULL, NULL, &tokenizer, NULL);
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
grn_column_index_accessor_index_column(grn_ctx *ctx, grn_accessor *a,
                                       grn_operator op,
                                       grn_obj **indexbuf, int buf_size,
                                       int *section)
{
  grn_obj *index_column = a->obj;

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
    if (section) {
      *section = specified_section;
    }
  }
  if (buf_size > 0) {
    *indexbuf = index_column;
  }

  return 1;
}

static inline int
grn_column_index_accessor(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                          grn_obj **indexbuf, int buf_size, int *section)
{
  int n = 0;
  grn_obj **ip = indexbuf;
  grn_accessor *a = (grn_accessor *)obj;

  while (a) {
    grn_hook *hooks;
    grn_bool found = GRN_FALSE;
    grn_hook_entry entry = -1;

    if (a->action == GRN_ACCESSOR_GET_COLUMN_VALUE &&
        GRN_OBJ_INDEX_COLUMNP(a->obj)) {
      return grn_column_index_accessor_index_column(ctx, a, op, indexbuf,
                                                    buf_size, section);
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

    if (entry == -1) {
      break;
    }

    for (hooks = DB_OBJ(a->obj)->hooks[entry]; hooks; hooks = hooks->next) {
      default_set_value_hook_data *data = (void *)NEXT_ADDR(hooks);
      grn_obj *target = grn_ctx_at(ctx, data->target);

      if (target->header.type != GRN_COLUMN_INDEX) { continue; }

      found = GRN_TRUE;
      if (!a->next) {
        if (!is_valid_index(ctx, target, op)) {
          continue;
        }

        if (section) {
          *section = (MULTI_COLUMN_INDEXP(target)) ? data->section : 0;
        }
        if (n < buf_size) {
          *ip++ = target;
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

int
grn_column_index(grn_ctx *ctx, grn_obj *obj, grn_operator op,
                 grn_obj **indexbuf, int buf_size, int *section)
{
  int n = 0;
  GRN_API_ENTER;
  if (GRN_DB_OBJP(obj)) {
    switch (op) {
    case GRN_OP_EQUAL :
      n = grn_column_index_column_equal(ctx, obj, op,
                                        indexbuf, buf_size, section);
      break;
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
      n = grn_column_index_column_match(ctx, obj, op,
                                        indexbuf, buf_size, section);
      break;
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_CALL :
      n = grn_column_index_column_range(ctx, obj, op,
                                        indexbuf, buf_size, section);
      break;
    default :
      break;
    }
  } else if (GRN_ACCESSORP(obj)) {
    if (section) {
      *section = 0;
    }
    switch (op) {
    case GRN_OP_EQUAL :
    case GRN_OP_TERM_EXTRACT :
      if (buf_size) { indexbuf[n] = obj; }
      n++;
      break;
    case GRN_OP_PREFIX :
      {
        grn_accessor *a = (grn_accessor *)obj;
        if (a->action == GRN_ACCESSOR_GET_KEY) {
          if (a->obj->header.type == GRN_TABLE_PAT_KEY) {
            if (buf_size) { indexbuf[n] = obj; }
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
            if (buf_size) { indexbuf[n] = obj; }
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
      n = grn_column_index_accessor(ctx, obj, op, indexbuf, buf_size, section);
      break;
    default :
      break;
    }
  }
  GRN_API_RETURN(n);
}

/* todo : refine */
static int
tokenize(const char *str, size_t str_len, const char **tokbuf, int buf_size, const char **rest)
{
  const char **tok = tokbuf, **tok_end = tokbuf + buf_size;
  if (buf_size > 0) {
    const char *str_end = str + str_len;
    while (str < str_end && (' ' == *str || ',' == *str)) { str++; }
    for (;;) {
      if (str == str_end) {
        *tok++ = str;
        break;
      }
      if (' ' == *str || ',' == *str) {
        // *str = '\0';
        *tok++ = str;
        if (tok == tok_end) { break; }
        do { str++; } while (str < str_end && (' ' == *str || ',' == *str));
      } else {
        str++;
      }
    }
  }
  if (rest) { *rest = str; }
  return tok - tokbuf;
}

grn_rc
grn_obj_columns(grn_ctx *ctx, grn_obj *table,
                const char *str, unsigned int str_size, grn_obj *res)
{
  grn_obj *col;
  const char *p = (char *)str, *q, *r, *pe = p + str_size, *tokbuf[256];
  while (p < pe) {
    int i, n = tokenize(p, pe - p, tokbuf, 256, &q);
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
                  cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                         GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
                  if (cols) {
                    grn_id *key;
                    grn_accessor *a, *ac;
                    grn_obj *target_table = table;
                    for (a = (grn_accessor *)ai; a; a = a->next) {
                      target_table = a->obj;
                    }
                    grn_table_columns(ctx, target_table,
                                      p, r - p - 1, (grn_obj *)cols);
                    GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
                      if ((col = grn_ctx_at(ctx, *key))) {
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
    int i, n = tokenize(str, str_size, tokbuf, str_size, NULL);
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
  if ((keys = grn_table_sort_key_from_str_geo(ctx, str, str_size, table, nkeys))) {
    return keys;
  }
  if ((tokbuf = GRN_MALLOCN(const char *, str_size))) {
    int i, n = tokenize(str, str_size, tokbuf, str_size, NULL);
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
              GRN_LOG(ctx, GRN_WARN,
                      "ignore invalid sort key: <%.*s>(<%.*s>)",
                      (int)(r - p), p, str_size, str);
            } else {
              WARN(GRN_INVALID_ARGUMENT,
                   "invalid sort key: <%.*s>(<%.*s>)",
                   (int)(r - p), p, str_size, str);
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
  int i;
  if (keys) {
    for (i = 0; i < nkeys; i++) {
      grn_obj_unlink(ctx, keys[i].key);
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
  grn_token *token = NULL;
  grn_token_mode mode = addp ? GRN_TOKEN_ADD : GRN_TOKEN_GET;
  GRN_API_ENTER;
  if (!(token = grn_token_open(ctx, table, str, str_len, mode, 0))) {
    goto exit;
  }
  if (buf) {
    GRN_BULK_REWIND(buf);
  } else {
    if (!(buf = grn_obj_open(ctx, GRN_UVECTOR, 0, DB_OBJ(table)->id))) {
      goto exit;
    }
  }
  while (token->status != GRN_TOKEN_DONE) {
    grn_id tid;
    if ((tid = grn_token_next(ctx, token))) {
      GRN_RECORD_PUT(ctx, buf, tid);
    }
  }
exit :
  if (token) {
    grn_token_close(ctx, token);
  }
  GRN_API_RETURN(buf);
}

/* grn_load */

static grn_obj *
values_add(grn_ctx *ctx, grn_loader *loader)
{
  grn_obj *res;
  uint32_t curr_size = loader->values_size * sizeof(grn_obj);
  if (curr_size < GRN_TEXT_LEN(&loader->values)) {
    res = (grn_obj *)(GRN_TEXT_VALUE(&loader->values) + curr_size);
    res->header.domain = GRN_DB_TEXT;
    GRN_BULK_REWIND(res);
  } else {
    if (grn_bulk_space(ctx, &loader->values, sizeof(grn_obj))) { return NULL; }
    res = (grn_obj *)(GRN_TEXT_VALUE(&loader->values) + curr_size);
    GRN_TEXT_INIT(res, 0);
  }
  loader->values_size++;
  loader->last = res;
  return res;
}

static grn_obj *
values_next(grn_ctx *ctx, grn_obj *value)
{
  if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET ||
      value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
    value += GRN_UINT32_VALUE(value);
  }
  return value + 1;
}

static int
values_len(grn_ctx *ctx, grn_obj *head, grn_obj *tail)
{
  int len;
  for (len = 0; head < tail; head = values_next(ctx, head), len++) ;
  return len;
}

static grn_id
loader_add(grn_ctx *ctx, grn_obj *key)
{
  int added = 0;
  grn_loader *loader = &ctx->impl->loader;
  grn_id id = grn_table_add_by_key(ctx, loader->table, key, &added);
  if (!added && loader->ifexists) {
    grn_obj *v = grn_expr_get_var_by_offset(ctx, loader->ifexists, 0);
    grn_obj *result;
    unsigned int result_boolean;
    GRN_RECORD_SET(ctx, v, id);
    result = grn_expr_exec(ctx, loader->ifexists, 0);
    GRN_TRUEP(ctx, result, result_boolean);
    if (!result_boolean) { id = 0; }
  }
  return id;
}

static void
set_vector(grn_ctx *ctx, grn_obj *column, grn_id id, grn_obj *vector)
{
  int n = GRN_UINT32_VALUE(vector);
  grn_obj buf, *v = vector + 1;
  grn_id range_id;
  grn_obj *range;

  range_id = DB_OBJ(column)->range;
  range = grn_ctx_at(ctx, range_id);
  if (GRN_OBJ_TABLEP(range)) {
    GRN_RECORD_INIT(&buf, GRN_OBJ_VECTOR, range_id);
    while (n--) {
      grn_bool cast_failed = GRN_FALSE;
      grn_obj record, *element = v;
      if (range_id != element->header.domain) {
        GRN_RECORD_INIT(&record, 0, range_id);
        if (grn_obj_cast(ctx, element, &record, GRN_TRUE)) {
          cast_failed = GRN_TRUE;
          ERR_CAST(column, range, element);
        }
        element = &record;
      }
      if (!cast_failed) {
        GRN_UINT32_PUT(ctx, &buf, GRN_RECORD_VALUE(element));
      }
      if (element == &record) { GRN_OBJ_FIN(ctx, element); }
      v = values_next(ctx, v);
    }
  } else {
    if (((struct _grn_type *)range)->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
      GRN_TEXT_INIT(&buf, GRN_OBJ_VECTOR);
      while (n--) {
        if (v->header.domain == GRN_DB_TEXT) {
          grn_bool cast_failed = GRN_FALSE;
          grn_obj casted_element, *element = v;
          if (range_id != element->header.domain) {
            GRN_OBJ_INIT(&casted_element, GRN_BULK, 0, range_id);
            if (grn_obj_cast(ctx, element, &casted_element, GRN_TRUE)) {
              cast_failed = GRN_TRUE;
              ERR_CAST(column, range, element);
            }
            element = &casted_element;
          }
          if (!cast_failed) {
            grn_vector_add_element(ctx, &buf,
                                   GRN_TEXT_VALUE(element),
                                   GRN_TEXT_LEN(element), 0,
                                   element->header.domain);
          }
          if (element == &casted_element) { GRN_OBJ_FIN(ctx, element); }
        } else {
          ERR(GRN_INVALID_ARGUMENT, "bad syntax.");
        }
        v = values_next(ctx, v);
      }
    } else {
      grn_id value_size = ((grn_db_obj *)range)->range;
      GRN_VALUE_FIX_SIZE_INIT(&buf, GRN_OBJ_VECTOR, range_id);
      while (n--) {
        grn_bool cast_failed = GRN_FALSE;
        grn_obj casted_element, *element = v;
        if (range_id != element->header.domain) {
          GRN_OBJ_INIT(&casted_element, GRN_BULK, 0, range_id);
          if (grn_obj_cast(ctx, element, &casted_element, GRN_TRUE)) {
            cast_failed = GRN_TRUE;
            ERR_CAST(column, range, element);
          }
          element = &casted_element;
        }
        if (!cast_failed) {
          grn_bulk_write(ctx, &buf, GRN_TEXT_VALUE(element), value_size);
        }
        if (element == &casted_element) { GRN_OBJ_FIN(ctx, element); }
        v = values_next(ctx, v);
      }
    }
  }
  grn_obj_set_value(ctx, column, id, &buf, GRN_OBJ_SET);
  GRN_OBJ_FIN(ctx, &buf);
}

static void
set_weight_vector(grn_ctx *ctx, grn_obj *column, grn_id id, grn_obj *index_value)
{
  if (!GRN_OBJ_WEIGHT_VECTOR_COLUMNP(column)) {
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    int column_name_size;
    column_name_size = grn_obj_name(ctx, column, column_name,
                                    GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_INVALID_ARGUMENT,
        "<%.*s>: columns except weight vector column don't support object value",
        column_name_size, column_name);
    return;
  }

  {
    unsigned int i, n;
    grn_obj vector;
    grn_obj weight_buffer;

    n = GRN_UINT32_VALUE(index_value);
    GRN_TEXT_INIT(&vector, GRN_OBJ_VECTOR);
    GRN_UINT32_INIT(&weight_buffer, 0);
    for (i = 0; i < n; i += 2) {
      grn_rc rc;
      grn_obj *key, *weight;

      key = index_value + 1 + i;
      weight = key + 1;

      GRN_BULK_REWIND(&weight_buffer);
      rc = grn_obj_cast(ctx, weight, &weight_buffer, GRN_TRUE);
      if (rc != GRN_SUCCESS) {
        grn_obj *range;
        range = grn_ctx_at(ctx, weight_buffer.header.domain);
        ERR_CAST(column, range, weight);
        grn_obj_unlink(ctx, range);
        break;
      }
      grn_vector_add_element(ctx, &vector,
                             GRN_BULK_HEAD(key), GRN_BULK_VSIZE(key),
                             GRN_UINT32_VALUE(&weight_buffer),
                             key->header.domain);
    }
    grn_obj_set_value(ctx, column, id, &vector, GRN_OBJ_SET);
    GRN_OBJ_FIN(ctx, &vector);
  }
}

static inline int
name_equal(const char *p, unsigned int size, const char *name)
{
  if (strlen(name) != size) { return 0; }
  if (*p != GRN_DB_PSEUDO_COLUMN_PREFIX) { return 0; }
  return !memcmp(p + 1, name + 1, size - 1);
}

static void
report_set_column_value_failure(grn_ctx *ctx,
                                grn_obj *key,
                                const char *column_name,
                                unsigned int column_name_size,
                                grn_obj *column_value)
{
  grn_obj key_inspected, column_value_inspected;

  GRN_TEXT_INIT(&key_inspected, 0);
  GRN_TEXT_INIT(&column_value_inspected, 0);
  limited_size_inspect(ctx, &key_inspected, key);
  limited_size_inspect(ctx, &column_value_inspected, column_value);
  GRN_LOG(ctx, GRN_LOG_ERROR,
          "[table][load] failed to set column value: %s: "
          "key: <%.*s>, column: <%.*s>, value: <%.*s>",
          ctx->errbuf,
          (int)GRN_TEXT_LEN(&key_inspected),
          GRN_TEXT_VALUE(&key_inspected),
          column_name_size,
          column_name,
          (int)GRN_TEXT_LEN(&column_value_inspected),
          GRN_TEXT_VALUE(&column_value_inspected));
  GRN_OBJ_FIN(ctx, &key_inspected);
  GRN_OBJ_FIN(ctx, &column_value_inspected);
}

static void
bracket_close(grn_ctx *ctx, grn_loader *loader)
{
  grn_obj *value, *col, *ve;
  grn_id id = GRN_ID_NIL;
  grn_obj *key_value = NULL;
  grn_obj **cols = (grn_obj **)GRN_BULK_HEAD(&loader->columns);
  uint32_t begin, ndata, ncols = GRN_BULK_VSIZE(&loader->columns) / sizeof(grn_obj *);
  GRN_UINT32_POP(&loader->level, begin);
  value = ((grn_obj *)(GRN_TEXT_VALUE(&loader->values))) + begin;
  ve = ((grn_obj *)(GRN_TEXT_VALUE(&loader->values))) + loader->values_size;
  GRN_ASSERT(value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET);
  GRN_UINT32_SET(ctx, value, loader->values_size - begin - 1);
  value++;
  if (GRN_BULK_VSIZE(&loader->level) <= sizeof(uint32_t) * loader->emit_level) {
    ndata = values_len(ctx, value, ve);
    if (loader->table) {
      switch (loader->table->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        if (loader->key_offset != -1 && ndata == ncols + 1) {
          key_value = value + loader->key_offset;
          id = loader_add(ctx, key_value);
        } else if (loader->key_offset == -1) {
          int i = 0;
          grn_obj *key_column_name = NULL;
          while (ndata--) {
            char *column_name = GRN_TEXT_VALUE(value);
            unsigned int column_name_size = GRN_TEXT_LEN(value);
            if (value->header.domain == GRN_DB_TEXT &&
                (name_equal(column_name, column_name_size,
                            GRN_COLUMN_NAME_KEY) ||
                 name_equal(column_name, column_name_size,
                            GRN_COLUMN_NAME_ID))) {
              if (loader->key_offset != -1) {
                GRN_LOG(ctx, GRN_LOG_ERROR,
                        "duplicated key columns: <%.*s> at %d and <%.*s> at %i",
                        (int)GRN_TEXT_LEN(key_column_name),
                        GRN_TEXT_VALUE(key_column_name),
                        loader->key_offset,
                        column_name_size, column_name, i);
                return;
              }
              key_column_name = value;
              loader->key_offset = i;
            } else {
              col = grn_obj_column(ctx, loader->table,
                                   column_name, column_name_size);
              if (!col) {
                ERR(GRN_INVALID_ARGUMENT,
                    "nonexistent column: <%.*s>",
                    column_name_size, column_name);
                return;
              }
              GRN_PTR_PUT(ctx, &loader->columns, col);
            }
            value++;
            i++;
          }
        }
        break;
      case GRN_TABLE_NO_KEY :
        if ((GRN_BULK_VSIZE(&loader->level)) > 0 &&
            (ndata == 0 || ndata == ncols)) {
          id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
        } else if (!ncols) {
          while (ndata--) {
            if (value->header.domain == GRN_DB_TEXT) {
              char *column_name = GRN_TEXT_VALUE(value);
              unsigned int column_name_size = GRN_TEXT_LEN(value);
              col = grn_obj_column(ctx, loader->table,
                                   column_name, column_name_size);
              if (!col) {
                ERR(GRN_INVALID_ARGUMENT,
                    "nonexistent column: <%.*s>",
                    column_name_size, column_name);
                return;
              }
              GRN_PTR_PUT(ctx, &loader->columns, col);
              value++;
            } else {
              grn_obj buffer;
              GRN_TEXT_INIT(&buffer, 0);
              grn_inspect(ctx, &buffer, value);
              ERR(GRN_INVALID_ARGUMENT,
                  "column name must be string: <%.*s>",
                  (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
              GRN_OBJ_FIN(ctx, &buffer);
              return;
            }
          }
        }
        break;
      default :
        break;
      }
      if (id) {
        int i = 0;
        while (ndata--) {
          grn_obj *column;
          if ((loader->table->header.type == GRN_TABLE_HASH_KEY ||
               loader->table->header.type == GRN_TABLE_PAT_KEY ||
               loader->table->header.type == GRN_TABLE_DAT_KEY) &&
              i == loader->key_offset) {
              /* skip this value, because it's already used as key value */
             value = values_next(ctx, value);
             i++;
             continue;
          }

          column = *cols;
          if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
            set_vector(ctx, column, id, value);
          } else if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
            set_weight_vector(ctx, column, id, value);
          } else {
            grn_obj_set_value(ctx, column, id, value, GRN_OBJ_SET);
          }
          if (ctx->rc != GRN_SUCCESS) {
            char column_name[GRN_TABLE_MAX_KEY_SIZE];
            unsigned int column_name_size;
            column_name_size = grn_obj_name(ctx, column, column_name,
                                            GRN_TABLE_MAX_KEY_SIZE);
            report_set_column_value_failure(ctx, key_value,
                                            column_name, column_name_size,
                                            value);
            ERRCLR(ctx);
          }
          value = values_next(ctx, value);
          cols++;
          i++;
        }
        if (loader->each) {
          grn_obj *v = grn_expr_get_var_by_offset(ctx, loader->each, 0);
          GRN_RECORD_SET(ctx, v, id);
          grn_expr_exec(ctx, loader->each, 0);
        }
        loader->nrecords++;
      }
    }
    loader->values_size = begin;
  }
}

static void
brace_close(grn_ctx *ctx, grn_loader *loader)
{
  uint32_t begin;
  grn_obj *key_value = NULL;
  grn_obj *value, *ve;
  grn_id id = GRN_ID_NIL;
  GRN_UINT32_POP(&loader->level, begin);
  value = ((grn_obj *)(GRN_TEXT_VALUE(&loader->values))) + begin;
  ve = ((grn_obj *)(GRN_TEXT_VALUE(&loader->values))) + loader->values_size;
  GRN_ASSERT(value->header.domain == GRN_JSON_LOAD_OPEN_BRACE);
  GRN_UINT32_SET(ctx, value, loader->values_size - begin - 1);
  value++;
  if (GRN_BULK_VSIZE(&loader->level) <= sizeof(uint32_t) * loader->emit_level) {
    if (loader->table) {
      switch (loader->table->header.type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_DAT_KEY :
        {
          grn_obj *v, *key_column_name = NULL;
          for (v = value; v + 1 < ve; v = values_next(ctx, v)) {
            char *column_name = GRN_TEXT_VALUE(v);
            unsigned int column_name_size = GRN_TEXT_LEN(v);
            if (v->header.domain == GRN_DB_TEXT &&
                (name_equal(column_name, column_name_size,
                            GRN_COLUMN_NAME_KEY) ||
                 name_equal(column_name, column_name_size,
                            GRN_COLUMN_NAME_ID))) {
              if (key_column_name) {
                GRN_LOG(ctx, GRN_LOG_ERROR, "duplicated key columns: %.*s and %.*s",
                        (int)GRN_TEXT_LEN(key_column_name),
                        GRN_TEXT_VALUE(key_column_name),
                        column_name_size, column_name);
                return;
              }
              key_column_name = value;
              v++;
              key_value = v;
              id = loader_add(ctx, key_value);
            } else {
              v = values_next(ctx, v);
            }
          }
        }
        break;
      case GRN_TABLE_NO_KEY :
        id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
        break;
      default :
        break;
      }
      if (id) {
        grn_obj *col;
        const char *name;
        unsigned int name_size;
        while (value + 1 < ve) {
          if (value->header.domain != GRN_DB_TEXT) { break; /* error */ }
          name = GRN_TEXT_VALUE(value);
          name_size = GRN_TEXT_LEN(value);
          col = grn_obj_column(ctx, loader->table, name, name_size);
          value++;
          /* auto column create
          if (!col) {
            if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
              grn_obj *v = value + 1;
              col = grn_column_create(ctx, loader->table, name, name_size,
                                      NULL, GRN_OBJ_PERSISTENT|GRN_OBJ_COLUMN_VECTOR,
                                      grn_ctx_at(ctx, v->header.domain));
            } else {
              col = grn_column_create(ctx, loader->table, name, name_size,
                                      NULL, GRN_OBJ_PERSISTENT,
                                      grn_ctx_at(ctx, value->header.domain));
            }
          }
          */
          if (col) {
            if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
              set_vector(ctx, col, id, value);
            } else if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
              set_weight_vector(ctx, col, id, value);
            } else {
              grn_obj_set_value(ctx, col, id, value, GRN_OBJ_SET);
            }
            if (ctx->rc != GRN_SUCCESS) {
              report_set_column_value_failure(ctx, key_value,
                                              name, name_size, value);
              ERRCLR(ctx);
            }
            grn_obj_unlink(ctx, col);
          } else {
            GRN_LOG(ctx, GRN_LOG_ERROR, "invalid column('%.*s')", (int)name_size, name);
          }
          value = values_next(ctx, value);
        }
        if (loader->each) {
          grn_obj *v = grn_expr_get_var_by_offset(ctx, loader->each, 0);
          GRN_RECORD_SET(ctx, v, id);
          grn_expr_exec(ctx, loader->each, 0);
        }
        loader->nrecords++;
      } else {
        GRN_LOG(ctx, GRN_LOG_ERROR, "neither _key nor _id is assigned");
      }
    }
    loader->values_size = begin;
  }
}

#define JSON_READ_OPEN_BRACKET() do {\
  GRN_UINT32_PUT(ctx, &loader->level, loader->values_size);\
  values_add(ctx, loader);\
  loader->last->header.domain = GRN_JSON_LOAD_OPEN_BRACKET;\
  loader->stat = GRN_LOADER_TOKEN;\
  str++;\
} while (0)

#define JSON_READ_OPEN_BRACE() do {\
  GRN_UINT32_PUT(ctx, &loader->level, loader->values_size);\
  values_add(ctx, loader);\
  loader->last->header.domain = GRN_JSON_LOAD_OPEN_BRACE;\
  loader->stat = GRN_LOADER_TOKEN;\
  str++;\
} while (0)

static void
json_read(grn_ctx *ctx, grn_loader *loader, const char *str, unsigned int str_len)
{
  const char *const beg = str;
  char c;
  int len;
  const char *se = str + str_len;
  while (str < se) {
    c = *str;
    switch (loader->stat) {
    case GRN_LOADER_BEGIN :
      if ((len = grn_isspace(str, ctx->encoding))) {
        str += len;
        c = *str;
        continue;
      }
      switch (c) {
      case '[' :
        JSON_READ_OPEN_BRACKET();
        break;
      case '{' :
        JSON_READ_OPEN_BRACE();
        break;
      default :
        ERR(GRN_INVALID_ARGUMENT,
            "JSON must start with '[' or '{': <%.*s>", str_len, beg);
        loader->stat = GRN_LOADER_END;
        break;
      }
      break;
    case GRN_LOADER_TOKEN :
      if ((len = grn_isspace(str, ctx->encoding))) {
        str += len;
        c = *str;
        continue;
      }
      switch (c) {
      case '"' :
        loader->stat = GRN_LOADER_STRING;
        values_add(ctx, loader);
        str++;
        break;
      case '[' :
        JSON_READ_OPEN_BRACKET();
        break;
      case '{' :
        JSON_READ_OPEN_BRACE();
        break;
      case ':' :
        str++;
        break;
      case ',' :
        str++;
        break;
      case ']' :
        bracket_close(ctx, loader);
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        str++;
        break;
      case '}' :
        brace_close(ctx, loader);
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        str++;
        break;
      case '+' : case '-' : case '0' : case '1' : case '2' : case '3' :
      case '4' : case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->stat = GRN_LOADER_NUMBER;
        values_add(ctx, loader);
        break;
      default :
        if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('_' == c)) {
          loader->stat = GRN_LOADER_SYMBOL;
          values_add(ctx, loader);
        } else {
          if ((len = grn_charlen(ctx, str, se))) {
            GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char('%c') at", c);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg) + len, beg);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%*s", (int)(str - beg) + 1, "^");
            str += len;
          } else {
            GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char(\\x%.2x) after", c);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg), beg);
            str = se;
          }
        }
        break;
      }
      break;
    case GRN_LOADER_SYMBOL :
      if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') ||
          ('0' <= c && c <= '9') || ('_' == c)) {
        GRN_TEXT_PUTC(ctx, loader->last, c);
        str++;
      } else {
        char *v = GRN_TEXT_VALUE(loader->last);
        switch (*v) {
        case 'n' :
          if (GRN_TEXT_LEN(loader->last) == 4 && !memcmp(v, "null", 4)) {
            loader->last->header.domain = GRN_DB_VOID;
            GRN_BULK_REWIND(loader->last);
          }
          break;
        case 't' :
          if (GRN_TEXT_LEN(loader->last) == 4 && !memcmp(v, "true", 4)) {
            loader->last->header.domain = GRN_DB_BOOL;
            GRN_BOOL_SET(ctx, loader->last, GRN_TRUE);
          }
          break;
        case 'f' :
          if (GRN_TEXT_LEN(loader->last) == 5 && !memcmp(v, "false", 5)) {
            loader->last->header.domain = GRN_DB_BOOL;
            GRN_BOOL_SET(ctx, loader->last, GRN_FALSE);
          }
          break;
        default :
          break;
        }
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
      }
      break;
    case GRN_LOADER_NUMBER :
      switch (c) {
      case '+' : case '-' : case '.' : case 'e' : case 'E' :
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        GRN_TEXT_PUTC(ctx, loader->last, c);
        str++;
        break;
      default :
        {
          const char *cur, *str = GRN_BULK_HEAD(loader->last);
          const char *str_end = GRN_BULK_CURR(loader->last);
          int64_t i = grn_atoll(str, str_end, &cur);
          if (cur == str_end) {
            loader->last->header.domain = GRN_DB_INT64;
            GRN_INT64_SET(ctx, loader->last, i);
          } else if (cur != str) {
            double d;
            char *end;
            grn_obj buf;
            GRN_TEXT_INIT(&buf, 0);
            GRN_TEXT_PUT(ctx, &buf, str, GRN_BULK_VSIZE(loader->last));
            GRN_TEXT_PUTC(ctx, &buf, '\0');
            errno = 0;
            d = strtod(GRN_TEXT_VALUE(&buf), &end);
            if (!errno && end + 1 == GRN_BULK_CURR(&buf)) {
              loader->last->header.domain = GRN_DB_FLOAT;
              GRN_FLOAT_SET(ctx, loader->last, d);
            }
            GRN_OBJ_FIN(ctx, &buf);
          }
        }
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        break;
      }
      break;
    case GRN_LOADER_STRING :
      switch (c) {
      case '\\' :
        loader->stat = GRN_LOADER_STRING_ESC;
        str++;
        break;
      case '"' :
        str++;
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        /*
        *(GRN_BULK_CURR(loader->last)) = '\0';
        GRN_LOG(ctx, GRN_LOG_ALERT, "read str(%s)", GRN_TEXT_VALUE(loader->last));
        */
        break;
      default :
        if ((len = grn_charlen(ctx, str, se))) {
          GRN_TEXT_PUT(ctx, loader->last, str, len);
          str += len;
        } else {
          GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char(\\x%.2x) after", c);
          GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg), beg);
          str = se;
        }
        break;
      }
      break;
    case GRN_LOADER_STRING_ESC :
      switch (c) {
      case 'b' :
        GRN_TEXT_PUTC(ctx, loader->last, '\b');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'f' :
        GRN_TEXT_PUTC(ctx, loader->last, '\f');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'n' :
        GRN_TEXT_PUTC(ctx, loader->last, '\n');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'r' :
        GRN_TEXT_PUTC(ctx, loader->last, '\r');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 't' :
        GRN_TEXT_PUTC(ctx, loader->last, '\t');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'u' :
        loader->stat = GRN_LOADER_UNICODE0;
        break;
      default :
        GRN_TEXT_PUTC(ctx, loader->last, c);
        loader->stat = GRN_LOADER_STRING;
        break;
      }
      str++;
      break;
    case GRN_LOADER_UNICODE0 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar = (c - '0') * 0x1000;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar = (c - 'a' + 10) * 0x1000;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar = (c - 'A' + 10) * 0x1000;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE1;
      str++;
      break;
    case GRN_LOADER_UNICODE1 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0') * 0x100;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10) * 0x100;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10) * 0x100;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE2;
      str++;
      break;
    case GRN_LOADER_UNICODE2 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0') * 0x10;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10) * 0x10;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10) * 0x10;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE3;
      str++;
      break;
    case GRN_LOADER_UNICODE3 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0');
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10);
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10);
        break;
      default :
        ;// todo : error
      }
      {
        uint32_t u = loader->unichar;
        if (u < 0x80) {
          GRN_TEXT_PUTC(ctx, loader->last, u);
        } else {
          if (u < 0x800) {
            GRN_TEXT_PUTC(ctx, loader->last, ((u >> 6) & 0x1f) | 0xc0);
          } else {
            GRN_TEXT_PUTC(ctx, loader->last, (u >> 12) | 0xe0);
            GRN_TEXT_PUTC(ctx, loader->last, ((u >> 6) & 0x3f) | 0x80);
          }
          GRN_TEXT_PUTC(ctx, loader->last, (u & 0x3f) | 0x80);
        }
      }
      loader->stat = GRN_LOADER_STRING;
      str++;
      break;
    case GRN_LOADER_END :
      str = se;
      break;
    }
  }
}

#undef JSON_READ_OPEN_BRACKET
#undef JSON_READ_OPEN_BRACE

static grn_rc
parse_load_columns(grn_ctx *ctx, grn_obj *table,
                   const char *str, unsigned int str_size, grn_obj *res)
{
  const char *p = (char *)str, *q, *r, *pe = p + str_size, *tokbuf[256];
  while (p < pe) {
    int i, n = tokenize(p, pe - p, tokbuf, 256, &q);
    for (i = 0; i < n; i++) {
      grn_obj *col;
      r = tokbuf[i];
      while (p < r && (' ' == *p || ',' == *p)) { p++; }
      col = grn_obj_column(ctx, table, p, r - p);
      if (!col) {
        ERR(GRN_INVALID_ARGUMENT, "nonexistent column: <%.*s>", (int)(r - p), p);
        goto exit;
      }
      GRN_PTR_PUT(ctx, res, col);
      p = r;
    }
    p = q;
  }
exit:
  return ctx->rc;
}

static grn_com_addr *addr;

void
grn_load_(grn_ctx *ctx, grn_content_type input_type,
          const char *table, unsigned int table_len,
          const char *columns, unsigned int columns_len,
          const char *values, unsigned int values_len,
          const char *ifexists, unsigned int ifexists_len,
          const char *each, unsigned int each_len,
          uint32_t emit_level)
{
  grn_loader *loader;
  loader = &ctx->impl->loader;
  loader->emit_level = emit_level;
  if (ctx->impl->edge) {
    grn_edge *edge = grn_edges_add_communicator(ctx, addr);
    grn_obj *msg = grn_msg_open(ctx, edge->com, &ctx->impl->edge->send_old);
    /* build msg */
    grn_edge_dispatch(ctx, edge, msg);
  }
  if (table && table_len) {
    grn_ctx_loader_clear(ctx);
    loader->input_type = input_type;
    if (grn_db_check_name(ctx, table, table_len)) {
      GRN_DB_CHECK_NAME_ERR("[table][load]", table, table_len);
      loader->stat = GRN_LOADER_END;
      return;
    }
    loader->table = grn_ctx_get(ctx, table, table_len);
    if (!loader->table) {
      ERR(GRN_INVALID_ARGUMENT, "nonexistent table: <%.*s>", table_len, table);
      loader->stat = GRN_LOADER_END;
      return;
    }
    if (loader->table && columns && columns_len) {
      int i, n_columns;
      grn_obj parsed_columns;

      GRN_PTR_INIT(&parsed_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
      if (parse_load_columns(ctx, loader->table, columns, columns_len,
                             &parsed_columns)) {
        loader->stat = GRN_LOADER_END;
        return;
      }
      n_columns = GRN_BULK_VSIZE(&parsed_columns) / sizeof(grn_obj *);
      for (i = 0; i < n_columns; i++) {
        grn_obj *column;
        column = GRN_PTR_VALUE_AT(&parsed_columns, i);
        if (column->header.type == GRN_ACCESSOR &&
            ((grn_accessor *)column)->action == GRN_ACCESSOR_GET_KEY) {
          loader->key_offset = i;
          grn_obj_unlink(ctx, column);
        } else {
          GRN_PTR_PUT(ctx, &loader->columns, column);
        }
      }
      GRN_OBJ_FIN(ctx, &parsed_columns);
    }
    if (ifexists && ifexists_len) {
      grn_obj *v;
      GRN_EXPR_CREATE_FOR_QUERY(ctx, loader->table, loader->ifexists, v);
      if (loader->ifexists && v) {
        grn_expr_parse(ctx, loader->ifexists, ifexists, ifexists_len,
                       NULL, GRN_OP_EQUAL, GRN_OP_AND,
                       GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
      }
    }
    if (each && each_len) {
      grn_obj *v;
      GRN_EXPR_CREATE_FOR_QUERY(ctx, loader->table, loader->each, v);
      if (loader->each && v) {
        grn_expr_parse(ctx, loader->each, each, each_len,
                       NULL, GRN_OP_EQUAL, GRN_OP_AND,
                       GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
      }
    }
  } else {
    if (!loader->table) {
      ERR(GRN_INVALID_ARGUMENT, "mandatory \"table\" parameter is absent");
      loader->stat = GRN_LOADER_END;
      return;
    }
    input_type = loader->input_type;
  }
  switch (input_type) {
  case GRN_CONTENT_JSON :
    json_read(ctx, loader, values, values_len);
    break;
  case GRN_CONTENT_NONE :
  case GRN_CONTENT_TSV :
  case GRN_CONTENT_XML :
  case GRN_CONTENT_MSGPACK :
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "unsupported input_type");
    // todo
    break;
  }
}

grn_rc
grn_load(grn_ctx *ctx, grn_content_type input_type,
         const char *table, unsigned int table_len,
         const char *columns, unsigned int columns_len,
         const char *values, unsigned int values_len,
         const char *ifexists, unsigned int ifexists_len,
         const char *each, unsigned int each_len)
{
  if (!ctx || !ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return ctx->rc;
  }
  GRN_API_ENTER;
  grn_load_(ctx, input_type, table, table_len,
            columns, columns_len, values, values_len,
            ifexists, ifexists_len, each, each_len, 1);
  GRN_API_RETURN(ctx->rc);
}
