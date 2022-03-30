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
#include "btr0sea.h"
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
#include "row0trunc.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "buf0lru.h"
#include "ibuf0ibuf.h"
#include "os0event.h"
#include "sync0sync.h"
#include "buf0flu.h"
#include "os0api.h"

/** Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex.
@return true if success, false if should retry later; since i/o's
generally complete in < 100 ms, and as InnoDB writes at most 128 pages
from the buffer pool in a batch, and then immediately flushes the
files, there is a good chance that the next time we find a suitable
node from the LRU list.
@param[in] print_info	if true, prints information why it
                        cannot close a file */
static
bool
fil_try_to_close_file_in_LRU(bool print_info);

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
identifier.

Some operating systems do not support many open files at the same time,
though NT seems to tolerate at least 900 open files. Therefore, we put the
open files in an LRU-list. If we need to open another file, we may close the
file at the end of the LRU-list. When an i/o-operation is pending on a file,
the file cannot be closed. We take the file nodes with pending i/o-operations
out of the LRU-list and keep a count of pending operations. When an operation
completes, we decrement the count and return the file node to the LRU-list if
the count drops to zero. */

/** Reference to the server data directory. Usually it is the
current working directory ".", but in the MySQL Embedded Server Library
it is an absolute path. */
const char*	fil_path_to_mysql_datadir;

/** Common InnoDB file extentions */
const char* dot_ext[] = { "", ".ibd", ".isl", ".cfg" };

/** The number of fsyncs done to the log */
ulint	fil_n_log_flushes			= 0;

/** Number of pending redo log flushes */
ulint	fil_n_pending_log_flushes		= 0;
/** Number of pending tablespace flushes */
ulint	fil_n_pending_tablespace_flushes	= 0;

/** The null file address */
fil_addr_t	fil_addr_null = {FIL_NULL, 0};

/** The tablespace memory cache. This variable is NULL before the module is
initialized. */
UNIV_INTERN fil_system_t*	fil_system	= NULL;

/** At this age or older a space/page will be rotated */
UNIV_INTERN extern uint srv_fil_crypt_rotate_key_age;
UNIV_INTERN extern ib_mutex_t fil_crypt_threads_mutex;

/** Determine if user has explicitly disabled fsync(). */
# define fil_buffering_disabled(s)	\
	((s)->purpose == FIL_TYPE_TABLESPACE	\
	 && srv_file_flush_method	\
	 == SRV_O_DIRECT_NO_FSYNC)

/** Determine if the space id is a user tablespace id or not.
@param[in]	space_id	Space ID to check
@return true if it is a user tablespace ID */
inline
bool
fil_is_user_tablespace_id(ulint space_id)
{
	return(space_id != TRX_SYS_SPACE
	       && space_id != SRV_TMP_SPACE_ID
	       && !srv_is_undo_tablespace(space_id));
}

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
	/** The fil_validate() call skip counter. Use a signed type
	because of the race condition below. */
	static int fil_validate_count = FIL_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly fil_validate() check
	in debug builds. */
	if (--fil_validate_count > 0) {
		return(true);
	}

	fil_validate_count = FIL_VALIDATE_SKIP;
	return(fil_validate());
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Determines if a file node belongs to the least-recently-used list.
@return true if the file belongs to fil_system->LRU mutex. */
UNIV_INLINE
bool
fil_space_belongs_in_lru(
/*=====================*/
	const fil_space_t*	space)	/*!< in: file space */
{
	switch (space->purpose) {
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_LOG:
		return(false);
	case FIL_TYPE_TABLESPACE:
		return(fil_is_user_tablespace_id(space->id));
	case FIL_TYPE_IMPORT:
		return(true);
	}

	ut_ad(0);
	return(false);
}

/********************************************************************//**
NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file node for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the node and the system appropriately. Takes the node
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
/*====================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space);	/*!< in: space */

/** Update the data structures when an i/o operation finishes.
@param[in,out] node		file node
@param[in] type			IO context */
static
void
fil_node_complete_io(fil_node_t* node, const IORequest& type);

/** Reads data from a space to a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read; this must not cross a
file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store data read; in aio this
must be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_read(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	return(fil_io(IORequestRead, true, page_id, page_size,
			byte_offset, len, buf, NULL));
}

/** Writes data to a space from a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to write; this must not cross
a file boundary; in aio this must be a block size multiple
@param[in]	buf		buffer from which to write; in aio this must
be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_write(
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	ut_ad(!srv_read_only_mode);

	return(fil_io(IORequestWrite, true, page_id, page_size,
		      byte_offset, len, buf, NULL));
}

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

	ut_ad(mutex_own(&fil_system->mutex));

	HASH_SEARCH(hash, fil_system->spaces, id,
		    fil_space_t*, space,
		    ut_ad(space->magic_n == FIL_SPACE_MAGIC_N),
		    space->id == id);

	return(space);
}

/** Look up a tablespace.
@param[in]	name	tablespace name
@return	tablespace
@retval	NULL	if not found */
fil_space_t*
fil_space_get_by_name(const char* name)
{
	fil_space_t*	space;
	ulint		fold;

	ut_ad(mutex_own(&fil_system->mutex));

	fold = ut_fold_string(name);

	HASH_SEARCH(name_hash, fil_system->name_hash, fold,
		    fil_space_t*, space,
		    ut_ad(space->magic_n == FIL_SPACE_MAGIC_N),
		    !strcmp(name, space->name));

	return(space);
}

/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
If this is not the case, fil_space_acquire() and fil_space_release()
should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or NULL if not found */
fil_space_t*
fil_space_get(
	ulint	id)
{
	mutex_enter(&fil_system->mutex);
	fil_space_t*	space = fil_space_get_by_id(id);
	mutex_exit(&fil_system->mutex);
	ut_ad(space == NULL || space->purpose != FIL_TYPE_LOG);
	return(space);
}

/** Gets the type of a file space.
@param[in]	id	tablespace identifier
@return file type */
fil_type_t
fil_space_get_type(
	ulint	id)
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	mutex_exit(&fil_system->mutex);

	return(space->purpose);
}

/** Note that a tablespace has been imported.
It is initially marked as FIL_TYPE_IMPORT so that no logging is
done during the import process when the space ID is stamped to each page.
Now we change it to FIL_SPACE_TABLESPACE to start redo and undo logging.
NOTE: temporary tablespaces are never imported.
@param[in]	id	tablespace identifier */
void
fil_space_set_imported(
	ulint	id)
{
	ut_ad(fil_system != NULL);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);
	const fil_node_t* node = UT_LIST_GET_FIRST(space->chain);

	ut_ad(space->purpose == FIL_TYPE_IMPORT);
	space->purpose = FIL_TYPE_TABLESPACE;
	space->atomic_write_supported = node->atomic_write
		&& srv_use_atomic_writes
		&& my_test_if_atomic_write(node->handle,
					   int(page_size_t(space->flags)
					       .physical()));
	mutex_exit(&fil_system->mutex);
}

/**********************************************************************//**
Checks if all the file nodes in a space are flushed. The caller must hold
the fil_system mutex.
@return true if all are flushed */
static
bool
fil_space_is_flushed(
/*=================*/
	fil_space_t*	space)	/*!< in: space */
{
	ut_ad(mutex_own(&fil_system->mutex));

	for (const fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		if (node->needs_flush) {

			ut_ad(!fil_buffering_disabled(space));
			return(false);
		}
	}

	return(true);
}


/** Append a file to the chain of files of a space.
@param[in]	name		file name of a file that is not open
@param[in]	handle		file handle, or OS_FILE_CLOSED
@param[in]	size		file size in entire database pages
@param[in]	is_raw		whether this is a raw device
@param[in]	atomic_write	true if atomic write could be enabled
@param[in]	max_pages	maximum number of pages in file,
or ULINT_MAX for unlimited
@return file object */
fil_node_t* fil_space_t::add(const char* name, pfs_os_file_t handle,
			     ulint size, bool is_raw, bool atomic_write,
			     ulint max_pages)
{
	fil_node_t*	node;

	ut_ad(name != NULL);
	ut_ad(fil_system != NULL);

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

	mutex_enter(&fil_system->mutex);
	this->size += size;
	UT_LIST_ADD_LAST(chain, node);
	if (node->is_open()) {
		fil_system->n_open++;
	}
	mutex_exit(&fil_system->mutex);

	return node;
}

/** Read the first page of a data file.
@param[in]	first	whether this is the very first read
@return	whether the page was found valid */
bool fil_node_t::read_page0(bool first)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(space->purpose != FIL_TYPE_LOG);
	const page_size_t page_size(space->flags);
	const ulint psize = page_size.physical();

	os_offset_t size_bytes = os_file_get_size(handle);
	ut_a(size_bytes != (os_offset_t) -1);
	const ulint min_size = FIL_IBD_FILE_INITIAL_SIZE * psize;

	if (size_bytes < min_size) {
		ib::error() << "The size of the file " << name
			    << " is only " << size_bytes
			    << " bytes, should be at least " << min_size;
		return false;
	}

	byte* buf2 = static_cast<byte*>(ut_malloc_nokey(2 * psize));

	/* Align the memory for file i/o if we might have O_DIRECT set */
	byte* page = static_cast<byte*>(ut_align(buf2, psize));
	IORequest request(IORequest::READ);
	if (os_file_read(request, handle, page, 0, psize) != DB_SUCCESS) {
		ib::error() << "Unable to read first page of file " << name;
		ut_free(buf2);
		return false;
	}
	srv_stats.page0_read.add(1);
	const ulint space_id = fsp_header_get_space_id(page);
	ulint flags = fsp_header_get_flags(page);
	const ulint size = fsp_header_get_field(page, FSP_SIZE);
	const ulint free_limit = fsp_header_get_field(page, FSP_FREE_LIMIT);
	const ulint free_len = flst_get_len(FSP_HEADER_OFFSET + FSP_FREE
					    + page);
	/* Try to read crypt_data from page 0 if it is not yet read. */
	if (!space->crypt_data) {
		space->crypt_data = fil_space_read_crypt_data(page_size, page);
	}
	ut_free(buf2);

	if (!fsp_flags_is_valid(flags, space->id)) {
		ulint cflags = fsp_flags_convert_from_101(flags);
		if (cflags == ULINT_UNDEFINED
		    || (cflags ^ space->flags) & ~FSP_FLAGS_MEM_MASK) {
			ib::error()
				<< "Expected tablespace flags "
				<< ib::hex(space->flags)
				<< " but found " << ib::hex(flags)
				<< " in the file " << name;
			return false;
		}

		flags = cflags;
	}

	if (UNIV_UNLIKELY(space_id != space->id)) {
		ib::error() << "Expected tablespace id " << space->id
			<< " but found " << space_id
			<< " in the file " << name;
		return false;
	}

	if (first) {
		ut_ad(space->id != TRX_SYS_SPACE);

		/* Truncate the size to a multiple of extent size. */
		ulint	mask = psize * FSP_EXTENT_SIZE - 1;

		if (size_bytes <= mask) {
			/* .ibd files start smaller than an
			extent size. Do not truncate valid data. */
		} else {
			size_bytes &= ~os_offset_t(mask);
		}

		this->size = ulint(size_bytes / psize);
		space->committed_size = space->size += this->size;

		if (block_size == 0) {
			block_size = os_file_get_block_size(handle, name);
		}
	} else if (space->id != TRX_SYS_SPACE || space->size_in_header) {
		/* If this is not the first-time open, do nothing.
		For the system tablespace, we always get invoked as
		first=false, so we detect the true first-time-open based
		on size_in_header and proceed to initialize the data. */
		return true;
	} else {
		/* Initialize the size of predefined tablespaces
		to FSP_SIZE. */
		space->committed_size = size;
	}

	ut_ad(space->free_limit == 0 || space->free_limit == free_limit);
	ut_ad(space->free_len == 0 || space->free_len == free_len);
	space->size_in_header = size;
	space->free_limit = free_limit;
	space->free_len = free_len;
	return true;
}

/** Open a file node of a tablespace.
The caller must own the fil_system mutex.
@param[in,out]	node	File node
@return false if the file can't be opened, otherwise true */
static bool fil_node_open_file(fil_node_t* node)
{
	bool		success;
	bool		read_only_mode;
	fil_space_t*	space = node->space;

	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(node->n_pending == 0);
	ut_a(!node->is_open());

	read_only_mode = space->purpose != FIL_TYPE_TEMPORARY
		&& srv_read_only_mode;

	const bool first_time_open = node->size == 0;

	ulint type;
	compile_time_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096);
	switch (FSP_FLAGS_GET_ZIP_SSIZE(space->flags)) {
	case 1:
	case 2:
		type = OS_DATA_FILE_NO_O_DIRECT;
		break;
	default:
		type = OS_DATA_FILE;
	}

	if (first_time_open
	    || (space->purpose == FIL_TYPE_TABLESPACE
		&& node == UT_LIST_GET_FIRST(space->chain)
		&& srv_startup_is_before_trx_rollback_phase
		&& !undo::Truncate::was_tablespace_truncated(space->id))) {
		/* We do not know the size of the file yet. First we
		open the file in the normal mode, no async I/O here,
		for simplicity. Then do some checks, and close the
		file again.  NOTE that we could not use the simple
		file read function os_file_read() in Windows to read
		from a file opened for async I/O! */

retry:
		node->handle = os_file_create(
			innodb_data_file_key, node->name,
			node->is_raw_disk
			? OS_FILE_OPEN_RAW | OS_FILE_ON_ERROR_NO_EXIT
			: OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
			OS_FILE_AIO,
			type,
			read_only_mode,
			&success);

		if (!success) {
			/* The following call prints an error message */
			ulint err = os_file_get_last_error(true);
			if (err == EMFILE + 100) {
				if (fil_try_to_close_file_in_LRU(true))
					goto retry;
			}

			ib::warn() << "Cannot open '" << node->name << "'."
				" Have you deleted .ibd files under a"
				" running mysqld server?";
			return(false);
		}

		if (!node->read_page0(first_time_open)) {
			os_file_close(node->handle);
			node->handle = OS_FILE_CLOSED;
			return false;
		}
	} else if (space->purpose == FIL_TYPE_LOG) {
		node->handle = os_file_create(
			innodb_log_file_key, node->name, OS_FILE_OPEN,
			OS_FILE_AIO, OS_LOG_FILE, read_only_mode, &success);
	} else {
		node->handle = os_file_create(
			innodb_data_file_key, node->name,
			node->is_raw_disk
			? OS_FILE_OPEN_RAW | OS_FILE_ON_ERROR_NO_EXIT
			: OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
			OS_FILE_AIO,
			type,
			read_only_mode,
			&success);
	}

	if (space->purpose != FIL_TYPE_LOG) {
		/*
		For the temporary tablespace and during the
		non-redo-logged adjustments in
		IMPORT TABLESPACE, we do not care about
		the atomicity of writes.

		Atomic writes is supported if the file can be used
		with atomic_writes (not log file), O_DIRECT is
		used (tested in ha_innodb.cc) and the file is
		device and file system that supports atomic writes
		for the given block size
		*/
		space->atomic_write_supported
			= space->purpose == FIL_TYPE_TEMPORARY
			|| space->purpose == FIL_TYPE_IMPORT
			|| (node->atomic_write
			    && srv_use_atomic_writes
			    && my_test_if_atomic_write(
				    node->handle,
				    int(page_size_t(space->flags)
					.physical())));
	}

	ut_a(success);
	ut_a(node->is_open());

	fil_system->n_open++;

	if (fil_space_belongs_in_lru(space)) {

		/* Put the node to the LRU list */
		UT_LIST_ADD_FIRST(fil_system->LRU, node);
	}

	return(true);
}

