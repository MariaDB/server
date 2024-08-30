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

#include "ts_expr_parser.h"

#include <stdlib.h>
#include <string.h>

#include "../grn_ctx.h"

#include "ts_log.h"
#include "ts_str.h"
#include "ts_util.h"

/*-------------------------------------------------------------
 * grn_ts_expr_token.
 */

#define GRN_TS_EXPR_TOKEN_INIT(TYPE)\
  memset(token, 0, sizeof(*token));\
  token->type = GRN_TS_EXPR_ ## TYPE ## _TOKEN;\
  token->src = src;
/* grn_ts_expr_dummy_token_init() initializes a token. */
static void
grn_ts_expr_dummy_token_init(grn_ctx *ctx, grn_ts_expr_dummy_token *token,
                             grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(DUMMY)
}

/* grn_ts_expr_start_token_init() initializes a token. */
static void
grn_ts_expr_start_token_init(grn_ctx *ctx, grn_ts_expr_start_token *token,
                             grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(START)
}

/* grn_ts_expr_end_token_init() initializes a token. */
static void
grn_ts_expr_end_token_init(grn_ctx *ctx, grn_ts_expr_end_token *token,
                           grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(END)
}

/* grn_ts_expr_const_token_init() initializes a token. */
static void
grn_ts_expr_const_token_init(grn_ctx *ctx, grn_ts_expr_const_token *token,
                             grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(CONST);
  grn_ts_buf_init(ctx, &token->buf);
}

/* grn_ts_expr_name_token_init() initializes a token. */
static void
grn_ts_expr_name_token_init(grn_ctx *ctx, grn_ts_expr_name_token *token,
                            grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(NAME);
}

/* grn_ts_expr_op_token_init() initializes a token. */
static void
grn_ts_expr_op_token_init(grn_ctx *ctx, grn_ts_expr_op_token *token,
                          grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(OP);
}

/* grn_ts_expr_bridge_token_init() initializes a token. */
static void
grn_ts_expr_bridge_token_init(grn_ctx *ctx, grn_ts_expr_bridge_token *token,
                              grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(BRIDGE)
}

/* grn_ts_expr_bracket_token_init() initializes a token. */
static void
grn_ts_expr_bracket_token_init(grn_ctx *ctx, grn_ts_expr_bracket_token *token,
                               grn_ts_str src)
{
  GRN_TS_EXPR_TOKEN_INIT(BRACKET)
}
#undef GRN_TS_EXPR_TOKEN_INIT

