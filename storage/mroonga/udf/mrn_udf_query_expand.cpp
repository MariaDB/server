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
#include <mrn_path_mapper.hpp>
#include <mrn_windows.hpp>
#include <mrn_macro.hpp>
#include <mrn_variables.hpp>
#include <mrn_database_manager.hpp>
#include <mrn_context_pool.hpp>
#include <mrn_current_thread.hpp>
#include <mrn_query_parser.hpp>

MRN_BEGIN_DECLS

extern mrn::DatabaseManager *mrn_db_manager;
extern mrn::ContextPool *mrn_context_pool;

namespace mrn {
  struct QueryExpandInfo {
    grn_ctx *ctx;
    grn_obj expanded_query;
    grn_obj *term_column;
    grn_obj *expanded_term_column;
  };
}

static void mrn_query_expand_info_free(mrn::QueryExpandInfo *info)
{
  MRN_DBUG_ENTER_FUNCTION();

  if (!info) {
    DBUG_VOID_RETURN;
  }

  if (info->ctx) {
    GRN_OBJ_FIN(info->ctx, &(info->expanded_query));
    if (grn_obj_is_accessor(info->ctx, info->expanded_term_column)) {
      grn_obj_unlink(info->ctx, info->expanded_term_column);
    }
    if (grn_obj_is_accessor(info->ctx, info->term_column)) {
      grn_obj_unlink(info->ctx, info->term_column);
    }
    mrn_context_pool->release(info->ctx);
  }
  my_free(info);

  DBUG_VOID_RETURN;
}

MRN_API my_bool mroonga_query_expand_init(UDF_INIT *init,
                                          UDF_ARGS *args,
                                          char *message)
{
  mrn::QueryExpandInfo *info = NULL;

  MRN_DBUG_ENTER_FUNCTION();

  init->ptr = NULL;
  if (args->arg_count != 4) {
    sprintf(message,
            "mroonga_query_expand(): wrong number of arguments: %u for 4",
            args->arg_count);
    goto error;
  }
  if (args->arg_type[0] != STRING_RESULT) {
    strcpy(message,
           "mroonga_query_expand(): "
           "the 1st argument must be table name as string");
    goto error;
  }
  if (args->arg_type[1] != STRING_RESULT) {
    strcpy(message,
           "mroonga_query_expand(): "
           "the 2nd argument must be term column name as string");
    goto error;
  }
  if (args->arg_type[2] != STRING_RESULT) {
    strcpy(message,
           "mroonga_query_expand(): "
           "the 3nd argument must be expanded term column name as string");
    goto error;
  }
  if (args->arg_type[3] != STRING_RESULT) {
    strcpy(message,
           "mroonga_query_expand(): "
           "the 4th argument must be query as string");
    goto error;
  }

  init->maybe_null = 1;

  info = static_cast<mrn::QueryExpandInfo *>(
    mrn_my_malloc(sizeof(mrn::QueryExpandInfo),
                  MYF(MY_WME | MY_ZEROFILL)));
  if (!info) {
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_query_expand(): failed to allocate memory");
    goto error;
  }

  {
    const char *current_db_path = MRN_THD_DB_PATH(current_thd);
    if (!current_db_path) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_query_expand(): no current database");
      goto error;
    }

    mrn::Database *db;
    int error = mrn_db_manager->open(current_db_path, &db);
    if (error != 0) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_query_expand(): failed to open database: %s",
               mrn_db_manager->error_message());
      goto error;
    }
    info->ctx = mrn_context_pool->pull();
    grn_ctx_use(info->ctx, db->get());
  }

  GRN_TEXT_INIT(&(info->expanded_query), 0);

  {
    const char *table_name = args->args[0];
    unsigned int table_name_length = args->lengths[0];
    grn_obj *table = grn_ctx_get(info->ctx,
                                 table_name,
                                 table_name_length);
    if (!table) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_query_expand(): table doesn't exist: <%.*s>",
               static_cast<int>(table_name_length),
               table_name);
      goto error;
    }

    const char *term_column_name = args->args[1];
    unsigned int term_column_name_length = args->lengths[1];
    info->term_column = grn_obj_column(info->ctx,
                                       table,
                                       term_column_name,
                                       term_column_name_length);
    if (!info->term_column) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_query_expand(): term column doesn't exist: <%.*s.%.*s>",
               static_cast<int>(table_name_length),
               table_name,
               static_cast<int>(term_column_name_length),
               term_column_name);
      goto error;
    }

    const char *expanded_term_column_name = args->args[2];
    unsigned int expanded_term_column_name_length = args->lengths[2];
    info->expanded_term_column = grn_obj_column(info->ctx,
                                                table,
                                                expanded_term_column_name,
                                                expanded_term_column_name_length);
    if (!info->expanded_term_column) {
      snprintf(message, MYSQL_ERRMSG_SIZE,
               "mroonga_query_expand(): "
               "expanded term column doesn't exist: <%.*s.%.*s>",
               static_cast<int>(table_name_length),
               table_name,
               static_cast<int>(expanded_term_column_name_length),
               expanded_term_column_name);
      goto error;
    }
  }

  init->ptr = reinterpret_cast<char *>(info);

  DBUG_RETURN(FALSE);

