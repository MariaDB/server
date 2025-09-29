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
@file btr/btr0btr.cc
The B-tree

Created 6/2/1994 Heikki Tuuri
*******************************************************/

#include "btr0btr.h"

#include "page0page.h"
#include "page0zip.h"
#include "gis0rtree.h"

#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "trx0trx.h"
#include "srv0mon.h"
#include "gis0geo.h"
#include "dict0boot.h"
#include "row0sel.h" /* row_search_max_autoinc() */
#include "log.h"

/**************************************************************//**
Checks if the page in the cursor can be merged with given page.
If necessary, re-organize the merge_page.
@return	true if possible to merge. */
static
bool
btr_can_merge_with_page(
/*====================*/
	btr_cur_t*	cursor,		/*!< in: cursor on the page to merge */
	uint32_t	page_no,	/*!< in: a sibling page */
	buf_block_t**	merge_block,	/*!< out: the merge block */
	mtr_t*		mtr);		/*!< in: mini-transaction */

/*
Latching strategy of the InnoDB B-tree
--------------------------------------

Node pointer page latches acquisition is protected by index->lock latch.

Before MariaDB 10.2.2, all node pointer pages were protected by index->lock
either in S (shared) or X (exclusive) mode and block->lock was not acquired on
node pointer pages.

After MariaDB 10.2.2, block->lock S-latch or X-latch is used to protect
node pointer pages and obtaiment of node pointer page latches is protected by
index->lock.

(0) Definition: B-tree level.

(0.1) The leaf pages of the B-tree are at level 0.

(0.2) The parent of a page at level L has level L+1. (The level of the
root page is equal to the tree height.)

(0.3) The B-tree lock (index->lock) is the parent of the root page and
has a level = tree height + 1.

Index->lock has 3 possible locking modes:

(1) S-latch:

(1.1) All latches for pages must be obtained in descending order of tree level.

(1.2) Before obtaining the first node pointer page latch at a given B-tree
level, parent latch must be held (at level +1 ).

(1.3) If a node pointer page is already latched at the same level
we can only obtain latch to its right sibling page latch at the same level.

(1.4) Release of the node pointer page latches must be done in
child-to-parent order. (Prevents deadlocks when obtained index->lock
in SX mode).

(1.4.1) Level L node pointer page latch can be released only when
no latches at children level i.e. level < L are hold.

(1.4.2) All latches from node pointer pages must be released so
that no latches are obtained between.

(1.5) [implied by (1.1), (1.2)] Root page latch must be first node pointer
latch obtained.

(2) SX-latch:

In this case rules (1.2) and (1.3) from S-latch case are relaxed and
merged into (2.2) and rule (1.4) is removed. Thus, latch acquisition
can be skipped at some tree levels and latches can be obtained in
a less restricted order.

(2.1) [identical to (1.1)]: All latches for pages must be obtained in descending
order of tree level.

(2.2) When a node pointer latch at level L is obtained,
the left sibling page latch in the same level or some ancestor
page latch (at level > L) must be hold.

(2.3) [implied by (2.1), (2.2)] The first node pointer page latch obtained can
be any node pointer page.

(3) X-latch:

Node pointer latches can be obtained in any order.

NOTE: New rules after MariaDB 10.2.2 does not affect the latching rules of leaf pages:

index->lock S-latch is needed in read for the node pointer traversal. When the leaf
level is reached, index-lock can be released (and with the MariaDB 10.2.2 changes, all
node pointer latches). Left to right index travelsal in leaf page level can be safely done
by obtaining right sibling leaf page latch and then releasing the old page latch.

Single leaf page modifications (BTR_MODIFY_LEAF) are protected by index->lock
S-latch.

B-tree operations involving page splits or merges (BTR_MODIFY_TREE) and page
allocations are protected by index->lock X-latch.

Node pointers
-------------
Leaf pages of a B-tree contain the index records stored in the
tree. On levels n > 0 we store 'node pointers' to pages on level
n - 1. For each page there is exactly one node pointer stored:
thus the our tree is an ordinary B-tree, not a B-link tree.

A node pointer contains a prefix P of an index record. The prefix
is long enough so that it determines an index record uniquely.
The file page number of the child page is added as the last
field. To the child page we can store node pointers or index records
which are >= P in the alphabetical order, but < P1 if there is
a next node pointer on the level, and P1 is its prefix.

If a node pointer with a prefix P points to a non-leaf child,
then the leftmost record in the child must have the same
prefix P. If it points to a leaf node, the child is not required
to contain any record with a prefix equal to P. The leaf case
is decided this way to allow arbitrary deletions in a leaf node
without touching upper levels of the tree.

We have predefined a special minimum record which we
define as the smallest record in any alphabetical order.
A minimum record is denoted by setting a bit in the record
header. A minimum record acts as the prefix of a node pointer
which points to a leftmost node on any level of the tree.

File page allocation
--------------------
In the root node of a B-tree there are two file segment headers.
The leaf pages of a tree are allocated from one file segment, to
make them consecutive on disk if possible. From the other file segment
we allocate pages for the non-leaf levels of the tree.
*/

/** Check a file segment header within a B-tree root page.
@param offset      file segment header offset
@param block       B-tree root page
@param space       tablespace
@return whether the segment header is valid */
bool btr_root_fseg_validate(ulint offset, const buf_block_t &block,
                            const fil_space_t &space)
{
  ut_ad(block.page.id().space() == space.id);
  const uint16_t hdr= mach_read_from_2(offset + FSEG_HDR_OFFSET +
                                       block.page.frame);
  if (FIL_PAGE_DATA <= hdr && hdr <= srv_page_size - FIL_PAGE_DATA_END &&
      mach_read_from_4(block.page.frame + offset + FSEG_HDR_SPACE) == space.id)
    return true;
  sql_print_error("InnoDB: Index root page " UINT32PF " in %s is corrupted "
                  "at " ULINTPF,
                  block.page.id().page_no(),
                  UT_LIST_GET_FIRST(space.chain)->name, offset);
  return false;
}

/** Report a read failure if it is a decryption failure.
@param err   error code
@param index the index that is being accessed */
ATTRIBUTE_COLD void btr_read_failed(dberr_t err, const dict_index_t &index)
{
  if (err == DB_DECRYPTION_FAILED)
    innodb_decryption_failed(nullptr, index.table);
}

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
                           dberr_t *err, bool *first
#if defined(UNIV_DEBUG) || !defined(DBUG_OFF)
                           , ulint page_get_mode
                           /*!< BUF_GET or BUF_GET_POSSIBLY_FREED */
#endif /* defined(UNIV_DEBUG) || !defined(DBUG_OFF) */
                            )
{
  ut_ad(latch_mode != RW_NO_LATCH);
  dberr_t local_err;
  if (!err)
    err= &local_err;
  buf_block_t *block=
      buf_page_get_gen(page_id_t{index.table->space->id, page},
                       index.table->space->zip_size(), latch_mode, nullptr,
#if defined(UNIV_DEBUG) || !defined(DBUG_OFF)
                       page_get_mode,
#else
                       BUF_GET,
#endif /* defined(UNIV_DEBUG) || !defined(DBUG_OFF) */
                       mtr, err);
  ut_ad(!block == (*err != DB_SUCCESS));

  if (UNIV_LIKELY(block != nullptr))
  {
    btr_search_drop_page_hash_index(block, &index);
    if (!!page_is_comp(block->page.frame) != index.table->not_redundant() ||
        btr_page_get_index_id(block->page.frame) != index.id ||
        !fil_page_index_page_check(block->page.frame) ||
        index.is_spatial() !=
        (fil_page_get_type(block->page.frame) == FIL_PAGE_RTREE))
    {
      *err= DB_PAGE_CORRUPTED;
      block= nullptr;
    }
    else if (!buf_page_make_young_if_needed(&block->page) && first)
      *first= true;
  }
  else
    btr_read_failed(*err, index);

  return block;
}

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
	dberr_t*		err)	/*!< out: error code */
{
  ut_ad(mode != RW_NO_LATCH);

  if (!index->table || !index->table->space)
  {
    *err= DB_TABLESPACE_NOT_FOUND;
    return nullptr;
  }

  buf_block_t *block;
#ifndef BTR_CUR_ADAPT
  static constexpr buf_block_t *guess= nullptr;
#else
  buf_block_t *&guess= index->search_info.root_guess;
  guess=
#endif
  block=
    buf_page_get_gen({index->table->space->id, index->page},
                     index->table->space->zip_size(), mode, guess, BUF_GET,
                     mtr, err);
  ut_ad(!block == (*err != DB_SUCCESS));

  if (UNIV_LIKELY(block != nullptr))
  {
    btr_search_drop_page_hash_index(block, index);
    if (!!page_is_comp(block->page.frame) !=
        index->table->not_redundant() ||
        btr_page_get_index_id(block->page.frame) != index->id ||
        !fil_page_index_page_check(block->page.frame) ||
        index->is_spatial() !=
        (fil_page_get_type(block->page.frame) == FIL_PAGE_RTREE))
    {
      *err= DB_PAGE_CORRUPTED;
      block= nullptr;
    }
    else if (!btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF,
                                     *block, *index->table->space) ||
             !btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_TOP,
                                     *block, *index->table->space))
    {
      *err= DB_CORRUPTION;
      block= nullptr;
    }
    else
      buf_page_make_young_if_needed(&block->page);
  }
  else
    btr_read_failed(*err, *index);

  return block;
}

/**************************************************************//**
Gets the root node of a tree and sx-latches it for segment access.
@return root page, sx-latched */
static
page_t*
btr_root_get(
/*=========*/
	dict_index_t*		index,	/*!< in: index tree */
	mtr_t*			mtr,	/*!< in: mtr */
	dberr_t*		err)	/*!< out: error code */
{
  /* Intended to be used for accessing file segment lists.
  Concurrent read of other data is allowed. */
  if (buf_block_t *root= btr_root_block_get(index, RW_SX_LATCH, mtr, err))
    return root->page.frame;
  return nullptr;
}

/**************************************************************//**
Checks a file segment header within a B-tree root page and updates
the segment header space id.
@return TRUE if valid */
static
bool
btr_root_fseg_adjust_on_import(
/*===========================*/
	fseg_header_t*	seg_header,	/*!< in/out: segment header */
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page,
					or NULL */
	ulint		space)		/*!< in: tablespace identifier */
{
	ulint	offset = mach_read_from_2(seg_header + FSEG_HDR_OFFSET);

	if (offset < FIL_PAGE_DATA
	    || offset > srv_page_size - FIL_PAGE_DATA_END) {
		return false;
	}

	seg_header += FSEG_HDR_SPACE;

	mach_write_to_4(seg_header, space);
	if (UNIV_LIKELY_NULL(page_zip)) {
		memcpy(page_zip->data + page_offset(seg_header), seg_header,
		       4);
	}

	return true;
}

/**************************************************************//**
Checks and adjusts the root node of a tree during IMPORT TABLESPACE.
@return error code, or DB_SUCCESS */
dberr_t
btr_root_adjust_on_import(
/*======================*/
	const dict_index_t*	index)	/*!< in: index tree */
{
	dberr_t			err;
	mtr_t			mtr;
	page_t*			page;
	page_zip_des_t*		page_zip;
	dict_table_t*		table = index->table;

	DBUG_EXECUTE_IF("ib_import_trigger_corruption_3",
			return(DB_CORRUPTION););

	mtr_start(&mtr);

	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	buf_block_t* block = buf_page_get_gen(
		page_id_t(table->space->id, index->page),
		table->space->zip_size(), RW_X_LATCH, NULL, BUF_GET,
		&mtr, &err);
	if (!block) {
		ut_ad(err != DB_SUCCESS);
		goto func_exit;
	}

	btr_search_drop_page_hash_index(block, index);
	page = buf_block_get_frame(block);
	page_zip = buf_block_get_page_zip(block);

	if (!fil_page_index_page_check(page) || page_has_siblings(page)) {
		err = DB_CORRUPTION;

	} else if (dict_index_is_clust(index)) {
		bool	page_is_compact_format;

		page_is_compact_format = page_is_comp(page) > 0;

		/* Check if the page format and table format agree. */
		if (page_is_compact_format != dict_table_is_comp(table)) {
			err = DB_CORRUPTION;
		} else {
			/* Check that the table flags and the tablespace
			flags match. */
			uint32_t tf = dict_tf_to_fsp_flags(table->flags);
			uint32_t sf = table->space->flags;
			sf &= ~FSP_FLAGS_MEM_MASK;
			tf &= ~FSP_FLAGS_MEM_MASK;
			if (fil_space_t::is_flags_equal(tf, sf)
			    || fil_space_t::is_flags_equal(sf, tf)) {
				mysql_mutex_lock(&fil_system.mutex);
				table->space->flags = (table->space->flags
						       & ~FSP_FLAGS_MEM_MASK)
					| (tf & FSP_FLAGS_MEM_MASK);
				mysql_mutex_unlock(&fil_system.mutex);
				err = DB_SUCCESS;
			} else {
				err = DB_CORRUPTION;
			}
		}
	} else {
		err = DB_SUCCESS;
	}

	/* Check and adjust the file segment headers, if all OK so far. */
	if (err == DB_SUCCESS
	    && (!btr_root_fseg_adjust_on_import(
			FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF
			+ page, page_zip, table->space_id)
		|| !btr_root_fseg_adjust_on_import(
			FIL_PAGE_DATA + PAGE_BTR_SEG_TOP
			+ page, page_zip, table->space_id))) {

		err = DB_CORRUPTION;
	}

func_exit:
	mtr_commit(&mtr);

	return(err);
}

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
	mtr_t*		mtr)	/*!< in: mtr */
{
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  byte *index_id= my_assume_aligned<2>(PAGE_HEADER + PAGE_INDEX_ID +
                                       block->page.frame);

  if (UNIV_LIKELY_NULL(page_zip))
  {
    mach_write_to_8(index_id, index->id);
    page_create_zip(block, index, level, 0, mtr);
  }
  else
  {
    page_create(block, mtr, dict_table_is_comp(index->table));
    if (index->is_spatial())
    {
      static_assert(((FIL_PAGE_INDEX & 0xff00) | byte(FIL_PAGE_RTREE)) ==
                    FIL_PAGE_RTREE, "compatibility");
      mtr->write<1>(*block, FIL_PAGE_TYPE + 1 + block->page.frame,
                    byte(FIL_PAGE_RTREE));
      if (mach_read_from_8(block->page.frame + FIL_RTREE_SPLIT_SEQ_NUM))
        mtr->memset(block, FIL_RTREE_SPLIT_SEQ_NUM, 8, 0);
    }
    /* Set the level of the new index page */
    mtr->write<2,mtr_t::MAYBE_NOP>(*block,
                                   my_assume_aligned<2>(PAGE_HEADER +
                                                        PAGE_LEVEL +
                                                        block->page.frame),
				   level);
    mtr->write<8,mtr_t::MAYBE_NOP>(*block, index_id, index->id);
  }
}

buf_block_t *
mtr_t::get_already_latched(const page_id_t id, mtr_memo_type_t type) const
{
  ut_ad(is_active());
  ut_ad(type == MTR_MEMO_PAGE_X_FIX || type == MTR_MEMO_PAGE_SX_FIX ||
        type == MTR_MEMO_PAGE_S_FIX);
  for (ulint i= 0; i < m_memo.size(); i++)
  {
    const mtr_memo_slot_t &slot= m_memo[i];
    const auto slot_type= mtr_memo_type_t(slot.type & ~MTR_MEMO_MODIFY);
    if (slot_type == MTR_MEMO_PAGE_X_FIX || slot_type == type)
    {
      buf_block_t *block= static_cast<buf_block_t*>(slot.object);
      if (block->page.id() == id)
        return block;
    }
  }
  return nullptr;
}

/** Fetch an index root page that was already latched in the
mini-transaction. */
static buf_block_t *btr_get_latched_root(const dict_index_t &index, mtr_t *mtr)
{
  return mtr->get_already_latched(page_id_t{index.table->space_id, index.page},
                                  MTR_MEMO_PAGE_SX_FIX);
}

/** Fetch an index page that should have been already latched in the
mini-transaction. */
static buf_block_t *
btr_block_reget(mtr_t *mtr, const dict_index_t &index,
                const page_id_t id, dberr_t *err)
{
  if (buf_block_t *block= mtr->get_already_latched(id, MTR_MEMO_PAGE_X_FIX))
  {
    *err= DB_SUCCESS;
    return block;
  }

  ut_ad(mtr->memo_contains_flagged(&index.lock, MTR_MEMO_X_LOCK));
  return btr_block_get(index, id.page_no(), RW_X_LATCH, mtr, err);
}

static MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Acquire a latch on the index root page for allocating or freeing pages.
@param index   index tree
@param mtr     mini-transaction
@param err     error code
@return root page
@retval nullptr if an error occurred */
buf_block_t *btr_root_block_sx(dict_index_t *index, mtr_t *mtr, dberr_t *err)
{
  buf_block_t *root=
    mtr->get_already_latched(page_id_t{index->table->space_id, index->page},
                             MTR_MEMO_PAGE_SX_FIX);
  if (!root)
  {
    root= btr_root_block_get(index, RW_SX_LATCH, mtr, err);
    if (UNIV_UNLIKELY(!root))
      return root;
  }
#ifdef BTR_CUR_HASH_ADAPT
  ut_d(else if (dict_index_t *index= root->index))
    ut_ad(!index->freed());
#endif
  return root;
}

/**************************************************************//**
Allocates a new file page to be used in an index tree. NOTE: we assume
that the caller has made the reservation for free extents!
@retval NULL if no page could be allocated */
MY_ATTRIBUTE((nonnull, warn_unused_result))
buf_block_t*
btr_page_alloc(
	dict_index_t*	index,		/*!< in: index */
	uint32_t	hint_page_no,	/*!< in: hint of a good page */
	byte		file_direction,	/*!< in: direction where a possible
					page split is made */
	ulint		level,		/*!< in: level where the page is placed
					in the tree */
	mtr_t*		mtr,		/*!< in/out: mini-transaction
					for the allocation */
	mtr_t*		init_mtr,	/*!< in/out: mtr or another
					mini-transaction in which the
					page should be initialized. */
	dberr_t*	err)		/*!< out: error code */
{
  ut_ad(level < BTR_MAX_NODE_LEVEL);

  buf_block_t *root= btr_root_block_sx(index, mtr, err);
  if (UNIV_UNLIKELY(!root))
    return root;
  fseg_header_t *seg_header= root->page.frame +
    (level ? PAGE_HEADER + PAGE_BTR_SEG_TOP : PAGE_HEADER + PAGE_BTR_SEG_LEAF);
  return fseg_alloc_free_page_general(seg_header, hint_page_no, file_direction,
                                      true, mtr, init_mtr, err);
}

/** Free an index page.
@param[in,out]	index	index tree
@param[in,out]	block	block to be freed
@param[in,out]	mtr	mini-transaction
@param[in]	blob	whether this is freeing a BLOB page
@param[in]	latched	whether index->table->space->x_lock() was called
@return error code */
dberr_t btr_page_free(dict_index_t* index, buf_block_t* block, mtr_t* mtr,
                      bool blob, bool space_latched)
{
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
#if defined BTR_CUR_HASH_ADAPT && defined UNIV_DEBUG
  if (btr_search_check_marked_free_index(block))
  {
    ut_ad(!blob);
    ut_ad(page_is_leaf(block->page.frame));
  }
#endif
  const uint32_t page{block->page.id().page_no()};
  ut_ad(index->table->space_id == block->page.id().space());
  /* The root page is freed by btr_free_root(). */
  ut_ad(page != index->page);
  ut_ad(mtr->is_named_space(index->table->space));

  /* The page gets invalid for optimistic searches: increment the frame
  modify clock */
  buf_block_modify_clock_inc(block);

  /* TODO: Discard any operations for block from mtr->m_log.
  The page will be freed, so previous changes to it by this
  mini-transaction should not matter. */

  fil_space_t *space= index->table->space;
  dberr_t err;

  if (buf_block_t *root= btr_root_block_sx(index, mtr, &err))
  {
    err= fseg_free_page(&root->page.frame[blob ||
                                          page_is_leaf(block->page.frame)
                                          ? PAGE_HEADER + PAGE_BTR_SEG_LEAF
                                          : PAGE_HEADER + PAGE_BTR_SEG_TOP],
                        space, page, mtr, space_latched);
    if (err == DB_SUCCESS)
      buf_page_free(space, page, mtr);
  }

  /* The page was marked free in the allocation bitmap, but it
  should remain exclusively latched until mtr_t::commit() or until it
  is explicitly freed from the mini-transaction. */
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  return err;
}

/** Set the child page number in a node pointer record.
@param[in,out]  block   non-leaf index page
@param[in,out]  rec     node pointer record in the page
@param[in]      offsets rec_get_offsets(rec)
@param[in]      page_no child page number
@param[in,out]  mtr     mini-transaction
Sets the child node file address in a node pointer. */
inline void btr_node_ptr_set_child_page_no(buf_block_t *block,
                                           rec_t *rec, const rec_offs *offsets,
                                           ulint page_no, mtr_t *mtr)
{
  ut_ad(rec_offs_validate(rec, NULL, offsets));
  ut_ad(!page_rec_is_leaf(rec));
  ut_ad(!rec_offs_comp(offsets) || rec_get_node_ptr_flag(rec));

  const ulint offs= rec_offs_data_size(offsets);
  ut_ad(rec_offs_nth_size(offsets, rec_offs_n_fields(offsets) - 1) ==
        REC_NODE_PTR_SIZE);

  if (UNIV_LIKELY_NULL(block->page.zip.data))
    page_zip_write_node_ptr(block, rec, offs, page_no, mtr);
  else
    mtr->write<4>(*block, rec + offs - REC_NODE_PTR_SIZE, page_no);
}

