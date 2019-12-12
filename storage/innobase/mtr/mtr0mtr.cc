/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "fsp0sysspace.h"
#include "page0types.h"
#include "mtr0log.h"
#include "log0recv.h"

/** Iterate over a memo block in reverse. */
template <typename Functor>
struct CIterate {
	CIterate() : functor() {}

	CIterate(const Functor& functor) : functor(functor) {}

	/** @return false if the functor returns false. */
	bool operator()(mtr_buf_t::block_t* block) const
	{
		const mtr_memo_slot_t*	start =
			reinterpret_cast<const mtr_memo_slot_t*>(
				block->begin());

		mtr_memo_slot_t*	slot =
			reinterpret_cast<mtr_memo_slot_t*>(
				block->end());

		ut_ad(!(block->used() % sizeof(*slot)));

		while (slot-- != start) {

			if (!functor(slot)) {
				return(false);
			}
		}

		return(true);
	}

	Functor functor;
};

template <typename Functor>
struct Iterate {
	Iterate() : functor() {}

	Iterate(const Functor& functor) : functor(functor) {}

	/** @return false if the functor returns false. */
	bool operator()(mtr_buf_t::block_t* block)
	{
		const mtr_memo_slot_t*	start =
			reinterpret_cast<const mtr_memo_slot_t*>(
				block->begin());

		mtr_memo_slot_t*	slot =
			reinterpret_cast<mtr_memo_slot_t*>(
				block->end());

		ut_ad(!(block->used() % sizeof(*slot)));

		while (slot-- != start) {

			if (!functor(slot)) {
				return(false);
			}
		}

		return(true);
	}

	Functor functor;
};

/** Find specific object */
struct Find {

	/** Constructor */
	Find(const void* object, ulint type)
		:
		m_slot(),
		m_type(type),
		m_object(object)
	{
		ut_a(object != NULL);
	}

	/** @return false if the object was found. */
	bool operator()(mtr_memo_slot_t* slot)
	{
		if (m_object == slot->object && m_type == slot->type) {
			m_slot = slot;
			return(false);
		}

		return(true);
	}

	/** Slot if found */
	mtr_memo_slot_t*m_slot;

	/** Type of the object to look for */
	ulint		m_type;

	/** The object instance to look for */
	const void*	m_object;
};

/** Find a page frame */
struct FindPage
{
	/** Constructor
	@param[in]	ptr	pointer to within a page frame
	@param[in]	flags	MTR_MEMO flags to look for */
	FindPage(const void* ptr, ulint flags)
		: m_ptr(ptr), m_flags(flags), m_slot(NULL)
	{
		/* There must be some flags to look for. */
		ut_ad(flags);
		/* We can only look for page-related flags. */
		ut_ad(!(flags & ulint(~(MTR_MEMO_PAGE_S_FIX
					| MTR_MEMO_PAGE_X_FIX
					| MTR_MEMO_PAGE_SX_FIX
					| MTR_MEMO_BUF_FIX
					| MTR_MEMO_MODIFY))));
	}

	/** Visit a memo entry.
	@param[in]	slot	memo entry to visit
	@retval	false	if a page was found
	@retval	true	if the iteration should continue */
	bool operator()(mtr_memo_slot_t* slot)
	{
		ut_ad(m_slot == NULL);

		if (!(m_flags & slot->type) || slot->object == NULL) {
			return(true);
		}

		buf_block_t* block = reinterpret_cast<buf_block_t*>(
			slot->object);

		if (m_ptr < block->frame
		    || m_ptr >= block->frame + srv_page_size) {
			return(true);
		}

		ut_ad(!(m_flags & (MTR_MEMO_PAGE_S_FIX
				   | MTR_MEMO_PAGE_SX_FIX
				   | MTR_MEMO_PAGE_X_FIX))
		      || rw_lock_own_flagged(&block->lock, m_flags));

		m_slot = slot;
		return(false);
	}

