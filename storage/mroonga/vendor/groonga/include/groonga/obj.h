/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef GROONGA_OBJ_H
#define GROONGA_OBJ_H

#ifdef  __cplusplus
extern "C" {
#endif

GRN_API grn_bool grn_obj_is_builtin(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_table(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_function_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_selector_proc(grn_ctx *ctx, grn_obj *obj);
GRN_API grn_bool grn_obj_is_scorer_proc(grn_ctx *ctx, grn_obj *obj);

#ifdef __cplusplus
}
#endif

#endif /* GROONGA_OBJ_H */
