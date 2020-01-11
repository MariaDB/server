/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/* Copyright(C) 2010-2014 Brazil

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

#ifdef GRN_EMBEDDED
#  define GRN_PLUGIN_FUNCTION_TAG suggest_suggest
#endif

#include <string.h>

#include "grn_ctx.h"
#include "grn_db.h"
#include "grn_ii.h"
#include "grn_token_cursor.h"
#include "grn_output.h"
#include <groonga/plugin.h>

#define VAR GRN_PROC_GET_VAR_BY_OFFSET
#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0
#define TEXT_VALUE_LEN(x) GRN_TEXT_VALUE(x), GRN_TEXT_LEN(x)

#define MIN_LEARN_DISTANCE (60 * GRN_TIME_USEC_PER_SEC)

#define COMPLETE 1
#define CORRECT  2
#define SUGGEST  4

typedef enum {
  GRN_SUGGEST_SEARCH_YES,
  GRN_SUGGEST_SEARCH_NO,
  GRN_SUGGEST_SEARCH_AUTO
} grn_suggest_search_mode;

typedef struct {
  grn_obj *post_event;
  grn_obj *post_type;
  grn_obj *post_item;
  grn_obj *seq;
  grn_obj *post_time;
  grn_obj *pairs;

  int learn_distance_in_seconds;

  grn_id post_event_id;
  grn_id post_type_id;
  grn_id post_item_id;
  grn_id seq_id;
  int64_t post_time_value;

  grn_obj *seqs;
  grn_obj *seqs_events;
  grn_obj *events;
  grn_obj *events_item;
  grn_obj *events_type;
  grn_obj *events_time;
  grn_obj *event_types;
  grn_obj *items;
  grn_obj *items_freq;
  grn_obj *items_freq2;
  grn_obj *items_last;
  grn_obj *pairs_pre;
  grn_obj *pairs_post;
  grn_obj *pairs_freq0;
  grn_obj *pairs_freq1;
  grn_obj *pairs_freq2;

  grn_obj dataset_name;

  grn_obj *configuration;

  grn_obj weight;
  grn_obj pre_events;

  uint64_t key_prefix;
  grn_obj pre_item;
} grn_suggest_learner;

static int
grn_parse_suggest_types(grn_obj *text)
{
  const char *nptr = GRN_TEXT_VALUE(text);
  const char *end = GRN_BULK_CURR(text);
  int types = 0;
  while (nptr < end) {
    if (*nptr == '|') {
      nptr += 1;
      continue;
    }
    {
      const char string[] = "complete";
      size_t length = sizeof(string) - 1;
      if (nptr + length <= end && memcmp(nptr, string, length) == 0) {
        types |= COMPLETE;
        nptr += length;
        continue;
      }
    }
    {
      const char string[] = "correct";
      size_t length = sizeof(string) - 1;
      if (nptr + length <= end && memcmp(nptr, string, length) == 0) {
        types |= CORRECT;
        nptr += length;
        continue;
      }
    }
    {
      const char string[] = "suggest";
      size_t length = sizeof(string) - 1;
      if (nptr + length <= end && memcmp(nptr, string, length) == 0) {
        types |= SUGGEST;
        nptr += length;
        continue;
      }
    }
    break;
  }
  return types;
}

static double
cooccurrence_search(grn_ctx *ctx, grn_obj *items, grn_obj *items_boost, grn_id id,
                    grn_obj *res, int query_type, int frequency_threshold,
                    double conditional_probability_threshold)
{
  double max_score = 0.0;
  if (id) {
    grn_ii_cursor *c;
    grn_obj *co = grn_obj_column(ctx, items, CONST_STR_LEN("co"));
    grn_obj *pairs = grn_ctx_at(ctx, grn_obj_get_range(ctx, co));
    grn_obj *items_freq = grn_obj_column(ctx, items, CONST_STR_LEN("freq"));
    grn_obj *items_freq2 = grn_obj_column(ctx, items, CONST_STR_LEN("freq2"));
    grn_obj *pairs_freq, *pairs_post = grn_obj_column(ctx, pairs, CONST_STR_LEN("post"));
    switch (query_type) {
    case COMPLETE :
      pairs_freq = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq0"));
      break;
    case CORRECT :
      pairs_freq = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq1"));
      break;
    case SUGGEST :
      pairs_freq = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq2"));
      break;
    default :
      return max_score;
    }
    if ((c = grn_ii_cursor_open(ctx, (grn_ii *)co, id, GRN_ID_NIL, GRN_ID_MAX,
                                ((grn_ii *)co)->n_elements - 1, 0))) {
      grn_posting *p;
      grn_obj post, pair_freq, item_freq, item_freq2, item_boost;
      GRN_RECORD_INIT(&post, 0, grn_obj_id(ctx, items));
      GRN_INT32_INIT(&pair_freq, 0);
      GRN_INT32_INIT(&item_freq, 0);
      GRN_INT32_INIT(&item_freq2, 0);
      GRN_INT32_INIT(&item_boost, 0);
      while ((p = grn_ii_cursor_next(ctx, c))) {
        grn_id post_id;
        int pfreq, ifreq, ifreq2, boost;
        double conditional_probability;
        GRN_BULK_REWIND(&post);
        GRN_BULK_REWIND(&pair_freq);
        GRN_BULK_REWIND(&item_freq);
        GRN_BULK_REWIND(&item_freq2);
        GRN_BULK_REWIND(&item_boost);
        grn_obj_get_value(ctx, pairs_post, p->rid, &post);
        grn_obj_get_value(ctx, pairs_freq, p->rid, &pair_freq);
        post_id = GRN_RECORD_VALUE(&post);
        grn_obj_get_value(ctx, items_freq, post_id, &item_freq);
        grn_obj_get_value(ctx, items_freq2, post_id, &item_freq2);
        grn_obj_get_value(ctx, items_boost, post_id, &item_boost);
        pfreq = GRN_INT32_VALUE(&pair_freq);
        ifreq = GRN_INT32_VALUE(&item_freq);
        ifreq2 = GRN_INT32_VALUE(&item_freq2);
        if (ifreq2 > 0) {
          conditional_probability = (double)pfreq / (double)ifreq2;
        } else {
          conditional_probability = 0.0;
        }
        boost = GRN_INT32_VALUE(&item_boost);
        if (pfreq >= frequency_threshold && ifreq >= frequency_threshold &&
            conditional_probability >= conditional_probability_threshold &&
            boost >= 0) {
          grn_rset_recinfo *ri;
          void *value;
          double score = pfreq;
          int added;
          if (max_score < score + boost) { max_score = score + boost; }
          /* put any formula if desired */
          if (grn_hash_add(ctx, (grn_hash *)res,
                           &post_id, sizeof(grn_id), &value, &added)) {
            ri = value;
            ri->score += score;
            if (added) {
              ri->score += boost;
            }
          }
        }
      }
      GRN_OBJ_FIN(ctx, &post);
      GRN_OBJ_FIN(ctx, &pair_freq);
      GRN_OBJ_FIN(ctx, &item_freq);
      GRN_OBJ_FIN(ctx, &item_freq2);
      GRN_OBJ_FIN(ctx, &item_boost);
      grn_ii_cursor_close(ctx, c);
    }
  }
  return max_score;
}

