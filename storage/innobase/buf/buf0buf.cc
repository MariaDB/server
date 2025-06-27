/*****************************************************************************

Copyright (c) 1995, 2018, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

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
#include "ut0crc32.h"
#include "mariadb_stats.h"
#include <string.h>

#ifdef UNIV_INNOCHECKSUM
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
#endif /* !UNIV_INNOCHECKSUM */
#include "page0zip.h"
#include "buf0dump.h"
#include <map>
#include <sstream>
#include "log.h"

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
buf_pool_t::chunk_t::map *buf_pool_t::chunk_t::map_reg;
buf_pool_t::chunk_t::map *buf_pool_t::chunk_t::map_ref;

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

	memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(src_frame, tmp_frame,
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
/** Checks whether the lsn present in the page is lesser than the
peek current lsn.
@param check_lsn    lsn to check
@param read_buf     page frame
@return whether the FIL_PAGE_LSN is invalid */
static bool buf_page_check_lsn(bool check_lsn, const byte *read_buf)
{
  if (!check_lsn)
    return false;
  lsn_t current_lsn= log_sys.get_lsn();
  if (UNIV_UNLIKELY(current_lsn == LOG_START_LSN + LOG_BLOCK_HDR_SIZE) &&
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
@param[in]	check_lsn	whether the LSN should be checked
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return whether the page is corrupted */
buf_page_is_corrupted_reason
buf_page_is_corrupted(
	bool			check_lsn,
	const byte*		read_buf,
	ulint			fsp_flags) noexcept
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
			    && page_counter++ == 3) {
				crc32++;
			}
		});

		if (crc32 != ut_crc32(read_buf,
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
				    && page_counter++ == 3)
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

#if defined(DBUG_OFF) && defined(HAVE_MADVISE) &&  defined(MADV_DODUMP)
/** Enable buffers to be dumped to core files

A convience function, not called anyhwere directly however
it is left available for gdb or any debugger to call
in the event that you want all of the memory to be dumped
to a core file.

Returns number of errors found in madvise calls. */
MY_ATTRIBUTE((used))
int
buf_madvise_do_dump()
{
	int ret= 0;

	/* mirrors allocation in log_t::create() */
	if (log_sys.buf) {
		ret += madvise(log_sys.buf,
			       srv_log_buffer_size,
			       MADV_DODUMP);
		ret += madvise(log_sys.flush_buf,
			       srv_log_buffer_size,
			       MADV_DODUMP);
	}
	/* mirrors recv_sys_t::create() */
	if (recv_sys.buf)
	{
		ret+= madvise(recv_sys.buf, recv_sys.len, MADV_DODUMP);
	}

	mysql_mutex_lock(&buf_pool.mutex);
	auto chunk = buf_pool.chunks;

	for (ulint n = buf_pool.n_chunks; n--; chunk++) {
		ret+= madvise(chunk->mem, chunk->mem_size(), MADV_DODUMP);
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	return ret;
}
#endif

#ifndef UNIV_DEBUG
static inline byte hex_to_ascii(byte hex_digit)
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

/** Initialize a buffer page descriptor.
@param[in,out]	block	buffer page descriptor
@param[in]	frame	buffer page frame */
static
void
buf_block_init(buf_block_t* block, byte* frame)
{
	/* This function should only be executed at database startup or by
	buf_pool.resize(). Either way, adaptive hash index must not exist. */
	assert_block_ahi_empty_on_init(block);

	block->page.frame = frame;

	MEM_MAKE_DEFINED(&block->modify_clock, sizeof block->modify_clock);
	ut_ad(!block->modify_clock);
	MEM_MAKE_DEFINED(&block->page.lock, sizeof block->page.lock);
	block->page.lock.init();
	block->page.init(buf_page_t::NOT_USED, page_id_t(~0ULL));
#ifdef BTR_CUR_HASH_ADAPT
	MEM_MAKE_DEFINED(&block->index, sizeof block->index);
	ut_ad(!block->index);
#endif /* BTR_CUR_HASH_ADAPT */
	ut_d(block->in_unzip_LRU_list = false);
	ut_d(block->in_withdraw_list = false);

	page_zip_des_init(&block->page.zip);

	MEM_MAKE_DEFINED(&block->page.hash, sizeof block->page.hash);
	ut_ad(!block->page.hash);
}

/** Allocate a chunk of buffer frames.
@param bytes    requested size
@return whether the allocation succeeded */
inline bool buf_pool_t::chunk_t::create(size_t bytes) noexcept
{
  DBUG_EXECUTE_IF("ib_buf_chunk_init_fails", return false;);
  /* Round down to a multiple of page size, although it already should be. */
  bytes= ut_2pow_round<size_t>(bytes, srv_page_size);

  mem= buf_pool.allocator.allocate_large_dontdump(bytes, &mem_pfx);

  if (UNIV_UNLIKELY(!mem))
    return false;

  MEM_UNDEFINED(mem, mem_size());

#ifdef HAVE_LIBNUMA
  if (srv_numa_interleave)
  {
    struct bitmask *numa_mems_allowed= numa_get_mems_allowed();
    MEM_MAKE_DEFINED(numa_mems_allowed, sizeof *numa_mems_allowed);
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
    MEM_UNDEFINED(block->page.frame, srv_page_size);
    /* Add the block to the free list */
    UT_LIST_ADD_LAST(buf_pool.free, &block->page);

    ut_d(block->page.in_free_list = TRUE);
    block++;
    frame+= srv_page_size;
  }

  reg();

  return true;
}

#ifdef UNIV_DEBUG
/** Check that all file pages in the buffer chunk are in a replaceable state.
@return address of a non-free block
@retval nullptr if all freed */
inline const buf_block_t *buf_pool_t::chunk_t::not_freed() const noexcept
{
  buf_block_t *block= blocks;
  for (auto i= size; i--; block++)
  {
    if (block->page.in_file())
    {
      /* The uncompressed buffer pool should never
      contain ROW_FORMAT=COMPRESSED block descriptors. */
      ut_ad(block->page.frame);
      const lsn_t lsn= block->page.oldest_modification();

      if (srv_read_only_mode)
      {
        /* The page cleaner is disabled in read-only mode.  No pages
        can be dirtied, so all of them must be clean. */
        ut_ad(lsn == 0 || lsn == recv_sys.recovered_lsn ||
              srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
        break;
      }

      if (fsp_is_system_temporary(block->page.id().space()))
      {
        ut_ad(lsn == 0 || lsn == 2);
        break;
      }

      if (lsn > 1 || !block->page.can_relocate())
        return block;

      break;
    }
  }

  return nullptr;
}
#endif /* UNIV_DEBUG */

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

/** Create the buffer pool.
@return whether the creation failed */
bool buf_pool_t::create()
{
  ut_ad(this == &buf_pool);
  ut_ad(srv_buf_pool_size % srv_buf_pool_chunk_unit == 0);
  ut_ad(!is_initialised());
  ut_ad(srv_buf_pool_size > 0);
  ut_ad(!resizing);
  ut_ad(!chunks_old);
  /* mariabackup loads tablespaces, and it requires field_ref_zero to be
  allocated before innodb initialization */
  ut_ad(srv_operation >= SRV_OPERATION_RESTORE || !field_ref_zero);

  NUMA_MEMPOLICY_INTERLEAVE_IN_SCOPE;

  if (!field_ref_zero) {
    if (auto b= aligned_malloc(UNIV_PAGE_SIZE_MAX, 4096))
      field_ref_zero= static_cast<const byte*>
        (memset_aligned<4096>(b, 0, UNIV_PAGE_SIZE_MAX));
    else
      return true;
  }

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
          block->page.lock.free();

        allocator.deallocate_large_dodump(chunk->mem, &chunk->mem_pfx);
      }
      ut_free(chunks);
      chunks= nullptr;
      UT_DELETE(chunk_t::map_reg);
      chunk_t::map_reg= nullptr;
      aligned_free(const_cast<byte*>(field_ref_zero));
      field_ref_zero= nullptr;
      ut_ad(!is_initialised());
      return true;
    }

    curr_size+= chunk->size;
  }
  while (++chunk < chunks + n_chunks);

  ut_ad(is_initialised());
  mysql_mutex_init(buf_pool_mutex_key, &mutex, MY_MUTEX_INIT_FAST);

  UT_LIST_INIT(LRU, &buf_page_t::LRU);
  UT_LIST_INIT(withdraw, &buf_page_t::list);
  withdraw_target= 0;
  UT_LIST_INIT(flush_list, &buf_page_t::list);
  UT_LIST_INIT(unzip_LRU, &buf_block_t::unzip_LRU);

  for (size_t i= 0; i < UT_ARR_SIZE(zip_free); ++i)
    UT_LIST_INIT(zip_free[i], &buf_buddy_free_t::list);
  ulint s= curr_size;
  s/= BUF_READ_AHEAD_PORTION;
  read_ahead_area= s >= READ_AHEAD_PAGES
    ? READ_AHEAD_PAGES
    : my_round_up_to_next_power(static_cast<uint32_t>(s));
  curr_pool_size= srv_buf_pool_size;

  n_chunks_new= n_chunks;

  page_hash.create(2 * curr_size);
  zip_hash.create(2 * curr_size);
  last_printout_time= time(NULL);

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

  /* FIXME: remove some of these variables */
  srv_buf_pool_curr_size= curr_pool_size;
  srv_buf_pool_old_size= srv_buf_pool_size;
  srv_buf_pool_base_size= srv_buf_pool_size;

  last_activity_count= srv_get_activity_count();

  chunk_t::map_ref= chunk_t::map_reg;
  buf_LRU_old_ratio_update(100 * 3 / 8, false);
  btr_search_sys_create();
  ut_ad(is_initialised());
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

  for (auto chunk= chunks + n_chunks; --chunk >= chunks; )
  {
    buf_block_t *block= chunk->blocks;

    for (auto i= chunk->size; i--; block++)
      block->page.lock.free();

    allocator.deallocate_large_dodump(chunk->mem, &chunk->mem_pfx);
  }

  pthread_cond_destroy(&done_flush_LRU);
  pthread_cond_destroy(&done_flush_list);
  pthread_cond_destroy(&do_flush_list);
  pthread_cond_destroy(&done_free);

  ut_free(chunks);
  chunks= nullptr;
  page_hash.free();
  zip_hash.free();

  io_buf.close();
  UT_DELETE(chunk_t::map_reg);
  chunk_t::map_reg= chunk_t::map_ref= nullptr;
  aligned_free(const_cast<byte*>(field_ref_zero));
  field_ref_zero= nullptr;
}

