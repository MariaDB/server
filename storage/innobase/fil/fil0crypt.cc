/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (c) 2014, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/
/**************************************************//**
@file fil0crypt.cc
Innodb file space encrypt/decrypt

Created            Jonas Oreland Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/

#include "fil0crypt.h"
#include "mtr0types.h"
#include "mach0data.h"
#include "page0zip.h"
#include "buf0checksum.h"
#ifdef UNIV_INNOCHECKSUM
# include "buf0buf.h"
#else
#include "buf0dblwr.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "ut0ut.h"
#include "fsp0fsp.h"
#include "fil0pagecompress.h"
#include <my_crypt.h>

static bool fil_crypt_threads_inited = false;

/** Is encryption enabled/disabled */
ulong srv_encrypt_tables;

/** No of key rotation threads requested */
uint srv_n_fil_crypt_threads;

/** No of key rotation threads started */
uint srv_n_fil_crypt_threads_started;

/** At this age or older a space/page will be rotated */
uint srv_fil_crypt_rotate_key_age;

/** Whether the encryption plugin does key rotation */
Atomic_relaxed<bool> srv_encrypt_rotate;

/** Condition variable for srv_n_fil_crypt_threads_started */
static pthread_cond_t fil_crypt_cond;

/** Condition variable to to signal the key rotation threads */
static pthread_cond_t fil_crypt_threads_cond;

/** Condition variable for interrupting sleeptime_ms sleep at the end
of fil_crypt_rotate_page() */
static pthread_cond_t fil_crypt_throttle_sleep_cond;

/** Mutex for key rotation threads. Acquired before fil_system.mutex! */
static mysql_mutex_t fil_crypt_threads_mutex;

/** Variable ensuring only 1 thread at time does initial conversion */
static bool fil_crypt_start_converting;

/** Variables for throttling */
uint srv_n_fil_crypt_iops;	 // 10ms per iop
static constexpr uint srv_alloc_time = 3; // allocate iops for 3s at a time
static uint n_fil_crypt_iops_allocated;

#define DEBUG_KEYROTATION_THROTTLING 0

/** Statistics variables */
static fil_crypt_stat_t crypt_stat;
static mysql_mutex_t crypt_stat_mutex;

/** Wake up the encryption threads */
void fil_crypt_threads_signal(bool broadcast)
{
  mysql_mutex_lock(&fil_crypt_threads_mutex);
  if (broadcast)
    pthread_cond_broadcast(&fil_crypt_threads_cond);
  else
    pthread_cond_signal(&fil_crypt_threads_cond);
  mysql_mutex_unlock(&fil_crypt_threads_mutex);
}

/***********************************************************************
Check if a key needs rotation given a key_state
@param[in]	crypt_data		Encryption information
@param[in]	key_version		Current key version
@param[in]	latest_key_version	Latest key version
@param[in]	rotate_key_age		when to rotate
@return true if key needs rotation, false if not */
static bool
fil_crypt_needs_rotation(
	const fil_space_crypt_t*	crypt_data,
	uint				key_version,
	uint				latest_key_version,
	uint				rotate_key_age)
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************
Init space crypt */
void fil_space_crypt_init()
{
  pthread_cond_init(&fil_crypt_throttle_sleep_cond, nullptr);
  mysql_mutex_init(0, &crypt_stat_mutex, nullptr);
}

/*********************************************************************
Cleanup space crypt */
void fil_space_crypt_cleanup()
{
  pthread_cond_destroy(&fil_crypt_throttle_sleep_cond);
  mysql_mutex_destroy(&crypt_stat_mutex);
}

/**
Get latest key version from encryption plugin.
@return key version or ENCRYPTION_KEY_VERSION_INVALID */
uint
fil_space_crypt_t::key_get_latest_version(void)
{
	uint key_version = key_found;

	if (is_key_found()) {
		key_version = encryption_key_get_latest_version(key_id);
		/* InnoDB does dirty read of srv_fil_crypt_rotate_key_age.
		It doesn't matter because srv_encrypt_rotate
		can be set to true only once */
		if (!srv_encrypt_rotate
		    && key_version > srv_fil_crypt_rotate_key_age) {
			srv_encrypt_rotate = true;
		}

		srv_stats.n_key_requests.inc();
		key_found = key_version;
	}

	return key_version;
}

/******************************************************************
Get the latest(key-version), waking the encrypt thread, if needed
@param[in,out]	crypt_data	Crypt data */
static inline
uint
fil_crypt_get_latest_key_version(
	fil_space_crypt_t* crypt_data)
{
	ut_ad(crypt_data != NULL);

	uint key_version = crypt_data->key_get_latest_version();

	if (crypt_data->is_key_found()) {

		if (fil_crypt_needs_rotation(
				crypt_data,
				crypt_data->min_key_version,
				key_version,
				srv_fil_crypt_rotate_key_age)) {
			if (fil_crypt_threads_inited) {
				fil_crypt_threads_signal();
			}
		}
	}

	return key_version;
}

/******************************************************************
Mutex helper for crypt_data->scheme */
void
crypt_data_scheme_locker(
/*=====================*/
	st_encryption_scheme*	scheme,
	int			exit)
{
	fil_space_crypt_t* crypt_data =
		static_cast<fil_space_crypt_t*>(scheme);

	if (exit) {
		mysql_mutex_unlock(&crypt_data->mutex);
	} else {
		mysql_mutex_lock(&crypt_data->mutex);
	}
}

/******************************************************************
Create a fil_space_crypt_t object
@param[in]	type		CRYPT_SCHEME_UNENCRYPTE or
				CRYPT_SCHEME_1
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF
@param[in]	min_key_version key_version or 0
@param[in]	key_id		Used key id
@return crypt object */
static
fil_space_crypt_t*
fil_space_create_crypt_data(
	uint			type,
	fil_encryption_t	encrypt_mode,
	uint			min_key_version,
	uint			key_id)
{
	fil_space_crypt_t* crypt_data = NULL;
	if (void* buf = ut_zalloc_nokey(sizeof(fil_space_crypt_t))) {
		crypt_data = new(buf)
			fil_space_crypt_t(
				type,
				min_key_version,
				key_id,
				encrypt_mode);
	}

	return crypt_data;
}

/******************************************************************
Create a fil_space_crypt_t object
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF

@param[in]	key_id		Encryption key id
@return crypt object */
fil_space_crypt_t*
fil_space_create_crypt_data(
	fil_encryption_t	encrypt_mode,
	uint			key_id)
{
	return (fil_space_create_crypt_data(0, encrypt_mode, 0, key_id));
}

/******************************************************************
Merge fil_space_crypt_t object
@param[in,out]	dst		Destination cryp data
@param[in]	src		Source crypt data */
static
void
fil_space_merge_crypt_data(
	fil_space_crypt_t* dst,
	const fil_space_crypt_t* src)
{
	mysql_mutex_lock(&dst->mutex);

	/* validate that they are mergeable */
	ut_a(src->type == CRYPT_SCHEME_UNENCRYPTED ||
	     src->type == CRYPT_SCHEME_1);

	ut_a(dst->type == CRYPT_SCHEME_UNENCRYPTED ||
	     dst->type == CRYPT_SCHEME_1);

	dst->encryption = src->encryption;
	dst->type = src->type;
	dst->min_key_version = src->min_key_version;
	dst->keyserver_requests += src->keyserver_requests;

	mysql_mutex_unlock(&dst->mutex);
}

/** Initialize encryption parameters from a tablespace header page.
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	page		first page of the tablespace
@return crypt data from page 0
@retval	NULL	if not present or not valid */
fil_space_crypt_t* fil_space_read_crypt_data(ulint zip_size, const byte* page)
{
	const ulint offset = FSP_HEADER_OFFSET
		+ fsp_header_get_encryption_offset(zip_size);

	if (memcmp(page + offset, CRYPT_MAGIC, MAGIC_SZ) != 0) {
		/* Crypt data is not stored. */
		return NULL;
	}

	uint8_t type = mach_read_from_1(page + offset + MAGIC_SZ + 0);
	uint8_t iv_length = mach_read_from_1(page + offset + MAGIC_SZ + 1);
	fil_space_crypt_t* crypt_data;

	if (!(type == CRYPT_SCHEME_UNENCRYPTED ||
	      type == CRYPT_SCHEME_1)
	    || iv_length != sizeof crypt_data->iv) {
		ib::error() << "Found non sensible crypt scheme: "
			    << type << "," << iv_length
			    << " for space: "
			    << page_get_space_id(page);
		return NULL;
	}

	uint min_key_version = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length);

	uint key_id = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length + 4);

	fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(
		page + offset + MAGIC_SZ + 2 + iv_length + 8);

	crypt_data = fil_space_create_crypt_data(encryption, key_id);
	/* We need to overwrite these as above function will initialize
	members */
	crypt_data->type = type;
	crypt_data->min_key_version = min_key_version;
	memcpy(crypt_data->iv, page + offset + MAGIC_SZ + 2, iv_length);

	return crypt_data;
}

/******************************************************************
Free a crypt data object
@param[in,out] crypt_data	crypt data to be freed */
void fil_space_destroy_crypt_data(fil_space_crypt_t **crypt_data)
{
	if (crypt_data != NULL && (*crypt_data) != NULL) {
		fil_space_crypt_t* c;
		if (UNIV_LIKELY(fil_crypt_threads_inited)) {
			mysql_mutex_lock(&fil_crypt_threads_mutex);
			c = *crypt_data;
			*crypt_data = NULL;
			mysql_mutex_unlock(&fil_crypt_threads_mutex);
		} else {
			ut_ad(srv_read_only_mode || !srv_was_started);
			c = *crypt_data;
			*crypt_data = NULL;
		}
		if (c) {
			c->~fil_space_crypt_t();
			ut_free(c);
		}
	}
}

