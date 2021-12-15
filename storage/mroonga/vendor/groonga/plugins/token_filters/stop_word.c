/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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
#  define GRN_PLUGIN_FUNCTION_TAG token_filters_stop_word
#endif

#include <grn_str.h>

#include <groonga.h>
#include <groonga/token_filter.h>

#include <string.h>

#define COLUMN_NAME "is_stop_word"

typedef struct {
  grn_obj *table;
  grn_token_mode mode;
  grn_obj *column;
  grn_obj value;
  grn_tokenizer_token token;
} grn_stop_word_token_filter;

static void *
stop_word_init(grn_ctx *ctx, grn_obj *table, grn_token_mode mode)
{
  grn_stop_word_token_filter *token_filter;

  if (mode != GRN_TOKEN_GET) {
    return NULL;
  }

  token_filter = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_stop_word_token_filter));
  if (!token_filter) {
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[token-filter][stop-word] "
                     "failed to allocate grn_stop_word_token_filter");
    return NULL;
  }

  token_filter->table = table;
  token_filter->mode = mode;
  token_filter->column = grn_obj_column(ctx,
                                        token_filter->table,
                                        COLUMN_NAME,
                                        strlen(COLUMN_NAME));
  if (!token_filter->column) {
    char table_name[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int table_name_size;

    table_name_size = grn_obj_name(ctx,
                                   token_filter->table,
                                   table_name,
                                   GRN_TABLE_MAX_KEY_SIZE);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKEN_FILTER_ERROR,
                     "[token-filter][stop-word] "
                     "column for judging stop word doesn't exit: <%.*s.%s>",
                     table_name_size,
                     table_name,
                     COLUMN_NAME);
    GRN_PLUGIN_FREE(ctx, token_filter);
    return NULL;
  }

  GRN_BOOL_INIT(&(token_filter->value), 0);
  grn_tokenizer_token_init(ctx, &(token_filter->token));

  return token_filter;
}

static void
stop_word_filter(grn_ctx *ctx,
                 grn_token *current_token,
                 grn_token *next_token,
                 void *user_data)
{
  grn_stop_word_token_filter *token_filter = user_data;
  grn_id id;
  grn_obj *data;

  if (!token_filter) {
    return;
  }

  data = grn_token_get_data(ctx, current_token);
  id = grn_table_get(ctx,
                     token_filter->table,
                     GRN_TEXT_VALUE(data),
                     GRN_TEXT_LEN(data));
  if (id != GRN_ID_NIL) {
    GRN_BULK_REWIND(&(token_filter->value));
    grn_obj_get_value(ctx,
                      token_filter->column,
                      id,
                      &(token_filter->value));
    if (GRN_BOOL_VALUE(&(token_filter->value))) {
      grn_tokenizer_status status;
      status = grn_token_get_status(ctx, current_token);
      status |= GRN_TOKEN_SKIP;
      grn_token_set_status(ctx, next_token, status);
    }
  }
}

static void
stop_word_fin(grn_ctx *ctx, void *user_data)
{
  grn_stop_word_token_filter *token_filter = user_data;
  if (!token_filter) {
    return;
  }

  grn_tokenizer_token_fin(ctx, &(token_filter->token));
  grn_obj_unlink(ctx, token_filter->column);
  grn_obj_unlink(ctx, &(token_filter->value));
  GRN_PLUGIN_FREE(ctx, token_filter);
}

grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  return ctx->rc;
}

grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_rc rc;

  rc = grn_token_filter_register(ctx,
                                 "TokenFilterStopWord", -1,
                                 stop_word_init,
                                 stop_word_filter,
                                 stop_word_fin);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
