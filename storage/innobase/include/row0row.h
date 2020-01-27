/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2020, MariaDB Corporation.

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
@file include/row0row.h
General row routines

Created 4/20/1996 Heikki Tuuri
*******************************************************/

#ifndef row0row_h
#define row0row_h

#include "que0types.h"
#include "ibuf0ibuf.h"
#include "trx0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "row0types.h"
#include "btr0types.h"

/*********************************************************************//**
Gets the offset of the DB_TRX_ID field, in bytes relative to the origin of
a clustered index record.
@return offset of DATA_TRX_ID */
UNIV_INLINE
ulint
row_get_trx_id_offset(
/*==================*/
	const dict_index_t*	index,	/*!< in: clustered index */
	const offset_t*		offsets)/*!< in: record offsets */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Reads the trx id field from a clustered index record.
@return value of the field */
UNIV_INLINE
trx_id_t
row_get_rec_trx_id(
/*===============*/
	const rec_t*		rec,	/*!< in: record */
	const dict_index_t*	index,	/*!< in: clustered index */
	const offset_t*		offsets)/*!< in: rec_get_offsets(rec, index) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Reads the roll pointer field from a clustered index record.
@return value of the field */
UNIV_INLINE
roll_ptr_t
row_get_rec_roll_ptr(
/*=================*/
	const rec_t*		rec,	/*!< in: record */
	const dict_index_t*	index,	/*!< in: clustered index */
	const offset_t*		offsets)/*!< in: rec_get_offsets(rec, index) */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/* Flags for row build type. */
#define ROW_BUILD_NORMAL	0	/*!< build index row */
#define ROW_BUILD_FOR_PURGE	1	/*!< build row for purge. */
#define ROW_BUILD_FOR_UNDO	2	/*!< build row for undo. */
#define ROW_BUILD_FOR_INSERT	3	/*!< build row for insert. */

/*****************************************************************//**
When an insert or purge to a table is performed, this function builds
the entry to be inserted into or purged from an index on the table.
@return index entry which should be inserted or purged
@retval NULL if the externally stored columns in the clustered index record
are unavailable and ext != NULL, or row is missing some needed columns. */
dtuple_t*
row_build_index_entry_low(
/*======================*/
	const dtuple_t*		row,	/*!< in: row which should be
					inserted or purged */
	const row_ext_t*	ext,	/*!< in: externally stored column
					prefixes, or NULL */
	const dict_index_t*	index,	/*!< in: index on the table */
	mem_heap_t*		heap,	/*!< in,out: memory heap from which
					the memory for the index entry
					is allocated */
	ulint			flag)	/*!< in: ROW_BUILD_NORMAL,
					ROW_BUILD_FOR_PURGE
                                        or ROW_BUILD_FOR_UNDO */
	MY_ATTRIBUTE((warn_unused_result, nonnull(1,3,4)));
/*****************************************************************//**
When an insert or purge to a table is performed, this function builds
the entry to be inserted into or purged from an index on the table.
@return index entry which should be inserted or purged, or NULL if the
externally stored columns in the clustered index record are
unavailable and ext != NULL */
UNIV_INLINE
dtuple_t*
row_build_index_entry(
/*==================*/
	const dtuple_t*		row,	/*!< in: row which should be
					inserted or purged */
	const row_ext_t*	ext,	/*!< in: externally stored column
					prefixes, or NULL */
	const dict_index_t*	index,	/*!< in: index on the table */
	mem_heap_t*		heap)	/*!< in,out: memory heap from which
					the memory for the index entry
					is allocated */
	MY_ATTRIBUTE((warn_unused_result, nonnull(1,3,4)));
/*******************************************************************//**
An inverse function to row_build_index_entry. Builds a row from a
record in a clustered index.
@return own: row built; see the NOTE below! */
dtuple_t*
row_build(
/*======*/
	ulint			type,	/*!< in: ROW_COPY_POINTERS or
					ROW_COPY_DATA; the latter
					copies also the data fields to
					heap while the first only
					places pointers to data fields
					on the index page, and thus is
					more efficient */
	const dict_index_t*	index,	/*!< in: clustered index */
	const rec_t*		rec,	/*!< in: record in the clustered
					index; NOTE: in the case
					ROW_COPY_POINTERS the data
					fields in the row will point
					directly into this record,
					therefore, the buffer page of
					this record must be at least
					s-latched and the latch held
					as long as the row dtuple is used! */
	const offset_t*		offsets,/*!< in: rec_get_offsets(rec,index)
					or NULL, in which case this function
					will invoke rec_get_offsets() */
	const dict_table_t*	col_table,
					/*!< in: table, to check which
					externally stored columns
					occur in the ordering columns
					of an index, or NULL if
					index->table should be
					consulted instead; the user
					columns in this table should be
					the same columns as in index->table */
	const dtuple_t*		defaults,
					/*!< in: default values of
					added, changed columns, or NULL */
	const ulint*		col_map,/*!< in: mapping of old column
					numbers to new ones, or NULL */
	row_ext_t**		ext,	/*!< out, own: cache of
					externally stored column
					prefixes, or NULL */
	mem_heap_t*		heap);	/*!< in: memory heap from which
					the memory needed is allocated */

