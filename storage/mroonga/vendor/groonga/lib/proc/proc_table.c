/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#include "../grn_proc.h"

#include "../grn_ctx.h"
#include "../grn_str.h"
#include "../grn_db.h"

#include <groonga/plugin.h>

static grn_table_flags
command_table_create_parse_flags(grn_ctx *ctx,
                                 const char *nptr,
                                 const char *end)
{
  grn_table_flags flags = 0;
  while (nptr < end) {
    size_t name_size;

    if (*nptr == '|' || *nptr == ' ') {
      nptr += 1;
      continue;
    }

#define CHECK_FLAG(name)                                                \
    name_size = strlen(#name);                                          \
    if ((unsigned long) (end - nptr) >= (unsigned long) name_size &&    \
        memcmp(nptr, #name, name_size) == 0) {                          \
      flags |= GRN_OBJ_ ## name;                                        \
      nptr += name_size;                                                \
      continue;                                                         \
    }

    CHECK_FLAG(TABLE_HASH_KEY);
    CHECK_FLAG(TABLE_PAT_KEY);
    CHECK_FLAG(TABLE_DAT_KEY);
    CHECK_FLAG(TABLE_NO_KEY);
    CHECK_FLAG(KEY_NORMALIZE);
    CHECK_FLAG(KEY_WITH_SIS);
    CHECK_FLAG(KEY_LARGE);

#undef CHECK_FLAG

    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][create][flags] unknown flag: <%.*s>",
                     (int)(end - nptr), nptr);
    return 0;
  }
  return flags;
}

static grn_bool
grn_proc_table_set_token_filters_put(grn_ctx *ctx,
                                     grn_obj *token_filters,
                                     const char *token_filter_name,
                                     int token_filter_name_length)
{
  grn_obj *token_filter;

  token_filter = grn_ctx_get(ctx,
                             token_filter_name,
                             token_filter_name_length);
  if (token_filter) {
    GRN_PTR_PUT(ctx, token_filters, token_filter);
    return GRN_TRUE;
  } else {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][create][token-filter] "
                     "nonexistent token filter: <%.*s>",
                     token_filter_name_length, token_filter_name);
    return GRN_FALSE;
  }
}

static grn_bool
grn_proc_table_set_token_filters_fill(grn_ctx *ctx,
                                      grn_obj *token_filters,
                                      grn_obj *token_filter_names)
{
  const char *start, *current, *end;
  const char *name_start, *name_end;
  const char *last_name_end;

  start = GRN_TEXT_VALUE(token_filter_names);
  end = start + GRN_TEXT_LEN(token_filter_names);
  current = start;
  name_start = NULL;
  name_end = NULL;
  last_name_end = start;
  while (current < end) {
    switch (current[0]) {
    case ' ' :
      if (name_start && !name_end) {
        name_end = current;
      }
      break;
    case ',' :
      if (!name_start) {
        goto break_loop;
      }
      if (!name_end) {
        name_end = current;
      }
      if (!grn_proc_table_set_token_filters_put(ctx,
                                                token_filters,
                                                name_start,
                                                name_end - name_start)) {
        return GRN_FALSE;
      }
      last_name_end = name_end + 1;
      name_start = NULL;
      name_end = NULL;
      break;
    default :
      if (!name_start) {
        name_start = current;
      }
      break;
    }
    current++;
  }

break_loop:
  if (!name_start) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][create][token-filter] empty token filter name: "
                     "<%.*s|%.*s|%.*s>",
                     (int)(last_name_end - start), start,
                     (int)(current - last_name_end), last_name_end,
                     (int)(end - current), current);
    return GRN_FALSE;
  }

  if (!name_end) {
    name_end = current;
  }
  grn_proc_table_set_token_filters_put(ctx,
                                       token_filters,
                                       name_start,
                                       name_end - name_start);

  return GRN_TRUE;
}

grn_bool
grn_proc_table_set_token_filters(grn_ctx *ctx,
                                 grn_obj *table,
                                 grn_obj *token_filter_names)
{
  grn_bool succeeded = GRN_FALSE;
  grn_obj token_filters;

  if (GRN_TEXT_LEN(token_filter_names) == 0) {
    return GRN_TRUE;
  }

  GRN_PTR_INIT(&token_filters, GRN_OBJ_VECTOR, 0);
  succeeded = grn_proc_table_set_token_filters_fill(ctx,
                                                    &token_filters,
                                                    token_filter_names);
  if (succeeded) {
    grn_obj_set_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
  }
  grn_obj_unlink(ctx, &token_filters);

  return succeeded;
}

