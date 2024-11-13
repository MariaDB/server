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
#include "os0event.h"
#include "sync0sync.h"
#include "buf0flu.h"
#include "log.h"
#ifdef __linux__
# include <sys/types.h>
# include <sys/sysmacros.h>
# include <dirent.h>
#endif

/** Determine if the space id is a user tablespace id or not.
@param space_id tablespace identifier
@return true if it is a user tablespace ID */
inline bool fil_is_user_tablespace_id(ulint space_id)
{
  return space_id != TRX_SYS_SPACE && space_id != SRV_TMP_SPACE_ID &&
    !srv_is_undo_tablespace(space_id);
}

/** Try to close a file to adhere to the innodb_open_files limit.
@param ignore_space Ignore the tablespace which is acquired by caller
@param print_info   whether to diagnose why a file cannot be closed
@return whether a file was closed */
bool fil_space_t::try_to_close(fil_space_t *ignore_space, bool print_info)
{
  ut_ad(mutex_own(&fil_system.mutex));
  for (fil_space_t *space= UT_LIST_GET_FIRST(fil_system.space_list); space;
       space= UT_LIST_GET_NEXT(space_list, space))
  {
    switch (space->purpose) {
    case FIL_TYPE_TEMPORARY:
      continue;
    case FIL_TYPE_IMPORT:
      break;
    case FIL_TYPE_TABLESPACE:
      if (space == ignore_space
          || !fil_is_user_tablespace_id(space->id))
        continue;
    }

    /* We are using an approximation of LRU replacement policy. In
    fil_node_open_file_low(), newly opened files are moved to the end
    of fil_system.space_list, so that they would be less likely to be
    closed here. */
    fil_node_t *node= UT_LIST_GET_FIRST(space->chain);
    if (!node)
      /* fil_ibd_create() did not invoke fil_space_t::add() yet */
      continue;
    ut_ad(!UT_LIST_GET_NEXT(chain, node));

    if (!node->is_open())
      continue;

    if (const auto n= space->set_closing())
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

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		tablespace identifier
@param[in]	old_path	old file name
@param[in]	new_name	new table name in the
databasename/tablename format
@param[in]	new_path_in	new file name,
or NULL if it is located in the normal data directory
@return true if success */
static bool
fil_rename_tablespace(
	ulint		id,
	const char*	old_path,
	const char*	new_name,
	const char*	new_path_in);

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
current working directory ".", but in the MySQL Embedded Server Library
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
UNIV_INTERN extern uint srv_fil_crypt_rotate_key_age;

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

/*******************************************************************//**
Returns the table space by a given id, NULL if not found.
It is unsafe to dereference the returned pointer. It is fine to check
for NULL. */
fil_space_t*
fil_space_get_by_id(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	ut_ad(fil_system.is_initialised());
	ut_ad(mutex_own(&fil_system.mutex));

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
fil_space_t*
fil_space_get(
	ulint	id)
{
	mutex_enter(&fil_system.mutex);
	fil_space_t*	space = fil_space_get_by_id(id);
	mutex_exit(&fil_system.mutex);
	return(space);
}

/** Validate the compression algorithm for full crc32 format.
@param[in]	space	tablespace object
@return whether the compression algorithm support */
static bool fil_comp_algo_validate(const fil_space_t* space)
{
	if (!space->full_crc32()) {
		return true;
	}

	DBUG_EXECUTE_IF("fil_comp_algo_validate_fail",
			return false;);

	ulint	comp_algo = space->get_compression_algo();
	switch (comp_algo) {
	case PAGE_UNCOMPRESSED:
	case PAGE_ZLIB_ALGORITHM:
#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM:
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM:
#endif /* HAVE_LZMA */
#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM:
#endif /* HAVE_BZIP2 */
#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM:
#endif /* HAVE_SNAPPY */
		return true;
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

	node->magic_n = FIL_NODE_MAGIC_N;

	node->init_size = size;
	node->max_size = max_pages;

	node->space = this;

	node->atomic_write = atomic_write;

	mutex_enter(&fil_system.mutex);
	this->size += size;
	UT_LIST_ADD_LAST(chain, node);
	if (node->is_open()) {
		node->find_metadata();
		n_pending.fetch_and(~CLOSING, std::memory_order_relaxed);
		if (++fil_system.n_open >= srv_max_n_open_files) {
			reacquire();
			try_to_close(this, true);
			release();
		}
	}
	mutex_exit(&fil_system.mutex);

	return node;
}

__attribute__((warn_unused_result, nonnull))
/** Open a tablespace file.
@param node  data file
@return whether the file was successfully opened */
static bool fil_node_open_file_low(fil_node_t *node)
{
  ut_ad(!node->is_open());
  ut_ad(node->space->is_closing());
  ut_ad(mutex_own(&fil_system.mutex));
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

    if (success && node->is_open())
    {
#ifndef _WIN32
      if (!node->space->id && !srv_read_only_mode && my_disable_locking &&
          os_file_lock(node->handle, node->name))
        goto fail;
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

  if (node->size);
  else if (!node->read_page0() || !fil_comp_algo_validate(node->space))
  {
#ifndef _WIN32
  fail:
#endif
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
@return whether the file was successfully opened */
static bool fil_node_open_file(fil_node_t *node)
{
  ut_ad(mutex_own(&fil_system.mutex));
  ut_ad(!node->is_open());
  ut_ad(fil_is_user_tablespace_id(node->space->id) ||
        srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_DELTA);
  ut_ad(node->space->purpose != FIL_TYPE_TEMPORARY);
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
      mutex_exit(&fil_system.mutex);
      os_thread_sleep(20000);
      /* Flush tablespaces so that we can close modified files. */
      fil_flush_file_spaces();
      mutex_enter(&fil_system.mutex);
      if (node->is_open())
        return true;
    }
  }

  /* The node can be opened beween releasing and acquiring fil_system.mutex
  in the above code */
  return node->is_open() || fil_node_open_file_low(node);
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
  ut_ad(mutex_own(&fil_system.mutex));
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
  ut_ad(!mutex_own(&fil_system.mutex));

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
    mutex_enter(&fil_system.mutex);
    if (is_in_unflushed_spaces)
    {
      is_in_unflushed_spaces= false;
      fil_system.unflushed_spaces.remove(*this);
    }
    mutex_exit(&fil_system.mutex);
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
	ut_ad(mutex_own(&fil_system.mutex));
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
		mutex_exit(&fil_system.mutex);
		os_thread_sleep(100000);
		return(true);
	}

	node->being_extended = true;

	/* At this point it is safe to release fil_system.mutex. No
	other thread can rename, delete, close or extend the file because
	we have set the node->being_extended flag. */
	mutex_exit(&fil_system.mutex);

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
				    space->is_compressed());

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
	mutex_enter(&fil_system.mutex);

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
		mutex_exit(&fil_system.mutex);
		space->flush_low();
		space->release();
		mutex_enter(&fil_system.mutex);
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
ATTRIBUTE_COLD bool fil_space_t::prepare_acquired()
{
  ut_ad(referenced());
  ut_ad(mutex_own(&fil_system.mutex));
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
      mutex_enter(&fil_system.mutex);

    ut_ad(mutex_own(&fil_system.mutex));
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
   n_pending.fetch_and(~CLOSING, std::memory_order_relaxed);

  return is_open;
}

/** @return whether the file is usable for io() */
ATTRIBUTE_COLD bool fil_space_t::acquire_and_prepare()
{
  mutex_enter(&fil_system.mutex);
  const auto flags= acquire_low() & (STOPPING | CLOSING);
  const bool is_open= !flags || (flags == CLOSING && prepare_acquired());
  mutex_exit(&fil_system.mutex);
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
  mutex_enter(&fil_system.mutex);
  if (acquired || space->is_being_truncated)
  {
    while (fil_space_extend_must_retry(space, UT_LIST_GET_LAST(space->chain),
                                       size, &success))
      mutex_enter(&fil_system.mutex);
  }
  mutex_exit(&fil_system.mutex);
  if (acquired)
    space->release();
  return success;
}

/** Prepare to free a file from fil_system. */
inline pfs_os_file_t fil_node_t::close_to_free(bool detach_handle)
{
  ut_ad(mutex_own(&fil_system.mutex));
  ut_a(magic_n == FIL_NODE_MAGIC_N);
  ut_a(!being_extended);

  if (is_open() &&
      (space->n_pending.fetch_or(fil_space_t::CLOSING,
                                 std::memory_order_acquire) &
       fil_space_t::PENDING))
  {
    mutex_exit(&fil_system.mutex);
    while (space->referenced())
      os_thread_sleep(100);
    mutex_enter(&fil_system.mutex);
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

/** Detach a tablespace from the cache and close the files. */
std::vector<pfs_os_file_t> fil_system_t::detach(fil_space_t *space,
                                                bool detach_handle)
{
  ut_ad(mutex_own(&fil_system.mutex));
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
  if (space_list_last_opened == space)
    space_list_last_opened = UT_LIST_GET_PREV(space_list, space);
  UT_LIST_REMOVE(space_list, space);
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

  std::vector<pfs_os_file_t> handles;
  handles.reserve(UT_LIST_GET_LEN(space->chain));

  for (fil_node_t* node= UT_LIST_GET_FIRST(space->chain); node;
       node= UT_LIST_GET_NEXT(chain, node))
  {
    auto handle= node->close_to_free(detach_handle);
    if (handle != OS_FILE_CLOSED)
      handles.push_back(handle);
  }

  ut_ad(!space->referenced());
  return handles;
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
		os_thread_sleep(100);
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

	rw_lock_free(&space->latch);
	fil_space_destroy_crypt_data(&space->crypt_data);

	space->~fil_space_t();
	ut_free(space);
}

/** Frees a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param[in]	id		tablespace identifier
@param[in]	x_latched	whether the caller holds X-mode space->latch
@return true if success */
bool
fil_space_free(
	ulint		id,
	bool		x_latched)
{
	ut_ad(id != TRX_SYS_SPACE);

	mutex_enter(&fil_system.mutex);
	fil_space_t*	space = fil_space_get_by_id(id);

	if (space != NULL) {
		fil_system.detach(space);
	}

	mutex_exit(&fil_system.mutex);

	if (space != NULL) {
		if (x_latched) {
			rw_lock_x_unlock(&space->latch);
		}

		if (!recv_recovery_is_on()) {
			mysql_mutex_lock(&log_sys.mutex);
		}

		mysql_mutex_assert_owner(&log_sys.mutex);

		if (space->max_lsn != 0) {
			ut_d(space->max_lsn = 0);
			UT_LIST_REMOVE(fil_system.named_spaces, space);
		}

		if (!recv_recovery_is_on()) {
			mysql_mutex_unlock(&log_sys.mutex);
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
@param opened     true if space files are opened
@return pointer to created tablespace, to be filled in with add()
@retval nullptr on failure (such as when the same tablespace exists) */
fil_space_t *fil_space_t::create(const char *name, ulint id, ulint flags,
                                 fil_type_t purpose,
				 fil_space_crypt_t *crypt_data,
				 fil_encryption_t mode,
				 bool opened)
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
	space->name = mem_strdup(name);

	UT_LIST_INIT(space->chain, &fil_node_t::chain);

	space->purpose = purpose;
	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;
	space->crypt_data = crypt_data;
	space->n_pending.store(CLOSING, std::memory_order_relaxed);

	DBUG_LOG("tablespace",
		 "Created metadata for " << id << " name " << name);
	if (crypt_data) {
		DBUG_LOG("crypt",
			 "Tablespace " << id << " name " << name
			 << " encryption " << crypt_data->encryption
			 << " key id " << crypt_data->key_id
			 << ":" << fil_crypt_get_mode(crypt_data)
			 << " " << fil_crypt_get_type(crypt_data));
	}

	rw_lock_create(fil_space_latch_key, &space->latch, SYNC_FSP);

	if (space->purpose == FIL_TYPE_TEMPORARY) {
		/* SysTablespace::open_or_create() would pass
		size!=0 to fil_space_t::add(), so first_time_open
		would not hold in fil_node_open_file(), and we
		must assign this manually. We do not care about
		the durability or atomicity of writes to the
		temporary tablespace files. */
		space->atomic_write_supported = true;
	}

	mutex_enter(&fil_system.mutex);

	if (const fil_space_t *old_space = fil_space_get_by_id(id)) {
		ib::error() << "Trying to add tablespace '" << name
			<< "' with id " << id
			<< " to the tablespace memory cache, but tablespace '"
			<< old_space->name << "' already exists in the cache!";
		mutex_exit(&fil_system.mutex);
		rw_lock_free(&space->latch);
		space->~fil_space_t();
		ut_free(space);
		return(NULL);
	}

	HASH_INSERT(fil_space_t, hash, &fil_system.spaces, id, space);

	if (opened)
	  fil_system.add_opened_last_to_space_list(space);
	else
	  UT_LIST_ADD_LAST(fil_system.space_list, space);

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
				<< " for " << name << ", old maximum was "
				<< fil_system.max_assigned_id;
		}

		fil_system.max_assigned_id = id;
	}

	const bool rotate =
		(purpose == FIL_TYPE_TABLESPACE
		 && (mode == FIL_ENCRYPTION_ON
		     || mode == FIL_ENCRYPTION_OFF || srv_encrypt_tables)
		 && fil_crypt_must_default_encrypt());

	/* Inform key rotation that there could be something
	to do */
	if (rotate) {
		/* Key rotation is not enabled, need to inform background
		encryption threads. */
		fil_system.default_encrypt_tables.push_back(*space);
		space->is_in_default_encrypt = true;
	}

	mutex_exit(&fil_system.mutex);

	if (rotate && srv_n_fil_crypt_threads_started) {
		os_event_set(fil_crypt_threads_event);
	}

	return(space);
}

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */
bool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id)	/*!< in/out: space id */
{
	ulint	id;
	bool	success;

	mutex_enter(&fil_system.mutex);

	id = *space_id;

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
		*space_id = ULINT_UNDEFINED;
	}

	mutex_exit(&fil_system.mutex);

	return(success);
}

/** Read the first page of a data file.
@return whether the page was found valid */
bool fil_space_t::read_page0()
{
  ut_ad(fil_system.is_initialised());
  ut_ad(mutex_own(&fil_system.mutex));
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
static fil_space_t *fil_space_get_space(ulint id)
{
  if (fil_space_t *space= fil_space_get_by_id(id))
    if (space->read_page0())
      return space;
  return nullptr;
}

void fil_space_set_recv_size_and_flags(ulint id, uint32_t size, uint32_t flags)
{
  ut_ad(id < SRV_SPACE_ID_UPPER_BOUND);
  mutex_enter(&fil_system.mutex);
  if (fil_space_t *space= fil_space_get_space(id))
  {
    if (size)
      space->recv_size= size;
    if (flags != FSP_FLAGS_FCRC32_MASK_MARKER)
      space->flags= flags;
  }
  mutex_exit(&fil_system.mutex);
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

  mutex_enter(&fil_system.mutex);

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
      node->find_metadata();
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
  mutex_exit(&fil_system.mutex);
  return success;
}

/** Close each file. Only invoked on fil_system.temp_space. */
void fil_space_t::close()
{
	if (!fil_system.is_initialised()) {
		return;
	}

	mutex_enter(&fil_system.mutex);
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

	mutex_exit(&fil_system.mutex);
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

	mutex_create(LATCH_ID_FIL_SYSTEM, &mutex);

	spaces.create(hash_size);

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

void fil_system_t::close()
{
  ut_ad(this == &fil_system);
  ut_a(unflushed_spaces.empty());
  ut_a(!UT_LIST_GET_LEN(space_list));
  ut_ad(!sys_space);
  ut_ad(!temp_space);

  if (is_initialised())
  {
    m_initialised= false;
    spaces.free();
    mutex_free(&mutex);
    fil_space_crypt_cleanup();
  }

  ut_ad(!spaces.array);

#ifdef __linux__
  ssd.clear();
  ssd.shrink_to_fit();
#endif /* __linux__ */
}

void fil_system_t::add_opened_last_to_space_list(fil_space_t *space)
{
  if (UNIV_LIKELY(space_list_last_opened != nullptr))
    UT_LIST_INSERT_AFTER(space_list, space_list_last_opened, space);
  else
    UT_LIST_ADD_FIRST(space_list, space);
  space_list_last_opened= space;
}

/** Extend all open data files to the recovered size */
ATTRIBUTE_COLD void fil_system_t::extend_to_recv_size()
{
  ut_ad(is_initialised());
  mutex_enter(&mutex);
  for (fil_space_t *space= UT_LIST_GET_FIRST(fil_system.space_list); space;
       space= UT_LIST_GET_NEXT(space_list, space))
  {
    const uint32_t size= space->recv_size;

    if (size > space->size)
    {
      if (space->is_closing())
        continue;
      space->reacquire();
      bool success;
      while (fil_space_extend_must_retry(space, UT_LIST_GET_LAST(space->chain),
                                         size, &success))
        mutex_enter(&mutex);
      /* Crash recovery requires the file extension to succeed. */
      ut_a(success);
      space->release();
    }
  }
  mutex_exit(&mutex);
}

/** Close all tablespace files at shutdown */
void fil_space_t::close_all()
{
	if (!fil_system.is_initialised()) {
		return;
	}

	fil_space_t*	space;

	/* At shutdown, we should not have any files in this list. */
	ut_ad(srv_fast_shutdown == 2
	      || !srv_was_started
	      || UT_LIST_GET_LEN(fil_system.named_spaces) == 0);
	fil_flush_file_spaces();

	mutex_enter(&fil_system.mutex);

	for (space = UT_LIST_GET_FIRST(fil_system.space_list); space; ) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (!node->is_open()) {
next:
				continue;
			}

			for (ulint count = 10000; count--; ) {
				if (!space->set_closing()) {
					node->close();
					goto next;
				}
				mutex_exit(&fil_system.mutex);
				os_thread_sleep(100);
				mutex_enter(&fil_system.mutex);
				if (!node->is_open()) {
					goto next;
				}
			}

			ib::error() << "File '" << node->name
				    << "' has " << space->referenced()
				    << " operations";
		}

		space = UT_LIST_GET_NEXT(space_list, space);
		fil_system.detach(prev_space);
		fil_space_free_low(prev_space);
	}

	mutex_exit(&fil_system.mutex);

	ut_ad(srv_fast_shutdown == 2
	      || !srv_was_started
	      || UT_LIST_GET_LEN(fil_system.named_spaces) == 0);
}

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id)	/*!< in: maximum known id */
{
	if (max_id >= SRV_SPACE_ID_UPPER_BOUND) {
		ib::fatal() << "Max tablespace id is too high, " << max_id;
	}

	mutex_enter(&fil_system.mutex);

	if (fil_system.max_assigned_id < max_id) {

		fil_system.max_assigned_id = max_id;
	}

	mutex_exit(&fil_system.mutex);
}

/** Write the flushed LSN to the page header of the first page in the
system tablespace.
@param[in]	lsn	flushed LSN
@return DB_SUCCESS or error number */
dberr_t
fil_write_flushed_lsn(
	lsn_t	lsn)
{
	byte*	buf;
	ut_ad(!srv_read_only_mode);

	if (!fil_system.sys_space->acquire()) {
		return DB_ERROR;
	}

	buf = static_cast<byte*>(aligned_malloc(srv_page_size, srv_page_size));

	auto fio = fil_system.sys_space->io(IORequestRead, 0, srv_page_size,
					    buf);

	if (fio.err == DB_SUCCESS) {
		mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
				lsn);

		ulint fsp_flags = mach_read_from_4(
			buf + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS);

		if (fil_space_t::full_crc32(fsp_flags)) {
			buf_flush_assign_full_crc32_checksum(buf);
		}

		fio = fil_system.sys_space->io(IORequestWrite,
					       0, srv_page_size, buf);
		fil_flush_file_spaces();
	} else {
		fil_system.sys_space->release();
	}

	aligned_free(buf);
	return fio.err;
}

/** Acquire a tablespace reference.
@param id      tablespace identifier
@return tablespace
@retval nullptr if the tablespace is missing or inaccessible */
fil_space_t *fil_space_t::get(ulint id)
{
  mutex_enter(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  const uint32_t n= space ? space->acquire_low() : 0;

  if (n & STOPPING)
    space= nullptr;
  else if ((n & CLOSING) && !space->prepare_acquired())
    space= nullptr;

  mutex_exit(&fil_system.mutex);
  return space;
}

/** Write a log record about a file operation.
@param type           file operation
@param first_page_no  first page number in the file
@param path           file path
@param new_path       new file path for type=FILE_RENAME */
inline void mtr_t::log_file_op(mfile_type_t type, ulint space_id,
			       const char *path, const char *new_path)
{
  ut_ad((new_path != nullptr) == (type == FILE_RENAME));
  ut_ad(!(byte(type) & 15));

  /* fil_name_parse() requires that there be at least one path
  separator and that the file path end with ".ibd". */
  ut_ad(strchr(path, OS_PATH_SEPARATOR) != NULL);
  ut_ad(!strcmp(&path[strlen(path) - strlen(DOT_IBD)], DOT_IBD));

  flag_modified();
  if (m_log_mode != MTR_LOG_ALL)
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
    ut_ad(strchr(new_path, OS_PATH_SEPARATOR));
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len + 1));
    m_log.push(reinterpret_cast<const byte*>(new_path), uint32_t(new_len));
  }
  else
    m_log.push(reinterpret_cast<const byte*>(path), uint32_t(len));
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write_rename_low(
	ulint		space_id,
	const char*	old_name,
	const char*	new_name,
	mtr_t*		mtr)
{
  ut_ad(!is_predefined_tablespace(space_id));
  mtr->log_file_op(FILE_RENAME, space_id, old_name, new_name);
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming */
static void
fil_name_write_rename(
	ulint		space_id,
	const char*	old_name,
	const char*	new_name)
{
	mtr_t	mtr;
	mtr.start();
	fil_name_write_rename_low(space_id, old_name, new_name, &mtr);
	mtr.commit();
	log_write_up_to(mtr.commit_lsn(), true);
}

/** Write FILE_MODIFY for a file.
@param[in]	space_id	tablespace id
@param[in]	name		tablespace file name
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write(
	ulint		space_id,
	const char*	name,
	mtr_t*		mtr)
{
  ut_ad(!is_predefined_tablespace(space_id));
  mtr->log_file_op(FILE_MODIFY, space_id, name);
}

/** Check for pending operations.
@param[in]	space	tablespace
@param[in]	count	number of attempts so far
@return 0 if no operations else count + 1. */
static ulint fil_check_pending_ops(const fil_space_t* space, ulint count)
{
	ut_ad(mutex_own(&fil_system.mutex));

	if (!space) {
		return 0;
	}

	if (auto n_pending_ops = space->referenced()) {

		/* Give a warning every 10 second, starting after 1 second */
		if ((count % 500) == 50) {
			ib::warn() << "Trying to delete"
				" tablespace '" << space->name
				<< "' but there are " << n_pending_ops
				<< " pending operations on it.";
		}

		return(count + 1);
	}

	return(0);
}

/*******************************************************************//**
Check for pending IO.
@return 0 if no pending else count + 1. */
static
ulint
fil_check_pending_io(
/*=================*/
	fil_space_t*	space,		/*!< in/out: Tablespace to check */
	fil_node_t**	node,		/*!< out: Node in space list */
	ulint		count)		/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system.mutex));

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_ad(UT_LIST_GET_LEN(space->chain) == 1);

	*node = UT_LIST_GET_FIRST(space->chain);

	if (const uint32_t p = space->referenced()) {
		ut_a(!(*node)->being_extended);

                /* Give a warning every 10 second, starting after 1 second */
		if ((count % 500) == 50) {
			ib::info() << "Trying to delete"
				" tablespace '" << space->name
				<< "' but there are " << p
				<< " pending i/o's on it.";
		}

		return(count + 1);
	}

	return(0);
}

