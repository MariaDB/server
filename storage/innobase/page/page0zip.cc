/*****************************************************************************

Copyright (c) 2005, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2014, 2022, MariaDB Corporation.

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
@file page/page0zip.cc
Compressed page interface

Created June 2005 by Marko Makela
*******************************************************/

#include "page0zip.h"
#include "fsp0types.h"
#include "page0page.h"
#include "buf0checksum.h"
#include "zlib.h"
#include "span.h"

using st_::span;

#ifndef UNIV_INNOCHECKSUM
#include "mtr0log.h"
#include "dict0dict.h"
#include "btr0cur.h"
#include "log0recv.h"
#include "row0row.h"
#include "btr0sea.h"
#include "dict0boot.h"
#include "lock0lock.h"
#include "srv0srv.h"
#include "buf0lru.h"
#include "srv0mon.h"

#include <map>
#include <algorithm>

/** Statistics on compression, indexed by page_zip_des_t::ssize - 1 */
page_zip_stat_t		page_zip_stat[PAGE_ZIP_SSIZE_MAX];
/** Statistics on compression, indexed by index->id */
page_zip_stat_per_index_t	page_zip_stat_per_index;

/** Compression level to be used by zlib. Settable by user. */
uint	page_zip_level;

/* Please refer to ../include/page0zip.ic for a description of the
compressed page format. */

/* The infimum and supremum records are omitted from the compressed page.
On compress, we compare that the records are there, and on uncompress we
restore the records. */
/** Extra bytes of an infimum record */
static const byte infimum_extra[] = {
	0x01,			/* info_bits=0, n_owned=1 */
	0x00, 0x02		/* heap_no=0, status=2 */
	/* ?, ?	*/		/* next=(first user rec, or supremum) */
};
/** Data bytes of an infimum record */
static const byte infimum_data[] = {
	0x69, 0x6e, 0x66, 0x69,
	0x6d, 0x75, 0x6d, 0x00	/* "infimum\0" */
};
/** Extra bytes and data bytes of a supremum record */
static const byte supremum_extra_data alignas(4) [] = {
	/* 0x0?, */		/* info_bits=0, n_owned=1..8 */
	0x00, 0x0b,		/* heap_no=1, status=3 */
	0x00, 0x00,		/* next=0 */
	0x73, 0x75, 0x70, 0x72,
	0x65, 0x6d, 0x75, 0x6d	/* "supremum" */
};

/** Assert that a block of memory is filled with zero bytes.
@param b in: memory block
@param s in: size of the memory block, in bytes */
#define ASSERT_ZERO(b, s) ut_ad(!memcmp(b, field_ref_zero, s))
/** Assert that a BLOB pointer is filled with zero bytes.
@param b in: BLOB pointer */
#define ASSERT_ZERO_BLOB(b) ASSERT_ZERO(b, FIELD_REF_SIZE)

/* Enable some extra debugging output.  This code can be enabled
independently of any UNIV_ debugging conditions. */
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
# include <stdarg.h>
MY_ATTRIBUTE((format (printf, 1, 2)))
/**********************************************************************//**
Report a failure to decompress or compress.
@return number of characters printed */
static
int
page_zip_fail_func(
/*===============*/
	const char*	fmt,	/*!< in: printf(3) format string */
	...)			/*!< in: arguments corresponding to fmt */
{
	int	res;
	va_list	ap;

	ut_print_timestamp(stderr);
	fputs("  InnoDB: ", stderr);
	va_start(ap, fmt);
	res = vfprintf(stderr, fmt, ap);
	va_end(ap);

	return(res);
}
/** Wrapper for page_zip_fail_func()
@param fmt_args in: printf(3) format string and arguments */
# define page_zip_fail(fmt_args) page_zip_fail_func fmt_args
#else /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
/** Dummy wrapper for page_zip_fail_func()
@param fmt_args ignored: printf(3) format string and arguments */
# define page_zip_fail(fmt_args) /* empty */
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

/**********************************************************************//**
Determine the guaranteed free space on an empty page.
@return minimum payload size on the page */
ulint
page_zip_empty_size(
/*================*/
	ulint	n_fields,	/*!< in: number of columns in the index */
	ulint	zip_size)	/*!< in: compressed page size in bytes */
{
	ulint	size = zip_size
		/* subtract the page header and the longest
		uncompressed data needed for one record */
		- (PAGE_DATA
		   + PAGE_ZIP_CLUST_LEAF_SLOT_SIZE
		   + 1/* encoded heap_no==2 in page_zip_write_rec() */
		   + 1/* end of modification log */
		   - REC_N_NEW_EXTRA_BYTES/* omitted bytes */)
		/* subtract the space for page_zip_fields_encode() */
		- compressBound(static_cast<uLong>(2 * (n_fields + 1)));
	return(lint(size) > 0 ? size : 0);
}

/** Check whether a tuple is too big for compressed table
@param[in]	index	dict index object
@param[in]	entry	entry for the index
@return	true if it's too big, otherwise false */
bool
page_zip_is_too_big(
	const dict_index_t*	index,
	const dtuple_t*		entry)
{
	const ulint zip_size = index->table->space->zip_size();

	/* Estimate the free space of an empty compressed page.
	Subtract one byte for the encoded heap_no in the
	modification log. */
	ulint	free_space_zip = page_zip_empty_size(
		index->n_fields, zip_size);
	ulint	n_uniq = dict_index_get_n_unique_in_tree(index);

	ut_ad(dict_table_is_comp(index->table));
	ut_ad(zip_size);

	if (free_space_zip == 0) {
		return(true);
	}

	/* Subtract one byte for the encoded heap_no in the
	modification log. */
	free_space_zip--;

	/* There should be enough room for two node pointer
	records on an empty non-leaf page.  This prevents
	infinite page splits. */

	if (entry->n_fields >= n_uniq
	    && (REC_NODE_PTR_SIZE
		+ rec_get_converted_size_comp_prefix(
			index, entry->fields, n_uniq, NULL)
		/* On a compressed page, there is
		a two-byte entry in the dense
		page directory for every record.
		But there is no record header. */
		- (REC_N_NEW_EXTRA_BYTES - 2)
		> free_space_zip / 2)) {
		return(true);
	}

	return(false);
}

/*************************************************************//**
Gets the number of elements in the dense page directory,
including deleted records (the free list).
@return number of elements in the dense page directory */
UNIV_INLINE
ulint
page_zip_dir_elems(
/*===============*/
	const page_zip_des_t*	page_zip)	/*!< in: compressed page */
{
	/* Exclude the page infimum and supremum from the record count. */
	return ulint(page_dir_get_n_heap(page_zip->data))
		- PAGE_HEAP_NO_USER_LOW;
}

/*************************************************************//**
Gets the size of the compressed page trailer (the dense page directory),
including deleted records (the free list).
@return length of dense page directory, in bytes */
UNIV_INLINE
ulint
page_zip_dir_size(
/*==============*/
	const page_zip_des_t*	page_zip)	/*!< in: compressed page */
{
	return(PAGE_ZIP_DIR_SLOT_SIZE * page_zip_dir_elems(page_zip));
}

/*************************************************************//**
Gets an offset to the compressed page trailer (the dense page directory),
including deleted records (the free list).
@return offset of the dense page directory */
UNIV_INLINE
ulint
page_zip_dir_start_offs(
/*====================*/
	const page_zip_des_t*	page_zip,	/*!< in: compressed page */
	ulint			n_dense)	/*!< in: directory size */
{
	ut_ad(n_dense * PAGE_ZIP_DIR_SLOT_SIZE < page_zip_get_size(page_zip));

	return(page_zip_get_size(page_zip) - n_dense * PAGE_ZIP_DIR_SLOT_SIZE);
}

/*************************************************************//**
Gets a pointer to the compressed page trailer (the dense page directory),
including deleted records (the free list).
@param[in] page_zip compressed page
@param[in] n_dense number of entries in the directory
@return pointer to the dense page directory */
#define page_zip_dir_start_low(page_zip, n_dense)			\
	((page_zip)->data + page_zip_dir_start_offs(page_zip, n_dense))
/*************************************************************//**
Gets a pointer to the compressed page trailer (the dense page directory),
including deleted records (the free list).
@param[in] page_zip compressed page
@return pointer to the dense page directory */
#define page_zip_dir_start(page_zip)					\
	page_zip_dir_start_low(page_zip, page_zip_dir_elems(page_zip))

/*************************************************************//**
Gets the size of the compressed page trailer (the dense page directory),
only including user records (excluding the free list).
@return length of dense page directory comprising existing records, in bytes */
UNIV_INLINE
ulint
page_zip_dir_user_size(
/*===================*/
	const page_zip_des_t*	page_zip)	/*!< in: compressed page */
{
	ulint	size = PAGE_ZIP_DIR_SLOT_SIZE
		* ulint(page_get_n_recs(page_zip->data));
	ut_ad(size <= page_zip_dir_size(page_zip));
	return(size);
}

/*************************************************************//**
Find the slot of the given record in the dense page directory.
@return dense directory slot, or NULL if record not found */
UNIV_INLINE
byte*
page_zip_dir_find_low(
/*==================*/
	byte*	slot,			/*!< in: start of records */
	byte*	end,			/*!< in: end of records */
	ulint	offset)			/*!< in: offset of user record */
{
	ut_ad(slot <= end);

	for (; slot < end; slot += PAGE_ZIP_DIR_SLOT_SIZE) {
		if ((mach_read_from_2(slot) & PAGE_ZIP_DIR_SLOT_MASK)
		    == offset) {
			return(slot);
		}
	}

	return(NULL);
}

/*************************************************************//**
Find the slot of the given non-free record in the dense page directory.
@return dense directory slot, or NULL if record not found */
UNIV_INLINE
byte*
page_zip_dir_find(
/*==============*/
	page_zip_des_t*	page_zip,		/*!< in: compressed page */
	ulint		offset)			/*!< in: offset of user record */
{
	byte*	end	= page_zip->data + page_zip_get_size(page_zip);

	ut_ad(page_zip_simple_validate(page_zip));

	return(page_zip_dir_find_low(end - page_zip_dir_user_size(page_zip),
				     end,
				     offset));
}

/*************************************************************//**
Find the slot of the given free record in the dense page directory.
@return dense directory slot, or NULL if record not found */
UNIV_INLINE
byte*
page_zip_dir_find_free(
/*===================*/
	page_zip_des_t*	page_zip,		/*!< in: compressed page */
	ulint		offset)			/*!< in: offset of user record */
{
	byte*	end	= page_zip->data + page_zip_get_size(page_zip);

	ut_ad(page_zip_simple_validate(page_zip));

	return(page_zip_dir_find_low(end - page_zip_dir_size(page_zip),
				     end - page_zip_dir_user_size(page_zip),
				     offset));
}

/*************************************************************//**
Read a given slot in the dense page directory.
@return record offset on the uncompressed page, possibly ORed with
PAGE_ZIP_DIR_SLOT_DEL or PAGE_ZIP_DIR_SLOT_OWNED */
UNIV_INLINE
ulint
page_zip_dir_get(
/*=============*/
	const page_zip_des_t*	page_zip,	/*!< in: compressed page */
	ulint			slot)		/*!< in: slot
						(0=first user record) */
{
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(slot < page_zip_dir_size(page_zip) / PAGE_ZIP_DIR_SLOT_SIZE);
	return(mach_read_from_2(page_zip->data + page_zip_get_size(page_zip)
				- PAGE_ZIP_DIR_SLOT_SIZE * (slot + 1)));
}

/** Write a byte string to a ROW_FORMAT=COMPRESSED page.
@param[in]      b       ROW_FORMAT=COMPRESSED index page
@param[in]      offset  byte offset from b.zip.data
@param[in]      len     length of the data to write */
inline void mtr_t::zmemcpy(const buf_block_t &b, ulint offset, ulint len)
{
  ut_ad(fil_page_get_type(b.page.zip.data) == FIL_PAGE_INDEX ||
        fil_page_get_type(b.page.zip.data) == FIL_PAGE_RTREE);
  ut_ad(page_zip_simple_validate(&b.page.zip));
  ut_ad(offset + len <= page_zip_get_size(&b.page.zip));

  memcpy_low(b, static_cast<uint16_t>(offset), &b.page.zip.data[offset], len);
  m_last_offset= static_cast<uint16_t>(offset + len);
}

/** Write a byte string to a ROW_FORMAT=COMPRESSED page.
@param[in]      b       ROW_FORMAT=COMPRESSED index page
@param[in]      dest    destination within b.zip.data
@param[in]      str     the data to write
@param[in]      len     length of the data to write
@tparam w       write request type */
template<mtr_t::write_type w>
inline void mtr_t::zmemcpy(const buf_block_t &b, void *dest, const void *str,
                           ulint len)
{
  byte *d= static_cast<byte*>(dest);
  const byte *s= static_cast<const byte*>(str);
  ut_ad(d >= b.page.zip.data + FIL_PAGE_OFFSET);
  if (w != FORCED)
  {
    ut_ad(len);
    const byte *const end= d + len;
    while (*d++ == *s++)
    {
      if (d == end)
      {
        ut_ad(w == MAYBE_NOP);
        return;
      }
    }
    s--;
    d--;
    len= static_cast<ulint>(end - d);
  }
  ::memcpy(d, s, len);
  zmemcpy(b, d - b.page.zip.data, len);
}

/** Write redo log for compressing a ROW_FORMAT=COMPRESSED index page.
@param[in,out]	block	ROW_FORMAT=COMPRESSED index page
@param[in]	index	the index that the block belongs to
@param[in,out]	mtr	mini-transaction */
static void page_zip_compress_write_log(buf_block_t *block,
                                        dict_index_t *index, mtr_t *mtr)
{
  ut_ad(!index->is_ibuf());

  if (mtr->get_log_mode() != MTR_LOG_ALL)
  {
    ut_ad(mtr->get_log_mode() == MTR_LOG_NONE ||
          mtr->get_log_mode() == MTR_LOG_NO_REDO);
    return;
  }

  const page_t *page= block->page.frame;
  const page_zip_des_t *page_zip= &block->page.zip;
  /* Read the number of user records. */
  ulint trailer_size= ulint(page_dir_get_n_heap(page_zip->data)) -
    PAGE_HEAP_NO_USER_LOW;
  /* Multiply by uncompressed of size stored per record */
  if (!page_is_leaf(page))
    trailer_size*= PAGE_ZIP_DIR_SLOT_SIZE + REC_NODE_PTR_SIZE;
  else if (index->is_clust())
    trailer_size*= PAGE_ZIP_DIR_SLOT_SIZE + DATA_TRX_ID_LEN +
      DATA_ROLL_PTR_LEN;
  else
    trailer_size*= PAGE_ZIP_DIR_SLOT_SIZE;
  /* Add the space occupied by BLOB pointers. */
  trailer_size+= page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
  ut_a(page_zip->m_end > PAGE_DATA);
  compile_time_assert(FIL_PAGE_DATA <= PAGE_DATA);
  ut_a(page_zip->m_end + trailer_size <= page_zip_get_size(page_zip));

  mtr->init(block);
  mtr->zmemcpy(*block, FIL_PAGE_PREV, page_zip->m_end - FIL_PAGE_PREV);

  if (trailer_size)
    mtr->zmemcpy(*block, page_zip_get_size(page_zip) - trailer_size,
                 trailer_size);
}

/******************************************************//**
Determine how many externally stored columns are contained
in existing records with smaller heap_no than rec. */
static
ulint
page_zip_get_n_prev_extern(
/*=======================*/
	const page_zip_des_t*	page_zip,/*!< in: dense page directory on
					compressed page */
	const rec_t*		rec,	/*!< in: compact physical record
					on a B-tree leaf page */
	const dict_index_t*	index)	/*!< in: record descriptor */
{
	const page_t*	page	= page_align(rec);
	ulint		n_ext	= 0;
	ulint		i;
	ulint		left;
	ulint		heap_no;
	ulint		n_recs	= page_get_n_recs(page_zip->data);

	ut_ad(page_is_leaf(page));
	ut_ad(page_is_comp(page));
	ut_ad(dict_table_is_comp(index->table));
	ut_ad(dict_index_is_clust(index));
	ut_ad(!dict_index_is_ibuf(index));

	heap_no = rec_get_heap_no_new(rec);
	ut_ad(heap_no >= PAGE_HEAP_NO_USER_LOW);
	left = heap_no - PAGE_HEAP_NO_USER_LOW;
	if (UNIV_UNLIKELY(!left)) {
		return(0);
	}

	for (i = 0; i < n_recs; i++) {
		const rec_t*	r	= page + (page_zip_dir_get(page_zip, i)
						  & PAGE_ZIP_DIR_SLOT_MASK);

		if (rec_get_heap_no_new(r) < heap_no) {
			n_ext += rec_get_n_extern_new(r, index,
						      ULINT_UNDEFINED);
			if (!--left) {
				break;
			}
		}
	}

	return(n_ext);
}

/**********************************************************************//**
Encode the length of a fixed-length column.
@return buf + length of encoded val */
static
byte*
page_zip_fixed_field_encode(
/*========================*/
	byte*	buf,	/*!< in: pointer to buffer where to write */
	ulint	val)	/*!< in: value to write */
{
	ut_ad(val >= 2);

	if (UNIV_LIKELY(val < 126)) {
		/*
		0 = nullable variable field of at most 255 bytes length;
		1 = not null variable field of at most 255 bytes length;
		126 = nullable variable field with maximum length >255;
		127 = not null variable field with maximum length >255
		*/
		*buf++ = (byte) val;
	} else {
		*buf++ = (byte) (0x80 | val >> 8);
		*buf++ = (byte) val;
	}

	return(buf);
}

/**********************************************************************//**
Write the index information for the compressed page.
@return used size of buf */
ulint
page_zip_fields_encode(
/*===================*/
	ulint			n,	/*!< in: number of fields
					to compress */
	const dict_index_t*	index,	/*!< in: index comprising
					at least n fields */
	ulint			trx_id_pos,
					/*!< in: position of the trx_id column
					in the index, or ULINT_UNDEFINED if
					this is a non-leaf page */
	byte*			buf)	/*!< out: buffer of (n + 1) * 2 bytes */
{
	const byte*	buf_start	= buf;
	ulint		i;
	ulint		col;
	ulint		trx_id_col	= 0;
	/* sum of lengths of preceding non-nullable fixed fields, or 0 */
	ulint		fixed_sum	= 0;

	ut_ad(trx_id_pos == ULINT_UNDEFINED || trx_id_pos < n);

	for (i = col = 0; i < n; i++) {
		dict_field_t*	field = dict_index_get_nth_field(index, i);
		ulint		val;

		if (dict_field_get_col(field)->prtype & DATA_NOT_NULL) {
			val = 1; /* set the "not nullable" flag */
		} else {
			val = 0; /* nullable field */
		}

		if (!field->fixed_len) {
			/* variable-length field */
			const dict_col_t*	column
				= dict_field_get_col(field);

			if (DATA_BIG_COL(column)) {
				val |= 0x7e; /* max > 255 bytes */
			}

			if (fixed_sum) {
				/* write out the length of any
				preceding non-nullable fields */
				buf = page_zip_fixed_field_encode(
					buf, fixed_sum << 1 | 1);
				fixed_sum = 0;
				col++;
			}

			*buf++ = (byte) val;
			col++;
		} else if (val) {
			/* fixed-length non-nullable field */

			if (fixed_sum && UNIV_UNLIKELY
			    (fixed_sum + field->fixed_len
			     > DICT_MAX_FIXED_COL_LEN)) {
				/* Write out the length of the
				preceding non-nullable fields,
				to avoid exceeding the maximum
				length of a fixed-length column. */
				buf = page_zip_fixed_field_encode(
					buf, fixed_sum << 1 | 1);
				fixed_sum = 0;
				col++;
			}

			if (i && UNIV_UNLIKELY(i == trx_id_pos)) {
				if (fixed_sum) {
					/* Write out the length of any
					preceding non-nullable fields,
					and start a new trx_id column. */
					buf = page_zip_fixed_field_encode(
						buf, fixed_sum << 1 | 1);
					col++;
				}

				trx_id_col = col;
				fixed_sum = field->fixed_len;
			} else {
				/* add to the sum */
				fixed_sum += field->fixed_len;
			}
		} else {
			/* fixed-length nullable field */

			if (fixed_sum) {
				/* write out the length of any
				preceding non-nullable fields */
				buf = page_zip_fixed_field_encode(
					buf, fixed_sum << 1 | 1);
				fixed_sum = 0;
				col++;
			}

			buf = page_zip_fixed_field_encode(
				buf, ulint(field->fixed_len) << 1);
			col++;
		}
	}

	if (fixed_sum) {
		/* Write out the lengths of last fixed-length columns. */
		buf = page_zip_fixed_field_encode(buf, fixed_sum << 1 | 1);
	}

	if (trx_id_pos != ULINT_UNDEFINED) {
		/* Write out the position of the trx_id column */
		i = trx_id_col;
	} else {
		/* Write out the number of nullable fields */
		i = index->n_nullable;
	}

	if (i < 128) {
		*buf++ = (byte) i;
	} else {
		*buf++ = (byte) (0x80 | i >> 8);
		*buf++ = (byte) i;
	}

	ut_ad((ulint) (buf - buf_start) <= (n + 2) * 2);
	return((ulint) (buf - buf_start));
}

