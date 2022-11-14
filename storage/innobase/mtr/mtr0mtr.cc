/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0log.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "fsp0sysspace.h"
#include "page0types.h"
#include "log0recv.h"
#include "srv0start.h"
#include "log.h"

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
	const ulint	m_type;

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
  case MTR_MEMO_S_LOCK:
    rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  case MTR_MEMO_SX_LOCK:
    rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  case MTR_MEMO_SPACE_X_LOCK:
    {
      fil_space_t *space= static_cast<fil_space_t*>(slot->object);
      space->set_committed_size();
      rw_lock_x_unlock(&space->latch);
    }
    break;
  case MTR_MEMO_X_LOCK:
    rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
    break;
  default:
#ifdef UNIV_DEBUG
    switch (slot->type & ~MTR_MEMO_MODIFY) {
    case MTR_MEMO_BUF_FIX:
    case MTR_MEMO_PAGE_S_FIX:
    case MTR_MEMO_PAGE_SX_FIX:
    case MTR_MEMO_PAGE_X_FIX:
      break;
    default:
      ut_ad("invalid type" == 0);
      break;
    }
#endif /* UNIV_DEBUG */
    buf_block_t *block= reinterpret_cast<buf_block_t*>(slot->object);
    buf_page_release_latch(block, slot->type & ~MTR_MEMO_MODIFY);
    block->unfix();
    break;
  }
  slot->object= nullptr;
}

/** Release the latches acquired by the mini-transaction. */
struct ReleaseLatches {
  /** @return true always. */
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (!slot->object)
      return true;
    switch (slot->type) {
    case MTR_MEMO_S_LOCK:
      rw_lock_s_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    case MTR_MEMO_SPACE_X_LOCK:
      {
        fil_space_t *space= static_cast<fil_space_t*>(slot->object);
        space->set_committed_size();
        rw_lock_x_unlock(&space->latch);
      }
      break;
    case MTR_MEMO_X_LOCK:
      rw_lock_x_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    case MTR_MEMO_SX_LOCK:
      rw_lock_sx_unlock(reinterpret_cast<rw_lock_t*>(slot->object));
      break;
    default:
#ifdef UNIV_DEBUG
      switch (slot->type & ~MTR_MEMO_MODIFY) {
      case MTR_MEMO_BUF_FIX:
      case MTR_MEMO_PAGE_S_FIX:
      case MTR_MEMO_PAGE_SX_FIX:
      case MTR_MEMO_PAGE_X_FIX:
        break;
      default:
        ut_ad("invalid type" == 0);
        break;
      }
#endif /* UNIV_DEBUG */
      buf_block_t *block= reinterpret_cast<buf_block_t*>(slot->object);
      buf_page_release_latch(block, slot->type & ~MTR_MEMO_MODIFY);
      block->unfix();
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

/** Stops iteration is savepoint is reached */
template <typename Functor> struct TillSavepoint
{

  /** Constructor
  @param[in] functor functor which is called if savepoint is not reached
  @param[in] savepoint savepoint value to rollback
  @param[in] used current position in slots container */
  TillSavepoint(const Functor &functor, ulint savepoint, ulint used)
      : functor(functor),
        m_slots_count((used - savepoint) / sizeof(mtr_memo_slot_t))
  {
    ut_ad(savepoint);
    ut_ad(used >= savepoint);
  }

  /** @return true if savepoint is not reached, false otherwise */
  bool operator()(mtr_memo_slot_t *slot)
  {
#ifdef UNIV_DEBUG
    /** This check is added because the code is invoked only from
    row_search_mvcc() to release latches acquired during clustered index search
    for secondary index record. To make it more universal we could add one more
    member in this functor for debug build to pass only certain slot types,
    but this is currently not necessary. */
    switch (slot->type)
    {
    case MTR_MEMO_S_LOCK:
    case MTR_MEMO_PAGE_S_FIX:
      break;
    default:
      ut_a(false);
    }
#endif
    return m_slots_count-- && functor(slot);
  }

private:
  /** functor to invoke */
  const Functor &functor;
  /** slots count left till savepoint */
  ulint m_slots_count;
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

/** Release page latches held by the mini-transaction. */
struct ReleaseBlocks
{
  const lsn_t start, end;
  ReleaseBlocks(lsn_t start, lsn_t end) : start(start), end(end) {}

  /** @return true always */
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (!slot->object)
      return true;
    switch (slot->type) {
    case MTR_MEMO_PAGE_X_MODIFY:
    case MTR_MEMO_PAGE_SX_MODIFY:
      break;
    default:
      ut_ad(!(slot->type & MTR_MEMO_MODIFY));
      return true;
    }

    buf_flush_note_modification(static_cast<buf_block_t*>(slot->object),
                                start, end);
    return true;
  }
};

/** Start a mini-transaction. */
void mtr_t::start()
{
  ut_ad(!m_freed_pages);
  ut_ad(!m_freed_space);
  MEM_UNDEFINED(this, sizeof *this);
  MEM_MAKE_DEFINED(&m_freed_space, sizeof m_freed_space);
  MEM_MAKE_DEFINED(&m_freed_pages, sizeof m_freed_pages);

  ut_d(m_start= true);
  ut_d(m_commit= false);

  m_last= nullptr;
  m_last_offset= 0;

  new(&m_memo) mtr_buf_t();
  new(&m_log) mtr_buf_t();

  m_made_dirty= false;
  m_inside_ibuf= false;
  m_modifications= false;
  m_log_mode= MTR_LOG_ALL;
  ut_d(m_user_space_id= TRX_SYS_SPACE);
  m_user_space= nullptr;
  m_commit_lsn= 0;
  m_trim_pages= false;
}

/** Release the resources */
inline void mtr_t::release_resources()
{
  ut_ad(is_active());
  ut_d(m_memo.for_each_block_in_reverse(CIterate<DebugCheck>()));
  m_log.erase();
  m_memo.erase();
  ut_d(m_commit= true);
}

/** Commit a mini-transaction. */
void mtr_t::commit()
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());

