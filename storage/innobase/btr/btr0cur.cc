/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2015, 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file btr/btr0cur.cc
The index tree cursor

All changes that row operations make to a B-tree or the records
there must go through this module! Undo log records are written here
of every modify or insert of a clustered index record.

			NOTE!!!
To make sure we do not run out of disk space during a pessimistic
insert or update, we have to reserve 2 x the height of the index tree
many pages in the tablespace before we start the operation, because
if leaf splitting has been started, it is difficult to undo, except
by crashing the database and doing a roll-forward.

Created 10/16/1994 Heikki Tuuri
*******************************************************/

#include "btr0cur.h"
#include "row0upd.h"
#include "mtr0log.h"
#include "page0page.h"
#include "page0zip.h"
#include "rem0rec.h"
#include "rem0cmp.h"
#include "buf0lru.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "row0log.h"
#include "row0purge.h"
#include "row0upd.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "que0que.h"
#include "row0row.h"
#include "srv0srv.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"
#include "zlib.h"
#include "srv0start.h"
#include "mysql_com.h"
#include "dict0stats.h"

/** Buffered B-tree operation types, introduced as part of delete buffering. */
enum btr_op_t {
	BTR_NO_OP = 0,			/*!< Not buffered */
	BTR_INSERT_OP,			/*!< Insert, do not ignore UNIQUE */
	BTR_INSERT_IGNORE_UNIQUE_OP,	/*!< Insert, ignoring UNIQUE */
	BTR_DELETE_OP,			/*!< Purge a delete-marked record */
	BTR_DELMARK_OP			/*!< Mark a record for deletion */
};

/** Modification types for the B-tree operation.
    Note that the order must be DELETE, BOTH, INSERT !!
 */
enum btr_intention_t {
	BTR_INTENTION_DELETE,
	BTR_INTENTION_BOTH,
	BTR_INTENTION_INSERT
};

/** For the index->lock scalability improvement, only possibility of clear
performance regression observed was caused by grown huge history list length.
That is because the exclusive use of index->lock also worked as reserving
free blocks and read IO bandwidth with priority. To avoid huge glowing history
list as same level with previous implementation, prioritizes pessimistic tree
operations by purge as the previous, when it seems to be growing huge.

 Experimentally, the history list length starts to affect to performance
throughput clearly from about 100000. */
#define BTR_CUR_FINE_HISTORY_LENGTH	100000

/** Number of searches down the B-tree in btr_cur_search_to_nth_level(). */
Atomic_counter<ulint>	btr_cur_n_non_sea;
/** Old value of btr_cur_n_non_sea.  Copied by
srv_refresh_innodb_monitor_stats().  Referenced by
srv_printf_innodb_monitor(). */
ulint	btr_cur_n_non_sea_old;
#ifdef BTR_CUR_HASH_ADAPT
/** Number of successful adaptive hash index lookups in
btr_cur_search_to_nth_level(). */
ulint	btr_cur_n_sea;
/** Old value of btr_cur_n_sea.  Copied by
srv_refresh_innodb_monitor_stats().  Referenced by
srv_printf_innodb_monitor(). */
ulint	btr_cur_n_sea_old;
#endif /* BTR_CUR_HASH_ADAPT */

#ifdef UNIV_DEBUG
/* Flag to limit optimistic insert records */
uint	btr_cur_limit_optimistic_insert_debug;
#endif /* UNIV_DEBUG */

/** In the optimistic insert, if the insert does not fit, but this much space
can be released by page reorganize, then it is reorganized */
#define BTR_CUR_PAGE_REORGANIZE_LIMIT	(srv_page_size / 32)

/** The structure of a BLOB part header */
/* @{ */
/*--------------------------------------*/
#define BTR_BLOB_HDR_PART_LEN		0	/*!< BLOB part len on this
						page */
#define BTR_BLOB_HDR_NEXT_PAGE_NO	4	/*!< next BLOB part page no,
						FIL_NULL if none */
/*--------------------------------------*/
#define BTR_BLOB_HDR_SIZE		8	/*!< Size of a BLOB
						part header, in bytes */

/** Estimated table level stats from sampled value.
@param value sampled stats
@param index index being sampled
@param sample number of sampled rows
@param ext_size external stored data size
@param not_empty table not empty
@return estimated table wide stats from sampled value */
#define BTR_TABLE_STATS_FROM_SAMPLE(value, index, sample, ext_size, not_empty) \
	(((value) * static_cast<ib_uint64_t>(index->stat_n_leaf_pages) \
	  + (sample) - 1 + (ext_size) + (not_empty)) / ((sample) + (ext_size)))

/* @} */

/*******************************************************************//**
Marks all extern fields in a record as owned by the record. This function
should be called if the delete mark of a record is removed: a not delete
marked record always owns all its extern fields. */
static
void
btr_cur_unmark_extern_fields(
/*=========================*/
	buf_block_t*	block,	/*!< in/out: index page */
	rec_t*		rec,	/*!< in/out: record in a clustered index */
	dict_index_t*	index,	/*!< in: index of the page */
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	mtr_t*		mtr);	/*!< in: mtr, or NULL if not logged */
/*******************************************************************//**
Adds path information to the cursor for the current page, for which
the binary search has been performed. */
static
void
btr_cur_add_path_info(
/*==================*/
	btr_cur_t*	cursor,		/*!< in: cursor positioned on a page */
	ulint		height,		/*!< in: height of the page in tree;
					0 means leaf node */
	ulint		root_height);	/*!< in: root node height in tree */
/***********************************************************//**
Frees the externally stored fields for a record, if the field is mentioned
in the update vector. */
static
void
btr_rec_free_updated_extern_fields(
/*===============================*/
	dict_index_t*	index,	/*!< in: index of rec; the index tree MUST be
				X-latched */
	rec_t*		rec,	/*!< in: record */
	buf_block_t*	block,	/*!< in: index page of rec */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update,	/*!< in: update vector */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr);	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the tree */
/***********************************************************//**
Frees the externally stored fields for a record. */
static
void
btr_rec_free_externally_stored_fields(
/*==================================*/
	dict_index_t*	index,	/*!< in: index of the data, the index
				tree MUST be X-latched */
	rec_t*		rec,	/*!< in: record */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec, index) */
	buf_block_t*	block,	/*!< in: index page of rec */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr);	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the index
				tree */

/*==================== B-TREE SEARCH =========================*/

/** Latches the leaf page or pages requested.
@param[in]	block		leaf page where the search converged
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...
@param[in]	cursor		cursor
@param[in]	mtr		mini-transaction
@return	blocks and savepoints which actually latched. */
btr_latch_leaves_t
btr_cur_latch_leaves(
	buf_block_t*		block,
	ulint			latch_mode,
	btr_cur_t*		cursor,
	mtr_t*			mtr)
{
	rw_lock_type_t	mode;
	uint32_t	left_page_no;
	uint32_t	right_page_no;
	buf_block_t*	get_block;
	bool		spatial;
	btr_latch_leaves_t latch_leaves = {{NULL, NULL, NULL}, {0, 0, 0}};

	compile_time_assert(int(MTR_MEMO_PAGE_S_FIX) == int(RW_S_LATCH));
	compile_time_assert(int(MTR_MEMO_PAGE_X_FIX) == int(RW_X_LATCH));
	compile_time_assert(int(MTR_MEMO_PAGE_SX_FIX) == int(RW_SX_LATCH));
	ut_ad(block->page.id.space() == cursor->index->table->space->id);

	spatial = dict_index_is_spatial(cursor->index) && cursor->rtr_info;
	ut_ad(buf_page_in_file(&block->page));

	switch (latch_mode) {
	case BTR_SEARCH_LEAF:
	case BTR_MODIFY_LEAF:
	case BTR_SEARCH_TREE:
		if (spatial) {
			cursor->rtr_info->tree_savepoints[RTR_MAX_LEVELS]
				= mtr_set_savepoint(mtr);
		}

		mode = latch_mode == BTR_MODIFY_LEAF ? RW_X_LATCH : RW_S_LATCH;
		latch_leaves.savepoints[1] = mtr_set_savepoint(mtr);
		get_block = btr_block_get(*cursor->index,
					  block->page.id.page_no(), mode,
					  true, mtr);
		latch_leaves.blocks[1] = get_block;
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(get_block->frame)
		     == page_is_comp(block->frame));
#endif /* UNIV_BTR_DEBUG */
		if (spatial) {
			cursor->rtr_info->tree_blocks[RTR_MAX_LEVELS]
				= get_block;
		}

		return(latch_leaves);
	case BTR_MODIFY_TREE:
		/* It is exclusive for other operations which calls
		btr_page_set_prev() */
		ut_ad(mtr_memo_contains_flagged(
			      mtr,
			      dict_index_get_lock(cursor->index),
			      MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
		/* x-latch also siblings from left to right */
		left_page_no = btr_page_get_prev(block->frame);

		if (left_page_no != FIL_NULL) {

			if (spatial) {
				cursor->rtr_info->tree_savepoints[
					RTR_MAX_LEVELS] = mtr_set_savepoint(mtr);
			}

			latch_leaves.savepoints[0] = mtr_set_savepoint(mtr);
			get_block = btr_block_get(
				*cursor->index, left_page_no, RW_X_LATCH,
				true, mtr);
			latch_leaves.blocks[0] = get_block;

			if (spatial) {
				cursor->rtr_info->tree_blocks[RTR_MAX_LEVELS]
					= get_block;
			}
		}

		if (spatial) {
			cursor->rtr_info->tree_savepoints[RTR_MAX_LEVELS + 1]
				= mtr_set_savepoint(mtr);
		}

		latch_leaves.savepoints[1] = mtr_set_savepoint(mtr);
		get_block = btr_block_get(
			*cursor->index, block->page.id.page_no(),
			RW_X_LATCH, true, mtr);
		latch_leaves.blocks[1] = get_block;

#ifdef UNIV_BTR_DEBUG
		/* Sanity check only after both the blocks are latched. */
		if (latch_leaves.blocks[0] != NULL) {
			ut_a(page_is_comp(latch_leaves.blocks[0]->frame)
			     == page_is_comp(block->frame));
			ut_a(btr_page_get_next(latch_leaves.blocks[0]->frame)
			     == block->page.id.page_no());
		}
		ut_a(page_is_comp(get_block->frame)
		     == page_is_comp(block->frame));
#endif /* UNIV_BTR_DEBUG */

		if (spatial) {
			cursor->rtr_info->tree_blocks[RTR_MAX_LEVELS + 1]
				= get_block;
		}

		right_page_no = btr_page_get_next(block->frame);

		if (right_page_no != FIL_NULL) {
			if (spatial) {
				cursor->rtr_info->tree_savepoints[
					RTR_MAX_LEVELS + 2] = mtr_set_savepoint(
								mtr);
			}
			latch_leaves.savepoints[2] = mtr_set_savepoint(mtr);
			get_block = btr_block_get(*cursor->index,
						  right_page_no, RW_X_LATCH,
						  true, mtr);
			latch_leaves.blocks[2] = get_block;
#ifdef UNIV_BTR_DEBUG
			ut_a(page_is_comp(get_block->frame)
			     == page_is_comp(block->frame));
			ut_a(btr_page_get_prev(get_block->frame)
			     == block->page.id.page_no());
#endif /* UNIV_BTR_DEBUG */
			if (spatial) {
				cursor->rtr_info->tree_blocks[
					RTR_MAX_LEVELS + 2] = get_block;
			}
		}

		return(latch_leaves);

	case BTR_SEARCH_PREV:
	case BTR_MODIFY_PREV:
		mode = latch_mode == BTR_SEARCH_PREV ? RW_S_LATCH : RW_X_LATCH;
		/* latch also left sibling */
		rw_lock_s_lock(&block->lock);
		left_page_no = btr_page_get_prev(block->frame);
		rw_lock_s_unlock(&block->lock);

		if (left_page_no != FIL_NULL) {
			latch_leaves.savepoints[0] = mtr_set_savepoint(mtr);
			get_block = btr_block_get(
				*cursor->index, left_page_no, mode,
				true, mtr);
			latch_leaves.blocks[0] = get_block;
			cursor->left_block = get_block;
#ifdef UNIV_BTR_DEBUG
			ut_a(page_is_comp(get_block->frame)
			     == page_is_comp(block->frame));
			ut_a(btr_page_get_next(get_block->frame)
			     == block->page.id.page_no());
#endif /* UNIV_BTR_DEBUG */
		}

		latch_leaves.savepoints[1] = mtr_set_savepoint(mtr);
		get_block = btr_block_get(*cursor->index,
					  block->page.id.page_no(), mode,
					  true, mtr);
		latch_leaves.blocks[1] = get_block;
#ifdef UNIV_BTR_DEBUG
		ut_a(page_is_comp(get_block->frame)
		     == page_is_comp(block->frame));
#endif /* UNIV_BTR_DEBUG */
		return(latch_leaves);
	case BTR_CONT_MODIFY_TREE:
		ut_ad(dict_index_is_spatial(cursor->index));
		return(latch_leaves);
	}

	ut_error;
	return(latch_leaves);
}

/** Load the instant ALTER TABLE metadata from the clustered index
when loading a table definition.
@param[in,out]	index	clustered index definition
@param[in,out]	mtr	mini-transaction
@return	error code
@retval	DB_SUCCESS	if no error occurred
@retval	DB_CORRUPTION	if any corruption was noticed */
static dberr_t btr_cur_instant_init_low(dict_index_t* index, mtr_t* mtr)
{
	ut_ad(index->is_primary());
	ut_ad(index->n_core_null_bytes == dict_index_t::NO_CORE_NULL_BYTES);
	ut_ad(index->table->supports_instant());
	ut_ad(index->table->is_readable());

	const fil_space_t* space = index->table->space;
	if (!space) {
unreadable:
		ib::error() << "Table " << index->table->name
			    << " has an unreadable root page";
		index->table->corrupted = true;
		return DB_CORRUPTION;
	}

	page_t* root = btr_root_get(index, mtr);

	if (!root || btr_cur_instant_root_init(index, root)) {
		goto unreadable;
	}

	ut_ad(index->n_core_null_bytes != dict_index_t::NO_CORE_NULL_BYTES);

	if (fil_page_get_type(root) == FIL_PAGE_INDEX) {
		ut_ad(!index->is_instant());
		return DB_SUCCESS;
	}

	btr_cur_t cur;
	/* Relax the assertion in rec_init_offsets(). */
	ut_ad(!index->in_instant_init);
	ut_d(index->in_instant_init = true);
	dberr_t err = btr_cur_open_at_index_side(true, index, BTR_SEARCH_LEAF,
						 &cur, 0, mtr);
	ut_d(index->in_instant_init = false);
	if (err != DB_SUCCESS) {
		index->table->corrupted = true;
		return err;
	}

	ut_ad(page_cur_is_before_first(&cur.page_cur));
	ut_ad(page_is_leaf(cur.page_cur.block->frame));

	page_cur_move_to_next(&cur.page_cur);

	const rec_t* rec = cur.page_cur.rec;
	const ulint comp = dict_table_is_comp(index->table);
	const ulint info_bits = rec_get_info_bits(rec, comp);

	if (page_rec_is_supremum(rec)
	    || !(info_bits & REC_INFO_MIN_REC_FLAG)) {
		if (!index->is_instant()) {
			/* The FIL_PAGE_TYPE_INSTANT and PAGE_INSTANT may be
			assigned even if instant ADD COLUMN was not
			committed. Changes to these page header fields are not
			undo-logged, but changes to the hidden metadata record
			are. If the server is killed and restarted, the page
			header fields could remain set even though no metadata
			record is present. */
			return DB_SUCCESS;
		}

		ib::error() << "Table " << index->table->name
			    << " is missing instant ALTER metadata";
		index->table->corrupted = true;
		return DB_CORRUPTION;
	}

	if ((info_bits & ~REC_INFO_DELETED_FLAG) != REC_INFO_MIN_REC_FLAG
	    || (comp && rec_get_status(rec) != REC_STATUS_INSTANT)) {
incompatible:
		ib::error() << "Table " << index->table->name
			<< " contains unrecognizable instant ALTER metadata";
		index->table->corrupted = true;
		return DB_CORRUPTION;
	}

	/* Read the metadata. We can get here on server restart
	or when the table was evicted from the data dictionary cache
	and is now being accessed again.

	Here, READ COMMITTED and REPEATABLE READ should be equivalent.
	Committing the ADD COLUMN operation would acquire
	MDL_EXCLUSIVE and LOCK_X|LOCK_TABLE, which would prevent any
	concurrent operations on the table, including table eviction
	from the cache. */

	if (info_bits & REC_INFO_DELETED_FLAG) {
		/* This metadata record includes a BLOB that identifies
		any dropped or reordered columns. */
		ulint trx_id_offset = index->trx_id_offset;
		/* If !index->trx_id_offset, the PRIMARY KEY contains
		variable-length columns. For the metadata record,
		variable-length columns should be written with zero
		length. However, before MDEV-21088 was fixed, for
		variable-length encoded PRIMARY KEY column of type
		CHAR, we wrote more than zero bytes. That is why we
		must determine the actual length of each PRIMARY KEY
		column.  The DB_TRX_ID will start right after any
		PRIMARY KEY columns. */
		ut_ad(index->n_uniq);

		/* We cannot invoke rec_get_offsets() before
		index->table->deserialise_columns(). Therefore,
		we must duplicate some logic here. */
		if (trx_id_offset) {
		} else if (index->table->not_redundant()) {
			/* The PRIMARY KEY contains variable-length columns.
			For the metadata record, variable-length columns are
			always written with zero length. The DB_TRX_ID will
			start right after any fixed-length columns. */

			/* OK, before MDEV-21088 was fixed, for
			variable-length encoded PRIMARY KEY column of
			type CHAR, we wrote more than zero bytes. In
			order to allow affected tables to be accessed,
			it would be nice to determine the actual
			length of each PRIMARY KEY column. However, to
			be able to do that, we should determine the
			size of the null-bit bitmap in the metadata
			record. And we cannot know that before reading
			the metadata BLOB, whose starting point we are
			trying to find here. (Although the PRIMARY KEY
			columns cannot be NULL, we would have to know
			where the lengths of variable-length PRIMARY KEY
			columns start.)

			So, unfortunately we cannot help users who
			were affected by MDEV-21088 on a ROW_FORMAT=COMPACT
			or ROW_FORMAT=DYNAMIC table. */

			for (uint i = index->n_uniq; i--; ) {
				trx_id_offset += index->fields[i].fixed_len;
			}
		} else if (rec_get_1byte_offs_flag(rec)) {
			trx_id_offset = rec_1_get_field_end_info(
				rec, index->n_uniq - 1);
			ut_ad(!(trx_id_offset & REC_1BYTE_SQL_NULL_MASK));
			trx_id_offset &= ~REC_1BYTE_SQL_NULL_MASK;
		} else {
			trx_id_offset = rec_2_get_field_end_info(
				rec, index->n_uniq - 1);
			ut_ad(!(trx_id_offset & REC_2BYTE_SQL_NULL_MASK));
			trx_id_offset &= ~REC_2BYTE_SQL_NULL_MASK;
		}

		const byte* ptr = rec + trx_id_offset
			+ (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

		if (mach_read_from_4(ptr + BTR_EXTERN_LEN)) {
			goto incompatible;
		}

		uint len = mach_read_from_4(ptr + BTR_EXTERN_LEN + 4);
		if (!len
		    || mach_read_from_4(ptr + BTR_EXTERN_OFFSET)
		    != FIL_PAGE_DATA
		    || mach_read_from_4(ptr + BTR_EXTERN_SPACE_ID)
		    != space->id) {
			goto incompatible;
		}

		buf_block_t* block = buf_page_get(
			page_id_t(space->id,
				  mach_read_from_4(ptr + BTR_EXTERN_PAGE_NO)),
			0, RW_S_LATCH, mtr);
		buf_block_dbg_add_level(block, SYNC_EXTERN_STORAGE);
		if (fil_page_get_type(block->frame) != FIL_PAGE_TYPE_BLOB
		    || mach_read_from_4(&block->frame[FIL_PAGE_DATA
						      + BTR_BLOB_HDR_NEXT_PAGE_NO])
		    != FIL_NULL
		    || mach_read_from_4(&block->frame[FIL_PAGE_DATA
						      + BTR_BLOB_HDR_PART_LEN])
		    != len) {
			goto incompatible;
		}

		/* The unused part of the BLOB page should be zero-filled. */
		for (const byte* b = block->frame
		       + (FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE) + len,
		       * const end = block->frame + srv_page_size
		       - BTR_EXTERN_LEN;
		     b < end; ) {
			if (*b++) {
				goto incompatible;
			}
		}

		if (index->table->deserialise_columns(
			    &block->frame[FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE],
			    len)) {
			goto incompatible;
		}

		/* Proceed to initialize the default values of
		any instantly added columns. */
	}

	mem_heap_t* heap = NULL;
	offset_t* offsets = rec_get_offsets(rec, index, NULL, true,
					    ULINT_UNDEFINED, &heap);
	if (rec_offs_any_default(offsets)) {
inconsistent:
		mem_heap_free(heap);
		goto incompatible;
	}

	/* In fact, because we only ever append fields to the metadata
	record, it is also OK to perform READ UNCOMMITTED and
	then ignore any extra fields, provided that
	trx_sys.is_registered(DB_TRX_ID). */
	if (rec_offs_n_fields(offsets)
	    > ulint(index->n_fields) + !!index->table->instant
	    && !trx_sys.is_registered(current_trx(),
				      row_get_rec_trx_id(rec, index,
							 offsets))) {
		goto inconsistent;
	}

	for (unsigned i = index->n_core_fields; i < index->n_fields; i++) {
		dict_col_t* col = index->fields[i].col;
		const unsigned o = i + !!index->table->instant;
		ulint len;
		const byte* data = rec_get_nth_field(rec, offsets, o, &len);
		ut_ad(!col->is_added());
		ut_ad(!col->def_val.data);
		col->def_val.len = len;
		switch (len) {
		case UNIV_SQL_NULL:
			continue;
		case 0:
			col->def_val.data = field_ref_zero;
			continue;
		}
		ut_ad(len != UNIV_SQL_DEFAULT);
		if (!rec_offs_nth_extern(offsets, o)) {
			col->def_val.data = mem_heap_dup(
				index->table->heap, data, len);
		} else if (len < BTR_EXTERN_FIELD_REF_SIZE
			   || !memcmp(data + len - BTR_EXTERN_FIELD_REF_SIZE,
				      field_ref_zero,
				      BTR_EXTERN_FIELD_REF_SIZE)) {
			col->def_val.len = UNIV_SQL_DEFAULT;
			goto inconsistent;
		} else {
			col->def_val.data = btr_copy_externally_stored_field(
				&col->def_val.len, data,
				cur.page_cur.block->zip_size(),
				len, index->table->heap);
		}
	}

	mem_heap_free(heap);
	return DB_SUCCESS;
}

/** Load the instant ALTER TABLE metadata from the clustered index
when loading a table definition.
@param[in,out]	table	table definition from the data dictionary
@return	error code
@retval	DB_SUCCESS	if no error occurred */
dberr_t
btr_cur_instant_init(dict_table_t* table)
{
	mtr_t		mtr;
	dict_index_t*	index = dict_table_get_first_index(table);
	mtr.start();
	dberr_t	err = index
		? btr_cur_instant_init_low(index, &mtr)
		: DB_CORRUPTION;
	mtr.commit();
	return(err);
}

/** Initialize the n_core_null_bytes on first access to a clustered
index root page.
@param[in]	index	clustered index that is on its first access
@param[in]	page	clustered index root page
@return	whether the page is corrupted */
bool btr_cur_instant_root_init(dict_index_t* index, const page_t* page)
{
	ut_ad(!index->is_dummy);
	ut_ad(fil_page_index_page_check(page));
	ut_ad(!page_has_siblings(page));
	ut_ad(page_get_space_id(page) == index->table->space_id);
	ut_ad(page_get_page_no(page) == index->page);
	ut_ad(!page_is_comp(page) == !dict_table_is_comp(index->table));
	ut_ad(index->is_primary());
	ut_ad(!index->is_instant());
	ut_ad(index->table->supports_instant());
	/* This is normally executed as part of btr_cur_instant_init()
	when dict_load_table_one() is loading a table definition.
	Other threads should not access or modify the n_core_null_bytes,
	n_core_fields before dict_load_table_one() returns.

	This can also be executed during IMPORT TABLESPACE, where the
	table definition is exclusively locked. */

	switch (fil_page_get_type(page)) {
	default:
		ut_ad("wrong page type" == 0);
		return true;
	case FIL_PAGE_INDEX:
		/* The field PAGE_INSTANT is guaranteed 0 on clustered
		index root pages of ROW_FORMAT=COMPACT or
		ROW_FORMAT=DYNAMIC when instant ADD COLUMN is not used. */
		ut_ad(!page_is_comp(page) || !page_get_instant(page));
		index->n_core_null_bytes = static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(index->n_nullable)));
		return false;
	case FIL_PAGE_TYPE_INSTANT:
		break;
	}

	const uint16_t n = page_get_instant(page);

	if (n < index->n_uniq + DATA_ROLL_PTR) {
		/* The PRIMARY KEY (or hidden DB_ROW_ID) and
		DB_TRX_ID,DB_ROLL_PTR columns must always be present
		as 'core' fields. */
		return true;
	}

	if (n > REC_MAX_N_FIELDS) {
		return true;
	}

	index->n_core_fields = n & dict_index_t::MAX_N_FIELDS;

	const rec_t* infimum = page_get_infimum_rec(page);
	const rec_t* supremum = page_get_supremum_rec(page);

	if (!memcmp(infimum, "infimum", 8)
	    && !memcmp(supremum, "supremum", 8)) {
		if (n > index->n_fields) {
			/* All fields, including those for instantly
			added columns, must be present in the
			data dictionary. */
			return true;
		}

		ut_ad(!index->is_dummy);
		ut_d(index->is_dummy = true);
		index->n_core_null_bytes = static_cast<uint8_t>(
			UT_BITS_IN_BYTES(index->get_n_nullable(n)));
		ut_d(index->is_dummy = false);
		return false;
	}

	if (memcmp(infimum, field_ref_zero, 8)
	    || memcmp(supremum, field_ref_zero, 7)) {
		/* The infimum and supremum records must either contain
		the original strings, or they must be filled with zero
		bytes, except for the bytes that we have repurposed. */
		return true;
	}

	index->n_core_null_bytes = supremum[7];
	return index->n_core_null_bytes > 128;
}

/** Optimistically latches the leaf page or pages requested.
@param[in]	block		guessed buffer block
@param[in]	modify_clock	modify clock value
@param[in,out]	latch_mode	BTR_SEARCH_LEAF, ...
@param[in,out]	cursor		cursor
@param[in]	file		file name
@param[in]	line		line where called
@param[in]	mtr		mini-transaction
@return true if success */
bool
btr_cur_optimistic_latch_leaves(
	buf_block_t*	block,
	ib_uint64_t	modify_clock,
	ulint*		latch_mode,
	btr_cur_t*	cursor,
	const char*	file,
	unsigned	line,
	mtr_t*		mtr)
{
	rw_lock_type_t	mode;
	ulint		left_page_no;

	switch (*latch_mode) {
	case BTR_SEARCH_LEAF:
	case BTR_MODIFY_LEAF:
		return(buf_page_optimistic_get(*latch_mode, block,
				modify_clock, file, line, mtr));
	case BTR_SEARCH_PREV:
	case BTR_MODIFY_PREV:
		mode = *latch_mode == BTR_SEARCH_PREV
			? RW_S_LATCH : RW_X_LATCH;

		buf_page_mutex_enter(block);
		if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
			buf_page_mutex_exit(block);
			return(false);
		}
		/* pin the block not to be relocated */
		buf_block_buf_fix_inc(block, file, line);
		buf_page_mutex_exit(block);

		rw_lock_s_lock(&block->lock);
		if (block->modify_clock != modify_clock) {
			rw_lock_s_unlock(&block->lock);

			goto unpin_failed;
		}
		left_page_no = btr_page_get_prev(block->frame);
		rw_lock_s_unlock(&block->lock);

		cursor->left_block = left_page_no != FIL_NULL
			? btr_block_get(*cursor->index, left_page_no, mode,
					page_is_leaf(block->frame), mtr)
			: NULL;

		if (buf_page_optimistic_get(mode, block, modify_clock,
					    file, line, mtr)) {
			if (btr_page_get_prev(block->frame) == left_page_no) {
				buf_block_buf_fix_dec(block);
				*latch_mode = mode;
				return(true);
			} else {
				/* release the block */
				btr_leaf_page_release(block, mode, mtr);
			}
		}

		/* release the left block */
		if (cursor->left_block != NULL) {
			btr_leaf_page_release(cursor->left_block,
					      mode, mtr);
		}
unpin_failed:
		/* unpin the block */
		buf_block_buf_fix_dec(block);
		return(false);

	default:
		ut_error;
		return(false);
	}
}

/**
Gets intention in btr_intention_t from latch_mode, and cleares the intention
at the latch_mode.
@param latch_mode	in/out: pointer to latch_mode
@return intention for latching tree */
static
btr_intention_t
btr_cur_get_and_clear_intention(
	ulint	*latch_mode)
{
	btr_intention_t	intention;

	switch (*latch_mode & (BTR_LATCH_FOR_INSERT | BTR_LATCH_FOR_DELETE)) {
	case BTR_LATCH_FOR_INSERT:
		intention = BTR_INTENTION_INSERT;
		break;
	case BTR_LATCH_FOR_DELETE:
		intention = BTR_INTENTION_DELETE;
		break;
	default:
		/* both or unknown */
		intention = BTR_INTENTION_BOTH;
	}
	*latch_mode &= ulint(~(BTR_LATCH_FOR_INSERT | BTR_LATCH_FOR_DELETE));

	return(intention);
}

/**
Gets the desired latch type for the root leaf (root page is root leaf)
at the latch mode.
@param latch_mode	in: BTR_SEARCH_LEAF, ...
@return latch type */
static
rw_lock_type_t
btr_cur_latch_for_root_leaf(
	ulint	latch_mode)
{
	switch (latch_mode) {
	case BTR_SEARCH_LEAF:
	case BTR_SEARCH_TREE:
	case BTR_SEARCH_PREV:
		return(RW_S_LATCH);
	case BTR_MODIFY_LEAF:
	case BTR_MODIFY_TREE:
	case BTR_MODIFY_PREV:
		return(RW_X_LATCH);
	case BTR_CONT_MODIFY_TREE:
	case BTR_CONT_SEARCH_TREE:
		/* A root page should be latched already,
		and don't need to be latched here.
		fall through (RW_NO_LATCH) */
	case BTR_NO_LATCHES:
		return(RW_NO_LATCH);
	}

	ut_error;
	return(RW_NO_LATCH); /* avoid compiler warnings */
}

/** Detects whether the modifying record might need a modifying tree structure.
@param[in]	index		index
@param[in]	page		page
@param[in]	lock_intention	lock intention for the tree operation
@param[in]	rec		record (current node_ptr)
@param[in]	rec_size	size of the record or max size of node_ptr
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	mtr		mtr
@return true if tree modification is needed */
static
bool
btr_cur_will_modify_tree(
	dict_index_t*	index,
	const page_t*	page,
	btr_intention_t	lock_intention,
	const rec_t*	rec,
	ulint		rec_size,
	ulint		zip_size,
	mtr_t*		mtr)
{
	ut_ad(!page_is_leaf(page));
	ut_ad(mtr_memo_contains_flagged(mtr, dict_index_get_lock(index),
					MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));

	/* Pessimistic delete of the first record causes delete & insert
	of node_ptr at upper level. And a subsequent page shrink is
	possible. It causes delete of node_ptr at the upper level.
	So we should pay attention also to 2nd record not only
	first record and last record. Because if the "delete & insert" are
	done for the different page, the 2nd record become
	first record and following compress might delete the record and causes
	the uppper level node_ptr modification. */

	const ulint n_recs = page_get_n_recs(page);

	if (lock_intention <= BTR_INTENTION_BOTH) {
		compile_time_assert(BTR_INTENTION_DELETE < BTR_INTENTION_BOTH);
		compile_time_assert(BTR_INTENTION_BOTH < BTR_INTENTION_INSERT);

		if (!page_has_siblings(page)) {
			return true;
		}

		ulint margin = rec_size;

		if (lock_intention == BTR_INTENTION_BOTH) {
			ulint	level = btr_page_get_level(page);

			/* This value is the worst expectation for the node_ptr
			records to be deleted from this page. It is used to
			expect whether the cursor position can be the left_most
			record in this page or not. */
			ulint   max_nodes_deleted = 0;

			/* By modifying tree operations from the under of this
			level, logically (2 ^ (level - 1)) opportunities to
			deleting records in maximum even unreally rare case. */
			if (level > 7) {
				/* TODO: adjust this practical limit. */
				max_nodes_deleted = 64;
			} else if (level > 0) {
				max_nodes_deleted = (ulint)1 << (level - 1);
			}
			/* check delete will cause. (BTR_INTENTION_BOTH
			or BTR_INTENTION_DELETE) */
			if (n_recs <= max_nodes_deleted * 2
			    || page_rec_is_first(rec, page)) {
				/* The cursor record can be the left most record
				in this page. */
				return true;
			}

			if (page_has_prev(page)
			    && page_rec_distance_is_at_most(
				    page_get_infimum_rec(page), rec,
				    max_nodes_deleted)) {
				return true;
			}

			if (page_has_next(page)
			    && page_rec_distance_is_at_most(
				    rec, page_get_supremum_rec(page),
				    max_nodes_deleted)) {
				return true;
			}

			/* Delete at leftmost record in a page causes delete
			& insert at its parent page. After that, the delete
			might cause btr_compress() and delete record at its
			parent page. Thus we should consider max deletes. */
			margin *= max_nodes_deleted;
		}

		/* Safe because we already have SX latch of the index tree */
		if (page_get_data_size(page)
		    < margin + BTR_CUR_PAGE_COMPRESS_LIMIT(index)) {
			return(true);
		}
	}

	if (lock_intention >= BTR_INTENTION_BOTH) {
		/* check insert will cause. BTR_INTENTION_BOTH
		or BTR_INTENTION_INSERT*/

		/* Once we invoke the btr_cur_limit_optimistic_insert_debug,
		we should check it here in advance, since the max allowable
		records in a page is limited. */
		LIMIT_OPTIMISTIC_INSERT_DEBUG(n_recs, return true);

		/* needs 2 records' space for the case the single split and
		insert cannot fit.
		page_get_max_insert_size_after_reorganize() includes space
		for page directory already */
		ulint	max_size
			= page_get_max_insert_size_after_reorganize(page, 2);

		if (max_size < BTR_CUR_PAGE_REORGANIZE_LIMIT + rec_size
		    || max_size < rec_size * 2) {
			return(true);
		}

		/* TODO: optimize this condition for ROW_FORMAT=COMPRESSED.
		This is based on the worst case, and we could invoke
		page_zip_available() on the block->page.zip. */
		/* needs 2 records' space also for worst compress rate. */
		if (zip_size
		    && page_zip_empty_size(index->n_fields, zip_size)
		    <= rec_size * 2 + page_get_data_size(page)
		    + page_dir_calc_reserved_space(n_recs + 2)) {
			return(true);
		}
	}

	return(false);
}

