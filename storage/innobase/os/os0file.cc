/***********************************************************************

Copyright (c) 1995, 2019, Oracle and/or its affiliates. All Rights Reserved.
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
@file os/os0file.cc
The interface to the operating system file i/o primitives

Created 10/21/1995 Heikki Tuuri
*******************************************************/

#include "os0file.h"
#include "sql_const.h"
#include "log.h"

#ifdef __linux__
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/sysmacros.h>
#endif

#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "buf0dblwr.h"

#include <tpool_structs.h>

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
# include <fcntl.h>
# include <linux/falloc.h>
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

#ifdef _WIN32
# include <winioctl.h>
#elif !defined O_DSYNC
# define O_DSYNC O_SYNC
#else
# include <unistd.h>
#endif

// my_test_if_atomic_write(), my_win_file_secattr()
#include <my_sys.h>

#include <thread>
#include <chrono>

/* Per-IO operation environment*/
class io_slots
{
private:
	tpool::cache<tpool::aiocb, true> m_cache;
	tpool::task_group m_group;
	int m_max_aio;
public:
	io_slots(int max_submitted_io, int max_callback_concurrency) :
		m_cache(max_submitted_io), m_group(max_callback_concurrency, false),
		m_max_aio(max_submitted_io)
	{
	}
	/* Get cached AIO control block */
	tpool::aiocb* acquire()
	{
		return m_cache.get();
	}
	/* Release AIO control block back to cache */
	void release(tpool::aiocb* aiocb)
	{
		m_cache.put(aiocb);
	}

	bool contains(tpool::aiocb* aiocb)
	{
		return m_cache.contains(aiocb);
	}

	/* Wait for completions of all AIO operations */
	void wait(std::unique_lock<std::mutex> &lk)
	{
		m_cache.wait(lk);
	}

	void wait()
	{
		m_cache.wait();
	}

	size_t pending_io_count()
	{
		return m_cache.pos();
	}

	std::chrono::duration<double> wait_time()
	{
		return m_cache.wait_time();
	}

	tpool::task_group* get_task_group()
	{
		return &m_group;
	}

	~io_slots()
	{
		wait();
	}

	std::mutex &mutex()
	{
		return m_cache.mutex();
	}

	void resize(int max_submitted_io, int max_callback_concurrency)
	{
		m_cache.resize(max_submitted_io);
		m_group.set_max_tasks(max_callback_concurrency);
		m_max_aio = max_submitted_io;
	}

	tpool::task_group& task_group()
	{
		return m_group;
	}
};

static io_slots *read_slots;
static io_slots *write_slots;

/**
  Statistics for asynchronous I/O
  @param op operation type (aio_opcode::AIO_PREAD or aio_opcode::AIO_PWRITE)
  @param stats pointer to the structure to fill
*/
void innodb_io_slots_stats(tpool::aio_opcode op, innodb_async_io_stats_t *stats)
{
   io_slots *slots= op == tpool::aio_opcode::AIO_PREAD? read_slots : write_slots;

   stats->pending_ops = slots->pending_io_count();
   stats->slot_wait_time_sec= slots->wait_time().count();
   slots->task_group().get_stats(&stats->completion_stats);
}

/** Number of retries for partial I/O's */
constexpr ulint NUM_RETRIES_ON_PARTIAL_IO = 10;

Atomic_counter<ulint> os_n_file_reads;
static ulint	os_bytes_read_since_printout;
Atomic_counter<size_t> os_n_file_writes;
Atomic_counter<size_t> os_n_fsyncs;
static ulint	os_n_file_reads_old;
static ulint	os_n_file_writes_old;
static ulint	os_n_fsyncs_old;

static time_t	os_last_printout;
bool	os_has_said_disk_full;

/** Default Zip compression level */
extern uint page_zip_level;

#ifdef UNIV_PFS_IO
/* Keys to register InnoDB I/O with performance schema */
mysql_pfs_key_t  innodb_data_file_key;
mysql_pfs_key_t  innodb_temp_file_key;
#endif

/** Handle errors for file operations.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation
@param[in]	should_abort	whether to abort on an unknown error
@param[in]	on_error_silent	whether to suppress reports of non-fatal errors
@return true if we should retry the operation */
static
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_abort,
	bool		on_error_silent) noexcept;

/** Does error handling when a file operation fails.
@param operation   name of operation that failed */
static void os_file_handle_error(const char *operation)
{
  os_file_handle_error_cond_exit(nullptr, operation, true, false);
}

/** Does error handling when a file operation fails.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation name that failed
@param[in]	on_error_silent	if true then don't print any message to the log.
@return true if we should retry the operation */
static
bool
os_file_handle_error_no_exit(
	const char*	name,
	const char*	operation,
	bool		on_error_silent)
{
	/* Don't exit in case of unknown error */
	return(os_file_handle_error_cond_exit(
			name, operation, false, on_error_silent));
}

/** Handle RENAME error.
@param name	old name of the file
@param new_name	new name of the file */
static void os_file_handle_rename_error(const char* name, const char* new_name)
{
	if (os_file_get_last_error(true) != OS_FILE_DISK_FULL) {
		ib::error() << "Cannot rename file '" << name << "' to '"
			<< new_name << "'";
	} else if (!os_has_said_disk_full) {
		os_has_said_disk_full = true;
		/* Disk full error is reported irrespective of the
		on_error_silent setting. */
		ib::error() << "Full disk prevents renaming file '"
			<< name << "' to '" << new_name << "'";
	}
}


#ifdef _WIN32

/**
 Wrapper around Windows DeviceIoControl() function.

 Works synchronously, also in case for handle opened
 for async access (i.e with FILE_FLAG_OVERLAPPED).

 Accepts the same parameters as DeviceIoControl(),except
 last parameter (OVERLAPPED).
*/
static
BOOL
os_win32_device_io_control(
	HANDLE handle,
	DWORD code,
	LPVOID inbuf,
	DWORD inbuf_size,
	LPVOID outbuf,
	DWORD outbuf_size,
	LPDWORD bytes_returned
)
{
	OVERLAPPED overlapped = { 0 };
	overlapped.hEvent = tpool::win_get_syncio_event();
	BOOL result = DeviceIoControl(handle, code, inbuf, inbuf_size, outbuf,
		outbuf_size,  NULL, &overlapped);

	if (result || (GetLastError() == ERROR_IO_PENDING)) {
		/* Wait for async io to complete */
		result = GetOverlappedResult(handle, &overlapped, bytes_returned, TRUE);
	}

	return result;
}

#endif



/** Helper class for doing synchronous file IO. Currently, the objective
is to hide the OS specific code, so that the higher level functions aren't
peppered with #ifdef. Makes the code flow difficult to follow.  */
class SyncFileIO
{
public:
  /** Constructor
  @param[in]     fh     File handle
  @param[in,out] buf    Buffer to read/write
  @param[in]     n      Number of bytes to read/write
  @param[in]     offset Offset where to read or write */
  SyncFileIO(os_file_t fh, void *buf, ulint n, os_offset_t offset) :
    m_fh(fh), m_buf(buf), m_n(static_cast<ssize_t>(n)), m_offset(offset)
  { ut_ad(m_n > 0); }

  /** Do the read/write
  @param[in]	request	The IO context and type
  @return the number of bytes read/written or negative value on error */
  ssize_t execute(const IORequest &request);

  /** Move the read/write offset up to where the partial IO succeeded.
  @param[in]	n_bytes	The number of bytes to advance */
  void advance(ssize_t n_bytes)
  {
    m_offset+= n_bytes;
    ut_ad(m_n >= n_bytes);
    m_n-= n_bytes;
    m_buf= reinterpret_cast<uchar*>(m_buf) + n_bytes;
  }

private:
  /** Open file handle */
  const os_file_t m_fh;
  /** Buffer to read/write */
  void *m_buf;
  /** Number of bytes to read/write */
  ssize_t m_n;
  /** Offset from where to read/write */
  os_offset_t m_offset;

  /** Do the read/write
  @param request The IO context and type
  @param n       Number of bytes to read/write
  @return the number of bytes read/written or negative value on error */
  ssize_t execute_low(const IORequest& request, ssize_t n);
};

#ifndef _WIN32 /* On Microsoft Windows, mandatory locking is used */
/** Obtain an exclusive lock on a file.
@param fd      file descriptor
@param name    file name
@return 0 on success */
int os_file_lock(int fd, const char *name) noexcept
{
	struct flock lk;

	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = lk.l_len = 0;

	if (fcntl(fd, F_SETLK, &lk) == -1) {

		ib::error()
			<< "Unable to lock " << name
			<< " error: " << errno;

		if (errno == EAGAIN || errno == EACCES) {

			ib::info()
				<< "Check that you do not already have"
				" another mariadbd process using the"
				" same InnoDB data or log files.";
		}

		return(-1);
	}

	return(0);
}
#endif /* !_WIN32 */

FILE *os_file_create_tmpfile() noexcept
{
	FILE*	file	= NULL;
	File	fd	= mysql_tmpfile("ib");

	if (fd >= 0) {
		file = my_fdopen(fd, 0, O_RDWR|O_TRUNC|O_CREAT|FILE_BINARY,
				 MYF(MY_WME));
		if (!file) {
			my_close(fd, MYF(MY_WME));
		}
	}

	if (file == NULL) {

		ib::error()
			<< "Unable to create temporary file; errno: "
			<< errno;
	}

	return(file);
}

/** Rewind file to its start, read at most size - 1 bytes from it to str, and
NUL-terminate str. All errors are silently ignored. This function is
mostly meant to be used with temporary files.
@param[in,out]	file		File to read from
@param[in,out]	str		Buffer where to read
@param[in]	size		Size of buffer */
void
os_file_read_string(
	FILE*		file,
	char*		str,
	ulint		size) noexcept
{
	if (size != 0) {
		rewind(file);

		size_t	flen = fread(str, 1, size - 1, file);

		str[flen] = '\0';
	}
}

void os_file_make_data_dir_path(char *data_dir_path) noexcept
{
	/* Replace the period before the extension with a null byte. */
	char*	ptr = strrchr(data_dir_path, '.');

	if (ptr == NULL) {
		return;
	}

	ptr[0] = '\0';

	/* The tablename starts after the last slash. */
	ptr = strrchr(data_dir_path, '/');


	if (ptr == NULL) {
		return;
	}

	ptr[0] = '\0';

	char*	tablename = ptr + 1;

	/* The databasename starts after the next to last slash. */
	ptr = strrchr(data_dir_path, '/');
#ifdef _WIN32
	if (char *aptr = strrchr(data_dir_path, '\\')) {
		if (aptr > ptr) {
			ptr = aptr;
		}
	}
#endif

	if (ptr == NULL) {
		return;
	}

	ulint	tablename_len = strlen(tablename);

	memmove(++ptr, tablename, tablename_len);

	ptr[tablename_len] = '\0';
}

/** Check if the path refers to the root of a drive using a pointer
to the last directory separator that the caller has fixed.
@param[in]	path	path name
@param[in]	path	last directory separator in the path
@return true if this path is a drive root, false if not */
static bool os_file_is_root(const char *path, const char *last_slash) noexcept
{
	return(
#ifdef _WIN32
	       (last_slash == path + 2 && path[1] == ':') ||
#endif /* _WIN32 */
	       last_slash == path);
}

