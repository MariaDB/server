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

typedef struct _grn_dat grn_dat;
typedef struct _grn_dat_cursor grn_dat_cursor;
typedef struct _grn_table_scan_hit grn_dat_scan_hit;

GRN_API int grn_dat_scan(grn_ctx *ctx, grn_dat *dat, const char *str,
                         unsigned int str_size, grn_dat_scan_hit *scan_hits,
                         unsigned int max_num_scan_hits, const char **str_rest);

GRN_API grn_id grn_dat_lcp_search(grn_ctx *ctx, grn_dat *dat,
                          const void *key, unsigned int key_size);

GRN_API grn_dat *grn_dat_create(grn_ctx *ctx, const char *path, unsigned int key_size,
                                unsigned int value_size, unsigned int flags);

GRN_API grn_dat *grn_dat_open(grn_ctx *ctx, const char *path);

GRN_API grn_rc grn_dat_close(grn_ctx *ctx, grn_dat *dat);

GRN_API grn_rc grn_dat_remove(grn_ctx *ctx, const char *path);

GRN_API grn_id grn_dat_get(grn_ctx *ctx, grn_dat *dat, const void *key,
                           unsigned int key_size, void **value);
GRN_API grn_id grn_dat_add(grn_ctx *ctx, grn_dat *dat, const void *key,
                           unsigned int key_size, void **value, int *added);

GRN_API int grn_dat_get_key(grn_ctx *ctx, grn_dat *dat, grn_id id, void *keybuf, int bufsize);
GRN_API int grn_dat_get_key2(grn_ctx *ctx, grn_dat *dat, grn_id id, grn_obj *bulk);

GRN_API grn_rc grn_dat_delete_by_id(grn_ctx *ctx, grn_dat *dat, grn_id id,
                                    grn_table_delete_optarg *optarg);
GRN_API grn_rc grn_dat_delete(grn_ctx *ctx, grn_dat *dat, const void *key, unsigned int key_size,
                              grn_table_delete_optarg *optarg);

GRN_API grn_rc grn_dat_update_by_id(grn_ctx *ctx, grn_dat *dat, grn_id src_key_id,
                                    const void *dest_key, unsigned int dest_key_size);
GRN_API grn_rc grn_dat_update(grn_ctx *ctx, grn_dat *dat,
                              const void *src_key, unsigned int src_key_size,
                              const void *dest_key, unsigned int dest_key_size);

GRN_API unsigned int grn_dat_size(grn_ctx *ctx, grn_dat *dat);

GRN_API grn_dat_cursor *grn_dat_cursor_open(grn_ctx *ctx, grn_dat *dat,
                                            const void *min, unsigned int min_size,
                                            const void *max, unsigned int max_size,
                                            int offset, int limit, int flags);
GRN_API grn_id grn_dat_cursor_next(grn_ctx *ctx, grn_dat_cursor *c);
GRN_API void grn_dat_cursor_close(grn_ctx *ctx, grn_dat_cursor *c);

GRN_API int grn_dat_cursor_get_key(grn_ctx *ctx, grn_dat_cursor *c, const void **key);
GRN_API grn_rc grn_dat_cursor_delete(grn_ctx *ctx, grn_dat_cursor *c,
                                     grn_table_delete_optarg *optarg);

#define GRN_DAT_EACH(ctx,dat,id,key,key_size,block) do {\
  grn_dat_cursor *_sc = grn_dat_cursor_open(ctx, dat, NULL, 0, NULL, 0, 0, -1, 0);\
  if (_sc) {\
    grn_id id;\
    unsigned int *_ks = (key_size);\
    if (_ks) {\
      while ((id = grn_dat_cursor_next(ctx, _sc))) {\
        int _ks_raw = grn_dat_cursor_get_key(ctx, _sc, (const void **)(key));\
        *(_ks) = (unsigned int)_ks_raw;\
        block\
      }\
    } else {\
      while ((id = grn_dat_cursor_next(ctx, _sc))) {\
        grn_dat_cursor_get_key(ctx, _sc, (const void **)(key));\
        block\
      }\
    }\
    grn_dat_cursor_close(ctx, _sc);\
  }\
} while (0)

#ifdef __cplusplus
}
#endif
