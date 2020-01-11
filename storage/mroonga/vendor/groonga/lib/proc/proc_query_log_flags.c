/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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

#include "../grn_proc.h"

#include <groonga/plugin.h>

static grn_obj *
command_query_log_flags_get(grn_ctx *ctx,
                            int nargs,
                            grn_obj **args,
                            grn_user_data *user_data)
{
  unsigned int current_flags;
  grn_obj inspected_flags;

  current_flags = grn_query_logger_get_flags(ctx);
  GRN_TEXT_INIT(&inspected_flags, 0);

  grn_inspect_query_log_flags(ctx, &inspected_flags,current_flags);
  grn_ctx_output_str(ctx,
                     GRN_TEXT_VALUE(&inspected_flags),
                     GRN_TEXT_LEN(&inspected_flags));

  GRN_OBJ_FIN(ctx, &inspected_flags);

  return NULL;
}

void
grn_proc_init_query_log_flags_get(grn_ctx *ctx)
{
  grn_plugin_command_create(ctx,
                            "query_log_flags_get", -1,
                            command_query_log_flags_get,
                            0,
                            NULL);
}

typedef enum {
  UPDATE_SET,
  UPDATE_ADD,
  UPDATE_REMOVE
} grn_query_log_flags_update_mode;

static void
grn_query_log_flags_update(grn_ctx *ctx,
                           grn_obj *flags_text,
                           grn_query_log_flags_update_mode mode,
                           const char *error_message_tag)
{
  unsigned int previous_flags;
  unsigned int flags = 0;

  previous_flags = grn_query_logger_get_flags(ctx);
  if (GRN_TEXT_LEN(flags_text) == 0) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "%s no query log flags",
                     error_message_tag);
    grn_ctx_output_null(ctx);
    return;
  }

  if (!grn_query_log_flags_parse(GRN_TEXT_VALUE(flags_text),
                                 GRN_TEXT_LEN(flags_text),
                                 &flags)) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "%s invalid query log flags: <%.*s>",
                     error_message_tag,
                     (int)GRN_TEXT_LEN(flags_text),
                     GRN_TEXT_VALUE(flags_text));
    grn_ctx_output_null(ctx);
    return;
  }

  switch (mode) {
  case UPDATE_SET :
    grn_query_logger_set_flags(ctx, flags);
    break;
  case UPDATE_ADD :
    grn_query_logger_add_flags(ctx, flags);
    break;
  case UPDATE_REMOVE :
    grn_query_logger_remove_flags(ctx, flags);
    break;
  }

  {
    unsigned int current_flags;
    grn_obj inspected_flags;

    current_flags = grn_query_logger_get_flags(ctx);
    GRN_TEXT_INIT(&inspected_flags, 0);

    grn_ctx_output_map_open(ctx, "query_log_flags", 2);

    grn_inspect_query_log_flags(ctx, &inspected_flags, previous_flags);
    grn_ctx_output_cstr(ctx, "previous");
    grn_ctx_output_str(ctx,
                       GRN_TEXT_VALUE(&inspected_flags),
                       GRN_TEXT_LEN(&inspected_flags));

    GRN_BULK_REWIND(&inspected_flags);
    grn_inspect_query_log_flags(ctx, &inspected_flags, current_flags);
    grn_ctx_output_cstr(ctx, "current");
    grn_ctx_output_str(ctx,
                       GRN_TEXT_VALUE(&inspected_flags),
                       GRN_TEXT_LEN(&inspected_flags));

    grn_ctx_output_map_close(ctx);

    GRN_OBJ_FIN(ctx, &inspected_flags);
  }

  return;
}

static grn_obj *
command_query_log_flags_set(grn_ctx *ctx,
                            int nargs,
                            grn_obj **args,
                            grn_user_data *user_data)
{
  grn_obj *flags_text;

  flags_text = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  grn_query_log_flags_update(ctx,
                             flags_text,
                             UPDATE_SET,
                             "[query-log][flags][set]");
  return NULL;
}

void
grn_proc_init_query_log_flags_set(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "flags", -1);
  grn_plugin_command_create(ctx,
                            "query_log_flags_set", -1,
                            command_query_log_flags_set,
                            1,
                            vars);
}

static grn_obj *
command_query_log_flags_add(grn_ctx *ctx,
                            int nargs,
                            grn_obj **args,
                            grn_user_data *user_data)
{
  grn_obj *flags_text;

  flags_text = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  grn_query_log_flags_update(ctx,
                             flags_text,
                             UPDATE_ADD,
                             "[query-log][flags][add]");
  return NULL;
}

void
grn_proc_init_query_log_flags_add(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "flags", -1);
  grn_plugin_command_create(ctx,
                            "query_log_flags_add", -1,
                            command_query_log_flags_add,
                            1,
                            vars);
}

static grn_obj *
command_query_log_flags_remove(grn_ctx *ctx,
                               int nargs,
                               grn_obj **args,
                               grn_user_data *user_data)
{
  grn_obj *flags_text;

  flags_text = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  grn_query_log_flags_update(ctx,
                             flags_text,
                             UPDATE_REMOVE,
                             "[query-log][flags][remove]");
  return NULL;
}

void
grn_proc_init_query_log_flags_remove(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "flags", -1);
  grn_plugin_command_create(ctx,
                            "query_log_flags_remove", -1,
                            command_query_log_flags_remove,
                            1,
                            vars);
}
