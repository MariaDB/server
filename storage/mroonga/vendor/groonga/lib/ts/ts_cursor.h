/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#pragma once

#include "../grn.h"

#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GRN_TS_OBJ_CURSOR /* Wrapper cursor. */
} grn_ts_cursor_type;

#define GRN_TS_CURSOR_COMMON_MEMBERS\
  grn_ts_cursor_type type; /* Cursor type. */

typedef struct {
  GRN_TS_CURSOR_COMMON_MEMBERS
} grn_ts_cursor;

/*
 * grn_ts_obj_cursor_open() creates a wrapper cursor.
 * The new cursor will be a wrapper for a Groonga cursor specified by `obj`.
 * On success, `obj` will be closed in grn_ts_cursor_close().
 * On failure, `obj` is left as is.
 */
grn_rc grn_ts_obj_cursor_open(grn_ctx *ctx, grn_obj *obj,
                              grn_ts_cursor **cursor);

/* grn_ts_cursor_close() destroys a cursor. */
grn_rc grn_ts_cursor_close(grn_ctx *ctx, grn_ts_cursor *cursor);

/* grn_ts_cursor_read() reads records from a cursor. */
grn_rc grn_ts_cursor_read(grn_ctx *ctx, grn_ts_cursor *cursor,
                          grn_ts_record *out, size_t max_n_out, size_t *n_out);

#ifdef __cplusplus
}
#endif

