/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
#include "buf0rea.h"
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
#include "row0ins.h"
#ifdef WITH_WSREP
#include "mysql/service_wsrep.h"
#endif /* WITH_WSREP */
#include "log.h"

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

#ifdef BTR_CUR_HASH_ADAPT
/** Number of searches down the B-tree in btr_cur_t::search_leaf(). */
ib_counter_t<ulint, ib_counter_element_t>	btr_cur_n_non_sea;
/** Old value of btr_cur_n_non_sea.  Copied by
srv_refresh_innodb_monitor_stats().  Referenced by
srv_printf_innodb_monitor(). */
ulint	btr_cur_n_non_sea_old;
/** Number of successful adaptive hash index lookups in
btr_cur_t::search_leaf(). */
ib_counter_t<ulint, ib_counter_element_t>	btr_cur_n_sea;
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
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	mtr_t*		mtr);	/*!< in: mtr, or NULL if not logged */
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
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
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
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	buf_block_t*	block,	/*!< in: index page of rec */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr);	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the index
				tree */

/*==================== B-TREE SEARCH =========================*/

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

	dberr_t err;
	const fil_space_t* space = index->table->space;
	if (!space) {
corrupted:
		err = DB_CORRUPTION;
unreadable:
		ib::error() << "Table " << index->table->name
			    << " has an unreadable root page";
		index->table->corrupted = true;
		index->table->file_unreadable = true;
		return err;
	}

	buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH, mtr, &err);
	if (!root) {
		goto unreadable;
	}

	if (btr_cur_instant_root_init(index, root->page.frame)) {
		goto corrupted;
	}

	ut_ad(index->n_core_null_bytes != dict_index_t::NO_CORE_NULL_BYTES);

	if (fil_page_get_type(root->page.frame) == FIL_PAGE_INDEX) {
		ut_ad(!index->is_instant());
		return DB_SUCCESS;
	}

	btr_cur_t cur;
	/* Relax the assertion in rec_init_offsets(). */
	ut_ad(!index->in_instant_init);
	ut_d(index->in_instant_init = true);
	err = cur.open_leaf(true, index, BTR_SEARCH_LEAF, mtr);
	ut_d(index->in_instant_init = false);
	if (err != DB_SUCCESS) {
		index->table->file_unreadable = true;
		index->table->corrupted = true;
		return err;
	}

	ut_ad(page_cur_is_before_first(&cur.page_cur));
	ut_ad(page_is_leaf(btr_cur_get_page(&cur)));

	const rec_t* rec = page_cur_move_to_next(&cur.page_cur);
	const ulint comp = dict_table_is_comp(index->table);
	const ulint info_bits = rec ? rec_get_info_bits(rec, comp) : 0;

	if (page_rec_is_supremum(rec)
	    || !(info_bits & REC_INFO_MIN_REC_FLAG)) {
		if (rec && !index->is_instant()) {
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
		if (!block) {
			goto incompatible;
		}

		if (fil_page_get_type(block->page.frame) != FIL_PAGE_TYPE_BLOB
		    || mach_read_from_4(&block->page.frame
					[FIL_PAGE_DATA
					 + BTR_BLOB_HDR_NEXT_PAGE_NO])
		    != FIL_NULL
		    || mach_read_from_4(&block->page.frame
					[FIL_PAGE_DATA
					 + BTR_BLOB_HDR_PART_LEN])
		    != len) {
			goto incompatible;
		}

		/* The unused part of the BLOB page should be zero-filled. */
		for (const byte* b = block->page.frame
		       + (FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE) + len,
		       * const end = block->page.frame + srv_page_size
		       - BTR_EXTERN_LEN;
		     b < end; ) {
			if (*b++) {
				goto incompatible;
			}
		}

		if (index->table->deserialise_columns(
			    &block->page.frame
			    [FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE], len)) {
			goto incompatible;
		}

		/* Proceed to initialize the default values of
		any instantly added columns. */
	}

	mem_heap_t* heap = NULL;
	rec_offs* offsets = rec_get_offsets(rec, index, NULL,
					    index->n_core_fields,
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
	ut_ad(index->is_primary());
	ut_ad(!index->is_instant());
	ut_ad(index->table->supports_instant());

	if (page_has_siblings(page)) {
		return true;
	}

	/* This is normally executed as part of btr_cur_instant_init()
	when dict_load_table_one() is loading a table definition.
	Other threads should not access or modify the n_core_null_bytes,
	n_core_fields before dict_load_table_one() returns.

	This can also be executed during IMPORT TABLESPACE, where the
	table definition is exclusively locked. */

	switch (fil_page_get_type(page)) {
	default:
		return true;
	case FIL_PAGE_INDEX:
		/* The field PAGE_INSTANT is guaranteed 0 on clustered
		index root pages of ROW_FORMAT=COMPACT or
		ROW_FORMAT=DYNAMIC when instant ADD COLUMN is not used. */
		if (page_is_comp(page) && page_get_instant(page)) {
			return true;
		}
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

/**
Gets intention in btr_intention_t from latch_mode, and cleares the intention
at the latch_mode.
@param latch_mode	in/out: pointer to latch_mode
@return intention for latching tree */
static
btr_intention_t btr_cur_get_and_clear_intention(btr_latch_mode *latch_mode)
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
	*latch_mode = btr_latch_mode(
		*latch_mode & ~(BTR_LATCH_FOR_INSERT | BTR_LATCH_FOR_DELETE));

	return(intention);
}

/** @return whether the distance between two records is at most the
specified value */
template<bool comp>
static bool
page_rec_distance_is_at_most(const page_t *page, const rec_t *left,
                             const rec_t *right, ulint val)
  noexcept
{
  do
  {
    if (left == right)
      return true;
    left= page_rec_next_get<comp>(page, left);
  }
  while (left && val--);
  return false;
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
	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));

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
			if (n_recs <= max_nodes_deleted * 2) {
				/* The cursor record can be the left most record
				in this page. */
				return true;
			}

			if (page_is_comp(page)) {
				const rec_t *const infimum
					= page + PAGE_NEW_INFIMUM;
				if (page_rec_next_get<true>(page, infimum)
				    == rec) {
					return true;
				}
				if (page_has_prev(page)
				    && page_rec_distance_is_at_most<true>(
					    page, infimum, rec,
					    max_nodes_deleted)) {
					return true;
				}
				if (page_has_next(page)
				    && page_rec_distance_is_at_most<true>(
					    page, rec,
					    page + PAGE_NEW_SUPREMUM,
					    max_nodes_deleted)) {
					return true;
				}
			} else {
				const rec_t *const infimum
					= page + PAGE_OLD_INFIMUM;
				if (page_rec_next_get<false>(page, infimum)
				    == rec) {
					return true;
				}
				if (page_has_prev(page)
				    && page_rec_distance_is_at_most<false>(
					    page, infimum, rec,
					    max_nodes_deleted)) {
					return true;
				}
				if (page_has_next(page)
				    && page_rec_distance_is_at_most<false>(
					    page, rec,
					    page + PAGE_OLD_SUPREMUM,
					    max_nodes_deleted)) {
					return true;
				}
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
@param bpage             buffer pool page
@param is_clust          whether this is a clustered index
@param lock_intention    lock intention for the tree operation
@param node_ptr_max_size the maximum size of a node pointer
@param compress_limit    BTR_CUR_PAGE_COMPRESS_LIMIT(index)
@param rec               record (current node_ptr)
@return true if tree modification is needed */
static bool btr_cur_need_opposite_intention(const buf_page_t &bpage,
                                            bool is_clust,
                                            btr_intention_t lock_intention,
                                            ulint node_ptr_max_size,
                                            ulint compress_limit,
                                            const rec_t *rec)
{
  ut_ad(bpage.frame == page_align(rec));
  if (UNIV_LIKELY_NULL(bpage.zip.data) &&
      !page_zip_available(&bpage.zip, is_clust, node_ptr_max_size, 1))
    return true;
  const page_t *const page= bpage.frame;
  if (lock_intention != BTR_INTENTION_INSERT)
  {
    /* We compensate also for btr_cur_compress_recommendation() */
    if (!page_has_siblings(page) ||
        page_rec_is_first(rec, page) || page_rec_is_last(rec, page) ||
        page_get_data_size(page) < node_ptr_max_size + compress_limit)
      return true;
    if (lock_intention == BTR_INTENTION_DELETE)
      return false;
  }
  else if (page_has_next(page) && page_rec_is_last(rec, page))
    return true;
  LIMIT_OPTIMISTIC_INSERT_DEBUG(page_get_n_recs(page), return true);
  const ulint max_size= page_get_max_insert_size_after_reorganize(page, 2);
  return max_size < BTR_CUR_PAGE_REORGANIZE_LIMIT + node_ptr_max_size ||
    max_size < node_ptr_max_size * 2;
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
		if (field_max_size && field->fixed_len) {
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
			case DATA_FIXBINARY:
			case DATA_BINARY:
			case DATA_VARMYSQL:
			case DATA_CHAR:
			case DATA_MYSQL:
				/* BINARY(0), VARBINARY(0),
				CHAR(0) and VARCHAR(0) are possible
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

/** @return a B-tree search mode suitable for non-leaf pages
@param mode  leaf page search mode */
static inline page_cur_mode_t btr_cur_nonleaf_mode(page_cur_mode_t mode)
{
  if (mode > PAGE_CUR_GE)
  {
    ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE);
    return mode;
  }
  if (mode == PAGE_CUR_GE)
    return PAGE_CUR_L;
  ut_ad(mode == PAGE_CUR_G);
  return PAGE_CUR_LE;
}

MY_ATTRIBUTE((nonnull,warn_unused_result))
/** Acquire a latch on the previous page without violating the latching order.
@param rw_latch the latch on block (RW_S_LATCH or RW_X_LATCH)
@param page_id  page identifier with valid space identifier
@param err      error code
@param mtr      mini-transaction
@retval 0  if an error occurred
@retval 1  if the page could be latched in the wrong order
@retval -1 if the latch on block was temporarily released */
static int btr_latch_prev(rw_lock_type_t rw_latch,
                          page_id_t page_id, dberr_t *err, mtr_t *mtr)
{
  ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_X_LATCH);

  buf_block_t *block= mtr->at_savepoint(mtr->get_savepoint() - 1);

  ut_ad(page_id.space() == block->page.id().space());

  const page_t *const page= block->page.frame;
  page_id.set_page_no(btr_page_get_prev(page));
  /* We are holding a latch on the current page.

  We will start by buffer-fixing the left sibling. Waiting for a latch
  on it while holding a latch on the current page could lead to a
  deadlock, because another thread could hold that latch and wait for
  a right sibling page latch (the current page).

  If there is a conflict, we will temporarily release our latch on the
  current block while waiting for a latch on the left sibling.  The
  buffer-fixes on both blocks will prevent eviction. */

 retry:
  int ret= 1;
  buf_block_t *prev= buf_pool.page_fix(page_id, err, buf_pool_t::FIX_NOWAIT);
  if (UNIV_UNLIKELY(!prev))
    return 0;
  if (prev == reinterpret_cast<buf_block_t*>(-1))
  {
    /* The block existed in buf_pool.page_hash, but not in a state that is
    safe to access without waiting for some pending operation, such as
    buf_page_t::read_complete() or buf_pool_t::unzip().

    Retry while temporarily releasing the successor block->page.lock
    (but retaining a buffer-fix so that the block cannot be evicted. */

    if (rw_latch == RW_S_LATCH)
      block->page.lock.s_unlock();
    else
      block->page.lock.x_unlock();

    prev= buf_pool.page_fix(page_id, err, buf_pool_t::FIX_WAIT_READ);

    if (!prev)
    {
      ut_ad(*err != DB_SUCCESS);
      if (rw_latch == RW_S_LATCH)
        block->page.lock.s_lock();
      else
        block->page.lock.x_lock();
      return 0;
    }
    else if (rw_latch == RW_S_LATCH)
      goto wait_for_s;
    else
      goto wait_for_x;
  }

  static_assert(MTR_MEMO_PAGE_S_FIX == mtr_memo_type_t(BTR_SEARCH_LEAF), "");
  static_assert(MTR_MEMO_PAGE_X_FIX == mtr_memo_type_t(BTR_MODIFY_LEAF), "");

  if (rw_latch == RW_S_LATCH
      ? prev->page.lock.s_lock_try()
      : prev->page.lock.x_lock_try())
    mtr->memo_push(prev, mtr_memo_type_t(rw_latch));
  else
  {
    if (rw_latch == RW_S_LATCH)
    {
      block->page.lock.s_unlock();
    wait_for_s:
      prev->page.lock.s_lock();
      block->page.lock.s_lock();
    }
    else
    {
      block->page.lock.x_unlock();
    wait_for_x:
      prev->page.lock.x_lock();
      block->page.lock.x_lock();
    }

    ut_ad(block == mtr->at_savepoint(mtr->get_savepoint() - 1));
    mtr->memo_push(prev, mtr_memo_type_t(rw_latch));
    const page_id_t prev_page_id= page_id;
    page_id.set_page_no(btr_page_get_prev(page));
    ret= -1;

    if (UNIV_UNLIKELY(page_id != prev_page_id))
    {
      mtr->release_last_page();
      if (page_id.page_no() == FIL_NULL)
        return ret;
      goto retry;
    }
  }

  const page_t *const p= prev->page.frame;
  if (memcmp_aligned<4>(FIL_PAGE_NEXT + p, FIL_PAGE_OFFSET + page, 4) ||
      memcmp_aligned<2>(FIL_PAGE_TYPE + p, FIL_PAGE_TYPE + page, 2) ||
      memcmp_aligned<2>(PAGE_HEADER + PAGE_INDEX_ID + p,
                        PAGE_HEADER + PAGE_INDEX_ID + page, 8) ||
      page_is_comp(p) != page_is_comp(page))
  {
    ut_ad("corrupted" == 0); // FIXME: remove this
    *err= DB_CORRUPTION;
    ret= 0;
  }

  return ret;
}

dberr_t btr_cur_t::search_leaf(const dtuple_t *tuple, page_cur_mode_t mode,
                               btr_latch_mode latch_mode, mtr_t *mtr)
{
  ut_ad(index()->is_btree() || index()->is_ibuf());
  ut_ad(!index()->is_ibuf() || ibuf_inside(mtr));

  buf_block_t *guess;
  btr_op_t btr_op;
  btr_intention_t lock_intention;
  bool detected_same_key_root= false;

  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs offsets2_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets2= offsets2_;
  rec_offs_init(offsets_);
  rec_offs_init(offsets2_);

  ut_ad(dict_index_check_search_tuple(index(), tuple));
  ut_ad(dtuple_check_typed(tuple));
  ut_ad(index()->page != FIL_NULL);

  MEM_UNDEFINED(&up_match, sizeof up_match);
  MEM_UNDEFINED(&up_bytes, sizeof up_bytes);
  MEM_UNDEFINED(&low_match, sizeof low_match);
  MEM_UNDEFINED(&low_bytes, sizeof low_bytes);
  ut_d(up_match= low_match= uint16_t(~0u));

  ut_ad(!(latch_mode & BTR_ALREADY_S_LATCHED) ||
        mtr->memo_contains_flagged(&index()->lock,
                                   MTR_MEMO_S_LOCK | MTR_MEMO_SX_LOCK |
                                   MTR_MEMO_X_LOCK));

  /* These flags are mutually exclusive, they are lumped together
     with the latch mode for historical reasons. It's possible for
     none of the flags to be set. */
  switch (UNIV_EXPECT(latch_mode & BTR_DELETE, 0)) {
  default:
    btr_op= BTR_NO_OP;
    break;
  case BTR_INSERT:
    btr_op= (latch_mode & BTR_IGNORE_SEC_UNIQUE)
      ? BTR_INSERT_IGNORE_UNIQUE_OP
      : BTR_INSERT_OP;
    break;
  case BTR_DELETE:
    btr_op= BTR_DELETE_OP;
    ut_a(purge_node);
    break;
  case BTR_DELETE_MARK:
    btr_op= BTR_DELMARK_OP;
    break;
  }

  /* Operations on the insert buffer tree cannot be buffered. */
  ut_ad(btr_op == BTR_NO_OP || !index()->is_ibuf());
  /* Operations on the clustered index cannot be buffered. */
  ut_ad(btr_op == BTR_NO_OP || !index()->is_clust());
  /* Operations on the temporary table(indexes) cannot be buffered. */
  ut_ad(btr_op == BTR_NO_OP || !index()->table->is_temporary());

  const bool latch_by_caller= latch_mode & BTR_ALREADY_S_LATCHED;
  lock_intention= btr_cur_get_and_clear_intention(&latch_mode);
  latch_mode= BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);

  ut_ad(!latch_by_caller
        || latch_mode == BTR_SEARCH_LEAF
        || latch_mode == BTR_MODIFY_LEAF
        || latch_mode == BTR_MODIFY_TREE
        || latch_mode == BTR_MODIFY_ROOT_AND_LEAF);

  flag= BTR_CUR_BINARY;
#ifndef BTR_CUR_ADAPT
  guess= nullptr;
#else
  auto info= &index()->search_info;
  guess= info->root_guess;

# ifdef BTR_CUR_HASH_ADAPT
#  ifdef UNIV_SEARCH_PERF_STAT
  info->n_searches++;
#  endif
  if (latch_mode > BTR_MODIFY_LEAF)
    /* The adaptive hash index cannot be useful for these searches. */;
  else if (mode != PAGE_CUR_LE && mode != PAGE_CUR_GE)
    ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_G);
  /* We do a dirty read of btr_search.enabled below,
  and btr_search_guess_on_hash() will have to check it again. */
  else if (!btr_search.enabled);
  else if (btr_search_guess_on_hash(index(), tuple, mode != PAGE_CUR_LE,
                                    latch_mode, this, mtr))
  {
    /* Search using the hash index succeeded */
    ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_GE);
    ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
    ut_ad(low_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
    ++btr_cur_n_sea;

    return DB_SUCCESS;
  }
  else
    ++btr_cur_n_non_sea;
# endif
#endif

  /* If the hash search did not succeed, do binary search down the
     tree */

  /* Store the position of the tree latch we push to mtr so that we
     know how to release it when we have latched leaf node(s) */

  const ulint savepoint= mtr->get_savepoint();

  ulint node_ptr_max_size= 0, compress_limit= 0;
  rw_lock_type_t rw_latch= RW_S_LATCH;

  switch (latch_mode) {
  case BTR_MODIFY_TREE:
    rw_latch= RW_X_LATCH;
    node_ptr_max_size= btr_node_ptr_max_size(index());
    if (latch_by_caller)
    {
      ut_ad(mtr->memo_contains_flagged(&index()->lock, MTR_MEMO_X_LOCK));
      break;
    }
    if (lock_intention == BTR_INTENTION_DELETE)
    {
      compress_limit= BTR_CUR_PAGE_COMPRESS_LIMIT(index());
      if (os_aio_pending_reads_approx() &&
          trx_sys.history_size_approx() > BTR_CUR_FINE_HISTORY_LENGTH)
      {
        /* Most delete-intended operations are due to the purge of history.
        Prioritize them when the history list is growing huge. */
        mtr_x_lock_index(index(), mtr);
        break;
      }
    }
    mtr_sx_lock_index(index(), mtr);
    break;
#ifdef UNIV_DEBUG
  case BTR_CONT_MODIFY_TREE:
    ut_ad("invalid mode" == 0);
    break;
#endif
  case BTR_MODIFY_ROOT_AND_LEAF:
    rw_latch= RW_SX_LATCH;
    /* fall through */
  default:
    if (!latch_by_caller)
      mtr_s_lock_index(index(), mtr);
  }

  dberr_t err;

  if (!index()->table->space)
  {
  corrupted:
    ut_ad("corrupted" == 0); // FIXME: remove this
    err= DB_CORRUPTION;
  func_exit:
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
    return err;
  }

  const ulint zip_size= index()->table->space->zip_size();

  /* Start with the root page. */
  page_id_t page_id(index()->table->space_id, index()->page);

  const page_cur_mode_t page_mode= btr_cur_nonleaf_mode(mode);
  ulint height= ULINT_UNDEFINED;
  up_match= 0;
  up_bytes= 0;
  low_match= 0;
  low_bytes= 0;
  ulint buf_mode= BUF_GET;
 search_loop:
  auto block_savepoint= mtr->get_savepoint();
  buf_block_t *block=
    buf_page_get_gen(page_id, zip_size, rw_latch, guess, buf_mode, mtr,
                     &err, height == 0 && !index()->is_clust());
  if (!block)
  {
    if (err != DB_SUCCESS)
    {
      btr_read_failed(err, *index());
      goto func_exit;
    }

    /* This must be a search to perform an insert, delete mark, or delete;
    try using the change buffer */
    ut_ad(height == 0);
    ut_ad(thr);

    switch (btr_op) {
    default:
      MY_ASSERT_UNREACHABLE();
      break;
    case BTR_INSERT_OP:
    case BTR_INSERT_IGNORE_UNIQUE_OP:
      ut_ad(buf_mode == BUF_GET_IF_IN_POOL);

      if (ibuf_insert(IBUF_OP_INSERT, tuple, index(), page_id, zip_size, thr))
      {
        flag= BTR_CUR_INSERT_TO_IBUF;
        goto func_exit;
      }
      break;

    case BTR_DELMARK_OP:
      ut_ad(buf_mode == BUF_GET_IF_IN_POOL);

      if (ibuf_insert(IBUF_OP_DELETE_MARK, tuple,
                      index(), page_id, zip_size, thr))
      {
        flag = BTR_CUR_DEL_MARK_IBUF;
        goto func_exit;
      }

      break;

    case BTR_DELETE_OP:
      ut_ad(buf_mode == BUF_GET_IF_IN_POOL_OR_WATCH);
      auto& chain = buf_pool.page_hash.cell_get(page_id.fold());

      if (!row_purge_poss_sec(purge_node, index(), tuple, mtr))
        /* The record cannot be purged yet. */
        flag= BTR_CUR_DELETE_REF;
      else if (ibuf_insert(IBUF_OP_DELETE, tuple, index(),
                           page_id, zip_size, thr))
        /* The purge was buffered. */
        flag= BTR_CUR_DELETE_IBUF;
      else
      {
        /* The purge could not be buffered. */
        buf_pool.watch_unset(page_id, chain);
        break;
      }

      buf_pool.watch_unset(page_id, chain);
      goto func_exit;
    }

    /* Change buffering did not succeed, we must read the page. */
    buf_mode= BUF_GET;
    goto search_loop;
  }

  if (!!page_is_comp(block->page.frame) != index()->table->not_redundant() ||
      btr_page_get_index_id(block->page.frame) != index()->id ||
      fil_page_get_type(block->page.frame) == FIL_PAGE_RTREE ||
      !fil_page_index_page_check(block->page.frame))
    goto corrupted;

  page_cur.block= block;
  ut_ad(block == mtr->at_savepoint(block_savepoint));
  const bool not_first_access{buf_page_make_young_if_needed(&block->page)};
#ifdef UNIV_ZIP_DEBUG
  if (const page_zip_des_t *page_zip= buf_block_get_page_zip(block))
    ut_a(page_zip_validate(page_zip, block->page.frame, index()));
#endif /* UNIV_ZIP_DEBUG */

  uint32_t page_level= btr_page_get_level(block->page.frame);

  if (height == ULINT_UNDEFINED)
  {
    /* We are in the B-tree index root page. */
#ifdef BTR_CUR_ADAPT
    info->root_guess= block;
#endif
  reached_root:
    height= page_level;
    tree_height= height + 1;

    if (!height)
    {
      /* The root page is also a leaf page.
      We may have to reacquire the page latch in a different mode. */
      switch (rw_latch) {
      case RW_S_LATCH:
        if (!(latch_mode & BTR_SEARCH_LEAF))
        {
          rw_latch= RW_X_LATCH;
          ut_ad(rw_lock_type_t(latch_mode & ~12) == RW_X_LATCH);
          mtr->lock_register(block_savepoint, MTR_MEMO_PAGE_X_FIX);
          if (!block->page.lock.s_x_upgrade_try())
          {
            block->page.lock.s_unlock();
            block->page.lock.x_lock();
            /* Dropping the index tree (and freeing the root page)
            should be impossible while we hold index()->lock. */
            ut_ad(!block->page.is_freed());
            page_level= btr_page_get_level(block->page.frame);
            if (UNIV_UNLIKELY(page_level != 0))
            {
              /* btr_root_raise_and_insert() was executed meanwhile */
              ut_ad(mtr->memo_contains_flagged(&index()->lock,
                                               MTR_MEMO_S_LOCK));
              block->page.lock.x_u_downgrade();
              block->page.lock.u_s_downgrade();
              rw_latch= RW_S_LATCH;
              mtr->lock_register(block_savepoint, MTR_MEMO_PAGE_S_FIX);
              goto reached_root;
            }
          }
        }
        if (latch_mode == BTR_MODIFY_PREV)
          goto reached_leaf;
        if (rw_latch != RW_S_LATCH)
          break;
        if (!latch_by_caller)
          /* Release the tree s-latch */
          mtr->rollback_to_savepoint(savepoint, savepoint + 1);
        goto reached_latched_leaf;
      case RW_SX_LATCH:
        ut_ad(latch_mode == BTR_MODIFY_ROOT_AND_LEAF);
        static_assert(int{BTR_MODIFY_ROOT_AND_LEAF} == int{RW_SX_LATCH}, "");
        rw_latch= RW_X_LATCH;
        mtr->lock_register(block_savepoint, MTR_MEMO_PAGE_X_FIX);
        block->page.lock.u_x_upgrade();
        break;
      case RW_X_LATCH:
        if (latch_mode == BTR_MODIFY_TREE)
          goto reached_index_root_and_leaf;
        break;
      case RW_NO_LATCH:
        ut_ad(0);
      }
      goto reached_root_and_leaf;
    }
  }
  else if (UNIV_UNLIKELY(height != page_level))
    goto corrupted;
  else
    switch (latch_mode) {
    case BTR_MODIFY_TREE:
      break;
    case BTR_MODIFY_ROOT_AND_LEAF:
      ut_ad((mtr->at_savepoint(block_savepoint - 1)->page.id().page_no() ==
             index()->page) == (tree_height <= height + 2));
      if (tree_height <= height + 2)
        /* Retain the root page latch. */
        break;
      /* fall through */
    default:
      ut_ad(block_savepoint > savepoint);
      mtr->rollback_to_savepoint(block_savepoint - 1, block_savepoint);
      block_savepoint--;
    }

  if (!height)
  {
  reached_leaf:
    /* We reached the leaf level. */
    ut_ad(block == mtr->at_savepoint(block_savepoint));

    if (latch_mode == BTR_MODIFY_ROOT_AND_LEAF)
    {
    reached_root_and_leaf:
      if (!latch_by_caller)
        mtr->rollback_to_savepoint(savepoint, savepoint + 1);
    reached_index_root_and_leaf:
      ut_ad(rw_latch == RW_X_LATCH);
#ifdef BTR_CUR_HASH_ADAPT
      btr_search_drop_page_hash_index(block, true);
#endif
      if (page_cur_search_with_match(tuple, mode, &up_match, &low_match,
                                     &page_cur, nullptr))
        goto corrupted;
      ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_GE);
      ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
      ut_ad(low_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
      goto func_exit;
    }

    switch (latch_mode) {
    case BTR_SEARCH_PREV:
    case BTR_MODIFY_PREV:
      static_assert(BTR_MODIFY_PREV & BTR_MODIFY_LEAF, "");
      static_assert(BTR_SEARCH_PREV & BTR_SEARCH_LEAF, "");
      ut_ad(!latch_by_caller);
      ut_ad(rw_latch ==
            rw_lock_type_t(latch_mode & (RW_X_LATCH | RW_S_LATCH)));

      /* latch also siblings from left to right */
      if (page_has_prev(block->page.frame) &&
          !btr_latch_prev(rw_latch, page_id, &err, mtr))
        goto func_exit;
      if (page_has_next(block->page.frame) &&
          !btr_block_get(*index(), btr_page_get_next(block->page.frame),
                         rw_latch, false, mtr, &err))
        goto func_exit;
      goto release_tree;
    case BTR_SEARCH_LEAF:
    case BTR_MODIFY_LEAF:
      if (!latch_by_caller)
      {
release_tree:
        /* Release the tree s-latch */
        block_savepoint--;
        mtr->rollback_to_savepoint(savepoint, savepoint + 1);
      }
      /* release upper blocks */
      if (savepoint < block_savepoint)
        mtr->rollback_to_savepoint(savepoint, block_savepoint);
      break;
    default:
      ut_ad(latch_mode == BTR_MODIFY_TREE);
      ut_ad(rw_latch == RW_X_LATCH);
      /* x-latch also siblings from left to right */
      if (page_has_prev(block->page.frame) &&
          !btr_latch_prev(rw_latch, page_id, &err, mtr))
        goto func_exit;
      if (page_has_next(block->page.frame) &&
          !btr_block_get(*index(), btr_page_get_next(block->page.frame),
                         RW_X_LATCH, false, mtr, &err))
        goto func_exit;
    }

  reached_latched_leaf:
#ifdef BTR_CUR_HASH_ADAPT
    if (!(tuple->info_bits & REC_INFO_MIN_REC_FLAG) && !index()->is_ibuf() &&
        btr_search.enabled)
    {
      if (page_cur_search_with_match_bytes(*tuple, mode, &up_match, &low_match,
                                           &page_cur, &up_bytes, &low_bytes))
        goto corrupted;
    }
    else
#endif /* BTR_CUR_HASH_ADAPT */
      if (page_cur_search_with_match(tuple, mode, &up_match, &low_match,
                                     &page_cur, nullptr))
        goto corrupted;

    ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_GE);
    ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
    ut_ad(low_match != uint16_t(~0U) || mode != PAGE_CUR_LE);

    if (latch_mode == BTR_MODIFY_TREE &&
        btr_cur_need_opposite_intention(block->page, index()->is_clust(),
                                        lock_intention,
                                        node_ptr_max_size, compress_limit,
                                        page_cur.rec))
        goto need_opposite_intention;

#ifdef BTR_CUR_HASH_ADAPT
    /* We do a dirty read of btr_search.enabled here.  We will recheck in
    btr_search_build_page_hash_index() before building a page hash
    index, while holding search latch. */
    if (!btr_search.enabled);
    else if (tuple->info_bits & REC_INFO_MIN_REC_FLAG)
      /* This may be a search tuple for btr_pcur_t::restore_position(). */
      ut_ad(tuple->is_metadata() ||
            (tuple->is_metadata(tuple->info_bits ^ REC_STATUS_INSTANT)));
    else if (index()->table->is_temporary());
    else if (!rec_is_metadata(page_cur.rec, *index()) &&
             index()->search_info.hash_analysis_useful())
      search_info_update();
#endif /* BTR_CUR_HASH_ADAPT */

    goto func_exit;
  }

  guess= nullptr;
  if (page_cur_search_with_match(tuple, page_mode, &up_match, &low_match,
                                 &page_cur, nullptr))
    goto corrupted;
  offsets= rec_get_offsets(page_cur.rec, index(), offsets, 0, ULINT_UNDEFINED,
                           &heap);

  ut_ad(block == mtr->at_savepoint(block_savepoint));

  switch (latch_mode) {
  default:
    break;
  case BTR_MODIFY_TREE:
    if (btr_cur_need_opposite_intention(block->page, index()->is_clust(),
                                        lock_intention,
                                        node_ptr_max_size, compress_limit,
                                        page_cur.rec))
      /* If the rec is the first or last in the page for pessimistic
      delete intention, it might cause node_ptr insert for the upper
      level. We should change the intention and retry. */
    need_opposite_intention:
      return pessimistic_search_leaf(tuple, mode, mtr);

    if (detected_same_key_root || lock_intention != BTR_INTENTION_BOTH ||
        index()->is_unique() ||
        (up_match <= rec_offs_n_fields(offsets) &&
         low_match <= rec_offs_n_fields(offsets)))
      break;

    /* If the first or the last record of the page or the same key
    value to the first record or last record, then another page might
    be chosen when BTR_CONT_MODIFY_TREE.  So, the parent page should
    not released to avoiding deadlock with blocking the another search
    with the same key value. */
    const rec_t *first=
      page_rec_get_next_const(page_get_infimum_rec(block->page.frame));
    ulint matched_fields;

    if (UNIV_UNLIKELY(!first))
      goto corrupted;
    if (page_cur.rec == first ||
        page_rec_is_last(page_cur.rec, block->page.frame))
    {
    same_key_root:
      detected_same_key_root= true;
      break;
    }

    matched_fields= 0;
    offsets2= rec_get_offsets(first, index(), offsets2, 0, ULINT_UNDEFINED,
                              &heap);
    cmp_rec_rec(page_cur.rec, first, offsets, offsets2, index(), false,
                &matched_fields);
    if (matched_fields >= rec_offs_n_fields(offsets) - 1)
      goto same_key_root;
    if (const rec_t* last=
        page_rec_get_prev_const(page_get_supremum_rec(block->page.frame)))
    {
      matched_fields= 0;
      offsets2= rec_get_offsets(last, index(), offsets2, 0, ULINT_UNDEFINED,
                                &heap);
      cmp_rec_rec(page_cur.rec, last, offsets, offsets2, index(), false,
                  &matched_fields);
      if (matched_fields >= rec_offs_n_fields(offsets) - 1)
        goto same_key_root;
    }
    else
      goto corrupted;

    /* Release the non-root parent page unless it may need to be modified. */
    if (tree_height > height + 1 &&
        !btr_cur_will_modify_tree(index(), block->page.frame, lock_intention,
                                  page_cur.rec, node_ptr_max_size,
                                  zip_size, mtr))
    {
      mtr->rollback_to_savepoint(block_savepoint - 1, block_savepoint);
      block_savepoint--;
    }
  }

  /* Go to the child node */
  page_id.set_page_no(btr_node_ptr_get_child_page_no(page_cur.rec, offsets));

  if (!--height)
  {
    /* We are about to access the leaf level. */

    switch (latch_mode) {
    case BTR_MODIFY_ROOT_AND_LEAF:
      rw_latch= RW_X_LATCH;
      break;
    case BTR_MODIFY_PREV: /* ibuf_insert() or btr_pcur_move_to_prev() */
    case BTR_SEARCH_PREV: /* btr_pcur_move_to_prev() */
      ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_X_LATCH);

      if (!not_first_access)
        buf_read_ahead_linear(page_id, false);

      if (page_has_prev(block->page.frame) &&
          page_rec_is_first(page_cur.rec, block->page.frame))
      {
        ut_ad(block_savepoint + 1 == mtr->get_savepoint());

        /* Latch the previous page if the node pointer is the leftmost
        of the current page. */
        int ret= btr_latch_prev(rw_latch, page_id, &err, mtr);
        if (!ret)
          goto func_exit;
        ut_ad(block_savepoint + 2 == mtr->get_savepoint());
        if (ret < 0)
        {
          up_match= 0, low_match= 0, up_bytes= 0, low_bytes= 0;
          /* While our latch on the level-2 page prevents splits or
          merges of this level-1 block, other threads may have
          modified it due to splitting or merging some level-0 (leaf)
          pages underneath it. Thus, we must search again. */
          if (page_cur_search_with_match(tuple, page_mode,
                                         &up_match, &low_match,
                                         &page_cur, nullptr))
            goto corrupted;
          offsets= rec_get_offsets(page_cur.rec, index(), offsets, 0,
                                   ULINT_UNDEFINED, &heap);
          page_id.set_page_no(btr_node_ptr_get_child_page_no(page_cur.rec,
                                                             offsets));
        }
      }
      rw_latch= rw_lock_type_t(latch_mode & (RW_X_LATCH | RW_S_LATCH));
      break;
    case BTR_MODIFY_LEAF:
    case BTR_SEARCH_LEAF:
      rw_latch= rw_lock_type_t(latch_mode);
      if (btr_op != BTR_NO_OP && !index()->is_ibuf() &&
          ibuf_should_try(index(), btr_op != BTR_INSERT_OP))
        /* Try to buffer the operation if the leaf page
        is not in the buffer pool. */
        buf_mode= btr_op == BTR_DELETE_OP
          ? BUF_GET_IF_IN_POOL_OR_WATCH
          : BUF_GET_IF_IN_POOL;
      else if (!not_first_access)
        buf_read_ahead_linear(page_id, false);
      break;
    case BTR_MODIFY_TREE:
      ut_ad(rw_latch == RW_X_LATCH);

      if (lock_intention == BTR_INTENTION_INSERT &&
          page_has_next(block->page.frame) &&
          page_rec_is_last(page_cur.rec, block->page.frame))
      {
        /* btr_insert_into_right_sibling() might cause deleting node_ptr
        at upper level */
        mtr->rollback_to_savepoint(block_savepoint);
        goto need_opposite_intention;
      }
      break;
    default:
      ut_ad(rw_latch == RW_X_LATCH);
    }
  }

  goto search_loop;
}