/** Amend encryption information from redo log.
@param[in]	space	tablespace
@param[in]	data	encryption metadata */
void fil_crypt_parse(fil_space_t* space, const byte* data)
{
	ut_ad(data[1] == MY_AES_BLOCK_SIZE);
	if (void* buf = ut_zalloc_nokey(sizeof(fil_space_crypt_t))) {
		fil_space_crypt_t* crypt_data = new(buf)
			fil_space_crypt_t(
				data[0],
				mach_read_from_4(&data[2 + MY_AES_BLOCK_SIZE]),
				mach_read_from_4(&data[6 + MY_AES_BLOCK_SIZE]),
				static_cast<fil_encryption_t>
				(data[10 + MY_AES_BLOCK_SIZE]));
		memcpy(crypt_data->iv, data + 2, MY_AES_BLOCK_SIZE);
		mysql_mutex_lock(&fil_system.mutex);
		if (space->crypt_data) {
			fil_space_merge_crypt_data(space->crypt_data,
						   crypt_data);
			fil_space_destroy_crypt_data(&crypt_data);
			crypt_data = space->crypt_data;
		} else {
			space->crypt_data = crypt_data;
		}
		mysql_mutex_unlock(&fil_system.mutex);
	}
}

/** Write encryption metadata to the first page.
@param[in,out]	block	first page of the tablespace
@param[in,out]	mtr	mini-transaction */
void fil_space_crypt_t::write_page0(buf_block_t* block, mtr_t* mtr)
{
	const ulint offset = FSP_HEADER_OFFSET
		+ fsp_header_get_encryption_offset(block->zip_size());
	byte* b = block->page.frame + offset;

	mtr->memcpy<mtr_t::MAYBE_NOP>(*block, b, CRYPT_MAGIC, MAGIC_SZ);

	b += MAGIC_SZ;
	byte* const start = b;
	*b++ = static_cast<byte>(type);
	compile_time_assert(sizeof iv == MY_AES_BLOCK_SIZE);
	compile_time_assert(sizeof iv == CRYPT_SCHEME_1_IV_LEN);
	*b++ = sizeof iv;
	memcpy(b, iv, sizeof iv);
	b += sizeof iv;
	mach_write_to_4(b, min_key_version);
	b += 4;
	mach_write_to_4(b, key_id);
	b += 4;
	*b++ = byte(encryption);
	ut_ad(b - start == 11 + MY_AES_BLOCK_SIZE);
	/* We must log also any unchanged bytes, because recovery will
	invoke fil_crypt_parse() based on this log record. */
	mtr->memcpy(*block, offset + MAGIC_SZ, b - start);
}

/** Encrypt a buffer for non full checksum.
@param[in,out]		crypt_data		Crypt data
@param[in]		space			space_id
@param[in]		offset			Page offset
@param[in]		lsn			Log sequence number
@param[in]		src_frame		Page to encrypt
@param[in]		zip_size		ROW_FORMAT=COMPRESSED
						page size, or 0
@param[in,out]		dst_frame		Output buffer
@return encrypted buffer or NULL */
static byte* fil_encrypt_buf_for_non_full_checksum(
	fil_space_crypt_t*	crypt_data,
	ulint			space,
	ulint			offset,
	lsn_t			lsn,
	const byte*		src_frame,
	ulint			zip_size,
	byte*			dst_frame)
{
	uint size = uint(zip_size ? zip_size : srv_page_size);
	uint key_version = fil_crypt_get_latest_key_version(crypt_data);
	ut_a(key_version != ENCRYPTION_KEY_VERSION_INVALID);
	ut_ad(!ut_align_offset(src_frame, 8));
	ut_ad(!ut_align_offset(dst_frame, 8));

	const bool page_compressed = fil_page_get_type(src_frame)
		== FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED;
	uint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += FIL_PAGE_ENCRYPT_COMP_METADATA_LEN;
	}

	/* FIL page header is not encrypted */
	memcpy(dst_frame, src_frame, header_len);
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
			key_version);

	/* Calculate the start offset in a page */
	uint		unencrypted_bytes = header_len + FIL_PAGE_DATA_END;
	uint		srclen = size - unencrypted_bytes;
	const byte*	src = src_frame + header_len;
	byte*		dst = dst_frame + header_len;
	uint32		dstlen = 0;

	if (page_compressed) {
		srclen = mach_read_from_2(src_frame + FIL_PAGE_DATA);
	}

	int rc = encryption_scheme_encrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   (uint32)space, (uint32)offset, lsn);
	ut_a(rc == MY_AES_OK);
	ut_a(dstlen == srclen);

	/* For compressed tables we do not store the FIL header because
	the whole page is not stored to the disk. In compressed tables only
	the FIL header + compressed (and now encrypted) payload alligned
	to sector boundary is written. */
	if (!page_compressed) {
		/* FIL page trailer is also not encrypted */
		static_assert(FIL_PAGE_DATA_END == 8, "alignment");
		memcpy_aligned<8>(dst_frame + size - FIL_PAGE_DATA_END,
				  src_frame + size - FIL_PAGE_DATA_END, 8);
	} else {
		/* Clean up rest of buffer */
		memset(dst_frame+header_len+srclen, 0,
		       size - (header_len + srclen));
	}

	/* store the post-encryption checksum after the key-version */
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4,
			zip_size
			? page_zip_calc_checksum(dst_frame, zip_size,
						 SRV_CHECKSUM_ALGORITHM_CRC32)
			: buf_calc_page_crc32(dst_frame));

	ut_ad(fil_space_verify_crypt_checksum(dst_frame, zip_size));

	srv_stats.pages_encrypted.inc();

	return dst_frame;
}

/** Encrypt a buffer for full checksum format.
@param[in,out]		crypt_data		Crypt data
@param[in]		space			space_id
@param[in]		offset			Page offset
@param[in]		lsn			Log sequence number
@param[in]		src_frame		Page to encrypt
@param[in,out]		dst_frame		Output buffer
@return encrypted buffer or NULL */
static byte* fil_encrypt_buf_for_full_crc32(
	fil_space_crypt_t*	crypt_data,
	ulint			space,
	ulint			offset,
	lsn_t			lsn,
	const byte*		src_frame,
	byte*			dst_frame)
{
	uint key_version = fil_crypt_get_latest_key_version(crypt_data);
	ut_d(bool corrupted = false);
	const uint size = buf_page_full_crc32_size(src_frame, NULL,
#ifdef UNIV_DEBUG
						   &corrupted
#else
						   NULL
#endif
						   );
	ut_ad(!corrupted);
	uint srclen = size - (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
			      + FIL_PAGE_FCRC32_CHECKSUM);
	const byte* src = src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
	byte* dst = dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
	uint dstlen = 0;

	ut_a(key_version != ENCRYPTION_KEY_VERSION_INVALID);

	/* Till FIL_PAGE_LSN, page is not encrypted */
	memcpy(dst_frame, src_frame, FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	/* Write key version to the page. */
	mach_write_to_4(dst_frame + FIL_PAGE_FCRC32_KEY_VERSION, key_version);

	int rc = encryption_scheme_encrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   uint(space), uint(offset), lsn);
	ut_a(rc == MY_AES_OK);
	ut_a(dstlen == srclen);

	const ulint payload = size - FIL_PAGE_FCRC32_CHECKSUM;
	mach_write_to_4(dst_frame + payload, my_crc32c(0, dst_frame, payload));
	/* Clean the rest of the buffer. FIXME: Punch holes when writing! */
	memset(dst_frame + (payload + 4), 0, srv_page_size - (payload + 4));

	srv_stats.pages_encrypted.inc();

	return dst_frame;
}

/** Encrypt a buffer.
@param[in,out]		crypt_data		Crypt data
@param[in]		space			space_id
@param[in]		offset			Page offset
@param[in]		src_frame		Page to encrypt
@param[in]		zip_size		ROW_FORMAT=COMPRESSED
						page size, or 0
@param[in,out]		dst_frame		Output buffer
@param[in]		use_full_checksum	full crc32 algo is used
@return encrypted buffer or NULL */
byte* fil_encrypt_buf(
	fil_space_crypt_t*	crypt_data,
	ulint			space,
	ulint			offset,
	const byte*		src_frame,
	ulint			zip_size,
	byte*			dst_frame,
	bool			use_full_checksum)
{
	const lsn_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);

	if (use_full_checksum) {
		ut_ad(!zip_size);
		return fil_encrypt_buf_for_full_crc32(
			crypt_data, space, offset,
			lsn, src_frame, dst_frame);
	}

	return fil_encrypt_buf_for_non_full_checksum(
		crypt_data, space, offset, lsn,
		src_frame, zip_size, dst_frame);
}

/** Check whether these page types are allowed to encrypt.
@param[in]	space		tablespace object
@param[in]	src_frame	source page
@return true if it is valid page type */
static bool fil_space_encrypt_valid_page_type(
	const fil_space_t*	space,
	const byte*		src_frame)
{
	switch (fil_page_get_type(src_frame)) {
	case FIL_PAGE_RTREE:
		return space->full_crc32();
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
		return false;
	}

	return true;
}

