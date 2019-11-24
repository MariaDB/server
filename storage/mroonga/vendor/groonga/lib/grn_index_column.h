/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "grn_db.h"

#ifdef  __cplusplus
extern "C" {
#endif

void grn_index_column_init_from_env(void);
grn_rc grn_index_column_build(grn_ctx *ctx, grn_obj *index_column);
grn_rc grn_index_column_rebuild(grn_ctx *ctx, grn_obj *index_column);

#ifdef __cplusplus
}
#endif
