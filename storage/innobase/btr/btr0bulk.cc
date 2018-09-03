/*****************************************************************************

Copyright (c) 2014, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file btr/btr0bulk.cc
The B-tree bulk load

Created 03/11/2014 Shaohua Wang
*******************************************************/

#include "btr0bulk.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "ibuf0ibuf.h"
#include "trx0trx.h"

/** Innodb B-tree index fill factor for bulk load. */
long	innobase_fill_factor;
/** whether to reduce redo logging during ALTER TABLE */
my_bool	innodb_log_optimize_ddl;

/** Initialize members, allocate page if needed and start mtr.
Note: we commit all mtrs on failure.
@return error code. */
dberr_t
PageBulk::init()
{
	buf_block_t*	new_block;
	page_t*		new_page;
	page_zip_des_t*	new_page_zip;
	ulint		new_page_no;

	ut_ad(m_heap == NULL);
	m_heap = mem_heap_create(1000);

	m_mtr.start();
	mtr_x_lock(&m_index->lock, &m_mtr);
	if (m_flush_observer) {
		m_mtr.set_log_mode(MTR_LOG_NO_REDO);
		m_mtr.set_flush_observer(m_flush_observer);
	} else {
		m_mtr.set_named_space(m_index->space);
	}

	if (m_page_no == FIL_NULL) {
		mtr_t	alloc_mtr;

		/* We commit redo log for allocation by a separate mtr,
		because we don't guarantee pages are committed following
		the allocation order, and we will always generate redo log
		for page allocation, even when creating a new tablespace. */
		alloc_mtr.start();
		alloc_mtr.set_named_space(m_index->space);

		ulint	n_reserved;
		bool	success;
		success = fsp_reserve_free_extents(&n_reserved, m_index->space,
						   1, FSP_NORMAL, &alloc_mtr);
		if (!success) {
			alloc_mtr.commit();
			m_mtr.commit();
			return(DB_OUT_OF_FILE_SPACE);
		}

		/* Allocate a new page. */
		new_block = btr_page_alloc(m_index, 0, FSP_UP, m_level,
					   &alloc_mtr, &m_mtr);

		if (n_reserved > 0) {
			fil_space_release_free_extents(m_index->space,
						       n_reserved);
		}

		alloc_mtr.commit();

		new_page = buf_block_get_frame(new_block);
		new_page_zip = buf_block_get_page_zip(new_block);
		new_page_no = page_get_page_no(new_page);

		if (new_page_zip) {
			page_create_zip(new_block, m_index, m_level, 0,
					NULL, &m_mtr);
			memset(FIL_PAGE_PREV + new_page, 0xff, 8);
			page_zip_write_header(new_page_zip,
					      FIL_PAGE_PREV + new_page,
					      8, &m_mtr);
			mach_write_to_8(PAGE_HEADER + PAGE_INDEX_ID + new_page,
					m_index->id);
			page_zip_write_header(new_page_zip,
					      PAGE_HEADER + PAGE_INDEX_ID
					      + new_page, 8, &m_mtr);
		} else {
			ut_ad(!dict_index_is_spatial(m_index));
			page_create(new_block, &m_mtr,
				    dict_table_is_comp(m_index->table),
				    false);
			mlog_write_ulint(FIL_PAGE_PREV + new_page, FIL_NULL,
					 MLOG_4BYTES, &m_mtr);
			mlog_write_ulint(FIL_PAGE_NEXT + new_page, FIL_NULL,
					 MLOG_4BYTES, &m_mtr);
			mlog_write_ulint(PAGE_HEADER + PAGE_LEVEL + new_page,
					 m_level, MLOG_2BYTES, &m_mtr);
			mlog_write_ull(PAGE_HEADER + PAGE_INDEX_ID + new_page,
				       m_index->id, &m_mtr);
		}
	} else {
		page_id_t	page_id(dict_index_get_space(m_index), m_page_no);
		page_size_t	page_size(dict_table_page_size(m_index->table));

		new_block = btr_block_get(page_id, page_size,
					  RW_X_LATCH, m_index, &m_mtr);

		new_page = buf_block_get_frame(new_block);
		new_page_zip = buf_block_get_page_zip(new_block);
		new_page_no = page_get_page_no(new_page);
		ut_ad(m_page_no == new_page_no);

		ut_ad(page_dir_get_n_heap(new_page) == PAGE_HEAP_NO_USER_LOW);

		btr_page_set_level(new_page, new_page_zip, m_level, &m_mtr);
	}

	if (!m_level && dict_index_is_sec_or_ibuf(m_index)) {
		page_update_max_trx_id(new_block, new_page_zip, m_trx_id,
				       &m_mtr);
	}

	m_block = new_block;
	m_block->skip_flush_check = true;
	m_page = new_page;
	m_page_zip = new_page_zip;
	m_page_no = new_page_no;
	m_cur_rec = page_get_infimum_rec(new_page);
	ut_ad(m_is_comp == !!page_is_comp(new_page));
	m_free_space = page_get_free_space_of_empty(m_is_comp);

	if (innobase_fill_factor == 100 && dict_index_is_clust(m_index)) {
		/* Keep default behavior compatible with 5.6 */
		m_reserved_space = dict_index_get_space_reserve();
	} else {
		m_reserved_space =
			UNIV_PAGE_SIZE * (100 - innobase_fill_factor) / 100;
	}

	m_padding_space =
		UNIV_PAGE_SIZE - dict_index_zip_pad_optimal_page_size(m_index);
	m_heap_top = page_header_get_ptr(new_page, PAGE_HEAP_TOP);
	m_rec_no = page_header_get_field(new_page, PAGE_N_RECS);

	ut_d(m_total_data = 0);
	/* See page_copy_rec_list_end_to_created_page() */
	ut_d(page_header_set_field(m_page, NULL, PAGE_HEAP_TOP,
				   srv_page_size - 1));

	return(DB_SUCCESS);
}

