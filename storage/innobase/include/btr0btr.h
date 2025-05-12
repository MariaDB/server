/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2014, 2023, MariaDB Corporation.

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
@file include/btr0btr.h
The B-tree

Created 6/2/1994 Heikki Tuuri
*******************************************************/

#pragma once

#include "dict0dict.h"
#include "data0data.h"
#include "rem0types.h"
#include "page0cur.h"
#include "btr0types.h"
#include "gis0type.h"

#define BTR_MAX_NODE_LEVEL	50	/*!< Maximum B-tree page level
					(not really a hard limit).
					Used in debug assertions
					in btr_page_set_level and
					btr_page_get_level */

/** Maximum record size which can be stored on a page, without using the
special big record storage structure */
#define	BTR_PAGE_MAX_REC_SIZE	(srv_page_size / 2 - 200)

/** @brief Maximum depth of a B-tree in InnoDB.

Note that this isn't a maximum as such; none of the tree operations
avoid producing trees bigger than this. It is instead a "max depth
that other code must work with", useful for e.g.  fixed-size arrays
that must store some information about each level in a tree. In other
words: if a B-tree with bigger depth than this is encountered, it is
not acceptable for it to lead to mysterious memory corruption, but it
is acceptable for the program to die with a clear assert failure. */
#define BTR_MAX_LEVELS		100

#define BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode)		\
	btr_latch_mode((latch_mode) & ~(BTR_RTREE_UNDO_INS	\
				| BTR_RTREE_DELETE_MARK		\
				| BTR_ALREADY_S_LATCHED		\
				| BTR_LATCH_FOR_INSERT		\
				| BTR_LATCH_FOR_DELETE))

#define BTR_LATCH_MODE_WITHOUT_INTENTION(latch_mode)			\
	btr_latch_mode((latch_mode)					\
		       & ~(BTR_LATCH_FOR_INSERT | BTR_LATCH_FOR_DELETE))

/**************************************************************//**
Checks and adjusts the root node of a tree during IMPORT TABLESPACE.
@return error code, or DB_SUCCESS */
dberr_t
btr_root_adjust_on_import(
/*======================*/
	const dict_index_t*	index)	/*!< in: index tree */
	MY_ATTRIBUTE((warn_unused_result));

/** Check a file segment header within a B-tree root page.
@param offset      file segment header offset
@param block       B-tree root page
@param space       tablespace
@return whether the segment header is valid */
bool btr_root_fseg_validate(ulint offset, const buf_block_t &block,
                            const fil_space_t &space);

/** Report a read failure if it is a decryption failure.
@param err   error code
@param index the index that is being accessed */
ATTRIBUTE_COLD void btr_read_failed(dberr_t err, const dict_index_t &index);

/** Get an index page and declare its latching order level.
@param  index         index tree
@param  page          page number
@param  latch_mode    latch mode
@param  mtr           mini-transaction
@param  err           error code
@param  first         set if this is a first-time access to the page
@return block */
buf_block_t *btr_block_get(const dict_index_t &index, uint32_t page,
                           rw_lock_type_t latch_mode, mtr_t *mtr,
                           dberr_t *err= nullptr, bool *first= nullptr
#if defined(UNIV_DEBUG) || !defined(DBUG_OFF)
                           , ulint page_get_mode= BUF_GET
                           /*!< BUF_GET or BUF_GET_POSSIBLY_FREED */
#endif /* defined(UNIV_DEBUG) || !defined(DBUG_OFF) */
                            );

/**************************************************************//**
Gets the index id field of a page.
@return index id */
UNIV_INLINE
index_id_t
btr_page_get_index_id(
/*==================*/
	const page_t*	page)	/*!< in: index page */
	MY_ATTRIBUTE((warn_unused_result));