/*******************************************************************//**
Check pending operations on a tablespace.
@return tablespace */
static
fil_space_t*
fil_check_pending_operations(
/*=========================*/
	ulint		id,		/*!< in: space id */
	bool		truncate,	/*!< in: whether to truncate a file */
	char**		path)		/*!< out/own: tablespace path */
{
	ulint		count = 0;

	ut_a(!is_system_tablespace(id));
	mutex_enter(&fil_system.mutex);
	fil_space_t* sp = fil_space_get_by_id(id);

	if (sp) {
		sp->set_stopping(true);
		if (sp->crypt_data) {
			sp->reacquire();
			mutex_exit(&fil_system.mutex);
			fil_space_crypt_close_tablespace(sp);
			mutex_enter(&fil_system.mutex);
			sp->release();
		}
	}

	/* Check for pending operations. */

	do {
		count = fil_check_pending_ops(sp, count);

		mutex_exit(&fil_system.mutex);

		if (count) {
			os_thread_sleep(20000); // Wait 0.02 seconds
		} else if (!sp) {
			return nullptr;
		}

		mutex_enter(&fil_system.mutex);

		sp = fil_space_get_by_id(id);
	} while (count);

	/* Check for pending IO. */

	for (;;) {
		if (truncate) {
			sp->is_being_truncated = true;
		}

		fil_node_t*	node;

		count = fil_check_pending_io(sp, &node, count);

		if (count == 0 && path) {
			*path = mem_strdup(node->name);
		}

		mutex_exit(&fil_system.mutex);

		if (count == 0) {
			break;
		}

		os_thread_sleep(20000);         // Wait 0.02 seconds
		mutex_enter(&fil_system.mutex);
		sp = fil_space_get_by_id(id);

		if (!sp) {
			mutex_exit(&fil_system.mutex);
			break;
		}
	}

	return sp;
}

