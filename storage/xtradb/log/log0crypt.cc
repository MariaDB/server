/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2018, MariaDB Corporation.

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
@file log0crypt.cc
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/
#include "m_string.h"
#include "log0crypt.h"
#include <mysql/service_my_crypt.h>

#include "log0log.h"
#include "srv0start.h" // for srv_start_lsn
#include "log0recv.h"  // for recv_sys

#include "ha_prototypes.h" // IB_LOG_

/* Used for debugging */
// #define DEBUG_CRYPT 1
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
Get crypt info from checkpoint.
@return a crypt info or NULL if not present. */
static
const crypt_info_t*
get_crypt_info(
/*===========*/
	ib_uint64_t checkpoint_no)
{
	/* so that no one is modifying array while we search */
	ut_ad(mutex_own(&(log_sys->mutex)));
	size_t items = crypt_info.size();

	/* a log block only stores 4-bytes of checkpoint no */
	checkpoint_no &= 0xFFFFFFFF;
	for (size_t i = 0; i < items; i++) {
		struct crypt_info_t* it = &crypt_info[i];

		if (it->checkpoint_no == checkpoint_no) {
			return it;
		}
	}

	/* If checkpoint contains more than one key and we did not
	find the correct one use the first one. */
	if (items) {
		return (&crypt_info[0]);
	}

	return NULL;
}

/*********************************************************************//**
Get crypt info from log block
@return a crypt info or NULL if not present. */
static
const crypt_info_t*
get_crypt_info(
/*===========*/
	const byte* log_block)
{
	ib_uint64_t checkpoint_no = log_block_get_checkpoint_no(log_block);
	return get_crypt_info(checkpoint_no);
}

/*********************************************************************//**
Print checkpoint no from log block and all encryption keys from
checkpoints if they are present. Used for problem analysis. */
void
log_crypt_print_checkpoint_keys(
/*============================*/
	const byte* log_block)
{
	ib_uint64_t checkpoint_no = log_block_get_checkpoint_no(log_block);

	if (crypt_info.size()) {
		fprintf(stderr,
			"InnoDB: redo log checkpoint: " UINT64PF " [ chk key ]: ",
			checkpoint_no);
		for (size_t i = 0; i < crypt_info.size(); i++) {
			struct crypt_info_t* it = &crypt_info[i];
			fprintf(stderr, "[ " UINT64PF " %u ] ",
				it->checkpoint_no,
				it->key_version);
		}
		fprintf(stderr, "\n");
	}
}