/** Insert a record in the page.
@param[in]	rec		record
@param[in]	offsets		record offsets */
void
PageBulk::insert(
	const rec_t*		rec,
	ulint*			offsets)
{
	ulint		rec_size;

	ut_ad(m_heap != NULL);

	rec_size = rec_offs_size(offsets);

#ifdef UNIV_DEBUG
	/* Check whether records are in order. */
	if (!page_rec_is_infimum(m_cur_rec)) {
		rec_t*	old_rec = m_cur_rec;
		ulint*	old_offsets = rec_get_offsets(
			old_rec, m_index, NULL,	page_rec_is_leaf(old_rec),
			ULINT_UNDEFINED, &m_heap);

		ut_ad(cmp_rec_rec(rec, old_rec, offsets, old_offsets, m_index)
		      > 0);
	}

	m_total_data += rec_size;
#endif /* UNIV_DEBUG */

	/* 1. Copy the record to page. */
	rec_t*	insert_rec = rec_copy(m_heap_top, rec, offsets);
	rec_offs_make_valid(insert_rec, m_index, offsets);

	/* 2. Insert the record in the linked list. */
	rec_t*	next_rec = page_rec_get_next(m_cur_rec);

	page_rec_set_next(insert_rec, next_rec);
	page_rec_set_next(m_cur_rec, insert_rec);

	/* 3. Set the n_owned field in the inserted record to zero,
	and set the heap_no field. */
	if (m_is_comp) {
		rec_set_n_owned_new(insert_rec, NULL, 0);
		rec_set_heap_no_new(insert_rec,
				    PAGE_HEAP_NO_USER_LOW + m_rec_no);
	} else {
		rec_set_n_owned_old(insert_rec, 0);
		rec_set_heap_no_old(insert_rec,
				    PAGE_HEAP_NO_USER_LOW + m_rec_no);
	}

	/* 4. Set member variables. */
	ulint		slot_size;
	slot_size = page_dir_calc_reserved_space(m_rec_no + 1)
		- page_dir_calc_reserved_space(m_rec_no);

	ut_ad(m_free_space >= rec_size + slot_size);
	ut_ad(m_heap_top + rec_size < m_page + UNIV_PAGE_SIZE);

	m_free_space -= rec_size + slot_size;
	m_heap_top += rec_size;
	m_rec_no += 1;

	if (!m_flush_observer && !m_page_zip) {
		/* For ROW_FORMAT=COMPRESSED, redo log may be written
		in PageBulk::compress(). */
		page_cur_insert_rec_write_log(insert_rec, rec_size,
					      m_cur_rec, m_index, &m_mtr);
	}

	m_cur_rec = insert_rec;
}

