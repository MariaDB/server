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
#include "../grn_ii.h"
#include <string.h>

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "mrb_ctx.h"
#include "mrb_converter.h"
#include "mrb_index_column.h"
#include "mrb_operator.h"
#include "mrb_options.h"

static struct mrb_data_type mrb_grn_index_column_type = {
  "Groonga::IndexColumn",
  NULL
};

static mrb_value
mrb_grn_index_column_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_index_column_ptr;

  mrb_get_args(mrb, "o", &mrb_index_column_ptr);
  DATA_TYPE(self) = &mrb_grn_index_column_type;
  DATA_PTR(self) = mrb_cptr(mrb_index_column_ptr);
  return self;
}

static mrb_value
mrb_grn_index_column_get_lexicon(mrb_state *mrb, mrb_value self)
{
  grn_obj *index_column;
  grn_obj *lexicon;

  index_column = DATA_PTR(self);
  lexicon = ((grn_ii *)index_column)->lexicon;

  return grn_mrb_value_from_grn_obj(mrb, lexicon);
}

static mrb_value
mrb_grn_index_column_get_source_ids(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index_column;
  grn_obj source_ids;
  unsigned int i, n_ids;
  mrb_value mrb_source_ids;

  index_column = DATA_PTR(self);
  GRN_RECORD_INIT(&source_ids, GRN_OBJ_VECTOR, GRN_DB_VOID);
  grn_obj_get_info(ctx, index_column, GRN_INFO_SOURCE, &source_ids);
  n_ids = GRN_BULK_VSIZE(&source_ids) / sizeof(grn_id);

  mrb_source_ids = mrb_ary_new_capa(mrb, n_ids);
  for (i = 0; i < n_ids; i++) {
    grn_id source_id = GRN_RECORD_VALUE_AT(&source_ids, i);
    mrb_ary_push(mrb, mrb_source_ids, mrb_fixnum_value(source_id));
  }

  GRN_OBJ_FIN(ctx, &source_ids);

  return mrb_source_ids;
}

static mrb_value
mrb_grn_index_column_estimate_size_for_term_id(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index_column;
  mrb_int term_id;
  unsigned int size;

  index_column = DATA_PTR(self);
  mrb_get_args(mrb, "i", &term_id);

  size = grn_ii_estimate_size(ctx, (grn_ii *)index_column, term_id);
  return mrb_fixnum_value(size);
}

static mrb_value
mrb_grn_index_column_estimate_size_for_query(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index_column;
  grn_obj *lexicon;
  mrb_value mrb_query;
  void *query;
  unsigned int query_size;
  grn_mrb_value_to_raw_data_buffer buffer;
  mrb_value mrb_options = mrb_nil_value();
  grn_search_optarg optarg;
  unsigned int size;

  index_column = DATA_PTR(self);
  mrb_get_args(mrb, "o|H", &mrb_query, &mrb_options);

  lexicon = grn_ctx_at(ctx, index_column->header.domain);
  grn_mrb_value_to_raw_data_buffer_init(mrb, &buffer);
  grn_mrb_value_to_raw_data(mrb, "query", mrb_query, lexicon->header.domain,
                            &buffer, &query, &query_size);

  memset(&optarg, 0, sizeof(grn_search_optarg));
  optarg.mode = GRN_OP_EXACT;

  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_mode;

    mrb_mode = grn_mrb_options_get_lit(mrb, mrb_options, "mode");
    if (!mrb_nil_p(mrb_mode)) {
      optarg.mode = grn_mrb_value_to_operator(mrb, mrb_mode);
    }
  }

  size = grn_ii_estimate_size_for_query(ctx, (grn_ii *)index_column,
                                        query, query_size, &optarg);
  grn_mrb_value_to_raw_data_buffer_fin(mrb, &buffer);

  grn_mrb_ctx_check(mrb);

  return mrb_fixnum_value(size);
}

static mrb_value
mrb_grn_index_column_estimate_size_for_lexicon_cursor(mrb_state *mrb,
                                                      mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *index_column;
  mrb_value mrb_lexicon_cursor;
  grn_table_cursor *lexicon_cursor;
  unsigned int size;

  index_column = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_lexicon_cursor);

  lexicon_cursor = DATA_PTR(mrb_lexicon_cursor);
  size = grn_ii_estimate_size_for_lexicon_cursor(ctx,
                                                 (grn_ii *)index_column,
                                                 lexicon_cursor);
  return mrb_fixnum_value(size);
}

void
grn_mrb_index_column_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *column_class;
  struct RClass *klass;

  column_class = mrb_class_get_under(mrb, module, "Column");
  klass = mrb_define_class_under(mrb, module, "IndexColumn", column_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_index_column_initialize, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "lexicon",
                    mrb_grn_index_column_get_lexicon,
                    MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "source_ids",
                    mrb_grn_index_column_get_source_ids,
                    MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "estimate_size_for_term_id",
                    mrb_grn_index_column_estimate_size_for_term_id,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "estimate_size_for_query",
                    mrb_grn_index_column_estimate_size_for_query,
                    MRB_ARGS_ARG(1, 1));
  mrb_define_method(mrb, klass, "estimate_size_for_lexicon_cursor",
                    mrb_grn_index_column_estimate_size_for_lexicon_cursor,
                    MRB_ARGS_REQ(1));
}
#endif
