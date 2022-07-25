/*****************************************************************************

Copyright (c) 1995, 2021, Oracle and/or its affiliates.
Copyright (c) 2008, Google Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

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
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0buf.cc
The database buffer buf_pool

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "mtr0types.h"
#include "mach0data.h"
#include "page0size.h"
#include "buf0buf.h"
#include <string.h>

#ifdef UNIV_NONINL
#include "buf0buf.inl"
#endif

#ifndef UNIV_INNOCHECKSUM
#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "buf0buddy.h"
#include "lock0lock.h"
#include "sync0rw.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "log0log.h"
#include "dict0stats_bg.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "srv0mon.h"
#include "log0crypt.h"
#endif /* !UNIV_INNOCHECKSUM */
#include "page0zip.h"
#include "sync0sync.h"
#include "buf0dump.h"
#include <new>
#include <map>
#include <sstream>
#ifndef UNIV_INNOCHECKSUM
#include "fil0pagecompress.h"
#include "fsp0pagecompress.h"
#endif
#include "ut0byte.h"
#include <new>

#ifdef UNIV_LINUX
#include <stdlib.h>
#endif

#ifdef HAVE_LZO
#include "lzo/lzo1x.h"
#endif

using st_::span;

#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>
struct set_numa_interleave_t
{
	set_numa_interleave_t()
	{
		if (srv_numa_interleave) {

			struct bitmask *numa_mems_allowed = numa_get_mems_allowed();
			ib::info() << "Setting NUMA memory policy to"
				" MPOL_INTERLEAVE";
			if (set_mempolicy(MPOL_INTERLEAVE,
					  numa_mems_allowed->maskp,
					  numa_mems_allowed->size) != 0) {

				ib::warn() << "Failed to set NUMA memory"
					" policy to MPOL_INTERLEAVE: "
					<< strerror(errno);
			}
			numa_bitmask_free(numa_mems_allowed);
		}
	}

	~set_numa_interleave_t()
	{
		if (srv_numa_interleave) {

			ib::info() << "Setting NUMA memory policy to"
				" MPOL_DEFAULT";
			if (set_mempolicy(MPOL_DEFAULT, NULL, 0) != 0) {
				ib::warn() << "Failed to set NUMA memory"
					" policy to MPOL_DEFAULT: "
					<< strerror(errno);
			}
		}
	}
};

#define NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE set_numa_interleave_t scoped_numa
#else
#define NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE
#endif /* HAVE_LIBNUMA */

#ifdef HAVE_SNAPPY
#include "snappy-c.h"
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
The buffer buf_pool contains a single mutex which protects all the
control data structures of the buf_pool. The content of a buffer frame is
protected by a separate read-write lock in its control block, though.
These locks can be locked and unlocked without owning the buf_pool->mutex.
The OS events in the buf_pool struct can be waited for without owning the
buf_pool->mutex.

The buf_pool->mutex is a hot-spot in main memory, causing a lot of
memory bus traffic on multiprocessor systems when processors
alternately access the mutex. On our Pentium, the mutex is accessed
maybe every 10 microseconds. We gave up the solution to have mutexes
for each control block, for instance, because it seemed to be
complicated.

A solution to reduce mutex contention of the buf_pool->mutex is to
create a separate mutex for the page hash table. On Pentium,
accessing the hash table takes 2 microseconds, about half
of the total buf_pool->mutex hold time.

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

#ifndef UNIV_INNOCHECKSUM
/** Value in microseconds */
static const int WAIT_FOR_READ	= 100;
static const int WAIT_FOR_WRITE = 100;
/** Number of attempts made to read in a page in the buffer pool */
static const ulint	BUF_PAGE_READ_MAX_RETRIES = 100;
/** Number of pages to read ahead */
static const ulint	BUF_READ_AHEAD_PAGES = 64;
/** The maximum portion of the buffer pool that can be used for the
read-ahead buffer.  (Divide buf_pool size by this amount) */
static const ulint	BUF_READ_AHEAD_PORTION = 32;

/** The buffer pools of the database */
buf_pool_t*	buf_pool_ptr;

/** true when resizing buffer pool is in the critical path. */
volatile bool	buf_pool_resizing;

/** Map of buffer pool chunks by its first frame address
This is newly made by initialization of buffer pool and buf_resize_thread.
Currently, no need mutex protection for update. */
typedef std::map<
	const byte*,
	buf_chunk_t*,
	std::less<const byte*>,
	ut_allocator<std::pair<const byte* const, buf_chunk_t*> > >
	buf_pool_chunk_map_t;

static buf_pool_chunk_map_t*			buf_chunk_map_reg;

/** Chunk map to be used to lookup.
The map pointed by this should not be updated */
static buf_pool_chunk_map_t*	buf_chunk_map_ref = NULL;

#ifdef UNIV_DEBUG
/** Disable resizing buffer pool to make assertion code not expensive. */
my_bool			buf_disable_resize_buffer_pool_debug = TRUE;
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** This is used to insert validation operations in execution
in the debug version */
static ulint	buf_dbg_counter	= 0;
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

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


/** Reserve a buffer slot for encryption, decryption or page compression.
@param[in,out]	buf_pool	buffer pool
@return reserved buffer slot */
static buf_tmp_buffer_t* buf_pool_reserve_tmp_slot(buf_pool_t* buf_pool)
{
	for (ulint i = 0; i < buf_pool->tmp_arr->n_slots; i++) {
		buf_tmp_buffer_t* slot = &buf_pool->tmp_arr->slots[i];
		if (slot->acquire()) {
			return slot;
		}
	}

	/* We assume that free slot is found */
	ut_error;
	return NULL;
}

/** Reserve a buffer for encryption, decryption or decompression.
@param[in,out]	slot	reserved slot */
static void buf_tmp_reserve_crypt_buf(buf_tmp_buffer_t* slot)
{
	if (!slot->crypt_buf) {
		slot->crypt_buf = static_cast<byte*>(
			aligned_malloc(srv_page_size, srv_page_size));
	}
}

/** Reserve a buffer for compression.
@param[in,out]	slot	reserved slot */
static void buf_tmp_reserve_compression_buf(buf_tmp_buffer_t* slot)
{
	if (!slot->comp_buf) {
		/* Both snappy and lzo compression methods require that
		output buffer used for compression is bigger than input
		buffer. Increase the allocated buffer size accordingly. */
		ulint size = srv_page_size;
#ifdef HAVE_LZO
		size += LZO1X_1_15_MEM_COMPRESS;
#elif defined HAVE_SNAPPY
		size = snappy_max_compressed_length(size);
#endif
		slot->comp_buf = static_cast<byte*>(
			aligned_malloc(size, srv_page_size));
	}
}

/** Registers a chunk to buf_pool_chunk_map
@param[in]	chunk	chunk of buffers */
static
void
buf_pool_register_chunk(
	buf_chunk_t*	chunk)
{
	buf_chunk_map_reg->insert(buf_pool_chunk_map_t::value_type(
		chunk->blocks->frame, chunk));
}

/** Decrypt a page for temporary tablespace.
@param[in,out]	tmp_frame	Temporary buffer
@param[in]	src_frame	Page to decrypt
@return true if temporary tablespace decrypted, false if not */
static bool buf_tmp_page_decrypt(byte* tmp_frame, byte* src_frame)
{
	if (buf_is_zeroes(span<const byte>(src_frame, srv_page_size))) {
		return true;
	}

	/* read space & lsn */
	uint header_len = FIL_PAGE_DATA;

	/* Copy FIL page header, it is not encrypted */
	memcpy(tmp_frame, src_frame, header_len);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + header_len;
	byte* dst = tmp_frame + header_len;
	uint srclen = uint(srv_page_size)
		- header_len - FIL_PAGE_DATA_END;
	ulint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);

	if (!log_tmp_block_decrypt(src, srclen, dst,
				   (offset * srv_page_size))) {
		return false;
	}

	memcpy(tmp_frame + srv_page_size - FIL_PAGE_DATA_END,
	       src_frame + srv_page_size - FIL_PAGE_DATA_END,
	       FIL_PAGE_DATA_END);

	memcpy(src_frame, tmp_frame, srv_page_size);
	srv_stats.pages_decrypted.inc();
	srv_stats.n_temp_blocks_decrypted.inc();

	return true; /* page was decrypted */
}

/** Decrypt a page.
@param[in,out]	bpage	Page control block
@param[in,out]	space	tablespace
@return whether the operation was successful */
static bool buf_page_decrypt_after_read(buf_page_t* bpage, fil_space_t* space)
{
	ut_ad(space->n_pending_ios > 0);
	ut_ad(space->id == bpage->id.space());

	byte* dst_frame = bpage->zip.data ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	bool page_compressed = fil_page_is_compressed(dst_frame);
	buf_pool_t* buf_pool = buf_pool_from_bpage(bpage);

	if (bpage->id.page_no() == 0) {
		/* File header pages are not encrypted/compressed */
		return (true);
	}

	if (space->purpose == FIL_TYPE_TEMPORARY
	    && innodb_encrypt_temporary_tables) {
		buf_tmp_buffer_t* slot = buf_pool_reserve_tmp_slot(buf_pool);
		buf_tmp_reserve_crypt_buf(slot);

		if (!buf_tmp_page_decrypt(slot->crypt_buf, dst_frame)) {
			slot->release();
			ib::error() << "Encrypted page " << bpage->id
				    << " in file " << space->chain.start->name;
			return false;
		}

		slot->release();
		return true;
	}

	/* Page is encrypted if encryption information is found from
	tablespace and page contains used key_version. This is true
	also for pages first compressed and then encrypted. */

	buf_tmp_buffer_t* slot;

	if (page_compressed) {
		/* the page we read is unencrypted */
		/* Find free slot from temporary memory array */
decompress:
		slot = buf_pool_reserve_tmp_slot(buf_pool);
		/* For decompression, use crypt_buf. */
		buf_tmp_reserve_crypt_buf(slot);
decompress_with_slot:
		ut_d(fil_page_type_validate(dst_frame));

		ulint write_size = fil_page_decompress(slot->crypt_buf,
						       dst_frame);
		slot->release();

		ut_ad(!write_size || fil_page_type_validate(dst_frame));
		ut_ad(space->n_pending_ios > 0);
		return write_size != 0;
	}

	if (space->crypt_data
	    && mach_read_from_4(FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
			       + dst_frame)) {
		/* Verify encryption checksum before we even try to
		decrypt. */
		if (!fil_space_verify_crypt_checksum(dst_frame, bpage->size)) {
decrypt_failed:
			ib::error() << "Encrypted page " << bpage->id
				    << " in file " << space->chain.start->name
				    << " looks corrupted; key_version="
				    << mach_read_from_4(
					    FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
					    + dst_frame);
			return false;
		}

		/* Find free slot from temporary memory array */
		slot = buf_pool_reserve_tmp_slot(buf_pool);
		buf_tmp_reserve_crypt_buf(slot);

		ut_d(fil_page_type_validate(dst_frame));

		/* decrypt using crypt_buf to dst_frame */
		if (!fil_space_decrypt(space, slot->crypt_buf, dst_frame)) {
			slot->release();
			goto decrypt_failed;
		}

		ut_d(fil_page_type_validate(dst_frame));

		if (fil_page_is_compressed_encrypted(dst_frame)) {
			goto decompress_with_slot;
		}

		slot->release();
	} else if (fil_page_is_compressed_encrypted(dst_frame)) {
		goto decompress;
	}

	ut_ad(space->n_pending_ios > 0);
	return true;
}

/* prototypes for new functions added to ha_innodb.cc */
trx_t* innobase_get_trx();

/********************************************************************//**
Gets the smallest oldest_modification lsn for any page in the pool. Returns
zero if all modified pages have been flushed to disk.
@return oldest modification in pool, zero if none */
lsn_t
buf_pool_get_oldest_modification(void)
/*==================================*/
{
	lsn_t		lsn = 0;
	lsn_t		oldest_lsn = 0;

	/* When we traverse all the flush lists we don't want another
	thread to add a dirty page to any flush list. */
	log_flush_order_mutex_enter();

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

		buf_page_t*	bpage;

		/* We don't let log-checkpoint halt because pages from system
		temporary are not yet flushed to the disk. Anyway, object
		residing in system temporary doesn't generate REDO logging. */
		for (bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
		     bpage != NULL
			&& fsp_is_system_temporary(bpage->id.space());
		     bpage = UT_LIST_GET_PREV(list, bpage)) {
			/* Do nothing. */
		}

		if (bpage != NULL) {
			ut_ad(bpage->in_flush_list);
			lsn = bpage->oldest_modification;
		}

		buf_flush_list_mutex_exit(buf_pool);

		if (!oldest_lsn || oldest_lsn > lsn) {
			oldest_lsn = lsn;
		}
	}

	log_flush_order_mutex_exit();

	/* The returned answer may be out of date: the flush_list can
	change after the mutex has been released. */

	return(oldest_lsn);
}

/********************************************************************//**
Get total buffer pool statistics. */
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
#endif /* !UNIV_INNOCHECKSUM */

/** Checks if the page is in crc32 checksum format.
@param[in]	read_buf		database page
@param[in]	checksum_field1		new checksum field
@param[in]	checksum_field2		old checksum field
@return true if the page is in crc32 checksum format. */
bool
buf_page_is_checksum_valid_crc32(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
{
	const uint32_t	crc32 = buf_calc_page_crc32(read_buf);

#ifdef UNIV_INNOCHECKSUM
	if (log_file
	    && srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_CRC32) {
		fprintf(log_file, "page::" UINT32PF ";"
			" crc32 calculated = " UINT32PF ";"
			" recorded checksum field1 = " ULINTPF " recorded"
			" checksum field2 =" ULINTPF "\n", cur_page_num,
			crc32, checksum_field1, checksum_field2);
	}
#endif /* UNIV_INNOCHECKSUM */

	if (checksum_field1 != checksum_field2) {
		return false;
	}

	return checksum_field1 == crc32
#ifdef INNODB_BUG_ENDIAN_CRC32
		|| checksum_field1 == buf_calc_page_crc32(read_buf, true)
#endif
		;
}

/** Checks if the page is in innodb checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in innodb checksum format. */
bool
buf_page_is_checksum_valid_innodb(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
{
	/* There are 2 valid formulas for
	checksum_field2 (old checksum field) which algo=innodb could have
	written to the page:

	1. Very old versions of InnoDB only stored 8 byte lsn to the
	start and the end of the page.

	2. Newer InnoDB versions store the old formula checksum
	(buf_calc_page_old_checksum()). */

	ulint	old_checksum = buf_calc_page_old_checksum(read_buf);
	ulint	new_checksum = buf_calc_page_new_checksum(read_buf);

#ifdef UNIV_INNOCHECKSUM
	if (log_file
	    && srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_INNODB) {
		fprintf(log_file, "page::" UINT32PF ";"
			" old style: calculated ="
			" " ULINTPF "; recorded = " ULINTPF "\n",
			cur_page_num, old_checksum,
			checksum_field2);
		fprintf(log_file, "page::" UINT32PF ";"
			" new style: calculated ="
			" " ULINTPF "; crc32 = " UINT32PF "; recorded = " ULINTPF "\n",
			cur_page_num, new_checksum,
			buf_calc_page_crc32(read_buf), checksum_field1);
	}

	if (log_file
	    && srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB) {
		fprintf(log_file, "page::" UINT32PF ";"
			" old style: calculated ="
			" " ULINTPF "; recorded checksum = " ULINTPF "\n",
			cur_page_num, old_checksum,
			checksum_field2);
		fprintf(log_file, "page::" UINT32PF ";"
			" new style: calculated ="
			" " ULINTPF "; recorded checksum  = " ULINTPF "\n",
			cur_page_num, new_checksum,
			checksum_field1);
	}
#endif /* UNIV_INNOCHECKSUM */


	if (checksum_field2 != mach_read_from_4(read_buf + FIL_PAGE_LSN)
	    && checksum_field2 != old_checksum) {
		DBUG_LOG("checksum",
			 "Page checksum crc32 not valid"
			 << " field1 " << checksum_field1
			 << " field2 " << checksum_field2
			 << " crc32 " << buf_calc_page_old_checksum(read_buf)
			 << " lsn " << mach_read_from_4(
				 read_buf + FIL_PAGE_LSN));
		return(false);
	}

	/* old field is fine, check the new field */

	/* InnoDB versions < 4.0.14 and < 4.1.1 stored the space id
	(always equal to 0), to FIL_PAGE_SPACE_OR_CHKSUM */

	if (checksum_field1 != 0 && checksum_field1 != new_checksum) {
		DBUG_LOG("checksum",
			 "Page checksum crc32 not valid"
			 << " field1 " << checksum_field1
			 << " field2 " << checksum_field2
			 << " crc32 " << buf_calc_page_new_checksum(read_buf)
			 << " lsn " << mach_read_from_4(
				 read_buf + FIL_PAGE_LSN));
		return(false);
	}

	return(true);
}

/** Checks if the page is in none checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in none checksum format. */
bool
buf_page_is_checksum_valid_none(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
{
#ifndef DBUG_OFF
	if (checksum_field1 != checksum_field2
	    && checksum_field1 != BUF_NO_CHECKSUM_MAGIC) {
		DBUG_LOG("checksum",
			 "Page checksum crc32 not valid"
			 << " field1 " << checksum_field1
			 << " field2 " << checksum_field2
			 << " crc32 " << BUF_NO_CHECKSUM_MAGIC
			 << " lsn " << mach_read_from_4(read_buf
							+ FIL_PAGE_LSN));
	}
#endif /* DBUG_OFF */

#ifdef UNIV_INNOCHECKSUM
	if (log_file
	    && srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_NONE) {
		fprintf(log_file,
			"page::" UINT32PF "; none checksum: calculated"
			" = %lu; recorded checksum_field1 = " ULINTPF
			" recorded checksum_field2 = " ULINTPF "\n",
			cur_page_num, BUF_NO_CHECKSUM_MAGIC,
			checksum_field1, checksum_field2);
	}
#endif /* UNIV_INNOCHECKSUM */

	return(checksum_field1 == checksum_field2
	       && checksum_field1 == BUF_NO_CHECKSUM_MAGIC);
}

#ifdef INNODB_BUG_ENDIAN_CRC32
/** Validate the CRC-32C checksum of a page.
@param[in]	page		buffer page (srv_page_size bytes)
@param[in]	checksum	CRC-32C checksum stored on page
@return	computed checksum */
static uint32_t buf_page_check_crc32(const byte* page, uint32_t checksum)
{
	uint32_t crc32 = buf_calc_page_crc32(page);

	if (checksum != crc32) {
		crc32 = buf_calc_page_crc32(page, true);
	}

	return crc32;
}
#else /* INNODB_BUG_ENDIAN_CRC32 */
/** Validate the CRC-32C checksum of a page.
@param[in]	page		buffer page (srv_page_size bytes)
@param[in]	checksum	CRC-32C checksum stored on page
@return	computed checksum */
# define buf_page_check_crc32(page, checksum) buf_calc_page_crc32(page)
#endif /* INNODB_BUG_ENDIAN_CRC32 */


/** Check if a buffer is all zeroes.
@param[in]	buf	data to check
@return whether the buffer is all zeroes */
bool buf_is_zeroes(span<const byte> buf)
{
  ut_ad(buf.size() <= sizeof field_ref_zero);
  return memcmp(buf.data(), field_ref_zero, buf.size()) == 0;
}

/** Check if a page is corrupt.
@param[in]	check_lsn	whether the LSN should be checked
@param[in]	read_buf	database page
@param[in]	page_size	page size
@param[in]	space		tablespace
@return whether the page is corrupted */
bool
buf_page_is_corrupted(
	bool			check_lsn,
	const byte*		read_buf,
	const page_size_t&	page_size,
#ifndef UNIV_INNOCHECKSUM
	const fil_space_t* 	space)
#else
	const void* 	 	space)
#endif
{
	ut_ad(page_size.logical() == srv_page_size);
#ifndef UNIV_INNOCHECKSUM
	DBUG_EXECUTE_IF("buf_page_import_corrupt_failure", return(true); );
#endif
	size_t		checksum_field1 = 0;
	size_t		checksum_field2 = 0;

	ulint page_type = mach_read_from_2(read_buf + FIL_PAGE_TYPE);

	/* We can trust page type if page compression is set on tablespace
	flags because page compression flag means file must have been
	created with 10.1 (later than 5.5 code base). In 10.1 page
	compressed tables do not contain post compression checksum and
	FIL_PAGE_END_LSN_OLD_CHKSUM field stored. Note that space can
	be null if we are in fil_check_first_page() and first page
	is not compressed or encrypted. Page checksum is verified
	after decompression (i.e. normally pages are already
	decompressed at this stage). */
	if ((page_type == FIL_PAGE_PAGE_COMPRESSED ||
	     page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED)
#ifndef UNIV_INNOCHECKSUM
	    && space && FSP_FLAGS_HAS_PAGE_COMPRESSION(space->flags)
#endif
	) {
		return(false);
	}

	if (!page_size.is_compressed()
	    && memcmp(read_buf + FIL_PAGE_LSN + 4,
		      read_buf + page_size.logical()
		      - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4)) {

		/* Stored log sequence numbers at the start and the end
		of page do not match */

		return(true);
	}

#ifndef UNIV_INNOCHECKSUM
	if (check_lsn && recv_lsn_checks_on) {
		lsn_t		current_lsn;
		const lsn_t	page_lsn
			= mach_read_from_8(read_buf + FIL_PAGE_LSN);

		/* Since we are going to reset the page LSN during the import
		phase it makes no sense to spam the log with error messages. */

		if (log_peek_lsn(&current_lsn) && current_lsn < page_lsn) {

			const ulint	space_id = mach_read_from_4(
				read_buf + FIL_PAGE_SPACE_ID);
			const ulint	page_no = mach_read_from_4(
				read_buf + FIL_PAGE_OFFSET);

			ib::error() << "Page " << page_id_t(space_id, page_no)
				<< " log sequence number " << page_lsn
				<< " is in the future! Current system"
				<< " log sequence number "
				<< current_lsn << ".";

			ib::error() << "Your database may be corrupt or"
				" you may have copied the InnoDB"
				" tablespace but not the InnoDB"
				" log files. "
				<< FORCE_RECOVERY_MSG;

		}
	}
#endif /* !UNIV_INNOCHECKSUM */

	/* Check whether the checksum fields have correct values */

	const srv_checksum_algorithm_t curr_algo =
		static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);

	if (curr_algo == SRV_CHECKSUM_ALGORITHM_NONE) {
		return(false);
	}

	if (page_size.is_compressed()) {
		return(!page_zip_verify_checksum(read_buf,
						 page_size.physical()));
	}

	checksum_field1 = mach_read_from_4(
		read_buf + FIL_PAGE_SPACE_OR_CHKSUM);

	checksum_field2 = mach_read_from_4(
		read_buf + page_size.logical() - FIL_PAGE_END_LSN_OLD_CHKSUM);

#if FIL_PAGE_LSN % 8
#error "FIL_PAGE_LSN must be 64 bit aligned"
#endif

	/* A page filled with NUL bytes is considered not corrupted.
	The FIL_PAGE_FILE_FLUSH_LSN field may be written nonzero for
	the first page of the system tablespace.
	Ignore it for the system tablespace. */
	if (!checksum_field1 && !checksum_field2) {
		/* Checksum fields can have valid value as zero.
		If the page is not empty then do the checksum
		calculation for the page. */
		bool all_zeroes = true;
		for (size_t i = 0; i < srv_page_size; i++) {
#ifndef UNIV_INNOCHECKSUM
			if (i == FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
			    && (!space || !space->id)) {
				i += 8;
			}
#endif
			if (read_buf[i]) {
				all_zeroes = false;
				break;
			}
		}

		if (all_zeroes) {
			return false;
		}
	}

	switch (curr_algo) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		return !buf_page_is_checksum_valid_crc32(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		return !buf_page_is_checksum_valid_innodb(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		return !buf_page_is_checksum_valid_none(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_NONE:
		/* should have returned false earlier */
		break;
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_INNODB:
		const uint32_t crc32 = buf_calc_page_crc32(read_buf);

		if (buf_page_is_checksum_valid_none(read_buf,
			checksum_field1, checksum_field2)) {
#ifdef UNIV_INNOCHECKSUM
			if (log_file) {
				fprintf(log_file, "page::" UINT32PF ";"
					" old style: calculated = %u;"
					" recorded = " ULINTPF ";\n",
					cur_page_num,
					buf_calc_page_old_checksum(read_buf),
					checksum_field2);
				fprintf(log_file, "page::" UINT32PF ";"
					" new style: calculated = " UINT32PF ";"
					" crc32 = " UINT32PF "; recorded = " ULINTPF ";\n",
					cur_page_num,
					buf_calc_page_new_checksum(read_buf),
					crc32,
					checksum_field1);
			}
#endif /* UNIV_INNOCHECKSUM */
			return false;
		}

		/* Very old versions of InnoDB only stored 8 byte lsn to the
		start and the end of the page. */

		/* Since innodb_checksum_algorithm is not strict_* allow
		any of the algos to match for the old field */

		if (checksum_field2
		    != mach_read_from_4(read_buf + FIL_PAGE_LSN)
		    && checksum_field2 != BUF_NO_CHECKSUM_MAGIC) {

			DBUG_EXECUTE_IF(
				"page_intermittent_checksum_mismatch", {
				static int page_counter;
				if (page_counter++ == 2) return true;
			});

			if ((checksum_field1 != crc32
			     || checksum_field2 != crc32)
			    && checksum_field2
			    != buf_calc_page_old_checksum(read_buf)) {
				return true;
			}
		}

		switch (checksum_field1) {
		case 0:
		case BUF_NO_CHECKSUM_MAGIC:
			break;
		default:
			if ((checksum_field1 != crc32
			     || checksum_field2 != crc32)
			    && checksum_field1
			    != buf_calc_page_new_checksum(read_buf)) {
				return true;
			}
		}

		break;
	}

	return false;
}

#ifndef UNIV_INNOCHECKSUM
/** Dump a page to stderr.
@param[in]	read_buf	database page
@param[in]	page_size	page size */
UNIV_INTERN
void
buf_page_print(const byte* read_buf, const page_size_t& page_size)
{
	dict_index_t*	index;

#ifndef UNIV_DEBUG
	ib::info() << "Page dump in ascii and hex ("
		<< page_size.physical() << " bytes):";

	ut_print_buf(stderr, read_buf, page_size.physical());
	fputs("\nInnoDB: End of page dump\n", stderr);
#endif

	if (page_size.is_compressed()) {
		/* Print compressed page. */
		ib::info() << "Compressed page type ("
			<< fil_page_get_type(read_buf)
			<< "); stored checksum in field1 "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_SPACE_OR_CHKSUM)
			<< "; calculated checksums for field1: "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32)
			<< " "
			<< page_zip_calc_checksum(
				read_buf, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_CRC32)
#ifdef INNODB_BUG_ENDIAN_CRC32
			<< "/"
			<< page_zip_calc_checksum(
				read_buf, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_CRC32, true)
#endif
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_INNODB)
			<< " "
			<< page_zip_calc_checksum(
				read_buf, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_INNODB)
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_NONE)
			<< " "
			<< page_zip_calc_checksum(
				read_buf, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_NONE)
			<< "; page LSN "
			<< mach_read_from_8(read_buf + FIL_PAGE_LSN)
			<< "; page number (if stored to page"
			<< " already) "
			<< mach_read_from_4(read_buf + FIL_PAGE_OFFSET)
			<< "; space id (if stored to page already) "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

	} else {
		const uint32_t	crc32 = buf_calc_page_crc32(read_buf);
#ifdef INNODB_BUG_ENDIAN_CRC32
		const uint32_t	crc32_legacy = buf_calc_page_crc32(read_buf,
								   true);
#endif /* INNODB_BUG_ENDIAN_CRC32 */
		ulint page_type = fil_page_get_type(read_buf);

		ib::info() << "Uncompressed page, stored checksum in field1 "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_SPACE_OR_CHKSUM)
			<< ", calculated checksums for field1: "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32) << " "
			<< crc32