/** Detects whether the modifying record might need a opposite modification
to the intention.
@param[in]	page		page
@param[in]	lock_intention	lock intention for the tree operation
@param[in]	rec		record (current node_ptr)
@return	true if tree modification is needed */
static
bool
btr_cur_need_opposite_intention(
	const page_t*	page,
	btr_intention_t	lock_intention,
	const rec_t*	rec)
{
	switch (lock_intention) {
	case BTR_INTENTION_DELETE:
		return (page_has_prev(page) && page_rec_is_first(rec, page)) ||
			(page_has_next(page) && page_rec_is_last(rec, page));
	case BTR_INTENTION_INSERT:
		return page_has_next(page) && page_rec_is_last(rec, page);
	case BTR_INTENTION_BOTH:
		return(false);
	}

	ut_error;
	return(false);
}

/**
@param[in]	index b-tree
@return maximum size of a node pointer record in bytes */
static ulint btr_node_ptr_max_size(const dict_index_t* index)
{
	if (dict_index_is_ibuf(index)) {
		/* cannot estimate accurately */
		/* This is universal index for change buffer.
		The max size of the entry is about max key length * 2.
		(index key + primary key to be inserted to the index)
		(The max key length is UNIV_PAGE_SIZE / 16 * 3 at
		 ha_innobase::max_supported_key_length(),
		 considering MAX_KEY_LENGTH = 3072 at MySQL imposes
		 the 3500 historical InnoDB value for 16K page size case.)
		For the universal index, node_ptr contains most of the entry.
		And 512 is enough to contain ibuf columns and meta-data */
		return srv_page_size / 8 * 3 + 512;
	}

	/* Each record has page_no, length of page_no and header. */
	ulint comp = dict_table_is_comp(index->table);
	ulint rec_max_size = comp
		? REC_NODE_PTR_SIZE + 1 + REC_N_NEW_EXTRA_BYTES
		+ UT_BITS_IN_BYTES(index->n_nullable)
		: REC_NODE_PTR_SIZE + 2 + REC_N_OLD_EXTRA_BYTES
		+ 2 * index->n_fields;

	/* Compute the maximum possible record size. */
	for (ulint i = 0; i < dict_index_get_n_unique_in_tree(index); i++) {
		const dict_field_t*	field
			= dict_index_get_nth_field(index, i);
		const dict_col_t*	col
			= dict_field_get_col(field);
		ulint			field_max_size;
		ulint			field_ext_max_size;

		/* Determine the maximum length of the index field. */

		field_max_size = dict_col_get_fixed_size(col, comp);
		if (field_max_size) {
			/* dict_index_add_col() should guarantee this */
			ut_ad(!field->prefix_len
			      || field->fixed_len == field->prefix_len);
			/* Fixed lengths are not encoded
			in ROW_FORMAT=COMPACT. */
			rec_max_size += field_max_size;
			continue;
		}

		field_max_size = dict_col_get_max_size(col);
		if (UNIV_UNLIKELY(!field_max_size)) {
			switch (col->mtype) {
			case DATA_VARCHAR:
				if (!comp
				    && (!strcmp(index->table->name.m_name,
						"SYS_FOREIGN")
					|| !strcmp(index->table->name.m_name,
						   "SYS_FOREIGN_COLS"))) {
					break;
				}
				/* fall through */
			case DATA_VARMYSQL:
			case DATA_CHAR:
			case DATA_MYSQL:
				/* CHAR(0) and VARCHAR(0) are possible
				data type definitions in MariaDB.
				The InnoDB internal SQL parser maps
				CHAR to DATA_VARCHAR, so DATA_CHAR (or
				DATA_MYSQL) is only coming from the
				MariaDB SQL layer. */
				if (comp) {
					/* Add a length byte, because
					fixed-length empty field are
					encoded as variable-length.
					For ROW_FORMAT=REDUNDANT,
					these bytes were added to
					rec_max_size before this loop. */
					rec_max_size++;
				}
				continue;
			}

			/* SYS_FOREIGN.ID is defined as CHAR in the
			InnoDB internal SQL parser, which translates
			into the incorrect VARCHAR(0).  InnoDB does
			not enforce maximum lengths of columns, so
			that is why any data can be inserted in the
			first place.

			Likewise, SYS_FOREIGN.FOR_NAME,
			SYS_FOREIGN.REF_NAME, SYS_FOREIGN_COLS.ID, are
			defined as CHAR, and also they are part of a key. */

			ut_ad(!strcmp(index->table->name.m_name,
				      "SYS_FOREIGN")
			      || !strcmp(index->table->name.m_name,
					 "SYS_FOREIGN_COLS"));
			ut_ad(!comp);
			ut_ad(col->mtype == DATA_VARCHAR);

			rec_max_size += (srv_page_size == UNIV_PAGE_SIZE_MAX)
				? REDUNDANT_REC_MAX_DATA_SIZE
				: page_get_free_space_of_empty(FALSE) / 2;
		} else if (field_max_size == NAME_LEN && i == 1
			   && (!strcmp(index->table->name.m_name,
				       TABLE_STATS_NAME)
			       || !strcmp(index->table->name.m_name,
					  INDEX_STATS_NAME))) {
			ut_ad(!strcmp(field->name, "table_name"));
			/* Interpret "table_name" as VARCHAR(199) even
			if it was incorrectly defined as VARCHAR(64).
			While the caller of ha_innobase enforces the
			maximum length on any data written, the InnoDB
			internal SQL parser will happily write as much
			data as is provided. The purpose of this hack
			is to avoid InnoDB hangs after persistent
			statistics on partitioned tables are
			deleted. */
			field_max_size = 199 * SYSTEM_CHARSET_MBMAXLEN;
		}
		field_ext_max_size = field_max_size < 256 ? 1 : 2;

		if (field->prefix_len
		    && field->prefix_len < field_max_size) {
			field_max_size = field->prefix_len;
		}

		if (comp) {
			/* Add the extra size for ROW_FORMAT=COMPACT.
			For ROW_FORMAT=REDUNDANT, these bytes were
			added to rec_max_size before this loop. */
			rec_max_size += field_ext_max_size;
		}

		rec_max_size += field_max_size;
	}

	return rec_max_size;
}

/********************************************************************//**
Searches an index tree and positions a tree cursor on a given level.
NOTE: n_fields_cmp in tuple must be set so that it cannot be compared
to node pointer page number fields on the upper levels of the tree!
Note that if mode is PAGE_CUR_LE, which is used in inserts, then
cursor->up_match and cursor->low_match both will have sensible values.
If mode is PAGE_CUR_GE, then up_match will a have a sensible value.

If mode is PAGE_CUR_LE , cursor is left at the place where an insert of the
search tuple should be performed in the B-tree. InnoDB does an insert
immediately after the cursor. Thus, the cursor may end up on a user record,
or on a page infimum record. */
dberr_t
btr_cur_search_to_nth_level_func(
	dict_index_t*	index,	/*!< in: index */
	ulint		level,	/*!< in: the tree level of search */
	const dtuple_t*	tuple,	/*!< in: data tuple; NOTE: n_fields_cmp in
				tuple must be set so that it cannot get
				compared to the node ptr page number field! */
	page_cur_mode_t	mode,	/*!< in: PAGE_CUR_L, ...;
				Inserts should always be made using
				PAGE_CUR_LE to search the position! */
	ulint		latch_mode, /*!< in: BTR_SEARCH_LEAF, ..., ORed with
				at most one of BTR_INSERT, BTR_DELETE_MARK,
				BTR_DELETE, or BTR_ESTIMATE;
				cursor->left_block is used to store a pointer
				to the left neighbor page, in the cases
				BTR_SEARCH_PREV and BTR_MODIFY_PREV;
				NOTE that if ahi_latch, we might not have a
				cursor page latch, we assume that ahi_latch
				protects the record! */
	btr_cur_t*	cursor, /*!< in/out: tree cursor; the cursor page is
				s- or x-latched, but see also above! */
#ifdef BTR_CUR_HASH_ADAPT
	rw_lock_t*	ahi_latch,
				/*!< in: currently held btr_search_latch
				(in RW_S_LATCH mode), or NULL */
#endif /* BTR_CUR_HASH_ADAPT */
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line where called */
	mtr_t*		mtr,	/*!< in: mtr */
	ib_uint64_t	autoinc)/*!< in: PAGE_ROOT_AUTO_INC to be written
				(0 if none) */
{
	page_t*		page = NULL; /* remove warning */
	buf_block_t*	block;
	buf_block_t*	guess;
	ulint		height;
	ulint		up_match;
	ulint		up_bytes;
	ulint		low_match;
	ulint		low_bytes;
	ulint		rw_latch;
	page_cur_mode_t	page_mode;
	page_cur_mode_t	search_mode = PAGE_CUR_UNSUPP;
	ulint		buf_mode;
	ulint		estimate;
	ulint		node_ptr_max_size = srv_page_size / 2;
	page_cur_t*	page_cursor;
	btr_op_t	btr_op;
	ulint		root_height = 0; /* remove warning */
	dberr_t		err = DB_SUCCESS;

	btr_intention_t	lock_intention;
	bool		modify_external;
	buf_block_t*	tree_blocks[BTR_MAX_LEVELS];
	ulint		tree_savepoints[BTR_MAX_LEVELS];
	ulint		n_blocks = 0;
	ulint		n_releases = 0;
	bool		detected_same_key_root = false;

	bool		retrying_for_search_prev = false;
	ulint		leftmost_from_level = 0;
	buf_block_t**	prev_tree_blocks = NULL;
	ulint*		prev_tree_savepoints = NULL;
	ulint		prev_n_blocks = 0;
	ulint		prev_n_releases = 0;
	bool		need_path = true;
	bool		rtree_parent_modified = false;
	bool		mbr_adj = false;
	bool		found = false;

	DBUG_ENTER("btr_cur_search_to_nth_level");

#ifdef BTR_CUR_ADAPT
	btr_search_t*	info;
#endif /* BTR_CUR_ADAPT */
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	offset_t	offsets2_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets2	= offsets2_;
	rec_offs_init(offsets_);
	rec_offs_init(offsets2_);
	/* Currently, PAGE_CUR_LE is the only search mode used for searches
	ending to upper levels */

	ut_ad(level == 0 || mode == PAGE_CUR_LE
	      || RTREE_SEARCH_MODE(mode));
	ut_ad(dict_index_check_search_tuple(index, tuple));
	ut_ad(!dict_index_is_ibuf(index) || ibuf_inside(mtr));
	ut_ad(dtuple_check_typed(tuple));
	ut_ad(!(index->type & DICT_FTS));
	ut_ad(index->page != FIL_NULL);

	UNIV_MEM_INVALID(&cursor->up_match, sizeof cursor->up_match);
	UNIV_MEM_INVALID(&cursor->up_bytes, sizeof cursor->up_bytes);
	UNIV_MEM_INVALID(&cursor->low_match, sizeof cursor->low_match);
	UNIV_MEM_INVALID(&cursor->low_bytes, sizeof cursor->low_bytes);
#ifdef UNIV_DEBUG
	cursor->up_match = ULINT_UNDEFINED;
	cursor->low_match = ULINT_UNDEFINED;
#endif /* UNIV_DEBUG */

	ibool	s_latch_by_caller;

	s_latch_by_caller = latch_mode & BTR_ALREADY_S_LATCHED;

	ut_ad(!s_latch_by_caller
	      || srv_read_only_mode
	      || mtr_memo_contains_flagged(mtr,
					   dict_index_get_lock(index),
					   MTR_MEMO_S_LOCK
					   | MTR_MEMO_SX_LOCK));

	/* These flags are mutually exclusive, they are lumped together
	with the latch mode for historical reasons. It's possible for
	none of the flags to be set. */
	switch (UNIV_EXPECT(latch_mode
			    & (BTR_INSERT | BTR_DELETE | BTR_DELETE_MARK),
			    0)) {
	case 0:
		btr_op = BTR_NO_OP;
		break;
	case BTR_INSERT:
		btr_op = (latch_mode & BTR_IGNORE_SEC_UNIQUE)
			? BTR_INSERT_IGNORE_UNIQUE_OP
			: BTR_INSERT_OP;
		break;
	case BTR_DELETE:
		btr_op = BTR_DELETE_OP;
		ut_a(cursor->purge_node);
		break;
	case BTR_DELETE_MARK:
		btr_op = BTR_DELMARK_OP;
		break;
	default:
		/* only one of BTR_INSERT, BTR_DELETE, BTR_DELETE_MARK
		should be specified at a time */
		ut_error;
	}

	/* Operations on the insert buffer tree cannot be buffered. */
	ut_ad(btr_op == BTR_NO_OP || !dict_index_is_ibuf(index));
	/* Operations on the clustered index cannot be buffered. */
	ut_ad(btr_op == BTR_NO_OP || !dict_index_is_clust(index));
	/* Operations on the temporary table(indexes) cannot be buffered. */
	ut_ad(btr_op == BTR_NO_OP || !index->table->is_temporary());
	/* Operation on the spatial index cannot be buffered. */
	ut_ad(btr_op == BTR_NO_OP || !dict_index_is_spatial(index));

	estimate = latch_mode & BTR_ESTIMATE;

	lock_intention = btr_cur_get_and_clear_intention(&latch_mode);

	modify_external = latch_mode & BTR_MODIFY_EXTERNAL;

	/* Turn the flags unrelated to the latch mode off. */
	latch_mode = BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);

	ut_ad(!modify_external || latch_mode == BTR_MODIFY_LEAF);

	ut_ad(!s_latch_by_caller
	      || latch_mode == BTR_SEARCH_LEAF
	      || latch_mode == BTR_SEARCH_TREE
	      || latch_mode == BTR_MODIFY_LEAF);

	ut_ad(autoinc == 0 || dict_index_is_clust(index));
	ut_ad(autoinc == 0
	      || latch_mode == BTR_MODIFY_TREE
	      || latch_mode == BTR_MODIFY_LEAF);
	ut_ad(autoinc == 0 || level == 0);

	cursor->flag = BTR_CUR_BINARY;
	cursor->index = index;

#ifndef BTR_CUR_ADAPT
	guess = NULL;
#else
	info = btr_search_get_info(index);

	if (!buf_pool.is_obsolete(info->withdraw_clock)) {
		guess = info->root_guess;
	} else {
		guess = NULL;
	}

#ifdef BTR_CUR_HASH_ADAPT

# ifdef UNIV_SEARCH_PERF_STAT
	info->n_searches++;
# endif
	if (autoinc == 0
	    && latch_mode <= BTR_MODIFY_LEAF
	    && info->last_hash_succ
# ifdef MYSQL_INDEX_DISABLE_AHI
	    && !index->disable_ahi
# endif
	    && !estimate
# ifdef PAGE_CUR_LE_OR_EXTENDS
	    && mode != PAGE_CUR_LE_OR_EXTENDS
# endif /* PAGE_CUR_LE_OR_EXTENDS */
	    && !dict_index_is_spatial(index)
	    /* If !ahi_latch, we do a dirty read of
	    btr_search_enabled below, and btr_search_guess_on_hash()
	    will have to check it again. */
	    && btr_search_enabled
	    && !modify_external
	    && !(tuple->info_bits & REC_INFO_MIN_REC_FLAG)
	    && btr_search_guess_on_hash(index, info, tuple, mode,
					latch_mode, cursor,
					ahi_latch, mtr)) {

		/* Search using the hash index succeeded */

		ut_ad(cursor->up_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_GE);
		ut_ad(cursor->up_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_LE);
		ut_ad(cursor->low_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_LE);
		btr_cur_n_sea++;

		DBUG_RETURN(err);
	}
# endif /* BTR_CUR_HASH_ADAPT */
#endif /* BTR_CUR_ADAPT */
	btr_cur_n_non_sea++;

	/* If the hash search did not succeed, do binary search down the
	tree */

#ifdef BTR_CUR_HASH_ADAPT
	if (ahi_latch) {
		/* Release possible search latch to obey latching order */
		rw_lock_s_unlock(ahi_latch);
	}
#endif /* BTR_CUR_HASH_ADAPT */

	/* Store the position of the tree latch we push to mtr so that we
	know how to release it when we have latched leaf node(s) */

	ulint savepoint = mtr_set_savepoint(mtr);

	rw_lock_type_t upper_rw_latch;

	switch (latch_mode) {
	case BTR_MODIFY_TREE:
		/* Most of delete-intended operations are purging.
		Free blocks and read IO bandwidth should be prior
		for them, when the history list is glowing huge. */
		if (lock_intention == BTR_INTENTION_DELETE
		    && trx_sys.rseg_history_len > BTR_CUR_FINE_HISTORY_LENGTH
		    && buf_pool.n_pend_reads) {
x_latch_index:
			mtr_x_lock_index(index, mtr);
		} else if (index->is_spatial()
			   && lock_intention <= BTR_INTENTION_BOTH) {
			/* X lock the if there is possibility of
			pessimistic delete on spatial index. As we could
			lock upward for the tree */
			goto x_latch_index;
		} else {
			mtr_sx_lock_index(index, mtr);
		}
		upper_rw_latch = RW_X_LATCH;
		break;
	case BTR_CONT_MODIFY_TREE:
	case BTR_CONT_SEARCH_TREE:
		/* Do nothing */
		ut_ad(srv_read_only_mode
		      || mtr_memo_contains_flagged(mtr,
						   dict_index_get_lock(index),
						   MTR_MEMO_X_LOCK
						   | MTR_MEMO_SX_LOCK));
		if (dict_index_is_spatial(index)
		    && latch_mode == BTR_CONT_MODIFY_TREE) {
			/* If we are about to locating parent page for split
			and/or merge operation for R-Tree index, X latch
			the parent */
			upper_rw_latch = RW_X_LATCH;
		} else {
			upper_rw_latch = RW_NO_LATCH;
		}
		break;
	default:
		if (!srv_read_only_mode) {
			if (s_latch_by_caller) {
				ut_ad(rw_lock_own(dict_index_get_lock(index),
				              RW_LOCK_S));
			} else if (!modify_external) {
				/* BTR_SEARCH_TREE is intended to be used with
				BTR_ALREADY_S_LATCHED */
				ut_ad(latch_mode != BTR_SEARCH_TREE);

				mtr_s_lock_index(index, mtr);
			} else {
				/* BTR_MODIFY_EXTERNAL needs to be excluded */
				mtr_sx_lock_index(index, mtr);
			}
			upper_rw_latch = RW_S_LATCH;
		} else {
			upper_rw_latch = RW_NO_LATCH;
		}
	}
	const rw_lock_type_t root_leaf_rw_latch = btr_cur_latch_for_root_leaf(
		latch_mode);

	page_cursor = btr_cur_get_page_cur(cursor);

	const ulint		zip_size = index->table->space->zip_size();

	/* Start with the root page. */
	page_id_t		page_id(index->table->space_id, index->page);

	if (root_leaf_rw_latch == RW_X_LATCH) {
		node_ptr_max_size = btr_node_ptr_max_size(index);
	}

	up_match = 0;
	up_bytes = 0;
	low_match = 0;
	low_bytes = 0;

	height = ULINT_UNDEFINED;

	/* We use these modified search modes on non-leaf levels of the
	B-tree. These let us end up in the right B-tree leaf. In that leaf
	we use the original search mode. */

	switch (mode) {
	case PAGE_CUR_GE:
		page_mode = PAGE_CUR_L;
		break;
	case PAGE_CUR_G:
		page_mode = PAGE_CUR_LE;
		break;
	default:
#ifdef PAGE_CUR_LE_OR_EXTENDS
		ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
		      || RTREE_SEARCH_MODE(mode)
		      || mode == PAGE_CUR_LE_OR_EXTENDS);
#else /* PAGE_CUR_LE_OR_EXTENDS */
		ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
		      || RTREE_SEARCH_MODE(mode));
#endif /* PAGE_CUR_LE_OR_EXTENDS */
		page_mode = mode;
		break;
	}

	/* Loop and search until we arrive at the desired level */
	btr_latch_leaves_t latch_leaves = {{NULL, NULL, NULL}, {0, 0, 0}};

search_loop:
	buf_mode = BUF_GET;
	rw_latch = RW_NO_LATCH;
	rtree_parent_modified = false;

	if (height != 0) {
		/* We are about to fetch the root or a non-leaf page. */
		if ((latch_mode != BTR_MODIFY_TREE || height == level)
		    && !retrying_for_search_prev) {
			/* If doesn't have SX or X latch of index,
			each pages should be latched before reading. */
			if (height == ULINT_UNDEFINED
			    && upper_rw_latch == RW_S_LATCH
			    && (modify_external || autoinc)) {
				/* needs sx-latch of root page
				for fseg operation or for writing
				PAGE_ROOT_AUTO_INC */
				rw_latch = RW_SX_LATCH;
			} else {
				rw_latch = upper_rw_latch;
			}
		}
	} else if (latch_mode <= BTR_MODIFY_LEAF) {
		rw_latch = latch_mode;

		if (btr_op != BTR_NO_OP
		    && ibuf_should_try(index, btr_op != BTR_INSERT_OP)) {

			/* Try to buffer the operation if the leaf
			page is not in the buffer pool. */

			buf_mode = btr_op == BTR_DELETE_OP
				? BUF_GET_IF_IN_POOL_OR_WATCH
				: BUF_GET_IF_IN_POOL;
		}
	}