/**********************************************************************//**
Populate the dense page directory from the sparse directory. */
static
void
page_zip_dir_encode(
/*================*/
	const page_t*	page,	/*!< in: compact page */
	byte*		buf,	/*!< in: pointer to dense page directory[-1];
				out: dense directory on compressed page */
	const rec_t**	recs)	/*!< in: pointer to an array of 0, or NULL;
				out: dense page directory sorted by ascending
				address (and heap_no) */
{
	const byte*	rec;
	ulint		status;
	ulint		min_mark;
	ulint		heap_no;
	ulint		i;
	ulint		n_heap;
	ulint		offs;

	min_mark = 0;

	if (page_is_leaf(page)) {
		status = REC_STATUS_ORDINARY;
	} else {
		status = REC_STATUS_NODE_PTR;
		if (UNIV_UNLIKELY(!page_has_prev(page))) {
			min_mark = REC_INFO_MIN_REC_FLAG;
		}
	}

	n_heap = page_dir_get_n_heap(page);

	/* Traverse the list of stored records in the collation order,
	starting from the first user record. */

	rec = page + PAGE_NEW_INFIMUM;

	i = 0;

	for (;;) {
		ulint	info_bits;
		offs = rec_get_next_offs(rec, TRUE);
		if (UNIV_UNLIKELY(offs == PAGE_NEW_SUPREMUM)) {
			break;
		}
		rec = page + offs;
		heap_no = rec_get_heap_no_new(rec);
		ut_a(heap_no >= PAGE_HEAP_NO_USER_LOW);
		ut_a(heap_no < n_heap);
		ut_a(offs < srv_page_size - PAGE_DIR);
		ut_a(offs >= PAGE_ZIP_START);
		compile_time_assert(!(PAGE_ZIP_DIR_SLOT_MASK
				      & (PAGE_ZIP_DIR_SLOT_MASK + 1)));
		compile_time_assert(PAGE_ZIP_DIR_SLOT_MASK
				    >= UNIV_ZIP_SIZE_MAX - 1);

		if (UNIV_UNLIKELY(rec_get_n_owned_new(rec) != 0)) {
			offs |= PAGE_ZIP_DIR_SLOT_OWNED;
		}

		info_bits = rec_get_info_bits(rec, TRUE);
		if (info_bits & REC_INFO_DELETED_FLAG) {
			info_bits &= ~REC_INFO_DELETED_FLAG;
			offs |= PAGE_ZIP_DIR_SLOT_DEL;
		}
		ut_a(info_bits == min_mark);
		/* Only the smallest user record can have
		REC_INFO_MIN_REC_FLAG set. */
		min_mark = 0;

		mach_write_to_2(buf - PAGE_ZIP_DIR_SLOT_SIZE * ++i, offs);

		if (UNIV_LIKELY_NULL(recs)) {
			/* Ensure that each heap_no occurs at most once. */
			ut_a(!recs[heap_no - PAGE_HEAP_NO_USER_LOW]);
			/* exclude infimum and supremum */
			recs[heap_no - PAGE_HEAP_NO_USER_LOW] = rec;
		}

		ut_a(ulint(rec_get_status(rec)) == status);
	}

	offs = page_header_get_field(page, PAGE_FREE);

	/* Traverse the free list (of deleted records). */
	while (offs) {
		ut_ad(!(offs & ~PAGE_ZIP_DIR_SLOT_MASK));
		rec = page + offs;

		heap_no = rec_get_heap_no_new(rec);
		ut_a(heap_no >= PAGE_HEAP_NO_USER_LOW);
		ut_a(heap_no < n_heap);

		ut_a(!rec[-REC_N_NEW_EXTRA_BYTES]); /* info_bits and n_owned */
		ut_a(ulint(rec_get_status(rec)) == status);

		mach_write_to_2(buf - PAGE_ZIP_DIR_SLOT_SIZE * ++i, offs);

		if (UNIV_LIKELY_NULL(recs)) {
			/* Ensure that each heap_no occurs at most once. */
			ut_a(!recs[heap_no - PAGE_HEAP_NO_USER_LOW]);
			/* exclude infimum and supremum */
			recs[heap_no - PAGE_HEAP_NO_USER_LOW] = rec;
		}

		offs = rec_get_next_offs(rec, TRUE);
	}

	/* Ensure that each heap no occurs at least once. */
	ut_a(i + PAGE_HEAP_NO_USER_LOW == n_heap);
}

extern "C" {

/**********************************************************************//**
Allocate memory for zlib. */
static
void*
page_zip_zalloc(
/*============*/
	void*	opaque,	/*!< in/out: memory heap */
	uInt	items,	/*!< in: number of items to allocate */
	uInt	size)	/*!< in: size of an item in bytes */
{
	return(mem_heap_zalloc(static_cast<mem_heap_t*>(opaque), items * size));
}

/**********************************************************************//**
Deallocate memory for zlib. */
static
void
page_zip_free(
/*==========*/
	void*	opaque MY_ATTRIBUTE((unused)),	/*!< in: memory heap */
	void*	address MY_ATTRIBUTE((unused)))/*!< in: object to free */
{
}

} /* extern "C" */

/**********************************************************************//**
Configure the zlib allocator to use the given memory heap. */
void
page_zip_set_alloc(
/*===============*/
	void*		stream,		/*!< in/out: zlib stream */
	mem_heap_t*	heap)		/*!< in: memory heap to use */
{
	z_stream*	strm = static_cast<z_stream*>(stream);

	strm->zalloc = page_zip_zalloc;
	strm->zfree = page_zip_free;
	strm->opaque = heap;
}

#if 0 || defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
/** Symbol for enabling compression and decompression diagnostics */
# define PAGE_ZIP_COMPRESS_DBG
#endif

#ifdef PAGE_ZIP_COMPRESS_DBG
/** Set this variable in a debugger to enable
excessive logging in page_zip_compress(). */
static bool	page_zip_compress_dbg;
/** Set this variable in a debugger to enable
binary logging of the data passed to deflate().
When this variable is nonzero, it will act
as a log file name generator. */
static unsigned	page_zip_compress_log;

/**********************************************************************//**
Wrapper for deflate().  Log the operation if page_zip_compress_dbg is set.
@return deflate() status: Z_OK, Z_BUF_ERROR, ... */
static
int
page_zip_compress_deflate(
/*======================*/
	FILE*		logfile,/*!< in: log file, or NULL */
	z_streamp	strm,	/*!< in/out: compressed stream for deflate() */
	int		flush)	/*!< in: deflate() flushing method */
{
	int	status;
	if (UNIV_UNLIKELY(page_zip_compress_dbg)) {
		ut_print_buf(stderr, strm->next_in, strm->avail_in);
	}
	if (UNIV_LIKELY_NULL(logfile)) {
		if (fwrite(strm->next_in, 1, strm->avail_in, logfile)
		    != strm->avail_in) {
			perror("fwrite");
		}
	}
	status = deflate(strm, flush);
	if (UNIV_UNLIKELY(page_zip_compress_dbg)) {
		fprintf(stderr, " -> %d\n", status);
	}
	return(status);
}

/* Redefine deflate(). */
# undef deflate
/** Debug wrapper for the zlib compression routine deflate().
Log the operation if page_zip_compress_dbg is set.
@param strm in/out: compressed stream
@param flush in: flushing method
@return deflate() status: Z_OK, Z_BUF_ERROR, ... */
# define deflate(strm, flush) page_zip_compress_deflate(logfile, strm, flush)
/** Declaration of the logfile parameter */
# define FILE_LOGFILE FILE* logfile,
/** The logfile parameter */
# define LOGFILE logfile,
#else /* PAGE_ZIP_COMPRESS_DBG */
/** Empty declaration of the logfile parameter */
# define FILE_LOGFILE
/** Missing logfile parameter */
# define LOGFILE
#endif /* PAGE_ZIP_COMPRESS_DBG */

/**********************************************************************//**
Compress the records of a node pointer page.
@return Z_OK, or a zlib error code */
static
int
page_zip_compress_node_ptrs(
/*========================*/
	FILE_LOGFILE
	z_stream*	c_stream,	/*!< in/out: compressed page stream */
	const rec_t**	recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense,	/*!< in: size of recs[] */
	dict_index_t*	index,		/*!< in: the index of the page */
	byte*		storage,	/*!< in: end of dense page directory */
	mem_heap_t*	heap)		/*!< in: temporary memory heap */
{
	int	err	= Z_OK;
	rec_offs* offsets = NULL;

	do {
		const rec_t*	rec = *recs++;

		offsets = rec_get_offsets(rec, index, offsets, 0,
					  ULINT_UNDEFINED, &heap);
		/* Only leaf nodes may contain externally stored columns. */
		ut_ad(!rec_offs_any_extern(offsets));

		MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
		MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
				  rec_offs_extra_size(offsets));

		/* Compress the extra bytes. */
		c_stream->avail_in = static_cast<uInt>(
			rec - REC_N_NEW_EXTRA_BYTES - c_stream->next_in);

		if (c_stream->avail_in) {
			err = deflate(c_stream, Z_NO_FLUSH);
			if (UNIV_UNLIKELY(err != Z_OK)) {
				break;
			}
		}
		ut_ad(!c_stream->avail_in);

		/* Compress the data bytes, except node_ptr. */
		c_stream->next_in = (byte*) rec;
		c_stream->avail_in = static_cast<uInt>(
			rec_offs_data_size(offsets) - REC_NODE_PTR_SIZE);

		if (c_stream->avail_in) {
			err = deflate(c_stream, Z_NO_FLUSH);
			if (UNIV_UNLIKELY(err != Z_OK)) {
				break;
			}
		}

		ut_ad(!c_stream->avail_in);

		memcpy(storage - REC_NODE_PTR_SIZE
		       * (rec_get_heap_no_new(rec) - 1),
		       c_stream->next_in, REC_NODE_PTR_SIZE);
		c_stream->next_in += REC_NODE_PTR_SIZE;
	} while (--n_dense);

	return(err);
}

/**********************************************************************//**
Compress the records of a leaf node of a secondary index.
@return Z_OK, or a zlib error code */
static
int
page_zip_compress_sec(
/*==================*/
	FILE_LOGFILE
	z_stream*	c_stream,	/*!< in/out: compressed page stream */
	const rec_t**	recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense)	/*!< in: size of recs[] */
{
	int		err	= Z_OK;

	ut_ad(n_dense > 0);

	do {
		const rec_t*	rec = *recs++;

		/* Compress everything up to this record. */
		c_stream->avail_in = static_cast<uInt>(
			rec - REC_N_NEW_EXTRA_BYTES
			- c_stream->next_in);

		if (UNIV_LIKELY(c_stream->avail_in != 0)) {
			MEM_CHECK_DEFINED(c_stream->next_in,
					  c_stream->avail_in);
			err = deflate(c_stream, Z_NO_FLUSH);
			if (UNIV_UNLIKELY(err != Z_OK)) {
				break;
			}
		}

		ut_ad(!c_stream->avail_in);
		ut_ad(c_stream->next_in == rec - REC_N_NEW_EXTRA_BYTES);

		/* Skip the REC_N_NEW_EXTRA_BYTES. */

		c_stream->next_in = (byte*) rec;
	} while (--n_dense);

	return(err);
}

