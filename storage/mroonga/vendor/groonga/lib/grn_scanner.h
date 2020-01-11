/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "grn_expr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _grn_scaner {
  grn_obj *expr;
  grn_obj *source_expr;
  scan_info **sis;
  unsigned int n_sis;
} grn_scanner;

grn_scanner *grn_scanner_open(grn_ctx *ctx, grn_obj *expr,
                              grn_operator op, grn_bool record_exist);
void grn_scanner_close(grn_ctx *ctx, grn_scanner *scanner);

#ifdef __cplusplus
}
#endif
