/*
  Copyright(C) 2009-2017 Brazil

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

typedef grn_bool grn_operator_exec_func(grn_ctx *ctx,
                                        grn_obj *x,
                                        grn_obj *y);

GRN_API const char *grn_operator_to_string(grn_operator op);
GRN_API grn_operator_exec_func *grn_operator_to_exec_func(grn_operator op);
GRN_API grn_bool grn_operator_exec_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_not_equal(grn_ctx *ctx,
                                             grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_less(grn_ctx *ctx, grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_greater(grn_ctx *ctx, grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_less_equal(grn_ctx *ctx,
                                              grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_greater_equal(grn_ctx *ctx,
                                                 grn_obj *x, grn_obj *y);
GRN_API grn_bool grn_operator_exec_match(grn_ctx *ctx,
                                         grn_obj *target, grn_obj *sub_text);
GRN_API grn_bool grn_operator_exec_prefix(grn_ctx *ctx,
                                          grn_obj *target, grn_obj *prefix);
GRN_API grn_bool grn_operator_exec_regexp(grn_ctx *ctx,
                                          grn_obj *target, grn_obj *pattern);

#ifdef __cplusplus
}
#endif
