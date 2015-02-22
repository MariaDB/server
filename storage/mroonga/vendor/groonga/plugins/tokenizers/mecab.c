/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2015 Brazil

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

#include <grn_str.h>

#include <groonga.h>
#include <groonga/tokenizer.h>

#include <mecab.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static mecab_t *sole_mecab = NULL;
static grn_plugin_mutex *sole_mecab_mutex = NULL;
static grn_encoding sole_mecab_encoding = GRN_ENC_NONE;

typedef struct {
  mecab_t *mecab;
  grn_obj buf;
  const char *next;
  const char *end;
  grn_tokenizer_query *query;
  grn_tokenizer_token token;
} grn_mecab_tokenizer;

static const char *
mecab_global_error_message(void)
{
  double version;

  version = atof(mecab_version());
  /* MeCab <= 0.993 doesn't support mecab_strerror(NULL). */
  if (version <= 0.993) {
    return "Unknown";
  }

  return mecab_strerror(NULL);
}


static grn_encoding
translate_mecab_charset_to_grn_encoding(const char *charset)
{
  if (strcasecmp(charset, "euc-jp") == 0) {
    return GRN_ENC_EUC_JP;
  } else if (strcasecmp(charset, "utf-8") == 0 ||
             strcasecmp(charset, "utf8") == 0) {
    return GRN_ENC_UTF8;
  } else if (strcasecmp(charset, "shift_jis") == 0 ||
             strcasecmp(charset, "shift-jis") == 0 ||
             strcasecmp(charset, "sjis") == 0) {
    return GRN_ENC_SJIS;
  }
  return GRN_ENC_NONE;
}

static grn_encoding
get_mecab_encoding(mecab_t *mecab)
{
  grn_encoding encoding = GRN_ENC_NONE;
  const mecab_dictionary_info_t *dictionary_info;
  dictionary_info = mecab_dictionary_info(mecab);
  if (dictionary_info) {
    const char *charset = dictionary_info->charset;
    encoding = translate_mecab_charset_to_grn_encoding(charset);
  }
  return encoding;
}

/*
  This function is called for a full text search query or a document to be
  indexed. This means that both short/long strings are given.
  The return value of this function is ignored. When an error occurs in this
  function, `ctx->rc' is overwritten with an error code (not GRN_SUCCESS).
 */
static grn_obj *
mecab_init(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  const char *s;
  grn_mecab_tokenizer *tokenizer;
  unsigned int normalizer_flags = 0;
  grn_tokenizer_query *query;
  grn_obj *normalized_query;
  const char *normalized_string;
  unsigned int normalized_string_length;

  query = grn_tokenizer_query_open(ctx, nargs, args, normalizer_flags);
  if (!query) {
    return NULL;
  }
  if (!sole_mecab) {
    grn_plugin_mutex_lock(ctx, sole_mecab_mutex);
    if (!sole_mecab) {
      sole_mecab = mecab_new2("-Owakati");
      if (!sole_mecab) {
        GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                         "[tokenizer][mecab] "
                         "mecab_new2() failed on mecab_init(): %s",
                         mecab_global_error_message());
      } else {
        sole_mecab_encoding = get_mecab_encoding(sole_mecab);
      }
    }
    grn_plugin_mutex_unlock(ctx, sole_mecab_mutex);
  }
  if (!sole_mecab) {
    grn_tokenizer_query_close(ctx, query);
    return NULL;
  }

  if (query->encoding != sole_mecab_encoding) {
    grn_tokenizer_query_close(ctx, query);
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab] "
                     "MeCab dictionary charset (%s) does not match "
                     "the table encoding: <%s>",
                     grn_encoding_to_string(sole_mecab_encoding),
                     grn_encoding_to_string(query->encoding));
    return NULL;
  }

  if (!(tokenizer = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_mecab_tokenizer)))) {
    grn_tokenizer_query_close(ctx, query);
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][mecab] "
                     "memory allocation to grn_mecab_tokenizer failed");
    return NULL;
  }
  tokenizer->mecab = sole_mecab;
  tokenizer->query = query;

  normalized_query = query->normalized_query;
  grn_string_get_normalized(ctx,
                            normalized_query,
                            &normalized_string,
                            &normalized_string_length,
                            NULL);
  GRN_TEXT_INIT(&(tokenizer->buf), 0);
  if (query->have_tokenized_delimiter) {
    tokenizer->next = normalized_string;
    tokenizer->end = tokenizer->next + normalized_string_length;
  } else if (normalized_string_length == 0) {
    tokenizer->next = "";
    tokenizer->end = tokenizer->next;
  } else {
    grn_plugin_mutex_lock(ctx, sole_mecab_mutex);
    s = mecab_sparse_tostr2(tokenizer->mecab,
                            normalized_string,
                            normalized_string_length);
    if (!s) {
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab] "
                       "mecab_sparse_tostr() failed len=%d err=%s",
                       normalized_string_length,
                       mecab_strerror(tokenizer->mecab));
    } else {
      GRN_TEXT_PUTS(ctx, &(tokenizer->buf), s);
    }
    grn_plugin_mutex_unlock(ctx, sole_mecab_mutex);
    if (!s) {
      grn_tokenizer_query_close(ctx, tokenizer->query);
      GRN_PLUGIN_FREE(ctx, tokenizer);
      return NULL;
    }
    {
      char *buf, *p;
      unsigned int bufsize;

      buf = GRN_TEXT_VALUE(&(tokenizer->buf));
      bufsize = GRN_TEXT_LEN(&(tokenizer->buf));
      /* A certain version of mecab returns trailing lf or spaces. */
      for (p = buf + bufsize - 2;
           buf <= p && isspace(*(unsigned char *)p);
           p--) { *p = '\0'; }
      tokenizer->next = buf;
      tokenizer->end = p + 1;
    }
  }
  user_data->ptr = tokenizer;

  grn_tokenizer_token_init(ctx, &(tokenizer->token));

  return NULL;
}

