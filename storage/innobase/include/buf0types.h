/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All rights reserved.

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
@file include/buf0types.h
The database buffer pool global types for the directory

Created 11/17/1995 Heikki Tuuri
*******************************************************/

#ifndef buf0types_h
#define buf0types_h

#include "os0event.h"
#include "ut0ut.h"

/** Buffer page (uncompressed or compressed) */
class buf_page_t;
/** Buffer block for which an uncompressed page exists */
struct buf_block_t;
/** Buffer pool chunk comprising buf_block_t */
struct buf_chunk_t;
/** Buffer pool comprising buf_chunk_t */
struct buf_pool_t;
/** Buffer pool statistics struct */
struct buf_pool_stat_t;
/** Buffer pool buddy statistics struct */
struct buf_buddy_stat_t;
/** Doublewrite memory struct */
struct buf_dblwr_t;
/** Flush observer for bulk create index */
class FlushObserver;

/** A buffer frame. @see page_t */
typedef	byte	buf_frame_t;

/** Flags for flush types */
enum buf_flush_t {
	BUF_FLUSH_LRU = 0,		/*!< flush via the LRU list */
	BUF_FLUSH_LIST,			/*!< flush via the flush list
					of dirty blocks */
	BUF_FLUSH_SINGLE_PAGE,		/*!< flush via the LRU list
					but only a single page */
	BUF_FLUSH_N_TYPES		/*!< index of last element + 1  */
};

/** Flags for io_fix types */
enum buf_io_fix {
	BUF_IO_NONE = 0,		/**< no pending I/O */
	BUF_IO_READ,			/**< read pending */
	BUF_IO_WRITE,			/**< write pending */
	BUF_IO_PIN			/**< disallow relocation of
					block and its removal of from
					the flush_list */
};

/** Alternatives for srv_checksum_algorithm, which can be changed by
setting innodb_checksum_algorithm */
enum srv_checksum_algorithm_t {
	SRV_CHECKSUM_ALGORITHM_CRC32,		/*!< Write crc32, allow crc32,
						innodb or none when reading */
	SRV_CHECKSUM_ALGORITHM_STRICT_CRC32,	/*!< Write crc32, allow crc32
						when reading */
	SRV_CHECKSUM_ALGORITHM_INNODB,		/*!< Write innodb, allow crc32,
						innodb or none when reading */
	SRV_CHECKSUM_ALGORITHM_STRICT_INNODB,	/*!< Write innodb, allow
						innodb when reading */
	SRV_CHECKSUM_ALGORITHM_NONE,		/*!< Write none, allow crc32,
						innodb or none when reading */
	SRV_CHECKSUM_ALGORITHM_STRICT_NONE	/*!< Write none, allow none
						when reading */
};

inline
bool
is_checksum_strict(srv_checksum_algorithm_t algo)
{
	return(algo == SRV_CHECKSUM_ALGORITHM_STRICT_CRC32
	       || algo == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB
	       || algo == SRV_CHECKSUM_ALGORITHM_STRICT_NONE);
}

inline
bool
is_checksum_strict(ulint algo)
{
	return(algo == SRV_CHECKSUM_ALGORITHM_STRICT_CRC32
	       || algo == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB
	       || algo == SRV_CHECKSUM_ALGORITHM_STRICT_NONE);
}

/** Parameters of binary buddy system for compressed pages (buf0buddy.h) */
/* @{ */
/** Zip shift value for the smallest page size */
#define BUF_BUDDY_LOW_SHIFT	UNIV_ZIP_SIZE_SHIFT_MIN

/** Smallest buddy page size */
#define BUF_BUDDY_LOW		(1U << BUF_BUDDY_LOW_SHIFT)

/** Actual number of buddy sizes based on current page size */
#define BUF_BUDDY_SIZES		(UNIV_PAGE_SIZE_SHIFT - BUF_BUDDY_LOW_SHIFT)

/** Maximum number of buddy sizes based on the max page size */
#define BUF_BUDDY_SIZES_MAX	(UNIV_PAGE_SIZE_SHIFT_MAX	\
				- BUF_BUDDY_LOW_SHIFT)

/** twice the maximum block size of the buddy system;
the underlying memory is aligned by this amount:
this must be equal to UNIV_PAGE_SIZE */
#define BUF_BUDDY_HIGH	(BUF_BUDDY_LOW << BUF_BUDDY_SIZES)
/* @} */

/** Page identifier. */
class page_id_t {
public:

	/** Constructor from (space, page_no).
	@param[in]	space	tablespace id
	@param[in]	page_no	page number */
	page_id_t(ulint space, ulint page_no)
		: m_space(uint32_t(space)), m_page_no(uint32(page_no))
	{
		ut_ad(space <= 0xFFFFFFFFU);
		ut_ad(page_no <= 0xFFFFFFFFU);
	}

	bool operator==(const page_id_t& rhs) const
	{
		return m_space == rhs.m_space && m_page_no == rhs.m_page_no;
	}
	bool operator!=(const page_id_t& rhs) const { return !(*this == rhs); }

	bool operator<(const page_id_t& rhs) const
	{
		if (m_space == rhs.m_space) {
			return m_page_no < rhs.m_page_no;
		}

		return m_space < rhs.m_space;
	}

	/** Retrieve the tablespace id.
	@return tablespace id */
	uint32_t space() const { return m_space; }

	/** Retrieve the page number.
	@return page number */
	uint32_t page_no() const { return m_page_no; }

	/** Retrieve the fold value.
	@return fold value */
	ulint fold() const { return (m_space << 20) + m_space + m_page_no; }

	/** Reset the page number only.
	@param[in]	page_no	page number */
	void set_page_no(ulint page_no)
	{
		m_page_no = uint32_t(page_no);

		ut_ad(page_no <= 0xFFFFFFFFU);
	}

	/** Set the FIL_NULL for the space and page_no */
	void set_corrupt_id()
	{
		m_space = m_page_no = ULINT32_UNDEFINED;
	}

private:

	/** Tablespace id. */
	uint32_t	m_space;

	/** Page number. */
	uint32_t	m_page_no;

	/** Declare the overloaded global operator<< as a friend of this
	class. Refer to the global declaration for further details.  Print
	the given page_id_t object.
	@param[in,out]	out	the output stream
	@param[in]	page_id	the page_id_t object to be printed
	@return the output stream */
        friend
        std::ostream&
        operator<<(
                std::ostream&           out,
                const page_id_t        page_id);
};

#ifndef UNIV_INNOCHECKSUM

#include "ut0mutex.h"
#include "sync0rw.h"

typedef ib_bpmutex_t BPageMutex;
typedef ib_mutex_t BufPoolMutex;
typedef ib_mutex_t FlushListMutex;
typedef BPageMutex BufPoolZipMutex;
typedef rw_lock_t BPageLock;
#endif /* !UNIV_INNOCHECKSUM */

#endif /* buf0types.h */
