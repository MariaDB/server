/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2014, 2019, MariaDB Corporation.

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

#ifndef btr0btr_h
#define btr0btr_h

#include "dict0dict.h"
#include "data0data.h"
#include "rem0types.h"
#include "page0cur.h"
#include "btr0types.h"
#include "gis0type.h"

/** Maximum record size which can be stored on a page, without using the
special big record storage structure */
#define	BTR_PAGE_MAX_REC_SIZE	(UNIV_PAGE_SIZE / 2 - 200)

/** @brief Maximum depth of a B-tree in InnoDB.

Note that this isn't a maximum as such; none of the tree operations
avoid producing trees bigger than this. It is instead a "max depth
that other code must work with", useful for e.g.  fixed-size arrays
that must store some information about each level in a tree. In other
words: if a B-tree with bigger depth than this is encountered, it is
not acceptable for it to lead to mysterious memory corruption, but it
is acceptable for the program to die with a clear assert failure. */
#define BTR_MAX_LEVELS		100

/** Latching modes for btr_cur_search_to_nth_level(). */
enum btr_latch_mode {
	/** Search a record on a leaf page and S-latch it. */
	BTR_SEARCH_LEAF = RW_S_LATCH,
	/** (Prepare to) modify a record on a leaf page and X-latch it. */
	BTR_MODIFY_LEAF	= RW_X_LATCH,
	/** Obtain no latches. */
	BTR_NO_LATCHES = RW_NO_LATCH,
	/** Start modifying the entire B-tree. */
	BTR_MODIFY_TREE = 33,
	/** Continue modifying the entire B-tree. */
	BTR_CONT_MODIFY_TREE = 34,
	/** Search the previous record. */
	BTR_SEARCH_PREV = 35,
	/** Modify the previous record. */
	BTR_MODIFY_PREV = 36,
	/** Start searching the entire B-tree. */
	BTR_SEARCH_TREE = 37,
	/** Continue searching the entire B-tree. */
	BTR_CONT_SEARCH_TREE = 38,

	/* BTR_INSERT, BTR_DELETE and BTR_DELETE_MARK are mutually
	exclusive. */
	/** The search tuple will be inserted to the secondary index
	at the searched position.  When the leaf page is not in the
	buffer pool, try to use the change buffer. */
	BTR_INSERT = 512,

	/** Try to delete mark a secondary index leaf page record at
	the searched position using the change buffer when the page is
	not in the buffer pool. */
	BTR_DELETE_MARK	= 4096,

	/** Try to purge the record using the change buffer when the
	secondary index leaf page is not in the buffer pool. */
	BTR_DELETE = 8192,

	/** The caller is already holding dict_index_t::lock S-latch. */
	BTR_ALREADY_S_LATCHED = 16384,
	/** Search and S-latch a leaf page, assuming that the
	dict_index_t::lock S-latch is being held. */
	BTR_SEARCH_LEAF_ALREADY_S_LATCHED = BTR_SEARCH_LEAF
	| BTR_ALREADY_S_LATCHED,
	/** Search the entire index tree, assuming that the
	dict_index_t::lock S-latch is being held. */
	BTR_SEARCH_TREE_ALREADY_S_LATCHED = BTR_SEARCH_TREE
	| BTR_ALREADY_S_LATCHED,
	/** Search and X-latch a leaf page, assuming that the
	dict_index_t::lock S-latch is being held. */
	BTR_MODIFY_LEAF_ALREADY_S_LATCHED = BTR_MODIFY_LEAF
	| BTR_ALREADY_S_LATCHED,

	/** Attempt to delete-mark a secondary index record. */
	BTR_DELETE_MARK_LEAF = BTR_MODIFY_LEAF | BTR_DELETE_MARK,
	/** Attempt to delete-mark a secondary index record
	while holding the dict_index_t::lock S-latch. */
	BTR_DELETE_MARK_LEAF_ALREADY_S_LATCHED = BTR_DELETE_MARK_LEAF
	| BTR_ALREADY_S_LATCHED,
	/** Attempt to purge a secondary index record. */
	BTR_PURGE_LEAF = BTR_MODIFY_LEAF | BTR_DELETE,
	/** Attempt to purge a secondary index record
	while holding the dict_index_t::lock S-latch. */
	BTR_PURGE_LEAF_ALREADY_S_LATCHED = BTR_PURGE_LEAF
	| BTR_ALREADY_S_LATCHED,

