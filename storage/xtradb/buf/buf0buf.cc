/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2013, 2016, MariaDB Corporation. All Rights Reserved.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0buf.cc
The database buffer buf_pool

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0buf.h"

#ifdef UNIV_NONINL
#include "buf0buf.ic"
#endif

#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#ifndef UNIV_HOTBACKUP
#include "buf0buddy.h"
#include "lock0lock.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "trx0undo.h"
#include "log0log.h"
#endif /* !UNIV_HOTBACKUP */
#include "srv0srv.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "page0zip.h"
#include "srv0mon.h"
#include "buf0checksum.h"
#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
#endif // HAVE_LIBNUMA
#include "trx0trx.h"
#include "srv0start.h"
#include "ut0byte.h"
#include "fil0pagecompress.h"
#include "ha_prototypes.h"

/* Enable this for checksum error messages. */
//#ifdef UNIV_DEBUG
//#define UNIV_DEBUG_LEVEL2 1
//#endif

/* prototypes for new functions added to ha_innodb.cc */
trx_t* innobase_get_trx();

/********************************************************************//**
Check if page is maybe compressed, encrypted or both when we encounter
corrupted page. Note that we can't be 100% sure if page is corrupted
or decrypt/decompress just failed.
*/
static
ibool
buf_page_check_corrupt(
/*===================*/
	buf_page_t*	bpage);	/*!< in/out: buffer page read from
					disk */

static inline
void
_increment_page_get_statistics(buf_block_t* block, trx_t* trx)
{
	ulint           block_hash;
	ulint           block_hash_byte;
	byte            block_hash_offset;

	ut_ad(block);
	ut_ad(trx && trx->take_stats);

	if (!trx->distinct_page_access_hash) {
		trx->distinct_page_access_hash
			= static_cast<byte *>(mem_alloc(DPAH_SIZE));
		memset(trx->distinct_page_access_hash, 0, DPAH_SIZE);
	}

	block_hash = ut_hash_ulint((block->page.space << 20) + block->page.space +
					block->page.offset, DPAH_SIZE << 3);
	block_hash_byte = block_hash >> 3;
	block_hash_offset = (byte) block_hash & 0x07;
	if (block_hash_byte >= DPAH_SIZE)
		fprintf(stderr, "!!! block_hash_byte = %lu  block_hash_offset = %d !!!\n", block_hash_byte, block_hash_offset);
	if (block_hash_offset > 7)
		fprintf(stderr, "!!! block_hash_byte = %lu  block_hash_offset = %d !!!\n", block_hash_byte, block_hash_offset);
	if ((trx->distinct_page_access_hash[block_hash_byte] & ((byte) 0x01 << block_hash_offset)) == 0)
		trx->distinct_page_access++;
	trx->distinct_page_access_hash[block_hash_byte] |= (byte) 0x01 << block_hash_offset;
	return;
}

#ifdef HAVE_LZO
#include "lzo/lzo1x.h"
#endif

/*
		IMPLEMENTATION OF THE BUFFER POOL
		=================================

Performance improvement:
------------------------
Thread scheduling in NT may be so slow that the OS wait mechanism should
not be used even in waiting for disk reads to complete.
Rather, we should put waiting query threads to the queue of
waiting jobs, and let the OS thread do something useful while the i/o
is processed. In this way we could remove most OS thread switches in
an i/o-intensive benchmark like TPC-C.

A possibility is to put a user space thread library between the database
and NT. User space thread libraries might be very fast.

SQL Server 7.0 can be configured to use 'fibers' which are lightweight
threads in NT. These should be studied.

		Buffer frames and blocks
		------------------------
Following the terminology of Gray and Reuter, we call the memory
blocks where file pages are loaded buffer frames. For each buffer
frame there is a control block, or shortly, a block, in the buffer
control array. The control info which does not need to be stored
in the file along with the file page, resides in the control block.

		Buffer pool struct
		------------------
The buffer buf_pool contains several mutexes which protect all the
control data structures of the buf_pool. The content of a buffer frame is
protected by a separate read-write lock in its control block, though.

		Control blocks
		--------------

The control block contains, for instance, the bufferfix count
which is incremented when a thread wants a file page to be fixed
in a buffer frame. The bufferfix operation does not lock the
contents of the frame, however. For this purpose, the control
block contains a read-write lock.

The buffer frames have to be aligned so that the start memory
address of a frame is divisible by the universal page size, which
is a power of two.

We intend to make the buffer buf_pool size on-line reconfigurable,
that is, the buf_pool size can be changed without closing the database.
Then the database administarator may adjust it to be bigger
at night, for example. The control block array must
contain enough control blocks for the maximum buffer buf_pool size
which is used in the particular database.
If the buf_pool size is cut, we exploit the virtual memory mechanism of
the OS, and just refrain from using frames at high addresses. Then the OS
can swap them to disk.

The control blocks containing file pages are put to a hash table
according to the file address of the page.
We could speed up the access to an individual page by using
"pointer swizzling": we could replace the page references on
non-leaf index pages by direct pointers to the page, if it exists
in the buf_pool. We could make a separate hash table where we could
chain all the page references in non-leaf pages residing in the buf_pool,
using the page reference as the hash key,
and at the time of reading of a page update the pointers accordingly.
Drawbacks of this solution are added complexity and,
possibly, extra space required on non-leaf pages for memory pointers.
A simpler solution is just to speed up the hash table mechanism
in the database, using tables whose size is a power of 2.

		Lists of blocks
		---------------

There are several lists of control blocks.

The free list (buf_pool->free) contains blocks which are currently not
used.

The common LRU list contains all the blocks holding a file page
except those for which the bufferfix count is non-zero.
The pages are in the LRU list roughly in the order of the last
access to the page, so that the oldest pages are at the end of the
list. We also keep a pointer to near the end of the LRU list,
which we can use when we want to artificially age a page in the
buf_pool. This is used if we know that some page is not needed
again for some time: we insert the block right after the pointer,
causing it to be replaced sooner than would normally be the case.
Currently this aging mechanism is used for read-ahead mechanism
of pages, and it can also be used when there is a scan of a full
table which cannot fit in the memory. Putting the pages near the
end of the LRU list, we make sure that most of the buf_pool stays
in the main memory, undisturbed.

The unzip_LRU list contains a subset of the common LRU list.  The
blocks on the unzip_LRU list hold a compressed file page and the
corresponding uncompressed page frame.  A block is in unzip_LRU if and
only if the predicate buf_page_belongs_to_unzip_LRU(&block->page)
holds.  The blocks in unzip_LRU will be in same order as they are in
the common LRU list.  That is, each manipulation of the common LRU
list will result in the same manipulation of the unzip_LRU list.

The chain of modified blocks (buf_pool->flush_list) contains the blocks
holding file pages that have been modified in the memory
but not written to disk yet. The block with the oldest modification
which has not yet been written to disk is at the end of the chain.
The access to this list is protected by buf_pool->flush_list_mutex.

The chain of unmodified compressed blocks (buf_pool->zip_clean)
contains the control blocks (buf_page_t) of those compressed pages
that are not in buf_pool->flush_list and for which no uncompressed
page has been allocated in the buffer pool.  The control blocks for
uncompressed pages are accessible via buf_block_t objects that are
reachable via buf_pool->chunks[].

The chains of free memory blocks (buf_pool->zip_free[]) are used by
the buddy allocator (buf0buddy.cc) to keep track of currently unused
memory blocks of size sizeof(buf_page_t)..UNIV_PAGE_SIZE / 2.  These
blocks are inside the UNIV_PAGE_SIZE-sized memory blocks of type
BUF_BLOCK_MEMORY that the buddy allocator requests from the buffer
pool.  The buddy allocator is solely used for allocating control
blocks for compressed pages (buf_page_t) and compressed page frames.

		Loading a file page
		-------------------

First, a victim block for replacement has to be found in the
buf_pool. It is taken from the free list or searched for from the
end of the LRU-list. An exclusive lock is reserved for the frame,
the io_fix field is set in the block fixing the block in buf_pool,
and the io-operation for loading the page is queued. The io-handler thread
releases the X-lock on the frame and resets the io_fix field
when the io operation completes.

A thread may request the above operation using the function
buf_page_get(). It may then continue to request a lock on the frame.
The lock is granted when the io-handler releases the x-lock.

		Read-ahead
		----------

The read-ahead mechanism is intended to be intelligent and
isolated from the semantically higher levels of the database
index management. From the higher level we only need the
information if a file page has a natural successor or
predecessor page. On the leaf level of a B-tree index,
these are the next and previous pages in the natural
order of the pages.

Let us first explain the read-ahead mechanism when the leafs
of a B-tree are scanned in an ascending or descending order.
When a read page is the first time referenced in the buf_pool,
the buffer manager checks if it is at the border of a so-called
linear read-ahead area. The tablespace is divided into these
areas of size 64 blocks, for example. So if the page is at the
border of such an area, the read-ahead mechanism checks if
all the other blocks in the area have been accessed in an
ascending or descending order. If this is the case, the system
looks at the natural successor or predecessor of the page,
checks if that is at the border of another area, and in this case
issues read-requests for all the pages in that area. Maybe
we could relax the condition that all the pages in the area
have to be accessed: if data is deleted from a table, there may
appear holes of unused pages in the area.

A different read-ahead mechanism is used when there appears
to be a random access pattern to a file.
If a new page is referenced in the buf_pool, and several pages
of its random access area (for instance, 32 consecutive pages
in a tablespace) have recently been referenced, we may predict
that the whole area may be needed in the near future, and issue
the read requests for the whole area.
*/

#ifndef UNIV_HOTBACKUP
/** Value in microseconds */
static const int WAIT_FOR_READ	= 100;
/** Number of attemtps made to read in a page in the buffer pool */
static const ulint BUF_PAGE_READ_MAX_RETRIES = 100;

/** The buffer pools of the database */
UNIV_INTERN buf_pool_t*	buf_pool_ptr;

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
static ulint	buf_dbg_counter	= 0; /*!< This is used to insert validation
					operations in execution in the
					debug version */
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_DEBUG
/** If this is set TRUE, the program prints info whenever
read-ahead or flush occurs */
UNIV_INTERN ibool		buf_debug_prints = FALSE;
#endif /* UNIV_DEBUG */

#ifdef UNIV_PFS_RWLOCK
/* Keys to register buffer block related rwlocks and mutexes with
performance schema */
UNIV_INTERN mysql_pfs_key_t	buf_block_lock_key;
# ifdef UNIV_SYNC_DEBUG
UNIV_INTERN mysql_pfs_key_t	buf_block_debug_latch_key;
# endif /* UNIV_SYNC_DEBUG */
#endif /* UNIV_PFS_RWLOCK */

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t	buffer_block_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_zip_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_flush_state_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_LRU_list_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_free_list_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_zip_free_mutex_key;
UNIV_INTERN mysql_pfs_key_t	buf_pool_zip_hash_mutex_key;
UNIV_INTERN mysql_pfs_key_t	flush_list_mutex_key;
#endif /* UNIV_PFS_MUTEX */

#if defined UNIV_PFS_MUTEX || defined UNIV_PFS_RWLOCK
# ifndef PFS_SKIP_BUFFER_MUTEX_RWLOCK

/* Buffer block mutexes and rwlocks can be registered
in one group rather than individually. If PFS_GROUP_BUFFER_SYNC
is defined, register buffer block mutex and rwlock
in one group after their initialization. */
#  define PFS_GROUP_BUFFER_SYNC

/* This define caps the number of mutexes/rwlocks can
be registered with performance schema. Developers can
modify this define if necessary. Please note, this would
be effective only if PFS_GROUP_BUFFER_SYNC is defined. */
#  define PFS_MAX_BUFFER_MUTEX_LOCK_REGISTER	ULINT_MAX

# endif /* !PFS_SKIP_BUFFER_MUTEX_RWLOCK */
#endif /* UNIV_PFS_MUTEX || UNIV_PFS_RWLOCK */

/** Macro to determine whether the read of write counter is used depending
on the io_type */
#define MONITOR_RW_COUNTER(io_type, counter)		\
	((io_type == BUF_IO_READ)			\
	 ? (counter##_READ)				\
	 : (counter##_WRITTEN))

/********************************************************************//**
Gets the smallest oldest_modification lsn for any page in the pool. Returns
zero if all modified pages have been flushed to disk.
@return oldest modification in pool, zero if none */
UNIV_INTERN
lsn_t
buf_pool_get_oldest_modification(void)
/*==================================*/
{
	ulint		i;
	buf_page_t*	bpage;
	lsn_t		lsn = 0;
	lsn_t		oldest_lsn = 0;

	/* When we traverse all the flush lists we don't want another
	thread to add a dirty page to any flush list. */
	if (srv_buf_pool_instances > 1)
	log_flush_order_mutex_enter();

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

		bpage = UT_LIST_GET_LAST(buf_pool->flush_list);

		if (bpage != NULL) {
			ut_ad(bpage->in_flush_list);
			lsn = bpage->oldest_modification;
		}

		buf_flush_list_mutex_exit(buf_pool);

		if (!oldest_lsn || oldest_lsn > lsn) {
			oldest_lsn = lsn;
		}
	}

	if (srv_buf_pool_instances > 1)
	log_flush_order_mutex_exit();

	/* The returned answer may be out of date: the flush_list can
	change after the mutex has been released. */

	return(oldest_lsn);
}

/********************************************************************//**
Gets the smallest oldest_modification lsn for any page in the pool. Returns
zero if all modified pages have been flushed to disk.
@return oldest modification in pool, zero if none */
UNIV_INTERN
lsn_t
buf_pool_get_oldest_modification_peek(void)
/*=======================================*/
{
	ulint		i;
	buf_page_t*	bpage;
	lsn_t		lsn = 0;
	lsn_t		oldest_lsn = 0;

	/* Dirsty read to buffer pool array */
	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

		bpage = UT_LIST_GET_LAST(buf_pool->flush_list);

		if (bpage != NULL) {
			ut_ad(bpage->in_flush_list);
			lsn = bpage->oldest_modification;
		}

		buf_flush_list_mutex_exit(buf_pool);

		if (!oldest_lsn || oldest_lsn > lsn) {
			oldest_lsn = lsn;
		}
	}

	/* The returned answer may be out of date: the flush_list can
	change after the mutex has been released. */

	return(oldest_lsn);
}

/********************************************************************//**
Get total buffer pool statistics. */
UNIV_INTERN
void
buf_get_total_list_len(
/*===================*/
	ulint*		LRU_len,	/*!< out: length of all LRU lists */
	ulint*		free_len,	/*!< out: length of all free lists */
	ulint*		flush_list_len)	/*!< out: length of all flush lists */
{
	ulint		i;

	*LRU_len = 0;
	*free_len = 0;
	*flush_list_len = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		*LRU_len += UT_LIST_GET_LEN(buf_pool->LRU);
		*free_len += UT_LIST_GET_LEN(buf_pool->free);
		*flush_list_len += UT_LIST_GET_LEN(buf_pool->flush_list);
	}
}

/********************************************************************//**
Get total list size in bytes from all buffer pools. */
UNIV_INTERN
void
buf_get_total_list_size_in_bytes(
/*=============================*/
	buf_pools_list_size_t*	buf_pools_list_size)	/*!< out: list sizes
							in all buffer pools */
{
	ut_ad(buf_pools_list_size);
	memset(buf_pools_list_size, 0, sizeof(*buf_pools_list_size));

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		/* We don't need mutex protection since this is
		for statistics purpose */
		buf_pools_list_size->LRU_bytes += buf_pool->stat.LRU_bytes;
		buf_pools_list_size->unzip_LRU_bytes +=
			UT_LIST_GET_LEN(buf_pool->unzip_LRU) * UNIV_PAGE_SIZE;
		buf_pools_list_size->flush_list_bytes +=
			buf_pool->stat.flush_list_bytes;
	}
}

/********************************************************************//**
Get total buffer pool statistics. */
UNIV_INTERN
void
buf_get_total_stat(
/*===============*/
	buf_pool_stat_t*	tot_stat)	/*!< out: buffer pool stats */
{
	ulint			i;

	memset(tot_stat, 0, sizeof(*tot_stat));

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_stat_t*buf_stat;
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_stat = &buf_pool->stat;
		tot_stat->n_page_gets += buf_stat->n_page_gets;
		tot_stat->n_pages_read += buf_stat->n_pages_read;
		tot_stat->n_pages_written += buf_stat->n_pages_written;
		tot_stat->n_pages_created += buf_stat->n_pages_created;
		tot_stat->n_ra_pages_read_rnd += buf_stat->n_ra_pages_read_rnd;
		tot_stat->n_ra_pages_read += buf_stat->n_ra_pages_read;
		tot_stat->n_ra_pages_evicted += buf_stat->n_ra_pages_evicted;
		tot_stat->n_pages_made_young += buf_stat->n_pages_made_young;

		tot_stat->n_pages_not_made_young +=
			buf_stat->n_pages_not_made_young;
	}
}

/********************************************************************//**
Allocates a buffer block.
@return own: the allocated block, in state BUF_BLOCK_MEMORY */
UNIV_INTERN
buf_block_t*
buf_block_alloc(
/*============*/
	buf_pool_t*	buf_pool)	/*!< in/out: buffer pool instance,
					or NULL for round-robin selection
					of the buffer pool */
{
	buf_block_t*	block;
	ulint		index;
	static ulint	buf_pool_index;

	if (buf_pool == NULL) {
		/* We are allocating memory from any buffer pool, ensure
		we spread the grace on all buffer pool instances. */
		index = buf_pool_index++ % srv_buf_pool_instances;
		buf_pool = buf_pool_from_array(index);
	}

	block = buf_LRU_get_free_block(buf_pool);

	buf_block_set_state(block, BUF_BLOCK_MEMORY);

	return(block);
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************************//**
Checks if a page is all zeroes.
@return	TRUE if the page is all zeroes */
bool
buf_page_is_zeroes(
/*===============*/
	const byte*	read_buf,	/*!< in: a database page */
	const ulint	zip_size)	/*!< in: size of compressed page;
					0 for uncompressed pages */
{
	const ulint page_size = zip_size ? zip_size : UNIV_PAGE_SIZE;

	for (ulint i = 0; i < page_size; i++) {
		if (read_buf[i] != 0) {
			return(false);
		}
	}
	return(true);
}

/** Checks if the page is in crc32 checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in crc32 checksum format */
UNIV_INLINE
bool
buf_page_is_checksum_valid_crc32(
	const byte*	read_buf,
	ulint		checksum_field1,
	ulint		checksum_field2)
{
	ib_uint32_t	crc32 = buf_calc_page_crc32(read_buf);

#ifdef UNIV_DEBUG_LEVEL2
	if (!(checksum_field1 == crc32 && checksum_field2 == crc32)) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Page checksum crc32 not valid field1 %lu field2 %lu crc32 %lu.",
			checksum_field1, checksum_field2, (ulint)crc32);
	}
#endif

	return(checksum_field1 == crc32 && checksum_field2 == crc32);
}

/** Checks if the page is in innodb checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in innodb checksum format */
UNIV_INLINE
bool
buf_page_is_checksum_valid_innodb(
	const byte*	read_buf,
	ulint		checksum_field1,
	ulint		checksum_field2)
{
	/* There are 2 valid formulas for
	checksum_field2 (old checksum field) which algo=innodb could have
	written to the page:

	1. Very old versions of InnoDB only stored 8 byte lsn to the
	start and the end of the page.

	2. Newer InnoDB versions store the old formula checksum
	(buf_calc_page_old_checksum()). */

	if (checksum_field2 != mach_read_from_4(read_buf + FIL_PAGE_LSN)
	    && checksum_field2 != buf_calc_page_old_checksum(read_buf)) {
#ifdef UNIV_DEBUG_LEVEL2
		ib_logf(IB_LOG_LEVEL_INFO,
			"Page checksum innodb not valid field1 %lu field2 %lu crc32 %lu lsn %lu.",
			checksum_field1, checksum_field2, buf_calc_page_old_checksum(read_buf),
			mach_read_from_4(read_buf + FIL_PAGE_LSN)
		);
#endif
		return(false);
	}

	/* old field is fine, check the new field */

	/* InnoDB versions < 4.0.14 and < 4.1.1 stored the space id
	(always equal to 0), to FIL_PAGE_SPACE_OR_CHKSUM */

	if (checksum_field1 != 0
	    && checksum_field1 != buf_calc_page_new_checksum(read_buf)) {
#ifdef UNIV_DEBUG_LEVEL2
		ib_logf(IB_LOG_LEVEL_INFO,
			"Page checksum innodb not valid field1 %lu field2 %lu crc32 %lu lsn %lu.",
			checksum_field1, checksum_field2, buf_calc_page_new_checksum(read_buf),
			mach_read_from_4(read_buf + FIL_PAGE_LSN)
		);
#endif
		return(false);
	}

	return(true);
}

/** Checks if the page is in none checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in none checksum format */
UNIV_INLINE
bool
buf_page_is_checksum_valid_none(
	const byte*	read_buf,
	ulint		checksum_field1,
	ulint		checksum_field2)
{
#ifdef UNIV_DEBUG_LEVEL2
	if (!(checksum_field1 == checksum_field2 || checksum_field1 == BUF_NO_CHECKSUM_MAGIC)) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Page checksum none not valid field1 %lu field2 %lu crc32 %lu lsn %lu.",
			checksum_field1, checksum_field2, BUF_NO_CHECKSUM_MAGIC,
			mach_read_from_4(read_buf + FIL_PAGE_LSN)
		);
	}
#endif

	return(checksum_field1 == checksum_field2
	       && checksum_field1 == BUF_NO_CHECKSUM_MAGIC);
}

