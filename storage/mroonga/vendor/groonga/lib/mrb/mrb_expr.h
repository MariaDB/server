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
#include "../grn_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

void grn_mrb_expr_init(grn_ctx *ctx);

grn_obj *grn_mrb_expr_rewrite(grn_ctx *ctx, grn_obj *expr);
scan_info **grn_mrb_scan_info_build(grn_ctx *ctx,
                                    grn_obj *expr,
                                    int *n,
                                    grn_operator op,
                                    grn_bool record_exist);
unsigned int grn_mrb_expr_estimate_size(grn_ctx *ctx,
                                        grn_obj *expr,
                                        grn_obj *table);

#ifdef __cplusplus
}
#endif

