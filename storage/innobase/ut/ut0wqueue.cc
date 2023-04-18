/*****************************************************************************

Copyright (c) 2006, 2015, Oracle and/or its affiliates. All Rights Reserved.
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

#include "ut0list.h"
#include "mem0mem.h"
#include "ut0wqueue.h"

/*******************************************************************//**
@file ut/ut0wqueue.cc
A work queue

Created 4/26/2006 Osku Salerma
************************************************************************/

/****************************************************************//**
Create a new work queue.
@return work queue */
ib_wqueue_t*
ib_wqueue_create(void)
/*===================*/
{
	ib_wqueue_t*	wq = static_cast<ib_wqueue_t*>(
		ut_malloc_nokey(sizeof(*wq)));

	mysql_mutex_init(0, &wq->mutex, nullptr);

	wq->items = ib_list_create();
	wq->length = 0;

	return(wq);
}

/****************************************************************//**
Free a work queue. */
void
ib_wqueue_free(
/*===========*/
	ib_wqueue_t*	wq)	/*!< in: work queue */
{
	mysql_mutex_destroy(&wq->mutex);
	ib_list_free(wq->items);

	ut_free(wq);
}

/** Add a work item to the queue.
@param[in,out]	wq		work queue
@param[in]	item		work item
@param[in,out]	heap		memory heap to use for allocating list node
@param[in]	wq_locked	work queue mutex locked */
void
ib_wqueue_add(ib_wqueue_t* wq, void* item, mem_heap_t* heap, bool wq_locked)
{
	if (!wq_locked) {
		mysql_mutex_lock(&wq->mutex);
	}

	ib_list_add_last(wq->items, item, heap);
	wq->length++;
	ut_ad(wq->length == ib_list_len(wq->items));

	if (!wq_locked) {
		mysql_mutex_unlock(&wq->mutex);
	}
}

/********************************************************************
Return first item on work queue or NULL if queue is empty
@return work item or NULL */
void*
ib_wqueue_nowait(
/*=============*/
	ib_wqueue_t*	wq)		/*<! in: work queue */
{
	ib_list_node_t*	node = NULL;

	mysql_mutex_lock(&wq->mutex);

	if(!ib_list_is_empty(wq->items)) {
		node = ib_list_get_first(wq->items);

		if (node) {
			ib_list_remove(wq->items, node);
			--wq->length;
			ut_ad(wq->length == ib_list_len(wq->items));
		}
	}

	mysql_mutex_unlock(&wq->mutex);

	return (node ? node->data : NULL);
}
/** Check if queue is empty.
@param wq wait queue
@return whether the queue is empty */
bool ib_wqueue_is_empty(ib_wqueue_t* wq)
{
	mysql_mutex_lock(&wq->mutex);
	bool is_empty = ib_list_is_empty(wq->items);
	mysql_mutex_unlock(&wq->mutex);
	return is_empty;
}
