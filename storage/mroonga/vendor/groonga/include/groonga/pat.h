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

#include "hash.h"

#ifdef  __cplusplus
extern "C" {
#endif

typedef struct _grn_pat grn_pat;
typedef struct _grn_pat_cursor grn_pat_cursor;
typedef struct _grn_table_scan_hit grn_pat_scan_hit;

GRN_API grn_pat *grn_pat_create(grn_ctx *ctx, const char *path, unsigned int key_size,
                                unsigned int value_size, unsigned int flags);

GRN_API grn_pat *grn_pat_open(grn_ctx *ctx, const char *path);

GRN_API grn_rc grn_pat_close(grn_ctx *ctx, grn_pat *pat);

GRN_API grn_rc grn_pat_remove(grn_ctx *ctx, const char *path);

GRN_API grn_id grn_pat_get(grn_ctx *ctx, grn_pat *pat, const void *key,
                           unsigned int key_size, void **value);
GRN_API grn_id grn_pat_add(grn_ctx *ctx, grn_pat *pat, const void *key,
                           unsigned int key_size, void **value, int *added);

GRN_API int grn_pat_get_key(grn_ctx *ctx, grn_pat *pat, grn_id id, void *keybuf, int bufsize);
GRN_API int grn_pat_get_key2(grn_ctx *ctx, grn_pat *pat, grn_id id, grn_obj *bulk);
GRN_API int grn_pat_get_value(grn_ctx *ctx, grn_pat *pat, grn_id id, void *valuebuf);
GRN_API grn_rc grn_pat_set_value(grn_ctx *ctx, grn_pat *pat, grn_id id,
                                 const void *value, int flags);

GRN_API grn_rc grn_pat_delete_by_id(grn_ctx *ctx, grn_pat *pat, grn_id id,
                                    grn_table_delete_optarg *optarg);
GRN_API grn_rc grn_pat_delete(grn_ctx *ctx, grn_pat *pat, const void *key, unsigned int key_size,
                              grn_table_delete_optarg *optarg);
GRN_API int grn_pat_delete_with_sis(grn_ctx *ctx, grn_pat *pat, grn_id id,
                                    grn_table_delete_optarg *optarg);

GRN_API int grn_pat_scan(grn_ctx *ctx, grn_pat *pat, const char *str, unsigned int str_len,
                         grn_pat_scan_hit *sh, unsigned int sh_size, const char **rest);

GRN_API grn_rc grn_pat_prefix_search(grn_ctx *ctx, grn_pat *pat,
                                     const void *key, unsigned int key_size, grn_hash *h);
GRN_API grn_rc grn_pat_suffix_search(grn_ctx *ctx, grn_pat *pat,
                                     const void *key, unsigned int key_size, grn_hash *h);
GRN_API grn_id grn_pat_lcp_search(grn_ctx *ctx, grn_pat *pat,
                                  const void *key, unsigned int key_size);

GRN_API unsigned int grn_pat_size(grn_ctx *ctx, grn_pat *pat);

GRN_API grn_pat_cursor *grn_pat_cursor_open(grn_ctx *ctx, grn_pat *pat,
                                            const void *min, unsigned int min_size,
                                            const void *max, unsigned int max_size,
                                            int offset, int limit, int flags);
GRN_API grn_id grn_pat_cursor_next(grn_ctx *ctx, grn_pat_cursor *c);
GRN_API void grn_pat_cursor_close(grn_ctx *ctx, grn_pat_cursor *c);

GRN_API int grn_pat_cursor_get_key(grn_ctx *ctx, grn_pat_cursor *c, void **key);
GRN_API int grn_pat_cursor_get_value(grn_ctx *ctx, grn_pat_cursor *c, void **value);

GRN_API int grn_pat_cursor_get_key_value(grn_ctx *ctx, grn_pat_cursor *c,
                                         void **key, unsigned int *key_size, void **value);
GRN_API grn_rc grn_pat_cursor_set_value(grn_ctx *ctx, grn_pat_cursor *c,
                                        const void *value, int flags);
GRN_API grn_rc grn_pat_cursor_delete(grn_ctx *ctx, grn_pat_cursor *c,
                                     grn_table_delete_optarg *optarg);

#define GRN_PAT_EACH(ctx,pat,id,key,key_size,value,block) do {          \
  grn_pat_cursor *_sc = grn_pat_cursor_open(ctx, pat, NULL, 0, NULL, 0, 0, -1, 0); \
  if (_sc) {\
    grn_id id;\
    while ((id = grn_pat_cursor_next(ctx, _sc))) {\
      grn_pat_cursor_get_key_value(ctx, _sc, (void **)(key),\
                                   (key_size), (void **)(value));\
      block\
    }\
    grn_pat_cursor_close(ctx, _sc);\
  }\
} while (0)

#ifdef __cplusplus
}
#endif
