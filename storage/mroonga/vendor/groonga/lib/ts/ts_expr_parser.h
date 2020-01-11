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

#include "ts_expr.h"
#include "ts_expr_builder.h"
#include "ts_str.h"
#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GRN_TS_EXPR_DUMMY_TOKEN,  /* No extra data. */
  GRN_TS_EXPR_START_TOKEN,  /* No extra data. */
  GRN_TS_EXPR_END_TOKEN,    /* No extra data. */
  GRN_TS_EXPR_CONST_TOKEN,  /* +data_kind, content and buf. */
  GRN_TS_EXPR_NAME_TOKEN,   /* +name. */
  GRN_TS_EXPR_OP_TOKEN,     /* +op_type. */
  GRN_TS_EXPR_BRIDGE_TOKEN, /* No extra data. */
  GRN_TS_EXPR_BRACKET_TOKEN /* No extra data. */
} grn_ts_expr_token_type;

#define GRN_TS_EXPR_TOKEN_COMMON_MEMBERS\
  grn_ts_str src;              /* Source string. */\
  grn_ts_expr_token_type type; /* Token type. */

typedef struct {
  GRN_TS_EXPR_TOKEN_COMMON_MEMBERS
} grn_ts_expr_token;

typedef grn_ts_expr_token grn_ts_expr_dummy_token;
typedef grn_ts_expr_token grn_ts_expr_start_token;
typedef grn_ts_expr_token grn_ts_expr_end_token;

typedef struct {
  GRN_TS_EXPR_TOKEN_COMMON_MEMBERS
  grn_ts_data_kind data_kind; /* The data kind of the const. */
  grn_ts_any content;         /* The const. */
  grn_ts_buf buf;             /* Buffer for content.as_text. */
} grn_ts_expr_const_token;

typedef grn_ts_expr_token grn_ts_expr_name_token;

typedef struct {
  GRN_TS_EXPR_TOKEN_COMMON_MEMBERS
  grn_ts_op_type op_type;     /* Operator type. */
} grn_ts_expr_op_token;

typedef grn_ts_expr_token grn_ts_expr_bridge_token;
typedef grn_ts_expr_token grn_ts_expr_bracket_token;

typedef struct  {
  grn_ts_expr_builder *builder;          /* Builder. */
  grn_ts_buf str_buf;                    /* Buffer for a source string. */
  grn_ts_expr_token **tokens;            /* Tokens. */
  size_t n_tokens;                       /* Number of tokens. */
  size_t max_n_tokens;                   /* Maximum number of tokens. */
  grn_ts_expr_dummy_token *dummy_tokens; /* Dummy tokens. */
  size_t n_dummy_tokens;                 /* Number of dummy tokens. */
  size_t max_n_dummy_tokens;             /* Maximum number of dummy tokens. */
  grn_ts_expr_token **stack;             /* Token stack. */
  size_t stack_depth;                    /* Token stack's current depth. */
  size_t stack_size;                     /* Token stack's capacity. */
} grn_ts_expr_parser;

/* grn_ts_expr_parser_open() creates a parser. */
grn_rc grn_ts_expr_parser_open(grn_ctx *ctx, grn_obj *table,
                               grn_ts_expr_parser **parser);

/* grn_ts_expr_parser_close() destroys a parser. */
grn_rc grn_ts_expr_parser_close(grn_ctx *ctx, grn_ts_expr_parser *parser);

/* grn_ts_expr_parser_parse() parses a string and creates an expression. */
grn_rc grn_ts_expr_parser_parse(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                grn_ts_str str, grn_ts_expr **expr);

/*
 * grn_ts_expr_parser_split() splits comma-separated strings into the first
 * expression and the rest.
 * Note that if `str` is empty, this function returns GRN_END_OF_DATA.
 */
grn_rc grn_ts_expr_parser_split(grn_ctx *ctx, grn_ts_expr_parser *parser,
                                grn_ts_str str, grn_ts_str *first,
                                grn_ts_str *rest);

#ifdef __cplusplus
}
#endif

