/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2017, MariaDB Corporation.

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
@file include/fil0fil.h
The low-level file system

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#ifndef fil0fil_h
#define fil0fil_h
#include "univ.i"

struct fil_space_t;

#ifndef UNIV_INNOCHECKSUM

#include "log0recv.h"
#include "dict0types.h"
#include "page0size.h"
#include "ibuf0types.h"

#include <list>
#include <vector>

// Forward declaration
struct trx_t;
class page_id_t;
class truncate_t;

typedef std::list<char*, ut_allocator<char*> >	space_name_list_t;

/** Structure containing encryption specification */
struct fil_space_crypt_t;

/** File types */
enum fil_type_t {
	/** temporary tablespace (temporary undo log or tables) */
	FIL_TYPE_TEMPORARY,
	/** a tablespace that is being imported (no logging until finished) */
	FIL_TYPE_IMPORT,
	/** persistent tablespace (for system, undo log or tables) */
	FIL_TYPE_TABLESPACE,
	/** redo log covering changes to files of FIL_TYPE_TABLESPACE */
	FIL_TYPE_LOG
};

/** Check if fil_type is any of FIL_TYPE_TEMPORARY, FIL_TYPE_IMPORT
or FIL_TYPE_TABLESPACE.
@param[in]	type	variable of type fil_type_t
@return true if any of FIL_TYPE_TEMPORARY, FIL_TYPE_IMPORT
or FIL_TYPE_TABLESPACE */
inline
bool
fil_type_is_data(
	fil_type_t	type)
{
	return(type == FIL_TYPE_TEMPORARY
	       || type == FIL_TYPE_IMPORT
	       || type == FIL_TYPE_TABLESPACE);
}

struct fil_node_t;

/** Tablespace or log data space */
struct fil_space_t {
	char*		name;	/*!< Tablespace name */
	ulint		id;	/*!< space id */
	lsn_t		max_lsn;
				/*!< LSN of the most recent
				fil_names_write_if_was_clean().
				Reset to 0 by fil_names_clear().
				Protected by log_sys->mutex.
				If and only if this is nonzero, the
				tablespace will be in named_spaces. */
	bool		stop_ios;/*!< true if we want to rename the
				.ibd file of tablespace and want to
				stop temporarily posting of new i/o
				requests on the file */
	bool		stop_new_ops;
				/*!< we set this true when we start
				deleting a single-table tablespace.
				When this is set following new ops
				are not allowed:
				* read IO request
				* ibuf merge
				* file flush
				Note that we can still possibly have
				new write operations because we don't
				check this flag when doing flush
				batches. */
	bool		is_being_truncated;
				/*!< this is set to true when we prepare to
				truncate a single-table tablespace and its
				.ibd file */
#ifdef UNIV_DEBUG
	ulint		redo_skipped_count;
				/*!< reference count for operations who want
				to skip redo log in the file space in order
				to make fsp_space_modify_check pass. */
#endif
	fil_type_t	purpose;/*!< purpose */
	UT_LIST_BASE_NODE_T(fil_node_t) chain;
				/*!< base node for the file chain */
	ulint		size;	/*!< tablespace file size in pages;
				0 if not known yet */
	ulint		size_in_header;
				/* FSP_SIZE in the tablespace header;
				0 if not known yet */
	ulint		free_len;
				/*!< length of the FSP_FREE list */
	ulint		free_limit;
				/*!< contents of FSP_FREE_LIMIT */
	ulint		recv_size;
				/*!< recovered tablespace size in pages;
				0 if no size change was read from the redo log,
				or if the size change was implemented */
	ulint		flags;	/*!< FSP_SPACE_FLAGS and FSP_FLAGS_MEM_ flags;
				see fsp0types.h,
				fsp_flags_is_valid(),
				page_size_t(ulint) (constructor) */
	ulint		n_reserved_extents;
				/*!< number of reserved free extents for
				ongoing operations like B-tree page split */
	ulint		n_pending_flushes; /*!< this is positive when flushing
				the tablespace to disk; dropping of the
				tablespace is forbidden if this is positive */
	/** Number of pending buffer pool operations accessing the tablespace
	without holding a table lock or dict_operation_lock S-latch
	that would prevent the table (and tablespace) from being
	dropped. An example is change buffer merge.
	The tablespace cannot be dropped while this is nonzero,
	or while fil_node_t::n_pending is nonzero.
	Protected by fil_system->mutex. */
	ulint		n_pending_ops;
	/** Number of pending block read or write operations
	(when a write is imminent or a read has recently completed).
	The tablespace object cannot be freed while this is nonzero,
	but it can be detached from fil_system.
	Note that fil_node_t::n_pending tracks actual pending I/O requests.
	Protected by fil_system->mutex. */
	ulint		n_pending_ios;
	hash_node_t	hash;	/*!< hash chain node */
	hash_node_t	name_hash;/*!< hash chain the name_hash table */
	rw_lock_t	latch;	/*!< latch protecting the file space storage
				allocation */
	UT_LIST_NODE_T(fil_space_t) unflushed_spaces;
				/*!< list of spaces with at least one unflushed
				file we have written to */
	UT_LIST_NODE_T(fil_space_t) named_spaces;
				/*!< list of spaces for which MLOG_FILE_NAME
				records have been issued */
	bool		is_in_unflushed_spaces;
				/*!< true if this space is currently in
				unflushed_spaces */
	UT_LIST_NODE_T(fil_space_t) space_list;
				/*!< list of all spaces */
	/** other tablespaces needing key rotation */
	UT_LIST_NODE_T(fil_space_t) rotation_list;
	/** whether this tablespace needs key rotation */
	bool		is_in_rotation_list;

