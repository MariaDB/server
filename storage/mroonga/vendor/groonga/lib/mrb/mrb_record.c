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
#include <mruby/variable.h>
#include <mruby/data.h>

#include "../grn_db.h"
#include "mrb_record.h"
#include "mrb_bulk.h"

typedef struct {
  grn_obj *table;
  grn_id id;
  grn_obj key;
} grn_mrb_record;

static void
mrb_grn_record_free(mrb_state *mrb, void *data)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_mrb_record *record = data;

  if (!record) {
    return;
  }

  GRN_OBJ_FIN(ctx, &(record->key));
  mrb_free(mrb, record);
}

static struct mrb_data_type mrb_grn_record_type = {
  "Groonga::Record",
  mrb_grn_record_free
};

static mrb_value
mrb_grn_record_initialize(mrb_state *mrb, mrb_value self)
{
  grn_mrb_record *record;
  mrb_value mrb_table;
  mrb_value mrb_id;

  mrb_get_args(mrb, "oo", &mrb_table, &mrb_id);

  DATA_TYPE(self) = &mrb_grn_record_type;

  record = mrb_malloc(mrb, sizeof(grn_mrb_record));
  record->table = DATA_PTR(mrb_table);
  if (mrb_nil_p(mrb_id)) {
    record->id = GRN_ID_NIL;
  } else {
    record->id = mrb_fixnum(mrb_id);
  }

  switch (record->table->header.domain) {
  case GRN_ID_NIL :
  case GRN_DB_SHORT_TEXT :
    GRN_SHORT_TEXT_INIT(&(record->key), 0);
    break;
  default :
    GRN_VALUE_FIX_SIZE_INIT(&(record->key), 0, record->table->header.domain);
    break;
  }

  DATA_PTR(self) = record;

  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@table"), mrb_table);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@id"), mrb_id);

  return self;
}

static mrb_value
mrb_grn_record_set_id(mrb_state *mrb, mrb_value self)
{
  grn_mrb_record *record;
  mrb_value mrb_id;

  record = DATA_PTR(self);
  mrb_get_args(mrb, "o", &mrb_id);

  if (mrb_nil_p(mrb_id)) {
    record->id = GRN_ID_NIL;
  } else {
    record->id = mrb_fixnum(mrb_id);
  }
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@id"), mrb_id);

  return mrb_id;
}

static mrb_value
mrb_grn_record_key(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_mrb_record *record;
  int key_size;

  record = DATA_PTR(self);

  if (record->id == GRN_ID_NIL) {
    return mrb_nil_value();
  }

  if (record->table->header.type == GRN_TABLE_NO_KEY) {
    return mrb_nil_value();
  }

  GRN_BULK_REWIND(&(record->key));
  key_size = grn_table_get_key(ctx, record->table, record->id,
                               GRN_BULK_HEAD(&(record->key)),
                               GRN_BULK_VSIZE(&(record->key)));
  if (key_size > GRN_BULK_VSIZE(&(record->key))) {
    grn_bulk_space(ctx, &(record->key), key_size);
    key_size = grn_table_get_key(ctx, record->table, record->id,
                                 GRN_BULK_HEAD(&(record->key)),
                                 GRN_BULK_VSIZE(&(record->key)));
  }

  return grn_mrb_value_from_bulk(mrb, &(record->key));
}

void
grn_mrb_record_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Record", data->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_record_initialize, MRB_ARGS_REQ(2));

  mrb_define_method(mrb, klass, "id=",
                    mrb_grn_record_set_id, MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "key",
                    mrb_grn_record_key, MRB_ARGS_NONE());
}
#endif
