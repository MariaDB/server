/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2012 Brazil

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

#include "grn.h"
#include <string.h>
#include "grn_string.h"
#include "grn_normalizer.h"
#include "grn_str.h"
#include "grn_util.h"

#include <groonga/tokenizer.h>

static grn_string *
grn_fake_string_open(grn_ctx *ctx, grn_string *string)
{
  /* TODO: support GRN_STRING_REMOVE_BLANK flag and ctypes */
  grn_string *nstr = string;
  const char *str;
  unsigned int str_len;

  str = nstr->original;
  str_len = nstr->original_length_in_bytes;

  if (!(nstr->normalized = GRN_MALLOC(str_len + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[strinig][fake] failed to allocate normalized text space");
    grn_string_close(ctx, (grn_obj *)nstr);
    return NULL;
  }

  if (nstr->flags & GRN_STRING_REMOVE_TOKENIZED_DELIMITER &&
      ctx->encoding == GRN_ENC_UTF8) {
    int char_length;
    const char *source_current = str;
    const char *source_end = str + str_len;
    char *destination = nstr->normalized;
    unsigned int destination_length = 0;
    while ((char_length = grn_charlen(ctx, source_current, source_end)) > 0) {
      if (!grn_tokenizer_is_tokenized_delimiter(ctx,
                                                source_current, char_length,
                                                ctx->encoding)) {
        grn_memcpy(destination, source_current, char_length);
        destination += char_length;
        destination_length += char_length;
      }
      source_current += char_length;
    }
    nstr->normalized[destination_length] = '\0';
    nstr->normalized_length_in_bytes = destination_length;
  } else {
    grn_memcpy(nstr->normalized, str, str_len);
    nstr->normalized[str_len] = '\0';
    nstr->normalized_length_in_bytes = str_len;
  }

  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    int16_t f = 0;
    unsigned char c;
    size_t i;
    if (!(nstr->checks = (int16_t *) GRN_MALLOC(sizeof(int16_t) * str_len))) {
      grn_string_close(ctx, (grn_obj *)nstr);
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[strinig][fake] failed to allocate checks space");
      return NULL;
    }
    switch (nstr->encoding) {
    case GRN_ENC_EUC_JP:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = ((c >= 0xa1U && c <= 0xfeU) || c == 0x8eU ? 2 : (c == 0x8fU ? 3 : 1)
            );
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    case GRN_ENC_SJIS:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = (c >= 0x81U && ((c <= 0x9fU) || (c >= 0xe0U && c <= 0xfcU)) ? 2 : 1);
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    case GRN_ENC_UTF8:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = (c & 0x80U ? (c & 0x20U ? (c & 0x10U ? 4 : 3)
                           : 2)
               : 1);
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    default:
      for (i = 0; i < str_len; i++) {
        nstr->checks[i] = 1;
      }
      break;
    }
  }
  return nstr;
}

grn_obj *
grn_string_open_(grn_ctx *ctx, const char *str, unsigned int str_len,
                 grn_obj *normalizer, int flags, grn_encoding encoding)
{
  grn_string *string;
  grn_obj *obj;
  grn_bool is_normalizer_auto;

  if (!str || !str_len) {
    return NULL;
  }

  is_normalizer_auto = (normalizer == GRN_NORMALIZER_AUTO);
  if (is_normalizer_auto) {
    normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    if (!normalizer) {
      ERR(GRN_INVALID_ARGUMENT,
          "[string][open] NormalizerAuto normalizer isn't available");
      return NULL;
    }
  }

  string = GRN_MALLOCN(grn_string, 1);
  if (!string) {
    if (is_normalizer_auto) {
      grn_obj_unlink(ctx, normalizer);
    }
    GRN_LOG(ctx, GRN_LOG_ALERT,
            "[string][open] failed to allocate memory");
    return NULL;
  }

  obj = (grn_obj *)string;
  GRN_OBJ_INIT(obj, GRN_STRING, GRN_OBJ_ALLOCATED, GRN_ID_NIL);
  string->original = str;
  string->original_length_in_bytes = str_len;
  string->normalized = NULL;
  string->normalized_length_in_bytes = 0;
  string->n_characters = 0;
  string->checks = NULL;
  string->ctypes = NULL;
  string->encoding = encoding;
  string->flags = flags;

  if (!normalizer) {
    return (grn_obj *)grn_fake_string_open(ctx, string);
  }

  grn_normalizer_normalize(ctx, normalizer, (grn_obj *)string);
  if (ctx->rc) {
    grn_obj_close(ctx, obj);
    obj = NULL;
  }

  if (is_normalizer_auto) {
    grn_obj_unlink(ctx, normalizer);
  }

  return obj;
}

grn_obj *
grn_string_open(grn_ctx *ctx, const char *str, unsigned int str_len,
                grn_obj *normalizer, int flags)
{
  return grn_string_open_(ctx, str, str_len, normalizer, flags, ctx->encoding);
}

grn_rc
grn_string_get_original(grn_ctx *ctx, grn_obj *string,
                        const char **original,
                        unsigned int *length_in_bytes)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    if (original) { *original = string_->original; }
    if (length_in_bytes) {
      *length_in_bytes = string_->original_length_in_bytes;
    }
    rc = GRN_SUCCESS;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }
  GRN_API_RETURN(rc);
}

int
grn_string_get_flags(grn_ctx *ctx, grn_obj *string)
{
  int flags = 0;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    flags = string_->flags;
  }
  GRN_API_RETURN(flags);
}

