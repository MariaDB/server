/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2017 Brazil

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
#include "grn_index_column.h"
#include "grn_pat.h"
#include "grn_dat.h"
#include "grn_ii.h"

grn_bool
grn_obj_is_true(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  switch (obj->header.type) {
  case GRN_BULK :
    switch (obj->header.domain) {
    case GRN_DB_BOOL :
      return GRN_BOOL_VALUE(obj);
      break;
    case GRN_DB_INT32 :
      return GRN_INT32_VALUE(obj) != 0;
      break;
    case GRN_DB_UINT32 :
      return GRN_UINT32_VALUE(obj) != 0;
      break;
    case GRN_DB_FLOAT : {
      double float_value;
      float_value = GRN_FLOAT_VALUE(obj);
      return (float_value < -DBL_EPSILON ||
              DBL_EPSILON < float_value);
      break;
    }
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      return GRN_TEXT_LEN(obj) != 0;
      break;
    default :
      return GRN_FALSE;
      break;
    }
    break;
  case GRN_VECTOR :
    return GRN_TRUE;
    break;
  default :
    return  GRN_FALSE;
    break;
  }
}

grn_bool
grn_obj_is_builtin(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id;

  if (!obj) { return GRN_FALSE; }

  id = grn_obj_id(ctx, obj);
  return grn_id_is_builtin(ctx, id);
}

grn_bool
grn_obj_is_bulk(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_BULK;
}

grn_bool
grn_obj_is_text_family_bulk(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_bulk(ctx, obj)) {
    return GRN_FALSE;
  }

  return GRN_TYPE_IS_TEXT_FAMILY(obj->header.domain);
}

grn_bool
grn_obj_is_table(grn_ctx *ctx, grn_obj *obj)
{
  grn_bool is_table = GRN_FALSE;

  if (!obj) {
    return GRN_FALSE;
  }

  switch (obj->header.type) {
  case GRN_TABLE_NO_KEY :
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    is_table = GRN_TRUE;
  default :
    break;
  }

  return is_table;
}

grn_bool
grn_obj_is_column(grn_ctx *ctx, grn_obj *obj)
{
  grn_bool is_column = GRN_FALSE;

  if (!obj) {
    return GRN_FALSE;
  }

  switch (obj->header.type) {
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_INDEX :
    is_column = GRN_TRUE;
  default :
    break;
  }

  return is_column;
}

grn_bool
grn_obj_is_scalar_column(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_column(ctx, obj)) {
    return GRN_FALSE;
  }

  return (obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_SCALAR;
}

grn_bool
grn_obj_is_vector_column(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_column(ctx, obj)) {
    return GRN_FALSE;
  }

  return ((obj->header.type == GRN_COLUMN_VAR_SIZE) &&
          ((obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) ==
           GRN_OBJ_COLUMN_VECTOR));
}

grn_bool
grn_obj_is_weight_vector_column(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_vector_column(ctx, obj)) {
    return GRN_FALSE;
  }

  return (obj->header.flags & GRN_OBJ_WITH_WEIGHT) == GRN_OBJ_WITH_WEIGHT;
}

grn_bool
grn_obj_is_reference_column(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj *range;

  if (!grn_obj_is_column(ctx, obj)) {
    return GRN_FALSE;
  }

  range = grn_ctx_at(ctx, grn_obj_get_range(ctx, obj));
  if (!range) {
    return GRN_FALSE;
  }

  switch (range->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
  case GRN_TABLE_NO_KEY:
    return GRN_TRUE;
  default:
    return GRN_FALSE;
  }
}

grn_bool
grn_obj_is_data_column(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_column(ctx, obj)) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_COLUMN_FIX_SIZE ||
    obj->header.type == GRN_COLUMN_VAR_SIZE;
}

grn_bool
grn_obj_is_index_column(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_column(ctx, obj)) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_COLUMN_INDEX;
}

grn_bool
grn_obj_is_accessor(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_ACCESSOR;
}

grn_bool
grn_obj_is_key_accessor(grn_ctx *ctx, grn_obj *obj)
{
  grn_accessor *accessor;

  if (!grn_obj_is_accessor(ctx, obj)) {
    return GRN_FALSE;
  }

  accessor = (grn_accessor *)obj;
  if (accessor->next) {
    return GRN_FALSE;
  }

  return accessor->action == GRN_ACCESSOR_GET_KEY;
}

