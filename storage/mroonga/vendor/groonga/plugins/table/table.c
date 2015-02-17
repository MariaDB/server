/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/* Copyright(C) 2012 Brazil

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

#include <string.h>

#include "grn_ctx.h"
#include "grn_db.h"
#include "grn_output.h"
#include "grn_util.h"
#include <groonga/plugin.h>

#define VAR GRN_PROC_GET_VAR_BY_OFFSET
#define TEXT_VALUE_LEN(x) GRN_TEXT_VALUE(x), GRN_TEXT_LEN(x)

static grn_obj *
grn_ctx_get_table_by_name_or_id(grn_ctx *ctx,
                                const char *name, unsigned int name_len)
{
  grn_obj *table;
  const char *end = name + name_len;
  const char *rest = NULL;
  grn_id id = grn_atoui(name, end, &rest);
  if (rest == end) {
    table = grn_ctx_at(ctx, id);
  } else {
    table = grn_ctx_get(ctx, name, name_len);
  }
  if (!GRN_OBJ_TABLEP(table)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid table name: <%.*s>", name_len, name);
    if (table) {
      grn_obj_unlink(ctx, table);
      table = NULL;
    }
  }
  return table;
}

static void
grn_output_table_name_or_id(grn_ctx *ctx, grn_obj *table)
{
  if (table) {
    if (((grn_db_obj *)table)->id & GRN_OBJ_TMP_OBJECT) {
      GRN_OUTPUT_INT64(((grn_db_obj *)table)->id);
    } else {
      int name_len;
      char name_buf[GRN_TABLE_MAX_KEY_SIZE];
      name_len = grn_obj_name(ctx, table, name_buf, GRN_TABLE_MAX_KEY_SIZE);
      GRN_OUTPUT_STR(name_buf, name_len);
    }
  } else {
    GRN_OUTPUT_INT64(0);
  }
}

static grn_bool
parse_bool_value(grn_ctx *ctx, grn_obj *text)
{
  grn_bool value = GRN_FALSE;
  if (GRN_TEXT_LEN(text) == 3 &&
      memcmp("yes", GRN_TEXT_VALUE(text), 3) == 0) {
    value = GRN_TRUE;
  }
  return value;
}

static grn_operator
parse_set_operator_value(grn_ctx *ctx, grn_obj *text)
{
  grn_operator value = GRN_OP_OR;
  if (GRN_TEXT_LEN(text) == 3) {
    if (memcmp("and", GRN_TEXT_VALUE(text), 3) == 0) {
      value = GRN_OP_AND;
    } else if (memcmp("but", GRN_TEXT_VALUE(text), 3) == 0) {
      value = GRN_OP_AND_NOT;
    }
  } else if (GRN_TEXT_LEN(text) == 6 &&
             memcmp("adjust", GRN_TEXT_VALUE(text), 6) == 0) {
    value = GRN_OP_ADJUST;
  } else if (GRN_TEXT_LEN(text) == 7 &&
             memcmp("and_not", GRN_TEXT_VALUE(text), 7) == 0) {
    value = GRN_OP_AND_NOT;
  }
  return value;
}

static grn_obj *
command_match(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *result_set = NULL;
  grn_obj *table = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(0)));
  if (table) {
    grn_expr_flags flags = GRN_EXPR_SYNTAX_QUERY;
    grn_obj *v, *query, *columns = NULL;
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, query, v);
    if (query) {
      if (GRN_TEXT_LEN(VAR(1))) {
        GRN_EXPR_CREATE_FOR_QUERY(ctx, table, columns, v);
        if (columns) {
          grn_expr_parse(ctx, columns, TEXT_VALUE_LEN(VAR(1)),
                         NULL, GRN_OP_MATCH, GRN_OP_AND,
                         GRN_EXPR_SYNTAX_SCRIPT);
        }
      }
      if (parse_bool_value(ctx, VAR(5))) {
        flags |= GRN_EXPR_ALLOW_COLUMN;
      }
      if (parse_bool_value(ctx, VAR(6))) {
        flags |= GRN_EXPR_ALLOW_PRAGMA;
      }
      grn_expr_parse(ctx, query, TEXT_VALUE_LEN(VAR(2)),
                     columns, GRN_OP_MATCH, GRN_OP_AND, flags);
      if (GRN_TEXT_LEN(VAR(3))) {
        result_set = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(3)));
      } else {
        result_set = grn_table_create(ctx, NULL, 0, NULL,
                                      GRN_TABLE_HASH_KEY|
                                      GRN_OBJ_WITH_SUBREC,
                                      table, NULL);
      }
      if (result_set) {
        grn_table_select(ctx, table, query, result_set,
                         parse_set_operator_value(ctx, VAR(4)));
      }
      grn_obj_unlink(ctx, columns);
      grn_obj_unlink(ctx, query);
    }
  }
  grn_output_table_name_or_id(ctx, result_set);
  return NULL;
}

static grn_obj *
command_filter_by_script(grn_ctx *ctx, int nargs,
                         grn_obj **args, grn_user_data *user_data)
{
  grn_obj *result_set = NULL;
  grn_obj *table = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(0)));
  if (table) {
    grn_expr_flags flags = GRN_EXPR_SYNTAX_SCRIPT;
    grn_obj *v, *query;
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, query, v);
    if (query) {
      if (parse_bool_value(ctx, VAR(4))) {
        flags |= GRN_EXPR_ALLOW_UPDATE;
      }
      grn_expr_parse(ctx, query, TEXT_VALUE_LEN(VAR(1)),
                     NULL, GRN_OP_MATCH, GRN_OP_AND, flags);
      if (GRN_TEXT_LEN(VAR(2))) {
        result_set = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(2)));
      } else {
        result_set = grn_table_create(ctx, NULL, 0, NULL,
                                      GRN_TABLE_HASH_KEY|
                                      GRN_OBJ_WITH_SUBREC,
                                      table, NULL);
      }
      if (result_set) {
        grn_table_select(ctx, table, query, result_set,
                         parse_set_operator_value(ctx, VAR(3)));
      }
      grn_obj_unlink(ctx, query);
    }
  }
  grn_output_table_name_or_id(ctx, result_set);
  return NULL;
}

static grn_obj *
command_filter(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_operator operator = GRN_OP_NOP;
  grn_obj *table, *column, *result_set = NULL;
  if (!(table = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(0))))) {
    goto exit;
  }
  if (!(column = grn_obj_column(ctx, table, TEXT_VALUE_LEN(VAR(1))))) {
    ERR(GRN_INVALID_ARGUMENT, "invalid column name: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(2)) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "missing mandatory argument: operator");
    goto exit;
  } else {
    uint32_t operator_len = GRN_TEXT_LEN(VAR(2));
    const char *operator_text = GRN_TEXT_VALUE(VAR(2));
    switch (operator_text[0]) {
    case '<' :
      if (operator_len == 1) {
        operator = GRN_OP_LESS;
      }
      break;
    }
    if (operator == GRN_OP_NOP) {
      ERR(GRN_INVALID_ARGUMENT, "invalid operator: <%.*s>",
          operator_len, operator_text);
      goto exit;
    }
  }
  if (GRN_TEXT_LEN(VAR(4))) {
    result_set = grn_ctx_get_table_by_name_or_id(ctx, TEXT_VALUE_LEN(VAR(4)));
  } else {
    result_set = grn_table_create(ctx, NULL, 0, NULL,
                                  GRN_TABLE_HASH_KEY|
                                  GRN_OBJ_WITH_SUBREC,
                                  table, NULL);
  }
  if (result_set) {
    grn_column_filter(ctx, column, operator, VAR(3), result_set,
                      parse_set_operator_value(ctx, VAR(5)));
  }
exit :
  grn_output_table_name_or_id(ctx, result_set);
  return NULL;
}

static grn_obj *
command_group(grn_ctx *ctx, int nargs, grn_obj **args,
              grn_user_data *user_data)
{
  const char *table = GRN_TEXT_VALUE(VAR(0));
  unsigned int table_len = GRN_TEXT_LEN(VAR(0));
  const char *key = GRN_TEXT_VALUE(VAR(1));
  unsigned int key_len = GRN_TEXT_LEN(VAR(1));
  const char *set = GRN_TEXT_VALUE(VAR(2));
  unsigned int set_len = GRN_TEXT_LEN(VAR(2));
  grn_obj *table_ = grn_ctx_get_table_by_name_or_id(ctx, table, table_len);
  grn_obj *set_ = NULL;
  if (table_) {
    uint32_t ngkeys;
    grn_table_sort_key *gkeys;
    gkeys = grn_table_sort_key_from_str(ctx, key, key_len, table_, &ngkeys);
    if (gkeys) {
      if (set_len) {
        set_ = grn_ctx_get_table_by_name_or_id(ctx, set, set_len);
      } else {
        set_ = grn_table_create_for_group(ctx, NULL, 0, NULL,
                                          gkeys[0].key, table_, 0);
      }
      if (set_) {
        if (GRN_TEXT_LEN(VAR(3))) {
          uint32_t gap = grn_atoui(GRN_TEXT_VALUE(VAR(3)),
                                   GRN_BULK_CURR(VAR(3)), NULL);
          grn_table_group_with_range_gap(ctx, table_, gkeys, set_, gap);
        } else {
          grn_table_group_result g = {
            set_, 0, 0, 1,
            GRN_TABLE_GROUP_CALC_COUNT, 0
          };
          grn_table_group(ctx, table_, gkeys, 1, &g, 1);
        }
      }
      grn_table_sort_key_close(ctx, gkeys, ngkeys);
    }
  }
  grn_output_table_name_or_id(ctx, set_);
  return NULL;
}

#define DEFAULT_LIMIT           10

static grn_obj *
command_sort(grn_ctx *ctx, int nargs, grn_obj **args,
             grn_user_data *user_data)
{
  const char *table = GRN_TEXT_VALUE(VAR(0));
  unsigned int table_len = GRN_TEXT_LEN(VAR(0));
  const char *keys = GRN_TEXT_VALUE(VAR(1));
  unsigned int keys_len = GRN_TEXT_LEN(VAR(1));
  int offset = GRN_TEXT_LEN(VAR(2))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(2)), GRN_BULK_CURR(VAR(2)), NULL)
    : 0;
  int limit = GRN_TEXT_LEN(VAR(3))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(3)), GRN_BULK_CURR(VAR(3)), NULL)
    : DEFAULT_LIMIT;
  grn_obj *table_ = grn_ctx_get_table_by_name_or_id(ctx, table, table_len);
  grn_obj *sorted = NULL;
  if (table_) {
    uint32_t nkeys;
    grn_table_sort_key *keys_;
    if (keys_len &&
        (keys_ = grn_table_sort_key_from_str(ctx, keys, keys_len,
                                             table_, &nkeys))) {
      if ((sorted = grn_table_create(ctx, NULL, 0, NULL,
                                     GRN_OBJ_TABLE_NO_KEY, NULL, table_))) {
        int table_size = (int)grn_table_size(ctx, table_);
        grn_normalize_offset_and_limit(ctx, table_size, &offset, &limit);
        grn_table_sort(ctx, table_, offset, limit, sorted, keys_, nkeys);
        grn_table_sort_key_close(ctx, keys_, nkeys);
      }
    }
  }
  grn_output_table_name_or_id(ctx, sorted);
  return NULL;
}

static grn_obj *
command_output(grn_ctx *ctx, int nargs, grn_obj **args,
               grn_user_data *user_data)
{
  const char *table = GRN_TEXT_VALUE(VAR(0));
  unsigned int table_len = GRN_TEXT_LEN(VAR(0));
  const char *columns = GRN_TEXT_VALUE(VAR(1));
  unsigned int columns_len = GRN_TEXT_LEN(VAR(1));
  int offset = GRN_TEXT_LEN(VAR(2))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(2)), GRN_BULK_CURR(VAR(2)), NULL)
    : 0;
  int limit = GRN_TEXT_LEN(VAR(3))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(3)), GRN_BULK_CURR(VAR(3)), NULL)
    : DEFAULT_LIMIT;
  grn_obj *table_ = grn_ctx_get_table_by_name_or_id(ctx, table, table_len);
  if (table_) {
    grn_obj_format format;
    int table_size = (int)grn_table_size(ctx, table_);
    GRN_OBJ_FORMAT_INIT(&format, table_size, 0, limit, offset);
    format.flags =
      GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
      GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET;
    /* TODO: accept only comma separated expr as columns */
    grn_obj_columns(ctx, table_, columns, columns_len, &format.columns);
    GRN_OUTPUT_OBJ(table_, &format);
    GRN_OBJ_FORMAT_FIN(ctx, &format);
  }
  return NULL;
}

