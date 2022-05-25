/*****************************************************************************

Copyright (c) 1996, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

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
@file row/row0row.cc
General row routines

Created 4/20/1996 Heikki Tuuri
*******************************************************/

#include "row0row.h"
#include "data0type.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0ext.h"
#include "row0upd.h"
#include "rem0cmp.h"
#include "ut0mem.h"
#include "gis0geo.h"
#include "row0mysql.h"

/** Build a spatial index key.
@param[in]	index	spatial index
@param[in]	ext	externally stored column prefixes, or NULL
@param[in,out]	dfield	field of the tuple to be copied
@param[in]	dfield2	field of the tuple to copy
@param[in]	flag	ROW_BUILD_NORMAL, ROW_BUILD_FOR_PURGE or
			ROW_BUILD_FOR_UNDO
@param[in,out]	heap	memory heap from which the memory
			of the field entry is allocated.
@retval false if undo log is logged before spatial index creation. */
static bool row_build_spatial_index_key(
	const dict_index_t*	index,
	const row_ext_t*	ext,
	dfield_t*		dfield,
	const dfield_t*		dfield2,
	ulint			flag,
	mem_heap_t*		heap)
{
	if (dfield2->type.mtype == DATA_MISSING) {
		return false;
	}

	double*			mbr;

	dfield_copy(dfield, dfield2);
	dfield->type.prtype |= DATA_GIS_MBR;

	/* Allocate memory for mbr field */
	mbr = static_cast<double*>(mem_heap_alloc(heap, DATA_MBR_LEN));

	/* Set mbr field data. */
	dfield_set_data(dfield, mbr, DATA_MBR_LEN);

	const fil_space_t* space = index->table->space;

	if (UNIV_UNLIKELY(!dfield2->data || !space)) {
		/* FIXME: dfield contains uninitialized data,
		but row_build_index_entry_low() will not return NULL.
		This bug is inherited from MySQL 5.7.5
		commit b66ad511b61fffe75c58d0a607cdb837c6e6c821. */
		return true;
	}

	const byte* dptr = NULL;
	ulint	dlen = 0;
	ulint	flen = 0;
	double	tmp_mbr[SPDIMS * 2];
	mem_heap_t*	temp_heap = NULL;

	if (!dfield_is_ext(dfield2)) {
		dptr = static_cast<const byte*>(dfield_get_data(dfield2));
		dlen = dfield_get_len(dfield2);
		ut_ad(dptr != &data_error);
		goto write_mbr;
	}

	if (flag == ROW_BUILD_FOR_PURGE) {
		const byte* ptr = static_cast<const byte*>(
			dfield_get_data(dfield2));

		switch (dfield_get_spatial_status(dfield2)) {
		case SPATIAL_ONLY:
			ut_ad(dfield_get_len(dfield2) == DATA_MBR_LEN);
			break;

		case SPATIAL_MIXED:
			ptr += dfield_get_len(dfield2);
			break;

		case SPATIAL_UNKNOWN:
			ut_ad(0);
			/* fall through */
		case SPATIAL_NONE:
			/* Undo record is logged before
			spatial index is created.*/
			return false;
		}

		memcpy(mbr, ptr, DATA_MBR_LEN);
		return true;
	}

	if (flag == ROW_BUILD_FOR_UNDO
	    && dict_table_has_atomic_blobs(index->table)) {
		/* For ROW_FORMAT=DYNAMIC or COMPRESSED, a prefix of
		off-page records is stored in the undo log record (for
		any column prefix indexes). For SPATIAL INDEX, we
		must ignore this prefix. The full column value is
		stored in the BLOB.  For non-spatial index, we would
		have already fetched a necessary prefix of the BLOB,
		available in the "ext" parameter.

		Here, for SPATIAL INDEX, we are fetching the full
		column, which is potentially wasting a lot of I/O,
		memory, and possibly involving a concurrency problem,
		similar to ones that existed before the introduction
		of row_ext_t.

		MDEV-11657 FIXME: write the MBR directly to the undo
		log record, and avoid recomputing it here! */
		flen = BTR_EXTERN_FIELD_REF_SIZE;
		ut_ad(dfield_get_len(dfield2) >= BTR_EXTERN_FIELD_REF_SIZE);
		dptr = static_cast<const byte*>(dfield_get_data(dfield2))
			+ dfield_get_len(dfield2)
			- BTR_EXTERN_FIELD_REF_SIZE;
	} else {
		flen = dfield_get_len(dfield2);
		dptr = static_cast<const byte*>(dfield_get_data(dfield2));
	}

	temp_heap = mem_heap_create(1000);

	dptr = btr_copy_externally_stored_field(
		&dlen, dptr, ext ? ext->zip_size : space->zip_size(),
		flen, temp_heap);

write_mbr:
	if (dlen <= GEO_DATA_HEADER_SIZE) {
		for (uint i = 0; i < SPDIMS; i += 2) {
			tmp_mbr[i] = DBL_MAX;
			tmp_mbr[i + 1] = -DBL_MAX;
		}
	} else {
		rtree_mbr_from_wkb(dptr + GEO_DATA_HEADER_SIZE,
				   uint(dlen - GEO_DATA_HEADER_SIZE),
				   SPDIMS, tmp_mbr);
	}

	dfield_write_mbr(dfield, tmp_mbr);
	if (temp_heap) {
		mem_heap_free(temp_heap);
	}

	return true;
}

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
{
	dtuple_t*	entry;
	ulint		entry_len;
	ulint		i = 0;
	ulint		num_v = 0;

	entry_len = dict_index_get_n_fields(index);

	if (flag == ROW_BUILD_FOR_INSERT && dict_index_is_clust(index)) {
		num_v = dict_table_get_n_v_cols(index->table);
		entry = dtuple_create_with_vcol(heap, entry_len, num_v);
	} else {
		entry = dtuple_create(heap, entry_len);
	}

	if (dict_index_is_ibuf(index)) {
		dtuple_set_n_fields_cmp(entry, entry_len);
		/* There may only be externally stored columns
		in a clustered index B-tree of a user table. */
		ut_a(!ext);
	} else {
		dtuple_set_n_fields_cmp(
			entry, dict_index_get_n_unique_in_tree(index));
		if (dict_index_is_spatial(index)) {
			/* Set the MBR field */
			if (!row_build_spatial_index_key(
				    index, ext,
				    dtuple_get_nth_field(entry, 0),
				    dtuple_get_nth_field(
					    row,
					    dict_index_get_nth_field(index, i)
					    ->col->ind), flag, heap)) {
				return NULL;
			}

			i = 1;
		}
	}

	for (; i < entry_len; i++) {
		const dict_field_t& f = index->fields[i];
		dfield_t* dfield = dtuple_get_nth_field(entry, i);

		if (f.col->is_dropped()) {
			ut_ad(index->is_primary());
			ut_ad(index->is_instant());
			ut_ad(!f.col->is_virtual());
			dict_col_copy_type(f.col, &dfield->type);
			if (f.col->is_nullable()) {
				dfield_set_null(dfield);
			} else {
				dfield_set_data(dfield, field_ref_zero,
						f.fixed_len);
			}
			continue;
		}

		const dfield_t* dfield2;

		if (f.col->is_virtual()) {
			const dict_v_col_t* v_col
				= reinterpret_cast<const dict_v_col_t*>(f.col);

			ut_ad(v_col->v_pos < dtuple_get_n_v_fields(row));
			dfield2 = dtuple_get_nth_v_field(row, v_col->v_pos);

			ut_ad(dfield_is_null(dfield2) ||
			      dfield_get_len(dfield2) == 0 || dfield2->data);
			ut_ad(!dfield_is_ext(dfield2));
			if (UNIV_UNLIKELY(dfield2->type.mtype
					  == DATA_MISSING)) {
				ut_ad(flag == ROW_BUILD_FOR_PURGE);
				return(NULL);
			}
		} else {
			dfield2 = dtuple_get_nth_field(row, f.col->ind);
			if (UNIV_UNLIKELY(dfield2->type.mtype
					  == DATA_MISSING)) {
				/* The field has not been initialized in
				the row. This should be from
				trx_undo_rec_get_partial_row(). */
				return(NULL);
			}

			ut_ad(!(dfield2->type.prtype & DATA_VIRTUAL));
		}

		compile_time_assert(DATA_MISSING == 0);

		*dfield = *dfield2;

		if (dfield_is_null(dfield)) {
			continue;
		}

		ut_ad(!(index->type & DICT_FTS));

		ulint len = dfield_get_len(dfield);

		if (f.prefix_len == 0
		    && (!dfield_is_ext(dfield)
			|| dict_index_is_clust(index))) {
			/* The *dfield = *dfield2 above suffices for
			columns that are stored in-page, or for
			clustered index record columns that are not
			part of a column prefix in the PRIMARY KEY. */
			continue;
		}

		/* If the column is stored externally (off-page) in
		the clustered index, it must be an ordering field in
		the secondary index. If !atomic_blobs, the only way
		we may have a secondary index pointing to a clustered
		index record with an off-page column is when it is a
		column prefix index. If atomic_blobs, also fully
		indexed long columns may be stored off-page. */
		ut_ad(f.col->ord_part);

		if (ext && !f.col->is_virtual()) {
			/* See if the column is stored externally. */
			const byte*	buf = row_ext_lookup(ext, f.col->ind,
							     &len);
			if (UNIV_LIKELY_NULL(buf)) {
				if (UNIV_UNLIKELY(buf == field_ref_zero)) {
					return(NULL);
				}
				dfield_set_data(dfield, buf, len);
			}

			if (f.prefix_len == 0) {
				/* If ROW_FORMAT=DYNAMIC or
				ROW_FORMAT=COMPRESSED, we can have a
				secondary index on an entire column
				that is stored off-page in the
				clustered index. As this is not a
				prefix index (prefix_len == 0),
				include the entire off-page column in
				the secondary index record. */
				continue;
			}
		} else if (dfield_is_ext(dfield)) {
			/* This table is either in
			(ROW_FORMAT=REDUNDANT or ROW_FORMAT=COMPACT)
			or a purge record where the ordered part of
			the field is not external.
			In ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT,
			the maximum column prefix
			index length is 767 bytes, and the clustered
			index record contains a 768-byte prefix of
			each off-page column. */
			ut_a(len >= BTR_EXTERN_FIELD_REF_SIZE);
			len -= BTR_EXTERN_FIELD_REF_SIZE;
			dfield_set_len(dfield, len);
		}

		/* If a column prefix index, take only the prefix. */
		if (f.prefix_len) {
			len = dtype_get_at_most_n_mbchars(
				f.col->prtype,
				f.col->mbminlen, f.col->mbmaxlen,
				f.prefix_len, len,
				static_cast<char*>(dfield_get_data(dfield)));
			dfield_set_len(dfield, len);
		}
	}

	for (i = num_v; i--; ) {
		ut_ad(index->is_primary());
		ut_ad(flag == ROW_BUILD_FOR_INSERT);
		dfield_t* dfield = dtuple_get_nth_v_field(entry, i);
		const dict_v_col_t* v_col = dict_table_get_nth_v_col(
			index->table, i);
		ut_ad(!v_col->m_col.is_dropped());
		ut_ad(v_col->v_pos < dtuple_get_n_v_fields(row));
		const dfield_t* dfield2 = dtuple_get_nth_v_field(
			row, v_col->v_pos);
		ut_ad(dfield_is_null(dfield2) ||
		      dfield_get_len(dfield2) == 0 || dfield2->data);
		ut_ad(dfield2->type.mtype != DATA_MISSING);
		*dfield = *dfield2;
	}

	return entry;
}

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
@param[in]	defaults	default values of added/changed columns, or NULL
@param[in]	add_v		new virtual columns added
				along with new indexes
