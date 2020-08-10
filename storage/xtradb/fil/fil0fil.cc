/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2020, MariaDB Corporation.

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

#include <debug_sync.h>
#include <my_dbug.h>

#include "mem0mem.h"
#include "hash0hash.h"
#include "os0file.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "page0page.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "row0mysql.h"
#ifndef UNIV_HOTBACKUP
# include "buf0lru.h"
# include "ibuf0ibuf.h"
# include "sync0sync.h"
#else /* !UNIV_HOTBACKUP */
# include "srv0srv.h"
static ulint srv_data_read, srv_data_written;
#endif /* !UNIV_HOTBACKUP */

#include "zlib.h"
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#endif
#include "row0mysql.h"
#include "trx0purge.h"

MYSQL_PLUGIN_IMPORT extern my_bool lower_case_file_system;


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

/** When mysqld is run, the default directory "." is the mysqld datadir,
but in the MySQL Embedded Server Library and mysqlbackup it is not the default
directory, and we must set the base file path explicitly */
UNIV_INTERN const char*	fil_path_to_mysql_datadir	= ".";

/** The number of fsyncs done to the log */
UNIV_INTERN ulint	fil_n_log_flushes			= 0;

/** Number of pending redo log flushes */
UNIV_INTERN ulint	fil_n_pending_log_flushes		= 0;
/** Number of pending tablespace flushes */
UNIV_INTERN ulint	fil_n_pending_tablespace_flushes	= 0;

/** Number of files currently open */
UNIV_INTERN ulint	fil_n_file_opened			= 0;

/** The null file address */
UNIV_INTERN fil_addr_t	fil_addr_null = {FIL_NULL, 0};

#ifdef UNIV_PFS_MUTEX
/* Key to register fil_system_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	fil_system_mutex_key;
#endif /* UNIV_PFS_MUTEX */

#ifdef UNIV_PFS_RWLOCK
/* Key to register file space latch with performance schema */
UNIV_INTERN mysql_pfs_key_t	fil_space_latch_key;
#endif /* UNIV_PFS_RWLOCK */

/** The tablespace memory cache. This variable is NULL before the module is
initialized. */
UNIV_INTERN fil_system_t*	fil_system	= NULL;

/** At this age or older a space/page will be rotated */
UNIV_INTERN extern uint srv_fil_crypt_rotate_key_age;
UNIV_INTERN extern ib_mutex_t fil_crypt_threads_mutex;

/** Determine if (i) is a user tablespace id or not. */
# define fil_is_user_tablespace_id(i) (i != 0 \
				       && !srv_is_undo_tablespace(i))

/** Determine if user has explicitly disabled fsync(). */
#ifndef __WIN__
# define fil_buffering_disabled(s)					\
	(((s)->purpose == FIL_TABLESPACE				\
	    && srv_unix_file_flush_method == SRV_UNIX_O_DIRECT_NO_FSYNC)\
	  || ((s)->purpose == FIL_LOG					\
	    && srv_unix_file_flush_method == SRV_UNIX_ALL_O_DIRECT))

#else /* __WIN__ */
# define fil_buffering_disabled(s)	(0)
#endif /* __WIN__ */

#ifdef UNIV_DEBUG
/** Try fil_validate() every this many times */
# define FIL_VALIDATE_SKIP	17

/******************************************************************//**
Checks the consistency of the tablespace cache some of the time.
@return	TRUE if ok or the check was skipped */
static
ibool
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
		return(TRUE);
	}

	fil_validate_count = FIL_VALIDATE_SKIP;
	return(fil_validate());
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Determines if a file node belongs to the least-recently-used list.
@return TRUE if the file belongs to fil_system->LRU mutex. */
UNIV_INLINE
ibool
fil_space_belongs_in_lru(
/*=====================*/
	const fil_space_t*	space)	/*!< in: file space */
{
	return(space->purpose == FIL_TABLESPACE
	       && fil_is_user_tablespace_id(space->id));
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
/********************************************************************//**
Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the node appropriately. */
static
void
fil_node_complete_io(
/*=================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	ulint		type);	/*!< in: OS_FILE_WRITE or OS_FILE_READ; marks
				the node as modified if
				type == OS_FILE_WRITE */
/** Free a space object from the tablespace memory cache. Close the files in
the chain but do not delete them. There must not be any pending i/o's or
flushes on the files.
The fil_system->mutex will be released.
@param[in]	id		tablespace ID
@param[in]	x_latched	whether the caller holds exclusive space->latch
@return whether the tablespace existed */
static
bool
fil_space_free_and_mutex_exit(ulint id, bool x_latched);
/********************************************************************//**
Reads data from a space to a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_read(
/*=====*/
	bool	sync,		/*!< in: true if synchronous aio is desired */
	ulint	space_id,	/*!< in: space id */
	ulint	zip_size,	/*!< in: compressed page size in bytes;
				0 for uncompressed pages */
	ulint	block_offset,	/*!< in: offset in number of blocks */
	ulint	byte_offset,	/*!< in: remainder of offset in bytes; in aio
				this must be divisible by the OS block size */
	ulint	len,		/*!< in: how many bytes to read; this must not
				cross a file boundary; in aio this must be a
				block size multiple */
	void*	buf,		/*!< in/out: buffer where to store data read;
				in aio this must be appropriately aligned */
	void*	message,	/*!< in: message for aio handler if non-sync
				aio used, else ignored */
	ulint*	write_size)	/*!< in/out: Actual write size initialized
				after fist successfull trim
				operation for this page and if
				initialized we do not trim again if
				actual page size does not decrease. */
{
	return(fil_io(OS_FILE_READ, sync, space_id, zip_size, block_offset,
		      byte_offset, len, buf, message, write_size));
}

/********************************************************************//**
Writes data to a space from a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_write(
/*======*/
	bool	sync,		/*!< in: true if synchronous aio is desired */
	ulint	space_id,	/*!< in: space id */
	ulint	zip_size,	/*!< in: compressed page size in bytes;
				0 for uncompressed pages */
	ulint	block_offset,	/*!< in: offset in number of blocks */
	ulint	byte_offset,	/*!< in: remainder of offset in bytes; in aio
				this must be divisible by the OS block size */
	ulint	len,		/*!< in: how many bytes to write; this must
				not cross a file boundary; in aio this must
				be a block size multiple */
	void*	buf,		/*!< in: buffer from which to write; in aio
				this must be appropriately aligned */
	void*	message,	/*!< in: message for aio handler if non-sync
				aio used, else ignored */
	ulint*	write_size)	/*!< in/out: Actual write size initialized
				after fist successfull trim
				operation for this page and if
				initialized we do not trim again if
				actual page size does not decrease. */
{
	ut_ad(!srv_read_only_mode);

	return(fil_io(OS_FILE_WRITE, sync, space_id, zip_size, block_offset,
			byte_offset, len, buf, message, write_size));
}

/*******************************************************************//**
Returns the table space by a given id, NULL if not found.
It is unsafe to dereference the returned pointer. It is fine to check
for NULL.
@param[in]	id		Tablespace id
@return table space or NULL */
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

	/* The system tablespace must always be found */
	ut_ad(space || id != 0 || srv_is_being_started);
	return(space);
}

/*******************************************************************//**
Returns the table space by a given name, NULL if not found. */
fil_space_t*
fil_space_get_by_name(
/*==================*/
	const char*	name)	/*!< in: space name */
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

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Returns the version number of a tablespace, -1 if not found.
@return version number, -1 if the tablespace does not exist in the
memory cache */
UNIV_INTERN
ib_int64_t
fil_space_get_version(
/*==================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ib_int64_t	version		= -1;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space) {
		version = space->tablespace_version;
	}

	mutex_exit(&fil_system->mutex);

	return(version);
}

/*******************************************************************//**
Returns the latch of a file space.
@return	latch protecting storage allocation */
UNIV_INTERN
prio_rw_lock_t*
fil_space_get_latch(
/*================*/
	ulint	id,	/*!< in: space id */
	ulint*	flags)	/*!< out: tablespace flags */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	if (flags) {
		*flags = space->flags;
	}

	mutex_exit(&fil_system->mutex);

	return(&(space->latch));
}

/*******************************************************************//**
Returns the type of a file space.
@return	ULINT_UNDEFINED, or FIL_TABLESPACE or FIL_LOG */
UNIV_INTERN
ulint
fil_space_get_type(
/*===============*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint type = ULINT_UNDEFINED;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	mutex_exit(&fil_system->mutex);

	if (space) {
		type = space->purpose;
	}

	return(type);
}
#endif /* !UNIV_HOTBACKUP */

/**********************************************************************//**
Checks if all the file nodes in a space are flushed. The caller must hold
the fil_system mutex.
@return	true if all are flushed */
static
bool
fil_space_is_flushed(
/*=================*/
	fil_space_t*	space)	/*!< in: space */
{
	fil_node_t*	node;

	ut_ad(mutex_own(&fil_system->mutex));

	node = UT_LIST_GET_FIRST(space->chain);

	while (node) {
		if (node->modification_counter > node->flush_counter) {

			ut_ad(!fil_buffering_disabled(space));
			return(false);
		}

		node = UT_LIST_GET_NEXT(chain, node);
	}

	return(true);
}

/*******************************************************************//**
Appends a new file to the chain of files of a space. File must be closed.
@return pointer to the file name, or NULL on error */
UNIV_INTERN
char*
fil_node_create(
/*============*/
	const char*	name,	/*!< in: file name (file must be closed) */
	ulint		size,	/*!< in: file size in database blocks, rounded
				downwards to an integer */
	ulint		id,	/*!< in: space id where to append */
	ibool		is_raw)	/*!< in: TRUE if a raw device or
				a raw disk partition */
{
	fil_node_t*	node;
	fil_space_t*	space;

	ut_a(fil_system);
	ut_a(name);

	mutex_enter(&fil_system->mutex);

	node = static_cast<fil_node_t*>(mem_zalloc(sizeof(fil_node_t)));

	node->name = mem_strdup(name);

	ut_a(!is_raw || srv_start_raw_disk_in_use);

	node->sync_event = os_event_create();
	node->is_raw_disk = is_raw;
	node->size = size;
	node->magic_n = FIL_NODE_MAGIC_N;

	space = fil_space_get_by_id(id);

	if (!space) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Error: Could not find tablespace %lu for\n"
			"InnoDB: file ", (ulong) id);
		ut_print_filename(stderr, name);
		fputs(" in the tablespace memory cache.\n", stderr);
		mem_free(node->name);

		mem_free(node);

		mutex_exit(&fil_system->mutex);

		return(NULL);
	}

	space->size += size;

	node->space = space;

	UT_LIST_ADD_LAST(chain, space->chain, node);

	if (id < SRV_LOG_SPACE_FIRST_ID && fil_system->max_assigned_id < id) {

		fil_system->max_assigned_id = id;
	}

	mutex_exit(&fil_system->mutex);

	return(node->name);
}

/********************************************************************//**
Opens a file of a node of a tablespace. The caller must own the fil_system
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_open_file(
/*===============*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space)	/*!< in: space */
{
	os_offset_t	size_bytes;
	ibool		ret;
	ibool		success;
	byte*		buf2;
	byte*		page;

	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->n_pending == 0);
	ut_a(node->open == FALSE);

	if (node->size == 0) {
		/* It must be a single-table tablespace and we do not know the
		size of the file yet. First we open the file in the normal
		mode, no async I/O here, for simplicity. Then do some checks,
		and close the file again.
		NOTE that we could not use the simple file read function
		os_file_read() in Windows to read from a file opened for
		async I/O! */

		node->handle = os_file_create_simple_no_error_handling(
			innodb_file_data_key, node->name, OS_FILE_OPEN,
			OS_FILE_READ_ONLY, &success, 0);

		if (!success) {
			/* The following call prints an error message */
			os_file_get_last_error(true);

			ib_logf(IB_LOG_LEVEL_WARN, "InnoDB: Error: cannot "
				"open %s\n. InnoDB: Have you deleted .ibd "
				"files under a running mysqld server?\n",
				node->name);

			return(false);
		}

		size_bytes = os_file_get_size(node->handle);
		ut_a(size_bytes != (os_offset_t) -1);

		node->file_block_size = os_file_get_block_size(
			node->handle, node->name);
		space->file_block_size = node->file_block_size;

#ifdef UNIV_HOTBACKUP
		if (space->id == 0) {
			node->size = (ulint) (size_bytes / UNIV_PAGE_SIZE);
			os_file_close(node->handle);
			goto add_size;
		}
#endif /* UNIV_HOTBACKUP */
		ut_a(space->purpose != FIL_LOG);
		ut_a(fil_is_user_tablespace_id(space->id));

		if (size_bytes < FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The size of the file %s is only " UINT64PF
				" bytes, should be at least " ULINTPF,
				node->name, size_bytes,
				FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE);
			os_file_close(node->handle);
			return(false);
		}

		/* Read the first page of the tablespace */

		buf2 = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));
		/* Align the memory for file i/o if we might have O_DIRECT
		set */
		page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

		success = os_file_read(node->handle, page, 0, UNIV_PAGE_SIZE);
		srv_stats.page0_read.inc();

		const ulint space_id = fsp_header_get_space_id(page);
		ulint flags = fsp_header_get_flags(page);

		/* Try to read crypt_data from page 0 if it is not yet
		read. */
		if (!node->space->crypt_data) {
			const ulint offset = fsp_header_get_crypt_offset(
					fsp_flags_get_zip_size(flags));
			node->space->crypt_data = fil_space_read_crypt_data(space_id, page, offset);
		}

		ut_free(buf2);
		os_file_close(node->handle);

		if (!fsp_flags_is_valid(flags)) {
			ulint cflags = fsp_flags_convert_from_101(flags);
			if (cflags == ULINT_UNDEFINED) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Expected tablespace flags 0x%x"
					" but found 0x%x in the file %s",
					int(space->flags), int(flags),
					node->name);
				return(false);
			}

			flags = cflags;
		}

		if (UNIV_UNLIKELY(space_id != space->id)) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"tablespace id is " ULINTPF " in the data dictionary"
				" but in file %s it is " ULINTPF "!\n",
				space->id, node->name, space_id);
			return(false);
		}

		if (ulint zip_size = fsp_flags_get_zip_size(flags)) {
			node->size = ulint(size_bytes / zip_size);
		} else {
			node->size = ulint(size_bytes / UNIV_PAGE_SIZE);
		}

#ifdef UNIV_HOTBACKUP
add_size:
#endif /* UNIV_HOTBACKUP */
		space->committed_size = space->size += node->size;
	}

	ulint atomic_writes = FSP_FLAGS_GET_ATOMIC_WRITES(space->flags);

	/* printf("Opening file %s\n", node->name); */

	/* Open the file for reading and writing, in Windows normally in the
	unbuffered async I/O mode, though global variables may make
	os_file_create() to fall back to the normal file I/O mode. */

	if (space->purpose == FIL_LOG) {
		node->handle = os_file_create(innodb_file_log_key,
					      node->name, OS_FILE_OPEN,
					      OS_FILE_AIO, OS_LOG_FILE,
					      &ret, atomic_writes);
	} else if (node->is_raw_disk) {
		node->handle = os_file_create(innodb_file_data_key,
					      node->name,
					      OS_FILE_OPEN_RAW,
					      OS_FILE_AIO, OS_DATA_FILE,
					      &ret, atomic_writes);
	} else {
		node->handle = os_file_create(innodb_file_data_key,
					      node->name, OS_FILE_OPEN,
					      OS_FILE_AIO, OS_DATA_FILE,
					      &ret, atomic_writes);
	}

	if (node->file_block_size == 0) {
		node->file_block_size = os_file_get_block_size(
			node->handle, node->name);
		space->file_block_size = node->file_block_size;
	}

	ut_a(ret);

	node->open = TRUE;

	system->n_open++;
	fil_n_file_opened++;

	if (fil_space_belongs_in_lru(space)) {

		/* Put the node to the LRU list */
		UT_LIST_ADD_FIRST(LRU, system->LRU, node);
	}

	return(true);
}

/**********************************************************************//**
Closes a file. */
static
void
fil_node_close_file(
/*================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system)	/*!< in: tablespace memory cache */
{
	ibool	ret;

	ut_ad(node && system);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->open);
	ut_a(node->n_pending == 0);
	ut_a(node->n_pending_flushes == 0);
	ut_a(!node->being_extended);
#ifndef UNIV_HOTBACKUP
	ut_a(node->modification_counter == node->flush_counter
	     || srv_fast_shutdown == 2);
#endif /* !UNIV_HOTBACKUP */

	ret = os_file_close(node->handle);
	ut_a(ret);

	/* printf("Closing file %s\n", node->name); */

	node->open = FALSE;
	ut_a(system->n_open > 0);
	system->n_open--;
	fil_n_file_opened--;

	if (fil_space_belongs_in_lru(node->space)) {

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		/* The node is in the LRU list, remove it */
		UT_LIST_REMOVE(LRU, system->LRU, node);
	}
}

/********************************************************************//**
Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex.
@return TRUE if success, FALSE if should retry later; since i/o's
generally complete in < 100 ms, and as InnoDB writes at most 128 pages
from the buffer pool in a batch, and then immediately flushes the
files, there is a good chance that the next time we find a suitable
node from the LRU list */
static
ibool
fil_try_to_close_file_in_LRU(
/*=========================*/
	ibool	print_info)	/*!< in: if TRUE, prints information why it
				cannot close a file */
{
	fil_node_t*	node;

	ut_ad(mutex_own(&fil_system->mutex));

	if (print_info) {
		fprintf(stderr,
			"InnoDB: fil_sys open file LRU len %lu\n",
			(ulong) UT_LIST_GET_LEN(fil_system->LRU));
	}

	for (node = UT_LIST_GET_LAST(fil_system->LRU);
	     node != NULL;
	     node = UT_LIST_GET_PREV(LRU, node)) {

		if (node->modification_counter == node->flush_counter
		    && node->n_pending_flushes == 0
		    && !node->being_extended) {

			fil_node_close_file(node, fil_system);

			return(TRUE);
		}

		if (!print_info) {
			continue;
		}

		if (node->n_pending_flushes > 0) {
			fputs("InnoDB: cannot close file ", stderr);
			ut_print_filename(stderr, node->name);
			fprintf(stderr, ", because n_pending_flushes %lu\n",
				(ulong) node->n_pending_flushes);
		}

		if (node->modification_counter != node->flush_counter) {
			fputs("InnoDB: cannot close file ", stderr);
			ut_print_filename(stderr, node->name);
			fprintf(stderr,
				", because mod_count %ld != fl_count %ld\n",
				(long) node->modification_counter,
				(long) node->flush_counter);

		}

		if (node->being_extended) {
			fputs("InnoDB: cannot close file ", stderr);
			ut_print_filename(stderr, node->name);
			fprintf(stderr, ", because it is being extended\n");
		}
	}

	return(FALSE);
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
			ut_ad(node->modification_counter
			      == node->flush_counter);
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

		ib_int64_t old_mod_counter = node->modification_counter;

		if (old_mod_counter <= node->flush_counter) {
			continue;
		}

		ut_a(node->open);

		if (space->purpose == FIL_TABLESPACE) {
			fil_n_pending_tablespace_flushes++;
		} else {
			fil_n_pending_log_flushes++;
			fil_n_log_flushes++;
		}
#ifdef __WIN__
		if (node->is_raw_disk) {

			goto skip_flush;
		}
#endif /* __WIN__ */
retry:
		if (node->n_pending_flushes > 0) {
			/* We want to avoid calling os_file_flush() on
			the file twice at the same time, because we do
			not know what bugs OS's may contain in file
			i/o */

			ib_int64_t sig_count =
				os_event_reset(node->sync_event);

			mutex_exit(&fil_system->mutex);

			os_event_wait_low(node->sync_event, sig_count);

			mutex_enter(&fil_system->mutex);

			if (node->flush_counter >= old_mod_counter) {

				goto skip_flush;
			}

			goto retry;
		}

		ut_a(node->open);
		node->n_pending_flushes++;

		mutex_exit(&fil_system->mutex);

		os_file_flush(node->handle);

		mutex_enter(&fil_system->mutex);

		os_event_set(node->sync_event);

		node->n_pending_flushes--;
skip_flush:
		if (node->flush_counter < old_mod_counter) {
			node->flush_counter = old_mod_counter;

			if (space->is_in_unflushed_spaces
			    && fil_space_is_flushed(space)) {

				space->is_in_unflushed_spaces = false;

				UT_LIST_REMOVE(
					unflushed_spaces,
					fil_system->unflushed_spaces,
					space);
			}
		}

		if (space->purpose == FIL_TABLESPACE) {
			fil_n_pending_tablespace_flushes--;
		} else {
			fil_n_pending_log_flushes--;
		}
	}

	space->n_pending_flushes--;
}