#ifdef INNODB_BUG_ENDIAN_CRC32
			<< "/" << crc32_legacy
#endif
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_INNODB) << " "
			<< buf_calc_page_new_checksum(read_buf)
			<< ", "
			<< " page type " << page_type << " == "
			<< fil_get_page_type_name(page_type) << "."
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_NONE) << " "
			<< BUF_NO_CHECKSUM_MAGIC
			<< ", stored checksum in field2 "
			<< mach_read_from_4(read_buf + page_size.logical()
					    - FIL_PAGE_END_LSN_OLD_CHKSUM)
			<< ", calculated checksums for field2: "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32) << " "
			<< crc32
#ifdef INNODB_BUG_ENDIAN_CRC32
			<< "/" << crc32_legacy
#endif
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_INNODB) << " "
			<< buf_calc_page_old_checksum(read_buf)
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_NONE) << " "
			<< BUF_NO_CHECKSUM_MAGIC
			<< ",  page LSN "
			<< mach_read_from_4(read_buf + FIL_PAGE_LSN)
			<< " "
			<< mach_read_from_4(read_buf + FIL_PAGE_LSN + 4)
			<< ", low 4 bytes of LSN at page end "
			<< mach_read_from_4(read_buf + page_size.logical()
					    - FIL_PAGE_END_LSN_OLD_CHKSUM + 4)
			<< ", page number (if stored to page already) "
			<< mach_read_from_4(read_buf + FIL_PAGE_OFFSET)
			<< ", space id (if created with >= MySQL-4.1.1"
			   " and stored already) "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	}

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

	switch (fil_page_get_type(read_buf)) {
		index_id_t	index_id;
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		index_id = btr_page_get_index_id(read_buf);
		ib::info() << "Page may be an index page where"
			" index id is " << index_id;

		index = dict_index_find_on_id_low(index_id);
		if (index) {
			ib::info()
				<< "Index " << index_id
				<< " is " << index->name
				<< " in table " << index->table->name;
		}
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
}

# ifdef PFS_GROUP_BUFFER_SYNC
extern mysql_pfs_key_t	buffer_block_mutex_key;

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
	buf_block_t*    block;
	ulint		num_to_register;

	block = chunk->blocks;

	num_to_register = ut_min(
		chunk->size, PFS_MAX_BUFFER_MUTEX_LOCK_REGISTER);

	for (ulint i = 0; i < num_to_register; i++) {
#  ifdef UNIV_PFS_MUTEX
		BPageMutex*	mutex;

		mutex = &block->mutex;
		mutex->pfs_add(buffer_block_mutex_key);
#  endif /* UNIV_PFS_MUTEX */

		rw_lock_t*	rwlock;

#  ifdef UNIV_PFS_RWLOCK
		rwlock = &block->lock;
		ut_a(!rwlock->pfs_psi);
		rwlock->pfs_psi = (PSI_server)
			? PSI_server->init_rwlock(buf_block_lock_key, rwlock)
			: NULL;

#   ifdef UNIV_DEBUG
		rwlock = &block->debug_latch;
		ut_a(!rwlock->pfs_psi);
		rwlock->pfs_psi = (PSI_server)
			? PSI_server->init_rwlock(buf_block_debug_latch_key,
						  rwlock)
			: NULL;
#   endif /* UNIV_DEBUG */

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
	/* This function should only be executed at database startup or by
	buf_pool_resize(). Either way, adaptive hash index must not exist. */
	assert_block_ahi_empty_on_init(block);

	block->frame = frame;

	block->page.buf_pool_index = buf_pool_index(buf_pool);
	block->page.flush_type = BUF_FLUSH_LRU;
	block->page.state = BUF_BLOCK_NOT_USED;
	block->page.buf_fix_count = 0;
	block->page.io_fix = BUF_IO_NONE;
	block->page.flush_observer = NULL;
	block->page.real_size = 0;
	block->modify_clock = 0;
	block->page.slot = NULL;

	ut_d(block->page.file_page_was_freed = FALSE);

#ifdef BTR_CUR_HASH_ADAPT
	block->index = NULL;
#endif /* BTR_CUR_HASH_ADAPT */
	ut_d(block->page.in_page_hash = FALSE);
	ut_d(block->page.in_zip_hash = FALSE);
	ut_d(block->page.in_flush_list = FALSE);
	ut_d(block->page.in_free_list = FALSE);
	ut_d(block->page.in_LRU_list = FALSE);
	ut_d(block->in_unzip_LRU_list = FALSE);
	ut_d(block->in_withdraw_list = FALSE);

	page_zip_des_init(&block->page.zip);

	mutex_create(LATCH_ID_BUF_BLOCK_MUTEX, &block->mutex);

#if defined PFS_SKIP_BUFFER_MUTEX_RWLOCK || defined PFS_GROUP_BUFFER_SYNC
	/* If PFS_SKIP_BUFFER_MUTEX_RWLOCK is defined, skip registration
	of buffer block rwlock with performance schema.

	If PFS_GROUP_BUFFER_SYNC is defined, skip the registration
	since buffer block rwlock will be registered later in
	pfs_register_buffer_block(). */

	rw_lock_create(PFS_NOT_INSTRUMENTED, &block->lock, SYNC_LEVEL_VARYING);

	ut_d(rw_lock_create(PFS_NOT_INSTRUMENTED, &block->debug_latch,
			    SYNC_LEVEL_VARYING));

#else /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */

	rw_lock_create(buf_block_lock_key, &block->lock, SYNC_LEVEL_VARYING);

	ut_d(rw_lock_create(buf_block_debug_latch_key,
			    &block->debug_latch, SYNC_LEVEL_VARYING));

#endif /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */

	block->lock.is_block_lock = 1;

	ut_ad(rw_lock_validate(&(block->lock)));
}

/********************************************************************//**
Allocates a chunk of buffer frames.
@return chunk, or NULL on failure */
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

	/* Round down to a multiple of page size,
	although it already should be. */
	mem_size = ut_2pow_round(mem_size, UNIV_PAGE_SIZE);
	/* Reserve space for the block descriptors. */
	mem_size += ut_2pow_round((mem_size / UNIV_PAGE_SIZE) * (sizeof *block)
				  + (UNIV_PAGE_SIZE - 1), UNIV_PAGE_SIZE);

	DBUG_EXECUTE_IF("ib_buf_chunk_init_fails", return(NULL););

	chunk->mem = buf_pool->allocator.allocate_large(mem_size,
							&chunk->mem_pfx);

	if (UNIV_UNLIKELY(chunk->mem == NULL)) {

		return(NULL);
	}

#ifdef HAVE_LIBNUMA
	if (srv_numa_interleave) {
		struct bitmask *numa_mems_allowed = numa_get_mems_allowed();
		int	st = mbind(chunk->mem, chunk->mem_size(),
				   MPOL_INTERLEAVE,
				   numa_mems_allowed->maskp,
				   numa_mems_allowed->size,
				   MPOL_MF_MOVE);
		if (st != 0) {
			ib::warn() << "Failed to set NUMA memory policy of"
				" buffer pool page frames to MPOL_INTERLEAVE"
				" (error: " << strerror(errno) << ").";
		}
		numa_bitmask_free(numa_mems_allowed);
	}
#endif /* HAVE_LIBNUMA */


	/* Allocate the block descriptors from
	the start of the memory block. */
	chunk->blocks = (buf_block_t*) chunk->mem;

	/* Align a pointer to the first frame.  Note that when
	opt_large_page_size is smaller than UNIV_PAGE_SIZE,
	we may allocate one fewer block than requested.  When
	it is bigger, we may allocate more blocks than requested. */

	frame = (byte*) ut_align(chunk->mem, UNIV_PAGE_SIZE);
	chunk->size = chunk->mem_pfx.m_size / UNIV_PAGE_SIZE
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

	/* Init block structs and assign frames for them. Then we
	assign the frames to the first blocks (we already mapped the
	memory above). */

	block = chunk->blocks;

	for (i = chunk->size; i--; ) {

		buf_block_init(buf_pool, block, frame);
		MEM_UNDEFINED(block->frame, srv_page_size);

		/* Add the block to the free list */
		UT_LIST_ADD_LAST(buf_pool->free, &block->page);

		ut_d(block->page.in_free_list = TRUE);
		ut_ad(buf_pool_from_block(block) == buf_pool);

		block++;
		frame += UNIV_PAGE_SIZE;
	}

	buf_pool_register_chunk(chunk);

#ifdef PFS_GROUP_BUFFER_SYNC
	pfs_register_buffer_block(chunk);
#endif /* PFS_GROUP_BUFFER_SYNC */
	return(chunk);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Finds a block in the given buffer chunk that points to a
given compressed page.
@return buffer block pointing to the compressed page, or NULL */
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
@return buffer block pointing to the compressed page, or NULL */
buf_block_t*
buf_pool_contains_zip(
/*==================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	const void*	data)		/*!< in: pointer to compressed page */
{
	ulint		n;
	buf_chunk_t*	chunk = buf_pool->chunks;

	ut_ad(buf_pool);
	ut_ad(buf_pool_mutex_own(buf_pool));
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
@return address of a non-free block, or NULL if all freed */
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
			if (srv_read_only_mode) {
				/* The page cleaner is disabled in
				read-only mode.  No pages can be
				dirtied, so all of them must be clean. */
				ut_ad(block->page.oldest_modification
				      == block->page.newest_modification);
				ut_ad(block->page.oldest_modification == 0
				      || block->page.oldest_modification
				      == recv_sys->recovered_lsn
				      || srv_force_recovery
				      == SRV_FORCE_NO_LOG_REDO);
				ut_ad(block->page.buf_fix_count == 0);
				ut_ad(block->page.io_fix == BUF_IO_NONE);
				break;
			}

			buf_page_mutex_enter(block);
			ready = buf_flush_ready_for_replace(&block->page);
			buf_page_mutex_exit(block);

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

	buf_pool_mutex_enter_all();

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);
		curr_size += buf_pool->curr_pool_size;
	}

	srv_buf_pool_curr_size = curr_size;
	srv_buf_pool_old_size = srv_buf_pool_size;
	srv_buf_pool_base_size = srv_buf_pool_size;

	buf_pool_mutex_exit_all();
}

/********************************************************************//**
Initialize a buffer pool instance.
@return DB_SUCCESS if all goes well. */
static
ulint
buf_pool_init_instance(
/*===================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		buf_pool_size,	/*!< in: size in bytes */
	ulint		instance_no)	/*!< in: id of the instance */
{
	ulint		i;
	ulint		chunk_size;
	buf_chunk_t*	chunk;

	ut_ad(buf_pool_size % srv_buf_pool_chunk_unit == 0);

	/* 1. Initialize general fields
	------------------------------- */
	mutex_create(LATCH_ID_BUF_POOL, &buf_pool->mutex);

	mutex_create(LATCH_ID_BUF_POOL_ZIP, &buf_pool->zip_mutex);

	new(&buf_pool->allocator)
		ut_allocator<unsigned char>(mem_key_buf_buf_pool);

	buf_pool_mutex_enter(buf_pool);

	if (buf_pool_size > 0) {
		buf_pool->n_chunks
			= buf_pool_size / srv_buf_pool_chunk_unit;
		chunk_size = srv_buf_pool_chunk_unit;

		buf_pool->chunks =
			reinterpret_cast<buf_chunk_t*>(ut_zalloc_nokey(
				buf_pool->n_chunks * sizeof(*chunk)));
		buf_pool->chunks_old = NULL;

		UT_LIST_INIT(buf_pool->LRU, &buf_page_t::LRU);
		UT_LIST_INIT(buf_pool->free, &buf_page_t::list);
		UT_LIST_INIT(buf_pool->withdraw, &buf_page_t::list);
		buf_pool->withdraw_target = 0;
		UT_LIST_INIT(buf_pool->flush_list, &buf_page_t::list);
		UT_LIST_INIT(buf_pool->unzip_LRU, &buf_block_t::unzip_LRU);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		UT_LIST_INIT(buf_pool->zip_clean, &buf_page_t::list);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		for (i = 0; i < UT_ARR_SIZE(buf_pool->zip_free); ++i) {
			UT_LIST_INIT(
				buf_pool->zip_free[i], &buf_buddy_free_t::list);
		}

		buf_pool->curr_size = 0;
		chunk = buf_pool->chunks;

		do {
			if (!buf_chunk_init(buf_pool, chunk, chunk_size)) {
				while (--chunk >= buf_pool->chunks) {
					buf_block_t*	block = chunk->blocks;

					for (i = chunk->size; i--; block++) {
						mutex_free(&block->mutex);
						rw_lock_free(&block->lock);

						ut_d(rw_lock_free(
							&block->debug_latch));
					}

					buf_pool->allocator.deallocate_large(
						chunk->mem, &chunk->mem_pfx);
				}
				ut_free(buf_pool->chunks);
				buf_pool_mutex_exit(buf_pool);

				/* InnoDB should free the mutex which was
				created so far before freeing the instance */
				mutex_free(&buf_pool->mutex);
				mutex_free(&buf_pool->zip_mutex);
				return(DB_ERROR);
			}

			buf_pool->curr_size += chunk->size;
		} while (++chunk < buf_pool->chunks + buf_pool->n_chunks);

		buf_pool->instance_no = instance_no;
		buf_pool->read_ahead_area =
			ut_min(BUF_READ_AHEAD_PAGES,
			       ut_2_power_up(buf_pool->curr_size /
					     BUF_READ_AHEAD_PORTION));
		buf_pool->curr_pool_size = buf_pool->curr_size * UNIV_PAGE_SIZE;

		buf_pool->old_size = buf_pool->curr_size;
		buf_pool->n_chunks_new = buf_pool->n_chunks;

		/* Number of locks protecting page_hash must be a
		power of two */
		srv_n_page_hash_locks = static_cast<ulong>(
			 ut_2_power_up(srv_n_page_hash_locks));
		ut_a(srv_n_page_hash_locks != 0);
		ut_a(srv_n_page_hash_locks <= MAX_PAGE_HASH_LOCKS);

		buf_pool->page_hash = ib_create(
			2 * buf_pool->curr_size,
			LATCH_ID_HASH_TABLE_RW_LOCK,
			srv_n_page_hash_locks, MEM_HEAP_FOR_PAGE_HASH);

		buf_pool->zip_hash = hash_create(2 * buf_pool->curr_size);

		buf_pool->last_printout_time = time(NULL);
	}
	/* 2. Initialize flushing fields
	-------------------------------- */

	mutex_create(LATCH_ID_FLUSH_LIST, &buf_pool->flush_list_mutex);

	for (i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {
		buf_pool->no_flush[i] = os_event_create(0);
	}

	buf_pool->watch = (buf_page_t*) ut_zalloc_nokey(
		sizeof(*buf_pool->watch) * BUF_POOL_WATCH_SIZE);
	for (i = 0; i < BUF_POOL_WATCH_SIZE; i++) {
		buf_pool->watch[i].buf_pool_index
			= unsigned(buf_pool->instance_no);
	}

	/* All fields are initialized by ut_zalloc_nokey(). */

	buf_pool->try_LRU_scan = TRUE;

	/* Initialize the hazard pointer for flush_list batches */
	new(&buf_pool->flush_hp)
		FlushHp(buf_pool, &buf_pool->flush_list_mutex);

	/* Initialize the hazard pointer for LRU batches */
	new(&buf_pool->lru_hp) LRUHp(buf_pool, &buf_pool->mutex);

	/* Initialize the iterator for LRU scan search */
	new(&buf_pool->lru_scan_itr) LRUItr(buf_pool, &buf_pool->mutex);

	/* Initialize the iterator for single page scan search */
	new(&buf_pool->single_scan_itr) LRUItr(buf_pool, &buf_pool->mutex);

	/* Initialize the temporal memory array and slots */
	buf_pool->tmp_arr = (buf_tmp_array_t *)ut_malloc_nokey(sizeof(buf_tmp_array_t));
	memset(buf_pool->tmp_arr, 0, sizeof(buf_tmp_array_t));
	ulint n_slots = (srv_n_read_io_threads + srv_n_write_io_threads) * (8 * OS_AIO_N_PENDING_IOS_PER_THREAD);
	buf_pool->tmp_arr->n_slots = n_slots;
	buf_pool->tmp_arr->slots = (buf_tmp_buffer_t*)ut_malloc_nokey(sizeof(buf_tmp_buffer_t) * n_slots);
	memset(buf_pool->tmp_arr->slots, 0, (sizeof(buf_tmp_buffer_t) * n_slots));

	buf_pool_mutex_exit(buf_pool);

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
	buf_page_t*	prev_bpage = 0;

	mutex_free(&buf_pool->mutex);
	mutex_free(&buf_pool->zip_mutex);
	mutex_free(&buf_pool->flush_list_mutex);

	if (buf_pool->flush_rbt) {
		rbt_free(buf_pool->flush_rbt);
		buf_pool->flush_rbt = NULL;
	}

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     bpage != NULL;
	     bpage = prev_bpage) {

		prev_bpage = UT_LIST_GET_PREV(LRU, bpage);
		buf_page_state	state = buf_page_get_state(bpage);

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_LRU_list);

		if (state != BUF_BLOCK_FILE_PAGE) {
			/* We must not have any dirty block except
			when doing a fast shutdown. */
			ut_ad(state == BUF_BLOCK_ZIP_PAGE
			      || srv_fast_shutdown == 2);
			buf_page_free_descriptor(bpage);
		}
	}

	ut_free(buf_pool->watch);
	buf_pool->watch = NULL;

	chunks = buf_pool->chunks;
	chunk = chunks + buf_pool->n_chunks;

	while (--chunk >= chunks) {
		buf_block_t*	block = chunk->blocks;

		for (ulint i = chunk->size; i--; block++) {
			mutex_free(&block->mutex);
			rw_lock_free(&block->lock);

			ut_d(rw_lock_free(&block->debug_latch));
		}

		buf_pool->allocator.deallocate_large(
			chunk->mem, &chunk->mem_pfx);
	}

	for (ulint i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; ++i) {
		os_event_destroy(buf_pool->no_flush[i]);
	}

	ut_free(buf_pool->chunks);
	ha_clear(buf_pool->page_hash);
	hash_table_free(buf_pool->page_hash);
	hash_table_free(buf_pool->zip_hash);

	/* Free all used temporary slots */
	if (buf_pool->tmp_arr) {
		for(ulint i = 0; i < buf_pool->tmp_arr->n_slots; i++) {
			buf_tmp_buffer_t* slot = &(buf_pool->tmp_arr->slots[i]);
			if (slot && slot->crypt_buf) {
				aligned_free(slot->crypt_buf);
				slot->crypt_buf = NULL;
			}

			if (slot && slot->comp_buf) {
				aligned_free(slot->comp_buf);
				slot->comp_buf = NULL;
			}
		}

		ut_free(buf_pool->tmp_arr->slots);
		ut_free(buf_pool->tmp_arr);
		buf_pool->tmp_arr = NULL;
	}

	buf_pool->allocator.~ut_allocator();
}