/** Try to reallocate a control block.
@param block  control block to reallocate
@return whether the reallocation succeeded */
inline bool buf_pool_t::realloc(buf_block_t *block) noexcept
{
	buf_block_t*	new_block;

	mysql_mutex_assert_owner(&mutex);
	ut_ad(block->page.in_file());
	ut_ad(block->page.frame);

	new_block = buf_LRU_get_free_only();

	if (new_block == NULL) {
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		page_cleaner_wakeup();
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		return(false); /* free list was not enough */
	}

	const page_id_t id{block->page.id()};
	hash_chain& chain = page_hash.cell_get(id.fold());
	page_hash_latch& hash_lock = page_hash.lock_get(chain);
	/* It does not make sense to use transactional_lock_guard
	here, because copying innodb_page_size (4096 to 65536) bytes
	as well as other changes would likely make the memory
	transaction too large. */
	hash_lock.lock();

	if (block->page.can_relocate()) {
		memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(
			new_block->page.frame, block->page.frame,
			srv_page_size);
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		const auto frame = new_block->page.frame;
		new_block->page.lock.free();
		new (&new_block->page) buf_page_t(block->page);
		new_block->page.frame = frame;

		/* relocate LRU list */
		if (buf_page_t*	prev_b = buf_pool.LRU_remove(&block->page)) {
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
			ut_d(new_block->in_unzip_LRU_list = true);

			buf_block_t*	prev_block = UT_LIST_GET_PREV(unzip_LRU, block);
			UT_LIST_REMOVE(unzip_LRU, block);

			ut_d(block->in_unzip_LRU_list = false);
			block->page.zip.data = NULL;
			page_zip_set_size(&block->page.zip, 0);

			if (prev_block != NULL) {
				UT_LIST_INSERT_AFTER(unzip_LRU, prev_block, new_block);
			} else {
				UT_LIST_ADD_FIRST(unzip_LRU, new_block);
			}
		} else {
			ut_ad(!block->in_unzip_LRU_list);
			ut_d(new_block->in_unzip_LRU_list = false);
		}

		/* relocate page_hash */
		hash_chain& chain = page_hash.cell_get(id.fold());
		ut_ad(&block->page == page_hash.get(id, chain));
		buf_pool.page_hash.replace(chain, &block->page,
					   &new_block->page);
		buf_block_modify_clock_inc(block);
		static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
		memset_aligned<4>(block->page.frame
				  + FIL_PAGE_OFFSET, 0xff, 4);
		static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
			      "not perfect alignment");
		memset_aligned<2>(block->page.frame
				  + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		MEM_UNDEFINED(block->page.frame, srv_page_size);
		block->page.set_state(buf_page_t::REMOVE_HASH);
		if (!fsp_is_system_temporary(id.space())) {
			buf_flush_relocate_on_flush_list(&block->page,
							 &new_block->page);
		}
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		block->page.set_corrupt_id();

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
		ut_d(block->page.set_state(buf_page_t::MEMORY));
		/* free block */
		new_block = block;
	}

	hash_lock.unlock();
	buf_LRU_block_free_non_file_page(new_block);
	return(true); /* free_list was enough */
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
inline bool buf_pool_t::withdraw_blocks() noexcept
{
	buf_block_t*	block;
	ulint		loop_count = 0;

	ib::info() << "start to withdraw the last "
		<< withdraw_target << " blocks";

	while (UT_LIST_GET_LEN(withdraw) < withdraw_target) {

		/* try to withdraw from free_list */
		ulint	count1 = 0;

		mysql_mutex_lock(&mutex);
		buf_buddy_condense_free();
		block = reinterpret_cast<buf_block_t*>(
			UT_LIST_GET_FIRST(free));
		while (block != NULL
		       && UT_LIST_GET_LEN(withdraw) < withdraw_target) {
			ut_ad(block->page.in_free_list);
			ut_ad(!block->page.oldest_modification());
			ut_ad(!block->page.in_LRU_list);
			ut_a(!block->page.in_file());

			buf_block_t*	next_block;
			next_block = reinterpret_cast<buf_block_t*>(
				UT_LIST_GET_NEXT(
					list, &block->page));

			if (will_be_withdrawn(block->page)) {
				/* This should be withdrawn */
				UT_LIST_REMOVE(free, &block->page);
				UT_LIST_ADD_LAST(withdraw, &block->page);
				ut_d(block->in_withdraw_list = true);
				count1++;
			}

			block = next_block;
		}

		/* reserve free_list length */
		if (UT_LIST_GET_LEN(withdraw) < withdraw_target) {
			try_LRU_scan = false;
			mysql_mutex_unlock(&mutex);
			mysql_mutex_lock(&flush_list_mutex);
			page_cleaner_wakeup(true);
			my_cond_wait(&done_flush_list,
				     &flush_list_mutex.m_mutex);
			mysql_mutex_unlock(&flush_list_mutex);
			mysql_mutex_lock(&mutex);
		}

		/* relocate blocks/buddies in withdrawn area */
		ulint	count2 = 0;

		buf_pool_mutex_exit_forbid();
		for (buf_page_t* bpage = UT_LIST_GET_FIRST(LRU), *next_bpage;
		     bpage; bpage = next_bpage) {
			ut_ad(bpage->in_file());
			next_bpage = UT_LIST_GET_NEXT(LRU, bpage);
			if (UNIV_LIKELY_NULL(bpage->zip.data)
			    && will_be_withdrawn(bpage->zip.data)
			    && bpage->can_relocate()) {
				if (!buf_buddy_realloc(
					    bpage->zip.data,
					    page_zip_get_size(&bpage->zip))) {
					/* failed to allocate block */
					break;
				}
				count2++;
				if (bpage->frame) {
					goto realloc_frame;
				}
			}

			if (bpage->frame && will_be_withdrawn(*bpage)
			    && bpage->can_relocate()) {
realloc_frame:
				if (!realloc(reinterpret_cast<buf_block_t*>(
						     bpage))) {
					/* failed to allocate block */
					break;
				}
				count2++;
			}
		}
		buf_pool_mutex_exit_allow();
		mysql_mutex_unlock(&mutex);

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
			ut_a(block->page.state() == buf_page_t::NOT_USED);
			ut_ad(block->in_withdraw_list);
		}
	}

	ib::info() << "withdrawn target: " << UT_LIST_GET_LEN(withdraw)
		   << " blocks";

	return(false);
}