	/** @return the slot that was found */
	mtr_memo_slot_t* get_slot() const
	{
		ut_ad(m_slot != NULL);
		return(m_slot);
	}
	/** @return the block that was found */
	buf_block_t* get_block() const
	{
		return(reinterpret_cast<buf_block_t*>(get_slot()->object));
	}
private:
	/** Pointer inside a page frame to look for */
	const void*const	m_ptr;
	/** MTR_MEMO flags to look for */
	const ulint		m_flags;
	/** The slot corresponding to m_ptr */
	mtr_memo_slot_t*	m_slot;
};

/** Release latches and decrement the buffer fix count.
@param slot	memo slot */
static void memo_slot_release(mtr_memo_slot_t *slot)
{
  switch (slot->type) {
#ifdef UNIV_DEBUG
  default:
    ut_ad(!"invalid type");
    break;
  case MTR_MEMO_MODIFY:
    break;
#endif /* UNIV_DEBUG */
  case MTR_MEMO_S_LOCK:
    rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  case MTR_MEMO_SX_LOCK:
    rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  case MTR_MEMO_X_LOCK:
    rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  case MTR_MEMO_BUF_FIX:
  case MTR_MEMO_PAGE_S_FIX:
  case MTR_MEMO_PAGE_SX_FIX:
  case MTR_MEMO_PAGE_X_FIX:
    buf_block_t *block= reinterpret_cast<buf_block_t*>(slot->object);
    block->unfix();
    buf_page_release_latch(block, slot->type);
    break;
  }
  slot->object= NULL;
}

/** Release the latches acquired by the mini-transaction. */
struct ReleaseLatches {
  /** @return true always. */
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (!slot->object)
      return true;
    switch (slot->type) {
#ifdef UNIV_DEBUG
    default:
      ut_ad(!"invalid type");
      break;
    case MTR_MEMO_MODIFY:
      break;
#endif /* UNIV_DEBUG */
    case MTR_MEMO_S_LOCK:
      rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    case MTR_MEMO_X_LOCK:
      rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    case MTR_MEMO_SX_LOCK:
      rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    case MTR_MEMO_BUF_FIX:
    case MTR_MEMO_PAGE_S_FIX:
    case MTR_MEMO_PAGE_SX_FIX:
    case MTR_MEMO_PAGE_X_FIX:
      buf_block_t *block= reinterpret_cast<buf_block_t*>(slot->object);
      block->unfix();
      buf_page_release_latch(block, slot->type);
      break;
    }
    slot->object= NULL;
    return true;
  }
};

/** Release the latches and blocks acquired by the mini-transaction. */
struct ReleaseAll {
  /** @return true always. */
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (slot->object)
      memo_slot_release(slot);
    return true;
  }
};

#ifdef UNIV_DEBUG
/** Check that all slots have been handled. */
struct DebugCheck {
	/** @return true always. */
	bool operator()(const mtr_memo_slot_t* slot) const
	{
		ut_ad(!slot->object);
		return(true);
	}
};
#endif

/** Release a resource acquired by the mini-transaction. */
struct ReleaseBlocks {
	/** Release specific object */
	ReleaseBlocks(lsn_t start_lsn, lsn_t end_lsn, FlushObserver* observer)
		:
		m_end_lsn(end_lsn),
		m_start_lsn(start_lsn),
		m_flush_observer(observer)
	{
		/* Do nothing */
	}

	/** Add the modified page to the buffer flush list. */
	void add_dirty_page_to_flush_list(mtr_memo_slot_t* slot) const
	{
		ut_ad(m_end_lsn > 0);
		ut_ad(m_start_lsn > 0);

		buf_block_t*	block;

		block = reinterpret_cast<buf_block_t*>(slot->object);

		buf_flush_note_modification(block, m_start_lsn,
					    m_end_lsn, m_flush_observer);
	}

	/** @return true always. */
	bool operator()(mtr_memo_slot_t* slot) const
	{
		if (slot->object != NULL) {

			if (slot->type == MTR_MEMO_PAGE_X_FIX
			    || slot->type == MTR_MEMO_PAGE_SX_FIX) {

				add_dirty_page_to_flush_list(slot);
			}
		}

		return(true);
	}