/* grn_ts_expr_dummy_token_fin() finalizes a token. */
static void
grn_ts_expr_dummy_token_fin(grn_ctx *ctx, grn_ts_expr_dummy_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_start_token_fin() finalizes a token. */
static void
grn_ts_expr_start_token_fin(grn_ctx *ctx, grn_ts_expr_start_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_end_token_fin() finalizes a token. */
static void
grn_ts_expr_end_token_fin(grn_ctx *ctx, grn_ts_expr_end_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_const_token_fin() finalizes a token. */
static void
grn_ts_expr_const_token_fin(grn_ctx *ctx, grn_ts_expr_const_token *token)
{
  grn_ts_buf_fin(ctx, &token->buf);
}

/* grn_ts_expr_name_token_fin() finalizes a token. */
static void
grn_ts_expr_name_token_fin(grn_ctx *ctx, grn_ts_expr_name_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_op_token_fin() finalizes a token. */
static void
grn_ts_expr_op_token_fin(grn_ctx *ctx, grn_ts_expr_op_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_bridge_token_fin() finalizes a token. */
static void
grn_ts_expr_bridge_token_fin(grn_ctx *ctx, grn_ts_expr_bridge_token *token)
{
  /* Nothing to do. */
}

/* grn_ts_expr_bracket_token_fin() finalizes a token. */
static void
grn_ts_expr_bracket_token_fin(grn_ctx *ctx, grn_ts_expr_bracket_token *token)
{
  /* Nothing to do. */
}

#define GRN_TS_EXPR_TOKEN_OPEN(TYPE, type)\
  grn_ts_expr_ ## type ## _token *new_token;\
  new_token = GRN_MALLOCN(grn_ts_expr_ ## type ## _token, 1);\
  if (!new_token) {\
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,\
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",\
                      sizeof(grn_ts_expr_ ## type ## _token));\
  }\
  grn_ts_expr_ ## type ## _token_init(ctx, new_token, src);\
  *token = new_token;
/* grn_ts_expr_dummy_token_open() creates a token. */
/*
static grn_rc
grn_ts_expr_dummy_token_open(grn_ctx *ctx, grn_ts_str src,
                             grn_ts_expr_dummy_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(DUMMY, dummy)
  return GRN_SUCCESS;
}
*/

/* grn_ts_expr_start_token_open() creates a token. */
static grn_rc
grn_ts_expr_start_token_open(grn_ctx *ctx, grn_ts_str src,
                             grn_ts_expr_start_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(START, start)
  return GRN_SUCCESS;
}

/* grn_ts_expr_end_token_open() creates a token. */
static grn_rc
grn_ts_expr_end_token_open(grn_ctx *ctx, grn_ts_str src,
                           grn_ts_expr_end_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(END, end)
  return GRN_SUCCESS;
}

/* grn_ts_expr_const_token_open() creates a token. */
static grn_rc
grn_ts_expr_const_token_open(grn_ctx *ctx, grn_ts_str src,
                             grn_ts_expr_const_token **token)
                             {
  GRN_TS_EXPR_TOKEN_OPEN(CONST, const)
  return GRN_SUCCESS;
}

/* grn_ts_expr_name_token_open() creates a token. */
static grn_rc
grn_ts_expr_name_token_open(grn_ctx *ctx, grn_ts_str src,
                            grn_ts_expr_name_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(NAME, name)
  return GRN_SUCCESS;
}

/* grn_ts_expr_op_token_open() creates a token. */
static grn_rc
grn_ts_expr_op_token_open(grn_ctx *ctx, grn_ts_str src, grn_ts_op_type op_type,
                          grn_ts_expr_op_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(OP, op)
  new_token->op_type = op_type;
  return GRN_SUCCESS;
}

/* grn_ts_expr_bridge_token_open() creates a token. */
static grn_rc
grn_ts_expr_bridge_token_open(grn_ctx *ctx, grn_ts_str src,
                              grn_ts_expr_bridge_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(BRIDGE, bridge)
  return GRN_SUCCESS;
}

/* grn_ts_expr_bracket_token_open() creates a token. */
static grn_rc
grn_ts_expr_bracket_token_open(grn_ctx *ctx, grn_ts_str src,
                               grn_ts_expr_bracket_token **token)
{
  GRN_TS_EXPR_TOKEN_OPEN(BRACKET, bracket)
  return GRN_SUCCESS;
}
#undef GRN_TS_EXPR_TOKEN_OPEN

#define GRN_TS_EXPR_TOKEN_CLOSE_CASE(TYPE, type)\
  case GRN_TS_EXPR_ ## TYPE ## _TOKEN: {\
    grn_ts_expr_ ## type ## _token *type ## _token;\
    type ## _token = (grn_ts_expr_ ## type ## _token *)token;\
    grn_ts_expr_ ## type ## _token_fin(ctx, type ## _token);\
    break;\
  }
/* grn_ts_expr_token_close() destroys a token. */
static void
grn_ts_expr_token_close(grn_ctx *ctx, grn_ts_expr_token *token)
{
  switch (token->type) {
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(DUMMY, dummy)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(START, start)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(END, end)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(CONST, const)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(NAME, name)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(OP, op)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(BRACKET, bracket)
    GRN_TS_EXPR_TOKEN_CLOSE_CASE(BRIDGE, bridge)
  }
  GRN_FREE(token);
}
#undef GRN_TS_EXPR_TOKEN_CLOSE_CASE

/*-------------------------------------------------------------
 * grn_ts_expr_parser.
 */

/* grn_ts_expr_parser_init() initializes a parser. */
static void
grn_ts_expr_parser_init(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  memset(parser, 0, sizeof(*parser));
  parser->builder = NULL;
  grn_ts_buf_init(ctx, &parser->str_buf);
  parser->tokens = NULL;
  parser->dummy_tokens = NULL;
  parser->stack = NULL;
}

/* grn_ts_expr_parser_fin() finalizes a parser. */
static void
grn_ts_expr_parser_fin(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  if (parser->stack) {
    GRN_FREE(parser->stack);
  }
  if (parser->dummy_tokens) {
    size_t i;
    for (i = 0; i < parser->n_dummy_tokens; i++) {
      grn_ts_expr_dummy_token_fin(ctx, &parser->dummy_tokens[i]);
    }
    GRN_FREE(parser->dummy_tokens);
  }
  if (parser->tokens) {
    size_t i;
    for (i = 0; i < parser->n_tokens; i++) {
      grn_ts_expr_token_close(ctx, parser->tokens[i]);
    }
    GRN_FREE(parser->tokens);
  }
  grn_ts_buf_fin(ctx, &parser->str_buf);
  if (parser->builder) {
    grn_ts_expr_builder_close(ctx, parser->builder);
  }
}

grn_rc
grn_ts_expr_parser_open(grn_ctx *ctx, grn_obj *table,
                        grn_ts_expr_parser **parser)
{
  grn_rc rc;
  grn_ts_expr_parser *new_parser;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !parser) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  new_parser = GRN_MALLOCN(grn_ts_expr_parser, 1);
  if (!new_parser) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_parser));
  }
  grn_ts_expr_parser_init(ctx, new_parser);
  rc = grn_ts_expr_builder_open(ctx, table, &new_parser->builder);
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_parser_fin(ctx, new_parser);
    GRN_FREE(new_parser);
    return  rc;
  }
  *parser = new_parser;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_parser_close(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!parser) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_expr_parser_fin(ctx, parser);
  GRN_FREE(parser);
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_start() creates the start token. */
static grn_rc
grn_ts_expr_parser_tokenize_start(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                  grn_ts_str str, grn_ts_expr_token **token)
{
  grn_ts_str token_str = { str.ptr, 0 };
  grn_ts_expr_start_token *new_token= 0;
  grn_rc rc = grn_ts_expr_start_token_open(ctx, token_str, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_end() creates the end token. */
static grn_rc
grn_ts_expr_parser_tokenize_end(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                grn_ts_str str, grn_ts_expr_token **token)
{
  grn_ts_str token_str = { str.ptr, 0 };
  grn_ts_expr_end_token *new_token= 0;
  grn_rc rc = grn_ts_expr_end_token_open(ctx, token_str, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_number() tokenizes an Int or Float literal. */
static grn_rc
grn_ts_expr_parser_tokenize_number(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                   grn_ts_str str, grn_ts_expr_token **token)
{
  char *end;
  grn_rc rc;
  grn_ts_int int_value;
  grn_ts_str token_str;
  grn_ts_expr_const_token *new_token= 0;

  int_value = strtol(str.ptr, &end, 0);
  if ((end != str.ptr) && (*end != '.') && (*end != 'e')) {
    if (grn_ts_byte_is_name_char(*end)) {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT,
                        "unterminated Int literal: \"%.*s\"",
                        (int)str.size, str.ptr);
    }
    token_str.ptr = str.ptr;
    token_str.size = end - str.ptr;
    rc = grn_ts_expr_const_token_open(ctx, token_str, &new_token);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    new_token->data_kind = GRN_TS_INT;
    new_token->content.as_int = int_value;
  } else {
    grn_ts_float float_value = strtod(str.ptr, &end);
    if (end == str.ptr) {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid number literal: \"%.*s\"",
                        (int)str.size, str.ptr);
    }
    if (grn_ts_byte_is_name_char(*end)) {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT,
                        "unterminated Float literal: \"%.*s\"",
                        (int)str.size, str.ptr);
    }
    token_str.ptr = str.ptr;
    token_str.size = end - str.ptr;
    rc = grn_ts_expr_const_token_open(ctx, token_str, &new_token);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    new_token->data_kind = GRN_TS_FLOAT;
    new_token->content.as_float = float_value;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_text() tokenizes a Text literal. */
static grn_rc
grn_ts_expr_parser_tokenize_text(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                 grn_ts_str str, grn_ts_expr_token **token)
{
  size_t i, n_escapes = 0;
  grn_rc rc;
  grn_ts_str token_str;
  grn_ts_expr_const_token *new_token= 0;
  for (i = 1; i < str.size; i++) {
    if (str.ptr[i] == '\\') {
      i++;
      n_escapes++;
    } else if (str.ptr[i] == '"') {
      break;
    }
  }
  if (i >= str.size) {
    GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "no closing double quote: \"%.*s\"",
                      (int)str.size, str.ptr);
  }
  token_str.ptr = str.ptr;
  token_str.size = i + 1;
  rc = grn_ts_expr_const_token_open(ctx, token_str, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  new_token->data_kind = GRN_TS_TEXT;
  if (n_escapes) {
    char *buf_ptr;
    const char *str_ptr = str.ptr + 1;
    size_t size = token_str.size - 2 - n_escapes;
    rc = grn_ts_buf_resize(ctx, &new_token->buf, size);
    if (rc != GRN_SUCCESS) {
      grn_ts_expr_token_close(ctx, (grn_ts_expr_token *)new_token);
      return rc;
    }
    buf_ptr = (char *)new_token->buf.ptr;
    for (i = 0; i < size; i++) {
      if (str_ptr[i] == '\\') {
        str_ptr++;
      }
      buf_ptr[i] = str_ptr[i];
    }
    new_token->content.as_text.ptr = buf_ptr;
    new_token->content.as_text.size = size;
  } else {
    new_token->content.as_text.ptr = token_str.ptr + 1;
    new_token->content.as_text.size = token_str.size - 2;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_name() tokenizes a Bool literal or a name. */
static grn_rc
grn_ts_expr_parser_tokenize_name(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                 grn_ts_str str, grn_ts_expr_token **token)
{
  size_t i;
  grn_ts_str token_str;
  for (i = 1; i < str.size; i++) {
    if (!grn_ts_byte_is_name_char(str.ptr[i])) {
      break;
    }
  }
  token_str.ptr = str.ptr;
  token_str.size = i;

  if (grn_ts_str_is_bool(token_str)) {
    grn_ts_expr_const_token *new_token= 0;
    grn_rc rc = grn_ts_expr_const_token_open(ctx, token_str, &new_token);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    new_token->data_kind = GRN_TS_BOOL;
    if (token_str.ptr[0] == 't') {
      new_token->content.as_bool = GRN_TRUE;
    } else {
      new_token->content.as_bool = GRN_FALSE;
    }
    *token = (grn_ts_expr_token *)new_token;
    return GRN_SUCCESS;
  }
  return grn_ts_expr_name_token_open(ctx, token_str, token);
}

/* grn_ts_expr_parser_tokenize_bridge() tokenizes a bridge. */
static grn_rc
grn_ts_expr_parser_tokenize_bridge(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                   grn_ts_str str, grn_ts_expr_token **token)
{
  grn_ts_str token_str = { str.ptr, 1 };
  grn_ts_expr_bridge_token *new_token= 0;
  grn_rc rc = grn_ts_expr_bridge_token_open(ctx, token_str, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_bracket() tokenizes a bracket. */
static grn_rc
grn_ts_expr_parser_tokenize_bracket(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                    grn_ts_str str,
                                    grn_ts_expr_token **token)
{
  grn_ts_str token_str = { str.ptr, 1 };
  grn_ts_expr_bracket_token *new_token= 0;
  grn_rc rc = grn_ts_expr_bracket_token_open(ctx, token_str, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/*
 * grn_ts_expr_parsre_tokenize_sign() tokenizes an operator '+' or '-'.
 * Note that '+' and '-' have two roles each.
 * '+' is GRN_TS_OP_POSITIVE or GRN_TS_OP_PLUS.
 * '-' is GRN_TS_OP_NEGATIVE or GRN_TS_OP_MINUS.
 */
static grn_rc
grn_ts_expr_parser_tokenize_sign(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                 grn_ts_str str, grn_ts_expr_token **token)
{
  size_t n_args;
  grn_rc rc;
  grn_ts_op_type op_type;
  grn_ts_str token_str = { str.ptr, 1 };
  grn_ts_expr_token *prev_token = parser->tokens[parser->n_tokens - 1];
  grn_ts_expr_op_token *new_token= 0;
  switch (prev_token->type) {
    case GRN_TS_EXPR_START_TOKEN:
    case GRN_TS_EXPR_OP_TOKEN: {
      n_args = 1;
      break;
    }
    case GRN_TS_EXPR_CONST_TOKEN:
    case GRN_TS_EXPR_NAME_TOKEN: {
      n_args = 2;
      break;
    }
    case GRN_TS_EXPR_BRACKET_TOKEN: {
      grn_ts_str bracket;
      const grn_ts_expr_bracket_token *bracket_token;
      bracket_token = (const grn_ts_expr_bracket_token *)prev_token;
      bracket = bracket_token->src;
      switch (bracket.ptr[0]) {
        case '(': case '[': {
          n_args = 1;
          break;
        }
        case ')': case ']': {
          n_args = 2;
          break;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "undefined bracket: \"%.*s\"",
                            (int)bracket.size, bracket.ptr);
        }
      }
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence: %d",
                        prev_token->type);
    }
  }
  if (token_str.ptr[0] == '+') {
    op_type = (n_args == 1) ? GRN_TS_OP_POSITIVE : GRN_TS_OP_PLUS;
  } else {
    op_type = (n_args == 1) ? GRN_TS_OP_NEGATIVE : GRN_TS_OP_MINUS;
  }
  rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_op() tokenizes an operator. */
static grn_rc
grn_ts_expr_parser_tokenize_op(grn_ctx *ctx, grn_ts_expr_parser *parser,
                               grn_ts_str str, grn_ts_expr_token **token)
{
  grn_rc rc = GRN_SUCCESS;
  grn_ts_str token_str = str;
  grn_ts_op_type op_type;
  grn_ts_expr_op_token *new_token= 0;
  switch (str.ptr[0]) {
    case '+': case '-': {
      return grn_ts_expr_parser_tokenize_sign(ctx, parser, str, token);
    }
    case '!': {
      if ((str.size >= 2) && (str.ptr[1] == '=')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_NOT_EQUAL;
      } else {
        token_str.size = 1;
        op_type = GRN_TS_OP_LOGICAL_NOT;
      }
      rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);
      break;
    }
#define GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE(label, TYPE_1, TYPE_2, TYPE_3,\
                                            TYPE_EQUAL)\
  case label: {\
    if ((str.size >= 2) && (str.ptr[1] == '=')) {\
      token_str.size = 2;\
      op_type = GRN_TS_OP_ ## TYPE_EQUAL;\
    } else if ((str.size >= 2) && (str.ptr[1] == label)) {\
      if ((str.size >= 3) && (str.ptr[2] == label)) {\
        token_str.size = 3;\
        op_type = GRN_TS_OP_ ## TYPE_3;\
      } else {\
        token_str.size = 2;\
        op_type = GRN_TS_OP_ ## TYPE_2;\
      }\
    } else {\
      token_str.size = 1;\
      op_type = GRN_TS_OP_ ## TYPE_1;\
    }\
    rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);\
    break;\
  }
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('<', LESS, SHIFT_ARITHMETIC_LEFT,
                                        SHIFT_LOGICAL_LEFT, LESS_EQUAL)
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('>', GREATER, SHIFT_ARITHMETIC_RIGHT,
                                        SHIFT_LOGICAL_RIGHT, GREATER_EQUAL)
#undef GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE
    case '&': {
      if ((str.size >= 2) && (str.ptr[1] == '&')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_LOGICAL_AND;
      } else if ((str.size >= 2) && (str.ptr[1] == '&')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_LOGICAL_SUB;
      } else {
        token_str.size = 1;
        op_type = GRN_TS_OP_BITWISE_AND;
      }
      rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);
      break;
    }
    case '|': {
      if ((str.size >= 2) && (str.ptr[1] == '|')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_LOGICAL_OR;
      } else {
        token_str.size = 1;
        op_type = GRN_TS_OP_BITWISE_OR;
      }
      rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);
      break;
    }
    case '=': {
      if ((str.size < 2) || (str.ptr[1] != '=')) {
        GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT,
                          "single equal not available: =\"%.*s\"",
                          (int)str.size, str.ptr);
      }
      token_str.size = 2;
      rc = grn_ts_expr_op_token_open(ctx, token_str, GRN_TS_OP_EQUAL,
                                     &new_token);
      break;
    }
#define GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE(label, TYPE)\
  case label: {\
    token_str.size = 1;\
    rc = grn_ts_expr_op_token_open(ctx, token_str, GRN_TS_OP_ ## TYPE,\
                                   &new_token);\
    break;\
  }
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('~', BITWISE_NOT)
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('^', BITWISE_XOR)
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('*', MULTIPLICATION)
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('/', DIVISION)
    GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE('%', MODULUS)
#undef GRN_TS_EXPR_PARSER_TOKENIZE_OP_CASE
    case '@': {
      if ((str.size >= 2) && (str.ptr[1] == '^')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_PREFIX_MATCH;
      } else if ((str.size >= 2) && (str.ptr[1] == '$')) {
        token_str.size = 2;
        op_type = GRN_TS_OP_SUFFIX_MATCH;
      } else {
        token_str.size = 1;
        op_type = GRN_TS_OP_MATCH;
      }
      rc = grn_ts_expr_op_token_open(ctx, token_str, op_type, &new_token);
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid character: \"%.*s\"",
                        (int)str.size, str.ptr);
    }
  }
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *token = (grn_ts_expr_token *)new_token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize_next() extracts the next token. */
static grn_rc
grn_ts_expr_parser_tokenize_next(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                 grn_ts_str str, grn_ts_expr_token **token)
{
  grn_ts_str rest;
  if (!parser->n_tokens) {
    return grn_ts_expr_parser_tokenize_start(ctx, parser, str, token);
  }
  rest = grn_ts_str_trim_left(str);
  if (!rest.size) {
    return grn_ts_expr_parser_tokenize_end(ctx, parser, rest, token);
  }
  if (grn_ts_str_has_number_prefix(rest)) {
    grn_ts_expr_token *prev_token;
    if ((rest.ptr[0] != '+') && (rest.ptr[0] != '-')) {
      return grn_ts_expr_parser_tokenize_number(ctx, parser, rest, token);
    }
    prev_token = parser->tokens[parser->n_tokens - 1];
    switch (prev_token->type) {
      case GRN_TS_EXPR_START_TOKEN:
      case GRN_TS_EXPR_OP_TOKEN: {
        return grn_ts_expr_parser_tokenize_number(ctx, parser, rest, token);
      }
      case GRN_TS_EXPR_BRACKET_TOKEN: {
        if ((prev_token->src.ptr[0] == '(') ||
            (prev_token->src.ptr[0] == '[')) {
          return grn_ts_expr_parser_tokenize_number(ctx, parser, rest, token);
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  if (rest.ptr[0] == '"') {
    return grn_ts_expr_parser_tokenize_text(ctx, parser, rest, token);
  }
  if (grn_ts_byte_is_name_char(rest.ptr[0])) {
    return grn_ts_expr_parser_tokenize_name(ctx, parser, rest, token);
  }
  switch (rest.ptr[0]) {
    case '(': case ')': case '[': case ']': {
      return grn_ts_expr_parser_tokenize_bracket(ctx, parser, rest, token);
    }
    case '.': {
      return grn_ts_expr_parser_tokenize_bridge(ctx, parser, rest, token);
    }
    default: {
      return grn_ts_expr_parser_tokenize_op(ctx, parser, rest, token);
    }
  }
}

/*
 * grn_ts_expr_parser_reserve_tokens() extends a token buffer for a new token.
 */
static grn_rc
grn_ts_expr_parser_reserve_tokens(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  size_t i, n_bytes, new_max_n_tokens;
  grn_ts_expr_token **new_tokens;
  if (parser->n_tokens < parser->max_n_tokens) {
    return GRN_SUCCESS;
  }
  new_max_n_tokens = parser->n_tokens * 2;
  if (!new_max_n_tokens) {
    new_max_n_tokens = 1;
  }
  n_bytes = sizeof(grn_ts_expr_token *) * new_max_n_tokens;
  new_tokens = (grn_ts_expr_token **)GRN_REALLOC(parser->tokens, n_bytes);
  if (!new_tokens) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                      n_bytes);
  }
  for (i = parser->n_tokens; i < new_max_n_tokens; i++) {
    new_tokens[i] = NULL;
  }
  parser->tokens = new_tokens;
  parser->max_n_tokens = new_max_n_tokens;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_tokenize() tokenizes a string. */
static grn_rc
grn_ts_expr_parser_tokenize(grn_ctx *ctx, grn_ts_expr_parser *parser,
                            grn_ts_str str)
{
  grn_ts_str rest = str;
  const char *end = str.ptr + str.size;
  grn_ts_expr_token *token = NULL;
  GRN_TS_DEBUG("str = \"%.*s\"", (int)str.size, str.ptr);
  do {
    grn_rc rc = grn_ts_expr_parser_reserve_tokens(ctx, parser);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    rc = grn_ts_expr_parser_tokenize_next(ctx, parser, rest, &token);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    if ((token->type != GRN_TS_EXPR_START_TOKEN) &&
        (token->type != GRN_TS_EXPR_END_TOKEN)) {
      GRN_TS_DEBUG("token = \"%.*s\"", (int)token->src.size, token->src.ptr);
    }
    parser->tokens[parser->n_tokens++] = token;
    rest.ptr = token->src.ptr + token->src.size;
    rest.size = end - rest.ptr;
  } while (token->type != GRN_TS_EXPR_END_TOKEN);
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_push_const() pushes a token to an expression. */
static grn_rc
grn_ts_expr_parser_push_const(grn_ctx *ctx, grn_ts_expr_parser *parser,
                              grn_ts_expr_const_token *token)
{
  return grn_ts_expr_builder_push_const(ctx, parser->builder, token->data_kind,
                                        GRN_DB_VOID, token->content);
}

/* grn_ts_expr_parser_push_name() pushes a token to an expression. */
static grn_rc
grn_ts_expr_parser_push_name(grn_ctx *ctx, grn_ts_expr_parser *parser,
                             grn_ts_expr_name_token *token)
{
  return grn_ts_expr_builder_push_name(ctx, parser->builder, token->src);
}

/* grn_ts_expr_parser_push_op() pushes a token to an expression. */
static grn_rc
grn_ts_expr_parser_push_op(grn_ctx *ctx, grn_ts_expr_parser *parser,
                           grn_ts_expr_op_token *token)
{
  return grn_ts_expr_builder_push_op(ctx, parser->builder, token->op_type);
}

/*
 * grn_ts_expr_parser_apply_one() applies a bridge or prior operator.
 * If there is no target, this function returns GRN_END_OF_DATA.
 */
// FIXME: Support a ternary operator.
static grn_rc
grn_ts_expr_parser_apply_one(grn_ctx *ctx, grn_ts_expr_parser *parser,
                             grn_ts_op_precedence precedence_threshold)
{
  grn_rc rc;
  grn_ts_str src;
  grn_ts_expr_token **stack = parser->stack;
  grn_ts_expr_dummy_token *dummy_token;
  size_t n_args, depth = parser->stack_depth;
  if (depth < 2) {
    return GRN_END_OF_DATA;
  }
  if (stack[depth - 1]->type != GRN_TS_EXPR_DUMMY_TOKEN) {
    GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "argument must be dummy token");
  }

  /* Check the number of arguments. */
  switch (stack[depth - 2]->type) {
    case GRN_TS_EXPR_BRIDGE_TOKEN: {
      rc = grn_ts_expr_builder_end_subexpr(ctx, parser->builder);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      n_args = 2;
      break;
    }
    case GRN_TS_EXPR_OP_TOKEN: {
      grn_ts_expr_op_token *op_token;
      grn_ts_op_precedence precedence;
      op_token = (grn_ts_expr_op_token *)stack[depth - 2];
      precedence = grn_ts_op_get_precedence(op_token->op_type);
      if (precedence < precedence_threshold) {
        return GRN_END_OF_DATA;
      }
      rc = grn_ts_expr_parser_push_op(ctx, parser, op_token);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      n_args = grn_ts_op_get_n_args(op_token->op_type);
      break;
    }
    default: {
      return GRN_END_OF_DATA;
    }
  }

  /* Concatenate the source strings. */
  switch (n_args) {
    case 1: {
      grn_ts_expr_token *arg = stack[depth - 1];
      src.ptr = stack[depth - 2]->src.ptr;
      src.size = (arg->src.ptr + arg->src.size) - src.ptr;
      break;
    }
    case 2: {
      grn_ts_expr_token *args[2] = { stack[depth - 3], stack[depth - 1] };
      src.ptr = args[0]->src.ptr;
      src.size = (args[1]->src.ptr + args[1]->src.size) - src.ptr;
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "invalid #arguments: %" GRN_FMT_SIZE,
                        n_args);
    }
  }

  /* Replace the operator and argument tokens with a dummy token. */
  dummy_token = &parser->dummy_tokens[parser->n_dummy_tokens++];
  GRN_TS_DEBUG("dummy token: \"%.*s\"", (int)src.size, src.ptr);
  grn_ts_expr_dummy_token_init(ctx, dummy_token, src);
  depth -= n_args + 1;
  stack[depth++] = dummy_token;
  parser->stack_depth = depth;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_apply() applies bridges and prior operators. */
static grn_rc
grn_ts_expr_parser_apply(grn_ctx *ctx, grn_ts_expr_parser *parser,
                         grn_ts_op_precedence precedence_threshold)
{
  for ( ; ; ) {
    grn_rc rc = grn_ts_expr_parser_apply_one(ctx, parser,
                                             precedence_threshold);
    if (rc == GRN_END_OF_DATA) {
      return GRN_SUCCESS;
    } else if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
}

/* grn_ts_expr_parser_analyze_op() analyzes a token. */
static grn_rc
grn_ts_expr_parser_analyze_op(grn_ctx *ctx, grn_ts_expr_parser *parser,
                              grn_ts_expr_op_token *token)
{
  size_t n_args = grn_ts_op_get_n_args(token->op_type);
  grn_ts_expr_token *ex_token = parser->stack[parser->stack_depth - 1];
  if (n_args == 1) {
    if (ex_token->type == GRN_TS_EXPR_DUMMY_TOKEN) {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
    }
  } else if (n_args == 2) {
    grn_ts_op_precedence precedence = grn_ts_op_get_precedence(token->op_type);
    grn_rc rc = grn_ts_expr_parser_apply(ctx, parser, precedence);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  parser->stack[parser->stack_depth++] = (grn_ts_expr_token *)token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_analyze_bridge() analyzes a token. */
static grn_rc
grn_ts_expr_parser_analyze_bridge(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                  grn_ts_expr_bridge_token *token)
{
  grn_rc rc = grn_ts_expr_builder_begin_subexpr(ctx, parser->builder);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  parser->stack[parser->stack_depth++] = (grn_ts_expr_token *)token;
  return GRN_SUCCESS;
}

/* grn_ts_expr_parser_analyze_bracket() analyzes a token. */
static grn_rc
grn_ts_expr_parser_analyze_bracket(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                   grn_ts_expr_bracket_token *token)
{
  grn_ts_expr_token *ex_token = parser->stack[parser->stack_depth - 1];
  switch (token->src.ptr[0]) {
    case '(': {
      if (ex_token->type == GRN_TS_EXPR_DUMMY_TOKEN) {
        GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
      }
      parser->stack[parser->stack_depth++] = (grn_ts_expr_token *)token;
      return GRN_SUCCESS;
    }
    case '[': {
      if (ex_token->type != GRN_TS_EXPR_DUMMY_TOKEN) {
        GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
      }
      parser->stack[parser->stack_depth++] = (grn_ts_expr_token *)token;
      return GRN_SUCCESS;
    }
    case ')': case ']': {
      grn_ts_expr_token *ex_ex_token;
      grn_rc rc = grn_ts_expr_parser_apply(ctx, parser, 0);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      if (parser->stack_depth < 2) {
        GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
      }
      ex_ex_token = parser->stack[parser->stack_depth - 2];
      if (ex_ex_token->type != GRN_TS_EXPR_BRACKET_TOKEN) {
        GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
      }
      if (token->src.ptr[0] == ')') {
        size_t depth = parser->stack_depth;
        grn_ts_str src;
        grn_ts_expr_dummy_token *dummy_token;
        if (ex_ex_token->src.ptr[0] != '(') {
          GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
        }
        src.ptr = ex_ex_token->src.ptr;
        src.size = (token->src.ptr + token->src.size) - src.ptr;
        dummy_token = &parser->dummy_tokens[parser->n_dummy_tokens++];
        GRN_TS_DEBUG("dummy token: \"%.*s\"", (int)src.size, src.ptr);
        grn_ts_expr_dummy_token_init(ctx, dummy_token, src);
        parser->stack[depth - 2] = dummy_token;
        parser->stack_depth--;
        // TODO: Apply a function.
      } else if (token->src.ptr[0] == ']') {
        size_t depth = parser->stack_depth;
        if (ex_ex_token->src.ptr[0] != '[') {
          GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "invalid token sequence");
        }
        parser->stack[depth - 2] = parser->stack[depth - 1];
        parser->stack_depth--;
        // TODO: Push a subscript operator.
      }
      return GRN_SUCCESS;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT, "undefined bracket: \"%.*s\"",
                        (int)token->src.size, token->src.ptr);
    }
  }
}

/* grn_ts_expr_parser_analyze_token() analyzes a token. */
static grn_rc
grn_ts_expr_parser_analyze_token(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                 grn_ts_expr_token *token)
{
  switch (token->type) {
    case GRN_TS_EXPR_START_TOKEN: {
      parser->stack[parser->stack_depth++] = token;
      return GRN_SUCCESS;
    }
    case GRN_TS_EXPR_END_TOKEN: {
      return grn_ts_expr_parser_apply(ctx, parser, 0);
    }
    case GRN_TS_EXPR_CONST_TOKEN: {
      grn_ts_expr_const_token *const_token = (grn_ts_expr_const_token *)token;
      grn_ts_expr_dummy_token *dummy_token;
      grn_rc rc = grn_ts_expr_parser_push_const(ctx, parser, const_token);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      dummy_token = &parser->dummy_tokens[parser->n_dummy_tokens++];
      grn_ts_expr_dummy_token_init(ctx, dummy_token, token->src);
      parser->stack[parser->stack_depth++] = dummy_token;
      return GRN_SUCCESS;
    }
    case GRN_TS_EXPR_NAME_TOKEN: {
      grn_ts_expr_name_token *name_token = (grn_ts_expr_name_token *)token;
      grn_ts_expr_dummy_token *dummy_token;
      grn_rc rc = grn_ts_expr_parser_push_name(ctx, parser, name_token);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      dummy_token = &parser->dummy_tokens[parser->n_dummy_tokens++];
      grn_ts_expr_dummy_token_init(ctx, dummy_token, token->src);
      parser->stack[parser->stack_depth++] = dummy_token;
      return GRN_SUCCESS;
    }
    case GRN_TS_EXPR_OP_TOKEN: {
      grn_ts_expr_op_token *op_token = (grn_ts_expr_op_token *)token;
      return grn_ts_expr_parser_analyze_op(ctx, parser, op_token);
    }
    case GRN_TS_EXPR_BRIDGE_TOKEN: {
      grn_ts_expr_bridge_token *bridge_token;
      bridge_token = (grn_ts_expr_bridge_token *)token;
      return grn_ts_expr_parser_analyze_bridge(ctx, parser, bridge_token);
    }
    case GRN_TS_EXPR_BRACKET_TOKEN: {
      grn_ts_expr_bracket_token *bracket_token;
      bracket_token = (grn_ts_expr_bracket_token *)token;
      return grn_ts_expr_parser_analyze_bracket(ctx, parser, bracket_token);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid token type: %d",
                        token->type);
    }
  }
}

/* grn_ts_expr_parser_analyze() analyzes tokens. */
static grn_rc
grn_ts_expr_parser_analyze(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  size_t i;

  /* Reserve temporary work spaces. */
  if (parser->n_tokens > parser->max_n_dummy_tokens) {
    size_t n_bytes = sizeof(grn_ts_expr_dummy_token) * parser->n_tokens;
    grn_ts_expr_dummy_token *dummy_tokens = parser->dummy_tokens;
    grn_ts_expr_dummy_token *new_dummy_tokens;
    new_dummy_tokens = (grn_ts_expr_dummy_token *)GRN_REALLOC(dummy_tokens,
                                                              n_bytes);
    if (!new_dummy_tokens) {
      GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                        "GRN_REALLOC failed: %" GRN_FMT_SIZE, n_bytes);
    }
    parser->dummy_tokens = new_dummy_tokens;
    parser->max_n_dummy_tokens = parser->n_tokens;
  }
  if (parser->n_tokens > parser->stack_size) {
    size_t n_bytes = sizeof(grn_ts_expr_token *) * parser->n_tokens;
    grn_ts_expr_token **new_stack;
    new_stack = (grn_ts_expr_token **)GRN_REALLOC(parser->stack, n_bytes);
    if (!new_stack) {
      GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                        "GRN_REALLOC failed: %" GRN_FMT_SIZE, n_bytes);
    }
    parser->stack = new_stack;
    parser->stack_size = parser->n_tokens;
  }

  /* Analyze tokens. */
  for (i = 0; i < parser->n_tokens; i++) {
    grn_rc rc;
    rc = grn_ts_expr_parser_analyze_token(ctx, parser, parser->tokens[i]);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  if (parser->stack_depth != 2) {
    GRN_TS_ERR_RETURN(GRN_INVALID_FORMAT,
                      "tokens left in stack: %" GRN_FMT_SIZE,
                      parser->stack_depth);
  }
  return GRN_SUCCESS;
}

/*
 * grn_ts_expr_parser_clear() clears the internal states for parsing the next
 * string.
 */
static void
grn_ts_expr_parser_clear(grn_ctx *ctx, grn_ts_expr_parser *parser)
{
  parser->stack_depth = 0;
  if (parser->dummy_tokens) {
    size_t i;
    for (i = 0; i < parser->n_dummy_tokens; i++) {
      grn_ts_expr_dummy_token_fin(ctx, &parser->dummy_tokens[i]);
    }
    parser->n_dummy_tokens = 0;
  }
  if (parser->tokens) {
    size_t i;
    for (i = 0; i < parser->n_tokens; i++) {
      grn_ts_expr_token_close(ctx, parser->tokens[i]);
    }
    parser->n_tokens = 0;
  }
  grn_ts_expr_builder_clear(ctx, parser->builder);
}

grn_rc
grn_ts_expr_parser_parse(grn_ctx *ctx, grn_ts_expr_parser *parser,
                         grn_ts_str str, grn_ts_expr **expr)
{
  grn_rc rc;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!parser || (!str.ptr && str.size)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_expr_parser_clear(ctx, parser);
  rc = grn_ts_buf_reserve(ctx, &parser->str_buf, str.size + 1);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  grn_memcpy(parser->str_buf.ptr, str.ptr, str.size);
  ((char *)parser->str_buf.ptr)[str.size] = '\0';
  str.ptr = (const char *)parser->str_buf.ptr;
  rc = grn_ts_expr_parser_tokenize(ctx, parser, str);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_parser_analyze(ctx, parser);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  return grn_ts_expr_builder_complete(ctx, parser->builder, expr);
}

grn_rc
grn_ts_expr_parser_split(grn_ctx *ctx, grn_ts_expr_parser *parser,
                         grn_ts_str str, grn_ts_str *first, grn_ts_str *rest)
{
  size_t i;
  char stack_top;
  grn_rc rc = GRN_SUCCESS;
  grn_ts_buf stack;

  // FIXME: `stack` should be a member of `parser`.
  grn_ts_buf_init(ctx, &stack);
  for ( ; ; ) {
    str = grn_ts_str_trim_left(str);
    if (!str.size) {
      rc = GRN_END_OF_DATA;
      break;
    }
    for (i = 0; i < str.size; i++) {
      if (stack.pos) {
        if (str.ptr[i] == stack_top) {
          if (--stack.pos) {
            stack_top = ((char *)stack.ptr)[stack.pos - 1];
          }
          continue;
        }
        if (stack_top == '"') {
          /* Skip the next byte of an escape character. */
          if ((str.ptr[i] == '\\') && (i < (str.size - 1))) {
            i++;
          }
          continue;
        }
      } else if (str.ptr[i] == ',') {
        /* An expression delimiter. */
        break;
      }
      switch (str.ptr[i]) {
        case '(': {
          stack_top = ')';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '[': {
          stack_top = ']';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '{': {
          stack_top = '}';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '"': {
          stack_top = '"';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
      }
      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    if (rc != GRN_SUCCESS) {
      break;
    }
    if (i) {
      /* Set the result. */
      first->ptr = str.ptr;
      first->size = i;
      if (first->size == str.size) {
        rest->ptr = str.ptr + str.size;
        rest->size = 0;
      } else {
        rest->ptr = str.ptr + first->size + 1;
        rest->size = str.size - first->size - 1;
      }
      break;
    }
    str.ptr++;
    str.size--;
  }
  grn_ts_buf_fin(ctx, &stack);
  return rc;
}
