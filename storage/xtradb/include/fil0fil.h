/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2020, MariaDB Corporation.

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
@file include/fil0fil.h
The low-level file system

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#ifndef fil0fil_h
#define fil0fil_h
#include "univ.i"

#ifndef UNIV_INNOCHECKSUM

#include "dict0types.h"
#include "ut0byte.h"
#include "os0file.h"
#include "hash0hash.h"
#ifndef UNIV_HOTBACKUP
#include "sync0rw.h"
#include "ibuf0types.h"
#include "log0log.h"
#endif /* !UNIV_HOTBACKUP */
#include "trx0types.h"

#include <list>

// Forward declaration
struct trx_t;

typedef std::list<const char*> space_name_list_t;

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

#define	FIL_ADDR_SIZE	6	/* address size is 6 bytes */

/** File space address */
struct fil_addr_t{
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
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID  34 /*!< starting from 4.1.x this
					contains the space id of the page */
#define FIL_PAGE_SPACE_ID  FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID

#define FIL_PAGE_DATA		38	/*!< start of the data on the page */
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
#define FIL_PAGE_PAGE_COMPRESSED 34354  /*!< Page compressed page */
#define FIL_PAGE_INDEX		17855	/*!< B-tree node */
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
#define FIL_PAGE_TYPE_LAST	FIL_PAGE_TYPE_ZBLOB2
					/*!< Last page type */
/* @} */

#ifndef UNIV_INNOCHECKSUM

/** Space types @{ */
#define FIL_TABLESPACE		501	/*!< tablespace */
#define FIL_LOG			502	/*!< redo log */
/* @} */

/** Structure containing encryption specification */
struct fil_space_crypt_t;

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

struct fsp_open_info {
	ibool		success;	/*!< Has the tablespace been opened? */
	const char*	check_msg;	/*!< fil_check_first_page() message */
	ibool		valid;		/*!< Is the tablespace valid? */
	pfs_os_file_t	file;		/*!< File handle */
	char*		filepath;	/*!< File path to open */
	ulint		id;		/*!< Space ID */
	ulint		flags;		/*!< Tablespace flags */
	ulint		encryption_error; /*!< if an encryption error occurs */
	fil_space_crypt_t* crypt_data;  /*!< crypt data */
	dict_table_t*	table;		/*!< table */
};

struct fil_space_t;

/** File node of a tablespace or the log data space */
struct fil_node_t {
	fil_space_t*	space;	/*!< backpointer to the space where this node
				belongs */
	char*		name;	/*!< path to the file */
	ibool		open;	/*!< TRUE if file open */
	pfs_os_file_t	handle;	/*!< OS handle to the file, if file open */
	os_event_t	sync_event;/*!< Condition event to group and
				serialize calls to fsync;
				os_event_set() and os_event_reset()
				are protected by fil_system_t::mutex */
	ibool		is_raw_disk;/*!< TRUE if the 'file' is actually a raw
				device or a raw disk partition */
	ulint		size;	/*!< size of the file in database pages, 0 if
				not known yet; the possible last incomplete
				megabyte may be ignored if space == 0 */
	ulint		n_pending;
				/*!< count of pending i/o's on this file;
				closing of the file is not allowed if
				this is > 0 */
	ulint		n_pending_flushes;
				/*!< count of pending flushes on this file;
				closing of the file is not allowed if
				this is > 0 */
	ibool		being_extended;
				/*!< TRUE if the node is currently
				being extended. */
	ib_int64_t	modification_counter;/*!< when we write to the file we
				increment this by one */
	ib_int64_t	flush_counter;/*!< up to what
				modification_counter value we have
				flushed the modifications to disk */
	ulint		file_block_size;/*!< file system block size */
	UT_LIST_NODE_T(fil_node_t) chain;
				/*!< link field for the file chain */
	UT_LIST_NODE_T(fil_node_t) LRU;
				/*!< link field for the LRU list */
	ulint		magic_n;/*!< FIL_NODE_MAGIC_N */
};

/** Value of fil_node_t::magic_n */
#define	FIL_NODE_MAGIC_N	89389

/** Tablespace or log data space: let us call them by a common name space */
struct fil_space_t {
	char*		name;	/*!< space name = the path to the first file in
				it */
	hash_node_t	name_hash;/*!< hash chain the name_hash table */
	ulint		id;	/*!< space id */
	hash_node_t	hash;	/*!< hash chain node */
	ib_int64_t	tablespace_version;
				/*!< in DISCARD/IMPORT this timestamp
				is used to check if we should ignore
				an insert buffer merge request for a
				page because it actually was for the
				previous incarnation of the space */
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
	ulint		purpose;/*!< FIL_TABLESPACE, FIL_LOG, or
				FIL_ARCH_LOG */
	UT_LIST_BASE_NODE_T(fil_node_t) chain;
				/*!< base node for the file chain */
	ulint		size;	/*!< space size in pages; 0 if a single-table
				tablespace whose size we do not know yet;
				last incomplete megabytes in data files may be
				ignored if space == 0 */
	ulint		recv_size;
				/*!< recovered tablespace size in pages;
				0 if no size change was read from the redo log,
				or if the size change was implemented */
  /** the committed size of the tablespace in pages */
  ulint committed_size;
	ulint		flags;	/*!< FSP_SPACE_FLAGS and FSP_FLAGS_MEM_ flags;
				see fsp0fsp.h,
				fsp_flags_is_valid(),
				fsp_flags_get_zip_size() */
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
#ifndef UNIV_HOTBACKUP
	prio_rw_lock_t	latch;	/*!< latch protecting the file space storage
				allocation */
#endif /* !UNIV_HOTBACKUP */

