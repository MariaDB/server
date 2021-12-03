/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015 Brazil

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
#include "../grn_db.h"
#include <string.h>

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/string.h>

#include "mrb_converter.h"
#include "mrb_bulk.h"

void
grn_mrb_value_to_raw_data_buffer_init(mrb_state *mrb,
                                      grn_mrb_value_to_raw_data_buffer *buffer)
{
  GRN_VOID_INIT(&(buffer->from));
  GRN_VOID_INIT(&(buffer->to));
}

void
grn_mrb_value_to_raw_data_buffer_fin(mrb_state *mrb,
                                     grn_mrb_value_to_raw_data_buffer *buffer)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  GRN_OBJ_FIN(ctx, &(buffer->from));
  GRN_OBJ_FIN(ctx, &(buffer->to));
}

void
grn_mrb_value_to_raw_data(mrb_state *mrb,
                          const char *context,
                          mrb_value mrb_value_,
                          grn_id domain_id,
                          grn_mrb_value_to_raw_data_buffer *buffer,
                          void **raw_value,
                          unsigned int *raw_value_size)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  enum mrb_vtype mrb_value_type;
  grn_bool try_cast = GRN_FALSE;
  grn_obj *from_bulk = NULL;

  if (mrb_nil_p(mrb_value_)) {
    *raw_value = NULL;
    *raw_value_size = 0;
    return;
  }

  mrb_value_type = mrb_type(mrb_value_);

  switch (mrb_value_type) {
  case MRB_TT_STRING :
    switch (domain_id) {
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      *raw_value = RSTRING_PTR(mrb_value_);
      *raw_value_size = RSTRING_LEN(mrb_value_);
      break;
    default :
      try_cast = GRN_TRUE;
      break;
    }
    break;
  default :
    {
      struct RClass *klass;
      grn_mrb_data *data = &(ctx->impl->mrb);

      klass = mrb_class(mrb, mrb_value_);
      if (domain_id == GRN_DB_TIME &&
          klass == data->builtin.time_class) {
        mrb_value mrb_sec;
        mrb_value mrb_usec;

        mrb_sec = mrb_funcall(mrb, mrb_value_, "to_i", 0);
        mrb_usec = mrb_funcall(mrb, mrb_value_, "usec", 0);
        buffer->value.time_value = GRN_TIME_PACK(mrb_fixnum(mrb_sec),
                                                 mrb_fixnum(mrb_usec));
        *raw_value = &(buffer->value.time_value);
        *raw_value_size = sizeof(buffer->value.time_value);
      } else {
        try_cast = GRN_TRUE;
        if (mrb_value_type == MRB_TT_DATA &&
            klass == mrb_class_get_under(mrb, data->module, "Bulk")) {
          from_bulk = DATA_PTR(mrb_value_);
        }
      }
    }
    break;
  }

  if (!try_cast) {
    return;
  }

  if (!from_bulk) {
    from_bulk = &(buffer->from);
    grn_mrb_value_to_bulk(mrb, mrb_value_, from_bulk);
  }
  if (!grn_mrb_bulk_cast(mrb, from_bulk, &(buffer->to), domain_id)) {
    grn_obj *domain;
    char domain_name[GRN_TABLE_MAX_KEY_SIZE];
    int domain_name_size;

    domain = grn_ctx_at(ctx, domain_id);
    domain_name_size = grn_obj_name(ctx, domain, domain_name,
                                    GRN_TABLE_MAX_KEY_SIZE);
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "%S: failed to convert to %S: %S",
               mrb_str_new_static(mrb, context, strlen(context)),
               mrb_str_new_static(mrb, domain_name, domain_name_size),
               mrb_funcall(mrb, mrb_value_, "inspect", 0));
  }
  *raw_value = GRN_BULK_HEAD(&(buffer->to));
  *raw_value_size = GRN_BULK_VSIZE(&(buffer->to));
}

mrb_value
grn_mrb_value_from_raw_data(mrb_state *mrb,
                            grn_id domain,
                            void *raw_value,
                            unsigned int raw_value_size)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_value_;

  switch (domain) {
  case GRN_DB_INT32 :
    if (raw_value_size == 0) {
      mrb_value_ = mrb_fixnum_value(0);
    } else {
      int32_t value;
      value = *((int32_t *)raw_value);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    mrb_value_ = mrb_str_new(mrb,
                             raw_value,
                             raw_value_size);
    break;
  default :
    {
      grn_obj *domain_object;
#define MESSAGE_SIZE 4096
      char message[MESSAGE_SIZE];
      char domain_name[GRN_TABLE_MAX_KEY_SIZE];
      int domain_name_size;

      domain_object = grn_ctx_at(ctx, domain);
      if (domain_object) {
        domain_name_size = grn_obj_name(ctx, domain_object,
                                        domain_name, GRN_TABLE_MAX_KEY_SIZE);
        grn_obj_unlink(ctx, domain_object);
      } else {
        grn_strcpy(domain_name, GRN_TABLE_MAX_KEY_SIZE, "unknown");
        domain_name_size = strlen(domain_name);
      }
      grn_snprintf(message, MESSAGE_SIZE, MESSAGE_SIZE,
                   "unsupported raw value type: <%d>(%.*s)",
                   domain,
                   domain_name_size,
                   domain_name);
      mrb_raise(mrb, E_RANGE_ERROR, message);
    }
#undef MESSAGE_SIZE
    break;
  }

  return mrb_value_;
}

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
  case GRN_PTR :
    klass = mrb_class_get_under(mrb, data->module, "Pointer");
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
    grn_snprintf(buffer, BUFFER_SIZE, BUFFER_SIZE,
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
mrb_grn_converter_class_convert(mrb_state *mrb, mrb_value klass)
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
  if (!grn_mrb_bulk_cast(mrb, from, to, to_type)) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "failed to convert to %S: %S",
               mrb_to_class,
               mrb_from);
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

  mrb_define_class_method(mrb, module, "convert",
                          mrb_grn_converter_class_convert,
                          MRB_ARGS_REQ(2));
}
#endif
