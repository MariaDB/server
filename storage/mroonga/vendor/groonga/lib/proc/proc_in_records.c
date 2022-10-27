/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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
#include "../grn_db.h"
#include "../grn_store.h"

#include <groonga/plugin.h>

typedef struct {
  int n_conditions;
  grn_obj *condition_table;
  grn_obj condition_columns;
  grn_operator *condition_modes;
  grn_obj *search_result;
} grn_in_records_data;

static void
grn_in_records_data_free(grn_ctx *ctx, grn_in_records_data *data)
{
  int i;
  int n_condition_columns;

  if (!data) {
    return;
  }

  GRN_PLUGIN_FREE(ctx, data->condition_modes);

  n_condition_columns =
    GRN_BULK_VSIZE(&(data->condition_columns)) / sizeof(grn_obj *);
  for (i = 0; i < n_condition_columns; i++) {
    grn_obj *condition_column;
    condition_column = GRN_PTR_VALUE_AT(&(data->condition_columns), i);
    if (condition_column && condition_column->header.type == GRN_ACCESSOR) {
      grn_obj_unlink(ctx, condition_column);
    }
  }
  GRN_OBJ_FIN(ctx, &(data->condition_columns));

  if (data->search_result) {
    grn_obj_close(ctx, data->search_result);
  }

  GRN_PLUGIN_FREE(ctx, data);
}

static grn_obj *
func_in_records_init(grn_ctx *ctx,
                     int n_args,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_in_records_data *data;
  grn_obj *condition_table;
  grn_expr_code *codes;
  int n_arg_codes;
  int n_logical_args;
  int n_conditions;
  int i;
  int nth;

  {
    grn_obj *caller;
    grn_expr *expr;
    grn_expr_code *call_code;

    caller = grn_plugin_proc_get_caller(ctx, user_data);
    expr = (grn_expr *)caller;
    call_code = expr->codes + expr->codes_curr - 1;
    n_logical_args = call_code->nargs - 1;
    codes = expr->codes + 1;
    n_arg_codes = expr->codes_curr - 2;
  }

  if (n_logical_args < 4) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): wrong number of arguments (%d for 4..)",
                     n_logical_args);
    return NULL;
  }

  if ((n_logical_args % 3) != 1) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): the number of arguments must be 1 + 3n (%d)",
                     n_logical_args);
    return NULL;
  }

  n_conditions = (n_logical_args - 1) / 3;

  condition_table = codes[0].value;
  if (!grn_obj_is_table(ctx, condition_table)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, condition_table);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): the first argument must be a table: <%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  data = GRN_PLUGIN_CALLOC(ctx, sizeof(grn_in_records_data));
  if (!data) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): failed to allocate internal data");
    return NULL;
  }
  user_data->ptr = data;

  data->n_conditions = n_conditions;
  data->condition_table = condition_table;
  GRN_PTR_INIT(&(data->condition_columns), GRN_OBJ_VECTOR, GRN_ID_NIL);
  data->condition_modes = GRN_PLUGIN_MALLOCN(ctx, grn_operator, n_conditions);
  if (!data->condition_modes) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): "
                     "failed to allocate internal data for condition modes");
    goto exit;
  }

  for (i = 1, nth = 0; i < n_arg_codes; nth++) {
    int value_i = i;
    int mode_name_i;
    grn_obj *mode_name;
    int column_name_i;
    grn_obj *column_name;
    grn_obj *condition_column;

    value_i += codes[value_i].modify;

    mode_name_i = value_i + 1;
    mode_name = codes[mode_name_i].value;
    data->condition_modes[nth] = grn_proc_option_value_mode(ctx,
                                                            mode_name,
                                                            GRN_OP_EQUAL,
                                                            "in_records()");
    if (ctx->rc != GRN_SUCCESS) {
      goto exit;
    }

    column_name_i = mode_name_i + 1;
    column_name = codes[column_name_i].value;
    if (!grn_obj_is_text_family_bulk(ctx, column_name)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, condition_table);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "in_records(): "
                       "the %dth argument must be column name as string: "
                       "<%.*s>",
                       column_name_i,
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      goto exit;
    }

    condition_column = grn_obj_column(ctx, condition_table,
                                      GRN_TEXT_VALUE(column_name),
                                      GRN_TEXT_LEN(column_name));
    if (!condition_column) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, condition_table);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "in_records(): "
                       "the %dth argument must be existing column name: "
                       "<%.*s>: <%.*s>",
                       column_name_i,
                       (int)GRN_TEXT_LEN(column_name),
                       GRN_TEXT_VALUE(column_name),
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      goto exit;
    }
    GRN_PTR_PUT(ctx, &(data->condition_columns), condition_column);

    i = column_name_i + 1;
  }

  return NULL;

