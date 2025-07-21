/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2023, MariaDB Corporation.

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
@file include/mtr0mtr.h
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "fil0fil.h"
#include "dyn0buf.h"
#include "buf0buf.h"
#include "small_vector.h"

/** Start a mini-transaction. */
#define mtr_start(m)		(m)->start()

/** Commit a mini-transaction. */
#define mtr_commit(m)		(m)->commit()

/** Change the logging mode of a mini-transaction.
@return	old mode */
#define mtr_set_log_mode(m, d)	(m)->set_log_mode((d))

#ifdef UNIV_PFS_RWLOCK
# define mtr_s_lock_index(i,m)	(m)->s_lock(__FILE__, __LINE__, &(i)->lock)
# define mtr_x_lock_index(i,m)	(m)->x_lock(__FILE__, __LINE__, &(i)->lock)
# define mtr_sx_lock_index(i,m)	(m)->u_lock(__FILE__, __LINE__, &(i)->lock)
#else
# define mtr_s_lock_index(i,m)	(m)->s_lock(&(i)->lock)
# define mtr_x_lock_index(i,m)	(m)->x_lock(&(i)->lock)
# define mtr_sx_lock_index(i,m)	(m)->u_lock(&(i)->lock)
#endif

/** Mini-transaction memo stack slot. */
struct mtr_memo_slot_t
{
  /** pointer to the object */
  void *object;
  /** type of the stored object */
  mtr_memo_type_t type;

  /** Release the object */
  void release() const;
};

/** Mini-transaction handle and buffer */
struct mtr_t {
  mtr_t();
  ~mtr_t();

  /** Start a mini-transaction. */
  void start();

  /** Commit the mini-transaction. */
  void commit();

  /** Release latches of unmodified buffer pages.
  @param begin   first slot to release
  @param end     last slot to release, or get_savepoint() */
  void rollback_to_savepoint(ulint begin, ulint end);

  /** Release latches of unmodified buffer pages.
  @param begin   first slot to release */
  void rollback_to_savepoint(ulint begin)
  { rollback_to_savepoint(begin, m_memo.size()); }

  /** Release the last acquired buffer page latch. */
  void release_last_page()
  { auto s= m_memo.size(); rollback_to_savepoint(s - 1, s); }

  /** Commit a mini-transaction that is shrinking a tablespace.
  @param space   tablespace that is being shrunk
  @param size    new size in pages */
  ATTRIBUTE_COLD void commit_shrink(fil_space_t &space, uint32_t size);

  /** Commit a mini-transaction that is deleting or renaming a file.
  @param space           tablespace that is being renamed or deleted
  @param name            new file name (nullptr=the file will be deleted)
  @return whether the operation succeeded */
  ATTRIBUTE_COLD bool commit_file(fil_space_t &space, const char *name);

  /** Commit a mini-transaction that did not modify any pages,
  but generated some redo log on a higher level, such as
  FILE_MODIFY records and an optional FILE_CHECKPOINT marker.
  The caller must hold exclusive log_sys.latch.
  This is to be used at log_checkpoint().
  @param checkpoint_lsn   the log sequence number of a checkpoint, or 0
  @return current LSN */
  ATTRIBUTE_COLD lsn_t commit_files(lsn_t checkpoint_lsn= 0);

  /** @return mini-transaction savepoint (current size of m_memo) */
  ulint get_savepoint() const
  {
    ut_ad(is_active());
    return m_memo.size();
  }

  /** Get the block at a savepoint */
  buf_block_t *at_savepoint(ulint savepoint) const
  {
    ut_ad(is_active());
    const mtr_memo_slot_t &slot= m_memo[savepoint];
    ut_ad(slot.type < MTR_MEMO_S_LOCK);
    ut_ad(slot.object);
    return static_cast<buf_block_t*>(slot.object);
  }

  /** Try to get a block at a savepoint.
  @param savepoint the savepoint right before the block was acquired
  @return the block at the savepoint
  @retval nullptr  if no buffer block was registered at that savepoint */
  buf_block_t *block_at_savepoint(ulint savepoint) const
  {
    ut_ad(is_active());
    const mtr_memo_slot_t &slot= m_memo[savepoint];
    return slot.type < MTR_MEMO_S_LOCK
      ? static_cast<buf_block_t*>(slot.object)
      : nullptr;
  }

