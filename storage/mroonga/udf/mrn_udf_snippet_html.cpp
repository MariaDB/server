/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2015-2017 Kouhei Sutou <kou@clear-code.com>

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
#include <mrn_query_parser.hpp>
#include <mrn_current_thread.hpp>

MRN_BEGIN_DECLS

extern mrn::DatabaseManager *mrn_db_manager;
extern mrn::ContextPool *mrn_context_pool;

typedef struct st_mrn_snippet_html_info
{
  grn_ctx *ctx;
  grn_obj *db;
  bool use_shared_db;
  grn_obj *snippet;
  String result_str;
  struct {
    bool used;
    grn_obj *table;
    grn_obj *default_column;
  } query_mode;
} mrn_snippet_html_info;

static my_bool mrn_snippet_html_prepare(mrn_snippet_html_info *info,
                                        UDF_ARGS *args,
                                        char *message,
                                        grn_obj **snippet)
{
  MRN_DBUG_ENTER_FUNCTION();

  grn_ctx *ctx = info->ctx;
  int flags = GRN_SNIP_SKIP_LEADING_SPACES;
  unsigned int width = 200;
  unsigned int max_n_results = 3;
  const char *open_tag = "<span class=\"keyword\">";
  const char *close_tag = "</span>";
  grn_snip_mapping *mapping = GRN_SNIP_MAPPING_HTML_ESCAPE;
  grn_obj *expr = NULL;
  String *result_str = &(info->result_str);

  *snippet = NULL;

  mrn::encoding::set_raw(ctx, system_charset_info);
  if (!(system_charset_info->state & (MY_CS_BINSORT | MY_CS_CSSORT))) {
    flags |= GRN_SNIP_NORMALIZE;
  }

  *snippet = grn_snip_open(ctx, flags,
                           width, max_n_results,
                           open_tag, strlen(open_tag),
                           close_tag, strlen(close_tag),
                           mapping);
  if (ctx->rc != GRN_SUCCESS) {
    if (message) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_snippet_html(): failed to open grn_snip: <%s>",
               ctx->errbuf);
    }
    goto error;
  }

  if (info->query_mode.used) {
    if (!info->query_mode.table) {
      grn_obj *short_text;
      short_text = grn_ctx_at(info->ctx, GRN_DB_SHORT_TEXT);
      info->query_mode.table = grn_table_create(info->ctx,
                                                NULL, 0, NULL,
                                                GRN_TABLE_HASH_KEY,
                                                short_text,
                                                NULL);
    }
    if (!info->query_mode.default_column) {
      info->query_mode.default_column =
        grn_obj_column(info->ctx,
                       info->query_mode.table,
                       GRN_COLUMN_NAME_KEY,
                       GRN_COLUMN_NAME_KEY_LEN);
    }

    grn_obj *record = NULL;
    GRN_EXPR_CREATE_FOR_QUERY(info->ctx, info->query_mode.table, expr, record);
    if (!expr) {
      if (message) {
        snprintf(message, MYSQL_ERRMSG_SIZE,
                 "mroonga_snippet_html(): "
                 "failed to create expression: <%s>",
                 ctx->errbuf);
      }
      goto error;
    }

    mrn::QueryParser query_parser(info->ctx,
                                  current_thd,
                                  expr,
                                  info->query_mode.default_column,
                                  0,
                                  NULL);
    grn_rc rc = query_parser.parse(args->args[1], args->lengths[1]);
    if (rc != GRN_SUCCESS) {
      if (message) {
        snprintf(message, MYSQL_ERRMSG_SIZE,
                 "mroonga_snippet_html(): "
                 "failed to parse query: <%s>",
                 ctx->errbuf);
      }
      goto error;
    }

    rc = grn_expr_snip_add_conditions(info->ctx,
                                      expr,
                                      *snippet,
                                      0,
                                      NULL, NULL,
                                      NULL, NULL);
    if (rc != GRN_SUCCESS) {
      if (message) {
        snprintf(message, MYSQL_ERRMSG_SIZE,
                 "mroonga_snippet_html(): "
                 "failed to add conditions: <%s>",
                 ctx->errbuf);
      }
      goto error;
    }
  } else {
    unsigned int i;
    for (i = 1; i < args->arg_count; ++i) {
      if (!args->args[i]) {
        continue;
      }
      grn_rc rc = grn_snip_add_cond(ctx, *snippet,
                                    args->args[i], args->lengths[i],
                                    NULL, 0,
                                    NULL, 0);
      if (rc != GRN_SUCCESS) {
        if (message) {
          snprintf(message, MYSQL_ERRMSG_SIZE,
                   "mroonga_snippet_html(): "
                   "failed to add a condition to grn_snip: <%s>",
                   ctx->errbuf);
        }
        goto error;
      }
    }
  }

  result_str->set_charset(system_charset_info);
  DBUG_RETURN(FALSE);

error:
  if (expr) {
    grn_obj_close(ctx, expr);
  }
  if (*snippet) {
    grn_obj_close(ctx, *snippet);
  }
  DBUG_RETURN(TRUE);
}

