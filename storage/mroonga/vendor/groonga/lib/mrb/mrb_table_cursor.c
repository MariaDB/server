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
#include <mruby/string.h>
#include <mruby/hash.h>
#include <mruby/variable.h>

#include "mrb_ctx.h"
#include "mrb_bulk.h"
#include "mrb_table_cursor.h"

#include "mrb_converter.h"
#include "mrb_options.h"

static struct mrb_data_type mrb_grn_table_cursor_type = {
  "Groonga::TableCursor",
  NULL
};

static mrb_value
mrb_grn_table_cursor_class_open_raw(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_table;
  mrb_value mrb_options = mrb_nil_value();
  grn_table_cursor *table_cursor;
  grn_obj *table;
  void *min = NULL;
  unsigned int min_size = 0;
  grn_mrb_value_to_raw_data_buffer min_buffer;
  void *max = NULL;
  unsigned int max_size = 0;
  grn_mrb_value_to_raw_data_buffer max_buffer;
  int offset = 0;
  int limit = -1;
  int flags = 0;

  mrb_get_args(mrb, "o|H", &mrb_table, &mrb_options);

  table = DATA_PTR(mrb_table);
  grn_mrb_value_to_raw_data_buffer_init(mrb, &min_buffer);
  grn_mrb_value_to_raw_data_buffer_init(mrb, &max_buffer);
  if (!mrb_nil_p(mrb_options)) {
    grn_id key_domain_id;
    mrb_value mrb_min;
    mrb_value mrb_max;
    mrb_value mrb_flags;

    if (table->header.type == GRN_DB) {
      key_domain_id = GRN_DB_SHORT_TEXT;
    } else {
      key_domain_id = table->header.domain;
    }

    mrb_min = grn_mrb_options_get_lit(mrb, mrb_options, "min");
    grn_mrb_value_to_raw_data(mrb, "min", mrb_min,
                              key_domain_id, &min_buffer, &min, &min_size);

    mrb_max = grn_mrb_options_get_lit(mrb, mrb_options, "max");
    grn_mrb_value_to_raw_data(mrb, "max", mrb_max,
                              key_domain_id, &max_buffer, &max, &max_size);

    mrb_flags = grn_mrb_options_get_lit(mrb, mrb_options, "flags");
    if (!mrb_nil_p(mrb_flags)) {
      flags = mrb_fixnum(mrb_flags);
    }
  }
  table_cursor = grn_table_cursor_open(ctx, table,
                                       min, min_size,
                                       max, max_size,
                                       offset, limit, flags);
  grn_mrb_value_to_raw_data_buffer_fin(mrb, &min_buffer);
  grn_mrb_value_to_raw_data_buffer_fin(mrb, &max_buffer);
  grn_mrb_ctx_check(mrb);

  {
    mrb_value mrb_table_cursor;
    mrb_table_cursor = mrb_funcall(mrb, klass,
                                   "new", 1, mrb_cptr_value(mrb, table_cursor));
    mrb_iv_set(mrb, mrb_table_cursor, mrb_intern_lit(mrb, "@table"), mrb_table);
    return mrb_table_cursor;
  }
}

static mrb_value
mrb_grn_table_cursor_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_table_cursor_ptr;

  mrb_get_args(mrb, "o", &mrb_table_cursor_ptr);
  DATA_TYPE(self) = &mrb_grn_table_cursor_type;
  DATA_PTR(self) = mrb_cptr(mrb_table_cursor_ptr);

  return self;
}

static mrb_value
mrb_grn_table_cursor_close(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_table_cursor *table_cursor;

  table_cursor = DATA_PTR(self);
  if (table_cursor) {
    DATA_PTR(self) = NULL;
    grn_table_cursor_close(ctx, table_cursor);
    grn_mrb_ctx_check(mrb);
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_cursor_next(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_id id;

  id = grn_table_cursor_next(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_fixnum_value(id);
}

static mrb_value
mrb_grn_table_cursor_count(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  int n_records = 0;

  while (grn_table_cursor_next(ctx, DATA_PTR(self)) != GRN_ID_NIL) {
    n_records++;
  }

  return mrb_fixnum_value(n_records);
}

static mrb_value
mrb_grn_table_cursor_get_key(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_table;
  grn_obj *table;
  grn_id domain;
  void *key;
  int key_size;

  mrb_table = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "@table"));
  table = DATA_PTR(mrb_table);
  if (table->header.type == GRN_DB) {
    domain = GRN_DB_SHORT_TEXT;
  } else {
    domain = table->header.domain;
  }
  key_size = grn_table_cursor_get_key(ctx, DATA_PTR(self), &key);

  return grn_mrb_value_from_raw_data(mrb, domain, key, key_size);
}

void
grn_mrb_table_cursor_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "TableCursor", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "open_raw",
                          mrb_grn_table_cursor_class_open_raw,
                          MRB_ARGS_ARG(1, 1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_table_cursor_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "close",
                    mrb_grn_table_cursor_close, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "next",
                    mrb_grn_table_cursor_next, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "count",
                    mrb_grn_table_cursor_count, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "key",
                    mrb_grn_table_cursor_get_key, MRB_ARGS_NONE());
}
#endif
