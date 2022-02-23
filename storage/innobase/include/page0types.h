/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2022, MariaDB Corporation.

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
@file include/page0types.h
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

#ifndef page0types_h
#define page0types_h

#include "dict0types.h"
#include "mtr0types.h"
#include "rem0types.h"
#include "ut0new.h"

#include <map>

/** Eliminates a name collision on HP-UX */
#define page_t	   ib_page_t
/** Type of the index page */
typedef	byte		page_t;
#ifndef UNIV_INNOCHECKSUM
/** Index page cursor */
struct page_cur_t;
/** Buffer pool block */
struct buf_block_t;

/** Compressed index page */
typedef byte		page_zip_t;

/* The following definitions would better belong to page0zip.h,
but we cannot include page0zip.h from rem0rec.ic, because
page0*.h includes rem0rec.h and may include rem0rec.ic. */

/** Number of bits needed for representing different compressed page sizes */
#define PAGE_ZIP_SSIZE_BITS 3

/** Maximum compressed page shift size */
#define PAGE_ZIP_SSIZE_MAX	\
	(UNIV_ZIP_SIZE_SHIFT_MAX - UNIV_ZIP_SIZE_SHIFT_MIN + 1)

/* Make sure there are enough bits available to store the maximum zip
ssize, which is the number of shifts from 512. */
#if PAGE_ZIP_SSIZE_MAX >= (1 << PAGE_ZIP_SSIZE_BITS)
# error "PAGE_ZIP_SSIZE_MAX >= (1 << PAGE_ZIP_SSIZE_BITS)"
#endif

/* Page cursor search modes; the values must be in this order! */
enum page_cur_mode_t {
	PAGE_CUR_UNSUPP	= 0,
	PAGE_CUR_G	= 1,
	PAGE_CUR_GE	= 2,
	PAGE_CUR_L	= 3,
	PAGE_CUR_LE	= 4,

/*      PAGE_CUR_LE_OR_EXTENDS = 5,*/ /* This is a search mode used in
				 "column LIKE 'abc%' ORDER BY column DESC";
				 we have to find strings which are <= 'abc' or
				 which extend it */

/* These search mode is for search R-tree index. */
	PAGE_CUR_CONTAIN		= 7,
	PAGE_CUR_INTERSECT		= 8,
	PAGE_CUR_WITHIN			= 9,
	PAGE_CUR_DISJOINT		= 10,
	PAGE_CUR_MBR_EQUAL		= 11,
	PAGE_CUR_RTREE_INSERT		= 12,
	PAGE_CUR_RTREE_LOCATE		= 13,
	PAGE_CUR_RTREE_GET_FATHER	= 14
};

class buf_pool_t;
class buf_page_t;

/** Compressed page descriptor */
struct page_zip_des_t
{
	page_zip_t*	data;		/*!< compressed page data */

	uint32_t	m_end:16;	/*!< end offset of modification log */
	uint32_t	m_nonempty:1;	/*!< TRUE if the modification log
					is not empty */
	uint32_t	n_blobs:12;	/*!< number of externally stored
					columns on the page; the maximum
					is 744 on a 16 KiB page */
	uint32_t	ssize:PAGE_ZIP_SSIZE_BITS;
					/*!< 0 or compressed page shift size;
					the size in bytes is
					(UNIV_ZIP_SIZE_MIN >> 1) << ssize. */
#ifdef UNIV_DEBUG
	uint16_t	m_start;	/*!< start offset of modification log */
	bool		m_external;	/*!< Allocated externally, not from the
					buffer pool */
#endif /* UNIV_DEBUG */

	void clear() {
		/* Clear everything except the member "fix". */
		memset((void*) this, 0,
		       reinterpret_cast<char*>(&fix)
		       - reinterpret_cast<char*>(this));
	}

	page_zip_des_t() = default;
	page_zip_des_t(const page_zip_des_t&) = default;

	/* Initialize everything except the member "fix". */
	page_zip_des_t(const page_zip_des_t& old, bool) {
		memcpy((void*) this, (void*) &old,
		       reinterpret_cast<char*>(&fix)
		       - reinterpret_cast<char*>(this));
	}

private:
	friend buf_pool_t;
	friend buf_page_t;
	/** fix count and state used in buf_page_t */
	Atomic_relaxed<uint32_t> fix;
};

/** Compression statistics for a given page size */
struct page_zip_stat_t {
	/** Number of page compressions */
	ulint		compressed;
	/** Number of successful page compressions */
	ulint		compressed_ok;
	/** Number of page decompressions */
	ulint		decompressed;
	/** Duration of page compressions in microseconds */
	ib_uint64_t	compressed_usec;
	/** Duration of page decompressions in microseconds */
	ib_uint64_t	decompressed_usec;
	page_zip_stat_t() :
		/* Initialize members to 0 so that when we do
		stlmap[key].compressed++ and element with "key" does not
		exist it gets inserted with zeroed members. */
		compressed(0),
		compressed_ok(0),
		decompressed(0),
		compressed_usec(0),
		decompressed_usec(0)
	{ }
};

/** Compression statistics types */
typedef std::map<
	index_id_t,
	page_zip_stat_t,
	std::less<index_id_t>,
	ut_allocator<std::pair<const index_id_t, page_zip_stat_t> > >
	page_zip_stat_per_index_t;

/** Statistics on compression, indexed by page_zip_des_t::ssize - 1 */
extern page_zip_stat_t			page_zip_stat[PAGE_ZIP_SSIZE_MAX];
/** Statistics on compression, indexed by dict_index_t::id */
extern page_zip_stat_per_index_t	page_zip_stat_per_index;

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
	MY_ATTRIBUTE((nonnull));
#endif /* !UNIV_INNOCHECKSUM */
#endif