  /** Retrieve a page that has already been latched.
  @param id    page identifier
  @param type  page latch type
  @return block
  @retval nullptr if the block had not been latched yet */
  buf_block_t *get_already_latched(const page_id_t id, mtr_memo_type_t type)
    const;

  /** @return the logging mode */
  mtr_log_t get_log_mode() const
  {
    static_assert(MTR_LOG_ALL == 0, "efficiency");
    return static_cast<mtr_log_t>(m_log_mode);
  }

  /** @return whether log is to be written for changes */
  bool is_logged() const
  {
    static_assert(MTR_LOG_ALL == 0, "efficiency");
    static_assert(MTR_LOG_NONE & MTR_LOG_NO_REDO, "efficiency");
    static_assert(!(MTR_LOG_NONE & MTR_LOG_SUB), "efficiency");
    return !(m_log_mode & MTR_LOG_NONE);
  }

  /** Change the logging mode.
  @param mode	 logging mode
  @return	old mode */
  mtr_log_t set_log_mode(mtr_log_t mode)
  {
    const mtr_log_t old_mode= get_log_mode();
    m_log_mode= mode & 3;
    return old_mode;
  }

  /** Set the log mode of a sub-minitransaction
  @param mtr  parent mini-transaction */
  void set_log_mode_sub(const mtr_t &mtr)
  {
    ut_ad(mtr.m_log_mode == MTR_LOG_ALL || mtr.m_log_mode == MTR_LOG_NO_REDO);
    m_log_mode= mtr.m_log_mode | MTR_LOG_SUB;
    static_assert((MTR_LOG_SUB | MTR_LOG_NO_REDO) == MTR_LOG_NO_REDO, "");
  }

  /** Check if we are holding a block latch in exclusive mode
  @param block  buffer pool block to search for */
  bool have_x_latch(const buf_block_t &block) const;

  /** Check if we are holding a block latch in S or U mode
  @param block  buffer pool block to search for */
  bool have_u_or_x_latch(const buf_block_t &block) const;

	/** Copy the tablespaces associated with the mini-transaction
	(needed for generating FILE_MODIFY records)
	@param[in]	mtr	mini-transaction that may modify
	the same set of tablespaces as this one */
	void set_spaces(const mtr_t& mtr)
	{
		ut_ad(!m_user_space_id);
		ut_ad(!m_user_space);

		ut_d(m_user_space_id = mtr.m_user_space_id);
		m_user_space = mtr.m_user_space;
	}

	/** Set the tablespace associated with the mini-transaction
	(needed for generating a FILE_MODIFY record)
	@param[in]	space_id	user or system tablespace ID
	@return	the tablespace */
	fil_space_t* set_named_space_id(uint32_t space_id)
	{
		ut_ad(!m_user_space_id);
		ut_d(m_user_space_id = space_id);
		if (!space_id) {
			return fil_system.sys_space;
		} else {
			ut_ad(m_user_space_id == space_id);
			ut_ad(!m_user_space);
			m_user_space = fil_space_get(space_id);
			ut_ad(m_user_space);
			return m_user_space;
		}
	}

	/** Set the tablespace associated with the mini-transaction
	(needed for generating a FILE_MODIFY record)
	@param[in]	space	user or system tablespace */
	void set_named_space(fil_space_t* space)
	{
		ut_ad(!m_user_space_id);
		ut_d(m_user_space_id = space->id);
		if (space->id) {
			m_user_space = space;
		}
	}

#ifdef UNIV_DEBUG
	/** Check the tablespace associated with the mini-transaction
	(needed for generating a FILE_MODIFY record)
	@param[in]	space	tablespace
	@return whether the mini-transaction is associated with the space */
	bool is_named_space(uint32_t space) const;
	/** Check the tablespace associated with the mini-transaction
	(needed for generating a FILE_MODIFY record)
	@param[in]	space	tablespace
	@return whether the mini-transaction is associated with the space */
	bool is_named_space(const fil_space_t* space) const;
#endif /* UNIV_DEBUG */

  /** Acquire a tablespace X-latch.
  @param space_id   tablespace ID
  @return the tablespace object (never NULL) */
  fil_space_t *x_lock_space(uint32_t space_id);