/** Read the B-tree or R-tree PAGE_LEVEL.
@param page B-tree or R-tree page
@return number of child page links to reach the leaf level
@retval 0 for leaf pages */
inline uint16_t btr_page_get_level(const page_t *page)
{
  uint16_t level= mach_read_from_2(my_assume_aligned<2>
                                   (PAGE_HEADER + PAGE_LEVEL + page));
  ut_ad(level <= BTR_MAX_NODE_LEVEL);
  return level;
} MY_ATTRIBUTE((warn_unused_result))

/** Read FIL_PAGE_NEXT.
@param page  buffer pool page
@return previous page number */
inline uint32_t btr_page_get_next(const page_t* page)
{
  return mach_read_from_4(my_assume_aligned<4>(page + FIL_PAGE_NEXT));
}

/** Read FIL_PAGE_PREV.
@param page  buffer pool page
@return previous page number */
inline uint32_t btr_page_get_prev(const page_t* page)
{
  return mach_read_from_4(my_assume_aligned<4>(page + FIL_PAGE_PREV));
}

/**************************************************************//**
Gets the child node file address in a node pointer.
NOTE: the offsets array must contain all offsets for the record since
we read the last field according to offsets and assume that it contains
the child page number. In other words offsets must have been retrieved
with rec_get_offsets(n_fields=ULINT_UNDEFINED).
@return child node address */
UNIV_INLINE
uint32_t
btr_node_ptr_get_child_page_no(
/*===========================*/
	const rec_t*	rec,	/*!< in: node pointer record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
	MY_ATTRIBUTE((warn_unused_result));

/** Create the root node for a new index tree.
@param[in]	type			type of the index
@param[in,out]	space			tablespace where created
@param[in]	index_id		index id
@param[in]	index			index, or NULL to create a system table
@param[in,out]	mtr			mini-transaction
@param[out]	err			error code
@return	page number of the created root
@retval	FIL_NULL	if did not succeed */
uint32_t
btr_create(
	ulint			type,
	fil_space_t*		space,
	index_id_t		index_id,
	dict_index_t*		index,
	mtr_t*			mtr,
	dberr_t*		err)
	MY_ATTRIBUTE((nonnull(2,5,6), warn_unused_result));

/** Free a persistent index tree if it exists.
@param[in,out]	space		tablespce
@param[in]	page		root page number
@param[in]	index_id	PAGE_INDEX_ID contents
@param[in,out]	mtr		mini-transaction */
void btr_free_if_exists(fil_space_t *space, uint32_t page,
                        index_id_t index_id, mtr_t *mtr);

/** Drop a temporary table
@param table   temporary table */
void btr_drop_temporary_table(const dict_table_t &table);

/** Read the last used AUTO_INCREMENT value from PAGE_ROOT_AUTO_INC.
@param[in,out]	index	clustered index
@return	the last used AUTO_INCREMENT value
@retval	0 on error or if no AUTO_INCREMENT value was used yet */
ib_uint64_t
btr_read_autoinc(dict_index_t* index)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Read the last used AUTO_INCREMENT value from PAGE_ROOT_AUTO_INC,
or fall back to MAX(auto_increment_column).
@param table          table containing an AUTO_INCREMENT column
@param col_no         index of the AUTO_INCREMENT column
@param mysql_version  TABLE_SHARE::mysql_version
@param max            the maximum value of the AUTO_INCREMENT column
@return the AUTO_INCREMENT value
@retval 0 on error or if no AUTO_INCREMENT value was used yet */
uint64_t btr_read_autoinc_with_fallback(const dict_table_t *table,
                                        unsigned col_no, ulong mysql_version,
                                        uint64_t max)
  MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Write the next available AUTO_INCREMENT value to PAGE_ROOT_AUTO_INC.
@param[in,out]	index	clustered index
@param[in]	autoinc	the AUTO_INCREMENT value
@param[in]	reset	whether to reset the AUTO_INCREMENT
			to a possibly smaller value than currently
			exists in the page */
void
btr_write_autoinc(dict_index_t* index, ib_uint64_t autoinc, bool reset = false)
	MY_ATTRIBUTE((nonnull));

/** Write instant ALTER TABLE metadata to a root page.
@param[in,out]	root	clustered index root page
@param[in]	index	clustered index with instant ALTER TABLE
@param[in,out]	mtr	mini-transaction */
void btr_set_instant(buf_block_t* root, const dict_index_t& index, mtr_t* mtr);

ATTRIBUTE_COLD __attribute__((nonnull))
/** Reset the table to the canonical format on ROLLBACK of instant ALTER TABLE.
@param[in]      index   clustered index with instant ALTER TABLE
@param[in]      all     whether to reset FIL_PAGE_TYPE as well
@param[in,out]  mtr     mini-transaction */
void btr_reset_instant(const dict_index_t &index, bool all, mtr_t *mtr);

/*************************************************************//**
Makes tree one level higher by splitting the root, and inserts
the tuple. It is assumed that mtr contains an x-latch on the tree.
NOTE that the operation of this function must always succeed,
we cannot reverse it: therefore enough free disk space must be
guaranteed to be available before this function is called.
@return inserted record */
rec_t*
btr_root_raise_and_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor at which to insert: must be
				on the root page; when the function returns,
				the cursor is positioned on the predecessor
				of the inserted record */
	rec_offs**	offsets,/*!< out: offsets on inserted record */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap
				that can be emptied, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err)	/*!< out: error code */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Reorganize an index page.
