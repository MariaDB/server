/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/log0recv.h
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#pragma once

#include "ut0new.h"
#include "buf0types.h"
#include "log0log.h"
#include "mtr0types.h"

#include <deque>
#include <map>

/** @return whether recovery is currently running. */
#define recv_recovery_is_on() UNIV_UNLIKELY(recv_sys.recovery_on)

/** Apply any buffered redo log to a page that was just read from a data file.
@param[in,out]	space	tablespace
@param[in,out]	bpage	buffer pool page */
ATTRIBUTE_COLD void recv_recover_page(fil_space_t* space, buf_page_t* bpage)
	MY_ATTRIBUTE((nonnull));

/** Start recovering from a redo log checkpoint.
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t recv_recovery_from_checkpoint_start();

/** Whether to store redo log records in recv_sys.pages */
enum store_t {
	/** Do not store redo log records. */
	STORE_NO,
	/** Store redo log records. */
	STORE_YES,
	/** Store redo log records if the tablespace exists. */
	STORE_IF_EXISTS
};


/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	type		file operation redo log type
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
extern void (*log_file_op)(uint32_t space_id, int type,
			   const byte* name, ulint len,
			   const byte* new_name, ulint new_len);

/** Report an operation which does INIT_PAGE for page0 during backup.
@param	space_id	tablespace identifier */
extern void (*first_page_init)(uint32_t space_id);

/** Stored redo log record */
struct log_rec_t
{
  log_rec_t(lsn_t lsn) : next(nullptr), lsn(lsn) { ut_ad(lsn); }
  log_rec_t()= delete;
  log_rec_t(const log_rec_t&)= delete;
  log_rec_t &operator=(const log_rec_t&)= delete;

  /** next record */
  log_rec_t *next;
  /** mtr_t::commit_lsn() of the mini-transaction */
  const lsn_t lsn;
};

struct recv_dblwr_t
{
  /** Add a page frame to the doublewrite recovery buffer. */
  void add(byte *page) { pages.push_front(page); }

  /** Validate the page.
  @param page_id  page identifier
  @param page     page contents
  @param space    the tablespace of the page (not available for page 0)
  @param tmp_buf  2*srv_page_size for decrypting and decompressing any
  page_compressed or encrypted pages
  @return whether the page is valid */
  bool validate_page(const page_id_t page_id, const byte *page,
                     const fil_space_t *space, byte *tmp_buf);

  /** Find a doublewrite copy of a page.
  @param page_id  page identifier
  @param space    tablespace (not available for page_id.page_no()==0)
  @param tmp_buf  2*srv_page_size for decrypting and decompressing any
  page_compressed or encrypted pages
  @return page frame
  @retval NULL if no valid page for page_id was found */
  byte* find_page(const page_id_t page_id, const fil_space_t *space= NULL,
                  byte *tmp_buf= NULL);

  typedef std::deque<byte*, ut_allocator<byte*> > list;

  /** Recovered doublewrite buffer page frames */
  list pages;
};

/** the recovery state and buffered records for a page */
struct page_recv_t
{
  /** Recovery state; protected by recv_sys.mutex */
  enum
  {
    /** not yet processed */
    RECV_NOT_PROCESSED,
    /** not processed; the page will be reinitialized */
    RECV_WILL_NOT_READ,
    /** page is being read */
    RECV_BEING_READ,
    /** log records are being applied on the page */
    RECV_BEING_PROCESSED
  } state= RECV_NOT_PROCESSED;
  /** Latest written byte offset when applying the log records.
  @see mtr_t::m_last_offset */
  uint16_t last_offset= 1;
  /** log records for a page */
  class recs_t
  {
    /** The first log record */
    log_rec_t *head= nullptr;
    /** The last log record */
    log_rec_t *tail= nullptr;
    friend struct page_recv_t;
  public:
    /** Append a redo log snippet for the page
    @param recs log snippet */
    void append(log_rec_t* recs)
    {
      if (tail)
        tail->next= recs;
      else
        head= recs;
      tail= recs;
    }

    /** @return the last log snippet */
    const log_rec_t* last() const { return tail; }
    /** @return the last log snippet */
    log_rec_t* last() { return tail; }

    class iterator
    {
      log_rec_t *cur;
    public:
      iterator(log_rec_t* rec) : cur(rec) {}
      log_rec_t* operator*() const { return cur; }
      iterator &operator++() { cur= cur->next; return *this; }
      bool operator!=(const iterator& i) const { return cur != i.cur; }
    };
    iterator begin() { return head; }
    iterator end() { return NULL; }
    bool empty() const { ut_ad(!head == !tail); return !head; }
    /** Clear and free the records; @see recv_sys_t::alloc() */
    inline void clear();
  } log;

