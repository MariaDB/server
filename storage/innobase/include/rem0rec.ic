/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file include/rem0rec.ic
Record manager

Created 5/30/1994 Heikki Tuuri
*************************************************************************/

#include "mach0data.h"
#include "ut0byte.h"
#include "dict0boot.h"
#include "btr0types.h"

/* Offsets of the bit-fields in an old-style record. NOTE! In the table the
most significant bytes and bits are written below less significant.

	(1) byte offset		(2) bit usage within byte
	downward from
	origin ->	1	8 bits pointer to next record
			2	8 bits pointer to next record
			3	1 bit short flag
				7 bits number of fields
			4	3 bits number of fields
				5 bits heap number
			5	8 bits heap number
			6	4 bits n_owned
				4 bits info bits
*/

/* Offsets of the bit-fields in a new-style record. NOTE! In the table the
most significant bytes and bits are written below less significant.

	(1) byte offset		(2) bit usage within byte
	downward from
	origin ->	1	8 bits relative offset of next record
			2	8 bits relative offset of next record
				  the relative offset is an unsigned 16-bit
				  integer:
				  (offset_of_next_record
				   - offset_of_this_record) mod 64Ki,
				  where mod is the modulo as a non-negative
				  number;
				  we can calculate the offset of the next
				  record with the formula:
				  relative_offset + offset_of_this_record
				  mod srv_page_size
			3	3 bits status:
					000=REC_STATUS_ORDINARY
					001=REC_STATUS_NODE_PTR
					010=REC_STATUS_INFIMUM
					011=REC_STATUS_SUPREMUM
					100=REC_STATUS_INSTANT
					1xx=reserved
				5 bits heap number
			4	8 bits heap number
			5	4 bits n_owned
				4 bits info bits
*/

/* We list the byte offsets from the origin of the record, the mask,
and the shift needed to obtain each bit-field of the record. */

#define REC_NEXT		2
#define REC_NEXT_MASK		0xFFFFUL
#define REC_NEXT_SHIFT		0

#define REC_OLD_SHORT		3	/* This is single byte bit-field */
#define REC_OLD_SHORT_MASK	0x1UL
#define REC_OLD_SHORT_SHIFT	0

#define REC_OLD_N_FIELDS	4
#define REC_OLD_N_FIELDS_MASK	0x7FEUL
#define REC_OLD_N_FIELDS_SHIFT	1

#define REC_OLD_HEAP_NO		5
#define REC_HEAP_NO_MASK	0xFFF8UL
#if 0 /* defined in rem0rec.h for use of page0zip.cc */
#define REC_NEW_HEAP_NO		4
#define	REC_HEAP_NO_SHIFT	3
#endif

#define REC_OLD_N_OWNED		6	/* This is single byte bit-field */
#define REC_NEW_N_OWNED		5	/* This is single byte bit-field */
#define	REC_N_OWNED_MASK	0xFUL
#define REC_N_OWNED_SHIFT	0

#define REC_OLD_INFO_BITS	6	/* This is single byte bit-field */
#define REC_NEW_INFO_BITS	5	/* This is single byte bit-field */
#define	REC_INFO_BITS_MASK	0xF0UL
#define REC_INFO_BITS_SHIFT	0

#if REC_OLD_SHORT_MASK << (8 * (REC_OLD_SHORT - 3)) \
		^ REC_OLD_N_FIELDS_MASK << (8 * (REC_OLD_N_FIELDS - 4)) \
		^ REC_HEAP_NO_MASK << (8 * (REC_OLD_HEAP_NO - 4)) \
		^ REC_N_OWNED_MASK << (8 * (REC_OLD_N_OWNED - 3)) \
		^ REC_INFO_BITS_MASK << (8 * (REC_OLD_INFO_BITS - 3)) \
		^ 0xFFFFFFFFUL
# error "sum of old-style masks != 0xFFFFFFFFUL"
#endif
#if REC_NEW_STATUS_MASK << (8 * (REC_NEW_STATUS - 3)) \
		^ REC_HEAP_NO_MASK << (8 * (REC_NEW_HEAP_NO - 4)) \
		^ REC_N_OWNED_MASK << (8 * (REC_NEW_N_OWNED - 3)) \
		^ REC_INFO_BITS_MASK << (8 * (REC_NEW_INFO_BITS - 3)) \
		^ 0xFFFFFFUL
# error "sum of new-style masks != 0xFFFFFFUL"
#endif

/******************************************************//**
Gets a bit field from within 1 byte. */
UNIV_INLINE
byte
rec_get_bit_field_1(
/*================*/
	const rec_t*	rec,	/*!< in: pointer to record origin */
	ulint		offs,	/*!< in: offset from the origin down */
	ulint		mask,	/*!< in: mask used to filter bits */
	ulint		shift)	/*!< in: shift right applied after masking */
{
  return static_cast<byte>((*(rec - offs) & mask) >> shift);
}

