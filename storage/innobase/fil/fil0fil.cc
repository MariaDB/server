/*****************************************************************************

Copyright (c) 1995, 2021, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2022, MariaDB Corporation.

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
#include "ibuf0ibuf.h"
#include "buf0flu.h"
#include "log.h"
#ifdef UNIV_LINUX
# include <sys/types.h>
# include <sys/sysmacros.h>
# include <dirent.h>
#endif

#include "lz4.h"
#include "lzo/lzo1x.h"
#include "lzma.h"
#include "bzlib.h"
#include "snappy-c.h"

/** Try to close a file to adhere to the innodb_open_files limit.
@param print_info   whether to diagnose why a file cannot be closed
@return whether a file was closed */
bool fil_space_t::try_to_close(bool print_info)
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  for (fil_space_t &space : fil_system.space_list)
  {
    switch (space.purpose) {
    case FIL_TYPE_TEMPORARY:
      continue;
    case FIL_TYPE_IMPORT:
      break;
    case FIL_TYPE_TABLESPACE:
      if (is_predefined_tablespace(space.id))
        continue;
    }

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

    if (const auto n= space.set_closing())
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
    return true;
  }

  return false;
}

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		tablespace identifier
@param[in]	old_path	old file name
@param[in]	new_path_in	new file name,
or NULL if it is located in the normal data directory
@return true if success */
static bool fil_rename_tablespace(uint32_t id, const char *old_path,
                                  const char *new_path_in);

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
static
bool
fil_validate_skip(void)
/*===================*/
{
	/** The fil_validate() call skip counter. */
	static Atomic_counter<uint32_t> fil_validate_count;

	/* We want to reduce the call frequency of the costly fil_validate()
	check in debug builds. */
	return (fil_validate_count++ % FIL_VALIDATE_SKIP) || fil_validate();
}
#endif /* UNIV_DEBUG */

/** Look up a tablespace.
@param tablespace identifier
@return tablespace
@retval nullptr if not found */
fil_space_t *fil_space_get_by_id(uint32_t id)
{
	fil_space_t*	space;

	ut_ad(fil_system.is_initialised());
	mysql_mutex_assert_owner(&fil_system.mutex);

	HASH_SEARCH(hash, &fil_system.spaces, id,
		    fil_space_t*, space,
		    ut_ad(space->magic_n == FIL_SPACE_MAGIC_N),
		    space->id == id);

	return(space);
}

/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
Normally, fil_space_t::get() should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or NULL if not found */
fil_space_t *fil_space_get(uint32_t id)
{
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}

/** Check if the compression algorithm is loaded
@param[in]	comp_algo ulint compression algorithm
@return whether the compression algorithm is loaded */
bool fil_comp_algo_loaded(ulint comp_algo)
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
			     uint32_t max_pages)
{
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

	mysql_mutex_lock(&fil_system.mutex);
	this->size += size;
	UT_LIST_ADD_LAST(chain, node);
	if (node->is_open()) {
		clear_closing();
		if (++fil_system.n_open >= srv_max_n_open_files) {
			reacquire();
			try_to_close(true);
			release();
		}
	}
	mysql_mutex_unlock(&fil_system.mutex);

	return node;
}

/** Open a tablespace file.
@param node  data file
@return whether the file was successfully opened */
static bool fil_node_open_file_low(fil_node_t *node)
{
  ut_ad(!node->is_open());
  ut_ad(node->space->is_closing());
  mysql_mutex_assert_owner(&fil_system.mutex);
  ulint type;
  static_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096, "compatibility");
  switch (FSP_FLAGS_GET_ZIP_SSIZE(node->space->flags)) {
  case 1:
  case 2:
    type= OS_DATA_FILE_NO_O_DIRECT;
    break;
  default:
    type= OS_DATA_FILE;
  }

  for (;;)
  {
    bool success;
    node->handle= os_file_create(innodb_data_file_key, node->name,
                                 node->is_raw_disk
                                 ? OS_FILE_OPEN_RAW | OS_FILE_ON_ERROR_NO_EXIT
                                 : OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
                                 OS_FILE_AIO, type,
                                 srv_read_only_mode, &success);
    if (success)
      break;

    /* The following call prints an error message */
    if (os_file_get_last_error(true) == EMFILE + 100 &&
        fil_space_t::try_to_close(true))
      continue;

    ib::warn() << "Cannot open '" << node->name << "'.";
    return false;
  }

  ulint comp_algo = node->space->get_compression_algo();
  bool comp_algo_invalid = false;

  if (node->size);
  else if (!node->read_page0() ||
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

  if (UNIV_LIKELY(!fil_system.freeze_space_list))
  {
    /* Move the file last in fil_system.space_list, so that
    fil_space_t::try_to_close() should close it as a last resort. */
    fil_system.space_list.erase(space_list_t::iterator(node->space));
    fil_system.space_list.push_back(*node->space);
  }

  fil_system.n_open++;
  return true;
}

/** Open a tablespace file.
@param node  data file
@return whether the file was successfully opened */
static bool fil_node_open_file(fil_node_t *node)
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(!node->is_open());
  ut_ad(!is_predefined_tablespace(node->space->id) ||
        srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_DELTA);
  ut_ad(node->space->purpose != FIL_TYPE_TEMPORARY);
  ut_ad(node->space->referenced());

  const auto old_time= fil_system.n_open_exceeded_time;

  for (ulint count= 0; fil_system.n_open >= srv_max_n_open_files; count++)
  {
    if (fil_space_t::try_to_close(count > 1))
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

  return fil_node_open_file_low(node);
}

