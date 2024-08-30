/*****************************************************************************

Copyright (c) 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file gis/gis0rtree.cc
InnoDB R-tree interfaces

Created 2013/03/27 Allen Lai and Jimmy Yang
***********************************************************************/

#include "fsp0fsp.h"
#include "page0page.h"
#include "page0cur.h"
#include "page0zip.h"
#include "gis0rtree.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "trx0undo.h"
#include "srv0mon.h"
#include "gis0geo.h"
#include <cmath>

/*************************************************************//**
Initial split nodes info for R-tree split.
@return initialized split nodes array */
static
rtr_split_node_t*
rtr_page_split_initialize_nodes(
/*============================*/
	mem_heap_t*	heap,	/*!< in: pointer to memory heap, or NULL */
	btr_cur_t*	cursor,	/*!< in: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	rec_offs**	offsets,/*!< in: offsets on inserted record */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	double**	buf_pos)/*!< in/out: current buffer position */
{
	rtr_split_node_t*	split_node_array;
	double*			buf;
	ulint			n_recs;
	rtr_split_node_t*	task;
	rtr_split_node_t*	stop;
	rtr_split_node_t*	cur;
	rec_t*			rec;
	buf_block_t*		block;
	page_t*			page;
	ulint			n_uniq;
	ulint			len;
	const byte*		source_cur;

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	n_uniq = dict_index_get_n_unique_in_tree(cursor->index());

	n_recs = ulint(page_get_n_recs(page)) + 1;

	/*We reserve 2 MBRs memory space for temp result of split
	algrithm. And plus the new mbr that need to insert, we
	need (n_recs + 3)*MBR size for storing all MBRs.*/
	buf = static_cast<double*>(mem_heap_alloc(
		heap, DATA_MBR_LEN * (n_recs + 3)
		+ sizeof(rtr_split_node_t) * (n_recs + 1)));

	split_node_array = (rtr_split_node_t*)(buf + SPDIMS * 2 * (n_recs + 3));
	task = split_node_array;
	*buf_pos = buf;
	stop = task + n_recs;

	rec = page_rec_get_next(page_get_infimum_rec(page));
	const ulint n_core = page_is_leaf(page)
		? cursor->index()->n_core_fields : 0;
	*offsets = rec_get_offsets(rec, cursor->index(), *offsets, n_core,
				   n_uniq, &heap);

	source_cur = rec_get_nth_field(rec, *offsets, 0, &len);

	for (cur = task; cur < stop - 1; ++cur) {
		cur->coords = reserve_coords(buf_pos, SPDIMS);
		cur->key = rec;

		memcpy(cur->coords, source_cur, DATA_MBR_LEN);

		rec = page_rec_get_next(rec);
		*offsets = rec_get_offsets(rec, cursor->index(), *offsets,
					   n_core, n_uniq, &heap);
		source_cur = rec_get_nth_field(rec, *offsets, 0, &len);
	}

	/* Put the insert key to node list */
	source_cur = static_cast<const byte*>(dfield_get_data(
		dtuple_get_nth_field(tuple, 0)));
	cur->coords = reserve_coords(buf_pos, SPDIMS);
	rec = (byte*) mem_heap_alloc(
		heap, rec_get_converted_size(cursor->index(), tuple, 0));

	rec = rec_convert_dtuple_to_rec(rec, cursor->index(), tuple, 0);
	cur->key = rec;

	memcpy(cur->coords, source_cur, DATA_MBR_LEN);

	return split_node_array;
}

