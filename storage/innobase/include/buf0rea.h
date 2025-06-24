/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
@file include/buf0rea.h
The database buffer read

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "buf0buf.h"

/** Read a page synchronously from a file. buf_page_t::read_complete()
will be invoked on read completion.
@param page_id   page identifier
@param chain     buf_pool.page_hash cell for page_id
@param unzip     whether to decompress ROW_FORMAT=COMPRESSED pages
@retval DB_SUCCESS if the page was read and is not corrupted
@retval DB_SUCCESS_LOCKED_REC if the page was not read
@retval DB_PAGE_CORRUPTED if page based on checksum check is corrupted,
@retval DB_DECRYPTION_FAILED if page post encryption checksum matches but
after decryption normal page checksum does not match.
@retval DB_TABLESPACE_DELETED if tablespace .ibd file is missing */
dberr_t buf_read_page(const page_id_t page_id,
                      buf_pool_t::hash_chain &chain, bool unzip= true)
  noexcept;

/** High-level function which reads a page asynchronously from a file to the
buffer buf_pool if it is not already there. Sets the io_fix flag and sets
an exclusive lock on the buffer frame. The flag is cleared and the x-lock
released by the i/o-handler thread.
@param[in,out]	space		tablespace
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0 */
void buf_read_page_background(fil_space_t *space, const page_id_t page_id,
                              ulint zip_size) noexcept
  MY_ATTRIBUTE((nonnull));

/** Applies a random read-ahead in buf_pool if there are at least a threshold
value of accessed pages from the random read-ahead area. Does not read any
page, not even the one at the position (space, offset), if the read-ahead
mechanism is not activated. NOTE: the calling thread may own latches on
pages: to avoid deadlocks this function must be written such that it cannot
end up waiting for these latches!
@param[in]	page_id		page id of a page which the current thread
wants to access
@return number of page read requests issued */
ulint buf_read_ahead_random(const page_id_t page_id) noexcept;

/** Applies linear read-ahead if in the buf_pool the page is a border page of
a linear read-ahead area and all the pages in the area have been accessed.
Does not read any page if the read-ahead mechanism is not activated. Note
that the algorithm looks at the 'natural' adjacent successor and
predecessor of the page, which on the leaf level of a B-tree are the next
and previous page in the chain of leaves. To know these, the page specified
in (space, offset) must already be present in the buf_pool. Thus, the
natural way to use this function is to call it when a page in the buf_pool
is accessed the first time, calling this function just after it has been
bufferfixed.
NOTE 1: as this function looks at the natural predecessor and successor
fields on the page, what happens, if these are not initialized to any
sensible value? No problem, before applying read-ahead we check that the
area to read is within the span of the space, if not, read-ahead is not
applied. An uninitialized value may result in a useless read operation, but
only very improbably.
NOTE 2: the calling thread may own latches on pages: to avoid deadlocks this
function must be written such that it cannot end up waiting for these
latches!
@param[in]	page_id		page id; see NOTE 3 above
@return number of page read requests issued */
ulint buf_read_ahead_linear(const page_id_t page_id) noexcept;

/** Schedule a page for recovery.
@param space    tablespace
@param page_id  page identifier
@param recs     log records
@param init_lsn page initialization, or 0 if the page needs to be read */
void buf_read_recover(fil_space_t *space, const page_id_t page_id,
                      page_recv_t &recs, lsn_t init_lsn) noexcept;
