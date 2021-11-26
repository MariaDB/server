/*****************************************************************************

Copyright (c) 2014, 2019, Oracle and/or its affiliates. All Rights Reserved.
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
@file btr/btr0bulk.cc
The B-tree bulk load

Created 03/11/2014 Shaohua Wang
*******************************************************/

#include "btr0bulk.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "ibuf0ibuf.h"
#include "page0page.h"
#include "trx0trx.h"

/** Innodb B-tree index fill factor for bulk load. */
uint	innobase_fill_factor;

/** Initialize members, allocate page if needed and start mtr.
Note: we commit all mtrs on failure.
@return error code. */
dberr_t
PageBulk::init()
{
	buf_block_t*	new_block;
	page_t*		new_page;

	ut_ad(m_heap == NULL);
	m_heap = mem_heap_create(1000);

	m_mtr.start();
	m_index->set_modified(m_mtr);

	if (m_page_no == FIL_NULL) {
		mtr_t	alloc_mtr;

		/* We commit redo log for allocation by a separate mtr,
		because we don't guarantee pages are committed following
		the allocation order, and we will always generate redo log
		for page allocation, even when creating a new tablespace. */
		alloc_mtr.start();
		m_index->set_modified(alloc_mtr);

		uint32_t n_reserved;
		if (!fsp_reserve_free_extents(&n_reserved,
					      m_index->table->space,
					      1, FSP_NORMAL, &alloc_mtr)) {
			alloc_mtr.commit();
			m_mtr.commit();
			return(DB_OUT_OF_FILE_SPACE);
		}

		/* Allocate a new page. */
		new_block = btr_page_alloc(m_index, 0, FSP_UP, m_level,
					   &alloc_mtr, &m_mtr);

		m_index->table->space->release_free_extents(n_reserved);

		alloc_mtr.commit();

		new_page = buf_block_get_frame(new_block);
		m_page_no = new_block->page.id().page_no();

		byte* index_id = my_assume_aligned<2>
			(PAGE_HEADER + PAGE_INDEX_ID + new_page);
		compile_time_assert(FIL_PAGE_NEXT == FIL_PAGE_PREV + 4);
		compile_time_assert(FIL_NULL == 0xffffffff);
		memset_aligned<8>(new_page + FIL_PAGE_PREV, 0xff, 8);

		if (UNIV_LIKELY_NULL(new_block->page.zip.data)) {
			mach_write_to_8(index_id, m_index->id);
			page_create_zip(new_block, m_index, m_level, 0,
					&m_mtr);
		} else {
			ut_ad(!m_index->is_spatial());
			page_create(new_block, &m_mtr,
				    m_index->table->not_redundant());
			m_mtr.memset(*new_block, FIL_PAGE_PREV, 8, 0xff);
			m_mtr.write<2,mtr_t::MAYBE_NOP>(*new_block, PAGE_HEADER
							+ PAGE_LEVEL
							+ new_page, m_level);
			m_mtr.write<8>(*new_block, index_id, m_index->id);
		}
	} else {
		new_block = btr_block_get(*m_index, m_page_no, RW_X_LATCH,
					  false, &m_mtr);

		new_page = buf_block_get_frame(new_block);
		ut_ad(new_block->page.id().page_no() == m_page_no);

		ut_ad(page_dir_get_n_heap(new_page) == PAGE_HEAP_NO_USER_LOW);

		btr_page_set_level(new_block, m_level, &m_mtr);
	}

	m_page_zip = buf_block_get_page_zip(new_block);

	if (!m_level && dict_index_is_sec_or_ibuf(m_index)) {
		page_update_max_trx_id(new_block, m_page_zip, m_trx_id,
				       &m_mtr);
	}

	m_block = new_block;
	m_page = new_page;
	m_cur_rec = page_get_infimum_rec(new_page);
	ut_ad(m_is_comp == !!page_is_comp(new_page));
	m_free_space = page_get_free_space_of_empty(m_is_comp);

	if (innobase_fill_factor == 100 && dict_index_is_clust(m_index)) {
		/* Keep default behavior compatible with 5.6 */
		m_reserved_space = dict_index_get_space_reserve();
	} else {
		m_reserved_space =
			srv_page_size * (100 - innobase_fill_factor) / 100;
	}

	m_padding_space =
		srv_page_size - dict_index_zip_pad_optimal_page_size(m_index);
	m_heap_top = page_header_get_ptr(new_page, PAGE_HEAP_TOP);
	m_rec_no = page_header_get_field(new_page, PAGE_N_RECS);
	/* Temporarily reset PAGE_DIRECTION_B from PAGE_NO_DIRECTION to 0,
	without writing redo log, to ensure that needs_finish() will hold
	on an empty page. */
	ut_ad(m_page[PAGE_HEADER + PAGE_DIRECTION_B] == PAGE_NO_DIRECTION);
	m_page[PAGE_HEADER + PAGE_DIRECTION_B] = 0;
	ut_d(m_total_data = 0);

	return(DB_SUCCESS);
}