/********************************************************************//**
Checks if a page is corrupt.
@return	TRUE if corrupted */
UNIV_INTERN
ibool
buf_page_is_corrupted(
/*==================*/
	bool		check_lsn,	/*!< in: true if we need to check
					and complain about the LSN */
	const byte*	read_buf,	/*!< in: a database page */
	ulint		zip_size)	/*!< in: size of compressed page;
					0 for uncompressed pages */
{
	ulint		checksum_field1;
	ulint		checksum_field2;
	ulint 		space_id = mach_read_from_4(
		read_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space_id);
	bool page_encrypted = false;

	/* Page is encrypted if encryption information is found from
	tablespace and page contains used key_version. This is true
	also for pages first compressed and then encrypted. */
	if (crypt_data &&
	    crypt_data->type != CRYPT_SCHEME_UNENCRYPTED &&
	    fil_page_is_encrypted(read_buf)) {
		page_encrypted = true;
	}

	if (!page_encrypted && !zip_size
	    && memcmp(read_buf + FIL_PAGE_LSN + 4,
		      read_buf + UNIV_PAGE_SIZE
		      - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4)) {

		/* Stored log sequence numbers at the start and the end
		of page do not match */

		ib_logf(IB_LOG_LEVEL_INFO,
			"Log sequence number at the start %lu and the end %lu do not match.",
			mach_read_from_4(read_buf + FIL_PAGE_LSN + 4),
			mach_read_from_4(read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + 4));

		return(TRUE);
	}

#ifndef UNIV_HOTBACKUP
	if (check_lsn && recv_lsn_checks_on) {
		lsn_t	current_lsn;

		/* Since we are going to reset the page LSN during the import
		phase it makes no sense to spam the log with error messages. */

		if (log_peek_lsn(&current_lsn)
		    && current_lsn
		    < mach_read_from_8(read_buf + FIL_PAGE_LSN)) {
			ut_print_timestamp(stderr);

			fprintf(stderr,
				" InnoDB: Error: page %lu log sequence number"
				" " LSN_PF "\n"
				"InnoDB: is in the future! Current system "
				"log sequence number " LSN_PF ".\n"
				"InnoDB: Your database may be corrupt or "
				"you may have copied the InnoDB\n"
				"InnoDB: tablespace but not the InnoDB "
				"log files. See\n"
				"InnoDB: " REFMAN
				"forcing-innodb-recovery.html\n"
				"InnoDB: for more information.\n",
				(ulint) mach_read_from_4(
					read_buf + FIL_PAGE_OFFSET),
				(lsn_t) mach_read_from_8(
					read_buf + FIL_PAGE_LSN),
				current_lsn);
		}
	}
#endif

	/* Check whether the checksum fields have correct values */

	if (srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_NONE) {
		return(FALSE);
	}

	if (zip_size) {
		return(!page_zip_verify_checksum(read_buf, zip_size));
	}

	if (page_encrypted) {
		return (FALSE);
	}

	checksum_field1 = mach_read_from_4(
		read_buf + FIL_PAGE_SPACE_OR_CHKSUM);

	checksum_field2 = mach_read_from_4(
		read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM);

#if FIL_PAGE_LSN % 8
#error "FIL_PAGE_LSN must be 64 bit aligned"
#endif

	/* declare empty pages non-corrupted */
	if (checksum_field1 == 0 && checksum_field2 == 0
	    && *reinterpret_cast<const ib_uint64_t*>(read_buf +
						     FIL_PAGE_LSN) == 0) {
		/* make sure that the page is really empty */
		for (ulint i = 0; i < UNIV_PAGE_SIZE; i++) {
			if (read_buf[i] != 0) {
				ib_logf(IB_LOG_LEVEL_INFO,
					"Checksum fields zero but page is not empty.");

				return(TRUE);
			}
		}

		return(FALSE);
	}

	DBUG_EXECUTE_IF("buf_page_is_corrupt_failure", return(TRUE); );

	ulint	page_no = mach_read_from_4(read_buf + FIL_PAGE_OFFSET);

	const srv_checksum_algorithm_t	curr_algo =
		static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);

	switch (curr_algo) {
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:

		if (buf_page_is_checksum_valid_crc32(read_buf,
			checksum_field1, checksum_field2)) {
			return(FALSE);
		}

		if (buf_page_is_checksum_valid_none(read_buf,
			checksum_field1, checksum_field2)) {
			if (curr_algo
			    == SRV_CHECKSUM_ALGORITHM_STRICT_CRC32) {
				page_warn_strict_checksum(
					curr_algo,
					SRV_CHECKSUM_ALGORITHM_NONE,
					space_id, page_no);
			}

			return(FALSE);
		}

		if (buf_page_is_checksum_valid_innodb(read_buf,
			checksum_field1, checksum_field2)) {
			if (curr_algo
			    == SRV_CHECKSUM_ALGORITHM_STRICT_CRC32) {
				page_warn_strict_checksum(
					curr_algo,
					SRV_CHECKSUM_ALGORITHM_INNODB,
					space_id, page_no);
			}

			return(FALSE);
		}

		return(TRUE);

	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:

		if (buf_page_is_checksum_valid_innodb(read_buf,
			checksum_field1, checksum_field2)) {
			return(FALSE);
		}

		if (buf_page_is_checksum_valid_none(read_buf,
			checksum_field1, checksum_field2)) {
			if (curr_algo
			    == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB) {
				page_warn_strict_checksum(
					curr_algo,
					SRV_CHECKSUM_ALGORITHM_NONE,
					space_id, page_no);
			}

			return(FALSE);
		}

		if (buf_page_is_checksum_valid_crc32(read_buf,
			checksum_field1, checksum_field2)) {
			if (curr_algo
			    == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB) {
				page_warn_strict_checksum(
					curr_algo,
					SRV_CHECKSUM_ALGORITHM_CRC32,
					space_id, page_no);
			}

			return(FALSE);
		}

		return(TRUE);

	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:

		if (buf_page_is_checksum_valid_none(read_buf,
			checksum_field1, checksum_field2)) {
			return(FALSE);
		}

		if (buf_page_is_checksum_valid_crc32(read_buf,
			checksum_field1, checksum_field2)) {
			page_warn_strict_checksum(
				curr_algo,
				SRV_CHECKSUM_ALGORITHM_CRC32,
				space_id, page_no);
			return(FALSE);
		}

		if (buf_page_is_checksum_valid_innodb(read_buf,
			checksum_field1, checksum_field2)) {
			page_warn_strict_checksum(
				curr_algo,
				SRV_CHECKSUM_ALGORITHM_INNODB,
				space_id, page_no);
			return(FALSE);
		}

		return(TRUE);

	case SRV_CHECKSUM_ALGORITHM_NONE:
		/* should have returned FALSE earlier */
		break;
	/* no default so the compiler will emit a warning if new enum
	is added and not handled here */
	}

	ut_error;
	return(FALSE);
}

/********************************************************************//**
Prints a page to stderr. */
UNIV_INTERN
void
buf_page_print(
/*===========*/
	const byte*	read_buf,	/*!< in: a database page */
	ulint		zip_size,	/*!< in: compressed page size, or
					0 for uncompressed pages */
	ulint		flags)		/*!< in: 0 or
					BUF_PAGE_PRINT_NO_CRASH or
					BUF_PAGE_PRINT_NO_FULL */

{
#ifndef UNIV_HOTBACKUP
	dict_index_t*	index;
#endif /* !UNIV_HOTBACKUP */
	ulint		size = zip_size;

	if (!size) {
		size = UNIV_PAGE_SIZE;
	}

	if (!(flags & BUF_PAGE_PRINT_NO_FULL)) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Page dump in ascii and hex (%lu bytes):\n",
			size);
		ut_print_buf(stderr, read_buf, size);
		fputs("\nInnoDB: End of page dump\n", stderr);
	}

	if (zip_size) {
		/* Print compressed page. */
		ut_print_timestamp(stderr);
		fprintf(stderr,
			" InnoDB: Compressed page type (" ULINTPF "); "
			"stored checksum in field1 " ULINTPF "; "
			"calculated checksums for field1: "
			"%s " ULINTPF ", "
			"%s " ULINTPF ", "
			"%s " ULINTPF "; "
			"page LSN " LSN_PF "; "
			"page number (if stored to page already) " ULINTPF "; "
			"space id (if stored to page already) " ULINTPF "\n",
			fil_page_get_type(read_buf),
			mach_read_from_4(read_buf + FIL_PAGE_SPACE_OR_CHKSUM),
			buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32),
			page_zip_calc_checksum(read_buf, zip_size,
				SRV_CHECKSUM_ALGORITHM_CRC32),
			buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_INNODB),
			page_zip_calc_checksum(read_buf, zip_size,
				SRV_CHECKSUM_ALGORITHM_INNODB),
			buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_NONE),
			page_zip_calc_checksum(read_buf, zip_size,
				SRV_CHECKSUM_ALGORITHM_NONE),
			mach_read_from_8(read_buf + FIL_PAGE_LSN),
			mach_read_from_4(read_buf + FIL_PAGE_OFFSET),
			mach_read_from_4(read_buf
					 + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
	} else {
		ut_print_timestamp(stderr);
		fprintf(stderr, " InnoDB: uncompressed page, "
			"stored checksum in field1 " ULINTPF ", "
			"calculated checksums for field1: "
			"%s " UINT32PF ", "
			"%s " ULINTPF ", "
			"%s " ULINTPF ", "

			"stored checksum in field2 " ULINTPF ", "
			"calculated checksums for field2: "
			"%s " UINT32PF ", "
			"%s " ULINTPF ", "
			"%s " ULINTPF ", "

			"page LSN " ULINTPF " " ULINTPF ", "
			"low 4 bytes of LSN at page end " ULINTPF ", "
			"page number (if stored to page already) " ULINTPF ", "
			"space id (if created with >= MySQL-4.1.1 "
			"and stored already) %lu\n",
			mach_read_from_4(read_buf + FIL_PAGE_SPACE_OR_CHKSUM),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_CRC32),
			buf_calc_page_crc32(read_buf),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_INNODB),
			buf_calc_page_new_checksum(read_buf),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_NONE),
			BUF_NO_CHECKSUM_MAGIC,

			mach_read_from_4(read_buf + UNIV_PAGE_SIZE
					 - FIL_PAGE_END_LSN_OLD_CHKSUM),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_CRC32),
			buf_calc_page_crc32(read_buf),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_INNODB),
			buf_calc_page_old_checksum(read_buf),
			buf_checksum_algorithm_name(SRV_CHECKSUM_ALGORITHM_NONE),
			BUF_NO_CHECKSUM_MAGIC,

			mach_read_from_4(read_buf + FIL_PAGE_LSN),
			mach_read_from_4(read_buf + FIL_PAGE_LSN + 4),
			mach_read_from_4(read_buf + UNIV_PAGE_SIZE
					 - FIL_PAGE_END_LSN_OLD_CHKSUM + 4),
			mach_read_from_4(read_buf + FIL_PAGE_OFFSET),
			mach_read_from_4(read_buf
					 + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));

		ulint page_type = fil_page_get_type(read_buf);

		fprintf(stderr, "InnoDB: page type %ld meaning %s\n", page_type,
			fil_get_page_type_name(page_type));
	}

#ifndef UNIV_HOTBACKUP
	if (mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE)
	    == TRX_UNDO_INSERT) {
		fprintf(stderr,
			"InnoDB: Page may be an insert undo log page\n");
	} else if (mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR
				    + TRX_UNDO_PAGE_TYPE)
		   == TRX_UNDO_UPDATE) {
		fprintf(stderr,
			"InnoDB: Page may be an update undo log page\n");
	}
#endif /* !UNIV_HOTBACKUP */

	switch (fil_page_get_type(read_buf)) {
		index_id_t	index_id;
	case FIL_PAGE_INDEX:
		index_id = btr_page_get_index_id(read_buf);
		fprintf(stderr,
			"InnoDB: Page may be an index page where"
			" index id is %llu\n",
			(ullint) index_id);
#ifndef UNIV_HOTBACKUP
		index = dict_index_find_on_id_low(index_id);
		if (index) {
			fputs("InnoDB: (", stderr);
			dict_index_name_print(stderr, NULL, index);
			fputs(")\n", stderr);
		}
#endif /* !UNIV_HOTBACKUP */
		break;
	case FIL_PAGE_INODE:
		fputs("InnoDB: Page may be an 'inode' page\n", stderr);
		break;
	case FIL_PAGE_IBUF_FREE_LIST:
		fputs("InnoDB: Page may be an insert buffer free list page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_ALLOCATED:
		fputs("InnoDB: Page may be a freshly allocated page\n",
		      stderr);
		break;
	case FIL_PAGE_IBUF_BITMAP:
		fputs("InnoDB: Page may be an insert buffer bitmap page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_SYS:
		fputs("InnoDB: Page may be a system page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_TRX_SYS:
		fputs("InnoDB: Page may be a transaction system page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_FSP_HDR:
		fputs("InnoDB: Page may be a file space header page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_XDES:
		fputs("InnoDB: Page may be an extent descriptor page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_BLOB:
		fputs("InnoDB: Page may be a BLOB page\n",
		      stderr);
		break;
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		fputs("InnoDB: Page may be a compressed BLOB page\n",
		      stderr);
		break;
	}

	ut_ad(flags & BUF_PAGE_PRINT_NO_CRASH);
}

#ifndef UNIV_HOTBACKUP

# ifdef PFS_GROUP_BUFFER_SYNC
/********************************************************************//**
This function registers mutexes and rwlocks in buffer blocks with
performance schema. If PFS_MAX_BUFFER_MUTEX_LOCK_REGISTER is
defined to be a value less than chunk->size, then only mutexes
and rwlocks in the first PFS_MAX_BUFFER_MUTEX_LOCK_REGISTER
blocks are registered. */
static
void
pfs_register_buffer_block(
/*======================*/
	buf_chunk_t*	chunk)		/*!< in/out: chunk of buffers */
{
	ulint		i;
	ulint		num_to_register;
	buf_block_t*    block;

	block = chunk->blocks;

	num_to_register = ut_min(chunk->size,
				 PFS_MAX_BUFFER_MUTEX_LOCK_REGISTER);

	for (i = 0; i < num_to_register; i++) {
		ib_mutex_t*	mutex;
		rw_lock_t*	rwlock;

#  ifdef UNIV_PFS_MUTEX
		mutex = &block->mutex;
		ut_a(!mutex->pfs_psi);
		mutex->pfs_psi = (PSI_server)
			? PSI_server->init_mutex(buffer_block_mutex_key, mutex)
			: NULL;
#  endif /* UNIV_PFS_MUTEX */

#  ifdef UNIV_PFS_RWLOCK
		rwlock = &block->lock;
		ut_a(!rwlock->pfs_psi);
		rwlock->pfs_psi = (PSI_server)
			? PSI_server->init_rwlock(buf_block_lock_key, rwlock)
			: NULL;

#   ifdef UNIV_SYNC_DEBUG
		rwlock = &block->debug_latch;
		ut_a(!rwlock->pfs_psi);
		rwlock->pfs_psi = (PSI_server)
			? PSI_server->init_rwlock(buf_block_debug_latch_key,
						  rwlock)
			: NULL;
#   endif /* UNIV_SYNC_DEBUG */

#  endif /* UNIV_PFS_RWLOCK */
		block++;
	}
}
# endif /* PFS_GROUP_BUFFER_SYNC */

/********************************************************************//**
Initializes a buffer control block when the buf_pool is created. */
static
void
buf_block_init(
/*===========*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_block_t*	block,		/*!< in: pointer to control block */
	byte*		frame)		/*!< in: pointer to buffer frame */
{
	UNIV_MEM_DESC(frame, UNIV_PAGE_SIZE);

	block->frame = frame;

	block->page.buf_pool_index = buf_pool_index(buf_pool);
	block->page.flush_type = BUF_FLUSH_LRU;
	block->page.state = BUF_BLOCK_NOT_USED;
	block->page.buf_fix_count = 0;
	block->page.io_fix = BUF_IO_NONE;
	block->page.key_version = 0;
	block->page.page_encrypted = false;
	block->page.page_compressed = false;
	block->page.encrypted = false;
	block->page.stored_checksum = BUF_NO_CHECKSUM_MAGIC;
	block->page.calculated_checksum = BUF_NO_CHECKSUM_MAGIC;
	block->page.real_size = 0;
	block->page.write_size = 0;
	block->modify_clock = 0;
	block->page.slot = NULL;

#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	block->page.file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES || UNIV_DEBUG */

	block->check_index_page_at_flush = FALSE;
	block->index = NULL;

#ifdef UNIV_DEBUG
	block->page.in_page_hash = FALSE;
	block->page.in_zip_hash = FALSE;
	block->page.in_flush_list = FALSE;
	block->page.in_free_list = FALSE;
	block->page.in_LRU_list = FALSE;
	block->in_unzip_LRU_list = FALSE;
#endif /* UNIV_DEBUG */
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	block->n_pointers = 0;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	page_zip_des_init(&block->page.zip);

#if defined PFS_SKIP_BUFFER_MUTEX_RWLOCK || defined PFS_GROUP_BUFFER_SYNC
	/* If PFS_SKIP_BUFFER_MUTEX_RWLOCK is defined, skip registration
	of buffer block mutex/rwlock with performance schema. If
	PFS_GROUP_BUFFER_SYNC is defined, skip the registration
	since buffer block mutex/rwlock will be registered later in
	pfs_register_buffer_block() */

	mutex_create(PFS_NOT_INSTRUMENTED, &block->mutex, SYNC_BUF_BLOCK);
	rw_lock_create(PFS_NOT_INSTRUMENTED, &block->lock, SYNC_LEVEL_VARYING);

# ifdef UNIV_SYNC_DEBUG
	rw_lock_create(PFS_NOT_INSTRUMENTED,
		       &block->debug_latch, SYNC_NO_ORDER_CHECK);
# endif /* UNIV_SYNC_DEBUG */

#else /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */
	mutex_create(buffer_block_mutex_key, &block->mutex, SYNC_BUF_BLOCK);
	rw_lock_create(buf_block_lock_key, &block->lock, SYNC_LEVEL_VARYING);

# ifdef UNIV_SYNC_DEBUG
	rw_lock_create(buf_block_debug_latch_key,
		       &block->debug_latch, SYNC_NO_ORDER_CHECK);
# endif /* UNIV_SYNC_DEBUG */
#endif /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */

	ut_ad(rw_lock_validate(&(block->lock)));
}

/********************************************************************//**
Allocates a chunk of buffer frames.
@return	chunk, or NULL on failure */
static
buf_chunk_t*
buf_chunk_init(
/*===========*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_chunk_t*	chunk,		/*!< out: chunk of buffers */
	ulint		mem_size)	/*!< in: requested size in bytes */
{
	buf_block_t*	block;
	byte*		frame;
	ulint		i;
	ulint		size_target;

	/* Round down to a multiple of page size,
	although it already should be. */
	mem_size = ut_2pow_round(mem_size, UNIV_PAGE_SIZE);
	size_target = (mem_size / UNIV_PAGE_SIZE) - 1;
	/* Reserve space for the block descriptors. */
	mem_size += ut_2pow_round((mem_size / UNIV_PAGE_SIZE) * (sizeof *block)
				  + (UNIV_PAGE_SIZE - 1), UNIV_PAGE_SIZE);

	chunk->mem_size = mem_size;
	chunk->mem = os_mem_alloc_large(&chunk->mem_size);

	if (UNIV_UNLIKELY(chunk->mem == NULL)) {

		return(NULL);
	}

#ifdef HAVE_LIBNUMA
	if (srv_numa_interleave) {
		struct bitmask *numa_mems_allowed = numa_get_mems_allowed();
		int	st = mbind(chunk->mem, chunk->mem_size,
				   MPOL_INTERLEAVE,
				   numa_mems_allowed->maskp,
				   numa_mems_allowed->size,
				   MPOL_MF_MOVE);
		if (st != 0) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Failed to set NUMA memory policy of buffer"
				" pool page frames to MPOL_INTERLEAVE"
				" (error: %s).", strerror(errno));
		}
	}
#endif // HAVE_LIBNUMA

	/* Allocate the block descriptors from
	the start of the memory block. */
	chunk->blocks = (buf_block_t*) chunk->mem;

	/* Align a pointer to the first frame.  Note that when
	os_large_page_size is smaller than UNIV_PAGE_SIZE,
	we may allocate one fewer block than requested.  When
	it is bigger, we may allocate more blocks than requested. */

	frame = (byte*) ut_align(chunk->mem, UNIV_PAGE_SIZE);
	chunk->size = chunk->mem_size / UNIV_PAGE_SIZE
		- (frame != chunk->mem);

	/* Subtract the space needed for block descriptors. */
	{
		ulint	size = chunk->size;

		while (frame < (byte*) (chunk->blocks + size)) {
			frame += UNIV_PAGE_SIZE;
			size--;
		}

		chunk->size = size;
	}

	if (chunk->size > size_target) {
		chunk->size = size_target;
	}

	/* Init block structs and assign frames for them. Then we
	assign the frames to the first blocks (we already mapped the
	memory above). */

	block = chunk->blocks;

	for (i = chunk->size; i--; ) {

		buf_block_init(buf_pool, block, frame);
		UNIV_MEM_INVALID(block->frame, UNIV_PAGE_SIZE);

		/* Add the block to the free list */
		UT_LIST_ADD_LAST(list, buf_pool->free, (&block->page));

		ut_d(block->page.in_free_list = TRUE);
		ut_ad(buf_pool_from_block(block) == buf_pool);

		block++;
		frame += UNIV_PAGE_SIZE;
	}

#ifdef PFS_GROUP_BUFFER_SYNC
	pfs_register_buffer_block(chunk);
#endif
	return(chunk);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Finds a block in the given buffer chunk that points to a
given compressed page.
@return	buffer block pointing to the compressed page, or NULL */
static
buf_block_t*
buf_chunk_contains_zip(
/*===================*/
	buf_chunk_t*	chunk,	/*!< in: chunk being checked */
	const void*	data)	/*!< in: pointer to compressed page */
{
	buf_block_t*	block;
	ulint		i;

	block = chunk->blocks;

	for (i = chunk->size; i--; block++) {
		if (block->page.zip.data == data) {

			return(block);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Finds a block in the buffer pool that points to a
given compressed page.
@return	buffer block pointing to the compressed page, or NULL */
UNIV_INTERN
buf_block_t*
buf_pool_contains_zip(
/*==================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	const void*	data)		/*!< in: pointer to compressed page */
{
	ulint		n;
	buf_chunk_t*	chunk = buf_pool->chunks;

	ut_ad(buf_pool);
	for (n = buf_pool->n_chunks; n--; chunk++) {

		buf_block_t* block = buf_chunk_contains_zip(chunk, data);

		if (block) {
			return(block);
		}
	}

	return(NULL);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Checks that all file pages in the buffer chunk are in a replaceable state.
@return	address of a non-free block, or NULL if all freed */
static
const buf_block_t*
buf_chunk_not_freed(
/*================*/
	buf_chunk_t*	chunk)	/*!< in: chunk being checked */
{
	buf_block_t*	block;
	ulint		i;

	block = chunk->blocks;

	for (i = chunk->size; i--; block++) {
		ibool	ready;

		switch (buf_block_get_state(block)) {
		case BUF_BLOCK_POOL_WATCH:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_ZIP_DIRTY:
			/* The uncompressed buffer pool should never
			contain compressed block descriptors. */
			ut_error;
			break;
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			/* Skip blocks that are not being used for
			file pages. */
			break;
		case BUF_BLOCK_FILE_PAGE:
			mutex_enter(&block->mutex);
			ready = buf_flush_ready_for_replace(&block->page);
			mutex_exit(&block->mutex);

			if (UNIV_UNLIKELY(block->page.is_corrupt)) {
				/* corrupt page may remain, it can be
				skipped */
				break;
			}

			if (!ready) {

				return(block);
			}

			break;
		}
	}

	return(NULL);
}

/********************************************************************//**
Set buffer pool size variables after resizing it */
static
void
buf_pool_set_sizes(void)
/*====================*/
{
	ulint	i;
	ulint	curr_size = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		curr_size += buf_pool->curr_pool_size;
	}

	srv_buf_pool_curr_size = curr_size;
	srv_buf_pool_old_size = srv_buf_pool_size;
}

/********************************************************************//**
Initialize a buffer pool instance.
@return DB_SUCCESS if all goes well. */
UNIV_INTERN
ulint
buf_pool_init_instance(
/*===================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		buf_pool_size,	/*!< in: size in bytes */
	ulint		instance_no)	/*!< in: id of the instance */
{
	ulint		i;
	buf_chunk_t*	chunk;

	/* 1. Initialize general fields
	------------------------------- */
	mutex_create(buf_pool_LRU_list_mutex_key,
		     &buf_pool->LRU_list_mutex, SYNC_BUF_LRU_LIST);
	mutex_create(buf_pool_free_list_mutex_key,
		     &buf_pool->free_list_mutex, SYNC_BUF_FREE_LIST);
	mutex_create(buf_pool_zip_free_mutex_key,
		     &buf_pool->zip_free_mutex, SYNC_BUF_ZIP_FREE);
	mutex_create(buf_pool_zip_hash_mutex_key,
		     &buf_pool->zip_hash_mutex, SYNC_BUF_ZIP_HASH);
	mutex_create(buf_pool_zip_mutex_key,
		     &buf_pool->zip_mutex, SYNC_BUF_BLOCK);
	mutex_create(buf_pool_flush_state_mutex_key,
		     &buf_pool->flush_state_mutex, SYNC_BUF_FLUSH_STATE);

	if (buf_pool_size > 0) {
		buf_pool->n_chunks = 1;

		buf_pool->chunks = chunk =
			(buf_chunk_t*) mem_zalloc(sizeof *chunk);

		UT_LIST_INIT(buf_pool->free);

		if (!buf_chunk_init(buf_pool, chunk, buf_pool_size)) {
			mem_free(chunk);
			mem_free(buf_pool);

			return(DB_ERROR);
		}

		buf_pool->instance_no = instance_no;
		buf_pool->old_pool_size = buf_pool_size;
		buf_pool->curr_size = chunk->size;
		buf_pool->read_ahead_area
			= ut_min(64, ut_2_power_up(buf_pool->curr_size / 32));
		buf_pool->curr_pool_size = buf_pool->curr_size * UNIV_PAGE_SIZE;

		/* Number of locks protecting page_hash must be a
		power of two */
		srv_n_page_hash_locks = static_cast<ulong>(
				 ut_2_power_up(srv_n_page_hash_locks));
		ut_a(srv_n_page_hash_locks != 0);
		ut_a(srv_n_page_hash_locks <= MAX_PAGE_HASH_LOCKS);

		buf_pool->page_hash = ha_create(2 * buf_pool->curr_size,
						srv_n_page_hash_locks,
						MEM_HEAP_FOR_PAGE_HASH,
						SYNC_BUF_PAGE_HASH);

		buf_pool->zip_hash = hash_create(2 * buf_pool->curr_size);

		buf_pool->last_printout_time = ut_time();
	}
	/* 2. Initialize flushing fields
	-------------------------------- */

	mutex_create(flush_list_mutex_key, &buf_pool->flush_list_mutex,
		     SYNC_BUF_FLUSH_LIST);

	for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {
		buf_pool->no_flush[i] = os_event_create();
	}

	buf_pool->watch = (buf_page_t*) mem_zalloc(
		sizeof(*buf_pool->watch) * BUF_POOL_WATCH_SIZE);

	/* All fields are initialized by mem_zalloc(). */

	/* Initialize the temporal memory array and slots */
	buf_pool->tmp_arr = (buf_tmp_array_t *)mem_zalloc(sizeof(buf_tmp_array_t));
	ulint n_slots = srv_n_read_io_threads * srv_n_write_io_threads * (8 * OS_AIO_N_PENDING_IOS_PER_THREAD);
	buf_pool->tmp_arr->n_slots = n_slots;
	buf_pool->tmp_arr->slots = (buf_tmp_buffer_t*)mem_zalloc(sizeof(buf_tmp_buffer_t) * n_slots);

	buf_pool->try_LRU_scan = TRUE;

	DBUG_EXECUTE_IF("buf_pool_init_instance_force_oom",
		return(DB_ERROR); );

	return(DB_SUCCESS);
}

/********************************************************************//**
free one buffer pool instance */
static
void
buf_pool_free_instance(
/*===================*/
	buf_pool_t*	buf_pool)	/* in,own: buffer pool instance
					to free */
{
	buf_chunk_t*	chunk;
	buf_chunk_t*	chunks;
	buf_page_t*	bpage;
	ulint		i;

	bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	while (bpage != NULL) {
		buf_page_t*	prev_bpage = UT_LIST_GET_PREV(LRU, bpage);
		enum buf_page_state	state = buf_page_get_state(bpage);

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_LRU_list);

		if (state != BUF_BLOCK_FILE_PAGE) {
			/* We must not have any dirty block except
			when doing a fast shutdown. */
			ut_ad(state == BUF_BLOCK_ZIP_PAGE
			      || srv_fast_shutdown == 2);
			buf_page_free_descriptor(bpage);
		}

		bpage = prev_bpage;
	}

	mem_free(buf_pool->watch);
	buf_pool->watch = NULL;

	for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {
		os_event_free(buf_pool->no_flush[i]);
	}
	mutex_free(&buf_pool->LRU_list_mutex);
	mutex_free(&buf_pool->free_list_mutex);
	mutex_free(&buf_pool->zip_free_mutex);
	mutex_free(&buf_pool->zip_hash_mutex);
	mutex_free(&buf_pool->zip_mutex);
	mutex_free(&buf_pool->flush_state_mutex);
	mutex_free(&buf_pool->flush_list_mutex);

	chunks = buf_pool->chunks;
	chunk = chunks + buf_pool->n_chunks;

	while (--chunk >= chunks) {
		buf_block_t* block = chunk->blocks;
		for (i = 0; i < chunk->size; i++, block++) {
			mutex_free(&block->mutex);
			rw_lock_free(&block->lock);
#ifdef UNIV_SYNC_DEBUG
			rw_lock_free(&block->debug_latch);
#endif
		}
		os_mem_free_large(chunk->mem, chunk->mem_size);
	}

	mem_free(buf_pool->chunks);
	ha_clear(buf_pool->page_hash);
	hash_table_free(buf_pool->page_hash);
	hash_table_free(buf_pool->zip_hash);

	/* Free all used temporary slots */
	if (buf_pool->tmp_arr) {
		for(ulint i = 0; i < buf_pool->tmp_arr->n_slots; i++) {
			buf_tmp_buffer_t* slot = &(buf_pool->tmp_arr->slots[i]);
#ifdef HAVE_LZO
			if (slot && slot->lzo_mem) {
				ut_free(slot->lzo_mem);
				slot->lzo_mem = NULL;
			}
#endif
			if (slot && slot->crypt_buf_free) {
				ut_free(slot->crypt_buf_free);
				slot->crypt_buf_free = NULL;
			}

			if (slot && slot->comp_buf_free) {
				ut_free(slot->comp_buf_free);
				slot->comp_buf_free = NULL;
			}
		}
	}

	mem_free(buf_pool->tmp_arr->slots);
	mem_free(buf_pool->tmp_arr);
	buf_pool->tmp_arr = NULL;
}

/********************************************************************//**
Creates the buffer pool.
@return	DB_SUCCESS if success, DB_ERROR if not enough memory or error */
UNIV_INTERN
dberr_t
buf_pool_init(
/*==========*/
	ulint	total_size,	/*!< in: size of the total pool in bytes */
	ulint	n_instances)	/*!< in: number of instances */
{
	ulint		i;
	const ulint	size	= total_size / n_instances;

	ut_ad(n_instances > 0);
	ut_ad(n_instances <= MAX_BUFFER_POOLS);
	ut_ad(n_instances == srv_buf_pool_instances);

#ifdef HAVE_LIBNUMA
	if (srv_numa_interleave) {
		struct bitmask *numa_mems_allowed = numa_get_mems_allowed();

		ib_logf(IB_LOG_LEVEL_INFO,
			"Setting NUMA memory policy to MPOL_INTERLEAVE");
		if (set_mempolicy(MPOL_INTERLEAVE,
				  numa_mems_allowed->maskp,
				  numa_mems_allowed->size) != 0) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Failed to set NUMA memory policy to"
				" MPOL_INTERLEAVE (error: %s).",
				strerror(errno));
		}
	}
#endif // HAVE_LIBNUMA

	buf_pool_ptr = (buf_pool_t*) mem_zalloc(
		n_instances * sizeof *buf_pool_ptr);

	for (i = 0; i < n_instances; i++) {
		buf_pool_t*	ptr	= &buf_pool_ptr[i];

		if (buf_pool_init_instance(ptr, size, i) != DB_SUCCESS) {

			/* Free all the instances created so far. */
			buf_pool_free(i);

			return(DB_ERROR);
		}
	}

	buf_pool_set_sizes();
	buf_LRU_old_ratio_update(100 * 3/ 8, FALSE);

	btr_search_sys_create(buf_pool_get_curr_size() / sizeof(void*) / 64);

#ifdef HAVE_LIBNUMA
	if (srv_numa_interleave) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Setting NUMA memory policy to MPOL_DEFAULT");
		if (set_mempolicy(MPOL_DEFAULT, NULL, 0) != 0) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Failed to set NUMA memory policy to"
				" MPOL_DEFAULT (error: %s).", strerror(errno));
		}
	}
#endif // HAVE_LIBNUMA

	return(DB_SUCCESS);
}

/********************************************************************//**
Frees the buffer pool at shutdown.  This must not be invoked before
freeing all mutexes. */
UNIV_INTERN
void
buf_pool_free(
/*==========*/
	ulint	n_instances)	/*!< in: numbere of instances to free */
{
	ulint	i;

	for (i = 0; i < n_instances; i++) {
		buf_pool_free_instance(buf_pool_from_array(i));
	}

	mem_free(buf_pool_ptr);
	buf_pool_ptr = NULL;
}

/********************************************************************//**
Clears the adaptive hash index on all pages in the buffer pool. */
UNIV_INTERN
void
buf_pool_clear_hash_index(void)
/*===========================*/
{
	ulint	p;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(btr_search_own_all(RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(!btr_search_enabled);

	for (p = 0; p < srv_buf_pool_instances; p++) {
		buf_pool_t*	buf_pool = buf_pool_from_array(p);
		buf_chunk_t*	chunks	= buf_pool->chunks;
		buf_chunk_t*	chunk	= chunks + buf_pool->n_chunks;

		while (--chunk >= chunks) {
			buf_block_t*	block	= chunk->blocks;
			ulint		i	= chunk->size;

			for (; i--; block++) {
				dict_index_t*	index	= block->index;

				/* We can set block->index = NULL
				when we have an x-latch on btr_search_latch;
				see the comment in buf0buf.h */

				if (!index) {
					/* Not hashed */
					continue;
				}

				block->index = NULL;
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
				block->n_pointers = 0;
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
			}
		}
	}
}

/********************************************************************//**
Relocate a buffer control block.  Relocates the block on the LRU list
and in buf_pool->page_hash.  Does not relocate bpage->list.
The caller must take care of relocating bpage->list. */
UNIV_INTERN
void
buf_relocate(
/*=========*/
	buf_page_t*	bpage,	/*!< in/out: control block being relocated;
				buf_page_get_state(bpage) must be
				BUF_BLOCK_ZIP_DIRTY or BUF_BLOCK_ZIP_PAGE */
	buf_page_t*	dpage)	/*!< in/out: destination control block */
{
	buf_page_t*	b;
	ulint		fold;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	fold = buf_page_address_fold(bpage->space, bpage->offset);

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));
	ut_ad(buf_page_hash_lock_held_x(buf_pool, bpage));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage == buf_page_hash_get_low(buf_pool,
					     bpage->space,
					     bpage->offset,
					     fold));

	ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
#ifdef UNIV_DEBUG
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_FILE_PAGE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
	case BUF_BLOCK_ZIP_DIRTY:
	case BUF_BLOCK_ZIP_PAGE:
		break;
	}
#endif /* UNIV_DEBUG */

	memcpy(dpage, bpage, sizeof *dpage);

	ut_d(bpage->in_LRU_list = FALSE);
	ut_d(bpage->in_page_hash = FALSE);

	/* relocate buf_pool->LRU */
	b = UT_LIST_GET_PREV(LRU, bpage);
	UT_LIST_REMOVE(LRU, buf_pool->LRU, bpage);

	if (b) {
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, b, dpage);
	} else {
		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, dpage);
	}

	if (UNIV_UNLIKELY(buf_pool->LRU_old == bpage)) {
		buf_pool->LRU_old = dpage;
#ifdef UNIV_LRU_DEBUG
		/* buf_pool->LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool->LRU_old->old);
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool->LRU_old)->old);
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old)->old);
	} else {
		/* Check that the "old" flag is consistent in
		the block and its neighbours. */
		buf_page_set_old(dpage, buf_page_is_old(dpage));
#endif /* UNIV_LRU_DEBUG */
	}

        ut_d(UT_LIST_VALIDATE(
		LRU, buf_page_t, buf_pool->LRU, CheckInLRUList()));

	/* relocate buf_pool->page_hash */
	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, fold, bpage);
	HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, fold, dpage);
}

