/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "mtr0mtr.h"

#ifdef UNIV_NONINL
#include "mtr0mtr.ic"
#endif

#include "buf0buf.h"
#include "buf0flu.h"
#include "page0types.h"
#include "mtr0log.h"
#include "log0log.h"
#include "buf0flu.h"

#ifndef UNIV_HOTBACKUP
# include "log0recv.h"

/***************************************************//**
Checks if a mini-transaction is dirtying a clean page.
@return TRUE if the mtr is dirtying a clean page. */
UNIV_INTERN
ibool
mtr_block_dirtied(
/*==============*/
	const buf_block_t*	block)	/*!< in: block being x-fixed */
{
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.buf_fix_count > 0);

	/* It is OK to read oldest_modification because no
	other thread can be performing a write of it and it
	is only during write that the value is reset to 0. */
	return(block->page.oldest_modification == 0);
}

/*****************************************************************//**
Releases the item in the slot given. */
static MY_ATTRIBUTE((nonnull))
void
mtr_memo_slot_release_func(
/*=======================*/
#ifdef UNIV_DEBUG
 	mtr_t*			mtr,	/*!< in/out: mini-transaction */
#endif /* UNIV_DEBUG */
	mtr_memo_slot_t*	slot)	/*!< in: memo slot */
{
	void*	object = slot->object;
	slot->object = NULL;

	/* slot release is a local operation for the current mtr.
	We must not be holding the flush_order mutex while
	doing this. */
	ut_ad(!log_flush_order_mutex_own());

	switch (slot->type) {
	case MTR_MEMO_PAGE_S_FIX:
	case MTR_MEMO_PAGE_X_FIX:
	case MTR_MEMO_BUF_FIX:
		buf_page_release((buf_block_t*) object, slot->type);
		break;
	case MTR_MEMO_S_LOCK:
		rw_lock_s_unlock((prio_rw_lock_t*) object);
		break;
	case MTR_MEMO_X_LOCK:
		rw_lock_x_unlock((prio_rw_lock_t*) object);
		break;
#ifdef UNIV_DEBUG
	default:
		ut_ad(slot->type == MTR_MEMO_MODIFY);
		ut_ad(mtr_memo_contains(mtr, object, MTR_MEMO_PAGE_X_FIX));
#endif /* UNIV_DEBUG */
	}
}

#ifdef UNIV_DEBUG
# define mtr_memo_slot_release(mtr, slot) mtr_memo_slot_release_func(mtr, slot)
#else /* UNIV_DEBUG */
# define mtr_memo_slot_release(mtr, slot) mtr_memo_slot_release_func(slot)
#endif /* UNIV_DEBUG */

/**********************************************************//**
Releases the mlocks and other objects stored in an mtr memo.
They are released in the order opposite to which they were pushed
to the memo. */
static MY_ATTRIBUTE((nonnull))
void
mtr_memo_pop_all(
/*=============*/
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(mtr->magic_n == MTR_MAGIC_N);
	ut_ad(mtr->state == MTR_COMMITTING); /* Currently only used in
					     commit */

	for (const dyn_block_t* block = dyn_array_get_last_block(&mtr->memo);
	     block;
	     block = dyn_array_get_prev_block(&mtr->memo, block)) {
		const mtr_memo_slot_t*	start
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block));
		mtr_memo_slot_t*	slot
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block)
				+ dyn_block_get_used(block));

		ut_ad(!(dyn_block_get_used(block) % sizeof(mtr_memo_slot_t)));

		while (slot-- != start) {
			if (slot->object != NULL) {
				mtr_memo_slot_release(mtr, slot);
			}
		}
	}
}

/*****************************************************************//**
Releases the item in the slot given. */
static
void
mtr_memo_slot_note_modification(
/*============================*/
	mtr_t*			mtr,	/*!< in: mtr */
	mtr_memo_slot_t*	slot)	/*!< in: memo slot */
{
	ut_ad(mtr->modifications);
	ut_ad(!srv_read_only_mode);
	ut_ad(mtr->magic_n == MTR_MAGIC_N);

	if (slot->object != NULL && slot->type == MTR_MEMO_PAGE_X_FIX) {
		buf_block_t*	block = (buf_block_t*) slot->object;

		ut_ad(!mtr->made_dirty || log_flush_order_mutex_own());
		buf_flush_note_modification(block, mtr);
	}
}