/** Close the file handle. */
void fil_node_t::close()
{
  prepare_to_close_or_detach();

  /* printf("Closing file %s\n", name); */
  int ret= os_file_close(handle);
  ut_a(ret);
  handle= OS_FILE_CLOSED;
}

pfs_os_file_t fil_node_t::detach()
{
  prepare_to_close_or_detach();

  pfs_os_file_t result= handle;
  handle= OS_FILE_CLOSED;
  return result;
}

void fil_node_t::prepare_to_close_or_detach()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(space->is_ready_to_close() || srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_RESTORE_DELTA);
  ut_a(is_open());
  ut_a(!being_extended);
  ut_a(space->is_ready_to_close() || space->purpose == FIL_TYPE_TEMPORARY ||
       srv_fast_shutdown == 2 || !srv_was_started);

  ut_a(fil_system.n_open > 0);
  fil_system.n_open--;
}

/** Flush any writes cached by the file system. */
void fil_space_t::flush_low()
{
  mysql_mutex_assert_not_owner(&fil_system.mutex);

  uint32_t n= 1;
  while (!n_pending.compare_exchange_strong(n, n | NEEDS_FSYNC,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed))
  {
    ut_ad(n & PENDING);
    if (n & STOPPING)
      return;
    if (n & NEEDS_FSYNC)
      break;
  }

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

  clear_flush();
  fil_n_pending_tablespace_flushes--;
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
	bool*		success)
{
	mysql_mutex_assert_owner(&fil_system.mutex);
	ut_ad(UT_LIST_GET_LAST(space->chain) == node);
	ut_ad(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_ad(node->space == space);
	ut_ad(space->referenced() || space->is_being_truncated);

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

	os_has_said_disk_full = *success;
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
		ut_ad(space->purpose == FIL_TYPE_TABLESPACE
		      || space->purpose == FIL_TYPE_IMPORT);
		if (space->purpose == FIL_TYPE_TABLESPACE
		    && !space->is_being_truncated) {
			goto do_flush;
		}
		break;
	case SRV_TMP_SPACE_ID:
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY);
		srv_tmp_space.set_last_file_size(pages_in_MiB);
		break;
	}

	return false;
}

/** @return whether the file is usable for io() */
ATTRIBUTE_COLD bool fil_space_t::prepare(bool have_mutex)
{
  ut_ad(referenced());
  if (!have_mutex)
    mysql_mutex_lock(&fil_system.mutex);
  mysql_mutex_assert_owner(&fil_system.mutex);
  fil_node_t *node= UT_LIST_GET_LAST(chain);
  ut_ad(!id || purpose == FIL_TYPE_TEMPORARY ||
        node == UT_LIST_GET_FIRST(chain));

  const bool is_open= node && (node->is_open() || fil_node_open_file(node));

  if (!is_open)
    release();
  else if (auto desired_size= recv_size)
  {
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

  if (!have_mutex)
    mysql_mutex_unlock(&fil_system.mutex);
  return is_open;
}

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool fil_space_extend(fil_space_t *space, uint32_t size)
{
  ut_ad(!srv_read_only_mode || space->purpose == FIL_TYPE_TEMPORARY);
  bool success= false;
  const bool acquired= space->acquire();
  mysql_mutex_lock(&fil_system.mutex);
  if (acquired || space->is_being_truncated)
  {
    while (fil_space_extend_must_retry(space, UT_LIST_GET_LAST(space->chain),
                                       size, &success))
      mysql_mutex_lock(&fil_system.mutex);
  }
  mysql_mutex_unlock(&fil_system.mutex);
  if (acquired)
    space->release();
  return success;
}

/** Prepare to free a file from fil_system. */
inline pfs_os_file_t fil_node_t::close_to_free(bool detach_handle)
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
      ut_ad(srv_file_flush_method != SRV_O_DIRECT_NO_FSYNC);
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
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  HASH_DELETE(fil_space_t, hash, &spaces, space->id, space);

  if (space->is_in_unflushed_spaces)
  {
    ut_ad(srv_file_flush_method != SRV_O_DIRECT_NO_FSYNC);
    space->is_in_unflushed_spaces= false;
    unflushed_spaces.remove(*space);
  }

  if (space->is_in_default_encrypt)
  {
    space->is_in_default_encrypt= false;
    default_encrypt_tables.remove(*space);
  }
  space_list.erase(space_list_t::iterator(space));
  if (space == sys_space)
    sys_space= nullptr;
  else if (space == temp_space)
    temp_space= nullptr;

  ut_a(space->magic_n == FIL_SPACE_MAGIC_N);

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
static
void
fil_space_free_low(
	fil_space_t*	space)
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
bool fil_space_free(uint32_t id, bool x_latched)
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
			log_sys.latch.wr_lock(SRW_LOCK_CALL);

			if (space->max_lsn) {
				ut_d(space->max_lsn = 0);
				fil_system.named_spaces.remove(*space);
			}

			log_sys.latch.wr_unlock();
		} else {
#ifndef SUX_LOCK_GENERIC
			ut_ad(log_sys.latch.is_write_locked());
#endif
			if (space->max_lsn) {
				ut_d(space->max_lsn = 0);
				fil_system.named_spaces.remove(*space);
			}
		}

		fil_space_free_low(space);
	}

	return(space != NULL);
}