retry_page_get:
	ut_ad(n_blocks < BTR_MAX_LEVELS);
	tree_savepoints[n_blocks] = mtr_set_savepoint(mtr);
	block = buf_page_get_gen(page_id, zip_size, rw_latch, guess,
				 buf_mode, file, line, mtr, &err,
				 height == 0 && !index->is_clust());
	tree_blocks[n_blocks] = block;

	/* Note that block==NULL signifies either an error or change
	buffering. */

	if (err != DB_SUCCESS) {
		ut_ad(block == NULL);
		if (err == DB_DECRYPTION_FAILED) {
			ib_push_warning((void *)NULL,
				DB_DECRYPTION_FAILED,
				"Table %s is encrypted but encryption service or"
				" used key_id is not available. "
				" Can't continue reading table.",
				index->table->name.m_name);
			index->table->file_unreadable = true;
		}

		goto func_exit;
	}

	if (block == NULL) {
		/* This must be a search to perform an insert/delete
		mark/ delete; try using the insert/delete buffer */

		ut_ad(height == 0);
		ut_ad(cursor->thr);

		switch (btr_op) {
		case BTR_INSERT_OP:
		case BTR_INSERT_IGNORE_UNIQUE_OP:
			ut_ad(buf_mode == BUF_GET_IF_IN_POOL);
			ut_ad(!dict_index_is_spatial(index));

			if (ibuf_insert(IBUF_OP_INSERT, tuple, index,
					page_id, zip_size, cursor->thr)) {

				cursor->flag = BTR_CUR_INSERT_TO_IBUF;

				goto func_exit;
			}
			break;

		case BTR_DELMARK_OP:
			ut_ad(buf_mode == BUF_GET_IF_IN_POOL);
			ut_ad(!dict_index_is_spatial(index));

			if (ibuf_insert(IBUF_OP_DELETE_MARK, tuple,
					index, page_id, zip_size,
					cursor->thr)) {

				cursor->flag = BTR_CUR_DEL_MARK_IBUF;

				goto func_exit;
			}

			break;

		case BTR_DELETE_OP:
			ut_ad(buf_mode == BUF_GET_IF_IN_POOL_OR_WATCH);
			ut_ad(!dict_index_is_spatial(index));

			if (!row_purge_poss_sec(cursor->purge_node,
						index, tuple)) {

				/* The record cannot be purged yet. */
				cursor->flag = BTR_CUR_DELETE_REF;
			} else if (ibuf_insert(IBUF_OP_DELETE, tuple,
					       index, page_id, zip_size,
					       cursor->thr)) {

				/* The purge was buffered. */
				cursor->flag = BTR_CUR_DELETE_IBUF;
			} else {
				/* The purge could not be buffered. */
				buf_pool_watch_unset(page_id);
				break;
			}

			buf_pool_watch_unset(page_id);
			goto func_exit;

		default:
			ut_error;
		}

		/* Insert to the insert/delete buffer did not succeed, we
		must read the page from disk. */

		buf_mode = BUF_GET;

		goto retry_page_get;
	}

	if (retrying_for_search_prev && height != 0) {
		/* also latch left sibling */
		ulint		left_page_no;
		buf_block_t*	get_block;

		ut_ad(rw_latch == RW_NO_LATCH);

		rw_latch = upper_rw_latch;

		rw_lock_s_lock(&block->lock);
		left_page_no = btr_page_get_prev(buf_block_get_frame(block));
		rw_lock_s_unlock(&block->lock);

		if (left_page_no != FIL_NULL) {
			ut_ad(prev_n_blocks < leftmost_from_level);

			prev_tree_savepoints[prev_n_blocks]
				= mtr_set_savepoint(mtr);
			get_block = buf_page_get_gen(
				page_id_t(page_id.space(), left_page_no),
				zip_size, rw_latch, NULL, buf_mode,
				file, line, mtr, &err);
			prev_tree_blocks[prev_n_blocks] = get_block;
			prev_n_blocks++;

			if (err != DB_SUCCESS) {
				if (err == DB_DECRYPTION_FAILED) {
					ib_push_warning((void *)NULL,
						DB_DECRYPTION_FAILED,
						"Table %s is encrypted but encryption service or"
						" used key_id is not available. "
						" Can't continue reading table.",
						index->table->name.m_name);
					index->table->file_unreadable = true;
				}

				goto func_exit;
			}

			/* BTR_MODIFY_TREE doesn't update prev/next_page_no,
			without their parent page's lock. So, not needed to
			retry here, because we have the parent page's lock. */
		}

		/* release RW_NO_LATCH page and lock with RW_S_LATCH */
		mtr_release_block_at_savepoint(
			mtr, tree_savepoints[n_blocks],
			tree_blocks[n_blocks]);

		tree_savepoints[n_blocks] = mtr_set_savepoint(mtr);
		block = buf_page_get_gen(page_id, zip_size,
					 rw_latch, NULL, buf_mode,
					 file, line, mtr, &err);
		tree_blocks[n_blocks] = block;

		if (err != DB_SUCCESS) {
			if (err == DB_DECRYPTION_FAILED) {
				ib_push_warning((void *)NULL,
					DB_DECRYPTION_FAILED,
					"Table %s is encrypted but encryption service or"
					" used key_id is not available. "
					" Can't continue reading table.",
					index->table->name.m_name);
				index->table->file_unreadable = true;
			}

			goto func_exit;
		}
	}

	page = buf_block_get_frame(block);

	if (height == ULINT_UNDEFINED
	    && page_is_leaf(page)
	    && rw_latch != RW_NO_LATCH
	    && rw_latch != root_leaf_rw_latch) {
		/* The root page is also a leaf page (root_leaf).
		We should reacquire the page, because the root page
		is latched differently from leaf pages. */
		ut_ad(root_leaf_rw_latch != RW_NO_LATCH);
		ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_SX_LATCH);
		ut_ad(rw_latch == RW_S_LATCH || modify_external || autoinc);
		ut_ad(!autoinc || root_leaf_rw_latch == RW_X_LATCH);

		ut_ad(n_blocks == 0);
		mtr_release_block_at_savepoint(
			mtr, tree_savepoints[n_blocks],
			tree_blocks[n_blocks]);

		upper_rw_latch = root_leaf_rw_latch;
		goto search_loop;
	}

	if (rw_latch != RW_NO_LATCH) {
#ifdef UNIV_ZIP_DEBUG
		const page_zip_des_t*	page_zip
			= buf_block_get_page_zip(block);
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

		buf_block_dbg_add_level(
			block, dict_index_is_ibuf(index)
			? SYNC_IBUF_TREE_NODE : SYNC_TREE_NODE);
	}

	ut_ad(fil_page_index_page_check(page));
	ut_ad(index->id == btr_page_get_index_id(page));

	if (height == ULINT_UNDEFINED) {
		/* We are in the root node */

		height = btr_page_get_level(page);
		root_height = height;
		cursor->tree_height = root_height + 1;

		if (dict_index_is_spatial(index)) {
			ut_ad(cursor->rtr_info);

			node_seq_t      seq_no = rtr_get_current_ssn_id(index);

			/* If SSN in memory is not initialized, fetch
			it from root page */
			if (seq_no < 1) {
				node_seq_t      root_seq_no;

				root_seq_no = page_get_ssn_id(page);

				mutex_enter(&(index->rtr_ssn.mutex));
				index->rtr_ssn.seq_no = root_seq_no + 1;
				mutex_exit(&(index->rtr_ssn.mutex));
			}

			/* Save the MBR */
			cursor->rtr_info->thr = cursor->thr;
			rtr_get_mbr_from_tuple(tuple, &cursor->rtr_info->mbr);
		}

#ifdef BTR_CUR_ADAPT
		if (block != guess) {
			info->root_guess = block;
			info->withdraw_clock = buf_pool.withdraw_clock();
		}
#endif
	}

	if (height == 0) {
		if (rw_latch == RW_NO_LATCH) {
			latch_leaves = btr_cur_latch_leaves(
				block, latch_mode, cursor, mtr);
		}

		switch (latch_mode) {
		case BTR_MODIFY_TREE:
		case BTR_CONT_MODIFY_TREE:
		case BTR_CONT_SEARCH_TREE:
			break;
		default:
			if (!s_latch_by_caller
			    && !srv_read_only_mode
			    && !modify_external) {
				/* Release the tree s-latch */
				/* NOTE: BTR_MODIFY_EXTERNAL
				needs to keep tree sx-latch */
				mtr_release_s_latch_at_savepoint(
					mtr, savepoint,
					dict_index_get_lock(index));
			}

			/* release upper blocks */
			if (retrying_for_search_prev) {
				ut_ad(!autoinc);
				for (;
				     prev_n_releases < prev_n_blocks;
				     prev_n_releases++) {
					mtr_release_block_at_savepoint(
						mtr,
						prev_tree_savepoints[
							prev_n_releases],
						prev_tree_blocks[
							prev_n_releases]);
				}
			}

			for (; n_releases < n_blocks; n_releases++) {
				if (n_releases == 0
				    && (modify_external || autoinc)) {
					/* keep the root page latch */
					ut_ad(mtr_memo_contains_flagged(
						mtr, tree_blocks[n_releases],
						MTR_MEMO_PAGE_SX_FIX
						| MTR_MEMO_PAGE_X_FIX));
					continue;
				}

				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}
		}

		page_mode = mode;
	}

	if (dict_index_is_spatial(index)) {
		/* Remember the page search mode */
		search_mode = page_mode;

		/* Some adjustment on search mode, when the
		page search mode is PAGE_CUR_RTREE_LOCATE
		or PAGE_CUR_RTREE_INSERT, as we are searching
		with MBRs. When it is not the target level, we
		should search all sub-trees that "CONTAIN" the
		search range/MBR. When it is at the target
		level, the search becomes PAGE_CUR_LE */
		if (page_mode == PAGE_CUR_RTREE_LOCATE
		    && level == height) {
			if (level == 0) {
				page_mode = PAGE_CUR_LE;
			} else {
				page_mode = PAGE_CUR_RTREE_GET_FATHER;
			}
		}

		if (page_mode == PAGE_CUR_RTREE_INSERT) {
			page_mode = (level == height)
					? PAGE_CUR_LE
					: PAGE_CUR_RTREE_INSERT;

			ut_ad(!page_is_leaf(page) || page_mode == PAGE_CUR_LE);
		}

		/* "need_path" indicates if we need to tracking the parent
		pages, if it is not spatial comparison, then no need to
		track it */
		if (page_mode < PAGE_CUR_CONTAIN) {
			need_path = false;
		}

		up_match = 0;
		low_match = 0;

		if (latch_mode == BTR_MODIFY_TREE
		    || latch_mode == BTR_CONT_MODIFY_TREE
		    || latch_mode == BTR_CONT_SEARCH_TREE) {
			/* Tree are locked, no need for Page Lock to protect
			the "path" */
			cursor->rtr_info->need_page_lock = false;
		}
        }

	if (dict_index_is_spatial(index) && page_mode >= PAGE_CUR_CONTAIN) {
		ut_ad(need_path);
		found = rtr_cur_search_with_match(
			block, index, tuple, page_mode, page_cursor,
			cursor->rtr_info);

		/* Need to use BTR_MODIFY_TREE to do the MBR adjustment */
		if (search_mode == PAGE_CUR_RTREE_INSERT
		    && cursor->rtr_info->mbr_adj) {
			if (latch_mode & BTR_MODIFY_LEAF) {
				/* Parent MBR needs updated, should retry
				with BTR_MODIFY_TREE */
				goto func_exit;
			} else if (latch_mode & BTR_MODIFY_TREE) {
				rtree_parent_modified = true;
				cursor->rtr_info->mbr_adj = false;
				mbr_adj = true;
			} else {
				ut_ad(0);
			}
		}

		if (found && page_mode == PAGE_CUR_RTREE_GET_FATHER) {
			cursor->low_match =
				DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1;
		}
#ifdef BTR_CUR_HASH_ADAPT
	} else if (height == 0 && btr_search_enabled
		   && !(tuple->info_bits & REC_INFO_MIN_REC_FLAG)
		   && !dict_index_is_spatial(index)) {
		/* The adaptive hash index is only used when searching
		for leaf pages (height==0), but not in r-trees.
		We only need the byte prefix comparison for the purpose
		of updating the adaptive hash index. */
		page_cur_search_with_match_bytes(
			block, index, tuple, page_mode, &up_match, &up_bytes,
			&low_match, &low_bytes, page_cursor);
#endif /* BTR_CUR_HASH_ADAPT */
	} else {
		/* Search for complete index fields. */
		up_bytes = low_bytes = 0;
		page_cur_search_with_match(
			block, index, tuple, page_mode, &up_match,
			&low_match, page_cursor,
			need_path ? cursor->rtr_info : NULL);
	}

	if (estimate) {
		btr_cur_add_path_info(cursor, height, root_height);
	}

	/* If this is the desired level, leave the loop */

	ut_ad(height == btr_page_get_level(page_cur_get_page(page_cursor)));

	/* Add Predicate lock if it is serializable isolation
	and only if it is in the search case */
	if (dict_index_is_spatial(index)
	    && cursor->rtr_info->need_prdt_lock
	    && mode != PAGE_CUR_RTREE_INSERT
	    && mode != PAGE_CUR_RTREE_LOCATE
	    && mode >= PAGE_CUR_CONTAIN) {
		trx_t*		trx = thr_get_trx(cursor->thr);
		lock_prdt_t	prdt;

		lock_mutex_enter();
		lock_init_prdt_from_mbr(
			&prdt, &cursor->rtr_info->mbr, mode,
			trx->lock.lock_heap);
		lock_mutex_exit();

		if (rw_latch == RW_NO_LATCH && height != 0) {
			rw_lock_s_lock(&(block->lock));
		}

		lock_prdt_lock(block, &prdt, index, LOCK_S,
			       LOCK_PREDICATE, cursor->thr);

		if (rw_latch == RW_NO_LATCH && height != 0) {
			rw_lock_s_unlock(&(block->lock));
		}
	}

	if (level != height) {

		const rec_t*	node_ptr;
		ut_ad(height > 0);

		height--;
		guess = NULL;

		node_ptr = page_cur_get_rec(page_cursor);

		offsets = rec_get_offsets(node_ptr, index, offsets, false,
					  ULINT_UNDEFINED, &heap);

		/* If the rec is the first or last in the page for
		pessimistic delete intention, it might cause node_ptr insert
		for the upper level. We should change the intention and retry.
		*/
		if (latch_mode == BTR_MODIFY_TREE
		    && btr_cur_need_opposite_intention(
			page, lock_intention, node_ptr)) {

need_opposite_intention:
			ut_ad(upper_rw_latch == RW_X_LATCH);

			if (n_releases > 0) {
				/* release root block */
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[0],
					tree_blocks[0]);
			}

			/* release all blocks */
			for (; n_releases <= n_blocks; n_releases++) {
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}

			lock_intention = BTR_INTENTION_BOTH;

			page_id.set_page_no(index->page);
			up_match = 0;
			low_match = 0;
			height = ULINT_UNDEFINED;

			n_blocks = 0;
			n_releases = 0;

			goto search_loop;
		}

		if (dict_index_is_spatial(index)) {
			if (page_rec_is_supremum(node_ptr)) {
				cursor->low_match = 0;
				cursor->up_match = 0;
				goto func_exit;
			}

			/* If we are doing insertion or record locating,
			remember the tree nodes we visited */
			if (page_mode == PAGE_CUR_RTREE_INSERT
			    || (search_mode == PAGE_CUR_RTREE_LOCATE
			        && (latch_mode != BTR_MODIFY_LEAF))) {
				bool		add_latch = false;

				if (latch_mode == BTR_MODIFY_TREE
				    && rw_latch == RW_NO_LATCH) {
					ut_ad(mtr_memo_contains_flagged(
						mtr, dict_index_get_lock(index),
						MTR_MEMO_X_LOCK
						| MTR_MEMO_SX_LOCK));
					rw_lock_s_lock(&block->lock);
					add_latch = true;
				}

				/* Store the parent cursor location */
#ifdef UNIV_DEBUG
				ulint	num_stored = rtr_store_parent_path(
					block, cursor, latch_mode,
					height + 1, mtr);
#else
				rtr_store_parent_path(
					block, cursor, latch_mode,
					height + 1, mtr);
#endif

				if (page_mode == PAGE_CUR_RTREE_INSERT) {
					btr_pcur_t*     r_cursor =
						rtr_get_parent_cursor(
							cursor, height + 1,
							true);
					/* If it is insertion, there should
					be only one parent for each level
					traverse */
#ifdef UNIV_DEBUG
					ut_ad(num_stored == 1);
#endif

					node_ptr = btr_pcur_get_rec(r_cursor);

				}

				if (add_latch) {
					rw_lock_s_unlock(&block->lock);
				}

				ut_ad(!page_rec_is_supremum(node_ptr));
			}

			ut_ad(page_mode == search_mode
			      || (page_mode == PAGE_CUR_WITHIN
				  && search_mode == PAGE_CUR_RTREE_LOCATE));

			page_mode = search_mode;
		}

		/* If the first or the last record of the page
		or the same key value to the first record or last record,
		the another page might be choosen when BTR_CONT_MODIFY_TREE.
		So, the parent page should not released to avoiding deadlock
		with blocking the another search with the same key value. */
		if (!detected_same_key_root
		    && lock_intention == BTR_INTENTION_BOTH
		    && !dict_index_is_unique(index)
		    && latch_mode == BTR_MODIFY_TREE
		    && (up_match >= rec_offs_n_fields(offsets) - 1
			|| low_match >= rec_offs_n_fields(offsets) - 1)) {
			const rec_t*	first_rec = page_rec_get_next_const(
				page_get_infimum_rec(page));
			ulint		matched_fields;

			ut_ad(upper_rw_latch == RW_X_LATCH);

			if (node_ptr == first_rec
			    || page_rec_is_last(node_ptr, page)) {
				detected_same_key_root = true;
			} else {
				matched_fields = 0;

				offsets2 = rec_get_offsets(
					first_rec, index, offsets2,
					false, ULINT_UNDEFINED, &heap);
				cmp_rec_rec(node_ptr, first_rec,
					    offsets, offsets2, index, false,
					    &matched_fields);

				if (matched_fields
				    >= rec_offs_n_fields(offsets) - 1) {
					detected_same_key_root = true;
				} else {
					const rec_t*	last_rec;

					last_rec = page_rec_get_prev_const(
						page_get_supremum_rec(page));

					matched_fields = 0;

					offsets2 = rec_get_offsets(
						last_rec, index, offsets2,
						false, ULINT_UNDEFINED, &heap);
					cmp_rec_rec(
						node_ptr, last_rec,
						offsets, offsets2, index,
						false, &matched_fields);
					if (matched_fields
					    >= rec_offs_n_fields(offsets) - 1) {
						detected_same_key_root = true;
					}
				}
			}
		}

		/* If the page might cause modify_tree,
		we should not release the parent page's lock. */
		if (!detected_same_key_root
		    && latch_mode == BTR_MODIFY_TREE
		    && !btr_cur_will_modify_tree(
				index, page, lock_intention, node_ptr,
				node_ptr_max_size, zip_size, mtr)
		    && !rtree_parent_modified) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			ut_ad(n_releases <= n_blocks);

			/* we can release upper blocks */
			for (; n_releases < n_blocks; n_releases++) {
				if (n_releases == 0) {
					/* we should not release root page
					to pin to same block. */
					continue;
				}

				/* release unused blocks to unpin */
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}
		}

		if (height == level
		    && latch_mode == BTR_MODIFY_TREE) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			/* we should sx-latch root page, if released already.
			It contains seg_header. */
			if (n_releases > 0) {
				mtr_block_sx_latch_at_savepoint(
					mtr, tree_savepoints[0],
					tree_blocks[0]);
			}

			/* x-latch the branch blocks not released yet. */
			for (ulint i = n_releases; i <= n_blocks; i++) {
				mtr_block_x_latch_at_savepoint(
					mtr, tree_savepoints[i],
					tree_blocks[i]);
			}
		}

		/* We should consider prev_page of parent page, if the node_ptr
		is the leftmost of the page. because BTR_SEARCH_PREV and
		BTR_MODIFY_PREV latches prev_page of the leaf page. */
		if ((latch_mode == BTR_SEARCH_PREV
		     || latch_mode == BTR_MODIFY_PREV)
		    && !retrying_for_search_prev) {
			/* block should be latched for consistent
			   btr_page_get_prev() */
			ut_ad(mtr_memo_contains_flagged(mtr, block,
				MTR_MEMO_PAGE_S_FIX
				| MTR_MEMO_PAGE_X_FIX));

			if (page_has_prev(page)
			    && page_rec_is_first(node_ptr, page)) {

				if (leftmost_from_level == 0) {
					leftmost_from_level = height + 1;
				}
			} else {
				leftmost_from_level = 0;
			}

			if (height == 0 && leftmost_from_level > 0) {
				/* should retry to get also prev_page
				from level==leftmost_from_level. */
				retrying_for_search_prev = true;

				prev_tree_blocks = static_cast<buf_block_t**>(
					ut_malloc_nokey(sizeof(buf_block_t*)
							* leftmost_from_level));

				prev_tree_savepoints = static_cast<ulint*>(
					ut_malloc_nokey(sizeof(ulint)
							* leftmost_from_level));

				/* back to the level (leftmost_from_level+1) */
				ulint	idx = n_blocks
					- (leftmost_from_level - 1);

				page_id.set_page_no(
					tree_blocks[idx]->page.id.page_no());

				for (ulint i = n_blocks
					       - (leftmost_from_level - 1);
				     i <= n_blocks; i++) {
					mtr_release_block_at_savepoint(
						mtr, tree_savepoints[i],
						tree_blocks[i]);
				}

				n_blocks -= (leftmost_from_level - 1);
				height = leftmost_from_level;
				ut_ad(n_releases == 0);

				/* replay up_match, low_match */
				up_match = 0;
				low_match = 0;
				rtr_info_t*	rtr_info	= need_path
					? cursor->rtr_info : NULL;

				for (ulint i = 0; i < n_blocks; i++) {
					page_cur_search_with_match(
						tree_blocks[i], index, tuple,
						page_mode, &up_match,
						&low_match, page_cursor,
						rtr_info);
				}

				goto search_loop;
			}
		}

		/* Go to the child node */
		page_id.set_page_no(
			btr_node_ptr_get_child_page_no(node_ptr, offsets));

		n_blocks++;

		if (UNIV_UNLIKELY(height == 0 && dict_index_is_ibuf(index))) {
			/* We're doing a search on an ibuf tree and we're one
			level above the leaf page. */

			ut_ad(level == 0);

			buf_mode = BUF_GET;
			rw_latch = RW_NO_LATCH;
			goto retry_page_get;
		}

		if (dict_index_is_spatial(index)
		    && page_mode >= PAGE_CUR_CONTAIN
		    && page_mode != PAGE_CUR_RTREE_INSERT) {
			ut_ad(need_path);
			rtr_node_path_t* path =
				cursor->rtr_info->path;

			if (!path->empty() && found) {
				ut_ad(path->back().page_no
				      == page_id.page_no());
				path->pop_back();
#ifdef UNIV_DEBUG
				if (page_mode == PAGE_CUR_RTREE_LOCATE
				    && (latch_mode != BTR_MODIFY_LEAF)) {
					btr_pcur_t*	cur
					= cursor->rtr_info->parent_path->back(
					  ).cursor;
					rec_t*	my_node_ptr
						= btr_pcur_get_rec(cur);

					offsets = rec_get_offsets(
						my_node_ptr, index, offsets,
						false, ULINT_UNDEFINED, &heap);

					ulint	my_page_no
					= btr_node_ptr_get_child_page_no(
						my_node_ptr, offsets);

					ut_ad(page_id.page_no() == my_page_no);
				}
#endif
			}
		}

		goto search_loop;
	} else if (!dict_index_is_spatial(index)
		   && latch_mode == BTR_MODIFY_TREE
		   && lock_intention == BTR_INTENTION_INSERT
		   && page_has_next(page)
		   && page_rec_is_last(page_cur_get_rec(page_cursor), page)) {

		/* btr_insert_into_right_sibling() might cause
		deleting node_ptr at upper level */

		guess = NULL;

		if (height == 0) {
			/* release the leaf pages if latched */
			for (uint i = 0; i < 3; i++) {
				if (latch_leaves.blocks[i] != NULL) {
					mtr_release_block_at_savepoint(
						mtr, latch_leaves.savepoints[i],
						latch_leaves.blocks[i]);
					latch_leaves.blocks[i] = NULL;
				}
			}
		}

		goto need_opposite_intention;
	}

	if (level != 0) {
		ut_ad(!autoinc);

		if (upper_rw_latch == RW_NO_LATCH) {
			ut_ad(latch_mode == BTR_CONT_MODIFY_TREE
			      || latch_mode == BTR_CONT_SEARCH_TREE);
			buf_block_t* child_block = btr_block_get(
				*index, page_id.page_no(),
				latch_mode == BTR_CONT_MODIFY_TREE
				? RW_X_LATCH : RW_SX_LATCH, false, mtr);
			btr_assert_not_corrupted(child_block, index);
		} else {
			ut_ad(mtr_memo_contains(mtr, block,
						static_cast<mtr_memo_type_t>
						(upper_rw_latch)));
			btr_assert_not_corrupted(block, index);

			if (s_latch_by_caller) {
				ut_ad(latch_mode == BTR_SEARCH_TREE);
				/* to exclude modifying tree operations
				should sx-latch the index. */
				ut_ad(mtr_memo_contains(
					mtr, dict_index_get_lock(index),
					MTR_MEMO_SX_LOCK));
				/* because has sx-latch of index,
				can release upper blocks. */
				for (; n_releases < n_blocks; n_releases++) {
					mtr_release_block_at_savepoint(
						mtr,
						tree_savepoints[n_releases],
						tree_blocks[n_releases]);
				}
			}
		}

		if (page_mode <= PAGE_CUR_LE) {
			cursor->low_match = low_match;
			cursor->up_match = up_match;
		}
	} else {
		cursor->low_match = low_match;
		cursor->low_bytes = low_bytes;
		cursor->up_match = up_match;
		cursor->up_bytes = up_bytes;

		if (autoinc) {
			page_set_autoinc(tree_blocks[0], autoinc, mtr, false);
		}

#ifdef BTR_CUR_HASH_ADAPT
		/* We do a dirty read of btr_search_enabled here.  We
		will properly check btr_search_enabled again in
		btr_search_build_page_hash_index() before building a
		page hash index, while holding search latch. */
		if (!btr_search_enabled) {
# ifdef MYSQL_INDEX_DISABLE_AHI
		} else if (index->disable_ahi) {
# endif
		} else if (tuple->info_bits & REC_INFO_MIN_REC_FLAG) {
			ut_ad(index->is_instant());
			/* This may be a search tuple for
			btr_pcur_restore_position(). */
			ut_ad(tuple->is_metadata()
			      || (tuple->is_metadata(tuple->info_bits
						     ^ REC_STATUS_INSTANT)));
		} else if (rec_is_metadata(btr_cur_get_rec(cursor), *index)) {
			/* Only user records belong in the adaptive
			hash index. */
		} else {
			btr_search_info_update(index, cursor);
		}
#endif /* BTR_CUR_HASH_ADAPT */
		ut_ad(cursor->up_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_GE);
		ut_ad(cursor->up_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_LE);
		ut_ad(cursor->low_match != ULINT_UNDEFINED
		      || mode != PAGE_CUR_LE);
	}

	/* For spatial index, remember  what blocks are still latched */
	if (dict_index_is_spatial(index)
	    && (latch_mode == BTR_MODIFY_TREE
		|| latch_mode == BTR_MODIFY_LEAF)) {
		for (ulint i = 0; i < n_releases; i++) {
			cursor->rtr_info->tree_blocks[i] = NULL;
			cursor->rtr_info->tree_savepoints[i] = 0;
		}

		for (ulint i = n_releases; i <= n_blocks; i++) {
			cursor->rtr_info->tree_blocks[i] = tree_blocks[i];
			cursor->rtr_info->tree_savepoints[i] = tree_savepoints[i];
		}
	}

func_exit:

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	if (retrying_for_search_prev) {
		ut_free(prev_tree_blocks);
		ut_free(prev_tree_savepoints);
	}

	if (mbr_adj) {
		/* remember that we will need to adjust parent MBR */
		cursor->rtr_info->mbr_adj = true;
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (ahi_latch) {
		rw_lock_s_lock(ahi_latch);
	}
#endif /* BTR_CUR_HASH_ADAPT */

	DBUG_RETURN(err);
}

/*****************************************************************//**
Opens a cursor at either end of an index. */
dberr_t
btr_cur_open_at_index_side_func(
/*============================*/
	bool		from_left,	/*!< in: true if open to the low end,
					false if to the high end */
	dict_index_t*	index,		/*!< in: index */
	ulint		latch_mode,	/*!< in: latch mode */
	btr_cur_t*	cursor,		/*!< in/out: cursor */
	ulint		level,		/*!< in: level to search for
					(0=leaf). */
	const char*	file,		/*!< in: file name */
	unsigned	line,		/*!< in: line where called */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	page_cur_t*	page_cursor;
	ulint		node_ptr_max_size = srv_page_size / 2;
	ulint		height;
	ulint		root_height = 0; /* remove warning */
	rec_t*		node_ptr;
	ulint		estimate;
	btr_intention_t	lock_intention;
	buf_block_t*	tree_blocks[BTR_MAX_LEVELS];
	ulint		tree_savepoints[BTR_MAX_LEVELS];
	ulint		n_blocks = 0;
	ulint		n_releases = 0;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	dberr_t		err = DB_SUCCESS;

	rec_offs_init(offsets_);

	estimate = latch_mode & BTR_ESTIMATE;
	latch_mode &= ulint(~BTR_ESTIMATE);

	ut_ad(level != ULINT_UNDEFINED);

	bool	s_latch_by_caller;

	s_latch_by_caller = latch_mode & BTR_ALREADY_S_LATCHED;
	latch_mode &= ulint(~BTR_ALREADY_S_LATCHED);

	lock_intention = btr_cur_get_and_clear_intention(&latch_mode);

	ut_ad(!(latch_mode & BTR_MODIFY_EXTERNAL));

	/* This function doesn't need to lock left page of the leaf page */
	if (latch_mode == BTR_SEARCH_PREV) {
		latch_mode = BTR_SEARCH_LEAF;
	} else if (latch_mode == BTR_MODIFY_PREV) {
		latch_mode = BTR_MODIFY_LEAF;
	}

	/* Store the position of the tree latch we push to mtr so that we
	know how to release it when we have latched the leaf node */

	ulint savepoint = mtr_set_savepoint(mtr);

	rw_lock_type_t upper_rw_latch;

	switch (latch_mode) {
	case BTR_CONT_MODIFY_TREE:
	case BTR_CONT_SEARCH_TREE:
		upper_rw_latch = RW_NO_LATCH;
		break;
	case BTR_MODIFY_TREE:
		/* Most of delete-intended operations are purging.
		Free blocks and read IO bandwidth should be prior
		for them, when the history list is glowing huge. */
		if (lock_intention == BTR_INTENTION_DELETE
		    && trx_sys.rseg_history_len > BTR_CUR_FINE_HISTORY_LENGTH
		    && buf_pool.n_pend_reads) {
			mtr_x_lock_index(index, mtr);
		} else {
			mtr_sx_lock_index(index, mtr);
		}
		upper_rw_latch = RW_X_LATCH;
		break;
	default:
		ut_ad(!s_latch_by_caller
		      || mtr_memo_contains_flagged(mtr,
						 dict_index_get_lock(index),
						 MTR_MEMO_SX_LOCK
						 | MTR_MEMO_S_LOCK));
		if (!srv_read_only_mode) {
			if (!s_latch_by_caller) {
				/* BTR_SEARCH_TREE is intended to be used with
				BTR_ALREADY_S_LATCHED */
				ut_ad(latch_mode != BTR_SEARCH_TREE);

				mtr_s_lock_index(index, mtr);
			}
			upper_rw_latch = RW_S_LATCH;
		} else {
			upper_rw_latch = RW_NO_LATCH;
		}
	}

	const rw_lock_type_t root_leaf_rw_latch = btr_cur_latch_for_root_leaf(
		latch_mode);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	page_id_t		page_id(index->table->space_id, index->page);
	const ulint		zip_size = index->table->space->zip_size();

	if (root_leaf_rw_latch == RW_X_LATCH) {
		node_ptr_max_size = btr_node_ptr_max_size(index);
	}

	height = ULINT_UNDEFINED;

	for (;;) {
		ut_ad(n_blocks < BTR_MAX_LEVELS);
		tree_savepoints[n_blocks] = mtr_set_savepoint(mtr);

		const ulint rw_latch = height
			&& (latch_mode != BTR_MODIFY_TREE || height == level)
			? upper_rw_latch : RW_NO_LATCH;
		buf_block_t* block = buf_page_get_gen(page_id, zip_size,
						      rw_latch, NULL, BUF_GET,
						      file, line, mtr, &err,
						      height == 0
						      && !index->is_clust());
		ut_ad((block != NULL) == (err == DB_SUCCESS));
		tree_blocks[n_blocks] = block;

		if (err != DB_SUCCESS) {
			if (err == DB_DECRYPTION_FAILED) {
				ib_push_warning((void *)NULL,
					DB_DECRYPTION_FAILED,
					"Table %s is encrypted but encryption service or"
					" used key_id is not available. "
					" Can't continue reading table.",
					index->table->name.m_name);
				index->table->file_unreadable = true;
			}

			goto exit_loop;
		}

		const page_t* page = buf_block_get_frame(block);

		if (height == ULINT_UNDEFINED
		    && page_is_leaf(page)
		    && rw_latch != RW_NO_LATCH
		    && rw_latch != root_leaf_rw_latch) {
			/* We should retry to get the page, because the root page
			is latched with different level as a leaf page. */
			ut_ad(root_leaf_rw_latch != RW_NO_LATCH);
			ut_ad(rw_latch == RW_S_LATCH);

			ut_ad(n_blocks == 0);
			mtr_release_block_at_savepoint(
				mtr, tree_savepoints[n_blocks],
				tree_blocks[n_blocks]);

			upper_rw_latch = root_leaf_rw_latch;
			continue;
		}

		ut_ad(fil_page_index_page_check(page));
		ut_ad(index->id == btr_page_get_index_id(page));

		if (height == ULINT_UNDEFINED) {
			/* We are in the root node */

			height = btr_page_get_level(page);
			root_height = height;
			ut_a(height >= level);
		} else {
			/* TODO: flag the index corrupted if this fails */
			ut_ad(height == btr_page_get_level(page));
		}

		if (height == 0) {
			if (rw_latch == RW_NO_LATCH) {
				btr_cur_latch_leaves(block, latch_mode,
						     cursor, mtr);
			}

			/* In versions <= 3.23.52 we had forgotten to
			release the tree latch here. If in an index
			scan we had to scan far to find a record
			visible to the current transaction, that could
			starve others waiting for the tree latch. */

			switch (latch_mode) {
			case BTR_MODIFY_TREE:
			case BTR_CONT_MODIFY_TREE:
			case BTR_CONT_SEARCH_TREE:
				break;
			default:
				if (UNIV_UNLIKELY(srv_read_only_mode)) {
					break;
				}
				if (!s_latch_by_caller) {
					/* Release the tree s-latch */
					mtr_release_s_latch_at_savepoint(
						mtr, savepoint, &index->lock);
				}

				/* release upper blocks */
				for (; n_releases < n_blocks; n_releases++) {
					mtr_release_block_at_savepoint(
						mtr,
						tree_savepoints[n_releases],
						tree_blocks[n_releases]);
				}
			}
		} else if (height == level /* height != 0 */
			   && UNIV_LIKELY(!srv_read_only_mode)) {
			/* We already have the block latched. */
			ut_ad(latch_mode == BTR_SEARCH_TREE);
			ut_ad(s_latch_by_caller);
			ut_ad(upper_rw_latch == RW_S_LATCH);
			ut_ad(mtr_memo_contains(mtr, block,
						MTR_MEMO_PAGE_S_FIX));

			if (s_latch_by_caller) {
				/* to exclude modifying tree operations
				should sx-latch the index. */
				ut_ad(mtr_memo_contains(mtr, &index->lock,
							MTR_MEMO_SX_LOCK));
				/* because has sx-latch of index,
				can release upper blocks. */
				for (; n_releases < n_blocks; n_releases++) {
					mtr_release_block_at_savepoint(
						mtr,
						tree_savepoints[n_releases],
						tree_blocks[n_releases]);
				}
			}
		}

		if (from_left) {
			page_cur_set_before_first(block, page_cursor);
		} else {
			page_cur_set_after_last(block, page_cursor);
		}

		if (height == level) {
			if (estimate) {
				btr_cur_add_path_info(cursor, height,
						      root_height);
			}

			break;
		}

		ut_ad(height > 0);

		if (from_left) {
			page_cur_move_to_next(page_cursor);
		} else {
			page_cur_move_to_prev(page_cursor);
		}

		if (estimate) {
			btr_cur_add_path_info(cursor, height, root_height);
		}

		height--;

		node_ptr = page_cur_get_rec(page_cursor);
		offsets = rec_get_offsets(node_ptr, cursor->index, offsets,
					  false, ULINT_UNDEFINED, &heap);

		/* If the rec is the first or last in the page for
		pessimistic delete intention, it might cause node_ptr insert
		for the upper level. We should change the intention and retry.
		*/
		if (latch_mode == BTR_MODIFY_TREE
		    && btr_cur_need_opposite_intention(
			page, lock_intention, node_ptr)) {

			ut_ad(upper_rw_latch == RW_X_LATCH);
			/* release all blocks */
			for (; n_releases <= n_blocks; n_releases++) {
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}

			lock_intention = BTR_INTENTION_BOTH;

			page_id.set_page_no(dict_index_get_page(index));

			height = ULINT_UNDEFINED;

			n_blocks = 0;
			n_releases = 0;

			continue;
		}

		if (latch_mode == BTR_MODIFY_TREE
		    && !btr_cur_will_modify_tree(
				cursor->index, page, lock_intention, node_ptr,
				node_ptr_max_size, zip_size, mtr)) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			ut_ad(n_releases <= n_blocks);

			/* we can release upper blocks */
			for (; n_releases < n_blocks; n_releases++) {
				if (n_releases == 0) {
					/* we should not release root page
					to pin to same block. */
					continue;
				}

				/* release unused blocks to unpin */
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}
		}

		if (height == level
		    && latch_mode == BTR_MODIFY_TREE) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			/* we should sx-latch root page, if released already.
			It contains seg_header. */
			if (n_releases > 0) {
				mtr_block_sx_latch_at_savepoint(
					mtr, tree_savepoints[0],
					tree_blocks[0]);
			}

			/* x-latch the branch blocks not released yet. */
			for (ulint i = n_releases; i <= n_blocks; i++) {
				mtr_block_x_latch_at_savepoint(
					mtr, tree_savepoints[i],
					tree_blocks[i]);
			}
		}

		/* Go to the child node */
		page_id.set_page_no(
			btr_node_ptr_get_child_page_no(node_ptr, offsets));

		n_blocks++;
	}

 exit_loop:
	if (heap) {
		mem_heap_free(heap);
	}

	return err;
}

/**********************************************************************//**
Positions a cursor at a randomly chosen position within a B-tree.
@return true if the index is available and we have put the cursor, false
if the index is unavailable */
bool
btr_cur_open_at_rnd_pos_func(
/*=========================*/
	dict_index_t*	index,		/*!< in: index */
	ulint		latch_mode,	/*!< in: BTR_SEARCH_LEAF, ... */
	btr_cur_t*	cursor,		/*!< in/out: B-tree cursor */
	const char*	file,		/*!< in: file name */
	unsigned	line,		/*!< in: line where called */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_cur_t*	page_cursor;
	ulint		node_ptr_max_size = srv_page_size / 2;
	ulint		height;
	rec_t*		node_ptr;
	btr_intention_t	lock_intention;
	buf_block_t*	tree_blocks[BTR_MAX_LEVELS];
	ulint		tree_savepoints[BTR_MAX_LEVELS];
	ulint		n_blocks = 0;
	ulint		n_releases = 0;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(!index->is_spatial());

	lock_intention = btr_cur_get_and_clear_intention(&latch_mode);

	ut_ad(!(latch_mode & BTR_MODIFY_EXTERNAL));

	ulint savepoint = mtr_set_savepoint(mtr);

	rw_lock_type_t upper_rw_latch;

	switch (latch_mode) {
	case BTR_MODIFY_TREE:
		/* Most of delete-intended operations are purging.
		Free blocks and read IO bandwidth should be prior
		for them, when the history list is glowing huge. */
		if (lock_intention == BTR_INTENTION_DELETE
		    && trx_sys.rseg_history_len > BTR_CUR_FINE_HISTORY_LENGTH
		    && buf_pool.n_pend_reads) {
			mtr_x_lock_index(index, mtr);
		} else {
			mtr_sx_lock_index(index, mtr);
		}
		upper_rw_latch = RW_X_LATCH;
		break;
	case BTR_SEARCH_PREV:
	case BTR_MODIFY_PREV:
		/* This function doesn't support left uncle
		   page lock for left leaf page lock, when
		   needed. */
	case BTR_SEARCH_TREE:
	case BTR_CONT_MODIFY_TREE:
	case BTR_CONT_SEARCH_TREE:
		ut_ad(0);
		/* fall through */
	default:
		if (!srv_read_only_mode) {
			mtr_s_lock_index(index, mtr);
			upper_rw_latch = RW_S_LATCH;
		} else {
			upper_rw_latch = RW_NO_LATCH;
		}
	}

	DBUG_EXECUTE_IF("test_index_is_unavailable",
			return(false););

	if (index->page == FIL_NULL) {
		/* Since we don't hold index lock until just now, the index
		could be modified by others, for example, if this is a
		statistics updater for referenced table, it could be marked
		as unavailable by 'DROP TABLE' in the mean time, since
		we don't hold lock for statistics updater */
		return(false);
	}

	const rw_lock_type_t root_leaf_rw_latch = btr_cur_latch_for_root_leaf(
		latch_mode);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	page_id_t		page_id(index->table->space_id, index->page);
	const ulint		zip_size = index->table->space->zip_size();
	dberr_t			err = DB_SUCCESS;

	if (root_leaf_rw_latch == RW_X_LATCH) {
		node_ptr_max_size = btr_node_ptr_max_size(index);
	}

	height = ULINT_UNDEFINED;

	for (;;) {
		page_t*		page;

		ut_ad(n_blocks < BTR_MAX_LEVELS);
		tree_savepoints[n_blocks] = mtr_set_savepoint(mtr);

		const rw_lock_type_t rw_latch = height
			&& latch_mode != BTR_MODIFY_TREE
			? upper_rw_latch : RW_NO_LATCH;
		buf_block_t* block = buf_page_get_gen(page_id, zip_size,
						      rw_latch, NULL, BUF_GET,
						      file, line, mtr, &err,
						      height == 0
						      && !index->is_clust());
		tree_blocks[n_blocks] = block;

		ut_ad((block != NULL) == (err == DB_SUCCESS));

		if (err != DB_SUCCESS) {
			if (err == DB_DECRYPTION_FAILED) {
				ib_push_warning((void *)NULL,
					DB_DECRYPTION_FAILED,
					"Table %s is encrypted but encryption service or"
					" used key_id is not available. "
					" Can't continue reading table.",
					index->table->name.m_name);
				index->table->file_unreadable = true;
			}

			break;
		}

		page = buf_block_get_frame(block);

		if (height == ULINT_UNDEFINED
		    && page_is_leaf(page)
		    && rw_latch != RW_NO_LATCH
		    && rw_latch != root_leaf_rw_latch) {
			/* We should retry to get the page, because the root page
			is latched with different level as a leaf page. */
			ut_ad(root_leaf_rw_latch != RW_NO_LATCH);
			ut_ad(rw_latch == RW_S_LATCH);

			ut_ad(n_blocks == 0);
			mtr_release_block_at_savepoint(
				mtr, tree_savepoints[n_blocks],
				tree_blocks[n_blocks]);

			upper_rw_latch = root_leaf_rw_latch;
			continue;
		}

		ut_ad(fil_page_index_page_check(page));
		ut_ad(index->id == btr_page_get_index_id(page));

		if (height == ULINT_UNDEFINED) {
			/* We are in the root node */

			height = btr_page_get_level(page);
		}

		if (height == 0) {
			if (rw_latch == RW_NO_LATCH
			    || srv_read_only_mode) {
				btr_cur_latch_leaves(block, latch_mode, cursor,
						     mtr);
			}

			/* btr_cur_open_at_index_side_func() and
			btr_cur_search_to_nth_level() release
			tree s-latch here.*/
			switch (latch_mode) {
			case BTR_MODIFY_TREE:
			case BTR_CONT_MODIFY_TREE:
			case BTR_CONT_SEARCH_TREE:
				break;
			default:
				/* Release the tree s-latch */
				if (!srv_read_only_mode) {
					mtr_release_s_latch_at_savepoint(
						mtr, savepoint,
						dict_index_get_lock(index));
				}

				/* release upper blocks */
				for (; n_releases < n_blocks; n_releases++) {
					mtr_release_block_at_savepoint(
						mtr,
						tree_savepoints[n_releases],
						tree_blocks[n_releases]);
				}
			}
		}

		page_cur_open_on_rnd_user_rec(block, page_cursor);

		if (height == 0) {

			break;
		}

		ut_ad(height > 0);

		height--;

		node_ptr = page_cur_get_rec(page_cursor);
		offsets = rec_get_offsets(node_ptr, cursor->index, offsets,
					  false, ULINT_UNDEFINED, &heap);

		/* If the rec is the first or last in the page for
		pessimistic delete intention, it might cause node_ptr insert
		for the upper level. We should change the intention and retry.
		*/
		if (latch_mode == BTR_MODIFY_TREE
		    && btr_cur_need_opposite_intention(
			page, lock_intention, node_ptr)) {

			ut_ad(upper_rw_latch == RW_X_LATCH);
			/* release all blocks */
			for (; n_releases <= n_blocks; n_releases++) {
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}

			lock_intention = BTR_INTENTION_BOTH;

			page_id.set_page_no(dict_index_get_page(index));

			height = ULINT_UNDEFINED;

			n_blocks = 0;
			n_releases = 0;

			continue;
		}

		if (latch_mode == BTR_MODIFY_TREE
		    && !btr_cur_will_modify_tree(
				cursor->index, page, lock_intention, node_ptr,
				node_ptr_max_size, zip_size, mtr)) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			ut_ad(n_releases <= n_blocks);

			/* we can release upper blocks */
			for (; n_releases < n_blocks; n_releases++) {
				if (n_releases == 0) {
					/* we should not release root page
					to pin to same block. */
					continue;
				}

				/* release unused blocks to unpin */
				mtr_release_block_at_savepoint(
					mtr, tree_savepoints[n_releases],
					tree_blocks[n_releases]);
			}
		}

		if (height == 0
		    && latch_mode == BTR_MODIFY_TREE) {
			ut_ad(upper_rw_latch == RW_X_LATCH);
			/* we should sx-latch root page, if released already.
			It contains seg_header. */
			if (n_releases > 0) {
				mtr_block_sx_latch_at_savepoint(
					mtr, tree_savepoints[0],
					tree_blocks[0]);
			}

			/* x-latch the branch blocks not released yet. */
			for (ulint i = n_releases; i <= n_blocks; i++) {
				mtr_block_x_latch_at_savepoint(
					mtr, tree_savepoints[i],
					tree_blocks[i]);
			}
		}

		/* Go to the child node */
		page_id.set_page_no(
			btr_node_ptr_get_child_page_no(node_ptr, offsets));

		n_blocks++;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return err == DB_SUCCESS;
}

