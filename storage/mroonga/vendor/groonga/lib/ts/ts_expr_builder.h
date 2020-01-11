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
#include "ts_expr.h"
#include "ts_expr_node.h"
#include "ts_op.h"
#include "ts_str.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  grn_obj *src_table;  /* The source table of a bridge (no ref. count). */
  grn_obj *dest_table; /* The destination table of a bridge. */
  size_t n_nodes;      /* The stack depth (position) of a bridge. */
} grn_ts_expr_bridge;

typedef struct {
  grn_obj *table;              /* Associated table. */
  grn_obj *curr_table;         /* Current table (no ref. count). */
  grn_ts_expr_node **nodes;    /* Node stack. */
  size_t n_nodes;              /* Number of nodes (stack depth). */
  size_t max_n_nodes;          /* Maximum number of nodes (stack capacity). */
  grn_ts_expr_bridge *bridges; /* Bridges to subexpressions. */
  size_t n_bridges;            /* Number of bridges (subexpression depth). */
  size_t max_n_bridges;        /* Max. number (capacity) of bridges. */
} grn_ts_expr_builder;

/* grn_ts_expr_builder_open() creates an expression builder. */
grn_rc grn_ts_expr_builder_open(grn_ctx *ctx, grn_obj *table,
                                grn_ts_expr_builder **builder);

/* grn_ts_expr_builder_close() destroys an expression builder. */
grn_rc grn_ts_expr_builder_close(grn_ctx *ctx, grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_complete() completes an expression. */
grn_rc grn_ts_expr_builder_complete(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                    grn_ts_expr **expr);

/* grn_ts_expr_builder_clear() clears the internal states. */
grn_rc grn_ts_expr_builder_clear(grn_ctx *ctx, grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_push_name() pushes a named object. */
grn_rc grn_ts_expr_builder_push_name(grn_ctx *ctx,
                                     grn_ts_expr_builder *builder,
                                     grn_ts_str name);

/*
 * grn_ts_expr_builder_push_obj() pushes an object.
 *
 * Acceptable objects are as follows:
 * - Consts
 *  - GRN_BULK: GRN_DB_*.
 *  - GRN_UVECTOR: GRN_DB_* except GRN_DB_[SHORT/LONG_]TEXT.
 *  - GRN_VECTOR: GRN_DB_[SHORT/LONG_]TEXT.
 * - Columns
 *  - GRN_ACCESSOR: _id, _score, _key, _value, and columns.
 *  - GRN_COLUMN_FIX_SIZE: GRN_DB_* except GRN_DB_[SHORT/LONG_]TEXT.
 *  - GRN_COLUMN_VAR_SIZE: GRN_DB_[SHORT/LONG_]TEXT.
 */
grn_rc grn_ts_expr_builder_push_obj(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                    grn_obj *obj);

/* grn_ts_expr_builder_push_id() pushes "_id". */
grn_rc grn_ts_expr_builder_push_id(grn_ctx *ctx, grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_push_score() pushes "_score". */
grn_rc grn_ts_expr_builder_push_score(grn_ctx *ctx,
                                      grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_push_key() pushes "_key". */
grn_rc grn_ts_expr_builder_push_key(grn_ctx *ctx,
                                    grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_push_value() pushes "_value". */
grn_rc grn_ts_expr_builder_push_value(grn_ctx *ctx,
                                      grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_push_const() pushes a const. */
grn_rc grn_ts_expr_builder_push_const(grn_ctx *ctx,
                                      grn_ts_expr_builder *builder,
                                      grn_ts_data_kind kind,
                                      grn_ts_data_type type,
                                      grn_ts_any value);

/* grn_ts_expr_builder_push_column() pushes a column. */
grn_rc grn_ts_expr_builder_push_column(grn_ctx *ctx,
                                       grn_ts_expr_builder *builder,
                                       grn_obj *column);

/* grn_ts_expr_builder_push_op() pushes an operator. */
grn_rc grn_ts_expr_builder_push_op(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                   grn_ts_op_type op_type);

/* grn_ts_expr_builder_begin_subexpr() begins a subexpression. */
grn_rc grn_ts_expr_builder_begin_subexpr(grn_ctx *ctx,
                                         grn_ts_expr_builder *builder);

/* grn_ts_expr_builder_end_subexpr() ends a subexpression. */
grn_rc grn_ts_expr_builder_end_subexpr(grn_ctx *ctx,
                                       grn_ts_expr_builder *builder);

#ifdef __cplusplus
}
#endif