  /** Acquire a shared rw-latch. */
  void s_lock(
#ifdef UNIV_PFS_RWLOCK
    const char *file, unsigned line,
#endif
    index_lock *lock)
  {
    memo_push(lock, MTR_MEMO_S_LOCK);
    lock->s_lock(SRW_LOCK_ARGS_(file, line) spin_control::skip_spin());
  }

  /** Acquire an exclusive rw-latch. */
  void x_lock(
#ifdef UNIV_PFS_RWLOCK
    const char *file, unsigned line,
#endif
    index_lock *lock)
  {
    memo_push(lock, MTR_MEMO_X_LOCK);
    lock->x_lock(SRW_LOCK_ARGS_(file, line) spin_control::skip_spin());
  }

  /** Acquire an update latch. */
  void u_lock(
#ifdef UNIV_PFS_RWLOCK
    const char *file, unsigned line,
#endif
    index_lock *lock)
  {
    memo_push(lock, MTR_MEMO_SX_LOCK);
    lock->u_lock(SRW_LOCK_ARGS_(file, line) spin_control::skip_spin());
  }

  /** Acquire an exclusive tablespace latch.
  @param space  tablespace */
  void x_lock_space(fil_space_t *space);

  /** Release an index latch. */
  void release(const index_lock &lock) { release(&lock); }
  /** Release a latch to an unmodified page. */
  void release(const buf_block_t &block) { release(&block); }
private:
  /** Release an unmodified object. */
  void release(const void *object);
public:
  /** Mark the given latched page as modified.
  @param block   page that will be modified */
  void set_modified(const buf_block_t &block);

  /** Set the state to not-modified. This will not log the changes.
  This is only used during redo log apply, to avoid logging the changes. */
  void discard_modifications() { m_modifications= false; }

  /** Get the LSN of commit().
  @return the commit LSN
  @retval 0 if the transaction only modified temporary tablespaces */
  lsn_t commit_lsn() const { ut_ad(has_committed()); return m_commit_lsn; }

  /** Note that some pages have been freed */
  void set_trim_pages() { m_trim_pages= true; }

  /** Latch a buffer pool block.
  @param block    block to be latched
  @param rw_latch RW_S_LATCH, RW_SX_LATCH, RW_X_LATCH, RW_NO_LATCH */
  void page_lock(buf_block_t *block, ulint rw_latch);

  /** Acquire a latch on a buffer-fixed buffer pool block.
  @param savepoint   savepoint location of the buffer-fixed block
  @param rw_latch    latch to acquire */
  void upgrade_buffer_fix(ulint savepoint, rw_lock_type_t rw_latch);

  /** Register a change to the page latch state. */
  void lock_register(ulint savepoint, mtr_memo_type_t type)
  {
    mtr_memo_slot_t &slot= m_memo[savepoint];
    ut_ad(slot.type <= MTR_MEMO_BUF_FIX);
    ut_ad(type < MTR_MEMO_S_LOCK);
    slot.type= type;
  }

  /** Upgrade U locks on a block to X */
  void page_lock_upgrade(const buf_block_t &block);

  /** Upgrade index U lock to X */
  ATTRIBUTE_COLD void index_lock_upgrade();

  /** Check if we are holding tablespace latch
  @param space  tablespace to search for
  @return whether space.latch is being held */
  bool memo_contains(const fil_space_t& space) const
    MY_ATTRIBUTE((warn_unused_result));
#ifdef UNIV_DEBUG
  /** Check if we are holding an rw-latch in this mini-transaction
  @param lock   latch to search for
  @param type   held latch type
  @return whether (lock,type) is contained */
  bool memo_contains(const index_lock &lock, mtr_memo_type_t type) const
    MY_ATTRIBUTE((warn_unused_result));

  /** Check if memo contains an index or buffer block latch.
  @param object    object to search
  @param flags     specify types of object latches
  @return true if contains */
  bool memo_contains_flagged(const void *object, ulint flags) const
    MY_ATTRIBUTE((warn_unused_result, nonnull));

  /** Check if memo contains the given page.
  @param ptr   pointer to within page frame
  @param flags types latch to look for
  @return the block
  @retval nullptr    if not found */
  buf_block_t *memo_contains_page_flagged(const byte *ptr, ulint flags) const;

  /** @return whether this mini-transaction modifies persistent data */
  bool has_modifications() const { return m_modifications; }
#endif /* UNIV_DEBUG */