/** Close a file node.
@param[in,out]	node	File node */
static
void
fil_node_close_file(
	fil_node_t*	node)
{
	bool	ret;

	ut_ad(mutex_own(&(fil_system->mutex)));
	ut_a(node->is_open());
	ut_a(node->n_pending == 0);
	ut_a(node->n_pending_flushes == 0);
	ut_a(!node->being_extended);
	ut_a(!node->needs_flush
	     || node->space->purpose == FIL_TYPE_TEMPORARY
	     || srv_fast_shutdown == 2
	     || !srv_was_started);

	ret = os_file_close(node->handle);
	ut_a(ret);

	/* printf("Closing file %s\n", node->name); */

	node->handle = OS_FILE_CLOSED;
	ut_ad(!node->is_open());
	ut_a(fil_system->n_open > 0);
	fil_system->n_open--;

	if (fil_space_belongs_in_lru(node->space)) {

		ut_a(UT_LIST_GET_LEN(fil_system->LRU) > 0);

		/* The node is in the LRU list, remove it */
		UT_LIST_REMOVE(fil_system->LRU, node);
	}
}

/** Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex.
@return true if success, false if should retry later; since i/o's
generally complete in < 100 ms, and as InnoDB writes at most 128 pages
from the buffer pool in a batch, and then immediately flushes the
files, there is a good chance that the next time we find a suitable
node from the LRU list.
@param[in] print_info	if true, prints information why it
			cannot close a file*/
static
bool
fil_try_to_close_file_in_LRU(

	bool	print_info)
{
	fil_node_t*	node;

	ut_ad(mutex_own(&fil_system->mutex));

	if (print_info) {
		ib::info() << "fil_sys open file LRU len "
			<< UT_LIST_GET_LEN(fil_system->LRU);
	}

	for (node = UT_LIST_GET_LAST(fil_system->LRU);
	     node != NULL;
	     node = UT_LIST_GET_PREV(LRU, node)) {

		if (!node->needs_flush
		    && node->n_pending_flushes == 0
		    && !node->being_extended) {

			fil_node_close_file(node);

			return(true);
		}

		if (!print_info) {
			continue;
		}

		if (node->n_pending_flushes > 0) {

			ib::info() << "Cannot close file " << node->name
				<< ", because n_pending_flushes "
				<< node->n_pending_flushes;
		}

		if (node->needs_flush) {
			ib::warn() << "Cannot close file " << node->name
				<< ", because is should be flushed first";
		}

		if (node->being_extended) {
			ib::info() << "Cannot close file " << node->name
				<< ", because it is being extended";
		}
	}

	return(false);
}

/** Flush any writes cached by the file system.
@param[in,out]	space		tablespace
@param[in]	metadata	whether to update file system metadata */
static void fil_flush_low(fil_space_t* space, bool metadata = false)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_ad(space);
	ut_ad(!space->stop_new_ops);

	if (fil_buffering_disabled(space)) {

		/* No need to flush. User has explicitly disabled
		buffering. */
		ut_ad(!space->is_in_unflushed_spaces);
		ut_ad(fil_space_is_flushed(space));
		ut_ad(space->n_pending_flushes == 0);

#ifdef UNIV_DEBUG
		for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {
			ut_ad(!node->needs_flush);
			ut_ad(node->n_pending_flushes == 0);
		}
#endif /* UNIV_DEBUG */

		if (!metadata) return;
	}

	/* Prevent dropping of the space while we are flushing */
	space->n_pending_flushes++;

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		if (!node->needs_flush) {
			continue;
		}

		ut_a(node->is_open());

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			ut_ad(0); // we already checked for this
		case FIL_TYPE_TABLESPACE:
		case FIL_TYPE_IMPORT:
			fil_n_pending_tablespace_flushes++;
			break;
		case FIL_TYPE_LOG:
			fil_n_pending_log_flushes++;
			fil_n_log_flushes++;
			break;
		}
#ifdef _WIN32
		if (node->is_raw_disk) {

			goto skip_flush;
		}
#endif /* _WIN32 */

		ut_a(node->is_open());
		node->n_pending_flushes++;
		node->needs_flush = false;

		mutex_exit(&fil_system->mutex);

		os_file_flush(node->handle);

		mutex_enter(&fil_system->mutex);

		node->n_pending_flushes--;
#ifdef _WIN32
skip_flush:
#endif /* _WIN32 */
		if (!node->needs_flush) {
			if (space->is_in_unflushed_spaces
			    && fil_space_is_flushed(space)) {

				fil_system->unflushed_spaces.remove(*space);
				space->is_in_unflushed_spaces = false;
			}
		}

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			break;
		case FIL_TYPE_TABLESPACE:
		case FIL_TYPE_IMPORT:
			fil_n_pending_tablespace_flushes--;
			continue;
		case FIL_TYPE_LOG:
			fil_n_pending_log_flushes--;
			continue;
		}

		ut_ad(0);
	}

	space->n_pending_flushes--;
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
	ulint		size,
	bool*		success)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_ad(UT_LIST_GET_LAST(space->chain) == node);
	ut_ad(size >= FIL_IBD_FILE_INITIAL_SIZE);

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
		mutex_exit(&fil_system->mutex);
		os_thread_sleep(100000);
		return(true);
	}

	node->being_extended = true;

	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		/* The tablespace data file, such as .ibd file, is missing */
		node->being_extended = false;
		return(false);
	}

	/* At this point it is safe to release fil_system mutex. No
	other thread can rename, delete, close or extend the file because
	we have set the node->being_extended flag. */
	mutex_exit(&fil_system->mutex);

	ut_ad(size >= space->size);

	ulint		last_page_no		= space->size;
	const ulint	file_start_page_no	= last_page_no - node->size;

	/* Determine correct file block size */
	if (node->block_size == 0) {
		node->block_size = os_file_get_block_size(
			node->handle, node->name);
	}

	const page_size_t	pageSize(space->flags);
	const ulint		page_size = pageSize.physical();

	/* fil_read_first_page() expects innodb_page_size bytes.
	fil_node_open_file() expects at least 4 * innodb_page_size bytes.
	os_file_set_size() expects multiples of 4096 bytes.
	For ROW_FORMAT=COMPRESSED tables using 1024-byte or 2048-byte
	pages, we will preallocate up to an integer multiple of 4096 bytes,
	and let normal writes append 1024, 2048, or 3072 bytes to the file. */
	os_offset_t new_size = std::max(
		(os_offset_t(size - file_start_page_no) * page_size)
		& ~os_offset_t(4095),
		os_offset_t(FIL_IBD_FILE_INITIAL_SIZE << srv_page_size_shift));

	*success = os_file_set_size(node->name, node->handle, new_size,
		FSP_FLAGS_HAS_PAGE_COMPRESSION(space->flags));

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

		last_page_no = ulint(fsize / page_size)
			+ file_start_page_no;
	}
	mutex_enter(&fil_system->mutex);

	ut_a(node->being_extended);
	node->being_extended = false;
	ut_a(last_page_no - file_start_page_no >= node->size);

	ulint file_size = last_page_no - file_start_page_no;
	space->size += file_size - node->size;
	node->size = file_size;
	const ulint pages_in_MiB = node->size
		& ~((1 << (20 - UNIV_PAGE_SIZE_SHIFT)) - 1);

	fil_node_complete_io(node,IORequestRead);

	/* Keep the last data file size info up to date, rounded to
	full megabytes */

	switch (space->id) {
	case TRX_SYS_SPACE:
		srv_sys_space.set_last_file_size(pages_in_MiB);
		fil_flush_low(space, true);
		return(false);
	default:
		ut_ad(space->purpose == FIL_TYPE_TABLESPACE
		      || space->purpose == FIL_TYPE_IMPORT);
		if (space->purpose == FIL_TYPE_TABLESPACE
		    && !space->is_being_truncated) {
			fil_flush_low(space, true);
		}
		return(false);
	case SRV_TMP_SPACE_ID:
		ut_ad(space->purpose == FIL_TYPE_TEMPORARY);
		srv_tmp_space.set_last_file_size(pages_in_MiB);
		return(false);
	}

}

/** Reserves the fil_system mutex and tries to make sure we can open at least one
file while holding it. This should be called before calling
fil_node_prepare_for_io(), because that function may need to open a file.
@param[in]	space_id	tablespace id
@return whether the tablespace is usable for io */
static
bool
fil_mutex_enter_and_prepare_for_io(ulint space_id)
{
	for (ulint count = 0;;) {
		mutex_enter(&fil_system->mutex);

		if (space_id >= SRV_LOG_SPACE_FIRST_ID) {
			/* We keep log files always open. */
			break;
		}

		fil_space_t*	space = fil_space_get_by_id(space_id);

		if (space == NULL) {
			return false;
		}

		fil_node_t*	node = UT_LIST_GET_LAST(space->chain);
		ut_ad(space->id == 0
		      || node == UT_LIST_GET_FIRST(space->chain));

		if (space->id == 0) {
			/* We keep the system tablespace files always
			open; this is important in preventing
			deadlocks in this module, as a page read
			completion often performs another read from
			the insert buffer. The insert buffer is in
			tablespace 0, and we cannot end up waiting in
			this function. */
		} else if (space->is_stopping() && !space->is_being_truncated) {
			/* If the tablespace is being deleted then InnoDB
			shouldn't prepare the tablespace for i/o */
			return false;
		} else if (!node || node->is_open()) {
			/* If the file is already open, no need to do
			anything; if the space does not exist, we handle the
			situation in the function which called this
			function */
		} else {
			while (fil_system->n_open >= fil_system->max_n_open) {
				/* Too many files are open */
				if (fil_try_to_close_file_in_LRU(count > 1)) {
					/* No problem */
				} else if (count >= 2) {
					ib::warn() << "innodb_open_files="
						<< fil_system->max_n_open
						<< " is exceeded ("
						<< fil_system->n_open
						<< ") files stay open)";
					break;
				} else {
					mutex_exit(&fil_system->mutex);
					os_aio_simulated_wake_handler_threads();
					os_thread_sleep(20000);
					/* Flush tablespaces so that we can
					close modified files in the LRU list */
					fil_flush_file_spaces(FIL_TYPE_TABLESPACE);

					count++;
					mutex_enter(&fil_system->mutex);
					continue;
				}
			}
		}

		if (ulint size = ulint(UNIV_UNLIKELY(space->recv_size))) {
			ut_ad(node);
			bool	success;
			if (fil_space_extend_must_retry(space, node, size,
							&success)) {
				continue;
			}

			ut_ad(mutex_own(&fil_system->mutex));
			/* Crash recovery requires the file extension
			to succeed. */
			ut_a(success);
			/* InnoDB data files cannot shrink. */
			ut_a(space->size >= size);
			if (size > space->committed_size) {
				space->committed_size = size;
			}

			/* There could be multiple concurrent I/O requests for
			this tablespace (multiple threads trying to extend
			this tablespace).

			Also, fil_space_set_recv_size() may have been invoked
			again during the file extension while fil_system->mutex
			was not being held by us.

			Only if space->recv_size matches what we read
			originally, reset the field. In this way, a
			subsequent I/O request will handle any pending
			fil_space_set_recv_size(). */

			if (size == space->recv_size) {
				space->recv_size = 0;
			}
		}

		break;
	}

	return true;
}

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool
fil_space_extend(
	fil_space_t*	space,
	ulint		size)
{
	ut_ad(!srv_read_only_mode || space->purpose == FIL_TYPE_TEMPORARY);

	bool	success;

	do {
		if (!fil_mutex_enter_and_prepare_for_io(space->id)) {
			success = false;
			break;
		}
	} while (fil_space_extend_must_retry(
			 space, UT_LIST_GET_LAST(space->chain), size,
			 &success));

	mutex_exit(&fil_system->mutex);
	return(success);
}

/** Prepare to free a file node object from a tablespace memory cache.
@param[in,out]	node	file node
@param[in]	space	tablespace */
static
void
fil_node_close_to_free(
	fil_node_t*	node,
	fil_space_t*	space)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(node->magic_n == FIL_NODE_MAGIC_N);
	ut_a(node->n_pending == 0);
	ut_a(!node->being_extended);

	if (node->is_open()) {
		/* We fool the assertion in fil_node_close_file() to think
		there are no unflushed modifications in the file */

		node->needs_flush = false;

		if (fil_buffering_disabled(space)) {

			ut_ad(!space->is_in_unflushed_spaces);
			ut_ad(fil_space_is_flushed(space));

		} else if (space->is_in_unflushed_spaces
			   && fil_space_is_flushed(space)) {

			fil_system->unflushed_spaces.remove(*space);
			space->is_in_unflushed_spaces = false;
		}

		fil_node_close_file(node);
	}
}

/** Detach a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param[in,out]	space		tablespace */
static
void
fil_space_detach(
	fil_space_t*	space)
{
	ut_ad(mutex_own(&fil_system->mutex));

	HASH_DELETE(fil_space_t, hash, fil_system->spaces, space->id, space);

	fil_space_t*	fnamespace = fil_space_get_by_name(space->name);

	ut_a(space == fnamespace);

	HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(space->name), space);

	if (space->is_in_unflushed_spaces) {

		ut_ad(!fil_buffering_disabled(space));

		fil_system->unflushed_spaces.remove(*space);
		space->is_in_unflushed_spaces = false;
	}

	if (space->is_in_default_encrypt) {
		fil_system->default_encrypt_tables.remove(*space);
		space->is_in_default_encrypt = false;
	}

	UT_LIST_REMOVE(fil_system->space_list, space);

	ut_a(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_a(space->n_pending_flushes == 0);

	for (fil_node_t* fil_node = UT_LIST_GET_FIRST(space->chain);
	     fil_node != NULL;
	     fil_node = UT_LIST_GET_NEXT(chain, fil_node)) {

		fil_node_close_to_free(fil_node, space);
	}
}

