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

#include <string.h>

#include "grn.h"
#include "grn_db.h"
#include "grn_scorer.h"
#include <groonga/scorer.h>

grn_obj *
grn_scorer_matched_record_get_table(grn_ctx *ctx,
                                    grn_scorer_matched_record *record)
{
  return record->table;
}

grn_obj *
grn_scorer_matched_record_get_lexicon(grn_ctx *ctx,
                                      grn_scorer_matched_record *record)
{
  return record->lexicon;
}

grn_id
grn_scorer_matched_record_get_id(grn_ctx *ctx,
                                 grn_scorer_matched_record *record)
{
  return record->id;
}

grn_obj *
grn_scorer_matched_record_get_terms(grn_ctx *ctx,
                                    grn_scorer_matched_record *record)
{
  return &(record->terms);
}

grn_obj *
grn_scorer_matched_record_get_term_weights(grn_ctx *ctx,
                                           grn_scorer_matched_record *record)
{
  return &(record->term_weights);
}

unsigned int
grn_scorer_matched_record_get_total_term_weights(grn_ctx *ctx,
                                                 grn_scorer_matched_record *record)
{
  return record->total_term_weights;
}

long long unsigned int
grn_scorer_matched_record_get_n_documents(grn_ctx *ctx,
                                          grn_scorer_matched_record *record)
{
  return record->n_documents;
}

unsigned int
grn_scorer_matched_record_get_n_occurrences(grn_ctx *ctx,
                                            grn_scorer_matched_record *record)
{
  return record->n_occurrences;
}

long long unsigned int
grn_scorer_matched_record_get_n_candidates(grn_ctx *ctx,
                                           grn_scorer_matched_record *record)
{
  return record->n_candidates;
}

unsigned int
grn_scorer_matched_record_get_n_tokens(grn_ctx *ctx,
                                       grn_scorer_matched_record *record)
{
  return record->n_tokens;
}

int
grn_scorer_matched_record_get_weight(grn_ctx *ctx,
                                     grn_scorer_matched_record *record)
{
  return record->weight;
}

grn_obj *
grn_scorer_matched_record_get_arg(grn_ctx *ctx,
                                  grn_scorer_matched_record *record,
                                  unsigned int i)
{
  grn_expr *expr;
  grn_expr_code *codes_original;
  uint32_t codes_curr_original;
  grn_obj *arg;

  if (!record->args_expr) {
    return NULL;
  }

  expr = (grn_expr *)(record->args_expr);
  /* TODO: support getting column value */
  codes_original = expr->codes;
  codes_curr_original = expr->codes_curr;
  expr->codes += record->args_expr_offset;
  expr->codes_curr = 1; /* TODO: support 1 or more codes */
  arg = grn_expr_exec(ctx, (grn_obj *)expr, 0);
  expr->codes_curr = codes_curr_original;
  expr->codes = codes_original;

  return arg;
}

unsigned int
grn_scorer_matched_record_get_n_args(grn_ctx *ctx,
                                     grn_scorer_matched_record *record)
{
  grn_expr *expr;
  grn_expr_code *codes;
  unsigned int n_args = 0;

  if (!record->args_expr) {
    return 0;
  }

  expr = (grn_expr *)(record->args_expr);
  codes = expr->codes + record->args_expr_offset;
  if (codes[0].op == GRN_OP_CALL) {
    return 0;
  }

  n_args++;
  for (; codes[0].op != GRN_OP_CALL; codes++) {
    if (codes[0].op == GRN_OP_COMMA) {
      n_args++;
    }
  }

  return n_args;
}

grn_rc
grn_scorer_register(grn_ctx *ctx,
                    const char *scorer_name_ptr,
                    int scorer_name_length,
                    grn_scorer_score_func *score)
{
  if (scorer_name_length == -1) {
    scorer_name_length = strlen(scorer_name_ptr);
  }

  {
    grn_obj *scorer_object = grn_proc_create(ctx,
                                             scorer_name_ptr,
                                             scorer_name_length,
                                             GRN_PROC_SCORER,
                                             NULL, NULL, NULL, 0, NULL);
    if (scorer_object == NULL) {
      GRN_PLUGIN_ERROR(ctx, GRN_SCORER_ERROR,
                       "[scorer][%.*s] failed to grn_proc_create()",
                       scorer_name_length, scorer_name_ptr);
      return ctx->rc;
    }

    {
      grn_proc *scorer = (grn_proc *)scorer_object;
      scorer->callbacks.scorer.score = score;
    }
  }

  return GRN_SUCCESS;
}