  /* This is a dirty read, for debugging. */
  ut_ad(!m_modifications || !recv_no_log_write);
  ut_ad(!m_modifications || m_log_mode != MTR_LOG_NONE);

  if (m_modifications && (m_log_mode == MTR_LOG_NO_REDO || !m_log.empty()))
  {
    ut_ad(!srv_read_only_mode || m_log_mode == MTR_LOG_NO_REDO);

    std::pair<lsn_t,page_flush_ahead> lsns;

    if (UNIV_LIKELY(m_log_mode == MTR_LOG_ALL))
    {
      lsns= do_write();

      if (m_made_dirty)
        mysql_mutex_lock(&log_sys.flush_order_mutex);

      /* It is now safe to release log_sys.mutex because the
      buf_pool.flush_order_mutex will ensure that we are the first one
      to insert into buf_pool.flush_list. */
      mysql_mutex_unlock(&log_sys.mutex);
    }
    else
    {
      ut_ad(m_log_mode == MTR_LOG_NO_REDO);
      ut_ad(m_log.size() == 0);
      m_commit_lsn= log_sys.get_lsn();
      lsns= { m_commit_lsn, PAGE_FLUSH_NO };
      if (UNIV_UNLIKELY(m_made_dirty)) /* This should be IMPORT TABLESPACE */
        mysql_mutex_lock(&log_sys.flush_order_mutex);
    }

    if (m_freed_pages)
    {
      ut_ad(!m_freed_pages->empty());
      ut_ad(m_freed_space);
      ut_ad(memo_contains(*m_freed_space));
      ut_ad(is_named_space(m_freed_space));
      /* Update the last freed lsn */
      m_freed_space->update_last_freed_lsn(m_commit_lsn);

      if (!is_trim_pages())
        for (const auto &range : *m_freed_pages)
          m_freed_space->add_free_range(range);
      else
        m_freed_space->clear_freed_ranges();
      delete m_freed_pages;
      m_freed_pages= nullptr;
      m_freed_space= nullptr;
      /* mtr_t::start() will reset m_trim_pages */
    }
    else
      ut_ad(!m_freed_space);

    m_memo.for_each_block_in_reverse
      (CIterate<const ReleaseBlocks>(ReleaseBlocks(lsns.first, m_commit_lsn)));
    if (m_made_dirty)
      mysql_mutex_unlock(&log_sys.flush_order_mutex);

    m_memo.for_each_block_in_reverse(CIterate<ReleaseLatches>());

    if (UNIV_UNLIKELY(lsns.second != PAGE_FLUSH_NO))
      buf_flush_ahead(m_commit_lsn, lsns.second == PAGE_FLUSH_SYNC);

    if (m_made_dirty)
      srv_stats.log_write_requests.inc();
  }
  else
    m_memo.for_each_block_in_reverse(CIterate<ReleaseAll>());

