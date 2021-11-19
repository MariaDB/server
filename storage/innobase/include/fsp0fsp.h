/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2021, MariaDB Corporation.

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
@file include/fsp0fsp.h
File space management

Created 12/18/1995 Heikki Tuuri
*******************************************************/

#ifndef fsp0fsp_h
#define fsp0fsp_h

#include "assume_aligned.h"
#include "fsp0types.h"
#include "fut0lst.h"
#include "ut0byte.h"

#ifndef UNIV_INNOCHECKSUM
#include "mtr0mtr.h"
#include "page0types.h"
#include "rem0types.h"
#else
# include "mach0data.h"
#endif /* !UNIV_INNOCHECKSUM */

/** @return the PAGE_SSIZE flags for the current innodb_page_size */
#define FSP_FLAGS_PAGE_SSIZE()						\
	((srv_page_size == UNIV_PAGE_SIZE_ORIG) ?			\
	 0U : (srv_page_size_shift - UNIV_ZIP_SIZE_SHIFT_MIN + 1)	\
	 << FSP_FLAGS_POS_PAGE_SSIZE)

/** @return the PAGE_SSIZE flags for the current innodb_page_size in
full checksum format */
#define FSP_FLAGS_FCRC32_PAGE_SSIZE()					\
	((srv_page_size_shift - UNIV_ZIP_SIZE_SHIFT_MIN + 1)		\
	<< FSP_FLAGS_FCRC32_POS_PAGE_SSIZE)

/* @defgroup Compatibility macros for MariaDB 10.1.0 through 10.1.20;
see the table in fsp0types.h @{ */
/** Zero relative shift position of the PAGE_COMPRESSION field */
#define FSP_FLAGS_POS_PAGE_COMPRESSION_MARIADB101	\
	(FSP_FLAGS_POS_ATOMIC_BLOBS			\
	 + FSP_FLAGS_WIDTH_ATOMIC_BLOBS)
/** Zero relative shift position of the PAGE_COMPRESSION_LEVEL field */
#define FSP_FLAGS_POS_PAGE_COMPRESSION_LEVEL_MARIADB101	\
	(FSP_FLAGS_POS_PAGE_COMPRESSION_MARIADB101 + 1)
/** Zero relative shift position of the ATOMIC_WRITES field */
#define FSP_FLAGS_POS_ATOMIC_WRITES_MARIADB101		\
	(FSP_FLAGS_POS_PAGE_COMPRESSION_LEVEL_MARIADB101 + 4)
/** Zero relative shift position of the PAGE_SSIZE field */
#define FSP_FLAGS_POS_PAGE_SSIZE_MARIADB101		\
	(FSP_FLAGS_POS_ATOMIC_WRITES_MARIADB101 + 2)

/** Bit mask of the PAGE_COMPRESSION field */
#define FSP_FLAGS_MASK_PAGE_COMPRESSION_MARIADB101		\
	(1U << FSP_FLAGS_POS_PAGE_COMPRESSION_MARIADB101)
/** Bit mask of the PAGE_COMPRESSION_LEVEL field */
#define FSP_FLAGS_MASK_PAGE_COMPRESSION_LEVEL_MARIADB101	\
	(15U << FSP_FLAGS_POS_PAGE_COMPRESSION_LEVEL_MARIADB101)
/** Bit mask of the ATOMIC_WRITES field */
#define FSP_FLAGS_MASK_ATOMIC_WRITES_MARIADB101			\
	(3U << FSP_FLAGS_POS_ATOMIC_WRITES_MARIADB101)
/** Bit mask of the PAGE_SSIZE field */
#define FSP_FLAGS_MASK_PAGE_SSIZE_MARIADB101			\
	(15U << FSP_FLAGS_POS_PAGE_SSIZE_MARIADB101)

/** Return the value of the PAGE_COMPRESSION field */
#define FSP_FLAGS_GET_PAGE_COMPRESSION_MARIADB101(flags)	\
		((flags & FSP_FLAGS_MASK_PAGE_COMPRESSION_MARIADB101)	\
		>> FSP_FLAGS_POS_PAGE_COMPRESSION_MARIADB101)
