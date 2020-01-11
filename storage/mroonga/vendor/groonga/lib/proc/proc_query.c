/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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

#include "../grn_proc.h"

#include <groonga/plugin.h>

static grn_obj *
command_query_expand(grn_ctx *ctx, int nargs, grn_obj **args,
                     grn_user_data *user_data)
{
  const char *expander;
  size_t expander_size;
  const char *query;
  size_t query_size;
  const char *flags_raw;
  size_t flags_raw_size;
  grn_expr_flags flags = GRN_EXPR_SYNTAX_QUERY;
  const char *term_column;
  size_t term_column_size;
  const char *expanded_term_column;
  size_t expanded_term_column_size;
  grn_obj expanded_query;

  expander = grn_plugin_proc_get_var_string(ctx,
                                            user_data,
                                            "expander",
                                            -1,
                                            &expander_size);
  query = grn_plugin_proc_get_var_string(ctx,
                                         user_data,
                                         "query",
                                         -1,
                                         &query_size);
  flags_raw = grn_plugin_proc_get_var_string(ctx,
                                             user_data,
                                             "flags",
                                             -1,
                                             &flags_raw_size);
  term_column = grn_plugin_proc_get_var_string(ctx,
                                            user_data,
                                            "term_column",
                                            -1,
                                            &term_column_size);
  expanded_term_column =
    grn_plugin_proc_get_var_string(ctx,
                                   user_data,
                                   "expanded_term_column",
                                   -1,
                                   &expanded_term_column_size);

  if (flags_raw_size > 0) {
    flags |= grn_proc_expr_query_flags_parse(ctx,
                                             flags_raw,
                                             flags_raw_size,
                                             "[query][expand]");
  } else {
    flags |= GRN_EXPR_ALLOW_PRAGMA | GRN_EXPR_ALLOW_COLUMN;
  }

  if (ctx->rc != GRN_SUCCESS) {
    return NULL;
  }

  GRN_TEXT_INIT(&expanded_query, 0);
  grn_proc_syntax_expand_query(ctx,
                               query,
                               query_size,
                               flags,
                               expander,
                               expander_size,
                               term_column,
                               term_column_size,
                               expanded_term_column,
                               expanded_term_column_size,
                               &expanded_query,
                               "[query][expand]");
  if (ctx->rc == GRN_SUCCESS) {
    grn_ctx_output_str(ctx,
                       GRN_TEXT_VALUE(&expanded_query),
                       GRN_TEXT_LEN(&expanded_query));
  }
  GRN_OBJ_FIN(ctx, &expanded_query);

  return NULL;
}

void
grn_proc_init_query_expand(grn_ctx *ctx)
{
  grn_expr_var vars[5];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "expander", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "query", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "flags", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "term_column", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "expanded_term_column", -1);
  grn_plugin_command_create(ctx,
                            "query_expand", -1,
                            command_query_expand,
                            5,
                            vars);
}
