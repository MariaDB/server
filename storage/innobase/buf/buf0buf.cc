/*****************************************************************************

Copyright (c) 1995, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2023, MariaDB Corporation.

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
#include "buf0checksum.h"
#include "mariadb_stats.h"
#include <string.h>

#ifdef UNIV_INNOCHECKSUM
# include "my_sys.h"
# include "buf0buf.h"
#else
#include "my_cpu.h"
#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "buf0rea.h"
#include "buf0flu.h"
#include "buf0buddy.h"
#include "buf0dblwr.h"
#include "lock0lock.h"
#include "btr0sea.h"
#include "log0log.h"
#include "dict0stats_bg.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "srv0mon.h"
#include "log0crypt.h"
#include "fil0pagecompress.h"
#endif /* !UNIV_INNOCHECKSUM */
#include "page0zip.h"
#include "buf0dump.h"
#include <map>
#include <sstream>
#include "log.h"
#include "my_virtual_mem.h"

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
			MEM_MAKE_DEFINED(numa_mems_allowed,
					 sizeof *numa_mems_allowed);
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
only if the predicate block->page.belongs_to_unzip_LRU()
holds.  The blocks in unzip_LRU will be in same order as they are in
the common LRU list.  That is, each manipulation of the common LRU
list will result in the same manipulation of the unzip_LRU list.

The chain of modified blocks (buf_pool.flush_list) contains the blocks
holding persistent file pages that have been modified in the memory
but not written to disk yet. The block with the oldest modification
which has not yet been written to disk is at the end of the chain.
The access to this list is protected by buf_pool.flush_list_mutex.

The control blocks for uncompressed pages are accessible via
buf_block_t objects that are reachable via buf_pool.chunks[].
The control blocks (buf_page_t) of those ROW_FORMAT=COMPRESSED pages
that are not in buf_pool.flush_list and for which no uncompressed
page has been allocated in buf_pool are only accessible via
buf_pool.LRU.

The chains of free memory blocks (buf_pool.zip_free[]) are used by
the buddy allocator (buf0buddy.cc) to keep track of currently unused
memory blocks of size 1024..innodb_page_size / 2.  These
blocks are inside the memory blocks of size innodb_page_size and type
BUF_BLOCK_MEMORY that the buddy allocator requests from the buffer
pool.  The buddy allocator is solely used for allocating
ROW_FORMAT=COMPRESSED page frames.

		Loading a file page
		-------------------

First, a victim block for replacement has to be found in the
buf_pool. It is taken from the free list or searched for from the
end of the LRU-list. An exclusive lock is reserved for the frame,
the io_fix is set in the block fixing the block in buf_pool,
and the io-operation for loading the page is queued. The io-handler thread
releases the X-lock on the frame and releases the io_fix
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
/** Compute the number of page frames needed for buf_block_t,
per innodb_buffer_pool_extent_size.
@param ps      innodb_page_size
@return number of buf_block_t frames per extent */
static constexpr uint8_t first_page(size_t ps)
{
  return uint8_t(innodb_buffer_pool_extent_size / ps -
                 innodb_buffer_pool_extent_size / (ps + sizeof(buf_block_t)));
}

/** Compute the number of bytes needed for buf_block_t,
per innodb_buffer_pool_extent_size.
@param ps      innodb_page_size
@return number of buf_block_t frames per extent */
static constexpr size_t first_frame(size_t ps)
{
  return first_page(ps) * ps;
}

/** Compute the number of pages per innodb_buffer_pool_extent_size.
@param ps      innodb_page_size
@return number of buf_block_t frames per extent */
static constexpr uint16_t pages(size_t ps)
{
  return uint16_t(innodb_buffer_pool_extent_size / ps - first_page(ps));
}

/** The byte offset of the first page frame in a buffer pool extent
of innodb_buffer_pool_extent_size bytes */
static constexpr size_t first_frame_in_extent[]=
{
  first_frame(4096), first_frame(8192), first_frame(16384),
  first_frame(32768), first_frame(65536)
};

/** The position offset of the first page frame in a buffer pool extent
of innodb_buffer_pool_extent_size bytes */
static constexpr uint8_t first_page_in_extent[]=
{
  first_page(4096), first_page(8192), first_page(16384),
  first_page(32768), first_page(65536)
};

/** Number of pages per buffer pool extent
of innodb_buffer_pool_extent_size bytes */
static constexpr size_t pages_in_extent[]=
{
  pages(4096), pages(8192), pages(16384), pages(32768), pages(65536)
};

# ifdef SUX_LOCK_GENERIC
void page_hash_latch::read_lock_wait() noexcept
{
  /* First, try busy spinning for a while. */
  for (auto spin= srv_n_spin_wait_rounds; spin--; )
  {
    LF_BACKOFF();
    if (read_trylock())
      return;
  }
  /* Fall back to yielding to other threads. */
  do
    std::this_thread::yield();
  while (!read_trylock());
}

void page_hash_latch::write_lock_wait() noexcept
{
  write_lock_wait_start();

  /* First, try busy spinning for a while. */
  for (auto spin= srv_n_spin_wait_rounds; spin--; )
  {
    if (write_lock_poll())
      return;
    LF_BACKOFF();
  }

  /* Fall back to yielding to other threads. */
  do
    std::this_thread::yield();
  while (!write_lock_poll());
}
# endif

/** Number of attempts made to read in a page in the buffer pool */
constexpr ulint	BUF_PAGE_READ_MAX_RETRIES= 100;
/** The maximum portion of the buffer pool that can be used for the
read-ahead buffer.  (Divide buf_pool size by this amount) */
constexpr uint32_t BUF_READ_AHEAD_PORTION= 32;

/** A 64KiB buffer of NUL bytes, for use in assertions and checks,
and dummy default values of instantly dropped columns.
Initially, BLOB field references are set to NUL bytes, in
dtuple_convert_big_rec(). */
const byte *field_ref_zero;

/** The InnoDB buffer pool */
buf_pool_t buf_pool;

#ifdef UNIV_DEBUG
/** This is used to insert validation operations in execution
in the debug version */
static Atomic_counter<size_t> buf_dbg_counter;
#endif /* UNIV_DEBUG */

/** Macro to determine whether the read of write counter is used depending
on the io_type */
#define MONITOR_RW_COUNTER(read, counter)		\
	(read ? (counter##_READ) : (counter##_WRITTEN))

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

	memcpy_aligned<UNIV_PAGE_SIZE_MIN>(src_frame, tmp_frame,
					   srv_page_size);
	srv_stats.pages_decrypted.inc();
	srv_stats.n_temp_blocks_decrypted.inc();

	return true; /* page was decrypted */
}

/** Decrypt a page.
@param[in,out]	bpage	Page control block
@param[in]	node	data file
@return whether the operation was successful */
static bool buf_page_decrypt_after_read(buf_page_t *bpage,
                                        const fil_node_t &node)
{
	ut_ad(node.space->referenced());
	ut_ad(node.space->id == bpage->id().space());
	const auto flags = node.space->flags;

	byte* dst_frame = bpage->zip.data ? bpage->zip.data : bpage->frame;
	bool page_compressed = node.space->is_compressed()
		&& buf_page_is_compressed(dst_frame, flags);
	const page_id_t id(bpage->id());

	if (id.page_no() == 0) {
		/* File header pages are not encrypted/compressed */
		return (true);
	}

	buf_tmp_buffer_t* slot;

	if (id.space() == SRV_TMP_SPACE_ID
	    && innodb_encrypt_temporary_tables) {
		slot = buf_pool.io_buf_reserve(false);
		slot->allocate();
		bool ok = buf_tmp_page_decrypt(slot->crypt_buf, dst_frame);
		slot->release();
		return ok;
	}

	/* Page is encrypted if encryption information is found from
	tablespace and page contains used key_version. This is true
	also for pages first compressed and then encrypted. */

	uint key_version = buf_page_get_key_version(dst_frame, flags);

	if (page_compressed && !key_version) {
		/* the page we read is unencrypted */
		/* Find free slot from temporary memory array */
decompress:
		if (fil_space_t::full_crc32(flags)
		    && buf_page_is_corrupted(true, dst_frame, flags)) {
			return false;
		}

		slot = buf_pool.io_buf_reserve(false);
		slot->allocate();

decompress_with_slot:
		ulint write_size = fil_page_decompress(
			slot->crypt_buf, dst_frame, flags);
		slot->release();
		ut_ad(node.space->referenced());
		return write_size != 0;
	}

	if (key_version && node.space->crypt_data) {
		/* Verify encryption checksum before we even try to
		decrypt. */
		if (!buf_page_verify_crypt_checksum(dst_frame, flags)) {
decrypt_failed:
			ib::error() << "Encrypted page " << id
				    << " in file " << node.name
				    << " looks corrupted; key_version="
				    << key_version;
			return false;
		}

		slot = buf_pool.io_buf_reserve(false);
		slot->allocate();

		/* decrypt using crypt_buf to dst_frame */
		if (!fil_space_decrypt(node.space, slot->crypt_buf, dst_frame)) {
			slot->release();
			goto decrypt_failed;
		}

		if ((fil_space_t::full_crc32(flags) && page_compressed)
		    || fil_page_get_type(dst_frame)
		    == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
			goto decompress_with_slot;
		}

		slot->release();
	} else if (fil_page_get_type(dst_frame)
		   == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		goto decompress;
	}

	ut_ad(node.space->referenced());
	return true;
}
#endif /* !UNIV_INNOCHECKSUM */

/** Checks if the page is in crc32 checksum format.
@param[in]	read_buf		database page
@param[in]	checksum_field1		new checksum field
@param[in]	checksum_field2		old checksum field
@return true if the page is in crc32 checksum format. */
static
bool
buf_page_is_checksum_valid_crc32(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2) noexcept
{
	const uint32_t	crc32 = buf_calc_page_crc32(read_buf);

#ifdef UNIV_INNOCHECKSUM
	extern FILE* log_file;
	extern uint32_t cur_page_num;
	if (log_file) {
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

	return checksum_field1 == crc32;
}

#ifndef UNIV_INNOCHECKSUM
/** Check whether a page is newer than the durable LSN.
@param check_lsn    whether to check the LSN
@param read_buf     page frame
@return whether the FIL_PAGE_LSN is invalid (ahead of the durable LSN) */
static bool buf_page_check_lsn(bool check_lsn, const byte *read_buf) noexcept
{
  if (!check_lsn)
    return false;
  /* A page may not be read before it is written, and it may not be
  written before the corresponding log has been durably written.
  Hence, we refer to the current durable LSN here */
  lsn_t current_lsn= log_sys.get_flushed_lsn(std::memory_order_relaxed);
  if (UNIV_UNLIKELY(current_lsn == log_sys.FIRST_LSN) &&
      srv_force_recovery == SRV_FORCE_NO_LOG_REDO)
    return false;
  const lsn_t page_lsn= mach_read_from_8(read_buf + FIL_PAGE_LSN);

  if (UNIV_LIKELY(current_lsn >= page_lsn))
    return false;

  const uint32_t space_id= mach_read_from_4(read_buf + FIL_PAGE_SPACE_ID);
  const uint32_t page_no= mach_read_from_4(read_buf + FIL_PAGE_OFFSET);

  sql_print_error("InnoDB: Page "
                  "[page id: space=" UINT32PF ", page number=" UINT32PF "]"
                  " log sequence number " LSN_PF
                  " is in the future! Current system log sequence number "
                  LSN_PF ".",
                  space_id, page_no, page_lsn, current_lsn);

  if (srv_force_recovery)
    return false;

  sql_print_error("InnoDB: Your database may be corrupt or"
                  " you may have copied the InnoDB"
                  " tablespace but not the ib_logfile0. %s",
                  FORCE_RECOVERY_MSG);

  return true;
}
#endif


/** Check if a buffer is all zeroes.
@param[in]	buf	data to check
@return whether the buffer is all zeroes */
bool buf_is_zeroes(span<const byte> buf) noexcept
{
  ut_ad(buf.size() <= UNIV_PAGE_SIZE_MAX);
  return memcmp(buf.data(), field_ref_zero, buf.size()) == 0;
}

/** Check if a page is corrupt.
@param check_lsn   whether FIL_PAGE_LSN should be checked
@param read_buf    database page
@param fsp_flags   contents of FIL_SPACE_FLAGS
@return whether the page is corrupted */
buf_page_is_corrupted_reason
buf_page_is_corrupted(bool check_lsn, const byte *read_buf, uint32_t fsp_flags) noexcept
{
	if (fil_space_t::full_crc32(fsp_flags)) {
		bool compressed = false, corrupted = false;
		const uint size = buf_page_full_crc32_size(
			read_buf, &compressed, &corrupted);
		if (corrupted) {
			return CORRUPTED_OTHER;
		}
		const byte* end = read_buf + (size - FIL_PAGE_FCRC32_CHECKSUM);
		uint crc32 = mach_read_from_4(end);

		if (!crc32 && size == srv_page_size
		    && buf_is_zeroes(span<const byte>(read_buf, size))) {
			return NOT_CORRUPTED;
		}

		DBUG_EXECUTE_IF(
			"page_intermittent_checksum_mismatch", {
			static int page_counter;
			if (mach_read_from_4(FIL_PAGE_OFFSET + read_buf)
			    && page_counter++ == 6) {
				crc32++;
			}
		});

		if (crc32 != my_crc32c(0, read_buf,
				       size - FIL_PAGE_FCRC32_CHECKSUM)) {
			return CORRUPTED_OTHER;
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
			return CORRUPTED_OTHER;
		}

		return
#ifndef UNIV_INNOCHECKSUM
			buf_page_check_lsn(check_lsn, read_buf)
			? CORRUPTED_FUTURE_LSN :
#endif
			NOT_CORRUPTED;
	}

	const ulint zip_size = fil_space_t::zip_size(fsp_flags);
	const uint16_t page_type = fil_page_get_type(read_buf);

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
	check_lsn:
		return
#ifndef UNIV_INNOCHECKSUM
			buf_page_check_lsn(check_lsn, read_buf)
			? CORRUPTED_FUTURE_LSN :
#endif
			NOT_CORRUPTED;
	}

	static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
	static_assert(FIL_PAGE_END_LSN_OLD_CHKSUM % 4 == 0, "alignment");

	if (!zip_size
	    && memcmp_aligned<4>(read_buf + FIL_PAGE_LSN + 4,
				 read_buf + srv_page_size
				 - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4)) {
		/* Stored log sequence numbers at the start and the end
		of page do not match */

		return CORRUPTED_OTHER;
	}

	/* Check whether the checksum fields have correct values */

	if (zip_size) {
		if (!page_zip_verify_checksum(read_buf, zip_size)) {
			return CORRUPTED_OTHER;
		}
		goto check_lsn;
	}

	const uint32_t checksum_field1 = mach_read_from_4(
		read_buf + FIL_PAGE_SPACE_OR_CHKSUM);

	const uint32_t checksum_field2 = mach_read_from_4(
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
			return NOT_CORRUPTED;
		}
	}

#ifndef UNIV_INNOCHECKSUM
	switch (srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
#endif /* !UNIV_INNOCHECKSUM */
		if (!buf_page_is_checksum_valid_crc32(read_buf,
						      checksum_field1,
						      checksum_field2)) {
			return CORRUPTED_OTHER;
		}
		goto check_lsn;
#ifndef UNIV_INNOCHECKSUM
	default:
		if (checksum_field1 == BUF_NO_CHECKSUM_MAGIC
		    && checksum_field2 == BUF_NO_CHECKSUM_MAGIC) {
			goto check_lsn;
		}

		const uint32_t crc32 = buf_calc_page_crc32(read_buf);

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
				if (mach_read_from_4(FIL_PAGE_OFFSET
						     + read_buf)
				    && page_counter++ == 6)
					return CORRUPTED_OTHER;
			});

			if ((checksum_field1 != crc32
			     || checksum_field2 != crc32)
			    && checksum_field2
			    != buf_calc_page_old_checksum(read_buf)) {
				return CORRUPTED_OTHER;
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
				return CORRUPTED_OTHER;
			}
		}
	}