/** Return the parent directory component of a null-terminated path.
Return a new buffer containing the string up to, but not including,
the final component of the path.
The path returned will not contain a trailing separator.
Do not return a root path, return NULL instead.
The final component trimmed off may be a filename or a directory name.
If the final component is the only component of the path, return NULL.
It is the caller's responsibility to free the returned string after it
is no longer needed.
@param[in]	path		Path name
@return own: parent directory of the path */
static
char*
os_file_get_parent_dir(
	const char*	path)
{
	/* Find the offset of the last slash */
	const char* last_slash = strrchr(path, '/');

#ifdef _WIN32
	if (const char *last = strrchr(path, '\\')) {
		if (last > last_slash) {
			last_slash = last;
		}
	}
#endif

	if (!last_slash) {
		/* No slash in the path, return NULL */
		return(NULL);
	}

	/* Ok, there is a slash. Is there anything after it? */
	const bool has_trailing_slash = last_slash[1] == '\0';

	/* Reduce repetitive slashes. */
	while (last_slash > path
	       && (IF_WIN(last_slash[-1] == '\\' ||,) last_slash[-1] == '/')) {
		last_slash--;
	}

	/* Check for the root of a drive. */
	if (os_file_is_root(path, last_slash)) {
		return(NULL);
	}

	/* If a trailing slash prevented the first strrchr() from trimming
	the last component of the path, trim that component now. */
	if (has_trailing_slash) {
		/* Back up to the previous slash. */
		last_slash--;
		while (last_slash > path
		       && (IF_WIN(last_slash[0] != '\\' &&,)
			   last_slash[0] != '/')) {
			last_slash--;
		}

		/* Reduce repetitive slashes. */
		while (last_slash > path
		       && (IF_WIN(last_slash[-1] == '\\' ||,)
			   last_slash[-1] == '/')) {
			last_slash--;
		}
	}

	/* Check for the root of a drive. */
	if (os_file_is_root(path, last_slash)) {
		return(NULL);
	}

	if (last_slash - path < 0) {
		/* Sanity check, it prevents gcc from trying to handle this case which
		 * results in warnings for some optimized builds */
		return (NULL);
	}

	/* Non-trivial directory component */

	return(mem_strdupl(path, ulint(last_slash - path)));
}
#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR

/* Test the function os_file_get_parent_dir. */
void
test_os_file_get_parent_dir(
	const char*	child_dir,
	const char*	expected_dir) noexcept
{
	char* child = mem_strdup(child_dir);
	char* expected = expected_dir == NULL ? NULL
			 : mem_strdup(expected_dir);

	char* parent = os_file_get_parent_dir(child);

	bool unexpected = (expected == NULL
			  ? (parent != NULL)
			  : (0 != strcmp(parent, expected)));
	if (unexpected) {
		ib::fatal() << "os_file_get_parent_dir('" << child
			<< "') returned '" << parent
			<< "', instead of '" << expected << "'.";
	}
	ut_free(parent);
	ut_free(child);
	ut_free(expected);
}

/* Test the function os_file_get_parent_dir. */
void
unit_test_os_file_get_parent_dir() noexcept
{
	test_os_file_get_parent_dir("/usr/lib/a", "/usr/lib");
	test_os_file_get_parent_dir("/usr/", NULL);
	test_os_file_get_parent_dir("//usr//", NULL);
	test_os_file_get_parent_dir("usr", NULL);
	test_os_file_get_parent_dir("usr//", NULL);
	test_os_file_get_parent_dir("/", NULL);
	test_os_file_get_parent_dir("//", NULL);
	test_os_file_get_parent_dir(".", NULL);
	test_os_file_get_parent_dir("..", NULL);
# ifdef _WIN32
	test_os_file_get_parent_dir("D:", NULL);
	test_os_file_get_parent_dir("D:/", NULL);
	test_os_file_get_parent_dir("D:\\", NULL);
	test_os_file_get_parent_dir("D:/data", NULL);
	test_os_file_get_parent_dir("D:/data/", NULL);
	test_os_file_get_parent_dir("D:\\data\\", NULL);
	test_os_file_get_parent_dir("D:///data/////", NULL);
	test_os_file_get_parent_dir("D:\\\\\\data\\\\\\\\", NULL);
	test_os_file_get_parent_dir("D:/data//a", "D:/data");
	test_os_file_get_parent_dir("D:\\data\\\\a", "D:\\data");
	test_os_file_get_parent_dir("D:///data//a///b/", "D:///data//a");
	test_os_file_get_parent_dir("D:\\\\\\data\\\\a\\\\\\b\\", "D:\\\\\\data\\\\a");
#endif  /* _WIN32 */
}
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */


dberr_t os_file_create_subdirs_if_needed(const char *path) noexcept
{
	if (srv_read_only_mode) {

		ib::error()
			<< "read only mode set. Can't create "
			<< "subdirectories '" << path << "'";

		return(DB_READ_ONLY);

	}

	char*	subdir = os_file_get_parent_dir(path);

	if (subdir == NULL) {
		/* subdir is root or cwd, nothing to do */
		return(DB_SUCCESS);
	}

	/* Test if subdir exists */
	os_file_type_t	type;
	bool	subdir_exists;
	bool	success = os_file_status(subdir, &subdir_exists, &type);

	if (success && !subdir_exists) {

		/* Subdir does not exist, create it */
		dberr_t	err = os_file_create_subdirs_if_needed(subdir);

		if (err != DB_SUCCESS) {

			ut_free(subdir);

			return(err);
		}

		success = os_file_create_directory(subdir, false);
	}

	ut_free(subdir);

	return(success ? DB_SUCCESS : DB_ERROR);
}



/** Do the read/write
@param[in]	request	The IO context and type
@param[in]      n       Number of bytes to read/write
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute_low(const IORequest& request, ssize_t n)
{
  ut_ad(n > 0);
  ut_ad(size_t(n) <= os_file_request_size_max);

  if (request.is_read())
    return IF_WIN(tpool::pread(m_fh, m_buf, n, m_offset), pread(m_fh, m_buf, n, m_offset));
  return IF_WIN(tpool::pwrite(m_fh, m_buf, n, m_offset), pwrite(m_fh, m_buf, n, m_offset));
}

/** Do the read/write
@param[in]	request	The IO context and type
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute(const IORequest& request)
{
  ssize_t n_bytes= 0;
  ut_ad(m_n > 0);

  while (size_t(m_n) > os_file_request_size_max)
  {
    ssize_t n_partial_bytes= execute_low(request, os_file_request_size_max);
    if (n_partial_bytes < 0)
      return n_partial_bytes;
    n_bytes+= n_partial_bytes;
    if (n_partial_bytes != os_file_request_size_max)
      return n_bytes;
    advance(os_file_request_size_max);
  }

  if (ssize_t n= execute_low(request, m_n))
  {
    if (n < 0)
      return n;
    n_bytes += n;
  }
  return n_bytes;
}

#ifndef _WIN32
/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return DB_SUCCESS or error code */
static
dberr_t
os_file_punch_hole_posix(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
{

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
	const int	mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

	int		ret = fallocate(fh, mode, off, len);

	if (ret == 0) {
		return(DB_SUCCESS);
	}

	if (errno == ENOTSUP) {
		return(DB_IO_NO_PUNCH_HOLE);
	}

	ib::warn()
		<< "fallocate("
		<<", FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, "
		<< off << ", " << len << ") returned errno: "
		<<  errno;

	return(DB_IO_ERROR);

#elif defined __sun__

	// Use F_FREESP

#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

	return(DB_IO_NO_PUNCH_HOLE);
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in]	report_all_errors	true if we want an error message
                                        printed of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
					to the log
@return error number, or OS error number + 100 */
ulint os_file_get_last_error(bool report_all_errors, bool on_error_silent)
  noexcept
{
	int	err = errno;

	if (err == 0) {
		return(0);
	}

	if (report_all_errors
	    || (err != ENOSPC && err != EEXIST && err != ENOENT
		&& !on_error_silent)) {

		ib::error()
			<< "Operating system error number "
			<< err
			<< " in a file operation.";

		if (err == EACCES) {

			ib::error()
				<< "The error means mariadbd does not have"
				" the access rights to the directory.";

		} else {
			if (strerror(err) != NULL) {

				ib::error()
					<< "Error number " << err << " means '"
					<< strerror(err) << "'";
			}

			ib::info() << OPERATING_SYSTEM_ERROR_MSG;
		}
	}

	switch (err) {
	case ENOSPC:
		return(OS_FILE_DISK_FULL);
	case ENOENT:
		return(OS_FILE_NOT_FOUND);
	case EEXIST:
		return(OS_FILE_ALREADY_EXISTS);
	case EXDEV:
	case ENOTDIR:
	case EISDIR:
	case EPERM:
		return(OS_FILE_PATH_ERROR);
	case EAGAIN:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_RESOURCES_RESERVED);
		}
		break;
	case EINTR:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_INTERRUPTED);
		}
		break;
	case EACCES:
		return(OS_FILE_ACCESS_VIOLATION);
	}
	return(OS_FILE_ERROR_MAX + err);
}

