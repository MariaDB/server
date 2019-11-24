/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2015 Brazil

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

#include "mrb_ctx.h"
#include "mrb_array.h"

static struct mrb_data_type mrb_grn_array_type = {
  "Groonga::Array",
  NULL
};

static mrb_value
mrb_grn_array_class_create(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *name;
  mrb_int name_length;
  const char *path = NULL;
  mrb_value mrb_value_type;
  grn_obj *value_type = NULL;
  grn_obj *array;

  mrb_get_args(mrb, "so", &name, &name_length, &mrb_value_type);
  if (!mrb_nil_p(mrb_value_type)) {
    value_type = DATA_PTR(mrb_value_type);
  }

  array = grn_table_create(ctx,
                           name, name_length,
                           path,
                           GRN_TABLE_NO_KEY,
                           NULL,
                           value_type);
  grn_mrb_ctx_check(mrb);

  return mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, array));
}

static mrb_value
mrb_grn_array_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_array_ptr;

  mrb_get_args(mrb, "o", &mrb_array_ptr);
  DATA_TYPE(self) = &mrb_grn_array_type;
  DATA_PTR(self) = mrb_cptr(mrb_array_ptr);
  return self;
}

void
grn_mrb_array_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *table_class;
  struct RClass *klass;

  table_class = mrb_class_get_under(mrb, module, "Table");
  klass = mrb_define_class_under(mrb, module, "Array", table_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "create",
                          mrb_grn_array_class_create,
                          MRB_ARGS_REQ(2));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_array_initialize, MRB_ARGS_REQ(1));
}
#endif
