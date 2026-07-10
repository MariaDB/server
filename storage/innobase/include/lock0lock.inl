/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/lock0lock.ic
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#include "dict0dict.h"
#include "buf0buf.h"
#include "page0page.h"

/*********************************************************************//**
Gets the heap_no of the smallest user record on a page.
@return heap_no of smallest user record, or PAGE_HEAP_NO_SUPREMUM */
UNIV_INLINE
ulint
lock_get_min_heap_no(
/*=================*/
	const buf_block_t*	block)	/*!< in: buffer block */
{
	const page_t*	page	= block->page.frame;

	if (page_is_comp(page)) {
		return(rec_get_heap_no_new(
			       page
			       + rec_get_next_offs(page + PAGE_NEW_INFIMUM,
						   TRUE)));
	} else {
		return(rec_get_heap_no_old(
			       page
			       + rec_get_next_offs(page + PAGE_OLD_INFIMUM,
						   FALSE)));
	}
}
