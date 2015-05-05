/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2015 Brazil

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

#include "grn.h"
#include "grn_db.h"
#include "grn_str.h"
#include "grn_normalizer.h"

#include <string.h>

#ifdef GRN_WITH_ONIGMO
# define GRN_SUPPORT_REGEXP
#endif

#ifdef GRN_SUPPORT_REGEXP
# include <oniguruma.h>
#endif

static const char *operator_names[] = {
  "push",
  "pop",
  "nop",
  "call",
  "intern",
  "get_ref",
  "get_value",
  "and",
  "and_not",
  "or",
  "assign",
  "star_assign",
  "slash_assign",
  "mod_assign",
  "plus_assign",
  "minus_assign",
  "shiftl_assign",
  "shiftr_assign",
  "shiftrr_assign",
  "and_assign",
  "xor_assign",
  "or_assign",
  "jump",
  "cjump",
  "comma",
  "bitwise_or",
  "bitwise_xor",
  "bitwise_and",
  "bitwise_not",
  "equal",
  "not_equal",
  "less",
  "greater",
  "less_equal",
  "greater_equal",
  "in",
  "match",
  "near",
  "near2",
  "similar",
  "term_extract",
  "shiftl",
  "shiftr",
  "shiftrr",
  "plus",
  "minus",
  "star",
  "slash",
  "mod",
  "delete",
  "incr",
  "decr",
  "incr_post",
  "decr_post",
  "not",
  "adjust",
  "exact",
  "lcp",
  "partial",
  "unsplit",
  "prefix",
  "suffix",
  "geo_distance1",
  "geo_distance2",
  "geo_distance3",
  "geo_distance4",
  "geo_withinp5",
  "geo_withinp6",
  "geo_withinp8",
  "obj_search",
  "expr_get_var",
  "table_create",
  "table_select",
  "table_sort",
  "table_group",
  "json_put",
  "get_member",
  "regexp"
};

#define GRN_OP_LAST GRN_OP_REGEXP

const char *
grn_operator_to_string(grn_operator op)
{
  if (op <= GRN_OP_LAST) {
    return operator_names[op];
  } else {
    return "unknown";
  }
}

#define DO_EQ_SUB do {\
  switch (y->header.domain) {\
  case GRN_DB_INT8 :\
    r = (x_ == GRN_INT8_VALUE(y));\
    break;\
  case GRN_DB_UINT8 :\
    r = (x_ == GRN_UINT8_VALUE(y));\
    break;\
  case GRN_DB_INT16 :\
    r = (x_ == GRN_INT16_VALUE(y));\
    break;\
  case GRN_DB_UINT16 :\
    r = (x_ == GRN_UINT16_VALUE(y));\
    break;\
  case GRN_DB_INT32 :\
    r = (x_ == GRN_INT32_VALUE(y));\
    break;\
  case GRN_DB_UINT32 :\
    r = (x_ == GRN_UINT32_VALUE(y));\
    break;\
  case GRN_DB_INT64 :\
    r = (x_ == GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) == GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = (x_ == GRN_UINT64_VALUE(y));\
    break;\
  case GRN_DB_FLOAT :\
    r = ((x_ <= GRN_FLOAT_VALUE(y)) && (x_ >= GRN_FLOAT_VALUE(y)));\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    {\
      const char *p_ = GRN_TEXT_VALUE(y);\
      int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
      r = (x_ == i_);\
    }\
    break;\
  default :\
    r = GRN_FALSE;\
    break;\
  }\
} while (0)