	/** MariaDB encryption data */
	fil_space_crypt_t* crypt_data;

	/** True if we have already printed compression failure */
	bool		printed_compression_failure;

	/** True if the device this filespace is on supports atomic writes */
	bool		atomic_write_supported;

	/** Release the reserved free extents.
	@param[in]	n_reserved	number of reserved extents */
	void release_free_extents(ulint n_reserved);

	/** True if file system storing this tablespace supports
	punch hole */
	bool		punch_hole;

	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */

	/** @return whether the tablespace is about to be dropped or
	truncated */
	bool is_stopping() const
	{
		return stop_new_ops || is_being_truncated;
	}
};

/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** File node of a tablespace or the log data space */
struct fil_node_t {
	/** tablespace containing this file */
	fil_space_t*	space;
	/** file name; protected by fil_system->mutex and log_sys->mutex. */
	char*		name;
	/** file handle (valid if is_open) */
	pfs_os_file_t	handle;
	/** event that groups and serializes calls to fsync;
	os_event_set() and os_event_reset() are protected by
	fil_system_t::mutex */
	os_event_t	sync_event;
	/** whether the file actually is a raw device or disk partition */
	bool		is_raw_disk;
	/** size of the file in database pages (0 if not known yet);
	the possible last incomplete megabyte may be ignored
	if space->id == 0 */
	ulint		size;
	/** initial size of the file in database pages;
	FIL_IBD_FILE_INITIAL_SIZE by default */
	ulint		init_size;
	/** maximum size of the file in database pages (0 if unlimited) */
	ulint		max_size;
	/** count of pending i/o's; is_open must be true if nonzero */
	ulint		n_pending;
	/** count of pending flushes; is_open must be true if nonzero */
	ulint		n_pending_flushes;
	/** whether the file is currently being extended */
	bool		being_extended;
	/** number of writes to the file since the system was started */
	int64_t		modification_counter;
	/** the modification_counter of the latest flush to disk */
	int64_t		flush_counter;
	/** link to other files in this tablespace */
	UT_LIST_NODE_T(fil_node_t) chain;
	/** link to the fil_system->LRU list (keeping track of open files) */
	UT_LIST_NODE_T(fil_node_t) LRU;

	/** whether this file could use atomic write (data file) */
	bool		atomic_write;

	/** Filesystem block size */
	ulint		block_size;

	/** FIL_NODE_MAGIC_N */
	ulint		magic_n;

	/** @return whether this file is open */
	bool is_open() const
	{
		return(handle != OS_FILE_CLOSED);
	}
};

/** Value of fil_node_t::magic_n */
#define	FIL_NODE_MAGIC_N	89389

/** Common InnoDB file extentions */
enum ib_extention {
	NO_EXT = 0,
	IBD = 1,
	ISL = 2,
	CFG = 3
};
extern const char* dot_ext[];
#define DOT_IBD dot_ext[IBD]
#define DOT_ISL dot_ext[ISL]
#define DOT_CFG dot_ext[CFG]

/** When mysqld is run, the default directory "." is the mysqld datadir,
but in the MySQL Embedded Server Library and mysqlbackup it is not the default
directory, and we must set the base file path explicitly */
extern const char*	fil_path_to_mysql_datadir;

/** Initial size of a single-table tablespace in pages */
#define FIL_IBD_FILE_INITIAL_SIZE	4

/** 'null' (undefined) page offset in the context of file spaces */
#define	FIL_NULL	ULINT32_UNDEFINED

/* Space address data type; this is intended to be used when
addresses accurate to a byte are stored in file pages. If the page part
of the address is FIL_NULL, the address is considered undefined. */

typedef	byte	fil_faddr_t;	/*!< 'type' definition in C: an address
				stored in a file page is a string of bytes */

#define FIL_ADDR_PAGE	0	/* first in address is the page offset */
#define	FIL_ADDR_BYTE	4	/* then comes 2-byte byte offset within page*/
#endif /* !UNIV_INNOCHECKSUM */
#define	FIL_ADDR_SIZE	6	/* address size is 6 bytes */

#ifndef UNIV_INNOCHECKSUM

/** File space address */
struct fil_addr_t {
	ulint	page;		/*!< page number within a space */
	ulint	boffset;	/*!< byte offset within the page */
};

/** The null file address */
extern fil_addr_t	fil_addr_null;

#endif /* !UNIV_INNOCHECKSUM */

/** The byte offsets on a file page for various variables @{ */
#define FIL_PAGE_SPACE_OR_CHKSUM 0	/*!< in < MySQL-4.0.14 space id the
					page belongs to (== 0) but in later
					versions the 'new' checksum of the
					page */
#define FIL_PAGE_OFFSET		4	/*!< page offset inside space */
#define FIL_PAGE_PREV		8	/*!< if there is a 'natural'
					predecessor of the page, its
					offset.  Otherwise FIL_NULL.
					This field is not set on BLOB
					pages, which are stored as a
					singly-linked list.  See also
					FIL_PAGE_NEXT. */
#define FIL_PAGE_NEXT		12	/*!< if there is a 'natural' successor
					of the page, its offset.
					Otherwise FIL_NULL.
					B-tree index pages
					(FIL_PAGE_TYPE contains FIL_PAGE_INDEX)
					on the same PAGE_LEVEL are maintained
					as a doubly linked list via
					FIL_PAGE_PREV and FIL_PAGE_NEXT
					in the collation order of the
					smallest user record on each page. */
#define FIL_PAGE_LSN		16	/*!< lsn of the end of the newest
					modification log record to the page */
