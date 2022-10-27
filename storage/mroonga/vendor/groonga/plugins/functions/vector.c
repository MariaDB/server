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

#ifdef GRN_EMBEDDED
#  define GRN_PLUGIN_FUNCTION_TAG functions_vector
#endif

#include <groonga/plugin.h>

static grn_obj *
func_vector_size(grn_ctx *ctx, int n_args, grn_obj **args,
                 grn_user_data *user_data)
{
  grn_obj *target;
  unsigned int size;
  grn_obj *grn_size;

  if (n_args != 1) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "vector_size(): wrong number of arguments (%d for 1)",
                     n_args);
    return NULL;
  }

  target = args[0];
  switch (target->header.type) {
  case GRN_VECTOR :
  case GRN_PVECTOR :
  case GRN_UVECTOR :
    size = grn_vector_size(ctx, target);
    break;
  default :
    {
      grn_obj inspected;

      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, target, &inspected);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "vector_size(): target object must be vector: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
    break;
  }

  grn_size = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_UINT32, 0);
  if (!grn_size) {
    return NULL;
  }

  GRN_UINT32_SET(ctx, grn_size, size);

  return grn_size;
}

static grn_obj *
func_vector_slice(grn_ctx *ctx, int n_args, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *target;
  grn_obj *from_raw = NULL;
  grn_obj *length_raw = NULL;
  int64_t from = 0;
  int64_t length = -1;
  uint32_t to = 0;
  uint32_t size = 0;
  grn_obj *slice;

  if (n_args < 2 || n_args > 3) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "vector_slice(): wrong number of arguments (%d for 2..3)",
                     n_args);
    return NULL;
  }

  target = args[0];
  from_raw = args[1];
  if (n_args == 3) {
    length_raw = args[2];
  }
  switch (target->header.type) {
  case GRN_VECTOR :
  case GRN_PVECTOR :
  case GRN_UVECTOR :
    size = grn_vector_size(ctx, target);
    break;
  default :
    {
      grn_obj inspected;

      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, target, &inspected);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "vector_slice(): target object must be vector: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
    break;
  }

  if (!grn_type_id_is_number_family(ctx, from_raw->header.domain)) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, from_raw);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "vector_slice(): from must be a number: <%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }
  if (from_raw->header.domain == GRN_DB_INT32) {
    from = GRN_INT32_VALUE(from_raw);
  } else if (from_raw->header.domain == GRN_DB_INT64) {
    from = GRN_INT64_VALUE(from_raw);
  } else {
    grn_obj buffer;
    grn_rc rc;

    GRN_INT64_INIT(&buffer, 0);
    rc = grn_obj_cast(ctx, from_raw, &buffer, GRN_FALSE);
    if (rc == GRN_SUCCESS) {
      from = GRN_INT64_VALUE(&buffer);
    }
    GRN_OBJ_FIN(ctx, &buffer);

    if (rc != GRN_SUCCESS) {
      grn_obj inspected;

      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, from_raw);
      GRN_PLUGIN_ERROR(ctx, rc,
                       "vector_slice(): "
                       "failed to cast from value to number: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
  }

  if (length_raw) {
    if (!grn_type_id_is_number_family(ctx, length_raw->header.domain)) {
      grn_obj inspected;

      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, length_raw);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "vector_slice(): length must be a number: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
    if (length_raw->header.domain == GRN_DB_INT32) {
      length = GRN_INT32_VALUE(length_raw);
    } else if (length_raw->header.domain == GRN_DB_INT64) {
      length = GRN_INT64_VALUE(length_raw);
    } else {
      grn_obj buffer;
      grn_rc rc;

      GRN_INT64_INIT(&buffer, 0);
      rc = grn_obj_cast(ctx, length_raw, &buffer, GRN_FALSE);
      if (rc == GRN_SUCCESS) {
        length = GRN_INT64_VALUE(&buffer);
      }
      GRN_OBJ_FIN(ctx, &buffer);

      if (rc != GRN_SUCCESS) {
        grn_obj inspected;

        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, length_raw);
        GRN_PLUGIN_ERROR(ctx, rc,
                         "vector_slice(): "
                         "failed to cast length value to number: <%.*s>",
                         (int)GRN_TEXT_LEN(&inspected),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        return NULL;
      }
    }
  }

  slice = grn_plugin_proc_alloc(ctx, user_data, target->header.domain, GRN_OBJ_VECTOR);
  if (!slice) {
    return NULL;
  }

  if (target->header.flags & GRN_OBJ_WITH_WEIGHT) {
    slice->header.flags |= GRN_OBJ_WITH_WEIGHT;
  }

  if (length < 0) {
    length = size + length + 1;
  }

  if (length > size) {
    length = size;
  }

  if (length <= 0) {
    return slice;
  }

  while (from < 0) {
    from += size;
  }

  to = from + length;
  if (to > size) {
    to = size;
  }

  switch (target->header.type) {
  case GRN_VECTOR :
    {
      unsigned int i;
      for (i = from; i < to; i++) {
        const char *content;
        unsigned int content_length;
        unsigned int weight;
        grn_id domain;
        content_length = grn_vector_get_element(ctx, target, i,
                                                &content, &weight, &domain);
        grn_vector_add_element(ctx, slice,
                               content, content_length, weight, domain);
      }
    }
    break;
  case GRN_PVECTOR :
    {
      unsigned int i;
      for (i = from; i < to; i++) {
        grn_obj *element = GRN_PTR_VALUE_AT(target, i);
        GRN_PTR_PUT(ctx, slice, element);
      }
    }
    break;
  case GRN_UVECTOR :
    {
      grn_obj *domain;

      domain = grn_ctx_at(ctx, target->header.domain);
      if (grn_obj_is_table(ctx, domain)) {
        unsigned int i;
        for (i = from; i < to; i++) {
          grn_id id;
          unsigned int weight;
          id = grn_uvector_get_element(ctx, target, i, &weight);
          grn_uvector_add_element(ctx, slice, id, weight);
        }
      } else {
#define PUT_SLICE_VALUES(type) do {                                     \
          unsigned int i;                                               \
          for (i = from; i < to; i++) {                                 \
            GRN_ ## type ## _PUT(ctx,                                   \
                                 slice,                                 \
                                 GRN_ ## type ## _VALUE_AT(target, i)); \
          }                                                             \
        } while (GRN_FALSE)
        switch (target->header.domain) {
        case GRN_DB_BOOL :
          PUT_SLICE_VALUES(BOOL);
          break;
        case GRN_DB_INT8 :
          PUT_SLICE_VALUES(INT8);
          break;
        case GRN_DB_UINT8 :
          PUT_SLICE_VALUES(UINT8);
          break;
        case GRN_DB_INT16 :
          PUT_SLICE_VALUES(INT16);
          break;
        case GRN_DB_UINT16 :
          PUT_SLICE_VALUES(UINT16);
          break;
        case GRN_DB_INT32 :
          PUT_SLICE_VALUES(INT32);
          break;
        case GRN_DB_UINT32 :
          PUT_SLICE_VALUES(UINT32);
          break;
        case GRN_DB_INT64 :
          PUT_SLICE_VALUES(INT64);
          break;
        case GRN_DB_UINT64 :
          PUT_SLICE_VALUES(UINT64);
          break;
        case GRN_DB_FLOAT :
          PUT_SLICE_VALUES(FLOAT);
          break;
        case GRN_DB_TIME :
          PUT_SLICE_VALUES(TIME);
          break;
        }
      }
    }
    break;
#undef PUT_SLICE_VALUES
  }

  return slice;
}