static grn_obj *
command_table_create(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_obj *name;
  grn_obj *flags_raw;
  grn_obj *key_type_name;
  grn_obj *value_type_name;
  grn_obj *default_tokenizer_name;
  grn_obj *normalizer_name;
  grn_obj *token_filters_name;
  grn_obj *table;
  const char *rest;
  grn_table_flags flags;

  name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
  flags_raw = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  key_type_name = grn_plugin_proc_get_var(ctx, user_data, "key_type", -1);
  value_type_name = grn_plugin_proc_get_var(ctx, user_data, "value_type", -1);
  default_tokenizer_name =
    grn_plugin_proc_get_var(ctx, user_data, "default_tokenizer", -1);
  normalizer_name =
    grn_plugin_proc_get_var(ctx, user_data, "normalizer", -1);
  token_filters_name =
    grn_plugin_proc_get_var(ctx, user_data, "token_filters", -1);

  flags = grn_atoi(GRN_TEXT_VALUE(flags_raw),
                   GRN_BULK_CURR(flags_raw),
                   &rest);

  if (GRN_TEXT_VALUE(flags_raw) == rest) {
    flags = command_table_create_parse_flags(ctx,
                                             GRN_TEXT_VALUE(flags_raw),
                                             GRN_BULK_CURR(flags_raw));
    if (ctx->rc) { goto exit; }
  }

  if (GRN_TEXT_LEN(name) == 0) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][create] should not create anonymous table");
    goto exit;
  }

  {
    grn_obj *key_type = NULL;
    grn_obj *value_type = NULL;

    if (GRN_TEXT_LEN(key_type_name) > 0) {
      key_type = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(key_type_name),
                             GRN_TEXT_LEN(key_type_name));
      if (!key_type) {
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "[table][create] "
                         "key type doesn't exist: <%.*s> (%.*s)",
                         (int)GRN_TEXT_LEN(name),
                         GRN_TEXT_VALUE(name),
                         (int)GRN_TEXT_LEN(key_type_name),
                         GRN_TEXT_VALUE(key_type_name));
        goto exit;
      }
    }

    if (GRN_TEXT_LEN(value_type_name) > 0) {
      value_type = grn_ctx_get(ctx,
                               GRN_TEXT_VALUE(value_type_name),
                               GRN_TEXT_LEN(value_type_name));
      if (!value_type) {
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "[table][create] "
                         "value type doesn't exist: <%.*s> (%.*s)",
                         (int)GRN_TEXT_LEN(name),
                         GRN_TEXT_VALUE(name),
                         (int)GRN_TEXT_LEN(value_type_name),
                         GRN_TEXT_VALUE(value_type_name));
        goto exit;
      }
    }

    flags |= GRN_OBJ_PERSISTENT;
    table = grn_table_create(ctx,
                             GRN_TEXT_VALUE(name),
                             GRN_TEXT_LEN(name),
                             NULL, flags,
                             key_type,
                             value_type);
    if (!table) {
      goto exit;
    }

    if (GRN_TEXT_LEN(default_tokenizer_name) > 0) {
      grn_obj *default_tokenizer;

      default_tokenizer =
        grn_ctx_get(ctx,
                    GRN_TEXT_VALUE(default_tokenizer_name),
                    GRN_TEXT_LEN(default_tokenizer_name));
      if (!default_tokenizer) {
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "[table][create][%.*s] unknown tokenizer: <%.*s>",
                         (int)GRN_TEXT_LEN(name),
                         GRN_TEXT_VALUE(name),
                         (int)GRN_TEXT_LEN(default_tokenizer_name),
                         GRN_TEXT_VALUE(default_tokenizer_name));
        grn_obj_remove(ctx, table);
        goto exit;
      }
      grn_obj_set_info(ctx, table,
                       GRN_INFO_DEFAULT_TOKENIZER,
                       default_tokenizer);
    }

    if (GRN_TEXT_LEN(normalizer_name) > 0) {
      grn_obj *normalizer;

      normalizer =
        grn_ctx_get(ctx,
                    GRN_TEXT_VALUE(normalizer_name),
                    GRN_TEXT_LEN(normalizer_name));
      if (!normalizer) {
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "[table][create][%.*s] unknown normalizer: <%.*s>",
                         (int)GRN_TEXT_LEN(name),
                         GRN_TEXT_VALUE(name),
                         (int)GRN_TEXT_LEN(normalizer_name),
                         GRN_TEXT_VALUE(normalizer_name));
        grn_obj_remove(ctx, table);
        goto exit;
      }
      grn_obj_set_info(ctx, table, GRN_INFO_NORMALIZER, normalizer);
    }

    if (!grn_proc_table_set_token_filters(ctx, table, token_filters_name)) {
      grn_obj_remove(ctx, table);
      goto exit;
    }

    grn_obj_unlink(ctx, table);
  }

