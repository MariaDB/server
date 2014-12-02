/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright(C) 2013 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "ruby_plugin.h"

static grn_obj *
command_ruby_load(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *path;
  mrb_value result;

  path = VAR(0);
  switch (path->header.domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    break;
  default :
    {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, path);
      ERR(GRN_INVALID_ARGUMENT, "path must be a string: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
    break;
  }

  GRN_TEXT_PUTC(ctx, path, '\0');
  result = grn_mrb_load(ctx, GRN_TEXT_VALUE(path));
  output_result(ctx, result);

  return NULL;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &vars[0], "path", -1);
  grn_plugin_command_create(ctx, "ruby_load", -1, command_ruby_load, 1, vars);

  return ctx->rc;
}