/*==================== B-TREE INSERT =========================*/

/*************************************************************//**
Inserts a record if there is enough space, or if enough space can
be freed by reorganizing. Differs from btr_cur_optimistic_insert because
no heuristics is applied to whether it pays to use CPU time for
reorganizing the page or not.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to inserted record if succeed, else NULL */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
rec_t*
btr_cur_insert_if_possible(
/*=======================*/
	btr_cur_t*	cursor,	/*!< in: cursor on page after which to insert;
				cursor stays valid */
	const dtuple_t*	tuple,	/*!< in: tuple to insert; the size info need not
				have been stored to tuple */
	offset_t**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	page_cur_t*	page_cursor;
	rec_t*		rec;

	ut_ad(dtuple_check_typed(tuple));

	ut_ad(mtr_memo_contains(mtr, btr_cur_get_block(cursor),
			       MTR_MEMO_PAGE_X_FIX));
	page_cursor = btr_cur_get_page_cur(cursor);

	/* Now, try the insert */
	rec = page_cur_tuple_insert(page_cursor, tuple, cursor->index,
				    offsets, heap, n_ext, mtr);

	/* If the record did not fit, reorganize.
	For compressed pages, page_cur_tuple_insert()
	attempted this already. */
	if (!rec && !page_cur_get_page_zip(page_cursor)
	    && btr_page_reorganize(page_cursor, cursor->index, mtr)) {
		rec = page_cur_tuple_insert(
			page_cursor, tuple, cursor->index,
			offsets, heap, n_ext, mtr);
	}

	ut_ad(!rec || rec_offs_validate(rec, cursor->index, *offsets));
	return(rec);
}

/*************************************************************//**
For an insert, checks the locks and does the undo logging if desired.
@return DB_SUCCESS, DB_WAIT_LOCK, DB_FAIL, or error number */
UNIV_INLINE MY_ATTRIBUTE((warn_unused_result, nonnull(2,3,5,6)))
dberr_t
btr_cur_ins_lock_and_undo(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if
				not zero, the parameters index and thr
				should be specified */
	btr_cur_t*	cursor,	/*!< in: cursor on page after which to insert */
	dtuple_t*	entry,	/*!< in/out: entry to insert */
	que_thr_t*	thr,	/*!< in: query thread or NULL */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	bool*		inherit)/*!< out: true if the inserted new record maybe
				should inherit LOCK_GAP type locks from the
				successor record */
{
	dict_index_t*	index;
	dberr_t		err = DB_SUCCESS;
	rec_t*		rec;
	roll_ptr_t	roll_ptr;

	/* Check if we have to wait for a lock: enqueue an explicit lock
	request if yes */

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));
	ut_ad(mtr->is_named_space(index->table->space));

	/* Check if there is predicate or GAP lock preventing the insertion */
	if (!(flags & BTR_NO_LOCKING_FLAG)) {
		if (dict_index_is_spatial(index)) {
			lock_prdt_t	prdt;
			rtr_mbr_t	mbr;

			rtr_get_mbr_from_tuple(entry, &mbr);

			/* Use on stack MBR variable to test if a lock is
			needed. If so, the predicate (MBR) will be allocated
			from lock heap in lock_prdt_insert_check_and_lock() */
			lock_init_prdt_from_mbr(
				&prdt, &mbr, 0, NULL);

			err = lock_prdt_insert_check_and_lock(
				flags, rec, btr_cur_get_block(cursor),
				index, thr, mtr, &prdt);
			*inherit = false;
		} else {
			err = lock_rec_insert_check_and_lock(
				flags, rec, btr_cur_get_block(cursor),
				index, thr, mtr, inherit);
		}
	}

	if (err != DB_SUCCESS
	    || !(~flags | (BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG))
	    || !dict_index_is_clust(index) || dict_index_is_ibuf(index)) {

		return(err);
	}

	if (flags & BTR_NO_UNDO_LOG_FLAG) {
		roll_ptr = roll_ptr_t(1) << ROLL_PTR_INSERT_FLAG_POS;
		if (!(flags & BTR_KEEP_SYS_FLAG)) {
upd_sys:
			dfield_t* r = dtuple_get_nth_field(
				entry, index->db_roll_ptr());
			ut_ad(r->len == DATA_ROLL_PTR_LEN);
			trx_write_roll_ptr(static_cast<byte*>(r->data),
					   roll_ptr);
		}
	} else {
		err = trx_undo_report_row_operation(thr, index, entry,
						    NULL, 0, NULL, NULL,
						    &roll_ptr);
		if (err == DB_SUCCESS) {
			goto upd_sys;
		}
	}

	return(err);
}

/**
Prefetch siblings of the leaf for the pessimistic operation.
@param block	leaf page */
static
void
btr_cur_prefetch_siblings(
	buf_block_t*	block)
{
	page_t*	page = buf_block_get_frame(block);

	ut_ad(page_is_leaf(page));

	ulint left_page_no = fil_page_get_prev(page);
	ulint right_page_no = fil_page_get_next(page);

	if (left_page_no != FIL_NULL) {
		buf_read_page_background(
			page_id_t(block->page.id.space(), left_page_no),
			block->zip_size(), false);
	}
	if (right_page_no != FIL_NULL) {
		buf_read_page_background(
			page_id_t(block->page.id.space(), right_page_no),
			block->zip_size(), false);
	}
}

/*************************************************************//**
Tries to perform an insert to a page in an index tree, next to cursor.
It is assumed that mtr holds an x-latch on the page. The operation does
not succeed if there is too little space on the page. If there is just
one record on the page, the insert will always succeed; this is to
prevent trying to split a page with just one record.
@return DB_SUCCESS, DB_WAIT_LOCK, DB_FAIL, or error number */
dberr_t
btr_cur_optimistic_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if not
				zero, the parameters index and thr should be
				specified */
	btr_cur_t*	cursor,	/*!< in: cursor on page after which to insert;
				cursor stays valid */
	offset_t**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap */
	dtuple_t*	entry,	/*!< in/out: entry to insert */
	rec_t**		rec,	/*!< out: pointer to inserted record if
				succeed */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr,	/*!< in/out: query thread; can be NULL if
				!(~flags
				& (BTR_NO_LOCKING_FLAG
				| BTR_NO_UNDO_LOG_FLAG)) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction;
				if this function returns DB_SUCCESS on
				a leaf page of a secondary index in a
				compressed tablespace, the caller must
				mtr_commit(mtr) before latching
				any further pages */
{
	big_rec_t*	big_rec_vec	= NULL;
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	buf_block_t*	block;
	page_t*		page;
	rec_t*		dummy;
	bool		leaf;
	bool		reorg;
	bool		inherit = true;
	ulint		rec_size;
	dberr_t		err;

	ut_ad(thr || !(~flags & (BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG)));
	*big_rec = NULL;

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	index = cursor->index;

	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));
	ut_ad(dtuple_check_typed(entry));

#ifdef UNIV_DEBUG_VALGRIND
	if (block->page.zip.data) {
		UNIV_MEM_ASSERT_RW(page, srv_page_size);
		UNIV_MEM_ASSERT_RW(block->page.zip.data,
				   block->zip_size());
	}
#endif /* UNIV_DEBUG_VALGRIND */

	leaf = page_is_leaf(page);

	if (UNIV_UNLIKELY(entry->is_alter_metadata())) {
		ut_ad(leaf);
		goto convert_big_rec;
	}

	/* Calculate the record size when entry is converted to a record */
	rec_size = rec_get_converted_size(index, entry, n_ext);

	if (page_zip_rec_needs_ext(rec_size, page_is_comp(page),
				   dtuple_get_n_fields(entry),
				   block->zip_size())) {
convert_big_rec:
		/* The record is so big that we have to store some fields
		externally on separate database pages */
		big_rec_vec = dtuple_convert_big_rec(index, 0, entry, &n_ext);

		if (UNIV_UNLIKELY(big_rec_vec == NULL)) {

			return(DB_TOO_BIG_RECORD);
		}

		rec_size = rec_get_converted_size(index, entry, n_ext);
	}

	if (block->page.zip.data && page_zip_is_too_big(index, entry)) {
		if (big_rec_vec != NULL) {
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);
		}

		return(DB_TOO_BIG_RECORD);
	}

	LIMIT_OPTIMISTIC_INSERT_DEBUG(page_get_n_recs(page),
				      goto fail);

	if (block->page.zip.data && leaf
	    && (page_get_data_size(page) + rec_size
		>= dict_index_zip_pad_optimal_page_size(index))) {
		/* If compression padding tells us that insertion will
		result in too packed up page i.e.: which is likely to
		cause compression failure then don't do an optimistic
		insertion. */
fail:
		err = DB_FAIL;

		/* prefetch siblings of the leaf for the pessimistic
		operation, if the page is leaf. */
		if (page_is_leaf(page)) {
			btr_cur_prefetch_siblings(block);
		}
fail_err:

		if (big_rec_vec) {
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);
		}

		return(err);
	}

	ulint	max_size = page_get_max_insert_size_after_reorganize(page, 1);

	if (page_has_garbage(page)) {
		if ((max_size < rec_size
		     || max_size < BTR_CUR_PAGE_REORGANIZE_LIMIT)
		    && page_get_n_recs(page) > 1
		    && page_get_max_insert_size(page, 1) < rec_size) {

			goto fail;
		}
	} else if (max_size < rec_size) {
		goto fail;
	}

	/* If there have been many consecutive inserts to the
	clustered index leaf page of an uncompressed table, check if
	we have to split the page to reserve enough free space for
	future updates of records. */

	if (leaf && !block->page.zip.data && dict_index_is_clust(index)
	    && page_get_n_recs(page) >= 2
	    && dict_index_get_space_reserve() + rec_size > max_size
	    && (btr_page_get_split_rec_to_right(cursor, &dummy)
		|| btr_page_get_split_rec_to_left(cursor))) {
		goto fail;
	}

	page_cursor = btr_cur_get_page_cur(cursor);

	DBUG_LOG("ib_cur",
		 "insert " << index->name << " (" << index->id << ") by "
		 << ib::hex(thr ? thr->graph->trx->id : 0)
		 << ' ' << rec_printer(entry).str());
	DBUG_EXECUTE_IF("do_page_reorganize",
			btr_page_reorganize(page_cursor, index, mtr););

	/* Now, try the insert */
	{
		const rec_t*	page_cursor_rec = page_cur_get_rec(page_cursor);

		/* Check locks and write to the undo log,
		if specified */
		err = btr_cur_ins_lock_and_undo(flags, cursor, entry,
						thr, mtr, &inherit);
		if (err != DB_SUCCESS) {
			goto fail_err;
		}

#ifdef UNIV_DEBUG
		if (!(flags & BTR_CREATE_FLAG)
		    && index->is_primary() && page_is_leaf(page)) {
			const dfield_t* trx_id = dtuple_get_nth_field(
				entry, dict_col_get_clust_pos(
					dict_table_get_sys_col(index->table,
							       DATA_TRX_ID),
					index));

			ut_ad(trx_id->len == DATA_TRX_ID_LEN);
			ut_ad(trx_id[1].len == DATA_ROLL_PTR_LEN);
			ut_ad(*static_cast<const byte*>
			      (trx_id[1].data) & 0x80);
			if (flags & BTR_NO_UNDO_LOG_FLAG) {
				ut_ad(!memcmp(trx_id->data, reset_trx_id,
					      DATA_TRX_ID_LEN));
			} else {
				ut_ad(thr->graph->trx->id);
				ut_ad(thr->graph->trx->id
				      == trx_read_trx_id(
					      static_cast<const byte*>(
						      trx_id->data)));
			}
		}
#endif

		*rec = page_cur_tuple_insert(
			page_cursor, entry, index, offsets, heap,
			n_ext, mtr);

		reorg = page_cursor_rec != page_cur_get_rec(page_cursor);
	}

	if (*rec) {
	} else if (block->page.zip.data) {
		ut_ad(!index->table->is_temporary());
		/* Reset the IBUF_BITMAP_FREE bits, because
		page_cur_tuple_insert() will have attempted page
		reorganize before failing. */
		if (leaf
		    && !dict_index_is_clust(index)) {
			ibuf_reset_free_bits(block);
		}

		goto fail;
	} else {
		ut_ad(!reorg);

		/* If the record did not fit, reorganize */
		if (!btr_page_reorganize(page_cursor, index, mtr)) {
			ut_ad(0);
			goto fail;
		}

		ut_ad(page_get_max_insert_size(page, 1) == max_size);

		reorg = TRUE;

		*rec = page_cur_tuple_insert(page_cursor, entry, index,
					     offsets, heap, n_ext, mtr);

		if (UNIV_UNLIKELY(!*rec)) {
			ib::fatal() <<  "Cannot insert tuple " << *entry
				<< "into index " << index->name
				<< " of table " << index->table->name
				<< ". Max size: " << max_size;
		}
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (!leaf) {
# ifdef MYSQL_INDEX_DISABLE_AHI
	} else if (index->disable_ahi) {
# endif
	} else if (entry->info_bits & REC_INFO_MIN_REC_FLAG) {
		ut_ad(entry->is_metadata());
		ut_ad(index->is_instant());
		ut_ad(flags == BTR_NO_LOCKING_FLAG);
	} else {
		rw_lock_t* ahi_latch = btr_get_search_latch(index);
		if (!reorg && cursor->flag == BTR_CUR_HASH) {
			btr_search_update_hash_node_on_insert(
				cursor, ahi_latch);
		} else {
			btr_search_update_hash_on_insert(cursor, ahi_latch);
		}
	}
#endif /* BTR_CUR_HASH_ADAPT */

	if (!(flags & BTR_NO_LOCKING_FLAG) && inherit) {

		lock_update_insert(block, *rec);
	}

	if (leaf
	    && !dict_index_is_clust(index)
	    && !index->table->is_temporary()) {
		/* Update the free bits of the B-tree page in the
		insert buffer bitmap. */

		/* The free bits in the insert buffer bitmap must
		never exceed the free space on a page.  It is safe to
		decrement or reset the bits in the bitmap in a
		mini-transaction that is committed before the
		mini-transaction that affects the free space. */

		/* It is unsafe to increment the bits in a separately
		committed mini-transaction, because in crash recovery,
		the free bits could momentarily be set too high. */

		if (block->page.zip.data) {
			/* Update the bits in the same mini-transaction. */
			ibuf_update_free_bits_zip(block, mtr);
		} else {
			/* Decrement the bits in a separate
			mini-transaction. */
			ibuf_update_free_bits_if_full(
				block, max_size,
				rec_size + PAGE_DIR_SLOT_SIZE);
		}
	}

	*big_rec = big_rec_vec;

	return(DB_SUCCESS);
}

/*************************************************************//**
Performs an insert on a page of an index tree. It is assumed that mtr
holds an x-latch on the tree and on the cursor page. If the insert is
made on the leaf level, to avoid deadlocks, mtr must also own x-latches
to brothers of page, if those brothers exist.
@return DB_SUCCESS or error number */
dberr_t
btr_cur_pessimistic_insert(
/*=======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if not
				zero, the parameter thr should be
				specified; if no undo logging is specified,
				then the caller must have reserved enough
				free extents in the file space so that the
				insertion will certainly succeed */
	btr_cur_t*	cursor,	/*!< in: cursor after which to insert;
				cursor stays valid */
	offset_t**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap
				that can be emptied */
	dtuple_t*	entry,	/*!< in/out: entry to insert */
	rec_t**		rec,	/*!< out: pointer to inserted record if
				succeed */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr,	/*!< in/out: query thread; can be NULL if
				!(~flags
				& (BTR_NO_LOCKING_FLAG
				| BTR_NO_UNDO_LOG_FLAG)) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	dict_index_t*	index		= cursor->index;
	big_rec_t*	big_rec_vec	= NULL;
	dberr_t		err;
	bool		inherit = false;
	bool		success;
	ulint		n_reserved	= 0;

	ut_ad(dtuple_check_typed(entry));
	ut_ad(thr || !(~flags & (BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG)));

	*big_rec = NULL;

	ut_ad(mtr_memo_contains_flagged(
		      mtr, dict_index_get_lock(btr_cur_get_index(cursor)),
		      MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains(mtr, btr_cur_get_block(cursor),
			       MTR_MEMO_PAGE_X_FIX));
	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));

	cursor->flag = BTR_CUR_BINARY;

	/* Check locks and write to undo log, if specified */

	err = btr_cur_ins_lock_and_undo(flags, cursor, entry,
					thr, mtr, &inherit);

	if (err != DB_SUCCESS) {

		return(err);
	}

	if (!(flags & BTR_NO_UNDO_LOG_FLAG)) {
		/* First reserve enough free space for the file segments
		of the index tree, so that the insert will not fail because
		of lack of space */

		ulint	n_extents = cursor->tree_height / 16 + 3;

		success = fsp_reserve_free_extents(&n_reserved,
						   index->table->space,
						   n_extents, FSP_NORMAL, mtr);
		if (!success) {
			return(DB_OUT_OF_FILE_SPACE);
		}
	}

	if (page_zip_rec_needs_ext(rec_get_converted_size(index, entry, n_ext),
				   index->table->not_redundant(),
				   dtuple_get_n_fields(entry),
				   btr_cur_get_block(cursor)->zip_size())
	    || UNIV_UNLIKELY(entry->is_alter_metadata()
			     && !dfield_is_ext(
				     dtuple_get_nth_field(
					     entry,
					     index->first_user_field())))) {
		/* The record is so big that we have to store some fields
		externally on separate database pages */

		if (UNIV_LIKELY_NULL(big_rec_vec)) {
			/* This should never happen, but we handle
			the situation in a robust manner. */
			ut_ad(0);
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);
		}

		big_rec_vec = dtuple_convert_big_rec(index, 0, entry, &n_ext);

		if (big_rec_vec == NULL) {

			index->table->space->release_free_extents(n_reserved);
			return(DB_TOO_BIG_RECORD);
		}
	}

	if (dict_index_get_page(index)
	    == btr_cur_get_block(cursor)->page.id.page_no()) {

		/* The page is the root page */
		*rec = btr_root_raise_and_insert(
			flags, cursor, offsets, heap, entry, n_ext, mtr);
	} else {
		*rec = btr_page_split_and_insert(
			flags, cursor, offsets, heap, entry, n_ext, mtr);
	}

	if (*rec == NULL && os_has_said_disk_full) {
		return(DB_OUT_OF_FILE_SPACE);
	}

	ut_ad(page_rec_get_next(btr_cur_get_rec(cursor)) == *rec
	      || dict_index_is_spatial(index));

	if (!(flags & BTR_NO_LOCKING_FLAG)) {
		ut_ad(!index->table->is_temporary());
		if (dict_index_is_spatial(index)) {
			/* Do nothing */
		} else {
			/* The cursor might be moved to the other page
			and the max trx id field should be updated after
			the cursor was fixed. */
			if (!dict_index_is_clust(index)) {
				page_update_max_trx_id(
					btr_cur_get_block(cursor),
					btr_cur_get_page_zip(cursor),
					thr_get_trx(thr)->id, mtr);
			}

			if (!page_rec_is_infimum(btr_cur_get_rec(cursor))
			    || !page_has_prev(btr_cur_get_page(cursor))) {
				/* split and inserted need to call
				lock_update_insert() always. */
				inherit = true;
			}
		}
	}

	if (!page_is_leaf(btr_cur_get_page(cursor))) {
		ut_ad(!big_rec_vec);
	} else {
#ifdef BTR_CUR_HASH_ADAPT
# ifdef MYSQL_INDEX_DISABLE_AHI
		if (index->disable_ahi); else
# endif
		if (entry->info_bits & REC_INFO_MIN_REC_FLAG) {
			ut_ad(entry->is_metadata());
			ut_ad(index->is_instant());
			ut_ad(flags & BTR_NO_LOCKING_FLAG);
			ut_ad(!(flags & BTR_CREATE_FLAG));
		} else {
			btr_search_update_hash_on_insert(
				cursor, btr_get_search_latch(index));
		}
#endif /* BTR_CUR_HASH_ADAPT */
		if (inherit && !(flags & BTR_NO_LOCKING_FLAG)) {

			lock_update_insert(btr_cur_get_block(cursor), *rec);
		}
	}

	index->table->space->release_free_extents(n_reserved);
	*big_rec = big_rec_vec;

	return(DB_SUCCESS);
}

/*==================== B-TREE UPDATE =========================*/

/*************************************************************//**
For an update, checks the locks and does the undo logging.
@return DB_SUCCESS, DB_WAIT_LOCK, or error number */
UNIV_INLINE MY_ATTRIBUTE((warn_unused_result))
dberr_t
btr_cur_upd_lock_and_undo(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on record to update */
	const offset_t*	offsets,/*!< in: rec_get_offsets() on cursor */
	const upd_t*	update,	/*!< in: update vector */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	roll_ptr_t*	roll_ptr)/*!< out: roll pointer */
{
	dict_index_t*	index;
	const rec_t*	rec;
	dberr_t		err;

	ut_ad((thr != NULL) || (flags & BTR_NO_LOCKING_FLAG));

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr->is_named_space(index->table->space));

	if (!dict_index_is_clust(index)) {
		ut_ad(dict_index_is_online_ddl(index)
		      == !!(flags & BTR_CREATE_FLAG));

		/* We do undo logging only when we update a clustered index
		record */
		return(lock_sec_rec_modify_check_and_lock(
			       flags, btr_cur_get_block(cursor), rec,
			       index, thr, mtr));
	}

	/* Check if we have to wait for a lock: enqueue an explicit lock
	request if yes */

	if (!(flags & BTR_NO_LOCKING_FLAG)) {
		err = lock_clust_rec_modify_check_and_lock(
			flags, btr_cur_get_block(cursor), rec, index,
			offsets, thr);
		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	/* Append the info about the update in the undo log */

	return((flags & BTR_NO_UNDO_LOG_FLAG)
	       ? DB_SUCCESS
	       : trx_undo_report_row_operation(
		       thr, index, NULL, update,
		       cmpl_info, rec, offsets, roll_ptr));
}

/** Write DB_TRX_ID,DB_ROLL_PTR to a clustered index entry.
@param[in,out]	entry		clustered index entry
@param[in]	index		clustered index
@param[in]	trx_id		DB_TRX_ID
@param[in]	roll_ptr	DB_ROLL_PTR */
static void btr_cur_write_sys(
	dtuple_t*		entry,
	const dict_index_t*	index,
	trx_id_t		trx_id,
	roll_ptr_t		roll_ptr)
{
	dfield_t* t = dtuple_get_nth_field(entry, index->db_trx_id());
	ut_ad(t->len == DATA_TRX_ID_LEN);
	trx_write_trx_id(static_cast<byte*>(t->data), trx_id);
	dfield_t* r = dtuple_get_nth_field(entry, index->db_roll_ptr());
	ut_ad(r->len == DATA_ROLL_PTR_LEN);
	trx_write_roll_ptr(static_cast<byte*>(r->data), roll_ptr);
}

/** Update DB_TRX_ID, DB_ROLL_PTR in a clustered index record.
@param[in,out]  block           clustered index leaf page
@param[in,out]  rec             clustered index record
@param[in]      index           clustered index
@param[in]      offsets         rec_get_offsets(rec, index)
@param[in]      trx             transaction
@param[in]      roll_ptr        DB_ROLL_PTR value
@param[in,out]  mtr             mini-transaction */
static void btr_cur_upd_rec_sys(buf_block_t *block, rec_t *rec,
                                dict_index_t *index, const offset_t *offsets,
                                const trx_t *trx, roll_ptr_t roll_ptr,
                                mtr_t *mtr)
{
  ut_ad(index->is_primary());
  ut_ad(rec_offs_validate(rec, index, offsets));

  if (UNIV_LIKELY_NULL(block->page.zip.data))
  {
    page_zip_write_trx_id_and_roll_ptr(block, rec, offsets, index->db_trx_id(),
                                       trx->id, roll_ptr, mtr);
    return;
  }

  ulint offset= index->trx_id_offset;

  if (!offset)
    offset= row_get_trx_id_offset(index, offsets);

  compile_time_assert(DATA_TRX_ID + 1 == DATA_ROLL_PTR);

  /* During IMPORT the trx id in the record can be in the future, if
  the .ibd file is being imported from another instance. During IMPORT
  roll_ptr will be 0. */
  ut_ad(roll_ptr == 0 ||
        lock_check_trx_id_sanity(trx_read_trx_id(rec + offset),
                                 rec, index, offsets));

  byte sys[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN];

  trx_write_trx_id(sys, trx->id);
  trx_write_roll_ptr(sys + DATA_TRX_ID_LEN, roll_ptr);

  ulint d= 0;
  const byte *src= nullptr;
  byte *dest= rec + offset;
  ulint len= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;

  if (UNIV_LIKELY(index->trx_id_offset))
  {
    const rec_t *prev= page_rec_get_prev_const(rec);
    if (UNIV_UNLIKELY(prev == rec))
      ut_ad(0);
    else if (page_rec_is_infimum(prev));
    else
      for (src= prev + offset; d < DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN; d++)
        if (src[d] != sys[d])
          break;
    if (d > 6 && memcmp(dest, sys, d))
    {
      /* We save space by replacing a single record

      WRITE,page_offset(dest),byte[13]

      with two records:

      MEMMOVE,page_offset(dest),d(1 byte),offset(1..3 bytes),
      WRITE|0x80,0,byte[13-d]

      The single WRITE record would be x+13 bytes long, with x>2.
      The MEMMOVE record would be up to x+1+3 = x+4 bytes, and the
      second WRITE would be 1+1+13-d = 15-d bytes.

      The total size is: x+13 versus x+4+15-d = x+19-d bytes.
      To save space, we must have d>6, that is, the complete DB_TRX_ID and
      the first byte(s) of DB_ROLL_PTR must match the previous record. */
      memcpy(dest, src, d);
      mtr->memmove(*block, page_offset(dest), page_offset(src), d);
      dest+= d;
      len-= d;
      /* DB_TRX_ID,DB_ROLL_PTR must be unique in each record when
      DB_TRX_ID refers to an active transaction. */
      ut_ad(len);
    }
    else
      d= 0;
  }

  if (UNIV_LIKELY(len)) /* extra safety, to avoid corrupting the log */
    mtr->memcpy<mtr_t::OPT>(*block, dest, sys + d, len);
}

/*************************************************************//**
See if there is enough place in the page modification log to log
an update-in-place.

@retval false if out of space; IBUF_BITMAP_FREE will be reset
outside mtr if the page was recompressed
@retval true if enough place;

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE if this is
a secondary index leaf page. This has to be done either within the
same mini-transaction, or by invoking ibuf_reset_free_bits() before
mtr_commit(mtr). */
bool
btr_cur_update_alloc_zip_func(
/*==========================*/
	page_zip_des_t*	page_zip,/*!< in/out: compressed page */
	page_cur_t*	cursor,	/*!< in/out: B-tree page cursor */
	dict_index_t*	index,	/*!< in: the index corresponding to cursor */
#ifdef UNIV_DEBUG
	offset_t*	offsets,/*!< in/out: offsets of the cursor record */
#endif /* UNIV_DEBUG */
	ulint		length,	/*!< in: size needed */
	bool		create,	/*!< in: true=delete-and-insert,
				false=update-in-place */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{

	/* Have a local copy of the variables as these can change
	dynamically. */
	const page_t*	page = page_cur_get_page(cursor);

	ut_ad(page_zip == page_cur_get_page_zip(cursor));
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(rec_offs_validate(page_cur_get_rec(cursor), index, offsets));

	if (page_zip_available(page_zip, dict_index_is_clust(index),
			       length, create)) {
		return(true);
	}

	if (!page_zip->m_nonempty && !page_has_garbage(page)) {
		/* The page has been freshly compressed, so
		reorganizing it will not help. */
		return(false);
	}

	if (create && page_is_leaf(page)
	    && (length + page_get_data_size(page)
		>= dict_index_zip_pad_optimal_page_size(index))) {
		return(false);
	}

	if (!btr_page_reorganize(cursor, index, mtr)) {
		goto out_of_space;
	}

	rec_offs_make_valid(page_cur_get_rec(cursor), index,
			    page_is_leaf(page), offsets);

	/* After recompressing a page, we must make sure that the free
	bits in the insert buffer bitmap will not exceed the free
	space on the page.  Because this function will not attempt
	recompression unless page_zip_available() fails above, it is
	safe to reset the free bits if page_zip_available() fails
	again, below.  The free bits can safely be reset in a separate
	mini-transaction.  If page_zip_available() succeeds below, we
	can be sure that the btr_page_reorganize() above did not reduce
	the free space available on the page. */

	if (page_zip_available(page_zip, dict_index_is_clust(index),
			       length, create)) {
		return(true);
	}

out_of_space:
	ut_ad(rec_offs_validate(page_cur_get_rec(cursor), index, offsets));

	/* Out of space: reset the free bits. */
	if (!dict_index_is_clust(index)
	    && !index->table->is_temporary()
	    && page_is_leaf(page)) {
		ibuf_reset_free_bits(page_cur_get_block(cursor));
	}

	return(false);
}

/** Apply an update vector to a record. No field size changes are allowed.

This is usually invoked on a clustered index. The only use case for a
secondary index is row_ins_sec_index_entry_by_modify() or its
counterpart in ibuf_insert_to_index_page().
@param[in,out]  rec     index record
@param[in]      index   the index of the record
@param[in]      offsets rec_get_offsets(rec, index)
@param[in]      update  update vector
@param[in,out]  block   index page
@param[in,out]  mtr     mini-transaction */
void btr_cur_upd_rec_in_place(rec_t *rec, const dict_index_t *index,
                              const offset_t *offsets, const upd_t *update,
                              buf_block_t *block, mtr_t *mtr)
{
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!index->table->skip_alter_undo);
	ut_ad(!block->page.zip.data || index->table->not_redundant());

#ifdef UNIV_DEBUG
	if (rec_offs_comp(offsets)) {
		switch (rec_get_status(rec)) {
		case REC_STATUS_ORDINARY:
			break;
		case REC_STATUS_INSTANT:
			ut_ad(index->is_instant());
			break;
		case REC_STATUS_NODE_PTR:
		case REC_STATUS_INFIMUM:
		case REC_STATUS_SUPREMUM:
			ut_ad("wrong record status in update" == 0);
		}
	}
#endif /* UNIV_DEBUG */

	static_assert(REC_INFO_BITS_SHIFT == 0, "compatibility");
	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		ut_ad(rec_offs_comp(offsets));
		byte* info_bits = &rec[-REC_NEW_INFO_BITS];
		const bool flip_del_mark = (*info_bits ^ update->info_bits)
			& REC_INFO_DELETED_FLAG;
		*info_bits &= byte(~REC_INFO_BITS_MASK);
		*info_bits |= update->info_bits;

		if (flip_del_mark) {
			page_zip_rec_set_deleted(block, rec, update->info_bits
						 & REC_INFO_DELETED_FLAG, mtr);
		}
	} else {
		byte* info_bits = &rec[rec_offs_comp(offsets)
				       ? -REC_NEW_INFO_BITS
				       : -REC_OLD_INFO_BITS];

		mtr->write<1,mtr_t::OPT>(*block, info_bits,
			      (*info_bits & ~REC_INFO_BITS_MASK)
			      | update->info_bits);
	}

	for (ulint i = 0; i < update->n_fields; i++) {
		const upd_field_t* uf = upd_get_nth_field(update, i);
		if (upd_fld_is_virtual_col(uf) && !index->has_virtual()) {
			continue;
		}
		const ulint n = uf->field_no;

		ut_ad(!dfield_is_ext(&uf->new_val)
		      == !rec_offs_nth_extern(offsets, n));
		ut_ad(!rec_offs_nth_default(offsets, n));

		if (UNIV_UNLIKELY(dfield_is_null(&uf->new_val))) {
			if (rec_offs_nth_sql_null(offsets, n)) {
				ut_ad(index->table->is_instant());
				ut_ad(n >= index->n_core_fields);
				continue;
			}

			ut_ad(!index->table->not_redundant());
			if (ulint size = rec_get_nth_field_size(rec, n)) {
				mtr->memset(
					block,
					page_offset(rec_get_field_start_offs(
							    rec, n) + rec),
					size, 0);
			}
			ulint l = rec_get_1byte_offs_flag(rec)
				? (n + 1) : (n + 1) * 2;
			byte* b = &rec[-REC_N_OLD_EXTRA_BYTES - l];
			compile_time_assert(REC_1BYTE_SQL_NULL_MASK << 8
					    == REC_2BYTE_SQL_NULL_MASK);
			mtr->write<1>(*block, b,
				      byte(*b | REC_1BYTE_SQL_NULL_MASK));
			continue;
		}

		ulint len;
		byte* data = rec_get_nth_field(rec, offsets, n, &len);
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			ut_ad(len == uf->new_val.len);
			memcpy(data, uf->new_val.data, len);
			continue;
		}

		if (UNIV_UNLIKELY(len != uf->new_val.len)) {
			ut_ad(len == UNIV_SQL_NULL);
			ut_ad(!rec_offs_comp(offsets));
			len = uf->new_val.len;
			ut_ad(len == rec_get_nth_field_size(rec, n));
			ulint l = rec_get_1byte_offs_flag(rec)
				? (n + 1) : (n + 1) * 2;
			byte* b = &rec[-REC_N_OLD_EXTRA_BYTES - l];
			compile_time_assert(REC_1BYTE_SQL_NULL_MASK << 8
					    == REC_2BYTE_SQL_NULL_MASK);
			mtr->write<1>(*block, b,
				      byte(*b & ~REC_1BYTE_SQL_NULL_MASK));
		}

		if (len) {
			mtr->memcpy<mtr_t::OPT>(*block, data, uf->new_val.data,
						len);
		}
	}

	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		page_zip_write_rec(block, rec, index, offsets, 0, mtr);
	}
}

