/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2014 Brazil

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
#include "../grn_db.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "mrb_converter.h"
#include "mrb_bulk.h"

struct RClass *
grn_mrb_class_from_grn_obj(mrb_state *mrb, grn_obj *object)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_mrb_data *data;
  struct RClass *klass = NULL;

  data = &(ctx->impl->mrb);
  switch (object->header.type) {
  case GRN_BULK :
    klass = mrb_class_get_under(mrb, data->module, "Bulk");
    break;
  case GRN_ACCESSOR :
    klass = mrb_class_get_under(mrb, data->module, "Accessor");
    break;
  case GRN_COLUMN_FIX_SIZE :
    klass = mrb_class_get_under(mrb, data->module, "FixedSizeColumn");
    break;
  case GRN_COLUMN_VAR_SIZE :
    klass = mrb_class_get_under(mrb, data->module, "VariableSizeColumn");
    break;
  case GRN_COLUMN_INDEX :
    klass = mrb_class_get_under(mrb, data->module, "IndexColumn");
    break;
  case GRN_TYPE :
    klass = mrb_class_get_under(mrb, data->module, "Type");
    break;
  case GRN_PROC :
    klass = mrb_class_get_under(mrb, data->module, "Procedure");
    break;
  case GRN_EXPR :
    klass = mrb_class_get_under(mrb, data->module, "Expression");
    break;
  case GRN_TABLE_NO_KEY :
    klass = mrb_class_get_under(mrb, data->module, "Array");
    break;
  case GRN_TABLE_HASH_KEY :
    klass = mrb_class_get_under(mrb, data->module, "HashTable");
    break;
  case GRN_TABLE_PAT_KEY :
    klass = mrb_class_get_under(mrb, data->module, "PatriciaTrie");
    break;
  case GRN_TABLE_DAT_KEY :
    klass = mrb_class_get_under(mrb, data->module, "DoubleArrayTrie");
    break;
  case GRN_DB :
    klass = mrb_class_get_under(mrb, data->module, "Database");
    break;
  case GRN_VOID :
    klass = mrb_class_get_under(mrb, data->module, "Void");
    break;
  default :
    break;
  }

  if (!klass) {
#define BUFFER_SIZE 1024
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE - 1,
             "can't find class for object type: %#x", object->header.type);
    mrb_raise(mrb, E_ARGUMENT_ERROR, buffer);
#undef BUFFER_SIZE
  }

  return klass;
}

mrb_value
grn_mrb_value_from_grn_obj(mrb_state *mrb, grn_obj *object)
{
  struct RClass *mrb_class;
  mrb_value mrb_new_arguments[1];
  mrb_value mrb_object;

  if (!object) {
    return mrb_nil_value();
  }

  mrb_class = grn_mrb_class_from_grn_obj(mrb, object);
  mrb_new_arguments[0] = mrb_cptr_value(mrb, object);
  mrb_object = mrb_obj_new(mrb, mrb_class, 1, mrb_new_arguments);
  return mrb_object;
}

grn_id
grn_mrb_class_to_type(mrb_state *mrb, struct RClass *klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_id type = GRN_DB_VOID;

  if (klass == mrb->nil_class) {
    type = GRN_DB_VOID;
  } else if (klass == mrb->true_class ||
             klass == mrb->false_class) {
    type = GRN_DB_BOOL;
  } else if (klass == mrb->symbol_class) {
    type = GRN_DB_TEXT;
  } else if (klass == mrb->fixnum_class) {
    type = GRN_DB_INT64;
  } else if (klass == mrb->float_class) {
    type = GRN_DB_FLOAT;
  } else if (klass == mrb->string_class) {
    type = GRN_DB_TEXT;
  } else if (klass == ctx->impl->mrb.builtin.time_class) {
    type = GRN_DB_TIME;
  } else {
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "unsupported class: %S", mrb_obj_value(klass));
  }

  return type;
}

static mrb_value
mrb_grn_converter_singleton_convert(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *from = &(ctx->impl->mrb.buffer.from);
  grn_obj *to = &(ctx->impl->mrb.buffer.to);
  mrb_value mrb_from;
  mrb_value mrb_to_class;
  grn_id to_type;

  mrb_get_args(mrb, "oC", &mrb_from, &mrb_to_class);

  grn_mrb_value_to_bulk(mrb, mrb_from, from);
  to_type = grn_mrb_class_to_type(mrb, mrb_class_ptr(mrb_to_class));
  grn_obj_reinit(ctx, to, to_type, 0);
  {
    grn_rc rc;
    rc = grn_obj_cast(ctx, from, to, GRN_FALSE);
    if (rc != GRN_SUCCESS) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR,
                 "failed to convert to %S: %S",
                 mrb_to_class,
                 from);
    }
  }

  return grn_mrb_value_from_bulk(mrb, to);
}

void
grn_mrb_converter_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module;

  module = mrb_define_module_under(mrb, data->module, "Converter");

  mrb_define_singleton_method(mrb, (struct RObject *)module, "convert",
                              mrb_grn_converter_singleton_convert,
                              MRB_ARGS_REQ(2));
}
#endif
