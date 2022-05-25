/*****************************************************************************

Copyright (c) 1996, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file trx/trx0rec.cc
Transaction undo log record

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rec.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0undo.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "ut0mem.h"
#include "row0ext.h"
#include "row0upd.h"
#include "que0que.h"
#include "trx0purge.h"
#include "trx0rseg.h"
#include "row0row.h"
#include "row0mysql.h"
#include "row0ins.h"

/** The search tuple corresponding to TRX_UNDO_INSERT_METADATA. */
const dtuple_t trx_undo_metadata = {
	/* This also works for REC_INFO_METADATA_ALTER, because the
	delete-mark (REC_INFO_DELETED_FLAG) is ignored when searching. */
	REC_INFO_METADATA_ADD, 0, 0,
	NULL, 0, NULL
#ifdef UNIV_DEBUG
	, DATA_TUPLE_MAGIC_N
#endif /* UNIV_DEBUG */
};

/*=========== UNDO LOG RECORD CREATION AND DECODING ====================*/

/** Calculate the free space left for extending an undo log record.
@param undo_block    undo log page
@param ptr           current end of the undo page
@return bytes left */
static ulint trx_undo_left(const buf_block_t *undo_block, const byte *ptr)
{
  ut_ad(ptr >=
        &undo_block->page.frame[TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE]);
  /* The 10 is supposed to be an extra safety margin (and needed for
  compatibility with older versions) */
  lint left= srv_page_size - (ptr - undo_block->page.frame) -
    (10 + FIL_PAGE_DATA_END);
  ut_ad(left >= 0);
  return left < 0 ? 0 : static_cast<ulint>(left);
}

/**********************************************************************//**
Set the next and previous pointers in the undo page for the undo record
that was written to ptr. Update the first free value by the number of bytes
written for this undo record.
@return offset of the inserted entry on the page if succeeded, 0 if fail */
static
uint16_t
trx_undo_page_set_next_prev_and_add(
/*================================*/
	buf_block_t*	undo_block,	/*!< in/out: undo log page */
	byte*		ptr,		/*!< in: ptr up to where data has been
					written on this undo page. */
	mtr_t*		mtr)		/*!< in: mtr */
{
  ut_ad(page_align(ptr) == undo_block->page.frame);

  if (UNIV_UNLIKELY(trx_undo_left(undo_block, ptr) < 2))
    return 0;

  byte *ptr_to_first_free= my_assume_aligned<2>(TRX_UNDO_PAGE_HDR +
						TRX_UNDO_PAGE_FREE +
						undo_block->page.frame);

  const uint16_t first_free= mach_read_from_2(ptr_to_first_free);

  /* Write offset of the previous undo log record */
  memcpy(ptr, ptr_to_first_free, 2);
  ptr += 2;

  const uint16_t end_of_rec= static_cast<uint16_t>
    (ptr - undo_block->page.frame);

  /* Update the offset to first free undo record */
  mach_write_to_2(ptr_to_first_free, end_of_rec);
  /* Write offset of the next undo log record */
  memcpy(undo_block->page.frame + first_free, ptr_to_first_free, 2);
  const byte *start= undo_block->page.frame + first_free + 2;

  mtr->undo_append(*undo_block, start, ptr - start - 2);
  return first_free;
}

/** Virtual column undo log version. To distinguish it from a length value
in 5.7.8 undo log, it starts with 0xF1 */
static const ulint VIRTUAL_COL_UNDO_FORMAT_1 = 0xF1;

/** Write virtual column index info (index id and column position in index)
to the undo log
@param[in,out]	undo_block	undo log page
@param[in]	table           the table
@param[in]	pos		the virtual column position
@param[in]      ptr             undo log record being written
@param[in]	first_v_col	whether this is the first virtual column
				which could start with a version marker
@return new undo log pointer */
static
byte*
trx_undo_log_v_idx(
	buf_block_t*		undo_block,
	const dict_table_t*	table,
	ulint			pos,
	byte*			ptr,
	bool			first_v_col)
{
	ut_ad(pos < table->n_v_def);
	dict_v_col_t*	vcol = dict_table_get_nth_v_col(table, pos);
	byte*		old_ptr;

	ut_ad(!vcol->v_indexes.empty());

	ulint		size = first_v_col ? 1 + 2 : 2;
	const ulint	avail = trx_undo_left(undo_block, ptr);

	/* The mach_write_compressed(ptr, flen) in
	trx_undo_page_report_modify() will consume additional 1 to 5 bytes. */
	if (avail < size + 5) {
		return(NULL);
	}

	ulint n_idx = 0;
	for (const auto& v_index : vcol->v_indexes) {
		n_idx++;
		/* FIXME: index->id is 64 bits! */
		size += mach_get_compressed_size(uint32_t(v_index.index->id));
		size += mach_get_compressed_size(v_index.nth_field);
	}

	size += mach_get_compressed_size(n_idx);

	if (avail < size + 5) {
		return(NULL);
	}

	ut_d(const byte* orig_ptr = ptr);

	if (first_v_col) {
		/* write the version marker */
		mach_write_to_1(ptr, VIRTUAL_COL_UNDO_FORMAT_1);

		ptr += 1;
	}

	old_ptr = ptr;

	ptr += 2;

	ptr += mach_write_compressed(ptr, n_idx);

	for (const auto& v_index : vcol->v_indexes) {
		ptr += mach_write_compressed(
			/* FIXME: index->id is 64 bits! */
			ptr, uint32_t(v_index.index->id));

		ptr += mach_write_compressed(ptr, v_index.nth_field);
	}

	ut_ad(orig_ptr + size == ptr);

	mach_write_to_2(old_ptr, ulint(ptr - old_ptr));

	return(ptr);
}

/** Read virtual column index from undo log, and verify the column is still
indexed, and return its position
@param[in]	table		the table
@param[in]	ptr		undo log pointer
@param[out]	col_pos		the column number or FIL_NULL
				if the column is not indexed any more
@return remaining part of undo log record after reading these values */
static
const byte*
trx_undo_read_v_idx_low(
	const dict_table_t*	table,
	const byte*		ptr,
	uint32_t*		col_pos)
{
	ulint		len = mach_read_from_2(ptr);
	const byte*	old_ptr = ptr;

	*col_pos = FIL_NULL;

	ptr += 2;

	ulint	num_idx = mach_read_next_compressed(&ptr);

	ut_ad(num_idx > 0);

	dict_index_t*	clust_index = dict_table_get_first_index(table);

	for (ulint i = 0; i < num_idx; i++) {
		index_id_t	id = mach_read_next_compressed(&ptr);
		ulint		pos = mach_read_next_compressed(&ptr);
		dict_index_t*	index = dict_table_get_next_index(clust_index);

		while (index != NULL) {
			/* Return if we find a matching index.
			TODO: in the future, it might be worth to add
			checks on other indexes */
			if (index->id == id) {
				const dict_col_t* col = dict_index_get_nth_col(
					index, pos);
				ut_ad(col->is_virtual());
				const dict_v_col_t*	vcol = reinterpret_cast<
					const dict_v_col_t*>(col);
				*col_pos = vcol->v_pos;
				return(old_ptr + len);
			}

			index = dict_table_get_next_index(index);
		}
	}

	return(old_ptr + len);
}

/** Read virtual column index from undo log or online log if the log
contains such info, and in the undo log case, verify the column is
still indexed, and output its position
@param[in]	table		the table
@param[in]	ptr		undo log pointer
@param[in]	first_v_col	if this is the first virtual column, which
				has the version marker
@param[in,out]	is_undo_log	this function is used to parse both undo log,
				and online log for virtual columns. So
				check to see if this is undo log. When
				first_v_col is true, is_undo_log is output,
				when first_v_col is false, is_undo_log is input
@param[out]	field_no	the column number, or FIL_NULL if not indexed
@return remaining part of undo log record after reading these values */
const byte*
trx_undo_read_v_idx(
	const dict_table_t*	table,
	const byte*		ptr,
	bool			first_v_col,
	bool*			is_undo_log,
	uint32_t*		field_no)
{
	/* Version marker only put on the first virtual column */
	if (first_v_col) {
		/* Undo log has the virtual undo log marker */
		*is_undo_log = (mach_read_from_1(ptr)
				== VIRTUAL_COL_UNDO_FORMAT_1);

		if (*is_undo_log) {
			ptr += 1;
		}
	}

	if (*is_undo_log) {
		ptr = trx_undo_read_v_idx_low(table, ptr, field_no);
	} else {
		*field_no -= REC_MAX_N_FIELDS;
	}

	return(ptr);
}

/** Reports in the undo log of an insert of virtual columns.
@param[in]	undo_block	undo log page
@param[in]	table		the table
@param[in]	row		dtuple contains the virtual columns
@param[in,out]	ptr		log ptr
@return true if write goes well, false if out of space */
static
bool
trx_undo_report_insert_virtual(
	buf_block_t*	undo_block,
	dict_table_t*	table,
	const dtuple_t*	row,
	byte**		ptr)
{
	byte*	start = *ptr;
	bool	first_v_col = true;

	if (trx_undo_left(undo_block, *ptr) < 2) {
		return(false);
	}

	/* Reserve 2 bytes to write the number
	of bytes the stored fields take in this
	undo record */
	*ptr += 2;

	for (ulint col_no = 0; col_no < dict_table_get_n_v_cols(table);
	     col_no++) {
		const dict_v_col_t*     col
			= dict_table_get_nth_v_col(table, col_no);

		if (col->m_col.ord_part) {

			/* make sure enought space to write the length */
			if (trx_undo_left(undo_block, *ptr) < 5) {
				return(false);
			}

			ulint   pos = col_no;
			pos += REC_MAX_N_FIELDS;
			*ptr += mach_write_compressed(*ptr, pos);

			*ptr = trx_undo_log_v_idx(undo_block, table,
						  col_no, *ptr, first_v_col);
			first_v_col = false;

			if (*ptr == NULL) {
				return(false);
			}

			const dfield_t* vfield = dtuple_get_nth_v_field(
				row, col->v_pos);
			switch (ulint flen = vfield->len) {
			case 0: case UNIV_SQL_NULL:
				if (trx_undo_left(undo_block, *ptr) < 5) {
					return(false);
				}

				*ptr += mach_write_compressed(*ptr, flen);
				break;
			default:
				ulint	max_len
					= dict_max_v_field_len_store_undo(
						table, col_no);

				if (flen > max_len) {
					flen = max_len;
				}

				if (trx_undo_left(undo_block, *ptr)
				    < flen + 5) {
					return(false);
				}
				*ptr += mach_write_compressed(*ptr, flen);

				memcpy(*ptr, vfield->data, flen);
				*ptr += flen;
			}
		}
	}

	/* Always mark the end of the log with 2 bytes length field */
	mach_write_to_2(start, ulint(*ptr - start));

	return(true);
}

