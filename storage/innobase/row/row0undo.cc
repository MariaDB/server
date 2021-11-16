/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file row/row0undo.cc
Row undo

Created 1/8/1997 Heikki Tuuri
*******************************************************/

#include "row0undo.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "row0uins.h"
#include "row0umod.h"
#include "row0upd.h"
#include "row0mysql.h"
#include "srv0srv.h"
#include "srv0start.h"

/* How to undo row operations?
(1) For an insert, we have stored a prefix of the clustered index record
in the undo log. Using it, we look for the clustered record, and using
that we look for the records in the secondary indexes. The insert operation
may have been left incomplete, if the database crashed, for example.
We may have look at the trx id and roll ptr to make sure the record in the
clustered index is really the one for which the undo log record was
written. We can use the framework we get from the original insert op.
(2) Delete marking: We can use the framework we get from the original
delete mark op. We only have to check the trx id.
(3) Update: This may be the most complicated. We have to use the framework
we get from the original update op.

What if the same trx repeatedly deletes and inserts an identical row.
Then the row id changes and also roll ptr. What if the row id was not
part of the ordering fields in the clustered index? Maybe we have to write
it to undo log. Well, maybe not, because if we order the row id and trx id
in descending order, then the only undeleted copy is the first in the
index. Our searches in row operations always position the cursor before
the first record in the result set. But, if there is no key defined for
a table, then it would be desirable that row id is in ascending order.
So, lets store row id in descending order only if it is not an ordering
field in the clustered index.

NOTE: Deletes and inserts may lead to situation where there are identical
records in a secondary index. Is that a problem in the B-tree? Yes.
Also updates can lead to this, unless trx id and roll ptr are included in
ord fields.
(1) Fix in clustered indexes: include row id, trx id, and roll ptr
in node pointers of B-tree.
(2) Fix in secondary indexes: include all fields in node pointers, and
if an entry is inserted, check if it is equal to the right neighbor,
in which case update the right neighbor: the neighbor must be delete
marked, set it unmarked and write the trx id of the current transaction.

What if the same trx repeatedly updates the same row, updating a secondary
index field or not? Updating a clustered index ordering field?

(1) If it does not update the secondary index and not the clustered index
ord field. Then the secondary index record stays unchanged, but the
trx id in the secondary index record may be smaller than in the clustered
index record. This is no problem?
(2) If it updates secondary index ord field but not clustered: then in
secondary index there are delete marked records, which differ in an
ord field. No problem.
(3) Updates clustered ord field but not secondary, and secondary index
is unique. Then the record in secondary index is just updated at the
clustered ord field.
(4)

Problem with duplicate records:
Fix 1: Add a trx op no field to all indexes. A problem: if a trx with a
bigger trx id has inserted and delete marked a similar row, our trx inserts
again a similar row, and a trx with an even bigger id delete marks it. Then
the position of the row should change in the index if the trx id affects
the alphabetical ordering.

Fix 2: If an insert encounters a similar row marked deleted, we turn the
insert into an 'update' of the row marked deleted. Then we must write undo
info on the update. A problem: what if a purge operation tries to remove
the delete marked row?

We can think of the database row versions as a linked list which starts
from the record in the clustered index, and is linked by roll ptrs
through undo logs. The secondary index records are references which tell
what kinds of records can be found in this linked list for a record
in the clustered index.

How to do the purge? A record can be removed from the clustered index
if its linked list becomes empty, i.e., the row has been marked deleted
and its roll ptr points to the record in the undo log we are going through,
doing the purge. Similarly, during a rollback, a record can be removed
if the stored roll ptr in the undo log points to a trx already (being) purged,
or if the roll ptr is NULL, i.e., it was a fresh insert. */

/********************************************************************//**
Creates a row undo node to a query graph.
@return own: undo node */
undo_node_t*
row_undo_node_create(
/*=================*/
	trx_t*		trx,	/*!< in/out: transaction */
	que_thr_t*	parent,	/*!< in: parent node, i.e., a thr node */
	mem_heap_t*	heap)	/*!< in: memory heap where created */
{
	undo_node_t*	undo;

	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE)
	      || trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED)
	      || trx_state_eq(trx, TRX_STATE_PREPARED));
	ut_ad(parent);

	undo = static_cast<undo_node_t*>(
		mem_heap_alloc(heap, sizeof(undo_node_t)));

	undo->common.type = QUE_NODE_UNDO;
	undo->common.parent = parent;

	undo->state = UNDO_NODE_FETCH_NEXT;
	undo->trx = trx;

	btr_pcur_init(&(undo->pcur));

	undo->heap = mem_heap_create(256);

	return(undo);
}

