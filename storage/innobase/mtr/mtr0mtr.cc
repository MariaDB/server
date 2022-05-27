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
#include "page0types.h"
#include "log0crypt.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif
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

		buf_page_t* bpage = static_cast<buf_page_t*>(slot->object);

		if (m_ptr < bpage->frame
		    || m_ptr >= bpage->frame + srv_page_size) {
			return(true);
		}
		ut_ad(!(slot->type & MTR_MEMO_PAGE_S_FIX)
		      || bpage->lock.have_s());
		ut_ad(!(slot->type & MTR_MEMO_PAGE_SX_FIX)
		      || bpage->lock.have_u_or_x());
		ut_ad(!(slot->type & MTR_MEMO_PAGE_X_FIX)
		      || bpage->lock.have_x());
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
  void *object= slot->object;
  slot->object= nullptr;
  switch (const auto type= slot->type) {
  case MTR_MEMO_S_LOCK:
    static_cast<index_lock*>(object)->s_unlock();
    break;
  case MTR_MEMO_X_LOCK:
  case MTR_MEMO_SX_LOCK:
    static_cast<index_lock*>(object)->
      u_or_x_unlock(type == MTR_MEMO_SX_LOCK);
    break;
  case MTR_MEMO_SPACE_X_LOCK:
    static_cast<fil_space_t*>(object)->set_committed_size();
    static_cast<fil_space_t*>(object)->x_unlock();
    break;
  case MTR_MEMO_SPACE_S_LOCK:
    static_cast<fil_space_t*>(object)->s_unlock();
    break;
  default:
    buf_page_t *bpage= static_cast<buf_page_t*>(object);
    bpage->unfix();
    switch (auto latch= slot->type & ~MTR_MEMO_MODIFY) {
    case MTR_MEMO_PAGE_S_FIX:
      bpage->lock.s_unlock();
      return;
    case MTR_MEMO_PAGE_SX_FIX:
    case MTR_MEMO_PAGE_X_FIX:
      bpage->lock.u_or_x_unlock(latch == MTR_MEMO_PAGE_SX_FIX);
      /* fall through */
    case MTR_MEMO_BUF_FIX:
      return;
    }
    ut_ad("invalid type" == 0);
  }
}

