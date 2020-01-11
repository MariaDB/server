/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016-2016 Brazil

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

#pragma once

#include "../grn.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------
 * Operator types.
 */

typedef enum {
  /* Invalid operator. */
  GRN_TS_OP_NOP,

  /* Unary operators. */
  GRN_TS_OP_LOGICAL_NOT, /* !X */
  GRN_TS_OP_BITWISE_NOT, /* ~X */
  GRN_TS_OP_POSITIVE,    /* +X */
  GRN_TS_OP_NEGATIVE,    /* -X */

  /* Typecast operators. */
  GRN_TS_OP_FLOAT,
  GRN_TS_OP_TIME,

  /* Binary operators. */
  GRN_TS_OP_LOGICAL_AND,            /* X && Y  */
  GRN_TS_OP_LOGICAL_OR,             /* X || Y  */
  GRN_TS_OP_LOGICAL_SUB,            /* X &! Y  */
  GRN_TS_OP_BITWISE_AND,            /* X & Y   */
  GRN_TS_OP_BITWISE_OR,             /* X | Y   */
  GRN_TS_OP_BITWISE_XOR,            /* X ^ Y   */
  GRN_TS_OP_EQUAL,                  /* X == Y  */
  GRN_TS_OP_NOT_EQUAL,              /* X != Y  */
  GRN_TS_OP_LESS,                   /* X < Y   */
  GRN_TS_OP_LESS_EQUAL,             /* X <= Y  */
  GRN_TS_OP_GREATER,                /* X > Y   */
  GRN_TS_OP_GREATER_EQUAL,          /* X >= Y  */
  GRN_TS_OP_SHIFT_ARITHMETIC_LEFT,  /* X << Y  */
  GRN_TS_OP_SHIFT_ARITHMETIC_RIGHT, /* X >> Y  */
  GRN_TS_OP_SHIFT_LOGICAL_LEFT,     /* X <<< Y */
  GRN_TS_OP_SHIFT_LOGICAL_RIGHT,    /* X >>> Y */
  GRN_TS_OP_PLUS,                   /* X + Y   */
  GRN_TS_OP_MINUS,                  /* X - Y   */
  GRN_TS_OP_MULTIPLICATION,         /* X * Y   */
  GRN_TS_OP_DIVISION,               /* X / Y   */
  GRN_TS_OP_MODULUS,                /* X % Y   */
  GRN_TS_OP_MATCH,                  /* X @ Y   */
  GRN_TS_OP_PREFIX_MATCH,           /* X @^ Y  */
  GRN_TS_OP_SUFFIX_MATCH            /* X @$ Y  */
} grn_ts_op_type;

/* Operator precedence. */
typedef int grn_ts_op_precedence;

/* grn_ts_op_get_n_args() returns the number of arguments. */
size_t grn_ts_op_get_n_args(grn_ts_op_type op_type);

/*
 * grn_ts_op_get_precedence() returns the precedence.
 * A prior operator has a higher precedence.
 */
grn_ts_op_precedence grn_ts_op_get_precedence(grn_ts_op_type op_type);

#ifdef __cplusplus
}
#endif