exit :
  grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);
  return NULL;
}

void
grn_proc_init_table_create(grn_ctx *ctx)
{
  grn_expr_var vars[7];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "flags", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "key_type", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "value_type", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "default_tokenizer", -1);
  grn_plugin_expr_var_init(ctx, &(vars[5]), "normalizer", -1);
  grn_plugin_expr_var_init(ctx, &(vars[6]), "token_filters", -1);
  grn_plugin_command_create(ctx,
                            "table_create", -1,
                            command_table_create,
                            7,
                            vars);
}

static int
output_table_info(grn_ctx *ctx, grn_obj *table)
{
  grn_id id;
  grn_obj o;
  const char *path;
  grn_table_flags flags;
  grn_obj *default_tokenizer;
  grn_obj *normalizer;
  grn_obj *token_filters;

  id = grn_obj_id(ctx, table);
  path = grn_obj_path(ctx, table);
  GRN_TEXT_INIT(&o, 0);
  grn_ctx_output_array_open(ctx, "TABLE", 8);
  grn_ctx_output_int64(ctx, id);
  grn_proc_output_object_id_name(ctx, id);
  grn_ctx_output_cstr(ctx, path);
  GRN_BULK_REWIND(&o);

  grn_table_get_info(ctx, table,
                     &flags,
                     NULL,
                     &default_tokenizer,
                     &normalizer,
                     &token_filters);
  grn_dump_table_create_flags(ctx, flags, &o);
  grn_ctx_output_obj(ctx, &o, NULL);
  grn_proc_output_object_id_name(ctx, table->header.domain);
  grn_proc_output_object_id_name(ctx, grn_obj_get_range(ctx, table));
  grn_proc_output_object_name(ctx, default_tokenizer);
  grn_proc_output_object_name(ctx, normalizer);
  grn_ctx_output_array_close(ctx);
  GRN_OBJ_FIN(ctx, &o);
  return 1;
}

static grn_obj *
command_table_list(grn_ctx *ctx, int nargs, grn_obj **args,
                   grn_user_data *user_data)
{
  grn_obj *db;
  grn_obj tables;
  int n_top_level_elements;
  int n_elements_for_header = 1;
  int n_tables;
  int i;

  db = grn_ctx_db(ctx);

  {
    grn_table_cursor *cursor;
    grn_id id;
    grn_obj *prefix;
    const void *min = NULL;
    unsigned int min_size = 0;
    int flags = 0;

    prefix = grn_plugin_proc_get_var(ctx, user_data, "prefix", -1);
    if (GRN_TEXT_LEN(prefix) > 0) {
      min = GRN_TEXT_VALUE(prefix);
      min_size = GRN_TEXT_LEN(prefix);
      flags |= GRN_CURSOR_PREFIX;
    }
    cursor = grn_table_cursor_open(ctx, db,
                                   min, min_size,
                                   NULL, 0,
                                   0, -1, flags);
    if (!cursor) {
      return NULL;
    }

    GRN_PTR_INIT(&tables, GRN_OBJ_VECTOR, GRN_ID_NIL);
    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_obj *object;
      const char *name;
      void *key;
      int i, key_size;
      grn_bool have_period = GRN_FALSE;

      key_size = grn_table_cursor_get_key(ctx, cursor, &key);
      name = key;
      for (i = 0; i < key_size; i++) {
        if (name[i] == '.') {
          have_period = GRN_TRUE;
          break;
        }
      }
      if (have_period) {
        continue;
      }

      object = grn_ctx_at(ctx, id);
      if (object) {
        if (grn_obj_is_table(ctx, object)) {
          GRN_PTR_PUT(ctx, &tables, object);
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
  }
  n_tables = GRN_BULK_VSIZE(&tables) / sizeof(grn_obj *);
  n_top_level_elements = n_elements_for_header + n_tables;
  grn_ctx_output_array_open(ctx, "TABLE_LIST", n_top_level_elements);

  grn_ctx_output_array_open(ctx, "HEADER", 8);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "id");
  grn_ctx_output_cstr(ctx, "UInt32");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "name");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "path");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "flags");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "domain");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "range");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "default_tokenizer");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_open(ctx, "PROPERTY", 2);
  grn_ctx_output_cstr(ctx, "normalizer");
  grn_ctx_output_cstr(ctx, "ShortText");
  grn_ctx_output_array_close(ctx);
  grn_ctx_output_array_close(ctx);

  for (i = 0; i < n_tables; i++) {
    grn_obj *table = GRN_PTR_VALUE_AT(&tables, i);
    output_table_info(ctx, table);
    grn_obj_unlink(ctx, table);
  }
  GRN_OBJ_FIN(ctx, &tables);

  grn_ctx_output_array_close(ctx);

  return NULL;
}