/*********************************************************************//**
Call AES CTR to encrypt/decrypt log blocks. */
static
Crypt_result
log_blocks_crypt(
/*=============*/
	const byte* block,	/*!< in: blocks before encrypt/decrypt*/
	lsn_t lsn,		/*!< in: log sequence number of the start
				of the buffer */
	ulint size,		/*!< in: size of block */
	byte* dst_block,	/*!< out: blocks after encrypt/decrypt */
	int what,		/*!< in: encrypt or decrypt*/
	const crypt_info_t* crypt_info) /*!< in: crypt info or NULL */
{
	byte *log_block = (byte*)block;
	Crypt_result rc = MY_AES_OK;
	uint dst_len;
	byte aes_ctr_counter[MY_AES_BLOCK_SIZE];

	const uint src_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE;
	for (ulint i = 0; i < size ; i += OS_FILE_LOG_BLOCK_SIZE,
	     lsn += OS_FILE_LOG_BLOCK_SIZE) {
		ulint log_block_no = log_block_get_hdr_no(log_block);

		const crypt_info_t* info = crypt_info == NULL ? get_crypt_info(log_block) :
			crypt_info;
#ifdef DEBUG_CRYPT
		fprintf(stderr,
			"%s %lu chkpt: %lu key: %u lsn: %lu\n",
			what == ENCRYPTION_FLAG_ENCRYPT ? "crypt" : "decrypt",
			log_block_no,
			log_block_get_checkpoint_no(log_block),
			info ? info->key_version : 0,
			log_block_start_lsn);
#endif
		/* If no key is found from checkpoint assume the log_block
		to be unencrypted. If checkpoint contains the encryption key
		compare log_block current checksum, if checksum matches,
		block can't be encrypted. */
		if (info == NULL ||
		    info->key_version == UNENCRYPTED_KEY_VER ||
			(log_block_checksum_is_ok_or_old_format(log_block, false) &&
			 what == ENCRYPTION_FLAG_DECRYPT)) {
			memcpy(dst_block, log_block, OS_FILE_LOG_BLOCK_SIZE);
			goto next;
		}

		ut_ad(what == ENCRYPTION_FLAG_DECRYPT ? !log_block_checksum_is_ok_or_old_format(log_block, false) :
			log_block_checksum_is_ok_or_old_format(log_block, false));

		// Assume log block header is not encrypted
		memcpy(dst_block, log_block, LOG_BLOCK_HDR_SIZE);

		// aes_ctr_counter = nonce(3-byte) + start lsn to a log block
		// (8-byte) + lbn (4-byte) + abn
		// (1-byte, only 5 bits are used). "+" means concatenate.
		bzero(aes_ctr_counter, MY_AES_BLOCK_SIZE);
		memcpy(aes_ctr_counter, info->crypt_nonce, 3);
		mach_write_to_8(aes_ctr_counter + 3, lsn);
		mach_write_to_4(aes_ctr_counter + 11, log_block_no);
		bzero(aes_ctr_counter + 15, 1);

		int rc;
		rc = encryption_crypt(log_block + LOG_BLOCK_HDR_SIZE, src_len,
					dst_block + LOG_BLOCK_HDR_SIZE, &dst_len,
					(unsigned char*)(info->crypt_key), 16,
					aes_ctr_counter, MY_AES_BLOCK_SIZE,
					what | ENCRYPTION_FLAG_NOPAD,
					LOG_DEFAULT_ENCRYPTION_KEY,
					info->key_version);

		ut_a(rc == MY_AES_OK);
		ut_a(dst_len == src_len);
next:
		log_block += OS_FILE_LOG_BLOCK_SIZE;
		dst_block += OS_FILE_LOG_BLOCK_SIZE;
	}

	return rc;
}

/** Encrypt/decrypt temporary log blocks.

@param[in]	src_block	block to encrypt or decrypt
@param[in]	size		size of the block
@param[out]	dst_block	destination block
@param[in]	what		ENCRYPTION_FLAG_ENCRYPT or
				ENCRYPTION_FLAG_DECRYPT
@param[in]	offs		offset to block
@param[in]	space_id	tablespace id
@return true if successful, false in case of failure
*/
static
bool
log_tmp_blocks_crypt(
	const byte*		src_block,
	ulint			size,
	byte*			dst_block,
	int			what,
	os_offset_t		offs,
	ulint			space_id)
{
	Crypt_result rc = MY_AES_OK;
	uint dst_len;
	byte aes_ctr_counter[MY_AES_BLOCK_SIZE];
	byte is_encrypt= what == ENCRYPTION_FLAG_ENCRYPT;
	const crypt_info_t* info = static_cast<const crypt_info_t*>(&crypt_info[0]);

	// AES_CTR_COUNTER = space_id + offs

	bzero(aes_ctr_counter, MY_AES_BLOCK_SIZE);
	mach_write_to_8(aes_ctr_counter, space_id);
	mach_write_to_8(aes_ctr_counter + 8, offs);

	rc = encryption_crypt(src_block, size,
				dst_block, &dst_len,
				(unsigned char*)(info->crypt_key), 16,
				aes_ctr_counter, MY_AES_BLOCK_SIZE,
				what | ENCRYPTION_FLAG_NOPAD,
				LOG_DEFAULT_ENCRYPTION_KEY,
				info->key_version);

	if (rc != MY_AES_OK) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"%s failed for temporary log file with rc = %d",
			is_encrypt ? "Encryption" : "Decryption",
			rc);
		return false;
	}

	return true;
}

