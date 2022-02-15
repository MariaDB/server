/***********************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2022, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

***********************************************************************/

/**************************************************//**
@file include/os0file.h
The interface to the operating system file io

Created 10/21/1995 Heikki Tuuri
*******************************************************/

#ifndef os0file_h
#define os0file_h

#include "fsp0types.h"
#include "tpool.h"
#include "my_counter.h"

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif /* !_WIN32 */

extern bool	os_has_said_disk_full;

/** File offset in bytes */
typedef ib_uint64_t os_offset_t;

class buf_tmp_buffer_t;

#ifdef _WIN32

/** We define always WIN_ASYNC_IO, and check at run-time whether
the OS actually supports it: Win 95 does not, NT does. */
# define WIN_ASYNC_IO

/** Use unbuffered I/O */
# define UNIV_NON_BUFFERED_IO

/** File handle */
typedef native_file_handle os_file_t;


#else /* _WIN32 */

/** File handle */
typedef int	os_file_t;

#endif /* _WIN32 */

static const os_file_t OS_FILE_CLOSED = IF_WIN(os_file_t(INVALID_HANDLE_VALUE),-1);

/** File descriptor with optional PERFORMANCE_SCHEMA instrumentation */
struct pfs_os_file_t
{
	/** Default constructor */
	pfs_os_file_t(os_file_t file = OS_FILE_CLOSED) : m_file(file)
#ifdef UNIV_PFS_IO
	, m_psi(NULL)
#endif
	{}

	/** The wrapped file handle */
	os_file_t   m_file;
#ifdef UNIV_PFS_IO
	/** PERFORMANCE_SCHEMA descriptor */
	struct PSI_file *m_psi;
#endif
	/** Implicit type conversion.
	@return the wrapped file handle */
	operator os_file_t() const { return m_file; }
	/** Assignment operator.
	@param[in]	file	file handle to be assigned */
	void operator=(os_file_t file) { m_file = file; }
	bool operator==(os_file_t file) const { return m_file == file; }
	bool operator!=(os_file_t file) const { return !(*this == file); }
#ifndef DBUG_OFF
	friend std::ostream& operator<<(std::ostream& os, pfs_os_file_t f){
		os << os_file_t(f);
		return os;
	}
#endif
};

/** Options for os_file_create_func @{ */
enum os_file_create_t {
	OS_FILE_OPEN = 51,		/*!< to open an existing file (if
					doesn't exist, error) */
	OS_FILE_CREATE,			/*!< to create new file (if
					exists, error) */
	OS_FILE_OVERWRITE,		/*!< to create a new file, if exists
					the overwrite old file */
	OS_FILE_OPEN_RAW,		/*!< to open a raw device or disk
					partition */
	OS_FILE_CREATE_PATH,		/*!< to create the directories */
	OS_FILE_OPEN_RETRY,		/*!< open with retry */

	/** Flags that can be combined with the above values. Please ensure
	that the above values stay below 128. */

	OS_FILE_ON_ERROR_NO_EXIT = 128,	/*!< do not exit on unknown errors */
	OS_FILE_ON_ERROR_SILENT = 256	/*!< don't print diagnostic messages to
					the log unless it is a fatal error,
					this flag is only used if
					ON_ERROR_NO_EXIT is set */
};

static const ulint OS_FILE_READ_ONLY = 333;
static const ulint OS_FILE_READ_WRITE = 444;

/** Used by MySQLBackup */
static const ulint OS_FILE_READ_ALLOW_DELETE = 555;

/* Options for file_create */
static const ulint OS_FILE_AIO = 61;
static const ulint OS_FILE_NORMAL = 62;
/* @} */

/** Types for file create @{ */
static const ulint OS_DATA_FILE = 100;
static const ulint OS_LOG_FILE = 101;
static const ulint OS_DATA_FILE_NO_O_DIRECT = 103;
/* @} */

/** Error codes from os_file_get_last_error @{ */
static const ulint OS_FILE_NAME_TOO_LONG = 36;
static const ulint OS_FILE_NOT_FOUND = 71;
static const ulint OS_FILE_DISK_FULL = 72;
static const ulint OS_FILE_ALREADY_EXISTS = 73;
static const ulint OS_FILE_PATH_ERROR = 74;

