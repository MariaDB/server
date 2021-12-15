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
#include "../grn_ii.h"
#include "../grn_db.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/hash.h>
#include <mruby/variable.h>

#include "mrb_ctx.h"
#include "mrb_index_cursor.h"
#include "mrb_converter.h"
#include "mrb_options.h"

static struct mrb_data_type mrb_grn_index_cursor_type = {
  "Groonga::IndexCursor",
  NULL
};

static mrb_value
mrb_grn_index_cursor_class_open_raw(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_table_cursor;
  mrb_value mrb_index;
  mrb_value mrb_options = mrb_nil_value();
  grn_obj *index_cursor;
  grn_table_cursor *table_cursor;
  grn_obj *index;
  grn_id rid_min = GRN_ID_NIL;
  grn_id rid_max = GRN_ID_MAX;
  int flags = 0;
  mrb_value mrb_index_cursor;

  mrb_get_args(mrb, "oo|H", &mrb_table_cursor, &mrb_index, &mrb_options);

  table_cursor = DATA_PTR(mrb_table_cursor);
  index = DATA_PTR(mrb_index);
  if (!mrb_nil_p(mrb_options)) {
    /* TODO */
  }
  index_cursor = grn_index_cursor_open(ctx, table_cursor, index,
                                       rid_min, rid_max, flags);
  grn_mrb_ctx_check(mrb);

  mrb_index_cursor = mrb_funcall(mrb, klass, "new", 1,
                                 mrb_cptr_value(mrb, index_cursor));
  mrb_iv_set(mrb, mrb_index_cursor, mrb_intern_lit(mrb, "@index"), mrb_index);
  return mrb_index_cursor;
}

static mrb_value
mrb_grn_index_cursor_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_index_cursor_ptr;

  mrb_get_args(mrb, "o", &mrb_index_cursor_ptr);
  DATA_TYPE(self) = &mrb_grn_index_cursor_type;
  DATA_PTR(self) = mrb_cptr(mrb_index_cursor_ptr);

  return self;
}

static mrb_value
mrb_grn_index_cursor_close(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index_cursor;

  index_cursor = DATA_PTR(self);
  if (index_cursor) {
    DATA_PTR(self) = NULL;
    grn_obj_close(ctx, index_cursor);
    grn_mrb_ctx_check(mrb);
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_index_cursor_count(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_id term_id;
  int n_records = 0;

  while (grn_index_cursor_next(ctx, DATA_PTR(self), &term_id)) {
    n_records++;
  }

  return mrb_fixnum_value(n_records);
}

static mrb_value
mrb_grn_index_cursor_select(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_result_set;
  mrb_value mrb_options;
  grn_obj *index_cursor;
  grn_obj *expr = NULL;
  grn_obj *expr_variable = NULL;
  int offset = 0;
  int limit = 10;
  int max_n_unmatched_records = -1;
  int n_matched_records = 0;
  int n_unmatched_records = 0;
  mrb_value mrb_index;
  grn_obj *index;
  grn_obj *lexicon;
  grn_obj *data_table;
  grn_hash *result_set;
  grn_posting *posting;
  grn_id term_id;
  grn_operator op = GRN_OP_OR;

  mrb_get_args(mrb, "o|H", &mrb_result_set, &mrb_options);

  index_cursor = DATA_PTR(self);
  result_set = DATA_PTR(mrb_result_set);

  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_expr;
    mrb_value mrb_offset;
    mrb_value mrb_limit;
    mrb_value mrb_max_n_unmatched_records;

    mrb_expr = grn_mrb_options_get_lit(mrb, mrb_options, "expression");
    if (!mrb_nil_p(mrb_expr)) {
      expr = DATA_PTR(mrb_expr);
      expr_variable = grn_expr_get_var_by_offset(ctx, expr, 0);
    }

    mrb_offset = grn_mrb_options_get_lit(mrb, mrb_options, "offset");
    if (!mrb_nil_p(mrb_offset)) {
      offset = mrb_fixnum(mrb_offset);
    }

    mrb_limit = grn_mrb_options_get_lit(mrb, mrb_options, "limit");
    if (!mrb_nil_p(mrb_limit)) {
      limit = mrb_fixnum(mrb_limit);
    }

    mrb_max_n_unmatched_records =
      grn_mrb_options_get_lit(mrb, mrb_options, "max_n_unmatched_records");
    if (!mrb_nil_p(mrb_max_n_unmatched_records)) {
      max_n_unmatched_records = mrb_fixnum(mrb_max_n_unmatched_records);
    }
  }

  if (limit <= 0) {
    return mrb_fixnum_value(n_matched_records);
  }

  mrb_index = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@index"));
  index = DATA_PTR(mrb_index);
  lexicon = ((grn_ii *)index)->lexicon;
  data_table = grn_ctx_at(ctx, grn_obj_get_range(ctx, index));

  if (max_n_unmatched_records < 0) {
    max_n_unmatched_records = INT32_MAX;
  }
  while ((posting = grn_index_cursor_next(ctx, index_cursor, &term_id))) {
    if (expr) {
      grn_bool matched_raw = GRN_FALSE;
      grn_obj *matched;

      GRN_RECORD_SET(ctx, expr_variable, posting->rid);
      matched = grn_expr_exec(ctx, expr, 0);
      if (matched) {
        matched_raw = grn_obj_is_true(ctx, matched);
      } else {
        grn_mrb_ctx_check(mrb);
      }

      if (!matched_raw) {
        n_unmatched_records++;
        if (n_unmatched_records > max_n_unmatched_records) {
          return mrb_fixnum_value(-1);
        }
        continue;
      }
    }
    n_matched_records++;
    if (offset > 0) {
      offset--;
      continue;
    }
    grn_ii_posting_add(ctx, posting, result_set, op);
    limit--;
    if (limit == 0) {
      break;
    }
  }
  grn_ii_resolve_sel_and(ctx, result_set, op);

  return mrb_fixnum_value(n_matched_records);
}

void
grn_mrb_index_cursor_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "IndexCursor", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "open_raw",
                          mrb_grn_index_cursor_class_open_raw,
                          MRB_ARGS_ARG(2, 1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_index_cursor_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "close",
                    mrb_grn_index_cursor_close, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "count",
                    mrb_grn_index_cursor_count, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "select",
                    mrb_grn_index_cursor_select, MRB_ARGS_ARG(1, 1));
}
#endif
