/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2017, MariaDB Corporation.

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
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "buf0dblwr.h"

#ifdef UNIV_NONINL
#include "buf0buf.ic"
#endif

#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"

#ifndef UNIV_HOTBACKUP

#ifdef UNIV_PFS_MUTEX
/* Key to register the mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	buf_dblwr_mutex_key;
#endif /* UNIV_PFS_RWLOCK */

/** The doublewrite buffer */
UNIV_INTERN buf_dblwr_t*	buf_dblwr = NULL;

/** Set to TRUE when the doublewrite buffer is being created */
UNIV_INTERN ibool	buf_dblwr_being_created = FALSE;

#define TRX_SYS_DOUBLEWRITE_BLOCKS 2

/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
UNIV_INTERN
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
@return	pointer to the doublewrite buffer within the filespace header
page. */
UNIV_INLINE
byte*
buf_dblwr_get(
/*==========*/
	mtr_t*	mtr)	/*!< in/out: MTR to hold the page latch */
{
	buf_block_t*	block;

	block = buf_page_get(TRX_SYS_SPACE, 0, TRX_SYS_PAGE_NO,
			     RW_X_LATCH, mtr);
	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	return(buf_block_get_frame(block) + TRX_SYS_DOUBLEWRITE);
}

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written to the dblwr buffer on disk. */
UNIV_INLINE
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

	/* Now we flush the data to disk (for example, with fsync) */
	fil_flush_file_spaces(FIL_TABLESPACE);
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
		mem_zalloc(sizeof(buf_dblwr_t)));

	/* There are two blocks of same size in the doublewrite
	buffer. */
	buf_size = TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	/* There must be atleast one buffer for single page writes
	and one buffer for batch writes. */
	ut_a(srv_doublewrite_batch_size > 0
	     && srv_doublewrite_batch_size < buf_size);

	mutex_create(buf_dblwr_mutex_key,
		     &buf_dblwr->mutex, SYNC_DOUBLEWRITE);

	buf_dblwr->b_event = os_event_create();
	buf_dblwr->s_event = os_event_create();
	buf_dblwr->first_free = 0;
	buf_dblwr->s_reserved = 0;
	buf_dblwr->b_reserved = 0;

	buf_dblwr->block1 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK1);
	buf_dblwr->block2 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK2);

	buf_dblwr->in_use = static_cast<bool*>(
		mem_zalloc(buf_size * sizeof(bool)));

	buf_dblwr->write_buf_unaligned = static_cast<byte*>(
		ut_malloc((1 + buf_size) * UNIV_PAGE_SIZE));

	buf_dblwr->write_buf = static_cast<byte*>(
		ut_align(buf_dblwr->write_buf_unaligned,
			 UNIV_PAGE_SIZE));

	buf_dblwr->buf_block_arr = static_cast<buf_page_t**>(
		mem_zalloc(buf_size * sizeof(void*)));
}

/** Create the doublewrite buffer if the doublewrite buffer header
is not present in the TRX_SYS page.
@return	whether the operation succeeded
@retval	true	if the doublewrite buffer exists or was created
@retval	false	if the creation failed (too small first data file) */
UNIV_INTERN
bool
buf_dblwr_create()
{
	buf_block_t*	block2;
	buf_block_t*	new_block;
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
	mtr_start(&mtr);
	buf_dblwr_being_created = TRUE;

	doublewrite = buf_dblwr_get(&mtr);

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has already been created:
		just read in some numbers */

		buf_dblwr_init(doublewrite);

		mtr_commit(&mtr);
		buf_dblwr_being_created = FALSE;
		return(true);
	}

	if (buf_pool_get_curr_size()
	    < ((TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		+ FSP_EXTENT_SIZE / 2 + 100)
	       * UNIV_PAGE_SIZE)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: "
			"innodb_buffer_pool_size is too small.");
		mtr_commit(&mtr);
		return(false);
	} else {
		fil_space_t* space = fil_space_acquire(TRX_SYS_SPACE);
		const bool fail = UT_LIST_GET_FIRST(space->chain)->size
			< 3 * FSP_EXTENT_SIZE;
		fil_space_release(space);

		if (fail) {
			goto too_small;
		}
	}

	block2 = fseg_create(TRX_SYS_SPACE, TRX_SYS_PAGE_NO,
			     TRX_SYS_DOUBLEWRITE
			     + TRX_SYS_DOUBLEWRITE_FSEG, &mtr);

	if (block2 == NULL) {
too_small:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create doublewrite buffer: "
			"the first file in innodb_data_file_path"
			" must be at least %luM.",
			3 * (FSP_EXTENT_SIZE * UNIV_PAGE_SIZE) >> 20);
		mtr_commit(&mtr);
		return(false);
	}

	ib_logf(IB_LOG_LEVEL_INFO,
		"Doublewrite buffer not found: creating new");

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
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Cannot create doublewrite buffer: you must "
				"increase your tablespace size. "
				"Cannot continue operation.");
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
		page_no = buf_block_get_page_no(new_block);

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
	log_make_checkpoint_at(LSN_MAX, TRUE);

	/* Remove doublewrite pages from LRU */
	buf_pool_invalidate();

	ib_logf(IB_LOG_LEVEL_INFO, "Doublewrite buffer created");

	goto start_again;
}

