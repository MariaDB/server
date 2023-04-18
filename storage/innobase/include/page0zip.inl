/*****************************************************************************

Copyright (c) 2005, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/page0zip.ic
Compressed page interface

Created June 2005 by Marko Makela
*******************************************************/

#include "page0page.h"

/* The format of compressed pages is as follows.

The header and trailer of the uncompressed pages, excluding the page
directory in the trailer, are copied as is to the header and trailer
of the compressed page.

At the end of the compressed page, there is a dense page directory
pointing to every user record contained on the page, including deleted
records on the free list.  The dense directory is indexed in the
collation order, i.e., in the order in which the record list is
linked on the uncompressed page.  The infimum and supremum records are
excluded.  The two most significant bits of the entries are allocated
for the delete-mark and an n_owned flag indicating the last record in
a chain of records pointed to from the sparse page directory on the
uncompressed page.

The data between PAGE_ZIP_START and the last page directory entry will
be written in compressed format, starting at offset PAGE_DATA.
Infimum and supremum records are not stored.  We exclude the
REC_N_NEW_EXTRA_BYTES in every record header.  These can be recovered
from the dense page directory stored at the end of the compressed
page.

The fields node_ptr (in non-leaf B-tree nodes; level>0), trx_id and
roll_ptr (in leaf B-tree nodes; level=0), and BLOB pointers of
externally stored columns are stored separately, in ascending order of
heap_no and column index, starting backwards from the dense page
directory.

The compressed data stream may be followed by a modification log
covering the compressed portion of the page, as follows.

MODIFICATION LOG ENTRY FORMAT
- write record:
  - (heap_no - 1) << 1 (1..2 bytes)
  - extra bytes backwards
  - data bytes
- clear record:
  - (heap_no - 1) << 1 | 1 (1..2 bytes)

The integer values are stored in a variable-length format:
- 0xxxxxxx: 0..127
- 1xxxxxxx xxxxxxxx: 0..32767

The end of the modification log is marked by a 0 byte.

In summary, the compressed page looks like this:

(1) Uncompressed page header (PAGE_DATA bytes)
(2) Compressed index information
(3) Compressed page data
(4) Page modification log (page_zip->m_start..page_zip->m_end)
(5) Empty zero-filled space
(6) BLOB pointers (on leaf pages)
  - BTR_EXTERN_FIELD_REF_SIZE for each externally stored column
  - in descending collation order
(7) Uncompressed columns of user records, n_dense * uncompressed_size bytes,
  - indexed by heap_no
  - DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN for leaf pages of clustered indexes
  - REC_NODE_PTR_SIZE for non-leaf pages
  - 0 otherwise
(8) dense page directory, stored backwards
  - n_dense = n_heap - 2
  - existing records in ascending collation order
  - deleted records (free list) in link order
*/

/**********************************************************************//**
Determine the size of a compressed page in bytes.
@return size in bytes */
UNIV_INLINE
ulint
page_zip_get_size(
/*==============*/
	const page_zip_des_t*	page_zip)	/*!< in: compressed page */
{
	ulint	size;

	if (!page_zip->ssize) {
		return(0);
	}

	size = (UNIV_ZIP_SIZE_MIN >> 1) << page_zip->ssize;

	ut_ad(size >= UNIV_ZIP_SIZE_MIN);
	ut_ad(size <= srv_page_size);

	return(size);
}
/**********************************************************************//**
Set the size of a compressed page in bytes. */
UNIV_INLINE
void
page_zip_set_size(
/*==============*/
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page */
	ulint		size)		/*!< in: size in bytes */
{
	if (size) {
		unsigned	ssize;

		ut_ad(ut_is_2pow(size));

		for (ssize = 1; size > (512U << ssize); ssize++) {
		}

		page_zip->ssize = ssize & ((1U << PAGE_ZIP_SSIZE_BITS) - 1);
	} else {
		page_zip->ssize = 0;
	}

	ut_ad(page_zip_get_size(page_zip) == size);
}