@param[in]	col_map		mapping of old column
				numbers to new ones, or NULL
@param[in]	ext		cache of externally stored column
				prefixes, or NULL
@param[in]	heap		memory heap from which
				the memory needed is allocated
@return own: row built; */
static inline
dtuple_t*
row_build_low(
	ulint			type,
	const dict_index_t*	index,
	const rec_t*		rec,
	const rec_offs*		offsets,
	const dict_table_t*	col_table,
	const dtuple_t*		defaults,
	const dict_add_v_col_t*	add_v,
	const ulint*		col_map,
	row_ext_t**		ext,
	mem_heap_t*		heap)
{
	const byte*		copy;
	dtuple_t*		row;
	ulint			n_ext_cols;
	ulint*			ext_cols	= NULL; /* remove warning */
	ulint			len;
	byte*			buf;
	ulint			j;
	mem_heap_t*		tmp_heap	= NULL;
	rec_offs		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_);

	ut_ad(index != NULL);
	ut_ad(rec != NULL);
	ut_ad(heap != NULL);
	ut_ad(dict_index_is_clust(index));
	ut_ad(!col_map || col_table);

	if (!offsets) {
		offsets = rec_get_offsets(rec, index, offsets_,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &tmp_heap);
	} else {
		ut_ad(rec_offs_validate(rec, index, offsets));
	}

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	/* Some blob refs can be NULL during crash recovery before
	trx_rollback_active() has completed execution, or when a concurrently
	executing insert or update has committed the B-tree mini-transaction
	but has not yet managed to restore the cursor position for writing
	the big_rec. Note that the mini-transaction can be committed multiple
	times, and the cursor restore can happen multiple times for single
	insert or update statement.  */
	ut_a(!rec_offs_any_null_extern(rec, offsets)
	     || trx_sys.is_registered(current_trx(),
				      row_get_rec_trx_id(rec, index,
							 offsets)));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

	if (type != ROW_COPY_POINTERS) {
		/* Take a copy of rec to heap */
		buf = static_cast<byte*>(
			mem_heap_alloc(heap, rec_offs_size(offsets)));

		copy = rec_copy(buf, rec, offsets);
	} else {
		copy = rec;
	}

	n_ext_cols = rec_offs_n_extern(offsets);
	if (n_ext_cols) {
		ext_cols = static_cast<ulint*>(
			mem_heap_alloc(heap, n_ext_cols * sizeof *ext_cols));
	}

	/* Avoid a debug assertion in rec_offs_validate(). */
	rec_offs_make_valid(copy, index, true, const_cast<rec_offs*>(offsets));

	if (!col_table) {
		ut_ad(!col_map);
		ut_ad(!defaults);
		col_table = index->table;
	}

	if (defaults) {
		ut_ad(col_map);
		row = dtuple_copy(defaults, heap);
		/* dict_table_copy_types() would set the fields to NULL */
		for (ulint i = 0; i < dict_table_get_n_cols(col_table); i++) {
			dict_col_copy_type(
				dict_table_get_nth_col(col_table, i),
				dfield_get_type(dtuple_get_nth_field(row, i)));
		}
	} else if (add_v != NULL) {
		row = dtuple_create_with_vcol(
			heap, dict_table_get_n_cols(col_table),
			dict_table_get_n_v_cols(col_table) + add_v->n_v_col);
		dict_table_copy_types(row, col_table);

		for (ulint i = 0; i < add_v->n_v_col; i++) {
			dict_col_copy_type(
				&add_v->v_col[i].m_col,
				dfield_get_type(dtuple_get_nth_v_field(
					row, i + col_table->n_v_def)));
		}
	} else {
		row = dtuple_create_with_vcol(
			heap, dict_table_get_n_cols(col_table),
			dict_table_get_n_v_cols(col_table));
		dict_table_copy_types(row, col_table);
	}

	dtuple_set_info_bits(row, rec_get_info_bits(
				     copy, rec_offs_comp(offsets)));

	j = 0;

	const dict_field_t* ind_field = index->fields;

	for (ulint i = 0; i < rec_offs_n_fields(offsets); i++) {
		if (i == index->first_user_field()
		    && rec_is_alter_metadata(rec, *index)) {
			ut_ad(rec_offs_nth_extern(offsets, i));
			ut_d(ulint len);
			ut_d(rec_get_nth_field_offs(offsets, i, &len));
			ut_ad(len == FIELD_REF_SIZE);
			continue;
		}

		if (UNIV_UNLIKELY(ind_field
				  >= &index->fields[index->n_fields])) {
			ut_ad(rec_is_metadata(rec, *index));
			continue;
		}

		const dict_col_t* col = dict_field_get_col(ind_field);

		if ((ind_field++)->prefix_len) {
			/* Column prefixes can only occur in key
			fields, which cannot be stored externally. For
			a column prefix, there should also be the full
			field in the clustered index tuple. The row
			tuple comprises full fields, not prefixes. */
			ut_ad(!rec_offs_nth_extern(offsets, i));
			continue;
		}

		if (col->is_dropped()) {
			continue;
		}

		ulint	col_no = dict_col_get_no(col);

		if (col_map) {
			col_no = col_map[col_no];

			if (col_no == ULINT_UNDEFINED) {
				/* dropped column */
				continue;
			}
		}

		dfield_t*	dfield = dtuple_get_nth_field(row, col_no);

		const void*	field = rec_get_nth_field(
			copy, offsets, i, &len);
		if (len == UNIV_SQL_DEFAULT) {
			field = index->instant_field_value(i, &len);
			if (field && type != ROW_COPY_POINTERS) {
				field = mem_heap_dup(heap, field, len);
			}
		}
		dfield_set_data(dfield, field, len);

		if (rec_offs_nth_extern(offsets, i)) {
			dfield_set_ext(dfield);

			col = dict_table_get_nth_col(col_table, col_no);

			if (col->ord_part) {
				/* We will have to fetch prefixes of
				externally stored columns that are
				referenced by column prefixes. */
				ext_cols[j++] = col_no;
			}
		}
	}

	rec_offs_make_valid(rec, index, true, const_cast<rec_offs*>(offsets));

	ut_ad(dtuple_check_typed(row));

	if (!ext) {
		/* REDUNDANT and COMPACT formats store a local
		768-byte prefix of each externally stored
		column. No cache is needed.

		During online table rebuild,
		row_log_table_apply_delete_low()
		may use a cache that was set up by
		row_log_table_delete(). */

	} else if (j) {
		*ext = row_ext_create(j, ext_cols, *index->table, row,
				      heap);
	} else {
		*ext = NULL;
	}

	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	return(row);
}


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
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec,index)
					or NULL, in which case this function
					will invoke rec_get_offsets() */
	const dict_table_t*	col_table,
					/*!< in: table, to check which
					externally stored columns
					occur in the ordering columns
					of an index, or NULL if
					index->table should be
					consulted instead */
	const dtuple_t*		defaults,
					/*!< in: default values of
					added and changed columns, or NULL */
	const ulint*		col_map,/*!< in: mapping of old column
					numbers to new ones, or NULL */
	row_ext_t**		ext,	/*!< out, own: cache of
					externally stored column
					prefixes, or NULL */
	mem_heap_t*		heap)	/*!< in: memory heap from which
					 the memory needed is allocated */
{
	return(row_build_low(type, index, rec, offsets, col_table,
			     defaults, NULL, col_map, ext, heap));
}

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
@return own: row built; */
dtuple_t*
row_build_w_add_vcol(
	ulint			type,
	const dict_index_t*	index,
	const rec_t*		rec,
	const rec_offs*		offsets,
	const dict_table_t*	col_table,
	const dtuple_t*		defaults,
	const dict_add_v_col_t*	add_v,
	const ulint*		col_map,
	row_ext_t**		ext,
	mem_heap_t*		heap)
{
	return(row_build_low(type, index, rec, offsets, col_table,
			     defaults, add_v, col_map, ext, heap));
}