/** Return the value of the PAGE_COMPRESSION_LEVEL field */
#define FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL_MARIADB101(flags)	\
		((flags & FSP_FLAGS_MASK_PAGE_COMPRESSION_LEVEL_MARIADB101) \
		>> FSP_FLAGS_POS_PAGE_COMPRESSION_LEVEL_MARIADB101)
/** Return the value of the PAGE_SSIZE field */
#define FSP_FLAGS_GET_PAGE_SSIZE_MARIADB101(flags)		\
		((flags & FSP_FLAGS_MASK_PAGE_SSIZE_MARIADB101)	\
		>> FSP_FLAGS_POS_PAGE_SSIZE_MARIADB101)

/* @} */

/* @defgroup Tablespace Header Constants (moved from fsp0fsp.c) @{ */

/** Offset of the space header within a file page */
#define FSP_HEADER_OFFSET	FIL_PAGE_DATA

/* The data structures in files are defined just as byte strings in C */
typedef	byte	xdes_t;

/*			SPACE HEADER
			============

File space header data structure: this data structure is contained in the
first page of a space. The space for this header is reserved in every extent
descriptor page, but used only in the first. */

/*-------------------------------------*/
#define FSP_SPACE_ID		0	/* space id */
#define FSP_NOT_USED		4	/* this field contained a value up to
					which we know that the modifications
					in the database have been flushed to
					the file space; not used now */
#define	FSP_SIZE		8	/* Current size of the space in
					pages */
#define	FSP_FREE_LIMIT		12	/* Minimum page number for which the
					free list has not been initialized:
					the pages >= this limit are, by
					definition, free; note that in a
					single-table tablespace where size
					< 64 pages, this number is 64, i.e.,
					we have initialized the space
					about the first extent, but have not
					physically allocated those pages to the
					file */
#define	FSP_SPACE_FLAGS		16	/* fsp_space_t.flags, similar to
					dict_table_t::flags */
#define	FSP_FRAG_N_USED		20	/* number of used pages in the
					FSP_FREE_FRAG list */
#define	FSP_FREE		24	/* list of free extents */
#define	FSP_FREE_FRAG		(24 + FLST_BASE_NODE_SIZE)
					/* list of partially free extents not
					belonging to any segment */
#define	FSP_FULL_FRAG		(24 + 2 * FLST_BASE_NODE_SIZE)
					/* list of full extents not belonging
					to any segment */
#define FSP_SEG_ID		(24 + 3 * FLST_BASE_NODE_SIZE)
					/* 8 bytes which give the first unused
					segment id */
#define FSP_SEG_INODES_FULL	(32 + 3 * FLST_BASE_NODE_SIZE)
					/* list of pages containing segment
					headers, where all the segment inode
					slots are reserved */
#define FSP_SEG_INODES_FREE	(32 + 4 * FLST_BASE_NODE_SIZE)
					/* list of pages containing segment
					headers, where not all the segment
					header slots are reserved */
/*-------------------------------------*/
/* File space header size */
#define	FSP_HEADER_SIZE		(32 + 5 * FLST_BASE_NODE_SIZE)

#define	FSP_FREE_ADD		4	/* this many free extents are added
					to the free list from above
					FSP_FREE_LIMIT at a time */
/* @} */

/* @defgroup File Segment Inode Constants (moved from fsp0fsp.c) @{ */

/*			FILE SEGMENT INODE
			==================

Segment inode which is created for each segment in a tablespace. NOTE: in
purge we assume that a segment having only one currently used page can be
freed in a few steps, so that the freeing cannot fill the file buffer with
bufferfixed file pages. */

typedef	byte	fseg_inode_t;

#define FSEG_INODE_PAGE_NODE	FSEG_PAGE_DATA
					/* the list node for linking
					segment inode pages */

#define FSEG_ARR_OFFSET		(FSEG_PAGE_DATA + FLST_NODE_SIZE)
/*-------------------------------------*/
#define	FSEG_ID			0	/* 8 bytes of segment id: if this is 0,
					it means that the header is unused */
