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

#ifndef WSREP_BINLOG_H
#define WSREP_BINLOG_H

#include "my_global.h"
#include "sql_class.h" // THD, IO_CACHE

#define HEAP_PAGE_SIZE 65536 /* 64K */
#define WSREP_MAX_WS_SIZE 2147483647 /* 2GB */

/*
  Write the contents of a cache to a memory buffer.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.
 */
int wsrep_write_cache_buf(IO_CACHE *cache, uchar **buf, size_t *buf_len);

/*
  Write the contents of a cache to wsrep provider.

  This function quite the same as MYSQL_BIN_LOG::write_cache(),
  with the exception that here we write in buffer instead of log file.

  @param len  total amount of data written
  @return     wsrep error status
 */
int  wsrep_write_cache(THD*      thd,
                       IO_CACHE* cache,
                       size_t*   len);

/* Dump replication buffer to disk */
void wsrep_dump_rbr_buf(THD *thd, const void* rbr_buf, size_t buf_len);

/* Dump replication buffer along with header to a file */
void wsrep_dump_rbr_buf_with_header(THD *thd, const void *rbr_buf,
                                    size_t buf_len);

int wsrep_binlog_close_connection(THD* thd);

/**
   Write a skip event into binlog.

   @param thd Thread object pointer
   @return Zero in case of success, non-zero on failure.
*/
int wsrep_write_skip_event(THD* thd);

/*
  Write dummy event into binlog in place of unused GTID.
  The binlog write is done in thd context.
*/
int wsrep_write_dummy_event_low(THD *thd, const char *msg);
/*
  Write dummy event to binlog in place of unused GTID and
  commit. The binlog write and commit are done in temporary
  thd context, the original thd state is not altered.
*/
int wsrep_write_dummy_event(THD* thd, const char *msg);

void wsrep_register_binlog_handler(THD *thd, bool trx);

#endif /* WSREP_BINLOG_H */