/******************************************************************
Encrypt a page

@param[in]		space		Tablespace
@param[in]		offset		Page offset
@param[in]		src_frame	Page to encrypt
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
byte* fil_space_encrypt(
	const fil_space_t*	space,
	ulint			offset,
	byte*			src_frame,
	byte*			dst_frame)
{
	if (!fil_space_encrypt_valid_page_type(space, src_frame)) {
		return src_frame;
	}

	if (!space->crypt_data || !space->crypt_data->is_encrypted()) {
		return (src_frame);
	}

	ut_ad(space->referenced());

	return fil_encrypt_buf(space->crypt_data, space->id, offset,
			       src_frame, space->zip_size(),
			       dst_frame, space->full_crc32());
}

/** Decrypt a page for full checksum format.
@param[in]	space			space id
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in,out]	src_frame		Page to decrypt
@return DB_SUCCESS or error */
static dberr_t fil_space_decrypt_full_crc32(
	ulint			space,
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	byte*			src_frame)
{
	uint key_version = mach_read_from_4(
		src_frame + FIL_PAGE_FCRC32_KEY_VERSION);
	lsn_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);
	uint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);

	ut_a(key_version != ENCRYPTION_KEY_NOT_ENCRYPTED);

	ut_ad(crypt_data);
	ut_ad(crypt_data->is_encrypted());

	memcpy(tmp_frame, src_frame, FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
	byte* dst = tmp_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
	uint dstlen = 0;
	bool corrupted = false;
	uint size = buf_page_full_crc32_size(src_frame, NULL, &corrupted);
	if (UNIV_UNLIKELY(corrupted)) {
		return DB_DECRYPTION_FAILED;
	}

	uint srclen = size - (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
			      + FIL_PAGE_FCRC32_CHECKSUM);

	int rc = encryption_scheme_decrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   (uint) space, offset, lsn);

	if (rc != MY_AES_OK || dstlen != srclen) {
		if (rc == -1) {
			return DB_DECRYPTION_FAILED;
		}

		ib::fatal() << "Unable to decrypt data-block "
			    << " src: " << src << "srclen: "
			    << srclen << " buf: " << dst << "buflen: "
			    << dstlen << " return-code: " << rc
			    << " Can't continue!";
	}

	/* Copy only checksum part in the trailer */
	memcpy(tmp_frame + srv_page_size - FIL_PAGE_FCRC32_CHECKSUM,
	       src_frame + srv_page_size - FIL_PAGE_FCRC32_CHECKSUM,
	       FIL_PAGE_FCRC32_CHECKSUM);

	srv_stats.pages_decrypted.inc();

	return DB_SUCCESS; /* page was decrypted */
}

/** Decrypt a page for non full checksum format.
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	physical_size		page size
@param[in,out]	src_frame		Page to decrypt
@return DB_SUCCESS or error */
static dberr_t fil_space_decrypt_for_non_full_checksum(
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	ulint			physical_size,
	byte*			src_frame)
{
	uint key_version = mach_read_from_4(
			src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	bool page_compressed = (fil_page_get_type(src_frame)
				== FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
	uint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);
	uint space = mach_read_from_4(
			src_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	ib_uint64_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);

	ut_a(key_version != ENCRYPTION_KEY_NOT_ENCRYPTED);
	ut_a(crypt_data != NULL && crypt_data->is_encrypted());

	/* read space & lsn */
	uint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += FIL_PAGE_ENCRYPT_COMP_METADATA_LEN;
	}

	/* Copy FIL page header, it is not encrypted */
	memcpy(tmp_frame, src_frame, header_len);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + header_len;
	byte* dst = tmp_frame + header_len;
	uint32 dstlen = 0;
	uint srclen = uint(physical_size) - header_len - FIL_PAGE_DATA_END;

	if (page_compressed) {
		srclen = mach_read_from_2(src_frame + FIL_PAGE_DATA);
	}

	int rc = encryption_scheme_decrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   space, offset, lsn);

	if (! ((rc == MY_AES_OK) && ((ulint) dstlen == srclen))) {

		if (rc == -1) {
			return DB_DECRYPTION_FAILED;
		}

		ib::fatal() << "Unable to decrypt data-block "
			    << " src: " << static_cast<const void*>(src)
			    << "srclen: "
			    << srclen << " buf: "
			    << static_cast<const void*>(dst) << "buflen: "
			    << dstlen << " return-code: " << rc
			    << " Can't continue!";
	}

	/* For compressed tables we do not store the FIL header because
	the whole page is not stored to the disk. In compressed tables only
	the FIL header + compressed (and now encrypted) payload alligned
	to sector boundary is written. */
	if (!page_compressed) {
		/* Copy FIL trailer */
		memcpy(tmp_frame + physical_size - FIL_PAGE_DATA_END,
		       src_frame + physical_size - FIL_PAGE_DATA_END,
		       FIL_PAGE_DATA_END);
	}

	srv_stats.pages_decrypted.inc();

	return DB_SUCCESS; /* page was decrypted */
}

/** Decrypt a page.
@param[in]	space_id		tablespace id
@param[in]	fsp_flags		Tablespace flags
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	physical_size		page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	err			DB_SUCCESS or DB_DECRYPTION_FAILED
@return DB_SUCCESS or error */
dberr_t
fil_space_decrypt(
	uint32_t		space_id,
	uint32_t		fsp_flags,
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	ulint			physical_size,
	byte*			src_frame)
{
	if (fil_space_t::full_crc32(fsp_flags)) {
		return fil_space_decrypt_full_crc32(
			space_id, crypt_data, tmp_frame, src_frame);
	}

	return fil_space_decrypt_for_non_full_checksum(crypt_data, tmp_frame,
						       physical_size,
						       src_frame);
}

/**
Decrypt a page.
@param[in]	space			Tablespace
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in,out]	src_frame		Page to decrypt
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
byte*
fil_space_decrypt(
	const fil_space_t* space,
	byte*		tmp_frame,
	byte*		src_frame)
{
	const ulint physical_size = space->physical_size();

	ut_ad(space->crypt_data != NULL && space->crypt_data->is_encrypted());
	ut_ad(space->referenced());

	if (DB_SUCCESS != fil_space_decrypt(space->id, space->flags,
					    space->crypt_data,
					    tmp_frame, physical_size,
					    src_frame)) {
		return nullptr;
	}

	/* Copy the decrypted page back to page buffer, not
	really any other options. */
	memcpy(src_frame, tmp_frame, physical_size);

	return src_frame;
}

/***********************************************************************/

/** A copy of global key state */
struct key_state_t {
	key_state_t() : key_id(0), key_version(0),
			rotate_key_age(srv_fil_crypt_rotate_key_age) {}
	bool operator==(const key_state_t& other) const {
		return key_version == other.key_version &&
			rotate_key_age == other.rotate_key_age;
	}
	uint key_id;
	uint key_version;
	uint rotate_key_age;
};

/***********************************************************************
Copy global key state
@param[in,out]	new_state	key state
@param[in]	crypt_data	crypt data */
static void
fil_crypt_get_key_state(
	key_state_t*			new_state,
	fil_space_crypt_t*		crypt_data)
{
	if (srv_encrypt_tables) {
		new_state->key_version = crypt_data->key_get_latest_version();
		new_state->rotate_key_age = srv_fil_crypt_rotate_key_age;

		ut_a(new_state->key_version != ENCRYPTION_KEY_NOT_ENCRYPTED);
	} else {
		new_state->key_version = 0;
		new_state->rotate_key_age = 0;
	}
}

/***********************************************************************
Check if a key needs rotation given a key_state
@param[in]	crypt_data		Encryption information
@param[in]	key_version		Current key version
@param[in]	latest_key_version	Latest key version
@param[in]	rotate_key_age		when to rotate
@return true if key needs rotation, false if not */
static bool
fil_crypt_needs_rotation(
	const fil_space_crypt_t*	crypt_data,
	uint				key_version,
	uint				latest_key_version,
	uint				rotate_key_age)
{
	if (key_version == ENCRYPTION_KEY_VERSION_INVALID) {
		return false;
	}

	if (key_version == 0 && latest_key_version != 0) {
		/* this is rotation unencrypted => encrypted
		* ignore rotate_key_age */
		return true;
	}

	if (latest_key_version == 0 && key_version != 0) {
		if (crypt_data->encryption == FIL_ENCRYPTION_DEFAULT) {
			/* this is rotation encrypted => unencrypted */
			return true;
		}
		return false;
	}

	if (crypt_data->encryption == FIL_ENCRYPTION_DEFAULT
	    && crypt_data->type == CRYPT_SCHEME_1
	    && !srv_encrypt_tables) {
		/* This is rotation encrypted => unencrypted */
		return true;
	}

	if (rotate_key_age == 0) {
		return false;
	}

	/* this is rotation encrypted => encrypted,
	* only reencrypt if key is sufficiently old */
	if (key_version + rotate_key_age < latest_key_version) {
		return true;
	}

	return false;
}

/** Read page 0 and possible crypt data from there.
@param[in,out]	space		Tablespace */
static inline
void
fil_crypt_read_crypt_data(fil_space_t* space)
{
	if (space->crypt_data || space->size || !space->get_size()) {
		/* The encryption metadata has already been read, or
		the tablespace is not encrypted and the file has been
		opened already, or the file cannot be accessed,
		likely due to a concurrent DROP
		(possibly as part of TRUNCATE or ALTER TABLE).
		FIXME: The file can become unaccessible any time
		after this check! We should really remove this
		function and instead make crypt_data an integral
		part of fil_space_t. */
		return;
	}

	const ulint zip_size = space->zip_size();
	mtr_t	mtr;
	mtr.start();
	if (buf_block_t* block = buf_page_get_gen(page_id_t(space->id, 0),
						  zip_size, RW_S_LATCH,
						  nullptr,
						  BUF_GET_POSSIBLY_FREED,
						  &mtr)) {
		if (block->page.is_freed()) {
			goto func_exit;
		}
		mysql_mutex_lock(&fil_system.mutex);
		if (!space->crypt_data && !space->is_stopping()) {
			space->crypt_data = fil_space_read_crypt_data(
				zip_size, block->page.frame);
		}
		mysql_mutex_unlock(&fil_system.mutex);
	}
func_exit:
	mtr.commit();
}