	/** In the case of BTR_MODIFY_TREE, the caller specifies
	the intention to delete record only. It is used to optimize
	block->lock range.*/
	BTR_LATCH_FOR_DELETE = 65536,

	/** Attempt to purge a secondary index record in the tree. */
	BTR_PURGE_TREE = BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE
};

/** This flag ORed to btr_latch_mode says that we do the search in query
optimization */
#define BTR_ESTIMATE		1024U

/** This flag ORed to BTR_INSERT says that we can ignore possible
UNIQUE definition on secondary indexes when we decide if we can use
the insert buffer to speed up inserts */
#define BTR_IGNORE_SEC_UNIQUE	2048U

/** In the case of BTR_MODIFY_TREE, the caller specifies the intention
to insert record only. It is used to optimize block->lock range.*/
#define BTR_LATCH_FOR_INSERT	32768U

/** This flag is for undo insert of rtree. For rtree, we need this flag
to find proper rec to undo insert.*/
#define BTR_RTREE_UNDO_INS	131072U

/** In the case of BTR_MODIFY_LEAF, the caller intends to allocate or
free the pages of externally stored fields. */
#define BTR_MODIFY_EXTERNAL	262144U

/** Try to delete mark the record at the searched position when the
record is in spatial index */
#define BTR_RTREE_DELETE_MARK	524288U

#define BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode)			\
	((latch_mode) & btr_latch_mode(~(BTR_INSERT			\
					 | BTR_DELETE_MARK		\
					 | BTR_RTREE_UNDO_INS		\
					 | BTR_RTREE_DELETE_MARK	\
					 | BTR_DELETE			\
					 | BTR_ESTIMATE			\
					 | BTR_IGNORE_SEC_UNIQUE	\
					 | BTR_ALREADY_S_LATCHED	\
					 | BTR_LATCH_FOR_INSERT		\
					 | BTR_LATCH_FOR_DELETE		\
					 | BTR_MODIFY_EXTERNAL)))

#define BTR_LATCH_MODE_WITHOUT_INTENTION(latch_mode)			\
	((latch_mode) & btr_latch_mode(~(BTR_LATCH_FOR_INSERT		\
					 | BTR_LATCH_FOR_DELETE		\
					 | BTR_MODIFY_EXTERNAL)))

/**************************************************************//**
Report that an index page is corrupted. */
void
btr_corruption_report(
/*==================*/
	const buf_block_t*	block,	/*!< in: corrupted block */
	const dict_index_t*	index)	/*!< in: index tree */
	ATTRIBUTE_COLD __attribute__((nonnull));

/** Assert that a B-tree page is not corrupted.
@param block buffer block containing a B-tree page
@param index the B-tree index */
#define btr_assert_not_corrupted(block, index)			\
	if ((ibool) !!page_is_comp(buf_block_get_frame(block))	\
	    != dict_table_is_comp((index)->table)) {		\
		btr_corruption_report(block, index);		\
		ut_error;					\
	}

/**************************************************************//**
Gets the root node of a tree and sx-latches it for segment access.
@return root page, sx-latched */
page_t*
btr_root_get(
/*=========*/
	const dict_index_t*	index,	/*!< in: index tree */
	mtr_t*			mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));

/**************************************************************//**
Checks and adjusts the root node of a tree during IMPORT TABLESPACE.
@return error code, or DB_SUCCESS */
dberr_t
btr_root_adjust_on_import(
/*======================*/
	const dict_index_t*	index)	/*!< in: index tree */
	MY_ATTRIBUTE((warn_unused_result));