  /** Push a buffer page to an the memo.
  @param block  buffer block
  @param type	object type: MTR_MEMO_S_LOCK, ... */
  void memo_push(buf_block_t *block, mtr_memo_type_t type)
    __attribute__((nonnull))
  {
    ut_ad(is_active());
    ut_ad(type <= MTR_MEMO_PAGE_SX_MODIFY);
    ut_ad(block->page.buf_fix_count());
    ut_ad(block->page.in_file());
#ifdef UNIV_DEBUG
    switch (type) {
    case MTR_MEMO_PAGE_S_FIX:
      ut_ad(block->page.lock.have_s());
      break;
    case MTR_MEMO_PAGE_X_FIX: case MTR_MEMO_PAGE_X_MODIFY:
      ut_ad(block->page.lock.have_x());
      break;
    case MTR_MEMO_PAGE_SX_FIX: case MTR_MEMO_PAGE_SX_MODIFY:
      ut_ad(block->page.lock.have_u_or_x());
      break;
    case MTR_MEMO_BUF_FIX:
      break;
    case MTR_MEMO_MODIFY:
    case MTR_MEMO_S_LOCK: case MTR_MEMO_X_LOCK: case MTR_MEMO_SX_LOCK:
    case MTR_MEMO_SPACE_X_LOCK:
      ut_ad("invalid type" == 0);
    }
#endif
    if (!(type & MTR_MEMO_MODIFY));
    else if (block->page.id().space() >= SRV_TMP_SPACE_ID)
    {
      block->page.set_temp_modified();
      type= mtr_memo_type_t(type & ~MTR_MEMO_MODIFY);
    }
    else
    {
      m_modifications= true;
      if (!m_made_dirty)
        /* If we are going to modify a previously clean persistent page,
        we must set m_made_dirty, so that commit() will acquire
        log_sys.flush_order_mutex and insert the block into
        buf_pool.flush_list. */
        m_made_dirty= block->page.oldest_modification() <= 1;
    }
    m_memo.emplace_back(mtr_memo_slot_t{block, type});
  }

  /** Push an index lock or tablespace latch to the memo.
  @param object index lock or tablespace latch
  @param type	object type: MTR_MEMO_S_LOCK, ... */
  void memo_push(void *object, mtr_memo_type_t type) __attribute__((nonnull))
  {
    ut_ad(is_active());
    ut_ad(type >= MTR_MEMO_S_LOCK);
    m_memo.emplace_back(mtr_memo_slot_t{object, type});
  }

  /** @return the size of the log is empty */
  size_t get_log_size() const { return m_log.size(); }
  /** @return whether the log and memo are empty */
  bool is_empty() const { return !get_savepoint() && !get_log_size(); }

  /** Write an OPT_PAGE_CHECKSUM record. */
  inline void page_checksum(const buf_page_t &bpage);

  /** Write request types */
  enum write_type
  {
    /** the page is guaranteed to always change */
    NORMAL= 0,
    /** optional: the page contents might not change */
    MAYBE_NOP,
    /** force a write, even if the page contents is not changing */
    FORCED
  };

  /** Write 1, 2, 4, or 8 bytes to a file page.
  @param[in]      block   file page
  @param[in,out]  ptr     pointer in file page
  @param[in]      val     value to write
  @tparam l       number of bytes to write
  @tparam w       write request type
  @tparam V       type of val
  @return whether any log was written */
  template<unsigned l,write_type w= NORMAL,typename V>
  inline bool write(const buf_block_t &block, void *ptr, V val)
    MY_ATTRIBUTE((nonnull));

  /** Log a write of a byte string to a page.
  @param[in]      b       buffer page
  @param[in]      ofs     byte offset from b->frame
  @param[in]      len     length of the data to write */
  inline void memcpy(const buf_block_t &b, ulint ofs, ulint len);

  /** Write a byte string to a page.
  @param[in,out]  b       buffer page
  @param[in]      dest    destination within b.frame
  @param[in]      str     the data to write
  @param[in]      len     length of the data to write
  @tparam w       write request type */
  template<write_type w= NORMAL>
  inline void memcpy(const buf_block_t &b, void *dest, const void *str,
                     ulint len);