/** Start encrypting a space
@param[in,out]		space		Tablespace
@return true if a recheck of tablespace is needed by encryption thread. */
static bool fil_crypt_start_encrypting_space(fil_space_t* space)
{
	mysql_mutex_lock(&fil_crypt_threads_mutex);

	fil_space_crypt_t *crypt_data = space->crypt_data;

	/* If space is not encrypted and encryption is not enabled, then
	do not continue encrypting the space. */
	if (!crypt_data && !srv_encrypt_tables) {
func_exit:
		mysql_mutex_unlock(&fil_crypt_threads_mutex);
		return false;
	}

	const bool recheck = fil_crypt_start_converting;

	if (recheck || crypt_data || space->is_stopping()) {
		mysql_mutex_unlock(&fil_crypt_threads_mutex);
		return recheck;
	}

	/* NOTE: we need to write and flush page 0 before publishing
	* the crypt data. This so that after restart there is no
	* risk of finding encrypted pages without having
	* crypt data in page 0 */

	/* 1 - create crypt data */
	crypt_data = fil_space_create_crypt_data(
		FIL_ENCRYPTION_DEFAULT, FIL_DEFAULT_ENCRYPTION_KEY);

	if (!crypt_data) {
		goto func_exit;
	}

	fil_crypt_start_converting = true;
	mysql_mutex_unlock(&fil_crypt_threads_mutex);

	mtr_t mtr;
	mtr.start();

	/* 2 - get page 0 */
	dberr_t err = DB_SUCCESS;
	if (buf_block_t* block = buf_page_get_gen(
		    page_id_t(space->id, 0), space->zip_size(),
		    RW_X_LATCH, NULL, BUF_GET_POSSIBLY_FREED,
		    &mtr, &err)) {
		if (block->page.is_freed()) {
			goto abort;
		}

		crypt_data->type = CRYPT_SCHEME_1;
		crypt_data->min_key_version = 0; // all pages are unencrypted
		crypt_data->rotate_state.start_time = time(0);
		crypt_data->rotate_state.starting = true;
		crypt_data->rotate_state.active_threads = 1;

		mysql_mutex_lock(&fil_system.mutex);
		const bool stopping = space->is_stopping();
		if (!stopping) {
			space->crypt_data = crypt_data;
		}
		mysql_mutex_unlock(&fil_system.mutex);

		if (stopping) {
			goto abort;
		}

		/* 3 - write crypt data to page 0 */
		mtr.set_named_space(space);
		crypt_data->write_page0(block, &mtr);

		mtr.commit();

		/* 4 - sync tablespace before publishing crypt data */
		while (buf_flush_list_space(space));

		/* 5 - publish crypt data */
		mysql_mutex_lock(&fil_crypt_threads_mutex);
		mysql_mutex_lock(&crypt_data->mutex);
		crypt_data->type = CRYPT_SCHEME_1;
		ut_a(crypt_data->rotate_state.active_threads == 1);
		crypt_data->rotate_state.active_threads = 0;
		crypt_data->rotate_state.starting = false;

		fil_crypt_start_converting = false;
		mysql_mutex_unlock(&fil_crypt_threads_mutex);
		mysql_mutex_unlock(&crypt_data->mutex);

		return false;
	}

abort:
	mtr.commit();
	mysql_mutex_lock(&fil_crypt_threads_mutex);
	fil_crypt_start_converting = false;
	mysql_mutex_unlock(&fil_crypt_threads_mutex);

	crypt_data->~fil_space_crypt_t();
	ut_free(crypt_data);
	return false;
}

/** State of a rotation thread */
struct rotate_thread_t {
  explicit rotate_thread_t(uint no) : thread_no(no) {}

  uint thread_no;
  bool first = true;              /*!< is position before first space */
  space_list_t::iterator space
    = fil_system.space_list.end();/*!< current space or .end() */
  uint32_t offset = 0;            /*!< current page number */
  ulint batch = 0;                /*!< #pages to rotate */
  uint min_key_version_found = 0; /*!< min key version found but not rotated */
  lsn_t end_lsn = 0;              /*!< max lsn when rotating this space */

  uint estimated_max_iops = 20;/*!< estimation of max iops */
  uint allocated_iops = 0;     /*!< allocated iops */
  ulint cnt_waited = 0;	       /*!< #times waited during this slot */
  uintmax_t sum_waited_us = 0; /*!< wait time during this slot */

	fil_crypt_stat_t crypt_stat; // statistics

	/** @return whether this thread should terminate */
	bool should_shutdown() const {
		mysql_mutex_assert_owner(&fil_crypt_threads_mutex);
		switch (srv_shutdown_state) {
		case SRV_SHUTDOWN_NONE:
			return thread_no >= srv_n_fil_crypt_threads;
		case SRV_SHUTDOWN_EXIT_THREADS:
			/* srv_init_abort() must have been invoked */
		case SRV_SHUTDOWN_CLEANUP:
		case SRV_SHUTDOWN_INITIATED:
			return true;
		case SRV_SHUTDOWN_LAST_PHASE:
			break;
		}
		ut_ad(0);
		return true;
	}
};

/** Avoid the removal of the tablespace from
default_encrypt_list only when
1) Another active encryption thread working on tablespace
2) Eligible for tablespace key rotation
3) Tablespace is in flushing phase
@return true if tablespace should be removed from
default encrypt */
static bool fil_crypt_must_remove(const fil_space_t &space)
{
  ut_ad(space.purpose == FIL_TYPE_TABLESPACE);
  fil_space_crypt_t *crypt_data = space.crypt_data;
  mysql_mutex_assert_owner(&fil_system.mutex);
  const ulong encrypt_tables= srv_encrypt_tables;
  if (!crypt_data)
    return !encrypt_tables;
  if (!crypt_data->is_key_found())
    return true;

  mysql_mutex_lock(&crypt_data->mutex);
  const bool remove= (space.is_stopping() || crypt_data->not_encrypted()) &&
    (!crypt_data->rotate_state.flushing &&
     !encrypt_tables == !!crypt_data->min_key_version &&
     !crypt_data->rotate_state.active_threads);
  mysql_mutex_unlock(&crypt_data->mutex);
  return remove;
}

/***********************************************************************
Check if space needs rotation given a key_state
@param[in,out]		state		Key rotation state
@param[in,out]		key_state	Key state
@param[in,out]		recheck		needs recheck ?
@return true if space needs key rotation */
static
bool
fil_crypt_space_needs_rotation(
	rotate_thread_t*	state,
	key_state_t*		key_state,
	bool*			recheck)
{
	mysql_mutex_assert_not_owner(&fil_crypt_threads_mutex);

	fil_space_t* space = &*state->space;

	ut_ad(space->referenced());
	ut_ad(space->purpose == FIL_TYPE_TABLESPACE);

	fil_space_crypt_t *crypt_data = space->crypt_data;

	if (crypt_data == NULL) {
		/**
		* space has no crypt data
		*   start encrypting it...
		*/
		*recheck = fil_crypt_start_encrypting_space(space);
		crypt_data = space->crypt_data;

		if (crypt_data == NULL) {
			return false;
		}

		crypt_data->key_get_latest_version();
	}

	/* If used key_id is not found from encryption plugin we can't
	continue to rotate the tablespace */
	if (!crypt_data->is_key_found()) {
		return false;
	}

	bool need_key_rotation = false;

	mysql_mutex_lock(&crypt_data->mutex);

	do {
		/* prevent threads from starting to rotate space */
		if (crypt_data->rotate_state.starting) {
			/* recheck this space later */
			*recheck = true;
			break;
		}

		/* prevent threads from starting to rotate space */
		if (space->is_stopping()) {
			break;
		}

		if (crypt_data->rotate_state.flushing) {
			break;
		}

		/* No need to rotate space if encryption is disabled */
		if (crypt_data->not_encrypted()) {
			break;
		}

		if (crypt_data->key_id != key_state->key_id) {
			key_state->key_id= crypt_data->key_id;
			fil_crypt_get_key_state(key_state, crypt_data);
		}

		need_key_rotation = fil_crypt_needs_rotation(
			crypt_data,
			crypt_data->min_key_version,
			key_state->key_version,
			key_state->rotate_key_age);
	} while (0);

	mysql_mutex_unlock(&crypt_data->mutex);
	return need_key_rotation;
}

/***********************************************************************
Update global statistics with thread statistics
@param[in,out]	state		key rotation statistics */
static void
fil_crypt_update_total_stat(
	rotate_thread_t *state)
{
	mysql_mutex_lock(&crypt_stat_mutex);
	crypt_stat.pages_read_from_cache +=
		state->crypt_stat.pages_read_from_cache;
	crypt_stat.pages_read_from_disk +=
		state->crypt_stat.pages_read_from_disk;
	crypt_stat.pages_modified += state->crypt_stat.pages_modified;
	crypt_stat.pages_flushed += state->crypt_stat.pages_flushed;
	// remote old estimate
	crypt_stat.estimated_iops -= state->crypt_stat.estimated_iops;
	// add new estimate
	crypt_stat.estimated_iops += state->estimated_max_iops;
	mysql_mutex_unlock(&crypt_stat_mutex);

	// make new estimate "current" estimate
	state->crypt_stat.pages_read_from_cache = 0;
	state->crypt_stat.pages_read_from_disk = 0;
	state->crypt_stat.pages_modified = 0;
	state->crypt_stat.pages_flushed = 0;
	// record our old (current) estimate
	state->crypt_stat.estimated_iops = state->estimated_max_iops;
}