inline void buf_pool_t::page_hash_table::write_lock_all() noexcept
{
  for (auto n= pad(n_cells) & ~ELEMENTS_PER_LATCH;; n-= ELEMENTS_PER_LATCH + 1)
  {
    reinterpret_cast<page_hash_latch&>(array[n]).lock();
    if (!n)
      break;
  }
}


inline void buf_pool_t::page_hash_table::write_unlock_all() noexcept
{
  for (auto n= pad(n_cells) & ~ELEMENTS_PER_LATCH;; n-= ELEMENTS_PER_LATCH + 1)
  {
    reinterpret_cast<page_hash_latch&>(array[n]).unlock();
    if (!n)
      break;
  }
}


namespace
{

struct find_interesting_trx
{
  void operator()(const trx_t &trx)
  {
    if (!trx.is_started())
      return;
    if (trx.mysql_thd == nullptr)
      return;
    if (withdraw_started <= trx.start_time_micro)
      return;

    if (!found)
    {
      sql_print_warning("InnoDB: The following trx might hold "
                    "the blocks in buffer pool to "
                    "be withdrawn. Buffer pool "
                    "resizing can complete only "
                    "after all the transactions "
                    "below release the blocks.");
      found= true;
    }

    lock_trx_print_wait_and_mvcc_state(stderr, &trx, current_time);
  }

  bool &found;
  /** microsecond_interval_timer() */
  const ulonglong withdraw_started;
  const my_hrtime_t current_time;
};

} // namespace

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

#ifdef BTR_CUR_HASH_ADAPT
	/* disable AHI if needed */
	buf_resize_status("Disabling adaptive hash index.");

	btr_search_s_lock_all();
	const bool btr_search_disabled = btr_search_enabled;
	btr_search_s_unlock_all();

	btr_search_disable();

	if (btr_search_disabled) {
		ib::info() << "disabled adaptive hash index.";
	}
#endif /* BTR_CUR_HASH_ADAPT */

	mysql_mutex_lock(&mutex);
	ut_ad(n_chunks_new == n_chunks);
	ut_ad(UT_LIST_GET_LEN(withdraw) == 0);

	n_chunks_new = (new_instance_size << srv_page_size_shift)
		/ srv_buf_pool_chunk_unit;
	curr_size = n_chunks_new * chunks->size;
	mysql_mutex_unlock(&mutex);

	if (is_shrinking()) {
		/* set withdraw target */
		size_t w = 0;

		for (const chunk_t* chunk = chunks + n_chunks_new,
		     * const echunk = chunks + n_chunks;
		     chunk != echunk; chunk++)
			w += chunk->size;

		ut_ad(withdraw_target == 0);
		withdraw_target = w;
	}

	buf_resize_status("Withdrawing blocks to be shrunken.");

	ulonglong	withdraw_started = microsecond_interval_timer();
	ulonglong	message_interval = 60ULL * 1000 * 1000;
	ulint		retry_interval = 1;

withdraw_retry:
	/* wait for the number of blocks fit to the new size (if needed)*/
	bool	should_retry_withdraw = is_shrinking()
		&& withdraw_blocks();

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		/* abort to resize for shutdown. */
		return;
	}

	/* abort buffer pool load */
	buf_load_abort();

	const ulonglong current_time = microsecond_interval_timer();

	if (should_retry_withdraw
	    && current_time - withdraw_started >= message_interval) {

		if (message_interval > 900000000) {
			message_interval = 1800000000;
		} else {
			message_interval *= 2;
		}

		bool found= false;
		find_interesting_trx f
			{found, withdraw_started, my_hrtime_coarse()};
		withdraw_started = current_time;

		/* This is going to exceed the maximum size of a
		memory transaction. */
		LockMutexGuard g{SRW_LOCK_CALL};
		trx_sys.trx_list.for_each(f);
	}

	if (should_retry_withdraw) {
		ib::info() << "Will retry to withdraw " << retry_interval
			<< " seconds later.";
		std::this_thread::sleep_for(
			std::chrono::seconds(retry_interval));

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
				should_wait = true;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(10)););
		}
	}