/********************************************************************//**
Determine if a block is a sentinel for a buffer pool watch.
@return	TRUE if a sentinel for a buffer pool watch, FALSE if not */
UNIV_INTERN
ibool
buf_pool_watch_is_sentinel(
/*=======================*/
	buf_pool_t*		buf_pool,	/*!< buffer pool instance */
	const buf_page_t*	bpage)		/*!< in: block */
{
	/* We must also own the appropriate hash lock. */
	ut_ad(buf_page_hash_lock_held_s_or_x(buf_pool, bpage));
	ut_ad(buf_page_in_file(bpage));

	if (bpage < &buf_pool->watch[0]
	    || bpage >= &buf_pool->watch[BUF_POOL_WATCH_SIZE]) {

		ut_ad(buf_page_get_state(bpage) != BUF_BLOCK_ZIP_PAGE
		      || bpage->zip.data != NULL);

		return(FALSE);
	}

	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage->zip.data == NULL);
	ut_ad(bpage->buf_fix_count > 0);
	return(TRUE);
}

/****************************************************************//**
Add watch for the given page to be read in. Caller must have
appropriate hash_lock for the bpage and hold the LRU list mutex to avoid a race
condition with buf_LRU_free_page inserting the same page into the page hash.
This function may release the hash_lock and reacquire it.
@return NULL if watch set, block if the page is in the buffer pool */
UNIV_INTERN
buf_page_t*
buf_pool_watch_set(
/*===============*/
	ulint	space,	/*!< in: space id */
	ulint	offset,	/*!< in: page number */
	ulint	fold)	/*!< in: buf_page_address_fold(space, offset) */
{
	buf_page_t*	bpage;
	ulint		i;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	prio_rw_lock_t*	hash_lock;

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	hash_lock = buf_page_hash_lock_get(buf_pool, fold);

#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(hash_lock, RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	bpage = buf_page_hash_get_low(buf_pool, space, offset, fold);

	if (bpage != NULL) {
page_found:
		if (!buf_pool_watch_is_sentinel(buf_pool, bpage)) {
			/* The page was loaded meanwhile. */
			return(bpage);
		}

		/* Add to an existing watch. */
#ifdef PAGE_ATOMIC_REF_COUNT
		os_atomic_increment_uint32(&bpage->buf_fix_count, 1);
#else
		++bpage->buf_fix_count;
#endif /* PAGE_ATOMIC_REF_COUNT */
		return(NULL);
	}

	/* From this point this function becomes fairly heavy in terms
	of latching. We acquire all the hash_locks. They are needed
	because we don't want to read any stale information in
	buf_pool->watch[]. However, it is not in the critical code path
	as this function will be called only by the purge thread. */


	/* To obey latching order first release the hash_lock. */
	rw_lock_x_unlock(hash_lock);

	hash_lock_x_all(buf_pool->page_hash);

	/* We have to recheck that the page
	was not loaded or a watch set by some other
	purge thread. This is because of the small
	time window between when we release the
	hash_lock to acquire all the hash locks above. */

	bpage = buf_page_hash_get_low(buf_pool, space, offset, fold);
	if (UNIV_LIKELY_NULL(bpage)) {
		hash_unlock_x_all_but(buf_pool->page_hash, hash_lock);
		goto page_found;
	}

	/* The maximum number of purge threads should never exceed
	BUF_POOL_WATCH_SIZE. So there is no way for purge thread
	instance to hold a watch when setting another watch. */
	for (i = 0; i < BUF_POOL_WATCH_SIZE; i++) {
		bpage = &buf_pool->watch[i];

		ut_ad(bpage->access_time == 0);
		ut_ad(bpage->newest_modification == 0);
		ut_ad(bpage->oldest_modification == 0);
		ut_ad(bpage->zip.data == NULL);
		ut_ad(!bpage->in_zip_hash);

		switch (bpage->state) {
		case BUF_BLOCK_POOL_WATCH:
			ut_ad(!bpage->in_page_hash);
			ut_ad(bpage->buf_fix_count == 0);

			bpage->state = BUF_BLOCK_ZIP_PAGE;
			bpage->space = static_cast<ib_uint32_t>(space);
			bpage->offset = static_cast<ib_uint32_t>(offset);
			bpage->buf_fix_count = 1;
			bpage->buf_pool_index = buf_pool_index(buf_pool);

			ut_d(bpage->in_page_hash = TRUE);
			HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
				    fold, bpage);

			/* Once the sentinel is in the page_hash we can
			safely release all locks except just the
			relevant hash_lock */
			hash_unlock_x_all_but(buf_pool->page_hash,
						hash_lock);

			return(NULL);
		case BUF_BLOCK_ZIP_PAGE:
			ut_ad(bpage->in_page_hash);
			ut_ad(bpage->buf_fix_count > 0);
			break;
		default:
			ut_error;
		}
	}

	/* Allocation failed.  Either the maximum number of purge
	threads should never exceed BUF_POOL_WATCH_SIZE, or this code
	should be modified to return a special non-NULL value and the
	caller should purge the record directly. */
	ut_error;

	/* Fix compiler warning */
	return(NULL);
}

/****************************************************************//**
Remove the sentinel block for the watch before replacing it with a real block.
buf_page_watch_clear() or buf_page_watch_occurred() will notice that
the block has been replaced with the real block.
@return reference count, to be added to the replacement block */
static
void
buf_pool_watch_remove(
/*==================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	ulint		fold,		/*!< in: buf_page_address_fold(
					space, offset) */
	buf_page_t*	watch)		/*!< in/out: sentinel for watch */
{
#ifdef UNIV_SYNC_DEBUG
	/* We must also own the appropriate hash_bucket mutex. */
	prio_rw_lock_t* hash_lock = buf_page_hash_lock_get(buf_pool, fold);
	ut_ad(rw_lock_own(hash_lock, RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	ut_ad(buf_page_get_state(watch) == BUF_BLOCK_ZIP_PAGE);

	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, fold, watch);
	ut_d(watch->in_page_hash = FALSE);
	watch->buf_fix_count = 0;
	watch->state = BUF_BLOCK_POOL_WATCH;
}

/****************************************************************//**
Stop watching if the page has been read in.
buf_pool_watch_set(space,offset) must have returned NULL before. */
UNIV_INTERN
void
buf_pool_watch_unset(
/*=================*/
	ulint	space,	/*!< in: space id */
	ulint	offset)	/*!< in: page number */
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	ulint		fold = buf_page_address_fold(space, offset);
	prio_rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool, fold);

	rw_lock_x_lock(hash_lock);

	/* The page must exist because buf_pool_watch_set() increments
	buf_fix_count. */

	bpage = buf_page_hash_get_low(buf_pool, space, offset, fold);

	if (!buf_pool_watch_is_sentinel(buf_pool, bpage)) {
		buf_block_unfix(reinterpret_cast<buf_block_t*>(bpage));
	} else {

		ut_ad(bpage->buf_fix_count > 0);

#ifdef PAGE_ATOMIC_REF_COUNT
		os_atomic_decrement_uint32(&bpage->buf_fix_count, 1);
#else
		--bpage->buf_fix_count;
#endif /* PAGE_ATOMIC_REF_COUNT */

		if (bpage->buf_fix_count == 0) {
			buf_pool_watch_remove(buf_pool, fold, bpage);
		}
	}

	rw_lock_x_unlock(hash_lock);
}

/****************************************************************//**
Check if the page has been read in.
This may only be called after buf_pool_watch_set(space,offset)
has returned NULL and before invoking buf_pool_watch_unset(space,offset).
@return	FALSE if the given page was not read in, TRUE if it was */
UNIV_INTERN
ibool
buf_pool_watch_occurred(
/*====================*/
	ulint	space,	/*!< in: space id */
	ulint	offset)	/*!< in: page number */
{
	ibool		ret;
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	ulint		fold	= buf_page_address_fold(space, offset);
	prio_rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool,
							     fold);

	rw_lock_s_lock(hash_lock);

	/* The page must exist because buf_pool_watch_set()
	increments buf_fix_count. */
	bpage = buf_page_hash_get_low(buf_pool, space, offset, fold);

	ret = !buf_pool_watch_is_sentinel(buf_pool, bpage);
	rw_lock_s_unlock(hash_lock);

	return(ret);
}

/********************************************************************//**
Moves a page to the start of the buffer pool LRU list. This high-level
function can be used to prevent an important page from slipping out of
the buffer pool. */
UNIV_INTERN
void
buf_page_make_young(
/*================*/
	buf_page_t*	bpage)	/*!< in: buffer block of a file page */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);

	ut_a(buf_page_in_file(bpage));

	buf_LRU_make_block_young(bpage);

	mutex_exit(&buf_pool->LRU_list_mutex);
}

/********************************************************************//**
Moves a page to the start of the buffer pool LRU list if it is too old.
This high-level function can be used to prevent an important page from
slipping out of the buffer pool. */
static
void
buf_page_make_young_if_needed(
/*==========================*/
	buf_page_t*	bpage)		/*!< in/out: buffer block of a
					file page */
{
	ut_a(buf_page_in_file(bpage));

	if (buf_page_peek_if_too_old(bpage)) {
		buf_page_make_young(bpage);
	}
}

/********************************************************************//**
Resets the check_index_page_at_flush field of a page if found in the buffer
pool. */
UNIV_INTERN
void
buf_reset_check_index_page_at_flush(
/*================================*/
	ulint	space,	/*!< in: space id */
	ulint	offset)	/*!< in: page number */
{
	buf_block_t*	block;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	block = (buf_block_t*) buf_page_hash_get(buf_pool, space, offset);

	if (block && buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE) {
		ut_ad(!buf_pool_watch_is_sentinel(buf_pool, &block->page));
		block->check_index_page_at_flush = FALSE;
	}
}

#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
/********************************************************************//**
Sets file_page_was_freed TRUE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated.
@return	control block if found in page hash table, otherwise NULL */
UNIV_INTERN
buf_page_t*
buf_page_set_file_page_was_freed(
/*=============================*/
	ulint	space,	/*!< in: space id */
	ulint	offset)	/*!< in: page number */
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	prio_rw_lock_t*	hash_lock;

	bpage = buf_page_hash_get_s_locked(buf_pool, space, offset,
					   &hash_lock);

	if (bpage) {
		ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);
		ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
		mutex_enter(block_mutex);
		rw_lock_s_unlock(hash_lock);
		/* bpage->file_page_was_freed can already hold
		when this code is invoked from dict_drop_index_tree() */
		bpage->file_page_was_freed = TRUE;
		mutex_exit(block_mutex);
	}

	return(bpage);
}

/********************************************************************//**
Sets file_page_was_freed FALSE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated.
@return	control block if found in page hash table, otherwise NULL */
UNIV_INTERN
buf_page_t*
buf_page_reset_file_page_was_freed(
/*===============================*/
	ulint	space,	/*!< in: space id */
	ulint	offset)	/*!< in: page number */
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	prio_rw_lock_t*	hash_lock;

	bpage = buf_page_hash_get_s_locked(buf_pool, space, offset,
					   &hash_lock);
	if (bpage) {
		ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);
		ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
		mutex_enter(block_mutex);
		rw_lock_s_unlock(hash_lock);
		bpage->file_page_was_freed = FALSE;
		mutex_exit(block_mutex);
	}

	return(bpage);
}
#endif /* UNIV_DEBUG_FILE_ACCESSES || UNIV_DEBUG */

/********************************************************************//**
Attempts to discard the uncompressed frame of a compressed page. The
caller should not be holding any mutexes when this function is called.
@return	TRUE if successful, FALSE otherwise. */
static
void
buf_block_try_discard_uncompressed(
/*===============================*/
	ulint		space,	/*!< in: space id */
	ulint		offset)	/*!< in: page number */
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	/* Since we need to acquire buf_pool->LRU_list_mutex to discard
	the uncompressed frame and because page_hash mutex resides below
	buf_pool->LRU_list_mutex in sync ordering therefore we must first
	release the page_hash mutex. This means that the block in question
	can move out of page_hash. Therefore we need to check again if the
	block is still in page_hash. */

	mutex_enter(&buf_pool->LRU_list_mutex);

	bpage = buf_page_hash_get(buf_pool, space, offset);

	if (bpage) {

		ib_mutex_t* block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_LRU_free_page(bpage, false)) {

			mutex_exit(block_mutex);
			return;
		}
		mutex_exit(block_mutex);
	}

	mutex_exit(&buf_pool->LRU_list_mutex);
}

