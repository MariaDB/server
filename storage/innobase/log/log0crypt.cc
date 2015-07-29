/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2015, MariaDB Corporation. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/
/**************************************************//**
@file log0crypt.cc
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/
#include "m_string.h"
#include "log0crypt.h"
#include <my_crypt.h>
#include <my_crypt.h>

#include "log0log.h"
#include "srv0start.h" // for srv_start_lsn
#include "log0recv.h"  // for recv_sys

#include "ha_prototypes.h" // IB_LOG_

#include "my_crypt.h"

#define UNENCRYPTED_KEY_VER	0

/* If true, enable redo log encryption. */
extern my_bool srv_encrypt_log;


#include <algorithm>    // std::sort
#include <deque>

/* If true, enable redo log encryption. */
UNIV_INTERN my_bool srv_encrypt_log = FALSE;
/*
  Sub system type for InnoDB redo log crypto.
  Set and used to validate crypto msg.
*/
static const byte redo_log_purpose_byte = 0x02;

#define LOG_DEFAULT_ENCRYPTION_KEY 1

/*
  Store this many keys into each checkpoint info
*/
static const size_t kMaxSavedKeys = LOG_CRYPT_MAX_ENTRIES;

struct crypt_info_t {
	ib_uint64_t     checkpoint_no; /*!< checkpoint no */
	uint		key_version;   /*!< mysqld key version */
	byte            crypt_msg[MY_AES_BLOCK_SIZE];
	byte		crypt_key[MY_AES_BLOCK_SIZE];
	byte            crypt_nonce[MY_AES_BLOCK_SIZE];
};

static std::deque<crypt_info_t> crypt_info;

/*********************************************************************//**
Get a log block's start lsn.
@return a log block's start lsn */
static inline
lsn_t
log_block_get_start_lsn(
/*====================*/
	lsn_t lsn,			/*!< in: checkpoint lsn */
	ulint log_block_no)		/*!< in: log block number */
{
	lsn_t start_lsn =
		(lsn & (lsn_t)0xffffffff00000000ULL) |
		(((log_block_no - 1) & (lsn_t)0x3fffffff) << 9);
	return start_lsn;
}

static
const crypt_info_t*
get_crypt_info(
/*===========*/
	ib_uint64_t checkpoint_no)
{
	/* so that no one is modifying array while we search */
	ut_ad(mutex_own(&(log_sys->mutex)));

	/* a log block only stores 4-bytes of checkpoint no */
	checkpoint_no &= 0xFFFFFFFF;
	for (size_t i = 0; i < crypt_info.size(); i++) {
		struct crypt_info_t* it = &crypt_info[i];

		if (it->checkpoint_no == checkpoint_no) {
			return it;
		}
	}
	return NULL;
}

static
const crypt_info_t*
get_crypt_info(
/*===========*/
	const byte* log_block) {
	ib_uint64_t checkpoint_no = log_block_get_checkpoint_no(log_block);
	return get_crypt_info(checkpoint_no);
}