#define FSEG_NOT_FULL_N_USED	8
					/* number of used segment pages in
					the FSEG_NOT_FULL list */
#define	FSEG_FREE		12
					/* list of free extents of this
					segment */
#define	FSEG_NOT_FULL		(12 + FLST_BASE_NODE_SIZE)
					/* list of partially free extents */
#define	FSEG_FULL		(12 + 2 * FLST_BASE_NODE_SIZE)
					/* list of full extents */
#define	FSEG_MAGIC_N		(12 + 3 * FLST_BASE_NODE_SIZE)
					/* magic number used in debugging */
#define	FSEG_FRAG_ARR		(16 + 3 * FLST_BASE_NODE_SIZE)
					/* array of individual pages
					belonging to this segment in fsp
					fragment extent lists */
#define FSEG_FRAG_ARR_N_SLOTS	(FSP_EXTENT_SIZE / 2)
					/* number of slots in the array for
					the fragment pages */
#define	FSEG_FRAG_SLOT_SIZE	4	/* a fragment page slot contains its
					page number within space, FIL_NULL
					means that the slot is not in use */
/*-------------------------------------*/
#define FSEG_INODE_SIZE					\
	(16 + 3 * FLST_BASE_NODE_SIZE			\
	 + FSEG_FRAG_ARR_N_SLOTS * FSEG_FRAG_SLOT_SIZE)

static constexpr uint32_t FSEG_MAGIC_N_VALUE= 97937874;

#define	FSEG_FILLFACTOR		8	/* If this value is x, then if
					the number of unused but reserved
					pages in a segment is less than
					reserved pages * 1/x, and there are
					at least FSEG_FRAG_LIMIT used pages,
					then we allow a new empty extent to
					be added to the segment in
					fseg_alloc_free_page. Otherwise, we
					use unused pages of the segment. */

#define FSEG_FRAG_LIMIT		FSEG_FRAG_ARR_N_SLOTS
					/* If the segment has >= this many
					used pages, it may be expanded by
					allocating extents to the segment;
					until that only individual fragment
					pages are allocated from the space */

#define	FSEG_FREE_LIST_LIMIT	40	/* If the reserved size of a segment
					is at least this many extents, we
					allow extents to be put to the free
					list of the extent: at most
					FSEG_FREE_LIST_MAX_LEN many */
#define	FSEG_FREE_LIST_MAX_LEN	4
/* @} */

/* @defgroup Extent Descriptor Constants (moved from fsp0fsp.c) @{ */

/*			EXTENT DESCRIPTOR
			=================

File extent descriptor data structure: contains bits to tell which pages in
the extent are free and which contain old tuple version to clean. */

/*-------------------------------------*/
#define	XDES_ID			0	/* The identifier of the segment
					to which this extent belongs */
#define XDES_FLST_NODE		8	/* The list node data structure
					for the descriptors */
#define	XDES_STATE		(FLST_NODE_SIZE + 8)
					/* contains state information
					of the extent */
#define	XDES_BITMAP		(FLST_NODE_SIZE + 12)
					/* Descriptor bitmap of the pages
					in the extent */
/*-------------------------------------*/

#define	XDES_BITS_PER_PAGE	2	/* How many bits are there per page */
#define	XDES_FREE_BIT		0	/* Index of the bit which tells if
					the page is free */
#define	XDES_CLEAN_BIT		1	/* NOTE: currently not used!
					Index of the bit which tells if
					there are old versions of tuples
					on the page */
/* States of a descriptor */
#define	XDES_FREE		1	/* extent is in free list of space */
#define	XDES_FREE_FRAG		2	/* extent is in free fragment list of
					space */
#define	XDES_FULL_FRAG		3	/* extent is in full fragment list of
					space */
#define	XDES_FSEG		4	/* extent belongs to a segment */

/** File extent data structure size in bytes. */
#define	XDES_SIZE							\
	(XDES_BITMAP							\
	+ UT_BITS_IN_BYTES(FSP_EXTENT_SIZE * XDES_BITS_PER_PAGE))

