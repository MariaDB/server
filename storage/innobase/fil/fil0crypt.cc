/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (c) 2014, 2021, MariaDB Corporation.

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

#include "fil0fil.h"
#include "mtr0types.h"
#include "mach0data.h"
#include "page0size.h"
#include "page0zip.h"
#ifndef UNIV_INNOCHECKSUM
#include "fil0crypt.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "log0recv.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "ut0ut.h"
#include "btr0scrub.h"
#include "fsp0fsp.h"
#include "fil0pagecompress.h"
#include <my_crypt.h>

static bool fil_crypt_threads_inited = false;

/** Is encryption enabled/disabled */
UNIV_INTERN ulong srv_encrypt_tables = 0;

/** No of key rotation threads requested */
UNIV_INTERN uint srv_n_fil_crypt_threads = 0;

/** No of key rotation threads started */
UNIV_INTERN uint srv_n_fil_crypt_threads_started = 0;

/** At this age or older a space/page will be rotated */
UNIV_INTERN uint srv_fil_crypt_rotate_key_age;

/** Whether the encryption plugin does key rotation */
static bool srv_encrypt_rotate;

/** Event to signal FROM the key rotation threads. */
static os_event_t fil_crypt_event;

/** Event to signal TO the key rotation threads. */
UNIV_INTERN os_event_t fil_crypt_threads_event;

/** Event for waking up threads throttle. */
static os_event_t fil_crypt_throttle_sleep_event;

/** Mutex for key rotation threads. */
UNIV_INTERN ib_mutex_t fil_crypt_threads_mutex;

/** Variable ensuring only 1 thread at time does initial conversion */
static bool fil_crypt_start_converting = false;

/** Variables for throttling */
UNIV_INTERN uint srv_n_fil_crypt_iops = 100;	 // 10ms per iop
static uint srv_alloc_time = 3;		    // allocate iops for 3s at a time
static uint n_fil_crypt_iops_allocated = 0;

/** Variables for scrubbing */
extern uint srv_background_scrub_data_interval;
extern uint srv_background_scrub_data_check_interval;

#define DEBUG_KEYROTATION_THROTTLING 0

/** Statistics variables */
static fil_crypt_stat_t crypt_stat;
static ib_mutex_t crypt_stat_mutex;

/** Is background scrubbing enabled, defined on btr0scrub.cc */
extern my_bool srv_background_scrub_data_uncompressed;
extern my_bool srv_background_scrub_data_compressed;

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
UNIV_INTERN
void
fil_space_crypt_init()
{
	fil_crypt_throttle_sleep_event = os_event_create(0);

	mutex_create(LATCH_ID_FIL_CRYPT_STAT_MUTEX, &crypt_stat_mutex);
	memset(&crypt_stat, 0, sizeof(crypt_stat));
}

