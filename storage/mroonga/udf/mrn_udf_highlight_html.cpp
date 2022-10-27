/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
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

typedef struct st_mrn_highlight_html_info
{
  grn_ctx *ctx;
  grn_obj *db;
  bool use_shared_db;
  grn_obj *keywords;
  String result_str;
  struct {
    bool used;
    grn_obj *table;
    grn_obj *default_column;
  } query_mode;
} mrn_highlight_html_info;

static my_bool mrn_highlight_html_prepare(mrn_highlight_html_info *info,
                                          UDF_ARGS *args,
                                          char *message,
                                          grn_obj **keywords)
{
  MRN_DBUG_ENTER_FUNCTION();

  grn_ctx *ctx = info->ctx;
  const char *normalizer_name = "NormalizerAuto";
  grn_obj *expr = NULL;
  String *result_str = &(info->result_str);

  *keywords = NULL;

  mrn::encoding::set_raw(ctx, system_charset_info);
  if (system_charset_info->state & (MY_CS_BINSORT | MY_CS_CSSORT)) {
    normalizer_name = NULL;
  }

  *keywords = grn_table_create(ctx, NULL, 0, NULL,
                               GRN_OBJ_TABLE_PAT_KEY,
                               grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                               NULL);
  if (ctx->rc != GRN_SUCCESS) {
    if (message) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_highlight_html(): "
               "failed to create grn_pat for keywords: <%s>",
               ctx->errbuf);
    }
    goto error;
  }
  if (normalizer_name) {
    grn_obj_set_info(ctx,
                     *keywords,
                     GRN_INFO_NORMALIZER,
                     grn_ctx_get(ctx, normalizer_name, -1));
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
                 "mroonga_highlight_html(): "
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
                 "mroonga_highlight_html(): "
                 "failed to parse query: <%s>",
                 ctx->errbuf);
      }
      goto error;
    }

    {
      grn_obj extracted_keywords;
      GRN_PTR_INIT(&extracted_keywords, GRN_OBJ_VECTOR, GRN_ID_NIL);
      grn_expr_get_keywords(ctx, expr, &extracted_keywords);

      size_t n_keywords =
        GRN_BULK_VSIZE(&extracted_keywords) / sizeof(grn_obj *);
      for (size_t i = 0; i < n_keywords; ++i) {
        grn_obj *extracted_keyword = GRN_PTR_VALUE_AT(&extracted_keywords, i);
        grn_table_add(ctx,
                      *keywords,
                      GRN_TEXT_VALUE(extracted_keyword),
                      GRN_TEXT_LEN(extracted_keyword),
                      NULL);
        if (ctx->rc != GRN_SUCCESS) {
          if (message) {
            snprintf(message, MYSQL_ERRMSG_SIZE,
                     "mroonga_highlight_html(): "
                     "failed to add a keyword: <%.*s>: <%s>",
                     static_cast<int>(GRN_TEXT_LEN(extracted_keyword)),
                     GRN_TEXT_VALUE(extracted_keyword),
                     ctx->errbuf);
            GRN_OBJ_FIN(ctx, &extracted_keywords);
          }
          goto error;
        }
      }
      GRN_OBJ_FIN(ctx, &extracted_keywords);
    }
  } else {
    for (unsigned int i = 1; i < args->arg_count; ++i) {
      if (!args->args[i]) {
        continue;
      }
      grn_table_add(ctx,
                    *keywords,
                    args->args[i],
                    args->lengths[i],
                    NULL);
      if (ctx->rc != GRN_SUCCESS) {
        if (message) {
          snprintf(message, MYSQL_ERRMSG_SIZE,
                   "mroonga_highlight_html(): "
                   "failed to add a keyword: <%.*s>: <%s>",
                   static_cast<int>(args->lengths[i]),
                   args->args[i],
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
  if (*keywords) {
    grn_obj_close(ctx, *keywords);
  }
  DBUG_RETURN(TRUE);
}

MRN_API my_bool mroonga_highlight_html_init(UDF_INIT *init,
                                            UDF_ARGS *args,
                                            char *message)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_highlight_html_info *info = NULL;

  init->ptr = NULL;

  if (args->arg_count < 1) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_highlight_html(): wrong number of arguments: %u for 1+",
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
               "mroonga_highlight_html(): all arguments must be string: "
               "<%u>=<%g>",
               i, *((double *)(args->args[i])));
      goto error;
      break;
    case INT_RESULT:
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_highlight_html(): all arguments must be string: "
               "<%u>=<%lld>",
               i, *((longlong *)(args->args[i])));
      goto error;
      break;
    default:
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_highlight_html(): all arguments must be string: <%u>",
               i);
      goto error;
      break;
    }
  }

  init->maybe_null = 0;

  info =
    reinterpret_cast<mrn_highlight_html_info *>(
      mrn_my_malloc(sizeof(mrn_highlight_html_info),
                    MYF(MY_WME | MY_ZEROFILL)));
  if (!info) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_highlight_html(): failed to allocate memory");
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
              "mroonga_highlight_html(): failed to %s: %s",
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
      if (mrn_highlight_html_prepare(info, args, message, &(info->keywords))) {
        goto error;
      }
    } else {
      info->keywords = NULL;
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

static bool highlight_html(grn_ctx *ctx,
                           grn_pat *keywords,
                           const char *target,
                           size_t target_length,
                           String *output)
{
  MRN_DBUG_ENTER_FUNCTION();

  grn_obj buffer;

  GRN_TEXT_INIT(&buffer, 0);

  {
    const char *open_tag = "<span class=\"keyword\">";
    size_t open_tag_length = strlen(open_tag);
    const char *close_tag = "</span>";
    size_t close_tag_length = strlen(close_tag);

    while (target_length > 0) {
#define MAX_N_HITS 16
      grn_pat_scan_hit hits[MAX_N_HITS];
      const char *rest;
      size_t previous = 0;
      size_t chunk_length;

      int n_hits = grn_pat_scan(ctx,
                                keywords,
                                target,
                                target_length,
                                hits, MAX_N_HITS, &rest);
      for (int i = 0; i < n_hits; i++) {
        if ((hits[i].offset - previous) > 0) {
          grn_text_escape_xml(ctx,
                              &buffer,
                              target + previous,
                              hits[i].offset - previous);
        }
        GRN_TEXT_PUT(ctx, &buffer, open_tag, open_tag_length);
        grn_text_escape_xml(ctx,
                            &buffer,
                            target + hits[i].offset,
                            hits[i].length);
        GRN_TEXT_PUT(ctx, &buffer, close_tag, close_tag_length);
        previous = hits[i].offset + hits[i].length;
      }

      chunk_length = rest - target;
      if ((chunk_length - previous) > 0) {
        grn_text_escape_xml(ctx,
                            &buffer,
                            target + previous,
                            target_length - previous);
      }
      target_length -= chunk_length;
      target = rest;
#undef MAX_N_HITS
    }
  }

  if (output->reserve(GRN_TEXT_LEN(&buffer))) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    GRN_OBJ_FIN(ctx, &buffer);
    DBUG_RETURN(false);
  }

  output->q_append(GRN_TEXT_VALUE(&buffer), GRN_TEXT_LEN(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);
  DBUG_RETURN(true);
}

MRN_API char *mroonga_highlight_html(UDF_INIT *init,
                                     UDF_ARGS *args,
                                     char *result,
                                     unsigned long *length,
                                     char *is_null,
                                     char *error)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_highlight_html_info *info =
    reinterpret_cast<mrn_highlight_html_info *>(init->ptr);

  grn_ctx *ctx = info->ctx;
  grn_obj *keywords = info->keywords;
  String *result_str = &(info->result_str);

  if (!args->args[0]) {
    *is_null = 1;
    DBUG_RETURN(NULL);
  }

  if (!keywords) {
    if (mrn_highlight_html_prepare(info, args, NULL, &keywords)) {
      goto error;
    }
  }

  *is_null = 0;
  result_str->length(0);

  if (!highlight_html(ctx,
                      reinterpret_cast<grn_pat *>(keywords),
                      args->args[0],
                      args->lengths[0],
                      result_str)) {
    goto error;
  }

  if (!info->keywords) {
    grn_rc rc = grn_obj_close(ctx, keywords);
    if (rc != GRN_SUCCESS) {
      my_printf_error(ER_MRN_ERROR_FROM_GROONGA_NUM,
                      ER_MRN_ERROR_FROM_GROONGA_STR, MYF(0), ctx->errbuf);
        goto error;
    }
  }

  *length = result_str->length();
  DBUG_RETURN((char *)result_str->ptr());

error:
  if (!info->keywords && keywords) {
    grn_obj_close(ctx, keywords);
  }

  *is_null = 1;
  *error = 1;

  DBUG_RETURN(NULL);
}

MRN_API void mroonga_highlight_html_deinit(UDF_INIT *init)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn_highlight_html_info *info =
    reinterpret_cast<mrn_highlight_html_info *>(init->ptr);
  if (!info) {
    DBUG_VOID_RETURN;
  }

  if (info->keywords) {
    grn_obj_close(info->ctx, info->keywords);
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