static grn_obj *
func_vector_new(grn_ctx *ctx, int n_args, grn_obj **args,
                grn_user_data *user_data)
{
  grn_obj *vector = NULL;
  int i;

  if (n_args == 0) {
    return grn_plugin_proc_alloc(ctx, user_data, GRN_DB_UINT32, GRN_OBJ_VECTOR);
  }

  vector = grn_plugin_proc_alloc(ctx,
                                 user_data,
                                 args[0]->header.domain,
                                 GRN_OBJ_VECTOR);
  if (!vector) {
    return NULL;
  }

  for (i = 0; i < n_args; i++) {
    grn_obj *element = args[i];
    switch (vector->header.type) {
    case GRN_VECTOR :
      grn_vector_add_element(ctx,
                             vector,
                             GRN_BULK_HEAD(element),
                             GRN_BULK_VSIZE(element),
                             0,
                             element->header.domain);
      break;
    case GRN_UVECTOR :
      grn_bulk_write(ctx,
                     vector,
                     GRN_BULK_HEAD(element),
                     GRN_BULK_VSIZE(element));
      break;
    case GRN_PVECTOR :
      GRN_PTR_PUT(ctx, vector, element);
      break;
    default :
      break;
    }
  }

  return vector;
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

  grn_proc_create(ctx, "vector_size", -1, GRN_PROC_FUNCTION, func_vector_size,
                  NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "vector_slice", -1, GRN_PROC_FUNCTION, func_vector_slice,
                  NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "vector_new", -1, GRN_PROC_FUNCTION, func_vector_new,
                  NULL, NULL, 0, NULL);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