/*********************************************************************
Cleanup space crypt */
UNIV_INTERN
void
fil_space_crypt_cleanup()
{
	os_event_destroy(fil_crypt_throttle_sleep_event);
	mutex_free(&crypt_stat_mutex);
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
			/* Below event seen as NULL-pointer at startup
			when new database was created and we create a
			checkpoint. Only seen when debugging. */
			if (fil_crypt_threads_inited) {
				os_event_set(fil_crypt_threads_event);
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
		mutex_exit(&crypt_data->mutex);
	} else {
		mutex_enter(&crypt_data->mutex);
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
UNIV_INTERN
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
UNIV_INTERN
void
fil_space_merge_crypt_data(
	fil_space_crypt_t* dst,
	const fil_space_crypt_t* src)
{
	mutex_enter(&dst->mutex);

	/* validate that they are mergeable */
	ut_a(src->type == CRYPT_SCHEME_UNENCRYPTED ||
	     src->type == CRYPT_SCHEME_1);

	ut_a(dst->type == CRYPT_SCHEME_UNENCRYPTED ||
	     dst->type == CRYPT_SCHEME_1);

	dst->encryption = src->encryption;
	dst->type = src->type;
	dst->min_key_version = src->min_key_version;
	dst->keyserver_requests += src->keyserver_requests;

	mutex_exit(&dst->mutex);
}

/** Initialize encryption parameters from a tablespace header page.
@param[in]	page_size	page size of the tablespace
@param[in]	page		first page of the tablespace
@return crypt data from page 0
@retval	NULL	if not present or not valid */
UNIV_INTERN
fil_space_crypt_t*
fil_space_read_crypt_data(const page_size_t& page_size, const byte* page)
{
	const ulint offset = FSP_HEADER_OFFSET
		+ fsp_header_get_encryption_offset(page_size);

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
	crypt_data->page0_offset = offset;
	memcpy(crypt_data->iv, page + offset + MAGIC_SZ + 2, iv_length);

	return crypt_data;
}

/******************************************************************
Free a crypt data object
@param[in,out] crypt_data	crypt data to be freed */
UNIV_INTERN
void
fil_space_destroy_crypt_data(
	fil_space_crypt_t **crypt_data)
{
	if (crypt_data != NULL && (*crypt_data) != NULL) {
		fil_space_crypt_t* c;
		if (UNIV_LIKELY(fil_crypt_threads_inited)) {
			mutex_enter(&fil_crypt_threads_mutex);
			c = *crypt_data;
			*crypt_data = NULL;
			mutex_exit(&fil_crypt_threads_mutex);
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

/** Fill crypt data information to the give page.
It should be called during ibd file creation.
@param[in]	flags	tablespace flags
@param[in,out]	page	first page of the tablespace */
void
fil_space_crypt_t::fill_page0(
	ulint	flags,
	byte*	page)
{
	const uint len = sizeof(iv);
	const ulint offset = FSP_HEADER_OFFSET
		+ fsp_header_get_encryption_offset(page_size_t(flags));
	page0_offset = offset;

	memcpy(page + offset, CRYPT_MAGIC, MAGIC_SZ);
	mach_write_to_1(page + offset + MAGIC_SZ, type);
	mach_write_to_1(page + offset + MAGIC_SZ + 1, len);
	memcpy(page + offset + MAGIC_SZ + 2, &iv, len);

	mach_write_to_4(page + offset + MAGIC_SZ + 2 + len,
			min_key_version);
	mach_write_to_4(page + offset + MAGIC_SZ + 2 + len + 4,
			key_id);
	mach_write_to_1(page + offset + MAGIC_SZ + 2  + len + 8,
			encryption);
}

/******************************************************************
Write crypt data to a page (0)
@param[in]	space	tablespace
@param[in,out]	page0	first page of the tablespace
@param[in,out]	mtr	mini-transaction */
UNIV_INTERN
void
fil_space_crypt_t::write_page0(
	const fil_space_t*	space,
	byte* 			page,
	mtr_t*			mtr)
{
	ut_ad(this == space->crypt_data);
	const uint len = sizeof(iv);
	const ulint offset = FSP_HEADER_OFFSET
		+ fsp_header_get_encryption_offset(page_size_t(space->flags));
	page0_offset = offset;

	/*
	redo log this as bytewise updates to page 0
	followed by an MLOG_FILE_WRITE_CRYPT_DATA
	(that will during recovery update fil_space_t)
	*/
	mlog_write_string(page + offset, CRYPT_MAGIC, MAGIC_SZ, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 0, type, MLOG_1BYTE, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 1, len, MLOG_1BYTE, mtr);
	mlog_write_string(page + offset + MAGIC_SZ + 2, iv, len,
			  mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len, min_key_version,
			 MLOG_4BYTES, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len + 4, key_id,
			 MLOG_4BYTES, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len + 8, encryption,
		MLOG_1BYTE, mtr);

	DBUG_EXECUTE_IF("ib_do_not_log_crypt_data", return;);

	byte* log_ptr = mlog_open(mtr, 11 + 17 + len);

	if (log_ptr != NULL) {
		log_ptr = mlog_write_initial_log_record_fast(
			page,
			MLOG_FILE_WRITE_CRYPT_DATA,
			log_ptr, mtr);
		mach_write_to_4(log_ptr, space->id);
		log_ptr += 4;
		mach_write_to_2(log_ptr, offset);
		log_ptr += 2;
		mach_write_to_1(log_ptr, type);
		log_ptr += 1;
		mach_write_to_1(log_ptr, len);
		log_ptr += 1;
		mach_write_to_4(log_ptr, min_key_version);
		log_ptr += 4;
		mach_write_to_4(log_ptr, key_id);
		log_ptr += 4;
		mach_write_to_1(log_ptr, encryption);
		log_ptr += 1;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(mtr, iv, len);
	}
}

/******************************************************************
Parse a MLOG_FILE_WRITE_CRYPT_DATA log entry
@param[in]	ptr		Log entry start
@param[in]	end_ptr		Log entry end
@param[in]	block		buffer block
@return position on log buffer */
UNIV_INTERN
byte*
fil_parse_write_crypt_data(
	byte*			ptr,
	const byte*		end_ptr,
	const buf_block_t*	block,
	dberr_t*		err)
{
	/* check that redo log entry is complete */
	uint entry_size =
		4 + // size of space_id
		2 + // size of offset
		1 + // size of type
		1 + // size of iv-len
		4 +  // size of min_key_version
		4 +  // size of key_id
		1; // fil_encryption_t

	*err = DB_SUCCESS;

	if (ptr + entry_size > end_ptr) {
		return NULL;
	}

	ulint space_id = mach_read_from_4(ptr);
	ptr += 4;
	uint offset = mach_read_from_2(ptr);
	ptr += 2;
	uint type = mach_read_from_1(ptr);
	ptr += 1;
	uint len = mach_read_from_1(ptr);
	ptr += 1;

	if ((type != CRYPT_SCHEME_1 && type != CRYPT_SCHEME_UNENCRYPTED)
	    || len != CRYPT_SCHEME_1_IV_LEN) {
		*err = DB_CORRUPTION;
		return NULL;
	}

	uint min_key_version = mach_read_from_4(ptr);
	ptr += 4;

	uint key_id = mach_read_from_4(ptr);
	ptr += 4;

	fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(ptr);
	ptr +=1;

	if (ptr + len > end_ptr) {
		return NULL;
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t* space = fil_space_get_by_id(space_id);

	if (!space) {
		mutex_exit(&fil_system->mutex);
		return ptr + len;
	}

	fil_space_crypt_t* crypt_data = fil_space_create_crypt_data(
		encryption, key_id);

	crypt_data->page0_offset = offset;
	crypt_data->min_key_version = min_key_version;
	crypt_data->type = type;
	memcpy(crypt_data->iv, ptr, len);
	ptr += len;

	if (space->crypt_data) {
		fil_space_merge_crypt_data(space->crypt_data, crypt_data);
		fil_space_destroy_crypt_data(&crypt_data);
		crypt_data = space->crypt_data;
	} else {
		space->crypt_data = crypt_data;
	}

	mutex_exit(&fil_system->mutex);

	if (crypt_data->should_encrypt() && !crypt_data->is_key_found()) {
		*err = DB_DECRYPTION_FAILED;
	}

	return ptr;
}

/** Encrypt a buffer.
@param[in,out]		crypt_data	Crypt data
@param[in]		space		space_id
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in]		page_size	Page size
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_encrypt_buf(
	fil_space_crypt_t*	crypt_data,
	ulint			space,
	ulint			offset,
	lsn_t			lsn,
	const byte*		src_frame,
	const page_size_t&	page_size,
	byte*			dst_frame)
{
	uint size = uint(page_size.physical());
	uint key_version = fil_crypt_get_latest_key_version(crypt_data);

	ut_a(key_version != ENCRYPTION_KEY_VERSION_INVALID);

	ulint orig_page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	ibool page_compressed = (orig_page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
	uint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += (FIL_PAGE_COMPRESSED_SIZE + FIL_PAGE_COMPRESSION_METHOD_SIZE);
	}

	/* FIL page header is not encrypted */
	memcpy(dst_frame, src_frame, header_len);

	/* Store key version */
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, key_version);

	/* Calculate the start offset in a page */
	uint unencrypted_bytes = header_len + FIL_PAGE_DATA_END;
	uint srclen = size - unencrypted_bytes;
	const byte* src = src_frame + header_len;
	byte* dst = dst_frame + header_len;
	uint32 dstlen = 0;

	if (page_compressed) {
		srclen = mach_read_from_2(src_frame + FIL_PAGE_DATA);
	}

	int rc = encryption_scheme_encrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   space, offset, lsn);
	ut_a(rc == MY_AES_OK);
	ut_a(dstlen == srclen);

	/* For compressed tables we do not store the FIL header because
	the whole page is not stored to the disk. In compressed tables only
	the FIL header + compressed (and now encrypted) payload alligned
	to sector boundary is written. */
	if (!page_compressed) {
		/* FIL page trailer is also not encrypted */
		memcpy(dst_frame + page_size.physical() - FIL_PAGE_DATA_END,
			src_frame + page_size.physical() - FIL_PAGE_DATA_END,
			FIL_PAGE_DATA_END);
	} else {
		/* Clean up rest of buffer */
		memset(dst_frame+header_len+srclen, 0,
		       page_size.physical() - (header_len + srclen));
	}

	/* handle post encryption checksum */
	ib_uint32_t checksum = 0;

	checksum = fil_crypt_calculate_checksum(page_size, dst_frame);

	// store the post-encryption checksum after the key-version
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4, checksum);

	ut_ad(fil_space_verify_crypt_checksum(dst_frame, page_size));

	srv_stats.pages_encrypted.inc();

	return dst_frame;
}

/******************************************************************
Encrypt a page

@param[in]		space		Tablespace
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in,out]		dst_frame	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_space_encrypt(
	const fil_space_t*	space,
	ulint			offset,
	lsn_t			lsn,
	byte*			src_frame,
	byte*			dst_frame)
{
	switch (mach_read_from_2(src_frame+FIL_PAGE_TYPE)) {
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_RTREE:
		/* File space header, extent descriptor or spatial index
		are not encrypted. */
		return src_frame;
	}

	if (!space->crypt_data || !space->crypt_data->is_encrypted()) {
		return (src_frame);
	}

	ut_ad(space->n_pending_ios > 0);
	return fil_encrypt_buf(space->crypt_data, space->id, offset, lsn,
			       src_frame, page_size_t(space->flags),
			       dst_frame);
}

