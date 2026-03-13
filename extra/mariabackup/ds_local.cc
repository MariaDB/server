/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Local datasink implementation for XtraBackup.

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

*******************************************************/

#include <my_global.h>
#include <my_base.h>
#include <mysys_err.h>
#include "common.h"
#include "datasink.h"
#include "fsp0fsp.h"
#ifdef _WIN32
#include <winioctl.h>
#endif

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
#include <linux/falloc.h>
#endif

typedef struct {
	File fd;
	my_bool init_ibd_done;
	my_bool is_ibd;
	my_bool compressed;
	size_t pagesize;
} ds_local_file_t;

static ds_ctxt_t *local_init(const char *root);
static ds_file_t *local_open(ds_ctxt_t *ctxt, const char *path,
			     const MY_STAT *mystat, bool rewrite);
static int local_write(ds_file_t *file, const uchar *buf, size_t len);
static int local_seek_set(ds_file_t *file, my_off_t offset);
static int local_close(ds_file_t *file);
static void local_deinit(ds_ctxt_t *ctxt);

static int local_remove(const char *path)
{
	return unlink(path);
}

static int local_rename(
	ds_ctxt_t *ctxt, const char *old_path, const char *new_path);
static int local_mremove(ds_ctxt_t *ctxt, const char *path);

extern "C" {
datasink_t datasink_local = {
	&local_init,
	&local_open,
	&local_write,
	&local_seek_set,
	&local_close,
	&local_remove,
	&local_rename,
	&local_mremove,
	&local_deinit
};
}

static
ds_ctxt_t *
local_init(const char *root)
{
	ds_ctxt_t *ctxt;

	if (my_mkdir(root, 0777, MYF(0)) < 0
	    && my_errno != EEXIST && my_errno != EISDIR)
	{
		char errbuf[MYSYS_STRERROR_SIZE];
		my_strerror(errbuf, sizeof(errbuf),my_errno);
		my_error(EE_CANT_MKDIR, MYF(ME_BELL),
			 root, my_errno,errbuf, my_errno);
		return NULL;
	}

	ctxt = (ds_ctxt_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_ctxt_t), MYF(MY_FAE));

	ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
local_open(ds_ctxt_t *ctxt, const char *path,
	   const MY_STAT *mystat __attribute__((unused)), bool rewrite)
{
	char 		fullpath[FN_REFLEN];
	char		dirpath[FN_REFLEN];
	size_t		dirpath_len;
	size_t		path_len;
	ds_local_file_t *local_file;
	ds_file_t	*file;
	File 		fd;

	fn_format(fullpath, path, ctxt->root, "", MYF(MY_RELATIVE_PATH));

	/* Create the directory if needed */
	dirname_part(dirpath, fullpath, &dirpath_len);
	if (my_mkdir(dirpath, 0777, MYF(0)) < 0 && my_errno != EEXIST) {
		char errbuf[MYSYS_STRERROR_SIZE];
		my_strerror(errbuf, sizeof(errbuf), my_errno);
		my_error(EE_CANT_MKDIR, MYF(ME_BELL),
			 dirpath, my_errno, errbuf);
		return NULL;
	}

	// TODO: check in Windows and set the corresponding flags on fail
	fd = my_create(fullpath, 0,
		O_WRONLY | O_BINARY | (rewrite ? O_TRUNC : O_EXCL) | O_NOFOLLOW,
		MYF(MY_WME));
	if (fd < 0) {
		return NULL;
	}

	path_len = strlen(fullpath) + 1; /* terminating '\0' */

	file = (ds_file_t *) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_file_t) +
				       sizeof(ds_local_file_t) +
				       path_len,
				       MYF(MY_FAE));
	local_file = (ds_local_file_t *) (file + 1);

	local_file->fd = fd;
	local_file->init_ibd_done = 0;
	local_file->is_ibd = (path_len > 5) && !strcmp(fullpath + path_len - 5, ".ibd");
	local_file->compressed = 0;
	local_file->pagesize = 0;
	file->path = (char *) local_file + sizeof(ds_local_file_t);
	memcpy(file->path, fullpath, path_len);

	file->ptr = local_file;

	return file;
}

/* Calculate size of data without trailing zero bytes. */
static size_t trim_binary_zeros(uchar *buf, size_t pagesize)
{
	size_t i;
	for (i = pagesize; (i > 0) && (buf[i - 1] == 0); i--) {};
	return i;
}


/*  Write data to the output file, and punch "holes" if needed. */
static int write_compressed(File fd,  uchar *data, size_t len, size_t pagesize)
{
	uchar *ptr = data;
	for (size_t written= 0; written < len;)
	{
		size_t n_bytes =  MY_MIN(pagesize, len - written);
		size_t datasize= trim_binary_zeros(ptr,n_bytes);
		if (datasize > 0) {
			if (!my_write(fd, ptr, datasize, MYF(MY_WME | MY_NABP)))
				posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			else
				return 1;
		}
		if (datasize < n_bytes) {
			/* This punches a "hole" in the file. */
			size_t hole_bytes = n_bytes - datasize;
                       my_off_t off = my_seek(fd, hole_bytes, MY_SEEK_CUR, MYF(MY_WME | MY_NABP));
                       if (off == MY_FILEPOS_ERROR)
                         return 1;
#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
                       /* punch holes harder for filesystems (like XFS) that
                          heuristically decide whether leave a hole after the
                          above or not based on the current access pattern
                          (which is sequential write and not at all typical for
                          what InnoDB will be doing with the file later */
                       fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                 off - hole_bytes, hole_bytes);
#endif
		}
		written += n_bytes;
		ptr += n_bytes;
	}
	return 0;
}