/** Free a tablespace object on which fil_space_detach() was invoked.
There must not be any pending i/o's or flushes on the files.
@param[in,out]	space		tablespace */
static
void
fil_space_free_low(
	fil_space_t*	space)
{
	/* The tablespace must not be in fil_system->named_spaces. */
	ut_ad(srv_fast_shutdown == 2 || !srv_was_started
	      || space->max_lsn == 0);

	/* Wait for fil_space_release_for_io(); after
	fil_space_detach(), the tablespace cannot be found, so
	fil_space_acquire_for_io() would return NULL */
	while (space->n_pending_ios) {
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

	ut_free(space->name);
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

	mutex_enter(&fil_system->mutex);
	fil_space_t*	space = fil_space_get_by_id(id);

	if (space != NULL) {
		fil_space_detach(space);
	}

	mutex_exit(&fil_system->mutex);

	if (space != NULL) {
		if (x_latched) {
			rw_lock_x_unlock(&space->latch);
		}

		if (!recv_recovery_is_on()) {
			log_mutex_enter();
		}

		ut_ad(log_mutex_own());

		if (space->max_lsn != 0) {
			ut_d(space->max_lsn = 0);
			UT_LIST_REMOVE(fil_system->named_spaces, space);
		}

		if (!recv_recovery_is_on()) {
			log_mutex_exit();
		}

		fil_space_free_low(space);
	}

	return(space != NULL);
}

/** Create a space memory object and put it to the fil_system hash table.
Error messages are issued to the server log.
@param[in]	name		tablespace name
@param[in]	id		tablespace identifier
@param[in]	flags		tablespace flags
@param[in]	purpose		tablespace purpose
@param[in,out]	crypt_data	encryption information
@param[in]	mode		encryption mode
@return pointer to created tablespace, to be filled in with fil_space_t::add()
@retval NULL on failure (such as when the same tablespace exists) */
fil_space_t*
fil_space_create(
	const char*		name,
	ulint			id,
	ulint			flags,
	fil_type_t		purpose,
	fil_space_crypt_t*	crypt_data,
	fil_encryption_t	mode)
{
	fil_space_t*	space;

	ut_ad(fil_system);
	ut_ad(fsp_flags_is_valid(flags & ~FSP_FLAGS_MEM_MASK, id));
	ut_ad(purpose == FIL_TYPE_LOG
	      || srv_page_size == UNIV_PAGE_SIZE_ORIG || flags != 0);

	DBUG_EXECUTE_IF("fil_space_create_failure", return(NULL););

	mutex_enter(&fil_system->mutex);

	/* Look for a matching tablespace. */
	space = fil_space_get_by_name(name);

	if (space != NULL) {
		mutex_exit(&fil_system->mutex);

		ib::warn() << "Tablespace '" << name << "' exists in the"
			" cache with id " << space->id << " != " << id;

		return(NULL);
	}

	space = fil_space_get_by_id(id);

	if (space != NULL) {
		ib::error() << "Trying to add tablespace '" << name
			<< "' with id " << id
			<< " to the tablespace memory cache, but tablespace '"
			<< space->name << "' already exists in the cache!";
		mutex_exit(&fil_system->mutex);
		return(NULL);
	}

	space = static_cast<fil_space_t*>(ut_zalloc_nokey(sizeof(*space)));

	space->id = id;
	space->name = mem_strdup(name);

	UT_LIST_INIT(space->chain, &fil_node_t::chain);

	if ((purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_IMPORT)
	    && !recv_recovery_is_on()
	    && id > fil_system->max_assigned_id) {

		if (!fil_system->space_id_reuse_warned) {
			fil_system->space_id_reuse_warned = true;
			if (srv_operation != SRV_OPERATION_BACKUP) {
				ib::warn() << "Allocated tablespace ID " << id
					<< " for " << name
					<< ", old maximum was "
					<< fil_system->max_assigned_id;
			}
		}

		fil_system->max_assigned_id = id;
	}

	space->purpose = purpose;
	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;
	space->crypt_data = crypt_data;

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
		ut_d(space->latch.set_temp_fsp());
		/* SysTablespace::open_or_create() would pass
		size!=0 to fil_space_t::add(), so first_time_open
		would not hold in fil_node_open_file(), and we
		must assign this manually. We do not care about
		the durability or atomicity of writes to the
		temporary tablespace files. */
		space->atomic_write_supported = true;
	}

	HASH_INSERT(fil_space_t, hash, fil_system->spaces, id, space);

	HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(name), space);

	UT_LIST_ADD_LAST(fil_system->space_list, space);

	if (id < SRV_LOG_SPACE_FIRST_ID && id > fil_system->max_assigned_id) {

		fil_system->max_assigned_id = id;
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
		fil_system->default_encrypt_tables.push_back(*space);
		space->is_in_default_encrypt = true;
		mutex_exit(&fil_system->mutex);
	} else {
		mutex_exit(&fil_system->mutex);
	}

	if (rotate && srv_n_fil_crypt_threads_started) {
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_set(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
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

	mutex_enter(&fil_system->mutex);

	id = *space_id;

	if (id < fil_system->max_assigned_id) {
		id = fil_system->max_assigned_id;
	}

	id++;

	if (id > (SRV_LOG_SPACE_FIRST_ID / 2) && (id % 1000000UL == 0)) {
		ib::warn() << "You are running out of new single-table"
			" tablespace id's. Current counter is " << id
			<< " and it must not exceed" << SRV_LOG_SPACE_FIRST_ID
			<< "! To reset the counter to zero you have to dump"
			" all your tables and recreate the whole InnoDB"
			" installation.";
	}

	success = (id < SRV_LOG_SPACE_FIRST_ID);

	if (success) {
		*space_id = fil_system->max_assigned_id = id;
	} else {
		ib::warn() << "You have run out of single-table tablespace"
			" id's! Current counter is " << id
			<< ". To reset the counter to zero"
			" you have to dump all your tables and"
			" recreate the whole InnoDB installation.";
		*space_id = ULINT_UNDEFINED;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/** Trigger a call to fil_node_t::read_page0()
@param[in]	id	tablespace identifier
@return	tablespace
@retval	NULL	if the tablespace does not exist or cannot be read */
fil_space_t* fil_system_t::read_page0(ulint id)
{
	mutex_exit(&mutex);

	ut_ad(id != 0);

	/* It is possible that the tablespace is dropped while we are
	not holding the mutex. */
	if (!fil_mutex_enter_and_prepare_for_io(id)) {
		return NULL;
	}

	fil_space_t* space = fil_space_get_by_id(id);

	if (space == NULL || UT_LIST_GET_LEN(space->chain) == 0) {
		return(NULL);
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(1 == UT_LIST_GET_LEN(space->chain));

	fil_node_t* node = UT_LIST_GET_FIRST(space->chain);

	/* It must be a single-table tablespace and we have not opened
	the file yet; the following calls will open it and update the
	size fields */

	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		/* The single-table tablespace can't be opened,
		because the ibd file is missing. */
		return(NULL);
	}

	fil_node_complete_io(node, IORequestRead);

	return space;
}

/*******************************************************************//**
Returns a pointer to the fil_space_t that is in the memory cache
associated with a space id. The caller must lock fil_system->mutex.
@return file_space_t pointer, NULL if space not found */
UNIV_INLINE
fil_space_t*
fil_space_get_space(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t* space = fil_space_get_by_id(id);
	if (space == NULL || space->size != 0) {
		return(space);
	}

	switch (space->purpose) {
	case FIL_TYPE_LOG:
		break;
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_IMPORT:
		space = fil_system->read_page0(id);
	}

	return(space);
}

/** Returns the path from the first fil_node_t found with this space ID.
The caller is responsible for freeing the memory allocated here for the
value returned.
@param[in]	id	Tablespace ID
@return own: A copy of fil_node_t::path, NULL if space ID is zero
or not found. */
char*
fil_space_get_first_path(
	ulint		id)
{
	fil_space_t*	space;
	fil_node_t*	node;
	char*		path;

	ut_ad(fil_system);
	ut_a(id);

	if (!fil_mutex_enter_and_prepare_for_io(id)) {
fail_exit:
		mutex_exit(&fil_system->mutex);
		return(NULL);
	}

	space = fil_space_get_space(id);

	if (space == NULL) {
		goto fail_exit;
	}

	ut_ad(mutex_own(&fil_system->mutex));

	node = UT_LIST_GET_FIRST(space->chain);

	path = mem_strdup(node->name);

	mutex_exit(&fil_system->mutex);

	return(path);
}

/** Set the recovered size of a tablespace in pages.
@param id	tablespace ID
@param size	recovered size in pages */
UNIV_INTERN
void
fil_space_set_recv_size(ulint id, ulint size)
{
	mutex_enter(&fil_system->mutex);
	ut_ad(size);
	ut_ad(id < SRV_LOG_SPACE_FIRST_ID);

	if (fil_space_t* space = fil_space_get_space(id)) {
		space->recv_size = size;
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Returns the size of the space in pages. The tablespace must be cached in the
memory cache.
@return space size, 0 if space not found */
ulint
fil_space_get_size(
/*===============*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		size;

	ut_ad(fil_system);
	mutex_enter(&fil_system->mutex);

	space = fil_space_get_space(id);

	size = space ? space->size : 0;

	mutex_exit(&fil_system->mutex);

	return(size);
}

/*******************************************************************//**
Returns the flags of the space. The tablespace must be cached
in the memory cache.
@return flags, ULINT_UNDEFINED if space not found */
ulint
fil_space_get_flags(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		flags;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_space(id);

	if (space == NULL) {
		mutex_exit(&fil_system->mutex);

		return(ULINT_UNDEFINED);
	}

	flags = space->flags;

	mutex_exit(&fil_system->mutex);

	return(flags);
}

/** Open each fil_node_t of a named fil_space_t if not already open.
@param[in]	name	Tablespace name
@return true if all nodes are open  */
bool
fil_space_open(
	const char*	name)
{
	ut_ad(fil_system != NULL);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_name(name);
	fil_node_t*	node;

	for (node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		if (!node->is_open()
		    && !fil_node_open_file(node)) {
			mutex_exit(&fil_system->mutex);
			return(false);
		}
	}

	mutex_exit(&fil_system->mutex);

	return(true);
}

/** Close each fil_node_t of a named fil_space_t if open.
@param[in]	name	Tablespace name */
void
fil_space_close(
	const char*	name)
{
	if (fil_system == NULL) {
		return;
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_name(name);
	if (space == NULL) {
		mutex_exit(&fil_system->mutex);
		return;
	}

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		if (node->is_open()) {
			fil_node_close_file(node);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Returns the page size of the space and whether it is compressed or not.
The tablespace must be cached in the memory cache.
@param[in]	id	space id
@param[out]	found	true if tablespace was found
@return page size */
const page_size_t
fil_space_get_page_size(
	ulint	id,
	bool*	found)
{
	const ulint	flags = fil_space_get_flags(id);

	if (flags == ULINT_UNDEFINED) {
		*found = false;
		return(univ_page_size);
	}

	*found = true;

	return(page_size_t(flags));
}

/****************************************************************//**
Initializes the tablespace memory cache. */
void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open)	/*!< in: max number of open files */
{
	ut_a(fil_system == NULL);

	ut_a(hash_size > 0);
	ut_a(max_n_open > 0);

	fil_system = new fil_system_t();

	mutex_create(LATCH_ID_FIL_SYSTEM, &fil_system->mutex);

	fil_system->spaces = hash_create(hash_size);
	fil_system->name_hash = hash_create(hash_size);

	UT_LIST_INIT(fil_system->LRU, &fil_node_t::LRU);
	UT_LIST_INIT(fil_system->space_list, &fil_space_t::space_list);
	UT_LIST_INIT(fil_system->named_spaces, &fil_space_t::named_spaces);

	fil_system->max_n_open = max_n_open;

	fil_space_crypt_init();
}

/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */
void
fil_open_log_and_system_tablespace_files(void)
/*==========================================*/
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	for (space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		fil_node_t*	node;

		if (fil_space_belongs_in_lru(space)) {

			continue;
		}

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (!node->is_open()) {
				if (!fil_node_open_file(node)) {
					/* This func is called during server's
					startup. If some file of log or system
					tablespace is missing, the server
					can't start successfully. So we should
					assert for it. */
					ut_a(0);
				}
			}

			if (fil_system->max_n_open < 10 + fil_system->n_open) {

				ib::warn() << "You must raise the value of"
					" innodb_open_files in my.cnf!"
					" Remember that InnoDB keeps all"
					" log files and all system"
					" tablespace files open"
					" for the whole time mysqld is"
					" running, and needs to open also"
					" some .ibd files if the"
					" file-per-table storage model is used."
					" Current open files "
					<< fil_system->n_open
					<< ", max allowed open files "
					<< fil_system->max_n_open
					<< ".";
			}
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
void
fil_close_all_files(void)
/*=====================*/
{
	fil_space_t*	space;

	/* At shutdown, we should not have any files in this list. */
	ut_ad(srv_fast_shutdown == 2
	      || !srv_was_started
	      || UT_LIST_GET_LEN(fil_system->named_spaces) == 0);

	mutex_enter(&fil_system->mutex);

	for (space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL; ) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->is_open()) {
				fil_node_close_file(node);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);
		fil_space_detach(prev_space);
		fil_space_free_low(prev_space);
	}

	mutex_exit(&fil_system->mutex);

	ut_ad(srv_fast_shutdown == 2
	      || !srv_was_started
	      || UT_LIST_GET_LEN(fil_system->named_spaces) == 0);
}

/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */
void
fil_close_log_files(
/*================*/
	bool	free)	/*!< in: whether to free the memory object */
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space != NULL) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		if (space->purpose != FIL_TYPE_LOG) {
			space = UT_LIST_GET_NEXT(space_list, space);
			continue;
		}

		/* Log files are not in the fil_system->named_spaces list. */
		ut_ad(space->max_lsn == 0);

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->is_open()) {
				fil_node_close_file(node);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);

		if (free) {
			fil_space_detach(prev_space);
			fil_space_free_low(prev_space);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id)	/*!< in: maximum known id */
{
	if (max_id >= SRV_LOG_SPACE_FIRST_ID) {
		ib::fatal() << "Max tablespace id is too high, " << max_id;
	}

	mutex_enter(&fil_system->mutex);

	if (fil_system->max_assigned_id < max_id) {

		fil_system->max_assigned_id = max_id;
	}

	mutex_exit(&fil_system->mutex);
}

/** Write the flushed LSN to the page header of the first page in the
system tablespace.
@param[in]	lsn	flushed LSN
@return DB_SUCCESS or error number */
dberr_t
fil_write_flushed_lsn(
	lsn_t	lsn)
{
	byte*	buf1;
	byte*	buf;
	dberr_t	err = DB_TABLESPACE_NOT_FOUND;

	buf1 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	buf = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));

	const page_id_t	page_id(TRX_SYS_SPACE, 0);

	err = fil_read(page_id, univ_page_size, 0, univ_page_size.physical(),
		       buf);

	if (err == DB_SUCCESS) {
		mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, lsn);
		err = fil_write(page_id, univ_page_size, 0,
				univ_page_size.physical(), buf);
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
	}

	ut_free(buf1);
	return(err);
}

/** Acquire a tablespace when it could be dropped concurrently.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@param[in]	silent	whether to silently ignore missing tablespaces
@return	the tablespace
@retval	NULL if missing or being deleted or truncated */
UNIV_INTERN
fil_space_t*
fil_space_acquire_low(ulint id, bool silent)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		if (!silent) {
			ib::warn() << "Trying to access missing"
				" tablespace " << id;
		}
	} else if (space->is_stopping()) {
		space = NULL;
	} else {
		space->n_pending_ops++;
	}

	mutex_exit(&fil_system->mutex);

	return(space);
}

/** Release a tablespace acquired with fil_space_acquire().
@param[in,out]	space	tablespace to release  */
void
fil_space_release(fil_space_t* space)
{
	mutex_enter(&fil_system->mutex);
	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_ad(space->n_pending_ops > 0);
	space->n_pending_ops--;
	mutex_exit(&fil_system->mutex);
}

/** Acquire a tablespace for reading or writing a block,
when it could be dropped concurrently.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing */
fil_space_t*
fil_space_acquire_for_io(ulint id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t* space = fil_space_get_by_id(id);

	if (space) {
		space->n_pending_ios++;
	}

	mutex_exit(&fil_system->mutex);

	return(space);
}

/** Release a tablespace acquired with fil_space_acquire_for_io().
@param[in,out]	space	tablespace to release  */
void
fil_space_release_for_io(fil_space_t* space)
{
	mutex_enter(&fil_system->mutex);
	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_ad(space->n_pending_ios > 0);
	space->n_pending_ios--;
	mutex_exit(&fil_system->mutex);
}

/********************************************************//**
Creates the database directory for a table if it does not exist yet. */
void
fil_create_directory_for_tablename(
/*===============================*/
	const char*	name)	/*!< in: name in the standard
				'databasename/tablename' format */
{
	const char*	namend;
	char*		path;
	ulint		len;

	len = strlen(fil_path_to_mysql_datadir);
	namend = strchr(name, '/');
	ut_a(namend);
	path = static_cast<char*>(ut_malloc_nokey(len + (namend - name) + 2));

	memcpy(path, fil_path_to_mysql_datadir, len);
	path[len] = '/';
	memcpy(path + len + 1, name, namend - name);
	path[len + (namend - name) + 1] = 0;

	os_normalize_path(path);

	bool	success = os_file_create_directory(path, false);
	ut_a(success);

	ut_free(path);
}

