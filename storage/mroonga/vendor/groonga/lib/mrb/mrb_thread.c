/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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

#include "mrb_thread.h"

static mrb_value
thread_limit(mrb_state *mrb, mrb_value self)
{
  uint32_t limit;
  limit = grn_thread_get_limit();
  return mrb_fixnum_value(limit);
}

void
grn_mrb_thread_init(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *thread_module;

  thread_module = mrb_define_module_under(mrb, module, "Thread");

  mrb_define_class_method(mrb, thread_module,
                          "limit", thread_limit, MRB_ARGS_NONE());
}
#endif
