/*
  Copyright(C) 2009-2014 Brazil

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef GROONGA_II_H
#define GROONGA_II_H

#ifdef  __cplusplus
extern "C" {
#endif

/* buffered index builder */

typedef struct _grn_ii grn_ii;
typedef struct _grn_ii_buffer grn_ii_buffer;

GRN_API unsigned int grn_ii_estimate_size(grn_ctx *ctx, grn_ii *ii, grn_id tid);

GRN_API grn_ii_buffer *grn_ii_buffer_open(grn_ctx *ctx, grn_ii *ii,
                                          long long unsigned int update_buffer_size);
GRN_API grn_rc grn_ii_buffer_append(grn_ctx *ctx,
                                    grn_ii_buffer *ii_buffer,
                                    grn_id rid,
                                    unsigned int section,
                                    grn_obj *value);
GRN_API grn_rc grn_ii_buffer_commit(grn_ctx *ctx, grn_ii_buffer *ii_buffer);
GRN_API grn_rc grn_ii_buffer_close(grn_ctx *ctx, grn_ii_buffer *ii_buffer);

#ifdef __cplusplus
}
#endif

#endif /* GROONGA_II_H */
