/******************************************************
Copyright (c) 2012 Percona LLC and/or its affiliates.

tmpfile datasink for XtraBackup.

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

/* Do all writes to temporary files first, then pipe them to the specified
datasink in a serialized way in deinit(). */

#include <my_base.h>
#include "common.h"
#include "datasink.h"

typedef struct {
	pthread_mutex_t	 mutex;
	LIST		*file_list;
} ds_tmpfile_ctxt_t;

typedef struct {
	LIST		 list;
	File		 fd;
	char		*orig_path;
	MY_STAT		 mystat;
	ds_file_t	*file;
} ds_tmp_file_t;

static ds_ctxt_t *tmpfile_init(const char *root);
static ds_file_t *tmpfile_open(ds_ctxt_t *ctxt, const char *path,
			       MY_STAT *mystat);
static int tmpfile_write(ds_file_t *file, const uchar *buf, size_t len);
static int tmpfile_close(ds_file_t *file);
static void tmpfile_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_tmpfile = {
	&tmpfile_init,
	&tmpfile_open,
	&tmpfile_write,
	&tmpfile_close,
	&dummy_remove,
	&tmpfile_deinit
};


static ds_ctxt_t *
tmpfile_init(const char *root)
{
	ds_ctxt_t		*ctxt;
	ds_tmpfile_ctxt_t	*tmpfile_ctxt;

	ctxt = (ds_ctxt_t *)my_malloc(sizeof(ds_ctxt_t) + sizeof(ds_tmpfile_ctxt_t),
			 MYF(MY_FAE));
	tmpfile_ctxt = (ds_tmpfile_ctxt_t *) (ctxt + 1);
	tmpfile_ctxt->file_list = NULL;
	if (pthread_mutex_init(&tmpfile_ctxt->mutex, NULL)) {

		my_free(ctxt);
		return NULL;
	}

	ctxt->ptr = tmpfile_ctxt;
	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static ds_file_t *
tmpfile_open(ds_ctxt_t *ctxt, const char *path,
			       MY_STAT *mystat)
{
	ds_tmpfile_ctxt_t	*tmpfile_ctxt;
	char			 tmp_path[FN_REFLEN];
	ds_tmp_file_t		*tmp_file;
	ds_file_t		*file;
	size_t			 path_len;
	File			 fd;

	/* Create a temporary file in tmpdir. The file will be automatically
	removed on close. Code copied from mysql_tmpfile(). */
	fd = create_temp_file(tmp_path,xtrabackup_tmpdir,
			      "xbtemp",
#ifdef __WIN__
			      O_BINARY | O_TRUNC | O_SEQUENTIAL |
			      O_TEMPORARY | O_SHORT_LIVED |
#endif /* __WIN__ */
			      O_CREAT | O_EXCL | O_RDWR,
			      MYF(MY_WME));

#ifndef __WIN__
	if (fd >= 0) {
		/* On Windows, open files cannot be removed, but files can be
		created with the O_TEMPORARY flag to the same effect
		("delete on close"). */
		unlink(tmp_path);
	}
#endif /* !__WIN__ */

	if (fd < 0) {
		return NULL;
	}

	path_len = strlen(path) + 1; /* terminating '\0' */

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_tmp_file_t) + path_len,
				       MYF(MY_FAE));

	tmp_file = (ds_tmp_file_t *) (file + 1);
	tmp_file->file = file;
	memcpy(&tmp_file->mystat, mystat, sizeof(MY_STAT));
	/* Save a copy of 'path', since it may not be accessible later */
	tmp_file->orig_path = (char *) tmp_file + sizeof(ds_tmp_file_t);

	tmp_file->fd = fd;
	memcpy(tmp_file->orig_path, path, path_len);

	/* Store the real temporary file name in file->path */
	file->path = my_strdup(tmp_path, MYF(MY_FAE));
	file->ptr = tmp_file;

	/* Store the file object in the list to be piped later */
	tmpfile_ctxt = (ds_tmpfile_ctxt_t *) ctxt->ptr;
	tmp_file->list.data = tmp_file;

	pthread_mutex_lock(&tmpfile_ctxt->mutex);
	tmpfile_ctxt->file_list = list_add(tmpfile_ctxt->file_list,
					   &tmp_file->list);
	pthread_mutex_unlock(&tmpfile_ctxt->mutex);

	return file;
}

static int
tmpfile_write(ds_file_t *file, const uchar *buf, size_t len)
{
	File fd = ((ds_tmp_file_t *) file->ptr)->fd;

	if (!my_write(fd, buf, len, MYF(MY_WME | MY_NABP))) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		return 0;
	}

	return 1;
}

static int
tmpfile_close(ds_file_t *file)
{
	/* Do nothing -- we will close (and thus remove) the file after piping
	it to the destination datasink in tmpfile_deinit(). */

	my_free(file->path);

	return 0;
}

static void
tmpfile_deinit(ds_ctxt_t *ctxt)
{
	LIST			*list;
	ds_tmpfile_ctxt_t	*tmpfile_ctxt;
	MY_STAT			 mystat;
	ds_tmp_file_t 		*tmp_file;
	ds_file_t		*dst_file;
	ds_ctxt_t		*pipe_ctxt;
	void			*buf = NULL;
	const size_t		 buf_size = 10 * 1024 * 1024;
	size_t			 bytes;
	size_t			 offset;

	pipe_ctxt = ctxt->pipe_ctxt;
	xb_a(pipe_ctxt != NULL);

	buf = my_malloc(buf_size, MYF(MY_FAE));

	tmpfile_ctxt = (ds_tmpfile_ctxt_t *) ctxt->ptr;
	list = tmpfile_ctxt->file_list;

	/* Walk the files in the order they have been added */
	list = list_reverse(list);
	while (list != NULL) {
		tmp_file = (ds_tmp_file_t *)list->data;
		/* Stat the file to replace size and mtime on the original
		* mystat struct */
		if (my_fstat(tmp_file->fd, &mystat, MYF(0))) {
			die("my_fstat() failed.");
		}
		tmp_file->mystat.st_size = mystat.st_size;
		tmp_file->mystat.st_mtime = mystat.st_mtime;

		dst_file = ds_open(pipe_ctxt, tmp_file->orig_path,
				   &tmp_file->mystat);
		if (dst_file == NULL) {
			die("could not stream a temporary file to "
			    "'%s'", tmp_file->orig_path);
		}

		/* copy to the destination datasink */
		posix_fadvise(tmp_file->fd, 0, 0, POSIX_FADV_SEQUENTIAL);
		if (my_seek(tmp_file->fd, 0, SEEK_SET, MYF(0)) ==
		    MY_FILEPOS_ERROR) {
			die("my_seek() failed for '%s', errno = %d.",
			    tmp_file->file->path, my_errno);
		}
		offset = 0;
		while ((bytes = my_read(tmp_file->fd, (unsigned char *)buf, buf_size,
					MYF(MY_WME))) > 0) {
			posix_fadvise(tmp_file->fd, offset, buf_size, POSIX_FADV_DONTNEED);
			offset += buf_size;
			if (ds_write(dst_file, buf, bytes)) {
				die("cannot write to stream for '%s'.",
				    tmp_file->orig_path);
			}
		}
		if (bytes == (size_t) -1) {
			die("my_read failed for %s", tmp_file->orig_path);
		}

		my_close(tmp_file->fd, MYF(MY_WME));
		ds_close(dst_file);

		list = list_rest(list);
		my_free(tmp_file->file);
	}

	pthread_mutex_destroy(&tmpfile_ctxt->mutex);

	my_free(buf);
	my_free(ctxt->root);
	my_free(ctxt);
}