/** Try to extend a tablespace.
@param[in,out]	space	tablespace to be extended
@param[in,out]	node	last file of the tablespace
@param[in]	size	desired size in number of pages
@param[out]	success	whether the operation succeeded
@return	whether the operation should be retried */
static UNIV_COLD __attribute__((warn_unused_result, nonnull))
bool
fil_space_extend_must_retry(
	fil_space_t*	space,
	fil_node_t*	node,
	ulint		size,
	ibool*		success)
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
	other thread can rename, delete or close the file because
	we have set the node->being_extended flag. */
	mutex_exit(&fil_system->mutex);

	ulint		start_page_no		= space->size;
	const ulint	file_start_page_no	= start_page_no - node->size;

	/* Determine correct file block size */
	if (node->file_block_size == 0) {
		node->file_block_size = os_file_get_block_size(
			node->handle, node->name);
		space->file_block_size = node->file_block_size;
	}

	ulint	page_size	= fsp_flags_get_zip_size(space->flags);
	if (!page_size) {
		page_size = UNIV_PAGE_SIZE;
	}

	/* fil_read_first_page() expects UNIV_PAGE_SIZE bytes.
	fil_node_open_file() expects at least 4 * UNIV_PAGE_SIZE bytes.*/
	os_offset_t new_size = std::max(
		os_offset_t(size - file_start_page_no) * page_size,
		os_offset_t(FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE));

	*success = os_file_set_size(node->name, node->handle, new_size,
		FSP_FLAGS_HAS_PAGE_COMPRESSION(space->flags));

	DBUG_EXECUTE_IF("ib_os_aio_func_io_failure_28",
		*success = FALSE;
		os_has_said_disk_full = TRUE;);

	if (*success) {
		os_file_flush(node->handle);
		os_has_said_disk_full = FALSE;
		start_page_no = size;
	}

	mutex_enter(&fil_system->mutex);

	ut_a(node->being_extended);
	ut_a(start_page_no - file_start_page_no >= node->size);

	ulint file_size = start_page_no - file_start_page_no;
	space->size += file_size - node->size;
	node->size = file_size;

	fil_node_complete_io(node, fil_system, OS_FILE_READ);

	node->being_extended = FALSE;

	if (space->id == 0) {
		ulint pages_per_mb = (1024 * 1024) / page_size;

		/* Keep the last data file size info up to date, rounded to
		full megabytes */

		srv_data_file_sizes[srv_n_data_files - 1]
			= (node->size / pages_per_mb) * pages_per_mb;
	}

	fil_flush_low(space, true);
	return(false);
}

/*******************************************************************//**
Reserves the fil_system mutex and tries to make sure we can open at least one
file while holding it. This should be called before calling
fil_node_prepare_for_io(), because that function may need to open a file. */
static
void
fil_mutex_enter_and_prepare_for_io(
/*===============================*/
	ulint	space_id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		count		= 0;

retry:
	mutex_enter(&fil_system->mutex);

	if (space_id >= SRV_LOG_SPACE_FIRST_ID) {
		/* We keep log files always open. */
		return;
	}

	space = fil_space_get_by_id(space_id);

	if (space == NULL) {
		return;
	}

	fil_node_t*	node = UT_LIST_GET_LAST(space->chain);

	ut_ad(space->id == 0 || node == UT_LIST_GET_FIRST(space->chain));

	if (space->id == 0) {
		/* We keep the system tablespace files always open;
		this is important in preventing deadlocks in this module, as
		a page read completion often performs another read from the
		insert buffer. The insert buffer is in tablespace 0, and we
		cannot end up waiting in this function. */
	} else if (!node || node->open) {
		/* If the file is already open, no need to do
		anything; if the space does not exist, we handle the
		situation in the function which called this
		function */
	} else {
		/* Too many files are open, try to close some */
		while (fil_system->n_open >= fil_system->max_n_open) {
			if (fil_try_to_close_file_in_LRU(count > 1)) {
				/* No problem */
			} else if (count >= 2) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"innodb_open_files=%lu is exceeded"
					" (%lu files stay open)",
					fil_system->max_n_open,
					fil_system->n_open);
				break;
			} else {
				mutex_exit(&fil_system->mutex);

				/* Wake the i/o-handler threads to
				make sure pending i/o's are
				performed */
				os_aio_simulated_wake_handler_threads();
				os_thread_sleep(20000);

				/* Flush tablespaces so that we can
				close modified files in the LRU list */
				fil_flush_file_spaces(FIL_TABLESPACE);

				count++;
				goto retry;
			}
		}
	}

	if (ulint size = UNIV_UNLIKELY(space->recv_size)) {
		ut_ad(node);
		ibool	success;
		if (fil_space_extend_must_retry(space, node, size, &success)) {
			goto retry;
		}

		ut_ad(mutex_own(&fil_system->mutex));
		/* Crash recovery requires the file extension to succeed. */
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

		Only if space->recv_size matches what we read originally,
		reset the field. In this way, a subsequent I/O request
		will handle any pending fil_space_set_recv_size(). */

		if (size == space->recv_size) {
			space->recv_size = 0;
		}
	}
}

/** Prepare a data file object for freeing.
@param[in,out]	space	tablespace
@param[in,out]	node	data file */
static
void
fil_node_free_part1(fil_space_t* space, fil_node_t* node)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(node->magic_n == FIL_NODE_MAGIC_N);
	ut_a(node->n_pending == 0);
	ut_a(!node->being_extended);

	if (node->open) {
		/* We fool the assertion in fil_node_close_file() to think
		there are no unflushed modifications in the file */

		node->modification_counter = node->flush_counter;
		os_event_set(node->sync_event);

		if (fil_buffering_disabled(space)) {

			ut_ad(!space->is_in_unflushed_spaces);
			ut_ad(fil_space_is_flushed(space));

		} else if (space->is_in_unflushed_spaces
			   && fil_space_is_flushed(space)) {

			space->is_in_unflushed_spaces = false;

			UT_LIST_REMOVE(unflushed_spaces,
				       fil_system->unflushed_spaces,
				       space);
		}

		fil_node_close_file(node, fil_system);
	}
}

/** Free a data file object.
@param[in,out]	space	tablespace
@param[in]	node	data file */
static
void
fil_node_free_part2(fil_space_t* space, fil_node_t* node)
{
	ut_ad(!node->open);

	space->size -= node->size;

	UT_LIST_REMOVE(chain, space->chain, node);

	os_event_free(node->sync_event);
	mem_free(node->name);
	mem_free(node);
}

#ifdef UNIV_LOG_ARCHIVE
/****************************************************************//**
Drops files from the start of a file space, so that its size is cut by
the amount given. */
UNIV_INTERN
void
fil_space_truncate_start(
/*=====================*/
	ulint	id,		/*!< in: space id */
	ulint	trunc_len)	/*!< in: truncate by this much; it is an error
				if this does not equal to the combined size of
				some initial files in the space */
{
	fil_node_t*	node;
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	while (trunc_len > 0) {
		node = UT_LIST_GET_FIRST(space->chain);

		ut_a(node->size * UNIV_PAGE_SIZE <= trunc_len);

		trunc_len -= node->size * UNIV_PAGE_SIZE;

		fil_node_free_part1(space, node);
		fil_node_free_part2(space, node);
	}

	mutex_exit(&fil_system->mutex);
}

/****************************************************************//**
Check is there node in file space with given name. */
UNIV_INTERN
ibool
fil_space_contains_node(
/*====================*/
	ulint	id,		/*!< in: space id */
	char*	node_name)	/*!< in: node name */
{
	fil_node_t*	node;
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	for (node = UT_LIST_GET_FIRST(space->chain); node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		if (ut_strcmp(node->name, node_name) == 0) {
			mutex_exit(&fil_system->mutex);
			return(TRUE);
		}

	}

	mutex_exit(&fil_system->mutex);
	return(FALSE);
}

#endif /* UNIV_LOG_ARCHIVE */

/*******************************************************************//**
Creates a space memory object and puts it to the 'fil system' hash table.
If there is an error, prints an error message to the .err log.
@param[in]	name		Space name
@param[in]	id		Space id
@param[in]	flags		Tablespace flags
@param[in]	purpose		FIL_TABLESPACE or FIL_LOG if log
@param[in]	crypt_data	Encryption information
@param[in]	create_table	True if this is create table
@param[in]	mode		Encryption mode
@return	TRUE if success */
UNIV_INTERN
bool
fil_space_create(
	const char*		name,
	ulint			id,
	ulint			flags,
	ulint			purpose,
	fil_space_crypt_t*	crypt_data,
	bool			create_table,
	fil_encryption_t	mode)
{
	fil_space_t*	space;

	DBUG_EXECUTE_IF("fil_space_create_failure", return(false););

	ut_a(fil_system);

	/* Look for a matching tablespace and if found free it. */
	do {
		mutex_enter(&fil_system->mutex);

		space = fil_space_get_by_name(name);

		if (space != 0) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Tablespace '%s' exists in the cache "
				"with id %lu != %lu",
				name, (ulong) space->id, (ulong) id);

			if (id == 0 || purpose != FIL_TABLESPACE) {

				mutex_exit(&fil_system->mutex);

				return(false);
			}

			ib_logf(IB_LOG_LEVEL_WARN,
				"Freeing existing tablespace '%s' entry "
				"from the cache with id %lu",
				name, (ulong) id);

			bool	success = fil_space_free_and_mutex_exit(
				space->id, false);
			ut_a(success);
		}

	} while (space != 0);

	space = fil_space_get_by_id(id);

	if (space != 0) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to add tablespace '%s' with id %lu "
			"to the tablespace memory cache, but tablespace '%s' "
			"with id %lu already exists in the cache!",
			name, (ulong) id, space->name, (ulong) space->id);

		mutex_exit(&fil_system->mutex);

		return(false);
	}

	space = static_cast<fil_space_t*>(mem_zalloc(sizeof(*space)));

	space->name = mem_strdup(name);
	space->id = id;

	fil_system->tablespace_version++;
	space->tablespace_version = fil_system->tablespace_version;

	if (purpose == FIL_TABLESPACE && !recv_recovery_on
	    && id > fil_system->max_assigned_id) {

		if (!fil_system->space_id_reuse_warned) {
			fil_system->space_id_reuse_warned = TRUE;
			if (!IS_XTRABACKUP()) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Allocated tablespace %lu, old maximum "
					"was %lu",
					(ulong)id,
					(ulong)fil_system->max_assigned_id);
			}
		}

		fil_system->max_assigned_id = id;
	}

	space->purpose = purpose;
	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;
	space->crypt_data = crypt_data;

	rw_lock_create(fil_space_latch_key, &space->latch, SYNC_FSP);

	HASH_INSERT(fil_space_t, hash, fil_system->spaces, id, space);

	HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(name), space);

	UT_LIST_ADD_LAST(space_list, fil_system->space_list, space);

	/* Inform key rotation that there could be something
	to do */
	if (purpose == FIL_TABLESPACE && !srv_fil_crypt_rotate_key_age && fil_crypt_threads_event &&
	    (mode == FIL_ENCRYPTION_ON || mode == FIL_ENCRYPTION_OFF ||
		    srv_encrypt_tables)) {
		/* Key rotation is not enabled, need to inform background
		encryption threads. */
		UT_LIST_ADD_LAST(rotation_list, fil_system->rotation_list, space);
		space->is_in_rotation_list = true;
		mutex_exit(&fil_system->mutex);
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_set(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
	} else {
		mutex_exit(&fil_system->mutex);
	}

	return(true);
}

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return	TRUE if assigned, FALSE if not */
UNIV_INTERN
ibool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id)	/*!< in/out: space id */
{
	ulint	id;
	ibool	success;

	mutex_enter(&fil_system->mutex);

	id = *space_id;

	if (id < fil_system->max_assigned_id) {
		id = fil_system->max_assigned_id;
	}

	id++;

	if (id > (SRV_LOG_SPACE_FIRST_ID / 2) && (id % 1000000UL == 0)) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"InnoDB: Warning: you are running out of new"
			" single-table tablespace id's.\n"
			"InnoDB: Current counter is %lu and it"
			" must not exceed %lu!\n"
			"InnoDB: To reset the counter to zero"
			" you have to dump all your tables and\n"
			"InnoDB: recreate the whole InnoDB installation.\n",
			(ulong) id,
			(ulong) SRV_LOG_SPACE_FIRST_ID);
	}

	success = (id < SRV_LOG_SPACE_FIRST_ID);

	if (success) {
		*space_id = fil_system->max_assigned_id = id;
	} else {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"InnoDB: You have run out of single-table"
			" tablespace id's!\n"
			"InnoDB: Current counter is %lu.\n"
			"InnoDB: To reset the counter to zero you"
			" have to dump all your tables and\n"
			"InnoDB: recreate the whole InnoDB installation.\n",
			(ulong) id);
		*space_id = ULINT_UNDEFINED;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/** Free a space object from the tablespace memory cache. Close the files in
the chain but do not delete them. There must not be any pending i/o's or
flushes on the files.
The fil_system->mutex will be released.
@param[in]	id		tablespace ID
@param[in]	x_latched	whether the caller holds exclusive space->latch
@return whether the tablespace existed */
static
bool
fil_space_free_and_mutex_exit(ulint id, bool x_latched)
{
	fil_space_t*	space;
	fil_space_t*	fnamespace;

	ut_ad(mutex_own(&fil_system->mutex));

	space = fil_space_get_by_id(id);

	if (!space) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"trying to remove non-existing tablespace " ULINTPF,
			id);
		mutex_exit(&fil_system->mutex);
		return(false);
	}

	HASH_DELETE(fil_space_t, hash, fil_system->spaces, id, space);

	fnamespace = fil_space_get_by_name(space->name);
	ut_a(fnamespace);
	ut_a(space == fnamespace);

	HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(space->name), space);

	if (space->is_in_unflushed_spaces) {

		ut_ad(!fil_buffering_disabled(space));
		space->is_in_unflushed_spaces = false;

		UT_LIST_REMOVE(unflushed_spaces, fil_system->unflushed_spaces,
			       space);
	}

	if (space->is_in_rotation_list) {
		space->is_in_rotation_list = false;
		ut_a(UT_LIST_GET_LEN(fil_system->rotation_list) > 0);
		UT_LIST_REMOVE(rotation_list, fil_system->rotation_list, space);
	}

	UT_LIST_REMOVE(space_list, fil_system->space_list, space);

	ut_a(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_a(0 == space->n_pending_flushes);

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {
		fil_node_free_part1(space, node);
	}

	mutex_exit(&fil_system->mutex);

	/* Wait for fil_space_release_for_io(); after
	fil_space_detach(), the tablespace cannot be found, so
	fil_space_acquire_for_io() would return NULL */
	while (space->n_pending_ios) {
		os_thread_sleep(100);
	}

	for (fil_node_t* fil_node = UT_LIST_GET_FIRST(space->chain);
	     fil_node != NULL;
	     fil_node = UT_LIST_GET_FIRST(space->chain)) {
		fil_node_free_part2(space, fil_node);
	}

	ut_a(0 == UT_LIST_GET_LEN(space->chain));

	if (x_latched) {
		rw_lock_x_unlock(&space->latch);
	}

	rw_lock_free(&(space->latch));

	fil_space_destroy_crypt_data(&(space->crypt_data));

	mem_free(space->name);
	mem_free(space);

	return(TRUE);
}

/*******************************************************************//**
Returns a pointer to the file_space_t that is in the memory cache
associated with a space id.
@return	file_space_t pointer, NULL if space not found */
fil_space_t*
fil_space_get(
/*==========*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	mutex_exit(&fil_system->mutex);

	return (space);
}

/*******************************************************************//**
Returns a pointer to the file_space_t that is in the memory cache
associated with a space id. The caller must lock fil_system->mutex.
@return	file_space_t pointer, NULL if space not found */
UNIV_INLINE
fil_space_t*
fil_space_get_space(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	fil_node_t*	node;

	ut_ad(fil_system);

	space = fil_space_get_by_id(id);
	if (space == NULL) {
		return(NULL);
	}

	if (space->size == 0 && space->purpose == FIL_TABLESPACE) {
		ut_a(id != 0);

		mutex_exit(&fil_system->mutex);

		/* It is possible that the space gets evicted at this point
		before the fil_mutex_enter_and_prepare_for_io() acquires
		the fil_system->mutex. Check for this after completing the
		call to fil_mutex_enter_and_prepare_for_io(). */
		fil_mutex_enter_and_prepare_for_io(id);

		/* We are still holding the fil_system->mutex. Check if
		the space is still in memory cache. */
		space = fil_space_get_by_id(id);
		if (space == NULL) {
			return(NULL);
		}

		/* The following code must change when InnoDB supports
		multiple datafiles per tablespace. Note that there is small
		change that space is found from tablespace list but
		we have not yet created node for it and as we hold
		fil_system mutex here fil_node_create can't continue. */
		ut_a(UT_LIST_GET_LEN(space->chain) == 1 || UT_LIST_GET_LEN(space->chain) == 0);

		node = UT_LIST_GET_FIRST(space->chain);

		if (node) {
			/* It must be a single-table tablespace and we have not opened
			the file yet; the following calls will open it and update the
			size fields */

			if (!fil_node_prepare_for_io(node, fil_system, space)) {
				/* The single-table tablespace can't be opened,
				because the ibd file is missing. */
				return(NULL);
			}
			fil_node_complete_io(node, fil_system, OS_FILE_READ);
		}
	}

	return(space);
}

/*******************************************************************//**
Returns the path from the first fil_node_t found for the space ID sent.
The caller is responsible for freeing the memory allocated here for the
value returned.
@return	own: A copy of fil_node_t::path, NULL if space ID is zero
or not found. */
UNIV_INTERN
char*
fil_space_get_first_path(
/*=====================*/
	ulint		id)	/*!< in: space id */
{
	fil_space_t*	space;
	fil_node_t*	node;
	char*		path;

	ut_ad(fil_system);
	ut_a(id);

	fil_mutex_enter_and_prepare_for_io(id);

	space = fil_space_get_space(id);

	if (space == NULL) {
		mutex_exit(&fil_system->mutex);

		return(NULL);
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
@return	space size, 0 if space not found */
UNIV_INTERN
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
@return	flags, ULINT_UNDEFINED if space not found */
UNIV_INTERN
ulint
fil_space_get_flags(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		flags;

	ut_ad(fil_system);

	if (!id) {
		return(0);
	}

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

/*******************************************************************//**
Returns the compressed page size of the space, or 0 if the space
is not compressed. The tablespace must be cached in the memory cache.
@return	compressed page size, ULINT_UNDEFINED if space not found */
UNIV_INTERN
ulint
fil_space_get_zip_size(
/*===================*/
	ulint	id)	/*!< in: space id */
{
	ulint	flags;

	flags = fil_space_get_flags(id);

	if (flags && flags != ULINT_UNDEFINED) {

		return(fsp_flags_get_zip_size(flags));
	}

	return(flags);
}

/****************************************************************//**
Initializes the tablespace memory cache. */
UNIV_INTERN
void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open)	/*!< in: max number of open files */
{
	ut_a(fil_system == NULL);

	ut_a(hash_size > 0);
	ut_a(max_n_open > 0);

	fil_system = static_cast<fil_system_t*>(
		mem_zalloc(sizeof(fil_system_t)));

	mutex_create(fil_system_mutex_key,
		     &fil_system->mutex, SYNC_ANY_LATCH);

	fil_system->spaces = hash_create(hash_size);
	fil_system->name_hash = hash_create(hash_size);

	fil_system->max_n_open = max_n_open;

	fil_space_crypt_init();
}

/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */
UNIV_INTERN
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

			if (!node->open) {
				if (!fil_node_open_file(node, fil_system,
							space)) {
					/* This func is called during server's
					startup. If some file of log or system
					tablespace is missing, the server
					can't start successfully. So we should
					assert for it. */
					ut_a(0);
				}
			}

			if (fil_system->max_n_open < 10 + fil_system->n_open) {

				fprintf(stderr,
					"InnoDB: Warning: you must"
					" raise the value of"
					" innodb_open_files in\n"
					"InnoDB: my.cnf! Remember that"
					" InnoDB keeps all log files"
					" and all system\n"
					"InnoDB: tablespace files open"
					" for the whole time mysqld is"
					" running, and\n"
					"InnoDB: needs to open also"
					" some .ibd files if the"
					" file-per-table storage\n"
					"InnoDB: model is used."
					" Current open files %lu,"
					" max allowed"
					" open files %lu.\n",
					(ulong) fil_system->n_open,
					(ulong) fil_system->max_n_open);
			}
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
UNIV_INTERN
void
fil_close_all_files(void)
/*=====================*/
{
	fil_space_t*	space;

	// Must check both flags as it's possible for this to be called during
	// server startup with srv_track_changed_pages == true but
	// srv_redo_log_thread_started == false
	if (srv_track_changed_pages && srv_redo_log_thread_started)
		os_event_wait(srv_redo_log_tracked_event);

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space != NULL) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->open) {
				fil_node_close_file(node, fil_system);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);

		/* This is executed during shutdown. No other thread
		can create or remove tablespaces while we are not
		holding fil_system->mutex. */
		fil_space_free_and_mutex_exit(prev_space->id, false);
		mutex_enter(&fil_system->mutex);
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */
UNIV_INTERN
void
fil_close_log_files(
/*================*/
	bool	free)	/*!< in: whether to free the memory object */
{
	fil_space_t*	space;

	// Must check both flags as it's possible for this to be called during
	// server startup with srv_track_changed_pages == true but
	// srv_redo_log_thread_started == false
	if (srv_track_changed_pages && srv_redo_log_thread_started)
		os_event_wait(srv_redo_log_tracked_event);

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space != NULL) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		if (space->purpose != FIL_LOG) {
			space = UT_LIST_GET_NEXT(space_list, space);
			continue;
		}

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->open) {
				fil_node_close_file(node, fil_system);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);

		if (free) {
			/* This is executed during startup. No other thread
			can create or remove tablespaces while we are not
			holding fil_system->mutex. */
			fil_space_free_and_mutex_exit(prev_space->id, false);
			mutex_enter(&fil_system->mutex);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
UNIV_INTERN
void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id)	/*!< in: maximum known id */
{
	if (max_id >= SRV_LOG_SPACE_FIRST_ID) {
		fprintf(stderr,
			"InnoDB: Fatal error: max tablespace id"
			" is too high, %lu\n", (ulong) max_id);
		ut_error;
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

	buf1 = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));
	buf = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));

	/* Acquire system tablespace */
	fil_space_t* space = fil_space_acquire(0);

	/* If tablespace is not encrypted, stamp flush_lsn to
	first page of all system tablespace datafiles to avoid
	unnecessary error messages on possible downgrade. */
	if (!space->crypt_data
	    || !space->crypt_data->should_encrypt()) {
		fil_node_t*     node;
		ulint   sum_of_sizes = 0;

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			err = fil_read(TRUE, 0, 0, sum_of_sizes, 0,
				       UNIV_PAGE_SIZE, buf, NULL, 0);

			if (err == DB_SUCCESS) {
				mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
					lsn);

				err = fil_write(TRUE, 0, 0, sum_of_sizes, 0,
						UNIV_PAGE_SIZE, buf, NULL, 0);

				sum_of_sizes += node->size;
			}
		}
	} else {
		/* When system tablespace is encrypted stamp flush_lsn to
		only the first page of the first datafile (rest of pages
		are encrypted). */
		err = fil_read(TRUE, 0, 0, 0, 0,
			       UNIV_PAGE_SIZE, buf, NULL, 0);

		if (err == DB_SUCCESS) {
			mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
					lsn);

			err = fil_write(TRUE, 0, 0, 0, 0,
					UNIV_PAGE_SIZE, buf, NULL, 0);
		}
	}

	fil_flush_file_spaces(FIL_TABLESPACE);
	fil_space_release(space);

	ut_free(buf1);

	return(err);
}