/**********************************************************************//**
Builds a Rtree node pointer out of a physical record and a page number.
Note: For Rtree, we just keep the mbr and page no field in non-leaf level
page. It's different with Btree, Btree still keeps PK fields so far.
@return	own: node pointer */
dtuple_t*
rtr_index_build_node_ptr(
/*=====================*/
	const dict_index_t*	index,	/*!< in: index */
	const rtr_mbr_t*	mbr,	/*!< in: mbr of lower page */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap)	/*!< in: memory heap where pointer
					created */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;
	ulint		n_unique;
	ulint		info_bits;

	ut_ad(dict_index_is_spatial(index));

	n_unique = DICT_INDEX_SPATIAL_NODEPTR_SIZE;

	tuple = dtuple_create(heap, n_unique + 1);

	/* For rtree internal node, we need to compare page number
	fields. */
	dtuple_set_n_fields_cmp(tuple, n_unique + 1);

	dict_index_copy_types(tuple, index, n_unique);

	/* Write page no field */
	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);

	/* Set info bits. */
	info_bits = rec_get_info_bits(rec, dict_table_is_comp(index->table));
	dtuple_set_info_bits(tuple, info_bits | REC_STATUS_NODE_PTR);

	/* Set mbr as index entry data */
	field = dtuple_get_nth_field(tuple, 0);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_MBR_LEN));

	rtr_write_mbr(buf, mbr);

	dfield_set_data(field, buf, DATA_MBR_LEN);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/**************************************************************//**
Update the mbr field of a spatial index row. */
void
rtr_update_mbr_field(
/*=================*/
	btr_cur_t*	cursor,		/*!< in/out: cursor pointed to rec.*/
	rec_offs*	offsets,	/*!< in/out: offsets on rec. */
	btr_cur_t*	cursor2,	/*!< in/out: cursor pointed to rec
					that should be deleted.
					this cursor is for btr_compress to
					delete the merged page's father rec.*/
	page_t*		child_page,	/*!< in: child page. */
	rtr_mbr_t*	mbr,		/*!< in: the new mbr. */
	rec_t*		new_rec,	/*!< in: rec to use */
	mtr_t*		mtr)		/*!< in: mtr */
{
	dict_index_t*	index = cursor->index();
	mem_heap_t*	heap;
	page_t*		page;
	rec_t*		rec;
	constexpr ulint flags = BTR_NO_UNDO_LOG_FLAG
			| BTR_NO_LOCKING_FLAG
			| BTR_KEEP_SYS_FLAG;
	dberr_t		err;
	big_rec_t*	dummy_big_rec;
	buf_block_t*	block;
	rec_t*		child_rec;
	ulint		up_match = 0;
	ulint		low_match = 0;
	ulint		child;
	ulint		rec_info;
	bool		ins_suc = true;
	ulint		cur2_pos = 0;
	ulint		del_page_no = 0;
	rec_offs*	offsets2;

	rec = btr_cur_get_rec(cursor);
	page = page_align(rec);

	rec_info = rec_get_info_bits(rec, rec_offs_comp(offsets));

	heap = mem_heap_create(100);
	block = btr_cur_get_block(cursor);
	ut_ad(page == buf_block_get_frame(block));

	child = btr_node_ptr_get_child_page_no(rec, offsets);
	const ulint n_core = page_is_leaf(block->page.frame)
		? index->n_core_fields : 0;

	if (new_rec) {
		child_rec = new_rec;
	} else {
		child_rec = page_rec_get_next(page_get_infimum_rec(child_page));
	}

	dtuple_t* node_ptr = rtr_index_build_node_ptr(
		index, mbr, child_rec, child, heap);

	/* We need to remember the child page no of cursor2, since page could be
	reorganized or insert a new rec before it. */
	if (cursor2) {
		ut_ad(cursor2->index() == index);
		rec_t*	del_rec = btr_cur_get_rec(cursor2);
		offsets2 = rec_get_offsets(btr_cur_get_rec(cursor2),
					   index, NULL, 0,
					   ULINT_UNDEFINED, &heap);
		del_page_no = btr_node_ptr_get_child_page_no(del_rec, offsets2);
		cur2_pos = page_rec_get_n_recs_before(btr_cur_get_rec(cursor2));
	}

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(rec_offs_base(offsets)[0 + 1] == DATA_MBR_LEN);
	ut_ad(node_ptr->fields[0].len == DATA_MBR_LEN);

	if (rec_info & REC_INFO_MIN_REC_FLAG) {
		/* When the rec is minimal rec in this level, we do
		in-place update for avoiding it move to other place. */
		page_zip_des_t* page_zip = buf_block_get_page_zip(block);

		if (UNIV_LIKELY_NULL(page_zip)) {
			/* Check if there's enough space for in-place
			update the zip page. */
			if (!btr_cur_update_alloc_zip(
					page_zip,
					btr_cur_get_page_cur(cursor),
					offsets,
					rec_offs_size(offsets),
					false, mtr)) {

				/* If there's not enought space for
				inplace update zip page, we do delete
				insert. */
				ins_suc = false;

				/* Since btr_cur_update_alloc_zip could
				reorganize the page, we need to repositon
				cursor2. */
				if (cursor2) {
					cursor2->page_cur.rec =
						page_rec_get_nth(page,
								 cur2_pos);
				}

				goto update_mbr;
			}

			/* Record could be repositioned */
			rec = btr_cur_get_rec(cursor);

#ifdef UNIV_DEBUG
			/* Make sure it is still the first record */
			rec_info = rec_get_info_bits(
					rec, rec_offs_comp(offsets));
			ut_ad(rec_info & REC_INFO_MIN_REC_FLAG);
#endif /* UNIV_DEBUG */
			memcpy(rec, node_ptr->fields[0].data, DATA_MBR_LEN);
			page_zip_write_rec(block, rec, index, offsets, 0, mtr);
		} else {
			mtr->memcpy<mtr_t::MAYBE_NOP>(*block, rec,
						      node_ptr->fields[0].data,
						      DATA_MBR_LEN);
		}

		if (cursor2) {
			rec_offs* offsets2;

			if (UNIV_LIKELY_NULL(page_zip)) {
				cursor2->page_cur.rec
					= page_rec_get_nth(page, cur2_pos);
			}
			offsets2 = rec_get_offsets(btr_cur_get_rec(cursor2),
						   index, NULL, 0,
						   ULINT_UNDEFINED, &heap);
			ut_ad(del_page_no == btr_node_ptr_get_child_page_no(
							cursor2->page_cur.rec,
							offsets2));

			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    offsets2, mtr);
		}
	} else if (page_get_n_recs(page) == 1) {
		/* When there's only one rec in the page, we do insert/delete to
		avoid page merge. */

		page_cur_t		page_cur;
		rec_t*			insert_rec;
		rec_offs*		insert_offsets = NULL;
		ulint			old_pos;
		rec_t*			old_rec;

		ut_ad(cursor2 == NULL);

		/* Insert the new mbr rec. */
		old_pos = page_rec_get_n_recs_before(rec);

		err = btr_cur_optimistic_insert(
			flags,
			cursor, &insert_offsets, &heap,
			node_ptr, &insert_rec, &dummy_big_rec, 0, NULL, mtr);

		ut_ad(err == DB_SUCCESS);

		btr_cur_position(index, insert_rec, block, cursor);

		/* Delete the old mbr rec. */
		old_rec = page_rec_get_nth(page, old_pos);
		ut_ad(old_rec != insert_rec);

		page_cur_position(old_rec, block, &page_cur);
		page_cur.index = index;
		offsets2 = rec_get_offsets(old_rec, index, NULL, n_core,
					   ULINT_UNDEFINED, &heap);
		page_cur_delete_rec(&page_cur, offsets2, mtr);

	} else {
update_mbr:
		/* When there're not only 1 rec in the page, we do delete/insert
		to avoid page split. */
		rec_t*			insert_rec;
		rec_offs*		insert_offsets = NULL;
		rec_t*			next_rec;

		/* Delete the rec which cursor point to. */
		next_rec = page_rec_get_next(rec);
		page_cur_delete_rec(&cursor->page_cur, offsets, mtr);
		if (!ins_suc) {
			ut_ad(rec_info & REC_INFO_MIN_REC_FLAG);

			btr_set_min_rec_mark(next_rec, *block, mtr);
		}

		/* If there's more than 1 rec left in the page, delete
		the rec which cursor2 point to. Otherwise, delete it later.*/
		if (cursor2 && page_get_n_recs(page) > 1) {
			ulint		cur2_rec_info;
			rec_t*		cur2_rec;

			cur2_rec = cursor2->page_cur.rec;
			offsets2 = rec_get_offsets(cur2_rec, index, NULL,
						   n_core,
						   ULINT_UNDEFINED, &heap);

			cur2_rec_info = rec_get_info_bits(cur2_rec,
						rec_offs_comp(offsets2));
			if (cur2_rec_info & REC_INFO_MIN_REC_FLAG) {
				/* If we delete the leftmost node
				pointer on a non-leaf level, we must
				mark the new leftmost node pointer as
				the predefined minimum record */
				rec_t*	next_rec = page_rec_get_next(cur2_rec);
				btr_set_min_rec_mark(next_rec, *block, mtr);
			}

			ut_ad(del_page_no
			      == btr_node_ptr_get_child_page_no(cur2_rec,
								offsets2));
			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    offsets2, mtr);
			cursor2 = NULL;
		}

		/* Insert the new rec. */
		if (page_cur_search_with_match(node_ptr, PAGE_CUR_LE,
					       &up_match, &low_match,
					       btr_cur_get_page_cur(cursor),
					       NULL)) {
			goto err_exit;
		}

		err = btr_cur_optimistic_insert(flags, cursor, &insert_offsets,
						&heap, node_ptr, &insert_rec,
						&dummy_big_rec, 0, NULL, mtr);

		/* If optimistic insert fail, try reorganize the page
		and insert again. */
		if (err == DB_SUCCESS) {
			ins_suc = true;
		} else if (ins_suc) {
			ut_ad(err == DB_FAIL);
			err = btr_page_reorganize(btr_cur_get_page_cur(cursor),
						  mtr);
			if (err == DB_SUCCESS) {
				err = btr_cur_optimistic_insert(
					flags, cursor, &insert_offsets, &heap,
					node_ptr, &insert_rec, &dummy_big_rec,
					0, NULL, mtr);
			}

			/* Will do pessimistic insert */
			if (err != DB_SUCCESS) {
				ut_ad(err == DB_FAIL);
				ins_suc = false;
			}
		}

		/* Insert succeed, position cursor the inserted rec.*/
		if (ins_suc) {
			btr_cur_position(index, insert_rec, block, cursor);
			offsets = rec_get_offsets(insert_rec,
						  index, offsets, n_core,
						  ULINT_UNDEFINED, &heap);
		}

		/* Delete the rec which cursor2 point to. */
		if (cursor2) {
			ulint		cur2_pno;
			rec_t*		cur2_rec;

			cursor2->page_cur.rec = page_rec_get_nth(page,
								 cur2_pos);

			cur2_rec = btr_cur_get_rec(cursor2);

			offsets2 = rec_get_offsets(cur2_rec, index, NULL,
						   n_core,
						   ULINT_UNDEFINED, &heap);

			/* If the cursor2 position is on a wrong rec, we
			need to reposition it. */
			cur2_pno = btr_node_ptr_get_child_page_no(cur2_rec, offsets2);
			if ((del_page_no != cur2_pno)
			    || (cur2_rec == insert_rec)) {
				cur2_rec = page_get_infimum_rec(page);

				while ((cur2_rec
					= page_rec_get_next(cur2_rec))) {
					if (page_rec_is_supremum(cur2_rec)) {
						break;
					}

					offsets2 = rec_get_offsets(cur2_rec, index,
								   NULL,
								   n_core,
								   ULINT_UNDEFINED,
								   &heap);
					cur2_pno = btr_node_ptr_get_child_page_no(
							cur2_rec, offsets2);
					if (cur2_pno == del_page_no) {
						if (insert_rec != cur2_rec) {
							cursor2->page_cur.rec =
								cur2_rec;
							break;
						}
					}
				}
			}

			rec_info = rec_get_info_bits(cur2_rec,
						     rec_offs_comp(offsets2));
			if (rec_info & REC_INFO_MIN_REC_FLAG) {
				/* If we delete the leftmost node
				pointer on a non-leaf level, we must
				mark the new leftmost node pointer as
				the predefined minimum record */
				rec_t*	next_rec = page_rec_get_next(cur2_rec);
				btr_set_min_rec_mark(next_rec, *block, mtr);
			}

			ut_ad(cur2_pno == del_page_no && cur2_rec != insert_rec);

			page_cur_delete_rec(btr_cur_get_page_cur(cursor2),
					    offsets2, mtr);
		}

		if (!ins_suc) {
			mem_heap_t*	new_heap = NULL;

			err = btr_cur_pessimistic_insert(
				flags,
				cursor, &insert_offsets, &new_heap,
				node_ptr, &insert_rec, &dummy_big_rec,
				0, NULL, mtr);

			ut_ad(err == DB_SUCCESS);

			if (new_heap) {
				mem_heap_free(new_heap);
			}

		}

		if (cursor2) {
			btr_cur_compress_if_useful(cursor, FALSE, mtr);
		}
	}

	ut_ad(page_has_prev(page)
	      || (REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
			  page_rec_get_next(page_get_infimum_rec(page)),
			  page_is_comp(page))));
