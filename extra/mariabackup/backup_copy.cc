/******************************************************
hot backup tool for InnoDB
(c) 2009-2015 Percona LLC and/or its affiliates
(c) 2017 MariaDB
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************

This file incorporates work covered by the following copyright and
permission notice:

Copyright (c) 2000, 2011, MySQL AB & Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
Street, Fifth Floor, Boston, MA 02110-1335 USA

*******************************************************/

#include <my_global.h>
#include <os0file.h>
#include <my_dir.h>
#include <ut0mem.h>
#include <srv0start.h>
#include <fil0fil.h>
#include <trx0sys.h>
#include <set>
#include <string>
#include <mysqld.h>
#include <sstream>
#include "fil_cur.h"
#include "xtrabackup.h"
#include "common.h"
#include "backup_copy.h"
#include "backup_debug.h"
#include "backup_mysql.h"
#include <btr0btr.h>

#ifdef _WIN32
#include <aclapi.h>
#endif


#define ROCKSDB_BACKUP_DIR "#rocksdb"

/* list of files to sync for --rsync mode */
static std::set<std::string> rsync_list;
/* locations of tablespaces read from .isl files */
static std::map<std::string, std::string> tablespace_locations;

/* Whether LOCK BINLOG FOR BACKUP has been issued during backup */
bool binlog_locked;

static void rocksdb_create_checkpoint();
static bool has_rocksdb_plugin();
static void copy_or_move_dir(const char *from, const char *to, bool copy, bool allow_hardlinks);
static void rocksdb_backup_checkpoint();
static void rocksdb_copy_back();

static bool is_abs_path(const char *path)
{
#ifdef _WIN32
	return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
#else
	return path[0] == '/';
#endif
}

/************************************************************************
Struct represents file or directory. */
struct datadir_node_t {
	ulint		dbpath_len;
	char		*filepath;
	ulint		filepath_len;
	char		*filepath_rel;
	ulint		filepath_rel_len;
	bool		is_empty_dir;
	bool		is_file;
};

/************************************************************************
Holds the state needed to enumerate files in MySQL data directory. */
struct datadir_iter_t {
	char		*datadir_path;
	char		*dbpath;
	ulint		dbpath_len;
	char		*filepath;
	ulint		filepath_len;
	char		*filepath_rel;
	ulint		filepath_rel_len;
	pthread_mutex_t	mutex;
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	dberr_t		err;
	bool		is_empty_dir;
	bool		is_file;
	bool		skip_first_level;
};


/************************************************************************
Represents the context of the thread processing MySQL data directory. */
struct datadir_thread_ctxt_t {
	datadir_iter_t		*it;
	uint			n_thread;
	uint			*count;
	pthread_mutex_t*	count_mutex;
	bool			ret;
};

static bool backup_files_from_datadir(const char *dir_path);

/************************************************************************
Retirn true if character if file separator */
bool
is_path_separator(char c)
{
	return(c == FN_LIBCHAR || c == FN_LIBCHAR2);
}


/************************************************************************
Fill the node struct. Memory for node need to be allocated and freed by
the caller. It is caller responsibility to initialize node with
datadir_node_init and cleanup the memory with datadir_node_free.
Node can not be shared between threads. */
static
void
datadir_node_fill(datadir_node_t *node, datadir_iter_t *it)
{
	if (node->filepath_len < it->filepath_len) {
		free(node->filepath);
		node->filepath = (char*)(malloc(it->filepath_len));
		node->filepath_len = it->filepath_len;
	}
	if (node->filepath_rel_len < it->filepath_rel_len) {
		free(node->filepath_rel);
		node->filepath_rel = (char*)(malloc(it->filepath_rel_len));
		node->filepath_rel_len = it->filepath_rel_len;
	}

	strcpy(node->filepath, it->filepath);
	strcpy(node->filepath_rel, it->filepath_rel);
	node->is_empty_dir = it->is_empty_dir;
	node->is_file = it->is_file;
}

static
void
datadir_node_free(datadir_node_t *node)
{
	free(node->filepath);
	free(node->filepath_rel);
	memset(node, 0, sizeof(datadir_node_t));
}

static
void
datadir_node_init(datadir_node_t *node)
{
	memset(node, 0, sizeof(datadir_node_t));
}


/************************************************************************
Create the MySQL data directory iterator. Memory needs to be released
with datadir_iter_free. Position should be advanced with
datadir_iter_next_file. Iterator can be shared between multiple
threads. It is guaranteed that each thread receives unique file from
data directory into its local node struct. */
static
datadir_iter_t *
datadir_iter_new(const char *path, bool skip_first_level = true)
{
	datadir_iter_t *it;

	it = static_cast<datadir_iter_t *>(malloc(sizeof(datadir_iter_t)));
	if (!it)
		goto error;
	memset(it, 0, sizeof(datadir_iter_t));

	pthread_mutex_init(&it->mutex, NULL);
	it->datadir_path = strdup(path);

	it->dir = os_file_opendir(it->datadir_path);

	if (it->dir == IF_WIN(INVALID_HANDLE_VALUE, nullptr)) {

		goto error;
	}

	it->err = DB_SUCCESS;

	it->dbpath_len = FN_REFLEN;
	it->dbpath = static_cast<char*>(malloc(it->dbpath_len));

	it->filepath_len = FN_REFLEN;
	it->filepath = static_cast<char*>(malloc(it->filepath_len));

	it->filepath_rel_len = FN_REFLEN;
	it->filepath_rel = static_cast<char*>(malloc(it->filepath_rel_len));

	it->skip_first_level = skip_first_level;

	return(it);

error:
	free(it);

	return(NULL);
}

static
bool
datadir_iter_next_database(datadir_iter_t *it)
{
	if (it->dbdir != NULL) {
		if (os_file_closedir_failed(it->dbdir)) {
			msg("Warning: could not"
			      " close database directory %s", it->dbpath);
			it->err = DB_ERROR;

		}
		it->dbdir = NULL;
	}

	while (os_file_readdir_next_file(it->datadir_path,
					  it->dir, &it->dbinfo) == 0) {
		ulint	len;

		if ((it->dbinfo.type == OS_FILE_TYPE_FILE
		     && it->skip_first_level)
		    || it->dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

			continue;
		}

		/* We found a symlink or a directory; try opening it to see
		if a symlink is a directory */

		len = strlen(it->datadir_path)
			+ strlen (it->dbinfo.name) + 2;
		if (len > it->dbpath_len) {
			it->dbpath_len = len;
			free(it->dbpath);

			it->dbpath = static_cast<char*>(
				malloc(it->dbpath_len));
		}
		snprintf(it->dbpath, it->dbpath_len, "%s/%s",
			 it->datadir_path, it->dbinfo.name);

		if (it->dbinfo.type == OS_FILE_TYPE_FILE) {
			it->is_file = true;
			return(true);
		}

		if (check_if_skip_database_by_path(it->dbpath)) {
			msg("Skipping db: %s", it->dbpath);
			continue;
		}

		/* We want wrong directory permissions to be a fatal error for
		XtraBackup. */
		it->dbdir = os_file_opendir(it->dbpath);

		if (it->dir != IF_WIN(INVALID_HANDLE_VALUE, nullptr)) {
			it->is_file = false;
			return(true);
		}

	}

	return(false);
}

/************************************************************************
Concatenate n parts into single path */
static
void
make_path_n(int n, char **path, ulint *path_len, ...)
{
	ulint len_needed = n + 1;
	char *p;
	int i;
	va_list vl;

	ut_ad(n > 0);

	va_start(vl, path_len);
	for (i = 0; i < n; i++) {
		p = va_arg(vl, char*);
		len_needed += strlen(p);
	}
	va_end(vl);

	if (len_needed < *path_len) {
		free(*path);
		*path = static_cast<char*>(malloc(len_needed));
	}

	va_start(vl, path_len);
	p = va_arg(vl, char*);
	strcpy(*path, p);
	for (i = 1; i < n; i++) {
		size_t plen;
		p = va_arg(vl, char*);
		plen = strlen(*path);
		if (!is_path_separator((*path)[plen - 1])) {
			(*path)[plen] = FN_LIBCHAR;
			(*path)[plen + 1] = 0;
		}
		strcat(*path + plen, p);
	}
	va_end(vl);
}

