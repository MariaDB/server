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
#include <mruby/variable.h>
#include <mruby/data.h>
#include <mruby/array.h>

#include "../expr.h"
#include "../util.h"
#include "../mrb.h"
#include "mrb_accessor.h"
#include "mrb_expr.h"
#include "mrb_converter.h"

static struct mrb_data_type mrb_grn_scan_info_type = {
  "Groonga::ScanInfo",
  NULL
};
static struct mrb_data_type mrb_grn_expr_code_type = {
  "Groonga::ExpressionCode",
  NULL
};
static struct mrb_data_type mrb_grn_expression_type = {
  "Groonga::Expression",
  NULL
};

static mrb_value
mrb_grn_scan_info_new(mrb_state *mrb, scan_info *scan_info)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *klass;
  mrb_value mrb_scan_info;

  mrb_scan_info = mrb_cptr_value(mrb, scan_info);
  klass = mrb_class_get_under(mrb, module, "ScanInfo");
  return mrb_obj_new(mrb, klass, 1, &mrb_scan_info);
}

static mrb_value
mrb_grn_expr_code_new(mrb_state *mrb, grn_expr_code *code)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *klass;
  mrb_value mrb_code;

  mrb_code = mrb_cptr_value(mrb, code);
  klass = mrb_class_get_under(mrb, module, "ExpressionCode");
  return mrb_obj_new(mrb, klass, 1, &mrb_code);
}

static mrb_value
mrb_grn_scan_info_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_ptr;

  mrb_get_args(mrb, "o", &mrb_ptr);
  DATA_TYPE(self) = &mrb_grn_scan_info_type;
  DATA_PTR(self) = mrb_cptr(mrb_ptr);
  return self;
}

static mrb_value
mrb_grn_expr_code_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_code;

  mrb_get_args(mrb, "o", &mrb_code);
  DATA_TYPE(self) = &mrb_grn_expr_code_type;
  DATA_PTR(self) = mrb_cptr(mrb_code);
  return self;
}

static mrb_value
mrb_grn_scan_info_put_index(mrb_state *mrb, mrb_value self)
{
  int sid;
  int32_t weight;
  scan_info *si;
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index;
  mrb_value mrb_index;

  mrb_get_args(mrb, "oii", &mrb_index, &sid, &weight);
  si = DATA_PTR(self);
  index = DATA_PTR(mrb_index);
  grn_scan_info_put_index(ctx, si, index, sid, weight);
  return self;
}

static mrb_value
mrb_grn_scan_info_get_op(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  grn_operator op;

  si = DATA_PTR(self);
  op = grn_scan_info_get_op(si);
  return mrb_fixnum_value(op);
}

static mrb_value
mrb_grn_scan_info_set_op(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  grn_operator op;

  mrb_get_args(mrb, "i", &op);
  si = DATA_PTR(self);
  grn_scan_info_set_op(si, op);
  return self;
}

static mrb_value
mrb_grn_scan_info_set_end(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  int end;

  mrb_get_args(mrb, "i", &end);
  si = DATA_PTR(self);
  grn_scan_info_set_end(si, end);
  return self;
}

static mrb_value
mrb_grn_scan_info_set_query(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  mrb_value mrb_query;

  mrb_get_args(mrb, "o", &mrb_query);
  si = DATA_PTR(self);
  if (mrb_nil_p(mrb_query)) {
    grn_scan_info_set_query(si, NULL);
  } else {
    grn_scan_info_set_query(si, DATA_PTR(mrb_query));
  }
  return self;
}

static mrb_value
mrb_grn_scan_info_set_flags(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  int flags;

  mrb_get_args(mrb, "i", &flags);
  si = DATA_PTR(self);
  grn_scan_info_set_flags(si, flags);
  return self;
}

static mrb_value
mrb_grn_scan_info_get_flags(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  int flags;

  si = DATA_PTR(self);
  flags = grn_scan_info_get_flags(si);
  return mrb_fixnum_value(flags);
}

static mrb_value
mrb_grn_scan_info_set_logical_op(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  grn_operator logical_op;

  mrb_get_args(mrb, "i", &logical_op);
  si = DATA_PTR(self);
  grn_scan_info_set_logical_op(si, logical_op);
  return self;
}

static mrb_value
mrb_grn_scan_info_get_logical_op(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  grn_operator logical_op;

  si = DATA_PTR(self);
  logical_op = grn_scan_info_get_logical_op(si);
  return mrb_fixnum_value(logical_op);
}

static mrb_value
mrb_grn_scan_info_set_max_interval(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  int max_interval;

  mrb_get_args(mrb, "i", &max_interval);
  si = DATA_PTR(self);
  grn_scan_info_set_max_interval(si, max_interval);
  return self;
}

static mrb_value
mrb_grn_scan_info_get_max_interval(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  int max_interval;

  si = DATA_PTR(self);
  max_interval = grn_scan_info_get_max_interval(si);
  return mrb_fixnum_value(max_interval);
}

static mrb_value
mrb_grn_scan_info_get_arg(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  scan_info *si;
  int index;
  grn_obj *arg;

  mrb_get_args(mrb, "i", &index);

  si = DATA_PTR(self);
  arg = grn_scan_info_get_arg(ctx, si, index);

  return grn_mrb_value_from_grn_obj(mrb, arg);
}

static mrb_value
mrb_grn_scan_info_push_arg(mrb_state *mrb, mrb_value self)
{
  scan_info *si;
  mrb_value mrb_arg;
  grn_bool success;

  mrb_get_args(mrb, "o", &mrb_arg);

  si = DATA_PTR(self);
  success = grn_scan_info_push_arg(si, DATA_PTR(mrb_arg));

  return mrb_bool_value(success);
}