MY_ATTRIBUTE((nonnull(1,2,3,4),warn_unused_result))
/************************************************************//**
Returns the child page of a node pointer and sx-latches it.
@return child page, sx-latched */
static
buf_block_t*
btr_node_ptr_get_child(
/*===================*/
	const rec_t*	node_ptr,/*!< in: node pointer */
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err = nullptr)	/*!< out: error code */
{
	ut_ad(rec_offs_validate(node_ptr, index, offsets));
	ut_ad(index->table->space_id
	      == page_get_space_id(page_align(node_ptr)));

	return btr_block_get(
		*index, btr_node_ptr_get_child_page_no(node_ptr, offsets),
		RW_SX_LATCH, mtr, err);
}

MY_ATTRIBUTE((nonnull(2,3,4), warn_unused_result))
/************************************************************//**
Returns the upper level node pointer to a page. It is assumed that mtr holds
an sx-latch on the tree.
@return rec_get_offsets() of the node pointer record */
static
rec_offs*
btr_page_get_father_node_ptr_for_validate(
	rec_offs*	offsets,/*!< in: work area for the return value */
	mem_heap_t*	heap,	/*!< in: memory heap to use */
	btr_cur_t*	cursor,	/*!< in: cursor pointing to user record,
				out: cursor on node pointer record,
				its page x-latched */
	mtr_t*		mtr)	/*!< in: mtr */
{
	const uint32_t page_no = btr_cur_get_block(cursor)->page.id().page_no();
	dict_index_t* index = btr_cur_get_index(cursor);
	ut_ad(!dict_index_is_spatial(index));
	ut_ad(mtr->memo_contains(index->lock, MTR_MEMO_X_LOCK));
	ut_ad(dict_index_get_page(index) != page_no);

	const auto level = btr_page_get_level(btr_cur_get_page(cursor));

	const rec_t* user_rec = btr_cur_get_rec(cursor);
	ut_a(page_rec_is_user_rec(user_rec));

	if (btr_cur_search_to_nth_level(level + 1,
					dict_index_build_node_ptr(index,
								  user_rec, 0,
								  heap, level),
					RW_S_LATCH,
					cursor, mtr) != DB_SUCCESS) {
		return nullptr;
	}

	const rec_t* node_ptr = btr_cur_get_rec(cursor);

	offsets = rec_get_offsets(node_ptr, index, offsets, 0,
				  ULINT_UNDEFINED, &heap);

	if (btr_node_ptr_get_child_page_no(node_ptr, offsets) != page_no) {
		offsets = nullptr;
	}

	return(offsets);
}

MY_ATTRIBUTE((nonnull(2,3,4), warn_unused_result))
/** Return the node pointer to a page.
@param offsets   work area for the return value
@param heap      memory heap
@param cursor    in: child page; out: node pointer to it
@param mtr       mini-transaction
@return rec_get_offsets() of the node pointer record
@retval nullptr  if the parent page had not been latched in mtr */
static rec_offs *btr_page_get_parent(rec_offs *offsets, mem_heap_t *heap,
                                     btr_cur_t *cursor, mtr_t *mtr)
{
  const uint32_t page_no= cursor->block()->page.id().page_no();
  const dict_index_t *index= cursor->index();
  ut_ad(!index->is_spatial());
  ut_ad(index->page != page_no);

  uint32_t p= index->page;
  auto level= btr_page_get_level(cursor->block()->page.frame);
  const dtuple_t *tuple=
    dict_index_build_node_ptr(index, btr_cur_get_rec(cursor), 0, heap, level);
  level++;

  ulint i;
  for (i= 0; i < mtr->get_savepoint(); i++)
    if (buf_block_t *block= mtr->block_at_savepoint(i))
      if (block->page.id().page_no() == p)
      {
        ut_ad(block->page.lock.have_u_or_x() ||
              (!block->page.lock.have_s() && index->lock.have_x()));
        uint16_t up_match= 0, low_match= 0;
        cursor->page_cur.block= block;
        if (page_cur_search_with_match(tuple, PAGE_CUR_LE, &up_match,
                                       &low_match, &cursor->page_cur,
                                       nullptr))
          return nullptr;
        offsets= rec_get_offsets(cursor->page_cur.rec, index, offsets, 0,
                                 ULINT_UNDEFINED, &heap);
        p= btr_node_ptr_get_child_page_no(cursor->page_cur.rec, offsets);
        if (p != page_no)
        {
          if (btr_page_get_level(block->page.frame) == level)
            return nullptr;
          i= 0; // MDEV-29835 FIXME: require all pages to be latched in order!
          continue;
        }
        ut_ad(block->page.lock.have_u_or_x());
        if (block->page.lock.have_u_not_x())
        {
          /* btr_cur_t::search_leaf(BTR_MODIFY_TREE) only U-latches the
          root page initially. */
          ut_ad(block->page.id().page_no() == index->page);
          block->page.lock.u_x_upgrade();
          mtr->page_lock_upgrade(*block);
        }
        return offsets;
      }

  return nullptr;
}

/************************************************************//**
Returns the upper level node pointer to a page. It is assumed that mtr holds
an x-latch on the tree.
@return rec_get_offsets() of the node pointer record
@retval nullptr on corruption */
static
rec_offs*
btr_page_get_father_block(
/*======================*/
	rec_offs*	offsets,/*!< in: work area for the return value */
	mem_heap_t*	heap,	/*!< in: memory heap to use */
	mtr_t*		mtr,	/*!< in: mtr */
	btr_cur_t*	cursor)	/*!< out: cursor on node pointer record,
				its page x-latched */
  noexcept
{
  const page_t *page= btr_cur_get_page(cursor);
  const rec_t *rec= page_is_comp(page)
    ? page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM)
    : page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);
  if (UNIV_UNLIKELY(!rec))
    return nullptr;
  cursor->page_cur.rec= const_cast<rec_t*>(rec);
  return btr_page_get_parent(offsets, heap, cursor, mtr);
}

/** Seek to the parent page of a B-tree page.
@param mtr      mini-transaction
@param cursor   cursor pointing to the x-latched parent page
@return whether the cursor was successfully positioned */
bool btr_page_get_father(mtr_t *mtr, btr_cur_t *cursor) noexcept
{
  page_t *page= btr_cur_get_page(cursor);
  const rec_t *rec= page_is_comp(page)
    ? page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM)
    : page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);
  if (UNIV_UNLIKELY(!rec))
    return false;
  cursor->page_cur.rec= const_cast<rec_t*>(rec);
  mem_heap_t *heap= mem_heap_create(100);
  const bool got= btr_page_get_parent(nullptr, heap, cursor, mtr);
  mem_heap_free(heap);
  return got;
}

#ifdef UNIV_DEBUG
/** PAGE_INDEX_ID value for freed index B-trees */
constexpr index_id_t	BTR_FREED_INDEX_ID = 0;
#endif

/** Free a B-tree root page. btr_free_but_not_root() must already
have been called.
@param block   index root page
@param space   tablespace
@param mtr     mini-transaction */
static void btr_free_root(buf_block_t *block, const fil_space_t &space,
                          mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->is_named_space(&space));

  btr_search_drop_page_hash_index(block, nullptr);

  if (btr_root_fseg_validate(PAGE_HEADER + PAGE_BTR_SEG_TOP, *block, space))
  {
    /* Free the entire segment in small steps. */
    ut_d(mtr->freeing_tree());
    while (!fseg_free_step(block, PAGE_HEADER + PAGE_BTR_SEG_TOP, mtr));
  }
}

MY_ATTRIBUTE((warn_unused_result))
/** Prepare to free a B-tree.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	index_id	PAGE_INDEX_ID contents
@param[in,out]	mtr		mini-transaction
@return root block, to invoke btr_free_but_not_root() and btr_free_root()
@retval NULL if the page is no longer a matching B-tree page */
static
buf_block_t *btr_free_root_check(const page_id_t page_id, ulint zip_size,
				 index_id_t index_id, mtr_t *mtr)
{
  ut_ad(page_id.space() != SRV_TMP_SPACE_ID);
  ut_ad(index_id != BTR_FREED_INDEX_ID);

  buf_block_t *block= buf_page_get_gen(page_id, zip_size, RW_X_LATCH,
                                       nullptr, BUF_GET_POSSIBLY_FREED, mtr);

  if (block)
  {
    btr_search_drop_page_hash_index(block,reinterpret_cast<dict_index_t*>(-1));
    if (fil_page_index_page_check(block->page.frame) &&
        index_id == btr_page_get_index_id(block->page.frame))
      /* This should be a root page. It should not be possible to
      reassign the same index_id for some other index in the
      tablespace. */
      ut_ad(!page_has_siblings(block->page.frame));
    else
      block= nullptr;
  }

  return block;
}

/** Initialize the root page of the b-tree
@param[in,out]  block           root block
@param[in]      index_id        index id
@param[in]      index           index of root page
@param[in,out]  mtr             mini-transaction */
static void btr_root_page_init(buf_block_t *block, index_id_t index_id,
                               dict_index_t *index, mtr_t *mtr)
{
  constexpr uint16_t field= PAGE_HEADER + PAGE_INDEX_ID;
  byte *page_index_id= my_assume_aligned<2>(field + block->page.frame);

  /* Create a new index page on the allocated segment page */
  if (UNIV_LIKELY_NULL(block->page.zip.data))
  {
    mach_write_to_8(page_index_id, index_id);
    ut_ad(!page_has_siblings(block->page.zip.data));
    page_create_zip(block, index, 0, 0, mtr);
  }
  else
  {
    page_create(block, mtr, index && index->table->not_redundant());
    if (index && index->is_spatial())
    {
      static_assert(((FIL_PAGE_INDEX & 0xff00) | byte(FIL_PAGE_RTREE)) ==
                    FIL_PAGE_RTREE, "compatibility");
      mtr->write<1>(*block, FIL_PAGE_TYPE + 1 + block->page.frame,
                    byte(FIL_PAGE_RTREE));
      if (mach_read_from_8(block->page.frame + FIL_RTREE_SPLIT_SEQ_NUM))
        mtr->memset(block, FIL_RTREE_SPLIT_SEQ_NUM, 8, 0);
    }
    /* Set the level of the new index page */
    mtr->write<2,mtr_t::MAYBE_NOP>(
        *block, PAGE_HEADER + PAGE_LEVEL + block->page.frame, 0U);
    mtr->write<8,mtr_t::MAYBE_NOP>(*block, page_index_id, index_id);
  }
}

/** Create the root node for a new index tree.
@param[in]	type			type of the index
@param[in]	index_id		index id
@param[in,out]	space			tablespace where created
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
{
	ut_ad(mtr->is_named_space(space));
	ut_ad(index_id != BTR_FREED_INDEX_ID);
	ut_ad(index || space == fil_system.sys_space);

	/* Create the two new segments for the index tree;
	the segment headers are put on the allocated root page */

	buf_block_t *block = fseg_create(space, PAGE_HEADER + PAGE_BTR_SEG_TOP,
					 mtr, err);

	if (!block) {
		return FIL_NULL;
	}

	if (!fseg_create(space, PAGE_HEADER + PAGE_BTR_SEG_LEAF, mtr,
			 err, false, block)) {
		/* Not enough space for new segment, free root
		segment before return. */
		btr_free_root(block, *space, mtr);
		return FIL_NULL;
	}

	ut_ad(!page_has_siblings(block->page.frame));

	btr_root_page_init(block, index_id, index, mtr);

	/* In the following assertion we test that two records of maximum
	allowed size fit on the root page: this fact is needed to ensure
	correctness of split algorithms */

	ut_ad(page_get_max_insert_size(block->page.frame, 2)
	      > 2 * BTR_PAGE_MAX_REC_SIZE);

	return(block->page.id().page_no());
}

/** Free a B-tree except the root page. The root page MUST be freed after
this by calling btr_free_root.
@param[in,out]	block		root page
@param[in]	log_mode	mtr logging mode */
static
void
btr_free_but_not_root(
	buf_block_t*	block,
	mtr_log_t	log_mode
#ifdef BTR_CUR_HASH_ADAPT
	,bool		ahi=false
#endif
	)
{
	mtr_t	mtr;

	ut_ad(fil_page_index_page_check(block->page.frame));
	ut_ad(!page_has_siblings(block->page.frame));
leaf_loop:
	mtr_start(&mtr);
	ut_d(mtr.freeing_tree());
	mtr_set_log_mode(&mtr, log_mode);
	fil_space_t *space = mtr.set_named_space_id(block->page.id().space());

	if (!btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF,
				    *block, *space)
	    || !btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_TOP,
				       *block, *space)) {
		mtr_commit(&mtr);
		return;
	}

	/* NOTE: page hash indexes are dropped when a page is freed inside
	fsp0fsp. */

	bool finished = fseg_free_step(block, PAGE_HEADER + PAGE_BTR_SEG_LEAF,
				       &mtr
#ifdef BTR_CUR_HASH_ADAPT
				       , ahi
#endif /* BTR_CUR_HASH_ADAPT */
				       );
	mtr_commit(&mtr);

	if (!finished) {

		goto leaf_loop;
	}
top_loop:
	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, log_mode);
	space = mtr.set_named_space_id(block->page.id().space());

	finished = !btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_TOP,
					   *block, *space)
		|| fseg_free_step_not_header(block,
					     PAGE_HEADER + PAGE_BTR_SEG_TOP,
					     &mtr
#ifdef BTR_CUR_HASH_ADAPT
					     ,ahi
#endif /* BTR_CUR_HASH_ADAPT */
					     );
	mtr_commit(&mtr);

	if (!finished) {
		goto top_loop;
	}
}

#ifdef BTR_CUR_HASH_ADAPT
TRANSACTIONAL_TARGET
#endif
/** Clear the index tree and reinitialize the root page, in the
rollback of TRX_UNDO_EMPTY. The BTR_SEG_LEAF is freed and reinitialized.
@param thr query thread
@return error code */
dberr_t dict_index_t::clear(que_thr_t *thr)
{
  mtr_t mtr;
  mtr.start();
  if (table->is_temporary())
    mtr.set_log_mode(MTR_LOG_NO_REDO);
  else
    set_modified(mtr);
  mtr_sx_lock_index(this, &mtr);

  dberr_t err;
  buf_block_t *root_block;
#ifndef BTR_CUR_ADAPT
  static constexpr buf_block_t *guess= nullptr;
#else
  buf_block_t *&guess= search_info.root_guess;
  guess=
#endif
  root_block= buf_page_get_gen({table->space_id, page},
                               table->space->zip_size(),
                               RW_X_LATCH, guess, BUF_GET, &mtr, &err);
  if (root_block)
  {
    btr_free_but_not_root(root_block, mtr.get_log_mode()
#ifdef BTR_CUR_HASH_ADAPT
		          ,any_ahi_pages()
#endif
                         );
    btr_search_drop_page_hash_index(root_block, nullptr);
#ifdef BTR_CUR_HASH_ADAPT
    ut_ad(!any_ahi_pages());
#endif
    mtr.memset(root_block, PAGE_HEADER + PAGE_BTR_SEG_LEAF,
               FSEG_HEADER_SIZE, 0);
    if (fseg_create(table->space, PAGE_HEADER + PAGE_BTR_SEG_LEAF, &mtr,
                    &err, false, root_block))
      btr_root_page_init(root_block, id, this, &mtr);
  }

  mtr.commit();
  return err;
}

/** Free a persistent index tree if it exists.
@param[in,out]	space		tablespce
@param[in]	page		root page number
@param[in]	index_id	PAGE_INDEX_ID contents
@param[in,out]	mtr		mini-transaction */
void btr_free_if_exists(fil_space_t *space, uint32_t page,
                        index_id_t index_id, mtr_t *mtr)
{
  if (buf_block_t *root= btr_free_root_check(page_id_t(space->id, page),
					     space->zip_size(),
					     index_id, mtr))
  {
    btr_free_but_not_root(root, mtr->get_log_mode());
    mtr->set_named_space(space);
    btr_free_root(root, *space, mtr);
  }
}

/** Drop a temporary table
@param table   temporary table */
void btr_drop_temporary_table(const dict_table_t &table)
{
  ut_ad(table.is_temporary());
  ut_ad(table.space == fil_system.temp_space);
  mtr_t mtr;
  mtr.start();
  for (const dict_index_t *index= table.indexes.start; index;
       index= dict_table_get_next_index(index))
  {
#ifndef BTR_CUR_ADAPT
    static constexpr buf_block_t *guess= nullptr;
#else
    buf_block_t *guess= index->search_info.root_guess;
#endif
    if (buf_block_t *block= buf_page_get_gen({SRV_TMP_SPACE_ID, index->page},
                                             0, RW_X_LATCH, guess, BUF_GET,
                                             &mtr, nullptr))
    {
      btr_free_but_not_root(block, MTR_LOG_NO_REDO);
      mtr.set_log_mode(MTR_LOG_NO_REDO);
      btr_free_root(block, *fil_system.temp_space, &mtr);
      mtr.commit();
      mtr.start();
    }
  }
  mtr.commit();
}

/** Read the last used AUTO_INCREMENT value from PAGE_ROOT_AUTO_INC.
@param[in,out]	index	clustered index
@return	the last used AUTO_INCREMENT value
@retval	0 on error or if no AUTO_INCREMENT value was used yet */
ib_uint64_t
btr_read_autoinc(dict_index_t* index)
{
  ut_ad(index->is_primary());
  ut_ad(index->table->persistent_autoinc);
  ut_ad(!index->table->is_temporary());
  mtr_t mtr;
  mtr.start();
  dberr_t err;
  uint64_t autoinc;
  if (buf_block_t *root= btr_root_block_get(index, RW_S_LATCH, &mtr, &err))
    autoinc= page_get_autoinc(root->page.frame);
  else
    autoinc= 0;
  mtr.commit();
  return autoinc;
}

dict_index_t *dict_table_t::get_index(const dict_col_t &col) const
{
  dict_index_t *index= dict_table_get_first_index(this);

  while (index && (index->fields[0].col != &col || index->is_corrupted()))
    index= dict_table_get_next_index(index);

  return index;
}

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
{
  ut_ad(table->persistent_autoinc);
  ut_ad(!table->is_temporary());

  uint64_t autoinc= 0;
  mtr_t mtr;
  mtr.start();
  const dict_index_t *const first_index= dict_table_get_first_index(table);

  if (buf_block_t *block=
      buf_page_get(page_id_t(table->space_id, first_index->page),
                   table->space->zip_size(), RW_SX_LATCH, &mtr))
  {
    btr_search_drop_page_hash_index(block, first_index);
    autoinc= page_get_autoinc(block->page.frame);

    if (autoinc > 0 && autoinc <= max && mysql_version >= 100210);
    else if (dict_index_t *index=
             table->get_index(*dict_table_get_nth_col(table, col_no)))
    {
      /* Read MAX(autoinc_col), in case this table had originally been
      created before MariaDB 10.2.4 introduced persistent AUTO_INCREMENT
      and MariaDB 10.2.10 fixed MDEV-12123, and there could be a garbage
      value in the PAGE_ROOT_AUTO_INC field. */
      const uint64_t max_autoinc= row_search_max_autoinc(index);
      const bool need_adjust{autoinc > max || autoinc < max_autoinc};
      ut_ad(max_autoinc <= max);

      if (UNIV_UNLIKELY(need_adjust) && !high_level_read_only && !opt_readonly)
      {
        sql_print_information("InnoDB: Resetting PAGE_ROOT_AUTO_INC from "
                              UINT64PF " to " UINT64PF
                              " on table %.*sQ.%sQ (created with version %lu)",
                              autoinc, max_autoinc,
                              int(table->name.dblen()), table->name.m_name,
                              table->name.basename(), mysql_version);
        autoinc= max_autoinc;
        index->set_modified(mtr);
        page_set_autoinc(block, max_autoinc, &mtr, true);
      }
    }
  }

  mtr.commit();
  return autoinc;
}

/** Write the next available AUTO_INCREMENT value to PAGE_ROOT_AUTO_INC.
@param[in,out]	index	clustered index
@param[in]	autoinc	the AUTO_INCREMENT value
@param[in]	reset	whether to reset the AUTO_INCREMENT
			to a possibly smaller value than currently
			exists in the page */
void
btr_write_autoinc(dict_index_t* index, ib_uint64_t autoinc, bool reset)
{
  ut_ad(index->is_primary());
  ut_ad(index->table->persistent_autoinc);
  ut_ad(!index->table->is_temporary());

  mtr_t mtr;
  mtr.start();
  fil_space_t *space= index->table->space;
  if (buf_block_t *root= buf_page_get(page_id_t(space->id, index->page),
				      space->zip_size(), RW_SX_LATCH, &mtr))
  {
#ifdef BTR_CUR_HASH_ADAPT
    ut_d(if (dict_index_t *ri= root->index)) ut_ad(ri == index);
#endif /* BTR_CUR_HASH_ADAPT */
    buf_page_make_young_if_needed(&root->page);
    mtr.set_named_space(space);
    page_set_autoinc(root, autoinc, &mtr, reset);
  }

  mtr.commit();
}

