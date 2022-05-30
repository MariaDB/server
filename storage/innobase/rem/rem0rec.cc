/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/********************************************************************//**
@file rem/rem0rec.cc
Record manager

Created 5/30/1994 Heikki Tuuri
*************************************************************************/

#include "rem0rec.h"
#include "page0page.h"
#include "mtr0log.h"
#include "fts0fts.h"
#include "trx0sys.h"
#include "row0log.h"

/*			PHYSICAL RECORD (OLD STYLE)
			===========================

The physical record, which is the data type of all the records
found in index pages of the database, has the following format
(lower addresses and more significant bits inside a byte are below
represented on a higher text line):

| offset of the end of the last field of data, the most significant
  bit is set to 1 if and only if the field is SQL-null,
  if the offset is 2-byte, then the second most significant
  bit is set to 1 if the field is stored on another page:
  mostly this will occur in the case of big BLOB fields |
...
| offset of the end of the first field of data + the SQL-null bit |
| 4 bits used to delete mark a record, and mark a predefined
  minimum record in alphabetical order |
| 4 bits giving the number of records owned by this record
  (this term is explained in page0page.h) |
| 13 bits giving the order number of this record in the
  heap of the index page |
| 10 bits giving the number of fields in this record |
| 1 bit which is set to 1 if the offsets above are given in
  one byte format, 0 if in two byte format |
| two bytes giving an absolute pointer to the next record in the page |
ORIGIN of the record
| first field of data |
...
| last field of data |

The origin of the record is the start address of the first field
of data. The offsets are given relative to the origin.
The offsets of the data fields are stored in an inverted
order because then the offset of the first fields are near the
origin, giving maybe a better processor cache hit rate in searches.

The offsets of the data fields are given as one-byte
(if there are less than 127 bytes of data in the record)
or two-byte unsigned integers. The most significant bit
is not part of the offset, instead it indicates the SQL-null
if the bit is set to 1. */

/*			PHYSICAL RECORD (NEW STYLE)
			===========================

The physical record, which is the data type of all the records
found in index pages of the database, has the following format
(lower addresses and more significant bits inside a byte are below
represented on a higher text line):

| length of the last non-null variable-length field of data:
  if the maximum length is 255, one byte; otherwise,
  0xxxxxxx (one byte, length=0..127), or 1exxxxxxxxxxxxxx (two bytes,
  length=128..16383, extern storage flag) |
...
| length of first variable-length field of data |
| SQL-null flags (1 bit per nullable field), padded to full bytes |
| 4 bits used to delete mark a record, and mark a predefined
  minimum record in alphabetical order |
| 4 bits giving the number of records owned by this record
  (this term is explained in page0page.h) |
| 13 bits giving the order number of this record in the
  heap of the index page |
| 3 bits record type: 000=conventional, 001=node pointer (inside B-tree),
  010=infimum, 011=supremum, 1xx=reserved |
| two bytes giving a relative pointer to the next record in the page |
ORIGIN of the record
| first field of data |
...
| last field of data |

The origin of the record is the start address of the first field
of data. The offsets are given relative to the origin.
The offsets of the data fields are stored in an inverted
order because then the offset of the first fields are near the
origin, giving maybe a better processor cache hit rate in searches.

The offsets of the data fields are given as one-byte
(if there are less than 127 bytes of data in the record)
or two-byte unsigned integers. The most significant bit
is not part of the offset, instead it indicates the SQL-null
if the bit is set to 1. */

/* CANONICAL COORDINATES. A record can be seen as a single
string of 'characters' in the following way: catenate the bytes
in each field, in the order of fields. An SQL-null field
is taken to be an empty sequence of bytes. Then after
the position of each field insert in the string
the 'character' <FIELD-END>, except that after an SQL-null field
insert <NULL-FIELD-END>. Now the ordinal position of each
byte in this canonical string is its canonical coordinate.
So, for the record ("AA", SQL-NULL, "BB", ""), the canonical
string is "AA<FIELD_END><NULL-FIELD-END>BB<FIELD-END><FIELD-END>".
We identify prefixes (= initial segments) of a record
with prefixes of the canonical string. The canonical
length of the prefix is the length of the corresponding
prefix of the canonical string. The canonical length of
a record is the length of its canonical string.

For example, the maximal common prefix of records
("AA", SQL-NULL, "BB", "C") and ("AA", SQL-NULL, "B", "C")
is "AA<FIELD-END><NULL-FIELD-END>B", and its canonical
length is 5.

A complete-field prefix of a record is a prefix which ends at the
end of some field (containing also <FIELD-END>).
A record is a complete-field prefix of another record, if
the corresponding canonical strings have the same property. */

/***************************************************************//**
Validates the consistency of an old-style physical record.
@return TRUE if ok */
static
ibool
rec_validate_old(
/*=============*/
	const rec_t*	rec);	/*!< in: physical record */

/******************************************************//**
Determine how many of the first n columns in a compact
physical record are stored externally.
@return number of externally stored columns */
ulint
rec_get_n_extern_new(
/*=================*/
	const rec_t*		rec,	/*!< in: compact physical record */
	const dict_index_t*	index,	/*!< in: record descriptor */
	ulint			n)	/*!< in: number of columns to scan */
{
	const byte*	nulls;
	const byte*	lens;
	ulint		null_mask;
	ulint		n_extern;
	ulint		i;

	ut_ad(dict_table_is_comp(index->table));
	ut_ad(!index->table->supports_instant());
	ut_ad(!index->is_instant());
	ut_ad(rec_get_status(rec) == REC_STATUS_ORDINARY
	      || rec_get_status(rec) == REC_STATUS_INSTANT);
	ut_ad(n == ULINT_UNDEFINED || n <= dict_index_get_n_fields(index));

	if (n == ULINT_UNDEFINED) {
		n = dict_index_get_n_fields(index);
	}

	nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
	lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
	null_mask = 1;
	n_extern = 0;
	i = 0;

	/* read the lengths of fields 0..n */
	do {
		const dict_field_t*	field
			= dict_index_get_nth_field(index, i);
		const dict_col_t*	col
			= dict_field_get_col(field);
		ulint			len;

		if (!(col->prtype & DATA_NOT_NULL)) {
			/* nullable field => read the null flag */

			if (UNIV_UNLIKELY(!(byte) null_mask)) {
				nulls--;
				null_mask = 1;
			}

			if (*nulls & null_mask) {
				null_mask <<= 1;
				/* No length is stored for NULL fields. */
				continue;
			}
			null_mask <<= 1;
		}

		if (UNIV_UNLIKELY(!field->fixed_len)) {
			/* Variable-length field: read the length */
			len = *lens--;
			/* If the maximum length of the field is up
			to 255 bytes, the actual length is always
			stored in one byte. If the maximum length is
			more than 255 bytes, the actual length is
			stored in one byte for 0..127.  The length
			will be encoded in two bytes when it is 128 or
			more, or when the field is stored externally. */
			if (DATA_BIG_COL(col)) {
				if (len & 0x80) {
					/* 1exxxxxxx xxxxxxxx */
					if (len & 0x40) {
						n_extern++;
					}
					lens--;
				}
			}
		}
	} while (++i < n);

	return(n_extern);
}

/** Format of a leaf-page ROW_FORMAT!=REDUNDANT record */
enum rec_leaf_format {
	/** Temporary file record */
	REC_LEAF_TEMP,
	/** Temporary file record, with added columns (REC_STATUS_INSTANT) */
	REC_LEAF_TEMP_INSTANT,
	/** Normal (REC_STATUS_ORDINARY) */
	REC_LEAF_ORDINARY,
	/** With add or drop columns (REC_STATUS_INSTANT) */
	REC_LEAF_INSTANT
};

/** Determine the offset to each field in a leaf-page record
in ROW_FORMAT=COMPACT,DYNAMIC,COMPRESSED.
This is a special case of rec_init_offsets() and rec_get_offsets_func().
@tparam	mblob	whether the record includes a metadata BLOB
@tparam	redundant_temp	whether the record belongs to a temporary file
			of a ROW_FORMAT=REDUNDANT table
@param[in]	rec	leaf-page record
@param[in]	index	the index that the record belongs in
@param[in]	n_core	number of core fields (index->n_core_fields)
@param[in]	def_val	default values for non-core fields, or
			NULL to refer to index->fields[].col->def_val
@param[in,out]	offsets	offsets, with valid rec_offs_n_fields(offsets)
@param[in]	format	record format */
template<bool mblob = false, bool redundant_temp = false>
static inline
void
rec_init_offsets_comp_ordinary(
	const rec_t*		rec,
	const dict_index_t*	index,
	rec_offs*		offsets,
	ulint			n_core,
	const dict_col_t::def_t*def_val,
	rec_leaf_format		format)
{
	rec_offs	offs		= 0;
	rec_offs	any		= 0;
	const byte*	nulls		= rec;
	const byte*	lens		= NULL;
	ulint		n_fields	= n_core;
	ulint		null_mask	= 1;

	ut_ad(n_core > 0);
	ut_ad(index->n_core_fields >= n_core);
	ut_ad(index->n_fields >= index->n_core_fields);
	ut_ad(index->n_core_null_bytes <= UT_BITS_IN_BYTES(index->n_nullable));
	ut_ad(format == REC_LEAF_TEMP || format == REC_LEAF_TEMP_INSTANT
	      || dict_table_is_comp(index->table));
	ut_ad(format != REC_LEAF_TEMP_INSTANT
	      || index->n_fields == rec_offs_n_fields(offsets));
	ut_d(ulint n_null= 0);

	const unsigned n_core_null_bytes = UNIV_UNLIKELY(index->n_core_fields
							 != n_core)
		? UT_BITS_IN_BYTES(unsigned(index->get_n_nullable(n_core)))
		: (redundant_temp
		   ? UT_BITS_IN_BYTES(index->n_nullable)
		   : index->n_core_null_bytes);

	if (mblob) {
		ut_ad(index->table->instant);
		ut_ad(index->is_instant());
		ut_ad(rec_offs_n_fields(offsets)
		      <= ulint(index->n_fields) + 1);
		ut_ad(!def_val);
		ut_ad(format == REC_LEAF_INSTANT);
		nulls -= REC_N_NEW_EXTRA_BYTES;
		n_fields = n_core + 1 + rec_get_n_add_field(nulls);
		ut_ad(n_fields <= ulint(index->n_fields) + 1);
		const ulint n_nullable = index->get_n_nullable(n_fields - 1);
		const ulint n_null_bytes = UT_BITS_IN_BYTES(n_nullable);
		ut_d(n_null = n_nullable);
		ut_ad(n_null <= index->n_nullable);
		ut_ad(n_null_bytes >= n_core_null_bytes
		      || n_core < index->n_core_fields);
		lens = --nulls - n_null_bytes;
		goto start;
	}

	switch (format) {
	case REC_LEAF_TEMP:
		if (dict_table_is_comp(index->table)) {
			/* No need to do adjust fixed_len=0. We only need to
			adjust it for ROW_FORMAT=REDUNDANT. */
			format = REC_LEAF_ORDINARY;
		}
		goto ordinary;
	case REC_LEAF_ORDINARY:
		nulls -= REC_N_NEW_EXTRA_BYTES;
ordinary:
		lens = --nulls - n_core_null_bytes;

		ut_d(n_null = std::min<uint>(n_core_null_bytes * 8U,
					     index->n_nullable));
		break;
	case REC_LEAF_INSTANT:
		nulls -= REC_N_NEW_EXTRA_BYTES;
		ut_ad(index->is_instant());
		/* fall through */
	case REC_LEAF_TEMP_INSTANT:
		n_fields = n_core + rec_get_n_add_field(nulls) + 1;
		ut_ad(n_fields <= index->n_fields);
		const ulint n_nullable = index->get_n_nullable(n_fields);
		const ulint n_null_bytes = UT_BITS_IN_BYTES(n_nullable);
		ut_d(n_null = n_nullable);
		ut_ad(n_null <= index->n_nullable);
		ut_ad(n_null_bytes >= n_core_null_bytes
		      || n_core < index->n_core_fields);
		lens = --nulls - n_null_bytes;
	}

start:
#ifdef UNIV_DEBUG
	/* We cannot invoke rec_offs_make_valid() if format==REC_LEAF_TEMP.
	Similarly, rec_offs_validate() will fail in that case, because
	it invokes rec_get_status(). */
	memcpy(&offsets[RECORD_OFFSET], &rec, sizeof(rec));
	memcpy(&offsets[INDEX_OFFSET], &index, sizeof(index));
#endif /* UNIV_DEBUG */

	/* read the lengths of fields 0..n_fields */
	rec_offs len;
	ulint i = 0;
	const dict_field_t* field = index->fields;

	do {
		if (mblob) {
			if (i == index->first_user_field()) {
				offs = static_cast<rec_offs>(offs
							     + FIELD_REF_SIZE);
				len = combine(offs, STORED_OFFPAGE);
				any |= REC_OFFS_EXTERNAL;
				field--;
				continue;
			} else if (i >= n_fields) {
				len = combine(offs, DEFAULT);
				any |= REC_OFFS_DEFAULT;
				continue;
			}
		} else if (i < n_fields) {
			/* The field is present, and will be covered below. */
		} else if (!mblob && def_val) {
			const dict_col_t::def_t& d = def_val[i - n_core];
			if (!d.data) {
				len = combine(offs, SQL_NULL);
				ut_ad(d.len == UNIV_SQL_NULL);
			} else {
				len = combine(offs, DEFAULT);
				any |= REC_OFFS_DEFAULT;
			}

			continue;
		} else {
			ulint dlen;
			if (!index->instant_field_value(i, &dlen)) {
				len = combine(offs, SQL_NULL);
				ut_ad(dlen == UNIV_SQL_NULL);
			} else {
				len = combine(offs, DEFAULT);
				any |= REC_OFFS_DEFAULT;
			}

			continue;
		}

		const dict_col_t* col = field->col;

		if (col->is_nullable()) {
			/* nullable field => read the null flag */
			ut_ad(n_null--);

			if (UNIV_UNLIKELY(!(byte) null_mask)) {
				nulls--;
				null_mask = 1;
			}

			if (*nulls & null_mask) {
				null_mask <<= 1;
				/* No length is stored for NULL fields.
				We do not advance offs, and we set
				the length to zero and enable the
				SQL NULL flag in offsets[]. */
				len = combine(offs, SQL_NULL);
				continue;
			}
			null_mask <<= 1;
		}

		if (!field->fixed_len
		    || (format == REC_LEAF_TEMP
			&& !dict_col_get_fixed_size(col, true))) {
			/* Variable-length field: read the length */
			len = *lens--;
			/* If the maximum length of the field is up
			to 255 bytes, the actual length is always
			stored in one byte. If the maximum length is
			more than 255 bytes, the actual length is
			stored in one byte for 0..127.  The length
			will be encoded in two bytes when it is 128 or
			more, or when the field is stored externally. */
			if ((len & 0x80) && DATA_BIG_COL(col)) {
				/* 1exxxxxxx xxxxxxxx */
				len = static_cast<rec_offs>(len << 8
							    | *lens--);
				offs = static_cast<rec_offs>(offs
							     + get_value(len));
				if (UNIV_UNLIKELY(len & 0x4000)) {
					ut_ad(index->is_primary());
					any |= REC_OFFS_EXTERNAL;
					len = combine(offs, STORED_OFFPAGE);
				} else {
					len = offs;
				}

				continue;
			}

			len = offs = static_cast<rec_offs>(offs + len);
		} else {
			len = offs = static_cast<rec_offs>(offs
							   + field->fixed_len);
		}
	} while (field++, rec_offs_base(offsets)[++i] = len,
		 i < rec_offs_n_fields(offsets));

	*rec_offs_base(offsets) = static_cast<rec_offs>((rec - (lens + 1))
							| REC_OFFS_COMPACT
							| any);
}