/** Create a tablespace in fil_system.
@param name       tablespace name
@param id         tablespace identifier
@param flags      tablespace flags
@param purpose    tablespace purpose
@param crypt_data encryption information
@param mode       encryption mode
@return pointer to created tablespace, to be filled in with add()
@retval nullptr on failure (such as when the same tablespace exists) */
fil_space_t *fil_space_t::create(uint32_t id, uint32_t flags,
                                 fil_type_t purpose,
				 fil_space_crypt_t *crypt_data,
				 fil_encryption_t mode)
{
	fil_space_t*	space;

	ut_ad(fil_system.is_initialised());
	ut_ad(fil_space_t::is_valid_flags(flags & ~FSP_FLAGS_MEM_MASK, id));
	ut_ad(srv_page_size == UNIV_PAGE_SIZE_ORIG || flags != 0);

	DBUG_EXECUTE_IF("fil_space_create_failure", return(NULL););

	/* FIXME: if calloc() is defined as an inline function that calls
	memset() or bzero(), then GCC 6 -flifetime-dse can optimize it away */
	space= new (ut_zalloc_nokey(sizeof(*space))) fil_space_t;

	space->id = id;

	UT_LIST_INIT(space->chain, &fil_node_t::chain);

	space->purpose = purpose;
	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;
	space->crypt_data = crypt_data;
	space->n_pending.store(CLOSING, std::memory_order_relaxed);

	DBUG_LOG("tablespace", "Created metadata for " << id);
	if (crypt_data) {
		DBUG_LOG("crypt",
			 "Tablespace " << id
			 << " encryption " << crypt_data->encryption
			 << " key id " << crypt_data->key_id
			 << ":" << fil_crypt_get_mode(crypt_data)
			 << " " << fil_crypt_get_type(crypt_data));
	}

	space->latch.SRW_LOCK_INIT(fil_space_latch_key);

	mysql_mutex_lock(&fil_system.mutex);

	if (const fil_space_t *old_space = fil_space_get_by_id(id)) {
		ib::error() << "Trying to add tablespace with id " << id
			    << " to the cache, but tablespace '"
			    << (old_space->chain.start
				? old_space->chain.start->name
				: "")
			    << "' already exists in the cache!";
		mysql_mutex_unlock(&fil_system.mutex);
		space->~fil_space_t();
		ut_free(space);
		return(NULL);
	}

	HASH_INSERT(fil_space_t, hash, &fil_system.spaces, id, space);

	fil_system.space_list.push_back(*space);

	switch (id) {
	case 0:
		ut_ad(!fil_system.sys_space);
		fil_system.sys_space = space;
		break;
	case SRV_TMP_SPACE_ID:
		ut_ad(!fil_system.temp_space);
		fil_system.temp_space = space;
		break;
	default:
		ut_ad(purpose != FIL_TYPE_TEMPORARY);
		if (UNIV_LIKELY(id <= fil_system.max_assigned_id)) {
			break;
		}
		if (UNIV_UNLIKELY(srv_operation == SRV_OPERATION_BACKUP)) {
			break;
		}
		if (!fil_system.space_id_reuse_warned) {
			ib::warn() << "Allocated tablespace ID " << id
				<< ", old maximum was "
				<< fil_system.max_assigned_id;
		}

		fil_system.max_assigned_id = id;
	}

	const bool rotate = purpose == FIL_TYPE_TABLESPACE
		&& (mode == FIL_ENCRYPTION_ON || mode == FIL_ENCRYPTION_OFF
		    || srv_encrypt_tables)
		&& fil_crypt_must_default_encrypt();

	if (rotate) {
		fil_system.default_encrypt_tables.push_back(*space);
		space->is_in_default_encrypt = true;
	}

	mysql_mutex_unlock(&fil_system.mutex);

	if (rotate && srv_n_fil_crypt_threads_started) {
		fil_crypt_threads_signal();
	}

	return(space);
}

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */
bool fil_assign_new_space_id(uint32_t *space_id)
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
@return whether the page was found valid */
bool fil_space_t::read_page0()
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
  const bool ok= node->is_open() || fil_node_open_file(node);
  release();
  return ok;
}

/** Look up a tablespace and ensure that its first page has been validated. */
static fil_space_t *fil_space_get_space(uint32_t id)
{
  if (fil_space_t *space= fil_space_get_by_id(id))
    if (space->read_page0())
      return space;
  return nullptr;
}