/** Reorganize an index page.
@param cursor      index page cursor
@param mtr         mini-transaction */
static dberr_t btr_page_reorganize_low(page_cur_t *cursor, mtr_t *mtr)
{
  buf_block_t *const block= cursor->block;

  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  ut_ad(!is_buf_block_get_page_zip(block));
  ut_ad(fil_page_index_page_check(block->page.frame));
  ut_ad(cursor->index->is_dummy ||
        block->page.id().space() == cursor->index->table->space->id);
  ut_ad(cursor->index->is_dummy ||
        block->page.id().page_no() != cursor->index->page ||
        !page_has_siblings(block->page.frame));

  /* Save the cursor position. */
  const ulint pos= page_rec_get_n_recs_before(cursor->rec);

  if (UNIV_UNLIKELY(pos == ULINT_UNDEFINED))
    return DB_CORRUPTION;

  btr_search_drop_page_hash_index(block, nullptr);

  buf_block_t *old= buf_block_alloc();
  /* Copy the old page to temporary space */
  memcpy_aligned<UNIV_PAGE_SIZE_MIN>(old->page.frame, block->page.frame,
                                     srv_page_size);

  const mtr_log_t log_mode= mtr->set_log_mode(MTR_LOG_NO_REDO);

  page_create(block, mtr, cursor->index->table->not_redundant());
  if (cursor->index->is_spatial())
    block->page.frame[FIL_PAGE_TYPE + 1]= byte(FIL_PAGE_RTREE);

  static_assert(((FIL_PAGE_INDEX & 0xff00) | byte(FIL_PAGE_RTREE)) ==
                FIL_PAGE_RTREE, "compatibility");

  /* Copy the records from the temporary space to the recreated page;
  do not copy the lock bits yet */

  dberr_t err=
    page_copy_rec_list_end_no_locks(block, old,
                                    page_get_infimum_rec(old->page.frame),
                                    cursor->index, mtr);
  mtr->set_log_mode(log_mode);

  if (UNIV_UNLIKELY(err != DB_SUCCESS))
    return err;

  /* Copy the PAGE_MAX_TRX_ID or PAGE_ROOT_AUTO_INC. */
  ut_ad(!page_get_max_trx_id(block->page.frame));
  memcpy_aligned<8>(PAGE_MAX_TRX_ID + PAGE_HEADER + block->page.frame,
                    PAGE_MAX_TRX_ID + PAGE_HEADER + old->page.frame, 8);
#ifdef UNIV_DEBUG
  if (page_get_max_trx_id(block->page.frame))
    /* PAGE_MAX_TRX_ID must be zero on non-leaf pages other than
    clustered index root pages. */
    ut_ad(!cursor->index->is_primary()
          ? page_is_leaf(block->page.frame)
          : block->page.id().page_no() == cursor->index->page);
  else
    /* PAGE_MAX_TRX_ID is unused in clustered index pages (other than
    the root where it is repurposed as PAGE_ROOT_AUTO_INC), non-leaf
    pages, and in temporary tables.  It was always zero-initialized in
    page_create().  PAGE_MAX_TRX_ID must be nonzero on secondary index
    leaf pages. */
    ut_ad(cursor->index->table->is_temporary() ||
          !page_is_leaf(block->page.frame) ||
          cursor->index->is_primary());
#endif

  const uint16_t data_size1= page_get_data_size(old->page.frame);
  const uint16_t data_size2= page_get_data_size(block->page.frame);
  const ulint max1=
    page_get_max_insert_size_after_reorganize(old->page.frame, 1);
  const ulint max2=
    page_get_max_insert_size_after_reorganize(block->page.frame, 1);

  if (UNIV_UNLIKELY(data_size1 != data_size2 || max1 != max2))
  {
    sql_print_error("InnoDB: Page old data size %u new data size %u"
                    ", page old max ins size %zu new max ins size %zu",
                    data_size1, data_size2, max1, max2);
    return DB_CORRUPTION;
  }

  /* Restore the cursor position. */
  if (!pos)
    ut_ad(cursor->rec == page_get_infimum_rec(block->page.frame));
  else if (!(cursor->rec= page_rec_get_nth(block->page.frame, pos)))
    return DB_CORRUPTION;

  if (block->page.id().page_no() != cursor->index->page ||
      fil_page_get_type(old->page.frame) != FIL_PAGE_TYPE_INSTANT)
    ut_ad(!memcmp(old->page.frame, block->page.frame, PAGE_HEADER));
  else if (!cursor->index->is_instant())
  {
    ut_ad(!memcmp(old->page.frame, block->page.frame, FIL_PAGE_TYPE));
    ut_ad(!memcmp(old->page.frame + FIL_PAGE_TYPE + 2,
                  block->page.frame + FIL_PAGE_TYPE + 2,
                  PAGE_HEADER - FIL_PAGE_TYPE - 2));
    mtr->write<2,mtr_t::FORCED>(*block, FIL_PAGE_TYPE + block->page.frame,
                                FIL_PAGE_INDEX);
  }
  else
  {
    /* Preserve the PAGE_INSTANT information. */
    memcpy_aligned<2>(FIL_PAGE_TYPE + block->page.frame,
                      FIL_PAGE_TYPE + old->page.frame, 2);
    memcpy_aligned<2>(PAGE_HEADER + PAGE_INSTANT + block->page.frame,
                      PAGE_HEADER + PAGE_INSTANT + old->page.frame, 2);
    if (!cursor->index->table->instant);
    else if (page_is_comp(block->page.frame))
    {
      memcpy(PAGE_NEW_INFIMUM + block->page.frame,
             PAGE_NEW_INFIMUM + old->page.frame, 8);
      memcpy(PAGE_NEW_SUPREMUM + block->page.frame,
             PAGE_NEW_SUPREMUM + old->page.frame, 8);
    }
    else
    {
      memcpy(PAGE_OLD_INFIMUM + block->page.frame,
             PAGE_OLD_INFIMUM + old->page.frame, 8);
      memcpy(PAGE_OLD_SUPREMUM + block->page.frame,
             PAGE_OLD_SUPREMUM + old->page.frame, 8);
    }

    ut_ad(!memcmp(old->page.frame, block->page.frame, PAGE_HEADER));
  }

  ut_ad(!memcmp(old->page.frame + PAGE_MAX_TRX_ID + PAGE_HEADER,
                block->page.frame + PAGE_MAX_TRX_ID + PAGE_HEADER,
                PAGE_DATA - (PAGE_MAX_TRX_ID + PAGE_HEADER)));

  if (!cursor->index->has_locking());
  else if (cursor->index->page == FIL_NULL)
    ut_ad(cursor->index->is_dummy);
  else
    lock_move_reorganize_page(block, old);

  /* Write log for the changes, if needed. */
  if (log_mode == MTR_LOG_ALL)
  {
    /* Check and log the changes in the page header. */
    ulint a, e;
    for (a= PAGE_HEADER, e= PAGE_MAX_TRX_ID + PAGE_HEADER; a < e; a++)
    {
      if (old->page.frame[a] == block->page.frame[a])
        continue;
      while (--e, old->page.frame[e] == block->page.frame[e]);
      e++;
      ut_ad(a < e);
      /* Write log for the changed page header fields. */
      mtr->memcpy(*block, a, e - a);
      break;
    }

    const uint16_t top= page_header_get_offs(block->page.frame, PAGE_HEAP_TOP);

    if (page_is_comp(block->page.frame))
    {
      /* info_bits=0, n_owned=1, heap_no=0, status */
      ut_ad(!memcmp(PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES +
                    block->page.frame,
                    PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES +
                    old->page.frame, 3));
      /* If the 'next' pointer of the infimum record has changed, log it. */
      a= PAGE_NEW_INFIMUM - 2;
      e= a + 2;
      if (block->page.frame[a] == old->page.frame[a])
        a++;
      if (--e, block->page.frame[e] != old->page.frame[e])
        e++;
      if (ulint len= e - a)
        mtr->memcpy(*block, a, len);
      /* The infimum record itself must not change. */
      ut_ad(!memcmp(PAGE_NEW_INFIMUM + block->page.frame,
                    PAGE_NEW_INFIMUM + old->page.frame, 8));
      /* Log any change of the n_owned of the supremum record. */
      a= PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES;
      if (block->page.frame[a] != old->page.frame[a])
        mtr->memcpy(*block, a, 1);
      /* The rest of the supremum record must not change. */
      ut_ad(!memcmp(&block->page.frame[a + 1], &old->page.frame[a + 1],
                    PAGE_NEW_SUPREMUM_END - PAGE_NEW_SUPREMUM +
                    REC_N_NEW_EXTRA_BYTES - 1));

      /* Log the differences in the payload. */
      for (a= PAGE_NEW_SUPREMUM_END, e= top; a < e; a++)
      {
        if (old->page.frame[a] == block->page.frame[a])
          continue;
        while (--e, old->page.frame[e] == block->page.frame[e]);
        e++;
        ut_ad(a < e);
        /* TODO: write MEMMOVE records to minimize this further! */
        mtr->memcpy(*block, a, e - a);
        break;
      }
    }
    else
    {
      /* info_bits=0, n_owned=1, heap_no=0, number of fields, 1-byte format */
      ut_ad(!memcmp(PAGE_OLD_INFIMUM - REC_N_OLD_EXTRA_BYTES +
                    block->page.frame,
                    PAGE_OLD_INFIMUM - REC_N_OLD_EXTRA_BYTES +
                    old->page.frame, 4));
      /* If the 'next' pointer of the infimum record has changed, log it. */
      a= PAGE_OLD_INFIMUM - 2;
      e= a + 2;
      if (block->page.frame[a] == old->page.frame[a])
        a++;
      if (--e, block->page.frame[e] != old->page.frame[e])
        e++;
      if (ulint len= e - a)
        mtr->memcpy(*block, a, len);
      /* The infimum record itself must not change. */
      ut_ad(!memcmp(PAGE_OLD_INFIMUM + block->page.frame,
                    PAGE_OLD_INFIMUM + old->page.frame, 8));
      /* Log any change of the n_owned of the supremum record. */
      a= PAGE_OLD_SUPREMUM - REC_N_OLD_EXTRA_BYTES;
      if (block->page.frame[a] != old->page.frame[a])
        mtr->memcpy(*block, a, 1);
      ut_ad(!memcmp(&block->page.frame[a + 1], &old->page.frame[a + 1],
                    PAGE_OLD_SUPREMUM_END - PAGE_OLD_SUPREMUM +
                    REC_N_OLD_EXTRA_BYTES - 1));

      /* Log the differences in the payload. */
      for (a= PAGE_OLD_SUPREMUM_END, e= top; a < e; a++)
      {
        if (old->page.frame[a] == block->page.frame[a])
          continue;
        while (--e, old->page.frame[e] == block->page.frame[e]);
        e++;
        ut_ad(a < e);
        /* TODO: write MEMMOVE records to minimize this further! */
        mtr->memcpy(*block, a, e - a);
        break;
      }
    }

    e= srv_page_size - PAGE_DIR;
    a= e - PAGE_DIR_SLOT_SIZE * page_dir_get_n_slots(block->page.frame);

    /* Zero out the payload area. */
    mtr->memset(*block, top, a - top, 0);

    /* Log changes to the page directory. */
    for (; a < e; a++)
    {
      if (old->page.frame[a] == block->page.frame[a])
        continue;
      while (--e, old->page.frame[e] == block->page.frame[e]);
      e++;
      ut_ad(a < e);
      /* Write log for the changed page directory slots. */
      mtr->memcpy(*block, a, e - a);
      break;
    }
  }

  buf_block_free(old);

  MONITOR_INC(MONITOR_INDEX_REORG_ATTEMPTS);
  MONITOR_INC(MONITOR_INDEX_REORG_SUCCESSFUL);
  return DB_SUCCESS;
}

/** Reorganize an index page.
@return error code
@retval DB_FAIL if reorganizing a ROW_FORMAT=COMPRESSED page failed */
dberr_t
btr_page_reorganize_block(
	ulint		z_level,/*!< in: compression level to be used
				if dealing with compressed page */
	buf_block_t*	block,	/*!< in/out: B-tree page */
	dict_index_t*	index,	/*!< in: the index tree of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  if (buf_block_get_page_zip(block))
    return page_zip_reorganize(block, index, z_level, mtr, true);
  page_cur_t cur;
  page_cur_set_before_first(block, &cur);
  cur.index= index;
  return btr_page_reorganize_low(&cur, mtr);
}

/** Reorganize an index page.
@param cursor  page cursor
@param mtr     mini-transaction
@return error code
@retval DB_FAIL if reorganizing a ROW_FORMAT=COMPRESSED page failed */
dberr_t btr_page_reorganize(page_cur_t *cursor, mtr_t *mtr)
{
  if (!buf_block_get_page_zip(cursor->block))
    return btr_page_reorganize_low(cursor, mtr);

  ulint pos= page_rec_get_n_recs_before(cursor->rec);
  if (UNIV_UNLIKELY(pos == ULINT_UNDEFINED))
    return DB_CORRUPTION;

  dberr_t err= page_zip_reorganize(cursor->block, cursor->index,
                                   page_zip_level, mtr, true);
  if (err == DB_FAIL);
  else if (!pos)
    ut_ad(cursor->rec == page_get_infimum_rec(cursor->block->page.frame));
  else if (!(cursor->rec= page_rec_get_nth(cursor->block->page.frame, pos)))
    err= DB_CORRUPTION;

  return err;
}

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
{
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_zip == buf_block_get_page_zip(block));
	ut_ad(!index->is_dummy);
	ut_ad(index->table->space->id == block->page.id().space());
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip
	     || page_zip_validate(page_zip, block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */

	btr_search_drop_page_hash_index(block, nullptr);

	/* Recreate the page: note that global data on page (possible
	segment headers, next page-field, etc.) is preserved intact */

	/* Preserve PAGE_ROOT_AUTO_INC when creating a clustered index
	root page. */
	const ib_uint64_t	autoinc
		= dict_index_is_clust(index)
		&& index->page == block->page.id().page_no()
		? page_get_autoinc(block->page.frame)
		: 0;

	if (page_zip) {
		page_create_zip(block, index, level, autoinc, mtr);
	} else {
		page_create(block, mtr, index->table->not_redundant());
		if (index->is_spatial()) {
			static_assert(((FIL_PAGE_INDEX & 0xff00)
				       | byte(FIL_PAGE_RTREE))
				      == FIL_PAGE_RTREE, "compatibility");
			mtr->write<1>(*block, FIL_PAGE_TYPE + 1
				      + block->page.frame,
				      byte(FIL_PAGE_RTREE));
			if (mach_read_from_8(block->page.frame
					     + FIL_RTREE_SPLIT_SEQ_NUM)) {
				mtr->memset(block, FIL_RTREE_SPLIT_SEQ_NUM,
					    8, 0);
			}
		}
		mtr->write<2,mtr_t::MAYBE_NOP>(*block, PAGE_HEADER + PAGE_LEVEL
					       + block->page.frame, level);
		if (autoinc) {
			mtr->write<8>(*block, PAGE_HEADER + PAGE_MAX_TRX_ID
				      + block->page.frame, autoinc);
		}
	}
}

/** Write instant ALTER TABLE metadata to a root page.
@param[in,out]	root	clustered index root page
@param[in]	index	clustered index with instant ALTER TABLE
@param[in,out]	mtr	mini-transaction */
void btr_set_instant(buf_block_t* root, const dict_index_t& index, mtr_t* mtr)
{
	ut_ad(index.n_core_fields > 0);
	ut_ad(index.n_core_fields < REC_MAX_N_FIELDS);
	ut_ad(index.is_instant());
	ut_ad(fil_page_get_type(root->page.frame) == FIL_PAGE_TYPE_INSTANT
	      || fil_page_get_type(root->page.frame) == FIL_PAGE_INDEX);
	ut_ad(!page_has_siblings(root->page.frame));
	ut_ad(root->page.id().page_no() == index.page);

	rec_t* infimum = page_get_infimum_rec(root->page.frame);
	rec_t* supremum = page_get_supremum_rec(root->page.frame);
	byte* page_type = root->page.frame + FIL_PAGE_TYPE;
	uint16_t i = page_header_get_field(root->page.frame, PAGE_INSTANT);

	switch (mach_read_from_2(page_type)) {
	case FIL_PAGE_TYPE_INSTANT:
		ut_ad(page_get_instant(root->page.frame)
		      == index.n_core_fields);
		if (memcmp(infimum, "infimum", 8)
		    || memcmp(supremum, "supremum", 8)) {
			ut_ad(index.table->instant);
			ut_ad(!memcmp(infimum, field_ref_zero, 8));
			ut_ad(!memcmp(supremum, field_ref_zero, 7));
			/* The n_core_null_bytes only matters for
			ROW_FORMAT=COMPACT and ROW_FORMAT=DYNAMIC tables. */
			ut_ad(supremum[7] == index.n_core_null_bytes
			      || !index.table->not_redundant());
			return;
		}
		break;
	default:
		ut_ad("wrong page type" == 0);
		/* fall through */
	case FIL_PAGE_INDEX:
		ut_ad(!page_is_comp(root->page.frame)
		      || !page_get_instant(root->page.frame));
		ut_ad(!memcmp(infimum, "infimum", 8));
		ut_ad(!memcmp(supremum, "supremum", 8));
		mtr->write<2>(*root, page_type, FIL_PAGE_TYPE_INSTANT);
		ut_ad(i <= PAGE_NO_DIRECTION);
		i |= static_cast<uint16_t>(index.n_core_fields << 3);
		mtr->write<2>(*root, PAGE_HEADER + PAGE_INSTANT
			      + root->page.frame, i);
		break;
	}

	if (index.table->instant) {
		mtr->memset(root, infimum - root->page.frame, 8, 0);
		mtr->memset(root, supremum - root->page.frame, 7, 0);
		mtr->write<1,mtr_t::MAYBE_NOP>(*root, &supremum[7],
					       index.n_core_null_bytes);
	}
}

/** Reset the table to the canonical format on ROLLBACK of instant ALTER TABLE.
@param[in]      index   clustered index with instant ALTER TABLE
@param[in]      all     whether to reset FIL_PAGE_TYPE as well
@param[in,out]  mtr     mini-transaction */
ATTRIBUTE_COLD
void btr_reset_instant(const dict_index_t &index, bool all, mtr_t *mtr)
{
  ut_ad(!index.table->is_temporary());
  ut_ad(index.is_primary());
  buf_block_t *root= btr_get_latched_root(index, mtr);
  byte *page_type= root->page.frame + FIL_PAGE_TYPE;
  if (all)
  {
    ut_ad(mach_read_from_2(page_type) == FIL_PAGE_TYPE_INSTANT ||
          mach_read_from_2(page_type) == FIL_PAGE_INDEX);
    mtr->write<2,mtr_t::MAYBE_NOP>(*root, page_type, FIL_PAGE_INDEX);
    byte *instant= PAGE_INSTANT + PAGE_HEADER + root->page.frame;
    mtr->write<2,mtr_t::MAYBE_NOP>(*root, instant,
                                   page_ptr_get_direction(instant + 1));
  }
  else
    ut_ad(mach_read_from_2(page_type) == FIL_PAGE_TYPE_INSTANT);
  static const byte supremuminfimum[8 + 8] = "supremuminfimum";
  uint16_t infimum, supremum;
  if (page_is_comp(root->page.frame))
  {
    infimum= PAGE_NEW_INFIMUM;
    supremum= PAGE_NEW_SUPREMUM;
  }
  else
  {
    infimum= PAGE_OLD_INFIMUM;
    supremum= PAGE_OLD_SUPREMUM;
  }
  ut_ad(!memcmp(&root->page.frame[infimum], supremuminfimum + 8, 8) ==
        !memcmp(&root->page.frame[supremum], supremuminfimum, 8));
  mtr->memcpy<mtr_t::MAYBE_NOP>(*root, &root->page.frame[infimum],
                                supremuminfimum + 8, 8);
  mtr->memcpy<mtr_t::MAYBE_NOP>(*root, &root->page.frame[supremum],
                                supremuminfimum, 8);
}

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
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err)	/*!< out: error code */
{
	dict_index_t*	index;
	dtuple_t*	node_ptr;
	ulint		level;
	rec_t*		node_ptr_rec;
	page_cur_t*	page_cursor;
	page_zip_des_t*	root_page_zip;
	page_zip_des_t*	new_page_zip;
	buf_block_t*	root;
	buf_block_t*	new_block;

	root = btr_cur_get_block(cursor);
	root_page_zip = buf_block_get_page_zip(root);
	ut_ad(!page_is_empty(root->page.frame));
	index = btr_cur_get_index(cursor);
	ut_ad(index->n_core_null_bytes <= UT_BITS_IN_BYTES(index->n_nullable));
	ut_ad(!index->is_spatial());
#ifdef UNIV_ZIP_DEBUG
	ut_a(!root_page_zip
	     || page_zip_validate(root_page_zip, root->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */
	const page_id_t root_id{root->page.id()};

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(root, MTR_MEMO_PAGE_X_FIX));

	if (index->page != root_id.page_no()) {
		ut_ad("corrupted root page number" == 0);
		return nullptr;
	}

	if (!btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF,
				    *root, *index->table->space)
	    || !btr_root_fseg_validate(FIL_PAGE_DATA + PAGE_BTR_SEG_TOP,
				       *root, *index->table->space)) {
		return nullptr;
	}

	/* Allocate a new page to the tree. Root splitting is done by first
	moving the root records to the new page, emptying the root, putting
	a node pointer to the new page, and then splitting the new page. */

	level = btr_page_get_level(root->page.frame);

	new_block = btr_page_alloc(index, 0, FSP_NO_DIR, level, mtr, mtr, err);

	if (!new_block) {
		return nullptr;
	}

	new_page_zip = buf_block_get_page_zip(new_block);
	ut_a(!new_page_zip == !root_page_zip);
	ut_a(!new_page_zip
	     || page_zip_get_size(new_page_zip)
	     == page_zip_get_size(root_page_zip));

	btr_page_create(new_block, new_page_zip, index, level, mtr);
	if (page_has_siblings(new_block->page.frame)) {
		compile_time_assert(FIL_PAGE_NEXT == FIL_PAGE_PREV + 4);
		compile_time_assert(FIL_NULL == 0xffffffff);
		static_assert(FIL_PAGE_PREV % 8 == 0, "alignment");
		memset_aligned<8>(new_block->page.frame + FIL_PAGE_PREV,
				  0xff, 8);
		mtr->memset(new_block, FIL_PAGE_PREV, 8, 0xff);
		if (UNIV_LIKELY_NULL(new_page_zip)) {
			memset_aligned<8>(new_page_zip->data + FIL_PAGE_PREV,
					  0xff, 8);
		}
	}

	/* Copy the records from root to the new page one by one. */
	if (0
#ifdef UNIV_ZIP_COPY
	    || new_page_zip
#endif /* UNIV_ZIP_COPY */
	    || !page_copy_rec_list_end(new_block, root,
				       page_get_infimum_rec(root->page.frame),
				       index, mtr, err)) {
		switch (*err) {
		case DB_SUCCESS:
			break;
		case DB_FAIL:
			*err = DB_SUCCESS;
			break;
		default:
			return nullptr;
		}

		ut_a(new_page_zip);

		/* Copy the page byte for byte. */
		page_zip_copy_recs(new_block, root_page_zip,
				   root->page.frame, index, mtr);

		/* Update the lock table and possible hash index. */
		if (index->has_locking()) {
			lock_move_rec_list_end(
				new_block, root,
				page_get_infimum_rec(root->page.frame));
		}

		btr_search_move_or_delete_hash_entries(new_block, root);
	}

	constexpr uint16_t max_trx_id = PAGE_HEADER + PAGE_MAX_TRX_ID;
	if (!index->is_primary()) {
		/* In secondary indexes,
		PAGE_MAX_TRX_ID can be reset on the root page, because
		the field only matters on leaf pages, and the root no
		longer is a leaf page. (Older versions of InnoDB did
		set PAGE_MAX_TRX_ID on all secondary index pages.) */
		byte* p = my_assume_aligned<8>(
			PAGE_HEADER + PAGE_MAX_TRX_ID + root->page.frame);
		if (mach_read_from_8(p)) {
			mtr->memset(root, max_trx_id, 8, 0);
			if (UNIV_LIKELY_NULL(root->page.zip.data)) {
				memset_aligned<8>(max_trx_id
						  + root->page.zip.data, 0, 8);
			}
		}
	} else {
		/* PAGE_ROOT_AUTO_INC is only present in the clustered index
		root page; on other clustered index pages, we want to reserve
		the field PAGE_MAX_TRX_ID for future use. */
		byte* p = my_assume_aligned<8>(
			PAGE_HEADER + PAGE_MAX_TRX_ID + new_block->page.frame);
		if (mach_read_from_8(p)) {
			mtr->memset(new_block, max_trx_id, 8, 0);
			if (UNIV_LIKELY_NULL(new_block->page.zip.data)) {
				memset_aligned<8>(max_trx_id
						  + new_block->page.zip.data,
						  0, 8);
			}
		}
	}

	/* If this is a pessimistic insert which is actually done to
	perform a pessimistic update then we have stored the lock
	information of the record to be inserted on the infimum of the
	root page: we cannot discard the lock structs on the root page */

	if (index->has_locking()) {
		lock_update_root_raise(*new_block, root_id);
	}

	/* Create a memory heap where the node pointer is stored */
	if (!*heap) {
		*heap = mem_heap_create(1000);
	}

	const uint32_t new_page_no = new_block->page.id().page_no();
	const rec_t* rec= page_is_comp(new_block->page.frame)
		? page_rec_next_get<true>(new_block->page.frame,
					  new_block->page.frame
					  + PAGE_NEW_INFIMUM)
		: page_rec_next_get<false>(new_block->page.frame,
					   new_block->page.frame
					   + PAGE_OLD_INFIMUM);
	ut_ad(rec); /* We just created the page. */

	/* Build the node pointer (= node key and page address) for the
	child */
	node_ptr = dict_index_build_node_ptr(index, rec, new_page_no, *heap,
					     level);
	/* The node pointer must be marked as the predefined minimum record,
	as there is no lower alphabetical limit to records in the leftmost
	node of a level: */
	dtuple_set_info_bits(node_ptr,
			     dtuple_get_info_bits(node_ptr)
			     | REC_INFO_MIN_REC_FLAG);

	/* Rebuild the root page to get free space */
	btr_page_empty(root, root_page_zip, index, level + 1, mtr);
	/* btr_page_empty() is supposed to zero-initialize the field. */
	ut_ad(!page_get_instant(root->page.frame));

	if (index->is_instant()) {
		ut_ad(!root_page_zip);
		btr_set_instant(root, *index, mtr);
	}

	ut_ad(!page_has_siblings(root->page.frame));

	page_cursor = btr_cur_get_page_cur(cursor);

	/* Insert node pointer to the root */

	page_cur_set_before_first(root, page_cursor);

	node_ptr_rec = page_cur_tuple_insert(page_cursor, node_ptr,
					     offsets, heap, 0, mtr);

	/* The root page should only contain the node pointer
	to new_block at this point.  Thus, the data should fit. */
	ut_a(node_ptr_rec);

	page_cursor->block = new_block;
	page_cursor->index = index;

	ut_ad(dtuple_check_typed(tuple));
	/* Reposition the cursor to the child node */
	uint16_t low_match = 0, up_match = 0;

	if (page_cur_search_with_match(tuple, PAGE_CUR_LE,
				       &up_match, &low_match,
				       page_cursor, nullptr)) {
		*err = DB_CORRUPTION;
		return nullptr;
	}

	/* Split the child and insert tuple */
	return btr_page_split_and_insert(flags, cursor, offsets, heap,
					 tuple, n_ext, mtr, err);
}