#define DEFAULT_LIMIT           10
#define DEFAULT_SORTBY          "-_score"
#define DEFAULT_OUTPUT_COLUMNS  "_key,_score"
#define DEFAULT_FREQUENCY_THRESHOLD 100
#define DEFAULT_CONDITIONAL_PROBABILITY_THRESHOLD 0.2

static void
output(grn_ctx *ctx, grn_obj *table, grn_obj *res, grn_id tid,
       grn_obj *sortby, grn_obj *output_columns, int offset, int limit)
{
  grn_obj *sorted;
  if ((sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY, NULL, res))) {
    uint32_t nkeys;
    grn_obj_format format;
    grn_table_sort_key *keys;
    const char *sortby_val = GRN_TEXT_VALUE(sortby);
    unsigned int sortby_len = GRN_TEXT_LEN(sortby);
    const char *oc_val = GRN_TEXT_VALUE(output_columns);
    unsigned int oc_len = GRN_TEXT_LEN(output_columns);
    if (!sortby_val || !sortby_len) {
      sortby_val = DEFAULT_SORTBY;
      sortby_len = sizeof(DEFAULT_SORTBY) - 1;
    }
    if (!oc_val || !oc_len) {
      oc_val = DEFAULT_OUTPUT_COLUMNS;
      oc_len = sizeof(DEFAULT_OUTPUT_COLUMNS) - 1;
    }
    if ((keys = grn_table_sort_key_from_str(ctx, sortby_val, sortby_len, res, &nkeys))) {
      grn_table_sort(ctx, res, offset, limit, sorted, keys, nkeys);
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "sort(%d)", limit);
      GRN_OBJ_FORMAT_INIT(&format, grn_table_size(ctx, res), 0, limit, offset);
      format.flags =
        GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
        GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET;
      grn_obj_columns(ctx, sorted, oc_val, oc_len, &format.columns);
      GRN_OUTPUT_OBJ(sorted, &format);
      GRN_OBJ_FORMAT_FIN(ctx, &format);
      grn_table_sort_key_close(ctx, keys, nkeys);
    }
    grn_obj_unlink(ctx, sorted);
  } else {
    ERR(GRN_UNKNOWN_ERROR, "cannot create temporary sort table.");
  }
}

static inline void
complete_add_item(grn_ctx *ctx, grn_id id, grn_obj *res, int frequency_threshold,
                  grn_obj *items_freq, grn_obj *items_boost,
                  grn_obj *item_freq, grn_obj *item_boost)
{
  GRN_BULK_REWIND(item_freq);
  GRN_BULK_REWIND(item_boost);
  grn_obj_get_value(ctx, items_freq, id, item_freq);
  grn_obj_get_value(ctx, items_boost, id, item_boost);
  if (GRN_INT32_VALUE(item_boost) >= 0) {
    double score;
    score = 1 +
            GRN_INT32_VALUE(item_freq) +
            GRN_INT32_VALUE(item_boost);
    if (score >= frequency_threshold) {
      void *value;
      if (grn_hash_add(ctx, (grn_hash *)res, &id, sizeof(grn_id),
                       &value, NULL)) {
        grn_rset_recinfo *ri;
        ri = value;
        ri->score += score;
      }
    }
  }
}