grn_rc
grn_string_get_normalized(grn_ctx *ctx, grn_obj *string,
                          const char **normalized,
                          unsigned int *length_in_bytes,
                          unsigned int *n_characters)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    if (normalized) { *normalized = string_->normalized; }
    if (length_in_bytes) {
      *length_in_bytes = string_->normalized_length_in_bytes;
    }
    if (n_characters) { *n_characters = string_->n_characters; }
    rc = GRN_SUCCESS;
  } else {
    if (normalized) { *normalized = NULL; }
    if (length_in_bytes) { *length_in_bytes = 0; }
    if (n_characters) { *n_characters = 0; }
    rc = GRN_INVALID_ARGUMENT;
  }
  GRN_API_RETURN(rc);
}

grn_rc
grn_string_set_normalized(grn_ctx *ctx, grn_obj *string,
                          char *normalized, unsigned int length_in_bytes,
                          unsigned int n_characters)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    if (string_->normalized) { GRN_FREE(string_->normalized); }
    string_->normalized = normalized;
    string_->normalized_length_in_bytes = length_in_bytes;
    string_->n_characters = n_characters;
    rc = GRN_SUCCESS;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }
  GRN_API_RETURN(rc);
}

const short *
grn_string_get_checks(grn_ctx *ctx, grn_obj *string)
{
  int16_t *checks = NULL;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    checks = string_->checks;
  } else {
    checks = NULL;
  }
  GRN_API_RETURN(checks);
}

grn_rc
grn_string_set_checks(grn_ctx *ctx, grn_obj *string, short *checks)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    if (string_->checks) { GRN_FREE(string_->checks); }
    string_->checks = checks;
    rc = GRN_SUCCESS;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }
  GRN_API_RETURN(rc);
}

const unsigned char *
grn_string_get_types(grn_ctx *ctx, grn_obj *string)
{
  unsigned char *types = NULL;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    types = string_->ctypes;
  } else {
    types = NULL;
  }
  GRN_API_RETURN(types);
}

grn_rc
grn_string_set_types(grn_ctx *ctx, grn_obj *string, unsigned char *types)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    if (string_->ctypes) { GRN_FREE(string_->ctypes); }
    string_->ctypes = types;
    rc = GRN_SUCCESS;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }
  GRN_API_RETURN(rc);
}

grn_encoding
grn_string_get_encoding(grn_ctx *ctx, grn_obj *string)
{
  grn_encoding encoding = GRN_ENC_NONE;
  grn_string *string_ = (grn_string *)string;
  GRN_API_ENTER;
  if (string_) {
    encoding = string_->encoding;
  }
  GRN_API_RETURN(encoding);
}

grn_rc
grn_string_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *string)
{
  grn_string *string_ = (grn_string *)string;

  GRN_TEXT_PUTS(ctx, buffer, "#<string:");

  GRN_TEXT_PUTS(ctx, buffer, " original:<");
  GRN_TEXT_PUT(ctx, buffer,
               string_->original,
               string_->original_length_in_bytes);
  GRN_TEXT_PUTS(ctx, buffer, ">");
  GRN_TEXT_PUTS(ctx, buffer, "(");
  grn_text_itoa(ctx, buffer, string_->original_length_in_bytes);
  GRN_TEXT_PUTS(ctx, buffer, ")");

  GRN_TEXT_PUTS(ctx, buffer, " normalized:<");
  GRN_TEXT_PUT(ctx, buffer,
               string_->normalized,
               string_->normalized_length_in_bytes);
  GRN_TEXT_PUTS(ctx, buffer, ">");
  GRN_TEXT_PUTS(ctx, buffer, "(");
  grn_text_itoa(ctx, buffer, string_->normalized_length_in_bytes);
  GRN_TEXT_PUTS(ctx, buffer, ")");

  GRN_TEXT_PUTS(ctx, buffer, " n_characters:");
  grn_text_itoa(ctx, buffer, string_->n_characters);

  GRN_TEXT_PUTS(ctx, buffer, " encoding:");
  grn_inspect_encoding(ctx, buffer, string_->encoding);

  GRN_TEXT_PUTS(ctx, buffer, " flags:");
  if (string_->flags & GRN_STRING_REMOVE_BLANK) {
  GRN_TEXT_PUTS(ctx, buffer, "REMOVE_BLANK|");
  }
  if (string_->flags & GRN_STRING_WITH_TYPES) {
    GRN_TEXT_PUTS(ctx, buffer, "WITH_TYPES|");
  }
  if (string_->flags & GRN_STRING_WITH_CHECKS) {
    GRN_TEXT_PUTS(ctx, buffer, "WITH_CHECKS|");
  }
  if (string_->flags & GRN_STRING_REMOVE_TOKENIZED_DELIMITER) {
    GRN_TEXT_PUTS(ctx, buffer, "REMOVE_TOKENIZED_DELIMITER|");
  }
  if (GRN_TEXT_VALUE(buffer)[GRN_TEXT_LEN(buffer) - 1] == '|') {
    grn_bulk_truncate(ctx, buffer, GRN_TEXT_LEN(buffer) - 1);
  }

  GRN_TEXT_PUTS(ctx, buffer, ">");

  return GRN_SUCCESS;
}

grn_rc
grn_string_close(grn_ctx *ctx, grn_obj *string)
{
  grn_rc rc;
  grn_string *string_ = (grn_string *)string;
  if (string_) {
    if (string_->normalized) { GRN_FREE(string_->normalized); }
    if (string_->ctypes) { GRN_FREE(string_->ctypes); }
    if (string_->checks) { GRN_FREE(string_->checks); }
    GRN_FREE(string);
    rc = GRN_SUCCESS;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }
  return rc;
}
