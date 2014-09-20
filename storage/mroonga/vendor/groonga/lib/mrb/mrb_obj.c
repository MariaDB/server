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
#include "../util.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "../mrb.h"
#include "mrb_obj.h"
#include "mrb_converter.h"

static mrb_value
object_get_name(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_length;

  object = DATA_PTR(self);
  name_length = grn_obj_name(ctx, object, name, GRN_TABLE_MAX_KEY_SIZE);

  return mrb_str_new(mrb, name, name_length);
}

static mrb_value
object_find_index(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  mrb_value mrb_operator;
  grn_obj *index;
  int n_indexes;
  int section_id;

  mrb_get_args(mrb, "o", &mrb_operator);
  object = DATA_PTR(self);
  n_indexes = grn_column_index(ctx,
                               object,
                               mrb_fixnum(mrb_operator),
                               &index,
                               1,
                               &section_id);
  if (n_indexes == 0) {
    return mrb_nil_value();
  } else {
    grn_mrb_data *data;
    struct RClass *klass;
    mrb_value args[2];

    data = &(ctx->impl->mrb);
    klass = mrb_class_get_under(mrb, data->module, "IndexInfo");
    args[0] = grn_mrb_value_from_grn_obj(mrb, index);
    args[1] = mrb_fixnum_value(section_id);
    return mrb_obj_new(mrb, klass, 2, args);
  }
}

static mrb_value
object_grn_inspect(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj buffer;
  mrb_value inspected;

  GRN_TEXT_INIT(&buffer, 0);
  grn_inspect(ctx, &buffer, DATA_PTR(self));
  inspected = mrb_str_new(mrb, GRN_TEXT_VALUE(&buffer), GRN_TEXT_LEN(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);

  return inspected;
}

void
grn_mrb_obj_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Object", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  data->object_class = klass;

  mrb_define_method(mrb, klass, "name", object_get_name, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "find_index",
                    object_find_index, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "grn_inspect",
                    object_grn_inspect, MRB_ARGS_NONE());

  grn_mrb_load(ctx, "index_info.rb");
}
#endif