#endif /* !UNIV_INNOCHECKSUM */
	goto check_lsn;
}

#ifndef UNIV_INNOCHECKSUM

#ifdef __linux__
#include <poll.h>
#include <sys/eventfd.h>
#include <fstream>

/** Memory Pressure

based off https://www.kernel.org/doc/html/latest/accounting/psi.html#pressure-interface
and https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html#memory */
class mem_pressure
{
  /* triggers + eventfd */
  struct pollfd m_fds[3];
  nfds_t m_num_fds;
  int m_event_fd= -1;
  Atomic_relaxed<bool> m_abort= false;

  std::thread m_thd;
  /* mem pressure garbage collection restricted to interval */
  static constexpr ulonglong max_interval_us= 60*1000000;

public:
  mem_pressure() : m_num_fds(0) {}

  bool setup()
  {
    m_num_fds= 0;

    if (my_use_large_pages)
      return false;

    static_assert(array_elements(m_fds) == (array_elements(m_triggers) + 1),
                  "insufficient fds");
    std::string memcgroup{"/sys/fs/cgroup"};
    std::string cgroup;
    {
      std::ifstream selfcgroup("/proc/self/cgroup");
      std::getline(selfcgroup, cgroup, '\n');
    }

    cgroup.erase(0, 3); // Remove "0::"
    memcgroup+= cgroup + "/memory.pressure";

    for (auto trig= std::begin(m_triggers); trig!= std::end(m_triggers); ++trig)
    {
      if ((m_fds[m_num_fds].fd=
             open(memcgroup.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0)
      {
        /* User can't do anything about it, no point giving warning */
        shutdown();
        return false;
      }
      my_register_filename(m_fds[m_num_fds].fd, memcgroup.c_str(), FILE_BY_OPEN, 0, MYF(0));
      ssize_t slen= strlen(*trig);
      if (write(m_fds[m_num_fds].fd, *trig, slen) < slen)
      {
        /* we may fail this one, but continue to the next */
        my_close(m_fds[m_num_fds].fd, MYF(MY_WME));
        continue;
      }
      m_fds[m_num_fds].events= POLLPRI;
      m_num_fds++;
    }
    if (m_num_fds < 1)
      return false;

    if ((m_event_fd= eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK)) == -1)
    {
      /* User can't do anything about it, no point giving warning */
      shutdown();
      return false;
    }
    my_register_filename(m_event_fd, "mem_pressure_eventfd", FILE_BY_DUP, 0, MYF(0));
    m_fds[m_num_fds].fd= m_event_fd;
    m_fds[m_num_fds].events= POLLIN;
    m_num_fds++;
    m_thd= std::thread(pressure_routine, this);
    sql_print_information("InnoDB: Initialized memory pressure event listener");
    return true;
  }

  void shutdown()
  {
    /* m_event_fd is in this list */
    while (m_num_fds)
    {
      m_num_fds--;
      my_close(m_fds[m_num_fds].fd, MYF(MY_WME));
      m_fds[m_num_fds].fd= -1;
    }
    m_event_fd= -1;
  }

  static void pressure_routine(mem_pressure *m);

#ifdef UNIV_DEBUG
  void trigger_collection()
  {
    uint64_t u= 1;
    if (m_event_fd < 0 || write(m_event_fd, &u, sizeof(uint64_t)) != sizeof(uint64_t))
      sql_print_information("InnoDB: (Debug) Failed to trigger memory pressure");
  }
#endif

  void quit()
  {
    uint64_t u= 1;
    m_abort= true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    /* return result ignored, cannot do anything with it */
    write(m_event_fd, &u, sizeof(uint64_t));
#pragma GCC diagnostic pop
  }

  void join()
  {
    if (m_thd.joinable())
    {
      quit();
      m_thd.join();
    }
  }

  static const char* const m_triggers[2];
};


/*
  ref: https://docs.kernel.org/accounting/psi.html
  maximum window size (second number) 10 seconds.
  window size in multiples of 2 second interval required (for Unprivileged)
  Time is in usec.
*/
const char* const mem_pressure::m_triggers[]=
  {"some 5000000 10000000", /* 5s out of 10s */
   "full 10000 2000000"}; /* 10ms out of 2s */

static mem_pressure mem_pressure_obj;

void mem_pressure::pressure_routine(mem_pressure *m)
{
  DBUG_ASSERT(m == &mem_pressure_obj);
  if (my_thread_init())
  {
    m->shutdown();
    return;
  }

  ulonglong last= microsecond_interval_timer() - max_interval_us;
  while (!m->m_abort)
  {
    if (poll(&m->m_fds[0], m->m_num_fds, -1) < 0)
    {
      if (errno == EINTR)
        continue;
      else
        break;
    }
    if (m->m_abort)
      break;

    for (pollfd &p : st_::span<pollfd>(m->m_fds, m->m_num_fds))
    {
      if (p.revents & POLLPRI)
      {
        ulonglong now= microsecond_interval_timer();
        if ((now - last) > max_interval_us)
        {
          last= now;
          buf_pool.garbage_collect();
        }
      }

#ifdef UNIV_DEBUG
      if (p.revents & POLLIN)
      {
        uint64_t u;
        /* we haven't aborted, so this must be a debug trigger */
        if (read(p.fd, &u, sizeof(u)) >=0)
          buf_pool.garbage_collect();
      }
#endif
    }
  }
  m->shutdown();

  my_thread_end();
}

/** Initialize mem pressure. */
ATTRIBUTE_COLD static void buf_mem_pressure_detect_init() noexcept
{
  mem_pressure_obj.setup();
}

ATTRIBUTE_COLD void buf_mem_pressure_shutdown() noexcept
{
  mem_pressure_obj.join();
}
#endif

#if defined __linux__ || !defined DBUG_OFF
inline void buf_pool_t::garbage_collect() noexcept
{
  mysql_mutex_lock(&mutex);
  const size_t old_size{size_in_bytes}, min_size{size_in_bytes_auto_min};
  const size_t reduce_size=
    std::max(innodb_buffer_pool_extent_size,
             ut_calc_align((old_size - min_size) / 2,
                           innodb_buffer_pool_extent_size));
  if (old_size < min_size + reduce_size ||
      first_to_withdraw || old_size != size_in_bytes_requested)
  {
    mysql_mutex_unlock(&mutex);
    sql_print_information("InnoDB: Memory pressure event disregarded;"
                          " innodb_buffer_pool_size=%zum,"
                          " innodb_buffer_pool_size_auto_min=%zum",
                          old_size >> 20, min_size >> 20);
    return;
  }

  size_t size= old_size - reduce_size;
  size_t n_blocks_new= get_n_blocks(size);

  ut_ad(UT_LIST_GET_LEN(withdrawn) == 0);
  ut_ad(n_blocks_to_withdraw == 0);

  n_blocks_to_withdraw= n_blocks - n_blocks_new;
  first_to_withdraw= &get_nth_page(n_blocks_new)->page;

  size_in_bytes_requested= size;
  mysql_mutex_unlock(&mutex);
  mysql_mutex_lock(&flush_list_mutex);
  page_cleaner_wakeup(true);
  my_cond_wait(&done_flush_list, &flush_list_mutex.m_mutex);
  mysql_mutex_unlock(&flush_list_mutex);
# ifdef BTR_CUR_HASH_ADAPT
  bool ahi_disabled= btr_search_disable();
# endif /* BTR_CUR_HASH_ADAPT */
  time_t start= time(nullptr);
  mysql_mutex_lock(&mutex);

  do
  {
    if (shrink(size))
    {
      const size_t old_blocks{n_blocks};
      n_blocks= n_blocks_new;

      size_t s= n_blocks_new / BUF_READ_AHEAD_PORTION;
      read_ahead_area= s >= READ_AHEAD_PAGES
        ? READ_AHEAD_PAGES
        : my_round_up_to_next_power(uint32(s));

      os_total_large_mem_allocated-= reduce_size;
      shrunk(size, reduce_size);
# ifdef BTR_CUR_HASH_ADAPT
      if (ahi_disabled)
        btr_search_enable(true);
# endif
      mysql_mutex_unlock(&mutex);
      sql_print_information("InnoDB: Memory pressure event shrunk"
                            " innodb_buffer_pool_size=%zum (%zu pages)"
                            " from %zum (%zu pages)",
                            size >> 20, n_blocks_new, old_size >> 20,
                            old_blocks);
      ut_d(validate());
      return;
    }
  }
  while (time(nullptr) - start < 15);

  ut_ad(size_in_bytes > size_in_bytes_requested);
  n_blocks_to_withdraw= 0;
  first_to_withdraw= nullptr;
  size_in_bytes_requested= size_in_bytes;

  while (buf_page_t *b= UT_LIST_GET_FIRST(withdrawn))
  {
    UT_LIST_REMOVE(withdrawn, b);
    UT_LIST_ADD_LAST(free, b);
    ut_d(b->in_free_list= true);
    ut_ad(b->state() == buf_page_t::NOT_USED);
    b->lock.init();
  }

  mysql_mutex_unlock(&mutex);
  sql_print_information("InnoDB: Memory pressure event failed to shrink"
                        " innodb_buffer_pool_size=%zum", old_size);
  ut_d(validate());
}
#endif

#if defined(DBUG_OFF) && defined(HAVE_MADVISE) &&  defined(MADV_DODUMP)
/** Enable buffers to be dumped to core files.

A convenience function, not called anyhwere directly however
it is left available for gdb or any debugger to call
in the event that you want all of the memory to be dumped
to a core file.

@return number of errors found in madvise() calls */
MY_ATTRIBUTE((used))
int buf_pool_t::madvise_do_dump() noexcept
{
	int ret= 0;

	/* mirrors allocation in log_t::create() */
	if (log_sys.buf) {
		ret += madvise(log_sys.buf, log_sys.buf_size, MADV_DODUMP);
		ret += madvise(log_sys.flush_buf, log_sys.buf_size,
			       MADV_DODUMP);
	}

	ret+= madvise(buf_pool.memory, buf_pool.size_in_bytes, MADV_DODUMP);
	return ret;
}
#endif

#ifndef UNIV_DEBUG
static inline byte hex_to_ascii(byte hex_digit) noexcept
{
  const int offset= hex_digit <= 9 ? '0' : 'a' - 10;
  return byte(hex_digit + offset);
}
#endif

/** Dump a page to stderr.
@param[in]	read_buf	database page
@param[in]	zip_size	compressed page size, or 0 */
ATTRIBUTE_COLD
void buf_page_print(const byte *read_buf, ulint zip_size) noexcept
{
#ifndef UNIV_DEBUG
  const size_t size = zip_size ? zip_size : srv_page_size;
  const byte * const end= read_buf + size;
  sql_print_information("InnoDB: Page dump (%zu bytes):", size);

  do
  {
    byte row[64];

    for (byte *r= row; r != &row[64]; r+= 2, read_buf++)
    {
      r[0]= hex_to_ascii(byte(*read_buf >> 4));
      r[1]= hex_to_ascii(*read_buf & 15);
    }

    sql_print_information("InnoDB: %.*s", 64, row);
  }
  while (read_buf != end);

  sql_print_information("InnoDB: End of page dump");
#endif
}

IF_DBUG(,inline) byte *buf_block_t::frame_address() const noexcept
{
  static_assert(ut_is_2pow(innodb_buffer_pool_extent_size), "");

  byte *frame_= reinterpret_cast<byte*>
    ((reinterpret_cast<size_t>(this) & ~(innodb_buffer_pool_extent_size - 1)) |
     first_frame_in_extent[srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN]);
  ut_ad(reinterpret_cast<const byte*>(this) + sizeof(*this) <= frame_);
  frame_+=
    (((reinterpret_cast<size_t>(this) & (innodb_buffer_pool_extent_size - 1)) /
      sizeof(*this)) << srv_page_size_shift);
  return frame_;
}

buf_block_t *buf_pool_t::block_from(const void *ptr) noexcept
{
  static_assert(ut_is_2pow(innodb_buffer_pool_extent_size), "");
  ut_ad(static_cast<const char*>(ptr) >= buf_pool.memory);

  byte *first_block= reinterpret_cast<byte*>
    (reinterpret_cast<size_t>(ptr) & ~(innodb_buffer_pool_extent_size - 1));
  const size_t first_frame=
    first_frame_in_extent[srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN];

  ut_ad(static_cast<const byte*>(ptr) >= first_block + first_frame);
  return reinterpret_cast<buf_block_t*>(first_block) +
    (((size_t(ptr) & (innodb_buffer_pool_extent_size - 1)) - first_frame) >>
     srv_page_size_shift);
}

/** Determine the address of the first invalid block descriptor
@param n_blocks   buf_pool.n_blocks
@return offset of the first invalid buf_block_t, relative to buf_pool.memory */
static size_t block_descriptors_in_bytes(size_t n_blocks) noexcept
{
  const size_t ssize= srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN;
  const size_t extent_size= pages_in_extent[ssize];
  return n_blocks / extent_size * innodb_buffer_pool_extent_size +
    (n_blocks % extent_size) * sizeof(buf_block_t);
}

buf_block_t *buf_pool_t::get_nth_page(size_t pos) const noexcept
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(pos < n_blocks);
  return reinterpret_cast<buf_block_t*>
    (memory + block_descriptors_in_bytes(pos));
}

buf_block_t *buf_pool_t::allocate() noexcept
{
  mysql_mutex_assert_owner(&mutex);

  while (buf_page_t *b= UT_LIST_GET_FIRST(free))
  {
    ut_ad(b->in_free_list);
    ut_d(b->in_free_list = FALSE);
    ut_ad(!b->oldest_modification());
    ut_ad(!b->in_LRU_list);
    ut_a(!b->in_file());
    UT_LIST_REMOVE(free, b);

    if (UNIV_LIKELY(!n_blocks_to_withdraw) || !withdraw(*b))
    {
      /* No adaptive hash index entries may point to a free block. */
      assert_block_ahi_empty(reinterpret_cast<buf_block_t*>(b));
      b->set_state(buf_page_t::MEMORY);
      b->set_os_used();
      return reinterpret_cast<buf_block_t*>(b);
    }
  }

  return nullptr;
}