/****************************************************************//**
At a database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory. */
void
buf_dblwr_init_or_load_pages(
/*=========================*/
	pfs_os_file_t	file,
	char*		path,
	bool		load_corrupt_pages)
{
	byte*	buf;
	byte*	read_buf;
	byte*	unaligned_read_buf;
	ulint	block1;
	ulint	block2;
	byte*	page;
	ibool	reset_space_ids = FALSE;
	byte*	doublewrite;
	ulint	space_id;
	ulint	i;
        ulint	block_bytes = 0;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;

	/* We do the file i/o past the buffer pool */

	unaligned_read_buf = static_cast<byte*>(ut_malloc(3 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));

	/* Read the trx sys header to check if we are using the doublewrite
	buffer */
	off_t  trx_sys_page = TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE;
	os_file_read(file, read_buf, trx_sys_page, UNIV_PAGE_SIZE);

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
		goto leave_func;
	}

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED)
	    != TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N) {

		/* We are upgrading from a version < 4.1.x to a version where
		multiple tablespaces are supported. We must reset the space id
		field in the pages in the doublewrite buffer because starting
		from this version the space id is stored to
		FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */

		reset_space_ids = TRUE;

		ib_logf(IB_LOG_LEVEL_INFO,
			"Resetting space id's in the doublewrite buffer");
	}

	/* Read the pages from the doublewrite buffer to memory */

        block_bytes = TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

	os_file_read(file, buf, block1 * UNIV_PAGE_SIZE, block_bytes);
	os_file_read(file, buf + block_bytes, block2 * UNIV_PAGE_SIZE,
		     block_bytes);

	/* Check if any of these pages is half-written in data files, in the
	intended position */

	page = buf;

	for (i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * TRX_SYS_DOUBLEWRITE_BLOCKS; i++) {

		ulint source_page_no;

		if (reset_space_ids) {

			space_id = 0;
			mach_write_to_4(page
					+ FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);
			/* We do not need to calculate new checksums for the
			pages because the field .._SPACE_ID does not affect
			them. Write the page back to where we read it from. */

			if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
				source_page_no = block1 + i;
			} else {
				source_page_no = block2
					+ i - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			}

			os_file_write(path, file, page,
				      source_page_no * UNIV_PAGE_SIZE,
				      UNIV_PAGE_SIZE);
		} else if (load_corrupt_pages
			   && !buf_page_is_zeroes(page, FIL_PAGE_DATA)) {
			/* Each valid page header must contain some
			nonzero bytes, such as FIL_PAGE_OFFSET
			or FIL_PAGE_LSN. */
			recv_dblwr.add(page);
		}

		page += UNIV_PAGE_SIZE;
	}

	if (reset_space_ids) {
		os_file_flush(file);
	}

leave_func:
	ut_free(unaligned_read_buf);
}

