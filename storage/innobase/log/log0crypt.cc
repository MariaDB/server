/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2017, MariaDB Corporation. All Rights Reserved.

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
MDEV-11782: Rewritten for MariaDB 10.2 by Marko Mäkelä, MariaDB Corporation.
*******************************************************/
#include "m_string.h"
#include "log0crypt.h"
#include <mysql/service_my_crypt.h>

#include "log0crypt.h"
#include "srv0start.h" // for srv_start_lsn
#include "log0recv.h"  // for recv_sys

/** innodb_encrypt_log: whether to encrypt the redo log */
my_bool srv_encrypt_log;

/** Redo log encryption key ID */
#define LOG_DEFAULT_ENCRYPTION_KEY 1

typedef union {
	uint32_t	words[MY_AES_BLOCK_SIZE / sizeof(uint32_t)];
	byte		bytes[MY_AES_BLOCK_SIZE];
} aes_block_t;

struct crypt_info_t {
	ulint		checkpoint_no; /*!< checkpoint no; 32 bits */
	uint		key_version;   /*!< mysqld key version */
	/** random string for encrypting the key */
	aes_block_t	crypt_msg;
	/** the secret key */
	aes_block_t	crypt_key;
	/** a random string for the per-block initialization vector */
	union {
		uint32_t	word;
		byte		bytes[4];
	} crypt_nonce;
};

/** The crypt info */
static crypt_info_t info;

/** Crypt info when upgrading from 10.1 */
static crypt_info_t infos[5];

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

/*********************************************************************//**
Get crypt info from checkpoint.
@return a crypt info or NULL if not present. */
static
const crypt_info_t*
get_crypt_info(ulint checkpoint_no)
{
	/* a log block only stores 4-bytes of checkpoint no */
	checkpoint_no &= 0xFFFFFFFF;
	for (unsigned i = 0; i < 5; i++) {
		const crypt_info_t* it = &infos[i];

		if (it->key_version && it->checkpoint_no == checkpoint_no) {
			return it;
		}
	}

	/* If checkpoint contains more than one key and we did not
	find the correct one use the first one. */
	return infos;
}