/** Decrypt a page.
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@return DB_SUCCESS or error */
UNIV_INTERN
dberr_t
fil_space_decrypt(
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	const page_size_t&	page_size,
	byte*			src_frame)
{
	ulint page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	uint key_version = mach_read_from_4(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	bool page_compressed = (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
	uint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);
	uint space = mach_read_from_4(src_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	ib_uint64_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);

	ut_a(key_version != ENCRYPTION_KEY_NOT_ENCRYPTED);
	ut_a(crypt_data != NULL && crypt_data->is_encrypted());

	/* read space & lsn */
	uint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += (FIL_PAGE_COMPRESSED_SIZE + FIL_PAGE_COMPRESSION_METHOD_SIZE);
	}

	/* Copy FIL page header, it is not encrypted */
	memcpy(tmp_frame, src_frame, header_len);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + header_len;
	byte* dst = tmp_frame + header_len;
	uint32 dstlen = 0;
	uint srclen = uint(page_size.physical())
		- header_len - FIL_PAGE_DATA_END;

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
		memcpy(tmp_frame + page_size.physical() - FIL_PAGE_DATA_END,
		       src_frame + page_size.physical() - FIL_PAGE_DATA_END,
		       FIL_PAGE_DATA_END);
	}

	srv_stats.pages_decrypted.inc();

	return DB_SUCCESS; /* page was decrypted */
}

/**
Decrypt a page.
@param[in]	space			Tablespace
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in,out]	src_frame		Page to decrypt
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
UNIV_INTERN
byte*
fil_space_decrypt(
	const fil_space_t* space,
	byte*		tmp_frame,
	byte*		src_frame)
{
	const page_size_t page_size(space->flags);

	ut_ad(space->crypt_data != NULL && space->crypt_data->is_encrypted());
	ut_ad(space->n_pending_ios > 0);

	if (DB_SUCCESS != fil_space_decrypt(space->crypt_data, tmp_frame,
					    page_size, src_frame)) {
		return NULL;
	}

	/* Copy the decrypted page back to page buffer, not
	really any other options. */
	memcpy(src_frame, tmp_frame, page_size.physical());

	return src_frame;
}

