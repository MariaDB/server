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

#define GRN_HASH_TINY         (0x01<<6)

typedef struct _grn_hash grn_hash;
typedef struct _grn_hash_cursor grn_hash_cursor;

GRN_API grn_hash *grn_hash_create(grn_ctx *ctx, const char *path, unsigned int key_size,
                                  unsigned int value_size, unsigned int flags);

GRN_API grn_hash *grn_hash_open(grn_ctx *ctx, const char *path);

GRN_API grn_rc grn_hash_close(grn_ctx *ctx, grn_hash *hash);

GRN_API grn_id grn_hash_add(grn_ctx *ctx, grn_hash *hash, const void *key,
                            unsigned int key_size, void **value, int *added);
GRN_API grn_id grn_hash_get(grn_ctx *ctx, grn_hash *hash, const void *key,
                            unsigned int key_size, void **value);

GRN_API int grn_hash_get_key(grn_ctx *ctx, grn_hash *hash, grn_id id, void *keybuf, int bufsize);
GRN_API int grn_hash_get_key2(grn_ctx *ctx, grn_hash *hash, grn_id id, grn_obj *bulk);
GRN_API int grn_hash_get_value(grn_ctx *ctx, grn_hash *hash, grn_id id, void *valuebuf);
GRN_API grn_rc grn_hash_set_value(grn_ctx *ctx, grn_hash *hash, grn_id id,
                                  const void *value, int flags);

GRN_API grn_rc grn_hash_delete_by_id(grn_ctx *ctx, grn_hash *hash, grn_id id,
                                     grn_table_delete_optarg *optarg);
GRN_API grn_rc grn_hash_delete(grn_ctx *ctx, grn_hash *hash,
                               const void *key, unsigned int key_size,
                               grn_table_delete_optarg *optarg);

GRN_API uint32_t grn_hash_size(grn_ctx *ctx, grn_hash *hash);

GRN_API grn_hash_cursor *grn_hash_cursor_open(grn_ctx *ctx, grn_hash *hash,
                                              const void *min, unsigned int min_size,
                                              const void *max, unsigned int max_size,
                                              int offset, int limit, int flags);
GRN_API grn_id grn_hash_cursor_next(grn_ctx *ctx, grn_hash_cursor *c);
GRN_API void grn_hash_cursor_close(grn_ctx *ctx, grn_hash_cursor *c);

GRN_API int grn_hash_cursor_get_key(grn_ctx *ctx, grn_hash_cursor *c, void **key);
GRN_API int grn_hash_cursor_get_value(grn_ctx *ctx, grn_hash_cursor *c, void **value);
GRN_API grn_rc grn_hash_cursor_set_value(grn_ctx *ctx, grn_hash_cursor *c,
                                         const void *value, int flags);

GRN_API int grn_hash_cursor_get_key_value(grn_ctx *ctx, grn_hash_cursor *c,
                                          void **key, unsigned int *key_size, void **value);

GRN_API grn_rc grn_hash_cursor_delete(grn_ctx *ctx, grn_hash_cursor *c,
                                      grn_table_delete_optarg *optarg);

#define GRN_HASH_EACH(ctx,hash,id,key,key_size,value,block) do {\
  grn_hash_cursor *_sc = grn_hash_cursor_open(ctx, hash, NULL, 0, NULL, 0, 0, -1, 0); \
  if (_sc) {\
    grn_id id;\
    while ((id = grn_hash_cursor_next(ctx, _sc))) {\
      grn_hash_cursor_get_key_value(ctx, _sc, (void **)(key),\
                                    (key_size), (void **)(value));\
      block\
    }\
    grn_hash_cursor_close(ctx, _sc);\
  }\
} while (0)

#define GRN_HASH_EACH_BEGIN(ctx, hash, cursor, id) do {\
  grn_hash_cursor *cursor;\
  cursor = grn_hash_cursor_open((ctx), (hash),\
                                NULL, 0, NULL, 0,\
                                0, -1, GRN_CURSOR_BY_ID);\
  if (cursor) {\
    grn_id id;\
    while ((id = grn_hash_cursor_next((ctx), cursor)) != GRN_ID_NIL) {

#define GRN_HASH_EACH_END(ctx, cursor)\
    }\
    grn_hash_cursor_close((ctx), cursor);\
  }\
} while(0)

#ifdef __cplusplus
}
#endif
