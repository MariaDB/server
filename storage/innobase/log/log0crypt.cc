/**************************************************//**
@file log0crypt.cc
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu
*******************************************************/
#include "m_string.h"
#include "log0crypt.h"
#include <my_crypt.h>

#include "log0log.h"
#include "srv0start.h" // for srv_start_lsn
#include "log0recv.h"  // for recv_sys

/* If true, enable redo log encryption. */
UNIV_INTERN my_bool srv_encrypt_log = FALSE;
/*
  Sub system type for InnoDB redo log crypto.
  Set and used to validate crypto msg.
*/
static const byte redo_log_purpose_byte = 0x02;
/* Plain text used by AES_ECB to generate redo log crypt key. */
byte redo_log_crypt_msg[MY_AES_BLOCK_SIZE] = {0};
/* IV to concatenate with counter used by AES_CTR for redo log
 * encryption/decryption. */
byte aes_ctr_nonce[MY_AES_BLOCK_SIZE] = {0};

/*********************************************************************//**
Generate a 128-bit value used to generate crypt key for redo log.
It is generated via the concatenation of 1 purpose byte (0x02) and 15-byte
random number.
Init AES-CTR iv/nonce with random number.
It is called when:
- redo logs do not exist when start up, or
- transition from without crypto.
Note:
We should not use flags and conditions such as:
	(srv_encrypt_log &&
	 debug_use_static_keys &&
	 get_latest_encryption_key_version() == UNENCRYPTED_KEY_VER)
because they haven't been read and set yet in the situation of resetting
redo logs.
*/
UNIV_INTERN
void
log_init_crypt_msg_and_nonce(void)
/*==============================*/
{
	mach_write_to_1(redo_log_crypt_msg, redo_log_purpose_byte);
	if (my_random_bytes(redo_log_crypt_msg + 1, PURPOSE_BYTE_LEN) != AES_OK)
	{
		fprintf(stderr,
			"\nInnodb redo log crypto: generate "
			"%u-byte random number as crypto msg failed.\n",
			PURPOSE_BYTE_LEN);
		abort();
	}

	if (my_random_bytes(aes_ctr_nonce, MY_AES_BLOCK_SIZE) != AES_OK)
	{
		fprintf(stderr,
			"\nInnodb redo log crypto: generate "
			"%u-byte random number as AES_CTR nonce failed.\n",
			MY_AES_BLOCK_SIZE);
		abort();
	}
}

