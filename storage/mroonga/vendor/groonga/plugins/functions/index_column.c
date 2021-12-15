/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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
#  define GRN_PLUGIN_FUNCTION_TAG functions_time
#endif

#include <groonga/plugin.h>

static grn_rc
selector_index_column_df_ratio_between(grn_ctx *ctx,
                                       grn_obj *table,
                                       grn_obj *index,
                                       int n_args,
                                       grn_obj **args,
                                       grn_obj *res,
                                       grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *index_column;
  grn_ii *ii;
  double min;
  double max;
  grn_obj *source_table;
  unsigned int n_documents;
  grn_posting posting;

  if ((n_args - 1) != 3) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "index_column_df_ratio_between(): "
                     "wrong number of arguments (%d for 3)", n_args - 1);
    rc = ctx->rc;
    goto exit;
  }

  index_column = args[1];
  ii = (grn_ii *)index_column;
  min = GRN_FLOAT_VALUE(args[2]);
  max = GRN_FLOAT_VALUE(args[3]);

  source_table = grn_ctx_at(ctx, grn_obj_get_range(ctx, index_column));
  n_documents = grn_table_size(ctx, source_table);
  memset(&posting, 0, sizeof(grn_posting));
  posting.sid = 1;

  if (op == GRN_OP_AND) {
    GRN_TABLE_EACH_BEGIN(ctx, res, cursor, record_id) {
      void *key;
      grn_id term_id;
      uint32_t n_match_documents;
      double df_ratio;

      grn_table_cursor_get_key(ctx, cursor, &key);
      term_id = *(grn_id *)key;
      n_match_documents = grn_ii_estimate_size(ctx, ii, term_id);
      if (n_match_documents > n_documents) {
        n_match_documents = n_documents;
      }
      df_ratio = (double)n_match_documents / (double)n_documents;
      if (min <= df_ratio && df_ratio <= max) {
        posting.rid = term_id;
        grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
      }
    } GRN_TABLE_EACH_END(ctx, cursor);
    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  } else {
    GRN_TABLE_EACH_BEGIN(ctx, table, cursor, term_id) {
      uint32_t n_match_documents;
      double df_ratio;

      n_match_documents = grn_ii_estimate_size(ctx, ii, term_id);
      if (n_match_documents > n_documents) {
        n_match_documents = n_documents;
      }
      df_ratio = (double)n_match_documents / (double)n_documents;
      {
        void *key;
        int key_size;
        key_size = grn_table_cursor_get_key(ctx, cursor, &key);
      }
      if (min <= df_ratio && df_ratio <= max) {
        posting.rid = term_id;
        grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
      }
    } GRN_TABLE_EACH_END(ctx, cursor);
  }

exit :
  return rc;
}

static grn_obj *
func_index_column_df_ratio(grn_ctx *ctx,
                           int n_args,
                           grn_obj **args,
                           grn_user_data *user_data)
{
  grn_obj *term_table;
  grn_obj *index_column_name;
  grn_obj *index_column;
  grn_ii *ii;
  grn_id term_id;

  if (n_args != 1) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "index_column_df_ratio(): "
                     "wrong number of arguments (%d for 1)", n_args - 1);
    return NULL;
  }

  {
    grn_obj *expr;
    grn_obj *variable;

    expr = grn_plugin_proc_get_caller(ctx, user_data);
    if (!expr) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "index_column_df_ratio(): "
                       "called directly");
      return NULL;
    }

    variable = grn_expr_get_var_by_offset(ctx, expr, 0);
    if (!variable) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "index_column_df_ratio(): "
                       "caller expression must have target record information");
      return NULL;
    }

    term_table = grn_ctx_at(ctx, variable->header.domain);
    term_id = GRN_RECORD_VALUE(variable);
    while (GRN_TRUE) {
      grn_obj *key_type;

      key_type = grn_ctx_at(ctx, term_table->header.domain);
      if (!grn_obj_is_table(ctx, key_type)) {
        break;
      }

      grn_table_get_key(ctx, term_table, term_id, &term_id, sizeof(grn_id));
      term_table = key_type;
    }
  }

  index_column_name = args[0];
  if (!grn_obj_is_text_family_bulk(ctx, index_column_name)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, index_column_name);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "index_column_df_ratio(): "
                     "the first argument must be index column name: %.*s",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return NULL;
  }

  index_column = grn_obj_column(ctx,
                                term_table,
                                GRN_TEXT_VALUE(index_column_name),
                                GRN_TEXT_LEN(index_column_name));
  if (!index_column) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "index_column_df_ratio(): "
                     "nonexistent object: <%.*s>",
                     (int)GRN_TEXT_LEN(index_column_name),
                     GRN_TEXT_VALUE(index_column_name));
    return NULL;
  }

  if (!grn_obj_is_index_column(ctx, index_column)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, index_column);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "index_column_df_ratio(): "
                     "the first argument must be index column: %.*s",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    if (grn_obj_is_accessor(ctx, index_column)) {
      grn_obj_unlink(ctx, index_column);
    }
    return NULL;
  }

  ii = (grn_ii *)index_column;

  {
    grn_obj *source_table;
    unsigned int n_documents;
    uint32_t n_match_documents;
    double df_ratio;
    grn_obj *df_ratio_value;

    source_table = grn_ctx_at(ctx, grn_obj_get_range(ctx, index_column));
    n_documents = grn_table_size(ctx, source_table);
    n_match_documents = grn_ii_estimate_size(ctx, ii, term_id);
    if (n_match_documents > n_documents) {
      n_match_documents = n_documents;
    }
    df_ratio = (double)n_match_documents / (double)n_documents;

    df_ratio_value = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_FLOAT, 0);
    if (!df_ratio_value) {
      return NULL;
    }
    GRN_FLOAT_SET(ctx, df_ratio_value, df_ratio);
    return df_ratio_value;
  }
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return ctx->rc;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_obj *selector_proc;

  selector_proc = grn_proc_create(ctx, "index_column_df_ratio_between", -1,
                                  GRN_PROC_FUNCTION,
                                  NULL, NULL, NULL, 0, NULL);
  grn_proc_set_selector(ctx, selector_proc,
                        selector_index_column_df_ratio_between);
  grn_proc_set_selector_operator(ctx, selector_proc, GRN_OP_NOP);

  grn_proc_create(ctx, "index_column_df_ratio", -1,
                  GRN_PROC_FUNCTION,
                  func_index_column_df_ratio, NULL, NULL, 0, NULL);

  return ctx->rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