/** Create the hash table.
@param n  the lower bound of n_cells */
void buf_pool_t::page_hash_table::create(ulint n) noexcept
{
  n_cells= ut_find_prime(n);
  const size_t size= MY_ALIGN(pad(n_cells) * sizeof *array,
                              CPU_LEVEL1_DCACHE_LINESIZE);
  void *v= aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
  memset_aligned<CPU_LEVEL1_DCACHE_LINESIZE>(v, 0, size);
  array= static_cast<hash_chain*>(v);
}

size_t buf_pool_t::get_n_blocks(size_t size_in_bytes) noexcept
{
  const size_t ssize= srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN;
  size_t n_blocks_alloc= size_in_bytes / innodb_buffer_pool_extent_size *
    pages_in_extent[ssize];

  if (const size_t incomplete_extent_pages=
      (size_in_bytes & (innodb_buffer_pool_extent_size - 1)) >>
      srv_page_size_shift)
  {
    ssize_t d= incomplete_extent_pages - first_page_in_extent[ssize];
    ut_ad(d > 0);
    n_blocks_alloc+= d;
  }

  return n_blocks_alloc;
}

size_t buf_pool_t::blocks_in_bytes(size_t n_blocks) noexcept
{
  const size_t shift{srv_page_size_shift};
  const size_t ssize{shift - UNIV_PAGE_SIZE_SHIFT_MIN};
  const size_t extent_size= pages_in_extent[ssize];
  size_t size_in_bytes= n_blocks / extent_size *
    innodb_buffer_pool_extent_size;
  if (size_t remainder= n_blocks % extent_size)
    size_in_bytes+= (remainder + first_page_in_extent[ssize]) << shift;
  ut_ad(get_n_blocks(size_in_bytes) == n_blocks);
  return size_in_bytes;
}

/** Create the buffer pool.
@return whether the creation failed */
bool buf_pool_t::create() noexcept
{
  ut_ad(this == &buf_pool);
  ut_ad(!is_initialised());
  ut_ad(size_in_bytes_requested > 0);
  ut_ad(!(size_in_bytes_max & (innodb_buffer_pool_extent_size - 1)));
  ut_ad(!(size_in_bytes_requested & ((1U << 20) - 1)));
  ut_ad(size_in_bytes_requested <= size_in_bytes_max);
  /* mariabackup loads tablespaces, and it requires field_ref_zero to be
  allocated before innodb initialization */
  ut_ad(srv_operation >= SRV_OPERATION_RESTORE || !field_ref_zero);

  if (!field_ref_zero)
  {
    if (auto b= aligned_malloc(UNIV_PAGE_SIZE_MAX, 4096))
    {
      field_ref_zero= static_cast<const byte*>
        (memset_aligned<4096>(b, 0, UNIV_PAGE_SIZE_MAX));
      goto init;
    }

  oom:
    ut_ad(!is_initialised());
    sql_print_error("InnoDB: Cannot map innodb_buffer_pool_size_max=%zum",
                    size_in_bytes_max >> 20);
    return true;
  }

 init:
  DBUG_EXECUTE_IF("ib_buf_chunk_init_fails", goto oom;);
  size_t size= size_in_bytes_max;
  sql_print_information("InnoDB: innodb_buffer_pool_size_max=%zum,"
                        " innodb_buffer_pool_size=%zum",
                        size >> 20, size_in_bytes_requested >> 20);

 retry:
  {
    NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;
#ifdef _WIN32
    memory_unaligned= my_virtual_mem_reserve(&size);
#else
    memory_unaligned= my_large_virtual_alloc(&size);
#endif
  }

  if (!memory_unaligned)
    goto oom;

  const size_t alignment_waste=
    ((~size_t(memory_unaligned) & (innodb_buffer_pool_extent_size - 1)) + 1) &
    (innodb_buffer_pool_extent_size - 1);

  if (size < size_in_bytes_max + alignment_waste)
  {
    my_virtual_mem_release(memory_unaligned, size);
    size+= 1 +
      (~size_t(memory_unaligned) & (innodb_buffer_pool_extent_size - 1));
    goto retry;
  }

  MEM_UNDEFINED(memory_unaligned, size);
  ut_dontdump(memory_unaligned, size, true);
  memory= memory_unaligned + alignment_waste;
  size_unaligned= size;
  size-= alignment_waste;
  size&= ~(innodb_buffer_pool_extent_size - 1);

  const size_t actual_size= size_in_bytes_requested;
  ut_ad(actual_size <= size);

  size_in_bytes= actual_size;
  os_total_large_mem_allocated+= actual_size;

#ifdef UNIV_PFS_MEMORY
  PSI_MEMORY_CALL(memory_alloc)(mem_key_buf_buf_pool, actual_size, &owner);
#endif
#ifdef _WIN32
  if (!my_virtual_mem_commit(memory, actual_size))
  {
    my_virtual_mem_release(memory_unaligned, size_unaligned);
    memory= nullptr;
    memory_unaligned= nullptr;
    goto oom;
  }
#else
  update_malloc_size(actual_size, 0);
#endif

#ifdef HAVE_LIBNUMA
  if (srv_numa_interleave)
  {
    struct bitmask *numa_mems_allowed= numa_get_mems_allowed();
    MEM_MAKE_DEFINED(numa_mems_allowed, sizeof *numa_mems_allowed);
    if (mbind(memory_unaligned, size_unaligned, MPOL_INTERLEAVE,
              numa_mems_allowed->maskp, numa_mems_allowed->size,
              MPOL_MF_MOVE))
      sql_print_warning("InnoDB: Failed to set NUMA memory policy of"
                        " buffer pool page frames to MPOL_INTERLEAVE"
                        " (error: %s).", strerror(errno));
    numa_bitmask_free(numa_mems_allowed);
  }
#endif /* HAVE_LIBNUMA */

  n_blocks= get_n_blocks(actual_size);
  n_blocks_to_withdraw= 0;
  UT_LIST_INIT(free, &buf_page_t::list);
  const size_t ssize= srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN;

  for (char *extent= memory,
         *end= memory + block_descriptors_in_bytes(n_blocks);
       extent < end; extent+= innodb_buffer_pool_extent_size)
  {
    buf_block_t *block= reinterpret_cast<buf_block_t*>(extent);
    const buf_block_t *extent_end= block + pages_in_extent[ssize];
    if (reinterpret_cast<const char*>(extent_end) > end)
      extent_end= reinterpret_cast<buf_block_t*>(end);
    MEM_MAKE_DEFINED(block, (extent_end - block) * sizeof *block);
    for (byte *frame= reinterpret_cast<byte*>(extent) +
           first_frame_in_extent[ssize];
         block < extent_end; block++, frame+= srv_page_size)
    {
      ut_ad(!memcmp(block, field_ref_zero, sizeof *block));
      block->page.frame= frame;
      block->page.lock.init();
      UT_LIST_ADD_LAST(free, &block->page);
      ut_d(block->page.in_free_list= true);
    }
  }

#if defined(__aarch64__)
  mysql_mutex_init(buf_pool_mutex_key, &mutex, MY_MUTEX_INIT_FAST);
#else
  mysql_mutex_init(buf_pool_mutex_key, &mutex, nullptr);
#endif

  UT_LIST_INIT(withdrawn, &buf_page_t::list);
  UT_LIST_INIT(LRU, &buf_page_t::LRU);
  UT_LIST_INIT(flush_list, &buf_page_t::list);
  UT_LIST_INIT(unzip_LRU, &buf_block_t::unzip_LRU);

  for (size_t i= 0; i < UT_ARR_SIZE(zip_free); ++i)
    UT_LIST_INIT(zip_free[i], &buf_buddy_free_t::list);
  ulint s= n_blocks;
  s/= BUF_READ_AHEAD_PORTION;
  read_ahead_area= s >= READ_AHEAD_PAGES
    ? READ_AHEAD_PAGES
    : my_round_up_to_next_power(static_cast<uint32_t>(s));

  page_hash.create(2 * n_blocks);
  last_printout_time= time(nullptr);

  mysql_mutex_init(flush_list_mutex_key, &flush_list_mutex,
                   MY_MUTEX_INIT_FAST);

  pthread_cond_init(&done_flush_LRU, nullptr);
  pthread_cond_init(&done_flush_list, nullptr);
  pthread_cond_init(&do_flush_list, nullptr);
  pthread_cond_init(&done_free, nullptr);

  try_LRU_scan= true;

  ut_d(flush_hp.m_mutex= &flush_list_mutex;);
  ut_d(lru_hp.m_mutex= &mutex);
  ut_d(lru_scan_itr.m_mutex= &mutex);

  io_buf.create((srv_n_read_io_threads + srv_n_write_io_threads) *
                OS_AIO_N_PENDING_IOS_PER_THREAD);

  last_activity_count= srv_get_activity_count();

  buf_LRU_old_ratio_update(100 * 3 / 8, false);
  btr_search_sys_create();

#ifdef __linux__
  if (srv_operation == SRV_OPERATION_NORMAL)
    buf_mem_pressure_detect_init();
#endif
  ut_ad(is_initialised());
  sql_print_information("InnoDB: Completed initialization of buffer pool");
  return false;
}

/** Clean up after successful create() */
void buf_pool_t::close() noexcept
{
  ut_ad(this == &buf_pool);
  if (!is_initialised())
    return;

  mysql_mutex_destroy(&mutex);
  mysql_mutex_destroy(&flush_list_mutex);

  for (buf_page_t *bpage= UT_LIST_GET_LAST(LRU), *prev_bpage= nullptr; bpage;
       bpage= prev_bpage)
  {
    prev_bpage= UT_LIST_GET_PREV(LRU, bpage);
    ut_ad(bpage->in_file());
    ut_ad(bpage->in_LRU_list);
    /* The buffer pool must be clean during normal shutdown.
    Only on aborted startup (with recovery) or with innodb_fast_shutdown=2
    we may discard changes. */
    ut_d(const lsn_t oldest= bpage->oldest_modification();)
    ut_ad(fsp_is_system_temporary(bpage->id().space())
          ? (oldest == 0 || oldest == 2)
          : oldest <= 1 || srv_is_being_started || srv_fast_shutdown == 2);

    if (UNIV_UNLIKELY(!bpage->frame))
    {
      bpage->lock.free();
      ut_free(bpage);
    }
  }

  {
    const size_t size{size_in_bytes};

    for (char *extent= memory,
           *end= memory + block_descriptors_in_bytes(n_blocks);
         extent < end; extent+= innodb_buffer_pool_extent_size)
      for (buf_block_t *block= reinterpret_cast<buf_block_t*>(extent),
             *extent_end= block +
             pages_in_extent[srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN];
           block < extent_end && reinterpret_cast<char*>(block) < end; block++)
      {
        MEM_MAKE_DEFINED(&block->page.lock, sizeof &block->page.lock);
        block->page.lock.free();
      }

    ut_dodump(memory_unaligned, size_unaligned);
#ifdef UNIV_PFS_MEMORY
    PSI_MEMORY_CALL(memory_free)(mem_key_buf_buf_pool, size, owner);
    owner= nullptr;
#endif
    os_total_large_mem_allocated-= size;
    my_virtual_mem_decommit(memory, size);
    my_virtual_mem_release(memory_unaligned, size_unaligned);
    memory= nullptr;
    memory_unaligned= nullptr;
  }

  pthread_cond_destroy(&done_flush_LRU);
  pthread_cond_destroy(&done_flush_list);
  pthread_cond_destroy(&do_flush_list);
  pthread_cond_destroy(&done_free);

  page_hash.free();

  io_buf.close();
  aligned_free(const_cast<byte*>(field_ref_zero));
  field_ref_zero= nullptr;
}

void buf_pool_t::io_buf_t::create(ulint n_slots) noexcept
{
  this->n_slots= n_slots;
  slots= static_cast<buf_tmp_buffer_t*>
    (ut_malloc_nokey(n_slots * sizeof *slots));
  memset((void*) slots, 0, n_slots * sizeof *slots);
}

void buf_pool_t::io_buf_t::close() noexcept
{
  for (buf_tmp_buffer_t *s= slots, *e= slots + n_slots; s != e; s++)
  {
    aligned_free(s->crypt_buf);
    aligned_free(s->comp_buf);
  }
  ut_free(slots);
  slots= nullptr;
  n_slots= 0;
}

buf_tmp_buffer_t *buf_pool_t::io_buf_t::reserve(bool wait_for_reads) noexcept
{
  for (;;)
  {
    for (buf_tmp_buffer_t *s= slots, *e= slots + n_slots; s != e; s++)
      if (s->acquire())
        return s;
    buf_dblwr.flush_buffered_writes();
    os_aio_wait_until_no_pending_writes(true);
    if (!wait_for_reads)
      continue;
    for (buf_tmp_buffer_t *s= slots, *e= slots + n_slots; s != e; s++)
      if (s->acquire())
        return s;
    os_aio_wait_until_no_pending_reads(true);
  }
}

ATTRIBUTE_COLD bool buf_pool_t::withdraw(buf_page_t &bpage) noexcept
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(n_blocks_to_withdraw);
  ut_ad(first_to_withdraw);
  ut_ad(!bpage.zip.data);
  if (&bpage < first_to_withdraw)
    return false;
  n_blocks_to_withdraw--;
  bpage.lock.free();
  UT_LIST_ADD_LAST(withdrawn, &bpage);
  return true;
}