/** Write a log record about an operation on a tablespace file.
@param[in]	type		MLOG_FILE_NAME or MLOG_FILE_DELETE
or MLOG_FILE_CREATE2 or MLOG_FILE_RENAME2
@param[in]	space_id	tablespace identifier
@param[in]	first_page_no	first page number in the file
@param[in]	path		file path
@param[in]	new_path	if type is MLOG_FILE_RENAME2, the new name
@param[in]	flags		if type is MLOG_FILE_CREATE2, the space flags
@param[in,out]	mtr		mini-transaction */
static
void
fil_op_write_log(
	mlog_id_t	type,
	ulint		space_id,
	ulint		first_page_no,
	const char*	path,
	const char*	new_path,
	ulint		flags,
	mtr_t*		mtr)
{
	byte*		log_ptr;
	ulint		len;

	ut_ad(first_page_no == 0 || type == MLOG_FILE_CREATE2);
	ut_ad(fsp_flags_is_valid(flags, space_id));

	/* fil_name_parse() requires that there be at least one path
	separator and that the file path end with ".ibd". */
	ut_ad(strchr(path, OS_PATH_SEPARATOR) != NULL);
	ut_ad(first_page_no /* trimming an undo tablespace */
	      || !strcmp(&path[strlen(path) - strlen(DOT_IBD)], DOT_IBD));

	log_ptr = mlog_open(mtr, 11 + 4 + 2 + 1);

	if (log_ptr == NULL) {
		/* Logging in mtr is switched off during crash recovery:
		in that case mlog_open returns NULL */
		return;
	}

	log_ptr = mlog_write_initial_log_record_low(
		type, space_id, first_page_no, log_ptr, mtr);

	if (type == MLOG_FILE_CREATE2) {
		mach_write_to_4(log_ptr, flags);
		log_ptr += 4;
	}

	/* Let us store the strings as null-terminated for easier readability
	and handling */

	len = strlen(path) + 1;

	mach_write_to_2(log_ptr, len);
	log_ptr += 2;
	mlog_close(mtr, log_ptr);

	mlog_catenate_string(
		mtr, reinterpret_cast<const byte*>(path), len);

	switch (type) {
	case MLOG_FILE_RENAME2:
		ut_ad(strchr(new_path, OS_PATH_SEPARATOR) != NULL);
		len = strlen(new_path) + 1;
		log_ptr = mlog_open(mtr, 2 + len);
		ut_a(log_ptr);
		mach_write_to_2(log_ptr, len);
		log_ptr += 2;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(
			mtr, reinterpret_cast<const byte*>(new_path), len);
		break;
	case MLOG_FILE_NAME:
	case MLOG_FILE_DELETE:
	case MLOG_FILE_CREATE2:
		break;
	default:
		ut_ad(0);
	}
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	first_page_no	first page number in the file
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write_rename_low(
	ulint		space_id,
	ulint		first_page_no,
	const char*	old_name,
	const char*	new_name,
	mtr_t*		mtr)
{
	ut_ad(!is_predefined_tablespace(space_id));

	fil_op_write_log(
		MLOG_FILE_RENAME2,
		space_id, first_page_no, old_name, new_name, 0, mtr);
}

/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming */
void
fil_name_write_rename(
	ulint		space_id,
	const char*	old_name,
	const char*	new_name)
{
	mtr_t	mtr;
	mtr.start();
	fil_name_write_rename_low(space_id, 0, old_name, new_name, &mtr);
	mtr.commit();
	log_write_up_to(mtr.commit_lsn(), true);
}

/** Write MLOG_FILE_NAME for a file.
@param[in]	space_id	tablespace id
@param[in]	first_page_no	first page number in the file
@param[in]	name		tablespace file name
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write(
	ulint		space_id,
	ulint		first_page_no,
	const char*	name,
	mtr_t*		mtr)
{
	fil_op_write_log(
		MLOG_FILE_NAME, space_id, first_page_no, name, NULL, 0, mtr);
}
/** Write MLOG_FILE_NAME for a file.
@param[in]	space		tablespace
@param[in]	first_page_no	first page number in the file
@param[in]	file		tablespace file
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write(
	const fil_space_t*	space,
	ulint			first_page_no,
	const fil_node_t*	file,
	mtr_t*			mtr)
{
	fil_name_write(space->id, first_page_no, file->name, mtr);
}

/********************************************************//**
Recreates table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
dberr_t
fil_recreate_table(
/*===============*/
	ulint		space_id,	/*!< in: space id */
	ulint		format_flags,	/*!< in: page format */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate)	/*!< in: The information of
					TRUNCATE log record */
{
	dberr_t			err = DB_SUCCESS;
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space_id,
								  &found));

	if (!found) {
		ib::info() << "Missing .ibd file for table '" << name
			<< "' with tablespace " << space_id;
		return(DB_ERROR);
	}

	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Scan for active indexes from REDO logs and drop
	all the indexes using low level function that take root_page_no
	and space-id. */
	truncate.drop_indexes(space_id);

	/* Step-2: Scan for active indexes and re-create them. */
	err = truncate.create_indexes(
		name, space_id, page_size, flags, format_flags);
	if (err != DB_SUCCESS) {
		ib::info() << "Failed to create indexes for the table '"
			<< name << "' with tablespace " << space_id
			<< " while fixing up truncate action";
		return(err);
	}

	truncate_t::s_fix_up_active = false;

	return(err);
}