/** An inverse function to row_build_index_entry. Builds a row from a
record in a clustered index, with possible indexing on ongoing
addition of new virtual columns.
@param[in]	type		ROW_COPY_POINTERS or ROW_COPY_DATA;
@param[in]	index		clustered index
@param[in]	rec		record in the clustered index
@param[in]	offsets		rec_get_offsets(rec,index) or NULL
@param[in]	col_table	table, to check which
				externally stored columns
				occur in the ordering columns
				of an index, or NULL if
				index->table should be
				consulted instead
@param[in]	defaults	default values of added, changed columns, or NULL
@param[in]	add_v		new virtual columns added
				along with new indexes
@param[in]	col_map		mapping of old column
				numbers to new ones, or NULL
@param[in]	ext		cache of externally stored column
				prefixes, or NULL
@param[in]	heap		memory heap from which
				the memory needed is allocated
@return own: row built */
dtuple_t*
row_build_w_add_vcol(
	ulint			type,
	const dict_index_t*	index,
	const rec_t*		rec,
	const offset_t*		offsets,
	const dict_table_t*	col_table,
	const dtuple_t*		defaults,
	const dict_add_v_col_t*	add_v,
	const ulint*		col_map,
	row_ext_t**		ext,
	mem_heap_t*		heap);

/*******************************************************************//**
Converts an index record to a typed data tuple.
@return index entry built; does not set info_bits, and the data fields
in the entry will point directly to rec */
dtuple_t*
row_rec_to_index_entry_low(
/*=======================*/
	const rec_t*		rec,	/*!< in: record in the index */
	const dict_index_t*	index,	/*!< in: index */
	const offset_t*		offsets,/*!< in: rec_get_offsets(rec, index) */
	mem_heap_t*		heap)	/*!< in: memory heap from which
					the memory needed is allocated */
	MY_ATTRIBUTE((warn_unused_result));
/*******************************************************************//**
Converts an index record to a typed data tuple. NOTE that externally
stored (often big) fields are NOT copied to heap.
@return own: index entry built */
dtuple_t*
row_rec_to_index_entry(
/*===================*/
	const rec_t*		rec,	/*!< in: record in the index */
	const dict_index_t*	index,	/*!< in: index */
	const offset_t*		offsets,/*!< in/out: rec_get_offsets(rec) */
	mem_heap_t*		heap)	/*!< in: memory heap from which
					the memory needed is allocated */
	MY_ATTRIBUTE((warn_unused_result));

/** Convert a metadata record to a data tuple.
@param[in]	rec		metadata record
@param[in]	index		clustered index after instant ALTER TABLE
@param[in]	offsets		rec_get_offsets(rec)
@param[in,out]	heap		memory heap for allocations
@param[in]	info_bits	the info_bits after an update
@param[in]	pad		whether to pad to index->n_fields */
dtuple_t*
row_metadata_to_tuple(
	const rec_t*		rec,
	const dict_index_t*	index,
	const offset_t*		offsets,
	mem_heap_t*		heap,
	ulint			info_bits,
	bool			pad)
	MY_ATTRIBUTE((nonnull,warn_unused_result));