ATTRIBUTE_COLD buf_pool_t::shrink_status buf_pool_t::shrink(size_t size)
  noexcept
{
  mysql_mutex_assert_owner(&mutex);
  DBUG_EXECUTE_IF("buf_shrink_fail", return SHRINK_ABORT;);
  buf_load_abort();

  if (!n_blocks_to_withdraw)
  {
  withdraw_done:
    first_to_withdraw= nullptr;
    while (buf_page_t *b= UT_LIST_GET_FIRST(withdrawn))
    {
      UT_LIST_REMOVE(withdrawn, b);
      /* satisfy the check in lazy_allocate() */
      ut_d(memset((void*) b, 0, sizeof(buf_block_t)));
    }
    return SHRINK_DONE;
  }

  buf_buddy_condense_free(size);

  for (buf_page_t *b= UT_LIST_GET_FIRST(free), *next; b; b= next)
  {
    ut_ad(b->in_free_list);
    ut_ad(!b->in_LRU_list);
    ut_ad(!b->zip.data);
    ut_ad(!b->oldest_modification());
    ut_a(b->state() == buf_page_t::NOT_USED);

    next= UT_LIST_GET_NEXT(list, b);

    if (b >= first_to_withdraw)
    {
      UT_LIST_REMOVE(free, b);
      b->lock.free();
      UT_LIST_ADD_LAST(withdrawn, b);
      if (!--n_blocks_to_withdraw)
        goto withdraw_done;
    }
  }

  buf_block_t *block= allocate();
  size_t scanned= 0;
  for (buf_page_t *b= lru_scan_itr.start(), *prev; block && b; b= prev)
  {
    ut_ad(b->in_LRU_list);
    ut_a(b->in_file());

    prev= UT_LIST_GET_PREV(LRU, b);

    if (!b->can_relocate())
    {
    next:
      if (++scanned & 31)
        continue;
      /* Avoid starvation by periodically releasing buf_pool.mutex. */
      lru_scan_itr.set(prev);
      mysql_mutex_unlock(&mutex);
      mysql_mutex_lock(&mutex);
      prev= lru_scan_itr.get();
      continue;
    }

    const page_id_t id{b->id()};
    hash_chain &chain= page_hash.cell_get(id.fold());
    page_hash_latch &hash_lock= page_hash.lock_get(chain);
    hash_lock.lock();

    {
      /* relocate flush_list and b->page.zip */
      bool have_flush_list_mutex= false;

      switch (b->oldest_modification()) {
      case 2:
        ut_ad(fsp_is_system_temporary(id.space()));
        /* fall through */
      case 0:
        break;
      default:
        mysql_mutex_lock(&flush_list_mutex);
        switch (ut_d(lsn_t om=) b->oldest_modification()) {
        case 1:
          delete_from_flush_list(b);
          /* fall through */
        case 0:
          mysql_mutex_unlock(&flush_list_mutex);
          break;
        default:
          ut_ad(om != 2);
          have_flush_list_mutex= true;
        }
      }

      if (!b->can_relocate())
      {
      next_quick:
        if (have_flush_list_mutex)
          mysql_mutex_unlock(&flush_list_mutex);
        hash_lock.unlock();
        continue;
      }

      if (UNIV_UNLIKELY(will_be_withdrawn(b->zip.data, size)))
      {
        block= buf_buddy_shrink(b, block);
        ut_ad(mach_read_from_4(b->zip.data + FIL_PAGE_OFFSET) == id.page_no());
        if (UNIV_UNLIKELY(!n_blocks_to_withdraw))
        {
          if (have_flush_list_mutex)
            mysql_mutex_unlock(&flush_list_mutex);
          hash_lock.unlock();
          if (block)
            buf_LRU_block_free_non_file_page(block);
          goto withdraw_done;
        }
        if (!block && !(block= allocate()))
          goto next_quick;
      }

      if (!b->frame || b < first_to_withdraw)
        goto next_quick;

      ut_ad(is_uncompressed_current(b));

      byte *const frame= block->page.frame;
      memcpy_aligned<4096>(frame, b->frame, srv_page_size);
      b->lock.free();
      block->page.lock.free();
      new(&block->page) buf_page_t(*b);
      block->page.frame= frame;

      if (have_flush_list_mutex)
      {
        buf_flush_relocate_on_flush_list(b, &block->page);
        mysql_mutex_unlock(&flush_list_mutex);
      }
    }

    /* relocate LRU list */
    if (buf_page_t *prev_b= LRU_remove(b))
      UT_LIST_INSERT_AFTER(LRU, prev_b, &block->page);
    else
      UT_LIST_ADD_FIRST(LRU, &block->page);

    if (LRU_old == b)
      LRU_old= &block->page;

    ut_ad(block->page.in_LRU_list);

    /* relocate page_hash */
    ut_ad(b == page_hash.get(id, chain));
    page_hash.replace(chain, b, &block->page);

    if (b->zip.data)
    {
      ut_ad(mach_read_from_4(b->zip.data + FIL_PAGE_OFFSET) == id.page_no());
      b->zip.data= nullptr;
      /* relocate unzip_LRU list */
      buf_block_t *old_block= reinterpret_cast<buf_block_t*>(b);
      ut_ad(old_block->in_unzip_LRU_list);
      ut_d(old_block->in_unzip_LRU_list= false);
      ut_d(block->in_unzip_LRU_list= true);

      buf_block_t *prev= UT_LIST_GET_PREV(unzip_LRU, old_block);
      UT_LIST_REMOVE(unzip_LRU, old_block);

      if (prev)
        UT_LIST_INSERT_AFTER(unzip_LRU, prev, block);
      else
        UT_LIST_ADD_FIRST(unzip_LRU, block);
    }

    buf_block_modify_clock_inc(block);

#ifdef BTR_CUR_HASH_ADAPT
    assert_block_ahi_empty_on_init(block);
    block->index= nullptr;
    block->n_hash_helps= 0;
    block->n_fields= 1;
    block->left_side= true;
#endif /* BTR_CUR_HASH_ADAPT */
    hash_lock.unlock();

    ut_d(b->in_LRU_list= false);

    b->set_state(buf_page_t::NOT_USED);
    UT_LIST_ADD_LAST(withdrawn, b);
    if (!--n_blocks_to_withdraw)
      goto withdraw_done;

    block= allocate();
    goto next;
  }

  if (UT_LIST_GET_LEN(free) + UT_LIST_GET_LEN(LRU) < usable_size() / 20)
    return SHRINK_ABORT;

  mysql_mutex_lock(&flush_list_mutex);

  if (LRU_warned && !UT_LIST_GET_FIRST(free))
  {
    LRU_warned_clear();
    mysql_mutex_unlock(&flush_list_mutex);
    return SHRINK_ABORT;
  }

  try_LRU_scan= false;
  mysql_mutex_unlock(&mutex);
  page_cleaner_wakeup(true);
  my_cond_wait(&done_flush_list, &flush_list_mutex.m_mutex);
  mysql_mutex_unlock(&flush_list_mutex);
  mysql_mutex_lock(&mutex);

  if (!n_blocks_to_withdraw)
    goto withdraw_done;

  return SHRINK_IN_PROGRESS;
}

inline void buf_pool_t::shrunk(size_t size, size_t reduced) noexcept
{
  ut_ad(size + reduced == size_in_bytes);
  size_in_bytes_requested= size;
  size_in_bytes= size;
# ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
  /* Only page_guess() may read this memory, which after
  my_virtual_mem_decommit() may be zeroed out or preserve its original
  contents.  Try to catch any unintended reads outside page_guess(). */
  MEM_UNDEFINED(memory + size, size_in_bytes_max - size);
# else
  for (size_t n= page_hash.pad(page_hash.n_cells), i= 0; i < n;
       i+= page_hash.ELEMENTS_PER_LATCH + 1)
  {
    auto &latch= reinterpret_cast<page_hash_latch&>(page_hash.array[i]);
    latch.lock();
    /* We already shrunk size_in_bytes. The exclusive lock here
    ensures that any page_guess() will detect an out-of-bounds
    guess before we invoke my_virtual_mem_decommit() below. */
    latch.unlock();
  }
# endif
  my_virtual_mem_decommit(memory + size, reduced);
#ifdef UNIV_PFS_MEMORY
  PSI_MEMORY_CALL(memory_free)(mem_key_buf_buf_pool, reduced, owner);
#endif
}

ATTRIBUTE_COLD void buf_pool_t::resize(size_t size, THD *thd) noexcept
{
  ut_ad(this == &buf_pool);
  mysql_mutex_assert_owner(&LOCK_global_system_variables);
  ut_ad(size <= size_in_bytes_max);
  if (my_use_large_pages)
  {
    my_error(ER_VARIABLE_IS_READONLY, MYF(0), "InnoDB",
             "innodb_buffer_pool_size", "large_pages=0");
    return;
  }

  size_t n_blocks_new= get_n_blocks(size);

  mysql_mutex_lock(&mutex);

  const size_t old_size= size_in_bytes;
  if (first_to_withdraw || old_size != size_in_bytes_requested)
  {
    mysql_mutex_unlock(&mutex);
    my_printf_error(ER_WRONG_USAGE,
                    "innodb_buffer_pool_size change is already in progress",
                    MYF(0));
    return;
  }

  ut_ad(UT_LIST_GET_LEN(withdrawn) == 0);
  ut_ad(n_blocks_to_withdraw == 0);
#ifdef __linux__
  DBUG_EXECUTE_IF("trigger_garbage_collection",
                  mem_pressure_obj.trigger_collection(););
#endif

  if (size == old_size)
  {
    mysql_mutex_unlock(&mutex);
    DBUG_EXECUTE_IF("trigger_garbage_collection",
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    garbage_collect(););
    return;
  }

#ifdef BTR_CUR_HASH_ADAPT
  bool ahi_disabled= false;
#endif

  const bool significant_change=
    n_blocks_new > n_blocks * 2 || n_blocks > n_blocks_new * 2;
  const ssize_t n_blocks_removed= n_blocks - n_blocks_new;

  if (n_blocks_removed <= 0)
  {
    if (!my_virtual_mem_commit(memory + old_size, size - old_size))
    {
      mysql_mutex_unlock(&mutex);
      sql_print_error("InnoDB: Cannot commit innodb_buffer_pool_size=%zum;"
                      " retaining innodb_buffer_pool_size=%zum",
                      size >> 20, old_size >> 20);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return;
    }

    size_in_bytes_requested= size;
    size_in_bytes= size;

    {
      const size_t ssize= srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN;
      const size_t pages= pages_in_extent[ssize];
      const size_t first_extent= n_blocks / pages;

      char *extent= memory + first_extent * innodb_buffer_pool_extent_size;

      buf_block_t *block= reinterpret_cast<buf_block_t*>(extent);
      if (const size_t first_blocks= n_blocks % pages)
      {
        /* Extend the last (partial) extent until its end */
        const buf_block_t *extent_end= block +
          (first_extent == (n_blocks_new / pages)
           ? (n_blocks_new % pages)
           : pages);
        block+= first_blocks;
        memset((void*) block, 0, (extent_end - block) * sizeof *block);

        for (byte *frame= reinterpret_cast<byte*>(extent) +
               first_frame_in_extent[ssize] +
               (first_blocks << srv_page_size_shift); block < extent_end;
             block++, frame+= srv_page_size)
        {
          block->page.frame= frame;
          block->page.lock.init();
          UT_LIST_ADD_LAST(free, &block->page);
          ut_d(block->page.in_free_list= true);
        }
        extent+= innodb_buffer_pool_extent_size;
      }

      /* Fill in further extents; @see buf_pool_t::create() */
      for (const char *const end_new= memory +
             block_descriptors_in_bytes(n_blocks_new);
           extent < end_new; extent+= innodb_buffer_pool_extent_size)
      {
        block= reinterpret_cast<buf_block_t*>(extent);
        const buf_block_t *extent_end= block + pages;
        if (reinterpret_cast<const char*>(extent_end) > end_new)
          extent_end= reinterpret_cast<const buf_block_t*>(end_new);

        memset((void*) block, 0, (extent_end - block) * sizeof *block);
        for (byte *frame= reinterpret_cast<byte*>(extent) +
               first_frame_in_extent[ssize];
             block < extent_end; block++, frame+= srv_page_size)
        {
          block->page.frame= frame;
          block->page.lock.init();
          UT_LIST_ADD_LAST(free, &block->page);
          ut_d(block->page.in_free_list= true);
        }
      }
    }

    mysql_mutex_unlock(&LOCK_global_system_variables);
  resized:
    ut_ad(UT_LIST_GET_LEN(withdrawn) == 0);
    ut_ad(n_blocks_to_withdraw == 0);
    ut_ad(!first_to_withdraw);
    const size_t old_blocks{n_blocks};
    n_blocks= n_blocks_new;

    size_t s= n_blocks_new / BUF_READ_AHEAD_PORTION;
    read_ahead_area= s >= READ_AHEAD_PAGES
      ? READ_AHEAD_PAGES
      : my_round_up_to_next_power(uint32(s));

    if (ssize_t d= size - old_size)
    {
      os_total_large_mem_allocated+= d;
      if (d > 0)
      {
        /* Already committed memory earlier */
        ut_ad(n_blocks_removed <= 0);
#ifdef UNIV_PFS_MEMORY
        PSI_MEMORY_CALL(memory_alloc)(mem_key_buf_buf_pool, d, &owner);
#endif
      }
      else
        shrunk(size, size_t(-d));
    }

    mysql_mutex_unlock(&mutex);

    if (significant_change)
    {
      sql_print_information("InnoDB: Resizing hash tables");
      srv_lock_table_size= 5 * n_blocks_new;
      lock_sys.resize(srv_lock_table_size);
      dict_sys.resize();
    }

#ifdef BTR_CUR_HASH_ADAPT
    if (ahi_disabled)
      btr_search_enable(true);
#endif
    if (n_blocks_removed)
      sql_print_information("InnoDB: innodb_buffer_pool_size=%zum (%zu pages)"
                            " resized from %zum (%zu pages)",
                            size >> 20, n_blocks_new, old_size >> 20,
                            old_blocks);
    mysql_mutex_lock(&LOCK_global_system_variables);
  }
  else
  {
    size_t to_withdraw= size_t(n_blocks_removed);
    n_blocks_to_withdraw= to_withdraw;
    first_to_withdraw= &get_nth_page(n_blocks_new)->page;
    size_in_bytes_requested= size;
    mysql_mutex_unlock(&LOCK_global_system_variables);

    mysql_mutex_unlock(&mutex);
    DEBUG_SYNC_C("buf_pool_shrink_before_wakeup");
    mysql_mutex_lock(&flush_list_mutex);
    page_cleaner_wakeup(true);
    my_cond_wait(&done_flush_list, &flush_list_mutex.m_mutex);
    mysql_mutex_unlock(&flush_list_mutex);
#ifdef BTR_CUR_HASH_ADAPT
    ahi_disabled= btr_search_disable();
#endif /* BTR_CUR_HASH_ADAPT */
    mysql_mutex_lock(&mutex);

    time_t last_message= 0;

    do
    {
      time_t now= time(nullptr);
      if (now - last_message > 15)
      {
        if (last_message != 0 && to_withdraw == n_blocks_to_withdraw)
          break;
        to_withdraw= n_blocks_to_withdraw;
        last_message= now;
        sql_print_information("InnoDB: Trying to shrink"
                              " innodb_buffer_pool_size=%zum (%zu pages)"
                              " from %zum (%zu pages, to withdraw %zu)",
                              size >> 20, n_blocks_new,
                              old_size >> 20, n_blocks, to_withdraw);
      }
      shrink_status s{shrink(size)};
      if (s == SHRINK_DONE)
        goto resized;
      if (s != SHRINK_IN_PROGRESS)
        break;
    }
    while (!thd_kill_level(thd));

    ut_ad(size_in_bytes > size_in_bytes_requested);
    n_blocks_to_withdraw= 0;
    first_to_withdraw= nullptr;
    size_in_bytes_requested= size_in_bytes;

    while (buf_page_t *b= UT_LIST_GET_FIRST(withdrawn))
    {
      UT_LIST_REMOVE(withdrawn, b);
      UT_LIST_ADD_LAST(free, b);
      ut_d(b->in_free_list= true);
      ut_ad(b->state() == buf_page_t::NOT_USED);
      b->lock.init();
    }

    mysql_mutex_unlock(&mutex);
    my_printf_error(ER_WRONG_USAGE, "innodb_buffer_pool_size change aborted",
                    MYF(ME_ERROR_LOG));
#ifdef BTR_CUR_HASH_ADAPT
    if (ahi_disabled)
      btr_search_enable(true);
#endif
    mysql_mutex_lock(&LOCK_global_system_variables);
  }

  ut_d(validate());
}