static void
complete(grn_ctx *ctx, grn_obj *items, grn_obj *items_boost, grn_obj *col,
         grn_obj *query, grn_obj *sortby,
         grn_obj *output_columns, int offset, int limit,
         int frequency_threshold, double conditional_probability_threshold,
         grn_suggest_search_mode prefix_search_mode)
{
  grn_obj *res;
  grn_obj *items_freq = grn_obj_column(ctx, items, CONST_STR_LEN("freq"));
  grn_obj item_freq, item_boost;
  GRN_INT32_INIT(&item_freq, 0);
  GRN_INT32_INIT(&item_boost, 0);
  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, items, NULL))) {
    grn_id tid = grn_table_get(ctx, items, TEXT_VALUE_LEN(query));
    if (GRN_TEXT_LEN(query)) {
      grn_table_cursor *cur;
      /* RK search + prefix search */
      grn_obj *index;
      /* FIXME: support index selection */
      if (grn_column_index(ctx, col, GRN_OP_PREFIX, &index, 1, NULL)) {
        if ((cur = grn_table_cursor_open(ctx, grn_ctx_at(ctx, index->header.domain),
                                         GRN_TEXT_VALUE(query),
                                         GRN_TEXT_LEN(query),
                                         NULL, 0, 0, -1,
                                         GRN_CURSOR_PREFIX|GRN_CURSOR_RK))) {
          grn_id id;
          while ((id = grn_table_cursor_next(ctx, cur))) {
            grn_ii_cursor *icur;
            if ((icur = grn_ii_cursor_open(ctx, (grn_ii *)index, id,
                                           GRN_ID_NIL, GRN_ID_MAX, 1, 0))) {
              grn_posting *p;
              while ((p = grn_ii_cursor_next(ctx, icur))) {
                complete_add_item(ctx, p->rid, res, frequency_threshold,
                                  items_freq, items_boost,
                                  &item_freq, &item_boost);
              }
              grn_ii_cursor_close(ctx, icur);
            }
          }
          grn_table_cursor_close(ctx, cur);
        } else {
          ERR(GRN_UNKNOWN_ERROR, "cannot open cursor for prefix RK search.");
        }
      } else {
        ERR(GRN_UNKNOWN_ERROR, "cannot find index for prefix RK search.");
      }
      cooccurrence_search(ctx, items, items_boost, tid, res, COMPLETE,
                          frequency_threshold,
                          conditional_probability_threshold);
      if (((prefix_search_mode == GRN_SUGGEST_SEARCH_YES) ||
           (prefix_search_mode == GRN_SUGGEST_SEARCH_AUTO &&
            !grn_table_size(ctx, res))) &&
          (cur = grn_table_cursor_open(ctx, items,
                                       GRN_TEXT_VALUE(query),
                                       GRN_TEXT_LEN(query),
                                       NULL, 0, 0, -1, GRN_CURSOR_PREFIX))) {
        grn_id id;
        while ((id = grn_table_cursor_next(ctx, cur))) {
          complete_add_item(ctx, id, res, frequency_threshold,
                            items_freq, items_boost, &item_freq, &item_boost);
        }
        grn_table_cursor_close(ctx, cur);
      }
    }
    output(ctx, items, res, tid, sortby, output_columns, offset, limit);
    grn_obj_close(ctx, res);
  } else {
    ERR(GRN_UNKNOWN_ERROR, "cannot create temporary table.");
  }
  GRN_OBJ_FIN(ctx, &item_boost);
  GRN_OBJ_FIN(ctx, &item_freq);
}

