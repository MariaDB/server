/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.

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

#include "buf0buf.h"
#include "mtr0mtr.h"
#include "srv0srv.h"
#include "fsp0types.h"

/********************************************************************//**
Inserts a modified block into the flush list. */
void
buf_flush_insert_into_flush_list(
/*=============================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_block_t*	block,		/*!< in/out: block which is modified */
	lsn_t		lsn);		/*!< in: oldest modification */

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
	lsn_t		end_lsn,	/*!< in: end lsn of the mtr that
					modified this block */
	FlushObserver*	observer)	/*!< in: flush observer */
{
	mutex_enter(&block->mutex);
	ut_ad(!srv_read_only_mode
	      || fsp_is_system_temporary(block->page.id.space()));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->page.buf_fix_count > 0);
	ut_ad(block->page.newest_modification <= end_lsn);
	block->page.newest_modification = end_lsn;

	/* Don't allow to set flush observer from non-null to null,
	or from one observer to another. */
	ut_ad(block->page.flush_observer == NULL
	      || block->page.flush_observer == observer);
	block->page.flush_observer = observer;

	if (block->page.oldest_modification == 0) {
		buf_pool_t*	buf_pool = buf_pool_from_block(block);

		buf_flush_insert_into_flush_list(buf_pool, block, start_lsn);
	} else {
		ut_ad(block->page.oldest_modification <= start_lsn);
	}

	mutex_exit(&block->mutex);

	srv_stats.buf_pool_write_requests.inc();
}