err_exit:
	mem_heap_free(heap);
}

MY_ATTRIBUTE((nonnull(1,3,4,5,6,8), warn_unused_result))
/**************************************************************//**
Update parent page's MBR and Predicate lock information during a split */
static
dberr_t
rtr_adjust_upper_level(
/*===================*/
	btr_cur_t*	sea_cur,	/*!< in: search cursor */
	ulint		flags,		/*!< in: undo logging and
					locking flags */
	buf_block_t*	block,		/*!< in/out: page to be split */
	buf_block_t*	new_block,	/*!< in/out: the new half page */
	rtr_mbr_t*	mbr,		/*!< in: MBR on the old page */
	rtr_mbr_t*	new_mbr,	/*!< in: MBR on the new page */
	que_thr_t*	thr,		/*!< in/out: query thread */
	mtr_t*		mtr)		/*!< in: mtr */
{
	ulint		page_no;
	ulint		new_page_no;
	btr_cur_t	cursor;
	rec_offs*	offsets;
	mem_heap_t*	heap;
	ulint		level;
	dtuple_t*	node_ptr_upper = nullptr;
	page_cur_t*	page_cursor;
	lock_prdt_t	prdt;
	lock_prdt_t	new_prdt;
	big_rec_t*	dummy_big_rec;
	rec_t*		rec;

	/* Create a memory heap where the data tuple is stored */
	heap = mem_heap_create(1024);

	cursor.page_cur.index = sea_cur->index();
	cursor.page_cur.block = block;

	/* Get the level of the split pages */
	level = btr_page_get_level(buf_block_get_frame(block));
	ut_ad(level == btr_page_get_level(buf_block_get_frame(new_block)));

	page_no = block->page.id().page_no();

	new_page_no = new_block->page.id().page_no();

	/* Set new mbr for the old page on the upper level. */
	/* Look up the index for the node pointer to page */
	offsets = rtr_page_get_father_block(nullptr, heap, sea_cur, &cursor,
					    thr, mtr);

	page_cursor = btr_cur_get_page_cur(&cursor);

	rtr_update_mbr_field(&cursor, offsets, nullptr, block->page.frame, mbr,
			     nullptr, mtr);

	/* Already updated parent MBR, reset in our path */
	if (sea_cur->rtr_info) {
		node_visit_t*	node_visit = rtr_get_parent_node(
						sea_cur, level + 1, true);
		if (node_visit) {
			node_visit->mbr_inc = 0;
		}
	}

	dberr_t err;

	if (const rec_t* first = page_rec_get_next_const(
		    page_get_infimum_rec(new_block->page.frame))) {
		/* Insert the node for the new page. */
		node_ptr_upper = rtr_index_build_node_ptr(
			sea_cur->index(), new_mbr, first, new_page_no, heap);
		ulint	up_match = 0, low_match = 0;
		err = page_cur_search_with_match(node_ptr_upper,
						 PAGE_CUR_LE,
						 &up_match, &low_match,
						 btr_cur_get_page_cur(&cursor),
						 NULL)
			? DB_CORRUPTION
			: btr_cur_optimistic_insert(flags
						    | BTR_NO_LOCKING_FLAG
						    | BTR_KEEP_SYS_FLAG
						    | BTR_NO_UNDO_LOG_FLAG,
						    &cursor, &offsets, &heap,
						    node_ptr_upper, &rec,
						    &dummy_big_rec, 0, NULL,
						    mtr);
	} else {
		err = DB_CORRUPTION;
	}

	if (err == DB_FAIL) {
		cursor.rtr_info = sea_cur->rtr_info;
		cursor.tree_height = sea_cur->tree_height;

		/* Recreate a memory heap as input parameter for
		btr_cur_pessimistic_insert(), because the heap may be
		emptied in btr_cur_pessimistic_insert(). */
		mem_heap_t* new_heap = mem_heap_create(1024);

		err = btr_cur_pessimistic_insert(flags
						 | BTR_NO_LOCKING_FLAG
						 | BTR_KEEP_SYS_FLAG
						 | BTR_NO_UNDO_LOG_FLAG,
						 &cursor, &offsets, &new_heap,
						 node_ptr_upper, &rec,
						 &dummy_big_rec, 0, NULL, mtr);
		cursor.rtr_info = NULL;
		mem_heap_free(new_heap);
	}

	if (err == DB_SUCCESS) {
		prdt.data = static_cast<void*>(mbr);
		prdt.op = 0;
		new_prdt.data = static_cast<void*>(new_mbr);
		new_prdt.op = 0;

		lock_prdt_update_parent(block, new_block, &prdt, &new_prdt,
					page_cursor->block->page.id());
	}

	mem_heap_free(heap);

	ut_ad(block->zip_size() == sea_cur->index()->table->space->zip_size());

	if (err != DB_SUCCESS) {
		return err;
	}

	const uint32_t next_page_no = btr_page_get_next(block->page.frame);

	if (next_page_no == FIL_NULL) {
	} else if (buf_block_t*	next_block =
		   btr_block_get(*sea_cur->index(), next_page_no, RW_X_LATCH,
				 mtr, &err)) {
		if (UNIV_UNLIKELY(memcmp_aligned<4>(next_block->page.frame
						    + FIL_PAGE_PREV,
						    block->page.frame
						    + FIL_PAGE_OFFSET, 4))) {
			return DB_CORRUPTION;
		}
		btr_page_set_prev(next_block, new_page_no, mtr);
	} else {
		return err;
	}

	btr_page_set_next(block, new_page_no, mtr);

	btr_page_set_prev(new_block, page_no, mtr);
	btr_page_set_next(new_block, next_page_no, mtr);
	return DB_SUCCESS;
}

