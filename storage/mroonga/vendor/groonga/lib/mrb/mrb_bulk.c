/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2016 Brazil

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
#include <string.h>

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/data.h>
#include <mruby/numeric.h>
#include <mruby/string.h>

#include "../grn_db.h"
#include "mrb_bulk.h"
#include "mrb_object.h"

static struct mrb_data_type mrb_grn_bulk_type = {
  "Groonga::Bulk",
  NULL
};

grn_obj *
grn_mrb_value_to_bulk(mrb_state *mrb, mrb_value mrb_value_, grn_obj *bulk)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  switch (mrb_type(mrb_value_)) {
  case MRB_TT_FALSE :
    if (mrb_nil_p(mrb_value_)) {
      grn_obj_reinit(ctx, bulk, GRN_DB_VOID, 0);
    } else {
      grn_obj_reinit(ctx, bulk, GRN_DB_BOOL, 0);
      GRN_BOOL_SET(ctx, bulk, GRN_FALSE);
    }
    break;
  case MRB_TT_TRUE :
    grn_obj_reinit(ctx, bulk, GRN_DB_BOOL, 0);
    GRN_BOOL_SET(ctx, bulk, GRN_TRUE);
    break;
  case MRB_TT_FIXNUM :
    grn_obj_reinit(ctx, bulk, GRN_DB_INT64, 0);
    GRN_INT64_SET(ctx, bulk, mrb_fixnum(mrb_value_));
    break;
  case MRB_TT_SYMBOL :
    {
      const char *name;
      mrb_int name_length;

      grn_obj_reinit(ctx, bulk, GRN_DB_TEXT, 0);
      name = mrb_sym2name_len(mrb, mrb_symbol(mrb_value_), &name_length);
      GRN_TEXT_SET(ctx, bulk, name, name_length);
    }
    break;
  case MRB_TT_FLOAT :
    grn_obj_reinit(ctx, bulk, GRN_DB_FLOAT, 0);
    GRN_FLOAT_SET(ctx, bulk, mrb_float(mrb_value_));
    break;
  case MRB_TT_STRING :
    grn_obj_reinit(ctx, bulk, GRN_DB_TEXT,
                   bulk->header.impl_flags & GRN_OBJ_DO_SHALLOW_COPY);
    GRN_TEXT_SET(ctx, bulk, RSTRING_PTR(mrb_value_), RSTRING_LEN(mrb_value_));
    break;
  default :
    {
      struct RClass *klass;

      klass = mrb_class(mrb, mrb_value_);
      if (klass == ctx->impl->mrb.builtin.time_class) {
        mrb_value mrb_sec;
        mrb_value mrb_usec;

        mrb_sec = mrb_funcall(mrb, mrb_value_, "to_i", 0);
        mrb_usec = mrb_funcall(mrb, mrb_value_, "usec", 0);
        grn_obj_reinit(ctx, bulk, GRN_DB_TIME, 0);
        GRN_TIME_SET(ctx, bulk,
                     GRN_TIME_PACK(mrb_fixnum(mrb_sec), mrb_fixnum(mrb_usec)));
      } else {
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
                   "unsupported object to convert to bulk: %S",
                    mrb_value_);
      }
    }
    break;
  }

  return bulk;
}

