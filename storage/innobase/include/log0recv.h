/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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

ATTRIBUTE_COLD MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Apply any buffered redo log to a page.
@param space     tablespace
@param bpage     buffer pool page
@return whether the page was recovered correctly */
bool recv_recover_page(fil_space_t* space, buf_page_t* bpage);

/** Read the latest checkpoint information from log file
and store it in log_sys.next_checkpoint and recv_sys.file_checkpoint
@return error code or DB_SUCCESS */
dberr_t recv_recovery_read_checkpoint();

/** Start recovering from a redo log checkpoint.
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t recv_recovery_from_checkpoint_start();

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

/** Report an operation which does undo log tablespace truncation
during backup
@param	space_id	undo tablespace identifier */
extern void (*undo_space_trunc)(uint32_t space_id);

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
  @param max_lsn  the maximum allowed LSN
  @param space    the tablespace of the page (not available for page 0)
  @param page     page contents
  @param tmp_buf  2*srv_page_size for decrypting and decompressing any
  page_compressed or encrypted pages
  @return whether the page is valid */
  bool validate_page(const page_id_t page_id, lsn_t max_lsn,
                     const fil_space_t *space,
                     const byte *page, byte *tmp_buf) const noexcept;

  /** Find a doublewrite copy of a page with the smallest FIL_PAGE_LSN
  that is large enough for recovery.
  @param page_id  page identifier
  @param max_lsn  the maximum allowed LSN
  @param space    tablespace (nullptr for page_id.page_no()==0)
  @param tmp_buf  2*srv_page_size for decrypting and decompressing any
  page_compressed or encrypted pages
  @return page frame
  @retval nullptr if no valid page for page_id was found */
  const byte *find_page(const page_id_t page_id, lsn_t max_lsn,
                        const fil_space_t *space= nullptr,
                        byte *tmp_buf= nullptr) const noexcept;

  /** Find the doublewrite copy of an encrypted page with the
  smallest FIL_PAGE_LSN that is large enough for recovery.
  @param space    tablespace object
  @param page_no  page number to find
  @param buf      buffer for unencrypted page
  @return buf
  @retval nullptr if the page was not found in doublewrite buffer */
  byte *find_encrypted_page(const fil_node_t &space, uint32_t page_no,
                            byte *buf) noexcept;

  /** Restore the first page of the given tablespace from
  doublewrite buffer.
  1) Find the page which has page_no as 0
  2) Read first 3 pages from tablespace file
  3) Compare the space_ids from the pages with page0 which
  was retrieved from doublewrite buffer
  @param name tablespace filepath
  @param file tablespace file handle
  @return space_id or 0 in case of error */
  inline uint32_t find_first_page(const char *name, pfs_os_file_t file)
    const noexcept;

  typedef std::deque<byte*, ut_allocator<byte*> > list;

  /** Recovered doublewrite buffer page frames */
  list pages;
};

/** recv_sys.pages entry; protected by recv_sys.mutex */
struct page_recv_t
{
  /** Recovery status: 0=not in progress, 1=log is being applied,
  -1=log has been applied and the entry may be erased.
  Transitions from 1 to -1 are NOT protected by recv_sys.mutex. */
  Atomic_relaxed<int8_t> being_processed{0};
  /** Whether reading the page will be skipped */
  bool skip_read= false;
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
    /** Remove the last records for the page
    @param start_lsn   start of the removed log */
    ATTRIBUTE_COLD void rewind(lsn_t start_lsn);

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
    /** Clear and free the records; @see recv_sys_t::add() */
    void clear();
  } log;

  /** Trim old log records for a page.
  @param start_lsn oldest log sequence number to preserve
  @return whether all the log for the page was trimmed */
  inline bool trim(lsn_t start_lsn);
  /** Ignore any earlier redo log records for this page. */
  inline void will_not_read();
};

/** Recovery system data structure */
struct recv_sys_t
{
  /** mutex protecting this as well as some of page_recv_t */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t mutex;
private:
  /** set when finding a corrupt log block or record, or there is a
  log parsing buffer overflow */
  bool found_corrupt_log;
  /** set when an inconsistency with the file system contents is detected
  during log scan or apply */
  bool found_corrupt_fs;
public:
  /** @return maximum guaranteed size of a mini-transaction on recovery */
  static constexpr size_t MTR_SIZE_MAX{1U << 20};

