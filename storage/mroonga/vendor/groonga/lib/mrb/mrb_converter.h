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

#pragma once

#include "../grn_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRN_MRB_DATA_PTR(mrb_object)                            \
  (mrb_nil_p((mrb_object)) ? NULL : DATA_PTR((mrb_object)))

void grn_mrb_converter_init(grn_ctx *ctx);

typedef struct {
  grn_obj from;
  grn_obj to;
  union {
    int64_t time_value;
  } value;
} grn_mrb_value_to_raw_data_buffer;

void grn_mrb_value_to_raw_data_buffer_init(mrb_state *mrb,
                                           grn_mrb_value_to_raw_data_buffer *buffer);
void grn_mrb_value_to_raw_data_buffer_fin(mrb_state *mrb,
                                          grn_mrb_value_to_raw_data_buffer *buffer);
void grn_mrb_value_to_raw_data(mrb_state *mrb,
                               const char *context,
                               mrb_value mrb_value_,
                               grn_id domain_id,
                               grn_mrb_value_to_raw_data_buffer *buffer,
                               void **raw_value,
                               unsigned int *raw_value_size);
mrb_value grn_mrb_value_from_raw_data(mrb_state *mrb,
                                      grn_id domain,
                                      void *raw_value,
                                      unsigned int raw_value_size);

struct RClass *grn_mrb_class_from_grn_obj(mrb_state *mrb, grn_obj *object);
mrb_value      grn_mrb_value_from_grn_obj(mrb_state *mrb, grn_obj *object);

grn_id grn_mrb_class_to_grn_type(mrb_state *mrb, struct RClass *klass);
grn_id grn_mrb_value_to_grn_type(mrb_state *mrb, mrb_value value);

#ifdef __cplusplus
}
#endif
