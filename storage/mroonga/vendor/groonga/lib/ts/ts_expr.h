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
#include "ts_expr_node.h"
#include "ts_str.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------
 * Enumeration types.
 */

typedef enum {
  GRN_TS_EXPR_ID,      /* IDs (_id). */
  GRN_TS_EXPR_SCORE,   /* Scores (_score). */
  GRN_TS_EXPR_CONST,   /* A const. */
  GRN_TS_EXPR_VARIABLE /* An expression that contains a variable. */
} grn_ts_expr_type;

/*-------------------------------------------------------------
 * Expression components.
 */

typedef struct {
  grn_obj *table;             /* Associated table. */
  grn_ts_expr_type type;      /* Expression type. */
  grn_ts_data_kind data_kind; /* Abstract data type. */
  grn_ts_data_type data_type; /* Detailed data type. */
  grn_ts_expr_node *root;     /* Root node. */
} grn_ts_expr;

/* grn_ts_expr_open() creates an expression. */
grn_rc grn_ts_expr_open(grn_ctx *ctx, grn_obj *table, grn_ts_expr_node *root,
                        grn_ts_expr **expr);

/* grn_ts_expr_parse() parses a string and creates an expression. */
grn_rc grn_ts_expr_parse(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                         grn_ts_expr **expr);

/* grn_ts_expr_close() destroys an expression. */
grn_rc grn_ts_expr_close(grn_ctx *ctx, grn_ts_expr *expr);

/* grn_ts_expr_evaluate() evaluates an expression. */
grn_rc grn_ts_expr_evaluate(grn_ctx *ctx, grn_ts_expr *expr,
                            const grn_ts_record *in, size_t n_in, void *out);

/* grn_ts_expr_evaluate_to_buf() evaluates an expression. */
grn_rc grn_ts_expr_evaluate_to_buf(grn_ctx *ctx, grn_ts_expr *expr,
                                   const grn_ts_record *in, size_t n_in,
                                   grn_ts_buf *out);

/* grn_ts_expr_filter() filters records. */
grn_rc grn_ts_expr_filter(grn_ctx *ctx, grn_ts_expr *expr,
                          grn_ts_record *in, size_t n_in,
                          grn_ts_record *out, size_t *n_out);

/* grn_ts_expr_adjust() updates scores. */
grn_rc grn_ts_expr_adjust(grn_ctx *ctx, grn_ts_expr *expr,
                          grn_ts_record *io, size_t n_io);

#ifdef __cplusplus
}
#endif

