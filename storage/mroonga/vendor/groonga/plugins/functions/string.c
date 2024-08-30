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
#  define GRN_PLUGIN_FUNCTION_TAG functions_string
#endif

#include <groonga/plugin.h>

/*
 * func_string_length() returns the number of characters in a string.
 * If the string contains an invalid byte sequence, this function returns the
 * number of characters before the invalid byte sequence.
 */
static grn_obj *
func_string_length(grn_ctx *ctx, int n_args, grn_obj **args,
                   grn_user_data *user_data)
{
  grn_obj *target;
  unsigned int length = 0;
  grn_obj *grn_length;

  if (n_args != 1) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "string_length(): wrong number of arguments (%d for 1)",
                     n_args);
    return NULL;
  }

  target = args[0];
  if (!(target->header.type == GRN_BULK &&
        ((target->header.domain == GRN_DB_SHORT_TEXT) ||
         (target->header.domain == GRN_DB_TEXT) ||
         (target->header.domain == GRN_DB_LONG_TEXT)))) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, target);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "string_length(): target object must be a text bulk: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  {
    const char *s = GRN_TEXT_VALUE(target);
    const char *e = GRN_TEXT_VALUE(target) + GRN_TEXT_LEN(target);
    const char *p;
    unsigned int cl = 0;
    for (p = s; p < e && (cl = grn_charlen(ctx, p, e)); p += cl) {
      length++;
    }
  }

  grn_length = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_UINT32, 0);
  if (!grn_length) {
    return NULL;
  }

  GRN_UINT32_SET(ctx, grn_length, length);

  return grn_length;
}

static grn_obj *
func_string_substring(grn_ctx *ctx, int n_args, grn_obj **args,
                      grn_user_data *user_data)
{
  grn_obj *target;
  grn_obj *from_raw;
  grn_obj *length_raw = NULL;
  int64_t from = 0;
  int64_t length = -1;
  const char *start = NULL;
  const char *end = NULL;
  grn_obj *substring;

  if (n_args < 2) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "string_substring(): wrong number of arguments (%d for 2..3)",
                     n_args);
    return NULL;
  }

  target = args[0];
  from_raw = args[1];
  if (n_args == 3) {
    length_raw = args[2];
  }

  if (!(target->header.type == GRN_BULK &&
        grn_type_id_is_text_family(ctx, target->header.domain))) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, target);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "string_substring(): target object must be a text bulk: "
                     "<%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  /* TODO: extract as grn_func_arg_int64() */
  if (!grn_type_id_is_number_family(ctx, from_raw->header.domain)) {
    grn_obj inspected;

    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, from_raw);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "string_substring(): from must be a number: <%.*s>",
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
                       "string_substring(): "
                       "failed to cast from value to number: <%.*s>",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
  }

  if (length_raw) {
    /* TODO: extract as grn_func_arg_int64() */
    if (!grn_type_id_is_number_family(ctx, length_raw->header.domain)) {
      grn_obj inspected;

      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, length_raw);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "string_substring(): length must be a number: <%.*s>",
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
                         "string_substring(): "
                         "failed to cast length value to number: <%.*s>",
                         (int)GRN_TEXT_LEN(&inspected),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        return NULL;
      }
    }
  }

  substring = grn_plugin_proc_alloc(ctx, user_data, target->header.domain, 0);
  if (!substring) {
    return NULL;
  }

  GRN_BULK_REWIND(substring);

  if (GRN_TEXT_LEN(target) == 0) {
    return substring;
  }
  if (length == 0) {
    return substring;
  }

  while (from < 0) {
    from += GRN_TEXT_LEN(target);
  }

  {
    const char *p;

    start = NULL;
    p = GRN_TEXT_VALUE(target);
    end = p + GRN_TEXT_LEN(target);

    if (from == 0) {
      start = p;
    } else {
      unsigned int char_length = 0;
      int64_t n_chars = 0;

      for (;
           p < end && (char_length = grn_charlen(ctx, p, end));
           p += char_length, n_chars++) {
        if (n_chars == from) {
          start = p;
          break;
        }
      }
    }

    if (start && length > 0) {
      unsigned int char_length = 0;
      int64_t n_chars = 0;

      for (;
           p < end && (char_length = grn_charlen(ctx, p, end));
           p += char_length, n_chars++) {
        if (n_chars == length) {
          end = p;
          break;
        }
      }
    }
  }

  if (start) {
    GRN_TEXT_SET(ctx, substring, start, end - start);
  }

  return substring;
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

  grn_proc_create(ctx, "string_length", -1, GRN_PROC_FUNCTION, func_string_length,
                  NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "string_substring", -1, GRN_PROC_FUNCTION, func_string_substring,
                  NULL, NULL, 0, NULL);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