  /** Log a write of a byte string to a ROW_FORMAT=COMPRESSED page.
  @param[in]      b       ROW_FORMAT=COMPRESSED index page
  @param[in]      offset  byte offset from b.zip.data
  @param[in]      len     length of the data to write */
  inline void zmemcpy(const buf_block_t &b, ulint offset, ulint len);

  /** Write a byte string to a ROW_FORMAT=COMPRESSED page.
  @param[in]      b       ROW_FORMAT=COMPRESSED index page
  @param[in]      dest    destination within b.zip.data
  @param[in]      str     the data to write
  @param[in]      len     length of the data to write
  @tparam w       write request type */
  template<write_type w= NORMAL>
  inline void zmemcpy(const buf_block_t &b, void *dest, const void *str,
                      ulint len);

  /** Log an initialization of a string of bytes.
  @param[in]      b       buffer page
  @param[in]      ofs     byte offset from b->frame
  @param[in]      len     length of the data to write
  @param[in]      val     the data byte to write */
  inline void memset(const buf_block_t &b, ulint ofs, ulint len, byte val);

  /** Initialize a string of bytes.
  @param[in,out]        b       buffer page
  @param[in]            ofs     byte offset from b->frame
  @param[in]            len     length of the data to write
  @param[in]            val     the data byte to write */
  inline void memset(const buf_block_t *b, ulint ofs, ulint len, byte val);

  /** Log an initialization of a repeating string of bytes.
  @param[in]      b       buffer page
  @param[in]      ofs     byte offset from b->frame
  @param[in]      len     length of the data to write, in bytes
  @param[in]      str     the string to write
  @param[in]      size    size of str, in bytes */
  inline void memset(const buf_block_t &b, ulint ofs, size_t len,
                     const void *str, size_t size);

  /** Initialize a repeating string of bytes.
  @param[in,out]  b       buffer page
  @param[in]      ofs     byte offset from b->frame
  @param[in]      len     length of the data to write, in bytes
  @param[in]      str     the string to write
  @param[in]      size    size of str, in bytes */
  inline void memset(const buf_block_t *b, ulint ofs, size_t len,
                     const void *str, size_t size);

  /** Log that a string of bytes was copied from the same page.
  @param[in]      b       buffer page
  @param[in]      d       destination offset within the page
  @param[in]      s       source offset within the page
  @param[in]      len     length of the data to copy */
  inline void memmove(const buf_block_t &b, ulint d, ulint s, ulint len);

  /** Initialize an entire page.
  @param[in,out]        b       buffer page */
  void init(buf_block_t *b);
  /** Free a page.
  @param space   tablespace
  @param offset  offset of the page to be freed */
  void free(const fil_space_t &space, uint32_t offset);
  /** Write log for partly initializing a B-tree or R-tree page.
  @param block    B-tree or R-tree page
  @param comp     false=ROW_FORMAT=REDUNDANT, true=COMPACT or DYNAMIC */
  inline void page_create(const buf_block_t &block, bool comp);

