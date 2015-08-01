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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
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
#include "mrb_table.h"
#include "mrb_converter.h"
#include "mrb_options.h"

static mrb_value
mrb_grn_table_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  grn_id key_domain_id;
  mrb_value mrb_key;
  grn_id record_id;
  grn_mrb_value_to_raw_data_buffer buffer;
  void *key;
  unsigned int key_size;

  mrb_get_args(mrb, "o", &mrb_key);

  table = DATA_PTR(self);
  if (table->header.type == GRN_DB) {
    key_domain_id = GRN_DB_SHORT_TEXT;
  } else {
    key_domain_id = table->header.domain;
  }

  grn_mrb_value_to_raw_data_buffer_init(mrb, &buffer);
  grn_mrb_value_to_raw_data(mrb, "key", mrb_key, key_domain_id,
                            &buffer, &key, &key_size);
  record_id = grn_table_get(ctx, table, key, key_size);
  grn_mrb_value_to_raw_data_buffer_fin(mrb, &buffer);

  if (record_id == GRN_ID_NIL) {
    return mrb_nil_value();
  } else {
    return mrb_fixnum_value(record_id);
  }
}

static mrb_value
mrb_grn_table_find_column(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  mrb_value mrb_column_name;
  grn_obj *column;

  mrb_get_args(mrb, "o", &mrb_column_name);

  table = DATA_PTR(self);
  column = grn_obj_column(ctx, table,
                          RSTRING_PTR(mrb_column_name),
                          RSTRING_LEN(mrb_column_name));
  grn_mrb_ctx_check(mrb);

  return grn_mrb_value_from_grn_obj(mrb, column);
}