static
bool
datadir_iter_next_file(datadir_iter_t *it)
{
	if (it->is_file && it->dbpath) {
		make_path_n(2, &it->filepath, &it->filepath_len,
				it->datadir_path, it->dbinfo.name);

		make_path_n(1, &it->filepath_rel, &it->filepath_rel_len,
				it->dbinfo.name);

		it->is_empty_dir = false;
		it->is_file = false;

		return(true);
	}

	if (!it->dbpath || !it->dbdir) {

		return(false);
	}

	while (os_file_readdir_next_file(it->dbpath, it->dbdir,
					  &it->fileinfo) == 0) {

		if (it->fileinfo.type == OS_FILE_TYPE_DIR) {

			continue;
		}

		/* We found a symlink or a file */
		make_path_n(3, &it->filepath, &it->filepath_len,
				it->datadir_path, it->dbinfo.name,
				it->fileinfo.name);

		make_path_n(2, &it->filepath_rel, &it->filepath_rel_len,
				it->dbinfo.name, it->fileinfo.name);

		it->is_empty_dir = false;

		return(true);
	}

	return(false);
}

static
bool
datadir_iter_next(datadir_iter_t *it, datadir_node_t *node)
{
	bool	ret = true;

	pthread_mutex_lock(&it->mutex);

	if (datadir_iter_next_file(it)) {

		datadir_node_fill(node, it);

		goto done;
	}

	while (datadir_iter_next_database(it)) {

		if (datadir_iter_next_file(it)) {

			datadir_node_fill(node, it);

			goto done;
		}

		make_path_n(2, &it->filepath, &it->filepath_len,
				it->datadir_path, it->dbinfo.name);

		make_path_n(1, &it->filepath_rel, &it->filepath_rel_len,
				it->dbinfo.name);

		it->is_empty_dir = true;

		datadir_node_fill(node, it);

		goto done;
	}

	/* nothing found */
	ret = false;

done:
	pthread_mutex_unlock(&it->mutex);

	return(ret);
}

/************************************************************************
Interface to read MySQL data file sequentially. One should open file
with datafile_open to get cursor and close the cursor with
datafile_close. Cursor can not be shared between multiple
threads. */
static
void
datadir_iter_free(datadir_iter_t *it)
{
	pthread_mutex_destroy(&it->mutex);

	if (it->dbdir) {

		os_file_closedir(it->dbdir);
	}

	if (it->dir) {

		os_file_closedir(it->dir);
	}

	free(it->dbpath);
	free(it->filepath);
	free(it->filepath_rel);
	free(it->datadir_path);
	free(it);
}


/************************************************************************
Holds the state needed to copy single data file. */
struct datafile_cur_t {
	pfs_os_file_t	file;
	char		rel_path[FN_REFLEN];
	char		abs_path[FN_REFLEN];
	MY_STAT		statinfo;
	uint		thread_n;
	byte*		buf;
	size_t		buf_size;
	size_t		buf_read;
	size_t		buf_offset;

	explicit datafile_cur_t(const char* filename = NULL) :
		file(), thread_n(0), buf(NULL), buf_size(0),
		buf_read(0), buf_offset(0)
	{
		memset(rel_path, 0, sizeof rel_path);
		if (filename) {
			strncpy(abs_path, filename, sizeof abs_path - 1);
			abs_path[(sizeof abs_path) - 1] = 0;
		} else {
			abs_path[0] = '\0';
		}
		rel_path[0] = '\0';
		memset(&statinfo, 0, sizeof statinfo);
	}
};

static
void
datafile_close(datafile_cur_t *cursor)
{
	if (cursor->file != OS_FILE_CLOSED) {
		os_file_close(cursor->file);
	}
	free(cursor->buf);
}

static
bool
datafile_open(const char *file, datafile_cur_t *cursor, uint thread_n)
{
	bool		success;

	new (cursor) datafile_cur_t(file);

	/* Get the relative path for the destination tablespace name, i.e. the
	one that can be appended to the backup root directory. Non-system
	tablespaces may have absolute paths for remote tablespaces in MySQL
	5.6+. We want to make "local" copies for the backup. */
	strncpy(cursor->rel_path,
		xb_get_relative_path(cursor->abs_path, FALSE),
		(sizeof cursor->rel_path) - 1);
	cursor->rel_path[(sizeof cursor->rel_path) - 1] = '\0';

	cursor->file = os_file_create_simple_no_error_handling(
		0, cursor->abs_path,
		OS_FILE_OPEN, OS_FILE_READ_ALLOW_DELETE, true, &success);
	if (!success) {
		/* The following call prints an error message */
		os_file_get_last_error(TRUE);

		msg(thread_n,"error: cannot open "
			"file %s", cursor->abs_path);

		return(false);
	}

	if (!my_stat(cursor->abs_path, &cursor->statinfo, 0)) {
		msg(thread_n, "error: cannot stat %s", cursor->abs_path);
		datafile_close(cursor);
		return(false);
	}

	posix_fadvise(cursor->file, 0, 0, POSIX_FADV_SEQUENTIAL);

	cursor->buf_size = 10 * 1024 * 1024;
	cursor->buf = static_cast<byte *>(malloc((ulint)cursor->buf_size));

	return(true);
}


static
xb_fil_cur_result_t
datafile_read(datafile_cur_t *cursor)
{
	ulint		to_read;

	xtrabackup_io_throttling();

	to_read = (ulint)MY_MIN(cursor->statinfo.st_size - cursor->buf_offset, 
		      cursor->buf_size);

	if (to_read == 0) {
		return(XB_FIL_CUR_EOF);
	}

	if (os_file_read(IORequestRead,
			  cursor->file, cursor->buf, cursor->buf_offset,
			  to_read) != DB_SUCCESS) {
		return(XB_FIL_CUR_ERROR);
	}

	posix_fadvise(cursor->file, cursor->buf_offset, to_read,
		      POSIX_FADV_DONTNEED);

	cursor->buf_read = to_read;
	cursor->buf_offset += to_read;

	return(XB_FIL_CUR_SUCCESS);
}



/************************************************************************
Check to see if a file exists.
Takes name of the file to check.
@return true if file exists. */
bool
file_exists(const char *filename)
{
	MY_STAT stat_arg;

	if (!my_stat(filename, &stat_arg, MYF(0))) {

		return(false);
	}

	return(true);
}

/************************************************************************
Trim leading slashes from absolute path so it becomes relative */
static
const char *
trim_dotslash(const char *path)
{
	while (*path) {
		if (is_path_separator(*path)) {
			++path;
			continue;
		}
		if (*path == '.' && is_path_separator(path[1])) {
			path += 2;
			continue;
		}
		break;
	}

	return(path);
}



/************************************************************************
Check if string ends with given suffix.
@return true if string ends with given suffix. */
bool
ends_with(const char *str, const char *suffix)
{
	size_t suffix_len = strlen(suffix);
	size_t str_len = strlen(str);
	return(str_len >= suffix_len
	       && strcmp(str + str_len - suffix_len, suffix) == 0);
}

static bool starts_with(const char *str, const char *prefix)
{
	return strncmp(str, prefix, strlen(prefix)) == 0;
}

/************************************************************************
Create directories recursively.
@return 0 if directories created successfully. */
static
int
mkdirp(const char *pathname, int Flags, myf MyFlags)
{
	char *parent, *p;

	/* make a parent directory path */
	if (!(parent= strdup(pathname)))
          return(-1);

	for (p = parent + strlen(parent);
	    !is_path_separator(*p) && p != parent; p--) ;

	*p = 0;

	/* try to make parent directory */
	if (p != parent && mkdirp(parent, Flags, MyFlags) != 0) {
		free(parent);
		return(-1);
	}

	/* make this one if parent has been made */
	if (my_mkdir(pathname, Flags, MyFlags) == 0) {
		free(parent);
		return(0);
	}

	/* if it already exists that is fine */
	if (errno == EEXIST) {
		free(parent);
		return(0);
	}

	free(parent);
	return(-1);
}

