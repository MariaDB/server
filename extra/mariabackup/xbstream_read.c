/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

The xbstream format reader implementation.

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
#include <zlib.h>
#include "common.h"
#include "xbstream.h"

/* Allocate 1 MB for the payload buffer initially */
#define INIT_BUFFER_LEN (1024 * 1024)

#ifndef MY_OFF_T_MAX
#define MY_OFF_T_MAX (~(my_off_t)0UL)
#endif

struct xb_rstream_struct {
	my_off_t	offset;
	File 		fd;
	void 		*buffer;
	size_t		buflen;
};

xb_rstream_t *
xb_stream_read_new(void)
{
	xb_rstream_t *stream;

	stream = (xb_rstream_t *) my_malloc(sizeof(xb_rstream_t), MYF(MY_FAE));

	stream->buffer = my_malloc(INIT_BUFFER_LEN, MYF(MY_FAE));
	stream->buflen = INIT_BUFFER_LEN;

#ifdef __WIN__
	setmode(fileno(stdin), _O_BINARY);
#endif

	stream->fd = my_fileno(stdin);
	stream->offset = 0;

	return stream;
}

static inline
xb_chunk_type_t
validate_chunk_type(uchar code)
{
	switch ((xb_chunk_type_t) code) {
	case XB_CHUNK_TYPE_PAYLOAD:
	case XB_CHUNK_TYPE_EOF:
		return (xb_chunk_type_t) code;
	default:
		return XB_CHUNK_TYPE_UNKNOWN;
	}
}

#define F_READ(buf,len)                                       	\
	do {                                                      	\
		if (xb_read_full(fd, buf, len) < len) {	\
			msg("xb_stream_read_chunk(): my_read() failed.\n"); \
			goto err;                                 	\
		}							\
	} while (0)

xb_rstream_result_t
xb_stream_read_chunk(xb_rstream_t *stream, xb_rstream_chunk_t *chunk)
{
	uchar		tmpbuf[16];
	uchar		*ptr = tmpbuf;
	uint		pathlen;
	size_t		tbytes;
	ulonglong	ullval;
	ulong		checksum_exp;
	ulong		checksum;
	File		fd = stream->fd;

	xb_ad(sizeof(tmpbuf) >= CHUNK_HEADER_CONSTANT_LEN);

	/* This is the only place where we expect EOF, so read with
	xb_read_full() rather than F_READ() */
	tbytes = xb_read_full(fd, ptr, CHUNK_HEADER_CONSTANT_LEN);
	if (tbytes == 0) {
		return XB_STREAM_READ_EOF;
	} else if (tbytes < CHUNK_HEADER_CONSTANT_LEN) {
		msg("xb_stream_read_chunk(): unexpected end of stream at "
		    "offset 0x%llx.\n", stream->offset);
		goto err;
	}

	ptr = tmpbuf;

	/* Chunk magic value */
	if (memcmp(tmpbuf, XB_STREAM_CHUNK_MAGIC, 8)) {
		msg("xb_stream_read_chunk(): wrong chunk magic at offset "
		    "0x%llx.\n", (ulonglong) stream->offset);
		goto err;
	}
	ptr += 8;
	stream->offset += 8;

	/* Chunk flags */
	chunk->flags = *ptr++;
	stream->offset++;

	/* Chunk type, ignore unknown ones if ignorable flag is set */
	chunk->type = validate_chunk_type(*ptr);
	if (chunk->type == XB_CHUNK_TYPE_UNKNOWN &&
	    !(chunk->flags & XB_STREAM_FLAG_IGNORABLE)) {
		msg("xb_stream_read_chunk(): unknown chunk type 0x%lu at "
		    "offset 0x%llx.\n", (ulong) *ptr,
		    (ulonglong) stream->offset);
		goto err;
	}
	ptr++;
	stream->offset++;

	/* Path length */
	pathlen = uint4korr(ptr);
	if (pathlen >= FN_REFLEN) {
		msg("xb_stream_read_chunk(): path length (%lu) is too large at "
		    "offset 0x%llx.\n", (ulong) pathlen, stream->offset);
		goto err;
	}
	chunk->pathlen = pathlen;
	stream->offset +=4;

	xb_ad((ptr + 4 - tmpbuf) == CHUNK_HEADER_CONSTANT_LEN);

	/* Path */
	if (chunk->pathlen > 0) {
		F_READ((uchar *) chunk->path, pathlen);
		stream->offset += pathlen;
	}
	chunk->path[pathlen] = '\0';

	if (chunk->type == XB_CHUNK_TYPE_EOF) {
		return XB_STREAM_READ_CHUNK;
	}

	/* Payload length */
	F_READ(tmpbuf, 16);
	ullval = uint8korr(tmpbuf);
	if (ullval > (ulonglong) SIZE_T_MAX) {
		msg("xb_stream_read_chunk(): chunk length is too large at "
		    "offset 0x%llx: 0x%llx.\n", (ulonglong) stream->offset,
		    ullval);
		goto err;
	}
	chunk->length = (size_t) ullval;
	stream->offset += 8;

	/* Payload offset */
	ullval = uint8korr(tmpbuf + 8);
	if (ullval > (ulonglong) MY_OFF_T_MAX) {
		msg("xb_stream_read_chunk(): chunk offset is too large at "
		    "offset 0x%llx: 0x%llx.\n", (ulonglong) stream->offset,
		    ullval);
		goto err;
	}
	chunk->offset = (my_off_t) ullval;
	stream->offset += 8;

	/* Reallocate the buffer if needed */
	if (chunk->length > stream->buflen) {
		stream->buffer = my_realloc(stream->buffer, chunk->length,
					    MYF(MY_WME));
		if (stream->buffer == NULL) {
			msg("xb_stream_read_chunk(): failed to increase buffer "
			    "to %lu bytes.\n", (ulong) chunk->length);
			goto err;
		}
		stream->buflen = chunk->length;
	}

	/* Checksum */
	F_READ(tmpbuf, 4);
	checksum_exp = uint4korr(tmpbuf);

	/* Payload */
	if (chunk->length > 0) {
		F_READ(stream->buffer, chunk->length);
		stream->offset += chunk->length;
	}

	checksum = crc32(0, stream->buffer, chunk->length);
	if (checksum != checksum_exp) {
		msg("xb_stream_read_chunk(): invalid checksum at offset "
		    "0x%llx: expected 0x%lx, read 0x%lx.\n",
		    (ulonglong) stream->offset, checksum_exp, checksum);
		goto err;
	}
	stream->offset += 4;

	chunk->data = stream->buffer;
	chunk->checksum = checksum;

	return XB_STREAM_READ_CHUNK;

err:
	return XB_STREAM_READ_ERROR;
}

int
xb_stream_read_done(xb_rstream_t *stream)
{
	my_free(stream->buffer);
	my_free(stream);

	return 0;
}
