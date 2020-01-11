/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2016 Brazil

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

#include <groonga/plugin.h>
#include <groonga/token.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define GRN_TOKENIZER_TOKENIZED_DELIMITER_UTF8     "\xEF\xBF\xBE"
#define GRN_TOKENIZER_TOKENIZED_DELIMITER_UTF8_LEN 3

/*
  grn_tokenizer_charlen() returns the length (#bytes) of the first character
  in the string specified by `str_ptr' and `str_length'. If the starting bytes
  are invalid as a character, grn_tokenizer_charlen() returns 0. See
  grn_encoding in "groonga.h" for more details of `encoding'

  Deprecated. Use grn_plugin_charlen() instead.
 */
int grn_tokenizer_charlen(grn_ctx *ctx, const char *str_ptr,
                          unsigned int str_length, grn_encoding encoding);

/*
  grn_tokenizer_isspace() returns the length (#bytes) of the first character
  in the string specified by `str_ptr' and `str_length' if it is a space
  character. Otherwise, grn_tokenizer_isspace() returns 0.

  Deprecated. Use grn_plugin_isspace() instead.
 */
int grn_tokenizer_isspace(grn_ctx *ctx, const char *str_ptr,
                          unsigned int str_length, grn_encoding encoding);

/*
  grn_tokenizer_is_tokenized_delimiter() returns whether is the first
  character in the string specified by `str_ptr' and `str_length' the
  special tokenized delimiter character or not.
 */
grn_bool grn_tokenizer_is_tokenized_delimiter(grn_ctx *ctx,
                                              const char *str_ptr,
                                              unsigned int str_length,
                                              grn_encoding encoding);

/*
  grn_tokenizer_have_tokenized_delimiter() returns whether is there
  the special delimiter character in the string specified by `str_ptr'
  and `str_length' the special tokenized delimiter character or not.
 */
GRN_PLUGIN_EXPORT grn_bool grn_tokenizer_have_tokenized_delimiter(grn_ctx *ctx,
                                                                  const char *str_ptr,
                                                                  unsigned int str_length,
                                                                  grn_encoding encoding);

/*
  grn_tokenizer_query is a structure for storing a query. See the following
  functions.
 */
typedef struct _grn_tokenizer_query grn_tokenizer_query;

struct _grn_tokenizer_query {
  grn_obj *normalized_query;
  char *query_buf;
  const char *ptr;
  unsigned int length;
  grn_encoding encoding;
  unsigned int flags;
  grn_bool have_tokenized_delimiter;
  /* Deprecated since 4.0.8. Use tokenize_mode instead. */
  grn_token_mode token_mode;
  grn_tokenize_mode tokenize_mode;
};

/*
  grn_tokenizer_query_open() parses `args' and returns a new object of
  grn_tokenizer_query. The new object stores information of the query.
  grn_tokenizer_query_open() normalizes the query if the target table
  requires normalization. grn_tokenizer_query_open() returns NULL if
  something goes wrong. Note that grn_tokenizer_query_open() must be called
  just once in the function that initializes a tokenizer.

  See `GRN_STRING_*' flags for `normalize_flags'.
 */
GRN_PLUGIN_EXPORT grn_tokenizer_query *grn_tokenizer_query_open(grn_ctx *ctx,
                                                                int num_args, grn_obj **args,
                                                                unsigned int normalize_flags);

/*
  grn_tokenizer_query_create() is deprecated. Use grn_tokenizer_query_open()
  instead.
*/

grn_tokenizer_query *grn_tokenizer_query_create(grn_ctx *ctx,
                                                int num_args, grn_obj **args);

/*
  grn_tokenizer_query_close() finalizes an object of grn_tokenizer_query
  and then frees memory allocated for that object.
 */
GRN_PLUGIN_EXPORT void grn_tokenizer_query_close(grn_ctx *ctx, grn_tokenizer_query *query);

/*
  grn_tokenizer_query_destroy() is deprecated. Use grn_tokenizer_query_close()
  instead.
 */
void grn_tokenizer_query_destroy(grn_ctx *ctx, grn_tokenizer_query *query);

/*
  grn_tokenizer_token is needed to return tokens. A grn_tokenizer_token object
  stores a token to be returned and it must be maintained until a request for
  next token or finalization comes.
 */
typedef struct _grn_tokenizer_token grn_tokenizer_token;

struct _grn_tokenizer_token {
  grn_obj str;
  grn_obj status;
};

/*
  grn_tokenizer_token_init() initializes `token'. Note that an initialized
  object must be finalized by grn_tokenizer_token_fin().
 */
GRN_PLUGIN_EXPORT void grn_tokenizer_token_init(grn_ctx *ctx, grn_tokenizer_token *token);

/*
  grn_tokenizer_token_fin() finalizes `token' that has been initialized by
  grn_tokenizer_token_init().
 */
GRN_PLUGIN_EXPORT void grn_tokenizer_token_fin(grn_ctx *ctx, grn_tokenizer_token *token);