/********************************************************************//**
Creates the buffer pool.
@return DB_SUCCESS if success, DB_ERROR if not enough memory or error */
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

	NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;

	buf_pool_resizing = false;

	buf_pool_ptr = (buf_pool_t*) ut_zalloc_nokey(
		n_instances * sizeof *buf_pool_ptr);

	buf_chunk_map_reg = UT_NEW_NOKEY(buf_pool_chunk_map_t());

	for (i = 0; i < n_instances; i++) {
		buf_pool_t*	ptr	= &buf_pool_ptr[i];

		if (buf_pool_init_instance(ptr, size, i) != DB_SUCCESS) {

			/* Free all the instances created so far. */
			buf_pool_free(i);

			return(DB_ERROR);
		}
	}

	buf_chunk_map_ref = buf_chunk_map_reg;

	buf_pool_set_sizes();
	buf_LRU_old_ratio_update(100 * 3/ 8, FALSE);

	btr_search_sys_create(buf_pool_get_curr_size() / sizeof(void*) / 64);

	return(DB_SUCCESS);
}

/********************************************************************//**
Frees the buffer pool at shutdown.  This must not be invoked before
freeing all mutexes. */
void
buf_pool_free(
/*==========*/
	ulint	n_instances)	/*!< in: numbere of instances to free */
{
	for (ulint i = 0; i < n_instances; i++) {
		buf_pool_free_instance(buf_pool_from_array(i));
	}

	UT_DELETE(buf_chunk_map_reg);
	buf_chunk_map_reg = buf_chunk_map_ref = NULL;

	ut_free(buf_pool_ptr);
	buf_pool_ptr = NULL;
}

/** Reallocate a control block.
@param[in]	buf_pool	buffer pool instance
@param[in]	block		pointer to control block
@retval false	if failed because of no free blocks. */
static
bool
buf_page_realloc(
	buf_pool_t*	buf_pool,
	buf_block_t*	block)
{
	buf_block_t*	new_block;

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	new_block = buf_LRU_get_free_only(buf_pool);

	if (new_block == NULL) {
		return(false); /* free_list was not enough */
	}

	rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool, block->page.id);

	rw_lock_x_lock(hash_lock);
	mutex_enter(&block->mutex);

	if (buf_page_can_relocate(&block->page)) {
		mutex_enter(&new_block->mutex);

		memcpy(new_block->frame, block->frame, srv_page_size);
		new (&new_block->page) buf_page_t(block->page);

		/* relocate LRU list */
		ut_ad(block->page.in_LRU_list);
		ut_ad(!block->page.in_zip_hash);
		ut_d(block->page.in_LRU_list = FALSE);

		buf_LRU_adjust_hp(buf_pool, &block->page);

		buf_page_t*	prev_b = UT_LIST_GET_PREV(LRU, &block->page);
		UT_LIST_REMOVE(buf_pool->LRU, &block->page);

		if (prev_b != NULL) {
			UT_LIST_INSERT_AFTER(buf_pool->LRU, prev_b, &new_block->page);
		} else {
			UT_LIST_ADD_FIRST(buf_pool->LRU, &new_block->page);
		}

		if (buf_pool->LRU_old == &block->page) {
			buf_pool->LRU_old = &new_block->page;
		}

		ut_ad(new_block->page.in_LRU_list);

		/* relocate unzip_LRU list */
		if (block->page.zip.data != NULL) {
			ut_ad(block->in_unzip_LRU_list);
			ut_d(new_block->in_unzip_LRU_list = TRUE);

			buf_block_t*	prev_block = UT_LIST_GET_PREV(unzip_LRU, block);
			UT_LIST_REMOVE(buf_pool->unzip_LRU, block);

			ut_d(block->in_unzip_LRU_list = FALSE);
			block->page.zip.data = NULL;
			page_zip_set_size(&block->page.zip, 0);

			if (prev_block != NULL) {
				UT_LIST_INSERT_AFTER(buf_pool->unzip_LRU, prev_block, new_block);
			} else {
				UT_LIST_ADD_FIRST(buf_pool->unzip_LRU, new_block);
			}
		} else {
			ut_ad(!block->in_unzip_LRU_list);
			ut_d(new_block->in_unzip_LRU_list = FALSE);
		}

		/* relocate buf_pool->page_hash */
		ut_ad(block->page.in_page_hash);
		ut_ad(&block->page == buf_page_hash_get_low(buf_pool,
							    block->page.id));
		ut_d(block->page.in_page_hash = FALSE);
		ulint	fold = block->page.id.fold();
		ut_ad(fold == new_block->page.id.fold());
		HASH_REPLACE(buf_page_t, hash, buf_pool->page_hash, fold,
			     &block->page, &new_block->page);

		ut_ad(new_block->page.in_page_hash);

		buf_block_modify_clock_inc(block);
		memset(block->frame + FIL_PAGE_OFFSET, 0xff, 4);
		memset(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		MEM_UNDEFINED(block->frame, srv_page_size);
		buf_block_set_state(block, BUF_BLOCK_REMOVE_HASH);
		block->page.id
		    = page_id_t(ULINT32_UNDEFINED, ULINT32_UNDEFINED);

		/* Relocate buf_pool->flush_list. */
		if (block->page.oldest_modification) {
			buf_flush_relocate_on_flush_list(
				&block->page, &new_block->page);
		}

		/* set other flags of buf_block_t */

#ifdef BTR_CUR_HASH_ADAPT
		/* This code should only be executed by buf_pool_resize(),
		while the adaptive hash index is disabled. */
		assert_block_ahi_empty(block);
		assert_block_ahi_empty_on_init(new_block);
		ut_ad(!block->index);
		new_block->index	= NULL;
		new_block->n_hash_helps	= 0;
		new_block->n_fields	= 1;
		new_block->left_side	= TRUE;
#endif /* BTR_CUR_HASH_ADAPT */

		new_block->lock_hash_val = block->lock_hash_val;
		ut_ad(new_block->lock_hash_val == lock_rec_hash(
			new_block->page.id.space(),
			new_block->page.id.page_no()));

		rw_lock_x_unlock(hash_lock);
		mutex_exit(&new_block->mutex);

		/* free block */
		buf_block_set_state(block, BUF_BLOCK_MEMORY);
		buf_LRU_block_free_non_file_page(block);

		mutex_exit(&block->mutex);
	} else {
		rw_lock_x_unlock(hash_lock);
		mutex_exit(&block->mutex);

		/* free new_block */
		mutex_enter(&new_block->mutex);
		buf_LRU_block_free_non_file_page(new_block);
		mutex_exit(&new_block->mutex);
	}

	return(true); /* free_list was enough */
}

/** Sets the global variable that feeds MySQL's innodb_buffer_pool_resize_status
to the specified string. The format and the following parameters are the
same as the ones used for printf(3).
@param[in]	fmt	format
@param[in]	...	extra parameters according to fmt */
static
void
buf_resize_status(
	const char*	fmt,
	...)
{
	va_list	ap;

	va_start(ap, fmt);

	vsnprintf(
		export_vars.innodb_buffer_pool_resize_status,
		sizeof(export_vars.innodb_buffer_pool_resize_status),
		fmt, ap);

	va_end(ap);

	ib::info() << export_vars.innodb_buffer_pool_resize_status;
}

/** Determines if a block is intended to be withdrawn.
@param[in]	buf_pool	buffer pool instance
@param[in]	block		pointer to control block
@retval true	if will be withdrawn */
bool
buf_block_will_withdrawn(
	buf_pool_t*		buf_pool,
	const buf_block_t*	block)
{
	ut_ad(buf_pool->curr_size < buf_pool->old_size);
	ut_ad(!buf_pool_resizing || buf_pool_mutex_own(buf_pool));

	const buf_chunk_t*	chunk
		= buf_pool->chunks + buf_pool->n_chunks_new;
	const buf_chunk_t*	echunk
		= buf_pool->chunks + buf_pool->n_chunks;

	while (chunk < echunk) {
		if (block >= chunk->blocks
		    && block < chunk->blocks + chunk->size) {
			return(true);
		}
		++chunk;
	}

	return(false);
}

/** Determines if a frame is intended to be withdrawn.
@param[in]	buf_pool	buffer pool instance
@param[in]	ptr		pointer to a frame
@retval true	if will be withdrawn */
bool
buf_frame_will_withdrawn(
	buf_pool_t*	buf_pool,
	const byte*	ptr)
{
	ut_ad(buf_pool->curr_size < buf_pool->old_size);
	ut_ad(!buf_pool_resizing || buf_pool_mutex_own(buf_pool));

	const buf_chunk_t*	chunk
		= buf_pool->chunks + buf_pool->n_chunks_new;
	const buf_chunk_t*	echunk
		= buf_pool->chunks + buf_pool->n_chunks;

	while (chunk < echunk) {
		if (ptr >= chunk->blocks->frame
		    && ptr < (chunk->blocks + chunk->size - 1)->frame
			     + UNIV_PAGE_SIZE) {
			return(true);
		}
		++chunk;
	}

	return(false);
}

/** Withdraw the buffer pool blocks from end of the buffer pool instance
until withdrawn by buf_pool->withdraw_target.
@param[in]	buf_pool	buffer pool instance
@retval true	if retry is needed */
static
bool
buf_pool_withdraw_blocks(
	buf_pool_t*	buf_pool)
{
	buf_block_t*	block;
	ulint		loop_count = 0;
	ulint		i = buf_pool_index(buf_pool);

	ib::info() << "buffer pool " << i
		<< " : start to withdraw the last "
		<< buf_pool->withdraw_target << " blocks.";

	/* Minimize buf_pool->zip_free[i] lists */
	buf_pool_mutex_enter(buf_pool);
	buf_buddy_condense_free(buf_pool);
	buf_pool_mutex_exit(buf_pool);

	while (UT_LIST_GET_LEN(buf_pool->withdraw)
	       < buf_pool->withdraw_target) {

		/* try to withdraw from free_list */
		ulint	count1 = 0;

		buf_pool_mutex_enter(buf_pool);
		block = reinterpret_cast<buf_block_t*>(
			UT_LIST_GET_FIRST(buf_pool->free));
		while (block != NULL
		       && UT_LIST_GET_LEN(buf_pool->withdraw)
			  < buf_pool->withdraw_target) {
			ut_ad(block->page.in_free_list);
			ut_ad(!block->page.in_flush_list);
			ut_ad(!block->page.in_LRU_list);
			ut_a(!buf_page_in_file(&block->page));

			buf_block_t*	next_block;
			next_block = reinterpret_cast<buf_block_t*>(
				UT_LIST_GET_NEXT(
					list, &block->page));

			if (buf_block_will_withdrawn(buf_pool, block)) {
				/* This should be withdrawn */
				UT_LIST_REMOVE(
					buf_pool->free,
					&block->page);
				UT_LIST_ADD_LAST(
					buf_pool->withdraw,
					&block->page);
				ut_d(block->in_withdraw_list = TRUE);
				count1++;
			}

			block = next_block;
		}
		buf_pool_mutex_exit(buf_pool);

		/* reserve free_list length */
		if (UT_LIST_GET_LEN(buf_pool->withdraw)
		    < buf_pool->withdraw_target) {
			ulint	scan_depth;
			flush_counters_t n;

			/* cap scan_depth with current LRU size. */
			buf_pool_mutex_enter(buf_pool);
			scan_depth = UT_LIST_GET_LEN(buf_pool->LRU);
			buf_pool_mutex_exit(buf_pool);

			scan_depth = ut_min(
				ut_max(buf_pool->withdraw_target
				       - UT_LIST_GET_LEN(buf_pool->withdraw),
				       static_cast<ulint>(srv_LRU_scan_depth)),
				scan_depth);

			buf_flush_do_batch(buf_pool, BUF_FLUSH_LRU,
				scan_depth, 0, &n);
			buf_flush_wait_batch_end(buf_pool, BUF_FLUSH_LRU);

			if (n.flushed) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_LRU_BATCH_FLUSH_TOTAL_PAGE,
					MONITOR_LRU_BATCH_FLUSH_COUNT,
					MONITOR_LRU_BATCH_FLUSH_PAGES,
					n.flushed);
			}
		}

		/* relocate blocks/buddies in withdrawn area */
		ulint	count2 = 0;

		buf_pool_mutex_enter(buf_pool);
		buf_page_t*	bpage;
		bpage = UT_LIST_GET_FIRST(buf_pool->LRU);
		while (bpage != NULL) {
			BPageMutex*	block_mutex;
			buf_page_t*	next_bpage;

			block_mutex = buf_page_get_mutex(bpage);
			mutex_enter(block_mutex);

			next_bpage = UT_LIST_GET_NEXT(LRU, bpage);

			if (bpage->zip.data != NULL
			    && buf_frame_will_withdrawn(
				buf_pool,
				static_cast<byte*>(bpage->zip.data))) {

				if (buf_page_can_relocate(bpage)) {
					mutex_exit(block_mutex);
					buf_pool_mutex_exit_forbid(buf_pool);
					if(!buf_buddy_realloc(
						buf_pool, bpage->zip.data,
						page_zip_get_size(
							&bpage->zip))) {

						/* failed to allocate block */
						buf_pool_mutex_exit_allow(
							buf_pool);
						break;
					}
					buf_pool_mutex_exit_allow(buf_pool);
					mutex_enter(block_mutex);
					count2++;
				}
				/* NOTE: if the page is in use,
				not reallocated yet */
			}

			if (buf_page_get_state(bpage)
			    == BUF_BLOCK_FILE_PAGE
			    && buf_block_will_withdrawn(
				buf_pool,
				reinterpret_cast<buf_block_t*>(bpage))) {

				if (buf_page_can_relocate(bpage)) {
					mutex_exit(block_mutex);
					buf_pool_mutex_exit_forbid(buf_pool);
					if(!buf_page_realloc(
						buf_pool,
						reinterpret_cast<buf_block_t*>(
							bpage))) {
						/* failed to allocate block */
						buf_pool_mutex_exit_allow(
							buf_pool);
						break;
					}
					buf_pool_mutex_exit_allow(buf_pool);
					count2++;
				} else {
					mutex_exit(block_mutex);
				}
				/* NOTE: if the page is in use,
				not reallocated yet */
			} else {
				mutex_exit(block_mutex);
			}

			bpage = next_bpage;
		}
		buf_pool_mutex_exit(buf_pool);

		buf_resize_status(
			"buffer pool %lu : withdrawing blocks. (%lu/%lu)",
			i, UT_LIST_GET_LEN(buf_pool->withdraw),
			buf_pool->withdraw_target);

		ib::info() << "buffer pool " << i << " : withdrew "
			<< count1 << " blocks from free list."
			<< " Tried to relocate " << count2 << " pages ("
			<< UT_LIST_GET_LEN(buf_pool->withdraw) << "/"
			<< buf_pool->withdraw_target << ").";

		if (++loop_count >= 10) {
			/* give up for now.
			retried after user threads paused. */

			ib::info() << "buffer pool " << i
				<< " : will retry to withdraw later.";

			/* need retry later */
			return(true);
		}
	}

	/* confirm withdrawn enough */
	const buf_chunk_t*	chunk
		= buf_pool->chunks + buf_pool->n_chunks_new;
	const buf_chunk_t*	echunk
		= buf_pool->chunks + buf_pool->n_chunks;

	while (chunk < echunk) {
		block = chunk->blocks;
		for (ulint j = chunk->size; j--; block++) {
			/* If !=BUF_BLOCK_NOT_USED block in the
			withdrawn area, it means corruption
			something */
			ut_a(buf_block_get_state(block)
				== BUF_BLOCK_NOT_USED);
			ut_ad(block->in_withdraw_list);
		}
		++chunk;
	}

	ib::info() << "buffer pool " << i << " : withdrawn target "
		<< UT_LIST_GET_LEN(buf_pool->withdraw) << " blocks.";

	return(false);
}

/** resize page_hash and zip_hash for a buffer pool instance.
@param[in]	buf_pool	buffer pool instance */
static
void
buf_pool_resize_hash(
	buf_pool_t*	buf_pool)
{
	hash_table_t*	new_hash_table;

	/* recreate page_hash */
	new_hash_table = ib_recreate(
		buf_pool->page_hash, 2 * buf_pool->curr_size);

	for (ulint i = 0; i < hash_get_n_cells(buf_pool->page_hash); i++) {
		buf_page_t*	bpage;

		bpage = static_cast<buf_page_t*>(
			HASH_GET_FIRST(
				buf_pool->page_hash, i));

		while (bpage) {
			buf_page_t*	prev_bpage = bpage;
			ulint		fold;

			bpage = static_cast<buf_page_t*>(
				HASH_GET_NEXT(
					hash, prev_bpage));

			fold = prev_bpage->id.fold();

			HASH_DELETE(buf_page_t, hash,
				buf_pool->page_hash, fold,
				prev_bpage);

			HASH_INSERT(buf_page_t, hash,
				new_hash_table, fold,
				prev_bpage);
		}
	}

	/* Concurrent threads may be accessing
	buf_pool->page_hash->n_cells, n_sync_obj and try to latch
	sync_obj[i] while we are resizing. Therefore we never
	deallocate page_hash, instead we overwrite n_cells (and other
	fields) with the new values. The n_sync_obj and sync_obj are
	actually same in both. */
	std::swap(*buf_pool->page_hash, *new_hash_table);
	hash_table_free(new_hash_table);

	/* recreate zip_hash */
	new_hash_table = hash_create(2 * buf_pool->curr_size);

	for (ulint i = 0; i < hash_get_n_cells(buf_pool->zip_hash); i++) {
		buf_page_t*	bpage;

		bpage = static_cast<buf_page_t*>(
			HASH_GET_FIRST(buf_pool->zip_hash, i));

		while (bpage) {
			buf_page_t*	prev_bpage = bpage;
			ulint		fold;

			bpage = static_cast<buf_page_t*>(
				HASH_GET_NEXT(
					hash, prev_bpage));

			fold = BUF_POOL_ZIP_FOLD(
				reinterpret_cast<buf_block_t*>(
					prev_bpage));

			HASH_DELETE(buf_page_t, hash,
				buf_pool->zip_hash, fold,
				prev_bpage);

			HASH_INSERT(buf_page_t, hash,
				new_hash_table, fold,
				prev_bpage);
		}
	}

	hash_table_free(buf_pool->zip_hash);
	buf_pool->zip_hash = new_hash_table;
}

/** Resize the buffer pool based on srv_buf_pool_size from
srv_buf_pool_old_size. */
static
void
buf_pool_resize()
{
	buf_pool_t*	buf_pool;
	ulint		new_instance_size;
	bool		warning = false;

	NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;

	ut_ad(!buf_pool_resizing);
	ut_ad(srv_buf_pool_chunk_unit > 0);

	new_instance_size = srv_buf_pool_size / srv_buf_pool_instances;
	new_instance_size /= UNIV_PAGE_SIZE;

	buf_resize_status("Resizing buffer pool from " ULINTPF " to "
			  ULINTPF " (unit=" ULINTPF ").",
			  srv_buf_pool_old_size, srv_buf_pool_size,
			  srv_buf_pool_chunk_unit);

	/* set new limit for all buffer pool for resizing */
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool = buf_pool_from_array(i);
		buf_pool_mutex_enter(buf_pool);

		ut_ad(buf_pool->curr_size == buf_pool->old_size);
		ut_ad(buf_pool->n_chunks_new == buf_pool->n_chunks);
		ut_ad(UT_LIST_GET_LEN(buf_pool->withdraw) == 0);
		ut_ad(buf_pool->flush_rbt == NULL);

		buf_pool->curr_size = new_instance_size;

		buf_pool->n_chunks_new = new_instance_size * UNIV_PAGE_SIZE
			/ srv_buf_pool_chunk_unit;

		buf_pool_mutex_exit(buf_pool);
	}
#ifdef BTR_CUR_HASH_ADAPT
	/* disable AHI if needed */
	bool	btr_search_disabled = false;

	buf_resize_status("Disabling adaptive hash index.");

	btr_search_s_lock_all();
	if (btr_search_enabled) {
		btr_search_s_unlock_all();
		btr_search_disabled = true;
	} else {
		btr_search_s_unlock_all();
	}

	btr_search_disable();

	if (btr_search_disabled) {
		ib::info() << "disabled adaptive hash index.";
	}
#endif /* BTR_CUR_HASH_ADAPT */

	/* set withdraw target */
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool = buf_pool_from_array(i);
		if (buf_pool->curr_size < buf_pool->old_size) {
			ulint	withdraw_target = 0;

			const buf_chunk_t*	chunk
				= buf_pool->chunks + buf_pool->n_chunks_new;
			const buf_chunk_t*	echunk
				= buf_pool->chunks + buf_pool->n_chunks;

			while (chunk < echunk) {
				withdraw_target += chunk->size;
				++chunk;
			}

			ut_ad(buf_pool->withdraw_target == 0);
			buf_pool->withdraw_target = withdraw_target;
		}
	}

	buf_resize_status("Withdrawing blocks to be shrunken.");

	time_t		withdraw_started = time(NULL);
	ulint		message_interval = 60;
	ulint		retry_interval = 1;

