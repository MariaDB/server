/*****************************************************************************

Copyright (c) 1995, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2013, 2020, MariaDB Corporation.

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

#include "assume_aligned.h"
#include "mtr0types.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "ut0crc32.h"
#include <string.h>

#ifndef UNIV_INNOCHECKSUM
#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "buf0buddy.h"
#include "buf0dblwr.h"
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
#include "fil0pagecompress.h"
#include "fsp0pagecompress.h"
#endif /* !UNIV_INNOCHECKSUM */
#include "page0zip.h"
#include "sync0sync.h"
#include "buf0dump.h"
#include <map>
#include <sstream>

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

/*
		IMPLEMENTATION OF THE BUFFER POOL
		=================================

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
These locks can be locked and unlocked without owning the buf_pool.mutex.
The OS events in the buf_pool struct can be waited for without owning the
buf_pool.mutex.

The buf_pool.mutex is a hot-spot in main memory, causing a lot of
memory bus traffic on multiprocessor systems when processors
alternately access the mutex. On our Pentium, the mutex is accessed
maybe every 10 microseconds. We gave up the solution to have mutexes
for each control block, for instance, because it seemed to be
complicated.

A solution to reduce mutex contention of the buf_pool.mutex is to
create a separate mutex for the page hash table. On Pentium,
accessing the hash table takes 2 microseconds, about half
of the total buf_pool.mutex hold time.

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

The free list (buf_pool.free) contains blocks which are currently not
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

The chain of modified blocks (buf_pool.flush_list) contains the blocks
holding file pages that have been modified in the memory
but not written to disk yet. The block with the oldest modification
which has not yet been written to disk is at the end of the chain.
The access to this list is protected by buf_pool.flush_list_mutex.

The chain of unmodified compressed blocks (buf_pool.zip_clean)
contains the control blocks (buf_page_t) of those compressed pages
that are not in buf_pool.flush_list and for which no uncompressed
page has been allocated in the buffer pool.  The control blocks for
uncompressed pages are accessible via buf_block_t objects that are
reachable via buf_pool.chunks[].

The chains of free memory blocks (buf_pool.zip_free[]) are used by
the buddy allocator (buf0buddy.cc) to keep track of currently unused
memory blocks of size sizeof(buf_page_t)..srv_page_size / 2.  These
blocks are inside the srv_page_size-sized memory blocks of type
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

/** The InnoDB buffer pool */
buf_pool_t buf_pool;
buf_pool_t::chunk_t::map *buf_pool_t::chunk_t::map_reg;
buf_pool_t::chunk_t::map *buf_pool_t::chunk_t::map_ref;

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
	uint header_len = FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;

	/* Copy FIL page header, it is not encrypted */
	memcpy(tmp_frame, src_frame, header_len);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + header_len;
	byte* dst = tmp_frame + header_len;
	uint srclen = uint(srv_page_size)
		- (header_len + FIL_PAGE_FCRC32_CHECKSUM);
	ulint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);

	if (!log_tmp_block_decrypt(src, srclen, dst,
				   (offset * srv_page_size))) {
		return false;
	}

	static_assert(FIL_PAGE_FCRC32_CHECKSUM == 4, "alignment");
	memcpy_aligned<4>(tmp_frame + srv_page_size - FIL_PAGE_FCRC32_CHECKSUM,
			  src_frame + srv_page_size - FIL_PAGE_FCRC32_CHECKSUM,
			  FIL_PAGE_FCRC32_CHECKSUM);

	memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(src_frame, tmp_frame,
					       srv_page_size);
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
	ut_ad(space->pending_io());
	ut_ad(space->id == bpage->id.space());

	byte* dst_frame = bpage->zip.data ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	bool page_compressed = space->is_compressed()
		&& buf_page_is_compressed(dst_frame, space->flags);

	if (bpage->id.page_no() == 0) {
		/* File header pages are not encrypted/compressed */
		return (true);
	}

	if (space->purpose == FIL_TYPE_TEMPORARY
	    && innodb_encrypt_temporary_tables) {
		buf_tmp_buffer_t* slot = buf_pool.io_buf_reserve();
		ut_a(slot);
		slot->allocate();

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
	uint key_version = buf_page_get_key_version(dst_frame, space->flags);

	if (page_compressed && !key_version) {
		/* the page we read is unencrypted */
		/* Find free slot from temporary memory array */
decompress:
		if (space->full_crc32()
		    && buf_page_is_corrupted(true, dst_frame, space->flags)) {
			return false;
		}

		slot = buf_pool.io_buf_reserve();
		ut_a(slot);
		slot->allocate();

decompress_with_slot:
		ut_d(fil_page_type_validate(space, dst_frame));

		bpage->write_size = fil_page_decompress(
			slot->crypt_buf, dst_frame, space->flags);
		slot->release();

		ut_ad(!bpage->write_size
		      || fil_page_type_validate(space, dst_frame));

		ut_ad(space->pending_io());

		return bpage->write_size != 0;
	}

	if (key_version && space->crypt_data) {
		/* Verify encryption checksum before we even try to
		decrypt. */
		if (!buf_page_verify_crypt_checksum(dst_frame, space->flags)) {
decrypt_failed:
			ib::error() << "Encrypted page " << bpage->id
				    << " in file " << space->chain.start->name
				    << " looks corrupted; key_version="
				    << key_version;
			return false;
		}

		slot = buf_pool.io_buf_reserve();
		ut_a(slot);
		slot->allocate();
		ut_d(fil_page_type_validate(space, dst_frame));

		/* decrypt using crypt_buf to dst_frame */
		if (!fil_space_decrypt(space, slot->crypt_buf, dst_frame)) {
			slot->release();
			goto decrypt_failed;
		}

		ut_d(fil_page_type_validate(space, dst_frame));

		if ((space->full_crc32() && page_compressed)
		    || fil_page_is_compressed_encrypted(dst_frame)) {
			goto decompress_with_slot;
		}

		slot->release();
	} else if (fil_page_is_compressed_encrypted(dst_frame)) {
		goto decompress;
	}

	ut_ad(space->pending_io());
	return true;
}

/**
@return the smallest oldest_modification lsn for any page.
@retval 0	if all modified persistent pages have been flushed */
lsn_t
buf_pool_get_oldest_modification()
{
	mutex_enter(&buf_pool.flush_list_mutex);

	buf_page_t*	bpage;

	/* FIXME: Keep temporary tablespace pages in a separate flush
	list. We would only need to write out temporary pages if the
	page is about to be evicted from the buffer pool, and the page
	contents is still needed (the page has not been freed). */
	for (bpage = UT_LIST_GET_LAST(buf_pool.flush_list);
	     bpage != NULL && fsp_is_system_temporary(bpage->id.space());
	     bpage = UT_LIST_GET_PREV(list, bpage)) {
		ut_ad(bpage->in_flush_list);
	}

	lsn_t oldest_lsn = bpage ? bpage->oldest_modification : 0;
	mutex_exit(&buf_pool.flush_list_mutex);

	/* The returned answer may be out of date: the flush_list can
	change after the mutex has been released. */

	return(oldest_lsn);
}

/** Allocate a buffer block.
@return own: the allocated block, in state BUF_BLOCK_MEMORY */
buf_block_t*
buf_block_alloc()
{
	buf_block_t* block = buf_LRU_get_free_block();
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
		fprintf(log_file, "page::%llu;"
			" crc32 calculated = %u;"
			" recorded checksum field1 = " ULINTPF " recorded"
			" checksum field2 =" ULINTPF "\n", cur_page_num,
			crc32, checksum_field1, checksum_field2);
	}
#endif /* UNIV_INNOCHECKSUM */

	if (checksum_field1 != checksum_field2) {
		return false;
	}

	return checksum_field1 == crc32;
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
		fprintf(log_file, "page::%llu;"
			" old style: calculated ="
			" " ULINTPF "; recorded = " ULINTPF "\n",
			cur_page_num, old_checksum,
			checksum_field2);
		fprintf(log_file, "page::%llu;"
			" new style: calculated ="
			" " ULINTPF "; crc32 = %u; recorded = " ULINTPF "\n",
			cur_page_num, new_checksum,
			buf_calc_page_crc32(read_buf), checksum_field1);
	}

	if (log_file
	    && srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB) {
		fprintf(log_file, "page::%llu;"
			" old style: calculated ="
			" " ULINTPF "; recorded checksum = " ULINTPF "\n",
			cur_page_num, old_checksum,
			checksum_field2);
		fprintf(log_file, "page::%llu;"
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
			"page::%llu; none checksum: calculated"
			" = %lu; recorded checksum_field1 = " ULINTPF
			" recorded checksum_field2 = " ULINTPF "\n",
			cur_page_num, BUF_NO_CHECKSUM_MAGIC,
			checksum_field1, checksum_field2);
	}
#endif /* UNIV_INNOCHECKSUM */

	return(checksum_field1 == checksum_field2
	       && checksum_field1 == BUF_NO_CHECKSUM_MAGIC);
}