/** Release the latches acquired by the mini-transaction. */
struct ReleaseLatches {
  /** @return true always. */
  bool operator()(mtr_memo_slot_t *slot) const
  {
    void *object= slot->object;
    if (!object)
      return true;
    slot->object= nullptr;
    switch (const auto type= slot->type) {
    case MTR_MEMO_S_LOCK:
      static_cast<index_lock*>(object)->s_unlock();
      break;
    case MTR_MEMO_SPACE_X_LOCK:
      static_cast<fil_space_t*>(object)->set_committed_size();
      static_cast<fil_space_t*>(object)->x_unlock();
      break;
    case MTR_MEMO_SPACE_S_LOCK:
      static_cast<fil_space_t*>(object)->s_unlock();
      break;
    case MTR_MEMO_X_LOCK:
    case MTR_MEMO_SX_LOCK:
      static_cast<index_lock*>(object)->
        u_or_x_unlock(type == MTR_MEMO_SX_LOCK);
      break;
    default:
      buf_page_t *bpage= static_cast<buf_page_t*>(object);
      bpage->unfix();
      switch (auto latch= slot->type & ~MTR_MEMO_MODIFY) {
      case MTR_MEMO_PAGE_S_FIX:
        bpage->lock.s_unlock();
        return true;
      case MTR_MEMO_PAGE_SX_FIX:
      case MTR_MEMO_PAGE_X_FIX:
        bpage->lock.u_or_x_unlock(latch == MTR_MEMO_PAGE_SX_FIX);
        /* fall through */
      case MTR_MEMO_BUF_FIX:
        return true;
      }
      ut_ad("invalid type" == 0);
    }
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
  mutable size_t modified;
  ReleaseBlocks(lsn_t start, lsn_t end) : start(start), end(end), modified(0)
  {
    ut_ad(!srv_read_only_mode);
    ut_ad(start);
    ut_ad(end);
  }

  /** @return true always */
  bool operator()(mtr_memo_slot_t* slot) const
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

    modified++;
    buf_block_t *b= static_cast<buf_block_t*>(slot->object);
    ut_d(const auto s= b->page.state());
    ut_ad(s > buf_page_t::FREED);
    ut_ad(s < buf_page_t::READ_FIX);
    ut_ad(mach_read_from_8(b->page.frame + FIL_PAGE_LSN) <= end);
    mach_write_to_8(b->page.frame + FIL_PAGE_LSN, end);
    if (UNIV_LIKELY_NULL(b->page.zip.data))
      memcpy_aligned<8>(FIL_PAGE_LSN + b->page.zip.data,
                        FIL_PAGE_LSN + b->page.frame, 8);

    const lsn_t oldest_modification= b->page.oldest_modification();

    if (oldest_modification > 1)
      ut_ad(oldest_modification <= start);
    else if (fsp_is_system_temporary(b->page.id().space()))
      b->page.set_temp_modified();
    else
      buf_pool.insert_into_flush_list(b, start);
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
  ut_d(m_freeing_tree= false);

  m_last= nullptr;
  m_last_offset= 0;

  new(&m_memo) mtr_buf_t();
  new(&m_log) mtr_buf_t();

  m_made_dirty= false;
  m_latch_ex= false;
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
  ut_ad(!m_latch_ex);

  if (m_modifications && (m_log_mode == MTR_LOG_NO_REDO || !m_log.empty()))
  {
    ut_ad(!srv_read_only_mode || m_log_mode == MTR_LOG_NO_REDO);

    std::pair<lsn_t,page_flush_ahead> lsns;

    if (UNIV_LIKELY(m_log_mode == MTR_LOG_ALL))
    {
      lsns= do_write();
      if (m_made_dirty);
      else if (m_latch_ex)
      {
        log_sys.latch.wr_unlock();
        m_latch_ex= false;
      }
      else
        log_sys.latch.rd_unlock();
    }
    else
    {
      ut_ad(m_log_mode == MTR_LOG_NO_REDO);
      ut_ad(m_log.size() == 0);
      m_commit_lsn= log_sys.get_lsn();
      lsns= { m_commit_lsn, PAGE_FLUSH_NO };
      if (UNIV_UNLIKELY(m_made_dirty)) /* This should be IMPORT TABLESPACE */
        log_sys.latch.rd_lock(SRW_LOCK_CALL);
    }

    if (m_freed_pages)
    {
      ut_ad(!m_freed_pages->empty());
      ut_ad(m_freed_space);
      ut_ad(m_freed_space->is_owner());
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

    Iterate<ReleaseBlocks> rb{ReleaseBlocks{lsns.first, m_commit_lsn}};
    m_memo.for_each_block_in_reverse(rb);
    if (!m_made_dirty);
    else if (m_latch_ex)
    {
      log_sys.latch.wr_unlock();
      m_latch_ex= false;
    }
    else
      log_sys.latch.rd_unlock();

    m_memo.for_each_block_in_reverse(CIterate<ReleaseLatches>());

    if (UNIV_UNLIKELY(lsns.second != PAGE_FLUSH_NO))
      buf_flush_ahead(m_commit_lsn, lsns.second == PAGE_FLUSH_SYNC);

    if (rb.functor.modified)
    {
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      buf_pool.flush_list_requests+= rb.functor.modified;
      buf_pool.page_cleaner_wakeup();
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    }
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
      const auto s= bpage.state();
      ut_ad(s >= buf_page_t::FREED);
      ut_ad(s < buf_page_t::READ_FIX);
      ut_ad(bpage.frame);
      const page_id_t id{bpage.id()};
      if (id < high)
      {
        ut_ad(id.space() == high.space() ||
              (id == page_id_t{0, TRX_SYS_PAGE_NO} &&
               srv_is_undo_tablespace(high.space())));
        break;
      }
      if (s >= buf_page_t::UNFIXED)
        bpage.set_freed(s);
      ut_ad(id.space() == high.space());
      if (bpage.oldest_modification() > 1)
        bpage.reset_oldest_modification();
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
  m_latch_ex= true;
  log_sys.latch.wr_lock(SRW_LOCK_CALL);

  const lsn_t start_lsn= do_write().first;

  /* Durably write the reduced FSP_SIZE before truncating the data file. */
  log_write_and_flush();
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif

  os_file_truncate(space.chain.start->name, space.chain.start->handle,
                   os_offset_t{space.size} << srv_page_size_shift, true);

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
                                   (ReleaseBlocks{start_lsn, m_commit_lsn}));
  log_sys.latch.wr_unlock();
  m_latch_ex= false;

  mysql_mutex_lock(&fil_system.mutex);
  ut_ad(space.is_being_truncated);
  ut_ad(space.is_stopping());
  space.clear_stopping();
  space.is_being_truncated= false;
  mysql_mutex_unlock(&fil_system.mutex);

  m_memo.for_each_block_in_reverse(CIterate<ReleaseLatches>());

  release_resources();
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
FILE_MODIFY records and an optional FILE_CHECKPOINT marker.
The caller must hold log_sys.mutex.
This is to be used at log_checkpoint().
@param checkpoint_lsn   the log sequence number of a checkpoint, or 0
@return current LSN */
lsn_t mtr_t::commit_files(lsn_t checkpoint_lsn)
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif
  ut_ad(is_active());
  ut_ad(!is_inside_ibuf());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(!m_made_dirty);
  ut_ad(m_memo.size() == 0);
  ut_ad(!srv_read_only_mode);
  ut_ad(!m_freed_space);
  ut_ad(!m_freed_pages);
  ut_ad(!m_user_space);
  ut_ad(!m_latch_ex);

  m_latch_ex= true;

  if (checkpoint_lsn)
  {
    byte *ptr= m_log.push<byte*>(3 + 8);
    *ptr= FILE_CHECKPOINT | (2 + 8);
    ::memset(ptr + 1, 0, 2);
    mach_write_to_8(ptr + 3, checkpoint_lsn);
  }

  size_t size= m_log.size() + 5;

  if (log_sys.is_encrypted())
  {
    /* We will not encrypt any FILE_ records, but we will reserve
    a nonce at the end. */
    size+= 8;
    m_commit_lsn= log_sys.get_lsn();
  }
  else
    m_commit_lsn= 0;

  m_crc= 0;
  m_log.for_each_block([this](const mtr_buf_t::block_t *b)
  { m_crc= my_crc32c(m_crc, b->begin(), b->used()); return true; });
  finish_write(size);
  release_resources();

  if (checkpoint_lsn)
    DBUG_PRINT("ib_log",
               ("FILE_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
                checkpoint_lsn, m_commit_lsn));

  return m_commit_lsn;
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(uint32_t space) const
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
@param[in]	space_id	tablespace ID
@return the tablespace object (never NULL) */
fil_space_t *mtr_t::x_lock_space(uint32_t space_id)
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
	x_lock_space(space);
	return(space);
}

/** Acquire an exclusive tablespace latch.
@param space  tablespace */
void mtr_t::x_lock_space(fil_space_t *space)
{
  ut_ad(space->purpose == FIL_TYPE_TEMPORARY ||
        space->purpose == FIL_TYPE_IMPORT ||
        space->purpose == FIL_TYPE_TABLESPACE);
  if (!memo_contains(*space))
  {
    memo_push(space, MTR_MEMO_SPACE_X_LOCK);
    space->x_lock();
  }
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

static bool log_close_warned;
static time_t log_close_warn_time;

/** Display a warning that the log tail is overwriting the head,
making the server crash-unsafe. */
ATTRIBUTE_COLD static void log_overwrite_warning(lsn_t age, lsn_t capacity)
{
  time_t t= time(nullptr);
  if (!log_close_warned || difftime(t, log_close_warn_time) > 15)
  {
    log_close_warned= true;
    log_close_warn_time= t;

    sql_print_error("InnoDB: The age of the last checkpoint is " LSN_PF
                    ", which exceeds the log capacity " LSN_PF ".",
                    age, capacity);
  }
}

/** Wait in append_prepare() for buffer to become available
@param ex   whether log_sys.latch is exclusively locked */
ATTRIBUTE_COLD void log_t::append_prepare_wait(bool ex) noexcept
{
  log_sys.waits++;
  log_sys.unlock_lsn();

  if (ex)
    log_sys.latch.wr_unlock();
  else
    log_sys.latch.rd_unlock();

  DEBUG_SYNC_C("log_buf_size_exceeded");
  log_buffer_flush_to_disk(log_sys.is_pmem());

  if (ex)
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
  else
    log_sys.latch.rd_lock(SRW_LOCK_CALL);

  log_sys.lock_lsn();
}

/** Reserve space in the log buffer for appending data.
@tparam pmem  log_sys.is_pmem()
@param size   total length of the data to append(), in bytes
@param ex     whether log_sys.latch is exclusively locked
@return the start LSN and the buffer position for append() */
template<bool pmem>
inline
std::pair<lsn_t,byte*> log_t::append_prepare(size_t size, bool ex) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_locked());
# ifndef _WIN32 // there is no accurate is_write_locked() on SRWLOCK
  ut_ad(ex == latch.is_write_locked());
# endif
#endif
  ut_ad(pmem == is_pmem());
  const lsn_t checkpoint_margin{last_checkpoint_lsn + log_capacity - size};
  const size_t avail{(pmem ? size_t(capacity()) : buf_size) - size};
  lock_lsn();
  write_to_buf++;

  for (ut_d(int count= 50);
       UNIV_UNLIKELY((pmem
                      ? size_t(get_lsn() -
                               get_flushed_lsn(std::memory_order_relaxed))
                      : size_t{buf_free}) > avail); )
  {
    append_prepare_wait(ex);
    ut_ad(count--);
  }

  const lsn_t l{lsn.load(std::memory_order_relaxed)};
  lsn.store(l + size, std::memory_order_relaxed);
  const size_t b{buf_free};
  size_t new_buf_free{b};
  new_buf_free+= size;
  if (pmem && new_buf_free >= file_size)
    new_buf_free-= size_t(capacity());
  buf_free= new_buf_free;
  unlock_lsn();

  if (UNIV_UNLIKELY(l > checkpoint_margin) ||
      (!pmem && b >= max_buf_free))
    set_check_flush_or_checkpoint();

  return {l, &buf[b]};
}

/** Finish appending data to the log.
@param lsn  the end LSN of the log record
@return whether buf_flush_ahead() will have to be invoked */
static mtr_t::page_flush_ahead log_close(lsn_t lsn) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_locked());
#endif

  const lsn_t checkpoint_age= lsn - log_sys.last_checkpoint_lsn;

  if (UNIV_UNLIKELY(checkpoint_age >= log_sys.log_capacity) &&
      /* silence message on create_log_file() after the log had been deleted */
      checkpoint_age != lsn)
    log_overwrite_warning(checkpoint_age, log_sys.log_capacity);
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_modified_age_async))
    return mtr_t::PAGE_FLUSH_NO;
  else if (UNIV_LIKELY(checkpoint_age <= log_sys.max_checkpoint_age))
    return mtr_t::PAGE_FLUSH_ASYNC;

  log_sys.set_check_flush_or_checkpoint();
  return mtr_t::PAGE_FLUSH_SYNC;
}