/** Reports in the undo log of an insert of a clustered index record.
@param	undo_block	undo log page
@param	trx		transaction
@param	index		clustered index
@param	clust_entry	index entry which will be inserted to the
			clustered index
@param	mtr		mini-transaction
@param	write_empty	write empty table undo log record
@return offset of the inserted entry on the page if succeed, 0 if fail */
static
uint16_t
trx_undo_page_report_insert(
	buf_block_t*	undo_block,
	trx_t*		trx,
	dict_index_t*	index,
	const dtuple_t*	clust_entry,
	mtr_t*		mtr,
	bool		write_empty)
{
	ut_ad(index->is_primary());
	/* MariaDB 10.3.1+ in trx_undo_page_init() always initializes
	TRX_UNDO_PAGE_TYPE as 0, but previous versions wrote
	TRX_UNDO_INSERT == 1 into insert_undo pages,
	or TRX_UNDO_UPDATE == 2 into update_undo pages. */
	ut_ad(mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
			       + undo_block->page.frame) <= 2);

	uint16_t first_free = mach_read_from_2(my_assume_aligned<2>
					       (TRX_UNDO_PAGE_HDR
						+ TRX_UNDO_PAGE_FREE
						+ undo_block->page.frame));
	byte* ptr = undo_block->page.frame + first_free;

	if (trx_undo_left(undo_block, ptr) < 2 + 1 + 11 + 11) {
		/* Not enough space for writing the general parameters */
		return(0);
	}

	/* Reserve 2 bytes for the pointer to the next undo log record */
	ptr += 2;

	/* Store first some general parameters to the undo log */
	*ptr++ = TRX_UNDO_INSERT_REC;
	ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);
	ptr += mach_u64_write_much_compressed(ptr, index->table->id);

	if (write_empty) {
		/* Table is in bulk operation */
		undo_block->page.frame[first_free + 2] = TRX_UNDO_EMPTY;
		goto done;
	}

	/*----------------------------------------*/
	/* Store then the fields required to uniquely determine the record
	to be inserted in the clustered index */
	if (UNIV_UNLIKELY(clust_entry->info_bits != 0)) {
		ut_ad(clust_entry->is_metadata());
		ut_ad(index->is_instant());
		ut_ad(undo_block->page.frame[first_free + 2]
		      == TRX_UNDO_INSERT_REC);
		undo_block->page.frame[first_free + 2]
			= TRX_UNDO_INSERT_METADATA;
		goto done;
	}

	for (unsigned i = 0; i < dict_index_get_n_unique(index); i++) {

		const dfield_t*	field	= dtuple_get_nth_field(clust_entry, i);
		ulint		flen	= dfield_get_len(field);

		if (trx_undo_left(undo_block, ptr) < 5) {

			return(0);
		}

		ptr += mach_write_compressed(ptr, flen);

		switch (flen) {
		case 0: case UNIV_SQL_NULL:
			break;
		default:
			if (trx_undo_left(undo_block, ptr) < flen) {

				return(0);
			}

			memcpy(ptr, dfield_get_data(field), flen);
			ptr += flen;
		}
	}

	if (index->table->n_v_cols) {
		if (!trx_undo_report_insert_virtual(
			undo_block, index->table, clust_entry, &ptr)) {
			return(0);
		}
	}

done:
	return(trx_undo_page_set_next_prev_and_add(undo_block, ptr, mtr));
}

/**********************************************************************//**
Reads from an undo log record the general parameters.
@return remaining part of undo log record after reading these values */
byte*
trx_undo_rec_get_pars(
/*==================*/
	trx_undo_rec_t*	undo_rec,	/*!< in: undo log record */
	ulint*		type,		/*!< out: undo record type:
					TRX_UNDO_INSERT_REC, ... */
	ulint*		cmpl_info,	/*!< out: compiler info, relevant only
					for update type records */
	bool*		updated_extern,	/*!< out: true if we updated an
					externally stored fild */
	undo_no_t*	undo_no,	/*!< out: undo log record number */
	table_id_t*	table_id)	/*!< out: table id */
{
	const byte*	ptr;
	ulint		type_cmpl;

	ptr = undo_rec + 2;

	type_cmpl = mach_read_from_1(ptr);
	ptr++;

	*updated_extern = !!(type_cmpl & TRX_UNDO_UPD_EXTERN);
	type_cmpl &= ~TRX_UNDO_UPD_EXTERN;
	*type = type_cmpl & (TRX_UNDO_CMPL_INFO_MULT - 1);
	ut_ad(*type >= TRX_UNDO_RENAME_TABLE);
	ut_ad(*type <= TRX_UNDO_EMPTY);
	*cmpl_info = type_cmpl / TRX_UNDO_CMPL_INFO_MULT;

	*undo_no = mach_read_next_much_compressed(&ptr);
	*table_id = mach_read_next_much_compressed(&ptr);
	ut_ad(*table_id);

	return(const_cast<byte*>(ptr));
}

/** Read from an undo log record a non-virtual column value.
@param[in,out]	ptr		pointer to remaining part of the undo record
@param[in,out]	field		stored field
@param[in,out]	len		length of the field, or UNIV_SQL_NULL
@param[in,out]	orig_len	original length of the locally stored part
of an externally stored column, or 0
@return remaining part of undo log record after reading these values */
byte*
trx_undo_rec_get_col_val(
	const byte*	ptr,
	const byte**	field,
	uint32_t*	len,
	uint32_t*	orig_len)
{
	*len = mach_read_next_compressed(&ptr);
	*orig_len = 0;

	switch (*len) {
	case UNIV_SQL_NULL:
		*field = NULL;
		break;
	case UNIV_EXTERN_STORAGE_FIELD:
		*orig_len = mach_read_next_compressed(&ptr);
		*len = mach_read_next_compressed(&ptr);
		*field = ptr;
		ptr += *len & ~SPATIAL_STATUS_MASK;

		ut_ad(*orig_len >= BTR_EXTERN_FIELD_REF_SIZE);
		ut_ad(*len > *orig_len);
		/* @see dtuple_convert_big_rec() */
		ut_ad(*len >= BTR_EXTERN_FIELD_REF_SIZE);

		/* we do not have access to index->table here
		ut_ad(dict_table_has_atomic_blobs(index->table)
		      || *len >= col->max_prefix
		      + BTR_EXTERN_FIELD_REF_SIZE);
		*/

		*len += UNIV_EXTERN_STORAGE_FIELD;
		break;
	default:
		*field = ptr;
		if (*len >= UNIV_EXTERN_STORAGE_FIELD) {
			ptr += (*len - UNIV_EXTERN_STORAGE_FIELD)
				& ~SPATIAL_STATUS_MASK;
		} else {
			ptr += *len;
		}
	}

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds a row reference from an undo log record.
@return pointer to remaining part of undo record */
byte*
trx_undo_rec_get_row_ref(
/*=====================*/
	byte*		ptr,	/*!< in: remaining part of a copy of an undo log
				record, at the start of the row reference;
				NOTE that this copy of the undo log record must
				be preserved as long as the row reference is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	const dtuple_t**ref,	/*!< out, own: row reference */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
{
	ulint		ref_len;
	ulint		i;

	ut_ad(index && ptr && ref && heap);
	ut_a(dict_index_is_clust(index));

	ref_len = dict_index_get_n_unique(index);

	dtuple_t* tuple = dtuple_create(heap, ref_len);
	*ref = tuple;

	dict_index_copy_types(tuple, index, ref_len);

	for (i = 0; i < ref_len; i++) {
		const byte*	field;
		uint32_t	len, orig_len;

		dfield_t* dfield = dtuple_get_nth_field(tuple, i);

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		dfield_set_data(dfield, field, len);
	}

	return(ptr);
}

/*******************************************************************//**
Skips a row reference from an undo log record.
@return pointer to remaining part of undo record */
static
byte*
trx_undo_rec_skip_row_ref(
/*======================*/
	byte*		ptr,	/*!< in: remaining part in update undo log
				record, at the start of the row reference */
	dict_index_t*	index)	/*!< in: clustered index */
{
	ulint	ref_len;
	ulint	i;

	ut_ad(index && ptr);
	ut_a(dict_index_is_clust(index));

	ref_len = dict_index_get_n_unique(index);

	for (i = 0; i < ref_len; i++) {
		const byte*	field;
		uint32_t len, orig_len;

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);
	}

	return(ptr);
}

/** Fetch a prefix of an externally stored column, for writing to the undo
log of an update or delete marking of a clustered index record.
@param[out]	ext_buf		buffer to hold the prefix data and BLOB pointer
@param[in]	prefix_len	prefix size to store in the undo log
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	field		an externally stored column
@param[in,out]	len		input: length of field; output: used length of
ext_buf
@return ext_buf */
static
byte*
trx_undo_page_fetch_ext(
	byte*			ext_buf,
	ulint			prefix_len,
	ulint			zip_size,
	const byte*		field,
	ulint*			len)
{
	/* Fetch the BLOB. */
	ulint	ext_len = btr_copy_externally_stored_field_prefix(
		ext_buf, prefix_len, zip_size, field, *len);
	/* BLOBs should always be nonempty. */
	ut_a(ext_len);
	/* Append the BLOB pointer to the prefix. */
	memcpy(ext_buf + ext_len,
	       field + *len - BTR_EXTERN_FIELD_REF_SIZE,
	       BTR_EXTERN_FIELD_REF_SIZE);
	*len = ext_len + BTR_EXTERN_FIELD_REF_SIZE;
	return(ext_buf);
}

/** Writes to the undo log a prefix of an externally stored column.
@param[out]	ptr		undo log position, at least 15 bytes must be
available
@param[out]	ext_buf		a buffer of DICT_MAX_FIELD_LEN_BY_FORMAT()
				size, or NULL when should not fetch a longer
				prefix
@param[in]	prefix_len	prefix size to store in the undo log
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	field		the locally stored part of the externally
stored column
@param[in,out]	len		length of field, in bytes
@param[in]	spatial_status	whether the column is used by spatial index or
				regular index
@return undo log position */
static
byte*
trx_undo_page_report_modify_ext(
	byte*			ptr,
	byte*			ext_buf,
	ulint			prefix_len,
	ulint			zip_size,
	const byte**		field,
	ulint*			len,
	spatial_status_t	spatial_status)
{
	ulint	spatial_len= 0;

	switch (spatial_status) {
	case SPATIAL_UNKNOWN:
	case SPATIAL_NONE:
		break;

	case SPATIAL_MIXED:
	case SPATIAL_ONLY:
		spatial_len = DATA_MBR_LEN;
		break;
	}

	/* Encode spatial status into length. */
	spatial_len |= ulint(spatial_status) << SPATIAL_STATUS_SHIFT;

	if (spatial_status == SPATIAL_ONLY) {
		/* If the column is only used by gis index, log its
		MBR is enough.*/
		ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD
					     + spatial_len);

		return(ptr);
	}

	if (ext_buf) {
		ut_a(prefix_len > 0);

		/* If an ordering column is externally stored, we will
		have to store a longer prefix of the field.  In this
		case, write to the log a marker followed by the
		original length and the real length of the field. */
		ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD);

		ptr += mach_write_compressed(ptr, *len);

		*field = trx_undo_page_fetch_ext(ext_buf, prefix_len,
						 zip_size, *field, len);

		ptr += mach_write_compressed(ptr, *len + spatial_len);
	} else {
		ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD
					     + *len + spatial_len);
	}

	return(ptr);
}

