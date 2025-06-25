/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2023, MariaDB Corporation.

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

/**
@file ibuf/ibuf0ibuf.cc
Upgrade and removal of the InnoDB change buffer
*/

#include "ibuf0ibuf.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "row0upd.h"
#include "my_service_manager.h"
#include "log.h"

/** Possible operations buffered in the change buffer. */
enum ibuf_op
{
  IBUF_OP_INSERT= 0,
  IBUF_OP_DELETE_MARK= 1,
  IBUF_OP_DELETE= 2,
};

constexpr const page_id_t ibuf_root{0, FSP_IBUF_TREE_ROOT_PAGE_NO};
constexpr const page_id_t ibuf_header{0, FSP_IBUF_HEADER_PAGE_NO};
constexpr const index_id_t ibuf_index_id{0xFFFFFFFF00000000ULL};

/* Format of the change buffer records:

MySQL 3.23 and MySQL 4.0 (not supported since MySQL 5.6.5 and MariaDB 10.0.11):

1. The first field is the page number.
2. The second field is an array which stores type info for each subsequent
   field (4 bytes per column).
   We store the information which affects the ordering of records, and
   also the physical storage size of an SQL NULL value. E.g., for CHAR(10) it
   is 10 bytes.
3. Next we have the fields of the actual index record.

MySQL 4.1:

1. The first field is the space id.
2. The second field is a one-byte marker (0) which differentiates records from
   the < 4.1.x storage format.
3. The third field is the page number.
4. The fourth field contains the type info
   (6 bytes per index field, 16-bit collation information added).
   Unless ROW_FORMAT=REDUNDANT, we add more metadata here so that
   we can access records in the index page.
5. The rest of the fields contain the fields of the actual index record.

MySQL 5.0 (starting with MySQL 5.0.3) and MySQL 5.1:

The first byte of the fourth field is an additional marker (0) if the record
is not in ROW_FORMAT=REDUNDANT. The presence of this marker can be detected by
looking at the length of the field modulo 6.

The high-order bit of the character set field in the type info is the
"nullable" flag for the field.

MySQL 5.5 and MariaDB 5.5 and later:

Unless innodb_change_buffering=inserts, the optional marker byte at
the start of the fourth field may be replaced by mandatory 3 fields,
comprising 4 bytes:

 1. 2 bytes: Counter field, used to sort records within a (space id, page
    no) in the order they were added. This is needed so that for example the
    sequence of operations "INSERT x, DEL MARK x, INSERT x" is handled
    correctly.

 2. 1 byte: Operation type (see ibuf_op).

 3. 1 byte: 0=ROW_FORMAT=REDUNDANT, 1=other
*/

/** first user record field */
constexpr unsigned IBUF_REC_FIELD_USER= 4;

/********************************************************************//**
Returns the page number field of an ibuf record.
@return page number */
static uint32_t ibuf_rec_get_page_no(const rec_t *rec)
{
  return mach_read_from_4(rec + 5);
}

/********************************************************************//**
Returns the space id field of an ibuf record.
@return space id */
static uint32_t ibuf_rec_get_space(const rec_t *rec)
{
  return mach_read_from_4(rec);
}

/********************************************************************//**
Add a column to the dummy index */
static
void
ibuf_dummy_index_add_col(
/*=====================*/
	dict_index_t*	index,	/*!< in: dummy index */
	const dtype_t*	type,	/*!< in: the data type of the column */
	ulint		len)	/*!< in: length of the column */
{
	ulint	i	= index->table->n_def;
	dict_mem_table_add_col(index->table, NULL, NULL,
			       dtype_get_mtype(type),
			       dtype_get_prtype(type),
			       dtype_get_len(type));
	dict_index_add_col(index, index->table,
			   dict_table_get_nth_col(index->table, i), len);
}

/**********************************************************************//**
Reads to a type the stored information which determines its alphabetical
ordering and the storage size of an SQL NULL value. This is the >= 4.1.x
storage format. */
static
void
dtype_new_read_for_order_and_null_size(
/*===================================*/
	dtype_t*	type,	/*!< in: type struct */
	const byte*	buf)	/*!< in: buffer for stored type order info */
{
	type->mtype = buf[0] & 63;
	type->prtype = buf[1];

	if (buf[0] & 128) {
		type->prtype |= DATA_BINARY_TYPE;
	}

	if (buf[4] & 128) {
		type->prtype |= DATA_NOT_NULL;
	}

	type->len = mach_read_from_2(buf + 2);

	uint32_t charset_coll = (mach_read_from_2(buf + 4) & CHAR_COLL_MASK)
		<< 16;

	if (dtype_is_string_type(type->mtype)) {
		type->prtype |= charset_coll;

		if (charset_coll == 0) {
			/* This insert buffer record was inserted before
			MySQL 4.1.2, and the charset-collation code was not
			explicitly stored to dtype->prtype at that time. It
			must be the default charset-collation of this MySQL
			installation. */
	                type->prtype |= default_charset_info->number << 16;
		}
	}

	dtype_set_mblen(type);
}