  /** Trim old log records for a page.
  @param start_lsn oldest log sequence number to preserve
  @return whether all the log for the page was trimmed */
  inline bool trim(lsn_t start_lsn);
  /** Ignore any earlier redo log records for this page. */
  inline void will_not_read();
  /** @return whether the log records for the page are being processed */
  bool is_being_processed() const { return state == RECV_BEING_PROCESSED; }
};

/** Recovery system data structure */
struct recv_sys_t
{
  /** mutex protecting apply_log_recs and page_recv_t::state */
  mysql_mutex_t mutex;
private:
  /** condition variable for
  !apply_batch_on || pages.empty() || found_corrupt_log || found_corrupt_fs */
  pthread_cond_t cond;
  /** whether recv_apply_hashed_log_recs() is running */
  bool apply_batch_on;
  /** set when finding a corrupt log block or record, or there is a
  log parsing buffer overflow */
  bool found_corrupt_log;
  /** set when an inconsistency with the file system contents is detected
  during log scan or apply */
  bool found_corrupt_fs;
public:
  /** @return maximum guaranteed size of a mini-transaction on recovery */
  static constexpr size_t MTR_SIZE_MAX{1U << 20};

  /** whether we are applying redo log records during crash recovery */
  bool recovery_on;
  /** whether recv_recover_page(), invoked from buf_page_t::read_complete(),
  should apply log records*/
  bool apply_log_recs;
  /** number of bytes in log_sys.buf */
  size_t len;
  /** start offset of non-parsed log records in log_sys.buf */
  size_t offset;
  /** log sequence number of the first non-parsed record */
  lsn_t lsn;
  /** log sequence number at the end of the FILE_CHECKPOINT record, or 0 */
  lsn_t file_checkpoint;
  /** the time when progress was last reported */
  time_t progress_time;

  using map = std::map<const page_id_t, page_recv_t,
                       std::less<const page_id_t>,
                       ut_allocator<std::pair<const page_id_t, page_recv_t>>>;
  /** buffered records waiting to be applied to pages */
  map pages;

private:
  /** Process a record that indicates that a tablespace size is being shrunk.
  @param page_id first page that is not in the file
  @param lsn     log sequence number of the shrink operation */
  inline void trim(const page_id_t page_id, lsn_t lsn);

  /** Undo tablespaces for which truncate has been logged
  (indexed by page_id_t::space() - srv_undo_space_id_start) */
  struct trunc
  {
    /** log sequence number of FILE_CREATE, or 0 if none */
    lsn_t lsn;
    /** truncated size of the tablespace, or 0 if not truncated */
    unsigned pages;
  } truncated_undo_spaces[127];

public:
  /** The contents of the doublewrite buffer */
  recv_dblwr_t dblwr;

  /** Last added LSN to pages, before switching to STORE_NO */
  lsn_t last_stored_lsn= 0;

  inline void read(os_offset_t offset, span<byte> buf);
  inline size_t files_size();
  void close_files() { files.clear(); files.shrink_to_fit(); }

private:
  /** Attempt to initialize a page based on redo log records.
  @param page_id  page identifier
  @param p        iterator pointing to page_id
  @param mtr      mini-transaction
  @param b        pre-allocated buffer pool block
  @return whether the page was successfully initialized */
  inline buf_block_t *recover_low(const page_id_t page_id, map::iterator &p,
                                  mtr_t &mtr, buf_block_t *b);
  /** Attempt to initialize a page based on redo log records.
  @param page_id  page identifier
  @return the recovered block
  @retval nullptr if the page cannot be initialized based on log records */
  buf_block_t *recover_low(const page_id_t page_id);

  /** All found log files (multiple ones are possible if we are upgrading
  from before MariaDB Server 10.5.1) */
  std::vector<log_file_t> files;

  /** Base node of the redo block list.
  List elements are linked via buf_block_t::unzip_LRU. */
  UT_LIST_BASE_NODE_T(buf_block_t) blocks;
public:
  /** Check whether the number of read redo log blocks exceeds the maximum.
  @return whether the memory is exhausted */
  inline bool is_memory_exhausted();
  /** Apply buffered log to persistent data pages.
  @param last_batch     whether it is possible to write more redo log */
  void apply(bool last_batch);

#ifdef UNIV_DEBUG
  /** whether all redo log in the current batch has been applied */
  bool after_apply= false;
#endif
  /** Initialize the redo log recovery subsystem. */
  void create();

