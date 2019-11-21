/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2019, MariaDB Corporation.

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

/******************************************************
@file include/fsp0types.h
File space management types

Created May 26, 2009 Vasil Dimov
*******************************************************/

#ifndef fsp0types_h
#define fsp0types_h

#include "univ.i"

#ifndef UNIV_INNOCHECKSUM

/** The fil_space_t::id of the redo log. All persistent tablespaces
have a smaller fil_space_t::id. */
#define SRV_LOG_SPACE_FIRST_ID		0xFFFFFFF0U
/** The fil_space_t::id of the innodb_temporary tablespace. */
#define SRV_TMP_SPACE_ID		0xFFFFFFFEU

#include "ut0byte.h"

/** @name Flags for inserting records in order
If records are inserted in order, there are the following
flags to tell this (their type is made byte for the compiler
to warn if direction and hint parameters are switched in
fseg_alloc_free_page) */
/* @{ */
#define	FSP_UP		((byte)111)	/*!< alphabetically upwards */
#define	FSP_DOWN	((byte)112)	/*!< alphabetically downwards */
#define	FSP_NO_DIR	((byte)113)	/*!< no order */
/* @} */

#endif /* !UNIV_INNOCHECKSUM */
/** File space extent size in pages
page size | file space extent size
----------+-----------------------
   4 KiB  | 256 pages = 1 MiB
   8 KiB  | 128 pages = 1 MiB
  16 KiB  |  64 pages = 1 MiB
  32 KiB  |  64 pages = 2 MiB
  64 KiB  |  64 pages = 4 MiB
*/
#define FSP_EXTENT_SIZE         ((UNIV_PAGE_SIZE <= (16384) ?	\
				(1048576 / UNIV_PAGE_SIZE) :	\
				((UNIV_PAGE_SIZE <= (32768)) ?	\
				(2097152 / UNIV_PAGE_SIZE) :	\
				(4194304 / UNIV_PAGE_SIZE))))

/** File space extent size (four megabyte) in pages for MAX page size */
#define	FSP_EXTENT_SIZE_MAX	(4194304 / UNIV_PAGE_SIZE_MAX)

/** File space extent size (one megabyte) in pages for MIN page size */
#define	FSP_EXTENT_SIZE_MIN	(1048576 / UNIV_PAGE_SIZE_MIN)

/** On a page of any file segment, data may be put starting from this
offset */
#define FSEG_PAGE_DATA		FIL_PAGE_DATA

#ifndef UNIV_INNOCHECKSUM
/** @name File segment header
The file segment header points to the inode describing the file segment. */
/* @{ */
/** Data type for file segment header */
typedef	byte	fseg_header_t;

#define FSEG_HDR_SPACE		0	/*!< space id of the inode */
#define FSEG_HDR_PAGE_NO	4	/*!< page number of the inode */
#define FSEG_HDR_OFFSET		8	/*!< byte offset of the inode */

#define FSEG_HEADER_SIZE	10	/*!< Length of the file system
					header, in bytes */
/* @} */

#ifdef UNIV_DEBUG

struct mtr_t;

/** A wrapper class to print the file segment header information. */
class fseg_header
{
public:
	/** Constructor of fseg_header.
	@param[in]	header	the underlying file segment header object
	@param[in]	mtr	the mini-transaction.  No redo logs are
				generated, only latches are checked within
				mini-transaction */
	fseg_header(
		const fseg_header_t*	header,
		mtr_t*			mtr)
		:
		m_header(header),
		m_mtr(mtr)
	{}

	/** Print the file segment header to the given output stream.
	@param[in,out]	out	the output stream into which the object
				is printed.
	@retval	the output stream into which the object was printed. */
	std::ostream&
	to_stream(std::ostream&	out) const;
private:
	/** The underlying file segment header */
	const fseg_header_t*	m_header;

	/** The mini transaction, which is used mainly to check whether
	appropriate latches have been taken by the calling thread. */
	mtr_t*			m_mtr;
};

