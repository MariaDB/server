/******************************************************
Copyright (c) 2011-2017 Percona LLC and/or its affiliates.

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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

#include <my_global.h>
#include <my_base.h>
#include <zlib.h>
#include "common.h"
#include "xbstream.h"
#include "crc_glue.h"
#ifdef WITH_S3_STORAGE_ENGINE
#include "maria_def.h"
#include "s3_func.h"
#include <string>
#endif // WITH_S3_STORAGE_ENGINE

/* Allocate 1 MB for the payload buffer initially */
#define INIT_BUFFER_LEN (1024 * 1024)

#ifndef MY_OFF_T_MAX
#define MY_OFF_T_MAX (~(my_off_t)0UL)
#endif

class xb_stdin_stream : public xb_rstream {
	my_off_t	m_offset;
	File		m_fd;
public:
	xb_stdin_stream() : m_offset(0), m_fd(my_fileno(stdin)) {
#ifdef __WIN__
		setmode(fileno(stdin), _O_BINARY);
#endif //__WIN__
	}
	~xb_stdin_stream() override {}
	size_t read(uchar *buf, size_t len) override {
		size_t result = xb_read_full(m_fd, (uchar *)buf, len);
		m_offset += result;
		return result;
	}
	my_off_t offset() override {
		return m_offset;
	}
};

std::unique_ptr<xb_rstream> xb_stream_stdin_new(void) {
	return std::unique_ptr<xb_rstream>(new xb_stdin_stream());
}

#ifdef WITH_S3_STORAGE_ENGINE
class xb_s3_rstream : public xb_rstream {
	ms3_st *m_client;
	const char *m_bucket;
	const char *m_path;
	my_off_t m_offset;
	uint64_t m_seq_num;
	S3_BLOCK m_block;
	my_off_t m_block_offset;
public:

	xb_s3_rstream(ms3_st *client, const char *bucket, const char *path) :
		m_client(client), m_bucket(bucket), m_path(path), m_offset(0),
		m_seq_num(0), m_block{nullptr, nullptr, 0}, m_block_offset(0) {}

	~xb_s3_rstream() override {
		if (m_block.alloc_ptr)
			s3_free(&m_block);
		if (m_client)
			ms3_deinit(m_client);
	}

	size_t read(uchar *dst, size_t len) override;

	my_off_t offset() override {
		return m_offset;
	}

};

size_t xb_s3_rstream::read(uchar *dst, size_t len) {
	size_t copied = 0;

	while (len) {

		if (m_block_offset + len > m_block.length) {
			size_t tail_size = m_block.length - m_block_offset;
			if (tail_size) {
				memcpy(dst, m_block.str + m_block_offset, tail_size);
				dst += tail_size;
				len -= tail_size;
				m_offset += tail_size;
				m_block_offset += tail_size;
				copied += tail_size;
			}
			if (m_block.alloc_ptr)
				s3_free(&m_block);
			m_block_offset = 0;
			std::string block_path(m_path);
			block_path.append("/").append(std::to_string(m_seq_num++));
			ms3_status_st status;
			if (ms3_status(m_client, m_bucket, block_path.c_str(), &status))
				return copied;
			if (s3_get_object(m_client, m_bucket, block_path.c_str(), &m_block,
						false, 1))
				return copied;
			continue;
		}

		memcpy(dst, m_block.str + m_block_offset, len);
		m_offset += len;
		m_block_offset += len;
		copied += len;
		break;
	}

	return copied;
}

std::unique_ptr<xb_rstream> xb_stream_s3_new(
	const char *access_key,
	const char *secret_key,
	const char *region,
	const char *host_name,
	const char *bucket,
	const char *path,
	ulong protocol_version) {

	S3_INFO info;
	info.protocol_version= (uint8_t) protocol_version;
	lex_string_set(&info.host_name,  host_name);
	lex_string_set(&info.access_key, access_key);
	lex_string_set(&info.secret_key, secret_key);
	lex_string_set(&info.region,     region);
	lex_string_set(&info.bucket,     bucket);
	ms3_st *s3_client;
	if (!(s3_client= s3_open_connection(&info)))
		die("Can't open connection to S3, error: %d %s", errno, ms3_error(errno));

	return std::unique_ptr<xb_rstream>(
			new xb_s3_rstream(s3_client, bucket, path));
}
#endif // WITH_S3_STORAGE_ENGINE

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