  /** whether we are applying redo log records during crash recovery.
  This can be cleared when holding mutex, or when pages.empty() and
  we are holding exclusive log_sys.latch. */
  Atomic_relaxed<bool> recovery_on= false;
  /** whether recv_recover_page(), invoked from buf_page_t::read_complete(),
  should apply log records*/
  bool apply_log_recs;
  /** number of bytes in log_sys.buf */
  size_t len;
  /** start offset of non-parsed log records in log_sys.buf */
  size_t offset;
  /** log sequence number of the first non-parsed record */
  lsn_t lsn;
  /** log sequence number of the last parsed mini-transaction */
  lsn_t scanned_lsn;
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
  /** iterator to pages, used by parse() */
  map::iterator pages_it;

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
  };

  trunc truncated_undo_spaces[127];
  trunc truncated_sys_space;

public:
  /** The contents of the doublewrite buffer */
  recv_dblwr_t dblwr;

  /** Data directory has been recognized as cloned data directory. */
  bool is_cloned_db= false;

  __attribute__((warn_unused_result)) 
  inline dberr_t read(os_offset_t offset, span<byte> buf);
  inline size_t files_size();
  void close_files();

  /** Advance pages_it if it matches the iterator */
  void pages_it_invalidate(const map::iterator &p)
  {
    mysql_mutex_assert_owner(&mutex);
    if (pages_it == p)
      pages_it++;
  }
  /** Invalidate pages_it if it points to the given tablespace */
  void pages_it_invalidate(uint32_t space_id)
  {
    mysql_mutex_assert_owner(&mutex);
    if (pages_it != pages.end() && pages_it->first.space() == space_id)
      pages_it= pages.end();
  }

  /** Allow to apply system tablespace truncate redo log only
  if the size to be extended is lesser than current size.
  @retval true  To apply the truncate shrink redo log record
  @retval false otherwise */
  bool check_sys_truncate();

private:
  /** Attempt to initialize a page based on redo log records.
  @param p        iterator
  @param mtr      mini-transaction
  @param b        pre-allocated buffer pool block
  @param init_lsn page initialization
  @return the recovered block
  @retval nullptr if the page cannot be initialized based on log records
  @retval -1      if the page cannot be recovered due to corruption */
  inline buf_block_t *recover_low(const map::iterator &p, mtr_t &mtr,
                                  buf_block_t *b, lsn_t init_lsn);

  /** All found log files (multiple ones are possible if we are upgrading
  from before MariaDB Server 10.5.1) */
  std::vector<log_file_t> files;

  /** Base node of the redo block list.
  List elements are linked via buf_block_t::unzip_LRU. */
  UT_LIST_BASE_NODE_T(buf_block_t) blocks;

  /** Allocate a block from the buffer pool for recv_sys.pages */
  ATTRIBUTE_COLD buf_block_t *add_block();

  /** Wait for buffer pool to become available.
  @param pages number of buffer pool pages needed */
  ATTRIBUTE_COLD void wait_for_pool(size_t pages);

  /** Free log for processed pages. */
  void garbage_collect();

  /** Apply a recovery batch.
  @param space_id       current tablespace identifier
  @param space          current tablespace
  @param free_block     spare buffer block
  @param last_batch     whether it is possible to write more redo log
  @return whether the caller must provide a new free_block */
  bool apply_batch(uint32_t space_id, fil_space_t *&space,
                   buf_block_t *&free_block, bool last_batch);

public:
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

  bool is_initialised() const { return scanned_lsn != 0; }

  /** Find the latest checkpoint.
  @return error code or DB_SUCCESS */
  dberr_t find_checkpoint();

  /** Register a redo log snippet for a page.
  @param it       page iterator
  @param start_lsn start LSN of the mini-transaction
  @param lsn      @see mtr_t::commit_lsn()
  @param l        redo log snippet
  @param len      length of l, in bytes
  @return whether we ran out of memory */
  bool add(map::iterator it, lsn_t start_lsn, lsn_t lsn,
           const byte *l, size_t len);

  /** Parsing result */
  enum parse_mtr_result {
    /** a record was successfully parsed */
    OK,
    /** the log ended prematurely (need to read more) */
    PREMATURE_EOF,
    /** the end of the log was reached */
    GOT_EOF,
    /** parse<true>(l, false) ran out of memory */
    GOT_OOM
  };

  /** Whether to store parsed log records */
  enum store{NO,BACKUP,YES};