@param cursor  page cursor
@param mtr     mini-transaction
@return error code
@retval DB_FAIL if reorganizing a ROW_FORMAT=COMPRESSED page failed */
dberr_t btr_page_reorganize(page_cur_t *cursor, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Decide if the page should be split at the convergence point of inserts
converging to the left.
@param cursor	insert position
@return the first record to be moved to the right half page
@retval	nullptr if no split is recommended */
rec_t *btr_page_get_split_rec_to_left(const btr_cur_t *cursor) noexcept;
/** Decide if the page should be split at the convergence point of inserts
converging to the right.
@param cursor     insert position
@param split_rec  if split recommended, the first record on the right
half page, or nullptr if the to-be-inserted record should be first
@return whether split is recommended */
bool
btr_page_get_split_rec_to_right(const btr_cur_t *cursor, rec_t **split_rec)
  noexcept;

/*************************************************************//**
Splits an index page to halves and inserts the tuple. It is assumed
that mtr holds an x-latch to the index tree. NOTE: the tree x-latch is
released within this function! NOTE that the operation of this
function must always succeed, we cannot reverse it: therefore enough
free disk space (2 pages) must be guaranteed to be available before
this function is called.

@return inserted record */
rec_t*
btr_page_split_and_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	rec_offs**	offsets,/*!< out: offsets on inserted record */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap
				that can be emptied, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err)	/*!< out: error code */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*******************************************************//**
