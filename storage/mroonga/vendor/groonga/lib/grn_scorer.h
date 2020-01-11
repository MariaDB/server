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

#include "grn_ctx.h"
#include "grn_db.h"

#include <groonga/scorer.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _grn_scorer_matched_record {
  grn_obj *table;
  grn_obj *lexicon;
  grn_id id;
  grn_obj terms;
  grn_obj term_weights;
  uint32_t total_term_weights;
  uint64_t n_documents;
  uint32_t n_occurrences;
  uint64_t n_candidates;
  uint32_t n_tokens;
  int weight;
  grn_obj *args_expr;
  unsigned int args_expr_offset;
};


#ifdef __cplusplus
}
#endif
