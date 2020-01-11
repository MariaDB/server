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

#include "ts_expr.h"
#include "ts_str.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: Sorting should take into account the order of input records. */

typedef struct grn_ts_sorter_node {
  grn_ts_expr *expr;               /* Expression. */
  grn_ts_bool reverse;             /* Reverse order or not. */
  grn_ts_buf buf;                  /* Buffer for values. */
  struct grn_ts_sorter_node *next; /* Next node. */
} grn_ts_sorter_node;

typedef struct {
  grn_obj *table;           /* Table. */
  grn_ts_sorter_node *head; /* First node. */
  size_t offset;            /* Top `offset` records will be discarded. */
  size_t limit;             /* At most `limit` records will be left. */
  grn_ts_bool partial;      /* Partial sorting or not. */
} grn_ts_sorter;

/* grn_ts_sorter_open() creates a sorter. */
grn_rc grn_ts_sorter_open(grn_ctx *ctx, grn_obj *table,
                          grn_ts_sorter_node *head, size_t offset,
                          size_t limit, grn_ts_sorter **sorter);

/* grn_ts_sorter_parse() parses a string and creates a sorter. */
grn_rc grn_ts_sorter_parse(grn_ctx *ctx, grn_obj *table,
                           grn_ts_str str, size_t offset,
                           size_t limit, grn_ts_sorter **sorter);

/* grn_ts_sorter_close() destroys a sorter. */
grn_rc grn_ts_sorter_close(grn_ctx *ctx, grn_ts_sorter *sorter);

/* grn_ts_sorter_progress() progresses sorting. */
grn_rc grn_ts_sorter_progress(grn_ctx *ctx, grn_ts_sorter *sorter,
                              grn_ts_record *recs, size_t n_recs,
                              size_t *n_rest);

/* grn_ts_sorter_complete() completes sorting. */
grn_rc grn_ts_sorter_complete(grn_ctx *ctx, grn_ts_sorter *sorter,
                              grn_ts_record *recs, size_t n_recs,
                              size_t *n_rest);

typedef struct {
  grn_obj *table;           /* Table. */
  grn_ts_sorter_node *head; /* First node. */
  grn_ts_sorter_node *tail; /* Last node. */
} grn_ts_sorter_builder;

/* grn_ts_sorter_builder_open() creates a sorter builder. */
grn_rc grn_ts_sorter_builder_open(grn_ctx *ctx, grn_obj *table,
                                  grn_ts_sorter_builder **builder);

/* grn_ts_sorter_builder_close() destroys a sorter builder. */
grn_rc grn_ts_sorter_builder_close(grn_ctx *ctx,
                                   grn_ts_sorter_builder *builder);

/* grn_ts_sorter_builder_complete() completes a sorter. */
grn_rc grn_ts_sorter_builder_complete(grn_ctx *ctx,
                                      grn_ts_sorter_builder *builder,
                                      size_t offset, size_t limit,
                                      grn_ts_sorter **sorter);

/* grn_ts_sorter_builder_push() pushes a node. */
grn_rc grn_ts_sorter_builder_push(grn_ctx *ctx, grn_ts_sorter_builder *builder,
                                  grn_ts_expr *expr, grn_ts_bool reverse);

#ifdef __cplusplus
}
#endif