/*************************************************************//**
Updates a record when the update causes no size changes in its fields.
We assume here that the ordering fields of the record do not change.
@return locking or undo log related error code, or
@retval DB_SUCCESS on success
@retval DB_ZIP_OVERFLOW if there is not enough space left
on the compressed page (IBUF_BITMAP_FREE was reset outside mtr) */
dberr_t
btr_cur_update_in_place(
/*====================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to update;
				cursor stays valid and positioned on the
				same record */
	offset_t*	offsets,/*!< in/out: offsets on cursor->page_cur.rec */
	const upd_t*	update,	/*!< in: update vector */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; if this
				is a secondary index, the caller must
				mtr_commit(mtr) before latching any
				further pages */
{
	dict_index_t*	index;
	dberr_t		err;
	rec_t*		rec;
	roll_ptr_t	roll_ptr	= 0;
	ulint		was_delete_marked;

	ut_ad(page_is_leaf(cursor->page_cur.block->frame));
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!!page_rec_is_comp(rec) == dict_table_is_comp(index->table));
	ut_ad(trx_id > 0 || (flags & BTR_KEEP_SYS_FLAG));
	/* The insert buffer tree should never be updated in place. */
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(dict_index_is_online_ddl(index) == !!(flags & BTR_CREATE_FLAG)
	      || dict_index_is_clust(index));
	ut_ad(thr_get_trx(thr)->id == trx_id
	      || (flags & ulint(~(BTR_KEEP_POS_FLAG | BTR_KEEP_IBUF_BITMAP)))
	      == (BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG
		  | BTR_CREATE_FLAG | BTR_KEEP_SYS_FLAG));
	ut_ad(fil_page_index_page_check(btr_cur_get_page(cursor)));
	ut_ad(btr_page_get_index_id(btr_cur_get_page(cursor)) == index->id);
	ut_ad(!(update->info_bits & REC_INFO_MIN_REC_FLAG));

	DBUG_LOG("ib_cur",
		 "update-in-place " << index->name << " (" << index->id
		 << ") by " << ib::hex(trx_id) << ": "
		 << rec_printer(rec, offsets).str());

	buf_block_t* block = btr_cur_get_block(cursor);
	page_zip_des_t*	page_zip = buf_block_get_page_zip(block);

	/* Check that enough space is available on the compressed page. */
	if (UNIV_LIKELY_NULL(page_zip)) {
		ut_ad(!index->table->is_temporary());

		if (!btr_cur_update_alloc_zip(
			    page_zip, btr_cur_get_page_cur(cursor),
			    index, offsets, rec_offs_size(offsets),
			    false, mtr)) {
			return(DB_ZIP_OVERFLOW);
		}

		rec = btr_cur_get_rec(cursor);
	}

	/* Do lock checking and undo logging */
	err = btr_cur_upd_lock_and_undo(flags, cursor, offsets,
					update, cmpl_info,
					thr, mtr, &roll_ptr);
	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		/* We may need to update the IBUF_BITMAP_FREE
		bits after a reorganize that was done in
		btr_cur_update_alloc_zip(). */
		goto func_exit;
	}

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		btr_cur_upd_rec_sys(block, rec, index, offsets,
				    thr_get_trx(thr), roll_ptr, mtr);
	}

	was_delete_marked = rec_get_deleted_flag(
		rec, page_is_comp(buf_block_get_frame(block)));
	/* In delete-marked records, DB_TRX_ID must always refer to an
	existing undo log record. */
	ut_ad(!was_delete_marked
	      || !dict_index_is_clust(index)
	      || row_get_rec_trx_id(rec, index, offsets));

#ifdef BTR_CUR_HASH_ADAPT
	{
		rw_lock_t* ahi_latch = block->index
			? btr_get_search_latch(index) : NULL;
		if (ahi_latch) {
			/* TO DO: Can we skip this if none of the fields
			index->search_info->curr_n_fields
			are being updated? */

			/* The function row_upd_changes_ord_field_binary
			does not work on a secondary index. */

			if (!dict_index_is_clust(index)
			    || row_upd_changes_ord_field_binary(
				    index, update, thr, NULL, NULL)) {
				ut_ad(!(update->info_bits
					& REC_INFO_MIN_REC_FLAG));
				/* Remove possible hash index pointer
				to this record */
				btr_search_update_hash_on_delete(cursor);
			}

			rw_lock_x_lock(ahi_latch);
		}

		assert_block_ahi_valid(block);
#endif /* BTR_CUR_HASH_ADAPT */

		btr_cur_upd_rec_in_place(rec, index, offsets, update, block,
					 mtr);

#ifdef BTR_CUR_HASH_ADAPT
		if (ahi_latch) {
			rw_lock_x_unlock(ahi_latch);
		}
	}
#endif /* BTR_CUR_HASH_ADAPT */

	if (was_delete_marked
	    && !rec_get_deleted_flag(
		    rec, page_is_comp(buf_block_get_frame(block)))) {
		/* The new updated record owns its possible externally
		stored fields */

		btr_cur_unmark_extern_fields(block, rec, index, offsets, mtr);
	}

	ut_ad(err == DB_SUCCESS);

func_exit:
	if (page_zip
	    && !(flags & BTR_KEEP_IBUF_BITMAP)
	    && !dict_index_is_clust(index)
	    && page_is_leaf(buf_block_get_frame(block))) {
		/* Update the free bits in the insert buffer. */
		ut_ad(!index->table->is_temporary());
		ibuf_update_free_bits_zip(block, mtr);
	}

	return(err);
}

/** Trim a metadata record during the rollback of instant ALTER TABLE.
@param[in]	entry	metadata tuple
@param[in]	index	primary key
@param[in]	update	update vector for the rollback */
ATTRIBUTE_COLD
static void btr_cur_trim_alter_metadata(dtuple_t* entry,
					const dict_index_t* index,
					const upd_t* update)
{
	ut_ad(index->is_instant());
	ut_ad(update->is_alter_metadata());
	ut_ad(entry->is_alter_metadata());

	ut_ad(update->fields[0].field_no == index->first_user_field());
	ut_ad(update->fields[0].new_val.ext);
	ut_ad(update->fields[0].new_val.len == FIELD_REF_SIZE);
	ut_ad(entry->n_fields - 1 == index->n_fields);

	const byte* ptr = static_cast<const byte*>(
		update->fields[0].new_val.data);
	ut_ad(!mach_read_from_4(ptr + BTR_EXTERN_LEN));
	ut_ad(mach_read_from_4(ptr + BTR_EXTERN_LEN + 4) > 4);
	ut_ad(mach_read_from_4(ptr + BTR_EXTERN_OFFSET) == FIL_PAGE_DATA);
	ut_ad(mach_read_from_4(ptr + BTR_EXTERN_SPACE_ID)
	      == index->table->space->id);

	ulint n_fields = update->fields[1].field_no;
	ut_ad(n_fields <= index->n_fields);
	if (n_fields != index->n_uniq) {
		ut_ad(n_fields
		      >= index->n_core_fields);
		entry->n_fields = n_fields;
		return;
	}

	/* This is based on dict_table_t::deserialise_columns()
	and btr_cur_instant_init_low(). */
	mtr_t mtr;
	mtr.start();
	buf_block_t* block = buf_page_get(
		page_id_t(index->table->space->id,
			  mach_read_from_4(ptr + BTR_EXTERN_PAGE_NO)),
		0, RW_S_LATCH, &mtr);
	buf_block_dbg_add_level(block, SYNC_EXTERN_STORAGE);
	ut_ad(fil_page_get_type(block->frame) == FIL_PAGE_TYPE_BLOB);
	ut_ad(mach_read_from_4(&block->frame[FIL_PAGE_DATA
					     + BTR_BLOB_HDR_NEXT_PAGE_NO])
	      == FIL_NULL);
	ut_ad(mach_read_from_4(&block->frame[FIL_PAGE_DATA
					     + BTR_BLOB_HDR_PART_LEN])
	      == mach_read_from_4(ptr + BTR_EXTERN_LEN + 4));
	n_fields = mach_read_from_4(
		&block->frame[FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE])
		+ index->first_user_field();
	/* Rollback should not increase the number of fields. */
	ut_ad(n_fields <= index->n_fields);
	ut_ad(n_fields + 1 <= entry->n_fields);
	/* dict_index_t::clear_instant_alter() cannot be invoked while
	rollback of an instant ALTER TABLE transaction is in progress
	for an is_alter_metadata() record. */
	ut_ad(n_fields >= index->n_core_fields);

	mtr.commit();
	entry->n_fields = n_fields + 1;
}

/** Trim an update tuple due to instant ADD COLUMN, if needed.
For normal records, the trailing instantly added fields that match
the initial default values are omitted.

For the special metadata record on a table on which instant
ADD COLUMN has already been executed, both ADD COLUMN and the
rollback of ADD COLUMN need to be handled specially.

@param[in,out]	entry	index entry
@param[in]	index	index
@param[in]	update	update vector
@param[in]	thr	execution thread */
static inline
void
btr_cur_trim(
	dtuple_t*		entry,
	const dict_index_t*	index,
	const upd_t*		update,
	const que_thr_t*	thr)
{
	if (!index->is_instant()) {
	} else if (UNIV_UNLIKELY(update->is_metadata())) {
		/* We are either updating a metadata record
		(instant ALTER TABLE on a table where instant ALTER was
		already executed) or rolling back such an operation. */
		ut_ad(!upd_get_nth_field(update, 0)->orig_len);
		ut_ad(entry->is_metadata());

		if (thr->graph->trx->in_rollback) {
			/* This rollback can occur either as part of
			ha_innobase::commit_inplace_alter_table() rolling
			back after a failed innobase_add_instant_try(),
			or as part of crash recovery. Either way, the
			table will be in the data dictionary cache, with
			the instantly added columns going to be removed
			later in the rollback. */
			ut_ad(index->table->cached);
			/* The DB_TRX_ID,DB_ROLL_PTR are always last,
			and there should be some change to roll back.
			The first field in the update vector is the
			first instantly added column logged by
			innobase_add_instant_try(). */
			ut_ad(update->n_fields > 2);
			if (update->is_alter_metadata()) {
				btr_cur_trim_alter_metadata(
					entry, index, update);
				return;
			}
			ut_ad(!entry->is_alter_metadata());

			ulint n_fields = upd_get_nth_field(update, 0)
				->field_no;
			ut_ad(n_fields + 1 >= entry->n_fields);
			entry->n_fields = n_fields;
		}
	} else {
		entry->trim(*index);
	}
}

/*************************************************************//**
Tries to update a record on a page in an index tree. It is assumed that mtr
holds an x-latch on the page. The operation does not succeed if there is too
little space on the page or if the update would result in too empty a page,
so that tree compression is recommended. We assume here that the ordering
fields of the record do not change.
@return error code, including
@retval DB_SUCCESS on success
@retval DB_OVERFLOW if the updated record does not fit
@retval DB_UNDERFLOW if the page would become too empty
@retval DB_ZIP_OVERFLOW if there is not enough space left
on the compressed page (IBUF_BITMAP_FREE was reset outside mtr) */
dberr_t
btr_cur_optimistic_update(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to update;
				cursor stays valid and positioned on the
				same record */
	offset_t**	offsets,/*!< out: offsets on cursor->page_cur.rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to NULL or memory heap */
	const upd_t*	update,	/*!< in: update vector; this must also
				contain trx id and roll ptr fields */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; if this
				is a secondary index, the caller must
				mtr_commit(mtr) before latching any
				further pages */
{
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	dberr_t		err;
	buf_block_t*	block;
	page_t*		page;
	page_zip_des_t*	page_zip;
	rec_t*		rec;
	ulint		max_size;
	ulint		new_rec_size;
	ulint		old_rec_size;
	ulint		max_ins_size = 0;
	dtuple_t*	new_entry;
	roll_ptr_t	roll_ptr;
	ulint		i;

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	ut_ad(trx_id > 0 || (flags & BTR_KEEP_SYS_FLAG));
	ut_ad(!!page_rec_is_comp(rec) == dict_table_is_comp(index->table));
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	/* This is intended only for leaf page updates */
	ut_ad(page_is_leaf(page));
	/* The insert buffer tree should never be updated in place. */
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(dict_index_is_online_ddl(index) == !!(flags & BTR_CREATE_FLAG)
	      || dict_index_is_clust(index));
	ut_ad(thr_get_trx(thr)->id == trx_id
	      || (flags & ulint(~(BTR_KEEP_POS_FLAG | BTR_KEEP_IBUF_BITMAP)))
	      == (BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG
		  | BTR_CREATE_FLAG | BTR_KEEP_SYS_FLAG));
	ut_ad(fil_page_index_page_check(page));
	ut_ad(btr_page_get_index_id(page) == index->id);

	*offsets = rec_get_offsets(rec, index, *offsets, true,
				   ULINT_UNDEFINED, heap);
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	ut_a(!rec_offs_any_null_extern(rec, *offsets)
	     || trx_is_recv(thr_get_trx(thr)));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

	if (UNIV_LIKELY(!update->is_metadata())
	    && !row_upd_changes_field_size_or_external(index, *offsets,
						       update)) {

		/* The simplest and the most common case: the update does not
		change the size of any field and none of the updated fields is
		externally stored in rec or update, and there is enough space
		on the compressed page to log the update. */

		return(btr_cur_update_in_place(
			       flags, cursor, *offsets, update,
			       cmpl_info, thr, trx_id, mtr));
	}

	if (rec_offs_any_extern(*offsets)) {
any_extern:
		/* Externally stored fields are treated in pessimistic
		update */

		/* prefetch siblings of the leaf for the pessimistic
		operation. */
		btr_cur_prefetch_siblings(block);

		return(DB_OVERFLOW);
	}

	if (rec_is_metadata(rec, *index) && index->table->instant) {
		goto any_extern;
	}

	for (i = 0; i < upd_get_n_fields(update); i++) {
		if (dfield_is_ext(&upd_get_nth_field(update, i)->new_val)) {

			goto any_extern;
		}
	}

	DBUG_LOG("ib_cur",
		 "update " << index->name << " (" << index->id << ") by "
		 << ib::hex(trx_id) << ": "
		 << rec_printer(rec, *offsets).str());

	page_cursor = btr_cur_get_page_cur(cursor);

	if (!*heap) {
		*heap = mem_heap_create(
			rec_offs_size(*offsets)
			+ DTUPLE_EST_ALLOC(rec_offs_n_fields(*offsets)));
	}

	new_entry = row_rec_to_index_entry(rec, index, *offsets, *heap);
	ut_ad(!dtuple_get_n_ext(new_entry));

	/* The page containing the clustered index record
	corresponding to new_entry is latched in mtr.
	Thus the following call is safe. */
	row_upd_index_replace_new_col_vals_index_pos(new_entry, index, update,
						     *heap);
	btr_cur_trim(new_entry, index, update, thr);
	old_rec_size = rec_offs_size(*offsets);
	new_rec_size = rec_get_converted_size(index, new_entry, 0);

	page_zip = buf_block_get_page_zip(block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	if (page_zip) {
		ut_ad(!index->table->is_temporary());

		if (page_zip_rec_needs_ext(new_rec_size, page_is_comp(page),
					   dict_index_get_n_fields(index),
					   block->zip_size())) {
			goto any_extern;
		}

		if (!btr_cur_update_alloc_zip(
			    page_zip, page_cursor, index, *offsets,
			    new_rec_size, true, mtr)) {
			return(DB_ZIP_OVERFLOW);
		}

		rec = page_cur_get_rec(page_cursor);
	}

	/* We limit max record size to 16k even for 64k page size. */
	if (new_rec_size >= COMPRESSED_REC_MAX_DATA_SIZE ||
			(!dict_table_is_comp(index->table)
			 && new_rec_size >= REDUNDANT_REC_MAX_DATA_SIZE)) {
		err = DB_OVERFLOW;

		goto func_exit;
	}

	if (UNIV_UNLIKELY(new_rec_size
			  >= (page_get_free_space_of_empty(page_is_comp(page))
			      / 2))) {
		/* We may need to update the IBUF_BITMAP_FREE
		bits after a reorganize that was done in
		btr_cur_update_alloc_zip(). */
		err = DB_OVERFLOW;
		goto func_exit;
	}

	if (UNIV_UNLIKELY(page_get_data_size(page)
			  - old_rec_size + new_rec_size
			  < BTR_CUR_PAGE_COMPRESS_LIMIT(index))) {
		/* We may need to update the IBUF_BITMAP_FREE
		bits after a reorganize that was done in
		btr_cur_update_alloc_zip(). */

		/* The page would become too empty */
		err = DB_UNDERFLOW;
		goto func_exit;
	}

	/* We do not attempt to reorganize if the page is compressed.
	This is because the page may fail to compress after reorganization. */
	max_size = page_zip
		? page_get_max_insert_size(page, 1)
		: (old_rec_size
		   + page_get_max_insert_size_after_reorganize(page, 1));

	if (!page_zip) {
		max_ins_size = page_get_max_insert_size_after_reorganize(
				page, 1);
	}

	if (!(((max_size >= BTR_CUR_PAGE_REORGANIZE_LIMIT)
	       && (max_size >= new_rec_size))
	      || (page_get_n_recs(page) <= 1))) {

		/* We may need to update the IBUF_BITMAP_FREE
		bits after a reorganize that was done in
		btr_cur_update_alloc_zip(). */

		/* There was not enough space, or it did not pay to
		reorganize: for simplicity, we decide what to do assuming a
		reorganization is needed, though it might not be necessary */

		err = DB_OVERFLOW;
		goto func_exit;
	}

	/* Do lock checking and undo logging */
	err = btr_cur_upd_lock_and_undo(flags, cursor, *offsets,
					update, cmpl_info,
					thr, mtr, &roll_ptr);
	if (err != DB_SUCCESS) {
		/* We may need to update the IBUF_BITMAP_FREE
		bits after a reorganize that was done in
		btr_cur_update_alloc_zip(). */
		goto func_exit;
	}

	/* Ok, we may do the replacement. Store on the page infimum the
	explicit locks on rec, before deleting rec (see the comment in
	btr_cur_pessimistic_update). */
	if (!dict_table_is_locking_disabled(index->table)) {
		lock_rec_store_on_page_infimum(block, rec);
	}

	if (UNIV_UNLIKELY(update->is_metadata())) {
		ut_ad(new_entry->is_metadata());
		ut_ad(index->is_instant());
		/* This can be innobase_add_instant_try() performing a
		subsequent instant ADD COLUMN, or its rollback by
		row_undo_mod_clust_low(). */
		ut_ad(flags & BTR_NO_LOCKING_FLAG);
	} else {
		btr_search_update_hash_on_delete(cursor);
	}

	page_cur_delete_rec(page_cursor, index, *offsets, mtr);

	page_cur_move_to_prev(page_cursor);

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		btr_cur_write_sys(new_entry, index, trx_id, roll_ptr);
	}

	/* There are no externally stored columns in new_entry */
	rec = btr_cur_insert_if_possible(
		cursor, new_entry, offsets, heap, 0/*n_ext*/, mtr);
	ut_a(rec); /* <- We calculated above the insert would fit */

	if (UNIV_UNLIKELY(update->is_metadata())) {
		/* We must empty the PAGE_FREE list, because if this
		was a rollback, the shortened metadata record
		would have too many fields, and we would be unable to
		know the size of the freed record. */
		btr_page_reorganize(page_cursor, index, mtr);
	} else if (!dict_table_is_locking_disabled(index->table)) {
		/* Restore the old explicit lock state on the record */
		lock_rec_restore_from_page_infimum(block, rec, block);
	}

	page_cur_move_to_next(page_cursor);
	ut_ad(err == DB_SUCCESS);

func_exit:
	if (!(flags & BTR_KEEP_IBUF_BITMAP)
	    && !dict_index_is_clust(index)) {
		/* Update the free bits in the insert buffer. */
		if (page_zip) {
			ut_ad(!index->table->is_temporary());
			ibuf_update_free_bits_zip(block, mtr);
		} else if (!index->table->is_temporary()) {
			ibuf_update_free_bits_low(block, max_ins_size, mtr);
		}
	}

	if (err != DB_SUCCESS) {
		/* prefetch siblings of the leaf for the pessimistic
		operation. */
		btr_cur_prefetch_siblings(block);
	}

	return(err);
}

/*************************************************************//**
If, in a split, a new supremum record was created as the predecessor of the
updated record, the supremum record must inherit exactly the locks on the
updated record. In the split it may have inherited locks from the successor
of the updated record, which is not correct. This function restores the
right locks for the new supremum. */
static
void
btr_cur_pess_upd_restore_supremum(
/*==============================*/
	buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*	rec,	/*!< in: updated record */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*		page;
	buf_block_t*	prev_block;

	page = buf_block_get_frame(block);

	if (page_rec_get_next(page_get_infimum_rec(page)) != rec) {
		/* Updated record is not the first user record on its page */

		return;
	}

	const uint32_t	prev_page_no = btr_page_get_prev(page);

	const page_id_t	page_id(block->page.id.space(), prev_page_no);

	ut_ad(prev_page_no != FIL_NULL);
	prev_block = buf_page_get_with_no_latch(page_id, block->zip_size(),
						mtr);
#ifdef UNIV_BTR_DEBUG
	ut_a(btr_page_get_next(prev_block->frame) == block->page.id.page_no());
#endif /* UNIV_BTR_DEBUG */

	/* We must already have an x-latch on prev_block! */
	ut_ad(mtr_memo_contains(mtr, prev_block, MTR_MEMO_PAGE_X_FIX));

	lock_rec_reset_and_inherit_gap_locks(prev_block, block,
					     PAGE_HEAP_NO_SUPREMUM,
					     page_rec_get_heap_no(rec));
}

/*************************************************************//**
Performs an update of a record on a page of a tree. It is assumed
that mtr holds an x-latch on the tree and on the cursor page. If the
update is made on the leaf level, to avoid deadlocks, mtr must also
own x-latches to brothers of page, if those brothers exist. We assume
here that the ordering fields of the record do not change.
@return DB_SUCCESS or error code */
dberr_t
btr_cur_pessimistic_update(
/*=======================*/
	ulint		flags,	/*!< in: undo logging, locking, and rollback
				flags */
	btr_cur_t*	cursor,	/*!< in/out: cursor on the record to update;
				cursor may become invalid if *big_rec == NULL
				|| !(flags & BTR_KEEP_POS_FLAG) */
	offset_t**	offsets,/*!< out: offsets on cursor->page_cur.rec */
	mem_heap_t**	offsets_heap,
				/*!< in/out: pointer to memory heap
				that can be emptied */
	mem_heap_t*	entry_heap,
				/*!< in/out: memory heap for allocating
				big_rec and the index tuple */
	big_rec_t**	big_rec,/*!< out: big rec vector whose fields have to
				be stored externally by the caller */
	upd_t*		update,	/*!< in/out: update vector; this is allowed to
				also contain trx id and roll ptr fields.
				Non-updated columns that are moved offpage will
				be appended to this. */
	ulint		cmpl_info,/*!< in: compiler info on secondary index
				updates */
	que_thr_t*	thr,	/*!< in: query thread */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction; must be
				committed before latching any further pages */
{
	big_rec_t*	big_rec_vec	= NULL;
	big_rec_t*	dummy_big_rec;
	dict_index_t*	index;
	buf_block_t*	block;
	page_zip_des_t*	page_zip;
	rec_t*		rec;
	page_cur_t*	page_cursor;
	dberr_t		err;
	dberr_t		optim_err;
	roll_ptr_t	roll_ptr;
	ibool		was_first;
	ulint		n_reserved	= 0;

	*offsets = NULL;
	*big_rec = NULL;

	block = btr_cur_get_block(cursor);
	page_zip = buf_block_get_page_zip(block);
	index = cursor->index;

	ut_ad(mtr_memo_contains_flagged(mtr, dict_index_get_lock(index),
					MTR_MEMO_X_LOCK |
					MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, block->frame, index));
#endif /* UNIV_ZIP_DEBUG */
	ut_ad(!page_zip || !index->table->is_temporary());
	/* The insert buffer tree should never be updated in place. */
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(trx_id > 0
	      || (flags & BTR_KEEP_SYS_FLAG));
	ut_ad(dict_index_is_online_ddl(index) == !!(flags & BTR_CREATE_FLAG)
	      || dict_index_is_clust(index));
	ut_ad(thr_get_trx(thr)->id == trx_id
	      || (flags & ulint(~BTR_KEEP_POS_FLAG))
	      == (BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG
		  | BTR_CREATE_FLAG | BTR_KEEP_SYS_FLAG));

	err = optim_err = btr_cur_optimistic_update(
		flags | BTR_KEEP_IBUF_BITMAP,
		cursor, offsets, offsets_heap, update,
		cmpl_info, thr, trx_id, mtr);

	switch (err) {
	case DB_ZIP_OVERFLOW:
	case DB_UNDERFLOW:
	case DB_OVERFLOW:
		break;
	default:
	err_exit:
		/* We suppressed this with BTR_KEEP_IBUF_BITMAP.
		For DB_ZIP_OVERFLOW, the IBUF_BITMAP_FREE bits were
		already reset by btr_cur_update_alloc_zip() if the
		page was recompressed. */
		if (page_zip
		    && optim_err != DB_ZIP_OVERFLOW
		    && !dict_index_is_clust(index)
		    && page_is_leaf(block->frame)) {
			ut_ad(!index->table->is_temporary());
			ibuf_update_free_bits_zip(block, mtr);
		}

		if (big_rec_vec != NULL) {
			dtuple_big_rec_free(big_rec_vec);
		}

		return(err);
	}

	rec = btr_cur_get_rec(cursor);
	ut_ad(rec_offs_validate(rec, index, *offsets));

	dtuple_t* new_entry;

	const bool is_metadata = rec_is_metadata(rec, *index);

	if (UNIV_UNLIKELY(is_metadata)) {
		ut_ad(update->is_metadata());
		ut_ad(flags & BTR_NO_LOCKING_FLAG);
		ut_ad(index->is_instant());
		new_entry = row_metadata_to_tuple(
			rec, index, *offsets, entry_heap,
			update->info_bits, !thr_get_trx(thr)->in_rollback);
		ut_ad(new_entry->n_fields
		      == ulint(index->n_fields)
		      + update->is_alter_metadata());
	} else {
		new_entry = row_rec_to_index_entry(rec, index, *offsets,
						   entry_heap);
	}

	/* The page containing the clustered index record
	corresponding to new_entry is latched in mtr.  If the
	clustered index record is delete-marked, then its externally
	stored fields cannot have been purged yet, because then the
	purge would also have removed the clustered index record
	itself.  Thus the following call is safe. */
	row_upd_index_replace_new_col_vals_index_pos(new_entry, index, update,
						     entry_heap);
	btr_cur_trim(new_entry, index, update, thr);

	/* We have to set appropriate extern storage bits in the new
	record to be inserted: we have to remember which fields were such */

	ut_ad(!page_is_comp(block->frame) || !rec_get_node_ptr_flag(rec));
	ut_ad(rec_offs_validate(rec, index, *offsets));

	if ((flags & BTR_NO_UNDO_LOG_FLAG)
	    && rec_offs_any_extern(*offsets)) {
		/* We are in a transaction rollback undoing a row
		update: we must free possible externally stored fields
		which got new values in the update, if they are not
		inherited values. They can be inherited if we have
		updated the primary key to another value, and then
		update it back again. */

		ut_ad(big_rec_vec == NULL);
		ut_ad(dict_index_is_clust(index));
		ut_ad(thr_get_trx(thr)->in_rollback);

		DEBUG_SYNC_C("blob_rollback_middle");

		btr_rec_free_updated_extern_fields(
			index, rec, block, *offsets, update, true, mtr);
	}

	ulint n_ext = index->is_primary() ? dtuple_get_n_ext(new_entry) : 0;

	if (page_zip_rec_needs_ext(
		    rec_get_converted_size(index, new_entry, n_ext),
		    page_is_comp(block->frame),
		    dict_index_get_n_fields(index),
		    block->zip_size())
	    || (UNIV_UNLIKELY(update->is_alter_metadata())
		&& !dfield_is_ext(dtuple_get_nth_field(
					  new_entry,
					  index->first_user_field())))) {
		big_rec_vec = dtuple_convert_big_rec(index, update, new_entry, &n_ext);
		if (UNIV_UNLIKELY(big_rec_vec == NULL)) {

			/* We cannot goto return_after_reservations,
			because we may need to update the
			IBUF_BITMAP_FREE bits, which was suppressed by
			BTR_KEEP_IBUF_BITMAP. */
#ifdef UNIV_ZIP_DEBUG
			ut_a(!page_zip
			     || page_zip_validate(page_zip, block->frame,
						  index));
#endif /* UNIV_ZIP_DEBUG */
			index->table->space->release_free_extents(n_reserved);
			err = DB_TOO_BIG_RECORD;
			goto err_exit;
		}

		ut_ad(page_is_leaf(block->frame));
		ut_ad(dict_index_is_clust(index));
		ut_ad(flags & BTR_KEEP_POS_FLAG);
	}

	/* Do lock checking and undo logging */
	err = btr_cur_upd_lock_and_undo(flags, cursor, *offsets,
					update, cmpl_info,
					thr, mtr, &roll_ptr);
	if (err != DB_SUCCESS) {
		goto err_exit;
	}

	if (optim_err == DB_OVERFLOW) {

		/* First reserve enough free space for the file segments
		of the index tree, so that the update will not fail because
		of lack of space */

		ulint	n_extents = cursor->tree_height / 16 + 3;

		if (!fsp_reserve_free_extents(
		            &n_reserved, index->table->space, n_extents,
		            flags & BTR_NO_UNDO_LOG_FLAG
		            ? FSP_CLEANING : FSP_NORMAL,
		            mtr)) {
			err = DB_OUT_OF_FILE_SPACE;
			goto err_exit;
		}
	}

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		btr_cur_write_sys(new_entry, index, trx_id, roll_ptr);
	}

	const ulint max_ins_size = page_zip
		? 0 : page_get_max_insert_size_after_reorganize(block->frame,
								1);

	if (UNIV_UNLIKELY(is_metadata)) {
		ut_ad(new_entry->is_metadata());
		ut_ad(index->is_instant());
		/* This can be innobase_add_instant_try() performing a
		subsequent instant ALTER TABLE, or its rollback by
		row_undo_mod_clust_low(). */
		ut_ad(flags & BTR_NO_LOCKING_FLAG);
	} else {
		btr_search_update_hash_on_delete(cursor);

		/* Store state of explicit locks on rec on the page
		infimum record, before deleting rec. The page infimum
		acts as a dummy carrier of the locks, taking care also
		of lock releases, before we can move the locks back on
		the actual record. There is a special case: if we are
		inserting on the root page and the insert causes a
		call of btr_root_raise_and_insert. Therefore we cannot
		in the lock system delete the lock structs set on the
		root page even if the root page carries just node
		pointers. */
		if (!dict_table_is_locking_disabled(index->table)) {
			lock_rec_store_on_page_infimum(block, rec);
		}
	}

#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, block->frame, index));
#endif /* UNIV_ZIP_DEBUG */
	page_cursor = btr_cur_get_page_cur(cursor);

	page_cur_delete_rec(page_cursor, index, *offsets, mtr);

	page_cur_move_to_prev(page_cursor);

	rec = btr_cur_insert_if_possible(cursor, new_entry,
					 offsets, offsets_heap, n_ext, mtr);

	if (rec) {
		page_cursor->rec = rec;

		if (UNIV_UNLIKELY(is_metadata)) {
			/* We must empty the PAGE_FREE list, because if this
			was a rollback, the shortened metadata record
			would have too many fields, and we would be unable to
			know the size of the freed record. */
			btr_page_reorganize(page_cursor, index, mtr);
			rec = page_cursor->rec;
			rec_offs_make_valid(rec, index, true, *offsets);
		} else if (!dict_table_is_locking_disabled(index->table)) {
			lock_rec_restore_from_page_infimum(
				btr_cur_get_block(cursor), rec, block);
		}

		if (!rec_get_deleted_flag(rec, rec_offs_comp(*offsets))
		    || rec_is_alter_metadata(rec, *index)) {
			/* The new inserted record owns its possible externally
			stored fields */
			btr_cur_unmark_extern_fields(btr_cur_get_block(cursor),
						     rec, index, *offsets, mtr);
		} else {
			/* In delete-marked records, DB_TRX_ID must
			always refer to an existing undo log record. */
			ut_ad(row_get_rec_trx_id(rec, index, *offsets));
		}

		bool adjust = big_rec_vec && (flags & BTR_KEEP_POS_FLAG);
		ut_ad(!adjust || page_is_leaf(block->frame));

		if (btr_cur_compress_if_useful(cursor, adjust, mtr)) {
			if (adjust) {
				rec_offs_make_valid(page_cursor->rec, index,
						    true, *offsets);
			}
		} else if (!dict_index_is_clust(index)
			   && page_is_leaf(block->frame)) {
			/* Update the free bits in the insert buffer.
			This is the same block which was skipped by
			BTR_KEEP_IBUF_BITMAP. */
			if (page_zip) {
				ut_ad(!index->table->is_temporary());
				ibuf_update_free_bits_zip(block, mtr);
			} else if (!index->table->is_temporary()) {
				ibuf_update_free_bits_low(block, max_ins_size,
							  mtr);
			}
		}

		if (!srv_read_only_mode
		    && !big_rec_vec
		    && page_is_leaf(block->frame)
		    && !dict_index_is_online_ddl(index)) {

			mtr_memo_release(mtr, dict_index_get_lock(index),
					 MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK);

			/* NOTE: We cannot release root block latch here, because it
			has segment header and already modified in most of cases.*/
		}

		err = DB_SUCCESS;
		goto return_after_reservations;
	} else {
		/* If the page is compressed and it initially
		compresses very well, and there is a subsequent insert
		of a badly-compressing record, it is possible for
		btr_cur_optimistic_update() to return DB_UNDERFLOW and
		btr_cur_insert_if_possible() to return FALSE. */
		ut_a(page_zip || optim_err != DB_UNDERFLOW);

		/* Out of space: reset the free bits.
		This is the same block which was skipped by
		BTR_KEEP_IBUF_BITMAP. */
		if (!dict_index_is_clust(index)
		    && !index->table->is_temporary()
		    && page_is_leaf(block->frame)) {
			ibuf_reset_free_bits(block);
		}
	}

	if (big_rec_vec != NULL) {
		ut_ad(page_is_leaf(block->frame));
		ut_ad(dict_index_is_clust(index));
		ut_ad(flags & BTR_KEEP_POS_FLAG);

		/* btr_page_split_and_insert() in
		btr_cur_pessimistic_insert() invokes
		mtr_memo_release(mtr, index->lock, MTR_MEMO_SX_LOCK).
		We must keep the index->lock when we created a
		big_rec, so that row_upd_clust_rec() can store the
		big_rec in the same mini-transaction. */

		ut_ad(mtr_memo_contains_flagged(mtr,
						dict_index_get_lock(index),
						MTR_MEMO_X_LOCK |
						MTR_MEMO_SX_LOCK));

		mtr_sx_lock_index(index, mtr);
	}

	/* Was the record to be updated positioned as the first user
	record on its page? */
	was_first = page_cur_is_before_first(page_cursor);

	/* Lock checks and undo logging were already performed by
	btr_cur_upd_lock_and_undo(). We do not try
	btr_cur_optimistic_insert() because
	btr_cur_insert_if_possible() already failed above. */

	err = btr_cur_pessimistic_insert(BTR_NO_UNDO_LOG_FLAG
					 | BTR_NO_LOCKING_FLAG
					 | BTR_KEEP_SYS_FLAG,
					 cursor, offsets, offsets_heap,
					 new_entry, &rec,
					 &dummy_big_rec, n_ext, NULL, mtr);
	ut_a(rec);
	ut_a(err == DB_SUCCESS);
	ut_a(dummy_big_rec == NULL);
	ut_ad(rec_offs_validate(rec, cursor->index, *offsets));
	page_cursor->rec = rec;

	/* Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (dict_index_is_sec_or_ibuf(index)
	    && !index->table->is_temporary()) {
		/* Update PAGE_MAX_TRX_ID in the index page header.
		It was not updated by btr_cur_pessimistic_insert()
		because of BTR_NO_LOCKING_FLAG. */
		page_update_max_trx_id(btr_cur_get_block(cursor),
				       btr_cur_get_page_zip(cursor),
				       trx_id, mtr);
	}

	if (!rec_get_deleted_flag(rec, rec_offs_comp(*offsets))) {
		/* The new inserted record owns its possible externally
		stored fields */
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, block->frame,
						    index));