#endif /* !DBUG_OFF */

	if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
		return;
	}

	/* Indicate critical path */
	resizing.store(true, std::memory_order_relaxed);

  mysql_mutex_lock(&mutex);
  page_hash.write_lock_all();

	chunk_t::map_reg = UT_NEW_NOKEY(chunk_t::map());

	/* add/delete chunks */

	buf_resize_status("buffer pool resizing with chunks "
			  ULINTPF " to " ULINTPF ".",
			  n_chunks, n_chunks_new);

	if (is_shrinking()) {
		/* delete chunks */
		chunk_t* chunk = chunks + n_chunks_new;
		const chunk_t* const echunk = chunks + n_chunks;

		ulint	sum_freed = 0;

		while (chunk < echunk) {
			/* buf_LRU_block_free_non_file_page() invokes
			MEM_NOACCESS() on any buf_pool.free blocks.
			We must cancel the effect of that. In
			MemorySanitizer, MEM_NOACCESS() is no-op, so
			we must not do anything special for it here. */
#ifdef HAVE_valgrind
# if !__has_feature(memory_sanitizer)
			MEM_MAKE_DEFINED(chunk->mem, chunk->mem_size());
# endif
#else
			MEM_MAKE_ADDRESSABLE(chunk->mem, chunk->size);
#endif

			buf_block_t*	block = chunk->blocks;

			for (ulint j = chunk->size; j--; block++) {
				block->page.lock.free();
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

		ulint	n_chunks_copy = ut_min(n_chunks_new, n_chunks);

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
  ulint s= curr_size;
  s/= BUF_READ_AHEAD_PORTION;
  read_ahead_area= s >= READ_AHEAD_PAGES
    ? READ_AHEAD_PAGES
    : my_round_up_to_next_power(static_cast<uint32_t>(s));
  curr_pool_size= n_chunks * srv_buf_pool_chunk_unit;
  srv_buf_pool_curr_size= curr_pool_size;/* FIXME: remove*/
  extern ulonglong innobase_buffer_pool_size;
  innobase_buffer_pool_size= buf_pool_size_align(srv_buf_pool_curr_size);

	const bool	new_size_too_diff
		= srv_buf_pool_base_size > srv_buf_pool_size * 2
			|| srv_buf_pool_base_size * 2 < srv_buf_pool_size;

  mysql_mutex_unlock(&mutex);
  page_hash.write_unlock_all();

	UT_DELETE(chunk_map_old);

	resizing.store(false, std::memory_order_relaxed);

	/* Normalize other components, if the new size is too different */
	if (!warning && new_size_too_diff) {
		srv_buf_pool_base_size = srv_buf_pool_size;

		buf_resize_status("Resizing also other hash tables.");

		srv_lock_table_size = 5
			* (srv_buf_pool_size >> srv_page_size_shift);
		lock_sys.resize(srv_lock_table_size);
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

	ut_d(validate());

	return;
}

/** Thread pool task invoked by innodb_buffer_pool_size changes. */
static void buf_resize_callback(void *)
{
  DBUG_ENTER("buf_resize_callback");
  ut_ad(srv_shutdown_state < SRV_SHUTDOWN_CLEANUP);
  mysql_mutex_lock(&buf_pool.mutex);
  const auto size= srv_buf_pool_size;
  const bool work= srv_buf_pool_old_size != size;
  mysql_mutex_unlock(&buf_pool.mutex);

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
@param bpage   ROW_FORMAT=COMPRESSED only block
@param dpage   destination control block */
static void buf_relocate(buf_page_t *bpage, buf_page_t *dpage)
{
  const page_id_t id{bpage->id()};
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  ut_ad(!bpage->frame);
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(buf_pool.page_hash.lock_get(chain).is_write_locked());
  ut_ad(bpage == buf_pool.page_hash.get(id, chain));
  ut_ad(!buf_pool.watch_is_sentinel(*bpage));
  ut_d(const auto state= bpage->state());
  ut_ad(state >= buf_page_t::FREED);
  ut_ad(state <= buf_page_t::READ_FIX);
  ut_ad(bpage->lock.is_write_locked());
  const auto frame= dpage->frame;

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

buf_page_t *buf_pool_t::watch_set(const page_id_t id,
                                  buf_pool_t::hash_chain &chain) noexcept
{
  ut_ad(&chain == &page_hash.cell_get(id.fold()));
  page_hash.lock_get(chain).lock();

  buf_page_t *bpage= page_hash.get(id, chain);

  if (bpage)
  {
got_block:
    bpage->fix();
    if (watch_is_sentinel(*bpage))
    {
      ut_ad(!bpage->oldest_modification());
      bpage= nullptr;
    }
    page_hash.lock_get(chain).unlock();
    return bpage;
  }

  page_hash.lock_get(chain).unlock();
  /* Allocate a watch[] and then try to insert it into the page_hash. */
  mysql_mutex_lock(&mutex);

  /* The maximum number of purge tasks should never exceed
  the UT_ARR_SIZE(watch) - 1, and there is no way for a purge task to hold a
  watch when setting another watch. */
  for (buf_page_t *w= &watch[UT_ARR_SIZE(watch)]; w-- >= watch; )
  {
    ut_ad(w->access_time == 0);
    ut_ad(!w->oldest_modification());
    ut_ad(!w->zip.data);
    ut_ad(!w->in_zip_hash);
    static_assert(buf_page_t::NOT_USED == 0, "efficiency");
    if (ut_d(auto s=) w->state())
    {
      /* This watch may be in use for some other page. */
      ut_ad(s >= buf_page_t::UNFIXED);
      continue;
    }
    /* w is pointing to watch[], which is protected by mutex.
    Normally, buf_page_t::id for objects that are reachable by
    page_hash.get(id, chain) are protected by hash_lock. */
    w->set_state(buf_page_t::UNFIXED + 1);
    w->id_= id;

    page_hash.lock_get(chain).lock();
    bpage= page_hash.get(id, chain);
    if (UNIV_LIKELY_NULL(bpage))
    {
      w->set_state(buf_page_t::NOT_USED);
      mysql_mutex_unlock(&mutex);
      goto got_block;
    }

    ut_ad(w->state() == buf_page_t::UNFIXED + 1);
    buf_pool.page_hash.append(chain, w);
    mysql_mutex_unlock(&mutex);
    page_hash.lock_get(chain).unlock();
    return nullptr;
  }

  ut_error;
}

/** Stop watching whether a page has been read in.
watch_set(id) must have returned nullptr before.
@param id         page identifier
@param chain      unlocked hash table chain */
TRANSACTIONAL_TARGET
void buf_pool_t::watch_unset(const page_id_t id, buf_pool_t::hash_chain &chain)
  noexcept
{
  mysql_mutex_assert_not_owner(&mutex);
  buf_page_t *w;
  {
    transactional_lock_guard<page_hash_latch> g{page_hash.lock_get(chain)};
    /* The page must exist because watch_set() did fix(). */
    w= page_hash.get(id, chain);
    ut_ad(w->in_page_hash);
    if (!watch_is_sentinel(*w))
    {
    no_watch:
      w->unfix();
      w= nullptr;
    }
    else
    {
      ut_ad(!w->oldest_modification());
      const auto state= w->state();
      ut_ad(~buf_page_t::LRU_MASK & state);
      ut_ad(state >= buf_page_t::UNFIXED + 1);
      if (state != buf_page_t::UNFIXED + 1)
        goto no_watch;
    }
  }

  if (!w)
    return;

  const auto old= w;
  /* The following is based on buf_pool_t::watch_remove(). */
  mysql_mutex_lock(&mutex);
  w= page_hash.get(id, chain);

  {
    transactional_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    auto f= w->unfix();
    ut_ad(f < buf_page_t::READ_FIX || w != old);

    if (f == buf_page_t::UNFIXED && w == old)
    {
      page_hash.remove(chain, w);
      // Now that w is detached from page_hash, release it to watch[].
      ut_ad(w->id_ == id);
      ut_ad(!w->frame);
      ut_ad(!w->zip.data);
      w->set_state(buf_page_t::NOT_USED);
    }
  }

  mysql_mutex_unlock(&mutex);
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
  if (block->page.is_ibuf_exist())
    ibuf_merge_or_delete_for_page(nullptr, page_id, block->page.zip_size());
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
        const bool got_s_latch= bpage && !buf_pool.watch_is_sentinel(*bpage) &&
          bpage->lock.s_lock_try();
        xend();
        if (got_s_latch)
          break;
      }
    }
#endif

    hash_lock.lock_shared();
    bpage= buf_pool.page_hash.get(page_id, chain);
    if (!bpage || buf_pool.watch_is_sentinel(*bpage))
    {
      hash_lock.unlock_shared();
      switch (dberr_t err= buf_read_page(page_id, false)) {
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
/** Try to merge buffered changes to a buffer pool page.
@param block     buffer-fixed and latched block
@param rw_latch  RW_X_LATCH, RW_SX_LATCH, RW_S_LATCH held on block
@param err       error code
@return whether the page is invalid (corrupted) */
static bool buf_page_ibuf_merge_try(buf_block_t *block, ulint rw_latch,
                                    dberr_t *err)
{
  ut_ad(block->page.lock.have_any());
  ut_ad(block->page.buf_fix_count());

  if (fil_page_get_type(block->page.frame) != FIL_PAGE_INDEX ||
      !page_is_leaf(block->page.frame))
    return false;

  if (rw_latch != RW_X_LATCH)
  {
    if (rw_latch == RW_S_LATCH)
    {
      if (!block->page.lock.s_x_upgrade())
      {
        uint32_t state;
        state= block->page.state();
        if (state < buf_page_t::UNFIXED)
        {
        fail:
          block->page.lock.x_unlock();
          return true;
        }
        ut_ad(state & ~buf_page_t::LRU_MASK);
        ut_ad(state < buf_page_t::READ_FIX);
        if (state < buf_page_t::IBUF_EXIST || state >= buf_page_t::REINIT)
          /* ibuf_merge_or_delete_for_page() was already invoked in
          another thread. */
          goto downgrade_to_s;
      }
    }
    else
    {
      ut_ad(rw_latch == RW_SX_LATCH);
      block->page.lock.u_x_upgrade();
    }
  }

  ut_ad(block->page.lock.have_x());
  block->page.clear_ibuf_exist();
  if (dberr_t e= ibuf_merge_or_delete_for_page(block, block->page.id(),
                                               block->zip_size()))
  {
    if (err)
      *err= e;
    goto fail;
  }

  switch (rw_latch) {
  default:
    ut_ad(rw_latch == RW_X_LATCH);
    break;
  case RW_SX_LATCH:
    block->page.lock.x_u_downgrade();
    break;
  case RW_S_LATCH:
  downgrade_to_s:
    block->page.lock.x_u_downgrade();
    block->page.lock.u_s_downgrade();
    break;
  }

  return false;
}

ATTRIBUTE_COLD
buf_block_t *buf_pool_t::unzip(buf_page_t *b, buf_pool_t::hash_chain &chain)
  noexcept
{
  buf_block_t *block= buf_LRU_get_free_block(false);
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
  case buf_page_t::IBUF_EXIST + 1:
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

/** Apply a random read-ahead if it is enabled or there are at least a
threshold value of accessed pages from the random read-ahead area.
@param page_id   a page which the current thread wants to access
@param ibuf      whether we are inside ibuf routine */
TRANSACTIONAL_TARGET
static void buf_read_ahead_random(const page_id_t page_id, bool ibuf) noexcept
{
  if (!srv_random_read_ahead || page_id.space() >= SRV_TMP_SPACE_ID)
    /* Disable the read-ahead for temporary tablespace */
    return;

  if (srv_startup_is_before_trx_rollback_phase)
    /* No read-ahead to avoid thread deadlocks */
    return;

  if (trx_sys_hdr_page(page_id))
    return;

  if (os_aio_pending_reads_approx() > buf_pool.curr_size / 2)
    return;

  fil_space_t* space= fil_space_t::get(page_id.space());
  if (!space)
    return;

  if (!ibuf_bitmap_page(page_id, space->zip_size()))
  {
    const uint32_t buf_read_ahead_area= buf_pool.read_ahead_area;
    ulint count= 5 + buf_read_ahead_area / 8;
    const page_id_t low= page_id - (page_id.page_no() % buf_read_ahead_area);
    page_id_t high= low + buf_read_ahead_area;
    high.set_page_no(std::min(high.page_no(), space->last_page_number()));

    /* Count how many blocks in the area have been recently accessed,
    that is, reside near the start of the LRU list. */

    for (page_id_t i= low; i < high; ++i)
    {
      bool ok= false;
      {
        buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(i.fold());
        transactional_shared_lock_guard<page_hash_latch> g
          {buf_pool.page_hash.lock_get(chain)};
        if (const buf_page_t *bpage= buf_pool.page_hash.get(i, chain))
          if (bpage->is_accessed() && buf_page_peek_if_young(bpage))
            ok= true;
      }
      if (ok && !--count)
      {
        buf_read_ahead_random(space, ibuf, low, high);
        break;
      }
    }
  }

  space->release();
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
    if (b && !watch_is_sentinel(*b))
    {
      uint32_t state= b->fix() + 1;
      ut_ad(!b->in_zip_hash);
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
      unfix_corrupted:
        b->unfix();
      corrupted:
        if (err)
          *err= DB_CORRUPTION;
        return nullptr;
      }

      if ((state >= buf_page_t::READ_FIX && state < buf_page_t::WRITE_FIX) ||
          (state >= buf_page_t::IBUF_EXIST && state < buf_page_t::REINIT))
      {
        if (c == FIX_NOWAIT)
        {
        would_block:
          b->unfix();
          return reinterpret_cast<buf_block_t*>(-1);
        }

        if (UNIV_LIKELY(b->frame != nullptr));
        else if (state < buf_page_t::READ_FIX)
          goto unzip;
        else
        {
        wait_for_unzip:
          b->unfix();
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          continue;
        }
        b->lock.s_lock();
        state= b->state();
        ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);

        if (state >= buf_page_t::IBUF_EXIST && state < buf_page_t::REINIT &&
            buf_page_ibuf_merge_try(reinterpret_cast<buf_block_t*>(b),
                                    RW_S_LATCH, err))
          goto unfix_corrupted;

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

        if (state >= buf_page_t::IBUF_EXIST && state < buf_page_t::REINIT &&
            buf_page_ibuf_merge_try(block, RW_X_LATCH, err))
          goto unfix_corrupted;

        b->lock.x_unlock();
      }

      return reinterpret_cast<buf_block_t*>(b);
    }

    hash_lock.unlock_shared();

    if (c == FIX_NOWAIT)
      return reinterpret_cast<buf_block_t*>(-1);

    switch (dberr_t local_err= buf_read_page(id)) {
    default:
      if (err)
        *err= local_err;
      return nullptr;
    case DB_SUCCESS:
    case DB_SUCCESS_LOCKED_REC:
      mariadb_increment_pages_read(stats);
      buf_read_ahead_random(id, false);
    }
  }
}

/** Low level function used to get access to a database page.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		latch mode
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge to happen
@return pointer to the block
@retval nullptr	if the block is corrupted or unavailable */
TRANSACTIONAL_TARGET
buf_block_t*
buf_page_get_low(
	const page_id_t		page_id,
	ulint			zip_size,
	rw_lock_type_t		rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge) noexcept
{
	ulint		retries = 0;

	ut_ad(mtr->is_active());
	ut_ad(rw_latch != RW_NO_LATCH || !allow_ibuf_merge);

	if (err) {
		*err = DB_SUCCESS;
	}

#ifdef UNIV_DEBUG
	switch (mode) {
	default:
		ut_ad(!allow_ibuf_merge);
		ut_ad(mode == BUF_PEEK_IF_IN_POOL);
		break;
	case BUF_GET_POSSIBLY_FREED:
	case BUF_GET_IF_IN_POOL:
		/* The caller may pass a dummy page size,
		because it does not really matter. */
		break;
	case BUF_GET:
	case BUF_GET_IF_IN_POOL_OR_WATCH:
		ut_ad(!mtr->is_freeing_tree());
		fil_space_t* s = fil_space_get(page_id.space());
		ut_ad(s);
		ut_ad(s->zip_size() == zip_size);
	}
#endif /* UNIV_DEBUG */

	ut_ad(!ibuf_inside(mtr)
	      || ibuf_page_low(page_id, zip_size, FALSE, NULL));

	ha_handler_stats* const stats = mariadb_stats;
	buf_inc_get(stats);
	auto& chain= buf_pool.page_hash.cell_get(page_id.fold());
	page_hash_latch& hash_lock = buf_pool.page_hash.lock_get(chain);
loop:
	buf_block_t* block = guess;
	uint32_t state;

	if (block) {
		transactional_shared_lock_guard<page_hash_latch> g{hash_lock};
		if (buf_pool.is_uncompressed(block)
		    && page_id == block->page.id()) {
			ut_ad(!block->page.in_zip_hash);
			state = block->page.state();
			/* Ignore guesses that point to read-fixed blocks.
			We can only avoid a race condition by
			looking up the block via buf_pool.page_hash. */
			if ((state >= buf_page_t::FREED
			     && state < buf_page_t::READ_FIX)
			    || state >= buf_page_t::WRITE_FIX) {
				state = block->page.fix();
				goto got_block;
			}
		}
	}

	guess = nullptr;

	/* A memory transaction would frequently be aborted here. */
	hash_lock.lock_shared();
	block = reinterpret_cast<buf_block_t*>(
		buf_pool.page_hash.get(page_id, chain));
	if (UNIV_LIKELY(block
			&& !buf_pool.watch_is_sentinel(block->page))) {
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
	case BUF_GET_IF_IN_POOL_OR_WATCH:
		/* Buffer-fixing inside watch_set() will prevent eviction */
		block = reinterpret_cast<buf_block_t*>
			(buf_pool.watch_set(page_id, chain));

		if (block) {
			state = block->page.state();
			goto got_block_fixed;
		}

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

	switch (dberr_t local_err = buf_read_page(page_id)) {
	case DB_SUCCESS:
	case DB_SUCCESS_LOCKED_REC:
		mariadb_increment_pages_read(stats);
		buf_read_ahead_random(page_id, ibuf_inside(mtr));
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
	ut_ad(!block->page.in_zip_hash);
	state++;
got_block_fixed:
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
		block->page.lock.s_lock();
		state = block->page.state();
		ut_ad(state < buf_page_t::READ_FIX
		      || state >= buf_page_t::WRITE_FIX);
		const page_id_t id{block->page.id()};
		block->page.lock.s_unlock();

		if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
			if (UNIV_UNLIKELY(id == page_id)) {
				/* The page read was completed, and
				another thread marked the page as free
				while we were waiting. */
				goto ignore_block;
			}

			ut_ad(id == page_id_t{~0ULL});
			block->page.unfix();

			if (++retries < BUF_PAGE_READ_MAX_RETRIES) {
				goto loop;
			}

			if (err) {
				*err = DB_PAGE_CORRUPTED;
			}

			return nullptr;
		}
		ut_ad(id == page_id);
	} else if (mode != BUF_PEEK_IF_IN_POOL) {
	} else if (UNIV_UNLIKELY(!block->page.frame)) {
		/* The BUF_PEEK_IF_IN_POOL mode is mainly used for dropping an
		adaptive hash index. There cannot be an
		adaptive hash index for a compressed-only page. */
		goto ignore_block;
	}

	ut_ad(mode == BUF_GET_IF_IN_POOL || mode == BUF_PEEK_IF_IN_POOL
	      || block->zip_size() == zip_size);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
re_evict:
	if (mode != BUF_GET_IF_IN_POOL
	    && mode != BUF_GET_IF_IN_POOL_OR_WATCH) {
	} else if (!ibuf_debug || recv_recovery_is_on()) {
	} else if (fil_space_t* space = fil_space_t::get(page_id.space())) {
		for (ulint i = 0; i < mtr->get_savepoint(); i++) {
			if (buf_block_t* b = mtr->block_at_savepoint(i)) {
				if (b->page.oldest_modification() > 2
				    && b->page.lock.have_any()) {
					/* We are holding a dirty page latch
					that would hang buf_flush_sync(). */
					space->release();
					goto re_evict_fail;
				}
			}
		}

		/* Try to evict the block from the buffer pool, to use the
		insert buffer (change buffer) as much as possible. */

		mysql_mutex_lock(&buf_pool.mutex);

		block->unfix();

		/* Blocks cannot be relocated or enter or exit the
		buf_pool while we are holding the buf_pool.mutex. */
		const bool evicted = buf_LRU_free_page(&block->page, true);
		space->release();

		if (!evicted) {
			block->fix();
		}

		mysql_mutex_unlock(&buf_pool.mutex);

		if (evicted) {
			if (mode == BUF_GET_IF_IN_POOL_OR_WATCH) {
				buf_pool.watch_set(page_id, chain);
			}
			return(NULL);
		}

		buf_flush_sync();

		state = block->page.state();

		if (state == buf_page_t::UNFIXED + 1
		    && !block->page.oldest_modification()) {
			goto re_evict;
		}

		/* Failed to evict the page; change it directly */
	}
re_evict_fail:
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

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
		ut_ad(!allow_ibuf_merge);
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

	mtr->memo_push(block, mtr_memo_type_t(rw_latch));
	state = block->page.state();

	if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
	corrupted:
		mtr->release_last_page();
		goto ignore_unfixed;
	}

	ut_ad(state < buf_page_t::READ_FIX
	      || state > buf_page_t::WRITE_FIX);
	if (state >= buf_page_t::IBUF_EXIST && state < buf_page_t::REINIT
	    && allow_ibuf_merge
	    && buf_page_ibuf_merge_try(block, rw_latch, err)) {
		ut_ad(block == mtr->at_savepoint(mtr->get_savepoint() - 1));
		mtr->lock_register(mtr->get_savepoint() - 1, MTR_MEMO_BUF_FIX);
		goto corrupted;
	}
#ifdef BTR_CUR_HASH_ADAPT
	btr_search_drop_page_hash_index(block, true);
#endif /* BTR_CUR_HASH_ADAPT */

	ut_ad(page_id_t(page_get_space_id(block->page.frame),
			page_get_page_no(block->page.frame)) == page_id);
	return block;
}

/** Get access to a database page. Buffered redo log may be applied.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		latch mode
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in,out]	mtr			mini-transaction, or NULL
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge to happen
@return pointer to the block
@retval nullptr	if the block is corrupted or unavailable */
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	ulint			zip_size,
	rw_lock_type_t		rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge) noexcept
{
  buf_block_t *block= recv_sys.recover(page_id);
  if (UNIV_LIKELY(!block))
    return buf_page_get_low(page_id, zip_size, rw_latch,
                            guess, mode, mtr, err, allow_ibuf_merge);
  else if (UNIV_UNLIKELY(block == reinterpret_cast<buf_block_t*>(-1)))
  {
  corrupted:
    if (err)
      *err= DB_CORRUPTION;
    return nullptr;
  }
  auto s= block->page.fix();
  ut_ad(s >= buf_page_t::FREED);
  /* The block may be write-fixed at this point because we are not
  holding a lock, but it must not be read-fixed. */
  ut_ad(s < buf_page_t::READ_FIX || s >= buf_page_t::WRITE_FIX);
  if (err)
    *err= DB_SUCCESS;
  const bool must_merge= allow_ibuf_merge &&
    ibuf_page_exists(page_id, block->zip_size());
  if (s < buf_page_t::UNFIXED)
  {
  got_freed_page:
    ut_ad(mode == BUF_GET_POSSIBLY_FREED || mode == BUF_PEEK_IF_IN_POOL);
    mysql_mutex_lock(&buf_pool.mutex);
    block->page.unfix();
    buf_LRU_free_page(&block->page, true);
    mysql_mutex_unlock(&buf_pool.mutex);
    goto corrupted;
  }
  else if (must_merge &&
           fil_page_get_type(block->page.frame) == FIL_PAGE_INDEX &&
           page_is_leaf(block->page.frame))
  {
    block->page.lock.x_lock();
    s= block->page.state();
    ut_ad(s > buf_page_t::FREED);
    ut_ad(s < buf_page_t::READ_FIX);
    if (s < buf_page_t::UNFIXED)
    {
      block->page.lock.x_unlock();
      goto got_freed_page;
    }
    else
    {
      if (block->page.is_ibuf_exist())
        block->page.clear_ibuf_exist();
      if (dberr_t e=
          ibuf_merge_or_delete_for_page(block, page_id, block->zip_size()))
      {
        if (err)
          *err= e;
        buf_pool.corrupted_evict(&block->page, s);
        return nullptr;
      }
    }

    switch (rw_latch) {
    case RW_NO_LATCH:
      block->page.lock.x_unlock();
    case RW_X_LATCH:
      break;
    case RW_SX_LATCH:
      block->page.lock.x_u_downgrade();
      break;
    case RW_S_LATCH:
      block->page.lock.x_u_downgrade();
      block->page.lock.u_s_downgrade();
    }

    mtr->memo_push(block, mtr_memo_type_t(rw_latch));
    return block;
  }
  mtr->page_lock(block, rw_latch);
  return block;
}

TRANSACTIONAL_TARGET
buf_block_t *buf_page_optimistic_fix(buf_block_t *block, page_id_t id) noexcept
{
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  transactional_shared_lock_guard<page_hash_latch> g
    {buf_pool.page_hash.lock_get(chain)};
  if (UNIV_UNLIKELY(!buf_pool.is_uncompressed(block) ||
                    id != block->page.id() || !block->page.frame))
    return nullptr;
  const auto state= block->page.state();
  if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED ||
                    state >= buf_page_t::READ_FIX))
    return nullptr;
  block->page.fix();
  return block;
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

    ut_ad(!ibuf_inside(mtr) ||
          ibuf_page(block->page.id(), block->zip_size(), nullptr));

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
    ut_ad(!ibuf_inside(mtr) ||
          ibuf_page(block->page.id(), block->zip_size(), nullptr));

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

  if (bpage && !buf_pool.watch_is_sentinel(*bpage))
  {
#ifdef BTR_CUR_HASH_ADAPT
    const dict_index_t *drop_hash_entry= nullptr;
#endif
    bool ibuf_exist= false;

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
      {
        bpage->set_reinit(state & buf_page_t::LRU_MASK);
        ibuf_exist= (state & buf_page_t::LRU_MASK) == buf_page_t::IBUF_EXIST;
      }

      if (UNIV_LIKELY(bpage->frame != nullptr))
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        buf_block_t *block= reinterpret_cast<buf_block_t*>(bpage);
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
      ut_ad(bpage->frame);
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

    if (ibuf_exist && !recv_recovery_is_on())
      ibuf_merge_or_delete_for_page(nullptr, page_id, zip_size);

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

  /* Delete possible entries for the page from the insert buffer:
  such can exist if the page belonged to an index which was dropped */
  if (page_id < page_id_t{SRV_SPACE_ID_UPPER_BOUND, 0} &&
      !srv_is_undo_tablespace(page_id.space()) &&
      !recv_recovery_is_on())
    ibuf_merge_or_delete_for_page(nullptr, page_id, zip_size);

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
					read, MONITOR_INDEX_IBUF_LEAF_PAGE);
			} else {
				counter = MONITOR_RW_COUNTER(
					read,
					MONITOR_INDEX_IBUF_NON_LEAF_PAGE);
			}
		} else {
			if (level == 0) {
				counter = MONITOR_RW_COUNTER(
					read, MONITOR_INDEX_LEAF_PAGE);
			} else {
				counter = MONITOR_RW_COUNTER(
					read, MONITOR_INDEX_NON_LEAF_PAGE);
			}
		}
		break;

	case FIL_PAGE_UNDO_LOG:
		counter = MONITOR_RW_COUNTER(read, MONITOR_UNDO_LOG_PAGE);
		break;

	case FIL_PAGE_INODE:
		counter = MONITOR_RW_COUNTER(read, MONITOR_INODE_PAGE);
		break;

	case FIL_PAGE_IBUF_FREE_LIST:
		counter = MONITOR_RW_COUNTER(read, MONITOR_IBUF_FREELIST_PAGE);
		break;

	case FIL_PAGE_IBUF_BITMAP:
		counter = MONITOR_RW_COUNTER(read, MONITOR_IBUF_BITMAP_PAGE);
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
  }

  if (err == DB_PAGE_CORRUPTED || err == DB_DECRYPTION_FAILED)
  {
release_page:
    if (node.space->full_crc32() && recv_recovery_is_on() &&
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
success_page:

  const bool recovery= recv_recovery_is_on();

  if (recovery && !recv_recover_page(node.space, this))
    return DB_PAGE_CORRUPTED;

  const bool ibuf_may_exist= !recv_no_ibuf_operations &&
    (!expected_id.space() || !is_predefined_tablespace(expected_id.space())) &&
    fil_page_get_type(read_frame) == FIL_PAGE_INDEX &&
    page_is_leaf(read_frame);

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(*this, true);
  DBUG_PRINT("ib_buf", ("read page %u:%u", id().space(), id().page_no()));

  if (!recovery)
  {
    ut_d(auto f=) zip.fix.fetch_sub(ibuf_may_exist
                                    ? READ_FIX - IBUF_EXIST
                                    : READ_FIX - UNFIXED);
    ut_ad(f >= READ_FIX);
    ut_ad(f < WRITE_FIX);
  }
  else if (ibuf_may_exist)
    set_ibuf_exist();

  lock.x_unlock(true);

  return DB_SUCCESS;
}

#ifdef UNIV_DEBUG
/** Check that all blocks are in a replaceable state.
@return address of a non-free block
@retval nullptr if all freed */
void buf_pool_t::assert_all_freed() noexcept
{
  mysql_mutex_lock(&mutex);
  const chunk_t *chunk= chunks;
  for (auto i= n_chunks; i--; chunk++)
    if (const buf_block_t* block= chunk->not_freed())
      ib::fatal() << "Page " << block->page.id() << " still fixed or dirty";
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

	chunk_t* chunk = chunks;

	/* Check the uncompressed blocks. */

	for (auto i = n_chunks; i--; chunk++) {
		buf_block_t*	block = chunk->blocks;

		for (auto j = chunk->size; j--; block++) {
			ut_ad(block->page.frame);
			switch (const auto f = block->page.state()) {
			case buf_page_t::NOT_USED:
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

	if (n_chunks_new == n_chunks
	    && n_lru + n_free > curr_size + n_zip) {

		ib::fatal() << "n_LRU " << n_lru << ", n_free " << n_free
			<< ", pool " << curr_size
			<< " zip " << n_zip << ". Aborting...";
	}

	ut_ad(UT_LIST_GET_LEN(LRU) >= n_lru);

	if (n_chunks_new == n_chunks
	    && UT_LIST_GET_LEN(free) != n_free) {

		ib::fatal() << "Free list len "
			<< UT_LIST_GET_LEN(free)
			<< ", free blocks " << n_free << ". Aborting...";
	}

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

	mysql_mutex_lock(&mutex);
	mysql_mutex_lock(&flush_list_mutex);

	ib::info()
		<< "[buffer pool: size=" << curr_size
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

	chunk = chunks;

	for (i = n_chunks; i--; chunk++) {
		buf_block_t*	block		= chunk->blocks;
		ulint		n_blocks	= chunk->size;

		for (; n_blocks--; block++) {
			const buf_frame_t* frame = block->page.frame;

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

  mysql_mutex_lock(&buf_pool.mutex);

  for (buf_page_t *b= UT_LIST_GET_FIRST(buf_pool.LRU); b;
       b= UT_LIST_GET_NEXT(LRU, b))
    if (b->state() > buf_page_t::UNFIXED)
      fixed_pages_number++;

  mysql_mutex_unlock(&buf_pool.mutex);

  return fixed_pages_number;
}
#endif /* UNIV_DEBUG */

/** Collect buffer pool metadata.
@param[out]	pool_info	buffer pool metadata */
void buf_stats_get_pool_info(buf_pool_info_t *pool_info) noexcept
{
	time_t			current_time;
	double			time_elapsed;

	mysql_mutex_lock(&buf_pool.mutex);

	pool_info->pool_size = buf_pool.curr_size;

	pool_info->lru_len = UT_LIST_GET_LEN(buf_pool.LRU);

	pool_info->old_lru_len = buf_pool.LRU_old_len;

	pool_info->free_list_len = UT_LIST_GET_LEN(buf_pool.free);

	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	pool_info->flush_list_len = UT_LIST_GET_LEN(buf_pool.flush_list);

	pool_info->n_pend_unzip = UT_LIST_GET_LEN(buf_pool.unzip_LRU);

	pool_info->n_pend_reads = os_aio_pending_reads_approx();

	pool_info->n_pending_flush_lru = buf_pool.n_flush();

	pool_info->n_pending_flush_list = os_aio_pending_writes();
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

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
	mysql_mutex_unlock(&buf_pool.mutex);
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

	buf_stats_get_pool_info(&pool_info);
	buf_print_io_instance(&pool_info, file);
}

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param[in]	page		page frame
@param[in]	fsp_flags	tablespace flags
@return true if true if page is encrypted and OK, false otherwise */
bool buf_page_verify_crypt_checksum(const byte* page, ulint fsp_flags) noexcept
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
