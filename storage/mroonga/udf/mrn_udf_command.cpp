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
#include <mrn_path_mapper.hpp>
#include <mrn_windows.hpp>
#include <mrn_macro.hpp>
#include <mrn_database_manager.hpp>
#include <mrn_context_pool.hpp>
#include <mrn_variables.hpp>
#include <mrn_current_thread.hpp>

#include <sql_table.h>

MRN_BEGIN_DECLS

extern mrn::DatabaseManager *mrn_db_manager;
extern mrn::ContextPool *mrn_context_pool;

struct CommandInfo
{
  grn_ctx *ctx;
  grn_obj *db;
  bool use_shared_db;
  grn_obj command;
  String result;
};

MRN_API my_bool mroonga_command_init(UDF_INIT *init, UDF_ARGS *args,
                                     char *message)
{
  CommandInfo *info = NULL;

  init->ptr = NULL;
  if (args->arg_count == 0) {
    grn_snprintf(message,
                 MYSQL_ERRMSG_SIZE,
                 MYSQL_ERRMSG_SIZE,
                 "mroonga_command(): Wrong number of arguments: %u for 1..",
                 args->arg_count);
    goto error;
  }

  if ((args->arg_count % 2) == 0) {
    grn_snprintf(message,
                 MYSQL_ERRMSG_SIZE,
                 MYSQL_ERRMSG_SIZE,
                 "mroonga_command(): The number of arguments must be odd: %u",
                 args->arg_count);
    goto error;
  }

  for (unsigned int i = 0; i < args->arg_count; ++i) {
    switch (args->arg_type[i]) {
    case STRING_RESULT:
      // OK
      break;
    case REAL_RESULT:
      grn_snprintf(message,
                   MYSQL_ERRMSG_SIZE,
                   MYSQL_ERRMSG_SIZE,
                   "mroonga_command(): Argument must be string: <%g>",
                   *reinterpret_cast<double *>(args->args[i]));
      goto error;
      break;
    case INT_RESULT:
      grn_snprintf(message,
                   MYSQL_ERRMSG_SIZE,
                   MYSQL_ERRMSG_SIZE,
                   "mroonga_command(): Argument must be string: <%lld>",
                   *reinterpret_cast<longlong *>(args->args[i]));
      goto error;
      break;
    case DECIMAL_RESULT:
      grn_snprintf(message,
                   MYSQL_ERRMSG_SIZE,
                   MYSQL_ERRMSG_SIZE,
                   "mroonga_command(): Argument must be string: <%.*s>",
                   static_cast<int>(args->lengths[i]),
                   args->args[i]);
      goto error;
      break;
    default:
      grn_snprintf(message,
                   MYSQL_ERRMSG_SIZE,
                   MYSQL_ERRMSG_SIZE,
                   "mroonga_command(): Argument must be string: <%d>(%u)",
                   args->arg_type[i],
                   i);
      goto error;
      break;
    }
  }
  init->maybe_null = 1;
  init->const_item = 0;

  info = (CommandInfo *)mrn_my_malloc(sizeof(CommandInfo),
                                      MYF(MY_WME | MY_ZEROFILL));
  if (!info) {
    strcpy(message, "mroonga_command(): out of memory");
    goto error;
  }

  info->ctx = mrn_context_pool->pull();
  {
    const char *current_db_path = MRN_THD_DB_PATH(current_thd);
    const char *action;
    if (current_db_path) {
      action = "open database";
      char encoded_db_path[FN_REFLEN + 1];
      uint encoded_db_path_length =
        tablename_to_filename(current_db_path,
                              encoded_db_path,
                              sizeof(encoded_db_path));
      encoded_db_path[encoded_db_path_length] = '\0';
      mrn::Database *db;
      int error = mrn_db_manager->open(encoded_db_path, &db);
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
      grn_snprintf(message,
                   MYSQL_ERRMSG_SIZE,
                   MYSQL_ERRMSG_SIZE,
                   "mroonga_command(): failed to %s: %s",
                   action,
                   info->ctx->errbuf);
      goto error;
    }
  }
  GRN_TEXT_INIT(&(info->command), 0);

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

static void mroonga_command_escape_value(grn_ctx *ctx,
                                         grn_obj *command,
                                         const char *value,
                                         unsigned long value_length)
{
  GRN_TEXT_PUTC(ctx, command, '"');

  const char *value_current = value;
  const char *value_end = value_current + value_length;
  while (value_current < value_end) {
    int char_length = grn_charlen(ctx, value_current, value_end);

    if (char_length == 0) {
      break;
    } else if (char_length == 1) {
      switch (*value_current) {
      case '\\':
      case '"':
        GRN_TEXT_PUTC(ctx, command, '\\');
        GRN_TEXT_PUTC(ctx, command, *value_current);
        break;
      case '\n':
        GRN_TEXT_PUTS(ctx, command, "\\n");
        break;
      default:
        GRN_TEXT_PUTC(ctx, command, *value_current);
        break;
      }
    } else {
      GRN_TEXT_PUT(ctx, command, value_current, char_length);
    }

    value_current += char_length;
  }

  GRN_TEXT_PUTC(ctx, command, '"');
}

MRN_API char *mroonga_command(UDF_INIT *init, UDF_ARGS *args, char *result,
                              unsigned long *length, char *is_null, char *error)
{
  CommandInfo *info = (CommandInfo *)init->ptr;
  grn_ctx *ctx = info->ctx;
  int flags = 0;

  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }

  GRN_BULK_REWIND(&(info->command));
  GRN_TEXT_PUT(ctx, &(info->command), args->args[0], args->lengths[0]);
  for (unsigned int i = 1; i < args->arg_count; i += 2) {
    if (!args->args[i] || !args->args[i + 1]) {
      *is_null = 1;
      return NULL;
    }

    const char *name = args->args[i];
    unsigned long name_length = args->lengths[i];
    GRN_TEXT_PUTS(ctx, &(info->command), " --");
    GRN_TEXT_PUT(ctx, &(info->command), name, name_length);

    const char *value = args->args[i + 1];
    unsigned long value_length = args->lengths[i + 1];
    GRN_TEXT_PUTS(ctx, &(info->command), " ");
    mroonga_command_escape_value(ctx, &(info->command), value, value_length);
  }

  *is_null = 0;

  grn_ctx_send(ctx,
               GRN_TEXT_VALUE(&(info->command)),
               GRN_TEXT_LEN(&(info->command)),
               0);
  if (ctx->rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    goto error;
  }

  info->result.length(0);
  do {
    char *buffer;
    unsigned int buffer_length;
    grn_ctx_recv(ctx, &buffer, &buffer_length, &flags);
    if (ctx->rc) {
      my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
      goto error;
    }
    if (buffer_length > 0) {
      if (info->result.reserve(buffer_length)) {
        my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
        goto error;
      }
      info->result.q_append(buffer, buffer_length);
    }
  } while (flags & GRN_CTX_MORE);

  *length = info->result.length();
  return (char *)(info->result.ptr());

error:
  *error = 1;
  return NULL;
}

MRN_API void mroonga_command_deinit(UDF_INIT *init)
{
  CommandInfo *info = (CommandInfo *)init->ptr;
  if (info) {
    GRN_OBJ_FIN(info->ctx, &(info->command));
    if (!info->use_shared_db) {
      grn_obj_close(info->ctx, info->db);
    }
    mrn_context_pool->release(info->ctx);
    MRN_STRING_FREE(info->result);
    my_free(info);
  }
}

MRN_END_DECLS