ATTRIBUTE_COLD void mtr_t::index_lock_upgrade()
{
  auto &slot= m_memo[get_savepoint() - 1];
  if (slot.type == MTR_MEMO_X_LOCK)
    return;
  ut_ad(slot.type == MTR_MEMO_SX_LOCK);
  index_lock *lock= static_cast<index_lock*>(slot.object);
  lock->u_x_upgrade(SRW_LOCK_CALL);
  slot.type= MTR_MEMO_X_LOCK;
}

/** Mark a non-leaf page "least recently used", but avoid invoking
buf_page_t::set_accessed(), because we do not want linear read-ahead */
static void btr_cur_nonleaf_make_young(buf_page_t *bpage)
{
  if (UNIV_UNLIKELY(buf_page_peek_if_too_old(bpage)))
    buf_page_make_young(bpage);
}

ATTRIBUTE_COLD
dberr_t btr_cur_t::pessimistic_search_leaf(const dtuple_t *tuple,
                                           page_cur_mode_t mode, mtr_t *mtr)
{
  ut_ad(index()->is_btree() || index()->is_ibuf());
  ut_ad(!index()->is_ibuf() || ibuf_inside(mtr));

  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs* offsets= offsets_;
  rec_offs_init(offsets_);

  ut_ad(flag == BTR_CUR_BINARY);
  ut_ad(dict_index_check_search_tuple(index(), tuple));
  ut_ad(dtuple_check_typed(tuple));
  buf_block_t *block= mtr->at_savepoint(1);
  ut_ad(block->page.id().page_no() == index()->page);
  block->page.fix();
  mtr->rollback_to_savepoint(1);
  mtr->index_lock_upgrade();

  const page_cur_mode_t page_mode{btr_cur_nonleaf_mode(mode)};

  mtr->page_lock(block, RW_X_LATCH);

  up_match= 0;
  up_bytes= 0;
  low_match= 0;
  low_bytes= 0;
  ulint height= btr_page_get_level(block->page.frame);
  tree_height= height + 1;
  mem_heap_t *heap= nullptr;

 search_loop:
  dberr_t err;
  page_cur.block= block;

  if (UNIV_UNLIKELY(!height))
  {
    if (page_cur_search_with_match(tuple, mode, &up_match, &low_match,
                                   &page_cur, nullptr))
    corrupted:
      err= DB_CORRUPTION;
    else
    {
      ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_GE);
      ut_ad(up_match != uint16_t(~0U) || mode != PAGE_CUR_LE);
      ut_ad(low_match != uint16_t(~0U) || mode != PAGE_CUR_LE);

#ifdef BTR_CUR_HASH_ADAPT
      /* We do a dirty read of btr_search.enabled here.  We will recheck in
      btr_search_build_page_hash_index() before building a page hash
      index, while holding search latch. */
      if (!btr_search.enabled);
      else if (tuple->info_bits & REC_INFO_MIN_REC_FLAG)
        /* This may be a search tuple for btr_pcur_t::restore_position(). */
        ut_ad(tuple->is_metadata() ||
              (tuple->is_metadata(tuple->info_bits ^ REC_STATUS_INSTANT)));
      else if (index()->table->is_temporary());
      else if (!rec_is_metadata(page_cur.rec, *index()) &&
               index()->search_info.hash_analysis_useful())
        search_info_update();
#endif /* BTR_CUR_HASH_ADAPT */
      err= DB_SUCCESS;
    }

  func_exit:
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
    return err;
  }

  if (page_cur_search_with_match(tuple, page_mode, &up_match, &low_match,
                                 &page_cur, nullptr))
    goto corrupted;

  page_id_t page_id{block->page.id()};

  offsets= rec_get_offsets(page_cur.rec, index(), offsets, 0, ULINT_UNDEFINED,
                           &heap);
  /* Go to the child node */
  page_id.set_page_no(btr_node_ptr_get_child_page_no(page_cur.rec, offsets));

  block=
    buf_page_get_gen(page_id, block->zip_size(), RW_X_LATCH, nullptr, BUF_GET,
                     mtr, &err, !--height && !index()->is_clust());

  if (!block)
  {
    btr_read_failed(err, *index());
    goto func_exit;
  }

  if (!!page_is_comp(block->page.frame) != index()->table->not_redundant() ||
      btr_page_get_index_id(block->page.frame) != index()->id ||
      fil_page_get_type(block->page.frame) == FIL_PAGE_RTREE ||
      !fil_page_index_page_check(block->page.frame))
    goto corrupted;

  if (height != btr_page_get_level(block->page.frame))
    goto corrupted;

  btr_cur_nonleaf_make_young(&block->page);

