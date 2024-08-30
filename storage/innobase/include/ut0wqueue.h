/*****************************************************************************

Copyright (c) 2006, 2014, Oracle and/or its affiliates. All Rights Reserved.
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

/*******************************************************************//**
@file include/ut0wqueue.h
A work queue

Created 4/26/2006 Osku Salerma
************************************************************************/

/*******************************************************************//**
A Work queue. Threads can add work items to the queue and other threads can
wait for work items to be available and take them off the queue for
processing.
************************************************************************/

#pragma once

#include "ut0list.h"
#include "mem0mem.h"

// Forward declaration
struct ib_list_t;

/** Work queue */
struct ib_wqueue_t
{
  /** Mutex protecting everything */
  mysql_mutex_t mutex;
  /** Work item list */
  ib_list_t *items;
  /** ib_list_len(*items) */
  size_t length;
};

/****************************************************************//**
Create a new work queue.
@return work queue */
ib_wqueue_t*
ib_wqueue_create();
/*===============*/

/****************************************************************//**
Free a work queue. */
void
ib_wqueue_free(
/*===========*/
	ib_wqueue_t*	wq);		/*!< in: work queue */

/** Add a work item to the queue.
@param[in,out]	wq		work queue
@param[in]	item		work item
@param[in,out]	heap		memory heap to use for allocating list node
@param[in]	wq_locked	work queue mutex locked */
void
ib_wqueue_add(ib_wqueue_t* wq, void* item, mem_heap_t* heap,
	      bool wq_locked = false);

/** Check if queue is empty.
@param wq wait queue
@return whether the queue is empty */
bool ib_wqueue_is_empty(ib_wqueue_t* wq);

/********************************************************************
Return first item on work queue or NULL if queue is empty
@return work item or NULL */
void*
ib_wqueue_nowait(
/*=============*/
	ib_wqueue_t*	wq);		/*<! in: work queue */