/*************************************************************//**
Moves record list to another page for rtree splitting.

@return error code
@retval DB_FAIL on ROW_FORMAT=COMPRESSED compression failure */
static
dberr_t
rtr_split_page_move_rec_list(
/*=========================*/
	rtr_split_node_t*	node_array,	/*!< in: split node array. */
	int			first_rec_group,/*!< in: group number of the
						first rec. */
	buf_block_t*		new_block,	/*!< in/out: index page
						where to move */
	buf_block_t*		block,		/*!< in/out: page containing
						split_rec */
	rec_t*			first_rec,	/*!< in: first record not to
						move */
	dict_index_t*		index,		/*!< in: record descriptor */
	mem_heap_t*		heap,		/*!< in: pointer to memory
						heap, or NULL */
	mtr_t*			mtr)		/*!< in: mtr */
{
	rtr_split_node_t*	cur_split_node;
	rtr_split_node_t*	end_split_node;
	page_cur_t		page_cursor;
	page_cur_t		new_page_cursor;
	page_t*			page;
	page_t*			new_page;
	rec_offs		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*		offsets		= offsets_;
	page_zip_des_t*		new_page_zip
		= buf_block_get_page_zip(new_block);
	rec_t*			rec;
	ulint			moved		= 0;
	ulint			max_to_move	= 0;
	rtr_rec_move_t*		rec_move	= NULL;

	ut_ad(index->is_spatial());

	rec_offs_init(offsets_);

	page_cur_set_before_first(block, &page_cursor);
	page_cur_set_before_first(new_block, &new_page_cursor);
	page_cursor.index = new_page_cursor.index = index;

	page = buf_block_get_frame(block);
	new_page = buf_block_get_frame(new_block);

	end_split_node = node_array + page_get_n_recs(page);

	mtr_log_t	log_mode = MTR_LOG_NONE;

	if (new_page_zip) {
		log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);
	}

	max_to_move = page_get_n_recs(buf_block_get_frame(block));
	rec_move = static_cast<rtr_rec_move_t*>(mem_heap_alloc(
			heap,
			sizeof (*rec_move) * max_to_move));
	const ulint n_core = page_is_leaf(page)
		? index->n_core_fields : 0;

	/* Insert the recs in group 2 to new page.  */
	for (cur_split_node = node_array;
	     cur_split_node < end_split_node; ++cur_split_node) {
		if (cur_split_node->n_node != first_rec_group) {
			lock_rec_store_on_page_infimum(
				block, cur_split_node->key);

			offsets = rec_get_offsets(cur_split_node->key,
						  index, offsets, n_core,
						  ULINT_UNDEFINED, &heap);

			ut_ad(!n_core || cur_split_node->key != first_rec);

			rec = page_cur_insert_rec_low(
				&new_page_cursor,
				cur_split_node->key, offsets, mtr);

			if (UNIV_UNLIKELY
			    (!rec
			     || !page_cur_move_to_next(&new_page_cursor))) {
				return DB_CORRUPTION;
			}

			lock_rec_restore_from_page_infimum(
				*new_block, rec, block->page.id());

			rec_move[moved].new_rec = rec;
			rec_move[moved].old_rec = cur_split_node->key;
			rec_move[moved].moved = false;
			moved++;

			if (moved > max_to_move) {
				ut_ad(0);
				break;
			}
		}
	}

	/* Update PAGE_MAX_TRX_ID on the uncompressed page.
	Modifications will be redo logged and copied to the compressed
	page in page_zip_compress() or page_zip_reorganize() below.
	Multiple transactions cannot simultaneously operate on the
	same temp-table in parallel.
	max_trx_id is ignored for temp tables because it not required
	for MVCC. */
	if (n_core && !index->table->is_temporary()) {
		page_update_max_trx_id(new_block, NULL,
				       page_get_max_trx_id(page),
				       mtr);
	}

	if (new_page_zip) {
		mtr_set_log_mode(mtr, log_mode);

		if (!page_zip_compress(new_block, index,
				       page_zip_level, mtr)) {
			if (dberr_t err =
				page_zip_reorganize(new_block, index,
						    page_zip_level, mtr)) {
				if (err == DB_FAIL) {
					ut_a(page_zip_decompress(new_page_zip,
								 new_page,
								 FALSE));
				}
				return err;
			}
		}
	}

	/* Update the lock table */
	lock_rtr_move_rec_list(new_block, block, rec_move, moved);

	/* Delete recs in second group from the old page. */
	for (cur_split_node = node_array;
	     cur_split_node < end_split_node; ++cur_split_node) {
		if (cur_split_node->n_node != first_rec_group) {
			page_cur_position(cur_split_node->key,
					  block, &page_cursor);
			offsets = rec_get_offsets(
				page_cur_get_rec(&page_cursor), index,
				offsets, n_core, ULINT_UNDEFINED,
				&heap);
			page_cur_delete_rec(&page_cursor, offsets, mtr);
		}
	}

	return DB_SUCCESS;
}

/*************************************************************//**
Splits an R-tree index page to halves and inserts the tuple. It is assumed
that mtr holds an x-latch to the index tree. NOTE: the tree x-latch is
released within this function! NOTE that the operation of this
function must always succeed, we cannot reverse it: therefore enough
free disk space (2 pages) must be guaranteed to be available before
this function is called.
@return inserted record */
rec_t*
rtr_page_split_and_insert(
/*======================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in/out: cursor at which to insert; when the
				function returns, the cursor is positioned
				on the predecessor of the inserted record */
	rec_offs**	offsets,/*!< out: offsets on inserted record */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	const dtuple_t*	tuple,	/*!< in: tuple to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr,	/*!< in: mtr */
	dberr_t*	err,	/*!< out: error code */
	que_thr_t*	thr)	/*!< in: query thread */
{
	buf_block_t*		block;
	page_t*			page;
	page_t*			new_page;
	buf_block_t*		new_block;
	page_zip_des_t*		page_zip;
	page_zip_des_t*		new_page_zip;
	page_cur_t*		page_cursor;
	rec_t*			rec = 0;
	ulint			n_recs;
	ulint			total_data;
	ulint			insert_size;
	rtr_split_node_t*	rtr_split_node_array;
	rtr_split_node_t*	cur_split_node;
	rtr_split_node_t*	end_split_node;
	double*			buf_pos;
	node_seq_t		current_ssn;
	node_seq_t		next_ssn;
	buf_block_t*		root_block;
	rtr_mbr_t		mbr;
	rtr_mbr_t		new_mbr;
	lock_prdt_t		prdt;
	lock_prdt_t		new_prdt;
	rec_t*			first_rec = NULL;
	int			first_rec_group = 1;
	IF_DBUG(bool iterated = false,);

	buf_pool.pages_split++;

	if (!*heap) {
		*heap = mem_heap_create(1024);
	}

func_start:
	mem_heap_empty(*heap);
	*offsets = NULL;

	ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
					 MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
	ut_ad(!dict_index_is_online_ddl(cursor->index()));
	ut_ad(cursor->index()->lock.have_u_or_x());

	block = btr_cur_get_block(cursor);
	page = buf_block_get_frame(block);
	page_zip = buf_block_get_page_zip(block);
	current_ssn = page_get_ssn_id(page);

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_get_n_recs(page) >= 1);

	const page_id_t page_id(block->page.id());

	if (!page_has_prev(page) && !page_is_leaf(page)) {
		first_rec = page_rec_get_next(
			page_get_infimum_rec(buf_block_get_frame(block)));
		if (UNIV_UNLIKELY(!first_rec)) {
corrupted:
			*err = DB_CORRUPTION;
			return nullptr;
		}
	}

	/* Initial split nodes array. */
	rtr_split_node_array = rtr_page_split_initialize_nodes(
		*heap, cursor, offsets, tuple, &buf_pos);

	/* Divide all mbrs to two groups. */
	n_recs = ulint(page_get_n_recs(page)) + 1;

	end_split_node = rtr_split_node_array + n_recs;

#ifdef UNIV_GIS_DEBUG
	fprintf(stderr, "Before split a page:\n");
	for (cur_split_node = rtr_split_node_array;
		cur_split_node < end_split_node; ++cur_split_node) {
		for (int i = 0; i < SPDIMS * 2; i++) {
			fprintf(stderr, "%.2lf ",
			        *(cur_split_node->coords + i));
		}
		fprintf(stderr, "\n");
	}