withdraw_retry:
	bool	should_retry_withdraw = false;

	/* wait for the number of blocks fit to the new size (if needed)*/
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool = buf_pool_from_array(i);
		if (buf_pool->curr_size < buf_pool->old_size) {

			should_retry_withdraw |=
				buf_pool_withdraw_blocks(buf_pool);
		}
	}

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		/* abort to resize for shutdown. */
		return;
	}

	/* abort buffer pool load */
	buf_load_abort();

	const time_t current_time = time(NULL);

	if (should_retry_withdraw
	    && difftime(current_time, withdraw_started) >= message_interval) {

		if (message_interval > 900) {
			message_interval = 1800;
		} else {
			message_interval *= 2;
		}

		lock_mutex_enter();
		trx_sys_mutex_enter();
		bool	found = false;
		for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys->mysql_trx_list);
		     trx != NULL;
		     trx = UT_LIST_GET_NEXT(mysql_trx_list, trx)) {
			if (trx->state != TRX_STATE_NOT_STARTED
			    && trx->mysql_thd != NULL
			    && withdraw_started > trx->start_time) {
				if (!found) {
					ib::warn() <<
						"The following trx might hold"
						" the blocks in buffer pool to"
					        " be withdrawn. Buffer pool"
						" resizing can complete only"
						" after all the transactions"
						" below release the blocks.";
					found = true;
				}

				lock_trx_print_wait_and_mvcc_state(
					stderr, trx, current_time);
			}
		}
		trx_sys_mutex_exit();
		lock_mutex_exit();

		withdraw_started = current_time;
	}

	if (should_retry_withdraw) {
		ib::info() << "Will retry to withdraw " << retry_interval
			<< " seconds later.";
		os_thread_sleep(retry_interval * 1000000);

		if (retry_interval > 5) {
			retry_interval = 10;
		} else {
			retry_interval *= 2;
		}

		goto withdraw_retry;
	}


	buf_resize_status("Latching whole of buffer pool.");

#ifndef DBUG_OFF
	{
		bool	should_wait = true;

		while (should_wait) {
			should_wait = false;
			DBUG_EXECUTE_IF(
				"ib_buf_pool_resize_wait_before_resize",
				should_wait = true; os_thread_sleep(10000););
		}
	}
#endif /* !DBUG_OFF */

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* Indicate critical path */
	buf_pool_resizing = true;

	/* Acquire all buf_pool_mutex/hash_lock */
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);

		buf_pool_mutex_enter(buf_pool);
	}
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);

		hash_lock_x_all(buf_pool->page_hash);
	}

	buf_chunk_map_reg = UT_NEW_NOKEY(buf_pool_chunk_map_t());

	/* add/delete chunks */
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);
		buf_chunk_t*	chunk;
		buf_chunk_t*	echunk;

		buf_resize_status("buffer pool %lu :"
			" resizing with chunks %lu to %lu.",
			i, buf_pool->n_chunks, buf_pool->n_chunks_new);

		if (buf_pool->n_chunks_new < buf_pool->n_chunks) {
			/* delete chunks */
			chunk = buf_pool->chunks
				+ buf_pool->n_chunks_new;
			echunk = buf_pool->chunks + buf_pool->n_chunks;

			ulint	sum_freed = 0;

			while (chunk < echunk) {
				buf_block_t*	block = chunk->blocks;

				for (ulint j = chunk->size;
				     j--; block++) {
					mutex_free(&block->mutex);
					rw_lock_free(&block->lock);

					ut_d(rw_lock_free(
						&block->debug_latch));
				}

				buf_pool->allocator.deallocate_large(
					chunk->mem, &chunk->mem_pfx);

				sum_freed += chunk->size;

				++chunk;
			}

			/* discard withdraw list */
			UT_LIST_INIT(buf_pool->withdraw,
				     &buf_page_t::list);
			buf_pool->withdraw_target = 0;

			ib::info() << "buffer pool " << i << " : "
				<< buf_pool->n_chunks - buf_pool->n_chunks_new
				<< " chunks (" << sum_freed
				<< " blocks) were freed.";

			buf_pool->n_chunks = buf_pool->n_chunks_new;
		}

		{
			/* reallocate buf_pool->chunks */
			const ulint	new_chunks_size
				= buf_pool->n_chunks_new * sizeof(*chunk);

			buf_chunk_t*	new_chunks
				= reinterpret_cast<buf_chunk_t*>(
					ut_zalloc_nokey_nofatal(new_chunks_size));

			DBUG_EXECUTE_IF("buf_pool_resize_chunk_null",
					ut_free(new_chunks);
					new_chunks = NULL;);

			if (new_chunks == NULL) {
				ib::error() << "buffer pool " << i
					<< " : failed to allocate"
					" the chunk array.";
				buf_pool->n_chunks_new
					= buf_pool->n_chunks;
				warning = true;
				buf_pool->chunks_old = NULL;
				for (ulint j = 0; j < buf_pool->n_chunks_new; j++) {
					buf_pool_register_chunk(&(buf_pool->chunks[j]));
				}
				goto calc_buf_pool_size;
			}

			ulint	n_chunks_copy = ut_min(buf_pool->n_chunks_new,
						       buf_pool->n_chunks);

			memcpy(new_chunks, buf_pool->chunks,
			       n_chunks_copy * sizeof(*chunk));

			for (ulint j = 0; j < n_chunks_copy; j++) {
				buf_pool_register_chunk(&new_chunks[j]);
			}

			buf_pool->chunks_old = buf_pool->chunks;
			buf_pool->chunks = new_chunks;
		}


		if (buf_pool->n_chunks_new > buf_pool->n_chunks) {
			/* add chunks */
			chunk = buf_pool->chunks + buf_pool->n_chunks;
			echunk = buf_pool->chunks
				+ buf_pool->n_chunks_new;

			ulint	sum_added = 0;
			ulint	n_chunks = buf_pool->n_chunks;

			while (chunk < echunk) {
				ulong	unit = srv_buf_pool_chunk_unit;

				if (!buf_chunk_init(buf_pool, chunk, unit)) {

					ib::error() << "buffer pool " << i
						<< " : failed to allocate"
						" new memory.";

					warning = true;

					buf_pool->n_chunks_new
						= n_chunks;

					break;
				}

				sum_added += chunk->size;

				++n_chunks;
				++chunk;
			}

			ib::info() << "buffer pool " << i << " : "
				<< buf_pool->n_chunks_new - buf_pool->n_chunks
				<< " chunks (" << sum_added
				<< " blocks) were added.";

			buf_pool->n_chunks = n_chunks;
		}
calc_buf_pool_size:

		/* recalc buf_pool->curr_size */
		ulint	new_size = 0;

		chunk = buf_pool->chunks;
		do {
			new_size += chunk->size;
		} while (++chunk < buf_pool->chunks
				   + buf_pool->n_chunks);

		buf_pool->curr_size = new_size;
		buf_pool->n_chunks_new = buf_pool->n_chunks;

		if (buf_pool->chunks_old) {
			ut_free(buf_pool->chunks_old);
			buf_pool->chunks_old = NULL;
		}
	}

	buf_pool_chunk_map_t*	chunk_map_old = buf_chunk_map_ref;
	buf_chunk_map_ref = buf_chunk_map_reg;

	/* set instance sizes */
	{
		ulint	curr_size = 0;

		for (ulint i = 0; i < srv_buf_pool_instances; i++) {
			buf_pool = buf_pool_from_array(i);

			ut_ad(UT_LIST_GET_LEN(buf_pool->withdraw) == 0);

			buf_pool->read_ahead_area =
				ut_min(BUF_READ_AHEAD_PAGES,
				       ut_2_power_up(buf_pool->curr_size /
						      BUF_READ_AHEAD_PORTION));
			buf_pool->curr_pool_size
				= buf_pool->curr_size * UNIV_PAGE_SIZE;
			curr_size += buf_pool->curr_pool_size;
			buf_pool->old_size = buf_pool->curr_size;
		}
		srv_buf_pool_curr_size = curr_size;
		innodb_set_buf_pool_size(buf_pool_size_align(curr_size));
	}

	const bool	new_size_too_diff
		= srv_buf_pool_base_size > srv_buf_pool_size * 2
			|| srv_buf_pool_base_size * 2 < srv_buf_pool_size;

	/* Normalize page_hash and zip_hash,
	if the new size is too different */
	if (!warning && new_size_too_diff) {

		buf_resize_status("Resizing hash tables.");

		for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
			buf_pool_t*	buf_pool = buf_pool_from_array(i);

			buf_pool_resize_hash(buf_pool);

			ib::info() << "buffer pool " << i
				<< " : hash tables were resized.";
		}
	}

	/* Release all buf_pool_mutex/page_hash */
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);

		hash_unlock_x_all(buf_pool->page_hash);
		buf_pool_mutex_exit(buf_pool);
	}

	UT_DELETE(chunk_map_old);

	buf_pool_resizing = false;

	/* Normalize other components, if the new size is too different */
	if (!warning && new_size_too_diff) {
		srv_buf_pool_base_size = srv_buf_pool_size;

		buf_resize_status("Resizing also other hash tables.");

		/* normalize lock_sys */
		srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);
		lock_sys_resize(srv_lock_table_size);

		/* normalize dict_sys */
		dict_resize();

		ib::info() << "Resized hash tables at lock_sys,"
#ifdef BTR_CUR_HASH_ADAPT
			" adaptive hash index,"
#endif /* BTR_CUR_HASH_ADAPT */
			" dictionary.";
	}

	/* normalize ibuf->max_size */
	ibuf_max_size_update(srv_change_buffer_max_size);

	if (srv_buf_pool_old_size != srv_buf_pool_size) {

		ib::info() << "Completed to resize buffer pool from "
			<< srv_buf_pool_old_size
			<< " to " << srv_buf_pool_size << ".";
		srv_buf_pool_old_size = srv_buf_pool_size;
	}

#ifdef BTR_CUR_HASH_ADAPT
	/* enable AHI if needed */
	if (btr_search_disabled) {
		btr_search_enable(true);
		ib::info() << "Re-enabled adaptive hash index.";
	}
#endif /* BTR_CUR_HASH_ADAPT */

	char	now[32];

	ut_sprintf_timestamp(now);
	if (!warning) {
		buf_resize_status("Completed resizing buffer pool at %s.",
			now);
	} else {
		buf_resize_status("Resizing buffer pool failed,"
			" finished resizing at %s.", now);
	}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	return;
}

/** This is the thread for resizing buffer pool. It waits for an event and
when waked up either performs a resizing and sleeps again.
@return	this function does not return, calls os_thread_exit()
*/
extern "C"
os_thread_ret_t
DECLARE_THREAD(buf_resize_thread)(void*)
{
	my_thread_init();

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		os_event_wait(srv_buf_resize_event);
		os_event_reset(srv_buf_resize_event);

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}

		buf_pool_mutex_enter_all();
		if (srv_buf_pool_old_size == srv_buf_pool_size) {
			buf_pool_mutex_exit_all();
			std::ostringstream sout;
			sout << "Size did not change (old size = new size = "
				<< srv_buf_pool_size << ". Nothing to do.";
			buf_resize_status(sout.str().c_str());

			/* nothing to do */
			continue;
		}
		buf_pool_mutex_exit_all();

		buf_pool_resize();
	}

	srv_buf_resize_thread_active = false;

	my_thread_end();
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/********************************************************************//**
Relocate a buffer control block.  Relocates the block on the LRU list
and in buf_pool->page_hash.  Does not relocate bpage->list.
The caller must take care of relocating bpage->list. */
static
void
buf_relocate(
/*=========*/
	buf_page_t*	bpage,	/*!< in/out: control block being relocated;
				buf_page_get_state(bpage) must be
				BUF_BLOCK_ZIP_DIRTY or BUF_BLOCK_ZIP_PAGE */
	buf_page_t*	dpage)	/*!< in/out: destination control block */
{
	buf_page_t*	b;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_pool_mutex_own(buf_pool));
	ut_ad(buf_page_hash_lock_held_x(buf_pool, bpage));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage == buf_page_hash_get_low(buf_pool, bpage->id));

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

	new (dpage) buf_page_t(*bpage);

	/* Important that we adjust the hazard pointer before
	removing bpage from LRU list. */
	buf_LRU_adjust_hp(buf_pool, bpage);

	ut_d(bpage->in_LRU_list = FALSE);
	ut_d(bpage->in_page_hash = FALSE);

	/* relocate buf_pool->LRU */
	b = UT_LIST_GET_PREV(LRU, bpage);
	UT_LIST_REMOVE(buf_pool->LRU, bpage);

	if (b != NULL) {
		UT_LIST_INSERT_AFTER(buf_pool->LRU, b, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool->LRU, dpage);
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

        ut_d(CheckInLRUList::validate(buf_pool));

	/* relocate buf_pool->page_hash */
	ulint	fold = bpage->id.fold();
	ut_ad(fold == dpage->id.fold());
	HASH_REPLACE(buf_page_t, hash, buf_pool->page_hash, fold, bpage,
		     dpage);
}

/** Hazard Pointer implementation. */

/** Set current value
@param bpage	buffer block to be set as hp */
void
HazardPointer::set(buf_page_t* bpage)
{
	ut_ad(mutex_own(m_mutex));
	ut_ad(!bpage || buf_pool_from_bpage(bpage) == m_buf_pool);
	ut_ad(!bpage || buf_page_in_file(bpage));

	m_hp = bpage;
}

/** Checks if a bpage is the hp
@param bpage    buffer block to be compared
@return true if it is hp */

bool
HazardPointer::is_hp(const buf_page_t* bpage)
{
	ut_ad(mutex_own(m_mutex));
	ut_ad(!m_hp || buf_pool_from_bpage(m_hp) == m_buf_pool);
	ut_ad(!bpage || buf_pool_from_bpage(bpage) == m_buf_pool);

	return(bpage == m_hp);
}

/** Adjust the value of hp. This happens when some other thread working
on the same list attempts to remove the hp from the list.
@param bpage	buffer block to be compared */

void
FlushHp::adjust(const buf_page_t* bpage)
{
	ut_ad(bpage != NULL);

	/** We only support reverse traversal for now. */
	if (is_hp(bpage)) {
		m_hp = UT_LIST_GET_PREV(list, m_hp);
	}

	ut_ad(!m_hp || m_hp->in_flush_list);
}

/** Adjust the value of hp. This happens when some other thread working
on the same list attempts to remove the hp from the list.
@param bpage	buffer block to be compared */

void
LRUHp::adjust(const buf_page_t* bpage)
{
	ut_ad(bpage);

	/** We only support reverse traversal for now. */
	if (is_hp(bpage)) {
		m_hp = UT_LIST_GET_PREV(LRU, m_hp);
	}

	ut_ad(!m_hp || m_hp->in_LRU_list);
}

/** Selects from where to start a scan. If we have scanned too deep into
the LRU list it resets the value to the tail of the LRU list.
@return buf_page_t from where to start scan. */

buf_page_t*
LRUItr::start()
{
	ut_ad(mutex_own(m_mutex));

	if (!m_hp || m_hp->old) {
		m_hp = UT_LIST_GET_LAST(m_buf_pool->LRU);
	}

	return(m_hp);
}

/** Determine if a block is a sentinel for a buffer pool watch.
@param[in]	buf_pool	buffer pool instance
@param[in]	bpage		block
@return TRUE if a sentinel for a buffer pool watch, FALSE if not */
ibool
buf_pool_watch_is_sentinel(
	const buf_pool_t*	buf_pool,
	const buf_page_t*	bpage)
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
	return(TRUE);
}

/** Add watch for the given page to be read in. Caller must have
appropriate hash_lock for the bpage. This function may release the
hash_lock and reacquire it.
@param[in]	page_id		page id
@param[in,out]	hash_lock	hash_lock currently latched
@return NULL if watch set, block if the page is in the buffer pool */
static
buf_page_t*
buf_pool_watch_set(
	const page_id_t		page_id,
	rw_lock_t**		hash_lock)
{
	buf_page_t*	bpage;
	ulint		i;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	ut_ad(*hash_lock == buf_page_hash_lock_get(buf_pool, page_id));

	ut_ad(rw_lock_own(*hash_lock, RW_LOCK_X));

	bpage = buf_page_hash_get_low(buf_pool, page_id);

	if (bpage != NULL) {
page_found:
		if (!buf_pool_watch_is_sentinel(buf_pool, bpage)) {
			/* The page was loaded meanwhile. */
			return(bpage);
		}

		/* Add to an existing watch. */
		buf_block_fix(bpage);
		return(NULL);
	}

	/* From this point this function becomes fairly heavy in terms
	of latching. We acquire the buf_pool mutex as well as all the
	hash_locks. buf_pool mutex is needed because any changes to
	the page_hash must be covered by it and hash_locks are needed
	because we don't want to read any stale information in
	buf_pool->watch[]. However, it is not in the critical code path
	as this function will be called only by the purge thread. */

	/* To obey latching order first release the hash_lock. */
	rw_lock_x_unlock(*hash_lock);

	buf_pool_mutex_enter(buf_pool);
	hash_lock_x_all(buf_pool->page_hash);

	/* If not own buf_pool_mutex, page_hash can be changed. */
	*hash_lock = buf_page_hash_lock_get(buf_pool, page_id);

	/* We have to recheck that the page
	was not loaded or a watch set by some other
	purge thread. This is because of the small
	time window between when we release the
	hash_lock to acquire buf_pool mutex above. */

	bpage = buf_page_hash_get_low(buf_pool, page_id);
	if (UNIV_LIKELY_NULL(bpage)) {
		buf_pool_mutex_exit(buf_pool);
		hash_unlock_x_all_but(buf_pool->page_hash, *hash_lock);
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

			/* bpage is pointing to buf_pool->watch[],
			which is protected by buf_pool->mutex.
			Normally, buf_page_t objects are protected by
			buf_block_t::mutex or buf_pool->zip_mutex or both. */

			bpage->state = BUF_BLOCK_ZIP_PAGE;
			bpage->id = page_id;
			bpage->buf_fix_count = 1;

			ut_d(bpage->in_page_hash = TRUE);
			HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
				    page_id.fold(), bpage);

			buf_pool_mutex_exit(buf_pool);
			/* Once the sentinel is in the page_hash we can
			safely release all locks except just the
			relevant hash_lock */
			hash_unlock_x_all_but(buf_pool->page_hash,
						*hash_lock);

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

/** Remove the sentinel block for the watch before replacing it with a
real block. buf_page_watch_clear() or buf_page_watch_occurred() will notice
that the block has been replaced with the real block.
@param[in,out]	buf_pool	buffer pool instance
@param[in,out]	watch		sentinel for watch
@return reference count, to be added to the replacement block */
static
void
buf_pool_watch_remove(
	buf_pool_t*	buf_pool,
	buf_page_t*	watch)
{
#ifdef UNIV_DEBUG
	/* We must also own the appropriate hash_bucket mutex. */
	rw_lock_t* hash_lock = buf_page_hash_lock_get(buf_pool, watch->id);
	ut_ad(rw_lock_own(hash_lock, RW_LOCK_X));
#endif /* UNIV_DEBUG */

	ut_ad(buf_pool_mutex_own(buf_pool));

	HASH_DELETE(buf_page_t, hash, buf_pool->page_hash, watch->id.fold(),
		    watch);
	ut_d(watch->in_page_hash = FALSE);
	watch->buf_fix_count = 0;
	watch->state = BUF_BLOCK_POOL_WATCH;
}

/** Stop watching if the page has been read in.
buf_pool_watch_set(same_page_id) must have returned NULL before.
@param[in]	page_id	page id */
void buf_pool_watch_unset(const page_id_t page_id)
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	/* We only need to have buf_pool mutex in case where we end
	up calling buf_pool_watch_remove but to obey latching order
	we acquire it here before acquiring hash_lock. This should
	not cause too much grief as this function is only ever
	called from the purge thread. */
	buf_pool_mutex_enter(buf_pool);

	rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool, page_id);
	rw_lock_x_lock(hash_lock);

	/* The page must exist because buf_pool_watch_set()
	increments buf_fix_count. */
	bpage = buf_page_hash_get_low(buf_pool, page_id);

	if (buf_block_unfix(bpage) == 0
	    && buf_pool_watch_is_sentinel(buf_pool, bpage)) {
		buf_pool_watch_remove(buf_pool, bpage);
	}

	buf_pool_mutex_exit(buf_pool);
	rw_lock_x_unlock(hash_lock);
}

/** Check if the page has been read in.
This may only be called after buf_pool_watch_set(same_page_id)
has returned NULL and before invoking buf_pool_watch_unset(same_page_id).
@param[in]	page_id	page id
@return false if the given page was not read in, true if it was */
bool buf_pool_watch_occurred(const page_id_t page_id)
{
	bool		ret;
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);
	rw_lock_t*	hash_lock = buf_page_hash_lock_get(buf_pool, page_id);

	rw_lock_s_lock(hash_lock);

	/* If not own buf_pool_mutex, page_hash can be changed. */
	hash_lock = buf_page_hash_lock_s_confirm(hash_lock, buf_pool, page_id);

	/* The page must exist because buf_pool_watch_set()
	increments buf_fix_count. */
	bpage = buf_page_hash_get_low(buf_pool, page_id);

	ret = !buf_pool_watch_is_sentinel(buf_pool, bpage);
	rw_lock_s_unlock(hash_lock);

	return(ret);
}

/********************************************************************//**
Moves a page to the start of the buffer pool LRU list. This high-level
function can be used to prevent an important page from slipping out of
the buffer pool. */
void
buf_page_make_young(
/*================*/
	buf_page_t*	bpage)	/*!< in: buffer block of a file page */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	buf_pool_mutex_enter(buf_pool);

	ut_a(buf_page_in_file(bpage));

	buf_LRU_make_block_young(bpage);

	buf_pool_mutex_exit(buf_pool);
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
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(!buf_pool_mutex_own(buf_pool));
#endif /* UNIV_DEBUG */
	ut_a(buf_page_in_file(bpage));

	if (buf_page_peek_if_too_old(bpage)) {
		buf_page_make_young(bpage);
	}
}

#ifdef UNIV_DEBUG