#ifdef UNIV_DEBUG
/** Update debug data in offsets, in order to tame rec_offs_validate().
@param[in]	rec	record
@param[in]	index	the index that the record belongs in
@param[in]	leaf	whether the record resides in a leaf page
@param[in,out]	offsets	offsets from rec_get_offsets() to adjust */
void
rec_offs_make_valid(
	const rec_t*		rec,
	const dict_index_t*	index,
	bool			leaf,
	rec_offs*		offsets)
{
	const bool is_alter_metadata = leaf
		&& rec_is_alter_metadata(rec, *index);
	ut_ad((leaf && rec_is_metadata(rec, *index))
	      || index->is_dummy || index->is_ibuf()
	      || (leaf
		  ? rec_offs_n_fields(offsets)
		  <= dict_index_get_n_fields(index)
		  : rec_offs_n_fields(offsets) - 1
		  <= dict_index_get_n_unique_in_tree_nonleaf(index)));
	const bool is_user_rec = (dict_table_is_comp(index->table)
				  ? rec_get_heap_no_new(rec)
				  : rec_get_heap_no_old(rec))
		>= PAGE_HEAP_NO_USER_LOW;
	ulint n = rec_get_n_fields(rec, index);
	/* The infimum and supremum records carry 1 field. */
	ut_ad(is_user_rec || n == 1);
	ut_ad(is_user_rec || rec_offs_n_fields(offsets) == 1);
	ut_ad(!is_user_rec
	      || (n + (index->id == DICT_INDEXES_ID)) >= index->n_core_fields
	      || n >= rec_offs_n_fields(offsets));
	for (; n < rec_offs_n_fields(offsets); n++) {
		ut_ad(leaf);
		ut_ad(is_alter_metadata
		      || get_type(rec_offs_base(offsets)[1 + n]) == DEFAULT);
	}
	memcpy(&offsets[RECORD_OFFSET], &rec, sizeof(rec));
	memcpy(&offsets[INDEX_OFFSET], &index, sizeof(index));
}

/** Validate offsets returned by rec_get_offsets().
@param[in]	rec	record, or NULL
@param[in]	index	the index that the record belongs in, or NULL
@param[in,out]	offsets	the offsets of the record
@return true */
bool
rec_offs_validate(
	const rec_t*		rec,
	const dict_index_t*	index,
	const rec_offs*		offsets)
{
	ulint	i	= rec_offs_n_fields(offsets);
	ulint	last	= ULINT_MAX;
	ulint	comp	= *rec_offs_base(offsets) & REC_OFFS_COMPACT;

	if (rec) {
		ut_ad(!memcmp(&rec, &offsets[RECORD_OFFSET], sizeof(rec)));
		if (!comp) {
			const bool is_user_rec = rec_get_heap_no_old(rec)
				>= PAGE_HEAP_NO_USER_LOW;
			ulint n = rec_get_n_fields_old(rec);
			/* The infimum and supremum records carry 1 field. */
			ut_ad(is_user_rec || n == 1);
			ut_ad(is_user_rec || i == 1);
			ut_ad(!is_user_rec || n >= i || !index
			      || (n + (index->id == DICT_INDEXES_ID))
			      >= index->n_core_fields);
			for (; n < i; n++) {
				ut_ad(get_type(rec_offs_base(offsets)[1 + n])
				      == DEFAULT);
			}
		}
	}
	if (index) {
		ut_ad(!memcmp(&index, &offsets[INDEX_OFFSET], sizeof(index)));
		ulint max_n_fields = std::max<ulint>(
			dict_index_get_n_fields(index),
			dict_index_get_n_unique_in_tree(index) + 1);
		if (comp && rec) {
			switch (rec_get_status(rec)) {
			case REC_STATUS_INSTANT:
				ut_ad(index->is_instant() || index->is_dummy);
				ut_ad(max_n_fields == index->n_fields);
				max_n_fields += index->table->instant
					|| index->is_dummy;
				break;
			case REC_STATUS_ORDINARY:
				break;
			case REC_STATUS_NODE_PTR:
				max_n_fields = dict_index_get_n_unique_in_tree(
					index) + 1;
				break;
			case REC_STATUS_INFIMUM:
			case REC_STATUS_SUPREMUM:
				max_n_fields = 1;
				break;
			default:
				ut_error;
			}
		} else if (max_n_fields == index->n_fields
			   && (index->is_dummy
			       || (index->is_instant()
				   && index->table->instant))) {
			max_n_fields++;
		}
		/* index->n_def == 0 for dummy indexes if !comp */
		ut_ad(!comp || index->n_def);
		ut_ad(!index->n_def || i <= max_n_fields
		      || rec_is_metadata(rec, *index));
	}
	while (i--) {
		ulint curr = get_value(rec_offs_base(offsets)[1 + i]);
		ut_ad(curr <= last);
		last = curr;
	}
	return(TRUE);
}
#endif /* UNIV_DEBUG */