/** Get MBR from a Geometry column stored externally
@param[out]	mbr		MBR to fill
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	field		field contain the geometry data
@param[in,out]	len		length of field, in bytes
*/
static
void
trx_undo_get_mbr_from_ext(
/*======================*/
	double*		mbr,
	ulint		zip_size,
	const byte*	field,
	ulint*		len)
{
	uchar*		dptr = NULL;
	ulint		dlen;
	mem_heap_t*	heap = mem_heap_create(100);

	dptr = btr_copy_externally_stored_field(
		&dlen, field, zip_size, *len, heap);

	if (dlen <= GEO_DATA_HEADER_SIZE) {
		for (uint i = 0; i < SPDIMS; ++i) {
			mbr[i * 2] = DBL_MAX;
			mbr[i * 2 + 1] = -DBL_MAX;
		}
	} else {
		rtree_mbr_from_wkb(dptr + GEO_DATA_HEADER_SIZE,
				   static_cast<uint>(dlen
				   - GEO_DATA_HEADER_SIZE), SPDIMS, mbr);
	}

	mem_heap_free(heap);
}

/**********************************************************************//**
Reports in the undo log of an update or delete marking of a clustered index
record.
@return byte offset of the inserted undo log entry on the page if
succeed, 0 if fail */
static
uint16_t
trx_undo_page_report_modify(
/*========================*/
	buf_block_t*	undo_block,	/*!< in: undo log page */
	trx_t*		trx,		/*!< in: transaction */
	dict_index_t*	index,		/*!< in: clustered index where update or
					delete marking is done */
	const rec_t*	rec,		/*!< in: clustered index record which
					has NOT yet been modified */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update,		/*!< in: update vector which tells the
					columns to be updated; in the case of
					a delete, this should be set to NULL */
	ulint		cmpl_info,	/*!< in: compiler info on secondary
					index updates */
	const dtuple_t*	row,		/*!< in: clustered index row contains
					virtual column info */
	mtr_t*		mtr)		/*!< in: mtr */
{
	ut_ad(index->is_primary());
	ut_ad(rec_offs_validate(rec, index, offsets));
	/* MariaDB 10.3.1+ in trx_undo_page_init() always initializes
	TRX_UNDO_PAGE_TYPE as 0, but previous versions wrote
	TRX_UNDO_INSERT == 1 into insert_undo pages,
	or TRX_UNDO_UPDATE == 2 into update_undo pages. */
	ut_ad(mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE
			       + undo_block->page.frame) <= 2);

	byte* ptr_to_first_free = my_assume_aligned<2>(
		TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
		+ undo_block->page.frame);

	const uint16_t first_free = mach_read_from_2(ptr_to_first_free);
	byte *ptr = undo_block->page.frame + first_free;

	if (trx_undo_left(undo_block, ptr) < 50) {
		/* NOTE: the value 50 must be big enough so that the general
		fields written below fit on the undo log page */
		return 0;
	}

	/* Reserve 2 bytes for the pointer to the next undo log record */
	ptr += 2;

	dict_table_t*	table		= index->table;
	const byte*	field;
	ulint		flen;
	ulint		col_no;
	ulint		type_cmpl;
	byte*		type_cmpl_ptr;
	ulint		i;
	trx_id_t	trx_id;
	ibool		ignore_prefix = FALSE;
	byte		ext_buf[REC_VERSION_56_MAX_INDEX_COL_LEN
				+ BTR_EXTERN_FIELD_REF_SIZE];
	bool		first_v_col = true;

	/* Store first some general parameters to the undo log */

	if (!update) {
		ut_ad(!rec_is_delete_marked(rec, dict_table_is_comp(table)));
		type_cmpl = TRX_UNDO_DEL_MARK_REC;
	} else if (rec_is_delete_marked(rec, dict_table_is_comp(table))) {
		/* In delete-marked records, DB_TRX_ID must
		always refer to an existing update_undo log record. */
		ut_ad(row_get_rec_trx_id(rec, index, offsets));

		type_cmpl = TRX_UNDO_UPD_DEL_REC;
		/* We are about to update a delete marked record.
		We don't typically need the prefix in this case unless
		the delete marking is done by the same transaction
		(which we check below). */
		ignore_prefix = TRUE;
	} else {
		type_cmpl = TRX_UNDO_UPD_EXIST_REC;
	}

	type_cmpl |= cmpl_info * TRX_UNDO_CMPL_INFO_MULT;
	type_cmpl_ptr = ptr;

	*ptr++ = (byte) type_cmpl;
	ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);

	ptr += mach_u64_write_much_compressed(ptr, table->id);

	/*----------------------------------------*/
	/* Store the state of the info bits */

	*ptr++ = (byte) rec_get_info_bits(rec, dict_table_is_comp(table));

	/* Store the values of the system columns */
	field = rec_get_nth_field(rec, offsets, index->db_trx_id(), &flen);
	ut_ad(flen == DATA_TRX_ID_LEN);

	trx_id = trx_read_trx_id(field);

	/* If it is an update of a delete marked record, then we are
	allowed to ignore blob prefixes if the delete marking was done
	by some other trx as it must have committed by now for us to
	allow an over-write. */
	if (trx_id == trx->id) {
		ignore_prefix = false;
	}
	ptr += mach_u64_write_compressed(ptr, trx_id);

	field = rec_get_nth_field(rec, offsets, index->db_roll_ptr(), &flen);
	ut_ad(flen == DATA_ROLL_PTR_LEN);
	ut_ad(memcmp(field, field_ref_zero, DATA_ROLL_PTR_LEN));

	ptr += mach_u64_write_compressed(ptr, trx_read_roll_ptr(field));

	/*----------------------------------------*/
	/* Store then the fields required to uniquely determine the
	record which will be modified in the clustered index */

	for (i = 0; i < dict_index_get_n_unique(index); i++) {

		/* The ordering columns must not be instant added columns. */
		ut_ad(!rec_offs_nth_default(offsets, i));
		field = rec_get_nth_field(rec, offsets, i, &flen);

		/* The ordering columns must not be stored externally. */
		ut_ad(!rec_offs_nth_extern(offsets, i));
		ut_ad(dict_index_get_nth_col(index, i)->ord_part);

		if (trx_undo_left(undo_block, ptr) < 5) {
			return(0);
		}

		ptr += mach_write_compressed(ptr, flen);

		if (flen != UNIV_SQL_NULL) {
			if (trx_undo_left(undo_block, ptr) < flen) {
				return(0);
			}

			memcpy(ptr, field, flen);
			ptr += flen;
		}
	}

	/*----------------------------------------*/
	/* Save to the undo log the old values of the columns to be updated. */

	if (update) {
		if (trx_undo_left(undo_block, ptr) < 5) {
			return(0);
		}

		ulint	n_updated = upd_get_n_fields(update);

		/* If this is an online update while an inplace alter table
		is in progress and the table has virtual column, we will
		need to double check if there are any non-indexed columns
		being registered in update vector in case they will be indexed
		in new table */
		if (dict_index_is_online_ddl(index) && table->n_v_cols > 0) {
			for (i = 0; i < upd_get_n_fields(update); i++) {
				upd_field_t*	fld = upd_get_nth_field(
					update, i);
				ulint		pos = fld->field_no;

				/* These columns must not have an index
				on them */
				if (upd_fld_is_virtual_col(fld)
				    && dict_table_get_nth_v_col(
					    table, pos)->v_indexes.empty()) {
					n_updated--;
				}
			}
		}

		i = 0;

		if (UNIV_UNLIKELY(update->is_alter_metadata())) {
			ut_ad(update->n_fields >= 1);
			ut_ad(!upd_fld_is_virtual_col(&update->fields[0]));
			ut_ad(update->fields[0].field_no
			      == index->first_user_field());
			ut_ad(!dfield_is_ext(&update->fields[0].new_val));
			ut_ad(!dfield_is_null(&update->fields[0].new_val));
			/* The instant ADD COLUMN metadata record does not
			contain the BLOB. Do not write anything for it. */
			i = !rec_is_alter_metadata(rec, *index);
			n_updated -= i;
		}

		ptr += mach_write_compressed(ptr, n_updated);

		for (; i < upd_get_n_fields(update); i++) {
			if (trx_undo_left(undo_block, ptr) < 5) {
				return 0;
			}

			upd_field_t*	fld = upd_get_nth_field(update, i);

			bool	is_virtual = upd_fld_is_virtual_col(fld);
			ulint	max_v_log_len = 0;

			ulint pos = fld->field_no;
			const dict_col_t* col = NULL;

			if (is_virtual) {
				/* Skip the non-indexed column, during
				an online alter table */
				if (dict_index_is_online_ddl(index)
				    && dict_table_get_nth_v_col(
					table, pos)->v_indexes.empty()) {
					continue;
				}

				/* add REC_MAX_N_FIELDS to mark this
				is a virtual col */
				ptr += mach_write_compressed(
					ptr, pos + REC_MAX_N_FIELDS);

				if (trx_undo_left(undo_block, ptr) < 15) {
					return 0;
				}

				ut_ad(fld->field_no < table->n_v_def);

				ptr = trx_undo_log_v_idx(undo_block, table,
							 fld->field_no, ptr,
							 first_v_col);
				if (ptr == NULL) {
					 return(0);
				}
				first_v_col = false;

				max_v_log_len
					= dict_max_v_field_len_store_undo(
						table, fld->field_no);

				field = static_cast<byte*>(
					fld->old_v_val->data);
				flen = fld->old_v_val->len;

				/* Only log sufficient bytes for index
				record update */
				if (flen != UNIV_SQL_NULL) {
					flen = ut_min(
						flen, max_v_log_len);
				}

				goto store_len;
			}

			if (UNIV_UNLIKELY(update->is_metadata())) {
				ut_ad(pos >= index->first_user_field());
				ut_ad(rec_is_metadata(rec, *index));

				if (rec_is_alter_metadata(rec, *index)) {
					ut_ad(update->is_alter_metadata());

					field = rec_offs_n_fields(offsets)
						> pos
						&& !rec_offs_nth_default(
							offsets, pos)
						? rec_get_nth_field(
							rec, offsets,
							pos, &flen)
						: index->instant_field_value(
							pos - 1, &flen);

					if (pos == index->first_user_field()) {
						ut_ad(rec_offs_nth_extern(
							offsets, pos));
						ut_ad(flen == FIELD_REF_SIZE);
						goto write_field;
					}
					col = dict_index_get_nth_col(index,
								     pos - 1);
				} else if (!update->is_alter_metadata()) {
					goto get_field;
				} else {
					/* We are converting an ADD COLUMN
					metadata record to an ALTER TABLE
					metadata record, with BLOB. Subtract
					the missing metadata BLOB field. */
					ut_ad(pos > index->first_user_field());
					--pos;
					goto get_field;
				}
			} else {
get_field:
				col = dict_index_get_nth_col(index, pos);
				field = rec_get_nth_cfield(
					rec, index, offsets, pos, &flen);
			}
write_field:
			/* Write field number to undo log */
			ptr += mach_write_compressed(ptr, pos);

			if (trx_undo_left(undo_block, ptr) < 15) {
				return 0;
			}

			if (rec_offs_n_fields(offsets) > pos
			    && rec_offs_nth_extern(offsets, pos)) {
				ut_ad(col || pos == index->first_user_field());
				ut_ad(col || update->is_alter_metadata());
				ut_ad(col
				      || rec_is_alter_metadata(rec, *index));
				ulint prefix_len = col
					? dict_max_field_len_store_undo(
						table, col)
					: 0;

				ut_ad(prefix_len + BTR_EXTERN_FIELD_REF_SIZE
				      <= sizeof ext_buf);

				ptr = trx_undo_page_report_modify_ext(
					ptr,
					col
					&& col->ord_part
					&& !ignore_prefix
					&& flen < REC_ANTELOPE_MAX_INDEX_COL_LEN
					? ext_buf : NULL, prefix_len,
					table->space->zip_size(),
					&field, &flen, SPATIAL_UNKNOWN);

				*type_cmpl_ptr |= TRX_UNDO_UPD_EXTERN;
			} else {
store_len:
				ptr += mach_write_compressed(ptr, flen);
			}

			if (flen != UNIV_SQL_NULL) {
				if (trx_undo_left(undo_block, ptr) < flen) {
					return(0);
				}

				memcpy(ptr, field, flen);
				ptr += flen;
			}

			/* Also record the new value for virtual column */
			if (is_virtual) {
				field = static_cast<byte*>(fld->new_val.data);
				flen = fld->new_val.len;
				if (flen != UNIV_SQL_NULL) {
					flen = ut_min(
						flen, max_v_log_len);
				}

				if (trx_undo_left(undo_block, ptr) < 15) {
					return(0);
				}

				ptr += mach_write_compressed(ptr, flen);

				if (flen != UNIV_SQL_NULL) {
					if (trx_undo_left(undo_block, ptr)
					    < flen) {
						return(0);
					}

					memcpy(ptr, field, flen);
					ptr += flen;
				}
			}
		}
	}

	/* Reset the first_v_col, so to put the virtual column undo
	version marker again, when we log all the indexed columns */
	first_v_col = true;

	/*----------------------------------------*/
	/* In the case of a delete marking, and also in the case of an update
	where any ordering field of any index changes, store the values of all
	columns which occur as ordering fields in any index. This info is used
	in the purge of old versions where we use it to build and search the
	delete marked index records, to look if we can remove them from the
	index tree. Note that starting from 4.0.14 also externally stored
	fields can be ordering in some index. Starting from 5.2, we no longer
	store REC_MAX_INDEX_COL_LEN first bytes to the undo log record,
	but we can construct the column prefix fields in the index by
	fetching the first page of the BLOB that is pointed to by the
	clustered index. This works also in crash recovery, because all pages
	(including BLOBs) are recovered before anything is rolled back. */

	if (!update || !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
		byte*		old_ptr = ptr;
		double		mbr[SPDIMS * 2];
		mem_heap_t*	row_heap = NULL;

		if (trx_undo_left(undo_block, ptr) < 5) {
			return(0);
		}

		/* Reserve 2 bytes to write the number of bytes the stored
		fields take in this undo record */

		ptr += 2;

		for (col_no = 0; col_no < dict_table_get_n_cols(table);
		     col_no++) {

			const dict_col_t*	col
				= dict_table_get_nth_col(table, col_no);

			if (!col->ord_part) {
				continue;
			}

			const ulint pos = dict_index_get_nth_col_pos(
				index, col_no, NULL);
			/* All non-virtual columns must be present in
			the clustered index. */
			ut_ad(pos != ULINT_UNDEFINED);

			const bool is_ext = rec_offs_nth_extern(offsets, pos);
			const spatial_status_t spatial_status = is_ext
				? dict_col_get_spatial_status(col)
				: SPATIAL_NONE;

			switch (spatial_status) {
			case SPATIAL_UNKNOWN:
				ut_ad(0);
				/* fall through */
			case SPATIAL_MIXED:
			case SPATIAL_ONLY:
				/* Externally stored spatially indexed
				columns will be (redundantly) logged
				again, because we did not write the
				MBR yet, that is, the previous call to
				trx_undo_page_report_modify_ext()
				was with SPATIAL_UNKNOWN. */
				break;
			case SPATIAL_NONE:
				if (!update) {
					/* This is a DELETE operation. */
					break;
				}
				/* Avoid redundantly logging indexed
				columns that were updated. */

				for (i = 0; i < update->n_fields; i++) {
					const ulint field_no
						= upd_get_nth_field(update, i)
						->field_no;
					if (field_no >= index->n_fields
					    || dict_index_get_nth_field(
						    index, field_no)->col
					    == col) {
						goto already_logged;
					}
				}
			}

			if (true) {
				/* Write field number to undo log */
				if (trx_undo_left(undo_block, ptr) < 5 + 15) {
					return(0);
				}

				ptr += mach_write_compressed(ptr, pos);

				/* Save the old value of field */
				field = rec_get_nth_cfield(
					rec, index, offsets, pos, &flen);

				if (is_ext) {
					const dict_col_t*	col =
						dict_index_get_nth_col(
							index, pos);
					ulint			prefix_len =
						dict_max_field_len_store_undo(
							table, col);

					ut_a(prefix_len < sizeof ext_buf);
					const ulint zip_size
						= table->space->zip_size();

					/* If there is a spatial index on it,
					log its MBR */
					if (spatial_status != SPATIAL_NONE) {
						ut_ad(DATA_GEOMETRY_MTYPE(
								col->mtype));

						trx_undo_get_mbr_from_ext(
							mbr, zip_size,
							field, &flen);
					}

					ptr = trx_undo_page_report_modify_ext(
						ptr,
						flen < REC_ANTELOPE_MAX_INDEX_COL_LEN
						&& !ignore_prefix
						? ext_buf : NULL, prefix_len,
						zip_size,
						&field, &flen,
						spatial_status);
				} else {
					ptr += mach_write_compressed(
						ptr, flen);
				}

				if (flen != UNIV_SQL_NULL
				    && spatial_status != SPATIAL_ONLY) {
					if (trx_undo_left(undo_block, ptr)
					    < flen) {
						return(0);
					}

					memcpy(ptr, field, flen);
					ptr += flen;
				}

				if (spatial_status != SPATIAL_NONE) {
					if (trx_undo_left(undo_block, ptr)
					    < DATA_MBR_LEN) {
						return(0);
					}

					for (int i = 0; i < SPDIMS * 2;
					     i++) {
						mach_double_write(
							ptr, mbr[i]);
						ptr +=  sizeof(double);
					}
				}
			}

already_logged:
			continue;
		}

		for (col_no = 0; col_no < dict_table_get_n_v_cols(table);
		     col_no++) {
			const dict_v_col_t*     col
				= dict_table_get_nth_v_col(table, col_no);

			if (col->m_col.ord_part) {
				ulint   pos = col_no;
				ulint	max_v_log_len
					= dict_max_v_field_len_store_undo(
						table, pos);

				/* Write field number to undo log.
				Make sure there is enought space in log */
				if (trx_undo_left(undo_block, ptr) < 5) {
					return(0);
				}

				pos += REC_MAX_N_FIELDS;
				ptr += mach_write_compressed(ptr, pos);

				ut_ad(col_no < table->n_v_def);
				ptr = trx_undo_log_v_idx(undo_block, table,
							 col_no, ptr,
							 first_v_col);
				first_v_col = false;

				if (!ptr) {
					 return(0);
				}

				const dfield_t* vfield = NULL;

				if (update) {
					ut_ad(!row);
					if (update->old_vrow == NULL) {
						flen = UNIV_SQL_NULL;
					} else {
						vfield = dtuple_get_nth_v_field(
							update->old_vrow,
							col->v_pos);
					}
				} else if (row) {
					vfield = dtuple_get_nth_v_field(
						row, col->v_pos);
				} else {
					ut_ad(0);
				}

				if (vfield) {
					field = static_cast<byte*>(vfield->data);
					flen = vfield->len;
				} else {
					ut_ad(flen == UNIV_SQL_NULL);
				}

				if (flen != UNIV_SQL_NULL) {
					flen = ut_min(
						flen, max_v_log_len);
				}

				ptr += mach_write_compressed(ptr, flen);

				switch (flen) {
				case 0: case UNIV_SQL_NULL:
					break;
				default:
					if (trx_undo_left(undo_block, ptr)
					    < flen) {
						return(0);
					}

					memcpy(ptr, field, flen);
					ptr += flen;
				}
			}
		}

		mach_write_to_2(old_ptr, ulint(ptr - old_ptr));

		if (row_heap) {
			mem_heap_free(row_heap);
		}
	}

	/*----------------------------------------*/
	/* Write pointers to the previous and the next undo log records */
	if (trx_undo_left(undo_block, ptr) < 2) {
		return(0);
	}

	mach_write_to_2(ptr, first_free);
	const uint16_t new_free = static_cast<uint16_t>(
		ptr + 2 - undo_block->page.frame);
	mach_write_to_2(undo_block->page.frame + first_free, new_free);

	mach_write_to_2(ptr_to_first_free, new_free);

	const byte* start = &undo_block->page.frame[first_free + 2];
	mtr->undo_append(*undo_block, start, ptr - start);
	return(first_free);
}