/** File extent data structure size in bytes for MAX page size. */
#define	XDES_SIZE_MAX							\
	(XDES_BITMAP							\
	+ UT_BITS_IN_BYTES(FSP_EXTENT_SIZE_MAX * XDES_BITS_PER_PAGE))

/** File extent data structure size in bytes for MIN page size. */
#define	XDES_SIZE_MIN							\
	(XDES_BITMAP							\
	+ UT_BITS_IN_BYTES(FSP_EXTENT_SIZE_MIN * XDES_BITS_PER_PAGE))

/** Offset of the descriptor array on a descriptor page */
#define	XDES_ARR_OFFSET		(FSP_HEADER_OFFSET + FSP_HEADER_SIZE)

/**
Determine if a page is marked free.
@param[in]	descr	extent descriptor
@param[in]	offset	page offset within extent
@return whether the page is free */
inline bool xdes_is_free(const xdes_t *descr, ulint offset)
{
  ut_ad(offset < FSP_EXTENT_SIZE);
  ulint index= XDES_FREE_BIT + XDES_BITS_PER_PAGE * offset;
  return ut_bit_get_nth(descr[XDES_BITMAP + (index >> 3)], index & 7);
}

#ifndef UNIV_INNOCHECKSUM
/* @} */

/** Read a tablespace header field.
@param[in]	page	first page of a tablespace
@param[in]	field	the header field
@return the contents of the header field */
inline uint32_t fsp_header_get_field(const page_t* page, ulint field)
{
  return mach_read_from_4(FSP_HEADER_OFFSET + field +
			  my_assume_aligned<UNIV_ZIP_SIZE_MIN>(page));
}

/** Read the flags from the tablespace header page.
@param[in]	page	first page of a tablespace
@return the contents of FSP_SPACE_FLAGS */
inline uint32_t fsp_header_get_flags(const page_t *page)
{
  return fsp_header_get_field(page, FSP_SPACE_FLAGS);
}

/** Get the byte offset of encryption information in page 0.
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return	byte offset relative to FSP_HEADER_OFFSET */
inline MY_ATTRIBUTE((pure, warn_unused_result))
ulint fsp_header_get_encryption_offset(ulint zip_size)
{
	return zip_size
		? XDES_ARR_OFFSET + XDES_SIZE * zip_size / FSP_EXTENT_SIZE
		: XDES_ARR_OFFSET + (XDES_SIZE << srv_page_size_shift)
		/ FSP_EXTENT_SIZE;
}

/** Check the encryption key from the first page of a tablespace.
@param[in]	fsp_flags	tablespace flags
@param[in]	page		first page of a tablespace
@return true if success */
bool
fsp_header_check_encryption_key(
	ulint			fsp_flags,
	page_t*			page);

/** Initialize a tablespace header.
@param[in,out]	space	tablespace
@param[in]	size	current size in blocks
@param[in,out]	mtr	mini-transaction */
void fsp_header_init(fil_space_t* space, uint32_t size, mtr_t* mtr)
	MY_ATTRIBUTE((nonnull));

/** Create a new segment.
@param space                tablespace
@param byte_offset          byte offset of the created segment header
@param mtr                  mini-transaction
@param has_done_reservation whether fsp_reserve_free_extents() was invoked
@param block                block where segment header is placed,
                            or NULL to allocate an additional page for that
@return the block where the segment header is placed, x-latched
@retval NULL if could not create segment because of lack of space */
buf_block_t*
fseg_create(fil_space_t *space, ulint byte_offset, mtr_t *mtr,
            bool has_done_reservation= false, buf_block_t *block= NULL);

/** Calculate the number of pages reserved by a segment,
and how many pages are currently used.
@param[in]      block   buffer block containing the file segment header
@param[in]      header  file segment header
@param[out]     used    number of pages that are used (not more than reserved)
@param[in,out]  mtr     mini-transaction
@return number of reserved pages */
ulint fseg_n_reserved_pages(const buf_block_t &block,
                            const fseg_header_t *header, ulint *used,
                            mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));
