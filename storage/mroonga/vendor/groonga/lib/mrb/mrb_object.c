/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2016 Brazil

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
#include "../grn_util.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/string.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "../grn_mrb.h"
#include "mrb_ctx.h"
#include "mrb_object.h"
#include "mrb_operator.h"
#include "mrb_options.h"
#include "mrb_converter.h"

static mrb_value
object_remove_force(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *name;
  mrb_int name_size;

  mrb_get_args(mrb, "s", &name, &name_size);
  grn_obj_remove_force(ctx, name, name_size);
  grn_mrb_ctx_check(mrb);

  return mrb_nil_value();
}

mrb_value
grn_mrb_object_inspect(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  mrb_value inspected;

  object = DATA_PTR(self);
  inspected = mrb_str_buf_new(mrb, 48);

  mrb_str_cat_lit(mrb, inspected, "#<");
  mrb_str_cat_cstr(mrb, inspected, mrb_obj_classname(mrb, self));
  mrb_str_cat_lit(mrb, inspected, ":");
  mrb_str_concat(mrb, inspected, mrb_ptr_to_str(mrb, mrb_cptr(self)));
  if (object) {
    grn_obj buffer;
    GRN_TEXT_INIT(&buffer, 0);
    grn_inspect(ctx, &buffer, object);
    mrb_str_cat_lit(mrb, inspected, " ");
    mrb_str_cat(mrb, inspected, GRN_TEXT_VALUE(&buffer), GRN_TEXT_LEN(&buffer));
    GRN_OBJ_FIN(ctx, &buffer);
  } else {
    mrb_str_cat_lit(mrb, inspected, " (closed)");
  }
  mrb_str_cat_lit(mrb, inspected, ">");

  return inspected;
}

static mrb_value
object_get_id(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_id id;

  id = grn_obj_id(ctx, DATA_PTR(self));

  return mrb_fixnum_value(id);
}

static mrb_value
object_get_name(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_length;

  object = DATA_PTR(self);
  name_length = grn_obj_name(ctx, object, name, GRN_TABLE_MAX_KEY_SIZE);

  if (name_length == 0) {
    return mrb_nil_value();
  } else {
    return mrb_str_new(mrb, name, name_length);
  }
}

static mrb_value
object_get_path(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  const char *path;

  object = DATA_PTR(self);
  path = grn_obj_path(ctx, object);

  if (path) {
    return mrb_str_new_cstr(mrb, path);
  } else {
    return mrb_nil_value();
  }
}

static mrb_value
object_grn_inspect(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj buffer;
  mrb_value inspected;

  GRN_TEXT_INIT(&buffer, 0);
  grn_inspect(ctx, &buffer, DATA_PTR(self));
  inspected = mrb_str_new(mrb, GRN_TEXT_VALUE(&buffer), GRN_TEXT_LEN(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);

  return inspected;
}

static mrb_value
object_equal(mrb_state *mrb, mrb_value self)
{
  grn_obj *object, *other_object;
  mrb_value mrb_other;

  mrb_get_args(mrb, "o", &mrb_other);
  if (!mrb_obj_is_kind_of(mrb, mrb_other, mrb_obj_class(mrb, self))) {
    return mrb_false_value();
  }

  object = DATA_PTR(self);
  other_object = DATA_PTR(mrb_other);
  return mrb_bool_value(object == other_object);
}

static mrb_value
object_hash(mrb_state *mrb, mrb_value self)
{
  grn_obj *object;

  object = DATA_PTR(self);
  return mrb_fixnum_value((mrb_int)((uint64_t)object));
}

static mrb_value
object_close(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;

  object = DATA_PTR(self);
  if (!object) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "already closed object");
  }

  grn_obj_close(ctx, object);
  DATA_PTR(self) = NULL;

  return mrb_nil_value();
}

static mrb_value
object_remove(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_options = mrb_nil_value();
  grn_bool dependent = GRN_FALSE;
  grn_obj *object;

  mrb_get_args(mrb, "|H", &mrb_options);
  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_dependent;
    mrb_dependent = grn_mrb_options_get_lit(mrb, mrb_options, "dependent");
    dependent = mrb_test(mrb_dependent);
  }

  object = DATA_PTR(self);
  if (dependent) {
    grn_obj_remove_dependent(ctx, object);
  } else {
    grn_obj_remove(ctx, object);
  }
  grn_mrb_ctx_check(mrb);

  DATA_PTR(self) = NULL;

  return mrb_nil_value();
}

static mrb_value
object_is_closed(mrb_state *mrb, mrb_value self)
{
  grn_obj *object;

  object = DATA_PTR(self);
  return mrb_bool_value(object == NULL);
}

static mrb_value
object_get_domain_id(mrb_state *mrb, mrb_value self)
{
  grn_obj *object;
  grn_id domain_id;

  object = DATA_PTR(self);
  domain_id = object->header.domain;

  if (domain_id == GRN_ID_NIL) {
    return mrb_nil_value();
  } else {
    return mrb_fixnum_value(domain_id);
  }
}

static mrb_value
object_get_range_id(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  grn_id range_id;

  object = DATA_PTR(self);
  range_id = grn_obj_get_range(ctx, object);

  if (range_id == GRN_ID_NIL) {
    return mrb_nil_value();
  } else {
    return mrb_fixnum_value(range_id);
  }
}

static mrb_value
object_is_temporary(mrb_state *mrb, mrb_value self)
{
  grn_obj *object;
  grn_obj_flags flags;

  object = DATA_PTR(self);
  flags = object->header.flags;
  return mrb_bool_value((flags & GRN_OBJ_PERSISTENT) != GRN_OBJ_PERSISTENT);
}

static mrb_value
object_is_persistent(mrb_state *mrb, mrb_value self)
{
  grn_obj *object;
  grn_obj_flags flags;

  object = DATA_PTR(self);
  flags = object->header.flags;
  return mrb_bool_value((flags & GRN_OBJ_PERSISTENT) == GRN_OBJ_PERSISTENT);
}

static mrb_value
object_is_true(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;

  object = DATA_PTR(self);
  return mrb_bool_value(grn_obj_is_true(ctx, object));
}

static mrb_value
object_check_corrupt(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_obj *object;
  grn_bool is_corrupt;

  object = DATA_PTR(self);
  is_corrupt = grn_obj_is_corrupt(ctx, object);
  grn_mrb_ctx_check(mrb);
  return mrb_bool_value(is_corrupt);
}

void
grn_mrb_object_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Object", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);
  data->object_class = klass;

  mrb_define_class_method(mrb,
                          klass,
                          "remove_force",
                          object_remove_force,
                          MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "inspect",
                    grn_mrb_object_inspect, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "id", object_get_id, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "name", object_get_name, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "path", object_get_path, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "grn_inspect",
                    object_grn_inspect, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "==", object_equal, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "eql?", object_equal, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "hash", object_hash, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "close", object_close, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "remove", object_remove, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, klass, "closed?", object_is_closed, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "domain_id", object_get_domain_id,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "range_id", object_get_range_id,
                    MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "temporary?", object_is_temporary,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "persistent?", object_is_persistent,
                    MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "true?", object_is_true, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "check_corrupt", object_check_corrupt,
                    MRB_ARGS_NONE());

  grn_mrb_load(ctx, "index_info.rb");
}
#endif
