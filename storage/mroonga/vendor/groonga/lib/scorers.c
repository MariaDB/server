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

#include "grn_db.h"

#include <groonga/scorer.h>

#include <math.h>

static double
scorer_tf_idf(grn_ctx *ctx, grn_scorer_matched_record *record)
{
  double min_score = 1.0;
  double tf;
  double n_all_documents;
  double n_candidates;
  double n_tokens;
  double n_estimated_match_documents;

  tf = grn_scorer_matched_record_get_n_occurrences(ctx, record) +
    grn_scorer_matched_record_get_total_term_weights(ctx, record);
  n_all_documents = grn_scorer_matched_record_get_n_documents(ctx, record);
  n_candidates = grn_scorer_matched_record_get_n_candidates(ctx, record);
  n_tokens = grn_scorer_matched_record_get_n_tokens(ctx, record);
  n_estimated_match_documents = n_candidates / n_tokens;

  if (n_estimated_match_documents >= n_all_documents) {
    return min_score;
  } else {
    double idf;
    double tf_idf;

    idf = log(n_all_documents / n_estimated_match_documents);
    tf_idf = tf * idf;
    return fmax(tf_idf, min_score);
  }
}

static double
scorer_tf_at_most(grn_ctx *ctx, grn_scorer_matched_record *record)
{
  double tf;
  double max;
  grn_obj *max_raw;

  tf = grn_scorer_matched_record_get_n_occurrences(ctx, record) +
    grn_scorer_matched_record_get_total_term_weights(ctx, record);
  max_raw = grn_scorer_matched_record_get_arg(ctx, record, 0);

  if (!max_raw) {
    return tf;
  }

  if (max_raw->header.type != GRN_BULK) {
    return tf;
  }

  if (max_raw->header.domain == GRN_DB_FLOAT) {
    max = GRN_FLOAT_VALUE(max_raw);
  } else {
    grn_obj casted_max_raw;
    GRN_FLOAT_INIT(&casted_max_raw, 0);
    if (grn_obj_cast(ctx, max_raw, &casted_max_raw, GRN_FALSE) != GRN_SUCCESS) {
      GRN_OBJ_FIN(ctx, &casted_max_raw);
      return tf;
    } else {
      max = GRN_FLOAT_VALUE(&casted_max_raw);
    }
    GRN_OBJ_FIN(ctx, &casted_max_raw);
  }

  return fmin(tf, max);
}

grn_rc
grn_db_init_builtin_scorers(grn_ctx *ctx)
{
  grn_scorer_register(ctx, "scorer_tf_idf", -1, scorer_tf_idf);
  grn_scorer_register(ctx, "scorer_tf_at_most", -1, scorer_tf_at_most);
  return GRN_SUCCESS;
}