std::pair<lsn_t,mtr_t::page_flush_ahead> mtr_t::do_write()
{
  ut_ad(!recv_no_log_write);
  ut_ad(m_log_mode == MTR_LOG_ALL);
#ifndef SUX_LOCK_GENERIC
  ut_ad(!m_latch_ex || log_sys.latch.is_write_locked());
#endif

  size_t len= m_log.size() + 5;
  ut_ad(len > 5);
  if (log_sys.is_encrypted())
  {
    len+= 8;
    encrypt();
  }
  else
  {
    m_crc= 0;
    m_commit_lsn= 0;
    m_log.for_each_block([this](const mtr_buf_t::block_t *b)
    { m_crc= my_crc32c(m_crc, b->begin(), b->used()); return true; });
  }

  if (!m_latch_ex)
    log_sys.latch.rd_lock(SRW_LOCK_CALL);

  if (UNIV_UNLIKELY(m_user_space && !m_user_space->max_lsn &&
                    !is_predefined_tablespace(m_user_space->id)))
  {
    if (!m_latch_ex)
    {
      m_latch_ex= true;
      log_sys.latch.rd_unlock();
      log_sys.latch.wr_lock(SRW_LOCK_CALL);
      if (UNIV_UNLIKELY(m_user_space->max_lsn != 0))
        goto func_exit;
    }
    name_write();
  }
func_exit:
  return finish_write(len);
}