/********************************************************************//**
Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with buf_page_release_zip().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches.
@return	pointer to the block */
UNIV_INTERN
buf_page_t*
buf_page_get_zip(
/*=============*/
	ulint		space,	/*!< in: space id */
	ulint		zip_size,/*!< in: compressed page size */
	ulint		offset)	/*!< in: page number */
{
	buf_page_t*	bpage;
	ib_mutex_t*	block_mutex;
	prio_rw_lock_t*	hash_lock;
	ibool		discard_attempted = FALSE;
	ibool		must_read;
	trx_t*		trx = NULL;
	ulint		sec;
	ulint		ms;
	ib_uint64_t	start_time;
	ib_uint64_t	finish_time;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	if (UNIV_UNLIKELY(innobase_get_slow_log())) {
		trx = innobase_get_trx();
	}
	buf_pool->stat.n_page_gets++;

	for (;;) {
lookup:

		/* The following call will also grab the page_hash
		mutex if the page is found. */
		bpage = buf_page_hash_get_s_locked(buf_pool, space,
						offset, &hash_lock);
		if (bpage) {
			ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
			break;
		}

		/* Page not in buf_pool: needs to be read from file */

		ut_ad(!hash_lock);
		buf_read_page(space, zip_size, offset, trx, NULL);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(++buf_dbg_counter % 5771 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
	}

	ut_ad(buf_page_hash_lock_held_s(buf_pool, bpage));

	if (!bpage->zip.data) {
		/* There is no compressed page. */
err_exit:
		rw_lock_s_unlock(hash_lock);
		return(NULL);
	}

	if (UNIV_UNLIKELY(bpage->is_corrupt && srv_pass_corrupt_table <= 1)) {

		rw_lock_s_unlock(hash_lock);

		return(NULL);
	}

	ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;

	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		block_mutex = &buf_pool->zip_mutex;
		mutex_enter(block_mutex);
#ifdef PAGE_ATOMIC_REF_COUNT
		os_atomic_increment_uint32(&bpage->buf_fix_count, 1);
#else
		++bpage->buf_fix_count;
#endif /* PAGE_ATOMIC_REF_COUNT */
		goto got_block;
	case BUF_BLOCK_FILE_PAGE:
		/* Discard the uncompressed page frame if possible. */
		if (!discard_attempted) {
			rw_lock_s_unlock(hash_lock);
			buf_block_try_discard_uncompressed(space, offset);
			discard_attempted = TRUE;
			goto lookup;
		}

		block_mutex = &((buf_block_t*) bpage)->mutex;

		mutex_enter(block_mutex);

		buf_block_buf_fix_inc((buf_block_t*) bpage, __FILE__, __LINE__);
		goto got_block;
	}

	ut_error;
	goto err_exit;

got_block:
	must_read = buf_page_get_io_fix(bpage) == BUF_IO_READ;

	rw_lock_s_unlock(hash_lock);
#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	ut_a(!bpage->file_page_was_freed);
#endif /* defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG */

	buf_page_set_accessed(bpage);

	mutex_exit(block_mutex);

	buf_page_make_young_if_needed(bpage);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(bpage->buf_fix_count > 0);
	ut_a(buf_page_in_file(bpage));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	if (must_read) {
		/* Let us wait until the read operation
		completes */

		if (UNIV_UNLIKELY(trx && trx->take_stats))
		{
			ut_usectime(&sec, &ms);
			start_time = (ib_uint64_t)sec * 1000000 + ms;
		} else {
			start_time = 0;
		}
		for (;;) {
			enum buf_io_fix	io_fix;

			mutex_enter(block_mutex);
			io_fix = buf_page_get_io_fix(bpage);
			mutex_exit(block_mutex);

			if (io_fix == BUF_IO_READ) {

				os_thread_sleep(WAIT_FOR_READ);
			} else {
				break;
			}
		}
		if (UNIV_UNLIKELY(start_time != 0))
		{
			ut_usectime(&sec, &ms);
			finish_time = (ib_uint64_t)sec * 1000000 + ms;
			trx->io_reads_wait_timer += (ulint)(finish_time - start_time);
		}
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_page_get_space(bpage),
			    buf_page_get_page_no(bpage)) == 0);
#endif
	return(bpage);
}

/********************************************************************//**
Initialize some fields of a control block. */
UNIV_INLINE
void
buf_block_init_low(
/*===============*/
	buf_block_t*	block)	/*!< in: block to init */
{
	block->check_index_page_at_flush = FALSE;
	block->index		= NULL;

	block->n_hash_helps	= 0;
	block->n_fields		= 1;
	block->n_bytes		= 0;
	block->left_side	= TRUE;
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************************//**
Decompress a block.
@return	TRUE if successful */
UNIV_INTERN
ibool
buf_zip_decompress(
/*===============*/
	buf_block_t*	block,	/*!< in/out: block */
	ibool		check)	/*!< in: TRUE=verify the page checksum */
{
	const byte*	frame = block->page.zip.data;
	ulint		size = page_zip_get_size(&block->page.zip);

	ut_ad(buf_block_get_zip_size(block));
	ut_a(buf_block_get_space(block) != 0);

	if (UNIV_UNLIKELY(check && !page_zip_verify_checksum(frame, size))) {

		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: compressed page checksum mismatch"
			" (space %u page %u): stored: %lu, crc32: %lu "
			"innodb: %lu, none: %lu\n",
			block->page.space, block->page.offset,
			mach_read_from_4(frame + FIL_PAGE_SPACE_OR_CHKSUM),
			page_zip_calc_checksum(frame, size,
					       SRV_CHECKSUM_ALGORITHM_CRC32),
			page_zip_calc_checksum(frame, size,
					       SRV_CHECKSUM_ALGORITHM_INNODB),
			page_zip_calc_checksum(frame, size,
					       SRV_CHECKSUM_ALGORITHM_NONE));
		return(FALSE);
	}

	switch (fil_page_get_type(frame)) {
	case FIL_PAGE_INDEX:
		if (page_zip_decompress(&block->page.zip,
					block->frame, TRUE)) {
			return(TRUE);
		}

		fprintf(stderr,
			"InnoDB: unable to decompress space %u page %u\n",
			block->page.space,
			block->page.offset);
		return(FALSE);

	case FIL_PAGE_TYPE_ALLOCATED:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* Copy to uncompressed storage. */
		memcpy(block->frame, frame,
		       buf_block_get_zip_size(block));
		return(TRUE);
	}

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: unknown compressed page"
		" type %lu\n",
		fil_page_get_type(frame));
	return(FALSE);
}

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Gets the block to whose frame the pointer is pointing to if found
in this buffer pool instance.
@return	pointer to block */
UNIV_INTERN
buf_block_t*
buf_block_align_instance(
/*=====================*/
 	buf_pool_t*	buf_pool,	/*!< in: buffer in which the block
					resides */
	const byte*	ptr)		/*!< in: pointer to a frame */
{
	buf_chunk_t*	chunk;
	ulint		i;

	/* TODO: protect buf_pool->chunks with a mutex (it will
	currently remain constant after buf_pool_init()) */
	for (chunk = buf_pool->chunks, i = buf_pool->n_chunks; i--; chunk++) {
		ulint	offs;

		if (UNIV_UNLIKELY(ptr < chunk->blocks->frame)) {

			continue;
		}
		/* else */

		offs = ptr - chunk->blocks->frame;

		offs >>= UNIV_PAGE_SIZE_SHIFT;

		if (UNIV_LIKELY(offs < chunk->size)) {
			buf_block_t*	block = &chunk->blocks[offs];

			/* The function buf_chunk_init() invokes
			buf_block_init() so that block[n].frame ==
			block->frame + n * UNIV_PAGE_SIZE.  Check it. */
			ut_ad(block->frame == page_align(ptr));
#ifdef UNIV_DEBUG
			/* A thread that updates these fields must
			hold one of the buf_pool mutexes, depending on the
			page state, and block->mutex.  Acquire
			only the latter. */
			mutex_enter(&block->mutex);

			switch (buf_block_get_state(block)) {
			case BUF_BLOCK_POOL_WATCH:
			case BUF_BLOCK_ZIP_PAGE:
			case BUF_BLOCK_ZIP_DIRTY:
				/* These types should only be used in
				the compressed buffer pool, whose
				memory is allocated from
				buf_pool->chunks, in UNIV_PAGE_SIZE
				blocks flagged as BUF_BLOCK_MEMORY. */
				ut_error;
				break;
			case BUF_BLOCK_NOT_USED:
			case BUF_BLOCK_READY_FOR_USE:
			case BUF_BLOCK_MEMORY:
				/* Some data structures contain
				"guess" pointers to file pages.  The
				file pages may have been freed and
				reused.  Do not complain. */
				break;
			case BUF_BLOCK_REMOVE_HASH:
				/* buf_LRU_block_remove_hashed_page()
				will overwrite the FIL_PAGE_OFFSET and
				FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID with
				0xff and set the state to
				BUF_BLOCK_REMOVE_HASH. */
				ut_ad(page_get_space_id(page_align(ptr))
				      == 0xffffffff);
				ut_ad(page_get_page_no(page_align(ptr))
				      == 0xffffffff);
				break;
			case BUF_BLOCK_FILE_PAGE: {
				ulint space =  page_get_space_id(page_align(ptr));
				ulint offset = page_get_page_no(page_align(ptr));

				if (block->page.space != space ||
					block->page.offset != offset) {
					ib_logf(IB_LOG_LEVEL_ERROR,
						"Corruption: Block space_id %lu != page space_id %lu or "
						"Block offset %lu != page offset %lu",
						(ulint)block->page.space, space,
						(ulint)block->page.offset, offset);
				}

				ut_ad(block->page.space
					== page_get_space_id(page_align(ptr)));
				ut_ad(block->page.offset
				      == page_get_page_no(page_align(ptr)));
				break;
			}
			}

			mutex_exit(&block->mutex);
#endif /* UNIV_DEBUG */

			return(block);
		}
	}

	return(NULL);
}

/*******************************************************************//**
Gets the block to whose frame the pointer is pointing to.
@return	pointer to block, never NULL */
UNIV_INTERN
buf_block_t*
buf_block_align(
/*============*/
	const byte*	ptr)	/*!< in: pointer to a frame */
{
	ulint		i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_block_t*	block;

		block = buf_block_align_instance(
			buf_pool_from_array(i), ptr);
		if (block) {
			return(block);
		}
	}

	/* The block should always be found. */
	ut_error;
	return(NULL);
}

/********************************************************************//**
Find out if a pointer belongs to a buf_block_t. It can be a pointer to
the buf_block_t itself or a member of it. This functions checks one of
the buffer pool instances.
@return	TRUE if ptr belongs to a buf_block_t struct */
static
ibool
buf_pointer_is_block_field_instance(
/*================================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	const void*	ptr)		/*!< in: pointer not dereferenced */
{
	const buf_chunk_t*		chunk	= buf_pool->chunks;
	const buf_chunk_t* const	echunk	= chunk + buf_pool->n_chunks;

	/* TODO: protect buf_pool->chunks with a mutex (it will
	currently remain constant after buf_pool_init()) */
	while (chunk < echunk) {
		if (ptr >= (void*) chunk->blocks
		    && ptr < (void*) (chunk->blocks + chunk->size)) {

			return(TRUE);
		}

		chunk++;
	}

	return(FALSE);
}

/********************************************************************//**
Find out if a pointer belongs to a buf_block_t. It can be a pointer to
the buf_block_t itself or a member of it
@return	TRUE if ptr belongs to a buf_block_t struct */
UNIV_INTERN
ibool
buf_pointer_is_block_field(
/*=======================*/
	const void*	ptr)	/*!< in: pointer not dereferenced */
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		ibool	found;

		found = buf_pointer_is_block_field_instance(
			buf_pool_from_array(i), ptr);
		if (found) {
			return(TRUE);
		}
	}

	return(FALSE);
}

/********************************************************************//**
Find out if a buffer block was created by buf_chunk_init().
@return	TRUE if "block" has been added to buf_pool->free by buf_chunk_init() */
static
ibool
buf_block_is_uncompressed(
/*======================*/
	buf_pool_t*		buf_pool,	/*!< in: buffer pool instance */
	const buf_block_t*	block)		/*!< in: pointer to block,
						not dereferenced */
{
	if ((((ulint) block) % sizeof *block) != 0) {
		/* The pointer should be aligned. */
		return(FALSE);
	}

	return(buf_pointer_is_block_field_instance(buf_pool, (void*) block));
}

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/********************************************************************//**
Return true if probe is enabled.
@return true if probe enabled. */
static
bool
buf_debug_execute_is_force_flush()
/*==============================*/
{
	DBUG_EXECUTE_IF("ib_buf_force_flush", return(true); );

	/* This is used during queisce testing, we want to ensure maximum
	buffering by the change buffer. */

	if (srv_ibuf_disable_background_merge) {
		return(true);
	}

	return(false);
}
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/**
Wait for the block to be read in.
@param block	The block to check
@param trx	Transaction to account the I/Os to */
static
void
buf_wait_for_read(buf_block_t* block, trx_t* trx)
{
	/* Note: For the PAGE_ATOMIC_REF_COUNT case:

	We are using the block->lock to check for IO state (and a dirty read).
	We set the IO_READ state under the protection of the hash_lock
	(and block->mutex). This is safe because another thread can only
	access the block (and check for IO state) after the block has been
	added to the page hashtable. */

	if (buf_block_get_io_fix_unlocked(block) == BUF_IO_READ) {

		ib_uint64_t	start_time;
		ulint		sec;
		ulint		ms;

		/* Wait until the read operation completes */

		ib_mutex_t*	mutex = buf_page_get_mutex(&block->page);

		if (UNIV_UNLIKELY(trx && trx->take_stats))
		{
			ut_usectime(&sec, &ms);
			start_time = (ib_uint64_t)sec * 1000000 + ms;
		} else {
			start_time = 0;
		}

		for (;;) {
			buf_io_fix	io_fix;

			mutex_enter(mutex);

			io_fix = buf_block_get_io_fix(block);

			mutex_exit(mutex);

			if (io_fix == BUF_IO_READ) {
				/* Wait by temporaly s-latch */
				rw_lock_s_lock(&block->lock);
				rw_lock_s_unlock(&block->lock);
			} else {
				break;
			}
		}

		if (UNIV_UNLIKELY(start_time != 0))
		{
			ut_usectime(&sec, &ms);
			ib_uint64_t finish_time
				= (ib_uint64_t)sec * 1000000 + ms;
			trx->io_reads_wait_timer
				+= (ulint)(finish_time - start_time);
		}

	}
}

/********************************************************************//**
This is the general function used to get access to a database page.
@return	pointer to the block or NULL */
UNIV_INTERN
buf_block_t*
buf_page_get_gen(
/*=============*/
	ulint		space,	/*!< in: space id */
	ulint		zip_size,/*!< in: compressed page size in bytes
				or 0 for uncompressed pages */
	ulint		offset,	/*!< in: page number */
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH */
	buf_block_t*	guess,	/*!< in: guessed block or NULL */
	ulint		mode,	/*!< in: BUF_GET, BUF_GET_IF_IN_POOL,
				BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or
				BUF_GET_IF_IN_POOL_OR_WATCH */
	const char*	file,	/*!< in: file name */
	ulint		line,	/*!< in: line where called */
	mtr_t*		mtr,	/*!< in: mini-transaction */
	dberr_t*        err)	/*!< out: error code */
{
	buf_block_t*	block;
	ulint		fold;
	unsigned	access_time;
	ulint		fix_type;
	prio_rw_lock_t*	hash_lock;
	ulint		retries = 0;
	trx_t*		trx = NULL;
	buf_block_t*	fix_block;
	ib_mutex_t*	fix_mutex = NULL;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	ut_ad(mtr);
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad((rw_latch == RW_S_LATCH)
	      || (rw_latch == RW_X_LATCH)
	      || (rw_latch == RW_NO_LATCH));

	if (err) {
		*err = DB_SUCCESS;
	}

#ifdef UNIV_DEBUG
	switch (mode) {
	case BUF_GET_NO_LATCH:
		ut_ad(rw_latch == RW_NO_LATCH);
		break;
	case BUF_GET:
	case BUF_GET_IF_IN_POOL:
	case BUF_PEEK_IF_IN_POOL:
	case BUF_GET_IF_IN_POOL_OR_WATCH:
	case BUF_GET_POSSIBLY_FREED:
		break;
	default:
		ut_error;
	}
#endif /* UNIV_DEBUG */
	ut_ad(zip_size == fil_space_get_zip_size(space));
	ut_ad(ut_is_2pow(zip_size));
#ifndef UNIV_LOG_DEBUG
	ut_ad(!ibuf_inside(mtr)
	      || ibuf_page_low(space, zip_size, offset,
			       FALSE, file, line, NULL));
#endif
	if (UNIV_UNLIKELY(innobase_get_slow_log())) {
		trx = innobase_get_trx();
	}
	buf_pool->stat.n_page_gets++;
	fold = buf_page_address_fold(space, offset);
	hash_lock = buf_page_hash_lock_get(buf_pool, fold);
loop:
	block = guess;

	rw_lock_s_lock(hash_lock);

	if (block != NULL) {

		/* If the guess is a compressed page descriptor that
		has been allocated by buf_page_alloc_descriptor(),
		it may have been freed by buf_relocate(). */

		if (!buf_block_is_uncompressed(buf_pool, block)
		    || offset != block->page.offset
		    || space != block->page.space
		    || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {

			/* Our guess was bogus or things have changed
			since. */
			block = guess = NULL;
		} else {
			ut_ad(!block->page.in_zip_hash);
		}
	}

	if (block == NULL) {
		block = (buf_block_t*) buf_page_hash_get_low(
			buf_pool, space, offset, fold);
	}

	if (!block || buf_pool_watch_is_sentinel(buf_pool, &block->page)) {
		rw_lock_s_unlock(hash_lock);
		block = NULL;
	}

	if (block == NULL) {
		buf_page_t*	bpage=NULL;

		/* Page not in buf_pool: needs to be read from file */

		if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
			mutex_enter(&buf_pool->LRU_list_mutex);
			rw_lock_x_lock(hash_lock);
			block = (buf_block_t*) buf_pool_watch_set(
				space, offset, fold);
			mutex_exit(&buf_pool->LRU_list_mutex);

			if (UNIV_LIKELY_NULL(block)) {
				/* We can release hash_lock after we
				increment the fix count to make
				sure that no state change takes place. */
				fix_block = block;
				buf_block_fix(fix_block);

				/* Now safe to release page_hash mutex */
				rw_lock_x_unlock(hash_lock);
				goto got_block;
			}

			rw_lock_x_unlock(hash_lock);
		}

		if (mode == BUF_GET_IF_IN_POOL
		    || mode == BUF_PEEK_IF_IN_POOL
		    || mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
#ifdef UNIV_SYNC_DEBUG
			ut_ad(!rw_lock_own(hash_lock, RW_LOCK_EX));
			ut_ad(!rw_lock_own(hash_lock, RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */
			return(NULL);
		}

		if (buf_read_page(space, zip_size, offset, trx, &bpage)) {
			buf_read_ahead_random(space, zip_size, offset,
					      ibuf_inside(mtr), trx);

			retries = 0;
		} else if (retries < BUF_PAGE_READ_MAX_RETRIES) {
			++retries;

			bool corrupted = true;

			if (bpage) {
				corrupted = buf_page_check_corrupt(bpage);
			}

			/* Do not try again for encrypted pages */
			if (!corrupted) {
				ib_mutex_t* pmutex = buf_page_get_mutex(bpage);
				mutex_enter(&buf_pool->LRU_list_mutex);
				mutex_enter(pmutex);

				ut_ad(buf_pool->n_pend_reads > 0);
				os_atomic_decrement_ulint(&buf_pool->n_pend_reads, 1);
				buf_page_set_io_fix(bpage, BUF_IO_NONE);

				if (!buf_LRU_free_page(bpage, true)) {
					mutex_exit(&buf_pool->LRU_list_mutex);
				}

				mutex_exit(pmutex);
				rw_lock_x_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_READ);

				if (err) {
					*err = DB_DECRYPTION_FAILED;
				}

				return (NULL);
			}

			DBUG_EXECUTE_IF(
				"innodb_page_corruption_retries",
				retries = BUF_PAGE_READ_MAX_RETRIES;
			);
		} else {
			bool corrupted = true;

			if (bpage) {
				corrupted = buf_page_check_corrupt(bpage);
			}

			if (corrupted) {
				fprintf(stderr, "InnoDB: Error: Unable"
					" to read tablespace %lu page no"
					" %lu into the buffer pool after"
					" %lu attempts\n"
					"InnoDB: The most probable cause"
					" of this error may be that the"
					" table has been corrupted.\n"
					"InnoDB: You can try to fix this"
					" problem by using"
					" innodb_force_recovery.\n"
					"InnoDB: Please see reference manual"
					" for more details.\n"
					"InnoDB: Aborting...\n",
					space, offset,
					BUF_PAGE_READ_MAX_RETRIES);

				ut_error;
			} else {
				ib_mutex_t* pmutex = buf_page_get_mutex(bpage);
				mutex_enter(&buf_pool->LRU_list_mutex);
				mutex_enter(pmutex);

				ut_ad(buf_pool->n_pend_reads > 0);
				os_atomic_decrement_ulint(&buf_pool->n_pend_reads, 1);
				buf_page_set_io_fix(bpage, BUF_IO_NONE);

				if (!buf_LRU_free_page(bpage, true)) {
					mutex_exit(&buf_pool->LRU_list_mutex);
				}

				mutex_exit(pmutex);
				rw_lock_x_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_READ);

				if (err) {
					*err = DB_DECRYPTION_FAILED;
				}

				return (NULL);
			}
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(++buf_dbg_counter % 5771 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		goto loop;
	} else {
		fix_block = block;
	}

	buf_block_fix(fix_block);

	/* Now safe to release page_hash mutex */
	rw_lock_s_unlock(hash_lock);

got_block:

	fix_mutex = buf_page_get_mutex(&fix_block->page);

	ut_ad(page_zip_get_size(&block->page.zip) == zip_size);

	if (mode == BUF_GET_IF_IN_POOL || mode == BUF_PEEK_IF_IN_POOL) {

		bool	must_read;

		{
			buf_page_t*	fix_page = &fix_block->page;

			mutex_enter(fix_mutex);

			buf_io_fix	io_fix = buf_page_get_io_fix(fix_page);

			must_read = (io_fix == BUF_IO_READ);

			mutex_exit(fix_mutex);
		}

		if (must_read) {
			/* The page is being read to buffer pool,
			but we cannot wait around for the read to
			complete. */
			buf_block_unfix(fix_block);

			return(NULL);
		}
	}

	if (UNIV_UNLIKELY(fix_block->page.is_corrupt &&
			  srv_pass_corrupt_table <= 1)) {

		buf_block_unfix(fix_block);

		return(NULL);
	}

	switch(buf_block_get_state(fix_block)) {
		buf_page_t*	bpage;

	case BUF_BLOCK_FILE_PAGE:
		ut_ad(fix_mutex != &buf_pool->zip_mutex);
		break;

	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		if (mode == BUF_PEEK_IF_IN_POOL) {
			/* This mode is only used for dropping an
			adaptive hash index.  There cannot be an
			adaptive hash index for a compressed-only
			page, so do not bother decompressing the page. */
			buf_block_unfix(fix_block);

			return(NULL);
		}

		bpage = &block->page;
		ut_ad(fix_mutex == &buf_pool->zip_mutex);

		/* Note: We have already buffer fixed this block. */
		if (bpage->buf_fix_count > 1
		    || buf_page_get_io_fix_unlocked(bpage) != BUF_IO_NONE) {

			/* This condition often occurs when the buffer
			is not buffer-fixed, but I/O-fixed by
			buf_page_init_for_read(). */

			buf_block_unfix(fix_block);

			/* The block is buffer-fixed or I/O-fixed.
			Try again later. */
			os_thread_sleep(WAIT_FOR_READ);

			goto loop;
		}

		/* Buffer-fix the block so that it cannot be evicted
		or relocated while we are attempting to allocate an
		uncompressed page. */

		/* Allocate an uncompressed page. */

		block = buf_LRU_get_free_block(buf_pool);

		mutex_enter(&buf_pool->LRU_list_mutex);

		rw_lock_x_lock(hash_lock);

		/* Buffer-fixing prevents the page_hash from changing. */
		ut_ad(bpage == buf_page_hash_get_low(
			      buf_pool, space, offset, fold));

		buf_block_mutex_enter(block);

		mutex_enter(&buf_pool->zip_mutex);

		ut_ad(fix_block->page.buf_fix_count > 0);

#ifdef PAGE_ATOMIC_REF_COUNT
		os_atomic_decrement_uint32(&fix_block->page.buf_fix_count, 1);
#else
		--fix_block->page.buf_fix_count;
#endif /* PAGE_ATOMIC_REF_COUNT */

		fix_block = block;

		if (bpage->buf_fix_count > 0
		    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

			mutex_exit(&buf_pool->zip_mutex);
			/* The block was buffer-fixed or I/O-fixed while
			buf_pool->mutex was not held by this thread.
			Free the block that was allocated and retry.
			This should be extremely unlikely, for example,
			if buf_page_get_zip() was invoked. */

			buf_LRU_block_free_non_file_page(block);
			mutex_exit(&buf_pool->LRU_list_mutex);
			rw_lock_x_unlock(hash_lock);
			buf_block_mutex_exit(block);

			/* Try again */
			goto loop;
		}

		/* Move the compressed page from bpage to block,
		and uncompress it. */

		/* Note: this is the uncompressed block and it is not
		accessible by other threads yet because it is not in
		any list or hash table */
		buf_relocate(bpage, &block->page);

		buf_block_init_low(block);

		/* Set after relocate(). */
		block->page.buf_fix_count = 1;

		block->lock_hash_val = lock_rec_hash(space, offset);

		UNIV_MEM_DESC(&block->page.zip.data,
			page_zip_get_size(&block->page.zip));

		if (buf_page_get_state(&block->page) == BUF_BLOCK_ZIP_PAGE) {
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
			UT_LIST_REMOVE(list, buf_pool->zip_clean,
				       &block->page);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
			ut_ad(!block->page.in_flush_list);
		} else {
			/* Relocate buf_pool->flush_list. */
			buf_flush_relocate_on_flush_list(bpage, &block->page);
		}

		/* Buffer-fix, I/O-fix, and X-latch the block
		for the duration of the decompression.
		Also add the block to the unzip_LRU list. */
		block->page.state = BUF_BLOCK_FILE_PAGE;

		/* Insert at the front of unzip_LRU list */
		buf_unzip_LRU_add_block(block, FALSE);

		mutex_exit(&buf_pool->LRU_list_mutex);

		buf_block_set_io_fix(block, BUF_IO_READ);
		rw_lock_x_lock_inline(&block->lock, 0, file, line);

		UNIV_MEM_INVALID(bpage, sizeof *bpage);

		rw_lock_x_unlock(hash_lock);

		os_atomic_increment_ulint(&buf_pool->n_pend_unzip, 1);

		mutex_exit(&buf_pool->zip_mutex);

		access_time = buf_page_is_accessed(&block->page);

		buf_block_mutex_exit(block);

		buf_page_free_descriptor(bpage);

		/* Decompress the page while not holding
		any buf_pool or block->mutex. */

		/* Page checksum verification is already done when
		the page is read from disk. Hence page checksum
		verification is not necessary when decompressing the page. */
		{
			bool	success = buf_zip_decompress(block, FALSE);
			ut_a(success);
		}

		if (!recv_no_ibuf_operations) {
			if (access_time) {
#ifdef UNIV_IBUF_COUNT_DEBUG
				ut_a(ibuf_count_get(space, offset) == 0);
#endif /* UNIV_IBUF_COUNT_DEBUG */
			} else {
				ibuf_merge_or_delete_for_page(
					block, space, offset, zip_size, TRUE);
			}
		}

		/* Unfix and unlatch the block. */
		buf_block_mutex_enter(fix_block);

		buf_block_set_io_fix(fix_block, BUF_IO_NONE);

		buf_block_mutex_exit(fix_block);

		os_atomic_decrement_ulint(&buf_pool->n_pend_unzip, 1);

		rw_lock_x_unlock(&block->lock);

		break;

	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	ut_ad(block == fix_block);
	ut_ad(fix_block->page.buf_fix_count > 0);

#ifdef UNIV_SYNC_DEBUG
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_EX));
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */

	ut_ad(buf_block_get_state(fix_block) == BUF_BLOCK_FILE_PAGE);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG

	if ((mode == BUF_GET_IF_IN_POOL || mode == BUF_GET_IF_IN_POOL_OR_WATCH)
	    && (ibuf_debug || buf_debug_execute_is_force_flush())) {

		/* Try to evict the block from the buffer pool, to use the
		insert buffer (change buffer) as much as possible. */

		mutex_enter(&buf_pool->LRU_list_mutex);

		buf_block_unfix(fix_block);

		/* Now we are only holding the buf_pool->LRU_list_mutex,
		not block->mutex or hash_lock. Blocks cannot be
		relocated or enter or exit the buf_pool while we
		are holding the buf_pool->LRU_list_mutex. */

		fix_mutex = buf_page_get_mutex(&fix_block->page);
		mutex_enter(fix_mutex);

		if (buf_LRU_free_page(&fix_block->page, true)) {

			mutex_exit(fix_mutex);

			if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
				mutex_enter(&buf_pool->LRU_list_mutex);
				rw_lock_x_lock(hash_lock);

				/* Set the watch, as it would have
				been set if the page were not in the
				buffer pool in the first place. */
				block = (buf_block_t*) buf_pool_watch_set(
					space, offset, fold);
				mutex_exit(&buf_pool->LRU_list_mutex);
			} else {
				rw_lock_x_lock(hash_lock);
				block = (buf_block_t*) buf_page_hash_get_low(
					buf_pool, space, offset, fold);
			}

			rw_lock_x_unlock(hash_lock);

			if (block != NULL) {
				/* Either the page has been read in or
				a watch was set on that in the window
				where we released the buf_pool::mutex
				and before we acquire the hash_lock
				above. Try again. */
				guess = block;
				goto loop;
			}

			fprintf(stderr,
				"innodb_change_buffering_debug evict %u %u\n",
				(unsigned) space, (unsigned) offset);
			return(NULL);
		}

		if (buf_flush_page_try(buf_pool, fix_block)) {
			fprintf(stderr,
				"innodb_change_buffering_debug flush %u %u\n",
				(unsigned) space, (unsigned) offset);
			guess = fix_block;
			goto loop;
		}

		mutex_exit(&buf_pool->LRU_list_mutex);

		buf_block_mutex_exit(fix_block);

		buf_block_fix(fix_block);

		/* Failed to evict the page; change it directly */
	}
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

	ut_ad(fix_block->page.buf_fix_count > 0);

#ifdef UNIV_SYNC_DEBUG
	/* We have already buffer fixed the page, and we are committed to
	returning this page to the caller. Register for debugging. */
	{
		ibool	ret;
		ret = rw_lock_s_lock_nowait(&fix_block->debug_latch, file, line);
		ut_a(ret);
	}
#endif /* UNIV_SYNC_DEBUG */

#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	ut_a(mode == BUF_GET_POSSIBLY_FREED
	     || !fix_block->page.file_page_was_freed);