/****************************************************************//**
Process the double write buffer pages. */
void
buf_dblwr_process()
/*===============*/
{
	ulint	space_id;
	ulint	page_no;
	ulint	page_no_dblwr = 0;
	byte*	page;
	byte*	read_buf;
	byte*	unaligned_read_buf;
	recv_dblwr_t& recv_dblwr = recv_sys->dblwr;

	if (!buf_dblwr) {
		return;
	}

	ib_logf(IB_LOG_LEVEL_INFO,
		"Restoring possible half-written data pages "
		"from the doublewrite buffer...");

	unaligned_read_buf = static_cast<byte*>(ut_malloc(3 * UNIV_PAGE_SIZE));

	read_buf = static_cast<byte*>(
		ut_align(unaligned_read_buf, UNIV_PAGE_SIZE));
	byte* const buf = read_buf + UNIV_PAGE_SIZE;

	for (std::list<byte*>::iterator i = recv_dblwr.pages.begin();
	     i != recv_dblwr.pages.end(); ++i, ++page_no_dblwr ) {
		page = *i;
		page_no  = mach_read_from_4(page + FIL_PAGE_OFFSET);
		space_id = mach_read_from_4(page + FIL_PAGE_SPACE_ID);

		FilSpace space(space_id, true);

		if (!space()) {
			/* Maybe we have dropped the single-table tablespace
			and this page once belonged to it: do nothing */
			continue;
		}

		if (!fil_check_adress_in_tablespace(space_id, page_no)) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"A copy of page " ULINTPF ":" ULINTPF
				" in the doublewrite buffer slot " ULINTPF
				" is not within space bounds",
				space_id, page_no, page_no_dblwr);
			continue;
		}

		ulint	zip_size = fsp_flags_get_zip_size(space()->flags);
		ut_ad(!buf_page_is_zeroes(page, zip_size));

		/* Read in the actual page from the file */
		fil_io(OS_FILE_READ,
		       true,
		       space_id,
		       zip_size,
		       page_no,
		       0,
		       zip_size ? zip_size : UNIV_PAGE_SIZE,
		       read_buf,
		       NULL,
		       0);

		const bool is_all_zero = buf_page_is_zeroes(
			read_buf, zip_size);

		if (is_all_zero) {
			/* We will check if the copy in the
			doublewrite buffer is valid. If not, we will
			ignore this page (there should be redo log
			records to initialize it). */
		} else {
			/* Decompress the page before
			validating the checksum. */
			ulint decomp = fil_page_decompress(buf, read_buf);
			if (!decomp) {
				goto bad;
			}
			if (!decomp || (decomp != srv_page_size && zip_size)) {
				goto bad;
			}

			if (fil_space_verify_crypt_checksum(
				    read_buf, zip_size, NULL, page_no)
			    || !buf_page_is_corrupted(
				    true, read_buf, zip_size, space())) {
				/* The page is good; there is no need
				to consult the doublewrite buffer. */
				continue;
			}

bad:
			/* We intentionally skip this message for
			is_all_zero pages. */
			ib_logf(IB_LOG_LEVEL_INFO,
				"Trying to recover page " ULINTPF ":" ULINTPF
				" from the doublewrite buffer.",
				space_id, page_no);
		}

		ulint decomp = fil_page_decompress(buf, page);
		if (!decomp || (decomp != srv_page_size && zip_size)) {
			goto bad_doublewrite;
		}
		if (!fil_space_verify_crypt_checksum(page, zip_size, NULL,
						     page_no)
		    && buf_page_is_corrupted(true, page, zip_size, space)) {
			if (!is_all_zero) {
bad_doublewrite:
				ib_logf(IB_LOG_LEVEL_WARN,
					"A doublewrite copy of page "
					ULINTPF ":" ULINTPF " is corrupted.",
					space_id, page_no);
			}
			/* Theoretically we could have another good
			copy for this page in the doublewrite
			buffer. If not, we will report a fatal error
			for a corrupted page somewhere else if that
			page was truly needed. */
			continue;
		}

		if (page_no == 0) {
			/* Check the FSP_SPACE_FLAGS. */
			ulint flags = fsp_header_get_flags(page);
			if (!fsp_flags_is_valid(flags)
			    && fsp_flags_convert_from_101(flags)
			    == ULINT_UNDEFINED) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Ignoring a doublewrite copy of page "
					ULINTPF ":0 due to invalid flags 0x%x",
					space_id, int(flags));
				continue;
			}
			/* The flags on the page should be converted later. */
		}

		/* Write the good page from the doublewrite buffer to
		the intended position. */

		fil_io(OS_FILE_WRITE, true, space_id, zip_size, page_no, 0,
		       zip_size ? zip_size : UNIV_PAGE_SIZE,
		       page, NULL, 0);

		ib_logf(IB_LOG_LEVEL_INFO,
			"Recovered page " ULINTPF ":" ULINTPF " from"
			" the doublewrite buffer.",
			space_id, page_no);
	}

	ut_free(unaligned_read_buf);
	fil_flush_file_spaces(FIL_TABLESPACE);

        {
		size_t bytes = TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;
		byte *unaligned_buf = static_cast<byte*>(
			ut_malloc(bytes + UNIV_PAGE_SIZE - 1));

		byte *buf = static_cast<byte*>(
			ut_align(unaligned_buf, UNIV_PAGE_SIZE));
		memset(buf, 0, bytes);

		fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
			buf_dblwr->block1, 0, bytes, buf, NULL, NULL);
		fil_io(OS_FILE_WRITE, true, TRX_SYS_SPACE, 0,
			buf_dblwr->block2, 0, bytes, buf, NULL, NULL);

		ut_free(unaligned_buf);
        }
}

