/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2021, MariaDB Corporation.

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

/********************************************************************//**
@file include/btr0sea.ic
The index tree adaptive search

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#include "dict0mem.h"
#include "btr0cur.h"
#include "buf0buf.h"

#ifdef BTR_CUR_HASH_ADAPT
/** Updates the search info.
@param cursor   cursor which was just positioned */
void btr_search_info_update_slow(const btr_cur_t *cursor);

/*********************************************************************//**
Updates the search info. */
static inline
void
btr_search_info_update(
/*===================*/
	dict_index_t*	index,	/*!< in: index of the cursor */
	btr_cur_t*	cursor)	/*!< in: cursor which was just positioned */
{
	ut_ad(!index->is_spatial());
	ut_ad(!index->table->is_temporary());

	if (!btr_search.enabled) {
		return;
	}

	if (!index->search_info.hash_analysis_useful()) {
		return;
	}

	ut_ad(cursor->flag != BTR_CUR_HASH);

	btr_search_info_update_slow(cursor);
}

/** Lock all search latches in exclusive mode. */
static inline void btr_search_x_lock_all()
{
	btr_search.parts.latch.wr_lock(SRW_LOCK_CALL);
}

/** Unlock all search latches from exclusive mode. */
static inline void btr_search_x_unlock_all()
{
	btr_search.parts.latch.wr_unlock();
}

/** Lock all search latches in shared mode. */
static inline void btr_search_s_lock_all()
{
	btr_search.parts.latch.rd_lock(SRW_LOCK_CALL);
}

/** Unlock all search latches from shared mode. */
static inline void btr_search_s_unlock_all()
{
	btr_search.parts.latch.rd_unlock();
}
#endif /* BTR_CUR_HASH_ADAPT */
