/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2012 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef GRN_STORE_H
#define GRN_STORE_H

#include "grn.h"
#include "grn_ctx.h"
#include "grn_hash.h"
#include "grn_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**** fixed sized elements ****/

typedef struct _grn_ra grn_ra;

struct _grn_ra {
  grn_db_obj obj;
  grn_io *io;
  int element_width;
  int element_mask;
  struct grn_ra_header *header;
};

struct grn_ra_header {
  uint32_t element_size;
  uint32_t nrecords; /* nrecords is not maintained by default */
  uint32_t reserved[10];
};

grn_ra *grn_ra_create(grn_ctx *ctx, const char *path, unsigned int element_size);
grn_ra *grn_ra_open(grn_ctx *ctx, const char *path);
grn_rc grn_ra_info(grn_ctx *ctx, grn_ra *ra, unsigned int *element_size);
grn_rc grn_ra_close(grn_ctx *ctx, grn_ra *ra);
grn_rc grn_ra_remove(grn_ctx *ctx, const char *path);
void *grn_ra_ref(grn_ctx *ctx, grn_ra *ra, grn_id id);
grn_rc grn_ra_unref(grn_ctx *ctx, grn_ra *ra, grn_id id);

typedef struct _grn_ra_cache grn_ra_cache;

struct _grn_ra_cache {
  void *p;
  int32_t seg;
};

#define GRN_RA_CACHE_INIT(ra,c) do {\
  (c)->p = NULL; (c)->seg = -1;\
} while (0)

#define GRN_RA_CACHE_FIN(ra,c) do {\
  if ((c)->seg != -1) { GRN_IO_SEG_UNREF((ra)->io, (c)->seg); }\
} while (0);

void *grn_ra_ref_cache(grn_ctx *ctx, grn_ra *ra, grn_id id, grn_ra_cache *cache);

/**** variable sized elements ****/

extern grn_bool grn_ja_skip_same_value_put;

typedef struct _grn_ja grn_ja;

struct _grn_ja {
  grn_db_obj obj;
  grn_io *io;
  struct grn_ja_header *header;
};

GRN_API grn_ja *grn_ja_create(grn_ctx *ctx, const char *path,
                              uint32_t max_element_size, uint32_t flags);
grn_ja *grn_ja_open(grn_ctx *ctx, const char *path);
grn_rc grn_ja_info(grn_ctx *ctx, grn_ja *ja, unsigned int *max_element_size);
GRN_API grn_rc grn_ja_close(grn_ctx *ctx, grn_ja *ja);
grn_rc grn_ja_remove(grn_ctx *ctx, const char *path);
grn_rc grn_ja_put(grn_ctx *ctx, grn_ja *ja, grn_id id,
                  void *value, uint32_t value_len, int flags, uint64_t *cas);
int grn_ja_at(grn_ctx *ctx, grn_ja *ja, grn_id id, void *valbuf, int buf_size);

GRN_API void *grn_ja_ref(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_io_win *iw,
                         uint32_t *value_len);
grn_obj *grn_ja_get_value(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_obj *value);

GRN_API grn_rc grn_ja_unref(grn_ctx *ctx, grn_io_win *iw);
int grn_ja_defrag(grn_ctx *ctx, grn_ja *ja, int threshold);

GRN_API grn_rc grn_ja_putv(grn_ctx *ctx, grn_ja *ja, grn_id id,
                           grn_obj *vector, int flags);
GRN_API uint32_t grn_ja_size(grn_ctx *ctx, grn_ja *ja, grn_id id);

void grn_ja_check(grn_ctx *ctx, grn_ja *ja);

/*

typedef struct _grn_vgram_vnode
{
  struct _grn_vgram_vnode *car;
  struct _grn_vgram_vnode *cdr;
  grn_id tid;
  grn_id vid;
  int freq;
  int len;
} grn_vgram_vnode;

typedef struct _grn_vgram grn_vgram;
struct _grn_vgram {
  void *vgram;
};

struct _grn_vgram_buf {
  size_t len;
  grn_id *tvs;
  grn_id *tvp;
  grn_id *tve;
  grn_vgram_vnode *vps;
  grn_vgram_vnode *vpp;
  grn_vgram_vnode *vpe;
};

grn_vgram *grn_vgram_create(const char *path);
grn_vgram *grn_vgram_open(const char *path);
grn_rc grn_vgram_close(grn_vgram *vgram);
grn_rc grn_vgram_update(grn_vgram *vgram, grn_id rid, grn_vgram_buf *b, grn_hash *terms);

grn_vgram_buf *grn_vgram_buf_open(size_t len);
grn_rc grn_vgram_buf_add(grn_vgram_buf *b, grn_id tid);
grn_rc grn_vgram_buf_close(grn_vgram_buf *b);

*/

#ifdef __cplusplus
}
#endif

#endif /* GRN_STORE_H */
