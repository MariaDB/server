/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2020, MariaDB Corporation.

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
@file include/ha0ha.h
The hash table interface for the adaptive hash index

Created 8/18/1994 Heikki Tuuri
*******************************************************/

#ifndef ha0ha_h
#define ha0ha_h

#include "hash0hash.h"
#include "page0types.h"
#include "buf0types.h"
#include "rem0types.h"

#ifdef BTR_CUR_HASH_ADAPT
/*************************************************************//**
Looks for an element in a hash table.
@return pointer to the data of the first hash table node in chain
having the fold number, NULL if not found */
UNIV_INLINE
const rec_t*
ha_search_and_get_data(
/*===================*/
	hash_table_t*	table,	/*!< in: hash table */
	ulint		fold);	/*!< in: folded value of the searched data */

/** The hash table external chain node */
struct ha_node_t {
	ulint		fold;	/*!< fold value for the data */
	ha_node_t*	next;	/*!< next chain node or NULL if none */
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	buf_block_t*	block;	/*!< buffer block containing the data, or NULL */
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	const rec_t*	data;	/*!< pointer to the data */
};

#include "ha0ha.inl"
#endif /* BTR_CUR_HASH_ADAPT */

#endif