/** Sets file_page_was_freed TRUE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated.
@param[in]	page_id	page id
@return control block if found in page hash table, otherwise NULL */
buf_page_t* buf_page_set_file_page_was_freed(const page_id_t page_id)
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);
	rw_lock_t*	hash_lock;

	bpage = buf_page_hash_get_s_locked(buf_pool, page_id, &hash_lock);

	if (bpage) {
		BPageMutex*	block_mutex = buf_page_get_mutex(bpage);
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

/** Sets file_page_was_freed FALSE if the page is found in the buffer pool.
This function should be called when we free a file page and want the
debug version to check that it is not accessed any more unless
reallocated.
@param[in]	page_id	page id
@return control block if found in page hash table, otherwise NULL */
buf_page_t* buf_page_reset_file_page_was_freed(const page_id_t page_id)
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);
	rw_lock_t*	hash_lock;

	bpage = buf_page_hash_get_s_locked(buf_pool, page_id, &hash_lock);
	if (bpage) {
		BPageMutex*	block_mutex = buf_page_get_mutex(bpage);
		ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
		mutex_enter(block_mutex);
		rw_lock_s_unlock(hash_lock);
		bpage->file_page_was_freed = FALSE;
		mutex_exit(block_mutex);
	}

	return(bpage);
}
#endif /* UNIV_DEBUG */

/** Attempts to discard the uncompressed frame of a compressed page.
The caller should not be holding any mutexes when this function is called.
@param[in]	page_id	page id */
static void buf_block_try_discard_uncompressed(const page_id_t page_id)
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	/* Since we need to acquire buf_pool mutex to discard
	the uncompressed frame and because page_hash mutex resides
	below buf_pool mutex in sync ordering therefore we must
	first release the page_hash mutex. This means that the
	block in question can move out of page_hash. Therefore
	we need to check again if the block is still in page_hash. */
	buf_pool_mutex_enter(buf_pool);

	bpage = buf_page_hash_get(buf_pool, page_id);

	if (bpage) {
		buf_LRU_free_page(bpage, false);
	}

	buf_pool_mutex_exit(buf_pool);
}

/** Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with buf_page_release_zip().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches.
@param[in]	page_id		page id
@param[in]	page_size	page size
@return pointer to the block */
buf_page_t*
buf_page_get_zip(
	const page_id_t		page_id,
	const page_size_t&	page_size)
{
	buf_page_t*	bpage;
	BPageMutex*	block_mutex;
	rw_lock_t*	hash_lock;
	ibool		discard_attempted = FALSE;
	ibool		must_read;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	buf_pool->stat.n_page_gets++;

	for (;;) {
lookup:

		/* The following call will also grab the page_hash
		mutex if the page is found. */
		bpage = buf_page_hash_get_s_locked(buf_pool, page_id,
						   &hash_lock);
		if (bpage) {
			ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));
			break;
		}

		/* Page not in buf_pool: needs to be read from file */

		ut_ad(!hash_lock);
		dberr_t err = buf_read_page(page_id, page_size);

		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			ib::error() << "Reading compressed page " << page_id
				<< " failed with error: " << err;

			goto err_exit;
		}

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

	ut_ad(!buf_pool_watch_is_sentinel(buf_pool, bpage));

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		buf_block_fix(bpage);
		block_mutex = &buf_pool->zip_mutex;
		goto got_block;
	case BUF_BLOCK_FILE_PAGE:
		/* Discard the uncompressed page frame if possible. */
		if (!discard_attempted) {
			rw_lock_s_unlock(hash_lock);
			buf_block_try_discard_uncompressed(page_id);
			discard_attempted = TRUE;
			goto lookup;
		}

		buf_block_buf_fix_inc((buf_block_t*) bpage,
				      __FILE__, __LINE__);

		block_mutex = &((buf_block_t*) bpage)->mutex;
		goto got_block;
	default:
		break;
	}

	ut_error;
	goto err_exit;

got_block:
	mutex_enter(block_mutex);
	must_read = buf_page_get_io_fix(bpage) == BUF_IO_READ;

	rw_lock_s_unlock(hash_lock);

	ut_ad(!bpage->file_page_was_freed);

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
	}

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
#ifdef BTR_CUR_HASH_ADAPT
	/* No adaptive hash index entries may point to a previously
	unused (and now freshly allocated) block. */
	assert_block_ahi_empty_on_init(block);
	block->index		= NULL;

	block->n_hash_helps	= 0;
	block->n_fields		= 1;
	block->n_bytes		= 0;
	block->left_side	= TRUE;
#endif /* BTR_CUR_HASH_ADAPT */
}

/********************************************************************//**
Decompress a block.
@return TRUE if successful */
ibool
buf_zip_decompress(
/*===============*/
	buf_block_t*	block,	/*!< in/out: block */
	ibool		check)	/*!< in: TRUE=verify the page checksum */
{
	const byte*	frame = block->page.zip.data;
	ulint		size = page_zip_get_size(&block->page.zip);
	/* The tablespace will not be found if this function is called
	during IMPORT. */
	fil_space_t* space = fil_space_acquire_for_io(block->page.id.space());
	const unsigned key_version = mach_read_from_4(
		frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	fil_space_crypt_t* crypt_data = space ? space->crypt_data : NULL;
	const bool encrypted = crypt_data
		&& crypt_data->type != CRYPT_SCHEME_UNENCRYPTED
		&& (!crypt_data->is_default_encryption()
		    || srv_encrypt_tables);

	ut_ad(block->page.size.is_compressed());
	ut_a(block->page.id.space() != 0);

	if (UNIV_UNLIKELY(check && !page_zip_verify_checksum(frame, size))) {

		ib::error() << "Compressed page checksum mismatch for "
			<< (space ? space->chain.start->name : "")
			<< block->page.id << ": stored: "
			<< mach_read_from_4(frame + FIL_PAGE_SPACE_OR_CHKSUM)
			<< ", crc32: "
			<< page_zip_calc_checksum(
				frame, size, SRV_CHECKSUM_ALGORITHM_CRC32)
#ifdef INNODB_BUG_ENDIAN_CRC32
			<< "/"
			<< page_zip_calc_checksum(
				frame, size, SRV_CHECKSUM_ALGORITHM_CRC32,
				true)
#endif
			<< " innodb: "
			<< page_zip_calc_checksum(
				frame, size, SRV_CHECKSUM_ALGORITHM_INNODB)
			<< ", none: "
			<< page_zip_calc_checksum(
				frame, size, SRV_CHECKSUM_ALGORITHM_NONE)
			<< " (algorithm: " << srv_checksum_algorithm << ")";

		goto err_exit;
	}

	switch (fil_page_get_type(frame)) {
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		if (page_zip_decompress(&block->page.zip,
					block->frame, TRUE)) {
			if (space) {
				fil_space_release_for_io(space);
			}
			return(TRUE);
		}

		ib::error() << "Unable to decompress "
			<< (space ? space->chain.start->name : "")
			<< block->page.id;
		goto err_exit;
	case FIL_PAGE_TYPE_ALLOCATED:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* Copy to uncompressed storage. */
		memcpy(block->frame, frame, block->page.size.physical());
		if (space) {
			fil_space_release_for_io(space);
		}

		return(TRUE);
	}

	ib::error() << "Unknown compressed page type "
		<< fil_page_get_type(frame)
		<< " in " << (space ? space->chain.start->name : "")
		<< block->page.id;

err_exit:
	if (encrypted) {
		ib::info() << "Row compressed page could be encrypted"
			" with key_version " << key_version;
		dict_set_encrypted_by_space(block->page.id.space());
	} else {
		dict_set_corrupted_by_space(block->page.id.space());
	}

	if (space) {
		fil_space_release_for_io(space);
	}

	return(FALSE);
}

#ifdef BTR_CUR_HASH_ADAPT
/** Get a buffer block from an adaptive hash index pointer.
This function does not return if the block is not identified.
@param[in]	ptr	pointer to within a page frame
@return pointer to block, never NULL */
buf_block_t*
buf_block_from_ahi(const byte* ptr)
{
	buf_pool_chunk_map_t::iterator it;

	buf_pool_chunk_map_t*	chunk_map = buf_chunk_map_ref;
	ut_ad(buf_chunk_map_ref == buf_chunk_map_reg);
	ut_ad(!buf_pool_resizing);

	buf_chunk_t*	chunk;
	it = chunk_map->upper_bound(ptr);

	ut_a(it != chunk_map->begin());

	if (it == chunk_map->end()) {
		chunk = chunk_map->rbegin()->second;
	} else {
		chunk = (--it)->second;
	}

	ulint		offs = ptr - chunk->blocks->frame;

	offs >>= UNIV_PAGE_SIZE_SHIFT;

	ut_a(offs < chunk->size);

	buf_block_t*	block = &chunk->blocks[offs];

	/* The function buf_chunk_init() invokes buf_block_init() so that
	block[n].frame == block->frame + n * UNIV_PAGE_SIZE.  Check it. */
	ut_ad(block->frame == page_align(ptr));
	/* Read the state of the block without holding a mutex.
	A state transition from BUF_BLOCK_FILE_PAGE to
	BUF_BLOCK_REMOVE_HASH is possible during this execution. */
	ut_d(const buf_page_state state = buf_block_get_state(block));
	ut_ad(state == BUF_BLOCK_FILE_PAGE || state == BUF_BLOCK_REMOVE_HASH);
	return(block);
}
#endif /* BTR_CUR_HASH_ADAPT */

/********************************************************************//**
Find out if a pointer belongs to a buf_block_t. It can be a pointer to
the buf_block_t itself or a member of it
@return TRUE if ptr belongs to a buf_block_t struct */
ibool
buf_pointer_is_block_field(
/*=======================*/
	const void*	ptr)	/*!< in: pointer not dereferenced */
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		if (buf_pool_from_array(i)->is_block_field(ptr)) {
			return(TRUE);
		}
	}

	return(FALSE);
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

/** Wait for the block to be read in.
@param[in]	block	The block to check */
static
void
buf_wait_for_read(
	buf_block_t*	block)
{
	/* Note:

	We are using the block->lock to check for IO state (and a dirty read).
	We set the IO_READ state under the protection of the hash_lock
	(and block->mutex). This is safe because another thread can only
	access the block (and check for IO state) after the block has been
	added to the page hashtable. */

	if (buf_block_get_io_fix(block) == BUF_IO_READ) {

		/* Wait until the read operation completes */

		BPageMutex*	mutex = buf_page_get_mutex(&block->page);

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
	}
}

#ifdef BTR_CUR_HASH_ADAPT
/** If a stale adaptive hash index exists on the block, drop it.
Multiple executions of btr_search_drop_page_hash_index() on the
same block must be prevented by exclusive page latch. */
ATTRIBUTE_COLD
static void buf_defer_drop_ahi(buf_block_t *block, mtr_memo_type_t fix_type)
{
  switch (fix_type) {
  case MTR_MEMO_BUF_FIX:
    /* We do not drop the adaptive hash index, because safely doing
    so would require acquiring block->lock, and that is not safe
    to acquire in some RW_NO_LATCH access paths. Those code paths
    should have no business accessing the adaptive hash index anyway. */
    break;
  case MTR_MEMO_PAGE_S_FIX:
    /* Temporarily release our S-latch. */
    rw_lock_s_unlock(&block->lock);
    rw_lock_x_lock(&block->lock);
    if (dict_index_t *index= block->index)
      if (index->freed())
        btr_search_drop_page_hash_index(block);
    rw_lock_x_unlock(&block->lock);
    rw_lock_s_lock(&block->lock);
    break;
  case MTR_MEMO_PAGE_SX_FIX:
    rw_lock_sx_unlock(&block->lock);
    rw_lock_x_lock(&block->lock);
    if (dict_index_t *index= block->index)
      if (index->freed())
        btr_search_drop_page_hash_index(block);
    rw_lock_x_unlock(&block->lock);
    rw_lock_sx_lock(&block->lock);
    break;
  default:
    ut_ad(fix_type == MTR_MEMO_PAGE_X_FIX);
    btr_search_drop_page_hash_index(block);
  }
}
#endif /* BTR_CUR_HASH_ADAPT */

/** Lock the page with the given latch type.
@param[in,out]	block		block to be locked
@param[in]	rw_latch	RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	mtr		mini-transaction
@param[in]	file		file name
@param[in]	line		line where called
@return pointer to locked block */
static buf_block_t* buf_page_mtr_lock(buf_block_t *block,
                                      ulint rw_latch,
                                      mtr_t* mtr,
                                      const char *file,
                                      unsigned line)
{
  mtr_memo_type_t fix_type;
  switch (rw_latch)
  {
  case RW_NO_LATCH:
    fix_type= MTR_MEMO_BUF_FIX;
    goto done;
  case RW_S_LATCH:
    rw_lock_s_lock_inline(&block->lock, 0, file, line);
    fix_type= MTR_MEMO_PAGE_S_FIX;
    break;
  case RW_SX_LATCH:
    rw_lock_sx_lock_inline(&block->lock, 0, file, line);
    fix_type= MTR_MEMO_PAGE_SX_FIX;
    break;
  default:
    ut_ad(rw_latch == RW_X_LATCH);
    rw_lock_x_lock_inline(&block->lock, 0, file, line);
    fix_type= MTR_MEMO_PAGE_X_FIX;
    break;
  }

#ifdef BTR_CUR_HASH_ADAPT
  {
    dict_index_t *index= block->index;
    if (index && index->freed())
      buf_defer_drop_ahi(block, fix_type);
  }
#endif /* BTR_CUR_HASH_ADAPT */

done:
  mtr_memo_push(mtr, block, fix_type);
  return block;
}

/** This is the low level function used to get access to a database page.
@param[in]	page_id		page id
@param[in]	rw_latch	RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess		guessed block or NULL
@param[in]	mode		BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file		file name
@param[in]	line		line where called
@param[in]	mtr		mini-transaction
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_low(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err)
{
	buf_block_t*	block;
	unsigned	access_time;
	rw_lock_t*	hash_lock;
	buf_block_t*	fix_block;
	ulint		retries = 0;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	ut_ad((mtr == NULL) == (mode == BUF_EVICT_IF_IN_POOL));
	ut_ad(!mtr || mtr->is_active());
	ut_ad((rw_latch == RW_S_LATCH)
	      || (rw_latch == RW_X_LATCH)
	      || (rw_latch == RW_SX_LATCH)
	      || (rw_latch == RW_NO_LATCH));

	if (err) {
		*err = DB_SUCCESS;
	}

#ifdef UNIV_DEBUG
	switch (mode) {
	case BUF_EVICT_IF_IN_POOL:
		/* After DISCARD TABLESPACE, the tablespace would not exist,
		but in IMPORT TABLESPACE, PageConverter::operator() must
		replace any old pages, which were not evicted during DISCARD.
		Skip the assertion on space_page_size. */
		break;
	case BUF_PEEK_IF_IN_POOL:
	case BUF_GET_IF_IN_POOL:
		/* The caller may pass a dummy page size,
		because it does not really matter. */
		break;
	default:
		ut_error;
	case BUF_GET_NO_LATCH:
		ut_ad(rw_latch == RW_NO_LATCH);
		/* fall through */
	case BUF_GET:
	case BUF_GET_IF_IN_POOL_OR_WATCH:
	case BUF_GET_POSSIBLY_FREED:
		bool			found;
		const page_size_t&	space_page_size
			= fil_space_get_page_size(page_id.space(), &found);
		ut_ad(found);
		ut_ad(page_size.equals_to(space_page_size));
	}
#endif /* UNIV_DEBUG */

	ut_ad(!mtr || !ibuf_inside(mtr)
	      || ibuf_page_low(page_id, page_size, FALSE, file, line, NULL));

	buf_pool->stat.n_page_gets++;
	hash_lock = buf_page_hash_lock_get(buf_pool, page_id);