/** Close a single-table tablespace on failed IMPORT TABLESPACE.
The tablespace must be cached in the memory cache.
Free all pages used by the tablespace. */
void fil_close_tablespace(ulint id)
{
	ut_ad(!is_system_tablespace(id));
	char* path = nullptr;
	fil_space_t* space = fil_check_pending_operations(id, false, &path);
	if (!space) {
		return;
	}

	rw_lock_x_lock(&space->latch);

	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have invoked space->set_stopping(), readahead
	can no longer read more pages of this tablespace to buf_pool.
	Thus we can clean the tablespace out of buf_pool
	completely and permanently. */
	while (buf_flush_list_space(space));
	ut_ad(space->is_stopping());

	/* If the free is successful, the X lock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free(id, true)) {
		rw_lock_x_unlock(&space->latch);
	}

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	if (char* cfg_name = fil_make_filepath(path, NULL, CFG, false)) {
		os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
		ut_free(cfg_name);
	}

	ut_free(path);
}

/** Delete a tablespace and associated .ibd file.
@param[in]	id		tablespace identifier
@param[in]	if_exists	whether to ignore missing tablespace
@param[in,out]	detached_handles	return detached handles if not nullptr
@return	DB_SUCCESS or error */
dberr_t fil_delete_tablespace(ulint id, bool if_exists,
			      std::vector<pfs_os_file_t>* detached_handles)
{
	char* path = NULL;
	ut_ad(!is_system_tablespace(id));
	ut_ad(!detached_handles || detached_handles->empty());

	dberr_t err;
	fil_space_t *space = fil_check_pending_operations(id, false, &path);

	if (!space) {
		err = DB_TABLESPACE_NOT_FOUND;
		if (!if_exists) {
			ib::error() << "Cannot delete tablespace " << id
				    << " because it is not found"
				       " in the tablespace memory cache.";
		}

		goto func_exit;
	}

	/* IMPORTANT: Because we have set space::stop_new_ops there
	can't be any new reads or flushes. We are here
	because node::n_pending was zero above. However, it is still
	possible to have pending read and write requests:

	A read request can happen because the reader thread has
	gone through the ::stop_new_ops check in buf_page_init_for_read()
	before the flag was set and has not yet incremented ::n_pending
	when we checked it above.

	A write request can be issued any time because we don't check
	fil_space_t::is_stopping() when queueing a block for write.

	We deal with pending write requests in the following function
	where we'd minimally evict all dirty pages belonging to this
	space from the flush_list. Note that if a block is IO-fixed
	we'll wait for IO to complete.

	To deal with potential read requests, we will check the
	is_stopping() in fil_space_t::io(). */

	err = DB_SUCCESS;
	buf_flush_remove_pages(id);

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */
	{
		/* Before deleting the file, write a log record about
		it, so that InnoDB crash recovery will expect the file
		to be gone. */
		mtr_t		mtr;

		mtr.start();
		mtr.log_file_op(FILE_DELETE, id, path);
		mtr.commit();
		/* Even if we got killed shortly after deleting the
		tablespace file, the record must have already been
		written to the redo log. */
		log_write_up_to(mtr.commit_lsn(), true);

		char*	cfg_name = fil_make_filepath(path, NULL, CFG, false);
		if (cfg_name != NULL) {
			os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
			ut_free(cfg_name);
		}
	}

	/* Delete the link file pointing to the ibd file we are deleting. */
	if (FSP_FLAGS_HAS_DATA_DIR(space->flags)) {
		RemoteDatafile::delete_link_file(space->name);
	}

	mutex_enter(&fil_system.mutex);

	/* Double check the sanity of pending ops after reacquiring
	the fil_system::mutex. */
	if (const fil_space_t* s = fil_space_get_by_id(id)) {
		ut_a(s == space);
		ut_a(!space->referenced());
		ut_a(UT_LIST_GET_LEN(space->chain) == 1);
		auto handles = fil_system.detach(space,
						 detached_handles != nullptr);
		if (detached_handles) {
			*detached_handles = std::move(handles);
		}
		mutex_exit(&fil_system.mutex);

		mysql_mutex_lock(&log_sys.mutex);

		if (space->max_lsn != 0) {
			ut_d(space->max_lsn = 0);
			UT_LIST_REMOVE(fil_system.named_spaces, space);
		}

		mysql_mutex_unlock(&log_sys.mutex);
		fil_space_free_low(space);

		if (!os_file_delete(innodb_data_file_key, path)
		    && !os_file_delete_if_exists(
			    innodb_data_file_key, path, NULL)) {

			/* Note: This is because we have removed the
			tablespace instance from the cache. */

			err = DB_IO_ERROR;
		}
	} else {
		mutex_exit(&fil_system.mutex);
		err = DB_TABLESPACE_NOT_FOUND;
	}

func_exit:
	ut_free(path);
	ibuf_delete_for_discarded_space(id);
	return(err);
}

/** Prepare to truncate an undo tablespace.
@param[in]	space_id	undo tablespace id
@return	the tablespace
@retval	NULL if tablespace not found */
fil_space_t *fil_truncate_prepare(ulint space_id)
{
  return fil_check_pending_operations(space_id, true, nullptr);
}

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the directory path or the full path and filename.
@param[in] name NULL if path is full, or Table/Tablespace name
@param[in] extension NULL or the file extension to use.
@param[in] trim_name true if the last name on the path should be trimmed.
@return own: file name */
char*
fil_make_filepath_low(
	const char*	path,
	const char*	name,
	ib_extention	extension,
	bool		trim_name)
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is NULL, we can use the default path,
	but there needs to be a name. */
	ut_ad(path != NULL || name != NULL);

	if (path == NULL) {
		path = fil_path_to_mysql_datadir;
	}

	ulint	len		= 0;	/* current length */
	ulint	path_len	= strlen(path);
	ulint	name_len	= (name ? strlen(name) : 0);
	const char* suffix	= dot_ext[extension];
	ulint	suffix_len	= strlen(suffix);
	ulint	full_len	= path_len + 1 + name_len + suffix_len + 1;

	char*	full_name = static_cast<char*>(ut_malloc_nokey(full_len));
	if (full_name == NULL) {
		return NULL;
	}

	/* If the name is a relative path, do not prepend "./". */
	if (path[0] == '.'
	    && (path[1] == '\0' || path[1] == OS_PATH_SEPARATOR)
	    && name != NULL && name[0] == '.') {
		path = NULL;
		path_len = 0;
	}

	if (path != NULL) {
		memcpy(full_name, path, path_len);
		len = path_len;
	}

	full_name[len] = '\0';
	os_normalize_path(full_name);

	if (trim_name) {
		/* Find the offset of the last DIR separator and set it to
		null in order to strip off the old basename from this path. */
		char* last_dir_sep = strrchr(full_name, OS_PATH_SEPARATOR);
		if (last_dir_sep) {
			last_dir_sep[0] = '\0';
			len = strlen(full_name);
		}
	}

	if (name != NULL) {
		if (len && full_name[len - 1] != OS_PATH_SEPARATOR) {
			/* Add a DIR separator */
			full_name[len] = OS_PATH_SEPARATOR;
			full_name[++len] = '\0';
		}

		char*	ptr = &full_name[len];
		memcpy(ptr, name, name_len);
		len += name_len;
		full_name[len] = '\0';
		os_normalize_path(ptr);
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

/** Test if a tablespace file can be renamed to a new filepath by checking
if that the old filepath exists and the new filepath does not exist.
@param[in]	old_path	old filepath
@param[in]	new_path	new filepath
@param[in]	replace_new	whether to ignore the existence of new_path
@return innodb error code */
static dberr_t
fil_rename_tablespace_check(
	const char*	old_path,
	const char*	new_path,
	bool		replace_new)
{
	bool	exists = false;
	os_file_type_t	ftype;

	if (os_file_status(old_path, &exists, &ftype) && !exists) {
		ib::error() << "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' because the source file"
			<< " does not exist.";
		return(DB_TABLESPACE_NOT_FOUND);
	}

	exists = false;
	auto schema_path= fil_make_dirpath(new_path, NULL, NO_EXT, true);
	if (schema_path == NULL) {
		return DB_ERROR;
	}

	if (os_file_status(schema_path, &exists, &ftype) && !exists) {
		sql_print_error("InnoDB: Cannot rename '%s' to '%s'"
				" because the target schema directory doesn't exist.",
				old_path, new_path);
		ut_free(schema_path);
		return DB_ERROR;
	}
	ut_free(schema_path);
	exists = false;

	if (os_file_status(new_path, &exists, &ftype) && !exists) {
		return DB_SUCCESS;
	}

	if (!replace_new) {
		ib::error() << "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' because the target file exists."
			" Remove the target file and try again.";
		return(DB_TABLESPACE_EXISTS);
	}

	/* This must be during the ROLLBACK of TRUNCATE TABLE.
	Because InnoDB only allows at most one data dictionary
	transaction at a time, and because this incomplete TRUNCATE
	would have created a new tablespace file, we must remove
	a possibly existing tablespace that is associated with the
	new tablespace file. */
retry:
	mutex_enter(&fil_system.mutex);
	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system.space_list);
	     space; space = UT_LIST_GET_NEXT(space_list, space)) {
		ulint id = space->id;
		if (id
		    && space->purpose == FIL_TYPE_TABLESPACE
		    && !strcmp(new_path,
			       UT_LIST_GET_FIRST(space->chain)->name)) {
			ib::info() << "TRUNCATE rollback: " << id
				<< "," << new_path;
			mutex_exit(&fil_system.mutex);
			dberr_t err = fil_delete_tablespace(id);
			if (err != DB_SUCCESS) {
				return err;
			}
			goto retry;
		}
	}
	mutex_exit(&fil_system.mutex);
	fil_delete_file(new_path);

	return(DB_SUCCESS);
}