static void
correct(grn_ctx *ctx, grn_obj *items, grn_obj *items_boost,
        grn_obj *query, grn_obj *sortby,
        grn_obj *output_columns, int offset, int limit,
        int frequency_threshold, double conditional_probability_threshold,
        grn_suggest_search_mode similar_search_mode)
{
  grn_obj *res;
  grn_obj *items_freq2 = grn_obj_column(ctx, items, CONST_STR_LEN("freq2"));
  grn_obj item_freq2, item_boost;
  GRN_INT32_INIT(&item_freq2, 0);
  GRN_INT32_INIT(&item_boost, 0);
  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, items, NULL))) {
    grn_id tid = grn_table_get(ctx, items, TEXT_VALUE_LEN(query));
    double max_score;
    max_score = cooccurrence_search(ctx, items, items_boost, tid, res, CORRECT,
                                    frequency_threshold,
                                    conditional_probability_threshold);
    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SCORE,
                  ":", "cooccur(%f)", max_score);
    if (GRN_TEXT_LEN(query) &&
        ((similar_search_mode == GRN_SUGGEST_SEARCH_YES) ||
         (similar_search_mode == GRN_SUGGEST_SEARCH_AUTO &&
          max_score < frequency_threshold))) {
      grn_obj *key, *index;
      if ((key = grn_obj_column(ctx, items,
                                GRN_COLUMN_NAME_KEY,
                                GRN_COLUMN_NAME_KEY_LEN))) {
        if (grn_column_index(ctx, key, GRN_OP_MATCH, &index, 1, NULL)) {
          grn_select_optarg optarg;
          memset(&optarg, 0, sizeof(grn_select_optarg));
          optarg.mode = GRN_OP_SIMILAR;
          optarg.similarity_threshold = 0;
          optarg.max_size = 2;
          grn_ii_select(ctx, (grn_ii *)index, TEXT_VALUE_LEN(query),
                        (grn_hash *)res, GRN_OP_OR, &optarg);
          grn_obj_unlink(ctx, index);
          GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                        ":", "similar(%d)", grn_table_size(ctx, res));
          {
            grn_hash_cursor *hc = grn_hash_cursor_open(ctx, (grn_hash *)res, NULL,
                                                       0, NULL, 0, 0, -1, 0);
            if (hc) {
              while (grn_hash_cursor_next(ctx, hc)) {
                void *key, *value;
                if (grn_hash_cursor_get_key_value(ctx, hc, &key, NULL, &value)) {
                  grn_id *rp;
                  rp = key;
                  GRN_BULK_REWIND(&item_freq2);
                  GRN_BULK_REWIND(&item_boost);
                  grn_obj_get_value(ctx, items_freq2, *rp, &item_freq2);
                  grn_obj_get_value(ctx, items_boost, *rp, &item_boost);
                  if (GRN_INT32_VALUE(&item_boost) >= 0) {
                    double score;
                    grn_rset_recinfo *ri;
                    score = 1 +
                            (GRN_INT32_VALUE(&item_freq2) >> 4) +
                            GRN_INT32_VALUE(&item_boost);
                    ri = value;
                    ri->score += score;
                    if (score >= frequency_threshold) { continue; }
                  }
                  /* score < frequency_threshold || item_boost < 0 */
                  grn_hash_cursor_delete(ctx, hc, NULL);
                }
              }
              grn_hash_cursor_close(ctx, hc);
            }
          }
          GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                        ":", "filter(%d)", grn_table_size(ctx, res));
          {
            /* exec _score -= edit_distance(_key, "query string") for all records */
            grn_obj *var;
            grn_obj *expr;

            GRN_EXPR_CREATE_FOR_QUERY(ctx, res, expr, var);
            if (expr) {
              grn_table_cursor *tc;
              grn_obj *score = grn_obj_column(ctx, res,
                                              GRN_COLUMN_NAME_SCORE,
                                              GRN_COLUMN_NAME_SCORE_LEN);
              grn_obj *key = grn_obj_column(ctx, res,
                                            GRN_COLUMN_NAME_KEY,
                                            GRN_COLUMN_NAME_KEY_LEN);
              grn_expr_append_obj(ctx, expr,
                                  score,
                                  GRN_OP_GET_VALUE, 1);
              grn_expr_append_obj(ctx, expr,
                                  grn_ctx_get(ctx, CONST_STR_LEN("edit_distance")),
                                  GRN_OP_PUSH, 1);
              grn_expr_append_obj(ctx, expr,
                                  key,
                                  GRN_OP_GET_VALUE, 1);
              grn_expr_append_const(ctx, expr, query, GRN_OP_PUSH, 1);
              grn_expr_append_op(ctx, expr, GRN_OP_CALL, 2);
              grn_expr_append_op(ctx, expr, GRN_OP_MINUS_ASSIGN, 2);

              if ((tc = grn_table_cursor_open(ctx, res, NULL, 0, NULL, 0, 0, -1, 0))) {
                grn_id id;
                grn_obj score_value;
                GRN_FLOAT_INIT(&score_value, 0);
                while ((id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
                  GRN_RECORD_SET(ctx, var, id);
                  grn_expr_exec(ctx, expr, 0);
                  GRN_BULK_REWIND(&score_value);
                  grn_obj_get_value(ctx, score, id, &score_value);
                  if (GRN_FLOAT_VALUE(&score_value) < frequency_threshold) {
                    grn_table_cursor_delete(ctx, tc);
                  }
                }
                grn_obj_unlink(ctx, &score_value);
                grn_table_cursor_close(ctx, tc);
              }
              grn_obj_unlink(ctx, score);
              grn_obj_unlink(ctx, key);
              grn_obj_unlink(ctx, expr);
            } else {
              ERR(GRN_UNKNOWN_ERROR,
                  "error on building expr. for calicurating edit distance");
            }
          }
        }
        grn_obj_unlink(ctx, key);
      }
    }
    output(ctx, items, res, tid, sortby, output_columns, offset, limit);
    grn_obj_close(ctx, res);
  } else {
    ERR(GRN_UNKNOWN_ERROR, "cannot create temporary table.");
  }
  GRN_OBJ_FIN(ctx, &item_boost);
  GRN_OBJ_FIN(ctx, &item_freq2);
}

