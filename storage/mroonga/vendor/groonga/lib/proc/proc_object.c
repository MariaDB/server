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
#include "../grn_io.h"

#include <groonga/plugin.h>

#include <sys/types.h>
#include <sys/stat.h>

static grn_obj *
command_object_exist(grn_ctx *ctx,
                     int nargs,
                     grn_obj **args,
                     grn_user_data *user_data)
{
  grn_obj *db;
  grn_obj *name;
  grn_id id;

  db = grn_ctx_db(ctx);
  name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
  if (GRN_TEXT_LEN(name) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[object][exist] name is missing");
    grn_ctx_output_bool(ctx, GRN_FALSE);
    return NULL;
  }

  id = grn_table_get(ctx, db,
                     GRN_TEXT_VALUE(name),
                     GRN_TEXT_LEN(name));
  grn_ctx_output_bool(ctx, id != GRN_ID_NIL);
  return NULL;
}

void
grn_proc_init_object_exist(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_command_create(ctx,
                            "object_exist", -1,
                            command_object_exist,
                            1,
                            vars);
}

static grn_obj *
command_object_remove(grn_ctx *ctx,
                      int nargs,
                      grn_obj **args,
                      grn_user_data *user_data)
{
  grn_obj *db;
  grn_obj *name;
  grn_bool force;
  grn_obj *target;
  grn_bool failed_to_open;

  db = grn_ctx_db(ctx);
  name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
  force = grn_plugin_proc_get_var_bool(ctx, user_data, "force", -1, GRN_FALSE);

  if (GRN_TEXT_LEN(name) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[object][remove] name is missing");
    grn_ctx_output_bool(ctx, GRN_FALSE);
    return NULL;
  }

  target = grn_ctx_get(ctx,
                       GRN_TEXT_VALUE(name),
                       GRN_TEXT_LEN(name));
  if (target) {
    grn_obj_remove(ctx, target);
    if (!force || ctx->rc == GRN_SUCCESS) {
      grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);
      return NULL;
    }
    grn_obj_close(ctx, target);
    failed_to_open = GRN_TRUE;
  } else {
    failed_to_open = (ctx->rc != GRN_SUCCESS);
  }

  if (force) {
    grn_obj_remove_force(ctx, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name));
    grn_ctx_output_bool(ctx, ctx->rc == GRN_SUCCESS);
  } else {
    if (failed_to_open) {
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[object][remove] "
                       "failed to open the target object: <%.*s>",
                       (int)GRN_TEXT_LEN(name),
                       GRN_TEXT_VALUE(name));
    } else {
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[object][remove] target object doesn't exist: <%.*s>",
                       (int)GRN_TEXT_LEN(name),
                       GRN_TEXT_VALUE(name));
    }
    grn_ctx_output_bool(ctx, GRN_FALSE);
  }

  return NULL;
}

void
grn_proc_init_object_remove(grn_ctx *ctx)
{
  grn_expr_var vars[2];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "force", -1);
  grn_plugin_command_create(ctx,
                            "object_remove", -1,
                            command_object_remove,
                            2,
                            vars);
}
