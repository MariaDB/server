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

#include <my_config.h>
#include <mysql_version.h>
#include <my_base.h>
#include <mysys_err.h>
#include "common.h"
#include "datasink.h"
#include "fsp0fsp.h"
#ifdef _WIN32
#include <winioctl.h>
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
			     MY_STAT *mystat);
static int local_write(ds_file_t *file, const uchar *buf, size_t len);
static int local_close(ds_file_t *file);
static void local_deinit(ds_ctxt_t *ctxt);

static int local_remove(const char *path)
{
	return unlink(path);
}

extern "C" {
datasink_t datasink_local = {
	&local_init,
	&local_open,
	&local_write,
	&local_close,
	&local_remove,
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
		my_error(EE_CANT_MKDIR, MYF(ME_BELL | ME_WAITTANG),
			 root, my_errno,errbuf, my_errno);
		return NULL;
	}

	ctxt = (ds_ctxt_t *)my_malloc(sizeof(ds_ctxt_t), MYF(MY_FAE));

	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
local_open(ds_ctxt_t *ctxt, const char *path,
	   MY_STAT *mystat __attribute__((unused)))
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
		my_error(EE_CANT_MKDIR, MYF(ME_BELL | ME_WAITTANG),
			 dirpath, my_errno, errbuf);
		return NULL;
	}

	fd = my_create(fullpath, 0, O_WRONLY | O_BINARY | O_EXCL | O_NOFOLLOW,
		     MYF(MY_WME));
	if (fd < 0) {
		return NULL;
	}

	path_len = strlen(fullpath) + 1; /* terminating '\0' */

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
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
			if (my_seek(fd, hole_bytes, MY_SEEK_CUR, MYF(MY_WME | MY_NABP))
				== MY_FILEPOS_ERROR)
			 return 1;
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

	ulint flags = mach_read_from_4(&buf[FIL_PAGE_DATA + FSP_SPACE_FLAGS]);
	ulint ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
	local_file->pagesize= ssize == 0 ? UNIV_PAGE_SIZE_ORIG : ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
	local_file->compressed =  (my_bool)FSP_FLAGS_HAS_PAGE_COMPRESSION(flags);

#if defined(_WIN32) && (MYSQL_VERSION_ID > 100200)
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
