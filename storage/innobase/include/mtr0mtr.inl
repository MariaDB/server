/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/mtr0mtr.ic
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#include "buf0buf.h"

/** Check if a mini-transaction is dirtying a clean page.
@return true if the mtr is dirtying a clean page. */
inline bool mtr_t::is_block_dirtied(const buf_block_t *block)
{
  ut_ad(block->page.in_file());
  ut_ad(block->page.frame);
  ut_ad(block->page.buf_fix_count());
  return block->page.oldest_modification() <= 1 &&
    block->page.id().space() < SRV_TMP_SPACE_ID;
}

/**
Pushes an object to an mtr memo stack. */
void
mtr_t::memo_push(void* object, mtr_memo_type_t type)
{
	ut_ad(is_active());
	ut_ad(object != NULL);
	ut_ad(type >= MTR_MEMO_PAGE_S_FIX);
	ut_ad(type <= MTR_MEMO_SPACE_S_LOCK);
	ut_ad(ut_is_2pow(type));

	/* If this mtr has x-fixed a clean page then we set
	the made_dirty flag. This tells mtr_t::commit()
	to hold log_sys.latch longer. */

	if (!m_made_dirty
            && (type == MTR_MEMO_PAGE_X_FIX || type == MTR_MEMO_PAGE_SX_FIX)) {

		m_made_dirty = is_block_dirtied(
			reinterpret_cast<const buf_block_t*>(object));
	}

	mtr_memo_slot_t* slot = m_memo.push<mtr_memo_slot_t*>(sizeof(*slot));

	slot->type = type;
	slot->object = object;
}

/**
Releases the (index tree) s-latch stored in an mtr memo after a
savepoint. */
void
mtr_t::release_s_latch_at_savepoint(
	ulint		savepoint,
	index_lock*	lock)
{
	ut_ad(is_active());
	ut_ad(m_memo.size() > savepoint);

	mtr_memo_slot_t* slot = m_memo.at<mtr_memo_slot_t*>(savepoint);

	ut_ad(slot->object == lock);
	ut_ad(slot->type == MTR_MEMO_S_LOCK);

	lock->s_unlock();

	slot->object = NULL;
}

/**
SX-latches the not yet latched block after a savepoint. */

void
mtr_t::sx_latch_at_savepoint(
	ulint		savepoint,
	buf_block_t*	block)
{
	ut_ad(is_active());
	ut_ad(m_memo.size() > savepoint);

	ut_ad(!memo_contains_flagged(
			block,
			MTR_MEMO_PAGE_S_FIX
			| MTR_MEMO_PAGE_X_FIX
			| MTR_MEMO_PAGE_SX_FIX));

	mtr_memo_slot_t* slot = m_memo.at<mtr_memo_slot_t*>(savepoint);

	ut_ad(slot->object == block);

	/* == RW_NO_LATCH */
	ut_a(slot->type == MTR_MEMO_BUF_FIX);

	block->page.lock.u_lock();
	ut_ad(!block->page.is_io_fixed());

	if (!m_made_dirty) {
		m_made_dirty = is_block_dirtied(block);
	}

	slot->type = MTR_MEMO_PAGE_SX_FIX;
}

/**
X-latches the not yet latched block after a savepoint. */

void
mtr_t::x_latch_at_savepoint(
	ulint		savepoint,
	buf_block_t*	block)
{
	ut_ad(is_active());
	ut_ad(m_memo.size() > savepoint);

	ut_ad(!memo_contains_flagged(
			block,
			MTR_MEMO_PAGE_S_FIX
			| MTR_MEMO_PAGE_X_FIX
			| MTR_MEMO_PAGE_SX_FIX));

	mtr_memo_slot_t* slot = m_memo.at<mtr_memo_slot_t*>(savepoint);

	ut_ad(slot->object == block);

	/* == RW_NO_LATCH */
	ut_a(slot->type == MTR_MEMO_BUF_FIX);

	block->page.lock.x_lock();
	ut_ad(!block->page.is_io_fixed());

	if (!m_made_dirty) {
		m_made_dirty = is_block_dirtied(block);
	}

	slot->type = MTR_MEMO_PAGE_X_FIX;
}

/**
Releases the block in an mtr memo after a savepoint. */

void
mtr_t::release_block_at_savepoint(
	ulint		savepoint,
	buf_block_t*	block)
{
  ut_ad(is_active());

  mtr_memo_slot_t *slot = m_memo.at<mtr_memo_slot_t*>(savepoint);

  ut_a(slot->object == block);
  slot->object= nullptr;
  block->page.unfix();

  switch (slot->type) {
  case MTR_MEMO_PAGE_S_FIX:
    block->page.lock.s_unlock();
    break;
  case MTR_MEMO_PAGE_SX_FIX:
  case MTR_MEMO_PAGE_X_FIX:
    block->page.lock.u_or_x_unlock(slot->type == MTR_MEMO_PAGE_SX_FIX);
    break;
  default:
    break;
  }
}