/***********************************************************************
Allocate iops to thread from global setting,
used before starting to rotate a space.
@param[in,out]		state		Rotation state
@return true if allocation succeeded, false if failed */
static bool fil_crypt_alloc_iops(rotate_thread_t *state)
{
	mysql_mutex_assert_owner(&fil_crypt_threads_mutex);
	ut_ad(state->allocated_iops == 0);

	/* We have not yet selected the space to rotate, thus
	state might not contain space and we can't check
	its status yet. */

	uint max_iops = state->estimated_max_iops;

	if (n_fil_crypt_iops_allocated >= srv_n_fil_crypt_iops) {
wait:
		my_cond_wait(&fil_crypt_threads_cond,
			     &fil_crypt_threads_mutex.m_mutex);
		return false;
	}

	uint alloc = srv_n_fil_crypt_iops - n_fil_crypt_iops_allocated;

	if (alloc > max_iops) {
		alloc = max_iops;
	}

	if (!alloc) {
		goto wait;
	}

	n_fil_crypt_iops_allocated += alloc;

	state->allocated_iops = alloc;
	return true;
}

/**
Reallocate iops to thread when processing a tablespace
@param[in,out]		state		Rotation state
@return whether the thread should continue running */
static bool fil_crypt_realloc_iops(rotate_thread_t *state)
{
	ut_a(state->allocated_iops > 0);

	if (10 * state->cnt_waited > state->batch) {
		/* if we waited more than 10% re-estimate max_iops */
		ulint avg_wait_time_us =
			ulint(state->sum_waited_us / state->cnt_waited);

		if (avg_wait_time_us == 0) {
			avg_wait_time_us = 1; // prevent division by zero
		}

		DBUG_PRINT("ib_crypt",
			("thr_no: %u - update estimated_max_iops from %u to "
			 ULINTPF ".",
			state->thread_no,
			state->estimated_max_iops,
			1000000 / avg_wait_time_us));

		state->estimated_max_iops = std::max(
			1U, uint(1000000 / avg_wait_time_us));
		state->cnt_waited = 0;
		state->sum_waited_us = 0;
	} else {
		DBUG_PRINT("ib_crypt",
			   ("thr_no: %u only waited " ULINTPF
			    "%% skip re-estimate.",
			    state->thread_no,
			    (100 * state->cnt_waited)
			    / (state->batch ? state->batch : 1)));
	}

	ut_ad(state->estimated_max_iops);

	mysql_mutex_lock(&fil_crypt_threads_mutex);

	if (state->should_shutdown()) {
		mysql_mutex_unlock(&fil_crypt_threads_mutex);
		return false;
	}

	if (state->allocated_iops > state->estimated_max_iops) {
		/* release iops */
		uint extra = state->allocated_iops - state->estimated_max_iops;
		state->allocated_iops = state->estimated_max_iops;
		ut_ad(n_fil_crypt_iops_allocated >= extra);
		n_fil_crypt_iops_allocated -= extra;
		pthread_cond_broadcast(&fil_crypt_threads_cond);
	} else if (srv_n_fil_crypt_iops > n_fil_crypt_iops_allocated) {
		/* there are extra iops free */
		uint add = srv_n_fil_crypt_iops - n_fil_crypt_iops_allocated;
		if (state->allocated_iops + add > state->estimated_max_iops) {
			/* but don't alloc more than our max */
			add= state->estimated_max_iops - state->allocated_iops;
		}
		n_fil_crypt_iops_allocated += add;
		state->allocated_iops += add;

		DBUG_PRINT("ib_crypt",
			   ("thr_no: %u increased iops from %u to %u.",
			    state->thread_no,
			    state->allocated_iops - add,
			    state->allocated_iops));
	}

	fil_crypt_update_total_stat(state);
	mysql_mutex_unlock(&fil_crypt_threads_mutex);
	return true;
}

/** Release excess allocated iops
@param state   rotation state
@param wake    whether to wake up other threads */
static void fil_crypt_return_iops(rotate_thread_t *state, bool wake= true)
{
  mysql_mutex_assert_owner(&fil_crypt_threads_mutex);

  if (uint iops= state->allocated_iops)
  {
    ut_ad(n_fil_crypt_iops_allocated >= iops);
    n_fil_crypt_iops_allocated-= iops;
    state->allocated_iops= 0;
    if (wake)
      pthread_cond_broadcast(&fil_crypt_threads_cond);
  }

  fil_crypt_update_total_stat(state);
}

/** Acquire a tablespace reference.
@return whether a tablespace reference was successfully acquired */
inline bool fil_space_t::acquire_if_not_stopped()
{
  mysql_mutex_assert_owner(&fil_system.mutex);
  const uint32_t n= acquire_low();
  if (UNIV_LIKELY(!(n & (STOPPING | CLOSING))))
    return true;
  if (UNIV_UNLIKELY(n & STOPPING))
    return false;
  return UNIV_LIKELY(!(n & CLOSING)) || prepare(true);
}

bool fil_crypt_must_default_encrypt()
{
  return !srv_fil_crypt_rotate_key_age || !srv_encrypt_rotate;
}

/** Return the next tablespace from default_encrypt_tables list.
@param space   previous tablespace (nullptr to start from the start)
@param recheck whether the removal condition needs to be rechecked after
the encryption parameters were changed
@param encrypt expected state of innodb_encrypt_tables
@return the next tablespace to process (n_pending_ops incremented)
@retval fil_system.temp_space if there is no work to do
@retval nullptr upon reaching the end of the iteration */
inline fil_space_t *fil_system_t::default_encrypt_next(fil_space_t *space,
                                                       bool recheck,
                                                       bool encrypt)
{
  mysql_mutex_assert_owner(&mutex);

  auto it= space && space->is_in_default_encrypt
    ? sized_ilist<fil_space_t, default_encrypt_tag_t>::iterator(space)
    : default_encrypt_tables.begin();
  const auto end= default_encrypt_tables.end();

  if (space)
  {
    const bool released= !space->release();

    if (space->is_in_default_encrypt)
    {
      while (++it != end &&
             (!UT_LIST_GET_LEN(it->chain) || it->is_stopping()));

      /* If one of the encryption threads already started
      the encryption of the table then don't remove the
      unencrypted spaces from default encrypt list.

      If there is a change in innodb_encrypt_tables variables
      value then don't remove the last processed tablespace
      from the default encrypt list. */
      if (released && !recheck && fil_crypt_must_remove(*space))
      {
        ut_a(!default_encrypt_tables.empty());
        default_encrypt_tables.remove(*space);
        space->is_in_default_encrypt= false;
      }
    }
  }
  else while (it != end &&
	      (!UT_LIST_GET_LEN(it->chain) || it->is_stopping()))
  {
    /* Find the next suitable default encrypt table if
    beginning of default_encrypt_tables list has been scheduled
    to be deleted */
    it++;
  }

  if (it == end)
    return temp_space;

  do
  {
    space= &*it;
    if (space->acquire_if_not_stopped())
      return space;
    if (++it == end)
      return nullptr;
  }
  while (!UT_LIST_GET_LEN(it->chain) || it->is_stopping());

  return nullptr;
}

/** Determine the next tablespace for encryption key rotation.
@param space    current tablespace (nullptr to start from the beginning)
@param recheck  whether the removal condition needs to be rechecked after
encryption parameters were changed
@param encrypt  expected state of innodb_encrypt_tables
@return the next tablespace
@retval fil_system.temp_space if there is no work to do
@retval end() upon reaching the end of the iteration */
space_list_t::iterator fil_space_t::next(space_list_t::iterator space,
                                         bool recheck, bool encrypt)
{
  mysql_mutex_lock(&fil_system.mutex);

  if (fil_crypt_must_default_encrypt())
  {
    fil_space_t *next_space=
      fil_system.default_encrypt_next(space == fil_system.space_list.end()
				      ? nullptr : &*space, recheck, encrypt);
    space= next_space
      ? space_list_t::iterator(next_space)
      : fil_system.space_list.end();
  }
  else
  {
    if (space == fil_system.space_list.end())
      space= fil_system.space_list.begin();
    else
    {
      /* Move on to the next fil_space_t */
      space->release();
      ++space;
    }

    for (; space != fil_system.space_list.end(); ++space)
    {
      if (space->purpose != FIL_TYPE_TABLESPACE)
        continue;
      const uint32_t n= space->acquire_low();
      if (UNIV_LIKELY(!(n & (STOPPING | CLOSING))))
        break;
      if (!(n & STOPPING) && space->prepare(true))
        break;
    }
  }

  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}

