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

#include "ts_buf.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* grn_ts_obj_increment_ref_count() increments an object reference count. */
grn_rc grn_ts_obj_increment_ref_count(grn_ctx *ctx, grn_obj *obj);

/* grn_ts_obj_is_table() returns whether or not an object is a table. */
grn_ts_bool grn_ts_obj_is_table(grn_ctx *ctx, grn_obj *obj);

/* grn_ts_obj_is_column() returns whether or not an object is a column. */
grn_ts_bool grn_ts_obj_is_column(grn_ctx *ctx, grn_obj *obj);

/*
 * grn_ts_ja_get_value() gets a value from ja and writes it to buf. Note that
 * the value is appended to the end of buf.
 */
grn_rc grn_ts_ja_get_value(grn_ctx *ctx, grn_obj *ja, grn_ts_id id,
                           grn_ts_buf *buf, size_t *value_size);

/* grn_ts_table_has_key() returns whether or not a table has _key. */
grn_ts_bool grn_ts_table_has_key(grn_ctx *ctx, grn_obj *table);

/* grn_ts_table_has_value() returns whether or not a table has _value. */
grn_ts_bool grn_ts_table_has_value(grn_ctx *ctx, grn_obj *table);

/*
 * grn_ts_table_get_value() gets a reference to a value (_value). On failure,
 * this function returns NULL.
 */
const void *grn_ts_table_get_value(grn_ctx *ctx, grn_obj *table, grn_ts_id id);

#ifdef __cplusplus
}
#endif

