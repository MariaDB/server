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
#ifdef HAVE_LINUX_UNISTD_H
#include "unistd.h"
#endif
#include "buf0dblwr.h"

#include <tpool_structs.h>

#ifdef LINUX_NATIVE_AIO
#include <libaio.h>
#endif /* LINUX_NATIVE_AIO */

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
# include <fcntl.h>
# include <linux/falloc.h>
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

#ifdef _WIN32
#include <winioctl.h>
#endif

// my_test_if_atomic_write() , my_win_secattr()
#include <my_sys.h>

#include <thread>
#include <chrono>
#include <memory>

/* Per-IO operation environment*/
class io_slots
{
private:
	tpool::cache<tpool::aiocb> m_cache;
	tpool::task_group m_group;
	int m_max_aio;
public:
	io_slots(int max_submitted_io, int max_callback_concurrency) :
		m_cache(max_submitted_io),
		m_group(max_callback_concurrency),
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
	void wait()
	{
		m_cache.wait();
	}

	size_t pending_io_count()
	{
		return (size_t)m_max_aio - m_cache.size();
	}

	tpool::task_group* get_task_group()
	{
		return &m_group;
	}

	~io_slots()
	{
		wait();
	}
};

static std::unique_ptr<io_slots> read_slots;
static std::unique_ptr<io_slots> write_slots;

/** Number of retries for partial I/O's */
constexpr ulint NUM_RETRIES_ON_PARTIAL_IO = 10;

/* This specifies the file permissions InnoDB uses when it creates files in
Unix; the value of os_innodb_umask is initialized in ha_innodb.cc to
my_umask */

#ifndef _WIN32
/** Umask for creating files */
static ulint	os_innodb_umask = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
#else
/** Umask for creating files */
static ulint	os_innodb_umask	= 0;
#endif /* _WIN32 */

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
static MY_ATTRIBUTE((warn_unused_result))
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_abort,
	bool		on_error_silent);

/** Does error handling when a file operation fails.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation name that failed
@return true if we should retry the operation */
static
bool
os_file_handle_error(
	const char*	name,
	const char*	operation)
{
	/* Exit in case of unknown error */
	return(os_file_handle_error_cond_exit(name, operation, true, false));
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
};

#undef USE_FILE_LOCK
#ifndef _WIN32
/* On Windows, mandatory locking is used */
# define USE_FILE_LOCK
#endif
#ifdef USE_FILE_LOCK
/** Obtain an exclusive lock on a file.
@param[in]	fd		file descriptor
@param[in]	name		file name
@return 0 on success */
static
int
os_file_lock(
	int		fd,
	const char*	name)
{
	if (my_disable_locking) {
		return 0;
	}

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
#endif /* USE_FILE_LOCK */


/** Create a temporary file. This function is like tmpfile(3), but
the temporary file is created in the in the mysql server configuration
parameter (--tmpdir).
@return temporary file handle, or NULL on error */
FILE*
os_file_create_tmpfile()
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
	ulint		size)
{
	if (size != 0) {
		rewind(file);

		size_t	flen = fread(str, 1, size - 1, file);

		str[flen] = '\0';
	}
}

/** This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@param[in]	old_path		Pathname
@param[in]	tablename		Contains new base name
@return own: new full pathname */
char *os_file_make_new_pathname(const char *old_path, const char *tablename)
{
  /* Split the tablename into its database and table name components.
  They are separated by a '/'. */
  const char *last_slash= strrchr(tablename, '/');
  const char *base_name= last_slash ? last_slash + 1 : tablename;

  /* Find the offset of the last slash. We will strip off the
  old basename.ibd which starts after that slash. */
  last_slash = strrchr(old_path, '/');
#ifdef _WIN32
  if (const char *last= strrchr(old_path, '\\'))
    if (last > last_slash)
      last_slash= last;
#endif

  size_t dir_len= last_slash
    ? size_t(last_slash - old_path)
    : strlen(old_path);

  /* allocate a new path and move the old directory path to it. */
  size_t new_path_len= dir_len + strlen(base_name) + sizeof "/.ibd";
  char *new_path= static_cast<char*>(ut_malloc_nokey(new_path_len));
  memcpy(new_path, old_path, dir_len);
  snprintf(new_path + dir_len, new_path_len - dir_len, "/%s.ibd", base_name);
  return new_path;
}

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
	char*	data_dir_path)
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
UNIV_INLINE
bool
os_file_is_root(
	const char*	path,
	const char*	last_slash)
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
	const char*	expected_dir)
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
unit_test_os_file_get_parent_dir()
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