/**********************************************************//**
Add the modified pages to the buffer flush list. They are released
in the order opposite to which they were pushed to the memo. NOTE! It is
essential that the x-rw-lock on a modified buffer page is not released
before buf_page_note_modification is called for that page! Otherwise,
some thread might race to modify it, and the flush list sort order on
lsn would be destroyed. */
static
void
mtr_memo_note_modifications(
/*========================*/
	mtr_t*	mtr)	/*!< in: mtr */
{
	ut_ad(!srv_read_only_mode);
	ut_ad(mtr->magic_n == MTR_MAGIC_N);
	ut_ad(mtr->state == MTR_COMMITTING); /* Currently only used in
					     commit */

	for (const dyn_block_t* block = dyn_array_get_last_block(&mtr->memo);
	     block;
	     block = dyn_array_get_prev_block(&mtr->memo, block)) {
		const mtr_memo_slot_t*	start
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block));
		mtr_memo_slot_t*	slot
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block)
				+ dyn_block_get_used(block));

		ut_ad(!(dyn_block_get_used(block) % sizeof(mtr_memo_slot_t)));

		while (slot-- != start) {
			if (slot->object != NULL) {
				mtr_memo_slot_note_modification(mtr, slot);
			}
		}
	}
}

/************************************************************//**
Append the dirty pages to the flush list. */
static
void
mtr_add_dirtied_pages_to_flush_list(
/*================================*/
	mtr_t*	mtr)	/*!< in/out: mtr */
{
	ut_ad(!srv_read_only_mode);

	/* No need to acquire log_flush_order_mutex if this mtr has
	not dirtied a clean page. log_flush_order_mutex is used to
	ensure ordered insertions in the flush_list. We need to
	insert in the flush_list iff the page in question was clean
	before modifications. */
	if (mtr->made_dirty) {
		log_flush_order_mutex_enter();
	}

	/* It is now safe to release the log mutex because the
	flush_order mutex will ensure that we are the first one
	to insert into the flush list. */
	log_release();

	if (mtr->modifications) {
		mtr_memo_note_modifications(mtr);
	}

	if (mtr->made_dirty) {
		log_flush_order_mutex_exit();
	}
}

/************************************************************//**
Writes the contents of a mini-transaction log, if any, to the database log. */
static
void
mtr_log_reserve_and_write(
/*======================*/
	mtr_t*	mtr)	/*!< in/out: mtr */
{
	dyn_array_t*	mlog;
	ulint		data_size;
	byte*		first_data;

	ut_ad(!srv_read_only_mode);

	mlog = &(mtr->log);

	first_data = dyn_block_get_data(mlog);

	if (mtr->n_log_recs > 1) {
		mlog_catenate_ulint(mtr, MLOG_MULTI_REC_END, MLOG_1BYTE);
	} else {
		*first_data = (byte)((ulint)*first_data
				     | MLOG_SINGLE_REC_FLAG);
	}

	if (mlog->heap == NULL) {
		ulint	len;

		len = mtr->log_mode != MTR_LOG_NO_REDO
			? dyn_block_get_used(mlog) : 0;

		mtr->end_lsn = log_reserve_and_write_fast(
			first_data, len, &mtr->start_lsn);

		if (mtr->end_lsn) {

			/* Success. We have the log mutex.
			Add pages to flush list and exit */
			mtr_add_dirtied_pages_to_flush_list(mtr);

			return;
		}
	} else {
		mutex_enter(&log_sys->mutex);
	}

	data_size = dyn_array_get_data_size(mlog);

	/* Open the database log for log_write_low */
	mtr->start_lsn = log_open(data_size);

	if (mtr->log_mode == MTR_LOG_ALL) {

		for (dyn_block_t* block = mlog;
		     block != 0;
		     block = dyn_array_get_next_block(mlog, block)) {

			log_write_low(
				dyn_block_get_data(block),
				dyn_block_get_used(block));
		}

	} else {
		ut_ad(mtr->log_mode == MTR_LOG_NONE
		      || mtr->log_mode == MTR_LOG_NO_REDO);
		/* Do nothing */
	}

	mtr->end_lsn = log_close();

	mtr_add_dirtied_pages_to_flush_list(mtr);
}
#endif /* !UNIV_HOTBACKUP */

/***************************************************************//**
Commits a mini-transaction. */
UNIV_INTERN
void
mtr_commit(
/*=======*/
	mtr_t*	mtr)	/*!< in: mini-transaction */
{
	ut_ad(mtr->magic_n == MTR_MAGIC_N);
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad(!mtr->inside_ibuf);
	ut_d(mtr->state = MTR_COMMITTING);

#ifndef UNIV_HOTBACKUP
	/* This is a dirty read, for debugging. */
	ut_ad(!recv_no_log_write);

	if (mtr->modifications && mtr->n_log_recs) {
		ut_ad(!srv_read_only_mode);
		mtr_log_reserve_and_write(mtr);
	}

	mtr_memo_pop_all(mtr);
#endif /* !UNIV_HOTBACKUP */

	dyn_array_free(&(mtr->memo));
	dyn_array_free(&(mtr->log));
#ifdef UNIV_DEBUG_VALGRIND
	/* Declare everything uninitialized except
	mtr->start_lsn, mtr->end_lsn and mtr->state. */
	{
		lsn_t	start_lsn	= mtr->start_lsn;
		lsn_t	end_lsn		= mtr->end_lsn;
		UNIV_MEM_INVALID(mtr, sizeof *mtr);
		mtr->start_lsn = start_lsn;
		mtr->end_lsn = end_lsn;
	}
#endif /* UNIV_DEBUG_VALGRIND */
	ut_d(mtr->state = MTR_COMMITTED);
}

