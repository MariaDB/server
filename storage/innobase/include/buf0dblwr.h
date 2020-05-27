/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file include/buf0dblwr.h
Doublewrite buffer module

Created 2011/12/19 Inaam Rana
*******************************************************/

#ifndef buf0dblwr_h
#define buf0dblwr_h

#include "ut0byte.h"
#include "log0log.h"
#include "buf0types.h"
#include "log0recv.h"

/** Doublewrite system */
extern buf_dblwr_t*	buf_dblwr;
/** Set to TRUE when the doublewrite buffer is being created */
extern ibool		buf_dblwr_being_created;

/** Create the doublewrite buffer if the doublewrite buffer header
is not present in the TRX_SYS page.
@return	whether the operation succeeded
@retval	true	if the doublewrite buffer exists or was created
@retval	false	if the creation failed (too small first data file) */
MY_ATTRIBUTE((warn_unused_result))
bool
buf_dblwr_create();

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
	const char*	path);

/** Process and remove the double write buffer pages for all tablespaces. */
void
buf_dblwr_process();

/****************************************************************//**
frees doublewrite buffer. */
void
buf_dblwr_free();

/** Update the doublewrite buffer on write completion. */
void buf_dblwr_update(const buf_page_t &bpage, bool single_page);
/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no);	/*!< in: page number */

/********************************************************************//**
Flush a batch of writes to the datafiles that have already been
written to the dblwr buffer on disk. */
void
buf_dblwr_sync_datafiles();

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk.
It is very important to call this function after a batch of writes
has been posted, and also when we may have to wait for a page latch!
Otherwise a deadlock of threads can occur. */
void
buf_dblwr_flush_buffered_writes();

/** Doublewrite control struct */
struct buf_dblwr_t{
	ib_mutex_t	mutex;	/*!< mutex protecting the first_free
				field and write_buf */
	ulint		block1;	/*!< the page number of the first
				doublewrite block (64 pages) */
	ulint		block2;	/*!< page number of the second block */
	ulint		first_free;/*!< first free position in write_buf
				measured in units of srv_page_size */
	ulint		b_reserved;/*!< number of slots currently reserved
				for batch flush. */
	os_event_t	b_event;/*!< event where threads wait for a
				batch flush to end;
				os_event_set() and os_event_reset()
				are protected by buf_dblwr_t::mutex */
	ulint		s_reserved;/*!< number of slots currently
				reserved for single page flushes. */
	os_event_t	s_event;/*!< event where threads wait for a
				single page flush slot. Protected by mutex. */
	bool		batch_running;/*!< set to TRUE if currently a batch
				is being written from the doublewrite
				buffer. */
	byte*		write_buf;/*!< write buffer used in writing to the
				doublewrite buffer, aligned to an
				address divisible by srv_page_size
				(which is required by Windows aio) */

  struct element
  {
    /** block descriptor */
    buf_page_t *bpage;
    /** flush type */
    IORequest::flush_t flush;
    /** payload size in bytes */
    size_t size;
  };

  /** buffer blocks to be written via write_buf */
  element *buf_block_arr;

  /** Schedule a page write. If the doublewrite memory buffer is full,
  buf_dblwr_flush_buffered_writes() will be invoked to make space.
  @param bpage     buffer pool page to be written
  @param flush     type of flush
  @param size      payload size in bytes */
  void add_to_batch(buf_page_t *bpage, IORequest::flush_t flush, size_t size);
  /** Write a page to the doublewrite buffer on disk, sync it, then write
  the page to the datafile and sync the datafile. This function is used
  for single page flushes. If all the buffers allocated for single page
  flushes in the doublewrite buffer are in use we wait here for one to
  become free. We are guaranteed that a slot will become free because any
  thread that is using a slot must also release the slot before leaving
  this function.
  @param bpage   buffer pool page to be written
  @param sync    whether synchronous operation is requested
  @param size    payload size in bytes */
  void write_single_page(buf_page_t *bpage, bool sync, size_t size);
};

#endif