#endif
	/* Check if this is the first access to the page */
	access_time = buf_page_is_accessed(&fix_block->page);

	/* This is a heuristic and we don't care about ordering issues. */
	if (access_time == 0) {
		buf_block_mutex_enter(fix_block);

		buf_page_set_accessed(&fix_block->page);

		buf_block_mutex_exit(fix_block);
	}

	if (mode != BUF_PEEK_IF_IN_POOL) {
		buf_page_make_young_if_needed(&fix_block->page);
	}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(fix_block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(fix_block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#ifdef PAGE_ATOMIC_REF_COUNT
	/* We have to wait here because the IO_READ state was set
	under the protection of the hash_lock and the block->mutex
	but not the block->lock. */
	buf_wait_for_read(fix_block, trx);
#endif /* PAGE_ATOMIC_REF_COUNT */

	switch (rw_latch) {
	case RW_NO_LATCH:

#ifndef PAGE_ATOMIC_REF_COUNT
		buf_wait_for_read(fix_block, trx);
#endif /* !PAGE_ATOMIC_REF_COUNT */

		fix_type = MTR_MEMO_BUF_FIX;
		break;

	case RW_S_LATCH:
		rw_lock_s_lock_inline(&fix_block->lock, 0, file, line);

		fix_type = MTR_MEMO_PAGE_S_FIX;
		break;

	default:
		ut_ad(rw_latch == RW_X_LATCH);
		rw_lock_x_lock_inline(&fix_block->lock, 0, file, line);

		fix_type = MTR_MEMO_PAGE_X_FIX;
		break;
	}

	mtr_memo_push(mtr, fix_block, fix_type);

	if (mode != BUF_PEEK_IF_IN_POOL && !access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(
			space, zip_size, offset, ibuf_inside(mtr), trx);
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(fix_block),
			    buf_block_get_page_no(fix_block)) == 0);
#endif
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_EX));
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */

	if (UNIV_UNLIKELY(trx && trx->take_stats)) {
		_increment_page_get_statistics(block, trx);
	}

	return(fix_block);
}

/********************************************************************//**
This is the general function used to get optimistic access to a database
page.
@return	TRUE if success */
UNIV_INTERN
ibool
buf_page_optimistic_get(
/*====================*/
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/*!< in: guessed buffer block */
	ib_uint64_t	modify_clock,/*!< in: modify clock value */
	const char*	file,	/*!< in: file name */
	ulint		line,	/*!< in: line where called */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	buf_pool_t*	buf_pool;
	unsigned	access_time;
	ibool		success;
	ulint		fix_type;
	trx_t*		trx = NULL;

	ut_ad(block);
	ut_ad(mtr);
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	mutex_enter(&block->mutex);

	if (UNIV_UNLIKELY(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE)) {

		mutex_exit(&block->mutex);

		return(FALSE);
	}

	buf_block_buf_fix_inc(block, file, line);

	access_time = buf_page_is_accessed(&block->page);

	buf_page_set_accessed(&block->page);

	mutex_exit(&block->mutex);

	buf_page_make_young_if_needed(&block->page);

	ut_ad(!ibuf_inside(mtr)
	      || ibuf_page(buf_block_get_space(block),
			   buf_block_get_zip_size(block),
			   buf_block_get_page_no(block), NULL));

	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_nowait(&(block->lock),
						file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait_inline(&(block->lock),
							    file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	if (UNIV_UNLIKELY(!success)) {
		buf_block_buf_fix_dec(block);

		return(FALSE);
	}

	if (UNIV_UNLIKELY(modify_clock != block->modify_clock)) {
		buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

		if (rw_latch == RW_S_LATCH) {
			rw_lock_s_unlock(&(block->lock));
		} else {
			rw_lock_x_unlock(&(block->lock));
		}

		buf_block_buf_fix_dec(block);

		return(FALSE);
	}

	mtr_memo_push(mtr, block, fix_type);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	mutex_enter(&block->mutex);
	ut_a(!block->page.file_page_was_freed);
	mutex_exit(&block->mutex);
#endif
	if (UNIV_UNLIKELY(innobase_get_slow_log())) {
		trx = innobase_get_trx();
	}

	if (!access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(buf_block_get_space(block),
				      buf_block_get_zip_size(block),
				      buf_block_get_page_no(block),
				      ibuf_inside(mtr), trx);
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif
	buf_pool = buf_pool_from_block(block);
	buf_pool->stat.n_page_gets++;

	if (UNIV_UNLIKELY(trx && trx->take_stats)) {
		_increment_page_get_statistics(block, trx);
	}
	return(TRUE);
}

/********************************************************************//**
This is used to get access to a known database page, when no waiting can be
done. For example, if a search in an adaptive hash index leads us to this
frame.
@return	TRUE if success */
UNIV_INTERN
ibool
buf_page_get_known_nowait(
/*======================*/
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/*!< in: the known page */
	ulint		mode,	/*!< in: BUF_MAKE_YOUNG or BUF_KEEP_OLD */
	const char*	file,	/*!< in: file name */
	ulint		line,	/*!< in: line where called */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	buf_pool_t*	buf_pool;
	ibool		success;
	ulint		fix_type;
	trx_t*		trx = NULL;

	ut_ad(mtr);
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	mutex_enter(&block->mutex);

	if (buf_block_get_state(block) == BUF_BLOCK_REMOVE_HASH) {
		/* Another thread is just freeing the block from the LRU list
		of the buffer pool: do not try to access this page; this
		attempt to access the page can only come through the hash
		index because when the buffer block state is ..._REMOVE_HASH,
		we have already removed it from the page address hash table
		of the buffer pool. */

		mutex_exit(&block->mutex);

		return(FALSE);
	}

	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	buf_block_buf_fix_inc(block, file, line);

	buf_page_set_accessed(&block->page);

	mutex_exit(&block->mutex);

	buf_pool = buf_pool_from_block(block);

	if (mode == BUF_MAKE_YOUNG) {
		buf_page_make_young_if_needed(&block->page);
	}

	ut_ad(!ibuf_inside(mtr) || mode == BUF_KEEP_OLD);

	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_nowait(&(block->lock),
						file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait_inline(&(block->lock),
							    file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	if (!success) {
		buf_block_buf_fix_dec(block);

		return(FALSE);
	}

	mtr_memo_push(mtr, block, fix_type);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	if (mode != BUF_KEEP_OLD) {
		/* If mode == BUF_KEEP_OLD, we are executing an I/O
		completion routine.  Avoid a bogus assertion failure
		when ibuf_merge_or_delete_for_page() is processing a
		page that was just freed due to DROP INDEX, or
		deleting a record from SYS_INDEXES. This check will be
		skipped in recv_recover_page() as well. */

		mutex_enter(&block->mutex);
		ut_a(!block->page.file_page_was_freed);
		mutex_exit(&block->mutex);
	}
#endif

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a((mode == BUF_KEEP_OLD)
	     || (ibuf_count_get(buf_block_get_space(block),
				buf_block_get_page_no(block)) == 0));
#endif
	buf_pool->stat.n_page_gets++;

	if (UNIV_UNLIKELY(innobase_get_slow_log())) {

		trx = innobase_get_trx();
		if (trx != NULL && trx->take_stats) {

			_increment_page_get_statistics(block, trx);
		}
	}

	return(TRUE);
}

/*******************************************************************//**
Given a tablespace id and page number tries to get that page. If the
page is not in the buffer pool it is not loaded and NULL is returned.
Suitable for using when holding the lock_sys_t::mutex.
@return	pointer to a page or NULL */
UNIV_INTERN
buf_block_t*
buf_page_try_get_func(
/*==================*/
	ulint		space_id,/*!< in: tablespace id */
	ulint		page_no,/*!< in: page number */
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	bool		possibly_freed,
	const char*	file,	/*!< in: file name */
	ulint		line,	/*!< in: line where called */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	buf_block_t*	block;
	ibool		success;
	ulint		fix_type;
	buf_pool_t*	buf_pool = buf_pool_get(space_id, page_no);
	prio_rw_lock_t*	hash_lock;

	ut_ad(mtr);
	ut_ad(mtr->state == MTR_ACTIVE);

	block = buf_block_hash_get_s_locked(buf_pool, space_id,
					    page_no, &hash_lock);

	if (!block || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
		if (block) {
			rw_lock_s_unlock(hash_lock);
		}
		return(NULL);
	}

	ut_ad(!buf_pool_watch_is_sentinel(buf_pool, &block->page));

	mutex_enter(&block->mutex);
	rw_lock_s_unlock(hash_lock);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_a(buf_block_get_space(block) == space_id);
	ut_a(buf_block_get_page_no(block) == page_no);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_block_buf_fix_inc(block, file, line);
	mutex_exit(&block->mutex);

	if (rw_latch == RW_S_LATCH) {
		fix_type = MTR_MEMO_PAGE_S_FIX;
		success = rw_lock_s_lock_nowait(&block->lock, file, line);
	} else {
		success = false;
	}

	if (!success) {
		/* Let us try to get an X-latch. If the current thread
		is holding an X-latch on the page, we cannot get an
		S-latch. */

		fix_type = MTR_MEMO_PAGE_X_FIX;
		success = rw_lock_x_lock_func_nowait_inline(&block->lock,
							    file, line);
	}

	if (!success) {
		buf_block_buf_fix_dec(block);

		return(NULL);
	}

	mtr_memo_push(mtr, block, fix_type);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	if (!possibly_freed) {
		mutex_enter(&block->mutex);
		ut_a(!block->page.file_page_was_freed);
		mutex_exit(&block->mutex);
	}
#endif /* UNIV_DEBUG_FILE_ACCESSES || UNIV_DEBUG */
	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	buf_pool->stat.n_page_gets++;

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif

	return(block);
}

/********************************************************************//**
Initialize some fields of a control block. */
UNIV_INLINE
void
buf_page_init_low(
/*==============*/
	buf_page_t*	bpage)	/*!< in: block to init */
{
	bpage->flush_type = BUF_FLUSH_LRU;
	bpage->io_fix = BUF_IO_NONE;
	bpage->buf_fix_count = 0;
	bpage->freed_page_clock = 0;
	bpage->access_time = 0;
	bpage->newest_modification = 0;
	bpage->oldest_modification = 0;
	bpage->write_size = 0;
	bpage->key_version = 0;
	bpage->stored_checksum = BUF_NO_CHECKSUM_MAGIC;
	bpage->calculated_checksum = BUF_NO_CHECKSUM_MAGIC;
	bpage->page_encrypted = false;
	bpage->page_compressed = false;
	bpage->encrypted = false;
	bpage->real_size = 0;

	HASH_INVALIDATE(bpage, hash);
	bpage->is_corrupt = FALSE;
#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
	bpage->file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES || UNIV_DEBUG */
}

/********************************************************************//**
Inits a page to the buffer buf_pool. */
static MY_ATTRIBUTE((nonnull))
void
buf_page_init(
/*==========*/
	buf_pool_t*	buf_pool,/*!< in/out: buffer pool */
	ulint		space,	/*!< in: space id */
	ulint		offset,	/*!< in: offset of the page within space
				in units of a page */
	ulint		fold,	/*!< in: buf_page_address_fold(space,offset) */
	ulint		zip_size,/*!< in: compressed page size, or 0 */
	buf_block_t*	block)	/*!< in/out: block to init */
{
	buf_page_t*	hash_page;

	ut_ad(buf_pool == buf_pool_get(space, offset));

	ut_ad(mutex_own(&(block->mutex)));
	ut_a(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE);

#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(buf_page_hash_lock_get(buf_pool, fold),
			  RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	/* Set the state of the block */
	buf_block_set_file_page(block, space, offset);

#ifdef UNIV_DEBUG_VALGRIND
	if (!space) {
		/* Silence valid Valgrind warnings about uninitialized
		data being written to data files.  There are some unused
		bytes on some pages that InnoDB does not initialize. */
		UNIV_MEM_VALID(block->frame, UNIV_PAGE_SIZE);
	}
#endif /* UNIV_DEBUG_VALGRIND */

	buf_block_init_low(block);

	block->lock_hash_val = lock_rec_hash(space, offset);

	buf_page_init_low(&block->page);

	/* Insert into the hash table of file pages */

	hash_page = buf_page_hash_get_low(buf_pool, space, offset, fold);

	if (hash_page == NULL) {
		/* Block not found in the hash table */
	} else if (buf_pool_watch_is_sentinel(buf_pool, hash_page)) {

		mutex_enter(&buf_pool->zip_mutex);

		ib_uint32_t	buf_fix_count = hash_page->buf_fix_count;

		ut_a(buf_fix_count > 0);

#ifdef PAGE_ATOMIC_REF_COUNT
		os_atomic_increment_uint32(
			&block->page.buf_fix_count, buf_fix_count);
#else
		block->page.buf_fix_count += ulint(buf_fix_count);
#endif /* PAGE_ATOMIC_REF_COUNT */

		buf_pool_watch_remove(buf_pool, fold, hash_page);

		mutex_exit(&buf_pool->zip_mutex);

	} else {
		fprintf(stderr,
			"InnoDB: Error: page %lu %lu already found"
			" in the hash table: %p, %p\n",
			space,
			offset,
			(const void*) hash_page, (const void*) block);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		mutex_exit(&block->mutex);
		buf_print();
		buf_LRU_print();
		buf_validate();
		buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		ut_error;
	}

	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_page_hash);
	ut_d(block->page.in_page_hash = TRUE);

	HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, fold, &block->page);

	if (zip_size) {
		page_zip_set_size(&block->page.zip, zip_size);
	}
}

/********************************************************************//**
Function which inits a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if we specify to read only ibuf pages and the page is not an ibuf page, or
(3) if the space is deleted or being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later.
@return	pointer to the block or NULL */
UNIV_INTERN
buf_page_t*
buf_page_init_for_read(
/*===================*/
	dberr_t*	err,	/*!< out: DB_SUCCESS or DB_TABLESPACE_DELETED */
	ulint		mode,	/*!< in: BUF_READ_IBUF_PAGES_ONLY, ... */
	ulint		space,	/*!< in: space id */
	ulint		zip_size,/*!< in: compressed page size, or 0 */
	ibool		unzip,	/*!< in: TRUE=request uncompressed page */
	ib_int64_t	tablespace_version,
				/*!< in: prevents reading from a wrong
				version of the tablespace in case we have done
				DISCARD + IMPORT */
	ulint		offset)	/*!< in: page number */
{
	buf_block_t*	block;
	buf_page_t*	bpage	= NULL;
	buf_page_t*	watch_page;
	prio_rw_lock_t*	hash_lock;
	mtr_t		mtr;
	ulint		fold;
	ibool		lru;
	void*		data;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	ut_ad(buf_pool);

	*err = DB_SUCCESS;

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {
		/* It is a read-ahead within an ibuf routine */

		ut_ad(!ibuf_bitmap_page(zip_size, offset));

		ibuf_mtr_start(&mtr);

		if (!recv_no_ibuf_operations
		    && !ibuf_page(space, zip_size, offset, &mtr)) {

			ibuf_mtr_commit(&mtr);

			return(NULL);
		}
	} else {
		ut_ad(mode == BUF_READ_ANY_PAGE);
	}

	if (zip_size && !unzip && !recv_recovery_is_on()) {
		block = NULL;
	} else {
		block = buf_LRU_get_free_block(buf_pool);
		ut_ad(block);
		ut_ad(buf_pool_from_block(block) == buf_pool);
	}

	fold = buf_page_address_fold(space, offset);
	hash_lock = buf_page_hash_lock_get(buf_pool, fold);

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);
	rw_lock_x_lock(hash_lock);

	watch_page = buf_page_hash_get_low(buf_pool, space, offset, fold);
	if (watch_page && !buf_pool_watch_is_sentinel(buf_pool, watch_page)) {
		/* The page is already in the buffer pool. */
		watch_page = NULL;
err_exit:
		mutex_exit(&buf_pool->LRU_list_mutex);
		rw_lock_x_unlock(hash_lock);
		if (block) {
			mutex_enter(&block->mutex);
			buf_LRU_block_free_non_file_page(block);
			mutex_exit(&block->mutex);
		}

		bpage = NULL;
		goto func_exit;
	}

	if (fil_tablespace_deleted_or_being_deleted_in_mem(
		    space, tablespace_version)) {
		/* The page belongs to a space which has been
		deleted or is being deleted. */
		*err = DB_TABLESPACE_DELETED;

		goto err_exit;
	}

	if (block) {
		bpage = &block->page;

		mutex_enter(&block->mutex);

		ut_ad(buf_pool_from_bpage(bpage) == buf_pool);

		buf_page_init(buf_pool, space, offset, fold, zip_size, block);

#ifdef PAGE_ATOMIC_REF_COUNT
		/* Note: We set the io state without the protection of
		the block->lock. This is because other threads cannot
		access this block unless it is in the hash table. */

		buf_page_set_io_fix(bpage, BUF_IO_READ);
#endif /* PAGE_ATOMIC_REF_COUNT */

		/* The block must be put to the LRU list, to the old blocks */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);
		mutex_exit(&buf_pool->LRU_list_mutex);

		/* We set a pass-type x-lock on the frame because then
		the same thread which called for the read operation
		(and is running now at this point of code) can wait
		for the read to complete by waiting for the x-lock on
		the frame; if the x-lock were recursive, the same
		thread would illegally get the x-lock before the page
		read is completed.  The x-lock is cleared by the
		io-handler thread. */

		rw_lock_x_lock_gen(&block->lock, BUF_IO_READ);

#ifndef PAGE_ATOMIC_REF_COUNT
		buf_page_set_io_fix(bpage, BUF_IO_READ);
#endif /* !PAGE_ATOMIC_REF_COUNT */

		rw_lock_x_unlock(hash_lock);

		if (zip_size) {
			/* buf_pool->LRU_list_mutex may be released and
			reacquired by buf_buddy_alloc().  Thus, we
			must release block->mutex in order not to
			break the latching order in the reacquisition
			of buf_pool->LRU_list_mutex.  We also must defer this
			operation until after the block descriptor has
			been added to buf_pool->LRU and
			buf_pool->page_hash. */
			mutex_exit(&block->mutex);
			mutex_enter(&buf_pool->LRU_list_mutex);
			data = buf_buddy_alloc(buf_pool, zip_size, &lru);
			mutex_enter(&block->mutex);
			block->page.zip.data = (page_zip_t*) data;

			/* To maintain the invariant
			block->in_unzip_LRU_list
			== buf_page_belongs_to_unzip_LRU(&block->page)
			we have to add this block to unzip_LRU
			after block->page.zip.data is set. */
			ut_ad(buf_page_belongs_to_unzip_LRU(&block->page));
			buf_unzip_LRU_add_block(block, TRUE);
			mutex_exit(&buf_pool->LRU_list_mutex);
		}

		mutex_exit(&block->mutex);
	} else {
		rw_lock_x_unlock(hash_lock);

		/* The compressed page must be allocated before the
		control block (bpage), in order to avoid the
		invocation of buf_buddy_relocate_block() on
		uninitialized data. */
		data = buf_buddy_alloc(buf_pool, zip_size, &lru);

		rw_lock_x_lock(hash_lock);

		/* We must check the page_hash again, as it may have been
		modified. */

		watch_page = buf_page_hash_get_low(
				buf_pool, space, offset, fold);

		if (UNIV_UNLIKELY(watch_page
			    && !buf_pool_watch_is_sentinel(buf_pool,
							   watch_page))) {

			/* The block was added by some other thread. */
			mutex_exit(&buf_pool->LRU_list_mutex);
			rw_lock_x_unlock(hash_lock);
			watch_page = NULL;
			buf_buddy_free(buf_pool, data, zip_size);

			bpage = NULL;
			goto func_exit;
		}

		bpage = buf_page_alloc_descriptor();

		/* Initialize the buf_pool pointer. */
		bpage->buf_pool_index = buf_pool_index(buf_pool);

		page_zip_des_init(&bpage->zip);
		page_zip_set_size(&bpage->zip, zip_size);
		bpage->zip.data = (page_zip_t*) data;

		bpage->slot = NULL;

		mutex_enter(&buf_pool->zip_mutex);
		UNIV_MEM_DESC(bpage->zip.data,
			      page_zip_get_size(&bpage->zip));

		buf_page_init_low(bpage);

		bpage->state	= BUF_BLOCK_ZIP_PAGE;
		bpage->space	= static_cast<ib_uint32_t>(space);
		bpage->offset	= static_cast<ib_uint32_t>(offset);

#ifdef UNIV_DEBUG
		bpage->in_page_hash = FALSE;
		bpage->in_zip_hash = FALSE;
		bpage->in_flush_list = FALSE;
		bpage->in_free_list = FALSE;
		bpage->in_LRU_list = FALSE;
#endif /* UNIV_DEBUG */

		ut_d(bpage->in_page_hash = TRUE);

		if (watch_page != NULL) {

			/* Preserve the reference count. */
			ib_uint32_t	buf_fix_count;

			buf_fix_count = watch_page->buf_fix_count;

			ut_a(buf_fix_count > 0);

			ut_ad(buf_own_zip_mutex_for_page(bpage));

#ifdef PAGE_ATOMIC_REF_COUNT
			os_atomic_increment_uint32(
				&bpage->buf_fix_count, buf_fix_count);
#else
			bpage->buf_fix_count += buf_fix_count;
#endif /* PAGE_ATOMIC_REF_COUNT */

			ut_ad(buf_pool_watch_is_sentinel(buf_pool, watch_page));
			buf_pool_watch_remove(buf_pool, fold, watch_page);
		}

		HASH_INSERT(buf_page_t, hash, buf_pool->page_hash, fold,
			    bpage);

		rw_lock_x_unlock(hash_lock);

		/* The block must be put to the LRU list, to the old blocks.
		The zip_size is already set into the page zip */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		mutex_exit(&buf_pool->LRU_list_mutex);

		buf_page_set_io_fix(bpage, BUF_IO_READ);

		mutex_exit(&buf_pool->zip_mutex);
	}

	os_atomic_increment_ulint(&buf_pool->n_pend_reads, 1);
func_exit:

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {

		ibuf_mtr_commit(&mtr);
	}


#ifdef UNIV_SYNC_DEBUG
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_EX));
	ut_ad(!rw_lock_own(hash_lock, RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */

	ut_ad(!bpage || buf_page_in_file(bpage));
	return(bpage);
}

/********************************************************************//**
Initializes a page to the buffer buf_pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen).
@return	pointer to the block, page bufferfixed */
UNIV_INTERN
buf_block_t*
buf_page_create(
/*============*/
	ulint	space,	/*!< in: space id */
	ulint	offset,	/*!< in: offset of the page within space in units of
			a page */
	ulint	zip_size,/*!< in: compressed page size, or 0 */
	mtr_t*	mtr)	/*!< in: mini-transaction handle */
{
	buf_frame_t*	frame;
	buf_block_t*	block;
	ulint		fold;
	buf_block_t*	free_block	= NULL;
	buf_pool_t*	buf_pool	= buf_pool_get(space, offset);
	prio_rw_lock_t*	hash_lock;

	ut_ad(mtr);
	ut_ad(mtr->state == MTR_ACTIVE);
	ut_ad(space || !zip_size);

	free_block = buf_LRU_get_free_block(buf_pool);

	fold = buf_page_address_fold(space, offset);
	hash_lock = buf_page_hash_lock_get(buf_pool, fold);

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);
	rw_lock_x_lock(hash_lock);

	block = (buf_block_t*) buf_page_hash_get_low(
		buf_pool, space, offset, fold);

	if (block
	    && buf_page_in_file(&block->page)
	    && !buf_pool_watch_is_sentinel(buf_pool, &block->page)) {
#ifdef UNIV_IBUF_COUNT_DEBUG
		ut_a(ibuf_count_get(space, offset) == 0);
#endif
#if defined UNIV_DEBUG_FILE_ACCESSES || defined UNIV_DEBUG
		block->page.file_page_was_freed = FALSE;
#endif /* UNIV_DEBUG_FILE_ACCESSES || UNIV_DEBUG */

		/* Page can be found in buf_pool */
		rw_lock_x_unlock(hash_lock);
		mutex_exit(&buf_pool->LRU_list_mutex);

		buf_block_free(free_block);

		return(buf_page_get_with_no_latch(space, zip_size, offset, mtr));
	}

	/* If we get here, the page was not in buf_pool: init it there */

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Creating space %lu page %lu to buffer\n",
			space, offset);
	}
