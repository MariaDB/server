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

#include "grn_db.h"
#include "grn_window_functions.h"

static grn_rc
window_record_number(grn_ctx *ctx,
                     grn_obj *output_column,
                     grn_window *window,
                     grn_obj **args,
                     int n_args)
{
  grn_id id;
  uint32_t nth_record = 1;
  grn_obj value;

  GRN_UINT32_INIT(&value, 0);
  while ((id = grn_window_next(ctx, window))) {
    GRN_UINT32_SET(ctx, &value, nth_record);
    grn_obj_set_value(ctx, output_column, id, &value, GRN_OBJ_SET);
    nth_record++;
  }
  GRN_OBJ_FIN(ctx, &value);

  return GRN_SUCCESS;
}

static grn_rc
window_sum(grn_ctx *ctx,
           grn_obj *output_column,
           grn_window *window,
           grn_obj **args,
           int n_args)
{
  grn_id id;
  grn_obj *target;

  if (n_args != 1) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "window_sum(): wrong number of arguments (%d for 1)",
                     n_args);
    return ctx->rc;
  }

  target = args[0];
  if (target->header.type != GRN_ACCESSOR) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, target);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "window_sum(): "
                     "the target column must be accessor: <%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return ctx->rc;
  }

  {
    const grn_id output_column_range_id = grn_obj_get_range(ctx, output_column);
    const grn_id target_range_id = grn_obj_get_range(ctx, target);
    grn_obj sum;
    grn_obj value;

    switch (target_range_id) {
    case GRN_DB_INT8 :
    case GRN_DB_INT16 :
    case GRN_DB_INT32 :
    case GRN_DB_INT64 :
    case GRN_DB_UINT8 :
    case GRN_DB_UINT16 :
    case GRN_DB_UINT32 :
    case GRN_DB_UINT64 :
    case GRN_DB_FLOAT :
      break;
    default :
      {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, target);
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "window_sum(): "
                         "the target column must be number column: <%.*s>",
                         (int)GRN_TEXT_LEN(&inspected),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        return ctx->rc;
      }
      break;
    }

    switch (output_column_range_id) {
    case GRN_DB_INT8 :
    case GRN_DB_INT16 :
    case GRN_DB_INT32 :
    case GRN_DB_INT64 :
      GRN_INT64_INIT(&sum, 0);
      break;
    case GRN_DB_UINT8 :
    case GRN_DB_UINT16 :
    case GRN_DB_UINT32 :
    case GRN_DB_UINT64 :
      GRN_UINT64_INIT(&sum, 0);
      break;
    case GRN_DB_FLOAT :
      GRN_FLOAT_INIT(&sum, 0);
      break;
    default :
      {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, output_column);
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "window_sum(): "
                         "the output column must be number column: <%.*s>",
                         (int)GRN_TEXT_LEN(&inspected),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        return ctx->rc;
      }
      break;
    }
    GRN_VOID_INIT(&value);

    if (grn_window_is_sorted(ctx, window)) {
      while ((id = grn_window_next(ctx, window))) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx, target, id, &value);
        switch (target_range_id) {
        case GRN_DB_INT8 :
          GRN_INT64_SET(ctx,
                        &sum,
                        GRN_INT64_VALUE(&sum) + GRN_INT8_VALUE(&value));
          break;
        case GRN_DB_INT16 :
          GRN_INT64_SET(ctx,
                        &sum,
                        GRN_INT64_VALUE(&sum) + GRN_INT16_VALUE(&value));
          break;
        case GRN_DB_INT32 :
          GRN_INT64_SET(ctx,
                        &sum,
                        GRN_INT64_VALUE(&sum) + GRN_INT32_VALUE(&value));
          break;
        case GRN_DB_INT64 :
          GRN_INT64_SET(ctx,
                        &sum,
                        GRN_INT64_VALUE(&sum) + GRN_INT64_VALUE(&value));
          break;
        case GRN_DB_UINT8 :
          GRN_UINT64_SET(ctx,
                         &sum,
                         GRN_UINT64_VALUE(&sum) + GRN_UINT8_VALUE(&value));
          break;
        case GRN_DB_UINT16 :
          GRN_UINT64_SET(ctx,
                         &sum,
                         GRN_UINT64_VALUE(&sum) + GRN_UINT16_VALUE(&value));
          break;
        case GRN_DB_UINT32 :
          GRN_UINT64_SET(ctx,
                         &sum,
                         GRN_UINT64_VALUE(&sum) + GRN_UINT32_VALUE(&value));
          break;
        case GRN_DB_UINT64 :
          GRN_UINT64_SET(ctx,
                         &sum,
                         GRN_UINT64_VALUE(&sum) + GRN_UINT64_VALUE(&value));
          break;
        case GRN_DB_FLOAT :
          GRN_FLOAT_SET(ctx,
                        &sum,
                        GRN_FLOAT_VALUE(&sum) + GRN_FLOAT_VALUE(&value));
          break;
        default :
          break;
        }
        grn_obj_set_value(ctx, output_column, id, &sum, GRN_OBJ_SET);
      }
    } else {
      int64_t sum_raw_int64 = 0;
      uint64_t sum_raw_uint64 = 0;
      double sum_raw_double = 0.0;

      while ((id = grn_window_next(ctx, window))) {
        GRN_BULK_REWIND(&value);
        grn_obj_get_value(ctx, target, id, &value);
        switch (target_range_id) {
        case GRN_DB_INT8 :
          sum_raw_int64 += GRN_INT8_VALUE(&value);
          break;
        case GRN_DB_INT16 :
          sum_raw_int64 += GRN_INT16_VALUE(&value);
          break;
        case GRN_DB_INT32 :
          sum_raw_int64 += GRN_INT32_VALUE(&value);
          break;
        case GRN_DB_INT64 :
          sum_raw_int64 += GRN_INT64_VALUE(&value);
          break;
        case GRN_DB_UINT8 :
          sum_raw_uint64 += GRN_UINT8_VALUE(&value);
          break;
        case GRN_DB_UINT16 :
          sum_raw_uint64 += GRN_UINT16_VALUE(&value);
          break;
        case GRN_DB_UINT32 :
          sum_raw_uint64 += GRN_UINT32_VALUE(&value);
          break;
        case GRN_DB_UINT64 :
          sum_raw_uint64 += GRN_UINT64_VALUE(&value);
          break;
        case GRN_DB_FLOAT :
          sum_raw_double += GRN_FLOAT_VALUE(&value);
          break;
        default :
          break;
        }
      }

      switch (output_column_range_id) {
      case GRN_DB_INT8 :
      case GRN_DB_INT16 :
      case GRN_DB_INT32 :
      case GRN_DB_INT64 :
        GRN_INT64_SET(ctx, &sum, sum_raw_int64);
        break;
      case GRN_DB_UINT8 :
      case GRN_DB_UINT16 :
      case GRN_DB_UINT32 :
      case GRN_DB_UINT64 :
        GRN_UINT64_SET(ctx, &sum, sum_raw_uint64);
        break;
      case GRN_DB_FLOAT :
        GRN_FLOAT_SET(ctx, &sum, sum_raw_double);
        break;
      }

      grn_window_rewind(ctx, window);
      while ((id = grn_window_next(ctx, window))) {
        grn_obj_set_value(ctx, output_column, id, &sum, GRN_OBJ_SET);
      }
    }

    GRN_OBJ_FIN(ctx, &value);
    GRN_OBJ_FIN(ctx, &sum);
  }

  return GRN_SUCCESS;
}

