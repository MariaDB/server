/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2020, MariaDB Corporation.

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
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "buf0dblwr.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"

using st_::span;

/** The doublewrite buffer */
buf_dblwr_t*	buf_dblwr = NULL;

/** Set to TRUE when the doublewrite buffer is being created */
ibool	buf_dblwr_being_created = FALSE;

#define TRX_SYS_DOUBLEWRITE_BLOCKS 2

/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no)	/*!< in: page number */
{
	if (buf_dblwr == NULL) {

		return(FALSE);
	}

	if (page_no >= buf_dblwr->block1
	    && page_no < buf_dblwr->block1
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	if (page_no >= buf_dblwr->block2
	    && page_no < buf_dblwr->block2
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	return(FALSE);
}

/****************************************************************//**
Calls buf_page_get() on the TRX_SYS_PAGE and returns a pointer to the
doublewrite buffer within it.
@return pointer to the doublewrite buffer within the filespace header
page. */
UNIV_INLINE
byte*
buf_dblwr_get(
/*==========*/
	mtr_t*	mtr)	/*!< in/out: MTR to hold the page latch */
{
	buf_block_t*	block;

	block = buf_page_get(page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
			     univ_page_size, RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	return(buf_block_get_frame(block) + TRX_SYS_DOUBLEWRITE);
}

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written to the dblwr buffer on disk. */
void
buf_dblwr_sync_datafiles()
/*======================*/
{
	/* Wake possible simulated aio thread to actually post the
	writes to the operating system */
	os_aio_simulated_wake_handler_threads();

	/* Wait that all async writes to tablespaces have been posted to
	the OS */
	os_aio_wait_until_no_pending_writes();
}

/****************************************************************//**
Creates or initialializes the doublewrite buffer at a database start. */
static
void
buf_dblwr_init(
/*===========*/
	byte*	doublewrite)	/*!< in: pointer to the doublewrite buf
				header on trx sys page */
{
	ulint	buf_size;

	buf_dblwr = static_cast<buf_dblwr_t*>(
		ut_zalloc_nokey(sizeof(buf_dblwr_t)));

	/* There are two blocks of same size in the doublewrite
	buffer. */
	buf_size = TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	/* There must be atleast one buffer for single page writes
	and one buffer for batch writes. */
	ut_a(srv_doublewrite_batch_size > 0
	     && srv_doublewrite_batch_size < buf_size);

	mutex_create(LATCH_ID_BUF_DBLWR, &buf_dblwr->mutex);

	buf_dblwr->b_event = os_event_create("dblwr_batch_event");
	buf_dblwr->s_event = os_event_create("dblwr_single_event");
	buf_dblwr->first_free = 0;
	buf_dblwr->s_reserved = 0;
	buf_dblwr->b_reserved = 0;

	buf_dblwr->block1 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK1);
	buf_dblwr->block2 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK2);

	buf_dblwr->in_use = static_cast<bool*>(
		ut_zalloc_nokey(buf_size * sizeof(bool)));

	buf_dblwr->write_buf_unaligned = static_cast<byte*>(
		ut_malloc_nokey((1 + buf_size) * UNIV_PAGE_SIZE));

	buf_dblwr->write_buf = static_cast<byte*>(
		ut_align(buf_dblwr->write_buf_unaligned,
			 UNIV_PAGE_SIZE));

	buf_dblwr->buf_block_arr = static_cast<buf_page_t**>(
		ut_zalloc_nokey(buf_size * sizeof(void*)));
}

/** Create the doublewrite buffer if the doublewrite buffer header
is not present in the TRX_SYS page.
@return	whether the operation succeeded
@retval	true	if the doublewrite buffer exists or was created
@retval	false	if the creation failed (too small first data file) */
bool
buf_dblwr_create()
{
	buf_block_t*	block2;
	buf_block_t*	new_block;
	buf_block_t*	trx_sys_block;
	byte*	doublewrite;
	byte*	fseg_header;
	ulint	page_no;
	ulint	prev_page_no;
	ulint	i;
	mtr_t	mtr;

	if (buf_dblwr) {
		/* Already inited */
		return(true);
	}

start_again:
	mtr.start();
	buf_dblwr_being_created = TRUE;

	doublewrite = buf_dblwr_get(&mtr);

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has already been created:
		just read in some numbers */

		buf_dblwr_init(doublewrite);

		mtr.commit();
		buf_dblwr_being_created = FALSE;
		return(true);
	} else {
		fil_space_t* space = fil_space_acquire(TRX_SYS_SPACE);
		const bool fail = UT_LIST_GET_FIRST(space->chain)->size
			< 3 * FSP_EXTENT_SIZE;
		fil_space_release(space);

		if (fail) {
			goto too_small;
		}
	}

	trx_sys_block = buf_page_get(
		page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
		page_size_t(srv_page_size, srv_page_size, 0), RW_X_LATCH,
		&mtr);

	block2 = fseg_create(TRX_SYS_SPACE,
			     TRX_SYS_DOUBLEWRITE
			     + TRX_SYS_DOUBLEWRITE_FSEG, &mtr, trx_sys_block);

	if (block2 == NULL) {
too_small:
		ib::error()
			<< "Cannot create doublewrite buffer: "
			"the first file in innodb_data_file_path"
			" must be at least "
			<< (3 * (FSP_EXTENT_SIZE * UNIV_PAGE_SIZE) >> 20)
			<< "M.";
		mtr.commit();
		return(false);
	}

	ib::info() << "Doublewrite buffer not found: creating new";

	/* FIXME: After this point, the doublewrite buffer creation
	is not atomic. The doublewrite buffer should not exist in
	the InnoDB system tablespace file in the first place.
	It could be located in separate optional file(s) in a
	user-specified location. */

	/* fseg_create acquires a second latch on the page,
	therefore we must declare it: */

	buf_block_dbg_add_level(block2, SYNC_NO_ORDER_CHECK);

	fseg_header = doublewrite + TRX_SYS_DOUBLEWRITE_FSEG;
	prev_page_no = 0;

	for (i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		     + FSP_EXTENT_SIZE / 2; i++) {
		new_block = fseg_alloc_free_page(
			fseg_header, prev_page_no + 1, FSP_UP, &mtr);
		if (new_block == NULL) {
			ib::error() << "Cannot create doublewrite buffer: "
				" you must increase your tablespace size."
				" Cannot continue operation.";
			/* This may essentially corrupt the doublewrite
			buffer. However, usually the doublewrite buffer
			is created at database initialization, and it
			should not matter (just remove all newly created
			InnoDB files and restart). */
			mtr.commit();
			return(false);
		}

		/* We read the allocated pages to the buffer pool;
		when they are written to disk in a flush, the space
		id and page number fields are also written to the
		pages. When we at database startup read pages
		from the doublewrite buffer, we know that if the
		space id and page number in them are the same as
		the page position in the tablespace, then the page
		has not been written to in doublewrite. */

		ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
		page_no = new_block->page.id.page_no();
		/* We only do this in the debug build, to ensure that
		both the check in buf_flush_init_for_writing() and
		recv_parse_or_apply_log_rec_body() will see a valid
		page type. The flushes of new_block are actually
		unnecessary here.  */
		ut_d(mlog_write_ulint(FIL_PAGE_TYPE + new_block->frame,
				      FIL_PAGE_TYPE_SYS, MLOG_2BYTES, &mtr));

		if (i == FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK1,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i == FSP_EXTENT_SIZE / 2
			   + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			ut_a(page_no == 2 * FSP_EXTENT_SIZE);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);
			mlog_write_ulint(doublewrite
					 + TRX_SYS_DOUBLEWRITE_REPEAT
					 + TRX_SYS_DOUBLEWRITE_BLOCK2,
					 page_no, MLOG_4BYTES, &mtr);

		} else if (i > FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == prev_page_no + 1);
		}

		if (((i + 1) & 15) == 0) {
			/* rw_locks can only be recursively x-locked
			2048 times. (on 32 bit platforms,
			(lint) 0 - (X_LOCK_DECR * 2049)
			is no longer a negative number, and thus
			lock_word becomes like a shared lock).
			For 4k page size this loop will
			lock the fseg header too many times. Since
			this code is not done while any other threads
			are active, restart the MTR occasionally. */
			mtr_commit(&mtr);
			mtr_start(&mtr);
			doublewrite = buf_dblwr_get(&mtr);
			fseg_header = doublewrite
				      + TRX_SYS_DOUBLEWRITE_FSEG;
		}

		prev_page_no = page_no;
	}

	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);
	mlog_write_ulint(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC
			 + TRX_SYS_DOUBLEWRITE_REPEAT,
			 TRX_SYS_DOUBLEWRITE_MAGIC_N,
			 MLOG_4BYTES, &mtr);

	mlog_write_ulint(doublewrite
			 + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED,
			 TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N,
			 MLOG_4BYTES, &mtr);
	mtr_commit(&mtr);

	/* Flush the modified pages to disk and make a checkpoint */
	log_make_checkpoint();
	buf_dblwr_being_created = FALSE;

	/* Remove doublewrite pages from LRU */
	buf_pool_invalidate();

	ib::info() <<  "Doublewrite buffer created";

	goto start_again;
}

