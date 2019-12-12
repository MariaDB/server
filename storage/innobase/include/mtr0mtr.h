/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2019, MariaDB Corporation.

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

#ifndef mtr0mtr_h
#define mtr0mtr_h

#include "fil0fil.h"
#include "dyn0buf.h"

/** Start a mini-transaction. */
#define mtr_start(m)		(m)->start()

/** Commit a mini-transaction. */
#define mtr_commit(m)		(m)->commit()

/** Set and return a savepoint in mtr.
@return	savepoint */
#define mtr_set_savepoint(m)	(m)->get_savepoint()

/** Release the (index tree) s-latch stored in an mtr memo after a
savepoint. */
#define mtr_release_s_latch_at_savepoint(m, s, l)			\
				(m)->release_s_latch_at_savepoint((s), (l))

/** Get the logging mode of a mini-transaction.
@return	logging mode: MTR_LOG_NONE, ... */
#define mtr_get_log_mode(m)	(m)->get_log_mode()

/** Change the logging mode of a mini-transaction.
@return	old mode */
#define mtr_set_log_mode(m, d)	(m)->set_log_mode((d))

/** Release an object in the memo stack.
@return true if released */
#define mtr_memo_release(m, o, t)					\
				(m)->memo_release((o), (t))

#ifdef UNIV_DEBUG
/** Check if memo contains the given item.
@return	TRUE if contains */
#define mtr_memo_contains(m, o, t)					\
				(m)->memo_contains((m)->get_memo(), (o), (t))

/** Check if memo contains the given page.
@return	TRUE if contains */
#define mtr_memo_contains_page(m, p, t)					\
	(m)->memo_contains_page_flagged((p), (t))
#endif /* UNIV_DEBUG */

/** Print info of an mtr handle. */
#define mtr_print(m)		(m)->print()

/** Return the log object of a mini-transaction buffer.
@return	log */
#define mtr_get_log(m)		(m)->get_log()

/** Push an object to an mtr memo stack. */
#define mtr_memo_push(m, o, t)	(m)->memo_push(o, t)

#define mtr_s_lock_space(s, m)	(m)->s_lock_space((s), __FILE__, __LINE__)
#define mtr_x_lock_space(s, m)	(m)->x_lock_space((s), __FILE__, __LINE__)

#define mtr_s_lock_index(i, m)	(m)->s_lock(&(i)->lock, __FILE__, __LINE__)
#define mtr_x_lock_index(i, m)	(m)->x_lock(&(i)->lock, __FILE__, __LINE__)
#define mtr_sx_lock_index(i, m)	(m)->sx_lock(&(i)->lock, __FILE__, __LINE__)

#define mtr_memo_contains_flagged(m, p, l)				\
				(m)->memo_contains_flagged((p), (l))

#define mtr_memo_contains_page_flagged(m, p, l)				\
				(m)->memo_contains_page_flagged((p), (l))

#define mtr_release_block_at_savepoint(m, s, b)				\
				(m)->release_block_at_savepoint((s), (b))

#define mtr_block_sx_latch_at_savepoint(m, s, b)			\
				(m)->sx_latch_at_savepoint((s), (b))

#define mtr_block_x_latch_at_savepoint(m, s, b)				\
				(m)->x_latch_at_savepoint((s), (b))

/** Check if a mini-transaction is dirtying a clean page.
@param b	block being x-fixed
@return true if the mtr is dirtying a clean page. */
#define mtr_block_dirtied(b)	mtr_t::is_block_dirtied((b))

/** Append records to the system-wide redo log buffer.
@param[in]	log	redo log records */
void
mtr_write_log(
	const mtr_buf_t*	log);

/** Mini-transaction memo stack slot. */
struct mtr_memo_slot_t {
	/** pointer to the object */
	void*		object;

	/** type of the stored object (MTR_MEMO_S_LOCK, ...) */
	ulint		type;
};

/** Mini-transaction handle and buffer */
struct mtr_t {
	mtr_t() : m_state(MTR_STATE_INIT) {}

	/** Start a mini-transaction. */
	void start();

	/** Commit the mini-transaction. */
	void commit();

	/** Commit a mini-transaction that did not modify any pages,
	but generated some redo log on a higher level, such as
	MLOG_FILE_NAME records and a MLOG_CHECKPOINT marker.
	The caller must invoke log_mutex_enter() and log_mutex_exit().
	This is to be used at log_checkpoint().
	@param[in]	checkpoint_lsn		the LSN of the log checkpoint
	@param[in]	write_mlog_checkpoint	Write MLOG_CHECKPOINT marker
						if it is enabled. */
	void commit_checkpoint(
		lsn_t	checkpoint_lsn,
		bool	write_mlog_checkpoint);

	/** Return current size of the buffer.
	@return	savepoint */
	ulint get_savepoint() const {ut_ad(is_active()); return m_memo.size();}

	/** Release the (index tree) s-latch stored in an mtr memo after a
	savepoint.
	@param savepoint	value returned by @see set_savepoint.
	@param lock		latch to release */
	inline void release_s_latch_at_savepoint(
		ulint		savepoint,
		rw_lock_t*	lock);