  release_resources();
}

/** Release latches till savepoint. To simplify the code only
MTR_MEMO_S_LOCK and MTR_MEMO_PAGE_S_FIX slot types are allowed to be
released, otherwise it would be neccesary to add one more argument in the
function to point out what slot types are allowed for rollback, and this
would be overengineering as corrently the function is used only in one place
in the code.
@param savepoint   savepoint, can be obtained with get_savepoint */
void mtr_t::rollback_to_savepoint(ulint savepoint)
{
  Iterate<TillSavepoint<ReleaseLatches>> iteration(
      TillSavepoint<ReleaseLatches>(ReleaseLatches(), savepoint,
                                    get_savepoint()));
  m_memo.for_each_block_in_reverse(iteration);
}

/** Shrink a tablespace. */
struct Shrink
{
  /** the first non-existing page in the tablespace */
  const page_id_t high;

  Shrink(const fil_space_t &space) : high({space.id, space.size}) {}

  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (!slot->object)
      return true;
    switch (slot->type) {
    default:
      ut_ad("invalid type" == 0);
      return false;
    case MTR_MEMO_SPACE_X_LOCK:
      ut_ad(high.space() == static_cast<fil_space_t*>(slot->object)->id);
      return true;
    case MTR_MEMO_PAGE_X_MODIFY:
    case MTR_MEMO_PAGE_SX_MODIFY:
    case MTR_MEMO_PAGE_X_FIX:
    case MTR_MEMO_PAGE_SX_FIX:
      auto &bpage= static_cast<buf_block_t*>(slot->object)->page;
      ut_ad(bpage.io_fix() == BUF_IO_NONE);
      const auto id= bpage.id();
      if (id < high)
      {
        ut_ad(id.space() == high.space() ||
              (id == page_id_t{0, TRX_SYS_PAGE_NO} &&
               srv_is_undo_tablespace(high.space())));
        break;
      }
      ut_ad(id.space() == high.space());
      ut_ad(bpage.state() == BUF_BLOCK_FILE_PAGE);
      if (bpage.oldest_modification() > 1)
        bpage.clear_oldest_modification(false);
      slot->type= static_cast<mtr_memo_type_t>(slot->type & ~MTR_MEMO_MODIFY);
    }
    return true;
  }
};

