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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#include <mysql_version.h>
#include <my_base.h>
#include <mysys_err.h>
#include "common.h"
#include "datasink.h"

typedef struct {
	File fd;
} ds_local_file_t;

static ds_ctxt_t *local_init(const char *root);
static ds_file_t *local_open(ds_ctxt_t *ctxt, const char *path,
			     MY_STAT *mystat);
static int local_write(ds_file_t *file, const void *buf, size_t len);
static int local_close(ds_file_t *file);
static void local_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_local = {
	&local_init,
	&local_open,
	&local_write,
	&local_close,
	&local_deinit
};

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

	ctxt = my_malloc(sizeof(ds_ctxt_t), MYF(MY_FAE));

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

	file->path = (char *) local_file + sizeof(ds_local_file_t);
	memcpy(file->path, fullpath, path_len);

	file->ptr = local_file;

	return file;
}

static
int
local_write(ds_file_t *file, const void *buf, size_t len)
{
	File fd = ((ds_local_file_t *) file->ptr)->fd;

	if (!my_write(fd, buf, len, MYF(MY_WME | MY_NABP))) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		return 0;
	}

	return 1;
}

static
int
local_close(ds_file_t *file)
{
	File fd = ((ds_local_file_t *) file->ptr)->fd;

	my_free(file);

	my_sync(fd, MYF(MY_WME));

	return my_close(fd, MYF(MY_WME));
}

static
void
local_deinit(ds_ctxt_t *ctxt)
{
	my_free(ctxt->root);
	my_free(ctxt);
}