#define DO_EQ(x,y,r) do {\
  switch (x->header.domain) {\
  case GRN_DB_VOID :\
    r = GRN_FALSE;\
    break;\
  case GRN_DB_INT8 :\
    {\
      int8_t x_ = GRN_INT8_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT8 :\
    {\
      uint8_t x_ = GRN_UINT8_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT16 :\
    {\
      int16_t x_ = GRN_INT16_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT16 :\
    {\
      uint16_t x_ = GRN_UINT16_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT32 :\
    {\
      int32_t x_ = GRN_INT32_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_UINT32 :\
    {\
      uint32_t x_ = GRN_UINT32_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_INT64 :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_TIME :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = (x_ == GRN_TIME_PACK(GRN_INT32_VALUE(y), 0));\
        break;\
      case GRN_DB_UINT32 :\
        r = (x_ == GRN_TIME_PACK(GRN_UINT32_VALUE(y), 0));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = (x_ == GRN_INT64_VALUE(y));\
        break;\
      case GRN_DB_UINT64 :\
        r = (x_ == GRN_UINT64_VALUE(y));\
        break;\
      case GRN_DB_FLOAT :\
        r = (x_ == GRN_TIME_PACK(GRN_FLOAT_VALUE(y), 0));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          grn_obj time_value_;\
          GRN_TIME_INIT(&time_value_, 0);\
          if (grn_obj_cast(ctx, y, &time_value_, GRN_FALSE) == GRN_SUCCESS) {\
            r = (x_ == GRN_TIME_VALUE(&time_value_));\
          } else {\
            r = GRN_FALSE;\
          }\
          GRN_OBJ_FIN(ctx, &time_value_);\
        }\
        break;\
      default :\
        r = GRN_FALSE;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_UINT64 :\
    {\
      uint64_t x_ = GRN_UINT64_VALUE(x);\
      DO_EQ_SUB;\
    }\
    break;\
  case GRN_DB_FLOAT :\
    {\
      double x_ = GRN_FLOAT_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = ((x_ <= GRN_INT32_VALUE(y)) && (x_ >= GRN_INT32_VALUE(y)));\
        break;\
      case GRN_DB_UINT32 :\
        r = ((x_ <= GRN_UINT32_VALUE(y)) && (x_ >= GRN_UINT32_VALUE(y)));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = ((x_ <= GRN_INT64_VALUE(y)) && (x_ >= GRN_INT64_VALUE(y)));\
        break;\
      case GRN_DB_UINT64 :\
        r = ((x_ <= GRN_UINT64_VALUE(y)) && (x_ >= GRN_UINT64_VALUE(y)));\
        break;\
      case GRN_DB_FLOAT :\
        r = ((x_ <= GRN_FLOAT_VALUE(y)) && (x_ >= GRN_FLOAT_VALUE(y)));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          const char *p_ = GRN_TEXT_VALUE(y);\
          int i_ = grn_atoi(p_, p_ + GRN_TEXT_LEN(y), NULL);\
          r = (x_ <= i_ && x_ >= i_);\
        }\
        break;\
      default :\
        r = GRN_FALSE;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    if (GRN_DB_SHORT_TEXT <= y->header.domain && y->header.domain <= GRN_DB_LONG_TEXT) {\
      uint32_t la = GRN_TEXT_LEN(x), lb = GRN_TEXT_LEN(y);\
      r =  (la == lb && !memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), lb));\
    } else {\
      const char *q_ = GRN_TEXT_VALUE(x);\
      int x_ = grn_atoi(q_, q_ + GRN_TEXT_LEN(x), NULL);\
      DO_EQ_SUB;\
    }\
    break;\
  default :\
    if ((x->header.domain == y->header.domain)) {\
      r = (GRN_BULK_VSIZE(x) == GRN_BULK_VSIZE(y) &&\
           !(memcmp(GRN_BULK_HEAD(x), GRN_BULK_HEAD(y), GRN_BULK_VSIZE(x))));\
    } else {\
      grn_obj dest;\
      if (x->header.domain < y->header.domain) {\
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, y->header.domain);\
        if (!grn_obj_cast(ctx, x, &dest, GRN_FALSE)) {\
          r = (GRN_BULK_VSIZE(&dest) == GRN_BULK_VSIZE(y) &&\
               !memcmp(GRN_BULK_HEAD(&dest), GRN_BULK_HEAD(y), GRN_BULK_VSIZE(y))); \
        } else {\
          r = GRN_FALSE;\
        }\
      } else {\
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, x->header.domain);\
        if (!grn_obj_cast(ctx, y, &dest, GRN_FALSE)) {\
          r = (GRN_BULK_VSIZE(&dest) == GRN_BULK_VSIZE(x) &&\
               !memcmp(GRN_BULK_HEAD(&dest), GRN_BULK_HEAD(x), GRN_BULK_VSIZE(x))); \
        } else {\
          r = GRN_FALSE;\
        }\
      }\
      GRN_OBJ_FIN(ctx, &dest);\
    }\
    break;\
  }\
} while (0)

