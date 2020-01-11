/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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
#include "grn_str.h"
#include "grn_db.h"

#define ASIZE                   256U
#define MAX_SNIP_TAG_COUNT      512U
#define MAX_SNIP_COND_COUNT     32U
#define MAX_SNIP_RESULT_COUNT   16U

#ifdef __cplusplus
extern "C"
{
#endif

#define SNIPCOND_NONSTOP 0
#define SNIPCOND_STOP    1
#define SNIPCOND_ACROSS  2

#define GRN_QUERY_SCAN_ALLOCCONDS 0x0002

typedef struct _snip_cond
{
  /* initial parameters */
  const char *opentag;
  const char *closetag;
  size_t opentag_len;
  size_t closetag_len;
  grn_obj *keyword;

  /* Tuned BM pre */
  size_t bmBc[ASIZE];
  size_t shift;

  /* Tuned BM temporal result */
  size_t found;
  size_t last_found;
  size_t last_offset;
  size_t start_offset;
  size_t end_offset;
  size_t found_alpha_head;

  /* search result */
  int count;

  /* stop flag */
  int_least8_t stopflag;
} snip_cond;

typedef struct
{
  size_t start_offset;
  size_t end_offset;
  snip_cond *cond;
} _snip_tag_result;

typedef struct
{
  size_t start_offset;
  size_t end_offset;
  unsigned int first_tag_result_idx;
  unsigned int last_tag_result_idx;
  unsigned int tag_count;
} _snip_result;

typedef struct _grn_snip
{
  grn_db_obj obj;
  grn_encoding encoding;
  int flags;
  size_t width;
  unsigned int max_results;
  const char *defaultopentag;
  const char *defaultclosetag;
  size_t defaultopentag_len;
  size_t defaultclosetag_len;

  grn_snip_mapping *mapping;

  snip_cond cond[MAX_SNIP_COND_COUNT];
  unsigned int cond_len;

  unsigned int tag_count;
  unsigned int snip_count;

  const char *string;
  grn_obj *nstr;

  _snip_result snip_result[MAX_SNIP_RESULT_COUNT];
  _snip_tag_result tag_result[MAX_SNIP_TAG_COUNT];

  size_t max_tagged_len;

  grn_obj *normalizer;
} grn_snip;

grn_rc grn_snip_close(grn_ctx *ctx, grn_snip *snip);
grn_rc grn_snip_cond_init(grn_ctx *ctx, snip_cond *sc, const char *keyword, unsigned int keyword_len,
                          grn_encoding enc, grn_obj *normalizer, int flags);
void grn_snip_cond_reinit(snip_cond *cond);
grn_rc grn_snip_cond_close(grn_ctx *ctx, snip_cond *cond);
void grn_bm_tunedbm(grn_ctx *ctx, snip_cond *cond, grn_obj *string, int flags);

#ifdef __cplusplus
}
#endif