exit :
  grn_in_records_data_free(ctx, data);

  return NULL;
}

static grn_obj *
func_in_records_next(grn_ctx *ctx,
                     int n_args,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_in_records_data *data = user_data->ptr;
  grn_obj *found;
  grn_obj *condition;
  grn_obj *variable;
  int i;

  found = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_BOOL, 0);
  if (!found) {
    return NULL;
  }
  GRN_BOOL_SET(ctx, found, GRN_FALSE);

  if (!data) {
    return found;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx,
                            data->condition_table,
                            condition,
                            variable);
  if (!condition) {
    grn_rc rc = ctx->rc;
    if (rc == GRN_SUCCESS) {
      rc = GRN_NO_MEMORY_AVAILABLE;
    }
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "in_records(): "
                     "failed to create internal expression: %s",
                     ctx->errbuf);
    return found;
  }

  for (i = 1; i < n_args; i += 3) {
    int nth = (i - 1) / 3;
    grn_obj *value = args[i];
    grn_obj *condition_column;
    grn_operator condition_mode;

    condition_column = GRN_PTR_VALUE_AT(&(data->condition_columns), nth);
    condition_mode = data->condition_modes[nth];

    switch (condition_mode) {
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, condition, condition_mode, 2);
      break;
    case GRN_OP_LESS :
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, condition, GRN_OP_GREATER_EQUAL, 2);
      break;
    case GRN_OP_GREATER :
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, condition, GRN_OP_LESS_EQUAL, 2);
      break;
    case GRN_OP_LESS_EQUAL :
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, condition, GRN_OP_GREATER, 2);
      break;
    case GRN_OP_GREATER_EQUAL :
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, condition, GRN_OP_LESS, 2);
      break;
    default :
      grn_expr_append_obj(ctx, condition, value, GRN_OP_PUSH, 1);
      grn_expr_append_obj(ctx, condition, condition_column, GRN_OP_GET_VALUE, 1);
      grn_expr_append_op(ctx, condition, condition_mode, 2);
      break;
    }

    if (nth > 0) {
      grn_expr_append_op(ctx, condition, GRN_OP_AND, 2);
    }
  }

  data->search_result = grn_table_select(ctx,
                                         data->condition_table,
                                         condition,
                                         data->search_result,
                                         GRN_OP_OR);
  if (grn_table_size(ctx, data->search_result) > 0) {
    GRN_BOOL_SET(ctx, found, GRN_TRUE);

    GRN_TABLE_EACH_BEGIN(ctx, data->search_result, cursor, id) {
      grn_table_cursor_delete(ctx, cursor);
    } GRN_TABLE_EACH_END(ctx, cursor);
  }

  grn_obj_close(ctx, condition);

  return found;
}

static grn_obj *
func_in_records_fin(grn_ctx *ctx,
                    int n_args,
                    grn_obj **args,
                    grn_user_data *user_data)
{
  grn_in_records_data *data = user_data->ptr;

  grn_in_records_data_free(ctx, data);

  return NULL;
}

