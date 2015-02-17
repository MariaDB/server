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

#include <string.h>

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
  "get_member"
};

const char *
grn_operator_to_string(grn_operator op)
{
  if (GRN_OP_PUSH <= op && op <= GRN_OP_GET_MEMBER) {
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
        }\
      } else {\
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, x->header.domain);\
        if (!grn_obj_cast(ctx, y, &dest, GRN_FALSE)) {\
          r = (GRN_BULK_VSIZE(&dest) == GRN_BULK_VSIZE(x) &&\
               !memcmp(GRN_BULK_HEAD(&dest), GRN_BULK_HEAD(x), GRN_BULK_VSIZE(x))); \
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