/* Overloading the global output operator to print a file segment header
@param[in,out]	out	the output stream into which object will be printed
@param[in]	header	the file segment header to be printed
@retval the output stream */
inline
std::ostream&
operator<<(
	std::ostream&		out,
	const fseg_header&	header)
{
	return(header.to_stream(out));
}
#endif /* UNIV_DEBUG */

/** Flags for fsp_reserve_free_extents */
enum fsp_reserve_t {
	FSP_NORMAL,	/* reservation during normal B-tree operations */
	FSP_UNDO,	/* reservation done for undo logging */
	FSP_CLEANING,	/* reservation done during purge operations */
	FSP_BLOB	/* reservation being done for BLOB insertion */
};

/* Number of pages described in a single descriptor page: currently each page
description takes less than 1 byte; a descriptor page is repeated every
this many file pages */
/* #define XDES_DESCRIBED_PER_PAGE		UNIV_PAGE_SIZE */
/* This has been replaced with either UNIV_PAGE_SIZE or page_zip->size. */

/** @name The space low address page map
The pages at FSP_XDES_OFFSET and FSP_IBUF_BITMAP_OFFSET are repeated
every XDES_DESCRIBED_PER_PAGE pages in every tablespace. */
/* @{ */
/*--------------------------------------*/
#define FSP_XDES_OFFSET			0	/* !< extent descriptor */
#define FSP_IBUF_BITMAP_OFFSET		1	/* !< insert buffer bitmap */
				/* The ibuf bitmap pages are the ones whose
				page number is the number above plus a
				multiple of XDES_DESCRIBED_PER_PAGE */

#define FSP_FIRST_INODE_PAGE_NO		2	/*!< in every tablespace */
				/* The following pages exist
				in the system tablespace (space 0). */
#define FSP_IBUF_HEADER_PAGE_NO		3	/*!< insert buffer
						header page, in
						tablespace 0 */
#define FSP_IBUF_TREE_ROOT_PAGE_NO	4	/*!< insert buffer
						B-tree root page in
						tablespace 0 */
				/* The ibuf tree root page number in
				tablespace 0; its fseg inode is on the page
				number FSP_FIRST_INODE_PAGE_NO */
#define FSP_TRX_SYS_PAGE_NO		5	/*!< transaction
						system header, in
						tablespace 0 */
#define	FSP_FIRST_RSEG_PAGE_NO		6	/*!< first rollback segment
						page, in tablespace 0 */
#define FSP_DICT_HDR_PAGE_NO		7	/*!< data dictionary header
						page, in tablespace 0 */
/*--------------------------------------*/
/* @} */

/** Check if tablespace is system temporary.
@param[in]      space_id        verify is checksum is enabled for given space.
@return true if tablespace is system temporary. */
inline
bool
fsp_is_system_temporary(ulint	space_id)
{
	return(space_id == SRV_TMP_SPACE_ID);
}

#ifdef UNIV_DEBUG
/** Skip some of the sanity checks that are time consuming even in debug mode
and can affect frequent verification runs that are done to ensure stability of
the product.
@return true if check should be skipped for given space. */
bool
fsp_skip_sanity_check(
	ulint	space_id);
#endif /* UNIV_DEBUG */

#endif /* !UNIV_INNOCHECKSUM */

/* @defgroup fsp_flags InnoDB Tablespace Flag Constants @{ */

/** Width of the POST_ANTELOPE flag */
#define FSP_FLAGS_WIDTH_POST_ANTELOPE	1
/** Number of flag bits used to indicate the tablespace zip page size */
#define FSP_FLAGS_WIDTH_ZIP_SSIZE	4
/** Width of the ATOMIC_BLOBS flag.  The ability to break up a long
column into an in-record prefix and an externally stored part is available
to the two Barracuda row formats COMPRESSED and DYNAMIC. */
#define FSP_FLAGS_WIDTH_ATOMIC_BLOBS	1
/** Number of flag bits used to indicate the tablespace page size */
#define FSP_FLAGS_WIDTH_PAGE_SSIZE	4
/** Number of reserved bits */
#define FSP_FLAGS_WIDTH_RESERVED 6
/** Number of flag bits used to indicate the page compression */
#define FSP_FLAGS_WIDTH_PAGE_COMPRESSION 1