/******************************************************************
Calculate post encryption checksum
@param[in]	page_size	page size
@param[in]	dst_frame	Block where checksum is calculated
@return page checksum
not needed. */
UNIV_INTERN
uint32_t
fil_crypt_calculate_checksum(
	const page_size_t&	page_size,
	const byte*		dst_frame)
{
	/* For encrypted tables we use only crc32 and strict_crc32 */
	return page_size.is_compressed()
		? page_zip_calc_checksum(dst_frame, page_size.physical(),
					 SRV_CHECKSUM_ALGORITHM_CRC32)
		: buf_calc_page_crc32(dst_frame);
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
	if (space->crypt_data || space->size
	    || !fil_space_get_size(space->id)) {
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

	const page_size_t page_size(space->flags);
	mtr_t	mtr;
	mtr.start();
	if (buf_block_t* block = buf_page_get(page_id_t(space->id, 0),
					      page_size, RW_S_LATCH, &mtr)) {
		mutex_enter(&fil_system->mutex);
		if (!space->crypt_data) {
			space->crypt_data = fil_space_read_crypt_data(
				page_size, block->frame);
		}
		mutex_exit(&fil_system->mutex);
	}
	mtr.commit();
}

/** Start encrypting a space
@param[in,out]		space		Tablespace
@return true if a recheck of tablespace is needed by encryption thread. */
static bool fil_crypt_start_encrypting_space(fil_space_t* space)
{
	bool recheck = false;

	mutex_enter(&fil_crypt_threads_mutex);

	fil_space_crypt_t *crypt_data = space->crypt_data;

	/* If space is not encrypted and encryption is not enabled, then
	do not continue encrypting the space. */
	if (!crypt_data && !srv_encrypt_tables) {
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	if (crypt_data != NULL || fil_crypt_start_converting) {
		/* someone beat us to it */
		if (fil_crypt_start_converting) {
			recheck = true;
		}

		mutex_exit(&fil_crypt_threads_mutex);
		return recheck;
	}

	/* NOTE: we need to write and flush page 0 before publishing
	* the crypt data. This so that after restart there is no
	* risk of finding encrypted pages without having
	* crypt data in page 0 */

	/* 1 - create crypt data */
	crypt_data = fil_space_create_crypt_data(
		FIL_ENCRYPTION_DEFAULT, FIL_DEFAULT_ENCRYPTION_KEY);

	if (crypt_data == NULL) {
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	crypt_data->min_key_version = 0; // all pages are unencrypted
	crypt_data->rotate_state.start_time = time(0);
	crypt_data->rotate_state.starting = true;
	crypt_data->rotate_state.active_threads = 1;

	mutex_enter(&fil_system->mutex);
	space->crypt_data = crypt_data;
	mutex_exit(&fil_system->mutex);

	fil_crypt_start_converting = true;
	mutex_exit(&fil_crypt_threads_mutex);

	do
	{
		mtr_t mtr;
		mtr.start();
		mtr.set_named_space(space);

		/* 2 - get page 0 */
		dberr_t err = DB_SUCCESS;
		buf_block_t* block = buf_page_get_gen(
			page_id_t(space->id, 0), page_size_t(space->flags),
			RW_X_LATCH, NULL, BUF_GET,
			__FILE__, __LINE__,
			&mtr, &err);


		/* 3 - write crypt data to page 0 */
		byte* frame = buf_block_get_frame(block);
		crypt_data->type = CRYPT_SCHEME_1;
		crypt_data->write_page0(space, frame, &mtr);

		mtr.commit();

		/* record lsn of update */
		lsn_t end_lsn = mtr.commit_lsn();

		/* 4 - sync tablespace before publishing crypt data */

		bool success = false;

		do {
			ulint n_pages = 0;
			success = buf_flush_lists(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
		} while (!success);

		/* 5 - publish crypt data */
		mutex_enter(&fil_crypt_threads_mutex);
		mutex_enter(&crypt_data->mutex);
		crypt_data->type = CRYPT_SCHEME_1;
		ut_a(crypt_data->rotate_state.active_threads == 1);
		crypt_data->rotate_state.active_threads = 0;
		crypt_data->rotate_state.starting = false;

		fil_crypt_start_converting = false;
		mutex_exit(&crypt_data->mutex);
		mutex_exit(&fil_crypt_threads_mutex);

		return recheck;
	} while (0);

	mutex_enter(&crypt_data->mutex);
	ut_a(crypt_data->rotate_state.active_threads == 1);
	crypt_data->rotate_state.active_threads = 0;
	mutex_exit(&crypt_data->mutex);

	mutex_enter(&fil_crypt_threads_mutex);
	fil_crypt_start_converting = false;
	mutex_exit(&fil_crypt_threads_mutex);

	return recheck;
}

/** State of a rotation thread */
struct rotate_thread_t {
	explicit rotate_thread_t(uint no) {
		memset(this, 0, sizeof(* this));
		thread_no = no;
		first = true;
		estimated_max_iops = 20;
	}

	uint thread_no;
	bool first;		    /*!< is position before first space */
	fil_space_t* space;	    /*!< current space or NULL */
	ulint offset;		    /*!< current offset */
	ulint batch;		    /*!< #pages to rotate */
	uint  min_key_version_found;/*!< min key version found but not rotated */
	lsn_t end_lsn;		    /*!< max lsn when rotating this space */

	uint estimated_max_iops;   /*!< estimation of max iops */
	uint allocated_iops;	   /*!< allocated iops */
	ulint cnt_waited;	   /*!< #times waited during this slot */
	uintmax_t sum_waited_us;   /*!< wait time during this slot */

	fil_crypt_stat_t crypt_stat; // statistics

	btr_scrub_t scrub_data;      /* thread local data used by btr_scrub-functions
				     * when iterating pages of tablespace */

	/** @return whether this thread should terminate */
	bool should_shutdown() const {
		switch (srv_shutdown_state) {
		case SRV_SHUTDOWN_NONE:
			return thread_no >= srv_n_fil_crypt_threads;
		case SRV_SHUTDOWN_EXIT_THREADS:
			/* srv_init_abort() must have been invoked */
		case SRV_SHUTDOWN_CLEANUP:
		case SRV_SHUTDOWN_INITIATED:
			return true;
		case SRV_SHUTDOWN_FLUSH_PHASE:
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
  ut_ad(mutex_own(&fil_system->mutex));
  const ulong encrypt_tables= srv_encrypt_tables;
  if (!crypt_data)
    return !encrypt_tables;
  if (!crypt_data->is_key_found())
    return true;

  mutex_enter(&crypt_data->mutex);
  const bool remove= (space.is_stopping() || crypt_data->not_encrypted()) &&
    (!crypt_data->rotate_state.flushing &&
     !encrypt_tables == !!crypt_data->min_key_version &&
     !crypt_data->rotate_state.active_threads);
  mutex_exit(&crypt_data->mutex);
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
	fil_space_t* space = state->space;

	/* Make sure that tablespace is normal tablespace */
	if (space->purpose != FIL_TYPE_TABLESPACE) {
		return false;
	}

	ut_ad(space->n_pending_ops > 0);

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

	mutex_enter(&crypt_data->mutex);

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

		bool need_key_rotation = fil_crypt_needs_rotation(
			crypt_data,
			crypt_data->min_key_version,
			key_state->key_version,
			key_state->rotate_key_age);

		crypt_data->rotate_state.scrubbing.is_active =
			btr_scrub_start_space(*space, &state->scrub_data);

		time_t diff = time(0) - crypt_data->rotate_state.scrubbing.
			last_scrub_completed;

		bool need_scrubbing =
			(srv_background_scrub_data_uncompressed ||
			 srv_background_scrub_data_compressed) &&
			crypt_data->rotate_state.scrubbing.is_active
			&& diff >= 0
			&& ulint(diff) >= srv_background_scrub_data_interval;

		if (need_key_rotation == false && need_scrubbing == false) {
			break;
		}

		mutex_exit(&crypt_data->mutex);

		return true;
	} while (0);

	mutex_exit(&crypt_data->mutex);


	return false;
}

/***********************************************************************
Update global statistics with thread statistics
@param[in,out]	state		key rotation statistics */
static void
fil_crypt_update_total_stat(
	rotate_thread_t *state)
{
	mutex_enter(&crypt_stat_mutex);
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
	mutex_exit(&crypt_stat_mutex);

	// make new estimate "current" estimate
	memset(&state->crypt_stat, 0, sizeof(state->crypt_stat));
	// record our old (current) estimate
	state->crypt_stat.estimated_iops = state->estimated_max_iops;
}

/***********************************************************************
Allocate iops to thread from global setting,
used before starting to rotate a space.
@param[in,out]		state		Rotation state
@return true if allocation succeeded, false if failed */
static
bool
fil_crypt_alloc_iops(
	rotate_thread_t *state)
{
	ut_ad(state->allocated_iops == 0);

	/* We have not yet selected the space to rotate, thus
	state might not contain space and we can't check
	its status yet. */

	uint max_iops = state->estimated_max_iops;
	mutex_enter(&fil_crypt_threads_mutex);

	if (n_fil_crypt_iops_allocated >= srv_n_fil_crypt_iops) {
		/* this can happen when user decreases srv_fil_crypt_iops */
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	uint alloc = srv_n_fil_crypt_iops - n_fil_crypt_iops_allocated;

	if (alloc > max_iops) {
		alloc = max_iops;
	}

	n_fil_crypt_iops_allocated += alloc;
	mutex_exit(&fil_crypt_threads_mutex);

	state->allocated_iops = alloc;

	return alloc > 0;
}

/***********************************************************************
Reallocate iops to thread,
used when inside a space
@param[in,out]		state		Rotation state */
static
void
fil_crypt_realloc_iops(
	rotate_thread_t *state)
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

		state->estimated_max_iops = uint(1000000 / avg_wait_time_us);
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

	if (state->estimated_max_iops <= state->allocated_iops) {
		/* return extra iops */
		uint extra = state->allocated_iops - state->estimated_max_iops;

		if (extra > 0) {
			mutex_enter(&fil_crypt_threads_mutex);
			if (n_fil_crypt_iops_allocated < extra) {
				/* unknown bug!
				* crash in debug
				* keep n_fil_crypt_iops_allocated unchanged
				* in release */
				ut_ad(0);
				extra = 0;
			}
			n_fil_crypt_iops_allocated -= extra;
			state->allocated_iops -= extra;

			if (state->allocated_iops == 0) {
				/* no matter how slow io system seems to be
				* never decrease allocated_iops to 0... */
				state->allocated_iops ++;
				n_fil_crypt_iops_allocated ++;
			}

			os_event_set(fil_crypt_threads_event);
			mutex_exit(&fil_crypt_threads_mutex);
		}
	} else {
		/* see if there are more to get */
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < srv_n_fil_crypt_iops) {
			/* there are extra iops free */
			uint extra = srv_n_fil_crypt_iops -
				n_fil_crypt_iops_allocated;
			if (state->allocated_iops + extra >
			    state->estimated_max_iops) {
				/* but don't alloc more than our max */
				extra = state->estimated_max_iops -
					state->allocated_iops;
			}
			n_fil_crypt_iops_allocated += extra;
			state->allocated_iops += extra;

			DBUG_PRINT("ib_crypt",
				("thr_no: %u increased iops from %u to %u.",
				state->thread_no,
				state->allocated_iops - extra,
				state->allocated_iops));

		}
		mutex_exit(&fil_crypt_threads_mutex);
	}

	fil_crypt_update_total_stat(state);
}

/***********************************************************************
Return allocated iops to global
@param[in,out]		state		Rotation state */
static
void
fil_crypt_return_iops(
	rotate_thread_t *state)
{
	if (state->allocated_iops > 0) {
		uint iops = state->allocated_iops;
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < iops) {
			/* unknown bug!
			* crash in debug
			* keep n_fil_crypt_iops_allocated unchanged
			* in release */
			ut_ad(0);
			iops = 0;
		}

		n_fil_crypt_iops_allocated -= iops;
		state->allocated_iops = 0;
		os_event_set(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
	}

	fil_crypt_update_total_stat(state);
}

bool fil_crypt_must_default_encrypt()
{
  return !srv_fil_crypt_rotate_key_age || !srv_encrypt_rotate;
}

/** Return the next tablespace from default_encrypt_tables.
@param space   previous tablespace (NULL to start from the start)
@param recheck whether the removal condition needs to be rechecked after
the encryption parameters were changed
@param encrypt expected state of innodb_encrypt_tables
@return the next tablespace to process (n_pending_ops incremented)
@retval NULL if this was the last */
inline fil_space_t *fil_system_t::default_encrypt_next(
  fil_space_t *space, bool recheck, bool encrypt)
{
  ut_ad(mutex_own(&mutex));

  sized_ilist<fil_space_t, rotation_list_tag_t>::iterator it=
    space && space->is_in_default_encrypt
    ? space
    : default_encrypt_tables.begin();
  const sized_ilist<fil_space_t, rotation_list_tag_t>::iterator end=
    default_encrypt_tables.end();

  if (space)
  {
    const bool released= !--space->n_pending_ops;

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
    return NULL;

  space= &*it;
  space->n_pending_ops++;
  ut_ad(!space->is_stopping());
  return space;
}

/** Return the next tablespace.
@param space    previous tablespace (NULL to start from the beginning)
@param recheck  whether the removal condition needs to be rechecked after
the encryption parameters were changed
@param encrypt  expected state of innodb_encrypt_tables
@return pointer to the next tablespace (with n_pending_ops incremented)
@retval NULL if this was the last */
static fil_space_t *fil_space_next(fil_space_t *space, bool recheck,
                                   bool encrypt)
{
  mutex_enter(&fil_system->mutex);
  ut_ad(!space || space->n_pending_ops);

  if (fil_crypt_must_default_encrypt())
    space= fil_system->default_encrypt_next(space, recheck, encrypt);
  else if (!space)
  {
    space= UT_LIST_GET_FIRST(fil_system->space_list);
    /* We can trust that space is not NULL because at least the
    system tablespace is always present and loaded first. */
    space->n_pending_ops++;
  }
  else
  {
    ut_ad(space->n_pending_ops > 0);
    /* Move on to the next fil_space_t */
    space->n_pending_ops--;
    space= UT_LIST_GET_NEXT(space_list, space);

    /* Skip abnormal tablespaces or those that are being created by
    fil_ibd_create(), or being dropped. */
    while (space &&
           (UT_LIST_GET_LEN(space->chain) == 0 ||
            space->is_stopping() || space->purpose != FIL_TYPE_TABLESPACE))
      space= UT_LIST_GET_NEXT(space_list, space);

    if (space)
      space->n_pending_ops++;
  }

  mutex_exit(&fil_system->mutex);
  return space;
}

/** Search for a space needing rotation
@param[in,out]	key_state	Key state
@param[in,out]	state		Rotation state
@param[in,out]	recheck		recheck of the tablespace is needed or
				still encryption thread does write page 0 */
static bool fil_crypt_find_space_to_rotate(
	key_state_t*		key_state,
	rotate_thread_t*	state,
	bool*			recheck)
{
	/* we need iops to start rotating */
	while (!state->should_shutdown() && !fil_crypt_alloc_iops(state)) {
		if (state->space && state->space->is_stopping()) {
			fil_space_release(state->space);
			state->space = NULL;
		}

		os_event_reset(fil_crypt_threads_event);
		os_event_wait_time(fil_crypt_threads_event, 100000);
	}

	if (state->should_shutdown()) {
		if (state->space) {
			fil_space_release(state->space);
			state->space = NULL;
		}
		return false;
	}

	if (state->first) {
		state->first = false;
		if (state->space) {
			fil_space_release(state->space);
		}
		state->space = NULL;
	}

	state->space = fil_space_next(state->space, *recheck,
				      key_state->key_version != 0);

	while (!state->should_shutdown() && state->space) {
		/* If there is no crypt data and we have not yet read
		page 0 for this tablespace, we need to read it before
		we can continue. */
		if (!state->space->crypt_data) {
			fil_crypt_read_crypt_data(state->space);
		}

		if (fil_crypt_space_needs_rotation(state, key_state, recheck)) {
			ut_ad(key_state->key_id);
			/* init state->min_key_version_found before
			* starting on a space */
			state->min_key_version_found = key_state->key_version;
			return true;
		}

		state->space = fil_space_next(state->space, *recheck,
					      key_state->key_version != 0);
	}

	if (state->space) {
		fil_space_release(state->space);
		state->space = NULL;
	}

	/* no work to do; release our allocation of I/O capacity */
	fil_crypt_return_iops(state);

	return false;

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
	mutex_enter(&crypt_data->mutex);
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

	mutex_exit(&crypt_data->mutex);
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
	fil_space_t* space = state->space;

	ut_ad(!space || space->n_pending_ops > 0);

	/* If space is marked to be dropped stop rotation. */
	if (!space || space->is_stopping()) {
		return false;
	}

	fil_space_crypt_t *crypt_data = space->crypt_data;

	mutex_enter(&crypt_data->mutex);
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

	crypt_data->rotate_state.next_offset += batch;
	mutex_exit(&crypt_data->mutex);
	return found;
}

#define fil_crypt_get_page_throttle(state,offset,mtr,sleeptime_ms) \
	fil_crypt_get_page_throttle_func(state, offset, mtr, \
					 sleeptime_ms, __FILE__, __LINE__)

/***********************************************************************
Get a page and compute sleep time
@param[in,out]		state		Rotation state
@param[in]		offset		Page offset
@param[in,out]		mtr		Minitransaction
@param[out]		sleeptime_ms	Sleep time
@param[in]		file		File where called
@param[in]		line		Line where called
@return page or NULL*/
static
buf_block_t*
fil_crypt_get_page_throttle_func(
	rotate_thread_t*	state,
	ulint 			offset,
	mtr_t*			mtr,
	ulint*			sleeptime_ms,
	const char*		file,
	unsigned		line)
{
	fil_space_t* space = state->space;
	const page_size_t page_size = page_size_t(space->flags);
	const page_id_t page_id(space->id, offset);
	ut_ad(space->n_pending_ops > 0);

	/* Before reading from tablespace we need to make sure that
	the tablespace is not about to be dropped. */
	if (space->is_stopping()) {
		return NULL;
	}

	dberr_t err = DB_SUCCESS;
	buf_block_t* block = buf_page_get_gen(page_id, page_size, RW_X_LATCH,
					      NULL,
					      BUF_PEEK_IF_IN_POOL, file, line,
					      mtr, &err);
	if (block != NULL) {
		/* page was in buffer pool */
		state->crypt_stat.pages_read_from_cache++;
		return block;
	}

	if (space->is_stopping()) {
		return NULL;
	}

	state->crypt_stat.pages_read_from_disk++;

	const ulonglong start = my_interval_timer();
	block = buf_page_get_gen(page_id, page_size,
				 RW_X_LATCH,
				 NULL, BUF_GET_POSSIBLY_FREED,
				file, line, mtr, &err);
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
Get block and allocation status

note: innodb locks fil_space_latch and then block when allocating page
but locks block and then fil_space_latch when freeing page.

@param[in,out]		state		Rotation state
@param[in]		offset		Page offset
@param[in,out]		mtr		Minitransaction
@param[out]		allocation_status Allocation status
@param[out]		sleeptime_ms	Sleep time
@return block or NULL
*/
static
buf_block_t*
btr_scrub_get_block_and_allocation_status(
	rotate_thread_t*	state,
	ulint 			offset,
	mtr_t*			mtr,
	btr_scrub_page_allocation_status_t *allocation_status,
	ulint*			sleeptime_ms)
{
	mtr_t local_mtr;
	buf_block_t *block = NULL;
	fil_space_t* space = state->space;

	ut_ad(space->n_pending_ops > 0);

	mtr_start(&local_mtr);

	*allocation_status = fseg_page_is_free(space, offset) ?
		BTR_SCRUB_PAGE_FREE :
		BTR_SCRUB_PAGE_ALLOCATED;

	if (*allocation_status == BTR_SCRUB_PAGE_FREE) {
		/* this is easy case, we lock fil_space_latch first and
		then block */
		block = fil_crypt_get_page_throttle(state,
						    offset, mtr,
						    sleeptime_ms);
		mtr_commit(&local_mtr);
	} else {
		/* page is allocated according to xdes */

		/* release fil_space_latch *before* fetching block */
		mtr_commit(&local_mtr);

		/* NOTE: when we have locked dict_index_get_lock(),
		* it's safe to release fil_space_latch and then fetch block
		* as dict_index_get_lock() is needed to make tree modifications
		* such as free-ing a page
		*/

		block = fil_crypt_get_page_throttle(state,
						    offset, mtr,
						    sleeptime_ms);
	}

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
	fil_space_t*space = state->space;
	ulint space_id = space->id;
	ulint offset = state->offset;
	ulint sleeptime_ms = 0;
	fil_space_crypt_t *crypt_data = space->crypt_data;

	ut_ad(space->n_pending_ops > 0);
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
		int needs_scrubbing = BTR_SCRUB_SKIP_PAGE;
		lsn_t block_lsn = block->page.newest_modification;
		byte* frame = buf_block_get_frame(block);
		uint kv =  mach_read_from_4(frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

		if (space->is_stopping()) {
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
			mlog_write_ulint(frame + FIL_PAGE_SPACE_ID,
					 space_id, MLOG_4BYTES, &mtr);

			/* statistics */
			state->crypt_stat.pages_modified++;
		} else {
			if (crypt_data->is_encrypted()) {
				if (kv < state->min_key_version_found) {
					state->min_key_version_found = kv;
				}
			}

			needs_scrubbing = btr_page_needs_scrubbing(
				&state->scrub_data, block,
				BTR_SCRUB_PAGE_ALLOCATION_UNKNOWN);
		}

		mtr.commit();
		lsn_t end_lsn = mtr.commit_lsn();

		if (needs_scrubbing == BTR_SCRUB_PAGE) {
			mtr.start();
			/*
			* refetch page and allocation status
			*/
			btr_scrub_page_allocation_status_t allocated;

			block = btr_scrub_get_block_and_allocation_status(
				state, offset, &mtr,
				&allocated,
				&sleeptime_ms);

			if (block) {
				mtr.set_named_space(space);

				/* get required table/index and index-locks */
				needs_scrubbing = btr_scrub_recheck_page(
					&state->scrub_data, block, allocated, &mtr);

				if (needs_scrubbing == BTR_SCRUB_PAGE) {
					/* we need to refetch it once more now that we have
					* index locked */
					block = btr_scrub_get_block_and_allocation_status(
						state, offset, &mtr,
						&allocated,
						&sleeptime_ms);

					needs_scrubbing = btr_scrub_page(&state->scrub_data,
						block, allocated,
						&mtr);
				}

				/* NOTE: mtr is committed inside btr_scrub_recheck_page()
				* and/or btr_scrub_page. This is to make sure that
				* locks & pages are latched in corrected order,
				* the mtr is in some circumstances restarted.
				* (mtr_commit() + mtr_start())
				*/
			}
		}

		if (needs_scrubbing != BTR_SCRUB_PAGE) {
			/* if page didn't need scrubbing it might be that cleanups
			are needed. do those outside of any mtr to prevent deadlocks.

			the information what kinds of cleanups that are needed are
			encoded inside the needs_scrubbing, but this is opaque to
			this function (except the value BTR_SCRUB_PAGE) */
			btr_scrub_skip_page(&state->scrub_data, needs_scrubbing);
		}

		if (needs_scrubbing == BTR_SCRUB_TURNED_OFF) {
			/* if we just detected that scrubbing was turned off
			* update global state to reflect this */
			ut_ad(crypt_data);
			mutex_enter(&crypt_data->mutex);
			crypt_data->rotate_state.scrubbing.is_active = false;
			mutex_exit(&crypt_data->mutex);
		}

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
		os_event_reset(fil_crypt_throttle_sleep_event);
		os_event_wait_time(fil_crypt_throttle_sleep_event,
				   1000 * sleeptime_ms);
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
	ulint space = state->space->id;
	ulint end = std::min(state->offset + state->batch,
			     state->space->free_limit);

	ut_ad(state->space->n_pending_ops > 0);

	for (; state->offset < end; state->offset++) {

		/* we can't rotate pages in dblwr buffer as
		* it's not possible to read those due to lots of asserts
		* in buffer pool.
		*
		* However since these are only (short-lived) copies of
		* real pages, they will be updated anyway when the
		* real page is updated
		*/
		if (space == TRX_SYS_SPACE &&
		    buf_dblwr_page_inside(state->offset)) {
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
	fil_space_t* space = state->space;
	fil_space_crypt_t *crypt_data = space->crypt_data;

	ut_ad(space->n_pending_ops > 0);

	/* flush tablespace pages so that there are no pages left with old key */
	lsn_t end_lsn = crypt_data->rotate_state.end_lsn;

	if (end_lsn > 0 && !space->is_stopping()) {
		bool success = false;
		ulint n_pages = 0;
		ulint sum_pages = 0;
		const ulonglong start = my_interval_timer();

		do {
			success = buf_flush_lists(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
			sum_pages += n_pages;
		} while (!success && !space->is_stopping());

		const ulonglong end = my_interval_timer();

		if (sum_pages && end > start) {
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

	dberr_t err;

	if (buf_block_t* block = buf_page_get_gen(
		    page_id_t(space->id, 0), page_size_t(space->flags),
		    RW_X_LATCH, NULL, BUF_GET,
		    __FILE__, __LINE__, &mtr, &err)) {
		mtr.set_named_space(space);
		crypt_data->write_page0(space, block->frame, &mtr);
	}

	mtr.commit();
}

/***********************************************************************
Complete rotating a space
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_complete_rotate_space(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	ut_ad(crypt_data);
	ut_ad(state->space->n_pending_ops > 0);

	/* Space might already be dropped */
	if (!state->space->is_stopping()) {
		mutex_enter(&crypt_data->mutex);

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
		}

		/* inform scrubbing */
		crypt_data->rotate_state.scrubbing.is_active = false;
		mutex_exit(&crypt_data->mutex);

		/* all threads must call btr_scrub_complete_space wo/ mutex held */
		if (state->scrub_data.scrubbing) {
			btr_scrub_complete_space(&state->scrub_data);
			if (should_flush) {
				/* only last thread updates last_scrub_completed */
				ut_ad(crypt_data);
				mutex_enter(&crypt_data->mutex);
				crypt_data->rotate_state.scrubbing.
					last_scrub_completed = time(0);
				mutex_exit(&crypt_data->mutex);
			}
		}

		if (should_flush) {
			fil_crypt_flush_space(state);

			mutex_enter(&crypt_data->mutex);
			crypt_data->rotate_state.flushing = false;
			mutex_exit(&crypt_data->mutex);
		}
	} else {
		mutex_enter(&crypt_data->mutex);
		ut_a(crypt_data->rotate_state.active_threads > 0);
		crypt_data->rotate_state.active_threads--;
		mutex_exit(&crypt_data->mutex);
	}
}

/*********************************************************************//**
A thread which monitors global key state and rotates tablespaces accordingly
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(fil_crypt_thread)(void*)
{
	mutex_enter(&fil_crypt_threads_mutex);
	uint thread_no = srv_n_fil_crypt_threads_started;
	srv_n_fil_crypt_threads_started++;
	os_event_set(fil_crypt_event); /* signal that we started */
	mutex_exit(&fil_crypt_threads_mutex);

	/* state of this thread */
	rotate_thread_t thr(thread_no);

	/* if we find a space that is starting, skip over it and recheck it later */
	bool recheck = false;

	while (!thr.should_shutdown()) {

		key_state_t new_state;

		time_t wait_start = time(0);

		while (!thr.should_shutdown()) {

			/* wait for key state changes
			* i.e either new key version of change or
			* new rotate_key_age */
			os_event_reset(fil_crypt_threads_event);

			if (os_event_wait_time(fil_crypt_threads_event, 1000000) == 0) {
				break;
			}

			if (recheck) {
				/* check recheck here, after sleep, so
				* that we don't busy loop while when one thread is starting
				* a space*/
				break;
			}

			time_t waited = time(0) - wait_start;

			/* Break if we have waited the background scrub
			internal and background scrubbing is enabled */
			if (waited >= 0
			    && ulint(waited) >= srv_background_scrub_data_check_interval
			    && (srv_background_scrub_data_uncompressed
			        || srv_background_scrub_data_compressed)) {
				break;
			}
		}

		recheck = false;
		thr.first = true;      // restart from first tablespace

		/* iterate all spaces searching for those needing rotation */
		while (!thr.should_shutdown() &&
		       fil_crypt_find_space_to_rotate(&new_state, &thr, &recheck)) {

			/* we found a space to rotate */
			fil_crypt_start_rotate_space(&new_state, &thr);

			/* iterate all pages (cooperativly with other threads) */
			while (!thr.should_shutdown() &&
			       fil_crypt_find_page_to_rotate(&new_state, &thr)) {

				if (!thr.space->is_stopping()) {
					/* rotate a (set) of pages */
					fil_crypt_rotate_pages(&new_state, &thr);
				}

				/* If space is marked as stopping, release
				space and stop rotation. */
				if (thr.space->is_stopping()) {
					fil_crypt_complete_rotate_space(
						&new_state, &thr);
					fil_space_release(thr.space);
					thr.space = NULL;
					break;
				}

				/* realloc iops */
				fil_crypt_realloc_iops(&thr);
			}

			/* complete rotation */
			if (thr.space) {
				fil_crypt_complete_rotate_space(&new_state, &thr);
			}

			/* force key state refresh */
			new_state.key_id = 0;

			/* return iops */
			fil_crypt_return_iops(&thr);
		}
	}

	/* return iops if shutting down */
	fil_crypt_return_iops(&thr);

	/* release current space if shutting down */
	if (thr.space) {
		fil_space_release(thr.space);
		thr.space = NULL;
	}

	mutex_enter(&fil_crypt_threads_mutex);
	srv_n_fil_crypt_threads_started--;
	os_event_set(fil_crypt_event); /* signal that we stopped */
	mutex_exit(&fil_crypt_threads_mutex);

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************
Adjust thread count for key rotation
@param[in]	enw_cnt		Number of threads to be used */
UNIV_INTERN
void
fil_crypt_set_thread_cnt(
	const uint	new_cnt)
{
	if (!fil_crypt_threads_inited) {
		fil_crypt_threads_init();
	}

	mutex_enter(&fil_crypt_threads_mutex);

	if (new_cnt > srv_n_fil_crypt_threads) {
		uint add = new_cnt - srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = new_cnt;
		for (uint i = 0; i < add; i++) {
			os_thread_id_t rotation_thread_id;
			os_thread_create(fil_crypt_thread, NULL, &rotation_thread_id);
			ib::info() << "Creating #"
				   << i+1 << " encryption thread id "
				   << os_thread_pf(rotation_thread_id)
				   << " total threads " << new_cnt << ".";
		}
	} else if (new_cnt < srv_n_fil_crypt_threads) {
		srv_n_fil_crypt_threads = new_cnt;
		os_event_set(fil_crypt_threads_event);
	}

	mutex_exit(&fil_crypt_threads_mutex);

	while(srv_n_fil_crypt_threads_started != srv_n_fil_crypt_threads) {
		os_event_reset(fil_crypt_event);
		os_event_wait_time(fil_crypt_event, 100000);
	}

	/* Send a message to encryption threads that there could be
	something to do. */
	if (srv_n_fil_crypt_threads) {
		os_event_set(fil_crypt_threads_event);
	}
}

/** Initialize the tablespace default_encrypt_tables
if innodb_encryption_rotate_key_age=0. */
static void fil_crypt_default_encrypt_tables_fill()
{
	ut_ad(mutex_own(&fil_system->mutex));

	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {
		if (space->purpose != FIL_TYPE_TABLESPACE
		    || space->is_in_default_encrypt
		    || space->is_stopping()
		    || UT_LIST_GET_LEN(space->chain) == 0) {
			continue;
		}

		/* Ensure that crypt_data has been initialized. */
		if (!space->size) {
			/* Protect the tablespace while we may
			release fil_system->mutex. */
			space->n_pending_ops++;
			ut_d(const fil_space_t* s=)
				fil_system->read_page0(space->id);
			ut_ad(!s || s == space);
			space->n_pending_ops--;
			if (!space->size) {
				/* Page 0 was not loaded.
				Skip this tablespace. */
				continue;
			}
		}

		/* Skip ENCRYPTION!=DEFAULT tablespaces. */
		if (space->crypt_data
		    && !space->crypt_data->is_default_encryption()) {
			continue;
		}

		if (srv_encrypt_tables) {
			/* Skip encrypted tablespaces if
			innodb_encrypt_tables!=OFF */
			if (space->crypt_data
			    && space->crypt_data->min_key_version) {
				continue;
			}
		} else {
			/* Skip unencrypted tablespaces if
			innodb_encrypt_tables=OFF */
			if (!space->crypt_data
			    || !space->crypt_data->min_key_version) {
				continue;
			}
		}

		fil_system->default_encrypt_tables.push_back(*space);
		space->is_in_default_encrypt = true;
	}
}

/*********************************************************************
Adjust max key age
@param[in]	val		New max key age */
UNIV_INTERN
void
fil_crypt_set_rotate_key_age(
	uint	val)
{
	mutex_enter(&fil_system->mutex);
	srv_fil_crypt_rotate_key_age = val;
	if (val == 0) {
		fil_crypt_default_encrypt_tables_fill();
	}
	mutex_exit(&fil_system->mutex);
	os_event_set(fil_crypt_threads_event);
}

/*********************************************************************
Adjust rotation iops
@param[in]	val		New max roation iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(
	uint val)
{
	srv_n_fil_crypt_iops = val;
	os_event_set(fil_crypt_threads_event);
}

/*********************************************************************
Adjust encrypt tables
@param[in]	val		New setting for innodb-encrypt-tables */
UNIV_INTERN
void
fil_crypt_set_encrypt_tables(
	uint val)
{
	if (!fil_crypt_threads_inited) {
		return;
	}

	mutex_enter(&fil_system->mutex);

	srv_encrypt_tables = val;

	if (fil_crypt_must_default_encrypt()) {
		fil_crypt_default_encrypt_tables_fill();
	}

	mutex_exit(&fil_system->mutex);

	os_event_set(fil_crypt_threads_event);
}

/*********************************************************************
Init threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_init()
{
	if (!fil_crypt_threads_inited) {
		fil_crypt_event = os_event_create(0);
		fil_crypt_threads_event = os_event_create(0);
		mutex_create(LATCH_ID_FIL_CRYPT_THREADS_MUTEX,
		     &fil_crypt_threads_mutex);

		uint cnt = srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = 0;
		fil_crypt_threads_inited = true;
		fil_crypt_set_thread_cnt(cnt);
	}
}

/*********************************************************************
Clean up key rotation threads resources */
UNIV_INTERN
void
fil_crypt_threads_cleanup()
{
	if (!fil_crypt_threads_inited) {
		return;
	}
	ut_a(!srv_n_fil_crypt_threads_started);
	os_event_destroy(fil_crypt_event);
	os_event_destroy(fil_crypt_threads_event);
	mutex_free(&fil_crypt_threads_mutex);
	fil_crypt_threads_inited = false;
}

/*********************************************************************
Wait for crypt threads to stop accessing space
@param[in]	space		Tablespace */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
	const fil_space_t*	space)
{
	fil_space_crypt_t* crypt_data = space->crypt_data;

	if (!crypt_data || srv_n_fil_crypt_threads == 0
	    || !fil_crypt_threads_inited) {
		return;
	}

	mutex_enter(&fil_crypt_threads_mutex);

	time_t start = time(0);
	time_t last = start;

	mutex_enter(&crypt_data->mutex);
	mutex_exit(&fil_crypt_threads_mutex);

	ulint cnt = crypt_data->rotate_state.active_threads;
	bool flushing = crypt_data->rotate_state.flushing;

	while (cnt > 0 || flushing) {
		mutex_exit(&crypt_data->mutex);
		/* release dict mutex so that scrub threads can release their
		* table references */
		dict_mutex_exit_for_mysql();

		/* wakeup throttle (all) sleepers */
		os_event_set(fil_crypt_throttle_sleep_event);
		os_event_set(fil_crypt_threads_event);

		os_thread_sleep(20000);
		dict_mutex_enter_for_mysql();
		mutex_enter(&crypt_data->mutex);
		cnt = crypt_data->rotate_state.active_threads;
		flushing = crypt_data->rotate_state.flushing;

		time_t now = time(0);

		if (now >= last + 30) {
			ib::warn() << "Waited "
				   << now - start
				   << " seconds to drop space: "
				   << space->name << " ("
				   << space->id << ") active threads "
				   << cnt << "flushing="
				   << flushing << ".";
			last = now;
		}
	}

	mutex_exit(&crypt_data->mutex);
}

/*********************************************************************
Get crypt status for a space (used by information_schema)
@param[in]	space		Tablespace
@param[out]	status		Crypt status */
UNIV_INTERN
void
fil_space_crypt_get_status(
	const fil_space_t*			space,
	struct fil_space_crypt_status_t*	status)
{
	memset(status, 0, sizeof(*status));

	ut_ad(space->n_pending_ops > 0);

	/* If there is no crypt data and we have not yet read
	page 0 for this tablespace, we need to read it before
	we can continue. */
	if (!space->crypt_data) {
		fil_crypt_read_crypt_data(const_cast<fil_space_t*>(space));
	}

	status->space = ULINT_UNDEFINED;

	if (fil_space_crypt_t* crypt_data = space->crypt_data) {
		status->space = space->id;
		mutex_enter(&crypt_data->mutex);
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

		mutex_exit(&crypt_data->mutex);

		if (srv_encrypt_tables || crypt_data->min_key_version) {
			status->current_key_version =
				fil_crypt_get_latest_key_version(crypt_data);
		}
	}
}

/*********************************************************************
Return crypt statistics
@param[out]	stat		Crypt statistics */
UNIV_INTERN
void
fil_crypt_total_stat(
	fil_crypt_stat_t *stat)
{
	mutex_enter(&crypt_stat_mutex);
	*stat = crypt_stat;
	mutex_exit(&crypt_stat_mutex);
}

/*********************************************************************
Get scrub status for a space (used by information_schema)

@param[in]	space		Tablespace
@param[out]	status		Scrub status */
UNIV_INTERN
void
fil_space_get_scrub_status(
	const fil_space_t*			space,
	struct fil_space_scrub_status_t*	status)
{
	memset(status, 0, sizeof(*status));

	ut_ad(space->n_pending_ops > 0);
	fil_space_crypt_t* crypt_data = space->crypt_data;

	status->space = space->id;

	if (crypt_data != NULL) {
		status->compressed = FSP_FLAGS_GET_ZIP_SSIZE(space->flags) > 0;
		mutex_enter(&crypt_data->mutex);
		status->last_scrub_completed =
			crypt_data->rotate_state.scrubbing.last_scrub_completed;
		if (crypt_data->rotate_state.active_threads > 0 &&
		    crypt_data->rotate_state.scrubbing.is_active) {
			status->scrubbing = true;
			status->current_scrub_started =
				crypt_data->rotate_state.start_time;
			status->current_scrub_active_threads =
				crypt_data->rotate_state.active_threads;
			status->current_scrub_page_number =
				crypt_data->rotate_state.next_offset;
			status->current_scrub_max_page_number =
				crypt_data->rotate_state.max_offset;
		}

		mutex_exit(&crypt_data->mutex);
	}
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
@param[in]	page_size	page size
@return whether the encrypted page is OK */
bool
fil_space_verify_crypt_checksum(const byte* page, const page_size_t& page_size)
{
	ut_ad(mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION));

	/* Compressed and encrypted pages do not have checksum. Assume not
	corrupted. Page verification happens after decompression in
	buf_page_io_complete() using buf_page_is_corrupted(). */
	if (mach_read_from_2(page + FIL_PAGE_TYPE)
	    == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		return true;
	}

	/* Read stored post encryption checksum. */
	const ib_uint32_t checksum = mach_read_from_4(
		page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);

	/* If stored checksum matches one of the calculated checksums
	page is not corrupted. */

	switch (srv_checksum_algorithm_t(srv_checksum_algorithm)) {
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		if (page_size.is_compressed()) {
			return checksum == page_zip_calc_checksum(
				page, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_CRC32)
#ifdef INNODB_BUG_ENDIAN_CRC32
				|| checksum == page_zip_calc_checksum(
					page, page_size.physical(),
					SRV_CHECKSUM_ALGORITHM_CRC32, true)
#endif
				;
		}

		return checksum == buf_calc_page_crc32(page)
#ifdef INNODB_BUG_ENDIAN_CRC32
			|| checksum == buf_calc_page_crc32(page, true)
#endif
			;
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		/* Starting with MariaDB 10.1.25, 10.2.7, 10.3.1,
		due to MDEV-12114, fil_crypt_calculate_checksum()
		is only using CRC32 for the encrypted pages.
		Due to this, we must treat "strict_none" as "none". */
	case SRV_CHECKSUM_ALGORITHM_NONE:
		return true;
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		/* Starting with MariaDB 10.1.25, 10.2.7, 10.3.1,
		due to MDEV-12114, fil_crypt_calculate_checksum()
		is only using CRC32 for the encrypted pages.
		Due to this, we must treat "strict_innodb" as "innodb". */
	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_CRC32:
		if (checksum == BUF_NO_CHECKSUM_MAGIC) {
			return true;
		}
		if (page_size.is_compressed()) {
			return checksum == page_zip_calc_checksum(
				page, page_size.physical(),
				SRV_CHECKSUM_ALGORITHM_CRC32)
#ifdef INNODB_BUG_ENDIAN_CRC32
				|| checksum == page_zip_calc_checksum(
					page, page_size.physical(),
					SRV_CHECKSUM_ALGORITHM_CRC32, true)
#endif
				|| checksum == page_zip_calc_checksum(
					page, page_size.physical(),
					SRV_CHECKSUM_ALGORITHM_INNODB);
		}

		return checksum == buf_calc_page_crc32(page)
#ifdef INNODB_BUG_ENDIAN_CRC32
			|| checksum == buf_calc_page_crc32(page, true)
#endif
			|| checksum == buf_calc_page_new_checksum(page);
	}

	ut_ad(!"unhandled innodb_checksum_algorithm");
	return false;
}