void fil_space_set_recv_size_and_flags(uint32_t id, uint32_t size,
                                       uint32_t flags)
{
  ut_ad(id < SRV_SPACE_ID_UPPER_BOUND);
  mysql_mutex_lock(&fil_system.mutex);
  if (fil_space_t *space= fil_space_get_space(id))
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

  mysql_mutex_lock(&fil_system.mutex);

  for (fil_node_t *node= UT_LIST_GET_FIRST(chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
  {
    if (!node->is_open() && !fil_node_open_file_low(node))
    {
err_exit:
      success= false;
      break;
    }

    if (create_new_db)
    {
      node->find_metadata(node->handle);
      continue;
    }
    if (skip_read)
    {
      size+= node->size;
      continue;
    }

    if (!node->read_page0())
    {
      fil_system.n_open--;
      os_file_close(node->handle);
      node->handle= OS_FILE_CLOSED;
      goto err_exit;
    }

    skip_read= true;
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
	ut_ad(!spaces.array);

	m_initialised = true;

	compile_time_assert(!(UNIV_PAGE_SIZE_MAX % FSP_EXTENT_SIZE_MAX));
	compile_time_assert(!(UNIV_PAGE_SIZE_MIN % FSP_EXTENT_SIZE_MIN));

	ut_ad(hash_size > 0);

	mysql_mutex_init(fil_system_mutex_key, &mutex, nullptr);

	spaces.create(hash_size);

	fil_space_crypt_init();
#ifdef UNIV_LINUX
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

void fil_system_t::close()
{
  ut_ad(this == &fil_system);
  ut_a(unflushed_spaces.empty());
  ut_a(space_list.empty());
  ut_ad(!sys_space);
  ut_ad(!temp_space);

  if (is_initialised())
  {
    m_initialised= false;
    spaces.free();
    mysql_mutex_destroy(&mutex);
    fil_space_crypt_cleanup();
  }

  ut_ad(!spaces.array);

#ifdef UNIV_LINUX
  ssd.clear();
  ssd.shrink_to_fit();
#endif /* UNIV_LINUX */
}

/** Extend all open data files to the recovered size */
ATTRIBUTE_COLD void fil_system_t::extend_to_recv_size()
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

/** Close all tablespace files at shutdown */
void fil_space_t::close_all()
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
      {
      next:
        continue;
      }

      for (ulint count= 10000; count--;)
      {
        if (!space.set_closing())
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

      ib::error() << "File '" << node->name << "' has " << space.referenced()
                  << " operations";
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
void fil_set_max_space_id_if_bigger(uint32_t max_id)
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
fil_space_t *fil_space_t::get(uint32_t id)
{
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  const uint32_t n= space ? space->acquire_low() : 0;
  mysql_mutex_unlock(&fil_system.mutex);

  if (n & STOPPING)
    space= nullptr;
  else if ((n & CLOSING) && !space->prepare())
    space= nullptr;

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

  flag_modified();
  if (m_log_mode != MTR_LOG_ALL)
    return;
  m_last= nullptr;

  const size_t len= strlen(path);
  const size_t new_len= type == FILE_RENAME ? 1 + strlen(new_path) : 0;
  ut_ad(len > 0);
  byte *const log_ptr= m_log.open(1 + 3/*length*/ + 5/*space_id*/ +
                                  1/*page_no=0*/);
  byte *end= log_ptr + 1;
  end= mlog_encode_varint(end, space_id);
  *end++= 0;
  if (UNIV_LIKELY(end + len + new_len >= &log_ptr[16]))
  {
    *log_ptr= type;
    size_t total_len= len + new_len + end - log_ptr - 15;
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
    *log_ptr= static_cast<byte>(type | (end + len + new_len - &log_ptr[1]));
    ut_ad(*log_ptr & 15);
  }

  m_log.close(end);

  if (type == FILE_RENAME)
  {
    ut_ad(strchr(new_path, '/'));
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len + 1));
    m_log.push(reinterpret_cast<const byte*>(new_path), uint32_t(new_len - 1));
  }
  else
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len));
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming
@param[in,out]	mtr		mini-transaction */
static void fil_name_write_rename_low(uint32_t space_id, const char *old_name,
                                      const char *new_name, mtr_t *mtr)
{
  ut_ad(!is_predefined_tablespace(space_id));
  mtr->log_file_op(FILE_RENAME, space_id, old_name, new_name);
}

static void fil_name_commit_durable(mtr_t *mtr)
{
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  auto lsn= mtr->commit_files();
  log_sys.latch.wr_unlock();
  mtr->flag_wr_unlock();
  log_write_up_to(lsn, true);
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming */
static void fil_name_write_rename(uint32_t space_id,
				  const char *old_name, const char* new_name)
{
  mtr_t mtr;
  mtr.start();
  fil_name_write_rename_low(space_id, old_name, new_name, &mtr);
  fil_name_commit_durable(&mtr);
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

fil_space_t *fil_space_t::check_pending_operations(uint32_t id)
{
  ut_a(!is_system_tablespace(id));
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);

  if (!space)
  {
    mysql_mutex_unlock(&fil_system.mutex);
    return nullptr;
  }

  if (space->pending() & STOPPING)
  {
being_deleted:
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
  else
  {
    if (space->crypt_data)
    {
      space->reacquire();
      mysql_mutex_unlock(&fil_system.mutex);
      fil_space_crypt_close_tablespace(space);
      mysql_mutex_lock(&fil_system.mutex);
      space->release();
    }
    if (space->set_stopping_check())
      goto being_deleted;
  }

  mysql_mutex_unlock(&fil_system.mutex);

  for (ulint count= 0;; count++)
  {
    const unsigned pending= space->referenced();
    if (!pending)
      return space;
    /* Issue a warning every 10.24 seconds, starting after 2.56 seconds */
    if ((count & 511) == 128)
      sql_print_warning("InnoDB: Trying to delete tablespace '%s' "
                        "but there are %u pending operations",
                        space->chain.start->name, id);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

/** Close a single-table tablespace on failed IMPORT TABLESPACE.
The tablespace must be cached in the memory cache.
Free all pages used by the tablespace. */
void fil_close_tablespace(uint32_t id)
{
	ut_ad(!is_system_tablespace(id));
	fil_space_t* space = fil_space_t::check_pending_operations(id);
	if (!space) {
		return;
	}

	space->x_lock();

	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have invoked space->set_stopping(), readahead
	can no longer read more pages of this tablespace to buf_pool.
	Thus we can clean the tablespace out of buf_pool
	completely and permanently. */
	while (buf_flush_list_space(space));
	ut_ad(space->is_stopping());

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	if (char* cfg_name = fil_make_filepath(space->chain.start->name,
					       fil_space_t::name_type{},
					       CFG, false)) {
		os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
		ut_free(cfg_name);
	}

	/* If the free is successful, the wrlock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free(id, true)) {
		space->x_unlock();
	}
}

/** Delete a tablespace and associated .ibd file.
@param id    tablespace identifier
@return detached file handle (to be closed by the caller)
@return	OS_FILE_CLOSED if no file existed */
pfs_os_file_t fil_delete_tablespace(uint32_t id)
{
  ut_ad(!is_system_tablespace(id));
  pfs_os_file_t handle= OS_FILE_CLOSED;
  if (fil_space_t *space= fil_space_t::check_pending_operations(id))
  {
    /* Before deleting the file(s), persistently write a log record. */
    mtr_t mtr;
    mtr.start();
    mtr.log_file_op(FILE_DELETE, id, space->chain.start->name);
    fil_name_commit_durable(&mtr);

    /* Remove any additional files. */
    if (char *cfg_name= fil_make_filepath(space->chain.start->name,
					  fil_space_t::name_type{}, CFG,
					  false))
    {
      os_file_delete_if_exists(innodb_data_file_key, cfg_name, nullptr);
      ut_free(cfg_name);
    }
    if (FSP_FLAGS_HAS_DATA_DIR(space->flags))
      RemoteDatafile::delete_link_file(space->name());

    /* Remove the directory entry. The file will actually be deleted
    when our caller closes the handle. */
    os_file_delete(innodb_data_file_key, space->chain.start->name);

    mysql_mutex_lock(&fil_system.mutex);
    /* Sanity checks after reacquiring fil_system.mutex */
    ut_ad(space == fil_space_get_by_id(id));
    ut_ad(!space->referenced());
    ut_ad(space->is_stopping());
    ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
    /* Detach the file handle. */
    handle= fil_system.detach(space, true);
    mysql_mutex_unlock(&fil_system.mutex);

    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    if (space->max_lsn)
    {
      ut_d(space->max_lsn = 0);
      fil_system.named_spaces.remove(*space);
    }
    log_sys.latch.wr_unlock();

    fil_space_free_low(space);
  }

  ibuf_delete_for_discarded_space(id);
  return handle;
}

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the directory path or the full path and filename.
@param[in] name {} if path is full, or Table/Tablespace name
@param[in] ext the file extension to use
@param[in] trim_name true if the last name on the path should be trimmed.
@return own: file name */
char* fil_make_filepath(const char *path, const fil_space_t::name_type &name,
                        ib_extention ext, bool trim_name)
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is NULL, we can use the default path,
	but there needs to be a name. */
	ut_ad(path || name.data());

	/* If we are going to strip a name off the path, there better be a
	path and a new name to put back on. */
	ut_ad(!trim_name || (path && name.data()));

	if (path == NULL) {
		path = fil_path_to_mysql_datadir;
	}

	ulint	len		= 0;	/* current length */
	ulint	path_len	= strlen(path);
	const char* suffix	= dot_ext[ext];
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
                        ib_extention suffix, bool strip_name)
{
  return fil_make_filepath(path, {name.m_name, strlen(name.m_name)},
                           suffix, strip_name);
}

dberr_t fil_space_t::rename(const char *path, bool log, bool replace)
{
  ut_ad(UT_LIST_GET_LEN(chain) == 1);
  ut_ad(!is_system_tablespace(id));

  const char *old_path= chain.start->name;

  if (!strcmp(path, old_path))
    return DB_SUCCESS;

  if (log)
  {
    bool exists= false;
    os_file_type_t ftype;

    if (os_file_status(old_path, &exists, &ftype) && !exists)
    {
      ib::error() << "Cannot rename '" << old_path << "' to '" << path
                  << "' because the source file does not exist.";
      return DB_TABLESPACE_NOT_FOUND;
    }

    exists= false;
    if (replace);
    else if (!os_file_status(path, &exists, &ftype) || exists)
    {
      ib::error() << "Cannot rename '" << old_path << "' to '" << path
                  << "' because the target file exists.";
      return DB_TABLESPACE_EXISTS;
    }

    fil_name_write_rename(id, old_path, path);
  }

  return fil_rename_tablespace(id, old_path, path) ? DB_SUCCESS : DB_ERROR;
}

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		tablespace identifier
@param[in]	old_path	old file name
@param[in]	new_path_in	new file name,
or NULL if it is located in the normal data directory
@return true if success */
static bool fil_rename_tablespace(uint32_t id, const char *old_path,
                                  const char *new_path_in)
{
	fil_space_t*	space;
	fil_node_t*	node;
	ut_a(id != 0);

	mysql_mutex_lock(&fil_system.mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib::error() << "Cannot find space id " << id
			<< " in the tablespace memory cache, though the file '"
			<< old_path
			<< "' in a rename operation should have that id.";
		mysql_mutex_unlock(&fil_system.mutex);
		return(false);
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	node = UT_LIST_GET_FIRST(space->chain);
	space->reacquire();

	mysql_mutex_unlock(&fil_system.mutex);

	char*	new_file_name = mem_strdup(new_path_in);
	char*	old_file_name = node->name;

	ut_ad(strchr(old_file_name, '/'));
	ut_ad(strchr(new_file_name, '/'));

	if (!recv_recovery_is_on()) {
		log_sys.latch.wr_lock(SRW_LOCK_CALL);
	}

	/* log_sys.latch is above fil_system.mutex in the latching order */
#ifndef SUX_LOCK_GENERIC
	ut_ad(log_sys.latch.is_write_locked() ||
	      srv_operation == SRV_OPERATION_RESTORE_DELTA);
#endif
	mysql_mutex_lock(&fil_system.mutex);
	space->release();
	ut_ad(node->name == old_file_name);
	bool success;
	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
			goto skip_second_rename; );
	success = os_file_rename(innodb_data_file_key,
				 old_file_name,
				 new_file_name);
	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
skip_second_rename:
                       success = false; );

	ut_ad(node->name == old_file_name);

	if (success) {
		node->name = new_file_name;
	} else {
		old_file_name = new_file_name;
	}

	if (!recv_recovery_is_on()) {
		log_sys.latch.wr_unlock();
	}

	mysql_mutex_unlock(&fil_system.mutex);

	ut_free(old_file_name);

	return(success);
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
	dberr_t*	err)
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
	fil_name_commit_durable(&mtr);

	ulint type;
	static_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096,
		      "compatibility");
	switch (FSP_FLAGS_GET_ZIP_SSIZE(flags)) {
	case 1:
	case 2:
		type = OS_DATA_FILE_NO_O_DIRECT;
		break;
	default:
		type = OS_DATA_FILE;
	}

	file = os_file_create(
		innodb_data_file_key, path,
		OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT,
		OS_FILE_AIO, type, srv_read_only_mode, &success);

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

	if (fil_space_t* space = fil_space_t::create(space_id, flags,
						     FIL_TYPE_TABLESPACE,
						     crypt_data, mode)) {
		fil_node_t* node = space->add(path, file, size, false, true);
		IF_WIN(node->find_metadata(), node->find_metadata(file, true));
		mtr.start();
		mtr.set_named_space(space);
		fsp_header_init(space, size, &mtr);
		mtr.commit();
		*err = DB_SUCCESS;
		return space;
	}

	if (space_name.data()) {
		RemoteDatafile::delete_link_file(space_name);
	}

	*err = DB_ERROR;
	goto err_exit;
}

/** Try to open a single-table tablespace and optionally check that the
space id in it is correct. If this does not succeed, print an error message
to the .err log. This function is used to open a tablespace when we start
mysqld after the dictionary has been booted, and also in IMPORT TABLESPACE.

NOTE that we assume this operation is used either at the database startup
or under the protection of dict_sys.latch, so that two users cannot
race here. This operation does not leave the file associated with the
tablespace open, but closes it after we have looked at the space id in it.

If the validate boolean is set, we read the first page of the file and
check that the space id in the file is what we expect. We assume that
this function runs much faster if no check is made, since accessing the
file inode probably is much faster (the OS caches them) than accessing
the first page of the file.  This boolean may be initially false, but if
a remote tablespace is found it will be changed to true.

If the fix_dict boolean is set, then it is safe to use an internal SQL
statement to update the dictionary tables if they are incorrect.

@param[in]	validate	0=maybe missing, 1=do not validate, 2=validate
@param[in]	purpose		FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in]	id		tablespace ID
@param[in]	flags		expected FSP_SPACE_FLAGS
@param[in]	name		table name
If file-per-table, it is the table name in the databasename/tablename format
@param[in]	path_in		expected filepath, usually read from dictionary
@param[out]	err		DB_SUCCESS or error code
@return	tablespace
@retval	NULL	if the tablespace could not be opened */
fil_space_t*
fil_ibd_open(
	unsigned		validate,
	fil_type_t		purpose,
	uint32_t		id,
	uint32_t		flags,
	fil_space_t::name_type	name,
	const char*		path_in,
	dberr_t*		err)
{
	mysql_mutex_lock(&fil_system.mutex);
	fil_space_t* space = fil_space_get_by_id(id);
	mysql_mutex_unlock(&fil_system.mutex);
	if (space) {
		if (validate > 1 && !srv_read_only_mode) {
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
	} else if (validate > 1) {
		must_validate = true;
	}

	/* Always look for a file at the default location. But don't log
	an error if the tablespace is already open in remote or dict. */
	ut_a(df_default.filepath());

	/* Mariabackup will not copy files whose names start with
	#sql-. We will suppress messages about such files missing on
	the first server startup. The tables ought to be dropped by
	drop_garbage_tables_after_restore() a little later. */

	const bool strict = validate && !tablespaces_found
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

		os_file_get_last_error(true);
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

	space = fil_space_t::create(id, flags, purpose, crypt_data);
	if (!space) {
		goto error;
	}

	/* We do not measure the size of the file, that is why
	we pass the 0 below */

	space->add(
		df_remote.is_open() ? df_remote.filepath() :
		df_default.filepath(), OS_FILE_CLOSED, 0, false, true);

	if (must_validate && !srv_read_only_mode) {
		df_remote.close();
		df_default.close();
		if (space->acquire()) {
			if (purpose != FIL_TYPE_IMPORT) {
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
fil_ibd_load(uint32_t space_id, const char *filename, fil_space_t *&space)
{
	/* If the a space is already in the file system cache with this
	space ID, then there is nothing to do. */
	mysql_mutex_lock(&fil_system.mutex);
	space = fil_space_get_by_id(space_id);
	mysql_mutex_unlock(&fil_system.mutex);

	if (space) {
		/* Compare the filename we are trying to open with the
		filename from the first node of the tablespace we opened
		previously. Fail if it is different. */
		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		if (0 != strcmp(innobase_basename(filename),
				innobase_basename(node->name))) {
			ib::info()
				<< "Ignoring data file '" << filename
				<< "' with space ID " << space->id
				<< ". Another data file called " << node->name
				<< " exists with the same space ID.";
			space = NULL;
			return(FIL_LOAD_ID_CHANGED);
		}
		return(FIL_LOAD_OK);
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

	if (crypt_data && !crypt_data->is_key_found()) {
		crypt_data->~fil_space_crypt_t();
		ut_free(crypt_data);
		return FIL_LOAD_INVALID;
	}

	space = fil_space_t::create(
		space_id, flags, FIL_TYPE_TABLESPACE, crypt_data);

	if (space == NULL) {
		return(FIL_LOAD_INVALID);
	}

	ut_ad(space->id == file.space_id());
	ut_ad(space->id == space_id);

	/* We do not use the size information we have about the file, because
	the rounding formula for extents and pages is somewhat complex; we
	let fil_node_open() do that task. */

	space->add(file.filepath(), OS_FILE_CLOSED, 0, false, false);

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
	if (space->full_crc32() || fil_space_t::full_crc32(flags)) {
		return;
	}
	if (!space->size && (space->purpose != FIL_TYPE_TABLESPACE
			     || !space->get_size())) {
		return;
	}
	/* This code is executed during server startup while no
	connections are allowed. We do not need to protect against
	DROP TABLE by fil_space_acquire(). */
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
                                               uint32_t table_flags)
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
static void fil_invalid_page_access_msg(bool fatal, const char *name,
                                        os_offset_t offset, ulint len,
                                        bool is_read)
{
  sql_print_error("%s%s %zu bytes at " UINT64PF
                  " outside the bounds of the file: %s",
                  fatal ? "[FATAL] InnoDB: " : "InnoDB: ",
                  is_read ? "Trying to read" : "Trying to write",
                  len, offset, name);
  if (fatal)
    abort();
}

/** Update the data structures on write completion */
inline void fil_node_t::complete_write()
{
  mysql_mutex_assert_not_owner(&fil_system.mutex);

  if (space->purpose != FIL_TYPE_TEMPORARY &&
      srv_file_flush_method != SRV_O_DIRECT_NO_FSYNC &&
      space->set_needs_flush())
  {
    mysql_mutex_lock(&fil_system.mutex);
    if (!space->is_in_unflushed_spaces)
    {
      space->is_in_unflushed_spaces= true;
      fil_system.unflushed_spaces.push_front(*space);
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
                         void *buf, buf_page_t *bpage)
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

	if (type.type == IORequest::READ_ASYNC && is_stopping()) {
		release();
		return {DB_TABLESPACE_DELETED, nullptr};
	}

	ulint p = static_cast<ulint>(offset >> srv_page_size_shift);
	bool fatal;

	if (UNIV_LIKELY_NULL(UT_LIST_GET_NEXT(chain, node))) {
		ut_ad(this == fil_system.sys_space
		      || this == fil_system.temp_space);
		ut_ad(!(offset & ((1 << srv_page_size_shift) - 1)));

		while (node->size <= p) {
			p -= node->size;
			node = UT_LIST_GET_NEXT(chain, node);
			if (!node) {
				release();
				if (type.type != IORequest::READ_ASYNC) {
					fatal = true;
fail:
					fil_invalid_page_access_msg(
						fatal, node->name,
						offset, len,
						type.is_read());
				}
				return {DB_IO_ERROR, nullptr};
			}
		}

		offset = os_offset_t{p} << srv_page_size_shift;
	}

	if (UNIV_UNLIKELY(node->size <= p)) {
		release();

		if (type.type == IORequest::READ_ASYNC) {
			/* If we can tolerate the non-existent pages, we
			should return with DB_ERROR and let caller decide
			what to do. */
			return {DB_ERROR, nullptr};
		}

		fatal = node->space->purpose != FIL_TYPE_IMPORT;
		goto fail;
	}

	dberr_t err;

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

	/* We an try to recover the page from the double write buffer if
	the decompression fails or the page is corrupt. */

	ut_a(type.type == IORequest::DBLWR_RECOVER || err == DB_SUCCESS);
	if (!type.is_async()) {
		if (type.is_write()) {
release_sync_write:
			node->complete_write();
release:
			release();
		}
		ut_ad(fil_validate_skip());
	}
	if (err != DB_SUCCESS) {
		goto release;
	}
	return {err, node};
}

#include <tpool.h>

/** Callback for AIO completion */
void fil_aio_callback(const IORequest &request)
{
  ut_ad(fil_validate_skip());
  ut_ad(request.node);

  if (!request.bpage)
  {
    ut_ad(!srv_read_only_mode);
    if (request.type == IORequest::DBLWR_BATCH)
      buf_dblwr.flush_buffered_writes_completed(request);
    else
      ut_ad(request.type == IORequest::WRITE_ASYNC);
write_completed:
    request.node->complete_write();
  }
  else if (request.is_write())
  {
    buf_page_write_complete(request);
    goto write_completed;
  }
  else
  {
    ut_ad(request.is_read());

    /* IMPORTANT: since i/o handling for reads will read also the insert
    buffer in fil_system.sys_space, we have to be very careful not to
    introduce deadlocks. We never close fil_system.sys_space data
    files and never issue asynchronous reads of change buffer pages. */
    const page_id_t id(request.bpage->id());

    if (dberr_t err= request.bpage->read_complete(*request.node))
    {
      if (recv_recovery_is_on() && !srv_force_recovery)
      {
        mysql_mutex_lock(&recv_sys.mutex);
        recv_sys.set_corrupt_fs();
        mysql_mutex_unlock(&recv_sys.mutex);
      }

      ib::error() << "Failed to read page " << id.page_no()
                  << " from file '" << request.node->name << "': " << err;
    }
  }

  request.node->space->release();
}

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS. */
void fil_flush_file_spaces()
{
  if (srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC)
  {
    ut_d(mysql_mutex_lock(&fil_system.mutex));
    ut_ad(fil_system.unflushed_spaces.empty());
    ut_d(mysql_mutex_unlock(&fil_system.mutex));
    return;
  }

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
	void	operator()(const fil_node_t* elem)
	{
		n_open += elem->is_open();
		size += elem->size;
	}

	/** Validate a tablespace.
	@param[in]	space	tablespace to validate
	@return		number of open file nodes */
	static ulint validate(const fil_space_t* space)
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
bool fil_validate()
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

/*********************************************************************//**
Sets the file page type. */
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type)	/*!< in: type */
{
	ut_ad(page);

	mach_write_to_2(page + FIL_PAGE_TYPE, type);
}

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables.
@param[in] ibd_filepath File path of the IBD tablespace */
void fil_delete_file(const char *ibd_filepath)
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
void
fil_space_validate_for_mtr_commit(
	const fil_space_t*	space)
{
	mysql_mutex_assert_not_owner(&fil_system.mutex);
	ut_ad(space != NULL);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);
	ut_ad(!is_predefined_tablespace(space->id));

	/* We are serving mtr_commit(). While there is an active
	mini-transaction, we should have !space->stop_new_ops. This is
	guaranteed by meta-data locks or transactional locks. */
	ut_ad(!space->is_stopping()
	      || space->is_being_truncated /* fil_truncate_prepare() */
	      || space->referenced());
}
#endif /* UNIV_DEBUG */

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space)
{
#ifndef SUX_LOCK_GENERIC
	ut_ad(log_sys.latch.is_write_locked());
#endif
	ut_ad(recv_recovery_is_on());
	ut_ad(log_sys.get_lsn() != 0);
	ut_ad(space->max_lsn == 0);
	ut_d(fil_space_validate_for_mtr_commit(space));

	fil_system.named_spaces.push_back(*space);
	space->max_lsn = log_sys.get_lsn();
}

/** Write a FILE_MODIFY record when a non-predefined persistent
tablespace was modified for the first time since fil_names_clear(). */
ATTRIBUTE_NOINLINE ATTRIBUTE_COLD void mtr_t::name_write()
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif
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
lsn_t fil_names_clear(lsn_t lsn)
{
	mtr_t	mtr;

#ifndef SUX_LOCK_GENERIC
	ut_ad(log_sys.latch.is_write_locked());
#endif
	ut_ad(lsn);
	ut_ad(log_sys.is_latest());

	mtr.start();

	for (auto it = fil_system.named_spaces.begin();
	     it != fil_system.named_spaces.end(); ) {
		if (mtr.get_log()->size() + strlen(it->chain.start->name)
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
fil_space_t::name_type fil_space_t::name() const
{
  switch (id) {
  case 0:
    return name_type{"innodb_system", 13};
  case SRV_TMP_SPACE_ID:
    return name_type{"innodb_temporary", 16};
  }

  if (!UT_LIST_GET_FIRST(chain) || srv_is_undo_tablespace(id))
    return name_type{};

  ut_ad(purpose != FIL_TYPE_TEMPORARY);
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

fil_space_t *fil_space_t::next_in_space_list()
{
  space_list_t::iterator it(this);
  auto end= fil_system.space_list.end();
  if (it == end)
    return nullptr;
  ++it;
  return it == end ? nullptr : &*it;
}

fil_space_t *fil_space_t::prev_in_space_list()
{
  space_list_t::iterator it(this);
  if (it == fil_system.space_list.begin())
    return nullptr;
  --it;
  return &*it;
}

fil_space_t *fil_space_t::next_in_unflushed_spaces()
{
  sized_ilist<fil_space_t, unflushed_spaces_tag_t>::iterator it(this);
  auto end= fil_system.unflushed_spaces.end();
  if (it == end)
    return nullptr;
  ++it;
  return it == end ? nullptr : &*it;
}

fil_space_t *fil_space_t::prev_in_unflushed_spaces()
{
  sized_ilist<fil_space_t, unflushed_spaces_tag_t>::iterator it(this);
  if (it == fil_system.unflushed_spaces.begin())
    return nullptr;
  --it;
  return &*it;
}

#endif