/** Wrapper to fsync() or fdatasync() that retries the call on some errors.
Returns the value 0 if successful; otherwise the value -1 is returned and
the global variable errno is set to indicate the error.
@param[in]	file		open file handle
@return 0 if success, -1 otherwise */
static int os_file_sync_posix(os_file_t file) noexcept
{
#if !defined(HAVE_FDATASYNC) || HAVE_DECL_FDATASYNC == 0
  auto func= fsync;
  auto func_name= "fsync()";
#else
  auto func= fdatasync;
  auto func_name= "fdatasync()";
#endif

  ulint failures= 0;

  for (;;)
  {
    ++os_n_fsyncs;

    int ret= func(file);

    if (ret == 0)
      return ret;

    switch (errno)
    {
    case ENOLCK:
      ++failures;
      ut_a(failures < 1000);

      if (!(failures % 100))
        ib::warn() << func_name << ": No locks available; retrying";

      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      break;

    case EINTR:
      ++failures;
      ut_a(failures < 2000);
      break;

    default:
      ib::fatal() << func_name << " returned " << errno;
    }
  }
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
static
bool
os_file_status_posix(
	const char*	path,
	bool*		exists,
	os_file_type_t* type) noexcept
{
	struct stat	statinfo;

	int	ret = stat(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */
	} else if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (S_ISDIR(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_DIR;

	} else if (S_ISLNK(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_LINK;

	} else if (S_ISREG(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_FILE;
	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

bool os_file_flush_func(os_file_t file) noexcept
{
	if (UNIV_UNLIKELY(my_disable_sync)) return true;

	int	ret;

	ret = os_file_sync_posix(file);

	if (ret == 0) {
		return(true);
	}

	/* Since Linux returns EINVAL if the 'file' is actually a raw device,
	we choose to ignore that error if we are using raw disks */

	if (srv_start_raw_disk_in_use && errno == EINVAL) {

		return(true);
	}

	ib::error() << "The OS said file flush did not succeed";

	os_file_handle_error("flush");

	/* It is a fatal error if a file flush does not succeed, because then
	the database can get corrupt on disk */
	ut_error;

	return(false);
}

/** NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true, read only checks are enforced
@param[out]	success		true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_func(
	const char*	name,
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success) noexcept
{
	pfs_os_file_t	file;

	*success = false;

	int create_flag = O_RDONLY | O_CLOEXEC;

	if (read_only) {
	} else if (create_mode == OS_FILE_CREATE) {
		create_flag = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
	} else {
		ut_ad(create_mode == OS_FILE_OPEN);
		if (access_type != OS_FILE_READ_ONLY) {
			create_flag = O_RDWR | O_CLOEXEC;
		}
	}

	if (fil_system.is_write_through()) create_flag |= O_DSYNC;
#ifdef O_DIRECT
	int direct_flag = fil_system.is_buffered() ? 0 : O_DIRECT;
#else
	constexpr int direct_flag = 0;
#endif

	for (;;) {
		file = open(name, create_flag | direct_flag, my_umask);

		if (file == -1) {
#ifdef O_DIRECT
			if (direct_flag && errno == EINVAL) {
				direct_flag = 0;
				continue;
			}
#endif

			if (!os_file_handle_error_no_exit(
				    name,
				    create_mode == OS_FILE_CREATE
				    ? "create" : "open", false)) {
				break;
			}
		} else {
			*success = true;
			break;
		}
	}

	if (!read_only
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && !my_disable_locking
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;
	}

	return(file);
}

bool os_file_create_directory(const char *pathname, bool fail_if_exists)
  noexcept
{
	int	rcode;

	rcode = mkdir(pathname, 0770);

	if (!(rcode == 0 || (errno == EEXIST && !fail_if_exists))) {
		/* failure */
		os_file_handle_error_no_exit(pathname, "mkdir", false);

		return(false);
	}

	return(true);
}

#ifdef O_DIRECT
# ifdef __linux__
/** Note that the log file uses buffered I/O. */
static ATTRIBUTE_COLD void os_file_log_buffered()
{
  log_sys.log_maybe_unbuffered= false;
  log_sys.log_buffered= true;
}

/** @return whether the log file may work with unbuffered I/O. */
static ATTRIBUTE_COLD bool os_file_log_maybe_unbuffered(const struct stat &st)
{
  char b[20 + sizeof "/sys/dev/block/" ":" "/../queue/physical_block_size"];
  if (snprintf(b, sizeof b, "/sys/dev/block/%u:%u/queue/physical_block_size",
               major(st.st_dev), minor(st.st_dev)) >=
      static_cast<int>(sizeof b))
    return false;
  int f= open(b, O_RDONLY);
  if (f == -1)
  {
    if (snprintf(b, sizeof b, "/sys/dev/block/%u:%u/../queue/"
                 "physical_block_size",
                 major(st.st_dev), minor(st.st_dev)) >=
        static_cast<int>(sizeof b))
      return false;
    f= open(b, O_RDONLY);
  }
  unsigned long s= 0;
  if (f != -1)
  {
    ssize_t l= read(f, b, sizeof b);
    if (l > 0 && size_t(l) < sizeof b && b[l - 1] == '\n')
    {
      char *end= b;
      s= strtoul(b, &end, 10);
      if (b == end || *end != '\n')
        s = 0;
    }
    close(f);
  }
  if (s > 4096 || s < 64 || !ut_is_2pow(s))
    return false;
  log_sys.set_block_size(uint32_t(s));

  return !(st.st_size & (s - 1));
}
# endif /* __linux__ */
#endif /* O_DIRECT */

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	read_only	true, if read only checks should be enforcedm
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	os_file_create_t create_mode,
	ulint		type,
	bool		read_only,
	bool*		success) noexcept
{
	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		errno = ENOSPC;
		return(OS_FILE_CLOSED);
	);

	int create_flag;

	if (read_only) {
		create_flag = O_RDONLY | O_CLOEXEC;
	} else if (create_mode == OS_FILE_CREATE
		   || create_mode == OS_FILE_CREATE_SILENT) {
		create_flag = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
	} else {
		ut_ad(create_mode == OS_FILE_OPEN
		      || create_mode == OS_FILE_OPEN_SILENT
		      || create_mode == OS_FILE_OPEN_RETRY
		      || create_mode == OS_FILE_OPEN_RETRY_SILENT
		      || create_mode == OS_FILE_OPEN_RAW);
		create_flag = O_RDWR | O_CLOEXEC;
	}

#ifdef O_DIRECT
# ifdef __linux__
	struct stat st;
# endif
	ut_a(type == OS_LOG_FILE
	     || type == OS_DATA_FILE || type == OS_DATA_FILE_NO_O_DIRECT);
	int direct_flag = 0;

	if (type == OS_DATA_FILE) {
		if (!fil_system.is_buffered()) {
			direct_flag = O_DIRECT;
		}
# ifdef __linux__
	} else if (type == OS_LOG_FILE && create_mode != OS_FILE_CREATE
		   && create_mode != OS_FILE_CREATE_SILENT
		   && !log_sys.is_opened()) {
		if (stat(name, &st)) {
			if (errno == ENOENT) {
				goto not_found;
			}
			log_sys.set_block_size(512);
			goto skip_o_direct;
		} else if (!os_file_log_maybe_unbuffered(st)
                           || log_sys.log_buffered) {
skip_o_direct:
			os_file_log_buffered();
		} else {
			direct_flag = O_DIRECT;
			log_sys.log_maybe_unbuffered = true;
		}
# endif
	}
#else
	ut_a(type == OS_LOG_FILE || type == OS_DATA_FILE);
	constexpr int direct_flag = 0;
#endif

	if (read_only) {
	} else if (type == OS_LOG_FILE
		   ? log_sys.log_write_through
		   : fil_system.is_write_through()) {
		create_flag |= O_DSYNC;
	}

	os_file_t	file;

	for (;;) {
		file = open(name, create_flag | direct_flag, my_umask);

		if (file == -1) {
#ifdef O_DIRECT
			if (direct_flag && errno == EINVAL) {
				direct_flag = 0;
# ifdef __linux__
				if (type == OS_LOG_FILE) {
					os_file_log_buffered();
				}
# endif
				if (create_mode == OS_FILE_CREATE
				    || create_mode == OS_FILE_CREATE_SILENT) {
					/* Linux may create the file
					before rejecting the O_DIRECT. */
					unlink(name);
				}
				continue;
			}
# ifdef __linux__
not_found:
# endif
#endif
			if (os_file_handle_error_no_exit(
				    name, (create_flag & O_CREAT)
				    ? "create" : "open",
				    create_mode & OS_FILE_ON_ERROR_SILENT)) {
				continue;
			}

			return OS_FILE_CLOSED;
		} else {
			*success = true;
			break;
		}
	}

#ifdef __linux__
	if ((create_flag & O_CREAT) && type == OS_LOG_FILE) {
		if (fstat(file, &st) || !os_file_log_maybe_unbuffered(st)) {
			os_file_log_buffered();
		} else {
			close(file);
			return os_file_create_func(name, OS_FILE_OPEN,
						   type, false, success);
		}
	}
#endif

	if (!read_only
	    && create_mode != OS_FILE_OPEN_RAW
	    && !my_disable_locking
	    && os_file_lock(file, name)) {

		if (create_mode == OS_FILE_OPEN_RETRY
		    || create_mode == OS_FILE_OPEN_RETRY_SILENT) {
			ib::info()
				<< "Retrying to lock the first data file";

			for (int i = 0; i < 100; i++) {
				std::this_thread::sleep_for(
					std::chrono::seconds(1));

				if (!os_file_lock(file, name)) {
					*success = true;
					return(file);
				}
			}

			ib::info()
				<< "Unable to open the first data file";
		}

		*success = false;
		close(file);
		file = -1;
	}

	return(file);
}

/** NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
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
{
	os_file_t	file;
	int		create_flag = O_RDONLY | O_CLOEXEC;

	*success = false;

	if (read_only) {
	} else if (create_mode == OS_FILE_CREATE) {
		create_flag = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
	} else {
		ut_ad(create_mode == OS_FILE_OPEN);
		if (access_type != OS_FILE_READ_ONLY) {
			ut_a(access_type == OS_FILE_READ_WRITE
			     || access_type == OS_FILE_READ_ALLOW_DELETE);

			create_flag = O_RDWR;
		}
	}

	file = open(name, create_flag, my_umask);

	*success = (file != -1);

	if (!read_only
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && !my_disable_locking
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;

	}

	return(file);
}

/** Deletes a file if it exists. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@param[out]	exist		indicate if file pre-exist
@return true if success */
bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	if (exist != NULL) {
		*exist = true;
	}

	int	ret;

	ret = unlink(name);

	if (ret != 0 && errno == ENOENT) {
		if (exist != NULL) {
			*exist = false;
		}
	} else if (ret != 0 && errno != ENOENT) {
		os_file_handle_error_no_exit(name, "delete", false);

		return(false);
	}

	return(true);
}

/** Deletes a file. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@return true if success */
bool
os_file_delete_func(
	const char*	name)
{
	int	ret;

	ret = unlink(name);

	if (ret != 0) {
		os_file_handle_error_no_exit(name, "delete", FALSE);

		return(false);
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_rename(), not directly this
function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@return true if success */
bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	int	ret;

	ret = rename(oldpath, newpath);

	if (ret != 0) {
		os_file_handle_rename_error(oldpath, newpath);

		return(false);
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_close(), not directly this
function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in]	file		Handle to close
@return true if success */
bool os_file_close_func(os_file_t file)
{
  int ret= close(file);

  if (!ret)
    return true;

  os_file_handle_error("close");
  return false;
}

os_offset_t os_file_get_size(os_file_t file) noexcept
{
  return lseek(file, 0, SEEK_END);
}

os_file_size_t os_file_get_size(const char *filename) noexcept
{
	struct stat	s;
	os_file_size_t	file_size;

	int	ret = stat(filename, &s);

	if (ret == 0) {
		file_size.m_total_size = s.st_size;
		/* st_blocks is in 512 byte sized blocks */
		file_size.m_alloc_size = s.st_blocks * 512;
	} else {
		file_size.m_total_size = ~0U;
		file_size.m_alloc_size = (os_offset_t) errno;
	}

	return(file_size);
}

/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in,out]	statinfo	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	if true read only mode checks are enforced
@return DB_SUCCESS if all OK */
static
dberr_t
os_file_get_status_posix(
	const char*	path,
	os_file_stat_t* stat_info,
	struct stat*	statinfo,
	bool		check_rw_perm,
	bool		read_only)
{
	int	ret = stat(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR
		    || errno == ENAMETOOLONG)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "stat", false);

		return(DB_FAIL);
	}

	switch (statinfo->st_mode & S_IFMT) {
	case S_IFDIR:
		stat_info->type = OS_FILE_TYPE_DIR;
		break;
	case S_IFLNK:
		stat_info->type = OS_FILE_TYPE_LINK;
		break;
	case S_IFBLK:
		/* Handle block device as regular file. */
	case S_IFCHR:
		/* Handle character device as regular file. */
	case S_IFREG:
		stat_info->type = OS_FILE_TYPE_FILE;
		break;
	default:
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	stat_info->size = statinfo->st_size;
	stat_info->block_size = statinfo->st_blksize;
	stat_info->alloc_size = statinfo->st_blocks * 512;

	if (check_rw_perm
	    && (stat_info->type == OS_FILE_TYPE_FILE
		|| stat_info->type == OS_FILE_TYPE_BLOCK)) {

		stat_info->rw_perm = !access(path, read_only
					     ? R_OK : R_OK | W_OK);
	}

	return(DB_SUCCESS);
}

/** Truncates a file to a specified size in bytes.
Do nothing if the size to preserve is greater or equal to the current
size of the file.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size to preserve in bytes
@return true if success */
static
bool
os_file_truncate_posix(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size)
{
	int	res = ftruncate(file, size);

	if (res == -1) {

		bool	retry;

		retry = os_file_handle_error_no_exit(
			pathname, "truncate", false);

		if (retry) {
			ib::warn()
				<< "Truncate failed for '"
				<< pathname << "'";
		}
	}

	return(res == 0);
}

bool os_file_set_eof(FILE *file) noexcept
{
	return(!ftruncate(fileno(file), ftell(file)));
}

bool os_file_set_size(const char *name, os_file_t file, os_offset_t size,
                      bool is_sparse) noexcept
{
	ut_ad(!(size & 4095));

	if (is_sparse) {
		bool success = !ftruncate(file, size);
		if (!success) {
			sql_print_error("InnoDB: ftruncate of file %s to %"
					PRIu64 " bytes failed with error %d",
					name, size, errno);
		}
		return success;
	}

# ifdef HAVE_POSIX_FALLOCATE
	int err;
	os_offset_t current_size;
	do {
		current_size = os_file_get_size(file);
		if (current_size == os_offset_t(-1)) {
			err = errno;
		} else {
			if (current_size >= size) {
				return true;
			}
			current_size &= ~4095ULL;
#  ifdef __linux__
			if (!fallocate(file, 0, current_size,
				       size - current_size)) {
				err = 0;
				break;
			}

			err = errno;
#  else
			err = posix_fallocate(file, current_size,
					      size - current_size);
#  endif
		}
	} while (err == EINTR
		 && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED);

	switch (err) {
	case 0:
		return true;
	default:
		sql_print_error("InnoDB: preallocating %" PRIu64
				" bytes for file %s failed with error %d",
				size, name, err);
		/* fall through */
	case EINTR:
		errno = err;
		return false;
	case EINVAL:
	case EOPNOTSUPP:
		/* fall back to the code below */
		break;
	}
# else /* HAVE_POSIX_ALLOCATE */
	os_offset_t current_size = os_file_get_size(file);
# endif /* HAVE_POSIX_ALLOCATE */

	current_size &= ~4095ULL;

	if (current_size >= size) {
		return true;
	}

	/* Write up to 1 megabyte at a time. */
	ulint	buf_size = std::min<ulint>(64,
					   ulint(size >> srv_page_size_shift))
		<< srv_page_size_shift;

	/* Align the buffer for possible raw i/o */
	byte*	buf = static_cast<byte*>(aligned_malloc(buf_size,
							srv_page_size));
	/* Write buffer full of zeros */
	memset(buf, 0, buf_size);

	while (current_size < size
	       && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {
		ulint	n_bytes;

		if (size - current_size < (os_offset_t) buf_size) {
			n_bytes = (ulint) (size - current_size);
		} else {
			n_bytes = buf_size;
		}

		if (os_file_write(IORequestWrite, name,
				  file, buf, current_size, n_bytes) !=
		    DB_SUCCESS) {
			break;
		}

		current_size += n_bytes;
	}

	aligned_free(buf);

	return current_size >= size && os_file_flush(file);
}

#else /* !_WIN32 */

#include <WinIoCtl.h>



/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return 0 on success or errno */
static
dberr_t
os_file_punch_hole_win32(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
{
	FILE_ZERO_DATA_INFORMATION	punch;

	punch.FileOffset.QuadPart = off;
	punch.BeyondFinalZero.QuadPart = off + len;

	/* If lpOverlapped is NULL, lpBytesReturned cannot be NULL,
	therefore we pass a dummy parameter. */
	DWORD	temp;
	BOOL	success = os_win32_device_io_control(
		fh, FSCTL_SET_ZERO_DATA, &punch, sizeof(punch),
		NULL, 0, &temp);

	return(success ? DB_SUCCESS: DB_IO_NO_PUNCH_HOLE);
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
static
bool
os_file_status_win32(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
	int		ret;
	struct _stat64	statinfo;

	ret = _stat64(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */

	} else if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (_S_IFDIR & statinfo.st_mode) {
		*type = OS_FILE_TYPE_DIR;

	} else if (_S_IFREG & statinfo.st_mode) {
		*type = OS_FILE_TYPE_FILE;

	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

/* Dynamically load NtFlushBuffersFileEx, used in os_file_flush_func */
#include <winternl.h>
typedef NTSTATUS(WINAPI* pNtFlushBuffersFileEx)(
  HANDLE FileHandle, ULONG Flags, PVOID Parameters, ULONG ParametersSize,
  PIO_STATUS_BLOCK IoStatusBlock);

static pNtFlushBuffersFileEx my_NtFlushBuffersFileEx
  = (pNtFlushBuffersFileEx)GetProcAddress(GetModuleHandle("ntdll"),
    "NtFlushBuffersFileEx");

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool os_file_flush_func(os_file_t file) noexcept
{
  if (UNIV_UNLIKELY(my_disable_sync))
    return true;

  ++os_n_fsyncs;
  static bool disable_datasync;

  if (my_NtFlushBuffersFileEx && !disable_datasync)
  {
    IO_STATUS_BLOCK iosb{};
    NTSTATUS status= my_NtFlushBuffersFileEx(
        file, FLUSH_FLAGS_FILE_DATA_SYNC_ONLY, nullptr, 0, &iosb);
    if (!status)
      return true;
    /*
      NtFlushBuffersFileEx(FLUSH_FLAGS_FILE_DATA_SYNC_ONLY) might fail
      unless on Win10+, and maybe non-NTFS. Switch to using FlushFileBuffers().
    */
    disable_datasync= true;
  }

  if (FlushFileBuffers(file))
    return true;

  /* Since Windows returns ERROR_INVALID_FUNCTION if the 'file' is
  actually a raw device, we choose to ignore that error if we are using
  raw disks */
  if (srv_start_raw_disk_in_use && GetLastError() == ERROR_INVALID_FUNCTION)
    return true;

  os_file_handle_error("flush");

  /* It is a fatal error if a file flush does not succeed, because then
  the database can get corrupt on disk */
  ut_error;

  return false;
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
then OS error number + OS_FILE_ERROR_MAX is returned.
@param[in]	report_all_errors	true if we want an error message
printed of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
					to the log
@return error number, or OS error number + OS_FILE_ERROR_MAX */
ulint os_file_get_last_error(bool report_all_errors, bool on_error_silent)
  noexcept
{
	ulint	err = (ulint) GetLastError();

	if (err == ERROR_SUCCESS) {
		return(0);
	}

	if (report_all_errors
	    || (!on_error_silent
		&& err != ERROR_DISK_FULL
		&& err != ERROR_FILE_NOT_FOUND
		&& err != ERROR_FILE_EXISTS)) {

		ib::error()
			<< "Operating system error number " << err
			<< " in a file operation.";

		switch (err) {
		case ERROR_PATH_NOT_FOUND:
			break;
		case ERROR_ACCESS_DENIED:
			ib::error()
				<< "The error means mariadbd does not have"
				" the access rights to"
				" the directory. It may also be"
				" you have created a subdirectory"
				" of the same name as a data file.";
			break;
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			ib::error()
				<< "The error means that another program"
				" is using InnoDB's files."
				" This might be a backup or antivirus"
				" software or another instance"
				" of MariaDB."
				" Please close it to get rid of this error.";
			break;
		case ERROR_WORKING_SET_QUOTA:
		case ERROR_NO_SYSTEM_RESOURCES:
			ib::error()
				<< "The error means that there are no"
				" sufficient system resources or quota to"
				" complete the operation.";
			break;
		case ERROR_OPERATION_ABORTED:
			ib::error()
				<< "The error means that the I/O"
				" operation has been aborted"
				" because of either a thread exit"
				" or an application request."
				" Retry attempt is made.";
			break;
		default:
			ib::info() << OPERATING_SYSTEM_ERROR_MSG;
		}
	}

	if (err == ERROR_FILE_NOT_FOUND) {
		return(OS_FILE_NOT_FOUND);
	} else if (err == ERROR_DISK_FULL) {
		return(OS_FILE_DISK_FULL);
	} else if (err == ERROR_FILE_EXISTS) {
		return(OS_FILE_ALREADY_EXISTS);
	} else if (err == ERROR_SHARING_VIOLATION
		   || err == ERROR_LOCK_VIOLATION) {
		return(OS_FILE_SHARING_VIOLATION);
	} else if (err == ERROR_WORKING_SET_QUOTA
		   || err == ERROR_NO_SYSTEM_RESOURCES) {
		return(OS_FILE_INSUFFICIENT_RESOURCE);
	} else if (err == ERROR_OPERATION_ABORTED) {
		return(OS_FILE_OPERATION_ABORTED);
	} else if (err == ERROR_ACCESS_DENIED) {
		return(OS_FILE_ACCESS_VIOLATION);
	}

	return(OS_FILE_ERROR_MAX + err);
}


/** NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_func(
	const char*	name,
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success) noexcept
{
	os_file_t	file;

	*success = false;

	DWORD		access = GENERIC_READ;
	DWORD		create_flag;
	DWORD		attributes = 0;

	ut_ad(srv_operation == SRV_OPERATION_NORMAL);

	if (read_only || create_mode == OS_FILE_OPEN) {
		create_flag = OPEN_EXISTING;
	} else {
		ut_ad(create_mode == OS_FILE_CREATE);
		create_flag = CREATE_NEW;
	}

	if (access_type == OS_FILE_READ_ONLY) {
	} else if (read_only) {
		ib::info()
			<< "Read only mode set. Unable to"
			" open file '" << name << "' in RW mode, "
			<< "trying RO mode";
	} else {
		ut_ad(access_type == OS_FILE_READ_WRITE);
		access = GENERIC_READ | GENERIC_WRITE;
	}

	if (fil_system.is_write_through())
		attributes |= FILE_FLAG_WRITE_THROUGH;
	if (!fil_system.is_buffered())
		attributes |= FILE_FLAG_NO_BUFFERING;

	for (;;) {
		/* Use default security attributes and no template file. */

		file = CreateFile(
			(LPCTSTR) name, access,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			my_win_file_secattr(), create_flag, attributes, NULL);

		if (file != INVALID_HANDLE_VALUE) {
			*success = true;
			break;
		}

		if (!os_file_handle_error_no_exit(name,
						  create_flag == CREATE_NEW
						  ? "create" : "open",
						  false)) {
			break;
		}
	}

	return(file);
}

bool os_file_create_directory(const char *pathname, bool fail_if_exists)
  noexcept
{
	BOOL	rcode;

	rcode = CreateDirectory((LPCTSTR) pathname, NULL);
	if (!(rcode != 0
	      || (GetLastError() == ERROR_ALREADY_EXISTS
		  && !fail_if_exists))) {

		os_file_handle_error_no_exit(
			pathname, "CreateDirectory", false);

		return(false);
	}

	return(true);
}

/** Get disk sector size for a file. */
static size_t get_sector_size(HANDLE file)
{
  FILE_STORAGE_INFO fsi;
  ULONG s= 4096;
  if (GetFileInformationByHandleEx(file, FileStorageInfo, &fsi, sizeof fsi))
  {
    s= fsi.PhysicalBytesPerSectorForPerformance;
    if (s > 4096 || s < 64 || !ut_is_2pow(s))
      return 4096;
  }
  return s;
}

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	os_file_create_t create_mode,
	ulint		type,
	bool		read_only,
	bool*		success) noexcept
{
	os_file_t	file;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		SetLastError(ERROR_DISK_FULL);
		return(OS_FILE_CLOSED);
	);

	DWORD		create_flag = OPEN_EXISTING;
	DWORD		share_mode = read_only
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ | FILE_SHARE_DELETE;

	switch (create_mode) {
	case OS_FILE_OPEN_RAW:
		ut_a(!read_only);
		/* On Windows Physical devices require admin privileges and
		have to have the write-share mode set. See the remarks
		section for the CreateFile() function documentation in MSDN. */

		share_mode |= FILE_SHARE_WRITE;
		break;
	case OS_FILE_CREATE_SILENT:
	case OS_FILE_CREATE:
		create_flag = CREATE_NEW;
		break;
	default:
		ut_ad(create_mode == OS_FILE_OPEN
		      || create_mode == OS_FILE_OPEN_SILENT
		      || create_mode == OS_FILE_OPEN_RETRY_SILENT
		      || create_mode == OS_FILE_OPEN_RETRY);
		break;
	}

	DWORD attributes= FILE_FLAG_OVERLAPPED;

	if (type == OS_LOG_FILE) {
		if (!log_sys.is_opened() && !log_sys.log_buffered) {
			attributes|= FILE_FLAG_NO_BUFFERING;
		}
		if (log_sys.log_write_through)
			attributes|= FILE_FLAG_WRITE_THROUGH;
	} else {
		if (type == OS_DATA_FILE && !fil_system.is_buffered())
			attributes|= FILE_FLAG_NO_BUFFERING;
		if (fil_system.is_write_through())
			attributes|= FILE_FLAG_WRITE_THROUGH;
	}

	DWORD access = read_only ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;

	for (;;) {
		const  char *operation;

		/* Use default security attributes and no template file. */
		file = CreateFile(
			name, access, share_mode, my_win_file_secattr(),
			create_flag, attributes, NULL);

		*success = file != INVALID_HANDLE_VALUE;

		if (*success && type == OS_LOG_FILE) {
			uint32_t s = uint32_t(get_sector_size(file));
			log_sys.set_block_size(s);
			if (attributes & FILE_FLAG_NO_BUFFERING) {
				if (os_file_get_size(file) % s) {
					attributes &= ~FILE_FLAG_NO_BUFFERING;
					create_flag = OPEN_ALWAYS;
					CloseHandle(file);
					continue;
				}
				log_sys.log_buffered = false;
			}
		}

		if (*success) {
			break;
		}

		operation = create_flag == CREATE_NEW ? "create" : "open";

		if (!os_file_handle_error_no_exit(name, operation,
						  create_mode
						  & OS_FILE_ON_ERROR_SILENT)) {
			break;
		}
	}

	if (*success &&  (attributes & FILE_FLAG_OVERLAPPED) && srv_thread_pool) {
		srv_thread_pool->bind(file);
	}
	return(file);
}

/** NOTE! Use the corresponding macro os_file_create_simple_no_error_handling(),
not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option is
				used by a backup program reading the file
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
{
	os_file_t	file;

	DWORD		access = GENERIC_READ;
	DWORD		create_flag = OPEN_EXISTING;
	DWORD		attributes	= 0;
	DWORD		share_mode = FILE_SHARE_READ | FILE_SHARE_DELETE;

	ut_a(name);

	if (read_only) {
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE
			| FILE_SHARE_DELETE;
	} else {
		if (create_mode == OS_FILE_CREATE) {
			create_flag = CREATE_NEW;
		} else {
			ut_ad(create_mode == OS_FILE_OPEN);
		}

		switch (access_type) {
		case OS_FILE_READ_ONLY: break;
		case OS_FILE_READ_WRITE:
			access = GENERIC_READ | GENERIC_WRITE;
			break;
		default:
			ut_ad(access_type == OS_FILE_READ_ALLOW_DELETE);
			/* A backup program has to give mariadbd the maximum
			freedom to do what it likes with the file */
			share_mode |= FILE_SHARE_DELETE | FILE_SHARE_WRITE
				| FILE_SHARE_READ;
		}
	}

	file = CreateFile((LPCTSTR) name,
			  access,
			  share_mode,
			  my_win_file_secattr(),
			  create_flag,
			  attributes,
			  NULL);		// No template file

	*success = (file != INVALID_HANDLE_VALUE);

	return(file);
}