/***********************************************************//**
Looks for the clustered index record when node has the row reference.
The pcur in node is used in the search. If found, stores the row to node,
and stores the position of pcur, and detaches it. The pcur must be closed
by the caller in any case.
@return true if found; NOTE the node->pcur must be closed by the
caller, regardless of the return value */
bool
row_undo_search_clust_to_pcur(
/*==========================*/
	undo_node_t*	node)	/*!< in/out: row undo node */
{
	dict_index_t*	clust_index;
	bool		found;
	mtr_t		mtr;
	row_ext_t**	ext;
	const rec_t*	rec;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(!node->table->skip_alter_undo);

	mtr_start(&mtr);

	clust_index = dict_table_get_first_index(node->table);

	found = row_search_on_row_ref(&node->pcur, BTR_MODIFY_LEAF,
				      node->table, node->ref, &mtr);

	if (!found) {
		goto func_exit;
	}

	rec = btr_pcur_get_rec(&node->pcur);

	offsets = rec_get_offsets(rec, clust_index, offsets,
				  clust_index->n_core_fields,
				  ULINT_UNDEFINED, &heap);

	found = row_get_rec_roll_ptr(rec, clust_index, offsets)
		== node->roll_ptr;

	if (found) {
		ut_ad(row_get_rec_trx_id(rec, clust_index, offsets)
		      == node->trx->id || node->table->is_temporary());

		if (dict_table_has_atomic_blobs(node->table)) {
			/* There is no prefix of externally stored
			columns in the clustered index record. Build a
			cache of column prefixes. */
			ext = &node->ext;
		} else {
			/* REDUNDANT and COMPACT formats store a local
			768-byte prefix of each externally stored
			column. No cache is needed. */
			ext = NULL;
			node->ext = NULL;
		}

		node->row = row_build(ROW_COPY_DATA, clust_index, rec,
				      offsets, NULL,
				      NULL, NULL, ext, node->heap);

		/* We will need to parse out virtual column info from undo
		log, first mark them DATA_MISSING. So we will know if the
		value gets updated */
		if (node->table->n_v_cols
		    && (node->state == UNDO_UPDATE_PERSISTENT
			|| node->state == UNDO_UPDATE_TEMPORARY)
		    && !(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
			for (ulint i = 0;
			     i < dict_table_get_n_v_cols(node->table); i++) {
				dfield_get_type(dtuple_get_nth_v_field(
					node->row, i))->mtype = DATA_MISSING;
			}
		}

		if (node->rec_type == TRX_UNDO_UPD_EXIST_REC) {
			ut_ad((node->row->info_bits & ~REC_INFO_DELETED_FLAG)
			      == REC_INFO_MIN_REC_FLAG
			      || node->row->info_bits == 0);
			node->undo_row = dtuple_copy(node->row, node->heap);
			row_upd_replace(node->undo_row, &node->undo_ext,
					clust_index, node->update, node->heap);
		} else {
			ut_ad(((node->row->info_bits & ~REC_INFO_DELETED_FLAG)
			       == REC_INFO_MIN_REC_FLAG)
			      == (node->rec_type == TRX_UNDO_INSERT_METADATA));
			node->undo_row = NULL;
			node->undo_ext = NULL;
		}

		btr_pcur_store_position(&node->pcur, &mtr);
	}

	if (heap) {
		mem_heap_free(heap);
	}

func_exit:
	btr_pcur_commit_specify_mtr(&node->pcur, &mtr);
	return(found);
}

/** Try to truncate the undo logs.
@param[in,out]	trx	transaction */
static void row_undo_try_truncate(trx_t* trx)
{
	if (trx_undo_t*	undo = trx->rsegs.m_redo.undo) {
		ut_ad(undo->rseg == trx->rsegs.m_redo.rseg);
		trx_undo_truncate_end(*undo, trx->undo_no, false);
	}

	if (trx_undo_t* undo = trx->rsegs.m_noredo.undo) {
		ut_ad(undo->rseg == trx->rsegs.m_noredo.rseg);
		trx_undo_truncate_end(*undo, trx->undo_no, true);
	}
}

/** Get the latest undo log record for rollback.
@param[in,out]	node		rollback context
@return	whether an undo log record was fetched */
static bool row_undo_rec_get(undo_node_t* node)
{
	trx_t* trx = node->trx;

	if (trx->pages_undone) {
		trx->pages_undone = 0;
		row_undo_try_truncate(trx);
	}

	trx_undo_t*	undo	= NULL;
	trx_undo_t*	update	= trx->rsegs.m_redo.undo;
	trx_undo_t*	temp	= trx->rsegs.m_noredo.undo;
	const undo_no_t	limit	= trx->roll_limit;
	bool		is_temp = false;

	ut_ad(!update || !temp || update->empty() || temp->empty()
	      || update->top_undo_no != temp->top_undo_no);

	if (update && !update->empty() && update->top_undo_no >= limit) {
		if (!undo) {
			undo = update;
		} else if (undo->top_undo_no < update->top_undo_no) {
			undo = update;
		}
	}

	if (temp && !temp->empty() && temp->top_undo_no >= limit) {
		if (!undo || undo->top_undo_no < temp->top_undo_no) {
			undo = temp;
			is_temp = true;
		}
	}

	if (undo == NULL) {
		row_undo_try_truncate(trx);
		/* Mark any ROLLBACK TO SAVEPOINT completed, so that
		if the transaction object is committed and reused
		later, we will default to a full ROLLBACK. */
		trx->roll_limit = 0;
		trx->in_rollback = false;
		return false;
	}

	ut_ad(!undo->empty());
	ut_ad(limit <= undo->top_undo_no);

	node->roll_ptr = trx_undo_build_roll_ptr(
		false, trx_sys.rseg_id(undo->rseg, !is_temp),
		undo->top_page_no, undo->top_offset);

	mtr_t	mtr;
	mtr.start();

	buf_block_t* undo_page = trx_undo_page_get_s_latched(
		page_id_t(undo->rseg->space->id, undo->top_page_no), &mtr);

	uint16_t offset = undo->top_offset;

	buf_block_t* prev_page = undo_page;
	if (trx_undo_rec_t* prev_rec = trx_undo_get_prev_rec(
		    prev_page, offset, undo->hdr_page_no, undo->hdr_offset,
		    true, &mtr)) {
		if (prev_page != undo_page) {
			trx->pages_undone++;
		}

		undo->top_page_no = prev_page->page.id().page_no();
		undo->top_offset  = page_offset(prev_rec);
		undo->top_undo_no = trx_undo_rec_get_undo_no(prev_rec);
		ut_ad(!undo->empty());
	} else {
		undo->top_undo_no = IB_ID_MAX;
		ut_ad(undo->empty());
	}

	node->undo_rec = trx_undo_rec_copy(undo_page->page.frame + offset,
					   node->heap);
	mtr.commit();

	switch (trx_undo_rec_get_type(node->undo_rec)) {
	case TRX_UNDO_INSERT_METADATA:
		/* This record type was introduced in MDEV-11369
		instant ADD COLUMN, which was implemented after
		MDEV-12288 removed the insert_undo log. There is no
		instant ADD COLUMN for temporary tables. Therefore,
		this record can only be present in the main undo log. */
		/* fall through */
	case TRX_UNDO_RENAME_TABLE:
		ut_ad(undo == update);
		/* fall through */
	case TRX_UNDO_INSERT_REC:
	case TRX_UNDO_EMPTY:
		node->roll_ptr |= 1ULL << ROLL_PTR_INSERT_FLAG_POS;
		node->state = undo == temp
			? UNDO_INSERT_TEMPORARY : UNDO_INSERT_PERSISTENT;
		break;
	default:
		node->state = undo == temp
			? UNDO_UPDATE_TEMPORARY : UNDO_UPDATE_PERSISTENT;
		break;
	}

	trx->undo_no = node->undo_no = trx_undo_rec_get_undo_no(
		node->undo_rec);
	return true;
}

/***********************************************************//**
Fetches an undo log record and does the undo for the recorded operation.
If none left, or a partial rollback completed, returns control to the
parent node, which is always a query thread node.
@return DB_SUCCESS if operation successfully completed, else error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
row_undo(
/*=====*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	ut_ad(node->trx->in_rollback);

	if (node->state == UNDO_NODE_FETCH_NEXT && !row_undo_rec_get(node)) {
		/* Rollback completed for this query thread */
		thr->run_node = que_node_get_parent(node);
		return DB_SUCCESS;
	}

	dberr_t err;

	switch (node->state) {
	case UNDO_INSERT_PERSISTENT:
	case UNDO_INSERT_TEMPORARY:
		err = row_undo_ins(node, thr);
		break;
	case UNDO_UPDATE_PERSISTENT:
	case UNDO_UPDATE_TEMPORARY:
		err = row_undo_mod(node, thr);
		break;
	default:
		ut_ad("wrong state" == 0);
		err = DB_CORRUPTION;
	}

	node->state = UNDO_NODE_FETCH_NEXT;
	btr_pcur_close(&(node->pcur));

	mem_heap_empty(node->heap);

	thr->run_node = node;

	return(err);
}

/***********************************************************//**
Undoes a row operation in a table. This is a high-level function used
in SQL execution graphs.
@return query thread to run next or NULL */
que_thr_t*
row_undo_step(
/*==========*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t		err;
	undo_node_t*	node;
	trx_t*		trx = thr_get_trx(thr);

	node = static_cast<undo_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_UNDO);

	if (UNIV_UNLIKELY(!trx->dict_operation
			  && !srv_undo_sources
			  && srv_shutdown_state != SRV_SHUTDOWN_NONE)
	    && (srv_fast_shutdown == 3 || trx == trx_roll_crash_recv_trx)) {
		/* Shutdown has been initiated. */
		trx->error_state = DB_INTERRUPTED;
		return NULL;
	}

	if (UNIV_UNLIKELY(trx == trx_roll_crash_recv_trx)) {
		trx_roll_report_progress();
	}

	err = row_undo(node, thr);

#ifdef ENABLED_DEBUG_SYNC
	if (trx->mysql_thd) {
		DEBUG_SYNC_C("trx_after_rollback_row");
	}
#endif /* ENABLED_DEBUG_SYNC */

	trx->error_state = err;

	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		ib::fatal() << "Error (" << err << ") in rollback.";
	}

	return(thr);
}