grn_bool
grn_operator_exec_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_EQ(x, y, r);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_not_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_EQ(x, y, r);
  GRN_API_RETURN(!r);
}

#define DO_COMPARE_SUB_NUMERIC(y,op) do {\
  switch ((y)->header.domain) {\
  case GRN_DB_INT8 :\
    r = (x_ op GRN_INT8_VALUE(y));\
    break;\
  case GRN_DB_UINT8 :\
    r = (x_ op GRN_UINT8_VALUE(y));\
    break;\
  case GRN_DB_INT16 :\
    r = (x_ op GRN_INT16_VALUE(y));\
    break;\
  case GRN_DB_UINT16 :\
    r = (x_ op GRN_UINT16_VALUE(y));\
    break;\
  case GRN_DB_INT32 :\
    r = (x_ op GRN_INT32_VALUE(y));\
    break;\
  case GRN_DB_UINT32 :\
    r = (x_ op GRN_UINT32_VALUE(y));\
    break;\
  case GRN_DB_INT64 :\
    r = (x_ op GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) op GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = (x_ op GRN_UINT64_VALUE(y));\
    break;\
  case GRN_DB_FLOAT :\
    r = (x_ op GRN_FLOAT_VALUE(y));\
    break;\
  default :\
    r = GRN_FALSE;\
    break;\
  }\
} while (0)

#define DO_COMPARE_SUB(op) do {\
  switch (y->header.domain) {\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    {\
      grn_obj y_;\
      GRN_OBJ_INIT(&y_, GRN_BULK, 0, x->header.domain);\
      if (grn_obj_cast(ctx, y, &y_, GRN_FALSE)) {\
        r = GRN_FALSE;\
      } else {\
        DO_COMPARE_SUB_NUMERIC(&y_, op);\
      }\
      GRN_OBJ_FIN(ctx, &y_);\
    }\
    break;\
  default :\
    DO_COMPARE_SUB_NUMERIC(y,op);\
    break;\
  }\
} while (0)

