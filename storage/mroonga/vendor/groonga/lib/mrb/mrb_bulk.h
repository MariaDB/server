/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2016 Brazil

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
#include "../grn_db.h"

#ifdef __cplusplus
extern "C" {
#endif

void grn_mrb_bulk_init(grn_ctx *ctx);

mrb_value grn_mrb_value_from_bulk(mrb_state *mrb, grn_obj *bulk);
grn_obj  *grn_mrb_value_to_bulk(mrb_state *mrb,
                                mrb_value mrb_value_,
                                grn_obj *bulk);
grn_bool grn_mrb_bulk_cast(mrb_state *mrb,
                           grn_obj *from,
                           grn_obj *to,
                           grn_id domain_id);

#ifdef __cplusplus
}
#endif
