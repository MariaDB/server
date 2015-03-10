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

#include <grn_mrb.h>
#include <grn_output.h>
#include <grn_db.h>
#include <grn_ctx_impl.h>
#include <grn_util.h>

#include <groonga/plugin.h>

#include <mruby.h>

#define VAR GRN_PROC_GET_VAR_BY_OFFSET

static void
output_result(grn_ctx *ctx, mrb_value result)
{
  mrb_state *mrb = ctx->impl->mrb.state;

  GRN_OUTPUT_MAP_OPEN("result", 1);
  if (mrb->exc) {
    mrb_value mrb_message;
    grn_obj grn_message;
    GRN_OUTPUT_CSTR("exception");
    GRN_OUTPUT_MAP_OPEN("exception", 1);
    GRN_OUTPUT_CSTR("message");
    mrb_message = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "message", 0);
    GRN_VOID_INIT(&grn_message);
    if (grn_mrb_to_grn(ctx, mrb_message, &grn_message) == GRN_SUCCESS) {
      GRN_OUTPUT_OBJ(&grn_message, NULL);
    } else {
      GRN_OUTPUT_CSTR("unsupported message type");
    }
    grn_obj_unlink(ctx, &grn_message);
    GRN_OUTPUT_MAP_CLOSE();
  } else {
    grn_obj grn_result;
    GRN_OUTPUT_CSTR("value");
    GRN_VOID_INIT(&grn_result);
    if (grn_mrb_to_grn(ctx, result, &grn_result) == GRN_SUCCESS) {
      GRN_OUTPUT_OBJ(&grn_result, NULL);
    } else {
      GRN_OUTPUT_CSTR("unsupported return value");
    }
    grn_obj_unlink(ctx, &grn_result);
  }
  GRN_OUTPUT_MAP_CLOSE();
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