/**********************************************************************//**
Compress a record of a leaf node of a clustered index that contains
externally stored columns.
@return Z_OK, or a zlib error code */
static
int
page_zip_compress_clust_ext(
/*========================*/
	FILE_LOGFILE
	z_stream*	c_stream,	/*!< in/out: compressed page stream */
	const rec_t*	rec,		/*!< in: record */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec) */
	ulint		trx_id_col,	/*!< in: position of of DB_TRX_ID */
	byte*		deleted,	/*!< in: dense directory entry pointing
					to the head of the free list */
	byte*		storage,	/*!< in: end of dense page directory */
	byte**		externs,	/*!< in/out: pointer to the next
					available BLOB pointer */
	ulint*		n_blobs)	/*!< in/out: number of
					externally stored columns */
{
	int	err;
	ulint	i;

	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		ulint		len;
		const byte*	src;

		if (UNIV_UNLIKELY(i == trx_id_col)) {
			ut_ad(!rec_offs_nth_extern(offsets, i));
			/* Store trx_id and roll_ptr
			in uncompressed form. */
			src = rec_get_nth_field(rec, offsets, i, &len);
			ut_ad(src + DATA_TRX_ID_LEN
			      == rec_get_nth_field(rec, offsets,
						   i + 1, &len));
			ut_ad(len == DATA_ROLL_PTR_LEN);

			/* Compress any preceding bytes. */
			c_stream->avail_in = static_cast<uInt>(
				src - c_stream->next_in);

			if (c_stream->avail_in) {
				err = deflate(c_stream, Z_NO_FLUSH);
				if (UNIV_UNLIKELY(err != Z_OK)) {

					return(err);
				}
			}

			ut_ad(!c_stream->avail_in);
			ut_ad(c_stream->next_in == src);

			memcpy(storage
			       - (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
			       * (rec_get_heap_no_new(rec) - 1),
			       c_stream->next_in,
			       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

			c_stream->next_in
				+= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;

			/* Skip also roll_ptr */
			i++;
		} else if (rec_offs_nth_extern(offsets, i)) {
			src = rec_get_nth_field(rec, offsets, i, &len);
			ut_ad(len >= BTR_EXTERN_FIELD_REF_SIZE);
			src += len - BTR_EXTERN_FIELD_REF_SIZE;

			c_stream->avail_in = static_cast<uInt>(
				src - c_stream->next_in);
			if (UNIV_LIKELY(c_stream->avail_in != 0)) {
				err = deflate(c_stream, Z_NO_FLUSH);
				if (UNIV_UNLIKELY(err != Z_OK)) {

					return(err);
				}
			}

			ut_ad(!c_stream->avail_in);
			ut_ad(c_stream->next_in == src);

			/* Reserve space for the data at
			the end of the space reserved for
			the compressed data and the page
			modification log. */

			if (UNIV_UNLIKELY
			    (c_stream->avail_out
			     <= BTR_EXTERN_FIELD_REF_SIZE)) {
				/* out of space */
				return(Z_BUF_ERROR);
			}

			ut_ad(*externs == c_stream->next_out
			      + c_stream->avail_out
			      + 1/* end of modif. log */);

			c_stream->next_in
				+= BTR_EXTERN_FIELD_REF_SIZE;

			/* Skip deleted records. */
			if (UNIV_LIKELY_NULL
			    (page_zip_dir_find_low(
				    storage, deleted,
				    page_offset(rec)))) {
				continue;
			}

			(*n_blobs)++;
			c_stream->avail_out
				-= BTR_EXTERN_FIELD_REF_SIZE;
			*externs -= BTR_EXTERN_FIELD_REF_SIZE;

			/* Copy the BLOB pointer */
			memcpy(*externs, c_stream->next_in
			       - BTR_EXTERN_FIELD_REF_SIZE,
			       BTR_EXTERN_FIELD_REF_SIZE);
		}
	}

	return(Z_OK);
}

/**********************************************************************//**
Compress the records of a leaf node of a clustered index.
@return Z_OK, or a zlib error code */
static
int
page_zip_compress_clust(
/*====================*/
	FILE_LOGFILE
	z_stream*	c_stream,	/*!< in/out: compressed page stream */
	const rec_t**	recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense,	/*!< in: size of recs[] */
	dict_index_t*	index,		/*!< in: the index of the page */
	ulint*		n_blobs,	/*!< in: 0; out: number of
					externally stored columns */
	ulint		trx_id_col,	/*!< index of the trx_id column */
	byte*		deleted,	/*!< in: dense directory entry pointing
					to the head of the free list */
	byte*		storage,	/*!< in: end of dense page directory */
	mem_heap_t*	heap)		/*!< in: temporary memory heap */
{
	int	err		= Z_OK;
	rec_offs* offsets		= NULL;
	/* BTR_EXTERN_FIELD_REF storage */
	byte*	externs		= storage - n_dense
		* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

	ut_ad(*n_blobs == 0);

	do {
		const rec_t*	rec = *recs++;

		offsets = rec_get_offsets(rec, index, offsets, index->n_fields,
					  ULINT_UNDEFINED, &heap);
		ut_ad(rec_offs_n_fields(offsets)
		      == dict_index_get_n_fields(index));
		MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
		MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
				  rec_offs_extra_size(offsets));

		/* Compress the extra bytes. */
		c_stream->avail_in = static_cast<uInt>(
			rec - REC_N_NEW_EXTRA_BYTES
			- c_stream->next_in);

		if (c_stream->avail_in) {
			err = deflate(c_stream, Z_NO_FLUSH);
			if (UNIV_UNLIKELY(err != Z_OK)) {

				goto func_exit;
			}
		}
		ut_ad(!c_stream->avail_in);
		ut_ad(c_stream->next_in == rec - REC_N_NEW_EXTRA_BYTES);

		/* Compress the data bytes. */

		c_stream->next_in = (byte*) rec;

		/* Check if there are any externally stored columns.
		For each externally stored column, store the
		BTR_EXTERN_FIELD_REF separately. */
		if (rec_offs_any_extern(offsets)) {
			ut_ad(dict_index_is_clust(index));

			err = page_zip_compress_clust_ext(
				LOGFILE
				c_stream, rec, offsets, trx_id_col,
				deleted, storage, &externs, n_blobs);

			if (UNIV_UNLIKELY(err != Z_OK)) {

				goto func_exit;
			}
		} else {
			ulint		len;
			const byte*	src;

			/* Store trx_id and roll_ptr in uncompressed form. */
			src = rec_get_nth_field(rec, offsets,
						trx_id_col, &len);
			ut_ad(src + DATA_TRX_ID_LEN
			      == rec_get_nth_field(rec, offsets,
						   trx_id_col + 1, &len));
			ut_ad(len == DATA_ROLL_PTR_LEN);
			MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
			MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
					  rec_offs_extra_size(offsets));

			/* Compress any preceding bytes. */
			c_stream->avail_in = static_cast<uInt>(
				src - c_stream->next_in);

			if (c_stream->avail_in) {
				err = deflate(c_stream, Z_NO_FLUSH);
				if (UNIV_UNLIKELY(err != Z_OK)) {

					return(err);
				}
			}

			ut_ad(!c_stream->avail_in);
			ut_ad(c_stream->next_in == src);

			memcpy(storage
			       - (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
			       * (rec_get_heap_no_new(rec) - 1),
			       c_stream->next_in,
			       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

			c_stream->next_in
				+= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;

			/* Skip also roll_ptr */
			ut_ad(trx_id_col + 1 < rec_offs_n_fields(offsets));
		}

		/* Compress the last bytes of the record. */
		c_stream->avail_in = static_cast<uInt>(
			rec + rec_offs_data_size(offsets) - c_stream->next_in);

		if (c_stream->avail_in) {
			err = deflate(c_stream, Z_NO_FLUSH);
			if (UNIV_UNLIKELY(err != Z_OK)) {

				goto func_exit;
			}
		}
		ut_ad(!c_stream->avail_in);
	} while (--n_dense);

func_exit:
	return(err);}

/** Attempt to compress a ROW_FORMAT=COMPRESSED page.
@retval true on success
@retval false on failure; block->page.zip will be left intact. */
bool
page_zip_compress(
	buf_block_t*		block,	/*!< in/out: buffer block */
	dict_index_t*		index,	/*!< in: index of the B-tree node */
	ulint			level,	/*!< in: commpression level */
	mtr_t*			mtr)	/*!< in/out: mini-transaction */
{
	z_stream		c_stream;
	int			err;
	byte*			fields;		/*!< index field information */
	byte*			buf;		/*!< compressed payload of the
						page */
	byte*			buf_end;	/* end of buf */
	ulint			n_dense;
	ulint			slot_size;	/* amount of uncompressed bytes
						per record */
	const rec_t**		recs;		/*!< dense page directory,
						sorted by address */
	mem_heap_t*		heap;
	ulint			trx_id_col = ULINT_UNDEFINED;
	ulint			n_blobs	= 0;
	byte*			storage;	/* storage of uncompressed
						columns */
	const ulonglong		ns = my_interval_timer();
#ifdef PAGE_ZIP_COMPRESS_DBG
	FILE*			logfile = NULL;
#endif
	/* A local copy of srv_cmp_per_index_enabled to avoid reading that
	variable multiple times in this function since it can be changed at
	anytime. */
	my_bool			cmp_per_index_enabled;
	cmp_per_index_enabled	= srv_cmp_per_index_enabled;

	page_t* page = block->page.frame;
	page_zip_des_t* page_zip = &block->page.zip;

	ut_a(page_is_comp(page));
	ut_a(fil_page_index_page_check(page));
	ut_ad(page_simple_validate_new((page_t*) page));
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(dict_table_is_comp(index->table));
	ut_ad(!dict_index_is_ibuf(index));

	MEM_CHECK_DEFINED(page, srv_page_size);

	/* Check the data that will be omitted. */
	ut_a(!memcmp(page + (PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES),
		     infimum_extra, sizeof infimum_extra));
	ut_a(!memcmp(page + PAGE_NEW_INFIMUM,
		     infimum_data, sizeof infimum_data));
	ut_a(page[PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES]
	     /* info_bits == 0, n_owned <= max */
	     <= PAGE_DIR_SLOT_MAX_N_OWNED);
	ut_a(!memcmp(page + (PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES + 1),
		     supremum_extra_data, sizeof supremum_extra_data));

	if (page_is_empty(page)) {
		ut_a(rec_get_next_offs(page + PAGE_NEW_INFIMUM, TRUE)
		     == PAGE_NEW_SUPREMUM);
	}

	const ulint n_fields = page_is_leaf(page)
		? dict_index_get_n_fields(index)
		: dict_index_get_n_unique_in_tree_nonleaf(index);
	index_id_t ind_id = index->id;

	/* The dense directory excludes the infimum and supremum records. */
	n_dense = ulint(page_dir_get_n_heap(page)) - PAGE_HEAP_NO_USER_LOW;
#ifdef PAGE_ZIP_COMPRESS_DBG
	if (UNIV_UNLIKELY(page_zip_compress_dbg)) {
		ib::info() << "compress "
			<< static_cast<void*>(page_zip) << " "
			<< static_cast<const void*>(page) << " "
			<< page_is_leaf(page) << " "
			<< n_fields << " " << n_dense;
	}

	if (UNIV_UNLIKELY(page_zip_compress_log)) {
		/* Create a log file for every compression attempt. */
		char	logfilename[9];
		snprintf(logfilename, sizeof logfilename,
			 "%08x", page_zip_compress_log++);
		logfile = fopen(logfilename, "wb");

		if (logfile) {
			/* Write the uncompressed page to the log. */
			if (fwrite(page, 1, srv_page_size, logfile)
			    != srv_page_size) {
				perror("fwrite");
			}
			/* Record the compressed size as zero.
			This will be overwritten at successful exit. */
			putc(0, logfile);
			putc(0, logfile);
			putc(0, logfile);
			putc(0, logfile);
		}
	}
#endif /* PAGE_ZIP_COMPRESS_DBG */
	page_zip_stat[page_zip->ssize - 1].compressed++;
	if (cmp_per_index_enabled) {
		mysql_mutex_lock(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index[ind_id].compressed++;
		mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
	}

	if (UNIV_UNLIKELY(n_dense * PAGE_ZIP_DIR_SLOT_SIZE
			  >= page_zip_get_size(page_zip))) {

		goto err_exit;
	}

	MONITOR_INC(MONITOR_PAGE_COMPRESS);

	heap = mem_heap_create(page_zip_get_size(page_zip)
			       + n_fields * (2 + sizeof(ulint))
			       + REC_OFFS_HEADER_SIZE
			       + n_dense * ((sizeof *recs)
					    - PAGE_ZIP_DIR_SLOT_SIZE)
			       + srv_page_size * 4
			       + (512 << MAX_MEM_LEVEL));

	recs = static_cast<const rec_t**>(
		mem_heap_zalloc(heap, n_dense * sizeof *recs));

	fields = static_cast<byte*>(mem_heap_alloc(heap, (n_fields + 1) * 2));

	buf = static_cast<byte*>(
		mem_heap_alloc(heap, page_zip_get_size(page_zip) - PAGE_DATA));

	buf_end = buf + page_zip_get_size(page_zip) - PAGE_DATA;

	/* Compress the data payload. */
	page_zip_set_alloc(&c_stream, heap);

	err = deflateInit2(&c_stream, static_cast<int>(level),
			   Z_DEFLATED, static_cast<int>(srv_page_size_shift),
			   MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY);
	ut_a(err == Z_OK);

	c_stream.next_out = buf;

	/* Subtract the space reserved for uncompressed data. */
	/* Page header and the end marker of the modification log */
	c_stream.avail_out = static_cast<uInt>(buf_end - buf - 1);

	/* Dense page directory and uncompressed columns, if any */
	if (page_is_leaf(page)) {
		if (dict_index_is_clust(index)) {
			trx_id_col = index->db_trx_id();

			slot_size = PAGE_ZIP_DIR_SLOT_SIZE
				+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;

		} else {
			/* Signal the absence of trx_id
			in page_zip_fields_encode() */
			trx_id_col = 0;
			slot_size = PAGE_ZIP_DIR_SLOT_SIZE;
		}
	} else {
		slot_size = PAGE_ZIP_DIR_SLOT_SIZE + REC_NODE_PTR_SIZE;
		trx_id_col = ULINT_UNDEFINED;
	}

	if (UNIV_UNLIKELY(c_stream.avail_out <= n_dense * slot_size
			  + 6/* sizeof(zlib header and footer) */)) {
		goto zlib_error;
	}

	c_stream.avail_out -= uInt(n_dense * slot_size);
	c_stream.avail_in = uInt(page_zip_fields_encode(n_fields, index,
							trx_id_col, fields));
	c_stream.next_in = fields;

	if (UNIV_LIKELY(!trx_id_col)) {
		trx_id_col = ULINT_UNDEFINED;
	}

	MEM_CHECK_DEFINED(c_stream.next_in, c_stream.avail_in);
	err = deflate(&c_stream, Z_FULL_FLUSH);
	if (err != Z_OK) {
		goto zlib_error;
	}

	ut_ad(!c_stream.avail_in);

	page_zip_dir_encode(page, buf_end, recs);

	c_stream.next_in = (byte*) page + PAGE_ZIP_START;

	storage = buf_end - n_dense * PAGE_ZIP_DIR_SLOT_SIZE;

	/* Compress the records in heap_no order. */
	if (UNIV_UNLIKELY(!n_dense)) {
	} else if (!page_is_leaf(page)) {
		/* This is a node pointer page. */
		err = page_zip_compress_node_ptrs(LOGFILE
						  &c_stream, recs, n_dense,
						  index, storage, heap);
		if (UNIV_UNLIKELY(err != Z_OK)) {
			goto zlib_error;
		}
	} else if (UNIV_LIKELY(trx_id_col == ULINT_UNDEFINED)) {
		/* This is a leaf page in a secondary index. */
		err = page_zip_compress_sec(LOGFILE
					    &c_stream, recs, n_dense);
		if (UNIV_UNLIKELY(err != Z_OK)) {
			goto zlib_error;
		}
	} else {
		/* This is a leaf page in a clustered index. */
		err = page_zip_compress_clust(LOGFILE
					      &c_stream, recs, n_dense,
					      index, &n_blobs, trx_id_col,
					      buf_end - PAGE_ZIP_DIR_SLOT_SIZE
					      * page_get_n_recs(page),
					      storage, heap);
		if (UNIV_UNLIKELY(err != Z_OK)) {
			goto zlib_error;
		}
	}

	/* Finish the compression. */
	ut_ad(!c_stream.avail_in);
	/* Compress any trailing garbage, in case the last record was
	allocated from an originally longer space on the free list,
	or the data of the last record from page_zip_compress_sec(). */
	c_stream.avail_in = static_cast<uInt>(
		page_header_get_field(page, PAGE_HEAP_TOP)
		- (c_stream.next_in - page));
	ut_a(c_stream.avail_in <= srv_page_size - PAGE_ZIP_START - PAGE_DIR);

	MEM_CHECK_DEFINED(c_stream.next_in, c_stream.avail_in);
	err = deflate(&c_stream, Z_FINISH);

	if (UNIV_UNLIKELY(err != Z_STREAM_END)) {
zlib_error:
		deflateEnd(&c_stream);
		mem_heap_free(heap);
err_exit:
#ifdef PAGE_ZIP_COMPRESS_DBG
		if (logfile) {
			fclose(logfile);
		}
#endif /* PAGE_ZIP_COMPRESS_DBG */
		if (page_is_leaf(page)) {
			dict_index_zip_failure(index);
		}

		const uint64_t time_diff = (my_interval_timer() - ns) / 1000;
		page_zip_stat[page_zip->ssize - 1].compressed_usec
			+= time_diff;
		if (cmp_per_index_enabled) {
			mysql_mutex_lock(&page_zip_stat_per_index_mutex);
			page_zip_stat_per_index[ind_id].compressed_usec
				+= time_diff;
			mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
		}
		return false;
	}

	err = deflateEnd(&c_stream);
	ut_a(err == Z_OK);

	ut_ad(buf + c_stream.total_out == c_stream.next_out);
	ut_ad((ulint) (storage - c_stream.next_out) >= c_stream.avail_out);

#if defined HAVE_valgrind && !__has_feature(memory_sanitizer)
	/* Valgrind believes that zlib does not initialize some bits
	in the last 7 or 8 bytes of the stream.  Make Valgrind happy. */
	MEM_MAKE_DEFINED(buf, c_stream.total_out);
#endif /* HAVE_valgrind && !memory_sanitizer */

	/* Zero out the area reserved for the modification log.
	Space for the end marker of the modification log is not
	included in avail_out. */
	memset(c_stream.next_out, 0, c_stream.avail_out + 1/* end marker */);

#ifdef UNIV_DEBUG
	page_zip->m_start =
#endif /* UNIV_DEBUG */
		page_zip->m_end = uint16_t(PAGE_DATA + c_stream.total_out);
	page_zip->m_nonempty = FALSE;
	page_zip->n_blobs = unsigned(n_blobs) & ((1U << 12) - 1);
	/* Copy those header fields that will not be written
	in buf_flush_init_for_writing() */
	memcpy_aligned<8>(page_zip->data + FIL_PAGE_PREV, page + FIL_PAGE_PREV,
			  FIL_PAGE_LSN - FIL_PAGE_PREV);
	memcpy_aligned<2>(page_zip->data + FIL_PAGE_TYPE, page + FIL_PAGE_TYPE,
			  2);
	memcpy_aligned<2>(page_zip->data + FIL_PAGE_DATA, page + FIL_PAGE_DATA,
			  PAGE_DATA - FIL_PAGE_DATA);
	/* Copy the rest of the compressed page */
	memcpy_aligned<2>(page_zip->data + PAGE_DATA, buf,
			  page_zip_get_size(page_zip) - PAGE_DATA);
	mem_heap_free(heap);
#ifdef UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	page_zip_compress_write_log(block, index, mtr);

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

#ifdef PAGE_ZIP_COMPRESS_DBG
	if (logfile) {
		/* Record the compressed size of the block. */
		byte sz[4];
		mach_write_to_4(sz, c_stream.total_out);
		fseek(logfile, srv_page_size, SEEK_SET);
		if (fwrite(sz, 1, sizeof sz, logfile) != sizeof sz) {
			perror("fwrite");
		}
		fclose(logfile);
	}
#endif /* PAGE_ZIP_COMPRESS_DBG */
	const uint64_t time_diff = (my_interval_timer() - ns) / 1000;
	page_zip_stat[page_zip->ssize - 1].compressed_ok++;
	page_zip_stat[page_zip->ssize - 1].compressed_usec += time_diff;
	if (cmp_per_index_enabled) {
		mysql_mutex_lock(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index[ind_id].compressed_ok++;
		page_zip_stat_per_index[ind_id].compressed_usec += time_diff;
		mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
	}

	if (page_is_leaf(page)) {
		dict_index_zip_success(index);
	}

	return true;
}

/**********************************************************************//**
Deallocate the index information initialized by page_zip_fields_decode(). */
static
void
page_zip_fields_free(
/*=================*/
	dict_index_t*	index)	/*!< in: dummy index to be freed */
{
	if (index) {
		dict_table_t*	table = index->table;
		index->zip_pad.mutex.~mutex();
		mem_heap_free(index->heap);

		dict_mem_table_free(table);
	}
}

/**********************************************************************//**
Read the index information for the compressed page.
@return own: dummy index describing the page, or NULL on error */
static
dict_index_t*
page_zip_fields_decode(
/*===================*/
	const byte*	buf,	/*!< in: index information */
	const byte*	end,	/*!< in: end of buf */
	ulint*		trx_id_col,/*!< in: NULL for non-leaf pages;
				for leaf pages, pointer to where to store
				the position of the trx_id column */
	bool		is_spatial)/*< in: is spatial index or not */
{
	const byte*	b;
	ulint		n;
	ulint		i;
	ulint		val;
	dict_table_t*	table;
	dict_index_t*	index;

	/* Determine the number of fields. */
	for (b = buf, n = 0; b < end; n++) {
		if (*b++ & 0x80) {
			b++; /* skip the second byte */
		}
	}

	n--; /* n_nullable or trx_id */

	if (UNIV_UNLIKELY(n > REC_MAX_N_FIELDS)) {

		page_zip_fail(("page_zip_fields_decode: n = %lu\n",
			       (ulong) n));
		return(NULL);
	}

	if (UNIV_UNLIKELY(b > end)) {

		page_zip_fail(("page_zip_fields_decode: %p > %p\n",
			       (const void*) b, (const void*) end));
		return(NULL);
	}

	table = dict_table_t::create({C_STRING_WITH_LEN("ZIP_DUMMY")},
				     nullptr, n, 0, DICT_TF_COMPACT, 0);
	index = dict_mem_index_create(table, "ZIP_DUMMY", 0, n);
	index->n_uniq = static_cast<unsigned>(n) & dict_index_t::MAX_N_FIELDS;
	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	index->cached = TRUE;

	/* Initialize the fields. */
	for (b = buf, i = 0; i < n; i++) {
		ulint	mtype;
		ulint	len;

		val = *b++;

		if (UNIV_UNLIKELY(val & 0x80)) {
			/* fixed length > 62 bytes */
			val = (val & 0x7f) << 8 | *b++;
			len = val >> 1;
			mtype = DATA_FIXBINARY;
		} else if (UNIV_UNLIKELY(val >= 126)) {
			/* variable length with max > 255 bytes */
			len = 0x7fff;
			mtype = DATA_BINARY;
		} else if (val <= 1) {
			/* variable length with max <= 255 bytes */
			len = 0;
			mtype = DATA_BINARY;
		} else {
			/* fixed length < 62 bytes */
			len = val >> 1;
			mtype = DATA_FIXBINARY;
		}

		dict_mem_table_add_col(table, NULL, NULL, mtype,
				       val & 1 ? DATA_NOT_NULL : 0, len);
		dict_index_add_col(index, table,
				   dict_table_get_nth_col(table, i), 0);
	}

	val = *b++;
	if (UNIV_UNLIKELY(val & 0x80)) {
		val = (val & 0x7f) << 8 | *b++;
	}

	/* Decode the position of the trx_id column. */
	if (trx_id_col) {
		if (!val) {
			val = ULINT_UNDEFINED;
		} else if (UNIV_UNLIKELY(val >= n)) {
fail:
			page_zip_fields_free(index);
			return NULL;
		} else {
			index->type = DICT_CLUSTERED;
		}

		*trx_id_col = val;
	} else {
		/* Decode the number of nullable fields. */
		if (UNIV_UNLIKELY(index->n_nullable > val)) {
			goto fail;
		} else {
			index->n_nullable = static_cast<unsigned>(val)
				& dict_index_t::MAX_N_FIELDS;
		}
	}

	/* ROW_FORMAT=COMPRESSED does not support instant ADD COLUMN */
	index->n_core_fields = index->n_fields;
	index->n_core_null_bytes = static_cast<uint8_t>(
		UT_BITS_IN_BYTES(unsigned(index->n_nullable)));

	ut_ad(b == end);

	if (is_spatial) {
		index->type |= DICT_SPATIAL;
	}

	return(index);
}

/**********************************************************************//**
Populate the sparse page directory from the dense directory.
@return TRUE on success, FALSE on failure */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
ibool
page_zip_dir_decode(
/*================*/
	const page_zip_des_t*	page_zip,/*!< in: dense page directory on
					compressed page */
	page_t*			page,	/*!< in: compact page with valid header;
					out: trailer and sparse page directory
					filled in */
	rec_t**			recs,	/*!< out: dense page directory sorted by
					ascending address (and heap_no) */
	ulint			n_dense)/*!< in: number of user records, and
					size of recs[] */
{
	ulint	i;
	ulint	n_recs;
	byte*	slot;

	n_recs = page_get_n_recs(page);

	if (UNIV_UNLIKELY(n_recs > n_dense)) {
		page_zip_fail(("page_zip_dir_decode 1: %lu > %lu\n",
			       (ulong) n_recs, (ulong) n_dense));
		return(FALSE);
	}

	/* Traverse the list of stored records in the sorting order,
	starting from the first user record. */

	slot = page + (srv_page_size - PAGE_DIR - PAGE_DIR_SLOT_SIZE);
	UNIV_PREFETCH_RW(slot);

	/* Zero out the page trailer. */
	memset(slot + PAGE_DIR_SLOT_SIZE, 0, PAGE_DIR);

	mach_write_to_2(slot, PAGE_NEW_INFIMUM);
	slot -= PAGE_DIR_SLOT_SIZE;
	UNIV_PREFETCH_RW(slot);

	/* Initialize the sparse directory and copy the dense directory. */
	for (i = 0; i < n_recs; i++) {
		ulint	offs = page_zip_dir_get(page_zip, i);

		if (offs & PAGE_ZIP_DIR_SLOT_OWNED) {
			mach_write_to_2(slot, offs & PAGE_ZIP_DIR_SLOT_MASK);
			slot -= PAGE_DIR_SLOT_SIZE;
			UNIV_PREFETCH_RW(slot);
		}

		if (UNIV_UNLIKELY((offs & PAGE_ZIP_DIR_SLOT_MASK)
				  < PAGE_ZIP_START + REC_N_NEW_EXTRA_BYTES)) {
			page_zip_fail(("page_zip_dir_decode 2: %u %u %lx\n",
				       (unsigned) i, (unsigned) n_recs,
				       (ulong) offs));
			return(FALSE);
		}

		recs[i] = page + (offs & PAGE_ZIP_DIR_SLOT_MASK);
	}

	mach_write_to_2(slot, PAGE_NEW_SUPREMUM);
	{
		const page_dir_slot_t*	last_slot = page_dir_get_nth_slot(
			page, page_dir_get_n_slots(page) - 1U);

		if (UNIV_UNLIKELY(slot != last_slot)) {
			page_zip_fail(("page_zip_dir_decode 3: %p != %p\n",
				       (const void*) slot,
				       (const void*) last_slot));
			return(FALSE);
		}
	}

	/* Copy the rest of the dense directory. */
	for (; i < n_dense; i++) {
		ulint	offs = page_zip_dir_get(page_zip, i);

		if (UNIV_UNLIKELY(offs & ~PAGE_ZIP_DIR_SLOT_MASK)) {
			page_zip_fail(("page_zip_dir_decode 4: %u %u %lx\n",
				       (unsigned) i, (unsigned) n_dense,
				       (ulong) offs));
			return(FALSE);
		}

		recs[i] = page + offs;
	}

	std::sort(recs, recs + n_dense);
	return(TRUE);
}

/**********************************************************************//**
Initialize the REC_N_NEW_EXTRA_BYTES of each record.
@return TRUE on success, FALSE on failure */
static
ibool
page_zip_set_extra_bytes(
/*=====================*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	page_t*			page,	/*!< in/out: uncompressed page */
	ulint			info_bits)/*!< in: REC_INFO_MIN_REC_FLAG or 0 */
{
	ulint	n;
	ulint	i;
	ulint	n_owned = 1;
	ulint	offs;
	rec_t*	rec;

	n = page_get_n_recs(page);
	rec = page + PAGE_NEW_INFIMUM;

	for (i = 0; i < n; i++) {
		offs = page_zip_dir_get(page_zip, i);

		if (offs & PAGE_ZIP_DIR_SLOT_DEL) {
			info_bits |= REC_INFO_DELETED_FLAG;
		}
		if (UNIV_UNLIKELY(offs & PAGE_ZIP_DIR_SLOT_OWNED)) {
			info_bits |= n_owned;
			n_owned = 1;
		} else {
			n_owned++;
		}
		offs &= PAGE_ZIP_DIR_SLOT_MASK;
		if (UNIV_UNLIKELY(offs < PAGE_ZIP_START
				  + REC_N_NEW_EXTRA_BYTES)) {
			page_zip_fail(("page_zip_set_extra_bytes 1:"
				       " %u %u %lx\n",
				       (unsigned) i, (unsigned) n,
				       (ulong) offs));
			return(FALSE);
		}

		rec_set_next_offs_new(rec, offs);
		rec = page + offs;
		rec[-REC_N_NEW_EXTRA_BYTES] = (byte) info_bits;
		info_bits = 0;
	}

	/* Set the next pointer of the last user record. */
	rec_set_next_offs_new(rec, PAGE_NEW_SUPREMUM);

	/* Set n_owned of the supremum record. */
	page[PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES] = (byte) n_owned;

	/* The dense directory excludes the infimum and supremum records. */
	n = ulint(page_dir_get_n_heap(page)) - PAGE_HEAP_NO_USER_LOW;

	if (i >= n) {
		if (UNIV_LIKELY(i == n)) {
			return(TRUE);
		}

		page_zip_fail(("page_zip_set_extra_bytes 2: %u != %u\n",
			       (unsigned) i, (unsigned) n));
		return(FALSE);
	}

	offs = page_zip_dir_get(page_zip, i);

	/* Set the extra bytes of deleted records on the free list. */
	for (;;) {
		if (UNIV_UNLIKELY(!offs)
		    || UNIV_UNLIKELY(offs & ~PAGE_ZIP_DIR_SLOT_MASK)) {

			page_zip_fail(("page_zip_set_extra_bytes 3: %lx\n",
				       (ulong) offs));
			return(FALSE);
		}

		rec = page + offs;
		rec[-REC_N_NEW_EXTRA_BYTES] = 0; /* info_bits and n_owned */

		if (++i == n) {
			break;
		}

		offs = page_zip_dir_get(page_zip, i);
		rec_set_next_offs_new(rec, offs);
	}

	/* Terminate the free list. */
	rec[-REC_N_NEW_EXTRA_BYTES] = 0; /* info_bits and n_owned */
	rec_set_next_offs_new(rec, 0);

	return(TRUE);
}

/**********************************************************************//**
Apply the modification log to a record containing externally stored
columns.  Do not copy the fields that are stored separately.
@return pointer to modification log, or NULL on failure */
static
const byte*
page_zip_apply_log_ext(
/*===================*/
	rec_t*		rec,		/*!< in/out: record */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec) */
	ulint		trx_id_col,	/*!< in: position of of DB_TRX_ID */
	const byte*	data,		/*!< in: modification log */
	const byte*	end)		/*!< in: end of modification log */
{
	ulint	i;
	ulint	len;
	byte*	next_out = rec;

	/* Check if there are any externally stored columns.
	For each externally stored column, skip the
	BTR_EXTERN_FIELD_REF. */

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		byte*	dst;

		if (UNIV_UNLIKELY(i == trx_id_col)) {
			/* Skip trx_id and roll_ptr */
			dst = rec_get_nth_field(rec, offsets,
						i, &len);
			if (UNIV_UNLIKELY(dst - next_out >= end - data)
			    || UNIV_UNLIKELY
			    (len < (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN))
			    || rec_offs_nth_extern(offsets, i)) {
				page_zip_fail(("page_zip_apply_log_ext:"
					       " trx_id len %lu,"
					       " %p - %p >= %p - %p\n",
					       (ulong) len,
					       (const void*) dst,
					       (const void*) next_out,
					       (const void*) end,
					       (const void*) data));
				return(NULL);
			}

			memcpy(next_out, data, ulint(dst - next_out));
			data += ulint(dst - next_out);
			next_out = dst + (DATA_TRX_ID_LEN
					  + DATA_ROLL_PTR_LEN);
		} else if (rec_offs_nth_extern(offsets, i)) {
			dst = rec_get_nth_field(rec, offsets,
						i, &len);
			ut_ad(len
			      >= BTR_EXTERN_FIELD_REF_SIZE);

			len += ulint(dst - next_out)
				- BTR_EXTERN_FIELD_REF_SIZE;

			if (UNIV_UNLIKELY(data + len >= end)) {
				page_zip_fail(("page_zip_apply_log_ext:"
					       " ext %p+%lu >= %p\n",
					       (const void*) data,
					       (ulong) len,
					       (const void*) end));
				return(NULL);
			}

			memcpy(next_out, data, len);
			data += len;
			next_out += len
				+ BTR_EXTERN_FIELD_REF_SIZE;
		}
	}

	/* Copy the last bytes of the record. */
	len = ulint(rec_get_end(rec, offsets) - next_out);
	if (UNIV_UNLIKELY(data + len >= end)) {
		page_zip_fail(("page_zip_apply_log_ext:"
			       " last %p+%lu >= %p\n",
			       (const void*) data,
			       (ulong) len,
			       (const void*) end));
		return(NULL);
	}
	memcpy(next_out, data, len);
	data += len;

	return(data);
}

/**********************************************************************//**
Apply the modification log to an uncompressed page.
Do not copy the fields that are stored separately.
@return pointer to end of modification log, or NULL on failure */
static
const byte*
page_zip_apply_log(
/*===============*/
	const byte*	data,	/*!< in: modification log */
	ulint		size,	/*!< in: maximum length of the log, in bytes */
	rec_t**		recs,	/*!< in: dense page directory,
				sorted by address (indexed by
				heap_no - PAGE_HEAP_NO_USER_LOW) */
	ulint		n_dense,/*!< in: size of recs[] */
	ulint		n_core,	/*!< in: index->n_fields, or 0 for non-leaf */
	ulint		trx_id_col,/*!< in: column number of trx_id in the index,
				or ULINT_UNDEFINED if none */
	ulint		heap_status,
				/*!< in: heap_no and status bits for
				the next record to uncompress */
	dict_index_t*	index,	/*!< in: index of the page */
	rec_offs*	offsets)/*!< in/out: work area for
				rec_get_offsets_reverse() */
{
	const byte* const end = data + size;

	for (;;) {
		ulint	val;
		rec_t*	rec;
		ulint	len;
		ulint	hs;

		val = *data++;
		if (UNIV_UNLIKELY(!val)) {
			return(data - 1);
		}
		if (val & 0x80) {
			val = (val & 0x7f) << 8 | *data++;
			if (UNIV_UNLIKELY(!val)) {
				page_zip_fail(("page_zip_apply_log:"
					       " invalid val %x%x\n",
					       data[-2], data[-1]));
				return(NULL);
			}
		}
		if (UNIV_UNLIKELY(data >= end)) {
			page_zip_fail(("page_zip_apply_log: %p >= %p\n",
				       (const void*) data,
				       (const void*) end));
			return(NULL);
		}
		if (UNIV_UNLIKELY((val >> 1) > n_dense)) {
			page_zip_fail(("page_zip_apply_log: %lu>>1 > %lu\n",
				       (ulong) val, (ulong) n_dense));
			return(NULL);
		}

		/* Determine the heap number and status bits of the record. */
		rec = recs[(val >> 1) - 1];

		hs = ((val >> 1) + 1) << REC_HEAP_NO_SHIFT;
		hs |= heap_status & ((1 << REC_HEAP_NO_SHIFT) - 1);

		/* This may either be an old record that is being
		overwritten (updated in place, or allocated from
		the free list), or a new record, with the next
		available_heap_no. */
		if (UNIV_UNLIKELY(hs > heap_status)) {
			page_zip_fail(("page_zip_apply_log: %lu > %lu\n",
				       (ulong) hs, (ulong) heap_status));
			return(NULL);
		} else if (hs == heap_status) {
			/* A new record was allocated from the heap. */
			if (UNIV_UNLIKELY(val & 1)) {
				/* Only existing records may be cleared. */
				page_zip_fail(("page_zip_apply_log:"
					       " attempting to create"
					       " deleted rec %lu\n",
					       (ulong) hs));
				return(NULL);
			}
			heap_status += 1 << REC_HEAP_NO_SHIFT;
		}

		mach_write_to_2(rec - REC_NEW_HEAP_NO, hs);

		if (val & 1) {
			/* Clear the data bytes of the record. */
			mem_heap_t*	heap	= NULL;
			rec_offs*	offs;
			offs = rec_get_offsets(rec, index, offsets, n_core,
					       ULINT_UNDEFINED, &heap);
			memset(rec, 0, rec_offs_data_size(offs));

			if (UNIV_LIKELY_NULL(heap)) {
				mem_heap_free(heap);
			}
			continue;
		}

		compile_time_assert(REC_STATUS_NODE_PTR == TRUE);
		rec_get_offsets_reverse(data, index,
					hs & REC_STATUS_NODE_PTR,
					offsets);
		/* Silence a debug assertion in rec_offs_make_valid().
		This will be overwritten in page_zip_set_extra_bytes(),
		called by page_zip_decompress_low(). */
		ut_d(rec[-REC_NEW_INFO_BITS] = 0);
		rec_offs_make_valid(rec, index, n_core != 0, offsets);

		/* Copy the extra bytes (backwards). */
		{
			byte*	start	= rec_get_start(rec, offsets);
			byte*	b	= rec - REC_N_NEW_EXTRA_BYTES;
			while (b != start) {
				*--b = *data++;
			}
		}

		/* Copy the data bytes. */
		if (UNIV_UNLIKELY(rec_offs_any_extern(offsets))) {
			/* Non-leaf nodes should not contain any
			externally stored columns. */
			if (UNIV_UNLIKELY(hs & REC_STATUS_NODE_PTR)) {
				page_zip_fail(("page_zip_apply_log:"
					       " %lu&REC_STATUS_NODE_PTR\n",
					       (ulong) hs));
				return(NULL);
			}

			data = page_zip_apply_log_ext(
				rec, offsets, trx_id_col, data, end);

			if (UNIV_UNLIKELY(!data)) {
				return(NULL);
			}
		} else if (UNIV_UNLIKELY(hs & REC_STATUS_NODE_PTR)) {
			len = rec_offs_data_size(offsets)
				- REC_NODE_PTR_SIZE;
			/* Copy the data bytes, except node_ptr. */
			if (UNIV_UNLIKELY(data + len >= end)) {
				page_zip_fail(("page_zip_apply_log:"
					       " node_ptr %p+%lu >= %p\n",
					       (const void*) data,
					       (ulong) len,
					       (const void*) end));
				return(NULL);
			}
			memcpy(rec, data, len);
			data += len;
		} else if (UNIV_LIKELY(trx_id_col == ULINT_UNDEFINED)) {
			len = rec_offs_data_size(offsets);

			/* Copy all data bytes of
			a record in a secondary index. */
			if (UNIV_UNLIKELY(data + len >= end)) {
				page_zip_fail(("page_zip_apply_log:"
					       " sec %p+%lu >= %p\n",
					       (const void*) data,
					       (ulong) len,
					       (const void*) end));
				return(NULL);
			}

			memcpy(rec, data, len);
			data += len;
		} else {
			/* Skip DB_TRX_ID and DB_ROLL_PTR. */
			ulint	l = rec_get_nth_field_offs(offsets,
							   trx_id_col, &len);
			byte*	b;

			if (UNIV_UNLIKELY(data + l >= end)
			    || UNIV_UNLIKELY(len < (DATA_TRX_ID_LEN
						    + DATA_ROLL_PTR_LEN))) {
				page_zip_fail(("page_zip_apply_log:"
					       " trx_id %p+%lu >= %p\n",
					       (const void*) data,
					       (ulong) l,
					       (const void*) end));
				return(NULL);
			}

			/* Copy any preceding data bytes. */
			memcpy(rec, data, l);
			data += l;

			/* Copy any bytes following DB_TRX_ID, DB_ROLL_PTR. */
			b = rec + l + (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			len = ulint(rec_get_end(rec, offsets) - b);
			if (UNIV_UNLIKELY(data + len >= end)) {
				page_zip_fail(("page_zip_apply_log:"
					       " clust %p+%lu >= %p\n",
					       (const void*) data,
					       (ulong) len,
					       (const void*) end));
				return(NULL);
			}
			memcpy(b, data, len);
			data += len;
		}
	}
}

/**********************************************************************//**
Set the heap_no in a record, and skip the fixed-size record header
that is not included in the d_stream.
@return TRUE on success, FALSE if d_stream does not end at rec */
static
ibool
page_zip_decompress_heap_no(
/*========================*/
	z_stream*	d_stream,	/*!< in/out: compressed page stream */
	rec_t*		rec,		/*!< in/out: record */
	ulint&		heap_status)	/*!< in/out: heap_no and status bits */
{
	if (d_stream->next_out != rec - REC_N_NEW_EXTRA_BYTES) {
		/* n_dense has grown since the page was last compressed. */
		return(FALSE);
	}

	/* Skip the REC_N_NEW_EXTRA_BYTES. */
	d_stream->next_out = rec;

	/* Set heap_no and the status bits. */
	mach_write_to_2(rec - REC_NEW_HEAP_NO, heap_status);
	heap_status += 1 << REC_HEAP_NO_SHIFT;
	return(TRUE);
}

/**********************************************************************//**
Decompress the records of a node pointer page.
@return TRUE on success, FALSE on failure */
static
ibool
page_zip_decompress_node_ptrs(
/*==========================*/
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page */
	z_stream*	d_stream,	/*!< in/out: compressed page stream */
	rec_t**		recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense,	/*!< in: size of recs[] */
	dict_index_t*	index,		/*!< in: the index of the page */
	rec_offs*	offsets,	/*!< in/out: temporary offsets */
	mem_heap_t*	heap)		/*!< in: temporary memory heap */
{
	ulint		heap_status = REC_STATUS_NODE_PTR
		| PAGE_HEAP_NO_USER_LOW << REC_HEAP_NO_SHIFT;
	ulint		slot;
	const byte*	storage;

	/* Subtract the space reserved for uncompressed data. */
	d_stream->avail_in -= static_cast<uInt>(
		n_dense * (PAGE_ZIP_DIR_SLOT_SIZE + REC_NODE_PTR_SIZE));

	/* Decompress the records in heap_no order. */
	for (slot = 0; slot < n_dense; slot++) {
		rec_t*	rec = recs[slot];

		d_stream->avail_out = static_cast<uInt>(
			rec - REC_N_NEW_EXTRA_BYTES - d_stream->next_out);

		ut_ad(d_stream->avail_out < srv_page_size
		      - PAGE_ZIP_START - PAGE_DIR);
		switch (inflate(d_stream, Z_SYNC_FLUSH)) {
		case Z_STREAM_END:
			page_zip_decompress_heap_no(
				d_stream, rec, heap_status);
			goto zlib_done;
		case Z_OK:
		case Z_BUF_ERROR:
			if (!d_stream->avail_out) {
				break;
			}
			/* fall through */
		default:
			page_zip_fail(("page_zip_decompress_node_ptrs:"
				       " 1 inflate(Z_SYNC_FLUSH)=%s\n",
				       d_stream->msg));
			goto zlib_error;
		}

		if (!page_zip_decompress_heap_no(
			    d_stream, rec, heap_status)) {
			ut_ad(0);
		}

		/* Read the offsets. The status bits are needed here. */
		offsets = rec_get_offsets(rec, index, offsets, 0,
					  ULINT_UNDEFINED, &heap);

		/* Non-leaf nodes should not have any externally
		stored columns. */
		ut_ad(!rec_offs_any_extern(offsets));

		/* Decompress the data bytes, except node_ptr. */
		d_stream->avail_out =static_cast<uInt>(
			rec_offs_data_size(offsets) - REC_NODE_PTR_SIZE);

		switch (inflate(d_stream, Z_SYNC_FLUSH)) {
		case Z_STREAM_END:
			goto zlib_done;
		case Z_OK:
		case Z_BUF_ERROR:
			if (!d_stream->avail_out) {
				break;
			}
			/* fall through */
		default:
			page_zip_fail(("page_zip_decompress_node_ptrs:"
				       " 2 inflate(Z_SYNC_FLUSH)=%s\n",
				       d_stream->msg));
			goto zlib_error;
		}

		/* Clear the node pointer in case the record
		will be deleted and the space will be reallocated
		to a smaller record. */
		memset(d_stream->next_out, 0, REC_NODE_PTR_SIZE);
		d_stream->next_out += REC_NODE_PTR_SIZE;

		ut_ad(d_stream->next_out == rec_get_end(rec, offsets));
	}

	/* Decompress any trailing garbage, in case the last record was
	allocated from an originally longer space on the free list. */
	d_stream->avail_out = static_cast<uInt>(
		page_header_get_field(page_zip->data, PAGE_HEAP_TOP)
		- page_offset(d_stream->next_out));
	if (UNIV_UNLIKELY(d_stream->avail_out > srv_page_size
			  - PAGE_ZIP_START - PAGE_DIR)) {

		page_zip_fail(("page_zip_decompress_node_ptrs:"
			       " avail_out = %u\n",
			       d_stream->avail_out));
		goto zlib_error;
	}

	if (UNIV_UNLIKELY(inflate(d_stream, Z_FINISH) != Z_STREAM_END)) {
		page_zip_fail(("page_zip_decompress_node_ptrs:"
			       " inflate(Z_FINISH)=%s\n",
			       d_stream->msg));
zlib_error:
		inflateEnd(d_stream);
		return(FALSE);
	}

	/* Note that d_stream->avail_out > 0 may hold here
	if the modification log is nonempty. */

zlib_done:
	if (UNIV_UNLIKELY(inflateEnd(d_stream) != Z_OK)) {
		ut_error;
	}

	{
		page_t*	page = page_align(d_stream->next_out);

		/* Clear the unused heap space on the uncompressed page. */
		memset(d_stream->next_out, 0,
		       ulint(page_dir_get_nth_slot(page,
						   page_dir_get_n_slots(page)
						   - 1U)
			     - d_stream->next_out));
	}

#ifdef UNIV_DEBUG
	page_zip->m_start = uint16_t(PAGE_DATA + d_stream->total_in);
#endif /* UNIV_DEBUG */

	/* Apply the modification log. */
	{
		const byte*	mod_log_ptr;
		mod_log_ptr = page_zip_apply_log(d_stream->next_in,
						 d_stream->avail_in + 1,
						 recs, n_dense, 0,
						 ULINT_UNDEFINED, heap_status,
						 index, offsets);

		if (UNIV_UNLIKELY(!mod_log_ptr)) {
			return(FALSE);
		}
		page_zip->m_end = uint16_t(mod_log_ptr - page_zip->data);
		page_zip->m_nonempty = mod_log_ptr != d_stream->next_in;
	}

	if (UNIV_UNLIKELY
	    (page_zip_get_trailer_len(page_zip,
				      dict_index_is_clust(index))
	     + page_zip->m_end >= page_zip_get_size(page_zip))) {
		page_zip_fail(("page_zip_decompress_node_ptrs:"
			       " %lu + %lu >= %lu, %lu\n",
			       (ulong) page_zip_get_trailer_len(
				       page_zip, dict_index_is_clust(index)),
			       (ulong) page_zip->m_end,
			       (ulong) page_zip_get_size(page_zip),
			       (ulong) dict_index_is_clust(index)));
		return(FALSE);
	}

	/* Restore the uncompressed columns in heap_no order. */
	storage = page_zip_dir_start_low(page_zip, n_dense);

	for (slot = 0; slot < n_dense; slot++) {
		rec_t*		rec	= recs[slot];

		offsets = rec_get_offsets(rec, index, offsets, 0,
					  ULINT_UNDEFINED, &heap);
		/* Non-leaf nodes should not have any externally
		stored columns. */
		ut_ad(!rec_offs_any_extern(offsets));
		storage -= REC_NODE_PTR_SIZE;

		memcpy(rec_get_end(rec, offsets) - REC_NODE_PTR_SIZE,
		       storage, REC_NODE_PTR_SIZE);
	}

	return(TRUE);
}

/**********************************************************************//**
Decompress the records of a leaf node of a secondary index.
@return TRUE on success, FALSE on failure */
static
ibool
page_zip_decompress_sec(
/*====================*/
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page */
	z_stream*	d_stream,	/*!< in/out: compressed page stream */
	rec_t**		recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense,	/*!< in: size of recs[] */
	dict_index_t*	index,		/*!< in: the index of the page */
	rec_offs*	offsets)	/*!< in/out: temporary offsets */
{
	ulint	heap_status	= REC_STATUS_ORDINARY
		| PAGE_HEAP_NO_USER_LOW << REC_HEAP_NO_SHIFT;
	ulint	slot;

	ut_a(!dict_index_is_clust(index));

	/* Subtract the space reserved for uncompressed data. */
	d_stream->avail_in -= static_cast<uint>(
		n_dense * PAGE_ZIP_DIR_SLOT_SIZE);

	for (slot = 0; slot < n_dense; slot++) {
		rec_t*	rec = recs[slot];

		/* Decompress everything up to this record. */
		d_stream->avail_out = static_cast<uint>(
			rec - REC_N_NEW_EXTRA_BYTES - d_stream->next_out);

		if (UNIV_LIKELY(d_stream->avail_out)) {
			switch (inflate(d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
				page_zip_decompress_heap_no(
					d_stream, rec, heap_status);
				goto zlib_done;
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream->avail_out) {
					break;
				}
				/* fall through */
			default:
				page_zip_fail(("page_zip_decompress_sec:"
					       " inflate(Z_SYNC_FLUSH)=%s\n",
					       d_stream->msg));
				goto zlib_error;
			}
		}

		if (!page_zip_decompress_heap_no(
			    d_stream, rec, heap_status)) {
			ut_ad(0);
		}
	}

	/* Decompress the data of the last record and any trailing garbage,
	in case the last record was allocated from an originally longer space
	on the free list. */
	d_stream->avail_out = static_cast<uInt>(
		page_header_get_field(page_zip->data, PAGE_HEAP_TOP)
		- page_offset(d_stream->next_out));
	if (UNIV_UNLIKELY(d_stream->avail_out > srv_page_size
			  - PAGE_ZIP_START - PAGE_DIR)) {

		page_zip_fail(("page_zip_decompress_sec:"
			       " avail_out = %u\n",
			       d_stream->avail_out));
		goto zlib_error;
	}

	if (UNIV_UNLIKELY(inflate(d_stream, Z_FINISH) != Z_STREAM_END)) {
		page_zip_fail(("page_zip_decompress_sec:"
			       " inflate(Z_FINISH)=%s\n",
			       d_stream->msg));
zlib_error:
		inflateEnd(d_stream);
		return(FALSE);
	}

	/* Note that d_stream->avail_out > 0 may hold here
	if the modification log is nonempty. */

zlib_done:
	if (UNIV_UNLIKELY(inflateEnd(d_stream) != Z_OK)) {
		ut_error;
	}

	{
		page_t*	page = page_align(d_stream->next_out);

		/* Clear the unused heap space on the uncompressed page. */
		memset(d_stream->next_out, 0,
		       ulint(page_dir_get_nth_slot(page,
						   page_dir_get_n_slots(page)
						   - 1U)
			     - d_stream->next_out));
	}

	ut_d(page_zip->m_start = uint16_t(PAGE_DATA + d_stream->total_in));

	/* Apply the modification log. */
	{
		const byte*	mod_log_ptr;
		mod_log_ptr = page_zip_apply_log(d_stream->next_in,
						 d_stream->avail_in + 1,
						 recs, n_dense,
						 index->n_fields,
						 ULINT_UNDEFINED, heap_status,
						 index, offsets);

		if (UNIV_UNLIKELY(!mod_log_ptr)) {
			return(FALSE);
		}
		page_zip->m_end = uint16_t(mod_log_ptr - page_zip->data);
		page_zip->m_nonempty = mod_log_ptr != d_stream->next_in;
	}

	if (UNIV_UNLIKELY(page_zip_get_trailer_len(page_zip, FALSE)
			  + page_zip->m_end >= page_zip_get_size(page_zip))) {

		page_zip_fail(("page_zip_decompress_sec: %lu + %lu >= %lu\n",
			       (ulong) page_zip_get_trailer_len(
				       page_zip, FALSE),
			       (ulong) page_zip->m_end,
			       (ulong) page_zip_get_size(page_zip)));
		return(FALSE);
	}

	/* There are no uncompressed columns on leaf pages of
	secondary indexes. */

	return(TRUE);
}

/**********************************************************************//**
Decompress a record of a leaf node of a clustered index that contains
externally stored columns.
@return TRUE on success */
static
ibool
page_zip_decompress_clust_ext(
/*==========================*/
	z_stream*	d_stream,	/*!< in/out: compressed page stream */
	rec_t*		rec,		/*!< in/out: record */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec) */
	ulint		trx_id_col)	/*!< in: position of of DB_TRX_ID */
{
	ulint	i;

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		ulint	len;
		byte*	dst;

		if (UNIV_UNLIKELY(i == trx_id_col)) {
			/* Skip trx_id and roll_ptr */
			dst = rec_get_nth_field(rec, offsets, i, &len);
			if (UNIV_UNLIKELY(len < DATA_TRX_ID_LEN
					  + DATA_ROLL_PTR_LEN)) {

				page_zip_fail(("page_zip_decompress_clust_ext:"
					       " len[%lu] = %lu\n",
					       (ulong) i, (ulong) len));
				return(FALSE);
			}

			if (rec_offs_nth_extern(offsets, i)) {

				page_zip_fail(("page_zip_decompress_clust_ext:"
					       " DB_TRX_ID at %lu is ext\n",
					       (ulong) i));
				return(FALSE);
			}

			d_stream->avail_out = static_cast<uInt>(
				dst - d_stream->next_out);

			switch (inflate(d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream->avail_out) {
					break;
				}
				/* fall through */
			default:
				page_zip_fail(("page_zip_decompress_clust_ext:"
					       " 1 inflate(Z_SYNC_FLUSH)=%s\n",
					       d_stream->msg));
				return(FALSE);
			}

			ut_ad(d_stream->next_out == dst);

			/* Clear DB_TRX_ID and DB_ROLL_PTR in order to
			avoid uninitialized bytes in case the record
			is affected by page_zip_apply_log(). */
			memset(dst, 0, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

			d_stream->next_out += DATA_TRX_ID_LEN
				+ DATA_ROLL_PTR_LEN;
		} else if (rec_offs_nth_extern(offsets, i)) {
			dst = rec_get_nth_field(rec, offsets, i, &len);
			ut_ad(len >= BTR_EXTERN_FIELD_REF_SIZE);
			dst += len - BTR_EXTERN_FIELD_REF_SIZE;

			d_stream->avail_out = static_cast<uInt>(
				dst - d_stream->next_out);
			switch (inflate(d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream->avail_out) {
					break;
				}
				/* fall through */
			default:
				page_zip_fail(("page_zip_decompress_clust_ext:"
					       " 2 inflate(Z_SYNC_FLUSH)=%s\n",
					       d_stream->msg));
				return(FALSE);
			}

			ut_ad(d_stream->next_out == dst);

			/* Clear the BLOB pointer in case
			the record will be deleted and the
			space will not be reused.  Note that
			the final initialization of the BLOB
			pointers (copying from "externs"
			or clearing) will have to take place
			only after the page modification log
			has been applied.  Otherwise, we
			could end up with an uninitialized
			BLOB pointer when a record is deleted,
			reallocated and deleted. */
			memset(d_stream->next_out, 0,
			       BTR_EXTERN_FIELD_REF_SIZE);
			d_stream->next_out
				+= BTR_EXTERN_FIELD_REF_SIZE;
		}
	}

	return(TRUE);
}

/**********************************************************************//**
Compress the records of a leaf node of a clustered index.
@return TRUE on success, FALSE on failure */
static
ibool
page_zip_decompress_clust(
/*======================*/
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page */
	z_stream*	d_stream,	/*!< in/out: compressed page stream */
	rec_t**		recs,		/*!< in: dense page directory
					sorted by address */
	ulint		n_dense,	/*!< in: size of recs[] */
	dict_index_t*	index,		/*!< in: the index of the page */
	ulint		trx_id_col,	/*!< index of the trx_id column */
	rec_offs*	offsets,	/*!< in/out: temporary offsets */
	mem_heap_t*	heap)		/*!< in: temporary memory heap */
{
	int		err;
	ulint		slot;
	ulint		heap_status	= REC_STATUS_ORDINARY
		| PAGE_HEAP_NO_USER_LOW << REC_HEAP_NO_SHIFT;
	const byte*	storage;
	const byte*	externs;

	ut_a(dict_index_is_clust(index));

	/* Subtract the space reserved for uncompressed data. */
	d_stream->avail_in -= static_cast<uInt>(n_dense)
			    * (PAGE_ZIP_CLUST_LEAF_SLOT_SIZE);

	/* Decompress the records in heap_no order. */
	for (slot = 0; slot < n_dense; slot++) {
		rec_t*	rec	= recs[slot];

		d_stream->avail_out =static_cast<uInt>(
			rec - REC_N_NEW_EXTRA_BYTES - d_stream->next_out);

		ut_ad(d_stream->avail_out < srv_page_size
		      - PAGE_ZIP_START - PAGE_DIR);
		err = inflate(d_stream, Z_SYNC_FLUSH);
		switch (err) {
		case Z_STREAM_END:
			page_zip_decompress_heap_no(
				d_stream, rec, heap_status);
			goto zlib_done;
		case Z_OK:
		case Z_BUF_ERROR:
			if (UNIV_LIKELY(!d_stream->avail_out)) {
				break;
			}
			/* fall through */
		default:
			page_zip_fail(("page_zip_decompress_clust:"
				       " 1 inflate(Z_SYNC_FLUSH)=%s\n",
				       d_stream->msg));
			goto zlib_error;
		}

		if (!page_zip_decompress_heap_no(
			    d_stream, rec, heap_status)) {
			ut_ad(0);
		}

		/* Read the offsets. The status bits are needed here. */
		offsets = rec_get_offsets(rec, index, offsets, index->n_fields,
					  ULINT_UNDEFINED, &heap);

		/* This is a leaf page in a clustered index. */

		/* Check if there are any externally stored columns.
		For each externally stored column, restore the
		BTR_EXTERN_FIELD_REF separately. */

		if (rec_offs_any_extern(offsets)) {
			if (UNIV_UNLIKELY
			    (!page_zip_decompress_clust_ext(
				    d_stream, rec, offsets, trx_id_col))) {

				goto zlib_error;
			}
		} else {
			/* Skip trx_id and roll_ptr */
			ulint	len;
			byte*	dst = rec_get_nth_field(rec, offsets,
							trx_id_col, &len);
			if (UNIV_UNLIKELY(len < DATA_TRX_ID_LEN
					  + DATA_ROLL_PTR_LEN)) {

				page_zip_fail(("page_zip_decompress_clust:"
					       " len = %lu\n", (ulong) len));
				goto zlib_error;
			}

			d_stream->avail_out = static_cast<uInt>(
				dst - d_stream->next_out);

			switch (inflate(d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream->avail_out) {
					break;
				}
				/* fall through */
			default:
				page_zip_fail(("page_zip_decompress_clust:"
					       " 2 inflate(Z_SYNC_FLUSH)=%s\n",
					       d_stream->msg));
				goto zlib_error;
			}

			ut_ad(d_stream->next_out == dst);

			/* Clear DB_TRX_ID and DB_ROLL_PTR in order to
			avoid uninitialized bytes in case the record
			is affected by page_zip_apply_log(). */
			memset(dst, 0, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

			d_stream->next_out += DATA_TRX_ID_LEN
				+ DATA_ROLL_PTR_LEN;
		}

		/* Decompress the last bytes of the record. */
		d_stream->avail_out = static_cast<uInt>(
			rec_get_end(rec, offsets) - d_stream->next_out);

		switch (inflate(d_stream, Z_SYNC_FLUSH)) {
		case Z_STREAM_END:
		case Z_OK:
		case Z_BUF_ERROR:
			if (!d_stream->avail_out) {
				break;
			}
			/* fall through */
		default:
			page_zip_fail(("page_zip_decompress_clust:"
				       " 3 inflate(Z_SYNC_FLUSH)=%s\n",
				       d_stream->msg));
			goto zlib_error;
		}
	}

	/* Decompress any trailing garbage, in case the last record was
	allocated from an originally longer space on the free list. */
	d_stream->avail_out = static_cast<uInt>(
		page_header_get_field(page_zip->data, PAGE_HEAP_TOP)
		- page_offset(d_stream->next_out));
	if (UNIV_UNLIKELY(d_stream->avail_out > srv_page_size
			  - PAGE_ZIP_START - PAGE_DIR)) {

		page_zip_fail(("page_zip_decompress_clust:"
			       " avail_out = %u\n",
			       d_stream->avail_out));
		goto zlib_error;
	}

	if (UNIV_UNLIKELY(inflate(d_stream, Z_FINISH) != Z_STREAM_END)) {
		page_zip_fail(("page_zip_decompress_clust:"
			       " inflate(Z_FINISH)=%s\n",
			       d_stream->msg));
zlib_error:
		inflateEnd(d_stream);
		return(FALSE);
	}

	/* Note that d_stream->avail_out > 0 may hold here
	if the modification log is nonempty. */

zlib_done:
	if (UNIV_UNLIKELY(inflateEnd(d_stream) != Z_OK)) {
		ut_error;
	}

	{
		page_t*	page = page_align(d_stream->next_out);

		/* Clear the unused heap space on the uncompressed page. */
		memset(d_stream->next_out, 0,
		       ulint(page_dir_get_nth_slot(page,
						   page_dir_get_n_slots(page)
						   - 1U)
			     - d_stream->next_out));
	}

	ut_d(page_zip->m_start = uint16_t(PAGE_DATA + d_stream->total_in));

	/* Apply the modification log. */
	{
		const byte*	mod_log_ptr;
		mod_log_ptr = page_zip_apply_log(d_stream->next_in,
						 d_stream->avail_in + 1,
						 recs, n_dense,
						 index->n_fields,
						 trx_id_col, heap_status,
						 index, offsets);

		if (UNIV_UNLIKELY(!mod_log_ptr)) {
			return(FALSE);
		}
		page_zip->m_end = uint16_t(mod_log_ptr - page_zip->data);
		page_zip->m_nonempty = mod_log_ptr != d_stream->next_in;
	}

	if (UNIV_UNLIKELY(page_zip_get_trailer_len(page_zip, TRUE)
			  + page_zip->m_end >= page_zip_get_size(page_zip))) {

		page_zip_fail(("page_zip_decompress_clust: %lu + %lu >= %lu\n",
			       (ulong) page_zip_get_trailer_len(
				       page_zip, TRUE),
			       (ulong) page_zip->m_end,
			       (ulong) page_zip_get_size(page_zip)));
		return(FALSE);
	}

	storage = page_zip_dir_start_low(page_zip, n_dense);

	externs = storage - n_dense
		* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

	/* Restore the uncompressed columns in heap_no order. */

	for (slot = 0; slot < n_dense; slot++) {
		ulint	i;
		ulint	len;
		byte*	dst;
		rec_t*	rec	= recs[slot];
		bool	exists	= !page_zip_dir_find_free(
			page_zip, page_offset(rec));
		offsets = rec_get_offsets(rec, index, offsets, index->n_fields,
					  ULINT_UNDEFINED, &heap);

		dst = rec_get_nth_field(rec, offsets,
					trx_id_col, &len);
		ut_ad(len >= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
		storage -= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
		memcpy(dst, storage,
		       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

		/* Check if there are any externally stored
		columns in this record.  For each externally
		stored column, restore or clear the
		BTR_EXTERN_FIELD_REF. */
		if (!rec_offs_any_extern(offsets)) {
			continue;
		}

		for (i = 0; i < rec_offs_n_fields(offsets); i++) {
			if (!rec_offs_nth_extern(offsets, i)) {
				continue;
			}
			dst = rec_get_nth_field(rec, offsets, i, &len);

			if (UNIV_UNLIKELY(len < BTR_EXTERN_FIELD_REF_SIZE)) {
				page_zip_fail(("page_zip_decompress_clust:"
					       " %lu < 20\n",
					       (ulong) len));
				return(FALSE);
			}

			dst += len - BTR_EXTERN_FIELD_REF_SIZE;

			if (UNIV_LIKELY(exists)) {
				/* Existing record:
				restore the BLOB pointer */
				externs -= BTR_EXTERN_FIELD_REF_SIZE;

				if (UNIV_UNLIKELY
				    (externs < page_zip->data
				     + page_zip->m_end)) {
					page_zip_fail(("page_zip_"
						       "decompress_clust:"
						       " %p < %p + %lu\n",
						       (const void*) externs,
						       (const void*)
						       page_zip->data,
						       (ulong)
						       page_zip->m_end));
					return(FALSE);
				}

				memcpy(dst, externs,
				       BTR_EXTERN_FIELD_REF_SIZE);

				page_zip->n_blobs++;
			} else {
				/* Deleted record:
				clear the BLOB pointer */
				memset(dst, 0,
				       BTR_EXTERN_FIELD_REF_SIZE);
			}
		}
	}

	return(TRUE);
}

/**********************************************************************//**
Decompress a page.  This function should tolerate errors on the compressed
page.  Instead of letting assertions fail, it will return FALSE if an
inconsistency is detected.
@return TRUE on success, FALSE on failure */
static
ibool
page_zip_decompress_low(
/*====================*/
	page_zip_des_t*	page_zip,/*!< in: data, ssize;
				out: m_start, m_end, m_nonempty, n_blobs */
	page_t*		page,	/*!< out: uncompressed page, may be trashed */
	ibool		all)	/*!< in: TRUE=decompress the whole page;
				FALSE=verify but do not copy some
				page header fields that should not change
				after page creation */
{
	z_stream	d_stream;
	dict_index_t*	index	= NULL;
	rec_t**		recs;	/*!< dense page directory, sorted by address */
	ulint		n_dense;/* number of user records on the page */
	ulint		trx_id_col = ULINT_UNDEFINED;
	mem_heap_t*	heap;
	rec_offs*	offsets;

	ut_ad(page_zip_simple_validate(page_zip));
	MEM_CHECK_ADDRESSABLE(page, srv_page_size);
	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

	/* The dense directory excludes the infimum and supremum records. */
	n_dense = page_dir_get_n_heap(page_zip->data) - PAGE_HEAP_NO_USER_LOW;
	if (UNIV_UNLIKELY(n_dense * PAGE_ZIP_DIR_SLOT_SIZE
			  >= page_zip_get_size(page_zip))) {
		page_zip_fail(("page_zip_decompress 1: %lu %lu\n",
			       (ulong) n_dense,
			       (ulong) page_zip_get_size(page_zip)));
		return(FALSE);
	}

	heap = mem_heap_create(n_dense * (3 * sizeof *recs) + srv_page_size);

	recs = static_cast<rec_t**>(
		mem_heap_alloc(heap, n_dense * sizeof *recs));

	if (all) {
		/* Copy the page header. */
		memcpy_aligned<2>(page, page_zip->data, PAGE_DATA);
	} else {
		/* Check that the bytes that we skip are identical. */
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
		ut_a(!memcmp(FIL_PAGE_TYPE + page,
			     FIL_PAGE_TYPE + page_zip->data,
			     PAGE_HEADER - FIL_PAGE_TYPE));
		ut_a(!memcmp(PAGE_HEADER + PAGE_LEVEL + page,
			     PAGE_HEADER + PAGE_LEVEL + page_zip->data,
			     PAGE_DATA - (PAGE_HEADER + PAGE_LEVEL)));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

		/* Copy the mutable parts of the page header. */
		memcpy_aligned<8>(page, page_zip->data, FIL_PAGE_TYPE);
		memcpy_aligned<2>(PAGE_HEADER + page,
				  PAGE_HEADER + page_zip->data,
				  PAGE_LEVEL - PAGE_N_DIR_SLOTS);

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
		/* Check that the page headers match after copying. */
		ut_a(!memcmp(page, page_zip->data, PAGE_DATA));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
	}

#ifdef UNIV_ZIP_DEBUG
	/* Clear the uncompressed page, except the header. */
	memset(PAGE_DATA + page, 0x55, srv_page_size - PAGE_DATA);
#endif /* UNIV_ZIP_DEBUG */
	MEM_UNDEFINED(PAGE_DATA + page, srv_page_size - PAGE_DATA);

	/* Copy the page directory. */
	if (UNIV_UNLIKELY(!page_zip_dir_decode(page_zip, page, recs,
					       n_dense))) {
zlib_error:
		mem_heap_free(heap);
		return(FALSE);
	}

	/* Copy the infimum and supremum records. */
	memcpy(page + (PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES),
	       infimum_extra, sizeof infimum_extra);
	if (page_is_empty(page)) {
		rec_set_next_offs_new(page + PAGE_NEW_INFIMUM,
				      PAGE_NEW_SUPREMUM);
	} else {
		rec_set_next_offs_new(page + PAGE_NEW_INFIMUM,
				      page_zip_dir_get(page_zip, 0)
				      & PAGE_ZIP_DIR_SLOT_MASK);
	}
	memcpy(page + PAGE_NEW_INFIMUM, infimum_data, sizeof infimum_data);
	memcpy_aligned<4>(PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES + 1
			  + page, supremum_extra_data,
			  sizeof supremum_extra_data);

	page_zip_set_alloc(&d_stream, heap);

	d_stream.next_in = page_zip->data + PAGE_DATA;
	/* Subtract the space reserved for
	the page header and the end marker of the modification log. */
	d_stream.avail_in = static_cast<uInt>(
		page_zip_get_size(page_zip) - (PAGE_DATA + 1));
	d_stream.next_out = page + PAGE_ZIP_START;
	d_stream.avail_out = uInt(srv_page_size - PAGE_ZIP_START);

	if (UNIV_UNLIKELY(inflateInit2(&d_stream, int(srv_page_size_shift))
			  != Z_OK)) {
		ut_error;
	}

	/* Decode the zlib header and the index information. */
	if (UNIV_UNLIKELY(inflate(&d_stream, Z_BLOCK) != Z_OK)) {

		page_zip_fail(("page_zip_decompress:"
			       " 1 inflate(Z_BLOCK)=%s\n", d_stream.msg));
		goto zlib_error;
	}

	if (UNIV_UNLIKELY(inflate(&d_stream, Z_BLOCK) != Z_OK)) {

		page_zip_fail(("page_zip_decompress:"
			       " 2 inflate(Z_BLOCK)=%s\n", d_stream.msg));
		goto zlib_error;
	}

	index = page_zip_fields_decode(
		page + PAGE_ZIP_START, d_stream.next_out,
		page_is_leaf(page) ? &trx_id_col : NULL,
		fil_page_get_type(page) == FIL_PAGE_RTREE);

	if (UNIV_UNLIKELY(!index)) {

		goto zlib_error;
	}

	/* Decompress the user records. */
	page_zip->n_blobs = 0;
	d_stream.next_out = page + PAGE_ZIP_START;

	{
		/* Pre-allocate the offsets for rec_get_offsets_reverse(). */
		ulint	n = 1 + 1/* node ptr */ + REC_OFFS_HEADER_SIZE
			+ dict_index_get_n_fields(index);

		offsets = static_cast<rec_offs*>(
			mem_heap_alloc(heap, n * sizeof(ulint)));

		rec_offs_set_n_alloc(offsets, n);
	}

	/* Decompress the records in heap_no order. */
	if (!page_is_leaf(page)) {
		/* This is a node pointer page. */
		ulint	info_bits;

		if (UNIV_UNLIKELY
		    (!page_zip_decompress_node_ptrs(page_zip, &d_stream,
						    recs, n_dense, index,
						    offsets, heap))) {
			goto err_exit;
		}

		info_bits = page_has_prev(page) ? 0 : REC_INFO_MIN_REC_FLAG;

		if (UNIV_UNLIKELY(!page_zip_set_extra_bytes(page_zip, page,
							    info_bits))) {
			goto err_exit;
		}
	} else if (UNIV_LIKELY(trx_id_col == ULINT_UNDEFINED)) {
		/* This is a leaf page in a secondary index. */
		if (UNIV_UNLIKELY(!page_zip_decompress_sec(page_zip, &d_stream,
							   recs, n_dense,
							   index, offsets))) {
			goto err_exit;
		}

		if (UNIV_UNLIKELY(!page_zip_set_extra_bytes(page_zip,
							    page, 0))) {
err_exit:
			page_zip_fields_free(index);
			mem_heap_free(heap);
			return(FALSE);
		}
	} else {
		/* This is a leaf page in a clustered index. */
		if (UNIV_UNLIKELY(!page_zip_decompress_clust(page_zip,
							     &d_stream, recs,
							     n_dense, index,
							     trx_id_col,
							     offsets, heap))) {
			goto err_exit;
		}

		if (UNIV_UNLIKELY(!page_zip_set_extra_bytes(page_zip,
							    page, 0))) {
			goto err_exit;
		}
	}

	ut_a(page_is_comp(page));
	MEM_CHECK_DEFINED(page, srv_page_size);

	page_zip_fields_free(index);
	mem_heap_free(heap);

	return(TRUE);
}

/**********************************************************************//**
Decompress a page.  This function should tolerate errors on the compressed
page.  Instead of letting assertions fail, it will return FALSE if an
inconsistency is detected.
@return TRUE on success, FALSE on failure */
ibool
page_zip_decompress(
/*================*/
	page_zip_des_t*	page_zip,/*!< in: data, ssize;
				out: m_start, m_end, m_nonempty, n_blobs */
	page_t*		page,	/*!< out: uncompressed page, may be trashed */
	ibool		all)	/*!< in: TRUE=decompress the whole page;
				FALSE=verify but do not copy some
				page header fields that should not change
				after page creation */
{
	const ulonglong ns = my_interval_timer();

	if (!page_zip_decompress_low(page_zip, page, all)) {
		return(FALSE);
	}

	const uint64_t time_diff = (my_interval_timer() - ns) / 1000;
	page_zip_stat[page_zip->ssize - 1].decompressed++;
	page_zip_stat[page_zip->ssize - 1].decompressed_usec += time_diff;

	index_id_t	index_id = btr_page_get_index_id(page);

	if (srv_cmp_per_index_enabled) {
		mysql_mutex_lock(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index[index_id].decompressed++;
		page_zip_stat_per_index[index_id].decompressed_usec += time_diff;
		mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
	}

	/* Update the stat counter for LRU policy. */
	buf_LRU_stat_inc_unzip();

	MONITOR_INC(MONITOR_PAGE_DECOMPRESS);

	return(TRUE);
}

#ifdef UNIV_ZIP_DEBUG
/**********************************************************************//**
Dump a block of memory on the standard error stream. */
static
void
page_zip_hexdump_func(
/*==================*/
	const char*	name,	/*!< in: name of the data structure */
	const void*	buf,	/*!< in: data */
	ulint		size)	/*!< in: length of the data, in bytes */
{
	const byte*	s	= static_cast<const byte*>(buf);
	ulint		addr;
	const ulint	width	= 32; /* bytes per line */

	fprintf(stderr, "%s:\n", name);

	for (addr = 0; addr < size; addr += width) {
		ulint	i;

		fprintf(stderr, "%04lx ", (ulong) addr);

		i = ut_min(width, size - addr);

		while (i--) {
			fprintf(stderr, "%02x", *s++);
		}

		putc('\n', stderr);
	}
}

/** Dump a block of memory on the standard error stream.
@param buf in: data
@param size in: length of the data, in bytes */
#define page_zip_hexdump(buf, size) page_zip_hexdump_func(#buf, buf, size)

/** Flag: make page_zip_validate() compare page headers only */
bool	page_zip_validate_header_only;

/**********************************************************************//**
Check that the compressed and decompressed pages match.
@return TRUE if valid, FALSE if not */
ibool
page_zip_validate_low(
/*==================*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	const page_t*		page,	/*!< in: uncompressed page */
	const dict_index_t*	index,	/*!< in: index of the page, if known */
	ibool			sloppy)	/*!< in: FALSE=strict,
					TRUE=ignore the MIN_REC_FLAG */
{
	page_zip_des_t	temp_page_zip;
	ibool		valid;

	if (memcmp(page_zip->data + FIL_PAGE_PREV, page + FIL_PAGE_PREV,
		   FIL_PAGE_LSN - FIL_PAGE_PREV)
	    || memcmp(page_zip->data + FIL_PAGE_TYPE, page + FIL_PAGE_TYPE, 2)
	    || memcmp(page_zip->data + FIL_PAGE_DATA, page + FIL_PAGE_DATA,
		      PAGE_ROOT_AUTO_INC)
	    /* The PAGE_ROOT_AUTO_INC can be updated while holding an SX-latch
	    on the clustered index root page (page number 3 in .ibd files).
	    That allows concurrent readers (holding buf_block_t::lock S-latch).
	    Because we do not know what type of a latch our caller is holding,
	    we will ignore the field on clustered index root pages in order
	    to avoid false positives. */
	    || (page_get_page_no(page) != 3/* clustered index root page */
		&& memcmp(&page_zip->data[FIL_PAGE_DATA + PAGE_ROOT_AUTO_INC],
			  &page[FIL_PAGE_DATA + PAGE_ROOT_AUTO_INC], 8))
	    || memcmp(&page_zip->data[FIL_PAGE_DATA + PAGE_HEADER_PRIV_END],
		      &page[FIL_PAGE_DATA + PAGE_HEADER_PRIV_END],
		      PAGE_DATA - FIL_PAGE_DATA - PAGE_HEADER_PRIV_END)) {
		page_zip_fail(("page_zip_validate: page header\n"));
		page_zip_hexdump(page_zip, sizeof *page_zip);
		page_zip_hexdump(page_zip->data, page_zip_get_size(page_zip));
		page_zip_hexdump(page, srv_page_size);
		return(FALSE);
	}

	ut_a(page_is_comp(page));

	if (page_zip_validate_header_only) {
		return(TRUE);
	}

	/* page_zip_decompress() expects the uncompressed page to be
	srv_page_size aligned. */
	page_t* temp_page = static_cast<byte*>(aligned_malloc(srv_page_size,
							      srv_page_size));

	MEM_CHECK_DEFINED(page, srv_page_size);
	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

	temp_page_zip = *page_zip;
	valid = page_zip_decompress_low(&temp_page_zip, temp_page, TRUE);
	if (!valid) {
		fputs("page_zip_validate(): failed to decompress\n", stderr);
		goto func_exit;
	}
	if (page_zip->n_blobs != temp_page_zip.n_blobs) {
		page_zip_fail(("page_zip_validate: n_blobs: %u!=%u\n",
			       page_zip->n_blobs, temp_page_zip.n_blobs));
		valid = FALSE;
	}
#ifdef UNIV_DEBUG
	if (page_zip->m_start != temp_page_zip.m_start) {
		page_zip_fail(("page_zip_validate: m_start: %u!=%u\n",
			       page_zip->m_start, temp_page_zip.m_start));
		valid = FALSE;
	}
#endif /* UNIV_DEBUG */
	if (page_zip->m_end != temp_page_zip.m_end) {
		page_zip_fail(("page_zip_validate: m_end: %u!=%u\n",
			       page_zip->m_end, temp_page_zip.m_end));
		valid = FALSE;
	}
	if (page_zip->m_nonempty != temp_page_zip.m_nonempty) {
		page_zip_fail(("page_zip_validate(): m_nonempty: %u!=%u\n",
			       page_zip->m_nonempty,
			       temp_page_zip.m_nonempty));
		valid = FALSE;
	}
	if (memcmp(page + PAGE_HEADER, temp_page + PAGE_HEADER,
		   srv_page_size - PAGE_HEADER - FIL_PAGE_DATA_END)) {

		/* In crash recovery, the "minimum record" flag may be
		set incorrectly until the mini-transaction is
		committed.  Let us tolerate that difference when we
		are performing a sloppy validation. */

		rec_offs*	offsets;
		mem_heap_t*	heap;
		const rec_t*	rec;
		const rec_t*	trec;
		byte		info_bits_diff;
		ulint		offset
			= rec_get_next_offs(page + PAGE_NEW_INFIMUM, TRUE);
		ut_a(offset >= PAGE_NEW_SUPREMUM);
		offset -= 5/*REC_NEW_INFO_BITS*/;

		info_bits_diff = page[offset] ^ temp_page[offset];

		if (info_bits_diff == REC_INFO_MIN_REC_FLAG) {
			temp_page[offset] = page[offset];

			if (!memcmp(page + PAGE_HEADER,
				    temp_page + PAGE_HEADER,
				    srv_page_size - PAGE_HEADER
				    - FIL_PAGE_DATA_END)) {

				/* Only the minimum record flag
				differed.  Let us ignore it. */
				page_zip_fail(("page_zip_validate:"
					       " min_rec_flag"
					       " (%s" ULINTPF "," ULINTPF
					       ",0x%02x)\n",
					       sloppy ? "ignored, " : "",
					       page_get_space_id(page),
					       page_get_page_no(page),
					       page[offset]));
				/* We don't check for spatial index, since
				the "minimum record" could be deleted when
				doing rtr_update_mbr_field.
				GIS_FIXME: need to validate why
				rtr_update_mbr_field.() could affect this */
				if (index && dict_index_is_spatial(index)) {
					valid = true;
				} else {
					valid = sloppy;
				}
				goto func_exit;
			}
		}

		/* Compare the pointers in the PAGE_FREE list. */
		rec = page_header_get_ptr(page, PAGE_FREE);
		trec = page_header_get_ptr(temp_page, PAGE_FREE);

		while (rec || trec) {
			if (page_offset(rec) != page_offset(trec)) {
				page_zip_fail(("page_zip_validate:"
					       " PAGE_FREE list: %u!=%u\n",
					       (unsigned) page_offset(rec),
					       (unsigned) page_offset(trec)));
				valid = FALSE;
				goto func_exit;
			}

			rec = page_rec_get_next_low(rec, TRUE);
			trec = page_rec_get_next_low(trec, TRUE);
		}

		/* Compare the records. */
		heap = NULL;
		offsets = NULL;
		rec = page_rec_get_next_low(
			page + PAGE_NEW_INFIMUM, TRUE);
		trec = page_rec_get_next_low(
			temp_page + PAGE_NEW_INFIMUM, TRUE);
		const ulint n_core = page_is_leaf(page) ? index->n_fields : 0;

		do {
			if (page_offset(rec) != page_offset(trec)) {
				page_zip_fail(("page_zip_validate:"
					       " record list: 0x%02x!=0x%02x\n",
					       (unsigned) page_offset(rec),
					       (unsigned) page_offset(trec)));
				valid = FALSE;
				break;
			}

			if (index) {
				/* Compare the data. */
				offsets = rec_get_offsets(
					rec, index, offsets, n_core,
					ULINT_UNDEFINED, &heap);

				if (memcmp(rec - rec_offs_extra_size(offsets),
					   trec - rec_offs_extra_size(offsets),
					   rec_offs_size(offsets))) {
					page_zip_fail(
						("page_zip_validate:"
						 " record content: 0x%02x",
						 (unsigned) page_offset(rec)));
					valid = FALSE;
					break;
				}
			}

			rec = page_rec_get_next_low(rec, TRUE);
			trec = page_rec_get_next_low(trec, TRUE);
		} while (rec || trec);

		if (heap) {
			mem_heap_free(heap);
		}
	}

func_exit:
	if (!valid) {
		page_zip_hexdump(page_zip, sizeof *page_zip);
		page_zip_hexdump(page_zip->data, page_zip_get_size(page_zip));
		page_zip_hexdump(page, srv_page_size);
		page_zip_hexdump(temp_page, srv_page_size);
	}
	aligned_free(temp_page);
	return(valid);
}

/**********************************************************************//**
Check that the compressed and decompressed pages match.
@return TRUE if valid, FALSE if not */
ibool
page_zip_validate(
/*==============*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	const page_t*		page,	/*!< in: uncompressed page */
	const dict_index_t*	index)	/*!< in: index of the page, if known */
{
	return(page_zip_validate_low(page_zip, page, index,
				     recv_recovery_is_on()));
}
#endif /* UNIV_ZIP_DEBUG */

#ifdef UNIV_DEBUG
/**********************************************************************//**
Assert that the compressed and decompressed page headers match.
@return TRUE */
static
ibool
page_zip_header_cmp(
/*================*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	const byte*		page)	/*!< in: uncompressed page */
{
	ut_ad(!memcmp(page_zip->data + FIL_PAGE_PREV, page + FIL_PAGE_PREV,
		      FIL_PAGE_LSN - FIL_PAGE_PREV));
	ut_ad(!memcmp(page_zip->data + FIL_PAGE_TYPE, page + FIL_PAGE_TYPE,
		      2));
	ut_ad(!memcmp(page_zip->data + FIL_PAGE_DATA, page + FIL_PAGE_DATA,
		      PAGE_DATA - FIL_PAGE_DATA));

	return(TRUE);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Write a record on the compressed page that contains externally stored
columns.  The data must already have been written to the uncompressed page.
@return end of modification log */
static
byte*
page_zip_write_rec_ext(
/*===================*/
	buf_block_t*	block,		/*!< in/out: compressed page */
	const byte*	rec,		/*!< in: record being written */
	const dict_index_t*index,	/*!< in: record descriptor */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	ulint		create,		/*!< in: nonzero=insert, zero=update */
	ulint		trx_id_col,	/*!< in: position of DB_TRX_ID */
	ulint		heap_no,	/*!< in: heap number of rec */
	byte*		storage,	/*!< in: end of dense page directory */
	byte*		data,		/*!< in: end of modification log */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	const byte*	start	= rec;
	ulint		i;
	ulint		len;
	byte*		externs	= storage;
	ulint		n_ext	= rec_offs_n_extern(offsets);
	const page_t* const page = block->page.frame;
	page_zip_des_t* const page_zip = &block->page.zip;

	ut_ad(rec_offs_validate(rec, index, offsets));
	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));

	externs -= (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
		* (page_dir_get_n_heap(page) - PAGE_HEAP_NO_USER_LOW);

	/* Note that this will not take into account
	the BLOB columns of rec if create==TRUE. */
	ut_ad(data + rec_offs_data_size(offsets)
	      - (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
	      - n_ext * FIELD_REF_SIZE
	      < externs - FIELD_REF_SIZE * page_zip->n_blobs);

	if (n_ext) {
		ulint	blob_no = page_zip_get_n_prev_extern(
			page_zip, rec, index);
		byte*	ext_end = externs - page_zip->n_blobs * FIELD_REF_SIZE;
		ut_ad(blob_no <= page_zip->n_blobs);
		externs -= blob_no * FIELD_REF_SIZE;

		if (create) {
			page_zip->n_blobs = (page_zip->n_blobs + n_ext)
				& ((1U << 12) - 1);
			ASSERT_ZERO_BLOB(ext_end - n_ext * FIELD_REF_SIZE);
			if (ulint len = ulint(externs - ext_end)) {
				byte* ext_start = ext_end
					- n_ext * FIELD_REF_SIZE;
				memmove(ext_start, ext_end, len);
				mtr->memmove(*block,
					     ext_start - page_zip->data,
					     ext_end - page_zip->data, len);
			}
		}

		ut_a(blob_no + n_ext <= page_zip->n_blobs);
	}

	for (i = 0; i < rec_offs_n_fields(offsets); i++) {
		const byte*	src;

		if (UNIV_UNLIKELY(i == trx_id_col)) {
			ut_ad(!rec_offs_nth_extern(offsets,
						   i));
			ut_ad(!rec_offs_nth_extern(offsets,
						   i + 1));
			/* Locate trx_id and roll_ptr. */
			src = rec_get_nth_field(rec, offsets,
						i, &len);
			ut_ad(len == DATA_TRX_ID_LEN);
			ut_ad(src + DATA_TRX_ID_LEN
			      == rec_get_nth_field(
				      rec, offsets,
				      i + 1, &len));
			ut_ad(len == DATA_ROLL_PTR_LEN);

			/* Log the preceding fields. */
			ASSERT_ZERO(data, src - start);
			memcpy(data, start, ulint(src - start));
			data += src - start;
			start = src + (DATA_TRX_ID_LEN
				       + DATA_ROLL_PTR_LEN);

			/* Store trx_id and roll_ptr. */
			constexpr ulint sys_len = DATA_TRX_ID_LEN
				+ DATA_ROLL_PTR_LEN;
			byte* sys = storage - sys_len * (heap_no - 1);
			memcpy(sys, src, sys_len);
			i++; /* skip also roll_ptr */
			mtr->zmemcpy(*block, sys - page_zip->data, sys_len);
		} else if (rec_offs_nth_extern(offsets, i)) {
			src = rec_get_nth_field(rec, offsets,
						i, &len);

			ut_ad(dict_index_is_clust(index));
			ut_ad(len >= FIELD_REF_SIZE);
			src += len - FIELD_REF_SIZE;

			ASSERT_ZERO(data, src - start);
			memcpy(data, start, ulint(src - start));
			data += src - start;
			start = src + FIELD_REF_SIZE;

			/* Store the BLOB pointer. */
			externs -= FIELD_REF_SIZE;
			ut_ad(data < externs);
			memcpy(externs, src, FIELD_REF_SIZE);
			mtr->zmemcpy(*block, externs - page_zip->data,
				     FIELD_REF_SIZE);
		}
	}

	/* Log the last bytes of the record. */
	len = rec_offs_data_size(offsets) - ulint(start - rec);

	ASSERT_ZERO(data, len);
	memcpy(data, start, len);
	data += len;

	return(data);
}

/** Write an entire record to the ROW_FORMAT=COMPRESSED page.
The data must already have been written to the uncompressed page.
@param[in,out]	block		ROW_FORMAT=COMPRESSED page
@param[in]	rec		record in the uncompressed page
@param[in]	index		the index that the page belongs to
@param[in]	offsets		rec_get_offsets(rec, index)
@param[in]	create		nonzero=insert, zero=update
@param[in,out]	mtr		mini-transaction */
void page_zip_write_rec(buf_block_t *block, const byte *rec,
                        const dict_index_t *index, const rec_offs *offsets,
                        ulint create, mtr_t *mtr)
{
	const page_t* const page = block->page.frame;
	page_zip_des_t* const page_zip = &block->page.zip;
	byte*		data;
	byte*		storage;
	ulint		heap_no;
	byte*		slot;

	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip_get_size(page_zip)
	      > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_comp(offsets));
	ut_ad(rec_offs_validate(rec, index, offsets));

	ut_ad(page_zip->m_start >= PAGE_DATA);

	ut_ad(page_zip_header_cmp(page_zip, page));
	ut_ad(page_simple_validate_new((page_t*) page));

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));

	slot = page_zip_dir_find(page_zip, page_offset(rec));
	ut_a(slot);
	byte s = *slot;
	/* Copy the delete mark. */
	if (rec_get_deleted_flag(rec, TRUE)) {
		/* In delete-marked records, DB_TRX_ID must
		always refer to an existing undo log record.
		On non-leaf pages, the delete-mark flag is garbage. */
		ut_ad(!index->is_primary() || !page_is_leaf(page)
		      || row_get_rec_trx_id(rec, index, offsets));
		s |= PAGE_ZIP_DIR_SLOT_DEL >> 8;
	} else {
		s &= byte(~(PAGE_ZIP_DIR_SLOT_DEL >> 8));
	}

	if (s != *slot) {
		*slot = s;
		mtr->zmemcpy(*block, slot - page_zip->data, 1);
	}

	ut_ad(rec_get_start((rec_t*) rec, offsets) >= page + PAGE_ZIP_START);
	ut_ad(rec_get_end((rec_t*) rec, offsets) <= page + srv_page_size
	      - PAGE_DIR - PAGE_DIR_SLOT_SIZE
	      * page_dir_get_n_slots(page));

	heap_no = rec_get_heap_no_new(rec);
	ut_ad(heap_no >= PAGE_HEAP_NO_USER_LOW); /* not infimum or supremum */
	ut_ad(heap_no < page_dir_get_n_heap(page));

	/* Append to the modification log. */
	data = page_zip->data + page_zip->m_end;
	ut_ad(!*data);

	/* Identify the record by writing its heap number - 1.
	0 is reserved to indicate the end of the modification log. */

	if (UNIV_UNLIKELY(heap_no - 1 >= 64)) {
		*data++ = (byte) (0x80 | (heap_no - 1) >> 7);
		ut_ad(!*data);
	}
	*data++ = (byte) ((heap_no - 1) << 1);
	ut_ad(!*data);

	{
		const byte*	start	= rec - rec_offs_extra_size(offsets);
		const byte*	b	= rec - REC_N_NEW_EXTRA_BYTES;

		/* Write the extra bytes backwards, so that
		rec_offs_extra_size() can be easily computed in
		page_zip_apply_log() by invoking
		rec_get_offsets_reverse(). */

		while (b != start) {
			*data++ = *--b;
			ut_ad(!*data);
		}
	}

	/* Write the data bytes.  Store the uncompressed bytes separately. */
	storage = page_zip_dir_start(page_zip);

	if (page_is_leaf(page)) {
		if (dict_index_is_clust(index)) {
			/* Store separately trx_id, roll_ptr and
			the BTR_EXTERN_FIELD_REF of each BLOB column. */
			if (rec_offs_any_extern(offsets)) {
				data = page_zip_write_rec_ext(
					block,
					rec, index, offsets, create,
					index->db_trx_id(), heap_no,
					storage, data, mtr);
			} else {
				/* Locate trx_id and roll_ptr. */
				ulint len;
				const byte*	src
					= rec_get_nth_field(rec, offsets,
							    index->db_trx_id(),
							    &len);
				ut_ad(len == DATA_TRX_ID_LEN);
				ut_ad(src + DATA_TRX_ID_LEN
				      == rec_get_nth_field(
					      rec, offsets,
					      index->db_roll_ptr(), &len));
				ut_ad(len == DATA_ROLL_PTR_LEN);

				/* Log the preceding fields. */
				ASSERT_ZERO(data, src - rec);
				memcpy(data, rec, ulint(src - rec));
				data += src - rec;

				/* Store trx_id and roll_ptr. */
				constexpr ulint sys_len
					= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
				byte* sys = storage - sys_len * (heap_no - 1);
				memcpy(sys, src, sys_len);

				src += sys_len;
				mtr->zmemcpy(*block, sys - page_zip->data,
					     sys_len);
				/* Log the last bytes of the record. */
				len = rec_offs_data_size(offsets)
					- ulint(src - rec);

				ASSERT_ZERO(data, len);
				memcpy(data, src, len);
				data += len;
			}
		} else {
			/* Leaf page of a secondary index:
			no externally stored columns */
			ut_ad(!rec_offs_any_extern(offsets));

			/* Log the entire record. */
			ulint len = rec_offs_data_size(offsets);

			ASSERT_ZERO(data, len);
			memcpy(data, rec, len);
			data += len;
		}
	} else {
		/* This is a node pointer page. */
		/* Non-leaf nodes should not have any externally
		stored columns. */
		ut_ad(!rec_offs_any_extern(offsets));

		/* Copy the data bytes, except node_ptr. */
		ulint len = rec_offs_data_size(offsets) - REC_NODE_PTR_SIZE;
		ut_ad(data + len < storage - REC_NODE_PTR_SIZE
		      * (page_dir_get_n_heap(page) - PAGE_HEAP_NO_USER_LOW));
		ASSERT_ZERO(data, len);
		memcpy(data, rec, len);
		data += len;

		/* Copy the node pointer to the uncompressed area. */
		byte* node_ptr = storage - REC_NODE_PTR_SIZE * (heap_no - 1);
		mtr->zmemcpy<mtr_t::MAYBE_NOP>(*block, node_ptr,
					       rec + len, REC_NODE_PTR_SIZE);
	}

	ut_a(!*data);
	ut_ad((ulint) (data - page_zip->data) < page_zip_get_size(page_zip));
	mtr->zmemcpy(*block, page_zip->m_end,
		     data - page_zip->data - page_zip->m_end);
	page_zip->m_end = uint16_t(data - page_zip->data);
	page_zip->m_nonempty = TRUE;

#ifdef UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page_align(rec), index));
#endif /* UNIV_ZIP_DEBUG */
}

/**********************************************************************//**
Write a BLOB pointer of a record on the leaf page of a clustered index.
The information must already have been updated on the uncompressed page. */
void
page_zip_write_blob_ptr(
/*====================*/
	buf_block_t*	block,	/*!< in/out: ROW_FORMAT=COMPRESSED page */
	const byte*	rec,	/*!< in/out: record whose data is being
				written */
	dict_index_t*	index,	/*!< in: index of the page */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	ulint		n,	/*!< in: column index */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	const byte*	field;
	byte*		externs;
	const page_t* const page = block->page.frame;
	page_zip_des_t* const page_zip = &block->page.zip;
	ulint		blob_no;
	ulint		len;

	ut_ad(page_align(rec) == page);
	ut_ad(index != NULL);
	ut_ad(offsets != NULL);
	ut_ad(page_simple_validate_new((page_t*) page));
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip_get_size(page_zip)
	      > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_comp(offsets));
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(rec_offs_any_extern(offsets));
	ut_ad(rec_offs_nth_extern(offsets, n));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(page_zip_header_cmp(page_zip, page));

	ut_ad(page_is_leaf(page));
	ut_ad(dict_index_is_clust(index));

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));

	blob_no = page_zip_get_n_prev_extern(page_zip, rec, index)
		+ rec_get_n_extern_new(rec, index, n);
	ut_a(blob_no < page_zip->n_blobs);

	externs = page_zip->data + page_zip_get_size(page_zip)
		- (page_dir_get_n_heap(page) - PAGE_HEAP_NO_USER_LOW)
		* PAGE_ZIP_CLUST_LEAF_SLOT_SIZE;

	field = rec_get_nth_field(rec, offsets, n, &len);

	externs -= (blob_no + 1) * BTR_EXTERN_FIELD_REF_SIZE;
	field += len - BTR_EXTERN_FIELD_REF_SIZE;

	mtr->zmemcpy<mtr_t::MAYBE_NOP>(*block, externs, field,
				       BTR_EXTERN_FIELD_REF_SIZE);

#ifdef UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
}

/**********************************************************************//**
Write the node pointer of a record on a non-leaf compressed page. */
void
page_zip_write_node_ptr(
/*====================*/
	buf_block_t*	block,	/*!< in/out: compressed page */
	byte*		rec,	/*!< in/out: record */
	ulint		size,	/*!< in: data size of rec */
	ulint		ptr,	/*!< in: node pointer */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	byte*	field;
	byte*	storage;
	page_zip_des_t* const page_zip = &block->page.zip;

	ut_d(const page_t* const page = block->page.frame);
	ut_ad(page_simple_validate_new(page));
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip_get_size(page_zip)
	      > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(page_rec_is_comp(rec));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(page_zip_header_cmp(page_zip, page));

	ut_ad(!page_is_leaf(page));

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
	MEM_CHECK_DEFINED(rec, size);

	storage = page_zip_dir_start(page_zip)
		- (rec_get_heap_no_new(rec) - 1) * REC_NODE_PTR_SIZE;
	field = rec + size - REC_NODE_PTR_SIZE;

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(!memcmp(storage, field, REC_NODE_PTR_SIZE));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
	compile_time_assert(REC_NODE_PTR_SIZE == 4);
	mach_write_to_4(field, ptr);
	mtr->zmemcpy(*block, storage, field, REC_NODE_PTR_SIZE);
}

/** Write the DB_TRX_ID,DB_ROLL_PTR into a clustered index leaf page record.
@param[in,out]	block		ROW_FORMAT=COMPRESSED page
@param[in,out]	rec		record
@param[in]	offsets		rec_get_offsets(rec, index)
@param[in]	trx_id_field	field number of DB_TRX_ID (number of PK fields)
@param[in]	trx_id		DB_TRX_ID value (transaction identifier)
@param[in]	roll_ptr	DB_ROLL_PTR value (undo log pointer)
@param[in,out]	mtr		mini-transaction */
void
page_zip_write_trx_id_and_roll_ptr(
	buf_block_t*	block,
	byte*		rec,
	const rec_offs*	offsets,
	ulint		trx_id_col,
	trx_id_t	trx_id,
	roll_ptr_t	roll_ptr,
	mtr_t*		mtr)
{
	page_zip_des_t* const page_zip = &block->page.zip;

	ut_d(const page_t* const page = block->page.frame);
	ut_ad(page_align(rec) == page);
	ut_ad(page_simple_validate_new(page));
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip_get_size(page_zip)
	      > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(rec_offs_comp(offsets));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(page_zip_header_cmp(page_zip, page));

	ut_ad(page_is_leaf(page));

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

	constexpr ulint sys_len = DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
	const ulint heap_no = rec_get_heap_no_new(rec);
	ut_ad(heap_no >= PAGE_HEAP_NO_USER_LOW);
	byte* storage = page_zip_dir_start(page_zip) - (heap_no - 1) * sys_len;

	compile_time_assert(DATA_TRX_ID + 1 == DATA_ROLL_PTR);
	ulint len;
	byte* field = rec_get_nth_field(rec, offsets, trx_id_col, &len);
	ut_ad(len == DATA_TRX_ID_LEN);
	ut_ad(field + DATA_TRX_ID_LEN
	      == rec_get_nth_field(rec, offsets, trx_id_col + 1, &len));
	ut_ad(len == DATA_ROLL_PTR_LEN);
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(!memcmp(storage, field, sys_len));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
	compile_time_assert(DATA_TRX_ID_LEN == 6);
	mach_write_to_6(field, trx_id);
	compile_time_assert(DATA_ROLL_PTR_LEN == 7);
	mach_write_to_7(field + DATA_TRX_ID_LEN, roll_ptr);
	len = 0;
	if (heap_no > PAGE_HEAP_NO_USER_LOW) {
		byte* prev = storage + sys_len;
		for (; len < sys_len && prev[len] == field[len]; len++);
		if (len > 4) {
			/* We save space by replacing a single record

			WRITE,offset(storage),byte[13]

			with up to two records:

			MEMMOVE,offset(storage),len(1 byte),+13(1 byte),
			WRITE|0x80,0,byte[13-len]

			The single WRITE record would be x+13 bytes long (x>2).
			The MEMMOVE record would be x+1+1 = x+2 bytes, and
			the second WRITE would be 1+1+13-len = 15-len bytes.

			The total size is: x+13 versus x+2+15-len = x+17-len.
			To save space, we must have len>4. */
			memcpy(storage, prev, len);
			mtr->memmove(*block, ulint(storage - page_zip->data),
				     ulint(storage - page_zip->data) + sys_len,
				     len);
			storage += len;
			field += len;
			if (UNIV_LIKELY(len < sys_len)) {
				goto write;
			}
		} else {
			len = 0;
			goto write;
		}
	} else {
write:
                mtr->zmemcpy<mtr_t::MAYBE_NOP>(*block, storage, field,
					       sys_len - len);
	}
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(!memcmp(storage - len, field - len, sys_len));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));
	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
}