/** Decide if the page should be split at the convergence point of inserts
converging to the left.
@param cursor	insert position
@return the first record to be moved to the right half page
@retval	nullptr if no split is recommended */
rec_t *btr_page_get_split_rec_to_left(const btr_cur_t *cursor) noexcept
{
  const rec_t *split_rec= btr_cur_get_rec(cursor);
  const page_t *page= btr_cur_get_page(cursor);
  const rec_t *const last= page + page_header_get_offs(page, PAGE_LAST_INSERT);

  if (page_is_comp(page))
  {
    if (last != page_rec_next_get<true>(page, split_rec))
      return nullptr;
    /* The metadata record must be present in the leftmost leaf page
    of the clustered index, if and only if index->is_instant().
    However, during innobase_instant_try(), index->is_instant() would
    already hold when row_ins_clust_index_entry_low() is being invoked
    to insert the the metadata record.  So, we can only assert that
    when the metadata record exists, index->is_instant() must hold. */
    const rec_t *const infimum= page + PAGE_NEW_INFIMUM;
    ut_ad(!page_is_leaf(page) || page_has_prev(page) ||
          cursor->index()->is_instant() ||
          !(rec_get_info_bits(page_rec_next_get<true>(page, infimum), true) &
            REC_INFO_MIN_REC_FLAG));
    /* If the convergence is in the middle of a page, include also the
    record immediately before the new insert to the upper page.
    Otherwise, we could repeatedly move from page to page lots of
    records smaller than the convergence point. */
    if (split_rec == infimum ||
        split_rec == page_rec_next_get<true>(page, infimum))
      split_rec= page_rec_next_get<true>(page, split_rec);
  }
  else
  {
    if (last != page_rec_next_get<false>(page, split_rec))
      return nullptr;
    const rec_t *const infimum= page + PAGE_OLD_INFIMUM;
    ut_ad(!page_is_leaf(page) || page_has_prev(page) ||
          cursor->index()->is_instant() ||
          !(rec_get_info_bits(page_rec_next_get<false>(page, infimum), false) &
            REC_INFO_MIN_REC_FLAG));
    if (split_rec == infimum ||
        split_rec == page_rec_next_get<false>(page, infimum))
      split_rec= page_rec_next_get<false>(page, split_rec);
  }

  return const_cast<rec_t*>(split_rec);
}

/** Decide if the page should be split at the convergence point of inserts
converging to the right.
@param cursor     insert position
@param split_rec  if split recommended, the first record on the right
half page, or nullptr if the to-be-inserted record should be first
@return whether split is recommended */
bool
btr_page_get_split_rec_to_right(const btr_cur_t *cursor, rec_t **split_rec)
  noexcept
{
  const rec_t *insert_point= btr_cur_get_rec(cursor);
  const page_t *page= btr_cur_get_page(cursor);

  /* We use eager heuristics: if the new insert would be right after
  the previous insert on the same page, we assume that there is a
  pattern of sequential inserts here. */
  if (page + page_header_get_offs(page, PAGE_LAST_INSERT) != insert_point)
    return false;

  if (page_is_comp(page))
  {
    const rec_t *const supremum= page + PAGE_NEW_SUPREMUM;
    insert_point= page_rec_next_get<true>(page, insert_point);
    if (!insert_point);
    else if (insert_point == supremum)
      insert_point= nullptr;
    else
    {
      insert_point= page_rec_next_get<true>(page, insert_point);
      if (insert_point == supremum)
        insert_point= nullptr;
      /* If there are >= 2 user records up from the insert point,
      split all but 1 off. We want to keep one because then sequential
      inserts can do the necessary checks of the right search position
      just by looking at the records on this page. */
    }
  }
  else
  {
    const rec_t *const supremum= page + PAGE_OLD_SUPREMUM;
    insert_point= page_rec_next_get<false>(page, insert_point);
    if (!insert_point);
    else if (insert_point == supremum)
      insert_point= nullptr;
    else
    {
      insert_point= page_rec_next_get<false>(page, insert_point);
      if (insert_point == supremum)
        insert_point= nullptr;
    }
  }

  *split_rec= const_cast<rec_t*>(insert_point);
  return true;
}

/*************************************************************//**
Calculates a split record such that the tuple will certainly fit on
its half-page when the split is performed. We assume in this function
only that the cursor page has at least one user record.
@return split record, or NULL if tuple will be the first record on
the lower or upper half-page (determined by btr_page_tuple_smaller()) */
static
rec_t*
btr_page_get_split_rec(
/*===================*/
	btr_cur_t*	cursor,	/*!< in: cursor at which insert should be made */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext)	/*!< in: number of externally stored columns */
{
	page_t*		page;
	page_zip_des_t*	page_zip;
	ulint		insert_size;
	ulint		free_space;
	ulint		total_data;
	ulint		total_n_recs;
	ulint		total_space;
	ulint		incl_data;
	rec_t*		ins_rec;
	rec_t*		rec;
	rec_t*		next_rec;
	ulint		n;
	mem_heap_t*	heap;
	rec_offs*	offsets;

	page = btr_cur_get_page(cursor);

	insert_size = rec_get_converted_size(cursor->index(), tuple, n_ext);
	free_space  = page_get_free_space_of_empty(page_is_comp(page));

	page_zip = btr_cur_get_page_zip(cursor);
	if (page_zip) {
		/* Estimate the free space of an empty compressed page. */
		ulint	free_space_zip = page_zip_empty_size(
			cursor->index()->n_fields,
			page_zip_get_size(page_zip));

		if (free_space > (ulint) free_space_zip) {
			free_space = (ulint) free_space_zip;
		}
	}

	/* free_space is now the free space of a created new page */

	total_data   = page_get_data_size(page) + insert_size;
	total_n_recs = ulint(page_get_n_recs(page)) + 1;
	ut_ad(total_n_recs >= 2);
	total_space  = total_data + page_dir_calc_reserved_space(total_n_recs);

	n = 0;
	incl_data = 0;
	ins_rec = btr_cur_get_rec(cursor);
	rec = page_get_infimum_rec(page);

	heap = NULL;
	offsets = NULL;

	/* We start to include records to the left half, and when the
	space reserved by them exceeds half of total_space, then if
	the included records fit on the left page, they will be put there
	if something was left over also for the right page,
	otherwise the last included record will be the first on the right
	half page */

	do {
		/* Decide the next record to include */
		if (rec == ins_rec) {
			rec = NULL;	/* NULL denotes that tuple is
					now included */
		} else if (rec == NULL) {
			rec = page_rec_get_next(ins_rec);
		} else {
			rec = page_rec_get_next(rec);
		}

		if (rec == NULL) {
			/* Include tuple */
			incl_data += insert_size;
		} else {
			offsets = rec_get_offsets(rec, cursor->index(),
						  offsets, page_is_leaf(page)
						  ? cursor->index()
						  ->n_core_fields
						  : 0,
						  ULINT_UNDEFINED, &heap);
			incl_data += rec_offs_size(offsets);
		}

		n++;
	} while (incl_data + page_dir_calc_reserved_space(n)
		 < total_space / 2);

	if (incl_data + page_dir_calc_reserved_space(n) <= free_space) {
		/* The next record will be the first on
		the right half page if it is not the
		supremum record of page */

		if (rec == ins_rec) {
			rec = NULL;

			goto func_exit;
		} else if (rec == NULL) {
			next_rec = page_rec_get_next(ins_rec);
		} else {
			next_rec = page_rec_get_next(rec);
		}
		ut_ad(next_rec);
		if (!page_rec_is_supremum(next_rec)) {
			rec = next_rec;
		}
	}

func_exit:
	if (heap) {
		mem_heap_free(heap);
	}
	return(rec);
}

#ifdef UNIV_DEBUG
/*************************************************************//**
Returns TRUE if the insert fits on the appropriate half-page with the
chosen split_rec.
@return true if fits */
static MY_ATTRIBUTE((nonnull(1,3,4,6), warn_unused_result))
bool
btr_page_insert_fits(
/*=================*/
	btr_cur_t*	cursor,	/*!< in: cursor at which insert
				should be made */
	const rec_t*	split_rec,/*!< in: suggestion for first record
				on upper half-page, or NULL if
				tuple to be inserted should be first */
	rec_offs**	offsets,/*!< in: rec_get_offsets(
				split_rec, cursor->index()); out: garbage */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mem_heap_t**	heap)	/*!< in: temporary memory heap */
{
	page_t*		page;
	ulint		insert_size;
	ulint		free_space;
	ulint		total_data;
	ulint		total_n_recs;
	const rec_t*	rec;
	const rec_t*	end_rec;

	page = btr_cur_get_page(cursor);

	ut_ad(!split_rec
	      || !page_is_comp(page) == !rec_offs_comp(*offsets));
	ut_ad(!split_rec
	      || rec_offs_validate(split_rec, cursor->index(), *offsets));

	insert_size = rec_get_converted_size(cursor->index(), tuple, n_ext);
	free_space  = page_get_free_space_of_empty(page_is_comp(page));

	/* free_space is now the free space of a created new page */

	total_data   = page_get_data_size(page) + insert_size;
	total_n_recs = ulint(page_get_n_recs(page)) + 1;

	/* We determine which records (from rec to end_rec, not including
	end_rec) will end up on the other half page from tuple when it is
	inserted. */

	if (!(end_rec = split_rec)) {
		end_rec = page_rec_get_next(btr_cur_get_rec(cursor));
	} else if (cmp_dtuple_rec(tuple, split_rec, cursor->index(),
				  *offsets) < 0) {
		rec = split_rec;
		end_rec = page_get_supremum_rec(page);
		goto got_rec;
	}

	if (!(rec = page_rec_get_next(page_get_infimum_rec(page)))) {
		return false;
	}

got_rec:
	if (total_data + page_dir_calc_reserved_space(total_n_recs)
	    <= free_space) {

		/* Ok, there will be enough available space on the
		half page where the tuple is inserted */

		return(true);
	}

	while (rec != end_rec) {
		/* In this loop we calculate the amount of reserved
		space after rec is removed from page. */

		*offsets = rec_get_offsets(rec, cursor->index(), *offsets,
					   page_is_leaf(page)
					   ? cursor->index()->n_core_fields
					   : 0,
					   ULINT_UNDEFINED, heap);

		total_data -= rec_offs_size(*offsets);
		total_n_recs--;

		if (total_data + page_dir_calc_reserved_space(total_n_recs)
		    <= free_space) {

			/* Ok, there will be enough available space on the
			half page where the tuple is inserted */

			return(true);
		}

		if (!(rec = page_rec_get_next_const(rec))) {
			break;
		}
	}

	return(false);
}
#endif

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
{
	big_rec_t*	dummy_big_rec;
	btr_cur_t	cursor;
	rec_t*		rec;
	mem_heap_t*	heap = NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets         = offsets_;
	rec_offs_init(offsets_);
	rtr_info_t	rtr_info;

	ut_ad(level > 0);

	flags |= BTR_NO_LOCKING_FLAG | BTR_KEEP_SYS_FLAG
		| BTR_NO_UNDO_LOG_FLAG;
	cursor.page_cur.index = index;

	dberr_t err;

	if (index->is_spatial()) {
		/* For spatial index, initialize structures to track
		its parents etc. */
		rtr_init_rtr_info(&rtr_info, false, &cursor, index, false);

		rtr_info_update_btr(&cursor, &rtr_info);
		err = rtr_search_to_nth_level(&cursor, nullptr, tuple,
					      BTR_CONT_MODIFY_TREE, mtr,
					      PAGE_CUR_RTREE_INSERT, level);
	} else {
		err = btr_cur_search_to_nth_level(level, tuple, RW_X_LATCH,
						  &cursor, mtr);
	}

	ut_ad(cursor.flag == BTR_CUR_BINARY);
	ut_ad(btr_cur_get_block(&cursor)
	      != mtr->at_savepoint(mtr->get_savepoint() - 1)
	      || index->is_spatial()
	      || mtr->memo_contains(index->lock, MTR_MEMO_X_LOCK));

	if (UNIV_LIKELY(err == DB_SUCCESS)) {
		err = btr_cur_optimistic_insert(flags,
						&cursor, &offsets, &heap,
						tuple, &rec,
						&dummy_big_rec, 0, NULL, mtr);
	}

	if (err == DB_FAIL) {
		err = btr_cur_pessimistic_insert(flags,
						 &cursor, &offsets, &heap,
						 tuple, &rec,
						 &dummy_big_rec, 0, NULL, mtr);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	if (index->is_spatial()) {
		ut_ad(cursor.rtr_info);

		rtr_clean_rtr_info(&rtr_info, true);
	}

	return err;
}

static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
static_assert(FIL_PAGE_PREV % 4 == 0, "alignment");
static_assert(FIL_PAGE_NEXT % 4 == 0, "alignment");

MY_ATTRIBUTE((nonnull,warn_unused_result))
/**************************************************************//**
Attaches the halves of an index page on the appropriate level in an
index tree. */
static
dberr_t
btr_attach_half_pages(
/*==================*/
	ulint		flags,		/*!< in: undo logging and
					locking flags */
	dict_index_t*	index,		/*!< in: the index tree */
	buf_block_t*	block,		/*!< in/out: page to be split */
	const rec_t*	split_rec,	/*!< in: first record on upper
					half page */
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	ulint		direction,	/*!< in: FSP_UP or FSP_DOWN */
	mtr_t*		mtr)		/*!< in: mtr */
{
	dtuple_t*	node_ptr_upper;
	mem_heap_t*	heap;
	buf_block_t*	prev_block = nullptr;
	buf_block_t*	next_block = nullptr;
	buf_block_t*	lower_block;
	buf_block_t*	upper_block;

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->memo_contains_flagged(new_block, MTR_MEMO_PAGE_X_FIX));

	/* Create a memory heap where the data tuple is stored */
	heap = mem_heap_create(1024);

	/* Based on split direction, decide upper and lower pages */
	if (direction == FSP_DOWN) {

		btr_cur_t	cursor;
		rec_offs*	offsets;

		lower_block = new_block;
		upper_block = block;

		cursor.page_cur.block = block;
		cursor.page_cur.index = index;

		/* Look up the index for the node pointer to page */
		offsets = btr_page_get_father_block(nullptr, heap, mtr,
						    &cursor);

		if (UNIV_UNLIKELY(!offsets)) {
			mem_heap_free(heap);
			return DB_CORRUPTION;
		}

		/* Replace the address of the old child node (= page) with the
		address of the new lower half */

		btr_node_ptr_set_child_page_no(
			btr_cur_get_block(&cursor),
			btr_cur_get_rec(&cursor),
			offsets, lower_block->page.id().page_no(), mtr);
		mem_heap_empty(heap);
	} else {
		lower_block = block;
		upper_block = new_block;
	}

	/* Get the level of the split pages */
	const ulint level = btr_page_get_level(block->page.frame);
	ut_ad(level == btr_page_get_level(new_block->page.frame));
	page_id_t id{block->page.id()};

	/* Get the previous and next pages of page */
	const uint32_t prev_page_no = btr_page_get_prev(block->page.frame);
	const uint32_t next_page_no = btr_page_get_next(block->page.frame);

	/* for consistency, both blocks should be locked, before change */
	if (prev_page_no != FIL_NULL && direction == FSP_DOWN) {
		id.set_page_no(prev_page_no);
		prev_block = mtr->get_already_latched(id, MTR_MEMO_PAGE_X_FIX);
#if 1 /* MDEV-29835 FIXME: acquire page latches upfront */
		if (!prev_block) {
			ut_ad(mtr->memo_contains(index->lock,
						 MTR_MEMO_X_LOCK));
			prev_block = btr_block_get(*index, prev_page_no,
						   RW_X_LATCH, mtr);
		}
#endif
	}
	if (next_page_no != FIL_NULL && direction != FSP_DOWN) {
		id.set_page_no(next_page_no);
		next_block = mtr->get_already_latched(id, MTR_MEMO_PAGE_X_FIX);
#if 1 /* MDEV-29835 FIXME: acquire page latches upfront */
		if (!next_block) {
			ut_ad(mtr->memo_contains(index->lock,
						 MTR_MEMO_X_LOCK));
			next_block = btr_block_get(*index, next_page_no,
						   RW_X_LATCH, mtr);
		}
#endif
	}

	/* Build the node pointer (= node key and page address) for the upper
	half */

	node_ptr_upper = dict_index_build_node_ptr(
		index, split_rec, upper_block->page.id().page_no(),
		heap, level);

	/* Insert it next to the pointer to the lower half. Note that this
	may generate recursion leading to a split on the higher level. */

	dberr_t err = btr_insert_on_non_leaf_level(
		flags, index, level + 1, node_ptr_upper, mtr);

	/* Free the memory heap */
	mem_heap_free(heap);

	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		return err;
	}

	/* Update page links of the level */

	if (prev_block) {
		if (UNIV_UNLIKELY(memcmp_aligned<4>(prev_block->page.frame
                                                    + FIL_PAGE_NEXT,
                                                    block->page.frame
                                                    + FIL_PAGE_OFFSET,
                                                    4))) {
			return DB_CORRUPTION;
		}
		btr_page_set_next(prev_block, lower_block->page.id().page_no(),
				  mtr);
	}

	if (next_block) {
		if (UNIV_UNLIKELY(memcmp_aligned<4>(next_block->page.frame
                                                    + FIL_PAGE_PREV,
                                                    block->page.frame
                                                    + FIL_PAGE_OFFSET,
                                                    4))) {
			return DB_CORRUPTION;
		}
		btr_page_set_prev(next_block, upper_block->page.id().page_no(),
				  mtr);
	}

	if (direction == FSP_DOWN) {
		ut_ad(lower_block == new_block);
		ut_ad(btr_page_get_next(upper_block->page.frame)
		      == next_page_no);
		btr_page_set_prev(lower_block, prev_page_no, mtr);
	} else {
		ut_ad(upper_block == new_block);
		ut_ad(btr_page_get_prev(lower_block->page.frame)
		      == prev_page_no);
		btr_page_set_next(upper_block, next_page_no, mtr);
	}

	btr_page_set_prev(upper_block, lower_block->page.id().page_no(), mtr);
	btr_page_set_next(lower_block, upper_block->page.id().page_no(), mtr);

	return DB_SUCCESS;
}

