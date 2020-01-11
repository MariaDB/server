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
#include "ts_cursor.h"
#include "ts_expr.h"
#include "ts_sorter.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int REMOVE_ME;
} grn_ts_plan_node;

typedef struct {
  grn_obj *table;
  grn_ts_plan_node *root;
} grn_ts_plan;

/* grn_ts_plan_open() creates a plan. */
grn_rc grn_ts_plan_open(grn_ctx *ctx, grn_obj *table, grn_ts_plan_node *root,
                        grn_ts_plan **plan);

/* grn_ts_plan_close() destroys a plan. */
grn_rc grn_ts_plan_close(grn_ctx *ctx, grn_ts_plan *plan);

/* grn_ts_plan_exec() executes a plan. */
grn_rc grn_ts_plan_exec(grn_ctx *ctx, grn_ts_plan *plan,
                        grn_ts_rbuf *rbuf, size_t *n_hits);

typedef struct {
  grn_obj *table;
} grn_ts_planner;

/* grn_ts_planner_open() creates a planner. */
grn_rc grn_ts_planner_open(grn_ctx *ctx, grn_obj *table,
                           grn_ts_planner **planner);

/* grn_ts_planner_close() destroys a planner. */
grn_rc grn_ts_planner_close(grn_ctx *ctx, grn_ts_planner *planner);

/* grn_ts_planner_complete() completes a planner. */
grn_rc grn_ts_planner_complete(grn_ctx *ctx, grn_ts_planner *planner,
                               grn_ts_plan **plan);

/* grn_ts_planner_push_cursor() pushes a cursor. */
grn_rc grn_ts_planner_push_cursor(grn_ctx *ctx, grn_ts_planner *planner,
                                  grn_ts_cursor *cursor);

/* grn_ts_planner_push_filter() pushes a filter. */
grn_rc grn_ts_planner_push_filter(grn_ctx *ctx, grn_ts_planner *planner,
                                  grn_ts_expr *expr);

/* grn_ts_planner_push_scorer() pushes a scorer. */
grn_rc grn_ts_planner_push_scorer(grn_ctx *ctx, grn_ts_planner *planner,
                                  grn_ts_expr *expr);

/* grn_ts_planner_push_sorter() pushes a sorter. */
grn_rc grn_ts_planner_push_sorter(grn_ctx *ctx, grn_ts_planner *planner,
                                  grn_ts_sorter *sorter);

#ifdef __cplusplus
}
#endif