/**********************************************************************//**
Clear an area on the uncompressed and compressed page.
Do not clear the data payload, as that would grow the modification log. */
static
void
page_zip_clear_rec(
/*===============*/
	buf_block_t*	block,		/*!< in/out: compressed page */
	byte*		rec,		/*!< in: record to clear */
	const dict_index_t*	index,	/*!< in: index of rec */
	const rec_offs*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	ulint	heap_no;
	byte*	storage;
	byte*	field;
	ulint	len;

	ut_ad(page_align(rec) == block->page.frame);
	page_zip_des_t* const page_zip = &block->page.zip;

	/* page_zip_validate() would fail here if a record
	containing externally stored columns is being deleted. */
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_zip_dir_find(page_zip, page_offset(rec)));
	ut_ad(page_zip_dir_find_free(page_zip, page_offset(rec)));
	ut_ad(page_zip_header_cmp(page_zip, block->page.frame));

	heap_no = rec_get_heap_no_new(rec);
	ut_ad(heap_no >= PAGE_HEAP_NO_USER_LOW);

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
	MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
	MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
			  rec_offs_extra_size(offsets));

	if (!page_is_leaf(block->page.frame)) {
		/* Clear node_ptr. On the compressed page,
		there is an array of node_ptr immediately before the
		dense page directory, at the very end of the page. */
		storage	= page_zip_dir_start(page_zip);
		ut_ad(dict_index_get_n_unique_in_tree_nonleaf(index) ==
		      rec_offs_n_fields(offsets) - 1);
		field	= rec_get_nth_field(rec, offsets,
					    rec_offs_n_fields(offsets) - 1,
					    &len);
		ut_ad(len == REC_NODE_PTR_SIZE);
		ut_ad(!rec_offs_any_extern(offsets));
		memset(field, 0, REC_NODE_PTR_SIZE);
		storage -= (heap_no - 1) * REC_NODE_PTR_SIZE;
		len = REC_NODE_PTR_SIZE;
clear_page_zip:
		memset(storage, 0, len);
		mtr->memset(*block, storage - page_zip->data, len, 0);
	} else if (index->is_clust()) {
		/* Clear trx_id and roll_ptr. On the compressed page,
		there is an array of these fields immediately before the
		dense page directory, at the very end of the page. */
		const ulint	trx_id_pos
			= dict_col_get_clust_pos(
			dict_table_get_sys_col(
				index->table, DATA_TRX_ID), index);
		field	= rec_get_nth_field(rec, offsets, trx_id_pos, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
		memset(field, 0, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

		if (rec_offs_any_extern(offsets)) {
			ulint	i;

			for (i = rec_offs_n_fields(offsets); i--; ) {
				/* Clear all BLOB pointers in order to make
				page_zip_validate() pass. */
				if (rec_offs_nth_extern(offsets, i)) {
					field = rec_get_nth_field(
						rec, offsets, i, &len);
					ut_ad(len
					      == BTR_EXTERN_FIELD_REF_SIZE);
					memset(field + len
					       - BTR_EXTERN_FIELD_REF_SIZE,
					       0, BTR_EXTERN_FIELD_REF_SIZE);
				}
			}
		}

		len = DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
		storage = page_zip_dir_start(page_zip)
			- (heap_no - 1)
			* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
		goto clear_page_zip;
	} else {
		ut_ad(!rec_offs_any_extern(offsets));
	}
}

/** Modify the delete-mark flag of a ROW_FORMAT=COMPRESSED record.
@param[in,out]  block   buffer block
@param[in,out]  rec     record on a physical index page
@param[in]      flag    the value of the delete-mark flag
@param[in,out]  mtr     mini-transaction  */
void page_zip_rec_set_deleted(buf_block_t *block, rec_t *rec, bool flag,
                              mtr_t *mtr)
{
  ut_ad(page_align(rec) == block->page.frame);
  byte *slot= page_zip_dir_find(&block->page.zip, page_offset(rec));
  byte b= *slot;
  if (flag)
    b|= (PAGE_ZIP_DIR_SLOT_DEL >> 8);
  else
    b&= byte(~(PAGE_ZIP_DIR_SLOT_DEL >> 8));
  mtr->zmemcpy<mtr_t::MAYBE_NOP>(*block, slot, &b, 1);
#ifdef UNIV_ZIP_DEBUG
  ut_a(page_zip_validate(&block->page.zip, block->page.frame, nullptr));
#endif /* UNIV_ZIP_DEBUG */
}

/**********************************************************************//**
Write the "owned" flag of a record on a compressed page.  The n_owned field
must already have been written on the uncompressed page. */
void
page_zip_rec_set_owned(
/*===================*/
	buf_block_t*	block,	/*!< in/out: ROW_FORMAT=COMPRESSED page */
	const byte*	rec,	/*!< in: record on the uncompressed page */
	ulint		flag,	/*!< in: the owned flag (nonzero=TRUE) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  ut_ad(page_align(rec) == block->page.frame);
  page_zip_des_t *const page_zip= &block->page.zip;
  byte *slot= page_zip_dir_find(page_zip, page_offset(rec));
  MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
  byte b= *slot;
  if (flag)
    b|= (PAGE_ZIP_DIR_SLOT_OWNED >> 8);
  else
    b&= byte(~(PAGE_ZIP_DIR_SLOT_OWNED >> 8));
  mtr->zmemcpy<mtr_t::MAYBE_NOP>(*block, slot, &b, 1);
}

/**********************************************************************//**
Insert a record to the dense page directory. */
void
page_zip_dir_insert(
/*================*/
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	uint16_t	free_rec,/*!< in: record from which rec was
				allocated, or 0 */
	byte*		rec,	/*!< in: record to insert */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(page_align(cursor->rec) == cursor->block->page.frame);
	ut_ad(page_align(rec) == cursor->block->page.frame);
	page_zip_des_t *const page_zip= &cursor->block->page.zip;

	ulint	n_dense;
	byte*	slot_rec;
	byte*	slot_free;

	ut_ad(cursor->rec != rec);
	ut_ad(page_rec_get_next_const(cursor->rec) == rec);
	ut_ad(page_zip_simple_validate(page_zip));

	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

	if (page_rec_is_infimum(cursor->rec)) {
		/* Use the first slot. */
		slot_rec = page_zip->data + page_zip_get_size(page_zip);
	} else {
		byte*	end	= page_zip->data + page_zip_get_size(page_zip);
		byte*	start	= end - page_zip_dir_user_size(page_zip);

		if (UNIV_LIKELY(!free_rec)) {
			/* PAGE_N_RECS was already incremented
			in page_cur_insert_rec_zip(), but the
			dense directory slot at that position
			contains garbage.  Skip it. */
			start += PAGE_ZIP_DIR_SLOT_SIZE;
		}

		slot_rec = page_zip_dir_find_low(start, end,
						 page_offset(cursor->rec));
		ut_a(slot_rec);
	}

	/* Read the old n_dense (n_heap may have been incremented). */
	n_dense = page_dir_get_n_heap(page_zip->data)
		- (PAGE_HEAP_NO_USER_LOW + 1U);

	if (UNIV_UNLIKELY(free_rec)) {
		/* The record was allocated from the free list.
		Shift the dense directory only up to that slot.
		Note that in this case, n_dense is actually
		off by one, because page_cur_insert_rec_zip()
		did not increment n_heap. */
		ut_ad(rec_get_heap_no_new(rec) < n_dense + 1
		      + PAGE_HEAP_NO_USER_LOW);
		ut_ad(page_offset(rec) >= free_rec);
		slot_free = page_zip_dir_find(page_zip, free_rec);
		ut_ad(slot_free);
		slot_free += PAGE_ZIP_DIR_SLOT_SIZE;
	} else {
		/* The record was allocated from the heap.
		Shift the entire dense directory. */
		ut_ad(rec_get_heap_no_new(rec) == n_dense
		      + PAGE_HEAP_NO_USER_LOW);

		/* Shift to the end of the dense page directory. */
		slot_free = page_zip->data + page_zip_get_size(page_zip)
			- PAGE_ZIP_DIR_SLOT_SIZE * n_dense;
	}

	if (const ulint slot_len = ulint(slot_rec - slot_free)) {
		/* Shift the dense directory to allocate place for rec. */
		memmove_aligned<2>(slot_free - PAGE_ZIP_DIR_SLOT_SIZE,
				   slot_free, slot_len);
		mtr->memmove(*cursor->block, (slot_free - page_zip->data)
			     - PAGE_ZIP_DIR_SLOT_SIZE,
			     slot_free - page_zip->data, slot_len);
	}

	/* Write the entry for the inserted record.
	The "owned" flag must be zero. */
	uint16_t offs = page_offset(rec);
	if (rec_get_deleted_flag(rec, true)) {
		offs |= PAGE_ZIP_DIR_SLOT_DEL;
	}

	mach_write_to_2(slot_rec - PAGE_ZIP_DIR_SLOT_SIZE, offs);
	mtr->zmemcpy(*cursor->block, slot_rec - page_zip->data
		     - PAGE_ZIP_DIR_SLOT_SIZE, PAGE_ZIP_DIR_SLOT_SIZE);
}

/** Shift the dense page directory and the array of BLOB pointers
when a record is deleted.
@param[in,out]  block   index page
@param[in,out]  rec     record being deleted
@param[in]      index   the index that the page belongs to
@param[in]      offsets rec_get_offsets(rec, index)
@param[in]      free    previous start of the free list
@param[in,out]  mtr     mini-transaction */
void page_zip_dir_delete(buf_block_t *block, byte *rec,
                         const dict_index_t *index, const rec_offs *offsets,
                         const byte *free, mtr_t *mtr)
{
  ut_ad(page_align(rec) == block->page.frame);
  page_zip_des_t *const page_zip= &block->page.zip;

  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(rec_offs_comp(offsets));

  MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));
  MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
  MEM_CHECK_DEFINED(rec - rec_offs_extra_size(offsets),
		    rec_offs_extra_size(offsets));

  mach_write_to_2(rec - REC_NEXT,
                  free ? static_cast<uint16_t>(free - rec) : 0);
  byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER +
                                        block->page.frame);
  mtr->write<2>(*block, page_free, page_offset(rec));
  byte *garbage= my_assume_aligned<2>(PAGE_GARBAGE + PAGE_HEADER +
                                      block->page.frame);
  mtr->write<2>(*block, garbage, rec_offs_size(offsets) +
                mach_read_from_2(garbage));
  compile_time_assert(PAGE_GARBAGE == PAGE_FREE + 2);
  memcpy_aligned<4>(PAGE_FREE + PAGE_HEADER + page_zip->data, page_free, 4);
  byte *slot_rec= page_zip_dir_find(page_zip, page_offset(rec));
  ut_a(slot_rec);
  uint16_t n_recs= page_get_n_recs(block->page.frame);
  ut_ad(n_recs);
  ut_ad(n_recs > 1 || page_get_page_no(block->page.frame) == index->page);
  /* This could not be done before page_zip_dir_find(). */
  byte *page_n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER +
                                          block->page.frame);
  mtr->write<2>(*block, page_n_recs, n_recs - 1U);
  memcpy_aligned<2>(PAGE_N_RECS + PAGE_HEADER + page_zip->data, page_n_recs,
                    2);

  byte *slot_free;

  if (UNIV_UNLIKELY(!free))
    /* Make the last slot the start of the free list. */
    slot_free= page_zip->data + page_zip_get_size(page_zip) -
      PAGE_ZIP_DIR_SLOT_SIZE * (page_dir_get_n_heap(page_zip->data) -
                                PAGE_HEAP_NO_USER_LOW);
  else
  {
    slot_free= page_zip_dir_find_free(page_zip, page_offset(free));
    ut_a(slot_free < slot_rec);
    /* Grow the free list by one slot by moving the start. */
    slot_free+= PAGE_ZIP_DIR_SLOT_SIZE;
  }

  const ulint slot_len= slot_rec > slot_free ? ulint(slot_rec - slot_free) : 0;
  if (slot_len)
  {
    memmove_aligned<2>(slot_free + PAGE_ZIP_DIR_SLOT_SIZE, slot_free,
                       slot_len);
    mtr->memmove(*block, (slot_free - page_zip->data) + PAGE_ZIP_DIR_SLOT_SIZE,
                 slot_free - page_zip->data, slot_len);
  }

  /* Write the entry for the deleted record.
  The "owned" and "deleted" flags will be cleared. */
  mach_write_to_2(slot_free, page_offset(rec));
  mtr->zmemcpy(*block, slot_free - page_zip->data, 2);

  if (const ulint n_ext= rec_offs_n_extern(offsets))
  {
    ut_ad(index->is_primary());
    ut_ad(page_is_leaf(block->page.frame));

    /* Shift and zero fill the array of BLOB pointers. */
    ulint blob_no = page_zip_get_n_prev_extern(page_zip, rec, index);
    ut_a(blob_no + n_ext <= page_zip->n_blobs);

    byte *externs= page_zip->data + page_zip_get_size(page_zip) -
      (page_dir_get_n_heap(block->page.frame) - PAGE_HEAP_NO_USER_LOW) *
      PAGE_ZIP_CLUST_LEAF_SLOT_SIZE;
    byte *ext_end= externs - page_zip->n_blobs * FIELD_REF_SIZE;

    /* Shift and zero fill the array. */
    if (const ulint ext_len= ulint(page_zip->n_blobs - n_ext - blob_no) *
        BTR_EXTERN_FIELD_REF_SIZE)
    {
      memmove(ext_end + n_ext * FIELD_REF_SIZE, ext_end, ext_len);
      mtr->memmove(*block, (ext_end - page_zip->data) + n_ext * FIELD_REF_SIZE,
                   ext_end - page_zip->data, ext_len);
    }
    memset(ext_end, 0, n_ext * FIELD_REF_SIZE);
    mtr->memset(*block, ext_end - page_zip->data, n_ext * FIELD_REF_SIZE, 0);
    page_zip->n_blobs = (page_zip->n_blobs - n_ext) & ((1U << 12) - 1);
  }

  /* The compression algorithm expects info_bits and n_owned
  to be 0 for deleted records. */
  rec[-REC_N_NEW_EXTRA_BYTES]= 0; /* info_bits and n_owned */

  page_zip_clear_rec(block, rec, index, offsets, mtr);
}

