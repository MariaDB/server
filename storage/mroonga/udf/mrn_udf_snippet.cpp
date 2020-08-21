/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2017 Kouhei Sutou <kou@clear-code.com>

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
#include <mrn_err.h>
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

struct st_mrn_snip_info
{
  grn_ctx *ctx;
  grn_obj *db;
  bool use_shared_db;
  grn_obj *snippet;
  String result_str;
};

static my_bool mrn_snippet_prepare(st_mrn_snip_info *snip_info, UDF_ARGS *args,
                                   char *message, grn_obj **snippet)
{
  unsigned int i;
  CHARSET_INFO *cs;
  grn_ctx *ctx = snip_info->ctx;
  long long snip_max_len;
  long long snip_max_num;
  long long skip_leading_spaces;
  long long html_escape;
  int flags = GRN_SNIP_COPY_TAG;
  grn_snip_mapping *mapping = NULL;
  grn_rc rc;
  String *result_str = &snip_info->result_str;

  *snippet = NULL;
  snip_max_len = *((long long *) args->args[1]);
  snip_max_num = *((long long *) args->args[2]);

  if (args->arg_type[3] == STRING_RESULT) {
    if (!(cs = get_charset_by_name(args->args[3], MYF(0)))) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "Unknown charset: <%s>", args->args[3]);
      goto error;
    }
  } else {
    uint charset_id = static_cast<uint>(*((long long *) args->args[3]));
    if (!(cs = get_charset(charset_id, MYF(0)))) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "Unknown charset ID: <%u>", charset_id);
      goto error;
    }
  }
  if (!mrn::encoding::set_raw(ctx, cs)) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "Unsupported charset: <%s>", cs->col_name.str);
    goto error;
  }

  if (!(cs->state & (MY_CS_BINSORT | MY_CS_CSSORT))) {
    flags |= GRN_SNIP_NORMALIZE;
  }

  skip_leading_spaces = *((long long *) args->args[4]);
  if (skip_leading_spaces) {
    flags |= GRN_SNIP_SKIP_LEADING_SPACES;
  }

  html_escape = *((long long *) args->args[5]);
  if (html_escape) {
    mapping = (grn_snip_mapping *) -1;
  }

  *snippet = grn_snip_open(ctx, flags, static_cast<unsigned int>(snip_max_len),
                           static_cast<unsigned int>(snip_max_num),
                           "", 0, "", 0, mapping);
  if (ctx->rc) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "Failed to open grn_snip: <%s>", ctx->errbuf);
    goto error;
  }

  for (i = 8; i < args->arg_count; i += 3) {
    rc = grn_snip_add_cond(ctx, *snippet,
                           args->args[i], args->lengths[i],
                           args->args[i + 1], args->lengths[i + 1],
                           args->args[i + 2], args->lengths[i + 2]);
    if (rc) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "Failed to add a condition to grn_snip: <%s>", ctx->errbuf);
      goto error;
    }
  }

  result_str->set_charset(cs);
  return FALSE;

error:
  if (*snippet) {
    grn_obj_close(ctx, *snippet);
  }
  return TRUE;
}