#define	FIL_PAGE_TYPE		24	/*!< file page type: FIL_PAGE_INDEX,...,
					2 bytes.

					The contents of this field can only
					be trusted in the following case:
					if the page is an uncompressed
					B-tree index page, then it is
					guaranteed that the value is
					FIL_PAGE_INDEX.
					The opposite does not hold.

					In tablespaces created by
					MySQL/InnoDB 5.1.7 or later, the
					contents of this field is valid
					for all uncompressed pages. */
#define FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION 26 /*!< for the first page
					in a system tablespace data file
					(ibdata*, not *.ibd): the file has
					been flushed to disk at least up
					to this lsn
					for other pages: a 32-bit key version
					used to encrypt the page + 32-bit checksum
					or 64 bits of zero if no encryption
					*/

/** This overloads FIL_PAGE_FILE_FLUSH_LSN for RTREE Split Sequence Number */
#define	FIL_RTREE_SPLIT_SEQ_NUM	FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION

/** starting from 4.1.x this contains the space id of the page */
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID  34

#define FIL_PAGE_SPACE_ID  FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID

#define FIL_PAGE_DATA		38U	/*!< start of the data on the page */

/* Following are used when page compression is used */
#define FIL_PAGE_COMPRESSED_SIZE 2      /*!< Number of bytes used to store
					actual payload data size on
					compressed pages. */
#define FIL_PAGE_COMPRESSION_METHOD_SIZE 2
					/*!< Number of bytes used to store
					actual compression method. */
/* @} */
/** File page trailer @{ */
#define FIL_PAGE_END_LSN_OLD_CHKSUM 8	/*!< the low 4 bytes of this are used
					to store the page checksum, the
					last 4 bytes should be identical
					to the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_DATA_END	8	/*!< size of the page trailer */
/* @} */

/** File page types (values of FIL_PAGE_TYPE) @{ */
#define FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED 37401 /*!< Page is compressed and
						 then encrypted */
#define FIL_PAGE_PAGE_COMPRESSED 34354  /*!< page compressed page */
#define FIL_PAGE_INDEX		17855	/*!< B-tree node */
#define FIL_PAGE_RTREE		17854	/*!< B-tree node */
#define FIL_PAGE_UNDO_LOG	2	/*!< Undo log page */
#define FIL_PAGE_INODE		3	/*!< Index node */
#define FIL_PAGE_IBUF_FREE_LIST	4	/*!< Insert buffer free list */
/* File page types introduced in MySQL/InnoDB 5.1.7 */
#define FIL_PAGE_TYPE_ALLOCATED	0	/*!< Freshly allocated page */
#define FIL_PAGE_IBUF_BITMAP	5	/*!< Insert buffer bitmap */
#define FIL_PAGE_TYPE_SYS	6	/*!< System page */
#define FIL_PAGE_TYPE_TRX_SYS	7	/*!< Transaction system data */
#define FIL_PAGE_TYPE_FSP_HDR	8	/*!< File space header */
#define FIL_PAGE_TYPE_XDES	9	/*!< Extent descriptor page */
#define FIL_PAGE_TYPE_BLOB	10	/*!< Uncompressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB	11	/*!< First compressed BLOB page */
#define FIL_PAGE_TYPE_ZBLOB2	12	/*!< Subsequent compressed BLOB page */
#define FIL_PAGE_TYPE_UNKNOWN	13	/*!< In old tablespaces, garbage
					in FIL_PAGE_TYPE is replaced with this
					value when flushing pages. */

/* File page types introduced in MySQL 5.7, not supported in MariaDB */
//#define FIL_PAGE_COMPRESSED	14
//#define FIL_PAGE_ENCRYPTED	15
//#define FIL_PAGE_COMPRESSED_AND_ENCRYPTED 16
//#define FIL_PAGE_ENCRYPTED_RTREE 17

/** Used by i_s.cc to index into the text description. */
#define FIL_PAGE_TYPE_LAST	FIL_PAGE_TYPE_UNKNOWN
					/*!< Last page type */
/* @} */

/** macro to check whether the page type is index (Btree or Rtree) type */
#define fil_page_type_is_index(page_type)                          \
        (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_RTREE)

/** Check whether the page is index page (either regular Btree index or Rtree
index */
#define fil_page_index_page_check(page)                         \
        fil_page_type_is_index(fil_page_get_type(page))

#ifndef UNIV_INNOCHECKSUM

/** Enum values for encryption table option */
enum fil_encryption_t {
	/** Encrypted if innodb_encrypt_tables=ON (srv_encrypt_tables) */
	FIL_ENCRYPTION_DEFAULT,
	/** Encrypted */
	FIL_ENCRYPTION_ON,
	/** Not encrypted */
	FIL_ENCRYPTION_OFF
};

/** The number of fsyncs done to the log */
extern ulint	fil_n_log_flushes;

/** Number of pending redo log flushes */
extern ulint	fil_n_pending_log_flushes;
/** Number of pending tablespace flushes */
extern ulint	fil_n_pending_tablespace_flushes;

/** Number of files currently open */
extern ulint	fil_n_file_opened;

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
	MY_ATTRIBUTE((warn_unused_result));

