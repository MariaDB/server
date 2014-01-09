/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "wsrep_binlog.h"
#include "wsrep_priv.h"

/*
  Write the contents of a cache to a memory buffer.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.
 */
int wsrep_write_cache_buf(IO_CACHE *cache, uchar **buf, size_t *buf_len)
{
  *buf= NULL;
  *buf_len= 0;

  my_off_t const saved_pos(my_b_tell(cache));

  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
  {
    WSREP_ERROR("failed to initialize io-cache");
    return ER_ERROR_ON_WRITE;
  }

  uint length = my_b_bytes_in_cache(cache);
  if (unlikely(0 == length)) length = my_b_fill(cache);

  size_t total_length = 0;

  if (likely(length > 0)) do
  {
      total_length += length;
      /*
        Bail out if buffer grows too large.
        A temporary fix to avoid allocating indefinitely large buffer,
        not a real limit on a writeset size which includes other things
        like header and keys.
      */
      if (total_length > wsrep_max_ws_size)
      {
          WSREP_WARN("transaction size limit (%lu) exceeded: %zu",
                     wsrep_max_ws_size, total_length);
          goto error;
      }

      uchar* tmp = (uchar *)my_realloc(*buf, total_length, MYF(0));
      if (!tmp)
      {
          WSREP_ERROR("could not (re)allocate buffer: %zu + %u",
                      *buf_len, length);
          goto error;
      }
      *buf = tmp;

      memcpy(*buf + *buf_len, cache->read_pos, length);
      *buf_len = total_length;
      cache->read_pos = cache->read_end;
  } while ((cache->file >= 0) && (length = my_b_fill(cache)));

  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_WARN("failed to initialize io-cache");
    goto cleanup;
  }

  return 0;

error:
  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_ERROR("failed to initialize io-cache");
  }
cleanup:
  my_free(*buf);
  *buf= NULL;
  *buf_len= 0;
  return ER_ERROR_ON_WRITE;
}

#define STACK_SIZE 4096 /* 4K - for buffer preallocated on the stack:
                         * many transactions would fit in there
                         * so there is no need to reach for the heap */

/* Returns minimum multiple of HEAP_PAGE_SIZE that is >= length */
static inline size_t
heap_size(size_t length)
{
    return (length + HEAP_PAGE_SIZE - 1)/HEAP_PAGE_SIZE*HEAP_PAGE_SIZE;
}

/* append data to writeset */
static inline wsrep_status_t
wsrep_append_data(wsrep_t*           const wsrep,
                  wsrep_ws_handle_t* const ws,
                  const void*        const data,
                  size_t             const len)
{
    struct wsrep_buf const buff = { data, len };
    wsrep_status_t const rc(wsrep->append_data(wsrep, ws, &buff, 1,
                                               WSREP_DATA_ORDERED, true));
    if (rc != WSREP_OK)
    {
        WSREP_WARN("append_data() returned %d", rc);
    }

    return rc;
}

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.

  This version reads all of cache into single buffer and then appends to a
  writeset at once.
 */
