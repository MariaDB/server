/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2016 Brazil

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
#include "grn_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

GRN_API grn_rc grn_normalize_offset_and_limit(grn_ctx *ctx, int size, int *offset, int *limit);

GRN_API char *grn_path_separator_to_system(char *dest, char *groonga_path);

void grn_p_record(grn_ctx *ctx, grn_obj *table, grn_id id);

/*
 * grn_mkstemp generates a unique filename from path_template, creates a
 * file with permissions 0600 and returns a open file desciptor for the file.
 * The last 6 bytes of path_template must be "XXXXXX" and these are replaced
 * with a string that makes the filename unique.
 */
int grn_mkstemp(char *path_template);
grn_bool grn_path_exist(const char *path);

int grn_tokenize(const char *str, size_t str_len,
                 const char **tokbuf, int buf_size,
                 const char **rest);

#ifdef __cplusplus
}
#endif