/** wait for OS aio resources to become available again */
static const ulint OS_FILE_AIO_RESOURCES_RESERVED = 75;

static const ulint OS_FILE_SHARING_VIOLATION = 76;
static const ulint OS_FILE_ERROR_NOT_SPECIFIED = 77;
static const ulint OS_FILE_INSUFFICIENT_RESOURCE = 78;
static const ulint OS_FILE_AIO_INTERRUPTED = 79;
static const ulint OS_FILE_OPERATION_ABORTED = 80;
static const ulint OS_FILE_ACCESS_VIOLATION = 81;
static const ulint OS_FILE_OPERATION_NOT_SUPPORTED = 125;
static const ulint OS_FILE_ERROR_MAX = 200;
/* @} */

/**
The I/O context that is passed down to the low level IO code */
class IORequest
{
public:
  enum Type
  {
    /** Synchronous read */
    READ_SYNC= 2,
    /** Asynchronous read; some errors will be ignored */
    READ_ASYNC= READ_SYNC | 1,
    /** Possibly partial read; only used with
    os_file_read_no_error_handling() */
    READ_MAYBE_PARTIAL= READ_SYNC | 4,
    /** Read for doublewrite buffer recovery */
    DBLWR_RECOVER= READ_SYNC | 8,
    /** Synchronous write */
    WRITE_SYNC= 16,
    /** Asynchronous write */
    WRITE_ASYNC= WRITE_SYNC | 1,
    /** A doublewrite batch */
    DBLWR_BATCH= WRITE_ASYNC | 8,
    /** Write data; evict the block on write completion */
    WRITE_LRU= WRITE_ASYNC | 32,
    /** Write data and punch hole for the rest */
    PUNCH= WRITE_ASYNC | 64,
    /** Write data and punch hole; evict the block on write completion */
    PUNCH_LRU= PUNCH | WRITE_LRU,
    /** Zero out a range of bytes in fil_space_t::io() */
    PUNCH_RANGE= WRITE_SYNC | 128,
  };

  constexpr IORequest(buf_page_t *bpage, buf_tmp_buffer_t *slot,
                      fil_node_t *node, Type type) :
    bpage(bpage), slot(slot), node(node), type(type) {}

  constexpr IORequest(Type type= READ_SYNC, buf_page_t *bpage= nullptr,
                      buf_tmp_buffer_t *slot= nullptr) :
    bpage(bpage), slot(slot), type(type) {}

  bool is_read() const { return (type & READ_SYNC) != 0; }
  bool is_write() const { return (type & WRITE_SYNC) != 0; }
  bool is_LRU() const { return (type & (WRITE_LRU ^ WRITE_ASYNC)) != 0; }
  bool is_async() const { return (type & (READ_SYNC ^ READ_ASYNC)) != 0; }

  /** If requested, free storage space associated with a section of the file.
  @param off   byte offset from the start (SEEK_SET)
  @param len   size of the hole in bytes
  @return DB_SUCCESS or error code */
  dberr_t maybe_punch_hole(os_offset_t off, ulint len)
  {
    return off && len && node && (type & (PUNCH ^ WRITE_ASYNC))
      ? punch_hole(off, len)
      : DB_SUCCESS;
  }

private:
  /** Free storage space associated with a section of the file.
  @param off   byte offset from the start (SEEK_SET)
  @param len   size of the hole in bytes
  @return DB_SUCCESS or error code */
  dberr_t punch_hole(os_offset_t off, ulint len) const;

public:
  /** Page to be written on write operation */
  buf_page_t *const bpage= nullptr;

  /** Memory to be used for encrypted or page_compressed pages */
  buf_tmp_buffer_t *const slot= nullptr;

  /** File descriptor */
  fil_node_t *const node= nullptr;

  /** Request type bit flags */
  const Type type;
};

constexpr IORequest IORequestRead(IORequest::READ_SYNC);
constexpr IORequest IORequestReadPartial(IORequest::READ_MAYBE_PARTIAL);
constexpr IORequest IORequestWrite(IORequest::WRITE_SYNC);

/** Sparse file size information. */
struct os_file_size_t {
	/** Total size of file in bytes */
	os_offset_t	m_total_size;

	/** If it is a sparse file then this is the number of bytes
	actually allocated for the file. */
	os_offset_t	m_alloc_size;
};