void
grn_proc_init_table_list(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "prefix", -1);
  grn_plugin_command_create(ctx,
                            "table_list", -1,
                            command_table_list,
                            1,
                            vars);
}

static grn_obj *
command_table_remove(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_obj *name;
  grn_obj *table;
  grn_bool dependent;

  name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
  dependent = grn_plugin_proc_get_var_bool(ctx, user_data, "dependent", -1,
                                           GRN_FALSE);
  table = grn_ctx_get(ctx,
                      GRN_TEXT_VALUE(name),
                      GRN_TEXT_LEN(name));
  if (!table) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][remove] table isn't found: <%.*s>",
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name));
    grn_ctx_output_bool(ctx, GRN_FALSE);
    return NULL;
  }

  if (!grn_obj_is_table(ctx, table)) {
    const char *type_name;
    type_name = grn_obj_type_to_string(table->header.type);
    grn_obj_unlink(ctx, table);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][remove] not table: <%.*s>: <%s>",
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name),
                     type_name);
    grn_ctx_output_bool(ctx, GRN_FALSE);
    return NULL;
  }

  if (dependent) {
    grn_obj_remove_dependent(ctx, table);
  } else {
    grn_obj_remove(ctx, table);
  }
  grn_ctx_output_bool(ctx, !ctx->rc);
  return NULL;
}

void
grn_proc_init_table_remove(grn_ctx *ctx)
{
  grn_expr_var vars[2];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "dependent", -1);
  grn_plugin_command_create(ctx,
                            "table_remove", -1,
                            command_table_remove,
                            2,
                            vars);
}

static grn_obj *
command_table_rename(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *name;
  grn_obj *new_name;
  grn_obj *table = NULL;

  name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
  new_name = grn_plugin_proc_get_var(ctx, user_data, "new_name", -1);
  if (GRN_TEXT_LEN(name) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    GRN_PLUGIN_ERROR(ctx, rc, "[table][rename] table name isn't specified");
    goto exit;
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name));
  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "[table][rename] table isn't found: <%.*s>",
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name));
    goto exit;
  }
  if (GRN_TEXT_LEN(new_name) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "[table][rename] new table name isn't specified: <%.*s>",
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name));
    goto exit;
  }
  rc = grn_table_rename(ctx, table,
                        GRN_TEXT_VALUE(new_name),
                        GRN_TEXT_LEN(new_name));
  if (rc != GRN_SUCCESS && ctx->rc == GRN_SUCCESS) {
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "[table][rename] failed to rename: <%.*s> -> <%.*s>",
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name),
                     (int)GRN_TEXT_LEN(new_name),
                     GRN_TEXT_VALUE(new_name));
  }
exit :
  grn_ctx_output_bool(ctx, !rc);
  if (table) { grn_obj_unlink(ctx, table); }
  return NULL;
}

void
grn_proc_init_table_rename(grn_ctx *ctx)
{
  grn_expr_var vars[2];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "new_name", -1);
  grn_plugin_command_create(ctx,
                            "table_rename", -1,
                            command_table_rename,
                            2,
                            vars);
}

