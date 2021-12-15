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

#ifdef GRN_EMBEDDED
#  define GRN_PLUGIN_FUNCTION_TAG tokenizers_mecab
#endif

#include <grn_str.h>

#include <groonga.h>
#include <groonga/tokenizer.h>

#include <mecab.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static unsigned int sole_mecab_init_counter = 0;
static mecab_t *sole_mecab = NULL;
static grn_plugin_mutex *sole_mecab_mutex = NULL;
static grn_encoding sole_mecab_encoding = GRN_ENC_NONE;

static grn_bool grn_mecab_chunked_tokenize_enabled = GRN_FALSE;
static int grn_mecab_chunk_size_threshold = 8192;

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
  if (grn_strcasecmp(charset, "euc-jp") == 0) {
    return GRN_ENC_EUC_JP;
  } else if (grn_strcasecmp(charset, "utf-8") == 0 ||
             grn_strcasecmp(charset, "utf8") == 0) {
    return GRN_ENC_UTF8;
  } else if (grn_strcasecmp(charset, "shift_jis") == 0 ||
             grn_strcasecmp(charset, "shift-jis") == 0 ||
             grn_strcasecmp(charset, "sjis") == 0) {
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

static inline grn_bool
is_delimiter_character(grn_ctx *ctx, const char *character, int character_bytes)
{
  switch (character_bytes) {
  case 1 :
    switch (character[0]) {
    case ',' :
    case '.' :
    case '!' :
    case '?' :
      return GRN_TRUE;
    default :
      return GRN_FALSE;
    }
  case 3 :
    switch ((unsigned char)(character[0])) {
    case 0xE3 :
      switch ((unsigned char)(character[1])) {
      case 0x80 :
        switch ((unsigned char)(character[2])) {
        case 0x81 : /* U+3001 (0xE3 0x80 0x81 in UTF-8) IDEOGRAPHIC COMMA */
        case 0x82 : /* U+3002 (0xE3 0x80 0x82 in UTF-8) IDEOGRAPHIC FULL STOP */
          return GRN_TRUE;
        default :
          return GRN_FALSE;
        }
      default :
        return GRN_FALSE;
      }
      return GRN_FALSE;
    case 0xEF :
      switch ((unsigned char)(character[1])) {
      case 0xBC :
        switch ((unsigned char)(character[2])) {
        case 0x81 :
          /* U+FF01 (0xEF 0xBC 0x81 in UTF-8) FULLWIDTH EXCLAMATION MARK */
        case 0x9F :
          /* U+FF1F (0xEF 0xBC 0x9F in UTF-8) FULLWIDTH QUESTION MARK */
          return GRN_TRUE;
        default :
          return GRN_FALSE;
        }
      default :
        return GRN_FALSE;
      }
      return GRN_FALSE;
    default :
      return GRN_FALSE;
    }
  default :
    return GRN_FALSE;
  }
}

static grn_bool
chunked_tokenize_utf8_chunk(grn_ctx *ctx,
                            grn_mecab_tokenizer *tokenizer,
                            const char *chunk,
                            unsigned int chunk_bytes)
{
  const char *tokenized_chunk;
  size_t tokenized_chunk_length;

  tokenized_chunk = mecab_sparse_tostr2(tokenizer->mecab, chunk, chunk_bytes);
  if (!tokenized_chunk) {
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab][chunk] "
                     "mecab_sparse_tostr2() failed len=%d err=%s",
                     chunk_bytes,
                     mecab_strerror(tokenizer->mecab));
    return GRN_FALSE;
  }

  if (GRN_TEXT_LEN(&(tokenizer->buf)) > 0) {
    GRN_TEXT_PUTS(ctx, &(tokenizer->buf), " ");
  }

  tokenized_chunk_length = strlen(tokenized_chunk);
  if (tokenized_chunk_length >= 1 &&
      isspace((unsigned char)tokenized_chunk[tokenized_chunk_length - 1])) {
    GRN_TEXT_PUT(ctx, &(tokenizer->buf),
                 tokenized_chunk, tokenized_chunk_length - 1);
  } else {
    GRN_TEXT_PUT(ctx, &(tokenizer->buf),
                 tokenized_chunk, tokenized_chunk_length);
  }

  return GRN_TRUE;
}