/*************************************************************//**
Determine if a tuple is smaller than any record on the page.
@return TRUE if smaller */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
btr_page_tuple_smaller(
/*===================*/
	btr_cur_t*	cursor,	/*!< in: b-tree cursor */
	const dtuple_t*	tuple,	/*!< in: tuple to consider */
	rec_offs**	offsets,/*!< in/out: temporary storage */
	ulint		n_uniq,	/*!< in: number of unique fields
				in the index page records */
	mem_heap_t**	heap)	/*!< in/out: heap for offsets */
{
	buf_block_t*	block;
	const rec_t*	first_rec;
	page_cur_t	pcur;

	/* Read the first user record in the page. */
	block = btr_cur_get_block(cursor);
	page_cur_set_before_first(block, &pcur);
	if (UNIV_UNLIKELY(!(first_rec = page_cur_move_to_next(&pcur)))) {
		ut_ad("corrupted page" == 0);
		return false;
	}

	*offsets = rec_get_offsets(first_rec, cursor->index(), *offsets,
				   page_is_leaf(block->page.frame)
				   ? cursor->index()->n_core_fields : 0,
				   n_uniq, heap);

	return cmp_dtuple_rec(tuple, first_rec, cursor->index(), *offsets) < 0;
}

/** Insert the tuple into the right sibling page, if the cursor is at the end
of a page.
@param[in]	flags	undo logging and locking flags
@param[in,out]	cursor	cursor at which to insert; when the function succeeds,
			the cursor is positioned before the insert point.
@param[out]	offsets	offsets on inserted record
@param[in,out]	heap	memory heap for allocating offsets
@param[in]	tuple	tuple to insert
@param[in]	n_ext	number of externally stored columns
@param[in,out]	mtr	mini-transaction
@return	inserted record (first record on the right sibling page);
	the cursor will be positioned on the page infimum
@retval	NULL if the operation was not performed */
static
rec_t*
btr_insert_into_right_sibling(
	ulint		flags,
	btr_cur_t*	cursor,
	rec_offs**	offsets,
	mem_heap_t*	heap,
	const dtuple_t*	tuple,
	ulint		n_ext,
	mtr_t*		mtr)
{
	buf_block_t*	block = btr_cur_get_block(cursor);
	page_t*		page = buf_block_get_frame(block);
	const uint32_t	next_page_no = btr_page_get_next(page);

	ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
					 MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(heap);
	ut_ad(dtuple_check_typed(tuple));

	if (next_page_no == FIL_NULL || !page_rec_is_supremum(
			page_rec_get_next(btr_cur_get_rec(cursor)))) {

		return nullptr;
	}

	page_cur_t	next_page_cursor;
	buf_block_t*	next_block;
	page_t*		next_page;
	btr_cur_t	next_father_cursor;
	rec_t*		rec = nullptr;

	next_block = btr_block_get(*cursor->index(), next_page_no, RW_X_LATCH,
				   mtr);
	if (UNIV_UNLIKELY(!next_block)) {
		return nullptr;
	}
	next_page = buf_block_get_frame(next_block);
	const bool is_leaf = page_is_leaf(next_page);

	next_page_cursor.index = cursor->index();
	next_page_cursor.block = next_block;
	next_father_cursor.page_cur = next_page_cursor;

	if (!btr_page_get_father(mtr, &next_father_cursor)) {
		return nullptr;
	}

	uint16_t up_match = 0, low_match = 0;

	if (page_cur_search_with_match(tuple,
				       PAGE_CUR_LE, &up_match, &low_match,
				       &next_page_cursor, nullptr)) {
		return nullptr;
	}

	/* Extends gap lock for the next page */
	if (is_leaf && cursor->index()->has_locking()) {
		lock_update_node_pointer(block, next_block);
	}

	rec = page_cur_tuple_insert(&next_page_cursor, tuple, offsets, &heap,
				    n_ext, mtr);

	if (!rec) {
		return nullptr;
	}

	ibool	compressed;
	dberr_t	err;
	ulint	level = btr_page_get_level(next_page);

	/* adjust cursor position */
	*btr_cur_get_page_cur(cursor) = next_page_cursor;

	ut_ad(btr_cur_get_rec(cursor) == page_get_infimum_rec(next_page));
	ut_ad(page_rec_get_next(page_get_infimum_rec(next_page)) == rec);

	/* We have to change the parent node pointer */

	compressed = btr_cur_pessimistic_delete(
		&err, TRUE, &next_father_cursor,
		BTR_CREATE_FLAG, false, mtr);

	if (err != DB_SUCCESS) {
		return nullptr;
	}

	if (!compressed) {
		btr_cur_compress_if_useful(&next_father_cursor, false, mtr);
	}

	dtuple_t*	node_ptr = dict_index_build_node_ptr(
		cursor->index(), rec, next_block->page.id().page_no(),
		heap, level);

	if (btr_insert_on_non_leaf_level(flags, cursor->index(), level + 1,
					 node_ptr, mtr) != DB_SUCCESS) {
		return nullptr;
	}

	ut_ad(rec_offs_validate(rec, cursor->index(), *offsets));
	return(rec);
}

/*************************************************************//**
Moves record list end to another page. Moved records include
split_rec.
@return error code */
static
dberr_t
page_move_rec_list_end(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in: index page from where to move */
	rec_t*		split_rec,	/*!< in: first record to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	ulint		old_data_size;
	ulint		new_data_size;
	ulint		old_n_recs;
	ulint		new_n_recs;

	ut_ad(!dict_index_is_spatial(index));

	old_data_size = page_get_data_size(new_page);
	old_n_recs = page_get_n_recs(new_page);
#ifdef UNIV_ZIP_DEBUG
	{
		page_zip_des_t*	new_page_zip
			= buf_block_get_page_zip(new_block);
		page_zip_des_t*	page_zip
			= buf_block_get_page_zip(block);
		ut_a(!new_page_zip == !page_zip);
		ut_a(!new_page_zip
		     || page_zip_validate(new_page_zip, new_page, index));
		ut_a(!page_zip
		     || page_zip_validate(page_zip, page_align(split_rec),
					  index));
	}
#endif /* UNIV_ZIP_DEBUG */

	dberr_t err;
	if (!page_copy_rec_list_end(new_block, block,
				    split_rec, index, mtr, &err)) {
		return err;
	}

	new_data_size = page_get_data_size(new_page);
	new_n_recs = page_get_n_recs(new_page);

	ut_ad(new_data_size >= old_data_size);

	return page_delete_rec_list_end(split_rec, block, index,
					new_n_recs - old_n_recs,
					new_data_size - old_data_size, mtr);
}

/*************************************************************//**
Moves record list start to another page. Moved records do not include
split_rec.
@return error code */
static
dberr_t
page_move_rec_list_start(
/*=====================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in/out: page containing split_rec */
	rec_t*		split_rec,	/*!< in: first record not to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
  dberr_t err;
  if (page_copy_rec_list_start(new_block, block, split_rec, index, mtr, &err))
    page_delete_rec_list_start(split_rec, block, index, mtr);
  return err;
}

/*************************************************************//**
Splits an index page to halves and inserts the tuple. It is assumed
that mtr holds an x-latch to the index tree. NOTE: the tree x-latch is
released within this function! NOTE that the operation of this
function must always succeed, we cannot reverse it: therefore enough
free disk space (2 pages) must be guaranteed to be available before
this function is called.
@return inserted record or NULL if run out of space */
rec_t*
btr_page_split_and_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	rec_offs**	offsets,/*!< out: offsets on inserted record */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err)	/*!< out: error code */
{
	buf_block_t*	block;
	page_t*		page;
	page_zip_des_t*	page_zip;
	buf_block_t*	new_block;
	page_t*		new_page;
	page_zip_des_t*	new_page_zip;
	rec_t*		split_rec;
	buf_block_t*	left_block;
	buf_block_t*	right_block;
	page_cur_t*	page_cursor;
	rec_t*		first_rec;
	byte*		buf = 0; /* remove warning */
	rec_t*		move_limit;
	ulint		n_iterations = 0;
	ulint		n_uniq;

	ut_ad(*err == DB_SUCCESS);
	ut_ad(dtuple_check_typed(tuple));
	ut_ad(!cursor->index()->is_spatial());

	buf_pool.pages_split++;

	if (!*heap) {
		*heap = mem_heap_create(1024);
	}
	n_uniq = dict_index_get_n_unique_in_tree(cursor->index());
func_start:
	mem_heap_empty(*heap);
	*offsets = NULL;

	ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
					 MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(!dict_index_is_online_ddl(cursor->index())
	      || (flags & BTR_CREATE_FLAG)
	      || dict_index_is_clust(cursor->index()));
	ut_ad(cursor->index()->lock.have_u_or_x());

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	page_zip = buf_block_get_page_zip(block);

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!page_is_empty(page));

	/* try to insert to the next page if possible before split */
	if (rec_t* rec = btr_insert_into_right_sibling(
		    flags, cursor, offsets, *heap, tuple, n_ext, mtr)) {
		return(rec);
	}

	/* 1. Decide the split record; split_rec == NULL means that the
	tuple to be inserted should be the first record on the upper
	half-page */
	bool insert_left = false;
	uint32_t hint_page_no = block->page.id().page_no() + 1;
	byte direction = FSP_UP;

	if (n_iterations > 0) {
		split_rec = btr_page_get_split_rec(cursor, tuple, n_ext);

		if (split_rec == NULL) {
			insert_left = btr_page_tuple_smaller(
				cursor, tuple, offsets, n_uniq, heap);
		}
	} else if (btr_page_get_split_rec_to_right(cursor, &split_rec)) {
	} else if ((split_rec = btr_page_get_split_rec_to_left(cursor))) {
		direction = FSP_DOWN;
		hint_page_no -= 2;
	} else {
		/* If there is only one record in the index page, we
		can't split the node in the middle by default. We need
		to determine whether the new record will be inserted
		to the left or right. */

		if (page_get_n_recs(page) > 1) {
			split_rec = page_get_middle_rec(page);
		} else if (btr_page_tuple_smaller(cursor, tuple,
						  offsets, n_uniq, heap)) {
			split_rec = page_rec_get_next(
				page_get_infimum_rec(page));
		} else {
			split_rec = NULL;
			goto got_split_rec;
		}

		if (UNIV_UNLIKELY(!split_rec)) {
			*err = DB_CORRUPTION;
			return nullptr;
		}
	}

got_split_rec:
	/* 2. Allocate a new page to the index */
	const uint16_t page_level = btr_page_get_level(page);
	new_block = btr_page_alloc(cursor->index(), hint_page_no, direction,
				   page_level, mtr, mtr, err);

	if (!new_block) {
		return nullptr;
	}

	new_page = buf_block_get_frame(new_block);
	new_page_zip = buf_block_get_page_zip(new_block);

	if (page_level && UNIV_LIKELY_NULL(new_page_zip)) {
		/* ROW_FORMAT=COMPRESSED non-leaf pages are not expected
		to contain FIL_NULL in FIL_PAGE_PREV at this stage. */
		memset_aligned<4>(new_page + FIL_PAGE_PREV, 0, 4);
	}
	btr_page_create(new_block, new_page_zip, cursor->index(),
			page_level, mtr);

	/* 3. Calculate the first record on the upper half-page, and the
	first record (move_limit) on original page which ends up on the
	upper half */

	if (split_rec) {
		first_rec = move_limit = split_rec;

		*offsets = rec_get_offsets(split_rec, cursor->index(),
					   *offsets, page_is_leaf(page)
					   ? cursor->index()->n_core_fields
					   : 0,
					   n_uniq, heap);

		insert_left = cmp_dtuple_rec(tuple, split_rec, cursor->index(),
					     *offsets) < 0;

		if (!insert_left && new_page_zip && n_iterations > 0) {
			/* If a compressed page has already been split,
			avoid further splits by inserting the record
			to an empty page. */
			split_rec = NULL;
			goto insert_empty;
		}
	} else if (insert_left) {
		if (UNIV_UNLIKELY(!n_iterations)) {
corrupted:
			*err = DB_CORRUPTION;
			return nullptr;
		}
		first_rec = page_rec_get_next(page_get_infimum_rec(page));
insert_move_limit:
		move_limit = page_rec_get_next(btr_cur_get_rec(cursor));
		if (UNIV_UNLIKELY(!first_rec || !move_limit)) {
			goto corrupted;
		}
	} else {
insert_empty:
		ut_ad(!split_rec);
		ut_ad(!insert_left);
		buf = UT_NEW_ARRAY_NOKEY(
			byte,
			rec_get_converted_size(cursor->index(), tuple, n_ext));

		first_rec = rec_convert_dtuple_to_rec(buf, cursor->index(),
						      tuple, n_ext);
		goto insert_move_limit;
	}

	/* 4. Do first the modifications in the tree structure */

	/* FIXME: write FIL_PAGE_PREV,FIL_PAGE_NEXT in new_block earlier! */
	*err = btr_attach_half_pages(flags, cursor->index(), block,
				     first_rec, new_block, direction, mtr);

	if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
		return nullptr;
	}

#ifdef UNIV_DEBUG
	/* If the split is made on the leaf level and the insert will fit
	on the appropriate half-page, we may release the tree x-latch.
	We can then move the records after releasing the tree latch,
	thus reducing the tree latch contention. */
	const bool insert_will_fit = !new_page_zip
		&& btr_page_insert_fits(cursor, split_rec, offsets, tuple,
					n_ext, heap);
#endif
	if (!split_rec && !insert_left) {
		UT_DELETE_ARRAY(buf);
		buf = NULL;
	}

#if 0 // FIXME: this used to be a no-op, and may cause trouble if enabled
	if (insert_will_fit
	    && page_is_leaf(page)
	    && !dict_index_is_online_ddl(cursor->index())) {
		mtr->release(cursor->index()->lock);
		/* NOTE: We cannot release root block latch here, because it
		has segment header and already modified in most of cases.*/
	}
#endif

	/* 5. Move then the records to the new page */
	if (direction == FSP_DOWN) {
		/*		fputs("Split left\n", stderr); */

		if (0
#ifdef UNIV_ZIP_COPY
		    || page_zip
#endif /* UNIV_ZIP_COPY */
		    || (*err = page_move_rec_list_start(new_block, block,
							move_limit,
							cursor->index(),
							mtr))) {
			if (*err != DB_FAIL) {
				return nullptr;
			}

			/* For some reason, compressing new_block failed,
			even though it should contain fewer records than
			the original page.  Copy the page byte for byte
			and then delete the records from both pages
			as appropriate.  Deleting will always succeed. */
			ut_a(new_page_zip);

			page_zip_copy_recs(new_block, page_zip, page,
					   cursor->index(), mtr);
			*err = page_delete_rec_list_end(move_limit
							- page + new_page,
							new_block,
							cursor->index(),
							ULINT_UNDEFINED,
							ULINT_UNDEFINED, mtr);
			if (*err != DB_SUCCESS) {
				return nullptr;
			}

			/* Update the lock table and possible hash index. */
			if (cursor->index()->has_locking()) {
				lock_move_rec_list_start(
					new_block, block, move_limit,
					new_page + PAGE_NEW_INFIMUM);
			}

			btr_search_move_or_delete_hash_entries(
				new_block, block);

			/* Delete the records from the source page. */

			page_delete_rec_list_start(move_limit, block,
						   cursor->index(), mtr);
		}

		left_block = new_block;
		right_block = block;

		if (cursor->index()->has_locking()) {
			lock_update_split_left(right_block, left_block);
		}
	} else {
		/*		fputs("Split right\n", stderr); */

		if (0
#ifdef UNIV_ZIP_COPY
		    || page_zip
#endif /* UNIV_ZIP_COPY */
		    || (*err = page_move_rec_list_end(new_block, block,
						      move_limit,
						      cursor->index(), mtr))) {
			if (*err != DB_FAIL) {
				return nullptr;
			}

			/* For some reason, compressing new_page failed,
			even though it should contain fewer records than
			the original page.  Copy the page byte for byte
			and then delete the records from both pages
			as appropriate.  Deleting will always succeed. */
			ut_a(new_page_zip);

			page_zip_copy_recs(new_block, page_zip, page,
					   cursor->index(), mtr);
			page_delete_rec_list_start(move_limit - page
						   + new_page, new_block,
						   cursor->index(), mtr);

			/* Update the lock table and possible hash index. */
			if (cursor->index()->has_locking()) {
				lock_move_rec_list_end(new_block, block,
						       move_limit);
			}

			btr_search_move_or_delete_hash_entries(
				new_block, block);

			/* Delete the records from the source page. */

			*err = page_delete_rec_list_end(move_limit, block,
							cursor->index(),
							ULINT_UNDEFINED,
							ULINT_UNDEFINED, mtr);
			if (*err != DB_SUCCESS) {
				return nullptr;
			}
		}

		left_block = block;
		right_block = new_block;

		if (cursor->index()->has_locking()) {
			lock_update_split_right(right_block, left_block);
		}
	}

#ifdef UNIV_ZIP_DEBUG
	if (page_zip) {
		ut_a(page_zip_validate(page_zip, page, cursor->index()));
		ut_a(page_zip_validate(new_page_zip, new_page,
				       cursor->index()));
	}
