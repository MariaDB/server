/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file include/dict0mem.ic
Data dictionary memory object creation

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include "data0type.h"
#include "dict0mem.h"
#include "fil0fil.h"

/**********************************************************************//**
This function poplulates a dict_index_t index memory structure with
supplied information. */
UNIV_INLINE
void
dict_mem_fill_index_struct(
/*=======================*/
	dict_index_t*	index,		/*!< out: index to be filled */
	mem_heap_t*	heap,		/*!< in: memory heap */
	const char*	index_name,	/*!< in: index name */
	ulint		type,		/*!< in: DICT_UNIQUE,
					DICT_CLUSTERED, ... ORed */
	ulint		n_fields)	/*!< in: number of fields */
{

	if (heap) {
		index->heap = heap;
		index->name = mem_heap_strdup(heap, index_name);
		index->fields = (dict_field_t*) mem_heap_alloc(
			heap, 1 + n_fields * sizeof(dict_field_t));
	} else {
		index->name = index_name;
		index->heap = NULL;
		index->fields = NULL;
	}

	index->type = type & ((1U << DICT_IT_BITS) - 1);
	index->page = FIL_NULL;
	index->merge_threshold = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	index->n_fields = static_cast<unsigned>(n_fields)
		& index->MAX_N_FIELDS;
	index->n_core_fields = static_cast<unsigned>(n_fields)
		& index->MAX_N_FIELDS;
	/* The '1 +' above prevents allocation
	of an empty mem block */
	index->nulls_equal = false;
	ut_d(index->magic_n = DICT_INDEX_MAGIC_N);
}