#ifdef UNIV_ZIP_DEBUG
  const page_zip_des_t *page_zip= buf_block_get_page_zip(block);
  ut_a(!page_zip || page_zip_validate(page_zip, block->page.frame, index()));
#endif /* UNIV_ZIP_DEBUG */

  if (page_has_prev(block->page.frame) &&
      !btr_latch_prev(RW_X_LATCH, page_id, &err, mtr))
    goto func_exit;
  if (page_has_next(block->page.frame) &&
      !btr_block_get(*index(), btr_page_get_next(block->page.frame),
                     RW_X_LATCH, false, mtr, &err))
    goto func_exit;
  goto search_loop;
}

/********************************************************************//**
Searches an index tree and positions a tree cursor on a given non-leaf level.
NOTE: n_fields_cmp in tuple must be set so that it cannot be compared
to node pointer page number fields on the upper levels of the tree!
cursor->up_match and cursor->low_match both will have sensible values.
Cursor is left at the place where an insert of the
search tuple should be performed in the B-tree. InnoDB does an insert
immediately after the cursor. Thus, the cursor may end up on a user record,
or on a page infimum record.
@param level      the tree level of search
@param tuple      data tuple; NOTE: n_fields_cmp in tuple must be set so that
                  it cannot get compared to the node ptr page number field!
@param latch      RW_S_LATCH or RW_X_LATCH
@param cursor     tree cursor; the cursor page is s- or x-latched, but see also
                  above!
@param mtr        mini-transaction
@return DB_SUCCESS on success or error code otherwise */
TRANSACTIONAL_TARGET
dberr_t btr_cur_search_to_nth_level(ulint level,
                                    const dtuple_t *tuple,
                                    rw_lock_type_t rw_latch,
                                    btr_cur_t *cursor, mtr_t *mtr)
{
  dict_index_t *const index= cursor->index();

  ut_ad(index->is_btree() || index->is_ibuf());
  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs_init(offsets_);
  ut_ad(level);
  ut_ad(dict_index_check_search_tuple(index, tuple));
  ut_ad(index->is_ibuf() ? ibuf_inside(mtr) : index->is_btree());
  ut_ad(dtuple_check_typed(tuple));
  ut_ad(index->page != FIL_NULL);

  MEM_UNDEFINED(&cursor->up_bytes, sizeof cursor->up_bytes);
  MEM_UNDEFINED(&cursor->low_bytes, sizeof cursor->low_bytes);
  cursor->up_match= 0;
  cursor->low_match= 0;
  cursor->flag= BTR_CUR_BINARY;

#ifndef BTR_CUR_ADAPT
  buf_block_t *block= nullptr;
#else
  buf_block_t *block= index->search_info.root_guess;
#endif /* BTR_CUR_ADAPT */

  ut_ad(mtr->memo_contains_flagged(&index->lock,
                                   MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));

  const ulint zip_size= index->table->space->zip_size();

  /* Start with the root page. */
  page_id_t page_id(index->table->space_id, index->page);
  ulint height= ULINT_UNDEFINED;

search_loop:
  dberr_t err= DB_SUCCESS;
  if (buf_block_t *b=
      mtr->get_already_latched(page_id, mtr_memo_type_t(rw_latch)))
    block= b;
  else if (!(block= buf_page_get_gen(page_id, zip_size, rw_latch,
                                     block, BUF_GET, mtr, &err)))
  {
    btr_read_failed(err, *index);
    goto func_exit;
  }
  else
    btr_cur_nonleaf_make_young(&block->page);

#ifdef UNIV_ZIP_DEBUG
  if (const page_zip_des_t *page_zip= buf_block_get_page_zip(block))
    ut_a(page_zip_validate(page_zip, block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */

  if (!!page_is_comp(block->page.frame) != index->table->not_redundant() ||
      btr_page_get_index_id(block->page.frame) != index->id ||
      fil_page_get_type(block->page.frame) == FIL_PAGE_RTREE ||
      !fil_page_index_page_check(block->page.frame))
  {
  corrupted:
    err= DB_CORRUPTION;
  func_exit:
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
    return err;
  }

  const uint32_t page_level= btr_page_get_level(block->page.frame);

  if (height == ULINT_UNDEFINED)
  {
    /* We are in the root node */
    height= page_level;
    if (!height)
      goto corrupted;
    cursor->tree_height= height + 1;
  }
  else if (height != ulint{page_level})
    goto corrupted;

  cursor->page_cur.block= block;

  /* Search for complete index fields. */
  if (page_cur_search_with_match(tuple, PAGE_CUR_LE, &cursor->up_match,
                                 &cursor->low_match, &cursor->page_cur,
                                 nullptr))
    goto corrupted;

  /* If this is the desired level, leave the loop */
  if (level == height)
    goto func_exit;

  ut_ad(height > level);
  height--;

  offsets = rec_get_offsets(cursor->page_cur.rec, index, offsets, 0,
                            ULINT_UNDEFINED, &heap);
  /* Go to the child node */
  page_id.set_page_no(btr_node_ptr_get_child_page_no(cursor->page_cur.rec,
                                                     offsets));
  block= nullptr;
  goto search_loop;
}

dberr_t btr_cur_t::open_leaf(bool first, dict_index_t *index,
                             btr_latch_mode latch_mode, mtr_t *mtr)
{
  ulint n_blocks= 0;
  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  dberr_t err;

  rec_offs_init(offsets_);

  const bool latch_by_caller= latch_mode & BTR_ALREADY_S_LATCHED;
  latch_mode= btr_latch_mode(latch_mode & ~BTR_ALREADY_S_LATCHED);

  btr_intention_t lock_intention= btr_cur_get_and_clear_intention(&latch_mode);

  /* Store the position of the tree latch we push to mtr so that we
  know how to release it when we have latched the leaf node */

  auto savepoint= mtr->get_savepoint();

  rw_lock_type_t upper_rw_latch= RW_X_LATCH;
  ulint node_ptr_max_size= 0, compress_limit= 0;

  if (latch_mode == BTR_MODIFY_TREE)
  {
    node_ptr_max_size= btr_node_ptr_max_size(index);
    /* Most of delete-intended operations are purging. Free blocks
    and read IO bandwidth should be prioritized for them, when the
    history list is growing huge. */
    savepoint++;
    if (lock_intention == BTR_INTENTION_DELETE)
    {
      compress_limit= BTR_CUR_PAGE_COMPRESS_LIMIT(index);

      if (os_aio_pending_reads_approx() &&
          trx_sys.history_size_approx() > BTR_CUR_FINE_HISTORY_LENGTH)
      {
        mtr_x_lock_index(index, mtr);
        goto index_locked;
      }
    }
    mtr_sx_lock_index(index, mtr);
  }
  else
  {
    static_assert(int{BTR_CONT_MODIFY_TREE} == (12 | BTR_MODIFY_LEAF), "");
    ut_ad(!(latch_mode & 8));
    /* This function doesn't need to lock left page of the leaf page */
    static_assert(int{BTR_SEARCH_PREV} == (4 | BTR_SEARCH_LEAF), "");
    static_assert(int{BTR_MODIFY_PREV} == (4 | BTR_MODIFY_LEAF), "");
    latch_mode= btr_latch_mode(latch_mode & ~4);
    ut_ad(!latch_by_caller ||
          mtr->memo_contains_flagged(&index->lock,
                                     MTR_MEMO_SX_LOCK | MTR_MEMO_S_LOCK));
    upper_rw_latch= RW_S_LATCH;
    if (!latch_by_caller)
    {
      savepoint++;
      mtr_s_lock_index(index, mtr);
    }
  }

index_locked:
  ut_ad(savepoint == mtr->get_savepoint());

  const rw_lock_type_t root_leaf_rw_latch=
    rw_lock_type_t(latch_mode & (RW_S_LATCH | RW_X_LATCH));

  page_cur.index = index;

  uint32_t page= index->page;

  for (ulint height= ULINT_UNDEFINED;;)
  {
    ut_ad(n_blocks < BTR_MAX_LEVELS);
    ut_ad(savepoint + n_blocks == mtr->get_savepoint());

    bool first_access= false;
    buf_block_t* block=
      btr_block_get(*index, page,
                    height ? upper_rw_latch : root_leaf_rw_latch,
                    !height, mtr, &err, &first_access);
    ut_ad(!block == (err != DB_SUCCESS));

    if (!block)
      break;

    if (first)
      page_cur_set_before_first(block, &page_cur);
    else
      page_cur_set_after_last(block, &page_cur);

    const uint32_t l= btr_page_get_level(block->page.frame);

    if (height == ULINT_UNDEFINED)
    {
      /* We are in the root node */
      height= l;
      if (height);
      else if (upper_rw_latch != root_leaf_rw_latch)
      {
        /* We should retry to get the page, because the root page
        is latched with different level as a leaf page. */
        ut_ad(n_blocks == 0);
        ut_ad(root_leaf_rw_latch != RW_NO_LATCH);
        upper_rw_latch= root_leaf_rw_latch;
        mtr->rollback_to_savepoint(savepoint);
        height= ULINT_UNDEFINED;
        continue;
      }
      else
      {
      reached_leaf:
        const auto leaf_savepoint= mtr->get_savepoint();
        ut_ad(leaf_savepoint);
        ut_ad(block == mtr->at_savepoint(leaf_savepoint - 1));

        if (latch_mode == BTR_MODIFY_TREE)
        {
          /* x-latch also siblings from left to right */
          if (page_has_prev(block->page.frame) &&
              !btr_latch_prev(RW_X_LATCH, block->page.id(), &err, mtr))
            break;
          if (page_has_next(block->page.frame) &&
              !btr_block_get(*index, btr_page_get_next(block->page.frame),
                             RW_X_LATCH, false, mtr, &err))
            break;

          if (!index->lock.have_x() &&
              btr_cur_need_opposite_intention(block->page, index->is_clust(),
                                              lock_intention,
                                              node_ptr_max_size,
                                              compress_limit, page_cur.rec))
            goto need_opposite_intention;
        }
        else
        {
          if (latch_mode != BTR_CONT_MODIFY_TREE)
          {
            ut_ad(latch_mode == BTR_MODIFY_LEAF ||
                  latch_mode == BTR_SEARCH_LEAF);
            /* Release index->lock if needed, and the non-leaf pages. */
            mtr->rollback_to_savepoint(savepoint - !latch_by_caller,
                                       leaf_savepoint - 1);
          }
        }
        break;
      }
    }
    else if (UNIV_UNLIKELY(height != l))
    {
    corrupted:
      err= DB_CORRUPTION;
      break;
    }

    if (!height)
      goto reached_leaf;

    height--;

    if (first
        ? !page_cur_move_to_next(&page_cur)
        : !page_cur_move_to_prev(&page_cur))
      goto corrupted;

    offsets= rec_get_offsets(page_cur.rec, index, offsets, 0, ULINT_UNDEFINED,
                             &heap);
    page= btr_node_ptr_get_child_page_no(page_cur.rec, offsets);

    ut_ad(latch_mode != BTR_MODIFY_TREE || upper_rw_latch == RW_X_LATCH);

    if (latch_mode != BTR_MODIFY_TREE)
    {
      if (!height && first && first_access)
        buf_read_ahead_linear({block->page.id().space(), page}, false);
    }
    else if (btr_cur_need_opposite_intention(block->page, index->is_clust(),
                                             lock_intention,
                                             node_ptr_max_size, compress_limit,
                                             page_cur.rec))
    {
    need_opposite_intention:
      /* If the rec is the first or last in the page for pessimistic
      delete intention, it might cause node_ptr insert for the upper
      level. We should change the intention and retry. */

      mtr->rollback_to_savepoint(savepoint);
      mtr->index_lock_upgrade();
      /* X-latch all pages from now on */
      latch_mode= BTR_CONT_MODIFY_TREE;
      page= index->page;
      height= ULINT_UNDEFINED;
      n_blocks= 0;
      continue;
    }
    else
    {
      if (!btr_cur_will_modify_tree(index, block->page.frame,
                                    lock_intention, page_cur.rec,
                                    node_ptr_max_size,
                                    index->table->space->zip_size(), mtr))
      {
        ut_ad(n_blocks);
        /* release buffer-fixes on pages that will not be modified
        (except the root) */
        if (n_blocks > 1)
        {
          mtr->rollback_to_savepoint(savepoint + 1, savepoint + n_blocks - 1);
          n_blocks= 1;
        }
      }
    }

    /* Go to the child node */
    n_blocks++;
  }

  if (UNIV_LIKELY_NULL(heap))
    mem_heap_free(heap);

  return err;
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
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	page_cur_t*	page_cursor;
	rec_t*		rec;

	ut_ad(dtuple_check_typed(tuple));

	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));
	page_cursor = btr_cur_get_page_cur(cursor);

	/* Now, try the insert */
	rec = page_cur_tuple_insert(page_cursor, tuple, offsets, heap, n_ext,
				    mtr);

	/* If the record did not fit, reorganize.
	For compressed pages, page_cur_tuple_insert()
	attempted this already. */
	if (!rec && !page_cur_get_page_zip(page_cursor)
	    && btr_page_reorganize(page_cursor, mtr) == DB_SUCCESS) {
		rec = page_cur_tuple_insert(page_cursor, tuple, offsets, heap,
					    n_ext, mtr);
	}

	ut_ad(!rec || rec_offs_validate(rec, page_cursor->index, *offsets));
	return(rec);
}