/** Deletes a file if it exists. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@param[out]	exist		indicate if file pre-exist
@return true if success */
bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	ulint	count	= 0;

	if (exist != NULL) {
		*exist = true;
	}

	for (;;) {
		/* In Windows, deleting an .ibd file may fail if
		the file is being accessed by an external program,
		such as a backup tool. */

		bool	ret = DeleteFile((LPCTSTR) name);

		if (ret) {
			return(true);
		}

		switch (GetLastError()) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			/* the file does not exist, this not an error */
			if (exist != NULL) {
				*exist = false;
			}
			/* fall through */
		case ERROR_ACCESS_DENIED:
			return(true);
		}

		++count;

		if (count > 100 && 0 == (count % 10)) {

			/* Print error information */
			os_file_get_last_error(true);

			ib::warn() << "Delete of file '" << name << "' failed.";
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));

		if (count > 2000) {

			return(false);
		}
	}
}

/** Deletes a file. The file has to be closed before calling this.
@param[in]	name		File path as NUL terminated string
@return true if success */
bool
os_file_delete_func(
	const char*	name)
{
	ulint	count	= 0;

	for (;;) {
		/* In Windows, deleting an .ibd file may fail if
		the file is being accessed by an external program,
		such as a backup tool. */

		BOOL	ret = DeleteFile((LPCTSTR) name);

		if (ret) {
			return(true);
		}

		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			/* If the file does not exist, we classify this as
			a 'mild' error and return */

			return(false);
		}

		++count;

		if (count > 100 && 0 == (count % 10)) {

			/* print error information */
			os_file_get_last_error(true);

			ib::warn()
				<< "Cannot delete file '" << name << "'. Is "
				<< "another program accessing it?";
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));

		if (count > 2000) {

			return(false);
		}
	}

	ut_error;
	return(false);
}

