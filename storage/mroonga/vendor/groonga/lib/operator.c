/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2017 Brazil

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
#include "grn_db.h"
#include "grn_str.h"
#include "grn_normalizer.h"

#include <string.h>

#ifdef GRN_WITH_ONIGMO
# define GRN_SUPPORT_REGEXP
#endif

#ifdef GRN_SUPPORT_REGEXP
# include <onigmo.h>
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
  "regexp",
  "fuzzy"
};

#define GRN_OP_LAST GRN_OP_FUZZY

const char *
grn_operator_to_string(grn_operator op)
{
  if (op <= GRN_OP_LAST) {
    return operator_names[op];
  } else {
    return "unknown";
  }
}

grn_operator_exec_func *
grn_operator_to_exec_func(grn_operator op)
{
  grn_operator_exec_func *func = NULL;

  switch (op) {
  case GRN_OP_EQUAL :
    func = grn_operator_exec_equal;
    break;
  case GRN_OP_NOT_EQUAL :
    func = grn_operator_exec_not_equal;
    break;
  case GRN_OP_LESS :
    func = grn_operator_exec_less;
    break;
  case GRN_OP_GREATER :
    func = grn_operator_exec_greater;
    break;
  case GRN_OP_LESS_EQUAL :
    func = grn_operator_exec_less_equal;
    break;
  case GRN_OP_GREATER_EQUAL :
    func = grn_operator_exec_greater_equal;
    break;
  case GRN_OP_MATCH :
    func = grn_operator_exec_match;
    break;
  case GRN_OP_PREFIX :
    func = grn_operator_exec_prefix;
    break;
  case GRN_OP_REGEXP :
    func = grn_operator_exec_regexp;
    break;
  default :
    break;
  }

  return func;
}

#define DO_EQ_SUB do {\
  switch (y->header.domain) {\
  case GRN_DB_INT8 :\
    r = ((unsigned char) x_ == (unsigned char) GRN_INT8_VALUE(y));       \
    break;\
  case GRN_DB_UINT8 :\
    r = (x_ == GRN_UINT8_VALUE(y));\
    break;\
  case GRN_DB_INT16 :\
    r = ((int) x_ == (int) GRN_INT16_VALUE(y));  \
    break;\
  case GRN_DB_UINT16 :\
    r = (x_ == GRN_UINT16_VALUE(y));\
    break;\
  case GRN_DB_INT32 :\
    r = ((int) x_ == (int) GRN_INT32_VALUE(y));  \
    break;\
  case GRN_DB_UINT32 :\
    r = ((uint) x_ == GRN_UINT32_VALUE(y));      \
    break;\
  case GRN_DB_INT64 :\
    r = ((long long) x_ == GRN_INT64_VALUE(y));  \
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) == GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = ((unsigned long long) x_ == GRN_UINT64_VALUE(y));   \
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
      r = ((int) x_ == i_);                              \
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
        r = ((long long) x_ == GRN_INT64_VALUE(y));       \
        break;\
      case GRN_DB_UINT64 :\
        r = ((unsigned long long) x_ == GRN_UINT64_VALUE(y));    \
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
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_EQ(x, y, r);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_not_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_EQ(x, y, r);
  GRN_API_RETURN(!r);
}

#define DO_COMPARE_SCALAR_SUB_NUMERIC(y,op) do {\
  switch ((y)->header.domain) {\
  case GRN_DB_BOOL :\
    r = (x_ op (uint8_t)(GRN_BOOL_VALUE(y) ? 1 : 0));\
    break;\
  case GRN_DB_INT8 :\
    r = ((signed char) x_ op GRN_INT8_VALUE(y)); \
    break;\
  case GRN_DB_UINT8 :\
    r = ((unsigned char) x_ op GRN_UINT8_VALUE(y));      \
    break;\
  case GRN_DB_INT16 :\
    r = ((short) x_ op GRN_INT16_VALUE(y));      \
    break;\
  case GRN_DB_UINT16 :\
    r = ((unsigned short) x_ op GRN_UINT16_VALUE(y));    \
    break;\
  case GRN_DB_INT32 :\
    r = ((int) x_ op GRN_INT32_VALUE(y));        \
    break;\
  case GRN_DB_UINT32 :\
    r = ((uint) x_ op GRN_UINT32_VALUE(y));      \
    break;\
  case GRN_DB_INT64 :\
    r = ((long long) x_ op GRN_INT64_VALUE(y));  \
    break;\
  case GRN_DB_TIME :\
    r = (GRN_TIME_PACK(x_,0) op GRN_INT64_VALUE(y));\
    break;\
  case GRN_DB_UINT64 :\
    r = ((unsigned long long) x_ op GRN_UINT64_VALUE(y));        \
    break;\
  case GRN_DB_FLOAT :\
    r = (x_ op GRN_FLOAT_VALUE(y));\
    break;\
  default :\
    r = GRN_FALSE;\
    break;\
  }\
} while (0)

