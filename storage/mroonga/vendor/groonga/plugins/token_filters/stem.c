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
#  define GRN_PLUGIN_FUNCTION_TAG token_filters_stem
#endif

#include <grn_str.h>

#include <groonga.h>
#include <groonga/token_filter.h>

#include <ctype.h>
#include <string.h>

#include <libstemmer.h>

typedef struct {
  struct sb_stemmer *stemmer;
  grn_tokenizer_token token;
  grn_obj buffer;
} grn_stem_token_filter;

static void *
stem_init(grn_ctx *ctx, grn_obj *table, grn_token_mode mode)
{
  grn_stem_token_filter *token_filter;

  token_filter = GRN_PLUGIN_MALLOC(ctx, sizeof(grn_stem_token_filter));
  if (!token_filter) {
    GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                     "[token-filter][stem] "
                     "failed to allocate grn_stem_token_filter");
    return NULL;
  }

  {
    /* TODO: Support other languages. */
    const char *algorithm = "english";
    const char *encoding = "UTF_8";
    token_filter->stemmer = sb_stemmer_new(algorithm, encoding);
    if (!token_filter->stemmer) {
      GRN_PLUGIN_FREE(ctx, token_filter);
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "[token-filter][stem] "
                       "failed to create stemmer: "
                       "algorithm=<%s>, encoding=<%s>",
                       algorithm, encoding);
      return NULL;
    }
  }
  grn_tokenizer_token_init(ctx, &(token_filter->token));
  GRN_TEXT_INIT(&(token_filter->buffer), 0);

  return token_filter;
}

static grn_bool
is_stemmable(grn_obj *data, grn_bool *is_all_upper)
{
  const char *current, *end;
  grn_bool have_lower = GRN_FALSE;
  grn_bool have_upper = GRN_FALSE;

  *is_all_upper = GRN_FALSE;

  switch (data->header.domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    break;
  default :
    return GRN_FALSE;
  }

  current = GRN_TEXT_VALUE(data);
  end = current + GRN_TEXT_LEN(data);

  for (; current < end; current++) {
    if (islower((unsigned char)*current)) {
      have_lower = GRN_TRUE;
      continue;
    }
    if (isupper((unsigned char)*current)) {
      have_upper = GRN_TRUE;
      continue;
    }
    if (isdigit((unsigned char)*current)) {
      continue;
    }
    switch (*current) {
    case '-' :
    case '\'' :
      break;
    default :
      return GRN_FALSE;
    }
  }

  if (!have_lower && have_upper) {
    *is_all_upper = GRN_TRUE;
  }

  return GRN_TRUE;
}

static void
normalize(grn_ctx *ctx,
          const char *string, unsigned int length,
          grn_obj *normalized)
{
  const char *current, *end;
  const char *unwritten;

  current = unwritten = string;
  end = current + length;

  for (; current < end; current++) {
    if (isupper((unsigned char)*current)) {
      if (current > unwritten) {
        GRN_TEXT_PUT(ctx, normalized, unwritten, current - unwritten);
      }
      GRN_TEXT_PUTC(ctx, normalized, tolower((unsigned char)*current));
      unwritten = current + 1;
    }
  }

  if (current != unwritten) {
    GRN_TEXT_PUT(ctx, normalized, unwritten, current - unwritten);
  }
}

static void
unnormalize(grn_ctx *ctx,
            const char *string, unsigned int length,
            grn_obj *normalized)
{
  const char *current, *end;
  const char *unwritten;

  current = unwritten = string;
  end = current + length;

  for (; current < end; current++) {
    if (islower((unsigned char)*current)) {
      if (current > unwritten) {
        GRN_TEXT_PUT(ctx, normalized, unwritten, current - unwritten);
      }
      GRN_TEXT_PUTC(ctx, normalized, toupper((unsigned char)*current));
      unwritten = current + 1;
    }
  }

  if (current != unwritten) {
    GRN_TEXT_PUT(ctx, normalized, unwritten, current - unwritten);
  }
}

static void
stem_filter(grn_ctx *ctx,
            grn_token *current_token,
            grn_token *next_token,
            void *user_data)
{
  grn_stem_token_filter *token_filter = user_data;
  grn_obj *data;
  grn_bool is_all_upper = GRN_FALSE;

  if (GRN_CTX_GET_ENCODING(ctx) != GRN_ENC_UTF8) {
    return;
  }

  data = grn_token_get_data(ctx, current_token);
  if (!is_stemmable(data, &is_all_upper)) {
    return;
  }

  {
    const sb_symbol *stemmed;

    if (is_all_upper) {
      grn_obj *buffer;
      buffer = &(token_filter->buffer);
      GRN_BULK_REWIND(buffer);
      normalize(ctx,
                GRN_TEXT_VALUE(data),
                GRN_TEXT_LEN(data),
                buffer);
      stemmed = sb_stemmer_stem(token_filter->stemmer,
                                GRN_TEXT_VALUE(buffer), GRN_TEXT_LEN(buffer));
      if (stemmed) {
        GRN_BULK_REWIND(buffer);
        unnormalize(ctx,
                    stemmed,
                    sb_stemmer_length(token_filter->stemmer),
                    buffer);
        grn_token_set_data(ctx, next_token,
                           GRN_TEXT_VALUE(buffer), GRN_TEXT_LEN(buffer));
      } else {
        GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                         "[token-filter][stem] "
                         "failed to allocate memory for stemmed word: <%.*s> "
                         "(normalized: <%.*s>)",
                         (int)GRN_TEXT_LEN(data), GRN_TEXT_VALUE(data),
                         (int)GRN_TEXT_LEN(buffer), GRN_TEXT_VALUE(buffer));
      }
    } else {
      stemmed = sb_stemmer_stem(token_filter->stemmer,
                                GRN_TEXT_VALUE(data), GRN_TEXT_LEN(data));
      if (stemmed) {
        grn_token_set_data(ctx, next_token,
                           stemmed,
                           sb_stemmer_length(token_filter->stemmer));
      } else {
        GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                         "[token-filter][stem] "
                         "failed to allocate memory for stemmed word: <%.*s>",
                         (int)GRN_TEXT_LEN(data), GRN_TEXT_VALUE(data));
      }
    }
  }
}

static void
stem_fin(grn_ctx *ctx, void *user_data)
{
  grn_stem_token_filter *token_filter = user_data;
  if (!token_filter) {
    return;
  }

  grn_tokenizer_token_fin(ctx, &(token_filter->token));
  if (token_filter->stemmer) {
    sb_stemmer_delete(token_filter->stemmer);
  }
  GRN_OBJ_FIN(ctx, &(token_filter->buffer));
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
                                 "TokenFilterStem", -1,
                                 stem_init,
                                 stem_filter,
                                 stem_fin);

  return rc;
}

grn_rc
GRN_PLUGIN_FIN(grn_ctx *ctx)
{
  return GRN_SUCCESS;
}