/******************************************************//**
Sets a bit field within 1 byte. */
UNIV_INLINE
void
rec_set_bit_field_1(
/*================*/
	rec_t*	rec,	/*!< in: pointer to record origin */
	ulint	val,	/*!< in: value to set */
	ulint	offs,	/*!< in: offset from the origin down */
	ulint	mask,	/*!< in: mask used to filter bits */
	ulint	shift)	/*!< in: shift right applied after masking */
{
	ut_ad(rec);
	ut_ad(offs <= REC_N_OLD_EXTRA_BYTES);
	ut_ad(mask);
	ut_ad(mask <= 0xFFUL);
	ut_ad(((mask >> shift) << shift) == mask);
	ut_ad(((val << shift) & mask) == (val << shift));

	mach_write_to_1(rec - offs,
			(mach_read_from_1(rec - offs) & ~mask)
			| (val << shift));
}

/******************************************************//**
Gets a bit field from within 2 bytes. */
UNIV_INLINE
ulint
rec_get_bit_field_2(
/*================*/
	const rec_t*	rec,	/*!< in: pointer to record origin */
	ulint		offs,	/*!< in: offset from the origin down */
	ulint		mask,	/*!< in: mask used to filter bits */
	ulint		shift)	/*!< in: shift right applied after masking */
{
	ut_ad(rec);

	return((mach_read_from_2(rec - offs) & mask) >> shift);
}

/******************************************************//**
Sets a bit field within 2 bytes. */
UNIV_INLINE
void
rec_set_bit_field_2(
/*================*/
	rec_t*	rec,	/*!< in: pointer to record origin */
	ulint	val,	/*!< in: value to set */
	ulint	offs,	/*!< in: offset from the origin down */
	ulint	mask,	/*!< in: mask used to filter bits */
	ulint	shift)	/*!< in: shift right applied after masking */
{
	ut_ad(rec);
	ut_ad(offs <= REC_N_OLD_EXTRA_BYTES);
	ut_ad(mask > 0xFFUL);
	ut_ad(mask <= 0xFFFFUL);
	ut_ad((mask >> shift) & 1);
	ut_ad(0 == ((mask >> shift) & ((mask >> shift) + 1)));
	ut_ad(((mask >> shift) << shift) == mask);
	ut_ad(((val << shift) & mask) == (val << shift));

	mach_write_to_2(rec - offs,
			(mach_read_from_2(rec - offs) & ~mask)
			| (val << shift));
}

/******************************************************//**
The following function is used to get the pointer of the next chained record
on the same page.
@return pointer to the next chained record, or NULL if none */
UNIV_INLINE
const rec_t*
rec_get_next_ptr_const(
/*===================*/
	const rec_t*	rec,	/*!< in: physical record */
	ulint		comp)	/*!< in: nonzero=compact page format */
{
	ulint	field_value;

	compile_time_assert(REC_NEXT_MASK == 0xFFFFUL);
	compile_time_assert(REC_NEXT_SHIFT == 0);

	field_value = mach_read_from_2(rec - REC_NEXT);

	if (field_value == 0) {

		return(NULL);
	}

	if (comp) {
#if UNIV_PAGE_SIZE_MAX <= 32768
		/* Note that for 64 KiB pages, field_value can 'wrap around'
		and the debug assertion is not valid */

		/* In the following assertion, field_value is interpreted
		as signed 16-bit integer in 2's complement arithmetics.
		If all platforms defined int16_t in the standard headers,
		the expression could be written simpler as
		(int16_t) field_value + ut_align_offset(...) < srv_page_size
		*/
		ut_ad((field_value >= 32768
		       ? field_value - 65536
		       : field_value)
		      + ut_align_offset(rec, srv_page_size)
		      < srv_page_size);
#endif
		/* There must be at least REC_N_NEW_EXTRA_BYTES + 1
		between each record. */
		ut_ad((field_value > REC_N_NEW_EXTRA_BYTES
		       && field_value < 32768)
		      || field_value < (uint16) -REC_N_NEW_EXTRA_BYTES);

		return((byte*) ut_align_down(rec, srv_page_size)
		       + ut_align_offset(rec + field_value, srv_page_size));
	} else {
		ut_ad(field_value < srv_page_size);

		return((byte*) ut_align_down(rec, srv_page_size)
		       + field_value);
	}
}