#endif /* UNIV_ZIP_DEBUG */
		btr_cur_unmark_extern_fields(btr_cur_get_block(cursor), rec,
					     index, *offsets, mtr);
	} else {
		/* In delete-marked records, DB_TRX_ID must
		always refer to an existing undo log record. */
		ut_ad(row_get_rec_trx_id(rec, index, *offsets));
	}

	if (UNIV_UNLIKELY(is_metadata)) {
		/* We must empty the PAGE_FREE list, because if this
		was a rollback, the shortened metadata record
		would have too many fields, and we would be unable to
		know the size of the freed record. */
		btr_page_reorganize(page_cursor, index, mtr);
		rec = page_cursor->rec;
	} else if (!dict_table_is_locking_disabled(index->table)) {
		lock_rec_restore_from_page_infimum(
			btr_cur_get_block(cursor), rec, block);
	}

	/* If necessary, restore also the correct lock state for a new,
	preceding supremum record created in a page split. While the old
	record was nonexistent, the supremum might have inherited its locks
	from a wrong record. */

	if (!was_first && !dict_table_is_locking_disabled(index->table)) {
		btr_cur_pess_upd_restore_supremum(btr_cur_get_block(cursor),
						  rec, mtr);
	}

return_after_reservations:
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(btr_cur_get_page_zip(cursor),
					    btr_cur_get_page(cursor), index));
#endif /* UNIV_ZIP_DEBUG */

	index->table->space->release_free_extents(n_reserved);
	*big_rec = big_rec_vec;
	return(err);
}

/*==================== B-TREE DELETE MARK AND UNMARK ===============*/

/** Modify the delete-mark flag of a record.
@tparam         flag    the value of the delete-mark flag
@param[in,out]  block   buffer block
@param[in,out]  rec     record on a physical index page
@param[in,out]  mtr     mini-transaction  */
template<bool flag>
void btr_rec_set_deleted(buf_block_t *block, rec_t *rec, mtr_t *mtr)
{
  if (page_rec_is_comp(rec))
  {
    byte *b= &rec[-REC_NEW_INFO_BITS];
    const byte v= flag
      ? (*b | REC_INFO_DELETED_FLAG)
      : (*b & byte(~REC_INFO_DELETED_FLAG));
    if (*b == v);
    else if (UNIV_LIKELY_NULL(block->page.zip.data))
    {
      *b= v;
      page_zip_rec_set_deleted(block, rec, flag, mtr);
    }
    else
      mtr->write<1>(*block, b, v);
  }
  else
  {
    ut_ad(!block->page.zip.data);
    byte *b= &rec[-REC_OLD_INFO_BITS];
    const byte v = flag
      ? (*b | REC_INFO_DELETED_FLAG)
      : (*b & byte(~REC_INFO_DELETED_FLAG));
    mtr->write<1,mtr_t::OPT>(*block, b, v);
  }
}

template void btr_rec_set_deleted<false>(buf_block_t *, rec_t *, mtr_t *);
template void btr_rec_set_deleted<true>(buf_block_t *, rec_t *, mtr_t *);

/***********************************************************//**
Marks a clustered index record deleted. Writes an undo log record to
undo log on this delete marking. Writes in the trx id field the id
of the deleting transaction, and in the roll ptr field pointer to the
undo log record created.
@return DB_SUCCESS, DB_LOCK_WAIT, or error number */
dberr_t
btr_cur_del_mark_set_clust_rec(
/*===========================*/
	buf_block_t*	block,	/*!< in/out: buffer block of the record */
	rec_t*		rec,	/*!< in/out: record */
	dict_index_t*	index,	/*!< in: clustered index of the record */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec) */
	que_thr_t*	thr,	/*!< in: query thread */
	const dtuple_t*	entry,	/*!< in: dtuple for the deleting record, also
				contains the virtual cols if there are any */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	roll_ptr_t	roll_ptr;
	dberr_t		err;
	trx_t*		trx;

	ut_ad(dict_index_is_clust(index));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!!page_rec_is_comp(rec) == dict_table_is_comp(index->table));
	ut_ad(buf_block_get_frame(block) == page_align(rec));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(mtr->is_named_space(index->table->space));

	if (rec_get_deleted_flag(rec, rec_offs_comp(offsets))) {
		/* We may already have delete-marked this record
		when executing an ON DELETE CASCADE operation. */
		ut_ad(row_get_rec_trx_id(rec, index, offsets)
		      == thr_get_trx(thr)->id);
		return(DB_SUCCESS);
	}

	err = lock_clust_rec_modify_check_and_lock(BTR_NO_LOCKING_FLAG, block,
						   rec, index, offsets, thr);

	if (err != DB_SUCCESS) {

		return(err);
	}

	err = trx_undo_report_row_operation(thr, index,
					    entry, NULL, 0, rec, offsets,
					    &roll_ptr);
	if (err != DB_SUCCESS) {

		return(err);
	}

	/* The search latch is not needed here, because
	the adaptive hash index does not depend on the delete-mark
	and the delete-mark is being updated in place. */

	btr_rec_set_deleted<true>(block, rec, mtr);

	trx = thr_get_trx(thr);

	DBUG_LOG("ib_cur",
		 "delete-mark clust " << index->table->name
		 << " (" << index->id << ") by "
		 << ib::hex(trx_get_id_for_print(trx)) << ": "
		 << rec_printer(rec, offsets).str());

	if (dict_index_is_online_ddl(index)) {
		row_log_table_delete(rec, index, offsets, NULL);
	}

	btr_cur_upd_rec_sys(block, rec, index, offsets, trx, roll_ptr, mtr);
	return(err);
}

/*==================== B-TREE RECORD REMOVE =========================*/

/*************************************************************//**
Tries to compress a page of the tree if it seems useful. It is assumed
that mtr holds an x-latch on the tree and on the cursor page. To avoid
deadlocks, mtr must also own x-latches to brothers of page, if those
brothers exist. NOTE: it is assumed that the caller has reserved enough
free extents so that the compression will always succeed if done!
@return TRUE if compression occurred */
ibool
btr_cur_compress_if_useful(
/*=======================*/
	btr_cur_t*	cursor,	/*!< in/out: cursor on the page to compress;
				cursor does not stay valid if !adjust and
				compression occurs */
	ibool		adjust,	/*!< in: TRUE if should adjust the
				cursor position even if compression occurs */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(mtr_memo_contains_flagged(
		mtr, dict_index_get_lock(btr_cur_get_index(cursor)),
		MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains(mtr, btr_cur_get_block(cursor),
			       MTR_MEMO_PAGE_X_FIX));

	if (cursor->index->is_spatial()) {
		const trx_t*	trx = cursor->rtr_info->thr
			? thr_get_trx(cursor->rtr_info->thr)
			: NULL;
		const buf_block_t* block = btr_cur_get_block(cursor);

		/* Check whether page lock prevents the compression */
		if (!lock_test_prdt_page_lock(trx, block->page.id.space(),
					      block->page.id.page_no())) {
			return(false);
		}
	}

	return(btr_cur_compress_recommendation(cursor, mtr)
	       && btr_compress(cursor, adjust, mtr));
}

/*******************************************************//**
Removes the record on which the tree cursor is positioned on a leaf page.
It is assumed that the mtr has an x-latch on the page where the cursor is
positioned, but no latch on the whole tree.
@return TRUE if success, i.e., the page did not become too empty */
ibool
btr_cur_optimistic_delete_func(
/*===========================*/
	btr_cur_t*	cursor,	/*!< in: cursor on leaf page, on the record to
				delete; cursor stays valid: if deletion
				succeeds, on function exit it points to the
				successor of the deleted record */
#ifdef UNIV_DEBUG
	ulint		flags,	/*!< in: BTR_CREATE_FLAG or 0 */
#endif /* UNIV_DEBUG */
	mtr_t*		mtr)	/*!< in: mtr; if this function returns
				TRUE on a leaf page of a secondary
				index, the mtr must be committed
				before latching any further pages */
{
	buf_block_t*	block;
	rec_t*		rec;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	ibool		no_compress_needed;
	rec_offs_init(offsets_);

	ut_ad(flags == 0 || flags == BTR_CREATE_FLAG);
	ut_ad(mtr_memo_contains(mtr, btr_cur_get_block(cursor),
				MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->is_named_space(cursor->index->table->space));
	ut_ad(!cursor->index->is_dummy);

	/* This is intended only for leaf page deletions */

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.id.space() == cursor->index->table->space->id);
	ut_ad(page_is_leaf(buf_block_get_frame(block)));
	ut_ad(!dict_index_is_online_ddl(cursor->index)
	      || dict_index_is_clust(cursor->index)
	      || (flags & BTR_CREATE_FLAG));

	rec = btr_cur_get_rec(cursor);

	if (UNIV_UNLIKELY(block->page.id.page_no() == cursor->index->page
			  && page_get_n_recs(block->frame) == 1
			  + (cursor->index->is_instant()
			     && !rec_is_metadata(rec, *cursor->index)))) {
		/* The whole index (and table) becomes logically empty.
		Empty the whole page. That is, if we are deleting the
		only user record, also delete the metadata record
		if one exists for instant ADD COLUMN (not generic ALTER TABLE).
		If we are deleting the metadata record and the
		table becomes empty, clean up the whole page. */
		dict_index_t* index = cursor->index;
		const rec_t* first_rec = page_rec_get_next_const(
			page_get_infimum_rec(block->frame));
		ut_ad(!index->is_instant()
		      || rec_is_metadata(first_rec, *index));
		const bool is_metadata = rec_is_metadata(rec, *index);
		/* We can remove the metadata when rolling back an
		instant ALTER TABLE operation, or when deleting the
		last user record on the page such that only metadata for
		instant ADD COLUMN (not generic ALTER TABLE) remains. */
		const bool empty_table = is_metadata
			|| !index->is_instant()
			|| (first_rec != rec
			    && rec_is_add_metadata(first_rec, *index));
		if (UNIV_LIKELY(empty_table)) {
			if (UNIV_LIKELY(!is_metadata)) {
				lock_update_delete(block, rec);
			}
			btr_page_empty(block, buf_block_get_page_zip(block),
				       index, 0, mtr);
			if (index->is_instant()) {
				/* MDEV-17383: free metadata BLOBs! */
				index->clear_instant_alter();
			}
			page_cur_set_after_last(block,
						btr_cur_get_page_cur(cursor));
			return true;
		}
	}

	offsets = rec_get_offsets(rec, cursor->index, offsets, true,
				  ULINT_UNDEFINED, &heap);

	no_compress_needed = !rec_offs_any_extern(offsets)
		&& btr_cur_can_delete_without_compress(
			cursor, rec_offs_size(offsets), mtr);

	if (no_compress_needed) {

		page_t*		page	= buf_block_get_frame(block);
		page_zip_des_t*	page_zip= buf_block_get_page_zip(block);

		if (UNIV_UNLIKELY(rec_get_info_bits(rec, page_rec_is_comp(rec))
				  & REC_INFO_MIN_REC_FLAG)) {
			/* This should be rolling back instant ADD COLUMN.
			If this is a recovered transaction, then
			index->is_instant() will hold until the
			insert into SYS_COLUMNS is rolled back. */
			ut_ad(cursor->index->table->supports_instant());
			ut_ad(cursor->index->is_primary());
			ut_ad(!page_zip);
			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    cursor->index, offsets, mtr);
			/* We must empty the PAGE_FREE list, because
			after rollback, this deleted metadata record
			would have too many fields, and we would be
			unable to know the size of the freed record. */
			btr_page_reorganize(btr_cur_get_page_cur(cursor),
					    cursor->index, mtr);
			goto func_exit;
		} else {
			lock_update_delete(block, rec);

			btr_search_update_hash_on_delete(cursor);
		}

		if (page_zip) {
#ifdef UNIV_ZIP_DEBUG
			ut_a(page_zip_validate(page_zip, page, cursor->index));
#endif /* UNIV_ZIP_DEBUG */
			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    cursor->index, offsets, mtr);
#ifdef UNIV_ZIP_DEBUG
			ut_a(page_zip_validate(page_zip, page, cursor->index));
#endif /* UNIV_ZIP_DEBUG */

			/* On compressed pages, the IBUF_BITMAP_FREE
			space is not affected by deleting (purging)
			records, because it is defined as the minimum
			of space available *without* reorganize, and
			space available in the modification log. */
		} else {
			const ulint	max_ins
				= page_get_max_insert_size_after_reorganize(
					page, 1);

			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    cursor->index, offsets, mtr);

			/* The change buffer does not handle inserts
			into non-leaf pages, into clustered indexes,
			or into the change buffer. */
			if (!dict_index_is_clust(cursor->index)
			    && !cursor->index->table->is_temporary()
			    && !dict_index_is_ibuf(cursor->index)) {
				ibuf_update_free_bits_low(block, max_ins, mtr);
			}
		}
	} else {
		/* prefetch siblings of the leaf for the pessimistic
		operation. */
		btr_cur_prefetch_siblings(block);
	}

func_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return(no_compress_needed);
}

/*************************************************************//**
Removes the record on which the tree cursor is positioned. Tries
to compress the page if its fillfactor drops below a threshold
or if it is the only page on the level. It is assumed that mtr holds
an x-latch on the tree and on the cursor page. To avoid deadlocks,
mtr must also own x-latches to brothers of page, if those brothers
exist.
@return TRUE if compression occurred and FALSE if not or something
wrong. */
ibool
btr_cur_pessimistic_delete(
/*=======================*/
	dberr_t*	err,	/*!< out: DB_SUCCESS or DB_OUT_OF_FILE_SPACE;
				the latter may occur because we may have
				to update node pointers on upper levels,
				and in the case of variable length keys
				these may actually grow in size */
	ibool		has_reserved_extents, /*!< in: TRUE if the
				caller has already reserved enough free
				extents so that he knows that the operation
				will succeed */
	btr_cur_t*	cursor,	/*!< in: cursor on the record to delete;
				if compression does not occur, the cursor
				stays valid: it points to successor of
				deleted record on function exit */
	ulint		flags,	/*!< in: BTR_CREATE_FLAG or 0 */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mtr */
{
	buf_block_t*	block;
	page_t*		page;
	page_zip_des_t*	page_zip;
	dict_index_t*	index;
	rec_t*		rec;
	ulint		n_reserved	= 0;
	bool		success;
	ibool		ret		= FALSE;
	mem_heap_t*	heap;
	offset_t*	offsets;
#ifdef UNIV_DEBUG
	bool		parent_latched	= false;
#endif /* UNIV_DEBUG */

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	index = btr_cur_get_index(cursor);

	ut_ad(flags == 0 || flags == BTR_CREATE_FLAG);
	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));
	ut_ad(mtr_memo_contains_flagged(mtr, dict_index_get_lock(index),
					MTR_MEMO_X_LOCK
					| MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->is_named_space(index->table->space));
	ut_ad(!index->is_dummy);
	ut_ad(block->page.id.space() == index->table->space->id);

	if (!has_reserved_extents) {
		/* First reserve enough free space for the file segments
		of the index tree, so that the node pointer updates will
		not fail because of lack of space */

		ulint	n_extents = cursor->tree_height / 32 + 1;

		success = fsp_reserve_free_extents(&n_reserved,
						   index->table->space,
						   n_extents,
						   FSP_CLEANING, mtr);
		if (!success) {
			*err = DB_OUT_OF_FILE_SPACE;

			return(FALSE);
		}
	}

	heap = mem_heap_create(1024);
	rec = btr_cur_get_rec(cursor);
	page_zip = buf_block_get_page_zip(block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	offsets = rec_get_offsets(rec, index, NULL, page_is_leaf(page),
				  ULINT_UNDEFINED, &heap);

	if (rec_offs_any_extern(offsets)) {
		btr_rec_free_externally_stored_fields(index,
						      rec, offsets, block,
						      rollback, mtr);
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
	}

	rec_t* next_rec = NULL;
	bool min_mark_next_rec = false;

	if (page_is_leaf(page)) {
		const bool is_metadata = rec_is_metadata(
			rec, page_rec_is_comp(rec));
		if (UNIV_UNLIKELY(is_metadata)) {
			/* This should be rolling back instant ALTER TABLE.
			If this is a recovered transaction, then
			index->is_instant() will hold until the
			insert into SYS_COLUMNS is rolled back. */
			ut_ad(rollback);
			ut_ad(index->table->supports_instant());
			ut_ad(index->is_primary());
		} else if (flags == 0) {
			lock_update_delete(block, rec);
		}

		if (block->page.id.page_no() != index->page) {
			if (page_get_n_recs(page) < 2) {
				goto discard_page;
			}
		} else if (page_get_n_recs(page) == 1
			   + (index->is_instant() && !is_metadata)) {
			/* The whole index (and table) becomes logically empty.
			Empty the whole page. That is, if we are deleting the
			only user record, also delete the metadata record
			if one exists for instant ADD COLUMN
			(not generic ALTER TABLE).
			If we are deleting the metadata record
			(in the rollback of instant ALTER TABLE) and the
			table becomes empty, clean up the whole page. */

			const rec_t* first_rec = page_rec_get_next_const(
				page_get_infimum_rec(page));
			ut_ad(!index->is_instant()
			      || rec_is_metadata(first_rec, *index));
			if (is_metadata || !index->is_instant()
			    || (first_rec != rec
				&& rec_is_add_metadata(first_rec, *index))) {
				btr_page_empty(block, page_zip, index, 0, mtr);
				if (index->is_instant()) {
					/* MDEV-17383: free metadata BLOBs! */
					index->clear_instant_alter();
				}
				page_cur_set_after_last(
					block,
					btr_cur_get_page_cur(cursor));
				ret = TRUE;
				goto return_after_reservations;
			}
		}

		if (UNIV_LIKELY(!is_metadata)) {
			btr_search_update_hash_on_delete(cursor);
		} else {
			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    index, offsets, mtr);
			/* We must empty the PAGE_FREE list, because
			after rollback, this deleted metadata record
			would carry too many fields, and we would be
			unable to know the size of the freed record. */
			btr_page_reorganize(btr_cur_get_page_cur(cursor),
					    index, mtr);
			ut_ad(!ret);
			goto return_after_reservations;
		}
	} else if (UNIV_UNLIKELY(page_rec_is_first(rec, page))) {
		if (page_rec_is_last(rec, page)) {
discard_page:
			ut_ad(page_get_n_recs(page) == 1);
			/* If there is only one record, drop
			the whole page. */

			btr_discard_page(cursor, mtr);

			ret = TRUE;
			goto return_after_reservations;
		}

		next_rec = page_rec_get_next(rec);

		if (!page_has_prev(page)) {
			/* If we delete the leftmost node pointer on a
			non-leaf level, we must mark the new leftmost node
			pointer as the predefined minimum record */

			min_mark_next_rec = true;
		} else if (dict_index_is_spatial(index)) {
			/* For rtree, if delete the leftmost node pointer,
			we need to update parent page. */
			rtr_mbr_t	father_mbr;
			rec_t*		father_rec;
			btr_cur_t	father_cursor;
			offset_t*	offsets;
			bool		upd_ret;
			ulint		len;

			rtr_page_get_father_block(NULL, heap, index,
						  block, mtr, NULL,
						  &father_cursor);
			offsets = rec_get_offsets(
				btr_cur_get_rec(&father_cursor), index, NULL,
				false, ULINT_UNDEFINED, &heap);

			father_rec = btr_cur_get_rec(&father_cursor);
			rtr_read_mbr(rec_get_nth_field(
				father_rec, offsets, 0, &len), &father_mbr);

			upd_ret = rtr_update_mbr_field(&father_cursor, offsets,
						       NULL, page, &father_mbr,
						       next_rec, mtr);

			if (!upd_ret) {
				*err = DB_ERROR;

				mem_heap_free(heap);
				return(FALSE);
			}

			ut_d(parent_latched = true);
		} else {
			/* Otherwise, if we delete the leftmost node pointer
			on a page, we have to change the parent node pointer
			so that it is equal to the new leftmost node pointer
			on the page */
			btr_cur_t cursor;
			btr_page_get_father(index, block, mtr, &cursor);
			btr_cur_node_ptr_delete(&cursor, mtr);
			const ulint	level = btr_page_get_level(page);
			// FIXME: reuse the node_ptr from above
			dtuple_t*	node_ptr = dict_index_build_node_ptr(
				index, next_rec, block->page.id.page_no(),
				heap, level);

			btr_insert_on_non_leaf_level(
				flags, index, level + 1, node_ptr, mtr);

			ut_d(parent_latched = true);
		}
	}

	/* SPATIAL INDEX never use SX locks; we can allow page merges
	while holding X lock on the spatial index tree.
	Do not allow merges of non-leaf B-tree pages unless it is
	safe to do so. */
	{
		const bool allow_merge = page_is_leaf(page)
			|| dict_index_is_spatial(index)
			|| btr_cur_will_modify_tree(
				index, page, BTR_INTENTION_DELETE, rec,
				btr_node_ptr_max_size(index),
				block->zip_size(), mtr);
		page_cur_delete_rec(btr_cur_get_page_cur(cursor), index,
				    offsets, mtr);

		if (min_mark_next_rec) {
			btr_set_min_rec_mark(next_rec, *block, mtr);
		}

#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

		ut_ad(!parent_latched
		      || btr_check_node_ptr(index, block, mtr));

		if (!ret && btr_cur_compress_recommendation(cursor, mtr)) {
			if (UNIV_LIKELY(allow_merge)) {
				ret = btr_cur_compress_if_useful(
					cursor, FALSE, mtr);
			} else {
				ib::warn() << "Not merging page "
					   << block->page.id
					   << " in index " << index->name
					   << " of " << index->table->name;
				ut_ad("MDEV-14637" == 0);
			}
		}
	}

return_after_reservations:
	*err = DB_SUCCESS;

	mem_heap_free(heap);

	if (!srv_read_only_mode
	    && page_is_leaf(page)
	    && !dict_index_is_online_ddl(index)) {

		mtr_memo_release(mtr, dict_index_get_lock(index),
				 MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK);

		/* NOTE: We cannot release root block latch here, because it
		has segment header and already modified in most of cases.*/
	}

	index->table->space->release_free_extents(n_reserved);
	return(ret);
}

/** Delete the node pointer in a parent page.
@param[in,out]	parent	cursor pointing to parent record
@param[in,out]	mtr	mini-transaction */
void btr_cur_node_ptr_delete(btr_cur_t* parent, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, btr_cur_get_block(parent),
				MTR_MEMO_PAGE_X_FIX));
	dberr_t err;
	ibool compressed = btr_cur_pessimistic_delete(&err, TRUE, parent,
						      BTR_CREATE_FLAG, false,
						      mtr);
	ut_a(err == DB_SUCCESS);
	if (!compressed) {
		btr_cur_compress_if_useful(parent, FALSE, mtr);
	}
}

/*******************************************************************//**
Adds path information to the cursor for the current page, for which
the binary search has been performed. */
static
void
btr_cur_add_path_info(
/*==================*/
	btr_cur_t*	cursor,		/*!< in: cursor positioned on a page */
	ulint		height,		/*!< in: height of the page in tree;
					0 means leaf node */
	ulint		root_height)	/*!< in: root node height in tree */
{
	btr_path_t*	slot;

	ut_a(cursor->path_arr);

	if (root_height >= BTR_PATH_ARRAY_N_SLOTS - 1) {
		/* Do nothing; return empty path */

		slot = cursor->path_arr;
		slot->nth_rec = ULINT_UNDEFINED;

		return;
	}

	if (height == 0) {
		/* Mark end of slots for path */
		slot = cursor->path_arr + root_height + 1;
		slot->nth_rec = ULINT_UNDEFINED;
	}

	slot = cursor->path_arr + (root_height - height);

	const buf_block_t* block = btr_cur_get_block(cursor);

	slot->nth_rec = page_rec_get_n_recs_before(btr_cur_get_rec(cursor));
	slot->n_recs = page_get_n_recs(block->frame);
	slot->page_no = block->page.id.page_no();
	slot->page_level = btr_page_get_level(block->frame);
}

/*******************************************************************//**
Estimate the number of rows between slot1 and slot2 for any level on a
B-tree. This function starts from slot1->page and reads a few pages to
the right, counting their records. If we reach slot2->page quickly then
we know exactly how many records there are between slot1 and slot2 and
we set is_n_rows_exact to TRUE. If we cannot reach slot2->page quickly
then we calculate the average number of records in the pages scanned
so far and assume that all pages that we did not scan up to slot2->page
contain the same number of records, then we multiply that average to
the number of pages between slot1->page and slot2->page (which is
n_rows_on_prev_level). In this case we set is_n_rows_exact to FALSE.
@return number of rows, not including the borders (exact or estimated) */
static
ha_rows
btr_estimate_n_rows_in_range_on_level(
/*==================================*/
	dict_index_t*	index,			/*!< in: index */
	btr_path_t*	slot1,			/*!< in: left border */
	btr_path_t*	slot2,			/*!< in: right border */
	ha_rows		n_rows_on_prev_level,	/*!< in: number of rows
						on the previous level for the
						same descend paths; used to
						determine the number of pages
						on this level */
	bool*		is_n_rows_exact)	/*!< out: TRUE if the returned
						value is exact i.e. not an
						estimation */
{
	ha_rows		n_rows = 0;
	uint		n_pages_read = 0;
	ulint		level;

	/* Assume by default that we will scan all pages between
	slot1->page_no and slot2->page_no. */
	*is_n_rows_exact = true;

	/* Add records from slot1->page_no which are to the right of
	the record which serves as a left border of the range, if any
	(we don't include the record itself in this count). */
	if (slot1->nth_rec <= slot1->n_recs) {
		n_rows += slot1->n_recs - slot1->nth_rec;
	}

	/* Add records from slot2->page_no which are to the left of
	the record which servers as a right border of the range, if any
	(we don't include the record itself in this count). */
	if (slot2->nth_rec > 1) {
		n_rows += slot2->nth_rec - 1;
	}

	/* Count the records in the pages between slot1->page_no and
	slot2->page_no (non inclusive), if any. */

	/* Do not read more than this number of pages in order not to hurt
	performance with this code which is just an estimation. If we read
	this many pages before reaching slot2->page_no then we estimate the
	average from the pages scanned so far. */
#	define N_PAGES_READ_LIMIT	10

	const fil_space_t*	space = index->table->space;
	page_id_t		page_id(space->id, slot1->page_no);
	const ulint		zip_size = space->zip_size();

	level = slot1->page_level;

	do {
		mtr_t		mtr;
		page_t*		page;
		buf_block_t*	block;
		dberr_t		err=DB_SUCCESS;

		mtr_start(&mtr);

		/* Fetch the page. Because we are not holding the
		index->lock, the tree may have changed and we may be
		attempting to read a page that is no longer part of
		the B-tree. We pass BUF_GET_POSSIBLY_FREED in order to
		silence a debug assertion about this. */
		block = buf_page_get_gen(page_id, zip_size, RW_S_LATCH,
					 NULL, BUF_GET_POSSIBLY_FREED,
					 __FILE__, __LINE__, &mtr, &err);

		ut_ad((block != NULL) == (err == DB_SUCCESS));

		if (err != DB_SUCCESS) {
			if (err == DB_DECRYPTION_FAILED) {
				ib_push_warning((void *)NULL,
					DB_DECRYPTION_FAILED,
					"Table %s is encrypted but encryption service or"
					" used key_id is not available. "
					" Can't continue reading table.",
					index->table->name.m_name);
				index->table->file_unreadable = true;
			}

			mtr_commit(&mtr);
			goto inexact;
		}

		page = buf_block_get_frame(block);

		/* It is possible that the tree has been reorganized in the
		meantime and this is a different page. If this happens the
		calculated estimate will be bogus, which is not fatal as
		this is only an estimate. We are sure that a page with
		page_no exists because InnoDB never frees pages, only
		reuses them. */
		if (!fil_page_index_page_check(page)
		    || btr_page_get_index_id(page) != index->id
		    || btr_page_get_level(page) != level) {

			/* The page got reused for something else */
			mtr_commit(&mtr);
			goto inexact;
		}

		/* It is possible but highly unlikely that the page was
		originally written by an old version of InnoDB that did
		not initialize FIL_PAGE_TYPE on other than B-tree pages.
		For example, this could be an almost-empty BLOB page
		that happens to contain the magic values in the fields
		that we checked above. */

		n_pages_read++;

		if (page_id.page_no() != slot1->page_no) {
			/* Do not count the records on slot1->page_no,
			we already counted them before this loop. */
			n_rows += page_get_n_recs(page);
		}

		page_id.set_page_no(btr_page_get_next(page));

		mtr_commit(&mtr);

		if (n_pages_read == N_PAGES_READ_LIMIT
		    || page_id.page_no() == FIL_NULL) {
			/* Either we read too many pages or
			we reached the end of the level without passing
			through slot2->page_no, the tree must have changed
			in the meantime */
			goto inexact;
		}

	} while (page_id.page_no() != slot2->page_no);

	return(n_rows);

inexact:

	*is_n_rows_exact = false;

	/* We did interrupt before reaching slot2->page */

	if (n_pages_read > 0) {
		/* The number of pages on this level is
		n_rows_on_prev_level, multiply it by the
		average number of recs per page so far */
		n_rows = n_rows_on_prev_level * n_rows / n_pages_read;
	} else {
		/* The tree changed before we could even
		start with slot1->page_no */
		n_rows = 10;
	}

	return(n_rows);
}

/** If the tree gets changed too much between the two dives for the left
and right boundary then btr_estimate_n_rows_in_range_low() will retry
that many times before giving up and returning the value stored in
rows_in_range_arbitrary_ret_val. */
static const unsigned	rows_in_range_max_retries = 4;

/** We pretend that a range has that many records if the tree keeps changing
for rows_in_range_max_retries retries while we try to estimate the records
in a given range. */
static const ha_rows	rows_in_range_arbitrary_ret_val = 10;