static grn_bool
chunked_tokenize_utf8(grn_ctx *ctx,
                      grn_mecab_tokenizer *tokenizer,
                      const char *string,
                      unsigned int string_bytes)
{
  const char *chunk_start;
  const char *current;
  const char *last_delimiter;
  const char *string_end = string + string_bytes;
  grn_encoding encoding = tokenizer->query->encoding;

  if (string_bytes < grn_mecab_chunk_size_threshold) {
    return chunked_tokenize_utf8_chunk(ctx,
                                       tokenizer,
                                       string,
                                       string_bytes);
  }

  chunk_start = current = string;
  last_delimiter = NULL;
  while (current < string_end) {
    int space_bytes;
    int character_bytes;
    const char *current_character;

    space_bytes = grn_isspace(current, encoding);
    if (space_bytes > 0) {
      if (chunk_start != current) {
        grn_bool succeeded;
        succeeded = chunked_tokenize_utf8_chunk(ctx,
                                                tokenizer,
                                                chunk_start,
                                                current - chunk_start);
        if (!succeeded) {
          return succeeded;
        }
      }
      current += space_bytes;
      chunk_start = current;
      last_delimiter = NULL;
      continue;
    }

    character_bytes = grn_charlen_(ctx, current, string_end, encoding);
    if (character_bytes == 0) {
      GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                       "[tokenizer][mecab][chunk] "
                       "invalid byte sequence: position=%d",
                       (int)(current - string));
      return GRN_FALSE;
    }

    current_character = current;
    current += character_bytes;
    if (is_delimiter_character(ctx, current_character, character_bytes)) {
      last_delimiter = current;
    }

    if ((current - chunk_start) >= grn_mecab_chunk_size_threshold) {
      grn_bool succeeded;
      if (last_delimiter) {
        succeeded = chunked_tokenize_utf8_chunk(ctx,
                                                tokenizer,
                                                chunk_start,
                                                last_delimiter - chunk_start);
        chunk_start = last_delimiter;
      } else {
        succeeded = chunked_tokenize_utf8_chunk(ctx,
                                                tokenizer,
                                                chunk_start,
                                                current - chunk_start);
        chunk_start = current;
      }
      if (!succeeded) {
        return succeeded;
      }
      last_delimiter = NULL;
    }
  }

  if (current == chunk_start) {
    return GRN_TRUE;
  } else {
    return chunked_tokenize_utf8_chunk(ctx,
                                       tokenizer,
                                       chunk_start,
                                       current - chunk_start);
  }
}

static mecab_t *
mecab_create(grn_ctx *ctx)
{
  mecab_t *mecab;
  int argc = 0;
  const char *argv[4];

  argv[argc++] = "Groonga";
  argv[argc++] = "-Owakati";
#ifdef GRN_WITH_BUNDLED_MECAB
  argv[argc++] = "--rcfile";
# ifdef WIN32
  {
    static char windows_mecab_rc_file[PATH_MAX];

    grn_strcpy(windows_mecab_rc_file,
               PATH_MAX,
               grn_plugin_windows_base_dir());
    grn_strcat(windows_mecab_rc_file,
               PATH_MAX,
               "/");
    grn_strcat(windows_mecab_rc_file,
               PATH_MAX,
               GRN_BUNDLED_MECAB_RELATIVE_RC_PATH);
    {
      char *c;
      for (c = windows_mecab_rc_file; *c != '\0'; c++) {
        if (*c == '/') {
          *c = '\\';
        }
      }
    }
    argv[argc++] = windows_mecab_rc_file;
  }
# else /* WIN32 */
  argv[argc++] = GRN_BUNDLED_MECAB_RC_PATH;
# endif /* WIN32 */
#endif /* GRN_WITH_BUNDLED_MECAB */
  mecab = mecab_new(argc, (char **)argv);

  if (!mecab) {
#ifdef GRN_WITH_BUNDLED_MECAB
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab] failed to create mecab_t: %s: "
                     "mecab_new(\"%s\", \"%s\", \"%s\", \"%s\")",
                     mecab_global_error_message(),
                     argv[0], argv[1], argv[2], argv[3]);
