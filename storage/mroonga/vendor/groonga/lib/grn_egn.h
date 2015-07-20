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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef GRN_EGN_H
#define GRN_EGN_H

#include "grn.h"

#ifdef __cplusplus
extern "C" {
#endif

// Constant values.

typedef grn_operator grn_egn_operator_type;
typedef grn_builtin_type grn_egn_data_type;

typedef enum {
  GRN_EGN_ID_NODE,
  GRN_EGN_SCORE_NODE,
  GRN_EGN_CONSTANT_NODE,
  GRN_EGN_COLUMN_NODE,
  GRN_EGN_OPERATOR_NODE
} grn_egn_expression_node_type;

typedef enum {
  GRN_EGN_INCOMPLETE,
  GRN_EGN_ID,
  GRN_EGN_SCORE,
  GRN_EGN_CONSTANT,
  GRN_EGN_VARIABLE
} grn_egn_expression_type;

// Built-in data types.

typedef grn_id grn_egn_id;
typedef float grn_egn_score;
typedef struct {
  grn_egn_id id;
  grn_egn_score score;
} grn_egn_record;

typedef grn_bool grn_egn_bool;
typedef int64_t grn_egn_int;
typedef double grn_egn_float;
typedef int64_t grn_egn_time;
typedef struct {
  const char *ptr;
  size_t size;
} grn_egn_text;
typedef grn_geo_point grn_egn_geo_point;

/*
 * grn_egn_select() finds records passing through a filter (specified by
 * `filter' and `filter_size') and writes the associated values (specified by
 * `output_columns' and `output_columns_size') into the output buffer of `ctx'
 * (`ctx->impl->outbuf').
 *
 * Note that the first `offset` records will be discarded and at most `limit`
 * records will be output.
 *
 * On success, grn_egn_select() returns GRN_SUCCESS.
 * On failure, grn_egn_select() returns an error code and set the details into
 * `ctx`.
 */
grn_rc grn_egn_select(grn_ctx *ctx, grn_obj *table,
                      const char *filter, size_t filter_size,
                      const char *output_columns, size_t output_columns_size,
                      int offset, int limit);

#ifdef __cplusplus
}
#endif

#endif /* GRN_EGN_H */