/************************************************************************
Return true if first and second arguments are the same path. */
bool
equal_paths(const char *first, const char *second)
{
#ifdef HAVE_REALPATH
	char *real_first, *real_second;
	int result;

	real_first = realpath(first, 0);
	if (real_first == NULL) {
		return false;
	}

	real_second = realpath(second, 0);
	if (real_second == NULL) {
		free(real_first);
		return false;
	}

	result = strcmp(real_first, real_second);
	free(real_first);
	free(real_second);
	return result == 0;
#else
	return strcmp(first, second) == 0;
#endif
}

/************************************************************************
Check if directory exists. Optionally create directory if doesn't
exist.
@return true if directory exists and if it was created successfully. */
bool
directory_exists(const char *dir, bool create)
{
	os_file_dir_t os_dir;
	MY_STAT stat_arg;
	char errbuf[MYSYS_STRERROR_SIZE];

	if (my_stat(dir, &stat_arg, MYF(0)) == NULL) {

		if (!create) {
			return(false);
		}

		if (mkdirp(dir, 0777, MYF(0)) < 0) {
			my_strerror(errbuf, sizeof(errbuf), my_errno);
			msg("Can not create directory %s: %s", dir, errbuf);
			return(false);
		}
	}

	/* could be symlink */
	os_dir = os_file_opendir(dir);

	if (os_dir == IF_WIN(INVALID_HANDLE_VALUE, nullptr)) {
		my_strerror(errbuf, sizeof(errbuf), my_errno);
		msg("Can not open directory %s: %s", dir,
			errbuf);

		return(false);
	}

	os_file_closedir(os_dir);

	return(true);
}

/************************************************************************
Check that directory exists and it is empty. */
static
bool
directory_exists_and_empty(const char *dir, const char *comment)
{
	os_file_dir_t os_dir;
	dberr_t err;
	os_file_stat_t info;
	bool empty;

	if (!directory_exists(dir, true)) {
		return(false);
	}

	os_dir = os_file_opendir(dir);

	if (os_dir == IF_WIN(INVALID_HANDLE_VALUE, nullptr)) {
		msg("%s can not open directory %s", comment, dir);
		return(false);
	}

	empty = (fil_file_readdir_next_file(&err, dir, os_dir, &info) != 0);

	os_file_closedir(os_dir);

	if (!empty) {
		msg("%s directory %s is not empty!", comment, dir);
	}

	return(empty);
}


/************************************************************************
Check if file name ends with given set of suffixes.
@return true if it does. */
static
bool
filename_matches(const char *filename, const char **ext_list)
{
	const char **ext;

	for (ext = ext_list; *ext; ext++) {
		if (ends_with(filename, *ext)) {
			return(true);
		}
	}

	return(false);
}


/************************************************************************
Copy data file for backup. Also check if it is allowed to copy by
comparing its name to the list of known data file types and checking
if passes the rules for partial backup.
@return true if file backed up or skipped successfully. */
static
bool
datafile_copy_backup(const char *filepath, uint thread_n)
{
	const char *ext_list[] = {"frm", "isl", "MYD", "MYI", "MAD", "MAI",
		"MRG", "TRG", "TRN", "ARM", "ARZ", "CSM", "CSV", "opt", "par",
		NULL};

	/* Get the name and the path for the tablespace. node->name always
	contains the path (which may be absolute for remote tablespaces in
	5.6+). space->name contains the tablespace name in the form
	"./database/table.ibd" (in 5.5-) or "database/table" (in 5.6+). For a
	multi-node shared tablespace, space->name contains the name of the first
	node, but that's irrelevant, since we only need node_name to match them
	against filters, and the shared tablespace is always copied regardless
	of the filters value. */

	if (check_if_skip_table(filepath)) {
		msg(thread_n,"Skipping %s.", filepath);
		return(true);
	}

	if (filename_matches(filepath, ext_list)) {
		return copy_file(ds_data, filepath, filepath, thread_n);
	}

	return(true);
}


/************************************************************************
Same as datafile_copy_backup, but put file name into the list for
rsync command. */
static
bool
datafile_rsync_backup(const char *filepath, bool save_to_list, FILE *f)
{
	const char *ext_list[] = {"frm", "isl", "MYD", "MYI", "MAD", "MAI",
		"MRG", "TRG", "TRN", "ARM", "ARZ", "CSM", "CSV", "opt", "par",
		NULL};

	/* Get the name and the path for the tablespace. node->name always
	contains the path (which may be absolute for remote tablespaces in
	5.6+). space->name contains the tablespace name in the form
	"./database/table.ibd" (in 5.5-) or "database/table" (in 5.6+). For a
	multi-node shared tablespace, space->name contains the name of the first
	node, but that's irrelevant, since we only need node_name to match them
	against filters, and the shared tablespace is always copied regardless
	of the filters value. */

	if (check_if_skip_table(filepath)) {
		return(true);
	}

	if (filename_matches(filepath, ext_list)) {
		fprintf(f, "%s\n", filepath);
		if (save_to_list) {
			rsync_list.insert(filepath);
		}
	}

	return(true);
}

bool backup_file_print_buf(const char *filename, const char *buf, int buf_len)
{
	ds_file_t	*dstfile	= NULL;
	MY_STAT		 stat;			/* unused for now */
	const char	*action;

	memset(&stat, 0, sizeof(stat));

	stat.st_size = buf_len;
	stat.st_mtime = my_time(0);

	dstfile = ds_open(ds_data, filename, &stat);
	if (dstfile == NULL) {
		msg("error: Can't open the destination stream for %s",
			filename);
		goto error;
	}

	action = xb_get_copy_action("Writing");
	msg("%s %s", action, filename);

	if (buf_len == -1) {
		goto error;
	}

	if (ds_write(dstfile, buf, buf_len)) {
		goto error;
	}

	/* close */
	msg("        ...done");

	if (ds_close(dstfile)) {
		goto error_close;
	}

	return(true);

error:
	if (dstfile != NULL) {
		ds_close(dstfile);
	}

error_close:
	msg("Error: backup file failed.");
	return(false); /*ERROR*/

	return true;
};

static
bool
backup_file_vprintf(const char *filename, const char *fmt, va_list ap)
{
	char		*buf		= 0;
	int		 buf_len;
	buf_len = vasprintf(&buf, fmt, ap);
	bool result = backup_file_print_buf(filename, buf, buf_len);
	free(buf);
	return result;
}

bool
backup_file_printf(const char *filename, const char *fmt, ...)
{
	bool result;
	va_list ap;

	va_start(ap, fmt);

	result = backup_file_vprintf(filename, fmt, ap);

	va_end(ap);

	return(result);
}

static
bool
run_data_threads(datadir_iter_t *it, void (*func)(datadir_thread_ctxt_t *ctxt), uint n)
{
	datadir_thread_ctxt_t	*data_threads;
	uint			i, count;
	pthread_mutex_t		count_mutex;
	bool			ret;

	data_threads = (datadir_thread_ctxt_t*)
		malloc(sizeof(datadir_thread_ctxt_t) * n);

	pthread_mutex_init(&count_mutex, NULL);
	count = n;

	for (i = 0; i < n; i++) {
		data_threads[i].it = it;
		data_threads[i].n_thread = i + 1;
		data_threads[i].count = &count;
		data_threads[i].count_mutex = &count_mutex;
		std::thread(func, data_threads + i).detach();
	}

	/* Wait for threads to exit */
	while (1) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		pthread_mutex_lock(&count_mutex);
		if (count == 0) {
			pthread_mutex_unlock(&count_mutex);
			break;
		}
		pthread_mutex_unlock(&count_mutex);
	}

	pthread_mutex_destroy(&count_mutex);

	ret = true;
	for (i = 0; i < n; i++) {
		ret = data_threads[i].ret && ret;
		if (!data_threads[i].ret) {
			msg("Error: thread %u failed.", i);
		}
	}

	free(data_threads);

	return(ret);
}


