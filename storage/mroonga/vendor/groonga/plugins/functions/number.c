/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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
#  define GRN_PLUGIN_FUNCTION_TAG functions_number
#endif

#include <groonga/plugin.h>

#include <math.h>

static grn_obj *
func_number_classify(grn_ctx *ctx, int n_args, grn_obj **args,
                     grn_user_data *user_data)
{
  grn_obj *number;
  grn_obj *interval;
  grn_obj casted_interval;
  grn_obj *classed_number;

  if (n_args != 2) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "number_classify(): wrong number of arguments (%d for 2)",
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
                     "number_classify(): the first argument must be a number: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  interval = args[1];
  if (!(interval->header.type == GRN_BULK &&
        grn_type_id_is_number_family(ctx, interval->header.domain))) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, interval);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "number_classify(): the second argument must be a number: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  classed_number = grn_plugin_proc_alloc(ctx,
                                         user_data,
                                         number->header.domain,
                                         0);
  if (!classed_number) {
    return NULL;
  }

  GRN_VALUE_FIX_SIZE_INIT(&casted_interval, 0, number->header.domain);
  grn_obj_cast(ctx, interval, &casted_interval, GRN_FALSE);

#define CLASSIFY_RAW(type, getter, setter, classifier) {                \
    type number_raw;                                                    \
    type interval_raw;                                                  \
    type class_raw;                                                     \
    type classed_number_raw;                                            \
                                                                        \
    number_raw = getter(number);                                        \
    interval_raw = getter(&casted_interval);                            \
    class_raw = classifier(number_raw, interval_raw);                   \
    classed_number_raw = class_raw * interval_raw;                      \
    setter(ctx, classed_number, classed_number_raw);                    \
  }

#define CLASSIFIER_INT(number_raw, interval_raw)        \
  (number_raw) < 0 ?                                    \
    ((((number_raw) + 1) / (interval_raw)) - 1) :       \
    (((number_raw) / (interval_raw)))

#define CLASSIFY_INT(type, getter, setter)              \
  CLASSIFY_RAW(type, getter, setter, CLASSIFIER_INT)

#define CLASSIFIER_UINT(number_raw, interval_raw)       \
  ((number_raw) / (interval_raw))

#define CLASSIFY_UINT(type, getter, setter)             \
  CLASSIFY_RAW(type, getter, setter, CLASSIFIER_UINT)

#define CLASSIFIER_FLOAT(number_raw, interval_raw)      \
  floor((number_raw) / (interval_raw))

#define CLASSIFY_FLOAT(getter, setter)                          \
  CLASSIFY_RAW(double, getter, setter, CLASSIFIER_FLOAT)

  switch (number->header.domain) {
  case GRN_DB_INT8 :
    CLASSIFY_INT(int8_t, GRN_INT8_VALUE, GRN_INT8_SET);
    break;
  case GRN_DB_UINT8 :
    CLASSIFY_UINT(uint8_t, GRN_UINT8_VALUE, GRN_UINT8_SET);
    break;
  case GRN_DB_INT16 :
    CLASSIFY_INT(int16_t, GRN_INT16_VALUE, GRN_INT16_SET);
    break;
  case GRN_DB_UINT16 :
    CLASSIFY_UINT(uint16_t, GRN_UINT16_VALUE, GRN_UINT16_SET);
    break;
  case GRN_DB_INT32 :
    CLASSIFY_INT(int32_t, GRN_INT32_VALUE, GRN_INT32_SET);
    break;
  case GRN_DB_UINT32 :
    CLASSIFY_UINT(uint32_t, GRN_UINT32_VALUE, GRN_UINT32_SET);
    break;
  case GRN_DB_INT64 :
    CLASSIFY_INT(int64_t, GRN_INT64_VALUE, GRN_INT64_SET);
    break;
  case GRN_DB_UINT64 :
    CLASSIFY_UINT(uint64_t, GRN_UINT64_VALUE, GRN_UINT64_SET);
    break;
  case GRN_DB_FLOAT :
    CLASSIFY_FLOAT(GRN_FLOAT_VALUE, GRN_FLOAT_SET);
    break;
  default :
    break;
  }
#undef CLASSIFY_FLOAT
#undef CLASSIFIER_FLAOT
#undef CLASSIFY_UINT
#undef CLASSIFIER_UINT
#undef CLASSIFY_INT
#undef CLASSIFIER_INT
#undef CLASSIFY_RAW

  GRN_OBJ_FIN(ctx, &casted_interval);

  return classed_number;
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
                  "number_classify", -1,
                  GRN_PROC_FUNCTION,
                  func_number_classify,
                  NULL, NULL, 0, NULL);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