loop:
	block = guess;

	rw_lock_s_lock(hash_lock);

	/* If not own buf_pool_mutex, page_hash can be changed. */
	hash_lock = buf_page_hash_lock_s_confirm(hash_lock, buf_pool, page_id);

	if (block != NULL) {

		/* If the guess is a compressed page descriptor that
		has been allocated by buf_page_alloc_descriptor(),
		it may have been freed by buf_relocate(). */

		if (!buf_pool->is_block_field(block)
		    || page_id != block->page.id
		    || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {

			/* Our guess was bogus or things have changed
			since. */
			block = guess = NULL;
		} else {
			ut_ad(!block->page.in_zip_hash);
		}
	}

	if (block == NULL) {
		block = (buf_block_t*) buf_page_hash_get_low(buf_pool, page_id);
	}

	if (!block || buf_pool_watch_is_sentinel(buf_pool, &block->page)) {
		rw_lock_s_unlock(hash_lock);
		block = NULL;
	}

	if (block == NULL) {

		/* Page not in buf_pool: needs to be read from file */

		if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
			rw_lock_x_lock(hash_lock);

			/* If not own buf_pool_mutex,
			page_hash can be changed. */
			hash_lock = buf_page_hash_lock_x_confirm(
				hash_lock, buf_pool, page_id);

			block = (buf_block_t*) buf_pool_watch_set(
				page_id, &hash_lock);

			if (block) {
				/* We can release hash_lock after we
				increment the fix count to make
				sure that no state change takes place. */
				fix_block = block;

				if (fsp_is_system_temporary(page_id.space())) {
					/* For temporary tablespace,
					the mutex is being used for
					synchronization between user
					thread and flush thread,
					instead of block->lock. See
					buf_flush_page() for the flush
					thread counterpart. */

					BPageMutex*	fix_mutex
						= buf_page_get_mutex(
							&fix_block->page);
					mutex_enter(fix_mutex);
					buf_block_fix(fix_block);
					mutex_exit(fix_mutex);
				} else {
					buf_block_fix(fix_block);
				}

				/* Now safe to release page_hash mutex */
				rw_lock_x_unlock(hash_lock);
				goto got_block;
			}

			rw_lock_x_unlock(hash_lock);
		}

		switch (mode) {
		case BUF_GET_IF_IN_POOL:
		case BUF_GET_IF_IN_POOL_OR_WATCH:
		case BUF_PEEK_IF_IN_POOL:
		case BUF_EVICT_IF_IN_POOL:
			ut_ad(!rw_lock_own_flagged(
				      hash_lock,
				      RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));
			return(NULL);
		}

		/* The call path is buf_read_page() ->
		buf_read_page_low() (fil_io()) ->
		buf_page_io_complete() ->
		buf_decrypt_after_read(). Here fil_space_t* is used
		and we decrypt -> buf_page_check_corrupt() where page
		checksums are compared. Decryption, decompression as
		well as error handling takes place at a lower level.
		Here we only need to know whether the page really is
		corrupted, or if an encrypted page with a valid
		checksum cannot be decypted. */

		dberr_t local_err = buf_read_page(page_id, page_size);

		if (local_err == DB_SUCCESS) {
			buf_read_ahead_random(page_id, page_size,
					      ibuf_inside(mtr));

			retries = 0;
		} else if (mode == BUF_GET_POSSIBLY_FREED) {
			if (err) {
				*err = local_err;
			}
			return NULL;
		} else if (retries < BUF_PAGE_READ_MAX_RETRIES) {
			++retries;

			DBUG_EXECUTE_IF(
				"innodb_page_corruption_retries",
				retries = BUF_PAGE_READ_MAX_RETRIES;
			);
		} else {
			if (err) {
				*err = local_err;
			}

			/* Pages whose encryption key is unavailable or used
			key, encryption algorithm or encryption method is
			incorrect are marked as encrypted in
			buf_page_check_corrupt(). Unencrypted page could be
			corrupted in a way where the key_id field is
			nonzero. There is no checksum on field
			FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION. */
			if (local_err == DB_DECRYPTION_FAILED) {
				return (NULL);
			}

			if (local_err == DB_PAGE_CORRUPTED
			    && srv_force_recovery) {
				return NULL;
			}

			/* Try to set table as corrupted instead of
			asserting. */
			if (page_id.space() != TRX_SYS_SPACE &&
			    dict_set_corrupted_by_space(page_id.space())) {
				return (NULL);
			}

			if (local_err == DB_IO_ERROR) {
				return NULL;
			}

			ib::fatal() << "Unable to read page " << page_id
				<< " into the buffer pool after "
				<< BUF_PAGE_READ_MAX_RETRIES
				<< ". The most probable cause"
				" of this error may be that the"
				" table has been corrupted."
				" See https://mariadb.com/kb/en/library/innodb-recovery-modes/";
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(fsp_skip_sanity_check(page_id.space())
		     || ++buf_dbg_counter % 5771
		     || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		goto loop;
	} else {
		fix_block = block;
	}

	if (fsp_is_system_temporary(page_id.space())) {
		/* For temporary tablespace, the mutex is being used
		for synchronization between user thread and flush
		thread, instead of block->lock. See buf_flush_page()
		for the flush thread counterpart. */
		BPageMutex*	fix_mutex = buf_page_get_mutex(
			&fix_block->page);
		mutex_enter(fix_mutex);
		buf_block_fix(fix_block);
		mutex_exit(fix_mutex);
	} else {
		buf_block_fix(fix_block);
	}

	/* Now safe to release page_hash mutex */
	rw_lock_s_unlock(hash_lock);

got_block:

	switch (mode) {
	case BUF_GET_IF_IN_POOL:
	case BUF_PEEK_IF_IN_POOL:
	case BUF_EVICT_IF_IN_POOL:
		buf_page_t*	fix_page = &fix_block->page;
		BPageMutex*	fix_mutex = buf_page_get_mutex(fix_page);
		mutex_enter(fix_mutex);
		const bool	must_read
			= (buf_page_get_io_fix(fix_page) == BUF_IO_READ);
		mutex_exit(fix_mutex);

		if (must_read) {
			/* The page is being read to buffer pool,
			but we cannot wait around for the read to
			complete. */
			buf_block_unfix(fix_block);

			return(NULL);
		}
	}

	switch (buf_block_get_state(fix_block)) {
		buf_page_t*	bpage;

	case BUF_BLOCK_FILE_PAGE:
		bpage = &block->page;
		if (fsp_is_system_temporary(page_id.space())
		    && buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
			/* This suggests that the page is being flushed.
			Avoid returning reference to this page.
			Instead wait for the flush action to complete. */
			buf_block_unfix(fix_block);
			os_thread_sleep(WAIT_FOR_WRITE);
			goto loop;
		}

		if (UNIV_UNLIKELY(mode == BUF_EVICT_IF_IN_POOL)) {
evict_from_pool:
			ut_ad(!fix_block->page.oldest_modification);
			buf_pool_mutex_enter(buf_pool);
			buf_block_unfix(fix_block);

			if (!buf_LRU_free_page(&fix_block->page, true)) {
				ut_ad(0);
			}

			buf_pool_mutex_exit(buf_pool);
			return(NULL);
		}

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

		/* Note: We have already buffer fixed this block. */
		if (bpage->buf_fix_count > 1
		    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

			/* This condition often occurs when the buffer
			is not buffer-fixed, but I/O-fixed by
			buf_page_init_for_read(). */
			buf_block_unfix(fix_block);

			/* The block is buffer-fixed or I/O-fixed.
			Try again later. */
			os_thread_sleep(WAIT_FOR_READ);

			goto loop;
		}

		if (UNIV_UNLIKELY(mode == BUF_EVICT_IF_IN_POOL)) {
			goto evict_from_pool;
		}

		/* Buffer-fix the block so that it cannot be evicted
		or relocated while we are attempting to allocate an
		uncompressed page. */

		block = buf_LRU_get_free_block(buf_pool);

		buf_pool_mutex_enter(buf_pool);

		/* If not own buf_pool_mutex, page_hash can be changed. */
		hash_lock = buf_page_hash_lock_get(buf_pool, page_id);

		rw_lock_x_lock(hash_lock);

		/* Buffer-fixing prevents the page_hash from changing. */
		ut_ad(bpage == buf_page_hash_get_low(buf_pool, page_id));

		buf_block_unfix(fix_block);

		buf_page_mutex_enter(block);
		mutex_enter(&buf_pool->zip_mutex);

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
			buf_pool_mutex_exit(buf_pool);
			rw_lock_x_unlock(hash_lock);
			buf_page_mutex_exit(block);

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

		/* Set after buf_relocate(). */
		block->page.buf_fix_count = 1;

		block->lock_hash_val = lock_rec_hash(page_id.space(),
						     page_id.page_no());

		if (buf_page_get_state(&block->page) == BUF_BLOCK_ZIP_PAGE) {
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
			UT_LIST_REMOVE(buf_pool->zip_clean, &block->page);
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

		buf_block_set_io_fix(block, BUF_IO_READ);
		rw_lock_x_lock_inline(&block->lock, 0, file, line);

		MEM_UNDEFINED(bpage, sizeof *bpage);

		rw_lock_x_unlock(hash_lock);
		buf_pool->n_pend_unzip++;
		mutex_exit(&buf_pool->zip_mutex);
		buf_pool_mutex_exit(buf_pool);

		access_time = buf_page_is_accessed(&block->page);

		buf_page_mutex_exit(block);

		buf_page_free_descriptor(bpage);

		/* Decompress the page while not holding
		buf_pool->mutex or block->mutex. */

		{
			bool	success = buf_zip_decompress(block, false);

			if (!success) {
				buf_pool_mutex_enter(buf_pool);
				buf_page_mutex_enter(fix_block);
				buf_block_set_io_fix(fix_block, BUF_IO_NONE);
				buf_page_mutex_exit(fix_block);

				--buf_pool->n_pend_unzip;
				buf_block_unfix(fix_block);
				buf_pool_mutex_exit(buf_pool);
				rw_lock_x_unlock(&fix_block->lock);

				if (err) {
					*err = DB_PAGE_CORRUPTED;
				}
				return NULL;
			}
		}

		if (!access_time && !recv_no_ibuf_operations) {
			ibuf_merge_or_delete_for_page(
				block, page_id, page_size);
		}

		buf_pool_mutex_enter(buf_pool);

		buf_page_mutex_enter(fix_block);

		buf_block_set_io_fix(fix_block, BUF_IO_NONE);

		buf_page_mutex_exit(fix_block);

		--buf_pool->n_pend_unzip;

		buf_pool_mutex_exit(buf_pool);

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

	ut_ad(!rw_lock_own_flagged(hash_lock,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	ut_ad(buf_block_get_state(fix_block) == BUF_BLOCK_FILE_PAGE);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG

	if ((mode == BUF_GET_IF_IN_POOL || mode == BUF_GET_IF_IN_POOL_OR_WATCH)
	    && (ibuf_debug || buf_debug_execute_is_force_flush())) {

		/* Try to evict the block from the buffer pool, to use the
		insert buffer (change buffer) as much as possible. */

		buf_pool_mutex_enter(buf_pool);

		buf_block_unfix(fix_block);

		/* Now we are only holding the buf_pool->mutex,
		not block->mutex or hash_lock. Blocks cannot be
		relocated or enter or exit the buf_pool while we
		are holding the buf_pool->mutex. */

		if (buf_LRU_free_page(&fix_block->page, true)) {

			buf_pool_mutex_exit(buf_pool);

			/* If not own buf_pool_mutex,
			page_hash can be changed. */
			hash_lock = buf_page_hash_lock_get(buf_pool, page_id);

			rw_lock_x_lock(hash_lock);

			/* If not own buf_pool_mutex,
			page_hash can be changed. */
			hash_lock = buf_page_hash_lock_x_confirm(
				hash_lock, buf_pool, page_id);

			if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
				/* Set the watch, as it would have
				been set if the page were not in the
				buffer pool in the first place. */
				block = (buf_block_t*) buf_pool_watch_set(
					page_id, &hash_lock);
			} else {
				block = (buf_block_t*) buf_page_hash_get_low(
					buf_pool, page_id);
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

			return(NULL);
		}

		buf_page_mutex_enter(fix_block);

		if (buf_flush_page_try(buf_pool, fix_block)) {
			guess = fix_block;

			goto loop;
		}

		buf_page_mutex_exit(fix_block);

		buf_block_fix(fix_block);

		/* Failed to evict the page; change it directly */

		buf_pool_mutex_exit(buf_pool);
	}
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

	ut_ad(fix_block->page.buf_fix_count > 0);

#ifdef UNIV_DEBUG
	/* We have already buffer fixed the page, and we are committed to
	returning this page to the caller. Register for debugging.
	Avoid debug latching if page/block belongs to system temporary
	tablespace (Not much needed for table with single threaded access.). */
	if (!fsp_is_system_temporary(page_id.space())) {
		ibool   ret;
		ret = rw_lock_s_lock_nowait(
			&fix_block->debug_latch, file, line);
		ut_a(ret);
	}
#endif /* UNIV_DEBUG */

	/* While tablespace is reinited the indexes are already freed but the
	blocks related to it still resides in buffer pool. Trying to remove
	such blocks from buffer pool would invoke removal of AHI entries
	associated with these blocks. Logic to remove AHI entry will try to
	load the block but block is already in free state. Handle the said case
	with mode = BUF_PEEK_IF_IN_POOL that is invoked from
	"btr_search_drop_page_hash_when_freed". */
	ut_ad(mode == BUF_GET_POSSIBLY_FREED
	      || mode == BUF_PEEK_IF_IN_POOL
	      || !fix_block->page.file_page_was_freed);

	/* Check if this is the first access to the page */
	access_time = buf_page_is_accessed(&fix_block->page);

	/* This is a heuristic and we don't care about ordering issues. */
	if (access_time == 0) {
		buf_page_mutex_enter(fix_block);

		buf_page_set_accessed(&fix_block->page);

		buf_page_mutex_exit(fix_block);
	}

	if (mode != BUF_PEEK_IF_IN_POOL) {
		buf_page_make_young_if_needed(&fix_block->page);
	}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(fsp_skip_sanity_check(page_id.space())
	     || ++buf_dbg_counter % 5771
	     || buf_validate());
	ut_a(buf_block_get_state(fix_block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	/* We have to wait here because the IO_READ state was set
	under the protection of the hash_lock and not the block->mutex
	and block->lock. */
	buf_wait_for_read(fix_block);

	if (fix_block->page.id != page_id) {

		buf_block_unfix(fix_block);

#ifdef UNIV_DEBUG
		if (!fsp_is_system_temporary(page_id.space())) {
			rw_lock_s_unlock(&fix_block->debug_latch);
		}
#endif /* UNIV_DEBUG */

		if (err) {
			*err = DB_PAGE_CORRUPTED;
		}

		return NULL;
	}

	fix_block = buf_page_mtr_lock(fix_block, rw_latch, mtr, file, line);

	if (mode != BUF_PEEK_IF_IN_POOL && !access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(page_id, page_size, ibuf_inside(mtr));
	}

	ut_ad(!rw_lock_own_flagged(hash_lock,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	return(fix_block);
}

/** This is the general function used to get access to a database page.
It does page initialization and applies the buffered redo logs.
@param[in]	page_id		page id
@param[in]	rw_latch	RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess		guessed block or NULL
@param[in]	mode		BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file		file name
@param[in]	line		line where called
@param[in]	mtr		mini-transaction
@param[out]	err		DB_SUCCESS or error code
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err)
{
  if (buf_block_t *block = recv_recovery_create_page(page_id))
  {
    buf_block_fix(block);
    ut_ad(rw_lock_s_lock_nowait(&block->debug_latch, file, line));
    block= buf_page_mtr_lock(block, rw_latch, mtr, file, line);
    return block;
  }

  return buf_page_get_low(page_id, page_size, rw_latch,
                          guess, mode, file, line, mtr, err);
}

/********************************************************************//**
This is the general function used to get optimistic access to a database
page.
@return TRUE if success */
ibool
buf_page_optimistic_get(
/*====================*/
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/*!< in: guessed buffer block */
	ib_uint64_t	modify_clock,/*!< in: modify clock value */
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line where called */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	buf_pool_t*	buf_pool;
	unsigned	access_time;
	ibool		success;

	ut_ad(block);
	ut_ad(mtr);
	ut_ad(mtr->is_active());
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	buf_page_mutex_enter(block);

	if (UNIV_UNLIKELY(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE)) {

		buf_page_mutex_exit(block);

		return(FALSE);
	}

	buf_block_buf_fix_inc(block, file, line);

	access_time = buf_page_is_accessed(&block->page);

	buf_page_set_accessed(&block->page);

	buf_page_mutex_exit(block);

	buf_page_make_young_if_needed(&block->page);

	ut_ad(!ibuf_inside(mtr)
	      || ibuf_page(block->page.id, block->page.size, NULL));

	mtr_memo_type_t	fix_type;

	switch (rw_latch) {
	case RW_S_LATCH:
		success = rw_lock_s_lock_nowait(&block->lock, file, line);

		fix_type = MTR_MEMO_PAGE_S_FIX;
		break;
	case RW_X_LATCH:
		success = rw_lock_x_lock_func_nowait_inline(
			&block->lock, file, line);

		fix_type = MTR_MEMO_PAGE_X_FIX;
		break;
	default:
		ut_error; /* RW_SX_LATCH is not implemented yet */
	}

	if (!success) {
		buf_block_buf_fix_dec(block);
		return(FALSE);
	}

	if (modify_clock != block->modify_clock) {

		buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

		if (rw_latch == RW_S_LATCH) {
			rw_lock_s_unlock(&block->lock);
		} else {
			rw_lock_x_unlock(&block->lock);
		}

		buf_block_buf_fix_dec(block);
		return(FALSE);
	}

	mtr_memo_push(mtr, block, fix_type);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(fsp_skip_sanity_check(block->page.id.space())
	     || ++buf_dbg_counter % 5771
	     || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	ut_d(buf_page_mutex_enter(block));
	ut_ad(!block->page.file_page_was_freed);
	ut_d(buf_page_mutex_exit(block));

	if (!access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */
		buf_read_ahead_linear(block->page.id, block->page.size,
				      ibuf_inside(mtr));
	}

	buf_pool = buf_pool_from_block(block);
	buf_pool->stat.n_page_gets++;

	return(TRUE);
}

/********************************************************************//**
This is used to get access to a known database page, when no waiting can be
done. For example, if a search in an adaptive hash index leads us to this
frame.
@return TRUE if success */
ibool
buf_page_get_known_nowait(
/*======================*/
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/*!< in: the known page */
	ulint		mode,	/*!< in: BUF_MAKE_YOUNG or BUF_KEEP_OLD */
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line where called */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	buf_pool_t*	buf_pool;
	ibool		success;

	ut_ad(mtr->is_active());
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	buf_page_mutex_enter(block);

	if (buf_block_get_state(block) == BUF_BLOCK_REMOVE_HASH) {
		/* Another thread is just freeing the block from the LRU list
		of the buffer pool: do not try to access this page; this
		attempt to access the page can only come through the hash
		index because when the buffer block state is ..._REMOVE_HASH,
		we have already removed it from the page address hash table
		of the buffer pool. */

		buf_page_mutex_exit(block);

		return(FALSE);
	}

	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	buf_block_buf_fix_inc(block, file, line);

	buf_page_set_accessed(&block->page);

	buf_page_mutex_exit(block);

	buf_pool = buf_pool_from_block(block);

#ifdef BTR_CUR_HASH_ADAPT
	if (mode == BUF_MAKE_YOUNG) {
		buf_page_make_young_if_needed(&block->page);
	}
#endif /* BTR_CUR_HASH_ADAPT */

	ut_ad(!ibuf_inside(mtr) || mode == BUF_KEEP_OLD);

	mtr_memo_type_t	fix_type;

	switch (rw_latch) {
	case RW_S_LATCH:
		success = rw_lock_s_lock_nowait(&block->lock, file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
		break;
	case RW_X_LATCH:
		success = rw_lock_x_lock_func_nowait_inline(
			&block->lock, file, line);

		fix_type = MTR_MEMO_PAGE_X_FIX;
		break;
	default:
		ut_error; /* RW_SX_LATCH is not implemented yet */
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

#ifdef UNIV_DEBUG
	if (mode != BUF_KEEP_OLD) {
		/* If mode == BUF_KEEP_OLD, we are executing an I/O
		completion routine.  Avoid a bogus assertion failure
		when ibuf_merge_or_delete_for_page() is processing a
		page that was just freed due to DROP INDEX, or
		deleting a record from SYS_INDEXES. This check will be
		skipped in recv_recover_page() as well. */

# ifdef BTR_CUR_HASH_ADAPT
		ut_ad(!block->page.file_page_was_freed
		      || (block->index && block->index->freed()));
# else /* BTR_CUR_HASH_ADAPT */
		ut_ad(!block->page.file_page_was_freed);
# endif /* BTR_CUR_HASH_ADAPT */
	}
#endif /* UNIV_DEBUG */

	buf_pool->stat.n_page_gets++;

	return(TRUE);
}

/** Given a tablespace id and page number tries to get that page. If the
page is not in the buffer pool it is not loaded and NULL is returned.
Suitable for using when holding the lock_sys_t::mutex.
@param[in]	page_id	page id
@param[in]	file	file name
@param[in]	line	line where called
@param[in]	mtr	mini-transaction
@return pointer to a page or NULL */
buf_block_t*
buf_page_try_get_func(
	const page_id_t		page_id,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr)
{
	buf_block_t*	block;
	ibool		success;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);
	rw_lock_t*	hash_lock;

	ut_ad(mtr);
	ut_ad(mtr->is_active());

	block = buf_block_hash_get_s_locked(buf_pool, page_id, &hash_lock);

	if (!block || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
		if (block) {
			rw_lock_s_unlock(hash_lock);
		}
		return(NULL);
	}

	ut_ad(!buf_pool_watch_is_sentinel(buf_pool, &block->page));

	buf_page_mutex_enter(block);
	rw_lock_s_unlock(hash_lock);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_a(page_id == block->page.id);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_block_buf_fix_inc(block, file, line);
	buf_page_mutex_exit(block);

	mtr_memo_type_t	fix_type = MTR_MEMO_PAGE_S_FIX;
	success = rw_lock_s_lock_nowait(&block->lock, file, line);

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
	ut_a(fsp_skip_sanity_check(block->page.id.space())
	     || ++buf_dbg_counter % 5771
	     || buf_validate());
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	ut_d(buf_page_mutex_enter(block));
	ut_d(ut_a(!block->page.file_page_was_freed));
	ut_d(buf_page_mutex_exit(block));

	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	buf_pool->stat.n_page_gets++;

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
	bpage->old = 0;
	bpage->freed_page_clock = 0;
	bpage->access_time = 0;
	bpage->newest_modification = 0;
	bpage->oldest_modification = 0;
	bpage->real_size = 0;
	bpage->slot = NULL;

	HASH_INVALIDATE(bpage, hash);

	ut_d(bpage->file_page_was_freed = FALSE);
}

/** Inits a page to the buffer buf_pool.
@param[in,out]	buf_pool	buffer pool
@param[in]	page_id		page id
@param[in,out]	block		block to init */
static
void
buf_page_init(
	buf_pool_t*		buf_pool,
	const page_id_t		page_id,
	const page_size_t&	page_size,
	buf_block_t*		block)
{
	buf_page_t*	hash_page;

	ut_ad(buf_pool == buf_pool_get(page_id));
	ut_ad(buf_pool_mutex_own(buf_pool));

	ut_ad(buf_page_mutex_own(block));
	ut_a(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE);

	ut_ad(rw_lock_own(buf_page_hash_lock_get(buf_pool, page_id),
			  RW_LOCK_X));

	/* Set the state of the block */
	buf_block_set_file_page(block, page_id);

	buf_block_init_low(block);

	block->lock_hash_val = lock_rec_hash(page_id.space(),
					     page_id.page_no());

	buf_page_init_low(&block->page);

	/* Insert into the hash table of file pages */

	hash_page = buf_page_hash_get_low(buf_pool, page_id);

	if (hash_page == NULL) {
		/* Block not found in hash table */
	} else if (UNIV_LIKELY(buf_pool_watch_is_sentinel(buf_pool,
							  hash_page))) {
		/* Preserve the reference count. */
		ib_uint32_t	buf_fix_count = hash_page->buf_fix_count;

		ut_a(buf_fix_count > 0);

		my_atomic_add32((int32*) &block->page.buf_fix_count, buf_fix_count);

		buf_pool_watch_remove(buf_pool, hash_page);
	} else {
		ib::fatal() << "Page already foudn in the hash table: "
			    << page_id;
	}

	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_page_hash);
	ut_d(block->page.in_page_hash = TRUE);

	block->page.id = page_id;
	block->page.size.copy_from(page_size);

	HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
		    page_id.fold(), &block->page);

	if (page_size.is_compressed()) {
		page_zip_set_size(&block->page.zip, page_size.physical());
	}
}

/** Initialize a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if we specify to read only ibuf pages and the page is not an ibuf page, or
(3) if the space is deleted or being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later.
@param[out]	err			DB_SUCCESS or DB_TABLESPACE_DELETED
@param[in]	mode			BUF_READ_IBUF_PAGES_ONLY, ...
@param[in]	page_id			page id
@param[in]	unzip			whether the uncompressed page is
					requested (for ROW_FORMAT=COMPRESSED)
@return pointer to the block
@retval	NULL	in case of an error */
buf_page_t*
buf_page_init_for_read(
	dberr_t*		err,
	ulint			mode,
	const page_id_t		page_id,
	const page_size_t&	page_size,
	bool			unzip)
{
	buf_block_t*	block;
	buf_page_t*	bpage	= NULL;
	buf_page_t*	watch_page;
	rw_lock_t*	hash_lock;
	mtr_t		mtr;
	ibool		lru	= FALSE;
	void*		data;
	buf_pool_t*	buf_pool = buf_pool_get(page_id);

	ut_ad(buf_pool);

	*err = DB_SUCCESS;

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {
		/* It is a read-ahead within an ibuf routine */

		ut_ad(!ibuf_bitmap_page(page_id, page_size));

		ibuf_mtr_start(&mtr);

		if (!recv_no_ibuf_operations &&
		    !ibuf_page(page_id, page_size, &mtr)) {

			ibuf_mtr_commit(&mtr);

			return(NULL);
		}
	} else {
		ut_ad(mode == BUF_READ_ANY_PAGE);
	}

	if (page_size.is_compressed() && !unzip && !recv_recovery_is_on()) {
		block = NULL;
	} else {
		block = buf_LRU_get_free_block(buf_pool);
		ut_ad(block);
		ut_ad(buf_pool_from_block(block) == buf_pool);
	}

	buf_pool_mutex_enter(buf_pool);

	hash_lock = buf_page_hash_lock_get(buf_pool, page_id);
	rw_lock_x_lock(hash_lock);

	watch_page = buf_page_hash_get_low(buf_pool, page_id);
	if (watch_page && !buf_pool_watch_is_sentinel(buf_pool, watch_page)) {
		/* The page is already in the buffer pool. */
		watch_page = NULL;
		rw_lock_x_unlock(hash_lock);
		if (block) {
			buf_page_mutex_enter(block);
			buf_LRU_block_free_non_file_page(block);
			buf_page_mutex_exit(block);
		}

		bpage = NULL;
		goto func_exit;
	}

	if (block) {
		bpage = &block->page;

		buf_page_mutex_enter(block);

		ut_ad(buf_pool_from_bpage(bpage) == buf_pool);

		buf_page_init(buf_pool, page_id, page_size, block);

		/* Note: We are using the hash_lock for protection. This is
		safe because no other thread can lookup the block from the
		page hashtable yet. */

		buf_page_set_io_fix(bpage, BUF_IO_READ);

		rw_lock_x_unlock(hash_lock);

		/* The block must be put to the LRU list, to the old blocks */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);

		/* We set a pass-type x-lock on the frame because then
		the same thread which called for the read operation
		(and is running now at this point of code) can wait
		for the read to complete by waiting for the x-lock on
		the frame; if the x-lock were recursive, the same
		thread would illegally get the x-lock before the page
		read is completed.  The x-lock is cleared by the
		io-handler thread. */

		rw_lock_x_lock_gen(&block->lock, BUF_IO_READ);

		if (page_size.is_compressed()) {
			/* buf_pool->mutex may be released and
			reacquired by buf_buddy_alloc().  Thus, we
			must release block->mutex in order not to
			break the latching order in the reacquisition
			of buf_pool->mutex.  We also must defer this
			operation until after the block descriptor has
			been added to buf_pool->LRU and
			buf_pool->page_hash. */
			buf_page_mutex_exit(block);
			data = buf_buddy_alloc(buf_pool, page_size.physical(),
					       &lru);
			buf_page_mutex_enter(block);
			block->page.zip.data = (page_zip_t*) data;

			/* To maintain the invariant
			block->in_unzip_LRU_list
			== buf_page_belongs_to_unzip_LRU(&block->page)
			we have to add this block to unzip_LRU
			after block->page.zip.data is set. */
			ut_ad(buf_page_belongs_to_unzip_LRU(&block->page));
			buf_unzip_LRU_add_block(block, TRUE);
		}

		buf_page_mutex_exit(block);
	} else {
		rw_lock_x_unlock(hash_lock);

		/* The compressed page must be allocated before the
		control block (bpage), in order to avoid the
		invocation of buf_buddy_relocate_block() on
		uninitialized data. */
		data = buf_buddy_alloc(buf_pool, page_size.physical(), &lru);

		rw_lock_x_lock(hash_lock);

		/* If buf_buddy_alloc() allocated storage from the LRU list,
		it released and reacquired buf_pool->mutex.  Thus, we must
		check the page_hash again, as it may have been modified. */
		if (UNIV_UNLIKELY(lru)) {

			watch_page = buf_page_hash_get_low(buf_pool, page_id);

			if (UNIV_UNLIKELY(watch_page
			    && !buf_pool_watch_is_sentinel(buf_pool,
							   watch_page))) {

				/* The block was added by some other thread. */
				rw_lock_x_unlock(hash_lock);
				watch_page = NULL;
				buf_buddy_free(buf_pool, data,
					       page_size.physical());

				bpage = NULL;
				goto func_exit;
			}
		}

		bpage = buf_page_alloc_descriptor();

		/* Initialize the buf_pool pointer. */
		bpage->buf_pool_index = buf_pool_index(buf_pool);

		page_zip_des_init(&bpage->zip);
		page_zip_set_size(&bpage->zip, page_size.physical());
		bpage->zip.data = (page_zip_t*) data;

		bpage->size.copy_from(page_size);

		mutex_enter(&buf_pool->zip_mutex);

		buf_page_init_low(bpage);

		bpage->state = BUF_BLOCK_ZIP_PAGE;
		bpage->id = page_id;
		bpage->flush_observer = NULL;

		ut_d(bpage->in_page_hash = FALSE);
		ut_d(bpage->in_zip_hash = FALSE);
		ut_d(bpage->in_flush_list = FALSE);
		ut_d(bpage->in_free_list = FALSE);
		ut_d(bpage->in_LRU_list = FALSE);

		ut_d(bpage->in_page_hash = TRUE);

		if (watch_page != NULL) {

			/* Preserve the reference count. */
			ib_uint32_t	buf_fix_count;

			buf_fix_count = watch_page->buf_fix_count;

			ut_a(buf_fix_count > 0);

			my_atomic_add32((int32*) &bpage->buf_fix_count, buf_fix_count);

			ut_ad(buf_pool_watch_is_sentinel(buf_pool, watch_page));
			buf_pool_watch_remove(buf_pool, watch_page);
		}

		HASH_INSERT(buf_page_t, hash, buf_pool->page_hash,
			    bpage->id.fold(), bpage);

		rw_lock_x_unlock(hash_lock);

		/* The block must be put to the LRU list, to the old blocks.
		The zip size is already set into the page zip */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		buf_page_set_io_fix(bpage, BUF_IO_READ);

		mutex_exit(&buf_pool->zip_mutex);
	}

	buf_pool->n_pend_reads++;
func_exit:
	buf_pool_mutex_exit(buf_pool);

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {

		ibuf_mtr_commit(&mtr);
	}

	ut_ad(!rw_lock_own_flagged(hash_lock,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));
	ut_ad(!bpage || buf_page_in_file(bpage));

	return(bpage);
}

/** Initializes a page to the buffer buf_pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen).
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	mtr		mini-transaction
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	mtr_t*			mtr)
{
	buf_frame_t*	frame;
	buf_block_t*	block;
	buf_block_t*	free_block	= NULL;
	buf_pool_t*	buf_pool= buf_pool_get(page_id);
	rw_lock_t*	hash_lock;

	ut_ad(mtr->is_active());
	ut_ad(page_id.space() != 0 || !page_size.is_compressed());
loop:
	free_block = buf_LRU_get_free_block(buf_pool);
	buf_pool_mutex_enter(buf_pool);

	hash_lock = buf_page_hash_lock_get(buf_pool, page_id);
	rw_lock_x_lock(hash_lock);

	block = (buf_block_t*) buf_page_hash_get_low(buf_pool, page_id);

	if (block
	    && buf_page_in_file(&block->page)
	    && !buf_pool_watch_is_sentinel(buf_pool, &block->page)) {
		ut_d(block->page.file_page_was_freed = FALSE);
		buf_page_state page_state = buf_block_get_state(block);
		bool have_x_latch = false;
#ifdef BTR_CUR_HASH_ADAPT
		const dict_index_t *drop_hash_entry= NULL;
#endif
		switch (page_state) {
		default:
			ut_ad(0);
			break;
		case BUF_BLOCK_ZIP_PAGE:
		case BUF_BLOCK_ZIP_DIRTY:
			buf_block_init_low(free_block);
			mutex_enter(&buf_pool->zip_mutex);

			buf_page_mutex_enter(free_block);
			if (buf_page_get_io_fix(&block->page) != BUF_IO_NONE) {
				mutex_exit(&buf_pool->zip_mutex);
				rw_lock_x_unlock(hash_lock);
				buf_LRU_block_free_non_file_page(free_block);
				buf_pool_mutex_exit(buf_pool);
				buf_page_mutex_exit(free_block);

				goto loop;
			}

			rw_lock_x_lock(&free_block->lock);

			buf_relocate(&block->page, &free_block->page);
			if (page_state == BUF_BLOCK_ZIP_DIRTY) {
				ut_ad(block->page.in_flush_list);
				ut_ad(block->page.oldest_modification > 0);
				buf_flush_relocate_on_flush_list(
					&block->page, &free_block->page);
			} else {
				ut_ad(block->page.oldest_modification == 0);
				ut_ad(!block->page.in_flush_list);
#ifdef UNIV_DEBUG
				UT_LIST_REMOVE(
					buf_pool->zip_clean, &block->page);
#endif
			}

			free_block->page.state = BUF_BLOCK_FILE_PAGE;
			mutex_exit(&buf_pool->zip_mutex);
			free_block->lock_hash_val = lock_rec_hash(
					page_id.space(), page_id.page_no());
			buf_unzip_LRU_add_block(free_block, false);
			buf_page_free_descriptor(&block->page);
			block = free_block;
			buf_block_fix(block);
			buf_page_mutex_exit(free_block);
			free_block = NULL;
			break;
		case BUF_BLOCK_FILE_PAGE:
			have_x_latch = mtr->have_x_latch(*block);
			if (!have_x_latch) {
				buf_block_fix(block);
				buf_page_mutex_enter(block);
				while (buf_block_get_io_fix(block)
				       != BUF_IO_NONE
				       || block->page.buf_fix_count != 1) {
					buf_page_mutex_exit(block);
					buf_pool_mutex_exit(buf_pool);
					rw_lock_x_unlock(hash_lock);

					os_thread_sleep(1000);

					buf_pool_mutex_enter(buf_pool);
					rw_lock_x_lock(hash_lock);
					buf_page_mutex_enter(block);
				}
				rw_lock_x_lock(&block->lock);
				buf_page_mutex_exit(block);
			}
#ifdef BTR_CUR_HASH_ADAPT
			drop_hash_entry = block->index;
#endif
			break;
		}
		/* Page can be found in buf_pool */
		buf_pool_mutex_exit(buf_pool);
		rw_lock_x_unlock(hash_lock);

		if (free_block) {
			buf_block_free(free_block);
		}
#ifdef BTR_CUR_HASH_ADAPT
		if (drop_hash_entry) {
			btr_search_drop_page_hash_index(block);
		}
#endif /* BTR_CUR_HASH_ADAPT */

		if (!have_x_latch) {
#ifdef UNIV_DEBUG
			if (!fsp_is_system_temporary(page_id.space())) {
				rw_lock_s_lock_nowait(
					&block->debug_latch,
					__FILE__, __LINE__);
			}
#endif /* UNIV_DEBUG */

			mtr_memo_push(mtr, block, MTR_MEMO_PAGE_X_FIX);
		}
		return block;
	}

	/* If we get here, the page was not in buf_pool: init it there */

	DBUG_PRINT("ib_buf", ("create page %u:%u",
			      page_id.space(), page_id.page_no()));

	block = free_block;

	buf_page_mutex_enter(block);

	buf_page_init(buf_pool, page_id, page_size, block);

	rw_lock_x_lock(&block->lock);

	rw_lock_x_unlock(hash_lock);

	/* The block must be put to the LRU list */
	buf_LRU_add_block(&block->page, FALSE);

	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	buf_pool->stat.n_pages_created++;

	if (page_size.is_compressed()) {
		void*	data;
		ibool	lru;

		/* Prevent race conditions during buf_buddy_alloc(),
		which may release and reacquire buf_pool->mutex,
		by IO-fixing and X-latching the block. */

		buf_page_set_io_fix(&block->page, BUF_IO_READ);

		buf_page_mutex_exit(block);
		/* buf_pool->mutex may be released and reacquired by
		buf_buddy_alloc().  Thus, we must release block->mutex
		in order not to break the latching order in
		the reacquisition of buf_pool->mutex.  We also must
		defer this operation until after the block descriptor
		has been added to buf_pool->LRU and buf_pool->page_hash. */
		data = buf_buddy_alloc(buf_pool, page_size.physical(), &lru);
		buf_page_mutex_enter(block);
		block->page.zip.data = (page_zip_t*) data;

		/* To maintain the invariant
		block->in_unzip_LRU_list
		== buf_page_belongs_to_unzip_LRU(&block->page)
		we have to add this block to unzip_LRU after
		block->page.zip.data is set. */
		ut_ad(buf_page_belongs_to_unzip_LRU(&block->page));
		buf_unzip_LRU_add_block(block, FALSE);

		buf_page_set_io_fix(&block->page, BUF_IO_NONE);
	}

	buf_pool_mutex_exit(buf_pool);

	mtr_memo_push(mtr, block, MTR_MEMO_PAGE_X_FIX);

	buf_page_set_accessed(&block->page);

	buf_page_mutex_exit(block);

	/* Delete possible entries for the page from the insert buffer:
	such can exist if the page belonged to an index which was dropped */
	if (!recv_recovery_is_on()) {
		ibuf_merge_or_delete_for_page(NULL, page_id, page_size);
	}

	frame = block->frame;

	memset(frame + FIL_PAGE_PREV, 0xff, 4);
	memset(frame + FIL_PAGE_NEXT, 0xff, 4);
	mach_write_to_2(frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_ALLOCATED);

	/* FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION is only used on the
	following pages:
	(1) The first page of the InnoDB system tablespace (page 0:0)
	(2) FIL_RTREE_SPLIT_SEQ_NUM on R-tree pages
	(3) key_version on encrypted pages (not page 0:0) */

	memset(frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
	memset(frame + FIL_PAGE_LSN, 0, 8);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(++buf_dbg_counter % 5771 || buf_validate());
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
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
	case FIL_PAGE_RTREE:
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

/** Mark a table corrupted.
@param[in]	bpage	corrupted page
@param[in]	space	tablespace of the corrupted page */
ATTRIBUTE_COLD
static void buf_mark_space_corrupt(buf_page_t* bpage, const fil_space_t& space)
{
	/* If block is not encrypted find the table with specified
	space id, and mark it corrupted. Encrypted tables
	are marked unusable later e.g. in ::open(). */
	if (!space.crypt_data
	    || space.crypt_data->type == CRYPT_SCHEME_UNENCRYPTED) {
		dict_set_corrupted_by_space(bpage->id.space());
	} else {
		dict_set_encrypted_by_space(bpage->id.space());
	}
}

/** Mark a table corrupted.
@param[in]	bpage	Corrupted page
@param[in]	space	Corrupted page belongs to tablespace
Also remove the bpage from LRU list. */
static
void
buf_corrupt_page_release(buf_page_t* bpage, const fil_space_t* space)
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	const ibool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);
	page_id_t	old_page_id = bpage->id;

	/* First unfix and release lock on the bpage */
	buf_pool_mutex_enter(buf_pool);
	mutex_enter(buf_page_get_mutex(bpage));
	ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_READ);
	ut_ad(bpage->id.space() == space->id);

	/* buf_fix_count can be greater than zero. Because other thread
	can wait in buf_page_wait_read() for the page to be read. */

	bpage->id.set_corrupt_id();
	/* Set BUF_IO_NONE before we remove the block from LRU list */
	buf_page_set_io_fix(bpage, BUF_IO_NONE);

	if (uncompressed) {
		rw_lock_x_unlock_gen(
			&((buf_block_t*) bpage)->lock,
			BUF_IO_READ);
	}

	mutex_exit(buf_page_get_mutex(bpage));

	if (!srv_force_recovery) {
		buf_mark_space_corrupt(bpage, *space);
	}

	/* After this point bpage can't be referenced. */
	buf_LRU_free_one_page(bpage, old_page_id);

	ut_ad(buf_pool->n_pend_reads > 0);
	buf_pool->n_pend_reads--;

	buf_pool_mutex_exit(buf_pool);
}

/** Check if page is maybe compressed, encrypted or both when we encounter
corrupted page. Note that we can't be 100% sure if page is corrupted
or decrypt/decompress just failed.
@param[in,out]	bpage		page
@param[in,out]	space		tablespace from fil_space_acquire_for_io()
@return	whether the operation succeeded
@retval	DB_SUCCESS		if page has been read and is not corrupted
@retval	DB_PAGE_CORRUPTED	if page based on checksum check is corrupted
@retval	DB_DECRYPTION_FAILED	if page post encryption checksum matches but
after decryption normal page checksum does not match.
@retval	DB_TABLESPACE_DELETED	if accessed tablespace is not found */
static dberr_t buf_page_check_corrupt(buf_page_t* bpage, fil_space_t* space)
{
	ut_ad(space->n_pending_ios > 0);

	byte* dst_frame = (bpage->zip.data) ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	dberr_t err = DB_SUCCESS;

	/* In buf_decrypt_after_read we have either decrypted the page if
	page post encryption checksum matches and used key_id is found
	from the encryption plugin. If checksum did not match page was
	not decrypted and it could be either encrypted and corrupted
	or corrupted or good page. If we decrypted, there page could
	still be corrupted if used key does not match. */
	const bool seems_encrypted = mach_read_from_4(
		dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION)
		&& space->crypt_data
		&& space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED;

	/* If traditional checksums match, we assume that page is
	not anymore encrypted. */
	if (buf_page_is_corrupted(
		true, dst_frame, bpage->size, space)) {
		err = DB_PAGE_CORRUPTED;
	}

	if (seems_encrypted && err == DB_PAGE_CORRUPTED
	    && bpage->id.page_no() != 0) {
		err = DB_DECRYPTION_FAILED;

		ib::error()
			<< "The page " << bpage->id << " in file '"
			<< space->chain.start->name
			<< "' cannot be decrypted.";

		ib::info()
			<< "However key management plugin or used key_version "
			<< mach_read_from_4(dst_frame
					    + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION)
			<< " is not found or"
			" used encryption algorithm or method does not match.";

		if (bpage->id.space() != TRX_SYS_SPACE) {
			ib::info()
				<< "Marking tablespace as missing."
				" You may drop this table or"
				" install correct key management plugin"
				" and key file.";
		}
	}

	return (err);
}

/** Complete a read or write request of a file page to or from the buffer pool.
@param[in,out]	bpage	page to complete
@param[in]	dblwr	whether the doublewrite buffer was used (on write)
@param[in]	evict	whether or not to evict the page from LRU list
@return whether the operation succeeded
@retval	DB_SUCCESS		always when writing, or if a read page was OK
@retval	DB_TABLESPACE_DELETED	if the tablespace does not exist
@retval	DB_PAGE_CORRUPTED	if the checksum fails on a page read
@retval	DB_DECRYPTION_FAILED	if page post encryption checksum matches but
				after decryption normal page checksum does
				not match */
UNIV_INTERN
dberr_t
buf_page_io_complete(buf_page_t* bpage, bool dblwr, bool evict)
{
	enum buf_io_fix	io_type;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	const bool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);
	ut_a(buf_page_in_file(bpage));

	/* We do not need protect io_fix here by mutex to read
	it because this is the only function where we can change the value
	from BUF_IO_READ or BUF_IO_WRITE to some other value, and our code
	ensures that this is the only thread that handles the i/o for this
	block. */

	io_type = buf_page_get_io_fix(bpage);
	ut_ad(io_type == BUF_IO_READ || io_type == BUF_IO_WRITE);
	ut_ad(bpage->size.is_compressed() == (bpage->zip.data != NULL));
	ut_ad(uncompressed || bpage->zip.data);

	if (io_type == BUF_IO_READ) {
		ulint	read_page_no = 0;
		ulint	read_space_id = 0;
		byte*	frame = bpage->zip.data
			? bpage->zip.data
			: reinterpret_cast<buf_block_t*>(bpage)->frame;
		ut_ad(frame);
		fil_space_t* space = fil_space_acquire_for_io(
			bpage->id.space());
		if (!space) {
			return DB_TABLESPACE_DELETED;
		}

		dberr_t	err;

		if (!buf_page_decrypt_after_read(bpage, space)) {
			err = DB_DECRYPTION_FAILED;
			goto database_corrupted;
		}

		if (bpage->zip.data && uncompressed) {
			my_atomic_addlint(&buf_pool->n_pend_unzip, 1);
			ibool ok = buf_zip_decompress((buf_block_t*) bpage,
						      FALSE);
			my_atomic_addlint(&buf_pool->n_pend_unzip, -1);

			if (!ok) {
				ib::info() << "Page "
					   << bpage->id
					   << " zip_decompress failure.";

				err = DB_PAGE_CORRUPTED;
				goto database_corrupted;
			}
		}

		/* If this page is not uninitialized and not in the
		doublewrite buffer, then the page number and space id
		should be the same as in block. */
		read_page_no = mach_read_from_4(frame + FIL_PAGE_OFFSET);
		read_space_id = mach_read_from_4(
			frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

		if (bpage->id.space() == TRX_SYS_SPACE
		    && buf_dblwr_page_inside(bpage->id.page_no())) {

			ib::error() << "Reading page " << bpage->id
				<< ", which is in the doublewrite buffer!";

		} else if (read_space_id == 0 && read_page_no == 0) {
			/* This is likely an uninitialized page. */
		} else if ((bpage->id.space() != TRX_SYS_SPACE
			    && bpage->id.space() != read_space_id)
			   || bpage->id.page_no() != read_page_no) {
			/* We did not compare space_id to read_space_id
			in the system tablespace, because the field
			was written as garbage before MySQL 4.1.1,
			which did not support innodb_file_per_table. */

			ib::error() << "Space id and page no stored in "
				"the page, read in are "
				<< page_id_t(read_space_id, read_page_no)
				<< ", should be " << bpage->id;
		}

		err = buf_page_check_corrupt(bpage, space);

		if (err != DB_SUCCESS) {
database_corrupted:
			/* Not a real corruption if it was triggered by
			error injection */
			DBUG_EXECUTE_IF(
				"buf_page_import_corrupt_failure",
				if (!is_predefined_tablespace(
					    bpage->id.space())) {
					buf_corrupt_page_release(bpage, space);
					ib::info() << "Simulated IMPORT "
						"corruption";
					fil_space_release_for_io(space);
					return(err);
				}
				err = DB_SUCCESS;
				goto page_not_corrupt;
			);

			if (uncompressed && bpage->zip.data) {
				memset(reinterpret_cast<buf_block_t*>(bpage)
				       ->frame, 0, srv_page_size);
			}

			if (err == DB_PAGE_CORRUPTED) {
				ib::error()
					<< "Database page corruption on disk"
					" or a failed file read of tablespace "
					<< space->name << " page " << bpage->id
					<< ". You may have to recover from "
					<< "a backup.";

				buf_page_print(frame, bpage->size);

				ib::info()
					<< "It is also possible that your"
					" operating system has corrupted"
					" its own file cache and rebooting"
					" your computer removes the error."
					" If the corrupt page is an index page."
					" You can also try to fix the"
					" corruption by dumping, dropping,"
					" and reimporting the corrupt table."
					" You can use CHECK TABLE to scan"
					" your table for corruption. "
					<< FORCE_RECOVERY_MSG;
			}

			if (!srv_force_recovery) {

				/* If page space id is larger than TRX_SYS_SPACE
				(0), we will attempt to mark the corresponding
				table as corrupted instead of crashing server */
				if (bpage->id.space() == TRX_SYS_SPACE) {
					ib::fatal() << "Aborting because of"
						" a corrupt database page.";
				}

				buf_corrupt_page_release(bpage, space);
				fil_space_release_for_io(space);
				return(err);
			}
		}

		DBUG_EXECUTE_IF("buf_page_import_corrupt_failure",
				page_not_corrupt: bpage = bpage; );

		if (err == DB_PAGE_CORRUPTED
		    || err == DB_DECRYPTION_FAILED) {
			const page_id_t corrupt_page_id = bpage->id;

			buf_corrupt_page_release(bpage, space);

			if (recv_recovery_is_on()) {
				recv_recover_corrupt_page(corrupt_page_id);
			}

			fil_space_release_for_io(space);
			return err;
		}

		if (recv_recovery_is_on()) {
			recv_recover_page(bpage);
		}

		/* If space is being truncated then avoid ibuf operation.
		During re-init we have already freed ibuf entries. */
		if (uncompressed
		    && !recv_no_ibuf_operations
		    && (bpage->id.space() == 0
			|| !is_predefined_tablespace(bpage->id.space()))
		    && !srv_is_tablespace_truncated(bpage->id.space())
		    && fil_page_get_type(frame) == FIL_PAGE_INDEX
		    && page_is_leaf(frame)) {

			ibuf_merge_or_delete_for_page(
				(buf_block_t*) bpage, bpage->id,
				bpage->size);
		}

		fil_space_release_for_io(space);
	} else {
		/* io_type == BUF_IO_WRITE */
		if (bpage->slot) {
			/* Mark slot free */
			bpage->slot->release();
			bpage->slot = NULL;
		}
	}

	BPageMutex* block_mutex = buf_page_get_mutex(bpage);
	buf_pool_mutex_enter(buf_pool);
	mutex_enter(block_mutex);

	/* Because this thread which does the unlocking is not the same that
	did the locking, we use a pass value != 0 in unlock, which simply
	removes the newest lock debug record, without checking the thread
	id. */

	buf_page_set_io_fix(bpage, BUF_IO_NONE);
	buf_page_monitor(bpage, io_type);

	if (io_type == BUF_IO_READ) {
		/* NOTE that the call to ibuf may have moved the ownership of
		the x-latch to this OS thread: do not let this confuse you in
		debugging! */

		ut_ad(buf_pool->n_pend_reads > 0);
		buf_pool->n_pend_reads--;
		buf_pool->stat.n_pages_read++;

		if (uncompressed) {
			rw_lock_x_unlock_gen(&((buf_block_t*) bpage)->lock,
					     BUF_IO_READ);
		}

		mutex_exit(block_mutex);
	} else {
		/* Write means a flush operation: call the completion
		routine in the flush system */

		buf_flush_write_complete(bpage, dblwr);

		if (uncompressed) {
			rw_lock_sx_unlock_gen(&((buf_block_t*) bpage)->lock,
					      BUF_IO_WRITE);
		}

		buf_pool->stat.n_pages_written++;

		/* We decide whether or not to evict the page from the
		LRU list based on the flush_type.
		* BUF_FLUSH_LIST: don't evict
		* BUF_FLUSH_LRU: always evict
		* BUF_FLUSH_SINGLE_PAGE: eviction preference is passed
		by the caller explicitly. */
		if (buf_page_get_flush_type(bpage) == BUF_FLUSH_LRU) {
			evict = true;
		}

		mutex_exit(block_mutex);

		if (evict) {
			buf_LRU_free_page(bpage, true);
		}
	}

	DBUG_PRINT("ib_buf", ("%s page %u:%u",
			      io_type == BUF_IO_READ ? "read" : "wrote",
			      bpage->id.space(), bpage->id.page_no()));

	buf_pool_mutex_exit(buf_pool);

	return DB_SUCCESS;
}

/*********************************************************************//**
Asserts that all file pages in the buffer are in a replaceable state.
@return TRUE */
static
ibool
buf_all_freed_instance(
/*===================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instancce */
{
	ulint		i;
	buf_chunk_t*	chunk;

	ut_ad(buf_pool);

	buf_pool_mutex_enter(buf_pool);

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {

		if (const buf_block_t* block = buf_chunk_not_freed(chunk)) {
			ib::fatal() << "Page " << block->page.id
				<< " still fixed or dirty";
		}
	}

	buf_pool_mutex_exit(buf_pool);

	return(TRUE);
}

/** Refreshes the statistics used to print per-second averages.
@param[in,out]	buf_pool	buffer pool instance */
static
void
buf_refresh_io_stats(
	buf_pool_t*	buf_pool)
{
	buf_pool->last_printout_time = time(NULL);
	buf_pool->old_stat = buf_pool->stat;
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

	buf_pool_mutex_enter(buf_pool);

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

			buf_pool_mutex_exit(buf_pool);
			buf_flush_wait_batch_end(buf_pool, type);
			buf_pool_mutex_enter(buf_pool);
		}
	}

	buf_pool_mutex_exit(buf_pool);

	ut_ad(buf_all_freed_instance(buf_pool));

	buf_pool_mutex_enter(buf_pool);

	while (buf_LRU_scan_and_free_block(buf_pool, true)) {
	}

	ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == 0);
	ut_ad(UT_LIST_GET_LEN(buf_pool->unzip_LRU) == 0);

	buf_pool->freed_page_clock = 0;
	buf_pool->LRU_old = NULL;
	buf_pool->LRU_old_len = 0;

	memset(&buf_pool->stat, 0x00, sizeof(buf_pool->stat));
	buf_refresh_io_stats(buf_pool);

	buf_pool_mutex_exit(buf_pool);
}

/*********************************************************************//**
Invalidates the file pages in the buffer pool when an archive recovery is
completed. All the file pages buffered must be in a replaceable state when
this function is called: not latched and not modified. */
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
@return TRUE */
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

	ut_ad(buf_pool);

	buf_pool_mutex_enter(buf_pool);
	hash_lock_x_all(buf_pool->page_hash);

	chunk = buf_pool->chunks;

	/* Check the uncompressed blocks. */

	for (i = buf_pool->n_chunks; i--; chunk++) {

		ulint		j;
		buf_block_t*	block = chunk->blocks;

		for (j = chunk->size; j--; block++) {

			buf_page_mutex_enter(block);

			switch (buf_block_get_state(block)) {
			case BUF_BLOCK_POOL_WATCH:
			case BUF_BLOCK_ZIP_PAGE:
			case BUF_BLOCK_ZIP_DIRTY:
				/* These should only occur on
				zip_clean, zip_free[], or flush_list. */
				ut_error;
				break;

			case BUF_BLOCK_FILE_PAGE:
				ut_a(buf_page_hash_get_low(
						buf_pool, block->page.id)
				     == &block->page);

				switch (buf_page_get_io_fix(&block->page)) {
				case BUF_IO_NONE:
					break;

				case BUF_IO_WRITE:
					switch (buf_page_get_flush_type(
							&block->page)) {
					case BUF_FLUSH_LRU:
						n_lru_flush++;
						goto assert_s_latched;
					case BUF_FLUSH_SINGLE_PAGE:
						n_page_flush++;
assert_s_latched:
						ut_a(rw_lock_is_locked(
							     &block->lock,
								     RW_LOCK_S)
						     || rw_lock_is_locked(
								&block->lock,
								RW_LOCK_SX));
						break;
					case BUF_FLUSH_LIST:
						n_list_flush++;
						break;
					default:
						ut_error;
					}

					break;

				case BUF_IO_READ:

					ut_a(rw_lock_is_locked(&block->lock,
							       RW_LOCK_X));
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

			buf_page_mutex_exit(block);
		}
	}

	mutex_enter(&buf_pool->zip_mutex);

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
		ut_a(buf_page_hash_get_low(buf_pool, b->id) == b);
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
			switch (buf_page_get_io_fix(b)) {
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
			}
			break;
		case BUF_BLOCK_FILE_PAGE:
			/* uncompressed page */
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
		ut_a(buf_page_hash_get_low(buf_pool, b->id) == b);
	}

	ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == n_flush);

	hash_unlock_x_all(buf_pool->page_hash);
	buf_flush_list_mutex_exit(buf_pool);

	mutex_exit(&buf_pool->zip_mutex);

	if (buf_pool->curr_size == buf_pool->old_size
	    && n_lru + n_free > buf_pool->curr_size + n_zip) {

		ib::fatal() << "n_LRU " << n_lru << ", n_free " << n_free
			<< ", pool " << buf_pool->curr_size
			<< " zip " << n_zip << ". Aborting...";
	}

	ut_a(UT_LIST_GET_LEN(buf_pool->LRU) == n_lru);
	if (buf_pool->curr_size == buf_pool->old_size
	    && UT_LIST_GET_LEN(buf_pool->free) != n_free) {

		ib::fatal() << "Free list len "
			<< UT_LIST_GET_LEN(buf_pool->free)
			<< ", free blocks " << n_free << ". Aborting...";
	}

	ut_a(buf_pool->n_flush[BUF_FLUSH_LIST] == n_list_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_LRU] == n_lru_flush);
	ut_a(buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE] == n_page_flush);

	buf_pool_mutex_exit(buf_pool);

	ut_a(buf_LRU_validate());
	ut_a(buf_flush_validate(buf_pool));

	return(TRUE);
}