static grn_obj *
command_each(grn_ctx *ctx, int nargs, grn_obj **args,
             grn_user_data *user_data)
{
  const char *table = GRN_TEXT_VALUE(VAR(0));
  unsigned int table_len = GRN_TEXT_LEN(VAR(0));
  const char *expr = GRN_TEXT_VALUE(VAR(1));
  unsigned int expr_len = GRN_TEXT_LEN(VAR(1));
  grn_obj *table_ = grn_ctx_get_table_by_name_or_id(ctx, table, table_len);
  if (table_) {
    grn_obj *v, *expr_;
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table_, expr_, v);
    if (expr_ && v) {
      grn_table_cursor *tc;
      grn_expr_parse(ctx, expr_, expr, expr_len,
                     NULL, GRN_OP_MATCH, GRN_OP_AND,
                     GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
      if ((tc = grn_table_cursor_open(ctx, table_, NULL, 0,
                                      NULL, 0, 0, -1, 0))) {
        grn_id id;
        while ((id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
          GRN_RECORD_SET(ctx, v, id);
          grn_expr_exec(ctx, expr_, 0);
        }
        grn_table_cursor_close(ctx, tc);
      }
      grn_obj_unlink(ctx, expr_);
    }
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
command_unlink(grn_ctx *ctx, int nargs, grn_obj **args,
               grn_user_data *user_data)
{
  const char *table = GRN_TEXT_VALUE(VAR(0));
  unsigned int table_len = GRN_TEXT_LEN(VAR(0));
  grn_obj *table_ = grn_ctx_get_table_by_name_or_id(ctx, table, table_len);
  if (table_) {
    grn_obj_unlink(ctx, table_);
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
command_add(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_load_(ctx, GRN_CONTENT_JSON,
            GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
            NULL, 0,
            GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
            NULL, 0, NULL, 0, 0);
  GRN_OUTPUT_BOOL(ctx->impl->loader.nrecords);
  if (ctx->impl->loader.table) {
    grn_db_touch(ctx, DB_OBJ(ctx->impl->loader.table)->db);
  }
  return NULL;
}

static grn_obj *
command_set(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  int table_name_len = GRN_TEXT_LEN(VAR(0));
  const char *table_name = GRN_TEXT_VALUE(VAR(0));
  grn_obj *table = grn_ctx_get(ctx, table_name, table_name_len);
  if (table) {
    grn_id id = GRN_ID_NIL;
    int key_len = GRN_TEXT_LEN(VAR(2));
    int id_len = GRN_TEXT_LEN(VAR(5));
    if (key_len) {
      const char *key = GRN_TEXT_VALUE(VAR(2));
      id = grn_table_get(ctx, table, key, key_len);
    } else {
      if (id_len) {
        id = grn_atoui(GRN_TEXT_VALUE(VAR(5)), GRN_BULK_CURR(VAR(5)), NULL);
      }
      id = grn_table_at(ctx, table, id);
    }
    if (id) {
      grn_obj obj;
      grn_obj_format format;
      GRN_RECORD_INIT(&obj, 0, ((grn_db_obj *)table)->id);
      GRN_OBJ_FORMAT_INIT(&format, 1, 0, 1, 0);
      GRN_RECORD_SET(ctx, &obj, id);
      grn_obj_columns(ctx, table,
                      GRN_TEXT_VALUE(VAR(4)),
                      GRN_TEXT_LEN(VAR(4)), &format.columns);
      format.flags = 0 /* GRN_OBJ_FORMAT_WITH_COLUMN_NAMES */;
      GRN_OUTPUT_OBJ(&obj, &format);
      GRN_OBJ_FORMAT_FIN(ctx, &format);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT,
        "nonexistent table name: <%.*s>", table_name_len, table_name);
  }
  return NULL;
}

static grn_rc
command_get_resolve_parameters(grn_ctx *ctx, grn_user_data *user_data,
                               grn_obj **table, grn_id *id)
{
  const char *table_text, *id_text, *key_text;
  int table_length, id_length, key_length;

  table_text = GRN_TEXT_VALUE(VAR(0));
  table_length = GRN_TEXT_LEN(VAR(0));
  if (table_length == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[table][get] table isn't specified");
    return ctx->rc;
  }

  *table = grn_ctx_get(ctx, table_text, table_length);
  if (!*table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][get] table doesn't exist: <%.*s>", table_length, table_text);
    return ctx->rc;
  }

  key_text = GRN_TEXT_VALUE(VAR(1));
  key_length = GRN_TEXT_LEN(VAR(1));
  id_text = GRN_TEXT_VALUE(VAR(3));
  id_length = GRN_TEXT_LEN(VAR(3));
  switch ((*table)->header.type) {
  case GRN_TABLE_NO_KEY:
    if (key_length) {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][get] should not specify key for NO_KEY table: <%.*s>: "
          "table: <%.*s>",
          key_length, key_text,
          table_length, table_text);
      return ctx->rc;
    }
    if (id_length) {
      const char *rest = NULL;
      *id = grn_atoi(id_text, id_text + id_length, &rest);
      if (rest == id_text) {
        ERR(GRN_INVALID_ARGUMENT,
            "[table][get] ID should be a number: <%.*s>: table: <%.*s>",
            id_length, id_text,
            table_length, table_text);
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][get] ID isn't specified: table: <%.*s>",
          table_length, table_text);
    }
    break;
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
    if (key_length && id_length) {
      ERR(GRN_INVALID_ARGUMENT,
          "[table][get] should not specify both key and ID: "
          "key: <%.*s>: ID: <%.*s>: table: <%.*s>",
          key_length, key_text,
          id_length, id_text,
          table_length, table_text);
      return ctx->rc;
    }
    if (key_length) {
      *id = grn_table_get(ctx, *table, key_text, key_length);
      if (!*id) {
        ERR(GRN_INVALID_ARGUMENT,
            "[table][get] nonexistent key: <%.*s>: table: <%.*s>",
            key_length, key_text,
            table_length, table_text);
      }
    } else {
      if (id_length) {
        const char *rest = NULL;
        *id = grn_atoi(id_text, id_text + id_length, &rest);
        if (rest == id_text) {
          ERR(GRN_INVALID_ARGUMENT,
              "[table][get] ID should be a number: <%.*s>: table: <%.*s>",
              id_length, id_text,
              table_length, table_text);
        }
      } else {
        ERR(GRN_INVALID_ARGUMENT,
            "[table][get] key nor ID isn't specified: table: <%.*s>",
            table_length, table_text);
      }
    }
    break;
  default:
    ERR(GRN_INVALID_ARGUMENT,
        "[table][get] not a table: <%.*s>", table_length, table_text);
    break;
  }

  return ctx->rc;
}

static grn_obj *
command_get(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_id id = GRN_ID_NIL;
  grn_obj *table = NULL;
  if (!command_get_resolve_parameters(ctx, user_data, &table, &id)) {
    grn_obj obj;
    grn_obj_format format;
    GRN_OUTPUT_ARRAY_OPEN("RESULT", 2);
    GRN_RECORD_INIT(&obj, 0, ((grn_db_obj *)table)->id);
    GRN_OBJ_FORMAT_INIT(&format, 1, 0, 1, 0);
    GRN_RECORD_SET(ctx, &obj, id);
    grn_obj_columns(ctx, table, GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)),
                    &format.columns);
    format.flags =
      GRN_OBJ_FORMAT_WITH_COLUMN_NAMES |
      GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET;
    GRN_OUTPUT_OBJ(&obj, &format);
    GRN_OBJ_FORMAT_FIN(ctx, &format);
    GRN_OUTPUT_ARRAY_CLOSE();
  }
  return NULL;
}