/** Search for a space needing rotation
@param[in,out]	key_state	Key state
@param[in,out]	state		Rotation state
@param[in,out]	recheck		recheck of the tablespace is needed or
				still encryption thread does write page 0
@return whether the thread should keep running */
static bool fil_crypt_find_space_to_rotate(
	key_state_t*		key_state,
	rotate_thread_t*	state,
	bool*			recheck)
{
	/* we need iops to start rotating */
	do {
		if (state->should_shutdown()) {
			if (state->space != fil_system.space_list.end()) {
				state->space->release();
				state->space = fil_system.space_list.end();
			}
			return false;
		}
	} while (!fil_crypt_alloc_iops(state));

	if (state->first) {
		state->first = false;
		if (state->space != fil_system.space_list.end()) {
			state->space->release();
		}
		state->space = fil_system.space_list.end();
	}

	state->space = fil_space_t::next(state->space, *recheck,
					 key_state->key_version != 0);

	bool wake = true;
	while (state->space != fil_system.space_list.end()) {
		if (state->space
			== space_list_t::iterator(fil_system.temp_space)) {
			wake = false;
			goto done;
		}

		if (state->should_shutdown()) {
			state->space->release();
done:
			state->space = fil_system.space_list.end();
			break;
		}

		mysql_mutex_unlock(&fil_crypt_threads_mutex);
		/* If there is no crypt data and we have not yet read
		page 0 for this tablespace, we need to read it before
		we can continue. */
		if (!state->space->crypt_data) {
			fil_crypt_read_crypt_data(&*state->space);
		}

		if (fil_crypt_space_needs_rotation(state, key_state, recheck)) {
			ut_ad(key_state->key_id);
			/* init state->min_key_version_found before
			* starting on a space */
			state->min_key_version_found = key_state->key_version;
			mysql_mutex_lock(&fil_crypt_threads_mutex);
			return true;
		}

		state->space = fil_space_t::next(state->space, *recheck,
						 key_state->key_version != 0);
		mysql_mutex_lock(&fil_crypt_threads_mutex);
	}

	/* no work to do; release our allocation of I/O capacity */
	fil_crypt_return_iops(state, wake);
	return true;
}

/***********************************************************************
Start rotating a space
@param[in]	key_state		Key state
@param[in,out]	state			Rotation state */
static
void
fil_crypt_start_rotate_space(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	ut_ad(crypt_data);
	mysql_mutex_lock(&crypt_data->mutex);
	ut_ad(key_state->key_id == crypt_data->key_id);

	if (crypt_data->rotate_state.active_threads == 0) {
		/* only first thread needs to init */
		crypt_data->rotate_state.next_offset = 1; // skip page 0
		/* no need to rotate beyond current max
		* if space extends, it will be encrypted with newer version */
		/* FIXME: max_offset could be removed and instead
		space->size consulted.*/
		crypt_data->rotate_state.max_offset = state->space->size;
		crypt_data->rotate_state.end_lsn = 0;
		crypt_data->rotate_state.min_key_version_found =
			key_state->key_version;

		crypt_data->rotate_state.start_time = time(0);

		if (crypt_data->type == CRYPT_SCHEME_UNENCRYPTED &&
			crypt_data->is_encrypted() &&
			key_state->key_version != 0) {
			/* this is rotation unencrypted => encrypted */
			crypt_data->type = CRYPT_SCHEME_1;
		}
	}

	/* count active threads in space */
	crypt_data->rotate_state.active_threads++;

	/* Initialize thread local state */
	state->end_lsn = crypt_data->rotate_state.end_lsn;
	state->min_key_version_found =
		crypt_data->rotate_state.min_key_version_found;

	mysql_mutex_unlock(&crypt_data->mutex);
}

/***********************************************************************
Search for batch of pages needing rotation
@param[in]	key_state		Key state
@param[in,out]	state			Rotation state
@return true if page needing key rotation found, false if not found */
static
bool
fil_crypt_find_page_to_rotate(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	ulint batch = srv_alloc_time * state->allocated_iops;

	ut_ad(state->space == fil_system.space_list.end()
		|| state->space->referenced());

	/* If space is marked to be dropped stop rotation. */
	if (state->space == fil_system.space_list.end()
		|| state->space->is_stopping()) {
		return false;
	}

	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	mysql_mutex_lock(&crypt_data->mutex);
	ut_ad(key_state->key_id == crypt_data->key_id);

	bool found = crypt_data->rotate_state.max_offset >=
		crypt_data->rotate_state.next_offset;

	if (found) {
		state->offset = crypt_data->rotate_state.next_offset;
		ulint remaining = crypt_data->rotate_state.max_offset -
			crypt_data->rotate_state.next_offset;

		if (batch <= remaining) {
			state->batch = batch;
		} else {
			state->batch = remaining;
		}
	}

	crypt_data->rotate_state.next_offset += uint32_t(batch);
	mysql_mutex_unlock(&crypt_data->mutex);
	return found;
}

/***********************************************************************
Get a page and compute sleep time
@param[in,out]		state		Rotation state
@param[in]		offset		Page offset
@param[in,out]		mtr		Minitransaction
@param[out]		sleeptime_ms	Sleep time
@return page or NULL*/
static
buf_block_t*
fil_crypt_get_page_throttle(
	rotate_thread_t*	state,
	uint32_t		offset,
	mtr_t*			mtr,
	ulint*			sleeptime_ms)
{
	fil_space_t* space = &*state->space;
	const ulint zip_size = space->zip_size();
	const page_id_t page_id(space->id, offset);
	ut_ad(space->referenced());

	/* Before reading from tablespace we need to make sure that
	the tablespace is not about to be dropped. */
	if (space->is_stopping()) {
		return NULL;
	}

	buf_block_t* block = buf_page_get_gen(page_id, zip_size, RW_X_LATCH,
					      NULL,
					      BUF_PEEK_IF_IN_POOL, mtr);
	if (block != NULL) {
		/* page was in buffer pool */
		state->crypt_stat.pages_read_from_cache++;
		return block;
	}

	if (space->is_stopping()) {
		return NULL;
	}

	if (fseg_page_is_free(space, state->offset)) {
		/* page is already freed */
		return NULL;
	}

	state->crypt_stat.pages_read_from_disk++;

	const ulonglong start = my_interval_timer();
	block = buf_page_get_gen(page_id, zip_size,
				 RW_X_LATCH,
				 NULL, BUF_GET_POSSIBLY_FREED, mtr);
	const ulonglong end = my_interval_timer();

	state->cnt_waited++;

	if (end > start) {
		state->sum_waited_us += (end - start) / 1000;
	}

	/* average page load */
	ulint add_sleeptime_ms = 0;
	ulint avg_wait_time_us =ulint(state->sum_waited_us / state->cnt_waited);
	ulint alloc_wait_us = 1000000 / state->allocated_iops;

	if (avg_wait_time_us < alloc_wait_us) {
		/* we reading faster than we allocated */
		add_sleeptime_ms = (alloc_wait_us - avg_wait_time_us) / 1000;
	} else {
		/* if page load time is longer than we want, skip sleeping */
	}

	*sleeptime_ms += add_sleeptime_ms;

	return block;
}

/***********************************************************************
Rotate one page
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_rotate_page(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_t *space = &*state->space;
	ulint space_id = space->id;
	uint32_t offset = state->offset;
	ulint sleeptime_ms = 0;
	fil_space_crypt_t *crypt_data = space->crypt_data;

	ut_ad(space->referenced());
	ut_ad(offset > 0);

	/* In fil_crypt_thread where key rotation is done we have
	acquired space and checked that this space is not yet
	marked to be dropped. Similarly, in fil_crypt_find_page_to_rotate().
	Check here also to give DROP TABLE or similar a change. */
	if (space->is_stopping()) {
		return;
	}

	if (space_id == TRX_SYS_SPACE && offset == TRX_SYS_PAGE_NO) {
		/* don't encrypt this as it contains address to dblwr buffer */
		return;
	}

	mtr_t mtr;
	mtr.start();
	if (buf_block_t* block = fil_crypt_get_page_throttle(state,
							     offset, &mtr,
							     &sleeptime_ms)) {
		bool modified = false;
		byte* frame = buf_block_get_frame(block);
		const lsn_t block_lsn = mach_read_from_8(FIL_PAGE_LSN + frame);
		uint kv = buf_page_get_key_version(frame, space->flags);

		if (block->page.is_freed()) {
			/* Do not modify freed pages to avoid an assertion
			failure on recovery.*/
		} else if (block->page.oldest_modification() > 1) {
			/* Do not unnecessarily touch pages that are
			already dirty. */
		} else if (space->is_stopping()) {
			/* The tablespace is closing (in DROP TABLE or
			TRUNCATE TABLE or similar): avoid further access */
		} else if (!kv && !*reinterpret_cast<uint16_t*>
			   (&frame[FIL_PAGE_TYPE])) {
			/* It looks like this page is not
			allocated. Because key rotation is accessing
			pages in a pattern that is unlike the normal
			B-tree and undo log access pattern, we cannot
			invoke fseg_page_is_free() here, because that
			could result in a deadlock. If we invoked
			fseg_page_is_free() and released the
			tablespace latch before acquiring block->lock,
			then the fseg_page_is_free() information
			could be stale already. */

			/* If the data file was originally created
			before MariaDB 10.0 or MySQL 5.6, some
			allocated data pages could carry 0 in
			FIL_PAGE_TYPE. The FIL_PAGE_TYPE on those
			pages will be updated in
			buf_flush_init_for_writing() when the page
			is modified the next time.

			Also, when the doublewrite buffer pages are
			allocated on bootstrap in a non-debug build,
			some dummy pages will be allocated, with 0 in
			the FIL_PAGE_TYPE. Those pages should be
			skipped from key rotation forever. */
		} else if (fil_crypt_needs_rotation(
				crypt_data,
				kv,
				key_state->key_version,
				key_state->rotate_key_age)) {

			mtr.set_named_space(space);
			modified = true;

			/* force rotation by dummy updating page */
			mtr.write<1,mtr_t::FORCED>(*block,
						   &frame[FIL_PAGE_SPACE_ID],
						   frame[FIL_PAGE_SPACE_ID]);

			/* statistics */
			state->crypt_stat.pages_modified++;
		} else {
			if (crypt_data->is_encrypted()) {
				if (kv < state->min_key_version_found) {
					state->min_key_version_found = kv;
				}
			}
		}

		mtr.commit();
		lsn_t end_lsn = mtr.commit_lsn();


		if (modified) {
			/* if we modified page, we take lsn from mtr */
			ut_a(end_lsn > state->end_lsn);
			ut_a(end_lsn > block_lsn);
			state->end_lsn = end_lsn;
		} else {
			/* if we did not modify page, check for max lsn */
			if (block_lsn > state->end_lsn) {
				state->end_lsn = block_lsn;
			}
		}
	} else {
		/* If block read failed mtr memo and log should be empty. */
		ut_ad(!mtr.has_modifications());
		ut_ad(!mtr.is_dirty());
		ut_ad(mtr.get_memo()->size() == 0);
		ut_ad(mtr.get_log()->size() == 0);
		mtr.commit();
	}

	if (sleeptime_ms) {
		mysql_mutex_lock(&fil_crypt_threads_mutex);
		timespec abstime;
		set_timespec_nsec(abstime, 1000000ULL * sleeptime_ms);
		my_cond_timedwait(&fil_crypt_throttle_sleep_cond,
				  &fil_crypt_threads_mutex.m_mutex, &abstime);
		mysql_mutex_unlock(&fil_crypt_threads_mutex);
	}
}