#define DO_COMPARE_BUILTIN(x,y,r,op) do {\
  switch (x->header.domain) {\
  case GRN_DB_INT8 :\
    {\
      int8_t x_ = GRN_INT8_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT8 :\
    {\
      uint8_t x_ = GRN_UINT8_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_INT16 :\
    {\
      int16_t x_ = GRN_INT16_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT16 :\
    {\
      uint16_t x_ = GRN_UINT16_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_INT32 :\
    {\
      int32_t x_ = GRN_INT32_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT32 :\
    {\
      uint32_t x_ = GRN_UINT32_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_TIME :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      switch (y->header.domain) {\
      case GRN_DB_INT32 :\
        r = (x_ op GRN_TIME_PACK(GRN_INT32_VALUE(y), 0));\
        break;\
      case GRN_DB_UINT32 :\
        r = (x_ op GRN_TIME_PACK(GRN_UINT32_VALUE(y), 0));\
        break;\
      case GRN_DB_INT64 :\
      case GRN_DB_TIME :\
        r = (x_ op GRN_INT64_VALUE(y));\
        break;\
      case GRN_DB_UINT64 :\
        r = (x_ op GRN_UINT64_VALUE(y));\
        break;\
      case GRN_DB_FLOAT :\
        r = (x_ op GRN_TIME_PACK(GRN_FLOAT_VALUE(y), 0));\
        break;\
      case GRN_DB_SHORT_TEXT :\
      case GRN_DB_TEXT :\
      case GRN_DB_LONG_TEXT :\
        {\
          grn_obj time_value_;\
          GRN_TIME_INIT(&time_value_, 0);\
          if (grn_obj_cast(ctx, y, &time_value_, GRN_FALSE) == GRN_SUCCESS) {\
            r = (x_ op GRN_TIME_VALUE(&time_value_));\
          } else {\
            r = GRN_FALSE;\
          }\
          GRN_OBJ_FIN(ctx, &time_value_);\
        }\
        break;\
      default :\
        r = GRN_FALSE;\
        break;\
      }\
    }\
    break;\
  case GRN_DB_INT64 :\
    {\
      int64_t x_ = GRN_INT64_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT64 :\
    {\
      uint64_t x_ = GRN_UINT64_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_FLOAT :\
    {\
      double x_ = GRN_FLOAT_VALUE(x);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  case GRN_DB_SHORT_TEXT :\
  case GRN_DB_TEXT :\
  case GRN_DB_LONG_TEXT :\
    if (GRN_DB_SHORT_TEXT <= y->header.domain && y->header.domain <= GRN_DB_LONG_TEXT) {\
      int r_;\
      uint32_t la = GRN_TEXT_LEN(x), lb = GRN_TEXT_LEN(y);\
      if (la > lb) {\
        if (!(r_ = memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), lb))) {\
          r_ = 1;\
        }\
      } else {\
        if (!(r_ = memcmp(GRN_TEXT_VALUE(x), GRN_TEXT_VALUE(y), la))) {\
          r_ = la == lb ? 0 : -1;\
        }\
      }\
      r = (r_ op 0);\
    } else {\
      const char *q_ = GRN_TEXT_VALUE(x);\
      int x_ = grn_atoi(q_, q_ + GRN_TEXT_LEN(x), NULL);\
      DO_COMPARE_SUB(op);\
    }\
    break;\
  default :\
    r = GRN_FALSE;\
    break;\
  }\
} while (0)

#define DO_COMPARE(x, y, r, op) do {\
  if (x->header.domain >= GRN_N_RESERVED_TYPES) {\
    grn_obj *table;\
    table = grn_ctx_at(ctx, x->header.domain);\
    switch (table->header.type) {\
    case GRN_TABLE_HASH_KEY :\
    case GRN_TABLE_PAT_KEY :\
      {\
        grn_obj key;\
        int length;\
        GRN_OBJ_INIT(&key, GRN_BULK, 0, table->header.domain);\
        length = grn_table_get_key2(ctx, table, GRN_RECORD_VALUE(x), &key);\
        if (length > 0) {\
          grn_obj *x_original = x;\
          x = &key;\
          DO_COMPARE_BUILTIN((&key), y, r, op);\
          x = x_original;\
        } else {\
          r = GRN_FALSE;\
        }\
        GRN_OBJ_FIN(ctx, &key);\
      }\
      break;\
    default :\
      r = GRN_FALSE;\
      break;\
    }\
    grn_obj_unlink(ctx, table);\
  } else {\
    DO_COMPARE_BUILTIN(x, y, r, op);\
  }\
} while (0)

grn_bool
grn_operator_exec_less(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, <);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_greater(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, >);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_less_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, <=);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_greater_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, >=);
  GRN_API_RETURN(r);
}

static grn_bool
string_have_sub_text(grn_ctx *ctx,
                     const char *text, unsigned int text_len,
                     const char *sub_text, unsigned int sub_text_len)
{
  /* TODO: Use more fast algorithm such as Boyer-Moore algorithm that
   * is used in snip.c. */
  const char *text_end = text + text_len;
  unsigned int sub_text_current = 0;

  for (; text < text_end; text++) {
    if (text[0] == sub_text[sub_text_current]) {
      sub_text_current++;
      if (sub_text_current == sub_text_len) {
        return GRN_TRUE;
      }
    } else {
      sub_text_current = 0;
    }
  }

  return GRN_FALSE;
}

static grn_bool
string_have_prefix(grn_ctx *ctx,
                   const char *target, unsigned int target_len,
                   const char *prefix, unsigned int prefix_len)
{
  return (target_len >= prefix_len &&
          strncmp(target, prefix, prefix_len) == 0);
}

static grn_bool
string_match_regexp(grn_ctx *ctx,
                    const char *target, unsigned int target_len,
                    const char *pattern, unsigned int pattern_len)
{
#ifdef GRN_SUPPORT_REGEXP
  OnigRegex regex;
  OnigEncoding onig_encoding;
  int onig_result;
  OnigErrorInfo onig_error_info;

  if (ctx->encoding == GRN_ENC_NONE) {
    return GRN_FALSE;
  }

  switch (ctx->encoding) {
  case GRN_ENC_EUC_JP :
    onig_encoding = ONIG_ENCODING_EUC_JP;
    break;
  case GRN_ENC_UTF8 :
    onig_encoding = ONIG_ENCODING_UTF8;
    break;
  case GRN_ENC_SJIS :
    onig_encoding = ONIG_ENCODING_CP932;
    break;
  case GRN_ENC_LATIN1 :
    onig_encoding = ONIG_ENCODING_ISO_8859_1;
    break;
  case GRN_ENC_KOI8R :
    onig_encoding = ONIG_ENCODING_KOI8_R;
    break;
  default :
    return GRN_FALSE;
  }

  onig_result = onig_new(&regex,
                         pattern,
                         pattern + pattern_len,
                         ONIG_OPTION_ASCII_RANGE,
                         onig_encoding,
                         ONIG_SYNTAX_RUBY,
                         &onig_error_info);
  if (onig_result != ONIG_NORMAL) {
    char message[ONIG_MAX_ERROR_MESSAGE_LEN];
    onig_error_code_to_str(message, onig_result, onig_error_info);
    ERR(GRN_INVALID_ARGUMENT,
        "[operator][regexp] "
        "failed to create regular expression object: <%.*s>: %s",
        pattern_len, pattern,
        message);
    return GRN_FALSE;
  }

  {
    OnigPosition position;
    position = onig_search(regex,
                           target,
                           target + target_len,
                           target,
                           target + target_len,
                           NULL,
                           ONIG_OPTION_NONE);
    onig_free(regex);
    return position != ONIG_MISMATCH;
  }
#else
  return GRN_FALSE;
#endif
}

static grn_bool
exec_text_operator(grn_ctx *ctx,
                   grn_operator op,
                   const char *target,
                   unsigned int target_len,
                   const char *query,
                   unsigned int query_len)
{
  grn_bool matched = GRN_FALSE;

  switch (op) {
  case GRN_OP_MATCH :
    matched = string_have_sub_text(ctx, target, target_len, query, query_len);
    break;
  case GRN_OP_PREFIX :
    matched = string_have_prefix(ctx, target, target_len, query, query_len);
    break;
  case GRN_OP_REGEXP :
    matched = string_match_regexp(ctx, target, target_len, query, query_len);
    break;
  default :
    matched = GRN_FALSE;
    break;
  }

  return matched;
}

static grn_bool
exec_text_operator_raw_text_raw_text(grn_ctx *ctx,
                                     grn_operator op,
                                     const char *target,
                                     unsigned int target_len,
                                     const char *query,
                                     unsigned int query_len)
{
  grn_obj *normalizer;
  grn_obj *norm_target;
  grn_obj *norm_query;
  const char *norm_target_raw;
  const char *norm_query_raw;
  unsigned int norm_target_raw_length_in_bytes;
  unsigned int norm_query_raw_length_in_bytes;
  grn_bool matched = GRN_FALSE;

  if (target_len == 0 || query_len == 0) {
    return GRN_FALSE;
  }

  if (op == GRN_OP_REGEXP) {
    return exec_text_operator(ctx, op,
                              target, target_len,
                              query, query_len);
  }

  normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
  norm_target = grn_string_open(ctx, target, target_len, normalizer, 0);
  norm_query  = grn_string_open(ctx, query,  query_len,  normalizer, 0);
  grn_string_get_normalized(ctx, norm_target,
                            &norm_target_raw,
                            &norm_target_raw_length_in_bytes,
                            NULL);
  grn_string_get_normalized(ctx, norm_query,
                            &norm_query_raw,
                            &norm_query_raw_length_in_bytes,
                            NULL);

  matched = exec_text_operator(ctx, op,
                               norm_target_raw,
                               norm_target_raw_length_in_bytes,
                               norm_query_raw,
                               norm_query_raw_length_in_bytes);

  grn_obj_close(ctx, norm_target);
  grn_obj_close(ctx, norm_query);
  grn_obj_unlink(ctx, normalizer);

  return matched;
}

static grn_bool
exec_text_operator_record_text(grn_ctx *ctx,
                               grn_operator op,
                               grn_obj *record, grn_obj *table,
                               grn_obj *query)
{
  grn_obj *normalizer;
  char record_key[GRN_TABLE_MAX_KEY_SIZE];
  int record_key_len;
  grn_bool matched = GRN_FALSE;

  if (table->header.domain != GRN_DB_SHORT_TEXT) {
    return GRN_FALSE;
  }

  if (GRN_TEXT_LEN(query) == 0) {
    return GRN_FALSE;
  }

  record_key_len = grn_table_get_key(ctx, table, GRN_RECORD_VALUE(record),
                                     record_key, GRN_TABLE_MAX_KEY_SIZE);
  grn_table_get_info(ctx, table, NULL, NULL, NULL, &normalizer, NULL);
  if (normalizer && (op != GRN_OP_REGEXP)) {
    grn_obj *norm_query;
    const char *norm_query_raw;
    unsigned int norm_query_raw_length_in_bytes;
    norm_query = grn_string_open(ctx,
                                 GRN_TEXT_VALUE(query),
                                 GRN_TEXT_LEN(query),
                                 normalizer,
                                 0);
    grn_string_get_normalized(ctx, norm_query,
                              &norm_query_raw,
                              &norm_query_raw_length_in_bytes,
                              NULL);
    matched = exec_text_operator(ctx,
                                 op,
                                 record_key,
                                 record_key_len,
                                 norm_query_raw,
                                 norm_query_raw_length_in_bytes);
    grn_obj_close(ctx, norm_query);
  } else {
    matched = exec_text_operator_raw_text_raw_text(ctx,
                                                   op,
                                                   record_key,
                                                   record_key_len,
                                                   GRN_TEXT_VALUE(query),
                                                   GRN_TEXT_LEN(query));
  }

  return matched;
}

static grn_bool
exec_text_operator_text_text(grn_ctx *ctx,
                             grn_operator op,
                             grn_obj *target,
                             grn_obj *query)
{
  return exec_text_operator_raw_text_raw_text(ctx,
                                              op,
                                              GRN_TEXT_VALUE(target),
                                              GRN_TEXT_LEN(target),
                                              GRN_TEXT_VALUE(query),
                                              GRN_TEXT_LEN(query));
}

static grn_bool
exec_text_operator_bulk_bulk(grn_ctx *ctx,
                             grn_operator op,
                             grn_obj *target,
                             grn_obj *query)
{
  switch (target->header.domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    switch (query->header.domain) {
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      return exec_text_operator_text_text(ctx, op, target, query);
    default :
      break;
    }
    return GRN_FALSE;
  default:
    {
      grn_obj *domain;
      domain = grn_ctx_at(ctx, target->header.domain);
      if (GRN_OBJ_TABLEP(domain)) {
        switch (query->header.domain) {
        case GRN_DB_SHORT_TEXT :
        case GRN_DB_TEXT :
        case GRN_DB_LONG_TEXT :
          return exec_text_operator_record_text(ctx, op, target, domain, query);
        default :
          break;
        }
      }
    }
    return GRN_FALSE;
  }
}

grn_bool
grn_operator_exec_match(grn_ctx *ctx, grn_obj *target, grn_obj *sub_text)
{
  grn_bool matched;
  GRN_API_ENTER;
  matched = exec_text_operator_bulk_bulk(ctx, GRN_OP_MATCH, target, sub_text);
  GRN_API_RETURN(matched);
}

grn_bool
grn_operator_exec_prefix(grn_ctx *ctx, grn_obj *target, grn_obj *prefix)
{
  grn_bool matched;
  GRN_API_ENTER;
  matched = exec_text_operator_bulk_bulk(ctx, GRN_OP_PREFIX, target, prefix);
  GRN_API_RETURN(matched);
}

grn_bool
grn_operator_exec_regexp(grn_ctx *ctx, grn_obj *target, grn_obj *pattern)
{
  grn_bool matched;
  GRN_API_ENTER;
  matched = exec_text_operator_bulk_bulk(ctx, GRN_OP_REGEXP, target, pattern);
  GRN_API_RETURN(matched);
}