/************************************************************************
Copy file for backup/restore.
@return true in case of success. */
bool
copy_file(ds_ctxt_t *datasink,
	  const char *src_file_path,
	  const char *dst_file_path,
	  uint thread_n)
{
	char			 dst_name[FN_REFLEN];
	ds_file_t		*dstfile = NULL;
	datafile_cur_t		 cursor;
	xb_fil_cur_result_t	 res;
	DBUG_ASSERT(datasink->datasink->remove);
	const char	*dst_path =
		(xtrabackup_copy_back || xtrabackup_move_back)?
		dst_file_path : trim_dotslash(dst_file_path);

	if (!datafile_open(src_file_path, &cursor, thread_n)) {
		goto error_close;
	}

	strncpy(dst_name, cursor.rel_path, sizeof(dst_name));

	dstfile = ds_open(datasink, dst_path, &cursor.statinfo);
	if (dstfile == NULL) {
		msg(thread_n,"error: "
			"cannot open the destination stream for %s", dst_name);
		goto error;
	}

	msg(thread_n, "%s %s to %s", xb_get_copy_action(), src_file_path, dstfile->path);

	/* The main copy loop */
	while ((res = datafile_read(&cursor)) == XB_FIL_CUR_SUCCESS) {

		if (ds_write(dstfile, cursor.buf, cursor.buf_read)) {
			goto error;
		}
		DBUG_EXECUTE_IF("copy_file_error", errno=ENOSPC;goto error;);
	}

	if (res == XB_FIL_CUR_ERROR) {
		goto error;
	}

	/* close */
	msg(thread_n,"        ...done");
	datafile_close(&cursor);
	if (ds_close(dstfile)) {
		goto error_close;
	}
	return(true);

error:
	datafile_close(&cursor);
	if (dstfile != NULL) {
		datasink->datasink->remove(dstfile->path);
		ds_close(dstfile);
	}

error_close:
	msg(thread_n,"Error: copy_file() failed.");
	return(false); /*ERROR*/
}


/************************************************************************
Try to move file by renaming it. If source and destination are on
different devices fall back to copy and unlink.
@return true in case of success. */
static
bool
move_file(ds_ctxt_t *datasink,
	  const char *src_file_path,
	  const char *dst_file_path,
	  const char *dst_dir, uint thread_n)
{
	char errbuf[MYSYS_STRERROR_SIZE];
	char dst_file_path_abs[FN_REFLEN];
	char dst_dir_abs[FN_REFLEN];
	size_t dirname_length;

	snprintf(dst_file_path_abs, sizeof(dst_file_path_abs),
		 "%s/%s", dst_dir, dst_file_path);

	dirname_part(dst_dir_abs, dst_file_path_abs, &dirname_length);

	if (!directory_exists(dst_dir_abs, true)) {
		return(false);
	}

	if (file_exists(dst_file_path_abs)) {
		msg("Error: Move file %s to %s failed: Destination "
			"file exists", src_file_path, dst_file_path_abs);
		return(false);
	}

	msg(thread_n,"Moving %s to %s", src_file_path, dst_file_path_abs);

	if (my_rename(src_file_path, dst_file_path_abs, MYF(0)) != 0) {
		if (my_errno == EXDEV) {
			/* Fallback to copy/unlink */
			if(!copy_file(datasink, src_file_path,
					dst_file_path, thread_n))
					return false;
			msg(thread_n,"Removing %s", src_file_path);
			if (unlink(src_file_path) != 0) {
				my_strerror(errbuf, sizeof(errbuf), errno);
				msg("Warning: unlink %s failed: %s",
					src_file_path,
					errbuf);
			}
			return true;
		}
		my_strerror(errbuf, sizeof(errbuf), my_errno);
		msg("Can not move file %s to %s: %s",
			src_file_path, dst_file_path_abs,
			errbuf);
		return(false);
	}
	msg(thread_n,"        ...done");

	return(true);
}


/************************************************************************
Read link from .isl file if any and store it in the global map associated
with given tablespace. */
static
void
read_link_file(const char *ibd_filepath, const char *link_filepath)
{
	char *filepath= NULL;

	FILE *file = fopen(link_filepath, "r+b");
	if (file) {
		filepath = static_cast<char*>(malloc(OS_FILE_MAX_PATH));

		os_file_read_string(file, filepath, OS_FILE_MAX_PATH);
		fclose(file);

		if (size_t len = strlen(filepath)) {
			/* Trim whitespace from end of filepath */
			ulint lastch = len - 1;
			while (lastch > 4 && filepath[lastch] <= 0x20) {
				filepath[lastch--] = 0x00;
			}
		}

		tablespace_locations[ibd_filepath] = filepath;
	}
	free(filepath);
}


/************************************************************************
Return the location of given .ibd if it was previously read
from .isl file.
@return NULL or destination .ibd file path. */
static
const char *
tablespace_filepath(const char *ibd_filepath)
{
	std::map<std::string, std::string>::iterator it;

	it = tablespace_locations.find(ibd_filepath);

	if (it != tablespace_locations.end()) {
		return it->second.c_str();
	}

	return NULL;
}


/************************************************************************
Copy or move file depending on current mode.
@return true in case of success. */
static
bool
copy_or_move_file(const char *src_file_path,
		  const char *dst_file_path,
		  const char *dst_dir,
		  uint thread_n,
		 bool copy = xtrabackup_copy_back)
{
	ds_ctxt_t *datasink = ds_data;		/* copy to datadir by default */
	char filedir[FN_REFLEN];
	size_t filedir_len;
	bool ret;

	/* read the link from .isl file */
	if (ends_with(src_file_path, ".isl")) {
		char *ibd_filepath;

		ibd_filepath = strdup(src_file_path);
		strcpy(ibd_filepath + strlen(ibd_filepath) - 3, "ibd");

		read_link_file(ibd_filepath, src_file_path);

		free(ibd_filepath);
	}

	/* check if there is .isl file */
	if (ends_with(src_file_path, ".ibd")) {
		char *link_filepath;
		const char *filepath;

		link_filepath = strdup(src_file_path);
		strcpy(link_filepath + strlen(link_filepath) - 3, "isl");

		read_link_file(src_file_path, link_filepath);

		filepath = tablespace_filepath(src_file_path);

		if (filepath != NULL) {
			dirname_part(filedir, filepath, &filedir_len);

			dst_file_path = filepath + filedir_len;
			dst_dir = filedir;

			if (!directory_exists(dst_dir, true)) {
				ret = false;
				free(link_filepath);
				goto cleanup;
			}

			datasink = ds_create(dst_dir, DS_TYPE_LOCAL);
		}

		free(link_filepath);
	}

	ret = (copy ?
		copy_file(datasink, src_file_path, dst_file_path, thread_n) :
		move_file(datasink, src_file_path, dst_file_path,
			  dst_dir, thread_n));

cleanup:

	if (datasink != ds_data) {
		ds_destroy(datasink);
	}

	return(ret);
}




