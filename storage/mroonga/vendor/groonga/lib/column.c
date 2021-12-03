/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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
#include "grn_store.h"
#include "grn_ii.h"

grn_column_flags
grn_column_get_flags(grn_ctx *ctx, grn_obj *column)
{
  grn_column_flags flags = 0;

  GRN_API_ENTER;

  if (!column) {
    GRN_API_RETURN(0);
  }

  switch (column->header.type) {
  case GRN_COLUMN_FIX_SIZE :
    flags = column->header.flags;
    break;
  case GRN_COLUMN_VAR_SIZE :
    flags = grn_ja_get_flags(ctx, (grn_ja *)column);
    break;
  case GRN_COLUMN_INDEX :
    flags = grn_ii_get_flags(ctx, (grn_ii *)column);
    break;
  default :
    break;
  }

  GRN_API_RETURN(flags);
}