/* Calculate Innodb tablespace specific data, when first page is written.
   We're interested in page compression and page size.
*/
static void init_ibd_data(ds_local_file_t *local_file, const uchar *buf, size_t len)
{
	if (len < FIL_PAGE_DATA + FSP_SPACE_FLAGS) {
		/* Weird, bail out.*/
		return;
	}

	uint32_t flags = mach_read_from_4(&buf[FIL_PAGE_DATA + FSP_SPACE_FLAGS]);
	uint32_t ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
	local_file->pagesize= ssize == 0 ? UNIV_PAGE_SIZE_ORIG : ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
	local_file->compressed = fil_space_t::full_crc32(flags)
		? fil_space_t::is_compressed(flags)
		: bool(FSP_FLAGS_HAS_PAGE_COMPRESSION(flags));

#if defined(_WIN32)
	/* Make compressed file sparse, on Windows.
	In 10.1, we do not use sparse files. */
	if (local_file->compressed) {
		HANDLE handle= my_get_osfhandle(local_file->fd);
		if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, NULL, 0)) {
			fprintf(stderr, "Warning: cannot make file sparse");
			local_file->compressed = 0;
		}
	}
#endif
}


static
int
local_write(ds_file_t *file, const uchar *buf, size_t len)
{
	uchar *b = (uchar*)buf;
	ds_local_file_t *local_file= (ds_local_file_t *)file->ptr;
	File fd = local_file->fd;

	if (local_file->is_ibd && !local_file->init_ibd_done) {
		init_ibd_data(local_file, b , len);
		local_file->init_ibd_done= 1;
	}

	if (local_file->compressed) {
		return write_compressed(fd, b, len, local_file->pagesize);
	}

	if (!my_write(fd, b , len, MYF(MY_WME | MY_NABP))) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		return 0;
	}
	return 1;
}

static
int
local_seek_set(ds_file_t *file, my_off_t offset) {
	ds_local_file_t *local_file= (ds_local_file_t *)file->ptr;
	if (my_seek(local_file->fd, offset, SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR)
		return 1;
	return 0;
}

/* Set EOF at file's current position.*/
static int set_eof(File fd)
{
#ifdef _WIN32
	return !SetEndOfFile(my_get_osfhandle(fd));
#elif defined(HAVE_FTRUNCATE)
	return ftruncate(fd, my_tell(fd, MYF(MY_WME)));
#else
#error no ftruncate
#endif
}


static
int
local_close(ds_file_t *file)
{
	ds_local_file_t *local_file= (ds_local_file_t *)file->ptr;
	File fd = local_file->fd;
	int ret= 0;

	if (local_file->compressed) {
		ret = set_eof(fd);
	}

	my_close(fd, MYF(MY_WME));
	my_free(file);
	return ret;
}

static
void
local_deinit(ds_ctxt_t *ctxt)
{
	my_free(ctxt->root);
	my_free(ctxt);
}


static int local_rename(
	ds_ctxt_t *ctxt, const char *old_path, const char *new_path) {
	char		full_old_path[FN_REFLEN];
	char		full_new_path[FN_REFLEN];
	fn_format(full_old_path, old_path, ctxt->root, "", MYF(MY_RELATIVE_PATH));
	fn_format(full_new_path, new_path, ctxt->root, "", MYF(MY_RELATIVE_PATH));
	// Ignore errors as .frm files can me copied separately.
	// TODO: return error processing here after the corresponding changes in
	// xtrabackup.cc
	(void)my_rename(full_old_path, full_new_path, MYF(0));
//	if (my_rename(full_old_path, full_new_path, MYF(0))) {
//		msg("Failed to rename file %s to %s", old_path, new_path);
//		return 1;
//	}
	return 0;
}

// It's ok if destination does not contain the file or folder
static int local_mremove(ds_ctxt_t *ctxt, const char *path) {
	char		full_path[FN_REFLEN];
	fn_format(full_path, path, ctxt->root, "", MYF(MY_RELATIVE_PATH));
	size_t full_path_len = strlen(full_path);
	if (full_path[full_path_len - 1] == '*') {
		full_path[full_path_len - 1] = '\0';
		char *preffix = strrchr(full_path, '/');
		const char *full_path_dir = full_path;
		size_t preffix_len;
		if (preffix) {
			preffix_len = (full_path_len - 1) - (preffix - full_path);
			*(preffix++) = '\0';
		}
		else {
			preffix = full_path;
			preffix_len = full_path_len - 1;
			full_path_dir= IF_WIN(".\\", "./");
		}
		if (!preffix_len)
			return 0;
		MY_DIR *dir= my_dir(full_path_dir, 0);
		if (!dir)
			return 0;
		for (size_t i = 0; i < dir->number_of_files; ++i) {
			char full_fpath[FN_REFLEN];
			if (strncmp(dir->dir_entry[i].name, preffix, preffix_len))
				continue;
			fn_format(full_fpath, dir->dir_entry[i].name,
					full_path_dir, "", MYF(MY_RELATIVE_PATH));
			(void)my_delete(full_fpath, MYF(0));
		}
		my_dirend(dir);
	}
	else {
		MY_STAT stat;
		if (!my_stat(full_path, &stat, MYF(0)))
			return 0;
		MY_DIR *dir= my_dir(full_path, 0);
		if (!dir) {
			// TODO: check for error here if necessary
			(void)my_delete(full_path, MYF(0));
			return 0;
		}
		for (size_t i = 0; i < dir->number_of_files; ++i) {
			char		full_fpath[FN_REFLEN];
			fn_format(full_fpath, dir->dir_entry[i].name,
					full_path, "", MYF(MY_RELATIVE_PATH));
			(void)my_delete(full_fpath, MYF(0));
		}
		my_dirend(dir);
		(void)my_rmtree(full_path, MYF(0));
	}
	return 0;
}