/** Check the consistency of the first data page of a tablespace
at database startup.
@param[in]	page		page frame
@param[in]	space_id	tablespace identifier
@param[in]	flags		tablespace flags
@retval NULL on success, or if innodb_force_recovery is set
@return pointer to an error message string */
static MY_ATTRIBUTE((warn_unused_result))
const char*
fil_check_first_page(const page_t* page, ulint space_id, ulint flags)
{
	if (srv_force_recovery >= SRV_FORCE_IGNORE_CORRUPT) {
		return(NULL);
	}

	if (UNIV_PAGE_SIZE != fsp_flags_get_page_size(flags)) {
		fprintf(stderr,
			"InnoDB: Error: Current page size %lu != "
			" page size on page %lu\n",
			UNIV_PAGE_SIZE, fsp_flags_get_page_size(flags));

		return("innodb-page-size mismatch");
	}

	if (!space_id && !flags) {
		ulint		nonzero_bytes	= UNIV_PAGE_SIZE;
		const byte*	b		= page;

		while (!*b && --nonzero_bytes) {
			b++;
		}

		if (!nonzero_bytes) {
			return("space header page consists of zero bytes");
		}
	}

	if (buf_page_is_corrupted(
			false, page, fsp_flags_get_zip_size(flags), NULL)) {
		return("checksum mismatch");
	}

	if (page_get_space_id(page) == space_id
	    && page_get_page_no(page) == 0) {
		return(NULL);
	}

	return("inconsistent data in space header");
}

/** Reads the flushed lsn, arch no, space_id and tablespace flag fields from
the first page of a first data file at database startup.
@param[in]	data_file		open data file
@param[in]	one_read_only		true if first datafile is already
					read
@param[out]	flags			FSP_SPACE_FLAGS
@param[out]	space_id		tablepspace ID
@param[out]	flushed_lsn		flushed lsn value
@param[out]	crypt_data		encryption crypt data
@param[in]	check_first_page	true if first page contents
					should be checked
@return NULL on success, or if innodb_force_recovery is set
@retval pointer to an error message string */
UNIV_INTERN
const char*
fil_read_first_page(
	pfs_os_file_t	data_file,
	ibool		one_read_already,
	ulint*		flags,
	ulint*		space_id,
	lsn_t*		flushed_lsn,
	fil_space_crypt_t**   crypt_data,
	bool		check_first_page)
{
	byte*		buf;
	byte*		page;
	const char*	check_msg = NULL;
	fil_space_crypt_t* cdata;

	if (IS_XTRABACKUP() && srv_backup_mode) {
		/* Files smaller than page size may occur
		in xtrabackup, when server creates new file
		but has not yet written into it, or wrote only
		partially. Checks size here, to avoid exit in os_file_read.
		This file will be skipped by xtrabackup if it is too small.
		*/
		os_offset_t	file_size;
		file_size = os_file_get_size(data_file);
		if (file_size < FIL_IBD_FILE_INITIAL_SIZE*UNIV_PAGE_SIZE) {
			return "File size is less than minimum";
		}
	}

	buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));

	/* Align the memory for a possible read from a raw device */

	page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

	os_file_read(data_file, page, 0, UNIV_PAGE_SIZE);

	srv_stats.page0_read.inc();

	/* The FSP_HEADER on page 0 is only valid for the first file
	in a tablespace.  So if this is not the first datafile, leave
	*flags and *space_id as they were read from the first file and
	do not validate the first page. */
	if (!one_read_already) {
		/* Undo tablespace does not contain correct FSP_HEADER,
		and actually we really need to read only crypt_data. */
		if (check_first_page) {
			*space_id = fsp_header_get_space_id(page);
			*flags = fsp_header_get_flags(page);

			if (flushed_lsn) {
				*flushed_lsn = mach_read_from_8(page +
				       FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
			}

			if (!fsp_flags_is_valid(*flags)) {
				ulint cflags = fsp_flags_convert_from_101(*flags);
				if (cflags == ULINT_UNDEFINED) {
					ib_logf(IB_LOG_LEVEL_ERROR,
						"Invalid flags 0x%x in tablespace %u",
						unsigned(*flags), unsigned(*space_id));
					return "invalid tablespace flags";
				} else {
					*flags = cflags;
				}
			}

			if (!(IS_XTRABACKUP() && srv_backup_mode)) {
				check_msg = fil_check_first_page(page, *space_id, *flags);
			}
		}

		/* Possible encryption crypt data is also stored only to first page
		of the first datafile. */

		const ulint offset = fsp_header_get_crypt_offset(
					fsp_flags_get_zip_size(*flags));

		cdata = fil_space_read_crypt_data(*space_id, page, offset);

		if (crypt_data) {
			*crypt_data = cdata;
		}

		/* If file space is encrypted we need to have at least some
		encryption service available where to get keys */
		if (cdata && cdata->should_encrypt()) {

			if (!encryption_key_id_exists(cdata->key_id)) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Tablespace id " ULINTPF
					" is encrypted but encryption service"
					" or used key_id %u is not available. "
					"Can't continue opening tablespace.",
					*space_id, cdata->key_id);

				return ("table encrypted but encryption service not available.");
			}
		}
	}

	ut_free(buf);

	if (check_msg) {
		return(check_msg);
	}

	return(NULL);
}

/*================ SINGLE-TABLE TABLESPACES ==========================*/

/********************************************************//**
Creates the database directory for a table if it does not exist yet. */
static
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
	path = static_cast<char*>(mem_alloc(len + (namend - name) + 2));

	memcpy(path, fil_path_to_mysql_datadir, len);
	path[len] = '/';
	memcpy(path + len + 1, name, namend - name);
	path[len + (namend - name) + 1] = 0;

	srv_normalize_path_for_win(path);

	ut_a(os_file_create_directory(path, FALSE));
	mem_free(path);
}

#ifndef UNIV_HOTBACKUP
/********************************************************//**
Writes a log record about an .ibd file create/rename/delete. */
static
void
fil_op_write_log(
/*=============*/
	ulint		type,		/*!< in: MLOG_FILE_CREATE,
					MLOG_FILE_CREATE2,
					MLOG_FILE_DELETE, or
					MLOG_FILE_RENAME */
	ulint		space_id,	/*!< in: space id */
	ulint		log_flags,	/*!< in: redo log flags (stored
					in the page number field) */
	ulint		flags,		/*!< in: compressed page size
					and file format
					if type==MLOG_FILE_CREATE2, or 0 */
	const char*	name,		/*!< in: table name in the familiar
					'databasename/tablename' format, or
					the file path in the case of
					MLOG_FILE_DELETE */
	const char*	new_name,	/*!< in: if type is MLOG_FILE_RENAME,
					the new table name in the
					'databasename/tablename' format */
	mtr_t*		mtr)		/*!< in: mini-transaction handle */
{
	byte*	log_ptr;
	ulint	len;

	log_ptr = mlog_open(mtr, 11 + 2 + 1);
	ut_ad(fsp_flags_is_valid(flags));

	if (!log_ptr) {
		/* Logging in mtr is switched off during crash recovery:
		in that case mlog_open returns NULL */
		return;
	}

	log_ptr = mlog_write_initial_log_record_for_file_op(
		type, space_id, log_flags, log_ptr, mtr);
	if (type == MLOG_FILE_CREATE2) {
		mach_write_to_4(log_ptr, flags);
		log_ptr += 4;
	}
	/* Let us store the strings as null-terminated for easier readability
	and handling */

	len = strlen(name) + 1;

	mach_write_to_2(log_ptr, len);
	log_ptr += 2;
	mlog_close(mtr, log_ptr);

	mlog_catenate_string(mtr, (byte*) name, len);

	if (type == MLOG_FILE_RENAME) {
		len = strlen(new_name) + 1;
		log_ptr = mlog_open(mtr, 2 + len);
		ut_a(log_ptr);
		mach_write_to_2(log_ptr, len);
		log_ptr += 2;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(mtr, (byte*) new_name, len);
	}
}
#endif

/*******************************************************************//**
Parses the body of a log record written about an .ibd file operation. That is,
the log record part after the standard (type, space id, page no) header of the
log record.

If desired, also replays the delete or rename operation if the .ibd file
exists and the space id in it matches. Replays the create operation if a file
at that path does not exist yet. If the database directory for the file to be
created does not exist, then we create the directory, too.

Note that mysqlbackup --apply-log sets fil_path_to_mysql_datadir to point to
the datadir that we should use in replaying the file operations.

InnoDB recovery does not replay these fully since it always sets the space id
to zero. But mysqlbackup does replay them.  TODO: If remote tablespaces are
used, mysqlbackup will only create tables in the default directory since
MLOG_FILE_CREATE and MLOG_FILE_CREATE2 only know the tablename, not the path.

@return end of log record, or NULL if the record was not completely
contained between ptr and end_ptr */
UNIV_INTERN
byte*
fil_op_log_parse_or_replay(
/*=======================*/
	byte*	ptr,		/*!< in: buffer containing the log record body,
				or an initial segment of it, if the record does
				not fir completely between ptr and end_ptr */
	byte*	end_ptr,	/*!< in: buffer end */
	ulint	type,		/*!< in: the type of this log record */
	ulint	space_id,	/*!< in: the space id of the tablespace in
				question, or 0 if the log record should
				only be parsed but not replayed */
	ulint	log_flags)	/*!< in: redo log flags
				(stored in the page number parameter) */
{
	ulint		name_len;
	ulint		new_name_len;
	const char*	name;
	const char*	new_name	= NULL;
	ulint		flags		= 0;

	if (type == MLOG_FILE_CREATE2) {
		if (end_ptr < ptr + 4) {

			return(NULL);
		}

		flags = mach_read_from_4(ptr);
		ptr += 4;
	}

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	name_len = mach_read_from_2(ptr);

	ptr += 2;

	if (end_ptr < ptr + name_len) {

		return(NULL);
	}

	name = (const char*) ptr;

	ptr += name_len;

	if (type == MLOG_FILE_RENAME) {
		if (end_ptr < ptr + 2) {

			return(NULL);
		}

		new_name_len = mach_read_from_2(ptr);

		ptr += 2;

		if (end_ptr < ptr + new_name_len) {

			return(NULL);
		}

		new_name = (const char*) ptr;

		ptr += new_name_len;
	}

	/* We managed to parse a full log record body */
	/*
	printf("Parsed log rec of type %lu space %lu\n"
	"name %s\n", type, space_id, name);

	if (type == MLOG_FILE_RENAME) {
	printf("new name %s\n", new_name);
	}
	*/
	if (!space_id) {
		return(ptr);
	} else {
		/* Only replay file ops during recovery.  This is a
		release-build assert to minimize any data loss risk by a
		misapplied file operation.  */
		ut_a(recv_recovery_is_on());
	}

	/* Let us try to perform the file operation, if sensible. Note that
	mysqlbackup has at this stage already read in all space id info to the
	fil0fil.cc data structures.

	NOTE that our algorithm is not guaranteed to work correctly if there
	were renames of tables during the backup. See mysqlbackup code for more
	on the problem. */

	switch (type) {
	case MLOG_FILE_DELETE:
		if (fil_tablespace_exists_in_mem(space_id)) {
			dberr_t	err = fil_delete_tablespace(space_id);
			ut_a(err == DB_SUCCESS);
		}

		break;

	case MLOG_FILE_RENAME:
		/* In order to replay the rename, the following must hold:
		* The new name is not already used.
		* A tablespace is open in memory with the old name.
		* The space ID for that tablepace matches this log entry.
		This will prevent unintended renames during recovery. */

		if (fil_get_space_id_for_table(new_name) == ULINT_UNDEFINED
		    && space_id == fil_get_space_id_for_table(name)) {
			/* Create the database directory for the new name, if
			it does not exist yet */
			fil_create_directory_for_tablename(new_name);

			if (!fil_rename_tablespace(name, space_id,
						   new_name, NULL)) {
				ut_error;
			}
		}

		break;

	case MLOG_FILE_CREATE:
	case MLOG_FILE_CREATE2:
		if (fil_tablespace_exists_in_mem(space_id)) {
			/* Do nothing */
		} else if (fil_get_space_id_for_table(name)
			   != ULINT_UNDEFINED) {
			/* Do nothing */
		} else if (log_flags & MLOG_FILE_FLAG_TEMP) {
			/* Temporary table, do nothing */
		} else {
			/* Create the database directory for name, if it does
			not exist yet */
			fil_create_directory_for_tablename(name);

			if (fil_create_new_single_table_tablespace(
				    space_id, name, NULL, flags,
				    DICT_TF2_USE_TABLESPACE,
				    FIL_IBD_FILE_INITIAL_SIZE,
				    FIL_ENCRYPTION_DEFAULT,
				    FIL_DEFAULT_ENCRYPTION_KEY) != DB_SUCCESS) {
				ut_error;
			}
		}

		break;

	default:
		ut_error;
	}

	return(ptr);
}

/*******************************************************************//**
Allocates a file name for the EXPORT/IMPORT config file name.  The
string must be freed by caller with mem_free().
@return own: file name */
static
char*
fil_make_cfg_name(
/*==============*/
	const char*	filepath)	/*!< in: .ibd file name */
{
	char*	cfg_name;

	/* Create a temporary file path by replacing the .ibd suffix
	with .cfg. */

	ut_ad(strlen(filepath) > 4);

	cfg_name = mem_strdup(filepath);
	ut_snprintf(cfg_name + strlen(cfg_name) - 3, 4, "cfg");
	return(cfg_name);
}

/*******************************************************************//**
Check for change buffer merges.
@return 0 if no merges else count + 1. */
static
ulint
fil_ibuf_check_pending_ops(
/*=======================*/
	fil_space_t*	space,	/*!< in/out: Tablespace to check */
	ulint		count)	/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system->mutex));

	if (space != 0 && space->n_pending_ops != 0) {

		if (count > 5000) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Trying to close/delete tablespace "
				"'%s' but there are %lu pending change "
				"buffer merges on it.",
				space->name,
				(ulong) space->n_pending_ops);
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
	fil_space_t*	space,	/*!< in/out: Tablespace to check */
	fil_node_t**	node,	/*!< out: Node in space list */
	ulint		count)	/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(space->n_pending_ops == 0);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	*node = UT_LIST_GET_FIRST(space->chain);

	if (space->n_pending_flushes > 0 || (*node)->n_pending > 0) {

		ut_a(!(*node)->being_extended);

		if (count > 1000) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Trying to close/delete tablespace '%s' "
				"but there are %lu flushes "
				" and %lu pending i/o's on it.",
				space->name,
				(ulong) space->n_pending_flushes,
				(ulong) (*node)->n_pending);
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
	ulint		id,	/*!< in: space id */
	fil_space_t**	space,	/*!< out: tablespace instance in memory */
	char**		path)	/*!< out/own: tablespace path */
{
	ulint		count = 0;

	ut_a(id != TRX_SYS_SPACE);
	ut_ad(space);

	*space = 0;

	mutex_enter(&fil_system->mutex);
	fil_space_t* sp = fil_space_get_by_id(id);

	if (sp) {
		sp->stop_new_ops = true;
		/* space could be freed by other threads as soon
		as n_pending_ops reaches 0, thus increment pending
		ops here. */
		sp->n_pending_ops++;
	}

	mutex_exit(&fil_system->mutex);

	/* Wait for crypt threads to stop accessing space */
	if (sp) {
		fil_space_crypt_close_tablespace(sp);
		/* We have "acquired" this space and must
		free it now as below we compare n_pending_ops. */
		fil_space_release(sp);
	}

	/* Check for pending change buffer merges. */

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		count = fil_ibuf_check_pending_ops(sp, count);

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	/* Check for pending IO. */

	*path = 0;

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		if (sp == NULL) {
			mutex_exit(&fil_system->mutex);
			return(DB_TABLESPACE_NOT_FOUND);
		}

		fil_node_t*	node;

		count = fil_check_pending_io(sp, &node, count);

		if (count == 0) {
			*path = mem_strdup(node->name);
		}

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	ut_ad(sp);

	*space = sp;
	return(DB_SUCCESS);
}

/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return	DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*		trx,	/*!< in/out: Transaction covering the close */
	ulint		id)	/*!< in: space id */
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(id != TRX_SYS_SPACE);

	dberr_t		err = fil_check_pending_operations(id, &space, &path);

	if (err != DB_SUCCESS) {
		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	rw_lock_x_lock(&space->latch);

#ifndef UNIV_HOTBACKUP
	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have set space->stop_new_ops = TRUE, readahead
	or ibuf merge can no longer read more pages of this tablespace to the
	buffer pool. Thus we can clean the tablespace out of the buffer pool
	completely and permanently. The flag stop_new_ops also prevents
	fil_flush() from being applied to this tablespace. */

	buf_LRU_flush_or_remove_pages(id, trx);
#endif
	mutex_enter(&fil_system->mutex);

	/* If the free is successful, the X lock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free_and_mutex_exit(id, TRUE)) {
		rw_lock_x_unlock(&space->latch);
		err = DB_TABLESPACE_NOT_FOUND;
	} else {
		err = DB_SUCCESS;
	}

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	char*	cfg_name = fil_make_cfg_name(path);

	os_file_delete_if_exists(innodb_file_data_key, cfg_name);

	mem_free(path);
	mem_free(cfg_name);

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
UNIV_INTERN bool fil_table_accessible(const dict_table_t* table)
{
	if (UNIV_UNLIKELY(!table->is_readable() || table->corrupted)) {
		return(false);
	}

	if (fil_space_t* space = fil_space_acquire(table->space)) {
		bool accessible = !space->is_stopping();
		fil_space_release(space);
		return(accessible);
	} else {
		return(false);
	}
}

/** Delete a tablespace and associated .ibd file.
@param[in]	id		tablespace identifier
@param[in]	drop_ahi	whether to drop the adaptive hash index
@return	DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_delete_tablespace(ulint id, bool drop_ahi)
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(id != TRX_SYS_SPACE);

	dberr_t		err = fil_check_pending_operations(id, &space, &path);

	if (err != DB_SUCCESS) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot delete tablespace %lu because it is not "
			"found in the tablespace memory cache.",
			(ulong) id);

		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	/* Important: We rely on the data dictionary mutex to ensure
	that a race is not possible here. It should serialize the tablespace
	drop/free. We acquire an X latch only to avoid a race condition
	when accessing the tablespace instance via:

	  fsp_get_available_space_in_free_extents().

	There our main motivation is to reduce the contention on the
	dictionary mutex. */

	rw_lock_x_lock(&space->latch);

#ifndef UNIV_HOTBACKUP
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
	space from the flush_list. Not that if a block is IO-fixed
	we'll wait for IO to complete.

	To deal with potential read requests by checking the
	::stop_new_ops flag in fil_io() */

	buf_LRU_flush_or_remove_pages(id, NULL);

#endif /* !UNIV_HOTBACKUP */

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */
	{
		char*	cfg_name = fil_make_cfg_name(path);
		os_file_delete_if_exists(innodb_file_data_key, cfg_name);
		mem_free(cfg_name);
	}

	/* Delete the link file pointing to the ibd file we are deleting. */
	if (FSP_FLAGS_HAS_DATA_DIR(space->flags)) {
		fil_delete_link_file(space->name);
	}

	mutex_enter(&fil_system->mutex);

	/* Double check the sanity of pending ops after reacquiring
	the fil_system::mutex. */
	if (fil_space_get_by_id(id)) {
		ut_a(space->n_pending_ops == 0);
		ut_a(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		ut_a(node->n_pending == 0);
	}

	if (!fil_space_free_and_mutex_exit(id, true)) {
		err = DB_TABLESPACE_NOT_FOUND;
	}

	if (err != DB_SUCCESS) {
		rw_lock_x_unlock(&space->latch);
	} else if (!os_file_delete(innodb_file_data_key, path)
		   && !os_file_delete_if_exists(innodb_file_data_key, path)) {

		/* Note: This is because we have removed the
		tablespace instance from the cache. */

		err = DB_IO_ERROR;
	}

	if (err == DB_SUCCESS && !IS_XTRABACKUP()) {
#ifndef UNIV_HOTBACKUP
		/* Write a log record about the deletion of the .ibd
		file, so that mysqlbackup can replay it in the
		--apply-log phase. We use a dummy mtr and the familiar
		log write mechanism. */
		mtr_t		mtr;

		/* When replaying the operation in mysqlbackup, do not try
		to write any log record */
		mtr_start(&mtr);

		fil_op_write_log(MLOG_FILE_DELETE, id, 0, 0, path, NULL, &mtr);
		mtr_commit(&mtr);
#endif
		err = DB_SUCCESS;
	}

	mem_free(path);

	return(err);
}

/*******************************************************************//**
Returns TRUE if a single-table tablespace is being deleted.
@return TRUE if being deleted */
UNIV_INTERN
ibool
fil_tablespace_is_being_deleted(
/*============================*/
	ulint		id)	/*!< in: space id */
{
	fil_space_t*	space;
	ibool		is_being_deleted;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != NULL);

	is_being_deleted = space->stop_new_ops;

	mutex_exit(&fil_system->mutex);

	return(is_being_deleted);
}

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. Free all the pages in use by the tablespace.
@return	DB_SUCCESS or error */
UNIV_INTERN
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
		ib_logf(IB_LOG_LEVEL_WARN,
			"While deleting tablespace %lu in DISCARD TABLESPACE."
			" File rename/delete failed: %s",
			(ulong) id, ut_strerr(err));
		break;

	case DB_TABLESPACE_NOT_FOUND:
		ib_logf(IB_LOG_LEVEL_WARN,
			"Cannot delete tablespace %lu in DISCARD "
			"TABLESPACE. %s",
			(ulong) id, ut_strerr(err));
		break;

	default:
		ut_error;
	}

	/* Remove all insert buffer entries for the tablespace */

	ibuf_delete_for_discarded_space(id);

	return(err);
}
#endif /* !UNIV_HOTBACKUP */