/********************************************************//**
Recreates the tablespace and table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
dberr_t
fil_recreate_tablespace(
/*====================*/
	ulint		space_id,	/*!< in: space id */
	ulint		format_flags,	/*!< in: page format */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate,	/*!< in: The information of
					TRUNCATE log record */
	lsn_t		recv_lsn)	/*!< in: the end LSN of
						the log record */
{
	dberr_t		err = DB_SUCCESS;
	mtr_t		mtr;

	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Invalidate buffer pool pages belonging to the tablespace
	to re-create. */
	buf_LRU_flush_or_remove_pages(space_id, NULL);

	/* Remove all insert buffer entries for the tablespace */
	ibuf_delete_for_discarded_space(space_id);

	/* Step-2: truncate tablespace (reset the size back to original or
	default size) of tablespace. */
	err = truncate.truncate(
		space_id, truncate.get_dir_path(), name, flags, true);

	if (err != DB_SUCCESS) {

		ib::info() << "Cannot access .ibd file for table '"
			<< name << "' with tablespace " << space_id
			<< " while truncating";
		return(DB_ERROR);
	}

	bool			found;
	const page_size_t&	page_size =
		fil_space_get_page_size(space_id, &found);

	if (!found) {
		ib::info() << "Missing .ibd file for table '" << name
			<< "' with tablespace " << space_id;
		return(DB_ERROR);
	}

	/* Step-3: Initialize Header. */
	if (page_size.is_compressed()) {
		byte*	buf;
		page_t*	page;

		buf = static_cast<byte*>(ut_zalloc_nokey(3 * UNIV_PAGE_SIZE));

		/* Align the memory for file i/o */
		page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

		flags |= FSP_FLAGS_PAGE_SSIZE();

		fsp_header_init_fields(page, space_id, flags);

		mach_write_to_4(
			page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

		page_zip_des_t  page_zip;
		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + UNIV_PAGE_SIZE;

#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
		page_zip.m_end = page_zip.m_nonempty = page_zip.n_blobs = 0;
		buf_flush_init_for_writing(NULL, page, &page_zip, 0);

		err = fil_write(page_id_t(space_id, 0), page_size, 0,
				page_size.physical(), page_zip.data);

		ut_free(buf);

		if (err != DB_SUCCESS) {
			ib::info() << "Failed to clean header of the"
				" table '" << name << "' with tablespace "
				<< space_id;
			return(err);
		}
	}

	mtr_start(&mtr);
	/* Don't log the operation while fixing up table truncate operation
	as crash at this level can still be sustained with recovery restarting
	from last checkpoint. */
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	/* Initialize the first extent descriptor page and
	the second bitmap page for the new tablespace. */
	fsp_header_init(space_id, FIL_IBD_FILE_INITIAL_SIZE, &mtr);
	mtr_commit(&mtr);

	/* Step-4: Re-Create Indexes to newly re-created tablespace.
	This operation will restore tablespace back to what it was
	when it was created during CREATE TABLE. */
	err = truncate.create_indexes(
		name, space_id, page_size, flags, format_flags);
	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Step-5: Write new created pages into ibd file handle and
	flush it to disk for the tablespace, in case i/o-handler thread
	deletes the bitmap page from buffer. */
	mtr_start(&mtr);

	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	mutex_exit(&fil_system->mutex);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	for (ulint page_no = 0; page_no < node->size; ++page_no) {

		const page_id_t	cur_page_id(space_id, page_no);

		buf_block_t*	block = buf_page_get(cur_page_id, page_size,
						     RW_X_LATCH, &mtr);

		byte*	page = buf_block_get_frame(block);

		if (!FSP_FLAGS_GET_ZIP_SSIZE(flags)) {
			ut_ad(!page_size.is_compressed());

			buf_flush_init_for_writing(
				block, page, NULL, recv_lsn);

			err = fil_write(cur_page_id, page_size, 0,
					page_size.physical(), page);
		} else {
			ut_ad(page_size.is_compressed());

			/* We don't want to rewrite empty pages. */

			if (fil_page_get_type(page) != 0) {
				page_zip_des_t*  page_zip =
					buf_block_get_page_zip(block);

				buf_flush_init_for_writing(
					block, page, page_zip, recv_lsn);

				err = fil_write(cur_page_id, page_size, 0,
						page_size.physical(),
						page_zip->data);
			} else {
#ifdef UNIV_DEBUG
				const byte*	data = block->page.zip.data;

				/* Make sure that the page is really empty */
				for (ulint i = 0;
				     i < page_size.physical();
				     ++i) {

					ut_a(data[i] == 0);
				}
#endif /* UNIV_DEBUG */
			}
		}

		if (err != DB_SUCCESS) {
			ib::info() << "Cannot write page " << page_no
				<< " into a .ibd file for table '"
				<< name << "' with tablespace " << space_id;
		}
	}

	mtr_commit(&mtr);

	truncate_t::s_fix_up_active = false;

	return(err);
}

/** Replay a file rename operation if possible.
@param[in]	space_id	tablespace identifier
@param[in]	first_page_no	first page number in the file
@param[in]	name		old file name
@param[in]	new_name	new file name
@return	whether the operation was successfully applied
(the name did not exist, or new_name did not exist and
name was successfully renamed to new_name)  */
bool
fil_op_replay_rename(
	ulint		space_id,
	ulint		first_page_no,
	const char*	name,
	const char*	new_name)
{
	ut_ad(first_page_no == 0);

	/* In order to replay the rename, the following must hold:
	* The new name is not already used.
	* A tablespace exists with the old name.
	* The space ID for that tablepace matches this log entry.
	This will prevent unintended renames during recovery. */
	fil_space_t*	space = fil_space_get(space_id);

	if (space == NULL) {
		return(true);
	}

	const bool name_match
		= strcmp(name, UT_LIST_GET_FIRST(space->chain)->name) == 0;

	if (!name_match) {
		return(true);
	}

	/* Create the database directory for the new name, if
	it does not exist yet */

	const char*	namend = strrchr(new_name, OS_PATH_SEPARATOR);
	ut_a(namend != NULL);

	char*		dir = static_cast<char*>(
		ut_malloc_nokey(namend - new_name + 1));

	memcpy(dir, new_name, namend - new_name);
	dir[namend - new_name] = '\0';

	bool		success = os_file_create_directory(dir, false);
	ut_a(success);

	ulint		dirlen = 0;

	if (const char* dirend = strrchr(dir, OS_PATH_SEPARATOR)) {
		dirlen = dirend - dir + 1;
	}

	ut_free(dir);

	/* New path must not exist. */
	dberr_t		err = fil_rename_tablespace_check(
		space_id, name, new_name, false);
	if (err != DB_SUCCESS) {
		ib::error() << " Cannot replay file rename."
			" Remove either file and try again.";
		return(false);
	}

	char*		new_table = mem_strdupl(
		new_name + dirlen,
		strlen(new_name + dirlen)
		- 4 /* remove ".ibd" */);

	ut_ad(new_table[namend - new_name - dirlen]
	      == OS_PATH_SEPARATOR);
#if OS_PATH_SEPARATOR != '/'
	new_table[namend - new_name - dirlen] = '/';
#endif

	if (!fil_rename_tablespace(
		    space_id, name, new_table, new_name)) {
		ut_error;
	}

	ut_free(new_table);
	return(true);
}

/** File operations for tablespace */
enum fil_operation_t {
	FIL_OPERATION_DELETE,	/*!< delete a single-table tablespace */
	FIL_OPERATION_CLOSE,	/*!< close a single-table tablespace */
	FIL_OPERATION_TRUNCATE	/*!< truncate an undo tablespace */
};

/** Check for pending operations.
@param[in]	space	tablespace
@param[in]	count	number of attempts so far
@return 0 if no operations else count + 1. */
static
ulint
fil_check_pending_ops(const fil_space_t* space, ulint count)
{
	ut_ad(mutex_own(&fil_system->mutex));

	if (space == NULL) {
		return 0;
	}

	if (ulint n_pending_ops = space->n_pending_ops) {

		if (count > 5000) {
			ib::warn() << "Trying to close/delete/truncate"
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
	fil_operation_t	operation,	/*!< in: File operation */
	fil_space_t*	space,		/*!< in/out: Tablespace to check */
	fil_node_t**	node,		/*!< out: Node in space list */
	ulint		count)		/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(space->n_pending_ops == 0);

	switch (operation) {
	case FIL_OPERATION_DELETE:
	case FIL_OPERATION_CLOSE:
		break;
	case FIL_OPERATION_TRUNCATE:
		space->is_being_truncated = true;
		break;
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	*node = UT_LIST_GET_FIRST(space->chain);

	if (space->n_pending_flushes > 0 || (*node)->n_pending > 0) {

		ut_a(!(*node)->being_extended);

		if (count > 1000) {
			ib::warn() << "Trying to delete/close/truncate"
				" tablespace '" << space->name
				<< "' but there are "
				<< space->n_pending_flushes
				<< " flushes and " << (*node)->n_pending
				<< " pending i/o's on it.";
		}

		return(count + 1);
	}

	return(0);
}

/*******************************************************************//**
Check pending operations on a tablespace.
@return DB_SUCCESS or error failure. */
static
dberr_t
fil_check_pending_operations(
/*=========================*/
	ulint		id,		/*!< in: space id */
	fil_operation_t	operation,	/*!< in: File operation */
	fil_space_t**	space,		/*!< out: tablespace instance
					in memory */
	char**		path)		/*!< out/own: tablespace path */
{
	ulint		count = 0;

	ut_a(!is_system_tablespace(id));
	ut_ad(space);

	*space = 0;

	mutex_enter(&fil_system->mutex);
	fil_space_t* sp = fil_space_get_by_id(id);

	if (sp) {
		sp->stop_new_ops = true;
		if (sp->crypt_data) {
			sp->n_pending_ops++;
			mutex_exit(&fil_system->mutex);
			fil_space_crypt_close_tablespace(sp);
			mutex_enter(&fil_system->mutex);
			ut_ad(sp->n_pending_ops > 0);
			sp->n_pending_ops--;
		}
	}

	/* Check for pending operations. */

	do {
		sp = fil_space_get_by_id(id);

		count = fil_check_pending_ops(sp, count);

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

		mutex_enter(&fil_system->mutex);
	} while (count > 0);

	/* Check for pending IO. */

	for (;;) {
		sp = fil_space_get_by_id(id);

		if (sp == NULL) {
			mutex_exit(&fil_system->mutex);
			return(DB_TABLESPACE_NOT_FOUND);
		}

		fil_node_t*	node;

		count = fil_check_pending_io(operation, sp, &node, count);

		if (count == 0 && path) {
			*path = mem_strdup(node->name);
		}

		mutex_exit(&fil_system->mutex);

		if (count == 0) {
			break;
		}

		os_thread_sleep(20000);
		mutex_enter(&fil_system->mutex);
	}

	ut_ad(sp);

	*space = sp;
	return(DB_SUCCESS);
}

/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return DB_SUCCESS or error */
dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*		trx,	/*!< in/out: Transaction covering the close */
	ulint		id)	/*!< in: space id */
{
	char*		path = 0;
	fil_space_t*	space = 0;
	dberr_t		err;

	ut_a(!is_system_tablespace(id));

	err = fil_check_pending_operations(id, FIL_OPERATION_CLOSE,
					   &space, &path);

	if (err != DB_SUCCESS) {
		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	rw_lock_x_lock(&space->latch);

	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have set space->stop_new_ops = true, readahead
	or ibuf merge can no longer read more pages of this tablespace to the
	buffer pool. Thus we can clean the tablespace out of the buffer pool
	completely and permanently. The flag stop_new_ops also prevents
	fil_flush() from being applied to this tablespace. */

	{
		FlushObserver observer(id, trx, NULL);
		buf_LRU_flush_or_remove_pages(id, &observer);
	}

	/* If the free is successful, the X lock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free(id, true)) {
		rw_lock_x_unlock(&space->latch);
		err = DB_TABLESPACE_NOT_FOUND;
	} else {
		err = DB_SUCCESS;
	}

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	char*	cfg_name = fil_make_filepath(path, NULL, CFG, false);
	if (cfg_name != NULL) {
		os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
		ut_free(cfg_name);
	}

	ut_free(path);

	return(err);
}

/** Determine whether a table can be accessed in operations that are
not (necessarily) protected by meta-data locks.
(Rollback would generally be protected, but rollback of
FOREIGN KEY CASCADE/SET NULL is not protected by meta-data locks
but only by InnoDB table locks, which may be broken by
lock_remove_all_on_table().)
@param[in]	table	persistent table
checked @return whether the table is accessible */
bool fil_table_accessible(const dict_table_t* table)
{
	if (UNIV_UNLIKELY(!table->is_readable() || table->corrupted)) {
		return(false);
	}

	if (fil_space_t* space = fil_space_acquire(table->space)) {
		bool accessible = !space->is_stopping();
		fil_space_release(space);
		ut_ad(accessible || dict_table_is_file_per_table(table));
		return(accessible);
	} else {
		return(false);
	}
}

/** Delete a tablespace and associated .ibd file.
@param[in]	id		tablespace identifier
@param[in]	if_exists	whether to ignore missing tablespace
@return	DB_SUCCESS or error */
dberr_t fil_delete_tablespace(ulint id, bool if_exists)
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(!is_system_tablespace(id));

	dberr_t err = fil_check_pending_operations(
		id, FIL_OPERATION_DELETE, &space, &path);

	if (err != DB_SUCCESS) {
		if (!if_exists) {
			ib::error() << "Cannot delete tablespace " << id
				    << " because it is not found"
				       " in the tablespace memory cache.";
		}

		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	/* IMPORTANT: Because we have set space::stop_new_ops there
	can't be any new ibuf merges, reads or flushes. We are here
	because node::n_pending was zero above. However, it is still
	possible to have pending read and write requests:

	A read request can happen because the reader thread has
	gone through the ::stop_new_ops check in buf_page_init_for_read()
	before the flag was set and has not yet incremented ::n_pending
	when we checked it above.

	A write request can be issued any time because we don't check
	the ::stop_new_ops flag when queueing a block for write.

	We deal with pending write requests in the following function
	where we'd minimally evict all dirty pages belonging to this
	space from the flush_list. Note that if a block is IO-fixed
	we'll wait for IO to complete.

	To deal with potential read requests, we will check the
	::stop_new_ops flag in fil_io(). */

	buf_LRU_flush_or_remove_pages(id, NULL);

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */
	{
		/* Before deleting the file, write a log record about
		it, so that InnoDB crash recovery will expect the file
		to be gone. */
		mtr_t		mtr;

		mtr_start(&mtr);
		fil_op_write_log(MLOG_FILE_DELETE, id, 0, path, NULL, 0, &mtr);
		mtr_commit(&mtr);
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

	mutex_enter(&fil_system->mutex);

	/* Double check the sanity of pending ops after reacquiring
	the fil_system::mutex. */
	if (const fil_space_t* s = fil_space_get_by_id(id)) {
		ut_a(s == space);
		ut_a(space->n_pending_ops == 0);
		ut_a(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		ut_a(node->n_pending == 0);

		fil_space_detach(space);
		mutex_exit(&fil_system->mutex);

		log_mutex_enter();

		if (space->max_lsn != 0) {
			ut_d(space->max_lsn = 0);
			UT_LIST_REMOVE(fil_system->named_spaces, space);
		}

		log_mutex_exit();
		fil_space_free_low(space);

		if (!os_file_delete(innodb_data_file_key, path)
		    && !os_file_delete_if_exists(
			    innodb_data_file_key, path, NULL)) {

			/* Note: This is because we have removed the
			tablespace instance from the cache. */

			err = DB_IO_ERROR;
		}
	} else {
		mutex_exit(&fil_system->mutex);
		err = DB_TABLESPACE_NOT_FOUND;
	}

	ut_free(path);

	return(err);
}

/** Prepare to truncate an undo tablespace.
@param[in]	space_id	undo tablespace id
@return	the tablespace
@retval	NULL if tablespace not found */
fil_space_t* fil_truncate_prepare(ulint space_id)
{
	/* Stop all I/O on the tablespace and ensure that related
	pages are flushed to disk. */
	fil_space_t* space;
	if (fil_check_pending_operations(space_id, FIL_OPERATION_TRUNCATE,
					 &space, NULL) != DB_SUCCESS) {
		return NULL;
	}
	ut_ad(space != NULL);
	return space;
}

/** Write log about an undo tablespace truncate operation. */
void fil_truncate_log(fil_space_t* space, ulint size, mtr_t* mtr)
{
	/* Write a MLOG_FILE_CREATE2 record with the new size, so that
	recovery and backup will ignore any preceding redo log records
	for writing pages that are after the new end of the tablespace. */
	ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
	const fil_node_t* file = UT_LIST_GET_FIRST(space->chain);
	fil_op_write_log(MLOG_FILE_CREATE2, space->id, size, file->name,
			 NULL, space->flags & ~FSP_FLAGS_MEM_MASK, mtr);
}

/** Truncate the tablespace to needed size.
@param[in]	space_id	id of tablespace to truncate
@param[in]	size_in_pages	truncate size.
@return true if truncate was successful. */
bool
fil_truncate_tablespace(
	ulint		space_id,
	ulint		size_in_pages)
{
	/* Step-1: Prepare tablespace for truncate. This involves
	stopping all the new operations + IO on that tablespace
	and ensuring that related pages are flushed to disk. */
	if (fil_prepare_for_truncate(space_id) != DB_SUCCESS) {
		return(false);
	}

	/* Step-2: Invalidate buffer pool pages belonging to the tablespace
	to re-create. Remove all insert buffer entries for the tablespace */
	buf_LRU_flush_or_remove_pages(space_id, NULL);

	/* Step-3: Truncate the tablespace and accordingly update
	the fil_space_t handler that is used to access this tablespace. */
	mutex_enter(&fil_system->mutex);
	fil_space_t*	space = fil_space_get_by_id(space_id);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	ut_ad(node->is_open());

	space->size = node->size = size_in_pages;

	bool success = os_file_truncate(node->name, node->handle, 0);
	if (success) {

		os_offset_t	size = os_offset_t(size_in_pages) * UNIV_PAGE_SIZE;

		success = os_file_set_size(
			node->name, node->handle, size,
			FSP_FLAGS_HAS_PAGE_COMPRESSION(space->flags));

		if (success) {
			space->stop_new_ops = false;
			space->is_being_truncated = false;
		}
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/*******************************************************************//**
Prepare for truncating a single-table tablespace.
1) Check pending operations on a tablespace;
2) Remove all insert buffer entries for the tablespace;
@return DB_SUCCESS or error */
dberr_t
fil_prepare_for_truncate(
/*=====================*/
	ulint	id)		/*!< in: space id */
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(!is_system_tablespace(id));

	dberr_t	err = fil_check_pending_operations(
		id, FIL_OPERATION_TRUNCATE, &space, &path);

	ut_free(path);

	if (err == DB_TABLESPACE_NOT_FOUND) {
		ib::error() << "Cannot truncate tablespace " << id
			<< " because it is not found in the tablespace"
			" memory cache.";
	}

	return(err);
}

/** Reinitialize the original tablespace header with the same space id
for single tablespace
@param[in]      table		table belongs to tablespace
@param[in]      size            size in blocks
@param[in]      trx             Transaction covering truncate */
void
fil_reinit_space_header_for_table(
	dict_table_t*	table,
	ulint		size,
	trx_t*		trx)
{
	ulint	id = table->space;

	ut_a(!is_system_tablespace(id));

	/* Invalidate in the buffer pool all pages belonging
	to the tablespace. The buffer pool scan may take long
	time to complete, therefore we release dict_sys->mutex
	and the dict operation lock during the scan and aquire
	it again after the buffer pool scan.*/

	/* Release the lock on the indexes too. So that
	they won't violate the latch ordering. */
	dict_table_x_unlock_indexes(table);
	row_mysql_unlock_data_dictionary(trx);

	DEBUG_SYNC_C("buffer_pool_scan");
	buf_LRU_flush_or_remove_pages(id, NULL);

	row_mysql_lock_data_dictionary(trx);

	dict_table_x_lock_indexes(table);

	/* Remove all insert buffer entries for the tablespace */
	ibuf_delete_for_discarded_space(id);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	space->size = node->size = size;

	mutex_exit(&fil_system->mutex);

	mtr_t	mtr;

	mtr_start(&mtr);
	mtr.set_named_space(id);

	fsp_header_init(id, size, &mtr);

	mtr_commit(&mtr);
}

#ifdef UNIV_DEBUG
/** Increase redo skipped count for a tablespace.
@param[in]	id	space id */
void
fil_space_inc_redo_skipped_count(
	ulint		id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != NULL);

	space->redo_skipped_count++;

	mutex_exit(&fil_system->mutex);
}

/** Decrease redo skipped count for a tablespace.
@param[in]	id	space id */
void
fil_space_dec_redo_skipped_count(
	ulint		id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != NULL);
	ut_a(space->redo_skipped_count > 0);

	space->redo_skipped_count--;

	mutex_exit(&fil_system->mutex);
}
#endif /* UNIV_DEBUG */

/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. Free all the pages in use by the tablespace.
@return DB_SUCCESS or error */
dberr_t
fil_discard_tablespace(
/*===================*/
	ulint	id)	/*!< in: space id */
{
	dberr_t	err;

	switch (err = fil_delete_tablespace(id)) {
	case DB_SUCCESS:
		break;

	case DB_IO_ERROR:
		ib::warn() << "While deleting tablespace " << id
			<< " in DISCARD TABLESPACE. File rename/delete"
			" failed: " << err;
		break;

	case DB_TABLESPACE_NOT_FOUND:
		ib::warn() << "Cannot delete tablespace " << id
			<< " in DISCARD TABLESPACE: " << err;
		break;

	default:
		ut_error;
	}

	/* Remove all insert buffer entries for the tablespace */

	ibuf_delete_for_discarded_space(id);

	return(err);
}

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the direcory path or the full path and filename.
@param[in] name NULL if path is full, or Table/Tablespace name
@param[in] suffix NULL or the file extention to use.
@param[in] trim_name true if the last name on the path should be trimmed.
@return own: file name */
char*
fil_make_filepath(
	const char*	path,
	const char*	name,
	ib_extention	ext,
	bool		trim_name)
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is NULL, we can use the default path,
	but there needs to be a name. */
	ut_ad(path != NULL || name != NULL);

	/* If we are going to strip a name off the path, there better be a
	path and a new name to put back on. */
	ut_ad(!trim_name || (path != NULL && name != NULL));

	if (path == NULL) {
		path = fil_path_to_mysql_datadir;
	}

	ulint	len		= 0;	/* current length */
	ulint	path_len	= strlen(path);
	ulint	name_len	= (name ? strlen(name) : 0);
	const char* suffix	= dot_ext[ext];
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
@param[in]	space_id	tablespace id
@param[in]	old_path	old filepath
@param[in]	new_path	new filepath
@param[in]	is_discarded	whether the tablespace is discarded
@param[in]	replace_new	whether to ignore the existence of new_path
@return innodb error code */
dberr_t
fil_rename_tablespace_check(
	ulint		space_id,
	const char*	old_path,
	const char*	new_path,
	bool		is_discarded,
	bool		replace_new)
{
	bool	exists = false;
	os_file_type_t	ftype;

	if (!is_discarded
	    && os_file_status(old_path, &exists, &ftype)
	    && !exists) {
		ib::error() << "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' for space ID " << space_id
			<< " because the source file"
			<< " does not exist.";
		return(DB_TABLESPACE_NOT_FOUND);
	}

	exists = false;
	if (os_file_status(new_path, &exists, &ftype) && !exists) {
		return DB_SUCCESS;
	}

	if (!replace_new) {
		ib::error() << "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' for space ID " << space_id
			<< " because the target file exists."
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
	mutex_enter(&fil_system->mutex);
	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space; space = UT_LIST_GET_NEXT(space_list, space)) {
		ulint id = space->id;
		if (id && id < SRV_LOG_SPACE_FIRST_ID
		    && space->purpose == FIL_TYPE_TABLESPACE
		    && !strcmp(new_path,
			       UT_LIST_GET_FIRST(space->chain)->name)) {
			ib::info() << "TRUNCATE rollback: " << id
				<< "," << new_path;
			mutex_exit(&fil_system->mutex);
			dberr_t err = fil_delete_tablespace(id);
			if (err != DB_SUCCESS) {
				return err;
			}
			goto retry;
		}
	}
	mutex_exit(&fil_system->mutex);
	fil_delete_file(new_path);

	return(DB_SUCCESS);
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
bool
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

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib::error() << "Cannot find space id " << id
			<< " in the tablespace memory cache, though the file '"
			<< old_path
			<< "' in a rename operation should have that id.";
func_exit:
		mutex_exit(&fil_system->mutex);
		return(false);
	}

	if (space != fil_space_get_by_name(space->name)) {
		ib::error() << "Cannot find " << space->name
			<< " in tablespace memory cache";
		goto func_exit;
	}

	if (fil_space_get_by_name(new_name)) {
		ib::error() << new_name
			<< " is already in tablespace memory cache";
		goto func_exit;
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	node = UT_LIST_GET_FIRST(space->chain);
	space->n_pending_ops++;

	mutex_exit(&fil_system->mutex);

	char*	new_file_name = new_path_in == NULL
		? fil_make_filepath(NULL, new_name, IBD, false)
		: mem_strdup(new_path_in);
	char*	old_file_name = node->name;
	char*	new_space_name = mem_strdup(new_name);
	char*	old_space_name = space->name;
	ulint	old_fold = ut_fold_string(old_space_name);
	ulint	new_fold = ut_fold_string(new_space_name);

	ut_ad(strchr(old_file_name, OS_PATH_SEPARATOR) != NULL);
	ut_ad(strchr(new_file_name, OS_PATH_SEPARATOR) != NULL);

	if (!recv_recovery_is_on()) {
		log_mutex_enter();
	}

	/* log_sys->mutex is above fil_system->mutex in the latching order */
	ut_ad(log_mutex_own());
	mutex_enter(&fil_system->mutex);
	ut_ad(space->n_pending_ops);
	space->n_pending_ops--;
	ut_ad(space->name == old_space_name);
	/* We already checked these. */
	ut_ad(space == fil_space_get_by_name(old_space_name));
	ut_ad(!fil_space_get_by_name(new_space_name));
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
		log_mutex_exit();
	}

	ut_ad(space->name == old_space_name);
	if (success) {
		HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
			    old_fold, space);
		space->name = new_space_name;
		HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
			    new_fold, space);
	} else {
		/* Because nothing was renamed, we must free the new
		names, not the old ones. */
		old_file_name = new_file_name;
		old_space_name = new_space_name;
	}

	mutex_exit(&fil_system->mutex);

	ut_free(old_file_name);
	ut_free(old_space_name);

	return(success);
}

/** Create a tablespace file.
@param[in]	space_id	Tablespace ID
@param[in]	name		Tablespace name in dbname/tablename format.
@param[in]	path		Path and filename of the datafile to create.
@param[in]	flags		Tablespace flags
@param[in]	size		Initial size of the tablespace file in
                                pages, must be >= FIL_IBD_FILE_INITIAL_SIZE
@param[in]	mode		MariaDB encryption mode
@param[in]	key_id		MariaDB encryption key_id
@return DB_SUCCESS or error code */
dberr_t
fil_ibd_create(
	ulint		space_id,
	const char*	name,
	const char*	path,
	ulint		flags,
	ulint		size,
	fil_encryption_t mode,
	uint32_t	key_id)
{
	pfs_os_file_t	file;
	dberr_t		err;
	byte*		buf2;
	byte*		page;
	bool		success;
	bool		has_data_dir = FSP_FLAGS_HAS_DATA_DIR(flags) != 0;
	fil_space_t*	space = NULL;
	fil_space_crypt_t *crypt_data = NULL;

	ut_ad(!is_system_tablespace(space_id));
	ut_ad(!srv_read_only_mode);
	ut_a(space_id < SRV_LOG_SPACE_FIRST_ID);
	ut_a(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_a(fsp_flags_is_valid(flags & ~FSP_FLAGS_MEM_MASK, space_id));

	/* Create the subdirectories in the path, if they are
	not there already. */
	err = os_file_create_subdirs_if_needed(path);
	if (err != DB_SUCCESS) {
		return(err);
	}

	ulint type;
	compile_time_assert(((UNIV_ZIP_SIZE_MIN >> 1) << 3) == 4096);
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
		OS_FILE_NORMAL,
		type,
		srv_read_only_mode,
		&success);

	if (!success) {
		/* The following call will print an error message */
		ulint	error = os_file_get_last_error(true);

		ib::error() << "Cannot create file '" << path << "'";

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib::info() << "The file '" << path << "'"
				" already exists though the"
				" corresponding table did not exist"
				" in the InnoDB data dictionary."
				" You can resolve the problem by removing"
				" the file.";

			return(DB_TABLESPACE_EXISTS);
		}

		if (error == OS_FILE_DISK_FULL) {
			return(DB_OUT_OF_FILE_SPACE);
		}

		return(DB_ERROR);
	}

	const bool is_compressed = FSP_FLAGS_HAS_PAGE_COMPRESSION(flags);

#ifdef _WIN32
	if (is_compressed) {
		os_file_set_sparse_win32(file);
	}
#endif

	success = os_file_set_size(
		path, file,
		os_offset_t(size) << UNIV_PAGE_SIZE_SHIFT, is_compressed);

	if (!success) {
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		return(DB_OUT_OF_FILE_SPACE);
	}

	bool punch_hole = os_is_sparse_file_supported(file);

	ulint block_size = os_file_get_block_size(file, path);

	/* We have to write the space id to the file immediately and flush the
	file to disk. This is because in crash recovery we must be aware what
	tablespaces exist and what are their space id's, so that we can apply
	the log records to the right file. It may take quite a while until
	buffer pool flush algorithms write anything to the file and flush it to
	disk. If we would not write here anything, the file would be filled
	with zeros from the call of os_file_set_size(), until a buffer pool
	flush would write to it. */

	buf2 = static_cast<byte*>(ut_malloc_nokey(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	flags |= FSP_FLAGS_PAGE_SSIZE();
	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	/* Create crypt data if the tablespace is either encrypted or user has
	requested it to remain unencrypted. */
	if (mode == FIL_ENCRYPTION_ON || mode == FIL_ENCRYPTION_OFF ||
		srv_encrypt_tables) {
		crypt_data = fil_space_create_crypt_data(mode, key_id);
	}

	if (crypt_data) {
		/* Write crypt data information in page0 while creating
		ibd file. */
		crypt_data->fill_page0(flags, page);
	}

	const page_size_t	page_size(flags);
	IORequest		request(IORequest::WRITE);

	if (!page_size.is_compressed()) {

		buf_flush_init_for_writing(NULL, page, NULL, 0);

		err = os_file_write(
			request, path, file, page, 0, page_size.physical());
	} else {
		page_zip_des_t	page_zip;
		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + UNIV_PAGE_SIZE;
#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;

		buf_flush_init_for_writing(NULL, page, &page_zip, 0);

		err = os_file_write(
			request, path, file, page_zip.data, 0,
			page_size.physical());
	}

	ut_free(buf2);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Could not write the first page to"
			<< " tablespace '" << path << "'";

		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);

		return(DB_ERROR);
	}

	success = os_file_flush(file);

	if (!success) {
		ib::error() << "File flush of tablespace '"
			<< path << "' failed";
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		return(DB_ERROR);
	}

	if (has_data_dir) {
		/* Make the ISL file if the IBD file is not
		in the default location. */
		err = RemoteDatafile::create_link_file(name, path);
		if (err != DB_SUCCESS) {
			os_file_close(file);
			os_file_delete(innodb_data_file_key, path);
			return(err);
		}
	}

	space = fil_space_create(name, space_id, flags, FIL_TYPE_TABLESPACE,
				 crypt_data, mode);
	if (!space) {
		if (crypt_data) {
			free(crypt_data);
		}

		err = DB_ERROR;
	} else {
		mtr_t mtr;
		fil_node_t* file = space->add(path, OS_FILE_CLOSED, size,
					      false, true);

		mtr.start();
		fil_op_write_log(
			MLOG_FILE_CREATE2, space_id, 0, file->name,
			NULL, space->flags & ~FSP_FLAGS_MEM_MASK, &mtr);
		fil_name_write(space, 0, file, &mtr);
		mtr.commit();

		file->block_size = block_size;
		space->punch_hole = punch_hole;

		err = DB_SUCCESS;
	}

	os_file_close(file);

	if (err != DB_SUCCESS) {
		if (has_data_dir) {
			RemoteDatafile::delete_link_file(name);
		}

		os_file_delete(innodb_data_file_key, path);
	}

	return(err);
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
@return DB_SUCCESS or error code */
dberr_t
fil_ibd_open(
	bool		validate,
	bool		fix_dict,
	fil_type_t	purpose,
	ulint		id,
	ulint		flags,
	const char*	space_name,
	const char*	path_in)
{
	dberr_t		err = DB_SUCCESS;
	bool		dict_filepath_same_as_default = false;
	bool		link_file_found = false;
	bool		link_file_is_bad = false;
	Datafile	df_default;	/* default location */
	Datafile	df_dict;	/* dictionary location */
	RemoteDatafile	df_remote;	/* remote location */
	ulint		tablespaces_found = 0;
	ulint		valid_tablespaces_found = 0;

	ut_ad(!fix_dict || rw_lock_own(&dict_operation_lock, RW_LOCK_X));

	ut_ad(!fix_dict || mutex_own(&dict_sys->mutex));
	ut_ad(!fix_dict || !srv_read_only_mode);
	ut_ad(!fix_dict || srv_log_file_size != 0);
	ut_ad(fil_type_is_data(purpose));

	/* Table flags can be ULINT_UNDEFINED if
	dict_tf_to_fsp_flags_failure is set. */
	if (flags == ULINT_UNDEFINED) {
		return(DB_CORRUPTION);
	}

	ut_ad(fsp_flags_is_valid(flags & ~FSP_FLAGS_MEM_MASK, id));
	df_default.init(space_name, flags);
	df_dict.init(space_name, flags);
	df_remote.init(space_name, flags);

	/* Discover the correct file by looking in three possible locations
	while avoiding unecessary effort. */

	/* We will always look for an ibd in the default location. */
	df_default.make_filepath(NULL, space_name, IBD);

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

	/* Always look for a file at the default location. But don't log
	an error if the tablespace is already open in remote or dict. */
	ut_a(df_default.filepath());
	const bool	strict = (tablespaces_found == 0);
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
		os_file_get_last_error(true);
		ib::error() << "Could not find a valid tablespace file for `"
			<< space_name << "`. " << TROUBLESHOOT_DATADICT_MSG;
		return(DB_CORRUPTION);
	}
	if (!validate) {
		goto skip_validate;
	}

	/* Do not open any tablespaces if more than one tablespace with
	the correct space ID and flags were found. */
	if (tablespaces_found > 1) {
		ib::error() << "A tablespace for `" << space_name
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
				<< space_name << "`";

			/* If the file is not open it cannot be valid. */
			ut_ad(df_default.is_open() || !df_default.is_valid());
			ut_ad(df_dict.is_open()    || !df_dict.is_valid());
			ut_ad(df_remote.is_open()  || !df_remote.is_valid());

			/* Having established that, this is an easy way to
			look for corrupted data files. */
			if (df_default.is_open() != df_default.is_valid()
			    || df_dict.is_open() != df_dict.is_valid()
			    || df_remote.is_open() != df_remote.is_valid()) {
				return(DB_CORRUPTION);
			}
error:
			return(DB_ERROR);
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
				RemoteDatafile::delete_link_file(space_name);
			}

		} else if (!link_file_found || link_file_is_bad) {
			ut_ad(df_dict.is_open());
			/* Fix the link file if we got our filepath
			from the dictionary but a link file did not
			exist or it did not point to a valid file. */
			RemoteDatafile::delete_link_file(space_name);
			RemoteDatafile::create_link_file(
				space_name, df_dict.filepath());
		}

	} else if (df_remote.is_open()) {
		if (dict_filepath_same_as_default) {
			dict_update_filepath(id, df_remote.filepath());

		} else if (path_in == NULL) {
			/* SYS_DATAFILES record for this space ID
			was not found. */
			dict_replace_tablespace_and_filepath(
				id, space_name, df_remote.filepath(), flags);
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
				id, space_name, df_default.filepath(), flags);
		}
	}

