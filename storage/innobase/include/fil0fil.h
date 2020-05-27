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

#include "fsp0types.h"
#include "mach0data.h"
#include "assume_aligned.h"

#ifndef UNIV_INNOCHECKSUM

#include "hash0hash.h"
#include "log0recv.h"
#include "dict0types.h"
#include "intrusive_list.h"
#ifdef UNIV_LINUX
# include <set>
#endif

struct unflushed_spaces_tag_t;
struct rotation_list_tag_t;

// Forward declaration
extern my_bool srv_use_doublewrite_buf;

/** Possible values of innodb_flush_method */
enum srv_flush_t
{
  /** fsync, the default */
  SRV_FSYNC= 0,
  /** open log files in O_DSYNC mode */
  SRV_O_DSYNC,
  /** do not call os_file_flush() when writing data files, but do flush
  after writing to log files */
  SRV_LITTLESYNC,
  /** do not flush after writing */
  SRV_NOSYNC,
  /** invoke os_file_set_nocache() on data files. This implies using
  non-buffered IO but still using fsync, the reason for which is that
  some FS do not flush meta-data when unbuffered IO happens */
  SRV_O_DIRECT,
  /** do not use fsync() when using direct IO i.e.: it can be set to
  avoid the fsync() call that we make when using SRV_UNIX_O_DIRECT.
  However, in this case user/DBA should be sure about the integrity of
  the meta-data */
  SRV_O_DIRECT_NO_FSYNC
#ifdef _WIN32
  /** Traditional Windows appoach to open all files without caching,
  and do FileFlushBuffers() */
  ,SRV_ALL_O_DIRECT_FSYNC
#endif
};

/** innodb_flush_method */
extern ulong srv_file_flush_method;

/** Undo tablespaces starts with space_id. */
extern	ulint	srv_undo_space_id_start;
/** The number of UNDO tablespaces that are open and ready to use. */
extern ulint	srv_undo_tablespaces_open;

/** Check whether given space id is undo tablespace id
@param[in]	space_id	space id to check
@return true if it is undo tablespace else false. */
inline bool srv_is_undo_tablespace(ulint space_id)
{
  return srv_undo_space_id_start > 0 &&
    space_id >= srv_undo_space_id_start &&
    space_id < srv_undo_space_id_start + srv_undo_tablespaces_open;
}

extern struct buf_dblwr_t* buf_dblwr;
class page_id_t;

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
};

struct fil_node_t;

#endif

/** Tablespace or log data space */
#ifndef UNIV_INNOCHECKSUM
struct fil_space_t : intrusive::list_node<unflushed_spaces_tag_t>,
                     intrusive::list_node<rotation_list_tag_t>