/*******************************************************************//**
Renames the memory cache structures of a single-table tablespace.
@return	TRUE if success */
static
ibool
fil_rename_tablespace_in_mem(
/*=========================*/
	fil_space_t*	space,	/*!< in: tablespace memory object */
	fil_node_t*	node,	/*!< in: file node of that tablespace */
	const char*	new_name,	/*!< in: new name */
	const char*	new_path)	/*!< in: new file path */
{
	fil_space_t*	space2;
	const char*	old_name	= space->name;

	ut_ad(mutex_own(&fil_system->mutex));

	space2 = fil_space_get_by_name(old_name);
	if (space != space2) {
		fputs("InnoDB: Error: cannot find ", stderr);
		ut_print_filename(stderr, old_name);
		fputs(" in tablespace memory cache\n", stderr);

		return(FALSE);
	}

	space2 = fil_space_get_by_name(new_name);
	if (space2 != NULL) {
		fputs("InnoDB: Error: ", stderr);
		ut_print_filename(stderr, new_name);
		fputs(" is already in tablespace memory cache\n", stderr);

		return(FALSE);
	}

	HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(space->name), space);
	mem_free(space->name);
	mem_free(node->name);

	space->name = mem_strdup(new_name);
	node->name = mem_strdup(new_path);

	HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(new_name), space);
	return(TRUE);
}

/*******************************************************************//**
Allocates a file name for a single-table tablespace. The string must be freed
by caller with mem_free().
@return	own: file name */
UNIV_INTERN
char*
fil_make_ibd_name(
/*==============*/
	const char*	name,		/*!< in: table name or a dir path */
	bool		is_full_path)	/*!< in: TRUE if it is a dir path */
{
	char*	filename;
	ulint	namelen		= strlen(name);
	ulint	dirlen		= strlen(fil_path_to_mysql_datadir);
	ulint	pathlen		= dirlen + namelen + sizeof "/.ibd";

	filename = static_cast<char*>(mem_alloc(pathlen));

	if (is_full_path) {
		memcpy(filename, name, namelen);
		memcpy(filename + namelen, ".ibd", sizeof ".ibd");
	} else {
		ut_snprintf(filename, pathlen, "%s/%s.ibd",
			fil_path_to_mysql_datadir, name);

	}

	srv_normalize_path_for_win(filename);

	return(filename);
}

/*******************************************************************//**
Allocates a file name for a tablespace ISL file (InnoDB Symbolic Link).
The string must be freed by caller with mem_free().
@return	own: file name */
UNIV_INTERN
char*
fil_make_isl_name(
/*==============*/
	const char*	name)	/*!< in: table name */
{
	char*	filename;
	ulint	namelen		= strlen(name);
	ulint	dirlen		= strlen(fil_path_to_mysql_datadir);
	ulint	pathlen		= dirlen + namelen + sizeof "/.isl";

	filename = static_cast<char*>(mem_alloc(pathlen));

	ut_snprintf(filename, pathlen, "%s/%s.isl",
		fil_path_to_mysql_datadir, name);

	srv_normalize_path_for_win(filename);

	return(filename);
}

/** Test if a tablespace file can be renamed to a new filepath by checking
if that the old filepath exists and the new filepath does not exist.
@param[in]	space_id	tablespace id
@param[in]	old_path	old filepath
@param[in]	new_path	new filepath
@param[in]	is_discarded	whether the tablespace is discarded
@return innodb error code */
dberr_t
fil_rename_tablespace_check(
	ulint		space_id,
	const char*	old_path,
	const char*	new_path,
	bool		is_discarded)
{
	ulint	exists = false;
	os_file_type_t	ftype;

	if (!is_discarded
	    && os_file_status(old_path, &exists, &ftype)
	    && !exists) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot rename '%s' to '%s' for space ID %lu"
			" because the source file does not exist.",
			old_path, new_path, space_id);

		return(DB_TABLESPACE_NOT_FOUND);
	}

	exists = false;
	if (!os_file_status(new_path, &exists, &ftype) || exists) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot rename '%s' to '%s' for space ID %lu"
			" because the target file exists."
			" Remove the target file and try again.",
			old_path, new_path, space_id);

		return(DB_TABLESPACE_EXISTS);
	}

	return(DB_SUCCESS);
}

/*******************************************************************//**
Renames a single-table tablespace. The tablespace must be cached in the
tablespace memory cache.
@return	TRUE if success */
UNIV_INTERN
ibool
fil_rename_tablespace(
/*==================*/
	const char*	old_name_in,	/*!< in: old table name in the
					standard databasename/tablename
					format of InnoDB, or NULL if we
					do the rename based on the space
					id only */
	ulint		id,		/*!< in: space id */
	const char*	new_name,	/*!< in: new table name in the
					standard databasename/tablename
					format of InnoDB */
	const char*	new_path_in)	/*!< in: new full datafile path
					if the tablespace is remotely
					located, or NULL if it is located
					in the normal data directory. */
{
	ibool		success;
	fil_space_t*	space;
	fil_node_t*	node;
	char*		new_path;
	char*		old_name;
	char*		old_path;
	const char*	not_given	= "(name not specified)";

	ut_a(id != 0);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot find space id %lu in the tablespace "
			"memory cache, though the table '%s' in a "
			"rename operation should have that id.",
			(ulong) id, old_name_in ? old_name_in : not_given);
		mutex_exit(&fil_system->mutex);

		return(FALSE);
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	node = UT_LIST_GET_FIRST(space->chain);

	/* Check that the old name in the space is right */

	if (old_name_in) {
		old_name = mem_strdup(old_name_in);
		ut_a(strcmp(space->name, old_name) == 0);
	} else {
		old_name = mem_strdup(space->name);
	}
	old_path = mem_strdup(node->name);

	/* Rename the tablespace and the node in the memory cache */
	new_path = new_path_in ? mem_strdup(new_path_in)
		: fil_make_ibd_name(new_name, false);

	success = fil_rename_tablespace_in_mem(
		space, node, new_name, new_path);

	if (success) {
		DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
				goto skip_second_rename; );
		success = os_file_rename(
			innodb_file_data_key, old_path, new_path);
		DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
skip_second_rename:
				success = FALSE; );

		if (!success) {
			/* We have to revert the changes we made
			to the tablespace memory cache */

			ut_a(fil_rename_tablespace_in_mem(
					space, node, old_name, old_path));
		}
	}

	mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP
	if (success && !recv_recovery_on && !IS_XTRABACKUP()) {
		mtr_t		mtr;

		mtr_start(&mtr);

		fil_op_write_log(MLOG_FILE_RENAME, id, 0, 0, old_name, new_name,
				 &mtr);
		mtr_commit(&mtr);
	}
#endif /* !UNIV_HOTBACKUP */

	mem_free(new_path);
	mem_free(old_path);
	mem_free(old_name);

	return(success);
}

/*******************************************************************//**
Creates a new InnoDB Symbolic Link (ISL) file.  It is always created
under the 'datadir' of MySQL. The datadir is the directory of a
running mysqld program. We can refer to it by simply using the path '.'.
@return	DB_SUCCESS or error code */
UNIV_INTERN
dberr_t
fil_create_link_file(
/*=================*/
	const char*	tablename,	/*!< in: tablename */
	const char*	filepath)	/*!< in: pathname of tablespace */
{
	dberr_t		err = DB_SUCCESS;
	char*		link_filepath;
	char*		prev_filepath = fil_read_link_file(tablename);

	ut_ad(!srv_read_only_mode);

	if (prev_filepath) {
		/* Truncate will call this with an existing
		link file which contains the same filepath. */
		if (0 == strcmp(prev_filepath, filepath)) {
			mem_free(prev_filepath);
			return(DB_SUCCESS);
		}
		mem_free(prev_filepath);
	}

	link_filepath = fil_make_isl_name(tablename);

	/** Check if the file already exists. */
	FILE*                   file = NULL;
	ibool                   exists;
	os_file_type_t          ftype;

	bool success = os_file_status(link_filepath, &exists, &ftype);

	ulint error = 0;
	if (success && !exists) {
		file = fopen(link_filepath, "w");
		if (file == NULL) {
			/* This call will print its own error message */
			error = os_file_get_last_error(true);
		}
	} else {
		error = OS_FILE_ALREADY_EXISTS;
	}
	if (error != 0) {

		ut_print_timestamp(stderr);
		fputs("  InnoDB: Cannot create file ", stderr);
		ut_print_filename(stderr, link_filepath);
		fputs(".\n", stderr);

		if (error == OS_FILE_ALREADY_EXISTS) {
			fputs("InnoDB: The link file: ", stderr);
			ut_print_filename(stderr, filepath);
			fputs(" already exists.\n", stderr);
			err = DB_TABLESPACE_EXISTS;
		} else if (error == OS_FILE_DISK_FULL) {
			err = DB_OUT_OF_FILE_SPACE;
		} else if (error == OS_FILE_OPERATION_NOT_SUPPORTED) {
			err = DB_UNSUPPORTED;
		} else {
			err = DB_ERROR;
		}

		/* file is not open, no need to close it. */
		mem_free(link_filepath);
		return(err);
	}

	ulint rbytes = fwrite(filepath, 1, strlen(filepath), file);
	if (rbytes != strlen(filepath)) {
		os_file_get_last_error(true);
		ib_logf(IB_LOG_LEVEL_ERROR,
			"cannot write link file "
			 "%s",filepath);
		err = DB_ERROR;
	}

	/* Close the file, we only need it at startup */
	fclose(file);

	mem_free(link_filepath);

	return(err);
}

/*******************************************************************//**
Deletes an InnoDB Symbolic Link (ISL) file. */
UNIV_INTERN
void
fil_delete_link_file(
/*=================*/
	const char*	tablename)	/*!< in: name of table */
{
	char* link_filepath = fil_make_isl_name(tablename);

	os_file_delete_if_exists(innodb_file_data_key, link_filepath);

	mem_free(link_filepath);
}

/*******************************************************************//**
Reads an InnoDB Symbolic Link (ISL) file.
It is always created under the 'datadir' of MySQL.  The name is of the
form {databasename}/{tablename}. and the isl file is expected to be in a
'{databasename}' directory called '{tablename}.isl'. The caller must free
the memory of the null-terminated path returned if it is not null.
@return	own: filepath found in link file, NULL if not found. */
UNIV_INTERN
char*
fil_read_link_file(
/*===============*/
	const char*	name)		/*!< in: tablespace name */
{
	char*		filepath = NULL;
	char*		link_filepath;
	FILE*		file = NULL;

	/* The .isl file is in the 'normal' tablespace location. */
	link_filepath = fil_make_isl_name(name);

	file = fopen(link_filepath, "r+b");

	mem_free(link_filepath);

	if (file) {
		filepath = static_cast<char*>(mem_alloc(OS_FILE_MAX_PATH));

		os_file_read_string(file, filepath, OS_FILE_MAX_PATH);
		fclose(file);

		if (strlen(filepath)) {
			/* Trim whitespace from end of filepath */
			ulint lastch = strlen(filepath) - 1;
			while (lastch > 4 && filepath[lastch] <= 0x20) {
				filepath[lastch--] = 0x00;
			}
			srv_normalize_path_for_win(filepath);
		}
	}

	return(filepath);
}

/*******************************************************************//**
Opens a handle to the file linked to in an InnoDB Symbolic Link file.
@return	TRUE if remote linked tablespace file is found and opened. */
UNIV_INTERN
ibool
fil_open_linked_file(
/*===============*/
	const char*	tablename,	/*!< in: database/tablename */
	char**		remote_filepath,/*!< out: remote filepath */
	pfs_os_file_t*	remote_file,	/*!< out: remote file handle */
	ulint           atomic_writes)  /*!< in: atomic writes table option
					value */
{
	ibool		success;

	*remote_filepath = fil_read_link_file(tablename);
	if (*remote_filepath == NULL) {
		return(FALSE);
	}

	/* The filepath provided is different from what was
	found in the link file. */
	*remote_file = os_file_create_simple_no_error_handling(
		innodb_file_data_key, *remote_filepath,
		OS_FILE_OPEN, OS_FILE_READ_ONLY,
		&success, atomic_writes);

	if (!success) {
		char*	link_filepath = fil_make_isl_name(tablename);

		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"A link file was found named '%s' "
			"but the linked tablespace '%s' "
			"could not be opened.",
			link_filepath, *remote_filepath);

		mem_free(link_filepath);
		mem_free(*remote_filepath);
		*remote_filepath = NULL;
	}

	return(success);
}

/*******************************************************************//**
Creates a new single-table tablespace to a database directory of MySQL.
Database directories are under the 'datadir' of MySQL. The datadir is the
directory of a running mysqld program. We can refer to it by simply the
path '.'. Tables created with CREATE TEMPORARY TABLE we place in the temp
dir of the mysqld server.

@return	DB_SUCCESS or error code */
UNIV_INTERN
dberr_t
fil_create_new_single_table_tablespace(
/*===================================*/
	ulint		space_id,	/*!< in: space id */
	const char*	tablename,	/*!< in: the table name in the usual
					databasename/tablename format
					of InnoDB */
	const char*	dir_path,	/*!< in: NULL or a dir path */
	ulint		flags,		/*!< in: tablespace flags */
	ulint		flags2,		/*!< in: table flags2 */
	ulint		size,		/*!< in: the initial size of the
					tablespace file in pages,
					must be >= FIL_IBD_FILE_INITIAL_SIZE */
	fil_encryption_t mode,	/*!< in: encryption mode */
	ulint		key_id)	/*!< in: encryption key_id */
{
	pfs_os_file_t	file;

	ibool		ret;
	dberr_t		err;
	byte*		buf2;
	byte*		page;
	char*		path;
	ibool		success;
	/* TRUE if a table is created with CREATE TEMPORARY TABLE */
	bool		is_temp = !!(flags2 & DICT_TF2_TEMPORARY);


	/* For XtraBackup recovery we force remote tablespaces to be local,
	i.e. never execute the code path corresponding to has_data_dir == true.
	We don't create .isl files either, because we rely on innobackupex to
	copy them under a global lock, and use them to copy remote tablespaces
	to their proper locations on --copy-back.

	See also MySQL bug #72022: dir_path is always NULL for remote
	tablespaces when a MLOG_FILE_CREATE* log record is replayed (the remote
	directory is not available from MLOG_FILE_CREATE*). */
	bool		has_data_dir = FSP_FLAGS_HAS_DATA_DIR(flags) != 0 && !IS_XTRABACKUP();
	ulint		atomic_writes = FSP_FLAGS_GET_ATOMIC_WRITES(flags);
	fil_space_crypt_t *crypt_data = NULL;

	ut_a(space_id > 0);
	ut_ad(!srv_read_only_mode);
	ut_a(space_id < SRV_LOG_SPACE_FIRST_ID);
	ut_a(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_a(fsp_flags_is_valid(flags & ~FSP_FLAGS_MEM_MASK));

	if (is_temp) {
		/* Temporary table filepath */
		ut_ad(dir_path);
		path = fil_make_ibd_name(dir_path, true);
	} else if (has_data_dir) {
		ut_ad(dir_path);
		path = os_file_make_remote_pathname(dir_path, tablename, "ibd");

		/* Since this tablespace file will be created in a
		remote directory, let's create the subdirectories
		in the path, if they are not there already. */
		success = os_file_create_subdirs_if_needed(path);
		if (!success) {
			err = DB_ERROR;
			goto error_exit_3;
		}
	} else {
		path = fil_make_ibd_name(tablename, false);
	}

	file = os_file_create(
		innodb_file_data_key, path,
		OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT,
		OS_FILE_NORMAL,
		OS_DATA_FILE,
		&ret,
		atomic_writes);

	if (ret == FALSE) {
		/* The following call will print an error message */
		ulint	error = os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create file '%s'\n", path);

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The file '%s' already exists though the "
				"corresponding table did not exist "
				"in the InnoDB data dictionary. "
				"Have you moved InnoDB .ibd files "
				"around without using the SQL commands "
				"DISCARD TABLESPACE and IMPORT TABLESPACE, "
				"or did mysqld crash in the middle of "
				"CREATE TABLE? "
				"You can resolve the problem by removing "
				"the file '%s' under the 'datadir' of MySQL.",
				path, path);

			err = DB_TABLESPACE_EXISTS;
			goto error_exit_3;
		}

		if (error == OS_FILE_OPERATION_NOT_SUPPORTED) {
			err = DB_UNSUPPORTED;
			goto error_exit_3;
		}

		if (error == OS_FILE_DISK_FULL) {
			err = DB_OUT_OF_FILE_SPACE;
			goto error_exit_3;
		}

		err = DB_ERROR;
		goto error_exit_3;
	}

	{
		/* fil_read_first_page() expects UNIV_PAGE_SIZE bytes.
		fil_node_open_file() expects at least 4 * UNIV_PAGE_SIZE bytes.
		Do not create too short ROW_FORMAT=COMPRESSED files. */
		const ulint zip_size = fsp_flags_get_zip_size(flags);
		const ulint page_size = zip_size ? zip_size : UNIV_PAGE_SIZE;
		const os_offset_t fsize = std::max(
			os_offset_t(size) * page_size,
			os_offset_t(FIL_IBD_FILE_INITIAL_SIZE
				    * UNIV_PAGE_SIZE));
		/* ROW_FORMAT=COMPRESSED files never use page_compression
		(are never sparse). */
		ut_ad(!zip_size || !FSP_FLAGS_HAS_PAGE_COMPRESSION(flags));

		ret = os_file_set_size(path, file, fsize,
				       FSP_FLAGS_HAS_PAGE_COMPRESSION(flags));
	}

	if (!ret) {
		err = DB_OUT_OF_FILE_SPACE;
		goto error_exit_2;
	}

	/* printf("Creating tablespace %s id %lu\n", path, space_id); */

	/* We have to write the space id to the file immediately and flush the
	file to disk. This is because in crash recovery we must be aware what
	tablespaces exist and what are their space id's, so that we can apply
	the log records to the right file. It may take quite a while until
	buffer pool flush algorithms write anything to the file and flush it to
	disk. If we would not write here anything, the file would be filled
	with zeros from the call of os_file_set_size(), until a buffer pool
	flush would write to it. */

	buf2 = static_cast<byte*>(ut_malloc(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	flags |= FSP_FLAGS_PAGE_SSIZE();
	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	if (const ulint zip_size = fsp_flags_get_zip_size(flags)) {
		page_zip_des_t	page_zip;

		page_zip_set_size(&page_zip, zip_size);
		page_zip.data = page + UNIV_PAGE_SIZE;
#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;
		buf_flush_init_for_writing(page, &page_zip, 0);
		ret = os_file_write(path, file, page_zip.data, 0, zip_size);
	} else {
		buf_flush_init_for_writing(page, NULL, 0);
		ret = os_file_write(path, file, page, 0, UNIV_PAGE_SIZE);
	}

	ut_free(buf2);

	if (!ret) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Could not write the first page to tablespace "
			"'%s'", path);

		err = DB_ERROR;
		goto error_exit_2;
	}

	ret = os_file_flush(file);

	if (!ret) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"File flush of tablespace '%s' failed", path);
		err = DB_ERROR;
		goto error_exit_2;
	}

	if (has_data_dir) {
		/* Now that the IBD file is created, make the ISL file. */
		err = fil_create_link_file(tablename, path);
		if (err != DB_SUCCESS) {
			goto error_exit_2;
		}
	}

	/* Create crypt data if the tablespace is either encrypted or user has
	requested it to remain unencrypted. */
	if (mode == FIL_ENCRYPTION_ON || mode == FIL_ENCRYPTION_OFF ||
		srv_encrypt_tables) {
		crypt_data = fil_space_create_crypt_data(mode, key_id);
	}

	success = fil_space_create(tablename, space_id, flags, FIL_TABLESPACE,
				   crypt_data, true, mode);

	if (!success || !fil_node_create(path, size, space_id, FALSE)) {
		err = DB_ERROR;
		goto error_exit_1;
	}