/** The tablespace memory cache; also the totality of logs (the log
data space) is stored here; below we talk about tablespaces, but also
the ib_logfiles form a 'space' and it is handled here */
struct fil_system_t {
	ib_mutex_t	mutex;		/*!< The mutex protecting the cache */
	hash_table_t*	spaces;		/*!< The hash table of spaces in the
					system; they are hashed on the space
					id */
	hash_table_t*	name_hash;	/*!< hash table based on the space
					name */
	UT_LIST_BASE_NODE_T(fil_node_t) LRU;
					/*!< base node for the LRU list of the
					most recently used open files with no
					pending i/o's; if we start an i/o on
					the file, we first remove it from this
					list, and return it to the start of
					the list when the i/o ends;
					log files and the system tablespace are
					not put to this list: they are opened
					after the startup, and kept open until
					shutdown */
	UT_LIST_BASE_NODE_T(fil_space_t) unflushed_spaces;
					/*!< base node for the list of those
					tablespaces whose files contain
					unflushed writes; those spaces have
					at least one file node where
					modification_counter > flush_counter */
	ulint		n_open;		/*!< number of files currently open */
	ulint		max_n_open;	/*!< n_open is not allowed to exceed
					this */
	int64_t		modification_counter;/*!< when we write to a file we
					increment this by one */
	ulint		max_assigned_id;/*!< maximum space id in the existing
					tables, or assigned during the time
					mysqld has been up; at an InnoDB
					startup we scan the data dictionary
					and set here the maximum of the
					space id's of the tables there */
	UT_LIST_BASE_NODE_T(fil_space_t) space_list;
					/*!< list of all file spaces */
	UT_LIST_BASE_NODE_T(fil_space_t) named_spaces;
					/*!< list of all file spaces
					for which a MLOG_FILE_NAME
					record has been written since
					the latest redo log checkpoint.
					Protected only by log_sys->mutex. */
	UT_LIST_BASE_NODE_T(fil_space_t) rotation_list;
					/*!< list of all file spaces needing
					key rotation.*/

	ibool		space_id_reuse_warned;
					/* !< TRUE if fil_space_create()
					has issued a warning about
					potential space_id reuse */
};

/** The tablespace memory cache. This variable is NULL before the module is
initialized. */
extern fil_system_t*	fil_system;

#include "fil0crypt.h"

/** Returns the latch of a file space.
@param[in]	id	space id
@param[out]	flags	tablespace flags
@return latch protecting storage allocation */
rw_lock_t*
fil_space_get_latch(
	ulint	id,
	ulint*	flags);

/** Gets the type of a file space.
@param[in]	id	tablespace identifier
@return file type */
fil_type_t
fil_space_get_type(
	ulint	id);

/** Note that a tablespace has been imported.
It is initially marked as FIL_TYPE_IMPORT so that no logging is
done during the import process when the space ID is stamped to each page.
Now we change it to FIL_SPACE_TABLESPACE to start redo and undo logging.
NOTE: temporary tablespaces are never imported.
@param[in]	id	tablespace identifier */
void
fil_space_set_imported(
	ulint	id);

/** Append a file to the chain of files of a space.
@param[in]	name		file name of a file that is not open
@param[in]	size		file size in entire database blocks
@param[in,out]	space		tablespace from fil_space_create()
@param[in]	is_raw		whether this is a raw device or partition
@param[in]	atomic_write	true if atomic write could be enabled
@param[in]	max_pages	maximum number of pages in file,
ULINT_MAX means the file size is unlimited.
@return pointer to the file name
@retval NULL if error */
char*
fil_node_create(
	const char*	name,
	ulint		size,
	fil_space_t*	space,
	bool		is_raw,
	bool		atomic_write,
	ulint		max_pages = ULINT_MAX)
	MY_ATTRIBUTE((warn_unused_result));

/** Create a space memory object and put it to the fil_system hash table.
Error messages are issued to the server log.
@param[in]	name		tablespace name
@param[in]	id		tablespace identifier
@param[in]	flags		tablespace flags
@param[in]	purpose		tablespace purpose
@param[in,out]	crypt_data	encryption information
@param[in]	mode		encryption mode
@return pointer to created tablespace, to be filled in with fil_node_create()
@retval NULL on failure (such as when the same tablespace exists) */
fil_space_t*
fil_space_create(
	const char*		name,
	ulint			id,
	ulint			flags,
	fil_type_t		purpose,
	fil_space_crypt_t*	crypt_data,
	fil_encryption_t	mode = FIL_ENCRYPTION_DEFAULT)
	MY_ATTRIBUTE((warn_unused_result));

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */
bool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id);	/*!< in/out: space id */

/** Frees a space object from the tablespace memory cache.
Closes the files in the chain but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param[in]	id		tablespace identifier
@param[in]	x_latched	whether the caller holds X-mode space->latch
@return true if success */
bool
fil_space_free(
	ulint		id,
	bool		x_latched);

/** Returns the path from the first fil_node_t found with this space ID.
The caller is responsible for freeing the memory allocated here for the
value returned.
@param[in]	id	Tablespace ID
@return own: A copy of fil_node_t::path, NULL if space ID is zero
or not found. */
char*
fil_space_get_first_path(
	ulint		id);