inline void log_t::resize_write(lsn_t lsn, const byte *end, size_t len,
                                size_t seq) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_locked());
#endif

  if (UNIV_LIKELY_NULL(resize_buf))
  {
    ut_ad(end >= buf);
    end-= len;
    size_t s;

#ifdef HAVE_PMEM
    if (!resize_flush_buf)
    {
      ut_ad(is_pmem());
      const size_t resize_capacity{resize_target - START_OFFSET};
      const lsn_t resizing{resize_in_progress()};
      if (UNIV_UNLIKELY(lsn < resizing))
      {
        size_t l= resizing - lsn;
        if (l >= len)
          return;
        end+= l - len;
        len-= l;
        lsn+= l;
      }
      lsn-= resizing;
      s= START_OFFSET + lsn % resize_capacity;

      if (UNIV_UNLIKELY(end < &buf[START_OFFSET]))
      {
        /* The source buffer (log_sys.buf) wrapped around */
        ut_ad(end + capacity() < &buf[file_size]);
        ut_ad(end + len >= &buf[START_OFFSET]);
        ut_ad(end + capacity() + len >= &buf[file_size]);

        size_t l= size_t(buf - (end - START_OFFSET));
        if (UNIV_LIKELY(s + len <= resize_target))
        {
          /* The destination buffer (log_sys.resize_buf) did not wrap around */
          memcpy(resize_buf + s, end + capacity(), l);
          memcpy(resize_buf + s + l, &buf[START_OFFSET], len - l);
          goto pmem_nowrap;
        }
        else
        {
          /* Both log_sys.buf and log_sys.resize_buf wrapped around */
          const size_t rl= resize_target - s;
          if (l <= rl)
          {
            /* log_sys.buf wraps around first */
            memcpy(resize_buf + s, end + capacity(), l);
            memcpy(resize_buf + s + l, &buf[START_OFFSET], rl - l);
            memcpy(resize_buf + START_OFFSET, &buf[START_OFFSET + rl - l],
                   len - l);
          }
          else
          {
            /* log_sys.resize_buf wraps around first */
            memcpy(resize_buf + s, end + capacity(), rl);
            memcpy(resize_buf + START_OFFSET, end + capacity() + rl, l - rl);
            memcpy(resize_buf + START_OFFSET + (l - rl),
                   &buf[START_OFFSET], len - l);
          }
          goto pmem_wrap;
        }
      }
      else
      {
        ut_ad(end + len <= &buf[file_size]);

        if (UNIV_LIKELY(s + len <= resize_target))
        {
          memcpy(resize_buf + s, end, len);
        pmem_nowrap:
          s+= len - seq;
        }
        else
        {
          /* The log_sys.resize_buf wrapped around */
          memcpy(resize_buf + s, end, resize_target - s);
          memcpy(resize_buf + START_OFFSET, end + (resize_target - s),
                 len - (resize_target - s));
        pmem_wrap:
          s+= len - seq;
          if (s >= resize_target)
            s-= resize_capacity;
          resize_lsn.fetch_add(resize_capacity); /* Move the target ahead. */
        }
      }
    }
    else
#endif
    {
      ut_ad(resize_flush_buf);
      s= end - buf;
      ut_ad(s + len <= buf_size);
      memcpy(resize_buf + s, end, len);
      s+= len - seq;
    }

    /* Always set the sequence bit. If the resized log were to wrap around,
    we will advance resize_lsn. */
    ut_ad(resize_buf[s] <= 1);
    resize_buf[s]= 1;
  }
}