#ifndef UNIV_HOTBACKUP
	if (!IS_XTRABACKUP())
	{
		mtr_t		mtr;
		ulint		mlog_file_flag = 0;

		if (is_temp) {
			mlog_file_flag |= MLOG_FILE_FLAG_TEMP;
		}

		mtr_start(&mtr);

		fil_op_write_log(flags
				 ? MLOG_FILE_CREATE2
				 : MLOG_FILE_CREATE,
				 space_id, mlog_file_flag,
				 flags & ~FSP_FLAGS_MEM_MASK,
				 tablename, NULL, &mtr);

		mtr_commit(&mtr);
	}
#endif
	err = DB_SUCCESS;

	/* Error code is set.  Cleanup the various variables used.
	These labels reflect the order in which variables are assigned or
	actions are done. */
error_exit_1:
	if (has_data_dir && err != DB_SUCCESS) {
		fil_delete_link_file(tablename);
	}
error_exit_2:
	os_file_close(file);
	if (err != DB_SUCCESS) {
		os_file_delete(innodb_file_data_key, path);
	}
error_exit_3:
	mem_free(path);

	return(err);
}

#include "pars0pars.h"
#include "que0que.h"
#include "dict0priv.h"
static
void
fil_remove_invalid_table_from_data_dict(const char *name)
{
	trx_t*		trx;
	pars_info_t*	info = NULL;

	trx = trx_allocate_for_mysql();
	trx_start_for_ddl(trx, TRX_DICT_OP_TABLE);

	ut_ad(mutex_own(&dict_sys->mutex));

	trx->op_info = "removing invalid table from data dictionary";

	info = pars_info_create();

	pars_info_add_str_literal(info, "table_name", name);

	que_eval_sql(info,
		"PROCEDURE DROP_TABLE_PROC () IS\n"
		"sys_foreign_id CHAR;\n"
		"table_id CHAR;\n"
		"index_id CHAR;\n"
		"foreign_id CHAR;\n"
		"found INT;\n"

		"DECLARE CURSOR cur_fk IS\n"
		"SELECT ID FROM SYS_FOREIGN\n"
		"WHERE FOR_NAME = :table_name\n"
		"AND TO_BINARY(FOR_NAME)\n"
		"  = TO_BINARY(:table_name)\n"
		"LOCK IN SHARE MODE;\n"

		"DECLARE CURSOR cur_idx IS\n"
		"SELECT ID FROM SYS_INDEXES\n"
		"WHERE TABLE_ID = table_id\n"
		"LOCK IN SHARE MODE;\n"

		"BEGIN\n"
		"SELECT ID INTO table_id\n"
		"FROM SYS_TABLES\n"
		"WHERE NAME = :table_name\n"
		"LOCK IN SHARE MODE;\n"
		"IF (SQL % NOTFOUND) THEN\n"
		"       RETURN;\n"
		"END IF;\n"
		"found := 1;\n"
		"SELECT ID INTO sys_foreign_id\n"
		"FROM SYS_TABLES\n"
		"WHERE NAME = 'SYS_FOREIGN'\n"
		"LOCK IN SHARE MODE;\n"
		"IF (SQL % NOTFOUND) THEN\n"
		"       found := 0;\n"
		"END IF;\n"
		"IF (:table_name = 'SYS_FOREIGN') THEN\n"
		"       found := 0;\n"
		"END IF;\n"
		"IF (:table_name = 'SYS_FOREIGN_COLS') THEN\n"
		"       found := 0;\n"
		"END IF;\n"
		"OPEN cur_fk;\n"
		"WHILE found = 1 LOOP\n"
		"       FETCH cur_fk INTO foreign_id;\n"
		"       IF (SQL % NOTFOUND) THEN\n"
		"               found := 0;\n"
		"       ELSE\n"
		"               DELETE FROM SYS_FOREIGN_COLS\n"
		"               WHERE ID = foreign_id;\n"
		"               DELETE FROM SYS_FOREIGN\n"
		"               WHERE ID = foreign_id;\n"
		"       END IF;\n"
		"END LOOP;\n"
		"CLOSE cur_fk;\n"
		"found := 1;\n"
		"OPEN cur_idx;\n"
		"WHILE found = 1 LOOP\n"
		"       FETCH cur_idx INTO index_id;\n"
		"       IF (SQL % NOTFOUND) THEN\n"
		"               found := 0;\n"
		"       ELSE\n"
		"               DELETE FROM SYS_FIELDS\n"
		"               WHERE INDEX_ID = index_id;\n"
		"               DELETE FROM SYS_INDEXES\n"
		"               WHERE ID = index_id\n"
		"               AND TABLE_ID = table_id;\n"
		"       END IF;\n"
		"END LOOP;\n"
		"CLOSE cur_idx;\n"
		"DELETE FROM SYS_COLUMNS\n"
		"WHERE TABLE_ID = table_id;\n"
		"DELETE FROM SYS_TABLES\n"
		"WHERE NAME = :table_name;\n"
		"END;\n"
		, FALSE, trx);

	/* SYS_DATAFILES and SYS_TABLESPACES do not necessarily exist
	on XtraBackup recovery. See comments around
	dict_create_or_check_foreign_constraint_tables() in
	innobase_start_or_create_for_mysql(). */
	if (dict_table_get_low("SYS_DATAFILES") != NULL) {
		info = pars_info_create();

		pars_info_add_str_literal(info, "table_name", name);

		que_eval_sql(info,
			"PROCEDURE DROP_TABLE_PROC () IS\n"
			"space_id INT;\n"

			"BEGIN\n"
			"SELECT SPACE INTO space_id\n"
			"FROM SYS_TABLES\n"
			"WHERE NAME = :table_name;\n"
			"IF (SQL % NOTFOUND) THEN\n"
			"       RETURN;\n"
			"END IF;\n"
			"DELETE FROM SYS_TABLESPACES\n"
			"WHERE SPACE = space_id;\n"
			"DELETE FROM SYS_DATAFILES\n"
			"WHERE SPACE = space_id;\n"
			"END;\n"
			, FALSE, trx);
	}

	trx_commit_for_mysql(trx);

	trx_free_for_mysql(trx);
}


#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Report information about a bad tablespace. */
static
void
fil_report_bad_tablespace(
/*======================*/
	const char*	filepath,	/*!< in: filepath */
	const char*	check_msg,	/*!< in: fil_check_first_page() */
	ulint		found_id,	/*!< in: found space ID */
	ulint		found_flags,	/*!< in: found flags */
	ulint		expected_id,	/*!< in: expected space id */
	ulint		expected_flags)	/*!< in: expected flags */
{
	if (check_msg) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Error %s in file '%s',"
			"tablespace id=%lu, flags=%lu. "
			"Please refer to "
			REFMAN "innodb-troubleshooting-datadict.html "
			"for how to resolve the issue.",
			check_msg, filepath,
			(ulong) expected_id, (ulong) expected_flags);
		return;
	}

	ib_logf(IB_LOG_LEVEL_ERROR,
		"In file '%s', tablespace id and flags are %lu and %lu, "
		"but in the InnoDB data dictionary they are %lu and %lu. "
		"Have you moved InnoDB .ibd files around without using the "
		"commands DISCARD TABLESPACE and IMPORT TABLESPACE? "
		"Please refer to "
		REFMAN "innodb-troubleshooting-datadict.html "
		"for how to resolve the issue.",
		filepath, (ulong) found_id, (ulong) found_flags,
		(ulong) expected_id, (ulong) expected_flags);
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
	ut_ad(fsp_flags_is_valid(flags));

	mtr_t	mtr;
	mtr_start(&mtr);
	if (buf_block_t* b = buf_page_get(
		    space_id, fsp_flags_get_zip_size(flags), 0, RW_X_LATCH,
		    &mtr)) {
		ulint f = fsp_header_get_flags(b->frame);
		/* Suppress the message if only the DATA_DIR flag to differs. */
		if ((f ^ flags) & ~(1U << FSP_FLAGS_POS_RESERVED)) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"adjusting FSP_SPACE_FLAGS of tablespace "
				ULINTPF " from 0x%x to 0x%x",
				space_id, int(f), int(flags));
		}
		if (f != flags) {
			mlog_write_ulint(FSP_HEADER_OFFSET
					 + FSP_SPACE_FLAGS + b->frame,
					 flags, MLOG_4BYTES, &mtr);
		}
	}

	mtr_commit(&mtr);
}

/********************************************************************//**
Tries to open a single-table tablespace and optionally checks that the
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
the first page of the file.  This boolean may be initially FALSE, but if
a remote tablespace is found it will be changed to true.

If the fix_dict boolean is set, then it is safe to use an internal SQL
statement to update the dictionary tables if they are incorrect.

@return	DB_SUCCESS or error code */
UNIV_INTERN
dberr_t
fil_open_single_table_tablespace(
/*=============================*/
	bool		validate,	/*!< in: Do we validate tablespace? */
	bool		fix_dict,	/*!< in: Can we fix the dictionary? */
	ulint		id,		/*!< in: space id */
	ulint		flags,		/*!< in: expected FSP_SPACE_FLAGS */
	const char*	tablename,	/*!< in: table name in the
					databasename/tablename format */
	const char*	path_in)	/*!< in: table */
{
	dberr_t		err = DB_SUCCESS;
	bool		dict_filepath_same_as_default = false;
	bool		link_file_found = false;
	bool		link_file_is_bad = false;
	fsp_open_info	def;
	fsp_open_info	dict;
	fsp_open_info	remote;
	ulint		tablespaces_found = 0;
	ulint		valid_tablespaces_found = 0;
	fil_space_crypt_t* crypt_data = NULL;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(!fix_dict || rw_lock_own(&dict_operation_lock, RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(!fix_dict || mutex_own(&(dict_sys->mutex)));

	/* Table flags can be ULINT_UNDEFINED if
	dict_tf_to_fsp_flags_failure is set. */
	if (flags == ULINT_UNDEFINED) {
		return(DB_CORRUPTION);
	}

	ut_ad(fsp_flags_is_valid(flags & ~FSP_FLAGS_MEM_MASK, id));
	const ulint atomic_writes = FSP_FLAGS_GET_ATOMIC_WRITES(flags);

	memset(&def, 0, sizeof(def));
	memset(&dict, 0, sizeof(dict));
	memset(&remote, 0, sizeof(remote));

	/* Discover the correct filepath.  We will always look for an ibd
	in the default location. If it is remote, it should not be here. */
	def.filepath = fil_make_ibd_name(tablename, false);

	/* The path_in was read from SYS_DATAFILES.
	We skip SYS_DATAFILES validation and remote tablespaces discovery for
	XtraBackup, as all tablespaces are local for XtraBackup recovery. */
	if (path_in && !IS_XTRABACKUP()) {
		if (strcmp(def.filepath, path_in)) {
			dict.filepath = mem_strdup(path_in);
			/* possibility of multiple files. */
			validate = true;
		} else {
			dict_filepath_same_as_default = true;
		}
	}

	link_file_found = fil_open_linked_file(
		tablename, &remote.filepath, &remote.file, atomic_writes);
	remote.success = link_file_found;
	if (remote.success) {
		/* possibility of multiple files. */
		validate = true;
		tablespaces_found++;

		/* A link file was found. MySQL does not allow a DATA
		DIRECTORY to be be the same as the default filepath. */
		ut_a(strcmp(def.filepath, remote.filepath));

		/* If there was a filepath found in SYS_DATAFILES,
		we hope it was the same as this remote.filepath found
		in the ISL file. */
		if (dict.filepath
		    && (0 == strcmp(dict.filepath, remote.filepath))) {
			remote.success = FALSE;
			os_file_close(remote.file);
			mem_free(remote.filepath);
			remote.filepath = NULL;
			tablespaces_found--;
		}
	}

	/* Attempt to open the tablespace at other possible filepaths. */
	if (dict.filepath) {
		dict.file = os_file_create_simple_no_error_handling(
			innodb_file_data_key, dict.filepath, OS_FILE_OPEN,
			OS_FILE_READ_ONLY, &dict.success, atomic_writes);
		if (dict.success) {
			/* possibility of multiple files. */
			validate = true;
			tablespaces_found++;
		}
	}

	/* Always look for a file at the default location. */
	ut_a(def.filepath);
	def.file = os_file_create_simple_no_error_handling(
		innodb_file_data_key, def.filepath, OS_FILE_OPEN,
		OS_FILE_READ_ONLY, &def.success, atomic_writes);

	if (def.success) {
		tablespaces_found++;
	}

	/*  We have now checked all possible tablespace locations and
	have a count of how many we found.  If things are normal, we
	only found 1. */
	if (!validate && tablespaces_found == 1) {
		goto skip_validate;
	}

	/* Read the first page of the datadir tablespace, if found. */
	if (def.success) {
		def.check_msg = fil_read_first_page(
			def.file, false, &def.flags, &def.id,
			NULL, &def.crypt_data);

		def.valid = !def.check_msg && def.id == id
			&& fsp_flags_match(flags, def.flags);

		if (def.valid) {
			valid_tablespaces_found++;
		} else {
			/* Do not use this tablespace. */
			fil_report_bad_tablespace(
				def.filepath, def.check_msg, def.id,
				def.flags, id, flags);
		}
	}

	/* Read the first page of the remote tablespace */
	if (remote.success) {
		remote.check_msg = fil_read_first_page(
			remote.file, false, &remote.flags, &remote.id,
			NULL, &remote.crypt_data);

		/* Validate this single-table-tablespace with SYS_TABLES. */
		remote.valid = !remote.check_msg && remote.id == id
			&& fsp_flags_match(flags, remote.flags);

		if (remote.valid) {
			valid_tablespaces_found++;
		} else {
			/* Do not use this linked tablespace. */
			fil_report_bad_tablespace(
				remote.filepath, remote.check_msg, remote.id,
				remote.flags, id, flags);
			link_file_is_bad = true;
		}
	}

	/* Read the first page of the datadir tablespace, if found. */
	if (dict.success) {
		dict.check_msg = fil_read_first_page(
			dict.file, false, &dict.flags, &dict.id,
			NULL, &dict.crypt_data);

		/* Validate this single-table-tablespace with SYS_TABLES. */
		dict.valid = !dict.check_msg && dict.id == id
			&& fsp_flags_match(flags, dict.flags);

		if (dict.valid) {
			valid_tablespaces_found++;
		} else {
			/* Do not use this tablespace. */
			fil_report_bad_tablespace(
				dict.filepath, dict.check_msg, dict.id,
				dict.flags, id, flags);
		}
	}

	/* Make sense of these three possible locations.
	First, bail out if no tablespace files were found. */
	if (valid_tablespaces_found == 0) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IS_XTRABACKUP() ? IB_LOG_LEVEL_WARN : IB_LOG_LEVEL_ERROR,
			"Could not find a valid tablespace file for '%s'. "
			"See " REFMAN "innodb-troubleshooting-datadict.html "
			"for how to resolve the issue.",
			tablename);

		if (IS_XTRABACKUP() && fix_dict) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"It will be removed from the data dictionary.");

			if (purge_sys) {
				fil_remove_invalid_table_from_data_dict(tablename);
			}
		}

		err = DB_CORRUPTION;

		goto cleanup_and_exit;
	}

	/* Do not open any tablespaces if more than one tablespace with
	the correct space ID and flags were found. */
	if (tablespaces_found > 1) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"A tablespace for %s has been found in "
			"multiple places;", tablename);

		if (def.success) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Default location; %s"
				", Space ID=" ULINTPF " , Flags=" ULINTPF " .",
				def.filepath,
				def.id,
				def.flags);
		}

		if (remote.success) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Remote location; %s"
				", Space ID=" ULINTPF " , Flags=" ULINTPF " .",
				remote.filepath,
				remote.id,
				remote.flags);
		}

		if (dict.success) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Dictionary location; %s"
				", Space ID=" ULINTPF " , Flags=" ULINTPF " .",
				dict.filepath,
				dict.id,
				dict.flags);
		}

		/* Force-recovery will allow some tablespaces to be
		skipped by REDO if there was more than one file found.
		Unlike during the REDO phase of recovery, we now know
		if the tablespace is valid according to the dictionary,
		which was not available then. So if we did not force
		recovery and there is only one good tablespace, ignore
		any bad tablespaces. */
		if (valid_tablespaces_found > 1 || srv_force_recovery > 0) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Will not open the tablespace for '%s'",
				tablename);

			if (def.success != def.valid
			    || dict.success != dict.valid
			    || remote.success != remote.valid) {
				err = DB_CORRUPTION;
			} else {
				err = DB_ERROR;
			}
			goto cleanup_and_exit;
		}

		/* There is only one valid tablespace found and we did
		not use srv_force_recovery during REDO.  Use this one
		tablespace and clean up invalid tablespace pointers */
		if (def.success && !def.valid) {
			def.success = false;
			os_file_close(def.file);
			tablespaces_found--;
		}

		if (dict.success && !dict.valid) {
			dict.success = false;
			os_file_close(dict.file);
			/* Leave dict.filepath so that SYS_DATAFILES
			can be corrected below. */
			tablespaces_found--;
		}
		if (remote.success && !remote.valid) {
			remote.success = false;
			os_file_close(remote.file);
			mem_free(remote.filepath);
			remote.filepath = NULL;
			tablespaces_found--;
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

	/* We may need to change what is stored in SYS_DATAFILES or
	SYS_TABLESPACES or adjust the link file.
	Since a failure to update SYS_TABLESPACES or SYS_DATAFILES does
	not prevent opening and using the single_table_tablespace either
	this time or the next, we do not check the return code or fail
	to open the tablespace. But dict_update_filepath() will issue a
	warning to the log. */
	if (dict.filepath) {
		if (remote.success) {
			dict_update_filepath(id, remote.filepath);
		} else if (def.success) {
			dict_update_filepath(id, def.filepath);
			if (link_file_is_bad) {
				fil_delete_link_file(tablename);
			}
		} else if (!link_file_found || link_file_is_bad) {
			ut_ad(dict.success);
			/* Fix the link file if we got our filepath
			from the dictionary but a link file did not
			exist or it did not point to a valid file. */
			fil_delete_link_file(tablename);
			fil_create_link_file(tablename, dict.filepath);
		}

	} else if (remote.success && dict_filepath_same_as_default) {
		dict_update_filepath(id, remote.filepath);

	} else if (remote.success && path_in == NULL) {
		/* SYS_DATAFILES record for this space ID was not found. */
		dict_insert_tablespace_and_filepath(
			id, tablename, remote.filepath, flags);
	}

skip_validate:
	if (remote.success)
		crypt_data = remote.crypt_data;
	else if (dict.success)
		crypt_data = dict.crypt_data;
	else if (def.success)
		crypt_data = def.crypt_data;

	if (err != DB_SUCCESS) {
		; // Don't load the tablespace into the cache
	} else if (!fil_space_create(tablename, id, flags, FIL_TABLESPACE,
				     crypt_data, false)) {
		err = DB_ERROR;
	} else {
		/* We do not measure the size of the file, that is why
		we pass the 0 below */

		if (!fil_node_create(remote.success ? remote.filepath :
				     dict.success ? dict.filepath :
				     def.filepath, 0, id, FALSE)) {
			err = DB_ERROR;
		}
	}

cleanup_and_exit:
	if (remote.success) {
		os_file_close(remote.file);
	}
	if (remote.filepath) {
		mem_free(remote.filepath);
	}
	if (remote.crypt_data && remote.crypt_data != crypt_data) {
		if (err == DB_SUCCESS) {
			fil_space_destroy_crypt_data(&remote.crypt_data);
		}
	}
	if (dict.success) {
		os_file_close(dict.file);
	}
	if (dict.filepath) {
		mem_free(dict.filepath);
	}
	if (dict.crypt_data && dict.crypt_data != crypt_data) {
		fil_space_destroy_crypt_data(&dict.crypt_data);
	}
	if (def.success) {
		os_file_close(def.file);
	}
	if (def.crypt_data && def.crypt_data != crypt_data) {
		if (err == DB_SUCCESS) {
			fil_space_destroy_crypt_data(&def.crypt_data);
		}
	}

	mem_free(def.filepath);

	if (err == DB_SUCCESS && validate && !srv_read_only_mode) {
		fsp_flags_try_adjust(id, flags & ~FSP_FLAGS_MEM_MASK);
	}

	return(err);
}
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_HOTBACKUP
/*******************************************************************//**
Allocates a file name for an old version of a single-table tablespace.
The string must be freed by caller with mem_free()!
@return	own: file name */
static
char*
fil_make_ibbackup_old_name(
/*=======================*/
	const char*	name)		/*!< in: original file name */
{
	static const char suffix[] = "_ibbackup_old_vers_";
	char*	path;
	ulint	len	= strlen(name);

	path = static_cast<char*>(mem_alloc(len + (15 + sizeof suffix)));

	memcpy(path, name, len);
	memcpy(path + len, suffix, (sizeof suffix) - 1);
	ut_sprintf_timestamp_without_extra_chars(
		path + len + ((sizeof suffix) - 1));
	return(path);
}
#endif /* UNIV_HOTBACKUP */