	/** Release the block in an mtr memo after a savepoint. */
	inline void release_block_at_savepoint(
		ulint		savepoint,
		buf_block_t*	block);

	/** SX-latch a not yet latched block after a savepoint. */
	inline void sx_latch_at_savepoint(ulint savepoint, buf_block_t* block);

	/** X-latch a not yet latched block after a savepoint. */
	inline void x_latch_at_savepoint(ulint savepoint, buf_block_t*	block);

	/** Get the logging mode.
	@return	logging mode */
	inline mtr_log_t get_log_mode() const
		MY_ATTRIBUTE((warn_unused_result));

	/** Change the logging mode.
	@param mode	 logging mode
	@return	old mode */
	inline mtr_log_t set_log_mode(mtr_log_t mode);

	/** Copy the tablespaces associated with the mini-transaction
	(needed for generating MLOG_FILE_NAME records)
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
	(needed for generating a MLOG_FILE_NAME record)
	@param[in]	space_id	user or system tablespace ID
	@return	the tablespace */
	fil_space_t* set_named_space_id(ulint space_id)
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
	(needed for generating a MLOG_FILE_NAME record)
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
	(needed for generating a MLOG_FILE_NAME record)
	@param[in]	space	tablespace
	@return whether the mini-transaction is associated with the space */
	bool is_named_space(ulint space) const;
	/** Check the tablespace associated with the mini-transaction
	(needed for generating a MLOG_FILE_NAME record)
	@param[in]	space	tablespace
	@return whether the mini-transaction is associated with the space */
	bool is_named_space(const fil_space_t* space) const;
#endif /* UNIV_DEBUG */

	/** Acquire a tablespace X-latch.
	@param[in]	space_id	tablespace ID
	@param[in]	file		file name from where called
	@param[in]	line		line number in file
	@return the tablespace object (never NULL) */
	fil_space_t* x_lock_space(
		ulint		space_id,
		const char*	file,
		unsigned	line);

	/** Acquire a shared rw-latch.
	@param[in]	lock	rw-latch
	@param[in]	file	file name from where called
	@param[in]	line	line number in file */
	void s_lock(rw_lock_t* lock, const char* file, unsigned line)
	{
		rw_lock_s_lock_inline(lock, 0, file, line);
		memo_push(lock, MTR_MEMO_S_LOCK);
	}

	/** Acquire an exclusive rw-latch.
	@param[in]	lock	rw-latch
	@param[in]	file	file name from where called
	@param[in]	line	line number in file */
	void x_lock(rw_lock_t* lock, const char* file, unsigned line)
	{
		rw_lock_x_lock_inline(lock, 0, file, line);
		memo_push(lock, MTR_MEMO_X_LOCK);
	}

	/** Acquire an shared/exclusive rw-latch.
	@param[in]	lock	rw-latch
	@param[in]	file	file name from where called
	@param[in]	line	line number in file */
	void sx_lock(rw_lock_t* lock, const char* file, unsigned line)
	{
		rw_lock_sx_lock_inline(lock, 0, file, line);
		memo_push(lock, MTR_MEMO_SX_LOCK);
	}

	/** Acquire a tablespace S-latch.
	@param[in]	space	tablespace
	@param[in]	file	file name from where called
	@param[in]	line	line number in file */
	void s_lock_space(fil_space_t* space, const char* file, unsigned line)
	{
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT
		      || space->purpose == FIL_TYPE_TABLESPACE);
		s_lock(&space->latch, file, line);
	}

	/** Acquire a tablespace X-latch.
	@param[in]	space	tablespace
	@param[in]	file	file name from where called
	@param[in]	line	line number in file */
	void x_lock_space(fil_space_t* space, const char* file, unsigned line)
	{
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY
		      || space->purpose == FIL_TYPE_IMPORT
		      || space->purpose == FIL_TYPE_TABLESPACE);
		x_lock(&space->latch, file, line);
	}

	/** Release an object in the memo stack.
	@param object	object
	@param type	object type: MTR_MEMO_S_LOCK, ...
	@return bool if lock released */
	bool memo_release(const void* object, ulint type);
	/** Release a page latch.
	@param[in]	ptr	pointer to within a page frame
	@param[in]	type	object type: MTR_MEMO_PAGE_X_FIX, ... */
	void release_page(const void* ptr, mtr_memo_type_t type);

	/** Note that the mini-transaction has modified data. */
	void set_modified() { m_modifications = true; }

	/** Set the state to not-modified. This will not log the
	changes.  This is only used during redo log apply, to avoid
	logging the changes. */
	void discard_modifications() { m_modifications = false; }

	/** Get the LSN of commit().
	@return the commit LSN
	@retval 0 if the transaction only modified temporary tablespaces */
	lsn_t commit_lsn() const
	{
		ut_ad(has_committed());
		return(m_commit_lsn);
	}

	/** Note that we are inside the change buffer code. */
	void enter_ibuf() { m_inside_ibuf = true; }

	/** Note that we have exited from the change buffer code. */
	void exit_ibuf() { m_inside_ibuf = false; }

	/** @return true if we are inside the change buffer code */
	bool is_inside_ibuf() const { return m_inside_ibuf; }

	/*
	@return true if the mini-transaction is active */
	bool is_active() const { return m_state == MTR_STATE_ACTIVE; }

	/** Get flush observer
	@return flush observer */
	FlushObserver* get_flush_observer() const { return m_flush_observer; }

	/** Set flush observer
	@param[in]	observer	flush observer */
	void set_flush_observer(FlushObserver*	observer)
	{
		ut_ad(observer == NULL || m_log_mode == MTR_LOG_NO_REDO);
		m_flush_observer = observer;
	}

