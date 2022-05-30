/*****************************************************************************

Copyright (c) 2005, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
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
@file include/page0zip.h
Compressed page interface

Created June 2005 by Marko Makela
*******************************************************/

#ifndef page0zip_h
#define page0zip_h

#include "buf0types.h"

#ifndef UNIV_INNOCHECKSUM
#include "mtr0types.h"
#include "page0types.h"
#include "dict0types.h"
#include "srv0srv.h"
#include "trx0types.h"
#include "mem0mem.h"

/* Compression level to be used by zlib. Settable by user. */
extern uint	page_zip_level;

/* Default compression level. */
#define DEFAULT_COMPRESSION_LEVEL	6
/** Start offset of the area that will be compressed */
#define PAGE_ZIP_START			PAGE_NEW_SUPREMUM_END
/** Size of an compressed page directory entry */
#define PAGE_ZIP_DIR_SLOT_SIZE		2
/** Predefine the sum of DIR_SLOT, TRX_ID & ROLL_PTR */
#define PAGE_ZIP_CLUST_LEAF_SLOT_SIZE		\
		(PAGE_ZIP_DIR_SLOT_SIZE		\
		+ DATA_TRX_ID_LEN		\
		+ DATA_ROLL_PTR_LEN)
/** Mask of record offsets */
#define PAGE_ZIP_DIR_SLOT_MASK		0x3fffU
/** 'owned' flag */
#define PAGE_ZIP_DIR_SLOT_OWNED		0x4000U
/** 'deleted' flag */
#define PAGE_ZIP_DIR_SLOT_DEL		0x8000U

/**********************************************************************//**
Determine the size of a compressed page in bytes.
@return size in bytes */
UNIV_INLINE
ulint
page_zip_get_size(
/*==============*/
	const page_zip_des_t*	page_zip)	/*!< in: compressed page */
	MY_ATTRIBUTE((warn_unused_result));
/**********************************************************************//**
Set the size of a compressed page in bytes. */
UNIV_INLINE
void
page_zip_set_size(
/*==============*/
	page_zip_des_t*	page_zip,	/*!< in/out: compressed page */
	ulint		size);		/*!< in: size in bytes */

/** Determine if a record is so big that it needs to be stored externally.
@param[in]	rec_size	length of the record in bytes
@param[in]	comp		nonzero=compact format
@param[in]	n_fields	number of fields in the record; ignored if
tablespace is not compressed
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return false if the entire record can be stored locally on the page */
inline bool page_zip_rec_needs_ext(ulint rec_size, ulint comp, ulint n_fields,
				   ulint zip_size)
	MY_ATTRIBUTE((warn_unused_result));

/**********************************************************************//**
Determine the guaranteed free space on an empty page.
@return minimum payload size on the page */
ulint
page_zip_empty_size(
/*================*/
	ulint	n_fields,	/*!< in: number of columns in the index */
	ulint	zip_size)	/*!< in: compressed page size in bytes */
	MY_ATTRIBUTE((const));

/** Check whether a tuple is too big for compressed table
@param[in]	index	dict index object
@param[in]	entry	entry for the index
@return	true if it's too big, otherwise false */
bool
page_zip_is_too_big(
	const dict_index_t*	index,
	const dtuple_t*		entry);

/**********************************************************************//**
Initialize a compressed page descriptor. */
#define page_zip_des_init(page_zip) (page_zip)->clear()

/**********************************************************************//**
Configure the zlib allocator to use the given memory heap. */
void
page_zip_set_alloc(
/*===============*/
	void*		stream,		/*!< in/out: zlib stream */
	mem_heap_t*	heap);		/*!< in: memory heap to use */

/** Attempt to compress a ROW_FORMAT=COMPRESSED page.
@retval true on success
@retval false on failure; block->page.zip will be left intact. */
bool
page_zip_compress(
	buf_block_t*		block,	/*!< in/out: buffer block */
	dict_index_t*		index,	/*!< in: index of the B-tree node */
	ulint			level,	/*!< in: commpression level */
	mtr_t*			mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));

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
	byte*			buf);	/*!< out: buffer of (n + 1) * 2 bytes */

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
	MY_ATTRIBUTE((nonnull(1,2)));

