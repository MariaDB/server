/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

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
#include "grn_token_cursor.h"
#include "grn_string.h"
#include "grn_pat.h"
#include "grn_dat.h"

static void
grn_token_cursor_open_initialize_token_filters(grn_ctx *ctx,
                                               grn_token_cursor *token_cursor)
{
  grn_obj *token_filters = token_cursor->token_filter.objects;
  unsigned int i, n_token_filters;

  token_cursor->token_filter.data = NULL;

  if (token_filters) {
    n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);
  } else {
    n_token_filters = 0;
  }

  if (n_token_filters == 0) {
    return;
  }

  token_cursor->token_filter.data = GRN_CALLOC(sizeof(void *) * n_token_filters);
  if (!token_cursor->token_filter.data) {
    return;
  }

  for (i = 0; i < n_token_filters; i++) {
    grn_obj *token_filter_object = GRN_PTR_VALUE_AT(token_filters, i);
    grn_proc *token_filter = (grn_proc *)token_filter_object;

    token_cursor->token_filter.data[i] =
      token_filter->callbacks.token_filter.init(ctx,
                                                token_cursor->table,
                                                token_cursor->mode);
  }
}

grn_token_cursor *
grn_token_cursor_open(grn_ctx *ctx, grn_obj *table,
                      const char *str, size_t str_len,
                      grn_tokenize_mode mode, unsigned int flags)
{
  grn_token_cursor *token_cursor;
  grn_encoding encoding;
  grn_obj *tokenizer;
  grn_obj *normalizer;
  grn_obj *token_filters;
  grn_table_flags table_flags;
  if (grn_table_get_info(ctx, table, &table_flags, &encoding, &tokenizer,
                         &normalizer, &token_filters)) {
    return NULL;
  }
  if (!(token_cursor = GRN_MALLOC(sizeof(grn_token_cursor)))) { return NULL; }
  token_cursor->table = table;
  token_cursor->mode = mode;
  token_cursor->encoding = encoding;
  token_cursor->tokenizer = tokenizer;
  token_cursor->token_filter.objects = token_filters;
  token_cursor->token_filter.data = NULL;
  token_cursor->orig = (const unsigned char *)str;
  token_cursor->orig_blen = str_len;
  token_cursor->curr = NULL;
  token_cursor->nstr = NULL;
  token_cursor->curr_size = 0;
  token_cursor->pos = -1;
  token_cursor->status = GRN_TOKEN_CURSOR_DOING;
  token_cursor->force_prefix = GRN_FALSE;
  if (tokenizer) {
    grn_obj str_, flags_, mode_;
    GRN_TEXT_INIT(&str_, GRN_OBJ_DO_SHALLOW_COPY);
    GRN_TEXT_SET_REF(&str_, str, str_len);
    GRN_UINT32_INIT(&flags_, 0);
    GRN_UINT32_SET(ctx, &flags_, flags);
    GRN_UINT32_INIT(&mode_, 0);
    GRN_UINT32_SET(ctx, &mode_, mode);
    token_cursor->pctx.caller = NULL;
    token_cursor->pctx.user_data.ptr = NULL;
    token_cursor->pctx.proc = (grn_proc *)tokenizer;
    token_cursor->pctx.hooks = NULL;
    token_cursor->pctx.currh = NULL;
    token_cursor->pctx.phase = PROC_INIT;
    grn_ctx_push(ctx, &mode_);
    grn_ctx_push(ctx, &str_);
    grn_ctx_push(ctx, &flags_);
    ((grn_proc *)tokenizer)->funcs[PROC_INIT](ctx, 1, &table, &token_cursor->pctx.user_data);
    grn_obj_close(ctx, &flags_);
    grn_obj_close(ctx, &str_);
    grn_obj_close(ctx, &mode_);
  } else {
    int nflags = 0;
    token_cursor->nstr = grn_string_open_(ctx, str, str_len,
                                          normalizer,
                                          nflags,
                                          token_cursor->encoding);
    if (token_cursor->nstr) {
      const char *normalized;
      grn_string_get_normalized(ctx, token_cursor->nstr,
                                &normalized, &(token_cursor->curr_size), NULL);
      token_cursor->curr = (const unsigned char *)normalized;
    } else {
      ERR(GRN_TOKENIZER_ERROR,
          "[token-cursor][open] failed to grn_string_open()");
    }
  }

  if (ctx->rc == GRN_SUCCESS) {
    grn_token_cursor_open_initialize_token_filters(ctx, token_cursor);
  }

  if (ctx->rc) {
    grn_token_cursor_close(ctx, token_cursor);
    token_cursor = NULL;
  }
  return token_cursor;
}