/** Checks whether the lsn present in the page is lesser than the
peek current lsn.
@param[in]	check_lsn	lsn to check
@param[in]	read_buf	page. */
static void buf_page_check_lsn(bool check_lsn, const byte* read_buf)
{
#ifndef UNIV_INNOCHECKSUM
	if (check_lsn && recv_lsn_checks_on) {
		const lsn_t current_lsn = log_sys.get_lsn();
		const lsn_t	page_lsn
			= mach_read_from_8(read_buf + FIL_PAGE_LSN);

		/* Since we are going to reset the page LSN during the import
		phase it makes no sense to spam the log with error messages. */
		if (current_lsn < page_lsn) {

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
}


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
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	space		tablespace
@return whether the page is corrupted */
bool
buf_page_is_corrupted(
	bool			check_lsn,
	const byte*		read_buf,
	ulint			fsp_flags)
{
#ifndef UNIV_INNOCHECKSUM
	DBUG_EXECUTE_IF("buf_page_import_corrupt_failure", return(true); );
#endif
	if (fil_space_t::full_crc32(fsp_flags)) {
		bool compressed = false, corrupted = false;
		const uint size = buf_page_full_crc32_size(
			read_buf, &compressed, &corrupted);
		if (corrupted) {
			return true;
		}
		const byte* end = read_buf + (size - FIL_PAGE_FCRC32_CHECKSUM);
		uint crc32 = mach_read_from_4(end);

		if (!crc32 && size == srv_page_size
		    && buf_is_zeroes(span<const byte>(read_buf, size))) {
			return false;
		}

		DBUG_EXECUTE_IF(
			"page_intermittent_checksum_mismatch", {
			static int page_counter;
			if (page_counter++ == 2) {
				crc32++;
			}
		});

		if (crc32 != ut_crc32(read_buf,
				      size - FIL_PAGE_FCRC32_CHECKSUM)) {
			return true;
		}
		static_assert(FIL_PAGE_FCRC32_KEY_VERSION == 0, "alignment");
		static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
		static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
		if (!compressed
		    && !mach_read_from_4(FIL_PAGE_FCRC32_KEY_VERSION
					 + read_buf)
		    && memcmp_aligned<4>(read_buf + (FIL_PAGE_LSN + 4),
					 end - (FIL_PAGE_FCRC32_END_LSN
						- FIL_PAGE_FCRC32_CHECKSUM),
					 4)) {
			return true;
		}

		buf_page_check_lsn(check_lsn, read_buf);
		return false;
	}

	size_t		checksum_field1 = 0;
	size_t		checksum_field2 = 0;
	uint32_t	crc32 = 0;
	bool		crc32_inited = false;
	bool		crc32_chksum = false;
	const ulint zip_size = fil_space_t::zip_size(fsp_flags);
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
	    && FSP_FLAGS_HAS_PAGE_COMPRESSION(fsp_flags)
#endif
	) {
		return(false);
	}

	static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
	static_assert(FIL_PAGE_END_LSN_OLD_CHKSUM % 4 == 0, "alignment");

	if (!zip_size
	    && memcmp_aligned<4>(read_buf + FIL_PAGE_LSN + 4,
				 read_buf + srv_page_size
				 - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4)) {
		/* Stored log sequence numbers at the start and the end
		of page do not match */

		return(true);
	}

	buf_page_check_lsn(check_lsn, read_buf);

	/* Check whether the checksum fields have correct values */

	const srv_checksum_algorithm_t curr_algo =
		static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);

	if (curr_algo == SRV_CHECKSUM_ALGORITHM_NONE) {
		return(false);
	}

	if (zip_size) {
		return !page_zip_verify_checksum(read_buf, zip_size);
	}

	checksum_field1 = mach_read_from_4(
		read_buf + FIL_PAGE_SPACE_OR_CHKSUM);

	checksum_field2 = mach_read_from_4(
		read_buf + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM);

	static_assert(FIL_PAGE_LSN % 8 == 0, "alignment");

	/* A page filled with NUL bytes is considered not corrupted.
	Before MariaDB Server 10.1.25 (MDEV-12113) or 10.2.2 (or MySQL 5.7),
	the FIL_PAGE_FILE_FLUSH_LSN field may have been written nonzero
	for the first page of each file of the system tablespace.
	We want to ignore it for the system tablespace, but because
	we do not know the expected tablespace here, we ignore the
	field for all data files, except for
	innodb_checksum_algorithm=full_crc32 which we handled above. */
	if (!checksum_field1 && !checksum_field2) {
		/* Checksum fields can have valid value as zero.
		If the page is not empty then do the checksum
		calculation for the page. */
		bool all_zeroes = true;
		for (size_t i = 0; i < srv_page_size; i++) {
#ifndef UNIV_INNOCHECKSUM
			if (i == FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION) {
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
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		return !buf_page_is_checksum_valid_crc32(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		return !buf_page_is_checksum_valid_innodb(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		return !buf_page_is_checksum_valid_none(
			read_buf, checksum_field1, checksum_field2);
	case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_INNODB:
		if (buf_page_is_checksum_valid_none(read_buf,
			checksum_field1, checksum_field2)) {
#ifdef UNIV_INNOCHECKSUM
			if (log_file) {
				fprintf(log_file, "page::%llu;"
					" old style: calculated = %u;"
					" recorded = " ULINTPF ";\n",
					cur_page_num,
					buf_calc_page_old_checksum(read_buf),
					checksum_field2);
				fprintf(log_file, "page::%llu;"
					" new style: calculated = %u;"
					" crc32 = %u; recorded = " ULINTPF ";\n",
					cur_page_num,
					buf_calc_page_new_checksum(read_buf),
					buf_calc_page_crc32(read_buf),
					checksum_field1);
			}
#endif /* UNIV_INNOCHECKSUM */
			return false;
		}

		crc32_chksum = curr_algo == SRV_CHECKSUM_ALGORITHM_CRC32
			|| curr_algo == SRV_CHECKSUM_ALGORITHM_FULL_CRC32;

		/* Very old versions of InnoDB only stored 8 byte lsn to the
		start and the end of the page. */

		/* Since innodb_checksum_algorithm is not strict_* allow
		any of the algos to match for the old field */

		if (checksum_field2
		    != mach_read_from_4(read_buf + FIL_PAGE_LSN)
		    && checksum_field2 != BUF_NO_CHECKSUM_MAGIC) {

			if (crc32_chksum) {
				crc32 = buf_calc_page_crc32(read_buf);
				crc32_inited = true;

				DBUG_EXECUTE_IF(
					"page_intermittent_checksum_mismatch", {
					static int page_counter;
					if (page_counter++ == 2) {
						crc32++;
					}
				});

				if (checksum_field2 != crc32
				    && checksum_field2
				       != buf_calc_page_old_checksum(read_buf)) {
					return true;
				}
			} else {
				ut_ad(curr_algo
				      == SRV_CHECKSUM_ALGORITHM_INNODB);

				if (checksum_field2
				    != buf_calc_page_old_checksum(read_buf)) {
					crc32 = buf_calc_page_crc32(read_buf);
					crc32_inited = true;

					if (checksum_field2 != crc32) {
						return true;
					}
				}
			}
		}

		if (checksum_field1 == 0
		    || checksum_field1 == BUF_NO_CHECKSUM_MAGIC) {
		} else if (crc32_chksum) {

			if (!crc32_inited) {
				crc32 = buf_calc_page_crc32(read_buf);
				crc32_inited = true;
			}

			if (checksum_field1 != crc32
			    && checksum_field1
			    != buf_calc_page_new_checksum(read_buf)) {
				return true;
			}
		} else {
			ut_ad(curr_algo == SRV_CHECKSUM_ALGORITHM_INNODB);

			if (checksum_field1
			    != buf_calc_page_new_checksum(read_buf)) {

				if (!crc32_inited) {
					crc32 = buf_calc_page_crc32(read_buf);
					crc32_inited = true;
				}

				if (checksum_field1 != crc32) {
					return true;
				}
			}
		}

		if (crc32_inited
		    && ((checksum_field1 == crc32
			 && checksum_field2 != crc32)
			|| (checksum_field1 != crc32
			    && checksum_field2 == crc32))) {
			return true;
		}

		break;
	case SRV_CHECKSUM_ALGORITHM_NONE:
		/* should have returned false earlier */
		break;
	}

	return false;
}

#ifndef UNIV_INNOCHECKSUM

#if defined(DBUG_OFF) && defined(HAVE_MADVISE) &&  defined(MADV_DODUMP)
/** Enable buffers to be dumped to core files

A convience function, not called anyhwere directly however
it is left available for gdb or any debugger to call
in the event that you want all of the memory to be dumped
to a core file.

Returns number of errors found in madvise calls. */
int
buf_madvise_do_dump()
{
	int ret= 0;

	/* mirrors allocation in log_t::create() */
	if (log_sys.buf) {
		ret+= madvise(log_sys.first_in_use
			      ? log_sys.buf
			      : log_sys.buf - srv_log_buffer_size,
			      srv_log_buffer_size * 2,
			      MADV_DODUMP);
	}
	/* mirrors recv_sys_t::create() */
	if (recv_sys.buf)
	{
		ret+= madvise(recv_sys.buf, recv_sys.len, MADV_DODUMP);
	}

	mutex_enter(&buf_pool.mutex);
	auto chunk = buf_pool.chunks;

	for (ulint n = buf_pool.n_chunks; n--; chunk++) {
		ret+= madvise(chunk->mem, chunk->mem_size(), MADV_DODUMP);
	}

	mutex_exit(&buf_pool.mutex);
	return ret;
}
#endif

/** Dump a page to stderr.
@param[in]	read_buf	database page
@param[in]	zip_size	compressed page size, or 0 */
void buf_page_print(const byte* read_buf, ulint zip_size)
{
	dict_index_t*	index;

#ifndef UNIV_DEBUG
	const ulint size = zip_size ? zip_size : srv_page_size;
	ib::info() << "Page dump in ascii and hex ("
		<< size << " bytes):";

	ut_print_buf(stderr, read_buf, size);
	fputs("\nInnoDB: End of page dump\n", stderr);
#endif

	if (zip_size) {
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
				read_buf, zip_size,
				SRV_CHECKSUM_ALGORITHM_CRC32)
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_INNODB)
			<< " "
			<< page_zip_calc_checksum(
				read_buf, zip_size,
				SRV_CHECKSUM_ALGORITHM_INNODB)
			<< ", "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_NONE)
			<< " "
			<< page_zip_calc_checksum(
				read_buf, zip_size,
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
		ulint page_type = fil_page_get_type(read_buf);

		ib::info() << "Uncompressed page, stored checksum in field1 "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_SPACE_OR_CHKSUM)
			<< ", calculated checksums for field1: "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32) << " "
			<< crc32
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
			<< mach_read_from_4(read_buf + srv_page_size
					    - FIL_PAGE_END_LSN_OLD_CHKSUM)
			<< ", calculated checksums for field2: "
			<< buf_checksum_algorithm_name(
				SRV_CHECKSUM_ALGORITHM_CRC32) << " "
			<< crc32
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
			<< mach_read_from_4(read_buf + srv_page_size
					    - FIL_PAGE_END_LSN_OLD_CHKSUM + 4)
			<< ", page number (if stored to page already) "
			<< mach_read_from_4(read_buf + FIL_PAGE_OFFSET)
			<< ", space id (if created with >= MySQL-4.1.1"
			   " and stored already) "
			<< mach_read_from_4(
				read_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	}

	switch (fil_page_get_type(read_buf)) {
		index_id_t	index_id;
	case FIL_PAGE_INDEX:
	case FIL_PAGE_TYPE_INSTANT:
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
	case FIL_PAGE_UNDO_LOG:
		fputs("InnoDB: Page may be an undo log page\n", stderr);
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
	buf_pool_t::chunk_t*	chunk)		/*!< in/out: chunk of buffers */
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
		rwlock = block->debug_latch;
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

/** Initialize a buffer page descriptor.
@param[in,out]	block	buffer page descriptor
@param[in]	frame	buffer page frame */
static
void
buf_block_init(buf_block_t* block, byte* frame)
{
	UNIV_MEM_DESC(frame, srv_page_size);

	/* This function should only be executed at database startup or by
	buf_pool.resize(). Either way, adaptive hash index must not exist. */
	assert_block_ahi_empty_on_init(block);

	block->frame = frame;

	block->page.flush_type = BUF_FLUSH_LRU;
	block->page.state = BUF_BLOCK_NOT_USED;
	block->page.buf_fix_count = 0;
	block->page.io_fix = BUF_IO_NONE;
	block->page.real_size = 0;
	block->page.write_size = 0;
	block->modify_clock = 0;
	block->page.slot = NULL;
	block->page.status = buf_page_t::NORMAL;

#ifdef BTR_CUR_HASH_ADAPT
	block->index = NULL;
#endif /* BTR_CUR_HASH_ADAPT */
	block->skip_flush_check = false;

	ut_d(block->page.in_page_hash = FALSE);
	ut_d(block->page.in_zip_hash = FALSE);
	ut_d(block->page.in_flush_list = FALSE);
	ut_d(block->page.in_free_list = FALSE);
	ut_d(block->page.in_LRU_list = FALSE);
	ut_d(block->in_unzip_LRU_list = FALSE);
	ut_d(block->in_withdraw_list = FALSE);

	page_zip_des_init(&block->page.zip);

	mutex_create(LATCH_ID_BUF_BLOCK_MUTEX, &block->mutex);
	ut_d(block->debug_latch = (rw_lock_t *) ut_malloc_nokey(sizeof(rw_lock_t)));

#if defined PFS_SKIP_BUFFER_MUTEX_RWLOCK || defined PFS_GROUP_BUFFER_SYNC
	/* If PFS_SKIP_BUFFER_MUTEX_RWLOCK is defined, skip registration
	of buffer block rwlock with performance schema.

	If PFS_GROUP_BUFFER_SYNC is defined, skip the registration
	since buffer block rwlock will be registered later in
	pfs_register_buffer_block(). */

	rw_lock_create(PFS_NOT_INSTRUMENTED, &block->lock, SYNC_LEVEL_VARYING);

	ut_d(rw_lock_create(PFS_NOT_INSTRUMENTED, block->debug_latch,
			    SYNC_LEVEL_VARYING));

#else /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */

	rw_lock_create(buf_block_lock_key, &block->lock, SYNC_LEVEL_VARYING);

	ut_d(rw_lock_create(buf_block_debug_latch_key,
			    block->debug_latch, SYNC_LEVEL_VARYING));

#endif /* PFS_SKIP_BUFFER_MUTEX_RWLOCK || PFS_GROUP_BUFFER_SYNC */

	block->lock.is_block_lock = 1;

	ut_ad(rw_lock_validate(&(block->lock)));
}

/** Allocate a chunk of buffer frames.
@param bytes    requested size
@return whether the allocation succeeded */
inline bool buf_pool_t::chunk_t::create(size_t bytes)
{
  DBUG_EXECUTE_IF("ib_buf_chunk_init_fails", return false;);
  /* Round down to a multiple of page size, although it already should be. */
  bytes= ut_2pow_round<size_t>(bytes, srv_page_size);

  mem= buf_pool.allocator.allocate_large_dontdump(bytes, &mem_pfx);

  if (UNIV_UNLIKELY(!mem))
    return false;

#ifdef HAVE_LIBNUMA
  if (srv_numa_interleave)
  {
    struct bitmask *numa_mems_allowed= numa_get_mems_allowed();
    if (mbind(mem, mem_size(), MPOL_INTERLEAVE,
              numa_mems_allowed->maskp, numa_mems_allowed->size,
              MPOL_MF_MOVE))
    {
      ib::warn() << "Failed to set NUMA memory policy of"
              " buffer pool page frames to MPOL_INTERLEAVE"
              " (error: " << strerror(errno) << ").";
    }
    numa_bitmask_free(numa_mems_allowed);
  }
#endif /* HAVE_LIBNUMA */


  /* Allocate the block descriptors from
  the start of the memory block. */
  blocks= reinterpret_cast<buf_block_t*>(mem);

  /* Align a pointer to the first frame.  Note that when
  opt_large_page_size is smaller than srv_page_size,
  (with max srv_page_size at 64k don't think any hardware
  makes this true),
  we may allocate one fewer block than requested.  When
  it is bigger, we may allocate more blocks than requested. */
  static_assert(sizeof(byte*) == sizeof(ulint), "pointer size");

  byte *frame= reinterpret_cast<byte*>((reinterpret_cast<ulint>(mem) +
                                        srv_page_size - 1) &
                                       ~ulint{srv_page_size - 1});
  size= (mem_pfx.m_size >> srv_page_size_shift) - (frame != mem);

  /* Subtract the space needed for block descriptors. */
  {
    ulint s= size;

    while (frame < reinterpret_cast<const byte*>(blocks + s))
    {
      frame+= srv_page_size;
      s--;
    }

    size= s;
  }

  /* Init block structs and assign frames for them. Then we assign the
  frames to the first blocks (we already mapped the memory above). */

  buf_block_t *block= blocks;

  for (auto i= size; i--; ) {
    buf_block_init(block, frame);
    UNIV_MEM_INVALID(block->frame, srv_page_size);
    /* Add the block to the free list */
    UT_LIST_ADD_LAST(buf_pool.free, &block->page);

    ut_d(block->page.in_free_list = TRUE);
    block++;
    frame+= srv_page_size;
  }

  reg();

#ifdef PFS_GROUP_BUFFER_SYNC
  pfs_register_buffer_block(this);
#endif /* PFS_GROUP_BUFFER_SYNC */
  return true;
}

#ifdef UNIV_DEBUG
/** Check that all file pages in the buffer chunk are in a replaceable state.
@return address of a non-free block
@retval nullptr if all freed */
inline const buf_block_t *buf_pool_t::chunk_t::not_freed() const
{
  buf_block_t *block= blocks;
  for (auto i= size; i--; block++)
  {
    switch (buf_block_get_state(block)) {
    case BUF_BLOCK_POOL_WATCH:
    case BUF_BLOCK_ZIP_PAGE:
    case BUF_BLOCK_ZIP_DIRTY:
      /* The uncompressed buffer pool should never
      contain ROW_FORMAT=COMPRESSED block descriptors. */
      ut_error;
      break;
    case BUF_BLOCK_NOT_USED:
    case BUF_BLOCK_READY_FOR_USE:
    case BUF_BLOCK_MEMORY:
    case BUF_BLOCK_REMOVE_HASH:
      /* Skip blocks that are not being used for file pages. */
      break;
    case BUF_BLOCK_FILE_PAGE:
      if (srv_read_only_mode)
      {
        /* The page cleaner is disabled in read-only mode.  No pages
        can be dirtied, so all of them must be clean. */
        ut_ad(block->page.oldest_modification == 0 ||
              block->page.oldest_modification == recv_sys.recovered_lsn ||
              srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
        ut_ad(block->page.buf_fix_count == 0);
        ut_ad(block->page.io_fix == BUF_IO_NONE);
        break;
      }

      buf_page_mutex_enter(block);
      auto ready= buf_flush_ready_for_replace(&block->page);
      buf_page_mutex_exit(block);

      if (!ready)
        return block;

      break;
    }
  }

  return nullptr;
}
#endif /* UNIV_DEBUG */

/** Free the synchronization objects of a buffer pool block descriptor
@param[in,out]	block	buffer pool block descriptor */
static void buf_block_free_mutexes(buf_block_t* block)
{
	mutex_free(&block->mutex);
	rw_lock_free(&block->lock);
	ut_d(rw_lock_free(block->debug_latch));
	ut_d(ut_free(block->debug_latch));
}

/** Create the buffer pool.
@return whether the creation failed */
bool buf_pool_t::create()
{
  ut_ad(this == &buf_pool);
  ut_ad(srv_buf_pool_size % srv_buf_pool_chunk_unit == 0);
  ut_ad(!is_initialised());
  ut_ad(srv_buf_pool_size > 0);

  NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;

  ut_ad(!resizing);
  ut_ad(!withdrawing);
  ut_ad(!withdraw_clock());
  ut_ad(!chunks_old);

  chunk_t::map_reg= UT_NEW_NOKEY(chunk_t::map());

  new(&allocator) ut_allocator<unsigned char>(mem_key_buf_buf_pool);

  n_chunks= srv_buf_pool_size / srv_buf_pool_chunk_unit;
  const size_t chunk_size= srv_buf_pool_chunk_unit;

  chunks= static_cast<chunk_t*>(ut_zalloc_nokey(n_chunks * sizeof *chunks));
  UT_LIST_INIT(free, &buf_page_t::list);
  curr_size= 0;
  auto chunk= chunks;

  do
  {
    if (!chunk->create(chunk_size))
    {
      while (--chunk >= chunks)
      {
        buf_block_t* block= chunk->blocks;

          for (auto i= chunk->size; i--; block++)
          buf_block_free_mutexes(block);

          allocator.deallocate_large_dodump(chunk->mem, &chunk->mem_pfx);
        }
        ut_free(chunks);
        chunks= nullptr;
        UT_DELETE(chunk_t::map_reg);
        chunk_t::map_reg= nullptr;
        ut_ad(!is_initialised());
        return true;
    }

    curr_size+= chunk->size;
  }
  while (++chunk < chunks + n_chunks);

  ut_ad(is_initialised());
  mutex_create(LATCH_ID_BUF_POOL, &mutex);
  mutex_create(LATCH_ID_BUF_POOL_ZIP, &zip_mutex);

  UT_LIST_INIT(LRU, &buf_page_t::LRU);
  UT_LIST_INIT(withdraw, &buf_page_t::list);
  withdraw_target= 0;
  UT_LIST_INIT(flush_list, &buf_page_t::list);
  UT_LIST_INIT(unzip_LRU, &buf_block_t::unzip_LRU);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  UT_LIST_INIT(zip_clean, &buf_page_t::list);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

  for (size_t i= 0; i < UT_ARR_SIZE(zip_free); ++i)
    UT_LIST_INIT(zip_free[i], &buf_buddy_free_t::list);

  read_ahead_area= ut_min(BUF_READ_AHEAD_PAGES,
                          ut_2_power_up(curr_size / BUF_READ_AHEAD_PORTION));
  curr_pool_size= srv_buf_pool_size;

  old_size= curr_size;
  n_chunks_new= n_chunks;

  /* Number of locks protecting page_hash must be a power of two */
  srv_n_page_hash_locks= static_cast<ulong>
    (ut_2_power_up(srv_n_page_hash_locks));
  ut_a(srv_n_page_hash_locks != 0);
  ut_a(srv_n_page_hash_locks <= MAX_PAGE_HASH_LOCKS);

  page_hash= ib_create(2 * curr_size,
                         LATCH_ID_HASH_TABLE_RW_LOCK,
                         srv_n_page_hash_locks, MEM_HEAP_FOR_PAGE_HASH);

  ut_ad(!page_hash_old);
  zip_hash= hash_create(2 * curr_size);
  last_printout_time= time(NULL);

  mutex_create(LATCH_ID_FLUSH_LIST, &flush_list_mutex);

  for (int i= BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++)
    no_flush[i]= os_event_create(0);

  watch= static_cast<buf_page_t*>
    (ut_zalloc_nokey(sizeof *watch * BUF_POOL_WATCH_SIZE));

  try_LRU_scan= true;

  ut_d(flush_hp.m_mutex= &flush_list_mutex;);
  ut_d(lru_hp.m_mutex= &mutex);
  ut_d(lru_scan_itr.m_mutex= &mutex);
  ut_d(single_scan_itr.m_mutex= &mutex);

  io_buf.create((srv_n_read_io_threads + srv_n_write_io_threads) *
                OS_AIO_N_PENDING_IOS_PER_THREAD);

  /* FIXME: remove some of these variables */
  srv_buf_pool_curr_size= curr_pool_size;
  srv_buf_pool_old_size= srv_buf_pool_size;
  srv_buf_pool_base_size= srv_buf_pool_size;

  chunk_t::map_ref= chunk_t::map_reg;
  buf_LRU_old_ratio_update(100 * 3 / 8, false);
  btr_search_sys_create(srv_buf_pool_curr_size / sizeof(void*) / 64);
  ut_ad(is_initialised());
  return false;
}

/** Clean up after successful create() */
void buf_pool_t::close()
{
  ut_ad(this == &buf_pool);
  if (!is_initialised())
    return;

  mutex_free(&mutex);
  mutex_free(&zip_mutex);
  mutex_free(&flush_list_mutex);

  if (flush_rbt)
  {
    rbt_free(flush_rbt);
    flush_rbt= nullptr;
  }

  for (buf_page_t *bpage= UT_LIST_GET_LAST(LRU), *prev_bpage= nullptr; bpage;
       bpage= prev_bpage)
  {
    prev_bpage= UT_LIST_GET_PREV(LRU, bpage);
    buf_page_state state= buf_page_get_state(bpage);

    ut_ad(buf_page_in_file(bpage));
    ut_ad(bpage->in_LRU_list);

    if (state != BUF_BLOCK_FILE_PAGE)
    {
      /* We must not have any dirty block except during a fast shutdown. */
      ut_ad(state == BUF_BLOCK_ZIP_PAGE || srv_fast_shutdown == 2);
      buf_page_free_descriptor(bpage);
    }
  }

  ut_free(watch);
  watch= nullptr;

  for (auto chunk= chunks + n_chunks; --chunk >= chunks; )
  {
    buf_block_t *block= chunk->blocks;

    for (auto i= chunk->size; i--; block++)
      buf_block_free_mutexes(block);

    allocator.deallocate_large_dodump(chunk->mem, &chunk->mem_pfx);
  }

  for (ulint i= BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; ++i)
    os_event_destroy(no_flush[i]);

  ut_free(chunks);
  chunks= nullptr;
  ha_clear(page_hash);
  hash_table_free(page_hash);
  hash_table_free(zip_hash);

  io_buf.close();
  UT_DELETE(chunk_t::map_reg);
  chunk_t::map_reg= chunk_t::map_ref= nullptr;
}

/** Try to reallocate a control block.
@param block  control block to reallocate
@return whether the reallocation succeeded */
inline bool buf_pool_t::realloc(buf_block_t *block)
{
	buf_block_t*	new_block;

	ut_ad(withdrawing);
	ut_ad(mutex_own(&mutex));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	new_block = buf_LRU_get_free_only();

	if (new_block == NULL) {
		return(false); /* free list was not enough */
	}

	rw_lock_t* hash_lock = buf_page_hash_lock_get(block->page.id);
	rw_lock_x_lock(hash_lock);
	mutex_enter(&block->mutex);

	if (buf_page_can_relocate(&block->page)) {
		mutex_enter(&new_block->mutex);

		memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(
			new_block->frame, block->frame, srv_page_size);
		new (&new_block->page) buf_page_t(block->page);

		/* relocate LRU list */
		ut_ad(block->page.in_LRU_list);
		ut_ad(!block->page.in_zip_hash);
		ut_d(block->page.in_LRU_list = FALSE);

		buf_LRU_adjust_hp(&block->page);

		buf_page_t*	prev_b = UT_LIST_GET_PREV(LRU, &block->page);
		UT_LIST_REMOVE(LRU, &block->page);

		if (prev_b != NULL) {
			UT_LIST_INSERT_AFTER(LRU, prev_b, &new_block->page);
		} else {
			UT_LIST_ADD_FIRST(LRU, &new_block->page);
		}

		if (LRU_old == &block->page) {
			LRU_old = &new_block->page;
		}

		ut_ad(new_block->page.in_LRU_list);

		/* relocate unzip_LRU list */
		if (block->page.zip.data != NULL) {
			ut_ad(block->in_unzip_LRU_list);
			ut_d(new_block->in_unzip_LRU_list = TRUE);
			UNIV_MEM_DESC(&new_block->page.zip.data,
				      page_zip_get_size(&new_block->page.zip));

			buf_block_t*	prev_block = UT_LIST_GET_PREV(unzip_LRU, block);
			UT_LIST_REMOVE(unzip_LRU, block);

			ut_d(block->in_unzip_LRU_list = FALSE);
			block->page.zip.data = NULL;
			page_zip_set_size(&block->page.zip, 0);

			if (prev_block != NULL) {
				UT_LIST_INSERT_AFTER(unzip_LRU, prev_block, new_block);
			} else {
				UT_LIST_ADD_FIRST(unzip_LRU, new_block);
			}
		} else {
			ut_ad(!block->in_unzip_LRU_list);
			ut_d(new_block->in_unzip_LRU_list = FALSE);
		}

		/* relocate page_hash */
		ut_ad(block->page.in_page_hash);
		ut_ad(&block->page == buf_page_hash_get_low(block->page.id));
		ut_d(block->page.in_page_hash = FALSE);
		ulint	fold = block->page.id.fold();
		ut_ad(fold == new_block->page.id.fold());
		HASH_REPLACE(buf_page_t, hash, page_hash, fold,
			     &block->page, &new_block->page);

		ut_ad(new_block->page.in_page_hash);

		buf_block_modify_clock_inc(block);
		static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
		memset_aligned<4>(block->frame + FIL_PAGE_OFFSET, 0xff, 4);
		static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
			      "not perfect alignment");
		memset_aligned<2>(block->frame
				  + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		UNIV_MEM_INVALID(block->frame, srv_page_size);
		buf_block_set_state(block, BUF_BLOCK_REMOVE_HASH);
		block->page.id
		    = page_id_t(ULINT32_UNDEFINED, ULINT32_UNDEFINED);

		/* Relocate flush_list. */
		if (block->page.oldest_modification) {
			buf_flush_relocate_on_flush_list(
				&block->page, &new_block->page);
		}

		/* set other flags of buf_block_t */

#ifdef BTR_CUR_HASH_ADAPT
		/* This code should only be executed by resize(),
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

/** Withdraw blocks from the buffer pool until meeting withdraw_target.
@return whether retry is needed */
inline bool buf_pool_t::withdraw_blocks()
{
	buf_block_t*	block;
	ulint		loop_count = 0;

	ib::info() << "start to withdraw the last "
		<< withdraw_target << " blocks";

	/* Minimize zip_free[i] lists */
	mutex_enter(&mutex);
	buf_buddy_condense_free();
	mutex_exit(&mutex);

	while (UT_LIST_GET_LEN(withdraw) < withdraw_target) {

		/* try to withdraw from free_list */
		ulint	count1 = 0;

		mutex_enter(&mutex);
		block = reinterpret_cast<buf_block_t*>(
			UT_LIST_GET_FIRST(free));
		while (block != NULL
		       && UT_LIST_GET_LEN(withdraw) < withdraw_target) {
			ut_ad(block->page.in_free_list);
			ut_ad(!block->page.in_flush_list);
			ut_ad(!block->page.in_LRU_list);
			ut_a(!buf_page_in_file(&block->page));

			buf_block_t*	next_block;
			next_block = reinterpret_cast<buf_block_t*>(
				UT_LIST_GET_NEXT(
					list, &block->page));

			if (buf_pool.will_be_withdrawn(block->page)) {
				/* This should be withdrawn */
				UT_LIST_REMOVE(free, &block->page);
				UT_LIST_ADD_LAST(withdraw, &block->page);
				ut_d(block->in_withdraw_list = TRUE);
				count1++;
			}

			block = next_block;
		}
		mutex_exit(&mutex);

		/* reserve free_list length */
		if (UT_LIST_GET_LEN(withdraw) < withdraw_target) {
			ulint	scan_depth;
			flush_counters_t n;

			/* cap scan_depth with current LRU size. */
			mutex_enter(&mutex);
			scan_depth = UT_LIST_GET_LEN(LRU);
			mutex_exit(&mutex);

			scan_depth = ut_min(
				ut_max(withdraw_target
				       - UT_LIST_GET_LEN(withdraw),
				       static_cast<ulint>(srv_LRU_scan_depth)),
				scan_depth);

			buf_flush_do_batch(BUF_FLUSH_LRU, scan_depth, 0, &n);
			buf_flush_wait_batch_end(BUF_FLUSH_LRU);

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

		mutex_enter(&mutex);
		buf_page_t*	bpage;
		bpage = UT_LIST_GET_FIRST(LRU);
		while (bpage != NULL) {
			BPageMutex*	block_mutex;
			buf_page_t*	next_bpage;

			block_mutex = buf_page_get_mutex(bpage);
			mutex_enter(block_mutex);

			next_bpage = UT_LIST_GET_NEXT(LRU, bpage);

			if (bpage->zip.data != NULL
			    && will_be_withdrawn(bpage->zip.data)
			    && buf_page_can_relocate(bpage)) {
				mutex_exit(block_mutex);
				buf_pool_mutex_exit_forbid();
				if (!buf_buddy_realloc(
					    bpage->zip.data,
					    page_zip_get_size(&bpage->zip))) {
					/* failed to allocate block */
					buf_pool_mutex_exit_allow();
					break;
				}
				buf_pool_mutex_exit_allow();
				mutex_enter(block_mutex);
				count2++;
			}

			if (buf_page_get_state(bpage)
			    == BUF_BLOCK_FILE_PAGE
			    && buf_pool.will_be_withdrawn(*bpage)) {
				if (buf_page_can_relocate(bpage)) {
					mutex_exit(block_mutex);
					buf_pool_mutex_exit_forbid();
					if (!realloc(
						reinterpret_cast<buf_block_t*>(
							bpage))) {
						/* failed to allocate block */
						buf_pool_mutex_exit_allow();
						break;
					}
					buf_pool_mutex_exit_allow();
					count2++;
				} else {
					mutex_exit(block_mutex);
				}
				/* NOTE: if the page is in use,
				not relocated yet */
			} else {
				mutex_exit(block_mutex);
			}

			bpage = next_bpage;
		}
		mutex_exit(&mutex);

		buf_resize_status(
			"withdrawing blocks. (" ULINTPF "/" ULINTPF ")",
			UT_LIST_GET_LEN(withdraw),
			withdraw_target);

		ib::info() << "withdrew "
			<< count1 << " blocks from free list."
			<< " Tried to relocate " << count2 << " pages ("
			<< UT_LIST_GET_LEN(withdraw) << "/"
			<< withdraw_target << ")";

		if (++loop_count >= 10) {
			/* give up for now.
			retried after user threads paused. */

			ib::info() << "will retry to withdraw later";

			/* need retry later */
			return(true);
		}
	}

	/* confirm withdrawn enough */
	for (const chunk_t* chunk = chunks + n_chunks_new,
	     * const echunk = chunks + n_chunks; chunk != echunk; chunk++) {
		block = chunk->blocks;
		for (ulint j = chunk->size; j--; block++) {
			ut_a(buf_block_get_state(block) == BUF_BLOCK_NOT_USED);
			ut_ad(block->in_withdraw_list);
		}
	}

	ib::info() << "withdrawn target: " << UT_LIST_GET_LEN(withdraw)
		   << " blocks";

	/* retry is not needed */
	++withdraw_clock_;

	return(false);
}

/** resize page_hash and zip_hash */
static void buf_pool_resize_hash()
{
	hash_table_t*	new_hash_table;

	ut_ad(buf_pool.page_hash_old == NULL);

	/* recreate page_hash */
	new_hash_table = ib_recreate(
		buf_pool.page_hash, 2 * buf_pool.curr_size);

	for (ulint i = 0; i < hash_get_n_cells(buf_pool.page_hash); i++) {
		buf_page_t*	bpage;

		bpage = static_cast<buf_page_t*>(
			HASH_GET_FIRST(
				buf_pool.page_hash, i));

		while (bpage) {
			buf_page_t*	prev_bpage = bpage;
			ulint		fold;

			bpage = static_cast<buf_page_t*>(
				HASH_GET_NEXT(
					hash, prev_bpage));

			fold = prev_bpage->id.fold();

			HASH_DELETE(buf_page_t, hash,
				buf_pool.page_hash, fold,
				prev_bpage);

			HASH_INSERT(buf_page_t, hash,
				new_hash_table, fold,
				prev_bpage);
		}
	}

	buf_pool.page_hash_old = buf_pool.page_hash;
	buf_pool.page_hash = new_hash_table;

	/* recreate zip_hash */
	new_hash_table = hash_create(2 * buf_pool.curr_size);

	for (ulint i = 0; i < hash_get_n_cells(buf_pool.zip_hash); i++) {
		buf_page_t*	bpage;

		bpage = static_cast<buf_page_t*>(
			HASH_GET_FIRST(buf_pool.zip_hash, i));

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
				buf_pool.zip_hash, fold,
				prev_bpage);

			HASH_INSERT(buf_page_t, hash,
				new_hash_table, fold,
				prev_bpage);
		}
	}

	hash_table_free(buf_pool.zip_hash);
	buf_pool.zip_hash = new_hash_table;
}

/** Resize from srv_buf_pool_old_size to srv_buf_pool_size. */
inline void buf_pool_t::resize()
{
  ut_ad(this == &buf_pool);

	bool		warning = false;

	NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;

	ut_ad(!resize_in_progress());
	ut_ad(srv_buf_pool_chunk_unit > 0);

	ulint new_instance_size = srv_buf_pool_size >> srv_page_size_shift;

	buf_resize_status("Resizing buffer pool from " ULINTPF " to "
			  ULINTPF " (unit=" ULINTPF ").",
			  srv_buf_pool_old_size, srv_buf_pool_size,
			  srv_buf_pool_chunk_unit);

	mutex_enter(&mutex);
	ut_ad(curr_size == old_size);
	ut_ad(n_chunks_new == n_chunks);
	ut_ad(UT_LIST_GET_LEN(withdraw) == 0);
	ut_ad(flush_rbt == NULL);

	n_chunks_new = (new_instance_size << srv_page_size_shift)
		/ srv_buf_pool_chunk_unit;
	curr_size = n_chunks_new * chunks->size;
	mutex_exit(&mutex);

#ifdef BTR_CUR_HASH_ADAPT
	/* disable AHI if needed */
	const bool btr_search_disabled = btr_search_enabled;

	buf_resize_status("Disabling adaptive hash index.");

	btr_search_s_lock_all();
	if (btr_search_disabled) {
		btr_search_s_unlock_all();
	} else {
		btr_search_s_unlock_all();
	}

	btr_search_disable(true);

	if (btr_search_disabled) {
		ib::info() << "disabled adaptive hash index.";
	}
#endif /* BTR_CUR_HASH_ADAPT */

	if (curr_size < old_size) {
		/* set withdraw target */
		size_t w = 0;

		for (const chunk_t* chunk = chunks + n_chunks_new,
		     * const echunk = chunks + n_chunks;
		     chunk != echunk; chunk++)
			w += chunk->size;

		ut_ad(withdraw_target == 0);
		withdraw_target = w;
		withdrawing.store(true, std::memory_order_relaxed);
	}

	buf_resize_status("Withdrawing blocks to be shrunken.");

	time_t		withdraw_started = time(NULL);
	double		message_interval = 60;
	ulint		retry_interval = 1;

withdraw_retry:
	/* wait for the number of blocks fit to the new size (if needed)*/
	bool	should_retry_withdraw = curr_size < old_size
		&& withdraw_blocks();

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		/* abort to resize for shutdown. */
		withdrawing.store(false, std::memory_order_relaxed);
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
		mutex_enter(&trx_sys.mutex);
		bool	found = false;
		for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys.trx_list);
		     trx != NULL;
		     trx = UT_LIST_GET_NEXT(trx_list, trx)) {
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
		mutex_exit(&trx_sys.mutex);
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

	withdrawing.store(false, std::memory_order_relaxed);

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
	resizing.store(true, std::memory_order_relaxed);

	mutex_enter(&mutex);
	hash_lock_x_all(page_hash);
	chunk_t::map_reg = UT_NEW_NOKEY(chunk_t::map());

	/* add/delete chunks */

	buf_resize_status("buffer pool resizing with chunks "
			  ULINTPF " to " ULINTPF ".",
			  n_chunks, n_chunks_new);

	if (n_chunks_new < n_chunks) {
		/* delete chunks */
		chunk_t* chunk = chunks + n_chunks_new;
		const chunk_t* const echunk = chunks + n_chunks;

		ulint	sum_freed = 0;

		while (chunk < echunk) {
			buf_block_t*	block = chunk->blocks;

			for (ulint j = chunk->size; j--; block++) {
				buf_block_free_mutexes(block);
			}

			allocator.deallocate_large_dodump(
				chunk->mem, &chunk->mem_pfx);
			sum_freed += chunk->size;
			++chunk;
		}

		/* discard withdraw list */
		UT_LIST_INIT(withdraw, &buf_page_t::list);
		withdraw_target = 0;

		ib::info() << n_chunks - n_chunks_new
			   << " chunks (" << sum_freed
			   << " blocks) were freed.";

		n_chunks = n_chunks_new;
	}

	{
		/* reallocate chunks */
		const size_t	new_chunks_size
			= n_chunks_new * sizeof(chunk_t);

		chunk_t*	new_chunks = static_cast<chunk_t*>(
			ut_zalloc_nokey_nofatal(new_chunks_size));

		DBUG_EXECUTE_IF("buf_pool_resize_chunk_null",
				ut_free(new_chunks); new_chunks= nullptr; );

		if (!new_chunks) {
			ib::error() << "failed to allocate"
				" the chunk array.";
			n_chunks_new = n_chunks;
			warning = true;
			chunks_old = NULL;
			goto calc_buf_pool_size;
		}

		ulint	n_chunks_copy = ut_min(n_chunks_new,
					       n_chunks);

		memcpy(new_chunks, chunks,
		       n_chunks_copy * sizeof *new_chunks);

		for (ulint j = 0; j < n_chunks_copy; j++) {
			new_chunks[j].reg();
		}

		chunks_old = chunks;
		chunks = new_chunks;
	}

	if (n_chunks_new > n_chunks) {
		/* add chunks */
		ulint	sum_added = 0;
		ulint	n = n_chunks;
		const size_t unit = srv_buf_pool_chunk_unit;

		for (chunk_t* chunk = chunks + n_chunks,
		     * const echunk = chunks + n_chunks_new;
		     chunk != echunk; chunk++) {
			if (!chunk->create(unit)) {
				ib::error() << "failed to allocate"
					" memory for buffer pool chunk";

				warning = true;
				n_chunks_new = n_chunks;
				break;
			}

			sum_added += chunk->size;
			++n;
		}

		ib::info() << n_chunks_new - n_chunks
			   << " chunks (" << sum_added
			   << " blocks) were added.";

		n_chunks = n;
	}
calc_buf_pool_size:
	/* recalc curr_size */
	ulint	new_size = 0;

	{
		chunk_t* chunk = chunks;
		const chunk_t* const echunk = chunk + n_chunks;
		do {
			new_size += chunk->size;
		} while (++chunk != echunk);
	}

	curr_size = new_size;
	n_chunks_new = n_chunks;

	if (chunks_old) {
		ut_free(chunks_old);
		chunks_old = NULL;
	}

	chunk_t::map* chunk_map_old = chunk_t::map_ref;
	chunk_t::map_ref = chunk_t::map_reg;

	/* set size */
	ut_ad(UT_LIST_GET_LEN(withdraw) == 0);
	read_ahead_area = ut_min(
		BUF_READ_AHEAD_PAGES,
		ut_2_power_up(curr_size / BUF_READ_AHEAD_PORTION));
	curr_pool_size = n_chunks * srv_buf_pool_chunk_unit;
	srv_buf_pool_curr_size = curr_pool_size;/* FIXME: remove*/
	old_size = curr_size;
	innodb_set_buf_pool_size(buf_pool_size_align(srv_buf_pool_curr_size));

	const bool	new_size_too_diff
		= srv_buf_pool_base_size > srv_buf_pool_size * 2
			|| srv_buf_pool_base_size * 2 < srv_buf_pool_size;

	/* Normalize page_hash and zip_hash,
	if the new size is too different */
	if (!warning && new_size_too_diff) {
		buf_resize_status("Resizing hash table");
		buf_pool_resize_hash();
		ib::info() << "hash tables were resized";
	}

	hash_unlock_x_all(page_hash);
	mutex_exit(&mutex);

	if (page_hash_old != NULL) {
		hash_table_free(page_hash_old);
		page_hash_old = NULL;
	}

	UT_DELETE(chunk_map_old);

	resizing.store(false, std::memory_order_relaxed);

	/* Normalize other components, if the new size is too different */
	if (!warning && new_size_too_diff) {
		srv_buf_pool_base_size = srv_buf_pool_size;

		buf_resize_status("Resizing also other hash tables.");

		/* normalize lock_sys */
		srv_lock_table_size = 5
			* (srv_buf_pool_size >> srv_page_size_shift);
		lock_sys.resize(srv_lock_table_size);

		/* normalize btr_search_sys */
		btr_search_sys_resize(
			buf_pool_get_curr_size() / sizeof(void*) / 64);

		dict_sys.resize();

		ib::info() << "Resized hash tables at lock_sys,"
#ifdef BTR_CUR_HASH_ADAPT
			" adaptive hash index,"
#endif /* BTR_CUR_HASH_ADAPT */
			" dictionary.";
	}

	/* normalize ibuf.max_size */
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
		btr_search_enable();
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
	validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	return;
}

/** Thread pool task invoked by innodb_buffer_pool_size changes. */
static void buf_resize_callback(void *)
{
  DBUG_ENTER("buf_resize_callback");
  ut_a(srv_shutdown_state == SRV_SHUTDOWN_NONE);
  mutex_enter(&buf_pool.mutex);
  const auto size= srv_buf_pool_size;
  const bool work= srv_buf_pool_old_size != size;
  mutex_exit(&buf_pool.mutex);

  if (work)
    buf_pool.resize();
  else
  {
    std::ostringstream sout;
    sout << "Size did not change: old size = new size = " << size;
    buf_resize_status(sout.str().c_str());
  }
  DBUG_VOID_RETURN;
}

/* Ensure that task does not run in parallel, by setting max_concurrency to 1 for the thread group */
static tpool::task_group single_threaded_group(1);
static tpool::waitable_task buf_resize_task(buf_resize_callback,
	nullptr, &single_threaded_group);

void buf_resize_start()
{
	srv_thread_pool->submit_task(&buf_resize_task);
}

void buf_resize_shutdown()
{
	buf_resize_task.wait();
}


/** Relocate a ROW_FORMAT=COMPRESSED block in the LRU list and
buf_pool.page_hash.
The caller must relocate bpage->list.
@param bpage   control block in BUF_BLOCK_ZIP_DIRTY or BUF_BLOCK_ZIP_PAGE
@param dpage   destination control block */
static void buf_relocate(buf_page_t *bpage, buf_page_t *dpage)
{
	buf_page_t*	b;

	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(buf_page_hash_lock_held_x(bpage));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_a(buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	ut_a(bpage->buf_fix_count == 0);
	ut_ad(bpage->in_LRU_list);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage == buf_page_hash_get_low(bpage->id));
	ut_ad(!buf_pool_watch_is_sentinel(bpage));
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
	buf_LRU_adjust_hp(bpage);

	ut_d(bpage->in_LRU_list = FALSE);
	ut_d(bpage->in_page_hash = FALSE);

	/* relocate buf_pool.LRU */
	b = UT_LIST_GET_PREV(LRU, bpage);
	UT_LIST_REMOVE(buf_pool.LRU, bpage);

	if (b != NULL) {
		UT_LIST_INSERT_AFTER(buf_pool.LRU, b, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.LRU, dpage);
	}

	if (UNIV_UNLIKELY(buf_pool.LRU_old == bpage)) {
		buf_pool.LRU_old = dpage;
#ifdef UNIV_LRU_DEBUG
		/* buf_pool.LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool.LRU_old->old);
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->old);
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->old);
	} else {
		/* Check that the "old" flag is consistent in
		the block and its neighbours. */
		buf_page_set_old(dpage, buf_page_is_old(dpage));
#endif /* UNIV_LRU_DEBUG */
	}

        ut_d(CheckInLRUList::validate());

	/* relocate buf_pool.page_hash */
	ulint	fold = bpage->id.fold();
	ut_ad(fold == dpage->id.fold());
	HASH_REPLACE(buf_page_t, hash, buf_pool.page_hash, fold, bpage,
		     dpage);
}

/** Determine if a block is a sentinel for a buffer pool watch.
@param[in]	bpage		block
@return whether bpage a sentinel for a buffer pool watch */
bool buf_pool_watch_is_sentinel(const buf_page_t* bpage)
{
	/* We must own the appropriate hash lock. */
	ut_ad(buf_page_hash_lock_held_s_or_x(bpage));
	ut_ad(buf_page_in_file(bpage));

	if (bpage < &buf_pool.watch[0]
	    || bpage >= &buf_pool.watch[BUF_POOL_WATCH_SIZE]) {

		ut_ad(buf_page_get_state(bpage) != BUF_BLOCK_ZIP_PAGE
		      || bpage->zip.data != NULL);

		return false;
	}

	ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_ZIP_PAGE);
	ut_ad(!bpage->in_zip_hash);
	ut_ad(bpage->in_page_hash);
	ut_ad(bpage->zip.data == NULL);
	return true;
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

	ut_ad(*hash_lock == buf_page_hash_lock_get(page_id));

	ut_ad(rw_lock_own(*hash_lock, RW_LOCK_X));

	bpage = buf_page_hash_get_low(page_id);

	if (bpage != NULL) {
page_found:
		if (!buf_pool_watch_is_sentinel(bpage)) {
			/* The page was loaded meanwhile. */
			return(bpage);
		}

		/* Add to an existing watch. */
		bpage->fix();
		return(NULL);
	}

	/* From this point this function becomes fairly heavy in terms
	of latching. We acquire the buf_pool mutex as well as all the
	hash_locks. buf_pool mutex is needed because any changes to
	the page_hash must be covered by it and hash_locks are needed
	because we don't want to read any stale information in
	buf_pool.watch[]. However, it is not in the critical code path
	as this function will be called only by the purge thread. */

	/* To obey latching order first release the hash_lock. */
	rw_lock_x_unlock(*hash_lock);

	mutex_enter(&buf_pool.mutex);
	hash_lock_x_all(buf_pool.page_hash);

	/* We have to recheck that the page
	was not loaded or a watch set by some other
	purge thread. This is because of the small
	time window between when we release the
	hash_lock to acquire buf_pool.mutex above. */

	*hash_lock = buf_page_hash_lock_get(page_id);

	bpage = buf_page_hash_get_low(page_id);
	if (UNIV_LIKELY_NULL(bpage)) {
		mutex_exit(&buf_pool.mutex);
		hash_unlock_x_all_but(buf_pool.page_hash, *hash_lock);
		goto page_found;
	}

	/* The maximum number of purge threads should never exceed
	BUF_POOL_WATCH_SIZE. So there is no way for a purge task
	to hold a watch when setting another watch. */
	for (i = 0; i < BUF_POOL_WATCH_SIZE; i++) {
		bpage = &buf_pool.watch[i];

		ut_ad(bpage->access_time == 0);
		ut_ad(bpage->oldest_modification == 0);
		ut_ad(bpage->zip.data == NULL);
		ut_ad(!bpage->in_zip_hash);

		switch (bpage->state) {
		case BUF_BLOCK_POOL_WATCH:
			ut_ad(!bpage->in_page_hash);
			ut_ad(bpage->buf_fix_count == 0);

			/* bpage is pointing to buf_pool.watch[],
			which is protected by buf_pool.mutex.
			Normally, buf_page_t objects are protected by
			buf_block_t::mutex or buf_pool.zip_mutex or both. */

			bpage->state = BUF_BLOCK_ZIP_PAGE;
			bpage->id = page_id;
			bpage->buf_fix_count = 1;

			ut_d(bpage->in_page_hash = TRUE);
			HASH_INSERT(buf_page_t, hash, buf_pool.page_hash,
				    page_id.fold(), bpage);

			mutex_exit(&buf_pool.mutex);
			/* Once the sentinel is in the page_hash we can
			safely release all locks except just the
			relevant hash_lock */
			hash_unlock_x_all_but(buf_pool.page_hash,
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
real block. buf_pool_watch_unset() or buf_pool_watch_occurred() will notice
that the block has been replaced with the real block.
@param[in,out]	watch		sentinel for watch
@return reference count, to be added to the replacement block */
static void buf_pool_watch_remove(buf_page_t *watch)
{
  ut_ad(rw_lock_own(buf_page_hash_lock_get(watch->id), RW_LOCK_X));
  ut_ad(mutex_own(&buf_pool.mutex));

  ut_ad(watch->in_page_hash);
  ut_d(watch->in_page_hash= FALSE);
  HASH_DELETE(buf_page_t, hash, buf_pool.page_hash, watch->id.fold(), watch);
  watch->buf_fix_count= 0;
  watch->state= BUF_BLOCK_POOL_WATCH;
}

/** Stop watching if the page has been read in.
buf_pool_watch_set(same_page_id) must have returned NULL before.
@param[in]	page_id	page id */
void buf_pool_watch_unset(const page_id_t page_id)
{
  /* FIXME: We only need buf_pool.mutex during the HASH_DELETE
  because it protects watch->in_page_hash. */
  mutex_enter(&buf_pool.mutex);

  rw_lock_t *hash_lock= buf_page_hash_lock_get(page_id);
  rw_lock_x_lock(hash_lock);

  /* The page must exist because buf_pool_watch_set() increments
  buf_fix_count. */
  buf_page_t *watch= buf_page_hash_get_low(page_id);

  if (watch->unfix() == 0 && buf_pool_watch_is_sentinel(watch))
  {
    /* The following is based on buf_pool_watch_remove(). */
    ut_d(watch->in_page_hash= FALSE);
    HASH_DELETE(buf_page_t, hash, buf_pool.page_hash, page_id.fold(), watch);
    rw_lock_x_unlock(hash_lock);
    /* Now that the watch is no longer reachable via buf_pool.page_hash,
    release it to buf_pool.watch[] for reuse. */
    watch->buf_fix_count= 0;
    watch->state= BUF_BLOCK_POOL_WATCH;
  }
  else
    rw_lock_x_unlock(hash_lock);
  mutex_exit(&buf_pool.mutex);
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
	rw_lock_t*	hash_lock = buf_page_hash_lock_get(page_id);

	rw_lock_s_lock(hash_lock);

	/* If not own buf_pool_mutex, page_hash can be changed. */
	hash_lock = buf_page_hash_lock_s_confirm(hash_lock, page_id);

	/* The page must exist because buf_pool_watch_set()
	increments buf_fix_count. */
	bpage = buf_page_hash_get_low(page_id);

	ret = !buf_pool_watch_is_sentinel(bpage);
	rw_lock_s_unlock(hash_lock);

	return(ret);
}

/********************************************************************//**
Moves a page to the start of the buffer pool LRU list. This high-level
function can be used to prevent an important page from slipping out of
the buffer pool.
@param[in,out]	bpage	buffer block of a file page */
void buf_page_make_young(buf_page_t* bpage)
{
	mutex_enter(&buf_pool.mutex);

	ut_a(buf_page_in_file(bpage));

	buf_LRU_make_block_young(bpage);

	mutex_exit(&buf_pool.mutex);
}

/** Mark the page status as FREED for the given tablespace id and
page number. If the page is not in the buffer pool then ignore it.
X-lock should be taken on the page before marking the page status
as FREED. It avoids the concurrent flushing of freed page.
Currently, this function only marks the page as FREED if it is
in buffer pool.
@param[in]	page_id	page id
@param[in,out]	mtr	mini-transaction
@param[in]	file	file name
@param[in]	line	line where called */
void buf_page_free(const page_id_t page_id,
                   mtr_t *mtr,
                   const char *file,
                   unsigned line)
{
  ut_ad(mtr);
  ut_ad(mtr->is_active());
  buf_pool.stat.n_page_gets++;
  rw_lock_t *hash_lock= buf_page_hash_lock_get(page_id);
  rw_lock_s_lock(hash_lock);

  /* page_hash can be changed. */
  hash_lock= buf_page_hash_lock_s_confirm(hash_lock, page_id);
  buf_block_t *block= reinterpret_cast<buf_block_t*>
    (buf_page_hash_get_low(page_id));

  if (!block || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE)
  {
    /* FIXME: if block!=NULL, convert to BUF_BLOCK_FILE_PAGE,
    but avoid buf_zip_decompress() */
    /* FIXME: If block==NULL, introduce a separate data structure
    to cover freed page ranges to augment buf_flush_freed_page() */
    rw_lock_s_unlock(hash_lock);
    return;
  }

  block->fix();
  mutex_enter(&block->mutex);
  /* Now safe to release page_hash mutex */
  rw_lock_s_unlock(hash_lock);
  ut_ad(block->page.buf_fix_count > 0);

#ifdef UNIV_DEBUG
  if (!fsp_is_system_temporary(page_id.space()))
  {
    ibool ret= rw_lock_s_lock_nowait(block->debug_latch, file, line);
    ut_a(ret);
  }
#endif /* UNIV_DEBUG */

  mtr_memo_type_t fix_type= MTR_MEMO_PAGE_X_FIX;
  rw_lock_x_lock_inline(&block->lock, 0, file, line);
  mtr_memo_push(mtr, block, fix_type);

  block->page.status= buf_page_t::FREED;
  buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
  mutex_exit(&block->mutex);
}

/** Attempts to discard the uncompressed frame of a compressed page.
The caller should not be holding any mutexes when this function is called.
@param[in]	page_id	page id */
static void buf_block_try_discard_uncompressed(const page_id_t page_id)
{
	buf_page_t*	bpage;

	/* Since we need to acquire buf_pool mutex to discard
	the uncompressed frame and because page_hash mutex resides
	below buf_pool mutex in sync ordering therefore we must
	first release the page_hash mutex. This means that the
	block in question can move out of page_hash. Therefore
	we need to check again if the block is still in page_hash. */
	mutex_enter(&buf_pool.mutex);

	bpage = buf_page_hash_get(page_id);

	if (bpage) {
		buf_LRU_free_page(bpage, false);
	}

	mutex_exit(&buf_pool.mutex);
}

/** Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with buf_page_release_zip().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size
@return pointer to the block */
buf_page_t* buf_page_get_zip(const page_id_t page_id, ulint zip_size)
{
	buf_page_t*	bpage;
	BPageMutex*	block_mutex;
	rw_lock_t*	hash_lock;
	ibool		discard_attempted = FALSE;
	ibool		must_read;

	ut_ad(zip_size);
	ut_ad(ut_is_2pow(zip_size));
	buf_pool.stat.n_page_gets++;

	for (;;) {
lookup:

		/* The following call will also grab the page_hash
		mutex if the page is found. */
		bpage = buf_page_hash_get_s_locked(page_id, &hash_lock);
		if (bpage) {
			ut_ad(!buf_pool_watch_is_sentinel(bpage));
			break;
		}

		/* Page not in buf_pool: needs to be read from file */

		ut_ad(!hash_lock);
		dberr_t err = buf_read_page(page_id, zip_size);

		if (err != DB_SUCCESS) {
			ib::error() << "Reading compressed page " << page_id
				<< " failed with error: " << ut_strerr(err);

			goto err_exit;
		}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
	}

	ut_ad(buf_page_hash_lock_held_s(bpage));

	if (!bpage->zip.data) {
		/* There is no compressed page. */
err_exit:
		rw_lock_s_unlock(hash_lock);
		return(NULL);
	}

	ut_ad(!buf_pool_watch_is_sentinel(bpage));

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		bpage->fix();
		block_mutex = &buf_pool.zip_mutex;
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

	DBUG_ASSERT(bpage->status != buf_page_t::FREED);

	buf_page_set_accessed(bpage);

	mutex_exit(block_mutex);

	buf_page_make_young_if_needed(bpage);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
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
	block->skip_flush_check = false;
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

	ut_ad(block->zip_size());
	ut_a(block->page.id.space() != 0);

	if (UNIV_UNLIKELY(check && !page_zip_verify_checksum(frame, size))) {

		ib::error() << "Compressed page checksum mismatch for "
			<< (space ? space->chain.start->name : "")
			<< block->page.id << ": stored: "
			<< mach_read_from_4(frame + FIL_PAGE_SPACE_OR_CHKSUM)
			<< ", crc32: "
			<< page_zip_calc_checksum(
				frame, size, SRV_CHECKSUM_ALGORITHM_CRC32)
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
				space->release_for_io();
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
		memcpy(block->frame, frame, block->zip_size());
		if (space) {
			space->release_for_io();
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
	}

	if (space) {
		if (encrypted) {
			dict_set_encrypted_by_space(space);
		} else {
			dict_set_corrupted_by_space(space);
		}

		space->release_for_io();
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
    break;
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

  mtr_memo_push(mtr, block, fix_type);
  return block;
}

/** Low level function used to get access to a database page.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file			file name
@param[in]	line			line where called
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge to happen
while reading the page from file
then it makes sure that it does merging of change buffer changes while
reading the page from file.
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_low(
	const page_id_t		page_id,
	ulint			zip_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge)
{
	buf_block_t*	block;
	unsigned	access_time;
	rw_lock_t*	hash_lock;
	buf_block_t*	fix_block;
	ulint		retries = 0;

	ut_ad((mtr == NULL) == (mode == BUF_EVICT_IF_IN_POOL));
	ut_ad(!mtr || mtr->is_active());
	ut_ad((rw_latch == RW_S_LATCH)
	      || (rw_latch == RW_X_LATCH)
	      || (rw_latch == RW_SX_LATCH)
	      || (rw_latch == RW_NO_LATCH));
	ut_ad(!allow_ibuf_merge
	      || mode == BUF_GET
	      || mode == BUF_GET_IF_IN_POOL
	      || mode == BUF_GET_IF_IN_POOL_OR_WATCH);

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
		fil_space_t* s = fil_space_acquire_for_io(page_id.space());
		ut_ad(s);
		ut_ad(s->zip_size() == zip_size);
		s->release_for_io();
	}
#endif /* UNIV_DEBUG */

	ut_ad(!mtr || !ibuf_inside(mtr)
	      || ibuf_page_low(page_id, zip_size, FALSE, file, line, NULL));

	buf_pool.stat.n_page_gets++;
	hash_lock = buf_page_hash_lock_get(page_id);
loop:
	block = guess;

	rw_lock_s_lock(hash_lock);

	/* page_hash can be changed. */
	hash_lock = buf_page_hash_lock_s_confirm(hash_lock, page_id);

	if (block != NULL) {

		/* If the guess is a compressed page descriptor that
		has been allocated by buf_page_alloc_descriptor(),
		it may have been freed by buf_relocate(). */

		if (!buf_pool.is_uncompressed(block)
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
		block = (buf_block_t*) buf_page_hash_get_low(page_id);
	}

	if (!block || buf_pool_watch_is_sentinel(&block->page)) {
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
				hash_lock, page_id);

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
					fix_block->fix();
					mutex_exit(fix_mutex);
				} else {
					fix_block->fix();
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

		dberr_t local_err = buf_read_page(page_id, zip_size);

		if (local_err == DB_SUCCESS) {
			buf_read_ahead_random(page_id, zip_size,
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
			if (page_id.space() == TRX_SYS_SPACE) {
			} else if (page_id.space() == SRV_TMP_SPACE_ID) {
			} else if (fil_space_t* space
				   = fil_space_acquire_for_io(
					   page_id.space())) {
				bool set = dict_set_corrupted_by_space(space);
				space->release_for_io();
				if (set) {
					return NULL;
				}
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
		if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		goto loop;
	} else {
		fix_block = block;
	}

	if (fsp_is_system_temporary(page_id.space())) {
		/* For temporary tablespace, the mutex is being used
		for synchorization between user thread and flush thread,
		instead of block->lock. See buf_flush_page() for the flush
		thread counterpart. */
		BPageMutex*	fix_mutex = buf_page_get_mutex(
						&fix_block->page);
		mutex_enter(fix_mutex);
		fix_block->fix();
		mutex_exit(fix_mutex);
	} else {
		fix_block->fix();
	}

	/* Now safe to release page_hash mutex */
	rw_lock_s_unlock(hash_lock);

got_block:
	switch (mode) {
	default:
		ut_ad(block->zip_size() == zip_size);
		break;
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
			fix_block->unfix();

			return(NULL);
		}
	}

	switch (UNIV_EXPECT(buf_block_get_state(fix_block),
			    BUF_BLOCK_FILE_PAGE)) {
	case BUF_BLOCK_FILE_PAGE:
		if (fsp_is_system_temporary(page_id.space())
		    && buf_block_get_io_fix(block) != BUF_IO_NONE) {
			/* This suggests that the page is being flushed.
			Avoid returning reference to this page.
			Instead wait for the flush action to complete. */
			fix_block->unfix();
			os_thread_sleep(WAIT_FOR_WRITE);
			goto loop;
		}

		if (UNIV_UNLIKELY(mode == BUF_EVICT_IF_IN_POOL)) {
evict_from_pool:
			ut_ad(!fix_block->page.oldest_modification);
			mutex_enter(&buf_pool.mutex);
			fix_block->unfix();

			if (!buf_LRU_free_page(&fix_block->page, true)) {
				ut_ad(0);
			}

			mutex_exit(&buf_pool.mutex);
			return(NULL);
		}
		break;
	default:
		ut_error;
		break;

	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_ZIP_DIRTY:
		if (UNIV_UNLIKELY(mode == BUF_EVICT_IF_IN_POOL)) {
			goto evict_from_pool;
		}

		if (mode == BUF_PEEK_IF_IN_POOL) {
			/* This mode is only used for dropping an
			adaptive hash index.  There cannot be an
			adaptive hash index for a compressed-only
			page, so do not bother decompressing the page. */
			fix_block->unfix();

			return(NULL);
		}

		buf_page_t* bpage = &block->page;

		/* Note: We have already buffer fixed this block. */
		if (bpage->buf_fix_count > 1
		    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

			/* This condition often occurs when the buffer
			is not buffer-fixed, but I/O-fixed by
			buf_page_init_for_read(). */
			fix_block->unfix();

			/* The block is buffer-fixed or I/O-fixed.
			Try again later. */
			os_thread_sleep(WAIT_FOR_READ);

			goto loop;
		}

		/* Buffer-fix the block so that it cannot be evicted
		or relocated while we are attempting to allocate an
		uncompressed page. */

		block = buf_LRU_get_free_block();

		mutex_enter(&buf_pool.mutex);

		hash_lock = buf_page_hash_lock_get(page_id);

		rw_lock_x_lock(hash_lock);

		/* Buffer-fixing prevents the page_hash from changing. */
		ut_ad(bpage == buf_page_hash_get_low(page_id));

		fix_block->unfix();

		buf_page_mutex_enter(block);
		mutex_enter(&buf_pool.zip_mutex);

		fix_block = block;

		if (bpage->buf_fix_count > 0
		    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {

			mutex_exit(&buf_pool.zip_mutex);
			/* The block was buffer-fixed or I/O-fixed while
			buf_pool.mutex was not held by this thread.
			Free the block that was allocated and retry.
			This should be extremely unlikely, for example,
			if buf_page_get_zip() was invoked. */

			buf_LRU_block_free_non_file_page(block);
			mutex_exit(&buf_pool.mutex);
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

		UNIV_MEM_DESC(&block->page.zip.data,
			      page_zip_get_size(&block->page.zip));

		if (buf_page_get_state(&block->page) == BUF_BLOCK_ZIP_PAGE) {
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
			UT_LIST_REMOVE(buf_pool.zip_clean, &block->page);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
			ut_ad(!block->page.in_flush_list);
		} else {
			/* Relocate buf_pool.flush_list. */
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

		UNIV_MEM_INVALID(bpage, sizeof *bpage);

		rw_lock_x_unlock(hash_lock);
		buf_pool.n_pend_unzip++;
		mutex_exit(&buf_pool.zip_mutex);
		mutex_exit(&buf_pool.mutex);

		access_time = buf_page_is_accessed(&block->page);

		buf_page_mutex_exit(block);

		if (!access_time && !recv_no_ibuf_operations
		    && ibuf_page_exists(block->page.id, zip_size)) {
			block->page.ibuf_exist = true;
		}

		buf_page_free_descriptor(bpage);

		/* Decompress the page while not holding
		buf_pool.mutex or block->mutex. */

		if (!buf_zip_decompress(block, TRUE)) {
			mutex_enter(&buf_pool.mutex);
			buf_page_mutex_enter(fix_block);
			buf_block_set_io_fix(fix_block, BUF_IO_NONE);
			buf_page_mutex_exit(fix_block);

			--buf_pool.n_pend_unzip;
			mutex_exit(&buf_pool.mutex);
			fix_block->unfix();
			rw_lock_x_unlock(&fix_block->lock);

			if (err) {
				*err = DB_PAGE_CORRUPTED;
			}
			return NULL;
		}

		mutex_enter(&buf_pool.mutex);

		buf_page_mutex_enter(fix_block);

		buf_block_set_io_fix(fix_block, BUF_IO_NONE);

		buf_page_mutex_exit(fix_block);

		--buf_pool.n_pend_unzip;

		mutex_exit(&buf_pool.mutex);

		rw_lock_x_unlock(&block->lock);

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

		mutex_enter(&buf_pool.mutex);

		fix_block->unfix();

		/* Now we are only holding the buf_pool.mutex,
		not block->mutex or hash_lock. Blocks cannot be
		relocated or enter or exit the buf_pool while we
		are holding the buf_pool.mutex. */

		if (buf_LRU_free_page(&fix_block->page, true)) {

			mutex_exit(&buf_pool.mutex);

			/* page_hash can be changed. */
			hash_lock = buf_page_hash_lock_get(page_id);
			rw_lock_x_lock(hash_lock);

			/* If not own buf_pool_mutex,
			page_hash can be changed. */
			hash_lock = buf_page_hash_lock_x_confirm(
				hash_lock, page_id);

			if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
				/* Set the watch, as it would have
				been set if the page were not in the
				buffer pool in the first place. */
				block = (buf_block_t*) buf_pool_watch_set(
					page_id, &hash_lock);
			} else {
				block = (buf_block_t*) buf_page_hash_get_low(
					page_id);
			}

			rw_lock_x_unlock(hash_lock);

			if (block != NULL) {
				/* Either the page has been read in or
				a watch was set on that in the window
				where we released the buf_pool.mutex
				and before we acquire the hash_lock
				above. Try again. */
				guess = block;

				goto loop;
			}

			return(NULL);
		}

		buf_page_mutex_enter(fix_block);

		if (buf_flush_page_try(fix_block)) {
			guess = fix_block;

			goto loop;
		}

		buf_page_mutex_exit(fix_block);

		fix_block->fix();

		/* Failed to evict the page; change it directly */

		mutex_exit(&buf_pool.mutex);
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
			fix_block->debug_latch, file, line);
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
	      || fix_block->page.status != buf_page_t::FREED);

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
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
	ut_a(buf_block_get_state(fix_block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	/* We have to wait here because the IO_READ state was set
	under the protection of the hash_lock and not the block->mutex
	and block->lock. */
	buf_wait_for_read(fix_block);

	if (fix_block->page.id != page_id) {
		fix_block->unfix();

#ifdef UNIV_DEBUG
		if (!fsp_is_system_temporary(page_id.space())) {
			rw_lock_s_unlock(fix_block->debug_latch);
		}
#endif /* UNIV_DEBUG */

		if (err) {
			*err = DB_PAGE_CORRUPTED;
		}

		return NULL;
	}

	if (allow_ibuf_merge
	    && mach_read_from_2(fix_block->frame + FIL_PAGE_TYPE)
	    == FIL_PAGE_INDEX
	    && page_is_leaf(fix_block->frame)) {
		rw_lock_x_lock_inline(&fix_block->lock, 0, file, line);

		if (fix_block->page.ibuf_exist) {
			fix_block->page.ibuf_exist = false;
			ibuf_merge_or_delete_for_page(fix_block, page_id,
						      zip_size, true);
		}

		if (rw_latch == RW_X_LATCH) {
			mtr->memo_push(fix_block, MTR_MEMO_PAGE_X_FIX);
		} else {
			rw_lock_x_unlock(&fix_block->lock);
			goto get_latch;
		}
	} else {
get_latch:
		fix_block = buf_page_mtr_lock(fix_block, rw_latch, mtr,
					      file, line);
	}

	if (mode != BUF_PEEK_IF_IN_POOL && !access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */

		buf_read_ahead_linear(page_id, zip_size, ibuf_inside(mtr));
	}

	ut_ad(!rw_lock_own_flagged(hash_lock,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	return(fix_block);
}

/** Get access to a database page. Buffered redo log may be applied.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file			file name
@param[in]	line			line where called
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge while
reading the pages from file.
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	ulint			zip_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge)
{
  if (buf_block_t *block= recv_sys.recover(page_id))
  {
    block->fix();
    ut_ad(rw_lock_s_lock_nowait(block->debug_latch, file, line));
    block= buf_page_mtr_lock(block, rw_latch, mtr, file, line);
    return block;
  }

  return buf_page_get_low(page_id, zip_size, rw_latch,
                          guess, mode, file, line, mtr, err, allow_ibuf_merge);
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
	      || ibuf_page(block->page.id, block->zip_size(), NULL));

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
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	if (!access_time) {
		/* In the case of a first access, try to apply linear
		read-ahead */
		buf_read_ahead_linear(block->page.id, block->zip_size(),
				      ibuf_inside(mtr));
	}

	buf_pool.stat.n_page_gets++;

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
	rw_lock_t*	hash_lock;

	ut_ad(mtr);
	ut_ad(mtr->is_active());

	block = buf_block_hash_get_s_locked(page_id, &hash_lock);

	if (!block || buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {
		if (block) {
			rw_lock_s_unlock(hash_lock);
		}
		return(NULL);
	}

	ut_ad(!buf_pool_watch_is_sentinel(&block->page));

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
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
	ut_a(block->page.buf_fix_count > 0);
	ut_a(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

	buf_pool.stat.n_page_gets++;

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
	bpage->oldest_modification = 0;
	bpage->write_size = 0;
	bpage->real_size = 0;
	bpage->slot = NULL;
	bpage->ibuf_exist = false;
	bpage->status = buf_page_t::NORMAL;
	HASH_INVALIDATE(bpage, hash);
}

/** Inits a page to the buffer buf_pool.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	block		block to init */
static void buf_page_init(const page_id_t page_id, ulint zip_size,
                          buf_block_t *block)
{
	buf_page_t*	hash_page;

	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(buf_page_mutex_own(block));
	ut_a(buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE);
	ut_ad(rw_lock_own(buf_page_hash_lock_get(page_id), RW_LOCK_X));

	/* Set the state of the block */
	buf_block_set_file_page(block, page_id);

#ifdef UNIV_DEBUG_VALGRIND
	if (is_system_tablespace(page_id.space())) {
		/* Silence valid Valgrind warnings about uninitialized
		data being written to data files.  There are some unused
		bytes on some pages that InnoDB does not initialize. */
		UNIV_MEM_VALID(block->frame, srv_page_size);
	}
#endif /* UNIV_DEBUG_VALGRIND */

	buf_block_init_low(block);

	block->lock_hash_val = lock_rec_hash(page_id.space(),
					     page_id.page_no());

	buf_page_init_low(&block->page);

	/* Insert into the hash table of file pages */

	hash_page = buf_page_hash_get_low(page_id);

	if (hash_page == NULL) {
		/* Block not found in hash table */
	} else if (buf_pool_watch_is_sentinel(hash_page)) {
		/* Preserve the reference count. */
		ib_uint32_t	buf_fix_count = hash_page->buf_fix_count;

		ut_a(buf_fix_count > 0);

		block->page.buf_fix_count += buf_fix_count;

		buf_pool_watch_remove(hash_page);
	} else {
		ib::fatal() << "Page " << page_id
			<< " already found in the hash table: "
			<< hash_page << ", " << block;
	}

	ut_ad(!block->page.in_zip_hash);
	ut_ad(!block->page.in_page_hash);
	ut_d(block->page.in_page_hash = TRUE);

	block->page.id = page_id;

	HASH_INSERT(buf_page_t, hash, buf_pool.page_hash,
		    page_id.fold(), &block->page);

	page_zip_set_size(&block->page.zip, zip_size);
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
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	unzip			whether the uncompressed page is
					requested (for ROW_FORMAT=COMPRESSED)
@return pointer to the block
@retval	NULL	in case of an error */
buf_page_t*
buf_page_init_for_read(
	dberr_t*		err,
	ulint			mode,
	const page_id_t		page_id,
	ulint			zip_size,
	bool			unzip)
{
	buf_block_t*	block;
	buf_page_t*	bpage	= NULL;
	buf_page_t*	watch_page;
	rw_lock_t*	hash_lock;
	mtr_t		mtr;
	bool		lru	= false;
	void*		data;

	*err = DB_SUCCESS;

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {
		/* It is a read-ahead within an ibuf routine */

		ut_ad(!ibuf_bitmap_page(page_id, zip_size));

		ibuf_mtr_start(&mtr);

		if (!recv_no_ibuf_operations
		    && !ibuf_page(page_id, zip_size, &mtr)) {

			ibuf_mtr_commit(&mtr);

			return(NULL);
		}
	} else {
		ut_ad(mode == BUF_READ_ANY_PAGE);
	}

	if (zip_size && !unzip && !recv_recovery_is_on()) {
		block = NULL;
	} else {
		block = buf_LRU_get_free_block();
		ut_ad(block);
	}

	mutex_enter(&buf_pool.mutex);

	hash_lock = buf_page_hash_lock_get(page_id);
	rw_lock_x_lock(hash_lock);

	watch_page = buf_page_hash_get_low(page_id);
	if (watch_page && !buf_pool_watch_is_sentinel(watch_page)) {
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

		buf_page_init(page_id, zip_size, block);

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

		if (zip_size) {
			/* buf_pool.mutex may be released and
			reacquired by buf_buddy_alloc().  Thus, we
			must release block->mutex in order not to
			break the latching order in the reacquisition
			of buf_pool.mutex.  We also must defer this
			operation until after the block descriptor has
			been added to buf_pool.LRU and
			buf_pool.page_hash. */
			buf_page_mutex_exit(block);
			data = buf_buddy_alloc(zip_size, &lru);
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
		data = buf_buddy_alloc(zip_size, &lru);

		rw_lock_x_lock(hash_lock);

		/* If buf_buddy_alloc() allocated storage from the LRU list,
		it released and reacquired buf_pool.mutex.  Thus, we must
		check the page_hash again, as it may have been modified. */
		if (UNIV_UNLIKELY(lru)) {
			watch_page = buf_page_hash_get_low(page_id);

			if (UNIV_UNLIKELY(watch_page
			    && !buf_pool_watch_is_sentinel(watch_page))) {

				/* The block was added by some other thread. */
				rw_lock_x_unlock(hash_lock);
				watch_page = NULL;
				buf_buddy_free(data, zip_size);

				bpage = NULL;
				goto func_exit;
			}
		}

		bpage = buf_page_alloc_descriptor();

		page_zip_des_init(&bpage->zip);
		page_zip_set_size(&bpage->zip, zip_size);
		bpage->zip.data = (page_zip_t*) data;

		mutex_enter(&buf_pool.zip_mutex);
		UNIV_MEM_DESC(bpage->zip.data, zip_size);

		buf_page_init_low(bpage);

		bpage->state = BUF_BLOCK_ZIP_PAGE;
		bpage->id = page_id;
		bpage->status = buf_page_t::NORMAL;

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

			bpage->buf_fix_count += buf_fix_count;

			ut_ad(buf_pool_watch_is_sentinel(watch_page));
			buf_pool_watch_remove(watch_page);
		}

		HASH_INSERT(buf_page_t, hash, buf_pool.page_hash,
			    bpage->id.fold(), bpage);

		rw_lock_x_unlock(hash_lock);

		/* The block must be put to the LRU list, to the old blocks.
		The zip size is already set into the page zip */
		buf_LRU_add_block(bpage, TRUE/* to old blocks */);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		buf_page_set_io_fix(bpage, BUF_IO_READ);

		mutex_exit(&buf_pool.zip_mutex);
	}

	buf_pool.n_pend_reads++;
func_exit:
	mutex_exit(&buf_pool.mutex);

	if (mode == BUF_READ_IBUF_PAGES_ONLY) {

		ibuf_mtr_commit(&mtr);
	}

	ut_ad(!rw_lock_own_flagged(hash_lock,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));
	ut_ad(!bpage || buf_page_in_file(bpage));

	return(bpage);
}

/** Initialize a page in the buffer pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen).
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(
	const page_id_t		page_id,
	ulint			zip_size,
	mtr_t*			mtr)
{
	buf_frame_t*	frame;
	buf_block_t*	block;
	buf_block_t*	free_block	= NULL;
	rw_lock_t*	hash_lock;

	ut_ad(mtr->is_active());
	ut_ad(page_id.space() != 0 || !zip_size);

	free_block = buf_LRU_get_free_block();

	mutex_enter(&buf_pool.mutex);

	hash_lock = buf_page_hash_lock_get(page_id);
	rw_lock_x_lock(hash_lock);

	block = (buf_block_t*) buf_page_hash_get_low(page_id);

	if (block
	    && buf_page_in_file(&block->page)
	    && !buf_pool_watch_is_sentinel(&block->page)) {
		/* Page can be found in buf_pool */
		mutex_exit(&buf_pool.mutex);
		rw_lock_x_unlock(hash_lock);

		buf_block_free(free_block);

		if (!recv_recovery_is_on()) {
			/* FIXME: Remove the redundant lookup and avoid
			the unnecessary invocation of buf_zip_decompress().
			We may have to convert buf_page_t to buf_block_t,
			but we are going to initialize the page. */
			return buf_page_get_gen(page_id, zip_size, RW_NO_LATCH,
						block, BUF_GET_POSSIBLY_FREED,
						__FILE__, __LINE__, mtr);
		}

		mutex_exit(&recv_sys.mutex);
		block = buf_page_get_with_no_latch(page_id, zip_size, mtr);
		mutex_enter(&recv_sys.mutex);
		return block;
	}

	/* If we get here, the page was not in buf_pool: init it there */

	DBUG_PRINT("ib_buf", ("create page %u:%u",
			      page_id.space(), page_id.page_no()));

	block = free_block;

	buf_page_mutex_enter(block);

	buf_page_init(page_id, zip_size, block);

	rw_lock_x_unlock(hash_lock);

	/* The block must be put to the LRU list */
	buf_LRU_add_block(&block->page, FALSE);

	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	buf_pool.stat.n_pages_created++;

	if (zip_size) {
		/* Prevent race conditions during buf_buddy_alloc(),
		which may release and reacquire buf_pool.mutex,
		by IO-fixing and X-latching the block. */

		buf_page_set_io_fix(&block->page, BUF_IO_READ);
		rw_lock_x_lock(&block->lock);

		buf_page_mutex_exit(block);
		/* buf_pool.mutex may be released and reacquired by
		buf_buddy_alloc().  Thus, we must release block->mutex
		in order not to break the latching order in
		the reacquisition of buf_pool.mutex.  We also must
		defer this operation until after the block descriptor
		has been added to buf_pool.LRU and buf_pool.page_hash. */
		block->page.zip.data = buf_buddy_alloc(zip_size);
		buf_page_mutex_enter(block);

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

	mutex_exit(&buf_pool.mutex);

	mtr_memo_push(mtr, block, MTR_MEMO_BUF_FIX);

	buf_page_set_accessed(&block->page);

	buf_page_mutex_exit(block);

	/* Delete possible entries for the page from the insert buffer:
	such can exist if the page belonged to an index which was dropped */
	if (!recv_recovery_is_on()) {
		ibuf_merge_or_delete_for_page(NULL, page_id, zip_size, true);
	}

	frame = block->frame;

	static_assert(FIL_PAGE_PREV % 8 == 0, "alignment");
	static_assert(FIL_PAGE_PREV + 4 == FIL_PAGE_NEXT, "adjacent");
	memset_aligned<8>(frame + FIL_PAGE_PREV, 0xff, 8);
	mach_write_to_2(frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_ALLOCATED);

	/* FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION is only used on the
	following pages:
	(1) The first page of the InnoDB system tablespace (page 0:0)
	(2) FIL_RTREE_SPLIT_SEQ_NUM on R-tree pages
	(3) key_version on encrypted pages (not page 0:0) */

	memset(frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
	static_assert(FIL_PAGE_LSN % 8 == 0, "alignment");
	memset_aligned<8>(frame + FIL_PAGE_LSN, 0, 8);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
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
	case FIL_PAGE_TYPE_INSTANT:
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		level = btr_page_get_level(frame);

		/* Check if it is an index page for insert buffer */
		if (fil_page_get_type(frame) == FIL_PAGE_INDEX
		    && btr_page_get_index_id(frame)
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
		dict_set_corrupted_by_space(&space);
	} else {
		dict_set_encrypted_by_space(&space);
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
	const ibool	uncompressed = (buf_page_get_state(bpage)
					== BUF_BLOCK_FILE_PAGE);
	page_id_t	old_page_id = bpage->id;

	/* First unfix and release lock on the bpage */
	mutex_enter(&buf_pool.mutex);
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

	ut_ad(buf_pool.n_pend_reads > 0);
	buf_pool.n_pend_reads--;

	mutex_exit(&buf_pool.mutex);
}

/** Check if the encrypted page is corrupted for the full crc32 format.
@param[in]	space_id	page belongs to space id
@param[in]	d		page
@param[in]	is_compressed	compressed page
@return true if page is corrupted or false if it isn't */
static bool buf_page_full_crc32_is_corrupted(ulint space_id, const byte* d,
                                             bool is_compressed)
{
  if (space_id != mach_read_from_4(d + FIL_PAGE_SPACE_ID))
    return true;

  static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");

  return !is_compressed &&
    memcmp_aligned<4>(FIL_PAGE_LSN + 4 + d,
                      d + srv_page_size - FIL_PAGE_FCRC32_END_LSN, 4);
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
	ut_ad(space->pending_io());

	byte* dst_frame = (bpage->zip.data) ? bpage->zip.data :
		((buf_block_t*) bpage)->frame;
	dberr_t err = DB_SUCCESS;
	uint key_version = buf_page_get_key_version(dst_frame, space->flags);

	/* In buf_decrypt_after_read we have either decrypted the page if
	page post encryption checksum matches and used key_id is found
	from the encryption plugin. If checksum did not match page was
	not decrypted and it could be either encrypted and corrupted
	or corrupted or good page. If we decrypted, there page could
	still be corrupted if used key does not match. */
	const bool seems_encrypted = !space->full_crc32() && key_version
		&& space->crypt_data
		&& space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED;
	ut_ad(space->purpose != FIL_TYPE_TEMPORARY || space->full_crc32());

	/* If traditional checksums match, we assume that page is
	not anymore encrypted. */
	if (space->full_crc32()
	    && !buf_is_zeroes(span<const byte>(dst_frame,
					       space->physical_size()))
	    && (key_version || space->is_compressed()
		|| space->purpose == FIL_TYPE_TEMPORARY)) {
		if (buf_page_full_crc32_is_corrupted(
			    space->id, dst_frame, space->is_compressed())) {
			err = DB_PAGE_CORRUPTED;
		}
	} else if (buf_page_is_corrupted(true, dst_frame, space->flags)) {
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
			<< key_version
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
@param[in,out]	bpage		page to complete
@param[in]	dblwr		whether the doublewrite buffer was used (on write)
@param[in]	evict		whether or not to evict the page from LRU list
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
	ut_ad(!!bpage->zip.ssize == (bpage->zip.data != NULL));
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
			buf_pool.n_pend_unzip++;
			ibool ok = buf_zip_decompress((buf_block_t*) bpage,
						      FALSE);
			buf_pool.n_pend_unzip--;

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
		} else if (((!space->full_crc32()
			     || bpage->id.space() != TRX_SYS_SPACE)
			    && bpage->id.space() != read_space_id)
			   || bpage->id.page_no() != read_page_no) {
			/* We do not compare space_id to read_space_id
			in the system tablespace unless space->full_crc32(),
			because the field was written as garbage before
			MySQL 4.1.1, which introduced support for
			innodb_file_per_table. */

			if (space->full_crc32()
			    && *reinterpret_cast<uint32_t*>
			    (&frame[FIL_PAGE_FCRC32_KEY_VERSION])
			    && space->crypt_data
			    && space->crypt_data->type
			    != CRYPT_SCHEME_UNENCRYPTED) {
				ib::error() << "Cannot decrypt " << bpage->id;
				err = DB_DECRYPTION_FAILED;
				goto release_page;
			}

			ib::error() << "Space id and page no stored in "
				"the page, read in are "
				<< page_id_t(read_space_id, read_page_no)
				<< ", should be " << bpage->id;
		}

		err = buf_page_check_corrupt(bpage, space);

database_corrupted:

		if (err != DB_SUCCESS) {
			/* Not a real corruption if it was triggered by
			error injection */
			DBUG_EXECUTE_IF(
				"buf_page_import_corrupt_failure",
				if (!is_predefined_tablespace(
					    bpage->id.space())) {
					buf_corrupt_page_release(bpage, space);
					ib::info() << "Simulated IMPORT "
						"corruption";
					space->release_for_io();
					return(err);
				}
				err = DB_SUCCESS;
				goto page_not_corrupt;
			);

			if (err == DB_PAGE_CORRUPTED) {
				ib::error()
					<< "Database page corruption on disk"
					" or a failed file read of tablespace "
					<< space->name << " page " << bpage->id
					<< ". You may have to recover from "
					<< "a backup.";

				buf_page_print(frame, bpage->zip_size());

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
				space->release_for_io();
				return(err);
			}
		}

		DBUG_EXECUTE_IF("buf_page_import_corrupt_failure",
				page_not_corrupt: bpage = bpage; );

		if (err == DB_PAGE_CORRUPTED
		    || err == DB_DECRYPTION_FAILED) {
release_page:
			const page_id_t corrupt_page_id = bpage->id;

			buf_corrupt_page_release(bpage, space);

			if (recv_recovery_is_on()) {
				recv_sys.free_corrupted_page(corrupt_page_id);
			}

			space->release_for_io();
			return err;
		}

		if (recv_recovery_is_on()) {
			recv_recover_page(space, bpage);
		}

		if (uncompressed
		    && !recv_no_ibuf_operations
		    && (bpage->id.space() == 0
			|| !is_predefined_tablespace(bpage->id.space()))
		    && fil_page_get_type(frame) == FIL_PAGE_INDEX
		    && page_is_leaf(frame)
		    && ibuf_page_exists(bpage->id, bpage->zip_size())) {
			bpage->ibuf_exist = true;
		}

		space->release_for_io();
	} else {
		/* io_type == BUF_IO_WRITE */
		if (bpage->slot) {
			/* Mark slot free */
			bpage->slot->release();
			bpage->slot = NULL;
		}
	}

	BPageMutex* block_mutex = buf_page_get_mutex(bpage);
	mutex_enter(&buf_pool.mutex);
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

		ut_ad(buf_pool.n_pend_reads > 0);
		buf_pool.n_pend_reads--;
		buf_pool.stat.n_pages_read++;

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

		buf_pool.stat.n_pages_written++;

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
	mutex_exit(&buf_pool.mutex);
	return DB_SUCCESS;
}

#ifdef UNIV_DEBUG
/** Check that all blocks are in a replaceable state.
@return address of a non-free block
@retval nullptr if all freed */
void buf_pool_t::assert_all_freed()
{
  mutex_enter(&mutex);
  const chunk_t *chunk= chunks;
  for (auto i= n_chunks; i--; chunk++)
    if (const buf_block_t* block= chunk->not_freed())
      ib::fatal() << "Page " << block->page.id << " still fixed or dirty";
  mutex_exit(&mutex);
}
#endif /* UNIV_DEBUG */

/** Refresh the statistics used to print per-second averages. */
void buf_refresh_io_stats()
{
	buf_pool.last_printout_time = time(NULL);
	buf_pool.old_stat = buf_pool.stat;
}

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate()
{
	mutex_enter(&buf_pool.mutex);

	for (unsigned i = BUF_FLUSH_LRU; i < BUF_FLUSH_N_TYPES; i++) {

		/* As this function is called during startup and
		during redo application phase during recovery, InnoDB
		is single threaded (apart from IO helper threads) at
		this stage. No new write batch can be in intialization
		stage at this point. */
		ut_ad(!buf_pool.init_flush[i]);

		/* However, it is possible that a write batch that has
		been posted earlier is still not complete. For buffer
		pool invalidation to proceed we must ensure there is NO
		write activity happening. */
		if (buf_pool.n_flush[i] > 0) {
			buf_flush_t	type = buf_flush_t(i);

			mutex_exit(&buf_pool.mutex);
			buf_flush_wait_batch_end(type);
			mutex_enter(&buf_pool.mutex);
		}
	}

	ut_d(mutex_exit(&buf_pool.mutex));
	ut_d(buf_pool.assert_all_freed());
	ut_d(mutex_enter(&buf_pool.mutex));

	while (buf_LRU_scan_and_free_block(true));

	ut_ad(UT_LIST_GET_LEN(buf_pool.LRU) == 0);
	ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);

	buf_pool.freed_page_clock = 0;
	buf_pool.LRU_old = NULL;
	buf_pool.LRU_old_len = 0;

	memset(&buf_pool.stat, 0x00, sizeof(buf_pool.stat));
	buf_refresh_io_stats();
	mutex_exit(&buf_pool.mutex);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validate the buffer pool. */
void buf_pool_t::validate()
{
	buf_page_t*	b;
	chunk_t*	chunk;
	ulint		i;
	ulint		n_lru_flush	= 0;
	ulint		n_page_flush	= 0;
	ulint		n_list_flush	= 0;
	ulint		n_lru		= 0;
	ulint		n_flush		= 0;
	ulint		n_free		= 0;
	ulint		n_zip		= 0;

	mutex_enter(&buf_pool.mutex);
	hash_lock_x_all(buf_pool.page_hash);

	chunk = buf_pool.chunks;

	/* Check the uncompressed blocks. */

	for (i = buf_pool.n_chunks; i--; chunk++) {

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
				ut_ad(buf_page_hash_get_low(block->page.id)
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
					ut_ad(rw_lock_is_locked(&block->lock,
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

	mutex_enter(&buf_pool.zip_mutex);

	/* Check clean compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool.zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
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
		we have acquired buf_pool.zip_mutex above which acts
		as the 'block->mutex' for these bpages. */
		ut_ad(!b->oldest_modification);
		ut_ad(buf_page_hash_get_low(b->id) == b);
		n_lru++;
		n_zip++;
	}

	/* Check dirty blocks. */

	mutex_enter(&buf_pool.flush_list_mutex);
	for (b = UT_LIST_GET_FIRST(buf_pool.flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_flush_list);
		ut_ad(b->oldest_modification);
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
		ut_ad(buf_page_hash_get_low(b->id) == b);
	}

	ut_ad(UT_LIST_GET_LEN(buf_pool.flush_list) == n_flush);

	hash_unlock_x_all(buf_pool.page_hash);
	mutex_exit(&buf_pool.flush_list_mutex);

	mutex_exit(&buf_pool.zip_mutex);

	if (buf_pool.curr_size == buf_pool.old_size
	    && n_lru + n_free > buf_pool.curr_size + n_zip) {

		ib::fatal() << "n_LRU " << n_lru << ", n_free " << n_free
			<< ", pool " << buf_pool.curr_size
			<< " zip " << n_zip << ". Aborting...";
	}

	ut_ad(UT_LIST_GET_LEN(buf_pool.LRU) == n_lru);

	if (buf_pool.curr_size == buf_pool.old_size
	    && UT_LIST_GET_LEN(buf_pool.free) != n_free) {

		ib::fatal() << "Free list len "
			<< UT_LIST_GET_LEN(buf_pool.free)
			<< ", free blocks " << n_free << ". Aborting...";
	}

	ut_ad(buf_pool.n_flush[BUF_FLUSH_LIST] == n_list_flush);
	ut_ad(buf_pool.n_flush[BUF_FLUSH_LRU] == n_lru_flush);
	ut_ad(buf_pool.n_flush[BUF_FLUSH_SINGLE_PAGE] == n_page_flush);

	mutex_exit(&buf_pool.mutex);

	ut_d(buf_LRU_validate());
	ut_d(buf_flush_validate());
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Write information of the buf_pool to the error log. */
void buf_pool_t::print()
{
	index_id_t*	index_ids;
	ulint*		counts;
	ulint		size;
	ulint		i;
	ulint		j;
	index_id_t	id;
	ulint		n_found;
	chunk_t*	chunk;
	dict_index_t*	index;

	size = curr_size;

	index_ids = static_cast<index_id_t*>(
		ut_malloc_nokey(size * sizeof *index_ids));

	counts = static_cast<ulint*>(ut_malloc_nokey(sizeof(ulint) * size));

	mutex_enter(&mutex);
	mutex_enter(&flush_list_mutex);

	ib::info()
		<< "[buffer pool: size=" << curr_size
		<< ", database pages=" << UT_LIST_GET_LEN(LRU)
		<< ", free pages=" << UT_LIST_GET_LEN(free)
		<< ", modified database pages="
		<< UT_LIST_GET_LEN(flush_list)
		<< ", n pending decompressions=" << n_pend_unzip
		<< ", n pending reads=" << n_pend_reads
		<< ", n pending flush LRU=" << n_flush[BUF_FLUSH_LRU]
		<< " list=" << n_flush[BUF_FLUSH_LIST]
		<< " single page=" << n_flush[BUF_FLUSH_SINGLE_PAGE]
		<< ", pages made young=" << stat.n_pages_made_young
		<< ", not young=" << stat.n_pages_not_made_young
		<< ", pages read=" << stat.n_pages_read
		<< ", created=" << stat.n_pages_created
		<< ", written=" << stat.n_pages_written << "]";

	mutex_exit(&flush_list_mutex);

	/* Count the number of blocks belonging to each index in the buffer */

	n_found = 0;

	chunk = chunks;

	for (i = n_chunks; i--; chunk++) {
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

	mutex_exit(&mutex);

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

	validate();
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

#ifdef UNIV_DEBUG
/** @return the number of latched pages in the buffer pool */
ulint buf_get_latched_pages_number()
{
	buf_page_t*	b;
	ulint		i;
	ulint		fixed_pages_number = 0;

	mutex_enter(&buf_pool.mutex);

	auto chunk = buf_pool.chunks;

	for (i = buf_pool.n_chunks; i--; chunk++) {
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

	mutex_enter(&buf_pool.zip_mutex);

	/* Traverse the lists of clean and dirty compressed-only blocks. */

	for (b = UT_LIST_GET_FIRST(buf_pool.zip_clean); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_a(buf_page_get_state(b) == BUF_BLOCK_ZIP_PAGE);
		ut_a(buf_page_get_io_fix(b) != BUF_IO_WRITE);

		if (b->buf_fix_count != 0
		    || buf_page_get_io_fix(b) != BUF_IO_NONE) {
			fixed_pages_number++;
		}
	}

	mutex_enter(&buf_pool.flush_list_mutex);
	for (b = UT_LIST_GET_FIRST(buf_pool.flush_list); b;
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

	mutex_exit(&buf_pool.flush_list_mutex);
	mutex_exit(&buf_pool.zip_mutex);
	mutex_exit(&buf_pool.mutex);

	return(fixed_pages_number);
}
#endif /* UNIV_DEBUG */

/** Collect buffer pool metadata.
@param[out]	pool_info	buffer pool metadata */
void buf_stats_get_pool_info(buf_pool_info_t *pool_info)
{
	time_t			current_time;
	double			time_elapsed;

	mutex_enter(&buf_pool.mutex);
	mutex_enter(&buf_pool.flush_list_mutex);

	pool_info->pool_size = buf_pool.curr_size;

	pool_info->lru_len = UT_LIST_GET_LEN(buf_pool.LRU);

	pool_info->old_lru_len = buf_pool.LRU_old_len;

	pool_info->free_list_len = UT_LIST_GET_LEN(buf_pool.free);

	pool_info->flush_list_len = UT_LIST_GET_LEN(buf_pool.flush_list);

	pool_info->n_pend_unzip = UT_LIST_GET_LEN(buf_pool.unzip_LRU);

	pool_info->n_pend_reads = buf_pool.n_pend_reads;

	pool_info->n_pending_flush_lru =
		 (buf_pool.n_flush[BUF_FLUSH_LRU]
		  + buf_pool.init_flush[BUF_FLUSH_LRU]);

	pool_info->n_pending_flush_list =
		 (buf_pool.n_flush[BUF_FLUSH_LIST]
		  + buf_pool.init_flush[BUF_FLUSH_LIST]);

	pool_info->n_pending_flush_single_page =
		 (buf_pool.n_flush[BUF_FLUSH_SINGLE_PAGE]
		  + buf_pool.init_flush[BUF_FLUSH_SINGLE_PAGE]);

	mutex_exit(&buf_pool.flush_list_mutex);

	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time,
					buf_pool.last_printout_time);

	pool_info->n_pages_made_young = buf_pool.stat.n_pages_made_young;

	pool_info->n_pages_not_made_young =
		buf_pool.stat.n_pages_not_made_young;

	pool_info->n_pages_read = buf_pool.stat.n_pages_read;

	pool_info->n_pages_created = buf_pool.stat.n_pages_created;

	pool_info->n_pages_written = buf_pool.stat.n_pages_written;

	pool_info->n_page_gets = buf_pool.stat.n_page_gets;

	pool_info->n_ra_pages_read_rnd = buf_pool.stat.n_ra_pages_read_rnd;
	pool_info->n_ra_pages_read = buf_pool.stat.n_ra_pages_read;

	pool_info->n_ra_pages_evicted = buf_pool.stat.n_ra_pages_evicted;

	pool_info->page_made_young_rate =
	static_cast<double>(buf_pool.stat.n_pages_made_young
			    - buf_pool.old_stat.n_pages_made_young)
	/ time_elapsed;

	pool_info->page_not_made_young_rate =
	static_cast<double>(buf_pool.stat.n_pages_not_made_young
			    - buf_pool.old_stat.n_pages_not_made_young)
	/ time_elapsed;

	pool_info->pages_read_rate =
	static_cast<double>(buf_pool.stat.n_pages_read
			    - buf_pool.old_stat.n_pages_read)
	/ time_elapsed;

	pool_info->pages_created_rate =
	static_cast<double>(buf_pool.stat.n_pages_created
			    - buf_pool.old_stat.n_pages_created)
	/ time_elapsed;

	pool_info->pages_written_rate =
	static_cast<double>(buf_pool.stat.n_pages_written
			    - buf_pool.old_stat.n_pages_written)
	/ time_elapsed;

	pool_info->n_page_get_delta = buf_pool.stat.n_page_gets
				      - buf_pool.old_stat.n_page_gets;

	if (pool_info->n_page_get_delta) {
		pool_info->page_read_delta = buf_pool.stat.n_pages_read
					     - buf_pool.old_stat.n_pages_read;

		pool_info->young_making_delta =
			buf_pool.stat.n_pages_made_young
			- buf_pool.old_stat.n_pages_made_young;

		pool_info->not_young_making_delta =
			buf_pool.stat.n_pages_not_made_young
			- buf_pool.old_stat.n_pages_not_made_young;
	}
	pool_info->pages_readahead_rnd_rate =
	static_cast<double>(buf_pool.stat.n_ra_pages_read_rnd
			    - buf_pool.old_stat.n_ra_pages_read_rnd)
	/ time_elapsed;


	pool_info->pages_readahead_rate =
	static_cast<double>(buf_pool.stat.n_ra_pages_read
			    - buf_pool.old_stat.n_ra_pages_read)
	/ time_elapsed;

	pool_info->pages_evicted_rate =
	static_cast<double>(buf_pool.stat.n_ra_pages_evicted
			    - buf_pool.old_stat.n_ra_pages_evicted)
	/ time_elapsed;

	pool_info->unzip_lru_len = UT_LIST_GET_LEN(buf_pool.unzip_LRU);

	pool_info->io_sum = buf_LRU_stat_sum.io;

	pool_info->io_cur = buf_LRU_stat_cur.io;

	pool_info->unzip_sum = buf_LRU_stat_sum.unzip;

	pool_info->unzip_cur = buf_LRU_stat_cur.unzip;

	buf_refresh_io_stats();
	mutex_exit(&buf_pool.mutex);
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
		static_cast<double>(pool_info->flush_list_len)
		/ (static_cast<double>(pool_info->lru_len
				       + pool_info->free_list_len) + 1.0)
		* 100.0,
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
		double hit_rate = static_cast<double>(
			pool_info->page_read_delta)
			/ static_cast<double>(pool_info->n_page_get_delta);

		if (hit_rate > 1) {
			hit_rate = 1;
		}

		fprintf(file,
			"Buffer pool hit rate " ULINTPF " / 1000,"
			" young-making rate " ULINTPF " / 1000 not "
			ULINTPF " / 1000\n",
			ulint(1000 * (1 - hit_rate)),
			ulint(1000
			      * double(pool_info->young_making_delta)
			      / double(pool_info->n_page_get_delta)),
			ulint(1000 * double(pool_info->not_young_making_delta)
			      / double(pool_info->n_page_get_delta)));
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
	buf_pool_info_t	pool_info;

	buf_stats_get_pool_info(&pool_info);
	buf_print_io_instance(&pool_info, file);
}

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param[in]	page		page frame
@param[in]	fsp_flags	tablespace flags
@return true if true if page is encrypted and OK, false otherwise */
bool buf_page_verify_crypt_checksum(const byte* page, ulint fsp_flags)
{
	if (!fil_space_t::full_crc32(fsp_flags)) {
		return fil_space_verify_crypt_checksum(
			page, fil_space_t::zip_size(fsp_flags));
	}

	return !buf_page_is_corrupted(true, page, fsp_flags);
}

/** Checks that there currently are no I/O operations pending.
@return number of pending i/o */
ulint buf_pool_check_no_pending_io()
{
	/* FIXME: use atomics, no mutex */
	ulint pending_io = buf_pool.n_pend_reads;
	mutex_enter(&buf_pool.mutex);
	pending_io +=
		+ buf_pool.n_flush[BUF_FLUSH_LRU]
		+ buf_pool.n_flush[BUF_FLUSH_SINGLE_PAGE]
		+ buf_pool.n_flush[BUF_FLUSH_LIST];
	mutex_exit(&buf_pool.mutex);

	return(pending_io);
}

/** Print the given page_id_t object.
@param[in,out]	out	the output stream
@param[in]	page_id	the page_id_t object to be printed
@return the output stream */
std::ostream& operator<<(std::ostream &out, const page_id_t page_id)
{
  out << "[page id: space=" << page_id.space()
      << ", page number=" << page_id.page_no() << "]";
  return out;
}

/**
Should we punch hole to deallocate unused portion of the page.
@param[in]	bpage		Page control block
@return true if punch hole should be used, false if not */
bool
buf_page_should_punch_hole(
	const buf_page_t* bpage)
{
	return bpage->real_size != bpage->physical_size();
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
	return bpage->physical_size() - write_length;
}
#endif /* !UNIV_INNOCHECKSUM */