/** Determine the offsets to each field in the record.
 The offsets are written to a previously allocated array of
ulint, where rec_offs_n_fields(offsets) has been initialized to the
number of fields in the record.	 The rest of the array will be
initialized by this function.  rec_offs_base(offsets)[0] will be set
to the extra size (if REC_OFFS_COMPACT is set, the record is in the
new format; if REC_OFFS_EXTERNAL is set, the record contains externally
stored columns), and rec_offs_base(offsets)[1..n_fields] will be set to
offsets past the end of fields 0..n_fields, or to the beginning of
fields 1..n_fields+1.  When the type of the offset at [i+1]
is (SQL_NULL), the field i is NULL. When the type of the offset at [i+1]
is (STORED_OFFPAGE), the field i is stored externally.
@param[in]	rec	record
@param[in]	index	the index that the record belongs in
@param[in]	n_core	0, or index->n_core_fields for leaf page
@param[in,out]	offsets	array of offsets, with valid rec_offs_n_fields() */
static
void
rec_init_offsets(
	const rec_t*		rec,
	const dict_index_t*	index,
	ulint			n_core,
	rec_offs*		offsets)
{
	ulint	i	= 0;
	rec_offs	offs;

	/* This assertion was relaxed for the btr_cur_open_at_index_side()
	call in btr_cur_instant_init_low(). We cannot invoke
	index->is_instant(), because the same assertion would fail there
	until btr_cur_instant_init_low() has invoked
	dict_table_t::deserialise_columns(). */
	ut_ad(index->n_core_null_bytes <= UT_BITS_IN_BYTES(index->n_nullable)
	      || index->in_instant_init);
	ut_d(memcpy(&offsets[RECORD_OFFSET], &rec, sizeof(rec)));
	ut_d(memcpy(&offsets[INDEX_OFFSET], &index, sizeof(index)));
	ut_ad(index->n_fields >= n_core);
	ut_ad(index->n_core_fields >= n_core);

	if (dict_table_is_comp(index->table)) {
		const byte*	nulls;
		const byte*	lens;
		dict_field_t*	field;
		ulint		null_mask;
		rec_comp_status_t status = rec_get_status(rec);
		ulint		n_node_ptr_field = ULINT_UNDEFINED;

		switch (UNIV_EXPECT(status, REC_STATUS_ORDINARY)) {
		case REC_STATUS_INFIMUM:
		case REC_STATUS_SUPREMUM:
			/* the field is 8 bytes long */
			rec_offs_base(offsets)[0]
				= REC_N_NEW_EXTRA_BYTES | REC_OFFS_COMPACT;
			rec_offs_base(offsets)[1] = 8;
			return;
		case REC_STATUS_NODE_PTR:
			ut_ad(!n_core);
			n_node_ptr_field
				= dict_index_get_n_unique_in_tree_nonleaf(
					index);
			break;
		case REC_STATUS_INSTANT:
			ut_ad(index->is_instant());
			rec_init_offsets_comp_ordinary(rec, index, offsets,
						       n_core,
						       NULL,
						       REC_LEAF_INSTANT);
			return;
		case REC_STATUS_ORDINARY:
			rec_init_offsets_comp_ordinary(rec, index, offsets,
						       n_core,
						       NULL,
						       REC_LEAF_ORDINARY);
			return;
		}

		/* The n_nullable flags in the clustered index node pointer
		records in ROW_FORMAT=COMPACT or ROW_FORMAT=DYNAMIC must
		reflect the number of 'core columns'. These flags are
		useless garbage, and they are only reserved because of
		file format compatibility.
		(Clustered index node pointer records only contain the
		PRIMARY KEY columns, which are always NOT NULL,
		so we should have used n_nullable=0.) */
		ut_ad(index->n_core_fields > 0);

		nulls = rec - (REC_N_NEW_EXTRA_BYTES + 1);
		lens = nulls - index->n_core_null_bytes;
		offs = 0;
		null_mask = 1;

		/* read the lengths of fields 0..n */
		do {
			rec_offs len;
			if (UNIV_UNLIKELY(i == n_node_ptr_field)) {
				len = offs = static_cast<rec_offs>(
					offs + REC_NODE_PTR_SIZE);
				goto resolved;
			}

			field = dict_index_get_nth_field(index, i);
			if (!(dict_field_get_col(field)->prtype
			      & DATA_NOT_NULL)) {
				/* nullable field => read the null flag */

				if (UNIV_UNLIKELY(!(byte) null_mask)) {
					nulls--;
					null_mask = 1;
				}

				if (*nulls & null_mask) {
					null_mask <<= 1;
					/* No length is stored for NULL fields.
					We do not advance offs, and we set
					the length to zero and enable the
					SQL NULL flag in offsets[]. */
					len = combine(offs, SQL_NULL);
					goto resolved;
				}
				null_mask <<= 1;
			}

			if (UNIV_UNLIKELY(!field->fixed_len)) {
				const dict_col_t*	col
					= dict_field_get_col(field);
				/* Variable-length field: read the length */
				len = *lens--;
				/* If the maximum length of the field
				is up to 255 bytes, the actual length
				is always stored in one byte. If the
				maximum length is more than 255 bytes,
				the actual length is stored in one
				byte for 0..127.  The length will be
				encoded in two bytes when it is 128 or
				more, or when the field is stored
				externally. */
				if (DATA_BIG_COL(col)) {
					if (len & 0x80) {
						/* 1exxxxxxx xxxxxxxx */
						len = static_cast<rec_offs>(
							len << 8 | *lens--);

						/* B-tree node pointers
						must not contain externally
						stored columns.  Thus
						the "e" flag must be 0. */
						ut_a(!(len & 0x4000));
						offs = static_cast<rec_offs>(
							offs + get_value(len));
						len = offs;

						goto resolved;
					}
				}

				len = offs = static_cast<rec_offs>(offs + len);
			} else {
				len = offs = static_cast<rec_offs>(
					offs + field->fixed_len);
			}
resolved:
			rec_offs_base(offsets)[i + 1] = len;
		} while (++i < rec_offs_n_fields(offsets));

		*rec_offs_base(offsets)
			= static_cast<rec_offs>((rec - (lens + 1))
						| REC_OFFS_COMPACT);
	} else {
		/* Old-style record: determine extra size and end offsets */
		offs = REC_N_OLD_EXTRA_BYTES;
		const ulint n_fields = rec_get_n_fields_old(rec);
		const ulint n = std::min(n_fields, rec_offs_n_fields(offsets));
		rec_offs any;

		if (rec_get_1byte_offs_flag(rec)) {
			offs = static_cast<rec_offs>(offs + n_fields);
			any = offs;
			/* Determine offsets to fields */
			do {
				offs = rec_1_get_field_end_info(rec, i);
				if (offs & REC_1BYTE_SQL_NULL_MASK) {
					offs &= static_cast<rec_offs>(
						~REC_1BYTE_SQL_NULL_MASK);
					set_type(offs, SQL_NULL);
				}
				rec_offs_base(offsets)[1 + i] = offs;
			} while (++i < n);
		} else {
			offs = static_cast<rec_offs>(offs + 2 * n_fields);
			any = offs;
			/* Determine offsets to fields */
			do {
				offs = rec_2_get_field_end_info(rec, i);
				if (offs & REC_2BYTE_SQL_NULL_MASK) {
					offs &= static_cast<rec_offs>(
						~REC_2BYTE_SQL_NULL_MASK);
					set_type(offs, SQL_NULL);
				}
				if (offs & REC_2BYTE_EXTERN_MASK) {
					offs &= static_cast<rec_offs>(
						~REC_2BYTE_EXTERN_MASK);
					set_type(offs, STORED_OFFPAGE);
					any |= REC_OFFS_EXTERNAL;
				}
				rec_offs_base(offsets)[1 + i] = offs;
			} while (++i < n);
		}

		if (i < rec_offs_n_fields(offsets)) {
			ut_ad(index->is_instant()
			      || i + (index->id == DICT_INDEXES_ID)
			      == rec_offs_n_fields(offsets));

			ut_ad(i != 0);
			offs = combine(rec_offs_base(offsets)[i], DEFAULT);

			do {
				rec_offs_base(offsets)[1 + i] = offs;
			} while (++i < rec_offs_n_fields(offsets));

			any |= REC_OFFS_DEFAULT;
		}

		*rec_offs_base(offsets) = any;
	}
}

/** Determine the offsets to each field in an index record.
@param[in]	rec		physical record
@param[in]	index		the index that the record belongs to
@param[in,out]	offsets		array comprising offsets[0] allocated elements,
				or an array from rec_get_offsets(), or NULL
@param[in]	n_core		0, or index->n_core_fields for leaf page
@param[in]	n_fields	maximum number of offsets to compute
				(ULINT_UNDEFINED to compute all offsets)
@param[in,out]	heap		memory heap
@return the new offsets */
rec_offs*
rec_get_offsets_func(
	const rec_t*		rec,
	const dict_index_t*	index,
	rec_offs*		offsets,
	ulint			n_core,
	ulint			n_fields,
#ifdef UNIV_DEBUG
	const char*		file,	/*!< in: file name where called */
	unsigned		line,	/*!< in: line number where called */
#endif /* UNIV_DEBUG */
	mem_heap_t**		heap)	/*!< in/out: memory heap */
{
	ulint	n;
	ulint	size;
	bool	alter_metadata = false;

	ut_ad(index->n_core_fields >= n_core);
	/* This assertion was relaxed for the btr_cur_open_at_index_side()
	call in btr_cur_instant_init_low(). We cannot invoke
	index->is_instant(), because the same assertion would fail there
	until btr_cur_instant_init_low() has invoked
	dict_table_t::deserialise_columns(). */
	ut_ad(index->n_fields >= index->n_core_fields
	      || index->in_instant_init);

	if (dict_table_is_comp(index->table)) {
		switch (UNIV_EXPECT(rec_get_status(rec),
				    REC_STATUS_ORDINARY)) {
		case REC_STATUS_INSTANT:
			alter_metadata = rec_is_alter_metadata(rec, true);
			/* fall through */
		case REC_STATUS_ORDINARY:
			ut_ad(n_core);
			n = dict_index_get_n_fields(index) + alter_metadata;
			break;
		case REC_STATUS_NODE_PTR:
			/* Node pointer records consist of the
			uniquely identifying fields of the record
			followed by a child page number field. */
			ut_ad(!n_core);
			n = dict_index_get_n_unique_in_tree_nonleaf(index) + 1;
			break;
		case REC_STATUS_INFIMUM:
		case REC_STATUS_SUPREMUM:
			/* infimum or supremum record */
			ut_ad(rec_get_heap_no_new(rec)
			      == ulint(rec_get_status(rec)
                                       == REC_STATUS_INFIMUM
                                       ? PAGE_HEAP_NO_INFIMUM
                                       : PAGE_HEAP_NO_SUPREMUM));
			n = 1;
			break;
		default:
			ut_error;
			return(NULL);
		}
	} else {
		n = rec_get_n_fields_old(rec);
		/* Here, rec can be allocated from the heap (copied
		from an index page record), or it can be located in an
		index page. If rec is not in an index page, then
		page_rec_is_user_rec(rec) and similar predicates
		cannot be evaluated. We can still distinguish the
		infimum and supremum record based on the heap number. */
		const bool is_user_rec = rec_get_heap_no_old(rec)
			>= PAGE_HEAP_NO_USER_LOW;
		/* The infimum and supremum records carry 1 field. */
		ut_ad(is_user_rec || n == 1);
		ut_ad(!is_user_rec || n_core || index->is_dummy
		      || dict_index_is_ibuf(index)
		      || n == n_fields /* dict_stats_analyze_index_level() */
		      || n - 1
		      == dict_index_get_n_unique_in_tree_nonleaf(index));
		ut_ad(!is_user_rec || !n_core || index->is_dummy
		      || dict_index_is_ibuf(index)
		      || n == n_fields /* btr_pcur_restore_position() */
		      || (n + (index->id == DICT_INDEXES_ID) >= n_core));

		if (is_user_rec && n_core && n < index->n_fields) {
			ut_ad(!index->is_dummy);
			ut_ad(!dict_index_is_ibuf(index));
			n = index->n_fields;
		}
	}

	if (UNIV_UNLIKELY(n_fields < n)) {
		n = n_fields;
	}

	/* The offsets header consists of the allocation size at
	offsets[0] and the REC_OFFS_HEADER_SIZE bytes. */
	size = n + (1 + REC_OFFS_HEADER_SIZE);

	if (UNIV_UNLIKELY(!offsets)
	    || UNIV_UNLIKELY(rec_offs_get_n_alloc(offsets) < size)) {
		if (UNIV_UNLIKELY(!*heap)) {
			*heap = mem_heap_create_at(size * sizeof(*offsets),
						   file, line);
		}
		offsets = static_cast<rec_offs*>(
			mem_heap_alloc(*heap, size * sizeof(*offsets)));

		rec_offs_set_n_alloc(offsets, size);
	}

	rec_offs_set_n_fields(offsets, n);

	if (UNIV_UNLIKELY(alter_metadata) && index->table->not_redundant()) {
#ifdef UNIV_DEBUG
		memcpy(&offsets[RECORD_OFFSET], &rec, sizeof rec);
		memcpy(&offsets[INDEX_OFFSET], &index, sizeof index);
#endif /* UNIV_DEBUG */
		ut_ad(n_core);
		ut_ad(index->table->instant);
		ut_ad(index->is_instant());
		ut_ad(rec_offs_n_fields(offsets)
		      <= ulint(index->n_fields) + 1);
		rec_init_offsets_comp_ordinary<true>(rec, index, offsets,
						     index->n_core_fields,
						     nullptr,
						     REC_LEAF_INSTANT);
	} else {
		rec_init_offsets(rec, index, n_core, offsets);
	}
	return offsets;
}