static
bool
backup_files(const char *from, bool prep_mode)
{
	char rsync_tmpfile_name[FN_REFLEN];
	FILE *rsync_tmpfile = NULL;
	datadir_iter_t *it;
	datadir_node_t node;
	bool ret = true;

	if (prep_mode && !opt_rsync) {
		return(true);
	}

	if (opt_rsync) {
		snprintf(rsync_tmpfile_name, sizeof(rsync_tmpfile_name),
			"%s/%s%d", opt_mysql_tmpdir,
			"xtrabackup_rsyncfiles_pass",
			prep_mode ? 1 : 2);
		rsync_tmpfile = fopen(rsync_tmpfile_name, "w");
		if (rsync_tmpfile == NULL) {
			msg("Error: can't create file %s",
				rsync_tmpfile_name);
			return(false);
		}
	}

	msg("Starting %s non-InnoDB tables and files",
	       prep_mode ? "prep copy of" : "to backup");

	datadir_node_init(&node);
	it = datadir_iter_new(from);

	while (datadir_iter_next(it, &node)) {

		if (!node.is_empty_dir) {
			if (opt_rsync) {
				ret = datafile_rsync_backup(node.filepath,
					!prep_mode, rsync_tmpfile);
			} else {
				ret = datafile_copy_backup(node.filepath, 1);
			}
			if (!ret) {
				msg("Failed to copy file %s", node.filepath);
				goto out;
			}
		} else if (!prep_mode) {
			/* backup fake file into empty directory */
			char path[FN_REFLEN];
			snprintf(path, sizeof(path),
				 "%s/db.opt", node.filepath);
			if (!(ret = backup_file_printf(
					trim_dotslash(path), "%s", ""))) {
				msg("Failed to create file %s", path);
				goto out;
			}
		}
	}

	if (opt_rsync) {
		std::stringstream cmd;
		int err;

		if (buffer_pool_filename && file_exists(buffer_pool_filename)) {
			fprintf(rsync_tmpfile, "%s\n", buffer_pool_filename);
			rsync_list.insert(buffer_pool_filename);
		}
		if (file_exists("ib_lru_dump")) {
			fprintf(rsync_tmpfile, "%s\n", "ib_lru_dump");
			rsync_list.insert("ib_lru_dump");
		}

		fclose(rsync_tmpfile);
		rsync_tmpfile = NULL;

		cmd << "rsync -t . --files-from=" << rsync_tmpfile_name
		    << " " << xtrabackup_target_dir;

		msg("Starting rsync as: %s", cmd.str().c_str());
		if ((err = system(cmd.str().c_str()) && !prep_mode) != 0) {
			msg("Error: rsync failed with error code %d", err);
			ret = false;
			goto out;
		}
		msg("rsync finished successfully.");

		if (!prep_mode && !opt_no_lock) {
			char path[FN_REFLEN];
			char dst_path[FN_REFLEN];
			char *newline;

			/* Remove files that have been removed between first and
			second passes. Cannot use "rsync --delete" because it
			does not work with --files-from. */
			snprintf(rsync_tmpfile_name, sizeof(rsync_tmpfile_name),
				"%s/%s", opt_mysql_tmpdir,
				"xtrabackup_rsyncfiles_pass1");

			rsync_tmpfile = fopen(rsync_tmpfile_name, "r");
			if (rsync_tmpfile == NULL) {
				msg("Error: can't open file %s",
					rsync_tmpfile_name);
				ret = false;
				goto out;
			}

			while (fgets(path, sizeof(path), rsync_tmpfile)) {

				newline = strchr(path, '\n');
				if (newline) {
					*newline = 0;
				}
				if (rsync_list.count(path) < 1) {
					snprintf(dst_path, sizeof(dst_path),
						"%s/%s", xtrabackup_target_dir,
						path);
					msg("Removing %s", dst_path);
					unlink(dst_path);
				}
			}

			fclose(rsync_tmpfile);
			rsync_tmpfile = NULL;
		}
	}

	msg("Finished %s non-InnoDB tables and files",
	       prep_mode ? "a prep copy of" : "backing up");

out:
	datadir_iter_free(it);
	datadir_node_free(&node);

	if (rsync_tmpfile != NULL) {
		fclose(rsync_tmpfile);
	}

	return(ret);
}

void backup_fix_ddl(CorruptedPages &);

lsn_t get_current_lsn(MYSQL *connection)
{
	static const char lsn_prefix[] = "\nLog sequence number ";
	lsn_t lsn = 0;
	if (MYSQL_RES *res = xb_mysql_query(connection,
					    "SHOW ENGINE INNODB STATUS",
					    true, false)) {
		if (MYSQL_ROW row = mysql_fetch_row(res)) {
			const char *p= strstr(row[2], lsn_prefix);
			DBUG_ASSERT(p);
			if (p) {
				p += sizeof lsn_prefix - 1;
				lsn = lsn_t(strtoll(p, NULL, 10));
			}
		}
		mysql_free_result(res);
	}
	return lsn;
}

lsn_t server_lsn_after_lock;
extern void backup_wait_for_lsn(lsn_t lsn);
/** Start --backup */
bool backup_start(CorruptedPages &corrupted_pages)
{
	if (!opt_no_lock) {
		if (opt_safe_slave_backup) {
			if (!wait_for_safe_slave(mysql_connection)) {
				return(false);
			}
		}

		if (!backup_files(fil_path_to_mysql_datadir, true)) {
			return(false);
		}

		history_lock_time = time(NULL);

		if (!lock_tables(mysql_connection)) {
			return(false);
		}
		server_lsn_after_lock = get_current_lsn(mysql_connection);
	}

	if (!backup_files(fil_path_to_mysql_datadir, false)) {
		return(false);
	}

	if (!backup_files_from_datadir(fil_path_to_mysql_datadir)) {
		return false;
	}

	if (has_rocksdb_plugin()) {
		rocksdb_create_checkpoint();
	}

	msg("Waiting for log copy thread to read lsn %llu", (ulonglong)server_lsn_after_lock);
	backup_wait_for_lsn(server_lsn_after_lock);
	DBUG_EXECUTE_FOR_KEY("sleep_after_waiting_for_lsn", {},
		{
			ulong milliseconds = strtoul(dbug_val, NULL, 10);
			msg("sleep_after_waiting_for_lsn");
			my_sleep(milliseconds*1000UL);
		});

	backup_fix_ddl(corrupted_pages);

	// There is no need to stop slave thread before coping non-Innodb data when
	// --no-lock option is used because --no-lock option requires that no DDL or
	// DML to non-transaction tables can occur.
	if (opt_no_lock) {
		if (opt_safe_slave_backup) {
			if (!wait_for_safe_slave(mysql_connection)) {
				return(false);
			}
		}
	}

	if (opt_slave_info) {
		lock_binlog_maybe(mysql_connection);

		if (!write_slave_info(mysql_connection)) {
			return(false);
		}
	}

	/* The only reason why Galera/binlog info is written before
	wait_for_ibbackup_log_copy_finish() is that after that call the xtrabackup
	binary will start streamig a temporary copy of REDO log to stdout and
	thus, any streaming from innobackupex would interfere. The only way to
	avoid that is to have a single process, i.e. merge innobackupex and
	xtrabackup. */
	if (opt_galera_info) {
		if (!write_galera_info(mysql_connection)) {
			return(false);
		}
	}

	bool with_binlogs = opt_binlog_info == BINLOG_INFO_ON;

	if (with_binlogs || opt_galera_info) {
		if (!write_current_binlog_file(mysql_connection, with_binlogs)) {
			return(false);
		}
	}

	if (have_flush_engine_logs && !opt_no_lock) {
		msg("Executing FLUSH NO_WRITE_TO_BINLOG ENGINE LOGS...");
		xb_mysql_query(mysql_connection,
			"FLUSH NO_WRITE_TO_BINLOG ENGINE LOGS", false);
	}

	return(true);
}

/** Release resources after backup_start() */
void backup_release()
{
	/* release all locks */
	if (!opt_no_lock) {
		unlock_all(mysql_connection);
		history_lock_time = 0;
	} else {
		history_lock_time = time(NULL) - history_lock_time;
	}

	if (opt_lock_ddl_per_table) {
		mdl_unlock_all();
	}

	if (opt_safe_slave_backup && sql_thread_started) {
		msg("Starting slave SQL thread");
		xb_mysql_query(mysql_connection,
				"START SLAVE SQL_THREAD", false);
	}
}

static const char *default_buffer_pool_file = "ib_buffer_pool";

static
const char * get_buffer_pool_filename(size_t *length)
{
	/* If mariabackup is run for Galera, then the file
	name is changed to the default so that the receiving
	node can find this file and rename it according to its
	settings, otherwise we keep the original file name: */
	size_t dir_length = 0;
	const char *dst_name = default_buffer_pool_file;
	if (!opt_galera_info) {
		dir_length = dirname_length(buffer_pool_filename);
		dst_name = buffer_pool_filename + dir_length;
	}
	if (length) {
		*length=dir_length;
	}
	return dst_name;
}

/** Finish after backup_start() and backup_release() */
bool backup_finish()
{
	/* Copy buffer pool dump or LRU dump */
	if (!opt_rsync) {
		if (buffer_pool_filename && file_exists(buffer_pool_filename)) {
			const char *dst_name = get_buffer_pool_filename(NULL);
			copy_file(ds_data, buffer_pool_filename, dst_name, 0);
		}
		if (file_exists("ib_lru_dump")) {
			copy_file(ds_data, "ib_lru_dump", "ib_lru_dump", 0);
		}
	}

	if (has_rocksdb_plugin()) {
		rocksdb_backup_checkpoint();
	}

	msg("Backup created in directory '%s'", xtrabackup_target_dir);
	if (mysql_binlog_position != NULL) {
		msg("MySQL binlog position: %s", mysql_binlog_position);
	}
	if (mysql_slave_position && opt_slave_info) {
		msg("MySQL slave binlog position: %s",
			mysql_slave_position);
	}

	if (!write_backup_config_file()) {
		return(false);
	}

	if (!write_xtrabackup_info(mysql_connection, XTRABACKUP_INFO,
				    opt_history != 0, true)) {
		return(false);
	}

	return(true);
}