/*************************************************************//**
For an insert, checks the locks and does the undo logging if desired.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_FAIL, or error number */
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
	if (!(~flags | (BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG))) {
		return DB_SUCCESS;
	}

	/* Check if we have to wait for a lock: enqueue an explicit lock
	request if yes */

	rec_t* rec = btr_cur_get_rec(cursor);
	dict_index_t* index = cursor->index();

	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));
	ut_ad((flags & BTR_NO_UNDO_LOG_FLAG)
	      || !index->table->skip_alter_undo);

	ut_ad(mtr->is_named_space(index->table->space));

	/* Check if there is predicate or GAP lock preventing the insertion */
	if (!(flags & BTR_NO_LOCKING_FLAG)) {
		const unsigned type = index->type;
		if (UNIV_UNLIKELY(type & DICT_SPATIAL)) {
			lock_prdt_t	prdt;
			rtr_mbr_t	mbr;

			rtr_get_mbr_from_tuple(entry, &mbr);

			/* Use on stack MBR variable to test if a lock is
			needed. If so, the predicate (MBR) will be allocated
			from lock heap in lock_prdt_insert_check_and_lock() */
			lock_init_prdt_from_mbr(&prdt, &mbr, 0, nullptr);

			if (dberr_t err = lock_prdt_insert_check_and_lock(
				    rec, btr_cur_get_block(cursor),
				    index, thr, mtr, &prdt)) {
				return err;
			}
			*inherit = false;
		} else {
			ut_ad(!dict_index_is_online_ddl(index)
			      || index->is_primary()
			      || (flags & BTR_CREATE_FLAG));
#ifdef WITH_WSREP
			trx_t* trx= thr_get_trx(thr);
			/* If transaction scanning an unique secondary
			key is wsrep high priority thread (brute
			force) this scanning may involve GAP-locking
			in the index. As this locking happens also
			when applying replication events in high
			priority applier threads, there is a
			probability for lock conflicts between two
			wsrep high priority threads. To avoid this
			GAP-locking we mark that this transaction
			is using unique key scan here. */
			if ((type & (DICT_CLUSTERED | DICT_UNIQUE)) == DICT_UNIQUE
			    && trx->is_wsrep()
			    && wsrep_thd_is_BF(trx->mysql_thd, false)) {
				trx->wsrep = 3;
			}
#endif /* WITH_WSREP */
			if (dberr_t err = lock_rec_insert_check_and_lock(
				    rec, btr_cur_get_block(cursor),
				    index, thr, mtr, inherit)) {
				return err;
			}
		}
	}

	if (!index->is_primary() || !page_is_leaf(btr_cur_get_page(cursor))) {
		return DB_SUCCESS;
	}

	constexpr roll_ptr_t dummy_roll_ptr = roll_ptr_t{1}
		<< ROLL_PTR_INSERT_FLAG_POS;
	roll_ptr_t roll_ptr = dummy_roll_ptr;

	if (!(flags & BTR_NO_UNDO_LOG_FLAG)) {
		if (dberr_t err = trx_undo_report_row_operation(
			    thr, index, entry, NULL, 0, NULL, NULL,
			    &roll_ptr)) {
			return err;
		}

		if (roll_ptr != dummy_roll_ptr) {
			dfield_t* r = dtuple_get_nth_field(entry,
							   index->db_trx_id());
			trx_write_trx_id(static_cast<byte*>(r->data),
					 thr_get_trx(thr)->id);
		}
	}

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		dfield_t* r = dtuple_get_nth_field(
			entry, index->db_roll_ptr());
		ut_ad(r->len == DATA_ROLL_PTR_LEN);
		trx_write_roll_ptr(static_cast<byte*>(r->data), roll_ptr);
	}

	return DB_SUCCESS;
}

/**
Prefetch siblings of the leaf for the pessimistic operation.
@param block	leaf page
@param index    index of the page */
static void btr_cur_prefetch_siblings(const buf_block_t *block,
                                      const dict_index_t *index)
{
  ut_ad(page_is_leaf(block->page.frame));

  if (index->is_ibuf())
    return;

  const page_t *page= block->page.frame;
  uint32_t prev= mach_read_from_4(my_assume_aligned<4>(page + FIL_PAGE_PREV));
  uint32_t next= mach_read_from_4(my_assume_aligned<4>(page + FIL_PAGE_NEXT));

  fil_space_t *space= index->table->space;

  if (prev == FIL_NULL);
  else if (space->acquire())
    buf_read_page_background(space, page_id_t(space->id, prev),
                             block->zip_size());
  if (next == FIL_NULL);
  else if (space->acquire())
    buf_read_page_background(space, page_id_t(space->id, next),
                             block->zip_size());
}

/*************************************************************//**
Tries to perform an insert to a page in an index tree, next to cursor.
It is assumed that mtr holds an x-latch on the page. The operation does
not succeed if there is too little space on the page. If there is just
one record on the page, the insert will always succeed; this is to
prevent trying to split a page with just one record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_FAIL, or error number */
dberr_t
btr_cur_optimistic_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags: if not
				zero, the parameters index and thr should be
				specified */
	btr_cur_t*	cursor,	/*!< in: cursor on page after which to insert;
				cursor stays valid */
	rec_offs**	offsets,/*!< out: offsets on *rec */
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
	bool		reorg __attribute__((unused));
	bool		inherit = true;
	ulint		rec_size;
	dberr_t		err;

	ut_ad(thr || !(~flags & (BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG)));
	*big_rec = NULL;

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	index = cursor->index();

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));
	ut_ad(dtuple_check_typed(entry));

#ifdef HAVE_valgrind
	if (block->page.zip.data) {
		MEM_CHECK_DEFINED(page, srv_page_size);
		MEM_CHECK_DEFINED(block->page.zip.data, block->zip_size());
	}
#endif /* HAVE_valgrind */

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

	LIMIT_OPTIMISTIC_INSERT_DEBUG(page_get_n_recs(page), goto fail);

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
		if (leaf) {
			btr_cur_prefetch_siblings(block, index);
		}
fail_err:

		if (big_rec_vec) {
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);
		}

		return(err);
	}

	ulint	max_size = page_get_max_insert_size_after_reorganize(page, 1);
	if (max_size < rec_size) {
		goto fail;
	}

	const ulint n_recs = page_get_n_recs(page);
	if (UNIV_UNLIKELY(n_recs >= 8189)) {
		ut_ad(srv_page_size == 65536);
		goto fail;
	}

	if (page_has_garbage(page)) {
		if (max_size < BTR_CUR_PAGE_REORGANIZE_LIMIT
		    && n_recs > 1
		    && page_get_max_insert_size(page, 1) < rec_size) {

			goto fail;
		}
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
			ut_a(!n_recs || btr_page_reorganize(page_cursor, mtr)
			     == DB_SUCCESS););

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
		    && leaf && index->is_primary()) {
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
				ut_ad(thr->graph->trx->bulk_insert
				      || thr->graph->trx->id
				      == trx_read_trx_id(
					      static_cast<const byte*>(
							trx_id->data))
				      || index->table->is_temporary());
			}
		}
#endif

		*rec = page_cur_tuple_insert(page_cursor, entry, offsets, heap,
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
		reorg = true;

		/* If the record did not fit, reorganize */
		err = btr_page_reorganize(page_cursor, mtr);
		if (err != DB_SUCCESS
		    || page_get_max_insert_size(page, 1) != max_size
		    || !(*rec = page_cur_tuple_insert(page_cursor, entry,
						      offsets, heap, n_ext,
						      mtr))) {
			err = DB_CORRUPTION;
			goto fail_err;
		}
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (!leaf) {
	} else if (entry->info_bits & REC_INFO_MIN_REC_FLAG) {
		ut_ad(entry->is_metadata());
		ut_ad(index->is_instant());
		ut_ad(flags == BTR_NO_LOCKING_FLAG);
	} else if (!index->table->is_temporary()) {
		btr_search_update_hash_on_insert(cursor, reorg);
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
	rec_offs**	offsets,/*!< out: offsets on *rec */
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
	dict_index_t*	index		= cursor->index();
	big_rec_t*	big_rec_vec	= NULL;
	bool		inherit = false;
	uint32_t	n_reserved	= 0;

	ut_ad(dtuple_check_typed(entry));
	ut_ad(thr || !(~flags & (BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG)));

	*big_rec = NULL;

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));
	ut_ad(!dict_index_is_online_ddl(index)
	      || dict_index_is_clust(index)
	      || (flags & BTR_CREATE_FLAG));

	cursor->flag = BTR_CUR_BINARY;

	/* Check locks and write to undo log, if specified */

	dberr_t err = btr_cur_ins_lock_and_undo(flags, cursor, entry,
						thr, mtr, &inherit);

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* First reserve enough free space for the file segments of
	the index tree, so that the insert will not fail because of
	lack of space */

	if (!index->is_ibuf()
	    && (err = fsp_reserve_free_extents(&n_reserved, index->table->space,
					       uint32_t(cursor->tree_height / 16
							+ 3),
					       FSP_NORMAL, mtr))
	    != DB_SUCCESS) {
		return err;
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

	*rec = index->page == btr_cur_get_block(cursor)->page.id().page_no()
		? btr_root_raise_and_insert(flags, cursor, offsets, heap,
					    entry, n_ext, mtr, &err)
		: btr_page_split_and_insert(flags, cursor, offsets, heap,
					    entry, n_ext, mtr, &err);

	if (!*rec) {
		goto func_exit;
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
		if (entry->info_bits & REC_INFO_MIN_REC_FLAG) {
			ut_ad(entry->is_metadata());
			ut_ad(index->is_instant());
			ut_ad(flags & BTR_NO_LOCKING_FLAG);
			ut_ad(!(flags & BTR_CREATE_FLAG));
		} else if (!index->table->is_temporary()) {
			btr_search_update_hash_on_insert(cursor, false);
		}
#endif /* BTR_CUR_HASH_ADAPT */
		if (inherit && !(flags & BTR_NO_LOCKING_FLAG)) {

			lock_update_insert(btr_cur_get_block(cursor), *rec);
		}
	}

	err = DB_SUCCESS;
func_exit:
	index->table->space->release_free_extents(n_reserved);
	*big_rec = big_rec_vec;

	return err;
}

/*==================== B-TREE UPDATE =========================*/

/*************************************************************//**
For an update, checks the locks and does the undo logging.
@return DB_SUCCESS, DB_LOCK_WAIT, or error number */
UNIV_INLINE MY_ATTRIBUTE((warn_unused_result))
dberr_t
btr_cur_upd_lock_and_undo(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor on record to update */
	const rec_offs*	offsets,/*!< in: rec_get_offsets() on cursor */
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
	index = cursor->index();

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
			btr_cur_get_block(cursor), rec, index,
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

MY_ATTRIBUTE((warn_unused_result))
/** Update DB_TRX_ID, DB_ROLL_PTR in a clustered index record.
@param[in,out]  block           clustered index leaf page
@param[in,out]  rec             clustered index record
@param[in]      index           clustered index
@param[in]      offsets         rec_get_offsets(rec, index)
@param[in]      trx             transaction
@param[in]      roll_ptr        DB_ROLL_PTR value
@param[in,out]  mtr             mini-transaction
@return error code */
static dberr_t btr_cur_upd_rec_sys(buf_block_t *block, rec_t *rec,
                                   dict_index_t *index, const rec_offs *offsets,
                                   const trx_t *trx, roll_ptr_t roll_ptr,
                                   mtr_t *mtr)
{
  ut_ad(index->is_primary());
  ut_ad(rec_offs_validate(rec, index, offsets));

  if (UNIV_LIKELY_NULL(block->page.zip.data))
  {
    page_zip_write_trx_id_and_roll_ptr(block, rec, offsets, index->db_trx_id(),
                                       trx->id, roll_ptr, mtr);
    return DB_SUCCESS;
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
    if (UNIV_UNLIKELY(!prev || prev == rec))
      return DB_CORRUPTION;
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
      mtr->memmove(*block, dest - block->page.frame, src - block->page.frame,
                   d);
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
    mtr->memcpy<mtr_t::MAYBE_NOP>(*block, dest, sys + d, len);

  return DB_SUCCESS;
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
#ifdef UNIV_DEBUG
	rec_offs*	offsets,/*!< in/out: offsets of the cursor record */
#endif /* UNIV_DEBUG */
	ulint		length,	/*!< in: size needed */
	bool		create,	/*!< in: true=delete-and-insert,
				false=update-in-place */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	dict_index_t*	index = cursor->index;

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

	if (btr_page_reorganize(cursor, mtr) == DB_SUCCESS) {
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
			return true;
		}
	}

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
                              const rec_offs *offsets, const upd_t *update,
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

		mtr->write<1,mtr_t::MAYBE_NOP>(*block, info_bits,
					       (*info_bits
						& ~REC_INFO_BITS_MASK)
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
			switch (ulint size = rec_get_nth_field_size(rec, n)) {
			case 0:
				break;
			case 1:
				mtr->write<1,mtr_t::MAYBE_NOP>(
					*block,
					rec_get_field_start_offs(rec, n) + rec,
					0U);
				break;
			default:
				mtr->memset(
					block,
					rec_get_field_start_offs(rec, n) + rec
					- block->page.frame,
					size, 0);
			}
			ulint l = rec_get_1byte_offs_flag(rec)
				? (n + 1) : (n + 1) * 2;
			byte* b = rec - REC_N_OLD_EXTRA_BYTES - l;
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
			byte* b = rec - REC_N_OLD_EXTRA_BYTES - l;
			compile_time_assert(REC_1BYTE_SQL_NULL_MASK << 8
					    == REC_2BYTE_SQL_NULL_MASK);
			mtr->write<1>(*block, b,
				      byte(*b & ~REC_1BYTE_SQL_NULL_MASK));
		}

		if (len) {
			mtr->memcpy<mtr_t::MAYBE_NOP>(*block, data,
						      uf->new_val.data, len);
		}
	}

	if (UNIV_LIKELY(!block->page.zip.data)) {
		return;
	}

	switch (update->n_fields) {
	case 0:
		/* We only changed the delete-mark flag. */
		return;
	case 1:
		if (!index->is_clust()
		    || update->fields[0].field_no != index->db_roll_ptr()) {
			break;
		}
		goto update_sys;
	case 2:
		if (!index->is_clust()
		    || update->fields[0].field_no != index->db_trx_id()
		    || update->fields[1].field_no != index->db_roll_ptr()) {
			break;
		}
	update_sys:
		ulint len;
		const byte* sys = rec_get_nth_field(rec, offsets,
						    index->db_trx_id(), &len);
		ut_ad(len == DATA_TRX_ID_LEN);
		page_zip_write_trx_id_and_roll_ptr(
			block, rec, offsets, index->db_trx_id(),
			trx_read_trx_id(sys),
			trx_read_roll_ptr(sys + DATA_TRX_ID_LEN), mtr);
		return;
	}

	page_zip_write_rec(block, rec, index, offsets, 0, mtr);
}