/** Convert an index record to a data tuple.
@tparam metadata whether the index->instant_field_value() needs to be accessed
@tparam mblob 1 if rec_is_alter_metadata();
2 if we want converted metadata corresponding to info_bits
@param[in]	rec		index record
@param[in]	index		index
@param[in]	offsets		rec_get_offsets(rec, index)
@param[out]	n_ext		number of externally stored columns
@param[in,out]	heap		memory heap for allocations
@param[in]	info_bits	(only used if mblob=2)
@param[in]	pad		(only used if mblob=2)
@return index entry built; does not set info_bits, and the data fields
in the entry will point directly to rec */
template<bool metadata, int mblob = 0>
static inline
dtuple_t*
row_rec_to_index_entry_impl(
	const rec_t*		rec,
	const dict_index_t*	index,
	const rec_offs*		offsets,
	mem_heap_t*		heap,
	ulint			info_bits = 0,
	bool			pad = false)
{
	ut_ad(rec != NULL);
	ut_ad(heap != NULL);
	ut_ad(index != NULL);
	ut_ad(!mblob || index->is_primary());
	ut_ad(!mblob || !index->table->is_temporary());
	ut_ad(!mblob || !dict_index_is_spatial(index));
	compile_time_assert(!mblob || metadata);
	compile_time_assert(mblob <= 2);
	/* Because this function may be invoked by row0merge.cc
	on a record whose header is in different format, the check
	rec_offs_validate(rec, index, offsets) must be avoided here. */

	const bool got = mblob == 2 && rec_is_alter_metadata(rec, *index);
	ulint rec_len = rec_offs_n_fields(offsets);
	if (mblob == 2) {
		ut_ad(info_bits == REC_INFO_METADATA_ALTER
		      || info_bits == REC_INFO_METADATA_ADD);
		if (pad) {
			ut_ad(rec_len <= ulint(index->n_fields + got));
			rec_len = ulint(index->n_fields)
				+ (info_bits == REC_INFO_METADATA_ALTER);
		} else if (got) {
			rec_len = std::min(rec_len,
					   ulint(index->n_fields + got));
		} else if (info_bits == REC_INFO_METADATA_ALTER) {
			ut_ad(rec_len <= index->n_fields);
			rec_len++;
		}
	} else {
		ut_ad(info_bits == 0);
		ut_ad(!pad);
	}
	dtuple_t* entry = dtuple_create(heap, rec_len);
	dfield_t* dfield = entry->fields;

	dtuple_set_n_fields_cmp(entry,
				dict_index_get_n_unique_in_tree(index));
	ut_ad(mblob == 2
	      || rec_len == dict_index_get_n_fields(index) + uint(mblob == 1)
	      /* a record for older SYS_INDEXES table
	      (missing merge_threshold column) is acceptable. */
	      || (!index->table->is_temporary()
		  && index->table->id == DICT_INDEXES_ID
		  && rec_len + 1 == dict_index_get_n_fields(index)));

	ulint i;
	for (i = 0; i < (mblob ? index->first_user_field() : rec_len);
	     i++, dfield++) {
		dict_col_copy_type(dict_index_get_nth_col(index, i),
				   &dfield->type);
		if (!mblob
		    && dict_index_is_spatial(index)
		    && DATA_GEOMETRY_MTYPE(dfield->type.mtype)) {
			dfield->type.prtype |= DATA_GIS_MBR;
		}

		ulint len;
		const byte* field = metadata
			? rec_get_nth_cfield(rec, index, offsets, i, &len)
			: rec_get_nth_field(rec, offsets, i, &len);

		dfield_set_data(dfield, field, len);

		if (rec_offs_nth_extern(offsets, i)) {
			dfield_set_ext(dfield);
		}
	}

	if (mblob) {
		ulint len;
		const byte* field;
		ulint j = i;

		if (mblob == 2) {
			const bool want = info_bits == REC_INFO_METADATA_ALTER;
			if (got == want) {
				if (got) {
					goto copy_metadata;
				}
			} else {
				if (want) {
					/* Allocate a placeholder for
					adding metadata in an update. */
					len = FIELD_REF_SIZE;
					field = static_cast<byte*>(
						mem_heap_zalloc(heap, len));
					/* In reality there is one fewer
					field present in the record. */
					rec_len--;
					goto init_metadata;
				}

				/* Skip the undesired metadata blob
				(for example, when rolling back an
				instant ALTER TABLE). */
				i++;
			}
			goto copy_user_fields;
		}
copy_metadata:
		ut_ad(rec_offs_nth_extern(offsets, i));
		field = rec_get_nth_field(rec, offsets, i++, &len);
init_metadata:
		dfield->type.metadata_blob_init();
		ut_ad(len == FIELD_REF_SIZE);
		dfield_set_data(dfield, field, len);
		dfield_set_ext(dfield++);
copy_user_fields:
		for (; i < rec_len; i++, dfield++) {
			dict_col_copy_type(dict_index_get_nth_col(index, j++),
					   &dfield->type);
			if (mblob == 2 && pad
			    && i >= rec_offs_n_fields(offsets)) {
				field = index->instant_field_value(j - 1,
								   &len);
				dfield_set_data(dfield, field, len);
				continue;
			}

			field = rec_get_nth_field(rec, offsets, i, &len);
			dfield_set_data(dfield, field, len);

			if (rec_offs_nth_extern(offsets, i)) {
				dfield_set_ext(dfield);
			}
		}
	}

	if (mblob == 2) {
		ulint n_fields = ulint(dfield - entry->fields);
		ut_ad(entry->n_fields >= n_fields);
		entry->n_fields = n_fields;
	}
	ut_ad(dfield == entry->fields + entry->n_fields);
	ut_ad(dtuple_check_typed(entry));
	return entry;
}

