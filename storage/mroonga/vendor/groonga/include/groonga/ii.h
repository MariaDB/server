/*
  Copyright(C) 2009-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

/* buffered index builder */

typedef struct _grn_ii grn_ii;
typedef struct _grn_ii_buffer grn_ii_buffer;

GRN_API uint32_t grn_ii_get_n_elements(grn_ctx *ctx, grn_ii *ii);

GRN_API void grn_ii_cursor_set_min_enable_set(grn_bool enable);
GRN_API grn_bool grn_ii_cursor_set_min_enable_get(void);

GRN_API uint32_t grn_ii_estimate_size(grn_ctx *ctx, grn_ii *ii, grn_id tid);
GRN_API uint32_t grn_ii_estimate_size_for_query(grn_ctx *ctx, grn_ii *ii,
                                                const char *query,
                                                unsigned int query_len,
                                                grn_search_optarg *optarg);
GRN_API uint32_t grn_ii_estimate_size_for_lexicon_cursor(grn_ctx *ctx,
                                                         grn_ii *ii,
                                                         grn_table_cursor *lexicon_cursor);

GRN_API grn_ii_buffer *grn_ii_buffer_open(grn_ctx *ctx, grn_ii *ii,
                                          long long unsigned int update_buffer_size);
GRN_API grn_rc grn_ii_buffer_append(grn_ctx *ctx,
                                    grn_ii_buffer *ii_buffer,
                                    grn_id rid,
                                    unsigned int section,
                                    grn_obj *value);
GRN_API grn_rc grn_ii_buffer_commit(grn_ctx *ctx, grn_ii_buffer *ii_buffer);
GRN_API grn_rc grn_ii_buffer_close(grn_ctx *ctx, grn_ii_buffer *ii_buffer);

GRN_API grn_rc grn_ii_posting_add(grn_ctx *ctx, grn_posting *pos,
                                  grn_hash *s, grn_operator op);
GRN_API void grn_ii_resolve_sel_and(grn_ctx *ctx, grn_hash *s, grn_operator op);


/* Experimental */
typedef struct _grn_ii_cursor grn_ii_cursor;
GRN_API grn_ii_cursor *grn_ii_cursor_open(grn_ctx *ctx, grn_ii *ii, grn_id tid,
                                          grn_id min, grn_id max, int nelements, int flags);
GRN_API grn_posting *grn_ii_cursor_next(grn_ctx *ctx, grn_ii_cursor *c);
GRN_API grn_posting *grn_ii_cursor_next_pos(grn_ctx *ctx, grn_ii_cursor *c);
GRN_API grn_rc grn_ii_cursor_close(grn_ctx *ctx, grn_ii_cursor *c);

#ifdef __cplusplus
}
#endif