#endif /* UNIV_ZIP_DEBUG */

	/* At this point, split_rec, move_limit and first_rec may point
	to garbage on the old page. */

	/* 6. The split and the tree modification is now completed. Decide the
	page where the tuple should be inserted */
	rec_t* rec;
	buf_block_t* const insert_block = insert_left
		? left_block : right_block;

	/* 7. Reposition the cursor for insert and try insertion */
	page_cursor = btr_cur_get_page_cur(cursor);
	page_cursor->block = insert_block;

	uint16_t up_match = 0, low_match = 0;

	if (page_cur_search_with_match(tuple,
				       PAGE_CUR_LE, &up_match, &low_match,
				       page_cursor, nullptr)) {
		*err = DB_CORRUPTION;
		return nullptr;
	}

	rec = page_cur_tuple_insert(page_cursor, tuple,
				    offsets, heap, n_ext, mtr);

#ifdef UNIV_ZIP_DEBUG
	{
		page_t*		insert_page
			= buf_block_get_frame(insert_block);

		page_zip_des_t*	insert_page_zip
			= buf_block_get_page_zip(insert_block);

		ut_a(!insert_page_zip
		     || page_zip_validate(insert_page_zip, insert_page,
					  cursor->index()));
	}
#endif /* UNIV_ZIP_DEBUG */

	if (rec != NULL) {

		goto func_exit;
	}

	/* 8. If insert did not fit, try page reorganization.
	For compressed pages, page_cur_tuple_insert() will have
	attempted this already. */

	if (page_cur_get_page_zip(page_cursor)) {
		goto insert_failed;
	}

	*err = btr_page_reorganize(page_cursor, mtr);

	if (*err != DB_SUCCESS) {
		return nullptr;
	}

	rec = page_cur_tuple_insert(page_cursor, tuple,
				    offsets, heap, n_ext, mtr);

	if (rec == NULL) {
		/* The insert did not fit on the page: loop back to the
		start of the function for a new split */
insert_failed:
		n_iterations++;
		ut_ad(n_iterations < 2
		      || buf_block_get_page_zip(insert_block));
		ut_ad(!insert_will_fit);

		goto func_start;
	}

func_exit:
	ut_ad(page_validate(buf_block_get_frame(left_block),
			    page_cursor->index));
	ut_ad(page_validate(buf_block_get_frame(right_block),
			    page_cursor->index));

	ut_ad(!rec || rec_offs_validate(rec, page_cursor->index, *offsets));
	return(rec);
}

/** Remove a page from the level list of pages.
@param[in]	block		page to remove
@param[in]	index		index tree
@param[in,out]	mtr		mini-transaction */
dberr_t btr_level_list_remove(const buf_block_t& block,
                              const dict_index_t& index, mtr_t* mtr)
{
  ut_ad(mtr->memo_contains_flagged(&block, MTR_MEMO_PAGE_X_FIX));
  ut_ad(block.zip_size() == index.table->space->zip_size());
  ut_ad(index.table->space->id == block.page.id().space());
  /* Get the previous and next page numbers of page */
  const uint32_t prev_page_no= btr_page_get_prev(block.page.frame);
  const uint32_t next_page_no= btr_page_get_next(block.page.frame);
  page_id_t id{block.page.id()};
  buf_block_t *prev= nullptr, *next;
  dberr_t err;

  /* Update page links of the level */
  if (prev_page_no != FIL_NULL)
  {
    id.set_page_no(prev_page_no);
    prev= mtr->get_already_latched(id, MTR_MEMO_PAGE_X_FIX);
#if 1 /* MDEV-29835 FIXME: acquire page latches upfront */
    if (!prev)
    {
      ut_ad(mtr->memo_contains(index.lock, MTR_MEMO_X_LOCK));
      prev= btr_block_get(index, id.page_no(), RW_X_LATCH, mtr, &err);
      if (UNIV_UNLIKELY(!prev))
        return err;
    }
#endif
  }

  if (next_page_no != FIL_NULL)
  {
    id.set_page_no(next_page_no);
    next= mtr->get_already_latched(id, MTR_MEMO_PAGE_X_FIX);
#if 1 /* MDEV-29835 FIXME: acquire page latches upfront */
    if (!next)
    {
      ut_ad(mtr->memo_contains(index.lock, MTR_MEMO_X_LOCK));
      next= btr_block_get(index, id.page_no(), RW_X_LATCH, mtr, &err);
      if (UNIV_UNLIKELY(!next))
        return err;
    }
#endif
    btr_page_set_prev(next, prev_page_no, mtr);
  }

  if (prev)
    btr_page_set_next(prev, next_page_no, mtr);

  return DB_SUCCESS;
}

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
	que_thr_t*	thr,	/*!< in/out: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	dberr_t*	err)	/*!< out: error code */
{
	buf_block_t*	father_block;
	ulint		page_level;
	page_zip_des_t*	father_page_zip;
	page_t*		page		= buf_block_get_frame(block);
	ulint		root_page_no;
	buf_block_t*	blocks[BTR_MAX_LEVELS];
	ulint		n_blocks;	/*!< last used index in blocks[] */
	ulint		i;
	bool		lift_father_up;
	buf_block_t*	block_orig	= block;

	ut_ad(!page_has_siblings(page));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!page_is_empty(page));

	page_level = btr_page_get_level(page);
	root_page_no = dict_index_get_page(index);

	{
		btr_cur_t	cursor;
		rec_offs*	offsets	= NULL;
		mem_heap_t*	heap	= mem_heap_create(
			sizeof(*offsets)
			* (REC_OFFS_HEADER_SIZE + 1 + 1
			   + unsigned(index->n_fields)));
		buf_block_t*	b;
		cursor.page_cur.index = index;
		cursor.page_cur.block = block;

		if (index->is_spatial()) {
			offsets = rtr_page_get_father_block(
				nullptr, heap, nullptr, &cursor,
				thr, mtr);
		} else {
			offsets = btr_page_get_father_block(offsets, heap,
							    mtr, &cursor);
		}

		if (UNIV_UNLIKELY(!offsets)) {
parent_corrupted:
			mem_heap_free(heap);
			*err = DB_CORRUPTION;
			return nullptr;
		}

		father_block = btr_cur_get_block(&cursor);
		father_page_zip = buf_block_get_page_zip(father_block);

		n_blocks = 0;

		/* Store all ancestor pages so we can reset their
		levels later on.  We have to do all the searches on
		the tree now because later on, after we've replaced
		the first level, the tree is in an inconsistent state
		and can not be searched. */
		for (b = father_block;
		     b->page.id().page_no() != root_page_no; ) {
			ut_a(n_blocks < BTR_MAX_LEVELS);

			if (index->is_spatial()) {
				offsets = rtr_page_get_father_block(
					nullptr, heap, nullptr, &cursor, thr,
					mtr);
			} else {
				offsets = btr_page_get_father_block(offsets,
								    heap,
								    mtr,
								    &cursor);
			}

			if (UNIV_UNLIKELY(!offsets)) {
				goto parent_corrupted;
			}

			blocks[n_blocks++] = b = btr_cur_get_block(&cursor);
		}

		lift_father_up = (n_blocks && page_level == 0);
		if (lift_father_up) {
			/* The father page also should be the only on its level (not
			root). We should lift up the father page at first.
			Because the leaf page should be lifted up only for root page.
			The freeing page is based on page_level (==0 or !=0)
			to choose segment. If the page_level is changed ==0 from !=0,
			later freeing of the page doesn't find the page allocation
			to be freed.*/

			block = father_block;
			page = buf_block_get_frame(block);
			page_level = btr_page_get_level(page);

			ut_ad(!page_has_siblings(page));
			ut_ad(mtr->memo_contains_flagged(block,
							 MTR_MEMO_PAGE_X_FIX));

			father_block = blocks[0];
			father_page_zip = buf_block_get_page_zip(father_block);
		}

		mem_heap_free(heap);
	}

	btr_search_drop_page_hash_index(block, nullptr);

	/* Make the father empty */
	btr_page_empty(father_block, father_page_zip, index, page_level, mtr);
	/* btr_page_empty() is supposed to zero-initialize the field. */
	ut_ad(!page_get_instant(father_block->page.frame));

	if (index->is_instant()
	    && father_block->page.id().page_no() == root_page_no) {
		ut_ad(!father_page_zip);

		if (page_is_leaf(page)) {
			const rec_t* rec = page_rec_get_next(
				page_get_infimum_rec(page));
			ut_ad(rec_is_metadata(rec, *index));
			if (rec_is_add_metadata(rec, *index)
			    && page_get_n_recs(page) == 1) {
				index->clear_instant_add();
				goto copied;
			}
		}

		btr_set_instant(father_block, *index, mtr);
	}

	/* Copy the records to the father page one by one. */
	if (0
#ifdef UNIV_ZIP_COPY
	    || father_page_zip
#endif /* UNIV_ZIP_COPY */
	    || !page_copy_rec_list_end(father_block, block,
				       page_get_infimum_rec(page),
				       index, mtr, err)) {
		switch (*err) {
		case DB_SUCCESS:
			break;
		case DB_FAIL:
			*err = DB_SUCCESS;
			break;
		default:
			return nullptr;
		}

		const page_zip_des_t*	page_zip
			= buf_block_get_page_zip(block);
		ut_a(father_page_zip);
		ut_a(page_zip);

		/* Copy the page byte for byte. */
		page_zip_copy_recs(father_block,
				   page_zip, page, index, mtr);

		/* Update the lock table and possible hash index. */

		if (index->has_locking()) {
			lock_move_rec_list_end(father_block, block,
					       page_get_infimum_rec(page));
		}

		/* Also update the predicate locks */
		if (dict_index_is_spatial(index)) {
			lock_prdt_rec_move(father_block, block->page.id());
		} else {
			btr_search_move_or_delete_hash_entries(
				father_block, block);
		}
	}

copied:
	if (index->has_locking()) {
		const page_id_t id{block->page.id()};
		/* Free predicate page locks on the block */
		if (index->is_spatial()) {
			lock_sys.prdt_page_free_from_discard(id);
		} else {
			lock_update_copy_and_discard(*father_block, id);
		}
	}

	page_level++;

	/* Go upward to root page, decrementing levels by one. */
	for (i = lift_father_up ? 1 : 0; i < n_blocks; i++, page_level++) {
		ut_ad(btr_page_get_level(blocks[i]->page.frame)
		      == page_level + 1);
		btr_page_set_level(blocks[i], page_level, mtr);
	}

	if (dict_index_is_spatial(index)) {
		rtr_check_discard_page(index, NULL, block);
	}

	/* Free the file page */
	btr_page_free(index, block, mtr);

	ut_ad(page_validate(father_block->page.frame, index));
	ut_ad(btr_check_node_ptr(index, father_block, thr, mtr));

	return(lift_father_up ? block_orig : father_block);
}

/*************************************************************//**
Tries to merge the page first to the left immediate brother if such a
brother exists, and the node pointers to the current page and to the brother
reside on the same page. If the left brother does not satisfy these
conditions, looks at the right brother. If the page is the only one on that
level lifts the records of the page to the father page, thus reducing the
tree height. It is assumed that mtr holds an x-latch on the tree and on the
page. If cursor is on the leaf level, mtr must also hold x-latches to the
brothers, if they exist.
@return error code */
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
{
	dict_index_t*	index;
	buf_block_t*	merge_block = nullptr;
	page_t*		merge_page = nullptr;
	page_zip_des_t*	merge_page_zip;
	ibool		is_left;
	buf_block_t*	block;
	page_t*		page;
	btr_cur_t	father_cursor;
	mem_heap_t*	heap;
	rec_offs*	offsets;
	ulint		nth_rec = 0; /* remove bogus warning */
	bool		mbr_changed = false;
#ifdef UNIV_DEBUG
	bool		leftmost_child;
#endif
	DBUG_ENTER("btr_compress");

	block = btr_cur_get_block(cursor);
	page = btr_cur_get_page(cursor);
	index = btr_cur_get_index(cursor);

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));

	MONITOR_INC(MONITOR_INDEX_MERGE_ATTEMPTS);

	const uint32_t left_page_no = btr_page_get_prev(page);
	const uint32_t right_page_no = btr_page_get_next(page);
	dberr_t err = DB_SUCCESS;

	ut_ad(page_is_leaf(page) || left_page_no != FIL_NULL
	      || (REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
			  page_rec_get_next(page_get_infimum_rec(page)),
			  page_is_comp(page))));

	heap = mem_heap_create(100);
	father_cursor.page_cur.index = index;
	father_cursor.page_cur.block = block;

	if (index->is_spatial()) {
		ut_ad(cursor->rtr_info);
		offsets = rtr_page_get_father_block(
			nullptr, heap, cursor, &father_cursor,
			cursor->rtr_info->thr, mtr);
		ut_ad(cursor->page_cur.block->page.id() == block->page.id());
		rec_t*  my_rec = father_cursor.page_cur.rec;

		ulint page_no = btr_node_ptr_get_child_page_no(my_rec, offsets);

		if (page_no != block->page.id().page_no()) {
			ib::info() << "father positioned on page "
				<< page_no << "instead of "
				<< block->page.id().page_no();
			goto get_offsets;
		}
	} else {
get_offsets:
		offsets = btr_page_get_father_block(
			NULL, heap, mtr, &father_cursor);
	}

	if (UNIV_UNLIKELY(!offsets)) {
		goto corrupted;
	}

	if (adjust) {
		nth_rec = page_rec_get_n_recs_before(btr_cur_get_rec(cursor));
		if (UNIV_UNLIKELY(!nth_rec || nth_rec == ULINT_UNDEFINED)) {
		corrupted:
			err = DB_CORRUPTION;
			goto err_exit;
		}
	}

	if (left_page_no == FIL_NULL && right_page_no == FIL_NULL) {
		/* The page is the only one on the level, lift the records
		to the father */

		merge_block = btr_lift_page_up(index, block,
					       cursor->rtr_info
					       ? cursor->rtr_info->thr
					       : nullptr, mtr, &err);
success:
		if (adjust) {
			ut_ad(nth_rec > 0);
			if (rec_t* nth
			    = page_rec_get_nth(merge_block->page.frame,
					       nth_rec)) {
				btr_cur_position(index, nth,
						 merge_block, cursor);
			} else {
				goto corrupted;
			}
		}

		MONITOR_INC(MONITOR_INDEX_MERGE_SUCCESSFUL);
err_exit:
		mem_heap_free(heap);
		DBUG_RETURN(err);
	}

	ut_d(leftmost_child =
		left_page_no != FIL_NULL
		&& (page_rec_get_next(
			page_get_infimum_rec(
				btr_cur_get_page(&father_cursor)))
		    == btr_cur_get_rec(&father_cursor)));

	/* Decide the page to which we try to merge and which will inherit
	the locks */

	is_left = btr_can_merge_with_page(cursor, left_page_no,
					  &merge_block, mtr);

	DBUG_EXECUTE_IF("ib_always_merge_right", is_left = FALSE;);
retry:
	if (!is_left
	   && !btr_can_merge_with_page(cursor, right_page_no, &merge_block,
				       mtr)) {
		if (!merge_block) {
			merge_page = NULL;
		}
cannot_merge:
		err = DB_FAIL;
		goto err_exit;
	}

	merge_page = buf_block_get_frame(merge_block);

	if (UNIV_UNLIKELY(memcmp_aligned<4>(merge_page + (is_left
							  ? FIL_PAGE_NEXT
							  : FIL_PAGE_PREV),
					    block->page.frame
					    + FIL_PAGE_OFFSET, 4))) {
		goto corrupted;
	}

	ut_ad(page_validate(merge_page, index));

	merge_page_zip = buf_block_get_page_zip(merge_block);
#ifdef UNIV_ZIP_DEBUG
	if (merge_page_zip) {
		const page_zip_des_t*	page_zip
			= buf_block_get_page_zip(block);
		ut_a(page_zip);
		ut_a(page_zip_validate(merge_page_zip, merge_page, index));
		ut_a(page_zip_validate(page_zip, page, index));
	}
#endif /* UNIV_ZIP_DEBUG */

	btr_cur_t cursor2;
	cursor2.page_cur.index = index;
	cursor2.page_cur.block = merge_block;

	/* Move records to the merge page */
	if (is_left) {
		rtr_mbr_t	new_mbr;
		rec_offs*	offsets2 = NULL;

		/* For rtree, we need to update father's mbr. */
		if (index->is_spatial()) {
			/* We only support merge pages with the same parent
			page */
			if (!rtr_check_same_block(
				index, &cursor2,
				btr_cur_get_block(&father_cursor), heap)) {
				is_left = false;
				goto retry;
			}

			/* Set rtr_info for cursor2, since it is
			necessary in recursive page merge. */
			cursor2.rtr_info = cursor->rtr_info;
			cursor2.tree_height = cursor->tree_height;

			offsets2 = rec_get_offsets(
				btr_cur_get_rec(&cursor2), index, NULL,
				page_is_leaf(btr_cur_get_page(&cursor2))
				? index->n_fields : 0,
				ULINT_UNDEFINED, &heap);

			/* Check if parent entry needs to be updated */
			mbr_changed = rtr_merge_mbr_changed(
				&cursor2, &father_cursor,
				offsets2, offsets, &new_mbr);
		}

		rec_t*	orig_pred = page_copy_rec_list_start(
			merge_block, block, page_get_supremum_rec(page),
			index, mtr, &err);

		if (!orig_pred) {
			goto err_exit;
		}

		btr_search_drop_page_hash_index(block, nullptr);

		/* Remove the page from the level list */
		err = btr_level_list_remove(*block, *index, mtr);

		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			goto err_exit;
		}

		const page_id_t id{block->page.id()};

		if (index->is_spatial()) {
			rec_t*  my_rec = father_cursor.page_cur.rec;

			ulint page_no = btr_node_ptr_get_child_page_no(
						my_rec, offsets);

			if (page_no != block->page.id().page_no()) {
				ib::fatal() << "father positioned on "
					<< page_no << " instead of "
					<< block->page.id().page_no();
			}

			if (mbr_changed) {
				rtr_update_mbr_field(
					&cursor2, offsets2, &father_cursor,
					merge_page, &new_mbr, NULL, mtr);
			} else {
				rtr_node_ptr_delete(&father_cursor, mtr);
			}

			/* No GAP lock needs to be worrying about */
			lock_sys.prdt_page_free_from_discard(id);
		} else {
			err = btr_cur_node_ptr_delete(&father_cursor, mtr);
			if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
				goto err_exit;
			}
			if (index->has_locking()) {
				lock_update_merge_left(
					*merge_block, orig_pred, id);
			}
		}

		if (adjust) {
			ulint n = page_rec_get_n_recs_before(orig_pred);
			if (UNIV_UNLIKELY(!n || n == ULINT_UNDEFINED)) {
				goto corrupted;
			}
			nth_rec += n;
		}
	} else {
		rec_t*		orig_succ;
		ibool		compressed;
		dberr_t		err;
		byte		fil_page_prev[4];

		if (index->is_spatial()) {
			/* For spatial index, we disallow merge of blocks
			with different parents, since the merge would need
			to update entry (for MBR and Primary key) in the
			parent of block being merged */
			if (!rtr_check_same_block(
				index, &cursor2,
				btr_cur_get_block(&father_cursor), heap)) {
				goto cannot_merge;
			}

			/* Set rtr_info for cursor2, since it is
			necessary in recursive page merge. */
			cursor2.rtr_info = cursor->rtr_info;
			cursor2.tree_height = cursor->tree_height;
		} else if (!btr_page_get_father(mtr, &cursor2)) {
			goto cannot_merge;
		}

		if (merge_page_zip && left_page_no == FIL_NULL) {

			/* The function page_zip_compress(), which will be
			invoked by page_copy_rec_list_end() below,
			requires that FIL_PAGE_PREV be FIL_NULL.
			Clear the field, but prepare to restore it. */
			static_assert(FIL_PAGE_PREV % 8 == 0, "alignment");
			memcpy(fil_page_prev, merge_page + FIL_PAGE_PREV, 4);
			compile_time_assert(FIL_NULL == 0xffffffffU);
			memset_aligned<4>(merge_page + FIL_PAGE_PREV, 0xff, 4);
		}

		orig_succ = page_copy_rec_list_end(merge_block, block,
						   page_get_infimum_rec(page),
						   cursor->index(), mtr, &err);

		if (!orig_succ) {
			ut_a(merge_page_zip);
			if (left_page_no == FIL_NULL) {
				/* FIL_PAGE_PREV was restored from
				merge_page_zip. */
				ut_ad(!memcmp(fil_page_prev,
					      merge_page + FIL_PAGE_PREV, 4));
			}
			goto err_exit;
		}

		btr_search_drop_page_hash_index(block, nullptr);

		if (merge_page_zip && left_page_no == FIL_NULL) {

			/* Restore FIL_PAGE_PREV in order to avoid an assertion
			failure in btr_level_list_remove(), which will set
			the field again to FIL_NULL.  Even though this makes
			merge_page and merge_page_zip inconsistent for a
			split second, it is harmless, because the pages
			are X-latched. */
			memcpy(merge_page + FIL_PAGE_PREV, fil_page_prev, 4);
		}

		/* Remove the page from the level list */
		err = btr_level_list_remove(*block, *index, mtr);

		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			goto err_exit;
		}

		ut_ad(btr_node_ptr_get_child_page_no(
			      btr_cur_get_rec(&father_cursor), offsets)
		      == block->page.id().page_no());

		/* Replace the address of the old child node (= page) with the
		address of the merge page to the right */
		btr_node_ptr_set_child_page_no(
			btr_cur_get_block(&father_cursor),
			btr_cur_get_rec(&father_cursor),
			offsets, right_page_no, mtr);