/**********************************************************************//**
Allocates a single free page from a segment. This function implements
the intelligent allocation strategy which tries to minimize
file space fragmentation.
@param[in,out] seg_header segment header
@param[in] hint hint of which page would be desirable
@param[in] direction if the new page is needed because
				of an index page split, and records are
				inserted there in order, into which
				direction they go alphabetically: FSP_DOWN,
				FSP_UP, FSP_NO_DIR
@param[in,out] mtr mini-transaction
@return X-latched block, or NULL if no page could be allocated */
#define fseg_alloc_free_page(seg_header, hint, direction, mtr)		\
	fseg_alloc_free_page_general(seg_header, hint, direction,	\
				     false, mtr, mtr)
/**********************************************************************//**
Allocates a single free page from a segment. This function implements
the intelligent allocation strategy which tries to minimize file space
fragmentation.
@retval NULL if no page could be allocated */
buf_block_t*
fseg_alloc_free_page_general(
/*=========================*/
	fseg_header_t*	seg_header,/*!< in/out: segment header */
	uint32_t	hint,	/*!< in: hint of which page would be
				desirable */
	byte		direction,/*!< in: if the new page is needed because
				of an index page split, and records are
				inserted there in order, into which
				direction they go alphabetically: FSP_DOWN,
				FSP_UP, FSP_NO_DIR */
	bool		has_done_reservation, /*!< in: true if the caller has
				already done the reservation for the page
				with fsp_reserve_free_extents, then there
				is no need to do the check for this individual
				page */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	mtr_t*		init_mtr)/*!< in/out: mtr or another mini-transaction
				in which the page should be initialized. */
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Reserves free pages from a tablespace. All mini-transactions which may
use several pages from the tablespace should call this function beforehand
and reserve enough free extents so that they certainly will be able
to do their operation, like a B-tree page split, fully. Reservations
must be released with function fil_space_t::release_free_extents()!

The alloc_type below has the following meaning: FSP_NORMAL means an
operation which will probably result in more space usage, like an
insert in a B-tree; FSP_UNDO means allocation to undo logs: if we are
deleting rows, then this allocation will in the long run result in
less space usage (after a purge); FSP_CLEANING means allocation done
in a physical record delete (like in a purge) or other cleaning operation
which will result in less space usage in the long run. We prefer the latter
two types of allocation: when space is scarce, FSP_NORMAL allocations
will not succeed, but the latter two allocations will succeed, if possible.
The purpose is to avoid dead end where the database is full but the
user cannot free any space because these freeing operations temporarily
reserve some space.

Single-table tablespaces whose size is < FSP_EXTENT_SIZE pages are a special
case. In this function we would liberally reserve several extents for
every page split or merge in a B-tree. But we do not want to waste disk space
if the table only occupies < FSP_EXTENT_SIZE pages. That is why we apply
different rules in that special case, just ensuring that there are n_pages
free pages available.

@param[out]	n_reserved	number of extents actually reserved; if we
				return true and the tablespace size is <
				FSP_EXTENT_SIZE pages, then this can be 0,
				otherwise it is n_ext
@param[in,out]	space		tablespace
@param[in]	n_ext		number of extents to reserve
@param[in]	alloc_type	page reservation type (FSP_BLOB, etc)
@param[in,out]	mtr		the mini transaction
@param[in]	n_pages		for small tablespaces (tablespace size is
				less than FSP_EXTENT_SIZE), number of free
				pages to reserve.
@return true if we were able to make the reservation */
bool
fsp_reserve_free_extents(
	uint32_t*	n_reserved,
	fil_space_t*	space,
	uint32_t	n_ext,
	fsp_reserve_t	alloc_type,
	mtr_t*		mtr,
	uint32_t	n_pages = 2);

/** Free a page in a file segment.
@param[in,out]	seg_header	file segment header
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in,out]	mtr		mini-transaction
@param[in]	have_latch	whether space->x_lock() was already called */
void
fseg_free_page(
	fseg_header_t*	seg_header,
	fil_space_t*	space,
	uint32_t	offset,
	mtr_t*		mtr,
	bool		have_latch = false);