static mrb_value
mrb_grn_table_is_locked(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  unsigned int is_locked;

  is_locked = grn_obj_is_locked(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_bool_value(is_locked != 0);
}

static mrb_value
mrb_grn_table_get_size(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  unsigned int size;

  size = grn_table_size(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_fixnum_value(size);
}

static mrb_value
mrb_grn_table_is_empty(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  unsigned int size;

  size = grn_table_size(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_bool_value(size == 0);
}

static mrb_value
mrb_grn_table_select(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  grn_obj *expr;
  grn_obj *result = NULL;
  grn_operator operator = GRN_OP_OR;
  mrb_value mrb_expr;
  mrb_value mrb_options = mrb_nil_value();

  table = DATA_PTR(self);
  mrb_get_args(mrb, "o|H", &mrb_expr, &mrb_options);

  expr = DATA_PTR(mrb_expr);

  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_result;
    mrb_value mrb_operator;

    mrb_result = grn_mrb_options_get_lit(mrb, mrb_options, "result");
    if (!mrb_nil_p(mrb_result)) {
      result = DATA_PTR(mrb_result);
    }

    mrb_operator = grn_mrb_options_get_lit(mrb, mrb_options, "operator");
    if (!mrb_nil_p(mrb_operator)) {
      operator = mrb_fixnum(mrb_operator);
    }
  }

  result = grn_table_select(ctx, table, expr, result, operator);
  grn_mrb_ctx_check(mrb);

  return grn_mrb_value_from_grn_obj(mrb, result);
}

static mrb_value
mrb_grn_table_sort_raw(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  mrb_value mrb_keys;
  grn_table_sort_key *keys;
  int i, n_keys;
  mrb_int offset;
  mrb_int limit;
  mrb_value mrb_result;
  grn_obj *result;

  table = DATA_PTR(self);
  mrb_get_args(mrb, "oiio", &mrb_keys, &offset, &limit, &mrb_result);

  mrb_keys = mrb_convert_type(mrb, mrb_keys,
                              MRB_TT_ARRAY, "Array", "to_ary");

  n_keys = RARRAY_LEN(mrb_keys);
  keys = GRN_MALLOCN(grn_table_sort_key, n_keys);
  for (i = 0; i < n_keys; i++) {
    memcpy(&(keys[i]),
           DATA_PTR(RARRAY_PTR(mrb_keys)[i]),
           sizeof(grn_table_sort_key));
  }
  result = DATA_PTR(mrb_result);
  grn_table_sort(ctx, table, offset, limit, result, keys, n_keys);
  GRN_FREE(keys);
  grn_mrb_ctx_check(mrb);

  return mrb_result;
}

static mrb_value
mrb_grn_table_group_raw(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  mrb_value mrb_keys;
  grn_table_sort_key *keys;
  int i, n_keys;
  mrb_value mrb_result;
  grn_table_group_result *result;

  table = DATA_PTR(self);
  mrb_get_args(mrb, "oo", &mrb_keys, &mrb_result);

  mrb_keys = mrb_convert_type(mrb, mrb_keys,
                              MRB_TT_ARRAY, "Array", "to_ary");

  n_keys = RARRAY_LEN(mrb_keys);
  keys = GRN_MALLOCN(grn_table_sort_key, n_keys);
  for (i = 0; i < n_keys; i++) {
    memcpy(&(keys[i]),
           DATA_PTR(RARRAY_PTR(mrb_keys)[i]),
           sizeof(grn_table_sort_key));
  }
  result = DATA_PTR(mrb_result);
  grn_table_group(ctx, table, keys, n_keys, result, 1);
  GRN_FREE(keys);
  grn_mrb_ctx_check(mrb);

  return mrb_result;
}

static mrb_value
mrb_grn_table_delete(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *table;
  mrb_value mrb_options;
  mrb_value mrb_id;
  mrb_value mrb_key;
  mrb_value mrb_expression;

  table = DATA_PTR(self);
  mrb_get_args(mrb, "H", &mrb_options);

  mrb_id = grn_mrb_options_get_lit(mrb, mrb_options, "id");
  if (!mrb_nil_p(mrb_id)) {
    grn_table_delete_by_id(ctx, table, mrb_fixnum(mrb_id));
    grn_mrb_ctx_check(mrb);
    return mrb_nil_value();
  }

  mrb_key = grn_mrb_options_get_lit(mrb, mrb_options, "key");
  if (!mrb_nil_p(mrb_key)) {
    grn_id key_domain_id;
    void *key;
    unsigned int key_size;
    grn_mrb_value_to_raw_data_buffer buffer;

    key_domain_id = table->header.domain;
    grn_mrb_value_to_raw_data_buffer_init(mrb, &buffer);
    grn_mrb_value_to_raw_data(mrb, "key", mrb_key, key_domain_id,
                              &buffer, &key, &key_size);
    grn_table_delete(ctx, table, key, key_size);
    grn_mrb_value_to_raw_data_buffer_fin(mrb, &buffer);
    grn_mrb_ctx_check(mrb);
    return mrb_nil_value();
  }

  mrb_expression = grn_mrb_options_get_lit(mrb, mrb_options, "expression");
  if (!mrb_nil_p(mrb_expression)) {
    grn_obj *expression;
    grn_obj *selected_records;
    grn_table_cursor *cursor;

    expression = DATA_PTR(mrb_expression);
    selected_records = grn_table_select(ctx, table, expression, NULL, GRN_OP_OR);
    grn_mrb_ctx_check(mrb);
    cursor = grn_table_cursor_open(ctx, selected_records,
                                   NULL, 0,
                                   NULL, 0,
                                   0, -1, 0);
    if (cursor) {
      while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL) {
        grn_id *id;
        grn_table_cursor_get_key(ctx, cursor, (void **)&id);
        grn_table_delete_by_id(ctx, table, *id);
      }
      grn_table_cursor_close(ctx, cursor);
    }
    grn_mrb_ctx_check(mrb);

    return mrb_nil_value();
  }

  mrb_raisef(mrb, E_ARGUMENT_ERROR,
             "must have :id, :key or :expression: %S",
             mrb_options);

  return mrb_nil_value();
}

void
grn_mrb_table_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *object_class = data->object_class;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Table", object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_method(mrb, klass, "[]",
                    mrb_grn_table_array_reference, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "find_column",
                    mrb_grn_table_find_column, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "locked?",
                    mrb_grn_table_is_locked, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "size",
                    mrb_grn_table_get_size, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "empty?",
                    mrb_grn_table_is_empty, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "select",
                    mrb_grn_table_select, MRB_ARGS_ARG(1, 1));
  mrb_define_method(mrb, klass, "sort_raw",
                    mrb_grn_table_sort_raw, MRB_ARGS_REQ(4));
  mrb_define_method(mrb, klass, "group_raw",
                    mrb_grn_table_group_raw, MRB_ARGS_REQ(2));

  mrb_define_method(mrb, klass, "delete",
                    mrb_grn_table_delete, MRB_ARGS_REQ(1));

}
#endif
