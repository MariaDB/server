/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2017 Brazil

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
#include "mrb_logger.h"

static mrb_value
logger_s_get_default_path(mrb_state *mrb, mrb_value self)
{
  return mrb_str_new_cstr(mrb, grn_default_logger_get_path());
}

static mrb_value
logger_s_get_default_level(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_level_class;
  mrb_value mrb_level;

  mrb_level_class = mrb_const_get(mrb, self, mrb_intern_lit(mrb, "Level"));
  mrb_level = mrb_fixnum_value(grn_default_logger_get_max_level());
  return mrb_funcall(mrb, mrb_level_class, "find", 1, mrb_level);
}

static mrb_value
logger_need_log_p(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int level;

  mrb_get_args(mrb, "i", &level);

  return mrb_bool_value(grn_logger_pass(ctx, level));
}

static mrb_value
logger_log(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int level;
  char *file;
  mrb_int line;
  char *method;
  char *message;
  mrb_int message_size;

  mrb_get_args(mrb, "izizs",
               &level, &file, &line, &method, &message, &message_size);
  grn_logger_put(ctx, level, file, line, method,
                 "%.*s", (int)message_size, message);

  return self;
}

void
grn_mrb_logger_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Logger", mrb->object_class);

  mrb_define_singleton_method(mrb, (struct RObject *)klass, "default_path",
                              logger_s_get_default_path, MRB_ARGS_NONE());
  mrb_define_singleton_method(mrb, (struct RObject *)klass, "default_level",
                              logger_s_get_default_level, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "need_log?", logger_need_log_p, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "log", logger_log, MRB_ARGS_REQ(5));

  grn_mrb_load(ctx, "logger/level.rb");
  grn_mrb_load(ctx, "logger.rb");
}
#endif