constexpr ulint OS_AIO_N_PENDING_IOS_PER_THREAD= 256;

extern Atomic_counter<ulint> os_n_file_reads;
extern Atomic_counter<size_t> os_n_file_writes;
extern Atomic_counter<size_t> os_n_fsyncs;

/* File types for directory entry data type */

enum os_file_type_t {
	OS_FILE_TYPE_UNKNOWN = 0,
	OS_FILE_TYPE_FILE,			/* regular file */
	OS_FILE_TYPE_DIR,			/* directory */
	OS_FILE_TYPE_LINK,			/* symbolic link */
	OS_FILE_TYPE_BLOCK			/* block device */
};

/* Maximum path string length in bytes when referring to tables with in the
'./databasename/tablename.ibd' path format; we can allocate at least 2 buffers
of this size from the thread stack; that is why this should not be made much
bigger than 4000 bytes.  The maximum path length used by any storage engine
in the server must be at least this big. */

/* MySQL 5.7 my_global.h */
#ifndef FN_REFLEN_SE
#define FN_REFLEN_SE        4000
#endif

#define OS_FILE_MAX_PATH	4000
#if (FN_REFLEN_SE < OS_FILE_MAX_PATH)
# error "(FN_REFLEN_SE < OS_FILE_MAX_PATH)"
#endif

/** Struct used in fetching information of a file in a directory */
struct os_file_stat_t {
	char		name[OS_FILE_MAX_PATH];	/*!< path to a file */
	os_file_type_t	type;			/*!< file type */
	os_offset_t	size;			/*!< file size in bytes */
	os_offset_t	alloc_size;		/*!< Allocated size for
						sparse files in bytes */
	size_t		block_size;		/*!< Block size to use for IO
						in bytes*/
	time_t		ctime;			/*!< creation time */
	time_t		mtime;			/*!< modification time */
	time_t		atime;			/*!< access time */
	bool		rw_perm;		/*!< true if can be opened
						in read-write mode. Only valid
						if type == OS_FILE_TYPE_FILE */
};

/** Create a temporary file. This function is like tmpfile(3), but
the temporary file is created in the in the mysql server configuration
parameter (--tmpdir).
@return temporary file handle, or NULL on error */
FILE*
os_file_create_tmpfile();

/**
This function attempts to create a directory named pathname. The new directory
gets default permissions. On Unix, the permissions are (0770 & ~umask). If the
directory exists already, nothing is done and the call succeeds, unless the
fail_if_exists arguments is true.

@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated
				as an error.
@return true if call succeeds, false on error */
bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists);

/** NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeed, false if error
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success);

/** NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option
				is used by a backup program reading the file
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_no_error_handling_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
	MY_ATTRIBUTE((warn_unused_result));

#ifdef  _WIN32
#define os_file_set_nocache(fd, file_name, operation_name) do{}while(0)
#else
/** Tries to disable OS caching on an opened file descriptor.
@param[in]	fd		file descriptor to alter
@param[in]	file_name	file name, used in the diagnostic message
@param[in]	name		"open" or "create"; used in the diagnostic
				message */
void
os_file_set_nocache(
/*================*/
	int	fd,		/*!< in: file descriptor to alter */
	const char*	file_name,
	const char*	operation_name);
#endif

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use
				async I/O or unbuffered I/O: look in the
				function source code for the exact rules
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	read_only	if true read only mode checks are enforced
@param[in]	success		true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success)
	MY_ATTRIBUTE((warn_unused_result));

/** Deletes a file. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@return true if success */
bool
os_file_delete_func(const char* name);

/** Deletes a file if it exists. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@param[out]	exist		indicate if file pre-exist
@return true if success */
bool
os_file_delete_if_exists_func(const char* name, bool* exist);

/** NOTE! Use the corresponding macro os_file_rename(), not directly
this function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@return true if success */
bool
os_file_rename_func(const char* oldpath, const char* newpath);

/** NOTE! Use the corresponding macro os_file_close(), not directly this
function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in]	file		own: handle to a file
@return true if success */
bool os_file_close_func(os_file_t file);

#ifdef UNIV_PFS_IO

/* Keys to register InnoDB I/O with performance schema */
extern mysql_pfs_key_t	innodb_data_file_key;
extern mysql_pfs_key_t	innodb_temp_file_key;

