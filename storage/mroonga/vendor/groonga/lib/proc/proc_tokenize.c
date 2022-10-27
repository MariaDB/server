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

#include "../grn_proc.h"
#include "../grn_ctx.h"
#include "../grn_token_cursor.h"

#include <groonga/plugin.h>

static unsigned int
parse_tokenize_flags(grn_ctx *ctx, grn_obj *flag_names)
{
  unsigned int flags = 0;
  const char *names, *names_end;
  int length;

  names = GRN_TEXT_VALUE(flag_names);
  length = GRN_TEXT_LEN(flag_names);
  names_end = names + length;
  while (names < names_end) {
    if (*names == '|' || *names == ' ') {
      names += 1;
      continue;
    }

#define CHECK_FLAG(name)\
    if (((names_end - names) >= (sizeof(#name) - 1)) &&\
        (!memcmp(names, #name, sizeof(#name) - 1))) {\
      flags |= GRN_TOKEN_CURSOR_ ## name;\
      names += sizeof(#name) - 1;\
      continue;\
    }

    CHECK_FLAG(ENABLE_TOKENIZED_DELIMITER);

#define GRN_TOKEN_CURSOR_NONE 0
    CHECK_FLAG(NONE);
#undef GRN_TOKEN_CURSOR_NONE

    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[tokenize] invalid flag: <%.*s>",
                     (int)(names_end - names), names);
    return 0;
#undef CHECK_FLAG
  }

  return flags;
}

typedef struct {
  grn_id id;
  int32_t position;
  grn_bool force_prefix;
} tokenize_token;

static void
output_tokens(grn_ctx *ctx, grn_obj *tokens, grn_obj *lexicon, grn_obj *index_column)
{
  int i, n_tokens, n_elements;
  grn_obj estimated_size;

  n_tokens = GRN_BULK_VSIZE(tokens) / sizeof(tokenize_token);
  n_elements = 3;
  if (index_column) {
    n_elements++;
    GRN_UINT32_INIT(&estimated_size, 0);
  }

  grn_ctx_output_array_open(ctx, "TOKENS", n_tokens);
  for (i = 0; i < n_tokens; i++) {
    tokenize_token *token;
    char value[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int value_size;

    token = ((tokenize_token *)(GRN_BULK_HEAD(tokens))) + i;

    grn_ctx_output_map_open(ctx, "TOKEN", n_elements);

    grn_ctx_output_cstr(ctx, "value");
    value_size = grn_table_get_key(ctx, lexicon, token->id,
                                   value, GRN_TABLE_MAX_KEY_SIZE);
    grn_ctx_output_str(ctx, value, value_size);

    grn_ctx_output_cstr(ctx, "position");
    grn_ctx_output_int32(ctx, token->position);

    grn_ctx_output_cstr(ctx, "force_prefix");
    grn_ctx_output_bool(ctx, token->force_prefix);

    if (index_column) {
      GRN_BULK_REWIND(&estimated_size);
      grn_obj_get_value(ctx, index_column, token->id, &estimated_size);
      grn_ctx_output_cstr(ctx, "estimated_size");
      grn_ctx_output_int64(ctx, GRN_UINT32_VALUE(&estimated_size));
    }

    grn_ctx_output_map_close(ctx);
  }

  if (index_column) {
    GRN_OBJ_FIN(ctx, &estimated_size);
  }

  grn_ctx_output_array_close(ctx);
}

static grn_obj *
create_lexicon_for_tokenize(grn_ctx *ctx,
                            grn_obj *tokenizer_name,
                            grn_obj *normalizer_name,
                            grn_obj *token_filter_names)
{
  grn_obj *lexicon;
  grn_obj *tokenizer;
  grn_obj *normalizer = NULL;

  tokenizer = grn_ctx_get(ctx,
                          GRN_TEXT_VALUE(tokenizer_name),
                          GRN_TEXT_LEN(tokenizer_name));
  if (!tokenizer) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[tokenize] nonexistent tokenizer: <%.*s>",
                     (int)GRN_TEXT_LEN(tokenizer_name),
                     GRN_TEXT_VALUE(tokenizer_name));
    return NULL;
  }

  if (!grn_obj_is_tokenizer_proc(ctx, tokenizer)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, tokenizer);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "[tokenize] not tokenizer: %.*s",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    grn_obj_unlink(ctx, tokenizer);
    return NULL;
  }

  if (GRN_TEXT_LEN(normalizer_name) > 0) {
    normalizer = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(normalizer_name),
                             GRN_TEXT_LEN(normalizer_name));
    if (!normalizer) {
      grn_obj_unlink(ctx, tokenizer);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[tokenize] nonexistent normalizer: <%.*s>",
                       (int)GRN_TEXT_LEN(normalizer_name),
                       GRN_TEXT_VALUE(normalizer_name));
      return NULL;
    }

    if (!grn_obj_is_normalizer_proc(ctx, normalizer)) {
      grn_obj inspected;
      grn_obj_unlink(ctx, tokenizer);
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, normalizer);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[tokenize] not normalizer: %.*s",
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      grn_obj_unlink(ctx, normalizer);
      return NULL;
    }
  }

  lexicon = grn_table_create(ctx, NULL, 0,
                             NULL,
                             GRN_OBJ_TABLE_HASH_KEY,
                             grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                             NULL);
  grn_obj_set_info(ctx, lexicon,
                   GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
  grn_obj_unlink(ctx, tokenizer);
  if (normalizer) {
    grn_obj_set_info(ctx, lexicon,
                     GRN_INFO_NORMALIZER, normalizer);
    grn_obj_unlink(ctx, normalizer);
  }
  grn_proc_table_set_token_filters(ctx, lexicon, token_filter_names);

  return lexicon;
}

static void
tokenize(grn_ctx *ctx, grn_obj *lexicon, grn_obj *string, grn_tokenize_mode mode,
         unsigned int flags, grn_obj *tokens)
{
  grn_token_cursor *token_cursor;

  token_cursor =
    grn_token_cursor_open(ctx, lexicon,
                          GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                          mode, flags);
  if (!token_cursor) {
    return;
  }

  while (token_cursor->status == GRN_TOKEN_CURSOR_DOING) {
    grn_id token_id = grn_token_cursor_next(ctx, token_cursor);
    tokenize_token *current_token;
    if (token_id == GRN_ID_NIL) {
      continue;
    }
    grn_bulk_space(ctx, tokens, sizeof(tokenize_token));
    current_token = ((tokenize_token *)(GRN_BULK_CURR(tokens))) - 1;
    current_token->id = token_id;
    current_token->position = token_cursor->pos;
    current_token->force_prefix = token_cursor->force_prefix;
  }
  grn_token_cursor_close(ctx, token_cursor);
}

static grn_obj *
command_table_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table_name;
  grn_obj *string;
  grn_obj *flag_names;
  grn_obj *mode_name;
  grn_obj *index_column_name;

  table_name = grn_plugin_proc_get_var(ctx, user_data, "table", -1);
  string = grn_plugin_proc_get_var(ctx, user_data, "string", -1);
  flag_names = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  mode_name = grn_plugin_proc_get_var(ctx, user_data, "mode", -1);
  index_column_name = grn_plugin_proc_get_var(ctx, user_data, "index_column", -1);

  if (GRN_TEXT_LEN(table_name) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[table_tokenize] table name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[table_tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;
    grn_obj *index_column = NULL;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));
    if (!lexicon) {
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[table_tokenize] nonexistent lexicon: <%.*s>",
                       (int)GRN_TEXT_LEN(table_name),
                       GRN_TEXT_VALUE(table_name));
      return NULL;
    }