bool
ibx_copy_incremental_over_full()
{
	const char *ext_list[] = {"frm", "isl", "MYD", "MYI", "MAD", "MAI",
		"MRG", "TRG", "TRN", "ARM", "ARZ", "CSM", "CSV", "opt", "par",
		NULL};
	const char *sup_files[] = {"xtrabackup_binlog_info",
				   "xtrabackup_galera_info",
				   "xtrabackup_slave_info",
				   "xtrabackup_info",
				   "ib_lru_dump",
				   NULL};
	datadir_iter_t *it = NULL;
	datadir_node_t node;
	bool ret = true;
	char path[FN_REFLEN];
	int i;

	datadir_node_init(&node);

	/* If we were applying an incremental change set, we need to make
	sure non-InnoDB files and xtrabackup_* metainfo files are copied
	to the full backup directory. */

	if (xtrabackup_incremental) {

		ds_data = ds_create(xtrabackup_target_dir, DS_TYPE_LOCAL);

		it = datadir_iter_new(xtrabackup_incremental_dir);

		while (datadir_iter_next(it, &node)) {

			/* copy only non-innodb files */

			if (node.is_empty_dir
			    || !filename_matches(node.filepath, ext_list)) {
				continue;
			}

			if (file_exists(node.filepath_rel)) {
				unlink(node.filepath_rel);
			}

			if (!(ret = copy_file(ds_data, node.filepath,
						node.filepath_rel, 1))) {
				msg("Failed to copy file %s",
					node.filepath);
				goto cleanup;
			}
		}

		if (!(ret = backup_files_from_datadir(xtrabackup_incremental_dir)))
			goto cleanup;

		/* copy buffer pool dump */
		if (innobase_buffer_pool_filename) {
			const char *src_name = get_buffer_pool_filename(NULL);

			snprintf(path, sizeof(path), "%s/%s",
				xtrabackup_incremental_dir,
				src_name);

			if (file_exists(path)) {
				copy_file(ds_data, path, src_name, 0);
			}
		}

		/* copy supplementary files */

		for (i = 0; sup_files[i]; i++) {
			snprintf(path, sizeof(path), "%s/%s",
				xtrabackup_incremental_dir,
				sup_files[i]);

			if (file_exists(path))
			{
				if (file_exists(sup_files[i])) {
					unlink(sup_files[i]);
				}
				copy_file(ds_data, path, sup_files[i], 0);
			}
		}

		if (directory_exists(ROCKSDB_BACKUP_DIR, false)) {
			if (my_rmtree(ROCKSDB_BACKUP_DIR, MYF(0))) {
				die("Can't remove " ROCKSDB_BACKUP_DIR);
			}
		}
		snprintf(path, sizeof(path), "%s/" ROCKSDB_BACKUP_DIR, xtrabackup_incremental_dir);
		if (directory_exists(path, false)) {
			if (my_mkdir(ROCKSDB_BACKUP_DIR, 0777, MYF(0))) {
				die("my_mkdir failed for " ROCKSDB_BACKUP_DIR);
			}
			copy_or_move_dir(path, ROCKSDB_BACKUP_DIR, true, true);
		}
	}


cleanup:
	if (it != NULL) {
		datadir_iter_free(it);
	}

	if (ds_data != NULL) {
		ds_destroy(ds_data);
	}

	datadir_node_free(&node);

	return(ret);
}

bool
ibx_cleanup_full_backup()
{
	const char *ext_list[] = {"delta", "meta", "ibd", NULL};
	datadir_iter_t *it = NULL;
	datadir_node_t node;
	bool ret = true;

	datadir_node_init(&node);

	/* If we are applying an incremental change set, we need to make
	sure non-InnoDB files are cleaned up from full backup dir before
	we copy files from incremental dir. */

	it = datadir_iter_new(xtrabackup_target_dir);

	while (datadir_iter_next(it, &node)) {

		if (node.is_empty_dir) {
#ifdef _WIN32
			DeleteFile(node.filepath);
#else
			rmdir(node.filepath);
#endif
		}

		if (xtrabackup_incremental && !node.is_empty_dir
		    && !filename_matches(node.filepath, ext_list)) {
			unlink(node.filepath);
		}
	}

	datadir_iter_free(it);

	datadir_node_free(&node);

	return(ret);
}

bool
apply_log_finish()
{
	if (!ibx_cleanup_full_backup()
		|| !ibx_copy_incremental_over_full()) {
		return(false);
	}

	return(true);
}