/** Insert a record in the page.
@tparam fmt     the page format
@param[in,out]	rec		record
@param[in]	offsets		record offsets */
template<PageBulk::format fmt>
inline void PageBulk::insertPage(rec_t *rec, rec_offs *offsets)
{
  ut_ad((m_page_zip != nullptr) == (fmt == COMPRESSED));
  ut_ad((fmt != REDUNDANT) == m_is_comp);
  ut_ad(page_align(m_heap_top) == m_page);
  ut_ad(m_heap);

  const ulint rec_size= rec_offs_size(offsets);
  const ulint extra_size= rec_offs_extra_size(offsets);
  ut_ad(page_align(m_heap_top + rec_size) == m_page);
  ut_d(const bool is_leaf= page_rec_is_leaf(m_cur_rec));

#ifdef UNIV_DEBUG
  /* Check whether records are in order. */
  if (page_offset(m_cur_rec) !=
      (fmt == REDUNDANT ? PAGE_OLD_INFIMUM : PAGE_NEW_INFIMUM))
  {
    const rec_t *old_rec = m_cur_rec;
    rec_offs *old_offsets= rec_get_offsets(old_rec, m_index, nullptr, is_leaf
                                           ? m_index->n_core_fields : 0,
                                           ULINT_UNDEFINED, &m_heap);
    ut_ad(cmp_rec_rec(rec, old_rec, offsets, old_offsets, m_index) > 0);
  }

  m_total_data+= rec_size;
#endif /* UNIV_DEBUG */

  rec_t* const insert_rec= m_heap_top + extra_size;

  /* Insert the record in the linked list. */
  if (fmt != REDUNDANT)
  {
    const rec_t *next_rec= m_page +
      page_offset(m_cur_rec + mach_read_from_2(m_cur_rec - REC_NEXT));
    if (fmt != COMPRESSED)
      m_mtr.write<2>(*m_block, m_cur_rec - REC_NEXT,
                     static_cast<uint16_t>(insert_rec - m_cur_rec));
    else
    {
      mach_write_to_2(m_cur_rec - REC_NEXT,
                      static_cast<uint16_t>(insert_rec - m_cur_rec));
      memcpy(m_heap_top, rec - extra_size, rec_size);
    }

    rec_t * const this_rec= fmt != COMPRESSED
      ? const_cast<rec_t*>(rec) : insert_rec;
    rec_set_bit_field_1(this_rec, 0, REC_NEW_N_OWNED, REC_N_OWNED_MASK,
                        REC_N_OWNED_SHIFT);
    rec_set_bit_field_2(this_rec, PAGE_HEAP_NO_USER_LOW + m_rec_no,
                        REC_NEW_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    mach_write_to_2(this_rec - REC_NEXT,
                    static_cast<uint16_t>(next_rec - insert_rec));
  }
  else
  {
    memcpy(const_cast<rec_t*>(rec) - REC_NEXT, m_cur_rec - REC_NEXT, 2);
    m_mtr.write<2>(*m_block, m_cur_rec - REC_NEXT, page_offset(insert_rec));
    rec_set_bit_field_1(const_cast<rec_t*>(rec), 0,
                        REC_OLD_N_OWNED, REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    rec_set_bit_field_2(const_cast<rec_t*>(rec),
                        PAGE_HEAP_NO_USER_LOW + m_rec_no,
                        REC_OLD_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
  }

  if (fmt == COMPRESSED)
    /* We already wrote the record. Log is written in PageBulk::compress(). */;
  else if (page_offset(m_cur_rec) ==
           (fmt == REDUNDANT ? PAGE_OLD_INFIMUM : PAGE_NEW_INFIMUM))
    m_mtr.memcpy(*m_block, m_heap_top, rec - extra_size, rec_size);
  else
  {
    /* Try to copy common prefix from the preceding record. */
    const byte *r= rec - extra_size;
    const byte * const insert_rec_end= m_heap_top + rec_size;
    byte *b= m_heap_top;

    /* Skip any unchanged prefix of the record. */
    for (; * b == *r; b++, r++);

    ut_ad(b < insert_rec_end);

    const byte *c= m_cur_rec - (rec - r);
    const byte * const c_end= std::min(m_cur_rec + rec_offs_data_size(offsets),
                                       m_heap_top);

    /* Try to copy any bytes of the preceding record. */
    if (UNIV_LIKELY(c >= m_page && c < c_end))
    {
      const byte *cm= c;
      byte *bm= b;
      const byte *rm= r;
      for (; cm < c_end && *rm == *cm; cm++, bm++, rm++);
      ut_ad(bm <= insert_rec_end);
      size_t len= static_cast<size_t>(rm - r);
      ut_ad(!memcmp(r, c, len));
      if (len > 2)
      {
        memcpy(b, c, len);
        m_mtr.memmove(*m_block, page_offset(b), page_offset(c), len);
        c= cm;
        b= bm;
        r= rm;
      }
    }

    if (c < m_cur_rec)
    {
      if (!rec_offs_data_size(offsets))
      {
no_data:
        m_mtr.memcpy<mtr_t::FORCED>(*m_block, b, r, m_cur_rec - c);
        goto rec_done;
      }
      /* Some header bytes differ. Compare the data separately. */
      const byte *cd= m_cur_rec;
      byte *bd= insert_rec;
      const byte *rd= rec;
      /* Skip any unchanged prefix of the record. */
      for (;; cd++, bd++, rd++)
        if (bd == insert_rec_end)
          goto no_data;
        else if (*bd != *rd)
          break;

      /* Try to copy any data bytes of the preceding record. */
      if (c_end - cd > 2)
      {
        const byte *cdm= cd;
        const byte *rdm= rd;
        for (; cdm < c_end && *rdm == *cdm; cdm++, rdm++)
        ut_ad(rdm - rd + bd <= insert_rec_end);
        size_t len= static_cast<size_t>(rdm - rd);
        ut_ad(!memcmp(rd, cd, len));
        if (len > 2)
        {
          m_mtr.memcpy<mtr_t::FORCED>(*m_block, b, r, m_cur_rec - c);
          memcpy(bd, cd, len);
          m_mtr.memmove(*m_block, page_offset(bd), page_offset(cd), len);
          c= cdm;
          b= rdm - rd + bd;
          r= rdm;
        }
      }
    }

    if (size_t len= static_cast<size_t>(insert_rec_end - b))
      m_mtr.memcpy<mtr_t::FORCED>(*m_block, b, r, len);
  }

rec_done:
  ut_ad(fmt == COMPRESSED || !memcmp(m_heap_top, rec - extra_size, rec_size));
  rec_offs_make_valid(insert_rec, m_index, is_leaf, offsets);

  /* Update the member variables. */
  ulint slot_size= page_dir_calc_reserved_space(m_rec_no + 1) -
    page_dir_calc_reserved_space(m_rec_no);

  ut_ad(m_free_space >= rec_size + slot_size);
  ut_ad(m_heap_top + rec_size < m_page + srv_page_size);

  m_free_space-= rec_size + slot_size;
  m_heap_top+= rec_size;
  m_rec_no++;
  m_cur_rec= insert_rec;
}

/** Insert a record in the page.
@param[in]	rec		record
@param[in]	offsets		record offsets */
inline void PageBulk::insert(const rec_t *rec, rec_offs *offsets)
{
  byte rec_hdr[REC_N_OLD_EXTRA_BYTES];
  static_assert(REC_N_OLD_EXTRA_BYTES > REC_N_NEW_EXTRA_BYTES, "file format");

  if (UNIV_LIKELY_NULL(m_page_zip))
    insertPage<COMPRESSED>(const_cast<rec_t*>(rec), offsets);
  else if (m_is_comp)
  {
    memcpy(rec_hdr, rec - REC_N_NEW_EXTRA_BYTES, REC_N_NEW_EXTRA_BYTES);
    insertPage<DYNAMIC>(const_cast<rec_t*>(rec), offsets);
    memcpy(const_cast<rec_t*>(rec) - REC_N_NEW_EXTRA_BYTES, rec_hdr,
           REC_N_NEW_EXTRA_BYTES);
  }
  else
  {
    memcpy(rec_hdr, rec - REC_N_OLD_EXTRA_BYTES, REC_N_OLD_EXTRA_BYTES);
    insertPage<REDUNDANT>(const_cast<rec_t*>(rec), offsets);
    memcpy(const_cast<rec_t*>(rec) - REC_N_OLD_EXTRA_BYTES, rec_hdr,
           REC_N_OLD_EXTRA_BYTES);
  }
}

/** Set the number of owned records in the uncompressed page of
a ROW_FORMAT=COMPRESSED record without redo-logging. */
static void rec_set_n_owned_zip(rec_t *rec, ulint n_owned)
{
  rec_set_bit_field_1(rec, n_owned, REC_NEW_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
}

/** Mark end of insertion to the page. Scan all records to set page dirs,
and set page header members.
@tparam fmt  page format */
template<PageBulk::format fmt>
inline void PageBulk::finishPage()
{
  ut_ad((m_page_zip != nullptr) == (fmt == COMPRESSED));
  ut_ad((fmt != REDUNDANT) == m_is_comp);

  ulint count= 0;
  ulint n_recs= 0;
  byte *slot= my_assume_aligned<2>(m_page + srv_page_size -
                                   (PAGE_DIR + PAGE_DIR_SLOT_SIZE));
  const page_dir_slot_t *const slot0 = slot;
  compile_time_assert(PAGE_DIR_SLOT_SIZE == 2);
  if (fmt != REDUNDANT)
  {
    uint16_t offset= mach_read_from_2(PAGE_NEW_INFIMUM - REC_NEXT + m_page);
    ut_ad(offset >= PAGE_NEW_SUPREMUM - PAGE_NEW_INFIMUM);
    offset= static_cast<uint16_t>(offset + PAGE_NEW_INFIMUM);
    /* Set owner & dir. */
    while (offset != PAGE_NEW_SUPREMUM)
    {
      ut_ad(offset >= PAGE_NEW_SUPREMUM);
      ut_ad(offset < page_offset(slot));
      count++;
      n_recs++;

      if (count == (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2)
      {
        slot-= PAGE_DIR_SLOT_SIZE;
        mach_write_to_2(slot, offset);

        if (fmt != COMPRESSED)
          page_rec_set_n_owned<false>(m_block, m_page + offset, count, true,
                                      &m_mtr);
        else
          rec_set_n_owned_zip(m_page + offset, count);

        count= 0;
      }

      uint16_t next= static_cast<uint16_t>
        ((mach_read_from_2(m_page + offset - REC_NEXT) + offset) &
         (srv_page_size - 1));
      ut_ad(next);
      offset= next;
    }

    if (slot0 != slot && (count + 1 + (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2 <=
                          PAGE_DIR_SLOT_MAX_N_OWNED))
    {
      /* Merge the last two slots, like page_cur_insert_rec_low() does. */
      count+= (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2;

      rec_t *rec= const_cast<rec_t*>(page_dir_slot_get_rec(slot));
      if (fmt != COMPRESSED)
        page_rec_set_n_owned<false>(m_block, rec, 0, true, &m_mtr);
      else
        rec_set_n_owned_zip(rec, 0);
    }
    else
      slot-= PAGE_DIR_SLOT_SIZE;

    mach_write_to_2(slot, PAGE_NEW_SUPREMUM);
    if (fmt != COMPRESSED)
      page_rec_set_n_owned<false>(m_block, m_page + PAGE_NEW_SUPREMUM,
                                  count + 1, true, &m_mtr);
    else
      rec_set_n_owned_zip(m_page + PAGE_NEW_SUPREMUM, count + 1);
  }
  else
  {
    rec_t *insert_rec= m_page +
      mach_read_from_2(PAGE_OLD_INFIMUM - REC_NEXT + m_page);

    /* Set owner & dir. */
    while (insert_rec != m_page + PAGE_OLD_SUPREMUM)
    {
      count++;
      n_recs++;

      if (count == (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2)
      {
        slot-= PAGE_DIR_SLOT_SIZE;
        mach_write_to_2(slot, page_offset(insert_rec));
        page_rec_set_n_owned<false>(m_block, insert_rec, count, false, &m_mtr);
        count= 0;
      }

      insert_rec= m_page + mach_read_from_2(insert_rec - REC_NEXT);
    }

    if (slot0 != slot && (count + 1 + (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2 <=
                          PAGE_DIR_SLOT_MAX_N_OWNED))
    {
      /* Merge the last two slots, like page_cur_insert_rec_low() does. */
      count+= (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2;

      rec_t *rec= const_cast<rec_t*>(page_dir_slot_get_rec(slot));
      page_rec_set_n_owned<false>(m_block, rec, 0, false, &m_mtr);
    }
    else
      slot-= PAGE_DIR_SLOT_SIZE;

    mach_write_to_2(slot, PAGE_OLD_SUPREMUM);
    page_rec_set_n_owned<false>(m_block, m_page + PAGE_OLD_SUPREMUM, count + 1,
                                false, &m_mtr);
  }

  if (!m_rec_no);
  else if (fmt != COMPRESSED)
  {
    static_assert(PAGE_N_DIR_SLOTS == 0, "compatibility");
    alignas(8) byte page_header[PAGE_N_HEAP + 2];
    mach_write_to_2(page_header + PAGE_N_DIR_SLOTS,
                    1 + (slot0 - slot) / PAGE_DIR_SLOT_SIZE);
    mach_write_to_2(page_header + PAGE_HEAP_TOP, m_heap_top - m_page);
    mach_write_to_2(page_header + PAGE_N_HEAP,
                    (PAGE_HEAP_NO_USER_LOW + m_rec_no) |
                    uint16_t{fmt != REDUNDANT} << 15);
    m_mtr.memcpy(*m_block, PAGE_HEADER + m_page, page_header,
                 sizeof page_header);
    m_mtr.write<2>(*m_block, PAGE_HEADER + PAGE_N_RECS + m_page, m_rec_no);
    m_mtr.memcpy(*m_block, page_offset(slot), slot0 - slot);
  }
  else
  {
    /* For ROW_FORMAT=COMPRESSED, redo log may be written in
    PageBulk::compress(). */
    mach_write_to_2(PAGE_HEADER + PAGE_N_DIR_SLOTS + m_page,
                    1 + (slot0 - slot) / PAGE_DIR_SLOT_SIZE);
    mach_write_to_2(PAGE_HEADER + PAGE_HEAP_TOP + m_page,
                    static_cast<ulint>(m_heap_top - m_page));
    mach_write_to_2(PAGE_HEADER + PAGE_N_HEAP + m_page,
                    (PAGE_HEAP_NO_USER_LOW + m_rec_no) | 1U << 15);
    mach_write_to_2(PAGE_HEADER + PAGE_N_RECS + m_page, m_rec_no);
  }
}

inline bool PageBulk::needs_finish() const
{
  ut_ad(page_align(m_cur_rec) == m_block->page.frame);
  ut_ad(m_page == m_block->page.frame);
  if (!m_page[PAGE_HEADER + PAGE_DIRECTION_B])
    return true;
  ulint heap_no, n_heap= page_header_get_field(m_page, PAGE_N_HEAP);
  ut_ad((n_heap & 0x7fff) >= PAGE_HEAP_NO_USER_LOW);
  if (n_heap & 0x8000)
  {
    n_heap&= 0x7fff;
    heap_no= rec_get_heap_no_new(m_cur_rec);
    if (heap_no == PAGE_HEAP_NO_INFIMUM &&
	page_header_get_field(m_page, PAGE_HEAP_TOP) == PAGE_NEW_SUPREMUM_END)
      return false;
  }
  else
  {
    heap_no= rec_get_heap_no_old(m_cur_rec);
    if (heap_no == PAGE_HEAP_NO_INFIMUM &&
	page_header_get_field(m_page, PAGE_HEAP_TOP) == PAGE_OLD_SUPREMUM_END)
      return false;
  }
  return heap_no != n_heap - 1;
}

/** Mark end of insertion to the page. Scan all records to set page dirs,
and set page header members.
@tparam compressed  whether the page is in ROW_FORMAT=COMPRESSED */
inline void PageBulk::finish()
{
  ut_ad(!m_index->is_spatial());

  if (!needs_finish());
  else if (UNIV_LIKELY_NULL(m_page_zip))
    finishPage<COMPRESSED>();
  else if (m_is_comp)
    finishPage<DYNAMIC>();
  else
    finishPage<REDUNDANT>();

  /* In MariaDB 10.2, 10.3, 10.4, we would initialize
  PAGE_DIRECTION_B, PAGE_N_DIRECTION, PAGE_LAST_INSERT
  in the same way as we would during normal INSERT operations.
  Starting with MariaDB Server 10.5, bulk insert will not
  touch those fields. */
  ut_ad(!m_page[PAGE_HEADER + PAGE_INSTANT]);
  /* Restore the temporary change of PageBulk::init() that was necessary to
  ensure that PageBulk::needs_finish() holds on an empty page. */
  m_page[PAGE_HEADER + PAGE_DIRECTION_B]= PAGE_NO_DIRECTION;

  ut_ad(!page_header_get_field(m_page, PAGE_FREE));
  ut_ad(!page_header_get_field(m_page, PAGE_GARBAGE));
  ut_ad(!page_header_get_field(m_page, PAGE_LAST_INSERT));
  ut_ad(!page_header_get_field(m_page, PAGE_N_DIRECTION));
  ut_ad(m_total_data + page_dir_calc_reserved_space(m_rec_no) <=
        page_get_free_space_of_empty(m_is_comp));
  ut_ad(!needs_finish());
  ut_ad(page_validate(m_page, m_index));
}

/** Commit inserts done to the page
@param[in]	success		Flag whether all inserts succeed. */
void PageBulk::commit(bool success)
{
  finish();
  if (success && !dict_index_is_clust(m_index) && page_is_leaf(m_page))
    ibuf_set_bitmap_for_bulk_load(m_block, innobase_fill_factor == 100);
  m_mtr.commit();
}

/** Compress a page of compressed table
@return	true	compress successfully or no need to compress
@return	false	compress failed. */
bool
PageBulk::compress()
{
	ut_ad(m_page_zip != NULL);

	return page_zip_compress(m_block, m_index, page_zip_level, &m_mtr);
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
	rec_offs*	offsets;
	ulint		total_used_size;
	ulint		total_recs_size;
	ulint		n_recs;

	ut_ad(m_page_zip != NULL);
	ut_ad(m_rec_no >= 2);
	ut_ad(!m_index->is_instant());

	ut_ad(page_get_free_space_of_empty(m_is_comp) > m_free_space);
	total_used_size = page_get_free_space_of_empty(m_is_comp)
		- m_free_space;

	total_recs_size = 0;
	n_recs = 0;
	offsets = NULL;
	rec = page_get_infimum_rec(m_page);
	const ulint n_core = page_is_leaf(m_page) ? m_index->n_core_fields : 0;

	do {
		rec = page_rec_get_next(rec);
		ut_ad(page_rec_is_user_rec(rec));

		offsets = rec_get_offsets(rec, m_index, offsets, n_core,
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
	rec_offs*	offsets = NULL;

	ut_ad(m_rec_no == 0);
	ut_ad(page_rec_is_user_rec(rec));

	const ulint n_core = page_rec_is_leaf(rec)
		? m_index->n_core_fields : 0;

	do {
		offsets = rec_get_offsets(rec, m_index, offsets, n_core,
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
	rec_offs*	offsets = NULL;
	rec = page_rec_get_prev(split_rec);
	const ulint n_core = page_rec_is_leaf(split_rec)
		? m_index->n_core_fields : 0;

	offsets = rec_get_offsets(rec, m_index, offsets, n_core,
				  ULINT_UNDEFINED, &m_heap);
	mach_write_to_2(rec - REC_NEXT, m_is_comp
			? static_cast<uint16_t>
			(PAGE_NEW_SUPREMUM - page_offset(rec))
			: PAGE_OLD_SUPREMUM);

	/* Set related members */
	m_cur_rec = rec;
	m_heap_top = rec_get_end(rec, offsets);

	offsets = rec_get_offsets(last_rec, m_index, offsets, n_core,
				  ULINT_UNDEFINED, &m_heap);

	m_free_space += ulint(rec_get_end(last_rec, offsets) - m_heap_top)
		+ page_dir_calc_reserved_space(m_rec_no)
		- page_dir_calc_reserved_space(n);
	ut_ad(lint(m_free_space) > 0);
	m_rec_no = n;

#ifdef UNIV_DEBUG
	m_total_data -= ulint(rec_get_end(last_rec, offsets) - m_heap_top);
#endif /* UNIV_DEBUG */
}

/** Set next page
@param[in]	next_page_no	next page no */
inline void PageBulk::setNext(ulint next_page_no)
{
  if (UNIV_LIKELY_NULL(m_page_zip))
    /* For ROW_FORMAT=COMPRESSED, redo log may be written
    in PageBulk::compress(). */
    mach_write_to_4(m_page + FIL_PAGE_NEXT, next_page_no);
  else
    m_mtr.write<4>(*m_block, m_page + FIL_PAGE_NEXT, next_page_no);
}

/** Set previous page
@param[in]	prev_page_no	previous page no */
inline void PageBulk::setPrev(ulint prev_page_no)
{
  if (UNIV_LIKELY_NULL(m_page_zip))
    /* For ROW_FORMAT=COMPRESSED, redo log may be written
    in PageBulk::compress(). */
    mach_write_to_4(m_page + FIL_PAGE_PREV, prev_page_no);
  else
    m_mtr.write<4>(*m_block, m_page + FIL_PAGE_PREV, prev_page_no);
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
	return page_zip_rec_needs_ext(rec_size, m_is_comp,
				      dtuple_get_n_fields(tuple),
				      m_block->zip_size());
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
	rec_offs*		offsets)
{
	finish();

	/* Note: not all fields are initialized in btr_pcur. */
	btr_pcur_t	btr_pcur;
	btr_pcur.pos_state = BTR_PCUR_IS_POSITIONED;
	btr_pcur.latch_mode = BTR_MODIFY_LEAF;
	btr_pcur.btr_cur.index = m_index;
	btr_pcur.btr_cur.page_cur.index = m_index;
	btr_pcur.btr_cur.page_cur.rec = m_cur_rec;
	btr_pcur.btr_cur.page_cur.offsets = offsets;
	btr_pcur.btr_cur.page_cur.block = m_block;

	dberr_t	err = btr_store_big_rec_extern_fields(
		&btr_pcur, offsets, big_rec, &m_mtr, BTR_STORE_INSERT_BULK);

	/* Reset m_block and m_cur_rec from page cursor, because
	block may be changed during blob insert. (FIXME: Can it really?) */
	ut_ad(m_block == btr_pcur.btr_cur.page_cur.block);

	m_block = btr_pcur.btr_cur.page_cur.block;
	m_cur_rec = btr_pcur.btr_cur.page_cur.rec;
	m_page = buf_block_get_frame(m_block);

	return(err);
}

/** Release block by commiting mtr
Note: log_free_check requires holding no lock/latch in current thread. */
void
PageBulk::release()
{
	finish();

	/* We fix the block because we will re-pin it soon. */
	m_block->page.fix();

	/* No other threads can modify this block. */
	m_modify_clock = buf_block_get_modify_clock(m_block);

	m_mtr.commit();
}

/** Start mtr and latch the block */
dberr_t
PageBulk::latch()
{
	m_mtr.start();
	m_index->set_modified(m_mtr);

	ut_ad(m_block->page.buf_fix_count());

	/* In case the block is U-latched by page_cleaner. */
	if (!buf_page_optimistic_get(RW_X_LATCH, m_block, m_modify_clock,
				     &m_mtr)) {
		/* FIXME: avoid another lookup */
		m_block = buf_page_get_gen(page_id_t(m_index->table->space_id,
						     m_page_no),
					   0, RW_X_LATCH,
					   m_block, BUF_GET_IF_IN_POOL,
					   &m_mtr, &m_err);

		if (m_err != DB_SUCCESS) {
			return (m_err);
		}

		ut_ad(m_block != NULL);
	}

	ut_d(const auto buf_fix_count =) m_block->page.unfix();

	ut_ad(buf_fix_count);
	ut_ad(m_cur_rec > m_page);
	ut_ad(m_cur_rec < m_heap_top);

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

	if (page_bulk->getRecNo() <= 1) {
		return(DB_TOO_BIG_RECORD);
	}

	/* Initialize a new page */
	PageBulk new_page_bulk(m_index, m_trx->id, FIL_NULL,
			       page_bulk->getLevel());
	dberr_t	err = new_page_bulk.init();
	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Copy the upper half to the new page. */
	rec_t*	split_rec = page_bulk->getSplitRec();
	new_page_bulk.copyIn(split_rec);
	page_bulk->copyOut(split_rec);

	/* Commit the pages after split. */
	err = pageCommit(page_bulk, &new_page_bulk, true);
	if (err != DB_SUCCESS) {
		pageAbort(&new_page_bulk);
		return(err);
	}

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
		ut_ad(!page_has_next(page_bulk->getPage()));
		/* If a page is released and latched again, we need to
		mark it modified in mini-transaction.  */
		page_bulk->set_modified();
	}

	ut_ad(!m_index->lock.have_any());

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
	if (log_sys.check_flush_or_checkpoint()) {
		release();

		log_check_margins();

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
						level));
		err = new_page_bulk->init();
		if (err != DB_SUCCESS) {
			UT_DELETE(new_page_bulk);
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
	rec_offs*	offsets = NULL;

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
							  FIL_NULL, level));
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
				err = DB_INTERRUPTED;
				goto func_exit;
			}

			srv_inc_activity_count();
			logFreeCheck();
		}
	}

	/* Convert tuple to rec. */
        rec = rec_convert_dtuple_to_rec(static_cast<byte*>(mem_heap_alloc(
		page_bulk->m_heap, rec_size)), m_index, tuple, n_ext);
        offsets = rec_get_offsets(rec, m_index, offsets, level
				  ? 0 : m_index->n_core_fields,
				  ULINT_UNDEFINED, &page_bulk->m_heap);

	page_bulk->insert(rec, offsets);

	if (big_rec != NULL) {
		ut_ad(dict_index_is_clust(m_index));
		ut_ad(page_bulk->getLevel() == 0);
		ut_ad(page_bulk == m_page_bulks.at(0));

		/* Release all pages above the leaf level */
		for (ulint level = 1; level <= m_root_level; level++) {
			m_page_bulks.at(level)->release();
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
	uint32_t last_page_no = FIL_NULL;

	ut_ad(!m_index->table->is_temporary());

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
		PageBulk	root_page_bulk(m_index, m_trx->id,
					       m_index->page, m_root_level);

		mtr.start();
		m_index->set_modified(mtr);
		mtr_x_lock_index(m_index, &mtr);

		ut_ad(last_page_no != FIL_NULL);
		last_block = btr_block_get(*m_index, last_page_no, RW_X_LATCH,
					   false, &mtr);
		first_rec = page_rec_get_next(
			page_get_infimum_rec(last_block->page.frame));
		ut_ad(page_rec_is_user_rec(first_rec));

		/* Copy last page to root page. */
		err = root_page_bulk.init();
		if (err != DB_SUCCESS) {
			mtr.commit();
			return(err);
		}
		root_page_bulk.copyIn(first_rec);
		root_page_bulk.finish();

		/* Remove last page. */
		btr_page_free(m_index, last_block, &mtr);

		mtr.commit();

		err = pageCommit(&root_page_bulk, NULL, false);
		ut_ad(err == DB_SUCCESS);
	}

	ut_ad(err != DB_SUCCESS
	      || btr_validate_index(m_index, NULL) == DB_SUCCESS);
	return(err);
}