static void
suggest(grn_ctx *ctx, grn_obj *items, grn_obj *items_boost,
        grn_obj *query, grn_obj *sortby,
        grn_obj *output_columns, int offset, int limit,
        int frequency_threshold, double conditional_probability_threshold)
{
  grn_obj *res;
  if ((res = grn_table_create(ctx, NULL, 0, NULL,
                              GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, items, NULL))) {
    grn_id tid = grn_table_get(ctx, items, TEXT_VALUE_LEN(query));
    cooccurrence_search(ctx, items, items_boost, tid, res, SUGGEST,
                        frequency_threshold, conditional_probability_threshold);
    output(ctx, items, res, tid, sortby, output_columns, offset, limit);
    grn_obj_close(ctx, res);
  } else {
    ERR(GRN_UNKNOWN_ERROR, "cannot create temporary table.");
  }
}

static grn_suggest_search_mode
parse_search_mode(grn_ctx *ctx, grn_obj *mode_text)
{
  grn_suggest_search_mode mode;
  int mode_length;

  mode_length = GRN_TEXT_LEN(mode_text);
  if (mode_length == 3 &&
      grn_strncasecmp("yes", GRN_TEXT_VALUE(mode_text), 3) == 0) {
    mode = GRN_SUGGEST_SEARCH_YES;
  } else if (mode_length == 2 &&
             grn_strncasecmp("no", GRN_TEXT_VALUE(mode_text), 2) == 0) {
    mode = GRN_SUGGEST_SEARCH_NO;
  } else {
    mode = GRN_SUGGEST_SEARCH_AUTO;
  }

  return mode;
}

static grn_obj *
command_suggest(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *items, *col, *items_boost;
  int types;
  int offset = 0;
  int limit = DEFAULT_LIMIT;
  int frequency_threshold = DEFAULT_FREQUENCY_THRESHOLD;
  double conditional_probability_threshold =
    DEFAULT_CONDITIONAL_PROBABILITY_THRESHOLD;
  grn_suggest_search_mode prefix_search_mode;
  grn_suggest_search_mode similar_search_mode;

  types = grn_parse_suggest_types(VAR(0));
  if (GRN_TEXT_LEN(VAR(6)) > 0) {
    offset = grn_atoi(GRN_TEXT_VALUE(VAR(6)), GRN_BULK_CURR(VAR(6)), NULL);
  }
  if (GRN_TEXT_LEN(VAR(7)) > 0) {
    limit = grn_atoi(GRN_TEXT_VALUE(VAR(7)), GRN_BULK_CURR(VAR(7)), NULL);
  }
  if (GRN_TEXT_LEN(VAR(8)) > 0) {
    frequency_threshold = grn_atoi(GRN_TEXT_VALUE(VAR(8)), GRN_BULK_CURR(VAR(8)), NULL);
  }
  if (GRN_TEXT_LEN(VAR(9)) > 0) {
    GRN_TEXT_PUTC(ctx, VAR(9), '\0');
    conditional_probability_threshold = strtod(GRN_TEXT_VALUE(VAR(9)), NULL);
  }

  prefix_search_mode = parse_search_mode(ctx, VAR(10));
  similar_search_mode = parse_search_mode(ctx, VAR(11));

  if ((items = grn_ctx_get(ctx, TEXT_VALUE_LEN(VAR(1))))) {
    if ((items_boost = grn_obj_column(ctx, items, CONST_STR_LEN("boost")))) {
      int n_outputs = 0;
      if (types & COMPLETE) {
        n_outputs++;
      }
      if (types & CORRECT) {
        n_outputs++;
      }
      if (types & SUGGEST) {
        n_outputs++;
      }
      GRN_OUTPUT_MAP_OPEN("RESULT_SET", n_outputs);

      if (types & COMPLETE) {
        if ((col = grn_obj_column(ctx, items, TEXT_VALUE_LEN(VAR(2))))) {
          GRN_OUTPUT_CSTR("complete");
          complete(ctx, items, items_boost, col, VAR(3), VAR(4),
                   VAR(5), offset, limit,
                   frequency_threshold, conditional_probability_threshold,
                   prefix_search_mode);
        } else {
          ERR(GRN_INVALID_ARGUMENT, "invalid column.");
        }
      }
      if (types & CORRECT) {
        GRN_OUTPUT_CSTR("correct");
        correct(ctx, items, items_boost, VAR(3), VAR(4),
                VAR(5), offset, limit,
                frequency_threshold, conditional_probability_threshold,
                similar_search_mode);
      }
      if (types & SUGGEST) {
        GRN_OUTPUT_CSTR("suggest");
        suggest(ctx, items, items_boost, VAR(3), VAR(4),
                VAR(5), offset, limit,
                frequency_threshold, conditional_probability_threshold);
      }
      GRN_OUTPUT_MAP_CLOSE();
    } else {
      ERR(GRN_INVALID_ARGUMENT, "nonexistent column: <%.*s.boost>",
          (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    }
    grn_obj_unlink(ctx, items);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "nonexistent table: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
  }
  return NULL;
}

static void
learner_init_values(grn_ctx *ctx, grn_suggest_learner *learner)
{
  learner->post_event_id = GRN_RECORD_VALUE(learner->post_event);
  learner->post_type_id = GRN_RECORD_VALUE(learner->post_type);
  learner->post_item_id = GRN_RECORD_VALUE(learner->post_item);
  learner->seq_id = GRN_RECORD_VALUE(learner->seq);
  learner->post_time_value = GRN_TIME_VALUE(learner->post_time);
}

static void
learner_init(grn_ctx *ctx, grn_suggest_learner *learner,
             grn_obj *post_event, grn_obj *post_type, grn_obj *post_item,
             grn_obj *seq, grn_obj *post_time, grn_obj *pairs)
{
  learner->post_event = post_event;
  learner->post_type = post_type;
  learner->post_item = post_item;
  learner->seq = seq;
  learner->post_time = post_time;
  learner->pairs = pairs;

  learner->learn_distance_in_seconds = 0;

  learner_init_values(ctx, learner);
}

static void
learner_init_columns(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_id events_id, event_types_id;
  grn_obj *seqs, *events, *post_item, *items, *pairs;

  learner->seqs = seqs = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(learner->seq));
  learner->seqs_events = grn_obj_column(ctx, seqs, CONST_STR_LEN("events"));

  events_id = grn_obj_get_range(ctx, learner->seqs_events);
  learner->events = events = grn_ctx_at(ctx, events_id);
  learner->events_item = grn_obj_column(ctx, events, CONST_STR_LEN("item"));
  learner->events_type = grn_obj_column(ctx, events, CONST_STR_LEN("type"));
  learner->events_time = grn_obj_column(ctx, events, CONST_STR_LEN("time"));

  event_types_id = grn_obj_get_range(ctx, learner->events_type);
  learner->event_types = grn_obj_column(ctx, events, CONST_STR_LEN("time"));

  post_item = learner->post_item;
  learner->items = items = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(post_item));
  learner->items_freq = grn_obj_column(ctx, items, CONST_STR_LEN("freq"));
  learner->items_freq2 = grn_obj_column(ctx, items, CONST_STR_LEN("freq2"));
  learner->items_last = grn_obj_column(ctx, items, CONST_STR_LEN("last"));

  pairs = learner->pairs;
  learner->pairs_pre = grn_obj_column(ctx, pairs, CONST_STR_LEN("pre"));
  learner->pairs_post = grn_obj_column(ctx, pairs, CONST_STR_LEN("post"));
  learner->pairs_freq0 = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq0"));
  learner->pairs_freq1 = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq1"));
  learner->pairs_freq2 = grn_obj_column(ctx, pairs, CONST_STR_LEN("freq2"));
}

