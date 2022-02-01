/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2021, MariaDB Corporation.

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
@file include/buf0flu.ic
The database buffer pool flush algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "assume_aligned.h"
#include "buf0buf.h"
#include "srv0srv.h"

/********************************************************************//**
This function should be called at a mini-transaction commit, if a page was
modified in it. Puts the block to the list of modified blocks, if it is not
already in it. */
UNIV_INLINE
void
buf_flush_note_modification(
/*========================*/
	buf_block_t*	block,		/*!< in: block which is modified */
	lsn_t		start_lsn,	/*!< in: start lsn of the mtr that
					modified this block */
	lsn_t		end_lsn)	/*!< in: end lsn of the mtr that
					modified this block */
{
	ut_ad(!srv_read_only_mode);
	ut_ad(block->page.state() == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.buf_fix_count());
	ut_ad(mach_read_from_8(block->frame + FIL_PAGE_LSN) <= end_lsn);
	mach_write_to_8(block->frame + FIL_PAGE_LSN, end_lsn);
	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		memcpy_aligned<8>(FIL_PAGE_LSN + block->page.zip.data,
				  FIL_PAGE_LSN + block->frame, 8);
	}

	const lsn_t oldest_modification = block->page.oldest_modification();

	if (oldest_modification > 1) {
		ut_ad(oldest_modification <= start_lsn);
	} else if (fsp_is_system_temporary(block->page.id().space())) {
		block->page.set_temp_modified();
	} else {
		buf_pool.insert_into_flush_list(block, start_lsn);
	}

	srv_stats.buf_pool_write_requests.inc();
}
