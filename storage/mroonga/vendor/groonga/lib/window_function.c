/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016-2017 Brazil

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

#include "grn_ctx.h"
#include "grn_db.h"
#include "grn_expr.h"
#include "grn_window_function.h"

#include <string.h>

grn_rc
grn_window_init(grn_ctx *ctx,
                grn_window *window,
                grn_obj *table,
                grn_bool is_sorted)
{
  GRN_API_ENTER;

  window->table = table;
  GRN_RECORD_INIT(&(window->ids), GRN_OBJ_VECTOR, grn_obj_id(ctx, table));
  window->n_ids = 0;
  window->current_index = 0;
  window->direction = GRN_WINDOW_DIRECTION_ASCENDING;
  window->is_sorted = is_sorted;

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_rc
grn_window_fin(grn_ctx *ctx, grn_window *window)
{
  GRN_API_ENTER;

  GRN_OBJ_FIN(ctx, &(window->ids));

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_id
grn_window_next(grn_ctx *ctx, grn_window *window)
{
  grn_id next_id;

  GRN_API_ENTER;

  if (!window) {
    GRN_API_RETURN(GRN_ID_NIL);
  }

  if (window->direction == GRN_WINDOW_DIRECTION_ASCENDING) {
    if (window->current_index >= window->n_ids) {
      GRN_API_RETURN(GRN_ID_NIL);
    }
  } else {
    if (window->current_index < 0) {
      GRN_API_RETURN(GRN_ID_NIL);
    }
  }

  next_id = GRN_RECORD_VALUE_AT(&(window->ids), window->current_index);
  if (window->direction == GRN_WINDOW_DIRECTION_ASCENDING) {
    window->current_index++;
  } else {
    window->current_index--;
  }

  GRN_API_RETURN(next_id);
}

grn_rc
grn_window_rewind(grn_ctx *ctx, grn_window *window)
{
  GRN_API_ENTER;

  if (!window) {
    ERR(GRN_INVALID_ARGUMENT, "[window][rewind] window is NULL");
    GRN_API_RETURN(ctx->rc);
  }

  if (window->direction == GRN_WINDOW_DIRECTION_ASCENDING) {
    window->current_index = 0;
  } else {
    window->current_index = window->n_ids - 1;
  }

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_obj *
grn_window_get_table(grn_ctx *ctx, grn_window *window)
{
  GRN_API_ENTER;

  if (!window) {
    ERR(GRN_INVALID_ARGUMENT, "[window][rewind] window is NULL");
    GRN_API_RETURN(NULL);
  }

  GRN_API_RETURN(window->table);
}

grn_rc
grn_window_set_direction(grn_ctx *ctx,
                         grn_window *window,
                         grn_window_direction direction)
{
  GRN_API_ENTER;

  if (!window) {
    ERR(GRN_INVALID_ARGUMENT, "[window][set][direction] window is NULL");
    GRN_API_RETURN(ctx->rc);
  }

  switch (direction) {
  case GRN_WINDOW_DIRECTION_ASCENDING :
    window->direction = direction;
    window->current_index = 0;
    break;
  case GRN_WINDOW_DIRECTION_DESCENDING :
    window->direction = direction;
    window->current_index = window->n_ids - 1;
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT,
        "[window][set][direction] direction must be "
        "GRN_WINDOW_DIRECTION_ASCENDING(%d) or "
        "GRN_WINDOW_DIRECTION_DESCENDING(%d): %d",
        GRN_WINDOW_DIRECTION_ASCENDING,
        GRN_WINDOW_DIRECTION_DESCENDING,
        direction);
    GRN_API_RETURN(ctx->rc);
    break;
  }

  GRN_API_RETURN(GRN_SUCCESS);
}

static inline void
grn_window_reset(grn_ctx *ctx,
                 grn_window *window)
{
  GRN_BULK_REWIND(&(window->ids));
}

static inline void
grn_window_add_record(grn_ctx *ctx,
                      grn_window *window,
                      grn_id record_id)
{
  GRN_RECORD_PUT(ctx, &(window->ids), record_id);
}

static inline grn_bool
grn_window_is_empty(grn_ctx *ctx,
                    grn_window *window)
{
  return GRN_BULK_VSIZE(&(window->ids)) == 0;
}

grn_bool
grn_window_is_sorted(grn_ctx *ctx, grn_window *window)
{
  GRN_API_ENTER;

  if (!window) {
    ERR(GRN_INVALID_ARGUMENT, "[window][is-sorted] window is NULL");
    GRN_API_RETURN(GRN_FALSE);
  }

  GRN_API_RETURN(window->is_sorted);
}

size_t
grn_window_get_size(grn_ctx *ctx,
                    grn_window *window)
{
  GRN_API_ENTER;

  GRN_API_RETURN(window->n_ids);
}

grn_obj *
grn_window_function_create(grn_ctx *ctx,
                           const char *name,
                           int name_size,
                           grn_window_function_func func)
{
  grn_obj *window_function = NULL;

  GRN_API_ENTER;

  if (name_size == -1) {
    name_size = strlen(name);
  }

  window_function = grn_proc_create(ctx,
                                    name,
                                    name_size,
                                    GRN_PROC_WINDOW_FUNCTION,
                                    NULL, NULL, NULL, 0, NULL);
  if (!window_function) {
    ERR(GRN_WINDOW_FUNCTION_ERROR,
        "[window-function][%.*s] failed to create proc: %s",
        name_size, name,
        ctx->errbuf);
    GRN_API_RETURN(NULL);
  }

  {
    grn_proc *proc = (grn_proc *)window_function;
    proc->callbacks.window_function = func;
  }

  GRN_API_RETURN(window_function);
}

static grn_bool
grn_expr_is_window_function_call(grn_ctx *ctx,
                                 grn_obj *window_function_call)
{
  grn_expr *expr = (grn_expr *)window_function_call;
  grn_expr_code *func;
  grn_expr_code *call;

  func = &(expr->codes[0]);
  call = &(expr->codes[expr->codes_curr - 1]);

  if (func->op != GRN_OP_PUSH) {
    return GRN_FALSE;
  }
  if (!grn_obj_is_window_function_proc(ctx, func->value)) {
    return GRN_FALSE;
  }

  if (call->op != GRN_OP_CALL) {
    return GRN_FALSE;
  }
  if (call->nargs != (expr->codes_curr - 1)) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static grn_rc
grn_expr_call_window_function(grn_ctx *ctx,
                              grn_obj *output_column,
                              grn_window *window,
                              grn_obj *window_function_call)
{
  grn_rc rc;
  grn_expr *expr = (grn_expr *)window_function_call;
  grn_proc *proc;
  int32_t i, n;
  grn_obj args;

  proc = (grn_proc *)(expr->codes[0].value);

  GRN_PTR_INIT(&args, GRN_OBJ_VECTOR, GRN_ID_NIL);
  n = expr->codes_curr - 1;
  for (i = 1; i < n; i++) {
    /* TODO: Check op. */
    GRN_PTR_PUT(ctx, &args, expr->codes[i].value);
  }
  window->n_ids = GRN_BULK_VSIZE(&(window->ids)) / sizeof(grn_id);
  if (window->direction == GRN_WINDOW_DIRECTION_ASCENDING) {
    window->current_index = 0;
  } else {
    window->current_index = window->n_ids - 1;
  }
  rc = proc->callbacks.window_function(ctx,
                                       output_column,
                                       window,
                                       (grn_obj **)GRN_BULK_HEAD(&args),
                                       GRN_BULK_VSIZE(&args) / sizeof(grn_obj *));
  GRN_OBJ_FIN(ctx, &args);

  return rc;
}

grn_rc
grn_table_apply_window_function(grn_ctx *ctx,
                                grn_obj *table,
                                grn_obj *output_column,
                                grn_window_definition *definition,
                                grn_obj *window_function_call)
{
  GRN_API_ENTER;

  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][apply][window-function] table is NULL");
    GRN_API_RETURN(ctx->rc);
  }

  if (!grn_expr_is_window_function_call(ctx, window_function_call)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, window_function_call);
    ERR(GRN_INVALID_ARGUMENT,
        "[table][apply][window-function] must be window function call: %.*s",
        (int)GRN_TEXT_LEN(&inspected),
        GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    GRN_API_RETURN(ctx->rc);
  }

  {
    size_t n_sort_keys;
    grn_table_sort_key *sort_keys;
    grn_obj *sorted;
    grn_window window;

    n_sort_keys = definition->n_group_keys + definition->n_sort_keys;
    sort_keys = GRN_MALLOCN(grn_table_sort_key, n_sort_keys);
    if (!sort_keys) {
      grn_rc rc = ctx->rc;
      if (rc == GRN_SUCCESS) {
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
      ERR(rc,
          "[table][apply][window-function] "
          "failed to allocate internal sort keys: %s",
          ctx->errbuf);
      GRN_API_RETURN(ctx->rc);
    }
    {
      size_t i;
      for (i = 0; i < definition->n_group_keys; i++) {
        sort_keys[i] = definition->group_keys[i];
      }
      for (i = 0; i < definition->n_sort_keys; i++) {
        sort_keys[i + definition->n_group_keys] = definition->sort_keys[i];
      }
    }
    sorted = grn_table_create(ctx,
                              NULL, 0, NULL,
                              GRN_OBJ_TABLE_NO_KEY,
                              NULL,
                              table);
    if (!sorted) {
      grn_rc rc = ctx->rc;
      if (rc == GRN_SUCCESS) {
        rc = GRN_NO_MEMORY_AVAILABLE;
      }
      GRN_FREE(sort_keys);
      ERR(rc,
          "[table][apply][window-function] "
          "failed to allocate table to store sorted result: %s",
          ctx->errbuf);
      GRN_API_RETURN(ctx->rc);
    }
    grn_table_sort(ctx,
                   table,
                   0, -1,
                   sorted,
                   sort_keys, n_sort_keys);

    grn_window_init(ctx, &window, table, definition->n_sort_keys > 0);
    if (definition->n_group_keys > 0) {
      grn_obj *previous_values;
      grn_obj *current_values;
      size_t i, n;

      previous_values = GRN_MALLOCN(grn_obj, definition->n_group_keys);
      current_values = GRN_MALLOCN(grn_obj, definition->n_group_keys);
      n = definition->n_group_keys;

      for (i = 0; i < n; i++) {
        GRN_VOID_INIT(&(previous_values[i]));
        GRN_VOID_INIT(&(current_values[i]));
      }

      GRN_TABLE_EACH_BEGIN(ctx, sorted, cursor, id) {
        void *value;
        grn_id record_id;
        grn_bool is_group_key_changed = GRN_FALSE;

        grn_table_cursor_get_value(ctx, cursor, &value);
        record_id = *((grn_id *)value);

        for (i = 0; i < n; i++) {
          size_t reverse_i = n - i - 1;
          grn_obj *previous_value = &(previous_values[reverse_i]);
          grn_obj *current_value = &(current_values[reverse_i]);
          grn_obj *group_key = definition->group_keys[reverse_i].key;

          if (is_group_key_changed) {
            GRN_BULK_REWIND(previous_value);
            grn_obj_get_value(ctx, group_key, record_id, previous_value);
          } else {
            GRN_BULK_REWIND(current_value);
            grn_obj_get_value(ctx, group_key, record_id, current_value);
            if ((GRN_BULK_VSIZE(current_value) !=
                 GRN_BULK_VSIZE(previous_value)) ||
                (memcmp(GRN_BULK_HEAD(current_value),
                        GRN_BULK_HEAD(previous_value),
                        GRN_BULK_VSIZE(current_value)) != 0)) {
              is_group_key_changed = GRN_TRUE;
              grn_bulk_write_from(ctx,
                                  previous_value,
                                  GRN_BULK_HEAD(current_value),
                                  0,
                                  GRN_BULK_VSIZE(current_value));
            }
          }
        }

        if (is_group_key_changed && !grn_window_is_empty(ctx, &window)) {
          grn_expr_call_window_function(ctx,
                                        output_column,
                                        &window,
                                        window_function_call);
          grn_window_reset(ctx, &window);
        }
        grn_window_add_record(ctx, &window, record_id);
      } GRN_TABLE_EACH_END(ctx, cursor);
      grn_expr_call_window_function(ctx,
                                    output_column,
                                    &window,
                                    window_function_call);

      for (i = 0; i < definition->n_group_keys; i++) {
        GRN_OBJ_FIN(ctx, &(previous_values[i]));
        GRN_OBJ_FIN(ctx, &(current_values[i]));
      }
      GRN_FREE(previous_values);
      GRN_FREE(current_values);
    } else {
      GRN_TABLE_EACH_BEGIN(ctx, sorted, cursor, id) {
        void *value;
        grn_id record_id;

        grn_table_cursor_get_value(ctx, cursor, &value);
        record_id = *((grn_id *)value);
        grn_window_add_record(ctx, &window, record_id);
      } GRN_TABLE_EACH_END(ctx, cursor);
      grn_expr_call_window_function(ctx,
                                    output_column,
                                    &window,
                                    window_function_call);
    }
    grn_window_fin(ctx, &window);

    GRN_FREE(sort_keys);
  }

  GRN_API_RETURN(ctx->rc);
}
