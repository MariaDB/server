/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2013-2017 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>
#include <mrn_path_mapper.hpp>
#include <mrn_windows.hpp>
#include <mrn_macro.hpp>
#include <mrn_variables.hpp>
#include <mrn_context_pool.hpp>

MRN_BEGIN_DECLS

extern mrn::ContextPool *mrn_context_pool;

struct EscapeInfo
{
  grn_ctx *ctx;
  bool script_mode;
  grn_obj target_characters;
  grn_obj escaped_value;
};

MRN_API my_bool mroonga_escape_init(UDF_INIT *init, UDF_ARGS *args,
                                    char *message)
{
  EscapeInfo *info = NULL;
  bool script_mode = false;

  init->ptr = NULL;
  if (!(1 <= args->arg_count && args->arg_count <= 2)) {
    snprintf(message,
             MYSQL_ERRMSG_SIZE,
             "mroonga_escape(): Incorrect number of arguments: %u for 1..2",
             args->arg_count);
    goto error;
  }

  if (args->attribute_lengths[0] == strlen("script") &&
      strncmp(args->attributes[0], "script", strlen("script")) == 0) {
    switch (args->arg_type[0]) {
    case ROW_RESULT:
      snprintf(message,
               MYSQL_ERRMSG_SIZE,
               "mroonga_escape(): "
               "The 1st script argument must be "
               "string, integer or floating point: <row>");
      goto error;
      break;
    default:
      break;
    }
    script_mode = true;
  } else {
    if (args->arg_type[0] != STRING_RESULT) {
      strcpy(message,
             "mroonga_escape(): The 1st query argument must be string");
      goto error;
    }
  }
  if (args->arg_count == 2) {
    if (args->arg_type[1] != STRING_RESULT) {
      strcpy(message,
             "mroonga_escape(): "
             "The 2st argument must be escape target characters as string");
      goto error;
    }
  }

  init->maybe_null = 1;

  info = static_cast<EscapeInfo *>(mrn_my_malloc(sizeof(EscapeInfo),
                                                 MYF(MY_WME | MY_ZEROFILL)));
  if (!info) {
    strcpy(message, "mroonga_escape(): out of memory");
    goto error;
  }

  info->ctx = mrn_context_pool->pull();
  info->script_mode = script_mode;
  GRN_TEXT_INIT(&(info->target_characters), 0);
  GRN_TEXT_INIT(&(info->escaped_value), 0);

  init->ptr = reinterpret_cast<char *>(info);

  return FALSE;

error:
  if (info) {
    mrn_context_pool->release(info->ctx);
    my_free(info);
  }
  return TRUE;
}

static void escape(EscapeInfo *info, UDF_ARGS *args)
{
  grn_ctx *ctx = info->ctx;

  GRN_BULK_REWIND(&(info->escaped_value));
  if (info->script_mode) {
    switch (args->arg_type[0]) {
    case STRING_RESULT:
      {
        char *value = args->args[0];
        unsigned long value_length = args->lengths[0];
        GRN_TEXT_PUTC(ctx, &(info->escaped_value), '"');
        if (args->arg_count == 2) {
          grn_obj special_characters;
          GRN_TEXT_INIT(&special_characters, 0);
          GRN_TEXT_PUT(ctx,
                       &special_characters,
                       args->args[1],
                       args->lengths[1]);
          GRN_TEXT_PUTC(ctx, &special_characters, '\0');
          grn_expr_syntax_escape(ctx,
                                 value,
                                 value_length,
                                 GRN_TEXT_VALUE(&special_characters),
                                 '\\',
                                 &(info->escaped_value));
          GRN_OBJ_FIN(ctx, &special_characters);
        } else {
          const char *special_characters = "\"\\";
          grn_expr_syntax_escape(ctx,
                                 value,
                                 value_length,
                                 special_characters,
                                 '\\',
                                 &(info->escaped_value));
        }
        GRN_TEXT_PUTC(ctx, &(info->escaped_value), '"');
      }
      break;
    case REAL_RESULT:
      {
        double value = *reinterpret_cast<double *>(args->args[0]);
        grn_text_ftoa(ctx, &(info->escaped_value), value);
      }
      break;
    case INT_RESULT:
      {
        longlong value = *reinterpret_cast<longlong *>(args->args[0]);
        grn_text_lltoa(ctx, &(info->escaped_value), value);
      }
      break;
    case DECIMAL_RESULT:
      {
        grn_obj value_raw;
        GRN_TEXT_INIT(&value_raw, GRN_OBJ_DO_SHALLOW_COPY);
        GRN_TEXT_SET(ctx, &value_raw, args->args[0], args->lengths[0]);
        grn_obj value;
        GRN_FLOAT_INIT(&value, 0);
        if (grn_obj_cast(ctx, &value_raw, &value, GRN_FALSE) == GRN_SUCCESS) {
          grn_text_ftoa(ctx, &(info->escaped_value), GRN_FLOAT_VALUE(&value));
        } else {
          GRN_TEXT_PUT(ctx,
                       &(info->escaped_value),
                       args->args[0],
                       args->lengths[0]);
        }
        GRN_OBJ_FIN(ctx, &value);
        GRN_OBJ_FIN(ctx, &value_raw);
      }
      break;
    default:
      break;
    }
  } else {
    char *query = args->args[0];
    unsigned long query_length = args->lengths[0];
    if (args->arg_count == 2) {
      char *target_characters = args->args[1];
      unsigned long target_characters_length = args->lengths[1];
      GRN_TEXT_PUT(ctx, &(info->target_characters),
                   target_characters,
                   target_characters_length);
      GRN_TEXT_PUTC(ctx, &(info->target_characters), '\0');
      grn_expr_syntax_escape(ctx, query, query_length,
                             GRN_TEXT_VALUE(&(info->target_characters)),
                             GRN_QUERY_ESCAPE,
                             &(info->escaped_value));
    } else {
      grn_expr_syntax_escape_query(ctx, query, query_length,
                                   &(info->escaped_value));
    }
  }
}

MRN_API char *mroonga_escape(UDF_INIT *init, UDF_ARGS *args, char *result,
                             unsigned long *length, char *is_null, char *error)
{
  EscapeInfo *info = reinterpret_cast<EscapeInfo *>(init->ptr);
  grn_ctx *ctx = info->ctx;

  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }

  *is_null = 0;

  escape(info, args);

  if (ctx->rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    goto error;
  }

  *length = GRN_TEXT_LEN(&(info->escaped_value));
  return GRN_TEXT_VALUE(&(info->escaped_value));

error:
  *error = 1;
  return NULL;
}

MRN_API void mroonga_escape_deinit(UDF_INIT *init)
{
  EscapeInfo *info = reinterpret_cast<EscapeInfo *>(init->ptr);
  if (info) {
    grn_obj_unlink(info->ctx, &(info->target_characters));
    grn_obj_unlink(info->ctx, &(info->escaped_value));
    mrn_context_pool->release(info->ctx);
    my_free(info);
  }
}

MRN_END_DECLS