/** Relocate a ROW_FORMAT=COMPRESSED block in the LRU list and
buf_pool.page_hash.
The caller must relocate bpage->list.
@param bpage   ROW_FORMAT=COMPRESSED only block
@param dpage   destination control block */
static void buf_relocate(buf_page_t *bpage, buf_page_t *dpage) noexcept
{
  const page_id_t id{bpage->id()};
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  ut_ad(!bpage->frame);
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(mach_read_from_4(bpage->zip.data + FIL_PAGE_OFFSET) == id.page_no());
  ut_ad(buf_pool.page_hash.lock_get(chain).is_write_locked());
  ut_ad(bpage == buf_pool.page_hash.get(id, chain));
  ut_d(const auto state= bpage->state());
  ut_ad(state >= buf_page_t::FREED);
  ut_ad(state <= buf_page_t::READ_FIX);
  ut_ad(bpage->lock.is_write_locked());
  const auto frame= dpage->frame;
  ut_ad(frame == reinterpret_cast<buf_block_t*>(dpage)->frame_address());

  dpage->lock.free();
  new (dpage) buf_page_t(*bpage);

  dpage->frame= frame;

  /* Important that we adjust the hazard pointer before
  removing bpage from LRU list. */
  if (buf_page_t *b= buf_pool.LRU_remove(bpage))
    UT_LIST_INSERT_AFTER(buf_pool.LRU, b, dpage);
  else
    UT_LIST_ADD_FIRST(buf_pool.LRU, dpage);

  if (UNIV_UNLIKELY(buf_pool.LRU_old == bpage))
  {
    buf_pool.LRU_old= dpage;
#ifdef UNIV_LRU_DEBUG
    /* buf_pool.LRU_old must be the first item in the LRU list
    whose "old" flag is set. */
    ut_a(buf_pool.LRU_old->old);
    ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old) ||
         !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->old);
    ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old) ||
         UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->old);
  }
  else
  {
    /* Check that the "old" flag is consistent in
    the block and its neighbours. */
    dpage->set_old(dpage->is_old());
#endif /* UNIV_LRU_DEBUG */
  }

  ut_d(CheckInLRUList::validate());

  buf_pool.page_hash.replace(chain, bpage, dpage);
}

/** Mark the page status as FREED for the given tablespace and page number.
@param[in,out]	space	tablespace
@param[in]	page	page number
@param[in,out]	mtr	mini-transaction */
TRANSACTIONAL_TARGET
void buf_page_free(fil_space_t *space, uint32_t page, mtr_t *mtr)
{
  ut_ad(mtr);
  ut_ad(mtr->is_active());

  if (srv_immediate_scrub_data_uncompressed
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
      || space->is_compressed()
#endif
      )
    mtr->add_freed_offset(space, page);

  ++buf_pool.stat.n_page_gets;
  const page_id_t page_id(space->id, page);
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  uint32_t fix;
  buf_block_t *block;
  {
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    block= reinterpret_cast<buf_block_t*>
      (buf_pool.page_hash.get(page_id, chain));
    if (!block || !block->page.frame)
      /* FIXME: convert ROW_FORMAT=COMPRESSED, without buf_zip_decompress() */
      return;
    /* To avoid a deadlock with buf_LRU_free_page() of some other page
    and buf_page_write_complete() of this page, we must not wait for a
    page latch while holding a page_hash latch. */
    fix= block->page.fix();
  }

  if (UNIV_UNLIKELY(fix < buf_page_t::UNFIXED))
  {
    block->page.unfix();
    return;
  }

  block->page.lock.x_lock();
#ifdef BTR_CUR_HASH_ADAPT
  if (block->index)
    btr_search_drop_page_hash_index(block, false);
#endif /* BTR_CUR_HASH_ADAPT */
  block->page.set_freed(block->page.state());
  mtr->memo_push(block, MTR_MEMO_PAGE_X_MODIFY);
}

static void buf_inc_get(ha_handler_stats *stats)
{
  mariadb_increment_pages_accessed(stats);
  ++buf_pool.stat.n_page_gets;
}

TRANSACTIONAL_TARGET
buf_page_t *buf_page_get_zip(const page_id_t page_id) noexcept
{
  ha_handler_stats *const stats= mariadb_stats;
  buf_inc_get(stats);

  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  buf_page_t *bpage;

  for (;;)
  {
#ifndef NO_ELISION
    if (xbegin())
    {
      if (hash_lock.is_locked())
        xend();
      else
      {
        bpage= buf_pool.page_hash.get(page_id, chain);
        const bool got_s_latch= bpage && bpage->lock.s_lock_try();
        xend();
        if (got_s_latch)
          break;
      }
    }
#endif

    hash_lock.lock_shared();
    bpage= buf_pool.page_hash.get(page_id, chain);
    if (!bpage)
    {
      hash_lock.unlock_shared();
      switch (dberr_t err= buf_read_page(page_id, chain, false)) {
      case DB_SUCCESS:
      case DB_SUCCESS_LOCKED_REC:
        mariadb_increment_pages_read(stats);
        continue;
      case DB_TABLESPACE_DELETED:
        return nullptr;
      default:
        sql_print_error("InnoDB: Reading compressed page "
                        "[page id: space=" UINT32PF ", page number=" UINT32PF
                        "] failed with error: %s",
                        page_id.space(), page_id.page_no(), ut_strerr(err));
        return nullptr;
      }
    }

    ut_ad(bpage->in_file());
    ut_ad(page_id == bpage->id());

    const bool got_s_latch= bpage->lock.s_lock_try();
    hash_lock.unlock_shared();
    if (UNIV_LIKELY(got_s_latch))
      break;
    /* We may fail to acquire bpage->lock because a read is holding an
    exclusive latch on this block and either in progress or invoking
    buf_pool_t::corrupted_evict().

    Let us aqcuire and release buf_pool.mutex to ensure that any
    buf_pool_t::corrupted_evict() will proceed before we reacquire
    the hash_lock that it could be waiting for.

    While we are at it, let us also try to discard any uncompressed
    page frame of the compressed BLOB page, in case one had been
    allocated for writing the BLOB. */
    mysql_mutex_lock(&buf_pool.mutex);
    bpage= buf_pool.page_hash.get(page_id, chain);
    if (bpage)
      buf_LRU_free_page(bpage, false);
    mysql_mutex_unlock(&buf_pool.mutex);
  }

  if (UNIV_UNLIKELY(!bpage->zip.data))
  {
    ut_ad("no ROW_FORMAT=COMPRESSED page!" == 0);
    bpage->lock.s_unlock();
    bpage= nullptr;
  }
  else
    buf_page_make_young_if_needed(bpage);

#ifdef UNIV_DEBUG
  if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG */
  return bpage;
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

bool buf_zip_decompress(buf_block_t *block, bool check) noexcept
{
	const byte*	frame = block->page.zip.data;
	ulint		size = page_zip_get_size(&block->page.zip);
	/* The tablespace will not be found if this function is called
	during IMPORT. */
	fil_space_t* space= fil_space_t::get(block->page.id().space());
	const unsigned key_version = mach_read_from_4(
		frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	fil_space_crypt_t* crypt_data = space ? space->crypt_data : NULL;
	const bool encrypted = crypt_data
		&& crypt_data->type != CRYPT_SCHEME_UNENCRYPTED
		&& (!crypt_data->is_default_encryption()
		    || srv_encrypt_tables);

	ut_ad(block->zip_size());
	ut_a(block->page.id().space() != 0);
	ut_ad(mach_read_from_4(frame + FIL_PAGE_OFFSET)
              == block->page.id().page_no());

	if (UNIV_UNLIKELY(check && !page_zip_verify_checksum(frame, size))) {

		ib::error() << "Compressed page checksum mismatch for "
			<< (space ? space->chain.start->name : "")
			<< block->page.id() << ": stored: "
			<< mach_read_from_4(frame + FIL_PAGE_SPACE_OR_CHKSUM)
			<< ", crc32: "
			<< page_zip_calc_checksum(frame, size, false)
			<< " adler32: "
			<< page_zip_calc_checksum(frame, size, true);
		goto err_exit;
	}

	switch (fil_page_get_type(frame)) {
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		if (page_zip_decompress(&block->page.zip,
					block->page.frame, TRUE)) {
func_exit:
			if (space) {
				space->release();
			}
			return true;
		}

		ib::error() << "Unable to decompress "
			<< (space ? space->chain.start->name : "")
			<< block->page.id();
		goto err_exit;
	case FIL_PAGE_TYPE_ALLOCATED:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* Copy to uncompressed storage. */
		memcpy(block->page.frame, frame, block->zip_size());
		goto func_exit;
	}

	ib::error() << "Unknown compressed page type "
		<< fil_page_get_type(frame)
		<< " in " << (space ? space->chain.start->name : "")
		<< block->page.id();

err_exit:
	if (encrypted) {
		ib::info() << "Row compressed page could be encrypted"
			" with key_version " << key_version;
	}

	if (space) {
		space->release();
	}

	return false;
}

ATTRIBUTE_COLD
buf_block_t *buf_pool_t::unzip(buf_page_t *b, buf_pool_t::hash_chain &chain)
  noexcept
{
  buf_block_t *block= buf_LRU_get_free_block(have_no_mutex);
  buf_block_init_low(block);
  page_hash_latch &hash_lock= page_hash.lock_get(chain);
 wait_for_unfix:
  mysql_mutex_lock(&mutex);
  hash_lock.lock();

  /* b->lock implies !b->can_relocate() */
  ut_ad(b->lock.have_x());
  ut_ad(b == page_hash.get(b->id(), chain));

  /* Wait for b->unfix() in any other threads. */
  uint32_t state= b->state();
  ut_ad(buf_page_t::buf_fix_count(state));
  ut_ad(!buf_page_t::is_freed(state));

  switch (state) {
  case buf_page_t::UNFIXED + 1:
  case buf_page_t::REINIT + 1:
    break;
  default:
    ut_ad(state < buf_page_t::READ_FIX);

    if (state < buf_page_t::UNFIXED + 1)
    {
      ut_ad(state > buf_page_t::FREED);
      b->lock.x_unlock();
      hash_lock.unlock();
      buf_LRU_block_free_non_file_page(block);
      mysql_mutex_unlock(&mutex);
      b->unfix();
      return nullptr;
    }

    mysql_mutex_unlock(&mutex);
    hash_lock.unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    goto wait_for_unfix;
  }

  /* Ensure that another buf_page_get_low() or buf_page_t::page_fix()
  will wait for block->page.lock.x_unlock(). buf_relocate() will
  copy the state from b to block and replace b with block in page_hash. */
  b->set_state(buf_page_t::READ_FIX);

  mysql_mutex_lock(&flush_list_mutex);
  buf_relocate(b, &block->page);

  /* X-latch the block for the duration of the decompression. */
  block->page.lock.x_lock();

  buf_flush_relocate_on_flush_list(b, &block->page);
  mysql_mutex_unlock(&flush_list_mutex);

  /* Insert at the front of unzip_LRU list */
  buf_unzip_LRU_add_block(block, false);

  mysql_mutex_unlock(&mutex);
  hash_lock.unlock();

#if defined SUX_LOCK_GENERIC || defined UNIV_DEBUG
  b->lock.x_unlock();
  b->lock.free();
#endif
  ut_free(b);

  n_pend_unzip++;
  const bool ok{buf_zip_decompress(block, false)};
  n_pend_unzip--;

  if (UNIV_UNLIKELY(!ok))
  {
    mysql_mutex_lock(&mutex);
    block->page.read_unfix(state);
    block->page.lock.x_unlock();
    if (!buf_LRU_free_page(&block->page, true))
      ut_ad(0);
    mysql_mutex_unlock(&mutex);
    return nullptr;
  }
  else
    block->page.read_unfix(state);

  return block;
}

buf_block_t *buf_pool_t::page_fix(const page_id_t id,
                                  dberr_t *err,
                                  buf_pool_t::page_fix_conflicts c) noexcept
{
  ha_handler_stats *const stats= mariadb_stats;
  buf_inc_get(stats);
  auto& chain= page_hash.cell_get(id.fold());
  page_hash_latch &hash_lock= page_hash.lock_get(chain);
  for (;;)
  {
    hash_lock.lock_shared();
    buf_page_t *b= page_hash.get(id, chain);
    if (b)
    {
      uint32_t state= b->fix() + 1;
      hash_lock.unlock_shared();

      if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED))
      {
        ut_ad(state > buf_page_t::FREED);
        if (c == FIX_ALSO_FREED && b->id() == id)
        {
          ut_ad(state == buf_page_t::FREED + 1);
          return reinterpret_cast<buf_block_t*>(b);
        }
        /* The page was marked as freed or corrupted. */
        b->unfix();
      corrupted:
        if (err)
          *err= DB_CORRUPTION;
        return nullptr;
      }

      if (state >= buf_page_t::READ_FIX && state < buf_page_t::WRITE_FIX)
      {
        if (c == FIX_NOWAIT)
        {
        would_block:
          b->unfix();
          return reinterpret_cast<buf_block_t*>(-1);
        }

        if (UNIV_LIKELY(b->frame != nullptr))
          ut_ad(b->frame==reinterpret_cast<buf_block_t*>(b)->frame_address());
        else if (state < buf_page_t::READ_FIX)
          goto unzip;
        else
        {
        wait_for_unzip:
          b->unfix();
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          continue;
        }
        b->lock.s_lock_nospin();
        state= b->state();
        ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);

        b->lock.s_unlock();
      }

      if (UNIV_UNLIKELY(!b->frame))
      {
      unzip:
        if (b->lock.x_lock_try());
        else if (c == FIX_NOWAIT)
          goto would_block;
        else
          goto wait_for_unzip;

        buf_block_t *block= unzip(b, chain);
        if (!block)
          goto corrupted;

        b= &block->page;
        state= b->state();
        b->lock.x_unlock();
      }

      return reinterpret_cast<buf_block_t*>(b);
    }

    hash_lock.unlock_shared();

    if (c == FIX_NOWAIT)
      return reinterpret_cast<buf_block_t*>(-1);

    switch (dberr_t local_err= buf_read_page(id, chain)) {
    default:
      if (err)
        *err= local_err;
      return nullptr;
    case DB_SUCCESS:
    case DB_SUCCESS_LOCKED_REC:
      mariadb_increment_pages_read(stats);
      buf_read_ahead_random(id);
    }
  }
}

TRANSACTIONAL_TARGET
uint32_t buf_pool_t::page_guess(buf_block_t *b, page_hash_latch &latch,
                                const page_id_t id) noexcept
{
  transactional_shared_lock_guard<page_hash_latch> g{latch};
#ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
  /* shrunk() and my_virtual_mem_decommit() could retain the original
  contents of the virtual memory range or zero it out immediately or
  with a delay.  Any zeroing out may lead to a false positive for
  b->page.id() == id but never for b->page.state().  At the time of
  the shrunk() call, shrink() and buf_LRU_block_free_non_file_page()
  should guarantee that b->page.state() is equal to
  buf_page_t::NOT_USED (0) for all to-be-freed blocks. */
#else
  /* shrunk() made the memory inaccessible. */
  if (UNIV_UNLIKELY(reinterpret_cast<char*>(b) >= memory + size_in_bytes))
    return 0;
#endif
  const page_id_t block_id{b->page.id()};
#ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
  /* shrunk() may have invoked MEM_UNDEFINED() on this memory to be able
  to catch any unintended access elsewhere in our code. */
  MEM_MAKE_DEFINED(&block_id, sizeof block_id);
#endif

  if (id == block_id)
  {
    uint32_t state= b->page.state();
#ifndef HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT
    /* shrunk() may have invoked MEM_UNDEFINED() on this memory to be able
    to catch any unintended access elsewhere in our code. */
    MEM_MAKE_DEFINED(&state, sizeof state);
#endif
    /* Ignore guesses that point to read-fixed blocks.  We can only
    avoid a race condition by looking up the block via page_hash. */
    if ((state >= buf_page_t::FREED && state < buf_page_t::READ_FIX) ||
        state >= buf_page_t::WRITE_FIX)
      return b->page.fix();
    ut_ad(b->page.frame);
  }
  return 0;
}

