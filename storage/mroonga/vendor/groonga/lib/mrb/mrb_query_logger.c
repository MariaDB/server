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
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/string.h>

#include "../grn_mrb.h"
#include "mrb_query_logger.h"

static mrb_value
query_logger_need_log_p(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int flag;

  mrb_get_args(mrb, "i", &flag);

  return mrb_bool_value(grn_query_logger_pass(ctx, flag));
}

static mrb_value
query_logger_log_raw(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int flag;
  char *mark;
  char *message;
  mrb_int message_size;

  mrb_get_args(mrb, "izs", &flag, &mark, &message, &message_size);
  grn_query_logger_put(ctx, flag, mark,
                       "%.*s", (int)message_size, message);

  return self;
}

void
grn_mrb_query_logger_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "QueryLogger", mrb->object_class);

  mrb_define_method(mrb, klass, "need_log?", query_logger_need_log_p,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "log_raw", query_logger_log_raw,
                    MRB_ARGS_REQ(3));

  grn_mrb_load(ctx, "query_logger/flag.rb");
  grn_mrb_load(ctx, "query_logger.rb");
}
#endif