mrb_value
grn_mrb_value_from_bulk(mrb_state *mrb, grn_obj *bulk)
{
  mrb_value mrb_value_;
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  if (!bulk) {
    return mrb_nil_value();
  }

  switch (bulk->header.domain) {
  case GRN_DB_BOOL :
    {
      grn_bool value;
      value = GRN_BOOL_VALUE(bulk);
      mrb_value_ = mrb_bool_value(value);
    }
    break;
  case GRN_DB_INT8 :
    {
      int8_t value;
      value = GRN_INT8_VALUE(bulk);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_UINT8 :
    {
      uint8_t value;
      value = GRN_UINT8_VALUE(bulk);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_INT16 :
    {
      int16_t value;
      value = GRN_INT16_VALUE(bulk);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_UINT16 :
    {
      uint16_t value;
      value = GRN_UINT16_VALUE(bulk);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_INT32 :
    {
      int32_t value;
      value = GRN_INT32_VALUE(bulk);
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_UINT32 :
    {
      int64_t value;
      value = GRN_UINT32_VALUE(bulk);
      if (FIXABLE(value)) {
        mrb_value_ = mrb_fixnum_value(value);
      } else {
        mrb_value_ = mrb_float_value(mrb, value);
      }
    }
    break;
  case GRN_DB_INT64 :
    {
      int64_t value;
      value = GRN_INT64_VALUE(bulk);
      if (FIXABLE(value)) {
        mrb_value_ = mrb_fixnum_value(value);
      } else {
        mrb_value_ = mrb_float_value(mrb, value);
      }
    }
    break;
  case GRN_DB_UINT64 :
    {
      uint64_t value;
      value = GRN_UINT64_VALUE(bulk);
      if (FIXABLE(value)) {
        mrb_value_ = mrb_fixnum_value(value);
      } else {
        mrb_value_ = mrb_float_value(mrb, value);
      }
    }
    break;
  case GRN_DB_FLOAT :
    {
      double value;
      value = GRN_FLOAT_VALUE(bulk);
      mrb_value_ = mrb_float_value(mrb, value);
    }
    break;
  case GRN_DB_TIME :
    {
      int64_t value;
      int64_t sec;
      int32_t usec;
      mrb_value mrb_sec;

      value = GRN_TIME_VALUE(bulk);
      GRN_TIME_UNPACK(value, sec, usec);
      if (sec > MRB_INT_MAX) {
        mrb_sec = mrb_float_value(mrb, sec);
      } else {
        mrb_sec = mrb_fixnum_value(sec);
      }
      mrb_value_ = mrb_funcall(mrb,
                               mrb_obj_value(ctx->impl->mrb.builtin.time_class),
                               "at",
                               2,
                               mrb_sec,
                               mrb_fixnum_value(usec));
    }
    break;
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    mrb_value_ = mrb_str_new(mrb,
                             GRN_TEXT_VALUE(bulk),
                             GRN_TEXT_LEN(bulk));
    break;
  default :
    {
      grn_obj *domain;
      grn_bool is_record = GRN_FALSE;

      domain = grn_ctx_at(ctx, bulk->header.domain);
      if (domain) {
        switch (domain->header.type) {
        case GRN_TABLE_HASH_KEY :
        case GRN_TABLE_PAT_KEY :
        case GRN_TABLE_DAT_KEY :
        case GRN_TABLE_NO_KEY :
          is_record = GRN_TRUE;
          break;
        default :
          break;
        }
      }

      if (is_record) {
        mrb_value_ = mrb_fixnum_value(GRN_RECORD_VALUE(bulk));
        grn_obj_unlink(ctx, domain);
      } else {
#define MESSAGE_SIZE 4096
        char message[MESSAGE_SIZE];
        char domain_name[GRN_TABLE_MAX_KEY_SIZE];
        int domain_name_size;

        if (domain) {
          domain_name_size = grn_obj_name(ctx, domain,
                                          domain_name, GRN_TABLE_MAX_KEY_SIZE);
          grn_obj_unlink(ctx, domain);
        } else {
          grn_strcpy(domain_name, GRN_TABLE_MAX_KEY_SIZE, "unknown");
          domain_name_size = strlen(domain_name);
        }
        grn_snprintf(message, MESSAGE_SIZE, MESSAGE_SIZE,
                     "unsupported bulk value type: <%d>(%.*s)",
                     bulk->header.domain,
                     domain_name_size,
                     domain_name);
        mrb_raise(mrb, E_RANGE_ERROR, message);
      }
#undef MESSAGE_SIZE
    }
    break;
  }

  return mrb_value_;
}

grn_bool
grn_mrb_bulk_cast(mrb_state *mrb, grn_obj *from, grn_obj *to, grn_id domain_id)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_rc rc;

  grn_obj_reinit(ctx, to, domain_id, 0);
  rc = grn_obj_cast(ctx, from, to, GRN_FALSE);
  return rc == GRN_SUCCESS;
}

static mrb_value
mrb_grn_bulk_s_is_true(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_value_;
  grn_obj bulk;
  grn_bool is_true;

  mrb_get_args(mrb, "o", &mrb_value_);

  GRN_TEXT_INIT(&bulk, GRN_OBJ_DO_SHALLOW_COPY);
  grn_mrb_value_to_bulk(mrb, mrb_value_, &bulk);
  is_true = grn_obj_is_true(ctx, &bulk);
  GRN_OBJ_FIN(ctx, &bulk);

  return mrb_bool_value(is_true);
}

static mrb_value
mrb_grn_bulk_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_bulk_ptr;

  mrb_get_args(mrb, "o", &mrb_bulk_ptr);
  DATA_TYPE(self) = &mrb_grn_bulk_type;
  DATA_PTR(self) = mrb_cptr(mrb_bulk_ptr);
  return self;
}

static mrb_value
mrb_grn_bulk_get_domain(mrb_state *mrb, mrb_value self)
{
  grn_obj *bulk;

  bulk = DATA_PTR(self);
  return mrb_fixnum_value(bulk->header.domain);
}

static mrb_value
mrb_grn_bulk_get_value(mrb_state *mrb, mrb_value self)
{
  return grn_mrb_value_from_bulk(mrb, DATA_PTR(self));
}

static mrb_value
mrb_grn_bulk_equal(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_other;

  mrb_get_args(mrb, "o", &mrb_other);

  if (!mrb_obj_is_kind_of(mrb, mrb_other, mrb_class(mrb, self))) {
    return mrb_false_value();
  }

  return mrb_bool_value(DATA_PTR(self) == DATA_PTR(mrb_other));
}

void
grn_mrb_bulk_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Bulk", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_singleton_method(mrb, (struct RObject *)klass, "true?",
                              mrb_grn_bulk_s_is_true, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_bulk_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "domain",
                    mrb_grn_bulk_get_domain, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "value",
                    mrb_grn_bulk_get_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "==",
                    mrb_grn_bulk_equal, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "inspect",
                    grn_mrb_object_inspect, MRB_ARGS_NONE());
}
#endif
