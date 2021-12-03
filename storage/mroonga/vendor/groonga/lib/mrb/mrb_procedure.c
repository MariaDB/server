/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2016 Brazil

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

#include "mrb_procedure.h"

#include "mrb_operator.h"

static struct mrb_data_type mrb_grn_procedure_type = {
  "Groonga::Procedure",
  NULL
};

static mrb_value
mrb_grn_procedure_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_procedure_ptr;

  mrb_get_args(mrb, "o", &mrb_procedure_ptr);
  DATA_TYPE(self) = &mrb_grn_procedure_type;
  DATA_PTR(self) = mrb_cptr(mrb_procedure_ptr);
  return self;
}

static mrb_value
mrb_grn_procedure_selector_p(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *proc = DATA_PTR(self);

  return mrb_bool_value(grn_obj_is_selector_proc(ctx, proc));
}

static mrb_value
mrb_grn_procedure_selector_only_p(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *proc = DATA_PTR(self);

  return mrb_bool_value(grn_obj_is_selector_only_proc(ctx, proc));
}

static mrb_value
mrb_grn_procedure_scorer_p(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *proc = DATA_PTR(self);

  return mrb_bool_value(grn_obj_is_scorer_proc(ctx, proc));
}

static mrb_value
mrb_grn_procedure_get_selector_operator(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *proc = DATA_PTR(self);
  grn_operator selector_op;

  selector_op = grn_proc_get_selector_operator(ctx, proc);
  return grn_mrb_value_from_operator(mrb, selector_op);
}

void
grn_mrb_procedure_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *object_class = data->object_class;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Procedure", object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_procedure_initialize, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "selector?",
                    mrb_grn_procedure_selector_p, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "selector_only?",
                    mrb_grn_procedure_selector_only_p, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "scorer?",
                    mrb_grn_procedure_scorer_p, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "selector_operator",
                    mrb_grn_procedure_get_selector_operator, MRB_ARGS_NONE());
}
#endif