/** Width of all the currently known persistent tablespace flags */
#define FSP_FLAGS_WIDTH		(FSP_FLAGS_WIDTH_POST_ANTELOPE	\
				+ FSP_FLAGS_WIDTH_ZIP_SSIZE	\
				+ FSP_FLAGS_WIDTH_ATOMIC_BLOBS	\
				+ FSP_FLAGS_WIDTH_PAGE_SSIZE	\
				+ FSP_FLAGS_WIDTH_RESERVED	\
				+ FSP_FLAGS_WIDTH_PAGE_COMPRESSION)

/** A mask of all the known/used bits in FSP_SPACE_FLAGS */
#define FSP_FLAGS_MASK		(~(~0U << FSP_FLAGS_WIDTH))

/* FSP_SPACE_FLAGS position and name in MySQL 5.6/MariaDB 10.0 or older
and MariaDB 10.1.20 or older MariaDB 10.1 and in MariaDB 10.1.21
or newer.
MySQL 5.6		MariaDB 10.1.x		MariaDB 10.1.21
====================================================================
Below flags in same offset
====================================================================
0: POST_ANTELOPE	0:POST_ANTELOPE		0: POST_ANTELOPE
1..4: ZIP_SSIZE(0..5)	1..4:ZIP_SSIZE(0..5)	1..4: ZIP_SSIZE(0..5)
(NOTE: bit 4 is always 0)
5: ATOMIC_BLOBS    	5:ATOMIC_BLOBS		5: ATOMIC_BLOBS
=====================================================================
Below note the order difference:
=====================================================================
6..9: PAGE_SSIZE(3..7)	6: COMPRESSION		6..9: PAGE_SSIZE(3..7)
10: DATA_DIR		7..10: COMP_LEVEL(0..9)	10: RESERVED (5.6 DATA_DIR)
=====================================================================
The flags below were in incorrect position in MariaDB 10.1,
or have been introduced in MySQL 5.7 or 8.0:
=====================================================================
11: UNUSED		11..12:ATOMIC_WRITES	11: RESERVED (5.7 SHARED)
						12: RESERVED (5.7 TEMPORARY)
			13..15:PAGE_SSIZE(3..7)	13: RESERVED (5.7 ENCRYPTION)
						14: RESERVED (8.0 SDI)
						15: RESERVED
			16: PAGE_SSIZE_msb(0)	16: COMPRESSION
			17: DATA_DIR		17: UNUSED
			18: UNUSED
=====================================================================
The flags below only exist in fil_space_t::flags, not in FSP_SPACE_FLAGS:
=====================================================================
						27: DATA_DIR
						28..31: COMPRESSION_LEVEL
*/

/** A mask of the memory-only flags in fil_space_t::flags */
#define FSP_FLAGS_MEM_MASK		(~0U << FSP_FLAGS_MEM_DATA_DIR)

/** Zero relative shift position of the DATA_DIR flag */
#define FSP_FLAGS_MEM_DATA_DIR		27
/** Zero relative shift position of the COMPRESSION_LEVEL field */
#define FSP_FLAGS_MEM_COMPRESSION_LEVEL	28

/** Zero relative shift position of the POST_ANTELOPE field */
#define FSP_FLAGS_POS_POST_ANTELOPE	0
/** Zero relative shift position of the ZIP_SSIZE field */
#define FSP_FLAGS_POS_ZIP_SSIZE		(FSP_FLAGS_POS_POST_ANTELOPE	\
					+ FSP_FLAGS_WIDTH_POST_ANTELOPE)
/** Zero relative shift position of the ATOMIC_BLOBS field */
#define FSP_FLAGS_POS_ATOMIC_BLOBS	(FSP_FLAGS_POS_ZIP_SSIZE	\
					+ FSP_FLAGS_WIDTH_ZIP_SSIZE)
