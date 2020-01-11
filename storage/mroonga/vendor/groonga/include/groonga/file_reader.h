/*
  Copyright(C) 2015-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

typedef struct _grn_file_reader grn_file_reader;

GRN_API grn_file_reader *grn_file_reader_open(grn_ctx *ctx, const char *path);
GRN_API void grn_file_reader_close(grn_ctx *ctx,
                                   grn_file_reader *reader);

GRN_API grn_rc grn_file_reader_read_line(grn_ctx *ctx,
                                         grn_file_reader *reader,
                                         grn_obj *buffer);

#ifdef __cplusplus
}
#endif
