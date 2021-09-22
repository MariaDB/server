/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Streaming implementation for XtraBackup.

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

#include <mysql_version.h>
#include <my_base.h>
#include "common.h"
#include "datasink.h"
#include "xbstream.h"

typedef struct {
	xb_wstream_t	*xbstream;
	ds_file_t	*dest_file;
	pthread_mutex_t	mutex;
} ds_stream_ctxt_t;

typedef struct {
	xb_wstream_file_t	*xbstream_file;
	ds_stream_ctxt_t	*stream_ctxt;
} ds_stream_file_t;

/***********************************************************************
General streaming interface */

static ds_ctxt_t *xbstream_init(const char *root);
static ds_file_t *xbstream_open(ds_ctxt_t *ctxt, const char *path,
			      MY_STAT *mystat);
static int xbstream_write(ds_file_t *file, const uchar *buf, size_t len);
static int xbstream_close(ds_file_t *file);
static void xbstream_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_xbstream = {
	&xbstream_init,
	&xbstream_open,
	&xbstream_write,
	&xbstream_close,
	&dummy_remove,
	&xbstream_deinit
};

static
ssize_t
my_xbstream_write_callback(xb_wstream_file_t *f __attribute__((unused)),
		       void *userdata, const void *buf, size_t len)
{
	ds_stream_ctxt_t	*stream_ctxt;

	stream_ctxt = (ds_stream_ctxt_t *) userdata;

	xb_ad(stream_ctxt != NULL);
	xb_ad(stream_ctxt->dest_file != NULL);

	if (!ds_write(stream_ctxt->dest_file, buf, len)) {
		return len;
	}
	return -1;
}

static
ds_ctxt_t *
xbstream_init(const char *root __attribute__((unused)))
{
	ds_ctxt_t		*ctxt;
	ds_stream_ctxt_t	*stream_ctxt;
	xb_wstream_t *xbstream;

	ctxt = (ds_ctxt_t *)my_malloc(sizeof(ds_ctxt_t) + sizeof(ds_stream_ctxt_t),
			 MYF(MY_FAE));
	stream_ctxt = (ds_stream_ctxt_t *)(ctxt + 1);

	if (pthread_mutex_init(&stream_ctxt->mutex, NULL)) {
		msg("xbstream_init: pthread_mutex_init() failed.");
		goto err;
	}

	xbstream = xb_stream_write_new();
	if (xbstream == NULL) {
		msg("xb_stream_write_new() failed.");
		goto err;
	}
	stream_ctxt->xbstream = xbstream;
	stream_ctxt->dest_file = NULL;

	ctxt->ptr = stream_ctxt;

	return ctxt;

err:
	my_free(ctxt);
	return NULL;
}

static
ds_file_t *
xbstream_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat)
{
	ds_file_t		*file;
	ds_stream_file_t	*stream_file;
	ds_stream_ctxt_t	*stream_ctxt;
	ds_ctxt_t		*dest_ctxt;
	xb_wstream_t		*xbstream;
	xb_wstream_file_t	*xbstream_file;


	xb_ad(ctxt->pipe_ctxt != NULL);
	dest_ctxt = ctxt->pipe_ctxt;

	stream_ctxt = (ds_stream_ctxt_t *) ctxt->ptr;

	pthread_mutex_lock(&stream_ctxt->mutex);
	if (stream_ctxt->dest_file == NULL) {
		stream_ctxt->dest_file = ds_open(dest_ctxt, path, mystat);
	}
	pthread_mutex_unlock(&stream_ctxt->mutex);
	if (stream_ctxt->dest_file == NULL) {
		return NULL;
	}

	file = (ds_file_t *) my_malloc(sizeof(ds_file_t) +
				       sizeof(ds_stream_file_t),
				       MYF(MY_FAE));
	if (!file) {
		msg("my_malloc() failed.");
		goto err;
	}
	stream_file = (ds_stream_file_t *) (file + 1);

	xbstream = stream_ctxt->xbstream;

	xbstream_file = xb_stream_write_open(xbstream, path, mystat,
		                             stream_ctxt,
					     my_xbstream_write_callback);

	if (xbstream_file == NULL) {
		msg("xb_stream_write_open() failed.");
		goto err;
	}

	stream_file->xbstream_file = xbstream_file;
	stream_file->stream_ctxt = stream_ctxt;
	file->ptr = stream_file;
	file->path = stream_ctxt->dest_file->path;

	return file;

err:
	if (stream_ctxt->dest_file) {
		ds_close(stream_ctxt->dest_file);
		stream_ctxt->dest_file = NULL;
	}
	my_free(file);

	return NULL;
}

static
int
xbstream_write(ds_file_t *file, const uchar *buf, size_t len)
{
	ds_stream_file_t	*stream_file;
	xb_wstream_file_t	*xbstream_file;


	stream_file = (ds_stream_file_t *) file->ptr;

	xbstream_file = stream_file->xbstream_file;

	if (xb_stream_write_data(xbstream_file, buf, len)) {
		msg("xb_stream_write_data() failed.");
		return 1;
	}

	return 0;
}

static
int
xbstream_close(ds_file_t *file)
{
	ds_stream_file_t	*stream_file;
	int			rc = 0;

	stream_file = (ds_stream_file_t *)file->ptr;

	rc = xb_stream_write_close(stream_file->xbstream_file);

	my_free(file);

	return rc;
}

static
void
xbstream_deinit(ds_ctxt_t *ctxt)
{
	ds_stream_ctxt_t	*stream_ctxt;

	stream_ctxt = (ds_stream_ctxt_t *) ctxt->ptr;

	if (xb_stream_write_done(stream_ctxt->xbstream)) {
		msg("xb_stream_done() failed.");
	}

	if (stream_ctxt->dest_file) {
		ds_close(stream_ctxt->dest_file);
		stream_ctxt->dest_file = NULL;
	}

	pthread_mutex_destroy(&stream_ctxt->mutex);

	my_free(ctxt);
}
