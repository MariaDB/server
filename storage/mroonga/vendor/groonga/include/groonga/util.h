/*
  Copyright(C) 2010-2017 Brazil

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

#ifdef  __cplusplus
extern "C" {
#endif

GRN_API grn_obj *grn_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj);
GRN_API grn_obj *grn_inspect_indented(grn_ctx *ctx, grn_obj *buffer,
                                      grn_obj *obj, const char *indent);
GRN_API grn_obj *grn_inspect_limited(grn_ctx *ctx,
                                     grn_obj *buffer,
                                     grn_obj *obj);
GRN_API grn_obj *grn_inspect_name(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj);
GRN_API grn_obj *grn_inspect_encoding(grn_ctx *ctx, grn_obj *buffer, grn_encoding encoding);
GRN_API grn_obj *grn_inspect_type(grn_ctx *ctx, grn_obj *buffer, unsigned char type);
GRN_API grn_obj *grn_inspect_query_log_flags(grn_ctx *ctx,
                                             grn_obj *buffer,
                                             unsigned int flags);

GRN_API void grn_p(grn_ctx *ctx, grn_obj *obj);
GRN_API void grn_p_geo_point(grn_ctx *ctx, grn_geo_point *point);
GRN_API void grn_p_ii_values(grn_ctx *ctx, grn_obj *obj);

#ifdef __cplusplus
}
#endif
