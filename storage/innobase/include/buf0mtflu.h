/*****************************************************************************

Copyright (C) 2014 SkySQL Ab. All Rights Reserved.
Copyright (C) 2014 Fusion-io. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/buf0mtflu.h
Multi-threadef flush method interface function prototypes

Created 06/02/2014 Jan Lindström jan.lindstrom@skysql.com
		   Dhananjoy Das DDas@fusionio.com
***********************************************************************/

#ifndef buf0mtflu_h
#define buf0mtflu_h

/******************************************************************//**
Add exit work item to work queue to signal multi-threded flush
threads that they should exit.
*/
void
buf_mtflu_io_thread_exit(void);
/*===========================*/

/******************************************************************//**
Initialize multi-threaded flush thread syncronization data.
@return Initialized multi-threaded flush thread syncroniztion data. */
void*
buf_mtflu_handler_init(
/*===================*/
	ulint n_threads,	/*!< in: Number of threads to create */
	ulint wrk_cnt);		/*!< in: Number of work items */

/******************************************************************//**
Return true if multi-threaded flush is initialized
@return true if initialized, false if not */
bool
buf_mtflu_init_done(void);
/*======================*/

/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
UNIV_INTERN
ulint
buf_mtflu_flush_LRU_tail(void);
/*===========================*/

/*******************************************************************//**
Multi-threaded version of buf_flush_list
*/
bool
buf_mtflu_flush_list(
/*=================*/
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	lsn_t		lsn_limit,	/*!< in the case BUF_FLUSH_LIST all
					blocks whose oldest_modification is
					smaller than this should be flushed
					(if their number does not exceed
					min_n), otherwise ignored */
	ulint*		n_processed);	/*!< out: the number of pages
					which were processed is passed
					back to caller. Ignored if NULL */

/*********************************************************************//**
Set correct thread identifiers to io thread array based on
information we have. */
void
buf_mtflu_set_thread_ids(
/*=====================*/
	ulint n_threads,		/*!<in: Number of threads to fill */
	void* ctx,		        /*!<in: thread context */
	os_thread_id_t* thread_ids);	/*!<in: thread id array */

#endif