#else
struct fil_space_t
#endif
{
#ifndef UNIV_INNOCHECKSUM
	ulint		id;	/*!< space id */
	hash_node_t	hash;	/*!< hash chain node */
	char*		name;	/*!< Tablespace name */
	lsn_t		max_lsn;
				/*!< LSN of the most recent
				fil_names_write_if_was_clean().
				Reset to 0 by fil_names_clear().
				Protected by log_sys.mutex.
				If and only if this is nonzero, the
				tablespace will be in named_spaces. */
	/** set when an .ibd file is about to be deleted,
	or an undo tablespace is about to be truncated.
	When this is set, the following new ops are not allowed:
	* read IO request
	* file flush
	Note that we can still possibly have new write operations
	because we don't check this flag when doing flush batches. */
	bool		stop_new_ops;
	/** whether undo tablespace truncation is in progress */
	bool		is_being_truncated;
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
	ulint		n_reserved_extents;
				/*!< number of reserved free extents for
				ongoing operations like B-tree page split */
	ulint		n_pending_flushes; /*!< this is positive when flushing
				the tablespace to disk; dropping of the
				tablespace is forbidden if this is positive */
	/** Number of pending buffer pool operations accessing the tablespace
	without holding a table lock or dict_sys.latch S-latch
	that would prevent the table (and tablespace) from being
	dropped. An example is fil_crypt_thread.
	The tablespace cannot be dropped while this is nonzero,
	or while fil_node_t::n_pending is nonzero.
	Protected by fil_system.mutex and std::atomic. */
	std::atomic<ulint>		n_pending_ops;
	/** Number of pending block read or write operations
	(when a write is imminent or a read has recently completed).
	The tablespace object cannot be freed while this is nonzero,
	but it can be detached from fil_system.
	Note that fil_node_t::n_pending tracks actual pending I/O requests.
	Protected by fil_system.mutex and std::atomic. */
	std::atomic<ulint>		n_pending_ios;
	rw_lock_t	latch;	/*!< latch protecting the file space storage
				allocation */
	UT_LIST_NODE_T(fil_space_t) named_spaces;
				/*!< list of spaces for which FILE_MODIFY
				records have been issued */
	/** Checks that this tablespace in a list of unflushed tablespaces.
	@return true if in a list */
	bool is_in_unflushed_spaces() const;
	UT_LIST_NODE_T(fil_space_t) space_list;
				/*!< list of all spaces */
	/** Checks that this tablespace needs key rotation.
	@return true if in a rotation list */
	bool is_in_rotation_list() const;

	/** MariaDB encryption data */
	fil_space_crypt_t* crypt_data;

	/** True if the device this filespace is on supports atomic writes */
	bool		atomic_write_supported;

	/** True if file system storing this tablespace supports
	punch hole */
	bool		punch_hole;

	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */

	/** @return whether the tablespace is about to be dropped */
	bool is_stopping() const { return stop_new_ops;	}

	/** @return whether doublewrite buffering is needed */
	bool use_doublewrite() const
	{
		return !atomic_write_supported
			&& srv_use_doublewrite_buf && buf_dblwr;
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
	fil_node_t* add(const char* name, pfs_os_file_t handle,
			ulint size, bool is_raw, bool atomic_write,
			ulint max_pages = ULINT_MAX);
#ifdef UNIV_DEBUG
	/** Assert that the mini-transaction is compatible with
	updating an allocation bitmap page.
	@param[in]	mtr	mini-transaction */
	void modify_check(const mtr_t& mtr) const;
#endif /* UNIV_DEBUG */

	/** Try to reserve free extents.
	@param[in]	n_free_now	current number of free extents
	@param[in]	n_to_reserve	number of extents to reserve
	@return	whether the reservation succeeded */
	bool reserve_free_extents(ulint n_free_now, ulint n_to_reserve)
	{
		ut_ad(rw_lock_own(&latch, RW_LOCK_X));
		if (n_reserved_extents + n_to_reserve > n_free_now) {
			return false;
		}

		n_reserved_extents += n_to_reserve;
		return true;
	}

	/** Release the reserved free extents.
	@param[in]	n_reserved	number of reserved extents */
	void release_free_extents(ulint n_reserved)
	{
		if (!n_reserved) return;
		ut_ad(rw_lock_own(&latch, RW_LOCK_X));
		ut_a(n_reserved_extents >= n_reserved);
		n_reserved_extents -= n_reserved;
	}

	/** Rename a file.
	@param[in]	name	table name after renaming
	@param[in]	path	tablespace file name after renaming
	@param[in]	log	whether to write redo log
	@param[in]	replace	whether to ignore the existence of path
	@return	error code
	@retval	DB_SUCCESS	on success */
	dberr_t rename(const char* name, const char* path, bool log,
		       bool replace = false);

	/** Note that the tablespace has been imported.
	Initially, purpose=FIL_TYPE_IMPORT so that no redo log is
	written while the space ID is being updated in each page. */
	inline void set_imported();

	/** @return whether the storage device is rotational (HDD, not SSD) */
	inline bool is_rotational() const;

	/** Open each file. Only invoked on fil_system.temp_space.
	@return whether all files were opened */
	bool open();
	/** Close each file. Only invoked on fil_system.temp_space. */
	void close();

	/** Acquire a tablespace reference. */
	void acquire() { n_pending_ops++; }
	/** Release a tablespace reference. */
	void release() { ut_ad(referenced()); n_pending_ops--; }
	/** @return whether references are being held */
	bool referenced() const { return n_pending_ops; }

	/** Acquire a tablespace reference for I/O. */
	void acquire_for_io() { n_pending_ios++; }
	/** Release a tablespace reference for I/O. */
	void release_for_io() { ut_ad(pending_io()); n_pending_ios--; }
	/** @return whether I/O is pending */
	bool pending_io() const { return n_pending_ios; }

  /** @return whether the tablespace file can be closed and reopened */
  bool belongs_in_lru() const
  {
    switch (purpose) {
    case FIL_TYPE_TEMPORARY:
      ut_ad(id == SRV_TMP_SPACE_ID);
      return false;
    case FIL_TYPE_IMPORT:
      ut_ad(id != SRV_TMP_SPACE_ID);
      return true;
    case FIL_TYPE_TABLESPACE:
      ut_ad(id != SRV_TMP_SPACE_ID);
      return id && !srv_is_undo_tablespace(id);
    }
    ut_ad(0);
    return false;
  }
#endif /* !UNIV_INNOCHECKSUM */
	/** FSP_SPACE_FLAGS and FSP_FLAGS_MEM_ flags;
	check fsp0types.h to more info about flags. */
	ulint		flags;

	/** Determine if full_crc32 is used for a data file
	@param[in]	flags	tablespace flags (FSP_FLAGS)
	@return whether the full_crc32 algorithm is active */
	static bool full_crc32(ulint flags) {
		return flags & FSP_FLAGS_FCRC32_MASK_MARKER;
	}
	/** @return whether innodb_checksum_algorithm=full_crc32 is active */
	bool full_crc32() const { return full_crc32(flags); }
	/** Determine the logical page size.
	@param	flags	tablespace flags (FSP_FLAGS)
	@return the logical page size
	@retval 0 if the flags are invalid */
	static unsigned logical_size(ulint flags) {

		ulint page_ssize = 0;

		if (full_crc32(flags)) {
			page_ssize = FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(flags);
		} else {
			page_ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
		}

		switch (page_ssize) {
		case 3: return 4096;
		case 4: return 8192;
		case 5:
		{ ut_ad(full_crc32(flags)); return 16384; }
		case 0:
		{ ut_ad(!full_crc32(flags)); return 16384; }
		case 6: return 32768;
		case 7: return 65536;
		default: return 0;
		}
	}
	/** Determine the ROW_FORMAT=COMPRESSED page size.
	@param	flags	tablespace flags (FSP_FLAGS)
	@return the ROW_FORMAT=COMPRESSED page size
	@retval 0	if ROW_FORMAT=COMPRESSED is not used */
	static unsigned zip_size(ulint flags) {

		if (full_crc32(flags)) {
			return 0;
		}

		ulint zip_ssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
		return zip_ssize
			? (UNIV_ZIP_SIZE_MIN >> 1) << zip_ssize : 0;
	}
	/** Determine the physical page size.
	@param	flags	tablespace flags (FSP_FLAGS)
	@return the physical page size */
	static unsigned physical_size(ulint flags) {

		if (full_crc32(flags)) {
			return logical_size(flags);
		}

		ulint zip_ssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
		return zip_ssize
			? (UNIV_ZIP_SIZE_MIN >> 1) << zip_ssize
			: unsigned(srv_page_size);
	}
	/** @return the ROW_FORMAT=COMPRESSED page size
	@retval 0	if ROW_FORMAT=COMPRESSED is not used */
	unsigned zip_size() const { return zip_size(flags); }
	/** @return the physical page size */
	unsigned physical_size() const { return physical_size(flags); }
	/** Check whether the compression enabled in tablespace.
	@param[in]	flags	tablespace flags */
	static bool is_compressed(ulint flags) {

		if (full_crc32(flags)) {
			ulint algo = FSP_FLAGS_FCRC32_GET_COMPRESSED_ALGO(
				flags);
			DBUG_ASSERT(algo <= PAGE_ALGORITHM_LAST);
			return algo > 0;
		}

		return FSP_FLAGS_HAS_PAGE_COMPRESSION(flags);
	}
	/** @return whether the compression enabled for the tablespace. */
	bool is_compressed() const { return is_compressed(flags); }

	/** Get the compression algorithm for full crc32 format.
	@param[in]	flags	tablespace flags
	@return algorithm type of tablespace */
	static ulint get_compression_algo(ulint flags)
	{
		return full_crc32(flags)
			? FSP_FLAGS_FCRC32_GET_COMPRESSED_ALGO(flags)
			: 0;
	}
	/** @return the page_compressed algorithm
	@retval 0 if not page_compressed */
	ulint get_compression_algo() const {
		return fil_space_t::get_compression_algo(flags);
	}
	/** Determine if the page_compressed page contains an extra byte
	for exact compressed stream length
	@param[in]	flags	tablespace flags
	@return	whether the extra byte is needed */
	static bool full_crc32_page_compressed_len(ulint flags)
	{
		DBUG_ASSERT(full_crc32(flags));
		switch (get_compression_algo(flags)) {
		case PAGE_LZ4_ALGORITHM:
		case PAGE_LZO_ALGORITHM:
		case PAGE_SNAPPY_ALGORITHM:
			return true;
		}
		return false;
	}

	/** Whether the full checksum matches with non full checksum flags.
	@param[in]	flags		flags present
	@param[in]	expected	expected flags
	@return true if it is equivalent */
	static bool is_flags_full_crc32_equal(ulint flags, ulint expected)
	{
		ut_ad(full_crc32(flags));
		ulint page_ssize = FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(flags);

		if (full_crc32(expected)) {
			/* The data file may have been created with a
			different innodb_compression_algorithm. But
			we only support one innodb_page_size for all files. */
			return page_ssize
				== FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(expected);
		}

		ulint space_page_ssize = FSP_FLAGS_GET_PAGE_SSIZE(expected);

		if (page_ssize == 5) {
			if (space_page_ssize) {
				return false;
			}
		} else if (space_page_ssize != page_ssize) {
			return false;
		}

		return true;
	}
	/** Whether old tablespace flags match full_crc32 flags.
	@param[in]	flags		flags present
	@param[in]	expected	expected flags
	@return true if it is equivalent */
	static bool is_flags_non_full_crc32_equal(ulint flags, ulint expected)
	{
		ut_ad(!full_crc32(flags));

		if (!full_crc32(expected)) {
			return false;
		}

		ulint page_ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
		ulint space_page_ssize = FSP_FLAGS_FCRC32_GET_PAGE_SSIZE(
			expected);

		if (page_ssize) {
			if (space_page_ssize != 5) {
				return false;
			}
		} else if (space_page_ssize != page_ssize) {
			return false;
		}

		return true;
	}
	/** Whether both fsp flags are equivalent */
	static bool is_flags_equal(ulint flags, ulint expected)
	{
		if (!((flags ^ expected) & ~(1U << FSP_FLAGS_POS_RESERVED))) {
			return true;
		}

		return full_crc32(flags)
			? is_flags_full_crc32_equal(flags, expected)
			: is_flags_non_full_crc32_equal(flags, expected);
	}
	/** Validate the tablespace flags for full crc32 format.
	@param[in]	flags	the content of FSP_SPACE_FLAGS
	@return whether the flags are correct in full crc32 format */
	static bool is_fcrc32_valid_flags(ulint flags)
	{
		ut_ad(flags & FSP_FLAGS_FCRC32_MASK_MARKER);
		const ulint page_ssize = physical_size(flags);
		if (page_ssize < 3 || page_ssize & 8) {
			return false;
		}

		flags >>= FSP_FLAGS_FCRC32_POS_COMPRESSED_ALGO;

		return flags <= PAGE_ALGORITHM_LAST;
	}
	/** Validate the tablespace flags.
	@param[in]	flags	content of FSP_SPACE_FLAGS
	@param[in]	is_ibd	whether this is an .ibd file
				(not system tablespace)
	@return whether the flags are correct. */
	static bool is_valid_flags(ulint flags, bool is_ibd)
	{
		DBUG_EXECUTE_IF("fsp_flags_is_valid_failure",
				return false;);

		if (full_crc32(flags)) {
			return is_fcrc32_valid_flags(flags);
		}

		if (flags == 0) {
			return true;
		}

		if (flags & ~FSP_FLAGS_MASK) {
			return false;
		}

		if ((flags & (FSP_FLAGS_MASK_POST_ANTELOPE
			      | FSP_FLAGS_MASK_ATOMIC_BLOBS))
		    == FSP_FLAGS_MASK_ATOMIC_BLOBS) {
			/* If the "atomic blobs" flag (indicating
			ROW_FORMAT=DYNAMIC or ROW_FORMAT=COMPRESSED) flag
			is set, then the "post Antelope"
			(ROW_FORMAT!=REDUNDANT) flag must also be set. */
			return false;
		}

		/* Bits 10..14 should be 0b0000d where d is the DATA_DIR flag
		of MySQL 5.6 and MariaDB 10.0, which we ignore.
		In the buggy FSP_SPACE_FLAGS written by MariaDB 10.1.0 to 10.1.20,
		bits 10..14 would be nonzero 0bsssaa where sss is
		nonzero PAGE_SSIZE (3, 4, 6, or 7)
		and aa is ATOMIC_WRITES (not 0b11). */
		if (FSP_FLAGS_GET_RESERVED(flags) & ~1U) {
			return false;
		}

		const ulint	ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
		if (ssize == 1 || ssize == 2 || ssize == 5 || ssize & 8) {
			/* the page_size is not between 4k and 64k;
			16k should be encoded as 0, not 5 */
			return false;
		}

		const ulint     zssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
		if (zssize == 0) {
			/* not ROW_FORMAT=COMPRESSED */
		} else if (zssize > (ssize ? ssize : 5)) {
			/* Invalid KEY_BLOCK_SIZE */
			return false;
		} else if (~flags & (FSP_FLAGS_MASK_POST_ANTELOPE
				     | FSP_FLAGS_MASK_ATOMIC_BLOBS)) {
			/* both these flags should be set for
			ROW_FORMAT=COMPRESSED */
			return false;
		}

		/* The flags do look valid. But, avoid misinterpreting
		buggy MariaDB 10.1 format flags for
		PAGE_COMPRESSED=1 PAGE_COMPRESSION_LEVEL={0,2,3}
		as valid-looking PAGE_SSIZE if this is known to be
		an .ibd file and we are using the default innodb_page_size=16k. */
		return(ssize == 0 || !is_ibd
		       || srv_page_size != UNIV_PAGE_SIZE_ORIG);
	}
};

#ifndef UNIV_INNOCHECKSUM
/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** File node of a tablespace or the log data space */
struct fil_node_t {
	/** tablespace containing this file */
	fil_space_t*	space;
	/** file name; protected by fil_system.mutex and log_sys.mutex. */
	char*		name;
	/** file handle (valid if is_open) */
	pfs_os_file_t	handle;
	/** whether the file actually is a raw device or disk partition */
	bool		is_raw_disk;
	/** whether the file is on non-rotational media (SSD) */
	bool		on_ssd;
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
	/** whether this file had writes after lasy fsync() */
	bool		needs_flush;
	/** link to other files in this tablespace */
	UT_LIST_NODE_T(fil_node_t) chain;
	/** link to the fil_system.LRU list (keeping track of open files) */
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

	/** Read the first page of a data file.
	@param[in]	first	whether this is the very first read
	@return	whether the page was found valid */
	bool read_page0(bool first);

	/** Determine some file metadata when creating or reading the file.
	@param	file	the file that is being created, or OS_FILE_CLOSED */
	void find_metadata(os_file_t file = OS_FILE_CLOSED
#ifdef UNIV_LINUX
			   , struct stat* statbuf = NULL
#endif
			   );

  /** Close the file handle. */
  void close();
  /** Prepare to free a file from fil_system. */
  inline void close_to_free();

  /** Update the data structures on I/O completion */
  inline void complete_io(bool write= false);
};

/** Value of fil_node_t::magic_n */
#define	FIL_NODE_MAGIC_N	89389

inline void fil_space_t::set_imported()
{
	ut_ad(purpose == FIL_TYPE_IMPORT);
	purpose = FIL_TYPE_TABLESPACE;
	UT_LIST_GET_FIRST(chain)->find_metadata();
}

inline bool fil_space_t::is_rotational() const
{
	for (const fil_node_t* node = UT_LIST_GET_FIRST(chain);
	     node != NULL; node = UT_LIST_GET_NEXT(chain, node)) {
		if (!node->on_ssd) {
			return true;
		}
	}
	return false;
}

/** Common InnoDB file extensions */
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
#else
# include "univ.i"
#endif /* !UNIV_INNOCHECKSUM */

/** Initial size of a single-table tablespace in pages */
#define FIL_IBD_FILE_INITIAL_SIZE	4U

/** 'null' (undefined) page offset in the context of file spaces */
#define	FIL_NULL	ULINT32_UNDEFINED


#define FIL_ADDR_PAGE	0U	/* first in address is the page offset */
#define	FIL_ADDR_BYTE	4U	/* then comes 2-byte byte offset within page*/
#define	FIL_ADDR_SIZE	6U	/* address size is 6 bytes */

/** File space address */
struct fil_addr_t {
  /** page number within a tablespace */
  uint32_t page;
  /** byte offset within the page */
  uint16_t boffset;
};

/** The byte offsets on a file page for various variables @{ */
#define FIL_PAGE_SPACE_OR_CHKSUM 0	/*!< in < MySQL-4.0.14 space id the
					page belongs to (== 0) but in later
					versions the 'new' checksum of the
					page */
#define FIL_PAGE_OFFSET		4U	/*!< page offset inside space */
#define FIL_PAGE_PREV		8U	/*!< if there is a 'natural'
					predecessor of the page, its
					offset.  Otherwise FIL_NULL.
					This field is not set on BLOB
					pages, which are stored as a
					singly-linked list.  See also
					FIL_PAGE_NEXT. */
#define FIL_PAGE_NEXT		12U	/*!< if there is a 'natural' successor
					of the page, its offset.
					Otherwise FIL_NULL.
					B-tree index pages
					(FIL_PAGE_TYPE contains FIL_PAGE_INDEX)
					on the same PAGE_LEVEL are maintained
					as a doubly linked list via
					FIL_PAGE_PREV and FIL_PAGE_NEXT
					in the collation order of the
					smallest user record on each page. */
#define FIL_PAGE_LSN		16U	/*!< lsn of the end of the newest
					modification log record to the page */
#define	FIL_PAGE_TYPE		24U	/*!< file page type: FIL_PAGE_INDEX,...,
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

/** For the first page in a system tablespace data file(ibdata*, not *.ibd):
the file has been flushed to disk at least up to this lsn
For other pages: 32-bit key version used to encrypt the page + 32-bit checksum
or 64 bites of zero if no encryption */
#define FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION 26U

/** This overloads FIL_PAGE_FILE_FLUSH_LSN for RTREE Split Sequence Number */
#define	FIL_RTREE_SPLIT_SEQ_NUM	FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION

/** Start of the page_compressed content */
#define FIL_PAGE_COMP_ALGO	FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION

/** starting from 4.1.x this contains the space id of the page */
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID  34U

#define FIL_PAGE_SPACE_ID  FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID

#define FIL_PAGE_DATA		38U	/*!< start of the data on the page */

/** 32-bit key version used to encrypt the page in full_crc32 format.
For non-encrypted page, it contains 0. */
#define FIL_PAGE_FCRC32_KEY_VERSION	0

/** page_compressed without innodb_checksum_algorithm=full_crc32 @{ */
/** Number of bytes used to store actual payload data size on
page_compressed pages when not using full_crc32. */
#define FIL_PAGE_COMP_SIZE		0

/** Number of bytes for FIL_PAGE_COMP_SIZE */
#define FIL_PAGE_COMP_METADATA_LEN		2

/** Number of bytes used to store actual compression method
for encrypted tables when not using full_crc32. */
#define FIL_PAGE_ENCRYPT_COMP_ALGO		2

/** Extra header size for encrypted page_compressed pages when
not using full_crc32 */
#define FIL_PAGE_ENCRYPT_COMP_METADATA_LEN	4
/* @} */

/** File page trailer @{ */
#define FIL_PAGE_END_LSN_OLD_CHKSUM 8	/*!< the low 4 bytes of this are used
					to store the page checksum, the
					last 4 bytes should be identical
					to the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_DATA_END	8	/*!< size of the page trailer */

/** Store the last 4 bytes of FIL_PAGE_LSN */
#define FIL_PAGE_FCRC32_END_LSN 8

/** Store crc32 checksum at the end of the page */
#define FIL_PAGE_FCRC32_CHECKSUM	4
/* @} */

/** File page types (values of FIL_PAGE_TYPE) @{ */
/** page_compressed, encrypted=YES (not used for full_crc32) */
constexpr uint16_t FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED= 37401;
/** page_compressed (not used for full_crc32) */
constexpr uint16_t FIL_PAGE_PAGE_COMPRESSED= 34354;
/** B-tree index page */
constexpr uint16_t FIL_PAGE_INDEX= 17855;
/** R-tree index page (SPATIAL INDEX) */
constexpr uint16_t FIL_PAGE_RTREE= 17854;
/** Undo log page */
constexpr uint16_t FIL_PAGE_UNDO_LOG= 2;
/** Index node (of file-in-file metadata) */
constexpr uint16_t FIL_PAGE_INODE= 3;
/** Insert buffer free list */
constexpr uint16_t FIL_PAGE_IBUF_FREE_LIST= 4;
/** Freshly allocated page */
constexpr uint16_t FIL_PAGE_TYPE_ALLOCATED= 0;
/** Change buffer bitmap (pages n*innodb_page_size+1) */
constexpr uint16_t FIL_PAGE_IBUF_BITMAP= 5;
/** System page */
constexpr uint16_t FIL_PAGE_TYPE_SYS= 6;
/** Transaction system data */
constexpr uint16_t FIL_PAGE_TYPE_TRX_SYS= 7;
/** Tablespace header (page 0) */
constexpr uint16_t FIL_PAGE_TYPE_FSP_HDR= 8;
/** Extent descriptor page (pages n*innodb_page_size, except 0) */
constexpr uint16_t FIL_PAGE_TYPE_XDES= 9;
/** Uncompressed BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_BLOB= 10;
/** First ROW_FORMAT=COMPRESSED BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB= 11;
/** Subsequent ROW_FORMAT=COMPRESSED BLOB page */
constexpr uint16_t FIL_PAGE_TYPE_ZBLOB2= 12;
/** In old tablespaces, garbage in FIL_PAGE_TYPE is replaced with this
value when flushing pages. */
constexpr uint16_t FIL_PAGE_TYPE_UNKNOWN= 13;

/* File page types introduced in MySQL 5.7, not supported in MariaDB */
//constexpr uint16_t FIL_PAGE_COMPRESSED = 14;
//constexpr uint16_t FIL_PAGE_ENCRYPTED = 15;
//constexpr uint16_t FIL_PAGE_COMPRESSED_AND_ENCRYPTED = 16;
//constexpr FIL_PAGE_ENCRYPTED_RTREE = 17;
/** Clustered index root page after instant ADD COLUMN */
constexpr uint16_t FIL_PAGE_TYPE_INSTANT= 18;

/** Used by i_s.cc to index into the text description.
Note: FIL_PAGE_TYPE_INSTANT maps to the same as FIL_PAGE_INDEX. */
constexpr uint16_t FIL_PAGE_TYPE_LAST= FIL_PAGE_TYPE_UNKNOWN;

/** Set in FIL_PAGE_TYPE for full_crc32 pages in page_compressed format.
If the flag is set, then the following holds for the remaining bits
of FIL_PAGE_TYPE:
Bits 0..7 will contain the compressed page size in bytes.
Bits 8..14 are reserved and must be 0. */
constexpr uint16_t FIL_PAGE_COMPRESS_FCRC32_MARKER= 15;
/* @} */

/** @return whether the page type is B-tree or R-tree index */
inline bool fil_page_type_is_index(uint16_t page_type)
{
	switch (page_type) {
	case FIL_PAGE_TYPE_INSTANT:
	case FIL_PAGE_INDEX:
	case FIL_PAGE_RTREE:
		return(true);
	}
	return(false);
}

/** Check whether the page is index page (either regular Btree index or Rtree
index */
#define fil_page_index_page_check(page)                         \
        fil_page_type_is_index(fil_page_get_type(page))

/** Enum values for encryption table option */
enum fil_encryption_t {
	/** Encrypted if innodb_encrypt_tables=ON (srv_encrypt_tables) */
	FIL_ENCRYPTION_DEFAULT,
	/** Encrypted */
	FIL_ENCRYPTION_ON,
	/** Not encrypted */
	FIL_ENCRYPTION_OFF
};

/** Get the file page type.
@param[in]	page	file page
@return page type */
inline uint16_t fil_page_get_type(const byte *page)
{
  return mach_read_from_2(my_assume_aligned<2>(page + FIL_PAGE_TYPE));
}

#ifndef UNIV_INNOCHECKSUM

/** Number of pending tablespace flushes */
extern ulint	fil_n_pending_tablespace_flushes;

/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
If this is not the case, fil_space_acquire() and fil_space_t::release()
should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or NULL if not found */
fil_space_t*
fil_space_get(
	ulint	id)
	MY_ATTRIBUTE((warn_unused_result));

/** The tablespace memory cache; also the totality of logs (the log
data space) is stored here; below we talk about tablespaces */
struct fil_system_t {
  /**
    Constructor.

    Some members may require late initialisation, thus we just mark object as
    uninitialised. Real initialisation happens in create().
  */
  fil_system_t(): m_initialised(false)
  {
    UT_LIST_INIT(LRU, &fil_node_t::LRU);
    UT_LIST_INIT(space_list, &fil_space_t::space_list);
    UT_LIST_INIT(named_spaces, &fil_space_t::named_spaces);
  }

  bool is_initialised() const { return m_initialised; }

  /**
    Create the file system interface at database start.

    @param[in] hash_size	hash table size
  */
  void create(ulint hash_size);

  /** Close the file system interface at shutdown */
  void close();

private:
  bool m_initialised;
#ifdef UNIV_LINUX
  /** available block devices that reside on non-rotational storage */
  std::vector<dev_t> ssd;
public:
  /** @return whether a file system device is on non-rotational storage */
  bool is_ssd(dev_t dev) const
  {
    /* Linux seems to allow up to 15 partitions per block device.
    If the detected ssd carries "partition number 0" (it is the whole device),
    compare the candidate file system number without the partition number. */
    for (const auto s : ssd)
      if (dev == s || (dev & ~15U) == s)
        return true;
    return false;
  }
#endif
public:
  /** Detach a tablespace from the cache and close the files. */
  inline void detach(fil_space_t *space);

	ib_mutex_t	mutex;		/*!< The mutex protecting the cache */
	fil_space_t*	sys_space;	/*!< The innodb_system tablespace */
	fil_space_t*	temp_space;	/*!< The innodb_temporary tablespace */
	hash_table_t*	spaces;		/*!< The hash table of spaces in the
					system; they are hashed on the space
					id */
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
	intrusive::list<fil_space_t, unflushed_spaces_tag_t> unflushed_spaces;
					/*!< list of those
					tablespaces whose files contain
					unflushed writes; those spaces have
					at least one file node where
					needs_flush == true */
	ulint		n_open;		/*!< number of files currently open */
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
					for which a FILE_MODIFY
					record has been written since
					the latest redo log checkpoint.
					Protected only by log_sys.mutex. */
	intrusive::list<fil_space_t, rotation_list_tag_t> rotation_list;
					/*!< list of all file spaces needing
					key rotation.*/

	bool		space_id_reuse_warned;
					/*!< whether fil_space_create()
					has issued a warning about
					potential space_id reuse */

	/** Trigger a call to fil_node_t::read_page0()
	@param[in]	id	tablespace identifier
	@return	tablespace
	@retval	NULL	if the tablespace does not exist or cannot be read */
	fil_space_t* read_page0(ulint id);

	/** Return the next fil_space_t from key rotation list.
	Once started, the caller must keep calling this until it returns NULL.
	fil_space_acquire() and fil_space_t::release() are invoked here, which
	blocks a concurrent operation from dropping the tablespace.
	@param[in]      prev_space      Previous tablespace or NULL to start
					from beginning of fil_system->rotation
					list
	@param[in]      recheck         recheck of the tablespace is needed or
					still encryption thread does write page0
					for it
	@param[in]	key_version	key version of the key state thread
	If NULL, use the first fil_space_t on fil_system->space_list.
	@return pointer to the next fil_space_t.
	@retval NULL if this was the last */
	fil_space_t* keyrotate_next(
		fil_space_t*	prev_space,
		bool		remove,
		uint		key_version);
};

/** The tablespace memory cache. */
extern fil_system_t	fil_system;

/** Update the data structures on I/O completion */
inline void fil_node_t::complete_io(bool write)
{
  ut_ad(mutex_own(&fil_system.mutex));

  if (write)
  {
    if (srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC)
    {
      /* We don't need to keep track of unflushed changes as user has
      explicitly disabled buffering. */
      ut_ad(!space->is_in_unflushed_spaces());
      ut_ad(!needs_flush);
    }
    else if (!space->is_stopping())
    {
      needs_flush= true;
      if (!space->is_in_unflushed_spaces())
        fil_system.unflushed_spaces.push_front(*space);
    }
  }

  switch (n_pending--) {
  case 0:
    ut_error;
  case 1:
    if (space->belongs_in_lru())
      /* The node must be put back to the LRU list */
      UT_LIST_ADD_FIRST(fil_system.LRU, this);
  }
}

#include "fil0crypt.h"

/** Returns the latch of a file space.
@param[in]	id	space id
@param[out]	flags	tablespace flags
@return latch protecting storage allocation */
rw_lock_t*
fil_space_get_latch(
	ulint	id,
	ulint*	flags);

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

/** Opens all system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer. */
void
fil_open_system_tablespace_files();
/*==========================================*/
/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
void
fil_close_all_files(void);
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
@retval	NULL if missing or being deleted */
fil_space_t* fil_space_acquire_low(ulint id, bool silent)
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

/** Acquire a tablespace for reading or writing a block,
when it could be dropped concurrently.
@param[in]	id	tablespace ID
@return	the tablespace
@retval	NULL if missing */
fil_space_t*
fil_space_acquire_for_io(ulint id);

/** Return the next fil_space_t.
Once started, the caller must keep calling this until it returns NULL.
fil_space_acquire() and fil_space_t::release() are invoked here which
blocks a concurrent operation from dropping the tablespace.
@param[in,out]	prev_space	Pointer to the previous fil_space_t.
If NULL, use the first fil_space_t on fil_system.space_list.
@return pointer to the next fil_space_t.
@retval NULL if this was the last  */
fil_space_t*
fil_space_next(
	fil_space_t*	prev_space)
	MY_ATTRIBUTE((warn_unused_result));

/** Return the next fil_space_t from key rotation list.
Once started, the caller must keep calling this until it returns NULL.
fil_space_acquire() and fil_space_t::release() are invoked here which
blocks a concurrent operation from dropping the tablespace.
@param[in,out]	prev_space	Pointer to the previous fil_space_t.
If NULL, use the first fil_space_t on fil_system.space_list.
@param[in]	remove		Whether to remove the previous tablespace from
				the rotation list
@return pointer to the next fil_space_t.
@retval NULL if this was the last*/
fil_space_t*
fil_space_keyrotate_next(fil_space_t* prev_space, bool remove)
	MY_ATTRIBUTE((warn_unused_result));

/********************************************************//**
Creates the database directory for a table if it does not exist yet. */
void
fil_create_directory_for_tablename(
/*===============================*/
	const char*	name);	/*!< in: name in the standard
				'databasename/tablename' format */
/** Replay a file rename operation if possible.
@param[in]	space_id	tablespace identifier
@param[in]	name		old file name
@param[in]	new_name	new file name
@return	whether the operation was successfully applied
(the name did not exist, or new_name did not exist and
name was successfully renamed to new_name)  */
bool
fil_op_replay_rename(
	ulint		space_id,
	const char*	name,
	const char*	new_name)
	MY_ATTRIBUTE((warn_unused_result));

/** Determine whether a table can be accessed in operations that are
not (necessarily) protected by meta-data locks.
(Rollback would generally be protected, but rollback of
FOREIGN KEY CASCADE/SET NULL is not protected by meta-data locks
but only by InnoDB table locks, which may be broken by
lock_remove_all_on_table().)
@param[in]	table	persistent table
checked @return whether the table is accessible */
bool fil_table_accessible(const dict_table_t* table)
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Delete a tablespace and associated .ibd file.
@param[in]	id		tablespace identifier
@param[in]	if_exists	whether to ignore missing tablespace
@return	DB_SUCCESS or error */
dberr_t fil_delete_tablespace(ulint id, bool if_exists= false);

/** Prepare to truncate an undo tablespace.
@param[in]	space_id	undo tablespace id
@return	the tablespace
@retval	NULL if the tablespace does not exist */
fil_space_t* fil_truncate_prepare(ulint space_id);

/** Close a single-table tablespace on failed IMPORT TABLESPACE.
The tablespace must be cached in the memory cache.
Free all pages used by the tablespace. */
void fil_close_tablespace(ulint id);

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
@param[out]	err		DB_SUCCESS or error code
@return	the created tablespace
@retval	NULL	on error */
fil_space_t*
fil_ibd_create(
	ulint		space_id,
	const char*	name,
	const char*	path,
	ulint		flags,
	ulint		size,
	fil_encryption_t mode,
	uint32_t	key_id,
	dberr_t*	err)
	MY_ATTRIBUTE((nonnull(2,8), warn_unused_result));

/** Try to adjust FSP_SPACE_FLAGS if they differ from the expectations.
(Typically when upgrading from MariaDB 10.1.0..10.1.20.)
@param[in,out]	space		tablespace
@param[in]	flags		desired tablespace flags */
void fsp_flags_try_adjust(fil_space_t* space, ulint flags);

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
@param[in]	tablename	table name
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
	dberr_t*		err = NULL)
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
/** Determine if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id		Tablespace ID
@param[in]	name		Tablespace name used in fil_space_create().
@param[in]	table_flags	table flags
@return the tablespace
@retval	NULL	if no matching tablespace exists in the memory cache */
fil_space_t*
fil_space_for_table_exists_in_mem(
	ulint		id,
	const char*	name,
	ulint		table_flags);

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool
fil_space_extend(
	fil_space_t*	space,
	ulint		size);