/*********************************************************************//**
Call AES CTR to encrypt/decrypt log blocks. */
static
Crypt_result
log_blocks_crypt(
/*=============*/
	const byte* block,	/*!< in: blocks before encrypt/decrypt*/
	ulint size,		/*!< in: size of block */
	byte* dst_block,	/*!< out: blocks after encrypt/decrypt */
	bool is_encrypt)	/*!< in: encrypt or decrypt*/
{
	byte *log_block = (byte*)block;
	Crypt_result rc = MY_AES_OK;
	uint dst_len;
	byte aes_ctr_counter[MY_AES_BLOCK_SIZE];
	lsn_t lsn = is_encrypt ? log_sys->lsn : srv_start_lsn;

	const int src_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE;
	for (ulint i = 0; i < size ; i += OS_FILE_LOG_BLOCK_SIZE) {
		ulint log_block_no = log_block_get_hdr_no(log_block);
		lsn_t log_block_start_lsn = log_block_get_start_lsn(
			lsn, log_block_no);

		const crypt_info_t* info = get_crypt_info(log_block);
#ifdef DEBUG_CRYPT
		fprintf(stderr,
			"%s %lu chkpt: %lu key: %u lsn: %lu\n",
			is_encrypt ? "crypt" : "decrypt",
			log_block_no,
			log_block_get_checkpoint_no(log_block),
			info ? info->key_version : 0,
			log_block_start_lsn);
#endif
		if (info == NULL ||
		    info->key_version == UNENCRYPTED_KEY_VER) {
			memcpy(dst_block, log_block, OS_FILE_LOG_BLOCK_SIZE);
			goto next;
		}

		// Assume log block header is not encrypted
		memcpy(dst_block, log_block, LOG_BLOCK_HDR_SIZE);

		// aes_ctr_counter = nonce(3-byte) + start lsn to a log block
		// (8-byte) + lbn (4-byte) + abn
		// (1-byte, only 5 bits are used). "+" means concatenate.
		bzero(aes_ctr_counter, MY_AES_BLOCK_SIZE);
		memcpy(aes_ctr_counter, info->crypt_nonce, 3);
		mach_write_to_8(aes_ctr_counter + 3, log_block_start_lsn);
		mach_write_to_4(aes_ctr_counter + 11, log_block_no);
		bzero(aes_ctr_counter + 15, 1);

		int rc;
		if (is_encrypt) {
			rc = encryption_encrypt(log_block + LOG_BLOCK_HDR_SIZE, src_len,
						dst_block + LOG_BLOCK_HDR_SIZE, &dst_len,
						(unsigned char*)(info->crypt_key), 16,
						aes_ctr_counter, MY_AES_BLOCK_SIZE, 1,
						LOG_DEFAULT_ENCRYPTION_KEY,
						info->key_version);
		} else {
			rc = encryption_decrypt(log_block + LOG_BLOCK_HDR_SIZE, src_len,
						dst_block + LOG_BLOCK_HDR_SIZE, &dst_len,
						(unsigned char*)(info->crypt_key), 16,
						aes_ctr_counter, MY_AES_BLOCK_SIZE, 1,
						LOG_DEFAULT_ENCRYPTION_KEY,
						info->key_version);
		}

		ut_a(rc == MY_AES_OK);
		ut_a(dst_len == src_len);
next:
		log_block += OS_FILE_LOG_BLOCK_SIZE;
		dst_block += OS_FILE_LOG_BLOCK_SIZE;
	}

	return rc;
}

/*********************************************************************//**
Generate crypt key from crypt msg.
@return true if successfull, false if not. */
static
bool
init_crypt_key(
/*===========*/
	crypt_info_t* info)		/*< in/out: crypt info */
{
	if (info->key_version == UNENCRYPTED_KEY_VER) {
		memset(info->crypt_key, 0, sizeof(info->crypt_key));
		memset(info->crypt_msg, 0, sizeof(info->crypt_msg));
		memset(info->crypt_nonce, 0, sizeof(info->crypt_nonce));
		return true;
	}

	byte mysqld_key[MY_AES_BLOCK_SIZE] = {0};
	uint keylen= sizeof(mysqld_key);

	if (encryption_key_get(LOG_DEFAULT_ENCRYPTION_KEY, info->key_version, mysqld_key, &keylen))
	{
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Redo log crypto: getting mysqld crypto key "
			"from key version failed. Reason could be that requested"
			" key_version %u is not found or required encryption "
			" key management is not found.", info->key_version);
		return false;
	}

	uint dst_len;
	int rc= my_aes_encrypt_ecb(info->crypt_msg, sizeof(info->crypt_msg), //src, srclen
                        info->crypt_key, &dst_len, //dst, &dstlen
                        (unsigned char*)&mysqld_key, sizeof(mysqld_key),
                        NULL, 0, 1);

	if (rc != MY_AES_OK || dst_len != MY_AES_BLOCK_SIZE) {
		fprintf(stderr,
			"\nInnodb redo log crypto: getting redo log crypto key "
			"failed.\n");
		return false;
	}

	return true;
}

static bool mysort(const crypt_info_t& i,
		   const crypt_info_t& j)
{
	return i.checkpoint_no > j.checkpoint_no;
}

