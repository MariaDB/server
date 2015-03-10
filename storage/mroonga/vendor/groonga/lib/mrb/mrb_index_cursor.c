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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/hash.h>

#include "mrb_ctx.h"
#include "mrb_index_cursor.h"

static struct mrb_data_type mrb_grn_index_cursor_type = {
  "Groonga::IndexCursor",
  NULL
};

static mrb_value
mrb_grn_index_cursor_singleton_open_raw(mrb_state *mrb, mrb_value klass)
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

  mrb_get_args(mrb, "oo|H", &mrb_table_cursor, &mrb_index, &mrb_options);

  table_cursor = DATA_PTR(mrb_table_cursor);
  index = DATA_PTR(mrb_index);
  if (!mrb_nil_p(mrb_options)) {
    /* TODO */
  }
  index_cursor = grn_index_cursor_open(ctx, table_cursor, index,
                                       rid_min, rid_max, flags);
  grn_mrb_ctx_check(mrb);

  return mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, index_cursor));
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

void
grn_mrb_index_cursor_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "IndexCursor", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_singleton_method(mrb, (struct RObject *)klass, "open_raw",
                              mrb_grn_index_cursor_singleton_open_raw,
                              MRB_ARGS_ARG(2, 1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_index_cursor_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "close",
                    mrb_grn_index_cursor_close, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "count",
                    mrb_grn_index_cursor_count, MRB_ARGS_NONE());
}
#endif
