/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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
#include <mruby/string.h>

#include "../grn_mrb.h"
#include "mrb_config.h"
#include "mrb_ctx.h"

static mrb_value
config_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *key;
  mrb_int key_size;
  const char *value;
  uint32_t value_size;
  grn_rc rc;

  mrb_get_args(mrb, "s", &key, &key_size);

  rc = grn_config_get(ctx, key, key_size, &value, &value_size);
  if (rc != GRN_SUCCESS) {
    grn_mrb_ctx_check(mrb);
  }

  if (!value) {
    return mrb_nil_value();
  } else {
    return mrb_str_new(mrb, value, value_size);
  }
}

static mrb_value
config_array_set(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *key;
  mrb_int key_size;
  mrb_value mrb_value_;
  grn_rc rc;

  mrb_get_args(mrb, "sS", &key, &key_size, &mrb_value_);

  rc = grn_config_set(ctx,
                      key, key_size,
                      RSTRING_PTR(mrb_value_), RSTRING_LEN(mrb_value_));
  if (rc != GRN_SUCCESS) {
    grn_mrb_ctx_check(mrb);
  }

  return mrb_value_;
}

void
grn_mrb_config_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module;

  module = mrb_define_module_under(mrb, data->module, "Config");

  mrb_define_singleton_method(mrb, (struct RObject *)module,
                              "[]", config_array_reference,
                              MRB_ARGS_REQ(1));
  mrb_define_singleton_method(mrb, (struct RObject *)module,
                              "[]=", config_array_set,
                              MRB_ARGS_REQ(2));
}
#endif