/** Check if a ROW_FORMAT=COMPRESSED page can be updated in place
@param cur     cursor pointing to ROW_FORMAT=COMPRESSED page
@param offsets rec_get_offsets(btr_cur_get_rec(cur))
@param update  index fields being updated
@param mtr     mini-transaction
@return the record in the ROW_FORMAT=COMPRESSED page
@retval nullptr if the page cannot be updated in place */
ATTRIBUTE_COLD static
rec_t *btr_cur_update_in_place_zip_check(btr_cur_t *cur, rec_offs *offsets,
                                         const upd_t& update, mtr_t *mtr)
{
  dict_index_t *index= cur->index();
  ut_ad(!index->table->is_temporary());

  switch (update.n_fields) {
  case 0:
    /* We are only changing the delete-mark flag. */
    break;
  case 1:
    if (!index->is_clust() ||
        update.fields[0].field_no != index->db_roll_ptr())
      goto check_for_overflow;
    /* We are only changing the delete-mark flag and DB_ROLL_PTR. */
    break;
  case 2:
    if (!index->is_clust() ||
        update.fields[0].field_no != index->db_trx_id() ||
        update.fields[1].field_no != index->db_roll_ptr())
      goto check_for_overflow;
    /* We are only changing DB_TRX_ID, DB_ROLL_PTR, and the delete-mark.
    They can be updated in place in the uncompressed part of the
    ROW_FORMAT=COMPRESSED page. */
    break;
  check_for_overflow:
  default:
    if (!btr_cur_update_alloc_zip(btr_cur_get_page_zip(cur),
                                  btr_cur_get_page_cur(cur),
                                  offsets, rec_offs_size(offsets),
                                  false, mtr))
      return nullptr;
  }

  return btr_cur_get_rec(cur);
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
	rec_offs*	offsets,/*!< in/out: offsets on cursor->page_cur.rec */
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

	ut_ad(page_is_leaf(cursor->page_cur.block->page.frame));
	rec = btr_cur_get_rec(cursor);
	index = cursor->index();
	ut_ad(!index->is_ibuf());
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!!page_rec_is_comp(rec) == dict_table_is_comp(index->table));
	ut_ad(trx_id > 0 || (flags & BTR_KEEP_SYS_FLAG)
	      || index->table->is_temporary());
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
	if (UNIV_LIKELY_NULL(page_zip)
	    && !(rec = btr_cur_update_in_place_zip_check(
			 cursor, offsets, *update, mtr))) {
		return DB_ZIP_OVERFLOW;
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
		err = btr_cur_upd_rec_sys(block, rec, index, offsets,
					  thr_get_trx(thr), roll_ptr, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			goto func_exit;
		}
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
		auto part = block->index
			? &btr_search.get_part(*index) : nullptr;
		if (part) {
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

			part->latch.wr_lock(SRW_LOCK_CALL);
		}

		assert_block_ahi_valid(block);
#endif /* BTR_CUR_HASH_ADAPT */

		btr_cur_upd_rec_in_place(rec, index, offsets, update, block,
					 mtr);

#ifdef BTR_CUR_HASH_ADAPT
		if (part) {
			part->latch.wr_unlock();
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
		entry->n_fields = uint16_t(n_fields);
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
	if (!block) {
		ut_ad("corruption" == 0);
		mtr.commit();
		return;
	}
	ut_ad(fil_page_get_type(block->page.frame) == FIL_PAGE_TYPE_BLOB);
	ut_ad(mach_read_from_4(&block->page.frame
			       [FIL_PAGE_DATA + BTR_BLOB_HDR_NEXT_PAGE_NO])
	      == FIL_NULL);
	ut_ad(mach_read_from_4(&block->page.frame
			       [FIL_PAGE_DATA + BTR_BLOB_HDR_PART_LEN])
	      == mach_read_from_4(ptr + BTR_EXTERN_LEN + 4));
	n_fields = mach_read_from_4(
		&block->page.frame[FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE])
		+ index->first_user_field();
	/* Rollback should not increase the number of fields. */
	ut_ad(n_fields <= index->n_fields);
	ut_ad(n_fields + 1 <= entry->n_fields);
	/* dict_index_t::clear_instant_alter() cannot be invoked while
	rollback of an instant ALTER TABLE transaction is in progress
	for an is_alter_metadata() record. */
	ut_ad(n_fields >= index->n_core_fields);

	mtr.commit();
	entry->n_fields = uint16_t(n_fields + 1);
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
			entry->n_fields = uint16_t(n_fields);
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
	rec_offs**	offsets,/*!< out: offsets on cursor->page_cur.rec */
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
	index = cursor->index();
	ut_ad(index->has_locking());
	ut_ad(trx_id > 0 || (flags & BTR_KEEP_SYS_FLAG)
	      || index->table->is_temporary());
	ut_ad(!!page_rec_is_comp(rec) == dict_table_is_comp(index->table));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
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

	*offsets = rec_get_offsets(rec, index, *offsets, index->n_core_fields,
				   ULINT_UNDEFINED, heap);
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	ut_a(!rec_offs_any_null_extern(rec, *offsets)
	     || thr_get_trx(thr) == trx_roll_crash_recv_trx);
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
		ut_ad(!index->is_ibuf());
		/* Externally stored fields are treated in pessimistic
		update */

		/* prefetch siblings of the leaf for the pessimistic
		operation. */
		btr_cur_prefetch_siblings(block, index);

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
			    page_zip, page_cursor, *offsets,
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
	if (index->has_locking()) {
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

	page_cur_delete_rec(page_cursor, *offsets, mtr);

	if (!page_cur_move_to_prev(page_cursor)) {
		return DB_CORRUPTION;
	}

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		btr_cur_write_sys(new_entry, index, trx_id, roll_ptr);
	}

	rec = btr_cur_insert_if_possible(cursor, new_entry, offsets, heap,
					 0/*n_ext*/, mtr);
	if (UNIV_UNLIKELY(!rec)) {
		goto corrupted;
	}

	if (UNIV_UNLIKELY(update->is_metadata())) {
		/* We must empty the PAGE_FREE list, because if this
		was a rollback, the shortened metadata record
		would have too many fields, and we would be unable to
		know the size of the freed record. */
		err = btr_page_reorganize(page_cursor, mtr);
		if (err != DB_SUCCESS) {
			goto func_exit;
		}
	} else {
		/* Restore the old explicit lock state on the record */
		lock_rec_restore_from_page_infimum(*block, rec,
						   block->page.id());
	}

	ut_ad(err == DB_SUCCESS);
	if (!page_cur_move_to_next(page_cursor)) {
corrupted:
		err = DB_CORRUPTION;
	}

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
		btr_cur_prefetch_siblings(block, index);
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
dberr_t
btr_cur_pess_upd_restore_supremum(
/*==============================*/
	buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*	rec,	/*!< in: updated record */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*		page;

	page = buf_block_get_frame(block);

	if (page_rec_get_next(page_get_infimum_rec(page)) != rec) {
		/* Updated record is not the first user record on its page */
		return DB_SUCCESS;
	}

	const uint32_t	prev_page_no = btr_page_get_prev(page);

	const page_id_t block_id{block->page.id()};
	const page_id_t	prev_id(block_id.space(), prev_page_no);
	buf_block_t* prev_block
		= mtr->get_already_latched(prev_id, MTR_MEMO_PAGE_X_FIX);
	if (UNIV_UNLIKELY(!prev_block)) {
		return DB_CORRUPTION;
	}
	ut_ad(!memcmp_aligned<4>(prev_block->page.frame + FIL_PAGE_NEXT,
				 block->page.frame + FIL_PAGE_OFFSET, 4));

	lock_rec_reset_and_inherit_gap_locks(*prev_block, block_id,
					     PAGE_HEAP_NO_SUPREMUM,
					     page_is_comp(page)
					     ? rec_get_heap_no_new(rec)
					     : rec_get_heap_no_old(rec));
	return DB_SUCCESS;
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
	rec_offs**	offsets,/*!< out: offsets on cursor->page_cur.rec */
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
	bool		was_first;
	uint32_t	n_reserved	= 0;

	*offsets = NULL;
	*big_rec = NULL;

	block = btr_cur_get_block(cursor);
	page_zip = buf_block_get_page_zip(block);
	index = cursor->index();
	ut_ad(index->has_locking());

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK |
					 MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip
	     || page_zip_validate(page_zip, block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */
	ut_ad(!page_zip || !index->table->is_temporary());
	/* The insert buffer tree should never be updated in place. */
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(trx_id > 0 || (flags & BTR_KEEP_SYS_FLAG)
	      || index->table->is_temporary());
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
		    && page_is_leaf(block->page.frame)) {
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

	ut_ad(!page_is_comp(block->page.frame) || !rec_get_node_ptr_flag(rec));
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
		    page_is_comp(block->page.frame),
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
			     || page_zip_validate(page_zip, block->page.frame,
						  index));
#endif /* UNIV_ZIP_DEBUG */
			index->table->space->release_free_extents(n_reserved);
			err = DB_TOO_BIG_RECORD;
			goto err_exit;
		}

		ut_ad(page_is_leaf(block->page.frame));
		ut_ad(dict_index_is_clust(index));
		if (UNIV_UNLIKELY(!(flags & BTR_KEEP_POS_FLAG))) {
			ut_ad(page_zip != NULL);
			dtuple_convert_back_big_rec(index, new_entry,
						    big_rec_vec);
			big_rec_vec = NULL;
			n_ext = dtuple_get_n_ext(new_entry);
		}
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

		err = fsp_reserve_free_extents(
			&n_reserved, index->table->space,
			uint32_t(cursor->tree_height / 16 + 3),
			flags & BTR_NO_UNDO_LOG_FLAG
			? FSP_CLEANING : FSP_NORMAL,
			mtr);
                if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			err = DB_OUT_OF_FILE_SPACE;
			goto err_exit;
		}
	}

	if (!(flags & BTR_KEEP_SYS_FLAG)) {
		btr_cur_write_sys(new_entry, index, trx_id, roll_ptr);
	}

	const ulint max_ins_size = page_zip
		? 0
		: page_get_max_insert_size_after_reorganize(block->page.frame,
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
		lock_rec_store_on_page_infimum(block, rec);
	}

#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip
	     || page_zip_validate(page_zip, block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */
	page_cursor = btr_cur_get_page_cur(cursor);

	page_cur_delete_rec(page_cursor, *offsets, mtr);

	if (!page_cur_move_to_prev(page_cursor)) {
		err = DB_CORRUPTION;
		goto return_after_reservations;
	}

	rec = btr_cur_insert_if_possible(cursor, new_entry,
					 offsets, offsets_heap, n_ext, mtr);

	if (rec) {
		page_cursor->rec = rec;

		if (UNIV_UNLIKELY(is_metadata)) {
			/* We must empty the PAGE_FREE list, because if this
			was a rollback, the shortened metadata record
			would have too many fields, and we would be unable to
			know the size of the freed record. */
			err = btr_page_reorganize(page_cursor, mtr);
			if (err != DB_SUCCESS) {
				goto return_after_reservations;
			}
			rec = page_cursor->rec;
			rec_offs_make_valid(rec, index, true, *offsets);
			if (page_cursor->block->page.id().page_no()
			    == index->page) {
				btr_set_instant(page_cursor->block, *index,
						mtr);
			}
		} else {
			lock_rec_restore_from_page_infimum(
				*btr_cur_get_block(cursor), rec,
				block->page.id());
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
		ut_ad(!adjust || page_is_leaf(block->page.frame));

		if (btr_cur_compress_if_useful(cursor, adjust, mtr)) {
			if (adjust) {
				rec_offs_make_valid(page_cursor->rec, index,
						    true, *offsets);
			}
		} else if (!dict_index_is_clust(index)
			   && page_is_leaf(block->page.frame)) {
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

#if 0 // FIXME: this used to be a no-op, and will cause trouble if enabled
		if (!big_rec_vec
		    && page_is_leaf(block->page.frame)
		    && !dict_index_is_online_ddl(index)) {
			mtr->release(index->lock);
			/* NOTE: We cannot release root block latch here, because it
			has segment header and already modified in most of cases.*/
		}
#endif

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
		    && page_is_leaf(block->page.frame)) {
			ibuf_reset_free_bits(block);
		}
	}

	if (big_rec_vec != NULL) {
		ut_ad(page_is_leaf(block->page.frame));
		ut_ad(dict_index_is_clust(index));
		ut_ad(flags & BTR_KEEP_POS_FLAG);

		/* btr_page_split_and_insert() in
		btr_cur_pessimistic_insert() invokes
		mtr->release(index->lock).
		We must keep the index->lock when we created a
		big_rec, so that row_upd_clust_rec() can store the
		big_rec in the same mini-transaction. */

		ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
						 | MTR_MEMO_SX_LOCK));
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
	ut_a(err == DB_SUCCESS);
	ut_a(rec);
	ut_a(dummy_big_rec == NULL);
	ut_ad(rec_offs_validate(rec, cursor->index(), *offsets));
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
		ut_a(!page_zip
		     || page_zip_validate(page_zip, block->page.frame, index));
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
		err = btr_page_reorganize(page_cursor, mtr);
		if (err != DB_SUCCESS) {
			goto return_after_reservations;
		}
		rec = page_cursor->rec;
	} else {
		lock_rec_restore_from_page_infimum(
			*btr_cur_get_block(cursor), rec, block->page.id());
	}

	/* If necessary, restore also the correct lock state for a new,
	preceding supremum record created in a page split. While the old
	record was nonexistent, the supremum might have inherited its locks
	from a wrong record. */

	if (!was_first) {
		err = btr_cur_pess_upd_restore_supremum(
			btr_cur_get_block(cursor), rec, mtr);
	}

return_after_reservations:
#ifdef UNIV_ZIP_DEBUG
	ut_a(err ||
	     !page_zip || page_zip_validate(btr_cur_get_page_zip(cursor),
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
  if (UNIV_LIKELY(page_is_comp(block->page.frame) != 0))
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
    mtr->write<1,mtr_t::MAYBE_NOP>(*block, b, v);
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
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec) */
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
		 << ib::hex(trx->id) << ": "
		 << rec_printer(rec, offsets).str());

	return btr_cur_upd_rec_sys(block, rec, index, offsets, trx, roll_ptr,
				   mtr);
}

/*==================== B-TREE RECORD REMOVE =========================*/

/*************************************************************//**
Tries to compress a page of the tree if it seems useful. It is assumed
that mtr holds an x-latch on the tree and on the cursor page. To avoid
deadlocks, mtr must also own x-latches to brothers of page, if those
brothers exist. NOTE: it is assumed that the caller has reserved enough
free extents so that the compression will always succeed if done!
@return whether compression occurred */
bool
btr_cur_compress_if_useful(
/*=======================*/
	btr_cur_t*	cursor,	/*!< in/out: cursor on the page to compress;
				cursor does not stay valid if !adjust and
				compression occurs */
	bool		adjust,	/*!< in: whether the cursor position should be
				adjusted even when compression occurs */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
					 MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));

	if (cursor->index()->is_spatial()) {
		const trx_t*	trx = cursor->rtr_info->thr
			? thr_get_trx(cursor->rtr_info->thr)
			: NULL;
		const buf_block_t* block = btr_cur_get_block(cursor);

		/* Check whether page lock prevents the compression */
		if (!lock_test_prdt_page_lock(trx, block->page.id())) {
			return(false);
		}
	}

	return btr_cur_compress_recommendation(cursor, mtr)
		&& btr_compress(cursor, adjust, mtr) == DB_SUCCESS;
}