/** Zero relative shift position of the start of the PAGE_SSIZE bits */
#define FSP_FLAGS_POS_PAGE_SSIZE	(FSP_FLAGS_POS_ATOMIC_BLOBS	\
                                        + FSP_FLAGS_WIDTH_ATOMIC_BLOBS)
/** Zero relative shift position of the start of the RESERVED bits
these are only used in MySQL 5.7 and used for compatibility. */
#define FSP_FLAGS_POS_RESERVED		(FSP_FLAGS_POS_PAGE_SSIZE	\
					+ FSP_FLAGS_WIDTH_PAGE_SSIZE)
/** Zero relative shift position of the PAGE_COMPRESSION field */
#define FSP_FLAGS_POS_PAGE_COMPRESSION	(FSP_FLAGS_POS_RESERVED \
					+ FSP_FLAGS_WIDTH_RESERVED)

/** Bit mask of the POST_ANTELOPE field */
#define FSP_FLAGS_MASK_POST_ANTELOPE				\
		((~(~0U << FSP_FLAGS_WIDTH_POST_ANTELOPE))	\
		<< FSP_FLAGS_POS_POST_ANTELOPE)
/** Bit mask of the ZIP_SSIZE field */
#define FSP_FLAGS_MASK_ZIP_SSIZE				\
		((~(~0U << FSP_FLAGS_WIDTH_ZIP_SSIZE))		\
		<< FSP_FLAGS_POS_ZIP_SSIZE)
/** Bit mask of the ATOMIC_BLOBS field */
#define FSP_FLAGS_MASK_ATOMIC_BLOBS				\
		((~(~0U << FSP_FLAGS_WIDTH_ATOMIC_BLOBS))	\
		<< FSP_FLAGS_POS_ATOMIC_BLOBS)
/** Bit mask of the PAGE_SSIZE field */
#define FSP_FLAGS_MASK_PAGE_SSIZE				\
		((~(~0U << FSP_FLAGS_WIDTH_PAGE_SSIZE))		\
		<< FSP_FLAGS_POS_PAGE_SSIZE)
/** Bit mask of the RESERVED1 field */
#define FSP_FLAGS_MASK_RESERVED					\
		((~(~0U << FSP_FLAGS_WIDTH_RESERVED))		\
		<< FSP_FLAGS_POS_RESERVED)
/** Bit mask of the PAGE_COMPRESSION field */
#define FSP_FLAGS_MASK_PAGE_COMPRESSION				\
		((~(~0U << FSP_FLAGS_WIDTH_PAGE_COMPRESSION))	\
		<< FSP_FLAGS_POS_PAGE_COMPRESSION)

/** Bit mask of the in-memory COMPRESSION_LEVEL field */
#define FSP_FLAGS_MASK_MEM_COMPRESSION_LEVEL			\
		(15U << FSP_FLAGS_MEM_COMPRESSION_LEVEL)

/** Return the value of the POST_ANTELOPE field */
#define FSP_FLAGS_GET_POST_ANTELOPE(flags)			\
		((flags & FSP_FLAGS_MASK_POST_ANTELOPE)		\
		>> FSP_FLAGS_POS_POST_ANTELOPE)
/** Return the value of the ZIP_SSIZE field */
#define FSP_FLAGS_GET_ZIP_SSIZE(flags)				\
		((flags & FSP_FLAGS_MASK_ZIP_SSIZE)		\
		>> FSP_FLAGS_POS_ZIP_SSIZE)
/** Return the value of the ATOMIC_BLOBS field */
#define FSP_FLAGS_HAS_ATOMIC_BLOBS(flags)			\
		((flags & FSP_FLAGS_MASK_ATOMIC_BLOBS)		\
		>> FSP_FLAGS_POS_ATOMIC_BLOBS)
/** Return the value of the PAGE_SSIZE field */
#define FSP_FLAGS_GET_PAGE_SSIZE(flags)				\
		((flags & FSP_FLAGS_MASK_PAGE_SSIZE)		\
		>> FSP_FLAGS_POS_PAGE_SSIZE)