static grn_rc
selector_in_records(grn_ctx *ctx,
                    grn_obj *table,
                    grn_obj *index,
                    int n_args,
                    grn_obj **args,
                    grn_obj *res,
                    grn_operator op)
{
  grn_obj *condition_table;
  grn_operator *condition_modes = NULL;
  grn_obj condition_columns;
  int i, nth;

  /* TODO: Enable me when function call is supported. */
  return GRN_FUNCTION_NOT_IMPLEMENTED;

  if (n_args < 5) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): wrong number of arguments (%d for 4..)",
                     n_args - 1);
    return ctx->rc;
  }

  condition_table = args[1];
  if (!grn_obj_is_table(ctx, condition_table)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, condition_table);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "in_records(): the first argument must be a table: <%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return ctx->rc;
  }

  condition_modes = GRN_PLUGIN_MALLOCN(ctx, grn_operator, (n_args - 2) / 3);
  GRN_PTR_INIT(&condition_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  for (i = 2, nth = 0; i < n_args; i += 3, nth++) {
    int mode_name_i = i + 1;
    int column_name_i = i + 2;
    grn_obj *mode_name;
    grn_operator mode;
    grn_obj *column_name;
    grn_obj *condition_column;

    mode_name = args[mode_name_i];
    mode = grn_proc_option_value_mode(ctx,
                                      mode_name,
                                      GRN_OP_EQUAL,
                                      "in_records()");
    if (ctx->rc != GRN_SUCCESS) {
      goto exit;
    }

    condition_modes[nth] = mode;

    column_name = args[column_name_i];
    if (!grn_obj_is_text_family_bulk(ctx, column_name)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, condition_table);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "in_records(): "
                       "the %dth argument must be column name as string: "
                       "<%.*s>",
                       column_name_i,
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      goto exit;
    }

    condition_column = grn_obj_column(ctx, condition_table,
                                      GRN_TEXT_VALUE(column_name),
                                      GRN_TEXT_LEN(column_name));
    if (!condition_column) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, condition_table);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "in_records(): "
                       "the %dth argument must be existing column name: "
                       "<%.*s>: <%.*s>",
                       column_name_i,
                       (int)GRN_TEXT_LEN(column_name),
                       GRN_TEXT_VALUE(column_name),
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      goto exit;
    }
    GRN_PTR_PUT(ctx, &condition_columns, condition_column);
  }

  {
    grn_obj condition_column_value;

    GRN_VOID_INIT(&condition_column_value);
    GRN_TABLE_EACH_BEGIN(ctx, condition_table, cursor, id) {
      grn_obj *sub_res = NULL;

      for (i = 2; i < n_args; i += 3) {
        int nth = (i - 2) / 3;
        grn_operator sub_op;
        grn_obj *condition_column;
        grn_operator condition_mode;
        grn_obj *column = args[i];
        grn_obj *expr;
        grn_obj *variable;

        if (nth == 0) {
          sub_op = GRN_OP_OR;
        } else {
          sub_op = GRN_OP_AND;
        }

        condition_column = GRN_PTR_VALUE_AT(&condition_columns, nth);
        condition_mode = condition_modes[nth];

        GRN_BULK_REWIND(&condition_column_value);
        grn_obj_get_value(ctx,
                          condition_column,
                          id,
                          &condition_column_value);

        GRN_EXPR_CREATE_FOR_QUERY(ctx, table, expr, variable);
        if (!expr) {
          GRN_PLUGIN_ERROR(ctx,
                           GRN_INVALID_ARGUMENT,
                           "in_records(): failed to create expression");
          GRN_OBJ_FIN(ctx, &condition_column_value);
          if (sub_res) {
            grn_obj_close(ctx, sub_res);
          }
          goto exit;
        }
        grn_expr_append_obj(ctx, expr, column, GRN_OP_GET_VALUE, 1);
        grn_expr_append_obj(ctx, expr, &condition_column_value, GRN_OP_PUSH, 1);
        grn_expr_append_op(ctx, expr, condition_mode, 2);
        sub_res = grn_table_select(ctx, table, expr, sub_res, sub_op);
        grn_obj_close(ctx, expr);
      }

      if (sub_res) {
        grn_table_setoperation(ctx, res, sub_res, res, op);
        grn_obj_close(ctx, sub_res);
      }
    } GRN_TABLE_EACH_END(ctx, cursor);
    GRN_OBJ_FIN(ctx, &condition_column_value);
  }

exit :
  GRN_PLUGIN_FREE(ctx, condition_modes);

  for (i = 2; i < n_args; i += 3) {
    int nth = (i - 2) / 3;
    grn_obj *condition_column;
    condition_column = GRN_PTR_VALUE_AT(&condition_columns, nth);
    if (condition_column && condition_column->header.type == GRN_ACCESSOR) {
      grn_obj_unlink(ctx, condition_column);
    }
  }
  GRN_OBJ_FIN(ctx, &condition_columns);

  return ctx->rc;
}

void
grn_proc_init_in_records(grn_ctx *ctx)
{
  grn_obj *selector_proc;

  selector_proc = grn_proc_create(ctx, "in_records", -1, GRN_PROC_FUNCTION,
                                  func_in_records_init,
                                  func_in_records_next,
                                  func_in_records_fin,
                                  0,
                                  NULL);
  grn_proc_set_selector(ctx, selector_proc, selector_in_records);
  grn_proc_set_selector_operator(ctx, selector_proc, GRN_OP_NOP);
}