#ifndef UNIV_HOTBACKUP
/***************************************************//**
Releases an object in the memo stack.
@return true if released */
UNIV_INTERN
bool
mtr_memo_release(
/*=============*/
	mtr_t*	mtr,	/*!< in/out: mini-transaction */
	void*	object,	/*!< in: object */
	ulint	type)	/*!< in: object type: MTR_MEMO_S_LOCK, ... */
{
	ut_ad(mtr->magic_n == MTR_MAGIC_N);
	ut_ad(mtr->state == MTR_ACTIVE);
	/* We cannot release a page that has been written to in the
	middle of a mini-transaction. */
	ut_ad(!mtr->modifications || type != MTR_MEMO_PAGE_X_FIX);

	for (const dyn_block_t* block = dyn_array_get_last_block(&mtr->memo);
	     block;
	     block = dyn_array_get_prev_block(&mtr->memo, block)) {
		const mtr_memo_slot_t*	start
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block));
		mtr_memo_slot_t*	slot
			= reinterpret_cast<mtr_memo_slot_t*>(
				dyn_block_get_data(block)
				+ dyn_block_get_used(block));

		ut_ad(!(dyn_block_get_used(block) % sizeof(mtr_memo_slot_t)));

		while (slot-- != start) {
			if (object == slot->object && type == slot->type) {
				mtr_memo_slot_release(mtr, slot);
				return(true);
			}
		}
	}

	return(false);
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************//**
Reads 1 - 4 bytes from a file page buffered in the buffer pool.
@return	value read */
UNIV_INTERN
ulint
mtr_read_ulint(
/*===========*/
	const byte*	ptr,	/*!< in: pointer from where to read */
	ulint		type,	/*!< in: MLOG_1BYTE, MLOG_2BYTES, MLOG_4BYTES */
	mtr_t*		mtr MY_ATTRIBUTE((unused)))
				/*!< in: mini-transaction handle */
{
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad(mtr_memo_contains_page(mtr, ptr, MTR_MEMO_PAGE_S_FIX)
	      || mtr_memo_contains_page(mtr, ptr, MTR_MEMO_PAGE_X_FIX));

	return(mach_read_ulint(ptr, type));
}

#ifdef UNIV_DEBUG
# ifndef UNIV_HOTBACKUP
/**********************************************************//**
Checks if memo contains the given page.
@return	TRUE if contains */
UNIV_INTERN
ibool
mtr_memo_contains_page(
/*===================*/
	mtr_t*		mtr,	/*!< in: mtr */
	const byte*	ptr,	/*!< in: pointer to buffer frame */
	ulint		type)	/*!< in: type of object */
{
	return(mtr_memo_contains(mtr, buf_block_align(ptr), type));
}

/*********************************************************//**
Prints info of an mtr handle. */
UNIV_INTERN
void
mtr_print(
/*======*/
	mtr_t*	mtr)	/*!< in: mtr */
{
	fprintf(stderr,
		"Mini-transaction handle: memo size %lu bytes"
		" log size %lu bytes\n",
		(ulong) dyn_array_get_data_size(&(mtr->memo)),
		(ulong) dyn_array_get_data_size(&(mtr->log)));
}
# endif /* !UNIV_HOTBACKUP */
#endif /* UNIV_DEBUG */

/**********************************************************//**
Releases a buf_page stored in an mtr memo after a
savepoint. */
UNIV_INTERN
void
mtr_release_buf_page_at_savepoint(
/*=============================*/
	mtr_t*		mtr,		/*!< in: mtr */
	ulint		savepoint,	/*!< in: savepoint */
	buf_block_t*	block)		/*!< in: block to release */
{
	mtr_memo_slot_t* slot;
	dyn_array_t*	memo;

	ut_ad(mtr);
	ut_ad(mtr->magic_n == MTR_MAGIC_N);
	ut_ad(mtr->state == MTR_ACTIVE);

	memo = &(mtr->memo);

	ut_ad(dyn_array_get_data_size(memo) > savepoint);

	slot = (mtr_memo_slot_t*) dyn_array_get_element(memo, savepoint);

	ut_ad(slot->object == block);
	ut_ad(slot->type == MTR_MEMO_PAGE_S_FIX ||
	      slot->type == MTR_MEMO_PAGE_X_FIX ||
	      slot->type == MTR_MEMO_BUF_FIX);

	buf_page_release((buf_block_t*) slot->object, slot->type);
	slot->object = NULL;
}