/** Get crypt info
@return pointer to log crypt info or NULL
*/
inline
const crypt_info_t*
get_crypt_info()
{
	mutex_enter(&log_sys->mutex);
	const crypt_info_t* info = get_crypt_info(log_sys->next_checkpoint_no);
	mutex_exit(&log_sys->mutex);

	return info;
}

/** Find out is temporary log files encrypted.
@return true if temporary log file should be encrypted, false if not */
UNIV_INTERN
bool
log_tmp_is_encrypted()
{
	const crypt_info_t* info = get_crypt_info();

	if (info == NULL || info->key_version == UNENCRYPTED_KEY_VER) {
		return false;
	}

	return true;
}

/** Encrypt temporary log block.
@param[in]	src_block	block to encrypt or decrypt
@param[in]	size		size of the block
@param[out]	dst_block	destination block
@param[in]	offs		offset to block
@param[in]	space_id	tablespace id
@return true if successfull, false in case of failure
*/
UNIV_INTERN
bool
log_tmp_block_encrypt(
	const byte*		src_block,
	ulint			size,
	byte*			dst_block,
	os_offset_t		offs,
	ulint			space_id)
{
	return (log_tmp_blocks_crypt(src_block, size, dst_block,
				ENCRYPTION_FLAG_ENCRYPT, offs, space_id));
}

/** Decrypt temporary log block.
@param[in]	src_block	block to encrypt or decrypt
@param[in]	size		size of the block
@param[out]	dst_block	destination block
@param[in]	offs		offset to block
@param[in]	space_id	tablespace id
@return true if successfull, false in case of failure
*/
UNIV_INTERN
bool
log_tmp_block_decrypt(
	const byte*		src_block,
	ulint			size,
	byte*			dst_block,
	os_offset_t		offs,
	ulint			space_id)
{
	return (log_tmp_blocks_crypt(src_block, size, dst_block,
				ENCRYPTION_FLAG_DECRYPT, offs, space_id));
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

	byte mysqld_key[MY_AES_MAX_KEY_LENGTH] = {0};
	uint keylen= sizeof(mysqld_key);
	uint rc;

	rc = encryption_key_get(LOG_DEFAULT_ENCRYPTION_KEY, info->key_version, mysqld_key, &keylen);

	if (rc) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Redo log crypto: getting mysqld crypto key "
			"from key version failed err = %u. Reason could be that requested"
			" key_version %u is not found or required encryption "
			" key management is not found.", rc, info->key_version);
		return false;
	}

	uint dst_len;
	int err= my_aes_crypt(MY_AES_ECB, ENCRYPTION_FLAG_NOPAD|ENCRYPTION_FLAG_ENCRYPT,
                             info->crypt_msg, sizeof(info->crypt_msg), //src, srclen
                             info->crypt_key, &dst_len, //dst, &dstlen
                             (unsigned char*)&mysqld_key, sizeof(mysqld_key),
                             NULL, 0);

	if (err != MY_AES_OK || dst_len != MY_AES_BLOCK_SIZE) {
		fprintf(stderr,
			"\nInnodb redo log crypto: getting redo log crypto key "
			"failed err = %d len = %u.\n", err, dst_len);
		return false;
	}

	return true;
}

/*********************************************************************//**
Compare function for checkpoint numbers
@return true if first checkpoint is larger than second one */
static
bool
mysort(const crypt_info_t& i,
       const crypt_info_t& j)
{
	return i.checkpoint_no > j.checkpoint_no;
}