static void
learner_fin_columns(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_obj_unlink(ctx, learner->seqs);
  grn_obj_unlink(ctx, learner->seqs_events);

  grn_obj_unlink(ctx, learner->events);
  grn_obj_unlink(ctx, learner->events_item);
  grn_obj_unlink(ctx, learner->events_type);
  grn_obj_unlink(ctx, learner->events_time);

  grn_obj_unlink(ctx, learner->event_types);

  grn_obj_unlink(ctx, learner->items);
  grn_obj_unlink(ctx, learner->items_freq);
  grn_obj_unlink(ctx, learner->items_freq2);
  grn_obj_unlink(ctx, learner->items_last);

  grn_obj_unlink(ctx, learner->pairs_pre);
  grn_obj_unlink(ctx, learner->pairs_post);
  grn_obj_unlink(ctx, learner->pairs_freq0);
  grn_obj_unlink(ctx, learner->pairs_freq1);
  grn_obj_unlink(ctx, learner->pairs_freq2);
}

static void
learner_init_weight(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_obj *weight_column = NULL;
  unsigned int weight = 1;

  if (learner->configuration) {
    weight_column = grn_obj_column(ctx,
                                   learner->configuration,
                                   CONST_STR_LEN("weight"));
  }
  if (weight_column) {
    grn_id id;
    id = grn_table_get(ctx, learner->configuration,
                       GRN_TEXT_VALUE(&(learner->dataset_name)),
                       GRN_TEXT_LEN(&(learner->dataset_name)));
    if (id != GRN_ID_NIL) {
      grn_obj weight_value;
      GRN_UINT32_INIT(&weight_value, 0);
      grn_obj_get_value(ctx, weight_column, id, &weight_value);
      weight = GRN_UINT32_VALUE(&weight_value);
      GRN_OBJ_FIN(ctx, &weight_value);
    }
    grn_obj_unlink(ctx, weight_column);
  }

  GRN_UINT32_INIT(&(learner->weight), 0);
  GRN_UINT32_SET(ctx, &(learner->weight), weight);
}

static void
learner_init_dataset_name(grn_ctx *ctx, grn_suggest_learner *learner)
{
  char events_name[GRN_TABLE_MAX_KEY_SIZE];
  unsigned int events_name_size;
  unsigned int events_name_prefix_size;

  events_name_size = grn_obj_name(ctx, learner->events,
                                  events_name, GRN_TABLE_MAX_KEY_SIZE);
  GRN_TEXT_INIT(&(learner->dataset_name), 0);
  events_name_prefix_size = strlen("event_");
  if (events_name_size > events_name_prefix_size) {
    GRN_TEXT_PUT(ctx,
                 &(learner->dataset_name),
                 events_name + events_name_prefix_size,
                 events_name_size - events_name_prefix_size);
  }
}