/** Convert an index record to a data tuple.
@param[in]	rec	index record
@param[in]	index	index
@param[in]	offsets	rec_get_offsets(rec, index)
@param[in,out]	heap	memory heap for allocations */
dtuple_t*
row_rec_to_index_entry_low(
	const rec_t*		rec,
	const dict_index_t*	index,
	const rec_offs*		offsets,
	mem_heap_t*		heap)
{
	return row_rec_to_index_entry_impl<false>(rec, index, offsets, heap);
}

/*******************************************************************//**
Converts an index record to a typed data tuple. NOTE that externally
stored (often big) fields are NOT copied to heap.
@return own: index entry built */
dtuple_t*
row_rec_to_index_entry(
/*===================*/
	const rec_t*		rec,	/*!< in: record in the index */
	const dict_index_t*	index,	/*!< in: index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec) */
	mem_heap_t*		heap)	/*!< in: memory heap from which
					the memory needed is allocated */
{
	ut_ad(rec != NULL);
	ut_ad(heap != NULL);
	ut_ad(index != NULL);
	ut_ad(rec_offs_validate(rec, index, offsets));

	/* Take a copy of rec to heap */
	const rec_t* copy_rec = rec_copy(
		static_cast<byte*>(mem_heap_alloc(heap,
						  rec_offs_size(offsets))),
		rec, offsets);

	rec_offs_make_valid(copy_rec, index, true,
			    const_cast<rec_offs*>(offsets));

	dtuple_t* entry = rec_is_alter_metadata(copy_rec, *index)
		? row_rec_to_index_entry_impl<true,1>(
			copy_rec, index, offsets, heap)
		: row_rec_to_index_entry_impl<true>(
			copy_rec, index, offsets, heap);

	rec_offs_make_valid(rec, index, true,
			    const_cast<rec_offs*>(offsets));

	dtuple_set_info_bits(entry,
			     rec_get_info_bits(rec, rec_offs_comp(offsets)));

	return(entry);
}

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
	const rec_offs*		offsets,
	mem_heap_t*		heap,
	ulint			info_bits,
	bool			pad)
{
	ut_ad(info_bits == REC_INFO_METADATA_ALTER
	      || info_bits == REC_INFO_METADATA_ADD);
	ut_ad(rec_is_metadata(rec, *index));
	ut_ad(rec_offs_validate(rec, index, offsets));

	const rec_t* copy_rec = rec_copy(
		static_cast<byte*>(mem_heap_alloc(heap,
						  rec_offs_size(offsets))),
		rec, offsets);

	rec_offs_make_valid(copy_rec, index, true,
			    const_cast<rec_offs*>(offsets));

	dtuple_t* entry = info_bits == REC_INFO_METADATA_ALTER
		|| rec_is_alter_metadata(copy_rec, *index)
		? row_rec_to_index_entry_impl<true,2>(
			copy_rec, index, offsets, heap, info_bits, pad)
		: row_rec_to_index_entry_impl<true>(
			copy_rec, index, offsets, heap);

	rec_offs_make_valid(rec, index, true,
			    const_cast<rec_offs*>(offsets));

	dtuple_set_info_bits(entry, info_bits);
	return entry;
}

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
{
	dict_table_t*	table;
	dict_index_t*	clust_index;
	dfield_t*	dfield;
	dtuple_t*	ref;
	const byte*	field;
	ulint		len;
	ulint		ref_len;
	ulint		pos;
	byte*		buf;
	ulint		clust_col_prefix_len;
	ulint		i;
	mem_heap_t*	tmp_heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(index != NULL);
	ut_ad(rec != NULL);
	ut_ad(heap != NULL);
	ut_ad(!dict_index_is_clust(index));

	offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
				  ULINT_UNDEFINED, &tmp_heap);
	/* Secondary indexes must not contain externally stored columns. */
	ut_ad(!rec_offs_any_extern(offsets));

	if (type == ROW_COPY_DATA) {
		/* Take a copy of rec to heap */

		buf = static_cast<byte*>(
			mem_heap_alloc(heap, rec_offs_size(offsets)));

		rec = rec_copy(buf, rec, offsets);
		rec_offs_make_valid(rec, index, true, offsets);
	}

	table = index->table;

	clust_index = dict_table_get_first_index(table);

	ref_len = dict_index_get_n_unique(clust_index);

	ref = dtuple_create(heap, ref_len);

	dict_index_copy_types(ref, clust_index, ref_len);

	for (i = 0; i < ref_len; i++) {
		dfield = dtuple_get_nth_field(ref, i);

		pos = dict_index_get_nth_field_pos(index, clust_index, i);

		ut_a(pos != ULINT_UNDEFINED);

		ut_ad(!rec_offs_nth_default(offsets, pos));
		field = rec_get_nth_field(rec, offsets, pos, &len);

		dfield_set_data(dfield, field, len);

		/* If the primary key contains a column prefix, then the
		secondary index may contain a longer prefix of the same
		column, or the full column, and we must adjust the length
		accordingly. */

		clust_col_prefix_len = dict_index_get_nth_field(
			clust_index, i)->prefix_len;

		if (clust_col_prefix_len > 0) {
			if (len != UNIV_SQL_NULL) {

				const dtype_t*	dtype
					= dfield_get_type(dfield);

				dfield_set_len(dfield,
					       dtype_get_at_most_n_mbchars(
						       dtype->prtype,
						       dtype->mbminlen,
						       dtype->mbmaxlen,
						       clust_col_prefix_len,
						       len, (char*) field));
			}
		}
	}

	ut_ad(dtuple_check_typed(ref));
	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	return(ref);
}

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
	rec_offs*		offsets)/*!< in: rec_get_offsets(rec, index)
					or NULL */
{
	const dict_index_t*	clust_index;
	dfield_t*		dfield;
	const byte*		field;
	ulint			len;
	ulint			ref_len;
	ulint			pos;
	ulint			clust_col_prefix_len;
	ulint			i;
	mem_heap_t*		heap		= NULL;
	rec_offs		offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_);

	ut_ad(!dict_index_is_clust(index));
	ut_a(index->table);

	clust_index = dict_table_get_first_index(index->table);
	ut_ad(clust_index);

	if (!offsets) {
		offsets = rec_get_offsets(rec, index, offsets_,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &heap);
	} else {
		ut_ad(rec_offs_validate(rec, index, offsets));
	}

	/* Secondary indexes must not contain externally stored columns. */
	ut_ad(!rec_offs_any_extern(offsets));
	ref_len = dict_index_get_n_unique(clust_index);

	ut_ad(ref_len == dtuple_get_n_fields(ref));

	dict_index_copy_types(ref, clust_index, ref_len);

	for (i = 0; i < ref_len; i++) {
		dfield = dtuple_get_nth_field(ref, i);

		pos = dict_index_get_nth_field_pos(index, clust_index, i);

		ut_a(pos != ULINT_UNDEFINED);

		ut_ad(!rec_offs_nth_default(offsets, pos));
		field = rec_get_nth_field(rec, offsets, pos, &len);

		dfield_set_data(dfield, field, len);

		/* If the primary key contains a column prefix, then the
		secondary index may contain a longer prefix of the same
		column, or the full column, and we must adjust the length
		accordingly. */

		clust_col_prefix_len = dict_index_get_nth_field(
			clust_index, i)->prefix_len;

		if (clust_col_prefix_len > 0) {
			if (len != UNIV_SQL_NULL) {

				const dtype_t*	dtype
					= dfield_get_type(dfield);

				dfield_set_len(dfield,
					       dtype_get_at_most_n_mbchars(
						       dtype->prtype,
						       dtype->mbminlen,
						       dtype->mbmaxlen,
						       clust_col_prefix_len,
						       len, (char*) field));
			}
		}
	}

	ut_ad(dtuple_check_typed(ref));
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/***************************************************************//**
Searches the clustered index record for a row, if we have the row reference.
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
{
	ulint		low_match;
	rec_t*		rec;
	dict_index_t*	index;

	ut_ad(dtuple_check_typed(ref));

	index = dict_table_get_first_index(table);

	if (UNIV_UNLIKELY(ref->info_bits != 0)) {
		ut_ad(ref->is_metadata());
		ut_ad(ref->n_fields <= index->n_uniq);
		if (btr_pcur_open_at_index_side(
			    true, index, mode, pcur, true, 0, mtr)
		    != DB_SUCCESS
		    || !btr_pcur_move_to_next_user_rec(pcur, mtr)) {
			return FALSE;
		}
		/* We do not necessarily have index->is_instant() here,
		because we could be executing a rollback of an
		instant ADD COLUMN operation. The function
		rec_is_metadata() asserts index->is_instant();
		we do not want to call it here. */
		return rec_get_info_bits(btr_pcur_get_rec(pcur),
					 dict_table_is_comp(index->table))
			& REC_INFO_MIN_REC_FLAG;
	} else {
		ut_a(ref->n_fields == index->n_uniq);
		if (btr_pcur_open(index, ref, PAGE_CUR_LE, mode, pcur, mtr)
		    != DB_SUCCESS) {
			return FALSE;
		}
	}

	low_match = btr_pcur_get_low_match(pcur);

	rec = btr_pcur_get_rec(pcur);

	if (page_rec_is_infimum(rec)) {

		return(FALSE);
	}

	if (low_match != dtuple_get_n_fields(ref)) {

		return(FALSE);
	}

	return(TRUE);
}

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
{
	mem_heap_t*	heap;
	dtuple_t*	ref;
	dict_table_t*	table;
	btr_pcur_t	pcur;

	ut_ad(!dict_index_is_clust(index));

	table = index->table;

	heap = mem_heap_create(256);

	ref = row_build_row_ref(ROW_COPY_POINTERS, index, rec, heap);

	auto found = row_search_on_row_ref(&pcur, mode, table, ref, mtr);

	mem_heap_free(heap);

	*clust_index = dict_table_get_first_index(table);
	return found ? btr_pcur_get_rec(&pcur) : nullptr;
}

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
{
	ulint	n_fields;
	ulint	low_match;
	rec_t*	rec;

	ut_ad(dtuple_check_typed(entry));

	if (dict_index_is_spatial(index)) {
		ut_ad(mode & (BTR_MODIFY_LEAF | BTR_MODIFY_TREE));
		rtr_pcur_open(index, entry, PAGE_CUR_RTREE_LOCATE,
			      mode, pcur, mtr);
	} else {
		btr_pcur_open(index, entry, PAGE_CUR_LE, mode, pcur, mtr);
	}

	switch (btr_pcur_get_btr_cur(pcur)->flag) {
	case BTR_CUR_DELETE_REF:
		ut_ad(mode & BTR_DELETE);
		ut_ad(!index->is_spatial());
		return(ROW_NOT_DELETED_REF);

	case BTR_CUR_DEL_MARK_IBUF:
	case BTR_CUR_DELETE_IBUF:
	case BTR_CUR_INSERT_TO_IBUF:
		return(ROW_BUFFERED);

	case BTR_CUR_HASH:
	case BTR_CUR_HASH_FAIL:
	case BTR_CUR_BINARY:
		break;
	}

	low_match = btr_pcur_get_low_match(pcur);

	rec = btr_pcur_get_rec(pcur);

	n_fields = dtuple_get_n_fields(entry);

	if (page_rec_is_infimum(rec)) {

		return(ROW_NOT_FOUND);
	} else if (low_match != n_fields) {

		return(ROW_NOT_FOUND);
	}

	return(ROW_FOUND);
}

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_INT using "prtype" and writes the result to "buf".
If the data is in unknown format, then nothing is written to "buf",
0 is returned and "format_in_hex" is set to TRUE, otherwise
"format_in_hex" is left untouched.
Not more than "buf_size" bytes are written to "buf".
The result is always '\0'-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating '\0').
@return number of bytes that were written */
static
ulint
row_raw_format_int(
/*===============*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		prtype,		/*!< in: precise type */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size,	/*!< in: output buffer size
					in bytes */
	ibool*		format_in_hex)	/*!< out: should the data be
					formated in hex */
{
	ulint	ret;

	if (data_len <= sizeof(ib_uint64_t)) {

		ib_uint64_t	value;
		ibool		unsigned_type = prtype & DATA_UNSIGNED;

		value = mach_read_int_type(
			(const byte*) data, data_len, unsigned_type);

		ret = (ulint) snprintf(
			buf, buf_size,
			unsigned_type ? "%llu" : "%lld", (longlong) value)+1;
	} else {

		*format_in_hex = TRUE;
		ret = 0;
	}

	return(ut_min(ret, buf_size));
}