	UT_LIST_NODE_T(fil_space_t) unflushed_spaces;
				/*!< list of spaces with at least one unflushed
				file we have written to */
	bool		is_in_unflushed_spaces;
				/*!< true if this space is currently in
				unflushed_spaces */
	/** True if srv_pass_corrupt_table=true and tablespace contains
	corrupted page. */
	bool		is_corrupt;
				/*!< true if tablespace corrupted */
	fil_space_crypt_t* crypt_data;
				/*!< tablespace crypt data or NULL */
	ulint		file_block_size;
				/*!< file system block size */

	UT_LIST_NODE_T(fil_space_t) space_list;
				/*!< list of all spaces */

	/*!< Protected by fil_system */
	UT_LIST_NODE_T(fil_space_t) rotation_list;
				/*!< list of spaces needing
				key rotation */

	bool		is_in_rotation_list;
				/*!< true if this space is
				currently in key rotation list */

	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */

	/** @return whether the tablespace is about to be dropped or truncated */
	bool is_stopping() const
	{
		return stop_new_ops;
	}

  /** Clamp a page number for batched I/O, such as read-ahead.
  @param offset   page number limit
  @return offset clamped to the tablespace size */
  ulint max_page_number_for_io(ulint offset) const
  {
    const ulint limit= committed_size;
    return limit > offset ? offset : limit;
  }
};

/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** The tablespace memory cache; also the totality of logs (the log
data space) is stored here; below we talk about tablespaces, but also
the ib_logfiles form a 'space' and it is handled here */
struct fil_system_t {
#ifndef UNIV_HOTBACKUP
	ib_mutex_t		mutex;		/*!< The mutex protecting the cache */
#endif /* !UNIV_HOTBACKUP */
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
	ib_int64_t	modification_counter;/*!< when we write to a file we
					increment this by one */
	ulint		max_assigned_id;/*!< maximum space id in the existing
					tables, or assigned during the time
					mysqld has been up; at an InnoDB
					startup we scan the data dictionary
					and set here the maximum of the
					space id's of the tables there */
	ib_int64_t	tablespace_version;
					/*!< a counter which is incremented for
					every space object memory creation;
					every space mem object gets a
					'timestamp' from this; in DISCARD/
					IMPORT this is used to check if we
					should ignore an insert buffer merge
					request */
	UT_LIST_BASE_NODE_T(fil_space_t) space_list;
					/*!< list of all file spaces */

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

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Returns the version number of a tablespace, -1 if not found.
@return version number, -1 if the tablespace does not exist in the
memory cache */
UNIV_INTERN
ib_int64_t
fil_space_get_version(
/*==================*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the latch of a file space.
@return	latch protecting storage allocation */
UNIV_INTERN
prio_rw_lock_t*
fil_space_get_latch(
/*================*/
	ulint	id,	/*!< in: space id */
	ulint*	zip_size);/*!< out: compressed page size, or
			0 for uncompressed tablespaces */
/*******************************************************************//**
Returns the type of a file space.
@return	FIL_TABLESPACE or FIL_LOG */
UNIV_INTERN
ulint
fil_space_get_type(
/*===============*/
	ulint	id);	/*!< in: space id */
#endif /* !UNIV_HOTBACKUP */
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
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#ifdef UNIV_LOG_ARCHIVE
/****************************************************************//**
Drops files from the start of a file space, so that its size is cut by
the amount given. */
UNIV_INTERN
void
fil_space_truncate_start(
/*=====================*/
	ulint	id,		/*!< in: space id */
	ulint	trunc_len);	/*!< in: truncate by this much; it is an error
				if this does not equal to the combined size of
				some initial files in the space */
/****************************************************************//**
Check is there node in file space with given name. */
UNIV_INTERN
ibool
fil_space_contains_node(
/*====================*/
	ulint	id,		/*!< in: space id */
	char*	node_name);	/*!< in: node name */
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
	fil_encryption_t	mode = FIL_ENCRYPTION_DEFAULT);

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return	TRUE if assigned, FALSE if not */
UNIV_INTERN
ibool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id);	/*!< in/out: space id */
/*******************************************************************//**
Returns the path from the first fil_node_t found for the space ID sent.
The caller is responsible for freeing the memory allocated here for the
value returned.
@return	a copy of fil_node_t::path, NULL if space is zero or not found. */
UNIV_INTERN
char*
fil_space_get_first_path(
/*=====================*/
	ulint	id);	/*!< in: space id */
/** Set the recovered size of a tablespace in pages.
@param id	tablespace ID
@param size	recovered size in pages */
UNIV_INTERN
void
fil_space_set_recv_size(ulint id, ulint size);
/*******************************************************************//**
Returns the size of the space in pages. The tablespace must be cached in the
memory cache.
@return	space size, 0 if space not found */
UNIV_INTERN
ulint
fil_space_get_size(
/*===============*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the flags of the space. The tablespace must be cached
in the memory cache.
@return	flags, ULINT_UNDEFINED if space not found */
UNIV_INTERN
ulint
fil_space_get_flags(
/*================*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the compressed page size of the space, or 0 if the space
is not compressed. The tablespace must be cached in the memory cache.
@return	compressed page size, ULINT_UNDEFINED if space not found */
UNIV_INTERN
ulint
fil_space_get_zip_size(
/*===================*/
	ulint	id);	/*!< in: space id */
/****************************************************************//**
Initializes the tablespace memory cache. */
UNIV_INTERN
void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open);	/*!< in: max number of open files */
/*******************************************************************//**
Initializes the tablespace memory cache. */
UNIV_INTERN
void
fil_close(void);
/*===========*/
/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */
UNIV_INTERN
void
fil_open_log_and_system_tablespace_files(void);
/*==========================================*/
/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
UNIV_INTERN
void
fil_close_all_files(void);
/*=====================*/
/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */
UNIV_INTERN
void
fil_close_log_files(
/*================*/
	bool	free);	/*!< in: whether to free the memory object */
/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */
UNIV_INTERN
void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id);/*!< in: maximum known id */

#ifndef UNIV_HOTBACKUP

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
@param[in]	for_io	whether to look up the tablespace while performing I/O
			(possibly executing TRUNCATE)
@return	the tablespace
@retval	NULL if missing or being deleted or truncated */
inline
fil_space_t*
fil_space_acquire(ulint id)
{
	return(fil_space_acquire_low(id, false));
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
	return(fil_space_acquire_low(id, true));
}

/** Release a tablespace acquired with fil_space_acquire().
@param[in,out]	space	tablespace to release  */
UNIV_INTERN
void
fil_space_release(fil_space_t* space);

/** Acquire a tablespace for reading or writing a block,
when it could be dropped concurrently.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing */
UNIV_INTERN
fil_space_t*
fil_space_acquire_for_io(ulint id);

/** Release a tablespace acquired with fil_space_acquire_for_io().
@param[in,out]	space	tablespace to release  */
UNIV_INTERN
void
fil_space_release_for_io(fil_space_t* space);

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
	@param[in]	silent		whether not to print any errors */
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
	bool		check_first_page=true)
	MY_ATTRIBUTE((warn_unused_result));