#endif

	insert_size = rec_get_converted_size(cursor->index(), tuple, n_ext);
	total_data = page_get_data_size(page) + insert_size;
	first_rec_group = split_rtree_node(rtr_split_node_array,
					   static_cast<int>(n_recs),
					   static_cast<int>(total_data),
					   static_cast<int>(insert_size),
					   0, 2, 2, &buf_pos, SPDIMS,
					   static_cast<uchar*>(first_rec));

	/* Allocate a new page to the index */
	const uint16_t page_level = btr_page_get_level(page);
	new_block = btr_page_alloc(cursor->index(), page_id.page_no() + 1,
				   FSP_UP, page_level, mtr, mtr, err);
	if (UNIV_UNLIKELY(!new_block)) {
		return nullptr;
	}

	new_page_zip = buf_block_get_page_zip(new_block);
	if (page_level && UNIV_LIKELY_NULL(new_page_zip)) {
		/* ROW_FORMAT=COMPRESSED non-leaf pages are not expected
		to contain FIL_NULL in FIL_PAGE_PREV at this stage. */
		memset_aligned<4>(new_block->page.frame + FIL_PAGE_PREV, 0, 4);
	}
	btr_page_create(new_block, new_page_zip, cursor->index(),
			page_level, mtr);

	new_page = buf_block_get_frame(new_block);
	ut_ad(page_get_ssn_id(new_page) == 0);

	/* Set new ssn to the new page and page. */
	page_set_ssn_id(new_block, new_page_zip, current_ssn, mtr);
	next_ssn = rtr_get_new_ssn_id(cursor->index());

	page_set_ssn_id(block, page_zip, next_ssn, mtr);

	/* Keep recs in first group to the old page, move recs in second
	groups to the new page. */
	if (0
#ifdef UNIV_ZIP_COPY
	    || page_zip
#endif
	    || (*err = rtr_split_page_move_rec_list(rtr_split_node_array,
						    first_rec_group,
						    new_block, block,
						    first_rec, cursor->index(),
						    *heap, mtr))) {
		if (*err != DB_FAIL) {
			return nullptr;
		}

		*err = DB_SUCCESS;

		ulint			n		= 0;
		rec_t*			rec;
		ulint			moved		= 0;
		ulint			max_to_move	= 0;
		rtr_rec_move_t*		rec_move	= NULL;
		ulint			pos;

		/* For some reason, compressing new_page failed,
		even though it should contain fewer records than
		the original page.  Copy the page byte for byte
		and then delete the records from both pages
		as appropriate.  Deleting will always succeed. */
		ut_a(new_page_zip);

		page_zip_copy_recs(new_block,
				   page_zip, page, cursor->index(), mtr);

		page_cursor = btr_cur_get_page_cur(cursor);

		/* Move locks on recs. */
		max_to_move = page_get_n_recs(page);
		rec_move = static_cast<rtr_rec_move_t*>(mem_heap_alloc(
				*heap,
				sizeof (*rec_move) * max_to_move));

		/* Init the rec_move array for moving lock on recs.  */
		for (cur_split_node = rtr_split_node_array;
		     cur_split_node < end_split_node - 1; ++cur_split_node) {
			if (cur_split_node->n_node != first_rec_group) {
				pos = page_rec_get_n_recs_before(
					cur_split_node->key);
				rec = page_rec_get_nth(new_page, pos);
				ut_a(rec);

				rec_move[moved].new_rec = rec;
				rec_move[moved].old_rec = cur_split_node->key;
				rec_move[moved].moved = false;
				moved++;

				if (moved > max_to_move) {
					ut_ad(0);
					break;
				}
			}
		}

		/* Update the lock table */
		lock_rtr_move_rec_list(new_block, block, rec_move, moved);

		const ulint n_core = page_level
			? 0 : cursor->index()->n_core_fields;

		/* Delete recs in first group from the new page. */
		for (cur_split_node = rtr_split_node_array;
		     cur_split_node < end_split_node - 1; ++cur_split_node) {
			if (cur_split_node->n_node == first_rec_group) {
				ulint	pos;

				pos = page_rec_get_n_recs_before(
						cur_split_node->key);
				ut_a(pos > 0);
				rec_t* new_rec = page_rec_get_nth(new_page,
								  pos - n);

				ut_a(new_rec && page_rec_is_user_rec(new_rec));
				page_cur_position(new_rec, new_block,
						  page_cursor);

				*offsets = rec_get_offsets(
					page_cur_get_rec(page_cursor),
					cursor->index(), *offsets, n_core,
					ULINT_UNDEFINED, heap);

				page_cur_delete_rec(page_cursor,
						    *offsets, mtr);
				n++;
			}
		}

		/* Delete recs in second group from the old page. */
		for (cur_split_node = rtr_split_node_array;
		     cur_split_node < end_split_node - 1; ++cur_split_node) {
			if (cur_split_node->n_node != first_rec_group) {
				page_cur_position(cur_split_node->key,
						  block, page_cursor);
				*offsets = rec_get_offsets(
					page_cur_get_rec(page_cursor),
					page_cursor->index, *offsets, n_core,
					ULINT_UNDEFINED, heap);
				page_cur_delete_rec(page_cursor, *offsets,
						    mtr);
			}
		}

#ifdef UNIV_GIS_DEBUG
		ut_ad(page_validate(new_page, cursor->index()));
		ut_ad(page_validate(page, cursor->index()));
#endif
	}

	/* Insert the new rec to the proper page. */
	cur_split_node = end_split_node - 1;

	/* Reposition the cursor for insert and try insertion */
	page_cursor = btr_cur_get_page_cur(cursor);
	page_cursor->block = cur_split_node->n_node != first_rec_group
		? new_block : block;

	ulint up_match = 0, low_match = 0;

	if (page_cur_search_with_match(tuple,
				       PAGE_CUR_LE, &up_match, &low_match,
				       page_cursor, nullptr)) {
		goto corrupted;
	}

	/* It's possible that the new record is too big to be inserted into
	the page, and it'll need the second round split in this case.
	We test this scenario here*/
	DBUG_EXECUTE_IF("rtr_page_need_second_split",
			if (!iterated) {
				rec = NULL;
				goto after_insert; }
	);

	rec = page_cur_tuple_insert(page_cursor, tuple,
				    offsets, heap, n_ext, mtr);

	/* If insert did not fit, try page reorganization.
	For compressed pages, page_cur_tuple_insert() will have
	attempted this already. */
	if (rec == NULL) {
		if (!is_page_cur_get_page_zip(page_cursor)
		    && btr_page_reorganize(page_cursor, mtr)) {
			rec = page_cur_tuple_insert(page_cursor, tuple,
						    offsets,
						    heap, n_ext, mtr);

		}
		/* If insert fail, we will try to split the block again. */
	}

#ifdef UNIV_DEBUG
after_insert:
#endif
	/* Calculate the mbr on the upper half-page, and the mbr on
	original page. */
	rtr_page_cal_mbr(cursor->index(), block, &mbr, *heap);
	rtr_page_cal_mbr(cursor->index(), new_block, &new_mbr, *heap);
	prdt.data = &mbr;
	new_prdt.data = &new_mbr;

	/* Check any predicate locks need to be moved/copied to the
	new page */
	lock_prdt_update_split(new_block, &prdt, &new_prdt, page_id);

	/* Adjust the upper level. */
	*err = rtr_adjust_upper_level(cursor, flags, block, new_block,
				      &mbr, &new_mbr, thr, mtr);
	if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
		return nullptr;
	}

	/* Save the new ssn to the root page, since we need to reinit
	the first ssn value from it after restart server. */

	root_block = btr_root_block_get(cursor->index(), RW_SX_LATCH,
					mtr, err);
	if (UNIV_UNLIKELY(!root_block)) {
		return nullptr;
	}

	page_zip = buf_block_get_page_zip(root_block);
	page_set_ssn_id(root_block, page_zip, next_ssn, mtr);

	/* If the new res insert fail, we need to do another split
	 again. */
	if (!rec) {
		/* We need to clean the parent path here and search father
		node later, otherwise, it's possible that find a wrong
		parent. */
		rtr_clean_rtr_info(cursor->rtr_info, true);
		cursor->rtr_info = NULL;
		IF_DBUG(iterated=true,);

		rec_t* i_rec = page_rec_get_next(page_get_infimum_rec(
			buf_block_get_frame(block)));
		if (UNIV_UNLIKELY(!i_rec)) {
			goto corrupted;
		}
		btr_cur_position(cursor->index(), i_rec, block, cursor);

		goto func_start;
	}

#ifdef UNIV_GIS_DEBUG
	ut_ad(page_validate(buf_block_get_frame(block), cursor->index()));
	ut_ad(page_validate(buf_block_get_frame(new_block), cursor->index()));

	ut_ad(!rec || rec_offs_validate(rec, cursor->index(), *offsets));
#endif
	return(rec);
}