/** Mark end of insertion to the page. Scan all records to set page dirs,
and set page header members.
Note: we refer to page_copy_rec_list_end_to_created_page. */
void
PageBulk::finish()
{
	ut_ad(m_rec_no > 0);
	ut_ad(m_total_data + page_dir_calc_reserved_space(m_rec_no)
	      <= page_get_free_space_of_empty(m_is_comp));
	/* See page_copy_rec_list_end_to_created_page() */
	ut_d(page_dir_set_n_slots(m_page, NULL, srv_page_size / 2));

	ulint	count = 0;
	ulint	n_recs = 0;
	ulint	slot_index = 0;
	rec_t*	insert_rec = page_rec_get_next(page_get_infimum_rec(m_page));
	page_dir_slot_t* slot = NULL;

	/* Set owner & dir. */
	do {

		count++;
		n_recs++;

		if (count == (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2) {

			slot_index++;

			slot = page_dir_get_nth_slot(m_page, slot_index);

			page_dir_slot_set_rec(slot, insert_rec);
			page_dir_slot_set_n_owned(slot, NULL, count);

			count = 0;
		}

		insert_rec = page_rec_get_next(insert_rec);
	} while (!page_rec_is_supremum(insert_rec));

	if (slot_index > 0
	    && (count + 1 + (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2
		<= PAGE_DIR_SLOT_MAX_N_OWNED)) {
		/* We can merge the two last dir slots. This operation is
		here to make this function imitate exactly the equivalent
		task made using page_cur_insert_rec, which we use in database
		recovery to reproduce the task performed by this function.
		To be able to check the correctness of recovery, it is good
		that it imitates exactly. */

		count += (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2;

		page_dir_slot_set_n_owned(slot, NULL, 0);

		slot_index--;
	}

	slot = page_dir_get_nth_slot(m_page, 1 + slot_index);
	page_dir_slot_set_rec(slot, page_get_supremum_rec(m_page));
	page_dir_slot_set_n_owned(slot, NULL, count + 1);

	ut_ad(!dict_index_is_spatial(m_index));

	if (!m_flush_observer && !m_page_zip) {
		mlog_write_ulint(PAGE_HEADER + PAGE_N_DIR_SLOTS + m_page,
				 2 + slot_index, MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_HEAP_TOP + m_page,
				 ulint(m_heap_top - m_page),
				 MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_N_HEAP + m_page,
				 (PAGE_HEAP_NO_USER_LOW + m_rec_no)
				 | ulint(m_is_comp) << 15,
				 MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_N_RECS + m_page, m_rec_no,
				 MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_LAST_INSERT + m_page,
				 ulint(m_cur_rec - m_page),
				 MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_DIRECTION + m_page,
				 PAGE_RIGHT, MLOG_2BYTES, &m_mtr);
		mlog_write_ulint(PAGE_HEADER + PAGE_N_DIRECTION + m_page, 0,
				 MLOG_2BYTES, &m_mtr);
	} else {
		/* For ROW_FORMAT=COMPRESSED, redo log may be written
		in PageBulk::compress(). */
		mach_write_to_2(PAGE_HEADER + PAGE_N_DIR_SLOTS + m_page,
				2 + slot_index);
		mach_write_to_2(PAGE_HEADER + PAGE_HEAP_TOP + m_page,
				ulint(m_heap_top - m_page));
		mach_write_to_2(PAGE_HEADER + PAGE_N_HEAP + m_page,
				(PAGE_HEAP_NO_USER_LOW + m_rec_no)
				| ulint(m_is_comp) << 15);
		mach_write_to_2(PAGE_HEADER + PAGE_N_RECS + m_page, m_rec_no);
		mach_write_to_2(PAGE_HEADER + PAGE_LAST_INSERT + m_page,
				ulint(m_cur_rec - m_page));
		mach_write_to_2(PAGE_HEADER + PAGE_DIRECTION + m_page,
				PAGE_RIGHT);
		mach_write_to_2(PAGE_HEADER + PAGE_N_DIRECTION + m_page, 0);
	}

	m_block->skip_flush_check = false;
}

/** Commit inserts done to the page
@param[in]	success		Flag whether all inserts succeed. */
void
PageBulk::commit(
	bool	success)
{
	if (success) {
		ut_ad(page_validate(m_page, m_index));

		/* Set no free space left and no buffered changes in ibuf. */
		if (!dict_index_is_clust(m_index) && page_is_leaf(m_page)) {
			ibuf_set_bitmap_for_bulk_load(
				m_block, innobase_fill_factor == 100);
		}
	}

	m_mtr.commit();
}

/** Compress a page of compressed table
@return	true	compress successfully or no need to compress
@return	false	compress failed. */
bool
PageBulk::compress()
{
	ut_ad(m_page_zip != NULL);

	return(page_zip_compress(m_page_zip, m_page, m_index,
				 page_zip_level, NULL, &m_mtr));
}

/** Get node pointer
@return node pointer */
dtuple_t*
PageBulk::getNodePtr()
{
	rec_t*		first_rec;
	dtuple_t*	node_ptr;

	/* Create node pointer */
	first_rec = page_rec_get_next(page_get_infimum_rec(m_page));
	ut_a(page_rec_is_user_rec(first_rec));
	node_ptr = dict_index_build_node_ptr(m_index, first_rec, m_page_no,
					     m_heap, m_level);

	return(node_ptr);
}

/** Get split rec in left page.We split a page in half when compresssion fails,
and the split rec will be copied to right page.
@return split rec */
rec_t*
PageBulk::getSplitRec()
{
	rec_t*		rec;
	ulint*		offsets;
	ulint		total_used_size;
	ulint		total_recs_size;
	ulint		n_recs;

	ut_ad(m_page_zip != NULL);
	ut_ad(m_rec_no >= 2);

	ut_ad(page_get_free_space_of_empty(m_is_comp) > m_free_space);
	total_used_size = page_get_free_space_of_empty(m_is_comp)
		- m_free_space;

	total_recs_size = 0;
	n_recs = 0;
	offsets = NULL;
	rec = page_get_infimum_rec(m_page);

	do {
		rec = page_rec_get_next(rec);
		ut_ad(page_rec_is_user_rec(rec));

		offsets = rec_get_offsets(rec, m_index, offsets,
					  page_is_leaf(m_page),
					  ULINT_UNDEFINED, &m_heap);
		total_recs_size += rec_offs_size(offsets);
		n_recs++;
	} while (total_recs_size + page_dir_calc_reserved_space(n_recs)
		 < total_used_size / 2);

	/* Keep at least one record on left page */
	if (page_rec_is_infimum(page_rec_get_prev(rec))) {
		rec = page_rec_get_next(rec);
		ut_ad(page_rec_is_user_rec(rec));
	}

	return(rec);
}

/** Copy all records after split rec including itself.
@param[in]	rec	split rec */
void
PageBulk::copyIn(
	rec_t*		split_rec)
{

	rec_t*		rec = split_rec;
	ulint*		offsets = NULL;

	ut_ad(m_rec_no == 0);
	ut_ad(page_rec_is_user_rec(rec));

	do {
		offsets = rec_get_offsets(rec, m_index, offsets,
					  page_rec_is_leaf(split_rec),
					  ULINT_UNDEFINED, &m_heap);

		insert(rec, offsets);

		rec = page_rec_get_next(rec);
	} while (!page_rec_is_supremum(rec));

	ut_ad(m_rec_no > 0);
}

/** Remove all records after split rec including itself.
@param[in]	rec	split rec	*/
void
PageBulk::copyOut(
	rec_t*		split_rec)
{
	rec_t*		rec;
	rec_t*		last_rec;
	ulint		n;

	/* Suppose before copyOut, we have 5 records on the page:
	infimum->r1->r2->r3->r4->r5->supremum, and r3 is the split rec.

	after copyOut, we have 2 records on the page:
	infimum->r1->r2->supremum. slot ajustment is not done. */

	rec = page_rec_get_next(page_get_infimum_rec(m_page));
	last_rec = page_rec_get_prev(page_get_supremum_rec(m_page));
	n = 0;

	while (rec != split_rec) {
		rec = page_rec_get_next(rec);
		n++;
	}

	ut_ad(n > 0);

	/* Set last record's next in page */
	ulint*		offsets = NULL;
	rec = page_rec_get_prev(split_rec);
	offsets = rec_get_offsets(rec, m_index, offsets,
				  page_rec_is_leaf(split_rec),
				  ULINT_UNDEFINED, &m_heap);
	page_rec_set_next(rec, page_get_supremum_rec(m_page));

	/* Set related members */
	m_cur_rec = rec;
	m_heap_top = rec_get_end(rec, offsets);

	offsets = rec_get_offsets(last_rec, m_index, offsets,
				  page_rec_is_leaf(split_rec),
				  ULINT_UNDEFINED, &m_heap);

	m_free_space += rec_get_end(last_rec, offsets)
		- m_heap_top
		+ page_dir_calc_reserved_space(m_rec_no)
		- page_dir_calc_reserved_space(n);
	ut_ad(m_free_space > 0);
	m_rec_no = n;

#ifdef UNIV_DEBUG
	m_total_data -= rec_get_end(last_rec, offsets) - m_heap_top;
#endif /* UNIV_DEBUG */
}

/** Set next page
@param[in]	next_page_no	next page no */
inline void PageBulk::setNext(ulint next_page_no)
{
	if (UNIV_LIKELY_NULL(m_page_zip)) {
		/* For ROW_FORMAT=COMPRESSED, redo log may be written
		in PageBulk::compress(). */
		mach_write_to_4(m_page + FIL_PAGE_NEXT, next_page_no);
	} else {
		mlog_write_ulint(m_page + FIL_PAGE_NEXT, next_page_no,
				 MLOG_4BYTES, &m_mtr);
	}
}

/** Set previous page
@param[in]	prev_page_no	previous page no */
inline void PageBulk::setPrev(ulint prev_page_no)
{
	if (UNIV_LIKELY_NULL(m_page_zip)) {
		/* For ROW_FORMAT=COMPRESSED, redo log may be written
		in PageBulk::compress(). */
		mach_write_to_4(m_page + FIL_PAGE_PREV, prev_page_no);
	} else {
		mlog_write_ulint(m_page + FIL_PAGE_PREV, prev_page_no,
				 MLOG_4BYTES, &m_mtr);
	}
}

/** Check if required space is available in the page for the rec to be inserted.
We check fill factor & padding here.
@param[in]	length		required length
@return true	if space is available */
bool
PageBulk::isSpaceAvailable(
	ulint		rec_size)
{
	ulint	slot_size;
	ulint	required_space;

	slot_size = page_dir_calc_reserved_space(m_rec_no + 1)
		- page_dir_calc_reserved_space(m_rec_no);

	required_space = rec_size + slot_size;

	if (required_space > m_free_space) {
		ut_ad(m_rec_no > 0);
		return false;
	}

	/* Fillfactor & Padding apply to both leaf and non-leaf pages.
	Note: we keep at least 2 records in a page to avoid B-tree level
	growing too high. */
	if (m_rec_no >= 2
	    && ((m_page_zip == NULL && m_free_space - required_space
		 < m_reserved_space)
		|| (m_page_zip != NULL && m_free_space - required_space
		    < m_padding_space))) {
		return(false);
	}

	return(true);
}

/** Check whether the record needs to be stored externally.
@return false if the entire record can be stored locally on the page  */
bool
PageBulk::needExt(
	const dtuple_t*		tuple,
	ulint			rec_size)
{
	return(page_zip_rec_needs_ext(rec_size, m_is_comp,
		dtuple_get_n_fields(tuple), m_block->page.size));
}

/** Store external record
Since the record is not logged yet, so we don't log update to the record.
the blob data is logged first, then the record is logged in bulk mode.
@param[in]	big_rec		external recrod
@param[in]	offsets		record offsets
@return	error code */
dberr_t
PageBulk::storeExt(
	const big_rec_t*	big_rec,
	ulint*			offsets)
{
	/* Note: not all fileds are initialized in btr_pcur. */
	btr_pcur_t	btr_pcur;
	btr_pcur.pos_state = BTR_PCUR_IS_POSITIONED;
	btr_pcur.latch_mode = BTR_MODIFY_LEAF;
	btr_pcur.btr_cur.index = m_index;

	page_cur_t*	page_cur = &btr_pcur.btr_cur.page_cur;
	page_cur->index = m_index;
	page_cur->rec = m_cur_rec;
	page_cur->offsets = offsets;
	page_cur->block = m_block;

	dberr_t	err = btr_store_big_rec_extern_fields(
		&btr_pcur, offsets, big_rec, &m_mtr, BTR_STORE_INSERT_BULK);

	ut_ad(page_offset(m_cur_rec) == page_offset(page_cur->rec));

	/* Reset m_block and m_cur_rec from page cursor, because
	block may be changed during blob insert. */
	m_block = page_cur->block;
	m_cur_rec = page_cur->rec;
	m_page = buf_block_get_frame(m_block);

	return(err);
}

/** Release block by commiting mtr
Note: log_free_check requires holding no lock/latch in current thread. */
void
PageBulk::release()
{
	ut_ad(!dict_index_is_spatial(m_index));

	/* We fix the block because we will re-pin it soon. */
	buf_block_buf_fix_inc(m_block, __FILE__, __LINE__);

	/* No other threads can modify this block. */
	m_modify_clock = buf_block_get_modify_clock(m_block);

	m_mtr.commit();
}

/** Start mtr and latch the block */
dberr_t
PageBulk::latch()
{
	m_mtr.start();
	mtr_x_lock(&m_index->lock, &m_mtr);
	if (m_flush_observer) {
		m_mtr.set_log_mode(MTR_LOG_NO_REDO);
		m_mtr.set_flush_observer(m_flush_observer);
	} else {
		m_mtr.set_named_space(m_index->space);
	}

	/* In case the block is S-latched by page_cleaner. */
	if (!buf_page_optimistic_get(RW_X_LATCH, m_block, m_modify_clock,
				     __FILE__, __LINE__, &m_mtr)) {
		page_id_t       page_id(dict_index_get_space(m_index), m_page_no);
		page_size_t     page_size(dict_table_page_size(m_index->table));

		m_block = buf_page_get_gen(page_id, page_size, RW_X_LATCH,
					   m_block, BUF_GET_IF_IN_POOL,
					   __FILE__, __LINE__, &m_mtr, &m_err);

		if (m_err != DB_SUCCESS) {
			return (m_err);
		}

		ut_ad(m_block != NULL);
	}

	buf_block_buf_fix_dec(m_block);

	ut_ad(m_cur_rec > m_page && m_cur_rec < m_heap_top);

	return (m_err);
}

/** Split a page
@param[in]	page_bulk	page to split
@param[in]	next_page_bulk	next page
@return	error code */
dberr_t
BtrBulk::pageSplit(
	PageBulk*	page_bulk,
	PageBulk*	next_page_bulk)
{
	ut_ad(page_bulk->getPageZip() != NULL);

	/* 1. Check if we have only one user record on the page. */
	if (page_bulk->getRecNo() <= 1) {
		return(DB_TOO_BIG_RECORD);
	}

	/* 2. create a new page. */
	PageBulk new_page_bulk(m_index, m_trx->id, FIL_NULL,
			       page_bulk->getLevel(), m_flush_observer);
	dberr_t	err = new_page_bulk.init();
	if (err != DB_SUCCESS) {
		return(err);
	}

	/* 3. copy the upper half to new page. */
	rec_t*	split_rec = page_bulk->getSplitRec();
	new_page_bulk.copyIn(split_rec);
	page_bulk->copyOut(split_rec);

	/* 4. commit the splitted page. */
	err = pageCommit(page_bulk, &new_page_bulk, true);
	if (err != DB_SUCCESS) {
		pageAbort(&new_page_bulk);
		return(err);
	}

	/* 5. commit the new page. */
	err = pageCommit(&new_page_bulk, next_page_bulk, true);
	if (err != DB_SUCCESS) {
		pageAbort(&new_page_bulk);
		return(err);
	}

	return(err);
}

/** Commit(finish) a page. We set next/prev page no, compress a page of
compressed table and split the page if compression fails, insert a node
pointer to father page if needed, and commit mini-transaction.
@param[in]	page_bulk	page to commit
@param[in]	next_page_bulk	next page
@param[in]	insert_father	false when page_bulk is a root page and
				true when it's a non-root page
@return	error code */
dberr_t
BtrBulk::pageCommit(
	PageBulk*	page_bulk,
	PageBulk*	next_page_bulk,
	bool		insert_father)
{
	page_bulk->finish();

	/* Set page links */
	if (next_page_bulk != NULL) {
		ut_ad(page_bulk->getLevel() == next_page_bulk->getLevel());

		page_bulk->setNext(next_page_bulk->getPageNo());
		next_page_bulk->setPrev(page_bulk->getPageNo());
	} else {
		/** Suppose a page is released and latched again, we need to
		mark it modified in mini-transaction.  */
		page_bulk->setNext(FIL_NULL);
	}

	/* Compress page if it's a compressed table. */
	if (page_bulk->getPageZip() != NULL && !page_bulk->compress()) {
		return(pageSplit(page_bulk, next_page_bulk));
	}

	/* Insert node pointer to father page. */
	if (insert_father) {
		dtuple_t*	node_ptr = page_bulk->getNodePtr();
		dberr_t		err = insert(node_ptr, page_bulk->getLevel()+1);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	/* Commit mtr. */
	page_bulk->commit(true);

	return(DB_SUCCESS);
}

/** Log free check */
inline void BtrBulk::logFreeCheck()
{
	if (log_sys->check_flush_or_checkpoint) {
		release();

		log_free_check();

		latch();
	}
}

/** Release all latches */
void
BtrBulk::release()
{
	ut_ad(m_root_level + 1 == m_page_bulks.size());

	for (ulint level = 0; level <= m_root_level; level++) {
		PageBulk*    page_bulk = m_page_bulks.at(level);

		page_bulk->release();
	}
}

/** Re-latch all latches */
void
BtrBulk::latch()
{
	ut_ad(m_root_level + 1 == m_page_bulks.size());

	for (ulint level = 0; level <= m_root_level; level++) {
		PageBulk*    page_bulk = m_page_bulks.at(level);
		page_bulk->latch();
	}
}

/** Insert a tuple to page in a level
@param[in]	tuple	tuple to insert
@param[in]	level	B-tree level
@return error code */
dberr_t
BtrBulk::insert(
	dtuple_t*	tuple,
	ulint		level)
{
	bool		is_left_most = false;
	dberr_t		err = DB_SUCCESS;

	/* Check if we need to create a PageBulk for the level. */
	if (level + 1 > m_page_bulks.size()) {
		PageBulk*	new_page_bulk
			= UT_NEW_NOKEY(PageBulk(m_index, m_trx->id, FIL_NULL,
						level, m_flush_observer));
		err = new_page_bulk->init();
		if (err != DB_SUCCESS) {
			return(err);
		}

		m_page_bulks.push_back(new_page_bulk);
		ut_ad(level + 1 == m_page_bulks.size());
		m_root_level = level;

		is_left_most = true;
	}

	ut_ad(m_page_bulks.size() > level);

	PageBulk*	page_bulk = m_page_bulks.at(level);

	if (is_left_most && level > 0 && page_bulk->getRecNo() == 0) {
		/* The node pointer must be marked as the predefined minimum
		record,	as there is no lower alphabetical limit to records in
		the leftmost node of a level: */
		dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple)
					    | REC_INFO_MIN_REC_FLAG);
	}

	ulint		n_ext = 0;
	ulint		rec_size = rec_get_converted_size(m_index, tuple, n_ext);
	big_rec_t*	big_rec = NULL;
	rec_t*		rec = NULL;
	ulint*		offsets = NULL;

	if (page_bulk->needExt(tuple, rec_size)) {
		/* The record is so big that we have to store some fields
		externally on separate database pages */
		big_rec = dtuple_convert_big_rec(m_index, 0, tuple, &n_ext);

		if (big_rec == NULL) {
			return(DB_TOO_BIG_RECORD);
		}

		rec_size = rec_get_converted_size(m_index, tuple, n_ext);
	}

	if (page_bulk->getPageZip() != NULL
	    && page_zip_is_too_big(m_index, tuple)) {
		err = DB_TOO_BIG_RECORD;
		goto func_exit;
	}

	if (!page_bulk->isSpaceAvailable(rec_size)) {
		/* Create a sibling page_bulk. */
		PageBulk*	sibling_page_bulk;
		sibling_page_bulk = UT_NEW_NOKEY(PageBulk(m_index, m_trx->id,
							  FIL_NULL, level,
							  m_flush_observer));
		err = sibling_page_bulk->init();
		if (err != DB_SUCCESS) {
			UT_DELETE(sibling_page_bulk);
			goto func_exit;
		}

		/* Commit page bulk. */
		err = pageCommit(page_bulk, sibling_page_bulk, true);
		if (err != DB_SUCCESS) {
			pageAbort(sibling_page_bulk);
			UT_DELETE(sibling_page_bulk);
			goto func_exit;
		}

		/* Set new page bulk to page_bulks. */
		ut_ad(sibling_page_bulk->getLevel() <= m_root_level);
		m_page_bulks.at(level) = sibling_page_bulk;

		UT_DELETE(page_bulk);
		page_bulk = sibling_page_bulk;

		/* Important: log_free_check whether we need a checkpoint. */
		if (page_is_leaf(sibling_page_bulk->getPage())) {
			if (trx_is_interrupted(m_trx)) {
				if (m_flush_observer) {
					m_flush_observer->interrupted();
				}

				err = DB_INTERRUPTED;
				goto func_exit;
			}

			/* Wake up page cleaner to flush dirty pages. */
			srv_inc_activity_count();
			os_event_set(buf_flush_event);

			logFreeCheck();
		}

	}

	/* Convert tuple to rec. */
        rec = rec_convert_dtuple_to_rec(static_cast<byte*>(mem_heap_alloc(
		page_bulk->m_heap, rec_size)), m_index, tuple, n_ext);
        offsets = rec_get_offsets(rec, m_index, offsets, !level,
				  ULINT_UNDEFINED, &page_bulk->m_heap);

	page_bulk->insert(rec, offsets);

	if (big_rec != NULL) {
		ut_ad(dict_index_is_clust(m_index));
		ut_ad(page_bulk->getLevel() == 0);
		ut_ad(page_bulk == m_page_bulks.at(0));

		/* Release all latched but leaf node. */
		for (ulint level = 1; level <= m_root_level; level++) {
			PageBulk*    page_bulk = m_page_bulks.at(level);

			page_bulk->release();
		}

		err = page_bulk->storeExt(big_rec, offsets);

		/* Latch */
		for (ulint level = 1; level <= m_root_level; level++) {
			PageBulk*    page_bulk = m_page_bulks.at(level);
			page_bulk->latch();
		}
	}

func_exit:
	if (big_rec != NULL) {
		dtuple_convert_back_big_rec(m_index, tuple, big_rec);
	}

	return(err);
}

/** Btree bulk load finish. We commit the last page in each level
and copy the last page in top level to the root page of the index
if no error occurs.
@param[in]	err	whether bulk load was successful until now
@return error code  */
dberr_t
BtrBulk::finish(dberr_t	err)
{
	ulint		last_page_no = FIL_NULL;

	ut_ad(!dict_table_is_temporary(m_index->table));

	if (m_page_bulks.size() == 0) {
		/* The table is empty. The root page of the index tree
		is already in a consistent state. No need to flush. */
		return(err);
	}

	ut_ad(m_root_level + 1 == m_page_bulks.size());

	/* Finish all page bulks */
	for (ulint level = 0; level <= m_root_level; level++) {
		PageBulk*	page_bulk = m_page_bulks.at(level);

		last_page_no = page_bulk->getPageNo();

		if (err == DB_SUCCESS) {
			err = pageCommit(page_bulk, NULL,
					 level != m_root_level);
		}

		if (err != DB_SUCCESS) {
			pageAbort(page_bulk);
		}

		UT_DELETE(page_bulk);
	}

	if (err == DB_SUCCESS) {
		rec_t*		first_rec;
		mtr_t		mtr;
		buf_block_t*	last_block;
		page_t*		last_page;
		page_id_t	page_id(dict_index_get_space(m_index),
					last_page_no);
		page_size_t	page_size(dict_table_page_size(m_index->table));
		ulint		root_page_no = dict_index_get_page(m_index);
		PageBulk	root_page_bulk(m_index, m_trx->id,
					       root_page_no, m_root_level,
					       m_flush_observer);

		mtr_start(&mtr);
		mtr.set_named_space(dict_index_get_space(m_index));
		mtr_x_lock(dict_index_get_lock(m_index), &mtr);

		ut_ad(last_page_no != FIL_NULL);
		last_block = btr_block_get(page_id, page_size,
					   RW_X_LATCH, m_index, &mtr);
		last_page = buf_block_get_frame(last_block);
		first_rec = page_rec_get_next(page_get_infimum_rec(last_page));
		ut_ad(page_rec_is_user_rec(first_rec));

		/* Copy last page to root page. */
		err = root_page_bulk.init();
		if (err != DB_SUCCESS) {
			mtr_commit(&mtr);
			return(err);
		}
		root_page_bulk.copyIn(first_rec);

		/* Remove last page. */
		btr_page_free_low(m_index, last_block, m_root_level, false, &mtr);

		/* Do not flush the last page. */
		last_block->page.flush_observer = NULL;

		mtr_commit(&mtr);

		err = pageCommit(&root_page_bulk, NULL, false);
		ut_ad(err == DB_SUCCESS);
	}

	ut_ad(!sync_check_iterate(dict_sync_check()));

	ut_ad(err != DB_SUCCESS || btr_validate_index(m_index, NULL, false));
	return(err);
}