skip_validate:
	if (err == DB_SUCCESS) {
		const byte* first_page =
			df_default.is_open() ? df_default.get_first_page() :
			df_dict.is_open() ? df_dict.get_first_page() :
			df_remote.get_first_page();

		fil_space_crypt_t* crypt_data = first_page
			? fil_space_read_crypt_data(page_size_t(flags),
						    first_page)
			: NULL;

		fil_space_t* space = fil_space_create(
			space_name, id, flags, purpose, crypt_data);
		if (!space) {
			goto error;
		}

		/* We do not measure the size of the file, that is why
		we pass the 0 below */

		space->add(
			df_remote.is_open() ? df_remote.filepath() :
			df_dict.is_open() ? df_dict.filepath() :
			df_default.filepath(), OS_FILE_CLOSED, 0, false, true);

		if (err == DB_SUCCESS && validate
		    && purpose != FIL_TYPE_IMPORT && !srv_read_only_mode) {
			df_remote.close();
			df_dict.close();
			df_default.close();
			fsp_flags_try_adjust(id, flags & ~FSP_FLAGS_MEM_MASK);
		}
	}

	return(err);
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

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space != NULL) {
		*name = mem_strdup(space->name);

		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		*filepath = mem_strdup(node->name);

		success = true;
	}

	mutex_exit(&fil_system->mutex);

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
			      end - tablename))) {
		dbname = tablename;
		tablename = t + 1;
	}
