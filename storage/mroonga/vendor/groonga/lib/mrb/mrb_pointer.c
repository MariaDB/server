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
#include <string.h>

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "mrb_pointer.h"
#include "mrb_object.h"
#include "mrb_converter.h"

static struct mrb_data_type mrb_grn_pointer_type = {
  "Groonga::Pointer",
  NULL
};

static mrb_value
mrb_grn_pointer_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_pointer_ptr;

  mrb_get_args(mrb, "o", &mrb_pointer_ptr);
  DATA_TYPE(self) = &mrb_grn_pointer_type;
  DATA_PTR(self) = mrb_cptr(mrb_pointer_ptr);
  return self;
}

static mrb_value
mrb_grn_pointer_get_value(mrb_state *mrb, mrb_value self)
{
  grn_obj *pointer;

  pointer = DATA_PTR(self);
  if (GRN_BULK_VSIZE(pointer) == 0) {
    return mrb_nil_value();
  }

  return grn_mrb_value_from_grn_obj(mrb, GRN_PTR_VALUE(pointer));
}

void
grn_mrb_pointer_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Pointer", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_pointer_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "value",
                    mrb_grn_pointer_get_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "inspect",
                    grn_mrb_object_inspect, MRB_ARGS_NONE());
}
#endif