	/** Mini-transaction REDO start LSN */
	lsn_t		m_end_lsn;

	/** Mini-transaction REDO end LSN */
	lsn_t		m_start_lsn;

	/** Flush observer */
	FlushObserver*	m_flush_observer;
};

/** Write the block contents to the REDO log */
struct mtr_write_log_t {
	/** Append a block to the redo log buffer.
	@return whether the appending should continue */
	bool operator()(const mtr_buf_t::block_t* block) const
	{
		log_write_low(block->begin(), block->used());
		return(true);
	}
};

/** Append records to the system-wide redo log buffer.
@param[in]	log	redo log records */
void
mtr_write_log(
	const mtr_buf_t*	log)
{
	const ulint	len = log->size();
	mtr_write_log_t	write_log;

	ut_ad(!recv_no_log_write);
	DBUG_PRINT("ib_log",
		   (ULINTPF " extra bytes written at " LSN_PF,
		    len, log_sys.lsn));

	log_reserve_and_open(len);
	log->for_each_block(write_log);
	log_close();
}

/** Start a mini-transaction. */
void mtr_t::start()
{
  UNIV_MEM_INVALID(this, sizeof *this);

  new(&m_memo) mtr_buf_t();
  new(&m_log) mtr_buf_t();

  m_made_dirty= false;
  m_inside_ibuf= false;
  m_modifications= false;
  m_n_log_recs= 0;
  m_log_mode= MTR_LOG_ALL;
  ut_d(m_user_space_id= TRX_SYS_SPACE);
  m_user_space= NULL;
  m_state= MTR_STATE_ACTIVE;
  m_flush_observer= NULL;
  m_commit_lsn= 0;
}

/** Release the resources */
inline void mtr_t::release_resources()
{
  ut_d(m_memo.for_each_block_in_reverse(CIterate<DebugCheck>()));
  m_log.erase();
  m_memo.erase();
  m_state= MTR_STATE_COMMITTED;
}

/** Commit a mini-transaction. */
void
mtr_t::commit()
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());

  /* This is a dirty read, for debugging. */
  ut_ad(!m_modifications || !recv_no_log_write);
  ut_ad(!m_modifications || m_log_mode != MTR_LOG_NONE);

  if (m_modifications
      && (m_n_log_recs || m_log_mode == MTR_LOG_NO_REDO))
  {
    ut_ad(!srv_read_only_mode || m_log_mode == MTR_LOG_NO_REDO);

    lsn_t start_lsn;

    if (const ulint len= prepare_write())
      start_lsn= finish_write(len);
    else
      start_lsn= m_commit_lsn;

    if (m_made_dirty)
      log_flush_order_mutex_enter();

    /* It is now safe to release the log mutex because the
    flush_order mutex will ensure that we are the first one
    to insert into the flush list. */
    log_mutex_exit();

    m_memo.for_each_block_in_reverse(CIterate<const ReleaseBlocks>
                                     (ReleaseBlocks(start_lsn, m_commit_lsn,
                                                    m_flush_observer)));
    if (m_made_dirty)
      log_flush_order_mutex_exit();

    m_memo.for_each_block_in_reverse(CIterate<ReleaseLatches>());
  }
  else
    m_memo.for_each_block_in_reverse(CIterate<ReleaseAll>());

  release_resources();
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
MLOG_FILE_NAME records and a MLOG_CHECKPOINT marker.
The caller must invoke log_mutex_enter() and log_mutex_exit().
This is to be used at log_checkpoint().
@param[in]	checkpoint_lsn		the LSN of the log checkpoint
@param[in]	write_mlog_checkpoint	Write MLOG_CHECKPOINT marker
					if it is enabled. */
