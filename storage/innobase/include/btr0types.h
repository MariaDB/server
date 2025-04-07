/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2023, MariaDB Corporation.

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

/********************************************************************//**
@file include/btr0types.h
The index tree general types

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#pragma once

#include "page0types.h"
#include "rem0types.h"

/** Persistent cursor */
struct btr_pcur_t;
/** B-tree cursor */
struct btr_cur_t;

/** The size of a reference to data stored on a different page.
The reference is stored at the end of the prefix of the field
in the index record. */
#define FIELD_REF_SIZE			20U
#define BTR_EXTERN_FIELD_REF_SIZE	FIELD_REF_SIZE

/** If the data don't exceed the size, the data are stored locally. */
#define BTR_EXTERN_LOCAL_STORED_MAX_SIZE	\
	(BTR_EXTERN_FIELD_REF_SIZE * 2)

/** Latching modes for btr_cur_t::search_leaf(). */
enum btr_latch_mode {
	/** Search a record on a leaf page and S-latch it. */
	BTR_SEARCH_LEAF = RW_S_LATCH,
	/** (Prepare to) modify a record on a leaf page and X-latch it. */
	BTR_MODIFY_LEAF	= RW_X_LATCH,
	/** U-latch root and X-latch a leaf page */
	BTR_MODIFY_ROOT_AND_LEAF = RW_SX_LATCH,
	/** Obtain no latches. */
	BTR_NO_LATCHES = RW_NO_LATCH,
	/** Search the previous record.
	Used in btr_pcur_move_backward_from_page(). */
	BTR_SEARCH_PREV = 4 | BTR_SEARCH_LEAF,
	/** Modify the previous record.
	Used in btr_pcur_move_backward_from_page() and ibuf_insert(). */
	BTR_MODIFY_PREV = 4 | BTR_MODIFY_LEAF,
	/** Start modifying the entire B-tree. */
	BTR_MODIFY_TREE = 8 | BTR_MODIFY_LEAF,
	/** Continue modifying the entire R-tree.
	Only used by rtr_search_to_nth_level(). */
	BTR_CONT_MODIFY_TREE = 4 | BTR_MODIFY_TREE,

	/* BTR_INSERT, BTR_DELETE and BTR_DELETE_MARK are mutually
	exclusive. */
	/** The search tuple will be inserted to the secondary index
	at the searched position.  When the leaf page is not in the
	buffer pool, try to use the change buffer. */
	BTR_INSERT = 64,

	/** Try to delete mark a secondary index leaf page record at
	the searched position using the change buffer when the page is
	not in the buffer pool. */
	BTR_DELETE_MARK	= 128,

	/** Try to purge the record using the change buffer when the
	secondary index leaf page is not in the buffer pool. */
	BTR_DELETE = BTR_INSERT | BTR_DELETE_MARK,

	/** The caller is already holding dict_index_t::lock S-latch. */
	BTR_ALREADY_S_LATCHED = 256,
	/** Search and S-latch a leaf page, assuming that the
	dict_index_t::lock S-latch is being held. */
	BTR_SEARCH_LEAF_ALREADY_S_LATCHED = BTR_SEARCH_LEAF
	| BTR_ALREADY_S_LATCHED,
	/** Search and X-latch a leaf page, assuming that the
	dict_index_t::lock is being held in non-exclusive mode. */
	BTR_MODIFY_LEAF_ALREADY_LATCHED = BTR_MODIFY_LEAF
	| BTR_ALREADY_S_LATCHED,
	/** Attempt to modify records in an x-latched tree. */
	BTR_MODIFY_TREE_ALREADY_LATCHED = BTR_MODIFY_TREE
	| BTR_ALREADY_S_LATCHED,
	/** U-latch root and X-latch a leaf page, assuming that
	dict_index_t::lock is being held in U mode. */
	BTR_MODIFY_ROOT_AND_LEAF_ALREADY_LATCHED = BTR_MODIFY_ROOT_AND_LEAF
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
	BTR_LATCH_FOR_DELETE = 512,

	/** In the case of BTR_MODIFY_TREE, the caller specifies
	the intention to delete record only. It is used to optimize
	block->lock range.*/
	BTR_LATCH_FOR_INSERT = 1024,

	/** Attempt to delete a record in the tree. */
	BTR_PURGE_TREE = BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
	/** Attempt to delete a record in an x-latched tree. */
	BTR_PURGE_TREE_ALREADY_LATCHED = BTR_PURGE_TREE
	| BTR_ALREADY_S_LATCHED,

	/** Attempt to insert a record into the tree. */
	BTR_INSERT_TREE = BTR_MODIFY_TREE | BTR_LATCH_FOR_INSERT,

	/** This flag ORed to BTR_INSERT says that we can ignore possible
	UNIQUE definition on secondary indexes when we decide if we can use
	the insert buffer to speed up inserts */
	BTR_IGNORE_SEC_UNIQUE = 2048,
	/** Rollback in spatial index */
	BTR_RTREE_UNDO_INS = 4096,
	/** Try to delete mark a spatial index record */
	BTR_RTREE_DELETE_MARK = 8192
};