/** Write the mini-transaction log to the redo log buffer.
@param len   number of bytes to write
@return {start_lsn,flush_ahead} */
std::pair<lsn_t,mtr_t::page_flush_ahead>
mtr_t::finish_write(size_t len)
{
  ut_ad(!recv_no_log_write);
  ut_ad(m_log_mode == MTR_LOG_ALL);
#ifndef SUX_LOCK_GENERIC
# ifndef _WIN32 // there is no accurate is_write_locked() on SRWLOCK
  ut_ad(m_latch_ex == log_sys.latch.is_write_locked());
# endif
#endif

  const size_t size{m_commit_lsn ? 5U + 8U : 5U};
  std::pair<lsn_t, byte*> start;

  if (!log_sys.is_pmem())
  {
    start= log_sys.append_prepare<false>(len, m_latch_ex);
    m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
    { log_sys.append(start.second, b->begin(), b->used()); return true; });

#ifdef HAVE_PMEM
  write_trailer:
#endif
    *start.second++= log_sys.get_sequence_bit(start.first + len - size);
    if (m_commit_lsn)
    {
      mach_write_to_8(start.second, m_commit_lsn);
      m_crc= my_crc32c(m_crc, start.second, 8);
      start.second+= 8;
    }
    mach_write_to_4(start.second, m_crc);
    start.second+= 4;
  }