void
mtr_t::commit_checkpoint(
	lsn_t	checkpoint_lsn,
	bool	write_mlog_checkpoint)
{
	ut_ad(log_mutex_own());
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(get_log_mode() == MTR_LOG_ALL);
	ut_ad(!m_made_dirty);
	ut_ad(m_memo.size() == 0);
	ut_ad(!srv_read_only_mode);
	ut_ad(write_mlog_checkpoint || m_n_log_recs > 1);

	switch (m_n_log_recs) {
	case 0:
		break;
	case 1:
		*m_log.front()->begin() |= MLOG_SINGLE_REC_FLAG;
		break;
	default:
		mlog_catenate_ulint(&m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
	}

	if (write_mlog_checkpoint) {
		byte*	ptr = m_log.push<byte*>(SIZE_OF_MLOG_CHECKPOINT);
		compile_time_assert(SIZE_OF_MLOG_CHECKPOINT == 1 + 8);
		*ptr = MLOG_CHECKPOINT;
		mach_write_to_8(ptr + 1, checkpoint_lsn);
	}

	finish_write(m_log.size());
	release_resources();

	if (write_mlog_checkpoint) {
		DBUG_PRINT("ib_log",
			   ("MLOG_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
			    checkpoint_lsn, log_sys.lsn));
	}
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a MLOG_FILE_NAME record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(ulint space) const
{
	ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

	switch (get_log_mode()) {
	case MTR_LOG_NONE:
	case MTR_LOG_NO_REDO:
		return(true);
	case MTR_LOG_ALL:
	case MTR_LOG_SHORT_INSERTS:
		return(m_user_space_id == space
		       || is_predefined_tablespace(space));
	}

	ut_error;
	return(false);
}
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a MLOG_FILE_NAME record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool mtr_t::is_named_space(const fil_space_t* space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

  switch (get_log_mode()) {
  case MTR_LOG_NONE:
  case MTR_LOG_NO_REDO:
    return true;
  case MTR_LOG_ALL:
  case MTR_LOG_SHORT_INSERTS:
    return m_user_space == space || is_predefined_tablespace(space->id);
  }

  ut_error;
  return false;
}
#endif /* UNIV_DEBUG */

/** Acquire a tablespace X-latch.
NOTE: use mtr_x_lock_space().
@param[in]	space_id	tablespace ID
@param[in]	file		file name from where called
@param[in]	line		line number in file
@return the tablespace object (never NULL) */
fil_space_t*
mtr_t::x_lock_space(ulint space_id, const char* file, unsigned line)
{
	fil_space_t*	space;

	ut_ad(is_active());

	if (space_id == TRX_SYS_SPACE) {
		space = fil_system.sys_space;
	} else if ((space = m_user_space) && space_id == space->id) {
	} else {
		space = fil_space_get(space_id);
		ut_ad(get_log_mode() != MTR_LOG_NO_REDO
		      || space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT
		      || space->redo_skipped_count > 0);
	}

	ut_ad(space);
	ut_ad(space->id == space_id);
	x_lock_space(space, file, line);
	return(space);
}

/** Release an object in the memo stack.
@return true if released */
bool
mtr_t::memo_release(const void* object, ulint type)
{
	ut_ad(is_active());

	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!m_modifications || type != MTR_MEMO_PAGE_X_FIX);

	Iterate<Find> iteration(Find(object, type));

	if (!m_memo.for_each_block_in_reverse(iteration)) {
		memo_slot_release(iteration.functor.m_slot);
		return(true);
	}

	return(false);
}

/** Release a page latch.
@param[in]	ptr	pointer to within a page frame
@param[in]	type	object type: MTR_MEMO_PAGE_X_FIX, ... */
void
mtr_t::release_page(const void* ptr, mtr_memo_type_t type)
{
	ut_ad(is_active());

	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!m_modifications || type != MTR_MEMO_PAGE_X_FIX);

	Iterate<FindPage> iteration(FindPage(ptr, type));

	if (!m_memo.for_each_block_in_reverse(iteration)) {
		memo_slot_release(iteration.functor.get_slot());
		return;
	}

	/* The page was not found! */
	ut_ad(0);
}