/** @return the RESERVED flags */
#define FSP_FLAGS_GET_RESERVED(flags)				\
		((flags & FSP_FLAGS_MASK_RESERVED)		\
		>> FSP_FLAGS_POS_RESERVED)
/** @return the PAGE_COMPRESSION flag */
#define FSP_FLAGS_HAS_PAGE_COMPRESSION(flags)			\
		((flags & FSP_FLAGS_MASK_PAGE_COMPRESSION)	\
		>> FSP_FLAGS_POS_PAGE_COMPRESSION)

/** Return the contents of the UNUSED bits */
#define FSP_FLAGS_GET_UNUSED(flags)				\
		(flags >> FSP_FLAGS_POS_UNUSED)

/** @return the value of the DATA_DIR field */
#define FSP_FLAGS_HAS_DATA_DIR(flags)				\
	(flags & 1U << FSP_FLAGS_MEM_DATA_DIR)
/** @return the COMPRESSION_LEVEL field */
#define FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL(flags)		\
	((flags & FSP_FLAGS_MASK_MEM_COMPRESSION_LEVEL)		\
	 >> FSP_FLAGS_MEM_COMPRESSION_LEVEL)

/* @} */

/** Validate the tablespace flags, which are stored in the
tablespace header at offset FSP_SPACE_FLAGS.
@param[in]	flags	the contents of FSP_SPACE_FLAGS
@param[in]	is_ibd	whether this is an .ibd file (not system tablespace)
@return	whether the flags are correct (not in the buggy 10.1) format */
MY_ATTRIBUTE((warn_unused_result, const))
UNIV_INLINE
bool
fsp_flags_is_valid(ulint flags, bool is_ibd)
{
	DBUG_EXECUTE_IF("fsp_flags_is_valid_failure",
			return(false););
	if (flags == 0) {
		return(true);
	}
	if (flags & ~FSP_FLAGS_MASK) {
		return(false);
	}
	if ((flags & (FSP_FLAGS_MASK_POST_ANTELOPE | FSP_FLAGS_MASK_ATOMIC_BLOBS))
	    == FSP_FLAGS_MASK_ATOMIC_BLOBS) {
		/* If the "atomic blobs" flag (indicating
		ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED) flag
		is set, then the "post Antelope" (ROW_FORMAT!=REDUNDANT) flag
		must also be set. */
		return(false);
	}
	/* Bits 10..14 should be 0b0000d where d is the DATA_DIR flag
	of MySQL 5.6 and MariaDB 10.0, which we ignore.
	In the buggy FSP_SPACE_FLAGS written by MariaDB 10.1.0 to 10.1.20,
	bits 10..14 would be nonzero 0bsssaa where sss is
	nonzero PAGE_SSIZE (3, 4, 6, or 7)
	and aa is ATOMIC_WRITES (not 0b11). */
	if (FSP_FLAGS_GET_RESERVED(flags) & ~1U) {
		return(false);
	}

	const ulint	ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
	if (ssize == 1 || ssize == 2 || ssize == 5 || ssize & 8) {
		/* the page_size is not between 4k and 64k;
		16k should be encoded as 0, not 5 */
		return(false);
	}
	const ulint	zssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
	if (zssize == 0) {
		/* not ROW_FORMAT=COMPRESSED */
	} else if (zssize > (ssize ? ssize : 5)) {
		/* invalid KEY_BLOCK_SIZE */
		return(false);
	} else if (~flags & (FSP_FLAGS_MASK_POST_ANTELOPE
			     | FSP_FLAGS_MASK_ATOMIC_BLOBS)) {
		/* both these flags should be set for
		ROW_FORMAT=COMPRESSED */
		return(false);
	}

	/* The flags do look valid. But, avoid misinterpreting
	buggy MariaDB 10.1 format flags for
	PAGE_COMPRESSED=1 PAGE_COMPRESSION_LEVEL={0,2,3}
	as valid-looking PAGE_SSIZE if this is known to be
	an .ibd file and we are using the default innodb_page_size=16k. */
	return(ssize == 0 || !is_ibd || srv_page_size != UNIV_PAGE_SIZE_ORIG);
}

#endif /* fsp0types_h */