/** Creates all missing subdirectories along the given path.
@param[in]	path		Path name
@return DB_SUCCESS if OK, otherwise error code. */
dberr_t
os_file_create_subdirs_if_needed(
	const char*	path)
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
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute(const IORequest& request)
{
	ssize_t	n_bytes;

	if (request.is_read()) {
#ifdef _WIN32
		n_bytes = tpool::pread(m_fh, m_buf, m_n, m_offset);
#else
		n_bytes = pread(m_fh, m_buf, m_n, m_offset);
#endif
	} else {
		ut_ad(request.is_write());
#ifdef _WIN32
		n_bytes = tpool::pwrite(m_fh, m_buf, m_n, m_offset);
#else
		n_bytes = pwrite(m_fh, m_buf, m_n, m_offset);
#endif
	}

	return(n_bytes);
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

#elif defined(UNIV_SOLARIS)

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
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
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
static int os_file_sync_posix(os_file_t file)
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
	os_file_type_t* type)
{
	struct stat	statinfo;

	int	ret = stat(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */
		MSAN_STAT_WORKAROUND(&statinfo);
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

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool
os_file_flush_func(
	os_file_t	file)
{
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

	os_file_handle_error(NULL, "flush");

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
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	pfs_os_file_t	file;

	*success = false;

	int		create_flag;
	const char*	mode_str	= NULL;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {
		mode_str = "OPEN";

		if (access_type == OS_FILE_READ_ONLY) {

			create_flag = O_RDONLY;

		} else if (read_only) {

			create_flag = O_RDONLY;

		} else {
			create_flag = O_RDWR;
		}

	} else if (read_only) {

		mode_str = "OPEN";
		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		mode_str = "CREATE";
		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		mode_str = "CREATE PATH";
		/* Create subdirs along the path if needed. */

		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib::error()
				<< "Unable to create subdirectories '"
				<< name << "'";

			return(OS_FILE_CLOSED);
		}

		create_flag = O_RDWR | O_CREAT | O_EXCL;
		create_mode = OS_FILE_CREATE;
	} else {

		ib::error()
			<< "Unknown file create mode ("
			<< create_mode
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	bool	retry;

	do {
		file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

		if (file == -1) {
			*success = false;
			retry = os_file_handle_error(
				name,
				create_mode == OS_FILE_OPEN
				? "open" : "create");
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	/* This function is always called for data files, we should disable
	OS caching (O_DIRECT) here as we do in os_file_create_func(), so
	we open the same file in the same mode, see man page of open(2). */
	if (!srv_read_only_mode && *success) {
		switch (srv_file_flush_method) {
		case SRV_O_DIRECT:
		case SRV_O_DIRECT_NO_FSYNC:
			os_file_set_nocache(file, name, mode_str);
			break;
		default:
			break;
		}
	}

#ifdef USE_FILE_LOCK
	if (!read_only
	    && *success
	    && (access_type == OS_FILE_READ_WRITE)
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;
	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/** This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated as
				an error.
@return true if call succeeds, false on error */
bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
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

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use async
				I/O or unbuffered I/O: look in the function
				source code for the exact rules
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	read_only	true, if read only checks should be enforcedm
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success)
{
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		errno = ENOSPC;
		return(OS_FILE_CLOSED);
	);

	int		create_flag;
	const char*	mode_str	= NULL;

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;
	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ulint(~(OS_FILE_ON_ERROR_NO_EXIT
			       | OS_FILE_ON_ERROR_SILENT));

	if (create_mode == OS_FILE_OPEN
	    || create_mode == OS_FILE_OPEN_RAW
	    || create_mode == OS_FILE_OPEN_RETRY) {

		mode_str = "OPEN";

		create_flag = read_only ? O_RDONLY : O_RDWR;

	} else if (read_only) {

		mode_str = "OPEN";

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		mode_str = "CREATE";
		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		mode_str = "OVERWRITE";
		create_flag = O_RDWR | O_CREAT | O_TRUNC;

	} else {
		ib::error()
			<< "Unknown file create mode (" << create_mode << ")"
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	ut_a(type == OS_LOG_FILE
	     || type == OS_DATA_FILE
	     || type == OS_DATA_FILE_NO_O_DIRECT);

	ut_a(purpose == OS_FILE_AIO || purpose == OS_FILE_NORMAL);

	/* We let O_DSYNC only affect log files */

	if (!read_only
	    && type == OS_LOG_FILE
	    && srv_file_flush_method == SRV_O_DSYNC) {
#ifdef O_DSYNC
		create_flag |= O_DSYNC;
#else
		create_flag |= O_SYNC;
#endif
	}

	os_file_t	file;
	bool		retry;

	do {
		file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

		if (file == -1) {
			const char*	operation;

			operation = (create_mode == OS_FILE_CREATE
				     && !read_only) ? "create" : "open";

			*success = false;

			if (on_error_no_exit) {
				retry = os_file_handle_error_no_exit(
					name, operation, on_error_silent);
			} else {
				retry = os_file_handle_error(name, operation);
			}
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	if (!*success) {
		return file;
	}

#if (defined(UNIV_SOLARIS) && defined(DIRECTIO_ON)) || defined O_DIRECT
	if (type == OS_DATA_FILE) {
# ifdef __linux__
use_o_direct:
# endif
		switch (srv_file_flush_method) {
		case SRV_O_DSYNC:
		case SRV_O_DIRECT:
		case SRV_O_DIRECT_NO_FSYNC:
			os_file_set_nocache(file, name, mode_str);
			break;
		default:
			break;
		}
	}
# ifdef __linux__
	else if (type == OS_LOG_FILE && !log_sys.is_opened()) {
		struct stat st;
		char b[20 + sizeof "/sys/dev/block/" ":"
		       "/../queue/physical_block_size"];
		int f;
		if (fstat(file, &st)) {
			goto skip_o_direct;
		}
		MSAN_STAT_WORKAROUND(&st);
		if (st.st_size & 4095) {
			goto skip_o_direct;
		}
		if (snprintf(b, sizeof b,
			     "/sys/dev/block/%u:%u/queue/physical_block_size",
			     major(st.st_dev), minor(st.st_dev))
		    >= static_cast<int>(sizeof b)) {
			goto skip_o_direct;
		}
		if ((f = open(b, O_RDONLY)) == -1) {
			if (snprintf(b, sizeof b,
				     "/sys/dev/block/%u:%u/../queue/"
				     "physical_block_size",
				     major(st.st_dev), minor(st.st_dev))
			    >= static_cast<int>(sizeof b)) {
				goto skip_o_direct;
			}
			f = open(b, O_RDONLY);
		}
		if (f != -1) {
			ssize_t l = read(f, b, sizeof b);
			unsigned long s = 0;

			if (l > 0 && static_cast<size_t>(l) < sizeof b
			    && b[l - 1] == '\n') {
				char* end = b;
				s = strtoul(b, &end, 10);
				if (b == end || *end != '\n') {
					s = 0;
				}
			}
			close(f);
			if (s > 4096 || s < 64 || !ut_is_2pow(s)) {
				goto skip_o_direct;
			}
			log_sys.set_block_size(uint32_t(s));
			goto use_o_direct;
		} else {
skip_o_direct:
			log_sys.set_block_size(0);
		}
	}
# endif
#endif

#ifdef USE_FILE_LOCK
	if (!read_only
	    && create_mode != OS_FILE_OPEN_RAW && os_file_lock(file, name)) {
		if (create_mode == OS_FILE_OPEN_RETRY) {
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
#endif /* USE_FILE_LOCK */

	return(file);
}

/** NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
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
{
	os_file_t	file;
	int		create_flag;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	*success = false;

	if (create_mode == OS_FILE_OPEN) {

		if (access_type == OS_FILE_READ_ONLY) {

			create_flag = O_RDONLY;

		} else if (read_only) {

			create_flag = O_RDONLY;

		} else {

			ut_a(access_type == OS_FILE_READ_WRITE
			     || access_type == OS_FILE_READ_ALLOW_DELETE);

			create_flag = O_RDWR;
		}

	} else if (read_only) {

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else {

		ib::error()
			<< "Unknown file create mode "
			<< create_mode << " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

	*success = (file != -1);

#ifdef USE_FILE_LOCK
	if (!read_only
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;

	}
#endif /* USE_FILE_LOCK */

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

  os_file_handle_error(NULL, "close");
  return false;
}

/** Gets a file size.
@param[in]	file		handle to an open file
@return file size, or (os_offset_t) -1 on failure */
os_offset_t
os_file_get_size(os_file_t file)
{
  struct stat statbuf;
  if (fstat(file, &statbuf)) return os_offset_t(-1);
  MSAN_STAT_WORKAROUND(&statbuf);
  return statbuf.st_size;
}

/** Gets a file size.
@param[in]	filename	Full path to the filename to check
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to
	errno */
os_file_size_t
os_file_get_size(
	const char*	filename)
{
	struct stat	s;
	os_file_size_t	file_size;

	int	ret = stat(filename, &s);

	if (ret == 0) {
		MSAN_STAT_WORKAROUND(&s);
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

	MSAN_STAT_WORKAROUND(statinfo);

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

/** Truncates a file at its current position.
@return true if success */
bool
os_file_set_eof(
	FILE*		file)	/*!< in: file to be truncated */
{
	return(!ftruncate(fileno(file), ftell(file)));
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
bool os_file_flush_func(os_file_t file)
{
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

  os_file_handle_error(nullptr, "flush");

  /* It is a fatal error if a file flush does not succeed, because then
  the database can get corrupt on disk */
  ut_error;

  return false;
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
then OS error number + OS_FILE_ERROR_MAX is returned.
@param[in]	report_all_errors	true if we want an error message printed
					of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
					to the log
@return error number, or OS error number + OS_FILE_ERROR_MAX */
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
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
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes = 0;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));
	ut_ad(srv_operation == SRV_OPERATION_NORMAL);

	if (create_mode == OS_FILE_OPEN) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		/* Create subdirs along the path if needed. */
		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib::error()
				<< "Unable to create subdirectories '"
				<< name << "'";

			return(OS_FILE_CLOSED);
		}

		create_flag = CREATE_NEW;
		create_mode = OS_FILE_CREATE;

	} else {

		ib::error()
			<< "Unknown file create mode ("
			<< create_mode << ") for file '"
			<< name << "'";

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {

		access = GENERIC_READ;

	} else if (read_only) {

		ib::info()
			<< "Read only mode set. Unable to"
			" open file '" << name << "' in RW mode, "
			<< "trying RO mode";

		access = GENERIC_READ;

	} else if (access_type == OS_FILE_READ_WRITE) {

		access = GENERIC_READ | GENERIC_WRITE;

	} else {

		ib::error()
			<< "Unknown file access type (" << access_type << ") "
			"for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	bool	retry;

	do {
		/* Use default security attributes and no template file. */

		file = CreateFile(
			(LPCTSTR) name, access,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			my_win_file_secattr(), create_flag, attributes, NULL);

		if (file == INVALID_HANDLE_VALUE) {

			*success = false;

			retry = os_file_handle_error(
				name, create_mode == OS_FILE_OPEN ?
				"open" : "create");

		} else {

			retry = false;

			*success = true;
		}

	} while (retry);

	return(file);
}

/** This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated
				as an error.
@return true if call succeeds, false on error */
bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
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
size_t get_sector_size(HANDLE file)
{
  FILE_STORAGE_INFO fsi;
  ULONG s= 4096;
  if (GetFileInformationByHandleEx(file, FileStorageInfo, &fsi, sizeof fsi))
  {
    s= fsi.PhysicalBytesPerSectorForPerformance;
    if (s > 4096 || s < 64 || !ut_is_2pow(s))
    {
      return 4096;
    }
  }
  return s;
}

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use async
				I/O or unbuffered I/O: look in the function
				source code for the exact rules
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;
	bool		retry;
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		SetLastError(ERROR_DISK_FULL);
		return(OS_FILE_CLOSED);
	);

	DWORD		create_flag;
	DWORD		share_mode = read_only
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ | FILE_SHARE_DELETE;

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;

	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ~(OS_FILE_ON_ERROR_NO_EXIT | OS_FILE_ON_ERROR_SILENT);

	if (create_mode == OS_FILE_OPEN_RAW) {

		ut_a(!read_only);

		/* On Windows Physical devices require admin privileges and
		have to have the write-share mode set. See the remarks
		section for the CreateFile() function documentation in MSDN. */

		share_mode |= FILE_SHARE_WRITE;

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_OPEN
		   || create_mode == OS_FILE_OPEN_RETRY) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		create_flag = CREATE_ALWAYS;

	} else {
		ib::error()
			<< "Unknown file create mode (" << create_mode << ") "
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	DWORD attributes = (purpose == OS_FILE_AIO && srv_use_native_aio)
		? FILE_FLAG_OVERLAPPED : 0;

	if (type == OS_LOG_FILE) {
		if(srv_flush_log_at_trx_commit != 2 && !log_sys.is_opened())
			attributes|= FILE_FLAG_NO_BUFFERING;
		if (srv_file_flush_method == SRV_O_DSYNC)
			attributes|= FILE_FLAG_WRITE_THROUGH;
	}
	else if (type == OS_DATA_FILE)
	{
		switch (srv_file_flush_method)
		{
		case SRV_FSYNC:
		case SRV_LITTLESYNC:
		case SRV_NOSYNC:
			break;
		default:
			attributes|= FILE_FLAG_NO_BUFFERING;
		}
	}

	DWORD	access = GENERIC_READ;

	if (!read_only) {
		access |= GENERIC_WRITE;
	}

	for (;;) {
		const  char *operation;

		/* Use default security attributes and no template file. */
		file = CreateFile(
			name, access, share_mode, my_win_file_secattr(),
			create_flag, attributes, NULL);

		if (file != INVALID_HANDLE_VALUE && type == OS_LOG_FILE
			&& (attributes & FILE_FLAG_NO_BUFFERING)) {
			uint32 s= (uint32_t) get_sector_size(file);
			log_sys.set_block_size(uint32_t(s));
			/* FIXME! remove it when backup is fixed, so that it
				does not produce redo with irregular sizes.*/
			if (os_file_get_size(file) % s) {
				attributes &= ~FILE_FLAG_NO_BUFFERING;
				create_flag = OPEN_ALWAYS;
				CloseHandle(file);
				continue;
			}
		}

		*success = (file != INVALID_HANDLE_VALUE);
		if (*success) {
			break;
		}

		operation = (create_mode == OS_FILE_CREATE && !read_only) ?
			"create" : "open";

		if (on_error_no_exit) {
			retry = os_file_handle_error_no_exit(
				name, operation, on_error_silent);
		}
		else {
			retry = os_file_handle_error(name, operation);
		}

		if (!retry) {
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
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes	= 0;
	DWORD		share_mode = read_only
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ | FILE_SHARE_DELETE;

	ut_a(name);

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else {

		ib::error()
			<< "Unknown file create mode (" << create_mode << ") "
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {

		access = GENERIC_READ;

	} else if (read_only) {

		access = GENERIC_READ;

	} else if (access_type == OS_FILE_READ_WRITE) {

		access = GENERIC_READ | GENERIC_WRITE;

	} else if (access_type == OS_FILE_READ_ALLOW_DELETE) {

		ut_a(!read_only);

		access = GENERIC_READ;

		/*!< A backup program has to give mysqld the maximum
		freedom to do what it likes with the file */

		share_mode |= FILE_SHARE_DELETE | FILE_SHARE_WRITE
			| FILE_SHARE_READ;

	} else {

		ib::error()
			<< "Unknown file access type (" << access_type << ") "
			<< "for file '" << name << "'";

		return(OS_FILE_CLOSED);
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

	if (MoveFileEx(oldpath, newpath, MOVEFILE_REPLACE_EXISTING)) {
		return(true);
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
    os_file_handle_error(NULL, "close");
    return false;
  }

  if(srv_thread_pool)
    srv_thread_pool->unbind(file);
  return true;
}

/** Gets a file size.
@param[in]	file		Handle to a file
@return file size, or (os_offset_t) -1 on failure */
os_offset_t os_file_get_size(os_file_t file)
{
  LARGE_INTEGER li;
  if (GetFileSizeEx(file, &li))
    return li.QuadPart;
  return ((os_offset_t) -1);
}

/** Gets a file size.
@param[in]	filename	Full path to the filename to check
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to
	errno */
os_file_size_t
os_file_get_size(
	const char*	filename)
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
bool os_file_set_sparse_win32(os_file_t file, bool is_sparse)
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


/**
Change file size on Windows.

If file is extended, the bytes between old and new EOF
are zeros.

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
	os_offset_t	size)
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

/** Truncates a file at its current position.
@param[in]	file		Handle to be truncated
@return true if success */
bool
os_file_set_eof(
	FILE*		file)
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
			const char*	op = type.is_read()
				? "read" : "written";

			ib::warn()
				<< n
				<< " bytes should have been " << op << ". Only "
				<< bytes_returned
				<< " bytes " << op << ". Retrying"
				<< " for the remaining bytes.";
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
@param[in]	n		number of bytes to read, starting from offset
@param[in]	offset		file offset from the start where to read
@param[out]	err		DB_SUCCESS or error code
@return number of bytes written, -1 if error */
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
@param[in]	exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
os_file_read_page(
	const IORequest&	type,
	os_file_t	file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o,
	bool			exit_on_err)
{
	dberr_t		err;

	os_bytes_read_since_printout += n;

	ut_ad(n > 0);

	ssize_t	n_bytes = os_file_pread(type, file, buf, n, offset, &err);

	if (o) {
		*o = n_bytes;
	}

	if (ulint(n_bytes) == n || (err != DB_SUCCESS && !exit_on_err)) {
		return err;
	}
	int os_err = IF_WIN((int)GetLastError(), errno);

	if (!os_file_handle_error_cond_exit(
		    NULL, "read", exit_on_err, false)) {
		ib::fatal()
			<< "Tried to read " << n << " bytes at offset "
			<< offset << ", but was only able to read " << n_bytes
			<< ".Cannot read from file. OS error number "
			<< os_err << ".";
	} else {
		ib::error() << "Tried to read " << n << " bytes at offset "
		<< offset << ", but was only able to read " << n_bytes;
	}
	if (err == DB_SUCCESS) {
		err = DB_IO_ERROR;
	}

	return err;
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in]	report_all_errors	true if we want an error printed
					for all errors
@return error number, or OS error number + 100 */
ulint
os_file_get_last_error(
	bool	report_all_errors)
{
	return(os_file_get_last_error_low(report_all_errors, false));
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
	bool		on_error_silent)
{
	ulint	err;

	err = os_file_get_last_error_low(false, on_error_silent);

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

#ifndef _WIN32
/** Tries to disable OS caching on an opened file descriptor.
@param[in]	fd		file descriptor to alter
@param[in]	file_name	file name, used in the diagnostic message
@param[in]	name		"open" or "create"; used in the diagnostic
				message */
void
os_file_set_nocache(
	int	fd		MY_ATTRIBUTE((unused)),
	const char*	file_name	MY_ATTRIBUTE((unused)),
	const char*	operation_name	MY_ATTRIBUTE((unused)))
{
	/* some versions of Solaris may not have DIRECTIO_ON */
#if defined(UNIV_SOLARIS) && defined(DIRECTIO_ON)
	if (directio(fd, DIRECTIO_ON) == -1) {
		int	errno_save = errno;

		ib::error()
			<< "Failed to set DIRECTIO_ON on file "
			<< file_name << "; " << operation_name << ": "
			<< strerror(errno_save) << ","
			" continuing anyway.";
	}
#elif defined(O_DIRECT)
	if (fcntl(fd, F_SETFL, O_DIRECT) == -1) {
		int		errno_save = errno;
		static bool	warning_message_printed = false;
		if (errno_save == EINVAL) {
			if (!warning_message_printed) {
				warning_message_printed = true;
				ib::info()
					<< "Setting O_DIRECT on file "
					<< file_name << " failed";
			}
		} else {
			ib::warn()
				<< "Failed to set O_DIRECT on file "
				<< file_name << "; " << operation_name
				<< " : " << strerror(errno_save)
				<< ", continuing anyway.";
		}
	}
#endif /* defined(UNIV_SOLARIS) && defined(DIRECTIO_ON) */
}

#endif /* _WIN32 */

/** Check if the file system supports sparse files.
@param fh	file handle
@return true if the file system supports sparse files */
static bool os_is_sparse_file_supported(os_file_t fh)
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
	bool	is_sparse)
{
	ut_ad(!(size & 4095));

#ifdef _WIN32
	/* On Windows, changing file size works well and as expected for both
	sparse and normal files.

	However, 10.2 up until 10.2.9 made every file sparse in innodb,
	causing NTFS fragmentation issues(MDEV-13941). We try to undo
	the damage, and unsparse the file.*/

	if (!is_sparse && os_is_sparse_file_supported(file)) {
		if (!os_file_set_sparse_win32(file, false))
			/* Unsparsing file failed. Fallback to writing binary
			zeros, to avoid even higher fragmentation.*/
			goto fallback;
	}

	return os_file_change_size_win32(name, file, size);

fallback:
#else
	struct stat statbuf;

	if (is_sparse) {
		bool success = !ftruncate(file, size);
		if (!success) {
			ib::error() << "ftruncate of file " << name << " to "
				    << size << " bytes failed with error "
				    << errno;
		}
		return(success);
	}

# ifdef HAVE_POSIX_FALLOCATE
	int err;
	do {
		if (fstat(file, &statbuf)) {
			err = errno;
		} else {
			MSAN_STAT_WORKAROUND(&statbuf);
			os_offset_t current_size = statbuf.st_size;
			if (current_size >= size) {
				return true;
			}
			current_size &= ~4095ULL;
			err = posix_fallocate(file, current_size,
					      size - current_size);
		}
	} while (err == EINTR
		 && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED);

	switch (err) {
	case 0:
		return true;
	default:
		ib::error() << "preallocating "
			    << size << " bytes for file " << name
			    << " failed with error " << err;
		/* fall through */
	case EINTR:
		errno = err;
		return false;
	case EINVAL:
	case EOPNOTSUPP:
		/* fall back to the code below */
		break;
	}
# endif /* HAVE_POSIX_ALLOCATE */
#endif /* _WIN32*/

#ifdef _WIN32
	os_offset_t	current_size = os_file_get_size(file);
	FILE_STORAGE_INFO info;
	if (GetFileInformationByHandleEx(file, FileStorageInfo, &info,
					 sizeof info)) {
		if (info.LogicalBytesPerSector) {
			current_size &= ~os_offset_t(info.LogicalBytesPerSector
						     - 1);
		}
	}
#else
	if (fstat(file, &statbuf)) {
		return false;
	}
	os_offset_t current_size = statbuf.st_size & ~4095ULL;
#endif
	if (current_size >= size) {
		return true;
	}

	/* Write up to 1 megabyte at a time. */
	ulint	buf_size = ut_min(ulint(64),
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

	return(current_size >= size && os_file_flush(file));
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
	bool		allow_shrink)
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
	return(os_file_change_size_win32(pathname, file, size));
#else /* _WIN32 */
	return(os_file_truncate_posix(pathname, file, size));
#endif /* _WIN32 */
}

/** NOTE! Use the corresponding macro os_file_read(), not directly this
function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, DB_IO_ERROR on failure
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@return error code
@retval	DB_SUCCESS	if the operation succeeded */
dberr_t
os_file_read_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n)
{
	return(os_file_read_page(type, file, buf, offset, n, NULL, true));
}

/** NOTE! Use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, DB_IO_ERROR on failure
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	o		number of bytes actually read
@return DB_SUCCESS or error code */
dberr_t
os_file_read_no_error_handling_func(
	const IORequest&	type,
	os_file_t	file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o)
{
	return(os_file_read_page(type, file, buf, offset, n, o, false));
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
	os_file_type_t* type)
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
	os_offset_t	len)
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
dberr_t IORequest::punch_hole(os_offset_t off, ulint len) const
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
	bool		read_only)
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


extern void fil_aio_callback(const IORequest &request);

static void io_callback(tpool::aiocb *cb)
{
  ut_a(cb->m_err == DB_SUCCESS);
  const IORequest &request= *static_cast<const IORequest*>
    (static_cast<const void*>(cb->m_userdata));

  /* Return cb back to cache*/
  if (cb->m_opcode == tpool::aio_opcode::AIO_PREAD)
  {
    ut_ad(read_slots->contains(cb));
    fil_aio_callback(request);
    read_slots->release(cb);
  }
  else
  {
    ut_ad(write_slots->contains(cb));
    const IORequest req{request};
    write_slots->release(cb);
    fil_aio_callback(req);
  }
}

#ifdef LINUX_NATIVE_AIO
/** Checks if the system supports native linux aio. On some kernel
versions where native aio is supported it won't work on tmpfs. In such
cases we can't use native aio.

@return: true if supported, false otherwise. */
static bool is_linux_native_aio_supported()
{
	File		fd;
	io_context_t	io_ctx;
	std::string log_file_path = get_log_file_path();

	memset(&io_ctx, 0, sizeof(io_ctx));
	if (io_setup(1, &io_ctx)) {

		/* The platform does not support native aio. */

		return(false);

	}
	else if (!srv_read_only_mode) {

		/* Now check if tmpdir supports native aio ops. */
		fd = mysql_tmpfile("ib");

		if (fd < 0) {
			ib::warn()
				<< "Unable to create temp file to check"
				" native AIO support.";

			int ret = io_destroy(io_ctx);
			ut_a(ret != -EINVAL);
			ut_ad(ret != -EFAULT);

			return(false);
		}
	}
	else {
		fd = my_open(log_file_path.c_str(), O_RDONLY | O_CLOEXEC,
			     MYF(0));

		if (fd == -1) {

			ib::warn() << "Unable to open \"" << log_file_path
				   << "\" to check native"
				   << " AIO read support.";

			int ret = io_destroy(io_ctx);
			ut_a(ret != EINVAL);
			ut_ad(ret != EFAULT);

			return(false);
		}
	}

	struct io_event	io_event;

	memset(&io_event, 0x0, sizeof(io_event));

	byte* ptr = static_cast<byte*>(aligned_malloc(srv_page_size,
						      srv_page_size));

	struct iocb	iocb;

	/* Suppress valgrind warning. */
	memset(ptr, 0, srv_page_size);
	memset(&iocb, 0x0, sizeof(iocb));

	struct iocb* p_iocb = &iocb;

	if (!srv_read_only_mode) {

		io_prep_pwrite(p_iocb, fd, ptr, srv_page_size, 0);

	}
	else {
		ut_a(srv_page_size >= 512);
		io_prep_pread(p_iocb, fd, ptr, 512, 0);
	}

	int	err = io_submit(io_ctx, 1, &p_iocb);

	if (err >= 1) {
		/* Now collect the submitted IO request. */
		err = io_getevents(io_ctx, 1, 1, &io_event, NULL);
	}

	aligned_free(ptr);
	my_close(fd, MYF(MY_WME));

	switch (err) {
	case 1:
		{
			int ret = io_destroy(io_ctx);
			ut_a(ret != -EINVAL);
			ut_ad(ret != -EFAULT);

			return(true);
		}

	case -EINVAL:
	case -ENOSYS:
		ib::warn()
			<< "Linux Native AIO not supported. You can either"
			" move "
			<< (srv_read_only_mode ? log_file_path : "tmpdir")
			<< " to a file system that supports native"
			" AIO or you can set innodb_use_native_aio to"
			" FALSE to avoid this message.";

		/* fall through. */
	default:
		ib::warn()
			<< "Linux Native AIO check on "
			<< (srv_read_only_mode ? log_file_path : "tmpdir")
			<< "returned error[" << -err << "]";
	}

	int ret = io_destroy(io_ctx);
	ut_a(ret != -EINVAL);
	ut_ad(ret != -EFAULT);

	return(false);
}
#endif

int os_aio_init()
{
  int max_write_events= int(srv_n_write_io_threads *
                            OS_AIO_N_PENDING_IOS_PER_THREAD);
  int max_read_events= int(srv_n_read_io_threads *
                           OS_AIO_N_PENDING_IOS_PER_THREAD);
  int max_events= max_read_events + max_write_events;

  read_slots.reset(new io_slots(max_read_events, srv_n_read_io_threads));
  write_slots.reset(new io_slots(max_write_events, srv_n_write_io_threads));

  int ret;
#if LINUX_NATIVE_AIO
  if (srv_use_native_aio && !is_linux_native_aio_supported())
    goto disable;
#endif

  ret= srv_thread_pool->configure_aio(srv_use_native_aio, max_events);

#ifdef LINUX_NATIVE_AIO
  if (ret)
  {
    ut_ad(srv_use_native_aio);
disable:
    ib::warn() << "Linux Native AIO disabled.";
    srv_use_native_aio= false;
    ret= srv_thread_pool->configure_aio(false, max_events);
  }
#endif

#ifdef HAVE_URING
  if (ret)
  {
    ut_ad(srv_use_native_aio);
    ib::warn()
	    << "liburing disabled: falling back to innodb_use_native_aio=OFF";
    srv_use_native_aio= false;
    ret= srv_thread_pool->configure_aio(false, max_events);
  }
#endif

  if (ret)
  {
    read_slots.reset();
    write_slots.reset();
  }

  return ret;
}


void os_aio_free()
{
  srv_thread_pool->disable_aio();
  read_slots.reset();
  write_slots.reset();
}

/** Wait until there are no pending asynchronous writes. */
static void os_aio_wait_until_no_pending_writes_low()
{
  bool notify_wait = write_slots->pending_io_count() > 0;

  if (notify_wait)
    tpool::tpool_wait_begin();

   write_slots->wait();

   if (notify_wait)
     tpool::tpool_wait_end();
}

/** Wait until there are no pending asynchronous writes. */
void os_aio_wait_until_no_pending_writes()
{
  os_aio_wait_until_no_pending_writes_low();
  buf_dblwr.wait_flush_buffered_writes();
}

/** Wait until all pending asynchronous reads have completed. */
void os_aio_wait_until_no_pending_reads()
{
  const auto notify_wait= read_slots->pending_io_count();

  if (notify_wait)
    tpool::tpool_wait_begin();

  read_slots->wait();

  if (notify_wait)
    tpool::tpool_wait_end();
}

/** Request a read or write.
@param type		I/O request
@param buf		buffer
@param offset		file offset
@param n		number of bytes
@retval DB_SUCCESS if request was queued successfully
@retval DB_IO_ERROR on I/O error */
dberr_t os_aio(const IORequest &type, void *buf, os_offset_t offset, size_t n)
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
					    buf, offset, n)
			: os_file_write_func(type, type.node->name,
					     type.node->handle,
					     buf, offset, n);
func_exit:
#ifdef UNIV_PFS_IO
		register_pfs_file_io_end(locker, n);
#endif /* UNIV_PFS_IO */
		return err;
	}

	if (type.is_read()) {
		++os_n_file_reads;
	} else {
		++os_n_file_writes;
	}

	compile_time_assert(sizeof(IORequest) <= tpool::MAX_AIO_USERDATA_LEN);
	io_slots* slots= type.is_read() ? read_slots.get() : write_slots.get();
	tpool::aiocb* cb = slots->acquire();

	cb->m_buffer = buf;
	cb->m_callback = (tpool::callback_func)io_callback;
	cb->m_group = slots->get_task_group();
	cb->m_fh = type.node->handle.m_file;
	cb->m_len = (int)n;
	cb->m_offset = offset;
	cb->m_opcode = type.is_read() ? tpool::aio_opcode::AIO_PREAD : tpool::aio_opcode::AIO_PWRITE;
	new (cb->m_userdata) IORequest{type};

	if (srv_thread_pool->submit_io(cb)) {
		slots->release(cb);
		os_file_handle_error(type.node->name, type.is_read()
				     ? "aio read" : "aio write");
		err = DB_IO_ERROR;
	}

	goto func_exit;
}

/** Prints info of the aio arrays.
@param[in,out]	file		file where to print */
void
os_aio_print(FILE*	file)
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

/** Refreshes the statistics used to print per-second averages. */
void
os_aio_refresh_stats()
{
	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_n_file_reads_old = os_n_file_reads;

	os_n_file_writes_old = os_n_file_writes;

	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_last_printout = time(NULL);
}


/**
Set the file create umask
@param[in]	umask		The umask to use for file creation. */
void
os_file_set_umask(ulint umask)
{
	os_innodb_umask = umask;
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
static bool is_file_on_ssd(char *file_path)
{
  /* Cache of volume_path => volume_info, protected by rwlock.*/
  static std::unordered_map<std::string, bool> cache;
  static SRWLOCK lock= SRWLOCK_INIT;

  /* Preset result, in case something fails, e.g we're on network drive.*/
  char volume_path[MAX_PATH];
  if (!GetVolumePathName(file_path, volume_path, array_elements(volume_path)))
    return false;

  /* Try cached volume info first.*/
  std::string volume_path_str(volume_path);
  bool found;
  bool result;
  AcquireSRWLockShared(&lock);
  auto e= cache.find(volume_path_str);
  if ((found= e != cache.end()))
    result= e->second;
  ReleaseSRWLockShared(&lock);

  if (found)
    return result;

  result= is_volume_on_ssd(volume_path);

  /* Update cache */
  AcquireSRWLockExclusive(&lock);
  cache[volume_path_str]= result;
  ReleaseSRWLockExclusive(&lock);
  return result;
}

#endif

void fil_node_t::find_metadata(os_file_t file
#ifndef _WIN32
                               , bool create, struct stat *statbuf
#endif
                               )
{
  if (!is_open())
  {
    handle= file;
    ut_ad(is_open());
  }

  if (!space->is_compressed())
    punch_hole= 0;
  else if (my_test_if_thinly_provisioned(file))
    punch_hole= 2;
  else
    punch_hole= IF_WIN(, !create ||) os_is_sparse_file_supported(file);

#ifdef _WIN32
  on_ssd= is_file_on_ssd(name);
  FILE_STORAGE_INFO info;
  if (GetFileInformationByHandleEx(file, FileStorageInfo, &info, sizeof info))
    block_size= info.PhysicalBytesPerSectorForAtomicity;
  else
    block_size= 512;
#else
  struct stat sbuf;
  if (!statbuf && !fstat(file, &sbuf))
  {
    MSAN_STAT_WORKAROUND(&sbuf);
    statbuf= &sbuf;
  }
  if (statbuf)
    block_size= statbuf->st_blksize;
# ifdef UNIV_LINUX
  on_ssd= statbuf && fil_system.is_ssd(statbuf->st_dev);
# endif
#endif

  if (space->purpose != FIL_TYPE_TABLESPACE)
  {
    /* For temporary tablespace or during IMPORT TABLESPACE, we
    disable neighbour flushing and do not care about atomicity. */
    on_ssd= true;
    atomic_write= true;
  }
  else
    /* On Windows, all single sector writes are atomic, as per
    WriteFile() documentation on MSDN. */
    atomic_write= srv_use_atomic_writes &&
      IF_WIN(srv_page_size == block_size,
	     my_test_if_atomic_write(file, space->physical_size()));
}

/** Read the first page of a data file.
@return	whether the page was found valid */
bool fil_node_t::read_page0()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  const unsigned psize= space->physical_size();
#ifndef _WIN32
  struct stat statbuf;
  if (fstat(handle, &statbuf))
    return false;
  MSAN_STAT_WORKAROUND(&statbuf);
  os_offset_t size_bytes= statbuf.st_size;
#else
  os_offset_t size_bytes= os_file_get_size(handle);
  ut_a(size_bytes != (os_offset_t) -1);
#endif
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
    page_t *page= static_cast<byte*>(aligned_malloc(psize, psize));
    if (os_file_read(IORequestRead, handle, page, 0, psize)
        != DB_SUCCESS)
    {
      ib::error() << "Unable to read first page of file " << name;
corrupted:
      aligned_free(page);
      return false;
    }

    const ulint space_id= memcmp_aligned<2>
      (FIL_PAGE_SPACE_ID + page,
       FSP_HEADER_OFFSET + FSP_SPACE_ID + page, 4)
      ? ULINT_UNDEFINED
      : mach_read_from_4(FIL_PAGE_SPACE_ID + page);
    uint32_t flags= fsp_header_get_flags(page);
    const uint32_t size= fsp_header_get_field(page, FSP_SIZE);
    const uint32_t free_limit= fsp_header_get_field(page, FSP_FREE_LIMIT);
    const uint32_t free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE + page);
    if (!fil_space_t::is_valid_flags(flags, space->id))
    {
      uint32_t cflags= fsp_flags_convert_from_101(flags);
      if (cflags == UINT32_MAX)
      {
invalid:
        ib::error() << "Expected tablespace flags "
          << ib::hex(space->flags)
          << " but found " << ib::hex(flags)
          << " in the file " << name;
        goto corrupted;
      }

      uint32_t cf= cflags & ~FSP_FLAGS_MEM_MASK;
      uint32_t sf= space->flags & ~FSP_FLAGS_MEM_MASK;

      if (!fil_space_t::is_flags_equal(cf, sf) &&
          !fil_space_t::is_flags_equal(sf, cf))
        goto invalid;
      flags= cflags;
    }

    ut_ad(!(flags & FSP_FLAGS_MEM_MASK));

    /* Try to read crypt_data from page 0 if it is not yet read. */
    if (!space->crypt_data)
      space->crypt_data= fil_space_read_crypt_data(
        fil_space_t::zip_size(flags), page);
    aligned_free(page);

    if (UNIV_UNLIKELY(space_id != space->id))
    {
      ib::error() << "Expected tablespace id " << space->id
        << " but found " << space_id
        << " in the file " << name;
      return false;
    }

    space->flags= (space->flags & FSP_FLAGS_MEM_MASK) | flags;
    ut_ad(space->free_limit == 0 || space->free_limit == free_limit);
    ut_ad(space->free_len == 0 || space->free_len == free_len);
    space->size_in_header= size;
    space->free_limit= free_limit;
    space->free_len= free_len;
  }

  IF_WIN(find_metadata(), find_metadata(handle, false, &statbuf));
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