#endif /* UNIV_DEBUG */

	block = free_block;

	mutex_enter(&block->mutex);

	buf_page_init(buf_pool, space, offset, fold, zip_size, block);

	rw_lock_x_unlock(hash_lock);

	/* The block must be put to the LRU list */
	buf_LRU_add_block(&block->page, FALSE);

	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	buf_pool->stat.n_pages_created++;

	if (zip_size) {
		void*	data;
		ibool	lru;

		/* Prevent race conditions during buf_buddy_alloc(),
		which may release and reacquire buf_pool->LRU_list_mutex,
		by IO-fixing and X-latching the block. */

		buf_page_set_io_fix(&block->page, BUF_IO_READ);
		rw_lock_x_lock(&block->lock);

		mutex_exit(&block->mutex);
		/* buf_pool->LRU_list_mutex may be released and reacquired by
		buf_buddy_alloc().  Thus, we must release block->mutex
		in order not to break the latching order in
		the reacquisition of buf_pool->LRU_list_mutex.  We also must
		defer this operation until after the block descriptor
		has been added to buf_pool->LRU and buf_pool->page_hash. */
		data = buf_buddy_alloc(buf_pool, zip_size, &lru);
		mutex_enter(&block->mutex);
		block->page.zip.data = (page_zip_t*) data;

		/* To maintain the invariant
		block->in_unzip_LRU_list
		== buf_page_belongs_to_unzip_LRU(&block->page)
		we have to add this block to unzip_LRU after
		block->page.zip.data is set. */
		ut_ad(buf_page_belongs_to_unzip_LRU(&block->page));
		buf_unzip_LRU_add_block(block, FALSE);

		buf_page_set_io_fix(&block->page, BUF_IO_NONE);
		rw_lock_x_unlock(&block->lock);
	}

	mutex_exit(&buf_pool->LRU_list_mutex);

	mtr_memo_push(mtr, block, MTR_MEMO_BUF_FIX);

	buf_page_set_accessed(&block->page);

	mutex_exit(&block->mutex);

	/* Delete possible entries for the page from the insert buffer:
	such can exist if the page belonged to an index which was dropped */

	ibuf_merge_or_delete_for_page(NULL, space, offset, zip_size, TRUE);

	frame = block->frame;

	memset(frame + FIL_PAGE_PREV, 0xff, 4);
	memset(frame + FIL_PAGE_NEXT, 0xff, 4);
	mach_write_to_2(frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_ALLOCATED);

	/* Reset to zero the file flush lsn field in the page; if the first
	page of an ibdata file is 'created' in this function into the buffer
	pool then we lose the original contents of the file flush lsn stamp.
	Then InnoDB could in a crash recovery print a big, false, corruption
	warning if the stamp contains an lsn bigger than the ib_logfile lsn. */

	memset(frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(buf_block_get_space(block),
			    buf_block_get_page_no(block)) == 0);
#endif
	return(block);
}

/********************************************************************//**
Monitor the buffer page read/write activity, and increment corresponding
counter value if MONITOR_MODULE_BUF_PAGE (module_buf_page) module is
enabled. */
static
void
buf_page_monitor(
/*=============*/
	const buf_page_t*	bpage,	/*!< in: pointer to the block */
	enum buf_io_fix		io_type)/*!< in: io_fix types */
{
	const byte*	frame;
	monitor_id_t	counter;

	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	/* If the counter module is not turned on, just return */
	if (!MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)) {
		return;
	}

	ut_a(io_type == BUF_IO_READ || io_type == BUF_IO_WRITE);

	frame = bpage->zip.data
		? bpage->zip.data
		: ((buf_block_t*) bpage)->frame;

	switch (fil_page_get_type(frame)) {
		ulint	level;

	case FIL_PAGE_INDEX:
		level = btr_page_get_level_low(frame);

		/* Check if it is an index page for insert buffer */
		if (btr_page_get_index_id(frame)
		    == (index_id_t)(DICT_IBUF_ID_MIN + IBUF_SPACE_ID)) {
			if (level == 0) {
				counter = MONITOR_RW_COUNTER(
					io_type, MONITOR_INDEX_IBUF_LEAF_PAGE);
			} else {
				counter = MONITOR_RW_COUNTER(
					io_type,
					MONITOR_INDEX_IBUF_NON_LEAF_PAGE);
			}
		} else {
			if (level == 0) {
				counter = MONITOR_RW_COUNTER(
					io_type, MONITOR_INDEX_LEAF_PAGE);
			} else {
				counter = MONITOR_RW_COUNTER(
					io_type, MONITOR_INDEX_NON_LEAF_PAGE);
			}
		}
		break;

        case FIL_PAGE_UNDO_LOG:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_UNDO_LOG_PAGE);
		break;

        case FIL_PAGE_INODE:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_INODE_PAGE);
		break;

        case FIL_PAGE_IBUF_FREE_LIST:
		counter = MONITOR_RW_COUNTER(io_type,
					     MONITOR_IBUF_FREELIST_PAGE);
		break;

        case FIL_PAGE_IBUF_BITMAP:
		counter = MONITOR_RW_COUNTER(io_type,
					     MONITOR_IBUF_BITMAP_PAGE);
		break;

        case FIL_PAGE_TYPE_SYS:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_SYSTEM_PAGE);
		break;

        case FIL_PAGE_TYPE_TRX_SYS:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_TRX_SYSTEM_PAGE);
		break;

        case FIL_PAGE_TYPE_FSP_HDR:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_FSP_HDR_PAGE);
		break;

        case FIL_PAGE_TYPE_XDES:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_XDES_PAGE);
		break;

        case FIL_PAGE_TYPE_BLOB:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_BLOB_PAGE);
		break;

        case FIL_PAGE_TYPE_ZBLOB:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_ZBLOB_PAGE);
		break;

        case FIL_PAGE_TYPE_ZBLOB2:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_ZBLOB2_PAGE);
		break;

	default:
		counter = MONITOR_RW_COUNTER(io_type, MONITOR_OTHER_PAGE);
	}

	MONITOR_INC_NOCHECK(counter);
}

/********************************************************************//**
Mark a table with the specified space pointed by bpage->space corrupted.
Also remove the bpage from LRU list.
@return TRUE if successful */
static
ibool
buf_mark_space_corrupt(
/*===================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	const ibool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);
	ulint		space = bpage->space;
	ibool		ret = TRUE;
	const ulint	fold = buf_page_address_fold(bpage->space,
						     bpage->offset);
	prio_rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool, fold);

	/* First unfix and release lock on the bpage */
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));

	if (!bpage->encrypted) {
		mutex_enter(&buf_pool->LRU_list_mutex);
		rw_lock_x_lock(hash_lock);
		mutex_enter(buf_page_get_mutex(bpage));
		ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_READ);
		ut_ad(bpage->buf_fix_count == 0);

		/* Set BUF_IO_NONE before we remove the block from LRU list */
		buf_page_set_io_fix(bpage, BUF_IO_NONE);

		if (uncompressed) {
			rw_lock_x_unlock_gen(
				&((buf_block_t*) bpage)->lock,
				BUF_IO_READ);
		}
	}

	/* Find the table with specified space id, and mark it corrupted */
	if (dict_set_corrupted_by_space(space)) {
		if (!bpage->encrypted) {
			buf_LRU_free_one_page(bpage);
		}
	} else {
		if (!bpage->encrypted) {
			mutex_exit(buf_page_get_mutex(bpage));
		}
		ret = FALSE;
	}

	if(!bpage->encrypted) {
		mutex_exit(&buf_pool->LRU_list_mutex);
		ut_ad(buf_pool->n_pend_reads > 0);
		os_atomic_decrement_ulint(&buf_pool->n_pend_reads, 1);
	}

	return(ret);
}

/********************************************************************//**
Check if page is maybe compressed, encrypted or both when we encounter
corrupted page. Note that we can't be 100% sure if page is corrupted
or decrypt/decompress just failed.
*/
static
ibool
buf_page_check_corrupt(
/*===================*/
	buf_page_t*	bpage)	/*!< in/out: buffer page read from disk */
{
	ulint zip_size = buf_page_get_zip_size(bpage);
	byte* dst_frame = (zip_size) ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	bool page_compressed = bpage->page_encrypted;
	ulint stored_checksum = bpage->stored_checksum;
	ulint calculated_checksum = bpage->calculated_checksum;
	bool page_compressed_encrypted = bpage->page_compressed;
	ulint space_id = mach_read_from_4(
		dst_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space_id);
	fil_space_t* space = fil_space_found_by_id(space_id);
	bool corrupted = true;
	ulint key_version = bpage->key_version;

	if (key_version != 0 || page_compressed_encrypted) {
		bpage->encrypted = true;
	}

	if (key_version != 0 ||
		(crypt_data && crypt_data->type != CRYPT_SCHEME_UNENCRYPTED) ||
		page_compressed || page_compressed_encrypted) {

		/* Page is really corrupted if post encryption stored
		checksum does not match calculated checksum after page was
		read. For pages compressed and then encrypted, there is no
		checksum. */
		corrupted = (!page_compressed_encrypted && stored_checksum != calculated_checksum);

		if (corrupted) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"%s: Block in space_id %lu in file %s corrupted.",
				page_compressed_encrypted ? "Maybe corruption" : "Corruption",
				space_id, space ? space->name : "NULL");
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Page based on contents %s encrypted.",
				(key_version == 0 && page_compressed_encrypted == false) ? "not" : "maybe");
			if (stored_checksum != BUF_NO_CHECKSUM_MAGIC || calculated_checksum != BUF_NO_CHECKSUM_MAGIC) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Page stored checksum %lu but calculated checksum %lu.",
					stored_checksum, calculated_checksum);
			}
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Reason could be that key_version %lu in page "
				"or in crypt_data %p could not be found.",
				key_version, crypt_data);
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Reason could be also that key management plugin is not found or"
				" used encryption algorithm or method does not match.");
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Based on page page compressed %d, compressed and encrypted %d.",
				page_compressed, page_compressed_encrypted);
		} else {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Block in space_id %lu in file %s encrypted.",
				space_id, space ? space->name : "NULL");
			ib_logf(IB_LOG_LEVEL_ERROR,
				"However key management plugin or used key_id %lu is not found or"
				" used encryption algorithm or method does not match.",
				key_version);
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Marking tablespace as missing. You may drop this table or"
				" install correct key management plugin and key file.");
		}
	}

	return corrupted;
}

/********************************************************************//**
Completes an asynchronous read or write request of a file page to or from
the buffer pool.
@return true if successful */
UNIV_INTERN
bool
buf_page_io_complete(
/*=================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	enum buf_io_fix	io_type;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	const ibool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);
	bool		have_LRU_mutex = false;
	fil_space_t*	space = NULL;

	ut_a(buf_page_in_file(bpage));

	/* We do not need protect io_fix here by mutex to read
	it because this is the only function where we can change the value
	from BUF_IO_READ or BUF_IO_WRITE to some other value, and our code
	ensures that this is the only thread that handles the i/o for this
	block. */

	io_type = buf_page_get_io_fix_unlocked(bpage);
	ut_ad(io_type == BUF_IO_READ || io_type == BUF_IO_WRITE);

	if (io_type == BUF_IO_READ) {
		ulint	read_page_no;
		ulint	read_space_id;
		byte*	frame;

		if (!buf_page_decrypt_after_read(bpage)) {
			/* encryption error! */
			if (buf_page_get_zip_size(bpage)) {
				frame = bpage->zip.data;
			} else {
				frame = ((buf_block_t*) bpage)->frame;
			}

			ib_logf(IB_LOG_LEVEL_INFO,
				"Page %u in tablespace %u encryption error key_version %u.",
				bpage->offset, bpage->space, bpage->key_version);

			goto database_corrupted;
		}

		if (buf_page_get_zip_size(bpage)) {
			frame = bpage->zip.data;
			os_atomic_increment_ulint(&buf_pool->n_pend_unzip, 1);
			if (uncompressed
			    && !buf_zip_decompress((buf_block_t*) bpage,
						   FALSE)) {

				os_atomic_decrement_ulint(
					&buf_pool->n_pend_unzip, 1);

				ib_logf(IB_LOG_LEVEL_INFO,
					"Page %u in tablespace %u zip_decompress failure.",
					bpage->offset, bpage->space);

				goto database_corrupted;
			}
			os_atomic_decrement_ulint(&buf_pool->n_pend_unzip, 1);
		} else {
			ut_a(uncompressed);
			frame = ((buf_block_t*) bpage)->frame;
		}

		/* If this page is not uninitialized and not in the
		doublewrite buffer, then the page number and space id
		should be the same as in block. */
		read_page_no = mach_read_from_4(frame + FIL_PAGE_OFFSET);
		read_space_id = mach_read_from_4(
			frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

		if (bpage->space == TRX_SYS_SPACE
		    && buf_dblwr_page_inside(bpage->offset)) {

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Error: reading page %u\n"
				"InnoDB: which is in the"
				" doublewrite buffer!\n",
				bpage->offset);
		} else if (!read_space_id && !read_page_no) {
			/* This is likely an uninitialized page. */
		} else if ((bpage->space
			    && bpage->space != read_space_id)
			   || bpage->offset != read_page_no) {
			/* We did not compare space_id to read_space_id
			if bpage->space == 0, because the field on the
			page may contain garbage in MySQL < 4.1.1,
			which only supported bpage->space == 0. */

			ut_print_timestamp(stderr);
			fprintf(stderr,
				"  InnoDB: Error: space id and page n:o"
				" stored in the page\n"
				"InnoDB: read in are %lu:%lu,"
				" should be %u:%u!\n",
				read_space_id,
				read_page_no,
				bpage->space,
				bpage->offset);
		}

		if (UNIV_LIKELY(!bpage->is_corrupt ||
				!srv_pass_corrupt_table)) {
			/* From version 3.23.38 up we store the page checksum
			to the 4 first bytes of the page end lsn field */

			if (buf_page_is_corrupted(true, frame,
					buf_page_get_zip_size(bpage))) {

				/* Not a real corruption if it was triggered by
				error injection */
				DBUG_EXECUTE_IF("buf_page_is_corrupt_failure",
					if (bpage->space > TRX_SYS_SPACE
						&& buf_mark_space_corrupt(bpage)) {
						ib_logf(IB_LOG_LEVEL_INFO,
							"Simulated page corruption");
						return(true);
					}
					goto page_not_corrupt;
					;);
database_corrupted:

				bool corrupted = buf_page_check_corrupt(bpage);

				if (corrupted) {
					fil_system_enter();
					space = fil_space_get_by_id(bpage->space);
					fil_system_exit();
					ib_logf(IB_LOG_LEVEL_ERROR,
						"Database page corruption on disk"
						" or a failed");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"Space %u file %s read of page %u.",
						bpage->space,
						space ? space->name : "NULL",
						bpage->offset);
					ib_logf(IB_LOG_LEVEL_ERROR,
						"You may have to recover"
						" from a backup.");


					buf_page_print(frame, buf_page_get_zip_size(bpage),
						BUF_PAGE_PRINT_NO_CRASH);

					ib_logf(IB_LOG_LEVEL_ERROR,
						"It is also possible that your operating"
						"system has corrupted its own file cache.");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"and rebooting your computer removes the error.");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"If the corrupt page is an index page you can also try to");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"fix the corruption by dumping, dropping, and reimporting");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"the corrupt table. You can use CHECK");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"TABLE to scan your table for corruption.");
					ib_logf(IB_LOG_LEVEL_ERROR,
						"See also "
						REFMAN "forcing-innodb-recovery.html"
						" about forcing recovery.");
				}

				if (srv_pass_corrupt_table && bpage->space != 0
					&& bpage->space < SRV_LOG_SPACE_FIRST_ID) {
					trx_t*	trx;

					fprintf(stderr,
						"InnoDB: space %u will be treated as corrupt.\n",
						bpage->space);
					fil_space_set_corrupt(bpage->space);

					trx = innobase_get_trx();
					if (trx && trx->dict_operation_lock_mode == RW_X_LATCH) {
						dict_table_set_corrupt_by_space(bpage->space, FALSE);
					} else {
						dict_table_set_corrupt_by_space(bpage->space, TRUE);
					}
					bpage->is_corrupt = TRUE;
				}

				if (srv_force_recovery < SRV_FORCE_IGNORE_CORRUPT) {
					/* If page space id is larger than TRX_SYS_SPACE
					(0), we will attempt to mark the corresponding
					table as corrupted instead of crashing server */
					if (bpage->space > TRX_SYS_SPACE
						&& buf_mark_space_corrupt(bpage)) {
						return(false);
					} else {
						corrupted = buf_page_check_corrupt(bpage);
						ulint key_version = bpage->key_version;

						if (corrupted) {
							ib_logf(IB_LOG_LEVEL_ERROR,
								"Ending processing because of a corrupt database page.");

							ut_error;
						}

						ib_push_warning(innobase_get_trx(), DB_DECRYPTION_FAILED,
							"Table in tablespace %lu encrypted."
							"However key management plugin or used key_id %lu is not found or"
							" used encryption algorithm or method does not match."
							" Can't continue opening the table.",
							(ulint)bpage->space, key_version);

						if (bpage->space > TRX_SYS_SPACE) {
							if (corrupted) {
								buf_mark_space_corrupt(bpage);
							}
						} else {
							ut_error;
						}
						return(false);
					}
				}
			}
		} /**/

		DBUG_EXECUTE_IF("buf_page_is_corrupt_failure",
				page_not_corrupt:  bpage = bpage; );

		if (recv_recovery_is_on()) {
			/* Pages must be uncompressed for crash recovery. */
			ut_a(uncompressed);
			recv_recover_page(TRUE, (buf_block_t*) bpage);
		}

		if (uncompressed && !recv_no_ibuf_operations
		    && fil_page_get_type(frame) == FIL_PAGE_INDEX
		    && page_is_leaf(frame)) {

			buf_block_t*	block;
			ibool		update_ibuf_bitmap;

			if (UNIV_UNLIKELY(bpage->is_corrupt &&
					  srv_pass_corrupt_table)) {

				block = NULL;
				update_ibuf_bitmap = FALSE;
			} else {

				block = (buf_block_t *) bpage;
				update_ibuf_bitmap = TRUE;
			}

			if (bpage && bpage->encrypted) {
				fprintf(stderr,
					"InnoDB: Warning: Table in tablespace %lu encrypted."
					"However key management plugin or used key_id %u is not found or"
					" used encryption algorithm or method does not match."
					" Can't continue opening the table.\n",
					(ulint)bpage->space, bpage->key_version);
			} else {
				ibuf_merge_or_delete_for_page(
					block, bpage->space,
					bpage->offset, buf_page_get_zip_size(bpage),
					update_ibuf_bitmap);
			}

		}
	} else {
		/* io_type == BUF_IO_WRITE */
		if (bpage->slot) {
			/* Mark slot free */
			bpage->slot->reserved = false;
			bpage->slot = NULL;
		}
	}

	if (io_type == BUF_IO_WRITE
	    && (
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		/* to keep consistency at buf_LRU_insert_zip_clean() */
		buf_page_get_state(bpage) == BUF_BLOCK_ZIP_DIRTY ||
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		buf_page_get_flush_type(bpage) == BUF_FLUSH_LRU)) {

		have_LRU_mutex = true; /* optimistic */
	}
