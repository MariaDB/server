/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "ts_buf.h"

#include "../grn_ctx.h"

#include "ts_log.h"

#include <string.h>

/*-------------------------------------------------------------
 * grn_ts_buf
 */

void
grn_ts_buf_init(grn_ctx *ctx, grn_ts_buf *buf)
{
  buf->ptr = NULL;
  buf->size = 0;
  buf->pos = 0;
}

/*
grn_rc
grn_ts_buf_open(grn_ctx *ctx, grn_ts_buf **buf)
{
  grn_ts_buf *new_buf = GRN_MALLOCN(grn_ts_buf, 1);
  if (!new_buf) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_buf));
  }
  grn_ts_buf_init(ctx, new_buf);
  *buf = new_buf;
  return GRN_SUCCESS;
}
*/

void
grn_ts_buf_fin(grn_ctx *ctx, grn_ts_buf *buf)
{
  if (buf->ptr) {
    GRN_FREE(buf->ptr);
  }
}

/*
void
grn_ts_buf_close(grn_ctx *ctx, grn_ts_buf *buf)
{
  if (buf) {
    grn_ts_buf_fin(ctx, buf);
  }
}
*/

grn_rc
grn_ts_buf_reserve(grn_ctx *ctx, grn_ts_buf *buf, size_t min_size)
{
  void *new_ptr;
  size_t enough_size;
  if (min_size <= buf->size) {
    return GRN_SUCCESS;
  }
  enough_size = buf->size ? (buf->size << 1) : 1;
  while (enough_size < min_size) {
    if ((enough_size << 1) < enough_size) {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                        "size overflow: %" GRN_FMT_SIZE,
                        min_size);
    }
    enough_size <<= 1;
  }
  new_ptr = GRN_REALLOC(buf->ptr, enough_size);
  if (!new_ptr) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                      enough_size);
  }
  buf->ptr = new_ptr;
  buf->size = enough_size;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_buf_resize(grn_ctx *ctx, grn_ts_buf *buf, size_t new_size)
{
  void *new_ptr;
  if (new_size == buf->size) {
    return GRN_SUCCESS;
  }
  if (!new_size) {
    if (buf->ptr) {
      GRN_FREE(buf->ptr);
      buf->ptr = NULL;
      buf->size = new_size;
    }
    return GRN_SUCCESS;
  }
  new_ptr = GRN_REALLOC(buf->ptr, new_size);
  if (!new_ptr) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                      new_size);
  }
  buf->ptr = new_ptr;
  buf->size = new_size;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_buf_write(grn_ctx *ctx, grn_ts_buf *buf, const void *ptr, size_t size)
{
  size_t new_pos = buf->pos + size;
  if (new_pos < buf->pos) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "size overflow: %" GRN_FMT_SIZE " + %" GRN_FMT_SIZE,
                      buf->pos, size);
  }
  if (new_pos > buf->size) {
    grn_rc rc = grn_ts_buf_reserve(ctx, buf, new_pos);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  grn_memcpy((char *)buf->ptr + buf->pos, ptr, size);
  buf->pos += size;
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_rbuf
 */

void
grn_ts_rbuf_init(grn_ctx *ctx, grn_ts_rbuf *rbuf)
{
  rbuf->recs = NULL;
  rbuf->n_recs = 0;
  rbuf->max_n_recs = 0;
}

void
grn_ts_rbuf_fin(grn_ctx *ctx, grn_ts_rbuf *rbuf)
{
  if (rbuf->recs) {
    GRN_FREE(rbuf->recs);
  }
}

grn_rc
grn_ts_rbuf_open(grn_ctx *ctx, grn_ts_rbuf **rbuf)
{
  grn_ts_rbuf *new_rbuf = GRN_MALLOCN(grn_ts_rbuf, 1);
  if (!new_rbuf) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_rbuf));
  }
  grn_ts_rbuf_init(ctx, new_rbuf);
  *rbuf = new_rbuf;
  return GRN_SUCCESS;
}

void
grn_ts_rbuf_close(grn_ctx *ctx, grn_ts_rbuf *rbuf)
{
  if (rbuf) {
    grn_ts_rbuf_fin(ctx, rbuf);
  }
}

grn_rc
grn_ts_rbuf_reserve(grn_ctx *ctx, grn_ts_rbuf *rbuf, size_t min_max_n_recs)
{
  size_t n_bytes, enough_max_n_recs;
  grn_ts_record *new_recs;
  if (min_max_n_recs <= rbuf->max_n_recs) {
    return GRN_SUCCESS;
  }
  enough_max_n_recs = rbuf->max_n_recs ? (rbuf->max_n_recs << 1) : 1;
  while (enough_max_n_recs < min_max_n_recs) {
    if ((enough_max_n_recs << 1) < enough_max_n_recs) {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                        "size overflow: %" GRN_FMT_SIZE,
                        min_max_n_recs);
    }
    enough_max_n_recs <<= 1;
  }
  n_bytes = sizeof(grn_ts_record) * enough_max_n_recs;
  new_recs = GRN_REALLOC(rbuf->recs, n_bytes);
  if (!new_recs) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                      n_bytes);
  }
  rbuf->recs = new_recs;
  rbuf->max_n_recs = enough_max_n_recs;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_rbuf_resize(grn_ctx *ctx, grn_ts_rbuf *rbuf, size_t new_max_n_recs)
{
  size_t n_bytes;
  grn_ts_record *new_recs;
  if (new_max_n_recs == rbuf->max_n_recs) {
    return GRN_SUCCESS;
  }
  if (!new_max_n_recs) {
    if (rbuf->recs) {
      GRN_FREE(rbuf->recs);
      rbuf->recs = NULL;
      rbuf->max_n_recs = new_max_n_recs;
    }
    return GRN_SUCCESS;
  }
  n_bytes = sizeof(grn_ts_record) * new_max_n_recs;
  new_recs = GRN_REALLOC(rbuf->recs, n_bytes);
  if (!new_recs) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                      new_max_n_recs);
  }
  rbuf->recs = new_recs;
  rbuf->max_n_recs = new_max_n_recs;
  return GRN_SUCCESS;
}