#endif /* !UNIV_HOTBACKUP */
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
	ulint	log_flags);	/*!< in: redo log flags
				(stored in the page number parameter) */

/** Determine whether a table can be accessed in operations that are
not (necessarily) protected by meta-data locks.
(Rollback would generally be protected, but rollback of
FOREIGN KEY CASCADE/SET NULL is not protected by meta-data locks
but only by InnoDB table locks, which may be broken by
lock_remove_all_on_table().)
@param[in]	table	persistent table
checked @return whether the table is accessible */
UNIV_INTERN bool fil_table_accessible(const dict_table_t* table)
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Delete a tablespace and associated .ibd file.
@param[in]	id		tablespace identifier
@param[in]	drop_ahi	whether to drop the adaptive hash index
@return	DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_delete_tablespace(ulint id, bool drop_ahi = false);
/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return	DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*	trx,	/*!< in/out: Transaction covering the close */
	ulint	id);	/*!< in: space id */
#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. When the user does IMPORT TABLESPACE, the tablespace will have the
    same id as it originally had.

 4. Free all the pages in use by the tablespace if rename=TRUE.
@return	DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_discard_tablespace(
/*===================*/
	ulint	id)	/*!< in: space id */
	MY_ATTRIBUTE((warn_unused_result));
#endif /* !UNIV_HOTBACKUP */

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
	const char*	new_path);	/*!< in: new full datafile path
					if the tablespace is remotely
					located, or NULL if it is located
					in the normal data directory. */

