/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/row0vers.h
Row versions

Created 2/6/1997 Heikki Tuuri
*******************************************************/

#ifndef row0vers_h
#define row0vers_h

#include "data0data.h"
#include "trx0types.h"
#include "que0types.h"
#include "rem0types.h"
#include "mtr0mtr.h"
#include "dict0mem.h"
#include "row0types.h"

// Forward declaration
class ReadView;

/** Determine if an active transaction has inserted or modified a secondary
index record.
@param[in,out]	caller_trx	trx of current thread
@param[in]	rec	secondary index record
@param[in]	index	secondary index
@param[in]	offsets	rec_get_offsets(rec, index)
@return	the active transaction; state must be rechecked after
acquiring trx->mutex, and trx->release_reference() must be invoked
@retval	NULL if the record was committed */
trx_t*
row_vers_impl_x_locked(
	trx_t*		caller_trx,
	const rec_t*	rec,
	dict_index_t*	index,
	const rec_offs*	offsets);

/** Find out whether data tuple has missing data type
for indexed virtual column.
@param tuple   data tuple
@param index   virtual index
@return true if tuple has missing column type */
bool dtuple_vcol_data_missing(const dtuple_t &tuple,
                              const dict_index_t &index);
/** build virtual column value from current cluster index record data
@param[in,out]	row		the cluster index row in dtuple form
@param[in]	clust_index	clustered index
@param[in]	index		the secondary index
@param[in]	heap		heap used to build virtual dtuple. */
bool
row_vers_build_clust_v_col(
	dtuple_t*		row,
	dict_index_t*		clust_index,
	dict_index_t*		index,
	mem_heap_t*		heap);
/** Build a dtuple contains virtual column data for current cluster index
@param[in]	rec		cluster index rec
@param[in]	clust_index	cluster index
@param[in]	clust_offsets	cluster rec offset
@param[in]	index		secondary index
@param[in]	trx_id		transaction ID on the purging record,
				or 0 if called outside purge
@param[in]	roll_ptr	roll_ptr for the purge record
@param[in,out]	heap		heap memory
@param[in,out]	v_heap		heap memory to keep virtual column tuple
@param[in,out]	mtr		mini-transaction
@return dtuple contains virtual column data */
dtuple_t*
row_vers_build_cur_vrow(
	const rec_t*		rec,
	dict_index_t*		clust_index,
	rec_offs**		clust_offsets,
	dict_index_t*		index,
	trx_id_t		trx_id,
	roll_ptr_t		roll_ptr,
	mem_heap_t*		heap,
	mem_heap_t*		v_heap,
	mtr_t*			mtr);

/*****************************************************************//**
Constructs the version of a clustered index record which a consistent
read should see. We assume that the trx id stored in rec is such that
the consistent read should not see rec in its present version.
@return error code
@retval DB_SUCCESS if a previous version was fetched
@retval DB_MISSING_HISTORY if the history is missing (a sign of corruption) */
dberr_t
row_vers_build_for_consistent_read(
/*===============================*/
	const rec_t*	rec,	/*!< in: record in a clustered index; the
				caller must have a latch on the page; this
				latch locks the top of the stack of versions
				of this records */
	mtr_t*		mtr,	/*!< in: mtr holding the latch on rec; it will
				also hold the latch on purge_view */
	dict_index_t*	index,	/*!< in: the clustered index */
	rec_offs**	offsets,/*!< in/out: offsets returned by
				rec_get_offsets(rec, index) */
	ReadView*	view,	/*!< in: the consistent read view */
	mem_heap_t**	offset_heap,/*!< in/out: memory heap from which
				the offsets are allocated */
	mem_heap_t*	in_heap,/*!< in: memory heap from which the memory for
				*old_vers is allocated; memory for possible
				intermediate versions is allocated and freed
				locally within the function */
	rec_t**		old_vers,/*!< out, own: old version, or NULL
				if the history is missing or the record
				does not exist in the view, that is,
				it was freshly inserted afterwards */
	dtuple_t**	vrow);	/*!< out: reports virtual column info if any */

/*****************************************************************//**
Constructs the last committed version of a clustered index record,
which should be seen by a semi-consistent read. */
void
row_vers_build_for_semi_consistent_read(
/*====================================*/
	trx_t*		caller_trx,/*!<in/out: trx of current thread */
	const rec_t*	rec,	/*!< in: record in a clustered index; the
				caller must have a latch on the page; this
				latch locks the top of the stack of versions
				of this records */
	mtr_t*		mtr,	/*!< in: mtr holding the latch on rec */
	dict_index_t*	index,	/*!< in: the clustered index */
	rec_offs**	offsets,/*!< in/out: offsets returned by
				rec_get_offsets(rec, index) */
	mem_heap_t**	offset_heap,/*!< in/out: memory heap from which
				the offsets are allocated */
	mem_heap_t*	in_heap,/*!< in: memory heap from which the memory for
				*old_vers is allocated; memory for possible
				intermediate versions is allocated and freed
				locally within the function */
	const rec_t**	old_vers,/*!< out: rec, old version, or NULL if the
				record does not exist in the view, that is,
				it was freshly inserted afterwards */
	dtuple_t**	vrow);	/*!< out: holds virtual column info if any
				is updated in the view */

#endif
