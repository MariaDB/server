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
#include <mruby/class.h>

#include "mrb_table_sort_flags.h"

void
grn_mrb_table_sort_flags_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *flags_module;

  flags_module = mrb_define_module_under(mrb, module, "TableSortFlags");

  mrb_define_const(mrb, flags_module, "ASCENDING",
                   mrb_fixnum_value(GRN_TABLE_SORT_ASC));
  mrb_define_const(mrb, flags_module, "DESCENDING",
                   mrb_fixnum_value(GRN_TABLE_SORT_DESC));
}
#endif