/*******************************************************************//**
Allocates a file name for a single-table tablespace. The string must be freed
by caller with mem_free().
@return	own: file name */
UNIV_INTERN
char*
fil_make_ibd_name(
/*==============*/
	const char*	name,		/*!< in: table name or a dir path */
	bool		is_full_path);	/*!< in: TRUE if it is a dir path */
/*******************************************************************//**
Allocates a file name for a tablespace ISL file (InnoDB Symbolic Link).
The string must be freed by caller with mem_free().
@return	own: file name */
UNIV_INTERN
char*
fil_make_isl_name(
/*==============*/
	const char*	name);	/*!< in: table name */
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
	const char*	filepath);	/*!< in: pathname of tablespace */
/*******************************************************************//**
Deletes an InnoDB Symbolic Link (ISL) file. */
UNIV_INTERN
void
fil_delete_link_file(
/*==================*/
	const char*	tablename);	/*!< in: name of table */
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
	const char*	name);		/*!< in: tablespace name */

#include "fil0crypt.h"

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
	MY_ATTRIBUTE((nonnull(2), warn_unused_result));
#ifndef UNIV_HOTBACKUP
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
	const char*	filepath)	/*!< in: tablespace filepath */
	__attribute__((nonnull(5), warn_unused_result));

#endif /* !UNIV_HOTBACKUP */
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
fil_load_single_table_tablespaces(ibool (*pred)(const char*, const char*)=0);
/*===================================*/
/*******************************************************************//**
Returns TRUE if a single-table tablespace does not exist in the memory cache,
or is being deleted there.
@return	TRUE if does not exist or is being deleted */
UNIV_INTERN
ibool
fil_tablespace_deleted_or_being_deleted_in_mem(
/*===========================================*/
	ulint		id,	/*!< in: space id */
	ib_int64_t	version);/*!< in: tablespace_version should be this; if
				you pass -1 as the value of this, then this
				parameter is ignored */