/** Commit a mini-transaction that is shrinking a tablespace.
@param space   tablespace that is being shrunk */
void mtr_t::commit_shrink(fil_space_t &space)
{
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(!high_level_read_only);
  ut_ad(m_modifications);
  ut_ad(m_made_dirty);
  ut_ad(!recv_recovery_is_on());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(UT_LIST_GET_LEN(space.chain) == 1);

  log_write_and_flush_prepare();

  const lsn_t start_lsn= do_write().first;

  mysql_mutex_lock(&log_sys.flush_order_mutex);
  /* Durably write the reduced FSP_SIZE before truncating the data file. */
  log_write_and_flush();

  if (m_freed_pages)
  {
    ut_ad(!m_freed_pages->empty());
    ut_ad(m_freed_space == &space);
    ut_ad(memo_contains(*m_freed_space));
    ut_ad(is_named_space(m_freed_space));
    m_freed_space->update_last_freed_lsn(m_commit_lsn);

    if (!is_trim_pages())
      for (const auto &range : *m_freed_pages)
        m_freed_space->add_free_range(range);
    else
      m_freed_space->clear_freed_ranges();
    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
    /* mtr_t::start() will reset m_trim_pages */
  }
  else
    ut_ad(!m_freed_space);

  m_memo.for_each_block_in_reverse(CIterate<Shrink>{space});

  m_memo.for_each_block_in_reverse(CIterate<const ReleaseBlocks>
                                   (ReleaseBlocks(start_lsn, m_commit_lsn)));
  mysql_mutex_unlock(&log_sys.flush_order_mutex);

  mutex_enter(&fil_system.mutex);
  ut_ad(space.is_being_truncated);
  ut_ad(space.is_stopping());
  space.set_stopping(false);
  space.is_being_truncated= false;
  mutex_exit(&fil_system.mutex);

  /* Truncate the file before releasing the space.latch. File extension
  (and any allocation of pages beyond the current intended end of the file)
  is covered by exclusive space.latch, which we are still holding here. */
  os_file_truncate(space.chain.start->name, space.chain.start->handle,
                   os_offset_t{space.size} << srv_page_size_shift, true);

  m_memo.for_each_block_in_reverse(CIterate<ReleaseLatches>());
  srv_stats.log_write_requests.inc();

  release_resources();
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
FILE_MODIFY records and an optional FILE_CHECKPOINT marker.
The caller must hold log_sys.mutex.
This is to be used at log_checkpoint().
@param[in]	checkpoint_lsn		log checkpoint LSN, or 0 */
void mtr_t::commit_files(lsn_t checkpoint_lsn)
{
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(is_active());
	ut_ad(!is_inside_ibuf());
	ut_ad(m_log_mode == MTR_LOG_ALL);
	ut_ad(!m_made_dirty);
	ut_ad(m_memo.size() == 0);
	ut_ad(!srv_read_only_mode);
	ut_ad(!m_freed_space);
	ut_ad(!m_freed_pages);

	if (checkpoint_lsn) {
		byte*	ptr = m_log.push<byte*>(SIZE_OF_FILE_CHECKPOINT);
		compile_time_assert(SIZE_OF_FILE_CHECKPOINT == 3 + 8 + 1);
		*ptr = FILE_CHECKPOINT | (SIZE_OF_FILE_CHECKPOINT - 2);
		::memset(ptr + 1, 0, 2);
		mach_write_to_8(ptr + 3, checkpoint_lsn);
		ptr[3 + 8] = 0;
	} else {
		*m_log.push<byte*>(1) = 0;
	}

	finish_write(m_log.size());
	srv_stats.log_write_requests.inc();
	release_resources();

	if (checkpoint_lsn) {
		DBUG_PRINT("ib_log",
			   ("FILE_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
			    checkpoint_lsn, log_sys.get_lsn()));
	}
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(ulint space) const
{
	ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

	switch (m_log_mode) {
	case MTR_LOG_NONE:
	case MTR_LOG_NO_REDO:
		return(true);
	case MTR_LOG_ALL:
		return(m_user_space_id == space
		       || is_predefined_tablespace(space));
	}

	ut_error;
	return(false);
}
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool mtr_t::is_named_space(const fil_space_t* space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

  switch (m_log_mode) {
  case MTR_LOG_NONE:
  case MTR_LOG_NO_REDO:
    return true;
  case MTR_LOG_ALL:
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
		ut_ad(m_log_mode != MTR_LOG_NO_REDO
		      || space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT);
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

static bool log_margin_warned;
static time_t log_margin_warn_time;
static time_t log_close_warn_time;

/** Check margin not to overwrite transaction log from the last checkpoint.
If would estimate the log write to exceed the log_capacity,
waits for the checkpoint is done enough.
@param len   length of the data to be written */
static void log_margin_checkpoint_age(ulint len)
{
  const ulint framing_size= log_sys.framing_size();
  /* actual length stored per block */
  const ulint len_per_blk= OS_FILE_LOG_BLOCK_SIZE - framing_size;

  /* actual data length in last block already written */
  ulint extra_len= log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE;

  ut_ad(extra_len >= LOG_BLOCK_HDR_SIZE);
  extra_len-= LOG_BLOCK_HDR_SIZE;

  /* total extra length for block header and trailer */
  extra_len= ((len + extra_len) / len_per_blk) * framing_size;

  const ulint margin= len + extra_len;

  mysql_mutex_assert_owner(&log_sys.mutex);

  const lsn_t lsn= log_sys.get_lsn();

  if (UNIV_UNLIKELY(margin > log_sys.log_capacity))
  {
    time_t t= time(nullptr);

    /* return with warning output to avoid deadlock */
    if (!log_margin_warned || difftime(t, log_margin_warn_time) > 15)
    {
      log_margin_warned= true;
      log_margin_warn_time= t;

      sql_print_error("InnoDB: innodb_log_file_size is too small "
                      "for mini-transaction size " ULINTPF, len);
    }
  }
  else if (UNIV_LIKELY(lsn + margin <= log_sys.last_checkpoint_lsn +
                       log_sys.log_capacity))
    return;

  log_sys.set_check_flush_or_checkpoint();
}


/** Open the log for log_write_low(). The log must be closed with log_close().
@param len length of the data to be written
@return start lsn of the log record */
static lsn_t log_reserve_and_open(size_t len)
{
  for (ut_d(ulint count= 0);;)
  {
    mysql_mutex_assert_owner(&log_sys.mutex);

    /* Calculate an upper limit for the space the string may take in
    the log buffer */

    size_t len_upper_limit= (4 * OS_FILE_LOG_BLOCK_SIZE) +
      srv_log_write_ahead_size + (5 * len) / 4;

    if (log_sys.buf_free + len_upper_limit <= srv_log_buffer_size)
      break;

    mysql_mutex_unlock(&log_sys.mutex);
    DEBUG_SYNC_C("log_buf_size_exceeded");

    /* Not enough free space, do a write of the log buffer */
    log_write_up_to(log_sys.get_lsn(), false);

    srv_stats.log_waits.inc();

    ut_ad(++count < 50);

    mysql_mutex_lock(&log_sys.mutex);
  }

  return log_sys.get_lsn();
}

/** Append data to the log buffer. */
static void log_write_low(const void *str, size_t size)
{
  mysql_mutex_assert_owner(&log_sys.mutex);
  const ulint trailer_offset= log_sys.trailer_offset();

  do
  {
    /* Calculate a part length */
    size_t len= size;
    size_t data_len= (log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE) + size;

    if (data_len > trailer_offset)
    {
      data_len= trailer_offset;
      len= trailer_offset - log_sys.buf_free % OS_FILE_LOG_BLOCK_SIZE;
    }

    memcpy(log_sys.buf + log_sys.buf_free, str, len);

    size-= len;
    str= static_cast<const char*>(str) + len;

    byte *log_block= static_cast<byte*>(ut_align_down(log_sys.buf +
                                                      log_sys.buf_free,
                                                      OS_FILE_LOG_BLOCK_SIZE));

    log_block_set_data_len(log_block, data_len);
    lsn_t lsn= log_sys.get_lsn();

    if (data_len == trailer_offset)
    {
      /* This block became full */
      log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE);
      log_block_set_checkpoint_no(log_block, log_sys.next_checkpoint_no);
      len+= log_sys.framing_size();
      lsn+= len;
      /* Initialize the next block header */
      log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, lsn);
    }
    else
      lsn+= len;

    log_sys.set_lsn(lsn);
    log_sys.buf_free+= len;

    ut_ad(log_sys.buf_free <= size_t{srv_log_buffer_size});
  }
  while (size);
}

/** Close the log at mini-transaction commit.
@return whether buffer pool flushing is needed */
static mtr_t::page_flush_ahead log_close(lsn_t lsn)
{
  mysql_mutex_assert_owner(&log_sys.mutex);
  ut_ad(lsn == log_sys.get_lsn());

  byte *log_block= static_cast<byte*>(ut_align_down(log_sys.buf +
                                                    log_sys.buf_free,
                                                    OS_FILE_LOG_BLOCK_SIZE));

  if (!log_block_get_first_rec_group(log_block))
  {
    /* We initialized a new log block which was not written
    full by the current mtr: the next mtr log record group
    will start within this block at the offset data_len */
    log_block_set_first_rec_group(log_block,
                                  log_block_get_data_len(log_block));
  }

  if (log_sys.buf_free > log_sys.max_buf_free)
    log_sys.set_check_flush_or_checkpoint();

  const lsn_t checkpoint_age= lsn - log_sys.last_checkpoint_lsn;

  if (UNIV_UNLIKELY(checkpoint_age >= log_sys.log_capacity) &&
      /* silence message on create_log_file() after the log had been deleted */
      checkpoint_age != lsn)
  {
    time_t t= time(nullptr);
    if (!log_sys.overwrite_warned || difftime(t, log_close_warn_time) > 15)
    {
      if (!log_sys.overwrite_warned)
        log_sys.overwrite_warned= lsn;
      log_close_warn_time= t;

      sql_print_error("InnoDB: Crash recovery is broken due to"
                      " insufficient innodb_log_file_size;"
                      " last checkpoint LSN=" LSN_PF ", current LSN=" LSN_PF
                      "%s.",
                      lsn_t{log_sys.last_checkpoint_lsn}, lsn,
                      srv_shutdown_state != SRV_SHUTDOWN_INITIATED
                      ? ". Shutdown is in progress" : "");
    }
  }
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_modified_age_async))
    return mtr_t::PAGE_FLUSH_NO;
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_checkpoint_age))
    return mtr_t::PAGE_FLUSH_ASYNC;

  log_sys.set_check_flush_or_checkpoint();
  return mtr_t::PAGE_FLUSH_SYNC;
}

