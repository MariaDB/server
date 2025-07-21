/*****************************************************************************

Copyright (c) 1995, 2021, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2023, MariaDB Corporation.

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
@file fil/fil0fil.cc
The tablespace memory cache

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#include "fil0fil.h"
#include "fil0crypt.h"

#include "btr0btr.h"
#include "buf0buf.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "dict0load.h"
#include "fsp0file.h"
#include "fsp0fsp.h"
#include "hash0hash.h"
#include "log0log.h"
#include "log0recv.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "os0file.h"
#include "page0zip.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "buf0lru.h"
#include "buf0flu.h"
#include "log.h"
#ifdef __linux__
# include <sys/types.h>
# include <sys/sysmacros.h>
# include <dirent.h>
#endif

#include "lz4.h"
#include "lzo/lzo1x.h"
#include "lzma.h"
#include "bzlib.h"
#include "snappy-c.h"

ATTRIBUTE_COLD bool fil_space_t::set_corrupted() const noexcept
{
  if (!is_stopping() && !is_corrupted.test_and_set())
  {
    sql_print_error("InnoDB: File '%s' is corrupted", chain.start->name);
    return true;
  }
  return false;
}

/** Try to close a file to adhere to the innodb_open_files limit.
@param ignore_space Ignore the tablespace which is acquired by caller
@param print_info   whether to diagnose why a file cannot be closed
@return whether a file was closed */
bool fil_space_t::try_to_close(fil_space_t *ignore_space, bool print_info)
  noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  for (fil_space_t &space : fil_system.space_list)
  {
    if (&space == ignore_space || space.is_being_imported() ||
        space.id == TRX_SYS_SPACE || space.id == SRV_TMP_SPACE_ID ||
        srv_is_undo_tablespace(space.id))
      continue;
    ut_ad(!space.is_temporary());

    /* We are using an approximation of LRU replacement policy. In
    fil_node_open_file_low(), newly opened files are moved to the end
    of fil_system.space_list, so that they would be less likely to be
    closed here. */
    fil_node_t *node= UT_LIST_GET_FIRST(space.chain);
    if (!node)
      /* fil_ibd_create() did not invoke fil_space_t::add() yet */
      continue;
    ut_ad(!UT_LIST_GET_NEXT(chain, node));

    if (!node->is_open())
      continue;

    const auto n= space.set_closing();
    if (n & STOPPING)
      /* Let fil_space_t::drop() in another thread handle this. */
      continue;
    if (n & (PENDING | NEEDS_FSYNC))
    {
      if (!print_info)
        continue;
      print_info= false;
      const time_t now= time(nullptr);
      if (now - fil_system.n_open_exceeded_time < 5)
        continue; /* We display messages at most once in 5 seconds. */
      fil_system.n_open_exceeded_time= now;

      if (n & PENDING)
        sql_print_information("InnoDB: Cannot close file %s because of "
                              UINT32PF " pending operations%s", node->name,
                              n & PENDING,
                              (n & NEEDS_FSYNC) ? " and pending fsync" : "");
      else if (n & NEEDS_FSYNC)
        sql_print_information("InnoDB: Cannot close file %s because of "
                              "pending fsync", node->name);
      continue;
    }

    node->close();

    fil_system.move_closed_last_to_space_list(node->space);

    return true;
  }

  return false;
}

/*
		IMPLEMENTATION OF THE TABLESPACE MEMORY CACHE
		=============================================

The tablespace cache is responsible for providing fast read/write access to
tablespaces and logs of the database. File creation and deletion is done
in other modules which know more of the logic of the operation, however.

A tablespace consists of a chain of files. The size of the files does not
have to be divisible by the database block size, because we may just leave
the last incomplete block unused. When a new file is appended to the
tablespace, the maximum size of the file is also specified. At the moment,
we think that it is best to extend the file to its maximum size already at
the creation of the file, because then we can avoid dynamically extending
the file when more space is needed for the tablespace.

A block's position in the tablespace is specified with a 32-bit unsigned
integer. The files in the chain are thought to be catenated, and the block
corresponding to an address n is the nth block in the catenated file (where
the first block is named the 0th block, and the incomplete block fragments
at the end of files are not taken into account). A tablespace can be extended
by appending a new file at the end of the chain.

Our tablespace concept is similar to the one of Oracle.

To acquire more speed in disk transfers, a technique called disk striping is
sometimes used. This means that logical block addresses are divided in a
round-robin fashion across several disks. Windows NT supports disk striping,
so there we do not need to support it in the database. Disk striping is
implemented in hardware in RAID disks. We conclude that it is not necessary
to implement it in the database. Oracle 7 does not support disk striping,
either.

Another trick used at some database sites is replacing tablespace files by
raw disks, that is, the whole physical disk drive, or a partition of it, is
opened as a single file, and it is accessed through byte offsets calculated
from the start of the disk or the partition. This is recommended in some
books on database tuning to achieve more speed in i/o. Using raw disk
certainly prevents the OS from fragmenting disk space, but it is not clear
if it really adds speed. We measured on the Pentium 100 MHz + NT + NTFS file
system + EIDE Conner disk only a negligible difference in speed when reading
from a file, versus reading from a raw disk.

To have fast access to a tablespace or a log file, we put the data structures
to a hash table. Each tablespace and log file is given an unique 32-bit
identifier. */

/** Reference to the server data directory. Usually it is the
current working directory ".", but in the MariaDB Embedded Server Library
it is an absolute path. */
const char*	fil_path_to_mysql_datadir;

/** Common InnoDB file extensions */
const char* dot_ext[] = { "", ".ibd", ".isl", ".cfg" };

/** Number of pending tablespace flushes */
Atomic_counter<ulint> fil_n_pending_tablespace_flushes;

/** The tablespace memory cache. This variable is NULL before the module is
initialized. */
fil_system_t	fil_system;

/** At this age or older a space/page will be rotated */
extern uint srv_fil_crypt_rotate_key_age;

#ifdef UNIV_DEBUG
/** Try fil_validate() every this many times */
# define FIL_VALIDATE_SKIP	17

/******************************************************************//**
Checks the consistency of the tablespace cache some of the time.
@return true if ok or the check was skipped */
static bool fil_validate_skip() noexcept
{
	/** The fil_validate() call skip counter. */
	static Atomic_counter<uint32_t> fil_validate_count;

	/* We want to reduce the call frequency of the costly fil_validate()
	check in debug builds. */
	return (fil_validate_count++ % FIL_VALIDATE_SKIP) || fil_validate();
}
#endif /* UNIV_DEBUG */

fil_space_t *fil_space_get_by_id(uint32_t id) noexcept
{
  ut_ad(fil_system.is_initialised());
  mysql_mutex_assert_owner(&fil_system.mutex);
  return fil_system.spaces.cell_get(id)->find
    (&fil_space_t::hash, [id](const fil_space_t *s) { return s->id == id; });
}

/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
Normally, fil_space_t::get() should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or NULL if not found */
fil_space_t *fil_space_get(uint32_t id) noexcept
{
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}

/** Check if the compression algorithm is loaded
@param[in]	comp_algo ulint compression algorithm
@return whether the compression algorithm is loaded */
bool fil_comp_algo_loaded(ulint comp_algo) noexcept
{
	switch (comp_algo) {
	case PAGE_UNCOMPRESSED:
	case PAGE_ZLIB_ALGORITHM:
		return true;

	case PAGE_LZ4_ALGORITHM:
		return provider_service_lz4->is_loaded;

	case PAGE_LZO_ALGORITHM:
		return provider_service_lzo->is_loaded;

	case PAGE_LZMA_ALGORITHM:
		return provider_service_lzma->is_loaded;

	case PAGE_BZIP2_ALGORITHM:
		return provider_service_bzip2->is_loaded;

	case PAGE_SNAPPY_ALGORITHM:
		return provider_service_snappy->is_loaded;
	}

	return false;
}

/** Append a file to the chain of files of a space.
@param[in]	name		file name of a file that is not open
@param[in]	handle		file handle, or OS_FILE_CLOSED
@param[in]	size		file size in entire database pages
@param[in]	is_raw		whether this is a raw device
@param[in]	atomic_write	true if atomic write could be enabled
@param[in]	max_pages	maximum number of pages in file,
or UINT32_MAX for unlimited
@return file object */
fil_node_t* fil_space_t::add(const char* name, pfs_os_file_t handle,
			     uint32_t size, bool is_raw, bool atomic_write,
			     uint32_t max_pages) noexcept
{
	mysql_mutex_assert_owner(&fil_system.mutex);

	fil_node_t*	node;

	ut_ad(name != NULL);
	ut_ad(fil_system.is_initialised());

	node = reinterpret_cast<fil_node_t*>(ut_zalloc_nokey(sizeof(*node)));

	node->handle = handle;

	node->name = mem_strdup(name);

	ut_a(!is_raw || srv_start_raw_disk_in_use);

	node->is_raw_disk = is_raw;

	node->size = size;

	node->init_size = size;
	node->max_size = max_pages;

	node->space = this;

	node->atomic_write = atomic_write;

	this->size += size;
	UT_LIST_ADD_LAST(chain, node);
	if (node->is_open()) {
		clear_closing();
		if (++fil_system.n_open >= srv_max_n_open_files) {
			reacquire();
			try_to_close(this, true);
			release();
		}
	}

	return node;
}

__attribute__((warn_unused_result, nonnull(1)))
/** Open a tablespace file.
@param node  data file
@param page  first page of the tablespace, or nullptr
@param no_lsn  whether to skip the FIL_PAGE_LSN check
@return whether the file was successfully opened */
static bool fil_node_open_file_low(fil_node_t *node, const byte *page,
                                   bool no_lsn)
{
  ut_ad(!node->is_open());
  ut_ad(node->space->is_closing());
  mysql_mutex_assert_owner(&fil_system.mutex);
  static_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096, "compatibility");
#if defined _WIN32 || defined O_DIRECT
  ulint type;
  switch (FSP_FLAGS_GET_ZIP_SSIZE(node->space->flags)) {
  case 1:
  case 2:
    type= OS_DATA_FILE_NO_O_DIRECT;
    break;
  default:
    type= OS_DATA_FILE;
  }
#else
  constexpr auto type= OS_DATA_FILE;
#endif

  for (;;)
  {
    bool success;
    node->handle= os_file_create(innodb_data_file_key, node->name,
                                 node->is_raw_disk
                                 ? OS_FILE_OPEN_RAW : OS_FILE_OPEN,
                                 type,
                                 srv_read_only_mode, &success);

    if (success && node->is_open())
    {
#ifndef _WIN32
      if (!node->space->id && !srv_read_only_mode && my_disable_locking &&
          os_file_lock(node->handle, node->name))
      {
        os_file_close(node->handle);
        node->handle= OS_FILE_CLOSED;
        return false;
      }
#endif
      break;
    }

    /* The following call prints an error message */
    if (os_file_get_last_error(true) == EMFILE + 100 &&
        fil_space_t::try_to_close(nullptr, true))
      continue;

    ib::warn() << "Cannot open '" << node->name << "'.";
    return false;
  }

  ulint comp_algo = node->space->get_compression_algo();
  bool comp_algo_invalid = false;

  if (node->size);
  else if (!node->read_page0(page, no_lsn) ||
            // validate compression algorithm for full crc32 format
            (node->space->full_crc32() &&
             (comp_algo_invalid = !fil_comp_algo_loaded(comp_algo))))
  {
    if (comp_algo_invalid)
    {
      if (comp_algo <= PAGE_ALGORITHM_LAST)
        ib::warn() << "'" << node->name << "' is compressed with "
                   << page_compression_algorithms[comp_algo]
                   << ", which is not currently loaded";
      else
        ib::warn() << "'" << node->name << "' is compressed with "
                   << "invalid algorithm: " << comp_algo;
    }

    os_file_close(node->handle);
    node->handle= OS_FILE_CLOSED;
    return false;
  }

  ut_ad(node->is_open());

  fil_system.move_opened_last_to_space_list(node->space);

  fil_system.n_open++;
  return true;
}