#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate a compressed page descriptor.
@return TRUE if ok */
UNIV_INLINE
ibool
page_zip_simple_validate(
/*=====================*/
	const page_zip_des_t*	page_zip);	/*!< in: compressed page
						descriptor */
#endif /* UNIV_DEBUG */

#ifdef UNIV_ZIP_DEBUG
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
	MY_ATTRIBUTE((nonnull(1,2)));
/**********************************************************************//**
Check that the compressed and decompressed pages match. */
ibool
page_zip_validate(
/*==============*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	const page_t*		page,	/*!< in: uncompressed page */
	const dict_index_t*	index)	/*!< in: index of the page, if known */
	MY_ATTRIBUTE((nonnull(1,2)));
#endif /* UNIV_ZIP_DEBUG */

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
	MY_ATTRIBUTE((warn_unused_result));

/**********************************************************************//**
Determine if enough space is available in the modification log.
@return TRUE if page_zip_write_rec() will succeed */
UNIV_INLINE
ibool
page_zip_available(
/*===============*/
	const page_zip_des_t*	page_zip,/*!< in: compressed page */
	ibool			is_clust,/*!< in: TRUE if clustered index */
	ulint			length,	/*!< in: combined size of the record */
	ulint			create)	/*!< in: nonzero=add the record to
					the heap */
	MY_ATTRIBUTE((warn_unused_result));

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
  MY_ATTRIBUTE((nonnull));

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
	MY_ATTRIBUTE((nonnull));

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
	MY_ATTRIBUTE((nonnull));

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
	MY_ATTRIBUTE((nonnull));

/** Modify the delete-mark flag of a ROW_FORMAT=COMPRESSED record.
@param[in,out]  block   buffer block
@param[in,out]  rec     record on a physical index page
@param[in]      flag    the value of the delete-mark flag
@param[in,out]  mtr     mini-transaction  */
void page_zip_rec_set_deleted(buf_block_t *block, rec_t *rec, bool flag,
                              mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));

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
	MY_ATTRIBUTE((nonnull(1,3,4)));

/** Shift the dense page directory and the array of BLOB pointers
when a record is deleted.
@param[in,out]  block   index page
@param[in,out]  rec     record being deleted
@param[in]      index   the index that the page belongs to
@param[in]      offsets rec_get_offsets(rec, index)
@param[in]	free	previous start of the free list
@param[in,out]  mtr     mini-transaction */
void page_zip_dir_delete(buf_block_t *block, byte *rec,
                         const dict_index_t *index, const rec_offs *offsets,
                         const byte *free, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull(1,2,3,4,6)));

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
@retval false on failure; the block_zip will be left intact */
bool
page_zip_reorganize(
	buf_block_t*	block,	/*!< in/out: page with compressed page;
				on the compressed page, in: size;
				out: data, n_blobs,
				m_start, m_end, m_nonempty */
	dict_index_t*	index,	/*!< in: index of the B-tree node */
	ulint		z_level,/*!< in: compression level */
	mtr_t*		mtr,	/*!< in: mini-transaction */
	bool		restore = false)/*!< whether to restore on failure */
	MY_ATTRIBUTE((nonnull));

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
	mtr_t*			mtr);		/*!< in: mini-transaction */
#endif /* !UNIV_INNOCHECKSUM */

/** Calculate the compressed page checksum.
@param data		compressed page
@param size		size of compressed page
@param use_adler	whether to use Adler32 instead of a XOR of 3 CRC-32C
@return page checksum */
uint32_t page_zip_calc_checksum(const void *data, size_t size, bool use_adler);

/** Validate the checksum on a ROW_FORMAT=COMPRESSED page.
@param data    ROW_FORMAT=COMPRESSED page
@param size    size of the page, in bytes
@return whether the stored checksum matches innodb_checksum_algorithm */
bool page_zip_verify_checksum(const byte *data, size_t size);

#ifndef UNIV_INNOCHECKSUM
/**********************************************************************//**
Reset the counters used for filling
INFORMATION_SCHEMA.innodb_cmp_per_index. */
UNIV_INLINE
void
page_zip_reset_stat_per_index();
/*===========================*/

#include "page0zip.inl"
#endif /* !UNIV_INNOCHECKSUM */

#endif /* page0zip_h */
