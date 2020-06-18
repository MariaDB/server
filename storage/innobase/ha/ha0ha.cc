/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file ha/ha0ha.cc
The hash table with external chains

Created 8/22/1994 Heikki Tuuri
*************************************************************************/

#include "ha0ha.h"

#ifdef UNIV_DEBUG
# include "buf0buf.h"
#endif /* UNIV_DEBUG */
#include "btr0sea.h"
#include "page0page.h"

#ifdef BTR_CUR_HASH_ADAPT
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
/** Maximum number of records in a page */
static const ulint MAX_N_POINTERS
	= UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES;
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

/*************************************************************//**
Inserts an entry into a hash table. If an entry with the same fold number
is found, its node is updated to point to the new data, and no new node
is inserted. If btr_search_enabled is set to FALSE, we will only allow
updating existing nodes, but no new node is allowed to be added.
@return TRUE if succeed, FALSE if no more memory could be allocated */
ibool
ha_insert_for_fold_func(
/*====================*/
	hash_table_t*	table,	/*!< in: hash table */
	ulint		fold,	/*!< in: folded value of data; if a node with
				the same fold value already exists, it is
				updated to point to the same data, and no new
				node is created! */
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	buf_block_t*	block,	/*!< in: buffer block containing the data */
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	const rec_t*		data)	/*!< in: data, must not be NULL */
{
	hash_cell_t*	cell;
	ha_node_t*	node;
	ha_node_t*	prev_node;
	ulint		hash;

	ut_ad(data);
	ut_ad(table);
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
	ut_ad(table->heap->type & MEM_HEAP_BTR_SEARCH);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(block->frame == page_align(data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	ut_ad(btr_search_enabled);

	hash = hash_calc_hash(fold, table);

	cell = hash_get_nth_cell(table, hash);

	prev_node = static_cast<ha_node_t*>(cell->node);

	while (prev_node != NULL) {
		if (prev_node->fold == fold) {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
			if (table->adaptive) {
				buf_block_t* prev_block = prev_node->block;
				ut_a(prev_block->frame
				     == page_align(prev_node->data));
				ut_a(prev_block->n_pointers-- < MAX_N_POINTERS);
				ut_a(block->n_pointers++ < MAX_N_POINTERS);
			}

			prev_node->block = block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
			prev_node->data = data;

			return(TRUE);
		}

		prev_node = prev_node->next;
	}

	/* We have to allocate a new chain node */
	node = static_cast<ha_node_t*>(
		mem_heap_alloc(table->heap, sizeof(ha_node_t)));

	if (node == NULL) {
		/* It was a btr search type memory heap and at the moment
		no more memory could be allocated: return */
		return(FALSE);
	}

	ha_node_set_data(node, block, data);

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	if (table->adaptive) {
		ut_a(block->n_pointers++ < MAX_N_POINTERS);
	}
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

	node->fold = fold;

	node->next = NULL;

	prev_node = static_cast<ha_node_t*>(cell->node);

	if (prev_node == NULL) {

		cell->node = node;

		return(TRUE);
	}

	while (prev_node->next != NULL) {

		prev_node = prev_node->next;
	}

	prev_node->next = node;

	return(TRUE);
}

#ifdef UNIV_DEBUG
/** Verify if latch corresponding to the hash table is x-latched
@param table   hash table */
void ha_btr_search_latch_x_locked(const hash_table_t* table)
{
	ulint	i;
	for (i = 0; i < btr_ahi_parts; ++i) {
		if (btr_search_sys->hash_tables[i] == table) {
			break;
		}
	}

	ut_ad(i < btr_ahi_parts);
	ut_ad(rw_lock_own(btr_search_latches[i], RW_LOCK_X));
}
#endif /* UNIV_DEBUG */

/***********************************************************//**
Deletes a hash node. */
void
ha_delete_hash_node(
/*================*/
	hash_table_t*	table,		/*!< in: hash table */
	ha_node_t*	del_node)	/*!< in: node to be deleted */
{
	ut_ad(table);
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
	ut_d(ha_btr_search_latch_x_locked(table));
	ut_ad(btr_search_enabled);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(table->adaptive);
	ut_a(del_node->block->frame == page_align(del_node->data));
	ut_a(del_node->block->n_pointers-- < MAX_N_POINTERS);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

	ha_node_t* node;

	const ulint fold = del_node->fold;

	HASH_DELETE(ha_node_t, next, table, fold, del_node);

	ha_node_t* top_node = (ha_node_t*) mem_heap_get_top(table->heap,
							    sizeof(ha_node_t));

	/* If the node to remove is not the top node in the heap, compact the
	heap of nodes by moving the top node in the place of del_node. */

	if (del_node != top_node) {
		/* Copy the top node in place of del_node */

		*del_node = *top_node;

		hash_cell_t* cell = hash_get_nth_cell(
			table, hash_calc_hash(top_node->fold, table));

		/* Look for the pointer to the top node, to update it */

		if (cell->node == top_node) {
			/* The top node is the first in the chain */
			cell->node = del_node;
		} else {
			/* We have to look for the predecessor */
			node = static_cast<ha_node_t*>(cell->node);

			while (top_node != HASH_GET_NEXT(next, node)) {
				node = static_cast<ha_node_t*>(
					HASH_GET_NEXT(next, node));
			}

			/* Now we have the predecessor node */
			node->next = del_node;
		}
	}

	/* Free the space occupied by the top node */

	mem_heap_free_top(table->heap, sizeof(ha_node_t));
}

/*********************************************************//**
Looks for an element when we know the pointer to the data, and updates
the pointer to data, if found.
@return TRUE if found */
ibool
ha_search_and_update_if_found_func(
/*===============================*/
	hash_table_t*	table,	/*!< in/out: hash table */
	ulint		fold,	/*!< in: folded value of the searched data */
	const rec_t*	data,	/*!< in: pointer to the data */
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	buf_block_t*	new_block,/*!< in: block containing new_data */
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	const rec_t*	new_data)/*!< in: new pointer to the data */
{
	ha_node_t*	node;

	ut_ad(table);
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	ut_a(new_block->frame == page_align(new_data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

	ut_d(ha_btr_search_latch_x_locked(table));

	if (!btr_search_enabled) {
		return(FALSE);
	}

	node = ha_search_with_data(table, fold, data);

	if (node) {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
		if (table->adaptive) {
			ut_a(node->block->n_pointers-- < MAX_N_POINTERS);
			ut_a(new_block->n_pointers++ < MAX_N_POINTERS);
		}

		node->block = new_block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
		node->data = new_data;

		return(TRUE);
	}

	return(FALSE);
}

/*****************************************************************//**
Removes from the chain determined by fold all nodes whose data pointer
points to the page given. */
void
ha_remove_all_nodes_to_page(
/*========================*/
	hash_table_t*	table,	/*!< in: hash table */
	ulint		fold,	/*!< in: fold value */
	const page_t*	page)	/*!< in: buffer page */
{
	ha_node_t*	node;

	ut_ad(table);
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
	ut_ad(btr_search_enabled);
	ut_d(ha_btr_search_latch_x_locked(table));

	node = ha_chain_get_first(table, fold);

	while (node) {
		if (page_align(ha_node_get_data(node)) == page) {

			/* Remove the hash node */

			ha_delete_hash_node(table, node);

			/* Start again from the first node in the chain
			because the deletion may compact the heap of
			nodes and move other nodes! */

			node = ha_chain_get_first(table, fold);
		} else {
			node = ha_chain_get_next(node);
		}
	}
#ifdef UNIV_DEBUG
	/* Check that all nodes really got deleted */

	node = ha_chain_get_first(table, fold);

	while (node) {
		ut_a(page_align(ha_node_get_data(node)) != page);

		node = ha_chain_get_next(node);
	}
#endif /* UNIV_DEBUG */
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
/*************************************************************//**
Validates a given range of the cells in hash table.
@return TRUE if ok */
ibool
ha_validate(
/*========*/
	hash_table_t*	table,		/*!< in: hash table */
	ulint		start_index,	/*!< in: start index */
	ulint		end_index)	/*!< in: end index */
{
	ibool		ok	= TRUE;
	ulint		i;

	ut_ad(table);
	ut_ad(table->magic_n == HASH_TABLE_MAGIC_N);
	ut_a(start_index <= end_index);
	ut_a(start_index < hash_get_n_cells(table));
	ut_a(end_index < hash_get_n_cells(table));

	for (i = start_index; i <= end_index; i++) {
		ha_node_t*	node;
		hash_cell_t*	cell;

		cell = hash_get_nth_cell(table, i);

		for (node = static_cast<ha_node_t*>(cell->node);
		     node != 0;
		     node = node->next) {

			if (hash_calc_hash(node->fold, table) != i) {
				ib::error() << "Hash table node fold value "
					<< node->fold << " does not match the"
					" cell number " << i << ".";

				ok = FALSE;
			}
		}
	}

	return(ok);
}
#endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
