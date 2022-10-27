/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#ifdef __cplusplus
extern "C" {
#endif

#define GRN_CACHE_MAX_KEY_SIZE GRN_HASH_MAX_KEY_SIZE_LARGE

typedef struct {
  uint32_t nentries;
  uint32_t max_nentries;
  uint32_t nfetches;
  uint32_t nhits;
} grn_cache_statistics;

void grn_cache_init(void);
grn_rc grn_cache_fetch(grn_ctx *ctx, grn_cache *cache,
                       const char *str, uint32_t str_size,
                       grn_obj *output);
void grn_cache_update(grn_ctx *ctx, grn_cache *cache,
                      const char *str, uint32_t str_size, grn_obj *value);
void grn_cache_expire(grn_cache *cache, int32_t size);
void grn_cache_fin(void);
void grn_cache_get_statistics(grn_ctx *ctx, grn_cache *cache,
                              grn_cache_statistics *statistics);

#ifdef __cplusplus
}
#endif