/** Estimates the number of rows in a given index range.
@param[in]	index		index
@param[in]	tuple1		range start
@param[in]	tuple2		range end
@param[in]	nth_attempt	if the tree gets modified too much while
we are trying to analyze it, then we will retry (this function will call
itself, incrementing this parameter)
@return estimated number of rows; if after rows_in_range_max_retries
retries the tree keeps changing, then we will just return
rows_in_range_arbitrary_ret_val as a result (if
nth_attempt >= rows_in_range_max_retries and the tree is modified between
the two dives). */
static
ha_rows
btr_estimate_n_rows_in_range_low(
	dict_index_t*	index,
	btr_pos_t*	tuple1,
	btr_pos_t*	tuple2,
	unsigned	nth_attempt)
{
	btr_path_t	path1[BTR_PATH_ARRAY_N_SLOTS];
	btr_path_t	path2[BTR_PATH_ARRAY_N_SLOTS];
	btr_cur_t	cursor;
	btr_path_t*	slot1;
	btr_path_t*	slot2;
	bool		diverged;
	bool		diverged_lot;
	ulint		divergence_level;
	ha_rows		n_rows;
	bool		is_n_rows_exact;
	ulint		i;
	mtr_t		mtr;
	ha_rows		table_n_rows;
        page_cur_mode_t mode2= tuple2->mode;

	table_n_rows = dict_table_get_n_rows(index->table);

	/* Below we dive to the two records specified by tuple1 and tuple2 and
	we remember the entire dive paths from the tree root. The place where
	the tuple1 path ends on the leaf level we call "left border" of our
	interval and the place where the tuple2 path ends on the leaf level -
	"right border". We take care to either include or exclude the interval
	boundaries depending on whether <, <=, > or >= was specified. For
	example if "5 < x AND x <= 10" then we should not include the left
	boundary, but should include the right one. */

	mtr_start(&mtr);

	cursor.path_arr = path1;

	bool	should_count_the_left_border;

	if (dtuple_get_n_fields(tuple1->tuple) > 0) {

              btr_cur_search_to_nth_level(index, 0, tuple1->tuple,
                                            tuple1->mode,
					    BTR_SEARCH_LEAF | BTR_ESTIMATE,
					    &cursor, 0,
					    __FILE__, __LINE__, &mtr);

		ut_ad(!page_rec_is_infimum(btr_cur_get_rec(&cursor)));

		/* We should count the border if there are any records to
		match the criteria, i.e. if the maximum record on the tree is
		5 and x > 3 is specified then the cursor will be positioned at
		5 and we should count the border, but if x > 7 is specified,
		then the cursor will be positioned at 'sup' on the rightmost
		leaf page in the tree and we should not count the border. */
		should_count_the_left_border
			= !page_rec_is_supremum(btr_cur_get_rec(&cursor));
	} else {
		dberr_t err = DB_SUCCESS;

		err = btr_cur_open_at_index_side(true, index,
					   BTR_SEARCH_LEAF | BTR_ESTIMATE,
					   &cursor, 0, &mtr);

		if (err != DB_SUCCESS) {
			ib::warn() << " Error code: " << err
				   << " btr_estimate_n_rows_in_range_low "
				   << " called from file: "
				   << __FILE__ << " line: " << __LINE__
				   << " table: " << index->table->name
				   << " index: " << index->name;
		}

		ut_ad(page_rec_is_infimum(btr_cur_get_rec(&cursor)));

		/* The range specified is wihout a left border, just
		'x < 123' or 'x <= 123' and btr_cur_open_at_index_side()
		positioned the cursor on the infimum record on the leftmost
		page, which must not be counted. */
		should_count_the_left_border = false;
	}

        tuple1->page_id= cursor.page_cur.block->page.id;

	mtr_commit(&mtr);

	if (!index->is_readable()) {
		return 0;
	}

	mtr_start(&mtr);

	cursor.path_arr = path2;

	bool	should_count_the_right_border;

	if (dtuple_get_n_fields(tuple2->tuple) > 0) {

		btr_cur_search_to_nth_level(index, 0, tuple2->tuple,
                                            mode2,
					    BTR_SEARCH_LEAF | BTR_ESTIMATE,
					    &cursor, 0,
					    __FILE__, __LINE__, &mtr);

		const rec_t*	rec = btr_cur_get_rec(&cursor);

		ut_ad(!(mode2 == PAGE_CUR_L && page_rec_is_supremum(rec)));

		should_count_the_right_border
			= (mode2 == PAGE_CUR_LE /* if the range is '<=' */
			   /* and the record was found */
			   && cursor.low_match >= dtuple_get_n_fields(tuple2->tuple))
			|| (mode2 == PAGE_CUR_L /* or if the range is '<' */
			    /* and there are any records to match the criteria,
			    i.e. if the minimum record on the tree is 5 and
			    x < 7 is specified then the cursor will be
			    positioned at 5 and we should count the border, but
			    if x < 2 is specified, then the cursor will be
			    positioned at 'inf' and we should not count the
			    border */
			    && !page_rec_is_infimum(rec));
		/* Notice that for "WHERE col <= 'foo'" MySQL passes to
		ha_innobase::records_in_range():
		min_key=NULL (left-unbounded) which is expected
		max_key='foo' flag=HA_READ_AFTER_KEY (PAGE_CUR_G), which is
		unexpected - one would expect
		flag=HA_READ_KEY_OR_PREV (PAGE_CUR_LE). In this case the
		cursor will be positioned on the first record to the right of
		the requested one (can also be positioned on the 'sup') and
		we should not count the right border. */
	} else {
		dberr_t err = DB_SUCCESS;

		err = btr_cur_open_at_index_side(false, index,
					   BTR_SEARCH_LEAF | BTR_ESTIMATE,
					   &cursor, 0, &mtr);

		if (err != DB_SUCCESS) {
			ib::warn() << " Error code: " << err
				   << " btr_estimate_n_rows_in_range_low "
				   << " called from file: "
				   << __FILE__ << " line: " << __LINE__
				   << " table: " << index->table->name
				   << " index: " << index->name;
		}

		ut_ad(page_rec_is_supremum(btr_cur_get_rec(&cursor)));

		/* The range specified is wihout a right border, just
		'x > 123' or 'x >= 123' and btr_cur_open_at_index_side()
		positioned the cursor on the supremum record on the rightmost
		page, which must not be counted. */
		should_count_the_right_border = false;
	}

        tuple2->page_id= cursor.page_cur.block->page.id;

	mtr_commit(&mtr);

	/* We have the path information for the range in path1 and path2 */

	n_rows = 0;
	is_n_rows_exact = true;

	/* This becomes true when the two paths do not pass through the
	same pages anymore. */
	diverged = false;

	/* This becomes true when the paths are not the same or adjacent
	any more. This means that they pass through the same or
	neighboring-on-the-same-level pages only. */
	diverged_lot = false;

	/* This is the level where paths diverged a lot. */
	divergence_level = 1000000;

	for (i = 0; ; i++) {
		ut_ad(i < BTR_PATH_ARRAY_N_SLOTS);

		slot1 = path1 + i;
		slot2 = path2 + i;

		if (slot1->nth_rec == ULINT_UNDEFINED
		    || slot2->nth_rec == ULINT_UNDEFINED) {

			/* Here none of the borders were counted. For example,
			if on the leaf level we descended to:
			(inf, a, b, c, d, e, f, sup)
			         ^        ^
			       path1    path2
			then n_rows will be 2 (c and d). */

			if (is_n_rows_exact) {
				/* Only fiddle to adjust this off-by-one
				if the number is exact, otherwise we do
				much grosser adjustments below. */

				btr_path_t*	last1 = &path1[i - 1];
				btr_path_t*	last2 = &path2[i - 1];

				/* If both paths end up on the same record on
				the leaf level. */
				if (last1->page_no == last2->page_no
				    && last1->nth_rec == last2->nth_rec) {

					/* n_rows can be > 0 here if the paths
					were first different and then converged
					to the same record on the leaf level.
					For example:
					SELECT ... LIKE 'wait/synch/rwlock%'
					mode1=PAGE_CUR_GE,
					tuple1="wait/synch/rwlock"
					path1[0]={nth_rec=58, n_recs=58,
						  page_no=3, page_level=1}
					path1[1]={nth_rec=56, n_recs=55,
						  page_no=119, page_level=0}

					mode2=PAGE_CUR_G
					tuple2="wait/synch/rwlock"
					path2[0]={nth_rec=57, n_recs=57,
						  page_no=3, page_level=1}
					path2[1]={nth_rec=56, n_recs=55,
						  page_no=119, page_level=0} */

					/* If the range is such that we should
					count both borders, then avoid
					counting that record twice - once as a
					left border and once as a right
					border. */
					if (should_count_the_left_border
					    && should_count_the_right_border) {

						n_rows = 1;
					} else {
						/* Some of the borders should
						not be counted, e.g. [3,3). */
						n_rows = 0;
					}
				} else {
					if (should_count_the_left_border) {
						n_rows++;
					}

					if (should_count_the_right_border) {
						n_rows++;
					}
				}
			}

			if (i > divergence_level + 1 && !is_n_rows_exact) {
				/* In trees whose height is > 1 our algorithm
				tends to underestimate: multiply the estimate
				by 2: */

				n_rows = n_rows * 2;
			}

			DBUG_EXECUTE_IF("bug14007649", return(n_rows););

			/* Do not estimate the number of rows in the range
			to over 1 / 2 of the estimated rows in the whole
			table */

			if (n_rows > table_n_rows / 2 && !is_n_rows_exact) {

				n_rows = table_n_rows / 2;

				/* If there are just 0 or 1 rows in the table,
				then we estimate all rows are in the range */

				if (n_rows == 0) {
					n_rows = table_n_rows;
				}
			}

			return(n_rows);
		}

		if (!diverged && slot1->nth_rec != slot2->nth_rec) {

			/* If both slots do not point to the same page,
			this means that the tree must have changed between
			the dive for slot1 and the dive for slot2 at the
			beginning of this function. */
			if (slot1->page_no != slot2->page_no
			    || slot1->page_level != slot2->page_level) {

				/* If the tree keeps changing even after a
				few attempts, then just return some arbitrary
				number. */
				if (nth_attempt >= rows_in_range_max_retries) {
					return(rows_in_range_arbitrary_ret_val);
				}

				return btr_estimate_n_rows_in_range_low(
                                       index, tuple1, tuple2,
                                       nth_attempt + 1);
			}

			diverged = true;

			if (slot1->nth_rec < slot2->nth_rec) {
				/* We do not count the borders (nor the left
				nor the right one), thus "- 1". */
				n_rows = slot2->nth_rec - slot1->nth_rec - 1;

				if (n_rows > 0) {
					/* There is at least one row between
					the two borders pointed to by slot1
					and slot2, so on the level below the
					slots will point to non-adjacent
					pages. */
					diverged_lot = true;
					divergence_level = i;
				}
			} else {
				/* It is possible that
				slot1->nth_rec >= slot2->nth_rec
				if, for example, we have a single page
				tree which contains (inf, 5, 6, supr)
				and we select where x > 20 and x < 30;
				in this case slot1->nth_rec will point
				to the supr record and slot2->nth_rec
				will point to 6. */
				n_rows = 0;
				should_count_the_left_border = false;
				should_count_the_right_border = false;
			}

		} else if (diverged && !diverged_lot) {

			if (slot1->nth_rec < slot1->n_recs
			    || slot2->nth_rec > 1) {

				diverged_lot = true;
				divergence_level = i;

				n_rows = 0;

				if (slot1->nth_rec < slot1->n_recs) {
					n_rows += slot1->n_recs
						- slot1->nth_rec;
				}

				if (slot2->nth_rec > 1) {
					n_rows += slot2->nth_rec - 1;
				}
			}
		} else if (diverged_lot) {

			n_rows = btr_estimate_n_rows_in_range_on_level(
				index, slot1, slot2, n_rows,
				&is_n_rows_exact);
		}
	}
}

/** Estimates the number of rows in a given index range.
@param[in]	index	index
@param[in]	tuple1	range start, may also be empty tuple
@param[in]	mode1	search mode for range start
@param[in]	tuple2	range end, may also be empty tuple
@param[in]	mode2	search mode for range end
@return estimated number of rows */
ha_rows
btr_estimate_n_rows_in_range(
	dict_index_t*	index,
        btr_pos_t       *tuple1,
        btr_pos_t       *tuple2)
{
	return btr_estimate_n_rows_in_range_low(
		index, tuple1, tuple2, 1);
}

/*******************************************************************//**
Record the number of non_null key values in a given index for
each n-column prefix of the index where 1 <= n <= dict_index_get_n_unique(index).
The estimates are eventually stored in the array:
index->stat_n_non_null_key_vals[], which is indexed from 0 to n-1. */
static
void
btr_record_not_null_field_in_rec(
/*=============================*/
	ulint		n_unique,	/*!< in: dict_index_get_n_unique(index),
					number of columns uniquely determine
					an index entry */
	const offset_t*	offsets,	/*!< in: rec_get_offsets(rec, index),
					its size could be for all fields or
					that of "n_unique" */
	ib_uint64_t*	n_not_null)	/*!< in/out: array to record number of
					not null rows for n-column prefix */
{
	ulint	i;

	ut_ad(rec_offs_n_fields(offsets) >= n_unique);

	if (n_not_null == NULL) {
		return;
	}

	for (i = 0; i < n_unique; i++) {
		if (rec_offs_nth_sql_null(offsets, i)) {
			break;
		}

		n_not_null[i]++;
	}
}

/*******************************************************************//**
Estimates the number of different key values in a given index, for
each n-column prefix of the index where 1 <= n <= dict_index_get_n_unique(index).
The estimates are stored in the array index->stat_n_diff_key_vals[] (indexed
0..n_uniq-1) and the number of pages that were sampled is saved in
index->stat_n_sample_sizes[].
If innodb_stats_method is nulls_ignored, we also record the number of
non-null values for each prefix and stored the estimates in
array index->stat_n_non_null_key_vals.
@return true if the index is available and we get the estimated numbers,
false if the index is unavailable. */
bool
btr_estimate_number_of_different_key_vals(
/*======================================*/
	dict_index_t*	index)	/*!< in: index */
{
	btr_cur_t	cursor;
	page_t*		page;
	rec_t*		rec;
	ulint		n_cols;
	ib_uint64_t*	n_diff;
	ib_uint64_t*	n_not_null;
	ibool		stats_null_not_equal;
	uintmax_t	n_sample_pages=1; /* number of pages to sample */
	ulint		not_empty_flag	= 0;
	ulint		total_external_size = 0;
	ulint		i;
	ulint		j;
	uintmax_t	add_on;
	mtr_t		mtr;
	mem_heap_t*	heap		= NULL;
	offset_t*	offsets_rec	= NULL;
	offset_t*	offsets_next_rec = NULL;

	/* For spatial index, there is no such stats can be
	fetched. */
	if (dict_index_is_spatial(index)) {
		return(false);
	}

	n_cols = dict_index_get_n_unique(index);

	heap = mem_heap_create((sizeof *n_diff + sizeof *n_not_null)
			       * n_cols
			       + dict_index_get_n_fields(index)
			       * (sizeof *offsets_rec
				  + sizeof *offsets_next_rec));

	n_diff = (ib_uint64_t*) mem_heap_zalloc(
		heap, n_cols * sizeof(n_diff[0]));

	n_not_null = NULL;

	/* Check srv_innodb_stats_method setting, and decide whether we
	need to record non-null value and also decide if NULL is
	considered equal (by setting stats_null_not_equal value) */
	switch (srv_innodb_stats_method) {
	case SRV_STATS_NULLS_IGNORED:
		n_not_null = (ib_uint64_t*) mem_heap_zalloc(
			heap, n_cols * sizeof *n_not_null);
		/* fall through */

	case SRV_STATS_NULLS_UNEQUAL:
		/* for both SRV_STATS_NULLS_IGNORED and SRV_STATS_NULLS_UNEQUAL
		case, we will treat NULLs as unequal value */
		stats_null_not_equal = TRUE;
		break;

	case SRV_STATS_NULLS_EQUAL:
		stats_null_not_equal = FALSE;
		break;

	default:
		ut_error;
	}

	if (srv_stats_sample_traditional) {
		/* It makes no sense to test more pages than are contained
		in the index, thus we lower the number if it is too high */
		if (srv_stats_transient_sample_pages > index->stat_index_size) {
			if (index->stat_index_size > 0) {
				n_sample_pages = index->stat_index_size;
			}
		} else {
			n_sample_pages = srv_stats_transient_sample_pages;
		}
	} else {
		/* New logaritmic number of pages that are estimated.
		Number of pages estimated should be between 1 and
		index->stat_index_size.

		If we have only 0 or 1 index pages then we can only take 1
		sample. We have already initialized n_sample_pages to 1.

		So taking index size as I and sample as S and log(I)*S as L

		requirement 1) we want the out limit of the expression to not exceed I;
		requirement 2) we want the ideal pages to be at least S;
		so the current expression is min(I, max( min(S,I), L)

		looking for simplifications:

		case 1: assume S < I
		min(I, max( min(S,I), L) -> min(I , max( S, L))

		but since L=LOG2(I)*S and log2(I) >=1   L>S always so max(S,L) = L.

		so we have: min(I , L)

		case 2: assume I < S
		    min(I, max( min(S,I), L) -> min(I, max( I, L))

		case 2a: L > I
		    min(I, max( I, L)) -> min(I, L) -> I

		case 2b: when L < I
		    min(I, max( I, L))  ->  min(I, I ) -> I

		so taking all case2 paths is I, our expression is:
		n_pages = S < I? min(I,L) : I
                */
		if (index->stat_index_size > 1) {
			n_sample_pages = (srv_stats_transient_sample_pages < index->stat_index_size)
				? ut_min(index->stat_index_size,
					 static_cast<ulint>(
						 log2(double(index->stat_index_size))
						 * double(srv_stats_transient_sample_pages)))
				: index->stat_index_size;
		}
	}

	/* Sanity check */
	ut_ad(n_sample_pages > 0 && n_sample_pages <= (index->stat_index_size <= 1 ? 1 : index->stat_index_size));

	/* We sample some pages in the index to get an estimate */

	for (i = 0; i < n_sample_pages; i++) {
		mtr_start(&mtr);

		bool	available;

		available = btr_cur_open_at_rnd_pos(index, BTR_SEARCH_LEAF,
						    &cursor, &mtr);

		if (!available) {
			mtr_commit(&mtr);
			mem_heap_free(heap);

			return(false);
		}

		/* Count the number of different key values for each prefix of
		the key on this index page. If the prefix does not determine
		the index record uniquely in the B-tree, then we subtract one
		because otherwise our algorithm would give a wrong estimate
		for an index where there is just one key value. */

		if (!index->is_readable()) {
			mtr_commit(&mtr);
			goto exit_loop;
		}

		page = btr_cur_get_page(&cursor);

		rec = page_rec_get_next(page_get_infimum_rec(page));
		const bool is_leaf = page_is_leaf(page);

		if (!page_rec_is_supremum(rec)) {
			not_empty_flag = 1;
			offsets_rec = rec_get_offsets(rec, index, offsets_rec,
						      is_leaf,
						      ULINT_UNDEFINED, &heap);

			if (n_not_null != NULL) {
				btr_record_not_null_field_in_rec(
					n_cols, offsets_rec, n_not_null);
			}
		}

		while (!page_rec_is_supremum(rec)) {
			ulint	matched_fields;
			rec_t*	next_rec = page_rec_get_next(rec);
			if (page_rec_is_supremum(next_rec)) {
				total_external_size +=
					btr_rec_get_externally_stored_len(
						rec, offsets_rec);
				break;
			}

			offsets_next_rec = rec_get_offsets(next_rec, index,
							   offsets_next_rec,
							   is_leaf,
							   ULINT_UNDEFINED,
							   &heap);

			cmp_rec_rec(rec, next_rec,
				    offsets_rec, offsets_next_rec,
				    index, stats_null_not_equal,
				    &matched_fields);

			for (j = matched_fields; j < n_cols; j++) {
				/* We add one if this index record has
				a different prefix from the previous */

				n_diff[j]++;
			}

			if (n_not_null != NULL) {
				btr_record_not_null_field_in_rec(
					n_cols, offsets_next_rec, n_not_null);
			}

			total_external_size
				+= btr_rec_get_externally_stored_len(
					rec, offsets_rec);

			rec = next_rec;
			/* Initialize offsets_rec for the next round
			and assign the old offsets_rec buffer to
			offsets_next_rec. */
			{
				offset_t* offsets_tmp = offsets_rec;
				offsets_rec = offsets_next_rec;
				offsets_next_rec = offsets_tmp;
			}
		}

		if (n_cols == dict_index_get_n_unique_in_tree(index)
		    && page_has_siblings(page)) {

			/* If there is more than one leaf page in the tree,
			we add one because we know that the first record
			on the page certainly had a different prefix than the
			last record on the previous index page in the
			alphabetical order. Before this fix, if there was
			just one big record on each clustered index page, the
			algorithm grossly underestimated the number of rows
			in the table. */

			n_diff[n_cols - 1]++;
		}

		mtr_commit(&mtr);
	}

exit_loop:
	/* If we saw k borders between different key values on
	n_sample_pages leaf pages, we can estimate how many
	there will be in index->stat_n_leaf_pages */

	/* We must take into account that our sample actually represents
	also the pages used for external storage of fields (those pages are
	included in index->stat_n_leaf_pages) */

	for (j = 0; j < n_cols; j++) {
		index->stat_n_diff_key_vals[j]
			= BTR_TABLE_STATS_FROM_SAMPLE(
				n_diff[j], index, n_sample_pages,
				total_external_size, not_empty_flag);

		/* If the tree is small, smaller than
		10 * n_sample_pages + total_external_size, then
		the above estimate is ok. For bigger trees it is common that we
		do not see any borders between key values in the few pages
		we pick. But still there may be n_sample_pages
		different key values, or even more. Let us try to approximate
		that: */

		add_on = index->stat_n_leaf_pages
			/ (10 * (n_sample_pages
				 + total_external_size));

		if (add_on > n_sample_pages) {
			add_on = n_sample_pages;
		}

		index->stat_n_diff_key_vals[j] += add_on;

		index->stat_n_sample_sizes[j] = n_sample_pages;

		/* Update the stat_n_non_null_key_vals[] with our
		sampled result. stat_n_non_null_key_vals[] is created
		and initialized to zero in dict_index_add_to_cache(),
		along with stat_n_diff_key_vals[] array */
		if (n_not_null != NULL) {
			index->stat_n_non_null_key_vals[j] =
				 BTR_TABLE_STATS_FROM_SAMPLE(
					n_not_null[j], index, n_sample_pages,
					total_external_size, not_empty_flag);
		}
	}

	mem_heap_free(heap);

	return(true);
}

/*================== EXTERNAL STORAGE OF BIG FIELDS ===================*/

/***********************************************************//**
Gets the offset of the pointer to the externally stored part of a field.
@return offset of the pointer to the externally stored part */
static
ulint
btr_rec_get_field_ref_offs(
/*=======================*/
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	ulint		n)	/*!< in: index of the external field */
{
	ulint	field_ref_offs;
	ulint	local_len;

	ut_a(rec_offs_nth_extern(offsets, n));
	field_ref_offs = rec_get_nth_field_offs(offsets, n, &local_len);
	ut_a(len_is_stored(local_len));
	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	return(field_ref_offs + local_len - BTR_EXTERN_FIELD_REF_SIZE);
}

/** Gets a pointer to the externally stored part of a field.
@param rec record
@param offsets rec_get_offsets(rec)
@param n index of the externally stored field
@return pointer to the externally stored part */
#define btr_rec_get_field_ref(rec, offsets, n)			\
	((rec) + btr_rec_get_field_ref_offs(offsets, n))

/** Gets the externally stored size of a record, in units of a database page.
@param[in]	rec	record
@param[in]	offsets	array returned by rec_get_offsets()
@return externally stored part, in units of a database page */
ulint
btr_rec_get_externally_stored_len(
	const rec_t*	rec,
	const offset_t*	offsets)
{
	ulint	n_fields;
	ulint	total_extern_len = 0;
	ulint	i;

	ut_ad(!rec_offs_comp(offsets) || !rec_get_node_ptr_flag(rec));

	if (!rec_offs_any_extern(offsets)) {
		return(0);
	}

	n_fields = rec_offs_n_fields(offsets);

	for (i = 0; i < n_fields; i++) {
		if (rec_offs_nth_extern(offsets, i)) {

			ulint	extern_len = mach_read_from_4(
				btr_rec_get_field_ref(rec, offsets, i)
				+ BTR_EXTERN_LEN + 4);

			total_extern_len += ut_calc_align(
				extern_len, ulint(srv_page_size));
		}
	}

	return total_extern_len >> srv_page_size_shift;
}

/*******************************************************************//**
Sets the ownership bit of an externally stored field in a record. */
static
void
btr_cur_set_ownership_of_extern_field(
/*==================================*/
	buf_block_t*	block,	/*!< in/out: index page */
	rec_t*		rec,	/*!< in/out: clustered index record */
	dict_index_t*	index,	/*!< in: index of the page */
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	ulint		i,	/*!< in: field number */
	bool		val,	/*!< in: value to set */
	mtr_t*		mtr)	/*!< in: mtr, or NULL if not logged */
{
	byte*	data;
	ulint	local_len;
	ulint	byte_val;

	data = rec_get_nth_field(rec, offsets, i, &local_len);
	ut_ad(rec_offs_nth_extern(offsets, i));
	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;

	byte_val = mach_read_from_1(data + local_len + BTR_EXTERN_LEN);

	if (val) {
		byte_val &= ~BTR_EXTERN_OWNER_FLAG;
	} else {
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
		ut_a(!(byte_val & BTR_EXTERN_OWNER_FLAG));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */
		byte_val |= BTR_EXTERN_OWNER_FLAG;
	}

	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		mach_write_to_1(data + local_len + BTR_EXTERN_LEN, byte_val);
		page_zip_write_blob_ptr(block, rec, index, offsets, i, mtr);
	} else {
		mtr->write<1,mtr_t::OPT>(*block,
					 data + local_len + BTR_EXTERN_LEN,
					 byte_val);
	}
}

/*******************************************************************//**
Marks non-updated off-page fields as disowned by this record. The ownership
must be transferred to the updated record which is inserted elsewhere in the
index tree. In purge only the owner of externally stored field is allowed
to free the field. */
void
btr_cur_disown_inherited_fields(
/*============================*/
	buf_block_t*	block,	/*!< in/out: index page */
	rec_t*		rec,	/*!< in/out: record in a clustered index */
	dict_index_t*	index,	/*!< in: index of the page */
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	const upd_t*	update,	/*!< in: update vector */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!rec_offs_comp(offsets) || !rec_get_node_ptr_flag(rec));
	ut_ad(rec_offs_any_extern(offsets));

	for (uint16_t i = 0; i < rec_offs_n_fields(offsets); i++) {
		if (rec_offs_nth_extern(offsets, i)
		    && !upd_get_field_by_field_no(update, i, false)) {
			btr_cur_set_ownership_of_extern_field(
				block, rec, index, offsets, i, false, mtr);
		}
	}
}

/*******************************************************************//**
Marks all extern fields in a record as owned by the record. This function
should be called if the delete mark of a record is removed: a not delete
marked record always owns all its extern fields. */
static
void
btr_cur_unmark_extern_fields(
/*=========================*/
	buf_block_t*	block,	/*!< in/out: index page */
	rec_t*		rec,	/*!< in/out: record in a clustered index */
	dict_index_t*	index,	/*!< in: index of the page */
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	mtr_t*		mtr)	/*!< in: mtr, or NULL if not logged */
{
	ut_ad(!rec_offs_comp(offsets) || !rec_get_node_ptr_flag(rec));
	if (!rec_offs_any_extern(offsets)) {
		return;
	}

	const ulint n = rec_offs_n_fields(offsets);

	for (ulint i = 0; i < n; i++) {
		if (rec_offs_nth_extern(offsets, i)) {
			btr_cur_set_ownership_of_extern_field(
				block, rec, index, offsets, i, true, mtr);
		}
	}
}

/*******************************************************************//**
Returns the length of a BLOB part stored on the header page.
@return part length */
static
ulint
btr_blob_get_part_len(
/*==================*/
	const byte*	blob_header)	/*!< in: blob header */
{
	return(mach_read_from_4(blob_header + BTR_BLOB_HDR_PART_LEN));
}

/*******************************************************************//**
Returns the page number where the next BLOB part is stored.
@return page number or FIL_NULL if no more pages */
static
ulint
btr_blob_get_next_page_no(
/*======================*/
	const byte*	blob_header)	/*!< in: blob header */
{
	return(mach_read_from_4(blob_header + BTR_BLOB_HDR_NEXT_PAGE_NO));
}

/** Deallocate a buffer block that was reserved for a BLOB part.
@param block   buffer block
@param all     flag whether to remove a ROW_FORMAT=COMPRESSED page
@param mtr     mini-transaction to commit */
static void btr_blob_free(buf_block_t *block, bool all, mtr_t *mtr)
{
  const page_id_t page_id= block->page.id;
  ut_ad(mtr_memo_contains(mtr, block, MTR_MEMO_PAGE_X_FIX));
  mtr->commit();

  mutex_enter(&buf_pool.mutex);
  /* Free the block if it is still allocated to the same file page. */
  if (block->page.state == BUF_BLOCK_FILE_PAGE && block->page.id == page_id &&
      !buf_LRU_free_page(&block->page, all) && all && block->page.zip.data)
    /* Attempt to deallocate the redundant copy of the uncompressed page
    if the whole ROW_FORMAT=COMPRESSED block cannot be deallocted. */
    buf_LRU_free_page(&block->page, false);

  mutex_exit(&buf_pool.mutex);
}

/** Helper class used while writing blob pages, during insert or update. */
struct btr_blob_log_check_t {
	/** Persistent cursor on a clusterex index record with blobs. */
	btr_pcur_t*	m_pcur;
	/** Mini transaction holding the latches for m_pcur */
	mtr_t*		m_mtr;
	/** rec_get_offsets(rec, index); offset of clust_rec */
	const offset_t*	m_offsets;
	/** The block containing clustered record */
	buf_block_t**	m_block;
	/** The clustered record pointer */
	rec_t**		m_rec;
	/** The blob operation code */
	enum blob_op	m_op;

	/** Constructor
	@param[in]	pcur		persistent cursor on a clustered
					index record with blobs.
	@param[in]	mtr		mini-transaction holding latches for
					pcur.
	@param[in]	offsets		offsets of the clust_rec
	@param[in,out]	block		record block containing pcur record
	@param[in,out]	rec		the clustered record pointer
	@param[in]	op		the blob operation code */
	btr_blob_log_check_t(
		btr_pcur_t*	pcur,
		mtr_t*		mtr,
		const offset_t*	offsets,
		buf_block_t**	block,
		rec_t**		rec,
		enum blob_op	op)
		: m_pcur(pcur),
		  m_mtr(mtr),
		  m_offsets(offsets),
		  m_block(block),
		  m_rec(rec),
		  m_op(op)
	{
		ut_ad(rec_offs_validate(*m_rec, m_pcur->index(), m_offsets));
		ut_ad((*m_block)->frame == page_align(*m_rec));
		ut_ad(*m_rec == btr_pcur_get_rec(m_pcur));
	}

	/** Check if there is enough space in log file. Commit and re-start the
	mini transaction. */
	void check()
	{
		dict_index_t*	index = m_pcur->index();
		ulint		offs = 0;
		ulint		page_no = ULINT_UNDEFINED;

		if (UNIV_UNLIKELY(m_op == BTR_STORE_INSERT_BULK)) {
			offs = page_offset(*m_rec);
			page_no = (*m_block)->page.id.page_no();
			buf_block_buf_fix_inc(*m_block, __FILE__, __LINE__);
		} else {
			btr_pcur_store_position(m_pcur, m_mtr);
		}
		m_mtr->commit();

		DEBUG_SYNC_C("blob_write_middle");

		log_free_check();

		DEBUG_SYNC_C("blob_write_middle_after_check");

		const mtr_log_t log_mode = m_mtr->get_log_mode();
		m_mtr->start();
		m_mtr->set_log_mode(log_mode);
		index->set_modified(*m_mtr);

		if (UNIV_UNLIKELY(m_op == BTR_STORE_INSERT_BULK)) {
			m_pcur->btr_cur.page_cur.block = btr_block_get(
				*index, page_no, RW_X_LATCH, false, m_mtr);
			m_pcur->btr_cur.page_cur.rec
				= m_pcur->btr_cur.page_cur.block->frame
				+ offs;

			buf_block_buf_fix_dec(m_pcur->btr_cur.page_cur.block);
		} else {
			ut_ad(m_pcur->rel_pos == BTR_PCUR_ON);
			bool ret = btr_pcur_restore_position(
				BTR_MODIFY_LEAF | BTR_MODIFY_EXTERNAL,
				m_pcur, m_mtr);

			ut_a(ret);
		}

		*m_block	= btr_pcur_get_block(m_pcur);
		*m_rec		= btr_pcur_get_rec(m_pcur);

		rec_offs_make_valid(*m_rec, index, true,
				    const_cast<offset_t*>(m_offsets));

		ut_ad(m_mtr->memo_contains_page_flagged(
		      *m_rec,
		      MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX));

		ut_ad((m_op == BTR_STORE_INSERT_BULK)
		      == !mtr_memo_contains_flagged(m_mtr, &index->lock,
						    MTR_MEMO_SX_LOCK
						    | MTR_MEMO_X_LOCK));
	}
};