/** Construct an index entry and an index for applying an operation.
@param ibuf_rec       change buffer record in an X-latched page
@param not_redundant  whether another format than ROW_FORMAT=REDUNDANT is used
@param n_fields       number of index record fields
@param types          type information
@param heap           memory heap
@param index          dummy index metadata
@return the index entry for applying the operation */
static dtuple_t *ibuf_entry_build(const rec_t *ibuf_rec, ulint not_redundant,
                                  ulint n_fields, const byte *types,
                                  mem_heap_t *heap, dict_index_t *&index)
{
	dtuple_t*	tuple;
	dfield_t*	field;
	const byte*	data;
	ulint		len;

	tuple = dtuple_create(heap, uint16_t(n_fields));

	index = dict_mem_index_create(
		dict_table_t::create({C_STRING_WITH_LEN("")}, nullptr,
                                     n_fields, 0,
				     not_redundant ? DICT_TF_COMPACT : 0, 0),
                "IBUF_DUMMY", 0, n_fields);
	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	ut_d(index->cached = true);
	ut_d(index->is_dummy = true);

	for (ulint i = 0; i < n_fields; i++) {
		field = dtuple_get_nth_field(tuple, i);

		data = rec_get_nth_field_old(
			ibuf_rec, i + IBUF_REC_FIELD_USER, &len);

		dfield_set_data(field, data, len);

		dtype_new_read_for_order_and_null_size(
			dfield_get_type(field), types + i * 6);

		ibuf_dummy_index_add_col(index, dfield_get_type(field), len);
	}

	index->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(index->n_nullable)));

	/* Prevent an ut_ad() failure in page_zip_write_rec() by
	adding system columns to the dummy table pointed to by the
	dummy secondary index. The change buffer was only used for
	secondary indexes, whose records never contain any system
	columns, such as DB_TRX_ID. */
	ut_d(dict_table_add_system_columns(index->table, index->table->heap));
	return(tuple);
}