static
bool add_crypt_info(crypt_info_t* info)
{
	/* so that no one is searching array while we modify it */
	ut_ad(mutex_own(&(log_sys->mutex)));

	if (get_crypt_info(info->checkpoint_no) != NULL) {
		// already present...
		return true;
	}

	if (!init_crypt_key(info)) {
		return false;
	}

	crypt_info.push_back(*info);

	/* a log block only stores 4-bytes of checkpoint no */
	crypt_info.back().checkpoint_no &= 0xFFFFFFFF;

	// keep keys sorted, assuming that last added key will be used most
	std::sort(crypt_info.begin(), crypt_info.end(), mysort);

	return true;
}

/*********************************************************************//**
Encrypt log blocks. */
UNIV_INTERN
Crypt_result
log_blocks_encrypt(
/*===============*/
	const byte* block,		/*!< in: blocks before encryption */
	const ulint size,		/*!< in: size of blocks, must be multiple of a log block */
	byte* dst_block)		/*!< out: blocks after encryption */
{
	return log_blocks_crypt(block, size, dst_block, true);
}

/*********************************************************************//**
Set next checkpoint's key version to latest one, and generate current
key. Key version 0 means no encryption. */
UNIV_INTERN
void
log_crypt_set_ver_and_key(
/*======================*/
	ib_uint64_t next_checkpoint_no)
{
	crypt_info_t info;
	info.checkpoint_no = next_checkpoint_no;

	if (!srv_encrypt_log) {
		info.key_version = UNENCRYPTED_KEY_VER;
	} else {
		info.key_version = encryption_key_get_latest_version(LOG_DEFAULT_ENCRYPTION_KEY);
	}

	if (info.key_version == UNENCRYPTED_KEY_VER) {
		memset(info.crypt_msg, 0, sizeof(info.crypt_msg));
		memset(info.crypt_nonce, 0, sizeof(info.crypt_nonce));
	} else {
		if (my_random_bytes(info.crypt_msg, MY_AES_BLOCK_SIZE) != MY_AES_OK) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Redo log crypto: generate "
				"%u-byte random number as crypto msg failed.",
				MY_AES_BLOCK_SIZE);
			ut_error;
		}

		if (my_random_bytes(info.crypt_nonce, MY_AES_BLOCK_SIZE) != MY_AES_OK) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Redo log crypto: generate "
				"%u-byte random number as AES_CTR nonce failed.",
				MY_AES_BLOCK_SIZE);
			ut_error;
		}

	}

	add_crypt_info(&info);
}

/********************************************************
Encrypt one or more log block before it is flushed to disk */
UNIV_INTERN
void
log_encrypt_before_write(
/*===========================*/
	ib_uint64_t next_checkpoint_no,	/*!< in: log group to be flushed */
	byte* block,			/*!< in/out: pointer to a log block */
	const ulint size)		/*!< in: size of log blocks */
{
	ut_ad(size % OS_FILE_LOG_BLOCK_SIZE == 0);

	const crypt_info_t* info = get_crypt_info(next_checkpoint_no);
	if (info == NULL) {
		return;
	}

	if (info->key_version == UNENCRYPTED_KEY_VER) {
		return;
	}

	byte* dst_frame = (byte*)malloc(size);

	//encrypt log blocks content
	Crypt_result result = log_blocks_crypt(block, size, dst_frame, true);

	if (result == MY_AES_OK) {
		ut_ad(block[0] == dst_frame[0]);
		memcpy(block, dst_frame, size);
	}
	free(dst_frame);

	if (unlikely(result != MY_AES_OK)) {
		ut_error;
	}
}

/********************************************************
Decrypt a specified log segment after they are read from a log file to a buffer.
*/
void
log_decrypt_after_read(
/*==========================*/
	byte* frame,	        /*!< in/out: log segment */
	const ulint size)	/*!< in: log segment size */
{
	ut_ad(size % OS_FILE_LOG_BLOCK_SIZE == 0);
	byte* dst_frame = (byte*)malloc(size);

	// decrypt log blocks content
	Crypt_result result = log_blocks_crypt(frame, size, dst_frame, false);

	if (result == MY_AES_OK) {
		memcpy(frame, dst_frame, size);
	}
	free(dst_frame);

	if (unlikely(result != MY_AES_OK)) {
		ut_error;
	}
}