/*******************************************************//**
Removes the record on which the tree cursor is positioned on a leaf page.
It is assumed that the mtr has an x-latch on the page where the cursor is
positioned, but no latch on the whole tree.
@return error code
@retval DB_FAIL if the page would become too empty */
dberr_t
btr_cur_optimistic_delete(
/*======================*/
	btr_cur_t*	cursor,	/*!< in: cursor on leaf page, on the record to
				delete; cursor stays valid: if deletion
				succeeds, on function exit it points to the
				successor of the deleted record */
	ulint		flags,	/*!< in: BTR_CREATE_FLAG or 0 */
	mtr_t*		mtr)	/*!< in: mtr; if this function returns
				TRUE on a leaf page of a secondary
				index, the mtr must be committed
				before latching any further pages */
{
	buf_block_t*	block;
	rec_t*		rec;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(flags == 0 || flags == BTR_CREATE_FLAG);
	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->is_named_space(cursor->index()->table->space));
	ut_ad(!cursor->index()->is_dummy);

	/* This is intended only for leaf page deletions */

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.id().space() == cursor->index()->table->space->id);
	ut_ad(page_is_leaf(buf_block_get_frame(block)));
	ut_ad(!dict_index_is_online_ddl(cursor->index())
	      || cursor->index()->is_clust()
	      || (flags & BTR_CREATE_FLAG));

	rec = btr_cur_get_rec(cursor);

	offsets = rec_get_offsets(rec, cursor->index(), offsets,
				  cursor->index()->n_core_fields,
				  ULINT_UNDEFINED, &heap);

	dberr_t err = DB_SUCCESS;
	DBUG_EXECUTE_IF("btr_force_pessimistic_delete",
		err = DB_FAIL; goto func_exit;);

	if (rec_offs_any_extern(offsets)
	    || !btr_cur_can_delete_without_compress(cursor,
						    rec_offs_size(offsets),
						    mtr)) {
		/* prefetch siblings of the leaf for the pessimistic
		operation. */
		btr_cur_prefetch_siblings(block, cursor->index());
		err = DB_FAIL;
		goto func_exit;
	}

	if (UNIV_UNLIKELY(block->page.id().page_no() == cursor->index()->page
			  && page_get_n_recs(block->page.frame) == 1
			  + (cursor->index()->is_instant()
			     && !rec_is_metadata(rec, *cursor->index()))
			  && !cursor->index()
			  ->must_avoid_clear_instant_add())) {
		/* The whole index (and table) becomes logically empty.
		Empty the whole page. That is, if we are deleting the
		only user record, also delete the metadata record
		if one exists for instant ADD COLUMN (not generic ALTER TABLE).
		If we are deleting the metadata record and the
		table becomes empty, clean up the whole page. */
		dict_index_t* index = cursor->index();
		const rec_t* first_rec = page_rec_get_next_const(
			page_get_infimum_rec(block->page.frame));
		if (UNIV_UNLIKELY(!first_rec)) {
			err = DB_CORRUPTION;
			goto func_exit;
		}
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
			if (UNIV_LIKELY(!is_metadata && !flags)) {
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
			goto func_exit;
		}
	}

	{
		page_t*		page	= buf_block_get_frame(block);
		page_zip_des_t*	page_zip= buf_block_get_page_zip(block);

		if (UNIV_UNLIKELY(rec_get_info_bits(rec, page_is_comp(page))
				  & REC_INFO_MIN_REC_FLAG)) {
			/* This should be rolling back instant ADD COLUMN.
			If this is a recovered transaction, then
			index->is_instant() will hold until the
			insert into SYS_COLUMNS is rolled back. */
			ut_ad(cursor->index()->table->supports_instant());
			ut_ad(cursor->index()->is_primary());
			ut_ad(!page_zip);
			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    offsets, mtr);
			/* We must empty the PAGE_FREE list, because
			after rollback, this deleted metadata record
			would have too many fields, and we would be
			unable to know the size of the freed record. */
			err = btr_page_reorganize(btr_cur_get_page_cur(cursor),
						  mtr);
			goto func_exit;
		} else {
			if (!flags) {
				lock_update_delete(block, rec);
			}

			btr_search_update_hash_on_delete(cursor);
		}

		if (page_zip) {
#ifdef UNIV_ZIP_DEBUG
			ut_a(page_zip_validate(page_zip, page,
					       cursor->index()));
#endif /* UNIV_ZIP_DEBUG */
			page_cur_delete_rec(btr_cur_get_page_cur(cursor),
					    offsets, mtr);
#ifdef UNIV_ZIP_DEBUG
			ut_a(page_zip_validate(page_zip, page,
					       cursor->index()));
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
					    offsets, mtr);

			/* The change buffer does not handle inserts
			into non-leaf pages, into clustered indexes,
			or into the change buffer. */
			if (!cursor->index()->is_clust()
			    && !cursor->index()->table->is_temporary()
			    && !dict_index_is_ibuf(cursor->index())) {
				ibuf_update_free_bits_low(block, max_ins, mtr);
			}
		}
	}

func_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return err;
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
	uint32_t	n_reserved	= 0;
	ibool		ret		= FALSE;
	mem_heap_t*	heap;
	rec_offs*	offsets;
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
	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->is_named_space(index->table->space));
	ut_ad(!index->is_dummy);
	ut_ad(block->page.id().space() == index->table->space->id);

	if (!has_reserved_extents) {
		/* First reserve enough free space for the file segments
		of the index tree, so that the node pointer updates will
		not fail because of lack of space */

		uint32_t n_extents = uint32_t(cursor->tree_height / 32 + 1);

		*err = fsp_reserve_free_extents(&n_reserved,
						index->table->space,
						n_extents,
						FSP_CLEANING, mtr);
		if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
			return(FALSE);
		}
	}

	heap = mem_heap_create(1024);
	rec = btr_cur_get_rec(cursor);
	page_zip = buf_block_get_page_zip(block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	offsets = rec_get_offsets(rec, index, NULL, page_is_leaf(page)
				  ? index->n_core_fields : 0,
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
			rec, page_is_comp(block->page.frame));
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

		if (block->page.id().page_no() != index->page) {
			if (page_get_n_recs(page) < 2) {
				goto discard_page;
			}
		} else if (page_get_n_recs(page) == 1
			   + (index->is_instant() && !is_metadata)
			   && !index->must_avoid_clear_instant_add()) {
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
			if (UNIV_UNLIKELY(!first_rec)) {
				*err = DB_CORRUPTION;
				goto err_exit;
			}
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
					    offsets, mtr);
			/* We must empty the PAGE_FREE list, because
			after rollback, this deleted metadata record
			would carry too many fields, and we would be
			unable to know the size of the freed record. */
			*err = btr_page_reorganize(btr_cur_get_page_cur(cursor),
						   mtr);
			ut_ad(!ret);
			goto err_exit;
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

		if (UNIV_UNLIKELY(!(next_rec = page_rec_get_next(rec)))) {
			ut_ad(!ret);
			*err = DB_CORRUPTION;
			goto err_exit;
		}

		btr_cur_t cursor;
		cursor.page_cur.index = index;
		cursor.page_cur.block = block;

		if (!page_has_prev(page)) {
			/* If we delete the leftmost node pointer on a
			non-leaf level, we must mark the new leftmost node
			pointer as the predefined minimum record */

			min_mark_next_rec = true;
		} else if (index->is_spatial()) {
			/* For rtree, if delete the leftmost node pointer,
			we need to update parent page. */
			rtr_mbr_t	father_mbr;
			rec_t*		father_rec;
			rec_offs*	offsets;
			ulint		len;

			rtr_page_get_father_block(NULL, heap, mtr, NULL,
						  &cursor);
			father_rec = btr_cur_get_rec(&cursor);
			offsets = rec_get_offsets(father_rec, index, NULL,
						  0, ULINT_UNDEFINED, &heap);

			rtr_read_mbr(rec_get_nth_field(
				father_rec, offsets, 0, &len), &father_mbr);

			rtr_update_mbr_field(&cursor, offsets, NULL,
					     page, &father_mbr, next_rec, mtr);
			ut_d(parent_latched = true);
		} else {
			/* Otherwise, if we delete the leftmost node pointer
			on a page, we have to change the parent node pointer
			so that it is equal to the new leftmost node pointer
			on the page */
			ret = btr_page_get_father(mtr, &cursor);
			if (!ret) {
				*err = DB_CORRUPTION;
				goto err_exit;
			}
			*err = btr_cur_node_ptr_delete(&cursor, mtr);
			if (*err != DB_SUCCESS) {
got_err:
				ret = FALSE;
				goto err_exit;
			}

			const ulint	level = btr_page_get_level(page);
			// FIXME: reuse the node_ptr from above
			dtuple_t*	node_ptr = dict_index_build_node_ptr(
				index, next_rec, block->page.id().page_no(),
				heap, level);

			*err = btr_insert_on_non_leaf_level(
				flags, index, level + 1, node_ptr, mtr);
			if (*err != DB_SUCCESS) {
				ret = FALSE;
				goto got_err;
			}

			ut_d(parent_latched = true);
		}
	}

	/* SPATIAL INDEX never use U locks; we can allow page merges
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
		page_cur_delete_rec(btr_cur_get_page_cur(cursor),
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
					   << block->page.id()
					   << " in index " << index->name
					   << " of " << index->table->name;
				ut_ad("MDEV-14637" == 0);
			}
		}
	}

return_after_reservations:
	*err = DB_SUCCESS;
err_exit:
	mem_heap_free(heap);

#if 0 // FIXME: this used to be a no-op, and will cause trouble if enabled
	if (page_is_leaf(page)
	    && !dict_index_is_online_ddl(index)) {
		mtr->release(index->lock);
		/* NOTE: We cannot release root block latch here, because it
		has segment header and already modified in most of cases.*/
	}
#endif

	index->table->space->release_free_extents(n_reserved);
	return(ret);
}

/** Delete the node pointer in a parent page.
@param[in,out]	parent	cursor pointing to parent record
@param[in,out]	mtr	mini-transaction */
dberr_t btr_cur_node_ptr_delete(btr_cur_t* parent, mtr_t* mtr)
{
	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(parent),
					 MTR_MEMO_PAGE_X_FIX));
	dberr_t err;
	ibool compressed = btr_cur_pessimistic_delete(&err, TRUE, parent,
						      BTR_CREATE_FLAG, false,
						      mtr);
	if (err == DB_SUCCESS && !compressed) {
		btr_cur_compress_if_useful(parent, FALSE, mtr);
	}

	return err;
}

/** Represents the cursor for the number of rows estimation. The
content is used for level-by-level diving and estimation the number of rows
on each level. */
class btr_est_cur_t
{
  /* Assume a page like:
  records:             (inf, a, b, c, d, sup)
  index of the record:    0, 1, 2, 3, 4, 5
  */

  /** Index of the record where the page cursor stopped on this level
  (index in alphabetical order). In the above example, if the search stopped on
  record 'c', then nth_rec will be 3. */
  ulint m_nth_rec;

  /** Number of the records on the page, not counting inf and sup.
  In the above example n_recs will be 4. */
  ulint m_n_recs;

  /** Search tuple */
  const dtuple_t &m_tuple;
  /** Cursor search mode */
  page_cur_mode_t m_mode;
  /** Page cursor which is used for search */
  page_cur_t m_page_cur;
  /** Page id of the page to get on level down, can differ from
  m_block->page.id at the moment when the child's page id is already found, but
  the child's block has not fetched yet */
  page_id_t m_page_id;
  /** Current block */
  buf_block_t *m_block;
  /** Page search mode, can differ from m_mode for non-leaf pages, see c-tor
  comments for details */
  page_cur_mode_t m_page_mode;

  /** Matched fields and bytes which are used for on-page search, see
  btr_cur_t::(up|low)_(match|bytes) comments for details */
  uint16_t m_up_match= 0;
  uint16_t m_up_bytes= 0;
  uint16_t m_low_match= 0;
  uint16_t m_low_bytes= 0;

public:
  btr_est_cur_t(dict_index_t *index, const dtuple_t &tuple,
                page_cur_mode_t mode)
      : m_tuple(tuple), m_mode(mode),
        m_page_id(index->table->space_id, index->page), m_block(nullptr)
  {

    ut_ad(dict_index_check_search_tuple(index, &tuple));
    ut_ad(dtuple_check_typed(&tuple));

    m_page_cur.index = index;
    /* We use these modified search modes on non-leaf levels of the B-tree.
    These let us end up in the right B-tree leaf. In that leaf we use the
    original search mode. */
    switch (mode) {
    case PAGE_CUR_GE:
      m_page_mode= PAGE_CUR_L;
      break;
    case PAGE_CUR_G:
      m_page_mode= PAGE_CUR_LE;
      break;
    default:
      ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE);
      m_page_mode= mode;
      break;
    }
  }

  /** Retrieve block with m_page_id, release the previously gotten block
  if necessary. If this is a left border block cursor and both left and right
  border blocks have the same parent, don't unlatch the parent, as it must be
  latched to get the right block, and will be unlatched after the right block
  is fetched.
  @param  level distance from the leaf page level; ULINT_UNDEFINED when
          fetching the root page
  @param  mtr mtr
  @param  right_parent right border block parent, nullptr if the function
          is called for the right block itself
  @return true on success or false otherwise. */
  bool fetch_child(ulint level, mtr_t &mtr, const buf_block_t *right_parent)
  {
    buf_block_t *parent_block= m_block;

    m_block= btr_block_get(*index(), m_page_id.page_no(), RW_S_LATCH, !level,
                           &mtr, nullptr);
    if (!m_block)
      return false;

    if (parent_block && parent_block != right_parent)
    {
      ut_ad(mtr.get_savepoint() >= 2);
      mtr.rollback_to_savepoint(1, 2);
    }

    return level == ULINT_UNDEFINED ||
      btr_page_get_level(m_block->page.frame) == level;
  }

  /** Sets page mode for leaves */
  void set_page_mode_for_leaves() { m_page_mode= m_mode; }

  /** Does search on the current page. If there is no border in m_tuple, then
  just move the cursor to the most left or right record.
  @param level current level on tree.
  @param root_height root height
  @param left true if this is left border, false otherwise.
  @return true on success, false otherwise. */
  bool search_on_page(ulint level, ulint root_height, bool left)
  {
    if (level != btr_page_get_level(m_block->page.frame))
      return false;

    m_n_recs= page_get_n_recs(m_block->page.frame);

    if (dtuple_get_n_fields(&m_tuple) > 0)
    {
      m_up_bytes= m_low_bytes= 0;
      m_page_cur.block= m_block;
      if (page_cur_search_with_match(&m_tuple, m_page_mode,
                                     &m_up_match, &m_low_match, &m_page_cur,
                                     nullptr))
        return false;
      m_nth_rec= page_rec_get_n_recs_before(page_cur_get_rec(&m_page_cur));
    }
    else if (left)
    {
      page_cur_set_before_first(m_block, &m_page_cur);
      if (level)
      {
        if (!page_cur_move_to_next(&m_page_cur))
          return false;
        m_nth_rec= 1;
      }
      else
        m_nth_rec= 0;
    }
    else
    {
      m_nth_rec= m_n_recs;
      if (!level)
      {
        page_cur_set_after_last(m_block, &m_page_cur);
        ++m_nth_rec;
      }
      else
      {
        m_page_cur.block= m_block;
        m_page_cur.rec= page_rec_get_nth(m_block->page.frame, m_nth_rec);
      }
    }

    return true;
  }

  /** Read page id of the current record child.
  @param offsets offsets array.
  @param heap heap for offsets array */
  void read_child_page_id(rec_offs **offsets, mem_heap_t **heap)
  {
    const rec_t *node_ptr= page_cur_get_rec(&m_page_cur);

    /* FIXME: get the child page number directly without computing offsets */
    *offsets= rec_get_offsets(node_ptr, index(), *offsets, 0, ULINT_UNDEFINED,
                              heap);

    /* Go to the child node */
    m_page_id.set_page_no(btr_node_ptr_get_child_page_no(node_ptr, *offsets));
  }

  /** @return true if left border should be counted */
  bool should_count_the_left_border() const
  {
    if (dtuple_get_n_fields(&m_tuple) > 0)
    {
      ut_ad(!page_rec_is_infimum(page_cur_get_rec(&m_page_cur)));
      return !page_rec_is_supremum(page_cur_get_rec(&m_page_cur));
    }
    ut_ad(page_rec_is_infimum(page_cur_get_rec(&m_page_cur)));
    return false;
  }

  /** @return true if right border should be counted */
  bool should_count_the_right_border() const
  {
    if (dtuple_get_n_fields(&m_tuple) > 0)
    {
      const rec_t *rec= page_cur_get_rec(&m_page_cur);
      ut_ad(!(m_mode == PAGE_CUR_L && page_rec_is_supremum(rec)));

      return (m_mode == PAGE_CUR_LE /* if the range is '<=' */
              /* and the record was found */
              && m_low_match >= dtuple_get_n_fields(&m_tuple)) ||
             (m_mode == PAGE_CUR_L /* or if the range is '<' */
              /* and there are any records to match the criteria, i.e. if the
              minimum record on the tree is 5 and x < 7 is specified then the
              cursor will be positioned at 5 and we should count the border,
              but if x < 2 is specified, then the cursor will be positioned at
              'inf' and we should not count the border */
              && !page_rec_is_infimum(rec));
      /* Notice that for "WHERE col <= 'foo'" the server passes to
      ha_innobase::records_in_range(): min_key=NULL (left-unbounded) which is
      expected max_key='foo' flag=HA_READ_AFTER_KEY (PAGE_CUR_G), which is
      unexpected - one would expect flag=HA_READ_KEY_OR_PREV (PAGE_CUR_LE). In
      this case the cursor will be positioned on the first record to the right
      of the requested one (can also be positioned on the 'sup') and we should
      not count the right border. */
    }
    ut_ad(page_rec_is_supremum(page_cur_get_rec(&m_page_cur)));

    /* The range specified is without a right border, just 'x > 123'
    or 'x >= 123' and search_on_page() positioned the cursor on the
    supremum record on the rightmost page, which must not be counted. */
    return false;
  }

  /** @return index */
  const dict_index_t *index() const { return m_page_cur.index; }

  /** @return current block */
  const buf_block_t *block() const { return m_block; }

  /** @return current page id */
  page_id_t page_id() const { return m_page_id; }

  /** Copies block pointer and savepoint from another btr_est_cur_t in the case
  if both left and right border cursors point to the same block.
  @param o reference to the other btr_est_cur_t object. */
  void set_block(const btr_est_cur_t &o) { m_block= o.m_block; }

  /** @return current record number. */
  ulint nth_rec() const { return m_nth_rec; }

  /** @return number of records in the current page. */
  ulint n_recs() const { return m_n_recs; }
};

