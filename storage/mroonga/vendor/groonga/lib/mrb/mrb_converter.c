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

#include "../ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>

#include "mrb_converter.h"

struct RClass *
grn_mrb_class_from_grn_obj(mrb_state *mrb, grn_obj *object)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_mrb_data *data;
  struct RClass *klass = NULL;

  data = &(ctx->impl->mrb);
  switch (object->header.type) {
  case GRN_ACCESSOR :
    klass = mrb_class_get_under(mrb, data->module, "Accessor");
    break;
  case GRN_BULK :
    klass = mrb_class_get_under(mrb, data->module, "Bulk");
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
  case GRN_EXPR :
    klass = mrb_class_get_under(mrb, data->module, "Expression");
    break;
  case GRN_PROC :
    klass = mrb_class_get_under(mrb, data->module, "Procedure");
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
#endif
