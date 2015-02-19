/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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
#include <mruby/variable.h>
#include <mruby/data.h>
#include <mruby/numeric.h>
#include <mruby/string.h>

#include "../grn_db.h"
#include "mrb_bulk.h"

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
    grn_obj_reinit(ctx, bulk, GRN_DB_TEXT, 0);
    GRN_TEXT_SET(ctx, bulk, RSTRING_PTR(mrb_value_), RSTRING_LEN(mrb_value_));
    break;
  default :
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "unsupported object to convert to bulk: %S",
               mrb_value_);
    break;
  }

  return bulk;
}

mrb_value
grn_mrb_value_from_bulk(mrb_state *mrb, grn_obj *bulk)
{
  mrb_value mrb_value_;
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  switch (bulk->header.domain) {
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
      if (!FIXABLE(value)) {
        mrb_raisef(mrb, E_RANGE_ERROR,
                   "can't handle large number: <%S>: max: <%S>",
                   mrb_fixnum_value(value), /* TODO: This will cause overflow */
                   mrb_fixnum_value(MRB_INT_MAX));
      }
      mrb_value_ = mrb_fixnum_value(value);
    }
    break;
  case GRN_DB_TIME :
    {
      int64_t value;
      int32_t sec;
      int32_t usec;

      value = GRN_TIME_VALUE(bulk);
      GRN_TIME_UNPACK(value, sec, usec);
      mrb_value_ = mrb_funcall(mrb,
                               mrb_obj_value(ctx->impl->mrb.builtin.time_class),
                               "at",
                               2,
                               mrb_fixnum_value(sec),
                               mrb_fixnum_value(usec));
    }
    break;
  default :
    {
#define MESSAGE_SIZE 4096
      char message[MESSAGE_SIZE];
      grn_obj *domain;
      char domain_name[GRN_TABLE_MAX_KEY_SIZE];
      int domain_name_size;

      domain = grn_ctx_at(ctx, bulk->header.domain);
      if (domain) {
        domain_name_size = grn_obj_name(ctx, domain,
                                        domain_name, GRN_TABLE_MAX_KEY_SIZE);
        grn_obj_unlink(ctx, domain);
      } else {
        strcpy(domain_name, "unknown");
        domain_name_size = strlen(domain_name);
      }
      snprintf(message, MESSAGE_SIZE,
               "unsupported bulk value type: <%d>(%.*s)",
               bulk->header.domain,
               domain_name_size,
               domain_name);
      mrb_raise(mrb, E_RANGE_ERROR, message);
#undef MESSAGE_SIZE
    }
    break;
  }

  return mrb_value_;
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
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_bulk_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "domain",
                    mrb_grn_bulk_get_domain, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "value",
                    mrb_grn_bulk_get_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "==",
                    mrb_grn_bulk_equal, MRB_ARGS_REQ(1));
}
#endif