/** Determine if a record is so big that it needs to be stored externally.
@param[in]	rec_size	length of the record in bytes
@param[in]	comp		nonzero=compact format
@param[in]	n_fields	number of fields in the record; ignored if
tablespace is not compressed
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return false if the entire record can be stored locally on the page */
inline bool page_zip_rec_needs_ext(ulint rec_size, ulint comp, ulint n_fields,
				   ulint zip_size)
{
	/* FIXME: row size check is this function seems to be the most correct.
	Put it in a separate function and use in more places of InnoDB */

	ut_ad(rec_size
	      > ulint(comp ? REC_N_NEW_EXTRA_BYTES : REC_N_OLD_EXTRA_BYTES));
	ut_ad(comp || !zip_size);

#if UNIV_PAGE_SIZE_MAX > COMPRESSED_REC_MAX_DATA_SIZE
	if (comp ? rec_size >= COMPRESSED_REC_MAX_DATA_SIZE :
		   rec_size >= REDUNDANT_REC_MAX_DATA_SIZE) {
		return(TRUE);
	}
#endif

	if (zip_size) {
		ut_ad(comp);
		/* On a compressed page, there is a two-byte entry in
		the dense page directory for every record.  But there
		is no record header.  There should be enough room for
		one record on an empty leaf page.  Subtract 1 byte for
		the encoded heap number.  Check also the available space
		on the uncompressed page. */
		return(rec_size - (REC_N_NEW_EXTRA_BYTES - 2 - 1)
		       >= page_zip_empty_size(n_fields, zip_size)
		       || rec_size >= page_get_free_space_of_empty(TRUE) / 2);
	}

	return(rec_size >= page_get_free_space_of_empty(comp) / 2);
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate a compressed page descriptor.
@return TRUE if ok */
UNIV_INLINE
ibool
page_zip_simple_validate(
/*=====================*/
	const page_zip_des_t*	page_zip)/*!< in: compressed page descriptor */
{
	ut_ad(page_zip);
	ut_ad(page_zip->data);
	ut_ad(page_zip->ssize <= PAGE_ZIP_SSIZE_MAX);
	ut_ad(page_zip_get_size(page_zip)
	      > PAGE_DATA + PAGE_ZIP_DIR_SLOT_SIZE);
	ut_ad(page_zip->m_start <= page_zip->m_end);
	ut_ad(page_zip->m_end < page_zip_get_size(page_zip));
	ut_ad(page_zip->n_blobs
	      < page_zip_get_size(page_zip) / BTR_EXTERN_FIELD_REF_SIZE);
	return(TRUE);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Determine if the length of the page trailer.
@return length of the page trailer, in bytes, not including the
terminating zero byte of the modification log */
UNIV_INLINE
ibool
page_zip_get_trailer_len(
/*=====================*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	ibool			is_clust)/*!< in: TRUE if clustered index */
{
	ulint	uncompressed_size;

	ut_ad(page_zip_simple_validate(page_zip));
	MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

	if (!page_is_leaf(page_zip->data)) {
		uncompressed_size = PAGE_ZIP_DIR_SLOT_SIZE
			+ REC_NODE_PTR_SIZE;
		ut_ad(!page_zip->n_blobs);
	} else if (is_clust) {
		uncompressed_size = PAGE_ZIP_DIR_SLOT_SIZE
			+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
	} else {
		uncompressed_size = PAGE_ZIP_DIR_SLOT_SIZE;
		ut_ad(!page_zip->n_blobs);
	}

	return (ulint(page_dir_get_n_heap(page_zip->data)) - 2)
		* uncompressed_size
		+ ulint(page_zip->n_blobs) * BTR_EXTERN_FIELD_REF_SIZE;
}

/**********************************************************************//**
Determine how big record can be inserted without recompressing the page.
@return a positive number indicating the maximum size of a record
whose insertion is guaranteed to succeed, or zero or negative */
UNIV_INLINE
lint
page_zip_max_ins_size(
/*==================*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	ibool			is_clust)/*!< in: TRUE if clustered index */
{
	ulint	trailer_len;

	trailer_len = page_zip_get_trailer_len(page_zip, is_clust);

	/* When a record is created, a pointer may be added to
	the dense directory.
	Likewise, space for the columns that will not be
	compressed will be allocated from the page trailer.
	Also the BLOB pointers will be allocated from there, but
	we may as well count them in the length of the record. */

	trailer_len += PAGE_ZIP_DIR_SLOT_SIZE;

	return(lint(page_zip_get_size(page_zip)
		    - trailer_len - page_zip->m_end
		    - (REC_N_NEW_EXTRA_BYTES - 2)));
}

/**********************************************************************//**
Determine if enough space is available in the modification log.
@return TRUE if enough space is available */
UNIV_INLINE
ibool
page_zip_available(
/*===============*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	ibool			is_clust,/*!< in: TRUE if clustered index */
	ulint			length,	/*!< in: combined size of the record */
	ulint			create)	/*!< in: nonzero=add the record to
					the heap */
{
	ulint	trailer_len;

	ut_ad(length > REC_N_NEW_EXTRA_BYTES);

	trailer_len = page_zip_get_trailer_len(page_zip, is_clust);

	/* Subtract the fixed extra bytes and add the maximum
	space needed for identifying the record (encoded heap_no). */
	length -= REC_N_NEW_EXTRA_BYTES - 2;

	if (create > 0) {
		/* When a record is created, a pointer may be added to
		the dense directory.
		Likewise, space for the columns that will not be
		compressed will be allocated from the page trailer.
		Also the BLOB pointers will be allocated from there, but
		we may as well count them in the length of the record. */

		trailer_len += PAGE_ZIP_DIR_SLOT_SIZE;
	}

	return(length + trailer_len + page_zip->m_end
	       < page_zip_get_size(page_zip));
}

/**********************************************************************//**
Reset the counters used for filling
INFORMATION_SCHEMA.innodb_cmp_per_index. */
UNIV_INLINE
void
page_zip_reset_stat_per_index()
/*===========================*/
{
	mysql_mutex_lock(&page_zip_stat_per_index_mutex);
	page_zip_stat_per_index.clear();
	mysql_mutex_unlock(&page_zip_stat_per_index_mutex);
}