/** Low level function used to get access to a database page.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		latch mode
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
or BUF_PEEK_IF_IN_POOL
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@return pointer to the block
@retval nullptr	if the block is corrupted or unavailable */
TRANSACTIONAL_TARGET
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	ulint			zip_size,
	rw_lock_type_t		rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	mtr_t*			mtr,
	dberr_t*		err) noexcept
{
	ulint		retries = 0;

	/* BUF_GET_RECOVER is only used by recv_sys_t::recover(),
	which must be invoked during early server startup when crash
	recovery may be in progress. The only case when it may be
	invoked outside recovery is when dict_create() has initialized
	a new database and is invoking dict_boot(). In this case, the
	LSN will be small. At the end of a bootstrap, the shutdown LSN
	would typically be around 60000 with the default
	innodb_undo_tablespaces=3, and less than 110000 with the maximum
	innodb_undo_tablespaces=127. */
	ut_d(extern bool ibuf_upgrade_was_needed;)
	ut_ad(mode == BUF_GET_RECOVER
	      ? recv_recovery_is_on() || log_get_lsn() < 120000
	      || log_get_lsn() == recv_sys.lsn + SIZE_OF_FILE_CHECKPOINT
	      || ibuf_upgrade_was_needed
	      : !recv_recovery_is_on() || recv_sys.after_apply);
	ut_ad(mtr->is_active());

	if (err) {
		*err = DB_SUCCESS;
	}

#ifdef UNIV_DEBUG
	switch (mode) {
	default:
		ut_ad(mode == BUF_PEEK_IF_IN_POOL);
		break;
	case BUF_GET_POSSIBLY_FREED:
	case BUF_GET_IF_IN_POOL:
		/* The caller may pass a dummy page size,
		because it does not really matter. */
		break;
	case BUF_GET_RECOVER:
	case BUF_GET:
		ut_ad(!mtr->is_freeing_tree());
		fil_space_t* s = fil_space_get(page_id.space());
		ut_ad(s);
		ut_ad(s->zip_size() == zip_size);
	}
#endif /* UNIV_DEBUG */

	ha_handler_stats* const stats = mariadb_stats;
	buf_inc_get(stats);
	auto& chain= buf_pool.page_hash.cell_get(page_id.fold());
	page_hash_latch& hash_lock = buf_pool.page_hash.lock_get(chain);
loop:
	buf_block_t* block = guess;
	uint32_t state;

	if (block
	    && (state = buf_pool.page_guess(block, hash_lock, page_id))) {
		goto got_block;
	}

	guess = nullptr;

	/* A memory transaction would frequently be aborted here. */
	hash_lock.lock_shared();
	block = reinterpret_cast<buf_block_t*>(
		buf_pool.page_hash.get(page_id, chain));
	if (UNIV_LIKELY(block != nullptr)) {
		state = block->page.fix();
		hash_lock.unlock_shared();
		goto got_block;
	}
	hash_lock.unlock_shared();

	/* Page not in buf_pool: needs to be read from file */
	switch (mode) {
	case BUF_GET_IF_IN_POOL:
	case BUF_PEEK_IF_IN_POOL:
		return nullptr;
	}

	/* The call path is buf_read_page() ->
	buf_read_page_low() (fil_space_t::io()) ->
	buf_page_t::read_complete() ->
	buf_decrypt_after_read(). Here fil_space_t* is used
	and we decrypt -> buf_page_check_corrupt() where page
	checksums are compared. Decryption, decompression as
	well as error handling takes place at a lower level.
	Here we only need to know whether the page really is
	corrupted, or if an encrypted page with a valid
	checksum cannot be decypted. */

	switch (dberr_t local_err = buf_read_page(page_id, chain)) {
	case DB_SUCCESS:
	case DB_SUCCESS_LOCKED_REC:
		mariadb_increment_pages_read(stats);
		buf_read_ahead_random(page_id);
		break;
	default:
		if (mode != BUF_GET_POSSIBLY_FREED
		    && retries++ < BUF_PAGE_READ_MAX_RETRIES) {
			DBUG_EXECUTE_IF("intermittent_read_failure",
					retries = BUF_PAGE_READ_MAX_RETRIES;);
		}
		/* fall through */
	case DB_PAGE_CORRUPTED:
		if (err) {
			*err = local_err;
		}
		return nullptr;
	}

	ut_d(if (!(++buf_dbg_counter % 5771)) buf_pool.validate());
	goto loop;

got_block:
	state++;
	ut_ad(state > buf_page_t::FREED);

	if (state > buf_page_t::READ_FIX && state < buf_page_t::WRITE_FIX) {
		if (mode == BUF_PEEK_IF_IN_POOL) {
ignore_block:
			block->unfix();
ignore_unfixed:
			ut_ad(mode == BUF_GET_POSSIBLY_FREED
			      || mode == BUF_PEEK_IF_IN_POOL);
			if (err) {
				*err = DB_CORRUPTION;
			}
			return nullptr;
		}

		if (UNIV_UNLIKELY(!block->page.frame)) {
			goto wait_for_unzip;
		}
		/* A read-fix is released after block->page.lock
		in buf_page_t::read_complete() or
		buf_pool_t::corrupted_evict(), or
		after buf_zip_decompress() in this function. */
		block->page.lock.s_lock_nospin();
		state = block->page.state();
		ut_ad(state < buf_page_t::READ_FIX
		      || state > buf_page_t::WRITE_FIX);
		const page_id_t id{block->page.id()};

		if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
			block->page.unfix();
			block->page.lock.s_unlock();

			if (UNIV_UNLIKELY(id == page_id)) {
				/* The page read was completed, and
				another thread marked the page as free
				while we were waiting. */
				goto ignore_unfixed;
			}

			ut_ad(id == page_id_t{~0ULL});

			if (++retries < BUF_PAGE_READ_MAX_RETRIES) {
				goto loop;
			}

			if (err) {
				*err = DB_PAGE_CORRUPTED;
			}

			return nullptr;
		}
		ut_ad(id == page_id);

		if (UNIV_LIKELY(state > buf_page_t::UNFIXED
				&& block->page.frame)) {
			switch (rw_latch) {
				bool nowait;
			case RW_NO_LATCH:
				break;
			default:
				nowait = block->page.lock.s_x_upgrade();
				if (rw_latch == RW_SX_LATCH) {
					block->page.lock.x_u_downgrade();
				} else {
					ut_ad(rw_latch == RW_X_LATCH);
				}
				if (!nowait) {
					goto latch_waited;
				} else {
					ut_ad(state < buf_page_t::READ_FIX);
				}
				/* fall through */
			case RW_S_LATCH:
				mtr->memo_push(block,
					       mtr_memo_type_t(rw_latch));
				goto latched;
			}
		}

		block->page.lock.s_unlock();
	} else if (UNIV_UNLIKELY(!block->page.frame)
		   && mode == BUF_PEEK_IF_IN_POOL) {
		/* The BUF_PEEK_IF_IN_POOL mode is mainly used for dropping an
		adaptive hash index. There cannot be an
		adaptive hash index for a compressed-only page. */
		goto ignore_block;
	}

	ut_ad(mode == BUF_GET_IF_IN_POOL || mode == BUF_PEEK_IF_IN_POOL
	      || block->zip_size() == zip_size);

	if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
		goto ignore_block;
	}
	ut_ad((~buf_page_t::LRU_MASK) & state);
	ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);

	if (UNIV_UNLIKELY(!block->page.frame)) {
		if (!block->page.lock.x_lock_try()) {
wait_for_unzip:
			/* The page is being read or written, or
			another thread is executing buf_pool.unzip() on it. */
			block->page.unfix();
			std::this_thread::sleep_for(
				std::chrono::microseconds(100));
			goto loop;
		}

		block = buf_pool.unzip(&block->page, chain);

		if (!block) {
			goto ignore_unfixed;
		}

		block->page.lock.x_unlock();
	}

#ifdef UNIV_DEBUG
	if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG */

	/* The state = block->page.state() may be stale at this point,
	and in fact, at any point of time if we consider its
	buffer-fix component. If the block is being read into the
	buffer pool, it is possible that buf_page_t::read_complete()
	will invoke buf_pool_t::corrupted_evict() and therefore
	invalidate it (invoke buf_page_t::set_corrupt_id() and set the
	state to FREED). Therefore, after acquiring the page latch we
	must recheck the state. */

	switch (rw_latch) {
	case RW_NO_LATCH:
		mtr->memo_push(block, MTR_MEMO_BUF_FIX);
		return block;
	case RW_S_LATCH:
		block->page.lock.s_lock();
		break;
	case RW_SX_LATCH:
		block->page.lock.u_lock();
		ut_ad(!block->page.is_io_fixed());
		break;
	default:
		ut_ad(rw_latch == RW_X_LATCH);
		if (block->page.lock.x_lock_upgraded()) {
			ut_ad(block->page.id() == page_id);
			block->unfix();
			mtr->page_lock_upgrade(*block);
			return block;
		}
	}

latch_waited:
	mtr->memo_push(block, mtr_memo_type_t(rw_latch));
	state = block->page.state();

	if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
		mtr->release_last_page();
		goto ignore_unfixed;
	}

latched:
	ut_ad(state < buf_page_t::READ_FIX || state > buf_page_t::WRITE_FIX);

#ifdef BTR_CUR_HASH_ADAPT
	btr_search_drop_page_hash_index(block, true);
#endif /* BTR_CUR_HASH_ADAPT */

	ut_ad(block->page.frame == block->frame_address());
	ut_ad(page_id_t(page_get_space_id(block->page.frame),
			page_get_page_no(block->page.frame)) == page_id);

	return block;
}

buf_block_t *buf_page_optimistic_fix(buf_block_t *block, page_id_t id) noexcept
{
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  if (uint32_t state= buf_pool.page_guess(block, hash_lock, id))
  {
    if (UNIV_LIKELY(state >= buf_page_t::UNFIXED))
      return block;
    else
      /* Refuse access to pages that are marked as freed in the data file. */
      block->page.unfix();
  }
  return nullptr;
}

buf_block_t *buf_page_optimistic_get(buf_block_t *block,
                                     rw_lock_type_t rw_latch,
                                     uint64_t modify_clock,
                                     mtr_t *mtr) noexcept
{
  ut_ad(mtr->is_active());
  ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_X_LATCH);
  ut_ad(block->page.buf_fix_count());

  if (rw_latch == RW_S_LATCH)
  {
    if (!block->page.lock.s_lock_try())
    {
    fail:
      block->page.unfix();
      return nullptr;
    }

    if (modify_clock != block->modify_clock || block->page.is_freed())
    {
      block->page.lock.s_unlock();
      goto fail;
    }

    ut_ad(!block->page.is_read_fixed());
    buf_page_make_young_if_needed(&block->page);
    mtr->memo_push(block, MTR_MEMO_PAGE_S_FIX);
  }
  else if (block->page.lock.have_u_not_x())
  {
    block->page.lock.u_x_upgrade();
    block->page.unfix();
    mtr->page_lock_upgrade(*block);
    ut_ad(modify_clock == block->modify_clock);
  }
  else if (!block->page.lock.x_lock_try())
    goto fail;
  else
  {
    ut_ad(!block->page.is_io_fixed());

    if (modify_clock != block->modify_clock || block->page.is_freed())
    {
      block->page.lock.x_unlock();
      goto fail;
    }

    buf_page_make_young_if_needed(&block->page);
    mtr->memo_push(block, MTR_MEMO_PAGE_X_FIX);
  }

  ut_d(if (!(++buf_dbg_counter % 5771)) buf_pool.validate());
  ut_d(const auto state = block->page.state());
  ut_ad(state > buf_page_t::UNFIXED);
  ut_ad(state < buf_page_t::READ_FIX || state > buf_page_t::WRITE_FIX);
  ut_ad(~buf_page_t::LRU_MASK & state);
  ut_ad(block->page.frame);

  return block;
}

/** Try to S-latch a page.
Suitable for using when holding the lock_sys latches (as it avoids deadlock).
@param[in]	page_id	page identifier
@param[in,out]	mtr	mini-transaction
@return the block
@retval nullptr if an S-latch cannot be granted immediately */
TRANSACTIONAL_TARGET
buf_block_t *buf_page_try_get(const page_id_t page_id, mtr_t *mtr) noexcept
{
  ut_ad(mtr);
  ut_ad(mtr->is_active());
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
  buf_block_t *block;

  {
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    block= reinterpret_cast<buf_block_t*>
      (buf_pool.page_hash.get(page_id, chain));
    if (!block || !block->page.frame || !block->page.lock.s_lock_try())
      return nullptr;
  }

  block->page.fix();
  ut_ad(!block->page.is_read_fixed());
  mtr->memo_push(block, MTR_MEMO_PAGE_S_FIX);

#ifdef UNIV_DEBUG
  if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG */
  ut_ad(block->page.buf_fix_count());
  ut_ad(block->page.id() == page_id);

  buf_inc_get(mariadb_stats);
  return block;
}

/** Initialize the block.
@param page_id  page identifier
@param zip_size ROW_FORMAT=COMPRESSED page size, or 0
@param fix      initial buf_fix_count() */
void buf_block_t::initialise(const page_id_t page_id, ulint zip_size,
                             uint32_t fix) noexcept
{
  ut_ad(!page.in_file());
  buf_block_init_low(this);
  page.init(fix, page_id);
  page.set_os_used();
  page_zip_set_size(&page.zip, zip_size);
}