/** Set the recovered size of a tablespace in pages.
@param id	tablespace ID
@param size	recovered size in pages */
UNIV_INTERN
void
fil_space_set_recv_size(ulint id, ulint size);
/*******************************************************************//**
Returns the size of the space in pages. The tablespace must be cached in the
memory cache.
@return space size, 0 if space not found */
ulint
fil_space_get_size(
/*===============*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the flags of the space. The tablespace must be cached
in the memory cache.
@return flags, ULINT_UNDEFINED if space not found */
ulint
fil_space_get_flags(
/*================*/
	ulint	id);	/*!< in: space id */

/** Open each fil_node_t of a named fil_space_t if not already open.
@param[in]	name	Tablespace name
@return true if all file nodes are opened. */
bool
fil_space_open(
	const char*	name);

/** Close each fil_node_t of a named fil_space_t if open.
@param[in]	name	Tablespace name */
void
fil_space_close(
	const char*	name);

/** Returns the page size of the space and whether it is compressed or not.
The tablespace must be cached in the memory cache.
@param[in]	id	space id
@param[out]	found	true if tablespace was found
@return page size */
const page_size_t
fil_space_get_page_size(
	ulint	id,
	bool*	found);

/****************************************************************//**
Initializes the tablespace memory cache. */
void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open);	/*!< in: max number of open files */
/*******************************************************************//**
Initializes the tablespace memory cache. */
void
fil_close(void);
/*===========*/
/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */
void
fil_open_log_and_system_tablespace_files(void);
/*==========================================*/
/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
void
fil_close_all_files(void);
/*=====================*/
/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */
void
fil_close_log_files(
/*================*/
	bool	free);	/*!< in: whether to free the memory object */
/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id);/*!< in: maximum known id */

/** Write the flushed LSN to the page header of the first page in the
system tablespace.
@param[in]	lsn	flushed LSN
@return DB_SUCCESS or error number */
dberr_t
fil_write_flushed_lsn(
	lsn_t	lsn)
MY_ATTRIBUTE((warn_unused_result));

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
	MY_ATTRIBUTE((warn_unused_result));

/** Acquire a tablespace when it could be dropped concurrently.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing or being deleted or truncated */
inline
fil_space_t*
fil_space_acquire(ulint id)
{
	return (fil_space_acquire_low(id, false));
}

/** Acquire a tablespace that may not exist.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing or being deleted */
inline
fil_space_t*
fil_space_acquire_silent(ulint id)
{
	return (fil_space_acquire_low(id, true));
}

/** Release a tablespace acquired with fil_space_acquire().
@param[in,out]	space	tablespace to release  */
void
fil_space_release(fil_space_t* space);

/** Acquire a tablespace for reading or writing a block,
when it could be dropped concurrently.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing */
fil_space_t*
fil_space_acquire_for_io(ulint id);

/** Release a tablespace acquired with fil_space_acquire_for_io().
@param[in,out]	space	tablespace to release  */
void
fil_space_release_for_io(fil_space_t* space);

/** Return the next fil_space_t.
Once started, the caller must keep calling this until it returns NULL.
fil_space_acquire() and fil_space_release() are invoked here which
blocks a concurrent operation from dropping the tablespace.
@param[in,out]	prev_space	Pointer to the previous fil_space_t.
If NULL, use the first fil_space_t on fil_system->space_list.
@return pointer to the next fil_space_t.
@retval NULL if this was the last  */
fil_space_t*
fil_space_next(
	fil_space_t*	prev_space)
	MY_ATTRIBUTE((warn_unused_result));

/** Return the next fil_space_t from key rotation list.
Once started, the caller must keep calling this until it returns NULL.
fil_space_acquire() and fil_space_release() are invoked here which
blocks a concurrent operation from dropping the tablespace.
@param[in,out]	prev_space	Pointer to the previous fil_space_t.
If NULL, use the first fil_space_t on fil_system->space_list.
@return pointer to the next fil_space_t.
@retval NULL if this was the last*/
fil_space_t*
fil_space_keyrotate_next(
	fil_space_t*	prev_space)
	MY_ATTRIBUTE((warn_unused_result));

/** Wrapper with reference-counting for a fil_space_t. */
class FilSpace
{
public:
	/** Default constructor: Use this when reference counting
	is done outside this wrapper. */
	FilSpace() : m_space(NULL) {}

	/** Constructor: Look up the tablespace and increment the
	reference count if found.
	@param[in]	space_id	tablespace ID
	@param[in]	silent		whether not to display errors */
	explicit FilSpace(ulint space_id, bool silent = false)
		: m_space(fil_space_acquire_low(space_id, silent)) {}

	/** Assignment operator: This assumes that fil_space_acquire()
	has already been done for the fil_space_t. The caller must
	assign NULL if it calls fil_space_release().
	@param[in]	space	tablespace to assign */
	class FilSpace& operator=(fil_space_t* space)
	{
		/* fil_space_acquire() must have been invoked. */
		ut_ad(space == NULL || space->n_pending_ops > 0);
		m_space = space;
		return(*this);
	}

	/** Destructor - Decrement the reference count if a fil_space_t
	is still assigned. */
	~FilSpace()
	{
		if (m_space != NULL) {
			fil_space_release(m_space);
		}
	}

	/** Implicit type conversion
	@return the wrapped object */
	operator const fil_space_t*() const
	{
		return(m_space);
	}

	/** Explicit type conversion
	@return the wrapped object */
	const fil_space_t* operator()() const
	{
		return(m_space);
	}

private:
	/** The wrapped pointer */
	fil_space_t*	m_space;
};

/********************************************************//**
Creates the database directory for a table if it does not exist yet. */
void
fil_create_directory_for_tablename(
/*===============================*/
	const char*	name);	/*!< in: name in the standard
				'databasename/tablename' format */
/********************************************************//**
Recreates table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
dberr_t
fil_recreate_table(
/*===============*/
	ulint			space_id,	/*!< in: space id */
	ulint			format_flags,	/*!< in: page format */
	ulint			flags,		/*!< in: tablespace flags */
	const char*		name,		/*!< in: table name */
	truncate_t&		truncate);	/*!< in/out: The information of
						TRUNCATE log record */
