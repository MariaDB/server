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

#include "mrb_ctx.h"
#include "mrb_hash_table.h"
#include "mrb_options.h"

static struct mrb_data_type mrb_grn_hash_table_type = {
  "Groonga::HashTable",
  NULL
};

static mrb_value
mrb_grn_hash_table_class_create(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_options = mrb_nil_value();
  const char *name = NULL;
  unsigned int name_size = 0;
  const char *path = NULL;
  grn_obj_flags flags = GRN_OBJ_TABLE_HASH_KEY;
  grn_obj *key_type = NULL;
  grn_obj *value_type = NULL;
  grn_obj *table;

  mrb_get_args(mrb, "|H", &mrb_options);

  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_name;
    mrb_value mrb_flags;
    mrb_value mrb_key_type;
    mrb_value mrb_value_type;

    mrb_name = grn_mrb_options_get_lit(mrb, mrb_options, "name");
    if (!mrb_nil_p(mrb_name)) {
      name = RSTRING_PTR(mrb_name);
      name_size = RSTRING_LEN(mrb_name);
    }

    mrb_flags = grn_mrb_options_get_lit(mrb, mrb_options, "flags");
    if (!mrb_nil_p(mrb_flags)) {
      flags |= mrb_fixnum(mrb_flags);
    }

    mrb_key_type = grn_mrb_options_get_lit(mrb, mrb_options, "key_type");
    if (!mrb_nil_p(mrb_key_type)) {
      key_type = DATA_PTR(mrb_key_type);
    }

    mrb_value_type = grn_mrb_options_get_lit(mrb, mrb_options, "value_type");
    if (!mrb_nil_p(mrb_value_type)) {
      key_type = DATA_PTR(mrb_value_type);
    }
  }

  table = grn_table_create(ctx, name, name_size, path, flags,
                           key_type, value_type);
  grn_mrb_ctx_check(mrb);

  return mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, table));
}

static mrb_value
mrb_grn_hash_table_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_hash_table_ptr;

  mrb_get_args(mrb, "o", &mrb_hash_table_ptr);
  DATA_TYPE(self) = &mrb_grn_hash_table_type;
  DATA_PTR(self) = mrb_cptr(mrb_hash_table_ptr);
  return self;
}

void
grn_mrb_hash_table_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *table_class;
  struct RClass *klass;

  table_class = mrb_class_get_under(mrb, module, "Table");
  klass = mrb_define_class_under(mrb, module, "HashTable", table_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "create",
                          mrb_grn_hash_table_class_create,
                          MRB_ARGS_OPT(1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_hash_table_initialize, MRB_ARGS_REQ(1));
}
#endif
