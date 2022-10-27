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
#include "mrb_table_sort_key.h"

static void
mrb_grn_table_sort_key_free(mrb_state *mrb, void *data)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_table_sort_key *sort_key = data;

  if (!sort_key) {
    return;
  }

  if (sort_key->key) {
    if (sort_key->key->header.type == GRN_ACCESSOR) {
      grn_obj_unlink(ctx, sort_key->key);
    }
  }
  mrb_free(mrb, sort_key);
}

static struct mrb_data_type mrb_grn_table_sort_key_type = {
  "Groonga::TableSortKey",
  mrb_grn_table_sort_key_free
};

static mrb_value
mrb_grn_table_sort_key_initialize(mrb_state *mrb, mrb_value self)
{
  grn_table_sort_key *result;

  DATA_TYPE(self) = &mrb_grn_table_sort_key_type;

  result = mrb_calloc(mrb, 1, sizeof(grn_table_sort_key));
  DATA_PTR(self) = result;

  return self;
}


static mrb_value
mrb_grn_table_sort_key_close(mrb_state *mrb, mrb_value self)
{
  grn_table_sort_key *sort_key;

  sort_key = DATA_PTR(self);
  if (sort_key) {
    mrb_grn_table_sort_key_free(mrb, sort_key);
    DATA_PTR(self) = NULL;
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_sort_key_set_key(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_table_sort_key *sort_key;
  mrb_value mrb_key;

  sort_key = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_key);

  if (sort_key->key) {
    grn_obj_unlink(ctx, sort_key->key);
  }

  if (mrb_nil_p(mrb_key)) {
    sort_key->key = NULL;
  } else {
    sort_key->key = DATA_PTR(mrb_key);
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_sort_key_set_flags(mrb_state *mrb, mrb_value self)
{
  grn_table_sort_key *sort_key;
  mrb_int flags;

  sort_key = DATA_PTR(self);
  mrb_get_args(mrb, "i", &flags);

  sort_key->flags = flags;

  return mrb_nil_value();
}

static mrb_value
mrb_grn_table_sort_key_set_offset(mrb_state *mrb, mrb_value self)
{
  grn_table_sort_key *sort_key;
  mrb_int offset;

  sort_key = DATA_PTR(self);
  mrb_get_args(mrb, "i", &offset);

  sort_key->offset = offset;

  return mrb_nil_value();
}

void
grn_mrb_table_sort_key_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "TableSortKey",
                                 mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_table_sort_key_initialize, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "close",
                    mrb_grn_table_sort_key_close, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "key=",
                    mrb_grn_table_sort_key_set_key, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "flags=",
                    mrb_grn_table_sort_key_set_flags, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "offset=",
                    mrb_grn_table_sort_key_set_offset, MRB_ARGS_REQ(1));
}
#endif