static int
grn_token_cursor_next_apply_token_filters(grn_ctx *ctx,
                                          grn_token_cursor *token_cursor,
                                          grn_obj *current_token_data,
                                          grn_obj *status)
{
  grn_obj *token_filters = token_cursor->token_filter.objects;
  unsigned int i, n_token_filters;
  grn_token current_token;
  grn_token next_token;

  if (token_filters) {
    n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);
  } else {
    n_token_filters = 0;
  }

  GRN_TEXT_INIT(&(current_token.data), GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &(current_token.data),
               GRN_TEXT_VALUE(current_token_data),
               GRN_TEXT_LEN(current_token_data));
  current_token.status = GRN_INT32_VALUE(status);
  GRN_TEXT_INIT(&(next_token.data), GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &(next_token.data),
               GRN_TEXT_VALUE(&(current_token.data)),
               GRN_TEXT_LEN(&(current_token.data)));
  next_token.status = current_token.status;

  for (i = 0; i < n_token_filters; i++) {
    grn_obj *token_filter_object = GRN_PTR_VALUE_AT(token_filters, i);
    grn_proc *token_filter = (grn_proc *)token_filter_object;
    void *data = token_cursor->token_filter.data[i];

#define SKIP_FLAGS\
    (GRN_TOKEN_SKIP |\
     GRN_TOKEN_SKIP_WITH_POSITION)
    if (current_token.status & SKIP_FLAGS) {
      break;
    }
#undef SKIP_FLAGS

    token_filter->callbacks.token_filter.filter(ctx,
                                                &current_token,
                                                &next_token,
                                                data);
    GRN_TEXT_SET(ctx, &(current_token.data),
                 GRN_TEXT_VALUE(&(next_token.data)),
                 GRN_TEXT_LEN(&(next_token.data)));
    current_token.status = next_token.status;
  }

  token_cursor->curr =
    (const unsigned char *)GRN_TEXT_VALUE(&(current_token.data));
  token_cursor->curr_size = GRN_TEXT_LEN(&(current_token.data));

  return current_token.status;
}