/*********************************************************************//**
Writes the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, to a log header's
checkpoint buf. */
UNIV_INTERN
void
log_crypt_write_checkpoint_buf(
/*===========================*/
	byte*	buf)			/*!< in/out: checkpoint buffer */
{
	byte *save = buf;

	// Only write kMaxSavedKeys (sort keys to remove oldest)
	std::sort(crypt_info.begin(), crypt_info.end(), mysort);
	while (crypt_info.size() > kMaxSavedKeys) {
		crypt_info.pop_back();
	}

	bool encrypted = false;
	for (size_t i = 0; i < crypt_info.size(); i++) {
                const crypt_info_t & it = crypt_info[i];
		if (it.key_version != UNENCRYPTED_KEY_VER) {
			encrypted = true;
			break;
		}
	}

	if (encrypted == false) {
		// if no encryption is inuse then zero out
		// crypt data for upward/downward compability
		memset(buf + LOG_CRYPT_VER, 0, LOG_CRYPT_SIZE);
		return;
	}

	ib_uint64_t checkpoint_no = mach_read_from_8(buf + LOG_CHECKPOINT_NO);
	buf += LOG_CRYPT_VER;

	mach_write_to_1(buf + 0, redo_log_purpose_byte);
	mach_write_to_1(buf + 1, crypt_info.size());
	buf += 2;
	for (size_t i = 0; i < crypt_info.size(); i++) {
		struct crypt_info_t* it = &crypt_info[i];
		mach_write_to_4(buf + 0, it->checkpoint_no);
		mach_write_to_4(buf + 4, it->key_version);
		memcpy(buf + 8, it->crypt_msg, MY_AES_BLOCK_SIZE);
		memcpy(buf + 24, it->crypt_nonce, MY_AES_BLOCK_SIZE);
		buf += LOG_CRYPT_ENTRY_SIZE;
	}

#ifdef DEBUG_CRYPT
	fprintf(stderr, "write chk: %lu [ chk key ]: ", checkpoint_no);
	for (size_t i = 0; i < crypt_info.size(); i++) {
		struct crypt_info_t* it = &crypt_info[i];
		fprintf(stderr, "[ %lu %u ] ",
			it->checkpoint_no,
			it->key_version);
	}
	fprintf(stderr, "\n");
#else
	(void)checkpoint_no; // unused variable
#endif
        ut_a((ulint)(buf - save) <= OS_FILE_LOG_BLOCK_SIZE);
}

/*********************************************************************//**
Read the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, from a log header's
checkpoint buf. */
UNIV_INTERN
bool
log_crypt_read_checkpoint_buf(
/*===========================*/
	const byte*	buf) {			/*!< in: checkpoint buffer */

	buf += LOG_CRYPT_VER;

	byte scheme = buf[0];
	if (scheme != redo_log_purpose_byte) {
		return true;
	}
	buf++;
	size_t n = buf[0];
	buf++;

	for (size_t i = 0; i < n; i++) {
		struct crypt_info_t info;
		info.checkpoint_no = mach_read_from_4(buf + 0);
		info.key_version = mach_read_from_4(buf + 4);
		memcpy(info.crypt_msg, buf + 8, MY_AES_BLOCK_SIZE);
		memcpy(info.crypt_nonce, buf + 24, MY_AES_BLOCK_SIZE);

		if (!add_crypt_info(&info)) {
			return false;
		}
		buf += LOG_CRYPT_ENTRY_SIZE;
	}

#ifdef DEBUG_CRYPT
	fprintf(stderr, "read [ chk key ]: ");
	for (size_t i = 0; i < crypt_info.size(); i++) {
		struct crypt_info_t* it = &crypt_info[i];
		fprintf(stderr, "[ %lu %u ] ",
			it->checkpoint_no,
			it->key_version);
	}
	fprintf(stderr, "\n");
#endif
	return true;
}