/********************************************************//**
Recreates the tablespace and table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */
dberr_t
fil_recreate_tablespace(
/*====================*/
	ulint			space_id,	/*!< in: space id */
	ulint			format_flags,	/*!< in: page format */
	ulint			flags,		/*!< in: tablespace flags */
	const char*		name,		/*!< in: table name */
	truncate_t&		truncate,	/*!< in/out: The information of
						TRUNCATE log record */
	lsn_t			recv_lsn);	/*!< in: the end LSN of
						the log record */
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
	MY_ATTRIBUTE((warn_unused_result));

/** Determine whether a table can be accessed in operations that are
not (necessarily) protected by meta-data locks.
(Rollback would generally be protected, but rollback of
FOREIGN KEY CASCADE/SET NULL is not protected by meta-data locks
but only by InnoDB table locks, which may be broken by TRUNCATE TABLE.)
@param[in]	table	persistent table
checked @return whether the table is accessible */
bool
fil_table_accessible(const dict_table_t* table)
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Deletes an IBD tablespace, either general or single-table.
The tablespace must be cached in the memory cache. This will delete the
datafile, fil_space_t & fil_node_t entries from the file_system_t cache.
@param[in]	space_id	Tablespace id
@param[in]	buf_remove	Specify the action to take on the pages
for this table in the buffer pool.
@return true if success */
dberr_t
fil_delete_tablespace(
	ulint		id,
	buf_remove_t	buf_remove);

/** Truncate the tablespace to needed size.
@param[in]	space_id	id of tablespace to truncate
@param[in]	size_in_pages	truncate size.
@return true if truncate was successful. */
bool
fil_truncate_tablespace(
	ulint		space_id,
	ulint		size_in_pages);

/*******************************************************************//**
Prepare for truncating a single-table tablespace. The tablespace
must be cached in the memory cache.
1) Check pending operations on a tablespace;
2) Remove all insert buffer entries for the tablespace;
@return DB_SUCCESS or error */
dberr_t
fil_prepare_for_truncate(
/*=====================*/
	ulint	id);			/*!< in: space id */

/** Reinitialize the original tablespace header with the same space id
for single tablespace
@param[in]	id		space id of the tablespace
@param[in]	size            size in blocks
@param[in]	trx		Transaction covering truncate */
void
fil_reinit_space_header(
	ulint		id,
	ulint		size,
	trx_t*		trx);

/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return DB_SUCCESS or error */
dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*	trx,	/*!< in/out: Transaction covering the close */
	ulint	id);	/*!< in: space id */

/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. When the user does IMPORT TABLESPACE, the tablespace will have the
    same id as it originally had.

 4. Free all the pages in use by the tablespace if rename=true.
@return DB_SUCCESS or error */
dberr_t
fil_discard_tablespace(
/*===================*/
	ulint	id)	/*!< in: space id */
	MY_ATTRIBUTE((warn_unused_result));

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
	bool		is_discarded);

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
	const char*	new_path_in);

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the direcory path or the full path and filename.
@param[in] name NULL if path is full, or Table/Tablespace name
@param[in] suffix NULL or the file extention to use.
@return own: file name */
char*
fil_make_filepath(
	const char*	path,
	const char*	name,
	ib_extention	suffix,
	bool		strip_name);

/** Create a tablespace file.
@param[in]	space_id	Tablespace ID
@param[in]	name		Tablespace name in dbname/tablename format.
@param[in]	path		Path and filename of the datafile to create.
@param[in]	flags		Tablespace flags
@param[in]	size		Initial size of the tablespace file in pages,
must be >= FIL_IBD_FILE_INITIAL_SIZE
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
	MY_ATTRIBUTE((nonnull(2), warn_unused_result));

/** Try to adjust FSP_SPACE_FLAGS if they differ from the expectations.
(Typically when upgrading from MariaDB 10.1.0..10.1.20.)
@param[in]	space_id	tablespace ID
@param[in]	flags		desired tablespace flags */
UNIV_INTERN
void
fsp_flags_try_adjust(ulint space_id, ulint flags);

/********************************************************************//**
Tries to open a single-table tablespace and optionally checks the space id is
right in it. If does not succeed, prints an error message to the .err log. This
function is used to open a tablespace when we start up mysqld, and also in
IMPORT TABLESPACE.
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
	const char*	tablename,
	const char*	path_in)
	MY_ATTRIBUTE((warn_unused_result));

enum fil_load_status {
	/** The tablespace file(s) were found and valid. */
	FIL_LOAD_OK,
	/** The name no longer matches space_id */
	FIL_LOAD_ID_CHANGED,
	/** The file(s) were not found */
	FIL_LOAD_NOT_FOUND,
	/** The file(s) were not valid */
	FIL_LOAD_INVALID
};

/** Open a single-file tablespace and add it to the InnoDB data structures.
@param[in]	space_id	tablespace ID
@param[in]	filename	path/to/databasename/tablename.ibd
@param[out]	space		the tablespace, or NULL on error
@return status of the operation */
enum fil_load_status
fil_ibd_load(
	ulint		space_id,
	const char*	filename,
	fil_space_t*&	space)
	MY_ATTRIBUTE((warn_unused_result));


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
	os_file_stat_t*	info);	/*!< in/out: buffer where the
				info is returned */
/*******************************************************************//**
Returns true if a matching tablespace exists in the InnoDB tablespace memory
cache. Note that if we have not done a crash recovery at the database startup,
there may be many tablespaces which are not yet in the memory cache.
@return true if a matching tablespace exists in the memory cache */
bool
fil_space_for_table_exists_in_mem(
/*==============================*/
	ulint		id,		/*!< in: space id */
	const char*	name,		/*!< in: table name in the standard
					'databasename/tablename' format */
	bool		print_error_if_does_not_exist,
					/*!< in: print detailed error
					information to the .err log if a
					matching tablespace is not found from
					memory */
	bool		adjust_space,	/*!< in: whether to adjust space id
					when find table space mismatch */
	mem_heap_t*	heap,		/*!< in: heap memory */
	table_id_t	table_id,	/*!< in: table id */
	ulint		table_flags);	/*!< in: table flags */

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool
fil_space_extend(
	fil_space_t*	space,
	ulint		size);