/** NOTE! Use the corresponding macro os_file_rename(), not directly this
function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@return true if success */
bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	for (int retry= 50;; retry--){
		if (MoveFileEx(oldpath, newpath, MOVEFILE_REPLACE_EXISTING))
			return true;

		if (!retry)
			break;

		if (GetLastError() != ERROR_SHARING_VIOLATION)
			break;

		// oldpath was opened by someone else (antivirus?)
		//without FILE_SHARE_DELETE flag. Retry operation

		Sleep(10);
	}

	os_file_handle_rename_error(oldpath, newpath);
	return(false);
}

/** NOTE! Use the corresponding macro os_file_close(), not directly
this function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in,own]	file		Handle to a file
@return true if success */
bool os_file_close_func(os_file_t file)
{
  ut_ad(file);
  if (!CloseHandle(file))
  {
    os_file_handle_error("close");
    return false;
  }

  if(srv_thread_pool)
    srv_thread_pool->unbind(file);
  return true;
}

os_offset_t os_file_get_size(os_file_t file) noexcept
{
  LARGE_INTEGER li;
  if (GetFileSizeEx(file, &li))
    return li.QuadPart;
  return ((os_offset_t) -1);
}

os_file_size_t os_file_get_size(const char *filename) noexcept
{
	struct __stat64	s;
	os_file_size_t	file_size;

	int		ret = _stat64(filename, &s);

	if (ret == 0) {

		file_size.m_total_size = s.st_size;

		DWORD	low_size;
		DWORD	high_size;

		low_size = GetCompressedFileSize(filename, &high_size);

		if (low_size != INVALID_FILE_SIZE) {

			file_size.m_alloc_size = high_size;
			file_size.m_alloc_size <<= 32;
			file_size.m_alloc_size |= low_size;

		} else {
			ib::error()
				<< "GetCompressedFileSize("
				<< filename << ", ..) failed.";

			file_size.m_alloc_size = (os_offset_t) -1;
		}
	} else {
		file_size.m_total_size = ~0;
		file_size.m_alloc_size = (os_offset_t) ret;
	}

	return(file_size);
}

/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in,out]	statinfo	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	true if the file is opened in read-only mode
@return DB_SUCCESS if all OK */
static
dberr_t
os_file_get_status_win32(
	const char*	path,
	os_file_stat_t* stat_info,
	struct _stat64*	statinfo,
	bool		check_rw_perm,
	bool		read_only)
{
	int	ret = _stat64(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR
		    || errno == ENAMETOOLONG)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "STAT", false);

		return(DB_FAIL);

	} else if (_S_IFDIR & statinfo->st_mode) {

		stat_info->type = OS_FILE_TYPE_DIR;

	} else if (_S_IFREG & statinfo->st_mode) {

		DWORD	access = GENERIC_READ;

		if (!read_only) {
			access |= GENERIC_WRITE;
		}

		stat_info->type = OS_FILE_TYPE_FILE;

		/* Check if we can open it in read-only mode. */

		if (check_rw_perm) {
			HANDLE	fh;

			fh = CreateFile(
				(LPCTSTR) path,		// File to open
				access,
				FILE_SHARE_READ | FILE_SHARE_WRITE
				| FILE_SHARE_DELETE,	// Full sharing
				my_win_file_secattr(),
				OPEN_EXISTING,		// Existing file only
				FILE_ATTRIBUTE_NORMAL,	// Normal file
				NULL);			// No attr. template

			if (fh == INVALID_HANDLE_VALUE) {
				stat_info->rw_perm = false;
			} else {
				stat_info->rw_perm = true;
				CloseHandle(fh);
			}
		}
	} else {
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	return(DB_SUCCESS);
}

/**
Sets a sparse flag on Windows file.
@param[in]	file  file handle
@return true on success, false on error
*/
#include <versionhelpers.h>
bool os_file_set_sparse_win32(os_file_t file, bool is_sparse) noexcept
{
	if (!is_sparse && !IsWindows8OrGreater()) {
		/* Cannot  unset sparse flag on older Windows.
		Until Windows8 it is documented to produce unpredictable results,
		if there are unallocated ranges in file.*/
		return false;
	}
	DWORD temp;
	FILE_SET_SPARSE_BUFFER sparse_buffer;
	sparse_buffer.SetSparse = is_sparse;
	return os_win32_device_io_control(file,
		FSCTL_SET_SPARSE, &sparse_buffer, sizeof(sparse_buffer), 0, 0,&temp);
}

bool os_file_set_size(const char *pathname, os_file_t file, os_offset_t size)
  noexcept
{
	LARGE_INTEGER	length;

	length.QuadPart = size;

	BOOL	success = SetFilePointerEx(file, length, NULL, FILE_BEGIN);

	if (!success) {
		os_file_handle_error_no_exit(
			pathname, "SetFilePointerEx", false);
	} else {
		success = SetEndOfFile(file);
		if (!success) {
			os_file_handle_error_no_exit(
				pathname, "SetEndOfFile", false);
		}
	}
	return(success);
}

bool os_file_set_eof(FILE *file) noexcept
{
	HANDLE	h = (HANDLE) _get_osfhandle(fileno(file));

	return(SetEndOfFile(h));
}

#endif /* !_WIN32*/

/** Does a synchronous read or write depending upon the type specified
In case of partial reads/writes the function tries
NUM_RETRIES_ON_PARTIAL_IO times to read/write the complete data.
@param[in]	type,		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	err		DB_SUCCESS or error code
@return number of bytes read/written, -1 if error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_io(
	const IORequest&in_type,
	os_file_t	file,
	void*		buf,
	ulint		n,
	os_offset_t	offset,
	dberr_t*	err)
{
	ssize_t		original_n = ssize_t(n);
	IORequest	type = in_type;
	ssize_t		bytes_returned = 0;

	SyncFileIO	sync_file_io(file, buf, n, offset);

	for (ulint i = 0; i < NUM_RETRIES_ON_PARTIAL_IO; ++i) {

		ssize_t	n_bytes = sync_file_io.execute(type);

		/* Check for a hard error. Not much we can do now. */
		if (n_bytes < 0) {

			break;

		} else if (n_bytes + bytes_returned == ssize_t(n)) {

			bytes_returned += n_bytes;

			*err = type.maybe_punch_hole(offset, n);

			return(original_n);
		}

		/* Handle partial read/write. */

		ut_ad(ulint(n_bytes + bytes_returned) < n);

		bytes_returned += n_bytes;

		if (type.type != IORequest::READ_MAYBE_PARTIAL) {
			sql_print_warning("InnoDB: %zu bytes should have been"
					  " %s at %" PRIu64 " from %s,"
					  " but got only %zd."
					  " Retrying.",
					  n, type.is_read()
					  ? "read" : "written", offset,
					  type.node
					  ? type.node->name
					  : "(unknown file)", bytes_returned);
		}

		/* Advance the offset and buffer by n_bytes */
		sync_file_io.advance(n_bytes);
	}

	*err = DB_IO_ERROR;

	if (type.type != IORequest::READ_MAYBE_PARTIAL) {
		ib::warn()
			<< "Retry attempts for "
			<< (type.is_read() ? "reading" : "writing")
			<< " partial data failed.";
	}

	return(bytes_returned);
}

