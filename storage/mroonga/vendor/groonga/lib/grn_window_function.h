/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016-2017 Brazil

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

struct _grn_window {
  grn_obj *table;
  grn_obj *grouped_table;
  grn_obj ids;
  size_t n_ids;
  ssize_t current_index;
  grn_window_direction direction;
  grn_bool is_sorted;
};

grn_rc grn_window_init(grn_ctx *ctx,
                       grn_window *window,
                       grn_obj *table,
                       grn_bool is_sorted);
grn_rc grn_window_fin(grn_ctx *ctx, grn_window *window);

#ifdef __cplusplus
}
#endif