/*******************************************************************//**
Returns TRUE if a single-table tablespace exists in the memory cache.
@return	TRUE if exists */
UNIV_INTERN
ibool
fil_tablespace_exists_in_mem(
/*=========================*/
	ulint	id);	/*!< in: space id */
#ifndef UNIV_HOTBACKUP
/** Check if a matching tablespace exists in the InnoDB tablespace memory
cache. Note that if we have not done a crash recovery at the database startup,
there may be many tablespaces which are not yet in the memory cache.
@return whether a matching tablespace exists in the memory cache */
UNIV_INTERN
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
	bool		remove_from_data_dict_if_does_not_exist,
					/*!< in: remove from the data dictionary
					if tablespace does not exist */
	bool		adjust_space,	/*!< in: whether to adjust space id
					when find table space mismatch */
	mem_heap_t*	heap,		/*!< in: heap memory */
	table_id_t	table_id,	/*!< in: table id */
	ulint		table_flags);	/*!< in: table flags */
#else /* !UNIV_HOTBACKUP */
/********************************************************************//**
Extends all tablespaces to the size stored in the space header. During the
mysqlbackup --apply-log phase we extended the spaces on-demand so that log
records could be appllied, but that may have left spaces still too small
compared to the size stored in the space header. */
UNIV_INTERN
void
fil_extend_tablespaces_to_stored_len(void);
/*======================================*/
#endif /* !UNIV_HOTBACKUP */
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
	ulint	size_after_extend);/*!< in: desired size in pages after the
				extension; if the current space size is bigger
				than this already, the function does nothing */
/*******************************************************************//**
Tries to reserve free extents in a file space.
@return	TRUE if succeed */
UNIV_INTERN
ibool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve);	/*!< in: how many one wants to reserve */
/*******************************************************************//**
Releases free extents in a file space. */
UNIV_INTERN
void
fil_space_release_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_reserved);	/*!< in: how many one reserved */
/*******************************************************************//**
Gets the number of reserved extents. If the database is silent, this number
should be zero. */
UNIV_INTERN
ulint
fil_space_get_n_reserved_extents(
/*=============================*/
	ulint	id);		/*!< in: space id */
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
	trx_t*	trx = NULL,	/*!< in: trx */
	bool	should_buffer = false)
				/*!< in: whether to buffer an aio request.
				AIO read ahead uses this. If you plan to
				use this parameter, make sure you remember
				to call os_aio_dispatch_read_array_submit()
				when you're ready to commit all your requests.*/
	MY_ATTRIBUTE((nonnull(8)));

/** Determine the block size of the data file.
@param[in]	space		tablespace
@param[in]	offset		page number
@return	block size */
UNIV_INTERN
ulint
fil_space_get_block_size(const fil_space_t* space, unsigned offset);

/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */
UNIV_INTERN
void
fil_aio_wait(
/*=========*/
	ulint	segment);	/*!< in: the number of the segment in the aio
				array to wait for */
/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */
UNIV_INTERN
void
fil_flush(
/*======*/
	ulint	space_id);	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
/** Flush a tablespace.
@param[in,out]	space	tablespace to flush */
UNIV_INTERN
void
fil_flush(fil_space_t* space);

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG */
UNIV_INTERN
void
fil_flush_file_spaces(ulint purpose);
/******************************************************************//**
Checks the consistency of the tablespace cache.
@return	TRUE if ok */
UNIV_INTERN
ibool
fil_validate(void);
/*==============*/
/********************************************************************//**
Returns TRUE if file address is undefined.
@return	TRUE if undefined */
UNIV_INTERN
ibool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr);	/*!< in: address */
/********************************************************************//**
Get the predecessor of a file page.
@return	FIL_PAGE_PREV */
UNIV_INTERN
ulint
fil_page_get_prev(
/*==============*/
	const byte*	page);	/*!< in: file page */