dberr_t fil_space_t::rename(const char* name, const char* path, bool log,
			    bool replace)
{
	ut_ad(UT_LIST_GET_LEN(chain) == 1);
	ut_ad(!is_system_tablespace(id));

	if (log) {
		dberr_t err = fil_rename_tablespace_check(
			chain.start->name, path, replace);
		if (err != DB_SUCCESS) {
			return(err);
		}
		fil_name_write_rename(id, chain.start->name, path);
	}

	return fil_rename_tablespace(id, chain.start->name, name, path)
		? DB_SUCCESS : DB_ERROR;
}

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		tablespace identifier
@param[in]	old_path	old file name
@param[in]	new_name	new table name in the
databasename/tablename format
@param[in]	new_path_in	new file name,
or NULL if it is located in the normal data directory
@return true if success */
static bool
fil_rename_tablespace(
	ulint		id,
	const char*	old_path,
	const char*	new_name,
	const char*	new_path_in)
{
	fil_space_t*	space;
	fil_node_t*	node;
	ut_a(id != 0);

	ut_ad(strchr(new_name, '/') != NULL);

	mutex_enter(&fil_system.mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib::error() << "Cannot find space id " << id
			<< " in the tablespace memory cache, though the file '"
			<< old_path
			<< "' in a rename operation should have that id.";
		mutex_exit(&fil_system.mutex);
		return(false);
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	node = UT_LIST_GET_FIRST(space->chain);
	space->reacquire();

	mutex_exit(&fil_system.mutex);

	char*	new_file_name = new_path_in == NULL
		? fil_make_filepath(NULL, new_name, IBD, false)
		: mem_strdup(new_path_in);
	char*	old_file_name = node->name;
	char*	new_space_name = mem_strdup(new_name);
	char*	old_space_name = space->name;

	ut_ad(strchr(old_file_name, OS_PATH_SEPARATOR) != NULL);
	ut_ad(strchr(new_file_name, OS_PATH_SEPARATOR) != NULL);

	if (!recv_recovery_is_on()) {
		mysql_mutex_lock(&log_sys.mutex);
	}

	/* log_sys.mutex is above fil_system.mutex in the latching order */
	mysql_mutex_assert_owner(&log_sys.mutex);
	mutex_enter(&fil_system.mutex);
	space->release();
	ut_ad(space->name == old_space_name);
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
	}

	if (!recv_recovery_is_on()) {
		mysql_mutex_unlock(&log_sys.mutex);
	}

	ut_ad(space->name == old_space_name);
	if (success) {
		space->name = new_space_name;
	} else {
		/* Because nothing was renamed, we must free the new
		names, not the old ones. */
		old_file_name = new_file_name;
		old_space_name = new_space_name;
	}

	mutex_exit(&fil_system.mutex);

	ut_free(old_file_name);
	ut_free(old_space_name);

	return(success);
}