  /** Write log for inserting a B-tree or R-tree record in
  ROW_FORMAT=REDUNDANT.
  @param block      B-tree or R-tree page
  @param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
  @param prev_rec   byte offset of the predecessor of the record to insert,
                    starting from PAGE_OLD_INFIMUM
  @param info_bits  info_bits of the record
  @param n_fields_s number of fields << 1 | rec_get_1byte_offs_flag()
  @param hdr_c      number of common record header bytes with prev_rec
  @param data_c     number of common data bytes with prev_rec
  @param hdr        record header bytes to copy to the log
  @param hdr_l      number of copied record header bytes
  @param data       record payload bytes to copy to the log
  @param data_l     number of copied record data bytes */
  inline void page_insert(const buf_block_t &block, bool reuse,
                          ulint prev_rec, byte info_bits,
                          ulint n_fields_s, size_t hdr_c, size_t data_c,
                          const byte *hdr, size_t hdr_l,
                          const byte *data, size_t data_l);
  /** Write log for inserting a B-tree or R-tree record in
  ROW_FORMAT=COMPACT or ROW_FORMAT=DYNAMIC.
  @param block       B-tree or R-tree page
  @param reuse       false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
  @param prev_rec    byte offset of the predecessor of the record to insert,
                     starting from PAGE_NEW_INFIMUM
  @param info_status rec_get_info_and_status_bits()
  @param shift       unless !reuse: number of bytes the PAGE_FREE is moving
  @param hdr_c       number of common record header bytes with prev_rec
  @param data_c      number of common data bytes with prev_rec
  @param hdr         record header bytes to copy to the log
  @param hdr_l       number of copied record header bytes
  @param data        record payload bytes to copy to the log
  @param data_l      number of copied record data bytes */
  inline void page_insert(const buf_block_t &block, bool reuse,
                          ulint prev_rec, byte info_status,
                          ssize_t shift, size_t hdr_c, size_t data_c,
                          const byte *hdr, size_t hdr_l,
                          const byte *data, size_t data_l);
  /** Write log for deleting a B-tree or R-tree record in ROW_FORMAT=REDUNDANT.
  @param block      B-tree or R-tree page
  @param prev_rec   byte offset of the predecessor of the record to delete,
                    starting from PAGE_OLD_INFIMUM */
  inline void page_delete(const buf_block_t &block, ulint prev_rec);
  /** Write log for deleting a COMPACT or DYNAMIC B-tree or R-tree record.
  @param block      B-tree or R-tree page
  @param prev_rec   byte offset of the predecessor of the record to delete,
                    starting from PAGE_NEW_INFIMUM
  @param hdr_size   record header size, excluding REC_N_NEW_EXTRA_BYTES
  @param data_size  data payload size, in bytes */
  inline void page_delete(const buf_block_t &block, ulint prev_rec,
                          size_t hdr_size, size_t data_size);

  /** Write log for initializing an undo log page.
  @param block    undo page */
  inline void undo_create(const buf_block_t &block);
  /** Write log for appending an undo log record.
  @param block    undo page
  @param data     record within the undo page
  @param len      length of the undo record, in bytes */
  inline void undo_append(const buf_block_t &block,
                          const void *data, size_t len);
  /** Trim the end of a tablespace.
  @param id       first page identifier that will not be in the file */
  inline void trim_pages(const page_id_t id);

  /** Write a log record about a file operation.
  @param type           file operation
  @param space_id       tablespace identifier
  @param path           file path
  @param new_path       new file path for type=FILE_RENAME */
  inline void log_file_op(mfile_type_t type, uint32_t space_id,
                          const char *path,
                          const char *new_path= nullptr);

  /** Add freed page numbers to freed_pages */
  void add_freed_offset(fil_space_t *space, uint32_t page)
  {
    ut_ad(is_named_space(space));
    if (!m_freed_pages)
    {
      m_freed_pages= new range_set();
      ut_ad(!m_freed_space);
      m_freed_space= space;
    }
    else
      ut_ad(m_freed_space == space);
    m_freed_pages->add_value(page);
  }

  /** Determine the added buffer fix count of a block.
  @param block block to be checked
  @return number of buffer count added by this mtr */
  uint32_t get_fix_count(const buf_block_t *block) const;

  /** Note that log_sys.latch is no longer being held exclusively. */
  void flag_wr_unlock() noexcept { ut_ad(m_latch_ex); m_latch_ex= false; }

  /** type of page flushing is needed during commit() */
  enum page_flush_ahead
  {
    /** no need to trigger page cleaner */
    PAGE_FLUSH_NO= 0,
    /** asynchronous flushing is needed */
    PAGE_FLUSH_ASYNC,
    /** furious flushing is needed */
    PAGE_FLUSH_SYNC
  };

private:
  /** Handle any pages that were freed during the mini-transaction. */
  void process_freed_pages();
  /** Release modified pages when no log was written. */
  void release_unlogged();

  /** Log a write of a byte string to a page.
  @param block   buffer page
  @param offset  byte offset within page
  @param data    data to be written
  @param len     length of the data, in bytes */
  inline void memcpy_low(const buf_block_t &block, uint16_t offset,
                         const void *data, size_t len);
  /**
  Write a log record.
  @tparam type  redo log record type
  @param id     persistent page identifier
  @param bpage  buffer pool page, or nullptr
  @param len    number of additional bytes to write
  @param alloc  whether to allocate the additional bytes
  @param offset byte offset, or 0 if the record type does not allow one
  @return end of mini-transaction log, minus len */
  template<byte type>
  inline byte *log_write(const page_id_t id, const buf_page_t *bpage,
                         size_t len= 0, bool alloc= false, size_t offset= 0);

