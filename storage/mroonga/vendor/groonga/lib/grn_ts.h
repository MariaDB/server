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

#include "grn.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * grn_ts_select() finds records passing through a filter and writes the values
 * of output columns (the evaluation results of output expressions) into the
 * output buffer (`ctx->impl->outbuf`).
 *
 * Note that the first `offset` records will be discarded and at most `limit`
 * records will be output.
 *
 * On success, grn_ts_select() returns GRN_SUCCESS.
 * On failure, grn_ts_select() returns an error code and set the details into
 * `ctx`.
 */
grn_rc grn_ts_select(grn_ctx *ctx, grn_obj *table,
                     const char *filter_ptr, size_t filter_len,
                     const char *scorer_ptr, size_t scorer_len,
                     const char *sortby_ptr, size_t sortby_len,
                     const char *output_columns_ptr, size_t output_columns_len,
                     size_t offset, size_t limit);

#ifdef __cplusplus
}
#endif