/* Following four macros are instumentations to register
various file I/O operations with performance schema.
1) register_pfs_file_open_begin() and register_pfs_file_open_end() are
used to register file creation, opening, closing and renaming.
2) register_pfs_file_rename_begin() and  register_pfs_file_rename_end()
are used to register file renaming
2) register_pfs_file_io_begin() and register_pfs_file_io_end() are
used to register actual file read, write and flush
3) register_pfs_file_close_begin() and register_pfs_file_close_end()
are used to register file deletion operations*/
# define register_pfs_file_open_begin(state, locker, key, op, name,	\
				      src_file, src_line)		\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_name_locker)(		\
		state, key, op, name, &locker);				\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_open_wait)(			\
			locker, src_file, src_line);			\
	}								\
} while (0)

# define register_pfs_file_open_end(locker, file, result)		\
do {									\
	if (locker != NULL) {						\
		file.m_psi = PSI_FILE_CALL(end_file_open_wait)(	\
			locker, result);				\
	}								\
} while (0)

# define register_pfs_file_rename_begin(state, locker, key, op, name,	\
				src_file, src_line)			\
	register_pfs_file_open_begin(state, locker, key, op, name,	\
					src_file, src_line)		\

# define register_pfs_file_rename_end(locker, from, to, result)		\
do {									\
	if (locker != NULL) {						\
		 PSI_FILE_CALL(						\
			end_file_rename_wait)(				\
			locker, from, to, result);			\
	}								\
} while (0)

# define register_pfs_file_close_begin(state, locker, key, op, name,	\
				      src_file, src_line)		\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_name_locker)(		\
		state, key, op, name, &locker);				\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_close_wait)(			\
			locker, src_file, src_line);			\
	}								\
} while (0)

# define register_pfs_file_close_end(locker, result)			\
do {									\
	if (locker != NULL) {						\
		PSI_FILE_CALL(end_file_close_wait)(			\
			locker, result);				\
	}								\
} while (0)

# define register_pfs_file_io_begin(state, locker, file, count, op,	\
				    src_file, src_line)			\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_stream_locker)(		\
		state, file.m_psi, op);					\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_wait)(				\
			locker, count, src_file, src_line);		\
	}								\
} while (0)

# define register_pfs_file_io_end(locker, count)			\
do {									\
	if (locker != NULL) {						\
		PSI_FILE_CALL(end_file_wait)(locker, count);		\
	}								\
} while (0)

/* Following macros/functions are file I/O APIs that would be performance
schema instrumented if "UNIV_PFS_IO" is defined. They would point to
wrapper functions with performance schema instrumentation in such case.

os_file_create
os_file_create_simple
os_file_create_simple_no_error_handling
os_file_close
os_file_rename
os_aio
os_file_read
os_file_read_no_error_handling
os_file_write

The wrapper functions have the prefix of "innodb_". */

# define os_file_create(key, name, create, purpose, type, read_only,	\
			success)					\
	pfs_os_file_create_func(key, name, create, purpose,	type,	\
		read_only, success, __FILE__, __LINE__)

# define os_file_create_simple(key, name, create, access,		\
		read_only, success)					\
	pfs_os_file_create_simple_func(key, name, create, access,	\
		read_only, success, __FILE__, __LINE__)

# define os_file_create_simple_no_error_handling(			\
	key, name, create_mode, access, read_only, success)		\
	pfs_os_file_create_simple_no_error_handling_func(		\
		key, name, create_mode, access,				\
		read_only, success, __FILE__, __LINE__)

# define os_file_close(file)						\
	pfs_os_file_close_func(file, __FILE__, __LINE__)

# define os_file_read(type, file, buf, offset, n)			\
	pfs_os_file_read_func(type, file, buf, offset, n, __FILE__, __LINE__)

# define os_file_read_no_error_handling(type, file, buf, offset, n, o)	\
	pfs_os_file_read_no_error_handling_func(			\
		type, file, buf, offset, n, o, __FILE__, __LINE__)

# define os_file_write(type, name, file, buf, offset, n)	\
	pfs_os_file_write_func(type, name, file, buf, offset,	\
			       n, __FILE__, __LINE__)