/** Encrypt or decrypt log blocks.
@param[in,out]	buf	log blocks to encrypt or decrypt
@param[in]	size	size of the buffer, in bytes
@param[in]	decrypt	whether to decrypt instead of encrypting */
UNIV_INTERN
void
log_crypt(byte* buf, ulint size, bool decrypt)
{
	ut_ad(size % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a(info.key_version);

	uint dst_len;
	uint32_t aes_ctr_iv[MY_AES_BLOCK_SIZE / sizeof(uint32_t)];
	compile_time_assert(sizeof(uint32_t) == 4);

#define LOG_CRYPT_HDR_SIZE 4

	for (const byte* const end = buf + size; buf != end;
	     buf += OS_FILE_LOG_BLOCK_SIZE) {
		uint32_t dst[(OS_FILE_LOG_BLOCK_SIZE - LOG_CRYPT_HDR_SIZE)
			     / sizeof(uint32_t)];
		const ulint log_block_no = log_block_get_hdr_no(buf);

		/* The log block number is not encrypted. */
		*aes_ctr_iv =
#ifdef WORDS_BIGENDIAN
			~LOG_BLOCK_FLUSH_BIT_MASK
#else
			~(LOG_BLOCK_FLUSH_BIT_MASK >> 24)
#endif
			& (*dst = *reinterpret_cast<const uint32_t*>(
				   buf + LOG_BLOCK_HDR_NO));
#if LOG_BLOCK_HDR_NO + 4 != LOG_CRYPT_HDR_SIZE
# error "LOG_BLOCK_HDR_NO has been moved; redo log format affected!"
#endif
		aes_ctr_iv[1] = info.crypt_nonce.word;
		mach_write_to_8(reinterpret_cast<byte*>(aes_ctr_iv + 2),
				log_block_get_start_lsn(
					decrypt ? srv_start_lsn : log_sys->lsn,
					log_block_no));

		int rc = encryption_crypt(
			buf + LOG_CRYPT_HDR_SIZE, sizeof dst,
			reinterpret_cast<byte*>(dst), &dst_len,
			const_cast<byte*>(info.crypt_key.bytes),
			sizeof info.crypt_key,
			reinterpret_cast<byte*>(aes_ctr_iv), sizeof aes_ctr_iv,
			decrypt
			? ENCRYPTION_FLAG_DECRYPT | ENCRYPTION_FLAG_NOPAD
			: ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
			LOG_DEFAULT_ENCRYPTION_KEY,
			info.key_version);

		ut_a(rc == MY_AES_OK);
		ut_a(dst_len == sizeof dst);
		memcpy(buf + LOG_CRYPT_HDR_SIZE, dst, sizeof dst);
	}
}

/** Generate crypt key from crypt msg.
@param[in,out]	info	encryption key
@param[in]	upgrade	whether to use the key in MariaDB 10.1 format
@return whether the operation was successful */
static
bool
init_crypt_key(crypt_info_t* info, bool upgrade = false)
{
	byte	mysqld_key[MY_AES_MAX_KEY_LENGTH];
	uint	keylen = sizeof mysqld_key;

	compile_time_assert(16 == sizeof info->crypt_key);

	if (uint rc = encryption_key_get(LOG_DEFAULT_ENCRYPTION_KEY,
					 info->key_version, mysqld_key,
					 &keylen)) {
		ib::error()
			<< "Obtaining redo log encryption key version "
			<< info->key_version << " failed (" << rc
			<< "). Maybe the key or the required encryption "
			<< " key management plugin was not found.";
		return false;
	}

	if (upgrade) {
		while (keylen < sizeof mysqld_key) {
			mysqld_key[keylen++] = 0;
		}
	}

	uint dst_len;
	int err= my_aes_crypt(MY_AES_ECB,
			      ENCRYPTION_FLAG_NOPAD | ENCRYPTION_FLAG_ENCRYPT,
			      info->crypt_msg.bytes, sizeof info->crypt_msg,
			      info->crypt_key.bytes, &dst_len,
			      mysqld_key, keylen, NULL, 0);

	if (err != MY_AES_OK || dst_len != MY_AES_BLOCK_SIZE) {
		ib::error() << "Getting redo log crypto key failed: err = "
			<< err << ", len = " << dst_len;
		return false;
	}

	return true;
}

/** Initialize the redo log encryption key.
@return whether the operation succeeded */
UNIV_INTERN
bool
log_crypt_init()
{
	ut_ad(log_mutex_own());
	ut_ad(log_sys->is_encrypted());

	info.key_version = encryption_key_get_latest_version(
		LOG_DEFAULT_ENCRYPTION_KEY);

	if (info.key_version == ENCRYPTION_KEY_VERSION_INVALID) {
		ib::error() << "innodb_encrypt_log: cannot get key version";
		info.key_version = 0;
		return false;
	}

	if (my_random_bytes(info.crypt_msg.bytes, sizeof info.crypt_msg)
	    != MY_AES_OK
	    || my_random_bytes(info.crypt_nonce.bytes, sizeof info.crypt_nonce)
	    != MY_AES_OK) {
		ib::error() << "innodb_encrypt_log: my_random_bytes() failed";
		return false;
	}

	return init_crypt_key(&info);
}

/** Read the MariaDB 10.1 checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
UNIV_INTERN
bool
log_crypt_101_read_checkpoint(const byte* buf)
{
	buf += 20 + 32 * 9;

	const size_t n = *buf++ == 2 ? std::min(unsigned(*buf++), 5U) : 0;

	for (size_t i = 0; i < n; i++) {
		struct crypt_info_t& info = infos[i];
		info.checkpoint_no = mach_read_from_4(buf);
		info.key_version = mach_read_from_4(buf + 4);
		memcpy(info.crypt_msg.bytes, buf + 8, sizeof info.crypt_msg);
		memcpy(info.crypt_nonce.bytes, buf + 24,
		       sizeof info.crypt_nonce);

		if (!init_crypt_key(&info, true)) {
			return false;
		}
		buf += 4 + 4 + 2 * MY_AES_BLOCK_SIZE;
	}

	return true;
}

/** Decrypt a MariaDB 10.1 redo log block.
@param[in,out]	buf	log block
@return	whether the decryption was successful */
UNIV_INTERN
bool
log_crypt_101_read_block(byte* buf)
{
	ut_ad(log_block_calc_checksum_format_0(buf)
	      != log_block_get_checksum(buf));
	const crypt_info_t* info = get_crypt_info(
		log_block_get_checkpoint_no(buf));

	if (!info || info->key_version == 0) {
		return false;
	}

	byte dst[OS_FILE_LOG_BLOCK_SIZE];
	uint dst_len;
	byte aes_ctr_iv[MY_AES_BLOCK_SIZE];

	const uint src_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE;

	ulint log_block_no = log_block_get_hdr_no(buf);

	/* The log block header is not encrypted. */
	memcpy(dst, buf, LOG_BLOCK_HDR_SIZE);

	memcpy(aes_ctr_iv, info->crypt_nonce.bytes, 3);
	mach_write_to_8(aes_ctr_iv + 3,
			log_block_get_start_lsn(srv_start_lsn, log_block_no));
	memcpy(aes_ctr_iv + 11, buf, 4);
	aes_ctr_iv[11] &= ~(LOG_BLOCK_FLUSH_BIT_MASK >> 24);
	aes_ctr_iv[15] = 0;

	int rc = encryption_crypt(buf + LOG_BLOCK_HDR_SIZE, src_len,
				  dst + LOG_BLOCK_HDR_SIZE, &dst_len,
				  const_cast<byte*>(info->crypt_key.bytes),
				  MY_AES_BLOCK_SIZE,
				  aes_ctr_iv, MY_AES_BLOCK_SIZE,
				  ENCRYPTION_FLAG_DECRYPT
				  | ENCRYPTION_FLAG_NOPAD,
				  LOG_DEFAULT_ENCRYPTION_KEY,
				  info->key_version);

	if (rc != MY_AES_OK || dst_len != src_len
	    || log_block_calc_checksum_format_0(dst)
	    != log_block_get_checksum(dst)) {
		return false;
	}

	memcpy(buf, dst, sizeof dst);
	return true;
}

/** Add the encryption information to a redo log checkpoint buffer.
@param[in,out]	buf	checkpoint buffer */
UNIV_INTERN
void
log_crypt_write_checkpoint_buf(byte* buf)
{
	ut_ad(info.key_version);
	compile_time_assert(16 == sizeof info.crypt_msg);
	compile_time_assert(LOG_CHECKPOINT_CRYPT_MESSAGE
			    - LOG_CHECKPOINT_CRYPT_NONCE
			    == sizeof info.crypt_nonce);

	memcpy(buf + LOG_CHECKPOINT_CRYPT_MESSAGE, info.crypt_msg.bytes,
	       sizeof info.crypt_msg);
	memcpy(buf + LOG_CHECKPOINT_CRYPT_NONCE, info.crypt_nonce.bytes,
	       sizeof info.crypt_nonce);
	mach_write_to_4(buf + LOG_CHECKPOINT_CRYPT_KEY, info.key_version);
}

/** Read the checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
UNIV_INTERN
bool
log_crypt_read_checkpoint_buf(const byte* buf)
{
	info.checkpoint_no = mach_read_from_4(buf + (LOG_CHECKPOINT_NO + 4));
	info.key_version = mach_read_from_4(buf + LOG_CHECKPOINT_CRYPT_KEY);

#if MY_AES_BLOCK_SIZE != 16
# error "MY_AES_BLOCK_SIZE != 16; redo log checkpoint format affected"
#endif
	compile_time_assert(16 == sizeof info.crypt_msg);
	compile_time_assert(LOG_CHECKPOINT_CRYPT_MESSAGE
			    - LOG_CHECKPOINT_CRYPT_NONCE
			    == sizeof info.crypt_nonce);

	memcpy(info.crypt_msg.bytes, buf + LOG_CHECKPOINT_CRYPT_MESSAGE,
	       sizeof info.crypt_msg);
	memcpy(info.crypt_nonce.bytes, buf + LOG_CHECKPOINT_CRYPT_NONCE,
	       sizeof info.crypt_nonce);

	return init_crypt_key(&info);
}
