/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009 Brazil

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
#ifndef GRN_TOKEN_H
#define GRN_TOKEN_H

#ifndef GROONGA_IN_H
#include "groonga_in.h"
#endif /* GROONGA_IN_H */

#ifndef GRN_CTX_H
#include "ctx.h"
#endif /* GRN_CTX_H */

#ifndef GRN_DB_H
#include "db.h"
#endif /* GRN_DB_H */

#ifndef GRN_STR_H
#include "str.h"
#endif /* GRN_STR_H */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GRN_TOKEN_GET = 0,
  GRN_TOKEN_ADD,
  GRN_TOKEN_DEL
} grn_token_mode;

typedef enum {
  GRN_TOKEN_DOING = 0,
  GRN_TOKEN_DONE,
  GRN_TOKEN_NOT_FOUND
} grn_token_status;

typedef struct {
  grn_obj *table;
  const unsigned char *orig;
  const unsigned char *curr;
  uint32_t orig_blen;
  uint32_t curr_size;
  int32_t pos;
  grn_token_mode mode;
  grn_token_status status;
  uint8_t force_prefix;
  grn_obj_flags table_flags;
  grn_encoding encoding;
  grn_obj *tokenizer;
  grn_proc_ctx pctx;
  uint32_t variant;
  grn_obj *nstr;
} grn_token;

extern grn_obj *grn_token_uvector;

grn_rc grn_token_init(void);
grn_rc grn_token_fin(void);

#define GRN_TOKEN_ENABLE_TOKENIZED_DELIMITER (0x01L<<0)

GRN_API grn_token *grn_token_open(grn_ctx *ctx, grn_obj *table, const char *str,
                                  size_t str_len, grn_token_mode mode,
                                  unsigned int flags);

GRN_API grn_id grn_token_next(grn_ctx *ctx, grn_token *ng);
GRN_API grn_rc grn_token_close(grn_ctx *ctx, grn_token *ng);

grn_rc grn_db_init_mecab_tokenizer(grn_ctx *ctx);
grn_rc grn_db_init_builtin_tokenizers(grn_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* GRN_TOKEN_H */