/** Open a tablespace file.
@param node  data file
@param page  first page of the tablespace, or nullptr
@param no_lsn  whether to skip the FIL_PAGE_LSN check
@return whether the file was successfully opened */
static bool fil_node_open_file(fil_node_t *node, const byte *page, bool no_lsn)
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(!node->is_open());
  ut_ad(!is_predefined_tablespace(node->space->id) ||
        srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_DELTA);
  ut_ad(!node->space->is_temporary());
  ut_ad(node->space->referenced());

  const auto old_time= fil_system.n_open_exceeded_time;

  for (ulint count= 0; fil_system.n_open >= srv_max_n_open_files; count++)
  {
    if (fil_space_t::try_to_close(nullptr, count > 1))
      count= 0;
    else if (count >= 2)
    {
      if (old_time != fil_system.n_open_exceeded_time)
        sql_print_warning("InnoDB: innodb_open_files=" ULINTPF
                          " is exceeded (" ULINTPF " files stay open)",
                          srv_max_n_open_files, fil_system.n_open);
      break;
    }
    else
    {
      mysql_mutex_unlock(&fil_system.mutex);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      /* Flush tablespaces so that we can close modified files. */
      fil_flush_file_spaces();
      mysql_mutex_lock(&fil_system.mutex);
      if (node->is_open())
        return true;
    }
  }

  /* The node can be opened beween releasing and acquiring fil_system.mutex
  in the above code */
  return node->is_open() || fil_node_open_file_low(node, page, no_lsn);
}

/** Close the file handle. */
void fil_node_t::close() noexcept
{
  prepare_to_close_or_detach();

  /* printf("Closing file %s\n", name); */
  int ret= os_file_close(handle);
  ut_a(ret);
  handle= OS_FILE_CLOSED;
}

pfs_os_file_t fil_node_t::detach() noexcept
{
  prepare_to_close_or_detach();

  pfs_os_file_t result= handle;
  handle= OS_FILE_CLOSED;
  return result;
}

void fil_node_t::prepare_to_close_or_detach() noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(space->is_ready_to_close() || srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_RESTORE_DELTA);
  ut_a(is_open());
  ut_a(!being_extended);
  ut_a(space->is_ready_to_close() || space->is_temporary() ||
       srv_fast_shutdown == 2 || !srv_was_started);

  ut_a(fil_system.n_open > 0);
  fil_system.n_open--;
}