/*******************************************************************//**
Tries to reserve free extents in a file space.
@return true if succeed */
bool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve);	/*!< in: how many one wants to reserve */
/*******************************************************************//**
Releases free extents in a file space. */
void
fil_space_release_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_reserved);	/*!< in: how many one reserved */
/*******************************************************************//**
Gets the number of reserved extents. If the database is silent, this number
should be zero. */
ulint
fil_space_get_n_reserved_extents(
/*=============================*/
	ulint	id);		/*!< in: space id */

/** Reads or writes data. This operation could be asynchronous (aio).

@param[in]	type		IO context
@param[in]	sync		true if synchronous aio is desired
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
				must be divisible by the OS block size
@param[in]	len		how many bytes to read or write; this must
				not cross a file boundary; in aio this must
				be a block size multiple
@param[in,out]	buf		buffer where to store read data or from where
				to write; in aio this must be appropriately
				aligned
@param[in]	message		message for aio handler if non-sync aio
				used, else ignored

@return DB_SUCCESS, DB_TABLESPACE_DELETED or DB_TABLESPACE_TRUNCATED
if we are trying to do i/o on a tablespace which does not exist */
dberr_t
fil_io(
	const IORequest&	type,
	bool			sync,
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message);
/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */
void
fil_aio_wait(
/*=========*/
	ulint	segment);	/*!< in: the number of the segment in the aio
				array to wait for */
/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */
void
fil_flush(
/*======*/
	ulint	space_id);	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
/** Flush a tablespace.
@param[in,out]	space	tablespace to flush */
void
fil_flush(fil_space_t* space);

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG */
void
fil_flush_file_spaces(
	fil_type_t	purpose);
/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */
bool
fil_validate(void);
/*==============*/
/********************************************************************//**
Returns true if file address is undefined.
@return true if undefined */
bool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr);	/*!< in: address */
/********************************************************************//**
Get the predecessor of a file page.
@return FIL_PAGE_PREV */
ulint
fil_page_get_prev(
/*==============*/
	const byte*	page);	/*!< in: file page */
/********************************************************************//**
Get the successor of a file page.
@return FIL_PAGE_NEXT */
ulint
fil_page_get_next(
/*==============*/
	const byte*	page);	/*!< in: file page */
/*********************************************************************//**
Sets the file page type. */
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type);	/*!< in: type */
/** Reset the page type.
Data files created before MySQL 5.1 may contain garbage in FIL_PAGE_TYPE.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	page_id	page number
@param[in,out]	page	page with invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
void
fil_page_reset_type(
	const page_id_t&	page_id,
	byte*			page,
	ulint			type,
	mtr_t*			mtr);
/** Get the file page type.
@param[in]	page	file page
@return page type */
inline
ulint
fil_page_get_type(
	const byte*	page)
{
	return(mach_read_from_2(page + FIL_PAGE_TYPE));
}
/** Check (and if needed, reset) the page type.
Data files created before MySQL 5.1 may contain
garbage in the FIL_PAGE_TYPE field.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	page_id	page number
@param[in,out]	page	page with possibly invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
inline
void
fil_page_check_type(
	const page_id_t&	page_id,
	byte*			page,
	ulint			type,
	mtr_t*			mtr)
{
	ulint	page_type	= fil_page_get_type(page);

	if (page_type != type) {
		fil_page_reset_type(page_id, page, type, mtr);
	}
}

/** Check (and if needed, reset) the page type.
Data files created before MySQL 5.1 may contain
garbage in the FIL_PAGE_TYPE field.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in,out]	block	block with possibly invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
#define fil_block_check_type(block, type, mtr)				\
	fil_page_check_type(block->page.id, block->frame, type, mtr)

#ifdef UNIV_DEBUG
/** Increase redo skipped of a tablespace.
@param[in]	id	space id */
void
fil_space_inc_redo_skipped_count(
	ulint		id);

/** Decrease redo skipped of a tablespace.
@param[in]	id	space id */
void
fil_space_dec_redo_skipped_count(
	ulint		id);
#endif

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */
void
fil_delete_file(
/*============*/
	const char*	path);	/*!< in: filepath of the ibd tablespace */

/** Callback functor. */
struct PageCallback {

	/** Default constructor */
	PageCallback()
		:
		m_page_size(0, 0, false),
		m_filepath() UNIV_NOTHROW {}

	virtual ~PageCallback() UNIV_NOTHROW {}

	/** Called for page 0 in the tablespace file at the start.
	@param file_size size of the file in bytes
	@param block contents of the first page in the tablespace file
	@retval DB_SUCCESS or error code. */
	virtual dberr_t init(
		os_offset_t		file_size,
		const buf_block_t*	block) UNIV_NOTHROW = 0;

	/** Called for every page in the tablespace. If the page was not
	updated then its state must be set to BUF_PAGE_NOT_USED. For
	compressed tables the page descriptor memory will be at offset:
		block->frame + UNIV_PAGE_SIZE;
	@param offset physical offset within the file
	@param block block read from file, note it is not from the buffer pool
	@retval DB_SUCCESS or error code. */
	virtual dberr_t operator()(
		os_offset_t	offset,
		buf_block_t*	block) UNIV_NOTHROW = 0;