retry_mutex:
	if (have_LRU_mutex) {
		mutex_enter(&buf_pool->LRU_list_mutex);
	}

	ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);
	mutex_enter(block_mutex);

	if (io_type == BUF_IO_WRITE
			  && (
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
			      buf_page_get_state(bpage) == BUF_BLOCK_ZIP_DIRTY
			      ||
#endif
			      buf_page_get_flush_type(bpage) == BUF_FLUSH_LRU)
			  && !have_LRU_mutex) {

		mutex_exit(block_mutex);
		have_LRU_mutex = true;
		goto retry_mutex;
	}

#ifdef UNIV_IBUF_COUNT_DEBUG
	if (io_type == BUF_IO_WRITE || uncompressed) {
		/* For BUF_IO_READ of compressed-only blocks, the
		buffered operations will be merged by buf_page_get_gen()
		after the block has been uncompressed. */
		ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
	}
#endif
	/* Because this thread which does the unlocking is not the same that
	did the locking, we use a pass value != 0 in unlock, which simply
	removes the newest lock debug record, without checking the thread
	id. */

	switch (io_type) {
	case BUF_IO_READ:

		buf_page_set_io_fix(bpage, BUF_IO_NONE);

		/* NOTE that the call to ibuf may have moved the ownership of
		the x-latch to this OS thread: do not let this confuse you in
		debugging! */

		ut_ad(buf_pool->n_pend_reads > 0);
		os_atomic_decrement_ulint(&buf_pool->n_pend_reads, 1);
		os_atomic_increment_ulint(&buf_pool->stat.n_pages_read, 1);

		ut_ad(!have_LRU_mutex);

		if (uncompressed) {
			rw_lock_x_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_READ);
		}

		break;

	case BUF_IO_WRITE:
		/* Write means a flush operation: call the completion
		routine in the flush system */

		buf_flush_write_complete(bpage);

		os_atomic_increment_ulint(&buf_pool->stat.n_pages_written, 1);

		if (have_LRU_mutex) {
			mutex_exit(&buf_pool->LRU_list_mutex);
		}

		if (uncompressed) {
			rw_lock_s_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_WRITE);
		}

		break;

	default:
		ut_error;
	}

	buf_page_monitor(bpage, io_type);

#ifdef UNIV_DEBUG
	if (buf_debug_prints) {
		fprintf(stderr, "Has %s page space %lu page no %lu\n",
			io_type == BUF_IO_READ ? "read" : "written",
			buf_page_get_space(bpage),
			buf_page_get_page_no(bpage));
	}
#endif /* UNIV_DEBUG */

	mutex_exit(block_mutex);

	return(true);
}

/*********************************************************************//**
Asserts that all file pages in the buffer are in a replaceable state.
@return	TRUE */
static
ibool
buf_all_freed_instance(
/*===================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instancce */
{
	ulint		i;
	buf_chunk_t*	chunk;

	ut_ad(buf_pool);

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {

		mutex_enter(&buf_pool->LRU_list_mutex);

		const buf_block_t* block = buf_chunk_not_freed(chunk);

		mutex_exit(&buf_pool->LRU_list_mutex);

		if (UNIV_LIKELY_NULL(block)) {
				if (block->page.key_version == 0) {
				fil_space_t* space = fil_space_get(block->page.space);
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Page %u %u still fixed or dirty.",
					block->page.space,
					block->page.offset);
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Page oldest_modification %lu fix_count %d io_fix %d.",
					(ulong) block->page.oldest_modification,
					block->page.buf_fix_count,
					buf_page_get_io_fix(&block->page));
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Page space_id %u name %s.",
					block->page.space,
					(space && space->name) ? space->name : "NULL");
				ut_error;
			}
		}
	}

	return(TRUE);
}

/*********************************************************************//**
Invalidates file pages in one buffer pool instance */
static
void
buf_pool_invalidate_instance(
/*=========================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	ulint		i;

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));

	mutex_enter(&buf_pool->flush_state_mutex);

	for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {

		/* As this function is called during startup and
		during redo application phase during recovery, InnoDB
		is single threaded (apart from IO helper threads) at
		this stage. No new write batch can be in intialization
		stage at this point. */
		ut_ad(buf_pool->init_flush[i] == FALSE);

		/* However, it is possible that a write batch that has
		been posted earlier is still not complete. For buffer
		pool invalidation to proceed we must ensure there is NO
		write activity happening. */
		if (buf_pool->n_flush[i] > 0) {
			buf_flush_t	type = static_cast<buf_flush_t>(i);

			mutex_exit(&buf_pool->flush_state_mutex);
			buf_flush_wait_batch_end(buf_pool, type);
			mutex_enter(&buf_pool->flush_state_mutex);
		}
	}
	mutex_exit(&buf_pool->flush_state_mutex);

	ut_ad(buf_all_freed_instance(buf_pool));

	while (buf_LRU_scan_and_free_block(buf_pool, TRUE)) {
	}

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);

	ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0);
	ut_ad(UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0);

	buf_pool->freed_page_clock = 0;
	buf_pool->LRU_old = NULL;
	buf_pool->LRU_old_len = 0;

	mutex_exit(&buf_pool->LRU_list_mutex);

	memset(&buf_pool->stat, 0x00, sizeof(buf_pool->stat));
	buf_refresh_io_stats(buf_pool);
}

/*********************************************************************//**
Invalidates the file pages in the buffer pool when an archive recovery is
completed. All the file pages buffered must be in a replaceable state when
this function is called: not latched and not modified. */
UNIV_INTERN
void
buf_pool_invalidate(void)
/*=====================*/
{
	ulint   i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_invalidate_instance(buf_pool_from_array(i));
	}
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/*********************************************************************//**
Validates data in one buffer pool instance
@return	TRUE */
static
ibool
buf_pool_validate_instance(
/*=======================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	buf_page_t*	b;
	buf_chunk_t*	chunk;
	ulint		i;
	ulint		n_lru_flush	= 0;
	ulint		n_page_flush	= 0;
	ulint		n_list_flush	= 0;
	ulint		n_lru		= 0;
	ulint		n_flush		= 0;
	ulint		n_free		= 0;
	ulint		n_zip		= 0;
	ulint		fold		= 0;
	ulint		space		= 0;
	ulint		offset		= 0;

	ut_ad(buf_pool);

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);
	hash_lock_x_all(buf_pool->page_hash);
	mutex_enter(&buf_pool->zip_mutex);
	mutex_enter(&buf_pool->free_list_mutex);
	mutex_enter(&buf_pool->flush_state_mutex);

	chunk = buf_pool->chunks;

	/* Check the uncompressed blocks. */

	for (i = buf_pool->n_chunks; i--; chunk++) {

		ulint		j;
		buf_block_t*	block = chunk->blocks;

		for (j = chunk->size; j--; block++) {

			switch (buf_block_get_state(block)) {
			case BUF_BLOCK_POOL_WATCH:
			case BUF_BLOCK_ZIP_PAGE:
			case BUF_BLOCK_ZIP_DIRTY:
				/* These should only occur on
				zip_clean, zip_free[], or flush_list. */
				ut_error;
				break;

			case BUF_BLOCK_FILE_PAGE:

				space = buf_block_get_space(block);
				offset = buf_block_get_page_no(block);
				fold = buf_page_address_fold(space, offset);
				ut_a(buf_page_hash_get_low(buf_pool,
							   space,
							   offset,
							   fold)
				     == &block->page);

#ifdef UNIV_IBUF_COUNT_DEBUG
				ut_a(buf_page_get_io_fix_unlocked(&block->page)
				     == BUF_IO_READ
				     || !ibuf_count_get(buf_block_get_space(
								block),
							buf_block_get_page_no(
								block)));
#endif
				switch (buf_page_get_io_fix_unlocked(
						&block->page)) {
				case BUF_IO_NONE:
					break;

				case BUF_IO_WRITE:
					switch (buf_page_get_flush_type(
							&block->page)) {
					case BUF_FLUSH_LRU:
					case BUF_FLUSH_SINGLE_PAGE:
					case BUF_FLUSH_LIST:
						break;
					default:
						ut_error;
					}

					break;

				case BUF_IO_READ:

					ut_a(rw_lock_is_locked(&block->lock,
							       RW_LOCK_EX));
					break;

				case BUF_IO_PIN:
					break;
				}

				n_lru++;
				break;

			case BUF_BLOCK_NOT_USED:
				n_free++;
				break;

			case BUF_BLOCK_READY_FOR_USE:
			case BUF_BLOCK_MEMORY:
			case BUF_BLOCK_REMOVE_HASH:
				/* do nothing */
				break;
			}
		}
	}

	/* Check clean compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool->zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		switch (buf_page_get_io_fix(b)) {
		case BUF_IO_NONE:
		case BUF_IO_PIN:
			/* All clean blocks should be I/O-unfixed. */
			break;
		case BUF_IO_READ:
			/* In buf_LRU_free_page(), we temporarily set
			b->io_fix = BUF_IO_READ for a newly allocated
			control block in order to prevent
			buf_page_get_gen() from decompressing the block. */
			break;
		default:
			ut_error;
			break;
		}

		/* It is OK to read oldest_modification here because
		we have acquired buf_pool->zip_mutex above which acts
		as the 'block->mutex' for these bpages. */
		ut_a(!b->oldest_modification);
		fold = buf_page_address_fold(b->space, b->offset);
		ut_a(buf_page_hash_get_low(buf_pool, b->space, b->offset,
					   fold) == b);
		n_lru++;
		n_zip++;
	}

	/* Check dirty blocks. */

	buf_flush_list_mutex_enter(buf_pool);
	for (b = UT_LIST_GET_FIRST(buf_pool->flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);
		ut_a(b->oldest_modification);
		n_flush++;

		switch (buf_page_get_state(b)) {
		case BUF_BLOCK_ZIP_DIRTY:
			n_lru++;
			n_zip++;
			/* fallthrough */
		case BUF_BLOCK_FILE_PAGE:
			switch (buf_page_get_io_fix_unlocked(b)) {
			case BUF_IO_NONE:
			case BUF_IO_READ:
			case BUF_IO_PIN:
				break;
			case BUF_IO_WRITE:
				switch (buf_page_get_flush_type(b)) {
				case BUF_FLUSH_LRU:
					n_lru_flush++;
					break;
				case BUF_FLUSH_SINGLE_PAGE:
					n_page_flush++;
					break;
				case BUF_FLUSH_LIST:
					n_list_flush++;
					break;
				default:
					ut_error;
				}
				break;
			default:
				ut_error;
			}
			break;
		case BUF_BLOCK_POOL_WATCH:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		}
		fold = buf_page_address_fold(b->space, b->offset);
		ut_a(buf_page_hash_get_low(buf_pool, b->space, b->offset,
					   fold) == b);
	}

	ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == n_flush);

	hash_unlock_x_all(buf_pool->page_hash);
	buf_flush_list_mutex_exit(buf_pool);

	mutex_exit(&buf_pool->zip_mutex);

	if (n_lru + n_free > buf_pool->curr_size + n_zip) {
		fprintf(stderr, "n LRU %lu, n free %lu, pool %lu zip %lu\n",
			n_lru, n_free,
			buf_pool->curr_size, n_zip);
		ut_error;
	}

	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == n_lru);

	mutex_exit(&buf_pool->LRU_list_mutex);

	if (UT_LIST_GET_LEN(buf_pool->free) != n_free) {
		fprintf(stderr, "Free list len %lu, free blocks %lu\n",
			UT_LIST_GET_LEN(buf_pool->free),
			n_free);
		ut_error;
	}

	mutex_exit(&buf_pool->free_list_mutex);

	ut_a(buf_pool->n_flush[BUF_FLUSH_LIST] == n_list_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_LRU] == n_lru_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE] == n_page_flush);

	mutex_exit(&buf_pool->flush_state_mutex);

	ut_a(buf_LRU_validate());
	ut_a(buf_flush_validate(buf_pool));

	return(TRUE);
}

/*********************************************************************//**
Validates the buffer buf_pool data structure.
@return	TRUE */
UNIV_INTERN
ibool
buf_validate(void)
/*==============*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_pool_validate_instance(buf_pool);
	}
	return(TRUE);
}

#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/*********************************************************************//**
Prints info of the buffer buf_pool data structure for one instance. */
static
void
buf_print_instance(
/*===============*/
	buf_pool_t*	buf_pool)
{
	index_id_t*	index_ids;
	ulint*		counts;
	ulint		size;
	ulint		i;
	ulint		j;
	index_id_t	id;
	ulint		n_found;
	buf_chunk_t*	chunk;
	dict_index_t*	index;

	ut_ad(buf_pool);

	size = buf_pool->curr_size;

	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	index_ids = static_cast<index_id_t*>(
		mem_alloc(size * sizeof *index_ids));

	counts = static_cast<ulint*>(mem_alloc(sizeof(ulint) * size));

	/* Dirty reads below */

	fprintf(stderr,
		"buf_pool size %lu\n"
		"database pages %lu\n"
		"free pages %lu\n"
		"modified database pages %lu\n"
		"n pending decompressions %lu\n"
		"n pending reads %lu\n"
		"n pending flush LRU %lu list %lu single page %lu\n"
		"pages made young %lu, not young %lu\n"
		"pages read %lu, created %lu, written %lu\n",
		(ulint) size,
		(ulint) UT_LIST_GET_LEN(buf_pool->LRU),
		(ulint) UT_LIST_GET_LEN(buf_pool->free),
		(ulint) UT_LIST_GET_LEN(buf_pool->flush_list),
		(ulint) buf_pool->n_pend_unzip,
		(ulint) buf_pool->n_pend_reads,
		(ulint) buf_pool->n_flush[BUF_FLUSH_LRU],
		(ulint) buf_pool->n_flush[BUF_FLUSH_LIST],
		(ulint) buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE],
		(ulint) buf_pool->stat.n_pages_made_young,
		(ulint) buf_pool->stat.n_pages_not_made_young,
		(ulint) buf_pool->stat.n_pages_read,
		(ulint) buf_pool->stat.n_pages_created,
		(ulint) buf_pool->stat.n_pages_written);

	/* Count the number of blocks belonging to each index in the buffer */

	n_found = 0;

	mutex_enter(&buf_pool->LRU_list_mutex);

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {
		buf_block_t*	block		= chunk->blocks;
		ulint		n_blocks	= chunk->size;

		for (; n_blocks--; block++) {
			const buf_frame_t* frame = block->frame;

			if (fil_page_get_type(frame) == FIL_PAGE_INDEX) {

				id = btr_page_get_index_id(frame);

				/* Look for the id in the index_ids array */
				j = 0;

				while (j < n_found) {

					if (index_ids[j] == id) {
						counts[j]++;

						break;
					}
					j++;
				}

				if (j == n_found) {
					n_found++;
					index_ids[j] = id;
					counts[j] = 1;
				}
			}
		}
	}

	mutex_exit(&buf_pool->LRU_list_mutex);

	for (i = 0; i < n_found; i++) {
		index = dict_index_get_if_in_cache(index_ids[i]);

		fprintf(stderr,
			"Block count for index %llu in buffer is about %lu",
			(ullint) index_ids[i],
			(ulint) counts[i]);

		if (index) {
			putc(' ', stderr);
			dict_index_name_print(stderr, NULL, index);
		}

		putc('\n', stderr);
	}

	mem_free(index_ids);
	mem_free(counts);

	ut_a(buf_pool_validate_instance(buf_pool));
}

/*********************************************************************//**
Prints info of the buffer buf_pool data structure. */
UNIV_INTERN
void
buf_print(void)
/*===========*/
{
	ulint   i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		buf_print_instance(buf_pool);
	}
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

#ifdef UNIV_DEBUG
/*********************************************************************//**
Returns the number of latched pages in the buffer pool.
@return	number of latched pages */
UNIV_INTERN
ulint
buf_get_latched_pages_number_instance(
/*==================================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	buf_page_t*	b;
	ulint		i;
	buf_chunk_t*	chunk;
	ulint		fixed_pages_number = 0;

	/* The LRU list mutex is enough to protect the required fields below */
	mutex_enter(&buf_pool->LRU_list_mutex);

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {
		buf_block_t*	block;
		ulint		j;

		block = chunk->blocks;

		for (j = chunk->size; j--; block++) {
			if (buf_block_get_state(block)
			    != BUF_BLOCK_FILE_PAGE) {

				continue;
			}

			if (block->page.buf_fix_count != 0
			    || buf_page_get_io_fix_unlocked(&block->page)
			    != BUF_IO_NONE) {
				fixed_pages_number++;
			}

		}
	}

	mutex_exit(&buf_pool->LRU_list_mutex);

	mutex_enter(&buf_pool->zip_mutex);

	/* Traverse the lists of clean and dirty compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool->zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		ut_a(buf_page_get_io_fix(b) != BUF_IO_WRITE);

		if (b->buf_fix_count != 0
		    || buf_page_get_io_fix(b) != BUF_IO_NONE) {
			fixed_pages_number++;
		}
	}

	buf_flush_list_mutex_enter(buf_pool);
	for (b = UT_LIST_GET_FIRST(buf_pool->flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);

		switch (buf_page_get_state(b)) {
		case BUF_BLOCK_ZIP_DIRTY:
			if (b->buf_fix_count != 0
			    || buf_page_get_io_fix(b) != BUF_IO_NONE) {
				fixed_pages_number++;
			}
			break;
		case BUF_BLOCK_FILE_PAGE:
			/* uncompressed page */
		case BUF_BLOCK_REMOVE_HASH:
			/* We hold flush list but not LRU list mutex here.
			Thus encountering BUF_BLOCK_REMOVE_HASH pages is
			possible.  */
			break;
		case BUF_BLOCK_POOL_WATCH:
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_READY_FOR_USE:
		case BUF_BLOCK_MEMORY:
			ut_error;
			break;
		}
	}

	buf_flush_list_mutex_exit(buf_pool);
	mutex_exit(&buf_pool->zip_mutex);

	return(fixed_pages_number);
}

/*********************************************************************//**
Returns the number of latched pages in all the buffer pools.
@return	number of latched pages */
UNIV_INTERN
ulint
buf_get_latched_pages_number(void)
/*==============================*/
{
	ulint	i;
	ulint	total_latched_pages = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		total_latched_pages += buf_get_latched_pages_number_instance(
			buf_pool);
	}

	return(total_latched_pages);
}

#endif /* UNIV_DEBUG */

/*********************************************************************//**
Returns the number of pending buf pool read ios.
@return	number of pending read I/O operations */
UNIV_INTERN
ulint
buf_get_n_pending_read_ios(void)
/*============================*/
{
	ulint	i;
	ulint	pend_ios = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		pend_ios += buf_pool_from_array(i)->n_pend_reads;
	}

	return(pend_ios);
}

/*********************************************************************//**
Returns the ratio in percents of modified pages in the buffer pool /
database pages in the buffer pool.
@return	modified page percentage ratio */
UNIV_INTERN
double
buf_get_modified_ratio_pct(void)
/*============================*/
{
	double		percentage = 0.0;
	ulint		lru_len = 0;
	ulint		free_len = 0;
	ulint		flush_list_len = 0;

	buf_get_total_list_len(&lru_len, &free_len, &flush_list_len);

	percentage = (100.0 * flush_list_len) / (1.0 + lru_len + free_len);

	/* 1 + is there to avoid division by zero */

	return(percentage);
}