/*******************************************************************//**
Determine the space id of the given file descriptor by reading a few
pages from the beginning of the .ibd file.
@return true if space id was successfully identified, or false. */
static
bool
fil_user_tablespace_find_space_id(
/*==============================*/
	fsp_open_info*	fsp)	/* in/out: contains file descriptor, which is
				used as input.  contains space_id, which is
				the output */
{
	bool		st;
	os_offset_t	file_size;

	file_size = os_file_get_size(fsp->file);

	if (file_size == (os_offset_t) -1) {
		ib_logf(IB_LOG_LEVEL_ERROR, "Could not get file size: %s",
			fsp->filepath);
		return(false);
	}

	/* Assuming a page size, read the space_id from each page and store it
	in a map.  Find out which space_id is agreed on by majority of the
	pages.  Choose that space_id. */
	for (ulint page_size = UNIV_ZIP_SIZE_MIN;
	     page_size <= UNIV_PAGE_SIZE_MAX; page_size <<= 1) {

		/* map[space_id] = count of pages */
		std::map<ulint, ulint> verify;

		ulint page_count = 64;
		ulint valid_pages = 0;

		/* Adjust the number of pages to analyze based on file size */
		while ((page_count * page_size) > file_size) {
			--page_count;
		}

		ib_logf(IB_LOG_LEVEL_INFO, "Page size:%lu Pages to analyze:"
			"%lu", page_size, page_count);

		byte* buf = static_cast<byte*>(ut_malloc(2*page_size));
		byte* page = static_cast<byte*>(ut_align(buf, page_size));

		for (ulint j = 0; j < page_count; ++j) {

			st = os_file_read(fsp->file, page, (j* page_size), page_size);

			if (!st) {
				ib_logf(IB_LOG_LEVEL_INFO,
					"READ FAIL: page_no:%lu", j);
				continue;
			}

			bool uncompressed_ok = false;

			/* For uncompressed pages, the page size must be equal
			to UNIV_PAGE_SIZE. */
			if (page_size == UNIV_PAGE_SIZE) {
				uncompressed_ok = !buf_page_is_corrupted(
					false, page, 0, NULL);
			}

			bool compressed_ok = false;
			if (page_size <= UNIV_PAGE_SIZE_DEF) {
				compressed_ok = !buf_page_is_corrupted(
					false, page, page_size, NULL);
			}

			if (uncompressed_ok || compressed_ok) {

				ulint space_id = mach_read_from_4(page
					+ FIL_PAGE_SPACE_ID);

				if (space_id > 0) {
					ib_logf(IB_LOG_LEVEL_INFO,
						"VALID: space:%lu "
						"page_no:%lu page_size:%lu",
						space_id, j, page_size);
					verify[space_id]++;
					++valid_pages;
				}
			}
		}

		ut_free(buf);

		ib_logf(IB_LOG_LEVEL_INFO, "Page size: %lu, Possible space_id "
			"count:%lu", page_size, (ulint) verify.size());

		const ulint pages_corrupted = 3;
		for (ulint missed = 0; missed <= pages_corrupted; ++missed) {

			for (std::map<ulint, ulint>::iterator
			     m = verify.begin(); m != verify.end(); ++m ) {

				ib_logf(IB_LOG_LEVEL_INFO, "space_id:%lu, "
					"Number of pages matched: %lu/%lu "
					"(%lu)", m->first, m->second,
					valid_pages, page_size);

				if (m->second == (valid_pages - missed)) {

					ib_logf(IB_LOG_LEVEL_INFO,
						"Chosen space:%lu\n", m->first);

					fsp->id = m->first;
					return(true);
				}
			}

		}
	}

	return(false);
}

/*******************************************************************//**
Finds the given page_no of the given space id from the double write buffer,
and copies it to the corresponding .ibd file.
@return true if copy was successful, or false. */
bool
fil_user_tablespace_restore_page(
/*==============================*/
	fsp_open_info*	fsp,		/* in: contains space id and .ibd
					file information */
	ulint		page_no)	/* in: page_no to obtain from double
					write buffer */
{
	bool	err;
	ulint	flags;
	ulint	zip_size;
	ulint	page_size;
	ulint	buflen;
	byte*	page;

	ib_logf(IB_LOG_LEVEL_INFO, "Restoring page %lu of tablespace %lu",
		page_no, fsp->id);

	// find if double write buffer has page_no of given space id
	page = recv_sys->dblwr.find_page(fsp->id, page_no);

	if (!page) {
                ib_logf(IB_LOG_LEVEL_WARN, "Doublewrite does not have "
			"page_no=%lu of space: %lu", page_no, fsp->id);
		err = false;
		goto out;
	}

	flags = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);

	if (!fsp_flags_is_valid(flags)) {
		ulint cflags = fsp_flags_convert_from_101(flags);
		if (cflags == ULINT_UNDEFINED) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Ignoring a doublewrite copy of page "
				ULINTPF ":" ULINTPF
				" due to invalid flags 0x%x",
				fsp->id, page_no, int(flags));
			err = false;
			goto out;
		}
		flags = cflags;
		/* The flags on the page should be converted later. */
	}

	zip_size = fsp_flags_get_zip_size(flags);
	page_size = fsp_flags_get_page_size(flags);

	ut_ad(page_no == page_get_page_no(page));

	buflen = zip_size ? zip_size: page_size;

	ib_logf(IB_LOG_LEVEL_INFO, "Writing %lu bytes into file: %s",
		buflen, fsp->filepath);

	err = os_file_write(fsp->filepath, fsp->file, page,
			    (zip_size ? zip_size : page_size) * page_no,
		            buflen);

	os_file_flush(fsp->file);
out:
	return(err);
}

/********************************************************************//**
Opens an .ibd file and adds the associated single-table tablespace to the
InnoDB fil0fil.cc data structures.
Set fsp->success to TRUE if tablespace is valid, FALSE if not. */
static
void
fil_validate_single_table_tablespace(
/*=================================*/
	const char*	tablename,	/*!< in: database/tablename */
	fsp_open_info*	fsp)		/*!< in/out: tablespace info */
{
	bool restore_attempted = false;

check_first_page:
	fsp->success = TRUE;
	if (const char* check_msg = fil_read_first_page(
		    fsp->file, false, &fsp->flags, &fsp->id,
		    NULL, &fsp->crypt_data)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"%s in tablespace %s (table %s)",
			check_msg, fsp->filepath, tablename);
		fsp->success = FALSE;
	}

	if (!fsp->success) {
		if (IS_XTRABACKUP()) {
			/* Do not attempt restore from doublewrite buffer
			  in Xtrabackup, this does not work.*/
			return;
		}

		if (!restore_attempted) {
			if (!fil_user_tablespace_find_space_id(fsp)) {
				return;
			}
			restore_attempted = true;

			if (fsp->id > 0
			    && !fil_user_tablespace_restore_page(fsp, 0)) {
				return;
			}
			goto check_first_page;
		}
		return;
	}

	if (fsp->id == ULINT_UNDEFINED || fsp->id == 0) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Tablespace is not sensible;"
			" Table: %s  Space ID: %lu  Filepath: %s\n",
		tablename, (ulong) fsp->id, fsp->filepath);
		fsp->success = FALSE;
		return;
	}

	mutex_enter(&fil_system->mutex);
	fil_space_t* space = fil_space_get_by_id(fsp->id);
	mutex_exit(&fil_system->mutex);
	if (space != NULL) {
		char* prev_filepath = fil_space_get_first_path(fsp->id);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Attempted to open a previously opened tablespace. "
			"Previous tablespace %s uses space ID: %lu at "
			"filepath: %s. Cannot open tablespace %s which uses "
			"space ID: %lu at filepath: %s",
			space->name, (ulong) space->id, prev_filepath,
			tablename, (ulong) fsp->id, fsp->filepath);

		mem_free(prev_filepath);
		fsp->success = FALSE;
		return;
	}

	fsp->success = TRUE;
}


/********************************************************************//**
Opens an .ibd file and adds the associated single-table tablespace to the
InnoDB fil0fil.cc data structures. */
static
void
fil_load_single_table_tablespace(
/*=============================*/
	const char*	dbname,		/*!< in: database name */
	const char*	filename)	/*!< in: file name (not a path),
					including the .ibd or .isl extension */
{
	char*		tablename;
	ulint		tablename_len;
	ulint		dbname_len = strlen(dbname);
	ulint		filename_len = strlen(filename);
	fsp_open_info	def;
	fsp_open_info	remote;
	os_offset_t	size;
	fil_space_t*	space;

	fsp_open_info*	fsp;
	ulong		minimum_size;
	ibool		file_space_create_success;

	memset(&def, 0, sizeof(def));
	memset(&remote, 0, sizeof(remote));

	/* The caller assured that the extension is ".ibd" or ".isl". */
	ut_ad(0 == memcmp(filename + filename_len - 4, ".ibd", 4)
	      || 0 == memcmp(filename + filename_len - 4, ".isl", 4));

	/* Build up the tablename in the standard form database/table. */
	tablename = static_cast<char*>(
		mem_alloc(dbname_len + filename_len + 2));

	/* When lower_case_table_names = 2 it is possible that the
	dbname is in upper case ,but while storing it in fil_space_t
	we must convert it into lower case */
	sprintf(tablename, "%s" , dbname);
	tablename[dbname_len] = '\0';

        if (lower_case_file_system) {
                dict_casedn_str(tablename);
        }

	sprintf(tablename+dbname_len,"/%s",filename);
	tablename_len = strlen(tablename) - strlen(".ibd");
	tablename[tablename_len] = '\0';

	/* There may be both .ibd and .isl file in the directory.
	And it is possible that the .isl file refers to a different
	.ibd file.  If so, we open and compare them the first time
	one of them is sent to this function.  So if this table has
	already been loaded, there is nothing to do.*/
	mutex_enter(&fil_system->mutex);
	space = fil_space_get_by_name(tablename);
	if (space) {
		mem_free(tablename);
		mutex_exit(&fil_system->mutex);
		return;
	}
	mutex_exit(&fil_system->mutex);

	/* Build up the filepath of the .ibd tablespace in the datadir.
	This must be freed independent of def.success. */
	def.filepath = fil_make_ibd_name(tablename, false);

#ifdef __WIN__
# ifndef UNIV_HOTBACKUP
	/* If lower_case_table_names is 0 or 2, then MySQL allows database
	directory names with upper case letters. On Windows, all table and
	database names in InnoDB are internally always in lower case. Put the
	file path to lower case, so that we are consistent with InnoDB's
	internal data dictionary. */

	dict_casedn_str(def.filepath);
# endif /* !UNIV_HOTBACKUP */
#endif


	/* Check for a link file which locates a remote tablespace. */
	remote.success = (IS_XTRABACKUP() && !srv_backup_mode) ? 0 : fil_open_linked_file(
		tablename, &remote.filepath, &remote.file, FALSE);

	/* Read the first page of the remote tablespace */
	if (remote.success) {
		fil_validate_single_table_tablespace(tablename, &remote);
		if (!remote.success) {
			os_file_close(remote.file);
			mem_free(remote.filepath);

			if (srv_backup_mode && (remote.id == ULINT_UNDEFINED
				|| remote.id == 0)) {

				/* Ignore files that have uninitialized space
				IDs on the backup stage. This means that a
				tablespace has just been created and we will
				replay the corresponding log records on
				prepare. */
				goto func_exit_after_close;
			}
		}
	}


	/* Try to open the tablespace in the datadir. */
	def.file = os_file_create_simple_no_error_handling(
		innodb_file_data_key, def.filepath, OS_FILE_OPEN,
		OS_FILE_READ_WRITE, &def.success, FALSE);

	/* Read the first page of the remote tablespace */
	if (def.success) {
		fil_validate_single_table_tablespace(tablename, &def);
		if (!def.success) {
			os_file_close(def.file);

			if (IS_XTRABACKUP() && srv_backup_mode && (def.id == ULINT_UNDEFINED
				|| def.id == 0)) {

				/* Ignore files that have uninitialized space
				IDs on the backup stage. This means that a
				tablespace has just been created and we will
				replay the corresponding log records on
				prepare. */

				goto func_exit_after_close;
			}
		}
	}

	if (!def.success && !remote.success) {

		/* The following call prints an error message */
		os_file_get_last_error(true);
		fprintf(stderr,
			"InnoDB: Error: could not open single-table"
			" tablespace file %s\n", def.filepath);

		if (!strncmp(filename,
			     tmp_file_prefix, tmp_file_prefix_length)) {
			/* Ignore errors for #sql tablespaces. */
			mem_free(tablename);
			if (remote.filepath) {
				mem_free(remote.filepath);
			}
			if (def.filepath) {
				mem_free(def.filepath);
			}
			return;
		}
no_good_file:
		fprintf(stderr,
			"InnoDB: We do not continue the crash recovery,"
			" because the table may become\n"
			"InnoDB: corrupt if we cannot apply the log"
			" records in the InnoDB log to it.\n"
			"InnoDB: To fix the problem and start mysqld:\n"
			"InnoDB: 1) If there is a permission problem"
			" in the file and mysqld cannot\n"
			"InnoDB: open the file, you should"
			" modify the permissions.\n"
			"InnoDB: 2) If the table is not needed, or you"
			" can restore it from a backup,\n"
			"InnoDB: then you can remove the .ibd file,"
			" and InnoDB will do a normal\n"
			"InnoDB: crash recovery and ignore that table.\n"
			"InnoDB: 3) If the file system or the"
			" disk is broken, and you cannot remove\n"
			"InnoDB: the .ibd file, you can set"
			" innodb_force_recovery > 0 in my.cnf\n"
			"InnoDB: and force InnoDB to continue crash"
			" recovery here.\n");
will_not_choose:
		mem_free(tablename);
		if (remote.filepath) {
			mem_free(remote.filepath);
		}
		if (def.filepath) {
			mem_free(def.filepath);
		}

		if (srv_force_recovery > 0) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"innodb_force_recovery was set to %lu. "
				"Continuing crash recovery even though we "
				"cannot access the .ibd file of this table.",
				srv_force_recovery);
			return;
		}
		abort();
	}

	if (def.success && remote.success) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Tablespaces for %s have been found in two places;\n"
			"Location 1: SpaceID: " ULINTPF " File: %s\n"
			"Location 2: SpaceID: " ULINTPF " File: %s\n"
			"You must delete one of them.",
			tablename, def.id,
			def.filepath, remote.id,
			remote.filepath);

		def.success = FALSE;
		os_file_close(def.file);
		os_file_close(remote.file);
		goto will_not_choose;
	}

	/* At this point, only one tablespace is open */
	ut_a(def.success == !remote.success);

	fsp = def.success ? &def : &remote;

	/* Get and test the file size. */
	size = os_file_get_size(fsp->file);

	if (size == (os_offset_t) -1) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"could not measure the size of single-table "
			"tablespace file %s", fsp->filepath);

		os_file_close(fsp->file);
		goto no_good_file;
	}

	/* Every .ibd file is created >= 4 pages in size. Smaller files
	cannot be ok. */
	minimum_size = FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE;
	if (size < minimum_size) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"The size of single-table tablespace file %s "
			"is only " UINT64PF ", should be at least %lu!",
			fsp->filepath, size, minimum_size);
		os_file_close(fsp->file);
		goto no_good_file;
	}

#ifdef UNIV_HOTBACKUP
	if (fsp->id == ULINT_UNDEFINED || fsp->id == 0) {
		char*	new_path;

		fprintf(stderr,
			"InnoDB: Renaming tablespace %s of id %lu,\n"
			"InnoDB: to %s_ibbackup_old_vers_<timestamp>\n"
			"InnoDB: because its size %" PRId64 " is too small"
			" (< 4 pages 16 kB each),\n"
			"InnoDB: or the space id in the file header"
			" is not sensible.\n"
			"InnoDB: This can happen in an mysqlbackup run,"
			" and is not dangerous.\n",
			fsp->filepath, fsp->id, fsp->filepath, size);
		os_file_close(fsp->file);

		new_path = fil_make_ibbackup_old_name(fsp->filepath);

		bool	success = os_file_rename(
			innodb_file_data_key, fsp->filepath, new_path);

		ut_a(success);

		mem_free(new_path);

		goto func_exit_after_close;
	}

	/* A backup may contain the same space several times, if the space got
	renamed at a sensitive time. Since it is enough to have one version of
	the space, we rename the file if a space with the same space id
	already exists in the tablespace memory cache. We rather rename the
	file than delete it, because if there is a bug, we do not want to
	destroy valuable data. */

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(fsp->id);

	if (space) {
		char*	new_path;

		fprintf(stderr,
			"InnoDB: Renaming tablespace %s of id %lu,\n"
			"InnoDB: to %s_ibbackup_old_vers_<timestamp>\n"
			"InnoDB: because space %s with the same id\n"
			"InnoDB: was scanned earlier. This can happen"
			" if you have renamed tables\n"
			"InnoDB: during an mysqlbackup run.\n",
			fsp->filepath, fsp->id, fsp->filepath,
			space->name);
		os_file_close(fsp->file);

		new_path = fil_make_ibbackup_old_name(fsp->filepath);

		mutex_exit(&fil_system->mutex);

		bool	success = os_file_rename(
			innodb_file_data_key, fsp->filepath, new_path);

		ut_a(success);

		mem_free(new_path);

		goto func_exit_after_close;
	}
	mutex_exit(&fil_system->mutex);
#endif /* UNIV_HOTBACKUP */

	/* Adjust the memory-based flags that would normally be set by
	dict_tf_to_fsp_flags(). In recovery, we have no data dictionary. */
	if (FSP_FLAGS_HAS_PAGE_COMPRESSION(fsp->flags)) {
		fsp->flags |= page_zip_level
			<< FSP_FLAGS_MEM_COMPRESSION_LEVEL;
	}
	remote.flags |= 1U << FSP_FLAGS_MEM_DATA_DIR;
	/* We will leave atomic_writes at ATOMIC_WRITES_DEFAULT.
	That will be adjusted in fil_space_for_table_exists_in_mem(). */

	file_space_create_success = fil_space_create(
		tablename, fsp->id, fsp->flags, FIL_TABLESPACE,
		fsp->crypt_data, false);

	if (!file_space_create_success) {
		if (srv_force_recovery > 0) {
			fprintf(stderr,
				"InnoDB: innodb_force_recovery was set"
				" to %lu. Continuing crash recovery\n"
				"InnoDB: even though the tablespace"
				" creation of this table failed.\n",
				srv_force_recovery);
			goto func_exit;
		}

		/* Exit here with a core dump, stack, etc. */
		ut_a(file_space_create_success);
	}

	/* We do not use the size information we have about the file, because
	the rounding formula for extents and pages is somewhat complex; we
	let fil_node_open() do that task. */

	if (!fil_node_create(fsp->filepath, 0, fsp->id, FALSE)) {
		ut_error;
	}

func_exit:
	/* We reuse file handles on the backup stage in XtraBackup to avoid
	inconsistencies between the file name and the actual tablespace contents
	if a DDL occurs between a fil_load_single_table_tablespaces() call and
	the actual copy operation. */
	if (IS_XTRABACKUP() && srv_backup_mode && !srv_close_files) {

		fil_node_t*	node;
		fil_space_t*	space;

		mutex_enter(&fil_system->mutex);

		space = fil_space_get_by_id(fsp->id);

		if (space) {
			node = UT_LIST_GET_LAST(space->chain);

			/* The handle will be closed by xtrabackup in
			xtrabackup_copy_datafile(). We set node->open to TRUE to
			make sure no one calls fil_node_open_file()
			(i.e. attempts to reopen the tablespace by name) during
			the backup stage. */

			node->open = TRUE;
			node->handle = fsp->file;

			/* The following is copied from fil_node_open_file() to
			pass fil_system validaty checks. We cannot use
			fil_node_open_file() directly, as that would re-open the
			file by name and create another file handle. */

			fil_system->n_open++;
			fil_n_file_opened++;

			if (fil_space_belongs_in_lru(space)) {

				/* Put the node to the LRU list */
				UT_LIST_ADD_FIRST(LRU, fil_system->LRU, node);
			}
		}

		mutex_exit(&fil_system->mutex);
	}
	else {
		os_file_close(fsp->file);
	}


func_exit_after_close:
	ut_ad(!mutex_own(&fil_system->mutex));

	mem_free(tablename);
	if (remote.success) {
		mem_free(remote.filepath);
	}
	mem_free(def.filepath);
}

/***********************************************************************//**
A fault-tolerant function that tries to read the next file name in the
directory. We retry 100 times if os_file_readdir_next_file() returns -1. The
idea is to read as much good data as we can and jump over bad data.
@return 0 if ok, -1 if error even after the retries, 1 if at the end
of the directory */
UNIV_INTERN
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

		ib_logf(IB_LOG_LEVEL_ERROR,
			"os_file_readdir_next_file() returned -1 in "
			"directory %s, crash recovery may have failed "
			"for some .ibd files!", dirname);

		*err = DB_ERROR;
	}

	return(-1);
}


my_bool(*fil_check_if_skip_database_by_path)(const char* name);