#define DO_COMPARE_SCALAR_SUB_BUILTIN(op) do {\
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
        DO_COMPARE_SCALAR_SUB_NUMERIC(&y_, op);\
      }\
      GRN_OBJ_FIN(ctx, &y_);\
    }\
    break;\
  default :\
    DO_COMPARE_SCALAR_SUB_NUMERIC(y,op);\
    break;\
  }\
} while (0)

#define DO_COMPARE_SCALAR_SUB(op) do {\
  if (y->header.domain >= GRN_N_RESERVED_TYPES) {\
    grn_obj *y_table;\
    y_table = grn_ctx_at(ctx, y->header.domain);\
    switch (y_table->header.type) {\
    case GRN_TABLE_HASH_KEY :\
    case GRN_TABLE_PAT_KEY :\
    case GRN_TABLE_DAT_KEY :\
      {\
        grn_obj y_key;\
        int length;\
        GRN_OBJ_INIT(&y_key, GRN_BULK, 0, y_table->header.domain);\
        length = grn_table_get_key2(ctx, y_table, GRN_RECORD_VALUE(y), &y_key);\
        if (length > 0) {\
          grn_obj *y_original = y;\
          y = &y_key;\
          DO_COMPARE_SCALAR_SUB_BUILTIN(op);\
          y = y_original;\
        } else {\
          r = GRN_FALSE;\
        }\
        GRN_OBJ_FIN(ctx, &y_key);\
      }\
      break;\
    default :\
      r = GRN_FALSE;\
      break;\
    }\
    grn_obj_unlink(ctx, y_table);\
  } else {\
    DO_COMPARE_SCALAR_SUB_BUILTIN(op);\
  }\
} while (0)

