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

#include "mrb_ctx.h"
#include "mrb_database.h"
#include "mrb_converter.h"

static struct mrb_data_type mrb_grn_database_type = {
  "Groonga::Database",
  NULL
};

static mrb_value
mrb_grn_database_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_database_ptr;

  mrb_get_args(mrb, "o", &mrb_database_ptr);
  DATA_TYPE(self) = &mrb_grn_database_type;
  DATA_PTR(self) = mrb_cptr(mrb_database_ptr);
  return self;
}

static mrb_value
mrb_grn_database_class_open(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *database;
  char *path;

  mrb_get_args(mrb, "z", &path);

  database = grn_db_open(ctx, path);
  grn_mrb_ctx_check(mrb);

  return mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, database));
}

static mrb_value
mrb_grn_database_class_create(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *database;
  char *path;

  mrb_get_args(mrb, "z", &path);

  database = grn_db_create(ctx, path, NULL);
  grn_mrb_ctx_check(mrb);

  return mrb_funcall(mrb, klass, "new", 1, mrb_cptr_value(mrb, database));
}

static mrb_value
mrb_grn_database_recover(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  grn_db_recover(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_nil_value();
}

static mrb_value
mrb_grn_database_is_locked(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  unsigned int is_locked;

  is_locked = grn_obj_is_locked(ctx, DATA_PTR(self));
  grn_mrb_ctx_check(mrb);

  return mrb_bool_value(is_locked != 0);
}

static mrb_value
mrb_grn_database_get_last_modified(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  uint32_t last_modified;
  struct RClass *time_class;

  last_modified = grn_db_get_last_modified(ctx, DATA_PTR(self));

  time_class = mrb_class_get(mrb, "Time");
  return mrb_funcall(mrb,
                     mrb_obj_value(time_class),
                     "at",
                     1,
                     mrb_float_value(mrb, last_modified));
}

static mrb_value
mrb_grn_database_is_dirty(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_bool is_dirty;

  is_dirty = grn_db_is_dirty(ctx, DATA_PTR(self));

  return mrb_bool_value(is_dirty);
}

static mrb_value
mrb_grn_database_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *database;
  mrb_value mrb_id_or_key;

  mrb_get_args(mrb, "o", &mrb_id_or_key);

  database = DATA_PTR(self);

  if (mrb_fixnum_p(mrb_id_or_key)) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size;

    name_size = grn_table_get_key(ctx,
                                  grn_ctx_db(ctx),
                                  mrb_fixnum(mrb_id_or_key),
                                  name,
                                  GRN_TABLE_MAX_KEY_SIZE);
    if (name_size == 0) {
      return mrb_nil_value();
    } else {
      return mrb_str_new(mrb, name, name_size);
    }
  } else {
    grn_id name_domain_id = GRN_DB_SHORT_TEXT;
    grn_id id;
    grn_mrb_value_to_raw_data_buffer buffer;
    void *name;
    unsigned int name_size;

    grn_mrb_value_to_raw_data_buffer_init(mrb, &buffer);
    grn_mrb_value_to_raw_data(mrb, "name", mrb_id_or_key,
                              name_domain_id, &buffer,
                              &name, &name_size);
    id = grn_table_get(ctx, database, name, name_size);
    grn_mrb_value_to_raw_data_buffer_fin(mrb, &buffer);

    if (id == GRN_ID_NIL) {
      return mrb_nil_value();
    } else {
      return mrb_fixnum_value(id);
    }
  }
}

void
grn_mrb_database_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *object_class = data->object_class;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Database", object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "open",
                          mrb_grn_database_class_open,
                          MRB_ARGS_REQ(1));
  mrb_define_class_method(mrb, klass, "create",
                          mrb_grn_database_class_create,
                          MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_database_initialize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "recover",
                    mrb_grn_database_recover, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "locked?",
                    mrb_grn_database_is_locked, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "last_modified",
                    mrb_grn_database_get_last_modified, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "dirty?",
                    mrb_grn_database_is_dirty, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "[]",
                    mrb_grn_database_array_reference, MRB_ARGS_REQ(1));
}
#endif
