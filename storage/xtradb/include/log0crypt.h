/**************************************************//**
@file include/log0crypt.h
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu
*******************************************************/
#ifndef log0crypt_h
#define log0crypt_h

#include "univ.i"
#include "ut0byte.h"
#include "my_crypt.h"

typedef int Crypt_result;

/* If true, enable redo log encryption. */
extern my_bool srv_encrypt_log;

/***********************************************************************
Set next checkpoint's key version to latest one, and generate new key */
UNIV_INTERN
void
log_crypt_set_ver_and_key(
/*======================*/
	ib_uint64_t next_checkpoint_no);


/*********************************************************************//**
Writes the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, to a log header's
checkpoint buf. */
UNIV_INTERN
void
log_crypt_write_checkpoint_buf(
/*===========================*/
	byte*	buf);			/*!< in/out: checkpoint buffer */

/*********************************************************************//**
Read the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, from a log header's
checkpoint buf. */
UNIV_INTERN
bool
log_crypt_read_checkpoint_buf(
/*===========================*/
	const byte*	buf);			/*!< in: checkpoint buffer */

/********************************************************
Encrypt one or more log block before it is flushed to disk */
UNIV_INTERN
void
log_encrypt_before_write(
/*===========================*/
	ib_uint64_t next_checkpoint_no,	/*!< in: log group to be flushed */
	byte* block,			/*!< in/out: pointer to a log block */
	const ulint size);		/*!< in: size of log blocks */

/********************************************************
Decrypt a specified log segment after they are read from a log file to a buffer.
*/
UNIV_INTERN
void
log_decrypt_after_read(
/*==========================*/
	byte* frame,	/*!< in/out: log segment */
	const ulint size);	/*!< in: log segment size */

#endif  // log0crypt.h