/** Flush any writes cached by the file system. */
void fil_space_t::flush_low() noexcept
{
  mysql_mutex_assert_not_owner(&fil_system.mutex);

  uint32_t n= 1;
  while (!n_pending.compare_exchange_strong(n, n | NEEDS_FSYNC,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed))
  {
    ut_ad(n & PENDING);
    if (n & STOPPING_WRITES)
      return;
    if (n & NEEDS_FSYNC)
      break;
  }

  if (fil_system.is_write_through())
    goto skip_flush;

  fil_n_pending_tablespace_flushes++;
  for (fil_node_t *node= UT_LIST_GET_FIRST(chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
  {
    if (!node->is_open())
    {
      ut_ad(!is_in_unflushed_spaces);
      continue;
    }
    IF_WIN(if (node->is_raw_disk) continue,);
    os_file_flush(node->handle);
  }

  if (is_in_unflushed_spaces)
  {
    mysql_mutex_lock(&fil_system.mutex);
    if (is_in_unflushed_spaces)
    {
      is_in_unflushed_spaces= false;
      fil_system.unflushed_spaces.remove(*this);
    }
    mysql_mutex_unlock(&fil_system.mutex);
  }

  fil_n_pending_tablespace_flushes--;
skip_flush:
  clear_flush();
}

/** Try to extend a tablespace.
@param[in,out]	space	tablespace to be extended
@param[in,out]	node	last file of the tablespace
@param[in]	size	desired size in number of pages
@param[out]	success	whether the operation succeeded
@return	whether the operation should be retried */
static ATTRIBUTE_COLD __attribute__((warn_unused_result, nonnull))
bool
fil_space_extend_must_retry(
	fil_space_t*	space,
	fil_node_t*	node,
	uint32_t	size,
	bool*		success) noexcept
{
	mysql_mutex_assert_owner(&fil_system.mutex);
	ut_ad(UT_LIST_GET_LAST(space->chain) == node);
	ut_ad(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_ad(node->space == space);
	ut_ad(space->referenced());

	*success = space->size >= size;

	if (*success) {
		/* Space already big enough */
		return(false);
	}

	if (node->being_extended) {
		/* Another thread is currently extending the file. Wait
		for it to finish.
		It'd have been better to use event driven mechanism but
		the entire module is peppered with polling stuff. */
		mysql_mutex_unlock(&fil_system.mutex);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		return(true);
	}

	node->being_extended = true;

	/* At this point it is safe to release fil_system.mutex. No
	other thread can rename, delete, close or extend the file because
	we have set the node->being_extended flag. */
	mysql_mutex_unlock(&fil_system.mutex);

	ut_ad(size >= space->size);

	uint32_t	last_page_no		= space->size;
	const uint32_t	file_start_page_no	= last_page_no - node->size;

	const unsigned	page_size = space->physical_size();

	/* Datafile::read_first_page() expects innodb_page_size bytes.
	fil_node_t::read_page0() expects at least 4 * innodb_page_size bytes.
	os_file_set_size() expects multiples of 4096 bytes.
	For ROW_FORMAT=COMPRESSED tables using 1024-byte or 2048-byte
	pages, we will preallocate up to an integer multiple of 4096 bytes,
	and let normal writes append 1024, 2048, or 3072 bytes to the file. */
	os_offset_t new_size = std::max(
		(os_offset_t(size - file_start_page_no) * page_size)
		& ~os_offset_t(4095),
		os_offset_t(FIL_IBD_FILE_INITIAL_SIZE << srv_page_size_shift));

	*success = os_file_set_size(node->name, node->handle, new_size,
				    node->punch_hole == 1);

	os_has_said_disk_full = !*success;
	if (*success) {
		os_file_flush(node->handle);
		last_page_no = size;
	} else {
		/* Let us measure the size of the file
		to determine how much we were able to
		extend it */
		os_offset_t	fsize = os_file_get_size(node->handle);
		ut_a(fsize != os_offset_t(-1));

		last_page_no = uint32_t(fsize / page_size)
			+ file_start_page_no;
	}
	mysql_mutex_lock(&fil_system.mutex);

	ut_a(node->being_extended);
	node->being_extended = false;
	ut_a(last_page_no - file_start_page_no >= node->size);

	uint32_t file_size = last_page_no - file_start_page_no;
	space->size += file_size - node->size;
	node->size = file_size;
	const uint32_t pages_in_MiB = node->size
		& ~uint32_t((1U << (20U - srv_page_size_shift)) - 1);

	/* Keep the last data file size info up to date, rounded to
	full megabytes */

	switch (space->id) {
	case TRX_SYS_SPACE:
		srv_sys_space.set_last_file_size(pages_in_MiB);
	do_flush:
		space->reacquire();
		mysql_mutex_unlock(&fil_system.mutex);
		space->flush_low();
		space->release();
		mysql_mutex_lock(&fil_system.mutex);
		break;
	default:
		ut_ad(!space->is_temporary());
		if (!space->is_being_imported()) {
			goto do_flush;
		}
		break;
	case SRV_TMP_SPACE_ID:
		ut_ad(space->is_temporary());
		srv_tmp_space.set_last_file_size(pages_in_MiB);
		break;
	}

	return false;
}

bool recv_sys_t::check_sys_truncate()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  if (!truncated_sys_space.lsn)
    return false;
  if (fil_system.sys_space->size <= fil_system.sys_space->recv_size)
  {
    truncated_sys_space={0,0};
    return false;
  }
  return true;
}

/** @return whether the file is usable for io() */
ATTRIBUTE_COLD bool fil_space_t::prepare_acquired() noexcept
{
  ut_ad(referenced());
  mysql_mutex_assert_owner(&fil_system.mutex);
  fil_node_t *node= UT_LIST_GET_LAST(chain);
  ut_ad(!id || is_temporary() || node == UT_LIST_GET_FIRST(chain));

  const bool is_open= node &&
    (node->is_open() || fil_node_open_file(node, nullptr, false));

  if (!is_open)
    release();
  else if (node->deferred);
  else if (auto desired_size= recv_size)
  {
    if (id == TRX_SYS_SPACE && recv_sys.check_sys_truncate())
      goto clear;
    bool success;
    while (fil_space_extend_must_retry(this, node, desired_size, &success))
      mysql_mutex_lock(&fil_system.mutex);

    mysql_mutex_assert_owner(&fil_system.mutex);
    /* Crash recovery requires the file extension to succeed. */
    ut_a(success);
    /* InnoDB data files cannot shrink. */
    ut_a(size >= desired_size);
    if (desired_size > committed_size)
      committed_size= desired_size;

    /* There could be multiple concurrent I/O requests for this
    tablespace (multiple threads trying to extend this tablespace).

    Also, fil_space_set_recv_size_and_flags() may have been invoked
    again during the file extension while fil_system.mutex was not
    being held by us.

    Only if recv_size matches what we read originally, reset the
    field. In this way, a subsequent I/O request will handle any
    pending fil_space_set_recv_size_and_flags(). */

    if (desired_size == recv_size)
    {
      recv_size= 0;
      goto clear;
    }
  }
  else
clear:
    clear_closing();

  return is_open;
}

/** @return whether the file is usable for io() */
ATTRIBUTE_COLD bool fil_space_t::acquire_and_prepare() noexcept
{
  mysql_mutex_lock(&fil_system.mutex);
  const auto flags= acquire_low() & (STOPPING | CLOSING);
  const bool is_open= !flags || (flags == CLOSING && prepare_acquired());
  mysql_mutex_unlock(&fil_system.mutex);
  return is_open;
}

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool fil_space_extend(fil_space_t *space, uint32_t size) noexcept
{
  ut_ad(!srv_read_only_mode || space->is_temporary());
  bool success= false;
  const bool acquired= space->acquire();
  mysql_mutex_lock(&fil_system.mutex);
  if (acquired)
    while (fil_space_extend_must_retry(space, UT_LIST_GET_LAST(space->chain),
                                       size, &success))
      mysql_mutex_lock(&fil_system.mutex);
  mysql_mutex_unlock(&fil_system.mutex);
  if (acquired)
    space->release();
  return success;
}

/** Prepare to free a file from fil_system. */
inline pfs_os_file_t fil_node_t::close_to_free(bool detach_handle) noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_a(!being_extended);

  if (is_open() &&
      (space->n_pending.fetch_or(fil_space_t::CLOSING,
                                 std::memory_order_acquire) &
       fil_space_t::PENDING))
  {
    mysql_mutex_unlock(&fil_system.mutex);
    while (space->referenced())
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    mysql_mutex_lock(&fil_system.mutex);
  }

  while (is_open())
  {
    if (space->is_in_unflushed_spaces)
    {
      space->is_in_unflushed_spaces= false;
      fil_system.unflushed_spaces.remove(*space);
    }

    ut_a(!being_extended);
    if (detach_handle)
    {
      auto result= handle;
      handle= OS_FILE_CLOSED;
      return result;
    }
    bool ret= os_file_close(handle);
    ut_a(ret);
    handle= OS_FILE_CLOSED;
    break;
  }

  return OS_FILE_CLOSED;
}

/** Detach a tablespace from the cache and close the files.
@param space tablespace
@param detach_handle whether to detach the handle, instead of closing
@return detached handle
@retval OS_FILE_CLOSED if no handle was detached */
pfs_os_file_t fil_system_t::detach(fil_space_t *space, bool detach_handle)
  noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  spaces.cell_get(space->id)->remove(*space, &fil_space_t::hash);

  if (space->is_in_unflushed_spaces)
  {
    space->is_in_unflushed_spaces= false;
    unflushed_spaces.remove(*space);
  }

  if (space->is_in_default_encrypt)
  {
    space->is_in_default_encrypt= false;
    default_encrypt_tables.remove(*space);
  }

  {
    space_list_t::iterator s= space_list_t::iterator(space);
    if (space_list_last_opened == space)
    {
      if (s == space_list.begin())
      {
        ut_ad(srv_operation > SRV_OPERATION_EXPORT_RESTORED ||
              srv_shutdown_state > SRV_SHUTDOWN_NONE);
        space_list_last_opened= nullptr;
      }
      else
      {
        space_list_t::iterator prev= s;
        space_list_last_opened= &*--prev;
      }
    }
    space_list.erase(s);
  }

  if (space == sys_space)
    sys_space= nullptr;
  else if (space == temp_space)
    temp_space= nullptr;

  for (fil_node_t* node= UT_LIST_GET_FIRST(space->chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
    if (node->is_open())
    {
      ut_ad(n_open > 0);
      n_open--;
    }

  ut_ad(!detach_handle || space->id);
  ut_ad(!detach_handle || UT_LIST_GET_LEN(space->chain) <= 1);

  pfs_os_file_t handle= OS_FILE_CLOSED;

  for (fil_node_t* node= UT_LIST_GET_FIRST(space->chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
    handle= node->close_to_free(detach_handle);

  ut_ad(!space->referenced());
  return handle;
}

/** Free a tablespace object on which fil_system_t::detach() was invoked.
There must not be any pending i/o's or flushes on the files.
@param[in,out]	space		tablespace */
static void fil_space_free_low(fil_space_t *space) noexcept
{
	/* The tablespace must not be in fil_system.named_spaces. */
	ut_ad(srv_fast_shutdown == 2 || !srv_was_started
	      || space->max_lsn == 0);

	/* Wait for fil_space_t::release() after
	fil_system_t::detach(), the tablespace cannot be found, so
	fil_space_t::get() would return NULL */
	while (space->referenced()) {
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL; ) {
		ut_d(space->size -= node->size);
		ut_free(node->name);
		fil_node_t* old_node = node;
		node = UT_LIST_GET_NEXT(chain, node);
		ut_free(old_node);
	}

	ut_ad(space->size == 0);

	fil_space_destroy_crypt_data(&space->crypt_data);

	space->~fil_space_t();
	ut_free(space);
}

/** Frees a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param id          tablespace identifier
@param x_latched   whether the caller holds exclusive fil_space_t::latch
@return true if success */
bool fil_space_free(uint32_t id, bool x_latched) noexcept
{
	ut_ad(id != TRX_SYS_SPACE);

	mysql_mutex_lock(&fil_system.mutex);
	fil_space_t*	space = fil_space_get_by_id(id);

	if (space != NULL) {
		fil_system.detach(space);
	}

	mysql_mutex_unlock(&fil_system.mutex);

	if (space != NULL) {
		if (x_latched) {
			space->x_unlock();
		}

		if (!recv_recovery_is_on()) {
			log_sys.latch.wr_lock(SRW_LOCK_CALL_ false);

			if (space->max_lsn) {
				ut_d(space->max_lsn = 0);
				fil_system.named_spaces.remove(*space);
			}

			log_sys.latch.wr_unlock();
		} else {
			ut_ad(log_sys.latch_have_wr());
			if (space->max_lsn) {
				ut_d(space->max_lsn = 0);
				fil_system.named_spaces.remove(*space);
			}
		}

		fil_space_free_low(space);
	}

	return(space != NULL);
}

fil_space_t::fil_space_t(uint32_t id, uint32_t flags, bool being_imported,
                         fil_space_crypt_t *crypt_data) noexcept :
  id(id), crypt_data(crypt_data), being_imported(being_imported), flags(flags)
{
  UT_LIST_INIT(chain, &fil_node_t::chain);
  memset((void*) &latch, 0, sizeof latch);
  latch.SRW_LOCK_INIT(fil_space_latch_key);
}

fil_space_t *fil_space_t::create(uint32_t id, uint32_t flags,
                                 bool being_imported,
                                 fil_space_crypt_t *crypt_data,
                                 fil_encryption_t mode,
                                 bool opened) noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(fil_system.is_initialised());
  ut_ad(fil_space_t::is_valid_flags(flags & ~FSP_FLAGS_MEM_MASK, id));
  ut_ad(srv_page_size == UNIV_PAGE_SIZE_ORIG || flags != 0);

  DBUG_EXECUTE_IF("fil_space_create_failure", return nullptr;);

  fil_space_t** after= fil_system.spaces.cell_get(id)->search
    (&fil_space_t::hash, [id](const fil_space_t *space)
    { return !space || space->id == id; });
  ut_a(!*after);
  fil_space_t *space= new (ut_malloc_nokey(sizeof(*space)))
    fil_space_t(id, flags, being_imported, crypt_data);
  *after= space;

  if (crypt_data)
    DBUG_PRINT("crypt", ("Tablespace %" PRIu32 " encryption %d key id %" PRIu32
                         ":%s %s",
                         id, crypt_data->encryption, crypt_data->key_id,
                         fil_crypt_get_mode(crypt_data),
                         fil_crypt_get_type(crypt_data)));

  if (opened)
    fil_system.add_opened_last_to_space_list(space);
  else
    fil_system.space_list.push_back(*space);

  switch (id) {
  case 0:
    ut_ad(!fil_system.sys_space);
    fil_system.sys_space= space;
    break;
  case SRV_TMP_SPACE_ID:
    ut_ad(!fil_system.temp_space);
    fil_system.temp_space= space;
    return space;
  default:
    if (UNIV_LIKELY(id <= fil_system.max_assigned_id))
      break;
    if (UNIV_UNLIKELY(srv_operation == SRV_OPERATION_BACKUP))
      break;
    if (!fil_system.space_id_reuse_warned)
     sql_print_warning("InnoDB: Allocated tablespace ID %" PRIu32
                       ", old maximum was %" PRIu32,
                       id, fil_system.max_assigned_id);
    fil_system.max_assigned_id = id;
  }

  if ((mode == FIL_ENCRYPTION_ON ||
       (mode == FIL_ENCRYPTION_OFF || srv_encrypt_tables)) &&
      !space->is_being_imported() && fil_crypt_must_default_encrypt())
  {
    fil_system.default_encrypt_tables.push_back(*space);
    space->is_in_default_encrypt= true;

    if (srv_n_fil_crypt_threads_started)
    {
      mysql_mutex_unlock(&fil_system.mutex);
      fil_crypt_threads_signal();
      mysql_mutex_lock(&fil_system.mutex);
    }
  }

  return space;
}

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */
bool fil_assign_new_space_id(uint32_t *space_id) noexcept
{
	uint32_t id = *space_id;
	bool	success;

	mysql_mutex_lock(&fil_system.mutex);

	if (id < fil_system.max_assigned_id) {
		id = fil_system.max_assigned_id;
	}

	id++;

	if (id > (SRV_SPACE_ID_UPPER_BOUND / 2) && (id % 1000000UL == 0)) {
		ib::warn() << "You are running out of new single-table"
			" tablespace id's. Current counter is " << id
			<< " and it must not exceed" <<SRV_SPACE_ID_UPPER_BOUND
			<< "! To reset the counter to zero you have to dump"
			" all your tables and recreate the whole InnoDB"
			" installation.";
	}

	success = (id < SRV_SPACE_ID_UPPER_BOUND);

	if (success) {
		*space_id = fil_system.max_assigned_id = id;
	} else {
		ib::warn() << "You have run out of single-table tablespace"
			" id's! Current counter is " << id
			<< ". To reset the counter to zero"
			" you have to dump all your tables and"
			" recreate the whole InnoDB installation.";
		*space_id = UINT32_MAX;
	}

	mysql_mutex_unlock(&fil_system.mutex);

	return(success);
}

/** Read the first page of a data file.
@param dpage   copy of a first page, from the doublewrite buffer, or nullptr
@param no_lsn  whether to skip the FIL_PAGE_LSN check
@return whether the page was found valid */
bool fil_space_t::read_page0(const byte *dpage, bool no_lsn) noexcept
{
  ut_ad(fil_system.is_initialised());
  mysql_mutex_assert_owner(&fil_system.mutex);
  if (size)
    return true;

  fil_node_t *node= UT_LIST_GET_FIRST(chain);
  if (!node)
    return false;
  ut_ad(!UT_LIST_GET_NEXT(chain, node));

  if (UNIV_UNLIKELY(acquire_low() & STOPPING))
  {
    ut_ad("this should not happen" == 0);
    return false;
  }
  const bool ok= node->is_open() || fil_node_open_file(node, dpage, no_lsn);
  release();
  return ok;
}

void fil_space_set_recv_size_and_flags(uint32_t id, uint32_t size,
                                       uint32_t flags) noexcept
{
  ut_ad(id < SRV_SPACE_ID_UPPER_BOUND);
  mysql_mutex_assert_owner(&recv_sys.mutex);
  mysql_mutex_lock(&fil_system.mutex);
  if (fil_space_t *space= fil_space_get_by_id(id))
    if (space->read_page0(recv_sys.dblwr.find_page(page_id_t(id, 0), LSN_MAX),
                          true))
    {
      if (size)
        space->recv_size= size;
      if (flags != FSP_FLAGS_FCRC32_MASK_MARKER)
        space->flags= flags;
    }
  mysql_mutex_unlock(&fil_system.mutex);
}

/** Open each file. Never invoked on .ibd files.
@param create_new_db    whether to skip the call to fil_node_t::read_page0()
@return whether all files were opened */
bool fil_space_t::open(bool create_new_db)
{
  ut_ad(fil_system.is_initialised());
  ut_ad(!id || create_new_db);

  bool success= true;
  bool skip_read= create_new_db;

  const page_t *page= skip_read
    ? nullptr
    : recv_sys.dblwr.find_page(page_id_t{id, 0}, LSN_MAX);

  mysql_mutex_lock(&fil_system.mutex);

  for (fil_node_t *node= UT_LIST_GET_FIRST(chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
  {
    if (!node->is_open() && !fil_node_open_file_low(node, page, page))
    {
err_exit:
      success= false;
      break;
    }

    if (create_new_db)
    {
      node->find_metadata();
      continue;
    }
    if (skip_read)
    {
      size+= node->size;
      continue;
    }

    if (!node->read_page0(page, true))
    {
      fil_system.n_open--;
      os_file_close(node->handle);
      node->handle= OS_FILE_CLOSED;
      goto err_exit;
    }

    skip_read= true;
    page= nullptr;
  }

  if (!create_new_db)
    committed_size= size;
  mysql_mutex_unlock(&fil_system.mutex);
  return success;
}

/** Close each file. Only invoked on fil_system.temp_space. */
void fil_space_t::close()
{
	if (!fil_system.is_initialised()) {
		return;
	}

	mysql_mutex_lock(&fil_system.mutex);
	ut_ad(this == fil_system.temp_space
	      || srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA);

	for (fil_node_t* node = UT_LIST_GET_FIRST(chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {
		if (node->is_open()) {
			node->close();
		}
	}

	mysql_mutex_unlock(&fil_system.mutex);
}

void fil_system_t::create(ulint hash_size)
{
	ut_ad(this == &fil_system);
	ut_ad(!is_initialised());
	ut_ad(!(srv_page_size % FSP_EXTENT_SIZE));
	ut_ad(srv_page_size);

	compile_time_assert(!(UNIV_PAGE_SIZE_MAX % FSP_EXTENT_SIZE_MAX));
	compile_time_assert(!(UNIV_PAGE_SIZE_MIN % FSP_EXTENT_SIZE_MIN));

	ut_ad(hash_size > 0);

	mysql_mutex_init(fil_system_mutex_key, &mutex, nullptr);

	spaces.create(hash_size);

	need_unflushed_spaces = !write_through && buf_dblwr.need_fsync();

	fil_space_crypt_init();
#ifdef __linux__
	ssd.clear();
	char fn[sizeof(dirent::d_name)
		+ sizeof "/sys/block/" "/queue/rotational"];
	const size_t sizeof_fnp = (sizeof fn) - sizeof "/sys/block";
	memcpy(fn, "/sys/block/", sizeof "/sys/block");
	char* fnp = &fn[sizeof "/sys/block"];

	std::set<std::string> ssd_devices;
	if (DIR* d = opendir("/sys/block")) {
		while (struct dirent* e = readdir(d)) {
			if (e->d_name[0] == '.') {
				continue;
			}
			snprintf(fnp, sizeof_fnp, "%s/queue/rotational",
				 e->d_name);
			int f = open(fn, O_RDONLY);
			if (f == -1) {
				continue;
			}
			char b[sizeof "4294967295:4294967295\n"];
			ssize_t l = read(f, b, sizeof b);
			::close(f);
			if (l != 2 || memcmp("0\n", b, 2)) {
				continue;
			}
			snprintf(fnp, sizeof_fnp, "%s/dev", e->d_name);
			f = open(fn, O_RDONLY);
			if (f == -1) {
				continue;
			}
			l = read(f, b, sizeof b);
			::close(f);
			if (l <= 0 || b[l - 1] != '\n') {
				continue;
			}
			b[l - 1] = '\0';
			char* end = b;
			unsigned long dev_major = strtoul(b, &end, 10);
			if (b == end || *end != ':'
			    || dev_major != unsigned(dev_major)) {
				continue;
			}
			char* c = end + 1;
			unsigned long dev_minor = strtoul(c, &end, 10);
			if (c == end || *end
			    || dev_minor != unsigned(dev_minor)) {
				continue;
			}
			ssd.push_back(makedev(unsigned(dev_major),
					      unsigned(dev_minor)));
		}
		closedir(d);
	}
	/* fil_system_t::is_ssd() assumes the following */
	ut_ad(makedev(0, 8) == 8);
	ut_ad(makedev(0, 4) == 4);
	ut_ad(makedev(0, 2) == 2);
	ut_ad(makedev(0, 1) == 1);
#endif
}

void fil_system_t::close() noexcept
{
  ut_ad(this == &fil_system);
  ut_a(unflushed_spaces.empty());
  ut_a(space_list.empty());
  ut_ad(!sys_space);
  ut_ad(!temp_space);

  if (is_initialised())
  {
    spaces.free();
    mysql_mutex_destroy(&mutex);
    fil_space_crypt_cleanup();
  }

  ut_ad(!is_initialised());

#ifdef __linux__
  ssd.clear();
  ssd.shrink_to_fit();
#endif /* __linux__ */
}

void fil_system_t::add_opened_last_to_space_list(fil_space_t *space) noexcept
{
  if (UNIV_LIKELY(space_list_last_opened != nullptr))
    space_list.insert(++space_list_t::iterator(space_list_last_opened), *space);
  else
    space_list.push_front(*space);
  space_list_last_opened= space;
}

/** Extend all open data files to the recovered size */
ATTRIBUTE_COLD void fil_system_t::extend_to_recv_size() noexcept
{
  ut_ad(is_initialised());
  mysql_mutex_lock(&mutex);
  for (fil_space_t &space : fil_system.space_list)
  {
    const uint32_t size= space.recv_size;

    if (size > space.size)
    {
      if (space.is_closing())
        continue;
      space.reacquire();
      bool success;
      while (fil_space_extend_must_retry(&space, UT_LIST_GET_LAST(space.chain),
                                         size, &success))
        mysql_mutex_lock(&mutex);
      /* Crash recovery requires the file extension to succeed. */
      ut_a(success);
      space.release();
    }
  }
  mysql_mutex_unlock(&mutex);
}

ATTRIBUTE_COLD void fil_space_t::reopen_all()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  fil_system.freeze_space_list++;

  for (fil_space_t &space : fil_system.space_list)
  {
    for (fil_node_t *node= UT_LIST_GET_FIRST(space.chain); node;
         node= UT_LIST_GET_NEXT(chain, node))
      if (node->is_open())
        goto need_to_close;
    continue;

  need_to_close:
    uint32_t p= space.n_pending.fetch_or(CLOSING, std::memory_order_acquire);
    if (p & (STOPPING | CLOSING))
      continue;

    for (fil_node_t *node= UT_LIST_GET_FIRST(space.chain); node;
         node= UT_LIST_GET_NEXT(chain, node))
    {
      if (!node->is_open())
        continue;

      ulint type= OS_DATA_FILE;

#if defined _WIN32 || defined O_DIRECT
      switch (FSP_FLAGS_GET_ZIP_SSIZE(space.flags)) {
      case 1: case 2:
        type= OS_DATA_FILE_NO_O_DIRECT;
      }
#endif

      for (ulint count= 10000; count--;)
      {
        p= space.pending();

        if (!(p & CLOSING) || (p & STOPPING))
          break;

        if (!(p & PENDING) && !node->being_extended)
        {
          space.reacquire();
          mysql_mutex_unlock(&fil_system.mutex);
          /* Unconditionally flush the file, because
          fil_system.write_through was updated prematurely,
          potentially causing some flushes to be lost. */
          os_file_flush(node->handle);
          mysql_mutex_lock(&fil_system.mutex);
          p= space.n_pending.fetch_sub(1, std::memory_order_relaxed) - 1;

          if (!(p & CLOSING) || (p & STOPPING))
            break;

          if (!(p & PENDING) && !node->being_extended)
          {
            ut_a(os_file_close(node->handle));
            bool success;
            node->handle= os_file_create(innodb_data_file_key, node->name,
                                         node->is_raw_disk
                                         ? OS_FILE_OPEN_RAW : OS_FILE_OPEN,
                                         type,
                                         srv_read_only_mode, &success);
            ut_a(success);
            goto next_file;
          }
        }

        space.reacquire();
        mysql_mutex_unlock(&fil_system.mutex);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        mysql_mutex_lock(&fil_system.mutex);
        space.release();

        if (!node->is_open())
          goto next_file;
      }

      if (!(p & CLOSING) || (p & STOPPING))
      next_file:
        continue;

      sql_print_error("InnoDB: Failed to reopen file '%s' due to " UINT32PF
                      " operations", node->name, p & PENDING);
    }
  }

  fil_system.freeze_space_list--;
}

void fil_system_t::set_write_through(bool write_through)
{
  mysql_mutex_lock(&mutex);

  if (write_through != is_write_through())
  {
    this->write_through= write_through;
    fil_space_t::reopen_all();
    need_unflushed_spaces = !write_through && buf_dblwr.need_fsync();
  }

  mysql_mutex_unlock(&mutex);
}

void fil_system_t::set_buffered(bool buffered)
{
  mysql_mutex_lock(&mutex);

  if (buffered != is_buffered())
  {
    this->buffered= buffered;
    fil_space_t::reopen_all();
  }

  mysql_mutex_unlock(&mutex);
}

/** Close all tablespace files at shutdown */
void fil_space_t::close_all() noexcept
{
  if (!fil_system.is_initialised())
    return;

  /* At shutdown, we should not have any files in this list. */
  ut_ad(srv_fast_shutdown == 2 || !srv_was_started ||
        fil_system.named_spaces.empty());
  fil_flush_file_spaces();

  mysql_mutex_lock(&fil_system.mutex);

  while (!fil_system.space_list.empty())
  {
    fil_space_t &space= fil_system.space_list.front();

    for (fil_node_t *node= UT_LIST_GET_FIRST(space.chain); node != NULL;
         node= UT_LIST_GET_NEXT(chain, node))
    {
      if (!node->is_open())
      next:
        continue;

      for (ulint count= 10000; count--;)
      {
        const auto n= space.set_closing();
        if (n & STOPPING)
          goto next;
        if (!(n & (PENDING | NEEDS_FSYNC)))
        {
          node->close();
          goto next;
        }
        mysql_mutex_unlock(&fil_system.mutex);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        mysql_mutex_lock(&fil_system.mutex);
        if (!node->is_open())
          goto next;
      }

      sql_print_error("InnoDB: File '%s' has " UINT32PF " operations",
                      node->name, space.referenced());
    }

    fil_system.detach(&space);
    mysql_mutex_unlock(&fil_system.mutex);
    fil_space_free_low(&space);
    mysql_mutex_lock(&fil_system.mutex);
  }

  mysql_mutex_unlock(&fil_system.mutex);

  ut_ad(srv_fast_shutdown == 2 || !srv_was_started ||
        fil_system.named_spaces.empty());
}

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
void fil_set_max_space_id_if_bigger(uint32_t max_id) noexcept
{
	ut_a(max_id < SRV_SPACE_ID_UPPER_BOUND);

	mysql_mutex_lock(&fil_system.mutex);

	if (fil_system.max_assigned_id < max_id) {

		fil_system.max_assigned_id = max_id;
	}

	mysql_mutex_unlock(&fil_system.mutex);
}

/** Acquire a tablespace reference.
@param id      tablespace identifier
@return tablespace
@retval nullptr if the tablespace is missing or inaccessible */
fil_space_t *fil_space_t::get(uint32_t id) noexcept
{
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  const uint32_t n= space ? space->acquire_low() : 0;

  if (n & STOPPING)
    space= nullptr;
  else if ((n & CLOSING) && !space->prepare_acquired())
    space= nullptr;

  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}

/** Write a log record about a file operation.
@param type           file operation
@param first_page_no  first page number in the file
@param path           file path
@param new_path       new file path for type=FILE_RENAME */
inline void mtr_t::log_file_op(mfile_type_t type, uint32_t space_id,
			       const char *path, const char *new_path)
{
  ut_ad((new_path != nullptr) == (type == FILE_RENAME));
  ut_ad(!(byte(type) & 15));

  /* fil_name_parse() requires that there be at least one path
  separator and that the file path end with ".ibd". */
  ut_ad(strchr(path, '/'));
  ut_ad(!strcmp(&path[strlen(path) - strlen(DOT_IBD)], DOT_IBD));

  m_modifications= true;
  if (!is_logged())
    return;
  m_last= nullptr;

  const size_t len= strlen(path);
  const size_t new_len= new_path ? 1 + strlen(new_path) : 0;
  ut_ad(len > 0);
  byte *const log_ptr= m_log.open(1 + 3/*length*/ + 5/*space_id*/ +
                                  1/*page_no=0*/);
  *log_ptr= type;
  byte *end= log_ptr + 1;
  end= mlog_encode_varint(end, space_id);
  *end++= 0;
  const byte *const final_end= end + len + new_len;
  if (UNIV_LIKELY(final_end >= &log_ptr[16]))
  {
    size_t total_len= final_end - log_ptr - 15;
    if (total_len >= MIN_3BYTE)
      total_len+= 2;
    else if (total_len >= MIN_2BYTE)
      total_len++;
    end= mlog_encode_varint(log_ptr + 1, total_len);
    end= mlog_encode_varint(end, space_id);
    *end++= 0;
  }
  else
  {
    *log_ptr= static_cast<byte>(*log_ptr | (final_end - &log_ptr[1]));
    ut_ad(*log_ptr & 15);
  }

  m_log.close(end);

  if (new_path)
  {
    ut_ad(strchr(new_path, '/'));
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len + 1));
    m_log.push(reinterpret_cast<const byte*>(new_path), uint32_t(new_len - 1));
  }
  else
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len));
}

/** Write FILE_MODIFY for a file.
@param[in]	space_id	tablespace id
@param[in]	name		tablespace file name
@param[in,out]	mtr		mini-transaction */
static void fil_name_write(uint32_t space_id, const char *name,
                           mtr_t *mtr)
{
  ut_ad(!is_predefined_tablespace(space_id));
  mtr->log_file_op(FILE_MODIFY, space_id, name);
}

fil_space_t *fil_space_t::drop(uint32_t id, pfs_os_file_t *detached_handle)
{
  ut_a(!is_system_tablespace(id));
  ut_ad(id != SRV_TMP_SPACE_ID);
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);

  if (!space)
  {
    mysql_mutex_unlock(&fil_system.mutex);
    return nullptr;
  }

  if (space->pending() & STOPPING)
  {
    /* A thread executing DDL and another thread executing purge may
    be executing fil_delete_tablespace() concurrently for the same
    tablespace. Wait for the other thread to complete the operation. */
    for (ulint count= 0;; count++)
    {
      space= fil_space_get_by_id(id);
      ut_ad(!space || space->is_stopping());
      mysql_mutex_unlock(&fil_system.mutex);
      if (!space)
        return nullptr;
      /* Issue a warning every 10.24 seconds, starting after 2.56 seconds */
      if ((count & 511) == 128)
        sql_print_warning("InnoDB: Waiting for tablespace " UINT32PF
                          " to be deleted", id);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      mysql_mutex_lock(&fil_system.mutex);
    }
  }

  /* We must be the first one to set either STOPPING flag on the .ibd file,
  because the flags are only being set here, within a critical section of
  fil_system.mutex. */
  unsigned pending;
  ut_d(pending=)
    space->n_pending.fetch_add(STOPPING_READS + 1, std::memory_order_relaxed);
  ut_ad(!(pending & STOPPING));
  mysql_mutex_unlock(&fil_system.mutex);

  if (space->crypt_data)
    fil_space_crypt_close_tablespace(space);

  if (!space->is_being_imported())
  {
    if (id >= srv_undo_space_id_start &&
        id < srv_undo_space_id_start + srv_undo_tablespaces_open)
    {
      os_file_delete(innodb_data_file_key, space->chain.start->name);
      goto deleted;
    }

    /* Before deleting the file, persistently write a log record. */
    mtr_t mtr;
    mtr.start();
    mtr.log_file_op(FILE_DELETE, id, space->chain.start->name);
    mtr.commit_file(*space, nullptr);

    if (FSP_FLAGS_HAS_DATA_DIR(space->flags))
      RemoteDatafile::delete_link_file(space->name());

    os_file_delete(innodb_data_file_key, space->chain.start->name);
  }

  if (char *cfg_name= fil_make_filepath(space->chain.start->name,
                                        fil_space_t::name_type{}, CFG, false))
  {
    os_file_delete_if_exists(innodb_data_file_key, cfg_name, nullptr);
    ut_free(cfg_name);
  }

 deleted:
  mysql_mutex_lock(&fil_system.mutex);
  ut_ad(space == fil_space_get_by_id(id));
  pending=
    space->n_pending.fetch_add(STOPPING_WRITES - 1, std::memory_order_relaxed);
  ut_ad((pending & STOPPING) == STOPPING_READS);
  ut_ad(pending & PENDING);
  pending&= PENDING;
  if (--pending)
  {
    for (ulint count= 0;; count++)
    {
      ut_ad(space == fil_space_get_by_id(id));
      pending= space->n_pending.load(std::memory_order_relaxed) & PENDING;
      if (!pending)
        break;
      mysql_mutex_unlock(&fil_system.mutex);
      /* Issue a warning every 10.24 seconds, starting after 2.56 seconds */
      if ((count & 511) == 128)
        sql_print_warning("InnoDB: Trying to delete tablespace '%s' "
                          "but there are %u pending operations",
                          space->chain.start->name, pending);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      mysql_mutex_lock(&fil_system.mutex);
    }
  }

  pfs_os_file_t handle= fil_system.detach(space, true);
  mysql_mutex_unlock(&fil_system.mutex);
  if (detached_handle)
    *detached_handle = handle;
  else
    os_file_close(handle);
  return space;
}

/** Close a single-table tablespace on failed IMPORT TABLESPACE.
The tablespace must be cached in the memory cache.
Free all pages used by the tablespace. */
void fil_close_tablespace(uint32_t id) noexcept
{
	ut_ad(!is_system_tablespace(id));
	fil_space_t* space = fil_space_t::drop(id, nullptr);
	if (!space) {
		return;
	}

	space->x_lock();
	ut_ad(space->is_stopping());

	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since space->is_stopping() holds, readahead
	can no longer read more pages of this tablespace to buf_pool.
	Thus we can clean the tablespace out of buf_pool
	completely and permanently. */
	while (buf_flush_list_space(space));

	space->x_unlock();
	log_sys.latch.wr_lock(SRW_LOCK_CALL_ false);
	if (space->max_lsn != 0) {
		ut_d(space->max_lsn = 0);
		fil_system.named_spaces.remove(*space);
	}
	log_sys.latch.wr_unlock();
	fil_space_free_low(space);
}

/** Delete a tablespace and associated .ibd file.
@param id    tablespace identifier
@return detached file handle (to be closed by the caller)
@return	OS_FILE_CLOSED if no file existed */
pfs_os_file_t fil_delete_tablespace(uint32_t id) noexcept
{
  ut_ad(!is_system_tablespace(id));
  pfs_os_file_t handle= OS_FILE_CLOSED;
  if (fil_space_t *space= fil_space_t::drop(id, &handle))
    fil_space_free_low(space);
  return handle;
}

char* fil_make_filepath_low(const char *path,
                            const fil_space_t::name_type &name,
                            ib_extention extension, bool trim_name) noexcept
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is NULL, we can use the default path,
	but there needs to be a name. */
	ut_ad(path || name.data());

	if (path == NULL) {
		path = fil_path_to_mysql_datadir;
	}

	ulint	len		= 0;	/* current length */
	ulint	path_len	= strlen(path);
	const char* suffix	= dot_ext[extension];
	ulint	suffix_len	= strlen(suffix);
	ulint	full_len	= path_len + 1 + name.size() + suffix_len + 1;

	char*	full_name = static_cast<char*>(ut_malloc_nokey(full_len));
	if (full_name == NULL) {
		return NULL;
	}

	/* If the name is a relative or absolute path, do not prepend "./". */
	if (path[0] == '.'
	    && (path[1] == '\0' || path[1] == '/' IF_WIN(|| path[1] == '\\',))
	    && name.size() && (name.data()[0] == '.'
			       || is_absolute_path(name.data()))) {
		path = NULL;
		path_len = 0;
	}

	if (path != NULL) {
		memcpy(full_name, path, path_len);
		len = path_len;
	}

	full_name[len] = '\0';

	if (trim_name) {
		/* Find the offset of the last DIR separator and set it to
		null in order to strip off the old basename from this path. */
		char* last_dir_sep = strrchr(full_name, '/');
#ifdef _WIN32
		if (char *last = strrchr(full_name, '\\')) {
			if (last > last_dir_sep) {
				last_dir_sep = last;
			}
		}
#endif
		if (last_dir_sep) {
			last_dir_sep[0] = '\0';
			len = strlen(full_name);
		}
	}

	if (name.size()) {
		if (len && full_name[len - 1] != '/') {
			/* Add a DIR separator */
			full_name[len] = '/';
			full_name[++len] = '\0';
		}

		char*	ptr = &full_name[len];
		memcpy(ptr, name.data(), name.size());
		len += name.size();
		full_name[len] = '\0';
	}

	/* Make sure that the specified suffix is at the end of the filepath
	string provided. This assumes that the suffix starts with '.'.
	If the first char of the suffix is found in the filepath at the same
	length as the suffix from the end, then we will assume that there is
	a previous suffix that needs to be replaced. */
	if (suffix != NULL) {
		/* Need room for the trailing null byte. */
		ut_ad(len < full_len);

		if ((len > suffix_len)
		   && (full_name[len - suffix_len] == suffix[0])) {
			/* Another suffix exists, make it the one requested. */
			memcpy(&full_name[len - suffix_len], suffix, suffix_len);

		} else {
			/* No previous suffix, add it. */
			ut_ad(len + suffix_len < full_len);
			memcpy(&full_name[len], suffix, suffix_len);
			full_name[len + suffix_len] = '\0';
		}
	}

	return(full_name);
}

