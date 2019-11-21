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
#include <mruby/hash.h>
#include <mruby/array.h>
#include <mruby/string.h>

#include "mrb_ctx.h"
#include "mrb_converter.h"
#include "mrb_operator.h"
#include "mrb_table_group_result.h"

static void
mrb_grn_table_group_result_free(mrb_state *mrb, void *data)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_table_group_result *result = data;

  if (!result) {
    return;
  }

  if (result->calc_target) {
    grn_obj_unlink(ctx, result->calc_target);
  }
  if (result->table) {
    grn_obj_unlink(ctx, result->table);
  }
  mrb_free(mrb, result);
}

static struct mrb_data_type mrb_grn_table_group_result_type = {
  "Groonga::TableGroupResult",
  mrb_grn_table_group_result_free
};

static mrb_value
mrb_grn_table_group_result_initialize(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;

  DATA_TYPE(self) = &mrb_grn_table_group_result_type;

  result = mrb_calloc(mrb, 1, sizeof(grn_table_group_result));
  DATA_PTR(self) = result;

  return self;
}


static mrb_value
mrb_grn_table_group_result_close(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;

  result = DATA_PTR(self);
  if (result) {
    mrb_grn_table_group_result_free(mrb, result);
    DATA_PTR(self) = NULL;
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_get_table(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;

  result = DATA_PTR(self);

  return grn_mrb_value_from_grn_obj(mrb, result->table);
}

static mrb_value
mrb_grn_table_group_result_set_table(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_value mrb_table;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_table);

  if (mrb_nil_p(mrb_table)) {
    result->table = NULL;
  } else {
    result->table = DATA_PTR(mrb_table);
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_key_begin(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_int key_begin;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "i", &key_begin);

  result->key_begin = key_begin;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_key_end(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_int key_end;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "i", &key_end);

  result->key_end = key_end;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_limit(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_int limit;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "i", &limit);

  result->limit = limit;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_flags(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_int flags;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "i", &flags);

  result->flags = flags;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_operator(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_value mrb_operator;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_operator);

  result->op = grn_mrb_value_to_operator(mrb, mrb_operator);

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_max_n_sub_records(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_int max_n_sub_records;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "i", &max_n_sub_records);

  result->max_n_subrecs = max_n_sub_records;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_group_result_set_calc_target(mrb_state *mrb, mrb_value self)
{
  grn_table_group_result *result;
  mrb_value mrb_calc_target;

  result = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_calc_target);

  if (mrb_nil_p(mrb_calc_target)) {
    result->calc_target = NULL;
  } else {
    result->calc_target = DATA_PTR(mrb_calc_target);
  }

  return mrb_nil_value();
}

void
grn_mrb_table_group_result_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "TableGroupResult",
                                 mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_table_group_result_initialize, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "close",
                    mrb_grn_table_group_result_close, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "table",
                    mrb_grn_table_group_result_get_table, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "table=",
                    mrb_grn_table_group_result_set_table, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "key_begin=",
                    mrb_grn_table_group_result_set_key_begin, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "key_end=",
                    mrb_grn_table_group_result_set_key_end, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "limit=",
                    mrb_grn_table_group_result_set_limit, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "flags=",
                    mrb_grn_table_group_result_set_flags, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "operator=",
                    mrb_grn_table_group_result_set_operator, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "max_n_sub_records=",
                    mrb_grn_table_group_result_set_max_n_sub_records,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "calc_target=",
                    mrb_grn_table_group_result_set_calc_target, MRB_ARGS_REQ(1));
}
#endif