/*********************************************************************//**
Validates the buffer buf_pool data structure.
@return TRUE */
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

	index_ids = static_cast<index_id_t*>(
		ut_malloc_nokey(size * sizeof *index_ids));

	counts = static_cast<ulint*>(ut_malloc_nokey(sizeof(ulint) * size));

	buf_pool_mutex_enter(buf_pool);
	buf_flush_list_mutex_enter(buf_pool);

	ib::info() << *buf_pool;

	buf_flush_list_mutex_exit(buf_pool);

	/* Count the number of blocks belonging to each index in the buffer */

	n_found = 0;

	chunk = buf_pool->chunks;

	for (i = buf_pool->n_chunks; i--; chunk++) {
		buf_block_t*	block		= chunk->blocks;
		ulint		n_blocks	= chunk->size;

		for (; n_blocks--; block++) {
			const buf_frame_t* frame = block->frame;

			if (fil_page_index_page_check(frame)) {

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

	buf_pool_mutex_exit(buf_pool);

	for (i = 0; i < n_found; i++) {
		index = dict_index_get_if_in_cache(index_ids[i]);

		if (!index) {
			ib::info() << "Block count for index "
				<< index_ids[i] << " in buffer is about "
				<< counts[i];
		} else {
			ib::info() << "Block count for index " << index_ids[i]
				<< " in buffer is about " << counts[i]
				<< ", index " << index->name
				<< " of table " << index->table->name;
		}
	}

	ut_free(index_ids);
	ut_free(counts);

	ut_a(buf_pool_validate_instance(buf_pool));
}

/*********************************************************************//**
Prints info of the buffer buf_pool data structure. */
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
@return number of latched pages */
static
ulint
buf_get_latched_pages_number_instance(
/*==================================*/
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	buf_page_t*	b;
	ulint		i;
	buf_chunk_t*	chunk;
	ulint		fixed_pages_number = 0;

	buf_pool_mutex_enter(buf_pool);

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

			buf_page_mutex_enter(block);

			if (block->page.buf_fix_count != 0
			    || buf_page_get_io_fix(&block->page)
			    != BUF_IO_NONE) {
				fixed_pages_number++;
			}

			buf_page_mutex_exit(block);
		}
	}

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
	}

	buf_flush_list_mutex_exit(buf_pool);
	mutex_exit(&buf_pool->zip_mutex);
	buf_pool_mutex_exit(buf_pool);

	return(fixed_pages_number);
}

