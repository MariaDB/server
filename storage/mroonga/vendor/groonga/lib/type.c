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

#include "grn_ctx_impl.h"
#include "grn_db.h"

grn_bool
grn_type_id_is_builtin(grn_ctx *ctx, grn_id id)
{
  return id >= GRN_DB_OBJECT && id <= GRN_DB_WGS84_GEO_POINT;
}

grn_bool
grn_type_id_is_number_family(grn_ctx *ctx, grn_id id)
{
  return GRN_DB_INT8 <= id && id <= GRN_DB_FLOAT;
}

grn_bool
grn_type_id_is_text_family(grn_ctx *ctx, grn_id id)
{
  return GRN_DB_SHORT_TEXT <= id && id <= GRN_DB_LONG_TEXT;
}

grn_obj *
grn_type_create(grn_ctx *ctx, const char *name, unsigned int name_size,
                grn_obj_flags flags, unsigned int size)
{
  grn_id id;
  struct _grn_type *res = NULL;
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  GRN_API_ENTER;
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[type][create]", name, name_size);
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
    GRN_API_RETURN(NULL);
  }
  id = grn_obj_register(ctx, db, name, name_size);
  if (id && (res = GRN_MALLOC(sizeof(grn_db_obj)))) {
    GRN_DB_OBJ_SET_TYPE(res, GRN_TYPE);
    res->obj.header.flags = flags;
    res->obj.header.domain = GRN_ID_NIL;
    GRN_TYPE_SIZE(&res->obj) = size;
    if (grn_db_obj_init(ctx, db, id, DB_OBJ(res))) {
      // grn_obj_delete(ctx, db, id);
      GRN_FREE(res);
      GRN_API_RETURN(NULL);
    }
  }
  GRN_API_RETURN((grn_obj *)res);
}

uint32_t
grn_type_size(grn_ctx *ctx, grn_obj *type)
{
  uint32_t size;

  GRN_API_ENTER;
  if (!type) {
    ERR(GRN_INVALID_ARGUMENT, "[type][size] type is NULL");
    GRN_API_RETURN(0);
  }
  size = GRN_TYPE_SIZE(DB_OBJ(type));
  GRN_API_RETURN(size);
}