#define CHECK_TIME_EVERY_N_FILES   10
/********************************************************************//**
At the server startup, if we need crash recovery, scans the database
directories under the MySQL datadir, looking for .ibd files. Those files are
single-table tablespaces. We need to know the space id in each of them so that
we know into which file we should look to check the contents of a page stored
in the doublewrite buffer, also to know where to apply log records where the
space id is != 0.
@return	DB_SUCCESS or error number */
UNIV_INTERN
dberr_t
fil_load_single_table_tablespaces(ibool (*pred)(const char*, const char*))
/*===================================*/
{
	int		ret;
	char*		dbpath		= NULL;
	ulint		dbpath_len	= 100;
        ulint 		files_read	= 0;
        ulint 		files_read_at_last_check	= 0;
	time_t		prev_report_time = time(NULL);
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	dberr_t		err		= DB_SUCCESS;

	/* The datadir of MySQL is always the default directory of mysqld */

	dir = os_file_opendir(fil_path_to_mysql_datadir, TRUE);

	if (dir == NULL) {

		return(DB_ERROR);
	}

	dbpath = static_cast<char*>(mem_alloc(dbpath_len));

	/* Scan all directories under the datadir. They are the database
	directories of MySQL. */

	ret = fil_file_readdir_next_file(&err, fil_path_to_mysql_datadir, dir,
					 &dbinfo);
	while (ret == 0) {
		ulint len;
		/* printf("Looking at %s in datadir\n", dbinfo.name); */

		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

			goto next_datadir_item;
		}

		/* We found a symlink or a directory; try opening it to see
		if a symlink is a directory */

		len = strlen(fil_path_to_mysql_datadir)
			+ strlen (dbinfo.name) + 2;
		if (len > dbpath_len) {
			dbpath_len = len;

			if (dbpath) {
				mem_free(dbpath);
			}

			dbpath = static_cast<char*>(mem_alloc(dbpath_len));
		}
		ut_snprintf(dbpath, dbpath_len,
			    "%s/%s", fil_path_to_mysql_datadir, dbinfo.name);
		srv_normalize_path_for_win(dbpath);

		if (IS_XTRABACKUP()) {
			ut_a(fil_check_if_skip_database_by_path);
			if (fil_check_if_skip_database_by_path(dbpath)) {
				fprintf(stderr, "Skipping db: %s\n", dbpath);
				dbdir = NULL;
			} else {
				/* We want wrong directory permissions to be a fatal
				error for XtraBackup. */
				dbdir = os_file_opendir(dbpath, TRUE);
			}
		} else {
			dbdir = os_file_opendir(dbpath, FALSE);
		}

		if (dbdir != NULL) {

			/* We found a database directory; loop through it,
			looking for possible .ibd files in it */

			ret = fil_file_readdir_next_file(&err, dbpath, dbdir,
							 &fileinfo);
			while (ret == 0) {

				if (fileinfo.type == OS_FILE_TYPE_DIR) {

					goto next_file_item;
				}

				/* We found a symlink or a file

				Ignore .isl files on XtraBackup
				recovery, all tablespaces must be local. */
				if (strlen(fileinfo.name) > 4
				    && (0 == strcmp(fileinfo.name
						   + strlen(fileinfo.name) - 4,
						   ".ibd")
					|| ((!IS_XTRABACKUP() || srv_backup_mode) 
							&& 0 == strcmp(fileinfo.name
							 + strlen(fileinfo.name) - 4,
							".isl")))
					 && (!pred ||
						pred(dbinfo.name, fileinfo.name))) {
					/* The name ends in .ibd or .isl;
					try opening the file */
					fil_load_single_table_tablespace(
						dbinfo.name, fileinfo.name);
					files_read++;
					if (files_read - files_read_at_last_check >
					    CHECK_TIME_EVERY_N_FILES) {
						time_t cur_time= time(NULL);
						files_read_at_last_check= files_read;
						if (cur_time - prev_report_time
						    > 15) {
							ib_logf(IB_LOG_LEVEL_INFO, 
								"Processed %ld .ibd/.isl files",
								files_read);
							prev_report_time= cur_time;
						}
                                        }
				}
next_file_item:
				ret = fil_file_readdir_next_file(&err,
								 dbpath, dbdir,
								 &fileinfo);
			}

			if (0 != os_file_closedir(dbdir)) {
				fputs("InnoDB: Warning: could not"
				      " close database directory ", stderr);
				ut_print_filename(stderr, dbpath);
				putc('\n', stderr);

				err = DB_ERROR;
			}
		}

next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						 fil_path_to_mysql_datadir,
						 dir, &dbinfo);
	}

	mem_free(dbpath);

	if (0 != os_file_closedir(dir)) {
		fprintf(stderr,
			"InnoDB: Error: could not close MySQL datadir\n");

		return(DB_ERROR);
	}

	return(err);
}

/*******************************************************************//**
Returns TRUE if a single-table tablespace does not exist in the memory cache,
or is being deleted there.
@return	TRUE if does not exist or is being deleted */
UNIV_INTERN
ibool
fil_tablespace_deleted_or_being_deleted_in_mem(
/*===========================================*/
	ulint		id,	/*!< in: space id */
	ib_int64_t	version)/*!< in: tablespace_version should be this; if
				you pass -1 as the value of this, then this
				parameter is ignored */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL || space->is_stopping()) {
		mutex_exit(&fil_system->mutex);

		return(TRUE);
	}

	if (version != ((ib_int64_t)-1)
	    && space->tablespace_version != version) {
		mutex_exit(&fil_system->mutex);

		return(TRUE);
	}

	mutex_exit(&fil_system->mutex);

	return(FALSE);
}

/*******************************************************************//**
Returns TRUE if a single-table tablespace exists in the memory cache.
@return	TRUE if exists */
UNIV_INTERN
ibool
fil_tablespace_exists_in_mem(
/*=========================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	mutex_exit(&fil_system->mutex);

	return(space != NULL);
}

/*******************************************************************//**
Report that a tablespace for a table was not found. */
static
void
fil_report_missing_tablespace(
/*===========================*/
	const char*	name,			/*!< in: table name */
	ulint		space_id)		/*!< in: table's space id */
{
	char index_name[MAX_FULL_NAME_LEN + 1];

	innobase_format_name(index_name, sizeof(index_name), name, TRUE);

	ib_logf(IB_LOG_LEVEL_ERROR,
		"Table %s in the InnoDB data dictionary has tablespace id %lu, "
		"but tablespace with that id or name does not exist. Have "
		"you deleted or moved .ibd files? This may also be a table "
		"created with CREATE TEMPORARY TABLE whose .ibd and .frm "
		"files MySQL automatically removed, but the table still "
		"exists in the InnoDB internal data dictionary.",
		name, space_id);
}

/** Check if a matching tablespace exists in the InnoDB tablespace memory
cache. Note that if we have not done a crash recovery at the database startup,
there may be many tablespaces which are not yet in the memory cache.
@return whether a matching tablespace exists in the memory cache */
UNIV_INTERN
bool
fil_space_for_table_exists_in_mem(
/*==============================*/
	ulint		id,		/*!< in: space id */
	const char*	name,		/*!< in: table name used in
					fil_space_create().  Either the
					standard 'dbname/tablename' format
					or table->dir_path_of_temp_table */
	bool		print_error_if_does_not_exist,
					/*!< in: print detailed error
					information to the .err log if a
					matching tablespace is not found from
					memory */
	bool		remove_from_data_dict_if_does_not_exist,
					/*!< in: remove from the data dictionary
					if tablespace does not exist */
	bool		adjust_space,	/*!< in: whether to adjust space id
					when find table space mismatch */
	mem_heap_t*	heap,		/*!< in: heap memory */
	table_id_t	table_id,	/*!< in: table id */
	ulint		table_flags)	/*!< in: table flags */
{
	fil_space_t*	fnamespace;
	fil_space_t*	space;

	const ulint	expected_flags = dict_tf_to_fsp_flags(table_flags);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same id */

	space = fil_space_get_by_id(id);

	/* Look if there is a space with the same name; the name is the
	directory path from the datadir to the file */

	fnamespace = fil_space_get_by_name(name);
	bool valid = space && !((space->flags ^ expected_flags)
				& ~FSP_FLAGS_MEM_MASK);

	if (!space) {
	} else if (!valid || space == fnamespace) {
		/* Found with the same file name, or got a flag mismatch. */
		goto func_exit;
	} else if (adjust_space
		   && row_is_mysql_tmp_table_name(space->name)
		   && !row_is_mysql_tmp_table_name(name)) {
		/* Info from fnamespace comes from the ibd file
		itself, it can be different from data obtained from
		System tables since renaming files is not
		transactional. We shall adjust the ibd file name
		according to system table info. */
		mutex_exit(&fil_system->mutex);

		DBUG_EXECUTE_IF("ib_crash_before_adjust_fil_space",
				DBUG_SUICIDE(););

		char*	tmp_name = dict_mem_create_temporary_tablename(
			heap, name, table_id);

		fil_rename_tablespace(fnamespace->name, fnamespace->id,
				      tmp_name, NULL);

		DBUG_EXECUTE_IF("ib_crash_after_adjust_one_fil_space",
				DBUG_SUICIDE(););

		fil_rename_tablespace(space->name, id, name, NULL);

		DBUG_EXECUTE_IF("ib_crash_after_adjust_fil_space",
				DBUG_SUICIDE(););

		mutex_enter(&fil_system->mutex);
		fnamespace = fil_space_get_by_name(name);
		ut_ad(space == fnamespace);
		goto func_exit;
	}

	if (!print_error_if_does_not_exist) {
		valid = false;
		goto func_exit;
	}

	if (space == NULL) {
		if (fnamespace == NULL) {
			if (print_error_if_does_not_exist) {
				fil_report_missing_tablespace(name, id);
				if (IS_XTRABACKUP() && remove_from_data_dict_if_does_not_exist) {
					ib_logf(IB_LOG_LEVEL_WARN,
						"It will be removed from "
						"the data dictionary.");
				}
			}
		} else {
			ut_print_timestamp(stderr);
			fputs("  InnoDB: Error: table ", stderr);
			ut_print_filename(stderr, name);
			fprintf(stderr, "\n"
				"InnoDB: in InnoDB data dictionary has"
				" tablespace id %lu,\n"
				"InnoDB: but a tablespace with that id"
				" does not exist. There is\n"
				"InnoDB: a tablespace of name %s and id %lu,"
				" though. Have\n"
				"InnoDB: you deleted or moved .ibd files?\n",
				(ulong) id, fnamespace->name,
				(ulong) fnamespace->id);
		}
error_exit:
		fputs("InnoDB: Please refer to\n"
		      "InnoDB: " REFMAN "innodb-troubleshooting-datadict.html\n"
		      "InnoDB: for how to resolve the issue.\n", stderr);
		valid = false;
		goto func_exit;
	}

	if (0 != strcmp(space->name, name)) {
		ut_print_timestamp(stderr);
		fputs("  InnoDB: Error: table ", stderr);
		ut_print_filename(stderr, name);
		fprintf(stderr, "\n"
			"InnoDB: in InnoDB data dictionary has"
			" tablespace id %lu,\n"
			"InnoDB: but the tablespace with that id"
			" has name %s.\n"
			"InnoDB: Have you deleted or moved .ibd files?\n",
			(ulong) id, space->name);

		if (fnamespace != NULL) {
			fputs("InnoDB: There is a tablespace"
			      " with the right name\n"
			      "InnoDB: ", stderr);
			ut_print_filename(stderr, fnamespace->name);
			fprintf(stderr, ", but its id is %lu.\n",
				(ulong) fnamespace->id);
		}

		goto error_exit;
	}

func_exit:
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

/*******************************************************************//**
Checks if a single-table tablespace for a given table name exists in the
tablespace memory cache.
@return	space id, ULINT_UNDEFINED if not found */
UNIV_INTERN
ulint
fil_get_space_id_for_table(
/*=======================*/
	const char*	tablename)	/*!< in: table name in the standard
				'databasename/tablename' format */
{
	fil_space_t*	fnamespace;
	ulint		id		= ULINT_UNDEFINED;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same name. */

	fnamespace = fil_space_get_by_name(tablename);

	if (fnamespace) {
		id = fnamespace->id;
	}

	mutex_exit(&fil_system->mutex);

	return(id);
}

/**********************************************************************//**
Tries to extend a data file so that it would accommodate the number of pages
given. The tablespace must be cached in the memory cache. If the space is big
enough already, does nothing.
@return	TRUE if success */
UNIV_INTERN
ibool
fil_extend_space_to_desired_size(
/*=============================*/
	ulint*	actual_size,	/*!< out: size of the space after extension;
				if we ran out of disk space this may be lower
				than the desired size */
	ulint	space_id,	/*!< in: space id */
	ulint	size_after_extend)/*!< in: desired size in pages after the
				extension; if the current space size is bigger
				than this already, the function does nothing */
{
	ut_ad(!srv_read_only_mode);

	for (;;) {
		fil_mutex_enter_and_prepare_for_io(space_id);

		fil_space_t* space = fil_space_get_by_id(space_id);
		ut_a(space);
		ibool	success;

		if (!fil_space_extend_must_retry(
			    space, UT_LIST_GET_LAST(space->chain),
			    size_after_extend, &success)) {
			*actual_size = space->size;
			mutex_exit(&fil_system->mutex);
			return(success);
		}
	}
}

#ifdef UNIV_HOTBACKUP
/********************************************************************//**
Extends all tablespaces to the size stored in the space header. During the
mysqlbackup --apply-log phase we extended the spaces on-demand so that log
records could be applied, but that may have left spaces still too small
compared to the size stored in the space header. */
UNIV_INTERN
void
fil_extend_tablespaces_to_stored_len(void)
/*======================================*/
{
	fil_space_t*	space;
	byte*		buf;
	ulint		actual_size;
	ulint		size_in_header;
	dberr_t		error;
	ibool		success;

	buf = mem_alloc(UNIV_PAGE_SIZE);

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space) {
		ut_a(space->purpose == FIL_TABLESPACE);

		mutex_exit(&fil_system->mutex); /* no need to protect with a
					      mutex, because this is a
					      single-threaded operation */
		error = fil_read(TRUE, space->id,
				 fsp_flags_get_zip_size(space->flags),
			         0, 0, UNIV_PAGE_SIZE, buf, NULL, 0);
		ut_a(error == DB_SUCCESS);

		size_in_header = fsp_get_size_low(buf);

		success = fil_extend_space_to_desired_size(
			&actual_size, space->id, size_in_header);
		if (!success) {
			fprintf(stderr,
				"InnoDB: Error: could not extend the"
				" tablespace of %s\n"
				"InnoDB: to the size stored in header,"
				" %lu pages;\n"
				"InnoDB: size after extension %lu pages\n"
				"InnoDB: Check that you have free disk space"
				" and retry!\n",
				space->name, size_in_header, actual_size);
			ut_a(success);
		}

		mutex_enter(&fil_system->mutex);

		space = UT_LIST_GET_NEXT(space_list, space);
	}

	mutex_exit(&fil_system->mutex);

	mem_free(buf);
}
#endif

/*========== RESERVE FREE EXTENTS (for a B-tree split, for example) ===*/

/*******************************************************************//**
Tries to reserve free extents in a file space.
@return	TRUE if succeed */
UNIV_INTERN
ibool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve)	/*!< in: how many one wants to reserve */
{
	fil_space_t*	space;
	ibool		success;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	if (space->n_reserved_extents + n_to_reserve > n_free_now) {
		success = FALSE;
	} else {
		space->n_reserved_extents += n_to_reserve;
		success = TRUE;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/*******************************************************************//**
Releases free extents in a file space. */
UNIV_INTERN
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
UNIV_INTERN
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
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: Warning: open files %lu"
			" exceeds the limit %lu\n",
			(ulong) system->n_open,
			(ulong) system->max_n_open);
	}

	if (node->open == FALSE) {
		/* File is closed: open it */
		ut_a(node->n_pending == 0);

		if (!fil_node_open_file(node, system, space)) {
			return(false);
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(space)) {
		/* The node is in the LRU list, remove it */

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		UT_LIST_REMOVE(LRU, system->LRU, node);
	}

	node->n_pending++;

	return(true);
}