/*********************************************************************//**
Add crypt info to set if it is not already present
@return true if successfull, false if not- */
static
bool
add_crypt_info(
/*===========*/
	crypt_info_t*	info,		/*!< in: crypt info */
	bool		checkpoint_read)/*!< in: do we read checkpoint */
{
	const crypt_info_t* found=NULL;
	/* so that no one is searching array while we modify it */
	ut_ad(mutex_own(&(log_sys->mutex)));

	found = get_crypt_info(info->checkpoint_no);

	/* If one crypt info is found then we add a new one only if we
	are reading checkpoint from the log. New checkpoints will always
	use the first created crypt info. */
	if (found != NULL &&
		( found->checkpoint_no == info->checkpoint_no || !checkpoint_read)) {
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

	add_crypt_info(&info, false);
}

/********************************************************
Encrypt one or more log block before it is flushed to disk */
UNIV_INTERN
void
log_encrypt_before_write(
/*=====================*/
	ib_uint64_t next_checkpoint_no,	/*!< in: log group to be flushed */
	byte* block,			/*!< in/out: pointer to a log block */
	lsn_t lsn,			/*!< in: log sequence number of
					the start of the buffer */
	const ulint size)		/*!< in: size of log blocks */
{
	ut_ad(size % OS_FILE_LOG_BLOCK_SIZE == 0);

	const crypt_info_t* info = get_crypt_info(next_checkpoint_no);
	if (info == NULL) {
		return;
	}

	/* If the key is not encrypted or user has requested not to
	encrypt, do not change log block. */
	if (info->key_version == UNENCRYPTED_KEY_VER || !srv_encrypt_log) {
		return;
	}

	byte* dst_frame = (byte*)malloc(size);

	//encrypt log blocks content
	Crypt_result result = log_blocks_crypt(
		block, lsn, size, dst_frame, ENCRYPTION_FLAG_ENCRYPT, NULL);

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
/*===================*/
	byte* frame,	        /*!< in/out: log segment */
	lsn_t lsn,		/*!< in: log sequence number of the start
				of the buffer */
	const ulint size)	/*!< in: log segment size */
{
	ut_ad(size % OS_FILE_LOG_BLOCK_SIZE == 0);
	byte* dst_frame = (byte*)malloc(size);

	// decrypt log blocks content
	Crypt_result result = log_blocks_crypt(
		frame, lsn, size, dst_frame, ENCRYPTION_FLAG_DECRYPT, NULL);

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

		if (!add_crypt_info(&info, true)) {
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

/********************************************************
Check is the checkpoint information encrypted. This check
is based on fact has log group crypt info and based
on this crypt info was the key version different from
unencrypted key version. There is no realible way to
distinguish encrypted log block from corrupted log block,
but if log block corruption is found this function is
used to find out if log block is maybe encrypted but
encryption key, key management plugin or encryption
algorithm does not match.
@return TRUE, if log block may be encrypted */
UNIV_INTERN
ibool
log_crypt_block_maybe_encrypted(
/*============================*/
	const byte* 		log_block,	/*!< in: log block */
	log_crypt_err_t* 	err_info)	/*!< out: error info */
{
	ibool maybe_encrypted = FALSE;
	const crypt_info_t* crypt_info;

	*err_info = LOG_UNENCRYPTED;
	crypt_info = get_crypt_info(log_block);

	if (crypt_info &&
	    crypt_info->key_version != UNENCRYPTED_KEY_VER) {
		byte mysqld_key[MY_AES_BLOCK_SIZE] = {0};
		uint keylen= sizeof(mysqld_key);

		/* Log block contains crypt info and based on key
		version block could be encrypted. */
		*err_info = LOG_DECRYPT_MAYBE_FAILED;
		maybe_encrypted = TRUE;

		if (encryption_key_get(LOG_DEFAULT_ENCRYPTION_KEY,
				       crypt_info->key_version, mysqld_key, &keylen)) {
			*err_info = LOG_CRYPT_KEY_NOT_FOUND;
		}
	}

	return (maybe_encrypted);
}

/********************************************************
Print crypt error message to error log */
UNIV_INTERN
void
log_crypt_print_error(
/*==================*/
	log_crypt_err_t 	err_info) 	/*!< out: error info */
{
	switch(err_info) {
	case LOG_CRYPT_KEY_NOT_FOUND:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Redo log crypto: getting mysqld crypto key "
			"from key version failed. Reason could be that "
			"requested key version is not found or required "
			"encryption key management plugin is not found.");
		break;
	case LOG_DECRYPT_MAYBE_FAILED:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Redo log crypto: failed to decrypt log block. "
			"Reason could be that requested key version is "
			"not found, required encryption key management "
			"plugin is not found or configured encryption "
			"algorithm and/or method does not match.");
		break;
	default:
		ut_error; /* Real bug */
	}
}