/** Prepare to write the mini-transaction log to the redo log buffer.
@return number of bytes to write in finish_write() */
inline ulint mtr_t::prepare_write()
{
	ut_ad(!recv_no_log_write);

	if (UNIV_UNLIKELY(m_log_mode != MTR_LOG_ALL)) {
		ut_ad(m_log_mode == MTR_LOG_NO_REDO);
		ut_ad(m_log.size() == 0);
		log_mutex_enter();
		m_commit_lsn = log_sys.lsn;
		return 0;
	}

	ulint	len	= m_log.size();
	ulint	n_recs	= m_n_log_recs;
	ut_ad(len > 0);
	ut_ad(n_recs > 0);

	if (len > srv_log_buffer_size / 2) {
		log_buffer_extend(ulong((len + 1) * 2));
	}

	ut_ad(m_n_log_recs == n_recs);

	fil_space_t*	space = m_user_space;

	if (space != NULL && is_predefined_tablespace(space->id)) {
		/* Omit MLOG_FILE_NAME for predefined tablespaces. */
		space = NULL;
	}

	log_mutex_enter();

	if (fil_names_write_if_was_clean(space, this)) {
		/* This mini-transaction was the first one to modify
		this tablespace since the latest checkpoint, so
		some MLOG_FILE_NAME records were appended to m_log. */
		ut_ad(m_n_log_recs > n_recs);
		mlog_catenate_ulint(&m_log, MLOG_MULTI_REC_END, MLOG_1BYTE);
		len = m_log.size();
	} else {
		/* This was not the first time of dirtying a
		tablespace since the latest checkpoint. */

		ut_ad(n_recs == m_n_log_recs);

		if (n_recs <= 1) {
			ut_ad(n_recs == 1);

			/* Flag the single log record as the
			only record in this mini-transaction. */
			*m_log.front()->begin() |= MLOG_SINGLE_REC_FLAG;
		} else {
			/* Because this mini-transaction comprises
			multiple log records, append MLOG_MULTI_REC_END
			at the end. */

			mlog_catenate_ulint(&m_log, MLOG_MULTI_REC_END,
					    MLOG_1BYTE);
			len++;
		}
	}

	/* check and attempt a checkpoint if exceeding capacity */
	log_margin_checkpoint_age(len);

	return(len);
}

/** Append the redo log records to the redo log buffer
@param[in] len	number of bytes to write
@return start_lsn */
inline lsn_t mtr_t::finish_write(ulint len)
{
	ut_ad(m_log_mode == MTR_LOG_ALL);
	ut_ad(log_mutex_own());
	ut_ad(m_log.size() == len);
	ut_ad(len > 0);

	lsn_t start_lsn;

	if (m_log.is_small()) {
		const mtr_buf_t::block_t* front = m_log.front();
		ut_ad(len <= front->used());

		m_commit_lsn = log_reserve_and_write_fast(front->begin(), len,
							  &start_lsn);

		if (m_commit_lsn) {
			return start_lsn;
		}
	}

	/* Open the database log for log_write_low */
	start_lsn = log_reserve_and_open(len);

	mtr_write_log_t	write_log;
	m_log.for_each_block(write_log);

	m_commit_lsn = log_close();
	return start_lsn;
}

#ifdef UNIV_DEBUG
/** Check if memo contains the given item.
@return	true if contains */
bool
mtr_t::memo_contains(
	const mtr_buf_t*	memo,
	const void*		object,
	ulint			type)
{
	Iterate<Find> iteration(Find(object, type));
	if (memo->for_each_block_in_reverse(iteration)) {
		return(false);
	}

	switch (type) {
	case MTR_MEMO_X_LOCK:
		ut_ad(rw_lock_own((rw_lock_t*) object, RW_LOCK_X));
		break;
	case MTR_MEMO_SX_LOCK:
		ut_ad(rw_lock_own((rw_lock_t*) object, RW_LOCK_SX));
		break;
	case MTR_MEMO_S_LOCK:
		ut_ad(rw_lock_own((rw_lock_t*) object, RW_LOCK_S));
		break;
	}

	return(true);
}

