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


struct mtr_t;
struct rpl_binlog_state_base;
struct rpl_gtid;
struct handler_binlog_event_group_info;
class handler_binlog_reader;
struct handler_binlog_purge_info;


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


/*
  Empty chunk data, used to pass a dummy record to fsp_binlog_write_rec()
  in fsp_binlog_flush().
*/
struct chunk_data_flush : public chunk_data_base {
  ~chunk_data_flush() { }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    memset(p, 0xff, max_len);
    return {max_len, true};
  }
};


/*
  Data stored at the start of each binlog file.
  (The data is stored as little-engian values in the first page of the file;
  this is just a struct to pass around the values in-memory).
*/
struct binlog_header_data {
  /*
    The LSN corresponding to the start of the binlog file. Any redo record
    with smaller start (or end) LSN than this should be ignored during recovery
    and not applied to this file.
  */
  lsn_t start_lsn;
  /*
    The file_no of the binlog file. This is written into the header to be able
    to recover it in the case where no binlog files are present at server
    start (could be due to FLUSH BINARY LOGS or RESET MASTER).
  */
  uint64_t file_no;
  /* The length of this binlog file, in pages. */
  uint64_t page_count;
  /*
    The interval (in pages) at which the (differential) binlog GTID state is
    written into the binlog file, for faster GTID position search. This
    corresponds to the value of --innodb-binlog-state-interval at the time the
    binlog file was created.
  */
  uint64_t diff_state_interval;
  /* The log_2 of the page size (eg. ibb_page_size_shift). */
  uint32_t page_size_shift;
  /*
    Major and minor file format version number. The idea is that minor version
    increments are backwards compatible, major version upgrades are not.
  */
  uint32_t vers_major, vers_minor;
  /* Whether the page was found empty. */
  bool is_empty;
  /*
    Whether the page was found invalid, bad magic or major version, or CRC32
    error (and not empty).
  */
  bool is_invalid;
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
extern mysql_mutex_t purge_binlog_mutex;
extern size_t total_binlog_used_size;


static inline void
binlog_name_make(char name_buf[OS_FILE_MAX_PATH], uint64_t file_no,
                 const char *binlog_dir)
{
  snprintf(name_buf, OS_FILE_MAX_PATH,
           "%s/" BINLOG_NAME_BASE "%06" PRIu64 BINLOG_NAME_EXT,
           binlog_dir, file_no);
}


static inline void
binlog_name_make(char name_buf[OS_FILE_MAX_PATH], uint64_t file_no)
{
  binlog_name_make(name_buf, file_no, innodb_binlog_directory);
}


static inline void
binlog_name_make_short(char *name_buf, uint64_t file_no)
{
  sprintf(name_buf, BINLOG_NAME_BASE "%06" PRIu64 BINLOG_NAME_EXT, file_no);
}


extern void innodb_binlog_startup_init();
extern bool innodb_binlog_init(size_t binlog_size, const char *directory);
extern void innodb_binlog_close(bool shutdown);
extern bool ibb_write_header_page(mtr_t *mtr, uint64_t file_no,
                                  uint64_t file_size_in_pages, lsn_t start_lsn,
                                  uint64_t gtid_state_interval_in_pages);
extern bool binlog_gtid_state(rpl_binlog_state_base *state, mtr_t *mtr,
                              fsp_binlog_page_entry * &block, uint32_t &page_no,
                              uint32_t &page_offset, uint64_t file_no);
extern bool innodb_binlog_oob(THD *thd, const unsigned char *data,
                              size_t data_len, void **engine_data);
extern void innodb_free_oob(THD *thd, void *engine_data);
extern handler_binlog_reader *innodb_get_binlog_reader();
extern void innodb_binlog_trx(trx_t *trx, mtr_t *mtr);
extern bool innobase_binlog_write_direct
  (IO_CACHE *cache, handler_binlog_event_group_info *binlog_info,
   const rpl_gtid *gtid);
extern bool innodb_find_binlogs(uint64_t *out_first, uint64_t *out_last);
extern void innodb_binlog_status(char out_filename[FN_REFLEN],
                                 ulonglong *out_pos);
extern bool innodb_binlog_get_init_state(rpl_binlog_state_base *out_state);
extern bool innodb_reset_binlogs();
extern int innodb_binlog_purge(handler_binlog_purge_info *purge_info);
extern bool binlog_recover_write_data(bool space_id, uint32_t page_no,
                                      uint16_t offset,
                                      lsn_t start_lsn, lsn_t lsn,
                                      const byte *buf, size_t size) noexcept;
extern void binlog_recover_end(lsn_t lsn) noexcept;

#endif /* innodb_binlog_h */