#ifdef UNIV_DEBUG
	/** Check if memo contains the given item.
	@param memo	memo stack
	@param object,	object to search
	@param type	type of object
	@return	true if contains */
	static bool memo_contains(
		const mtr_buf_t*	memo,
		const void*		object,
		ulint			type)
		MY_ATTRIBUTE((warn_unused_result));

	/** Check if memo contains the given item.
	@param object		object to search
	@param flags		specify types of object (can be ORred) of
				MTR_MEMO_PAGE_S_FIX ... values
	@return true if contains */
	bool memo_contains_flagged(const void* ptr, ulint flags) const;

	/** Check if memo contains the given page.
	@param[in]	ptr	pointer to within buffer frame
	@param[in]	flags	specify types of object with OR of
				MTR_MEMO_PAGE_S_FIX... values
	@return	the block
	@retval	NULL	if not found */
	buf_block_t* memo_contains_page_flagged(
		const byte*	ptr,
		ulint		flags) const;

	/** Mark the given latched page as modified.
	@param[in]	ptr	pointer to within buffer frame */
	void memo_modify_page(const byte* ptr);

	/** Print info of an mtr handle. */
	void print() const;

	/** @return true if the mini-transaction has committed */
	bool has_committed() const { return m_state == MTR_STATE_COMMITTED; }

	/** @return true if mini-transaction contains modifications. */
	bool has_modifications() const { return m_modifications; }

	/** @return the memo stack */
	const mtr_buf_t* get_memo() const { return &m_memo; }

	/** @return the memo stack */
	mtr_buf_t* get_memo() { return &m_memo; }
#endif /* UNIV_DEBUG */

	/** @return true if a record was added to the mini-transaction */
	bool is_dirty() const { return m_made_dirty; }

	/** Note that a record has been added to the log */
	void added_rec() { ++m_n_log_recs; }

	/** Get the buffered redo log of this mini-transaction.
	@return	redo log */
	const mtr_buf_t* get_log() const { return &m_log; }

	/** Get the buffered redo log of this mini-transaction.
	@return	redo log */
	mtr_buf_t* get_log() { return &m_log; }

	/** Push an object to an mtr memo stack.
	@param object	object
	@param type	object type: MTR_MEMO_S_LOCK, ... */
	inline void memo_push(void* object, mtr_memo_type_t type);

	/** Check if this mini-transaction is dirtying a clean page.
	@param block	block being x-fixed
	@return true if the mtr is dirtying a clean page. */
	static inline bool is_block_dirtied(const buf_block_t* block)
		MY_ATTRIBUTE((warn_unused_result));

private:
	/** Prepare to write the mini-transaction log to the redo log buffer.
	@return number of bytes to write in finish_write() */
	inline ulint prepare_write();

	/** Append the redo log records to the redo log buffer.
	@param[in]	len	number of bytes to write
	@return start_lsn */
	inline lsn_t finish_write(ulint len);

	/** Release the resources */
	inline void release_resources();

	/** memo stack for locks etc. */
	mtr_buf_t	m_memo;

	/** mini-transaction log */
	mtr_buf_t	m_log;

	/** true if mtr has made at least one buffer pool page dirty */
	bool		m_made_dirty;

	/** true if inside ibuf changes */
	bool		m_inside_ibuf;

	/** true if the mini-transaction modified buffer pool pages */
	bool		m_modifications;

	/** Count of how many page initial log records have been
	written to the mtr log */
	ib_uint32_t	m_n_log_recs;

	/** specifies which operations should be logged; default
	value MTR_LOG_ALL */
	mtr_log_t	m_log_mode;
#ifdef UNIV_DEBUG
	/** Persistent user tablespace associated with the
	mini-transaction, or 0 (TRX_SYS_SPACE) if none yet */
	ulint		m_user_space_id;
#endif /* UNIV_DEBUG */
	/** User tablespace that is being modified by the mini-transaction */
	fil_space_t*	m_user_space;

	/** State of the transaction */
	mtr_state_t	m_state;

	/** Flush Observer */
	FlushObserver*	m_flush_observer;

	/** LSN at commit time */
	lsn_t		m_commit_lsn;
};

#include "mtr0mtr.ic"

#endif /* mtr0mtr_h */