  /** Free most recovery data structures. */
  void debug_free();

  /** Clean up after create() */
  void close();

  bool is_initialised() const { return last_stored_lsn != 0; }

  /** Find the latest checkpoint.
  @return error code or DB_SUCCESS */
  dberr_t find_checkpoint();

  /** Register a redo log snippet for a page.
  @param it       page iterator
  @param start_lsn start LSN of the mini-transaction
  @param lsn      @see mtr_t::commit_lsn()
  @param l        redo log snippet
  @param len      length of l, in bytes */
  inline void add(map::iterator it, lsn_t start_lsn, lsn_t lsn,
                  const byte *l, size_t len);

  enum parse_mtr_result { OK, PREMATURE_EOF, GOT_EOF };

private:
  /** Parse and register one log_t::FORMAT_10_8 mini-transaction.
  @param store   whether to store the records
  @param l       log data source */
  template<typename source>
  inline parse_mtr_result parse(store_t store, source& l) noexcept;
public:
  /** Parse and register one log_t::FORMAT_10_8 mini-transaction,
  handling log_sys.is_pmem() buffer wrap-around.
  @param store           whether to store the records */
  static parse_mtr_result parse_mtr(store_t store) noexcept;

  /** Parse and register one log_t::FORMAT_10_8 mini-transaction,
  handling log_sys.is_pmem() buffer wrap-around.
  @param store   whether to store the records */
  static parse_mtr_result parse_pmem(store_t store) noexcept
#ifdef HAVE_PMEM
    ;
#else
  { return parse_mtr(store); }
#endif

  /** Clear a fully processed set of stored redo log records. */
  inline void clear();

  /** Determine whether redo log recovery progress should be reported.
  @param time  the current time
  @return whether progress should be reported
  (the last report was at least 15 seconds ago) */
  bool report(time_t time)
  {
    if (time - progress_time < 15)
      return false;

    progress_time= time;
    return true;
  }

  /** The alloc() memory alignment, in bytes */
  static constexpr size_t ALIGNMENT= sizeof(size_t);

  /** Allocate memory for log_rec_t
  @param len  allocation size, in bytes
  @return pointer to len bytes of memory (never NULL) */
  inline void *alloc(size_t len);

  /** Free a redo log snippet.
  @param data buffer returned by alloc() */
  inline void free(const void *data);

  /** Remove records for a corrupted page.
  This function should only be called when innodb_force_recovery is set.
  @param page_id  corrupted page identifier */
  ATTRIBUTE_COLD void free_corrupted_page(page_id_t page_id);

  /** Flag data file corruption during recovery. */
  ATTRIBUTE_COLD void set_corrupt_fs();
  /** Flag log file corruption during recovery. */
  ATTRIBUTE_COLD void set_corrupt_log();
  /** Possibly finish a recovery batch. */
  inline void maybe_finish_batch();

  /** @return whether data file corruption was found */
  bool is_corrupt_fs() const { return UNIV_UNLIKELY(found_corrupt_fs); }
  /** @return whether log file corruption was found */
  bool is_corrupt_log() const { return UNIV_UNLIKELY(found_corrupt_log); }

  /** Attempt to initialize a page based on redo log records.
  @param page_id  page identifier
  @return the recovered block
  @retval nullptr if the page cannot be initialized based on log records */
  buf_block_t *recover(const page_id_t page_id)
  {
    return UNIV_UNLIKELY(recovery_on) ? recover_low(page_id) : nullptr;
  }

  /** Try to recover a tablespace that was not readable earlier
  @param p          iterator, initially pointing to page_id_t{space_id,0};
                    the records will be freed and the iterator advanced
  @param name       tablespace file name
  @param free_block spare buffer block
  @return whether recovery failed */
  bool recover_deferred(map::iterator &p, const std::string &name,
                        buf_block_t *&free_block);
};

/** The recovery system */
extern recv_sys_t	recv_sys;

/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this will be set if
recv_sys.pages becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

TRUE means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
extern bool		recv_no_ibuf_operations;
/** TRUE when recv_init_crash_recovery() has been called. */
extern bool		recv_needed_recovery;
#ifdef UNIV_DEBUG
/** whether writing to the redo log is forbidden;
protected by exclusive log_sys.latch. */
extern bool recv_no_log_write;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start(). */
extern bool		recv_lsn_checks_on;