/**************************************************************//**
Gets the height of the B-tree (the level of the root, when the leaf
level is assumed to be 0). The caller must hold an S or X latch on
the index.
@return tree height (level of the root) */
ulint
btr_height_get(
/*===========*/
	const dict_index_t*	index,	/*!< in: index tree */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((warn_unused_result));

/** Gets a buffer page and declares its latching order level.
@param[in]	page_id	page id
@param[in]	mode	latch mode
@param[in]	file	file name
@param[in]	line	line where called
@param[in]	index	index tree, may be NULL if it is not an insert buffer
tree
@param[in,out]	mtr	mini-transaction
@return block */
UNIV_INLINE
buf_block_t*
btr_block_get_func(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			mode,
	const char*		file,
	unsigned		line,
	dict_index_t*		index,
	mtr_t*			mtr);

/** Gets a buffer page and declares its latching order level.
@param page_id tablespace/page identifier
@param page_size page size
@param mode latch mode
@param index index tree, may be NULL if not the insert buffer tree
@param mtr mini-transaction handle
@return the block descriptor */
# define btr_block_get(page_id, page_size, mode, index, mtr)	\
	btr_block_get_func(page_id, page_size, mode,		\
		__FILE__, __LINE__, (dict_index_t*)index, mtr)
/**************************************************************//**
Gets the index id field of a page.
@return index id */
UNIV_INLINE
index_id_t
btr_page_get_index_id(
/*==================*/
	const page_t*	page)	/*!< in: index page */
	MY_ATTRIBUTE((warn_unused_result));
/********************************************************//**
Gets the node level field in an index page.
@return level, leaf level == 0 */
UNIV_INLINE
ulint
btr_page_get_level_low(
/*===================*/
	const page_t*	page)	/*!< in: index page */
	MY_ATTRIBUTE((warn_unused_result));
#define btr_page_get_level(page, mtr) btr_page_get_level_low(page)

/** Read FIL_PAGE_NEXT.
@param page  buffer pool page
@return previous page number */
inline uint32_t btr_page_get_next(const page_t* page)
{
  return mach_read_from_4(page + FIL_PAGE_NEXT);
}

/** Read FIL_PAGE_PREV.
@param page  buffer pool page
@return previous page number */
inline uint32_t btr_page_get_prev(const page_t* page)
{
  return mach_read_from_4(page + FIL_PAGE_PREV);
}

/**************************************************************//**
Releases the latch on a leaf page and bufferunfixes it. */
UNIV_INLINE
void
btr_leaf_page_release(
/*==================*/
	buf_block_t*	block,		/*!< in: buffer block */
	ulint		latch_mode,	/*!< in: BTR_SEARCH_LEAF or
					BTR_MODIFY_LEAF */
	mtr_t*		mtr)		/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/**************************************************************//**
Gets the child node file address in a node pointer.
NOTE: the offsets array must contain all offsets for the record since
we read the last field according to offsets and assume that it contains
the child page number. In other words offsets must have been retrieved
with rec_get_offsets(n_fields=ULINT_UNDEFINED).
@return child node address */
UNIV_INLINE
ulint
btr_node_ptr_get_child_page_no(
/*===========================*/
	const rec_t*	rec,	/*!< in: node pointer record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
	MY_ATTRIBUTE((warn_unused_result));

/** Create the root node for a new index tree.
@param[in]	type			type of the index
@param[in]	space			space where created
@param[in]	page_size		page size
@param[in]	index_id		index id
@param[in]	index			index, or NULL when applying TRUNCATE
log record during recovery
@param[in]	btr_redo_create_info	used for applying TRUNCATE log
@param[in]	mtr			mini-transaction handle
record during recovery
@return page number of the created root, FIL_NULL if did not succeed */
ulint
btr_create(
	ulint			type,
	ulint			space,
	const page_size_t&	page_size,
	index_id_t		index_id,
	dict_index_t*		index,
	const btr_create_t*	btr_redo_create_info,
	mtr_t*			mtr);

/** Free a persistent index tree if it exists.
@param[in]	page_id		root page id
@param[in]	page_size	page size
@param[in]	index_id	PAGE_INDEX_ID contents
@param[in,out]	mtr		mini-transaction */
void
btr_free_if_exists(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	index_id_t		index_id,
	mtr_t*			mtr);

/** Free an index tree in a temporary tablespace or during TRUNCATE TABLE.
@param[in]	page_id		root page id
@param[in]	page_size	page size */
void
btr_free(
	const page_id_t		page_id,
	const page_size_t&	page_size);

/** Read the last used AUTO_INCREMENT value from PAGE_ROOT_AUTO_INC.
@param[in,out]	index	clustered index
@return	the last used AUTO_INCREMENT value
@retval	0 on error or if no AUTO_INCREMENT value was used yet */
ib_uint64_t
btr_read_autoinc(dict_index_t* index)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Read the last used AUTO_INCREMENT value from PAGE_ROOT_AUTO_INC,
or fall back to MAX(auto_increment_column).
@param[in]	table	table containing an AUTO_INCREMENT column
@param[in]	col_no	index of the AUTO_INCREMENT column
@return	the AUTO_INCREMENT value
@retval	0 on error or if no AUTO_INCREMENT value was used yet */
ib_uint64_t
btr_read_autoinc_with_fallback(const dict_table_t* table, unsigned col_no)
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
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((warn_unused_result));
/*************************************************************//**
Reorganizes an index page.

IMPORTANT: On success, the caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index. This has to
be done either within the same mini-transaction, or by invoking
ibuf_reset_free_bits() before mtr_commit(). On uncompressed pages,
IBUF_BITMAP_FREE is unaffected by reorganization.

@retval true if the operation was successful
@retval false if it is a compressed page, and recompression failed */
bool
btr_page_reorganize_low(
/*====================*/
	bool		recovery,/*!< in: true if called in recovery:
				locks should not be updated, i.e.,
				there cannot exist locks on the
				page, and a hash index should not be
				dropped: it cannot exist */
	ulint		z_level,/*!< in: compression level to be used
				if dealing with compressed page */
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	dict_index_t*	index,	/*!< in: the index tree of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((warn_unused_result));
/*************************************************************//**
Reorganizes an index page.

IMPORTANT: On success, the caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index. This has to
be done either within the same mini-transaction, or by invoking
ibuf_reset_free_bits() before mtr_commit(). On uncompressed pages,
IBUF_BITMAP_FREE is unaffected by reorganization.

@retval true if the operation was successful
@retval false if it is a compressed page, and recompression failed */
bool
btr_page_reorganize(
/*================*/
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	dict_index_t*	index,	/*!< in: the index tree of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));
/** Decide if the page should be split at the convergence point of inserts
converging to the left.
@param[in]	cursor	insert position
@return the first record to be moved to the right half page
@retval	NULL if no split is recommended */
rec_t* btr_page_get_split_rec_to_left(const btr_cur_t* cursor);
/** Decide if the page should be split at the convergence point of inserts
converging to the right.
@param[in]	cursor		insert position
@param[out]	split_rec	if split recommended, the first record
				on the right half page, or
				NULL if the to-be-inserted record
				should be first
@return whether split is recommended */
bool
btr_page_get_split_rec_to_right(const btr_cur_t* cursor, rec_t** split_rec);

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
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((warn_unused_result));
/*******************************************************//**
Inserts a data tuple to a tree on a non-leaf level. It is assumed
that mtr holds an x-latch on the tree. */
void
btr_insert_on_non_leaf_level_func(
/*==============================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	dict_index_t*	index,	/*!< in: index */
	ulint		level,	/*!< in: level, must be > 0 */
	dtuple_t*	tuple,	/*!< in: the record to be inserted */
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line where called */
	mtr_t*		mtr);	/*!< in: mtr */
#define btr_insert_on_non_leaf_level(f,i,l,t,m)			\
	btr_insert_on_non_leaf_level_func(f,i,l,t,__FILE__,__LINE__,m)

/** Sets a record as the predefined minimum record. */
void btr_set_min_rec_mark(rec_t* rec, mtr_t* mtr) MY_ATTRIBUTE((nonnull));

/** Seek to the parent page of a B-tree page.
@param[in,out]	index	b-tree
@param[in]	block	child page
@param[in,out]	mtr	mini-transaction
@param[out]	cursor	cursor pointing to the x-latched parent page */
void btr_page_get_father(dict_index_t* index, buf_block_t* block, mtr_t* mtr,
			 btr_cur_t* cursor)
	MY_ATTRIBUTE((nonnull));
#ifdef UNIV_DEBUG
/************************************************************//**
Checks that the node pointer to a page is appropriate.
@return TRUE */
ibool
btr_check_node_ptr(
/*===============*/
	dict_index_t*	index,	/*!< in: index tree */
	buf_block_t*	block,	/*!< in: index page */
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
@return TRUE on success */
ibool
btr_compress(
/*=========*/
	btr_cur_t*	cursor,	/*!< in/out: cursor on the page to merge
				or lift; the page must not be empty:
				when deleting records, use btr_discard_page()
				if the page would become empty */
	ibool		adjust,	/*!< in: TRUE if should adjust the
				cursor position even if compression occurs */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Discards a page from a B-tree. This is used to remove the last record from
a B-tree page: the whole page must be removed at the same time. This cannot
be used for the root page, which is allowed to be empty. */
void
btr_discard_page(
/*=============*/
	btr_cur_t*	cursor,	/*!< in: cursor on the page to discard: not on
				the root page */
	mtr_t*		mtr);	/*!< in: mtr */
/****************************************************************//**
Parses the redo log record for setting an index record as the predefined
minimum record.
@return end of log record or NULL */
byte*
btr_parse_set_min_rec_mark(
/*=======================*/
	byte*	ptr,	/*!< in: buffer */
	byte*	end_ptr,/*!< in: buffer end */
	ulint	comp,	/*!< in: nonzero=compact page format */
	page_t*	page,	/*!< in: page or NULL */
	mtr_t*	mtr)	/*!< in: mtr or NULL */
	MY_ATTRIBUTE((nonnull(1,2), warn_unused_result));
/***********************************************************//**
Parses a redo log record of reorganizing a page.
@return end of log record or NULL */
byte*
btr_parse_page_reorganize(
/*======================*/
	byte*		ptr,	/*!< in: buffer */
	byte*		end_ptr,/*!< in: buffer end */
	dict_index_t*	index,	/*!< in: record descriptor */
	bool		compressed,/*!< in: true if compressed page */
	buf_block_t*	block,	/*!< in: page to be reorganized, or NULL */
	mtr_t*		mtr)	/*!< in: mtr or NULL */
	MY_ATTRIBUTE((warn_unused_result));
/**************************************************************//**
Gets the number of pages in a B-tree.
@return number of pages, or ULINT_UNDEFINED if the index is unavailable */
ulint
btr_get_size(
/*=========*/
	const dict_index_t*	index,	/*!< in: index */
	ulint		flag,	/*!< in: BTR_N_LEAF_PAGES or BTR_TOTAL_SIZE */
	mtr_t*		mtr)	/*!< in/out: mini-transaction where index
				is s-latched */
	MY_ATTRIBUTE((warn_unused_result));
/**************************************************************//**
Gets the number of reserved and used pages in a B-tree.
@return	number of pages reserved, or ULINT_UNDEFINED if the index
is unavailable */
UNIV_INTERN
ulint
btr_get_size_and_reserved(
/*======================*/
	dict_index_t*	index,	/*!< in: index */
	ulint		flag,	/*!< in: BTR_N_LEAF_PAGES or BTR_TOTAL_SIZE */
	ulint*		used,	/*!< out: number of pages used (<= reserved) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction where index
				is s-latched */
	__attribute__((nonnull));

/**************************************************************//**
Allocates a new file page to be used in an index tree. NOTE: we assume
that the caller has made the reservation for free extents!
@retval NULL if no page could be allocated
@retval block, rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block (not allocated or initialized) otherwise */
buf_block_t*
btr_page_alloc(
/*===========*/
	dict_index_t*	index,		/*!< in: index tree */
	ulint		hint_page_no,	/*!< in: hint of a good page */
	byte		file_direction,	/*!< in: direction where a possible
					page split is made */
	ulint		level,		/*!< in: level where the page is placed
					in the tree */
	mtr_t*		mtr,		/*!< in/out: mini-transaction
					for the allocation */
	mtr_t*		init_mtr)	/*!< in/out: mini-transaction
					for x-latching and initializing
					the page */
	MY_ATTRIBUTE((warn_unused_result));
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
@param[in]	blob	whether this is freeing a BLOB page */
MY_ATTRIBUTE((nonnull))
void btr_page_free(dict_index_t* index, buf_block_t* block, mtr_t* mtr,
		   bool blob = false);

/**************************************************************//**
Gets the root node of a tree and x- or s-latches it.
@return root page, x- or s-latched */
buf_block_t*
btr_root_block_get(
/*===============*/
	const dict_index_t*	index,	/*!< in: index tree */
	ulint			mode,	/*!< in: either RW_S_LATCH
					or RW_X_LATCH */
	mtr_t*			mtr);	/*!< in: mtr */

/*************************************************************//**
Reorganizes an index page.

IMPORTANT: On success, the caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index. This has to
be done either within the same mini-transaction, or by invoking
ibuf_reset_free_bits() before mtr_commit(). On uncompressed pages,
IBUF_BITMAP_FREE is unaffected by reorganization.

@retval true if the operation was successful
@retval false if it is a compressed page, and recompression failed */
UNIV_INTERN
bool
btr_page_reorganize_block(
/*======================*/
	bool		recovery,/*!< in: true if called in recovery:
				locks should not be updated, i.e.,
				there cannot exist locks on the
				page, and a hash index should not be
				dropped: it cannot exist */
	ulint		z_level,/*!< in: compression level to be used
				if dealing with compressed page */
	buf_block_t*	block,	/*!< in/out: B-tree page */
	dict_index_t*	index,	/*!< in: the index tree of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	__attribute__((nonnull));

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
ibool
btr_index_rec_validate(
/*===================*/
	const rec_t*		rec,		/*!< in: index record */
	const dict_index_t*	index,		/*!< in: index */
	ibool			dump_on_error)	/*!< in: TRUE if the function
						should print hex dump of record
						and page on error */
	MY_ATTRIBUTE((warn_unused_result));
/**************************************************************//**
Checks the consistency of an index tree.
@return	DB_SUCCESS if ok, error code if not */
dberr_t
btr_validate_index(
/*===============*/
	dict_index_t*	index,	/*!< in: index */
	const trx_t*	trx,	/*!< in: transaction or 0 */
	bool		lockout)/*!< in: true if X-latch index is intended */
	MY_ATTRIBUTE((warn_unused_result));

/*************************************************************//**
Removes a page from the level list of pages. */
UNIV_INTERN
MY_ATTRIBUTE((warn_unused_result)) dberr_t
btr_level_list_remove_func(
/*=======================*/
	ulint			space,	/*!< in: space where removed */
	const page_size_t&	page_size,/*!< in: page size */
	page_t*			page,	/*!< in/out: page to remove */
	dict_index_t*		index,	/*!< in: index tree */
	mtr_t*			mtr);	/*!< in/out: mini-transaction */
/*************************************************************//**
Removes a page from the level list of pages.
@param space	in: space where removed
@param zip_size	in: compressed page size in bytes, or 0 for uncompressed
@param page	in/out: page to remove
@param index	in: index tree
@param mtr	in/out: mini-transaction */
# define btr_level_list_remove(space,zip_size,page,index,mtr)		\
	btr_level_list_remove_func(space,zip_size,page,index,mtr)

/*************************************************************//**
If page is the only on its level, this function moves its records to the
father page, thus reducing the tree height.
@return father block */
UNIV_INTERN
buf_block_t*
btr_lift_page_up(
/*=============*/
	dict_index_t*	index,	/*!< in: index tree */
	buf_block_t*	block,	/*!< in: page which is the only on its level;
				must not be empty: use
				btr_discard_only_page_on_level if the last
				record from the page should be removed */
	mtr_t*		mtr)	/*!< in: mtr */
	__attribute__((nonnull));

#define BTR_N_LEAF_PAGES	1
#define BTR_TOTAL_SIZE		2

#include "btr0btr.inl"

/****************************************************************
Global variable controlling if scrubbing should be performed */
extern my_bool srv_immediate_scrub_data_uncompressed;

#endif
