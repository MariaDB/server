/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015 Brazil

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
#include <mruby/variable.h>
#include <mruby/data.h>

#include "../grn_db.h"
#include "mrb_ctx.h"
#include "mrb_accessor.h"
#include "mrb_converter.h"

static struct mrb_data_type mrb_grn_accessor_type = {
  "Groonga::Accessor",
  NULL
};

static mrb_value
mrb_grn_accessor_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_accessor_ptr;

  mrb_get_args(mrb, "o", &mrb_accessor_ptr);
  DATA_TYPE(self) = &mrb_grn_accessor_type;
  DATA_PTR(self) = mrb_cptr(mrb_accessor_ptr);
  return self;
}

static mrb_value
mrb_grn_accessor_next(mrb_state *mrb, mrb_value self)
{
  grn_accessor *accessor;

  accessor = DATA_PTR(self);
  return grn_mrb_value_from_grn_obj(mrb, (grn_obj *)(accessor->next));
}

static mrb_value
mrb_grn_accessor_have_next_p(mrb_state *mrb, mrb_value self)
{
  grn_accessor *accessor;

  accessor = DATA_PTR(self);
  return mrb_bool_value(accessor->next != NULL);
}

static mrb_value
mrb_grn_accessor_object(mrb_state *mrb, mrb_value self)
{
  grn_accessor *accessor;

  accessor = DATA_PTR(self);
  return grn_mrb_value_from_grn_obj(mrb, accessor->obj);
}

static mrb_value
mrb_grn_accessor_name(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_rc rc;
  grn_obj *accessor;
  grn_obj name;
  mrb_value mrb_name;

  accessor = DATA_PTR(self);
  GRN_TEXT_INIT(&name, 0);
  rc = grn_column_name_(ctx, accessor, &name);
  if (rc == GRN_SUCCESS) {
    mrb_name = mrb_str_new(mrb, GRN_TEXT_VALUE(&name), GRN_TEXT_LEN(&name));
    GRN_OBJ_FIN(ctx, &name);
  } else {
    mrb_name = mrb_nil_value();
    GRN_OBJ_FIN(ctx, &name);
    grn_mrb_ctx_check(mrb);
  }

  return mrb_name;
}

void
grn_mrb_accessor_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Accessor", data->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_accessor_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "next",
                    mrb_grn_accessor_next, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "have_next?",
                    mrb_grn_accessor_have_next_p, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "object",
                    mrb_grn_accessor_object, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "name",
                    mrb_grn_accessor_name, MRB_ARGS_NONE());
}
#endif