/*********************************************************************//**
Generate crypt key from crypt msg. */
UNIV_INTERN
void
log_init_crypt_key(
/*===============*/
	const byte* crypt_msg,		/*< in: crypt msg */
	const uint crypt_ver,		/*< in: key version */
	byte* key)			/*< out: crypt key*/
{
	if (crypt_ver == UNENCRYPTED_KEY_VER)
	{
		fprintf(stderr, "\nInnodb redo log crypto: unencrypted key ver.\n\n");
		memset(key, 0, MY_AES_BLOCK_SIZE);
		return;
	}

	if (crypt_msg[PURPOSE_BYTE_OFFSET] != redo_log_purpose_byte)
	{
		fprintf(stderr,
			"\nInnodb redo log crypto: msg type mismatched. "
			"Expected: %x; Actual: %x\n",
			redo_log_purpose_byte, crypt_msg[PURPOSE_BYTE_OFFSET]);
		abort();
	}

	byte mysqld_key[MY_AES_BLOCK_SIZE] = {0};
	if (get_encryption_key(crypt_ver, mysqld_key, MY_AES_BLOCK_SIZE))
	{
		fprintf(stderr,
			"\nInnodb redo log crypto: getting mysqld crypto key "
			"from key version failed.\n");
		abort();
	}

	uint32 dst_len;
	my_aes_encrypt_dynamic_type func= get_aes_encrypt_func(MY_AES_ALGORITHM_ECB);
	int rc= (*func)(crypt_msg, MY_AES_BLOCK_SIZE, //src, srclen
                        key, &dst_len, //dst, &dstlen
                        (unsigned char*)&mysqld_key, sizeof(mysqld_key),
                        NULL, 0,
                        1);

	if (rc != AES_OK || dst_len != MY_AES_BLOCK_SIZE)
	{
		fprintf(stderr,
			"\nInnodb redo log crypto: getting redo log crypto key "
			"failed.\n");
		abort();
	}
}

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
Call AES CTR to encrypt/decrypt log blocks. */
static
Crypt_result
log_blocks_crypt(
/*=============*/
	const byte* block,		/*!< in: blocks before encrypt/decrypt*/
	const ulint size,		/*!< in: size of block, must be multiple of a log block*/
	byte* dst_block,		/*!< out: blocks after encrypt/decrypt */
	const bool is_encrypt)		/*!< in: encrypt or decrypt*/
{
	byte *log_block = (byte*)block;
	Crypt_result rc = AES_OK;
	uint32 src_len, dst_len;
	byte aes_ctr_counter[MY_AES_BLOCK_SIZE];
	ulint log_block_no, log_block_start_lsn;
	byte *key;
	ulint lsn;
	if (is_encrypt)
	{
		ut_a(log_sys && log_sys->redo_log_crypt_ver != UNENCRYPTED_KEY_VER);
		key = (byte *)(log_sys->redo_log_crypt_key);
		lsn = log_sys->lsn;

	} else {
		ut_a(recv_sys && recv_sys->recv_log_crypt_ver != UNENCRYPTED_KEY_VER);
		key = (byte *)(recv_sys->recv_log_crypt_key);
		lsn = srv_start_lsn;
	}
	ut_a(size % OS_FILE_LOG_BLOCK_SIZE == 0);
	src_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE;
	for (ulint i = 0; i < size ; i += OS_FILE_LOG_BLOCK_SIZE)
	{
		log_block_no = log_block_get_hdr_no(log_block);
		log_block_start_lsn = log_block_get_start_lsn(lsn, log_block_no);

		// Assume log block header is not encrypted
		memcpy(dst_block, log_block, LOG_BLOCK_HDR_SIZE);

		// aes_ctr_counter = nonce(3-byte) + start lsn to a log block
		// (8-byte) + lbn (4-byte) + abn
		// (1-byte, only 5 bits are used). "+" means concatenate.
		bzero(aes_ctr_counter, MY_AES_BLOCK_SIZE);
		memcpy(aes_ctr_counter, &aes_ctr_nonce, 3);
		mach_write_to_8(aes_ctr_counter + 3, log_block_start_lsn);
		mach_write_to_4(aes_ctr_counter + 11, log_block_no);
		bzero(aes_ctr_counter + 15, 1);

		int rc = (* my_aes_encrypt_dynamic)(log_block + LOG_BLOCK_HDR_SIZE, src_len,
		                                dst_block + LOG_BLOCK_HDR_SIZE, &dst_len,
		                                (unsigned char*)key, 16,
		                                aes_ctr_counter, MY_AES_BLOCK_SIZE,
		                                1);

		ut_a(rc == AES_OK);
		ut_a(dst_len == src_len);
		log_block += OS_FILE_LOG_BLOCK_SIZE;
		dst_block += OS_FILE_LOG_BLOCK_SIZE;
	}

	return rc;
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
Decrypt log blocks. */
UNIV_INTERN
Crypt_result
log_blocks_decrypt(
/*===============*/
	const byte* block,		/*!< in: blocks before decryption */
	const ulint size,		/*!< in: size of blocks, must be multiple of a log block */
	byte* dst_block)		/*!< out: blocks after decryption */
{
	return log_blocks_crypt(block, size, dst_block, false);
}

/*********************************************************************//**
Set next checkpoint's key version to latest one, and generate current
key. Key version 0 means no encryption. */
UNIV_INTERN
void
log_crypt_set_ver_and_key(
/*======================*/
	uint& key_ver,			/*!< out: latest key version */
	byte* crypt_key)		/*!< out: crypto key */
{
	if (!srv_encrypt_log ||
	    (key_ver = get_latest_encryption_key_version()) == UNENCRYPTED_KEY_VER)
	{
		key_ver = UNENCRYPTED_KEY_VER;
		memset(crypt_key, 0, MY_AES_BLOCK_SIZE);
		return;
	}
	log_init_crypt_key(redo_log_crypt_msg, key_ver, crypt_key);
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
	ut_a(log_sys);
	mach_write_to_4(buf + LOG_CRYPT_VER, log_sys->redo_log_crypt_ver);
	if (!srv_encrypt_log ||
	    log_sys->redo_log_crypt_ver == UNENCRYPTED_KEY_VER) {
		memset(buf + LOG_CRYPT_MSG, 0, MY_AES_BLOCK_SIZE);
		memset(buf + LOG_CRYPT_IV, 0, MY_AES_BLOCK_SIZE);
		return;
	}
	ut_a(redo_log_crypt_msg[PURPOSE_BYTE_OFFSET] == redo_log_purpose_byte);
	memcpy(buf + LOG_CRYPT_MSG, redo_log_crypt_msg, MY_AES_BLOCK_SIZE);
	memcpy(buf + LOG_CRYPT_IV, aes_ctr_nonce, MY_AES_BLOCK_SIZE);
}