/** Estimate the number of rows between the left record of the path and the
right one(non-inclusive) for the certain level on a B-tree. This function
starts from the page next to the left page and reads a few pages to the right,
counting their records. If we reach the right page quickly then we know exactly
how many records there are between left and right records and we set
is_n_rows_exact to true. After some page is latched, the previous page is
unlatched. If we cannot reach the right page quickly then we calculate the
average number of records in the pages scanned so far and assume that all pages
that we did not scan up to the right page contain the same number of records,
then we multiply that average to the number of pages between right and left
records (which is n_rows_on_prev_level). In this case we set is_n_rows_exact to
false.
@param level current level.
@param left_cur the cursor of the left page.
@param right_page_no right page number.
@param n_rows_on_prev_level number of rows on the previous level.
@param[out] is_n_rows_exact true if exact rows number is returned.
@param[in,out] mtr mtr,
@return number of rows, not including the borders (exact or estimated). */
static ha_rows btr_estimate_n_rows_in_range_on_level(
    ulint level, btr_est_cur_t &left_cur, uint32_t right_page_no,
    ha_rows n_rows_on_prev_level, bool &is_n_rows_exact, mtr_t &mtr)
{
  ha_rows n_rows= 0;
  uint n_pages_read= 0;
  /* Do not read more than this number of pages in order not to hurt
  performance with this code which is just an estimation. If we read this many
  pages before reaching right_page_no, then we estimate the average from the
  pages scanned so far. */
  static constexpr uint n_pages_read_limit= 9;
  buf_block_t *block= nullptr;
  const dict_index_t *index= left_cur.index();

  /* Assume by default that we will scan all pages between left and right(non
  inclusive) pages */
  is_n_rows_exact= true;

  /* Add records from the left page which are to the right of the record which
  serves as a left border of the range, if any (we don't include the record
  itself in this count). */
  if (left_cur.nth_rec() <= left_cur.n_recs())
  {
    n_rows+= left_cur.n_recs() - left_cur.nth_rec();
  }

  /* Count the records in the pages between left and right (non inclusive)
  pages */

  const fil_space_t *space= index->table->space;
  page_id_t page_id(space->id,
                    btr_page_get_next(buf_block_get_frame(left_cur.block())));

  if (page_id.page_no() == FIL_NULL)
    goto inexact;

  do
  {
    page_t *page;
    buf_block_t *prev_block= block;

    /* Fetch the page. */
    block= btr_block_get(*index, page_id.page_no(), RW_S_LATCH, !level, &mtr,
                         nullptr);

    if (prev_block)
    {
      ulint savepoint = mtr.get_savepoint();
      /* Index s-lock, p1, p2 latches, can also be p1 and p2 parent latch if
      they are not diverged */
      ut_ad(savepoint >= 3);
      mtr.rollback_to_savepoint(savepoint - 2, savepoint - 1);
    }

    if (!block || btr_page_get_level(buf_block_get_frame(block)) != level)
      goto inexact;

    page= buf_block_get_frame(block);

    /* It is possible but highly unlikely that the page was originally written
    by an old version of InnoDB that did not initialize FIL_PAGE_TYPE on other
    than B-tree pages. For example, this could be an almost-empty BLOB page
    that happens to contain the magic values in the fields
    that we checked above. */

    n_pages_read++;

    n_rows+= page_get_n_recs(page);

    page_id.set_page_no(btr_page_get_next(page));

    if (n_pages_read == n_pages_read_limit)
    {
      /* We read too many pages or we reached the end of the level
      without passing through right_page_no. */
      goto inexact;
    }

  } while (page_id.page_no() != right_page_no);

  if (block)
  {
    ut_ad(block == mtr.at_savepoint(mtr.get_savepoint() - 1));
    mtr.rollback_to_savepoint(mtr.get_savepoint() - 1);
  }

  return (n_rows);

inexact:

  if (block)
  {
    ut_ad(block == mtr.at_savepoint(mtr.get_savepoint() - 1));
    mtr.rollback_to_savepoint(mtr.get_savepoint() - 1);
  }

  is_n_rows_exact= false;

  /* We did interrupt before reaching right page */

  if (n_pages_read > 0)
  {
    /* The number of pages on this level is
    n_rows_on_prev_level, multiply it by the
    average number of recs per page so far */
    n_rows= n_rows_on_prev_level * n_rows / n_pages_read;
  }
  else
  {
    n_rows= 10;
  }

  return (n_rows);
}

/** Estimates the number of rows in a given index range. Do search in the left
page, then if there are pages between left and right ones, read a few pages to
the right, if the right page is reached, count the exact number of rows without
fetching the right page, the right page will be fetched in the caller of this
function and the amount of its rows will be added. If the right page is not
reached, count the estimated(see btr_estimate_n_rows_in_range_on_level() for
details) rows number, and fetch the right page. If leaves are reached, unlatch
non-leaf pages except the right leaf parent. After the right leaf page is
fetched, commit mtr.
@param[in]  index index
@param[in]  range_start range start
@param[in]  range_end   range end
@return estimated number of rows; */
ha_rows btr_estimate_n_rows_in_range(dict_index_t *index,
                                     btr_pos_t *range_start,
                                     btr_pos_t *range_end)
{
  DBUG_ENTER("btr_estimate_n_rows_in_range");

  if (UNIV_UNLIKELY(index->page == FIL_NULL || index->is_corrupted()))
    DBUG_RETURN(0);

  ut_ad(index->is_btree());

  btr_est_cur_t p1(index, *range_start->tuple, range_start->mode);
  btr_est_cur_t p2(index, *range_end->tuple, range_end->mode);
  mtr_t mtr;

  ulint height;
  ulint root_height= 0; /* remove warning */

  mem_heap_t *heap= NULL;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs_init(offsets_);

  mtr.start();

  ut_ad(mtr.get_savepoint() == 0);
  mtr_s_lock_index(index, &mtr);

  ha_rows table_n_rows= dict_table_get_n_rows(index->table);

  height= ULINT_UNDEFINED;

  /* This becomes true when the two paths do not pass through the same pages
  anymore. */
  bool diverged= false;
  /* This is the height, i.e. the number of levels from the root, where paths
   are not the same or adjacent any more. */
  ulint divergence_height= ULINT_UNDEFINED;
  bool should_count_the_left_border= true;
  bool should_count_the_right_border= true;
  bool is_n_rows_exact= true;
  ha_rows n_rows= 0;

  /* Loop and search until we arrive at the desired level. */
search_loop:
  if (!p1.fetch_child(height, mtr, p2.block()))
    goto error;

  if (height == ULINT_UNDEFINED)
  {
    /* We are in the root node */
    height= btr_page_get_level(buf_block_get_frame(p1.block()));
    root_height= height;
  }

  if (!height)
  {
    p1.set_page_mode_for_leaves();
    p2.set_page_mode_for_leaves();
  }

  if (p1.page_id() == p2.page_id())
    p2.set_block(p1);
  else
  {
    ut_ad(diverged);
    if (divergence_height != ULINT_UNDEFINED) {
      /* We need to call p1.search_on_page() here as
      btr_estimate_n_rows_in_range_on_level() uses p1.m_n_recs and
      p1.m_nth_rec. */
      if (!p1.search_on_page(height, root_height, true))
        goto error;
      n_rows= btr_estimate_n_rows_in_range_on_level(
          height, p1, p2.page_id().page_no(), n_rows, is_n_rows_exact, mtr);
    }
    if (!p2.fetch_child(height, mtr, nullptr))
      goto error;
  }

  if (height == 0)
    /* There is no need to release non-leaf pages here as they must already be
    unlatched in btr_est_cur_t::fetch_child(). Try to search on pages after
    releasing the index latch, to decrease contention. */
    mtr.rollback_to_savepoint(0, 1);

  /* There is no need to search on left page if
  divergence_height != ULINT_UNDEFINED, as it was already searched before
  btr_estimate_n_rows_in_range_on_level() call */
  if (divergence_height == ULINT_UNDEFINED &&
      !p1.search_on_page(height, root_height, true))
    goto error;

  if (!p2.search_on_page(height, root_height, false))
    goto error;

  if (!diverged && (p1.nth_rec() != p2.nth_rec()))
  {
    ut_ad(p1.page_id() == p2.page_id());
    diverged= true;
    if (p1.nth_rec() < p2.nth_rec())
    {
      /* We do not count the borders (nor the left nor the right one), thus
      "- 1". */
      n_rows= p2.nth_rec() - p1.nth_rec() - 1;

      if (n_rows > 0)
      {
        /* There is at least one row between the two borders pointed to by p1
        and p2, so on the level below the slots will point to non-adjacent
        pages. */
        divergence_height= root_height - height;
      }
    }
    else
    {
      /* It is possible that p1->nth_rec > p2->nth_rec if, for example, we have
      a single page tree which contains (inf, 5, 6, supr) and we select where x
      > 20 and x < 30; in this case p1->nth_rec will point to the supr record
      and p2->nth_rec will point to 6. */
      n_rows= 0;
      should_count_the_left_border= false;
      should_count_the_right_border= false;
    }
  }
  else if (diverged && divergence_height == ULINT_UNDEFINED)
  {

    if (p1.nth_rec() < p1.n_recs() || p2.nth_rec() > 1)
    {
      ut_ad(p1.page_id() != p2.page_id());
      divergence_height= root_height - height;

      n_rows= 0;

      if (p1.nth_rec() < p1.n_recs())
      {
        n_rows+= p1.n_recs() - p1.nth_rec();
      }

      if (p2.nth_rec() > 1)
      {
        n_rows+= p2.nth_rec() - 1;
      }
    }
  }
  else if (divergence_height != ULINT_UNDEFINED)
  {
    /* All records before the right page was already counted. Add records from
    p2->page_no which are to the left of the record which servers as a right
    border of the range, if any (we don't include the record itself in this
    count). */
    if (p2.nth_rec() > 1)
      n_rows+= p2.nth_rec() - 1;
  }

  if (height)
  {
    ut_ad(height > 0);
    height--;
    ut_ad(mtr.memo_contains(p1.index()->lock, MTR_MEMO_S_LOCK));
    ut_ad(mtr.memo_contains_flagged(p1.block(), MTR_MEMO_PAGE_S_FIX));
    p1.read_child_page_id(&offsets, &heap);
    ut_ad(mtr.memo_contains(p2.index()->lock, MTR_MEMO_S_LOCK));
    ut_ad(mtr.memo_contains_flagged(p2.block(), MTR_MEMO_PAGE_S_FIX));
    p2.read_child_page_id(&offsets, &heap);
    goto search_loop;
  }

  should_count_the_left_border=
      should_count_the_left_border && p1.should_count_the_left_border();
  should_count_the_right_border=
      should_count_the_right_border && p2.should_count_the_right_border();

  mtr.commit();
  if (UNIV_LIKELY_NULL(heap))
    mem_heap_free(heap);


  range_start->page_id= p1.page_id();
  range_end->page_id= p2.page_id();

  /* Here none of the borders were counted. For example, if on the leaf level
  we descended to:
  (inf, a, b, c, d, e, f, sup)
           ^        ^
         path1    path2
  then n_rows will be 2 (c and d). */

  if (is_n_rows_exact)
  {
    /* Only fiddle to adjust this off-by-one if the number is exact, otherwise
    we do much grosser adjustments below. */

    /* If both paths end up on the same record on the leaf level. */
    if (p1.page_id() == p2.page_id() && p1.nth_rec() == p2.nth_rec())
    {

      /* n_rows can be > 0 here if the paths were first different and then
      converged to the same record on the leaf level.
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

      /* If the range is such that we should count both borders, then avoid
      counting that record twice - once as a left border and once as a right
      border. Some of the borders should not be counted, e.g. [3,3). */
      n_rows= should_count_the_left_border && should_count_the_right_border;
    }
    else
      n_rows+= should_count_the_left_border + should_count_the_right_border;
  }

  if (root_height > divergence_height && !is_n_rows_exact)
    /* In trees whose height is > 1 our algorithm tends to underestimate:
    multiply the estimate by 2: */
    n_rows*= 2;

  DBUG_EXECUTE_IF("bug14007649", DBUG_RETURN(n_rows););

  /* Do not estimate the number of rows in the range to over 1 / 2 of the
  estimated rows in the whole table */

  if (n_rows > table_n_rows / 2 && !is_n_rows_exact)
  {

    n_rows= table_n_rows / 2;

    /* If there are just 0 or 1 rows in the table, then we estimate all rows
    are in the range */

    if (n_rows == 0)
      n_rows= table_n_rows;
  }

  DBUG_RETURN(n_rows);

error:
  mtr.commit();
  if (UNIV_LIKELY_NULL(heap))
    mem_heap_free(heap);

  DBUG_RETURN(0);
}

/*================== EXTERNAL STORAGE OF BIG FIELDS ===================*/

/***********************************************************//**
Gets the offset of the pointer to the externally stored part of a field.
@return offset of the pointer to the externally stored part */
static
ulint
btr_rec_get_field_ref_offs(
/*=======================*/
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
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
	const rec_offs*	offsets)
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
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
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
		mtr->write<1,mtr_t::MAYBE_NOP>(*block, data + local_len
					       + BTR_EXTERN_LEN, byte_val);
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
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
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
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
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
uint32_t
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
uint32_t
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
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  block->page.fix();
#ifdef UNIV_DEBUG
  const page_id_t page_id{block->page.id()};
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
#endif
  mtr->commit();

  mysql_mutex_lock(&buf_pool.mutex);
  block->page.unfix();
  ut_ad(block->page.id() == page_id);
  ut_ad(&block->page == buf_pool.page_hash.get(page_id, chain));

  if (!buf_LRU_free_page(&block->page, all) && all && block->page.zip.data)
    /* Attempt to deallocate the redundant copy of the uncompressed page
    if the whole ROW_FORMAT=COMPRESSED block cannot be deallocted. */
    buf_LRU_free_page(&block->page, false);

  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Helper class used while writing blob pages, during insert or update. */
