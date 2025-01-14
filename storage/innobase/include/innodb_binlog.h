/*****************************************************************************

Copyright (c) 2024, Kristian Nielsen

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/innodb_binlog.h
InnoDB implementation of binlog.
*******************************************************/

#ifndef innodb_binlog_h
#define innodb_binlog_h

#include "univ.i"
#include "fsp_binlog.h"


struct fil_space_t;
struct buf_block_t;
struct mtr_t;
struct rpl_binlog_state_base;
struct rpl_gtid;
struct handler_binlog_event_group_info;
class handler_binlog_reader;


/*
  The struct chunk_data_base is a simple encapsulation of data for a chunk that
  is to be written to the binlog. Used to separate the generic code that
  handles binlog writing with page format and so on, from the details of the
  data being written, avoiding an intermediary buffer holding consecutive data.

  Currently used for:
   - chunk_data_cache: A binlog trx cache to be binlogged as a commit record.
   - chunk_data_oob: An out-of-band piece of event group data.
*/
struct chunk_data_base {
  /*
    Copy at most max_len bytes to address p.
    Returns a pair with amount copied, and a bool if this is the last data.
    Should return the maximum amount of data available (up to max_len). Thus
    the size returned should only be less than max_len if the last-data flag
    is returned as true.
  */
  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) = 0;
  virtual ~chunk_data_base() {};
};


#define BINLOG_NAME_BASE "binlog-"
#define BINLOG_NAME_EXT ".ibb"
/* '/' + "binlog-" + (<=20 digits) + '.' + "ibb" + '\0'. */
#define BINLOG_NAME_MAX_LEN 1 + 1 + 7 + 20 + 1 + 3 + 1


extern uint32_t innodb_binlog_size_in_pages;
extern const char *innodb_binlog_directory;
extern uint32_t binlog_cur_page_no;
extern uint32_t binlog_cur_page_offset;
extern ulonglong innodb_binlog_state_interval;
extern rpl_binlog_state_base binlog_diff_state;


static inline void
binlog_name_make(char name_buf[OS_FILE_MAX_PATH], uint64_t file_no)
{
  snprintf(name_buf, OS_FILE_MAX_PATH,
           "%s/" BINLOG_NAME_BASE "%06" PRIu64 BINLOG_NAME_EXT,
           innodb_binlog_directory, file_no);
}


extern void innodb_binlog_startup_init();
extern bool innodb_binlog_init(size_t binlog_size, const char *directory);
extern void innodb_binlog_close(bool shutdown);
extern bool binlog_gtid_state(rpl_binlog_state_base *state, mtr_t *mtr,
                              buf_block_t * &block, uint32_t &page_no,
                              uint32_t &page_offset, fil_space_t *space);
extern bool innodb_binlog_oob(THD *thd, const unsigned char *data,
                              size_t data_len, void **engine_data);
extern void innodb_free_oob(THD *thd, void *engine_data);
extern handler_binlog_reader *innodb_get_binlog_reader();
extern void innodb_binlog_trx(trx_t *trx, mtr_t *mtr);
extern bool innobase_binlog_write_direct
  (IO_CACHE *cache, handler_binlog_event_group_info *binlog_info,
   const rpl_gtid *gtid);
extern bool innodb_find_binlogs(uint64_t *out_first, uint64_t *out_last);
extern bool innodb_reset_binlogs();

#endif /* innodb_binlog_h */