struct fil_io_t
{
  /** error code */
  dberr_t err;
  /** file; node->space->release_for_io() must follow fil_io(sync=true) call */
  fil_node_t *node;
};

/** Reads or writes data. This operation could be asynchronous (aio).

@param[in]	type		IO context
@param[in]	sync		true if synchronous aio is desired
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
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
@param[in]	ignore		whether to ignore errors
@param[in]	punch_hole	punch the hole to the file for page_compressed
				tablespace
@return status and file descriptor */
fil_io_t
fil_io(
	const IORequest&	type,
	bool			sync,
	const page_id_t		page_id,
	ulint			zip_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message,
	bool			ignore = false,
	bool			punch_hole = false);

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
possibly cached by the OS. */
void fil_flush_file_spaces();
/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */
bool fil_validate();
/*********************************************************************//**
Sets the file page type. */
void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type);	/*!< in: type */

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables. */
void
fil_delete_file(
/*============*/
	const char*	path);	/*!< in: filepath of the ibd tablespace */

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

/** Acquire the fil_system mutex. */
#define fil_system_enter()	mutex_enter(&fil_system.mutex)
/** Release the fil_system mutex. */
#define fil_system_exit()	mutex_exit(&fil_system.mutex)

/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
fil_space_t*
fil_space_get_by_id(
/*================*/
	ulint	id);	/*!< in: space id */