/* FIXME: remove this! */
IF_WIN(, bool os_is_sparse_file_supported(os_file_t fh));

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
	ulint		space_id,
	const char*	name,
	const char*	path,
	ulint		flags,
	uint32_t	size,
	fil_encryption_t mode,
	uint32_t	key_id,
	dberr_t*	err)
{
	pfs_os_file_t	file;
	byte*		page;
	bool		success;
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
	bool punch_hole = is_compressed;
	fil_space_crypt_t* crypt_data = nullptr;
#ifdef _WIN32
	if (is_compressed) {
		os_file_set_sparse_win32(file);
	}
#endif

	if (!os_file_set_size(
		path, file,
		os_offset_t(size) << srv_page_size_shift, is_compressed)) {
		*err = DB_OUT_OF_FILE_SPACE;
err_exit:
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		free(crypt_data);
		return NULL;
	}

	/* FIXME: remove this */
	IF_WIN(, punch_hole = punch_hole && os_is_sparse_file_supported(file));

	/* We have to write the space id to the file immediately and flush the
	file to disk. This is because in crash recovery we must be aware what
	tablespaces exist and what are their space id's, so that we can apply
	the log records to the right file. It may take quite a while until
	buffer pool flush algorithms write anything to the file and flush it to
	disk. If we would not write here anything, the file would be filled
	with zeros from the call of os_file_set_size(), until a buffer pool
	flush would write to it. */

	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte*>(aligned_malloc(2 * srv_page_size,
						 srv_page_size));

	memset(page, '\0', srv_page_size);

	if (fil_space_t::full_crc32(flags)) {
		flags |= FSP_FLAGS_FCRC32_PAGE_SSIZE();
	} else {
		flags |= FSP_FLAGS_PAGE_SSIZE();
	}

	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	/* Create crypt data if the tablespace is either encrypted or user has
	requested it to remain unencrypted. */
	crypt_data = (mode != FIL_ENCRYPTION_DEFAULT || srv_encrypt_tables)
		? fil_space_create_crypt_data(mode, key_id)
		: NULL;

	if (crypt_data) {
		/* Write crypt data information in page0 while creating
		ibd file. */
		crypt_data->fill_page0(flags, page);
	}

	if (ulint zip_size = fil_space_t::zip_size(flags)) {
		page_zip_des_t	page_zip;
		page_zip_set_size(&page_zip, zip_size);
		page_zip.data = page + srv_page_size;
#ifdef UNIV_DEBUG
		page_zip.m_start = 0;
#endif /* UNIV_DEBUG */
		page_zip.m_end = 0;
		page_zip.m_nonempty = 0;
		page_zip.n_blobs = 0;

		buf_flush_init_for_writing(NULL, page, &page_zip, false);

		*err = os_file_write(IORequestWrite, path, file,
				     page_zip.data, 0, zip_size);
	} else {
		buf_flush_init_for_writing(NULL, page, NULL,
					   fil_space_t::full_crc32(flags));

		*err = os_file_write(IORequestWrite, path, file,
				     page, 0, srv_page_size);
	}

	aligned_free(page);

	if (*err != DB_SUCCESS) {
		ib::error()
			<< "Could not write the first page to"
			<< " tablespace '" << path << "'";
		goto err_exit;
	}

	if (!os_file_flush(file)) {
		ib::error() << "File flush of tablespace '"
			<< path << "' failed";
		*err = DB_ERROR;
		goto err_exit;
	}

	if (has_data_dir) {
		/* Make the ISL file if the IBD file is not
		in the default location. */
		*err = RemoteDatafile::create_link_file(name, path);
		if (*err != DB_SUCCESS) {
			goto err_exit;
		}
	}

	if (fil_space_t* space = fil_space_t::create(name, space_id, flags,
						     FIL_TYPE_TABLESPACE,
						     crypt_data, mode, true)) {
		space->punch_hole = punch_hole;
		fil_node_t* node = space->add(path, file, size, false, true);
		mtr_t mtr;
		mtr.start();
		mtr.log_file_op(FILE_CREATE, space_id, node->name);
		mtr.commit();

		*err = DB_SUCCESS;
		return space;
	}

	if (has_data_dir) {
		RemoteDatafile::delete_link_file(name);
	}

	*err = DB_ERROR;
	goto err_exit;
}