/******************************************************//**
The following function determines the offsets to each field
in the record.  It can reuse a previously allocated array. */
void
rec_get_offsets_reverse(
/*====================*/
	const byte*		extra,	/*!< in: the extra bytes of a
					compact record in reverse order,
					excluding the fixed-size
					REC_N_NEW_EXTRA_BYTES */
	const dict_index_t*	index,	/*!< in: record descriptor */
	ulint			node_ptr,/*!< in: nonzero=node pointer,
					0=leaf node */
	rec_offs*		offsets)/*!< in/out: array consisting of
					offsets[0] allocated elements */
{
	ulint		n;
	ulint		i;
	rec_offs	offs;
	rec_offs	any_ext = 0;
	const byte*	nulls;
	const byte*	lens;
	dict_field_t*	field;
	ulint		null_mask;
	ulint		n_node_ptr_field;

	ut_ad(dict_table_is_comp(index->table));
	ut_ad(!index->is_instant());

	if (UNIV_UNLIKELY(node_ptr != 0)) {
		n_node_ptr_field =
			dict_index_get_n_unique_in_tree_nonleaf(index);
		n = n_node_ptr_field + 1;
	} else {
		n_node_ptr_field = ULINT_UNDEFINED;
		n = dict_index_get_n_fields(index);
	}

	ut_a(rec_offs_get_n_alloc(offsets) >= n + (1 + REC_OFFS_HEADER_SIZE));
	rec_offs_set_n_fields(offsets, n);

	nulls = extra;
	lens = nulls + UT_BITS_IN_BYTES(index->n_nullable);
	i = offs = 0;
	null_mask = 1;

	/* read the lengths of fields 0..n */
	do {
		rec_offs len;
		if (UNIV_UNLIKELY(i == n_node_ptr_field)) {
			len = offs = static_cast<rec_offs>(
				offs + REC_NODE_PTR_SIZE);
			goto resolved;
		}

		field = dict_index_get_nth_field(index, i);
		if (!(dict_field_get_col(field)->prtype & DATA_NOT_NULL)) {
			/* nullable field => read the null flag */

			if (UNIV_UNLIKELY(!(byte) null_mask)) {
				nulls++;
				null_mask = 1;
			}

			if (*nulls & null_mask) {
				null_mask <<= 1;
				/* No length is stored for NULL fields.
				We do not advance offs, and we set
				the length to zero and enable the
				SQL NULL flag in offsets[]. */
				len = combine(offs, SQL_NULL);
				goto resolved;
			}
			null_mask <<= 1;
		}

		if (UNIV_UNLIKELY(!field->fixed_len)) {
			/* Variable-length field: read the length */
			const dict_col_t*	col
				= dict_field_get_col(field);
			len = *lens++;
			/* If the maximum length of the field is up
			to 255 bytes, the actual length is always
			stored in one byte. If the maximum length is
			more than 255 bytes, the actual length is
			stored in one byte for 0..127.  The length
			will be encoded in two bytes when it is 128 or
			more, or when the field is stored externally. */
			if (DATA_BIG_COL(col)) {
				if (len & 0x80) {
					/* 1exxxxxxx xxxxxxxx */
					len = static_cast<rec_offs>(
						len << 8 | *lens++);

					offs = static_cast<rec_offs>(
						offs + get_value(len));
					if (UNIV_UNLIKELY(len & 0x4000)) {
						any_ext = REC_OFFS_EXTERNAL;
						len = combine(offs,
							      STORED_OFFPAGE);
					} else {
						len = offs;
					}

					goto resolved;
				}
			}

			len = offs = static_cast<rec_offs>(offs + len);
		} else {
			len = offs = static_cast<rec_offs>(offs
							   + field->fixed_len);
		}
resolved:
		rec_offs_base(offsets)[i + 1] = len;
	} while (++i < rec_offs_n_fields(offsets));

	ut_ad(lens >= extra);
	*rec_offs_base(offsets)
		= static_cast<rec_offs>(lens - extra + REC_N_NEW_EXTRA_BYTES)
		  | REC_OFFS_COMPACT | any_ext;
}

/************************************************************//**
The following function is used to get the offset to the nth
data field in an old-style record.
@return offset to the field */
ulint
rec_get_nth_field_offs_old(
/*=======================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n,	/*!< in: index of the field */
	ulint*		len)	/*!< out: length of the field;
				UNIV_SQL_NULL if SQL null */
{
	ulint	os;
	ulint	next_os;

	ut_a(n < rec_get_n_fields_old(rec));

	if (rec_get_1byte_offs_flag(rec)) {
		os = rec_1_get_field_start_offs(rec, n);

		next_os = rec_1_get_field_end_info(rec, n);

		if (next_os & REC_1BYTE_SQL_NULL_MASK) {
			*len = UNIV_SQL_NULL;

			return(os);
		}

		next_os = next_os & ~REC_1BYTE_SQL_NULL_MASK;
	} else {
		os = rec_2_get_field_start_offs(rec, n);

		next_os = rec_2_get_field_end_info(rec, n);

		if (next_os & REC_2BYTE_SQL_NULL_MASK) {
			*len = UNIV_SQL_NULL;

			return(os);
		}

		next_os = next_os & ~(REC_2BYTE_SQL_NULL_MASK
				      | REC_2BYTE_EXTERN_MASK);
	}

	*len = next_os - os;

	ut_ad(*len < srv_page_size);

	return(os);
}

/** Determine the size of a data tuple prefix in ROW_FORMAT=COMPACT.
@tparam	mblob		whether the record includes a metadata BLOB
@tparam redundant_temp	whether to use the ROW_FORMAT=REDUNDANT format
@param[in]	index		record descriptor; dict_table_is_comp()
				is assumed to hold, even if it doesn't
@param[in]	dfield		array of data fields
@param[in]	n_fields	number of data fields
@param[out]	extra		extra size
@param[in]	status		status flags
@param[in]	temp		whether this is a temporary file record
@return total size */
template<bool mblob = false, bool redundant_temp = false>
static inline
ulint
rec_get_converted_size_comp_prefix_low(
	const dict_index_t*	index,
	const dfield_t*		dfield,
	ulint			n_fields,
	ulint*			extra,
	rec_comp_status_t	status,
	bool			temp)
{
	ulint	extra_size = temp ? 0 : REC_N_NEW_EXTRA_BYTES;
	ut_ad(n_fields > 0);
	ut_ad(n_fields - mblob <= dict_index_get_n_fields(index));
	ut_d(ulint n_null = index->n_nullable);
	ut_ad(status == REC_STATUS_ORDINARY || status == REC_STATUS_NODE_PTR
	      || status == REC_STATUS_INSTANT);
	unsigned n_core_fields = redundant_temp
		? row_log_get_n_core_fields(index)
		: index->n_core_fields;

	if (mblob) {
		ut_ad(index->table->instant);
		ut_ad(!redundant_temp && index->is_instant());
		ut_ad(status == REC_STATUS_INSTANT);
		ut_ad(n_fields == ulint(index->n_fields) + 1);
		extra_size += UT_BITS_IN_BYTES(index->n_nullable)
			+ rec_get_n_add_field_len(n_fields - 1
						  - n_core_fields);
	} else if (status == REC_STATUS_INSTANT
		   && (!temp || n_fields > n_core_fields)) {
		if (!redundant_temp) { ut_ad(index->is_instant()); }
		ut_ad(UT_BITS_IN_BYTES(n_null) >= index->n_core_null_bytes);
		extra_size += UT_BITS_IN_BYTES(index->get_n_nullable(n_fields))
			+ rec_get_n_add_field_len(n_fields - 1
						  - n_core_fields);
	} else {
		ut_ad(n_fields <= n_core_fields);
		extra_size += redundant_temp
			? UT_BITS_IN_BYTES(index->n_nullable)
			: index->n_core_null_bytes;
	}

	ulint data_size = 0;

	if (temp && dict_table_is_comp(index->table)) {
		/* No need to do adjust fixed_len=0. We only need to
		adjust it for ROW_FORMAT=REDUNDANT. */
		temp = false;
	}

	const dfield_t* const end = dfield + n_fields;
	/* read the lengths of fields 0..n */
	for (ulint i = 0; dfield < end; i++, dfield++) {
		if (mblob && i == index->first_user_field()) {
			data_size += FIELD_REF_SIZE;
			if (++dfield == end) {
				ut_ad(i == index->n_fields);
				break;
			}
		}

		ulint len = dfield_get_len(dfield);

		const dict_field_t* field = dict_index_get_nth_field(index, i);
#ifdef UNIV_DEBUG
		if (dict_index_is_spatial(index)) {
			if (DATA_GEOMETRY_MTYPE(field->col->mtype) && i == 0) {
				ut_ad(dfield->type.prtype & DATA_GIS_MBR);
			} else {
				ut_ad(dfield->type.mtype == DATA_SYS_CHILD
				      || dict_col_type_assert_equal(
					      field->col, &dfield->type));
			}
		} else {
			ut_ad(field->col->is_dropped()
			      || dict_col_type_assert_equal(field->col,
							    &dfield->type));
		}
#endif

		/* All NULLable fields must be included in the n_null count. */
		ut_ad(!field->col->is_nullable() || n_null--);

		if (dfield_is_null(dfield)) {
			/* No length is stored for NULL fields. */
			ut_ad(field->col->is_nullable());
			continue;
		}

		ut_ad(len <= field->col->len
		      || DATA_LARGE_MTYPE(field->col->mtype)
		      || (field->col->len == 0
			  && field->col->mtype == DATA_VARCHAR));

		ulint fixed_len = field->fixed_len;
		if (temp && fixed_len
		    && !dict_col_get_fixed_size(field->col, temp)) {
			fixed_len = 0;
		}
		/* If the maximum length of a variable-length field
		is up to 255 bytes, the actual length is always stored
		in one byte. If the maximum length is more than 255
		bytes, the actual length is stored in one byte for
		0..127.  The length will be encoded in two bytes when
		it is 128 or more, or when the field is stored externally. */

		if (fixed_len) {
#ifdef UNIV_DEBUG
			ut_ad(len <= fixed_len);

			if (dict_index_is_spatial(index)) {
				ut_ad(dfield->type.mtype == DATA_SYS_CHILD
				      || !field->col->mbmaxlen
				      || len >= field->col->mbminlen
				      * fixed_len / field->col->mbmaxlen);
			} else {
				ut_ad(dfield->type.mtype != DATA_SYS_CHILD);

				ut_ad(field->col->is_dropped()
				      || !field->col->mbmaxlen
				      || len >= field->col->mbminlen
				      * fixed_len / field->col->mbmaxlen);
			}

			/* dict_index_add_col() should guarantee this */
			ut_ad(!field->prefix_len
			      || fixed_len == field->prefix_len);
#endif /* UNIV_DEBUG */
		} else if (dfield_is_ext(dfield)) {
			ut_ad(DATA_BIG_COL(field->col));
			extra_size += 2;
		} else if (len < 128 || !DATA_BIG_COL(field->col)) {
			extra_size++;
		} else {
			/* For variable-length columns, we look up the
			maximum length from the column itself.  If this
			is a prefix index column shorter than 256 bytes,
			this will waste one byte. */
			extra_size += 2;
		}
		data_size += len;
	}

	if (extra) {
		*extra = extra_size;
	}

	return(extra_size + data_size);
}

/**********************************************************//**
Determines the size of a data tuple prefix in ROW_FORMAT=COMPACT.
@return total size */
ulint
rec_get_converted_size_comp_prefix(
/*===============================*/
	const dict_index_t*	index,	/*!< in: record descriptor */
	const dfield_t*		fields,	/*!< in: array of data fields */
	ulint			n_fields,/*!< in: number of data fields */
	ulint*			extra)	/*!< out: extra size */
{
	ut_ad(dict_table_is_comp(index->table));
	return(rec_get_converted_size_comp_prefix_low(
		       index, fields, n_fields, extra,
		       REC_STATUS_ORDINARY, false));
}

/** Determine the size of a record in ROW_FORMAT=COMPACT.
@param[in]	index		record descriptor. dict_table_is_comp()
				is assumed to hold, even if it doesn't
@param[in]	tuple		logical record
@param[out]	extra		extra size
@return total size */
ulint
rec_get_converted_size_comp(
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	ulint*			extra)
{
	ut_ad(tuple->n_fields > 0);

	rec_comp_status_t status = rec_comp_status_t(tuple->info_bits
						     & REC_NEW_STATUS_MASK);

	switch (UNIV_EXPECT(status, REC_STATUS_ORDINARY)) {
	case REC_STATUS_ORDINARY:
		ut_ad(!tuple->is_metadata());
		if (tuple->n_fields > index->n_core_fields) {
			ut_ad(index->is_instant());
			status = REC_STATUS_INSTANT;
		}
		/* fall through */
	case REC_STATUS_INSTANT:
		ut_ad(tuple->n_fields >= index->n_core_fields);
		if (tuple->is_alter_metadata()) {
			return rec_get_converted_size_comp_prefix_low<true>(
				index, tuple->fields, tuple->n_fields,
				extra, status, false);
		}
		ut_ad(tuple->n_fields <= index->n_fields);
		return rec_get_converted_size_comp_prefix_low(
			index, tuple->fields, tuple->n_fields,
			extra, status, false);
	case REC_STATUS_NODE_PTR:
		ut_ad(tuple->n_fields - 1
		      == dict_index_get_n_unique_in_tree_nonleaf(index));
		ut_ad(dfield_get_len(&tuple->fields[tuple->n_fields - 1])
		      == REC_NODE_PTR_SIZE);
		return REC_NODE_PTR_SIZE /* child page number */
			+ rec_get_converted_size_comp_prefix_low(
				index, tuple->fields, tuple->n_fields - 1,
				extra, status, false);
	case REC_STATUS_INFIMUM:
	case REC_STATUS_SUPREMUM:
		/* not supported */
		break;
	}

	ut_error;
	return(ULINT_UNDEFINED);
}