# define os_file_flush(file)					\
	pfs_os_file_flush_func(file, __FILE__, __LINE__)

# define os_file_rename(key, oldpath, newpath)				\
	pfs_os_file_rename_func(key, oldpath, newpath, __FILE__, __LINE__)

# define os_file_delete(key, name)					\
	pfs_os_file_delete_func(key, name, __FILE__, __LINE__)

# define os_file_delete_if_exists(key, name, exist)			\
	pfs_os_file_delete_if_exists_func(key, name, exist, __FILE__, __LINE__)

/** NOTE! Please use the corresponding macro os_file_create_simple(),
not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple() which opens or creates a file.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_simple_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
	MY_ATTRIBUTE((warn_unused_result));

/** NOTE! Please use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple_no_error_handling(). Add instrumentation to
monitor file creation/open.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option is
				used by a backup program reading the file
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_simple_no_error_handling_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
	MY_ATTRIBUTE((warn_unused_result));

/** NOTE! Please use the corresponding macro os_file_create(), not directly
this function!
A performance schema wrapper function for os_file_create().
Add instrumentation to monitor file creation/open.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use
				async I/O or unbuffered I/O: look in the
				function source code for the exact rules
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
	MY_ATTRIBUTE((warn_unused_result));

/** NOTE! Please use the corresponding macro os_file_close(), not directly
this function!
A performance schema instrumented wrapper function for os_file_close().
@param[in]	file		handle to a file
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_close_func(
	pfs_os_file_t	file,
	const char*	src_file,
	uint		src_line);

/** NOTE! Please use the corresponding macro os_file_read(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_read() which requests a synchronous read operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_read_func(
	const IORequest&	type,
	pfs_os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	const char*		src_file,
	uint			src_line);

/** NOTE! Please use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
This is the performance schema instrumented wrapper function for
os_file_read_no_error_handling_func() which requests a synchronous
read operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[out]	o		number of bytes actually read
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_read_no_error_handling_func(
	const IORequest&	type,
	pfs_os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o,
	const char*		src_file,
	uint			src_line);

/** NOTE! Please use the corresponding macro os_file_write(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_write() which requests a synchronous write operation.
@param[in]	type		IO request context
@param[in]	name		Name of the file or path as NUL terminated
				string
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_write_func(
	const IORequest&	type,
	const char*		name,
	pfs_os_file_t		file,
	const void*		buf,
	os_offset_t		offset,
	ulint			n,
	const char*		src_file,
	uint			src_line);

/** NOTE! Please use the corresponding macro os_file_flush(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_flush() which flushes the write buffers of a given file to the disk.
Flushes the write buffers of a given file to the disk.
@param[in]	file		Open file handle
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return TRUE if success */
UNIV_INLINE
bool
pfs_os_file_flush_func(
	pfs_os_file_t	file,
	const char*	src_file,
	uint		src_line);


/** NOTE! Please use the corresponding macro os_file_rename(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_rename()
@param[in]	key		Performance Schema Key
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_rename_func(
	mysql_pfs_key_t	key,
	const char*	oldpath,
	const char*	newpath,
	const char*	src_file,
	uint		src_line);

/**
NOTE! Please use the corresponding macro os_file_delete(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_delete()
@param[in]	key		Performance Schema Key
@param[in]	name		old file path as a null-terminated string
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_func(
	mysql_pfs_key_t	key,
	const char*	name,
	const char*	src_file,
	uint		src_line);

/**
NOTE! Please use the corresponding macro os_file_delete_if_exists(), not
directly this function!
This is the performance schema instrumented wrapper function for
os_file_delete_if_exists()
@param[in]	key		Performance Schema Key
@param[in]	name		old file path as a null-terminated string
@param[in]	exist		indicate if file pre-exist
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_if_exists_func(
	mysql_pfs_key_t	key,
	const char*	name,
	bool*		exist,
	const char*	src_file,
	uint		src_line);

#else /* UNIV_PFS_IO */

/* If UNIV_PFS_IO is not defined, these I/O APIs point
to original un-instrumented file I/O APIs */
# define os_file_create(key, name, create, purpose, type, read_only,	\
			success)					\
	os_file_create_func(name, create, purpose, type, read_only,	\
			success)