TRANSACTIONAL_TARGET
static buf_block_t *buf_page_create_low(page_id_t page_id, ulint zip_size,
                                        mtr_t *mtr, buf_block_t *free_block)
  noexcept
{
  ut_ad(mtr->is_active());
  ut_ad(page_id.space() != 0 || !zip_size);

  free_block->initialise(page_id, zip_size, buf_page_t::MEMORY);

  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(page_id.fold());
retry:
  mysql_mutex_lock(&buf_pool.mutex);

  buf_page_t *bpage= buf_pool.page_hash.get(page_id, chain);

  if (bpage)
  {
#ifdef BTR_CUR_HASH_ADAPT
    const dict_index_t *drop_hash_entry= nullptr;
#endif

    if (!mtr->have_x_latch(reinterpret_cast<const buf_block_t&>(*bpage)))
    {
      /* Buffer-fix the block to prevent the block being concurrently freed
      after we release the buffer pool mutex. It should work fine with
      concurrent load of the page (free on disk) to buffer pool due to
      possible read ahead. After we find a zero filled page during load, we
      call buf_pool_t::corrupted_evict, where we try to wait for all buffer
      fixes to go away only after resetting the page ID and releasing the
      page latch. */
      auto state= bpage->fix();
      DBUG_EXECUTE_IF("ib_buf_create_intermittent_wait",
      {
        static bool need_to_wait = false;
        need_to_wait = !need_to_wait;
        /* Simulate try lock failure in every alternate call. */
        if (need_to_wait) {
          goto must_wait;
        }
      });

      if (!bpage->lock.x_lock_try())
      {
#ifndef DBUG_OFF
      must_wait:
#endif
        mysql_mutex_unlock(&buf_pool.mutex);

        bpage->lock.x_lock();
        const page_id_t id{bpage->id()};
        if (UNIV_UNLIKELY(id != page_id))
        {
          ut_ad(id.is_corrupted());
          ut_ad(bpage->is_freed());
          bpage->unfix();
          bpage->lock.x_unlock();
          goto retry;
        }
        mysql_mutex_lock(&buf_pool.mutex);
        state= bpage->state();
        ut_ad(!bpage->is_io_fixed(state));
        ut_ad(bpage->buf_fix_count(state));
      }
      else
        state= bpage->state();

      ut_ad(state >= buf_page_t::FREED);
      ut_ad(state < buf_page_t::READ_FIX);

      if (state < buf_page_t::UNFIXED)
        bpage->set_reinit(buf_page_t::FREED);
      else
        bpage->set_reinit(state & buf_page_t::LRU_MASK);

      if (UNIV_LIKELY(bpage->frame != nullptr))
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        buf_block_t *block= reinterpret_cast<buf_block_t*>(bpage);
        ut_ad(bpage->frame == block->frame_address());
        mtr->memo_push(block, MTR_MEMO_PAGE_X_FIX);
#ifdef BTR_CUR_HASH_ADAPT
        drop_hash_entry= block->index;
#endif
      }
      else
      {
        page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
        /* It does not make sense to use transactional_lock_guard here,
        because buf_relocate() would likely make the memory transaction
        too large. */
        hash_lock.lock();

        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        buf_relocate(bpage, &free_block->page);
        free_block->page.lock.x_lock();
        buf_flush_relocate_on_flush_list(bpage, &free_block->page);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);

        buf_unzip_LRU_add_block(free_block, FALSE);

        mysql_mutex_unlock(&buf_pool.mutex);
        hash_lock.unlock();
#if defined SUX_LOCK_GENERIC || defined UNIV_DEBUG
        bpage->lock.x_unlock();
        bpage->lock.free();
#endif
        ut_free(bpage);
        mtr->memo_push(free_block, MTR_MEMO_PAGE_X_FIX);
        bpage= &free_block->page;
      }
    }
    else
    {
      mysql_mutex_unlock(&buf_pool.mutex);
      ut_ad(bpage->frame ==
            reinterpret_cast<buf_block_t*>(bpage)->frame_address());
#ifdef BTR_CUR_HASH_ADAPT
      ut_ad(!reinterpret_cast<buf_block_t*>(bpage)->index);
#endif
      const auto state= bpage->state();
      ut_ad(state >= buf_page_t::FREED);
      bpage->set_reinit(state < buf_page_t::UNFIXED ? buf_page_t::FREED
                        : state & buf_page_t::LRU_MASK);
    }

#ifdef BTR_CUR_HASH_ADAPT
    if (drop_hash_entry)
      btr_search_drop_page_hash_index(reinterpret_cast<buf_block_t*>(bpage),
                                      false);
#endif /* BTR_CUR_HASH_ADAPT */

    return reinterpret_cast<buf_block_t*>(bpage);
  }

  /* If we get here, the page was not in buf_pool: init it there */

  DBUG_PRINT("ib_buf", ("create page %u:%u",
                        page_id.space(), page_id.page_no()));

  bpage= &free_block->page;

  ut_ad(bpage->state() == buf_page_t::MEMORY);
  bpage->lock.x_lock();

  /* The block must be put to the LRU list */
  buf_LRU_add_block(bpage, false);
  {
    transactional_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    bpage->set_state(buf_page_t::REINIT + 1);
    buf_pool.page_hash.append(chain, bpage);
  }

  if (UNIV_UNLIKELY(zip_size))
  {
    bpage->zip.data= buf_buddy_alloc(zip_size);

    /* To maintain the invariant block->in_unzip_LRU_list ==
    block->page.belongs_to_unzip_LRU() we have to add this
    block to unzip_LRU after block->page.zip.data is set. */
    ut_ad(bpage->belongs_to_unzip_LRU());
    buf_unzip_LRU_add_block(reinterpret_cast<buf_block_t*>(bpage), FALSE);
  }

  buf_pool.stat.n_pages_created++;
  mysql_mutex_unlock(&buf_pool.mutex);

  mtr->memo_push(reinterpret_cast<buf_block_t*>(bpage), MTR_MEMO_PAGE_X_FIX);

  bpage->set_accessed();

  static_assert(FIL_PAGE_PREV + 4 == FIL_PAGE_NEXT, "adjacent");
  memset_aligned<8>(bpage->frame + FIL_PAGE_PREV, 0xff, 8);
  mach_write_to_2(bpage->frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_ALLOCATED);

  /* FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION is only used on the
  following pages:
  (1) The first page of the InnoDB system tablespace (page 0:0)
  (2) FIL_RTREE_SPLIT_SEQ_NUM on R-tree pages
  (3) key_version on encrypted pages (not page 0:0) */

  memset(bpage->frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
  memset_aligned<8>(bpage->frame + FIL_PAGE_LSN, 0, 8);

#ifdef UNIV_DEBUG
  if (!(++buf_dbg_counter % 5771)) buf_pool.validate();
#endif /* UNIV_DEBUG */
  return reinterpret_cast<buf_block_t*>(bpage);
}

/** Initialize a page in the buffer pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen).
@param[in,out]	space		space object
@param[in]	offset		offset of the tablespace
				or deferred space id if space
				object is null
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[in,out]	free_block	pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(fil_space_t *space, uint32_t offset,
                ulint zip_size, mtr_t *mtr, buf_block_t *free_block) noexcept
{
  space->free_page(offset, false);
  return buf_page_create_low({space->id, offset}, zip_size, mtr, free_block);
}

/** Initialize a page in buffer pool while initializing the
deferred tablespace
@param space_id		space identfier
@param zip_size		ROW_FORMAT=COMPRESSED page size or 0
@param mtr		mini-transaction
@param free_block 	pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t* buf_page_create_deferred(uint32_t space_id, ulint zip_size,
                                      mtr_t *mtr,
                                      buf_block_t *free_block) noexcept
{
  return buf_page_create_low({space_id, 0}, zip_size, mtr, free_block);
}

/** Monitor the buffer page read/write activity, and increment corresponding
counter value in MONITOR_MODULE_BUF_PAGE.
@param bpage   buffer page whose read or write was completed
@param read    true=read, false=write */
ATTRIBUTE_COLD
void buf_page_monitor(const buf_page_t &bpage, bool read) noexcept
{
	monitor_id_t	counter;

	const byte* frame = bpage.zip.data ? bpage.zip.data : bpage.frame;

	switch (fil_page_get_type(frame)) {
	case FIL_PAGE_TYPE_INSTANT:
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		if (page_is_leaf(frame)) {
			counter = MONITOR_RW_COUNTER(
				read, MONITOR_INDEX_LEAF_PAGE);
		} else {
			counter = MONITOR_RW_COUNTER(
				read, MONITOR_INDEX_NON_LEAF_PAGE);
		}
		break;

	case FIL_PAGE_UNDO_LOG:
		counter = MONITOR_RW_COUNTER(read, MONITOR_UNDO_LOG_PAGE);
		break;

	case FIL_PAGE_INODE:
		counter = MONITOR_RW_COUNTER(read, MONITOR_INODE_PAGE);
		break;

	case FIL_PAGE_TYPE_SYS:
		counter = MONITOR_RW_COUNTER(read, MONITOR_SYSTEM_PAGE);
		break;

	case FIL_PAGE_TYPE_TRX_SYS:
		counter = MONITOR_RW_COUNTER(read, MONITOR_TRX_SYSTEM_PAGE);
		break;

	case FIL_PAGE_TYPE_FSP_HDR:
		counter = MONITOR_RW_COUNTER(read, MONITOR_FSP_HDR_PAGE);
		break;

	case FIL_PAGE_TYPE_XDES:
		counter = MONITOR_RW_COUNTER(read, MONITOR_XDES_PAGE);
		break;

	case FIL_PAGE_TYPE_BLOB:
		counter = MONITOR_RW_COUNTER(read, MONITOR_BLOB_PAGE);
		break;

	case FIL_PAGE_TYPE_ZBLOB:
		counter = MONITOR_RW_COUNTER(read, MONITOR_ZBLOB_PAGE);
		break;

	case FIL_PAGE_TYPE_ZBLOB2:
		counter = MONITOR_RW_COUNTER(read, MONITOR_ZBLOB2_PAGE);
		break;

	default:
		counter = MONITOR_RW_COUNTER(read, MONITOR_OTHER_PAGE);
	}

	MONITOR_INC_NOCHECK(counter);
}

/** Check if the encrypted page is corrupted for the full crc32 format.
@param[in]	space_id	page belongs to space id
@param[in]	d		page
@param[in]	is_compressed	compressed page
@return true if page is corrupted or false if it isn't */
static bool buf_page_full_crc32_is_corrupted(ulint space_id, const byte* d,
                                             bool is_compressed) noexcept
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
@param[in]	node		data file
@return	whether the operation succeeded
@retval	DB_SUCCESS		if page has been read and is not corrupted
@retval	DB_PAGE_CORRUPTED	if page based on checksum check is corrupted
@retval DB_CORRUPTION		if the page LSN is in the future
@retval	DB_DECRYPTION_FAILED	if page post encryption checksum matches but
after decryption normal page checksum does not match. */
static dberr_t buf_page_check_corrupt(buf_page_t *bpage,
                                      const fil_node_t &node)
{
	ut_ad(node.space->referenced());

	byte* dst_frame = bpage->zip.data ? bpage->zip.data : bpage->frame;
	dberr_t err = DB_SUCCESS;
	uint key_version = buf_page_get_key_version(dst_frame,
						    node.space->flags);

	/* In buf_decrypt_after_read we have either decrypted the page if
	page post encryption checksum matches and used key_id is found
	from the encryption plugin. If checksum did not match page was
	not decrypted and it could be either encrypted and corrupted
	or corrupted or good page. If we decrypted, there page could
	still be corrupted if used key does not match. */
	const bool seems_encrypted = !node.space->full_crc32() && key_version
		&& node.space->crypt_data
		&& node.space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED;
	ut_ad(!node.space->is_temporary() || node.space->full_crc32());

	/* If traditional checksums match, we assume that page is
	not anymore encrypted. */
	if (node.space->full_crc32()
	    && !buf_is_zeroes(span<const byte>(dst_frame,
					       node.space->physical_size()))
	    && (key_version || node.space->is_compressed()
		|| node.space->is_temporary())) {
		if (buf_page_full_crc32_is_corrupted(
			    bpage->id().space(), dst_frame,
			    node.space->is_compressed())) {
			err = DB_PAGE_CORRUPTED;
		}
	} else {
		switch (buf_page_is_corrupted(true, dst_frame,
					      node.space->flags)) {
		case NOT_CORRUPTED:
			break;
		case CORRUPTED_OTHER:
			err = DB_PAGE_CORRUPTED;
			break;
		case CORRUPTED_FUTURE_LSN:
			err = DB_CORRUPTION;
			break;
		}
	}

	if (seems_encrypted && err == DB_PAGE_CORRUPTED
	    && bpage->id().page_no() != 0) {
		err = DB_DECRYPTION_FAILED;

		ib::error()
			<< "The page " << bpage->id()
			<< " in file '" << node.name
			<< "' cannot be decrypted; key_version="
			<< key_version;
	}

	return (err);
}

/** Complete a read of a page.
@param node     data file
@return whether the operation succeeded
@retval DB_PAGE_CORRUPTED    if the checksum or the page ID is incorrect
@retval DB_DECRYPTION_FAILED if the page cannot be decrypted */
dberr_t buf_page_t::read_complete(const fil_node_t &node) noexcept
{
  const page_id_t expected_id{id()};
  ut_ad(is_read_fixed());
  ut_ad(!buf_dblwr.is_inside(id()));
  ut_ad(id().space() == node.space->id);
  ut_ad(zip_size() == node.space->zip_size());
  ut_ad(!!zip.ssize == !!zip.data);

  const byte *read_frame= zip.data ? zip.data : frame;
  ut_ad(read_frame);

  dberr_t err;
  if (!buf_page_decrypt_after_read(this, node))
  {
    err= DB_DECRYPTION_FAILED;
    goto database_corrupted;
  }

  if (belongs_to_unzip_LRU())
  {
    buf_pool.n_pend_unzip++;
    auto ok= buf_zip_decompress(reinterpret_cast<buf_block_t*>(this), false);
    buf_pool.n_pend_unzip--;

    if (!ok)
    {
      err= DB_PAGE_CORRUPTED;
      goto database_corrupted_compressed;
    }
  }

  {
    const page_id_t read_id(mach_read_from_4(read_frame + FIL_PAGE_SPACE_ID),
                            mach_read_from_4(read_frame + FIL_PAGE_OFFSET));

    if (read_id == expected_id);
    else if (read_id == page_id_t(0, 0))
    {
      /* This is likely an uninitialized (all-zero) page. */
      err= DB_FAIL;
      goto release_page;
    }
    else if (!node.space->full_crc32() &&
             page_id_t(0, read_id.page_no()) == expected_id)
      /* FIL_PAGE_SPACE_ID was written as garbage in the system tablespace
      before MySQL 4.1.1, which introduced innodb_file_per_table. */;
    else if (node.space->full_crc32() &&
             *reinterpret_cast<const uint32_t*>
             (&read_frame[FIL_PAGE_FCRC32_KEY_VERSION]) &&
             node.space->crypt_data &&
             node.space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED)
    {
      err= DB_DECRYPTION_FAILED;
      goto release_page;
    }
    else
    {
      sql_print_error("InnoDB: Space id and page no stored in the page,"
                      " read in from %s are "
                      "[page id: space=" UINT32PF ", page number=" UINT32PF
                      "], should be "
                      "[page id: space=" UINT32PF ", page number=" UINT32PF
                      "]",
                      node.name,
                      read_id.space(), read_id.page_no(),
                      expected_id.space(), expected_id.page_no());
      err= DB_FAIL;
      goto release_page;
    }
  }

  err= buf_page_check_corrupt(this, node);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
database_corrupted:
    if (belongs_to_unzip_LRU())
database_corrupted_compressed:
      memset_aligned<UNIV_PAGE_SIZE_MIN>(frame, 0, srv_page_size);

    if (!srv_force_recovery)
      goto release_page;

    if (err == DB_PAGE_CORRUPTED || err == DB_DECRYPTION_FAILED)
    {
release_page:
      if (node.space->full_crc32() && node.space->crypt_data &&
          recv_recovery_is_on() &&
          recv_sys.dblwr.find_deferred_page(node, id().page_no(),
                                            const_cast<byte*>(read_frame)))
      {
        /* Recover from doublewrite buffer */
        err= DB_SUCCESS;
        goto success_page;
      }

      if (recv_sys.free_corrupted_page(expected_id, node));
      else if (err == DB_FAIL)
        err= DB_PAGE_CORRUPTED;
      else
      {
        sql_print_error("InnoDB: Failed to read page " UINT32PF
                        " from file '%s': %s", expected_id.page_no(),
                        node.name, ut_strerr(err));

        buf_page_print(read_frame, zip_size());

        if (node.space->set_corrupted() &&
            !is_predefined_tablespace(node.space->id))
          sql_print_information("InnoDB: You can use CHECK TABLE to scan"
                                " your table for corruption. %s",
                                FORCE_RECOVERY_MSG);
      }

      buf_pool.corrupted_evict(this, buf_page_t::READ_FIX);
      return err;
    }
  }
