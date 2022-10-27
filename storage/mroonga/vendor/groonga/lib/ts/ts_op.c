/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "ts_op.h"

size_t
grn_ts_op_get_n_args(grn_ts_op_type op_type)
{
  switch (op_type) {
    case GRN_TS_OP_LOGICAL_NOT: /* !X */
    case GRN_TS_OP_BITWISE_NOT: /* ~X */
    case GRN_TS_OP_POSITIVE:    /* +X */
    case GRN_TS_OP_NEGATIVE:    /* -X */
    case GRN_TS_OP_FLOAT:
    case GRN_TS_OP_TIME: {
      return 1;
    }
    case GRN_TS_OP_LOGICAL_AND:            /* X && Y  */
    case GRN_TS_OP_LOGICAL_OR:             /* X || Y  */
    case GRN_TS_OP_LOGICAL_SUB:            /* X &! Y  */
    case GRN_TS_OP_BITWISE_AND:            /* X & Y   */
    case GRN_TS_OP_BITWISE_OR:             /* X | Y   */
    case GRN_TS_OP_BITWISE_XOR:            /* X ^ Y   */
    case GRN_TS_OP_EQUAL:                  /* X == Y  */
    case GRN_TS_OP_NOT_EQUAL:              /* X != Y  */
    case GRN_TS_OP_LESS:                   /* X < Y   */
    case GRN_TS_OP_LESS_EQUAL:             /* X <= Y  */
    case GRN_TS_OP_GREATER:                /* X > Y   */
    case GRN_TS_OP_GREATER_EQUAL:          /* X >= Y  */
    case GRN_TS_OP_SHIFT_ARITHMETIC_LEFT:  /* X << Y  */
    case GRN_TS_OP_SHIFT_ARITHMETIC_RIGHT: /* X >> Y  */
    case GRN_TS_OP_SHIFT_LOGICAL_LEFT:     /* X <<< Y */
    case GRN_TS_OP_SHIFT_LOGICAL_RIGHT:    /* X >>> Y */
    case GRN_TS_OP_PLUS:                   /* X + Y   */
    case GRN_TS_OP_MINUS:                  /* X - Y   */
    case GRN_TS_OP_MULTIPLICATION:         /* X * Y   */
    case GRN_TS_OP_DIVISION:               /* X / Y   */
    case GRN_TS_OP_MODULUS:                /* X % Y   */
    case GRN_TS_OP_MATCH:                  /* X @ Y   */
    case GRN_TS_OP_PREFIX_MATCH:           /* X @^ Y  */
    case GRN_TS_OP_SUFFIX_MATCH: {         /* X @$ Y  */
      return 2;
    }
    default: {
      return 0;
    }
  }
}

grn_ts_op_precedence
grn_ts_op_get_precedence(grn_ts_op_type op_type)
{
  switch (op_type) {
    case GRN_TS_OP_LOGICAL_NOT:
    case GRN_TS_OP_BITWISE_NOT:
    case GRN_TS_OP_POSITIVE:
    case GRN_TS_OP_NEGATIVE: {
      return 15;
    }
    case GRN_TS_OP_FLOAT:
    case GRN_TS_OP_TIME: {
      return 16;
    }
    case GRN_TS_OP_LOGICAL_AND: {
      return 5;
    }
    case GRN_TS_OP_LOGICAL_OR: {
      return 3;
    }
    case GRN_TS_OP_LOGICAL_SUB: {
      return 4;
    }
    case GRN_TS_OP_BITWISE_AND: {
      return 8;
    }
    case GRN_TS_OP_BITWISE_OR: {
      return 6;
    }
    case GRN_TS_OP_BITWISE_XOR: {
      return 7;
    }
    case GRN_TS_OP_EQUAL:
    case GRN_TS_OP_NOT_EQUAL: {
      return 9;
    }
    case GRN_TS_OP_LESS:
    case GRN_TS_OP_LESS_EQUAL:
    case GRN_TS_OP_GREATER:
    case GRN_TS_OP_GREATER_EQUAL: {
      return 10;
    }
    case GRN_TS_OP_SHIFT_ARITHMETIC_LEFT:
    case GRN_TS_OP_SHIFT_ARITHMETIC_RIGHT:
    case GRN_TS_OP_SHIFT_LOGICAL_LEFT:
    case GRN_TS_OP_SHIFT_LOGICAL_RIGHT: {
      return 11;
    }
    case GRN_TS_OP_PLUS:
    case GRN_TS_OP_MINUS: {
      return 12;
    }
    case GRN_TS_OP_MULTIPLICATION:
    case GRN_TS_OP_DIVISION:
    case GRN_TS_OP_MODULUS: {
      return 13;
    }
    case GRN_TS_OP_MATCH:
    case GRN_TS_OP_PREFIX_MATCH:
    case GRN_TS_OP_SUFFIX_MATCH: {
      return 14;
    }
    default: {
      return 0;
    }
  }
}