/** Try to open a single-table tablespace and optionally check that the
space id in it is correct. If this does not succeed, print an error message
to the .err log. This function is used to open a tablespace when we start
mysqld after the dictionary has been booted, and also in IMPORT TABLESPACE.

NOTE that we assume this operation is used either at the database startup
or under the protection of the dictionary mutex, so that two users cannot
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

@param[in]	validate	true if we should validate the tablespace
@param[in]	fix_dict	true if the dictionary is available to be fixed
@param[in]	purpose		FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in]	id		tablespace ID
@param[in]	flags		expected FSP_SPACE_FLAGS
@param[in]	space_name	tablespace name of the datafile
If file-per-table, it is the table name in the databasename/tablename format
@param[in]	path_in		expected filepath, usually read from dictionary
@param[out]	err		DB_SUCCESS or error code
@return	tablespace
@retval	NULL	if the tablespace could not be opened */
fil_space_t*
fil_ibd_open(
	bool			validate,
	bool			fix_dict,
	fil_type_t		purpose,
	ulint			id,
	ulint			flags,
	const table_name_t&	tablename,
	const char*		path_in,
	dberr_t*		err)
{
	mutex_enter(&fil_system.mutex);
	if (fil_space_t* space = fil_space_get_by_id(id)) {
		if (strcmp(space->name, tablename.m_name)) {
			table_name_t space_name;
			space_name.m_name = space->name;
			ib::error()
				<< "Trying to open table " << tablename
				<< " with id " << id
				<< ", conflicting with " << space_name;
			space = NULL;
			if (err) *err = DB_TABLESPACE_EXISTS;
		} else if (err) *err = DB_SUCCESS;

		mutex_exit(&fil_system.mutex);

		if (space && validate && !srv_read_only_mode) {
			fsp_flags_try_adjust(space,
					     flags & ~FSP_FLAGS_MEM_MASK);
		}

		return space;
	}
	mutex_exit(&fil_system.mutex);

	bool		dict_filepath_same_as_default = false;
	bool		link_file_found = false;
	bool		link_file_is_bad = false;
	Datafile	df_default;	/* default location */
	Datafile	df_dict;	/* dictionary location */
	RemoteDatafile	df_remote;	/* remote location */
	ulint		tablespaces_found = 0;
	ulint		valid_tablespaces_found = 0;

	if (fix_dict) {
		ut_d(dict_sys.assert_locked());
		ut_ad(!srv_read_only_mode);
		ut_ad(srv_log_file_size != 0);
	}

	/* Table flags can be ULINT_UNDEFINED if
	dict_tf_to_fsp_flags_failure is set. */
	if (flags == ULINT_UNDEFINED) {
corrupted:
		if (err) *err = DB_CORRUPTION;
		return NULL;
	}

	ut_ad(fil_space_t::is_valid_flags(flags & ~FSP_FLAGS_MEM_MASK, id));
	df_default.init(tablename.m_name, flags);
	df_dict.init(tablename.m_name, flags);
	df_remote.init(tablename.m_name, flags);

	/* Discover the correct file by looking in three possible locations
	while avoiding unecessary effort. */

	/* We will always look for an ibd in the default location. */
	df_default.make_filepath(NULL, tablename.m_name, IBD);

	/* Look for a filepath embedded in an ISL where the default file
	would be. */
	if (df_remote.open_read_only(true) == DB_SUCCESS) {
		ut_ad(df_remote.is_open());

		/* Always validate a file opened from an ISL pointer */
		validate = true;
		++tablespaces_found;
		link_file_found = true;
	} else if (df_remote.filepath() != NULL) {
		/* An ISL file was found but contained a bad filepath in it.
		Better validate anything we do find. */
		validate = true;
	}

	/* Attempt to open the tablespace at the dictionary filepath. */
	if (path_in) {
		if (df_default.same_filepath_as(path_in)) {
			dict_filepath_same_as_default = true;
		} else {
			/* Dict path is not the default path. Always validate
			remote files. If default is opened, it was moved. */
			validate = true;
			df_dict.set_filepath(path_in);
			if (df_dict.open_read_only(true) == DB_SUCCESS) {
				ut_ad(df_dict.is_open());
				++tablespaces_found;
			}
		}
	}

	const bool operation_not_for_export =
	  srv_operation != SRV_OPERATION_RESTORE_EXPORT
	  && srv_operation != SRV_OPERATION_EXPORT_RESTORED;

	/* Always look for a file at the default location. But don't log
	an error if the tablespace is already open in remote or dict. */
	ut_a(df_default.filepath());
	const bool	strict = operation_not_for_export
	  && (tablespaces_found == 0);
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
	if (tablespaces_found > 1 && df_default.same_as(df_dict)) {
		--tablespaces_found;
		df_dict.close();
	}
	if (tablespaces_found > 1 && df_remote.same_as(df_dict)) {
		--tablespaces_found;
		df_dict.close();
	}

	/*  We have now checked all possible tablespace locations and
	have a count of how many unique files we found.  If things are
	normal, we only found 1. */
	/* For encrypted tablespace, we need to check the
	encryption in header of first page. */
	if (!validate && tablespaces_found == 1) {
		goto skip_validate;
	}

	/* Read and validate the first page of these three tablespace
	locations, if found. */
	valid_tablespaces_found +=
		(df_remote.validate_to_dd(id, flags) == DB_SUCCESS);

	valid_tablespaces_found +=
		(df_default.validate_to_dd(id, flags) == DB_SUCCESS);

	valid_tablespaces_found +=
		(df_dict.validate_to_dd(id, flags) == DB_SUCCESS);

	/* Make sense of these three possible locations.
	First, bail out if no tablespace files were found. */
	if (valid_tablespaces_found == 0) {
		os_file_get_last_error(
		    operation_not_for_export, !operation_not_for_export);
		if (operation_not_for_export)
		  ib::error() << "Could not find a valid tablespace file for `"
		    << tablename << "`. " << TROUBLESHOOT_DATADICT_MSG;
		goto corrupted;
	}
	if (!validate) {
		goto skip_validate;
	}

	/* Do not open any tablespaces if more than one tablespace with
	the correct space ID and flags were found. */
	if (tablespaces_found > 1) {
		ib::error() << "A tablespace for `" << tablename
			<< "` has been found in multiple places;";

		if (df_default.is_open()) {
			ib::error() << "Default location: "
				<< df_default.filepath()
				<< ", Space ID=" << df_default.space_id()
				<< ", Flags=" << df_default.flags();
		}
		if (df_remote.is_open()) {
			ib::error() << "Remote location: "
				<< df_remote.filepath()
				<< ", Space ID=" << df_remote.space_id()
				<< ", Flags=" << df_remote.flags();
		}
		if (df_dict.is_open()) {
			ib::error() << "Dictionary location: "
				<< df_dict.filepath()
				<< ", Space ID=" << df_dict.space_id()
				<< ", Flags=" << df_dict.flags();
		}

		/* Force-recovery will allow some tablespaces to be
		skipped by REDO if there was more than one file found.
		Unlike during the REDO phase of recovery, we now know
		if the tablespace is valid according to the dictionary,
		which was not available then. So if we did not force
		recovery and there is only one good tablespace, ignore
		any bad tablespaces. */
		if (valid_tablespaces_found > 1 || srv_force_recovery > 0) {
			ib::error() << "Will not open tablespace `"
				<< tablename << "`";

			/* If the file is not open it cannot be valid. */
			ut_ad(df_default.is_open() || !df_default.is_valid());
			ut_ad(df_dict.is_open()    || !df_dict.is_valid());
			ut_ad(df_remote.is_open()  || !df_remote.is_valid());

			/* Having established that, this is an easy way to
			look for corrupted data files. */
			if (df_default.is_open() != df_default.is_valid()
			    || df_dict.is_open() != df_dict.is_valid()
			    || df_remote.is_open() != df_remote.is_valid()) {
				goto corrupted;
			}
error:
			if (err) *err = DB_ERROR;
			return NULL;
		}

		/* There is only one valid tablespace found and we did
		not use srv_force_recovery during REDO.  Use this one
		tablespace and clean up invalid tablespace pointers */
		if (df_default.is_open() && !df_default.is_valid()) {
			df_default.close();
			tablespaces_found--;
		}

		if (df_dict.is_open() && !df_dict.is_valid()) {
			df_dict.close();
			/* Leave dict.filepath so that SYS_DATAFILES
			can be corrected below. */
			tablespaces_found--;
		}

		if (df_remote.is_open() && !df_remote.is_valid()) {
			df_remote.close();
			tablespaces_found--;
			link_file_is_bad = true;
		}
	}

	/* At this point, there should be only one filepath. */
	ut_a(tablespaces_found == 1);
	ut_a(valid_tablespaces_found == 1);

	/* Only fix the dictionary at startup when there is only one thread.
	Calls to dict_load_table() can be done while holding other latches. */
	if (!fix_dict) {
		goto skip_validate;
	}

	/* We may need to update what is stored in SYS_DATAFILES or
	SYS_TABLESPACES or adjust the link file.  Since a failure to
	update SYS_TABLESPACES or SYS_DATAFILES does not prevent opening
	and using the tablespace either this time or the next, we do not
	check the return code or fail to open the tablespace. But if it
	fails, dict_update_filepath() will issue a warning to the log. */
	if (df_dict.filepath()) {
		ut_ad(path_in != NULL);
		ut_ad(df_dict.same_filepath_as(path_in));

		if (df_remote.is_open()) {
			if (!df_remote.same_filepath_as(path_in)) {
				dict_update_filepath(id, df_remote.filepath());
			}

		} else if (df_default.is_open()) {
			ut_ad(!dict_filepath_same_as_default);
			dict_update_filepath(id, df_default.filepath());
			if (link_file_is_bad) {
				RemoteDatafile::delete_link_file(
					tablename.m_name);
			}

		} else if (!link_file_found || link_file_is_bad) {
			ut_ad(df_dict.is_open());
			/* Fix the link file if we got our filepath
			from the dictionary but a link file did not
			exist or it did not point to a valid file. */
			RemoteDatafile::delete_link_file(tablename.m_name);
			RemoteDatafile::create_link_file(
				tablename.m_name, df_dict.filepath());
		}

	} else if (df_remote.is_open()) {
		if (dict_filepath_same_as_default) {
			dict_update_filepath(id, df_remote.filepath());

		} else if (path_in == NULL) {
			/* SYS_DATAFILES record for this space ID
			was not found. */
			dict_replace_tablespace_and_filepath(
				id, tablename.m_name,
				df_remote.filepath(), flags);
		}

	} else if (df_default.is_open()) {
		/* We opened the tablespace in the default location.
		SYS_DATAFILES.PATH needs to be updated if it is different
		from this default path or if the SYS_DATAFILES.PATH was not
		supplied and it should have been. Also update the dictionary
		if we found an ISL file (since !df_remote.is_open).  Since
		path_in is not suppled for file-per-table, we must assume
		that it matched the ISL. */
		if ((path_in != NULL && !dict_filepath_same_as_default)
		    || (path_in == NULL && DICT_TF_HAS_DATA_DIR(flags))
		    || df_remote.filepath() != NULL) {
			dict_replace_tablespace_and_filepath(
				id, tablename.m_name, df_default.filepath(),
				flags);
		}
	}

skip_validate:
	const byte* first_page =
		df_default.is_open() ? df_default.get_first_page() :
		df_dict.is_open() ? df_dict.get_first_page() :
		df_remote.get_first_page();

	fil_space_crypt_t* crypt_data = first_page
		? fil_space_read_crypt_data(fil_space_t::zip_size(flags),
					    first_page)
		: NULL;

	fil_space_t* space = fil_space_t::create(
		tablename.m_name, id, flags, purpose, crypt_data);
	if (!space) {
		goto error;
	}

	/* We do not measure the size of the file, that is why
	we pass the 0 below */

	space->add(
		df_remote.is_open() ? df_remote.filepath() :
		df_dict.is_open() ? df_dict.filepath() :
		df_default.filepath(), OS_FILE_CLOSED, 0, false, true);

	if (validate && !srv_read_only_mode) {
		df_remote.close();
		df_dict.close();
		df_default.close();
		if (space->acquire()) {
			if (purpose != FIL_TYPE_IMPORT) {
				fsp_flags_try_adjust(space, flags
						     & ~FSP_FLAGS_MEM_MASK);
			}
			space->release();
		}
	}

	if (err) *err = DB_SUCCESS;
	return space;
}