static grn_obj *
command_push(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_NO_KEY:
      {
        grn_array *array = (grn_array *)table;
        grn_table_queue *queue = grn_array_queue(ctx, array);
        if (queue) {
          MUTEX_LOCK(queue->mutex);
          if (grn_table_queue_head(queue) == queue->cap) {
            grn_array_clear_curr_rec(ctx, array);
          }
          grn_load_(ctx, GRN_CONTENT_JSON,
                    GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
                    NULL, 0,
                    GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
                    NULL, 0, NULL, 0, 0);
          if (grn_table_queue_size(queue) == queue->cap) {
            grn_table_queue_tail_increment(queue);
          }
          grn_table_queue_head_increment(queue);
          COND_SIGNAL(queue->cond);
          MUTEX_UNLOCK(queue->mutex);
          GRN_OUTPUT_BOOL(ctx->impl->loader.nrecords);
          if (ctx->impl->loader.table) {
            grn_db_touch(ctx, DB_OBJ(ctx->impl->loader.table)->db);
          }
        } else {
          ERR(GRN_OPERATION_NOT_SUPPORTED, "table '%.*s' doesn't support push",
              (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
        }
      }
      break;
    default :
      ERR(GRN_OPERATION_NOT_SUPPORTED, "table '%.*s' doesn't support push",
          (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table '%.*s' does not exist.",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
  }
  return NULL;
}

static grn_obj *
command_pull(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (table) {
    switch (table->header.type) {
    case GRN_TABLE_NO_KEY:
      {
        grn_array *array = (grn_array *)table;
        grn_table_queue *queue = grn_array_queue(ctx, array);
        if (queue) {
          MUTEX_LOCK(queue->mutex);
          while (grn_table_queue_size(queue) == 0) {
            if (GRN_TEXT_LEN(VAR(2))) {
              MUTEX_UNLOCK(queue->mutex);
              GRN_OUTPUT_BOOL(0);
              return NULL;
            }
            COND_WAIT(queue->cond, queue->mutex);
          }
          grn_table_queue_tail_increment(queue);
          {
            grn_obj obj;
            grn_obj_format format;
            GRN_RECORD_INIT(&obj, 0, ((grn_db_obj *)table)->id);
            GRN_OBJ_FORMAT_INIT(&format, 1, 0, 1, 0);
            GRN_RECORD_SET(ctx, &obj, grn_table_queue_tail(queue));
            grn_obj_columns(ctx, table, GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
                            &format.columns);
            format.flags = 0 /* GRN_OBJ_FORMAT_WITH_COLUMN_NAMES */;
            GRN_OUTPUT_OBJ(&obj, &format);
            GRN_OBJ_FORMAT_FIN(ctx, &format);
          }
          MUTEX_UNLOCK(queue->mutex);
        } else {
          ERR(GRN_OPERATION_NOT_SUPPORTED, "table '%.*s' doesn't support pull",
              (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
        }
      }
      break;
    default :
      ERR(GRN_OPERATION_NOT_SUPPORTED, "table '%.*s' doesn't support pull",
          (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table '%.*s' does not exist.",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
  }
  return NULL;
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_expr_var vars[18];

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "expression", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "result_set", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "set_operation", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "allow_update", -1);
  grn_plugin_command_create(ctx, "filter_by_script", -1, command_filter_by_script, 5, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "column", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "operator", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "value", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "result_set", -1);
  grn_plugin_expr_var_init(ctx, &vars[5], "set_operation", -1);
  grn_plugin_command_create(ctx, "filter", -1, command_filter, 6, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "key", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "result_set", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "range_gap", -1);
  grn_plugin_command_create(ctx, "group", -1, command_group, 4, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "keys", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "offset", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "limit", -1);
  grn_plugin_command_create(ctx, "sort", -1, command_sort, 4, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "offset", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "limit", -1);
  grn_plugin_command_create(ctx, "output", -1, command_output, 4, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "expression", -1);
  grn_plugin_command_create(ctx, "each", -1, command_each, 2, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_command_create(ctx, "unlink", -1, command_unlink, 1, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "values", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "key", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "output_columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[5], "id", -1);
  grn_plugin_command_create(ctx, "add", -1, command_add, 2, vars);
  grn_plugin_command_create(ctx, "push", -1, command_push, 2, vars);
  grn_plugin_command_create(ctx, "set", -1, command_set, 6, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "key", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "output_columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "id", -1);
  grn_plugin_command_create(ctx, "get", -1, command_get, 4, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "output_columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "non_block", -1);
  grn_plugin_command_create(ctx, "pull", -1, command_pull, 3, vars);

  grn_plugin_expr_var_init(ctx, &vars[0], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "query", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "result_set", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "set_operation", -1);
  grn_plugin_expr_var_init(ctx, &vars[5], "allow_column_expression", -1);
  grn_plugin_expr_var_init(ctx, &vars[6], "allow_pragma", -1);
  grn_plugin_command_create(ctx, "match", -1, command_match, 7, vars);

  return ctx->rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