/****************************************************************//**
Frees doublewrite buffer. */
UNIV_INTERN
void
buf_dblwr_free(void)
/*================*/
{
	/* Free the double write data structures. */
	ut_a(buf_dblwr != NULL);
	ut_ad(buf_dblwr->s_reserved == 0);
	ut_ad(buf_dblwr->b_reserved == 0);

	os_event_free(buf_dblwr->b_event);
	os_event_free(buf_dblwr->s_event);
	ut_free(buf_dblwr->write_buf_unaligned);
	buf_dblwr->write_buf_unaligned = NULL;

	mem_free(buf_dblwr->buf_block_arr);
	buf_dblwr->buf_block_arr = NULL;

	mem_free(buf_dblwr->in_use);
	buf_dblwr->in_use = NULL;

	mutex_free(&buf_dblwr->mutex);
	mem_free(buf_dblwr);
	buf_dblwr = NULL;
}

/********************************************************************//**
Updates the doublewrite buffer when an IO request is completed. */
UNIV_INTERN
void
buf_dblwr_update(
/*=============*/
	const buf_page_t*	bpage,	/*!< in: buffer block descriptor */
	buf_flush_t		flush_type)/*!< in: flush type */
{
	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		return;
	}

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
			fil_flush_file_spaces(FIL_TABLESPACE);
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

		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: ERROR: The page to be written"
			" seems corrupt!\n"
			"InnoDB: The low 4 bytes of LSN fields do not match "
			"(" ULINTPF " != " ULINTPF ")!"
			" Noticed in the buffer pool.\n",
			mach_read_from_4(
				page + FIL_PAGE_LSN + 4),
			mach_read_from_4(
				page + UNIV_PAGE_SIZE
				- FIL_PAGE_END_LSN_OLD_CHKSUM + 4));
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
	buf_page_print(block->frame, 0);

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Apparent corruption of an"
		" index page n:o %lu in space %lu\n"
		"InnoDB: to be written to data file."
		" We intentionally crash server\n"
		"InnoDB: to prevent corrupt data"
		" from ending up in data\n"
		"InnoDB: files.\n",
		(ulong) buf_block_get_page_no(block),
		(ulong) buf_block_get_space(block));

	ut_error;
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
	if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE
	    || block->page.zip.data) {
		/* No simple validate for compressed pages exists. */
		return;
	}

	buf_dblwr_check_page_lsn(block->frame);

	if (!block->check_index_page_at_flush) {
		return;
	}

	if (page_is_comp(block->frame)) {
		if (!page_simple_validate_new(block->frame)) {
			buf_dblwr_assert_on_corrupt_block(block);
		}
	} else if (!page_simple_validate_old(block->frame)) {

		buf_dblwr_assert_on_corrupt_block(block);
	}
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
	ut_a(bpage);
	ut_a(buf_page_in_file(bpage));

	const ulint flags = sync
		? OS_FILE_WRITE
		: OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER;

	void * frame = buf_page_get_frame(bpage);

	if (bpage->zip.data) {
		fil_io(flags,
			sync,
			buf_page_get_space(bpage),
			buf_page_get_zip_size(bpage),
			buf_page_get_page_no(bpage),
			0,
			buf_page_get_zip_size(bpage),
			frame,
			(void*) bpage,
			0);

		return;
	}


	const buf_block_t* block = (buf_block_t*) bpage;
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	buf_dblwr_check_page_lsn(block->frame);

	fil_io(flags,
		sync,
		buf_block_get_space(block),
		0,
		buf_block_get_page_no(block),
		0,
		bpage->real_size,
		frame,
		(void*) block,
		(ulint *)&bpage->write_size);
}

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk,
and also wakes up the aio thread if simulated aio is used. It is very
important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
UNIV_INTERN
void
buf_dblwr_flush_buffered_writes(void)
/*=================================*/
{
	byte*		write_buf;
	ulint		first_free;
	ulint		len;

	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		/* Sync the writes to the disk. */
		buf_dblwr_sync_datafiles();
		return;
	}

