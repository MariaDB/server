/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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

#include "grn.h"
#include "grn_db.h"

grn_bool
grn_id_is_builtin(grn_ctx *ctx, grn_id id)
{
  if (id == GRN_ID_NIL) {
    return GRN_FALSE;
  } else {
    return id < GRN_N_RESERVED_TYPES;
  }
}

grn_bool
grn_id_is_builtin_type(grn_ctx *ctx, grn_id id)
{
  return GRN_DB_OBJECT <= id && id <= GRN_DB_WGS84_GEO_POINT;
}
