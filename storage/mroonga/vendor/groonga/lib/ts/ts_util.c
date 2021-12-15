/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "ts_util.h"

#include "../grn_dat.h"
#include "../grn_hash.h"
#include "../grn_pat.h"

#include "ts_log.h"

grn_rc
grn_ts_obj_increment_ref_count(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id = grn_obj_id(ctx, obj);
  grn_obj *obj_clone = grn_ctx_at(ctx, id);
  if (!obj_clone) {
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "grn_ctx_at failed: %d", id);
  }
  if (obj_clone != obj) {
    grn_obj_unlink(ctx, obj_clone);
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "wrong object: %p != %p",
                      obj, obj_clone);
  }
  return GRN_SUCCESS;
}

grn_ts_bool
grn_ts_obj_is_table(grn_ctx *ctx, grn_obj *obj)
{
  return grn_obj_is_table(ctx, obj);
}

grn_ts_bool
grn_ts_obj_is_column(grn_ctx *ctx, grn_obj *obj)
{
  switch (obj->header.type) {
    case GRN_COLUMN_FIX_SIZE:
    case GRN_COLUMN_VAR_SIZE: {
      return GRN_TRUE;
    }
    /* GRN_COLUMN_INDEX is not supported. */
    default: {
      return GRN_FALSE;
    }
  }
}

grn_rc
grn_ts_ja_get_value(grn_ctx *ctx, grn_obj *ja, grn_ts_id id,
                    grn_ts_buf *buf, size_t *value_size)
{
  grn_rc rc;
  uint32_t size;
  grn_io_win iw;
  char *ptr = (char *)grn_ja_ref(ctx, (grn_ja *)ja, id, &iw, &size);
  if (!ptr) {
    if (value_size) {
      *value_size = 0;
    }
    return GRN_SUCCESS;
  }
  rc = grn_ts_buf_write(ctx, buf, ptr, size);
  grn_ja_unref(ctx, &iw);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (value_size) {
    *value_size = size;
  }
  return GRN_SUCCESS;
}

grn_ts_bool
grn_ts_table_has_key(grn_ctx *ctx, grn_obj *table)
{
  switch (table->header.type) {
    case GRN_TABLE_HASH_KEY:
    case GRN_TABLE_PAT_KEY:
    case GRN_TABLE_DAT_KEY: {
      return GRN_TRUE;
    }
    default: {
      return GRN_FALSE;
    }
  }
}

grn_ts_bool
grn_ts_table_has_value(grn_ctx *ctx, grn_obj *table)
{
  return DB_OBJ(table)->range != GRN_DB_VOID;
}

const void *
grn_ts_table_get_value(grn_ctx *ctx, grn_obj *table, grn_ts_id id)
{
  switch (table->header.type) {
    case GRN_TABLE_HASH_KEY: {
      return grn_hash_get_value_(ctx, (grn_hash *)table, id, NULL);
    }
    case GRN_TABLE_PAT_KEY: {
      uint32_t size;
      return grn_pat_get_value_(ctx, (grn_pat *)table, id, &size);
    }
    /* GRN_TABLE_DAT_KEY does not support _value. */
    case GRN_TABLE_NO_KEY: {
      return _grn_array_get_value(ctx, (grn_array *)table, id);
    }
    default: {
      return NULL;
    }
  }
}
