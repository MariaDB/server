/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

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

typedef struct {
	File fd;
} ds_stdout_file_t;

static ds_ctxt_t *stdout_init(const char *root);
static ds_file_t *stdout_open(ds_ctxt_t *ctxt, const char *path,
			     MY_STAT *mystat);
static int stdout_write(ds_file_t *file, const uchar *buf, size_t len);
static int stdout_close(ds_file_t *file);
static void stdout_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_stdout = {
	&stdout_init,
	&stdout_open,
	&stdout_write,
	&stdout_close,
	&dummy_remove,
	&stdout_deinit
};

static
ds_ctxt_t *
stdout_init(const char *root)
{
	ds_ctxt_t *ctxt;

	ctxt = (ds_ctxt_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_ctxt_t), MYF(MY_FAE));

	ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));

	return ctxt;
}

static
ds_file_t *
stdout_open(ds_ctxt_t *ctxt __attribute__((unused)),
	    const char *path __attribute__((unused)),
	    MY_STAT *mystat __attribute__((unused)))
{
	ds_stdout_file_t 	*stdout_file;
	ds_file_t		*file;
	size_t			pathlen;
	const char		*fullpath = "<STDOUT>";

	pathlen = strlen(fullpath) + 1;

	file = (ds_file_t *) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_file_t) +
				       sizeof(ds_stdout_file_t) + pathlen, MYF(MY_FAE));
	stdout_file = (ds_stdout_file_t *) (file + 1);


#ifdef _WIN32
	setmode(fileno(stdout), _O_BINARY);
#endif

	stdout_file->fd = my_fileno(stdout);

	file->path = (char *) stdout_file + sizeof(ds_stdout_file_t);
	memcpy(file->path, fullpath, pathlen);

	file->ptr = stdout_file;

	return file;
}

static
int
stdout_write(ds_file_t *file, const uchar *buf, size_t len)
{
	File fd = ((ds_stdout_file_t *) file->ptr)->fd;

	if (!my_write(fd, buf, len, MYF(MY_WME | MY_NABP))) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		return 0;
	}

	return 1;
}

static
int
stdout_close(ds_file_t *file)
{
	my_free(file);

	return 1;
}

static
void
stdout_deinit(ds_ctxt_t *ctxt)
{
	my_free(ctxt->root);
	my_free(ctxt);
}