#ifdef UNIV_DEBUG
		if (!page_is_leaf(page) && left_page_no == FIL_NULL) {
			ut_ad(REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
				page_rec_get_next(page_get_infimum_rec(
					buf_block_get_frame(merge_block))),
				page_is_comp(page)));
		}
#endif /* UNIV_DEBUG */

		/* For rtree, we need to update father's mbr. */
		if (index->is_spatial()) {
			rec_offs* offsets2;
			ulint	rec_info;

			offsets2 = rec_get_offsets(
				btr_cur_get_rec(&cursor2), index, NULL,
				page_is_leaf(btr_cur_get_page(&cursor2))
				? index->n_fields : 0,
				ULINT_UNDEFINED, &heap);

			ut_ad(btr_node_ptr_get_child_page_no(
				btr_cur_get_rec(&cursor2), offsets2)
				== right_page_no);

			rec_info = rec_get_info_bits(
				btr_cur_get_rec(&father_cursor),
				rec_offs_comp(offsets));
			if (rec_info & REC_INFO_MIN_REC_FLAG) {
				/* When the father node ptr is minimal rec,
				we will keep it and delete the node ptr of
				merge page. */
				rtr_merge_and_update_mbr(&father_cursor,
							 &cursor2,
							 offsets, offsets2,
							 merge_page, mtr);
			} else {
				/* Otherwise, we will keep the node ptr of
				merge page and delete the father node ptr.
				This is for keeping the rec order in upper
				level. */
				rtr_merge_and_update_mbr(&cursor2,
							 &father_cursor,
							 offsets2, offsets,
							 merge_page, mtr);
			}
			const page_id_t id{block->page.id()};
			lock_sys.prdt_page_free_from_discard(id);
		} else {

			compressed = btr_cur_pessimistic_delete(&err, TRUE,
								&cursor2,
								BTR_CREATE_FLAG,
								false, mtr);
			ut_a(err == DB_SUCCESS);

			if (!compressed) {
				btr_cur_compress_if_useful(&cursor2, false,
							   mtr);
			}

			if (index->has_locking()) {
				lock_update_merge_right(
					merge_block, orig_succ, block);
			}
		}
	}

	ut_ad(page_validate(merge_page, index));
#ifdef UNIV_ZIP_DEBUG
	ut_a(!merge_page_zip || page_zip_validate(merge_page_zip, merge_page,
						  index));
#endif /* UNIV_ZIP_DEBUG */

	if (dict_index_is_spatial(index)) {
		rtr_check_discard_page(index, NULL, block);
	}

	/* Free the file page */
	err = btr_page_free(index, block, mtr);
        if (err == DB_SUCCESS) {
		ut_ad(leftmost_child
		      || btr_check_node_ptr(index, merge_block,
					    cursor->rtr_info
					    ? cursor->rtr_info->thr
					    : nullptr, mtr));
		goto success;
        } else {
		goto err_exit;
        }
}

/*************************************************************//**
Discards a page that is the only page on its level.  This will empty
the whole B-tree, leaving just an empty root page.  This function
should almost never be reached, because btr_compress(), which is invoked in
delete operations, calls btr_lift_page_up() to flatten the B-tree. */
ATTRIBUTE_COLD
static
void
btr_discard_only_page_on_level(
/*===========================*/
	btr_cur_t*	cur,	/*!< in: cursor on a page which is the
				only on its level */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dict_index_t* index = cur->index();
	buf_block_t* block = btr_cur_get_block(cur);
	ulint page_level = 0;

	ut_ad(!index->is_dummy);

	/* Save the PAGE_MAX_TRX_ID from the leaf page. */
	const trx_id_t max_trx_id = page_get_max_trx_id(block->page.frame);
	const rec_t* r = page_rec_get_next(
		page_get_infimum_rec(block->page.frame));
	/* In the caller we checked that a valid key exists in the page,
	because we were able to look up a parent page. */
	ut_ad(r);
	ut_ad(rec_is_metadata(r, *index) == index->is_instant());

	while (block->page.id().page_no() != dict_index_get_page(index)) {
		btr_cur_t	cursor;
		buf_block_t*	father;
		const page_t*	page	= buf_block_get_frame(block);

		ut_a(page_get_n_recs(page) == 1);
		ut_a(page_level == btr_page_get_level(page));
		ut_a(!page_has_siblings(page));
		ut_ad(fil_page_index_page_check(page));
		ut_ad(block->page.id().space() == index->table->space->id);
		ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
		btr_search_drop_page_hash_index(block, nullptr);
		cursor.page_cur.index = index;
		cursor.page_cur.block = block;

		if (index->is_spatial()) {
			/* Check any concurrent search having this page */
			rtr_check_discard_page(index, NULL, block);
			if (!rtr_page_get_father(mtr, nullptr, &cursor,
						 cur->rtr_info->thr)) {
				return;
			}
		} else {
			if (!btr_page_get_father(mtr, &cursor)) {
				return;
			}
		}
		father = btr_cur_get_block(&cursor);

		if (index->has_locking()) {
			lock_update_discard(
				father, PAGE_HEAP_NO_SUPREMUM, block);
		}

		/* Free the file page */
		if (btr_page_free(index, block, mtr) != DB_SUCCESS) {
			return;
		}

		block = father;
		page_level++;
	}

	/* block is the root page, which must be empty, except
	for the node pointer to the (now discarded) block(s). */
	ut_ad(!page_has_siblings(block->page.frame));

	mem_heap_t* heap = nullptr;
	const rec_t* rec = nullptr;
	rec_offs* offsets = nullptr;
	if (index->table->instant || index->must_avoid_clear_instant_add()) {
		if (!rec_is_metadata(r, *index)) {
		} else if (!index->table->instant
			   || rec_is_alter_metadata(r, *index)) {
			heap = mem_heap_create(srv_page_size);
			offsets = rec_get_offsets(r, index, nullptr,
						  index->n_core_fields,
						  ULINT_UNDEFINED, &heap);
			rec = rec_copy(mem_heap_alloc(heap,
						      rec_offs_size(offsets)),
				       r, offsets);
			rec_offs_make_valid(rec, index, true, offsets);
		}
	}

	btr_page_empty(block, buf_block_get_page_zip(block), index, 0, mtr);
	ut_ad(page_is_leaf(buf_block_get_frame(block)));
	/* btr_page_empty() is supposed to zero-initialize the field. */
	ut_ad(!page_get_instant(block->page.frame));

	if (index->is_primary()) {
		if (rec) {
			page_cur_t cur;
			page_cur_set_before_first(block, &cur);
			cur.index = index;
			DBUG_ASSERT(index->table->instant);
			DBUG_ASSERT(rec_is_alter_metadata(rec, *index));
			btr_set_instant(block, *index, mtr);
			rec = page_cur_insert_rec_low(&cur, rec, offsets, mtr);
			ut_ad(rec);
			mem_heap_free(heap);
		} else if (index->is_instant()) {
			index->clear_instant_add();
		}
	} else if (!index->table->is_temporary()) {
		ut_a(max_trx_id);
		page_set_max_trx_id(block,
				    buf_block_get_page_zip(block),
				    max_trx_id, mtr);
	}
}

/*************************************************************//**
Discards a page from a B-tree. This is used to remove the last record from
a B-tree page: the whole page must be removed at the same time. This cannot
be used for the root page, which is allowed to be empty. */
dberr_t
btr_discard_page(
/*=============*/
	btr_cur_t*	cursor,	/*!< in: cursor on the page to discard: not on
				the root page */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dict_index_t*	index;
	buf_block_t*	merge_block;
	buf_block_t*	block;
	btr_cur_t	parent_cursor;

	block = btr_cur_get_block(cursor);
	index = btr_cur_get_index(cursor);
	parent_cursor.page_cur = cursor->page_cur;

	ut_ad(dict_index_get_page(index) != block->page.id().page_no());

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));

	MONITOR_INC(MONITOR_INDEX_DISCARD);

	if (index->is_spatial()
	    ? !rtr_page_get_father(mtr, cursor, &parent_cursor,
				   cursor->rtr_info->thr)
	    : !btr_page_get_father(mtr, &parent_cursor)) {
		return DB_CORRUPTION;
	}

	/* Decide the page which will inherit the locks */

	const uint32_t left_page_no = btr_page_get_prev(block->page.frame);
	const uint32_t right_page_no = btr_page_get_next(block->page.frame);
	page_id_t merge_page_id{block->page.id()};

	ut_d(bool parent_is_different = false);
	dberr_t err;
	if (left_page_no != FIL_NULL) {
		merge_page_id.set_page_no(left_page_no);
		merge_block = btr_block_reget(mtr, *index, merge_page_id,
					      &err);
		if (UNIV_UNLIKELY(!merge_block)) {
			return err;
		}
#if 1 /* MDEV-29835 FIXME: Acquire the page latch upfront. */
		ut_ad(!memcmp_aligned<4>(merge_block->page.frame
					 + FIL_PAGE_NEXT,
					 block->page.frame + FIL_PAGE_OFFSET,
					 4));
#else
		if (UNIV_UNLIKELY(memcmp_aligned<4>(merge_block->page.frame
						    + FIL_PAGE_NEXT,
						    block->page.frame
						    + FIL_PAGE_OFFSET, 4))) {
			return DB_CORRUPTION;
		}
#endif
		ut_d(parent_is_different =
			(page_rec_get_next(
				page_get_infimum_rec(
					btr_cur_get_page(
						&parent_cursor)))
			 == btr_cur_get_rec(&parent_cursor)));
	} else if (right_page_no != FIL_NULL) {
		merge_page_id.set_page_no(right_page_no);
		merge_block = btr_block_reget(mtr, *index, merge_page_id,
                                              &err);
		if (UNIV_UNLIKELY(!merge_block)) {
			return err;
		}
#if 1 /* MDEV-29835 FIXME: Acquire the page latch upfront. */
		ut_ad(!memcmp_aligned<4>(merge_block->page.frame
					 + FIL_PAGE_PREV,
					 block->page.frame + FIL_PAGE_OFFSET,
					 4));
#else
		if (UNIV_UNLIKELY(memcmp_aligned<4>(merge_block->page.frame
						    + FIL_PAGE_PREV,
						    block->page.frame
						    + FIL_PAGE_OFFSET, 4))) {
			return DB_CORRUPTION;
		}
#endif
		ut_d(parent_is_different = page_rec_is_supremum(
			page_rec_get_next(btr_cur_get_rec(&parent_cursor))));
		if (page_is_leaf(merge_block->page.frame)) {
		} else if (rec_t* node_ptr =
                           page_rec_get_next(page_get_infimum_rec(
					   merge_block->page.frame))) {
			ut_ad(page_rec_is_user_rec(node_ptr));
			/* We have to mark the leftmost node pointer as the
			predefined minimum record. */
			btr_set_min_rec_mark<true>(node_ptr, *merge_block,
						   mtr);
		} else {
			return DB_CORRUPTION;
		}
	} else {
		btr_discard_only_page_on_level(cursor, mtr);
		return DB_SUCCESS;
	}

	if (UNIV_UNLIKELY(memcmp_aligned<2>(&merge_block->page.frame
					    [PAGE_HEADER + PAGE_LEVEL],
					    &block->page.frame
					    [PAGE_HEADER + PAGE_LEVEL], 2))) {
		return DB_CORRUPTION;
	}

	btr_search_drop_page_hash_index(block, nullptr);

	if (dict_index_is_spatial(index)) {
		rtr_node_ptr_delete(&parent_cursor, mtr);
	} else if (dberr_t err =
		   btr_cur_node_ptr_delete(&parent_cursor, mtr)) {
		return err;
	}

	/* Remove the page from the level list */
	if (dberr_t err = btr_level_list_remove(*block, *index, mtr)) {
		return err;
	}

#ifdef UNIV_ZIP_DEBUG
	if (page_zip_des_t* merge_page_zip
	    = buf_block_get_page_zip(merge_block))
		ut_a(page_zip_validate(merge_page_zip,
				       merge_block->page.frame, index));
#endif /* UNIV_ZIP_DEBUG */

	if (index->has_locking()) {
		if (left_page_no != FIL_NULL) {
			lock_update_discard(merge_block, PAGE_HEAP_NO_SUPREMUM,
					    block);
		} else {
			lock_update_discard(merge_block,
					    lock_get_min_heap_no(merge_block),
					    block);
		}

		if (index->is_spatial()) {
			rtr_check_discard_page(index, cursor, block);
		}
	}

	/* Free the file page */
	err = btr_page_free(index, block, mtr);

	if (err == DB_SUCCESS) {
		/* btr_check_node_ptr() needs parent block latched.
		If the merge_block's parent block is not same,
		we cannot use btr_check_node_ptr() */
		ut_ad(parent_is_different
		      || btr_check_node_ptr(index, merge_block,
					    cursor->rtr_info
					    ? cursor->rtr_info->thr
					    : nullptr, mtr));

		if (btr_cur_get_block(&parent_cursor)->page.id().page_no()
		    == index->page
		    && !page_has_siblings(btr_cur_get_page(&parent_cursor))
		    && page_get_n_recs(btr_cur_get_page(&parent_cursor))
		    == 1) {
			btr_lift_page_up(index, merge_block,
					 cursor->rtr_info
					 ? cursor->rtr_info->thr
					 : nullptr, mtr, &err);
		}
	}

	return err;
}

#ifdef UNIV_BTR_PRINT
/*************************************************************//**
Prints size info of a B-tree. */
void
btr_print_size(
/*===========*/
	dict_index_t*	index)	/*!< in: index tree */
{
	page_t*		root;
	fseg_header_t*	seg;
	mtr_t		mtr;

	mtr_start(&mtr);

	root = btr_root_get(index, &mtr);

	seg = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	fputs("INFO OF THE NON-LEAF PAGE SEGMENT\n", stderr);
	fseg_print(seg, &mtr);

	seg = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;

	fputs("INFO OF THE LEAF PAGE SEGMENT\n", stderr);
	fseg_print(seg, &mtr);

	mtr_commit(&mtr);
}

/************************************************************//**
Prints recursively index tree pages. */
static
void
btr_print_recursive(
/*================*/
	dict_index_t*	index,	/*!< in: index tree */
	buf_block_t*	block,	/*!< in: index page */
	ulint		width,	/*!< in: print this many entries from start
				and end */
	mem_heap_t**	heap,	/*!< in/out: heap for rec_get_offsets() */
	rec_offs**	offsets,/*!< in/out: buffer for rec_get_offsets() */
	mtr_t*		mtr)	/*!< in: mtr */
{
	const page_t*	page	= buf_block_get_frame(block);
	page_cur_t	cursor;
	ulint		n_recs;
	ulint		i	= 0;
	mtr_t		mtr2;

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_SX_FIX));

	ib::info() << "NODE ON LEVEL " << btr_page_get_level(page)
		<< " page " << block->page.id;

	page_print(block, index, width, width);

	n_recs = page_get_n_recs(page);

	page_cur_set_before_first(block, &cursor);
	page_cur_move_to_next(&cursor);

	while (!page_cur_is_after_last(&cursor)) {

		if (page_is_leaf(page)) {

			/* If this is the leaf level, do nothing */

		} else if ((i <= width) || (i >= n_recs - width)) {

			const rec_t*	node_ptr;

			mtr_start(&mtr2);

			node_ptr = page_cur_get_rec(&cursor);

			*offsets = rec_get_offsets(
				node_ptr, index, *offsets, 0,
				ULINT_UNDEFINED, heap);
			if (buf_block_t *child =
			    btr_node_ptr_get_child(node_ptr, index, *offsets,
						   &mtr2)) {
				btr_print_recursive(index, child, width, heap,
						    offsets, &mtr2);
			}
			mtr_commit(&mtr2);
		}

		page_cur_move_to_next(&cursor);
		i++;
	}
}

/**************************************************************//**
Prints directories and other info of all nodes in the tree. */
void
btr_print_index(
/*============*/
	dict_index_t*	index,	/*!< in: index */
	ulint		width)	/*!< in: print this many entries from start
				and end */
{
	mtr_t		mtr;
	buf_block_t*	root;
	mem_heap_t*	heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets	= offsets_;
	rec_offs_init(offsets_);

	fputs("--------------------------\n"
	      "INDEX TREE PRINT\n", stderr);

	mtr_start(&mtr);

	root = btr_root_block_get(index, RW_SX_LATCH, &mtr);

	btr_print_recursive(index, root, width, &heap, &offsets, &mtr);
	if (heap) {
		mem_heap_free(heap);
	}

	mtr_commit(&mtr);

	ut_ad(btr_validate_index(index, 0));
}
#endif /* UNIV_BTR_PRINT */

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
{
	mem_heap_t*	heap;
	dtuple_t*	tuple;
	rec_offs*	offsets;
	btr_cur_t	cursor;
	page_t*		page = buf_block_get_frame(block);

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));

	if (dict_index_get_page(index) == block->page.id().page_no()) {

		return(TRUE);
	}

	cursor.page_cur.index = index;
	cursor.page_cur.block = block;

	heap = mem_heap_create(256);

	if (dict_index_is_spatial(index)) {
		offsets = rtr_page_get_father_block(NULL, heap,
						    NULL, &cursor, thr, mtr);
	} else {
		offsets = btr_page_get_father_block(NULL, heap, mtr, &cursor);
	}

	ut_ad(offsets);

	if (page_is_leaf(page)) {

		goto func_exit;
	}

	tuple = dict_index_build_node_ptr(
		index, page_rec_get_next(page_get_infimum_rec(page)), 0, heap,
		btr_page_get_level(page));

	/* For spatial index, the MBR in the parent rec could be different
	with that of first rec of child, their relationship should be
	"WITHIN" relationship */
	if (dict_index_is_spatial(index)) {
		ut_a(!cmp_dtuple_rec_with_gis(
			tuple, btr_cur_get_rec(&cursor),
			PAGE_CUR_WITHIN));
	} else {
		ut_a(!cmp_dtuple_rec(tuple, btr_cur_get_rec(&cursor), index,
				     offsets));
	}
func_exit:
	mem_heap_free(heap);

	return(TRUE);
}
#endif /* UNIV_DEBUG */

/************************************************************//**
Display identification information for a record. */
static
void
btr_index_rec_validate_report(
/*==========================*/
	const page_t*		page,	/*!< in: index page */
	const rec_t*		rec,	/*!< in: index record */
	const dict_index_t*	index)	/*!< in: index */
{
	ib::info() << "Record in index " << index->name
		<< " of table " << index->table->name
		<< ", page " << page_id_t(page_get_space_id(page),
					  page_get_page_no(page))
		<< ", at offset " << rec - page;
}

/************************************************************//**
Checks the size and number of fields in a record based on the definition of
the index.
@return TRUE if ok */
bool
btr_index_rec_validate(
/*===================*/
	const page_cur_t&	cur,		/*!< in: cursor to index record */
	const dict_index_t*	index,		/*!< in: index */
	bool			dump_on_error)	/*!< in: true if the function
						should print hex dump of record
						and page on error */
	noexcept
{
	ulint		len;
	const rec_t*	rec = page_cur_get_rec(&cur);
	const page_t*	page = cur.block->page.frame;
	mem_heap_t*	heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets	= offsets_;
	rec_offs_init(offsets_);

	ut_ad(index->n_core_fields);

#ifdef VIRTUAL_INDEX_DEBUG
	if (dict_index_has_virtual(index)) {
		fprintf(stderr, "index name is %s\n", index->name());
	}
#endif
	if ((ibool)!!page_is_comp(page) != dict_table_is_comp(index->table)) {
		btr_index_rec_validate_report(page, rec, index);

		ib::error() << "Compact flag=" << !!page_is_comp(page)
			<< ", should be " << dict_table_is_comp(index->table);

		return(FALSE);
	}

	const bool is_alter_metadata = page_is_leaf(page)
		&& !page_has_prev(page)
		&& index->is_primary() && index->table->instant
		&& rec == page_rec_get_next_const(page_get_infimum_rec(page));

	if (is_alter_metadata
	    && !rec_is_alter_metadata(rec, page_is_comp(page))) {
		btr_index_rec_validate_report(page, rec, index);

		ib::error() << "First record is not ALTER TABLE metadata";
		return FALSE;
	}

	if (!page_is_comp(page)) {
		const ulint n_rec_fields = rec_get_n_fields_old(rec);
		if (n_rec_fields == DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD
		    && index->id == DICT_INDEXES_ID) {
			/* A record for older SYS_INDEXES table
			(missing merge_threshold column) is acceptable. */
		} else if (is_alter_metadata) {
			if (n_rec_fields != ulint(index->n_fields) + 1) {
				goto n_field_mismatch;
			}
		} else if (n_rec_fields < index->n_core_fields
			   || n_rec_fields > index->n_fields) {
n_field_mismatch:
			btr_index_rec_validate_report(page, rec, index);

			ib::error() << "Has " << rec_get_n_fields_old(rec)
				    << " fields, should have "
				    << index->n_core_fields << ".."
				    << index->n_fields;

			if (dump_on_error) {
				fputs("InnoDB: corrupt record ", stderr);
				rec_print_old(stderr, rec);
				putc('\n', stderr);
			}
			return(FALSE);
		}
	}

	offsets = rec_get_offsets(rec, index, offsets, page_is_leaf(page)
				  ? index->n_core_fields : 0,
				  ULINT_UNDEFINED, &heap);
	const dict_field_t* field = index->fields;
	ut_ad(rec_offs_n_fields(offsets)
	      == ulint(index->n_fields) + is_alter_metadata);

	for (unsigned i = 0; i < rec_offs_n_fields(offsets); i++) {
		rec_get_nth_field_offs(offsets, i, &len);

		ulint fixed_size;

		if (is_alter_metadata && i == index->first_user_field()) {
			fixed_size = FIELD_REF_SIZE;
			if (len != FIELD_REF_SIZE
			    || !rec_offs_nth_extern(offsets, i)) {
				goto len_mismatch;
			}

			continue;
		} else {
			fixed_size = dict_col_get_fixed_size(
				field->col, page_is_comp(page));
			if (rec_offs_nth_extern(offsets, i)) {
				const byte* data = rec_get_nth_field(
					rec, offsets, i, &len);
				len -= BTR_EXTERN_FIELD_REF_SIZE;
				ulint extern_len = mach_read_from_4(
					data + len + BTR_EXTERN_LEN + 4);
				if (fixed_size == extern_len + len) {
					goto next_field;
				}
			}
		}

		/* Note that if fixed_size != 0, it equals the
		length of a fixed-size column in the clustered index.
		We should adjust it here.
		A prefix index of the column is of fixed, but different
		length.  When fixed_size == 0, prefix_len is the maximum
		length of the prefix index column. */

		if (len_is_stored(len)
		    && (field->prefix_len
			? len > field->prefix_len
			: (fixed_size && len != fixed_size))) {
len_mismatch:
			btr_index_rec_validate_report(page, rec, index);
			ib::error	error;

			error << "Field " << i << " len is " << len
				<< ", should be " << fixed_size;

			if (dump_on_error) {
				error << "; ";
				rec_print(error.m_oss, rec,
					  rec_get_info_bits(
						  rec, rec_offs_comp(offsets)),
					  offsets);
			}
			if (heap) {
				mem_heap_free(heap);
			}
			return(FALSE);
		}
next_field:
		field++;
	}

#ifdef VIRTUAL_INDEX_DEBUG
	if (dict_index_has_virtual(index)) {
		rec_print_new(stderr, rec, offsets);
	}
#endif

	if (heap) {
		mem_heap_free(heap);
	}
	return(TRUE);
}

