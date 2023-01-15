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

	/* Function ib_wqueue_create() has not been used anywhere,
	not necessary to instrument this mutex */

	mutex_create(LATCH_ID_WORK_QUEUE, &wq->mutex);

	wq->items = ib_list_create();
	wq->event = os_event_create(0);
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
	mutex_free(&wq->mutex);
	ib_list_free(wq->items);
	os_event_destroy(wq->event);

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
		mutex_enter(&wq->mutex);
	}

	ib_list_add_last(wq->items, item, heap);
	wq->length++;
	ut_ad(wq->length == ib_list_len(wq->items));
	os_event_set(wq->event);

	if (!wq_locked) {
		mutex_exit(&wq->mutex);
	}
}

/****************************************************************//**
Wait for a work item to appear in the queue.
@return work item */
void*
ib_wqueue_wait(
/*===========*/
	ib_wqueue_t*	wq)	/*!< in: work queue */
{
	ib_list_node_t*	node;

	for (;;) {
		os_event_wait(wq->event);

		mutex_enter(&wq->mutex);

		node = ib_list_get_first(wq->items);

		if (node) {
			ib_list_remove(wq->items, node);
			if (!--wq->length) {
				/* We must reset the event when the list
				gets emptied. */
				os_event_reset(wq->event);
			}
			ut_ad(wq->length == ib_list_len(wq->items));

			break;
		}

		mutex_exit(&wq->mutex);
	}

	mutex_exit(&wq->mutex);

	return(node->data);
}


/********************************************************************
Wait for a work item to appear in the queue for specified time. */
void*
ib_wqueue_timedwait(
/*================*/
					/* out: work item or NULL on timeout*/
	ib_wqueue_t*	wq,		/* in: work queue */
	ulint		wait_in_usecs)	/* in: wait time in micro seconds */
{
	ib_list_node_t*	node = NULL;

	for (;;) {
		ulint		error;
		int64_t		sig_count;

		mutex_enter(&wq->mutex);

		node = ib_list_get_first(wq->items);

		if (node) {
			ib_list_remove(wq->items, node);
			wq->length--;
			ut_ad(wq->length == ib_list_len(wq->items));
			mutex_exit(&wq->mutex);
			break;
		}

		sig_count = os_event_reset(wq->event);

		mutex_exit(&wq->mutex);

		error = os_event_wait_time_low(wq->event,
					       (ulint) wait_in_usecs,
					       sig_count);

		if (error == OS_SYNC_TIME_EXCEEDED) {
			break;
		}
	}

	return(node ? node->data : NULL);
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

	mutex_enter(&wq->mutex);

	if(!ib_list_is_empty(wq->items)) {
		node = ib_list_get_first(wq->items);

		if (node) {
			ib_list_remove(wq->items, node);

		}
	}

	/* We must reset the event when the list
	gets emptied. */
	if(ib_list_is_empty(wq->items)) {
		os_event_reset(wq->event);
	}

	mutex_exit(&wq->mutex);

	return (node ? node->data : NULL);
}
/** Check if queue is empty.
@param wq wait queue
@return whether the queue is empty */
bool ib_wqueue_is_empty(ib_wqueue_t* wq)
{
	mutex_enter(&wq->mutex);
	bool is_empty = ib_list_is_empty(wq->items);
	mutex_exit(&wq->mutex);
	return is_empty;
}