/** Determine whether a page is free.
@param[in,out]	space	tablespace
@param[in]	page	page number
@return whether the page is marked as free */
bool
fseg_page_is_free(fil_space_t* space, unsigned page)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Frees part of a segment. This function can be used to free
a segment by repeatedly calling this function in different
mini-transactions. Doing the freeing in a single mini-transaction
might result in too big a mini-transaction.
@param	header	segment header; NOTE: if the header resides on first
		page of the frag list of the segment, this pointer
		becomes obsolete after the last freeing step
@param	mtr	mini-transaction
@param	ahi	Drop the adaptive hash index
@return whether the freeing was completed */
bool
fseg_free_step(
	fseg_header_t*	header,
	mtr_t*		mtr
#ifdef BTR_CUR_HASH_ADAPT
	,bool		ahi=false
#endif /* BTR_CUR_HASH_ADAPT */
	)
	MY_ATTRIBUTE((warn_unused_result));

/** Frees part of a segment. Differs from fseg_free_step because
this function leaves the header page unfreed.
@param	header	segment header which must reside on the first
		fragment page of the segment
@param	mtr	mini-transaction
@param	ahi	drop the adaptive hash index
@return whether the freeing was completed, except for the header page */
bool
fseg_free_step_not_header(
	fseg_header_t*	header,
	mtr_t*		mtr
#ifdef BTR_CUR_HASH_ADAPT
	,bool		ahi=false
#endif /* BTR_CUR_HASH_ADAPT */
	)
	MY_ATTRIBUTE((warn_unused_result));

/** Reset the page type.
Data files created before MySQL 5.1.48 may contain garbage in FIL_PAGE_TYPE.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	block	block with invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
ATTRIBUTE_COLD
void fil_block_reset_type(const buf_block_t& block, ulint type, mtr_t* mtr);

/** Check (and if needed, reset) the page type.
Data files created before MySQL 5.1.48 may contain
garbage in the FIL_PAGE_TYPE field.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	page_id	page number
@param[in,out]	page	page with possibly invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
inline void
fil_block_check_type(
	const buf_block_t&	block,
	ulint			type,
	mtr_t*			mtr)
{
  if (UNIV_UNLIKELY(type != fil_page_get_type(block.page.frame)))
    fil_block_reset_type(block, type, mtr);
}

/** Checks if a page address is an extent descriptor page address.
@param[in]	page_id		page id
@param[in]	physical_size	page size
@return whether a descriptor page */
inline bool fsp_descr_page(const page_id_t page_id, ulint physical_size)
{
	return (page_id.page_no() & (physical_size - 1)) == FSP_XDES_OFFSET;
}

/** Initialize a file page whose prior contents should be ignored.
@param[in,out]	block	buffer pool block */
void fsp_apply_init_file_page(buf_block_t *block);

/** Initialize a file page.
@param[in]	space	tablespace
@param[in,out]	block	file page
@param[in,out]	mtr	mini-transaction */
inline void fsp_init_file_page(
#ifdef UNIV_DEBUG
	const fil_space_t* space,
#endif
	buf_block_t* block, mtr_t* mtr)
{
	ut_d(space->modify_check(*mtr));
	ut_ad(space->id == block->page.id().space());
	fsp_apply_init_file_page(block);
	mtr->init(block);
}

#ifndef UNIV_DEBUG
# define fsp_init_file_page(space, block, mtr) fsp_init_file_page(block, mtr)
#endif

#ifdef UNIV_BTR_PRINT
/*******************************************************************//**
Writes info of a segment. */
void
fseg_print(
/*=======*/
	fseg_header_t*	header, /*!< in: segment header */
	mtr_t*		mtr);	/*!< in/out: mini-transaction */
#endif /* UNIV_BTR_PRINT */

