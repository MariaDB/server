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
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#include "wsrep_binlog.h"
#include "wsrep_priv.h"
#include "log.h"
#include "log_event.h"
#include "wsrep_applier.h"

extern handlerton *binlog_hton;
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
      uchar* tmp = (uchar *)my_realloc(*buf, total_length,
                                       MYF(MY_ALLOW_ZERO_PTR));
      if (!tmp)
      {
          WSREP_ERROR("could not (re)allocate buffer: %zu + %u",
                      *buf_len, length);
          goto error;
      }
      *buf = tmp;

      memcpy(*buf + *buf_len, cache->read_pos, length);
      *buf_len = total_length;

      if (cache->file < 0)
      {
        cache->read_pos= cache->read_end;
        break;
      }
  } while ((length = my_b_fill(cache)));

  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_WARN("failed to initialize io-cache");
    goto cleanup;
  }

  return 0;

error:
  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_WARN("failed to initialize io-cache");
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
	    err = WSREP_TRX_SIZE_EXCEEDED;
            goto cleanup;
        }

        if (total_length > allocated)
        {
            size_t const new_size(heap_size(total_length));
            uchar* tmp = (uchar *)my_realloc(heap_buf, new_size,
                                             MYF(MY_ALLOW_ZERO_PTR));
            if (!tmp)
            {
                WSREP_ERROR("could not (re)allocate buffer: %zu + %u",
                            allocated, length);
                err = WSREP_TRX_SIZE_EXCEEDED;
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
        if (cache->file < 0)
        {
          cache->read_pos= cache->read_end;
          break;
        }
    } while ((length = my_b_fill(cache)));

    if (used > 0)
        err = wsrep_append_data(wsrep, &thd->wsrep_ws_handle, buf, used);

    if (WSREP_OK == err) *len = total_length;

cleanup:
    if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
    {
        WSREP_ERROR("failed to reinitialize io-cache");
    }

    if (unlikely(WSREP_OK != err))
    {
      wsrep_dump_rbr_buf_with_header(thd, buf, used);
    }

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
      return WSREP_TRX_ERROR;
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
            err = WSREP_TRX_SIZE_EXCEEDED;
            goto cleanup;
        }

        if(WSREP_OK != (err=wsrep_append_data(wsrep, &thd->wsrep_ws_handle,
                                              cache->read_pos, length)))
                goto cleanup;

        if (cache->file < 0)
        {
          cache->read_pos= cache->read_end;
          break;
        }
    } while ((length = my_b_fill(cache)));

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
  int len= snprintf(NULL, 0, "%s/GRA_%lld_%lld.log",
                    wsrep_data_home_dir, (longlong) thd->thread_id,
                    (longlong) wsrep_thd_trx_seqno(thd));
  if (len < 0)
  {
    WSREP_ERROR("snprintf error: %d, skipping dump.", len);
    return;
  }
  /*
    len doesn't count the \0 end-of-string. Use len+1 below
    to alloc and pass as an argument to snprintf.
  */

  char *filename= (char *)malloc(len+1);
  int len1= snprintf(filename, len+1, "%s/GRA_%lld_%lld.log",
                    wsrep_data_home_dir, (longlong) thd->thread_id,
                    (long long)wsrep_thd_trx_seqno(thd));

  if (len > len1)
  {
    WSREP_ERROR("RBR dump path truncated: %d, skipping dump.", len);
    free(filename);
    return;
  }

  FILE *of= fopen(filename, "wb");

  if (of)
  {
    if (fwrite(rbr_buf, buf_len, 1, of) == 0)
       WSREP_ERROR("Failed to write buffer of length %llu to '%s'",
                   (unsigned long long)buf_len, filename);

    fclose(of);
  }
  else
  {
    WSREP_ERROR("Failed to open file '%s': %d (%s)",
                filename, errno, strerror(errno));
  }
  free(filename);
}

/*
  wsrep exploits binlog's caches even if binlogging itself is not
  activated. In such case connection close needs calling
  actual binlog's method.
  Todo: split binlog hton from its caches to use ones by wsrep
  without referring to binlog's stuff.
*/
int wsrep_binlog_close_connection(THD* thd)
{
  DBUG_ENTER("wsrep_binlog_close_connection");
  if (thd_get_ha_data(thd, binlog_hton) != NULL)
    binlog_hton->close_connection (binlog_hton, thd);
  DBUG_RETURN(0);
}

