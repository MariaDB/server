/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2017 Brazil

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
#include "../grn_proc.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "mrb_ctx.h"
#include "mrb_column.h"
#include "mrb_bulk.h"
#include "mrb_converter.h"

static mrb_value
mrb_grn_column_class_parse_flags(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *error_message_tag;
  char *flags_text;
  mrb_int flags_text_size;
  grn_column_flags flags;

  mrb_get_args(mrb, "zs", &error_message_tag, &flags_text, &flags_text_size);

  flags = grn_proc_column_parse_flags(ctx,
                                      error_message_tag,
                                      flags_text,
                                      flags_text + flags_text_size);
  return mrb_fixnum_value(flags);
}

static mrb_value
mrb_grn_column_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *column;
  mrb_int record_id;
  grn_obj *column_value;

  column = DATA_PTR(self);
  mrb_get_args(mrb, "i", &record_id);

  column_value = grn_obj_get_value(ctx, column, record_id, NULL);
  return grn_mrb_value_from_grn_obj(mrb, column_value);
}

static mrb_value
mrb_grn_column_is_scalar(mrb_state *mrb, mrb_value self)
{
  grn_obj *column;
  grn_obj_flags column_type;

  column = DATA_PTR(self);
  column_type = (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK);

  return mrb_bool_value(column_type == GRN_OBJ_COLUMN_SCALAR);
}

static mrb_value
mrb_grn_column_is_vector(mrb_state *mrb, mrb_value self)
{
  grn_obj *column;
  grn_obj_flags column_type;

  column = DATA_PTR(self);
  column_type = (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK);

  return mrb_bool_value(column_type == GRN_OBJ_COLUMN_VECTOR);
}

static mrb_value
mrb_grn_column_is_index(mrb_state *mrb, mrb_value self)
{
  grn_obj *column;
  grn_obj_flags column_type;

  column = DATA_PTR(self);
  column_type = (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK);

  return mrb_bool_value(column_type == GRN_OBJ_COLUMN_INDEX);
}

static mrb_value
mrb_grn_column_is_locked(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  unsigned int is_locked;

  is_locked = grn_obj_is_locked(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_bool_value(is_locked != 0);
}

static mrb_value
mrb_grn_column_get_table(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;

  table = grn_column_table(ctx, DATA_PTR(self));
  if (!table) {
    return mrb_nil_value();
  }

  return grn_mrb_value_from_grn_obj(mrb, table);
}

static mrb_value
mrb_grn_column_truncate(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *column;

  column = DATA_PTR(self);
  grn_column_truncate(ctx, column);
  grn_mrb_ctx_check(mrb);
  return mrb_nil_value();
}

void
grn_mrb_column_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *object_class = data->object_class;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Column", object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "parse_flags",
                          mrb_grn_column_class_parse_flags, MRB_ARGS_REQ(2));

  mrb_define_method(mrb, klass, "[]",
                    mrb_grn_column_array_reference, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "scalar?",
                    mrb_grn_column_is_scalar, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "vector?",
                    mrb_grn_column_is_vector, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "index?",
                    mrb_grn_column_is_index, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "locked?",
                    mrb_grn_column_is_locked, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "table",
                    mrb_grn_column_get_table, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "truncate",
                    mrb_grn_column_truncate, MRB_ARGS_NONE());
}
#endif