/*******************************************************************//**
Formats the raw data in "data" (in InnoDB on-disk format) that is of
type DATA_(CHAR|VARCHAR|MYSQL|VARMYSQL) using "prtype" and writes the
result to "buf".
If the data is in binary format, then nothing is written to "buf",
0 is returned and "format_in_hex" is set to TRUE, otherwise
"format_in_hex" is left untouched.
Not more than "buf_size" bytes are written to "buf".
The result is always '\0'-terminated (provided buf_size > 0) and the
number of bytes that were written to "buf" is returned (including the
terminating '\0').
@return number of bytes that were written */
static
ulint
row_raw_format_str(
/*===============*/
	const char*	data,		/*!< in: raw data */
	ulint		data_len,	/*!< in: raw data length
					in bytes */
	ulint		prtype,		/*!< in: precise type */
	char*		buf,		/*!< out: output buffer */
	ulint		buf_size,	/*!< in: output buffer size
					in bytes */
	ibool*		format_in_hex)	/*!< out: should the data be
					formated in hex */
{
	ulint	charset_coll;

	if (buf_size == 0) {

		return(0);
	}

	/* we assume system_charset_info is UTF-8 */

	charset_coll = dtype_get_charset_coll(prtype);

	if (UNIV_LIKELY(dtype_is_utf8(prtype))) {

		return(ut_str_sql_format(data, data_len, buf, buf_size));
	}
	/* else */

	if (charset_coll == DATA_MYSQL_BINARY_CHARSET_COLL) {

		*format_in_hex = TRUE;
		return(0);
	}
	/* else */

	return(innobase_raw_format(data, data_len, charset_coll,
					  buf, buf_size));
}

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
{
	ulint	mtype;
	ulint	prtype;
	ulint	ret;
	ibool	format_in_hex;

	ut_ad(data_len != UNIV_SQL_DEFAULT);

	if (buf_size == 0) {

		return(0);
	}

	if (data_len == UNIV_SQL_NULL) {

		ret = snprintf((char*) buf, buf_size, "NULL") + 1;

		return(ut_min(ret, buf_size));
	}

	mtype = dict_field->col->mtype;
	prtype = dict_field->col->prtype;

	format_in_hex = FALSE;

	switch (mtype) {
	case DATA_INT:

		ret = row_raw_format_int(data, data_len, prtype,
					 buf, buf_size, &format_in_hex);
		if (format_in_hex) {

			goto format_in_hex;
		}
		break;
	case DATA_CHAR:
	case DATA_VARCHAR:
	case DATA_MYSQL:
	case DATA_VARMYSQL:

		ret = row_raw_format_str(data, data_len, prtype,
					 buf, buf_size, &format_in_hex);
		if (format_in_hex) {

			goto format_in_hex;
		}

		break;
	/* XXX support more data types */
	default:
	format_in_hex:

		if (UNIV_LIKELY(buf_size > 2)) {

			memcpy(buf, "0x", 2);
			buf += 2;
			buf_size -= 2;
			ret = 2 + ut_raw_to_hex(data, data_len,
						buf, buf_size);
		} else {

			buf[0] = '\0';
			ret = 1;
		}
	}

	return(ret);
}

