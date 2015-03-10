/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2014 Brazil

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
#ifndef GRN_TOKEN_CURSOR_H
#define GRN_TOKEN_CURSOR_H

#include "grn_ctx.h"
#include "grn_db.h"

#include <groonga/tokenizer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GRN_TOKEN_CURSOR_DOING = 0,
  GRN_TOKEN_CURSOR_DONE,
  GRN_TOKEN_CURSOR_DONE_SKIP,
  GRN_TOKEN_CURSOR_NOT_FOUND
} grn_token_cursor_status;

struct _grn_token {
  grn_obj data;
  grn_token_status status;
};

typedef struct {
  grn_obj *table;
  const unsigned char *orig;
  const unsigned char *curr;
  uint32_t orig_blen;
  uint32_t curr_size;
  int32_t pos;
  grn_tokenize_mode mode;
  grn_token_cursor_status status;
  grn_bool force_prefix;
  grn_obj_flags table_flags;
  grn_encoding encoding;
  grn_obj *tokenizer;
  grn_proc_ctx pctx;
  grn_obj *token_filters;
  uint32_t variant;
  grn_obj *nstr;
} grn_token_cursor;

#define GRN_TOKEN_CURSOR_ENABLE_TOKENIZED_DELIMITER (0x01L<<0)

GRN_API grn_token_cursor *grn_token_cursor_open(grn_ctx *ctx, grn_obj *table,
                                                const char *str, size_t str_len,
                                                grn_tokenize_mode mode,
                                                unsigned int flags);

GRN_API grn_id grn_token_cursor_next(grn_ctx *ctx, grn_token_cursor *token_cursor);
GRN_API grn_rc grn_token_cursor_close(grn_ctx *ctx, grn_token_cursor *token_cursor);

#ifdef __cplusplus
}
#endif

#endif /* GRN_TOKEN_CURSOR_H */
