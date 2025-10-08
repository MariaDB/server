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
struct binlog_oob_context;


/**
  The struct chunk_data_base is a simple encapsulation of data for a chunk that
  is to be written to the binlog. Used to separate the generic code that
  handles binlog writing with page format and so on, from the details of the
  data being written, avoiding an intermediary buffer holding consecutive data.

  Currently used for:
   - chunk_data_cache: A binlog trx cache to be binlogged as a commit record.
   - chunk_data_oob: An out-of-band piece of event group data.
   - chunk_data_flush: For dummy filler data.
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


/**
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


static constexpr size_t IBB_BINLOG_HEADER_SIZE= 64;

/**
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
  /* The earliest file_no that we have oob references into. */
  uint64_t oob_ref_file_no;
  /*
    The earliest file_no that we have XA oob references into.
    ToDo: This in preparation for when XA is implemented.
  */
  uint64_t xa_ref_file_no;
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


/**
  The class pending_lsn_fifo keeps track of pending LSNs - and their
  corresponding binlog file_no/offset - that have been mtr-committed, but have
  not yet become durable.

  Used to delay sending to slaves any data that might be lost in case the
  master crashes just after sending.
*/
class pending_lsn_fifo {
  static constexpr uint32_t fixed_size_log2= 10;
  static constexpr uint32_t fixed_size= (2 << fixed_size_log2);
  static constexpr uint32_t mask= (2 << fixed_size_log2) - 1;
public:
  struct entry {
    lsn_t lsn;
    uint64_t file_no;
    uint64_t offset;
  } fifo[fixed_size];
  /*
    Set while we are duing a durable sync of the redo log to the LSN that we
    are requesting to become durable. Used to avoid multiple threads
    needlessly trying to sync the redo log on top of one another.
  */
  lsn_t flushing_lsn;
  /*
    The last added (and thus largest) lsn. Equal to cur_head().lsn when the
    fifo is not empty (and the lsn of the previous head when it is empty).
  */
  lsn_t last_lsn_added;
  /*
    The current file_no that has any durable data. Used to detect when an LSN
    moves the current durable end point to the next file, so that the previous
    file can then be marked as fully durable.
    The value ~0 is used as a marker for "not yet initialized".
  */
  uint64_t cur_file_no;
  /* The `head' points one past the most recent element. */
  uint32_t head;
  /* The `tail' points to the earliest element. */
  uint32_t tail;

  pending_lsn_fifo();
  void init(uint64_t start_file_no);
  void reset();
  bool is_empty() { return head == tail; }
  bool is_full() { return head == tail + fixed_size; }
  entry &cur_head() { ut_ad(!is_empty()); return fifo[(head - 1) & mask]; }
  entry &cur_tail() { ut_ad(!is_empty()); return fifo[tail & mask]; }
  void drop_tail() { ut_ad(!is_empty()); ++tail; }
  void new_head() { ut_ad(!is_full()); ++head; }
  void record_commit(binlog_oob_context *c);
  void add_to_fifo(uint64_t lsn, uint64_t file_no, uint64_t offset);
  bool process_durable_lsn(lsn_t lsn);
};


#define BINLOG_NAME_BASE "binlog-"
#define BINLOG_NAME_EXT ".ibb"
/* '/' + "binlog-" + (<=20 digits) + '.' + "ibb" + '\0'. */
#define BINLOG_NAME_MAX_LEN 1 + 1 + 7 + 20 + 1 + 3 + 1


extern pending_lsn_fifo ibb_pending_lsn_fifo;
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


extern bool is_binlog_name(const char *name, uint64_t *out_idx);
extern int get_binlog_header(const char *binlog_path, byte *page_buf,
                             lsn_t &out_lsn, bool &out_empty) noexcept;
extern void innodb_binlog_startup_init();
extern void ibb_set_max_size(size_t binlog_size);
extern bool innodb_binlog_init(size_t binlog_size, const char *directory);
extern void innodb_binlog_close(bool shutdown);
extern bool ibb_write_header_page(mtr_t *mtr, uint64_t file_no,
                                  uint64_t file_size_in_pages, lsn_t start_lsn,
                                  uint64_t gtid_state_interval_in_pages,
                                  LF_PINS *pins);
extern bool binlog_gtid_state(rpl_binlog_state_base *state, mtr_t *mtr,
                              fsp_binlog_page_entry * &block, uint32_t &page_no,
                              uint32_t &page_offset, uint64_t file_no);
extern bool innodb_binlog_oob_ordered(THD *thd, const unsigned char *data,
                                      size_t data_len, void **engine_data,
                                      void **stm_start_data,
                                      void **savepoint_data);
extern bool innodb_binlog_oob(THD *thd, const unsigned char *data,
                              size_t data_len, void **engine_data);
void ibb_savepoint_rollback(THD *thd, void **engine_data,
                            void **stmt_start_data, void **savepoint_data);
extern void innodb_reset_oob(void **engine_data);
extern void innodb_free_oob(void *engine_data);
extern handler_binlog_reader *innodb_get_binlog_reader(bool wait_durable);
extern void ibb_wait_durable_offset(uint64_t file_no, uint64_t wait_offset);
extern void ibb_get_filename(char name[FN_REFLEN], uint64_t file_no);
extern binlog_oob_context *innodb_binlog_trx(trx_t *trx, mtr_t *mtr);
extern void innodb_binlog_post_commit(mtr_t *mtr, binlog_oob_context *c);
extern bool innobase_binlog_write_direct_ordered
  (IO_CACHE *cache, handler_binlog_event_group_info *binlog_info,
   const rpl_gtid *gtid);
extern bool innobase_binlog_write_direct
  (IO_CACHE *cache, handler_binlog_event_group_info *binlog_info,
   const rpl_gtid *gtid);
extern void ibb_group_commit(THD *thd,
                             handler_binlog_event_group_info *binlog_info);
extern bool ibb_write_xa_prepare_ordered(
                             handler_binlog_event_group_info *binlog_info,
                             uchar engine_count);
extern bool ibb_write_xa_prepare(handler_binlog_event_group_info *binlog_info,
                                 uchar engine_count);
extern bool innodb_find_binlogs(uint64_t *out_first, uint64_t *out_last);
extern void innodb_binlog_status(uint64_t *out_file_no, uint64_t *out_pos);
extern bool innodb_binlog_get_init_state(rpl_binlog_state_base *out_state);
extern bool innodb_reset_binlogs();
extern int innodb_binlog_purge(handler_binlog_purge_info *purge_info);
extern bool binlog_recover_write_data(bool space_id, uint32_t page_no,
                                      uint16_t offset,
                                      lsn_t start_lsn, lsn_t lsn,
                                      const byte *buf, size_t size) noexcept;
extern void binlog_recover_end(lsn_t lsn) noexcept;

#endif /* innodb_binlog_h */