struct btr_blob_log_check_t {
	/** Persistent cursor on a clusterex index record with blobs. */
	btr_pcur_t*	m_pcur;
	/** Mini transaction holding the latches for m_pcur */
	mtr_t*		m_mtr;
	/** rec_get_offsets(rec, index); offset of clust_rec */
	const rec_offs*	m_offsets;
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
		const rec_offs*	offsets,
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
		ut_ad((*m_block)->page.frame == page_align(*m_rec));
		ut_ad(*m_rec == btr_pcur_get_rec(m_pcur));
	}

	/** Check if there is enough space in log file. Commit and re-start the
	mini transaction. */
	void check()
	{
		dict_index_t*	index = m_pcur->index();
		ulint		offs = 0;
		uint32_t	page_no = FIL_NULL;

		if (UNIV_UNLIKELY(m_op == BTR_STORE_INSERT_BULK)) {
			offs = *m_rec - (*m_block)->page.frame;
			ut_ad(offs == page_offset(*m_rec));
			page_no = (*m_block)->page.id().page_no();
			(*m_block)->page.fix();
			ut_ad(page_no != FIL_NULL);
		} else {
			btr_pcur_store_position(m_pcur, m_mtr);
		}
		m_mtr->commit();

		DEBUG_SYNC_C("blob_write_middle");

		const mtr_log_t log_mode = m_mtr->get_log_mode();
		m_mtr->start();
		m_mtr->set_log_mode(log_mode);
		index->set_modified(*m_mtr);

		log_free_check();

		DEBUG_SYNC_C("blob_write_middle_after_check");

		if (UNIV_UNLIKELY(page_no != FIL_NULL)) {
			dberr_t err;
			if (UNIV_LIKELY(index->page != page_no)) {
				ut_a(btr_root_block_get(index, RW_SX_LATCH,
							m_mtr, &err));
			}
			m_pcur->btr_cur.page_cur.block = btr_block_get(
				*index, page_no, RW_X_LATCH, false, m_mtr);
			/* The page should not be evicted or corrupted while
			we are holding a buffer-fix on it. */
			m_pcur->btr_cur.page_cur.block->page.unfix();
			m_pcur->btr_cur.page_cur.rec
				= m_pcur->btr_cur.page_cur.block->page.frame
				+ offs;
		} else {
			ut_ad(m_pcur->rel_pos == BTR_PCUR_ON);
			mtr_sx_lock_index(index, m_mtr);
			ut_a(m_pcur->restore_position(
			      BTR_MODIFY_ROOT_AND_LEAF_ALREADY_LATCHED,
			      m_mtr) == btr_pcur_t::SAME_ALL);
		}

		*m_block	= btr_pcur_get_block(m_pcur);
		*m_rec		= btr_pcur_get_rec(m_pcur);

		rec_offs_make_valid(*m_rec, index, true,
				    const_cast<rec_offs*>(m_offsets));

		ut_ad(m_mtr->memo_contains_page_flagged(
		      *m_rec,
		      MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX));

		ut_ad((m_op == BTR_STORE_INSERT_BULK)
		      == !m_mtr->memo_contains_flagged(&index->lock,
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
	btr_pcur_t*	pcur,		/*!< in: a persistent cursor */
	rec_offs*	offsets,	/*!< in/out: rec_get_offsets() on
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
	byte*		field_ref;
	ulint		extern_len;
	ulint		store_len;
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
	      || btr_mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
						| MTR_MEMO_SX_LOCK));
	ut_ad(btr_mtr->memo_contains_flagged(rec_block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(buf_block_get_frame(rec_block) == page_align(rec));
	ut_a(dict_index_is_clust(index));

	if (!fil_page_index_page_check(btr_pcur_get_page(pcur))) {
		if (op != BTR_STORE_INSERT_BULK) {
			return DB_PAGE_CORRUPTED;
		}
	}

	btr_blob_log_check_t redo_log(pcur, btr_mtr, offsets, &rec_block,
				      &rec, op);
	page_zip = buf_block_get_page_zip(rec_block);

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
		MEM_CHECK_DEFINED(big_rec_vec->fields[i].data, extern_len);
		ut_a(extern_len > 0);

		uint32_t prev_page_no = FIL_NULL;

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

			ut_ad(page_align(field_ref) == page_align(rec));

			if (!(blob_npages % commit_freq)) {

				redo_log.check();

				field_ref = btr_rec_get_field_ref(
					rec, offsets, field_no);

				page_zip = buf_block_get_page_zip(rec_block);
			}

			ut_ad(btr_mtr->get_already_latched(
				      page_id_t{index->table->space_id, index->page},
				      MTR_MEMO_PAGE_SX_FIX));

			mtr.start();
			index->set_modified(mtr);
			mtr.set_log_mode_sub(*btr_mtr);

			rec_block->page.fix();
			rec_block->page.lock.x_lock();

			mtr.memo_push(rec_block, MTR_MEMO_PAGE_X_FIX);
#ifdef BTR_CUR_HASH_ADAPT
			ut_ad(!btr_search_check_marked_free_index(rec_block));
#endif

			uint32_t hint_prev = prev_page_no;
			if (hint_prev == FIL_NULL) {
				hint_prev = rec_block->page.id().page_no();
			}

			block = btr_page_alloc(index, hint_prev + 1,
					       FSP_NO_DIR, 0, &mtr, &mtr,
					       &error);

			if (!block) {
alloc_fail:
                                mtr.commit();
				goto func_exit;
			}

			const uint32_t space_id = block->page.id().space();
			const uint32_t page_no = block->page.id().page_no();

			if (prev_page_no == FIL_NULL) {
			} else if (buf_block_t* prev_block =
				   buf_page_get_gen(page_id_t(space_id,
							  prev_page_no),
                                                    rec_block->zip_size(),
                                                    RW_X_LATCH, nullptr,
                                                    BUF_GET, &mtr, &error)) {
				if (page_zip) {
					mtr.write<4>(*prev_block,
						     prev_block->page.frame
						     + FIL_PAGE_NEXT,
						     page_no);
					memcpy_aligned<4>(
						buf_block_get_page_zip(
							prev_block)
						->data + FIL_PAGE_NEXT,
						prev_block->page.frame
						+ FIL_PAGE_NEXT, 4);
				} else {
					mtr.write<4>(*prev_block,
						     BTR_BLOB_HDR_NEXT_PAGE_NO
						     + FIL_PAGE_DATA
						     + prev_block->page.frame,
						     page_no);
				}
			} else {
				goto alloc_fail;
			}

			ut_ad(!page_has_siblings(block->page.frame));
			ut_ad(!fil_page_get_type(block->page.frame));

			if (page_zip) {
				int		err;
				page_zip_des_t*	blob_page_zip;

				mtr.write<1>(*block,
					     FIL_PAGE_TYPE + 1
					     + block->page.frame,
					     prev_page_no == FIL_NULL
					     ? FIL_PAGE_TYPE_ZBLOB
					     : FIL_PAGE_TYPE_ZBLOB2);
				block->page.zip.data[FIL_PAGE_TYPE + 1]
					= block->page.frame[FIL_PAGE_TYPE + 1];

				c_stream.next_out = block->page.frame
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
				memcpy(blob_page_zip->data, block->page.frame,
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
					     + block->page.frame,
					     FIL_PAGE_TYPE_BLOB);

				if (extern_len > payload_size) {
					store_len = payload_size;
				} else {
					store_len = extern_len;
				}

				mtr.memcpy<mtr_t::MAYBE_NOP>(
					*block,
					FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE
					+ block->page.frame,
					static_cast<const byte*>
					(big_rec_vec->fields[i].data)
					+ big_rec_vec->fields[i].len
					- extern_len, store_len);
				mtr.write<4>(*block, BTR_BLOB_HDR_PART_LEN
					     + FIL_PAGE_DATA
					     + block->page.frame,
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
					mtr.write<4,mtr_t::MAYBE_NOP>(
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
@param block   uncompressed BLOB page
@param op      operation
@return whether the type is invalid */
static bool btr_check_blob_fil_page_type(const buf_block_t& block,
                                         const char *op)
{
  uint16_t type= fil_page_get_type(block.page.frame);

  if (UNIV_LIKELY(type == FIL_PAGE_TYPE_BLOB));
  else if (fil_space_t *space= fil_space_t::get(block.page.id().space()))
  {
    /* Old versions of InnoDB did not initialize FIL_PAGE_TYPE on BLOB
    pages.  Do not print anything about the type mismatch when reading
    a BLOB page that may be from old versions. */
    bool fail= space->full_crc32() || DICT_TF_HAS_ATOMIC_BLOBS(space->flags);
    if (fail)
      sql_print_error("InnoDB: FIL_PAGE_TYPE=%u on BLOB %s file %s page %u",
                      type, op, space->chain.start->name,
                      block.page.id().page_no());
    space->release();
    return fail;
  }
  return false;
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
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index),
					or NULL */
	buf_block_t*	block,		/*!< in/out: page of field_ref */
	ulint		i,		/*!< in: field number of field_ref;
					ignored if rec == NULL */
	bool		rollback,	/*!< in: performing rollback? */
	mtr_t*		local_mtr)	/*!< in: mtr
					containing the latch to data an an
					X-latch to the index tree */
{
	const uint32_t	space_id	= mach_read_from_4(
		field_ref + BTR_EXTERN_SPACE_ID);

	ut_ad(index->is_primary());
	ut_ad(block->page.lock.have_x());
	ut_ad(local_mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					       | MTR_MEMO_SX_LOCK));
	ut_ad(local_mtr->memo_contains_page_flagged(field_ref,
						    MTR_MEMO_PAGE_X_FIX));
	ut_ad(!rec || rec_offs_validate(rec, index, offsets));
	ut_ad(!rec || field_ref == btr_rec_get_field_ref(rec, offsets, i));
	ut_ad(index->table->space_id == index->table->space->id);
	ut_ad(local_mtr->is_named_space(index->table->space));

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
	ut_ad(space_id == index->table->space_id);

	const ulint ext_zip_size = index->table->space->zip_size();
	/* !rec holds in a call from purge when field_ref is in an undo page */
	ut_ad(rec || !block->page.zip.data);

	for (;;) {
		mtr_t mtr;

		mtr.start();
		mtr.set_spaces(*local_mtr);
		mtr.set_log_mode_sub(*local_mtr);

		ut_ad(!index->table->is_temporary()
		      || local_mtr->get_log_mode() == MTR_LOG_NO_REDO);

		const uint32_t page_no = mach_read_from_4(
			field_ref + BTR_EXTERN_PAGE_NO);
		buf_block_t* ext_block;

		if (/* There is no external storage data */
		    page_no == FIL_NULL
		    /* This field does not own the externally stored field */
		    || (mach_read_from_1(field_ref + BTR_EXTERN_LEN)
			& BTR_EXTERN_OWNER_FLAG)
		    /* Rollback and inherited field */
		    || (rollback
			&& (mach_read_from_1(field_ref + BTR_EXTERN_LEN)
			    & BTR_EXTERN_INHERITED_FLAG))) {
skip_free:
			/* Do not free */
			mtr.commit();

			return;
		}

		ext_block = buf_page_get(page_id_t(space_id, page_no),
					 ext_zip_size, RW_X_LATCH, &mtr);

		if (!ext_block) {
			goto skip_free;
		}

		/* The buffer pool block containing the BLOB pointer is
		exclusively latched by local_mtr. To satisfy some design
		constraints, we must recursively latch it in mtr as well. */
		block->fix();
		block->page.lock.x_lock();

		mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);
#ifdef BTR_CUR_HASH_ADAPT
		ut_ad(!btr_search_check_marked_free_index(block));
#endif

		const page_t* page = buf_block_get_frame(ext_block);

		if (ext_zip_size) {
			/* Note that page_zip will be NULL
			in row_purge_upd_exist_or_extern(). */
			switch (fil_page_get_type(page)) {
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			default:
				MY_ASSERT_UNREACHABLE();
			}
			const uint32_t next_page_no = mach_read_from_4(
				page + FIL_PAGE_NEXT);

			btr_page_free(index, ext_block, &mtr, true,
				      local_mtr->memo_contains(
					      *index->table->space));

			if (UNIV_LIKELY_NULL(block->page.zip.data)) {
				mach_write_to_4(field_ref + BTR_EXTERN_PAGE_NO,
						next_page_no);
				memset(field_ref + BTR_EXTERN_LEN + 4, 0, 4);
				page_zip_write_blob_ptr(block, rec, index,
							offsets, i, &mtr);
			} else {
				mtr.write<4>(*block,
					     BTR_EXTERN_PAGE_NO + field_ref,
					     next_page_no);
				mtr.write<4,mtr_t::MAYBE_NOP>(*block,
							      BTR_EXTERN_LEN
							      + 4 + field_ref,
							      0U);
			}
		} else {
			ut_ad(!block->page.zip.data);
			btr_check_blob_fil_page_type(*ext_block, "purge");

			const uint32_t next_page_no = mach_read_from_4(
				page + FIL_PAGE_DATA
				+ BTR_BLOB_HDR_NEXT_PAGE_NO);
			btr_page_free(index, ext_block, &mtr, true,
				      local_mtr->memo_contains(
					      *index->table->space));

			mtr.write<4>(*block, BTR_EXTERN_PAGE_NO + field_ref,
				     next_page_no);
			/* Zero out the BLOB length.  If the server
			crashes during the execution of this function,
			trx_rollback_all_recovered() could
			dereference the half-deleted BLOB, fetching a
			wrong prefix for the BLOB. */
			mtr.write<4,mtr_t::MAYBE_NOP>(*block,
						      BTR_EXTERN_LEN + 4
						      + field_ref, 0U);
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
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	buf_block_t*	block,	/*!< in: index page of rec */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the index
				tree */
{
	ulint	n_fields;
	ulint	i;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX));
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
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update,	/*!< in: update vector */
	bool		rollback,/*!< in: performing rollback? */
	mtr_t*		mtr)	/*!< in: mini-transaction handle which contains
				an X-latch to record page and to the tree */
{
	ulint	n_fields;
	ulint	i;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX));

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
	uint32_t	len,	/*!< in: length of buf, in bytes */
	page_id_t	id,	/*!< in: page identifier of the first BLOB page */
	uint32_t	offset)	/*!< in: offset on the first BLOB page */
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

		block = buf_page_get(id, 0, RW_S_LATCH, &mtr);
		if (!block || btr_check_blob_fil_page_type(*block, "read")) {
			mtr.commit();
			return copied_len;
		}
		if (!buf_page_make_young_if_needed(&block->page)) {
			buf_read_ahead_linear(id, false);
		}

		page = buf_block_get_frame(block);

		blob_header = page + offset;
		part_len = btr_blob_get_part_len(blob_header);
		copy_len = ut_min(part_len, len - copied_len);

		memcpy(buf + copied_len,
		       blob_header + BTR_BLOB_HDR_SIZE, copy_len);
		copied_len += copy_len;

		id.set_page_no(btr_blob_get_next_page_no(blob_header));

		mtr_commit(&mtr);

		if (id.page_no() == FIL_NULL || copy_len != part_len) {
			MEM_CHECK_DEFINED(buf, copied_len);
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
@param[in]	id		page identifier of the BLOB pages
@return number of bytes written to buf */
static
ulint
btr_copy_zblob_prefix(
	byte*			buf,
	uint32_t		len,
	ulint			zip_size,
	page_id_t		id,
	uint32_t		offset)
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
	ut_ad(id.space());

	err = inflateInit(&d_stream);
	ut_a(err == Z_OK);

	for (;;) {
		buf_page_t*	bpage;
		uint32_t	next_page_no;

		bpage = buf_page_get_zip(id);

		if (UNIV_UNLIKELY(!bpage)) {
			goto func_exit;
		}

		if (UNIV_UNLIKELY
		    (fil_page_get_type(bpage->zip.data) != page_type)) {

			ib::error() << "Unexpected type "
				<< fil_page_get_type(bpage->zip.data)
				<< " of compressed BLOB page " << id;

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
				<< id
				<< " returned " << err
				<< " (" << d_stream.msg << ")";

		case Z_BUF_ERROR:
			goto end_of_blob;
		}

		if (next_page_no == FIL_NULL) {
			if (!d_stream.avail_in) {
				ib::error()
					<< "Unexpected end of compressed "
					<< "BLOB page " << id;
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
			bpage->lock.s_unlock();
			goto func_exit;
		}

		bpage->lock.s_unlock();

		/* On other BLOB pages except the first
		the BLOB header always is at the page header: */

		id.set_page_no(next_page_no);
		offset = FIL_PAGE_NEXT;
		page_type = FIL_PAGE_TYPE_ZBLOB2;
	}

func_exit:
	inflateEnd(&d_stream);
	mem_heap_free(heap);
	MEM_CHECK_DEFINED(buf, d_stream.total_out);
	return(d_stream.total_out);
}

/** Copies the prefix of an externally stored field of a record.
The clustered index record that points to this BLOB must be protected
by a lock or a page latch.
@param[out]	buf		the externally stored part of the
field, or a prefix of it
@param[in]	len		length of buf, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	id		page identifier of the first BLOB page
@param[in]	offset		offset on the first BLOB page
@return number of bytes written to buf */
static
ulint
btr_copy_externally_stored_field_prefix_low(
	byte*			buf,
	uint32_t		len,
	ulint			zip_size,
	page_id_t		id,
	uint32_t		offset)
{
  if (len == 0)
    return 0;

  return zip_size
    ? btr_copy_zblob_prefix(buf, len, zip_size, id, offset)
    : btr_copy_blob_prefix(buf, len, id, offset);
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

	uint32_t space_id = mach_read_from_4(data + BTR_EXTERN_SPACE_ID);
	uint32_t page_no = mach_read_from_4(data + BTR_EXTERN_PAGE_NO);
	uint32_t offset = mach_read_from_4(data + BTR_EXTERN_OFFSET);
	len -= local_len;

	return(local_len
	       + btr_copy_externally_stored_field_prefix_low(buf + local_len,
							     uint32_t(len),
							     zip_size,
							     page_id_t(
								     space_id,
								     page_no),
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
	byte*	buf;

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;

	uint32_t space_id = mach_read_from_4(data + local_len
					     + BTR_EXTERN_SPACE_ID);
	uint32_t page_no = mach_read_from_4(data + local_len
					    + BTR_EXTERN_PAGE_NO);
	uint32_t offset = mach_read_from_4(data + local_len
					   + BTR_EXTERN_OFFSET);

	/* Currently a BLOB cannot be bigger than 4 GB; we
	leave the 4 upper bytes in the length field unused */

	uint32_t extern_len = mach_read_from_4(data + local_len
					       + BTR_EXTERN_LEN + 4);

	buf = (byte*) mem_heap_alloc(heap, local_len + extern_len);

	memcpy(buf, data, local_len);
	*len = local_len
		+ btr_copy_externally_stored_field_prefix_low(buf + local_len,
							      extern_len,
							      zip_size,
							      page_id_t(
								      space_id,
								      page_no),
							      offset);

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
	const rec_offs*		offsets,
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
		trx_rollback_recovered() or any
		TRX_ISO_READ_UNCOMMITTED transactions. */
		return(NULL);
	}

	return(btr_copy_externally_stored_field(len, data,
						zip_size, local_len, heap));
}