/** Does a synchronous write operation in Posix.
@param[in]	type		IO context
@param[in]	file		handle to an open file
@param[out]	buf		buffer from which to write
@param[in]	n		number of bytes to write, starting from offset
@param[in]	offset		file offset from the start where to write
@param[out]	err		DB_SUCCESS or error code
@return number of bytes written
@retval -1 on error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_pwrite(
	const IORequest&	type,
	os_file_t		file,
	const byte*		buf,
	ulint			n,
	os_offset_t		offset,
	dberr_t*		err)
{
	ut_ad(type.is_write());

	++os_n_file_writes;

	const bool monitor = MONITOR_IS_ON(MONITOR_OS_PENDING_WRITES);
	MONITOR_ATOMIC_INC_LOW(MONITOR_OS_PENDING_WRITES, monitor);
	ssize_t	n_bytes = os_file_io(type, file, const_cast<byte*>(buf),
				     n, offset, err);
	MONITOR_ATOMIC_DEC_LOW(MONITOR_OS_PENDING_WRITES, monitor);

	return(n_bytes);
}

/** NOTE! Use the corresponding macro os_file_write(), not directly
Requests a synchronous write operation.
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer from which to write
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@return error code
@retval	DB_SUCCESS	if the operation succeeded */
dberr_t
os_file_write_func(
	const IORequest&	type,
	const char*		name,
	os_file_t		file,
	const void*		buf,
	os_offset_t		offset,
	ulint			n)
{
	dberr_t		err;

	ut_ad(n > 0);

	ssize_t	n_bytes = os_file_pwrite(type, file, (byte*)buf, n, offset, &err);

	if ((ulint) n_bytes != n && !os_has_said_disk_full) {

		ib::error()
			<< "Write to file " << name << " failed at offset "
			<< offset << ", " << n
			<< " bytes should have been written,"
			" only " << n_bytes << " were written."
			" Operating system error number " << IF_WIN(GetLastError(),errno) << "."
			" Check that your OS and file system"
			" support files of this size."
			" Check also that the disk is not full"
			" or a disk quota exceeded.";
#ifndef _WIN32
		if (strerror(errno) != NULL) {

			ib::error()
				<< "Error number " << errno
				<< " means '" << strerror(errno) << "'";
		}

		ib::info() << OPERATING_SYSTEM_ERROR_MSG;
#endif
		os_has_said_disk_full = true;
	}

	return(err);
}

/** Does a synchronous read operation in Posix.
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	err		DB_SUCCESS or error code
@return number of bytes read, -1 if error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_pread(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	ulint			n,
	os_offset_t		offset,
	dberr_t*		err)
{
	ut_ad(type.is_read());

	++os_n_file_reads;

	const bool monitor = MONITOR_IS_ON(MONITOR_OS_PENDING_READS);
	MONITOR_ATOMIC_INC_LOW(MONITOR_OS_PENDING_READS, monitor);
	ssize_t	n_bytes = os_file_io(type, file, buf, n, offset, err);
	MONITOR_ATOMIC_DEC_LOW(MONITOR_OS_PENDING_READS, monitor);

	return(n_bytes);
}

/** Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	o		number of bytes actually read
@return DB_SUCCESS or error code */
dberr_t
os_file_read_func(
	const IORequest&	type,
	os_file_t	file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o) noexcept
{
  ut_ad(!type.node || type.node->handle == file);
  ut_ad(n);

  os_bytes_read_since_printout+= n;

  dberr_t err;
  ssize_t n_bytes= os_file_pread(type, file, buf, n, offset, &err);

  if (o)
    *o= ulint(n_bytes);

  if (ulint(n_bytes) == n || err != DB_SUCCESS)
    return err;

  os_file_handle_error_no_exit(type.node ? type.node->name : nullptr, "read",
                               false);
  sql_print_error("InnoDB: Tried to read %zu bytes at offset %" PRIu64
                  " of file %s, but was only able to read %zd",
                  n, offset, type.node ? type.node->name : "(unknown)",
                  n_bytes);

  return err ? err : DB_IO_ERROR;
}

/** Handle errors for file operations.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation
@param[in]	should_abort	whether to abort on an unknown error
@param[in]	on_error_silent	whether to suppress reports of non-fatal errors
@return true if we should retry the operation */
static MY_ATTRIBUTE((warn_unused_result))
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_abort,
	bool		on_error_silent) noexcept
{
	ulint	err;

	err = os_file_get_last_error(false, on_error_silent);

	switch (err) {
	case OS_FILE_DISK_FULL:
		/* We only print a warning about disk full once */

		if (os_has_said_disk_full) {

			return(false);
		}

		/* Disk full error is reported irrespective of the
		on_error_silent setting. */

		if (name) {

			ib::error()
				<< "Encountered a problem with file '"
				<< name << "'";
		}

		ib::error()
			<< "Disk is full. Try to clean the disk to free space.";

		os_has_said_disk_full = true;

		return(false);

	case OS_FILE_AIO_RESOURCES_RESERVED:
	case OS_FILE_AIO_INTERRUPTED:

		return(true);

	case OS_FILE_PATH_ERROR:
	case OS_FILE_ALREADY_EXISTS:
	case OS_FILE_ACCESS_VIOLATION:
		return(false);

        case OS_FILE_NOT_FOUND:
		if (!on_error_silent) {
			sql_print_error("InnoDB: File %s was not found", name);
		}
		return false;

	case OS_FILE_SHARING_VIOLATION:

		std::this_thread::sleep_for(std::chrono::seconds(10));
		return(true);

	case OS_FILE_OPERATION_ABORTED:
	case OS_FILE_INSUFFICIENT_RESOURCE:

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		return(true);

	default:

		/* If it is an operation that can crash on error then it
		is better to ignore on_error_silent and print an error message
		to the log. */

		if (should_abort || !on_error_silent) {
			ib::error() << "File "
				<< (name != NULL ? name : "(unknown)")
				<< ": '" << operation << "'"
				" returned OS error " << err << "."
				<< (should_abort
				    ? " Cannot continue operation" : "");
		}

		if (should_abort) {
			abort();
		}
	}

	return(false);
}

