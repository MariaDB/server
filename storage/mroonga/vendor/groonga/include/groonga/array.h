/*
  Copyright(C) 2009-2016 Brazil

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

typedef struct _grn_array grn_array;
typedef struct _grn_array_cursor grn_array_cursor;

GRN_API grn_array *grn_array_create(grn_ctx *ctx, const char *path,
                                    unsigned int value_size, unsigned int flags);
GRN_API grn_array *grn_array_open(grn_ctx *ctx, const char *path);
GRN_API grn_rc grn_array_close(grn_ctx *ctx, grn_array *array);
GRN_API grn_id grn_array_add(grn_ctx *ctx, grn_array *array, void **value);
GRN_API grn_id grn_array_push(grn_ctx *ctx, grn_array *array,
                              void (*func)(grn_ctx *ctx, grn_array *array,
                                           grn_id id, void *func_arg),
                              void *func_arg);
GRN_API grn_id grn_array_pull(grn_ctx *ctx, grn_array *array, grn_bool blockp,
                              void (*func)(grn_ctx *ctx, grn_array *array,
                                           grn_id id, void *func_arg),
                              void *func_arg);
GRN_API void grn_array_unblock(grn_ctx *ctx, grn_array *array);
GRN_API int grn_array_get_value(grn_ctx *ctx, grn_array *array, grn_id id, void *valuebuf);
GRN_API grn_rc grn_array_set_value(grn_ctx *ctx, grn_array *array, grn_id id,
                                   const void *value, int flags);
GRN_API grn_array_cursor *grn_array_cursor_open(grn_ctx *ctx, grn_array *array,
                                                grn_id min, grn_id max,
                                                int offset, int limit, int flags);
GRN_API grn_id grn_array_cursor_next(grn_ctx *ctx, grn_array_cursor *cursor);
GRN_API int grn_array_cursor_get_value(grn_ctx *ctx, grn_array_cursor *cursor, void **value);
GRN_API grn_rc grn_array_cursor_set_value(grn_ctx *ctx, grn_array_cursor *cursor,
                                          const void *value, int flags);
GRN_API grn_rc grn_array_cursor_delete(grn_ctx *ctx, grn_array_cursor *cursor,
                                       grn_table_delete_optarg *optarg);
GRN_API void grn_array_cursor_close(grn_ctx *ctx, grn_array_cursor *cursor);
GRN_API grn_rc grn_array_delete_by_id(grn_ctx *ctx, grn_array *array, grn_id id,
                                      grn_table_delete_optarg *optarg);

GRN_API grn_id grn_array_next(grn_ctx *ctx, grn_array *array, grn_id id);

GRN_API void *_grn_array_get_value(grn_ctx *ctx, grn_array *array, grn_id id);

#define GRN_ARRAY_EACH(ctx,array,head,tail,id,value,block) do {\
  grn_array_cursor *_sc = grn_array_cursor_open(ctx, array, head, tail, 0, -1, 0); \
  if (_sc) {\
    grn_id id;\
    while ((id = grn_array_cursor_next(ctx, _sc))) {\
      grn_array_cursor_get_value(ctx, _sc, (void **)(value));\
      block\
    }\
    grn_array_cursor_close(ctx, _sc); \
  }\
} while (0)

#define GRN_ARRAY_EACH_BEGIN(ctx, array, cursor, head, tail, id) do {\
  grn_array_cursor *cursor;\
  cursor = grn_array_cursor_open((ctx), (array), (head), (tail), 0, -1, 0);\
  if (cursor) {\
    grn_id id;\
    while ((id = grn_array_cursor_next(ctx, cursor))) {

#define GRN_ARRAY_EACH_END(ctx, cursor)\
    }\
    grn_array_cursor_close(ctx, cursor);\
  }\
} while (0)

#ifdef __cplusplus
}
#endif