/** Note that a non-predefined persistent tablespace has been modified
by redo log.
@param[in,out]	space	tablespace */
void
fil_names_dirty(
	fil_space_t*	space);

/** Write FILE_MODIFY records when a non-predefined persistent
tablespace was modified for the first time since the latest
fil_names_clear().
@param[in,out]	space	tablespace */
void fil_names_dirty_and_write(fil_space_t* space);

/** Write FILE_MODIFY records if a persistent tablespace was modified
for the first time since the latest fil_names_clear().
@param[in,out]	space	tablespace
@param[in,out]	mtr	mini-transaction
@return whether any FILE_MODIFY record was written */
inline bool fil_names_write_if_was_clean(fil_space_t* space)
{
	ut_ad(log_mutex_own());

	if (space == NULL) {
		return(false);
	}

	const bool	was_clean = space->max_lsn == 0;
	ut_ad(space->max_lsn <= log_sys.get_lsn());
	space->max_lsn = log_sys.get_lsn();

	if (was_clean) {
		fil_names_dirty_and_write(space);
	}

	return(was_clean);
}

/** During crash recovery, open a tablespace if it had not been opened
yet, to get valid size and flags.
@param[in,out]	space	tablespace */
inline void fil_space_open_if_needed(fil_space_t* space)
{
	ut_ad(recv_recovery_is_on());

	if (space->size == 0) {
		/* Initially, size and flags will be set to 0,
		until the files are opened for the first time.
		fil_space_get_size() will open the file
		and adjust the size and flags. */
		ut_d(ulint size	=) fil_space_get_size(space->id);
		ut_ad(size == space->size);
	}
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