/*********************************************************************//**
Returns the number of latched pages in all the buffer pools.
@return number of latched pages */
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
@return number of pending read I/O operations */
ulint
buf_get_n_pending_read_ios(void)
/*============================*/
{
	ulint	pend_ios = 0;

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		pend_ios += buf_pool_from_array(i)->n_pend_reads;
	}

	return(pend_ios);
}

/*********************************************************************//**
Returns the ratio in percents of modified pages in the buffer pool /
database pages in the buffer pool.
@return modified page percentage ratio */
double
buf_get_modified_ratio_pct(void)
/*============================*/
{
	double		ratio;
	ulint		lru_len = 0;
	ulint		free_len = 0;
	ulint		flush_list_len = 0;

	buf_get_total_list_len(&lru_len, &free_len, &flush_list_len);

	ratio = static_cast<double>(100 * flush_list_len)
		/ (1 + lru_len + free_len);

	/* 1 + is there to avoid division by zero */

	return(ratio);
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
void
buf_stats_get_pool_info(
/*====================*/
	buf_pool_t*		buf_pool,	/*!< in: buffer pool */
	ulint			pool_id,	/*!< in: buffer pool ID */
	buf_pool_info_t*	all_pool_info)	/*!< in/out: buffer pool info
						to fill */
{
	buf_pool_info_t*	pool_info;
	time_t			current_time;
	double			time_elapsed;

	/* Find appropriate pool_info to store stats for this buffer pool */
	pool_info = &all_pool_info[pool_id];

	buf_pool_mutex_enter(buf_pool);
	buf_flush_list_mutex_enter(buf_pool);

	pool_info->pool_unique_id = pool_id;

	pool_info->pool_size = buf_pool->curr_size;

	pool_info->lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

	pool_info->old_lru_len = buf_pool->LRU_old_len;

	pool_info->free_list_len = UT_LIST_GET_LEN(buf_pool->free);

	pool_info->flush_list_len = UT_LIST_GET_LEN(buf_pool->flush_list);

	pool_info->n_pend_unzip = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	pool_info->n_pend_reads = buf_pool->n_pend_reads;

	pool_info->n_pending_flush_lru =
		 (buf_pool->n_flush[BUF_FLUSH_LRU]
		  + buf_pool->init_flush[BUF_FLUSH_LRU]);

	pool_info->n_pending_flush_list =
		 (buf_pool->n_flush[BUF_FLUSH_LIST]
		  + buf_pool->init_flush[BUF_FLUSH_LIST]);

	pool_info->n_pending_flush_single_page =
		 (buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]
		  + buf_pool->init_flush[BUF_FLUSH_SINGLE_PAGE]);

	buf_flush_list_mutex_exit(buf_pool);

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
	buf_pool_mutex_exit(buf_pool);
}

/*********************************************************************//**
Prints info of the buffer i/o. */
static
void
buf_print_io_instance(
/*==================*/
	buf_pool_info_t*pool_info,	/*!< in: buffer pool info */
	FILE*		file)		/*!< in/out: buffer where to print */
{
	ut_ad(pool_info);

	fprintf(file,
		"Buffer pool size   " ULINTPF "\n"
		"Free buffers       " ULINTPF "\n"
		"Database pages     " ULINTPF "\n"
		"Old database pages " ULINTPF "\n"
		"Modified db pages  " ULINTPF "\n"
		"Percent of dirty pages(LRU & free pages): %.3f\n"
		"Max dirty pages percent: %.3f\n"
		"Pending reads " ULINTPF "\n"
		"Pending writes: LRU " ULINTPF ", flush list " ULINTPF
		", single page " ULINTPF "\n",
		pool_info->pool_size,
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
		"Pages made young " ULINTPF ", not young " ULINTPF "\n"
		"%.2f youngs/s, %.2f non-youngs/s\n"
		"Pages read " ULINTPF ", created " ULINTPF
		", written " ULINTPF "\n"
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
		double hit_rate = double(pool_info->page_read_delta)
			/ pool_info->n_page_get_delta;

		if (hit_rate > 1) {
			hit_rate = 1;
		}

		fprintf(file,
			"Buffer pool hit rate " ULINTPF " / 1000,"
			" young-making rate " ULINTPF " / 1000 not "
			ULINTPF " / 1000\n",
			ulint(1000 * (1 - hit_rate)),
			ulint(1000 * double(pool_info->young_making_delta)
			      / pool_info->n_page_get_delta),
			ulint(1000 * double(pool_info->not_young_making_delta)
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
		"LRU len: " ULINTPF ", unzip_LRU len: " ULINTPF "\n"
		"I/O sum[" ULINTPF "]:cur[" ULINTPF "], "
		"unzip sum[" ULINTPF "]:cur[" ULINTPF "]\n",
		pool_info->lru_len, pool_info->unzip_lru_len,
		pool_info->io_sum, pool_info->io_cur,
		pool_info->unzip_sum, pool_info->unzip_cur);
}

/*********************************************************************//**
Prints info of the buffer i/o. */
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
		pool_info = (buf_pool_info_t*) ut_zalloc_nokey((
			srv_buf_pool_instances + 1) * sizeof *pool_info);

		pool_info_total = &pool_info[srv_buf_pool_instances];
	} else {
		ut_a(srv_buf_pool_instances == 1);

		pool_info_total = pool_info =
			static_cast<buf_pool_info_t*>(
				ut_zalloc_nokey(sizeof *pool_info));
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
			fprintf(file, "---BUFFER POOL " ULINTPF "\n", i);
			buf_print_io_instance(&pool_info[i], file);
		}
	}

	ut_free(pool_info);
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
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
@return number of pending i/o */
ulint
buf_pool_check_no_pending_io(void)
/*==============================*/
{
	ulint		i;
	ulint		pending_io = 0;

	buf_pool_mutex_enter_all();

	for (i = 0; i < srv_buf_pool_instances; i++) {
		const buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		pending_io += buf_pool->n_pend_reads
			      + buf_pool->n_flush[BUF_FLUSH_LRU]
			      + buf_pool->n_flush[BUF_FLUSH_SINGLE_PAGE]
			      + buf_pool->n_flush[BUF_FLUSH_LIST];

	}

	buf_pool_mutex_exit_all();

	return(pending_io);
}

/** Print the given page_id_t object.
@param[in,out]	out	the output stream
@param[in]	page_id	the page_id_t object to be printed
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		out,
	const page_id_t		page_id)
{
	out << "[page id: space=" << page_id.m_space
		<< ", page number=" << page_id.m_page_no << "]";
	return(out);
}

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Print the given buf_pool_t object.
@param[in,out]	out		the output stream
@param[in]	buf_pool	the buf_pool_t object to be printed
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		out,
	const buf_pool_t&	buf_pool)
{
	out << "[buffer pool instance: "
		<< "buf_pool size=" << buf_pool.curr_size
		<< ", database pages=" << UT_LIST_GET_LEN(buf_pool.LRU)
		<< ", free pages=" << UT_LIST_GET_LEN(buf_pool.free)
		<< ", modified database pages="
		<< UT_LIST_GET_LEN(buf_pool.flush_list)
		<< ", n pending decompressions=" << buf_pool.n_pend_unzip
		<< ", n pending reads=" << buf_pool.n_pend_reads
		<< ", n pending flush LRU=" << buf_pool.n_flush[BUF_FLUSH_LRU]
		<< " list=" << buf_pool.n_flush[BUF_FLUSH_LIST]
		<< " single page=" << buf_pool.n_flush[BUF_FLUSH_SINGLE_PAGE]
		<< ", pages made young=" << buf_pool.stat.n_pages_made_young
		<< ", not young=" << buf_pool.stat.n_pages_not_made_young
		<< ", pages read=" << buf_pool.stat.n_pages_read
		<< ", created=" << buf_pool.stat.n_pages_created
		<< ", written=" << buf_pool.stat.n_pages_written << "]";
	return(out);
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

/** Encrypt a buffer of temporary tablespace
@param[in]	offset		Page offset
@param[in]	src_frame	Page to encrypt
@param[in,out]	dst_frame	Output buffer
@return encrypted buffer or NULL */
static byte* buf_tmp_page_encrypt(
	ulint	offset,
	byte*	src_frame,
	byte*	dst_frame)
{
	uint header_len = FIL_PAGE_DATA;
	/* FIL page header is not encrypted */
	memcpy(dst_frame, src_frame, header_len);

	/* Calculate the start offset in a page */
	uint unencrypted_bytes = header_len + FIL_PAGE_DATA_END;
	uint srclen = srv_page_size - unencrypted_bytes;
	const byte* src = src_frame + header_len;
	byte* dst = dst_frame + header_len;

	if (!log_tmp_block_encrypt(src, srclen, dst, (offset * srv_page_size),
				   true)) {
		return NULL;
	}

	memcpy(dst_frame + srv_page_size - FIL_PAGE_DATA_END,
	       src_frame + srv_page_size - FIL_PAGE_DATA_END,
	       FIL_PAGE_DATA_END);

	/* Handle post encryption checksum */
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4,
			buf_calc_page_crc32(dst_frame));

	srv_stats.pages_encrypted.inc();
	srv_stats.n_temp_blocks_encrypted.inc();
	return dst_frame;
}

/** Encryption and page_compression hook that is called just before
a page is written to disk.
@param[in,out]	space		tablespace
@param[in,out]	bpage		buffer page
@param[in]	src_frame	physical page frame that is being encrypted
@return	page frame to be written to file
(may be src_frame or an encrypted/compressed copy of it) */
UNIV_INTERN
byte*
buf_page_encrypt_before_write(
	fil_space_t*	space,
	buf_page_t*	bpage,
	byte*		src_frame)
{
	ut_ad(space->id == bpage->id.space());
	bpage->real_size = UNIV_PAGE_SIZE;

	fil_page_type_validate(src_frame);

	switch (bpage->id.page_no()) {
	case 0:
		/* Page 0 of a tablespace is not encrypted/compressed */
		return src_frame;
	case TRX_SYS_PAGE_NO:
		if (bpage->id.space() == TRX_SYS_SPACE) {
			/* don't encrypt/compress page as it contains
			address to dblwr buffer */
			return src_frame;
		}
	}

	fil_space_crypt_t* crypt_data = space->crypt_data;

	bool encrypted, page_compressed;

	if (space->purpose == FIL_TYPE_TEMPORARY) {
		ut_ad(!crypt_data);
		encrypted = innodb_encrypt_temporary_tables;
		page_compressed = false;
	} else {
		encrypted = crypt_data
			&& !crypt_data->not_encrypted()
			&& crypt_data->type != CRYPT_SCHEME_UNENCRYPTED
			&& (!crypt_data->is_default_encryption()
			    || srv_encrypt_tables);

		page_compressed = FSP_FLAGS_HAS_PAGE_COMPRESSION(space->flags);
	}

	if (!encrypted && !page_compressed) {
		/* No need to encrypt or page compress the page.
		Clear key-version & crypt-checksum. */
		memset(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
		return src_frame;
	}

	ut_ad(!bpage->size.is_compressed() || !page_compressed);
	buf_pool_t* buf_pool = buf_pool_from_bpage(bpage);
	/* Find free slot from temporary memory array */
	buf_tmp_buffer_t* slot = buf_pool_reserve_tmp_slot(buf_pool);
	slot->out_buf = NULL;
	bpage->slot = slot;

	buf_tmp_reserve_crypt_buf(slot);
	byte *dst_frame = slot->crypt_buf;

	if (!page_compressed) {
not_compressed:
		byte* tmp;
		if (space->purpose == FIL_TYPE_TEMPORARY) {
			/* Encrypt temporary tablespace page content */
			tmp = buf_tmp_page_encrypt(bpage->id.page_no(),
						   src_frame, dst_frame);
		} else {
			/* Encrypt page content */
			tmp = fil_space_encrypt(
					space, bpage->id.page_no(),
					bpage->newest_modification,
					src_frame, dst_frame);
		}

		bpage->real_size = UNIV_PAGE_SIZE;
		slot->out_buf = dst_frame = tmp;

		ut_d(fil_page_type_validate(tmp));
	} else {
		ut_ad(space->purpose != FIL_TYPE_TEMPORARY);
		/* First we compress the page content */
		buf_tmp_reserve_compression_buf(slot);
		byte* tmp = slot->comp_buf;
		ulint out_len = fil_page_compress(
			src_frame, tmp,
			fsp_flags_get_page_compression_level(space->flags),
			fil_space_get_block_size(space, bpage->id.page_no()),
			encrypted);
		if (!out_len) {
			goto not_compressed;
		}

		bpage->real_size = out_len;

		/* Workaround for MDEV-15527. */
		memset(tmp + out_len, 0 , srv_page_size - out_len);
		ut_d(fil_page_type_validate(tmp));

		if (encrypted) {
			/* And then we encrypt the page content */
			tmp = fil_space_encrypt(space,
						bpage->id.page_no(),
						bpage->newest_modification,
						tmp,
						dst_frame);
		}

		slot->out_buf = dst_frame = tmp;
	}

	ut_d(fil_page_type_validate(dst_frame));

	// return dst_frame which will be written
	return dst_frame;
}

/**
Should we punch hole to deallocate unused portion of the page.
@param[in]	bpage		Page control block
@return true if punch hole should be used, false if not */
bool
buf_page_should_punch_hole(
	const buf_page_t* bpage)
{
	return (bpage->real_size != bpage->size.physical());
}

/**
Calculate the length of trim (punch_hole) operation.
@param[in]	bpage		Page control block
@param[in]	write_length	Write length
@return length of the trim or zero. */
ulint
buf_page_get_trim_length(
	const buf_page_t*	bpage,
	ulint			write_length)
{
	return (bpage->size.physical() - write_length);
}
#endif /* !UNIV_INNOCHECKSUM */