/** Looks for a pre-existing fil_space_t with the given tablespace ID
and, if found, returns the name and filepath in newly allocated buffers
that the caller must free.
@param[in]	space_id	The tablespace ID to search for.
@param[out]	name		Name of the tablespace found.
@param[out]	filepath	The filepath of the first datafile for the
tablespace.
@return true if tablespace is found, false if not. */
bool
fil_space_read_name_and_filepath(
	ulint	space_id,
	char**	name,
	char**	filepath)
{
	bool	success = false;
	*name = NULL;
	*filepath = NULL;

	mutex_enter(&fil_system.mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space != NULL) {
		*name = mem_strdup(space->name);

		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		*filepath = mem_strdup(node->name);

		success = true;
	}

	mutex_exit(&fil_system.mutex);

	return(success);
}

/** Convert a file name to a tablespace name.
@param[in]	filename	directory/databasename/tablename.ibd
@return database/tablename string, to be freed with ut_free() */
char*
fil_path_to_space_name(
	const char*	filename)
{
	/* Strip the file name prefix and suffix, leaving
	only databasename/tablename. */
	ulint		filename_len	= strlen(filename);
	const char*	end		= filename + filename_len;
#ifdef HAVE_MEMRCHR
	const char*	tablename	= 1 + static_cast<const char*>(
		memrchr(filename, OS_PATH_SEPARATOR,
			filename_len));
	const char*	dbname		= 1 + static_cast<const char*>(
		memrchr(filename, OS_PATH_SEPARATOR,
			tablename - filename - 1));
#else /* HAVE_MEMRCHR */
	const char*	tablename	= filename;
	const char*	dbname		= NULL;

	while (const char* t = static_cast<const char*>(
		       memchr(tablename, OS_PATH_SEPARATOR,
			      ulint(end - tablename)))) {
		dbname = tablename;
		tablename = t + 1;
	}
#endif /* HAVE_MEMRCHR */

	ut_ad(dbname != NULL);
	ut_ad(tablename > dbname);
	ut_ad(tablename < end);
	ut_ad(end - tablename > 4);
	ut_ad(memcmp(end - 4, DOT_IBD, 4) == 0);

	char*	name = mem_strdupl(dbname, ulint(end - dbname) - 4);

	ut_ad(name[tablename - dbname - 1] == OS_PATH_SEPARATOR);
#if OS_PATH_SEPARATOR != '/'
	/* space->name uses '/', not OS_PATH_SEPARATOR. */
	name[tablename - dbname - 1] = '/';
#endif

	return(name);
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
		if (db[0] == OS_PATH_SEPARATOR) {
			sep_found++;
		}
	}
	if (sep_found == 2) {
		db += 2;
		df_def_per.init(db, 0);
		df_def_per.make_filepath(NULL, db, IBD);
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
			ut_ad(0);
			break;
		case SRV_OPERATION_RESTORE_EXPORT:
		case SRV_OPERATION_RESTORE:
			break;
		case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_EXPORT_RESTORED:
			df_rem_per.set_name(db);
			if (df_rem_per.open_link_file() != DB_SUCCESS) {
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
			MLOG record. */
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
fil_ibd_load(
	ulint		space_id,
	const char*	filename,
	fil_space_t*&	space)
{
	/* If the a space is already in the file system cache with this
	space ID, then there is nothing to do. */
	mutex_enter(&fil_system.mutex);
	space = fil_space_get_by_id(space_id);
	mutex_exit(&fil_system.mutex);

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
		if (const char* name = strrchr(filename, OS_PATH_SEPARATOR)) {
			while (--name > filename
			       && *name != OS_PATH_SEPARATOR);
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

	/* Read and validate the first page of the tablespace.
	Assign a tablespace name based on the tablespace type. */
	switch (file.validate_for_recovery()) {
		os_offset_t	minimum_size;
	case DB_SUCCESS:
		if (file.space_id() != space_id) {
			return(FIL_LOAD_ID_CHANGED);
		}
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
	ulint flags = file.flags();
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
		file.name(), space_id, flags, FIL_TYPE_TABLESPACE, crypt_data);

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
void fsp_flags_try_adjust(fil_space_t* space, ulint flags)
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
		uint32_t f = fsp_header_get_flags(b->frame);
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
					   + b->frame, flags);
	}
func_exit:
	mtr.commit();
}

/** Determine if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id		Tablespace ID
@param[in]	name		Tablespace name used in fil_space_t::create().
@param[in]	table_flags	table flags
@return the tablespace
@retval	NULL	if no matching tablespace exists in the memory cache */
fil_space_t*
fil_space_for_table_exists_in_mem(
	ulint		id,
	const char*	name,
	ulint		table_flags)
{
	const ulint	expected_flags = dict_tf_to_fsp_flags(table_flags);

	mutex_enter(&fil_system.mutex);
	if (fil_space_t* space = fil_space_get_by_id(id)) {
		ulint tf = expected_flags & ~FSP_FLAGS_MEM_MASK;
		ulint sf = space->flags & ~FSP_FLAGS_MEM_MASK;

		if (!fil_space_t::is_flags_equal(tf, sf)
		    && !fil_space_t::is_flags_equal(sf, tf)) {
			goto func_exit;
		}

		if (strcmp(space->name, name)) {
			ib::error() << "Table " << name
				<< " in InnoDB data dictionary"
				" has tablespace id " << id
				<< ", but the tablespace"
				" with that id has name " << space->name << "."
				" Have you deleted or moved .ibd files?";
			ib::info() << TROUBLESHOOT_DATADICT_MSG;
			goto func_exit;
		}

		/* Adjust the flags that are in FSP_FLAGS_MEM_MASK.
		FSP_SPACE_FLAGS will not be written back here. */
		space->flags = (space->flags & ~FSP_FLAGS_MEM_MASK)
			| (expected_flags & FSP_FLAGS_MEM_MASK);
		mutex_exit(&fil_system.mutex);
		if (!srv_read_only_mode) {
			fsp_flags_try_adjust(space, expected_flags
					     & ~FSP_FLAGS_MEM_MASK);
		}
		return space;
	}

func_exit:
	mutex_exit(&fil_system.mutex);
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
  ut_ad(!mutex_own(&fil_system.mutex));

  if (space->purpose != FIL_TYPE_TEMPORARY &&
      srv_file_flush_method != SRV_O_DIRECT_NO_FSYNC &&
      space->set_needs_flush())
  {
    mutex_enter(&fil_system.mutex);
    if (!space->is_in_unflushed_spaces)
    {
      space->is_in_unflushed_spaces= true;
      fil_system.unflushed_spaces.push_front(*space);
    }
    mutex_exit(&fil_system.mutex);
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
	ut_ad(offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad((len % OS_FILE_LOG_BLOCK_SIZE) == 0);
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

	if (type.type == IORequest::READ_ASYNC && is_stopping()
	    && !is_being_truncated) {
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
				if (type.type == IORequest::READ_ASYNC) {
					release();
					return {DB_ERROR, nullptr};
				}

				fatal = true;
fail:
				fil_invalid_page_access_msg(fatal, node->name,
							    offset, len,
							    type.is_read());
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
			punch_hole = false;
			err = DB_SUCCESS;
		}
		goto release_sync_write;
	} else {
		/* Queue the aio request */
		err = os_aio(IORequest(bpage, node, type.type),
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
    request.node->complete_write();
    ut_ad(!srv_read_only_mode);
    if (request.type == IORequest::DBLWR_BATCH)
      buf_dblwr.flush_buffered_writes_completed(request);
    else
      ut_ad(request.type == IORequest::WRITE_ASYNC);
    request.node->complete_write();
  }
  else if (request.is_write())
  {
    request.node->complete_write();
    buf_page_write_complete(request);
  }
  else
  {
    ut_ad(request.is_read());

    /* IMPORTANT: since i/o handling for reads will read also the insert
    buffer in fil_system.sys_space, we have to be very careful not to
    introduce deadlocks. We never close fil_system.sys_space data
    files and never issue asynchronous reads of change buffer pages. */
    const page_id_t id(request.bpage->id());

    if (dberr_t err= buf_page_read_complete(request.bpage, *request.node))
    {
      if (recv_recovery_is_on() && !srv_force_recovery)
        recv_sys.found_corrupt_fs= true;

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
    ut_d(mutex_enter(&fil_system.mutex));
    ut_ad(fil_system.unflushed_spaces.empty());
    ut_d(mutex_exit(&fil_system.mutex));
    return;
  }

rescan:
  mutex_enter(&fil_system.mutex);

  for (fil_space_t &space : fil_system.unflushed_spaces)
  {
    if (space.needs_flush_not_stopping())
    {
      space.reacquire();
      mutex_exit(&fil_system.mutex);
      space.flush_low();
      space.release();
      goto rescan;
    }
  }

  mutex_exit(&fil_system.mutex);
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
		ut_ad(mutex_own(&fil_system.mutex));
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

	mutex_enter(&fil_system.mutex);

	for (fil_space_t *space = UT_LIST_GET_FIRST(fil_system.space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {
		n_open += Check::validate(space);
	}

	ut_a(fil_system.n_open == n_open);

	mutex_exit(&fil_system.mutex);

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
void
fil_delete_file(
/*============*/
	const char*	ibd_filepath)
{
	/* Force a delete of any stale .ibd files that are lying around. */

	ib::info() << "Deleting " << ibd_filepath;
	os_file_delete_if_exists(innodb_data_file_key, ibd_filepath, NULL);

	char*	cfg_filepath = fil_make_filepath(
		ibd_filepath, NULL, CFG, false);
	if (cfg_filepath != NULL) {
		os_file_delete_if_exists(
			innodb_data_file_key, cfg_filepath, NULL);
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
	ut_ad(!mutex_own(&fil_system.mutex));
	ut_ad(space != NULL);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);
	ut_ad(!is_predefined_tablespace(space->id));

	/* We are serving mtr_commit(). While there is an active
	mini-transaction, we should have !space->stop_new_ops. This is
	guaranteed by meta-data locks or transactional locks, or
	dict_sys.latch (X-lock in DROP, S-lock in purge). */
	ut_ad(!space->is_stopping()
	      || space->is_being_truncated /* fil_truncate_prepare() */
	      || space->referenced());
}
#endif /* UNIV_DEBUG */

/** Write a FILE_MODIFY record for a persistent tablespace.
@param[in]	space	tablespace
@param[in,out]	mtr	mini-transaction */
static
void
fil_names_write(
	const fil_space_t*	space,
	mtr_t*			mtr)
{
	ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
	fil_name_write(space->id, UT_LIST_GET_FIRST(space->chain)->name, mtr);
}

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space)
{
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(recv_recovery_is_on());
	ut_ad(log_sys.get_lsn() != 0);
	ut_ad(space->max_lsn == 0);
	ut_d(fil_space_validate_for_mtr_commit(space));

	UT_LIST_ADD_LAST(fil_system.named_spaces, space);
	space->max_lsn = log_sys.get_lsn();
}

/** Write FILE_MODIFY records when a non-predefined persistent
tablespace was modified for the first time since the latest
fil_names_clear().
@param[in,out]	space	tablespace */
void fil_names_dirty_and_write(fil_space_t* space)
{
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_d(fil_space_validate_for_mtr_commit(space));
	ut_ad(space->max_lsn == log_sys.get_lsn());

	UT_LIST_ADD_LAST(fil_system.named_spaces, space);
	mtr_t mtr;
	mtr.start();
	fil_names_write(space, &mtr);

	DBUG_EXECUTE_IF("fil_names_write_bogus",
			{
				char bogus_name[] = "./test/bogus file.ibd";
				os_normalize_path(bogus_name);
				fil_name_write(
					SRV_SPACE_ID_UPPER_BOUND,
					bogus_name, &mtr);
			});

	mtr.commit_files();
}

/** On a log checkpoint, reset fil_names_dirty_and_write() flags
and write out FILE_MODIFY and FILE_CHECKPOINT if needed.
@param[in]	lsn		checkpoint LSN
@param[in]	do_write	whether to always write FILE_CHECKPOINT
@return whether anything was written to the redo log
@retval false	if no flags were set and nothing written
@retval true	if anything was written to the redo log */
bool
fil_names_clear(
	lsn_t	lsn,
	bool	do_write)
{
	mtr_t	mtr;

	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(lsn);

	mtr.start();

	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system.named_spaces);
	     space != NULL; ) {
		if (mtr.get_log_size()
		    + strlen(space->chain.start->name)
		    >= RECV_SCAN_SIZE - (3 + 5 + 1)) {
			/* Prevent log parse buffer overflow */
			mtr.commit_files();
			mtr.start();
		}

		fil_space_t*	next = UT_LIST_GET_NEXT(named_spaces, space);

		ut_ad(space->max_lsn > 0);
		if (space->max_lsn < lsn) {
			/* The tablespace was last dirtied before the
			checkpoint LSN. Remove it from the list, so
			that if the tablespace is not going to be
			modified any more, subsequent checkpoints will
			avoid calling fil_names_write() on it. */
			space->max_lsn = 0;
			UT_LIST_REMOVE(fil_system.named_spaces, space);
		}

		/* max_lsn is the last LSN where fil_names_dirty_and_write()
		was called. If we kept track of "min_lsn" (the first LSN
		where max_lsn turned nonzero), we could avoid the
		fil_names_write() call if min_lsn > lsn. */

		fil_names_write(space, &mtr);
		do_write = true;

		space = next;
	}

	if (do_write) {
		mtr.commit_files(lsn);
	} else {
		ut_ad(!mtr.has_modifications());
	}

	return(do_write);
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
UNIV_INTERN
ulint
fil_space_get_block_size(const fil_space_t* space, unsigned offset)
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
