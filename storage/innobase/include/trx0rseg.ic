/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/trx0rseg.ic
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "srv0srv.h"
#include "mtr0log.h"

/** Gets a rollback segment header.
@param[in]	space		space where placed
@param[in]	page_no		page number of the header
@param[in,out]	mtr		mini-transaction
@return rollback segment header, page x-latched */
UNIV_INLINE
buf_block_t*
trx_rsegf_get(fil_space_t* space, uint32_t page_no, mtr_t* mtr)
{
	ut_ad(space == fil_system.sys_space || space == fil_system.temp_space
	      || srv_is_undo_tablespace(space->id)
	      || !srv_was_started);

	return buf_page_get(page_id_t(space->id, page_no),
			    0, RW_X_LATCH, mtr);
}