/** Removes a page from the free list and frees it to the fsp system.
@param mtr    mini-transaction
@return error code
@retval DB_SUCCESS            if more work may remain to be done
@retval DB_SUCCESS_LOCKED_REC if everything was freed */
ATTRIBUTE_COLD static dberr_t ibuf_remove_free_page(mtr_t &mtr)
{
  log_free_check();

  mtr.start();

  mtr.x_lock_space(fil_system.sys_space);
  dberr_t err;
  buf_block_t* header= buf_page_get_gen(ibuf_header, 0, RW_X_LATCH, nullptr,
                                        BUF_GET, &mtr, &err);

  if (!header)
  {
func_exit:
    mtr.commit();
    return err;
  }

  buf_block_t *root= buf_page_get_gen(ibuf_root, 0, RW_X_LATCH,
                                      nullptr, BUF_GET, &mtr, &err);

  if (UNIV_UNLIKELY(!root))
    goto func_exit;

  const uint32_t page_no= flst_get_last(PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST +
                                        root->page.frame).page;
  if (page_no == FIL_NULL)
  {
    mtr.set_modified(*root);
    fsp_init_file_page(fil_system.sys_space, root, &mtr);
    err= DB_SUCCESS_LOCKED_REC;
    goto func_exit;
  }

  if (page_no >= fil_system.sys_space->free_limit)
    goto corrupted;

  /* Since pessimistic inserts were prevented, we know that the
  page is still in the free list. NOTE that also deletes may take
  pages from the free list, but they take them from the start, and
  the free list was so long that they cannot have taken the last
  page from it. */

  err= fseg_free_page(header->page.frame + PAGE_DATA, fil_system.sys_space,
                      page_no, &mtr);

  if (err != DB_SUCCESS)
    goto func_exit;

  if (page_no != flst_get_last(PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST +
                               root->page.frame).page)
  {
  corrupted:
    err= DB_CORRUPTION;
    goto func_exit;
  }

  /* Remove the page from the free list and update the ibuf size data */
  if (buf_block_t *block=
      buf_page_get_gen(page_id_t{0, page_no}, 0, RW_X_LATCH, nullptr, BUF_GET,
                       &mtr, &err))
    err= flst_remove(root, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
                     block, PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE,
                     fil_system.sys_space->free_limit, &mtr);

  if (err == DB_SUCCESS)
    buf_page_free(fil_system.sys_space, page_no, &mtr);

  goto func_exit;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/********************************************************************//**
During merge, inserts to an index page a secondary index entry extracted
from the insert buffer.
@return	error code */
static
dberr_t
ibuf_insert_to_index_page_low(
/*==========================*/
	const dtuple_t*	entry,	/*!< in: buffered entry to insert */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	mtr_t*		mtr,	/*!< in/out: mtr */
	page_cur_t*	page_cur)/*!< in/out: cursor positioned on the record
				after which to insert the buffered entry */
{
  if (page_cur_tuple_insert(page_cur, entry, offsets, &heap, 0, mtr))
    return DB_SUCCESS;

  /* Page reorganization or recompression should already have been
  attempted by page_cur_tuple_insert(). */
  ut_ad(!is_buf_block_get_page_zip(page_cur->block));

  /* If the record did not fit, reorganize */
  if (dberr_t err= btr_page_reorganize(page_cur, mtr))
    return err;

  /* This time the record must fit */
  if (page_cur_tuple_insert(page_cur, entry, offsets, &heap, 0, mtr))
    return DB_SUCCESS;

  return DB_CORRUPTION;
}

/************************************************************************
During merge, inserts to an index page a secondary index entry extracted
from the insert buffer. */
static
dberr_t
ibuf_insert_to_index_page(
/*======================*/
	const dtuple_t*	entry,	/*!< in: buffered entry to insert */
	buf_block_t*	block,	/*!< in/out: index page where the buffered entry
				should be placed */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_cur_t	page_cur;
	page_t*		page		= buf_block_get_frame(block);
	rec_t*		rec;
	rec_offs*	offsets;
	mem_heap_t*	heap;

	DBUG_PRINT("ibuf", ("page " UINT32PF ":" UINT32PF,
			    block->page.id().space(),
			    block->page.id().page_no()));

	ut_ad(!dict_index_is_online_ddl(index));// this is an ibuf_dummy index
	ut_ad(dtuple_check_typed(entry));
#ifdef BTR_CUR_HASH_ADAPT
	/* ibuf_cleanup() must finish before the adaptive hash index
	can be inserted into. */
	ut_ad(!block->index);
#endif /* BTR_CUR_HASH_ADAPT */
	ut_ad(mtr->is_named_space(block->page.id().space()));
        const auto comp = page_is_comp(page);

	if (UNIV_UNLIKELY(index->table->not_redundant()
			  != !!page_is_comp(page))) {
		return DB_CORRUPTION;
	}

	if (comp) {
		rec = const_cast<rec_t*>(
			page_rec_next_get<true>(page,
						page + PAGE_NEW_INFIMUM));
		if (!rec || rec == page + PAGE_NEW_SUPREMUM) {
			return DB_CORRUPTION;
		}
	} else {
		rec = const_cast<rec_t*>(
			page_rec_next_get<false>(page,
						page + PAGE_OLD_INFIMUM));
		if (!rec || rec == page + PAGE_OLD_SUPREMUM) {
			return DB_CORRUPTION;
		}
	}

	if (!rec_n_fields_is_sane(index, rec, entry)) {
		return DB_CORRUPTION;
	}

	uint16_t up_match = 0, low_match = 0;
	page_cur.index = index;
	page_cur.block = block;

	if (page_cur_search_with_match(entry, PAGE_CUR_LE,
				       &up_match, &low_match, &page_cur,
				       nullptr)) {
		return DB_CORRUPTION;
	}

	dberr_t err = DB_SUCCESS;

	heap = mem_heap_create(
		sizeof(upd_t)
		+ REC_OFFS_HEADER_SIZE * sizeof(*offsets)
		+ dtuple_get_n_fields(entry)
		* (sizeof(upd_field_t) + sizeof *offsets));

	if (UNIV_UNLIKELY(low_match == dtuple_get_n_fields(entry))) {
		upd_t*		update;

		rec = page_cur_get_rec(&page_cur);

		/* This is based on
		row_ins_sec_index_entry_by_modify(BTR_MODIFY_LEAF). */
		ut_ad(rec_get_deleted_flag(rec, page_is_comp(page)));

		offsets = rec_get_offsets(rec, index, NULL, index->n_fields,
					  ULINT_UNDEFINED, &heap);
		update = row_upd_build_sec_rec_difference_binary(
			rec, index, offsets, entry, heap);

		if (update->n_fields == 0) {
			/* The records only differ in the delete-mark.
			Clear the delete-mark, like we did before
			Bug #56680 was fixed. */
			btr_rec_set_deleted<false>(block, rec, mtr);
			goto updated_in_place;
		}

		/* Copy the info bits. Clear the delete-mark. */
		update->info_bits = rec_get_info_bits(rec, page_is_comp(page));
		update->info_bits &= byte(~REC_INFO_DELETED_FLAG);
		page_zip_des_t* page_zip = buf_block_get_page_zip(block);

		/* We cannot invoke btr_cur_optimistic_update() here,
		because we do not have a btr_cur_t or que_thr_t,
		as the insert buffer merge occurs at a very low level. */
		if (!row_upd_changes_field_size_or_external(index, offsets,
							    update)
		    && (!page_zip || btr_cur_update_alloc_zip(
				page_zip, &page_cur, offsets,
				rec_offs_size(offsets), false, mtr))) {
			/* This is the easy case. Do something similar
			to btr_cur_update_in_place(). */
			rec = page_cur_get_rec(&page_cur);
			btr_cur_upd_rec_in_place(rec, index, offsets,
						 update, block, mtr);

			DBUG_EXECUTE_IF(
				"crash_after_log_ibuf_upd_inplace",
				log_buffer_flush_to_disk();
				ib::info() << "Wrote log record for ibuf"
					" update in place operation";
				DBUG_SUICIDE();
			);

			goto updated_in_place;
		}

		/* btr_cur_update_alloc_zip() may have changed this */
		rec = page_cur_get_rec(&page_cur);

		/* A collation may identify values that differ in
		storage length.
		Some examples (1 or 2 bytes):
		utf8_turkish_ci: I = U+0131 LATIN SMALL LETTER DOTLESS I
		utf8_general_ci: S = U+00DF LATIN SMALL LETTER SHARP S
		utf8_general_ci: A = U+00E4 LATIN SMALL LETTER A WITH DIAERESIS

		latin1_german2_ci: SS = U+00DF LATIN SMALL LETTER SHARP S

		Examples of a character (3-byte UTF-8 sequence)
		identified with 2 or 4 characters (1-byte UTF-8 sequences):

		utf8_unicode_ci: 'II' = U+2171 SMALL ROMAN NUMERAL TWO
		utf8_unicode_ci: '(10)' = U+247D PARENTHESIZED NUMBER TEN
		*/

		/* Delete the different-length record, and insert the
		buffered one. */

		page_cur_delete_rec(&page_cur, offsets, mtr);
		if (!(page_cur_move_to_prev(&page_cur))) {
			err = DB_CORRUPTION;
			goto updated_in_place;
		}
	} else {
		offsets = NULL;
	}

	err = ibuf_insert_to_index_page_low(entry, &offsets, heap, mtr,
                                            &page_cur);
updated_in_place:
	mem_heap_free(heap);

	return err;
}

/****************************************************************//**
During merge, sets the delete mark on a record for a secondary index
entry. */
static
void
ibuf_set_del_mark(
/*==============*/
	const dtuple_t*		entry,	/*!< in: entry */
	buf_block_t*		block,	/*!< in/out: block */
	dict_index_t*		index,	/*!< in: record descriptor */
	mtr_t*			mtr)	/*!< in: mtr */
{
	page_cur_t	page_cur;
	page_cur.block = block;
	page_cur.index = index;
	uint16_t up_match = 0, low_match = 0;

	ut_ad(dtuple_check_typed(entry));

	if (!page_cur_search_with_match(entry, PAGE_CUR_LE,
					&up_match, &low_match, &page_cur,
					nullptr)
	    && low_match == dtuple_get_n_fields(entry)) {
		rec_t* rec = page_cur_get_rec(&page_cur);

		/* Delete mark the old index record. According to a
		comment in row_upd_sec_index_entry(), it can already
		have been delete marked if a lock wait occurred in
		row_ins_sec_index_entry() in a previous invocation of
		row_upd_sec_index_entry(). */

		if (UNIV_LIKELY
		    (!rec_get_deleted_flag(
			    rec, dict_table_is_comp(index->table)))) {
			btr_rec_set_deleted<true>(block, rec, mtr);
		}
	} else {
		const page_t*		page
			= page_cur_get_page(&page_cur);
		const buf_block_t*	block
			= page_cur_get_block(&page_cur);

		ib::error() << "Unable to find a record to delete-mark";
		fputs("InnoDB: tuple ", stderr);
		dtuple_print(stderr, entry);
		fputs("\n"
		      "InnoDB: record ", stderr);
		rec_print(stderr, page_cur_get_rec(&page_cur), index);

		ib::error() << "page " << block->page.id() << " ("
			<< page_get_n_recs(page) << " records, index id "
			<< btr_page_get_index_id(page) << ").";

		ib::error() << BUG_REPORT_MSG;
		ut_ad(0);
	}
}

/****************************************************************//**
During merge, delete a record for a secondary index entry. */
static
void
ibuf_delete(
/*========*/
	const dtuple_t*	entry,	/*!< in: entry */
	buf_block_t*	block,	/*!< in/out: block */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in/out: mtr; must be committed
				before latching any further pages */
{
	page_cur_t	page_cur;
	page_cur.block = block;
	page_cur.index = index;
	uint16_t	up_match = 0, low_match = 0;

	ut_ad(dtuple_check_typed(entry));
	ut_ad(!index->is_spatial());
	ut_ad(!index->is_clust());

	if (!page_cur_search_with_match(entry, PAGE_CUR_LE,
					&up_match, &low_match, &page_cur,
					nullptr)
	    && low_match == dtuple_get_n_fields(entry)) {
		page_t*		page	= buf_block_get_frame(block);
		rec_t*		rec	= page_cur_get_rec(&page_cur);

		/* TODO: the below should probably be a separate function,
		it's a bastardized version of btr_cur_optimistic_delete. */

		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		rec_offs*	offsets	= offsets_;
		mem_heap_t*	heap = NULL;

		rec_offs_init(offsets_);

		offsets = rec_get_offsets(rec, index, offsets, index->n_fields,
					  ULINT_UNDEFINED, &heap);

		if (page_get_n_recs(page) <= 1
		    || !(REC_INFO_DELETED_FLAG
			 & rec_get_info_bits(rec, page_is_comp(page)))) {
			/* Refuse to purge the last record or a
			record that has not been marked for deletion. */
			ib::error() << "Unable to purge a record";
			fputs("InnoDB: tuple ", stderr);
			dtuple_print(stderr, entry);
			fputs("\n"
			      "InnoDB: record ", stderr);
			rec_print_new(stderr, rec, offsets);
			fprintf(stderr, "\nspace " UINT32PF " offset " UINT32PF
				" (%u records, index id %llu)\n"
				"InnoDB: Submit a detailed bug report"
				" to https://jira.mariadb.org/\n",
				block->page.id().space(),
				block->page.id().page_no(),
				(unsigned) page_get_n_recs(page),
				(ulonglong) btr_page_get_index_id(page));

			ut_ad(0);
			return;
		}

#ifdef UNIV_ZIP_DEBUG
		page_zip_des_t*	page_zip= buf_block_get_page_zip(block);
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
		page_cur_delete_rec(&page_cur, offsets, mtr);
#ifdef UNIV_ZIP_DEBUG
		ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	}
}

/** Reset the bits in the bitmap page for the given page number.
@param bitmap   change buffer bitmap page
@param offset   page number
@param mtr      mini-transaction */
static void ibuf_reset(buf_block_t &bitmap, uint32_t offset, mtr_t *mtr)
{
  offset&= uint32_t(bitmap.physical_size() - 1);
  byte *map_byte= &bitmap.page.frame[PAGE_DATA + offset / 2];
  /* We must reset IBUF_BITMAP_BUFFERED, but at the same time we will also
  reset IBUF_BITMAP_FREE (and IBUF_BITMAP_IBUF, which should be clear). */
  byte b= byte(*map_byte & ((offset & 1) ? byte{0xf} : byte{0xf0}));
  mtr->write<1,mtr_t::MAYBE_NOP>(bitmap, map_byte, b);
}

/** Move to the next change buffer record. */
ATTRIBUTE_COLD static dberr_t ibuf_move_to_next(btr_cur_t *cur, mtr_t *mtr)
{
  if (!page_cur_move_to_next(&cur->page_cur))
    return DB_CORRUPTION;
  if (!page_cur_is_after_last(&cur->page_cur))
    return DB_SUCCESS;

  /* The following is adapted from btr_pcur_move_to_next_page(),
  but we will not release any latches. */

  const buf_block_t &block= *cur->page_cur.block;
  const uint32_t next_page_no= btr_page_get_next(block.page.frame);
  switch (next_page_no) {
  case 0:
  case 1:
    return DB_CORRUPTION;
  case FIL_NULL:
    return DB_SUCCESS;
  }

  if (UNIV_UNLIKELY(next_page_no == block.page.id().page_no()))
    return DB_CORRUPTION;

  dberr_t err;
  buf_block_t *next=
    btr_block_get(*cur->index(), next_page_no, RW_X_LATCH, mtr, &err);
  if (!next)
    return err;

  if (UNIV_UNLIKELY(memcmp_aligned<4>(next->page.frame + FIL_PAGE_PREV,
                                      block.page.frame + FIL_PAGE_OFFSET, 4)))
    return DB_CORRUPTION;

  page_cur_set_before_first(next, &cur->page_cur);
  return page_cur_move_to_next(&cur->page_cur) ? DB_SUCCESS : DB_CORRUPTION;
}

/** @return if buffered changes exist for the page */
ATTRIBUTE_COLD
static bool ibuf_bitmap_buffered(const buf_block_t *bitmap, uint32_t offset)
{
  if (!bitmap)
    return false;
  offset&= uint32_t(bitmap->physical_size() - 1);
  byte *map_byte= &bitmap->page.frame[PAGE_DATA + offset / 2];
  return *map_byte & (byte{4} << ((offset & 1) << 4));
}

/** Apply changes to a block. */
ATTRIBUTE_COLD
static dberr_t ibuf_merge(fil_space_t *space, btr_cur_t *cur, mtr_t *mtr)
{
  if (btr_cur_get_rec(cur)[4])
    return DB_CORRUPTION;

  const uint32_t space_id= mach_read_from_4(btr_cur_get_rec(cur));
  const uint32_t page_no= mach_read_from_4(btr_cur_get_rec(cur) + 5);

  buf_block_t *block= space && page_no < space->size
    ? buf_page_get_gen(page_id_t{space_id, page_no}, space->zip_size(),
                       RW_X_LATCH, nullptr, BUF_GET_POSSIBLY_FREED, mtr)
    : nullptr;

  buf_block_t *bitmap= block
    ? buf_page_get_gen(page_id_t(space_id,
                                 uint32_t(page_no &
                                          ~(block->physical_size() - 1)) + 1),
                       block->zip_size(), RW_X_LATCH, nullptr,
                       BUF_GET_POSSIBLY_FREED, mtr)
    : nullptr;
  bool buffered= false;

  if (!block);
  else if (fil_page_get_type(block->page.frame) != FIL_PAGE_INDEX ||
           !page_is_leaf(block->page.frame) ||
           DB_SUCCESS == fseg_page_is_allocated(mtr, space, page_no))
    block= nullptr;
  else
    buffered= ibuf_bitmap_buffered(bitmap, block->page.id().page_no());

  do
  {
    rec_t *rec= cur->page_cur.rec;
    ulint n_fields= rec_get_n_fields_old(rec);

    if (n_fields < IBUF_REC_FIELD_USER + 1 || rec[4])
      return DB_CORRUPTION;

    n_fields-= IBUF_REC_FIELD_USER;

    ulint types_len, not_redundant;

    if (rec_get_1byte_offs_flag(rec))
    {
      if (rec_1_get_field_end_info(rec, 0) != 4 ||
          rec_1_get_field_end_info(rec, 1) != 5 ||
          rec_1_get_field_end_info(rec, 2) != 9)
        return DB_CORRUPTION;
      types_len= rec_1_get_field_end_info(rec, 3);
    }
    else
    {
      if (rec_2_get_field_end_info(rec, 0) != 4 ||
          rec_2_get_field_end_info(rec, 1) != 5 ||
          rec_2_get_field_end_info(rec, 2) != 9)
        return DB_CORRUPTION;
      types_len= rec_2_get_field_end_info(rec, 3);
    }

    if (types_len < 9 || (types_len - 9) / 6 != n_fields)
      return DB_CORRUPTION;

    ibuf_op op= IBUF_OP_INSERT;
    const ulint info_len= (types_len - 9) % 6;

    switch (info_len) {
    default:
      return DB_CORRUPTION;
    case 0: case 1:
      not_redundant= info_len;
      break;
    case 4:
      not_redundant= rec[9 + 3];
      if (rec[9 + 2] > IBUF_OP_DELETE || not_redundant > 1)
        return DB_CORRUPTION;
      op= static_cast<ibuf_op>(rec[9 + 2]);
    }

    const byte *const types= rec + 9 + info_len;

    if (ibuf_rec_get_space(rec) != space_id ||
        ibuf_rec_get_page_no(rec) != page_no)
      break;

    if (!rec_get_deleted_flag(rec, 0))
    {
      /* Delete-mark the record so that it will not be applied again if
      the server is killed before the completion of ibuf_upgrade(). */
      btr_rec_set_deleted<true>(cur->page_cur.block, rec, mtr);

      if (buffered)
      {
        page_header_reset_last_insert(block, mtr);
        page_update_max_trx_id(block, buf_block_get_page_zip(block),
                               page_get_max_trx_id(btr_cur_get_page(cur)),
                               mtr);
        dict_index_t *index;
        mem_heap_t *heap = mem_heap_create(512);
        dtuple_t *entry= ibuf_entry_build(rec, not_redundant, n_fields,
                                          types, heap, index);
        dict_table_t *table= index->table;
        ut_ad(!table->space);
        table->space= space;
        table->space_id= space_id;

        switch (op) {
        case IBUF_OP_INSERT:
          ibuf_insert_to_index_page(entry, block, index, mtr);
          break;
        case IBUF_OP_DELETE_MARK:
          ibuf_set_del_mark(entry, block, index, mtr);
          break;
        case IBUF_OP_DELETE:
          ibuf_delete(entry, block, index, mtr);
          break;
        }

        mem_heap_free(heap);
        dict_mem_index_free(index);
        dict_mem_table_free(table);
      }
    }

    if (dberr_t err= ibuf_move_to_next(cur, mtr))
      return err;
  }
  while (!page_cur_is_after_last(&cur->page_cur));

  if (bitmap)
    ibuf_reset(*bitmap, page_no, mtr);

  return DB_SUCCESS;
}

static dberr_t ibuf_open(btr_cur_t *cur, mtr_t *mtr)
{
  ut_ad(mtr->get_savepoint() == 1);

  uint32_t page= FSP_IBUF_TREE_ROOT_PAGE_NO;

  for (ulint height= ULINT_UNDEFINED;;)
  {
    dberr_t err;
    buf_block_t* block= btr_block_get(*cur->index(), page, RW_X_LATCH, mtr,
                                      &err);
    ut_ad(!block == (err != DB_SUCCESS));

    if (!block)
      return err;

    page_cur_set_before_first(block, &cur->page_cur);
    const uint32_t l= btr_page_get_level(block->page.frame);

    if (height == ULINT_UNDEFINED)
      height= l;
    else
    {
      /* Release the parent page latch. */
      ut_ad(mtr->get_savepoint() == 3);
      mtr->rollback_to_savepoint(1, 2);

      if (UNIV_UNLIKELY(height != l))
        return DB_CORRUPTION;
    }

    if (!height)
      return ibuf_move_to_next(cur, mtr);

    height--;

    if (!page_cur_move_to_next(&cur->page_cur))
      return DB_CORRUPTION;

    const rec_t *ptr= cur->page_cur.rec;
    const ulint n_fields= rec_get_n_fields_old(ptr);
    if (n_fields <= IBUF_REC_FIELD_USER)
      return DB_CORRUPTION;
    ulint len;
    ptr+= rec_get_nth_field_offs_old(ptr, n_fields - 1, &len);
    if (len != 4)
      return DB_CORRUPTION;
    page= mach_read_from_4(ptr);
  }
}

ATTRIBUTE_COLD dberr_t ibuf_upgrade()
{
  if (srv_read_only_mode)
  {
    sql_print_error("InnoDB: innodb_read_only_mode prevents an upgrade");
    return DB_READ_ONLY;
  }

  sql_print_information("InnoDB: Upgrading the change buffer");

#ifdef BTR_CUR_HASH_ADAPT
  const unsigned long ahi= btr_search.enabled;
  if (ahi)
    btr_search.disable();
#endif

  dict_table_t *ibuf_table= dict_table_t::create({C_STRING_WITH_LEN("ibuf")},
                                                 fil_system.sys_space,
                                                 1, 0, 0, 0);
  dict_index_t *ibuf_index=
    dict_mem_index_create(ibuf_table, "CLUST_IND", DICT_CLUSTERED, 1);
  ibuf_index->id= ibuf_index_id;
  ibuf_index->n_uniq= REC_MAX_N_FIELDS;
  ibuf_index->lock.SRW_LOCK_INIT(index_tree_rw_lock_key);
  ibuf_index->page= FSP_IBUF_TREE_ROOT_PAGE_NO;
  ut_d(ibuf_index->is_dummy= true);
  ut_d(ibuf_index->cached= true);

  size_t spaces=0, pages= 0;
  dberr_t err;
  mtr_t mtr{nullptr};
  mtr.start();
  mtr_x_lock_index(ibuf_index, &mtr);

  {
    btr_cur_t cur;
    uint32_t prev_space_id= ~0U;
    fil_space_t *space= nullptr;
    cur.page_cur.index= ibuf_index;
    log_free_check();
    err= ibuf_open(&cur, &mtr);

    while (err == DB_SUCCESS && !page_cur_is_after_last(&cur.page_cur))
    {
      const uint32_t space_id= ibuf_rec_get_space(cur.page_cur.rec);
      if (space_id != prev_space_id)
      {
        if (space)
          space->release();
        prev_space_id= space_id;
        space= fil_space_t::get(space_id);
        if (space)
        {
          /* Move to the next user tablespace. We buffer-fix the current
          change buffer leaf page to prevent it from being evicted
          before we have started a new mini-transaction. */
          cur.page_cur.block->fix();
          mtr.commit();
          log_free_check();
          mtr.start();
          mtr.page_lock(cur.page_cur.block, RW_X_LATCH);
          mtr.set_named_space(space);
        }
        spaces++;
      }
      pages++;
      err= ibuf_merge(space, &cur, &mtr);
      if (err == DB_SUCCESS)
      {
        /* Move to the next user index page. We buffer-fix the current
        change buffer leaf page to prevent it from being evicted
        before we have started a new mini-transaction. */
        cur.page_cur.block->fix();
        mtr.commit();

        if (recv_sys.report(time(nullptr)))
        {
          sql_print_information("InnoDB: merged changes to"
                                " %zu tablespaces, %zu pages", spaces, pages);
          service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                         "merged changes to"
                                         " %zu tablespaces, %zu pages",
                                         spaces, pages);
        }

        log_free_check();
        mtr.start();
        mtr.page_lock(cur.page_cur.block, RW_X_LATCH);
        if (space)
          mtr.set_named_space(space);
      }
    }
    mtr.commit();
    if (space)
      space->release();
  }

  if (err == DB_SUCCESS)
  {
    mtr.start();
    if (buf_block_t *root= buf_page_get_gen(ibuf_root, 0, RW_X_LATCH,
                                            nullptr, BUF_GET, &mtr, &err))
    {
      page_create(root, &mtr, false);
      mtr.write<2,mtr_t::MAYBE_NOP>(*root, PAGE_HEADER + PAGE_LEVEL +
                                    root->page.frame, 0U);
    }
    mtr.commit();

    while (err == DB_SUCCESS)
      err= ibuf_remove_free_page(mtr);

    if (err == DB_SUCCESS_LOCKED_REC)
      err= DB_SUCCESS;
  }

#ifdef BTR_CUR_HASH_ADAPT
  if (ahi)
    btr_search.enable(ahi, 0);
#endif

  ibuf_index->lock.free();
  dict_mem_index_free(ibuf_index);
  dict_mem_table_free(ibuf_table);

  if (err)
    sql_print_error("InnoDB: Unable to upgrade the change buffer");
  else
    sql_print_information("InnoDB: Upgraded the change buffer: "
                          "%zu tablespaces, %zu pages", spaces, pages);

  return err;
}

dberr_t ibuf_upgrade_needed()
{
  mtr_t mtr{nullptr};
  mtr.start();
  mtr.x_lock_space(fil_system.sys_space);
  dberr_t err;
  const buf_block_t *header_page= recv_sys.recover(ibuf_header, &mtr, &err);

  if (!header_page)
  {
  err_exit:
    sql_print_error("InnoDB: The change buffer is corrupted");
    if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO)
      err= DB_SUCCESS;
  func_exit:
    mtr.commit();
    return err;
  }

  const buf_block_t *root= recv_sys.recover(ibuf_root, &mtr, &err);
  if (!root)
    goto err_exit;

  if (UNIV_LIKELY(!page_has_siblings(root->page.frame)) &&
      UNIV_LIKELY(!memcmp(root->page.frame + FIL_PAGE_TYPE, field_ref_zero,
                          srv_page_size -
                          (FIL_PAGE_DATA_END + FIL_PAGE_TYPE))))
    /* the change buffer was removed; no need to upgrade */;
  else if (page_is_comp(root->page.frame) ||
           btr_page_get_index_id(root->page.frame) != ibuf_index_id ||
           fil_page_get_type(root->page.frame) != FIL_PAGE_INDEX)
  {
    err= DB_CORRUPTION;
    goto err_exit;
  }
  else if (srv_read_only_mode)
  {
    sql_print_error("InnoDB: innodb_read_only=ON prevents an upgrade"
                    " of the change buffer");
    err= DB_READ_ONLY;
  }
  else if (srv_force_recovery != SRV_FORCE_NO_LOG_REDO)
    err= DB_FAIL;

  goto func_exit;
}
