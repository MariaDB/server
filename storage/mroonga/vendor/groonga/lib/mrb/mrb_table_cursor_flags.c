/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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
#include <mruby/class.h>

#include "mrb_table_cursor_flags.h"

void
grn_mrb_table_cursor_flags_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *flags_module;

  flags_module = mrb_define_module_under(mrb, module, "TableCursorFlags");

  mrb_define_const(mrb, flags_module, "ASCENDING",
                   mrb_fixnum_value(GRN_CURSOR_ASCENDING));
  mrb_define_const(mrb, flags_module, "DESCENDING",
                   mrb_fixnum_value(GRN_CURSOR_DESCENDING));
  mrb_define_const(mrb, flags_module, "GE",
                   mrb_fixnum_value(GRN_CURSOR_GE));
  mrb_define_const(mrb, flags_module, "GT",
                   mrb_fixnum_value(GRN_CURSOR_GT));
  mrb_define_const(mrb, flags_module, "LE",
                   mrb_fixnum_value(GRN_CURSOR_LE));
  mrb_define_const(mrb, flags_module, "LT",
                   mrb_fixnum_value(GRN_CURSOR_LT));
  mrb_define_const(mrb, flags_module, "BY_KEY",
                   mrb_fixnum_value(GRN_CURSOR_BY_KEY));
  mrb_define_const(mrb, flags_module, "BY_ID",
                   mrb_fixnum_value(GRN_CURSOR_BY_ID));
  mrb_define_const(mrb, flags_module, "PREFIX",
                   mrb_fixnum_value(GRN_CURSOR_PREFIX));
  mrb_define_const(mrb, flags_module, "SIZE_BY_BIT",
                   mrb_fixnum_value(GRN_CURSOR_SIZE_BY_BIT));
  mrb_define_const(mrb, flags_module, "RK",
                   mrb_fixnum_value(GRN_CURSOR_RK));
}
#endif