#define DO_COMPARE_SCALAR_BUILTIN(x,y,r,op) do {\
  switch (x->header.domain) {\
  case GRN_DB_BOOL :\
    {\
      uint8_t x_ = GRN_BOOL_VALUE(x) ? 1 : 0;\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_INT8 :\
    {\
      int8_t x_ = GRN_INT8_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT8 :\
    {\
      uint8_t x_ = GRN_UINT8_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_INT16 :\
    {\
      int16_t x_ = GRN_INT16_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT16 :\
    {\
      uint16_t x_ = GRN_UINT16_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_INT32 :\
    {\
      int32_t x_ = GRN_INT32_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT32 :\
    {\
      uint32_t x_ = GRN_UINT32_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
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
        r = ((long long) x_ op GRN_INT64_VALUE(y));      \
        break;\
      case GRN_DB_UINT64 :\
        r = ((unsigned long long) x_ op GRN_UINT64_VALUE(y));    \
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
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_UINT64 :\
    {\
      uint64_t x_ = GRN_UINT64_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  case GRN_DB_FLOAT :\
    {\
      double x_ = GRN_FLOAT_VALUE(x);\
      DO_COMPARE_SCALAR_SUB(op);\
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
      DO_COMPARE_SCALAR_SUB(op);\
    }\
    break;\
  default :\
    r = GRN_FALSE;\
    break;\
  }\
} while (0)

#define DO_COMPARE_SCALAR(x, y, r, op) do {\
  if (x->header.domain >= GRN_N_RESERVED_TYPES) {\
    grn_obj *x_table;\
    x_table = grn_ctx_at(ctx, x->header.domain);\
    switch (x_table->header.type) {\
    case GRN_TABLE_HASH_KEY :\
    case GRN_TABLE_PAT_KEY :\
    case GRN_TABLE_DAT_KEY :\
      {\
        grn_obj x_key;\
        int length;\
        GRN_OBJ_INIT(&x_key, GRN_BULK, 0, x_table->header.domain);\
        length = grn_table_get_key2(ctx, x_table, GRN_RECORD_VALUE(x), &x_key);\
        if (length > 0) {\
          grn_obj *x_original = x;\
          x = &x_key;\
          DO_COMPARE_SCALAR_BUILTIN((&x_key), y, r, op);\
          x = x_original;\
        } else {\
          r = GRN_FALSE;\
        }\
        GRN_OBJ_FIN(ctx, &x_key);\
      }\
      break;\
    default :\
      r = GRN_FALSE;\
      break;\
    }\
    grn_obj_unlink(ctx, x_table);\
  } else {\
    DO_COMPARE_SCALAR_BUILTIN(x, y, r, op);\
  }\
} while (0)

#define DO_COMPARE(x, y, r, op) do {\
  if (x->header.type == GRN_UVECTOR) {\
    grn_obj element_buffer;\
    unsigned int i, n;\
    unsigned int element_size;\
    GRN_VALUE_FIX_SIZE_INIT(&element_buffer, 0, x->header.domain);\
    n = grn_uvector_size(ctx, x);\
    element_size = grn_uvector_element_size(ctx, x);\
    for (i = 0; i < n; i++) {\
      grn_obj *element = &element_buffer;\
      GRN_BULK_REWIND(element);\
      grn_bulk_write(ctx, element,\
                     ((uint8_t *)GRN_BULK_HEAD(x)) + (element_size * i),\
                     element_size);\
      DO_COMPARE_SCALAR(element, y, r, op);\
      if (r) {\
        break;\
      }\
    }\
    GRN_OBJ_FIN(ctx, &element_buffer);\
  } else {\
    if (GRN_BULK_VSIZE(x) == 0 || GRN_BULK_VSIZE(y) == 0) {\
      r = GRN_FALSE;\
    } else {\
      DO_COMPARE_SCALAR(x, y, r, op);\
    }\
  }\
} while (0)

grn_bool
grn_operator_exec_less(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, <);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_greater(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, >);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_less_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, <=);
  GRN_API_RETURN(r);
}

grn_bool
grn_operator_exec_greater_equal(grn_ctx *ctx, grn_obj *x, grn_obj *y)
{
  grn_bool r = GRN_FALSE;
  GRN_API_ENTER;
  DO_COMPARE(x, y, r, >=);
  GRN_API_RETURN(r);
}

static grn_bool
exec_match_uvector_bulk(grn_ctx *ctx, grn_obj *uvector, grn_obj *query)
{
  grn_bool matched = GRN_FALSE;
  unsigned int i, size;
  grn_obj element;
  unsigned int element_size;

  size = grn_uvector_size(ctx, uvector);
  element_size = grn_uvector_element_size(ctx, uvector);
  GRN_VALUE_FIX_SIZE_INIT(&element, 0, uvector->header.domain);
  for (i = 0; i < size; i++) {
    GRN_BULK_REWIND(&element);
    grn_bulk_write(ctx, &element,
                   GRN_BULK_HEAD(uvector) + (element_size * i),
                   element_size);
    if (grn_operator_exec_equal(ctx, &element, query)) {
      matched = GRN_TRUE;
      break;
    }
  }
  GRN_OBJ_FIN(ctx, &element);

  return matched;
}

static grn_bool
exec_match_vector_bulk(grn_ctx *ctx, grn_obj *vector, grn_obj *query)
{
  grn_bool matched = GRN_FALSE;
  unsigned int i, size;
  grn_obj element;

  size = grn_vector_size(ctx, vector);
  GRN_VOID_INIT(&element);
  for (i = 0; i < size; i++) {
    const char *content;
    unsigned int content_size;
    grn_id domain_id;

    content_size = grn_vector_get_element(ctx, vector, i,
                                          &content, NULL, &domain_id);
    grn_obj_reinit(ctx, &element, domain_id, 0);
    grn_bulk_write(ctx, &element, content, content_size);
    if (grn_operator_exec_equal(ctx, &element, query)) {
      matched = GRN_TRUE;
      break;
    }
  }
  GRN_OBJ_FIN(ctx, &element);

  return matched;
}

#ifdef GRN_SUPPORT_REGEXP
static OnigRegex
regexp_compile(grn_ctx *ctx,
               const char *pattern,
               unsigned int pattern_len,
               const OnigSyntaxType *syntax)
{
  OnigRegex regex;
  OnigEncoding onig_encoding;
  int onig_result;
  OnigErrorInfo onig_error_info;

  if (ctx->encoding == GRN_ENC_NONE) {
    return NULL;
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
    return NULL;
  }

  onig_result = onig_new(&regex,
                         pattern,
                         pattern + pattern_len,
                         ONIG_OPTION_ASCII_RANGE |
                         ONIG_OPTION_MULTILINE,
                         onig_encoding,
                         syntax,
                         &onig_error_info);
  if (onig_result != ONIG_NORMAL) {
    char message[ONIG_MAX_ERROR_MESSAGE_LEN];
    onig_error_code_to_str(message, onig_result, onig_error_info);
    ERR(GRN_INVALID_ARGUMENT,
        "[operator][regexp] "
        "failed to create regular expression object: <%.*s>: %s",
        pattern_len, pattern,
        message);
    return NULL;
  }

  return regex;
}

static grn_bool
regexp_is_match(grn_ctx *ctx, OnigRegex regex,
                const char *target, unsigned int target_len)
{
  OnigPosition position;

  position = onig_search(regex,
                         target,
                         target + target_len,
                         target,
                         target + target_len,
                         NULL,
                         ONIG_OPTION_NONE);
  return position != ONIG_MISMATCH;
}
#endif /* GRN_SUPPORT_REGEXP */

static grn_bool
string_have_sub_text(grn_ctx *ctx,
                     const char *text, unsigned int text_len,
                     const char *sub_text, unsigned int sub_text_len)
{
  if (sub_text_len == 0) {
    return GRN_FALSE;
  }

  if (sub_text_len > text_len) {
    return GRN_FALSE;
  }

#ifdef GRN_SUPPORT_REGEXP
  {
    OnigRegex regex;
    grn_bool matched;

    regex = regexp_compile(ctx, sub_text, sub_text_len, ONIG_SYNTAX_ASIS);
    if (!regex) {
      return GRN_FALSE;
    }

    matched = regexp_is_match(ctx, regex, text, text_len);
    onig_free(regex);
    return matched;
  }
#else /* GRN_SUPPORT_REGEXP */
  {
    const char *text_current = text;
    const char *text_end = text + text_len;
    const char *sub_text_current = sub_text;
    const char *sub_text_end = sub_text + sub_text_len;
    int sub_text_start_char_len;
    int sub_text_char_len;

    sub_text_start_char_len = grn_charlen(ctx, sub_text, sub_text_end);
    if (sub_text_start_char_len == 0) {
      return GRN_FALSE;
    }
    sub_text_char_len = sub_text_start_char_len;

    while (text_current < text_end) {
      int text_char_len;

      text_char_len = grn_charlen(ctx, text_current, text_end);
      if (text_char_len == 0) {
        return GRN_FALSE;
      }

      if (text_char_len == sub_text_char_len &&
          memcmp(text_current, sub_text_current, text_char_len) == 0) {
        sub_text_current += sub_text_char_len;
        if (sub_text_current == sub_text_end) {
          return GRN_TRUE;
        }

        sub_text_char_len = grn_charlen(ctx, sub_text_current, sub_text_end);
        if (sub_text_char_len == 0) {
          return GRN_FALSE;
        }
      } else {
        if (sub_text_current != sub_text) {
          sub_text_current = sub_text;
          sub_text_char_len = sub_text_start_char_len;
          continue;
        }
      }

      text_current += text_char_len;
    }

    return GRN_FALSE;
  }
#endif /* GRN_SUPPORT_REGEXP */
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
  grn_bool matched;

  regex = regexp_compile(ctx, pattern, pattern_len, ONIG_SYNTAX_RUBY);
  if (!regex) {
    return GRN_FALSE;
  }

  matched = regexp_is_match(ctx, regex, target, target_len);
  onig_free(regex);
  return matched;
#else /* GRN_SUPPORT_REGEXP */
  return GRN_FALSE;
#endif /* GRN_SUPPORT_REGEXP */
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

  if (target_len == 0 || query_len == 0) {
    return GRN_FALSE;
  }

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

  normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
  norm_target = grn_string_open(ctx, target, target_len, normalizer, 0);
  grn_string_get_normalized(ctx, norm_target,
                            &norm_target_raw,
                            &norm_target_raw_length_in_bytes,
                            NULL);

  if (op == GRN_OP_REGEXP) {
    norm_query = NULL;
    norm_query_raw = query;
    norm_query_raw_length_in_bytes = query_len;
  } else {
    norm_query = grn_string_open(ctx, query,  query_len,  normalizer, 0);
    grn_string_get_normalized(ctx, norm_query,
                              &norm_query_raw,
                              &norm_query_raw_length_in_bytes,
                              NULL);
  }

  matched = exec_text_operator(ctx, op,
                               norm_target_raw,
                               norm_target_raw_length_in_bytes,
                               norm_query_raw,
                               norm_query_raw_length_in_bytes);

  grn_obj_close(ctx, norm_target);
  if (norm_query) {
    grn_obj_close(ctx, norm_query);
  }
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
  if (normalizer) {
    grn_obj *norm_query;
    const char *norm_query_raw;
    unsigned int norm_query_raw_length_in_bytes;

    if (op == GRN_OP_REGEXP) {
      norm_query = NULL;
      norm_query_raw = GRN_TEXT_VALUE(query);
      norm_query_raw_length_in_bytes = GRN_TEXT_LEN(query);
    } else {
      norm_query = grn_string_open(ctx,
                                   GRN_TEXT_VALUE(query),
                                   GRN_TEXT_LEN(query),
                                   normalizer,
                                   0);
      grn_string_get_normalized(ctx, norm_query,
                                &norm_query_raw,
                                &norm_query_raw_length_in_bytes,
                                NULL);
    }
    matched = exec_text_operator(ctx,
                                 op,
                                 record_key,
                                 record_key_len,
                                 norm_query_raw,
                                 norm_query_raw_length_in_bytes);
    if (norm_query) {
      grn_obj_close(ctx, norm_query);
    }
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
  switch (target->header.type) {
  case GRN_UVECTOR :
    matched = exec_match_uvector_bulk(ctx, target, sub_text);
    break;
  case GRN_VECTOR :
    matched = exec_match_vector_bulk(ctx, target, sub_text);
    break;
  default :
    matched = exec_text_operator_bulk_bulk(ctx, GRN_OP_MATCH, target, sub_text);
    break;
  }
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

static grn_bool
exec_regexp_uvector_bulk(grn_ctx *ctx, grn_obj *uvector, grn_obj *pattern)
{
#ifdef GRN_SUPPORT_REGEXP
  grn_bool matched = GRN_FALSE;
  unsigned int i, size;
  OnigRegex regex;
  grn_obj *domain;
  grn_obj *normalizer;
  grn_obj *normalizer_auto = NULL;

  size = grn_uvector_size(ctx, uvector);
  if (size == 0) {
    return GRN_FALSE;
  }

  regex = regexp_compile(ctx,
                         GRN_TEXT_VALUE(pattern),
                         GRN_TEXT_LEN(pattern),
                         ONIG_SYNTAX_RUBY);
  if (!regex) {
    return GRN_FALSE;
  }

  domain = grn_ctx_at(ctx, uvector->header.domain);
  if (!domain) {
    onig_free(regex);
    return GRN_FALSE;
  }

  grn_table_get_info(ctx, domain, NULL, NULL, NULL, &normalizer, NULL);
  if (!normalizer) {
    normalizer_auto = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
  }

  for (i = 0; i < size; i++) {
    grn_id record_id;
    char key[GRN_TABLE_MAX_KEY_SIZE];
    int key_size;

    record_id = grn_uvector_get_element(ctx, uvector, i, NULL);
    key_size = grn_table_get_key(ctx, domain, record_id,
                                 key, GRN_TABLE_MAX_KEY_SIZE);
    if (key_size == 0) {
      continue;
    }

    if (normalizer) {
      matched = regexp_is_match(ctx, regex, key, key_size);
    } else {
      grn_obj *norm_key;
      const char *norm_key_raw;
      unsigned int norm_key_raw_length_in_bytes;

      norm_key = grn_string_open(ctx, key, key_size, normalizer_auto, 0);
      grn_string_get_normalized(ctx, norm_key,
                                &norm_key_raw,
                                &norm_key_raw_length_in_bytes,
                                NULL);
      matched = regexp_is_match(ctx, regex,
                                norm_key_raw,
                                norm_key_raw_length_in_bytes);
      grn_obj_unlink(ctx, norm_key);
    }

    if (matched) {
      break;
    }
  }

  if (normalizer_auto) {
    grn_obj_unlink(ctx, normalizer_auto);
  }

  grn_obj_unlink(ctx, domain);

  onig_free(regex);

  return matched;
#else /* GRN_SUPPORT_REGEXP */
  return GRN_FALSE;
#endif /* GRN_SUPPORT_REGEXP */
}

static grn_bool
exec_regexp_vector_bulk(grn_ctx *ctx, grn_obj *vector, grn_obj *pattern)
{
#ifdef GRN_SUPPORT_REGEXP
  grn_obj *normalizer = NULL;
  grn_bool matched = GRN_FALSE;
  unsigned int i, size;
  OnigRegex regex;

  size = grn_vector_size(ctx, vector);
  if (size == 0) {
    return GRN_FALSE;
  }

  regex = regexp_compile(ctx,
                         GRN_TEXT_VALUE(pattern),
                         GRN_TEXT_LEN(pattern),
                         ONIG_SYNTAX_RUBY);
  if (!regex) {
    return GRN_FALSE;
  }

  normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
  for (i = 0; i < size; i++) {
    const char *content;
    unsigned int content_size;
    grn_id domain_id;
    grn_obj *norm_content;
    const char *norm_content_raw;
    unsigned int norm_content_raw_length_in_bytes;

    content_size = grn_vector_get_element(ctx, vector, i,
                                          &content, NULL, &domain_id);
    if (content_size == 0) {
      continue;
    }

    norm_content = grn_string_open(ctx, content, content_size, normalizer, 0);
    grn_string_get_normalized(ctx, norm_content,
                              &norm_content_raw,
                              &norm_content_raw_length_in_bytes,
                              NULL);

    matched = regexp_is_match(ctx, regex,
                              norm_content_raw,
                              norm_content_raw_length_in_bytes);

    grn_obj_unlink(ctx, norm_content);

    if (matched) {
      break;
    }
  }
  grn_obj_unlink(ctx, normalizer);

  onig_free(regex);

  return matched;
#else /* GRN_SUPPORT_REGEXP */
  return GRN_FALSE;
#endif /* GRN_SUPPORT_REGEXP */
}

grn_bool
grn_operator_exec_regexp(grn_ctx *ctx, grn_obj *target, grn_obj *pattern)
{
  grn_bool matched = GRN_FALSE;
  GRN_API_ENTER;
  switch (target->header.type) {
  case GRN_UVECTOR :
    matched = exec_regexp_uvector_bulk(ctx, target, pattern);
    break;
  case GRN_VECTOR :
    matched = exec_regexp_vector_bulk(ctx, target, pattern);
    break;
  case GRN_BULK :
    matched = exec_text_operator_bulk_bulk(ctx, GRN_OP_REGEXP, target, pattern);
    break;
  default :
    matched = GRN_FALSE;
    break;
  }
  GRN_API_RETURN(matched);
}
