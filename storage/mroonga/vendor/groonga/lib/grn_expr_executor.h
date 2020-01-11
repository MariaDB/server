/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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

#include "grn_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _grn_expr_executor grn_expr_executor;

grn_expr_executor *grn_expr_executor_open(grn_ctx *ctx,
                                          grn_obj *expr);
grn_obj *grn_expr_executor_exec(grn_ctx *ctx,
                                grn_expr_executor *executor,
                                grn_id id);
grn_rc grn_expr_executor_close(grn_ctx *ctx,
                               grn_expr_executor *executor);

#ifdef __cplusplus
}
#endif