/******************************************************//**
The following function is used to get the pointer of the next chained record
on the same page.
@return pointer to the next chained record, or NULL if none */
UNIV_INLINE
rec_t*
rec_get_next_ptr(
/*=============*/
	rec_t*	rec,	/*!< in: physical record */
	ulint	comp)	/*!< in: nonzero=compact page format */
{
	return(const_cast<rec_t*>(rec_get_next_ptr_const(rec, comp)));
}

/******************************************************//**
The following function is used to get the offset of the next chained record
on the same page.
@return the page offset of the next chained record, or 0 if none */
UNIV_INLINE
ulint
rec_get_next_offs(
/*==============*/
	const rec_t*	rec,	/*!< in: physical record */
	ulint		comp)	/*!< in: nonzero=compact page format */
{
	ulint	field_value;
	compile_time_assert(REC_NEXT_MASK == 0xFFFFUL);
	compile_time_assert(REC_NEXT_SHIFT == 0);

	field_value = mach_read_from_2(rec - REC_NEXT);

	if (comp) {
#if UNIV_PAGE_SIZE_MAX <= 32768
		/* Note that for 64 KiB pages, field_value can 'wrap around'
		and the debug assertion is not valid */

		/* In the following assertion, field_value is interpreted
		as signed 16-bit integer in 2's complement arithmetics.
		If all platforms defined int16_t in the standard headers,
		the expression could be written simpler as
		(int16_t) field_value + ut_align_offset(...) < srv_page_size
		*/
		ut_ad((field_value >= 32768
		       ? field_value - 65536
		       : field_value)
		      + ut_align_offset(rec, srv_page_size)
		      < srv_page_size);
#endif
		if (field_value == 0) {

			return(0);
		}

		/* There must be at least REC_N_NEW_EXTRA_BYTES + 1
		between each record. */
		ut_ad((field_value > REC_N_NEW_EXTRA_BYTES
		       && field_value < 32768)
		      || field_value < (uint16) -REC_N_NEW_EXTRA_BYTES);

		return(ut_align_offset(rec + field_value, srv_page_size));
	} else {
		ut_ad(field_value < srv_page_size);

		return(field_value);
	}
}

/******************************************************//**
The following function is used to set the next record offset field
of an old-style record. */
UNIV_INLINE
void
rec_set_next_offs_old(
/*==================*/
	rec_t*	rec,	/*!< in: old-style physical record */
	ulint	next)	/*!< in: offset of the next record */
{
	ut_ad(srv_page_size > next);
	compile_time_assert(REC_NEXT_MASK == 0xFFFFUL);
	compile_time_assert(REC_NEXT_SHIFT == 0);
	mach_write_to_2(rec - REC_NEXT, next);
}

/******************************************************//**
The following function is used to set the next record offset field
of a new-style record. */
UNIV_INLINE
void
rec_set_next_offs_new(
/*==================*/
	rec_t*	rec,	/*!< in/out: new-style physical record */
	ulint	next)	/*!< in: offset of the next record */
{
	ulint	field_value;

	ut_ad(srv_page_size > next);

	if (!next) {
		field_value = 0;
	} else {
		/* The following two statements calculate
		next - offset_of_rec mod 64Ki, where mod is the modulo
		as a non-negative number */

		field_value = (ulint)
			((lint) next
			 - (lint) ut_align_offset(rec, srv_page_size));
		field_value &= REC_NEXT_MASK;
	}

	mach_write_to_2(rec - REC_NEXT, field_value);
}