/** Debug check for flags */
struct FlaggedCheck {
	FlaggedCheck(const void* ptr, ulint flags)
		:
		m_ptr(ptr),
		m_flags(flags)
	{
		/* There must be some flags to look for. */
		ut_ad(flags);
		/* Look for rw-lock-related and page-related flags. */
		ut_ad(!(flags & ulint(~(MTR_MEMO_PAGE_S_FIX
					| MTR_MEMO_PAGE_X_FIX
					| MTR_MEMO_PAGE_SX_FIX
					| MTR_MEMO_BUF_FIX
					| MTR_MEMO_MODIFY
					| MTR_MEMO_X_LOCK
					| MTR_MEMO_SX_LOCK
					| MTR_MEMO_S_LOCK))));
		/* Either some rw-lock-related or page-related flags
		must be specified, but not both at the same time. */
		ut_ad(!(flags & (MTR_MEMO_PAGE_S_FIX
				 | MTR_MEMO_PAGE_X_FIX
				 | MTR_MEMO_PAGE_SX_FIX
				 | MTR_MEMO_BUF_FIX
				 | MTR_MEMO_MODIFY))
		      == !!(flags & (MTR_MEMO_X_LOCK
				     | MTR_MEMO_SX_LOCK
				     | MTR_MEMO_S_LOCK)));
	}

	/** Visit a memo entry.
	@param[in]	slot	memo entry to visit
	@retval	false	if m_ptr was found
	@retval	true	if the iteration should continue */
	bool operator()(const mtr_memo_slot_t* slot) const
	{
		if (m_ptr != slot->object || !(m_flags & slot->type)) {
			return(true);
		}

		if (ulint flags = m_flags & (MTR_MEMO_PAGE_S_FIX
					     | MTR_MEMO_PAGE_SX_FIX
					     | MTR_MEMO_PAGE_X_FIX)) {
			rw_lock_t* lock = &static_cast<buf_block_t*>(
				const_cast<void*>(m_ptr))->lock;
			ut_ad(rw_lock_own_flagged(lock, flags));
		} else {
			rw_lock_t* lock = static_cast<rw_lock_t*>(
				const_cast<void*>(m_ptr));
			ut_ad(rw_lock_own_flagged(lock, m_flags >> 5));
		}

		return(false);
	}

	const void*const	m_ptr;
	const ulint		m_flags;
};

/** Check if memo contains the given item.
@param object		object to search
@param flags		specify types of object (can be ORred) of
			MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */
bool
mtr_t::memo_contains_flagged(const void* ptr, ulint flags) const
{
	ut_ad(is_active());

	return !m_memo.for_each_block_in_reverse(
		CIterate<FlaggedCheck>(FlaggedCheck(ptr, flags)));
}

/** Check if memo contains the given page.
@param[in]	ptr	pointer to within buffer frame
@param[in]	flags	specify types of object with OR of
			MTR_MEMO_PAGE_S_FIX... values
@return	the block
@retval	NULL	if not found */
buf_block_t*
mtr_t::memo_contains_page_flagged(
	const byte*	ptr,
	ulint		flags) const
{
	Iterate<FindPage> iteration(FindPage(ptr, flags));
	return m_memo.for_each_block_in_reverse(iteration)
		? NULL : iteration.functor.get_block();
}

/** Mark the given latched page as modified.
@param[in]	ptr	pointer to within buffer frame */
void
mtr_t::memo_modify_page(const byte* ptr)
{
	buf_block_t*	block = memo_contains_page_flagged(
		ptr, MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX);
	ut_ad(block != NULL);

	if (!memo_contains(get_memo(), block, MTR_MEMO_MODIFY)) {
		memo_push(block, MTR_MEMO_MODIFY);
	}
}

/** Print info of an mtr handle. */
void
mtr_t::print() const
{
	ib::info() << "Mini-transaction handle: memo size "
		<< m_memo.size() << " bytes log size "
		<< get_log()->size() << " bytes";
}

#endif /* UNIV_DEBUG */
