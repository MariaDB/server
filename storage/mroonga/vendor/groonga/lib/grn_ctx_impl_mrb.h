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

#ifndef GRN_CTX_IMPL_MRB_H
#define GRN_CTX_IMPL_MRB_H

#include "grn.h"
#include "grn_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

void grn_ctx_impl_mrb_init(grn_ctx *ctx);
void grn_ctx_impl_mrb_fin(grn_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* GRN_CTX_IMPL_MRB_H */