bool
copy_back()
{
	bool ret = false;
	datadir_iter_t *it = NULL;
	datadir_node_t node;
	char *dst_dir;

	memset(&node, 0, sizeof(node));

	if (!opt_force_non_empty_dirs) {
		if (!directory_exists_and_empty(mysql_data_home,
							"Original data")) {
			return(false);
		}
	} else {
		if (!directory_exists(mysql_data_home, true)) {
			return(false);
		}
	}

#ifdef _WIN32
	/* Initialize security descriptor for the new directories
	to be the same as for datadir */
	DWORD res = GetNamedSecurityInfoA(mysql_data_home,
		SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
		NULL, NULL, NULL, NULL,
		&my_dir_security_attributes.lpSecurityDescriptor);
	if (res != ERROR_SUCCESS) {
		msg("Unable to read security descriptor of %s",mysql_data_home);
	}
#endif

	if (srv_undo_dir && *srv_undo_dir
		&& !directory_exists(srv_undo_dir, true)) {
			return(false);
	}
	if (innobase_data_home_dir && *innobase_data_home_dir
		&& !directory_exists(innobase_data_home_dir, true)) {
			return(false);
	}
	if (srv_log_group_home_dir && *srv_log_group_home_dir
		&& !directory_exists(srv_log_group_home_dir, true)) {
			return(false);
	}

	/* cd to backup directory */
	if (my_setwd(xtrabackup_target_dir, MYF(MY_WME)))
	{
		msg("Can't my_setwd %s", xtrabackup_target_dir);
		return(false);
	}

	/* parse data file path */

	if (!innobase_data_file_path) {
  		innobase_data_file_path = (char*) "ibdata1:10M:autoextend";
	}

	srv_sys_space.set_path(".");

	if (!srv_sys_space.parse_params(innobase_data_file_path, true)) {
		msg("syntax error in innodb_data_file_path");
		return(false);
	}

	srv_max_n_threads = 1000;

	/* copy undo tablespaces */


	dst_dir = (srv_undo_dir && *srv_undo_dir)
			? srv_undo_dir : mysql_data_home;

	ds_data = ds_create(dst_dir, DS_TYPE_LOCAL);

	for (uint i = 1; i <= TRX_SYS_MAX_UNDO_SPACES; i++) {
		char filename[20];
		sprintf(filename, "undo%03u", i);
		if (!file_exists(filename)) {
			break;
		}
		if (!(ret = copy_or_move_file(filename, filename,
					      dst_dir, 1))) {
			goto cleanup;
		}
	}

	ds_destroy(ds_data);
	ds_data = NULL;

	/* copy redo logs */

	dst_dir = (srv_log_group_home_dir && *srv_log_group_home_dir)
		? srv_log_group_home_dir : mysql_data_home;

	/* --backup generates a single ib_logfile0, which we must copy. */

	ds_data = ds_create(dst_dir, DS_TYPE_LOCAL);
	if (!(ret = copy_or_move_file(LOG_FILE_NAME, LOG_FILE_NAME,
				      dst_dir, 1))) {
		goto cleanup;
	}
	ds_destroy(ds_data);

	/* copy innodb system tablespace(s) */

	dst_dir = (innobase_data_home_dir && *innobase_data_home_dir)
				? innobase_data_home_dir : mysql_data_home;

	ds_data = ds_create(dst_dir, DS_TYPE_LOCAL);

	for (Tablespace::const_iterator iter(srv_sys_space.begin()),
	     end(srv_sys_space.end());
	     iter != end;
	     ++iter) {
		const char *filepath = iter->filepath();
		if (!(ret = copy_or_move_file(base_name(filepath), filepath,
					      dst_dir, 1))) {
			goto cleanup;
		}
	}

	ds_destroy(ds_data);

	/* copy the rest of tablespaces */
	ds_data = ds_create(mysql_data_home, DS_TYPE_LOCAL);

	it = datadir_iter_new(".", false);

	datadir_node_init(&node);

	/* If mariabackup is run for Galera, then the file
	name is changed to the default so that the receiving
	node can find this file and rename it according to its
	settings, otherwise we keep the original file name: */
	size_t dir_length;
	const char *src_buffer_pool;
	src_buffer_pool = get_buffer_pool_filename(&dir_length);

	while (datadir_iter_next(it, &node)) {
		const char *ext_list[] = {"backup-my.cnf",
			"xtrabackup_binary", "xtrabackup_binlog_info",
			"xtrabackup_checkpoints", ".qp", ".pmap", ".tmp",
			NULL};
		const char *filename;
		char c_tmp;
		int i_tmp;

		if (strstr(node.filepath,"/" ROCKSDB_BACKUP_DIR "/")
#ifdef _WIN32
			|| strstr(node.filepath,"\\" ROCKSDB_BACKUP_DIR "\\")
#endif
		)
		{
			// copied at later step
			continue;
		}

		/* create empty directories */
		if (node.is_empty_dir) {
			char path[FN_REFLEN];

			snprintf(path, sizeof(path), "%s/%s",
				mysql_data_home, node.filepath_rel);

			msg("Creating directory %s", path);

			if (mkdirp(path, 0777, MYF(0)) < 0) {
				char errbuf[MYSYS_STRERROR_SIZE];
				my_strerror(errbuf, sizeof(errbuf), my_errno);
				msg("Can not create directory %s: %s",
					path, errbuf);
				ret = false;

				goto cleanup;

			}

			msg(" ...done.");

			continue;
		}

		filename = base_name(node.filepath);

		/* skip .qp files */
		if (filename_matches(filename, ext_list)) {
			continue;
		}

		/* skip undo tablespaces */
		if (sscanf(filename, "undo%d%c", &i_tmp, &c_tmp) == 1) {
			continue;
		}

		/* skip the redo log (it was already copied) */
		if (!strcmp(filename, LOG_FILE_NAME)) {
			continue;
		}

		/* skip buffer pool dump */
		if (!strcmp(filename, src_buffer_pool)) {
			continue;
		}

		/* skip innodb data files */
		for (Tablespace::const_iterator iter(srv_sys_space.begin()),
		       end(srv_sys_space.end()); iter != end; ++iter) {
			if (!strcmp(base_name(iter->filepath()), filename)) {
				goto next_file;
			}
		}

		if (!(ret = copy_or_move_file(node.filepath, node.filepath_rel,
					      mysql_data_home, 1))) {
			goto cleanup;
		}
	next_file:
		continue;
	}

	/* copy buffer pool dump */

	if (file_exists(src_buffer_pool)) {
		char dst_dir[FN_REFLEN];
		while (IS_TRAILING_SLASH(buffer_pool_filename, dir_length)) {
			dir_length--;
		}
		memcpy(dst_dir, buffer_pool_filename, dir_length);
		dst_dir[dir_length] = 0;
		if (!(ret = copy_or_move_file(src_buffer_pool,
				src_buffer_pool,
				dst_dir, 1)))
		{
			goto cleanup;
		}
	}

	rocksdb_copy_back();

cleanup:
	if (it != NULL) {
		datadir_iter_free(it);
	}

	datadir_node_free(&node);

	if (ds_data != NULL) {
		ds_destroy(ds_data);
	}

	ds_data = NULL;

	return(ret);
}

bool
decrypt_decompress_file(const char *filepath, uint thread_n)
{
	std::stringstream cmd, message;
	char *dest_filepath = strdup(filepath);
	bool needs_action = false;

	cmd << IF_WIN("type ","cat ") << filepath;

 	if (opt_decompress
 	    && ends_with(filepath, ".qp")) {
 		cmd << " | qpress -dio ";
 		dest_filepath[strlen(dest_filepath) - 3] = 0;
 		if (needs_action) {
 			message << " and ";
 		}
 		message << "decompressing";
 		needs_action = true;
 	}

 	cmd << " > " << dest_filepath;
 	message << " " << filepath;

 	free(dest_filepath);

 	if (needs_action) {

		msg(thread_n,"%s\n", message.str().c_str());

	 	if (system(cmd.str().c_str()) != 0) {
	 		return(false);
	 	}

		if (opt_remove_original) {
			msg(thread_n, "Removing %s", filepath);
			if (my_delete(filepath, MYF(MY_WME)) != 0) {
				return(false);
			}
		}
	 }

 	return(true);
}

static void decrypt_decompress_thread_func(datadir_thread_ctxt_t *ctxt)
{
	bool ret = true;
	datadir_node_t node;

	datadir_node_init(&node);

	while (datadir_iter_next(ctxt->it, &node)) {

		/* skip empty directories in backup */
		if (node.is_empty_dir) {
			continue;
		}

		if (!ends_with(node.filepath, ".qp")) {
			continue;
		}

		if (!(ret = decrypt_decompress_file(node.filepath,
							ctxt->n_thread))) {
			goto cleanup;
		}
	}

cleanup:

	datadir_node_free(&node);

	pthread_mutex_lock(ctxt->count_mutex);
	--(*ctxt->count);
	pthread_mutex_unlock(ctxt->count_mutex);

	ctxt->ret = ret;
}

bool
decrypt_decompress()
{
	bool ret;
	datadir_iter_t *it = NULL;

	srv_max_n_threads = 1000;

	/* cd to backup directory */
	if (my_setwd(xtrabackup_target_dir, MYF(MY_WME)))
	{
		msg("Can't my_setwd %s", xtrabackup_target_dir);
		return(false);
	}

	/* copy the rest of tablespaces */
	ds_data = ds_create(".", DS_TYPE_LOCAL);

	it = datadir_iter_new(".", false);

	ut_a(xtrabackup_parallel >= 0);

	ret = run_data_threads(it, decrypt_decompress_thread_func,
		xtrabackup_parallel ? xtrabackup_parallel : 1);

	if (it != NULL) {
		datadir_iter_free(it);
	}

	if (ds_data != NULL) {
		ds_destroy(ds_data);
	}

	ds_data = NULL;

	return(ret);
}

/*
  Copy some files from top level datadir.
  Do not copy the Innodb files (ibdata1, redo log files),
  as this is done in a separate step.
*/
static bool backup_files_from_datadir(const char *dir_path)
{
	os_file_dir_t dir = os_file_opendir(dir_path);
	if (dir == IF_WIN(INVALID_HANDLE_VALUE, nullptr)) return false;

	os_file_stat_t info;
	bool ret = true;
	while (os_file_readdir_next_file(dir_path, dir, &info) == 0) {

		if (info.type != OS_FILE_TYPE_FILE)
			continue;

		const char *pname = strrchr(info.name, '/');
#ifdef _WIN32
		if (const char *last = strrchr(info.name, '\\')) {
			if (!pname || last >pname) {
				pname = last;
			}
		}
#endif
		if (!pname)
			pname = info.name;

		if (!starts_with(pname, "aws-kms-key") &&
			!starts_with(pname, "aria_log"))
			/* For ES exchange the above line with the following code:
			(!xtrabackup_prepare || !xtrabackup_incremental_dir ||
				!starts_with(pname, "aria_log")))
			*/
			continue;

		if (xtrabackup_prepare && xtrabackup_incremental_dir &&
			file_exists(info.name))
			unlink(info.name);

		std::string full_path(dir_path);
		full_path.append(1, '/').append(info.name);
		if (!(ret = copy_file(ds_data, full_path.c_str() , info.name, 1)))
			break;
	}
	os_file_closedir(dir);
	return ret;
}


static int rocksdb_remove_checkpoint_directory()
{
	xb_mysql_query(mysql_connection, "set global rocksdb_remove_mariabackup_checkpoint=ON", false);
	return 0;
}

