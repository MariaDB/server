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

#ifdef GRN_EMBEDDED
#  define GRN_PLUGIN_FUNCTION_TAG functions_math
#endif

#include <groonga/plugin.h>

#include <math.h>
#include <stdlib.h>

static grn_obj *
func_math_abs(grn_ctx *ctx, int n_args, grn_obj **args,
              grn_user_data *user_data)
{
  grn_obj *number;
  grn_obj *grn_abs_number = NULL;

  if (n_args != 1) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "math_abs(): wrong number of arguments (%d for 1)",
                     n_args);
    return NULL;
  }

  number = args[0];
  if (!(number->header.type == GRN_BULK &&
        grn_type_id_is_number_family(ctx, number->header.domain))) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, number);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "math_abs(): the first argument must be a number: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

#define ABS_AS_IS(return_type, to_type, getter, setter) { \
    grn_abs_number = grn_plugin_proc_alloc(ctx,           \
                                           user_data,     \
                                           (return_type), \
                                           0);            \
    if (!grn_abs_number) {                                \
      return NULL;                                        \
    }                                                     \
    setter(ctx, grn_abs_number, getter(number));          \
  }
#define ABS_CONVERT_TYPE(func, return_type, to_type, getter, setter) { \
    grn_abs_number = grn_plugin_proc_alloc(ctx,                        \
                                           user_data,                  \
                                           (return_type),              \
                                           0);                         \
    if (!grn_abs_number) {                                             \
      return NULL;                                                     \
    } else {                                                           \
      to_type abs_number_raw = (to_type)(func)(getter(number));        \
      setter(ctx, grn_abs_number, abs_number_raw);                     \
    }                                                                  \
  }

  switch (number->header.domain) {
  case GRN_DB_INT8:
    ABS_CONVERT_TYPE(abs, GRN_DB_UINT8, uint8_t, GRN_INT8_VALUE, GRN_UINT8_SET);
    break;
  case GRN_DB_UINT8:
    ABS_AS_IS(GRN_DB_UINT8, uint8_t, GRN_UINT8_VALUE, GRN_UINT8_SET);
    break;
  case GRN_DB_INT16:
    ABS_CONVERT_TYPE(abs, GRN_DB_UINT16, uint16_t, GRN_INT16_VALUE, GRN_UINT16_SET);
    break;
  case GRN_DB_UINT16:
    ABS_AS_IS(GRN_DB_UINT16, uint16_t, GRN_UINT16_VALUE, GRN_UINT16_SET);
    break;
  case GRN_DB_INT32:
    ABS_CONVERT_TYPE(labs, GRN_DB_UINT32, uint32_t, GRN_INT32_VALUE, GRN_UINT32_SET);
    break;
  case GRN_DB_UINT32:
    ABS_AS_IS(GRN_DB_UINT32, uint32_t, GRN_UINT32_VALUE, GRN_UINT32_SET);
    break;
  case GRN_DB_INT64:
    ABS_CONVERT_TYPE(llabs, GRN_DB_UINT64, uint64_t, GRN_INT64_VALUE, GRN_UINT64_SET);
    break;
  case GRN_DB_UINT64:
    ABS_AS_IS(GRN_DB_UINT64, uint64_t, GRN_UINT64_VALUE, GRN_UINT64_SET);
    break;
  case GRN_DB_FLOAT:
    ABS_CONVERT_TYPE(fabs, GRN_DB_FLOAT, double, GRN_FLOAT_VALUE, GRN_FLOAT_SET);
    break;
  default :
    break;
  }
#undef ABS_CONVERT_TYPE
#undef ABS_AS_IS
  
  return grn_abs_number;
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return ctx->rc;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_rc rc = GRN_SUCCESS;

  grn_proc_create(ctx,
                  "math_abs", -1,
                  GRN_PROC_FUNCTION,
                  func_math_abs,
                  NULL, NULL, 0, NULL);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