static int wsrep_write_cache_once(wsrep_t*  const wsrep,
                                  THD*      const thd,
                                  IO_CACHE* const cache,
                                  size_t*   const len)
{
    my_off_t const saved_pos(my_b_tell(cache));

    if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
    {
        WSREP_ERROR("failed to initialize io-cache");
        return ER_ERROR_ON_WRITE;
    }

    int err(WSREP_OK);

    size_t total_length(0);
    uchar  stack_buf[STACK_SIZE]; /* to avoid dynamic allocations for few data*/
    uchar* heap_buf(NULL);
    uchar* buf(stack_buf);
    size_t allocated(sizeof(stack_buf));
    size_t used(0);

    uint length(my_b_bytes_in_cache(cache));
    if (unlikely(0 == length)) length = my_b_fill(cache);

    if (likely(length > 0)) do
    {
        total_length += length;
        /*
          Bail out if buffer grows too large.
          A temporary fix to avoid allocating indefinitely large buffer,
          not a real limit on a writeset size which includes other things
          like header and keys.
        */
        if (unlikely(total_length > wsrep_max_ws_size))
        {
            WSREP_WARN("transaction size limit (%lu) exceeded: %zu",
                       wsrep_max_ws_size, total_length);
            goto cleanup;
        }

        if (total_length > allocated)
        {
            size_t const new_size(heap_size(total_length));
            uchar* tmp = (uchar *)my_realloc(heap_buf, new_size, MYF(0));
            if (!tmp)
            {
                WSREP_ERROR("could not (re)allocate buffer: %zu + %u",
                            allocated, length);
                err = WSREP_SIZE_EXCEEDED;
                goto cleanup;
            }

            heap_buf = tmp;
            buf = heap_buf;
            allocated = new_size;

            if (used <= STACK_SIZE && used > 0) // there's data in stack_buf
            {
                DBUG_ASSERT(buf == stack_buf);
                memcpy(heap_buf, stack_buf, used);
            }
        }

        memcpy(buf + used, cache->read_pos, length);
        used = total_length;
        cache->read_pos = cache->read_end;
    } while ((cache->file >= 0) && (length = my_b_fill(cache)));

    if (used > 0)
        err = wsrep_append_data(wsrep, &thd->wsrep_ws_handle, buf, used);

    if (WSREP_OK == err) *len = total_length;

cleanup:
    if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
    {
        WSREP_ERROR("failed to reinitialize io-cache");
    }

    if (unlikely(WSREP_OK != err)) wsrep_dump_rbr_buf(thd, buf, used);

    my_free(heap_buf);
    return err;
}

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.

  This version uses incremental data appending as it reads it from cache.
 */
static int wsrep_write_cache_inc(wsrep_t*  const wsrep,
                                 THD*      const thd,
                                 IO_CACHE* const cache,
                                 size_t*   const len)
{
    my_off_t const saved_pos(my_b_tell(cache));

    if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
    {
      WSREP_ERROR("failed to initialize io-cache");
      return WSREP_TRX_ROLLBACK;
    }

    int err(WSREP_OK);

    size_t total_length(0);

    uint length(my_b_bytes_in_cache(cache));
    if (unlikely(0 == length)) length = my_b_fill(cache);

    if (likely(length > 0)) do
    {
        total_length += length;
        /* bail out if buffer grows too large
           not a real limit on a writeset size which includes other things
           like header and keys.
        */
        if (unlikely(total_length > wsrep_max_ws_size))
        {
            WSREP_WARN("transaction size limit (%lu) exceeded: %zu",
                       wsrep_max_ws_size, total_length);
            err = WSREP_SIZE_EXCEEDED;
            goto cleanup;
        }

        if(WSREP_OK != (err=wsrep_append_data(wsrep, &thd->wsrep_ws_handle,
                                              cache->read_pos, length)))
                goto cleanup;

        cache->read_pos = cache->read_end;
    } while ((cache->file >= 0) && (length = my_b_fill(cache)));

    if (WSREP_OK == err) *len = total_length;

cleanup:
    if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
    {
        WSREP_ERROR("failed to reinitialize io-cache");
    }

    return err;
}

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.
 */
int wsrep_write_cache(wsrep_t*  const wsrep,
                      THD*      const thd,
                      IO_CACHE* const cache,
                      size_t*   const len)
{
    if (wsrep_incremental_data_collection) {
        return wsrep_write_cache_inc(wsrep, thd, cache, len);
    }
    else {
        return wsrep_write_cache_once(wsrep, thd, cache, len);
    }
}

void wsrep_dump_rbr_buf(THD *thd, const void* rbr_buf, size_t buf_len)
{
  char filename[PATH_MAX]= {0};
  int len= snprintf(filename, PATH_MAX, "%s/GRA_%ld_%lld.log",
                    wsrep_data_home_dir, thd->thread_id,
                    (long long)wsrep_thd_trx_seqno(thd));
  if (len >= PATH_MAX)
  {
    WSREP_ERROR("RBR dump path too long: %d, skipping dump.", len);
    return;
  }

  FILE *of= fopen(filename, "wb");
  if (of)
  {
    fwrite (rbr_buf, buf_len, 1, of);
    fclose(of);
  }
  else
  {
    WSREP_ERROR("Failed to open file '%s': %d (%s)",
                filename, errno, strerror(errno));
  }
}