#endif /* HAVE_MEMRCHR */

	ut_ad(dbname != NULL);
	ut_ad(tablename > dbname);
	ut_ad(tablename < end);
	ut_ad(end - tablename > 4);
	ut_ad(memcmp(end - 4, DOT_IBD, 4) == 0);

	char*	name = mem_strdupl(dbname, end - dbname - 4);

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
		case SRV_OPERATION_RESTORE_ROLLBACK_XA:
		case SRV_OPERATION_RESTORE_EXPORT:
		case SRV_OPERATION_RESTORE:
			break;
		case SRV_OPERATION_NORMAL:
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
	mutex_enter(&fil_system->mutex);
	space = fil_space_get_by_id(space_id);
	mutex_exit(&fil_system->mutex);

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

	if (is_mariabackup_restore()) {
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
		minimum_size = FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE;

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
	if (FSP_FLAGS_HAS_PAGE_COMPRESSION(flags)) {
		flags |= page_zip_level
			<< FSP_FLAGS_MEM_COMPRESSION_LEVEL;
	}

	const byte* first_page = file.get_first_page();
	fil_space_crypt_t* crypt_data = first_page
		? fil_space_read_crypt_data(page_size_t(flags), first_page)
		: NULL;
	space = fil_space_create(
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

/***********************************************************************//**
A fault-tolerant function that tries to read the next file name in the
directory. We retry 100 times if os_file_readdir_next_file() returns -1. The
idea is to read as much good data as we can and jump over bad data.
@return 0 if ok, -1 if error even after the retries, 1 if at the end
of the directory */
int
fil_file_readdir_next_file(
/*=======================*/
	dberr_t*	err,	/*!< out: this is set to DB_ERROR if an error
				was encountered, otherwise not changed */
	const char*	dirname,/*!< in: directory name or path */
	os_file_dir_t	dir,	/*!< in: directory stream */
	os_file_stat_t*	info)	/*!< in/out: buffer where the
				info is returned */
{
	for (ulint i = 0; i < 100; i++) {
		int	ret = os_file_readdir_next_file(dirname, dir, info);

		if (ret != -1) {

			return(ret);
		}

		ib::error() << "os_file_readdir_next_file() returned -1 in"
			" directory " << dirname
			<< ", crash recovery may have failed"
			" for some .ibd files!";

		*err = DB_ERROR;
	}

	return(-1);
}

/** Try to adjust FSP_SPACE_FLAGS if they differ from the expectations.
(Typically when upgrading from MariaDB 10.1.0..10.1.20.)
@param[in]	space_id	tablespace ID
@param[in]	flags		desired tablespace flags */
UNIV_INTERN
void
fsp_flags_try_adjust(ulint space_id, ulint flags)
{
	ut_ad(!srv_read_only_mode);
	ut_ad(fsp_flags_is_valid(flags, space_id));
	if (!fil_space_get_size(space_id)) {
		return;
	}
	mtr_t	mtr;
	mtr.start();
	if (buf_block_t* b = buf_page_get(
		    page_id_t(space_id, 0), page_size_t(flags),
		    RW_X_LATCH, &mtr)) {
		ulint f = fsp_header_get_flags(b->frame);
		/* Suppress the message if only the DATA_DIR flag to differs. */
		if ((f ^ flags) & ~(1U << FSP_FLAGS_POS_RESERVED)) {
			ib::warn()
				<< "adjusting FSP_SPACE_FLAGS of tablespace "
				<< space_id
				<< " from " << ib::hex(f)
				<< " to " << ib::hex(flags);
		}
		if (f != flags) {
			mtr.set_named_space(space_id);
			mlog_write_ulint(FSP_HEADER_OFFSET
					 + FSP_SPACE_FLAGS + b->frame,
					 flags, MLOG_4BYTES, &mtr);
		}
	}
	mtr.commit();
}

/** Determine if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id		Tablespace ID
@param[in]	name		Tablespace name used in fil_space_create().
@param[in]	table_flags	table flags
@return true if a matching tablespace exists in the memory cache */
bool
fil_space_for_table_exists_in_mem(
	ulint		id,
	const char*	name,
	ulint		table_flags)
{
	fil_space_t*	space;

	const ulint	expected_flags = dict_tf_to_fsp_flags(table_flags);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same id */

	space = fil_space_get_by_id(id);

	/* Look if there is a space with the same name; the name is the
	directory path from the datadir to the file */

	const bool valid = space
		&& !((space->flags ^ expected_flags) & ~FSP_FLAGS_MEM_MASK)
		&& space == fil_space_get_by_name(name);

	if (valid) {
		/* Adjust the flags that are in FSP_FLAGS_MEM_MASK.
		FSP_SPACE_FLAGS will not be written back here. */
		space->flags = expected_flags;
	}
	mutex_exit(&fil_system->mutex);

	if (valid && !srv_read_only_mode) {
		fsp_flags_try_adjust(id, expected_flags & ~FSP_FLAGS_MEM_MASK);
	}

	return(valid);
}

/** Return the space ID based on the tablespace name.
The tablespace must be found in the tablespace memory cache.
This call is made from external to this module, so the mutex is not owned.
@param[in]	tablespace	Tablespace name
@return space ID if tablespace found, ULINT_UNDEFINED if space not. */
ulint
fil_space_get_id_by_name(
	const char*	tablespace)
{
	mutex_enter(&fil_system->mutex);

	/* Search for a space with the same name. */
	fil_space_t*	space = fil_space_get_by_name(tablespace);
	ulint		id = (space == NULL) ? ULINT_UNDEFINED : space->id;

	mutex_exit(&fil_system->mutex);

	return(id);
}

/*========== RESERVE FREE EXTENTS (for a B-tree split, for example) ===*/

/*******************************************************************//**
Tries to reserve free extents in a file space.
@return true if succeed */
bool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve)	/*!< in: how many one wants to reserve */
{
	fil_space_t*	space;
	bool		success;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	if (space->n_reserved_extents + n_to_reserve > n_free_now) {
		success = false;
	} else {
		space->n_reserved_extents += n_to_reserve;
		success = true;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/*******************************************************************//**
Releases free extents in a file space. */
void
fil_space_release_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_reserved)	/*!< in: how many one reserved */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);
	ut_a(space->n_reserved_extents >= n_reserved);

	space->n_reserved_extents -= n_reserved;

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Gets the number of reserved extents. If the database is silent, this number
should be zero. */
ulint
fil_space_get_n_reserved_extents(
/*=============================*/
	ulint	id)		/*!< in: space id */
{
	fil_space_t*	space;
	ulint		n;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	n = space->n_reserved_extents;

	mutex_exit(&fil_system->mutex);

	return(n);
}

/*============================ FILE I/O ================================*/

/********************************************************************//**
NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file node for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the node and the system appropriately. Takes the node
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
/*====================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space)	/*!< in: space */
{
	ut_ad(node && system && space);
	ut_ad(mutex_own(&(system->mutex)));

	if (system->n_open > system->max_n_open + 5) {
		ib::warn() << "Open files " << system->n_open
			<< " exceeds the limit " << system->max_n_open;
	}

	if (!node->is_open()) {
		/* File is closed: open it */
		ut_a(node->n_pending == 0);

		if (!fil_node_open_file(node)) {
			return(false);
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(space)) {
		/* The node is in the LRU list, remove it */

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		UT_LIST_REMOVE(system->LRU, node);
	}

	node->n_pending++;

	return(true);
}

/** Update the data structures when an i/o operation finishes.
@param[in,out] node		file node
@param[in] type			IO context */
static
void
fil_node_complete_io(fil_node_t* node, const IORequest& type)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(node->n_pending > 0);

	--node->n_pending;

	ut_ad(type.validate());

	if (type.is_write()) {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(node->space->id));

		if (fil_buffering_disabled(node->space)) {

			/* We don't need to keep track of unflushed
			changes as user has explicitly disabled
			buffering. */
			ut_ad(!node->space->is_in_unflushed_spaces);
			ut_ad(node->needs_flush == false);

		} else {
			node->needs_flush = true;

			if (!node->space->is_in_unflushed_spaces) {

				fil_system->unflushed_spaces.push_front(
					*node->space);
				node->space->is_in_unflushed_spaces = true;
			}
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(node->space)) {

		/* The node must be put back to the LRU list */
		UT_LIST_ADD_FIRST(fil_system->LRU, node);
	}
}

/** Compose error message about an invalid page access.
@param[in]	block_offset	block offset
@param[in]	space_id	space id
@param[in]	space_name	space name
@param[in]	byte_offset	byte offset
@param[in]	len		I/O length
@param[in]	is_read		I/O type
@return	std::string with error message */
static std::string fil_invalid_page_access_msg(size_t block_offset,
                                               size_t space_id,
                                               const char *space_name,
                                               size_t byte_offset, size_t len,
                                               bool is_read)
{
  std::stringstream ss;
  ss << "Trying to " << (is_read ? "read" : "write") << " page number "
     << block_offset << " in space " << space_id << ", space name "
     << space_name << ", which is outside the tablespace bounds. Byte offset "
     << byte_offset << ", len " << len
     << (space_id == 0 && !srv_was_started
             ? "Please check that the configuration matches"
               " the InnoDB system tablespace location (ibdata files)"
             : "");
  return ss.str();
}

/** Reads or writes data. This operation could be asynchronous (aio).

@param[in,out] type	IO context
@param[in] sync		true if synchronous aio is desired
@param[in] page_id	page id
@param[in] page_size	page size
@param[in] byte_offset	remainder of offset in bytes; in aio this
			must be divisible by the OS block size
@param[in] len		how many bytes to read or write; this must
			not cross a file boundary; in aio this must
			be a block size multiple
@param[in,out] buf	buffer where to store read data or from where
			to write; in aio this must be appropriately
			aligned
@param[in] message	message for aio handler if non-sync aio
			used, else ignored
@param[in] ignore_missing_space true=ignore missing space duging read
@return DB_SUCCESS, DB_TABLESPACE_DELETED or DB_TABLESPACE_TRUNCATED
	if we are trying to do i/o on a tablespace which does not exist */
dberr_t
fil_io(
	const IORequest&	type,
	bool			sync,
	const page_id_t		page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message,
	bool			ignore_missing_space)
{
	os_offset_t		offset;
	IORequest		req_type(type);

	ut_ad(req_type.validate());

	ut_ad(len > 0);
	ut_ad(byte_offset < UNIV_PAGE_SIZE);
	ut_ad(!page_size.is_compressed() || byte_offset == 0);
	ut_ad(UNIV_PAGE_SIZE == (ulong)(1 << UNIV_PAGE_SIZE_SHIFT));
#if (1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX"
#endif
#if (1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN"
#endif
	ut_ad(fil_validate_skip());

	/* ibuf bitmap pages must be read in the sync AIO mode: */
	ut_ad(recv_no_ibuf_operations
	      || req_type.is_write()
	      || !ibuf_bitmap_page(page_id, page_size)
	      || sync
	      || req_type.is_log());

	ulint	mode;

	if (sync) {

		mode = OS_AIO_SYNC;

	} else if (req_type.is_log()) {

		mode = OS_AIO_LOG;

	} else if (req_type.is_read()
		   && !recv_no_ibuf_operations
		   && ibuf_page(page_id, page_size, NULL)) {

		mode = OS_AIO_IBUF;

		/* Reduce probability of deadlock bugs in connection with ibuf:
		do not let the ibuf i/o handler sleep */

		req_type.clear_do_not_wake();
	} else {
		mode = OS_AIO_NORMAL;
	}

	if (req_type.is_read()) {

		srv_stats.data_read.add(len);

	} else if (req_type.is_write()) {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(page_id.space()));

		srv_stats.data_written.add(len);
	}

	/* Reserve the fil_system mutex and make sure that we can open at
	least one file while holding it, if the file is not already open */

	fil_mutex_enter_and_prepare_for_io(page_id.space());

	fil_space_t*	space = fil_space_get_by_id(page_id.space());

	/* If we are deleting a tablespace we don't allow async read operations
	on that. However, we do allow write operations and sync read operations. */
	if (space == NULL
	    || (req_type.is_read()
		&& !sync
		&& space->stop_new_ops
		&& !space->is_being_truncated)) {

		mutex_exit(&fil_system->mutex);

		if (!req_type.ignore_missing() && !ignore_missing_space) {
			ib::error()
				<< "Trying to do I/O to a tablespace which"
				" does not exist. I/O type: "
				<< (req_type.is_read() ? "read" : "write")
				<< ", page: " << page_id
				<< ", I/O length: " << len << " bytes";
		}

		return(DB_TABLESPACE_DELETED);
	}

	ut_ad(mode != OS_AIO_IBUF || fil_type_is_data(space->purpose));

	ulint		cur_page_no = page_id.page_no();
	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	for (;;) {

		if (node == NULL) {

			if (req_type.ignore_missing()) {
				mutex_exit(&fil_system->mutex);
				return(DB_ERROR);
			}

			if (space->purpose == FIL_TYPE_IMPORT) {
				mutex_exit(&fil_system->mutex);
				ib::error() << fil_invalid_page_access_msg(
					page_id.page_no(), page_id.space(),
					space->name, byte_offset, len,
					req_type.is_read());

				return DB_IO_ERROR;
			}

			ib::fatal() << fil_invalid_page_access_msg(
				page_id.page_no(), page_id.space(),
				space->name, byte_offset, len,
				req_type.is_read());

		} else if (fil_is_user_tablespace_id(space->id)
			   && node->size == 0) {

			/* We do not know the size of a single-table tablespace
			before we open the file */
			break;

		} else if (node->size > cur_page_no) {
			/* Found! */
			break;

		} else {
			if (space->id != TRX_SYS_SPACE
			    && UT_LIST_GET_LEN(space->chain) == 1
			    && (srv_is_tablespace_truncated(space->id)
				|| space->is_being_truncated
				|| srv_was_tablespace_truncated(space))
			    && req_type.is_read()) {

				/* Handle page which is outside the truncated
				tablespace bounds when recovering from a crash
				happened during a truncation */
				mutex_exit(&fil_system->mutex);
				return(DB_TABLESPACE_TRUNCATED);
			}

			cur_page_no -= node->size;

			node = UT_LIST_GET_NEXT(chain, node);
		}
	}

	/* Open file if closed */
	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		if (fil_type_is_data(space->purpose)
		    && fil_is_user_tablespace_id(space->id)) {
			mutex_exit(&fil_system->mutex);

			if (!req_type.ignore_missing()) {
				ib::error()
					<< "Trying to do I/O to a tablespace"
					" which exists without .ibd data file."
					" I/O type: "
					<< (req_type.is_read()
					    ? "read" : "write")
					<< ", page: "
					<< page_id_t(page_id.space(),
						     cur_page_no)
					<< ", I/O length: " << len << " bytes";
			}

			return(DB_TABLESPACE_DELETED);
		}

		/* The tablespace is for log. Currently, we just assert here
		to prevent handling errors along the way fil_io returns.
		Also, if the log files are missing, it would be hard to
		promise the server can continue running. */
		ut_a(0);
	}

	/* Check that at least the start offset is within the bounds of a
	single-table tablespace, including rollback tablespaces. */
	if (node->size <= cur_page_no
	    && space->id != TRX_SYS_SPACE
	    && fil_type_is_data(space->purpose)) {

		if (req_type.ignore_missing()) {
			/* If we can tolerate the non-existent pages, we
			should return with DB_ERROR and let caller decide
			what to do. */
			fil_node_complete_io(node, req_type);
			mutex_exit(&fil_system->mutex);
			return(DB_ERROR);
		}

		ib::fatal() << fil_invalid_page_access_msg(
			page_id.page_no(), page_id.space(),
			space->name, byte_offset, len, req_type.is_read());
	}

	/* Now we have made the changes in the data structures of fil_system */
	mutex_exit(&fil_system->mutex);

	/* Calculate the low 32 bits and the high 32 bits of the file offset */

	if (!page_size.is_compressed()) {

		offset = ((os_offset_t) cur_page_no
			  << UNIV_PAGE_SIZE_SHIFT) + byte_offset;

		ut_a(node->size - cur_page_no
		     >= ((byte_offset + len + (UNIV_PAGE_SIZE - 1))
			 / UNIV_PAGE_SIZE));
	} else {
		ulint	size_shift;

		switch (page_size.physical()) {
		case 1024: size_shift = 10; break;
		case 2048: size_shift = 11; break;
		case 4096: size_shift = 12; break;
		case 8192: size_shift = 13; break;
		case 16384: size_shift = 14; break;
		case 32768: size_shift = 15; break;
		case 65536: size_shift = 16; break;
		default: ut_error;
		}

		offset = ((os_offset_t) cur_page_no << size_shift)
			+ byte_offset;

		ut_a(node->size - cur_page_no
		     >= (len + (page_size.physical() - 1))
		     / page_size.physical());
	}

	/* Do AIO */

	ut_a(byte_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a((len % OS_FILE_LOG_BLOCK_SIZE) == 0);

	const char* name = node->name == NULL ? space->name : node->name;

	req_type.set_fil_node(node);

	ut_ad(!req_type.is_write()
	      || page_id.space() == SRV_LOG_SPACE_FIRST_ID
	      || !fil_is_user_tablespace_id(page_id.space())
	      || offset == page_id.page_no() * page_size.physical());

	/* Queue the aio request */
	dberr_t err = os_aio(
		req_type,
		mode, name, node->handle, buf, offset, len,
		space->purpose != FIL_TYPE_TEMPORARY
		&& srv_read_only_mode,
		node, message);

	/* We an try to recover the page from the double write buffer if
	the decompression fails or the page is corrupt. */

	ut_a(req_type.is_dblwr_recover() || err == DB_SUCCESS);

	if (sync) {
		/* The i/o operation is already completed when we return from
		os_aio: */

		mutex_enter(&fil_system->mutex);

		fil_node_complete_io(node, req_type);

		mutex_exit(&fil_system->mutex);

		ut_ad(fil_validate_skip());
	}

	return(err);
}

/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */
void
fil_aio_wait(
/*=========*/
	ulint	segment)	/*!< in: the number of the segment in the aio
				array to wait for */
{
	fil_node_t*	node;
	IORequest	type;
	void*		message;

	ut_ad(fil_validate_skip());

	dberr_t	err = os_aio_handler(segment, &node, &message, &type);

	ut_a(err == DB_SUCCESS);

	if (node == NULL) {
		ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
		return;
	}

	srv_set_io_thread_op_info(segment, "complete io for fil node");

	mutex_enter(&fil_system->mutex);

	fil_node_complete_io(node, type);
	const fil_type_t	purpose	= node->space->purpose;
	const ulint		space_id= node->space->id;
	const bool		dblwr	= node->space->use_doublewrite();

	mutex_exit(&fil_system->mutex);

	ut_ad(fil_validate_skip());

	/* Do the i/o handling */
	/* IMPORTANT: since i/o handling for reads will read also the insert
	buffer in tablespace 0, you have to be very careful not to introduce
	deadlocks in the i/o system. We keep tablespace 0 data files always
	open, and use a special i/o thread to serve insert buffer requests. */

	switch (purpose) {
	case FIL_TYPE_LOG:
		srv_set_io_thread_op_info(segment, "complete io for log");
		log_io_complete(static_cast<log_group_t*>(message));
		return;
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_IMPORT:
		srv_set_io_thread_op_info(segment, "complete io for buf page");

		/* async single page writes from the dblwr buffer don't have
		access to the page */
		buf_page_t* bpage = static_cast<buf_page_t*>(message);
		if (!bpage) {
			return;
		}

		ulint offset = bpage->id.page_no();
		dberr_t err = buf_page_io_complete(bpage, dblwr);
		if (err == DB_SUCCESS) {
			return;
		}

		ut_ad(type.is_read());
		if (recv_recovery_is_on() && !srv_force_recovery) {
			recv_sys->found_corrupt_fs = true;
		}

		if (fil_space_t* space = fil_space_acquire_for_io(space_id)) {
			if (space == node->space) {
				ib::error() << "Failed to read file '"
					    << node->name
					    << "' at offset " << offset
					    << ": " << err;
			}

			fil_space_release_for_io(space);
		}
		return;
	}

	ut_ad(0);
}