static bool has_rocksdb_plugin()
{
	static bool first_time = true;
	static bool has_plugin= false;
	if (!first_time || !xb_backup_rocksdb)
		return has_plugin;

	const char *query = "SELECT COUNT(*) FROM information_schema.plugins WHERE plugin_name='rocksdb'";
	MYSQL_RES* result = xb_mysql_query(mysql_connection, query, true);
	MYSQL_ROW row = mysql_fetch_row(result);
	if (row)
		has_plugin = !strcmp(row[0], "1");
	mysql_free_result(result);
	first_time = false;
	return has_plugin;
}

static char *trim_trailing_dir_sep(char *path)
{
	size_t path_len = strlen(path);
	while (path_len)
	{
		char c = path[path_len - 1];
		if (c == '/' IF_WIN(|| c == '\\', ))
			path_len--;
		else
			break;
	}
	path[path_len] = 0;
	return path;
}

/*
Create a file hardlink.
@return true on success, false on error.
*/
static bool make_hardlink(const char *from_path, const char *to_path)
{
	DBUG_EXECUTE_IF("no_hardlinks", return false;);
	char to_path_full[FN_REFLEN];
	if (!is_abs_path(to_path))
	{
		fn_format(to_path_full, to_path, ds_data->root, "", MYF(MY_RELATIVE_PATH));
	}
	else
	{
		strncpy(to_path_full, to_path, sizeof(to_path_full));
	}
#ifdef _WIN32
	return  CreateHardLink(to_path_full, from_path, NULL);
#else
	return !link(from_path, to_path_full);
#endif
}

/*
 Copies or moves a directory (non-recursively so far).
 Helper function used to backup rocksdb checkpoint, or copy-back the
 rocksdb files.

 Has optimization that allows to use hardlinks when possible
 (source and destination are directories on the same device)
*/
static void copy_or_move_dir(const char *from, const char *to, bool do_copy, bool allow_hardlinks)
{
	datadir_node_t node;
	datadir_node_init(&node);
	datadir_iter_t *it = datadir_iter_new(from, false);

	while (datadir_iter_next(it, &node))
	{
		char to_path[FN_REFLEN];
		const char *from_path = node.filepath;
		snprintf(to_path, sizeof(to_path), "%s/%s", to, base_name(from_path));
		bool rc = false;
		if (do_copy && allow_hardlinks)
		{
			rc = make_hardlink(from_path, to_path);
			if (rc)
			{
				msg("Creating hardlink from %s to %s",from_path, to_path);
			}
			else
			{
				allow_hardlinks = false;
			}
		}

		if (!rc) 
		{
			rc = (do_copy ?
				copy_file(ds_data, from_path, to_path, 1) :
				move_file(ds_data, from_path, node.filepath_rel,
					to, 1));
		}
		if (!rc)
			die("copy or move file failed");
	}
	datadir_iter_free(it);
	datadir_node_free(&node);

}

/*
  Obtain user level lock , to protect the checkpoint directory of the server
  from being  user/overwritten by different backup processes, if backups are
  running in parallel.
  
  This lock will be acquired before rocksdb checkpoint is created,  held
  while all files from it are being copied to their final backup destination,
  and finally released after the checkpoint is removed.
*/
static void rocksdb_lock_checkpoint()
{
	msg("Obtaining rocksdb checkpoint lock.");
	MYSQL_RES *res =
		xb_mysql_query(mysql_connection, "SELECT GET_LOCK('mariabackup_rocksdb_checkpoint',3600)", true, true);

	MYSQL_ROW r = mysql_fetch_row(res);
	if (r && r[0] && strcmp(r[0], "1"))
	{
		msg("Could not obtain rocksdb checkpont lock.");
		exit(EXIT_FAILURE);
	}
	mysql_free_result(res);
}

static void rocksdb_unlock_checkpoint()
{
	xb_mysql_query(mysql_connection, 
		"SELECT RELEASE_LOCK('mariabackup_rocksdb_checkpoint')", false, true);
}


/*
  Create temporary checkpoint in $rocksdb_datadir/mariabackup-checkpoint
  directory.
  A (user-level) lock named 'mariabackup_rocksdb_checkpoint' will also be
  acquired be this function.
*/
#define MARIADB_CHECKPOINT_DIR "mariabackup-checkpoint"
static 	char rocksdb_checkpoint_dir[FN_REFLEN];

static void rocksdb_create_checkpoint()
{
	MYSQL_RES *result = xb_mysql_query(mysql_connection, "SELECT @@rocksdb_datadir,@@datadir", true, true);
	MYSQL_ROW row = mysql_fetch_row(result);

	DBUG_ASSERT(row && row[0] && row[1]);

	char *rocksdbdir = row[0];
	char *datadir = row[1];

	if (is_abs_path(rocksdbdir))
	{
		snprintf(rocksdb_checkpoint_dir, sizeof(rocksdb_checkpoint_dir),
			"%s/" MARIADB_CHECKPOINT_DIR, trim_trailing_dir_sep(rocksdbdir));
	}
	else 
	{
		snprintf(rocksdb_checkpoint_dir, sizeof(rocksdb_checkpoint_dir),
			"%s/%s/" MARIADB_CHECKPOINT_DIR, trim_trailing_dir_sep(datadir),
			trim_dotslash(rocksdbdir));
	}
	mysql_free_result(result);

#ifdef _WIN32
	for (char *p = rocksdb_checkpoint_dir; *p; p++)
		if (*p == '\\') *p = '/';
#endif

	rocksdb_lock_checkpoint();

	if (!access(rocksdb_checkpoint_dir, 0))
	{
		msg("Removing rocksdb checkpoint from previous backup attempt.");
		rocksdb_remove_checkpoint_directory();
	}

	char query[FN_REFLEN + 32];
	snprintf(query, sizeof(query), "SET GLOBAL rocksdb_create_checkpoint='%s'", rocksdb_checkpoint_dir);
	xb_mysql_query(mysql_connection, query, false, true);
}

/*
  Copy files from rocksdb temporary checkpoint to final destination.
  remove temp.checkpoint directory (in server's datadir)
  and release user level lock acquired inside rocksdb_create_checkpoint().
*/
static void rocksdb_backup_checkpoint()
{
	msg("Backing up rocksdb files.");
	char rocksdb_backup_dir[FN_REFLEN];
	snprintf(rocksdb_backup_dir, sizeof(rocksdb_backup_dir), "%s/" ROCKSDB_BACKUP_DIR , xtrabackup_target_dir);
	bool backup_to_directory = xtrabackup_backup && xtrabackup_stream_fmt == XB_STREAM_FMT_NONE;
	if (backup_to_directory) 
	{
		if (my_mkdir(rocksdb_backup_dir, 0777, MYF(0))){
			die("Can't create rocksdb backup directory %s", rocksdb_backup_dir);
		}
	}
	copy_or_move_dir(rocksdb_checkpoint_dir, ROCKSDB_BACKUP_DIR, true, backup_to_directory);
	rocksdb_remove_checkpoint_directory();
	rocksdb_unlock_checkpoint();
}

/*
  Copies #rocksdb directory to the $rockdb_data_dir, on copy-back
*/
static void rocksdb_copy_back() {
	if (access(ROCKSDB_BACKUP_DIR, 0))
		return;
	char rocksdb_home_dir[FN_REFLEN];
        if (xb_rocksdb_datadir && is_abs_path(xb_rocksdb_datadir)) {
		strncpy(rocksdb_home_dir, xb_rocksdb_datadir, sizeof rocksdb_home_dir - 1);
		rocksdb_home_dir[sizeof rocksdb_home_dir - 1] = '\0';
	} else {
	   snprintf(rocksdb_home_dir, sizeof(rocksdb_home_dir), "%s/%s", mysql_data_home, 
		xb_rocksdb_datadir?trim_dotslash(xb_rocksdb_datadir): ROCKSDB_BACKUP_DIR);
	}
	mkdirp(rocksdb_home_dir, 0777, MYF(0));
	copy_or_move_dir(ROCKSDB_BACKUP_DIR, rocksdb_home_dir, xtrabackup_copy_back, xtrabackup_copy_back);
}