/***********************************************************************
Rotate a batch of pages
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_rotate_pages(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	const uint32_t space_id = state->space->id;
	uint32_t end = std::min(state->offset + uint32_t(state->batch),
				state->space->free_limit);

	ut_ad(state->space->referenced());

	for (; state->offset < end; state->offset++) {

		/* we can't rotate pages in dblwr buffer as
		* it's not possible to read those due to lots of asserts
		* in buffer pool.
		*
		* However since these are only (short-lived) copies of
		* real pages, they will be updated anyway when the
		* real page is updated
		*/
		if (buf_dblwr.is_inside(page_id_t(space_id, state->offset))) {
			continue;
		}

		/* If space is marked as stopping, stop rotating
		pages. */
		if (state->space->is_stopping()) {
			break;
		}

		fil_crypt_rotate_page(key_state, state);
	}
}

/***********************************************************************
Flush rotated pages and then update page 0

@param[in,out]		state	rotation state */
static
void
fil_crypt_flush_space(
	rotate_thread_t*	state)
{
	fil_space_t* space = &*state->space;
	fil_space_crypt_t *crypt_data = space->crypt_data;

	ut_ad(space->referenced());

	/* flush tablespace pages so that there are no pages left with old key */
	lsn_t end_lsn = crypt_data->rotate_state.end_lsn;

	if (end_lsn > 0 && !space->is_stopping()) {
		ulint sum_pages = 0;
		const ulonglong start = my_interval_timer();
		while (buf_flush_list_space(space, &sum_pages));
		if (sum_pages) {
			const ulonglong end = my_interval_timer();

			state->cnt_waited += sum_pages;
			state->sum_waited_us += (end - start) / 1000;

			/* statistics */
			state->crypt_stat.pages_flushed += sum_pages;
		}
	}

	if (crypt_data->min_key_version == 0) {
		crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	}

	if (space->is_stopping()) {
		return;
	}

	/* update page 0 */
	mtr_t mtr;
	mtr.start();

	if (buf_block_t* block = buf_page_get_gen(
		    page_id_t(space->id, 0), space->zip_size(),
		    RW_X_LATCH, NULL, BUF_GET_POSSIBLY_FREED, &mtr)) {
		if (!block->page.is_freed()) {
			mtr.set_named_space(space);
			crypt_data->write_page0(block, &mtr);
		}
	}

	mtr.commit();
}

/***********************************************************************
Complete rotating a space
@param[in,out]		state			Rotation state */
static void fil_crypt_complete_rotate_space(rotate_thread_t* state)
{
	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	ut_ad(crypt_data);
	ut_ad(state->space->referenced());

	mysql_mutex_lock(&crypt_data->mutex);

	/* Space might already be dropped */
	if (!state->space->is_stopping()) {
		/**
		* Update crypt data state with state from thread
		*/
		if (state->min_key_version_found <
			crypt_data->rotate_state.min_key_version_found) {
			crypt_data->rotate_state.min_key_version_found =
				state->min_key_version_found;
		}

		if (state->end_lsn > crypt_data->rotate_state.end_lsn) {
			crypt_data->rotate_state.end_lsn = state->end_lsn;
		}

		ut_a(crypt_data->rotate_state.active_threads > 0);
		crypt_data->rotate_state.active_threads--;
		bool last = crypt_data->rotate_state.active_threads == 0;

		/**
		* check if space is fully done
		* this as when threads shutdown, it could be that we "complete"
		* iterating before we have scanned the full space.
		*/
		bool done = crypt_data->rotate_state.next_offset >=
			crypt_data->rotate_state.max_offset;

		/**
		* we should flush space if we're last thread AND
		* the iteration is done
		*/
		bool should_flush = last && done;

		if (should_flush) {
			/* we're the last active thread */
			crypt_data->rotate_state.flushing = true;
			crypt_data->min_key_version =
				crypt_data->rotate_state.min_key_version_found;
			mysql_mutex_unlock(&crypt_data->mutex);
			fil_crypt_flush_space(state);

			mysql_mutex_lock(&crypt_data->mutex);
			crypt_data->rotate_state.flushing = false;
		}
	} else {
		ut_a(crypt_data->rotate_state.active_threads > 0);
		crypt_data->rotate_state.active_threads--;
	}

	mysql_mutex_unlock(&crypt_data->mutex);
}

/** A thread which monitors global key state and rotates tablespaces
accordingly */
static void fil_crypt_thread()
{
	mysql_mutex_lock(&fil_crypt_threads_mutex);
	rotate_thread_t thr(srv_n_fil_crypt_threads_started++);
	pthread_cond_signal(&fil_crypt_cond); /* signal that we started */

	if (!thr.should_shutdown()) {
		/* if we find a tablespace that is starting, skip over it
		and recheck it later */
		bool recheck = false;

wait_for_work:
		if (!recheck && !thr.should_shutdown()) {
			/* wait for key state changes
			* i.e either new key version of change or
			* new rotate_key_age */
			my_cond_wait(&fil_crypt_threads_cond,
				     &fil_crypt_threads_mutex.m_mutex);
		}

		recheck = false;
		thr.first = true;      // restart from first tablespace

		key_state_t new_state;

		/* iterate all spaces searching for those needing rotation */
		while (fil_crypt_find_space_to_rotate(&new_state, &thr,
						      &recheck)) {
			if (thr.space == fil_system.space_list.end()) {
				goto wait_for_work;
			}

			/* we found a space to rotate */
			mysql_mutex_unlock(&fil_crypt_threads_mutex);
			fil_crypt_start_rotate_space(&new_state, &thr);

			/* iterate all pages (cooperativly with other threads) */
			while (fil_crypt_find_page_to_rotate(&new_state, &thr)) {

				/* If space is marked as stopping, release
				space and stop rotation. */
				if (thr.space->is_stopping()) {
					fil_crypt_complete_rotate_space(&thr);
					thr.space->release();
					thr.space = fil_system.space_list.end();
					break;
				}

				fil_crypt_rotate_pages(&new_state, &thr);
				/* realloc iops */
				if (!fil_crypt_realloc_iops(&thr)) {
					break;
				}
			}

			/* complete rotation */
			if (thr.space != fil_system.space_list.end()) {
				fil_crypt_complete_rotate_space(&thr);
			}

			/* force key state refresh */
			new_state.key_id = 0;

			mysql_mutex_lock(&fil_crypt_threads_mutex);
			/* release iops */
			fil_crypt_return_iops(&thr);
		}

		if (thr.space != fil_system.space_list.end()) {
			thr.space->release();
			thr.space = fil_system.space_list.end();
		}
	}

	fil_crypt_return_iops(&thr);
	srv_n_fil_crypt_threads_started--;
	pthread_cond_signal(&fil_crypt_cond); /* signal that we stopped */
	mysql_mutex_unlock(&fil_crypt_threads_mutex);

#ifdef UNIV_PFS_THREAD
	pfs_delete_thread();
#endif
}

/*********************************************************************
Adjust thread count for key rotation
@param[in]	enw_cnt		Number of threads to be used */
void fil_crypt_set_thread_cnt(const uint new_cnt)
{
	if (!fil_crypt_threads_inited) {
		if (srv_shutdown_state != SRV_SHUTDOWN_NONE)
			return;
		fil_crypt_threads_init();
	}

	mysql_mutex_lock(&fil_crypt_threads_mutex);

	if (new_cnt > srv_n_fil_crypt_threads) {
		uint add = new_cnt - srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = new_cnt;
		for (uint i = 0; i < add; i++) {
			std::thread thd(fil_crypt_thread);
			ib::info() << "Creating #"
				   << i+1 << " encryption thread id "
				   << thd.get_id()
				   << " total threads " << new_cnt << ".";
			thd.detach();
		}
	} else if (new_cnt < srv_n_fil_crypt_threads) {
		srv_n_fil_crypt_threads = new_cnt;
	}

	pthread_cond_broadcast(&fil_crypt_threads_cond);

	while (srv_n_fil_crypt_threads_started != srv_n_fil_crypt_threads) {
		my_cond_wait(&fil_crypt_cond,
			     &fil_crypt_threads_mutex.m_mutex);
	}

	pthread_cond_broadcast(&fil_crypt_threads_cond);
	mysql_mutex_unlock(&fil_crypt_threads_mutex);
}