private:
  /** Parse and register one log_t::FORMAT_10_8 mini-transaction.
  @tparam storing   whether to store the records
  @param  l         log data source
  @param  if_exists if store: whether to check if the tablespace exists */
  template<typename source,store storing>
  inline parse_mtr_result parse(source &l, bool if_exists) noexcept;

  /** Rewind a mini-transaction when parse() runs out of memory.
  @param  l         log data source
  @param  begin     start of the mini-transaction */
  template<typename source>
  ATTRIBUTE_COLD void rewind(source &l, source &begin) noexcept;

  /** Report progress in terms of LSN or pages remaining */
  ATTRIBUTE_COLD void report_progress() const;
public:
  /** Parse and register one log_t::FORMAT_10_8 mini-transaction,
  without handling any log_sys.is_mmap() buffer wrap-around.
  @tparam storing   whether to store the records
  @param  if_exists storing=YES: whether to check if the tablespace exists */
  template<store storing>
  static parse_mtr_result parse_mtr(bool if_exists) noexcept;
  /** Parse and register one log_t::FORMAT_10_8 mini-transaction,
  handling log_sys.is_mmap() buffer wrap-around.
  @tparam storing   whether to store the records
  @param  if_exists storing=YES: whether to check if the tablespace exists */
  template<store storing>
  static parse_mtr_result parse_mmap(bool if_exists) noexcept;

  /** Erase log records for a page. */
  void erase(map::iterator p);

  /** Clear a fully processed set of stored redo log records. */
  void clear();

  /** Determine whether redo log recovery progress should be reported.
  @param time  the current time
  @return whether progress should be reported
  (the last report was at least 15 seconds ago) */
  bool report(time_t time);

  /** The alloc() memory alignment, in bytes */
  static constexpr size_t ALIGNMENT= sizeof(size_t);

  /** Free a redo log snippet.
  @param data buffer allocated in add() */
  inline void free(const void *data);

  /** Remove records for a corrupted page.
  @param page_id  corrupted page identifier
  @param node     file for which an error is to be reported
  @return whether an error message was reported */
  ATTRIBUTE_COLD bool free_corrupted_page(page_id_t page_id,
                                          const fil_node_t &node) noexcept;

  /** Flag data file corruption during recovery. */
  ATTRIBUTE_COLD void set_corrupt_fs() noexcept;
  /** Flag log file corruption during recovery. */
  ATTRIBUTE_COLD void set_corrupt_log() noexcept;

  /** @return whether data file corruption was found */
  bool is_corrupt_fs() const { return UNIV_UNLIKELY(found_corrupt_fs); }
  /** @return whether log file corruption was found */
  bool is_corrupt_log() const { return UNIV_UNLIKELY(found_corrupt_log); }

  /** Check if recovery reached a consistent log sequence number.
  @return whether the recovery failed to process enough log */
  inline bool validate_checkpoint() const noexcept;

  /** Read a page or recover it based on redo log records.
  @param page_id  page identifier
  @param mtr      mini-transaction
  @param err      error code
  @return the requested block
  @retval nullptr if the page cannot be accessed due to corruption */
  ATTRIBUTE_COLD
  buf_block_t *recover(const page_id_t page_id, mtr_t *mtr, dberr_t *err);

  /** Try to recover a tablespace that was not readable earlier
  @param p          iterator
  @param name       tablespace file name
  @param free_block spare buffer block
  @return recovered tablespace
  @retval nullptr if recovery failed */
  fil_space_t *recover_deferred(const map::iterator &p,
                                const std::string &name,
                                buf_block_t *&free_block);
};

/** The recovery system */
extern recv_sys_t	recv_sys;

/** TRUE when recv_init_crash_recovery() has been called. */
extern bool		recv_needed_recovery;
#ifdef UNIV_DEBUG
/** whether writing to the redo log is forbidden;
protected by exclusive log_sys.latch. */
extern bool recv_no_log_write;
#endif /* UNIV_DEBUG */