/** Convert FSP_SPACE_FLAGS from the buggy MariaDB 10.1.0..10.1.20 format.
@param[in]	flags	the contents of FSP_SPACE_FLAGS
@return	the flags corrected from the buggy MariaDB 10.1 format
@retval	UINT32_MAX  if the flags are not in the buggy 10.1 format */
MY_ATTRIBUTE((warn_unused_result, const))
inline uint32_t fsp_flags_convert_from_101(uint32_t flags)
{
	DBUG_EXECUTE_IF("fsp_flags_is_valid_failure", return UINT32_MAX;);
	if (flags == 0 || fil_space_t::full_crc32(flags)) {
		return(flags);
	}

	if (flags >> 18) {
		/* The most significant FSP_SPACE_FLAGS bit that was ever set
		by MariaDB 10.1.0 to 10.1.20 was bit 17 (misplaced DATA_DIR flag).
		The flags must be less than 1<<18 in order to be valid. */
		return UINT32_MAX;
	}

	if ((flags & (FSP_FLAGS_MASK_POST_ANTELOPE | FSP_FLAGS_MASK_ATOMIC_BLOBS))
	    == FSP_FLAGS_MASK_ATOMIC_BLOBS) {
		/* If the "atomic blobs" flag (indicating
		ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED) flag
		is set, then the "post Antelope" (ROW_FORMAT!=REDUNDANT) flag
		must also be set. */
		return UINT32_MAX;
	}

	/* Bits 6..10 denote compression in MariaDB 10.1.0 to 10.1.20.
	They must be either 0b00000 or 0b00011 through 0b10011.
	In correct versions, these bits would be
	0bd0sss where d is the DATA_DIR flag (garbage bit) and
	sss is the PAGE_SSIZE (3, 4, 6, or 7).

	NOTE: MariaDB 10.1.0 to 10.1.20 can misinterpret
	uncompressed data files with innodb_page_size=4k or 64k as
	compressed innodb_page_size=16k files. Below is an exhaustive
	state space analysis.

	-0by1zzz: impossible (the bit 4 must be clean; see above)
	-0b101xx: DATA_DIR, innodb_page_size>4k: invalid (COMPRESSION_LEVEL>9)
	+0bx0011: innodb_page_size=4k:
	!!!	Misinterpreted as COMPRESSION_LEVEL=9 or 1, COMPRESSION=1.
	-0bx0010: impossible, because sss must be 0b011 or 0b1xx
	-0bx0001: impossible, because sss must be 0b011 or 0b1xx
	-0b10000: DATA_DIR, innodb_page_size=16:
	invalid (COMPRESSION_LEVEL=8 but COMPRESSION=0)
	+0b00111: no DATA_DIR, innodb_page_size=64k:
	!!!	Misinterpreted as COMPRESSION_LEVEL=3, COMPRESSION=1.
	-0b00101: impossible, because sss must be 0 for 16k, not 0b101
	-0b001x0: no DATA_DIR, innodb_page_size=32k or 8k:
	invalid (COMPRESSION_LEVEL=3 but COMPRESSION=0)
	+0b00000: innodb_page_size=16k (looks like COMPRESSION=0)
	???	Could actually be compressed; see PAGE_SSIZE below */
	const uint32_t level = FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL_MARIADB101(
		flags);
	if (FSP_FLAGS_GET_PAGE_COMPRESSION_MARIADB101(flags) != (level != 0)
	    || level > 9) {
		/* The compression flags are not in the buggy MariaDB
		10.1 format. */
		return UINT32_MAX;
	}
	if (!(~flags & FSP_FLAGS_MASK_ATOMIC_WRITES_MARIADB101)) {
		/* The ATOMIC_WRITES flags cannot be 0b11.
		(The bits 11..12 should actually never be 0b11,
		because in MySQL they would be SHARED|TEMPORARY.) */
		return UINT32_MAX;
	}

	/* Bits 13..16 are the wrong position for PAGE_SSIZE, and they
	should contain one of the values 3,4,6,7, that is, be of the form
	0b0011 or 0b01xx (except 0b0101).
	In correct versions, these bits should be 0bc0se
	where c is the MariaDB COMPRESSED flag
	and e is the MySQL 5.7 ENCRYPTION flag
	and s is the MySQL 8.0 SDI flag. MariaDB can only support s=0, e=0.

	Compressed innodb_page_size=16k tables with correct FSP_SPACE_FLAGS
	will be properly rejected by older MariaDB 10.1.x because they
	would read as PAGE_SSIZE>=8 which is not valid. */

	const uint32_t ssize = FSP_FLAGS_GET_PAGE_SSIZE_MARIADB101(flags);
	if (ssize == 1 || ssize == 2 || ssize == 5 || ssize & 8) {
		/* the page_size is not between 4k and 64k;
		16k should be encoded as 0, not 5 */
		return UINT32_MAX;
	}
	const uint32_t zssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
	if (zssize == 0) {
		/* not ROW_FORMAT=COMPRESSED */
	} else if (zssize > (ssize ? ssize : 5)) {
		/* invalid KEY_BLOCK_SIZE */
		return UINT32_MAX;
	} else if (~flags & (FSP_FLAGS_MASK_POST_ANTELOPE
			     | FSP_FLAGS_MASK_ATOMIC_BLOBS)) {
		/* both these flags should be set for
		ROW_FORMAT=COMPRESSED */
		return UINT32_MAX;
	}

	flags = ((flags & 0x3f) | ssize << FSP_FLAGS_POS_PAGE_SSIZE
		 | FSP_FLAGS_GET_PAGE_COMPRESSION_MARIADB101(flags)
		 << FSP_FLAGS_POS_PAGE_COMPRESSION);
	ut_ad(fil_space_t::is_valid_flags(flags, false));
	return(flags);
}

