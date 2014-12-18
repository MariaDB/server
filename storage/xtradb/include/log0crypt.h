/**************************************************//**
@file include/log0crypt.h
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu
*******************************************************/
#ifndef log0crypt_h
#define log0crypt_h

#include "univ.i"
#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "my_aes.h"
#include "my_crypt_key_management.h"	// for key version and key 

#define PURPOSE_BYTE_LEN	MY_AES_BLOCK_SIZE - 1
#define PURPOSE_BYTE_OFFSET	0
#define UNENCRYPTED_KEY_VER	0

/* If true, enable redo log encryption. */
extern my_bool srv_encrypt_log;
/* Plain text used by AES_ECB to generate redo log crypt key. */
extern byte redo_log_crypt_msg[MY_AES_BLOCK_SIZE];
/* IV to concatenate with counter used by AES_CTR for redo log crypto. */
extern byte aes_ctr_nonce[MY_AES_BLOCK_SIZE];

/*********************************************************************//**
Generate a 128-bit random message used to generate redo log crypto key.
Init AES-CTR iv/nonce with random number.
It is called only when clean startup (i.e., redo logs do not exist). */
UNIV_INTERN
void
log_init_crypt_msg_and_nonce(void);
/*===============================*/
/*********************************************************************//**
Init log_sys redo log crypto key. */
UNIV_INTERN
void
log_init_crypt_key(
/*===============*/
	const byte* crypt_msg,		/*< in: crypt msg */
	const uint crypt_ver,		/*< in: mysqld key version */
	byte* crypt_key);		/*< out: crypt struct with key and iv */
/*********************************************************************//**
Encrypt log blocks. */
UNIV_INTERN
Crypt_result
log_blocks_encrypt(
/*===============*/
	const byte* blocks,		/*!< in: blocks before encryption */
	const ulint size,		/*!< in: size of blocks, must be multiple of a log block */
	byte* dst_blocks);		/*!< out: blocks after encryption */

/*********************************************************************//**
Decrypt log blocks. */
UNIV_INTERN
Crypt_result
log_blocks_decrypt(
/*===============*/
	const byte* blocks,		/*!< in: blocks before decryption */
	const ulint size,		/*!< in: size of blocks, must be multiple of a log block */
	byte* dst_blocks);		/*!< out: blocks after decryption */

/*********************************************************************//**
Set next checkpoint's key version to latest one, and generate current
key. Key version 0 means no encryption. */
UNIV_INTERN
void
log_crypt_set_ver_and_key(
/*======================*/
	uint& key_ver,			/*!< out: latest key version */
	byte* crypt_key);		/*!< out: crypto key */

/*********************************************************************//**
Writes the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, to a log header's
checkpoint buf. */
UNIV_INTERN
void
log_crypt_write_checkpoint_buf(
/*===========================*/
	byte*	buf);			/*!< in/out: checkpoint buffer */

#endif  // log0crypt.h
