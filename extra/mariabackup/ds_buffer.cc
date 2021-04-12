/******************************************************
Copyright (c) 2012-2013 Percona LLC and/or its affiliates.

buffer datasink for XtraBackup.

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

/* Does buffered output to a destination datasink set with ds_set_pipe().
Writes to the destination datasink are guaranteed to not be smaller than a
specified buffer size (DS_DEFAULT_BUFFER_SIZE by default), with the only
exception for the last write for a file. */

#include <mysql_version.h>
#include <my_base.h>
#include "ds_buffer.h"
#include "common.h"
#include "datasink.h"

#define DS_DEFAULT_BUFFER_SIZE (64 * 1024)

typedef struct {
	ds_file_t	*dst_file;
	char		*buf;
	size_t		 pos;
	size_t		size;
} ds_buffer_file_t;

typedef struct {
	size_t	buffer_size;
} ds_buffer_ctxt_t;

static ds_ctxt_t *buffer_init(const char *root);
static ds_file_t *buffer_open(ds_ctxt_t *ctxt, const char *path,
			      MY_STAT *mystat);
static int buffer_write(ds_file_t *file, const uchar *buf, size_t len);
static int buffer_close(ds_file_t *file);
static void buffer_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_buffer = {
	&buffer_init,
	&buffer_open,
	&buffer_write,
	&buffer_close,
	&dummy_remove,
	&buffer_deinit
};

/* Change the default buffer size */
void ds_buffer_set_size(ds_ctxt_t *ctxt, size_t size)
{
	ds_buffer_ctxt_t *buffer_ctxt = (ds_buffer_ctxt_t *) ctxt->ptr;

	buffer_ctxt->buffer_size = size;
}

static ds_ctxt_t *
buffer_init(const char *root)
{
	ds_ctxt_t		*ctxt;
	ds_buffer_ctxt_t	*buffer_ctxt;

	ctxt = (ds_ctxt_t *)my_malloc(sizeof(ds_ctxt_t) + sizeof(ds_buffer_ctxt_t),
			 MYF(MY_FAE));
	buffer_ctxt = (ds_buffer_ctxt_t *) (ctxt + 1);
	buffer_ctxt->buffer_size = DS_DEFAULT_BUFFER_SIZE;

	ctxt->ptr = buffer_ctxt;
	ctxt->root = my_strdup(root, MYF(MY_FAE));

	return ctxt;
}

static ds_file_t *
buffer_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_buffer_ctxt_t	*buffer_ctxt;
	ds_ctxt_t		*pipe_ctxt;
	ds_file_t		*dst_file;
	ds_file_t		*file;
	ds_buffer_file_t	*buffer_file;

	pipe_ctxt = ctxt->pipe_ctxt;
	xb_a(pipe_ctxt != NULL);

	dst_file = ds_open(pipe_ctxt, path, mystat);
	if (dst_file == NULL) {
		die("ds_open(%s) failed", path);
	}

	buffer_ctxt = (ds_buffer_ctxt_t *) ctxt->ptr;

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_buffer_file_t) +
				       buffer_ctxt->buffer_size,
				       MYF(MY_FAE));

	buffer_file = (ds_buffer_file_t *) (file + 1);
	buffer_file->dst_file = dst_file;
	buffer_file->buf = (char *) (buffer_file + 1);
	buffer_file->size = buffer_ctxt->buffer_size;
	buffer_file->pos = 0;

	file->path = dst_file->path;
	file->ptr = buffer_file;

	return file;
}

static int
buffer_write(ds_file_t *file, const uchar *buf, size_t len)
{
	ds_buffer_file_t	*buffer_file;

	buffer_file = (ds_buffer_file_t *) file->ptr;

	while (len > 0) {
		if (buffer_file->pos + len > buffer_file->size) {
			if (buffer_file->pos > 0) {
				size_t bytes;

				bytes = buffer_file->size - buffer_file->pos;
				memcpy(buffer_file->buf + buffer_file->pos, buf,
				       bytes);

				if (ds_write(buffer_file->dst_file,
					     buffer_file->buf,
					     buffer_file->size)) {
					return 1;
				}

				buffer_file->pos = 0;

				buf += bytes;
				len -= bytes;
			} else {
				/* We don't have any buffered bytes, just write
				the entire source buffer */
				if (ds_write(buffer_file->dst_file, buf, len)) {
					return 1;
				}
				break;
			}
		} else {
			memcpy(buffer_file->buf + buffer_file->pos, buf, len);
			buffer_file->pos += len;
			break;
		}
	}

	return 0;
}

static int
buffer_close(ds_file_t *file)
{
	ds_buffer_file_t	*buffer_file;
	int			ret;

	buffer_file = (ds_buffer_file_t *) file->ptr;
	if (buffer_file->pos > 0) {
		ds_write(buffer_file->dst_file, buffer_file->buf,
			 buffer_file->pos);
	}

	ret = ds_close(buffer_file->dst_file);

	my_free(file);

	return ret;
}

static void
buffer_deinit(ds_ctxt_t *ctxt)
{
	my_free(ctxt->root);
	my_free(ctxt);
}
