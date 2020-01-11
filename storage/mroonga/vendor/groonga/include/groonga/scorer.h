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

#include <groonga/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

typedef struct _grn_scorer_matched_record grn_scorer_matched_record;

GRN_API grn_obj *
  grn_scorer_matched_record_get_table(grn_ctx *ctx,
                                      grn_scorer_matched_record *record);
GRN_API grn_obj *
  grn_scorer_matched_record_get_lexicon(grn_ctx *ctx,
                                        grn_scorer_matched_record *record);
GRN_API grn_id
  grn_scorer_matched_record_get_id(grn_ctx *ctx,
                                   grn_scorer_matched_record *record);
GRN_API grn_obj *
  grn_scorer_matched_record_get_terms(grn_ctx *ctx,
                                      grn_scorer_matched_record *record);
GRN_API grn_obj *
  grn_scorer_matched_record_get_term_weights(grn_ctx *ctx,
                                             grn_scorer_matched_record *record);
GRN_API unsigned int
  grn_scorer_matched_record_get_total_term_weights(grn_ctx *ctx,
                                                   grn_scorer_matched_record *record);
GRN_API long long unsigned int
  grn_scorer_matched_record_get_n_documents(grn_ctx *ctx,
                                            grn_scorer_matched_record *record);
GRN_API unsigned int
  grn_scorer_matched_record_get_n_occurrences(grn_ctx *ctx,
                                              grn_scorer_matched_record *record);
GRN_API long long unsigned int
  grn_scorer_matched_record_get_n_candidates(grn_ctx *ctx,
                                             grn_scorer_matched_record *record);
GRN_API unsigned int
  grn_scorer_matched_record_get_n_tokens(grn_ctx *ctx,
                                         grn_scorer_matched_record *record);
GRN_API int
  grn_scorer_matched_record_get_weight(grn_ctx *ctx,
                                       grn_scorer_matched_record *record);
GRN_API grn_obj *
  grn_scorer_matched_record_get_arg(grn_ctx *ctx,
                                    grn_scorer_matched_record *record,
                                    unsigned int i);
GRN_API unsigned int
  grn_scorer_matched_record_get_n_args(grn_ctx *ctx,
                                       grn_scorer_matched_record *record);



typedef double grn_scorer_score_func(grn_ctx *ctx,
                                     grn_scorer_matched_record *record);

/*
  grn_scorer_register() registers a plugin to the database which is
  associated with `ctx'. `scorer_name_ptr' and `scorer_name_length' specify the
  plugin name. Alphabetic letters ('A'-'Z' and 'a'-'z'), digits ('0'-'9') and
  an underscore ('_') are capable characters.

  `score' is called for scoring matched records one by one.

  grn_scorer_register() returns GRN_SUCCESS on success, an error
  code on failure.
 */
GRN_PLUGIN_EXPORT grn_rc grn_scorer_register(grn_ctx *ctx,
                                             const char *scorer_name_ptr,
                                             int scorer_name_length,
                                             grn_scorer_score_func *score);

#ifdef __cplusplus
}  /* extern "C" */
#endif  /* __cplusplus */