static grn_rc
command_table_copy_resolve_target(grn_ctx *ctx,
                                  const char *label,
                                  grn_obj *name,
                                  grn_obj **table)
{
  if (GRN_TEXT_LEN(name) == 0) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][copy] %s name isn't specified",
                     label);
    return ctx->rc;
  }
  *table = grn_ctx_get(ctx,
                       GRN_TEXT_VALUE(name),
                       GRN_TEXT_LEN(name));
  if (!*table) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[table][copy] %s table isn't found: <%.*s>",
                     label,
                     (int)GRN_TEXT_LEN(name),
                     GRN_TEXT_VALUE(name));
    return ctx->rc;
  }

  return ctx->rc;
}

static void
command_table_copy_same_key_type(grn_ctx *ctx,
                                 grn_obj *from_table,
                                 grn_obj *to_table,
                                 grn_obj *from_name,
                                 grn_obj *to_name)
{
  GRN_TABLE_EACH_BEGIN_FLAGS(ctx, from_table, cursor, from_id,
                             GRN_CURSOR_BY_KEY | GRN_CURSOR_ASCENDING) {
    void   *key;
    int     key_size;
    grn_id  to_id;

    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
    to_id    = grn_table_add(ctx, to_table, key, key_size, NULL);
    if (to_id == GRN_ID_NIL) {
      grn_obj key_buffer;
      grn_obj inspected_key;
      if (from_table->header.domain == GRN_DB_SHORT_TEXT) {
        GRN_SHORT_TEXT_INIT(&key_buffer, 0);
      } else {
        GRN_VALUE_FIX_SIZE_INIT(&key_buffer, 0, from_table->header.domain);
      }
      grn_bulk_write(ctx, &key_buffer, key, key_size);
      GRN_TEXT_INIT(&inspected_key, 0);
      grn_inspect(ctx, &inspected_key, &key_buffer);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[table][copy] failed to copy key: <%.*s>: "
                       "<%.*s> -> <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected_key),
                       GRN_TEXT_VALUE(&inspected_key),
                       (int)GRN_TEXT_LEN(from_name),
                       GRN_TEXT_VALUE(from_name),
                       (int)GRN_TEXT_LEN(to_name),
                       GRN_TEXT_VALUE(to_name));
      GRN_OBJ_FIN(ctx, &inspected_key);
      GRN_OBJ_FIN(ctx, &key_buffer);
      break;
    }
  } GRN_TABLE_EACH_END(ctx, cursor);
}

static void
command_table_copy_different(grn_ctx *ctx,
                              grn_obj *from_table,
                              grn_obj *to_table,
                              grn_obj *from_name,
                              grn_obj *to_name)
{
  grn_obj from_key_buffer;
  grn_obj to_key_buffer;

  if (from_table->header.domain == GRN_DB_SHORT_TEXT) {
    GRN_SHORT_TEXT_INIT(&from_key_buffer, 0);
  } else {
    GRN_VALUE_FIX_SIZE_INIT(&from_key_buffer, 0, from_table->header.domain);
  }
  if (to_table->header.domain == GRN_DB_SHORT_TEXT) {
    GRN_SHORT_TEXT_INIT(&to_key_buffer, 0);
  } else {
    GRN_VALUE_FIX_SIZE_INIT(&to_key_buffer, 0, to_table->header.domain);
  }

  GRN_TABLE_EACH_BEGIN_FLAGS(ctx, from_table, cursor, from_id,
                             GRN_CURSOR_BY_KEY | GRN_CURSOR_ASCENDING) {
    void *key;
    int key_size;
    grn_rc cast_rc;
    grn_id to_id;

    GRN_BULK_REWIND(&from_key_buffer);
    GRN_BULK_REWIND(&to_key_buffer);

    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
    grn_bulk_write(ctx, &from_key_buffer, key, key_size);
    cast_rc = grn_obj_cast(ctx, &from_key_buffer, &to_key_buffer, GRN_FALSE);
    if (cast_rc != GRN_SUCCESS) {
      grn_obj *to_key_type;
      grn_obj inspected_key;
      grn_obj inspected_to_key_type;

      to_key_type = grn_ctx_at(ctx, to_table->header.domain);
      GRN_TEXT_INIT(&inspected_key, 0);
      GRN_TEXT_INIT(&inspected_to_key_type, 0);
      grn_inspect(ctx, &inspected_key, &from_key_buffer);
      grn_inspect(ctx, &inspected_to_key_type, to_key_type);
      ERR(cast_rc,
          "[table][copy] failed to cast key: <%.*s> -> %.*s: "
          "<%.*s> -> <%.*s>",
          (int)GRN_TEXT_LEN(&inspected_key),
          GRN_TEXT_VALUE(&inspected_key),
          (int)GRN_TEXT_LEN(&inspected_to_key_type),
          GRN_TEXT_VALUE(&inspected_to_key_type),
          (int)GRN_TEXT_LEN(from_name),
          GRN_TEXT_VALUE(from_name),
          (int)GRN_TEXT_LEN(to_name),
          GRN_TEXT_VALUE(to_name));
      GRN_OBJ_FIN(ctx, &inspected_key);
      GRN_OBJ_FIN(ctx, &inspected_to_key_type);
      break;
    }

    to_id = grn_table_add(ctx, to_table,
                          GRN_BULK_HEAD(&to_key_buffer),
                          GRN_BULK_VSIZE(&to_key_buffer),
                          NULL);
    if (to_id == GRN_ID_NIL) {
      grn_obj inspected_from_key;
      grn_obj inspected_to_key;
      GRN_TEXT_INIT(&inspected_from_key, 0);
      GRN_TEXT_INIT(&inspected_to_key, 0);
      grn_inspect(ctx, &inspected_from_key, &from_key_buffer);
      grn_inspect(ctx, &inspected_to_key, &to_key_buffer);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[table][copy] failed to copy key: <%.*s> -> <%.*s>: "
                       "<%.*s> -> <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected_from_key),
                       GRN_TEXT_VALUE(&inspected_from_key),
                       (int)GRN_TEXT_LEN(&inspected_to_key),
                       GRN_TEXT_VALUE(&inspected_to_key),
                       (int)GRN_TEXT_LEN(from_name),
                       GRN_TEXT_VALUE(from_name),
                       (int)GRN_TEXT_LEN(to_name),
                       GRN_TEXT_VALUE(to_name));
      GRN_OBJ_FIN(ctx, &inspected_from_key);
      GRN_OBJ_FIN(ctx, &inspected_to_key);
      break;
    }
  } GRN_TABLE_EACH_END(ctx, cursor);
  GRN_OBJ_FIN(ctx, &from_key_buffer);
  GRN_OBJ_FIN(ctx, &to_key_buffer);
}