static void
learner_fin_dataset_name(grn_ctx *ctx, grn_suggest_learner *learner)
{
  GRN_OBJ_FIN(ctx, &(learner->dataset_name));
}

static void
learner_init_configuration(grn_ctx *ctx, grn_suggest_learner *learner)
{
  learner->configuration = grn_ctx_get(ctx, "configuration", -1);
}

static void
learner_fin_configuration(grn_ctx *ctx, grn_suggest_learner *learner)
{
  if (learner->configuration) {
    grn_obj_unlink(ctx, learner->configuration);
  }
}

static void
learner_init_buffers(grn_ctx *ctx, grn_suggest_learner *learner)
{
  learner_init_weight(ctx, learner);
  GRN_RECORD_INIT(&(learner->pre_events), 0, grn_obj_id(ctx, learner->events));
}

static void
learner_fin_buffers(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_obj_unlink(ctx, &(learner->weight));
  grn_obj_unlink(ctx, &(learner->pre_events));
}

static void
learner_init_submit_learn(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_id items_id;

  learner->key_prefix = ((uint64_t)learner->post_item_id) << 32;

  items_id = grn_obj_get_range(ctx, learner->events_item);
  GRN_RECORD_INIT(&(learner->pre_item), 0, items_id);

  grn_obj_get_value(ctx, learner->seqs_events, learner->seq_id,
                    &(learner->pre_events));
}

static void
learner_fin_submit_learn(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_obj_unlink(ctx, &(learner->pre_item));
  GRN_BULK_REWIND(&(learner->pre_events));
}

static grn_bool
learner_is_valid_input(grn_ctx *ctx, grn_suggest_learner *learner)
{
  return learner->post_event_id && learner->post_item_id && learner->seq_id;
}

static void
learner_increment(grn_ctx *ctx, grn_suggest_learner *learner,
                  grn_obj *column, grn_id record_id)
{
  grn_obj_set_value(ctx, column, record_id, &(learner->weight), GRN_OBJ_INCR);
}

static void
learner_increment_item_freq(grn_ctx *ctx, grn_suggest_learner *learner,
                            grn_obj *column)
{
  learner_increment(ctx, learner, column, learner->post_item_id);
}

static void
learner_set_last_post_time(grn_ctx *ctx, grn_suggest_learner *learner)
{
  grn_obj_set_value(ctx, learner->items_last, learner->post_item_id,
                    learner->post_time, GRN_OBJ_SET);
}

static void
learner_learn_for_complete_and_correcnt(grn_ctx *ctx,
                                        grn_suggest_learner *learner)
{
  grn_obj *pre_item, *post_item, *pre_events;
  grn_obj pre_type, pre_time;
  grn_id *ep, *es;
  uint64_t key;
  int64_t post_time_value;

  pre_item = &(learner->pre_item);
  post_item = learner->post_item;
  pre_events = &(learner->pre_events);
  post_time_value = learner->post_time_value;
  GRN_RECORD_INIT(&pre_type, 0, grn_obj_get_range(ctx, learner->events_type));
  GRN_TIME_INIT(&pre_time, 0);
  ep = (grn_id *)GRN_BULK_CURR(pre_events);
  es = (grn_id *)GRN_BULK_HEAD(pre_events);
  while (es < ep--) {
    grn_id pair_id;
    int added;
    int64_t learn_distance;

    GRN_BULK_REWIND(&pre_type);
    GRN_BULK_REWIND(&pre_time);
    GRN_BULK_REWIND(pre_item);
    grn_obj_get_value(ctx, learner->events_type, *ep, &pre_type);
    grn_obj_get_value(ctx, learner->events_time, *ep, &pre_time);
    grn_obj_get_value(ctx, learner->events_item, *ep, pre_item);
    learn_distance = post_time_value - GRN_TIME_VALUE(&pre_time);
    if (learn_distance >= MIN_LEARN_DISTANCE) {
      learner->learn_distance_in_seconds =
        (int)(learn_distance / GRN_TIME_USEC_PER_SEC);
      break;
    }
    key = learner->key_prefix + GRN_RECORD_VALUE(pre_item);
    pair_id = grn_table_add(ctx, learner->pairs, &key, sizeof(uint64_t),
                            &added);
    if (added) {
      grn_obj_set_value(ctx, learner->pairs_pre, pair_id, pre_item,
                        GRN_OBJ_SET);
      grn_obj_set_value(ctx, learner->pairs_post, pair_id, post_item,
                        GRN_OBJ_SET);
    }
    if (GRN_RECORD_VALUE(&pre_type)) {
      learner_increment(ctx, learner, learner->pairs_freq1, pair_id);
      break;
    } else {
      learner_increment(ctx, learner, learner->pairs_freq0, pair_id);
    }
  }
  GRN_OBJ_FIN(ctx, &pre_type);
  GRN_OBJ_FIN(ctx, &pre_time);
}