MRN_API my_bool mroonga_snippet_init(UDF_INIT *init, UDF_ARGS *args, char *message)
{
  uint i;
  st_mrn_snip_info *snip_info = NULL;
  bool can_open_snippet = TRUE;
  init->ptr = NULL;
  if (args->arg_count < 11 || (args->arg_count - 11) % 3)
  {
    sprintf(message, "Incorrect number of arguments for mroonga_snippet(): %u",
            args->arg_count);
    goto error;
  }
  if (args->arg_type[0] != STRING_RESULT) {
    strcpy(message, "mroonga_snippet() requires string for 1st argument");
    goto error;
  }
  if (args->arg_type[1] != INT_RESULT) {
    strcpy(message, "mroonga_snippet() requires int for 2nd argument");
    goto error;
  }
  if (args->arg_type[2] != INT_RESULT) {
    strcpy(message, "mroonga_snippet() requires int for 3rd argument");
    goto error;
  }
  if (
    args->arg_type[3] != STRING_RESULT &&
    args->arg_type[3] != INT_RESULT
  ) {
    strcpy(message,
      "mroonga_snippet() requires string or int for 4th argument");
    goto error;
  }
  if (args->arg_type[4] != INT_RESULT) {
    strcpy(message, "mroonga_snippet() requires int for 5th argument");
    goto error;
  }
  if (args->arg_type[5] != INT_RESULT) {
    strcpy(message, "mroonga_snippet() requires int for 6th argument");
    goto error;
  }
  for (i = 6; i < args->arg_count; i++) {
    if (args->arg_type[i] != STRING_RESULT) {
      sprintf(message, "mroonga_snippet() requires string for %uth argument",
              i);
      goto error;
    }
  }
  init->maybe_null = 1;

  if (!(snip_info = (st_mrn_snip_info *) mrn_my_malloc(sizeof(st_mrn_snip_info),
                                                       MYF(MY_WME | MY_ZEROFILL))))
  {
    strcpy(message, "mroonga_snippet() out of memory");
    goto error;
  }
  snip_info->ctx = mrn_context_pool->pull();
  {
    const char *current_db_path = MRN_THD_DB_PATH(current_thd);
    const char *action;
    if (current_db_path) {
      action = "open database";
      mrn::Database *db;
      int error = mrn_db_manager->open(current_db_path, &db);
      if (error == 0) {
        snip_info->db = db->get();
        grn_ctx_use(snip_info->ctx, snip_info->db);
        snip_info->use_shared_db = true;
      }
    } else {
      action = "create anonymous database";
      snip_info->db = grn_db_create(snip_info->ctx, NULL, NULL);
      snip_info->use_shared_db = false;
    }
    if (!snip_info->db) {
      sprintf(message,
              "mroonga_snippet(): failed to %s: %s",
              action,
              snip_info->ctx->errbuf);
      goto error;
    }
  }

  for (i = 1; i < args->arg_count; i++) {
    if (!args->args[i]) {
      can_open_snippet = FALSE;
      break;
    }
  }
  if (can_open_snippet) {
    if (mrn_snippet_prepare(snip_info, args, message, &snip_info->snippet)) {
      goto error;
    }
  }
  init->ptr = (char *) snip_info;

  return FALSE;

error:
  if (snip_info) {
    if (!snip_info->use_shared_db) {
      grn_obj_close(snip_info->ctx, snip_info->db);
    }
    mrn_context_pool->release(snip_info->ctx);
    my_free(snip_info);
  }
  return TRUE;
}

MRN_API char *mroonga_snippet(UDF_INIT *init, UDF_ARGS *args, char *result,
                              unsigned long *length, char *is_null, char *error)
{
  st_mrn_snip_info *snip_info = (st_mrn_snip_info *) init->ptr;
  grn_ctx *ctx = snip_info->ctx;
  String *result_str = &snip_info->result_str;
  char *target;
  unsigned int target_length;
  grn_obj *snippet = NULL;
  grn_rc rc;
  unsigned int i, n_results, max_tagged_length, result_length;

  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }
  *is_null = 0;
  target = args->args[0];
  target_length = args->lengths[0];

  if (!snip_info->snippet) {
    for (i = 1; i < args->arg_count; i++) {
      if (!args->args[i]) {
        my_printf_error(ER_MRN_INVALID_NULL_VALUE_NUM,
                        ER_MRN_INVALID_NULL_VALUE_STR, MYF(0),
                        "mroonga_snippet() arguments");
        goto error;
      }
    }

    if (mrn_snippet_prepare(snip_info, args, NULL, &snippet)) {
      goto error;
    }
  } else {
    snippet = snip_info->snippet;
  }

  rc = grn_snip_exec(ctx, snippet, target, target_length,
                     &n_results, &max_tagged_length);
  if (rc) {
    my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                    ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
    goto error;
  }

  result_str->length(0);
  if (result_str->reserve((args->lengths[6] + args->lengths[7] +
                          max_tagged_length) * n_results)) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    goto error;
  }
  for (i = 0; i < n_results; i++) {
    result_str->q_append(args->args[6], args->lengths[6]);
    rc = grn_snip_get_result(ctx, snippet, i,
                             (char *) result_str->ptr() + result_str->length(),
                             &result_length);
    if (rc) {
      my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                      ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
      goto error;
    }
    result_str->length(result_str->length() + result_length);
    result_str->q_append(args->args[7], args->lengths[7]);
  }

  if (!snip_info->snippet) {
    rc = grn_obj_close(ctx, snippet);
    if (rc) {
      my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                      ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
      goto error;
    }
  }

  *length = result_str->length();
  return (char *) result_str->ptr();

error:
  *error = 1;
  return NULL;
}

MRN_API void mroonga_snippet_deinit(UDF_INIT *init)
{
  st_mrn_snip_info *snip_info = (st_mrn_snip_info *) init->ptr;
  if (snip_info) {
    if (snip_info->snippet) {
      grn_obj_close(snip_info->ctx, snip_info->snippet);
    }
    MRN_STRING_FREE(snip_info->result_str);
    if (!snip_info->use_shared_db) {
      grn_obj_close(snip_info->ctx, snip_info->db);
    }
    mrn_context_pool->release(snip_info->ctx);
    my_free(snip_info);
  }
}

MRN_END_DECLS