/*******************************************************************//**
Builds from a secondary index record a row reference with which we can
search the clustered index record.
@return own: row reference built; see the NOTE below! */
dtuple_t*
row_build_row_ref(
/*==============*/
	ulint		type,	/*!< in: ROW_COPY_DATA, or ROW_COPY_POINTERS:
				the former copies also the data fields to
				heap, whereas the latter only places pointers
				to data fields on the index page */
	dict_index_t*	index,	/*!< in: secondary index */
	const rec_t*	rec,	/*!< in: record in the index;
				NOTE: in the case ROW_COPY_POINTERS
				the data fields in the row will point
				directly into this record, therefore,
				the buffer page of this record must be
				at least s-latched and the latch held
				as long as the row reference is used! */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
	MY_ATTRIBUTE((warn_unused_result));
/*******************************************************************//**
Builds from a secondary index record a row reference with which we can
search the clustered index record. */
void
row_build_row_ref_in_tuple(
/*=======================*/
	dtuple_t*		ref,	/*!< in/out: row reference built;
					see the NOTE below! */
	const rec_t*		rec,	/*!< in: record in the index;
					NOTE: the data fields in ref
					will point directly into this
					record, therefore, the buffer
					page of this record must be at
					least s-latched and the latch
					held as long as the row
					reference is used! */
	const dict_index_t*	index,	/*!< in: secondary index */
	offset_t*		offsets)/*!< in: rec_get_offsets(rec, index)
					or NULL */
	MY_ATTRIBUTE((nonnull(1,2,3)));
/*******************************************************************//**
Builds from a secondary index record a row reference with which we can
search the clustered index record. */
UNIV_INLINE
void
row_build_row_ref_fast(
/*===================*/
	dtuple_t*	ref,	/*!< in/out: typed data tuple where the
				reference is built */
	const ulint*	map,	/*!< in: array of field numbers in rec
				telling how ref should be built from
				the fields of rec */
	const rec_t*	rec,	/*!< in: secondary index record;
				must be preserved while ref is used, as we do
				not copy field values to heap */
	const offset_t*	offsets);/*!< in: array returned by rec_get_offsets() */
/***************************************************************//**
Searches the clustered index record for a row, if we have the row
reference.
@return TRUE if found */
ibool
row_search_on_row_ref(
/*==================*/
	btr_pcur_t*		pcur,	/*!< out: persistent cursor, which must
					be closed by the caller */
	ulint			mode,	/*!< in: BTR_MODIFY_LEAF, ... */
	const dict_table_t*	table,	/*!< in: table */
	const dtuple_t*		ref,	/*!< in: row reference */
	mtr_t*			mtr)	/*!< in/out: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*********************************************************************//**
Fetches the clustered index record for a secondary index record. The latches
on the secondary index record are preserved.
@return record or NULL, if no record found */
rec_t*
row_get_clust_rec(
/*==============*/
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF, ... */
	const rec_t*	rec,	/*!< in: record in a secondary index */
	dict_index_t*	index,	/*!< in: secondary index */
	dict_index_t**	clust_index,/*!< out: clustered index */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Parse the integer data from specified data, which could be
DATA_INT, DATA_FLOAT or DATA_DOUBLE. If the value is less than 0
and the type is not unsigned then we reset the value to 0
@param[in]	data		data to read
@param[in]	len		length of data
@param[in]	mtype		mtype of data
@param[in]	unsigned_type	if the data is unsigned
@return the integer value from the data */
inline
ib_uint64_t
row_parse_int(
	const byte*	data,
	ulint		len,
	ulint		mtype,
	bool		unsigned_type);

/** Result of row_search_index_entry */
enum row_search_result {
	ROW_FOUND = 0,		/*!< the record was found */
	ROW_NOT_FOUND,		/*!< record not found */
	ROW_BUFFERED,		/*!< one of BTR_INSERT, BTR_DELETE, or
				BTR_DELETE_MARK was specified, the
				secondary index leaf page was not in
				the buffer pool, and the operation was
				enqueued in the insert/delete buffer */
	ROW_NOT_DELETED_REF	/*!< BTR_DELETE was specified, and
				row_purge_poss_sec() failed */
};

/***************************************************************//**
Searches an index record.
@return whether the record was found or buffered */
enum row_search_result
row_search_index_entry(
/*===================*/
	dict_index_t*	index,	/*!< in: index */
	const dtuple_t*	entry,	/*!< in: index entry */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF, ... */
	btr_pcur_t*	pcur,	/*!< in/out: persistent cursor, which must
				be closed by the caller */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#define ROW_COPY_DATA		1
#define ROW_COPY_POINTERS	2

/* The allowed latching order of index records is the following:
(1) a secondary index record ->
(2) the clustered index record ->
(3) rollback segment data for the clustered index record. */

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) using
"dict_field" and writes the result to "buf".
Not more than "buf_size" bytes are written to "buf".
The result is always NUL-terminated (provided buf_size is positive) and the
number of bytes that were written to "buf" is returned (including the
terminating NUL).
@return number of bytes that were written */
ulint
row_raw_format(
/*===========*/
	const char*		data,		/*!< in: raw data */
	ulint			data_len,	/*!< in: raw data length
						in bytes */
	const dict_field_t*	dict_field,	/*!< in: index field */
	char*			buf,		/*!< out: output buffer */
	ulint			buf_size)	/*!< in: output buffer size
						in bytes */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Prepare to start a mini-transaction to modify an index.
@param[in,out]	mtr		mini-transaction
@param[in,out]	index		possibly secondary index
@param[in]	pessimistic	whether this is a pessimistic operation */
inline
void
row_mtr_start(mtr_t* mtr, dict_index_t* index, bool pessimistic)
{
	mtr->start();

	switch (index->table->space_id) {
	case IBUF_SPACE_ID:
		if (pessimistic
		    && !(index->type & (DICT_UNIQUE | DICT_SPATIAL))) {
			ibuf_free_excess_pages();
		}
		break;
	case SRV_TMP_SPACE_ID:
		mtr->set_log_mode(MTR_LOG_NO_REDO);
		break;
	default:
		index->set_modified(*mtr);
		break;
	}

	log_free_check();
}

#include "row0row.ic"

#endif