MRN_API my_bool mroonga_snippet_html_init(UDF_INIT *init,
                                          UDF_ARGS *args,
                                          char *message)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_snippet_html_info *info = NULL;

  init->ptr = NULL;

  if (args->arg_count < 1) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_snippet_html(): wrong number of arguments: %u for 1+",
             args->arg_count);
    goto error;
  }


  for (unsigned int i = 0; i < args->arg_count; ++i) {
    switch (args->arg_type[i]) {
    case STRING_RESULT:
      /* OK */
      break;
    case REAL_RESULT:
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_snippet_html(): all arguments must be string: "
               "<%u>=<%g>",
               i, *((double *)(args->args[i])));
      goto error;
      break;
    case INT_RESULT:
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_snippet_html(): all arguments must be string: "
               "<%u>=<%lld>",
               i, *((longlong *)(args->args[i])));
      goto error;
      break;
    default:
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_snippet_html(): all arguments must be string: <%u>",
               i);
      goto error;
      break;
    }
  }

  init->maybe_null = 1;

  info = (mrn_snippet_html_info *)mrn_my_malloc(sizeof(mrn_snippet_html_info),
                                                MYF(MY_WME | MY_ZEROFILL));
  if (!info) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_snippet_html(): failed to allocate memory");
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
              "mroonga_snippet_html(): failed to %s: %s",
              action,
              info->ctx->errbuf);
      goto error;
    }
  }

  info->query_mode.used = FALSE;

  if (args->arg_count == 2 &&
      args->attribute_lengths[1] == strlen("query") &&
      strncmp(args->attributes[1], "query", strlen("query")) == 0) {
    info->query_mode.used = TRUE;
    info->query_mode.table = NULL;
    info->query_mode.default_column = NULL;
  }

  {
    bool all_keywords_are_constant = TRUE;
    for (unsigned int i = 1; i < args->arg_count; ++i) {
      if (!args->args[i]) {
        all_keywords_are_constant = FALSE;
        break;
      }
    }

    if (all_keywords_are_constant) {
      if (mrn_snippet_html_prepare(info, args, message, &(info->snippet))) {
        goto error;
      }
    } else {
      info->snippet = NULL;
    }
  }

  init->ptr = (char *)info;

  DBUG_RETURN(FALSE);

error:
  if (info) {
    if (!info->use_shared_db) {
      grn_obj_close(info->ctx, info->db);
    }
    mrn_context_pool->release(info->ctx);
    my_free(info);
  }
  DBUG_RETURN(TRUE);
}

MRN_API char *mroonga_snippet_html(UDF_INIT *init,
                                   UDF_ARGS *args,
                                   char *result,
                                   unsigned long *length,
                                   char *is_null,
                                   char *error)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_snippet_html_info *info =
    reinterpret_cast<mrn_snippet_html_info *>(init->ptr);

  grn_ctx *ctx = info->ctx;
  grn_obj *snippet = info->snippet;
  String *result_str = &(info->result_str);

  if (!args->args[0]) {
    *is_null = 1;
    DBUG_RETURN(NULL);
  }

  if (!snippet) {
    if (mrn_snippet_html_prepare(info, args, NULL, &snippet)) {
      goto error;
    }
  }

  {
    char *target = args->args[0];
    unsigned int target_length = args->lengths[0];

    unsigned int n_results, max_tagged_length;
    {
      grn_rc rc = grn_snip_exec(ctx, snippet, target, target_length,
                                &n_results, &max_tagged_length);
      if (rc != GRN_SUCCESS) {
        my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                        ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
        goto error;
      }
    }

    *is_null = 0;
    result_str->length(0);

    {
      const char *start_tag = "<div class=\"snippet\">";
      const char *end_tag = "</div>";
      size_t start_tag_length = strlen(start_tag);
      size_t end_tag_length = strlen(end_tag);
      unsigned int max_length_per_snippet =
        start_tag_length + end_tag_length + max_tagged_length;
      if (result_str->reserve(max_length_per_snippet * n_results)) {
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
        goto error;
      }

      for (unsigned int i = 0; i < n_results; ++i) {
        result_str->q_append(start_tag, start_tag_length);

        unsigned int result_length;
        grn_rc rc =
          grn_snip_get_result(ctx, snippet, i,
                              (char *)result_str->ptr() + result_str->length(),
                              &result_length);
        if (rc) {
          my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                          ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
          goto error;
        }
        result_str->length(result_str->length() + result_length);

        result_str->q_append(end_tag, end_tag_length);
      }
    }

    if (!info->snippet) {
      grn_rc rc = grn_obj_close(ctx, snippet);
      if (rc != GRN_SUCCESS) {
        my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                        ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
        goto error;
      }
    }
  }

  *length = result_str->length();
  DBUG_RETURN((char *)result_str->ptr());

error:
  if (!info->snippet && snippet) {
    grn_obj_close(ctx, snippet);
  }

  *is_null = 1;
  *error = 1;

  DBUG_RETURN(NULL);
}

MRN_API void mroonga_snippet_html_deinit(UDF_INIT *init)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_snippet_html_info *info =
    reinterpret_cast<mrn_snippet_html_info *>(init->ptr);
  if (!info) {
    DBUG_VOID_RETURN;
  }

  if (info->snippet) {
    grn_obj_close(info->ctx, info->snippet);
  }
  if (info->query_mode.used) {
    if (info->query_mode.default_column) {
      grn_obj_close(info->ctx, info->query_mode.default_column);
    }
    if (info->query_mode.table) {
      grn_obj_close(info->ctx, info->query_mode.table);
    }
  }
  MRN_STRING_FREE(info->result_str);
  if (!info->use_shared_db) {
    grn_obj_close(info->ctx, info->db);
  }
  mrn_context_pool->release(info->ctx);
  my_free(info);

  DBUG_VOID_RETURN;
}

MRN_END_DECLS