/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */
void
fil_flush(
/*======*/
	ulint	space_id)	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
{
	mutex_enter(&fil_system->mutex);

	if (fil_space_t* space = fil_space_get_by_id(space_id)) {
		if (space->purpose != FIL_TYPE_TEMPORARY
		    && !space->is_stopping()) {
			fil_flush_low(space);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Flush a tablespace.
@param[in,out]	space	tablespace to flush */
void
fil_flush(fil_space_t* space)
{
	ut_ad(space->n_pending_ios > 0);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE
	      || space->purpose == FIL_TYPE_IMPORT);

	if (!space->is_stopping()) {
		mutex_enter(&fil_system->mutex);
		if (!space->is_stopping()) {
			fil_flush_low(space);
		}
		mutex_exit(&fil_system->mutex);
	}
}

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG */
void
fil_flush_file_spaces(
	fil_type_t	purpose)
{
	ulint*		space_ids;
	ulint		n_space_ids;

	ut_ad(purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_LOG);

	mutex_enter(&fil_system->mutex);

	n_space_ids = fil_system->unflushed_spaces.size();
	if (n_space_ids == 0) {

		mutex_exit(&fil_system->mutex);
		return;
	}

	space_ids = static_cast<ulint*>(
		ut_malloc_nokey(n_space_ids * sizeof(*space_ids)));

	n_space_ids = 0;

	for (sized_ilist<fil_space_t, unflushed_spaces_tag_t>::iterator it
	     = fil_system->unflushed_spaces.begin(),
	     end = fil_system->unflushed_spaces.end();
	     it != end; ++it) {

		if (it->purpose == purpose && !it->is_stopping()) {
			space_ids[n_space_ids++] = it->id;
		}
	}

	mutex_exit(&fil_system->mutex);

	/* Flush the spaces.  It will not hurt to call fil_flush() on
	a non-existing space id. */
	for (ulint i = 0; i < n_space_ids; i++) {

		fil_flush(space_ids[i]);
	}

	ut_free(space_ids);
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
		ut_a(elem->is_open() || !elem->n_pending);
		n_open += elem->is_open();
		size += elem->size;
	}

	/** Validate a tablespace.
	@param[in]	space	tablespace to validate
	@return		number of open file nodes */
	static ulint validate(const fil_space_t* space)
	{
		ut_ad(mutex_own(&fil_system->mutex));
		Check	check;
		ut_list_validate(space->chain, check);
		ut_a(space->size == check.size);
		return(check.n_open);
	}
};

/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */
bool
fil_validate(void)
/*==============*/
{
	fil_node_t*	fil_node;
	ulint		n_open		= 0;

	mutex_enter(&fil_system->mutex);

	for (fil_space_t *space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {
		n_open += Check::validate(space);
	}

	ut_a(fil_system->n_open == n_open);

	ut_list_validate(fil_system->LRU);

	for (fil_node = UT_LIST_GET_FIRST(fil_system->LRU);
	     fil_node != 0;
	     fil_node = UT_LIST_GET_NEXT(LRU, fil_node)) {

		ut_a(fil_node->n_pending == 0);
		ut_a(!fil_node->being_extended);
		ut_a(fil_node->is_open());
		ut_a(fil_space_belongs_in_lru(fil_node->space));
	}

	mutex_exit(&fil_system->mutex);

	return(true);
}

/********************************************************************//**
Returns true if file address is undefined.
@return true if undefined */
bool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr)	/*!< in: address */
{
	return(addr.page == FIL_NULL);
}

/********************************************************************//**
Get the predecessor of a file page.
@return FIL_PAGE_PREV */
ulint
fil_page_get_prev(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

/********************************************************************//**
Get the successor of a file page.
@return FIL_PAGE_NEXT */
ulint
fil_page_get_next(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_NEXT));
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

/****************************************************************//**
Closes the tablespace memory cache. */
void
fil_close(void)
/*===========*/
{
	if (fil_system) {
		hash_table_free(fil_system->spaces);

		hash_table_free(fil_system->name_hash);

		ut_a(UT_LIST_GET_LEN(fil_system->LRU) == 0);
		ut_a(fil_system->unflushed_spaces.size() == 0);
		ut_a(UT_LIST_GET_LEN(fil_system->space_list) == 0);

		mutex_free(&fil_system->mutex);

		delete fil_system;
		fil_system = NULL;

		fil_space_crypt_cleanup();
	}
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

/** Generate redo log for swapping two .ibd files
@param[in]	old_table	old table
@param[in]	new_table	new table
@param[in]	tmp_name	temporary table name
@param[in,out]	mtr		mini-transaction
@return innodb error code */
dberr_t
fil_mtr_rename_log(
	const dict_table_t*	old_table,
	const dict_table_t*	new_table,
	const char*		tmp_name,
	mtr_t*			mtr)
{
	dberr_t	err;

	bool	old_is_file_per_table =
		!is_system_tablespace(old_table->space);

	bool	new_is_file_per_table =
		!is_system_tablespace(new_table->space);

	/* If neither table is file-per-table,
	there will be no renaming of files. */
	if (!old_is_file_per_table && !new_is_file_per_table) {
		return(DB_SUCCESS);
	}

	const char*	old_dir = DICT_TF_HAS_DATA_DIR(old_table->flags)
		? old_table->data_dir_path
		: NULL;

	char*	old_path = fil_make_filepath(
		old_dir, old_table->name.m_name, IBD, (old_dir != NULL));
	if (old_path == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	if (old_is_file_per_table) {
		char*	tmp_path = fil_make_filepath(
			old_dir, tmp_name, IBD, (old_dir != NULL));
		if (tmp_path == NULL) {
			ut_free(old_path);
			return(DB_OUT_OF_MEMORY);
		}

		/* Temp filepath must not exist. */
		err = fil_rename_tablespace_check(
			old_table->space, old_path, tmp_path,
			dict_table_is_discarded(old_table));
		if (err != DB_SUCCESS) {
			ut_free(old_path);
			ut_free(tmp_path);
			return(err);
		}

		fil_name_write_rename_low(
			old_table->space, 0, old_path, tmp_path, mtr);

		ut_free(tmp_path);
	}

	if (new_is_file_per_table) {
		const char*	new_dir = DICT_TF_HAS_DATA_DIR(new_table->flags)
			? new_table->data_dir_path
			: NULL;
		char*	new_path = fil_make_filepath(
				new_dir, new_table->name.m_name,
				IBD, (new_dir != NULL));
		if (new_path == NULL) {
			ut_free(old_path);
			return(DB_OUT_OF_MEMORY);
		}

		/* Destination filepath must not exist unless this ALTER
		TABLE starts and ends with a file_per-table tablespace. */
		if (!old_is_file_per_table) {
			err = fil_rename_tablespace_check(
				new_table->space, new_path, old_path,
				dict_table_is_discarded(new_table));
			if (err != DB_SUCCESS) {
				ut_free(old_path);
				ut_free(new_path);
				return(err);
			}
		}

		fil_name_write_rename_low(
			new_table->space, 0, new_path, old_path, mtr);

		ut_free(new_path);
	}

	ut_free(old_path);

	return(DB_SUCCESS);
}

#ifdef UNIV_DEBUG
/** Check that a tablespace is valid for mtr_commit().
@param[in]	space	persistent tablespace that has been changed */
static
void
fil_space_validate_for_mtr_commit(
	const fil_space_t*	space)
{
	ut_ad(!mutex_own(&fil_system->mutex));
	ut_ad(space != NULL);
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);
	ut_ad(!is_predefined_tablespace(space->id));

	/* We are serving mtr_commit(). While there is an active
	mini-transaction, we should have !space->stop_new_ops. This is
	guaranteed by meta-data locks or transactional locks, or
	dict_operation_lock (X-lock in DROP, S-lock in purge).

	However, a file I/O thread can invoke change buffer merge
	while fil_check_pending_operations() is waiting for operations
	to quiesce. This is not a problem, because
	ibuf_merge_or_delete_for_page() would call
	fil_space_acquire() before mtr_start() and
	fil_space_release() after mtr_commit(). This is why
	n_pending_ops should not be zero if stop_new_ops is set. */
	ut_ad(!space->stop_new_ops
	      || space->is_being_truncated /* fil_truncate_prepare() */
	      || space->n_pending_ops > 0);
}
#endif /* UNIV_DEBUG */

/** Write a MLOG_FILE_NAME record for a persistent tablespace.
@param[in]	space	tablespace
@param[in,out]	mtr	mini-transaction */
static
void
fil_names_write(
	const fil_space_t*	space,
	mtr_t*			mtr)
{
	ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
	fil_name_write(space, 0, UT_LIST_GET_FIRST(space->chain), mtr);
}

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space)
{
	ut_ad(log_mutex_own());
	ut_ad(recv_recovery_is_on());
	ut_ad(log_sys->lsn != 0);
	ut_ad(space->max_lsn == 0);
	ut_d(fil_space_validate_for_mtr_commit(space));

	UT_LIST_ADD_LAST(fil_system->named_spaces, space);
	space->max_lsn = log_sys->lsn;
}

/** Write MLOG_FILE_NAME records when a non-predefined persistent
tablespace was modified for the first time since the latest
fil_names_clear().
@param[in,out]	space	tablespace
@param[in,out]	mtr	mini-transaction */
void
fil_names_dirty_and_write(
	fil_space_t*	space,
	mtr_t*		mtr)
{
	ut_ad(log_mutex_own());
	ut_d(fil_space_validate_for_mtr_commit(space));
	ut_ad(space->max_lsn == log_sys->lsn);

	UT_LIST_ADD_LAST(fil_system->named_spaces, space);
	fil_names_write(space, mtr);

	DBUG_EXECUTE_IF("fil_names_write_bogus",
			{
				char bogus_name[] = "./test/bogus file.ibd";
				os_normalize_path(bogus_name);
				fil_name_write(
					SRV_LOG_SPACE_FIRST_ID, 0,
					bogus_name, mtr);
			});
}

/** On a log checkpoint, reset fil_names_dirty_and_write() flags
and write out MLOG_FILE_NAME and MLOG_CHECKPOINT if needed.
@param[in]	lsn		checkpoint LSN
@param[in]	do_write	whether to always write MLOG_CHECKPOINT
@return whether anything was written to the redo log
@retval false	if no flags were set and nothing written
@retval true	if anything was written to the redo log */
bool
fil_names_clear(
	lsn_t	lsn,
	bool	do_write)
{
	mtr_t	mtr;
	ulint	mtr_checkpoint_size = LOG_CHECKPOINT_FREE_PER_THREAD;

	DBUG_EXECUTE_IF(
		"increase_mtr_checkpoint_size",
		mtr_checkpoint_size = 75 * 1024;
		);

	ut_ad(log_mutex_own());

	if (log_sys->append_on_checkpoint) {
		mtr_write_log(log_sys->append_on_checkpoint);
		do_write = true;
	}

	mtr.start();

	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system->named_spaces);
	     space != NULL; ) {
		fil_space_t*	next = UT_LIST_GET_NEXT(named_spaces, space);

		ut_ad(space->max_lsn > 0);
		if (space->max_lsn < lsn) {
			/* The tablespace was last dirtied before the
			checkpoint LSN. Remove it from the list, so
			that if the tablespace is not going to be
			modified any more, subsequent checkpoints will
			avoid calling fil_names_write() on it. */
			space->max_lsn = 0;
			UT_LIST_REMOVE(fil_system->named_spaces, space);
		}

		/* max_lsn is the last LSN where fil_names_dirty_and_write()
		was called. If we kept track of "min_lsn" (the first LSN
		where max_lsn turned nonzero), we could avoid the
		fil_names_write() call if min_lsn > lsn. */

		fil_names_write(space, &mtr);
		do_write = true;

		const mtr_buf_t* mtr_log = mtr_get_log(&mtr);

		/** If the mtr buffer size exceeds the size of
		LOG_CHECKPOINT_FREE_PER_THREAD then commit the multi record
		mini-transaction, start the new mini-transaction to
		avoid the parsing buffer overflow error during recovery. */

		if (mtr_log->size() > mtr_checkpoint_size) {
			ut_ad(mtr_log->size() < (RECV_PARSING_BUF_SIZE / 2));
			mtr.commit_checkpoint(lsn, false);
			mtr.start();
		}

		space = next;
	}

	if (do_write) {
		mtr.commit_checkpoint(lsn, true);
	} else {
		ut_ad(!mtr.has_modifications());
	}

	return(do_write);
}

/** Truncate a single-table tablespace. The tablespace must be cached
in the memory cache.
@param space_id			space id
@param dir_path			directory path
@param tablename		the table name in the usual
				databasename/tablename format of InnoDB
@param flags			tablespace flags
@param trunc_to_default		truncate to default size if tablespace
				is being newly re-initialized.
@return DB_SUCCESS or error */
dberr_t
truncate_t::truncate(
/*=================*/
	ulint		space_id,
	const char*	dir_path,
	const char*	tablename,
	ulint		flags,
	bool		trunc_to_default)
{
	dberr_t		err = DB_SUCCESS;
	char*		path;

	ut_a(!is_system_tablespace(space_id));

	if (FSP_FLAGS_HAS_DATA_DIR(flags)) {
		ut_ad(dir_path != NULL);
		path = fil_make_filepath(dir_path, tablename, IBD, true);
	} else {
		path = fil_make_filepath(NULL, tablename, IBD, false);
	}

	if (path == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	if (trunc_to_default) {
		space->size = node->size = FIL_IBD_FILE_INITIAL_SIZE;
	}

	const bool already_open = node->is_open();

	if (!already_open) {

		bool	ret;

		node->handle = os_file_create_simple_no_error_handling(
			innodb_data_file_key, path, OS_FILE_OPEN,
			OS_FILE_READ_WRITE,
			fsp_is_system_temporary(space_id)
			? false : srv_read_only_mode, &ret);

		if (!ret) {
			ib::error() << "Failed to open tablespace file "
				<< path << ".";

			ut_free(path);

			return(DB_ERROR);
		}

		ut_a(node->is_open());
	}

	os_offset_t	trunc_size = trunc_to_default
		? FIL_IBD_FILE_INITIAL_SIZE
		: space->size;

	const bool success = os_file_truncate(
		path, node->handle, trunc_size * UNIV_PAGE_SIZE);

	if (!success) {
		ib::error() << "Cannot truncate file " << path
			<< " in TRUNCATE TABLESPACE.";
		err = DB_ERROR;
	}

	space->stop_new_ops = false;
	space->is_being_truncated = false;

	/* If we opened the file in this function, close it. */
	if (!already_open) {
		bool	closed = os_file_close(node->handle);

		if (!closed) {

			ib::error() << "Failed to close tablespace file "
				<< path << ".";

			err = DB_ERROR;
		} else {
			node->handle = OS_FILE_CLOSED;
		}
	}

	mutex_exit(&fil_system->mutex);

	ut_free(path);

	return(err);
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

/** Release the reserved free extents.
@param[in]	n_reserved	number of reserved extents */
void
fil_space_t::release_free_extents(ulint	n_reserved)
{
	ut_ad(rw_lock_own(&latch, RW_LOCK_X));

	ut_a(n_reserved_extents >= n_reserved);
	n_reserved_extents -= n_reserved;
}

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

/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
fil_space_t*
fil_space_found_by_id(
/*==================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t* space = NULL;
	mutex_enter(&fil_system->mutex);
	space = fil_space_get_by_id(id);

	/* Not found if space is being deleted */
	if (space && space->stop_new_ops) {
		space = NULL;
	}

	mutex_exit(&fil_system->mutex);
	return space;
}

/**
Get should we punch hole to tablespace.
@param[in]	node		File node
@return true, if punch hole should be tried, false if not. */
bool
fil_node_should_punch_hole(
	const fil_node_t*	node)
{
	return (node->space->punch_hole);
}

/**
Set punch hole to tablespace to given value.
@param[in]	node		File node
@param[in]	val		value to be set. */
void
fil_space_set_punch_hole(
	fil_node_t*		node,
	bool			val)
{
	node->space->punch_hole = val;
}