/********************************************************************//**
Get the successor of a file page.
@return	FIL_PAGE_NEXT */
UNIV_INTERN
ulint
fil_page_get_next(
/*==============*/
	const byte*	page);	/*!< in: file page */
/*********************************************************************//**
Sets the file page type. */
UNIV_INTERN
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type);	/*!< in: type */
/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */
UNIV_INTERN
ulint
fil_page_get_type(
/*==============*/
	const byte*	page);	/*!< in: file page */

/*******************************************************************//**
Returns TRUE if a single-table tablespace is being deleted.
@return TRUE if being deleted */
UNIV_INTERN
ibool
fil_tablespace_is_being_deleted(
/*============================*/
	ulint		id);	/*!< in: space id */

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */
UNIV_INTERN
void
fil_delete_file(
/*============*/
	const char*	path);	/*!< in: filepath of the ibd tablespace */

/*******************************************************************//**
Checks if a single-table tablespace for a given table name exists in the
tablespace memory cache.
@return	space id, ULINT_UNDEFINED if not found */
UNIV_INTERN
ulint
fil_get_space_id_for_table(
/*=======================*/
	const char*	name);	/*!< in: table name in the standard
				'databasename/tablename' format */

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
				/*!< in/out: Vector for collecting the names. */
	MY_ATTRIBUTE((warn_unused_result));

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
	MY_ATTRIBUTE((nonnull));

/*******************************************************************//**
Finds the given page_no of the given space id from the double write buffer,
and copies it to the corresponding .ibd file.
@return true if copy was successful, or false. */
bool
fil_user_tablespace_restore_page(
/*==============================*/
	fsp_open_info*	fsp,		/* in: contains space id and .ibd
					file information */
	ulint		page_no);	/* in: page_no to obtain from double
					write buffer */

/*******************************************************************//**
Returns a pointer to the file_space_t that is in the memory cache
associated with a space id.
@return	file_space_t pointer, NULL if space not found */
fil_space_t*
fil_space_get(
/*==========*/
	ulint	id);	/*!< in: space id */
#endif /* !UNIV_INNOCHECKSUM */

/*************************************************************************
Return local hash table informations. */

ulint
fil_system_hash_cells(void);
/*========================*/

ulint
fil_system_hash_nodes(void);
/*========================*/

/*************************************************************************
functions to access is_corrupt flag of fil_space_t*/

void
fil_space_set_corrupt(
/*==================*/
	ulint	space_id);

/** Acquire the fil_system mutex. */
#define fil_system_enter()	mutex_enter(&fil_system->mutex)
/** Release the fil_system mutex. */
#define fil_system_exit()	mutex_exit(&fil_system->mutex)

#ifndef UNIV_INNOCHECKSUM
/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
fil_space_t*
fil_space_found_by_id(
/*==================*/
	ulint	id);	/*!< in: space id */

/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
fil_space_t*
fil_space_get_by_id(
/*================*/
	ulint	id);	/*!< in: space id */

#endif /*  UNIV_INNOCHECKSUM */

/****************************************************************//**
Does error handling when a file operation fails.
@return	TRUE if we should retry the operation */
ibool
os_file_handle_error_no_exit(
/*=========================*/
	const char*	name,		/*!< in: name of a file or NULL */
	const char*	operation,	/*!< in: operation */
	ibool		on_error_silent,/*!< in: if TRUE then don't print
					any message to the log. */
	const char*	file,		/*!< in: file name */
	const ulint	line);		/*!< in: line */

/*******************************************************************//**
Return page type name */
UNIV_INLINE
const char*
fil_get_page_type_name(
/*===================*/
	ulint	page_type);	/*!< in: FIL_PAGE_TYPE */

#ifndef UNIV_NONINL
#include "fil0fil.ic"
#endif

#endif /* fil0fil_h */