#ifdef HAVE_PMEM
  else
  {
    start= log_sys.append_prepare<true>(len, m_latch_ex);
    if (UNIV_LIKELY(start.second + len <= &log_sys.buf[log_sys.file_size]))
    {
      m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
      { log_sys.append(start.second, b->begin(), b->used()); return true; });
      goto write_trailer;
    }
    m_log.for_each_block([&start](const mtr_buf_t::block_t *b)
    {
      size_t size{b->used()};
      const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
      const byte *src= b->begin();
      if (size > size_left)
      {
        ::memcpy(start.second, src, size_left);
        start.second= &log_sys.buf[log_sys.START_OFFSET];
        src+= size_left;
        size-= size_left;
      }
      ::memcpy(start.second, src, size);
      start.second+= size;
      return true;
    });
    const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
    if (size_left > size)
      goto write_trailer;

    byte tail[5 + 8];
    tail[0]= log_sys.get_sequence_bit(start.first + len - size);

    if (m_commit_lsn)
    {
      mach_write_to_8(tail + 1, m_commit_lsn);
      m_crc= my_crc32c(m_crc, tail + 1, 8);
      mach_write_to_4(tail + 9, m_crc);
    }
    else
      mach_write_to_4(tail + 1, m_crc);

    ::memcpy(start.second, tail, size_left);
    ::memcpy(log_sys.buf + log_sys.START_OFFSET, tail + size_left,
             size - size_left);
    start.second= log_sys.buf +
      ((size >= size_left) ? log_sys.START_OFFSET : log_sys.file_size) +
      (size - size_left);
  }
#endif

  log_sys.resize_write(start.first, start.second, len, size);

  m_commit_lsn= start.first + len;
  return {start.first, log_close(m_commit_lsn)};
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
  ut_ad(block.page.lock.have_x());
  return true;
}

