/*
  Copyright(C) 2016 Brazil

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

typedef enum {
  GRN_WINDOW_DIRECTION_ASCENDING,
  GRN_WINDOW_DIRECTION_DESCENDING
} grn_window_direction;

typedef struct _grn_window grn_window;

GRN_API grn_id grn_window_next(grn_ctx *ctx,
                               grn_window *window);
GRN_API grn_rc grn_window_rewind(grn_ctx *ctx,
                                 grn_window *window);
GRN_API grn_rc grn_window_set_direction(grn_ctx *ctx,
                                        grn_window *window,
                                        grn_window_direction direction);
GRN_API grn_obj *grn_window_get_table(grn_ctx *ctx,
                                      grn_window *window);
GRN_API grn_bool grn_window_is_sorted(grn_ctx *ctx,
                                      grn_window *window);
GRN_API size_t grn_window_get_size(grn_ctx *ctx,
                                   grn_window *window);

typedef struct _grn_window_definition {
  grn_table_sort_key *sort_keys;
  size_t n_sort_keys;
  grn_table_sort_key *group_keys;
  size_t n_group_keys;
} grn_window_definition;

typedef grn_rc grn_window_function_func(grn_ctx *ctx,
                                        grn_obj *output_column,
                                        grn_window *window,
                                        grn_obj **args,
                                        int n_args);

GRN_API grn_obj *grn_window_function_create(grn_ctx *ctx,
                                            const char *name,
                                            int name_size,
                                            grn_window_function_func func);


GRN_API grn_rc grn_table_apply_window_function(grn_ctx *ctx,
                                               grn_obj *table,
                                               grn_obj *output_column,
                                               grn_window_definition *definition,
                                               grn_obj *window_function_call);

#ifdef __cplusplus
}
#endif