char *fil_make_filepath(const char* path, const table_name_t name,
                        ib_extention suffix, bool strip_name) noexcept
{
  return fil_make_filepath_low(path, {name.m_name, strlen(name.m_name)},
                               suffix, strip_name);
}

/** Wrapper function over fil_make_filepath_low() to build directory name.
@param path the directory path or the full path and filename
@return own: directory name */
static inline char *fil_make_dirpath(const char *path) noexcept
{
  return fil_make_filepath_low(path, fil_space_t::name_type{}, NO_EXT, true);
}

dberr_t fil_space_t::rename(const char *path, bool log, bool replace) noexcept
{
  ut_ad(UT_LIST_GET_LEN(chain) == 1);
  ut_ad(!is_predefined_tablespace(id));

  const char *old_path= chain.start->name;

  ut_ad(strchr(old_path, '/'));
  ut_ad(strchr(path, '/'));

  if (!strcmp(path, old_path))
    return DB_SUCCESS;

  if (!log)
  {
    if (!os_file_rename(innodb_data_file_key, old_path, path))
      return DB_ERROR;
    mysql_mutex_lock(&fil_system.mutex);
    ut_free(chain.start->name);
    chain.start->name= mem_strdup(path);
    mysql_mutex_unlock(&fil_system.mutex);
    return DB_SUCCESS;
  }

  bool exists= false;
  os_file_type_t ftype;

  /* Check upfront if the rename operation might succeed, because we
  must durably write redo log before actually attempting to execute
  the rename in the file system. */
  if (os_file_status(old_path, &exists, &ftype) && !exists)
  {
    sql_print_error("InnoDB: Cannot rename '%s' to '%s'"
                    " because the source file does not exist.",
                    old_path, path);
    return DB_TABLESPACE_NOT_FOUND;
  }

  if (!replace)
  {
    char *schema_path= fil_make_dirpath(path);
    if (!schema_path)
      return DB_ERROR;

    exists= false;
    bool schema_fail= os_file_status(schema_path, &exists, &ftype) && !exists;
    ut_free(schema_path);

    if (schema_fail)
    {
      sql_print_error("InnoDB: Cannot rename '%s' to '%s'"
                      " because the target schema directory doesn't exist.",
                      old_path, path);
      return DB_ERROR;
    }

    exists= false;
    if (!os_file_status(path, &exists, &ftype) || exists)
    {
      sql_print_error("InnoDB: Cannot rename '%s' to '%s'"
                      " because the target file exists.",
                      old_path, path);
      return DB_TABLESPACE_EXISTS;
    }
  }

  mtr_t mtr;
  mtr.start();
  mtr.log_file_op(FILE_RENAME, id, old_path, path);
  return mtr.commit_file(*this, path) ? DB_SUCCESS : DB_ERROR;
}

