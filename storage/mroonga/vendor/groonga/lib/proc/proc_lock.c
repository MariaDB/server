/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#include "../grn_ctx.h"

#include <groonga/plugin.h>

static grn_obj *
command_lock_clear(grn_ctx *ctx,
                   int nargs,
                   grn_obj **args,
                   grn_user_data *user_data)
{
  int target_name_len;
  grn_obj *target_name;
  grn_obj *obj;

  target_name = grn_plugin_proc_get_var(ctx, user_data, "target_name", -1);
  target_name_len = GRN_TEXT_LEN(target_name);

  if (target_name_len) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(target_name), target_name_len);
  } else {
    obj = grn_ctx_db(ctx);
  }

  if (obj) {
    grn_obj_clear_lock(ctx, obj);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[lock][clear] target object not found: <%.*s>",
                     target_name_len, GRN_TEXT_VALUE(target_name));
  }

  grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);

  return NULL;
}

void
grn_proc_init_clearlock(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  /* Deprecated. Use "lock_clear" instead. */
  grn_plugin_expr_var_init(ctx, &(vars[0]), "target_name", -1);
  grn_plugin_command_create(ctx,
                            "clearlock", -1,
                            command_lock_clear,
                            1,
                            vars);
}

void
grn_proc_init_lock_clear(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "target_name", -1);
  grn_plugin_command_create(ctx,
                            "lock_clear", -1,
                            command_lock_clear,
                            1,
                            vars);
}

static grn_obj *
command_lock_acquire(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  int target_name_len;
  grn_obj *target_name;
  grn_obj *obj;

  target_name = grn_plugin_proc_get_var(ctx, user_data, "target_name", -1);
  target_name_len = GRN_TEXT_LEN(target_name);

  if (target_name_len) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(target_name), target_name_len);
  } else {
    obj = grn_ctx_db(ctx);
  }

  if (obj) {
    grn_obj_lock(ctx, obj, GRN_ID_NIL, grn_lock_timeout);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[lock][acquire] target object not found: <%.*s>",
                     target_name_len, GRN_TEXT_VALUE(target_name));
  }

  grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);

  return NULL;
}

void
grn_proc_init_lock_acquire(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "target_name", -1);
  grn_plugin_command_create(ctx,
                            "lock_acquire", -1,
                            command_lock_acquire,
                            1,
                            vars);
}

static grn_obj *
command_lock_release(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  int target_name_len;
  grn_obj *target_name;
  grn_obj *obj;

  target_name = grn_plugin_proc_get_var(ctx, user_data, "target_name", -1);
  target_name_len = GRN_TEXT_LEN(target_name);

  if (target_name_len) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(target_name), target_name_len);
  } else {
    obj = grn_ctx_db(ctx);
  }

  if (obj) {
    grn_obj_unlock(ctx, obj, GRN_ID_NIL);
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[lock][release] target object not found: <%.*s>",
                     target_name_len, GRN_TEXT_VALUE(target_name));
  }

  grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);

  return NULL;
}

void
grn_proc_init_lock_release(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "target_name", -1);
  grn_plugin_command_create(ctx,
                            "lock_release", -1,
                            command_lock_release,
                            1,
                            vars);
}