/*******************************************************************//**
Stores the fields in big_rec_vec to the tablespace and puts pointers to
them in rec.  The extern flags in rec will have to be set beforehand.
The fields are stored on pages allocated from leaf node
file segment of the index tree.

TODO: If the allocation extends the tablespace, it will not be redo logged, in
any mini-transaction.  Tablespace extension should be redo-logged, so that
recovery will not fail when the big_rec was written to the extended portion of
the file, in case the file was somehow truncated in the crash.

@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
dberr_t
btr_store_big_rec_extern_fields(
/*============================*/
	btr_pcur_t*	pcur,		/*!< in/out: a persistent cursor. if
					btr_mtr is restarted, then this can
					be repositioned. */
	offset_t*	offsets,	/*!< in/out: rec_get_offsets() on
					pcur. the "external storage" flags
					in offsets will correctly correspond
					to rec when this function returns */
	const big_rec_t*big_rec_vec,	/*!< in: vector containing fields
					to be stored externally */
	mtr_t*		btr_mtr,	/*!< in/out: mtr containing the
					latches to the clustered index. can be
					committed and restarted. */
	enum blob_op	op)		/*! in: operation code */
{
	ulint		rec_page_no;
	byte*		field_ref;
	ulint		extern_len;
	ulint		store_len;
	ulint		page_no;
	ulint		space_id;
	ulint		prev_page_no;
	ulint		hint_page_no;
	ulint		i;
	mtr_t		mtr;
	mem_heap_t*	heap = NULL;
	page_zip_des_t*	page_zip;
	z_stream	c_stream;
	dberr_t		error		= DB_SUCCESS;
	dict_index_t*	index		= pcur->index();
	buf_block_t*	rec_block	= btr_pcur_get_block(pcur);
	rec_t*		rec		= btr_pcur_get_rec(pcur);

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(rec_offs_any_extern(offsets));
	ut_ad(op == BTR_STORE_INSERT_BULK
	      || mtr_memo_contains_flagged(btr_mtr, &index->lock,
					   MTR_MEMO_X_LOCK
					   | MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains(btr_mtr, rec_block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(buf_block_get_frame(rec_block) == page_align(rec));
	ut_a(dict_index_is_clust(index));

	btr_blob_log_check_t redo_log(pcur, btr_mtr, offsets, &rec_block,
				      &rec, op);
	page_zip = buf_block_get_page_zip(rec_block);
	space_id = rec_block->page.id.space();
	rec_page_no = rec_block->page.id.page_no();
	ut_a(fil_page_index_page_check(page_align(rec))
	     || op == BTR_STORE_INSERT_BULK);

	if (page_zip) {
		int	err;

		/* Zlib deflate needs 128 kilobytes for the default
		window size, plus 512 << memLevel, plus a few
		kilobytes for small objects.  We use reduced memLevel
		to limit the memory consumption, and preallocate the
		heap, hoping to avoid memory fragmentation. */
		heap = mem_heap_create(250000);
		page_zip_set_alloc(&c_stream, heap);

		err = deflateInit2(&c_stream, int(page_zip_level),
				   Z_DEFLATED, 15, 7, Z_DEFAULT_STRATEGY);
		ut_a(err == Z_OK);
	}

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	/* All pointers to externally stored columns in the record
	must either be zero or they must be pointers to inherited
	columns, owned by this record or an earlier record version. */
	for (i = 0; i < big_rec_vec->n_fields; i++) {
		field_ref = btr_rec_get_field_ref(
			rec, offsets, big_rec_vec->fields[i].field_no);

		ut_a(!(field_ref[BTR_EXTERN_LEN] & BTR_EXTERN_OWNER_FLAG));
		/* Either this must be an update in place,
		or the BLOB must be inherited, or the BLOB pointer
		must be zero (will be written in this function). */
		ut_a(op == BTR_STORE_UPDATE
		     || (field_ref[BTR_EXTERN_LEN] & BTR_EXTERN_INHERITED_FLAG)
		     || !memcmp(field_ref, field_ref_zero,
				BTR_EXTERN_FIELD_REF_SIZE));
	}
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

	/* Space available in compressed page to carry blob data */
	const ulint	payload_size_zip = rec_block->physical_size()
		- FIL_PAGE_DATA;

	/* Space available in uncompressed page to carry blob data */
	const ulint	payload_size = payload_size_zip
		- (BTR_BLOB_HDR_SIZE + FIL_PAGE_DATA_END);

	/* We have to create a file segment to the tablespace
	for each field and put the pointer to the field in rec */

	for (i = 0; i < big_rec_vec->n_fields; i++) {
		const ulint field_no = big_rec_vec->fields[i].field_no;

		field_ref = btr_rec_get_field_ref(rec, offsets, field_no);
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
		/* A zero BLOB pointer should have been initially inserted. */
		ut_a(!memcmp(field_ref, field_ref_zero,
			     BTR_EXTERN_FIELD_REF_SIZE));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */
		extern_len = big_rec_vec->fields[i].len;
		UNIV_MEM_ASSERT_RW(big_rec_vec->fields[i].data,
				   extern_len);

		ut_a(extern_len > 0);

		prev_page_no = FIL_NULL;

		if (page_zip) {
			int	err = deflateReset(&c_stream);
			ut_a(err == Z_OK);

			c_stream.next_in = (Bytef*)
				big_rec_vec->fields[i].data;
			c_stream.avail_in = static_cast<uInt>(extern_len);
		}

		for (ulint blob_npages = 0;; ++blob_npages) {
			buf_block_t*	block;
			const ulint	commit_freq = 4;
			ulint		r_extents;

			ut_ad(page_align(field_ref) == page_align(rec));

			if (!(blob_npages % commit_freq)) {

				redo_log.check();

				field_ref = btr_rec_get_field_ref(
					rec, offsets, field_no);

				page_zip = buf_block_get_page_zip(rec_block);
				rec_page_no = rec_block->page.id.page_no();
			}

			mtr.start();
			index->set_modified(mtr);
			mtr.set_log_mode(btr_mtr->get_log_mode());

			buf_page_get(rec_block->page.id,
				     rec_block->zip_size(), RW_X_LATCH, &mtr);

			if (prev_page_no == FIL_NULL) {
				hint_page_no = 1 + rec_page_no;
			} else {
				hint_page_no = prev_page_no + 1;
			}

			if (!fsp_reserve_free_extents(&r_extents,
						      index->table->space, 1,
						      FSP_BLOB, &mtr, 1)) {
				mtr.commit();
				error = DB_OUT_OF_FILE_SPACE;
				goto func_exit;
			}

			block = btr_page_alloc(index, hint_page_no, FSP_NO_DIR,
					       0, &mtr, &mtr);

			index->table->space->release_free_extents(r_extents);

			ut_a(block != NULL);

			page_no = block->page.id.page_no();

			if (prev_page_no != FIL_NULL) {
				buf_block_t*	prev_block;

				prev_block = buf_page_get(
					page_id_t(space_id, prev_page_no),
					rec_block->zip_size(),
					RW_X_LATCH, &mtr);

				buf_block_dbg_add_level(prev_block,
							SYNC_EXTERN_STORAGE);

				if (page_zip) {
					mtr.write<4>(*prev_block,
						     prev_block->frame
						     + FIL_PAGE_NEXT,
						     page_no);
					memcpy_aligned<4>(
						buf_block_get_page_zip(
							prev_block)
						->data + FIL_PAGE_NEXT,
						prev_block->frame
						+ FIL_PAGE_NEXT, 4);
				} else {
					mtr.write<4>(*prev_block,
						     BTR_BLOB_HDR_NEXT_PAGE_NO
						     + FIL_PAGE_DATA
						     + prev_block->frame,
						     page_no);
				}
			} else if (dict_index_is_online_ddl(index)) {
				row_log_table_blob_alloc(index, page_no);
			}

			ut_ad(!page_has_siblings(block->frame));
			ut_ad(!fil_page_get_type(block->frame));

			if (page_zip) {
				int		err;
				page_zip_des_t*	blob_page_zip;

				mtr.write<1>(*block,
					     FIL_PAGE_TYPE + 1 + block->frame,
					     prev_page_no == FIL_NULL
					     ? FIL_PAGE_TYPE_ZBLOB
					     : FIL_PAGE_TYPE_ZBLOB2);
				block->page.zip.data[FIL_PAGE_TYPE + 1]
					= block->frame[FIL_PAGE_TYPE + 1];

				c_stream.next_out = block->frame
					+ FIL_PAGE_DATA;
				c_stream.avail_out = static_cast<uInt>(
					payload_size_zip);

				err = deflate(&c_stream, Z_FINISH);
				ut_a(err == Z_OK || err == Z_STREAM_END);
				ut_a(err == Z_STREAM_END
				     || c_stream.avail_out == 0);

				mtr.memcpy(*block,
					   FIL_PAGE_DATA,
					   page_zip_get_size(page_zip)
					   - FIL_PAGE_DATA
					   - c_stream.avail_out);
				/* Copy the page to compressed storage,
				because it will be flushed to disk
				from there. */
				blob_page_zip = buf_block_get_page_zip(block);
				ut_ad(blob_page_zip);
				ut_ad(page_zip_get_size(blob_page_zip)
				      == page_zip_get_size(page_zip));
				memcpy(blob_page_zip->data, block->frame,
				       page_zip_get_size(page_zip));

				if (err == Z_OK && prev_page_no != FIL_NULL) {

					goto next_zip_page;
				}

				if (err == Z_STREAM_END) {
					mach_write_to_4(field_ref
							+ BTR_EXTERN_LEN, 0);
					mach_write_to_4(field_ref
							+ BTR_EXTERN_LEN + 4,
							c_stream.total_in);
				} else {
					memset(field_ref + BTR_EXTERN_LEN,
					       0, 8);
				}

				if (prev_page_no == FIL_NULL) {
					ut_ad(blob_npages == 0);
					mach_write_to_4(field_ref
							+ BTR_EXTERN_SPACE_ID,
							space_id);

					mach_write_to_4(field_ref
							+ BTR_EXTERN_PAGE_NO,
							page_no);

					mach_write_to_4(field_ref
							+ BTR_EXTERN_OFFSET,
							FIL_PAGE_NEXT);
				}

				/* We compress a page when finish bulk insert.*/
				if (UNIV_LIKELY(op != BTR_STORE_INSERT_BULK)) {
					page_zip_write_blob_ptr(
						rec_block, rec, index, offsets,
						field_no, &mtr);
				}

next_zip_page:
				prev_page_no = page_no;

				/* Commit mtr and release the
				uncompressed page frame to save memory. */
				btr_blob_free(block, FALSE, &mtr);

				if (err == Z_STREAM_END) {
					break;
				}
			} else {
				mtr.write<1>(*block, FIL_PAGE_TYPE + 1
					     + block->frame,
					     FIL_PAGE_TYPE_BLOB);

				if (extern_len > payload_size) {
					store_len = payload_size;
				} else {
					store_len = extern_len;
				}

				mtr.memcpy<mtr_t::OPT>(
					*block,
					FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE
					+ block->frame,
					static_cast<const byte*>
					(big_rec_vec->fields[i].data)
					+ big_rec_vec->fields[i].len
					- extern_len, store_len);
				mtr.write<4>(*block, BTR_BLOB_HDR_PART_LEN
					     + FIL_PAGE_DATA + block->frame,
					     store_len);
				compile_time_assert(FIL_NULL == 0xffffffff);
				mtr.memset(block, BTR_BLOB_HDR_NEXT_PAGE_NO
					   + FIL_PAGE_DATA, 4, 0xff);

				extern_len -= store_len;

				ut_ad(!mach_read_from_4(BTR_EXTERN_LEN
							+ field_ref));
				mtr.write<4>(*rec_block,
					     BTR_EXTERN_LEN + 4 + field_ref,
					     big_rec_vec->fields[i].len
					     - extern_len);

				if (prev_page_no == FIL_NULL) {
					ut_ad(blob_npages == 0);
					mtr.write<4,mtr_t::OPT>(
						*rec_block,
						field_ref + BTR_EXTERN_SPACE_ID,
						space_id);

					mtr.write<4>(*rec_block, field_ref
						     + BTR_EXTERN_PAGE_NO,
						     page_no);

					mtr.write<4>(*rec_block, field_ref
						     + BTR_EXTERN_OFFSET,
						     FIL_PAGE_DATA);
				}

				prev_page_no = page_no;

				mtr.commit();

				if (extern_len == 0) {
					break;
				}
			}
		}

		DBUG_EXECUTE_IF("btr_store_big_rec_extern",
				error = DB_OUT_OF_FILE_SPACE;
				goto func_exit;);

		rec_offs_make_nth_extern(offsets, field_no);
	}

func_exit:
	if (page_zip) {
		deflateEnd(&c_stream);
	}

	if (heap != NULL) {
		mem_heap_free(heap);
	}

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	/* All pointers to externally stored columns in the record
	must be valid. */
	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		if (!rec_offs_nth_extern(offsets, i)) {
			continue;
		}

		field_ref = btr_rec_get_field_ref(rec, offsets, i);

		/* The pointer must not be zero if the operation
		succeeded. */
		ut_a(0 != memcmp(field_ref, field_ref_zero,
				 BTR_EXTERN_FIELD_REF_SIZE)
		     || error != DB_SUCCESS);
		/* The column must not be disowned by this record. */
		ut_a(!(field_ref[BTR_EXTERN_LEN] & BTR_EXTERN_OWNER_FLAG));
	}
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */
	return(error);
}

/** Check the FIL_PAGE_TYPE on an uncompressed BLOB page.
@param[in]      block   uncompressed BLOB page
@param[in]      read    true=read, false=purge */
static void btr_check_blob_fil_page_type(const buf_block_t& block, bool read)
{
  uint16_t type= fil_page_get_type(block.frame);

  if (UNIV_LIKELY(type == FIL_PAGE_TYPE_BLOB))
    return;
  /* FIXME: take the tablespace as a parameter */
  if (fil_space_t *space= fil_space_acquire_silent(block.page.id.space()))
  {
    /* Old versions of InnoDB did not initialize FIL_PAGE_TYPE on BLOB
    pages.  Do not print anything about the type mismatch when reading
    a BLOB page that may be from old versions. */
    if (space->full_crc32() || DICT_TF_HAS_ATOMIC_BLOBS(space->flags))
    {
      ib::fatal() << "FIL_PAGE_TYPE=" << type
		  << (read ? " on BLOB read file " : " on BLOB purge file ")
		  << space->chain.start->name
		  << " page " << block.page.id.page_no();
    }
    space->release();
  }
}

/*******************************************************************//**
Frees the space in an externally stored field to the file space
management if the field in data is owned by the externally stored field,
in a rollback we may have the additional condition that the field must
not be inherited. */
void
btr_free_externally_stored_field(
/*=============================*/
	dict_index_t*	index,		/*!< in: index of the data, the index
					tree MUST be X-latched; if the tree
					height is 1, then also the root page
					must be X-latched! (this is relevant
					in the case this function is called
					from purge where 'data' is located on
					an undo log page, not an index
					page) */
	byte*		field_ref,	/*!< in/out: field reference */
	const rec_t*	rec,		/*!< in: record containing field_ref, for
					page_zip_write_blob_ptr(), or NULL */
	const offset_t*	offsets,	/*!< in: rec_get_offsets(rec, index),
					or NULL */
	buf_block_t*	block,		/*!< in/out: page of field_ref */
	ulint		i,		/*!< in: field number of field_ref;
					ignored if rec == NULL */
	bool		rollback,	/*!< in: performing rollback? */
	mtr_t*		local_mtr)	/*!< in: mtr
					containing the latch to data an an
					X-latch to the index tree */
{
	page_t*		page;
	const ulint	space_id	= mach_read_from_4(
		field_ref + BTR_EXTERN_SPACE_ID);
	const ulint	start_page	= mach_read_from_4(
		field_ref + BTR_EXTERN_PAGE_NO);
	ulint		page_no;
	ulint		next_page_no;
	mtr_t		mtr;

	ut_ad(index->is_primary());
	ut_ad(mtr_memo_contains_flagged(local_mtr, dict_index_get_lock(index),
					MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(mtr_memo_contains_page(local_mtr, field_ref,
				     MTR_MEMO_PAGE_X_FIX));
	ut_ad(!rec || rec_offs_validate(rec, index, offsets));
	ut_ad(!rec || field_ref == btr_rec_get_field_ref(rec, offsets, i));
	ut_ad(local_mtr->is_named_space(
		      page_get_space_id(page_align(field_ref))));

	if (UNIV_UNLIKELY(!memcmp(field_ref, field_ref_zero,
				  BTR_EXTERN_FIELD_REF_SIZE))) {
		/* In the rollback, we may encounter a clustered index
		record with some unwritten off-page columns. There is
		nothing to free then. */
		ut_a(rollback);
		return;
	}

	ut_ad(!(mach_read_from_4(field_ref + BTR_EXTERN_LEN)
	        & ~((BTR_EXTERN_OWNER_FLAG
	             | BTR_EXTERN_INHERITED_FLAG) << 24)));
	ut_ad(space_id == index->table->space->id);
	ut_ad(space_id == index->table->space_id);

	const ulint ext_zip_size = index->table->space->zip_size();
	const ulint rec_zip_size = rec ? ext_zip_size : 0;

	/* !rec holds in a call from purge when field_ref is in an undo page */
	ut_ad(rec || !block->page.zip.data);

	for (;;) {
#ifdef UNIV_DEBUG
		buf_block_t*	rec_block;
#endif /* UNIV_DEBUG */
		buf_block_t*	ext_block;

		mtr_start(&mtr);
		mtr.set_spaces(*local_mtr);
		mtr.set_log_mode(local_mtr->get_log_mode());

		ut_ad(!index->table->is_temporary()
		      || local_mtr->get_log_mode() == MTR_LOG_NO_REDO);

		const page_t*	p = page_align(field_ref);

		const page_id_t	page_id(page_get_space_id(p),
					page_get_page_no(p));

#ifdef UNIV_DEBUG
		rec_block =
#endif /* UNIV_DEBUG */
		buf_page_get(page_id, rec_zip_size, RW_X_LATCH, &mtr);

		buf_block_dbg_add_level(rec_block, SYNC_NO_ORDER_CHECK);
		page_no = mach_read_from_4(field_ref + BTR_EXTERN_PAGE_NO);

		if (/* There is no external storage data */
		    page_no == FIL_NULL
		    /* This field does not own the externally stored field */
		    || (mach_read_from_1(field_ref + BTR_EXTERN_LEN)
			& BTR_EXTERN_OWNER_FLAG)
		    /* Rollback and inherited field */
		    || (rollback
			&& (mach_read_from_1(field_ref + BTR_EXTERN_LEN)
			    & BTR_EXTERN_INHERITED_FLAG))) {

			/* Do not free */
			mtr_commit(&mtr);

			return;
		}

		if (page_no == start_page && dict_index_is_online_ddl(index)) {
			row_log_table_blob_free(index, start_page);
		}

		ext_block = buf_page_get(
			page_id_t(space_id, page_no), ext_zip_size,
			RW_X_LATCH, &mtr);

		buf_block_dbg_add_level(ext_block, SYNC_EXTERN_STORAGE);
		page = buf_block_get_frame(ext_block);

		if (ext_zip_size) {
			/* Note that page_zip will be NULL
			in row_purge_upd_exist_or_extern(). */
			switch (fil_page_get_type(page)) {
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			default:
				ut_error;
			}
			next_page_no = mach_read_from_4(page + FIL_PAGE_NEXT);

			btr_page_free(index, ext_block, &mtr, true);

			if (UNIV_LIKELY_NULL(block->page.zip.data)) {
				mach_write_to_4(field_ref + BTR_EXTERN_PAGE_NO,
						next_page_no);
				memset(field_ref + BTR_EXTERN_LEN, 0, 4);
				page_zip_write_blob_ptr(block, rec, index,
							offsets, i, &mtr);
			} else {
				mtr.write<4>(*block,
					     BTR_EXTERN_PAGE_NO + field_ref,
					     next_page_no);
				mtr.write<4,mtr_t::OPT>(*block,
							BTR_EXTERN_LEN + 4
							+ field_ref, 0U);
			}
		} else {
			ut_ad(!block->page.zip.data);
			btr_check_blob_fil_page_type(*ext_block, false);

			next_page_no = mach_read_from_4(
				page + FIL_PAGE_DATA
				+ BTR_BLOB_HDR_NEXT_PAGE_NO);
			btr_page_free(index, ext_block, &mtr, true);

			mtr.write<4>(*block, BTR_EXTERN_PAGE_NO + field_ref,
				     next_page_no);
			/* Zero out the BLOB length.  If the server
			crashes during the execution of this function,
			trx_rollback_all_recovered() could
			dereference the half-deleted BLOB, fetching a
			wrong prefix for the BLOB. */
			mtr.write<4,mtr_t::OPT>(*block,
						BTR_EXTERN_LEN + 4 + field_ref,
						0U);
		}

		/* Commit mtr and release the BLOB block to save memory. */
		btr_blob_free(ext_block, TRUE, &mtr);
	}
}

/***********************************************************//**
Frees the externally stored fields for a record. */
static
void
btr_rec_free_externally_stored_fields(
/*==================================*/
	dict_index_t*	index,	/*!< in: index of the data, the index
				tree MUST be X-latched */
	rec_t*		rec,	/*!< in/out: record */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec, index) */
	buf_block_t*	block,	/*!< in: index page of rec */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the index
				tree */
{
	ulint	n_fields;
	ulint	i;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX));
	ut_ad(index->is_primary());
	ut_ad(page_rec_is_leaf(rec));
	/* Free possible externally stored fields in the record */

	ut_ad(dict_table_is_comp(index->table) == !!rec_offs_comp(offsets));
	n_fields = rec_offs_n_fields(offsets);

	for (i = 0; i < n_fields; i++) {
		if (rec_offs_nth_extern(offsets, i)) {
			btr_free_externally_stored_field(
				index, btr_rec_get_field_ref(rec, offsets, i),
				rec, offsets, block, i, rollback, mtr);
		}
	}
}

/***********************************************************//**
Frees the externally stored fields for a record, if the field is mentioned
in the update vector. */
static
void
btr_rec_free_updated_extern_fields(
/*===============================*/
	dict_index_t*	index,	/*!< in: index of rec; the index tree MUST be
				X-latched */
	rec_t*		rec,	/*!< in/out: record */
	buf_block_t*	block,	/*!< in: index page of rec */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update,	/*!< in: update vector */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the tree */
{
	ulint	n_fields;
	ulint	i;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX));

	/* Free possible externally stored fields in the record */

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		const upd_field_t* ufield = upd_get_nth_field(update, i);

		if (rec_offs_nth_extern(offsets, ufield->field_no)) {
			ulint	len;
			byte*	data = rec_get_nth_field(
				rec, offsets, ufield->field_no, &len);
			ut_a(len >= BTR_EXTERN_FIELD_REF_SIZE);

			btr_free_externally_stored_field(
				index, data + len - BTR_EXTERN_FIELD_REF_SIZE,
				rec, offsets, block,
				ufield->field_no, rollback, mtr);
		}
	}
}

/*******************************************************************//**
Copies the prefix of an uncompressed BLOB.  The clustered index record
that points to this BLOB must be protected by a lock or a page latch.
@return number of bytes written to buf */
static
ulint
btr_copy_blob_prefix(
/*=================*/
	byte*		buf,	/*!< out: the externally stored part of
				the field, or a prefix of it */
	ulint		len,	/*!< in: length of buf, in bytes */
	ulint		space_id,/*!< in: space id of the BLOB pages */
	ulint		page_no,/*!< in: page number of the first BLOB page */
	ulint		offset)	/*!< in: offset on the first BLOB page */
{
	ulint	copied_len	= 0;

	for (;;) {
		mtr_t		mtr;
		buf_block_t*	block;
		const page_t*	page;
		const byte*	blob_header;
		ulint		part_len;
		ulint		copy_len;

		mtr_start(&mtr);

		block = buf_page_get(page_id_t(space_id, page_no),
				     0, RW_S_LATCH, &mtr);
		buf_block_dbg_add_level(block, SYNC_EXTERN_STORAGE);
		page = buf_block_get_frame(block);

		btr_check_blob_fil_page_type(*block, true);

		blob_header = page + offset;
		part_len = btr_blob_get_part_len(blob_header);
		copy_len = ut_min(part_len, len - copied_len);

		memcpy(buf + copied_len,
		       blob_header + BTR_BLOB_HDR_SIZE, copy_len);
		copied_len += copy_len;

		page_no = btr_blob_get_next_page_no(blob_header);

		mtr_commit(&mtr);

		if (page_no == FIL_NULL || copy_len != part_len) {
			UNIV_MEM_ASSERT_RW(buf, copied_len);
			return(copied_len);
		}

		/* On other BLOB pages except the first the BLOB header
		always is at the page data start: */

		offset = FIL_PAGE_DATA;

		ut_ad(copied_len <= len);
	}
}

/** Copies the prefix of a compressed BLOB.
The clustered index record that points to this BLOB must be protected
by a lock or a page latch.
@param[out]	buf		the externally stored part of the field,
or a prefix of it
@param[in]	len		length of buf, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size
@param[in]	space_id	space id of the BLOB pages
@param[in]	offset		offset on the first BLOB page
@return number of bytes written to buf */
static
ulint
btr_copy_zblob_prefix(
	byte*			buf,
	ulint			len,
	ulint			zip_size,
	ulint			space_id,
	ulint			page_no,
	ulint			offset)
{
	ulint		page_type = FIL_PAGE_TYPE_ZBLOB;
	mem_heap_t*	heap;
	int		err;
	z_stream	d_stream;

	d_stream.next_out = buf;
	d_stream.avail_out = static_cast<uInt>(len);
	d_stream.next_in = Z_NULL;
	d_stream.avail_in = 0;

	/* Zlib inflate needs 32 kilobytes for the default
	window size, plus a few kilobytes for small objects. */
	heap = mem_heap_create(40000);
	page_zip_set_alloc(&d_stream, heap);

	ut_ad(zip_size);
	ut_ad(ut_is_2pow(zip_size));
	ut_ad(space_id);

	err = inflateInit(&d_stream);
	ut_a(err == Z_OK);

	for (;;) {
		buf_page_t*	bpage;
		ulint		next_page_no;

		/* There is no latch on bpage directly.  Instead,
		bpage is protected by the B-tree page latch that
		is being held on the clustered index record, or,
		in row_merge_copy_blobs(), by an exclusive table lock. */
		bpage = buf_page_get_zip(page_id_t(space_id, page_no),
					 zip_size);

		if (UNIV_UNLIKELY(!bpage)) {
			ib::error() << "Cannot load compressed BLOB "
				<< page_id_t(space_id, page_no);
			goto func_exit;
		}

		if (UNIV_UNLIKELY
		    (fil_page_get_type(bpage->zip.data) != page_type)) {

			ib::error() << "Unexpected type "
				<< fil_page_get_type(bpage->zip.data)
				<< " of compressed BLOB page "
				<< page_id_t(space_id, page_no);

			ut_ad(0);
			goto end_of_blob;
		}

		next_page_no = mach_read_from_4(bpage->zip.data + offset);

		if (UNIV_LIKELY(offset == FIL_PAGE_NEXT)) {
			/* When the BLOB begins at page header,
			the compressed data payload does not
			immediately follow the next page pointer. */
			offset = FIL_PAGE_DATA;
		} else {
			offset += 4;
		}

		d_stream.next_in = bpage->zip.data + offset;
		d_stream.avail_in = uInt(zip_size - offset);

		err = inflate(&d_stream, Z_NO_FLUSH);
		switch (err) {
		case Z_OK:
			if (!d_stream.avail_out) {
				goto end_of_blob;
			}
			break;
		case Z_STREAM_END:
			if (next_page_no == FIL_NULL) {
				goto end_of_blob;
			}
			/* fall through */
		default:
inflate_error:
			ib::error() << "inflate() of compressed BLOB page "
				<< page_id_t(space_id, page_no)
				<< " returned " << err
				<< " (" << d_stream.msg << ")";

		case Z_BUF_ERROR:
			goto end_of_blob;
		}

		if (next_page_no == FIL_NULL) {
			if (!d_stream.avail_in) {
				ib::error()
					<< "Unexpected end of compressed "
					<< "BLOB page "
					<< page_id_t(space_id, page_no);
			} else {
				err = inflate(&d_stream, Z_FINISH);
				switch (err) {
				case Z_STREAM_END:
				case Z_BUF_ERROR:
					break;
				default:
					goto inflate_error;
				}
			}

end_of_blob:
			buf_page_release_zip(bpage);
			goto func_exit;
		}

		buf_page_release_zip(bpage);

		/* On other BLOB pages except the first
		the BLOB header always is at the page header: */

		page_no = next_page_no;
		offset = FIL_PAGE_NEXT;
		page_type = FIL_PAGE_TYPE_ZBLOB2;
	}

func_exit:
	inflateEnd(&d_stream);
	mem_heap_free(heap);
	UNIV_MEM_ASSERT_RW(buf, d_stream.total_out);
	return(d_stream.total_out);
}

/** Copies the prefix of an externally stored field of a record.
The clustered index record that points to this BLOB must be protected
by a lock or a page latch.
@param[out]	buf		the externally stored part of the
field, or a prefix of it
@param[in]	len		length of buf, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	space_id	space id of the first BLOB page
@param[in]	page_no		page number of the first BLOB page
@param[in]	offset		offset on the first BLOB page
@return number of bytes written to buf */
static
ulint
btr_copy_externally_stored_field_prefix_low(
	byte*			buf,
	ulint			len,
	ulint			zip_size,
	ulint			space_id,
	ulint			page_no,
	ulint			offset)
{
	if (len == 0) {
		return(0);
	}

	if (zip_size) {
		return(btr_copy_zblob_prefix(buf, len, zip_size,
					     space_id, page_no, offset));
	} else {
		return(btr_copy_blob_prefix(buf, len, space_id,
					    page_no, offset));
	}
}

/** Copies the prefix of an externally stored field of a record.
The clustered index record must be protected by a lock or a page latch.
@param[out]	buf		the field, or a prefix of it
@param[in]	len		length of buf, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	data		'internally' stored part of the field
containing also the reference to the external part; must be protected by
a lock or a page latch
@param[in]	local_len	length of data, in bytes
@return the length of the copied field, or 0 if the column was being
or has been deleted */
ulint
btr_copy_externally_stored_field_prefix(
	byte*			buf,
	ulint			len,
	ulint			zip_size,
	const byte*		data,
	ulint			local_len)
{
	ulint	space_id;
	ulint	page_no;
	ulint	offset;

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;

	if (UNIV_UNLIKELY(local_len >= len)) {
		memcpy(buf, data, len);
		return(len);
	}

	memcpy(buf, data, local_len);
	data += local_len;

	ut_a(memcmp(data, field_ref_zero, BTR_EXTERN_FIELD_REF_SIZE));

	if (!mach_read_from_4(data + BTR_EXTERN_LEN + 4)) {
		/* The externally stored part of the column has been
		(partially) deleted.  Signal the half-deleted BLOB
		to the caller. */

		return(0);
	}

	space_id = mach_read_from_4(data + BTR_EXTERN_SPACE_ID);

	page_no = mach_read_from_4(data + BTR_EXTERN_PAGE_NO);

	offset = mach_read_from_4(data + BTR_EXTERN_OFFSET);

	return(local_len
	       + btr_copy_externally_stored_field_prefix_low(buf + local_len,
							     len - local_len,
							     zip_size,
							     space_id, page_no,
							     offset));
}

/** Copies an externally stored field of a record to mem heap.
The clustered index record must be protected by a lock or a page latch.
@param[out]	len		length of the whole field
@param[in]	data		'internally' stored part of the field
containing also the reference to the external part; must be protected by
a lock or a page latch
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	local_len	length of data
@param[in,out]	heap		mem heap
@return the whole field copied to heap */
byte*
btr_copy_externally_stored_field(
	ulint*			len,
	const byte*		data,
	ulint			zip_size,
	ulint			local_len,
	mem_heap_t*		heap)
{
	ulint	space_id;
	ulint	page_no;
	ulint	offset;
	ulint	extern_len;
	byte*	buf;

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;

	space_id = mach_read_from_4(data + local_len + BTR_EXTERN_SPACE_ID);

	page_no = mach_read_from_4(data + local_len + BTR_EXTERN_PAGE_NO);

	offset = mach_read_from_4(data + local_len + BTR_EXTERN_OFFSET);

	/* Currently a BLOB cannot be bigger than 4 GB; we
	leave the 4 upper bytes in the length field unused */

	extern_len = mach_read_from_4(data + local_len + BTR_EXTERN_LEN + 4);

	buf = (byte*) mem_heap_alloc(heap, local_len + extern_len);

	memcpy(buf, data, local_len);
	*len = local_len
		+ btr_copy_externally_stored_field_prefix_low(buf + local_len,
							      extern_len,
							      zip_size,
							      space_id,
							      page_no, offset);

	return(buf);
}

/** Copies an externally stored field of a record to mem heap.
@param[in]	rec		record in a clustered index; must be
protected by a lock or a page latch
@param[in]	offset		array returned by rec_get_offsets()
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	no		field number
@param[out]	len		length of the field
@param[in,out]	heap		mem heap
@return the field copied to heap, or NULL if the field is incomplete */
byte*
btr_rec_copy_externally_stored_field(
	const rec_t*		rec,
	const offset_t*		offsets,
	ulint			zip_size,
	ulint			no,
	ulint*			len,
	mem_heap_t*		heap)
{
	ulint		local_len;
	const byte*	data;

	ut_a(rec_offs_nth_extern(offsets, no));

	/* An externally stored field can contain some initial
	data from the field, and in the last 20 bytes it has the
	space id, page number, and offset where the rest of the
	field data is stored, and the data length in addition to
	the data stored locally. We may need to store some data
	locally to get the local record length above the 128 byte
	limit so that field offsets are stored in two bytes, and
	the extern bit is available in those two bytes. */

	data = rec_get_nth_field(rec, offsets, no, &local_len);

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	if (UNIV_UNLIKELY
	    (!memcmp(data + local_len - BTR_EXTERN_FIELD_REF_SIZE,
		     field_ref_zero, BTR_EXTERN_FIELD_REF_SIZE))) {
		/* The externally stored field was not written yet.
		This record should only be seen by
		recv_recovery_rollback_active() or any
		TRX_ISO_READ_UNCOMMITTED transactions. */
		return(NULL);
	}

	return(btr_copy_externally_stored_field(len, data,
						zip_size, local_len, heap));
}