#ifdef UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT

#ifdef HAVE_UT_CHRONO_T

void
test_row_raw_format_int()
{
	ulint	ret;
	char	buf[128];
	ibool	format_in_hex;
	ulint	i;

#define CALL_AND_TEST(data, data_len, prtype, buf, buf_size,\
		      ret_expected, buf_expected, format_in_hex_expected)\
	do {\
		ibool	ok = TRUE;\
		ulint	i;\
		memset(buf, 'x', 10);\
		buf[10] = '\0';\
		format_in_hex = FALSE;\
		fprintf(stderr, "TESTING \"\\x");\
		for (i = 0; i < data_len; i++) {\
			fprintf(stderr, "%02hhX", data[i]);\
		}\
		fprintf(stderr, "\", %lu, %lu, %lu\n",\
                        (ulint) data_len, (ulint) prtype,\
			(ulint) buf_size);\
		ret = row_raw_format_int(data, data_len, prtype,\
					 buf, buf_size, &format_in_hex);\
		if (ret != ret_expected) {\
			fprintf(stderr, "expected ret %lu, got %lu\n",\
				(ulint) ret_expected, ret);\
			ok = FALSE;\
                }\
                if (strcmp((char*) buf, buf_expected) != 0) {\
                        fprintf(stderr, "expected buf \"%s\", got \"%s\"\n",\
                                buf_expected, buf);\
                        ok = FALSE;\
                }\
                if (format_in_hex != format_in_hex_expected) {\
                        fprintf(stderr, "expected format_in_hex %d, got %d\n",\
                                (int) format_in_hex_expected,\
				(int) format_in_hex);\
                        ok = FALSE;\
                }\
                if (ok) {\
                        fprintf(stderr, "OK: %lu, \"%s\" %d\n\n",\
                                (ulint) ret, buf, (int) format_in_hex);\
                } else {\
                        return;\
                }\
        } while (0)