grn_bool
grn_obj_is_type(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_TYPE;
}

grn_bool
grn_obj_is_text_family_type(grn_ctx *ctx, grn_obj *obj)
{
  if (!grn_obj_is_type(ctx, obj)) {
    return GRN_FALSE;
  }

  return GRN_TYPE_IS_TEXT_FAMILY(grn_obj_id(ctx, obj));
}

grn_bool
grn_obj_is_proc(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_PROC;
}

grn_bool
grn_obj_is_tokenizer_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_TOKENIZER;
}

grn_bool
grn_obj_is_function_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_FUNCTION;
}

grn_bool
grn_obj_is_selector_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_function_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->callbacks.function.selector != NULL;
}

grn_bool
grn_obj_is_selector_only_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_selector_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->funcs[PROC_INIT] == NULL;
}

grn_bool
grn_obj_is_normalizer_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_NORMALIZER;
}

grn_bool
grn_obj_is_token_filter_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_TOKEN_FILTER;
}

grn_bool
grn_obj_is_scorer_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_SCORER;
}

grn_bool
grn_obj_is_window_function_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_WINDOW_FUNCTION;
}

grn_bool
grn_obj_is_expr(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_EXPR;
}

static void
grn_db_reindex(grn_ctx *ctx, grn_obj *db)
{
  grn_table_cursor *cursor;
  grn_id id;

  cursor = grn_table_cursor_open(ctx, db,
                                 NULL, 0, NULL, 0,
                                 0, -1,
                                 GRN_CURSOR_BY_ID);
  if (!cursor) {
    return;
  }

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
      grn_obj_reindex(ctx, object);
      break;
    default:
      break;
    }

    grn_obj_unlink(ctx, object);

    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  }
  grn_table_cursor_close(ctx, cursor);
}

static void
grn_table_reindex(grn_ctx *ctx, grn_obj *table)
{
  grn_hash *columns;

  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
  if (!columns) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[table][reindex] failed to create a table to store columns");
    return;
  }

  if (grn_table_columns(ctx, table, "", 0, (grn_obj *)columns) > 0) {
    grn_id *key;
    GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
      grn_obj *column = grn_ctx_at(ctx, *key);
      if (column && column->header.type == GRN_COLUMN_INDEX) {
        grn_obj_reindex(ctx, column);
      }
    });
  }
  grn_hash_close(ctx, columns);
}

static void
grn_data_column_reindex(grn_ctx *ctx, grn_obj *data_column)
{
  grn_hook *hooks;

  for (hooks = DB_OBJ(data_column)->hooks[GRN_HOOK_SET];
       hooks;
       hooks = hooks->next) {
    grn_obj_default_set_value_hook_data *data = (void *)GRN_NEXT_ADDR(hooks);
    grn_obj *target = grn_ctx_at(ctx, data->target);
    if (target->header.type != GRN_COLUMN_INDEX) {
      continue;
    }
    grn_obj_reindex(ctx, target);
    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
  }
}

grn_rc
grn_obj_reindex(grn_ctx *ctx, grn_obj *obj)
{
  GRN_API_ENTER;

  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT, "[object][reindex] object must not be NULL");
    GRN_API_RETURN(ctx->rc);
  }

  switch (obj->header.type) {
  case GRN_DB :
    grn_db_reindex(ctx, obj);
    break;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    grn_table_reindex(ctx, obj);
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    grn_data_column_reindex(ctx, obj);
    break;
  case GRN_COLUMN_INDEX :
    grn_index_column_rebuild(ctx, obj);
    break;
  default :
    {
      grn_obj type_name;
      GRN_TEXT_INIT(&type_name, 0);
      grn_inspect_type(ctx, &type_name, obj->header.type);
      ERR(GRN_INVALID_ARGUMENT,
          "[object][reindex] object must be TABLE_HASH_KEY, "
          "TABLE_PAT_KEY, TABLE_DAT_KEY or COLUMN_INDEX: <%.*s>",
          (int)GRN_TEXT_LEN(&type_name),
          GRN_TEXT_VALUE(&type_name));
      GRN_OBJ_FIN(ctx, &type_name);
      GRN_API_RETURN(ctx->rc);
    }
    break;
  }

  GRN_API_RETURN(ctx->rc);
}

