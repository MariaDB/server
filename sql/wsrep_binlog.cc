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

#include "mariadb.h"
#include "mysql/service_wsrep.h"
#include "wsrep_binlog.h"
#include "wsrep_priv.h"
#include "log.h"
#include "log_event.h"
#include "wsrep_applier.h"

#include "transaction.h"

const char *wsrep_fragment_units[] = { "bytes", "rows", "statements", NullS };
const char *wsrep_SR_store_types[] = { "none", "table", NullS };


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
  DBUG_ENTER("wsrep_write_cache_buf");

  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
  {
    WSREP_ERROR("failed to initialize io-cache");
    DBUG_RETURN(ER_ERROR_ON_WRITE);
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

  DBUG_RETURN(0);

error:
  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_WARN("failed to initialize io-cache");
  }
cleanup:
  my_free(*buf);
  *buf= NULL;
  *buf_len= 0;
  DBUG_RETURN(ER_ERROR_ON_WRITE);
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

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.

  This version uses incremental data appending as it reads it from cache.
 */
static int wsrep_write_cache_inc(THD*      const thd,
                                 IO_CACHE* const cache,
                                 size_t*   const len)
{
  DBUG_ENTER("wsrep_write_cache_inc");
  my_off_t const saved_pos(my_b_tell(cache));

  if (reinit_io_cache(cache, READ_CACHE, thd->wsrep_sr().bytes_certified(), 0, 0))
  {
    WSREP_ERROR("failed to initialize io-cache");
    DBUG_RETURN(1);;
  }

  int ret= 0;
  size_t total_length(0);

  uint length(my_b_bytes_in_cache(cache));
  if (unlikely(0 == length)) length = my_b_fill(cache);

  if (likely(length > 0))
  {
    do
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
        ret = 1;
        goto cleanup;
      }
      if (thd->wsrep_cs().append_data(wsrep::const_buffer(cache->read_pos, length)))
        goto cleanup;
      cache->read_pos = cache->read_end;
    } while ((cache->file >= 0) && (length = my_b_fill(cache)));
  }
  if (ret == 0)
  {
    assert(total_length + thd->wsrep_sr().bytes_certified() == saved_pos);
  }

cleanup:
  *len = total_length;
  if (reinit_io_cache(cache, WRITE_CACHE, saved_pos, 0, 0))
  {
    WSREP_ERROR("failed to reinitialize io-cache");
  }
  DBUG_RETURN(ret);
}

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.
 */
int wsrep_write_cache(THD*      const thd,
                      IO_CACHE* const cache,
                      size_t*   const len)
{
  return wsrep_write_cache_inc(thd, cache, len);
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

int wsrep_binlog_savepoint_set(THD *thd,  void *sv)
{
  if (!wsrep_emulate_bin_log) return 0;
  int rcode = binlog_hton->savepoint_set(binlog_hton, thd, sv);
  return rcode;
}

int wsrep_binlog_savepoint_rollback(THD *thd, void *sv)
{
  if (!wsrep_emulate_bin_log) return 0;
  int rcode = binlog_hton->savepoint_rollback(binlog_hton, thd, sv);
  return rcode;
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
  if (cache->error == -1)
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
  Log_event_writer writer(&cache, 0);
  Format_description_log_event *ev= 0;

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
    mysql_file_close(file, MYF(MY_WME));
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


#include "wsrep_thd_pool.h"
extern Wsrep_thd_pool *wsrep_thd_pool;

#include "log_event.h"
// #include "binlog.h"

int wsrep_write_skip_event(THD* thd)
{
  DBUG_ENTER("wsrep_write_skip_event");
  Ignorable_log_event skip_event(thd);
  int ret= mysql_bin_log.write_event(&skip_event);
  if (ret)
  {
    WSREP_WARN("wsrep_write_skip_event: write to binlog failed: %d", ret);
  }
  if (!ret && (ret = trans_commit_stmt(thd)))
  {
    WSREP_WARN("wsrep_write_skip_event: statt commit failed");
  }
  DBUG_RETURN(ret);
}

int wsrep_write_dummy_event_low(THD *thd, const char *msg)
{
  ::abort();
  return 0;
#if 0
  int ret= 0;
  if (mysql_bin_log.is_open() && wsrep_gtid_mode)
  {
    DBUG_ASSERT(thd->wsrep_trx_meta.gtid.seqno != WSREP_SEQNO_UNDEFINED);

    /* For restoring orig thd state before returing it back to pool */
    int wsrep_on= thd->variables.wsrep_on;
    int sql_log_bin= thd->variables.sql_log_bin;
    int option_bits= thd->variables.option_bits;
    enum wsrep_exec_mode exec_mode= thd->wsrep_exec_mode;

    /*
      Fake that the connection is local client connection for the duration
      of mysql_bin_log.write_event(), otherwise the write will fail.
    */
    thd->wsrep_exec_mode= LOCAL_STATE;
    thd->variables.wsrep_on= 1;
    thd->variables.sql_log_bin= 1;
    thd->variables.option_bits |= OPTION_BIN_LOG;

    Ignorable_log_event skip_event(thd);
    ret= mysql_bin_log.write_event(&skip_event);

    /* Restore original thd state */
    thd->wsrep_exec_mode= exec_mode;
    thd->variables.wsrep_on= wsrep_on;
    thd->variables.sql_log_bin= sql_log_bin;
    thd->variables.option_bits= option_bits;

    if (ret)
    {
      WSREP_ERROR("Write to binlog failed: %d", ret);
      abort();
      unireg_abort(1);
    }
    ret= trans_commit_stmt(thd);
    if (ret)
    {
      WSREP_ERROR("STMT commit failed: %d", ret);
      abort();
    }
  }

  return ret;
#endif
}

int wsrep_write_dummy_event(THD *orig_thd, const char *msg)
{
  return 0;
#if 0
  if (WSREP_EMULATE_BINLOG(orig_thd))
  {
    return 0;
  }

  /* Use tmp thd for the transaction to avoid messing up orig_thd
     transaction state */
  THD* thd= wsrep_thd_pool->get_thd(0);

  if (!thd)
  {
    return 1;
  }

  /*
    Use original THD wsrep TRX meta data and WS handle to make
    commit generate GTID and go through commit ordering hooks.
   */
  wsrep_trx_meta_t meta_save= thd->wsrep_trx_meta;
  thd->wsrep_trx_meta= orig_thd->wsrep_trx_meta;
  wsrep_ws_handle_t handle_save= thd->wsrep_ws_handle;
  thd->wsrep_ws_handle= orig_thd->wsrep_ws_handle;

  int ret= wsrep_write_dummy_event_low(thd, msg);

  if (ret || (ret= trans_commit(thd)))
  {
    WSREP_ERROR("Failed to write skip event into binlog");
    abort();
    unireg_abort(1);
  }

  thd->wsrep_trx_meta= meta_save;
  thd->wsrep_ws_handle= handle_save;

  wsrep_thd_pool->release_thd(thd);

  orig_thd->store_globals();
  return ret;
#endif
}