/**********************************************************************//**
Reorganize and compress a page.  This is a low-level operation for
compressed pages, to be used when page_zip_compress() fails.
On success, redo log will be written.
The function btr_page_reorganize() should be preferred whenever possible.
IMPORTANT: if page_zip_reorganize() is invoked on a leaf page of a
non-clustered index, the caller must update the insert buffer free
bits in the same mini-transaction in such a way that the modification
will be redo-logged.
@retval true on success
@retval false on failure; the block will be left intact */
bool
page_zip_reorganize(
	buf_block_t*	block,	/*!< in/out: page with compressed page;
				on the compressed page, in: size;
				out: data, n_blobs,
				m_start, m_end, m_nonempty */
	dict_index_t*	index,	/*!< in: index of the B-tree node */
	ulint		z_level,/*!< in: compression level */
	mtr_t*		mtr,	/*!< in: mini-transaction */
	bool		restore)/*!< whether to restore on failure */
{
	page_t*		page		= buf_block_get_frame(block);
	buf_block_t*	temp_block;
	page_t*		temp_page;

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(block->page.zip.data);
	ut_ad(page_is_comp(page));
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(!index->table->is_temporary());
	/* Note that page_zip_validate(page_zip, page, index) may fail here. */
	MEM_CHECK_DEFINED(page, srv_page_size);
	MEM_CHECK_DEFINED(buf_block_get_page_zip(block)->data,
			  page_zip_get_size(buf_block_get_page_zip(block)));

	/* Disable logging */
	mtr_log_t	log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);

	temp_block = buf_block_alloc();
	btr_search_drop_page_hash_index(block);
	temp_page = temp_block->page.frame;

	/* Copy the old page to temporary space */
	memcpy_aligned<UNIV_PAGE_SIZE_MIN>(temp_page, block->page.frame,
					   srv_page_size);

	/* Recreate the page: note that global data on page (possible
	segment headers, next page-field, etc.) is preserved intact */

	page_create(block, mtr, true);
	if (index->is_spatial()) {
		mach_write_to_2(FIL_PAGE_TYPE + page, FIL_PAGE_RTREE);
		memcpy_aligned<2>(block->page.zip.data + FIL_PAGE_TYPE,
				  page + FIL_PAGE_TYPE, 2);
		memset(FIL_RTREE_SPLIT_SEQ_NUM + page, 0, 8);
		memset(FIL_RTREE_SPLIT_SEQ_NUM + block->page.zip.data, 0, 8);
	}

	/* Copy the records from the temporary space to the recreated page;
	do not copy the lock bits yet */

	page_copy_rec_list_end_no_locks(block, temp_block,
					page_get_infimum_rec(temp_page),
					index, mtr);

	/* Copy the PAGE_MAX_TRX_ID or PAGE_ROOT_AUTO_INC. */
	memcpy_aligned<8>(page + (PAGE_HEADER + PAGE_MAX_TRX_ID),
			  temp_page + (PAGE_HEADER + PAGE_MAX_TRX_ID), 8);
	/* PAGE_MAX_TRX_ID must be set on secondary index leaf pages. */
	ut_ad(dict_index_is_clust(index) || !page_is_leaf(temp_page)
	      || page_get_max_trx_id(page) != 0);
	/* PAGE_MAX_TRX_ID must be zero on non-leaf pages other than
	clustered index root pages. */
	ut_ad(page_get_max_trx_id(page) == 0
	      || (dict_index_is_clust(index)
		  ? !page_has_siblings(temp_page)
		  : page_is_leaf(temp_page)));

	/* Restore logging. */
	mtr_set_log_mode(mtr, log_mode);

	if (!page_zip_compress(block, index, z_level, mtr)) {
		if (restore) {
			/* Restore the old page and exit. */
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
			/* Check that the bytes that we skip are identical. */
			ut_a(!memcmp(page, temp_page, PAGE_HEADER));
			ut_a(!memcmp(PAGE_HEADER + PAGE_N_RECS + page,
				     PAGE_HEADER + PAGE_N_RECS + temp_page,
				     PAGE_DATA - (PAGE_HEADER + PAGE_N_RECS)));
			ut_a(!memcmp(srv_page_size - FIL_PAGE_DATA_END + page,
				     srv_page_size - FIL_PAGE_DATA_END
				     + temp_page,
				     FIL_PAGE_DATA_END));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

			memcpy(PAGE_HEADER + page, PAGE_HEADER + temp_page,
			       PAGE_N_RECS - PAGE_N_DIR_SLOTS);
			memcpy(PAGE_DATA + page, PAGE_DATA + temp_page,
			       srv_page_size - PAGE_DATA - FIL_PAGE_DATA_END);

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
			ut_a(!memcmp(page, temp_page, srv_page_size));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
		}

		buf_block_free(temp_block);
		return false;
	}

	lock_move_reorganize_page(block, temp_block);

	buf_block_free(temp_block);
	return true;
}