/** Check if we are holding exclusive tablespace latch
@param space  tablespace to search for
@param shared whether to look for shared latch, instead of exclusive
@return whether space.latch is being held */
bool mtr_t::memo_contains(const fil_space_t& space, bool shared)
{
  Iterate<Find> iteration(Find(&space, shared
                               ? MTR_MEMO_SPACE_S_LOCK
                               : MTR_MEMO_SPACE_X_LOCK));
  if (m_memo.for_each_block_in_reverse(iteration))
    return false;
  ut_ad(shared || space.is_owner());
  return true;
}

#ifdef BTR_CUR_HASH_ADAPT
/** If a stale adaptive hash index exists on the block, drop it.
Multiple executions of btr_search_drop_page_hash_index() on the
same block must be prevented by exclusive page latch. */
ATTRIBUTE_COLD
static void mtr_defer_drop_ahi(buf_block_t *block, mtr_memo_type_t fix_type)
{
  switch (fix_type) {
  case MTR_MEMO_BUF_FIX:
    /* We do not drop the adaptive hash index, because safely doing
    so would require acquiring block->lock, and that is not safe
    to acquire in some RW_NO_LATCH access paths. Those code paths
    should have no business accessing the adaptive hash index anyway. */
    break;
  case MTR_MEMO_PAGE_S_FIX:
    /* Temporarily release our S-latch. */
    block->page.lock.s_unlock();
    block->page.lock.x_lock();
    if (dict_index_t *index= block->index)
      if (index->freed())
        btr_search_drop_page_hash_index(block);
    block->page.lock.x_unlock();
    block->page.lock.s_lock();
    ut_ad(!block->page.is_read_fixed());
    break;
  case MTR_MEMO_PAGE_SX_FIX:
    block->page.lock.u_x_upgrade();
    if (dict_index_t *index= block->index)
      if (index->freed())
        btr_search_drop_page_hash_index(block);
    block->page.lock.x_u_downgrade();
    break;
  default:
    ut_ad(fix_type == MTR_MEMO_PAGE_X_FIX);
    btr_search_drop_page_hash_index(block);
  }
}
#endif /* BTR_CUR_HASH_ADAPT */

/** Upgrade U-latched pages to X */
struct UpgradeX
{
  const buf_block_t &block;
  UpgradeX(const buf_block_t &block) : block(block) {}
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (slot->object == &block && (MTR_MEMO_PAGE_SX_FIX & slot->type))
      slot->type= static_cast<mtr_memo_type_t>
        (slot->type ^ (MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX));
    return true;
  }
};

/** Upgrade U locks on a block to X */
void mtr_t::page_lock_upgrade(const buf_block_t &block)
{
  ut_ad(block.page.lock.have_x());
  m_memo.for_each_block(CIterate<UpgradeX>((UpgradeX(block))));
#ifdef BTR_CUR_HASH_ADAPT
  ut_ad(!block.index || !block.index->freed());
#endif /* BTR_CUR_HASH_ADAPT */
}

/** Upgrade U locks to X */
struct UpgradeLockX
{
  const index_lock &lock;
  UpgradeLockX(const index_lock &lock) : lock(lock) {}
  bool operator()(mtr_memo_slot_t *slot) const
  {
    if (slot->object == &lock && (MTR_MEMO_SX_LOCK & slot->type))
      slot->type= static_cast<mtr_memo_type_t>
        (slot->type ^ (MTR_MEMO_SX_LOCK | MTR_MEMO_X_LOCK));
    return true;
  }
};

/** Upgrade U locks on a block to X */
void mtr_t::lock_upgrade(const index_lock &lock)
{
  ut_ad(lock.have_x());
  m_memo.for_each_block(CIterate<UpgradeLockX>((UpgradeLockX(lock))));
}