#if 1
	/* min values for signed 1-8 byte integers */

	CALL_AND_TEST("\x00", 1, 0,
		      buf, sizeof(buf), 5, "-128", 0);

	CALL_AND_TEST("\x00\x00", 2, 0,
		      buf, sizeof(buf), 7, "-32768", 0);

	CALL_AND_TEST("\x00\x00\x00", 3, 0,
		      buf, sizeof(buf), 9, "-8388608", 0);

	CALL_AND_TEST("\x00\x00\x00\x00", 4, 0,
		      buf, sizeof(buf), 12, "-2147483648", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00", 5, 0,
		      buf, sizeof(buf), 14, "-549755813888", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00", 6, 0,
		      buf, sizeof(buf), 17, "-140737488355328", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00\x00", 7, 0,
		      buf, sizeof(buf), 19, "-36028797018963968", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00\x00\x00", 8, 0,
		      buf, sizeof(buf), 21, "-9223372036854775808", 0);

	/* min values for unsigned 1-8 byte integers */

	CALL_AND_TEST("\x00", 1, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00", 2, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00", 3, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00\x00", 4, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00", 5, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00", 6, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00\x00", 7, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x00\x00\x00", 8, DATA_UNSIGNED,
		      buf, sizeof(buf), 2, "0", 0);

	/* max values for signed 1-8 byte integers */

	CALL_AND_TEST("\xFF", 1, 0,
		      buf, sizeof(buf), 4, "127", 0);

	CALL_AND_TEST("\xFF\xFF", 2, 0,
		      buf, sizeof(buf), 6, "32767", 0);

	CALL_AND_TEST("\xFF\xFF\xFF", 3, 0,
		      buf, sizeof(buf), 8, "8388607", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF", 4, 0,
		      buf, sizeof(buf), 11, "2147483647", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF", 5, 0,
		      buf, sizeof(buf), 13, "549755813887", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF", 6, 0,
		      buf, sizeof(buf), 16, "140737488355327", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 7, 0,
		      buf, sizeof(buf), 18, "36028797018963967", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8, 0,
		      buf, sizeof(buf), 20, "9223372036854775807", 0);

	/* max values for unsigned 1-8 byte integers */

	CALL_AND_TEST("\xFF", 1, DATA_UNSIGNED,
		      buf, sizeof(buf), 4, "255", 0);

	CALL_AND_TEST("\xFF\xFF", 2, DATA_UNSIGNED,
		      buf, sizeof(buf), 6, "65535", 0);

	CALL_AND_TEST("\xFF\xFF\xFF", 3, DATA_UNSIGNED,
		      buf, sizeof(buf), 9, "16777215", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF", 4, DATA_UNSIGNED,
		      buf, sizeof(buf), 11, "4294967295", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF", 5, DATA_UNSIGNED,
		      buf, sizeof(buf), 14, "1099511627775", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF", 6, DATA_UNSIGNED,
		      buf, sizeof(buf), 16, "281474976710655", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 7, DATA_UNSIGNED,
		      buf, sizeof(buf), 18, "72057594037927935", 0);

	CALL_AND_TEST("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8, DATA_UNSIGNED,
		      buf, sizeof(buf), 21, "18446744073709551615", 0);

	/* some random values */

	CALL_AND_TEST("\x52", 1, 0,
		      buf, sizeof(buf), 4, "-46", 0);

	CALL_AND_TEST("\x0E", 1, DATA_UNSIGNED,
		      buf, sizeof(buf), 3, "14", 0);

	CALL_AND_TEST("\x62\xCE", 2, 0,
		      buf, sizeof(buf), 6, "-7474", 0);

	CALL_AND_TEST("\x29\xD6", 2, DATA_UNSIGNED,
		      buf, sizeof(buf), 6, "10710", 0);

	CALL_AND_TEST("\x7F\xFF\x90", 3, 0,
		      buf, sizeof(buf), 5, "-112", 0);

	CALL_AND_TEST("\x00\xA1\x16", 3, DATA_UNSIGNED,
		      buf, sizeof(buf), 6, "41238", 0);

	CALL_AND_TEST("\x7F\xFF\xFF\xF7", 4, 0,
		      buf, sizeof(buf), 3, "-9", 0);

	CALL_AND_TEST("\x00\x00\x00\x5C", 4, DATA_UNSIGNED,
		      buf, sizeof(buf), 3, "92", 0);

	CALL_AND_TEST("\x7F\xFF\xFF\xFF\xFF\xFF\xDC\x63", 8, 0,
		      buf, sizeof(buf), 6, "-9117", 0);

	CALL_AND_TEST("\x00\x00\x00\x00\x00\x01\x64\x62", 8, DATA_UNSIGNED,
		      buf, sizeof(buf), 6, "91234", 0);
#endif

	/* speed test */

	ut_chrono_t	ch(__func__);

	for (i = 0; i < 1000000; i++) {
		row_raw_format_int("\x23", 1,
				   0, buf, sizeof(buf),
				   &format_in_hex);
		row_raw_format_int("\x23", 1,
				   DATA_UNSIGNED, buf, sizeof(buf),
				   &format_in_hex);

		row_raw_format_int("\x00\x00\x00\x00\x00\x01\x64\x62", 8,
				   0, buf, sizeof(buf),
				   &format_in_hex);
		row_raw_format_int("\x00\x00\x00\x00\x00\x01\x64\x62", 8,
				   DATA_UNSIGNED, buf, sizeof(buf),
				   &format_in_hex);
	}
}

#endif /* HAVE_UT_CHRONO_T */

#endif /* UNIV_ENABLE_UNIT_TEST_ROW_RAW_FORMAT_INT */
