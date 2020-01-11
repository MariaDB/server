/*
  Copyright(C) 2009-2016 Brazil

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

/* Just for backward compatibility.
   Use grn_type_id_is_text_family() instead. */
#define GRN_TYPE_IS_TEXT_FAMILY(type)                           \
  grn_type_id_is_text_family(NULL, (type))

GRN_API grn_bool grn_type_id_is_builtin(grn_ctx *ctx, grn_id id);
GRN_API grn_bool grn_type_id_is_number_family(grn_ctx *ctx, grn_id id);
GRN_API grn_bool grn_type_id_is_text_family(grn_ctx *ctx, grn_id id);

GRN_API grn_obj *grn_type_create(grn_ctx *ctx, const char *name, unsigned int name_size,
                                 grn_obj_flags flags, unsigned int size);
GRN_API uint32_t grn_type_size(grn_ctx *ctx, grn_obj *type);

#ifdef __cplusplus
}
#endif