/** Latch a buffer pool block.
@param block    block to be latched
@param rw_latch RW_S_LATCH, RW_SX_LATCH, RW_X_LATCH, RW_NO_LATCH */
void mtr_t::page_lock(buf_block_t *block, ulint rw_latch)
{
  mtr_memo_type_t fix_type;
  ut_d(const auto state= block->page.state());
  ut_ad(state > buf_page_t::FREED);
  ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);
  switch (rw_latch)
  {
  case RW_NO_LATCH:
    fix_type= MTR_MEMO_BUF_FIX;
    goto done;
  case RW_S_LATCH:
    fix_type= MTR_MEMO_PAGE_S_FIX;
    block->page.lock.s_lock();
    break;
  case RW_SX_LATCH:
    fix_type= MTR_MEMO_PAGE_SX_FIX;
    block->page.lock.u_lock();
    ut_ad(!block->page.is_io_fixed());
    break;
  default:
    ut_ad(rw_latch == RW_X_LATCH);
    fix_type= MTR_MEMO_PAGE_X_FIX;
    if (block->page.lock.x_lock_upgraded())
    {
      block->unfix();
      page_lock_upgrade(*block);
      return;
    }
    ut_ad(!block->page.is_io_fixed());
  }

#ifdef BTR_CUR_HASH_ADAPT
  if (dict_index_t *index= block->index)
    if (index->freed())
      mtr_defer_drop_ahi(block, fix_type);
#endif /* BTR_CUR_HASH_ADAPT */

done:
  ut_ad(state < buf_page_t::UNFIXED ||
        page_id_t(page_get_space_id(block->page.frame),
                  page_get_page_no(block->page.frame)) == block->page.id());
  memo_push(block, fix_type);
}

#ifdef UNIV_DEBUG
/** Check if we are holding an rw-latch in this mini-transaction
@param lock   latch to search for
@param type   held latch type
@return whether (lock,type) is contained */
bool mtr_t::memo_contains(const index_lock &lock, mtr_memo_type_t type)
{
  Iterate<Find> iteration(Find(&lock, type));
  if (m_memo.for_each_block_in_reverse(iteration))
    return false;

  switch (type) {
  case MTR_MEMO_X_LOCK:
    ut_ad(lock.have_x());
    break;
  case MTR_MEMO_SX_LOCK:
    ut_ad(lock.have_u_or_x());
    break;
  case MTR_MEMO_S_LOCK:
    ut_ad(lock.have_s());
    break;
  default:
    break;
  }

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
		if (m_ptr != slot->object) {
			return(true);
		}

		auto f = m_flags & slot->type;
		if (!f) {
			return true;
		}

		if (f & (MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_SX_FIX
			 | MTR_MEMO_PAGE_X_FIX)) {
			block_lock* lock = &static_cast<buf_block_t*>(
				const_cast<void*>(m_ptr))->page.lock;
			ut_ad(!(f & MTR_MEMO_PAGE_S_FIX) || lock->have_s());
			ut_ad(!(f & MTR_MEMO_PAGE_SX_FIX)
			      || lock->have_u_or_x());
			ut_ad(!(f & MTR_MEMO_PAGE_X_FIX) || lock->have_x());
		} else {
			index_lock* lock = static_cast<index_lock*>(
				const_cast<void*>(m_ptr));
			ut_ad(!(f & MTR_MEMO_S_LOCK) || lock->have_s());
			ut_ad(!(f & MTR_MEMO_SX_LOCK) || lock->have_u_or_x());
			ut_ad(!(f & MTR_MEMO_X_LOCK) || lock->have_x());
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

/** Print info of an mtr handle. */
void
mtr_t::print() const
{
	ib::info() << "Mini-transaction handle: memo size "
		<< m_memo.size() << " bytes log size "
		<< get_log()->size() << " bytes";
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
  m_memo.for_each_block(iteration);
  if (UNIV_UNLIKELY(!iteration.functor.found))
  {
    ut_ad("modifying an unlatched page" == 0);
    return;
  }
  iteration.functor.found->type= static_cast<mtr_memo_type_t>
    (iteration.functor.found->type | MTR_MEMO_MODIFY);
}
