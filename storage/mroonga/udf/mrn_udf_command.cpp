/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2013 Kouhei Sutou <kou@clear-code.com>

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>
#include <mrn_path_mapper.hpp>
#include <mrn_windows.hpp>
#include <mrn_macro.hpp>
#include <mrn_database_manager.hpp>

MRN_BEGIN_DECLS

extern mrn::DatabaseManager *mrn_db_manager;

struct CommandInfo
{
  grn_ctx ctx;
  grn_obj *db;
  bool use_shared_db;
  String result;
};

MRN_API my_bool mroonga_command_init(UDF_INIT *initid, UDF_ARGS *args,
                                     char *message)
{
  CommandInfo *info = NULL;

  initid->ptr = NULL;
  if (args->arg_count != 1) {
    sprintf(message,
            "mroonga_command(): Incorrect number of arguments: %u for 1",
            args->arg_count);
    goto error;
  }
  if (args->arg_type[0] != STRING_RESULT) {
    strcpy(message,
           "mroonga_command(): The 1st argument must be command as string");
    goto error;
  }
  initid->maybe_null = 1;
  initid->const_item = 1;

  info = (CommandInfo *)my_malloc(sizeof(CommandInfo),
                                  MYF(MY_WME | MY_ZEROFILL));
  if (!info) {
    strcpy(message, "mroonga_command(): out of memory");
    goto error;
  }

  grn_ctx_init(&(info->ctx), 0);
  {
    const char *current_db_path = current_thd->db;
    const char *action;
    if (current_db_path) {
      action = "open database";
      int error = mrn_db_manager->open(current_db_path, &(info->db));
      if (error == 0) {
        grn_ctx_use(&(info->ctx), info->db);
        info->use_shared_db = true;
      }
    } else {
      action = "create anonymous database";
      info->db = grn_db_create(&(info->ctx), NULL, NULL);
      info->use_shared_db = false;
    }
    if (!info->db) {
      sprintf(message,
              "mroonga_command(): failed to %s: %s",
              action,
              info->ctx.errbuf);
      goto error;
    }
  }

  initid->ptr = (char *)info;

  return FALSE;

error:
  if (info) {
    if (!info->use_shared_db) {
      grn_obj_close(&(info->ctx), info->db);
    }
    grn_ctx_fin(&(info->ctx));
    my_free(info);
  }
  return TRUE;
}

MRN_API char *mroonga_command(UDF_INIT *initid, UDF_ARGS *args, char *result,
                              unsigned long *length, char *is_null, char *error)
{
  CommandInfo *info = (CommandInfo *)initid->ptr;
  grn_ctx *ctx = &(info->ctx);
  char *command;
  unsigned int command_length;
  int flags = 0;

  if (!args->args[0]) {
    *is_null = 1;
    return NULL;
  }

  *is_null = 0;
  command = args->args[0];
  command_length = args->lengths[0];

  grn_ctx_send(ctx, command, command_length, 0);
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

MRN_API void mroonga_command_deinit(UDF_INIT *initid)
{
  CommandInfo *info = (CommandInfo *)initid->ptr;
  if (info) {
    if (!info->use_shared_db) {
      grn_obj_close(&(info->ctx), info->db);
    }
    grn_ctx_fin(&(info->ctx));
    info->result.free();
    my_free(info);
  }
}

MRN_END_DECLS