/*********************************************************//**
Builds an old-style physical record out of a data tuple and
stores it beginning from the start of the given buffer.
@return pointer to the origin of physical record */
static
rec_t*
rec_convert_dtuple_to_rec_old(
/*==========================*/
	byte*		buf,	/*!< in: start address of the physical record */
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	ulint		n_ext)	/*!< in: number of externally stored columns */
{
	const dfield_t*	field;
	ulint		n_fields;
	ulint		data_size;
	rec_t*		rec;
	ulint		end_offset;
	ulint		ored_offset;
	ulint		len;
	ulint		i;

	ut_ad(buf && dtuple);
	ut_ad(dtuple_validate(dtuple));
	ut_ad(dtuple_check_typed(dtuple));

	n_fields = dtuple_get_n_fields(dtuple);
	data_size = dtuple_get_data_size(dtuple, 0);

	ut_ad(n_fields > 0);

	/* Calculate the offset of the origin in the physical record */

	rec = buf + rec_get_converted_extra_size(data_size, n_fields, n_ext);
	/* Store the number of fields */
	rec_set_n_fields_old(rec, n_fields);

	/* Set the info bits of the record */
	rec_set_bit_field_1(rec,
			    dtuple_get_info_bits(dtuple) & REC_INFO_BITS_MASK,
			    REC_OLD_INFO_BITS,
			    REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
	rec_set_bit_field_2(rec, PAGE_HEAP_NO_USER_LOW, REC_OLD_HEAP_NO,
			    REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);

	/* Store the data and the offsets */

	end_offset = 0;

	if (!n_ext && data_size <= REC_1BYTE_OFFS_LIMIT) {

		rec_set_1byte_offs_flag(rec, TRUE);

		for (i = 0; i < n_fields; i++) {

			field = dtuple_get_nth_field(dtuple, i);

			if (dfield_is_null(field)) {
				len = dtype_get_sql_null_size(
					dfield_get_type(field), 0);
				data_write_sql_null(rec + end_offset, len);

				end_offset += len;
				ored_offset = end_offset
					| REC_1BYTE_SQL_NULL_MASK;
			} else {
				/* If the data is not SQL null, store it */
				len = dfield_get_len(field);

				if (len)
				  memcpy(rec + end_offset,
					 dfield_get_data(field), len);

				end_offset += len;
				ored_offset = end_offset;
			}

			rec_1_set_field_end_info(rec, i, ored_offset);
		}
	} else {
		rec_set_1byte_offs_flag(rec, FALSE);

		for (i = 0; i < n_fields; i++) {

			field = dtuple_get_nth_field(dtuple, i);

			if (dfield_is_null(field)) {
				len = dtype_get_sql_null_size(
					dfield_get_type(field), 0);
				data_write_sql_null(rec + end_offset, len);

				end_offset += len;
				ored_offset = end_offset
					| REC_2BYTE_SQL_NULL_MASK;
			} else {
				/* If the data is not SQL null, store it */
				len = dfield_get_len(field);

				if (len)
				   memcpy(rec + end_offset,
					  dfield_get_data(field), len);

				end_offset += len;
				ored_offset = end_offset;

				if (dfield_is_ext(field)) {
					ored_offset |= REC_2BYTE_EXTERN_MASK;
				}
			}

			rec_2_set_field_end_info(rec, i, ored_offset);
		}
	}

	return(rec);
}

/** Convert a data tuple into a ROW_FORMAT=COMPACT record.
@tparam	mblob	        whether the record includes a metadata BLOB
@tparam redundant_temp  whether to use the ROW_FORMAT=REDUNDANT format
@param[out]	rec		converted record
@param[in]	index		index
@param[in]	field		data fields to convert
@param[in]	n_fields	number of data fields
@param[in]	status		rec_get_status(rec)
@param[in]	temp		whether to use the format for temporary files
				in index creation */
template<bool mblob = false, bool redundant_temp = false>
static inline
void
rec_convert_dtuple_to_rec_comp(
	rec_t*			rec,
	const dict_index_t*	index,
	const dfield_t*		field,
	ulint			n_fields,
	rec_comp_status_t	status,
	bool			temp)
{
	byte*		end;
	byte*		nulls = temp
		? rec - 1 : rec - (REC_N_NEW_EXTRA_BYTES + 1);
	byte*		UNINIT_VAR(lens);
	ulint		UNINIT_VAR(n_node_ptr_field);
	ulint		null_mask	= 1;
	const ulint	n_core_fields = redundant_temp
			? row_log_get_n_core_fields(index)
			: index->n_core_fields;
	ut_ad(n_fields > 0);
	ut_ad(temp || dict_table_is_comp(index->table));
	ut_ad(index->n_core_null_bytes <= UT_BITS_IN_BYTES(index->n_nullable));

	ut_d(ulint n_null = index->n_nullable);

	if (mblob) {
		ut_ad(!temp);
		ut_ad(index->table->instant);
		ut_ad(!redundant_temp && index->is_instant());
		ut_ad(status == REC_STATUS_INSTANT);
		ut_ad(n_fields == ulint(index->n_fields) + 1);
		rec_set_n_add_field(nulls, n_fields - 1 - n_core_fields);
		rec_set_bit_field_2(rec, PAGE_HEAP_NO_USER_LOW,
				    REC_NEW_HEAP_NO, REC_HEAP_NO_MASK,
				    REC_HEAP_NO_SHIFT);
		rec_set_status(rec, REC_STATUS_INSTANT);
		n_node_ptr_field = ULINT_UNDEFINED;
		lens = nulls - UT_BITS_IN_BYTES(index->n_nullable);
		goto start;
	}
	switch (status) {
	case REC_STATUS_INSTANT:
		if (!redundant_temp) { ut_ad(index->is_instant()); }
		ut_ad(n_fields > n_core_fields);
		rec_set_n_add_field(nulls, n_fields - 1 - n_core_fields);
		/* fall through */
	case REC_STATUS_ORDINARY:
		ut_ad(n_fields <= dict_index_get_n_fields(index));
		if (!temp) {
			rec_set_bit_field_2(rec, PAGE_HEAP_NO_USER_LOW,
					    REC_NEW_HEAP_NO, REC_HEAP_NO_MASK,
					    REC_HEAP_NO_SHIFT);
			rec_set_status(rec, n_fields == n_core_fields
				       ? REC_STATUS_ORDINARY
				       : REC_STATUS_INSTANT);
		}

		if (dict_table_is_comp(index->table)) {
			/* No need to do adjust fixed_len=0. We only
			need to adjust it for ROW_FORMAT=REDUNDANT. */
			temp = false;
		}

		n_node_ptr_field = ULINT_UNDEFINED;

		lens = nulls - (index->is_instant()
				? UT_BITS_IN_BYTES(index->get_n_nullable(
					n_fields))
				: UT_BITS_IN_BYTES(
					unsigned(index->n_nullable)));
		break;
	case REC_STATUS_NODE_PTR:
		ut_ad(!temp);
		rec_set_bit_field_2(rec, PAGE_HEAP_NO_USER_LOW,
				    REC_NEW_HEAP_NO, REC_HEAP_NO_MASK,
				    REC_HEAP_NO_SHIFT);
		rec_set_status(rec, status);
		ut_ad(n_fields - 1
		      == dict_index_get_n_unique_in_tree_nonleaf(index));
		ut_d(n_null = std::min<uint>(index->n_core_null_bytes * 8U,
					     index->n_nullable));
		n_node_ptr_field = n_fields - 1;
		lens = nulls - index->n_core_null_bytes;
		break;
	case REC_STATUS_INFIMUM:
	case REC_STATUS_SUPREMUM:
		ut_error;
		return;
	}

start:
	end = rec;
	/* clear the SQL-null flags */
	memset(lens + 1, 0, ulint(nulls - lens));

	const dfield_t* const fend = field + n_fields;
	/* Store the data and the offsets */
	for (ulint i = 0; field < fend; i++, field++) {
		ulint len = dfield_get_len(field);

		if (mblob) {
			if (i == index->first_user_field()) {
				ut_ad(len == FIELD_REF_SIZE);
				ut_ad(dfield_is_ext(field));
				memcpy(end, dfield_get_data(field), len);
				end += len;
				if (++field == fend) {
					ut_ad(i == index->n_fields);
					break;
				}
				len = dfield_get_len(field);
			}
		} else if (UNIV_UNLIKELY(i == n_node_ptr_field)) {
			ut_ad(field->type.prtype & DATA_NOT_NULL);
			ut_ad(len == REC_NODE_PTR_SIZE);
			memcpy(end, dfield_get_data(field), len);
			end += REC_NODE_PTR_SIZE;
			break;
		}

		if (!(field->type.prtype & DATA_NOT_NULL)) {
			/* nullable field */
			ut_ad(n_null--);

			if (UNIV_UNLIKELY(!(byte) null_mask)) {
				nulls--;
				null_mask = 1;
			}

			ut_ad(*nulls < null_mask);

			/* set the null flag if necessary */
			if (dfield_is_null(field)) {
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 5 may need this here */
#endif
				*nulls |= static_cast<byte>(null_mask);
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
				null_mask <<= 1;
				continue;
			}

			null_mask <<= 1;
		}
		/* only nullable fields can be null */
		ut_ad(!dfield_is_null(field));

		const dict_field_t* ifield
			= dict_index_get_nth_field(index, i);
		ulint fixed_len = ifield->fixed_len;

		if (temp && fixed_len
		    && !dict_col_get_fixed_size(ifield->col, temp)) {
			fixed_len = 0;
		}

		/* If the maximum length of a variable-length field
		is up to 255 bytes, the actual length is always stored
		in one byte. If the maximum length is more than 255
		bytes, the actual length is stored in one byte for
		0..127.  The length will be encoded in two bytes when
		it is 128 or more, or when the field is stored externally. */
		if (fixed_len) {
			ut_ad(len <= fixed_len);
			ut_ad(!ifield->col->mbmaxlen
			      || len >= ifield->col->mbminlen
			      * fixed_len / ifield->col->mbmaxlen);
			ut_ad(!dfield_is_ext(field));
		} else if (dfield_is_ext(field)) {
			ut_ad(DATA_BIG_COL(ifield->col));
			ut_ad(len <= REC_ANTELOPE_MAX_INDEX_COL_LEN
					+ BTR_EXTERN_FIELD_REF_SIZE);
			*lens-- = static_cast<byte>(len >> 8 | 0xc0);
			*lens-- = static_cast<byte>(len);
		} else {
			ut_ad(len <= field->type.len
			      || DATA_LARGE_MTYPE(field->type.mtype)
			      || !strcmp(index->name,
					 FTS_INDEX_TABLE_IND_NAME));
			if (len < 128 || !DATA_BIG_LEN_MTYPE(
				    field->type.len, field->type.mtype)) {
				*lens-- = static_cast<byte>(len);
			} else {
				ut_ad(len < 16384);
				*lens-- = static_cast<byte>(len >> 8 | 0x80);
				*lens-- = static_cast<byte>(len);
			}
		}

		if (len) {
			memcpy(end, dfield_get_data(field), len);
			end += len;
		}
	}
}

/*********************************************************//**
Builds a new-style physical record out of a data tuple and
stores it beginning from the start of the given buffer.
@return pointer to the origin of physical record */
static
rec_t*
rec_convert_dtuple_to_rec_new(
/*==========================*/
	byte*			buf,	/*!< in: start address of
					the physical record */
	const dict_index_t*	index,	/*!< in: record descriptor */
	const dtuple_t*		dtuple)	/*!< in: data tuple */
{
	ut_ad(!(dtuple->info_bits
		& ~(REC_NEW_STATUS_MASK | REC_INFO_DELETED_FLAG
		    | REC_INFO_MIN_REC_FLAG)));

	ulint	extra_size;

	if (UNIV_UNLIKELY(dtuple->is_alter_metadata())) {
		ut_ad((dtuple->info_bits & REC_NEW_STATUS_MASK)
		      == REC_STATUS_INSTANT);
		rec_get_converted_size_comp_prefix_low<true>(
			index, dtuple->fields, dtuple->n_fields,
			&extra_size, REC_STATUS_INSTANT, false);
		buf += extra_size;
		rec_convert_dtuple_to_rec_comp<true>(
			buf, index, dtuple->fields, dtuple->n_fields,
			REC_STATUS_INSTANT, false);
	} else {
		rec_get_converted_size_comp(index, dtuple, &extra_size);
		buf += extra_size;
		rec_comp_status_t status = rec_comp_status_t(
			dtuple->info_bits & REC_NEW_STATUS_MASK);
		if (status == REC_STATUS_ORDINARY
		    && dtuple->n_fields > index->n_core_fields) {
			ut_ad(index->is_instant());
			status = REC_STATUS_INSTANT;
		}

		rec_convert_dtuple_to_rec_comp(
			buf, index, dtuple->fields, dtuple->n_fields,
			status, false);
	}

	rec_set_bit_field_1(buf, dtuple->info_bits & ~REC_NEW_STATUS_MASK,
			    REC_NEW_INFO_BITS,
			    REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
	return buf;
}

/*********************************************************//**
Builds a physical record out of a data tuple and
stores it beginning from the start of the given buffer.
@return pointer to the origin of physical record */
rec_t*
rec_convert_dtuple_to_rec(
/*======================*/
	byte*			buf,	/*!< in: start address of the
					physical record */
	const dict_index_t*	index,	/*!< in: record descriptor */
	const dtuple_t*		dtuple,	/*!< in: data tuple */
	ulint			n_ext)	/*!< in: number of
					externally stored columns */
{
	rec_t*	rec;

	ut_ad(buf != NULL);
	ut_ad(index != NULL);
	ut_ad(dtuple != NULL);
	ut_ad(dtuple_validate(dtuple));
	ut_ad(dtuple_check_typed(dtuple));

	if (dict_table_is_comp(index->table)) {
		rec = rec_convert_dtuple_to_rec_new(buf, index, dtuple);
	} else {
		rec = rec_convert_dtuple_to_rec_old(buf, dtuple, n_ext);
	}

	return(rec);
}

/** Determine the size of a data tuple prefix in a temporary file.
@tparam redundant_temp whether to use the ROW_FORMAT=REDUNDANT format
@param[in]	index		clustered or secondary index
@param[in]	fields		data fields
@param[in]	n_fields	number of data fields
@param[out]	extra		record header size
@param[in]	status		REC_STATUS_ORDINARY or REC_STATUS_INSTANT
@return	total size, in bytes */
template<bool redundant_temp>
ulint
rec_get_converted_size_temp(
	const dict_index_t*	index,
	const dfield_t*		fields,
	ulint			n_fields,
	ulint*			extra,
	rec_comp_status_t	status)
{
	return rec_get_converted_size_comp_prefix_low<false,redundant_temp>(
		index, fields, n_fields, extra, status, true);
}

template ulint rec_get_converted_size_temp<false>(
	const dict_index_t*, const dfield_t*, ulint, ulint*,
	rec_comp_status_t);

template ulint rec_get_converted_size_temp<true>(
	const dict_index_t*, const dfield_t*, ulint, ulint*,
	rec_comp_status_t);

/** Determine the offset to each field in temporary file.
@param[in]	rec	temporary file record
@param[in]	index	index of that the record belongs to
@param[in,out]	offsets	offsets to the fields; in: rec_offs_n_fields(offsets)
@param[in]	n_core	number of core fields (index->n_core_fields)
@param[in]	def_val	default values for non-core fields
@param[in]	status	REC_STATUS_ORDINARY or REC_STATUS_INSTANT */
void
rec_init_offsets_temp(
	const rec_t*		rec,
	const dict_index_t*	index,
	rec_offs*		offsets,
	ulint			n_core,
	const dict_col_t::def_t*def_val,
	rec_comp_status_t	status)
{
	ut_ad(status == REC_STATUS_ORDINARY
	      || status == REC_STATUS_INSTANT);
	/* The table may have been converted to plain format
	if it was emptied during an ALTER TABLE operation. */
	ut_ad(index->n_core_fields == n_core || !index->is_instant());
	ut_ad(index->n_core_fields >= n_core);
	if (index->table->not_redundant()) {
		rec_init_offsets_comp_ordinary(
			rec, index, offsets, n_core, def_val,
			status == REC_STATUS_INSTANT
			? REC_LEAF_TEMP_INSTANT
			: REC_LEAF_TEMP);
	} else {
		rec_init_offsets_comp_ordinary<false, true>(
			rec, index, offsets, n_core, def_val,
			status == REC_STATUS_INSTANT
			? REC_LEAF_TEMP_INSTANT
			: REC_LEAF_TEMP);
	}
}

/** Determine the offset to each field in temporary file.
@param[in]	rec	temporary file record
@param[in]	index	index of that the record belongs to
@param[in,out]	offsets	offsets to the fields; in: rec_offs_n_fields(offsets)
*/
void
rec_init_offsets_temp(
	const rec_t*		rec,
	const dict_index_t*	index,
	rec_offs*		offsets)
{
	ut_ad(!index->is_instant());
	if (index->table->not_redundant()) {
		rec_init_offsets_comp_ordinary(
			rec, index, offsets,
			index->n_core_fields, NULL, REC_LEAF_TEMP);
	} else {
		rec_init_offsets_comp_ordinary<false, true>(
			rec, index, offsets,
			index->n_core_fields, NULL, REC_LEAF_TEMP);
	}
}

/** Convert a data tuple prefix to the temporary file format.
@param[out]	rec		record in temporary file format
@param[in]	index		clustered or secondary index
@param[in]	fields		data fields
@param[in]	n_fields	number of data fields
@param[in]	status		REC_STATUS_ORDINARY or REC_STATUS_INSTANT
*/
template<bool redundant_temp>
void
rec_convert_dtuple_to_temp(
	rec_t*			rec,
	const dict_index_t*	index,
	const dfield_t*		fields,
	ulint			n_fields,
	rec_comp_status_t	status)
{
	rec_convert_dtuple_to_rec_comp<false,redundant_temp>(
		rec, index, fields, n_fields, status, true);
}

template void rec_convert_dtuple_to_temp<false>(
	rec_t*, const dict_index_t*, const dfield_t*,
	ulint, rec_comp_status_t);

template void rec_convert_dtuple_to_temp<true>(
	rec_t*, const dict_index_t*, const dfield_t*,
	ulint, rec_comp_status_t);

/** Copy the first n fields of a (copy of a) physical record to a data tuple.
The fields are copied into the memory heap.
@param[out]	tuple		data tuple
@param[in]	rec		index record, or a copy thereof
@param[in]	index		index of rec
@param[in]	n_core		index->n_core_fields at the time rec was
				copied, or 0 if non-leaf page record
@param[in]	n_fields	number of fields to copy
@param[in,out]	heap		memory heap */
void
rec_copy_prefix_to_dtuple(
	dtuple_t*		tuple,
	const rec_t*		rec,
	const dict_index_t*	index,
	ulint			n_core,
	ulint			n_fields,
	mem_heap_t*		heap)
{
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets	= offsets_;
	rec_offs_init(offsets_);

	ut_ad(n_core <= index->n_core_fields);
	ut_ad(n_core || n_fields - 1
	      <= dict_index_get_n_unique_in_tree_nonleaf(index));

	offsets = rec_get_offsets(rec, index, offsets, n_core,
				  n_fields, &heap);

	ut_ad(rec_validate(rec, offsets));
	ut_ad(!rec_offs_any_default(offsets));
	ut_ad(dtuple_check_typed(tuple));

	tuple->info_bits = rec_get_info_bits(rec, rec_offs_comp(offsets));

	for (ulint i = 0; i < n_fields; i++) {
		dfield_t*	field;
		const byte*	data;
		ulint		len;

		field = dtuple_get_nth_field(tuple, i);
		data = rec_get_nth_field(rec, offsets, i, &len);

		if (len != UNIV_SQL_NULL) {
			dfield_set_data(field,
					mem_heap_dup(heap, data, len), len);
			ut_ad(!rec_offs_nth_extern(offsets, i));
		} else {
			dfield_set_null(field);
		}
	}
}

/**************************************************************//**
Copies the first n fields of an old-style physical record
to a new physical record in a buffer.
@return own: copied record */
static
rec_t*
rec_copy_prefix_to_buf_old(
/*=======================*/
	const rec_t*	rec,		/*!< in: physical record */
	ulint		n_fields,	/*!< in: number of fields to copy */
	ulint		area_end,	/*!< in: end of the prefix data */
	byte**		buf,		/*!< in/out: memory buffer for
					the copied prefix, or NULL */
	ulint*		buf_size)	/*!< in/out: buffer size */
{
	rec_t*	copy_rec;
	ulint	area_start;
	ulint	prefix_len;

	if (rec_get_1byte_offs_flag(rec)) {
		area_start = REC_N_OLD_EXTRA_BYTES + n_fields;
	} else {
		area_start = REC_N_OLD_EXTRA_BYTES + 2 * n_fields;
	}

	prefix_len = area_start + area_end;

	if ((*buf == NULL) || (*buf_size < prefix_len)) {
		ut_free(*buf);
		*buf_size = prefix_len;
		*buf = static_cast<byte*>(ut_malloc_nokey(prefix_len));
	}

	memcpy(*buf, rec - area_start, prefix_len);

	copy_rec = *buf + area_start;

	rec_set_n_fields_old(copy_rec, n_fields);

	return(copy_rec);
}

/**************************************************************//**
Copies the first n fields of a physical record to a new physical record in
a buffer.
@return own: copied record */
rec_t*
rec_copy_prefix_to_buf(
/*===================*/
	const rec_t*		rec,		/*!< in: physical record */
	const dict_index_t*	index,		/*!< in: record descriptor */
	ulint			n_fields,	/*!< in: number of fields
						to copy */
	byte**			buf,		/*!< in/out: memory buffer
						for the copied prefix,
						or NULL */
	ulint*			buf_size)	/*!< in/out: buffer size */
{
	ut_ad(n_fields <= index->n_fields || dict_index_is_ibuf(index));
	ut_ad(index->n_core_null_bytes <= UT_BITS_IN_BYTES(index->n_nullable));
	UNIV_PREFETCH_RW(*buf);

	if (!dict_table_is_comp(index->table)) {
		ut_ad(rec_validate_old(rec));
		return(rec_copy_prefix_to_buf_old(
			       rec, n_fields,
			       rec_get_field_start_offs(rec, n_fields),
			       buf, buf_size));
	}

	ulint		prefix_len	= 0;
	ulint		instant_omit	= 0;
	const byte*	nulls		= rec - (REC_N_NEW_EXTRA_BYTES + 1);
	const byte*	nullf		= nulls;
	const byte*	lens		= nulls - index->n_core_null_bytes;

	switch (rec_get_status(rec)) {
	default:
		/* infimum or supremum record: no sense to copy anything */
		ut_error;
		return(NULL);
	case REC_STATUS_ORDINARY:
		ut_ad(n_fields <= index->n_core_fields);
		break;
	case REC_STATUS_NODE_PTR:
		/* For R-tree, we need to copy the child page number field. */
		compile_time_assert(DICT_INDEX_SPATIAL_NODEPTR_SIZE == 1);
		if (dict_index_is_spatial(index)) {
			ut_ad(index->n_core_null_bytes == 0);
			ut_ad(n_fields == DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1);
			ut_ad(index->fields[0].col->prtype & DATA_NOT_NULL);
			ut_ad(DATA_BIG_COL(index->fields[0].col));
			/* This is a deficiency of the format introduced
			in MySQL 5.7. The length in the R-tree index should
			always be DATA_MBR_LEN. */
			ut_ad(!index->fields[0].fixed_len);
			ut_ad(*lens == DATA_MBR_LEN);
			lens--;
			prefix_len = DATA_MBR_LEN + REC_NODE_PTR_SIZE;
			n_fields = 0; /* skip the "for" loop below */
			break;
		}
		/* it doesn't make sense to copy the child page number field */
		ut_ad(n_fields
		      <= dict_index_get_n_unique_in_tree_nonleaf(index));
		break;
	case REC_STATUS_INSTANT:
		/* We would have !index->is_instant() when rolling back
		an instant ADD COLUMN operation. */
		ut_ad(index->is_instant() || page_rec_is_metadata(rec));
		ut_ad(n_fields <= index->first_user_field());
		nulls++;
		const ulint n_rec = ulint(index->n_core_fields) + 1
			+ rec_get_n_add_field(nulls)
			- rec_is_alter_metadata(rec, true);
		instant_omit = ulint(&rec[-REC_N_NEW_EXTRA_BYTES] - nulls);
		ut_ad(instant_omit == 1 || instant_omit == 2);
		nullf = nulls;
		const uint nb = UT_BITS_IN_BYTES(index->get_n_nullable(n_rec));
		instant_omit += nb - index->n_core_null_bytes;
		lens = --nulls - nb;
	}

	const byte* const lenf = lens;
	UNIV_PREFETCH_R(lens);

	/* read the lengths of fields 0..n */
	for (ulint i = 0, null_mask = 1; i < n_fields; i++) {
		const dict_field_t*	field;
		const dict_col_t*	col;

		field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(field);

		if (!(col->prtype & DATA_NOT_NULL)) {
			/* nullable field => read the null flag */
			if (UNIV_UNLIKELY(!(byte) null_mask)) {
				nulls--;
				null_mask = 1;
			}

			if (*nulls & null_mask) {
				null_mask <<= 1;
				continue;
			}

			null_mask <<= 1;
		}

		if (field->fixed_len) {
			prefix_len += field->fixed_len;
		} else {
			ulint	len = *lens--;
			/* If the maximum length of the column is up
			to 255 bytes, the actual length is always
			stored in one byte. If the maximum length is
			more than 255 bytes, the actual length is
			stored in one byte for 0..127.  The length
			will be encoded in two bytes when it is 128 or
			more, or when the column is stored externally. */
			if (DATA_BIG_COL(col)) {
				if (len & 0x80) {
					/* 1exxxxxx */
					len &= 0x3f;
					len <<= 8;
					len |= *lens--;
					UNIV_PREFETCH_R(lens);
				}
			}
			prefix_len += len;
		}
	}

	UNIV_PREFETCH_R(rec + prefix_len);

	ulint size = prefix_len + ulint(rec - (lens + 1)) - instant_omit;

	if (*buf == NULL || *buf_size < size) {
		ut_free(*buf);
		*buf_size = size;
		*buf = static_cast<byte*>(ut_malloc_nokey(size));
	}

	if (instant_omit) {
		/* Copy and convert the record header to a format where
		instant ADD COLUMN has not been used:
		+ lengths of variable-length fields in the prefix
		- omit any null flag bytes for any instantly added columns
		+ index->n_core_null_bytes of null flags
		- omit the n_add_fields header (1 or 2 bytes)
		+ REC_N_NEW_EXTRA_BYTES of fixed header */
		byte* b = *buf;
		/* copy the lengths of the variable-length fields */
		memcpy(b, lens + 1, ulint(lenf - lens));
		b += ulint(lenf - lens);
		/* copy the null flags */
		memcpy(b, nullf - index->n_core_null_bytes,
		       index->n_core_null_bytes);
		b += index->n_core_null_bytes + REC_N_NEW_EXTRA_BYTES;
		ut_ad(ulint(b - *buf) + prefix_len == size);
		/* copy the fixed-size header and the record prefix */
		memcpy(b - REC_N_NEW_EXTRA_BYTES, rec - REC_N_NEW_EXTRA_BYTES,
		       prefix_len + REC_N_NEW_EXTRA_BYTES);
		ut_ad(rec_get_status(b) == REC_STATUS_INSTANT);
		rec_set_status(b, REC_STATUS_ORDINARY);
		return b;
	} else {
		memcpy(*buf, lens + 1, size);
		return *buf + (rec - (lens + 1));
	}
}

/***************************************************************//**
Validates the consistency of an old-style physical record.
@return TRUE if ok */
static
ibool
rec_validate_old(
/*=============*/
	const rec_t*	rec)	/*!< in: physical record */
{
	ulint		len;
	ulint		n_fields;
	ulint		len_sum		= 0;
	ulint		i;

	ut_a(rec);
	n_fields = rec_get_n_fields_old(rec);

	if ((n_fields == 0) || (n_fields > REC_MAX_N_FIELDS)) {
		ib::error() << "Record has " << n_fields << " fields";
		return(FALSE);
	}

	for (i = 0; i < n_fields; i++) {
		rec_get_nth_field_offs_old(rec, i, &len);

		if (!((len < srv_page_size) || (len == UNIV_SQL_NULL))) {
			ib::error() << "Record field " << i << " len " << len;
			return(FALSE);
		}

		if (len != UNIV_SQL_NULL) {
			len_sum += len;
		} else {
			len_sum += rec_get_nth_field_size(rec, i);
		}
	}

	if (len_sum != rec_get_data_size_old(rec)) {
		ib::error() << "Record len should be " << len_sum << ", len "
			<< rec_get_data_size_old(rec);
		return(FALSE);
	}

	return(TRUE);
}

/***************************************************************//**
Validates the consistency of a physical record.
@return TRUE if ok */
ibool
rec_validate(
/*=========*/
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint		len;
	ulint		n_fields;
	ulint		len_sum		= 0;
	ulint		i;

	n_fields = rec_offs_n_fields(offsets);

	if ((n_fields == 0) || (n_fields > REC_MAX_N_FIELDS)) {
		ib::error() << "Record has " << n_fields << " fields";
		return(FALSE);
	}

	ut_a(rec_offs_any_flag(offsets, REC_OFFS_COMPACT | REC_OFFS_DEFAULT)
	     || n_fields <= rec_get_n_fields_old(rec));

	for (i = 0; i < n_fields; i++) {
		rec_get_nth_field_offs(offsets, i, &len);

		switch (len) {
		default:
			if (len >= srv_page_size) {
				ib::error() << "Record field " << i
					<< " len " << len;
				return(FALSE);
			}
			len_sum += len;
			break;
		case UNIV_SQL_DEFAULT:
			break;
		case UNIV_SQL_NULL:
			if (!rec_offs_comp(offsets)) {
				len_sum += rec_get_nth_field_size(rec, i);
			}
		}
	}

	if (len_sum != rec_offs_data_size(offsets)) {
		ib::error() << "Record len should be " << len_sum << ", len "
			<< rec_offs_data_size(offsets);
		return(FALSE);
	}

	if (!rec_offs_comp(offsets)) {
		ut_a(rec_validate_old(rec));
	}

	return(TRUE);
}

/***************************************************************//**
Prints an old-style physical record. */
void
rec_print_old(
/*==========*/
	FILE*		file,	/*!< in: file where to print */
	const rec_t*	rec)	/*!< in: physical record */
{
	const byte*	data;
	ulint		len;
	ulint		n;
	ulint		i;

	n = rec_get_n_fields_old(rec);

	fprintf(file, "PHYSICAL RECORD: n_fields " ULINTPF ";"
		" %u-byte offsets; info bits %u\n",
		n,
		rec_get_1byte_offs_flag(rec) ? 1 : 2,
		rec_get_info_bits(rec, FALSE));

	for (i = 0; i < n; i++) {

		data = rec_get_nth_field_old(rec, i, &len);

		fprintf(file, " " ULINTPF ":", i);

		if (len != UNIV_SQL_NULL) {
			if (len <= 30) {

				ut_print_buf(file, data, len);
			} else {
				ut_print_buf(file, data, 30);

				fprintf(file, " (total " ULINTPF " bytes)",
					len);
			}
		} else {
			fprintf(file, " SQL NULL, size " ULINTPF " ",
				rec_get_nth_field_size(rec, i));
		}

		putc(';', file);
		putc('\n', file);
	}

	rec_validate_old(rec);
}

/***************************************************************//**
Prints a physical record in ROW_FORMAT=COMPACT.  Ignores the
record header. */
static
void
rec_print_comp(
/*===========*/
	FILE*		file,	/*!< in: file where to print */
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	i;

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
          const byte*	UNINIT_VAR(data);
		ulint		len;

		if (rec_offs_nth_default(offsets, i)) {
			len = UNIV_SQL_DEFAULT;
		} else {
			data = rec_get_nth_field(rec, offsets, i, &len);
		}

		fprintf(file, " " ULINTPF ":", i);

		if (len == UNIV_SQL_NULL) {
			fputs(" SQL NULL", file);
		} else if (len == UNIV_SQL_DEFAULT) {
			fputs(" SQL DEFAULT", file);
		} else {
			if (len <= 30) {

				ut_print_buf(file, data, len);
			} else if (rec_offs_nth_extern(offsets, i)) {
				ut_print_buf(file, data, 30);
				fprintf(file,
					" (total " ULINTPF " bytes, external)",
					len);
				ut_print_buf(file, data + len
					     - BTR_EXTERN_FIELD_REF_SIZE,
					     BTR_EXTERN_FIELD_REF_SIZE);
			} else {
				ut_print_buf(file, data, 30);

				fprintf(file, " (total " ULINTPF " bytes)",
					len);
			}
		}
		putc(';', file);
		putc('\n', file);
	}
}

/***************************************************************//**
Prints an old-style spatial index record. */
static
void
rec_print_mbr_old(
/*==============*/
	FILE*		file,	/*!< in: file where to print */
	const rec_t*	rec)	/*!< in: physical record */
{
	const byte*	data;
	ulint		len;
	ulint		n;
	ulint		i;

	ut_ad(rec);

	n = rec_get_n_fields_old(rec);

	fprintf(file, "PHYSICAL RECORD: n_fields %lu;"
		" %u-byte offsets; info bits %lu\n",
		(ulong) n,
		rec_get_1byte_offs_flag(rec) ? 1 : 2,
		(ulong) rec_get_info_bits(rec, FALSE));

	for (i = 0; i < n; i++) {

		data = rec_get_nth_field_old(rec, i, &len);

		fprintf(file, " %lu:", (ulong) i);

		if (len != UNIV_SQL_NULL) {
			if (i == 0) {
				fprintf(file, " MBR:");
				for (; len > 0; len -= sizeof(double)) {
					double	d = mach_double_read(data);

					if (len != sizeof(double)) {
						fprintf(file, "%.2lf,", d);
					} else {
						fprintf(file, "%.2lf", d);
					}

					data += sizeof(double);
				}
			} else {
				if (len <= 30) {

					ut_print_buf(file, data, len);
				} else {
					ut_print_buf(file, data, 30);

					fprintf(file, " (total %lu bytes)",
						(ulong) len);
				}
			}
		} else {
			fprintf(file, " SQL NULL, size " ULINTPF " ",
				rec_get_nth_field_size(rec, i));
		}

		putc(';', file);
		putc('\n', file);
	}

	if (rec_get_deleted_flag(rec, false)) {
		fprintf(file, " Deleted");
	}

	if (rec_get_info_bits(rec, true) & REC_INFO_MIN_REC_FLAG) {
		fprintf(file, " First rec");
	}

	rec_validate_old(rec);
}

/***************************************************************//**
Prints a spatial index record. */
void
rec_print_mbr_rec(
/*==============*/
	FILE*		file,	/*!< in: file where to print */
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(!rec_offs_any_default(offsets));

	if (!rec_offs_comp(offsets)) {
		rec_print_mbr_old(file, rec);
		return;
	}

	for (ulint i = 0; i < rec_offs_n_fields(offsets); i++) {
		const byte*	data;
		ulint		len;

		data = rec_get_nth_field(rec, offsets, i, &len);

		if (i == 0) {
			fprintf(file, " MBR:");
			for (; len > 0; len -= sizeof(double)) {
				double	d = mach_double_read(data);

				if (len != sizeof(double)) {
					fprintf(file, "%.2lf,", d);
				} else {
					fprintf(file, "%.2lf", d);
				}

				data += sizeof(double);
			}
		} else {
			fprintf(file, " %lu:", (ulong) i);

			if (len != UNIV_SQL_NULL) {
				if (len <= 30) {

					ut_print_buf(file, data, len);
				} else {
					ut_print_buf(file, data, 30);

					fprintf(file, " (total %lu bytes)",
						(ulong) len);
				}
			} else {
				fputs(" SQL NULL", file);
			}
		}
		putc(';', file);
	}

	if (rec_get_info_bits(rec, true) & REC_INFO_DELETED_FLAG) {
		fprintf(file, " Deleted");
	}

	if (rec_get_info_bits(rec, true) & REC_INFO_MIN_REC_FLAG) {
		fprintf(file, " First rec");
	}


	rec_validate(rec, offsets);
}

/***************************************************************//**
Prints a physical record. */
void
rec_print_new(
/*==========*/
	FILE*		file,	/*!< in: file where to print */
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ut_ad(rec_offs_validate(rec, NULL, offsets));

#ifdef UNIV_DEBUG
	if (rec_get_deleted_flag(rec, rec_offs_comp(offsets))) {
		DBUG_PRINT("info", ("deleted "));
	} else {
		DBUG_PRINT("info", ("not-deleted "));
	}
#endif /* UNIV_DEBUG */

	if (!rec_offs_comp(offsets)) {
		rec_print_old(file, rec);
		return;
	}

	fprintf(file, "PHYSICAL RECORD: n_fields " ULINTPF ";"
		" compact format; info bits %u\n",
		rec_offs_n_fields(offsets),
		rec_get_info_bits(rec, TRUE));

	rec_print_comp(file, rec, offsets);
	rec_validate(rec, offsets);
}

/***************************************************************//**
Prints a physical record. */
void
rec_print(
/*======*/
	FILE*			file,	/*!< in: file where to print */
	const rec_t*		rec,	/*!< in: physical record */
	const dict_index_t*	index)	/*!< in: record descriptor */
{
	if (!dict_table_is_comp(index->table)) {
		rec_print_old(file, rec);
		return;
	} else {
		mem_heap_t*	heap	= NULL;
		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		rec_offs_init(offsets_);

		rec_print_new(file, rec,
			      rec_get_offsets(rec, index, offsets_,
					      page_rec_is_leaf(rec)
					      ? index->n_core_fields : 0,
					      ULINT_UNDEFINED, &heap));
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	}
}

/** Pretty-print a record.
@param[in,out]	o	output stream
@param[in]	rec	physical record
@param[in]	info	rec_get_info_bits(rec)
@param[in]	offsets	rec_get_offsets(rec) */
void
rec_print(
	std::ostream&	o,
	const rec_t*	rec,
	ulint		info,
	const rec_offs*	offsets)
{
	const ulint	comp	= rec_offs_comp(offsets);
	const ulint	n	= rec_offs_n_fields(offsets);

	ut_ad(rec_offs_validate(rec, NULL, offsets));

	o << (comp ? "COMPACT RECORD" : "RECORD")
	  << "(info_bits=" << info << ", " << n << " fields): {";

	for (ulint i = 0; i < n; i++) {
		const byte*	data;
		ulint		len;

		if (i) {
			o << ',';
		}

		data = rec_get_nth_field(rec, offsets, i, &len);

		if (len == UNIV_SQL_DEFAULT) {
			o << "DEFAULT";
			continue;
		}

		if (len == UNIV_SQL_NULL) {
			o << "NULL";
			continue;
		}

		if (rec_offs_nth_extern(offsets, i)) {
			ulint	local_len = len - BTR_EXTERN_FIELD_REF_SIZE;
			ut_ad(len >= BTR_EXTERN_FIELD_REF_SIZE);

			o << '['
			  << local_len
			  << '+' << BTR_EXTERN_FIELD_REF_SIZE << ']';
			ut_print_buf(o, data, local_len);
			ut_print_buf_hex(o, data + local_len,
					 BTR_EXTERN_FIELD_REF_SIZE);
		} else {
			o << '[' << len << ']';
			ut_print_buf(o, data, len);
		}
	}

	o << "}";
}

/** Display a record.
@param[in,out]	o	output stream
@param[in]	r	record to display
@return	the output stream */
std::ostream&
operator<<(std::ostream& o, const rec_index_print& r)
{
	mem_heap_t*	heap	= NULL;
	rec_offs*	offsets	= rec_get_offsets(
		r.m_rec, r.m_index, NULL, page_rec_is_leaf(r.m_rec)
		? r.m_index->n_core_fields : 0,
		ULINT_UNDEFINED, &heap);
	rec_print(o, r.m_rec,
		  rec_get_info_bits(r.m_rec, rec_offs_comp(offsets)),
		  offsets);
	mem_heap_free(heap);
	return(o);
}

/** Display a record.
@param[in,out]	o	output stream
@param[in]	r	record to display
@return	the output stream */
std::ostream&
operator<<(std::ostream& o, const rec_offsets_print& r)
{
	rec_print(o, r.m_rec,
		  rec_get_info_bits(r.m_rec, rec_offs_comp(r.m_offsets)),
		  r.m_offsets);
	return(o);
}

#ifdef UNIV_DEBUG
/** Read the DB_TRX_ID of a clustered index record.
@param[in]	rec	clustered index record
@param[in]	index	clustered index
@return the value of DB_TRX_ID */
trx_id_t
rec_get_trx_id(
	const rec_t*		rec,
	const dict_index_t*	index)
{
	const byte*	trx_id;
	ulint		len;
	mem_heap_t*	heap		= NULL;
	rec_offs offsets_[REC_OFFS_HEADER_SIZE + MAX_REF_PARTS + 2];
	rec_offs_init(offsets_);
	rec_offs* offsets = offsets_;

	offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
				  index->db_trx_id() + 1, &heap);

	trx_id = rec_get_nth_field(rec, offsets, index->db_trx_id(), &len);

	ut_ad(len == DATA_TRX_ID_LEN);

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return(trx_read_trx_id(trx_id));
}
#endif /* UNIV_DEBUG */