/** Initialize the tablespace default_encrypt_tables
if innodb_encryption_rotate_key_age=0. */
static void fil_crypt_default_encrypt_tables_fill()
{
	mysql_mutex_assert_owner(&fil_system.mutex);

	for (fil_space_t& space : fil_system.space_list) {
		if (space.purpose != FIL_TYPE_TABLESPACE
		    || space.is_in_default_encrypt
		    || UT_LIST_GET_LEN(space.chain) == 0
		    || !space.acquire_if_not_stopped()) {
			continue;
		}

		/* Ensure that crypt_data has been initialized. */
		ut_ad(space.size);

		/* Skip ENCRYPTION!=DEFAULT tablespaces. */
		if (space.crypt_data
		    && !space.crypt_data->is_default_encryption()) {
			goto next;
		}

		if (srv_encrypt_tables) {
			/* Skip encrypted tablespaces if
			innodb_encrypt_tables!=OFF */
			if (space.crypt_data
			    && space.crypt_data->min_key_version) {
				goto next;
			}
		} else {
			/* Skip unencrypted tablespaces if
			innodb_encrypt_tables=OFF */
			if (!space.crypt_data
			    || !space.crypt_data->min_key_version) {
				goto next;
			}
		}

		fil_system.default_encrypt_tables.push_back(space);
		space.is_in_default_encrypt = true;
next:
		space.release();
	}
}

/*********************************************************************
Adjust max key age
@param[in]	val		New max key age */
void fil_crypt_set_rotate_key_age(uint val)
{
  mysql_mutex_lock(&fil_crypt_threads_mutex);
  mysql_mutex_lock(&fil_system.mutex);
  srv_fil_crypt_rotate_key_age= val;
  if (val == 0)
    fil_crypt_default_encrypt_tables_fill();
  mysql_mutex_unlock(&fil_system.mutex);
  pthread_cond_broadcast(&fil_crypt_threads_cond);
  mysql_mutex_unlock(&fil_crypt_threads_mutex);
}

/*********************************************************************
Adjust rotation iops
@param[in]	val		New max roation iops */
void fil_crypt_set_rotation_iops(uint val)
{
  mysql_mutex_lock(&fil_crypt_threads_mutex);
  srv_n_fil_crypt_iops= val;
  pthread_cond_broadcast(&fil_crypt_threads_cond);
  mysql_mutex_unlock(&fil_crypt_threads_mutex);
}

/*********************************************************************
Adjust encrypt tables
@param[in]	val		New setting for innodb-encrypt-tables */
void fil_crypt_set_encrypt_tables(ulong val)
{
  if (!fil_crypt_threads_inited)
    return;

  mysql_mutex_lock(&fil_crypt_threads_mutex);

  mysql_mutex_lock(&fil_system.mutex);
  srv_encrypt_tables= val;

  if (fil_crypt_must_default_encrypt())
    fil_crypt_default_encrypt_tables_fill();

  mysql_mutex_unlock(&fil_system.mutex);

  pthread_cond_broadcast(&fil_crypt_threads_cond);
  mysql_mutex_unlock(&fil_crypt_threads_mutex);
}

/*********************************************************************
Init threads for key rotation */
void fil_crypt_threads_init()
{
	if (!fil_crypt_threads_inited) {
		pthread_cond_init(&fil_crypt_cond, nullptr);
		pthread_cond_init(&fil_crypt_threads_cond, nullptr);
		mysql_mutex_init(0, &fil_crypt_threads_mutex, nullptr);
		uint cnt = srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = 0;
		fil_crypt_threads_inited = true;
		fil_crypt_set_thread_cnt(cnt);
	}
}

/*********************************************************************
Clean up key rotation threads resources */
void fil_crypt_threads_cleanup()
{
	if (!fil_crypt_threads_inited) {
		return;
	}
	ut_a(!srv_n_fil_crypt_threads_started);
	pthread_cond_destroy(&fil_crypt_cond);
	pthread_cond_destroy(&fil_crypt_threads_cond);
	mysql_mutex_destroy(&fil_crypt_threads_mutex);
	fil_crypt_threads_inited = false;
}

/*********************************************************************
Wait for crypt threads to stop accessing space
@param[in]	space		Tablespace */
void fil_space_crypt_close_tablespace(const fil_space_t *space)
{
	fil_space_crypt_t* crypt_data = space->crypt_data;

	if (!crypt_data || srv_n_fil_crypt_threads == 0
	    || !fil_crypt_threads_inited) {
		return;
	}

	time_t start = time(0);
	time_t last = start;

	mysql_mutex_lock(&crypt_data->mutex);

	while (crypt_data->rotate_state.active_threads
	       || crypt_data->rotate_state.flushing) {
		mysql_mutex_unlock(&crypt_data->mutex);

		/* wakeup throttle (all) sleepers */
		mysql_mutex_lock(&fil_crypt_threads_mutex);
		pthread_cond_broadcast(&fil_crypt_throttle_sleep_cond);
		pthread_cond_broadcast(&fil_crypt_threads_cond);
		mysql_mutex_unlock(&fil_crypt_threads_mutex);

		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		time_t now = time(0);

		if (UNIV_UNLIKELY(now >= last + 30)) {
			ib::warn() << "Waited "
				   << now - start
				   << " seconds to drop space: "
				   << space->chain.start->name << " ("
				   << space->id << ") active threads "
				   << crypt_data->rotate_state.active_threads
				   << "flushing="
				   << crypt_data->rotate_state.flushing << ".";
			last = now;
		}

		mysql_mutex_lock(&crypt_data->mutex);
	}

	mysql_mutex_unlock(&crypt_data->mutex);
}

/*********************************************************************
Get crypt status for a space (used by information_schema)
@param[in]	space		Tablespace
@param[out]	status		Crypt status */
void
fil_space_crypt_get_status(
	const fil_space_t*			space,
	struct fil_space_crypt_status_t*	status)
{
	memset(status, 0, sizeof(*status));

	ut_ad(space->referenced());

	/* If there is no crypt data and we have not yet read
	page 0 for this tablespace, we need to read it before
	we can continue. */
	if (!space->crypt_data) {
		fil_crypt_read_crypt_data(const_cast<fil_space_t*>(space));
	}

	status->space = ULINT_UNDEFINED;

	if (fil_space_crypt_t* crypt_data = space->crypt_data) {
		status->space = space->id;
		mysql_mutex_lock(&crypt_data->mutex);
		status->scheme = crypt_data->type;
		status->keyserver_requests = crypt_data->keyserver_requests;
		status->min_key_version = crypt_data->min_key_version;
		status->key_id = crypt_data->key_id;

		if (crypt_data->rotate_state.active_threads > 0 ||
		    crypt_data->rotate_state.flushing) {
			status->rotating = true;
			status->flushing =
				crypt_data->rotate_state.flushing;
			status->rotate_next_page_number =
				crypt_data->rotate_state.next_offset;
			status->rotate_max_page_number =
				crypt_data->rotate_state.max_offset;
		}

		mysql_mutex_unlock(&crypt_data->mutex);

		if (srv_encrypt_tables || crypt_data->min_key_version) {
			status->current_key_version =
				fil_crypt_get_latest_key_version(crypt_data);
		}
	}
}

/*********************************************************************
Return crypt statistics
@param[out]	stat		Crypt statistics */
void fil_crypt_total_stat(fil_crypt_stat_t *stat)
{
	mysql_mutex_lock(&crypt_stat_mutex);
	*stat = crypt_stat;
	mysql_mutex_unlock(&crypt_stat_mutex);
}

#endif /* UNIV_INNOCHECKSUM */

/**
Verify that post encryption checksum match calculated checksum.
This function should be called only if tablespace contains crypt_data
metadata (this is strong indication that tablespace is encrypted).
Function also verifies that traditional checksum does not match
calculated checksum as if it does page could be valid unencrypted,
encrypted, or corrupted.

@param[in,out]	page		page frame (checksum is temporarily modified)
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return true if page is encrypted AND OK, false otherwise */
bool fil_space_verify_crypt_checksum(const byte* page, ulint zip_size)
{
	if (ENCRYPTION_KEY_NOT_ENCRYPTED == mach_read_from_4(
			page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION)) {
		return false;
	}

	/* Compressed and encrypted pages do not have checksum. Assume not
	corrupted. Page verification happens after decompression in
	buf_page_t::read_complete() using buf_page_is_corrupted(). */
	if (fil_page_get_type(page) == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		return true;
	}

	/* Read stored post encryption checksum. */
	const ib_uint32_t checksum = mach_read_from_4(
		page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);

	/* If stored checksum matches one of the calculated checksums
	page is not corrupted. */

#ifndef UNIV_INNOCHECKSUM
	switch (srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
#endif /* !UNIV_INNOCHECKSUM */
		if (zip_size) {
			return checksum == page_zip_calc_checksum(
				page, zip_size, false);
		}

		return checksum == buf_calc_page_crc32(page);
#ifndef UNIV_INNOCHECKSUM
	default:
		if (checksum == BUF_NO_CHECKSUM_MAGIC) {
			return true;
		}
		if (zip_size) {
			return checksum == page_zip_calc_checksum(
				page, zip_size, false)
				|| checksum == page_zip_calc_checksum(
					page, zip_size, true);
		}

		return checksum == buf_calc_page_crc32(page)
			|| checksum == buf_calc_page_new_checksum(page);
	}
#endif /* !UNIV_INNOCHECKSUM */
}
