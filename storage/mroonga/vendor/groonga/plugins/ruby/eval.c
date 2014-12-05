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
command_ruby_eval(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  mrb_state *mrb = ctx->impl->mrb.state;
  grn_obj *script;
  mrb_value result;

  script = VAR(0);
  switch (script->header.domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    break;
  default :
    {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, script);
      ERR(GRN_INVALID_ARGUMENT, "script must be a string: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return NULL;
    }
    break;
  }

  mrb->exc = NULL;
  result = grn_mrb_eval(ctx, GRN_TEXT_VALUE(script), GRN_TEXT_LEN(script));
  output_result(ctx, result);

  return NULL;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_expr_var vars[1];

  grn_plugin_expr_var_init(ctx, &vars[0], "script", -1);
  grn_plugin_command_create(ctx, "ruby_eval", -1, command_ruby_eval, 1, vars);

  return ctx->rc;
}
