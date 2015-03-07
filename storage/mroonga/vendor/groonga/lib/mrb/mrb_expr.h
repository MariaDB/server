/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013 Brazil

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

#ifndef GRN_MRB_EXPR_H
#define GRN_MRB_EXPR_H

#include "../grn_ctx.h"
#include "../grn_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

void grn_mrb_expr_init(grn_ctx *ctx);
scan_info **grn_mrb_scan_info_build(grn_ctx *ctx, grn_obj *expr, int *n, grn_operator op, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* GRN_MRB_EXPR_H */