/*************************************************************//**
Makes tree one level higher by splitting the root, and inserts the tuple.
NOTE that the operation of this function must always succeed,
we cannot reverse it: therefore enough free disk space must be
guaranteed to be available before this function is called.
@return inserted record */
rec_t*
rtr_root_raise_and_insert(
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
	dberr_t*	err,	/*!< out: error code */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dict_index_t*	index;
	rec_t*		rec;
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
	ut_ad(index->is_spatial());
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
	dberr_t e;
	if (!err) {
		err = &e;
	}

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

		/* Move any existing predicate locks */
		lock_prdt_rec_move(new_block, root_id);
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
	rec = page_rec_get_next(page_get_infimum_rec(new_block->page.frame));
	ut_ad(rec); /* We just created the page. */

	/* Build the node pointer (= node key and page address) for the
	child */
	rtr_mbr_t new_mbr;
	rtr_page_cal_mbr(index, new_block, &new_mbr, *heap);
	node_ptr = rtr_index_build_node_ptr(index, &new_mbr, rec, new_page_no,
					    *heap);
	/* The node pointer must be marked as the predefined minimum record,
	as there is no lower alphabetical limit to records in the leftmost
	node of a level: */
	dtuple_set_info_bits(node_ptr,
			     dtuple_get_info_bits(node_ptr)
			     | REC_INFO_MIN_REC_FLAG);

	/* Rebuild the root page to get free space */
	btr_page_empty(root, root_page_zip, index, level + 1, mtr);
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

	if (tuple) {
		ut_ad(dtuple_check_typed(tuple));
		/* Reposition the cursor to the child node */
		ulint low_match = 0, up_match = 0;

		if (page_cur_search_with_match(tuple, PAGE_CUR_LE,
					       &up_match, &low_match,
					       page_cursor, nullptr)) {
			if (err) {
				*err = DB_CORRUPTION;
			}
			return nullptr;
		}
	} else {
		page_cursor->rec = page_get_infimum_rec(new_block->page.frame);
	}

	/* Split the child and insert tuple */
	return rtr_page_split_and_insert(flags, cursor, offsets, heap,
					 tuple, n_ext, mtr, err, thr);
}

/****************************************************************//**
Following the right link to find the proper block for insert.
@return the proper block.*/
dberr_t
rtr_ins_enlarge_mbr(
/*================*/
	btr_cur_t*		btr_cur,	/*!< in: btr cursor */
	mtr_t*			mtr)		/*!< in: mtr */
{
	dberr_t			err = DB_SUCCESS;
	rtr_mbr_t		new_mbr;
	buf_block_t*		block;
	mem_heap_t*		heap;
	page_cur_t*		page_cursor;
	rec_offs*		offsets;
	node_visit_t*		node_visit;
	btr_cur_t		cursor;
	page_t*			page;

	ut_ad(btr_cur->index()->is_spatial());

	/* If no rtr_info or rtree is one level tree, return. */
	if (!btr_cur->rtr_info || btr_cur->tree_height == 1) {
		return(err);
	}

	/* Check path info is not empty. */
	ut_ad(!btr_cur->rtr_info->parent_path->empty());
	ut_ad(btr_cur->rtr_info->thr || !btr_cur->index()->is_committed()
	      || btr_cur->index()->table->name.is_temporary());

	/* Create a memory heap. */
	heap = mem_heap_create(1024);

	/* Leaf level page is stored in cursor */
	page_cursor = btr_cur_get_page_cur(btr_cur);
	block = page_cur_get_block(page_cursor);

	for (ulint i = 1; i < btr_cur->tree_height; i++) {
		node_visit = rtr_get_parent_node(btr_cur, i, true);
		ut_ad(node_visit != NULL);

		/* If there's no mbr enlarge, return.*/
		if (node_visit->mbr_inc == 0) {
			block = btr_pcur_get_block(node_visit->cursor);
			continue;
		}

		/* Calculate the mbr of the child page. */
		rtr_page_cal_mbr(page_cursor->index, block, &new_mbr, heap);

		/* Get father block. */
		cursor.page_cur.index = page_cursor->index;
		cursor.page_cur.block = block;
		offsets = rtr_page_get_father_block(
			nullptr, heap, btr_cur, &cursor,
			btr_cur->rtr_info->thr, mtr);

		page = buf_block_get_frame(block);

		/* Update the mbr field of the rec. */
		rtr_update_mbr_field(&cursor, offsets, NULL, page,
				     &new_mbr, NULL, mtr);
		block = btr_cur_get_block(&cursor);
	}

	mem_heap_free(heap);

	return(err);
}