/******************************************************//**
The following function is used to get the number of fields
in an old-style record.
@return number of data fields */
UNIV_INLINE
ulint
rec_get_n_fields_old(
/*=================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	ulint	ret;

	ut_ad(rec);

	ret = rec_get_bit_field_2(rec, REC_OLD_N_FIELDS,
				  REC_OLD_N_FIELDS_MASK,
				  REC_OLD_N_FIELDS_SHIFT);
	ut_ad(ret <= REC_MAX_N_FIELDS);
	ut_ad(ret > 0);

	return(ret);
}

/******************************************************//**
The following function is used to set the number of fields
in an old-style record. */
UNIV_INLINE
void
rec_set_n_fields_old(
/*=================*/
	rec_t*	rec,		/*!< in: physical record */
	ulint	n_fields)	/*!< in: the number of fields */
{
	ut_ad(rec);
	ut_ad(n_fields <= REC_MAX_N_FIELDS);
	ut_ad(n_fields > 0);

	rec_set_bit_field_2(rec, n_fields, REC_OLD_N_FIELDS,
			    REC_OLD_N_FIELDS_MASK, REC_OLD_N_FIELDS_SHIFT);
}

/******************************************************//**
The following function is used to get the number of fields
in a record.
@return number of data fields */
UNIV_INLINE
ulint
rec_get_n_fields(
/*=============*/
	const rec_t*		rec,	/*!< in: physical record */
	const dict_index_t*	index)	/*!< in: record descriptor */
{
	ut_ad(rec);
	ut_ad(index);

	if (!dict_table_is_comp(index->table)) {
		return(rec_get_n_fields_old(rec));
	}

	switch (rec_get_status(rec)) {
	case REC_STATUS_INSTANT:
	case REC_STATUS_ORDINARY:
		return(dict_index_get_n_fields(index));
	case REC_STATUS_NODE_PTR:
		return(dict_index_get_n_unique_in_tree(index) + 1);
	case REC_STATUS_INFIMUM:
	case REC_STATUS_SUPREMUM:
		return(1);
	}

	ut_error;
	return(ULINT_UNDEFINED);
}

/** Confirms the n_fields of the entry is sane with comparing the other
record in the same page specified
@param[in]	index	index
@param[in]	rec	record of the same page
@param[in]	entry	index entry
@return	true if n_fields is sane */
UNIV_INLINE
bool
rec_n_fields_is_sane(
	dict_index_t*	index,
	const rec_t*	rec,
	const dtuple_t*	entry)
{
	const ulint n_fields = rec_get_n_fields(rec, index);

	return(n_fields == dtuple_get_n_fields(entry)
	       || (index->is_instant()
		   && n_fields >= index->n_core_fields)
	       /* a record for older SYS_INDEXES table
	       (missing merge_threshold column) is acceptable. */
	       || (index->table->id == DICT_INDEXES_ID
		   && n_fields == dtuple_get_n_fields(entry) - 1));
}

/******************************************************//**
The following function is used to get the number of records owned by the
previous directory record.
@return number of owned records */
UNIV_INLINE
ulint
rec_get_n_owned_old(
/*================*/
	const rec_t*	rec)	/*!< in: old-style physical record */
{
	return(rec_get_bit_field_1(rec, REC_OLD_N_OWNED,
				   REC_N_OWNED_MASK, REC_N_OWNED_SHIFT));
}

/******************************************************//**
The following function is used to get the number of records owned by the
previous directory record.
@return number of owned records */
UNIV_INLINE
ulint
rec_get_n_owned_new(
/*================*/
	const rec_t*	rec)	/*!< in: new-style physical record */
{
	return(rec_get_bit_field_1(rec, REC_NEW_N_OWNED,
				   REC_N_OWNED_MASK, REC_N_OWNED_SHIFT));
}

/******************************************************//**
The following function is used to retrieve the info bits of a record.
@return info bits */
UNIV_INLINE
byte
rec_get_info_bits(
/*==============*/
	const rec_t*	rec,	/*!< in: physical record */
	ulint		comp)	/*!< in: nonzero=compact page format */
{
	return rec_get_bit_field_1(
		rec, comp ? REC_NEW_INFO_BITS : REC_OLD_INFO_BITS,
		REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
}

/******************************************************//**
The following function is used to retrieve the info and status
bits of a record.  (Only compact records have status bits.)
@return info and status bits */
UNIV_INLINE
byte
rec_get_info_and_status_bits(
/*=========================*/
	const rec_t*	rec,	/*!< in: physical record */
	ulint		comp)	/*!< in: nonzero=compact page format */
{
  compile_time_assert(!((REC_NEW_STATUS_MASK >> REC_NEW_STATUS_SHIFT)
                        & (REC_INFO_BITS_MASK >> REC_INFO_BITS_SHIFT)));
  if (comp)
    return static_cast<byte>(rec_get_info_bits(rec, TRUE) |
                             rec_get_status(rec));
  else
    return rec_get_info_bits(rec, FALSE);
}
/******************************************************//**
The following function is used to set the info and status
bits of a record.  (Only compact records have status bits.) */
UNIV_INLINE
void
rec_set_info_and_status_bits(
/*=========================*/
	rec_t*	rec,	/*!< in/out: physical record */
	ulint	bits)	/*!< in: info bits */
{
	compile_time_assert(!((REC_NEW_STATUS_MASK >> REC_NEW_STATUS_SHIFT)
			      & (REC_INFO_BITS_MASK >> REC_INFO_BITS_SHIFT)));
	rec_set_status(rec, bits & REC_NEW_STATUS_MASK);
	rec_set_bit_field_1(rec, bits & ~REC_NEW_STATUS_MASK,
			    REC_NEW_INFO_BITS,
			    REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
}

/******************************************************//**
The following function tells if record is delete marked.
@return nonzero if delete marked */
UNIV_INLINE
ulint
rec_get_deleted_flag(
/*=================*/
	const rec_t*	rec,	/*!< in: physical record */
	ulint		comp)	/*!< in: nonzero=compact page format */
{
	if (comp) {
		return(rec_get_bit_field_1(rec, REC_NEW_INFO_BITS,
					   REC_INFO_DELETED_FLAG,
					   REC_INFO_BITS_SHIFT));
	} else {
		return(rec_get_bit_field_1(rec, REC_OLD_INFO_BITS,
					   REC_INFO_DELETED_FLAG,
					   REC_INFO_BITS_SHIFT));
	}
}

/******************************************************//**
The following function tells if a new-style record is a node pointer.
@return TRUE if node pointer */
UNIV_INLINE
bool
rec_get_node_ptr_flag(
/*==================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	return(REC_STATUS_NODE_PTR == rec_get_status(rec));
}

/******************************************************//**
The following function is used to get the order number
of an old-style record in the heap of the index page.
@return heap order number */
UNIV_INLINE
ulint
rec_get_heap_no_old(
/*================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	return(rec_get_bit_field_2(rec, REC_OLD_HEAP_NO,
				   REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT));
}

/******************************************************//**
The following function is used to get the order number
of a new-style record in the heap of the index page.
@return heap order number */
UNIV_INLINE
ulint
rec_get_heap_no_new(
/*================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	return(rec_get_bit_field_2(rec, REC_NEW_HEAP_NO,
				   REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT));
}

/******************************************************//**
The following function is used to test whether the data offsets in the record
are stored in one-byte or two-byte format.
@return TRUE if 1-byte form */
UNIV_INLINE
ibool
rec_get_1byte_offs_flag(
/*====================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	return(rec_get_bit_field_1(rec, REC_OLD_SHORT, REC_OLD_SHORT_MASK,
				   REC_OLD_SHORT_SHIFT));
}

/******************************************************//**
The following function is used to set the 1-byte offsets flag. */
UNIV_INLINE
void
rec_set_1byte_offs_flag(
/*====================*/
	rec_t*	rec,	/*!< in: physical record */
	ibool	flag)	/*!< in: TRUE if 1byte form */
{
	ut_ad(flag <= 1);

	rec_set_bit_field_1(rec, flag, REC_OLD_SHORT, REC_OLD_SHORT_MASK,
			    REC_OLD_SHORT_SHIFT);
}

/******************************************************//**
Returns the offset of nth field end if the record is stored in the 1-byte
offsets form. If the field is SQL null, the flag is ORed in the returned
value.
@return offset of the start of the field, SQL null flag ORed */
UNIV_INLINE
uint8_t
rec_1_get_field_end_info(
/*=====================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(rec_get_1byte_offs_flag(rec));
	ut_ad(n < rec_get_n_fields_old(rec));

	return(mach_read_from_1(rec - (REC_N_OLD_EXTRA_BYTES + n + 1)));
}

/******************************************************//**
Returns the offset of nth field end if the record is stored in the 2-byte
offsets form. If the field is SQL null, the flag is ORed in the returned
value.
@return offset of the start of the field, SQL null flag and extern
storage flag ORed */
UNIV_INLINE
uint16_t
rec_2_get_field_end_info(
/*=====================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(!rec_get_1byte_offs_flag(rec));
	ut_ad(n < rec_get_n_fields_old(rec));

	return(mach_read_from_2(rec - (REC_N_OLD_EXTRA_BYTES + 2 * n + 2)));
}

/******************************************************//**
Returns nonzero if the field is stored off-page.
@retval 0 if the field is stored in-page
@retval REC_2BYTE_EXTERN_MASK if the field is stored externally */
UNIV_INLINE
ulint
rec_2_is_field_extern(
/*==================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	return(rec_2_get_field_end_info(rec, n) & REC_2BYTE_EXTERN_MASK);
}

/**********************************************************//**
The following function sets the number of allocated elements
for an array of offsets. */
UNIV_INLINE
void
rec_offs_set_n_alloc(
/*=================*/
	rec_offs*offsets,	/*!< out: array for rec_get_offsets(),
				must be allocated */
	ulint	n_alloc)	/*!< in: number of elements */
{
	ut_ad(n_alloc > REC_OFFS_HEADER_SIZE);
	MEM_UNDEFINED(offsets, n_alloc * sizeof *offsets);
	offsets[0] = static_cast<rec_offs>(n_alloc);
}

/************************************************************//**
The following function is used to get an offset to the nth
data field in a record.
@return offset from the origin of rec */
UNIV_INLINE
rec_offs
rec_get_nth_field_offs(
/*===================*/
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	ulint		n,	/*!< in: index of the field */
	ulint*		len)	/*!< out: length of the field; UNIV_SQL_NULL
				if SQL null; UNIV_SQL_DEFAULT is default value */
{
	ut_ad(n < rec_offs_n_fields(offsets));

	rec_offs offs = n == 0 ? 0 : get_value(rec_offs_base(offsets)[n]);
	rec_offs next_offs = rec_offs_base(offsets)[1 + n];

	if (get_type(next_offs) == SQL_NULL) {
		*len = UNIV_SQL_NULL;
	} else if (get_type(next_offs) == DEFAULT) {
		*len = UNIV_SQL_DEFAULT;
	} else {
		*len = get_value(next_offs) - offs;
	}

	return(offs);
}

/******************************************************//**
Determine if the offsets are for a record containing null BLOB pointers.
@return first field containing a null BLOB pointer, or NULL if none found */
UNIV_INLINE
const byte*
rec_offs_any_null_extern(
/*=====================*/
	const rec_t*	rec,		/*!< in: record */
	const rec_offs*	offsets)	/*!< in: rec_get_offsets(rec) */
{
	ulint	i;
	ut_ad(rec_offs_validate(rec, NULL, offsets));

	if (!rec_offs_any_extern(offsets)) {
		return(NULL);
	}

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		if (rec_offs_nth_extern(offsets, i)) {
			ulint		len;
			const byte*	field
				= rec_get_nth_field(rec, offsets, i, &len);

			ut_a(len >= BTR_EXTERN_FIELD_REF_SIZE);
			if (!memcmp(field + len
				    - BTR_EXTERN_FIELD_REF_SIZE,
				    field_ref_zero,
				    BTR_EXTERN_FIELD_REF_SIZE)) {
				return(field);
			}
		}
	}

	return(NULL);
}

/******************************************************//**
Gets the physical size of a field.
@return length of field */
UNIV_INLINE
ulint
rec_offs_nth_size(
/*==============*/
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	ulint		n)	/*!< in: nth field */
{
	ut_ad(rec_offs_validate(NULL, NULL, offsets));
	ut_ad(n < rec_offs_n_fields(offsets));
	if (!n) {
		return get_value(rec_offs_base(offsets)[1 + n]);
	}
	return get_value((rec_offs_base(offsets)[1 + n]))
	       - get_value(rec_offs_base(offsets)[n]);
}

/******************************************************//**
Returns the number of extern bits set in a record.
@return number of externally stored fields */
UNIV_INLINE
ulint
rec_offs_n_extern(
/*==============*/
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	n = 0;

	if (rec_offs_any_extern(offsets)) {
		ulint	i;

		for (i = rec_offs_n_fields(offsets); i--; ) {
			if (rec_offs_nth_extern(offsets, i)) {
				n++;
			}
		}
	}

	return(n);
}

/******************************************************//**
Returns the offset of n - 1th field end if the record is stored in the 1-byte
offsets form. If the field is SQL null, the flag is ORed in the returned
value. This function and the 2-byte counterpart are defined here because the
C-compiler was not able to sum negative and positive constant offsets, and
warned of constant arithmetic overflow within the compiler.
@return offset of the start of the PREVIOUS field, SQL null flag ORed */
UNIV_INLINE
ulint
rec_1_get_prev_field_end_info(
/*==========================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(rec_get_1byte_offs_flag(rec));
	ut_ad(n <= rec_get_n_fields_old(rec));

	return(mach_read_from_1(rec - (REC_N_OLD_EXTRA_BYTES + n)));
}

/******************************************************//**
Returns the offset of n - 1th field end if the record is stored in the 2-byte
offsets form. If the field is SQL null, the flag is ORed in the returned
value.
@return offset of the start of the PREVIOUS field, SQL null flag ORed */
UNIV_INLINE
ulint
rec_2_get_prev_field_end_info(
/*==========================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(!rec_get_1byte_offs_flag(rec));
	ut_ad(n <= rec_get_n_fields_old(rec));

	return(mach_read_from_2(rec - (REC_N_OLD_EXTRA_BYTES + 2 * n)));
}

/******************************************************//**
Sets the field end info for the nth field if the record is stored in the
1-byte format. */
UNIV_INLINE
void
rec_1_set_field_end_info(
/*=====================*/
	rec_t*	rec,	/*!< in: record */
	ulint	n,	/*!< in: field index */
	ulint	info)	/*!< in: value to set */
{
	ut_ad(rec_get_1byte_offs_flag(rec));
	ut_ad(n < rec_get_n_fields_old(rec));

	mach_write_to_1(rec - (REC_N_OLD_EXTRA_BYTES + n + 1), info);
}

/******************************************************//**
Sets the field end info for the nth field if the record is stored in the
2-byte format. */
UNIV_INLINE
void
rec_2_set_field_end_info(
/*=====================*/
	rec_t*	rec,	/*!< in: record */
	ulint	n,	/*!< in: field index */
	ulint	info)	/*!< in: value to set */
{
	ut_ad(!rec_get_1byte_offs_flag(rec));
	ut_ad(n < rec_get_n_fields_old(rec));

	mach_write_to_2(rec - (REC_N_OLD_EXTRA_BYTES + 2 * n + 2), info);
}

/******************************************************//**
Returns the offset of nth field start if the record is stored in the 1-byte
offsets form.
@return offset of the start of the field */
UNIV_INLINE
ulint
rec_1_get_field_start_offs(
/*=======================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(rec_get_1byte_offs_flag(rec));
	ut_ad(n <= rec_get_n_fields_old(rec));

	if (n == 0) {

		return(0);
	}

	return(rec_1_get_prev_field_end_info(rec, n)
	       & ~REC_1BYTE_SQL_NULL_MASK);
}

/******************************************************//**
Returns the offset of nth field start if the record is stored in the 2-byte
offsets form.
@return offset of the start of the field */
UNIV_INLINE
ulint
rec_2_get_field_start_offs(
/*=======================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(!rec_get_1byte_offs_flag(rec));
	ut_ad(n <= rec_get_n_fields_old(rec));

	if (n == 0) {

		return(0);
	}

	return(rec_2_get_prev_field_end_info(rec, n)
	       & ~(REC_2BYTE_SQL_NULL_MASK | REC_2BYTE_EXTERN_MASK));
}

/******************************************************//**
The following function is used to read the offset of the start of a data field
in the record. The start of an SQL null field is the end offset of the
previous non-null field, or 0, if none exists. If n is the number of the last
field + 1, then the end offset of the last field is returned.
@return offset of the start of the field */
UNIV_INLINE
ulint
rec_get_field_start_offs(
/*=====================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: field index */
{
	ut_ad(rec);
	ut_ad(n <= rec_get_n_fields_old(rec));

	if (n == 0) {

		return(0);
	}

	if (rec_get_1byte_offs_flag(rec)) {

		return(rec_1_get_field_start_offs(rec, n));
	}

	return(rec_2_get_field_start_offs(rec, n));
}

/************************************************************//**
Gets the physical size of an old-style field.
Also an SQL null may have a field of size > 0,
if the data type is of a fixed size.
@return field size in bytes */
UNIV_INLINE
ulint
rec_get_nth_field_size(
/*===================*/
	const rec_t*	rec,	/*!< in: record */
	ulint		n)	/*!< in: index of the field */
{
	ulint	os;
	ulint	next_os;

	os = rec_get_field_start_offs(rec, n);
	next_os = rec_get_field_start_offs(rec, n + 1);

	ut_ad(next_os - os < srv_page_size);

	return(next_os - os);
}

/**********************************************************//**
The following function returns the data size of an old-style physical
record, that is the sum of field lengths. SQL null fields
are counted as length 0 fields. The value returned by the function
is the distance from record origin to record end in bytes.
@return size */
UNIV_INLINE
ulint
rec_get_data_size_old(
/*==================*/
	const rec_t*	rec)	/*!< in: physical record */
{
	ut_ad(rec);

	return(rec_get_field_start_offs(rec, rec_get_n_fields_old(rec)));
}

/**********************************************************//**
The following function sets the number of fields in offsets. */
UNIV_INLINE
void
rec_offs_set_n_fields(
/*==================*/
	rec_offs*	offsets,	/*!< in/out: array returned by
				rec_get_offsets() */
	ulint		n_fields)	/*!< in: number of fields */
{
	ut_ad(offsets);
	ut_ad(n_fields > 0);
	ut_ad(n_fields <= REC_MAX_N_FIELDS);
	ut_ad(n_fields + REC_OFFS_HEADER_SIZE
	      <= rec_offs_get_n_alloc(offsets));
	offsets[1] = static_cast<rec_offs>(n_fields);
}

/**********************************************************//**
The following function returns the data size of a physical
record, that is the sum of field lengths. SQL null fields
are counted as length 0 fields. The value returned by the function
is the distance from record origin to record end in bytes.
@return size */
UNIV_INLINE
ulint
rec_offs_data_size(
/*===============*/
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	size;

	ut_ad(rec_offs_validate(NULL, NULL, offsets));
	size = get_value(rec_offs_base(offsets)[rec_offs_n_fields(offsets)]);
	ut_ad(size < srv_page_size);
	return(size);
}

/**********************************************************//**
Returns the total size of record minus data size of record. The value
returned by the function is the distance from record start to record origin
in bytes.
@return size */
UNIV_INLINE
ulint
rec_offs_extra_size(
/*================*/
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ulint	size;
	ut_ad(rec_offs_validate(NULL, NULL, offsets));
	size = *rec_offs_base(offsets) & REC_OFFS_MASK;
	ut_ad(size < srv_page_size);
	return(size);
}

/**********************************************************//**
Returns the total size of a physical record.
@return size */
UNIV_INLINE
ulint
rec_offs_size(
/*==========*/
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	return(rec_offs_data_size(offsets) + rec_offs_extra_size(offsets));
}

#ifdef UNIV_DEBUG
/**********************************************************//**
Returns a pointer to the end of the record.
@return pointer to end */
UNIV_INLINE
byte*
rec_get_end(
/*========*/
	const rec_t*	rec,	/*!< in: pointer to record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	return(const_cast<rec_t*>(rec + rec_offs_data_size(offsets)));
}

/**********************************************************//**
Returns a pointer to the start of the record.
@return pointer to start */
UNIV_INLINE
byte*
rec_get_start(
/*==========*/
	const rec_t*	rec,	/*!< in: pointer to record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	return(const_cast<rec_t*>(rec - rec_offs_extra_size(offsets)));
}
#endif /* UNIV_DEBUG */

/** Copy a physical record to a buffer.
@param[in]	buf	buffer
@param[in]	rec	physical record
@param[in]	offsets	array returned by rec_get_offsets()
@return pointer to the origin of the copy */
UNIV_INLINE
rec_t*
rec_copy(
	void*		buf,
	const rec_t*	rec,
	const rec_offs*	offsets)
{
	ulint	extra_len;
	ulint	data_len;

	ut_ad(rec != NULL);
	ut_ad(buf != NULL);
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(rec_validate(rec, offsets));

	extra_len = rec_offs_extra_size(offsets);
	data_len = rec_offs_data_size(offsets);

	memcpy(buf, rec - extra_len, extra_len + data_len);

	return((byte*) buf + extra_len);
}

/**********************************************************//**
Returns the extra size of an old-style physical record if we know its
data size and number of fields.
@return extra size */
UNIV_INLINE
ulint
rec_get_converted_extra_size(
/*=========================*/
	ulint	data_size,	/*!< in: data size */
	ulint	n_fields,	/*!< in: number of fields */
	ulint	n_ext)		/*!< in: number of externally stored columns */
{
	if (!n_ext && data_size <= REC_1BYTE_OFFS_LIMIT) {

		return(REC_N_OLD_EXTRA_BYTES + n_fields);
	}

	return(REC_N_OLD_EXTRA_BYTES + 2 * n_fields);
}

/**********************************************************//**
The following function returns the size of a data tuple when converted to
a physical record.
@return size */
UNIV_INLINE
ulint
rec_get_converted_size(
/*===================*/
	dict_index_t*	index,	/*!< in: record descriptor */
	const dtuple_t*	dtuple,	/*!< in: data tuple */
	ulint		n_ext)	/*!< in: number of externally stored columns */
{
	ulint	data_size;
	ulint	extra_size;

	ut_ad(dtuple_check_typed(dtuple));
#ifdef UNIV_DEBUG
	if (dict_index_is_ibuf(index)) {
		ut_ad(dtuple->n_fields > 1);
	} else if ((dtuple_get_info_bits(dtuple) & REC_NEW_STATUS_MASK)
		   == REC_STATUS_NODE_PTR) {
		ut_ad(dtuple->n_fields - 1
		      == dict_index_get_n_unique_in_tree_nonleaf(index));
	} else if (index->table->id == DICT_INDEXES_ID) {
		/* The column SYS_INDEXES.MERGE_THRESHOLD was
		instantly added in MariaDB 10.2.2 (MySQL 5.7). */
		ut_ad(!index->table->is_temporary());
		ut_ad(index->n_fields == DICT_NUM_FIELDS__SYS_INDEXES);
		ut_ad(dtuple->n_fields == DICT_NUM_FIELDS__SYS_INDEXES
		      || dtuple->n_fields
		      == DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD);
	} else {
		ut_ad(dtuple->n_fields >= index->n_core_fields);
		ut_ad(dtuple->n_fields <= index->n_fields
		      || dtuple->is_alter_metadata());
	}
#endif

	if (dict_table_is_comp(index->table)) {
		return rec_get_converted_size_comp(index, dtuple, NULL);
	}

	data_size = dtuple_get_data_size(dtuple, 0);

	/* If primary key is being updated then the new record inherits
	externally stored fields from the delete-marked old record.
	In that case, n_ext may be less value than
	dtuple_get_n_ext(tuple). */
	ut_ad(n_ext <= dtuple_get_n_ext(dtuple));
	extra_size = rec_get_converted_extra_size(
		data_size, dtuple_get_n_fields(dtuple), n_ext);

	return(data_size + extra_size);
}