/**
At database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory.
@param[in]	file		File handle
@param[in]	path		Path name of file
@return DB_SUCCESS or error code */
dberr_t
buf_dblwr_init_or_load_pages(
	pfs_os_file_t	file,
	const char*	path)
{
	byte*		buf;
	byte*		page;
	ulint		block1;
	ulint		block2;
	ulint		space_id;
	byte*		read_buf;
	byte*		doublewrite;
	byte*		unaligned_read_buf;
	ibool		reset_space_ids = FALSE;
	recv_dblwr_t&	recv_dblwr = recv_sys->dblwr;

	/* We do the file i/o past the buffer pool */

	unaligned_read_buf = static_cast<byte*>(
		ut_malloc_nokey(3 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	/* Read the trx sys header to check if we are using the doublewrite
	buffer */
	dberr_t		err;

	IORequest	read_request(IORequest::READ);

	err = os_file_read(
		read_request,
		file, read_buf, TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE,
		UNIV_PAGE_SIZE);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the system tablespace header page";

		ut_free(unaligned_read_buf);

		return(err);
	}

	doublewrite = read_buf + TRX_SYS_DOUBLEWRITE;

	/* TRX_SYS_PAGE_NO is not encrypted see fil_crypt_rotate_page() */

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has been created */

		buf_dblwr_init(doublewrite);

		block1 = buf_dblwr->block1;
		block2 = buf_dblwr->block2;

		buf = buf_dblwr->write_buf;
	} else {
		ut_free(unaligned_read_buf);
		return(DB_SUCCESS);
	}

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED)
	    != TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N) {

		/* We are upgrading from a version < 4.1.x to a version where
		multiple tablespaces are supported. We must reset the space id
		field in the pages in the doublewrite buffer because starting
		from this version the space id is stored to
		FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */

		reset_space_ids = TRUE;

		ib::info() << "Resetting space id's in the doublewrite buffer";
	}

	/* Read the pages from the doublewrite buffer to memory */
	err = os_file_read(
		read_request,
		file, buf, block1 * UNIV_PAGE_SIZE,
		TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the first double write buffer "
			"extent";

		ut_free(unaligned_read_buf);

		return(err);
	}

	err = os_file_read(
		read_request,
		file,
		buf + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE,
		block2 * UNIV_PAGE_SIZE,
		TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the second double write buffer "
			"extent";

		ut_free(unaligned_read_buf);

		return(err);
	}

	/* Check if any of these pages is half-written in data files, in the
	intended position */

	page = buf;

	for (ulint i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 2; i++) {
		if (reset_space_ids) {
			ulint source_page_no;

			space_id = 0;
			mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
					space_id);
			/* We do not need to calculate new checksums for the
			pages because the field .._SPACE_ID does not affect
			them. Write the page back to where we read it from. */

			if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
				source_page_no = block1 + i;
			} else {
				source_page_no = block2
					+ i - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			}

			IORequest	write_request(IORequest::WRITE);

			err = os_file_write(
				write_request, path, file, page,
				source_page_no * UNIV_PAGE_SIZE,
				UNIV_PAGE_SIZE);
			if (err != DB_SUCCESS) {

				ib::error()
					<< "Failed to write to the double write"
					" buffer";

				ut_free(unaligned_read_buf);

				return(err);
			}

		} else if (memcmp(field_ref_zero, page + FIL_PAGE_LSN, 8)) {
			/* Each valid page header must contain
			a nonzero FIL_PAGE_LSN field. */
			recv_dblwr.add(page);
		}

		page += univ_page_size.physical();
	}

	if (reset_space_ids) {
		os_file_flush(file);
	}

	ut_free(unaligned_read_buf);

	return(DB_SUCCESS);
}

