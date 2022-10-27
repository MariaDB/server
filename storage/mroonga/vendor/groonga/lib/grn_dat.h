/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2017 Brazil

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
#include "grn_db.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _grn_dat {
  grn_db_obj obj;
  grn_io *io;
  struct grn_dat_header *header;
  uint32_t file_id;
  grn_encoding encoding;
  void *trie;
  void *old_trie;
  grn_obj *tokenizer;
  grn_obj *normalizer;
  grn_obj token_filters;
  grn_critical_section lock;
  grn_bool is_dirty;
};

struct grn_dat_header {
  uint32_t flags;
  grn_encoding encoding;
  grn_id tokenizer;
  uint32_t file_id;
  grn_id normalizer;
  uint32_t n_dirty_opens;
  uint32_t reserved[234];
};

struct _grn_dat_cursor {
  grn_db_obj obj;
  grn_dat *dat;
  void *cursor;
  const void *key;
  grn_id curr_rec;
};

GRN_API grn_id grn_dat_curr_id(grn_ctx *ctx, grn_dat *dat);

/*
  Currently, grn_dat_truncate() is available if the grn_dat object is
  associated with a file.
 */
GRN_API grn_rc grn_dat_truncate(grn_ctx *ctx, grn_dat *dat);

GRN_API const char *_grn_dat_key(grn_ctx *ctx, grn_dat *dat, grn_id id,
                                 uint32_t *key_size);
GRN_API grn_id grn_dat_next(grn_ctx *ctx, grn_dat *dat, grn_id id);
GRN_API grn_id grn_dat_at(grn_ctx *ctx, grn_dat *dat, grn_id id);

GRN_API grn_rc grn_dat_clear_status_flags(grn_ctx *ctx, grn_dat *dat);

/*
  Currently, grn_dat_repair() is available if the grn_dat object is associated
  with a file.
 */
GRN_API grn_rc grn_dat_repair(grn_ctx *ctx, grn_dat *dat);

GRN_API grn_rc grn_dat_flush(grn_ctx *ctx, grn_dat *dat);

grn_rc grn_dat_dirty(grn_ctx *ctx, grn_dat *dat);
grn_bool grn_dat_is_dirty(grn_ctx *ctx, grn_dat *dat);
grn_rc grn_dat_clean(grn_ctx *ctx, grn_dat *dat);
grn_rc grn_dat_clear_dirty(grn_ctx *ctx, grn_dat *dat);

grn_bool grn_dat_is_corrupt(grn_ctx *ctx, grn_dat *dat);

size_t grn_dat_get_disk_usage(grn_ctx *ctx, grn_dat *dat);

#ifdef __cplusplus
}
#endif
