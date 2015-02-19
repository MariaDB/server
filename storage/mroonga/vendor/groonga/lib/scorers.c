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

grn_rc
grn_db_init_builtin_scorers(grn_ctx *ctx)
{
  grn_scorer_register(ctx, "scorer_tf_idf", -1, scorer_tf_idf);
  return GRN_SUCCESS;
}
