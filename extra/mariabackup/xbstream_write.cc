/******************************************************
Copyright (c) 2011-2017 Percona LLC and/or its affiliates.

The xbstream format writer implementation.

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
#include <zlib.h>
#include "common.h"
#include "xbstream.h"
#include "crc_glue.h"

/* Group writes smaller than this into a single chunk */
#define XB_STREAM_MIN_CHUNK_SIZE (10 * 1024 * 1024)

struct xb_wstream_struct {
	pthread_mutex_t	mutex;
};

struct xb_wstream_file_struct {
	xb_wstream_t	*stream;
	char		*path;
	size_t		path_len;
	char		chunk[XB_STREAM_MIN_CHUNK_SIZE];
	char		*chunk_ptr;
	size_t		chunk_free;
	my_off_t	offset;
	void		*userdata;
	xb_stream_write_callback *write;
};

static int xb_stream_flush(xb_wstream_file_t *file);
static int xb_stream_write_chunk(xb_wstream_file_t *file,
				 const void *buf, size_t len);
static int xb_stream_write_eof(xb_wstream_file_t *file);

static
ssize_t
xb_stream_default_write_callback(xb_wstream_file_t *file __attribute__((unused)),
				 void *userdata __attribute__((unused)),
				 const void *buf, size_t len)
{
	if (my_write(my_fileno(stdout), (const uchar *)buf, len, MYF(MY_WME | MY_NABP)))
		return -1;
	return len;
}

xb_wstream_t *
xb_stream_write_new(void)
{
	xb_wstream_t	*stream;

	stream = (xb_wstream_t *) my_malloc(sizeof(xb_wstream_t), MYF(MY_FAE));
	pthread_mutex_init(&stream->mutex, NULL);

	return stream;;
}

xb_wstream_file_t *
xb_stream_write_open(xb_wstream_t *stream, const char *path,
		     MY_STAT *mystat __attribute__((unused)),
		     void *userdata,
		     xb_stream_write_callback *onwrite)
{
	xb_wstream_file_t	*file;
	size_t			path_len;

	path_len = strlen(path);

	if (path_len > FN_REFLEN) {
		msg("xb_stream_write_open(): file path is too long.");
		return NULL;
	}

	file = (xb_wstream_file_t *) my_malloc(sizeof(xb_wstream_file_t) +
					       path_len + 1, MYF(MY_FAE));

	file->path = (char *) (file + 1);
#ifdef _WIN32
	/* Normalize path on Windows, so we can restore elsewhere.*/
	{
		int i;
		for (i = 0; ; i++) {
			file->path[i] = (path[i] == '\\') ? '/' : path[i];
			if (!path[i])
				break;
		}
	}
#else
	memcpy(file->path, path, path_len + 1);
#endif
	file->path_len = path_len;

	file->stream = stream;
	file->offset = 0;
	file->chunk_ptr = file->chunk;
	file->chunk_free = XB_STREAM_MIN_CHUNK_SIZE;
	if (onwrite) {
#ifdef __WIN__
		setmode(fileno(stdout), _O_BINARY);
#endif
		file->userdata = userdata;
		file->write = onwrite;
	} else {
		file->userdata = NULL;
		file->write = xb_stream_default_write_callback;
	}

	return file;
}

int
xb_stream_write_data(xb_wstream_file_t *file, const void *buf, size_t len)
{
	if (len < file->chunk_free) {
		memcpy(file->chunk_ptr, buf, len);
		file->chunk_ptr += len;
		file->chunk_free -= len;

		return 0;
	}

	if (xb_stream_flush(file))
		return 1;

	return xb_stream_write_chunk(file, buf, len);
}

int
xb_stream_write_close(xb_wstream_file_t *file)
{
	if (xb_stream_flush(file) ||
	    xb_stream_write_eof(file)) {
		my_free(file);
		return 1;
	}

	my_free(file);

	return 0;
}

int
xb_stream_write_done(xb_wstream_t *stream)
{
	pthread_mutex_destroy(&stream->mutex);

	my_free(stream);

	return 0;
}

static
int
xb_stream_flush(xb_wstream_file_t *file)
{
	if (file->chunk_ptr == file->chunk) {
		return 0;
	}

	if (xb_stream_write_chunk(file, file->chunk,
				  file->chunk_ptr - file->chunk)) {
		return 1;
	}

	file->chunk_ptr = file->chunk;
	file->chunk_free = XB_STREAM_MIN_CHUNK_SIZE;

	return 0;
}

static
int
xb_stream_write_chunk(xb_wstream_file_t *file, const void *buf, size_t len)
{
	/* Chunk magic + flags + chunk type + path_len + path + len + offset +
	checksum */
	uchar		tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 +
			       FN_REFLEN + 8 + 8 + 4];
	uchar		*ptr;
	xb_wstream_t	*stream = file->stream;
	ulong		checksum;

	/* Write xbstream header */
	ptr = tmpbuf;

	/* Chunk magic */
	memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
	ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

	*ptr++ = 0;                              /* Chunk flags */

	*ptr++ = (uchar) XB_CHUNK_TYPE_PAYLOAD;  /* Chunk type */

	int4store(ptr, file->path_len);          /* Path length */
	ptr += 4;

	memcpy(ptr, file->path, file->path_len); /* Path */
	ptr += file->path_len;

	int8store(ptr, len);                     /* Payload length */
	ptr += 8;

	checksum = crc32_iso3309(0, (const uchar *)buf, (uint)len);   /* checksum */

	pthread_mutex_lock(&stream->mutex);

	int8store(ptr, file->offset);            /* Payload offset */
	ptr += 8;

	int4store(ptr, checksum);
	ptr += 4;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	if (file->write(file, file->userdata, tmpbuf, ptr-tmpbuf) == -1)
		goto err;


	if (file->write(file, file->userdata, buf, len) == -1) /* Payload */
		goto err;

	file->offset+= len;

	pthread_mutex_unlock(&stream->mutex);

	return 0;

err:

	pthread_mutex_unlock(&stream->mutex);

	return 1;
}

static
int
xb_stream_write_eof(xb_wstream_file_t *file)
{
	/* Chunk magic + flags + chunk type + path_len + path */
	uchar		tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 +
			       FN_REFLEN];
	uchar		*ptr;
	xb_wstream_t	*stream = file->stream;

	pthread_mutex_lock(&stream->mutex);

	/* Write xbstream header */
	ptr = tmpbuf;

	/* Chunk magic */
	memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
	ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

	*ptr++ = 0;                              /* Chunk flags */

	*ptr++ = (uchar) XB_CHUNK_TYPE_EOF;      /* Chunk type */

	int4store(ptr, file->path_len);          /* Path length */
	ptr += 4;

	memcpy(ptr, file->path, file->path_len); /* Path */
	ptr += file->path_len;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	if (file->write(file, file->userdata, tmpbuf,
			(ulonglong) (ptr - tmpbuf)) == -1)
		goto err;

	pthread_mutex_unlock(&stream->mutex);

	return 0;
err:

	pthread_mutex_unlock(&stream->mutex);

	return 1;
}