/** Write the block contents to the REDO log */
struct mtr_write_log
{
  /** Append a block to the redo log buffer.
  @return whether the appending should continue */
  bool operator()(const mtr_buf_t::block_t *block) const
  {
    log_write_low(block->begin(), block->used());
    return true;
  }
};

std::pair<lsn_t,mtr_t::page_flush_ahead> mtr_t::do_write()
{
	ut_ad(!recv_no_log_write);
	ut_ad(m_log_mode == MTR_LOG_ALL);

	ulint	len	= m_log.size();
	ut_ad(len > 0);

	if (len > srv_log_buffer_size / 2) {
		log_buffer_extend(ulong((len + 1) * 2));
	}

	fil_space_t*	space = m_user_space;

	if (space != NULL && is_predefined_tablespace(space->id)) {
		/* Omit FILE_MODIFY for predefined tablespaces. */
		space = NULL;
	}

	mysql_mutex_lock(&log_sys.mutex);

	if (fil_names_write_if_was_clean(space)) {
		len = m_log.size();
	} else {
		/* This was not the first time of dirtying a
		tablespace since the latest checkpoint. */
		ut_ad(len == m_log.size());
	}

	*m_log.push<byte*>(1) = 0;
	len++;

	/* check and attempt a checkpoint if exceeding capacity */
	log_margin_checkpoint_age(len);

	return finish_write(len);
}