  /** Write an EXTENDED log record.
  @param block  buffer pool page
  @param type   extended record subtype; @see mrec_ext_t */
  inline void log_write_extended(const buf_block_t &block, byte type);

  /** Write a FILE_MODIFY record when a non-predefined persistent
  tablespace was modified for the first time since fil_names_clear(). */
  ATTRIBUTE_NOINLINE ATTRIBUTE_COLD void name_write() noexcept;

  /** Encrypt the log */
  ATTRIBUTE_NOINLINE void encrypt();

  /** Commit the mini-transaction log.
  @tparam pmem log_sys.is_mmap()
  @param mtr   mini-transaction
  @param lsns  {start_lsn,flush_ahead} */
  template<bool pmem>
  static void commit_log(mtr_t *mtr, std::pair<lsn_t,page_flush_ahead> lsns)
    noexcept;

  /** Append the redo log records to the redo log buffer.
  @return {start_lsn,flush_ahead} */
  std::pair<lsn_t,page_flush_ahead> do_write();

  /** Append the redo log records to the redo log buffer.
  @tparam mmap log_sys.is_mmap()
  @param mtr   mini-transaction
  @param len   number of bytes to write
  @return {start_lsn,flush_ahead} */
  template<bool mmap> static
  std::pair<lsn_t,page_flush_ahead> finish_writer(mtr_t *mtr, size_t len);

  /** The applicable variant of commit_log() */
  static void (*commit_logger)(mtr_t *, std::pair<lsn_t,page_flush_ahead>);
  /** The applicable variant of finish_writer() */
  static std::pair<lsn_t,page_flush_ahead> (*finisher)(mtr_t *, size_t);

  std::pair<lsn_t,page_flush_ahead> finish_write(size_t len)
  { return finisher(this, len); }
public:
  /** Update finisher when spin_wait_delay is changing to or from 0. */
  static void finisher_update();
private:

  /** Release all latches. */
  void release();
  /** Release the resources */
  inline void release_resources();

#ifdef UNIV_DEBUG
public:
  /** @return whether the mini-transaction is active */
  bool is_active() const
  { ut_ad(!m_commit || m_start); return m_start && !m_commit; }
  /** @return whether the mini-transaction has been committed */
  bool has_committed() const { ut_ad(!m_commit || m_start); return m_commit; }
  /** @return whether the mini-transaction is freeing an index tree */
  bool is_freeing_tree() const { return m_freeing_tree; }
  /** Notify that the mini-transaction is freeing an index tree */
  void freeing_tree() { m_freeing_tree= true; }
private:
  /** whether start() has been called */
  bool m_start= false;
  /** whether commit() has been called */
  bool m_commit= false;
  /** whether freeing_tree() has been called */
  bool m_freeing_tree= false;
#endif
private:
  /** The page of the most recent m_log record written, or NULL */
  const buf_page_t* m_last;
  /** The current byte offset in m_last, or 0 */
  uint16_t m_last_offset;

  /** specifies which operations should be logged; default MTR_LOG_ALL */
  uint16_t m_log_mode:2;

  /** whether at least one persistent page was written to */
  uint16_t m_modifications:1;

  /** whether at least one previously clean buffer pool page was written to */
  uint16_t m_made_dirty:1;

  /** whether log_sys.latch is locked exclusively */
  uint16_t m_latch_ex:1;

  /** whether the pages has been trimmed */
  uint16_t m_trim_pages:1;

  /** CRC-32C of m_log */
  uint32_t m_crc;

#ifdef UNIV_DEBUG
  /** Persistent user tablespace associated with the
  mini-transaction, or 0 (TRX_SYS_SPACE) if none yet */
  uint32_t m_user_space_id;
#endif /* UNIV_DEBUG */

  /** acquired dict_index_t::lock, fil_space_t::latch, buf_block_t */
  small_vector<mtr_memo_slot_t, 16> m_memo;

  /** mini-transaction log */
  mtr_buf_t m_log;

  /** user tablespace that is being modified by the mini-transaction */
  fil_space_t* m_user_space;

  /** LSN at commit time */
  lsn_t m_commit_lsn;

  /** tablespace where pages have been freed */
  fil_space_t *m_freed_space= nullptr;
  /** set of freed page ids */
  range_set *m_freed_pages= nullptr;
};
