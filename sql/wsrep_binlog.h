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

#include "sql_class.h" // THD, IO_CACHE

#define HEAP_PAGE_SIZE 65536 /* 64K */
#define WSREP_MAX_WS_SIZE (0xFFFFFFFFUL - HEAP_PAGE_SIZE)

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
int wsrep_write_cache (wsrep_t*  wsrep,
                       THD*      thd,
                       IO_CACHE* cache,
                       size_t*   len);

/* Dump replication buffer to disk */
void wsrep_dump_rbr_buf(THD *thd, const void* rbr_buf, size_t buf_len);

/* Dump replication buffer to disk without intermediate buffer */
void wsrep_dump_rbr_direct(THD* thd, IO_CACHE* cache);

int wsrep_binlog_close_connection(THD* thd);
int wsrep_binlog_savepoint_set(THD *thd,  void *sv);
int wsrep_binlog_savepoint_rollback(THD *thd, void *sv);

#endif /* WSREP_BINLOG_H */