static void
learner_learn_for_suggest(grn_ctx *ctx, grn_suggest_learner *learner)
{
  char keybuf[GRN_TABLE_MAX_KEY_SIZE];
  int keylen = grn_table_get_key(ctx, learner->items, learner->post_item_id,
                                 keybuf, GRN_TABLE_MAX_KEY_SIZE);
  unsigned int token_flags = 0;
  grn_token_cursor *token_cursor =
    grn_token_cursor_open(ctx, learner->items, keybuf, keylen,
                          GRN_TOKEN_ADD, token_flags);
  if (token_cursor) {
    grn_id tid;
    grn_obj *pre_item = &(learner->pre_item);
    grn_obj *post_item = learner->post_item;
    grn_hash *token_ids = NULL;
    while ((tid = grn_token_cursor_next(ctx, token_cursor)) && tid != learner->post_item_id) {
      uint64_t key;
      int added;
      grn_id pair_id;
      key = learner->key_prefix + tid;
      pair_id = grn_table_add(ctx, learner->pairs, &key, sizeof(uint64_t),
                              &added);
      if (added) {
        GRN_RECORD_SET(ctx, pre_item, tid);
        grn_obj_set_value(ctx, learner->pairs_pre, pair_id,
                          pre_item, GRN_OBJ_SET);
        grn_obj_set_value(ctx, learner->pairs_post, pair_id,
                          post_item, GRN_OBJ_SET);
      }
      if (!token_ids) {
        token_ids = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                    GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
      }
      if (token_ids) {
        int token_added;
        grn_hash_add(ctx, token_ids, &tid, sizeof(grn_id), NULL, &token_added);
        if (token_added) {
          learner_increment(ctx, learner, learner->pairs_freq2, pair_id);
        }
      }
    }
    if (token_ids) {
      grn_hash_close(ctx, token_ids);
    }
    grn_token_cursor_close(ctx, token_cursor);
  }
}

static void
learner_append_post_event(grn_ctx *ctx, grn_suggest_learner *learner)
{
  GRN_RECORD_SET(ctx, &(learner->pre_events), learner->post_event_id);
  grn_obj_set_value(ctx, learner->seqs_events, learner->seq_id,
                    &(learner->pre_events), GRN_OBJ_APPEND);
}

static void
learner_learn(grn_ctx *ctx, grn_suggest_learner *learner)
{
  if (learner_is_valid_input(ctx, learner)) {
    learner_init_columns(ctx, learner);
    learner_init_dataset_name(ctx, learner);
    learner_init_configuration(ctx, learner);
    learner_init_buffers(ctx, learner);
    learner_increment_item_freq(ctx, learner, learner->items_freq);
    learner_set_last_post_time(ctx, learner);
    if (learner->post_type_id) {
      learner_init_submit_learn(ctx, learner);
      learner_increment_item_freq(ctx, learner, learner->items_freq2);
      learner_learn_for_complete_and_correcnt(ctx, learner);
      learner_learn_for_suggest(ctx, learner);
      learner_fin_submit_learn(ctx, learner);
    }
    learner_append_post_event(ctx, learner);
    learner_fin_buffers(ctx, learner);
    learner_fin_configuration(ctx, learner);
    learner_fin_dataset_name(ctx, learner);
    learner_fin_columns(ctx, learner);
  }
}

static grn_obj *
func_suggest_preparer(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  int learn_distance_in_seconds = 0;
  grn_obj *obj;
  if (nargs == 6) {
    grn_obj *post_event = args[0];
    grn_obj *post_type = args[1];
    grn_obj *post_item = args[2];
    grn_obj *seq = args[3];
    grn_obj *post_time = args[4];
    grn_obj *pairs = args[5];
    grn_suggest_learner learner;
    learner_init(ctx, &learner,
                 post_event, post_type, post_item, seq, post_time, pairs);
    learner_learn(ctx, &learner);
    learn_distance_in_seconds = learner.learn_distance_in_seconds;
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, learn_distance_in_seconds);
  }
  return obj;
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_expr_var vars[12];

  grn_plugin_expr_var_init(ctx, &vars[0], "types", -1);
  grn_plugin_expr_var_init(ctx, &vars[1], "table", -1);
  grn_plugin_expr_var_init(ctx, &vars[2], "column", -1);
  grn_plugin_expr_var_init(ctx, &vars[3], "query", -1);
  grn_plugin_expr_var_init(ctx, &vars[4], "sortby", -1);
  grn_plugin_expr_var_init(ctx, &vars[5], "output_columns", -1);
  grn_plugin_expr_var_init(ctx, &vars[6], "offset", -1);
  grn_plugin_expr_var_init(ctx, &vars[7], "limit", -1);
  grn_plugin_expr_var_init(ctx, &vars[8], "frequency_threshold", -1);
  grn_plugin_expr_var_init(ctx, &vars[9], "conditional_probability_threshold", -1);
  grn_plugin_expr_var_init(ctx, &vars[10], "prefix_search", -1);
  grn_plugin_expr_var_init(ctx, &vars[11], "similar_search", -1);
  grn_plugin_command_create(ctx, "suggest", -1, command_suggest, 12, vars);

  grn_proc_create(ctx, CONST_STR_LEN("suggest_preparer"), GRN_PROC_FUNCTION,
                  func_suggest_preparer, NULL, NULL, 0, NULL);
  return ctx->rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