/** Create a tablespace file.
@param[in]	space_id	Tablespace ID
@param[in]	name		Tablespace name in dbname/tablename format.
@param[in]	path		Path and filename of the datafile to create.
@param[in]	flags		Tablespace flags
@param[in]	size		Initial size of the tablespace file in pages,
must be >= FIL_IBD_FILE_INITIAL_SIZE
@param[in]	mode		MariaDB encryption mode
@param[in]	key_id		MariaDB encryption key_id
@param[out]	err		DB_SUCCESS or error code
@return	the created tablespace
@retval	NULL	on error */
fil_space_t*
fil_ibd_create(
	uint32_t	space_id,
	const table_name_t name,
	const char*	path,
	uint32_t	flags,
	uint32_t	size,
	fil_encryption_t mode,
	uint32_t	key_id,
	dberr_t*	err) noexcept
{
	pfs_os_file_t	file;
	bool		success;
	mtr_t		mtr;
	bool		has_data_dir = FSP_FLAGS_HAS_DATA_DIR(flags) != 0;

	ut_ad(!is_system_tablespace(space_id));
	ut_ad(!srv_read_only_mode);
	ut_a(space_id < SRV_SPACE_ID_UPPER_BOUND);
	ut_a(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_a(fil_space_t::is_valid_flags(flags & ~FSP_FLAGS_MEM_MASK, space_id));

	/* Create the subdirectories in the path, if they are
	not there already. */
	*err = os_file_create_subdirs_if_needed(path);
	if (*err != DB_SUCCESS) {
		return NULL;
	}

	mtr.start();
	mtr.log_file_op(FILE_CREATE, space_id, path);
	log_sys.latch.wr_lock(SRW_LOCK_CALL_ false);
	auto lsn= mtr.commit_files();
	log_sys.latch.wr_unlock();
	mtr.flag_wr_unlock();
	log_write_up_to(lsn, true);

	static_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096,
		      "compatibility");
#if defined _WIN32 || defined O_DIRECT
	ulint type;
	switch (FSP_FLAGS_GET_ZIP_SSIZE(flags)) {
	case 1:
	case 2:
		type = OS_DATA_FILE_NO_O_DIRECT;
		break;
	default:
		type = OS_DATA_FILE;
	}
#else
	constexpr auto type = OS_DATA_FILE;
#endif

	file = os_file_create(
		innodb_data_file_key, path,
		OS_FILE_CREATE,
		type, srv_read_only_mode, &success);

	if (!success) {
		/* The following call will print an error message */
		switch (os_file_get_last_error(true)) {
		case OS_FILE_ALREADY_EXISTS:
			ib::info() << "The file '" << path << "'"
				" already exists though the"
				" corresponding table did not exist"
				" in the InnoDB data dictionary."
				" You can resolve the problem by removing"
				" the file.";
			*err = DB_TABLESPACE_EXISTS;
			break;
		case OS_FILE_DISK_FULL:
			*err = DB_OUT_OF_FILE_SPACE;
			break;
		default:
			*err = DB_ERROR;
		}
		ib::error() << "Cannot create file '" << path << "'";
		return NULL;
	}

	const bool is_compressed = fil_space_t::is_compressed(flags);
#ifdef _WIN32
	const bool is_sparse = is_compressed;
	if (is_compressed) {
		os_file_set_sparse_win32(file);
	}
#else
	const bool is_sparse = is_compressed
		&& DB_SUCCESS == os_file_punch_hole(file, 0, 4096)
		&& !my_test_if_thinly_provisioned(file);
#endif

	if (fil_space_t::full_crc32(flags)) {
		flags |= FSP_FLAGS_FCRC32_PAGE_SSIZE();
	} else {
		flags |= FSP_FLAGS_PAGE_SSIZE();
	}

	/* Create crypt data if the tablespace is either encrypted or user has
	requested it to remain unencrypted. */
	fil_space_crypt_t* crypt_data = (mode != FIL_ENCRYPTION_DEFAULT
					 || srv_encrypt_tables)
		? fil_space_create_crypt_data(mode, key_id)
		: nullptr;

	if (!os_file_set_size(path, file,
			      os_offset_t(size) << srv_page_size_shift,
			      is_sparse)) {
		*err = DB_OUT_OF_FILE_SPACE;
err_exit:
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		free(crypt_data);
		return nullptr;
	}

	fil_space_t::name_type space_name;

	if (has_data_dir) {
		/* Make the ISL file if the IBD file is not
		in the default location. */
		space_name = {name.m_name, strlen(name.m_name)};
		*err = RemoteDatafile::create_link_file(space_name, path);
		if (*err != DB_SUCCESS) {
			goto err_exit;
		}
	}

	DBUG_EXECUTE_IF("checkpoint_after_file_create",
			log_make_checkpoint(););

	mysql_mutex_lock(&fil_system.mutex);
	if (fil_space_t* space = fil_space_t::create(space_id,
						     flags, false,
						     crypt_data, mode, true)) {
		fil_node_t* node = space->add(path, file, size, false, true);
		node->find_metadata(IF_WIN(,true));
		mysql_mutex_unlock(&fil_system.mutex);
		mtr.start();
		mtr.set_named_space(space);
		ut_a(fsp_header_init(space, size, &mtr) == DB_SUCCESS);
		mtr.commit();
		return space;
	} else {
		mysql_mutex_unlock(&fil_system.mutex);
	}

	if (space_name.data()) {
		RemoteDatafile::delete_link_file(space_name);
	}

	*err = DB_ERROR;
	goto err_exit;
}