try_again:
	mutex_enter(&buf_dblwr->mutex);

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

	if (buf_dblwr->first_free == 0) {

		mutex_exit(&buf_dblwr->mutex);

		return;
	}

	if (buf_dblwr->batch_running) {
		/* Another thread is running the batch right now. Wait
		for it to finish. */
		ib_int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	ut_a(!buf_dblwr->batch_running);
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

	fil_io(OS_FILE_WRITE,
		true,
		TRX_SYS_SPACE,
		0,
		buf_dblwr->block1,
		0,
		len,
		(void*)
		write_buf,
		NULL,
		0);

	if (buf_dblwr->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		/* No unwritten pages in the second block. */
		goto flush;
	}

	/* Write out the second block of the doublewrite buffer. */
	len = (buf_dblwr->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
	       * UNIV_PAGE_SIZE;

	write_buf = buf_dblwr->write_buf
		    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;

	fil_io(OS_FILE_WRITE,
		true,
		TRX_SYS_SPACE,
		0,
		buf_dblwr->block2,
		0,
		len,
		(void*) write_buf,
		NULL,
		0);

flush:
	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.add(buf_dblwr->first_free);
	srv_stats.dblwr_writes.inc();

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(ulint(TRX_SYS_SPACE));

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
UNIV_INTERN
void
buf_dblwr_add_to_batch(
/*====================*/
	buf_page_t*	bpage)	/*!< in: buffer block to write */
{
	ulint	zip_size;

	ut_a(buf_page_in_file(bpage));
	ut_ad(!mutex_own(&buf_pool_from_bpage(bpage)->LRU_list_mutex));

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
		ib_int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	if (buf_dblwr->first_free == srv_doublewrite_batch_size) {
		mutex_exit(&(buf_dblwr->mutex));

		buf_dblwr_flush_buffered_writes();

		goto try_again;
	}

	zip_size = buf_page_get_zip_size(bpage);
	void * frame = buf_page_get_frame(bpage);

	if (zip_size) {
		UNIV_MEM_ASSERT_RW(bpage->zip.data, zip_size);
		/* Copy the compressed page and clear the rest. */
		memcpy(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free,
                       frame, zip_size);
		memset(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free
		       + zip_size, 0, UNIV_PAGE_SIZE - zip_size);
	} else {
		ut_a(buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
		UNIV_MEM_ASSERT_RW(((buf_block_t*) bpage)->frame,
				   UNIV_PAGE_SIZE);

		memcpy(buf_dblwr->write_buf
		       + UNIV_PAGE_SIZE * buf_dblwr->first_free,
		       frame, UNIV_PAGE_SIZE);
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
UNIV_INTERN
void
buf_dblwr_write_single_page(
/*========================*/
	buf_page_t*	bpage,	/*!< in: buffer block to write */
	bool		sync)	/*!< in: true if sync IO requested */
{
	ulint		n_slots;
	ulint		size;
	ulint		zip_size;
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
		ib_int64_t	sig_count =
			os_event_reset(buf_dblwr->s_event);
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

	zip_size = buf_page_get_zip_size(bpage);
	void * frame = buf_page_get_frame(bpage);

	if (zip_size) {
		memcpy(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i,
		       frame, zip_size);
		memset(buf_dblwr->write_buf + UNIV_PAGE_SIZE * i
		       + zip_size, 0, UNIV_PAGE_SIZE - zip_size);

		fil_io(OS_FILE_WRITE,
			true,
			TRX_SYS_SPACE,
			0,
			offset,
			0,
			UNIV_PAGE_SIZE,
			(void*) (buf_dblwr->write_buf + UNIV_PAGE_SIZE * i),
			NULL,
			0);
	} else {
		/* It is a regular page. Write it directly to the
		doublewrite buffer */
		fil_io(OS_FILE_WRITE,
			true,
			TRX_SYS_SPACE,
			0,
			offset,
			0,
			bpage->real_size,
			frame,
			NULL,
			0);
	}

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(ulint(TRX_SYS_SPACE));

	/* We know that the write has been flushed to disk now
	and during recovery we will find it in the doublewrite buffer
	blocks. Next do the write to the intended position. */
	buf_dblwr_write_block_to_datafile(bpage, sync);
}
#endif /* !UNIV_HOTBACKUP */