# define os_file_create_simple(key, name, create_mode, access,		\
		read_only, success)					\
	os_file_create_simple_func(name, create_mode, access,		\
		read_only, success)

# define os_file_create_simple_no_error_handling(			\
	key, name, create_mode, access, read_only, success)		\
	os_file_create_simple_no_error_handling_func(			\
		name, create_mode, access, read_only, success)

# define os_file_close(file)	os_file_close_func(file)

# define os_file_read(type, file, buf, offset, n)			\
	os_file_read_func(type, file, buf, offset, n)

# define os_file_read_no_error_handling(type, file, buf, offset, n, o)	\
	os_file_read_no_error_handling_func(type, file, buf, offset, n, o)

# define os_file_write(type, name, file, buf, offset, n)	\
	os_file_write_func(type, name, file, buf, offset, n)

# define os_file_flush(file)	os_file_flush_func(file)

# define os_file_rename(key, oldpath, newpath)				\
	os_file_rename_func(oldpath, newpath)

# define os_file_delete(key, name)	os_file_delete_func(name)

# define os_file_delete_if_exists(key, name, exist)			\
	os_file_delete_if_exists_func(name, exist)

#endif	/* UNIV_PFS_IO */

/** Gets a file size.
@param[in]	file		handle to a file
@return file size if OK, else set m_total_size to ~0 and m_alloc_size
	to errno */
os_file_size_t
os_file_get_size(
	const char*	filename)
	MY_ATTRIBUTE((warn_unused_result));

/** Gets a file size.
@param[in]	file		handle to a file
@return file size, or (os_offset_t) -1 on failure */
os_offset_t
os_file_get_size(
	os_file_t	file)
	MY_ATTRIBUTE((warn_unused_result));

/** Extend a file.

On Windows, extending a file allocates blocks for the file,
unless the file is sparse.

On Unix, we will extend the file with ftruncate(), if
file needs to be sparse. Otherwise posix_fallocate() is used
when available, and if not, binary zeroes are added to the end
of file.

@param[in]	name	file name
@param[in]	file	file handle
@param[in]	size	desired file size
@param[in]	sparse	whether to create a sparse file (no preallocating)
@return	whether the operation succeeded */
bool
os_file_set_size(
	const char*	name,
	os_file_t	file,
	os_offset_t	size,
	bool		is_sparse = false)
	MY_ATTRIBUTE((warn_unused_result));

/** Truncates a file at its current position.
@param[in/out]	file	file to be truncated
@return true if success */
bool
os_file_set_eof(
	FILE*		file);	/*!< in: file to be truncated */

/** Truncate a file to a specified size in bytes.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size preserved in bytes
@param[in]	allow_shrink	whether to allow the file to become smaller
@return true if success */
bool
os_file_truncate(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size,
	bool		allow_shrink = false);

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool
os_file_flush_func(
	os_file_t	file);

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in]	report		true if we want an error message printed
				for all errors
@return error number, or OS error number + 100 */
ulint
os_file_get_last_error(
	bool		report);

/** NOTE! Use the corresponding macro os_file_read(), not directly this
function!
Requests a synchronous read operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@return DB_SUCCESS if request was successful */
dberr_t
os_file_read_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n)
	MY_ATTRIBUTE((warn_unused_result));

/** Rewind file to its start, read at most size - 1 bytes from it to str, and
NUL-terminate str. All errors are silently ignored. This function is
mostly meant to be used with temporary files.
@param[in,out]	file		file to read from
@param[in,out]	str		buffer where to read
@param[in]	size		size of buffer */
void
os_file_read_string(
	FILE*		file,
	char*		str,
	ulint		size);

/** NOTE! Use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
Requests a synchronous positioned read operation. This function does not do
any error handling. In case of error it returns FALSE.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[out]	o		number of bytes actually read
@return DB_SUCCESS or error code */
dberr_t
os_file_read_no_error_handling_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o)
	MY_ATTRIBUTE((warn_unused_result));

/** NOTE! Use the corresponding macro os_file_write(), not directly this
function!
Requests a synchronous write operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@return DB_SUCCESS if request was successful */
dberr_t
os_file_write_func(
	const IORequest&	type,
	const char*		name,
	os_file_t		file,
	const void*		buf,
	os_offset_t		offset,
	ulint			n)
	MY_ATTRIBUTE((warn_unused_result));

