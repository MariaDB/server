/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file include/row0purge.h
Purge obsolete records

Created 3/14/1997 Heikki Tuuri
*******************************************************/

#ifndef row0purge_h
#define row0purge_h

#include "que0types.h"
#include "btr0types.h"
#include "btr0pcur.h"
#include "trx0types.h"
#include "row0types.h"
#include "ut0vec.h"
#include "row0mysql.h"

/** Determines if it is possible to remove a secondary index entry.
Removal is possible if the secondary index entry does not refer to any
not delete marked version of a clustered index record where DB_TRX_ID
is newer than the purge view.

NOTE: This function should only be called by the purge thread, only
while holding a latch on the leaf page of the secondary index entry
(or keeping the buffer pool watch on the page).  It is possible that
this function first returns true and then false, if a user transaction
inserts a record that the secondary index entry would refer to.
However, in that case, the user transaction would also re-insert the
secondary index entry after purge has removed it and released the leaf
page latch.
@param[in,out]	node		row purge node
@param[in]	index		secondary index
@param[in]	entry		secondary index entry
@param[in,out]	sec_pcur	secondary index cursor or NULL
				if it is called for purge buffering
				operation.
@param[in,out]	sec_mtr		mini-transaction which holds
				secondary index entry or NULL if it is
				called for purge buffering operation.
@param[in]	is_tree		true=pessimistic purge,
				false=optimistic (leaf-page only)
@return true if the secondary index record can be purged */
bool
row_purge_poss_sec(
	purge_node_t*	node,
	dict_index_t*	index,
	const dtuple_t*	entry,
	btr_pcur_t*	sec_pcur=NULL,
	mtr_t*		sec_mtr=NULL,
	bool		is_tree=false);

/***************************************************************
Does the purge operation for a single undo log record. This is a high-level
function used in an SQL execution graph.
@return query thread to run next or NULL */
que_thr_t*
row_purge_step(
/*===========*/
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/* Purge node structure */

struct purge_node_t{
	que_common_t	common;	/*!< node type: QUE_NODE_PURGE */
	/*----------------------*/
	/* Local storage for this graph node */
	roll_ptr_t	roll_ptr;/* roll pointer to undo log record */
	ib_vector_t*    undo_recs;/*!< Undo recs to purge */

	undo_no_t	undo_no;/*!< undo number of the record */

	ulint		rec_type;/*!< undo log record type: TRX_UNDO_INSERT_REC,
				... */
private:
	/** latest unavailable table ID (do not bother looking up again) */
	table_id_t	unavailable_table_id;
	/** the latest modification of the table definition identified by
	unavailable_table_id, or TRX_ID_MAX */
	trx_id_t	def_trx_id;
public:
	dict_table_t*	table;	/*!< table where purge is done */

	ulint		cmpl_info;/* compiler analysis info of an update */

	upd_t*		update;	/*!< update vector for a clustered index
				record */
	dtuple_t*	ref;	/*!< NULL, or row reference to the next row to
				handle */
	dtuple_t*	row;	/*!< NULL, or a copy (also fields copied to
				heap) of the indexed fields of the row to
				handle */
	dict_index_t*	index;	/*!< NULL, or the next index whose record should
				be handled */
	mem_heap_t*	heap;	/*!< memory heap used as auxiliary storage for
				row; this must be emptied after a successful
				purge of a row */
	ibool		found_clust;/*!< whether the clustered index record
				determined by ref was found in the clustered
				index, and we were able to position pcur on
				it */
	btr_pcur_t	pcur;	/*!< persistent cursor used in searching the
				clustered index record */
#ifdef UNIV_DEBUG
	/** whether the operation is in progress */
	bool		in_progress;
#endif
	trx_id_t	trx_id;	/*!< trx id for this purging record */

	/** Virtual column information about opening of MariaDB table.
	It resets after processing each undo log record. */
	purge_vcol_info_t	vcol_info;

	/** Constructor */
	explicit purge_node_t(que_thr_t* parent) :
		common(QUE_NODE_PURGE, parent),
		undo_recs(NULL),
		unavailable_table_id(0),
		heap(mem_heap_create(256)),
#ifdef UNIV_DEBUG
		in_progress(false),
#endif
		vcol_info()
	{}

#ifdef UNIV_DEBUG
	/***********************************************************//**
	Validate the persisent cursor. The purge node has two references
	to the clustered index record - one via the ref member, and the
	other via the persistent cursor.  These two references must match
	each other if the found_clust flag is set.
	@return true if the persistent cursor is consistent with
	the ref member.*/
	bool validate_pcur();
#endif

	/** Whether purge failed to open the maria table for virtual column
	computation.
	@return true if the table failed to open. */
	bool vcol_op_failed() const { return !vcol_info.validate(); }

	/** Determine if a table should be skipped in purge.
	@param[in]	table_id	table identifier
	@return	whether to skip the table lookup and processing */
	bool is_skipped(table_id_t id) const
	{
		return id == unavailable_table_id && trx_id <= def_trx_id;
	}

	/** Remember that a table should be skipped in purge.
	@param[in]	id	table identifier
	@param[in]	limit	last transaction for which to skip */
	void skip(table_id_t id, trx_id_t limit)
	{
		DBUG_ASSERT(limit >= trx_id || !srv_safe_truncate);
		unavailable_table_id = id;
		def_trx_id = limit;
	}

	/** Start processing an undo log record. */
	void start()
	{
		ut_ad(in_progress);
		DBUG_ASSERT(common.type == QUE_NODE_PURGE);

		table = NULL;
		row = NULL;
		ref = NULL;
		index = NULL;
		update = NULL;
		found_clust = FALSE;
		rec_type = ULINT_UNDEFINED;
		cmpl_info = ULINT_UNDEFINED;
	}

	/** Reset the state at end
	@return the query graph parent */
	que_node_t* end()
	{
		DBUG_ASSERT(common.type == QUE_NODE_PURGE);
		undo_recs = NULL;
		ut_d(in_progress = false);
		vcol_info.reset();
		mem_heap_empty(heap);
		return common.parent;
	}
};

#endif