#else /* GRN_WITH_BUNDLED_MECAB */
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab] failed to create mecab_t: %s: "
                     "mecab_new(\"%s\", \"%s\")",
                     mecab_global_error_message(),
                     argv[0], argv[1]);
#endif /* GRN_WITH_BUNDLED_MECAB */
  }

  return mecab;
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
      sole_mecab = mecab_create(ctx);
      if (sole_mecab) {
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
    grn_bool succeeded;
    grn_plugin_mutex_lock(ctx, sole_mecab_mutex);
    if (grn_mecab_chunked_tokenize_enabled &&
        ctx->encoding == GRN_ENC_UTF8) {
      succeeded = chunked_tokenize_utf8(ctx,
                                        tokenizer,
                                        normalized_string,
                                        normalized_string_length);
    } else {
      const char *s;
      s = mecab_sparse_tostr2(tokenizer->mecab,
                              normalized_string,
                              normalized_string_length);
      if (!s) {
        succeeded = GRN_FALSE;
        GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                         "[tokenizer][mecab] "
                         "mecab_sparse_tostr() failed len=%d err=%s",
                         normalized_string_length,
                         mecab_strerror(tokenizer->mecab));
      } else {
        succeeded = GRN_TRUE;
        GRN_TEXT_PUTS(ctx, &(tokenizer->buf), s);
      }
    }
    grn_plugin_mutex_unlock(ctx, sole_mecab_mutex);
    if (!succeeded) {
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
      int space_len;

      space_len = grn_isspace(r, encoding);
      if (space_len > 0 && r == p) {
        cl = space_len;
        p = r + cl;
        continue;
      }

      if (!(cl = grn_charlen_(ctx, r, e, encoding))) {
        tokenizer->next = e;
        break;
      }

      if (space_len > 0) {
        const char *q = r + space_len;
        while (q < e && (space_len = grn_isspace(q, encoding))) {
          q += space_len;
        }
        tokenizer->next = q;
        break;
      }
    }

    if (r == e || tokenizer->next == e) {
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
  grn_encoding encoding;
  grn_bool have_same_encoding_dictionary;

  mecab = mecab_create(ctx);
  if (!mecab) {
    return;
  }

  encoding = GRN_CTX_GET_ENCODING(ctx);
  have_same_encoding_dictionary = (encoding == get_mecab_encoding(mecab));
  mecab_destroy(mecab);

  if (!have_same_encoding_dictionary) {
    GRN_PLUGIN_ERROR(ctx, GRN_TOKENIZER_ERROR,
                     "[tokenizer][mecab] "
                     "MeCab has no dictionary that uses the context encoding"
                     ": <%s>",
                     grn_encoding_to_string(encoding));
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
  ++sole_mecab_init_counter;
  if (sole_mecab_init_counter > 1)
  {
    return GRN_SUCCESS;
  }
  {
    char env[GRN_ENV_BUFFER_SIZE];

    grn_getenv("GRN_MECAB_CHUNKED_TOKENIZE_ENABLED",
               env,
               GRN_ENV_BUFFER_SIZE);
    grn_mecab_chunked_tokenize_enabled = (env[0] && strcmp(env, "yes") == 0);
  }

  {
    char env[GRN_ENV_BUFFER_SIZE];

    grn_getenv("GRN_MECAB_CHUNK_SIZE_THRESHOLD",
               env,
               GRN_ENV_BUFFER_SIZE);
    if (env[0]) {
      int threshold = -1;
      const char *end;
      const char *rest;

      end = env + strlen(env);
      threshold = grn_atoi(env, end, &rest);
      if (end > env && end == rest) {
        grn_mecab_chunk_size_threshold = threshold;
      }
    }
  }

  sole_mecab = NULL;
  sole_mecab_mutex = grn_plugin_mutex_open(ctx);
  if (!sole_mecab_mutex) {
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[tokenizer][mecab] grn_plugin_mutex_open() failed");
    return ctx->rc;
  }

  check_mecab_dictionary_encoding(ctx);
  if (ctx->rc != GRN_SUCCESS) {
    grn_plugin_mutex_close(ctx, sole_mecab_mutex);
    sole_mecab_mutex = NULL;
  }

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
  --sole_mecab_init_counter;
  if (sole_mecab_init_counter > 0)
  {
    return GRN_SUCCESS;
  }
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
