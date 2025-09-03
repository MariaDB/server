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

/** The maximum size of a read or write request.

According to Linux "man 2 read" and "man 2 write" this applies to
both 32-bit and 64-bit systems.

On FreeBSD, the limit is close to the Linux one, INT_MAX.

On Microsoft Windows, the limit is UINT_MAX (4 GiB - 1).

On other systems, the limit typically is up to SSIZE_T_MAX. */
static constexpr unsigned os_file_request_size_max= 0x7ffff000;

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
  /** create a new file */
  OS_FILE_CREATE= 0,
  /** open an existing file */
  OS_FILE_OPEN,
  /** retry opening an existing file */
  OS_FILE_OPEN_RETRY,
  /** open a raw block device */
  OS_FILE_OPEN_RAW,

  /** do not display diagnostic messages */
  OS_FILE_ON_ERROR_SILENT= 4,

  OS_FILE_CREATE_SILENT= OS_FILE_CREATE | OS_FILE_ON_ERROR_SILENT,
  OS_FILE_OPEN_SILENT= OS_FILE_OPEN | OS_FILE_ON_ERROR_SILENT,
  OS_FILE_OPEN_RETRY_SILENT= OS_FILE_OPEN_RETRY | OS_FILE_ON_ERROR_SILENT
};

static const ulint OS_FILE_READ_ONLY = 333;
static const ulint OS_FILE_READ_WRITE = 444;

/** Used by MySQLBackup */
static const ulint OS_FILE_READ_ALLOW_DELETE = 555;

/* @} */

/** Types for file create @{ */
static constexpr ulint OS_DATA_FILE = 100;
static constexpr ulint OS_LOG_FILE = 101;
#if defined _WIN32 || defined O_DIRECT
static constexpr ulint OS_DATA_FILE_NO_O_DIRECT = 103;
#endif
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
    /** Asynchronous doublewritten page */
    WRITE_DBL= WRITE_ASYNC | 4,
    /** A doublewrite batch */
    DBLWR_BATCH= WRITE_ASYNC | 8,
    /** Write data and punch hole for the rest */
    PUNCH= WRITE_ASYNC | 16,
    /** Write doublewritten data and punch hole for the rest */
    PUNCH_DBL= PUNCH | 4,
    /** Zero out a range of bytes in fil_space_t::io() */
    PUNCH_RANGE= WRITE_SYNC | 32,
  };

  constexpr IORequest(buf_page_t *bpage, buf_tmp_buffer_t *slot,
                      fil_node_t *node, Type type) :
    bpage(bpage), slot(slot), node(node), type(type) {}

  constexpr IORequest(Type type= READ_SYNC, buf_page_t *bpage= nullptr,
                      buf_tmp_buffer_t *slot= nullptr) :
    bpage(bpage), slot(slot), type(type) {}

  bool is_read() const noexcept { return (type & READ_SYNC) != 0; }
  bool is_write() const noexcept { return (type & WRITE_SYNC) != 0; }
  bool is_async() const noexcept
  { return (type & (READ_SYNC ^ READ_ASYNC)) != 0; }
  bool is_doublewritten() const noexcept { return (type & 4) != 0; }

  /** Create a write request for the doublewrite buffer. */
  IORequest doublewritten() const noexcept
  {
    ut_ad(type == WRITE_ASYNC || type == PUNCH);
    return IORequest{bpage, slot, node, Type(type | 4)};
  }

  void write_complete(int io_error) const noexcept;
  void read_complete(int io_error) const noexcept;
  void fake_read_complete(os_offset_t offset) const noexcept;

  /** If requested, free storage space associated with a section of the file.
  @param off   byte offset from the start (SEEK_SET)
  @param len   size of the hole in bytes
  @return DB_SUCCESS or error code */
  dberr_t maybe_punch_hole(os_offset_t off, ulint len) noexcept
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
  dberr_t punch_hole(os_offset_t off, ulint len) const noexcept;

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
FILE *os_file_create_tmpfile() noexcept;

/**
This function attempts to create a directory named pathname. The new directory
gets default permissions. On Unix, the permissions are (0770 & ~umask). If the
directory exists already, nothing is done and the call succeeds, unless the
fail_if_exists arguments is true.

@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated
				as an error.
@return true if call succeeds, false on error */
bool os_file_create_directory(const char *pathname, bool fail_if_exists)
  noexcept;

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
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success) noexcept;

/** NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated string
@param[in]	create_mode	OS_FILE_CREATE or OS_FILE_OPEN
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
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success) noexcept
	MY_ATTRIBUTE((warn_unused_result));

#ifndef _WIN32 /* On Microsoft Windows, mandatory locking is used */
/** Obtain an exclusive lock on a file.
@param fd      file descriptor
@param name    file name
@return 0 on success */
int os_file_lock(int fd, const char *name) noexcept;
#endif

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	read_only	if true read only mode checks are enforced
@param[in]	success		true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	os_file_create_t create_mode,
	ulint		type,
	bool		read_only,
	bool*		success) noexcept
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
		state, key, op, name, nullptr);				\
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