/********************************************************************//**
Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the node appropriately. */
static
void
fil_node_complete_io(
/*=================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	ulint		type)	/*!< in: OS_FILE_WRITE or OS_FILE_READ; marks
				the node as modified if
				type == OS_FILE_WRITE */
{
	ut_ad(node);
	ut_ad(system);
	ut_ad(mutex_own(&(system->mutex)));

	ut_a(node->n_pending > 0);

	node->n_pending--;

	if (type == OS_FILE_WRITE) {
		ut_ad(!srv_read_only_mode);
		system->modification_counter++;
		node->modification_counter = system->modification_counter;

		if (fil_buffering_disabled(node->space)) {

			/* We don't need to keep track of unflushed
			changes as user has explicitly disabled
			buffering. */
			ut_ad(!node->space->is_in_unflushed_spaces);
			node->flush_counter = node->modification_counter;

		} else if (!node->space->is_in_unflushed_spaces) {

			node->space->is_in_unflushed_spaces = true;
			UT_LIST_ADD_FIRST(unflushed_spaces,
					  system->unflushed_spaces,
					  node->space);
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(node->space)) {

		/* The node must be put back to the LRU list */
		UT_LIST_ADD_FIRST(LRU, system->LRU, node);
	}
}

/********************************************************************//**
Report information about an invalid page access. */
static
void
fil_report_invalid_page_access(
/*===========================*/
	ulint		block_offset,	/*!< in: block offset */
	ulint		space_id,	/*!< in: space id */
	const char*	space_name,	/*!< in: space name */
	ulint		byte_offset,	/*!< in: byte offset */
	ulint		len,		/*!< in: I/O length */
	ulint		type)		/*!< in: I/O type */
{
	ib_logf(IB_LOG_LEVEL_FATAL,
		"Trying to access page number " ULINTPF
		" in space " ULINTPF
		" space name %s,"
		" which is outside the tablespace bounds."
		" Byte offset " ULINTPF ", len " ULINTPF
		" i/o type " ULINTPF ".%s",
		block_offset, space_id, space_name,
		byte_offset, len, type,
		space_id == 0 && !srv_was_started
		? "Please check that the configuration matches"
		" the InnoDB system tablespace location (ibdata files)"
		: "");
}

/********************************************************************//**
Find correct node from file space
@return node */
static
fil_node_t*
fil_space_get_node(
	fil_space_t*	space,		/*!< in: file spage */
	ulint 		space_id,	/*!< in: space id   */
	ulint* 		block_offset,	/*!< in/out: offset in number of blocks */
	ulint 		byte_offset,	/*!< in: remainder of offset in bytes; in
					aio this must be divisible by the OS block
					size */
	ulint 		len)		/*!< in: how many bytes to read or write; this
					must not cross a file boundary; in aio this
					must be a block size multiple */
{
	fil_node_t*	node;
	ut_ad(mutex_own(&fil_system->mutex));

	node = UT_LIST_GET_FIRST(space->chain);

	for (;;) {
		if (node == NULL) {
			return(NULL);
		} else if (fil_is_user_tablespace_id(space->id)
			   && node->size == 0) {

			/* We do not know the size of a single-table tablespace
			before we open the file */
			break;
		} else if (node->size > *block_offset) {
			/* Found! */
			break;
		} else {
			(*block_offset) -= node->size;
			node = UT_LIST_GET_NEXT(chain, node);
		}
	}

	return (node);
}

/** Determine the block size of the data file.
@param[in]	space		tablespace
@param[in]	offset		page number
@return	block size */
UNIV_INTERN
ulint
fil_space_get_block_size(const fil_space_t* space, unsigned offset)
{
	ut_ad(space->n_pending_ios > 0);

	ulint block_size = 512;

	for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {
		block_size = node->file_block_size;
		if (node->size > offset) {
			break;
		}
		offset -= node->size;
	}

	/* Currently supporting block size up to 4K,
	fall back to default if bigger requested. */
	if (block_size > 4096) {
		block_size = 512;
	}

	return block_size;
}

/********************************************************************//**
Reads or writes data. This operation is asynchronous (aio).
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INTERN
dberr_t
fil_io(
/*===*/
	ulint	type,		/*!< in: OS_FILE_READ or OS_FILE_WRITE,
				ORed to OS_FILE_LOG, if a log i/o
				and ORed to OS_AIO_SIMULATED_WAKE_LATER
				if simulated aio and we want to post a
				batch of i/os; NOTE that a simulated batch
				may introduce hidden chances of deadlocks,
				because i/os are not actually handled until
				all have been posted: use with great
				caution! */
	bool	sync,		/*!< in: true if synchronous aio is desired */
	ulint	space_id,	/*!< in: space id */
	ulint	zip_size,	/*!< in: compressed page size in bytes;
				0 for uncompressed pages */
	ulint	block_offset,	/*!< in: offset in number of blocks */
	ulint	byte_offset,	/*!< in: remainder of offset in bytes; in
				aio this must be divisible by the OS block
				size */
	ulint	len,		/*!< in: how many bytes to read or write; this
				must not cross a file boundary; in aio this
				must be a block size multiple */
	void*	buf,		/*!< in/out: buffer where to store read data
				or from where to write; in aio this must be
				appropriately aligned */
	void*	message,	/*!< in: message for aio handler if non-sync
				aio used, else ignored */
	ulint*	write_size,	/*!< in/out: Actual write size initialized
				after fist successfull trim
				operation for this page and if
				initialized we do not trim again if
				actual page size does not decrease. */
	trx_t*	trx,
	bool	should_buffer)	/*!< in: whether to buffer an aio request.
				AIO read ahead uses this. If you plan to
				use this parameter, make sure you remember
				to call os_aio_dispatch_read_array_submit()
				when you're ready to commit all your requests.*/
{
	ulint		mode;
	fil_space_t*	space;
	fil_node_t*	node;
	ibool		ret=TRUE;
	ulint		is_log;
	ulint		wake_later;
	os_offset_t	offset;
	bool		ignore_nonexistent_pages;

	is_log = type & OS_FILE_LOG;
	type = type & ~OS_FILE_LOG;

	wake_later = type & OS_AIO_SIMULATED_WAKE_LATER;
	type = type & ~OS_AIO_SIMULATED_WAKE_LATER;

	ignore_nonexistent_pages = type & BUF_READ_IGNORE_NONEXISTENT_PAGES;
	type &= ~BUF_READ_IGNORE_NONEXISTENT_PAGES;

	ut_ad(byte_offset < UNIV_PAGE_SIZE);
	ut_ad(!zip_size || !byte_offset);
	ut_ad(ut_is_2pow(zip_size));
	ut_ad(buf);
	ut_ad(len > 0);
	ut_ad(UNIV_PAGE_SIZE == (ulong)(1 << UNIV_PAGE_SIZE_SHIFT));
#if (1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX"
#endif
#if (1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN"
#endif
	ut_ad(fil_validate_skip());
#ifndef UNIV_HOTBACKUP
# ifndef UNIV_LOG_DEBUG
	/* ibuf bitmap pages must be read in the sync aio mode: */
	ut_ad(recv_no_ibuf_operations
	      || type == OS_FILE_WRITE
	      || !ibuf_bitmap_page(zip_size, block_offset)
	      || sync
	      || is_log);
# endif /* UNIV_LOG_DEBUG */
	if (sync) {
		mode = OS_AIO_SYNC;
	} else if (is_log) {
		mode = OS_AIO_LOG;
	} else if (type == OS_FILE_READ
		   && !recv_no_ibuf_operations
		   && ibuf_page(space_id, zip_size, block_offset, NULL)) {
		mode = OS_AIO_IBUF;
	} else {
		mode = OS_AIO_NORMAL;
	}
#else /* !UNIV_HOTBACKUP */
	ut_a(sync);
	mode = OS_AIO_SYNC;
#endif /* !UNIV_HOTBACKUP */

	if (type == OS_FILE_READ) {
		srv_stats.data_read.add(len);
	} else if (type == OS_FILE_WRITE) {
		ut_ad(!srv_read_only_mode);
		srv_stats.data_written.add(len);
		if (mach_read_from_2(static_cast<const byte*>(buf)
				     + FIL_PAGE_TYPE) == FIL_PAGE_INDEX) {
			srv_stats.index_pages_written.inc();
		} else {
			srv_stats.non_index_pages_written.inc();
		}
	}

	/* Reserve the fil_system mutex and make sure that we can open at
	least one file while holding it, if the file is not already open */

	fil_mutex_enter_and_prepare_for_io(space_id);

	space = fil_space_get_by_id(space_id);

	/* If we are deleting a tablespace we don't allow async read operations
	on that. However, we do allow write and sync read operations */
	if (space == 0
	    || (type == OS_FILE_READ
		&& !sync
		&& space->stop_new_ops)) {
		mutex_exit(&fil_system->mutex);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to do i/o to a tablespace which does "
			"not exist. i/o type " ULINTPF
			", space id " ULINTPF " , "
			"page no. " ULINTPF
			", i/o length " ULINTPF " bytes",
			type, space_id, block_offset,
			len);

		return(DB_TABLESPACE_DELETED);
	}

	ut_ad(mode != OS_AIO_IBUF || space->purpose == FIL_TABLESPACE);

	node = fil_space_get_node(space, space_id, &block_offset, byte_offset, len);

	if (!node) {
		if (ignore_nonexistent_pages) {
			mutex_exit(&fil_system->mutex);
			return(DB_ERROR);
		}

		fil_report_invalid_page_access(
				block_offset, space_id, space->name,
				byte_offset, len, type);
	}

	/* Open file if closed */
	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		if (space->purpose == FIL_TABLESPACE
		    && fil_is_user_tablespace_id(space->id)) {
			mutex_exit(&fil_system->mutex);

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Trying to do i/o to a tablespace which "
				"exists without .ibd data file. "
				"i/o type " ULINTPF ", space id "
				ULINTPF ", page no " ULINTPF ", "
				"i/o length " ULINTPF " bytes",
				type, space_id,
				block_offset, len);

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
	if (UNIV_UNLIKELY(node->size <= block_offset)
		&& space->id != 0 && space->purpose == FIL_TABLESPACE) {

		fil_report_invalid_page_access(
			block_offset, space_id, space->name, byte_offset,
			len, type);
	}

	/* Now we have made the changes in the data structures of fil_system */
	mutex_exit(&fil_system->mutex);

	/* Calculate the low 32 bits and the high 32 bits of the file offset */

	if (!zip_size) {
		offset = ((os_offset_t) block_offset << UNIV_PAGE_SIZE_SHIFT)
			+ byte_offset;

		ut_a(node->size - block_offset
		     >= ((byte_offset + len + (UNIV_PAGE_SIZE - 1))
			 / UNIV_PAGE_SIZE));
	} else {
		ulint	zip_size_shift;
		switch (zip_size) {
		case 1024: zip_size_shift = 10; break;
		case 2048: zip_size_shift = 11; break;
		case 4096: zip_size_shift = 12; break;
		case 8192: zip_size_shift = 13; break;
		case 16384: zip_size_shift = 14; break;
		case 32768: zip_size_shift = 15; break;
		case 65536: zip_size_shift = 16; break;
		default: ut_error;
		}
		offset = ((os_offset_t) block_offset << zip_size_shift)
			+ byte_offset;
		ut_a(node->size - block_offset
		     >= (len + (zip_size - 1)) / zip_size);
	}

	/* Do aio */

	ut_a(byte_offset % OS_MIN_LOG_BLOCK_SIZE == 0);
	ut_a((len % OS_MIN_LOG_BLOCK_SIZE) == 0);

#ifndef UNIV_HOTBACKUP
	if (UNIV_UNLIKELY(space->is_corrupt && srv_pass_corrupt_table)) {

		/* should ignore i/o for the crashed space */
		if (srv_pass_corrupt_table == 1 ||
		    type == OS_FILE_WRITE) {

			mutex_enter(&fil_system->mutex);
			fil_node_complete_io(node, fil_system, type);
			mutex_exit(&fil_system->mutex);
			if (mode == OS_AIO_NORMAL) {
				ut_a(space->purpose == FIL_TABLESPACE);
				dberr_t err = buf_page_io_complete(static_cast<buf_page_t *>
					(message));

				if (err != DB_SUCCESS) {
					ib_logf(IB_LOG_LEVEL_ERROR,
						"Write operation failed for tablespace %s ("
						ULINTPF ") offset " ULINTPF " error=%d.",
						space->name, space->id, byte_offset, err);
				}
			}
		}

		if (srv_pass_corrupt_table == 1 && type == OS_FILE_READ) {

			return(DB_TABLESPACE_DELETED);

		} else if (type == OS_FILE_WRITE) {

			return(DB_SUCCESS);
		}
	}

	const char* name = node->name == NULL ? space->name : node->name;

	/* Queue the aio request */
	ret = os_aio(type, is_log, mode | wake_later, name, node->handle, buf,
		     offset, len, zip_size ? zip_size : UNIV_PAGE_SIZE, node,
		     message, space_id, trx, write_size, should_buffer);

#else
	/* In mysqlbackup do normal i/o, not aio */
	if (type == OS_FILE_READ) {
		ret = os_file_read(node->handle, buf, offset, len);
	} else {
		ut_ad(!srv_read_only_mode);
		ret = os_file_write(name, node->handle, buf,
				    offset, len);
	}
#endif /* !UNIV_HOTBACKUP */

	if (mode == OS_AIO_SYNC) {
		/* The i/o operation is already completed when we return from
		os_aio: */

		mutex_enter(&fil_system->mutex);

		fil_node_complete_io(node, fil_system, type);

		mutex_exit(&fil_system->mutex);

		ut_ad(fil_validate_skip());
	}

	if (!ret) {
		return(DB_OUT_OF_FILE_SPACE);
	}

	return(DB_SUCCESS);
}

#ifndef UNIV_HOTBACKUP
/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */
UNIV_INTERN
void
fil_aio_wait(
/*=========*/
	ulint	segment)	/*!< in: the number of the segment in the aio
				array to wait for */
{
	ibool		ret;
	fil_node_t*	fil_node;
	void*		message;
	ulint		type;
	ulint		space_id = 0;

	ut_ad(fil_validate_skip());

	if (srv_use_native_aio) {
		srv_set_io_thread_op_info(segment, "native aio handle");
#ifdef WIN_ASYNC_IO
		ret = os_aio_windows_handle(
			segment, 0, &fil_node, &message, &type, &space_id);
#elif defined(LINUX_NATIVE_AIO)
		ret = os_aio_linux_handle(
			segment, &fil_node, &message, &type, &space_id);
#else
		ut_error;
		ret = 0; /* Eliminate compiler warning */
#endif /* WIN_ASYNC_IO */
	} else {
		srv_set_io_thread_op_info(segment, "simulated aio handle");

		ret = os_aio_simulated_handle(
			segment, &fil_node, &message, &type, &space_id);
	}

	ut_a(ret);
	if (fil_node == NULL) {
		ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
		return;
	}

	srv_set_io_thread_op_info(segment, "complete io for fil node");

	mutex_enter(&fil_system->mutex);

	fil_node_complete_io(fil_node, fil_system, type);
	ulint purpose = fil_node->space->purpose;
	space_id = fil_node->space->id;

	mutex_exit(&fil_system->mutex);

	ut_ad(fil_validate_skip());

	/* Do the i/o handling */
	/* IMPORTANT: since i/o handling for reads will read also the insert
	buffer in tablespace 0, you have to be very careful not to introduce
	deadlocks in the i/o system. We keep tablespace 0 data files always
	open, and use a special i/o thread to serve insert buffer requests. */

	if (purpose == FIL_TABLESPACE) {
		srv_set_io_thread_op_info(segment, "complete io for buf page");
		buf_page_t* bpage = static_cast<buf_page_t*>(message);
		ulint offset = bpage->offset;
		dberr_t err = buf_page_io_complete(bpage);

		if (err != DB_SUCCESS) {
			ut_ad(type == OS_FILE_READ);
			/* In crash recovery set log corruption on
			and produce only an error to fail InnoDB startup. */
			if (recv_recovery_is_on() && !srv_force_recovery) {
				recv_sys->found_corrupt_log = true;
			}

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Read operation failed for tablespace %s"
				" offset " ULINTPF " with error %s",
				fil_node->name,
				offset,
				ut_strerr(err));
		}
	} else {
		srv_set_io_thread_op_info(segment, "complete io for log");
		log_io_complete(static_cast<log_group_t*>(message));
	}
}
#endif /* UNIV_HOTBACKUP */

/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */
UNIV_INTERN
void
fil_flush(
/*======*/
	ulint	space_id)	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
{
	mutex_enter(&fil_system->mutex);

	if (fil_space_t* space = fil_space_get_by_id(space_id)) {
		if (!space->stop_new_ops) {

			fil_flush_low(space);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Flush a tablespace.
@param[in,out]	space	tablespace to flush */
UNIV_INTERN
void
fil_flush(fil_space_t* space)
{
	ut_ad(space->n_pending_ios > 0);

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
UNIV_INTERN
void
fil_flush_file_spaces(ulint purpose)
{
	fil_space_t*	space;
	ulint*		space_ids;
	ulint		n_space_ids;
	ulint		i;

	mutex_enter(&fil_system->mutex);

	n_space_ids = UT_LIST_GET_LEN(fil_system->unflushed_spaces);
	if (n_space_ids == 0) {

		mutex_exit(&fil_system->mutex);
		return;
	}

	/* Assemble a list of space ids to flush.  Previously, we
	traversed fil_system->unflushed_spaces and called UT_LIST_GET_NEXT()
	on a space that was just removed from the list by fil_flush().
	Thus, the space could be dropped and the memory overwritten. */
	space_ids = static_cast<ulint*>(
		mem_alloc(n_space_ids * sizeof *space_ids));

	n_space_ids = 0;

	for (space = UT_LIST_GET_FIRST(fil_system->unflushed_spaces);
	     space;
	     space = UT_LIST_GET_NEXT(unflushed_spaces, space)) {

		if (space->purpose == purpose && !space->is_stopping()) {
			space_ids[n_space_ids++] = space->id;
		}
	}

	mutex_exit(&fil_system->mutex);

	/* Flush the spaces.  It will not hurt to call fil_flush() on
	a non-existing space id. */
	for (i = 0; i < n_space_ids; i++) {

		fil_flush(space_ids[i]);
	}

	mem_free(space_ids);
}

/** Functor to validate the space list. */
struct	Check {
	void	operator()(const fil_node_t* elem)
	{
		ut_a(elem->open || !elem->n_pending);
	}
};

/******************************************************************//**
Checks the consistency of the tablespace cache.
@return	TRUE if ok */
UNIV_INTERN
ibool
fil_validate(void)
/*==============*/
{
	fil_space_t*	space;
	fil_node_t*	fil_node;
	ulint		n_open		= 0;
	ulint		i;

	mutex_enter(&fil_system->mutex);

	/* Look for spaces in the hash table */

	for (i = 0; i < hash_get_n_cells(fil_system->spaces); i++) {

		for (space = static_cast<fil_space_t*>(
				HASH_GET_FIRST(fil_system->spaces, i));
		     space != 0;
		     space = static_cast<fil_space_t*>(
			     	HASH_GET_NEXT(hash, space))) {

			UT_LIST_VALIDATE(
				chain, fil_node_t, space->chain, Check());

			for (fil_node = UT_LIST_GET_FIRST(space->chain);
			     fil_node != 0;
			     fil_node = UT_LIST_GET_NEXT(chain, fil_node)) {

				if (fil_node->n_pending > 0) {
					ut_a(fil_node->open);
				}

				if (fil_node->open) {
					n_open++;
				}
			}
		}
	}

	ut_a(fil_system->n_open == n_open);

	UT_LIST_CHECK(LRU, fil_node_t, fil_system->LRU);

	for (fil_node = UT_LIST_GET_FIRST(fil_system->LRU);
	     fil_node != 0;
	     fil_node = UT_LIST_GET_NEXT(LRU, fil_node)) {

		ut_a(fil_node->n_pending == 0);
		ut_a(!fil_node->being_extended);
		ut_a(fil_node->open);
		ut_a(fil_space_belongs_in_lru(fil_node->space));
	}

	mutex_exit(&fil_system->mutex);

	return(TRUE);
}

/********************************************************************//**
Returns TRUE if file address is undefined.
@return	TRUE if undefined */
UNIV_INTERN
ibool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr)	/*!< in: address */
{
	return(addr.page == FIL_NULL);
}

/********************************************************************//**
Get the predecessor of a file page.
@return	FIL_PAGE_PREV */
UNIV_INTERN
ulint
fil_page_get_prev(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

/********************************************************************//**
Get the successor of a file page.
@return	FIL_PAGE_NEXT */
UNIV_INTERN
ulint
fil_page_get_next(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_NEXT));
}

/*********************************************************************//**
Sets the file page type. */
UNIV_INTERN
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type)	/*!< in: type */
{
	ut_ad(page);

	mach_write_to_2(page + FIL_PAGE_TYPE, type);
}

/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */
UNIV_INTERN
ulint
fil_page_get_type(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	ut_ad(page);

	return(mach_read_from_2(page + FIL_PAGE_TYPE));
}

/****************************************************************//**
Closes the tablespace memory cache. */
UNIV_INTERN
void
fil_close(void)
/*===========*/
{
	fil_space_crypt_cleanup();

	mutex_free(&fil_system->mutex);

	hash_table_free(fil_system->spaces);

	hash_table_free(fil_system->name_hash);

	ut_a(UT_LIST_GET_LEN(fil_system->LRU) == 0);
	ut_a(UT_LIST_GET_LEN(fil_system->unflushed_spaces) == 0);
	ut_a(UT_LIST_GET_LEN(fil_system->space_list) == 0);

	mem_free(fil_system);

	fil_system = NULL;
}

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */
UNIV_INTERN
void
fil_delete_file(
/*============*/
	const char*	ibd_name)	/*!< in: filepath of the ibd
					tablespace */
{
	/* Force a delete of any stale .ibd files that are lying around. */

	ib_logf(IB_LOG_LEVEL_INFO, "Deleting %s", ibd_name);

	os_file_delete_if_exists(innodb_file_data_key, ibd_name);

	char*	cfg_name = fil_make_cfg_name(ibd_name);

	os_file_delete_if_exists(innodb_file_data_key, cfg_name);

	mem_free(cfg_name);
}

/*************************************************************************
Return local hash table informations. */

ulint
fil_system_hash_cells(void)
/*=======================*/
{
       if (fil_system) {
               return (fil_system->spaces->n_cells
                       + fil_system->name_hash->n_cells);
       } else {
               return 0;
       }
}

ulint
fil_system_hash_nodes(void)
/*=======================*/
{
       if (fil_system) {
               return (UT_LIST_GET_LEN(fil_system->space_list)
                       * (sizeof(fil_space_t) + MEM_BLOCK_HEADER_SIZE));
       } else {
               return 0;
       }
}

/**
Iterate over all the spaces in the space list and fetch the
tablespace names. It will return a copy of the name that must be
freed by the caller using: delete[].
@return DB_SUCCESS if all OK. */
UNIV_INTERN
dberr_t
fil_get_space_names(
/*================*/
	space_name_list_t&	space_name_list)
				/*!< in/out: List to append to */
{
	fil_space_t*	space;
	dberr_t		err = DB_SUCCESS;

	mutex_enter(&fil_system->mutex);

	for (space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		if (space->purpose == FIL_TABLESPACE) {
			ulint	len;
			char*	name;

			len = strlen(space->name);
			name = new(std::nothrow) char[len + 1];

			if (name == 0) {
				/* Caller to free elements allocated so far. */
				err = DB_OUT_OF_MEMORY;
				break;
			}

			memcpy(name, space->name, len);
			name[len] = 0;

			space_name_list.push_back(name);
		}
	}

	mutex_exit(&fil_system->mutex);

	return(err);
}

/** Generate redo log for swapping two .ibd files
@param[in]	old_table	old table
@param[in]	new_table	new table
@param[in]	tmp_name	temporary table name
@param[in,out]	mtr		mini-transaction
@return innodb error code */
UNIV_INTERN
dberr_t
fil_mtr_rename_log(
	const dict_table_t*	old_table,
	const dict_table_t*	new_table,
	const char*		tmp_name,
	mtr_t*			mtr)
{
	dberr_t	err = DB_SUCCESS;
	char*	old_path;

	/* If neither table is file-per-table,
	there will be no renaming of files. */
	if (old_table->space == TRX_SYS_SPACE
	    && new_table->space == TRX_SYS_SPACE) {
		return(DB_SUCCESS);
	}

	if (DICT_TF_HAS_DATA_DIR(old_table->flags)) {
		old_path = os_file_make_remote_pathname(
			old_table->data_dir_path, old_table->name, "ibd");
	} else {
		old_path = fil_make_ibd_name(old_table->name, false);
	}
	if (old_path == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	if (old_table->space != TRX_SYS_SPACE) {
		char*	tmp_path;

		if (DICT_TF_HAS_DATA_DIR(old_table->flags)) {
			tmp_path = os_file_make_remote_pathname(
				old_table->data_dir_path, tmp_name, "ibd");
		}
		else {
			tmp_path = fil_make_ibd_name(tmp_name, false);
		}

		if (tmp_path == NULL) {
			mem_free(old_path);
			return(DB_OUT_OF_MEMORY);
		}

		/* Temp filepath must not exist. */
		err = fil_rename_tablespace_check(
			old_table->space, old_path, tmp_path,
			dict_table_is_discarded(old_table));
		mem_free(tmp_path);
		if (err != DB_SUCCESS) {
			mem_free(old_path);
			return(err);
		}

		fil_op_write_log(MLOG_FILE_RENAME, old_table->space,
				 0, 0, old_table->name, tmp_name, mtr);
	}

	if (new_table->space != TRX_SYS_SPACE) {

		/* Destination filepath must not exist unless this ALTER
		TABLE starts and ends with a file_per-table tablespace. */
		if (old_table->space == TRX_SYS_SPACE) {
			char*	new_path = NULL;

			if (DICT_TF_HAS_DATA_DIR(new_table->flags)) {
				new_path = os_file_make_remote_pathname(
					new_table->data_dir_path,
					new_table->name, "ibd");
			}
			else {
				new_path = fil_make_ibd_name(
					new_table->name, false);
			}

			if (new_path == NULL) {
				mem_free(old_path);
				return(DB_OUT_OF_MEMORY);
			}

			err = fil_rename_tablespace_check(
				new_table->space, new_path, old_path,
				dict_table_is_discarded(new_table));
			mem_free(new_path);
			if (err != DB_SUCCESS) {
				mem_free(old_path);
				return(err);
			}
		}

		fil_op_write_log(MLOG_FILE_RENAME, new_table->space,
				 0, 0, new_table->name, old_table->name, mtr);

	}

	mem_free(old_path);

	return(err);
}

/*************************************************************************
functions to access is_corrupt flag of fil_space_t*/

void
fil_space_set_corrupt(
/*==================*/
	ulint	space_id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(space_id);

	if (space) {
		space->is_corrupt = true;
	}

	mutex_exit(&fil_system->mutex);
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
			ib_logf(IB_LOG_LEVEL_WARN, "Trying to access missing"
				" tablespace " ULINTPF ".", id);
		}
	} else if (space->is_stopping()) {
		space = NULL;
	} else {
		space->n_pending_ops++;
	}

	mutex_exit(&fil_system->mutex);

	return(space);
}

/** Acquire a tablespace for reading or writing a block,
when it could be dropped concurrently.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing */
UNIV_INTERN
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
UNIV_INTERN
void
fil_space_release_for_io(fil_space_t* space)
{
	mutex_enter(&fil_system->mutex);
	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_ad(space->n_pending_ios > 0);
	space->n_pending_ios--;
	mutex_exit(&fil_system->mutex);
}

/** Release a tablespace acquired with fil_space_acquire().
@param[in,out]	space	tablespace to release  */
UNIV_INTERN
void
fil_space_release(fil_space_t* space)
{
	mutex_enter(&fil_system->mutex);
	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_ad(space->n_pending_ops > 0);
	space->n_pending_ops--;
	mutex_exit(&fil_system->mutex);
}