/** Process and remove the double write buffer pages for all tablespaces. */
void
buf_dblwr_process()
{
	ut_ad(recv_sys->parse_start_lsn);

	ulint		page_no_dblwr	= 0;
	byte*		read_buf;
	recv_dblwr_t&	recv_dblwr	= recv_sys->dblwr;

	if (!buf_dblwr) {
		return;
	}

	read_buf = static_cast<byte*>(
		aligned_malloc(3 * srv_page_size, srv_page_size));
	byte* const buf = read_buf + srv_page_size;

	for (recv_dblwr_t::list::iterator i = recv_dblwr.pages.begin();
	     i != recv_dblwr.pages.end();
	     ++i, ++page_no_dblwr) {
		byte* page = *i;
		const ulint page_no = page_get_page_no(page);

		if (!page_no) {
			/* page 0 should have been recovered
			already via Datafile::restore_from_doublewrite() */
			continue;
		}

		const ulint space_id = page_get_space_id(page);
		const lsn_t lsn = mach_read_from_8(page + FIL_PAGE_LSN);

		if (recv_sys->parse_start_lsn > lsn) {
			/* Pages written before the checkpoint are
			not useful for recovery. */
			continue;
		}

		const page_id_t page_id(space_id, page_no);

		if (recv_sys->scanned_lsn < lsn) {
			ib::warn() << "Ignoring a doublewrite copy of page "
				   << page_id
				   << " with future log sequence number "
				   << lsn;
			continue;
		}

		fil_space_t* space = fil_space_acquire_for_io(space_id);

		if (!space) {
			/* Maybe we have dropped the tablespace
			and this page once belonged to it: do nothing */
			continue;
		}

		fil_space_open_if_needed(space);

		if (UNIV_UNLIKELY(page_no >= space->size)) {

			/* Do not report the warning if the tablespace
			is scheduled for truncation or was truncated
			and we have parsed an MLOG_TRUNCATE record. */
			if (!srv_is_tablespace_truncated(space_id)
			    && !srv_was_tablespace_truncated(space)
			    && !srv_is_undo_tablespace(space_id)) {
				ib::warn() << "A copy of page " << page_no
					<< " in the doublewrite buffer slot "
					<< page_no_dblwr
					<< " is beyond the end of tablespace "
					<< space->name
					<< " (" << space->size << " pages)";
			}
next_page:
			fil_space_release_for_io(space);
			continue;
		}

		const page_size_t	page_size(space->flags);
		ut_ad(!buf_is_zeroes(span<const byte>(page,
						      page_size.physical())));

		/* We want to ensure that for partial reads the
		unread portion of the page is NUL. */
		memset(read_buf, 0x0, page_size.physical());

		IORequest	request;

		request.dblwr_recover();

		/* Read in the actual page from the file */
		dberr_t	err = fil_io(
			request, true,
			page_id, page_size,
				0, page_size.physical(), read_buf, NULL);

		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			ib::warn()
				<< "Double write buffer recovery: "
				<< page_id << " read failed with "
				<< "error: " << err;
		}

		if (buf_is_zeroes(span<const byte>(read_buf,
						   page_size.physical()))) {
			/* We will check if the copy in the
			doublewrite buffer is valid. If not, we will
			ignore this page (there should be redo log
			records to initialize it). */
		} else if (recv_dblwr.validate_page(
				page_id, read_buf, space, buf)) {
			goto next_page;
		} else {
			/* We intentionally skip this message for
			all-zero pages. */
			ib::info()
				<< "Trying to recover page " << page_id
				<< " from the doublewrite buffer.";
		}

		page = recv_dblwr.find_page(page_id, space, buf);

		if (!page) {
			goto next_page;
		}

		/* Write the good page from the doublewrite buffer to
		the intended position. */

		IORequest	write_request(IORequest::WRITE);

		fil_io(write_request, true, page_id, page_size,
		       0, page_size.physical(), page, NULL);

		ib::info() << "Recovered page " << page_id
			<< " from the doublewrite buffer.";

		goto next_page;
	}

	recv_dblwr.pages.clear();

	fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
	aligned_free(read_buf);
}

