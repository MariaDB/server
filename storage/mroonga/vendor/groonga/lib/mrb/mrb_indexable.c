/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2016 Brazil

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
#include "../grn_db.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/data.h>

#include "mrb_ctx.h"
#include "mrb_indexable.h"
#include "mrb_operator.h"
#include "mrb_converter.h"

static mrb_value
indexable_find_index(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  mrb_value mrb_operator;
  grn_operator operator;
  grn_index_datum index_datum;
  int n_index_data;

  mrb_get_args(mrb, "o", &mrb_operator);
  object = DATA_PTR(self);
  operator = grn_mrb_value_to_operator(mrb, mrb_operator);
  n_index_data = grn_column_find_index_data(ctx,
                                            object,
                                            operator,
                                            &index_datum,
                                            1);
  if (n_index_data == 0) {
    return mrb_nil_value();
  } else {
    grn_mrb_data *data;
    struct RClass *klass;
    mrb_value args[2];

    data = &(ctx->impl->mrb);
    klass = mrb_class_get_under(mrb, data->module, "IndexInfo");
    args[0] = grn_mrb_value_from_grn_obj(mrb, index_datum.index);
    args[1] = mrb_fixnum_value(index_datum.section);
    return mrb_obj_new(mrb, klass, 2, args);
  }
}

static mrb_value
indexable_indexes(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  grn_index_datum index_datum;
  grn_index_datum *index_data;
  int i, n_index_data;
  mrb_value mrb_indexes;

  object = DATA_PTR(self);
  n_index_data = grn_column_get_all_index_data(ctx, object, &index_datum, 1);
  if (n_index_data == 0) {
    return mrb_ary_new(mrb);
  }

  if (n_index_data == 1) {
    index_data = &index_datum;
  } else {
    index_data = GRN_MALLOCN(grn_index_datum, n_index_data);
    n_index_data = grn_column_get_all_index_data(ctx,
                                                 object,
                                                 index_data,
                                                 n_index_data);
  }

  mrb_indexes = mrb_ary_new_capa(mrb, n_index_data);
  for (i = 0; i < n_index_data; i++) {
    grn_mrb_data *data;
    struct RClass *klass;
    mrb_value args[2];

    data = &(ctx->impl->mrb);
    klass = mrb_class_get_under(mrb, data->module, "IndexInfo");
    args[0] = grn_mrb_value_from_grn_obj(mrb, index_data[i].index);
    args[1] = mrb_fixnum_value(index_data[i].section);
    mrb_ary_push(mrb, mrb_indexes, mrb_obj_new(mrb, klass, 2, args));
  }

  if (index_data != &index_datum) {
    GRN_FREE(index_data);
  }

  return mrb_indexes;
}

static mrb_value
indexable_index_ids(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  grn_hook_entry entry;
  int i;
  int n_indexes;
  mrb_value mrb_index_ids;
  grn_obj hook_data;

  object = DATA_PTR(self);

  if (grn_obj_is_key_accessor(ctx, object)) {
    object = grn_ctx_at(ctx, object->header.domain);
  }
  if (grn_obj_is_table(ctx, object)) {
    entry = GRN_HOOK_INSERT;
  } else if (grn_obj_is_column(ctx, object)) {
    entry = GRN_HOOK_SET;
  } else {
    return mrb_ary_new(mrb);
  }
  n_indexes = grn_obj_get_nhooks(ctx, object, entry);

  mrb_index_ids = mrb_ary_new_capa(mrb, n_indexes);

  GRN_TEXT_INIT(&hook_data, 0);
  for (i = 0; i < n_indexes; i++) {
    GRN_BULK_REWIND(&hook_data);
    grn_obj_get_hook(ctx, object, entry, i, &hook_data);
    if (GRN_BULK_VSIZE(&hook_data) ==
        sizeof(grn_obj_default_set_value_hook_data)) {
      grn_obj_default_set_value_hook_data *data;

      data = (grn_obj_default_set_value_hook_data *)GRN_TEXT_VALUE(&hook_data);
      mrb_ary_push(mrb, mrb_index_ids, mrb_fixnum_value(data->target));
    }
  }

  return mrb_index_ids;
}

void
grn_mrb_indexable_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module;

  module = mrb_define_module_under(mrb, data->module, "Indexable");

  mrb_define_method(mrb, module, "find_index",
                    indexable_find_index, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, module, "indexes",
                    indexable_indexes, MRB_ARGS_NONE());
  mrb_define_method(mrb, module, "index_ids",
                    indexable_index_ids, MRB_ARGS_NONE());
}
#endif
