/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "../grn.h"

#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------
 * grn_ts_buf
 */

/* grn_ts_buf works as a buffer for arbitrary data. */

typedef struct {
  void *ptr;   /* The starting address. */
  size_t size; /* The size in bytes. */
  size_t pos;  /* The current position for grn_ts_buf_write(). */
} grn_ts_buf;

/* grn_ts_buf_init() initializes a buffer. */
void grn_ts_buf_init(grn_ctx *ctx, grn_ts_buf *buf);

/* grn_ts_buf_fin() finalizes a buffer. */
void grn_ts_buf_fin(grn_ctx *ctx, grn_ts_buf *buf);

#if 0
/* grn_ts_buf_open() creates a buffer. */
grn_rc grn_ts_buf_open(grn_ctx *ctx, grn_ts_buf **buf);

/* grn_ts_buf_close() destroys a buffer. */
void grn_ts_buf_close(grn_ctx *ctx, grn_ts_buf *buf);
#endif

/*
 * grn_ts_buf_reserve() reserves enough memory to store `min_size` bytes.
 * Note that this function never shrinks a buffer and does nothing if
 * `min_size` is not greater than `buf->size`.
 */
grn_rc grn_ts_buf_reserve(grn_ctx *ctx, grn_ts_buf *buf, size_t min_size);

/* grn_ts_buf_resize() resizes a buffer. */
grn_rc grn_ts_buf_resize(grn_ctx *ctx, grn_ts_buf *buf, size_t new_size);

/*
 * grn_ts_buf_write() writes data into a buffer. `buf->pos` specifies the
 * position and it will be modified on success.
 * Note that this function resizes a buffer if required.
 */
grn_rc grn_ts_buf_write(grn_ctx *ctx, grn_ts_buf *buf,
                        const void *ptr, size_t size);

/*-------------------------------------------------------------
 * grn_ts_rbuf
 */

/* grn_ts_rbuf works as a buffer for records. */

typedef struct {
  grn_ts_record *recs; /* Pointer to records. */
  size_t n_recs;       /* The number of records. */
  size_t max_n_recs;   /* The maximum number of records. */
} grn_ts_rbuf;

/* grn_ts_rbuf_init() initializes a buffer. */
void grn_ts_rbuf_init(grn_ctx *ctx, grn_ts_rbuf *rbuf);

/* grn_ts_rbuf_fin() finalizes a buffer. */
void grn_ts_rbuf_fin(grn_ctx *ctx, grn_ts_rbuf *rbuf);

/* grn_ts_rbuf_open() creates a buffer. */
/*grn_rc grn_ts_rbuf_open(grn_ctx *ctx, grn_ts_rbuf **rbuf);*/

/* grn_ts_rbuf_close() destroys a buffer. */
/*void grn_ts_rbuf_close(grn_ctx *ctx, grn_ts_rbuf *rbuf);*/

/*
 * grn_ts_rbuf_reserve() reserves enough memory to store `n_recs` records.
 * Note that this function never shrinks a buffer and does nothing if `n_recs`
 * is not greater than the `rbuf->max_n_recs`.
 */
grn_rc grn_ts_rbuf_reserve(grn_ctx *ctx, grn_ts_rbuf *rbuf, size_t n_recs);

/* grn_ts_rbuf_resize() resizes a buffer. */
grn_rc grn_ts_rbuf_resize(grn_ctx *ctx, grn_ts_rbuf *rbuf,
                          size_t new_max_n_recs);

#ifdef __cplusplus
}
#endif