/** Append the redo log records to the redo log buffer.
@param len   number of bytes to write
@return {start_lsn,flush_ahead} */
inline std::pair<lsn_t,mtr_t::page_flush_ahead> mtr_t::finish_write(ulint len)
{
	ut_ad(m_log_mode == MTR_LOG_ALL);
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(m_log.size() == len);
	ut_ad(len > 0);

	lsn_t start_lsn;

	if (m_log.is_small()) {
		const mtr_buf_t::block_t* front = m_log.front();
		ut_ad(len <= front->used());

		m_commit_lsn = log_reserve_and_write_fast(front->begin(), len,
							  &start_lsn);

		if (!m_commit_lsn) {
			goto piecewise;
		}
	} else {
piecewise:
		/* Open the database log for log_write_low */
		start_lsn = log_reserve_and_open(len);
		mtr_write_log write_log;
		m_log.for_each_block(write_log);
		m_commit_lsn = log_sys.get_lsn();
	}
	page_flush_ahead flush= log_close(m_commit_lsn);
	DBUG_EXECUTE_IF("ib_log_flush_ahead", flush = PAGE_FLUSH_SYNC;);

	return std::make_pair(start_lsn, flush);
}

/** Find out whether a block was not X-latched by the mini-transaction */
struct FindBlockX
{
  const buf_block_t &block;