/**********************************************************************//**
Reads from an undo log update record the system field values of the old
version.
@return remaining part of undo log record after reading these values */
byte*
trx_undo_update_rec_get_sys_cols(
/*=============================*/
	const byte*	ptr,		/*!< in: remaining part of undo
					log record after reading
					general parameters */
	trx_id_t*	trx_id,		/*!< out: trx id */
	roll_ptr_t*	roll_ptr,	/*!< out: roll ptr */
	byte*		info_bits)	/*!< out: info bits state */
{
	/* Read the state of the info bits */
	*info_bits = *ptr++;

	/* Read the values of the system columns */

	*trx_id = mach_u64_read_next_compressed(&ptr);
	*roll_ptr = mach_u64_read_next_compressed(&ptr);

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds an update vector based on a remaining part of an undo log record.
@return remaining part of the record, NULL if an error detected, which
means that the record is corrupted */
byte*
trx_undo_update_rec_get_update(
/*===========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record, after reading the row reference
				NOTE that this copy of the undo log record must
				be preserved as long as the update vector is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint		type,	/*!< in: TRX_UNDO_UPD_EXIST_REC,
				TRX_UNDO_UPD_DEL_REC, or
				TRX_UNDO_DEL_MARK_REC; in the last case,
				only trx id and roll ptr fields are added to
				the update vector */
	trx_id_t	trx_id,	/*!< in: transaction id from this undo record */
	roll_ptr_t	roll_ptr,/*!< in: roll pointer from this undo record */
	byte		info_bits,/*!< in: info bits from this undo record */
	mem_heap_t*	heap,	/*!< in: memory heap from which the memory
				needed is allocated */
	upd_t**		upd)	/*!< out, own: update vector */
{
	upd_field_t*	upd_field;
	upd_t*		update;
	ulint		n_fields;
	byte*		buf;
	bool		first_v_col = true;
	bool		is_undo_log = true;
	ulint		n_skip_field = 0;

	ut_a(dict_index_is_clust(index));

	if (type != TRX_UNDO_DEL_MARK_REC) {
		n_fields = mach_read_next_compressed(&ptr);
	} else {
		n_fields = 0;
	}

	*upd = update = upd_create(n_fields + 2, heap);

	update->info_bits = info_bits;

	/* Store first trx id and roll ptr to update vector */

	upd_field = upd_get_nth_field(update, n_fields);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_TRX_ID_LEN));

	mach_write_to_6(buf, trx_id);

	upd_field_set_field_no(upd_field, index->db_trx_id(), index);
	dfield_set_data(&(upd_field->new_val), buf, DATA_TRX_ID_LEN);

	upd_field = upd_get_nth_field(update, n_fields + 1);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_ROLL_PTR_LEN));

	trx_write_roll_ptr(buf, roll_ptr);

	upd_field_set_field_no(upd_field, index->db_roll_ptr(), index);
	dfield_set_data(&(upd_field->new_val), buf, DATA_ROLL_PTR_LEN);

	/* Store then the updated ordinary columns to the update vector */

	for (ulint i = 0; i < n_fields; i++) {
		const byte* field;
		uint32_t len, orig_len;

		upd_field = upd_get_nth_field(update, i);
		uint32_t field_no = mach_read_next_compressed(&ptr);

		const bool is_virtual = (field_no >= REC_MAX_N_FIELDS);

		if (is_virtual) {
			/* If new version, we need to check index list to figure
			out the correct virtual column position */
			ptr = trx_undo_read_v_idx(
				index->table, ptr, first_v_col, &is_undo_log,
				&field_no);
			first_v_col = false;
			/* This column could be dropped or no longer indexed */
			if (field_no >= index->n_fields) {
				/* Mark this is no longer needed */
				upd_field->field_no = REC_MAX_N_FIELDS;

				ptr = trx_undo_rec_get_col_val(
					ptr, &field, &len, &orig_len);
				ptr = trx_undo_rec_get_col_val(
					ptr, &field, &len, &orig_len);
				n_skip_field++;
				continue;
			}

			upd_field_set_v_field_no(
				upd_field, static_cast<uint16_t>(field_no),
				index);
		} else if (UNIV_UNLIKELY((update->info_bits
					  & ~REC_INFO_DELETED_FLAG)
					 == REC_INFO_MIN_REC_FLAG)) {
			ut_ad(type == TRX_UNDO_UPD_EXIST_REC);
			const uint32_t uf = index->first_user_field();
			ut_ad(field_no >= uf);

			if (update->info_bits != REC_INFO_MIN_REC_FLAG) {
				/* Generic instant ALTER TABLE */
				if (field_no == uf) {
					upd_field->new_val.type
						.metadata_blob_init();
				} else if (field_no >= index->n_fields) {
					/* This is reachable during
					purge if the table was emptied
					and converted to the canonical
					format on a later ALTER TABLE.
					In this case,
					row_purge_upd_exist_or_extern()
					would only be interested in
					freeing any BLOBs that were
					updated, that is, the metadata
					BLOB above.  Other BLOBs in
					the metadata record are never
					updated; they are for the
					initial DEFAULT values of the
					instantly added columns, and
					they will never change.

					Note: if the table becomes
					empty during ROLLBACK or is
					empty during subsequent ALTER
					TABLE, and btr_page_empty() is
					called to re-create the root
					page without the metadata
					record, in that case we should
					only free the latest version
					of BLOBs in the record,
					which purge would never touch. */
					field_no = REC_MAX_N_FIELDS;
					n_skip_field++;
				} else {
					dict_col_copy_type(
						dict_index_get_nth_col(
							index, field_no - 1),
						&upd_field->new_val.type);
				}
			} else {
				/* Instant ADD COLUMN...LAST */
				dict_col_copy_type(
					dict_index_get_nth_col(index,
							       field_no),
					&upd_field->new_val.type);
			}
			upd_field->field_no = field_no
				& dict_index_t::MAX_N_FIELDS;
		} else if (field_no < index->n_fields) {
			upd_field_set_field_no(upd_field,
					       static_cast<uint16_t>(field_no),
					       index);
		} else {
			ib::error() << "Trying to access update undo rec"
				" field " << field_no
				<< " in index " << index->name
				<< " of table " << index->table->name
				<< " but index has only "
				<< dict_index_get_n_fields(index)
				<< " fields " << BUG_REPORT_MSG
				<< ". Run also CHECK TABLE "
				<< index->table->name << "."
				" n_fields = " << n_fields << ", i = " << i;

			ut_ad(0);
			*upd = NULL;
			return(NULL);
		}

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		upd_field->orig_len = static_cast<uint16_t>(orig_len);

		if (len == UNIV_SQL_NULL) {
			dfield_set_null(&upd_field->new_val);
		} else if (len < UNIV_EXTERN_STORAGE_FIELD) {
			dfield_set_data(&upd_field->new_val, field, len);
		} else {
			len -= UNIV_EXTERN_STORAGE_FIELD;

			dfield_set_data(&upd_field->new_val, field, len);
			dfield_set_ext(&upd_field->new_val);
		}

		ut_ad(update->info_bits != (REC_INFO_DELETED_FLAG
					    | REC_INFO_MIN_REC_FLAG)
		      || field_no != index->first_user_field()
		      || (upd_field->new_val.ext
			  && upd_field->new_val.len == FIELD_REF_SIZE));

		if (is_virtual) {
			upd_field->old_v_val = static_cast<dfield_t*>(
				mem_heap_alloc(
					heap, sizeof *upd_field->old_v_val));
			ptr = trx_undo_rec_get_col_val(
				ptr, &field, &len, &orig_len);
	                if (len == UNIV_SQL_NULL) {
				dfield_set_null(upd_field->old_v_val);
			} else if (len < UNIV_EXTERN_STORAGE_FIELD) {
				dfield_set_data(
					upd_field->old_v_val, field, len);
			} else {
				ut_ad(0);
			}
		}
	}

	/* We may have to skip dropped indexed virtual columns.
	Also, we may have to trim the update vector of a metadata record
	if dict_index_t::clear_instant_alter() was invoked on the table
	later, and the number of fields no longer matches. */

	if (n_skip_field) {
		upd_field_t* d = upd_get_nth_field(update, 0);
		const upd_field_t* const end = d + n_fields + 2;

		for (const upd_field_t* s = d; s != end; s++) {
			if (s->field_no != REC_MAX_N_FIELDS) {
				*d++ = *s;
			}
		}

		ut_ad(d + n_skip_field == end);
		update->n_fields = d - upd_get_nth_field(update, 0);
	}

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds a partial row from an update undo log record, for purge.
It contains the columns which occur as ordering in any index of the table.
Any missing columns are indicated by col->mtype == DATA_MISSING.
@return pointer to remaining part of undo record */
byte*
trx_undo_rec_get_partial_row(
/*=========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record of a suitable type, at the start of
				the stored index columns;
				NOTE that this copy of the undo log record must
				be preserved as long as the partial row is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	const upd_t*	update,	/*!< in: updated columns */
	dtuple_t**	row,	/*!< out, own: partial row */
	ibool		ignore_prefix, /*!< in: flag to indicate if we
				expect blob prefixes in undo. Used
				only in the assertion. */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
{
	const byte*	end_ptr;
	bool		first_v_col = true;
	bool		is_undo_log = true;

	ut_ad(index->is_primary());

	*row = dtuple_create_with_vcol(
		heap, dict_table_get_n_cols(index->table),
		dict_table_get_n_v_cols(index->table));

	/* Mark all columns in the row uninitialized, so that
	we can distinguish missing fields from fields that are SQL NULL. */
	for (ulint i = 0; i < dict_table_get_n_cols(index->table); i++) {
		dfield_get_type(dtuple_get_nth_field(*row, i))
			->mtype = DATA_MISSING;
	}

	dtuple_init_v_fld(*row);

	for (const upd_field_t* uf = update->fields, * const ue
		     = update->fields + update->n_fields;
	     uf != ue; uf++) {
		if (uf->old_v_val) {
			continue;
		}
		const dict_col_t& c = *dict_index_get_nth_col(index,
							      uf->field_no);
		if (!c.is_dropped()) {
			*dtuple_get_nth_field(*row, c.ind) = uf->new_val;
		}
	}

	end_ptr = ptr + mach_read_from_2(ptr);
	ptr += 2;

	while (ptr != end_ptr) {
		dfield_t*	dfield;
		const byte*	field;
		uint32_t	field_no;
		const dict_col_t* col;
		uint32_t len, orig_len;

		field_no = mach_read_next_compressed(&ptr);

		const bool is_virtual = (field_no >= REC_MAX_N_FIELDS);

		if (is_virtual) {
			ptr = trx_undo_read_v_idx(
				index->table, ptr, first_v_col, &is_undo_log,
				&field_no);
			first_v_col = false;
		}

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		/* This column could be dropped or no longer indexed */
		if (field_no == FIL_NULL) {
			ut_ad(is_virtual);
			continue;
		}

		if (is_virtual) {
			dict_v_col_t* vcol = dict_table_get_nth_v_col(
						index->table, field_no);
			col = &vcol->m_col;
			dfield = dtuple_get_nth_v_field(*row, vcol->v_pos);
			dict_col_copy_type(
				&vcol->m_col,
				dfield_get_type(dfield));
		} else {
			col = dict_index_get_nth_col(index, field_no);

			if (col->is_dropped()) {
				continue;
			}

			dfield = dtuple_get_nth_field(*row, col->ind);
			ut_ad(dfield->type.mtype == DATA_MISSING
			      || dict_col_type_assert_equal(col,
							    &dfield->type));
			ut_ad(dfield->type.mtype == DATA_MISSING
			      || dfield->len == len
			      || (len != UNIV_SQL_NULL
				  && len >= UNIV_EXTERN_STORAGE_FIELD));
			dict_col_copy_type(col, dfield_get_type(dfield));
		}

		dfield_set_data(dfield, field, len);

		if (len != UNIV_SQL_NULL
		    && len >= UNIV_EXTERN_STORAGE_FIELD) {
			spatial_status_t	spatial_status;

			/* Decode spatial status. */
			spatial_status = static_cast<spatial_status_t>(
				(len & SPATIAL_STATUS_MASK)
				>> SPATIAL_STATUS_SHIFT);
			len &= ~SPATIAL_STATUS_MASK;

			/* Keep compatible with 5.7.9 format. */
			if (spatial_status == SPATIAL_UNKNOWN) {
				spatial_status =
					dict_col_get_spatial_status(col);
			}

			switch (spatial_status) {
			case SPATIAL_ONLY:
				ut_ad(len - UNIV_EXTERN_STORAGE_FIELD
				      == DATA_MBR_LEN);
				dfield_set_len(
					dfield,
					len - UNIV_EXTERN_STORAGE_FIELD);
				break;

			case SPATIAL_MIXED:
				dfield_set_len(
					dfield,
					len - UNIV_EXTERN_STORAGE_FIELD
					- DATA_MBR_LEN);
				break;

			case SPATIAL_NONE:
				dfield_set_len(
					dfield,
					len - UNIV_EXTERN_STORAGE_FIELD);
				break;

			case SPATIAL_UNKNOWN:
				ut_ad(0);
				break;
			}

			dfield_set_ext(dfield);
			dfield_set_spatial_status(dfield, spatial_status);

			/* If the prefix of this column is indexed,
			ensure that enough prefix is stored in the
			undo log record. */
			if (!ignore_prefix && col->ord_part
			    && spatial_status != SPATIAL_ONLY) {
				ut_a(dfield_get_len(dfield)
				     >= BTR_EXTERN_FIELD_REF_SIZE);
				ut_a(dict_table_has_atomic_blobs(index->table)
				     || dfield_get_len(dfield)
				     >= REC_ANTELOPE_MAX_INDEX_COL_LEN
				     + BTR_EXTERN_FIELD_REF_SIZE);
			}
		}
	}

	return(const_cast<byte*>(ptr));
}

/** Report a RENAME TABLE operation.
@param[in,out]	trx	transaction
@param[in]	table	table that is being renamed
@param[in,out]	block	undo page
@param[in,out]	mtr	mini-transaction
@return	byte offset of the undo log record
@retval	0	in case of failure */
static
uint16_t
trx_undo_page_report_rename(trx_t* trx, const dict_table_t* table,
			    buf_block_t* block, mtr_t* mtr)
{
	byte*	ptr_first_free  = my_assume_aligned<2>(TRX_UNDO_PAGE_HDR
						       + TRX_UNDO_PAGE_FREE
						       + block->page.frame);
	const uint16_t first_free = mach_read_from_2(ptr_first_free);
	ut_ad(first_free >= TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
	ut_ad(first_free <= srv_page_size - FIL_PAGE_DATA_END);
	byte* const start = block->page.frame + first_free;
	size_t len = strlen(table->name.m_name);
	const size_t fixed = 2 + 1 + 11 + 11 + 2;
	ut_ad(len <= NAME_LEN * 2 + 1);
	/* The -10 is used in trx_undo_left() */
	compile_time_assert((NAME_LEN * 1) * 2 + fixed
			    + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE
			    < UNIV_PAGE_SIZE_MIN - 10 - FIL_PAGE_DATA_END);

	if (trx_undo_left(block, start) < fixed + len) {
		ut_ad(first_free > TRX_UNDO_PAGE_HDR
		      + TRX_UNDO_PAGE_HDR_SIZE);
		return 0;
	}

	byte* ptr = start + 2;
	*ptr++ = TRX_UNDO_RENAME_TABLE;
	ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);
	ptr += mach_u64_write_much_compressed(ptr, table->id);
	memcpy(ptr, table->name.m_name, len);
	ptr += len;
	mach_write_to_2(ptr, first_free);
	mach_write_to_2(ptr_first_free, ptr + 2 - block->page.frame);
	memcpy(start, ptr_first_free, 2);
	mtr->undo_append(*block, start + 2, ptr - start - 2);
	return first_free;
}

/** Report a RENAME TABLE operation.
@param[in,out]	trx	transaction
@param[in]	table	table that is being renamed
@return	DB_SUCCESS or error code */
dberr_t trx_undo_report_rename(trx_t* trx, const dict_table_t* table)
{
	ut_ad(!trx->read_only);
	ut_ad(trx->id);
	ut_ad(!table->is_temporary());

	mtr_t		mtr;
	dberr_t		err;
	mtr.start();
	if (buf_block_t* block = trx_undo_assign(trx, &err, &mtr)) {
		trx_undo_t*	undo = trx->rsegs.m_redo.undo;
		ut_ad(err == DB_SUCCESS);
		ut_ad(undo);
		for (ut_d(int loop_count = 0);;) {
			ut_ad(loop_count++ < 2);
			ut_ad(undo->last_page_no
			      == block->page.id().page_no());

			if (uint16_t offset = trx_undo_page_report_rename(
				    trx, table, block, &mtr)) {
				undo->top_page_no = undo->last_page_no;
				undo->top_offset  = offset;
				undo->top_undo_no = trx->undo_no++;
				undo->guess_block = block;
				ut_ad(!undo->empty());

				err = DB_SUCCESS;
				break;
			} else {
				mtr.commit();
				mtr.start();
				block = trx_undo_add_page(undo, &mtr);
				if (!block) {
					err = DB_OUT_OF_FILE_SPACE;
					break;
				}
			}
		}
	}

	mtr.commit();
	return err;
}

TRANSACTIONAL_TARGET ATTRIBUTE_NOINLINE
/** @return whether the transaction holds an exclusive lock on a table */
static bool trx_has_lock_x(const trx_t &trx, dict_table_t& table)
{
  ut_ad(!table.is_temporary());

  uint32_t n;

#if !defined NO_ELISION && !defined SUX_LOCK_GENERIC
  if (xbegin())
  {
    if (table.lock_mutex_is_locked())
      xabort();
    n= table.n_lock_x_or_s;
    xend();
  }
  else
#endif
  {
    table.lock_mutex_lock();
    n= table.n_lock_x_or_s;
    table.lock_mutex_unlock();
  }

  /* This thread is executing trx. No other thread can modify our table locks
  (only record locks might be created, in an implicit-to-explicit conversion).
  Hence, no mutex is needed here. */
  if (n)
    for (const lock_t *lock : trx.lock.table_locks)
      if (lock && lock->type_mode == (LOCK_X | LOCK_TABLE))
        return true;

  return false;
}

/***********************************************************************//**
Writes information to an undo log about an insert, update, or a delete marking
of a clustered index record. This information is used in a rollback of the
transaction and in consistent reads that must look to the history of this
transaction.
@return DB_SUCCESS or error code */
dberr_t
trx_undo_report_row_operation(
/*==========================*/
	que_thr_t*	thr,		/*!< in: query thread */
	dict_index_t*	index,		/*!< in: clustered index */
	const dtuple_t*	clust_entry,	/*!< in: in the case of an insert,
					index entry to insert into the
					clustered index; in updates,
					may contain a clustered index
					record tuple that also contains
					virtual columns of the table;
					otherwise, NULL */
	const upd_t*	update,		/*!< in: in the case of an update,
					the update vector, otherwise NULL */
	ulint		cmpl_info,	/*!< in: compiler info on secondary
					index updates */
	const rec_t*	rec,		/*!< in: case of an update or delete
					marking, the record in the clustered
					index; NULL if insert */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec) */
	roll_ptr_t*	roll_ptr)	/*!< out: DB_ROLL_PTR to the
					undo log record */
{
	trx_t*		trx;
#ifdef UNIV_DEBUG
	int		loop_count	= 0;
#endif /* UNIV_DEBUG */

	ut_a(dict_index_is_clust(index));
	ut_ad(!update || rec);
	ut_ad(!rec || rec_offs_validate(rec, index, offsets));
	ut_ad(!srv_read_only_mode);

	trx = thr_get_trx(thr);
	/* This function must not be invoked during rollback
	(of a TRX_STATE_PREPARE transaction or otherwise). */
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
	ut_ad(!trx->in_rollback);

	/* We must determine if this is the first time when this
	transaction modifies this table. */
	auto m = trx->mod_tables.emplace(index->table, trx->undo_no);
	ut_ad(m.first->second.valid(trx->undo_no));

	if (m.second && index->table->is_active_ddl()) {
		trx->apply_online_log= true;
	}

	bool bulk = !rec;

	if (!bulk) {
		/* An UPDATE or DELETE must not be covered by an
		earlier start_bulk_insert(). */
		ut_ad(!m.first->second.is_bulk_insert());
	} else if (m.first->second.is_bulk_insert()) {
		/* Above, the emplace() tried to insert an object with
		!is_bulk_insert(). Only an explicit start_bulk_insert()
		(below) can set the flag. */
		ut_ad(!m.second);
		/* We already wrote a TRX_UNDO_EMPTY record. */
		ut_ad(thr->run_node);
		ut_ad(que_node_get_type(thr->run_node) == QUE_NODE_INSERT);
		ut_ad(trx->bulk_insert);
		return DB_SUCCESS;
	} else if (!m.second || !trx->bulk_insert) {
		bulk = false;
	} else if (index->table->is_temporary()) {
	} else if (trx_has_lock_x(*trx, *index->table)
		   && index->table->bulk_trx_id == trx->id) {
		m.first->second.start_bulk_insert(index->table);

		if (dberr_t err = m.first->second.bulk_insert_buffered(
			    *clust_entry, *index, trx)) {
			return err;
		}
	} else {
		bulk = false;
	}

	mtr_t		mtr;
	mtr.start();
	trx_undo_t**	pundo;
	trx_rseg_t*	rseg;
	const bool	is_temp	= index->table->is_temporary();

	if (is_temp) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);

		rseg = trx->get_temp_rseg();
		pundo = &trx->rsegs.m_noredo.undo;
	} else {
		ut_ad(!trx->read_only);
		ut_ad(trx->id);
		pundo = &trx->rsegs.m_redo.undo;
		rseg = trx->rsegs.m_redo.rseg;
	}

	dberr_t		err;
	buf_block_t*	undo_block = trx_undo_assign_low(trx, rseg, pundo,
							 &err, &mtr);
	trx_undo_t*	undo	= *pundo;
	ut_ad((err == DB_SUCCESS) == (undo_block != NULL));
	if (UNIV_UNLIKELY(undo_block == NULL)) {
err_exit:
		mtr.commit();
		return err;
	}

	ut_ad(undo != NULL);

	do {
		uint16_t offset = !rec
			? trx_undo_page_report_insert(
				undo_block, trx, index, clust_entry, &mtr,
				bulk)
			: trx_undo_page_report_modify(
				undo_block, trx, index, rec, offsets, update,
				cmpl_info, clust_entry, &mtr);

		if (UNIV_UNLIKELY(offset == 0)) {
			const uint16_t first_free = mach_read_from_2(
				TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE
				+ undo_block->page.frame);
			memset(undo_block->page.frame + first_free, 0,
			       (srv_page_size - FIL_PAGE_DATA_END)
			       - first_free);

			if (first_free
			    == TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE) {
				/* The record did not fit on an empty
				undo page. Discard the freshly allocated
				page and return an error. */

				/* When we remove a page from an undo
				log, this is analogous to a
				pessimistic insert in a B-tree, and we
				must reserve the counterpart of the
				tree latch, which is the rseg
				mutex. We must commit the mini-transaction
				first, because it may be holding lower-level
				latches, such as SYNC_FSP_PAGE. */

				mtr.commit();
				mtr.start();
				if (is_temp) {
					mtr.set_log_mode(MTR_LOG_NO_REDO);
				}

				rseg->latch.wr_lock(SRW_LOCK_CALL);
				trx_undo_free_last_page(undo, &mtr);
				rseg->latch.wr_unlock();

				if (m.second) {
					/* We are not going to modify
					this table after all. */
					trx->mod_tables.erase(m.first);
				}

				err = DB_UNDO_RECORD_TOO_BIG;
				goto err_exit;
			} else {
				/* Write log for clearing the unused
				tail of the undo page. It might
				contain some garbage from a previously
				written record, and mtr_t::write()
				will optimize away writes of unchanged
				bytes. Failure to write this caused a
				recovery failure when we avoided
				reading the undo log page from the
				data file and initialized it based on
				redo log records (which included the
				write of the previous garbage). */
				mtr.memset(*undo_block, first_free,
					   srv_page_size - first_free
					   - FIL_PAGE_DATA_END, 0);
			}

			mtr.commit();
		} else {
			/* Success */
			undo->top_page_no = undo_block->page.id().page_no();
			mtr.commit();
			undo->top_offset  = offset;
			undo->top_undo_no = trx->undo_no++;
			undo->guess_block = undo_block;
			ut_ad(!undo->empty());

			if (!is_temp) {
				trx_mod_table_time_t& time = m.first->second;
				ut_ad(time.valid(undo->top_undo_no));

				if (!time.is_versioned()
				    && index->table->versioned_by_id()
				    && (!rec /* INSERT */
					|| (update
					    && update->affects_versioned()))) {
					time.set_versioned(undo->top_undo_no);
				}
			}

			if (!bulk) {
				*roll_ptr = trx_undo_build_roll_ptr(
					!rec, trx_sys.rseg_id(rseg, !is_temp),
					undo->top_page_no, offset);
			}

			return(DB_SUCCESS);
		}

		ut_ad(undo_block->page.id().page_no() == undo->last_page_no);

		/* We have to extend the undo log by one page */

		ut_ad(++loop_count < 2);
		mtr.start();

		if (is_temp) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		}

		undo_block = trx_undo_add_page(undo, &mtr);

		DBUG_EXECUTE_IF("ib_err_ins_undo_page_add_failure",
				undo_block = NULL;);
	} while (UNIV_LIKELY(undo_block != NULL));

	ib_errf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
		DB_OUT_OF_FILE_SPACE,
		//ER_INNODB_UNDO_LOG_FULL,
		"No more space left over in %s tablespace for allocating UNDO"
		" log pages. Please add new data file to the tablespace or"
		" check if filesystem is full or enable auto-extension for"
		" the tablespace",
		undo->rseg->space == fil_system.sys_space
		? "system" : is_temp ? "temporary" : "undo");

	/* Did not succeed: out of space */
	err = DB_OUT_OF_FILE_SPACE;
	goto err_exit;
}

