/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

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

#ifndef buf0rea_h
#define buf0rea_h

#include "buf0buf.h"

/** Read a page synchronously from a file. buf_page_t::read_complete()
will be invoked on read completion.
@param page_id   page id
@param unzip     whether to decompress ROW_FORMAT=COMPRESSED pages
@retval DB_SUCCESS if the page was read and is not corrupted
@retval DB_SUCCESS_LOCKED_REC if the page was not read
@retval DB_PAGE_CORRUPTED if page based on checksum check is corrupted
@retval DB_DECRYPTION_FAILED if page post encryption checksum matches but
after decryption normal page checksum does not match.
@retval DB_TABLESPACE_DELETED if tablespace .ibd file is missing */
dberr_t buf_read_page(const page_id_t page_id, bool unzip= true) noexcept;

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

/** Apply a random read-ahead of pages.
@param space     tablespace
@param ibuf      whether we are inside ibuf routine
@param low       first page to attempt to read
@param high      last page to attempt to read */
void buf_read_ahead_random(fil_space_t *space, bool ibuf,
                           page_id_t low, page_id_t high) noexcept;

/** Apply linear read-ahead if an undo log page is a border page of
a linear read-ahead area and all the pages in the area have been accessed.
Does not read any page if the read-ahead mechanism is not activated.
@param space     undo tablespace or fil_system.space, or nullptr
@param id        undo page identifier
@return number of page read requests issued */
ulint buf_read_ahead_undo(fil_space_t *space, const page_id_t id) noexcept;

/** Read ahead a number of pages.
@param space    tablespace
@param pages    pages to read ahead
@param ibuf     whether we are inside the ibuf routine */
void buf_read_ahead_pages(fil_space_t *space,
                          st_::span<const uint32_t> pages, bool ibuf) noexcept;

/** Read ahead a page if it is not yet in the buffer pool.
@param space    tablespace
@param page     page to read ahead
@param ibuf     whether we are inside the ibuf routine */
void buf_read_ahead_one(fil_space_t *space, uint32_t page, bool ibuf) noexcept;

/** Schedule a page for recovery.
@param space    tablespace
@param page_id  page identifier
@param recs     log records
@param init     page initialization, or nullptr if the page needs to be read */
void buf_read_recover(fil_space_t *space, const page_id_t page_id,
                      page_recv_t &recs, recv_init *init) noexcept;

/** @name Modes used in read-ahead @{ */
/** read only pages belonging to the insert buffer tree */
#define BUF_READ_IBUF_PAGES_ONLY	131
/** read any page */
#define BUF_READ_ANY_PAGE		132
/* @} */

#endif
