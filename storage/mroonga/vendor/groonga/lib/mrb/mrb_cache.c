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

#include "../grn_db.h"
#include "../grn_cache.h"
#include "mrb_bulk.h"
#include "mrb_cache.h"

static struct mrb_data_type mrb_grn_cache_type = {
  "Groonga::Cache",
  NULL
};

static mrb_value
mrb_grn_cache_class_current(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_cache *cache;
  mrb_value mrb_cache;

  cache = grn_cache_current_get(ctx);
  mrb_cache = mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, cache));

  return mrb_cache;
}

static mrb_value
mrb_grn_cache_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_cache_ptr;

  mrb_get_args(mrb, "o", &mrb_cache_ptr);
  DATA_TYPE(self) = &mrb_grn_cache_type;
  DATA_PTR(self) = mrb_cptr(mrb_cache_ptr);
  return self;
}

static mrb_value
mrb_grn_cache_fetch(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_cache *cache;
  char *key;
  mrb_int key_size;
  grn_rc rc;
  grn_obj cache_value;
  mrb_value mrb_cache_value;

  cache = DATA_PTR(self);
  mrb_get_args(mrb, "s", &key, &key_size);

  GRN_TEXT_INIT(&cache_value, 0);
  rc = grn_cache_fetch(ctx, cache, key, key_size, &cache_value);
  if (rc == GRN_SUCCESS) {
    mrb_cache_value = grn_mrb_value_from_bulk(mrb, &cache_value);
  } else {
    mrb_cache_value = mrb_nil_value();
  }
  GRN_OBJ_FIN(ctx, &cache_value);

  return mrb_cache_value;
}

static mrb_value
mrb_grn_cache_update(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_cache *cache;
  char *key;
  mrb_int key_size;
  char *value;
  mrb_int value_size;
  grn_obj value_buffer;

  cache = DATA_PTR(self);
  mrb_get_args(mrb, "ss", &key, &key_size, &value, &value_size);

  GRN_TEXT_INIT(&value_buffer, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &value_buffer, value, value_size);
  grn_cache_update(ctx, cache, key, key_size, &value_buffer);
  GRN_OBJ_FIN(ctx, &value_buffer);

  return mrb_nil_value();
}

void
grn_mrb_cache_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Cache", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "current",
                          mrb_grn_cache_class_current, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_cache_initialize, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "fetch",
                    mrb_grn_cache_fetch, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "update",
                    mrb_grn_cache_update, MRB_ARGS_REQ(2));
}
#endif