static mrb_value
mrb_grn_expr_code_get_weight(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(grn_expr_code_get_weight(ctx, DATA_PTR(self)));
}

static mrb_value
mrb_grn_expr_code_get_value(mrb_state *mrb, mrb_value self)
{
  grn_expr_code *expr_code;

  expr_code = DATA_PTR(self);
  return grn_mrb_value_from_grn_obj(mrb, expr_code->value);
}

static mrb_value
mrb_grn_expr_code_get_op(mrb_state *mrb, mrb_value self)
{
  grn_expr_code *expr_code;

  expr_code = DATA_PTR(self);
  return mrb_fixnum_value(expr_code->op);
}

static mrb_value
mrb_grn_expr_code_get_flags(mrb_state *mrb, mrb_value self)
{
  grn_expr_code *expr_code;

  expr_code = DATA_PTR(self);
  return mrb_fixnum_value(expr_code->flags);
}

static mrb_value
mrb_grn_expression_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_expression_ptr;

  mrb_get_args(mrb, "o", &mrb_expression_ptr);
  DATA_TYPE(self) = &mrb_grn_expression_type;
  DATA_PTR(self) = mrb_cptr(mrb_expression_ptr);
  return self;
}

static mrb_value
mrb_grn_expression_codes(mrb_state *mrb, mrb_value self)
{
  grn_expr *expr;
  mrb_value mrb_codes;
  int i;

  expr = DATA_PTR(self);
  mrb_codes = mrb_ary_new_capa(mrb, expr->codes_curr);
  for (i = 0; i < expr->codes_curr; i++) {
    grn_expr_code *code = expr->codes + i;
    mrb_ary_push(mrb, mrb_codes, mrb_grn_expr_code_new(mrb, code));
  }

  return mrb_codes;
}

static mrb_value
mrb_grn_expression_get_var_by_offset(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *expr;
  mrb_int offset;
  grn_obj *var;

  mrb_get_args(mrb, "i", &offset);

  expr = DATA_PTR(self);
  var = grn_expr_get_var_by_offset(ctx, expr, offset);
  return grn_mrb_value_from_grn_obj(mrb, var);
}

void
grn_mrb_expr_init(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *object_class = ctx->impl->mrb.object_class;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "ScanInfo", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_scan_info_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "put_index",
                    mrb_grn_scan_info_put_index, MRB_ARGS_REQ(3));
  mrb_define_method(mrb, klass, "op",
                    mrb_grn_scan_info_get_op, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "op=",
                    mrb_grn_scan_info_set_op, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "end=",
                    mrb_grn_scan_info_set_end, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "query=",
                    mrb_grn_scan_info_set_query, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "flags",
                    mrb_grn_scan_info_get_flags, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "flags=",
                    mrb_grn_scan_info_set_flags, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "logical_op",
                    mrb_grn_scan_info_get_logical_op, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "logical_op=",
                    mrb_grn_scan_info_set_logical_op, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "max_interval",
                    mrb_grn_scan_info_get_max_interval, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "max_interval=",
                    mrb_grn_scan_info_set_max_interval, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "get_arg",
                    mrb_grn_scan_info_get_arg, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "push_arg",
                    mrb_grn_scan_info_push_arg, MRB_ARGS_REQ(1));

  klass = mrb_define_class_under(mrb, module,
                                 "ExpressionCode", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_expr_code_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "weight",
                    mrb_grn_expr_code_get_weight, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "value",
                    mrb_grn_expr_code_get_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "op",
                    mrb_grn_expr_code_get_op, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "flags",
                    mrb_grn_expr_code_get_flags, MRB_ARGS_NONE());

  klass = mrb_define_class_under(mrb, module, "Expression", object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_expression_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "codes",
                    mrb_grn_expression_codes, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "get_var_by_offset",
                    mrb_grn_expression_get_var_by_offset, MRB_ARGS_REQ(1));

  grn_mrb_load(ctx, "expression.rb");
  grn_mrb_load(ctx, "scan_info.rb");
  grn_mrb_load(ctx, "scan_info_data.rb");
  grn_mrb_load(ctx, "scan_info_builder.rb");
}

scan_info **
grn_mrb_scan_info_build(grn_ctx *ctx, grn_obj *expr, int *n,
                        grn_operator op, uint32_t size)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  mrb_value mrb_expression;
  mrb_value mrb_sis;
  scan_info **sis = NULL;
  int i;
  int arena_index;

  arena_index = mrb_gc_arena_save(mrb);

  mrb_expression = grn_mrb_value_from_grn_obj(mrb, expr);
  mrb_sis = mrb_funcall(mrb, mrb_expression, "build_scan_info", 2,
                        mrb_fixnum_value(op),
                        mrb_fixnum_value(size));

  if (mrb_nil_p(mrb_sis)) {
    goto exit;
  }

  if (mrb_type(mrb_sis) == MRB_TT_EXCEPTION) {
    mrb->exc = mrb_obj_ptr(mrb_sis);
    mrb_print_error(mrb);
    goto exit;
  }

  *n = RARRAY_LEN(mrb_sis);
  sis = GRN_MALLOCN(scan_info *, *n);
  for (i = 0; i < *n; i++) {
    mrb_value mrb_si;
    mrb_value mrb_si_data;
    scan_info *si;
    int start;

    mrb_si_data = RARRAY_PTR(mrb_sis)[i];
    start = mrb_fixnum(mrb_funcall(mrb, mrb_si_data, "start", 0));
    si = grn_scan_info_open(ctx, start);
    mrb_si = mrb_grn_scan_info_new(mrb, si);
    mrb_funcall(mrb, mrb_si, "apply", 1, mrb_si_data);
    sis[i] = si;
  }

exit:
  mrb_gc_arena_restore(mrb, arena_index);

  return sis;
}
#endif