/** Mark the nth field as externally stored.
@param[in]	offsets		array returned by rec_get_offsets()
@param[in]	n		nth field */
void
rec_offs_make_nth_extern(
	rec_offs*	offsets,
	const ulint	n)
{
	ut_ad(!rec_offs_nth_sql_null(offsets, n));
	set_type(rec_offs_base(offsets)[1 + n], STORED_OFFPAGE);
}
#ifdef WITH_WSREP
# include "ha_prototypes.h"

int
wsrep_rec_get_foreign_key(
	byte 		*buf,     /* out: extracted key */
	ulint 		*buf_len, /* in/out: length of buf */
	const rec_t*	rec,	  /* in: physical record */
	dict_index_t*	index_for,  /* in: index in foreign table */
	dict_index_t*	index_ref,  /* in: index in referenced table */
	ibool		new_protocol) /* in: protocol > 1 */
{
	const byte*	data;
	ulint		len;
	ulint		key_len = 0;
	ulint		i;
	uint            key_parts;
	mem_heap_t*	heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	const rec_offs* offsets;

	ut_ad(index_for);
	ut_ad(index_ref);

        rec_offs_init(offsets_);
	offsets = rec_get_offsets(rec, index_for, offsets_,
				  index_for->n_core_fields,
				  ULINT_UNDEFINED, &heap);

	ut_ad(rec_offs_validate(rec, NULL, offsets));

	ut_ad(rec);

	key_parts = dict_index_get_n_unique_in_tree(index_for);
	for (i = 0; 
	     i < key_parts && 
	       (index_for->type & DICT_CLUSTERED || i < key_parts - 1); 
	     i++) {
		dict_field_t*	  field_f = 
			dict_index_get_nth_field(index_for, i);
		const dict_col_t* col_f = dict_field_get_col(field_f);
                dict_field_t*	  field_r = 
			dict_index_get_nth_field(index_ref, i);
		const dict_col_t* col_r = dict_field_get_col(field_r);

		ut_ad(!rec_offs_nth_default(offsets, i));
		data = rec_get_nth_field(rec, offsets, i, &len);
		if (key_len + ((len != UNIV_SQL_NULL) ? len + 1 : 1) > 
		    *buf_len) {
			fprintf(stderr,
				"WSREP: FK key len exceeded "
				ULINTPF " " ULINTPF " " ULINTPF "\n",
				key_len, len, *buf_len);
			goto err_out;
		}

		if (len == UNIV_SQL_NULL) {
			ut_a(!(col_f->prtype & DATA_NOT_NULL));
			*buf++ = 1;
			key_len++;
		} else if (!new_protocol) {
			if (!(col_r->prtype & DATA_NOT_NULL)) {
				*buf++ = 0;
				key_len++;
			}
			memcpy(buf, data, len);
			*buf_len = wsrep_innobase_mysql_sort(
				(int)(col_f->prtype & DATA_MYSQL_TYPE_MASK),
				dtype_get_charset_coll(col_f->prtype),
				buf, static_cast<uint>(len),
				static_cast<uint>(*buf_len));
		} else { /* new protocol */
			if (!(col_r->prtype & DATA_NOT_NULL)) {
				*buf++ = 0;
				key_len++;
			}
			switch (col_f->mtype) {
			case DATA_INT: {
				byte* ptr = buf+len;
				for (;;) {
					ptr--;
					*ptr = *data;
					if (ptr == buf) {
						break;
					}
					data++;
				}

				if (!(col_f->prtype & DATA_UNSIGNED)) {
					buf[len-1] = (byte) (buf[len-1] ^ 128);
				}

				break;
			}
			case DATA_VARCHAR:
			case DATA_VARMYSQL:
			case DATA_CHAR:
			case DATA_MYSQL:
				/* Copy the actual data */
				memcpy(buf, data, len);
				len = wsrep_innobase_mysql_sort(
					(int)
					(col_f->prtype & DATA_MYSQL_TYPE_MASK),
					dtype_get_charset_coll(col_f->prtype),
					buf, len, *buf_len);
				break;
			case DATA_BLOB:
			case DATA_BINARY:
			case DATA_FIXBINARY:
			case DATA_GEOMETRY:
				memcpy(buf, data, len);
				break;

			case DATA_FLOAT:
			{
				float f = mach_float_read(data);
				memcpy(buf, &f, sizeof(float));
			}
			break;
			case DATA_DOUBLE:
			{
				double d = mach_double_read(data);
				memcpy(buf, &d, sizeof(double));
			}
			break;
			default:
				break;
			}

			key_len += len;
			buf 	+= len;
		}
	}

	rec_validate(rec, offsets);

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	*buf_len = key_len;
	return DB_SUCCESS;

 err_out:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return DB_ERROR;
}
#endif // WITH_WSREP