xb_rstream_result_t
xb_stream_validate_checksum(xb_rstream_chunk_t *chunk)
{
	ulong	checksum;

	checksum = crc32_iso3309(0, (unsigned char *)chunk->data, (uint)chunk->length);
	if (checksum != chunk->checksum) {
		msg("xb_stream_read_chunk(): invalid checksum at offset "
		    "0x%llx: expected 0x%lx, read 0x%lx.",
		    (ulonglong) chunk->checksum_offset, chunk->checksum,
		    checksum);
		return XB_STREAM_READ_ERROR;
	}

	return XB_STREAM_READ_CHUNK;
}

#define F_READ(buf,len) \
	do { \
		if (stream->read((uchar *)buf, len) < len) { \
			msg("xb_stream_read_chunk(): stream->read() failed."); \
			goto err; \
		} \
	} while (0)

xb_rstream_result_t
xb_stream_read_chunk(xb_rstream *stream, xb_rstream_chunk_t *chunk)
{
	uchar		tmpbuf[16];
	uchar		*ptr = tmpbuf;
	uint		pathlen;
	size_t		tbytes;
	ulonglong	ullval;
	my_off_t offset = stream->offset();

	xb_ad(sizeof(tmpbuf) >= CHUNK_HEADER_CONSTANT_LEN);

	/* This is the only place where we expect EOF, so read with
	xb_read_full() rather than F_READ() */
	tbytes = stream->read(ptr, CHUNK_HEADER_CONSTANT_LEN);
	if (tbytes == 0) {
		return XB_STREAM_READ_EOF;
	} else if (tbytes < CHUNK_HEADER_CONSTANT_LEN) {
		msg("xb_stream_read_chunk(): unexpected end of stream at "
		    "offset 0x%llx.", offset);
		goto err;
	}

	ptr = tmpbuf;

	/* Chunk magic value */
	if (memcmp(tmpbuf, XB_STREAM_CHUNK_MAGIC, 8)) {
		msg("xb_stream_read_chunk(): wrong chunk magic at offset "
		    "0x%llx.", (ulonglong) offset);
		goto err;
	}
	ptr += 8;
	offset += 8;

	/* Chunk flags */
	chunk->flags = *ptr++;
	offset++;

	/* Chunk type, ignore unknown ones if ignorable flag is set */
	chunk->type = validate_chunk_type(*ptr);
	if (chunk->type == XB_CHUNK_TYPE_UNKNOWN &&
	    !(chunk->flags & XB_STREAM_FLAG_IGNORABLE)) {
		msg("xb_stream_read_chunk(): unknown chunk type 0x%lu at "
		    "offset 0x%llx.", (ulong) *ptr,
		    (ulonglong) offset);
		goto err;
	}
	ptr++;
	offset++;

	/* Path length */
	pathlen = uint4korr(ptr);
	if (pathlen >= FN_REFLEN) {
		msg("xb_stream_read_chunk(): path length (%lu) is too large at "
		    "offset 0x%llx.", (ulong) pathlen, offset);
		goto err;
	}
	chunk->pathlen = pathlen;
	offset +=4;

	xb_ad((ptr + 4 - tmpbuf) == CHUNK_HEADER_CONSTANT_LEN);

	/* Path */
	if (chunk->pathlen > 0) {
		F_READ((uchar *) chunk->path, pathlen);
		offset += pathlen;
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
		    "offset 0x%llx: 0x%llx.", (ulonglong) offset,
		    ullval);
		goto err;
	}
	chunk->length = (size_t) ullval;
	offset += 8;

	/* Payload offset */
	ullval = uint8korr(tmpbuf + 8);
	if (ullval > (ulonglong) MY_OFF_T_MAX) {
		msg("xb_stream_read_chunk(): chunk offset is too large at "
		    "offset 0x%llx: 0x%llx.", (ulonglong) offset,
		    ullval);
		goto err;
	}
	chunk->offset = (my_off_t) ullval;
	offset += 8;

	/* Reallocate the buffer if needed */
	if (chunk->length > chunk->buflen) {
		chunk->data = my_realloc(chunk->data, chunk->length,
					 MYF(MY_WME | MY_ALLOW_ZERO_PTR));
		if (chunk->data == NULL) {
			msg("xb_stream_read_chunk(): failed to increase buffer "
			    "to %lu bytes.", (ulong) chunk->length);
			goto err;
		}
		chunk->buflen = chunk->length;
	}

	/* Checksum */
	F_READ(tmpbuf, 4);
	chunk->checksum = uint4korr(tmpbuf);
	chunk->checksum_offset = offset;

	/* Payload */
	if (chunk->length > 0) {
		F_READ(chunk->data, chunk->length);
		offset += chunk->length;
	}

	offset += 4;

	return XB_STREAM_READ_CHUNK;

err:
	return XB_STREAM_READ_ERROR;
}