/**********************************************************************//**
Copy the records of a page byte for byte.  Do not copy the page header
or trailer, except those B-tree header fields that are directly
related to the storage of records.  Also copy PAGE_MAX_TRX_ID.
NOTE: The caller must update the lock table and the adaptive hash index. */
void
page_zip_copy_recs(
	buf_block_t*		block,		/*!< in/out: buffer block */
	const page_zip_des_t*	src_zip,	/*!< in: compressed page */
	const page_t*		src,		/*!< in: page */
	dict_index_t*		index,		/*!< in: index of the B-tree */
	mtr_t*			mtr)		/*!< in: mini-transaction */
{
	page_t* page = block->page.frame;
	page_zip_des_t* page_zip = &block->page.zip;

	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr->memo_contains_page_flagged(src, MTR_MEMO_PAGE_X_FIX));
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(!index->table->is_temporary());
#ifdef UNIV_ZIP_DEBUG
	/* The B-tree operations that call this function may set
	FIL_PAGE_PREV or PAGE_LEVEL, causing a temporary min_rec_flag
	mismatch.  A strict page_zip_validate() will be executed later
	during the B-tree operations. */
	ut_a(page_zip_validate_low(src_zip, src, index, TRUE));
#endif /* UNIV_ZIP_DEBUG */
	ut_a(page_zip_get_size(page_zip) == page_zip_get_size(src_zip));
	if (UNIV_UNLIKELY(src_zip->n_blobs)) {
		ut_a(page_is_leaf(src));
		ut_a(dict_index_is_clust(index));
	}

	MEM_CHECK_ADDRESSABLE(page, srv_page_size);
	MEM_CHECK_ADDRESSABLE(page_zip->data, page_zip_get_size(page_zip));
	MEM_CHECK_DEFINED(src, srv_page_size);
	MEM_CHECK_DEFINED(src_zip->data, page_zip_get_size(page_zip));

	/* Copy those B-tree page header fields that are related to
	the records stored in the page.  Also copy the field
	PAGE_MAX_TRX_ID.  Skip the rest of the page header and
	trailer.  On the compressed page, there is no trailer. */
	compile_time_assert(PAGE_MAX_TRX_ID + 8 == PAGE_HEADER_PRIV_END);
	memcpy_aligned<2>(PAGE_HEADER + page, PAGE_HEADER + src,
			  PAGE_HEADER_PRIV_END);
	memcpy_aligned<2>(PAGE_DATA + page, PAGE_DATA + src,
			  srv_page_size - (PAGE_DATA + FIL_PAGE_DATA_END));
	memcpy_aligned<2>(PAGE_HEADER + page_zip->data,
			  PAGE_HEADER + src_zip->data,
			  PAGE_HEADER_PRIV_END);
	memcpy_aligned<2>(PAGE_DATA + page_zip->data,
			  PAGE_DATA + src_zip->data,
			  page_zip_get_size(page_zip) - PAGE_DATA);

	if (dict_index_is_clust(index)) {
		/* Reset the PAGE_ROOT_AUTO_INC field when copying
		from a root page. */
		memset_aligned<8>(PAGE_HEADER + PAGE_ROOT_AUTO_INC
				  + page, 0, 8);
		memset_aligned<8>(PAGE_HEADER + PAGE_ROOT_AUTO_INC
				  + page_zip->data, 0, 8);
	} else {
		/* The PAGE_MAX_TRX_ID must be nonzero on leaf pages
		of secondary indexes, and 0 on others. */
		ut_ad(!page_is_leaf(src) == !page_get_max_trx_id(src));
	}

	/* Copy all fields of src_zip to page_zip, except the pointer
	to the compressed data page. */
	{
		page_zip_t*	data = page_zip->data;
		new (page_zip) page_zip_des_t(*src_zip, false);
		page_zip->data = data;
	}
	ut_ad(page_zip_get_trailer_len(page_zip, dict_index_is_clust(index))
	      + page_zip->m_end < page_zip_get_size(page_zip));

	if (!page_is_leaf(src)
	    && UNIV_UNLIKELY(!page_has_prev(src))
	    && UNIV_LIKELY(page_has_prev(page))) {
		/* Clear the REC_INFO_MIN_REC_FLAG of the first user record. */
		ulint	offs = rec_get_next_offs(page + PAGE_NEW_INFIMUM,
						 TRUE);
		if (UNIV_LIKELY(offs != PAGE_NEW_SUPREMUM)) {
			rec_t*	rec = page + offs;
			ut_a(rec[-REC_N_NEW_EXTRA_BYTES]
			     & REC_INFO_MIN_REC_FLAG);
			rec[-REC_N_NEW_EXTRA_BYTES]
				&= byte(~REC_INFO_MIN_REC_FLAG);
		}
	}