#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    if (GRN_TEXT_LEN(index_column_name) > 0) {
      index_column = grn_obj_column(ctx, lexicon,
                                    GRN_TEXT_VALUE(index_column_name),
                                    GRN_TEXT_LEN(index_column_name));
      if (!index_column) {
        GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                         "[table_tokenize] nonexistent index column: <%.*s>",
                         (int)GRN_TEXT_LEN(index_column_name),
                         GRN_TEXT_VALUE(index_column_name));
        goto exit;
      }
      if (index_column->header.type != GRN_COLUMN_INDEX) {
        GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                         "[table_tokenize] index column must be COLUMN_INDEX: <%.*s>",
                         (int)GRN_TEXT_LEN(index_column_name),
                         GRN_TEXT_VALUE(index_column_name));
        goto exit;
      }
    }

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
    if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("GET")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon, index_column);
    } else if (MODE_NAME_EQUAL("ADD")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon, index_column);
    } else {
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[table_tokenize] invalid mode: <%.*s>",
                       (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
    }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

exit:
    grn_obj_unlink(ctx, lexicon);
    if (index_column) {
      grn_obj_unlink(ctx, index_column);
    }
  }

  return NULL;
}

void
grn_proc_init_table_tokenize(grn_ctx *ctx)
{
  grn_expr_var vars[5];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "table", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "string", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "flags", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "mode", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "index_column", -1);
  grn_plugin_command_create(ctx,
                            "table_tokenize", -1,
                            command_table_tokenize,
                            5,
                            vars);
}

static grn_obj *
command_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *tokenizer_name;
  grn_obj *string;
  grn_obj *normalizer_name;
  grn_obj *flag_names;
  grn_obj *mode_name;
  grn_obj *token_filter_names;

  tokenizer_name = grn_plugin_proc_get_var(ctx, user_data, "tokenizer", -1);
  string = grn_plugin_proc_get_var(ctx, user_data, "string", -1);
  normalizer_name = grn_plugin_proc_get_var(ctx, user_data, "normalizer", -1);
  flag_names = grn_plugin_proc_get_var(ctx, user_data, "flags", -1);
  mode_name = grn_plugin_proc_get_var(ctx, user_data, "mode", -1);
  token_filter_names = grn_plugin_proc_get_var(ctx, user_data, "token_filters", -1);

  if (GRN_TEXT_LEN(tokenizer_name) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[tokenize] tokenizer name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT, "[tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = create_lexicon_for_tokenize(ctx,
                                          tokenizer_name,
                                          normalizer_name,
                                          token_filter_names);
    if (!lexicon) {
      return NULL;
    }
#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
      if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("ADD")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon, NULL);
      } else if (MODE_NAME_EQUAL("GET")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        GRN_BULK_REWIND(&tokens);
        tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon, NULL);
      } else {
        GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                         "[tokenize] invalid mode: <%.*s>",
                         (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
      }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

    grn_obj_unlink(ctx, lexicon);
  }

  return NULL;
}

void
grn_proc_init_tokenize(grn_ctx *ctx)
{
  grn_expr_var vars[6];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "tokenizer", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "string", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "normalizer", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "flags", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "mode", -1);
  grn_plugin_expr_var_init(ctx, &(vars[5]), "token_filters", -1);
  grn_plugin_command_create(ctx,
                            "tokenize", -1,
                            command_tokenize,
                            6,
                            vars);
}