fil_space_t *fil_ibd_open(uint32_t id, uint32_t flags,
                          fil_space_t::validate validate,
                          fil_space_t::name_type name,
                          const char *path_in, dberr_t *err) noexcept
{
	mysql_mutex_lock(&fil_system.mutex);
	fil_space_t* space = fil_space_get_by_id(id);
	mysql_mutex_unlock(&fil_system.mutex);
	if (space) {
		if (validate == fil_space_t::VALIDATE_SPACE_ID
		    && !srv_read_only_mode) {
			fsp_flags_try_adjust(space,
					     flags & ~FSP_FLAGS_MEM_MASK);
		}
		return space;
	}

	dberr_t local_err = DB_SUCCESS;

	/* Table flags can be ULINT_UNDEFINED if
	dict_tf_to_fsp_flags_failure is set. */
	if (flags == UINT32_MAX) {
corrupted:
		local_err = DB_CORRUPTION;
func_exit:
		if (err) *err = local_err;
		return space;
	}

	ut_ad(fil_space_t::is_valid_flags(flags & ~FSP_FLAGS_MEM_MASK, id));

	Datafile	df_default;	/* default location */
	RemoteDatafile	df_remote;	/* remote location */
	ulint		tablespaces_found = 0;
	ulint		valid_tablespaces_found = 0;

	df_default.init(flags);
	df_remote.init(flags);

	/* Discover the correct file by looking in three possible locations
	while avoiding unecessary effort. */

	/* We will always look for an ibd in the default location. */
	df_default.make_filepath(nullptr, name, IBD);

	/* Look for a filepath embedded in an ISL where the default file
	would be. */
	bool must_validate = df_remote.open_link_file(name);

	if (must_validate) {
		if (df_remote.open_read_only(true) == DB_SUCCESS) {
			ut_ad(df_remote.is_open());
			++tablespaces_found;
		} else {
			/* The following call prints an error message */
			os_file_get_last_error(true);
			ib::error() << "A link file was found named '"
				    << df_remote.link_filepath()
				    << "' but the linked tablespace '"
				    << df_remote.filepath()
				    << "' could not be opened read-only.";
		}
	} else if (path_in && !df_default.same_filepath_as(path_in)) {
		/* Dict path is not the default path. Always validate
		remote files. If default is opened, it was moved. */
		must_validate = true;
	} else if (validate >= fil_space_t::VALIDATE_SPACE_ID) {
		must_validate = true;
	}

	const bool operation_not_for_export =
	  srv_operation != SRV_OPERATION_RESTORE_EXPORT
	  && srv_operation != SRV_OPERATION_EXPORT_RESTORED;

	/* Always look for a file at the default location. But don't log
	an error if the tablespace is already open in remote or dict. */
	ut_a(df_default.filepath());

	/* Mariabackup will not copy files whose names start with
	#sql-. We will suppress messages about such files missing on
	the first server startup. The tables ought to be dropped by
	drop_garbage_tables_after_restore() a little later. */

	const bool strict = (validate != fil_space_t::MAYBE_MISSING)
		&& !tablespaces_found
		&& operation_not_for_export
		&& !(srv_operation == SRV_OPERATION_NORMAL
		     && srv_start_after_restore
		     && srv_force_recovery < SRV_FORCE_NO_BACKGROUND
		     && dict_table_t::is_temporary_name(
			     df_default.filepath()));

	if (df_default.open_read_only(strict) == DB_SUCCESS) {
		ut_ad(df_default.is_open());
		++tablespaces_found;
	}

	/* Check if multiple locations point to the same file. */
	if (tablespaces_found > 1 && df_default.same_as(df_remote)) {
		/* A link file was found with the default path in it.
		Use the default path and delete the link file. */
		--tablespaces_found;
		df_remote.delete_link_file();
		df_remote.close();
	}

	/*  We have now checked all possible tablespace locations and
	have a count of how many unique files we found.  If things are
	normal, we only found 1. */
	/* For encrypted tablespace, we need to check the
	encryption in header of first page. */
	if (!must_validate && tablespaces_found == 1) {
		goto skip_validate;
	}

	/* Read and validate the first page of these three tablespace
	locations, if found. */
	valid_tablespaces_found +=
		(df_remote.validate_to_dd(id, flags) == DB_SUCCESS);

	valid_tablespaces_found +=
		(df_default.validate_to_dd(id, flags) == DB_SUCCESS);

	/* Make sense of these three possible locations.
	First, bail out if no tablespace files were found. */
	if (valid_tablespaces_found == 0) {
		if (!strict
		    && IF_WIN(GetLastError() == ERROR_FILE_NOT_FOUND
			      || GetLastError() == ERROR_PATH_NOT_FOUND,
			      errno == ENOENT)) {
			/* Suppress a message about a missing file. */
			goto corrupted;
		}

		if (!operation_not_for_export) {
			goto corrupted;
		}
		sql_print_error("InnoDB: Could not find a valid tablespace"
				" file for %.*s. %s",
				static_cast<int>(name.size()), name.data(),
				TROUBLESHOOT_DATADICT_MSG);
		goto corrupted;
	}
	if (!must_validate) {
		goto skip_validate;
	}

	/* Do not open any tablespaces if more than one tablespace with
	the correct space ID and flags were found. */
	if (df_default.is_open() && df_remote.is_open()) {
		ib::error()
			<< "A tablespace has been found in multiple places: "
			<< df_default.filepath()
			<< "(Space ID=" << df_default.space_id()
			<< ", Flags=" << df_default.flags()
			<< ") and "
			<< df_remote.filepath()
			<< "(Space ID=" << df_remote.space_id()
			<< ", Flags=" << df_remote.flags()
			<< (valid_tablespaces_found > 1 || srv_force_recovery
			    ? "); will not open"
			    : ")");

		/* Force-recovery will allow some tablespaces to be
		skipped by REDO if there was more than one file found.
		Unlike during the REDO phase of recovery, we now know
		if the tablespace is valid according to the dictionary,
		which was not available then. So if we did not force
		recovery and there is only one good tablespace, ignore
		any bad tablespaces. */
		if (valid_tablespaces_found > 1 || srv_force_recovery > 0) {
			/* If the file is not open it cannot be valid. */
			ut_ad(df_default.is_open() || !df_default.is_valid());
			ut_ad(df_remote.is_open()  || !df_remote.is_valid());

			/* Having established that, this is an easy way to
			look for corrupted data files. */
			if (df_default.is_open() != df_default.is_valid()
			    || df_remote.is_open() != df_remote.is_valid()) {
				goto corrupted;
			}
error:
			local_err = DB_ERROR;
			goto func_exit;
		}

		/* There is only one valid tablespace found and we did
		not use srv_force_recovery during REDO.  Use this one
		tablespace and clean up invalid tablespace pointers */
		if (df_default.is_open() && !df_default.is_valid()) {
			df_default.close();
			tablespaces_found--;
		}

		if (df_remote.is_open() && !df_remote.is_valid()) {
			df_remote.close();
			tablespaces_found--;
		}
	}

	/* At this point, there should be only one filepath. */
	ut_a(tablespaces_found == 1);
	ut_a(valid_tablespaces_found == 1);

skip_validate:
	const byte* first_page =
		df_default.is_open() ? df_default.get_first_page() :
		df_remote.get_first_page();

	fil_space_crypt_t* crypt_data = first_page
		? fil_space_read_crypt_data(fil_space_t::zip_size(flags),
					    first_page)
		: NULL;

	mysql_mutex_lock(&fil_system.mutex);
	space = fil_space_t::create(id, flags,
				    validate == fil_space_t::VALIDATE_IMPORT,
				    crypt_data);
	if (!space) {
		mysql_mutex_unlock(&fil_system.mutex);
		goto error;
	}

	/* We do not measure the size of the file, that is why
	we pass the 0 below */

	space->add(
		df_remote.is_open() ? df_remote.filepath() :
		df_default.filepath(), OS_FILE_CLOSED, 0, false, true);
	mysql_mutex_unlock(&fil_system.mutex);

	if (must_validate && !srv_read_only_mode) {
		df_remote.close();
		df_default.close();
		if (space->acquire()) {
			if (validate < fil_space_t::VALIDATE_IMPORT) {
				fsp_flags_try_adjust(space, flags
						     & ~FSP_FLAGS_MEM_MASK);
			}
			space->release();
		}
	}

	goto func_exit;
}

/** Discover the correct IBD file to open given a remote or missing
filepath from the REDO log. Administrators can move a crashed
database to another location on the same machine and try to recover it.
Remote IBD files might be moved as well to the new location.
    The problem with this is that the REDO log contains the old location
which may be still accessible.  During recovery, if files are found in
both locations, we can chose on based on these priorities;
1. Default location
2. ISL location
3. REDO location
@param[in]	space_id	tablespace ID
@param[in]	df		Datafile object with path from redo
@return true if a valid datafile was found, false if not */
static
bool
fil_ibd_discover(
	ulint		space_id,
	Datafile&	df)
{
	Datafile	df_def_per;	/* default file-per-table datafile */
	RemoteDatafile	df_rem_per;	/* remote file-per-table datafile */

	/* Look for the datafile in the default location. */
	const char*	filename = df.filepath();
	const char*	basename = base_name(filename);

	/* If this datafile is file-per-table it will have a schema dir. */
	ulint		sep_found = 0;
	const char*	db = basename;
	for (; db > filename && sep_found < 2; db--) {
		switch (db[0]) {
#ifdef _WIN32
		case '\\':
#endif
		case '/':
			sep_found++;
		}
	}
	if (sep_found == 2) {
		db += 2;
		df_def_per.init(0);
		df_def_per.set_filepath(db);
		if (df_def_per.open_read_only(false) == DB_SUCCESS
		    && df_def_per.validate_for_recovery() == DB_SUCCESS
		    && df_def_per.space_id() == space_id) {
			df.set_filepath(df_def_per.filepath());
			df.open_read_only(false);
			return(true);
		}

		/* Look for a remote file-per-table tablespace. */

		switch (srv_operation) {
		case SRV_OPERATION_BACKUP:
		case SRV_OPERATION_RESTORE_DELTA:
		case SRV_OPERATION_BACKUP_NO_DEFER:
			ut_ad(0);
			break;
		case SRV_OPERATION_RESTORE_EXPORT:
		case SRV_OPERATION_RESTORE:
			break;
		case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_EXPORT_RESTORED:
			size_t len= strlen(db);
			if (len <= 4 || strcmp(db + len - 4, dot_ext[IBD])) {
				break;
			}
			df_rem_per.open_link_file({db, len - 4});

			if (!df_rem_per.filepath()) {
				break;
			}

			/* An ISL file was found with contents. */
			if (df_rem_per.open_read_only(false) != DB_SUCCESS
				|| df_rem_per.validate_for_recovery()
				   != DB_SUCCESS) {

				/* Assume that this ISL file is intended to
				be used. Do not continue looking for another
				if this file cannot be opened or is not
				a valid IBD file. */
				ib::error() << "ISL file '"
					<< df_rem_per.link_filepath()
					<< "' was found but the linked file '"
					<< df_rem_per.filepath()
					<< "' could not be opened or is"
					" not correct.";
				return(false);
			}

			/* Use this file if it has the space_id from the
			FILE_ record. */
			if (df_rem_per.space_id() == space_id) {
				df.set_filepath(df_rem_per.filepath());
				df.open_read_only(false);
				return(true);
			}

			/* Since old MLOG records can use the same basename
			in multiple CREATE/DROP TABLE sequences, this ISL
			file could be pointing to a later version of this
			basename.ibd file which has a different space_id.
			Keep looking. */
		}
	}

	/* No ISL files were found in the default location. Use the location
	given in the redo log. */
	if (df.open_read_only(false) == DB_SUCCESS
	    && df.validate_for_recovery() == DB_SUCCESS
	    && df.space_id() == space_id) {
		return(true);
	}

	/* A datafile was not discovered for the filename given. */
	return(false);
}