#ifdef UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
	page_zip_compress_write_log(block, index, mtr);
}
#endif /* !UNIV_INNOCHECKSUM */

/** Calculate the compressed page checksum.
@param data		compressed page
@param size		size of compressed page
@param use_adler	whether to use Adler32 instead of a XOR of 3 CRC-32C
@return page checksum */
uint32_t page_zip_calc_checksum(const void *data, size_t size, bool use_adler)
{
	uLong		adler;
	const Bytef*	s = static_cast<const byte*>(data);

	/* Exclude FIL_PAGE_SPACE_OR_CHKSUM, FIL_PAGE_LSN,
	and FIL_PAGE_FILE_FLUSH_LSN from the checksum. */
	ut_ad(size > FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

	if (!use_adler) {
		return my_crc32c(0, s + FIL_PAGE_OFFSET,
				 FIL_PAGE_LSN - FIL_PAGE_OFFSET)
			^ my_crc32c(0, s + FIL_PAGE_TYPE, 2)
			^ my_crc32c(0, s + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
				    size - FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	} else {
		adler = adler32(0L, s + FIL_PAGE_OFFSET,
				FIL_PAGE_LSN - FIL_PAGE_OFFSET);
		adler = adler32(adler, s + FIL_PAGE_TYPE, 2);
		adler = adler32(
			adler, s + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
			static_cast<uInt>(size)
			- FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

		return(uint32_t(adler));
	}
}

/** Validate the checksum on a ROW_FORMAT=COMPRESSED page.
@param data    ROW_FORMAT=COMPRESSED page
@param size    size of the page, in bytes
@return whether the stored checksum matches innodb_checksum_algorithm */
bool page_zip_verify_checksum(const byte *data, size_t size)
{
	if (buf_is_zeroes(span<const byte>(data, size))) {
		return true;
	}

	const uint32_t stored = mach_read_from_4(
		data + FIL_PAGE_SPACE_OR_CHKSUM);

	uint32_t calc = page_zip_calc_checksum(data, size, false);

#ifdef UNIV_INNOCHECKSUM
	extern FILE* log_file;
	extern uint32_t cur_page_num;

	if (log_file) {
		fprintf(log_file, "page::" UINT32PF ";"
			" checksum: calculated = " UINT32PF ";"
			" recorded = " UINT32PF "\n", cur_page_num,
			calc, stored);
	}
#endif /* UNIV_INNOCHECKSUM */

	if (stored == calc) {
		return(TRUE);
	}

#ifndef UNIV_INNOCHECKSUM
	switch (srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		break;
	default:
		if (stored == BUF_NO_CHECKSUM_MAGIC) {
			return(TRUE);
		}

		return stored == page_zip_calc_checksum(data, size, true);
	}
#endif /* !UNIV_INNOCHECKSUM */

	return FALSE;
}
