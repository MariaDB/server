/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2014 Brazil

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

#include "../ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/string.h>

#include "../mrb.h"
#include "mrb_ctx.h"
#include "mrb_converter.h"

static mrb_value
ctx_class_instance(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_ctx;

  mrb_ctx = mrb_obj_value(mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_ptr(klass)));
  DATA_PTR(mrb_ctx) = ctx;

  return mrb_ctx;
}

static mrb_value
ctx_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  char *name;
  int name_length;

  mrb_get_args(mrb, "s", &name, &name_length);
  object = grn_ctx_get(ctx, name, name_length);

  return grn_mrb_value_from_grn_obj(mrb, object);
}

static mrb_value
ctx_get_rc(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->rc);
}

static mrb_value
ctx_set_rc(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int rc;

  mrb_get_args(mrb, "i", &rc);
  ctx->rc = rc;

  return mrb_fixnum_value(ctx->rc);
}

static mrb_value
ctx_get_error_level(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->errlvl);
}

static mrb_value
ctx_set_error_level(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int error_level;

  mrb_get_args(mrb, "i", &error_level);
  ctx->errlvl = error_level;

  return mrb_fixnum_value(ctx->errlvl);
}

static mrb_value
ctx_get_error_file(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errfile);
}

static mrb_value
ctx_set_error_file(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_file;

  mrb_get_args(mrb, "S", &error_file);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@error_file"), error_file);
  ctx->errfile = mrb_string_value_cstr(mrb, &error_file);

  return error_file;
}

static mrb_value
ctx_get_error_line(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->errline);
}

static mrb_value
ctx_set_error_line(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int error_line;

  mrb_get_args(mrb, "i", &error_line);
  ctx->errline = error_line;

  return mrb_fixnum_value(ctx->errline);
}

static mrb_value
ctx_get_error_method(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errfunc);
}

static mrb_value
ctx_set_error_method(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_method;

  mrb_get_args(mrb, "S", &error_method);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@error_method"), error_method);
  ctx->errfunc = mrb_string_value_cstr(mrb, &error_method);

  return error_method;
}

static mrb_value
ctx_get_error_message(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errbuf);
}

static mrb_value
ctx_set_error_message(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_message;

  mrb_get_args(mrb, "S", &error_message);
  grn_ctx_log(ctx, "%.*s",
              RSTRING_LEN(error_message),
              RSTRING_PTR(error_message));

  return error_message;
}

void
grn_mrb_ctx_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Context", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "instance",
                          ctx_class_instance, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "[]", ctx_array_reference, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "rc", ctx_get_rc, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "rc=", ctx_set_rc, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_level", ctx_get_error_level,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_level=", ctx_set_error_level,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_file", ctx_get_error_file,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_file=", ctx_set_error_file,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_line", ctx_get_error_line,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_line=", ctx_set_error_line,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_method", ctx_get_error_method,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_method=", ctx_set_error_method,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_message", ctx_get_error_message,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_message=", ctx_set_error_message,
                    MRB_ARGS_REQ(1));

  grn_mrb_load(ctx, "context/error_level.rb");
  grn_mrb_load(ctx, "context/rc.rb");
  grn_mrb_load(ctx, "context.rb");
}
#endif
