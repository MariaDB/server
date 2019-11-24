/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

/* Just for backward compatibility. Use grn_obj_is_true() instead. */
#define GRN_OBJ_IS_TRUE(ctx, obj, result) do {  \
  result = grn_obj_is_true(ctx, obj);           \
} while (0)

GRN_API grn_bool grn_obj_is_true(grn_ctx *ctx, grn_obj *obj);

GRN_API grn_bool grn_obj_is_builtin(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_bulk(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_text_family_bulk(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_table(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_scalar_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_vector_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_weight_vector_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_reference_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_data_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_index_column(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_accessor(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_key_accessor(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_type(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_text_family_type(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_tokenizer_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_function_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_selector_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_selector_only_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_normalizer_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_token_filter_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_scorer_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_window_function_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_expr(grn_ctx *ctx, grn_obj *obj);

GRN_API grn_rc grn_obj_cast(grn_ctx *ctx,
                            grn_obj *src,
                            grn_obj *dest,
                            grn_bool add_record_if_not_exist);

GRN_API grn_rc grn_obj_reindex(grn_ctx *ctx, grn_obj *obj);

GRN_API void grn_obj_touch(grn_ctx *ctx, grn_obj *obj, grn_timeval *tv);
GRN_API uint32_t grn_obj_get_last_modified(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_dirty(grn_ctx *ctx, grn_obj *obj);

GRN_API const char *grn_obj_type_to_string(uint8_t type);

GRN_API grn_bool grn_obj_name_is_column(grn_ctx *ctx,
                                        const char *name,
                                        int name_len);

GRN_API grn_bool grn_obj_is_corrupt(grn_ctx *ctx, grn_obj *obj);
GRN_API size_t grn_obj_get_disk_usage(grn_ctx *ctx, grn_obj *obj);

#ifdef __cplusplus
}
#endif