static grn_rc
window_count(grn_ctx *ctx,
             grn_obj *output_column,
             grn_window *window,
             grn_obj **args,
             int n_args)
{
  grn_id id;
  grn_id output_column_range_id;
  grn_obj n_records;
  uint32_t n_records_raw = 0;


  if (n_args != 0) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "window_count(): wrong number of arguments (%d for 0)",
                     n_args);
    return ctx->rc;
  }

  output_column_range_id = grn_obj_get_range(ctx, output_column);
  switch (output_column_range_id) {
  case GRN_DB_INT8 :
  case GRN_DB_INT16 :
  case GRN_DB_INT32 :
  case GRN_DB_INT64 :
    GRN_INT64_INIT(&n_records, 0);
    break;
  case GRN_DB_UINT8 :
  case GRN_DB_UINT16 :
  case GRN_DB_UINT32 :
  case GRN_DB_UINT64 :
    GRN_UINT64_INIT(&n_records, 0);
    break;
  case GRN_DB_FLOAT :
    GRN_FLOAT_INIT(&n_records, 0);
    break;
  default :
    {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, output_column);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "window_count(): "
                       "the output column must be number column: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return ctx->rc;
    }
    break;
  }

  if (grn_window_is_sorted(ctx, window)) {
    while ((id = grn_window_next(ctx, window))) {
      n_records_raw++;
      switch (output_column_range_id) {
      case GRN_DB_INT8 :
      case GRN_DB_INT16 :
      case GRN_DB_INT32 :
      case GRN_DB_INT64 :
        GRN_INT64_SET(ctx, &n_records, n_records_raw);
        break;
      case GRN_DB_UINT8 :
      case GRN_DB_UINT16 :
      case GRN_DB_UINT32 :
      case GRN_DB_UINT64 :
        GRN_UINT64_SET(ctx, &n_records, n_records_raw);
        break;
      case GRN_DB_FLOAT :
        GRN_FLOAT_SET(ctx, &n_records, n_records_raw);
        break;
      default :
        break;
      }
      grn_obj_set_value(ctx, output_column, id, &n_records, GRN_OBJ_SET);
    }
  } else {
    while ((id = grn_window_next(ctx, window))) {
      n_records_raw++;
    }

    switch (output_column_range_id) {
    case GRN_DB_INT8 :
    case GRN_DB_INT16 :
    case GRN_DB_INT32 :
    case GRN_DB_INT64 :
      GRN_INT64_SET(ctx, &n_records, n_records_raw);
      break;
    case GRN_DB_UINT8 :
    case GRN_DB_UINT16 :
    case GRN_DB_UINT32 :
    case GRN_DB_UINT64 :
      GRN_UINT64_SET(ctx, &n_records, n_records_raw);
      break;
    case GRN_DB_FLOAT :
      GRN_FLOAT_SET(ctx, &n_records, n_records_raw);
      break;
    }

    grn_window_rewind(ctx, window);
    while ((id = grn_window_next(ctx, window))) {
      grn_obj_set_value(ctx, output_column, id, &n_records, GRN_OBJ_SET);
    }
  }

  GRN_OBJ_FIN(ctx, &n_records);

  return GRN_SUCCESS;
}

grn_rc
grn_db_init_builtin_window_functions(grn_ctx *ctx)
{
  /* For backward compatibility. */
  grn_window_function_create(ctx,
                             "record_number", -1,
                             window_record_number);
  grn_window_function_create(ctx,
                             "window_record_number", -1,
                             window_record_number);

  grn_window_function_create(ctx,
                             "window_sum", -1,
                             window_sum);

  grn_window_function_create(ctx,
                             "window_count", -1,
                             window_count);

  return GRN_SUCCESS;
}