  FindBlockX(const buf_block_t &block): block(block) {}

  /** @return whether the block was not found x-latched */
  bool operator()(const mtr_memo_slot_t *slot) const
  {
    return slot->object != &block || slot->type != MTR_MEMO_PAGE_X_FIX;
  }
};

#ifdef UNIV_DEBUG
/** Assert that the block is not present in the mini-transaction */
struct FindNoBlock
{
  const buf_block_t &block;

  FindNoBlock(const buf_block_t &block): block(block) {}

  /** @return whether the block was not found */
  bool operator()(const mtr_memo_slot_t *slot) const
  {
    return slot->object != &block;
  }
};
#endif /* UNIV_DEBUG */

bool mtr_t::have_x_latch(const buf_block_t &block) const
{
  if (m_memo.for_each_block(CIterate<FindBlockX>(FindBlockX(block))))
  {
    ut_ad(m_memo.for_each_block(CIterate<FindNoBlock>(FindNoBlock(block))));
    ut_ad(!memo_contains_flagged(&block,
                                 MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_SX_FIX |
                                 MTR_MEMO_BUF_FIX | MTR_MEMO_MODIFY));
    return false;
  }
  ut_ad(rw_lock_own(&block.lock, RW_LOCK_X));
  return true;
}

#ifdef UNIV_DEBUG
/** Check if we are holding an rw-latch in this mini-transaction
@param lock   latch to search for
@param type   held latch type
@return whether (lock,type) is contained */
bool mtr_t::memo_contains(const rw_lock_t &lock, mtr_memo_type_t type)
{
  Iterate<Find> iteration(Find(&lock, type));
  if (m_memo.for_each_block_in_reverse(iteration))
    return false;

  switch (type) {
  case MTR_MEMO_X_LOCK:
    ut_ad(rw_lock_own(&lock, RW_LOCK_X));
    break;
  case MTR_MEMO_SX_LOCK:
    ut_ad(rw_lock_own(&lock, RW_LOCK_SX));
    break;
  case MTR_MEMO_S_LOCK:
    ut_ad(rw_lock_own(&lock, RW_LOCK_S));
    break;
  default:
    break;
  }

  return true;
}

/** Check if we are holding exclusive tablespace latch
@param space  tablespace to search for
@return whether space.latch is being held */
bool mtr_t::memo_contains(const fil_space_t& space)
{
  Iterate<Find> iteration(Find(&space, MTR_MEMO_SPACE_X_LOCK));
  if (m_memo.for_each_block_in_reverse(iteration))
    return false;
  ut_ad(rw_lock_own(const_cast<rw_lock_t*>(&space.latch), RW_LOCK_X));
  return true;
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
#endif /* UNIV_DEBUG */


/** Find a block, preferrably in MTR_MEMO_MODIFY state */
struct FindModified
{
  mtr_memo_slot_t *found= nullptr;
  const buf_block_t& block;

  FindModified(const buf_block_t &block) : block(block) {}
  bool operator()(mtr_memo_slot_t *slot)
  {
    if (slot->object != &block)
      return true;
    found= slot;
    return !(slot->type & (MTR_MEMO_MODIFY |
                           MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX));
  }
};

/** Mark the given latched page as modified.
@param block   page that will be modified */
void mtr_t::modify(const buf_block_t &block)
{
  if (UNIV_UNLIKELY(m_memo.empty()))
  {
    /* This must be PageConverter::update_page() in IMPORT TABLESPACE. */
    ut_ad(!block.page.in_LRU_list);
    return;
  }

  Iterate<FindModified> iteration((FindModified(block)));
  if (UNIV_UNLIKELY(m_memo.for_each_block(iteration)))
  {
    ut_ad("modifying an unlatched page" == 0);
    return;
  }
  iteration.functor.found->type= static_cast<mtr_memo_type_t>
    (iteration.functor.found->type | MTR_MEMO_MODIFY);
  if (is_block_dirtied(&block))
    m_made_dirty= true;
}