bool fil_crypt_check(fil_space_crypt_t *crypt_data, const char *f_name)
  noexcept
{
  if (crypt_data->is_key_found())
    return true;
  sql_print_error("InnoDB: Encryption key is not found for %s", f_name);
  crypt_data->~fil_space_crypt_t();
  ut_free(crypt_data);
  return false;
}

/** Open an ibd tablespace and add it to the InnoDB data structures.
This is similar to fil_ibd_open() except that it is used while processing
the REDO log, so the data dictionary is not available and very little
validation is done. The tablespace name is extracred from the
dbname/tablename.ibd portion of the filename, which assumes that the file
is a file-per-table tablespace.  Any name will do for now.  General
tablespace names will be read from the dictionary after it has been
recovered.  The tablespace flags are read at this time from the first page
of the file in validate_for_recovery().
@param[in]	space_id	tablespace ID
@param[in]	filename	path/to/databasename/tablename.ibd
@param[out]	space		the tablespace, or NULL on error
@return status of the operation */
enum fil_load_status
fil_ibd_load(uint32_t space_id, const char *filename, fil_space_t *&space) noexcept
{
	/* If the a space is already in the file system cache with this
	space ID, then there is nothing to do. */
	mysql_mutex_lock(&fil_system.mutex);
	space = fil_space_get_by_id(space_id);
	mysql_mutex_unlock(&fil_system.mutex);

	if (space) {
		sql_print_information("InnoDB: Ignoring data file '%s'"
				      " with space ID %" PRIu32
				      ". Another data file called %s"
				      " exists"
				      " with the same space ID.",
				      filename, space->id,
				      UT_LIST_GET_FIRST(space->chain)->name);
		space = NULL;
		return FIL_LOAD_ID_CHANGED;
	}

	if (srv_operation == SRV_OPERATION_RESTORE) {
		/* Replace absolute DATA DIRECTORY file paths with
		short names relative to the backup directory. */
		const char* name = strrchr(filename, '/');
#ifdef _WIN32
		if (const char *last = strrchr(filename, '\\')) {
			if (last > name) {
				name = last;
			}
		}
#endif
		if (name) {
			while (--name > filename
#ifdef _WIN32
			       && *name != '\\'
#endif
			       && *name != '/');
			if (name > filename) {
				filename = name + 1;
			}
		}
	}

	Datafile	file;
	file.set_filepath(filename);
	file.open_read_only(false);

	if (!file.is_open()) {
		/* The file has been moved or it is a remote datafile. */
		if (!fil_ibd_discover(space_id, file)
		    || !file.is_open()) {
			return(FIL_LOAD_NOT_FOUND);
		}
	}

	os_offset_t	size;
	bool		deferred_space = false;

	/* Read and validate the first page of the tablespace.
	Assign a tablespace name based on the tablespace type. */
	switch (file.validate_for_recovery()) {
		os_offset_t	minimum_size;
	case DB_SUCCESS:
		deferred_space = file.m_defer;

		if (deferred_space) {
			goto tablespace_check;
		}

		if (file.space_id() != space_id) {
			return(FIL_LOAD_ID_CHANGED);
		}
tablespace_check:
		/* Get and test the file size. */
		size = os_file_get_size(file.handle());

		/* Every .ibd file is created >= 4 pages in size.
		Smaller files cannot be OK. */
		minimum_size = os_offset_t(FIL_IBD_FILE_INITIAL_SIZE)
			<< srv_page_size_shift;

		if (size == static_cast<os_offset_t>(-1)) {
			/* The following call prints an error message */
			os_file_get_last_error(true);

			ib::error() << "Could not measure the size of"
				" single-table tablespace file '"
				<< file.filepath() << "'";
		} else if (deferred_space) {
			return FIL_LOAD_DEFER;
		} else if (size < minimum_size) {
			ib::error() << "The size of tablespace file '"
				<< file.filepath() << "' is only " << size
				<< ", should be at least " << minimum_size
				<< "!";
		} else {
			/* Everything is fine so far. */
			break;
		}

		/* fall through */

	case DB_TABLESPACE_EXISTS:
		return(FIL_LOAD_INVALID);

	default:
		return(FIL_LOAD_NOT_FOUND);
	}

	ut_ad(space == NULL);

	/* Adjust the memory-based flags that would normally be set by
	dict_tf_to_fsp_flags(). In recovery, we have no data dictionary. */
	uint32_t flags = file.flags();
	if (fil_space_t::is_compressed(flags)) {
		flags |= page_zip_level
			<< FSP_FLAGS_MEM_COMPRESSION_LEVEL;
	}

	const byte* first_page = file.get_first_page();
	fil_space_crypt_t* crypt_data = first_page
		? fil_space_read_crypt_data(fil_space_t::zip_size(flags),
					    first_page)
		: NULL;

	if (crypt_data && !fil_crypt_check(crypt_data, filename)) {
		return FIL_LOAD_INVALID;
	}

	mysql_mutex_lock(&fil_system.mutex);

	space = fil_space_t::create(uint32_t(space_id), flags, false,
				    crypt_data);

	if (space == NULL) {
		mysql_mutex_unlock(&fil_system.mutex);
		return(FIL_LOAD_INVALID);
	}

	ut_ad(space->id == file.space_id());
	ut_ad(space->id == space_id);

	/* We do not use the size information we have about the file, because
	the rounding formula for extents and pages is somewhat complex; we
	let fil_node_open() do that task. */

	space->add(file.filepath(), OS_FILE_CLOSED, 0, false, false);
	mysql_mutex_unlock(&fil_system.mutex);

	return(FIL_LOAD_OK);
}

/** Try to adjust FSP_SPACE_FLAGS if they differ from the expectations.
(Typically when upgrading from MariaDB 10.1.0..10.1.20.)
@param[in,out]	space		tablespace
@param[in]	flags		desired tablespace flags */
void fsp_flags_try_adjust(fil_space_t *space, uint32_t flags)
{
	ut_ad(!srv_read_only_mode);
	ut_ad(fil_space_t::is_valid_flags(flags, space->id));
	ut_ad(!space->is_being_imported());
	ut_ad(!space->is_temporary());
	if (space->full_crc32() || fil_space_t::full_crc32(flags)) {
		return;
	}
	if (!space->size || !space->get_size()) {
		return;
	}
	mtr_t	mtr;
	mtr.start();
	if (buf_block_t* b = buf_page_get(
		    page_id_t(space->id, 0), space->zip_size(),
		    RW_X_LATCH, &mtr)) {
		uint32_t f = fsp_header_get_flags(b->page.frame);
		if (fil_space_t::full_crc32(f)) {
			goto func_exit;
		}
		if (fil_space_t::is_flags_equal(f, flags)) {
			goto func_exit;
		}
		/* Suppress the message if only the DATA_DIR flag to differs. */
		if ((f ^ flags) & ~(1U << FSP_FLAGS_POS_RESERVED)) {
			ib::warn()
				<< "adjusting FSP_SPACE_FLAGS of file '"
				<< UT_LIST_GET_FIRST(space->chain)->name
				<< "' from " << ib::hex(f)
				<< " to " << ib::hex(flags);
		}
		mtr.set_named_space(space);
		mtr.write<4,mtr_t::FORCED>(*b,
					   FSP_HEADER_OFFSET + FSP_SPACE_FLAGS
					   + b->page.frame, flags);
	}
func_exit:
	mtr.commit();
}

/** Determine if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id		Tablespace ID
@param[in]	table_flags	table flags
@return the tablespace
@retval	NULL	if no matching tablespace exists in the memory cache */
fil_space_t *fil_space_for_table_exists_in_mem(uint32_t id,
                                               uint32_t table_flags) noexcept
{
	const uint32_t expected_flags = dict_tf_to_fsp_flags(table_flags);

	mysql_mutex_lock(&fil_system.mutex);
	if (fil_space_t* space = fil_space_get_by_id(id)) {
		uint32_t tf = expected_flags & ~FSP_FLAGS_MEM_MASK;
		uint32_t sf = space->flags & ~FSP_FLAGS_MEM_MASK;

		if (!fil_space_t::is_flags_equal(tf, sf)
		    && !fil_space_t::is_flags_equal(sf, tf)) {
			goto func_exit;
		}

		/* Adjust the flags that are in FSP_FLAGS_MEM_MASK.
		FSP_SPACE_FLAGS will not be written back here. */
		space->flags = (space->flags & ~FSP_FLAGS_MEM_MASK)
			| (expected_flags & FSP_FLAGS_MEM_MASK);
		mysql_mutex_unlock(&fil_system.mutex);
		if (!srv_read_only_mode) {
			fsp_flags_try_adjust(space, expected_flags
					     & ~FSP_FLAGS_MEM_MASK);
		}
		return space;
	}

func_exit:
	mysql_mutex_unlock(&fil_system.mutex);
	return NULL;
}

/*============================ FILE I/O ================================*/

/** Report information about an invalid page access. */
ATTRIBUTE_COLD
static void fil_invalid_page_access_msg(const char *name,
                                        os_offset_t offset, ulint len,
                                        bool is_read) noexcept
{
  sql_print_error("%s %zu bytes at " UINT64PF
                  " outside the bounds of the file: %s",
                  is_read
                  ? "InnoDB: Trying to read"
                  : "[FATAL] InnoDB: Trying to write", len, offset, name);
  if (!is_read)
    abort();
}

/** Update the data structures on write completion */
void fil_space_t::complete_write() noexcept
{
  mysql_mutex_assert_not_owner(&fil_system.mutex);

  if (!is_temporary() &&
      fil_system.use_unflushed_spaces() && set_needs_flush())
  {
    mysql_mutex_lock(&fil_system.mutex);
    if (!is_in_unflushed_spaces)
    {
      is_in_unflushed_spaces= true;
      fil_system.unflushed_spaces.push_front(*this);
    }
    mysql_mutex_unlock(&fil_system.mutex);
  }
}

/** Read or write data.
@param type     I/O context
@param offset   offset in bytes
@param len      number of bytes
@param buf      the data to be read or written
@param bpage    buffer block (for type.is_async() completion callback)
@return status and file descriptor */
fil_io_t fil_space_t::io(const IORequest &type, os_offset_t offset, size_t len,
                         void *buf, buf_page_t *bpage) noexcept
{
	ut_ad(referenced());
	ut_ad(offset % UNIV_ZIP_SIZE_MIN == 0);
	ut_ad(len % 512 == 0); /* page_compressed */
	ut_ad(fil_validate_skip());
	ut_ad(type.is_read() || type.is_write());
	ut_ad(type.type != IORequest::DBLWR_BATCH);

	if (type.is_read()) {
		srv_stats.data_read.add(len);
	} else {
		ut_ad(!srv_read_only_mode || this == fil_system.temp_space);
		srv_stats.data_written.add(len);
	}

	fil_node_t* node= UT_LIST_GET_FIRST(chain);
	ut_ad(node);
	ulint p = static_cast<ulint>(offset >> srv_page_size_shift);
	dberr_t err;

	if (type.type == IORequest::READ_ASYNC && is_stopping()) {
		err = DB_TABLESPACE_DELETED;
		node = nullptr;
		goto release;
	}

	DBUG_EXECUTE_IF("intermittent_recovery_failure",
			if (type.is_read() && !(~my_timer_cycles() & 0x3ff0))
			goto io_error;);

	DBUG_EXECUTE_IF("intermittent_read_failure",
			if (srv_was_started && type.is_read() &&
			    !(~my_timer_cycles() & 0x3ff0)) goto io_error;);

	if (UNIV_LIKELY_NULL(UT_LIST_GET_NEXT(chain, node))) {
		ut_ad(this == fil_system.sys_space
		      || this == fil_system.temp_space);
		ut_ad(!(offset & ((1 << srv_page_size_shift) - 1)));

		while (node->size <= p) {
			p -= node->size;
			node = UT_LIST_GET_NEXT(chain, node);
			if (!node) {
fail:
				if (type.type != IORequest::READ_ASYNC) {
					fil_invalid_page_access_msg(
						node->name,
						offset, len,
						type.is_read());
				}
#ifndef DBUG_OFF
io_error:
#endif
				set_corrupted();
				err = DB_CORRUPTION;
				node = nullptr;
				goto release;
			}
		}

		offset = os_offset_t{p} << srv_page_size_shift;
	}

	if (UNIV_UNLIKELY(node->size <= p)) {
		goto fail;
	}

	if (type.type == IORequest::PUNCH_RANGE) {
		err = os_file_punch_hole(node->handle, offset, len);
		/* Punch hole is not supported, make space not to
		support punch hole */
		if (UNIV_UNLIKELY(err == DB_IO_NO_PUNCH_HOLE)) {
			node->punch_hole = false;
			err = DB_SUCCESS;
		}
		goto release_sync_write;
	} else {
		/* Queue the aio request */
		err = os_aio(IORequest{bpage, type.slot, node, type.type},
			     buf, offset, len);
	}

	if (!type.is_async()) {
		if (type.is_write()) {
release_sync_write:
			complete_write();
release:
			release();
			goto func_exit;
		}
		ut_ad(fil_validate_skip());
	}
	if (err != DB_SUCCESS) {
		goto release;
	}
func_exit:
	return {err, node};
}