const char *
grn_obj_type_to_string(uint8_t type)
{
  switch (type) {
  case GRN_VOID :
    return "void";
  case GRN_BULK :
    return "bulk";
  case GRN_PTR :
    return "ptr";
  case GRN_UVECTOR :
    return "uvector";
  case GRN_PVECTOR :
    return "pvector";
  case GRN_VECTOR :
    return "vector";
  case GRN_MSG :
    return "msg";
  case GRN_QUERY :
    return "query";
  case GRN_ACCESSOR :
    return "accessor";
  case GRN_SNIP :
    return "snip";
  case GRN_PATSNIP :
    return "patsnip";
  case GRN_STRING :
    return "string";
  case GRN_CURSOR_TABLE_HASH_KEY :
    return "cursor:table:hash_key";
  case GRN_CURSOR_TABLE_PAT_KEY :
    return "cursor:table:pat_key";
  case GRN_CURSOR_TABLE_DAT_KEY :
    return "cursor:table:dat_key";
  case GRN_CURSOR_TABLE_NO_KEY :
    return "cursor:table:no_key";
  case GRN_CURSOR_COLUMN_INDEX :
    return "cursor:column:index";
  case GRN_CURSOR_COLUMN_GEO_INDEX :
    return "cursor:column:geo_index";
  case GRN_CURSOR_CONFIG :
    return "cursor:config";
  case GRN_TYPE :
    return "type";
  case GRN_PROC :
    return "proc";
  case GRN_EXPR :
    return "expr";
  case GRN_TABLE_HASH_KEY :
    return "table:hash_key";
  case GRN_TABLE_PAT_KEY :
    return "table:pat_key";
  case GRN_TABLE_DAT_KEY :
    return "table:dat_key";
  case GRN_TABLE_NO_KEY :
    return "table:no_key";
  case GRN_DB :
    return "db";
  case GRN_COLUMN_FIX_SIZE :
    return "column:fix_size";
  case GRN_COLUMN_VAR_SIZE :
    return "column:var_size";
  case GRN_COLUMN_INDEX :
    return "column:index";
  default :
    return "unknown";
  }
}

grn_bool
grn_obj_name_is_column(grn_ctx *ctx, const char *name, int name_len)
{
  if (!name) {
    return GRN_FALSE;
  }

  if (name_len < 0) {
    name_len = strlen(name);
  }

  return memchr(name, GRN_DB_DELIMITER, name_len) != NULL;
}

grn_io *
grn_obj_get_io(grn_ctx *ctx, grn_obj *obj)
{
  grn_io *io = NULL;

  if (!obj) {
    return NULL;
  }

  if (obj->header.type == GRN_DB) {
    obj = ((grn_db *)obj)->keys;
  }

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

  return io;
}

size_t
grn_obj_get_disk_usage(grn_ctx *ctx, grn_obj *obj)
{
  size_t usage = 0;

  GRN_API_ENTER;

  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT, "[object][disk-usage] object must not be NULL");
    GRN_API_RETURN(0);
  }

  switch (obj->header.type) {
  case GRN_DB :
    {
      grn_db *db = (grn_db *)obj;
      usage = grn_obj_get_disk_usage(ctx, db->keys);
      if (db->specs) {
        usage += grn_obj_get_disk_usage(ctx, (grn_obj *)(db->specs));
      }
      usage += grn_obj_get_disk_usage(ctx, (grn_obj *)(db->config));
    }
    break;
  case GRN_TABLE_DAT_KEY :
    usage = grn_dat_get_disk_usage(ctx, (grn_dat *)obj);
    break;
  case GRN_COLUMN_INDEX :
    usage = grn_ii_get_disk_usage(ctx, (grn_ii *)obj);
    break;
  default :
    {
      grn_io *io;
      io = grn_obj_get_io(ctx, obj);
      if (io) {
        usage = grn_io_get_disk_usage(ctx, io);
      }
    }
    break;
  }

  GRN_API_RETURN(usage);
}