/** Compare tablespace flags.
@param[in]	expected	expected flags from dict_tf_to_fsp_flags()
@param[in]	actual		flags read from FSP_SPACE_FLAGS
@return whether the flags match */
MY_ATTRIBUTE((warn_unused_result))
inline bool fsp_flags_match(uint32_t expected, uint32_t actual)
{
  expected&= ~FSP_FLAGS_MEM_MASK;
  ut_ad(fil_space_t::is_valid_flags(expected, false));
  return actual == expected || fsp_flags_convert_from_101(actual) == expected;
}

/** Determine the descriptor index within a descriptor page.
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	offset		page offset
@return descriptor index */
inline ulint xdes_calc_descriptor_index(ulint zip_size, ulint offset)
{
	return ut_2pow_remainder<ulint>(offset,
					zip_size ? zip_size : srv_page_size)
		/ FSP_EXTENT_SIZE;
}

/** Determine the descriptor page number for a page.
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	offset		page offset
@return descriptor page offset */
inline uint32_t xdes_calc_descriptor_page(ulint zip_size, uint32_t offset)
{
	compile_time_assert(UNIV_PAGE_SIZE_MAX > XDES_ARR_OFFSET
			    + (UNIV_PAGE_SIZE_MAX / FSP_EXTENT_SIZE_MAX)
			    * XDES_SIZE_MAX);
	compile_time_assert(UNIV_PAGE_SIZE_MIN > XDES_ARR_OFFSET
			    + (UNIV_PAGE_SIZE_MIN / FSP_EXTENT_SIZE_MIN)
			    * XDES_SIZE_MIN);

	ut_ad(srv_page_size > XDES_ARR_OFFSET
	      + (srv_page_size / FSP_EXTENT_SIZE)
	      * XDES_SIZE);
	ut_ad(UNIV_ZIP_SIZE_MIN > XDES_ARR_OFFSET
	      + (UNIV_ZIP_SIZE_MIN / FSP_EXTENT_SIZE)
	      * XDES_SIZE);
	ut_ad(!zip_size
	      || zip_size > XDES_ARR_OFFSET
	      + (zip_size / FSP_EXTENT_SIZE) * XDES_SIZE);
	return ut_2pow_round(offset,
			     uint32_t(zip_size ? zip_size : srv_page_size));
}

#endif /* UNIV_INNOCHECKSUM */

#endif