/** Check if the file system supports sparse files.
@param fh	file handle
@return true if the file system supports sparse files */
static bool os_is_sparse_file_supported(os_file_t fh) noexcept
{
#ifdef _WIN32
	FILE_ATTRIBUTE_TAG_INFO info;
	if (GetFileInformationByHandleEx(fh, FileAttributeTagInfo,
		&info, (DWORD)sizeof(info))) {
		if (info.FileAttributes != INVALID_FILE_ATTRIBUTES) {
			return (info.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
		}
	}
	return false;
#else
	/* We don't know the FS block size, use the sector size. The FS
	will do the magic. */
	return DB_SUCCESS == os_file_punch_hole_posix(fh, 0, srv_page_size);
#endif /* _WIN32 */
}

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
	bool		allow_shrink) noexcept
{
	if (!allow_shrink) {
		/* Do nothing if the size preserved is larger than or
		equal to the current size of file */
		os_offset_t	size_bytes = os_file_get_size(file);

		if (size >= size_bytes) {
			return(true);
		}
	}

#ifdef _WIN32
	return os_file_set_size(pathname, file, size);
#else /* _WIN32 */
	return(os_file_truncate_posix(pathname, file, size));
#endif /* _WIN32 */
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
bool
os_file_status(
	const char*	path,
	bool*		exists,
	os_file_type_t* type) noexcept
{
#ifdef _WIN32
	return(os_file_status_win32(path, exists, type));
#else
	return(os_file_status_posix(path, exists, type));
#endif /* _WIN32 */
}

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
{
#ifdef _WIN32
	return os_file_punch_hole_win32(fh, off, len);
#else
	return os_file_punch_hole_posix(fh, off, len);
#endif /* _WIN32 */
}

/** Free storage space associated with a section of the file.
@param off   byte offset from the start (SEEK_SET)
@param len   size of the hole in bytes
@return DB_SUCCESS or error code */
dberr_t IORequest::punch_hole(os_offset_t off, ulint len) const noexcept
{
	ulint trim_len = bpage ? bpage->physical_size() - len : 0;

	if (trim_len == 0) {
		return(DB_SUCCESS);
	}

	off += len;

	/* Check does file system support punching holes for this
	tablespace. */
	if (!node->punch_hole) {
		return DB_IO_NO_PUNCH_HOLE;
	}

	dberr_t err = os_file_punch_hole(node->handle, off, trim_len);

	switch (err) {
	case DB_SUCCESS:
		srv_stats.page_compressed_trim_op.inc();
		return err;
	case DB_IO_NO_PUNCH_HOLE:
		node->punch_hole = false;
		err = DB_SUCCESS;
		/* fall through */
	default:
		return err;
	}
}

/*
  Get file system block size, by path.

  This is expensive on Windows, and not very useful in general,
  (only shown in some I_S table), so we keep that out of usual
  stat.
*/
size_t os_file_get_fs_block_size(const char *path)
{
#ifdef _WIN32
  char volname[MAX_PATH];
  if (!GetVolumePathName(path, volname, MAX_PATH))
    return 0;
  DWORD sectorsPerCluster;
  DWORD bytesPerSector;
  DWORD numberOfFreeClusters;
  DWORD totalNumberOfClusters;

  if (GetDiskFreeSpace(volname, &sectorsPerCluster, &bytesPerSector,
                       &numberOfFreeClusters, &totalNumberOfClusters))
    return ((size_t) bytesPerSector) * sectorsPerCluster;
#else
  os_file_stat_t info;
  if (os_file_get_status(path, &info, false, false) == DB_SUCCESS)
    return info.block_size;
#endif
  return 0;
}

/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	true if file is opened in read-only mode
@return DB_SUCCESS if all OK */
dberr_t
os_file_get_status(
	const char*	path,
	os_file_stat_t* stat_info,
	bool		check_rw_perm,
	bool		read_only) noexcept
{
	dberr_t	ret;

#ifdef _WIN32
	struct _stat64	info;

	ret = os_file_get_status_win32(
		path, stat_info, &info, check_rw_perm, read_only);

#else
	struct stat	info;

	ret = os_file_get_status_posix(
		path, stat_info, &info, check_rw_perm, read_only);

#endif /* _WIN32 */

	if (ret == DB_SUCCESS) {
		stat_info->ctime = info.st_ctime;
		stat_info->atime = info.st_atime;
		stat_info->mtime = info.st_mtime;
		stat_info->size  = info.st_size;
	}

	return(ret);
}

static void fake_io_callback(void *c)
{
  tpool::aiocb *cb= static_cast<tpool::aiocb*>(c);
  ut_ad(read_slots->contains(cb));
  static_cast<const IORequest*>(static_cast<const void*>(cb->m_userdata))->
    fake_read_complete(cb->m_offset);
  read_slots->release(cb);
}

static void read_io_callback(void *c)
{
  tpool::aiocb *cb= static_cast<tpool::aiocb*>(c);
  ut_ad(cb->m_opcode == tpool::aio_opcode::AIO_PREAD);
  ut_ad(read_slots->contains(cb));
  const IORequest &request= *static_cast<const IORequest*>
    (static_cast<const void*>(cb->m_userdata));
  request.read_complete(cb->m_err);
  read_slots->release(cb);
}

static void write_io_callback(void *c)
{
  tpool::aiocb *cb= static_cast<tpool::aiocb*>(c);
  ut_ad(cb->m_opcode == tpool::aio_opcode::AIO_PWRITE);
  ut_ad(write_slots->contains(cb));
  const IORequest &request= *static_cast<const IORequest*>
    (static_cast<const void*>(cb->m_userdata));

  if (UNIV_UNLIKELY(cb->m_err != 0))
    ib::info () << "IO Error: " << cb->m_err
                << " during write of "
                << cb->m_len << " bytes, for file "
                << request.node->name << "(" << cb->m_fh << "), returned "
                << cb->m_ret_len;

  request.write_complete(cb->m_err);
  write_slots->release(cb);
}

int os_aio_init() noexcept
{
  int max_write_events= int(srv_n_write_io_threads *
                            OS_AIO_N_PENDING_IOS_PER_THREAD);
  int max_read_events= int(srv_n_read_io_threads *
                           OS_AIO_N_PENDING_IOS_PER_THREAD);
  int max_events= max_read_events + max_write_events;
  int ret= 1;

  if (srv_use_native_aio)
  {
    tpool::aio_implementation aio_impl= tpool::OS_IO_DEFAULT;
#ifdef __linux__
    compile_time_assert(SRV_LINUX_AIO_IO_URING == (srv_linux_aio_t)tpool::OS_IO_URING);
    compile_time_assert(SRV_LINUX_AIO_LIBAIO == (srv_linux_aio_t) tpool::OS_IO_LIBAIO);
    compile_time_assert(SRV_LINUX_AIO_AUTO == (srv_linux_aio_t) tpool::OS_IO_DEFAULT);
    aio_impl=(tpool::aio_implementation) srv_linux_aio_method;
#endif

    ret= srv_thread_pool->configure_aio(srv_use_native_aio, max_events,
                                        aio_impl);
    if (ret)
    {
      srv_use_native_aio= false;
      sql_print_warning("InnoDB: native AIO failed: falling back to"
                        " innodb_use_native_aio=OFF");
    }
    else
      sql_print_information("InnoDB: Using %s", srv_thread_pool
                            ->get_aio_implementation());
  }
  if (ret)
    ret= srv_thread_pool->configure_aio(false, max_events,
                                        tpool::OS_IO_DEFAULT);
  if (!ret)
  {
    read_slots= new io_slots(max_read_events, srv_n_read_io_threads);
    write_slots= new io_slots(max_write_events, srv_n_write_io_threads);
  }
  else
    sql_print_error("InnoDB: Cannot initialize AIO sub-system");

  return ret;
}


/**
Change reader or writer thread parameter on a running server.
This includes resizing  the io slots, as we calculate
number of outstanding IOs based on the these variables.

It is trickier with when Linux AIO is involved (io_context
needs to be recreated to account for different number of
max_events). With Linux AIO, depending on fs-max-aio number
and user and system wide max-aio limitation, this can fail.

Otherwise, we just resize the slots, and allow for
more concurrent threads via thread_group setting.

@param[in] n_reader_threads - max number of concurrently
  executing read callbacks
@param[in] n_writer_thread - max number of cuncurrently
  executing write callbacks
@return 0 for success, !=0 for error.
*/
int os_aio_resize(ulint n_reader_threads, ulint n_writer_threads) noexcept
{
  /* Lock the slots, and wait until all current IOs finish.*/
  std::unique_lock<std::mutex> lk_read(read_slots->mutex()),
    lk_write(write_slots->mutex());

  read_slots->wait(lk_read);
  write_slots->wait(lk_write);

  /* Now, all IOs have finished and no new ones can start, due to locks. */
  int max_read_events= int(n_reader_threads * OS_AIO_N_PENDING_IOS_PER_THREAD);
  int max_write_events= int(n_writer_threads * OS_AIO_N_PENDING_IOS_PER_THREAD);
  int events= max_read_events + max_write_events;

  /* Do the Linux AIO dance (this will try to create a new
  io context with changed max_events, etc.) */

  int ret= srv_thread_pool->reconfigure_aio(srv_use_native_aio, events);

  if (ret)
  {
    /** Do the best effort. We can't change the parallel io number,
    but we still can adjust the number of concurrent completion handlers.*/
    read_slots->task_group().set_max_tasks(static_cast<int>(n_reader_threads));
    write_slots->task_group().set_max_tasks(static_cast<int>(n_writer_threads));
  }
  else
  {
    /* Allocation succeeded, resize the slots*/
    read_slots->resize(max_read_events, static_cast<int>(n_reader_threads));
    write_slots->resize(max_write_events, static_cast<int>(n_writer_threads));
  }
  return ret;
}

void os_aio_free() noexcept
{
  delete read_slots;
  delete write_slots;
  read_slots= nullptr;
  write_slots= nullptr;
  srv_thread_pool->disable_aio();
}

/** Wait until there are no pending asynchronous writes. */
static void os_aio_wait_until_no_pending_writes_low(bool declare) noexcept
{
  const bool notify_wait= declare && write_slots->pending_io_count();

  if (notify_wait)
    tpool::tpool_wait_begin();

   write_slots->wait();

   if (notify_wait)
     tpool::tpool_wait_end();
}

/** Wait until there are no pending asynchronous writes.
@param declare  whether the wait will be declared in tpool */
void os_aio_wait_until_no_pending_writes(bool declare) noexcept
{
  os_aio_wait_until_no_pending_writes_low(declare);
  buf_dblwr.wait_flush_buffered_writes();
}

/** @return number of pending reads */
size_t os_aio_pending_reads() noexcept
{
  std::lock_guard<std::mutex> lock(read_slots->mutex());
  return read_slots->pending_io_count();
}

/** @return approximate number of pending reads */
size_t os_aio_pending_reads_approx() noexcept
{
  return read_slots->pending_io_count();
}

/** @return number of pending writes */
size_t os_aio_pending_writes() noexcept
{
  std::lock_guard<std::mutex> lock(write_slots->mutex());
  return write_slots->pending_io_count();
}

/** @return approximate number of pending writes */
size_t os_aio_pending_writes_approx() noexcept
{
  return write_slots->pending_io_count();
}

/** Wait until all pending asynchronous reads have completed.
@param declare  whether the wait will be declared in tpool */
void os_aio_wait_until_no_pending_reads(bool declare) noexcept
{
  const bool notify_wait= declare && read_slots->pending_io_count();

  if (notify_wait)
    tpool::tpool_wait_begin();

  read_slots->wait();

  if (notify_wait)
    tpool::tpool_wait_end();
}

/** Submit a fake read request during crash recovery.
@param type  fake read request
@param offset additional context */
void os_fake_read(const IORequest &type, os_offset_t offset) noexcept
{
  tpool::aiocb *cb= read_slots->acquire();

  cb->m_group= read_slots->get_task_group();
  cb->m_fh= type.node->handle.m_file;
  cb->m_buffer= nullptr;
  cb->m_len= 0;
  cb->m_offset= offset;
  cb->m_opcode= tpool::aio_opcode::AIO_PREAD;
  new (cb->m_userdata) IORequest{type};
  cb->m_internal_task.m_func= fake_io_callback;
  cb->m_internal_task.m_arg= cb;
  cb->m_internal_task.m_group= cb->m_group;

  srv_thread_pool->submit_task(&cb->m_internal_task);
}


/** Request a read or write.
@param type		I/O request
@param buf		buffer
@param offset		file offset
@param n		number of bytes
@retval DB_SUCCESS if request was queued successfully
@retval DB_IO_ERROR on I/O error */
dberr_t os_aio(const IORequest &type, void *buf, os_offset_t offset, size_t n)
  noexcept
{
	ut_ad(n > 0);
	ut_ad(!(n & 511)); /* payload of page_compressed tables */
	ut_ad((offset % UNIV_ZIP_SIZE_MIN) == 0);
	ut_ad((reinterpret_cast<size_t>(buf) % UNIV_ZIP_SIZE_MIN) == 0);
	ut_ad(type.is_read() || type.is_write());
	ut_ad(type.node);
	ut_ad(type.node->is_open());

#ifdef WIN_ASYNC_IO
	ut_ad((n & 0xFFFFFFFFUL) == n);
#endif /* WIN_ASYNC_IO */

#ifdef UNIV_PFS_IO
	PSI_file_locker_state state;
	PSI_file_locker* locker= nullptr;
	register_pfs_file_io_begin(&state, locker, type.node->handle, n,
				   type.is_write()
				   ? PSI_FILE_WRITE : PSI_FILE_READ,
				   __FILE__, __LINE__);
#endif /* UNIV_PFS_IO */
	dberr_t err = DB_SUCCESS;

	if (!type.is_async()) {
		err = type.is_read()
			? os_file_read_func(type, type.node->handle,
					    buf, offset, n, nullptr)
			: os_file_write_func(type, type.node->name,
					     type.node->handle,
					     buf, offset, n);
func_exit:
#ifdef UNIV_PFS_IO
		register_pfs_file_io_end(locker, n);
#endif /* UNIV_PFS_IO */
		return err;
	}

	io_slots* slots;
	tpool::callback_func callback;
	tpool::aio_opcode opcode;

	if (type.is_read()) {
		++os_n_file_reads;
		slots = read_slots;
		callback = read_io_callback;
		opcode = tpool::aio_opcode::AIO_PREAD;
	} else {
		++os_n_file_writes;
		slots = write_slots;
		callback = write_io_callback;
		opcode = tpool::aio_opcode::AIO_PWRITE;
	}

	compile_time_assert(sizeof(IORequest) <= tpool::MAX_AIO_USERDATA_LEN);
	tpool::aiocb* cb = slots->acquire();

	cb->m_buffer = buf;
	cb->m_callback = callback;
	cb->m_group = slots->get_task_group();
	cb->m_fh = type.node->handle.m_file;
	cb->m_len = (int)n;
	cb->m_offset = offset;
	cb->m_opcode = opcode;
	new (cb->m_userdata) IORequest{type};

	if (srv_thread_pool->submit_io(cb)) {
		slots->release(cb);
		os_file_handle_error_no_exit(type.node->name, type.is_read()
					     ? "aio read" : "aio write",
					     false);
		err = DB_IO_ERROR;
		type.node->space->release();
	}

	goto func_exit;
}

void os_aio_print(FILE *file) noexcept
{
	time_t		current_time;
	double		time_elapsed;

	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time, os_last_printout);

	fprintf(file,
		"Pending flushes (fsync): " ULINTPF "\n"
		ULINTPF " OS file reads, %zu OS file writes, %zu OS fsyncs\n",
		ulint{fil_n_pending_tablespace_flushes},
		ulint{os_n_file_reads},
		static_cast<size_t>(os_n_file_writes),
		static_cast<size_t>(os_n_fsyncs));

	const ulint n_reads = ulint(MONITOR_VALUE(MONITOR_OS_PENDING_READS));
	const ulint n_writes = ulint(MONITOR_VALUE(MONITOR_OS_PENDING_WRITES));

	if (n_reads != 0 || n_writes != 0) {
		fprintf(file,
			ULINTPF " pending reads, " ULINTPF " pending writes\n",
			n_reads, n_writes);
	}

	ulint avg_bytes_read = (os_n_file_reads == os_n_file_reads_old)
		? 0
		: os_bytes_read_since_printout
		/ (os_n_file_reads - os_n_file_reads_old);

	fprintf(file,
		"%.2f reads/s, " ULINTPF " avg bytes/read,"
		" %.2f writes/s, %.2f fsyncs/s\n",
		static_cast<double>(os_n_file_reads - os_n_file_reads_old)
		/ time_elapsed,
		avg_bytes_read,
		static_cast<double>(os_n_file_writes - os_n_file_writes_old)
		/ time_elapsed,
		static_cast<double>(os_n_fsyncs - os_n_fsyncs_old)
		/ time_elapsed);

	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;

	os_last_printout = current_time;
}