/*
  This function returns tokens one by one.
 */
static grn_obj *
mecab_next(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  /* grn_obj *table = args[0]; */
  grn_mecab_tokenizer *tokenizer = user_data->ptr;
  grn_encoding encoding = tokenizer->query->encoding;

  if (tokenizer->query->have_tokenized_delimiter) {
    tokenizer->next =
      grn_tokenizer_tokenized_delimiter_next(ctx,
                                             &(tokenizer->token),
                                             tokenizer->next,
                                             tokenizer->end - tokenizer->next,
                                             encoding);
  } else {
    size_t cl;
    const char *p = tokenizer->next, *r;
    const char *e = tokenizer->end;
    grn_tokenizer_status status;

    for (r = p; r < e; r += cl) {
      if (!(cl = grn_charlen_(ctx, r, e, encoding))) {
        tokenizer->next = e;
        break;
      }
      if (grn_isspace(r, encoding)) {
        const char *q = r;
        while ((cl = grn_isspace(q, encoding))) { q += cl; }
        tokenizer->next = q;
        break;
      }
    }

    if (r == e) {
      status = GRN_TOKENIZER_LAST;
    } else {
      status = GRN_TOKENIZER_CONTINUE;
    }
    grn_tokenizer_token_push(ctx, &(tokenizer->token), p, r - p, status);
  }

  return NULL;
}

/*
  This function finalizes a tokenization.
 */
static grn_obj *
mecab_fin(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_mecab_tokenizer *tokenizer = user_data->ptr;
  if (!tokenizer) {
    return NULL;
  }
  grn_tokenizer_token_fin(ctx, &(tokenizer->token));
  grn_tokenizer_query_close(ctx, tokenizer->query);
  grn_obj_unlink(ctx, &(tokenizer->buf));
  GRN_PLUGIN_FREE(ctx, tokenizer);
  return NULL;
}

static void
check_mecab_dictionary_encoding(grn_ctx *ctx)
{
#ifdef HAVE_MECAB_DICTIONARY_INFO_T
  mecab_t *mecab;

  mecab = mecab_new2("-Owakati");
  if (mecab) {
    grn_encoding encoding;
    int have_same_encoding_dictionary = 0;

    encoding = GRN_CTX_GET_ENCODING(ctx);
    have_same_encoding_dictionary = encoding == get_mecab_encoding(mecab);
    mecab_destroy(mecab);

    if (!have_same_encoding_dictionary) {
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab] "
                       "MeCab has no dictionary that uses the context encoding"
                       ": <%s>",
                       grn_encoding_to_string(encoding));
    }
  } else {
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab] "
                     "mecab_new2 failed in check_mecab_dictionary_encoding: %s",
                     mecab_global_error_message());
  }
#endif
}

/*
  This function initializes a plugin. This function fails if there is no
  dictionary that uses the context encoding of groonga.
 */
grn_rc
GRN_PLUGIN_INIT(grn_ctx *ctx)
{
  sole_mecab = NULL;
  sole_mecab_mutex = grn_plugin_mutex_open(ctx);
  if (!sole_mecab_mutex) {
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][mecab] grn_plugin_mutex_open() failed");
    return ctx->rc;
  }

  check_mecab_dictionary_encoding(ctx);
  return ctx->rc;
}

/*
  This function registers a plugin to a database.
 */
grn_rc
GRN_PLUGIN_REGISTER(grn_ctx *ctx)
{
  grn_rc rc;

  rc = grn_tokenizer_register(ctx, "TokenMecab", 10,
                              mecab_init, mecab_next, mecab_fin);
  if (rc == GRN_SUCCESS) {
    grn_obj *token_mecab;
    token_mecab = grn_ctx_get(ctx, "TokenMecab", 10);
    /* Just for backward compatibility. TokenMecab was built-in not plugin. */
    if (token_mecab && grn_obj_id(ctx, token_mecab) != GRN_DB_MECAB) {
      rc = GRN_FILE_CORRUPT;
    }
  }

  return rc;
}

/*
  This function finalizes a plugin.
 */
grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  if (sole_mecab) {
    mecab_destroy(sole_mecab);
    sole_mecab = NULL;
  }
  if (sole_mecab_mutex) {
    grn_plugin_mutex_close(ctx, sole_mecab_mutex);
    sole_mecab_mutex = NULL;
  }

  return GRN_SUCCESS;
}