	/** Set the name of the physical file and the file handle that is used
	to open it for the file that is being iterated over.
	@param filename the name of the tablespace file
	@param file OS file handle */
	void set_file(const char* filename, pfs_os_file_t file) UNIV_NOTHROW
	{
		m_file = file;
		m_filepath = filename;
	}

	/**
	@return the space id of the tablespace */
	virtual ulint get_space_id() const UNIV_NOTHROW = 0;

	/**
	@retval the space flags of the tablespace being iterated over */
	virtual ulint get_space_flags() const UNIV_NOTHROW = 0;

	/** The compressed page size
	@return the compressed page size */
	const page_size_t& get_page_size() const
	{
		return(m_page_size);
	}

	/** The tablespace page size. */
	page_size_t		m_page_size;

	/** File handle to the tablespace */
	pfs_os_file_t		m_file;

	/** Physical file path. */
	const char*		m_filepath;

protected:
	// Disable copying
	PageCallback(const PageCallback&);
	PageCallback& operator=(const PageCallback&);
};

/********************************************************************//**
Iterate over all the pages in the tablespace.
@param table the table definiton in the server
@param n_io_buffers number of blocks to read and write together
@param callback functor that will do the page updates
@return DB_SUCCESS or error code */
dberr_t
fil_tablespace_iterate(
/*===================*/
	dict_table_t*		table,
	ulint			n_io_buffers,
	PageCallback&		callback)
	MY_ATTRIBUTE((warn_unused_result));

/********************************************************************//**
Looks for a pre-existing fil_space_t with the given tablespace ID
and, if found, returns the name and filepath in newly allocated buffers that the caller must free.
@param[in] space_id The tablespace ID to search for.
@param[out] name Name of the tablespace found.
@param[out] fileapth The filepath of the first datafile for thtablespace found.
@return true if tablespace is found, false if not. */
bool
fil_space_read_name_and_filepath(
	ulint	space_id,
	char**	name,
	char**	filepath);

/** Convert a file name to a tablespace name.
@param[in]	filename	directory/databasename/tablename.ibd
@return database/tablename string, to be freed with ut_free() */
char*
fil_path_to_space_name(
	const char*	filename);

/** Returns the space ID based on the tablespace name.
The tablespace must be found in the tablespace memory cache.
This call is made from external to this module, so the mutex is not owned.
@param[in]	tablespace	Tablespace name
@return space ID if tablespace found, ULINT_UNDEFINED if space not. */
ulint
fil_space_get_id_by_name(
	const char*	tablespace);

/**
Iterate over all the spaces in the space list and fetch the
tablespace names. It will return a copy of the name that must be
freed by the caller using: delete[].
@return DB_SUCCESS if all OK. */
dberr_t
fil_get_space_names(
/*================*/
	space_name_list_t&	space_name_list)
				/*!< in/out: Vector for collecting the names. */
	MY_ATTRIBUTE((warn_unused_result));

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
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Acquire the fil_system mutex. */
#define fil_system_enter()	mutex_enter(&fil_system->mutex)
/** Release the fil_system mutex. */
#define fil_system_exit()	mutex_exit(&fil_system->mutex)

/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
fil_space_t*
fil_space_get_by_id(
/*================*/
	ulint	id);	/*!< in: space id */

/*******************************************************************//**
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space);

/** Write MLOG_FILE_NAME records when a non-predefined persistent
tablespace was modified for the first time since the latest
fil_names_clear().
@param[in,out]	space	tablespace
@param[in,out]	mtr	mini-transaction */
void
fil_names_dirty_and_write(
	fil_space_t*	space,
	mtr_t*		mtr);

/** Write MLOG_FILE_NAME records if a persistent tablespace was modified
for the first time since the latest fil_names_clear().
@param[in,out]	space	tablespace
@param[in,out]	mtr	mini-transaction
@return whether any MLOG_FILE_NAME record was written */
inline MY_ATTRIBUTE((warn_unused_result))
bool
fil_names_write_if_was_clean(
	fil_space_t*	space,
	mtr_t*		mtr)
{
	ut_ad(log_mutex_own());

	if (space == NULL) {
		return(false);
	}

	const bool	was_clean = space->max_lsn == 0;
	ut_ad(space->max_lsn <= log_sys->lsn);
	space->max_lsn = log_sys->lsn;

	if (was_clean) {
		fil_names_dirty_and_write(space, mtr);
	}

	return(was_clean);
}

extern volatile bool	recv_recovery_on;

/** During crash recovery, open a tablespace if it had not been opened
yet, to get valid size and flags.
@param[in,out]	space	tablespace */
inline
void
fil_space_open_if_needed(
	fil_space_t*	space)
{
	ut_ad(recv_recovery_on);

	if (space->size == 0) {
		/* Initially, size and flags will be set to 0,
		until the files are opened for the first time.
		fil_space_get_size() will open the file
		and adjust the size and flags. */
#ifdef UNIV_DEBUG
		ulint		size	=
#endif /* UNIV_DEBUG */
			fil_space_get_size(space->id);
		ut_ad(size == space->size);
	}
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
	bool	do_write);

#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
void test_make_filepath();
#endif /* UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */

/** Determine the block size of the data file.
@param[in]	space		tablespace
@param[in]	offset		page number
@return	block size */
UNIV_INTERN
ulint
fil_space_get_block_size(const fil_space_t* space, unsigned offset);

#include "fil0fil.ic"
#endif /* UNIV_INNOCHECKSUM */

#endif /* fil0fil_h */
