/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>

#include "mrb_ctx.h"
#include "mrb_content_type.h"

void
grn_mrb_content_type_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module;

  module = mrb_define_module_under(mrb, data->module, "ContentType");

  mrb_define_const(mrb, module, "NONE",
                   mrb_fixnum_value(GRN_CONTENT_NONE));
  mrb_define_const(mrb, module, "TSV",
                   mrb_fixnum_value(GRN_CONTENT_TSV));
  mrb_define_const(mrb, module, "JSON",
                   mrb_fixnum_value(GRN_CONTENT_JSON));
  mrb_define_const(mrb, module, "XML",
                   mrb_fixnum_value(GRN_CONTENT_XML));
  mrb_define_const(mrb, module, "MSGPACK",
                   mrb_fixnum_value(GRN_CONTENT_MSGPACK));
  mrb_define_const(mrb, module, "GROONGA_COMMAND_LIST",
                   mrb_fixnum_value(GRN_CONTENT_GROONGA_COMMAND_LIST));
}
#endif