/****************************************************************//**
Frees doublewrite buffer. */
void
buf_dblwr_free()
{
	/* Free the double write data structures. */
	ut_a(buf_dblwr != NULL);
	ut_ad(buf_dblwr->s_reserved == 0);
	ut_ad(buf_dblwr->b_reserved == 0);

	os_event_destroy(buf_dblwr->b_event);
	os_event_destroy(buf_dblwr->s_event);
	ut_free(buf_dblwr->write_buf_unaligned);
	buf_dblwr->write_buf_unaligned = NULL;

	ut_free(buf_dblwr->buf_block_arr);
	buf_dblwr->buf_block_arr = NULL;

	ut_free(buf_dblwr->in_use);
	buf_dblwr->in_use = NULL;

	mutex_free(&buf_dblwr->mutex);
	ut_free(buf_dblwr);
	buf_dblwr = NULL;
}

/********************************************************************//**
Updates the doublewrite buffer when an IO request is completed. */
void
buf_dblwr_update(
/*=============*/
	const buf_page_t*	bpage,	/*!< in: buffer block descriptor */
	buf_flush_t		flush_type)/*!< in: flush type */
{
	ut_ad(srv_use_doublewrite_buf);
	ut_ad(buf_dblwr);
	ut_ad(!fsp_is_system_temporary(bpage->id.space()));
	ut_ad(!srv_read_only_mode);

	switch (flush_type) {
	case BUF_FLUSH_LIST:
	case BUF_FLUSH_LRU:
		mutex_enter(&buf_dblwr->mutex);

		ut_ad(buf_dblwr->batch_running);
		ut_ad(buf_dblwr->b_reserved > 0);
		ut_ad(buf_dblwr->b_reserved <= buf_dblwr->first_free);

		buf_dblwr->b_reserved--;

		if (buf_dblwr->b_reserved == 0) {
			mutex_exit(&buf_dblwr->mutex);
			/* This will finish the batch. Sync data files
			to the disk. */
			fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
			mutex_enter(&buf_dblwr->mutex);

			/* We can now reuse the doublewrite memory buffer: */
			buf_dblwr->first_free = 0;
			buf_dblwr->batch_running = false;
			os_event_set(buf_dblwr->b_event);
		}

		mutex_exit(&buf_dblwr->mutex);
		break;
	case BUF_FLUSH_SINGLE_PAGE:
		{
			const ulint size = TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			ulint i;
			mutex_enter(&buf_dblwr->mutex);
			for (i = srv_doublewrite_batch_size; i < size; ++i) {
				if (buf_dblwr->buf_block_arr[i] == bpage) {
					buf_dblwr->s_reserved--;
					buf_dblwr->buf_block_arr[i] = NULL;
					buf_dblwr->in_use[i] = false;
					break;
				}
			}

			/* The block we are looking for must exist as a
			reserved block. */
			ut_a(i < size);
		}
		os_event_set(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		break;
	case BUF_FLUSH_N_TYPES:
		ut_error;
	}
}

/********************************************************************//**
Check the LSN values on the page. */
static
void
buf_dblwr_check_page_lsn(
/*=====================*/
	const page_t*	page)		/*!< in: page to check */
{
	ibool page_compressed = (mach_read_from_2(page+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED);
	uint key_version = mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	/* Ignore page compressed or encrypted pages */
	if (page_compressed || key_version) {
		return;
	}

	if (memcmp(page + (FIL_PAGE_LSN + 4),
		   page + (UNIV_PAGE_SIZE
			   - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
		   4)) {

		const ulint	lsn1 = mach_read_from_4(
			page + FIL_PAGE_LSN + 4);
		const ulint	lsn2 = mach_read_from_4(
			page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM
			+ 4);

		ib::error() << "The page to be written seems corrupt!"
			" The low 4 bytes of LSN fields do not match"
			" (" << lsn1 << " != " << lsn2 << ")!"
			" Noticed in the buffer pool.";
	}
}

/********************************************************************//**
Asserts when a corrupt block is find during writing out data to the
disk. */
static
void
buf_dblwr_assert_on_corrupt_block(
/*==============================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	buf_page_print(block->frame, univ_page_size);

	ib::fatal() << "Apparent corruption of an index page "
		<< block->page.id
		<< " to be written to data file. We intentionally crash"
		" the server to prevent corrupt data from ending up in"
		" data files.";
}

/********************************************************************//**
Check the LSN values on the page with which this block is associated.
Also validate the page if the option is set. */
static
void
buf_dblwr_check_block(
/*==================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	switch (fil_page_get_type(block->frame)) {
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		if (page_is_comp(block->frame)) {
			if (page_simple_validate_new(block->frame)) {
				return;
			}
		} else if (page_simple_validate_old(block->frame)) {
			return;
		}
		/* While it is possible that this is not an index page
		but just happens to have wrongly set FIL_PAGE_TYPE,
		such pages should never be modified to without also
		adjusting the page type during page allocation or
		buf_flush_init_for_writing() or fil_block_reset_type(). */
		break;
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_UNKNOWN:
		/* Do not complain again, we already reset this field. */
	case FIL_PAGE_UNDO_LOG:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_FREE_LIST:
	case FIL_PAGE_TYPE_SYS:
	case FIL_PAGE_TYPE_TRX_SYS:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_BLOB:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* TODO: validate also non-index pages */
		return;
	case FIL_PAGE_TYPE_ALLOCATED:
		/* empty pages should never be flushed */
		return;
		break;
	}

	buf_dblwr_assert_on_corrupt_block(block);
}

/********************************************************************//**
Writes a page that has already been written to the doublewrite buffer
to the datafile. It is the job of the caller to sync the datafile. */
static
void
buf_dblwr_write_block_to_datafile(
/*==============================*/
	const buf_page_t*	bpage,	/*!< in: page to write */
	bool			sync)	/*!< in: true if sync IO
					is requested */
{
	ut_a(buf_page_in_file(bpage));

	ulint	type = IORequest::WRITE;

	if (sync) {
		type |= IORequest::DO_NOT_WAKE;
	}

	IORequest	request(type, const_cast<buf_page_t*>(bpage));

	/* We request frame here to get correct buffer in case of
	encryption and/or page compression */
	void * frame = buf_page_get_frame(bpage);

	if (bpage->zip.data != NULL) {
		ut_ad(bpage->size.is_compressed());

		fil_io(request, sync, bpage->id, bpage->size, 0,
		       bpage->size.physical(),
		       (void*) frame,
		       (void*) bpage);
	} else {
		ut_ad(!bpage->size.is_compressed());

		/* Our IO API is common for both reads and writes and is
		therefore geared towards a non-const parameter. */

		buf_block_t*	block = reinterpret_cast<buf_block_t*>(
			const_cast<buf_page_t*>(bpage));

		ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
		buf_dblwr_check_page_lsn(block->frame);

		fil_io(request,
			sync, bpage->id, bpage->size, 0, bpage->real_size,
			frame, block);
	}
}

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
void
buf_dblwr_flush_buffered_writes()
{
	byte*		write_buf;
	ulint		first_free;
	ulint		len;

	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		/* Sync the writes to the disk. */
		buf_dblwr_sync_datafiles();
		/* Now we flush the data to disk (for example, with fsync) */
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
		return;
	}

	ut_ad(!srv_read_only_mode);

try_again:
	mutex_enter(&buf_dblwr->mutex);

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

	if (buf_dblwr->first_free == 0) {

		mutex_exit(&buf_dblwr->mutex);

		/* Wake possible simulated aio thread as there could be
		system temporary tablespace pages active for flushing.
		Note: system temporary tablespace pages are not scheduled
		for doublewrite. */
		os_aio_simulated_wake_handler_threads();

		return;
	}

	if (buf_dblwr->batch_running) {
		/* Another thread is running the batch right now. Wait
		for it to finish. */
		int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_aio_simulated_wake_handler_threads();
		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	ut_ad(buf_dblwr->first_free == buf_dblwr->b_reserved);

	/* Disallow anyone else to post to doublewrite buffer or to
	start another batch of flushing. */
	buf_dblwr->batch_running = true;
	first_free = buf_dblwr->first_free;

	/* Now safe to release the mutex. Note that though no other
	thread is allowed to post to the doublewrite batch flushing
	but any threads working on single page flushes are allowed
	to proceed. */
	mutex_exit(&buf_dblwr->mutex);

	write_buf = buf_dblwr->write_buf;

	for (ulint len2 = 0, i = 0;
	     i < buf_dblwr->first_free;
	     len2 += UNIV_PAGE_SIZE, i++) {

		const buf_block_t*	block;

		block = (buf_block_t*) buf_dblwr->buf_block_arr[i];

		if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
		    || block->page.zip.data) {
			/* No simple validate for compressed
			pages exists. */
			continue;
		}

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block(block);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		buf_dblwr_check_page_lsn(write_buf + len2);
	}

	/* Write out the first block of the doublewrite buffer */
	len = ut_min(TRX_SYS_DOUBLEWRITE_BLOCK_SIZE,
		     buf_dblwr->first_free) * UNIV_PAGE_SIZE;

	fil_io(IORequestWrite, true,
	       page_id_t(TRX_SYS_SPACE, buf_dblwr->block1), univ_page_size,
	       0, len, (void*) write_buf, NULL);

	if (buf_dblwr->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		/* No unwritten pages in the second block. */
		goto flush;
	}

	/* Write out the second block of the doublewrite buffer. */
	len = (buf_dblwr->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
	       * UNIV_PAGE_SIZE;

	write_buf = buf_dblwr->write_buf
		    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

	fil_io(IORequestWrite, true,
	       page_id_t(TRX_SYS_SPACE, buf_dblwr->block2), univ_page_size,
	       0, len, (void*) write_buf, NULL);

flush:
	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.add(buf_dblwr->first_free);
	srv_stats.dblwr_writes.inc();

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(TRX_SYS_SPACE);

	/* We know that the writes have been flushed to disk now
	and in recovery we will find them in the doublewrite buffer
	blocks. Next do the writes to the intended positions. */

	/* Up to this point first_free and buf_dblwr->first_free are
	same because we have set the buf_dblwr->batch_running flag
	disallowing any other thread to post any request but we
	can't safely access buf_dblwr->first_free in the loop below.
	This is so because it is possible that after we are done with
	the last iteration and before we terminate the loop, the batch
	gets finished in the IO helper thread and another thread posts
	a new batch setting buf_dblwr->first_free to a higher value.
	If this happens and we are using buf_dblwr->first_free in the
	loop termination condition then we'll end up dispatching
	the same block twice from two different threads. */
	ut_ad(first_free == buf_dblwr->first_free);
	for (ulint i = 0; i < first_free; i++) {
		buf_dblwr_write_block_to_datafile(
			buf_dblwr->buf_block_arr[i], false);
	}

	/* Wake possible simulated aio thread to actually post the
	writes to the operating system. We don't flush the files
	at this point. We leave it to the IO helper thread to flush
	datafiles when the whole batch has been processed. */
	os_aio_simulated_wake_handler_threads();
}

/********************************************************************//**
Posts a buffer page for writing. If the doublewrite memory buffer is
full, calls buf_dblwr_flush_buffered_writes and waits for for free
space to appear. */
void
buf_dblwr_add_to_batch(
/*====================*/
	buf_page_t*	bpage)	/*!< in: buffer block to write */
{
	ut_a(buf_page_in_file(bpage));

try_again:
	mutex_enter(&buf_dblwr->mutex);

	ut_a(buf_dblwr->first_free <= srv_doublewrite_batch_size);

	if (buf_dblwr->batch_running) {

		/* This not nearly as bad as it looks. There is only
		page_cleaner thread which does background flushing
		in batches therefore it is unlikely to be a contention
		point. The only exception is when a user thread is
		forced to do a flush batch because of a sync
		checkpoint. */
		int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);
		os_aio_simulated_wake_handler_threads();

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	if (buf_dblwr->first_free == srv_doublewrite_batch_size) {
		mutex_exit(&(buf_dblwr->mutex));

		buf_dblwr_flush_buffered_writes();

		goto try_again;
	}

	byte*	p = buf_dblwr->write_buf
		+ univ_page_size.physical() * buf_dblwr->first_free;

	/* We request frame here to get correct buffer in case of
	encryption and/or page compression */
	void * frame = buf_page_get_frame(bpage);

	if (bpage->size.is_compressed()) {
		MEM_CHECK_DEFINED(bpage->zip.data, bpage->size.physical());
		/* Copy the compressed page and clear the rest. */

		memcpy(p, frame, bpage->size.physical());

		memset(p + bpage->size.physical(), 0x0,
		       univ_page_size.physical() - bpage->size.physical());
	} else {
		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
		MEM_CHECK_DEFINED(frame, bpage->size.logical());
		memcpy(p, frame, bpage->size.logical());
	}

	buf_dblwr->buf_block_arr[buf_dblwr->first_free] = bpage;

	buf_dblwr->first_free++;
	buf_dblwr->b_reserved++;

	ut_ad(!buf_dblwr->batch_running);
	ut_ad(buf_dblwr->first_free == buf_dblwr->b_reserved);
	ut_ad(buf_dblwr->b_reserved <= srv_doublewrite_batch_size);

	if (buf_dblwr->first_free == srv_doublewrite_batch_size) {
		mutex_exit(&(buf_dblwr->mutex));

		buf_dblwr_flush_buffered_writes();

		return;
	}

	mutex_exit(&(buf_dblwr->mutex));
}

/********************************************************************//**
Writes a page to the doublewrite buffer on disk, sync it, then write
the page to the datafile and sync the datafile. This function is used
for single page flushes. If all the buffers allocated for single page
flushes in the doublewrite buffer are in use we wait here for one to
become free. We are guaranteed that a slot will become free because any
thread that is using a slot must also release the slot before leaving
this function. */
void
buf_dblwr_write_single_page(
/*========================*/
	buf_page_t*	bpage,	/*!< in: buffer block to write */
	bool		sync)	/*!< in: true if sync IO requested */
{
	ulint		n_slots;
	ulint		size;
	ulint		offset;
	ulint		i;

	ut_a(buf_page_in_file(bpage));
	ut_a(srv_use_doublewrite_buf);
	ut_a(buf_dblwr != NULL);

	/* total number of slots available for single page flushes
	starts from srv_doublewrite_batch_size to the end of the
	buffer. */
	size = TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
	ut_a(size > srv_doublewrite_batch_size);
	n_slots = size - srv_doublewrite_batch_size;

	if (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE) {

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block((buf_block_t*) bpage);

		/* Check that the page as written to the doublewrite
		buffer has sane LSN values. */
		if (!bpage->zip.data) {
			buf_dblwr_check_page_lsn(
				((buf_block_t*) bpage)->frame);
		}
	}

retry:
	mutex_enter(&buf_dblwr->mutex);
	if (buf_dblwr->s_reserved == n_slots) {

		/* All slots are reserved. */
		int64_t	sig_count = os_event_reset(buf_dblwr->s_event);
		mutex_exit(&buf_dblwr->mutex);
		os_event_wait_low(buf_dblwr->s_event, sig_count);

		goto retry;
	}

	for (i = srv_doublewrite_batch_size; i < size; ++i) {

		if (!buf_dblwr->in_use[i]) {
			break;
		}
	}

	/* We are guaranteed to find a slot. */
	ut_a(i < size);
	buf_dblwr->in_use[i] = true;
	buf_dblwr->s_reserved++;
	buf_dblwr->buf_block_arr[i] = bpage;

	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.inc();
	srv_stats.dblwr_writes.inc();

	mutex_exit(&buf_dblwr->mutex);

	/* Lets see if we are going to write in the first or second
	block of the doublewrite buffer. */
	if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		offset = buf_dblwr->block1 + i;
	} else {
		offset = buf_dblwr->block2 + i
			 - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
	}

	/* We deal with compressed and uncompressed pages a little
	differently here. In case of uncompressed pages we can
	directly write the block to the allocated slot in the
	doublewrite buffer in the system tablespace and then after
	syncing the system table space we can proceed to write the page
	in the datafile.
	In case of compressed page we first do a memcpy of the block
	to the in-memory buffer of doublewrite before proceeding to
	write it. This is so because we want to pad the remaining
	bytes in the doublewrite page with zeros. */

	/* We request frame here to get correct buffer in case of
	encryption and/or page compression */
	void * frame = buf_page_get_frame(bpage);

	if (bpage->size.is_compressed()) {
		memcpy(buf_dblwr->write_buf + univ_page_size.physical() * i,
		       frame, bpage->size.physical());

		memset(buf_dblwr->write_buf + univ_page_size.physical() * i
		       + bpage->size.physical(), 0x0,
		       univ_page_size.physical() - bpage->size.physical());

		fil_io(IORequestWrite,
		       true,
		       page_id_t(TRX_SYS_SPACE, offset),
		       univ_page_size,
		       0,
		       univ_page_size.physical(),
		       (void *)(buf_dblwr->write_buf + univ_page_size.physical() * i),
		       NULL);
	} else {
		/* It is a regular page. Write it directly to the
		doublewrite buffer */
		fil_io(IORequestWrite,
		       true,
		       page_id_t(TRX_SYS_SPACE, offset),
		       univ_page_size,
		       0,
		       univ_page_size.physical(),
		       (void*) frame,
		       NULL);
	}

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(TRX_SYS_SPACE);

	/* We know that the write has been flushed to disk now
	and during recovery we will find it in the doublewrite buffer
	blocks. Next do the write to the intended position. */
	buf_dblwr_write_block_to_datafile(bpage, sync);
}