#if 0
void wsrep_dump_rbr_direct(THD* thd, IO_CACHE* cache)
{
  char filename[PATH_MAX]= {0};
  int len= snprintf(filename, PATH_MAX, "%s/GRA_%lld_%lld.log",
                    wsrep_data_home_dir, (longlong) thd->thread_id,
                    (longlong) wsrep_thd_trx_seqno(thd));
  size_t bytes_in_cache = 0;
  // check path
  if (len >= PATH_MAX)
  {
    WSREP_ERROR("RBR dump path too long: %d, skipping dump.", len);
    return ;
  }
  // init cache
  my_off_t const saved_pos(my_b_tell(cache));
  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
  {
    WSREP_ERROR("failed to initialize io-cache");
    return ;
  }
  // open file
  FILE* of = fopen(filename, "wb");
  if (!of)
  {
    WSREP_ERROR("Failed to open file '%s': %d (%s)",
                filename, errno, strerror(errno));
    goto cleanup;
  }
  // ready to write
  bytes_in_cache= my_b_bytes_in_cache(cache);
  if (unlikely(bytes_in_cache == 0)) bytes_in_cache = my_b_fill(cache);
  if (likely(bytes_in_cache > 0)) do
  {
    if (my_fwrite(of, cache->read_pos, bytes_in_cache,
                  MYF(MY_WME | MY_NABP)) == (size_t) -1)
    {
      WSREP_ERROR("Failed to write file '%s'", filename);
      goto cleanup;
    }

    if (cache->file < 0)
    {
      cache->read_pos= cache->read_end;
      break;
    }
  } while ((bytes_in_cache= my_b_fill(cache)));
  if(cache->error == -1)
  {
    WSREP_ERROR("RBR inconsistent");
    goto cleanup;
  }
cleanup:
  // init back
  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_ERROR("failed to reinitialize io-cache");
  }
  // close file
  if (of) fclose(of);
}
#endif

void thd_binlog_flush_pending_rows_event(THD *thd, bool stmt_end)
{
  thd->binlog_flush_pending_rows_event(stmt_end);
}

/* Dump replication buffer along with header to a file. */
void wsrep_dump_rbr_buf_with_header(THD *thd, const void *rbr_buf,
                                    size_t buf_len)
{
  DBUG_ENTER("wsrep_dump_rbr_buf_with_header");

  File file;
  IO_CACHE cache;
  Log_event_writer writer(&cache);
  Format_description_log_event *ev=NULL;

  longlong thd_trx_seqno= (long long)wsrep_thd_trx_seqno(thd);
  int len= snprintf(NULL, 0, "%s/GRA_%lld_%lld_v2.log",
                    wsrep_data_home_dir, (longlong)thd->thread_id,
                    thd_trx_seqno);
  /*
    len doesn't count the \0 end-of-string. Use len+1 below
    to alloc and pass as an argument to snprintf.
  */
  char *filename;
  if (len < 0 || !(filename= (char*)malloc(len+1)))
  {
    WSREP_ERROR("snprintf error: %d, skipping dump.", len);
    DBUG_VOID_RETURN;
  }

  int len1= snprintf(filename, len+1, "%s/GRA_%lld_%lld_v2.log",
                     wsrep_data_home_dir, (longlong) thd->thread_id,
                     thd_trx_seqno);

  if (len > len1)
  {
    WSREP_ERROR("RBR dump path truncated: %d, skipping dump.", len);
    free(filename);
    DBUG_VOID_RETURN;
  }

  if ((file= mysql_file_open(key_file_wsrep_gra_log, filename,
                             O_RDWR | O_CREAT | O_BINARY, MYF(MY_WME))) < 0)
  {
    WSREP_ERROR("Failed to open file '%s' : %d (%s)",
                filename, errno, strerror(errno));
    goto cleanup1;
  }

  if (init_io_cache(&cache, file, 0, WRITE_CACHE, 0, 0, MYF(MY_WME | MY_NABP)))
  {
    goto cleanup2;
  }

  if (my_b_safe_write(&cache, BINLOG_MAGIC, BIN_LOG_HEADER_SIZE))
  {
    goto cleanup2;
  }

  /*
    Instantiate an FDLE object for non-wsrep threads (to be written
    to the dump file).
  */
  ev= (thd->wsrep_applier) ? wsrep_get_apply_format(thd) :
    (new Format_description_log_event(4));

  if (writer.write(ev) || my_b_write(&cache, (uchar*)rbr_buf, buf_len) ||
      flush_io_cache(&cache))
  {
    WSREP_ERROR("Failed to write to '%s'.", filename);
    goto cleanup2;
  }

cleanup2:
  end_io_cache(&cache);

cleanup1:
  free(filename);
  mysql_file_close(file, MYF(MY_WME));

  if (!thd->wsrep_applier) delete ev;

  DBUG_VOID_RETURN;
}

