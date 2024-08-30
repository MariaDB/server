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

#include <my_global.h>
#include <my_base.h>
#include <zlib.h>
#include <stdint.h>
#include "common.h"
#include "xbstream.h"

/* Group writes smaller than this into a single chunk */
#define XB_STREAM_MIN_CHUNK_SIZE (10 * 1024 * 1024)

struct xb_wstream_struct {
	pthread_mutex_t	mutex;
	xb_stream_write_callback *write;
	void *user_data;
};

struct xb_wstream_file_struct {
	xb_wstream_t	*stream;
	char		*path;
	size_t		path_len;
	char		chunk[XB_STREAM_MIN_CHUNK_SIZE];
	char		*chunk_ptr;
	size_t		chunk_free;
	my_off_t	offset;
  bool rewrite;
};

static int xb_stream_flush(xb_wstream_file_t *file);
static int xb_stream_write_chunk(xb_wstream_file_t *file,
				 const void *buf, size_t len);
static int xb_stream_write_eof(xb_wstream_file_t *file);

static
ssize_t
xb_stream_default_write_callback(
				 void *userdata __attribute__((unused)),
				 const void *buf, size_t len)
{
	if (my_write(my_fileno(stdout), (const uchar *)buf, len, MYF(MY_WME | MY_NABP)))
		return -1;
	return len;
}

xb_wstream_t *
xb_stream_write_new(
	xb_stream_write_callback *write_callback, void *user_data)
{
	xb_wstream_t	*stream;

	stream = (xb_wstream_t *) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(xb_wstream_t), MYF(MY_FAE));
	pthread_mutex_init(&stream->mutex, NULL);
	if (write_callback) {
#ifdef _WIN32
		setmode(fileno(stdout), _O_BINARY);
#endif
		stream->write = write_callback;
		stream->user_data = user_data;
	}
	else {
		stream->write = xb_stream_default_write_callback;
		stream->user_data = user_data;
	}

	return stream;;
}

xb_wstream_file_t *
xb_stream_write_open(xb_wstream_t *stream, const char *path,
		const MY_STAT *mystat __attribute__((unused)), bool rewrite)
{
	xb_wstream_file_t	*file;
	size_t			path_len;

	path_len = strlen(path);

	if (path_len > FN_REFLEN) {
		msg("xb_stream_write_open(): file path is too long.");
		return NULL;
	}

	file = (xb_wstream_file_t *) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(xb_wstream_file_t) +
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
	file->rewrite = rewrite;

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

	*ptr++ =
    file->rewrite ? XB_STREAM_FLAG_REWRITE : 0; /* Chunk flags */

	*ptr++ = (uchar) XB_CHUNK_TYPE_PAYLOAD;  /* Chunk type */

	int4store(ptr, file->path_len);          /* Path length */
	ptr += 4;

	memcpy(ptr, file->path, file->path_len); /* Path */
	ptr += file->path_len;

	int8store(ptr, len);                     /* Payload length */
	ptr += 8;

	checksum = my_checksum(0, buf, len);

	pthread_mutex_lock(&stream->mutex);

	int8store(ptr, file->offset);            /* Payload offset */
	ptr += 8;

	int4store(ptr, checksum);
	ptr += 4;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	if (stream->write(stream->user_data, tmpbuf, ptr-tmpbuf) == -1)
		goto err;


	if (stream->write(stream->user_data, buf, len) == -1) /* Payload */
		goto err;

	file->offset+= len;

	pthread_mutex_unlock(&stream->mutex);

	return 0;

err:

	pthread_mutex_unlock(&stream->mutex);

	return 1;
}

int xb_stream_write_seek_set(xb_wstream_file_t *file, my_off_t offset)
{
	/* Chunk magic + flags + chunk type + path_len + path + offset */
	uchar		tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 +
			       FN_REFLEN + 8];
	int error = 0;
	xb_wstream_t	*stream = file->stream;
	uchar		*ptr = tmpbuf;
	/* Chunk magic */
	memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
	ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;
	*ptr++ = 0;                              /* Chunk flags */
	*ptr++ = (uchar) XB_CHUNK_TYPE_SEEK;   /* Chunk type */
	int4store(ptr, file->path_len);          /* Path length */
	ptr += 4;
	memcpy(ptr, file->path, file->path_len); /* Path */
	ptr += file->path_len;
	int8store(ptr, static_cast<int64_t>(offset));  /* Offset */
	ptr += 8;
	if (xb_stream_flush(file))
		return 1;
	pthread_mutex_lock(&stream->mutex);
	if (stream->write(stream->user_data, tmpbuf, ptr-tmpbuf) == -1)
		error = 1;
	if (!error)
		file->offset = offset;
	pthread_mutex_unlock(&stream->mutex);
	if (xb_stream_flush(file))
		return 1;
	return error;
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

	if (stream->write(stream->user_data, tmpbuf,
			(ulonglong) (ptr - tmpbuf)) == -1)
		goto err;

	pthread_mutex_unlock(&stream->mutex);

	return 0;
err:

	pthread_mutex_unlock(&stream->mutex);

	return 1;
}


int
xb_stream_write_remove(xb_wstream_t *stream, const char *path) {
	/* Chunk magic + flags + chunk type + path_len + path */
	uchar		tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 + 4 + FN_REFLEN];
	uchar		*ptr = tmpbuf;
	/* Chunk magic */
	memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
	ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

	*ptr++ = 0;                              /* Chunk flags */

	*ptr++ = (uchar) XB_CHUNK_TYPE_REMOVE;   /* Chunk type */
	size_t path_len = strlen(path);
	int4store(ptr, path_len);               /* Path length */
	ptr += 4;

	memcpy(ptr, path, path_len);            /* Path */
	ptr += path_len;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	pthread_mutex_lock(&stream->mutex);

	ssize_t result = stream->write(stream->user_data, tmpbuf,
		(ulonglong) (ptr - tmpbuf));

	pthread_mutex_unlock(&stream->mutex);

	return result < 0;

}

int
xb_stream_write_rename(
	xb_wstream_t *stream, const char *old_path, const char *new_path) {
	/* Chunk magic + flags + chunk type + path_len + path + path_len + path*/
	uchar		tmpbuf[sizeof(XB_STREAM_CHUNK_MAGIC) - 1 + 1 + 1 +
		4 + FN_REFLEN + 4 + FN_REFLEN];
	uchar		*ptr = tmpbuf;
	/* Chunk magic */
	memcpy(ptr, XB_STREAM_CHUNK_MAGIC, sizeof(XB_STREAM_CHUNK_MAGIC) - 1);
	ptr += sizeof(XB_STREAM_CHUNK_MAGIC) - 1;

	*ptr++ = 0;                              /* Chunk flags */

	*ptr++ = (uchar) XB_CHUNK_TYPE_RENAME;   /* Chunk type */
	size_t path_len = strlen(old_path);
	int4store(ptr, path_len);               /* Path length */
	ptr += 4;

	memcpy(ptr, old_path, path_len);            /* Path */
	ptr += path_len;

	path_len = strlen(new_path);
	int4store(ptr, path_len);               /* Path length */
	ptr += 4;

	memcpy(ptr, new_path, path_len);            /* Path */
	ptr += path_len;

	xb_ad(ptr <= tmpbuf + sizeof(tmpbuf));

	pthread_mutex_lock(&stream->mutex);

	ssize_t result = stream->write(stream->user_data, tmpbuf,
		(ulonglong) (ptr - tmpbuf));

	pthread_mutex_unlock(&stream->mutex);

	return result < 0;
}