/************************************************************//**
Checks the size and number of fields in records based on the definition of
the index.
@return true if ok */
static
bool
btr_index_page_validate(
/*====================*/
	buf_block_t*	block,	/*!< in: index page */
	dict_index_t*	index)	/*!< in: index */
{
	page_cur_t	cur;
#ifndef DBUG_OFF
	ulint		nth	= 1;
#endif /* !DBUG_OFF */

	page_cur_set_before_first(block, &cur);

	/* Directory slot 0 should only contain the infimum record. */
	DBUG_EXECUTE_IF("check_table_rec_next",
			ut_a(page_rec_get_nth_const(
				     page_cur_get_page(&cur), 0)
			     == cur.rec);
			ut_a(page_dir_slot_get_n_owned(
				     page_dir_get_nth_slot(
					     page_cur_get_page(&cur), 0))
			     == 1););

	while (page_cur_move_to_next(&cur)) {
		if (page_cur_is_after_last(&cur)) {
			return true;
		}

		if (!btr_index_rec_validate(cur, index, TRUE)) {
			break;
		}

		/* Verify that page_rec_get_nth_const() is correctly
		retrieving each record. */
		DBUG_EXECUTE_IF("check_table_rec_next",
				ut_a(cur.rec == page_rec_get_nth_const(
					     page_cur_get_page(&cur),
					     page_rec_get_n_recs_before(
						     cur.rec)));
				ut_a(nth++ == page_rec_get_n_recs_before(
					     cur.rec)););
	}

	return false;
}

/************************************************************//**
Report an error on one page of an index tree. */
static
void
btr_validate_report1(
/*=================*/
	dict_index_t*		index,	/*!< in: index */
	ulint			level,	/*!< in: B-tree level */
	const buf_block_t*	block)	/*!< in: index page */
{
	ib::error	error;
	error << "In page " << block->page.id().page_no()
		<< " of index " << index->name
		<< " of table " << index->table->name;

	if (level > 0) {
		error << ", index tree level " << level;
	}
}

/************************************************************//**
Report an error on two pages of an index tree. */
static
void
btr_validate_report2(
/*=================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			level,	/*!< in: B-tree level */
	const buf_block_t*	block1,	/*!< in: first index page */
	const buf_block_t*	block2)	/*!< in: second index page */
{
  ib::error error;
  error << "In pages " << block1->page.id()
	<< " and " << block2->page.id() << " of index " << index->name
	<< " of table " << index->table->name;

  if (level)
    error << ", index tree level " << level;
}

/** Validate an index tree level. */
static
dberr_t
btr_validate_level(
/*===============*/
	dict_index_t*	index,	/*!< in: index tree */
	const trx_t*	trx,	/*!< in: transaction or NULL */
	ulint		level)	/*!< in: level number */
{
	buf_block_t*	block;
	page_t*		page;
	buf_block_t*	right_block = 0; /* remove warning */
	page_t*		right_page = 0; /* remove warning */
	page_t*		father_page;
	btr_cur_t	node_cur;
	btr_cur_t	right_node_cur;
	rec_t*		rec;
	page_cur_t	cursor;
	dtuple_t*	node_ptr_tuple;
	mtr_t		mtr;
	mem_heap_t*	heap	= mem_heap_create(256);
	rec_offs*	offsets	= NULL;
	rec_offs*	offsets2= NULL;
#ifdef UNIV_ZIP_DEBUG
	page_zip_des_t*	page_zip;
#endif /* UNIV_ZIP_DEBUG */

	mtr.start();

	mtr_x_lock_index(index, &mtr);

	dberr_t err;
	block = btr_root_block_get(index, RW_SX_LATCH, &mtr, &err);
	if (!block) {
		mtr.commit();
		return err;
	}
	page = buf_block_get_frame(block);

	fil_space_t*		space	= index->table->space;

	while (level != btr_page_get_level(page)) {
		const rec_t*	node_ptr;
		switch (dberr_t e =
			fseg_page_is_allocated(space,
					       block->page.id().page_no())) {
		case DB_SUCCESS_LOCKED_REC:
			break;
		case DB_SUCCESS:
			btr_validate_report1(index, level, block);
			ib::warn() << "Page is free";
			e = DB_CORRUPTION;
			/* fall through */
		default:
			err = e;
		}
		ut_ad(index->table->space_id == block->page.id().space());
		ut_ad(block->page.id().space() == page_get_space_id(page));
#ifdef UNIV_ZIP_DEBUG
		page_zip = buf_block_get_page_zip(block);
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
		if (page_is_leaf(page)) {
corrupted:
			err = DB_CORRUPTION;
			goto invalid_page;
		}

		page_cur_set_before_first(block, &cursor);
		if (!(node_ptr = page_cur_move_to_next(&cursor))) {
			goto corrupted;
		}

		offsets = rec_get_offsets(node_ptr, index, offsets, 0,
					  ULINT_UNDEFINED, &heap);

		block = btr_node_ptr_get_child(node_ptr, index, offsets, &mtr,
					       &err);
		if (!block) {
			break;
		}
		page = buf_block_get_frame(block);

		/* For R-Tree, since record order might not be the same as
		linked index page in the lower level, we need to travers
		backwards to get the first page rec in this level.
		This is only used for index validation. Spatial index
		does not use such scan for any of its DML or query
		operations  */
		if (dict_index_is_spatial(index)) {
			uint32_t left_page_no = btr_page_get_prev(page);

			while (left_page_no != FIL_NULL) {
				/* To obey latch order of tree blocks,
				we should release the right_block once to
				obtain lock of the uncle block. */
				mtr.release_last_page();

				block = btr_block_get(*index, left_page_no,
						      RW_SX_LATCH, &mtr, &err);
				if (!block) {
					goto invalid_page;
				}
				page = buf_block_get_frame(block);
				left_page_no = btr_page_get_prev(page);
			}
		}
	}

	/* Now we are on the desired level. Loop through the pages on that
	level. */

loop:
	if (!block) {
invalid_page:
		mtr.commit();
func_exit:
		mem_heap_free(heap);
		return err;
	}

	mem_heap_empty(heap);
	offsets = offsets2 = NULL;

	mtr_x_lock_index(index, &mtr);

	page = block->page.frame;

#ifdef UNIV_ZIP_DEBUG
	page_zip = buf_block_get_page_zip(block);
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	if (DB_SUCCESS_LOCKED_REC
	    != fseg_page_is_allocated(space, block->page.id().page_no())) {
		btr_validate_report1(index, level, block);

		ib::warn() << "Page is marked as free";
		err = DB_CORRUPTION;
	} else if (btr_page_get_index_id(page) != index->id) {
		ib::error() << "Page index id " << btr_page_get_index_id(page)
			<< " != data dictionary index id " << index->id;
		err = DB_CORRUPTION;
	} else if (!page_validate(page, index)) {
		btr_validate_report1(index, level, block);
		err = DB_CORRUPTION;
	} else if (btr_page_get_level(page) != level) {
		btr_validate_report1(index, level, block);
		ib::error() << "Page level is not " << level;
		err = DB_CORRUPTION;
	} else if (level == 0 && !btr_index_page_validate(block, index)) {
		/* We are on level 0. Check that the records have the right
		number of fields, and field lengths are right. */
		err = DB_CORRUPTION;
	} else if (!page_is_empty(page)) {
	} else if (level) {
		btr_validate_report1(index, level, block);
		ib::error() << "Non-leaf page is empty";
	} else if (block->page.id().page_no() != index->page) {
		btr_validate_report1(index, level, block);
		ib::error() << "Empty leaf page is not index root";
	}

	uint32_t right_page_no = btr_page_get_next(page);
	uint32_t left_page_no = btr_page_get_prev(page);

	if (right_page_no != FIL_NULL) {
		const rec_t*	right_rec;

		right_block = btr_block_get(*index, right_page_no, RW_SX_LATCH,
					    &mtr, &err);
		if (!right_block) {
			btr_validate_report1(index, level, block);
			fputs("InnoDB: broken FIL_PAGE_NEXT link\n", stderr);
			goto invalid_page;
		}
		right_page = buf_block_get_frame(right_block);

		if (btr_page_get_prev(right_page) != page_get_page_no(page)) {
			btr_validate_report2(index, level, block, right_block);
			fputs("InnoDB: broken FIL_PAGE_NEXT"
			      " or FIL_PAGE_PREV links\n", stderr);
                        err = DB_CORRUPTION;
		}

		if (!(rec = page_rec_get_prev(page_get_supremum_rec(page)))) {
broken_links:
			btr_validate_report1(index, level, block);
			fputs("InnoDB: broken record links\n", stderr);
			goto invalid_page;
		}
		if (!(right_rec =
		      page_rec_get_next(page_get_infimum_rec(right_page)))) {
			goto broken_links;
		}

		offsets = rec_get_offsets(rec, index, offsets,
					  page_is_leaf(page)
					  ? index->n_core_fields : 0,
					  ULINT_UNDEFINED, &heap);
		offsets2 = rec_get_offsets(right_rec, index, offsets2,
					   page_is_leaf(right_page)
					   ? index->n_core_fields : 0,
					   ULINT_UNDEFINED, &heap);

		/* For spatial index, we cannot guarantee the key ordering
		across pages, so skip the record compare verification for
		now. Will enhanced in special R-Tree index validation scheme */
		if (index->is_btree()
		    && cmp_rec_rec(rec, right_rec,
				   offsets, offsets2, index) >= 0) {

			btr_validate_report2(index, level, block, right_block);

			fputs("InnoDB: records in wrong order"
			      " on adjacent pages\n", stderr);

			rec = page_rec_get_prev(page_get_supremum_rec(page));
			if (rec) {
				fputs("InnoDB: record ", stderr);
				rec_print(stderr, rec, index);
				putc('\n', stderr);
			}
			fputs("InnoDB: record ", stderr);
			rec = page_rec_get_next(
				page_get_infimum_rec(right_page));
			if (rec) {
				rec_print(stderr, rec, index);
			}
			putc('\n', stderr);
			err = DB_CORRUPTION;
		}
	}

	if (!level || left_page_no != FIL_NULL) {
	} else if (const rec_t* first =
		   page_rec_get_next_const(page_get_infimum_rec(page))) {
		if (!(REC_INFO_MIN_REC_FLAG
		      & rec_get_info_bits(first, page_is_comp(page)))) {
			btr_validate_report1(index, level, block);
			ib::error() << "Missing REC_INFO_MIN_REC_FLAG";
			err = DB_CORRUPTION;
		}
	} else {
		err = DB_CORRUPTION;
		goto node_ptr_fails;
	}

	/* Similarly skip the father node check for spatial index for now,
	for a couple of reasons:
	1) As mentioned, there is no ordering relationship between records
	in parent level and linked pages in the child level.
	2) Search parent from root is very costly for R-tree.
	We will add special validation mechanism for R-tree later (WL #7520) */
	if (index->is_btree() && block->page.id().page_no() != index->page) {
		/* Check father node pointers */
		rec_t*	node_ptr
			= page_rec_get_next(page_get_infimum_rec(page));
		if (!node_ptr) {
			err = DB_CORRUPTION;
			goto node_ptr_fails;
		}

		btr_cur_position(index, node_ptr, block, &node_cur);
		offsets = btr_page_get_father_node_ptr_for_validate(
			offsets, heap, &node_cur, &mtr);

		father_page = btr_cur_get_page(&node_cur);
		node_ptr = btr_cur_get_rec(&node_cur);

		rec = page_rec_get_prev(page_get_supremum_rec(page));
		if (rec) {
			btr_cur_position(index, rec, block, &node_cur);

			offsets = btr_page_get_father_node_ptr_for_validate(
				offsets, heap, &node_cur, &mtr);
		} else {
			offsets = nullptr;
		}

		if (!offsets || node_ptr != btr_cur_get_rec(&node_cur)
		    || btr_node_ptr_get_child_page_no(node_ptr, offsets)
		    != block->page.id().page_no()) {

			btr_validate_report1(index, level, block);

			fputs("InnoDB: node pointer to the page is wrong\n",
			      stderr);

			fputs("InnoDB: node ptr ", stderr);
			rec_print(stderr, node_ptr, index);

			if (offsets) {
				rec = btr_cur_get_rec(&node_cur);
				fprintf(stderr, "\n"
					"InnoDB: node ptr child page n:o %u\n",
					btr_node_ptr_get_child_page_no(
						rec, offsets));
				fputs("InnoDB: record on page ", stderr);
				rec_print_new(stderr, rec, offsets);
				putc('\n', stderr);
			}

			err = DB_CORRUPTION;
			goto node_ptr_fails;
		}

		if (page_is_leaf(page)) {
		} else if (const rec_t* first_rec =
			   page_rec_get_next(page_get_infimum_rec(page))) {
			node_ptr_tuple = dict_index_build_node_ptr(
				index, first_rec,
				0, heap, btr_page_get_level(page));

			if (cmp_dtuple_rec(node_ptr_tuple, node_ptr, index,
					   offsets)) {
				btr_validate_report1(index, level, block);

				ib::error() << "Node ptrs differ on levels > 0";

				fputs("InnoDB: node ptr ",stderr);
				rec_print_new(stderr, node_ptr, offsets);
				fputs("InnoDB: first rec ", stderr);
				rec_print(stderr, first_rec, index);
				putc('\n', stderr);
				err = DB_CORRUPTION;
				goto node_ptr_fails;
			}
		} else {
			err = DB_CORRUPTION;
			goto node_ptr_fails;
		}

		if (left_page_no == FIL_NULL) {
			if (page_has_prev(father_page)
			    || node_ptr != page_rec_get_next(
				     page_get_infimum_rec(father_page))) {
				err = DB_CORRUPTION;
				goto node_ptr_fails;
			}
		}

		if (right_page_no == FIL_NULL) {
			if (page_has_next(father_page)
			    || node_ptr != page_rec_get_prev(
				     page_get_supremum_rec(father_page))) {
				err = DB_CORRUPTION;
				goto node_ptr_fails;
			}
		} else if (const rec_t* right_node_ptr
			   = page_rec_get_next(node_ptr)) {
			btr_cur_position(
				index,
				page_get_infimum_rec(right_block->page.frame),
				right_block, &right_node_cur);
			if (!page_cur_move_to_next(&right_node_cur.page_cur)) {
				goto node_pointer_corrupted;
			}

			offsets = btr_page_get_father_node_ptr_for_validate(
					offsets, heap, &right_node_cur, &mtr);

			if (right_node_ptr
			    != page_get_supremum_rec(father_page)) {

				if (btr_cur_get_rec(&right_node_cur)
				    != right_node_ptr) {
node_pointer_corrupted:
					err = DB_CORRUPTION;
					fputs("InnoDB: node pointer to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);
				}
			} else {
				page_t*	right_father_page
					= btr_cur_get_page(&right_node_cur);

				if (btr_cur_get_rec(&right_node_cur)
				    != page_rec_get_next(
					    page_get_infimum_rec(
						    right_father_page))) {
					err = DB_CORRUPTION;
					fputs("InnoDB: node pointer 2 to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);
				}

				if (page_get_page_no(right_father_page)
				    != btr_page_get_next(father_page)) {

					err = DB_CORRUPTION;
					fputs("InnoDB: node pointer 3 to"
					      " the right page is wrong\n",
					      stderr);

					btr_validate_report1(index, level,
							     block);
				}
			}
		} else {
			err = DB_CORRUPTION;
		}
	}

node_ptr_fails:
	/* Commit the mini-transaction to release the latch on 'page'.
	Re-acquire the latch on right_page, which will become 'page'
	on the next loop.  The page has already been checked. */
	mtr.commit();

	if (trx_is_interrupted(trx)) {
		/* On interrupt, return the current status. */
	} else if (right_page_no != FIL_NULL) {

		mtr.start();

		block = btr_block_get(*index, right_page_no, RW_SX_LATCH,
				      &mtr, &err);
		goto loop;
	}

	goto func_exit;
}

/**************************************************************//**
Checks the consistency of an index tree.
@return	DB_SUCCESS if ok, error code if not */
dberr_t
btr_validate_index(
/*===============*/
	dict_index_t*	index,	/*!< in: index */
	const trx_t*	trx)	/*!< in: transaction or NULL */
{
  mtr_t mtr;
  mtr.start();

  mtr_x_lock_index(index, &mtr);

  dberr_t err;
  if (page_t *root= btr_root_get(index, &mtr, &err))
    for (auto level= btr_page_get_level(root);; level--)
    {
      if (dberr_t err_level= btr_validate_level(index, trx, level))
        err= err_level;
      if (!level)
        break;
    }

  mtr.commit();
  return err;
}

/**************************************************************//**
Checks if the page in the cursor can be merged with given page.
If necessary, re-organize the merge_page.
@return	true if possible to merge. */
static
bool
btr_can_merge_with_page(
/*====================*/
	btr_cur_t*	cursor,		/*!< in: cursor on the page to merge */
	uint32_t	page_no,	/*!< in: a sibling page */
	buf_block_t**	merge_block,	/*!< out: the merge block */
	mtr_t*		mtr)		/*!< in: mini-transaction */
{
	dict_index_t*	index;
	page_t*		page;
	ulint		n_recs;
	ulint		data_size;
	ulint		max_ins_size_reorg;
	ulint		max_ins_size;
	buf_block_t*	mblock;
	page_t*		mpage;
	DBUG_ENTER("btr_can_merge_with_page");

	if (page_no == FIL_NULL) {
error:
		*merge_block = NULL;
		DBUG_RETURN(false);
	}

	index = btr_cur_get_index(cursor);
	page = btr_cur_get_page(cursor);

	mblock = btr_block_get(*index, page_no, RW_X_LATCH, mtr);
	if (!mblock) {
		goto error;
	}
	mpage = buf_block_get_frame(mblock);

	n_recs = page_get_n_recs(page);
	data_size = page_get_data_size(page);

	max_ins_size_reorg = page_get_max_insert_size_after_reorganize(
		mpage, n_recs);

	if (data_size > max_ins_size_reorg) {
		goto error;
	}

	/* If compression padding tells us that merging will result in
	too packed up page i.e.: which is likely to cause compression
	failure then don't merge the pages. */
	if (mblock->page.zip.data && page_is_leaf(mpage)
	    && (page_get_data_size(mpage) + data_size
		>= dict_index_zip_pad_optimal_page_size(index))) {

		goto error;
	}

	max_ins_size = page_get_max_insert_size(mpage, n_recs);

	if (data_size > max_ins_size) {
		/* We have to reorganize mpage */
		if (btr_page_reorganize_block(page_zip_level, mblock, index,
					      mtr) != DB_SUCCESS) {
			goto error;
		}

		max_ins_size = page_get_max_insert_size(mpage, n_recs);

		ut_ad(page_validate(mpage, index));
		ut_ad(max_ins_size == max_ins_size_reorg);

		if (data_size > max_ins_size) {

			/* Add fault tolerance, though this should
			never happen */

			goto error;
		}
	}

	*merge_block = mblock;
	DBUG_RETURN(true);
}
