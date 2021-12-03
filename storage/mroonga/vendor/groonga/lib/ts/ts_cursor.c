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

#include "ts_cursor.h"

#include "../grn_ctx.h"
#include "../grn_dat.h"
#include "../grn_hash.h"
#include "../grn_pat.h"

#include "ts_log.h"
#include "ts_util.h"

/*-------------------------------------------------------------
 * grn_ts_obj_cursor.
 */

typedef struct {
  GRN_TS_CURSOR_COMMON_MEMBERS
  grn_obj *obj; /* Wrapped cursor object. */
} grn_ts_obj_cursor;

grn_rc
grn_ts_obj_cursor_open(grn_ctx *ctx, grn_obj *obj, grn_ts_cursor **cursor)
{
  grn_ts_obj_cursor *new_cursor;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!obj || !cursor) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (obj->header.type) {
    case GRN_CURSOR_TABLE_HASH_KEY:
    case GRN_CURSOR_TABLE_PAT_KEY:
    case GRN_CURSOR_TABLE_DAT_KEY:
    case GRN_CURSOR_TABLE_NO_KEY: {
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
    }
  }
  new_cursor = GRN_MALLOCN(grn_ts_obj_cursor, 1);
  if (!new_cursor) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_obj_cursor));
  }
  new_cursor->type = GRN_TS_OBJ_CURSOR;
  new_cursor->obj = obj;
  *cursor = (grn_ts_cursor *)new_cursor;
  return GRN_SUCCESS;
}

/* grn_ts_obj_cursor_close() destroys a wrapper cursor. */
static grn_rc
grn_ts_obj_cursor_close(grn_ctx *ctx, grn_ts_obj_cursor *cursor)
{
  if (cursor->obj) {
    grn_obj_close(ctx, cursor->obj);
  }
  GRN_FREE(cursor);
  return GRN_SUCCESS;
}

#define GRN_TS_OBJ_CURSOR_READ(type)\
  size_t i;\
  grn_ ## type ## _cursor *obj = (grn_ ## type ## _cursor *)cursor->obj;\
  for (i = 0; i < max_n_recs; i++) {\
    recs[i].id = grn_ ## type ## _cursor_next(ctx, obj);\
    if (!recs[i].id) {\
      break;\
    }\
    recs[i].score = 0;\
  }\
  *n_recs = i;\
  return GRN_SUCCESS;
/* grn_ts_obj_cursor_read() reads records from a wrapper cursor. */
static grn_rc
grn_ts_obj_cursor_read(grn_ctx *ctx, grn_ts_obj_cursor *cursor,
                       grn_ts_record *recs, size_t max_n_recs, size_t *n_recs)
{
  switch (cursor->obj->header.type) {
    case GRN_CURSOR_TABLE_HASH_KEY: {
      GRN_TS_OBJ_CURSOR_READ(hash)
    }
    case GRN_CURSOR_TABLE_PAT_KEY: {
      GRN_TS_OBJ_CURSOR_READ(pat)
    }
    case GRN_CURSOR_TABLE_DAT_KEY: {
      GRN_TS_OBJ_CURSOR_READ(dat)
    }
    case GRN_CURSOR_TABLE_NO_KEY: {
      GRN_TS_OBJ_CURSOR_READ(array)
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
    }
  }
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_cursor.
 */

grn_rc
grn_ts_cursor_close(grn_ctx *ctx, grn_ts_cursor *cursor)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!cursor) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (cursor->type) {
    case GRN_TS_OBJ_CURSOR: {
      return grn_ts_obj_cursor_close(ctx, (grn_ts_obj_cursor *)cursor);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid cursor type: %d",
                        cursor->type);
    }
  }
}

grn_rc
grn_ts_cursor_read(grn_ctx *ctx, grn_ts_cursor *cursor,
                   grn_ts_record *out, size_t max_n_out, size_t *n_out)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!cursor || (!out && max_n_out) || !n_out) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (cursor->type) {
    case GRN_TS_OBJ_CURSOR: {
      return grn_ts_obj_cursor_read(ctx, (grn_ts_obj_cursor *)cursor,
                                    out, max_n_out, n_out);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid cursor type: %d",
                        cursor->type);
    }
  }
}