static grn_obj *
command_table_copy(grn_ctx *ctx,
                   int nargs,
                   grn_obj **args,
                   grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *from_table = NULL;
  grn_obj *to_table = NULL;
  grn_obj *from_name;
  grn_obj *to_name;

  from_name = grn_plugin_proc_get_var(ctx, user_data, "from_name", -1);
  to_name   = grn_plugin_proc_get_var(ctx, user_data, "to_name", -1);

  rc = command_table_copy_resolve_target(ctx, "from", from_name, &from_table);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }
  rc = command_table_copy_resolve_target(ctx, "to", to_name, &to_table);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  if (from_table->header.type == GRN_TABLE_NO_KEY ||
      to_table->header.type == GRN_TABLE_NO_KEY) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_OPERATION_NOT_SUPPORTED,
                     "[table][copy] copy from/to TABLE_NO_KEY isn't supported: "
                     "<%.*s> -> <%.*s>",
                     (int)GRN_TEXT_LEN(from_name),
                     GRN_TEXT_VALUE(from_name),
                     (int)GRN_TEXT_LEN(to_name),
                     GRN_TEXT_VALUE(to_name));
    rc = ctx->rc;
    goto exit;
  }

  if (from_table == to_table) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_OPERATION_NOT_SUPPORTED,
                     "[table][copy] from table and to table is the same: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(from_name),
                     GRN_TEXT_VALUE(from_name));
    rc = ctx->rc;
    goto exit;
  }

  if (from_table->header.domain == to_table->header.domain) {
    command_table_copy_same_key_type(ctx,
                                     from_table, to_table,
                                     from_name, to_name);
  } else {
    command_table_copy_different(ctx,
                                 from_table, to_table,
                                 from_name, to_name);
  }

exit :
  grn_ctx_output_bool(ctx, rc == GRN_SUCCESS);

  if (to_table) {
    grn_obj_unlink(ctx, to_table);
  }
  if (from_table) {
    grn_obj_unlink(ctx, from_table);
  }

  return NULL;
}

void
grn_proc_init_table_copy(grn_ctx *ctx)
{
  grn_expr_var vars[2];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "from_name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "to_name", -1);
  grn_plugin_command_create(ctx,
                            "table_copy", -1,
                            command_table_copy,
                            2,
                            vars);
}