error:
  mrn_query_expand_info_free(info);
  DBUG_RETURN(TRUE);
}

static void query_expand(mrn::QueryExpandInfo *info, UDF_ARGS *args)
{
  grn_ctx *ctx = info->ctx;
  const char *query = args->args[3];
  unsigned int query_length = args->lengths[3];

  mrn::QueryParser query_parser(info->ctx,
                                current_thd,
                                NULL,
                                NULL,
                                0,
                                NULL);
  const char *raw_query;
  size_t raw_query_length;
  grn_operator default_operator;
  grn_expr_flags flags;
  query_parser.parse_pragma(query,
                            query_length,
                            &raw_query,
                            &raw_query_length,
                            &default_operator,
                            &flags);
  GRN_TEXT_SET(info->ctx,
               &(info->expanded_query),
               query,
               raw_query - query);
  grn_expr_syntax_expand_query_by_table(ctx,
                                        raw_query,
                                        raw_query_length,
                                        flags,
                                        info->term_column,
                                        info->expanded_term_column,
                                        &(info->expanded_query));
}

MRN_API char *mroonga_query_expand(UDF_INIT *init,
                                   UDF_ARGS *args,
                                   char *result,
                                   unsigned long *length,
                                   char *is_null,
                                   char *error)
{
  MRN_DBUG_ENTER_FUNCTION();

  mrn::QueryExpandInfo *info =
    reinterpret_cast<mrn::QueryExpandInfo *>(init->ptr);
  grn_ctx *ctx = info->ctx;

  if (!args->args[3]) {
    *is_null = 1;
    DBUG_RETURN(NULL);
  }

  *is_null = 0;

  query_expand(info, args);

  if (ctx->rc) {
    char message[MYSQL_ERRMSG_SIZE];
    snprintf(message, MYSQL_ERRMSG_SIZE,
             "mroonga_query_expand(): "
             "failed to expand: %s",
             ctx->errbuf);
    my_message(ER_ERROR_ON_WRITE, message, MYF(0));
    goto error;
  }

  *length = GRN_TEXT_LEN(&(info->expanded_query));
  DBUG_RETURN(GRN_TEXT_VALUE(&(info->expanded_query)));

error:
  *error = 1;
  DBUG_RETURN(NULL);
}

MRN_API void mroonga_query_expand_deinit(UDF_INIT *init)
{
  MRN_DBUG_ENTER_FUNCTION();
  mrn::QueryExpandInfo *info =
    reinterpret_cast<mrn::QueryExpandInfo *>(init->ptr);
  mrn_query_expand_info_free(info);
  DBUG_VOID_RETURN;
}

MRN_END_DECLS