success_page:

  const bool recovery= frame && recv_recovery_is_on();

  if (recovery && !recv_recover_page(node.space, this))
    return DB_PAGE_CORRUPTED;

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(*this, true);
  DBUG_PRINT("ib_buf", ("read page %u:%u", id().space(), id().page_no()));

  if (!recovery)
  {
    ut_d(auto f=) zip.fix.fetch_sub(READ_FIX - UNFIXED);
    ut_ad(f >= READ_FIX);
    ut_ad(f < WRITE_FIX);
  }

  lock.x_unlock(true);

  return DB_SUCCESS;
}

#ifdef BTR_CUR_HASH_ADAPT
/** Clear the adaptive hash index on all pages in the buffer pool. */
ATTRIBUTE_COLD void buf_pool_t::clear_hash_index() noexcept
{
  std::set<dict_index_t*> garbage;

  mysql_mutex_lock(&mutex);
  ut_ad(!btr_search_enabled);

  for (char *extent= memory,
         *end= memory + block_descriptors_in_bytes(n_blocks);
       extent < end; extent+= innodb_buffer_pool_extent_size)
    for (buf_block_t *block= reinterpret_cast<buf_block_t*>(extent),
           *extent_end= block +
           pages_in_extent[srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN];
         block < extent_end && reinterpret_cast<char*>(block) < end; block++)
    {
      dict_index_t *index= block->index;
      assert_block_ahi_valid(block);

      /* We can clear block->index and block->n_pointers when
      holding all AHI latches exclusively; see the comments in buf0buf.h */

      if (!index)
      {
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
        ut_a(!block->n_pointers);
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
        continue;
      }

      ut_d(const auto s= block->page.state());
      /* Another thread may have set the state to
      REMOVE_HASH in buf_LRU_block_remove_hashed().

      The state change in buf_pool_t::resize() is not observable
      here, because in that case we would have !block->index.

      In the end, the entire adaptive hash index will be removed. */
      ut_ad(s >= buf_page_t::UNFIXED || s == buf_page_t::REMOVE_HASH);
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
      block->n_pointers= 0;
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
      if (index->freed())
        garbage.insert(index);
      block->index= nullptr;
    }

  mysql_mutex_unlock(&mutex);

  for (dict_index_t *index : garbage)
    btr_search_lazy_free(index);
}
#endif /* BTR_CUR_HASH_ADAPT */

#ifdef UNIV_DEBUG
/** Check that all blocks are in a replaceable state.
@return address of a non-free block
@retval nullptr if all freed */
void buf_pool_t::assert_all_freed() noexcept
{
  mysql_mutex_lock(&mutex);

    for (char *extent= memory,
           *end= memory + block_descriptors_in_bytes(n_blocks);
         extent < end; extent+= innodb_buffer_pool_extent_size)
      for (buf_block_t *block= reinterpret_cast<buf_block_t*>(extent),
             *extent_end= block +
             pages_in_extent[srv_page_size_shift - UNIV_PAGE_SIZE_SHIFT_MIN];
           block < extent_end && reinterpret_cast<char*>(block) < end; block++)
    {
      if (!block->page.in_file())
        continue;
      switch (const lsn_t lsn= block->page.oldest_modification()) {
      case 0:
      case 1:
        break;

      case 2:
        ut_ad(fsp_is_system_temporary(block->page.id().space()));
        break;

      default:
        if (srv_read_only_mode)
        {
          /* The page cleaner is disabled in read-only mode.  No pages
          can be dirtied, so all of them must be clean. */
          ut_ad(lsn == recv_sys.lsn ||
                srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
          break;
        }

        goto fixed_or_dirty;
      }

      if (!block->page.can_relocate())
      fixed_or_dirty:
        ib::fatal() << "Page " << block->page.id() << " still fixed or dirty";
    }

  mysql_mutex_unlock(&mutex);
}
#endif /* UNIV_DEBUG */

/** Refresh the statistics used to print per-second averages. */
void buf_refresh_io_stats() noexcept
{
	buf_pool.last_printout_time = time(NULL);
	buf_pool.old_stat = buf_pool.stat;
}

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate() noexcept
{
	/* It is possible that a write batch that has been posted
	earlier is still not complete. For buffer pool invalidation to
	proceed we must ensure there is NO write activity happening. */

	os_aio_wait_until_no_pending_writes(false);
	ut_d(buf_pool.assert_all_freed());
	mysql_mutex_lock(&buf_pool.mutex);

	while (UT_LIST_GET_LEN(buf_pool.LRU)) {
		buf_LRU_scan_and_free_block();
	}

	ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);

	buf_pool.freed_page_clock = 0;
	buf_pool.LRU_old = NULL;
	buf_pool.LRU_old_len = 0;
	buf_pool.stat.init();

	buf_refresh_io_stats();
	mysql_mutex_unlock(&buf_pool.mutex);
}

#ifdef UNIV_DEBUG
/** Validate the buffer pool. */
void buf_pool_t::validate() noexcept
{
	ulint		n_lru		= 0;
	ulint		n_flushing	= 0;
	ulint		n_free		= 0;
	ulint		n_zip		= 0;

	mysql_mutex_lock(&mutex);

	/* Check the uncompressed blocks. */

	for (ulint i = 0; i < n_blocks; i++) {
		const buf_block_t* block = get_nth_page(i);
		ut_ad(block->page.frame == block->frame_address());

		switch (const auto f = block->page.state()) {
		case buf_page_t::NOT_USED:
			ut_ad(!block->page.in_LRU_list);
			n_free++;
			break;
		case buf_page_t::MEMORY:
		case buf_page_t::REMOVE_HASH:
			/* do nothing */
			break;
		default:
			if (f >= buf_page_t::READ_FIX
			    && f < buf_page_t::WRITE_FIX) {
				/* A read-fixed block is not
				necessarily in the page_hash yet. */
				break;
			}
			ut_ad(f >= buf_page_t::FREED);
			const page_id_t id{block->page.id()};
			ut_ad(page_hash.get(
				      id,
				      page_hash.cell_get(id.fold()))
			      == &block->page);
			n_lru++;
		}
	}

	/* Check dirty blocks. */

	mysql_mutex_lock(&flush_list_mutex);
	for (buf_page_t* b = UT_LIST_GET_FIRST(flush_list); b;
	     b = UT_LIST_GET_NEXT(list, b)) {
		ut_ad(b->in_file());
		ut_ad(b->oldest_modification());
		ut_ad(!fsp_is_system_temporary(b->id().space()));
		n_flushing++;

		if (UNIV_UNLIKELY(!b->frame)) {
			n_lru++;
			n_zip++;
		}
		const page_id_t id{b->id()};
		ut_ad(page_hash.get(id, page_hash.cell_get(id.fold())) == b);
	}

	ut_ad(UT_LIST_GET_LEN(flush_list) == n_flushing);

	mysql_mutex_unlock(&flush_list_mutex);
	ut_ad(n_lru + n_free <= n_blocks + n_zip);
	ut_ad(UT_LIST_GET_LEN(LRU) >= n_lru);
	ut_ad(UT_LIST_GET_LEN(free) <= n_free);
	ut_ad(size_in_bytes != size_in_bytes_requested
	      || UT_LIST_GET_LEN(free) == n_free);

	mysql_mutex_unlock(&mutex);

	ut_d(buf_LRU_validate());
	ut_d(buf_flush_validate());
}
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
/** Write information of the buf_pool to the error log. */
void buf_pool_t::print() noexcept
{
	index_id_t*	index_ids;
	ulint*		counts;
	ulint		i;
	index_id_t	id;
	ulint		n_found;
	dict_index_t*	index;

	mysql_mutex_lock(&mutex);

	index_ids = static_cast<index_id_t*>(
		ut_malloc_nokey(n_blocks * sizeof *index_ids));

	counts = static_cast<ulint*>(
		ut_malloc_nokey(sizeof(ulint) * n_blocks));

	mysql_mutex_lock(&flush_list_mutex);

	ib::info()
		<< "[buffer pool: size=" << n_blocks
		<< ", database pages=" << UT_LIST_GET_LEN(LRU)
		<< ", free pages=" << UT_LIST_GET_LEN(free)
		<< ", modified database pages="
		<< UT_LIST_GET_LEN(flush_list)
		<< ", n pending decompressions=" << n_pend_unzip
		<< ", n pending flush LRU=" << n_flush()
		<< " list=" << os_aio_pending_writes()
		<< ", pages made young=" << stat.n_pages_made_young
		<< ", not young=" << stat.n_pages_not_made_young
		<< ", pages read=" << stat.n_pages_read
		<< ", created=" << stat.n_pages_created
		<< ", written=" << stat.n_pages_written << "]";

	mysql_mutex_unlock(&flush_list_mutex);

	/* Count the number of blocks belonging to each index in the buffer */

	n_found = 0;

	for (size_t i = 0; i < n_blocks; i++) {
		buf_block_t* block = get_nth_page(i);
		const buf_frame_t* frame = block->page.frame;
		ut_ad(frame == block->frame_address());

		if (fil_page_index_page_check(frame)) {

			id = btr_page_get_index_id(frame);

			/* Look for the id in the index_ids array */
			for (ulint j = 0; j < n_found; j++) {
				if (index_ids[j] == id) {
					counts[j]++;
					goto found;
				}
			}

			index_ids[n_found] = id;
			counts[n_found] = 1;
			n_found++;
found:
			continue;
		}
	}

	mysql_mutex_unlock(&mutex);

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
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */

#ifdef UNIV_DEBUG
/** @return the number of latched pages in the buffer pool */
ulint buf_get_latched_pages_number() noexcept
{
  ulint fixed_pages_number= 0;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  for (buf_page_t *b= UT_LIST_GET_FIRST(buf_pool.LRU); b;
       b= UT_LIST_GET_NEXT(LRU, b))
    if (b->state() > buf_page_t::UNFIXED)
      fixed_pages_number++;

  return fixed_pages_number;
}
#endif /* UNIV_DEBUG */

void buf_pool_t::get_info(buf_pool_info_t *pool_info) noexcept
{
  mysql_mutex_lock(&mutex);
  pool_info->pool_size= curr_size();
  pool_info->lru_len= UT_LIST_GET_LEN(LRU);
  pool_info->old_lru_len= LRU_old_len;
  pool_info->free_list_len= UT_LIST_GET_LEN(free);

  mysql_mutex_lock(&flush_list_mutex);
  pool_info->flush_list_len= UT_LIST_GET_LEN(flush_list);
  pool_info->n_pend_unzip= UT_LIST_GET_LEN(unzip_LRU);
  pool_info->n_pend_reads= os_aio_pending_reads_approx();
  pool_info->n_pending_flush_lru= n_flush();
  pool_info->n_pending_flush_list= os_aio_pending_writes();
  mysql_mutex_unlock(&flush_list_mutex);

  double elapsed= 0.001 + difftime(time(nullptr), last_printout_time);

  pool_info->n_pages_made_young= stat.n_pages_made_young;
  pool_info->page_made_young_rate=
    double(stat.n_pages_made_young - old_stat.n_pages_made_young) /
    elapsed;
  pool_info->n_pages_not_made_young= stat.n_pages_not_made_young;
  pool_info->page_not_made_young_rate=
    double(stat.n_pages_not_made_young - old_stat.n_pages_not_made_young) /
    elapsed;
  pool_info->n_pages_read= stat.n_pages_read;
  pool_info->pages_read_rate=
    double(stat.n_pages_read - old_stat.n_pages_read) / elapsed;
  pool_info->n_pages_created= stat.n_pages_created;
  pool_info->pages_created_rate=
    double(stat.n_pages_created - old_stat.n_pages_created) / elapsed;
  pool_info->n_pages_written= stat.n_pages_written;
  pool_info->pages_written_rate=
    double(stat.n_pages_written - old_stat.n_pages_written) / elapsed;
  pool_info->n_page_gets= stat.n_page_gets;
  pool_info->n_page_get_delta= stat.n_page_gets - old_stat.n_page_gets;
  if (pool_info->n_page_get_delta)
  {
    pool_info->page_read_delta= stat.n_pages_read - old_stat.n_pages_read;
    pool_info->young_making_delta=
      stat.n_pages_made_young - old_stat.n_pages_made_young;
    pool_info->not_young_making_delta=
      stat.n_pages_not_made_young - old_stat.n_pages_not_made_young;
  }
  pool_info->n_ra_pages_read_rnd= stat.n_ra_pages_read_rnd;
  pool_info->pages_readahead_rnd_rate=
    double(stat.n_ra_pages_read_rnd - old_stat.n_ra_pages_read_rnd) / elapsed;
  pool_info->n_ra_pages_read= stat.n_ra_pages_read;
  pool_info->pages_readahead_rate=
    double(stat.n_ra_pages_read - old_stat.n_ra_pages_read) / elapsed;
  pool_info->n_ra_pages_evicted= stat.n_ra_pages_evicted;
  pool_info->pages_evicted_rate=
    double(stat.n_ra_pages_evicted - old_stat.n_ra_pages_evicted) / elapsed;
  pool_info->unzip_lru_len= UT_LIST_GET_LEN(unzip_LRU);
  pool_info->io_sum= buf_LRU_stat_sum.io;
  pool_info->io_cur= buf_LRU_stat_cur.io;
  pool_info->unzip_sum= buf_LRU_stat_sum.unzip;
  pool_info->unzip_cur= buf_LRU_stat_cur.unzip;
  buf_refresh_io_stats();
  mysql_mutex_unlock(&mutex);
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
		"Pending writes: LRU " ULINTPF ", flush list " ULINTPF "\n",
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
		pool_info->n_pending_flush_list);

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

	buf_pool.get_info(&pool_info);
	buf_print_io_instance(&pool_info, file);
}

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param page       page frame
@param fsp_flags  contents of FSP_SPACE_FLAGS
@return whether the page is encrypted and valid */
bool buf_page_verify_crypt_checksum(const byte *page, uint32_t fsp_flags) noexcept
{
	if (!fil_space_t::full_crc32(fsp_flags)) {
		return fil_space_verify_crypt_checksum(
			page, fil_space_t::zip_size(fsp_flags));
	}

	return !buf_page_is_corrupted(true, page, fsp_flags);
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
#endif /* !UNIV_INNOCHECKSUM */