grn_id
grn_token_cursor_next(grn_ctx *ctx, grn_token_cursor *token_cursor)
{
  int status;
  grn_id tid = GRN_ID_NIL;
  grn_obj *table = token_cursor->table;
  grn_obj *tokenizer = token_cursor->tokenizer;
  while (token_cursor->status != GRN_TOKEN_CURSOR_DONE) {
    if (tokenizer) {
      grn_obj *curr_, *stat_;
      ((grn_proc *)tokenizer)->funcs[PROC_NEXT](ctx, 1, &table, &token_cursor->pctx.user_data);
      stat_ = grn_ctx_pop(ctx);
      curr_ = grn_ctx_pop(ctx);
      status = grn_token_cursor_next_apply_token_filters(ctx, token_cursor,
                                                         curr_, stat_);
      token_cursor->status =
        ((status & GRN_TOKEN_LAST) ||
         (token_cursor->mode == GRN_TOKENIZE_GET &&
          (status & GRN_TOKEN_REACH_END)))
        ? GRN_TOKEN_CURSOR_DONE : GRN_TOKEN_CURSOR_DOING;
      token_cursor->force_prefix = GRN_FALSE;
#define SKIP_FLAGS \
      (GRN_TOKEN_SKIP | GRN_TOKEN_SKIP_WITH_POSITION)
      if (status & SKIP_FLAGS) {
        if (status & GRN_TOKEN_SKIP) {
          token_cursor->pos++;
        }
        if (token_cursor->status == GRN_TOKEN_CURSOR_DONE && tid == GRN_ID_NIL) {
          token_cursor->status = GRN_TOKEN_CURSOR_DONE_SKIP;
          break;
        } else {
          continue;
        }
      }
#undef SKIP_FLAGS
      if (status & GRN_TOKEN_FORCE_PREFIX) {
        token_cursor->force_prefix = GRN_TRUE;
      }
      if (token_cursor->curr_size == 0) {
        if (token_cursor->status != GRN_TOKEN_CURSOR_DONE) {
          char tokenizer_name[GRN_TABLE_MAX_KEY_SIZE];
          int tokenizer_name_length;
          tokenizer_name_length =
            grn_obj_name(ctx, token_cursor->tokenizer,
                         tokenizer_name, GRN_TABLE_MAX_KEY_SIZE);
          GRN_LOG(ctx, GRN_WARN,
                  "[token_next] ignore an empty token: <%.*s>: <%.*s>",
                  tokenizer_name_length, tokenizer_name,
                  token_cursor->orig_blen, token_cursor->orig);
        }
        continue;
      }
      if (token_cursor->curr_size > GRN_TABLE_MAX_KEY_SIZE) {
        GRN_LOG(ctx, GRN_WARN,
                "[token_next] ignore too long token. "
                "Token must be less than or equal to %d: <%d>(<%.*s>)",
                GRN_TABLE_MAX_KEY_SIZE,
                token_cursor->curr_size,
                token_cursor->curr_size, token_cursor->curr);
        continue;
      }
      if (status & GRN_TOKEN_UNMATURED) {
        if (status & GRN_TOKEN_OVERLAP) {
          if (token_cursor->mode == GRN_TOKENIZE_GET) {
            token_cursor->pos++;
            continue;
          }
        } else {
          if (status & GRN_TOKEN_REACH_END) {
            token_cursor->force_prefix = GRN_TRUE;
          }
        }
      }
    } else {
      token_cursor->status = GRN_TOKEN_CURSOR_DONE;
    }
    if (token_cursor->mode == GRN_TOKENIZE_ADD) {
      switch (table->header.type) {
      case GRN_TABLE_PAT_KEY :
        if (grn_io_lock(ctx, ((grn_pat *)table)->io, grn_lock_timeout)) {
          tid = GRN_ID_NIL;
        } else {
          tid = grn_pat_add(ctx, (grn_pat *)table, token_cursor->curr, token_cursor->curr_size,
                            NULL, NULL);
          grn_io_unlock(((grn_pat *)table)->io);
        }
        break;
      case GRN_TABLE_DAT_KEY :
        if (grn_io_lock(ctx, ((grn_dat *)table)->io, grn_lock_timeout)) {
          tid = GRN_ID_NIL;
        } else {
          tid = grn_dat_add(ctx, (grn_dat *)table, token_cursor->curr, token_cursor->curr_size,
                            NULL, NULL);
          grn_io_unlock(((grn_dat *)table)->io);
        }
        break;
      case GRN_TABLE_HASH_KEY :
        if (grn_io_lock(ctx, ((grn_hash *)table)->io, grn_lock_timeout)) {
          tid = GRN_ID_NIL;
        } else {
          tid = grn_hash_add(ctx, (grn_hash *)table, token_cursor->curr, token_cursor->curr_size,
                             NULL, NULL);
          grn_io_unlock(((grn_hash *)table)->io);
        }
        break;
      case GRN_TABLE_NO_KEY :
        if (token_cursor->curr_size == sizeof(grn_id)) {
          tid = *((grn_id *)token_cursor->curr);
        } else {
          tid = GRN_ID_NIL;
        }
        break;
      }
    } else if (token_cursor->mode != GRN_TOKENIZE_ONLY) {
      switch (table->header.type) {
      case GRN_TABLE_PAT_KEY :
        tid = grn_pat_get(ctx, (grn_pat *)table, token_cursor->curr, token_cursor->curr_size, NULL);
        break;
      case GRN_TABLE_DAT_KEY :
        tid = grn_dat_get(ctx, (grn_dat *)table, token_cursor->curr, token_cursor->curr_size, NULL);
        break;
      case GRN_TABLE_HASH_KEY :
        tid = grn_hash_get(ctx, (grn_hash *)table, token_cursor->curr, token_cursor->curr_size, NULL);
        break;
      case GRN_TABLE_NO_KEY :
        if (token_cursor->curr_size == sizeof(grn_id)) {
          tid = *((grn_id *)token_cursor->curr);
        } else {
          tid = GRN_ID_NIL;
        }
        break;
      }
    }
    if (token_cursor->mode != GRN_TOKENIZE_ONLY &&
        tid == GRN_ID_NIL && token_cursor->status != GRN_TOKEN_CURSOR_DONE) {
      token_cursor->status = GRN_TOKEN_CURSOR_NOT_FOUND;
    }
    token_cursor->pos++;
    break;
  }
  return tid;
}

static void
grn_token_cursor_close_token_filters(grn_ctx *ctx,
                                     grn_token_cursor *token_cursor)
{
  grn_obj *token_filters = token_cursor->token_filter.objects;
  unsigned int i, n_token_filters;

  if (!token_cursor->token_filter.data) {
    return;
  }

  if (token_filters) {
    n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);
  } else {
    n_token_filters = 0;
  }

  if (n_token_filters == 0) {
    return;
  }

  for (i = 0; i < n_token_filters; i++) {
    grn_obj *token_filter_object = GRN_PTR_VALUE_AT(token_filters, i);
    grn_proc *token_filter = (grn_proc *)token_filter_object;
    void *data = token_cursor->token_filter.data[i];

    token_filter->callbacks.token_filter.fin(ctx, data);
  }
  GRN_FREE(token_cursor->token_filter.data);
}

grn_rc
grn_token_cursor_close(grn_ctx *ctx, grn_token_cursor *token_cursor)
{
  if (token_cursor) {
    if (token_cursor->tokenizer) {
      ((grn_proc *)token_cursor->tokenizer)->funcs[PROC_FIN](ctx, 1, &token_cursor->table,
                                                             &token_cursor->pctx.user_data);
    }
    grn_token_cursor_close_token_filters(ctx, token_cursor);
    if (token_cursor->nstr) {
      grn_obj_close(ctx, token_cursor->nstr);
    }
    GRN_FREE(token_cursor);
    return GRN_SUCCESS;
  } else {
    return GRN_INVALID_ARGUMENT;
  }
}
