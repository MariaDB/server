/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2014-2015 Brazil

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

#include "grn_expr_code.h"

unsigned int
grn_expr_code_n_used_codes(grn_ctx *ctx,
                           grn_expr_code *start,
                           grn_expr_code *target)
{
  unsigned int n_codes;
  int i, n_args;
  grn_expr_code *sub_code;

  if (start == target) {
    return 0;
  }

  n_args = target->nargs;
  if (target->value) {
    n_args--;
    if (n_args == 0) {
      return 1;
    }
  }

  n_codes = 1;
  sub_code = target - 1;
  for (i = 0; i < n_args; i++) {
    int sub_n_codes;
    sub_n_codes = grn_expr_code_n_used_codes(ctx, start, sub_code);
    n_codes += sub_n_codes;
    sub_code -= sub_n_codes;
    if (sub_code < start) {
      /* TODO: report error */
      return 0;
    }
  }

  return n_codes;
}