/*******************************************************************//**
Aggregates a pool stats information with the total buffer pool stats  */
static
void
buf_stats_aggregate_pool_info(
/*==========================*/
	buf_pool_info_t*	total_info,	/*!< in/out: the buffer pool
						info to store aggregated
						result */
	const buf_pool_info_t*	pool_info)	/*!< in: individual buffer pool
						stats info */
{
	ut_a(total_info && pool_info);

	/* Nothing to copy if total_info is the same as pool_info */
	if (total_info == pool_info) {
		return;
	}

	total_info->pool_size += pool_info->pool_size;
	total_info->pool_size_bytes += pool_info->pool_size_bytes;
	total_info->lru_len += pool_info->lru_len;
	total_info->old_lru_len += pool_info->old_lru_len;
	total_info->free_list_len += pool_info->free_list_len;
	total_info->flush_list_len += pool_info->flush_list_len;
	total_info->n_pend_unzip += pool_info->n_pend_unzip;
	total_info->n_pend_reads += pool_info->n_pend_reads;
	total_info->n_pending_flush_lru += pool_info->n_pending_flush_lru;
	total_info->n_pending_flush_list += pool_info->n_pending_flush_list;
	total_info->n_pages_made_young += pool_info->n_pages_made_young;
	total_info->n_pages_not_made_young += pool_info->n_pages_not_made_young;
	total_info->n_pages_read += pool_info->n_pages_read;
	total_info->n_pages_created += pool_info->n_pages_created;
	total_info->n_pages_written += pool_info->n_pages_written;
	total_info->n_page_gets += pool_info->n_page_gets;
	total_info->n_ra_pages_read_rnd += pool_info->n_ra_pages_read_rnd;
	total_info->n_ra_pages_read += pool_info->n_ra_pages_read;
	total_info->n_ra_pages_evicted += pool_info->n_ra_pages_evicted;
	total_info->page_made_young_rate += pool_info->page_made_young_rate;
	total_info->page_not_made_young_rate +=
		pool_info->page_not_made_young_rate;
	total_info->pages_read_rate += pool_info->pages_read_rate;
	total_info->pages_created_rate += pool_info->pages_created_rate;
	total_info->pages_written_rate += pool_info->pages_written_rate;
	total_info->n_page_get_delta += pool_info->n_page_get_delta;
	total_info->page_read_delta += pool_info->page_read_delta;
	total_info->young_making_delta += pool_info->young_making_delta;
	total_info->not_young_making_delta += pool_info->not_young_making_delta;
	total_info->pages_readahead_rnd_rate += pool_info->pages_readahead_rnd_rate;
	total_info->pages_readahead_rate += pool_info->pages_readahead_rate;
	total_info->pages_evicted_rate += pool_info->pages_evicted_rate;
	total_info->unzip_lru_len += pool_info->unzip_lru_len;
	total_info->io_sum += pool_info->io_sum;
	total_info->io_cur += pool_info->io_cur;
	total_info->unzip_sum += pool_info->unzip_sum;
	total_info->unzip_cur += pool_info->unzip_cur;
}
/*******************************************************************//**
Collect buffer pool stats information for a buffer pool. Also
record aggregated stats if there are more than one buffer pool
in the server */
UNIV_INTERN
void
buf_stats_get_pool_info(
/*====================*/
	buf_pool_t*		buf_pool,	/*!< in: buffer pool */
	ulint			pool_id,	/*!< in: buffer pool ID */
	buf_pool_info_t*	all_pool_info)	/*!< in/out: buffer pool info
						to fill */
{
	buf_pool_info_t*        pool_info;
	time_t			current_time;
	double			time_elapsed;

	/* Find appropriate pool_info to store stats for this buffer pool */
	pool_info = &all_pool_info[pool_id];
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));

	pool_info->pool_unique_id = pool_id;

	pool_info->pool_size = buf_pool->curr_size;

	pool_info->pool_size_bytes = buf_pool->curr_pool_size;

	pool_info->lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

	pool_info->old_lru_len = buf_pool->LRU_old_len;

	pool_info->free_list_len = UT_LIST_GET_LEN(buf_pool->free);

	pool_info->flush_list_len = UT_LIST_GET_LEN(buf_pool->flush_list);

	pool_info->n_pend_unzip = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	pool_info->n_pend_reads = buf_pool->n_pend_reads;

	mutex_enter(&buf_pool->flush_state_mutex);

	pool_info->n_pending_flush_lru =
		 (buf_pool->n_flush[BUF_FLUSH_LRU]
		  + buf_pool->init_flush[BUF_FLUSH_LRU]);

	pool_info->n_pending_flush_list =
		 (buf_pool->n_flush[BUF_FLUSH_LIST]
		  + buf_pool->init_flush[BUF_FLUSH_LIST]);

	pool_info->n_pending_flush_single_page =
		 (buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]
		  + buf_pool->init_flush[BUF_FLUSH_SINGLE_PAGE]);

	mutex_exit(&buf_pool->flush_state_mutex);

	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time,
					buf_pool->last_printout_time);

	pool_info->n_pages_made_young = buf_pool->stat.n_pages_made_young;

	pool_info->n_pages_not_made_young =
		buf_pool->stat.n_pages_not_made_young;

	pool_info->n_pages_read = buf_pool->stat.n_pages_read;

	pool_info->n_pages_created = buf_pool->stat.n_pages_created;

	pool_info->n_pages_written = buf_pool->stat.n_pages_written;

	pool_info->n_page_gets = buf_pool->stat.n_page_gets;

	pool_info->n_ra_pages_read_rnd = buf_pool->stat.n_ra_pages_read_rnd;
	pool_info->n_ra_pages_read = buf_pool->stat.n_ra_pages_read;

	pool_info->n_ra_pages_evicted = buf_pool->stat.n_ra_pages_evicted;

	pool_info->page_made_young_rate =
		 (buf_pool->stat.n_pages_made_young
		  - buf_pool->old_stat.n_pages_made_young) / time_elapsed;

	pool_info->page_not_made_young_rate =
		 (buf_pool->stat.n_pages_not_made_young
		  - buf_pool->old_stat.n_pages_not_made_young) / time_elapsed;

	pool_info->pages_read_rate =
		(buf_pool->stat.n_pages_read
		  - buf_pool->old_stat.n_pages_read) / time_elapsed;

	pool_info->pages_created_rate =
		(buf_pool->stat.n_pages_created
		 - buf_pool->old_stat.n_pages_created) / time_elapsed;

	pool_info->pages_written_rate =
		(buf_pool->stat.n_pages_written
		 - buf_pool->old_stat.n_pages_written) / time_elapsed;

	pool_info->n_page_get_delta = buf_pool->stat.n_page_gets
				      - buf_pool->old_stat.n_page_gets;

	if (pool_info->n_page_get_delta) {
		pool_info->page_read_delta = buf_pool->stat.n_pages_read
					     - buf_pool->old_stat.n_pages_read;

		pool_info->young_making_delta =
			buf_pool->stat.n_pages_made_young
			- buf_pool->old_stat.n_pages_made_young;

		pool_info->not_young_making_delta =
			buf_pool->stat.n_pages_not_made_young
			- buf_pool->old_stat.n_pages_not_made_young;
	}
	pool_info->pages_readahead_rnd_rate =
		 (buf_pool->stat.n_ra_pages_read_rnd
		  - buf_pool->old_stat.n_ra_pages_read_rnd) / time_elapsed;


	pool_info->pages_readahead_rate =
		 (buf_pool->stat.n_ra_pages_read
		  - buf_pool->old_stat.n_ra_pages_read) / time_elapsed;

	pool_info->pages_evicted_rate =
		(buf_pool->stat.n_ra_pages_evicted
		 - buf_pool->old_stat.n_ra_pages_evicted) / time_elapsed;

	pool_info->unzip_lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	pool_info->io_sum = buf_LRU_stat_sum.io;

	pool_info->io_cur = buf_LRU_stat_cur.io;

	pool_info->unzip_sum = buf_LRU_stat_sum.unzip;

	pool_info->unzip_cur = buf_LRU_stat_cur.unzip;

	buf_refresh_io_stats(buf_pool);
}

/*********************************************************************//**
Prints info of the buffer i/o. */
UNIV_INTERN
void
buf_print_io_instance(
/*==================*/
	buf_pool_info_t*pool_info,	/*!< in: buffer pool info */
	FILE*		file)		/*!< in/out: buffer where to print */
{
	ut_ad(pool_info);

	fprintf(file,
		"Buffer pool size        %lu\n"
		"Buffer pool size, bytes " ULINTPF "\n"
		"Free buffers            %lu\n"
		"Database pages          %lu\n"
		"Old database pages      %lu\n"
		"Modified db pages       %lu\n"
		"Percent of dirty pages(LRU & free pages): %.3f\n"
		"Max dirty pages percent: %.3f\n"
		"Pending reads %lu\n"
		"Pending writes: LRU %lu, flush list %lu, single page %lu\n",
		pool_info->pool_size,
		pool_info->pool_size_bytes,
		pool_info->free_list_len,
		pool_info->lru_len,
		pool_info->old_lru_len,
		pool_info->flush_list_len,
		(((double) pool_info->flush_list_len) /
		  (pool_info->lru_len + pool_info->free_list_len + 1.0)) * 100.0,
		srv_max_buf_pool_modified_pct,
		pool_info->n_pend_reads,
		pool_info->n_pending_flush_lru,
		pool_info->n_pending_flush_list,
		pool_info->n_pending_flush_single_page);

	fprintf(file,
		"Pages made young %lu, not young %lu\n"
		"%.2f youngs/s, %.2f non-youngs/s\n"
		"Pages read %lu, created %lu, written %lu\n"
		"%.2f reads/s, %.2f creates/s, %.2f writes/s\n",
		pool_info->n_pages_made_young,
		pool_info->n_pages_not_made_young,
		pool_info->page_made_young_rate,
		pool_info->page_not_made_young_rate,
		pool_info->n_pages_read,
		pool_info->n_pages_created,
		pool_info->n_pages_written,
		pool_info->pages_read_rate,
		pool_info->pages_created_rate,
		pool_info->pages_written_rate);

	if (pool_info->n_page_get_delta) {
		double hit_rate = ((1000 * pool_info->page_read_delta)
				/ pool_info->n_page_get_delta);

		if (hit_rate > 1000) {
			hit_rate = 1000;
		}

		hit_rate = 1000 - hit_rate;

		fprintf(file,
			"Buffer pool hit rate %lu / 1000,"
			" young-making rate %lu / 1000 not %lu / 1000\n",
			(ulint) hit_rate,
			(ulint) (1000 * pool_info->young_making_delta
				 / pool_info->n_page_get_delta),
			(ulint) (1000 * pool_info->not_young_making_delta
				 / pool_info->n_page_get_delta));
	} else {
		fputs("No buffer pool page gets since the last printout\n",
		      file);
	}

	/* Statistics about read ahead algorithm */
	fprintf(file, "Pages read ahead %.2f/s,"
		" evicted without access %.2f/s,"
		" Random read ahead %.2f/s\n",

		pool_info->pages_readahead_rate,
		pool_info->pages_evicted_rate,
		pool_info->pages_readahead_rnd_rate);

	/* Print some values to help us with visualizing what is
	happening with LRU eviction. */
	fprintf(file,
		"LRU len: %lu, unzip_LRU len: %lu\n"
		"I/O sum[%lu]:cur[%lu], unzip sum[%lu]:cur[%lu]\n",
		pool_info->lru_len, pool_info->unzip_lru_len,
		pool_info->io_sum, pool_info->io_cur,
		pool_info->unzip_sum, pool_info->unzip_cur);
}

/*********************************************************************//**
Prints info of the buffer i/o. */
UNIV_INTERN
void
buf_print_io(
/*=========*/
	FILE*	file)	/*!< in/out: buffer where to print */
{
	ulint			i;
	buf_pool_info_t*	pool_info;
	buf_pool_info_t*	pool_info_total;

	/* If srv_buf_pool_instances is greater than 1, allocate
	one extra buf_pool_info_t, the last one stores
	aggregated/total values from all pools */
	if (srv_buf_pool_instances > 1) {
		pool_info = (buf_pool_info_t*) mem_zalloc((
			srv_buf_pool_instances + 1) * sizeof *pool_info);

		pool_info_total = &pool_info[srv_buf_pool_instances];
	} else {
		ut_a(srv_buf_pool_instances == 1);

		pool_info_total = pool_info =
			static_cast<buf_pool_info_t*>(
				mem_zalloc(sizeof *pool_info));
	}

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		/* Fetch individual buffer pool info and calculate
		aggregated stats along the way */
		buf_stats_get_pool_info(buf_pool, i, pool_info);

		/* If we have more than one buffer pool, store
		the aggregated stats  */
		if (srv_buf_pool_instances > 1) {
			buf_stats_aggregate_pool_info(pool_info_total,
						      &pool_info[i]);
		}
	}

	/* Print the aggreate buffer pool info */
	buf_print_io_instance(pool_info_total, file);

	/* If there are more than one buffer pool, print each individual pool
	info */
	if (srv_buf_pool_instances > 1) {
		fputs("----------------------\n"
		"INDIVIDUAL BUFFER POOL INFO\n"
		"----------------------\n", file);

		for (i = 0; i < srv_buf_pool_instances; i++) {
			fprintf(file, "---BUFFER POOL %lu\n", i);
			buf_print_io_instance(&pool_info[i], file);
		}
	}

	mem_free(pool_info);
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
UNIV_INTERN
void
buf_refresh_io_stats(
/*=================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	buf_pool->last_printout_time = ut_time();
	buf_pool->old_stat = buf_pool->stat;
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
UNIV_INTERN
void
buf_refresh_io_stats_all(void)
/*==========================*/
{
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_refresh_io_stats(buf_pool);
	}
}

/**********************************************************************//**
Check if all pages in all buffer pools are in a replacable state.
@return FALSE if not */
UNIV_INTERN
ibool
buf_all_freed(void)
/*===============*/
{
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		if (!buf_all_freed_instance(buf_pool)) {
			return(FALSE);
		}
	}

	return(TRUE);
}

/*********************************************************************//**
Checks that there currently are no pending i/o-operations for the buffer
pool.
@return	number of pending i/o */
UNIV_INTERN
ulint
buf_pool_check_no_pending_io(void)
/*==============================*/
{
	ulint		i;
	ulint		pending_io = 0;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		pending_io += buf_pool->n_pend_reads;

		mutex_enter(&buf_pool->flush_state_mutex);

		pending_io += buf_pool->n_flush[BUF_FLUSH_LRU];
		pending_io += buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE];
		pending_io += buf_pool->n_flush[BUF_FLUSH_LIST];

		mutex_exit(&buf_pool->flush_state_mutex);
	}

	return(pending_io);
}

#if 0
Code currently not used
/*********************************************************************//**
Gets the current length of the free list of buffer blocks.
@return	length of the free list */
UNIV_INTERN
ulint
buf_get_free_list_len(void)
/*=======================*/
{
	ulint	len;

	mutex_enter(&buf_pool->free_list_mutex);

	len = UT_LIST_GET_LEN(buf_pool->free);

	mutex_exit(&buf_pool->free_list_mutex);

	return(len);
}
#endif

#else /* !UNIV_HOTBACKUP */
/********************************************************************//**
Inits a page to the buffer buf_pool, for use in mysqlbackup --restore. */
UNIV_INTERN
void
buf_page_init_for_backup_restore(
/*=============================*/
	ulint		space,	/*!< in: space id */
	ulint		offset,	/*!< in: offset of the page within space
				in units of a page */
	ulint		zip_size,/*!< in: compressed page size in bytes
				or 0 for uncompressed pages */
	buf_block_t*	block)	/*!< in: block to init */
{
	block->page.state	= BUF_BLOCK_FILE_PAGE;
	block->page.space	= space;
	block->page.offset	= offset;

	page_zip_des_init(&block->page.zip);

	/* We assume that block->page.data has been allocated
	with zip_size == UNIV_PAGE_SIZE. */
	ut_ad(zip_size <= UNIV_ZIP_SIZE_MAX);
	ut_ad(ut_is_2pow(zip_size));
	page_zip_set_size(&block->page.zip, zip_size);
	if (zip_size) {
		block->page.zip.data = block->frame + UNIV_PAGE_SIZE;
	}
}
#endif /* !UNIV_HOTBACKUP */

/*********************************************************************//**
Aquire LRU list mutex */
void
buf_pool_mutex_enter(
/*=================*/
	buf_pool_t*	buf_pool) /*!< in: buffer pool */
{
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);
}
/*********************************************************************//**
Exit LRU list mutex */
void
buf_pool_mutex_exit(
/*================*/
	buf_pool_t*	buf_pool) /*!< in: buffer pool */
{
	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));
	mutex_exit(&buf_pool->LRU_list_mutex);
}

/********************************************************************//**
Reserve unused slot from temporary memory array and allocate necessary
temporary memory if not yet allocated.
@return reserved slot */
UNIV_INTERN
buf_tmp_buffer_t*
buf_pool_reserve_tmp_slot(
/*======================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool where to
					reserve */
	bool		compressed)	/*!< in: is file space compressed */
{
	buf_tmp_buffer_t *free_slot=NULL;

	/* Array is protected by buf_pool mutex */
	buf_pool_mutex_enter(buf_pool);

	for(ulint i = 0; i < buf_pool->tmp_arr->n_slots; i++) {
		buf_tmp_buffer_t *slot = &buf_pool->tmp_arr->slots[i];

		if(slot->reserved == false) {
			free_slot = slot;
			break;
		}
	}

	/* We assume that free slot is found */
	ut_a(free_slot != NULL);
	free_slot->reserved = true;
	/* Now that we have reserved this slot we can release
	buf_pool mutex */
	buf_pool_mutex_exit(buf_pool);

	/* Allocate temporary memory for encryption/decryption */
	if (free_slot->crypt_buf_free == NULL) {
		free_slot->crypt_buf_free = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE*2));
		free_slot->crypt_buf = static_cast<byte *>(ut_align(free_slot->crypt_buf_free, UNIV_PAGE_SIZE));
		memset(free_slot->crypt_buf_free, 0, UNIV_PAGE_SIZE *2);
	}

	/* For page compressed tables allocate temporary memory for
	compression/decompression */
	if (compressed && free_slot->comp_buf_free == NULL) {
		free_slot->comp_buf_free = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE*2));
		free_slot->comp_buf = static_cast<byte *>(ut_align(free_slot->comp_buf_free, UNIV_PAGE_SIZE));
		memset(free_slot->comp_buf_free, 0, UNIV_PAGE_SIZE *2);
#ifdef HAVE_LZO
		free_slot->lzo_mem = static_cast<byte *>(ut_malloc(LZO1X_1_15_MEM_COMPRESS));
		memset(free_slot->lzo_mem, 0, LZO1X_1_15_MEM_COMPRESS);
#endif
	}

	return (free_slot);
}

/********************************************************************//**
Encrypts a buffer page right before it's flushed to disk
*/
byte*
buf_page_encrypt_before_write(
/*==========================*/
	buf_page_t*	bpage,		/*!< in/out: buffer page to be flushed */
	byte*		src_frame,	/*!< in: src frame */
	ulint		space_id)	/*!< in: space id */
{
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space_id);
	ulint zip_size = buf_page_get_zip_size(bpage);
	ulint page_size = (zip_size) ? zip_size : UNIV_PAGE_SIZE;
	buf_pool_t* buf_pool = buf_pool_from_bpage(bpage);
	bool page_compressed = fil_space_is_page_compressed(bpage->space);
	bool encrypted = true;

	bpage->real_size = UNIV_PAGE_SIZE;

	fil_page_type_validate(src_frame);

	if (bpage->offset == 0) {
		/* Page 0 of a tablespace is not encrypted/compressed */
		ut_ad(bpage->key_version == 0);
		return src_frame;
	}

	if (bpage->space == TRX_SYS_SPACE && bpage->offset == TRX_SYS_PAGE_NO) {
		/* don't encrypt/compress page as it contains address to dblwr buffer */
		bpage->key_version = 0;
		return src_frame;
	}

	if (crypt_data != NULL && crypt_data->not_encrypted()) {
		/* Encryption is disabled */
		encrypted = false;
	}

	if (!srv_encrypt_tables && (crypt_data == NULL || crypt_data->is_default_encryption())) {
		/* Encryption is disabled */
		encrypted = false;
	}

	/* Is encryption needed? */
	if (crypt_data == NULL || crypt_data->type == CRYPT_SCHEME_UNENCRYPTED) {
		/* An unencrypted table */
		bpage->key_version = 0;
		encrypted = false;
	}

	if (!encrypted && !page_compressed) {
		/* No need to encrypt or page compress the page */
		return src_frame;
	}

	/* Find free slot from temporary memory array */
	buf_tmp_buffer_t* slot = buf_pool_reserve_tmp_slot(buf_pool, page_compressed);
	slot->out_buf = NULL;
	bpage->slot = slot;

	byte *dst_frame = slot->crypt_buf;

	if (!page_compressed) {
		/* Encrypt page content */
		byte* tmp = fil_space_encrypt(bpage->space,
					      bpage->offset,
					      bpage->newest_modification,
					      src_frame,
					      zip_size,
					      dst_frame);

		ulint key_version = mach_read_from_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
		ut_ad(key_version == 0 || key_version >= bpage->key_version);
		bpage->key_version = key_version;
		bpage->real_size = page_size;
		slot->out_buf = dst_frame = tmp;

#ifdef UNIV_DEBUG
		fil_page_type_validate(tmp);
#endif

	} else {
		/* First we compress the page content */
		ulint out_len = 0;
		ulint block_size = fil_space_get_block_size(bpage->space, bpage->offset, page_size);

		byte *tmp = fil_compress_page(bpage->space,
					(byte *)src_frame,
					slot->comp_buf,
					page_size,
					fil_space_get_page_compression_level(bpage->space),
					block_size,
					encrypted,
					&out_len,
					IF_LZO(slot->lzo_mem, NULL)
					);

		bpage->real_size = out_len;

#ifdef UNIV_DEBUG
		fil_page_type_validate(tmp);
#endif

		if(encrypted) {

			/* And then we encrypt the page content */
			tmp = fil_space_encrypt(bpage->space,
						bpage->offset,
						bpage->newest_modification,
						tmp,
						zip_size,
						dst_frame);
		}

		slot->out_buf = dst_frame = tmp;
	}

#ifdef UNIV_DEBUG
	fil_page_type_validate(dst_frame);
#endif

	// return dst_frame which will be written
	return dst_frame;
}

/********************************************************************//**
Decrypt page after it has been read from disk
*/
ibool
buf_page_decrypt_after_read(
/*========================*/
	buf_page_t*	bpage)	/*!< in/out: buffer page read from disk */
{
	ulint zip_size = buf_page_get_zip_size(bpage);
	ulint size = (zip_size) ? zip_size : UNIV_PAGE_SIZE;

	byte* dst_frame = (zip_size) ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	unsigned key_version =
		mach_read_from_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	bool page_compressed = fil_page_is_compressed(dst_frame);
	bool page_compressed_encrypted = fil_page_is_compressed_encrypted(dst_frame);
	buf_pool_t* buf_pool = buf_pool_from_bpage(bpage);
	bool success = true;
	ulint 		space_id = mach_read_from_4(
		dst_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space_id);

	/* Page is encrypted if encryption information is found from
	tablespace and page contains used key_version. This is true
	also for pages first compressed and then encrypted. */
	if (!crypt_data ||
	    (crypt_data &&
	     crypt_data->type == CRYPT_SCHEME_UNENCRYPTED &&
	     key_version != 0)) {
		byte*	frame = NULL;

		if (buf_page_get_zip_size(bpage)) {
			frame = bpage->zip.data;
		} else {
			frame = ((buf_block_t*) bpage)->frame;
		}

		/* If page is not corrupted at this point, page can't be
		encrypted, thus set key_version to 0. If page is corrupted,
		we assume at this point that it is encrypted as page
		contained key_version != 0. Note that page could still be
		really corrupted. This we will find out after decrypt by
		checking page checksums. */
		if (!buf_page_is_corrupted(false, frame, buf_page_get_zip_size(bpage))) {
			key_version = 0;
		}
	}

	/* If page is encrypted read post-encryption checksum */
	if (!page_compressed_encrypted && key_version != 0) {
		bpage->stored_checksum = mach_read_from_4(dst_frame +  + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);
	}

	ut_ad(bpage->key_version == 0);

	if (bpage->offset == 0) {
		/* File header pages are not encrypted/compressed */
		return (TRUE);
	}

	/* Store these for corruption check */
	bpage->key_version = key_version;
	bpage->page_encrypted = page_compressed_encrypted;
	bpage->page_compressed = page_compressed;

	if (page_compressed) {
		/* the page we read is unencrypted */
		/* Find free slot from temporary memory array */
		buf_tmp_buffer_t* slot = buf_pool_reserve_tmp_slot(buf_pool, page_compressed);

#ifdef UNIV_DEBUG
		fil_page_type_validate(dst_frame);
#endif

		/* decompress using comp_buf to dst_frame */
		fil_decompress_page(slot->comp_buf,
			dst_frame,
			size,
			&bpage->write_size);

		/* Mark this slot as free */
		slot->reserved = false;
		key_version = 0;

#ifdef UNIV_DEBUG
		fil_page_type_validate(dst_frame);
#endif
	} else {
		buf_tmp_buffer_t* slot = NULL;

		if (key_version) {
			/* Find free slot from temporary memory array */
			slot = buf_pool_reserve_tmp_slot(buf_pool, page_compressed);

#ifdef UNIV_DEBUG
			fil_page_type_validate(dst_frame);
#endif

			/* Calculate checksum before decrypt, this will be
			used later to find out if incorrect key was used. */
			if (!page_compressed_encrypted) {
				bpage->calculated_checksum = fil_crypt_calculate_checksum(zip_size, dst_frame);
			}

			/* decrypt using crypt_buf to dst_frame */
			byte* res = fil_space_decrypt(bpage->space,
						slot->crypt_buf,
						size,
						dst_frame);

			if (!res) {
				bpage->encrypted = true;
				success = false;
			}
#ifdef UNIV_DEBUG
			fil_page_type_validate(dst_frame);
#endif
		}

		if (page_compressed_encrypted && success) {
			if (!slot) {
				slot = buf_pool_reserve_tmp_slot(buf_pool, page_compressed);
			}

#ifdef UNIV_DEBUG
			fil_page_type_validate(dst_frame);
#endif
			/* decompress using comp_buf to dst_frame */
			fil_decompress_page(slot->comp_buf,
					dst_frame,
					size,
					&bpage->write_size);

#ifdef UNIV_DEBUG
			fil_page_type_validate(dst_frame);
#endif
		}

		/* Mark this slot as free */
		if (slot) {
			slot->reserved = false;
		}
	}

	bpage->key_version = key_version;

	return (success);
}
