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

#include "ts_expr.h"

#include <string.h>

#include "../grn_ctx.h"

#include "ts_log.h"
#include "ts_str.h"
#include "ts_util.h"
#include "ts_expr_parser.h"

/* grn_ts_expr_init() initializes an expression. */
static void
grn_ts_expr_init(grn_ctx *ctx, grn_ts_expr *expr)
{
  memset(expr, 0, sizeof(*expr));
  expr->table = NULL;
  expr->root = NULL;
}

/* grn_ts_expr_fin() finalizes an expression. */
static void
grn_ts_expr_fin(grn_ctx *ctx, grn_ts_expr *expr)
{
  if (expr->root) {
    grn_ts_expr_node_close(ctx, expr->root);
  }
  if (expr->table) {
    grn_obj_unlink(ctx, expr->table);
  }
}

grn_rc
grn_ts_expr_open(grn_ctx *ctx, grn_obj *table, grn_ts_expr_node *root,
                 grn_ts_expr **expr)
{
  grn_rc rc;
  grn_ts_expr *new_expr;
  grn_ts_expr_type type;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !root || !expr) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (root->type) {
    case GRN_TS_EXPR_ID_NODE: {
      type = GRN_TS_EXPR_ID;
      break;
    }
    case GRN_TS_EXPR_SCORE_NODE: {
      type = GRN_TS_EXPR_SCORE;
      break;
    }
    case GRN_TS_EXPR_KEY_NODE:
    case GRN_TS_EXPR_VALUE_NODE: {
      type = GRN_TS_EXPR_VARIABLE;
      break;
    }
    case GRN_TS_EXPR_CONST_NODE: {
      type = GRN_TS_EXPR_CONST;
      break;
    }
    case GRN_TS_EXPR_COLUMN_NODE:
    case GRN_TS_EXPR_OP_NODE:
    case GRN_TS_EXPR_BRIDGE_NODE: {
      type = GRN_TS_EXPR_VARIABLE;
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
    }
  }
  new_expr = GRN_MALLOCN(grn_ts_expr, 1);
  if (!new_expr) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE,
                      sizeof(grn_ts_expr));
  }
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    GRN_FREE(new_expr);
    return rc;
  }
  grn_ts_expr_init(ctx, new_expr);
  new_expr->table = table;
  new_expr->type = type;
  new_expr->data_kind = root->data_kind;
  new_expr->data_type = root->data_type;
  new_expr->root = root;
  *expr = new_expr;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_parse(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                  grn_ts_expr **expr)
{
  grn_rc rc;
  grn_ts_expr *new_expr;
  grn_ts_expr_parser *parser;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) ||
      (!str.ptr && str.size) || !expr) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_parser_open(ctx, table, &parser);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_parser_parse(ctx, parser, str, &new_expr);
  grn_ts_expr_parser_close(ctx, parser);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *expr = new_expr;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_close(grn_ctx *ctx, grn_ts_expr *expr)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!expr) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_expr_fin(ctx, expr);
  GRN_FREE(expr);
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_evaluate_to_buf(grn_ctx *ctx, grn_ts_expr *expr,
                            const grn_ts_record *in, size_t n_in,
                            grn_ts_buf *out)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!expr || (!in && n_in) || !out) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (!n_in) {
    return GRN_SUCCESS;
  }
  return grn_ts_expr_node_evaluate_to_buf(ctx, expr->root, in, n_in, out);
}

grn_rc
grn_ts_expr_evaluate(grn_ctx *ctx, grn_ts_expr *expr,
                     const grn_ts_record *in, size_t n_in, void *out)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!expr || (!in && n_in) || (n_in && !out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (!n_in) {
    return GRN_SUCCESS;
  }
  return grn_ts_expr_node_evaluate(ctx, expr->root, in, n_in, out);
}

grn_rc
grn_ts_expr_filter(grn_ctx *ctx, grn_ts_expr *expr,
                   grn_ts_record *in, size_t n_in,
                   grn_ts_record *out, size_t *n_out)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!expr || (!in && n_in) || !out || !n_out) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (!n_in) {
    *n_out = 0;
    return GRN_SUCCESS;
  }
  return grn_ts_expr_node_filter(ctx, expr->root, in, n_in, out, n_out);
}

grn_rc
grn_ts_expr_adjust(grn_ctx *ctx, grn_ts_expr *expr,
                   grn_ts_record *io, size_t n_io)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!expr || (!io && n_io)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (!n_io) {
    return GRN_SUCCESS;
  }
  return grn_ts_expr_node_adjust(ctx, expr->root, io, n_io);
}