/** Check the existence and type of the given file.
@param[in]	path		pathname of the file
@param[out]	exists		true if file exists
@param[out]	type		type of the file (if it exists)
@return true if call succeeded */
bool
os_file_status(
	const char*	path,
	bool*		exists,
	os_file_type_t* type);

/** This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@param[in]	old_path		pathname
@param[in]	new_name		new file name
@return own: new full pathname */
char*
os_file_make_new_pathname(
	const char*	old_path,
	const char*	new_name);

/** This function reduces a null-terminated full remote path name into
the path that is sent by MySQL for DATA DIRECTORY clause.  It replaces
the 'databasename/tablename.ibd' found at the end of the path with just
'tablename'.

Since the result is always smaller than the path sent in, no new memory
is allocated. The caller should allocate memory for the path sent in.
This function manipulates that path in place.

If the path format is not as expected, just return.  The result is used
to inform a SHOW CREATE TABLE command.
@param[in,out]	data_dir_path		Full path/data_dir_path */
void
os_file_make_data_dir_path(
	char*	data_dir_path);

/** Create all missing subdirectories along the given path.
@return DB_SUCCESS if OK, otherwise error code. */
dberr_t
os_file_create_subdirs_if_needed(
	const char*	path);

#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
/* Test the function os_file_get_parent_dir. */
void
unit_test_os_file_get_parent_dir();
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */

/**
Initializes the asynchronous io system. */
int os_aio_init();

/**
Frees the asynchronous io system. */
void os_aio_free();

/** Request a read or write.
@param type		I/O request
@param buf		buffer
@param offset		file offset
@param n		number of bytes
@retval DB_SUCCESS if request was queued successfully
@retval DB_IO_ERROR on I/O error */
dberr_t os_aio(const IORequest &type, void *buf, os_offset_t offset, size_t n);

/** Wait until there are no pending asynchronous writes. */
void os_aio_wait_until_no_pending_writes();

/** Wait until all pending asynchronous reads have completed. */
void os_aio_wait_until_no_pending_reads();

/** Prints info of the aio arrays.
@param[in/out]	file		file where to print */
void
os_aio_print(FILE* file);

/** Refreshes the statistics used to print per-second averages. */
void
os_aio_refresh_stats();

/** Checks that all slots in the system have been freed, that is, there are
no pending io operations. */
bool
os_aio_all_slots_free();


/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[in]	stat_info	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	if true read only mode checks are enforced
@return DB_SUCCESS if all OK */
dberr_t
os_file_get_status(
	const char*	path,
	os_file_stat_t* stat_info,
	bool		check_rw_perm,
	bool		read_only);

/** Set the file create umask
@param[in]	umask		The umask to use for file creation. */
void
os_file_set_umask(ulint umask);

#ifdef _WIN32

/**
Make file sparse, on Windows.

@param[in]	file  file handle
@param[in]	is_sparse if true, make file sparse,
			otherwise "unsparse" the file
@return true on success, false on error */
bool os_file_set_sparse_win32(os_file_t file, bool is_sparse = true);

/**
Changes file size on Windows

If file is extended, following happens  the bytes between
old and new EOF are zeros.

If file is sparse, "virtual" block is added at the end of
allocated area.

If file is normal, file system allocates storage.

@param[in]	pathname	file path
@param[in]	file		file handle
@param[in]	size		size to preserve in bytes
@return true if success */
bool
os_file_change_size_win32(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size);

#endif /*_WIN32 */

/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return DB_SUCCESS or error code */
dberr_t
os_file_punch_hole(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
	MY_ATTRIBUTE((warn_unused_result));

/* Determine if a path is an absolute path or not.
@param[in]	OS directory or file path to evaluate
@retval true if an absolute path
@retval false if a relative path */
inline bool is_absolute_path(const char *path)
{
  switch (path[0]) {
#ifdef _WIN32
  case '\0':
    return false;
  case '\\':
#endif
  case '/':
    return true;
  }

#ifdef _WIN32
  if (path[1] == ':')
  {
    switch (path[2]) {
    case '/':
    case '\\':
      return true;
    }
  }
#endif /* _WIN32 */

  return false;
}

#include "os0file.inl"

#endif /* os0file_h */
