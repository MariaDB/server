/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2010-2011 Brazil

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
#ifndef GRN_UTIL_H
#define GRN_UTIL_H

#ifndef GROONGA_IN_H
#include "groonga_in.h"
#endif /* GROONGA_IN_H */

#ifndef GRN_CTX_H
#include "ctx.h"
#endif /* GRN_CTX_H */

#ifdef __cplusplus
extern "C" {
#endif

GRN_API grn_rc grn_normalize_offset_and_limit(grn_ctx *ctx, int size, int *offset, int *limit);

GRN_API grn_obj *grn_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj);
GRN_API grn_obj *grn_inspect_name(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj);
GRN_API grn_obj *grn_inspect_encoding(grn_ctx *ctx, grn_obj *buffer, grn_encoding encoding);
GRN_API grn_obj *grn_inspect_type(grn_ctx *ctx, grn_obj *buffer, unsigned char type);
void grn_p(grn_ctx *ctx, grn_obj *obj);
void grn_p_geo_point(grn_ctx *ctx, grn_geo_point *point);

GRN_API const char *grn_win32_base_dir(void);
GRN_API char *grn_path_separator_to_system(char *dest, char *groonga_path);

#ifdef __cplusplus
}
#endif

#endif /* GRN_UTIL_H */
