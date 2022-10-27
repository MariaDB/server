/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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
#include <mruby/array.h>

#include "mrb_ctx.h"
#include "mrb_window_definition.h"

static void
mrb_grn_window_definition_free(mrb_state *mrb, void *data)
{
  grn_window_definition *definition = data;

  if (!definition) {
    return;
  }

  if (definition->sort_keys) {
    mrb_free(mrb, definition->sort_keys);
  }
  if (definition->group_keys) {
    mrb_free(mrb, definition->group_keys);
  }
  mrb_free(mrb, definition);
}

static struct mrb_data_type mrb_grn_window_definition_type = {
  "Groonga::WindowDefinition",
  mrb_grn_window_definition_free
};

static mrb_value
mrb_grn_window_definition_initialize(mrb_state *mrb, mrb_value self)
{
  grn_window_definition *result;

  DATA_TYPE(self) = &mrb_grn_window_definition_type;

  result = mrb_calloc(mrb, 1, sizeof(grn_window_definition));
  DATA_PTR(self) = result;

  return self;
}


static mrb_value
mrb_grn_window_definition_close(mrb_state *mrb, mrb_value self)
{
  grn_window_definition *definition;

  definition = DATA_PTR(self);
  if (definition) {
    mrb_grn_window_definition_free(mrb, definition);
    DATA_PTR(self) = NULL;
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_window_definition_set_sort_keys(mrb_state *mrb, mrb_value self)
{
  grn_window_definition *definition;
  mrb_value mrb_keys;

  definition = DATA_PTR(self);
  mrb_get_args(mrb, "A!", &mrb_keys);

  if (definition->sort_keys) {
    mrb_free(mrb, definition->sort_keys);
  }

  if (mrb_nil_p(mrb_keys)) {
    definition->sort_keys = NULL;
    definition->n_sort_keys = 0;
  } else {
    mrb_int i, n;
    n = RARRAY_LEN(mrb_keys);
    definition->sort_keys = mrb_calloc(mrb, n, sizeof(grn_table_sort_key));
    for (i = 0; i < n; i++) {
      grn_table_sort_key *sort_key = DATA_PTR(RARRAY_PTR(mrb_keys)[i]);
      definition->sort_keys[i] = *sort_key;
    }
    definition->n_sort_keys = n;
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_window_definition_set_group_keys(mrb_state *mrb, mrb_value self)
{
  grn_window_definition *definition;
  mrb_value mrb_keys;

  definition = DATA_PTR(self);
  mrb_get_args(mrb, "A!", &mrb_keys);

  if (definition->group_keys) {
    mrb_free(mrb, definition->group_keys);
  }

  if (mrb_nil_p(mrb_keys)) {
    definition->group_keys = NULL;
    definition->n_group_keys = 0;
  } else {
    mrb_int i, n;
    n = RARRAY_LEN(mrb_keys);
    definition->group_keys = mrb_calloc(mrb, n, sizeof(grn_table_sort_key));
    for (i = 0; i < n; i++) {
      grn_table_sort_key *group_key = DATA_PTR(RARRAY_PTR(mrb_keys)[i]);
      definition->group_keys[i] = *group_key;
    }
    definition->n_group_keys = n;
  }

  return mrb_nil_value();
}

void
grn_mrb_window_definition_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "WindowDefinition",
                                 mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_window_definition_initialize, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "close",
                    mrb_grn_window_definition_close, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "sort_keys=",
                    mrb_grn_window_definition_set_sort_keys, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "group_keys=",
                    mrb_grn_window_definition_set_group_keys, MRB_ARGS_REQ(1));
}
#endif