Inserts a data tuple to a tree on a non-leaf level. It is assumed
that mtr holds an x-latch on the tree. */
dberr_t
btr_insert_on_non_leaf_level(
	ulint		flags,	/*!< in: undo logging and locking flags */
	dict_index_t*	index,	/*!< in: index */
	ulint		level,	/*!< in: level, must be > 0 */
	dtuple_t*	tuple,	/*!< in: the record to be inserted */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Set a child page pointer record as the predefined minimum record.
@tparam has_prev  whether the page is supposed to have a left sibling
@param[in,out]  rec     leftmost record on a leftmost non-leaf page
@param[in,out]  block   buffer pool block
@param[in,out]  mtr     mini-transaction */
template<bool has_prev= false>
inline void btr_set_min_rec_mark(rec_t *rec, const buf_block_t &block,
                                 mtr_t *mtr)
{
  ut_ad(block.page.frame == page_align(rec));
  ut_ad(!page_is_leaf(block.page.frame));
  ut_ad(has_prev == page_has_prev(block.page.frame));

  rec-= page_is_comp(block.page.frame) ? REC_NEW_INFO_BITS : REC_OLD_INFO_BITS;

  if (block.page.zip.data)
    /* This flag is computed from other contents on a ROW_FORMAT=COMPRESSED
    page. We are not modifying the compressed page frame at all. */
    *rec|= REC_INFO_MIN_REC_FLAG;
  else
    mtr->write<1>(block, rec, *rec | REC_INFO_MIN_REC_FLAG);
}

/** Seek to the parent page of a B-tree page.
@param mtr      mini-transaction
@param cursor   cursor pointing to the x-latched parent page
@return whether the cursor was successfully positioned */
bool btr_page_get_father(mtr_t *mtr, btr_cur_t *cursor) noexcept
  MY_ATTRIBUTE((nonnull,warn_unused_result));
#ifdef UNIV_DEBUG
/************************************************************//**
Checks that the node pointer to a page is appropriate.
@return TRUE */
ibool
btr_check_node_ptr(
/*===============*/
	dict_index_t*	index,	/*!< in: index tree */
	buf_block_t*	block,	/*!< in: index page */
	que_thr_t*	thr,	/*!< in/out: query thread */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((warn_unused_result));
#endif /* UNIV_DEBUG */
/*************************************************************//**
Tries to merge the page first to the left immediate brother if such a
brother exists, and the node pointers to the current page and to the
brother reside on the same page. If the left brother does not satisfy these
conditions, looks at the right brother. If the page is the only one on that
level lifts the records of the page to the father page, thus reducing the
tree height. It is assumed that mtr holds an x-latch on the tree and on the
page. If cursor is on the leaf level, mtr must also hold x-latches to
the brothers, if they exist.
@return error code
@retval DB_FAIL if the tree could not be merged */
dberr_t
btr_compress(
/*=========*/
	btr_cur_t*	cursor,	/*!< in/out: cursor on the page to merge
				or lift; the page must not be empty:
				when deleting records, use btr_discard_page()
				if the page would become empty */
	bool		adjust,	/*!< in: whether the cursor position should be
				adjusted even when compression occurs */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*************************************************************//**
Discards a page from a B-tree. This is used to remove the last record from
a B-tree page: the whole page must be removed at the same time. This cannot
be used for the root page, which is allowed to be empty. */
dberr_t
btr_discard_page(
/*=============*/
	btr_cur_t*	cursor,	/*!< in: cursor on the page to discard: not on
				the root page */
	mtr_t*		mtr);	/*!< in: mtr */

/**************************************************************//**
Allocates a new file page to be used in an index tree. NOTE: we assume
that the caller has made the reservation for free extents!
@retval NULL if no page could be allocated */
buf_block_t*
btr_page_alloc(
/*===========*/
	dict_index_t*	index,		/*!< in: index tree */
	uint32_t	hint_page_no,	/*!< in: hint of a good page */
	byte		file_direction,	/*!< in: direction where a possible
					page split is made */
	ulint		level,		/*!< in: level where the page is placed
					in the tree */
	mtr_t*		mtr,		/*!< in/out: mini-transaction
					for the allocation */
	mtr_t*		init_mtr,	/*!< in/out: mini-transaction
					for x-latching and initializing
					the page */
	dberr_t*	err)		/*!< out: error code */
	MY_ATTRIBUTE((warn_unused_result));
/** Empty an index page (possibly the root page). @see btr_page_create().
@param[in,out]	block		page to be emptied
@param[in,out]	page_zip	compressed page frame, or NULL
@param[in]	index		index of the page
@param[in]	level		B-tree level of the page (0=leaf)
@param[in,out]	mtr		mini-transaction */
void
btr_page_empty(
	buf_block_t*	block,
	page_zip_des_t*	page_zip,
	dict_index_t*	index,
	ulint		level,
	mtr_t*		mtr)
	MY_ATTRIBUTE((nonnull(1, 3, 5)));
/**************************************************************//**
Creates a new index page (not the root, and also not
used in page reorganization).  @see btr_page_empty(). */
void
btr_page_create(
/*============*/
	buf_block_t*	block,	/*!< in/out: page to be created */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	dict_index_t*	index,	/*!< in: index */
	ulint		level,	/*!< in: the B-tree level of the page */
	mtr_t*		mtr);	/*!< in: mtr */

/** Free an index page.
@param[in,out]	index	index tree
@param[in,out]	block	block to be freed
@param[in,out]	mtr	mini-transaction
@param[in]	blob	whether this is freeing a BLOB page
@param[in]	latched	whether index->table->space->x_lock() was called */
MY_ATTRIBUTE((nonnull))
dberr_t btr_page_free(dict_index_t *index, buf_block_t *block, mtr_t *mtr,
                      bool blob= false, bool space_latched= false);

/**************************************************************//**
Gets the root node of a tree and x- or s-latches it.
@return root page, x- or s-latched */
buf_block_t*
btr_root_block_get(
/*===============*/
	dict_index_t*		index,	/*!< in: index tree */
	rw_lock_type_t		mode,	/*!< in: either RW_S_LATCH
					or RW_X_LATCH */
	mtr_t*			mtr,	/*!< in: mtr */
	dberr_t*		err);	/*!< out: error code */

/** Reorganize an index page.
@return error code
@retval DB_FAIL if reorganizing a ROW_FORMAT=COMPRESSED page failed */
dberr_t btr_page_reorganize_block(
	ulint		z_level,/*!< in: compression level to be used
				if dealing with compressed page */
	buf_block_t*	block,	/*!< in/out: B-tree page */
	dict_index_t*	index,	/*!< in: the index tree of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	__attribute__((nonnull, warn_unused_result));

#ifdef UNIV_BTR_PRINT
/*************************************************************//**
Prints size info of a B-tree. */
void
btr_print_size(
/*===========*/
	dict_index_t*	index)	/*!< in: index tree */
	MY_ATTRIBUTE((nonnull));
/**************************************************************//**
Prints directories and other info of all nodes in the index. */
void
btr_print_index(
/*============*/
	dict_index_t*	index,	/*!< in: index */
	ulint		width)	/*!< in: print this many entries from start
				and end */
	MY_ATTRIBUTE((nonnull));
#endif /* UNIV_BTR_PRINT */
/************************************************************//**
Checks the size and number of fields in a record based on the definition of
the index.
@return TRUE if ok */
bool
btr_index_rec_validate(
/*===================*/
	const page_cur_t&	cur,		/*!< in: index record */
	const dict_index_t*	index,		/*!< in: index */
	bool			dump_on_error)	/*!< in: true if the function
						should print hex dump of record
						and page on error */
	noexcept MY_ATTRIBUTE((warn_unused_result));
/**************************************************************//**
Checks the consistency of an index tree.
@return	DB_SUCCESS if ok, error code if not */
dberr_t
btr_validate_index(
/*===============*/
	dict_index_t*	index,	/*!< in: index */
	const trx_t*	trx)	/*!< in: transaction or 0 */
	MY_ATTRIBUTE((warn_unused_result));

/** Remove a page from the level list of pages.
@param[in]	block		page to remove
@param[in]	index		index tree
@param[in,out]	mtr		mini-transaction */
dberr_t btr_level_list_remove(const buf_block_t& block,
                              const dict_index_t& index, mtr_t* mtr)
  MY_ATTRIBUTE((warn_unused_result));

/*************************************************************//**
If page is the only on its level, this function moves its records to the
father page, thus reducing the tree height.
@return father block */
buf_block_t*
btr_lift_page_up(
	dict_index_t*	index,	/*!< in: index tree */
	buf_block_t*	block,	/*!< in: page which is the only on its level;
				must not be empty: use
				btr_discard_only_page_on_level if the last
				record from the page should be removed */
	que_thr_t*	thr,	/*!< in/out: query thread for SPATIAL INDEX */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	dberr_t*	err)	/*!< out: error code */
	__attribute__((nonnull(1,2,4,5)));

#define BTR_N_LEAF_PAGES	1
#define BTR_TOTAL_SIZE		2

#include "btr0btr.inl"

/****************************************************************
Global variable controlling if scrubbing should be performed */
extern my_bool srv_immediate_scrub_data_uncompressed;
