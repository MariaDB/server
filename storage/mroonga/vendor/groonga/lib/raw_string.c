/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016-2017 Brazil

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

#include "grn_raw_string.h"
#include "grn_str.h"

void
grn_raw_string_lstrip(grn_ctx *ctx,
                      grn_raw_string *string)
{
  const char *end;
  int space_len;

  end = string->value + string->length;
  while (string->value < end) {
    space_len = grn_isspace(string->value, ctx->encoding);
    if (space_len == 0) {
      break;
    }
    string->value += space_len;
    string->length -= space_len;
  }
}