/*************************************************************//**
Copy recs from a page to new_block of rtree.

@return error code */
dberr_t
rtr_page_copy_rec_list_end_no_locks(
/*================================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	rtr_rec_move_t*	rec_move,	/*!< in: recording records moved */
	ulint		max_move,	/*!< in: num of rec to move */
	ulint*		num_moved,	/*!< out: num of rec to move */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_t*		new_page	= buf_block_get_frame(new_block);
	page_cur_t	page_cur;
	page_cur_t	cur1;
	rec_t*		cur_rec;
	rec_offs	offsets_1[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets1 = offsets_1;
	rec_offs	offsets_2[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets2 = offsets_2;
	ulint		moved = 0;
	const ulint	n_core = page_is_leaf(new_page)
		? index->n_core_fields : 0;

	rec_offs_init(offsets_1);
	rec_offs_init(offsets_2);

	page_cur_position(rec, block, &cur1);

	if (page_cur_is_before_first(&cur1) && !page_cur_move_to_next(&cur1)) {
		return DB_CORRUPTION;
	}

	ut_a(page_is_comp(new_page) == page_rec_is_comp(rec));
	ut_a(mach_read_from_2(new_page + srv_page_size - 10) == (ulint)
	     (page_is_comp(new_page) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM));

	cur_rec = page_rec_get_next(
		page_get_infimum_rec(buf_block_get_frame(new_block)));
	if (UNIV_UNLIKELY(!cur_rec)) {
		return DB_CORRUPTION;
	}
	page_cur_position(cur_rec, new_block, &page_cur);
	page_cur.index = index;

	/* Copy records from the original page to the new page */
	while (!page_cur_is_after_last(&cur1)) {
		rec_t*	cur1_rec = page_cur_get_rec(&cur1);
		rec_t*	ins_rec;

		if (page_rec_is_infimum(cur_rec)) {
			cur_rec = page_rec_get_next(cur_rec);
			if (UNIV_UNLIKELY(!cur_rec)) {
				return DB_CORRUPTION;
			}
		}

		offsets1 = rec_get_offsets(cur1_rec, index, offsets1, n_core,
					   ULINT_UNDEFINED, &heap);
		while (!page_rec_is_supremum(cur_rec)) {
			ulint		cur_matched_fields = 0;
			int		cmp;

			offsets2 = rec_get_offsets(cur_rec, index, offsets2,
						   n_core,
						   ULINT_UNDEFINED, &heap);
			cmp = cmp_rec_rec(cur1_rec, cur_rec,
					  offsets1, offsets2, index, false,
					  &cur_matched_fields);
			if (cmp < 0) {
				goto move_to_prev;
			} else if (cmp > 0) {
				/* Skip small recs. */
				cur_rec = page_cur_move_to_next(&page_cur);
			} else if (n_core) {
				if (rec_get_deleted_flag(cur1_rec,
					dict_table_is_comp(index->table))) {
					goto next;
				} else {
					/* We have two identical leaf records,
					skip copying the undeleted one, and
					unmark deleted on the current page */
					btr_rec_set_deleted<false>(
						new_block, cur_rec, mtr);
					goto next;
				}
			}
		}

		/* If position is on suprenum rec, need to move to
		previous rec. */
		if (page_rec_is_supremum(cur_rec)) {
move_to_prev:
			cur_rec = page_cur_move_to_prev(&page_cur);
		} else {
			cur_rec = page_cur_get_rec(&page_cur);
		}

		if (UNIV_UNLIKELY(!cur_rec)) {
			return DB_CORRUPTION;
		}

		offsets1 = rec_get_offsets(cur1_rec, index, offsets1, n_core,
					   ULINT_UNDEFINED, &heap);

		ins_rec = page_cur_insert_rec_low(&page_cur,
						  cur1_rec, offsets1, mtr);
		if (UNIV_UNLIKELY(!ins_rec || moved >= max_move)) {
			return DB_CORRUPTION;
		}

		rec_move[moved].new_rec = ins_rec;
		rec_move[moved].old_rec = cur1_rec;
		rec_move[moved].moved = false;
		moved++;
next:
		if (UNIV_UNLIKELY(!page_cur_move_to_next(&cur1))) {
			return DB_CORRUPTION;
		}
	}

	*num_moved = moved;
	return DB_SUCCESS;
}

/*************************************************************//**
Copy recs till a specified rec from a page to new_block of rtree.

@return error code */
dberr_t
rtr_page_copy_rec_list_start_no_locks(
/*==================================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	rtr_rec_move_t*	rec_move,	/*!< in: recording records moved */
	ulint		max_move,	/*!< in: num of rec to move */
	ulint*		num_moved,	/*!< out: num of rec to move */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_cur_t	cur1;
	rec_t*		cur_rec;
	rec_offs	offsets_1[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets1 = offsets_1;
	rec_offs	offsets_2[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets2 = offsets_2;
	page_cur_t	page_cur;
	ulint		moved = 0;
	const ulint	n_core = page_is_leaf(buf_block_get_frame(block))
		? index->n_core_fields : 0;

	rec_offs_init(offsets_1);
	rec_offs_init(offsets_2);

	page_cur_set_before_first(block, &cur1);
	if (UNIV_UNLIKELY(!page_cur_move_to_next(&cur1))) {
		return DB_CORRUPTION;
	}

	cur_rec = page_rec_get_next(
		page_get_infimum_rec(buf_block_get_frame(new_block)));
	if (UNIV_UNLIKELY(!cur_rec)) {
		return DB_CORRUPTION;
	}
	page_cur_position(cur_rec, new_block, &page_cur);
	page_cur.index = index;

	while (page_cur_get_rec(&cur1) != rec) {
		rec_t*	cur1_rec = page_cur_get_rec(&cur1);
		rec_t*	ins_rec;

		if (page_rec_is_infimum(cur_rec)) {
			cur_rec = page_rec_get_next(cur_rec);
			if (UNIV_UNLIKELY(!cur_rec)) {
				return DB_CORRUPTION;
			}
		}

		offsets1 = rec_get_offsets(cur1_rec, index, offsets1, n_core,
					   ULINT_UNDEFINED, &heap);

		while (!page_rec_is_supremum(cur_rec)) {
			ulint		cur_matched_fields = 0;

			offsets2 = rec_get_offsets(cur_rec, index, offsets2,
						   n_core,
						   ULINT_UNDEFINED, &heap);
			int cmp = cmp_rec_rec(cur1_rec, cur_rec,
					      offsets1, offsets2, index, false,
					      &cur_matched_fields);
			if (cmp < 0) {
				goto move_to_prev;
			} else if (cmp > 0) {
				/* Skip small recs. */
				cur_rec = page_cur_move_to_next(&page_cur);
			} else if (n_core) {
				if (rec_get_deleted_flag(
					cur1_rec,
					dict_table_is_comp(index->table))) {
					goto next;
				} else {
					/* We have two identical leaf records,
					skip copying the undeleted one, and
					unmark deleted on the current page */
					btr_rec_set_deleted<false>(
						new_block, cur_rec, mtr);
					goto next;
				}
			}
		}

		/* If position is on suprenum rec, need to move to
		previous rec. */
		if (page_rec_is_supremum(cur_rec)) {
move_to_prev:
			cur_rec = page_cur_move_to_prev(&page_cur);
		} else {
			cur_rec = page_cur_get_rec(&page_cur);
		}

		if (UNIV_UNLIKELY(!cur_rec)) {
			return DB_CORRUPTION;
		}

		offsets1 = rec_get_offsets(cur1_rec, index, offsets1, n_core,
					   ULINT_UNDEFINED, &heap);

		ins_rec = page_cur_insert_rec_low(&page_cur,
						  cur1_rec, offsets1, mtr);
		if (UNIV_UNLIKELY(!ins_rec || moved >= max_move)) {
			return DB_CORRUPTION;
		}

		rec_move[moved].new_rec = ins_rec;
		rec_move[moved].old_rec = cur1_rec;
		rec_move[moved].moved = false;
		moved++;
next:
		if (UNIV_UNLIKELY(!page_cur_move_to_next(&cur1))) {
			return DB_CORRUPTION;
		}
	}

	*num_moved = moved;
	return DB_SUCCESS;
}

/****************************************************************//**
Check two MBRs are identical or need to be merged */
bool
rtr_merge_mbr_changed(
/*==================*/
	btr_cur_t*		cursor,		/*!< in/out: cursor */
	btr_cur_t*		cursor2,	/*!< in: the other cursor */
	rec_offs*		offsets,	/*!< in: rec offsets */
	rec_offs*		offsets2,	/*!< in: rec offsets */
	rtr_mbr_t*		new_mbr)	/*!< out: MBR to update */
{
	double*		mbr;
	double		mbr1[SPDIMS * 2];
	double		mbr2[SPDIMS * 2];
	rec_t*		rec;
	ulint		len;
	bool		changed = false;

	ut_ad(cursor->index()->is_spatial());

	rec = btr_cur_get_rec(cursor);

	rtr_read_mbr(rec_get_nth_field(rec, offsets, 0, &len),
		     reinterpret_cast<rtr_mbr_t*>(mbr1));

	rec = btr_cur_get_rec(cursor2);

	rtr_read_mbr(rec_get_nth_field(rec, offsets2, 0, &len),
		     reinterpret_cast<rtr_mbr_t*>(mbr2));

	mbr = reinterpret_cast<double*>(new_mbr);

	for (int i = 0; i < SPDIMS * 2; i += 2) {
		changed = (changed || mbr1[i] != mbr2[i]);
		*mbr = mbr1[i] < mbr2[i] ? mbr1[i] : mbr2[i];
		mbr++;
		changed = (changed || mbr1[i + 1] != mbr2 [i + 1]);
		*mbr = mbr1[i + 1] > mbr2[i + 1] ? mbr1[i + 1] : mbr2[i + 1];
		mbr++;
	}

	return(changed);
}

/****************************************************************//**
Merge 2 mbrs and update the the mbr that cursor is on. */
void
rtr_merge_and_update_mbr(
/*=====================*/
	btr_cur_t*		cursor,		/*!< in/out: cursor */
	btr_cur_t*		cursor2,	/*!< in: the other cursor */
	rec_offs*		offsets,	/*!< in: rec offsets */
	rec_offs*		offsets2,	/*!< in: rec offsets */
	page_t*			child_page,	/*!< in: the page. */
	mtr_t*			mtr)		/*!< in: mtr */
{
	rtr_mbr_t		new_mbr;

	if (rtr_merge_mbr_changed(cursor, cursor2, offsets, offsets2,
				  &new_mbr)) {
		rtr_update_mbr_field(cursor, offsets, cursor2, child_page,
				     &new_mbr, NULL, mtr);
	} else {
		rtr_node_ptr_delete(cursor2, mtr);
	}
}

/*************************************************************//**
Deletes on the upper level the node pointer to a page. */
void
rtr_node_ptr_delete(
/*================*/
	btr_cur_t*	cursor, /*!< in: search cursor, contains information
				about parent nodes in search */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ibool		compressed;
	dberr_t		err;

	compressed = btr_cur_pessimistic_delete(&err, TRUE, cursor,
						BTR_CREATE_FLAG, false, mtr);
	ut_a(err == DB_SUCCESS);

	if (!compressed) {
		btr_cur_compress_if_useful(cursor, FALSE, mtr);
	}
}

/**************************************************************//**
Check whether a Rtree page is child of a parent page
@return true if there is child/parent relationship */
bool
rtr_check_same_block(
/*================*/
	dict_index_t*	index,	/*!< in: index tree */
	btr_cur_t*	cursor,	/*!< in/out: position at the parent entry
				pointing to the child if successful */
	buf_block_t*	parentb,/*!< in: parent page to check */
	mem_heap_t*	heap)	/*!< in: memory heap */

{
	const uint32_t	page_no =
		btr_cur_get_block(cursor)->page.id().page_no();
	rec_offs*	offsets;
	rec_t*		rec = page_get_infimum_rec(parentb->page.frame);

	while ((rec = page_rec_get_next(rec)) && !page_rec_is_supremum(rec)) {
		offsets = rec_get_offsets(
			rec, index, NULL, 0, ULINT_UNDEFINED, &heap);

		if (btr_node_ptr_get_child_page_no(rec, offsets) == page_no) {
			btr_cur_position(index, rec, parentb, cursor);
			return(true);
		}
	}

	return(false);
}

/*************************************************************//**
Calculates MBR_AREA(a+b) - MBR_AREA(a)
Note: when 'a' and 'b' objects are far from each other,
the area increase can be really big, so this function
can return 'inf' as a result.
Return the area increaed. */
static double
rtree_area_increase(
	const uchar*	a,		/*!< in: original mbr. */
	const uchar*	b,		/*!< in: new mbr. */
	double*		ab_area)	/*!< out: increased area. */
{
	double		a_area = 1.0;
	double		loc_ab_area = 1.0;
	double		amin, amax, bmin, bmax;
	double		data_round = 1.0;

	static_assert(DATA_MBR_LEN == SPDIMS * 2 * sizeof(double),
		      "compatibility");

	for (auto i = SPDIMS; i--; ) {
		double	area;

		amin = mach_double_read(a);
		bmin = mach_double_read(b);
		amax = mach_double_read(a + sizeof(double));
		bmax = mach_double_read(b + sizeof(double));

		a += 2 * sizeof(double);
		b += 2 * sizeof(double);

		area = amax - amin;
		if (area == 0) {
			a_area *= LINE_MBR_WEIGHTS;
		} else {
			a_area *= area;
		}

		area = (double)std::max(amax, bmax) -
		       (double)std::min(amin, bmin);
		if (area == 0) {
			loc_ab_area *= LINE_MBR_WEIGHTS;
		} else {
			loc_ab_area *= area;
		}

		/* Value of amax or bmin can be so large that small difference
		are ignored. For example: 3.2884281489988079e+284 - 100 =
		3.2884281489988079e+284. This results some area difference
		are not detected */
		if (loc_ab_area == a_area) {
			if (bmin < amin || bmax > amax) {
				data_round *= ((double)std::max(amax, bmax)
					       - amax
					       + (amin - (double)std::min(
								amin, bmin)));
			} else {
				data_round *= area;
			}
		}
	}

	*ab_area = loc_ab_area;

	if (loc_ab_area == a_area && data_round != 1.0) {
		return(data_round);
	}

	return(loc_ab_area - a_area);
}

/** Calculates overlapping area
@param[in]	a	mbr a
@param[in]	b	mbr b
@return overlapping area */
static double rtree_area_overlapping(const byte *a, const byte *b)
{
	double	area = 1.0;
	double	amin;
	double	amax;
	double	bmin;
	double	bmax;

	static_assert(DATA_MBR_LEN == SPDIMS * 2 * sizeof(double),
		      "compatibility");

	for (auto i = SPDIMS; i--; ) {
		amin = mach_double_read(a);
		bmin = mach_double_read(b);
		amax = mach_double_read(a + sizeof(double));
		bmax = mach_double_read(b + sizeof(double));
		a += 2 * sizeof(double);
		b += 2 * sizeof(double);

		amin = std::max(amin, bmin);
		amax = std::min(amax, bmax);

		if (amin > amax) {
			return(0);
		} else {
			area *= (amax - amin);
		}
	}

	return(area);
}

/****************************************************************//**
Calculate the area increased for a new record
@return area increased */
double
rtr_rec_cal_increase(
/*=================*/
	const dtuple_t*	dtuple,	/*!< in: data tuple to insert, which
				cause area increase */
	const rec_t*	rec,	/*!< in: physical record which differs from
				dtuple in some of the common fields, or which
				has an equal number or more fields than
				dtuple */
	double*		area)	/*!< out: increased area */
{
	const dfield_t*	dtuple_field;

	ut_ad(!page_rec_is_supremum(rec));
	ut_ad(!page_rec_is_infimum(rec));

	dtuple_field = dtuple_get_nth_field(dtuple, 0);
	ut_ad(dfield_get_len(dtuple_field) == DATA_MBR_LEN);

	return rtree_area_increase(rec,
				   static_cast<const byte*>(
					   dfield_get_data(dtuple_field)),
				   area);
}

/** Estimates the number of rows in a given area.
@param[in]	index	index
@param[in]	tuple	range tuple containing mbr, may also be empty tuple
@param[in]	mode	search mode
@return estimated number of rows */
ha_rows
rtr_estimate_n_rows_in_range(
	dict_index_t*	index,
	const dtuple_t*	tuple,
	page_cur_mode_t	mode)
{
	ut_ad(dict_index_is_spatial(index));

	/* Check tuple & mode */
	if (tuple->n_fields == 0) {
		return(HA_POS_ERROR);
	}

	switch (mode) {
	case PAGE_CUR_DISJOINT:
	case PAGE_CUR_CONTAIN:
	case PAGE_CUR_INTERSECT:
	case PAGE_CUR_WITHIN:
	case PAGE_CUR_MBR_EQUAL:
		break;
	default:
		return(HA_POS_ERROR);
	}

	DBUG_EXECUTE_IF("rtr_pcur_move_to_next_return",
		return(2);
	);

	/* Read mbr from tuple. */
	rtr_mbr_t	range_mbr;
	double		range_area;

	const dfield_t* dtuple_field = dtuple_get_nth_field(tuple, 0);
	ut_ad(dfield_get_len(dtuple_field) >= DATA_MBR_LEN);
	const byte* range_mbr_ptr = reinterpret_cast<const byte*>(
		dfield_get_data(dtuple_field));

	rtr_read_mbr(range_mbr_ptr, &range_mbr);
	range_area = (range_mbr.xmax - range_mbr.xmin)
		 * (range_mbr.ymax - range_mbr.ymin);

	/* Get index root page. */
	mtr_t		mtr;

	mtr.start();
	index->set_modified(mtr);
	mtr_s_lock_index(index, &mtr);

	dberr_t err;
	buf_block_t* block = btr_root_block_get(index, RW_S_LATCH, &mtr, &err);
	if (!block) {
err_exit:
		mtr.commit();
		return HA_POS_ERROR;
	}
	const page_t* page = buf_block_get_frame(block);
	const unsigned n_recs = page_header_get_field(page, PAGE_N_RECS);

	if (n_recs == 0) {
		goto err_exit;
	}

	/* Scan records in root page and calculate area. */
	double	area = 0;
	for (const rec_t* rec = page_rec_get_next_const(
		     page_get_infimum_rec(block->page.frame));
	     rec && !page_rec_is_supremum(rec);
	     rec = page_rec_get_next_const(rec)) {
		rtr_mbr_t	mbr;
		double		rec_area;

		rtr_read_mbr(rec, &mbr);

		rec_area = (mbr.xmax - mbr.xmin) * (mbr.ymax - mbr.ymin);

		if (rec_area == 0) {
			switch (mode) {
			case PAGE_CUR_CONTAIN:
			case PAGE_CUR_INTERSECT:
				area += 1;
				break;

			case PAGE_CUR_DISJOINT:
				break;

			case PAGE_CUR_WITHIN:
			case PAGE_CUR_MBR_EQUAL:
				if (!rtree_key_cmp(
					    PAGE_CUR_WITHIN, range_mbr_ptr,
					    rec)) {
					area += 1;
				}

				break;

			default:
				ut_error;
			}
		} else {
			switch (mode) {
			case PAGE_CUR_CONTAIN:
			case PAGE_CUR_INTERSECT:
				area += rtree_area_overlapping(
					range_mbr_ptr, rec)
					/ rec_area;
				break;

			case PAGE_CUR_DISJOINT:
				area += 1;
				area -= rtree_area_overlapping(
					range_mbr_ptr, rec)
					/ rec_area;
				break;

			case PAGE_CUR_WITHIN:
			case PAGE_CUR_MBR_EQUAL:
				if (!rtree_key_cmp(
					    PAGE_CUR_WITHIN, range_mbr_ptr,
					    rec)) {
					area += range_area / rec_area;
				}

				break;
			default:
				ut_error;
			}
		}
	}

	mtr.commit();

	if (!std::isfinite(area)) {
		return(HA_POS_ERROR);
	}

	area /= n_recs;
	return ha_rows(static_cast<double>(dict_table_get_n_rows(index->table))
		       * area);
}