/*============== BUILDING PREVIOUS VERSION OF A RECORD ===============*/

/** Copy an undo record to heap.
@param[in]	roll_ptr	roll pointer to a record that exists
@param[in,out]	heap		memory heap where copied */
static
trx_undo_rec_t*
trx_undo_get_undo_rec_low(
	roll_ptr_t		roll_ptr,
	mem_heap_t*		heap)
{
	trx_undo_rec_t*	undo_rec;
	ulint		rseg_id;
	uint32_t	page_no;
	uint16_t	offset;
	bool		is_insert;
	mtr_t		mtr;

	trx_undo_decode_roll_ptr(roll_ptr, &is_insert, &rseg_id, &page_no,
				 &offset);
	ut_ad(page_no > FSP_FIRST_INODE_PAGE_NO);
	ut_ad(offset >= TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
	trx_rseg_t* rseg = &trx_sys.rseg_array[rseg_id];
	ut_ad(rseg->is_persistent());

	mtr.start();

	buf_block_t *undo_page = trx_undo_page_get_s_latched(
		page_id_t(rseg->space->id, page_no), &mtr);

	undo_rec = trx_undo_rec_copy(
		undo_page->page.frame + offset, heap);

	mtr.commit();

	return(undo_rec);
}

/** Copy an undo record to heap.
@param[in]	roll_ptr	roll pointer to record
@param[in,out]	heap		memory heap where copied
@param[in]	trx_id		id of the trx that generated
				the roll pointer: it points to an
				undo log of this transaction
@param[in]	name		table name
@param[out]	undo_rec	own: copy of the record
@retval true if the undo log has been
truncated and we cannot fetch the old version
@retval false if the undo log record is available
NOTE: the caller must have latches on the clustered index page. */
static MY_ATTRIBUTE((warn_unused_result))
bool
trx_undo_get_undo_rec(
	roll_ptr_t		roll_ptr,
	mem_heap_t*		heap,
	trx_id_t		trx_id,
	const table_name_t&	name,
	trx_undo_rec_t**	undo_rec)
{
	purge_sys.latch.rd_lock(SRW_LOCK_CALL);

	bool missing_history = purge_sys.changes_visible(trx_id, name);
	if (!missing_history) {
		*undo_rec = trx_undo_get_undo_rec_low(roll_ptr, heap);
	}

	purge_sys.latch.rd_unlock();

	return(missing_history);
}

#ifdef UNIV_DEBUG
#define ATTRIB_USED_ONLY_IN_DEBUG
#else /* UNIV_DEBUG */
#define ATTRIB_USED_ONLY_IN_DEBUG	MY_ATTRIBUTE((unused))
#endif /* UNIV_DEBUG */

/** Build a previous version of a clustered index record. The caller
must hold a latch on the index page of the clustered index record.
@param	index_rec	clustered index record in the index tree
@param	index_mtr	mtr which contains the latch to index_rec page
			and purge_view
@param	rec		version of a clustered index record
@param	index		clustered index
@param	offsets		rec_get_offsets(rec, index)
@param	heap		memory heap from which the memory needed is
			allocated
@param	old_vers	previous version or NULL if rec is the
			first inserted version, or if history data
			has been deleted (an error), or if the purge
			could have removed the version
			though it has not yet done so
@param	v_heap		memory heap used to create vrow
			dtuple if it is not yet created. This heap
			diffs from "heap" above in that it could be
			prebuilt->old_vers_heap for selection
@param	v_row		virtual column info, if any
@param	v_status	status determine if it is going into this
			function by purge thread or not.
			And if we read "after image" of undo log
@param	undo_block	undo log block which was cached during
			online dml apply or nullptr
@retval true if previous version was built, or if it was an insert
or the table has been rebuilt
@retval false if the previous version is earlier than purge_view,
or being purged, which means that it may have been removed */
bool
trx_undo_prev_version_build(
	const rec_t	*index_rec ATTRIB_USED_ONLY_IN_DEBUG,
	mtr_t		*index_mtr ATTRIB_USED_ONLY_IN_DEBUG,
	const rec_t 	*rec,
	dict_index_t	*index,
	rec_offs	*offsets,
	mem_heap_t	*heap,
	rec_t		**old_vers,
	mem_heap_t	*v_heap,
	dtuple_t	**vrow,
	ulint		v_status)
{
	trx_undo_rec_t*	undo_rec	= NULL;
	dtuple_t*	entry;
	trx_id_t	rec_trx_id;
	ulint		type;
	undo_no_t	undo_no;
	table_id_t	table_id;
	trx_id_t	trx_id;
	roll_ptr_t	roll_ptr;
	upd_t*		update;
	byte*		ptr;
	byte		info_bits;
	ulint		cmpl_info;
	bool		dummy_extern;
	byte*		buf;

	ut_ad(!index->table->is_temporary());
	ut_ad(index_mtr->memo_contains_page_flagged(index_rec,
						    MTR_MEMO_PAGE_S_FIX
						    | MTR_MEMO_PAGE_X_FIX));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_a(index->is_primary());

	roll_ptr = row_get_rec_roll_ptr(rec, index, offsets);

	*old_vers = NULL;

	if (trx_undo_roll_ptr_is_insert(roll_ptr)) {
		/* The record rec is the first inserted version */
		return(true);
	}

	rec_trx_id = row_get_rec_trx_id(rec, index, offsets);

	ut_ad(!index->table->skip_alter_undo);

	if (trx_undo_get_undo_rec(
		    roll_ptr, heap, rec_trx_id, index->table->name,
		    &undo_rec)) {
		if (v_status & TRX_UNDO_PREV_IN_PURGE) {
			/* We are fetching the record being purged */
			undo_rec = trx_undo_get_undo_rec_low(roll_ptr, heap);
		} else {
			/* The undo record may already have been purged,
			during purge or semi-consistent read. */
			return(false);
		}
	}

	ptr = trx_undo_rec_get_pars(undo_rec, &type, &cmpl_info,
				    &dummy_extern, &undo_no, &table_id);

	if (table_id != index->table->id) {
		/* The table should have been rebuilt, but purge has
		not yet removed the undo log records for the
		now-dropped old table (table_id). */
		return(true);
	}

	ptr = trx_undo_update_rec_get_sys_cols(ptr, &trx_id, &roll_ptr,
					       &info_bits);

	/* (a) If a clustered index record version is such that the
	trx id stamp in it is bigger than purge_sys.view, then the
	BLOBs in that version are known to exist (the purge has not
	progressed that far);

	(b) if the version is the first version such that trx id in it
	is less than purge_sys.view, and it is not delete-marked,
	then the BLOBs in that version are known to exist (the purge
	cannot have purged the BLOBs referenced by that version
	yet).

	This function does not fetch any BLOBs.  The callers might, by
	possibly invoking row_ext_create() via row_build().  However,
	they should have all needed information in the *old_vers
	returned by this function.  This is because *old_vers is based
	on the transaction undo log records.  The function
	trx_undo_page_fetch_ext() will write BLOB prefixes to the
	transaction undo log that are at least as long as the longest
	possible column prefix in a secondary index.  Thus, secondary
	index entries for *old_vers can be constructed without
	dereferencing any BLOB pointers. */

	ptr = trx_undo_rec_skip_row_ref(ptr, index);

	ptr = trx_undo_update_rec_get_update(ptr, index, type, trx_id,
					     roll_ptr, info_bits,
					     heap, &update);
	ut_a(ptr);

	if (row_upd_changes_field_size_or_external(index, offsets, update)) {
		/* We should confirm the existence of disowned external data,
		if the previous version record is delete marked. If the trx_id
		of the previous record is seen by purge view, we should treat
		it as missing history, because the disowned external data
		might be purged already.

		The inherited external data (BLOBs) can be freed (purged)
		after trx_id was committed, provided that no view was started
		before trx_id. If the purge view can see the committed
		delete-marked record by trx_id, no transactions need to access
		the BLOB. */

		/* the row_upd_changes_disowned_external(update) call could be
		omitted, but the synchronization on purge_sys.latch is likely
		more expensive. */

		if ((update->info_bits & REC_INFO_DELETED_FLAG)
		    && row_upd_changes_disowned_external(update)) {
			purge_sys.latch.rd_lock(SRW_LOCK_CALL);

			bool missing_extern = purge_sys.changes_visible(
				trx_id,	index->table->name);

			purge_sys.latch.rd_unlock();

			if (missing_extern) {
				/* treat as a fresh insert, not to
				cause assertion error at the caller. */
				return(true);
			}
		}

		/* We have to set the appropriate extern storage bits in the
		old version of the record: the extern bits in rec for those
		fields that update does NOT update, as well as the bits for
		those fields that update updates to become externally stored
		fields. Store the info: */

		entry = row_rec_to_index_entry(rec, index, offsets, heap);
		/* The page containing the clustered index record
		corresponding to entry is latched in mtr.  Thus the
		following call is safe. */
		if (!row_upd_index_replace_new_col_vals(entry, *index, update,
							heap)) {
			ut_a(v_status & TRX_UNDO_PREV_IN_PURGE);
			return false;
		}

		/* Get number of externally stored columns in updated record */
		const ulint n_ext = index->is_primary()
			? dtuple_get_n_ext(entry) : 0;

		buf = static_cast<byte*>(mem_heap_alloc(
			heap, rec_get_converted_size(index, entry, n_ext)));

		*old_vers = rec_convert_dtuple_to_rec(buf, index,
						      entry, n_ext);
	} else {
		buf = static_cast<byte*>(mem_heap_alloc(
			heap, rec_offs_size(offsets)));

		*old_vers = rec_copy(buf, rec, offsets);
		rec_offs_make_valid(*old_vers, index, true, offsets);
		rec_set_bit_field_1(*old_vers, update->info_bits,
				    rec_offs_comp(offsets)
				    ? REC_NEW_INFO_BITS : REC_OLD_INFO_BITS,
				    REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
		for (ulint i = 0; i < update->n_fields; i++) {
			const upd_field_t* uf = upd_get_nth_field(update, i);
			if (upd_fld_is_virtual_col(uf)) {
				/* There are no virtual columns in
				a clustered index record. */
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
				ulint l = rec_get_1byte_offs_flag(*old_vers)
					? (n + 1) : (n + 1) * 2;
				byte* b = *old_vers - REC_N_OLD_EXTRA_BYTES
					- l;
				*b= byte(*b | REC_1BYTE_SQL_NULL_MASK);
				compile_time_assert(REC_1BYTE_SQL_NULL_MASK << 8
						    == REC_2BYTE_SQL_NULL_MASK);
				continue;
			}

			ulint len;
			memcpy(rec_get_nth_field(*old_vers, offsets, n, &len),
			       uf->new_val.data, uf->new_val.len);
			if (UNIV_UNLIKELY(len != uf->new_val.len)) {
				ut_ad(len == UNIV_SQL_NULL);
				ut_ad(!rec_offs_comp(offsets));
				ut_ad(uf->new_val.len
				      == rec_get_nth_field_size(rec, n));
				ulint l = rec_get_1byte_offs_flag(*old_vers)
					? (n + 1) : (n + 1) * 2;
				*(*old_vers - REC_N_OLD_EXTRA_BYTES - l)
					&= byte(~REC_1BYTE_SQL_NULL_MASK);
			}
		}
	}

	/* Set the old value (which is the after image of an update) in the
	update vector to dtuple vrow */
	if (v_status & TRX_UNDO_GET_OLD_V_VALUE) {
		row_upd_replace_vcol((dtuple_t*)*vrow, index->table, update,
				     false, NULL, NULL);
	}

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	rec_offs offsets_dbg[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_dbg);
	ut_a(!rec_offs_any_null_extern(
		*old_vers, rec_get_offsets(*old_vers, index, offsets_dbg,
					   index->n_core_fields,
					   ULINT_UNDEFINED, &heap)));
#endif // defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG

	if (vrow && !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
		if (!(*vrow)) {
			*vrow = dtuple_create_with_vcol(
				v_heap ? v_heap : heap,
				dict_table_get_n_cols(index->table),
				dict_table_get_n_v_cols(index->table));
			dtuple_init_v_fld(*vrow);
		}

		ut_ad(index->table->n_v_cols);
		trx_undo_read_v_cols(index->table, ptr, *vrow,
				     v_status & TRX_UNDO_PREV_IN_PURGE);
	}

	return(true);
}

/** Read virtual column value from undo log
@param[in]	table		the table
@param[in]	ptr		undo log pointer
@param[in,out]	row		the dtuple to fill
@param[in]	in_purge	whether this is called by purge */
void
trx_undo_read_v_cols(
	const dict_table_t*	table,
	const byte*		ptr,
	dtuple_t*		row,
	bool			in_purge)
{
	const byte*     end_ptr;
	bool		first_v_col = true;
	bool		is_undo_log = true;

	end_ptr = ptr + mach_read_from_2(ptr);
	ptr += 2;
	while (ptr < end_ptr) {
		dfield_t* dfield;
		const byte* field;
		uint32_t field_no, len, orig_len;

		field_no = mach_read_next_compressed(
				const_cast<const byte**>(&ptr));

		const bool is_virtual = (field_no >= REC_MAX_N_FIELDS);

		if (is_virtual) {
			ptr = trx_undo_read_v_idx(
				table, ptr, first_v_col, &is_undo_log,
				&field_no);
			first_v_col = false;
		}

		ptr = trx_undo_rec_get_col_val(
			ptr, &field, &len, &orig_len);

		/* The virtual column is no longer indexed or does not exist.
		This needs to put after trx_undo_rec_get_col_val() so the
		undo ptr advances */
		if (field_no == FIL_NULL) {
			ut_ad(is_virtual);
			continue;
		}

		if (is_virtual) {
			dict_v_col_t*	vcol = dict_table_get_nth_v_col(
				table, field_no);

			dfield = dtuple_get_nth_v_field(row, vcol->v_pos);

			if (!in_purge
			    || dfield_get_type(dfield)->mtype == DATA_MISSING) {
				dict_col_copy_type(
					&vcol->m_col,
					dfield_get_type(dfield));
				dfield_set_data(dfield, field, len);
			}
		}
	}

	ut_ad(ptr == end_ptr);
}