# define os_file_create(key, name, create, type, read_only,	\
			success)					\
	pfs_os_file_create_func(key, name, create,	type,	\
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

# define os_file_read(type, file, buf, offset, n, o)			\
	pfs_os_file_read_func(type, file, buf, offset, n,o, __FILE__, __LINE__)

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
	os_file_create_t create_mode,
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
@param[in]	create_mode	OS_FILE_CREATE or OS_FILE_OPEN
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
	os_file_create_t create_mode,
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
	os_file_create_t create_mode,
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
# define os_file_create(key, name, create, type, read_only,	\
			success)					\
	os_file_create_func(name, create, type, read_only,	\
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

# define os_file_read(type, file, buf, offset, n, o)		\
	os_file_read_func(type, file, buf, offset, n, o)

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
os_file_size_t os_file_get_size(const char *filename) noexcept
	MY_ATTRIBUTE((warn_unused_result));

/** Determine the logical size of a file.
This may change the current write position of the file to the end of the file.
(Not currently a problem; InnoDB typically uses positioned I/O.)
@param file  handle to an open file
@return file size, in octets
@retval -1 on failure */
os_offset_t os_file_get_size(os_file_t file) noexcept
	MY_ATTRIBUTE((warn_unused_result));

/** Truncates a file at its current position.
@param[in/out]	file	file to be truncated
@return true if success */
bool os_file_set_eof(FILE *file) noexcept;

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
	bool		allow_shrink = false) noexcept;

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool os_file_flush_func(os_file_t file) noexcept;

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + OS_FILE_ERROR_MAX is returned.
@param[in]	report_all_errors	true if we want an error message
                                        printed of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
                                        to the log
@return error number, or OS error number + OS_FILE_ERROR_MAX */
ulint os_file_get_last_error(bool report_all_errors,
                             bool on_error_silent= false) noexcept;

/** NOTE! Use the corresponding macro os_file_read(), not directly this
function!
Requests a synchronous read operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[out]	o		number of bytes actually read
@return DB_SUCCESS if request was successful */
dberr_t
os_file_read_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o) noexcept
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
	ulint		size) noexcept;

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
	os_file_type_t* type) noexcept;

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
void os_file_make_data_dir_path(char *data_dir_path) noexcept;

/** Create all missing subdirectories along the given path.
@return DB_SUCCESS if OK, otherwise error code. */
dberr_t os_file_create_subdirs_if_needed(const char* path) noexcept;

#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR
/* Test the function os_file_get_parent_dir. */
void
unit_test_os_file_get_parent_dir() noexcept;
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */

/**
Initializes the asynchronous io system. */
int os_aio_init() noexcept;

/**
Frees the asynchronous io system. */
void os_aio_free() noexcept;

/** Submit a fake read request during crash recovery.
@param type   fake read request
@param offset additional context */
void os_fake_read(const IORequest &type, os_offset_t offset) noexcept;

/** Request a read or write.
@param type		I/O request
@param buf		buffer
@param offset		file offset
@param n		number of bytes
@retval DB_SUCCESS if request was queued successfully
@retval DB_IO_ERROR on I/O error */
dberr_t os_aio(const IORequest &type, void *buf, os_offset_t offset, size_t n)
  noexcept;

/** @return number of pending reads */
size_t os_aio_pending_reads() noexcept;
/** @return approximate number of pending reads */
size_t os_aio_pending_reads_approx() noexcept;
/** @return number of pending writes */
size_t os_aio_pending_writes() noexcept;
/** @return approximate number of pending writes */
size_t os_aio_pending_writes_approx() noexcept;

/** Wait until there are no pending asynchronous writes.
@param declare  whether the wait will be declared in tpool */
void os_aio_wait_until_no_pending_writes(bool declare) noexcept;

/** Wait until all pending asynchronous reads have completed.
@param declare  whether the wait will be declared in tpool */
void os_aio_wait_until_no_pending_reads(bool declare) noexcept;

/** Prints info of the aio arrays.
@param[in/out]	file		file where to print */
void os_aio_print(FILE *file) noexcept;

/** Refreshes the statistics used to print per-second averages. */
void os_aio_refresh_stats() noexcept;

/** Checks that all slots in the system have been freed, that is, there are
no pending io operations. */
bool os_aio_all_slots_free() noexcept;


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
	bool		read_only) noexcept;

#ifdef _WIN32

/**
Make file sparse, on Windows.

@param[in]	file  file handle
@param[in]	is_sparse if true, make file sparse,
			otherwise "unsparse" the file
@return true on success, false on error */
bool os_file_set_sparse_win32(os_file_t file, bool is_sparse = true) noexcept;

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
bool os_file_set_size(const char *pathname, os_file_t file, os_offset_t size)
  noexcept;

inline bool
os_file_set_size(const char* name, os_file_t file, os_offset_t size, bool)
  noexcept
{
  return os_file_set_size(name, file, size);
}
#else
/** Extend a file by appending NUL.
@param[in]	name	file name
@param[in]	file	file handle
@param[in]	size	desired file size
@param[in]	sparse	whether to create a sparse file with ftruncate()
@return	whether the operation succeeded */
bool os_file_set_size(const char *name, os_file_t file, os_offset_t size,
                      bool is_sparse= false) noexcept;
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
	os_offset_t	len) noexcept
	MY_ATTRIBUTE((warn_unused_result));

/* Determine if a path is an absolute path or not.
@param[in]	OS directory or file path to evaluate
@retval true if an absolute path
@retval false if a relative path */
inline bool is_absolute_path(const char *path) noexcept
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