void os_aio_refresh_stats() noexcept
{
	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_n_file_reads_old = os_n_file_reads;

	os_n_file_writes_old = os_n_file_writes;

	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_last_printout = time(NULL);
}

#ifdef _WIN32

/* Checks whether physical drive is on SSD.*/
static bool is_drive_on_ssd(DWORD nr)
{
  char physical_drive_path[32];
  snprintf(physical_drive_path, sizeof(physical_drive_path),
           "\\\\.\\PhysicalDrive%lu", nr);

  HANDLE h= CreateFile(physical_drive_path, 0,
                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;

  DEVICE_SEEK_PENALTY_DESCRIPTOR seek_penalty;
  STORAGE_PROPERTY_QUERY storage_query{};
  storage_query.PropertyId= StorageDeviceSeekPenaltyProperty;
  storage_query.QueryType= PropertyStandardQuery;

  bool on_ssd= false;
  DWORD bytes_written;
  if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &storage_query,
                      sizeof storage_query, &seek_penalty, sizeof seek_penalty,
                      &bytes_written, nullptr))
  {
    on_ssd= !seek_penalty.IncursSeekPenalty;
  }
  else
  {
    on_ssd= false;
  }
  CloseHandle(h);
  return on_ssd;
}

/*
  Checks whether volume is on SSD, by checking all physical drives
  in that volume.
*/
static bool is_volume_on_ssd(const char *volume_mount_point)
{
  char volume_name[MAX_PATH];

  if (!GetVolumeNameForVolumeMountPoint(volume_mount_point, volume_name,
                                        array_elements(volume_name)))
  {
    /* This can fail, e.g if file is on network share */
    return false;
  }

  /* Chomp last backslash, this is needed to open volume.*/
  size_t length= strlen(volume_name);
  if (length && volume_name[length - 1] == '\\')
    volume_name[length - 1]= 0;

  /* Open volume handle */
  HANDLE volume_handle= CreateFile(
      volume_name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

  if (volume_handle == INVALID_HANDLE_VALUE)
    return false;

  /*
   Enumerate all volume extends, check whether all of them are on SSD
  */

  /* Anticipate common case where there is only one extent.*/
  VOLUME_DISK_EXTENTS single_extent;

  /* But also have a place to manage allocated data.*/
  std::unique_ptr<BYTE[]> lifetime;

  DWORD bytes_written;
  VOLUME_DISK_EXTENTS *extents= nullptr;
  if (DeviceIoControl(volume_handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                      nullptr, 0, &single_extent, sizeof(single_extent),
                      &bytes_written, nullptr))
  {
    /* Worked on the first try. Use the preallocated buffer.*/
    extents= &single_extent;
  }
  else
  {
    VOLUME_DISK_EXTENTS *last_query= &single_extent;
    while (GetLastError() == ERROR_MORE_DATA)
    {
      DWORD extentCount= last_query->NumberOfDiskExtents;
      DWORD allocatedSize=
          FIELD_OFFSET(VOLUME_DISK_EXTENTS, Extents[extentCount]);
      lifetime.reset(new BYTE[allocatedSize]);
      last_query= (VOLUME_DISK_EXTENTS *) lifetime.get();
      if (DeviceIoControl(volume_handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                          nullptr, 0, last_query, allocatedSize,
                          &bytes_written, nullptr))
      {
        extents= last_query;
        break;
      }
    }
  }
  CloseHandle(volume_handle);
  if (!extents)
    return false;

  for (DWORD i= 0; i < extents->NumberOfDiskExtents; i++)
    if (!is_drive_on_ssd(extents->Extents[i].DiskNumber))
      return false;

  return true;
}

#include <unordered_map>
static bool is_path_on_ssd(char *file_path)
{
  /* Preset result, in case something fails, e.g we're on network drive.*/
  char volume_path[MAX_PATH];
  if (!GetVolumePathName(file_path, volume_path, array_elements(volume_path)))
    return false;
  return is_volume_on_ssd(volume_path);
}

static bool is_file_on_ssd(HANDLE handle, char *file_path)
{
  ULONGLONG volume_serial_number;
  FILE_ID_INFO info;
  if(!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
    return false;
  volume_serial_number= info.VolumeSerialNumber;

  static std::unordered_map<ULONGLONG, bool> cache;
  static SRWLOCK lock= SRWLOCK_INIT;
  bool found;
  bool result;
  AcquireSRWLockShared(&lock);
  auto e= cache.find(volume_serial_number);
  if ((found= e != cache.end()))
    result= e->second;
  ReleaseSRWLockShared(&lock);
  if (!found)
  {
    result= is_path_on_ssd(file_path);
    /* Update cache */
    AcquireSRWLockExclusive(&lock);
    cache[volume_serial_number]= result;
    ReleaseSRWLockExclusive(&lock);
  }
  return result;
}

#endif

void fil_node_t::find_metadata(IF_WIN(,bool create)) noexcept
{
  ut_ad(is_open());
  os_file_t file= handle;

  if (!space->is_compressed())
    punch_hole= 0;
  else if (my_test_if_thinly_provisioned(file))
    punch_hole= 2;
  else
    punch_hole= IF_WIN(, !create ||) os_is_sparse_file_supported(file);
  /* For temporary tablespace or during IMPORT TABLESPACE, we
  disable neighbour flushing and do not care about atomicity. */
  if (space->is_temporary())
  {
    on_ssd= true;
    atomic_write= true;
    return;
  }
  if (space->is_being_imported())
  {
    on_ssd= true;
    atomic_write= true;
    if (!space->is_compressed())
      return;
  }
#ifdef _WIN32
  on_ssd= is_file_on_ssd(file, name);
  FILE_STORAGE_INFO info;
  if (GetFileInformationByHandleEx(file, FileStorageInfo, &info, sizeof info))
    block_size= info.PhysicalBytesPerSectorForAtomicity;
  else
    block_size= 512;
#else
  struct stat statbuf;
  if (!fstat(file, &statbuf))
  {
    block_size= statbuf.st_blksize;
# ifdef __linux__
    on_ssd= fil_system.is_ssd(statbuf.st_dev);
# endif
  }
#endif

  /* On Windows, all single sector writes are atomic, as per
  WriteFile() documentation on MSDN. */
  atomic_write= srv_use_atomic_writes &&
    IF_WIN(srv_page_size == block_size,
           my_test_if_atomic_write(file, space->physical_size()));
}

/** Read the first page of a data file.
@return	whether the page was found valid */
bool fil_node_t::read_page0(const byte *dpage, bool no_lsn) noexcept
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  ut_ad(!dpage || no_lsn);
  const unsigned psize= space->physical_size();
  os_offset_t size_bytes= os_file_get_size(handle);
  if (size_bytes == os_offset_t(-1))
    return false;
  const uint32_t min_size= FIL_IBD_FILE_INITIAL_SIZE * psize;

  if (size_bytes < min_size)
  {
    ib::error() << "The size of the file " << name
      << " is only " << size_bytes
      << " bytes, should be at least " << min_size;
    return false;
  }

  if (!deferred)
  {
    page_t *apage= static_cast<byte*>(aligned_malloc(psize, psize));
    if (os_file_read(IORequestRead, handle, apage, 0, psize, nullptr) !=
        DB_SUCCESS)
    {
      sql_print_error("InnoDB: Unable to read first page of file %s", name);
   err_exit:
      aligned_free(apage);
      return false;
    }

    const page_t *page= apage;
  retry:
    const ulint space_id= memcmp_aligned<2>
      (FIL_PAGE_SPACE_ID + page,
       FSP_HEADER_OFFSET + FSP_SPACE_ID + page, 4)
      ? ULINT_UNDEFINED
      : mach_read_from_4(FIL_PAGE_SPACE_ID + page);
    uint32_t flags= fsp_header_get_flags(page);
    const uint32_t size= fsp_header_get_field(page, FSP_SIZE);
    if (!space_id && !flags && !size && dpage)
    {
    retry_dpage:
      page= dpage;
      dpage= nullptr;
      goto retry;
    }
    const uint32_t free_limit= fsp_header_get_field(page, FSP_FREE_LIMIT);
    const uint32_t free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE + page);

    if (!fil_space_t::is_valid_flags(flags, space->id))
    {
      uint32_t cflags= fsp_flags_convert_from_101(flags);
      if (cflags != UINT32_MAX)
      {
        uint32_t cf= cflags & ~FSP_FLAGS_MEM_MASK;
        uint32_t sf= space->flags & ~FSP_FLAGS_MEM_MASK;

        if (fil_space_t::is_flags_equal(cf, sf) ||
            fil_space_t::is_flags_equal(sf, cf))
        {
          flags= cflags;
          goto flags_ok;
        }
      }

      goto invalid;
    }

    if (!fil_space_t::is_flags_equal((flags & ~FSP_FLAGS_MEM_MASK),
                                     (space->flags & ~FSP_FLAGS_MEM_MASK)) &&
        !fil_space_t::is_flags_equal((space->flags & ~FSP_FLAGS_MEM_MASK),
                                     (flags & ~FSP_FLAGS_MEM_MASK)))
    {
    invalid:
      if (dpage)
        goto retry_dpage;
      sql_print_error("InnoDB: Expected tablespace flags 0x%" PRIx32
                      " but found 0x%" PRIx32
                      " in the file %s", space->flags, flags, name);
      goto err_exit;
    }

  flags_ok:
    ut_ad(!(flags & FSP_FLAGS_MEM_MASK));

    if (buf_page_is_corrupted(!no_lsn, page, flags) != NOT_CORRUPTED)
    {
      if (dpage)
        goto retry_dpage;
      sql_print_error("InnoDB: The first page of file %s is corrupted", name);
      goto err_exit;
    }

    if (UNIV_UNLIKELY(space_id != space->id))
    {
      if (dpage)
        goto retry_dpage;
      sql_print_error("InnoDB: Expected tablespace id %zu but found %zu"
                      " in the file %s", ulint{space->id}, ulint{space_id},
                      name);
      goto err_exit;
    }

    /* Try to read crypt_data from page 0 if it is not yet read. */
    if (!space->crypt_data)
      space->crypt_data= fil_space_read_crypt_data(
        fil_space_t::zip_size(flags), page);
    aligned_free(apage);

    space->flags= (space->flags & FSP_FLAGS_MEM_MASK) | flags;
    ut_ad(space->free_limit == 0 || space->free_limit == free_limit);
    ut_ad(space->free_len == 0 || space->free_len == free_len);
    space->size_in_header= size;
    space->free_limit= free_limit;
    space->free_len= free_len;
  }

  find_metadata();
  /* Truncate the size to a multiple of extent size. */
  ulint	mask= psize * FSP_EXTENT_SIZE - 1;

  if (size_bytes <= mask);
    /* .ibd files start smaller than an
    extent size. Do not truncate valid data. */
  else
    size_bytes&= ~os_offset_t(mask);

  this->size= uint32_t(size_bytes / psize);
  space->set_sizes(this->size);
  return true;
}
