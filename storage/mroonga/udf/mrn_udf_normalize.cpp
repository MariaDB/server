/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2015 Naoya Murakami <naoya@createfield.com>
  Copyright(C) 2017 Kouhei Sutou <kou@clear-code.com>

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
#include <mrn_encoding.hpp>
#include <mrn_windows.hpp>
#include <mrn_table.hpp>
#include <mrn_macro.hpp>
#include <mrn_database_manager.hpp>
#include <mrn_context_pool.hpp>
#include <mrn_variables.hpp>
#include <mrn_current_thread.hpp>

MRN_BEGIN_DECLS

extern mrn::DatabaseManager *mrn_db_manager;
extern mrn::ContextPool *mrn_context_pool;

#define DEFAULT_NORMALIZER_NAME "NormalizerAuto"

struct st_mrn_normalize_info
{
  grn_ctx *ctx;
  grn_obj *db;
  bool use_shared_db;
  grn_obj *normalizer;
  int flags;
  String result_str;
};

MRN_API my_bool mroonga_normalize_init(UDF_INIT *init, UDF_ARGS *args,
                                       char *message)
{
  st_mrn_normalize_info *info = NULL;
  String *result_str = NULL;

  init->ptr = NULL;
  if (!(1 <= args->arg_count && args->arg_count <= 2)) {
    sprintf(message,
            "mroonga_normalize(): Incorrect number of arguments: %u for 1..2",
            args->arg_count);
    goto error;
  }
  if (args->arg_type[0] != STRING_RESULT) {
    strcpy(message,
           "mroonga_normalize(): The 1st argument must be query as string");
    goto error;
  }
  if (args->arg_count == 2) {
    if (args->arg_type[1] != STRING_RESULT) {
      strcpy(message,
             "mroonga_normalize(): "
             "The 2st argument must be normalizer name as string");
      goto error;
    }
  }

  init->maybe_null = 1;

  info = (st_mrn_normalize_info *)mrn_my_malloc(sizeof(st_mrn_normalize_info),
                                                MYF(MY_WME | MY_ZEROFILL));
  if (!info) {
    strcpy(message, "mroonga_normalize(): out of memory");
    goto error;
  }

  info->ctx = mrn_context_pool->pull();
  {
    const char *current_db_path = MRN_THD_DB_PATH(current_thd);
    const char *action;
    if (current_db_path) {
      action = "open database";
      mrn::Database *db;
      int error = mrn_db_manager->open(current_db_path, &db);
      if (error == 0) {
        info->db = db->get();
        grn_ctx_use(info->ctx, info->db);
        info->use_shared_db = true;
      }
    } else {
      action = "create anonymous database";
      info->db = grn_db_create(info->ctx, NULL, NULL);
      info->use_shared_db = false;
    }
    if (!info->db) {
      sprintf(message,
              "mroonga_normalize(): failed to %s: %s",
              action,
              info->ctx->errbuf);
      goto error;
    }
  }

  if (args->arg_count == 1) {
    info->normalizer = grn_ctx_get(info->ctx, DEFAULT_NORMALIZER_NAME, -1);
  } else {
    info->normalizer = grn_ctx_get(info->ctx, args->args[1], args->lengths[1]);
  }
  if (!info->normalizer) {
    sprintf(message, "mroonga_normalize(): nonexistent normalizer %.*s",
            (int)args->lengths[1], args->args[1]);
    goto error;
  }
  info->flags = 0;

  result_str = &(info->result_str);
  mrn::encoding::set_raw(info->ctx, system_charset_info);
  result_str->set_charset(system_charset_info);

  init->ptr = (char *)info;

  return FALSE;

error:
  if (info) {
    if (!info->use_shared_db) {
      grn_obj_close(info->ctx, info->db);
    }
    mrn_context_pool->release(info->ctx);
    my_free(info);
  }
  return TRUE;
}

MRN_API char *mroonga_normalize(UDF_INIT *init, UDF_ARGS *args, char *result,
                                unsigned long *length, char *is_null, char *error)
{
  st_mrn_normalize_info *info = (st_mrn_normalize_info *)init->ptr;
  grn_ctx *ctx = info->ctx;
  String *result_str = &(info->result_str);

  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }

  result_str->length(0);
  {
    char *target = args->args[0];
    unsigned int target_length = args->lengths[0];
    grn_obj *grn_string;
    const char *normalized;
    unsigned int normalized_length_in_bytes;
    unsigned int normalized_n_characters;

    grn_string = grn_string_open(ctx,
                                 target, target_length,
                                 info->normalizer, info->flags);
    grn_string_get_normalized(ctx, grn_string,
                              &normalized,
                              &normalized_length_in_bytes,
                              &normalized_n_characters);
    if (result_str->reserve(normalized_length_in_bytes)) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      goto error;
    }
    result_str->q_append(normalized, normalized_length_in_bytes);
    result_str->length(normalized_length_in_bytes);
    grn_obj_unlink(ctx, grn_string);
  }
  *is_null = 0;

  if (ctx->rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    goto error;
  }

  *length = result_str->length();
  return (char *)result_str->ptr();

error:
  *is_null = 1;
  *error = 1;
  return NULL;
}

MRN_API void mroonga_normalize_deinit(UDF_INIT *init)
{
  st_mrn_normalize_info *info = (st_mrn_normalize_info *)init->ptr;

  if (info) {
    MRN_STRING_FREE(info->result_str);
    if (info->normalizer) {
      grn_obj_unlink(info->ctx, info->normalizer);
    }
    if (!info->use_shared_db) {
      grn_obj_close(info->ctx, info->db);
    }
    mrn_context_pool->release(info->ctx);
    my_free(info);
  }
}

MRN_END_DECLS