/*
 * grn_tokenizer_status is a flag set for tokenizer status codes.
 * If a document or query contains no tokens, push an empty string with
 * GRN_TOKENIZER_TOKEN_LAST as a token.
 *
 * @deprecated since 4.0.8. Use grn_token_status instead.
 */
typedef grn_token_status grn_tokenizer_status;

/*
 * GRN_TOKENIZER_TOKEN_CONTINUE means that the next token is not the last one.
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_CONTINUE instead.
 */
#define GRN_TOKENIZER_TOKEN_CONTINUE           GRN_TOKEN_CONTINUE
/*
 * GRN_TOKENIZER_TOKEN_LAST means that the next token is the last one.
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_LAST instead.
 */
#define GRN_TOKENIZER_TOKEN_LAST               GRN_TOKEN_LAST
/*
 * GRN_TOKENIZER_TOKEN_OVERLAP means that ...
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_OVERLAP instead.
 */
#define GRN_TOKENIZER_TOKEN_OVERLAP            GRN_TOKEN_OVERLAP
/*
 * GRN_TOKENIZER_TOKEN_UNMATURED means that ...
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_UNMATURED instead.
 */
#define GRN_TOKENIZER_TOKEN_UNMATURED          GRN_TOKEN_UNMATURED
/*
 * GRN_TOKENIZER_TOKEN_REACH_END means that ...
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_REACH_END instead.
 */
#define GRN_TOKENIZER_TOKEN_REACH_END          GRN_TOKEN_REACH_END
/*
 * GRN_TOKENIZER_TOKEN_SKIP means that the token is skipped
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_SKIP instead.
 */
#define GRN_TOKENIZER_TOKEN_SKIP               GRN_TOKEN_SKIP
/*
 * GRN_TOKENIZER_TOKEN_SKIP_WITH_POSITION means that the token and postion is skipped
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_SKIP_WITH_POSITION instead.
 */
#define GRN_TOKENIZER_TOKEN_SKIP_WITH_POSITION GRN_TOKEN_SKIP_WITH_POSITION
/*
 * GRN_TOKENIZER_TOKEN_FORCE_PREIX that the token is used common prefix search
 *
 * @deprecated since 4.0.8. Use GRN_TOKEN_FORCE_PREIX instead.
 */
#define GRN_TOKENIZER_TOKEN_FORCE_PREFIX       GRN_TOKEN_FORCE_PREFIX

/*
 * GRN_TOKENIZER_CONTINUE and GRN_TOKENIZER_LAST are deprecated. They
 * are just for backward compatibility. Use
 * GRN_TOKENIZER_TOKEN_CONTINUE and GRN_TOKENIZER_TOKEN_LAST
 * instead.
 */
#define GRN_TOKENIZER_CONTINUE GRN_TOKENIZER_TOKEN_CONTINUE
#define GRN_TOKENIZER_LAST     GRN_TOKENIZER_TOKEN_LAST

/*
  grn_tokenizer_token_push() pushes the next token into `token'. Note that
  grn_tokenizer_token_push() does not make a copy of the given string. This
  means that you have to maintain a memory space allocated to the string.
  Also note that the grn_tokenizer_token object must be maintained until the
  request for the next token or finalization comes. See grn_token_status in
  this header for more details of `status'.
 */
GRN_PLUGIN_EXPORT void grn_tokenizer_token_push(grn_ctx *ctx, grn_tokenizer_token *token,
                                                const char *str_ptr, unsigned int str_length,
                                                grn_token_status status);

/*
  grn_tokenizer_tokenized_delimiter_next() extracts the next token
  from the string specified by `str_ptr' and `str_length' and pushes
  the next token into `token'. It returns the string after the next
  token. The returned string may be `NULL' when all tokens are
  extracted.
 */
GRN_PLUGIN_EXPORT const char *grn_tokenizer_tokenized_delimiter_next(grn_ctx *ctx,
                                                                     grn_tokenizer_token *token,
                                                                     const char *str_ptr,
                                                                     unsigned int str_length,
                                                                     grn_encoding encoding);

/*
  grn_tokenizer_register() registers a plugin to the database which is
  associated with `ctx'. `plugin_name_ptr' and `plugin_name_length' specify the
  plugin name. Alphabetic letters ('A'-'Z' and 'a'-'z'), digits ('0'-'9') and
  an underscore ('_') are capable characters. `init', `next' and `fin' specify
  the plugin functions. `init' is called for initializing a tokenizer for a
  document or query. `next' is called for extracting tokens one by one. `fin'
  is called for finalizing a tokenizer. grn_tokenizer_register() returns
  GRN_SUCCESS on success, an error code on failure. See "groonga.h" for more
  details of grn_proc_func and grn_user_data, that is used as an argument of
  grn_proc_func.
 */
GRN_PLUGIN_EXPORT grn_rc grn_tokenizer_register(grn_ctx *ctx, const char *plugin_name_ptr,
                                                unsigned int plugin_name_length,
                                                grn_proc_func *init, grn_proc_func *next,
                                                grn_proc_func *fin);

#ifdef __cplusplus
}  /* extern "C" */
#endif  /* __cplusplus */
