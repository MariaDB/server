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

#define WSREP_FRAG_BYTES      0
#define WSREP_FRAG_EVENTS     1
#define WSREP_FRAG_ROWS       2
#define WSREP_FRAG_STATEMENTS 3

#define WSREP_SR_STORE_NONE      0
#define WSREP_SR_STORE_FILE      1
#define WSREP_SR_STORE_TABLE     2

extern ulong wsrep_SR_store_type;
extern const char *wsrep_fragment_units[];
extern const char *wsrep_SR_store_types[];

class wsrep_SR_trx;
#include "sql_class.h" // THD, IO_CACHE

#define HEAP_PAGE_SIZE 65536 /* 64K */
#define WSREP_MAX_WS_SIZE 2147483647 /* 2GB */

bool  wsrep_fragment_full(THD *thd);
void  wsrep_reset_SR_trans(THD *thd);
void  wsrep_reset_SR_fill(THD *thd);
int   wsrep_append_SR_trans(THD *thd, ulong unit, ulong size, bool replicate);
bool  wsrep_fragmented(THD *thd);
void  wsrep_step_fragment_base(THD *thd, ulong size);
ulong wsrep_get_fragment_base(THD *thd);
void wsrep_append_fill_rate(THD*, ulong);
void wsrep_reset_fragment_fill(THD*, ulong);
ulong wsrep_get_fragment_fill(THD*);

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
wsrep_trx_status wsrep_write_cache(wsrep_t*  wsrep,
                                   THD*      thd,
                                   IO_CACHE* cache,
                                   size_t*   len);

/* Dump replication buffer to disk */
void wsrep_dump_rbr_buf(THD *thd, const void* rbr_buf, size_t buf_len);

/* Dump replication buffer along with header to a file */
void wsrep_dump_rbr_buf_with_header(THD *thd, const void *rbr_buf,
                                    size_t buf_len);

int wsrep_binlog_close_connection(THD* thd);
uint wsrep_get_trans_cache_position(THD *thd);

/* Write dummy event to binlog in place of unused GTID */
int wsrep_write_dummy_event(THD* thd);

void wsrep_register_binlog_handler(THD *thd, bool trx);

#endif /* WSREP_BINLOG_H */
