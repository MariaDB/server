/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

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

#include "grn.h"
#include "grn_raw_string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRN_JSON_LOAD_OPEN_BRACKET 0x40000000
#define GRN_JSON_LOAD_OPEN_BRACE   0x40000001

typedef struct grn_load_input_ {
  grn_content_type type;
  grn_raw_string table;
  grn_raw_string columns;
  grn_raw_string values;
  grn_raw_string if_exists;
  grn_raw_string each;
  grn_bool output_ids;
  grn_bool output_errors;
  uint32_t emit_level;
} grn_load_input;

void grn_load_internal(grn_ctx *ctx, grn_load_input *input);

#ifdef __cplusplus
}
#endif