#include <tpool.h>

void IORequest::write_complete(int io_error) const noexcept
{
  ut_ad(fil_validate_skip());
  ut_ad(node);
  fil_space_t *space= node->space;
  ut_ad(is_write());

  if (!bpage)
  {
    ut_ad(!srv_read_only_mode);
    if (type == IORequest::DBLWR_BATCH)
    {
      buf_dblwr.flush_buffered_writes_completed(*this);
      /* Above, we already invoked os_file_flush() on the
      doublewrite buffer if needed. */
      goto func_exit;
    }
    else
      ut_ad(type == IORequest::WRITE_ASYNC);
  }
  else
    buf_page_write_complete(*this, io_error);

  space->complete_write();
 func_exit:
  space->release();
}

void IORequest::read_complete(int io_error) const noexcept
{
  ut_ad(fil_validate_skip());
  ut_ad(node);
  ut_ad(is_read());
  ut_ad(bpage);
  ut_d(auto s= bpage->state());
  ut_ad(s > buf_page_t::READ_FIX);
  ut_ad(s <= buf_page_t::WRITE_FIX);

  const page_id_t id(bpage->id());
  const bool in_recovery{recv_sys.recovery_on};

  if (UNIV_UNLIKELY(io_error != 0))
  {
    sql_print_error("InnoDB: Read error %d of page " UINT32PF " in file %s",
                    io_error, id.page_no(), node->name);
    recv_sys.free_corrupted_page(id, *node);
    buf_pool.corrupted_evict(bpage, buf_page_t::READ_FIX + 1);
  corrupted:
    if (in_recovery && !srv_force_recovery)
    {
      mysql_mutex_lock(&recv_sys.mutex);
      recv_sys.set_corrupt_fs();
      mysql_mutex_unlock(&recv_sys.mutex);
    }
  }
  else if (bpage->read_complete(*node, in_recovery))
    goto corrupted;
  else
    bpage->unfix();

  node->space->release();
}

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS. */
void fil_flush_file_spaces() noexcept
{
rescan:
  mysql_mutex_lock(&fil_system.mutex);

  for (fil_space_t &space : fil_system.unflushed_spaces)
  {
    if (space.needs_flush_not_stopping())
    {
      space.reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      space.flush_low();
      space.release();
      goto rescan;
    }
  }

  mysql_mutex_unlock(&fil_system.mutex);
}

/** Functor to validate the file node list of a tablespace. */
struct	Check {
	/** Total size of file nodes visited so far */
	ulint	size;
	/** Total number of open files visited so far */
	ulint	n_open;

	/** Constructor */
	Check() : size(0), n_open(0) {}

	/** Visit a file node
	@param[in]	elem	file node to visit */
	void	operator()(const fil_node_t* elem) noexcept
	{
		n_open += elem->is_open();
		size += elem->size;
	}

	/** Validate a tablespace.
	@param[in]	space	tablespace to validate
	@return		number of open file nodes */
	static ulint validate(const fil_space_t* space) noexcept
	{
		mysql_mutex_assert_owner(&fil_system.mutex);
		Check	check;
		ut_list_validate(space->chain, check);
		ut_a(space->size == check.size);

		switch (space->id) {
		case TRX_SYS_SPACE:
			ut_ad(fil_system.sys_space == NULL
			      || fil_system.sys_space == space);
			break;
		case SRV_TMP_SPACE_ID:
			ut_ad(fil_system.temp_space == NULL
			      || fil_system.temp_space == space);
			break;
		default:
			break;
		}

		return(check.n_open);
	}
};

/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */
bool fil_validate() noexcept
{
	ulint		n_open		= 0;

	mysql_mutex_lock(&fil_system.mutex);

	for (fil_space_t &space : fil_system.space_list) {
		n_open += Check::validate(&space);
	}

	ut_a(fil_system.n_open == n_open);

	mysql_mutex_unlock(&fil_system.mutex);

	return(true);
}

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables.
@param[in] ibd_filepath File path of the IBD tablespace */
void fil_delete_file(const char *ibd_filepath) noexcept
{
  ib::info() << "Deleting " << ibd_filepath;
  os_file_delete_if_exists(innodb_data_file_key, ibd_filepath, nullptr);

  if (char *cfg_filepath= fil_make_filepath(ibd_filepath,
					    fil_space_t::name_type{}, CFG,
					    false))
  {
    os_file_delete_if_exists(innodb_data_file_key, cfg_filepath, nullptr);
    ut_free(cfg_filepath);
  }
}

#ifdef UNIV_DEBUG
/** Check that a tablespace is valid for mtr_commit().
@param[in]	space	persistent tablespace that has been changed */
static
void fil_space_validate_for_mtr_commit(const fil_space_t *space) noexcept
{
	mysql_mutex_assert_not_owner(&fil_system.mutex);
	ut_ad(space != NULL);
	ut_ad(!is_predefined_tablespace(space->id));
	ut_ad(!space->is_being_imported());

	/* We are serving mtr_commit(). While there is an active
	mini-transaction, we should have !space->is_stopping(). This is
	guaranteed by meta-data locks or transactional locks. */
	ut_ad(!space->is_stopping() || space->referenced());
}
#endif /* UNIV_DEBUG */

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void fil_names_dirty(fil_space_t *space) noexcept
{
	ut_ad(log_sys.latch_have_wr());
	ut_ad(recv_recovery_is_on());
	ut_ad(log_sys.get_lsn() != 0);
	ut_ad(space->max_lsn == 0);
	ut_d(fil_space_validate_for_mtr_commit(space));

	fil_system.named_spaces.push_back(*space);
	space->max_lsn = log_sys.get_lsn();
}

/** Write a FILE_MODIFY record when a non-predefined persistent
tablespace was modified for the first time since fil_names_clear(). */
ATTRIBUTE_NOINLINE ATTRIBUTE_COLD void mtr_t::name_write() noexcept
{
  ut_ad(log_sys.latch_have_wr());
  ut_d(fil_space_validate_for_mtr_commit(m_user_space));
  ut_ad(!m_user_space->max_lsn);
  m_user_space->max_lsn= log_sys.get_lsn();

  fil_system.named_spaces.push_back(*m_user_space);
  ut_ad(UT_LIST_GET_LEN(m_user_space->chain) == 1);

  mtr_t mtr;
  mtr.start();
  fil_name_write(m_user_space->id,
                 UT_LIST_GET_FIRST(m_user_space->chain)->name,
                 &mtr);
  mtr.commit_files();
}

/** On a log checkpoint, reset fil_names_dirty_and_write() flags
and write out FILE_MODIFY if needed, and write FILE_CHECKPOINT.
@param lsn  checkpoint LSN
@return current LSN */
ATTRIBUTE_COLD lsn_t fil_names_clear(lsn_t lsn) noexcept
{
	mtr_t	mtr;

	ut_ad(log_sys.latch_have_wr());
	ut_ad(lsn);
	ut_ad(log_sys.is_latest());

	mtr.start();

	for (auto it = fil_system.named_spaces.begin();
	     it != fil_system.named_spaces.end(); ) {
		if (mtr.get_log_size() + strlen(it->chain.start->name)
		    >= recv_sys.MTR_SIZE_MAX - (3 + 5)) {
			/* Prevent log parse buffer overflow */
			mtr.commit_files();
			mtr.start();
		}

		auto next = std::next(it);

		ut_ad(it->max_lsn > 0);
		if (it->max_lsn < lsn) {
			/* The tablespace was last dirtied before the
			checkpoint LSN. Remove it from the list, so
			that if the tablespace is not going to be
			modified any more, subsequent checkpoints will
			avoid calling fil_names_write() on it. */
			it->max_lsn = 0;
			fil_system.named_spaces.erase(it);
		}

		/* max_lsn is the last LSN where fil_names_dirty_and_write()
		was called. If we kept track of "min_lsn" (the first LSN
		where max_lsn turned nonzero), we could avoid the
		fil_names_write() call if min_lsn > lsn. */
		ut_ad(UT_LIST_GET_LEN((*it).chain) == 1);
		fil_name_write((*it).id, UT_LIST_GET_FIRST((*it).chain)->name,
			       &mtr);
		it = next;
	}

	return mtr.commit_files(lsn);
}

/* Unit Tests */
#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
#define MF  fil_make_filepath
#define DISPLAY ib::info() << path
void
test_make_filepath()
{
	char* path;
	const char* long_path =
		"this/is/a/very/long/path/including/a/very/"
		"looooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooong"
		"/folder/name";
	path = MF("/this/is/a/path/with/a/filename", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", NULL, ISL, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", NULL, CFG, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.dat", NULL, IBD, false); DISPLAY;
	path = MF(NULL, "tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "tablespacename", IBD, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", ISL, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", CFG, false); DISPLAY;
	path = MF(NULL, "dbname\\tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "dbname\\tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("./this/is/a/path", "dbname/tablespacename.ibd", IBD, true); DISPLAY;
	path = MF("this\\is\\a\\path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("/this/is/a/path", "dbname\\tablespacename", IBD, true); DISPLAY;
	path = MF(long_path, NULL, IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, true); DISPLAY;
}
#endif /* UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */
/* @} */

/** Determine the block size of the data file.
@param[in]	space		tablespace
@param[in]	offset		page number
@return	block size */
ulint fil_space_get_block_size(const fil_space_t *space, unsigned offset)
  noexcept
{
	ulint block_size = 512;

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {
		block_size = node->block_size;
		if (node->size > offset) {
			ut_ad(node->size <= 0xFFFFFFFFU);
			break;
		}
		offset -= static_cast<unsigned>(node->size);
	}

	/* Currently supporting block size up to 4K,
	fall back to default if bigger requested. */
	if (block_size > 4096) {
		block_size = 512;
	}

	return block_size;
}

/** @return the tablespace name (databasename/tablename) */
fil_space_t::name_type fil_space_t::name() const noexcept
{
  switch (id) {
  case 0:
    return name_type{"innodb_system", 13};
  case SRV_TMP_SPACE_ID:
    return name_type{"innodb_temporary", 16};
  }

  if (!UT_LIST_GET_FIRST(chain) || srv_is_undo_tablespace(id))
    return name_type{};

  ut_ad(!is_temporary());
  ut_ad(UT_LIST_GET_LEN(chain) == 1);

  const char *path= UT_LIST_GET_FIRST(chain)->name;
  const char *sep= strchr(path, '/');
  ut_ad(sep);

  while (const char *next_sep= strchr(sep + 1, '/'))
    path= sep + 1, sep= next_sep;

#ifdef _WIN32
  if (const char *last_sep= strchr(path, '\\'))
    if (last_sep < sep)
      path= last_sep;
#endif

  size_t len= strlen(path);
  ut_ad(len > 4);
  len-= 4;
  ut_ad(!strcmp(&path[len], DOT_IBD));

  return name_type{path, len};
}

#ifdef UNIV_DEBUG

fil_space_t *fil_space_t::next_in_space_list() noexcept
{
  space_list_t::iterator it(this);
  auto end= fil_system.space_list.end();
  if (it == end)
    return nullptr;
  ++it;
  return it == end ? nullptr : &*it;
}

fil_space_t *fil_space_t::prev_in_space_list() noexcept
{
  space_list_t::iterator it(this);
  if (it == fil_system.space_list.begin())
    return nullptr;
  --it;
  return &*it;
}

fil_space_t *fil_space_t::next_in_unflushed_spaces() noexcept
{
  sized_ilist<fil_space_t, unflushed_spaces_tag_t>::iterator it(this);
  auto end= fil_system.unflushed_spaces.end();
  if (it == end)
    return nullptr;
  ++it;
  return it == end ? nullptr : &*it;
}

fil_space_t *fil_space_t::prev_in_unflushed_spaces() noexcept
{
  sized_ilist<fil_space_t, unflushed_spaces_tag_t>::iterator it(this);
  if (it == fil_system.unflushed_spaces.begin())
    return nullptr;
  --it;
  return &*it;
}

#endif
