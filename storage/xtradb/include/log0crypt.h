/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2016, MariaDB Corporation. All Rights Reserved.

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
@file include/log0crypt.h
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu
Modified           Jan Lindström jan.lindstrom@mariadb.com
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
	ib_uint64_t next_checkpoint_no);/*!< in: next checkpoint no */


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
	const byte*	buf);		/*!< in: checkpoint buffer */

/********************************************************
Encrypt one or more log block before it is flushed to disk */
UNIV_INTERN
void
log_encrypt_before_write(
/*=====================*/
	ib_uint64_t	next_checkpoint_no,	/*!< in: log group to be flushed */
	byte* 		block,			/*!< in/out: pointer to a log block */
	const ulint 	size);			/*!< in: size of log blocks */

/********************************************************
Decrypt a specified log segment after they are read from a log file to a buffer.
*/
UNIV_INTERN
void
log_decrypt_after_read(
/*===================*/
	byte*		frame,	/*!< in/out: log segment */
	const ulint	size);	/*!< in: log segment size */

/* Error codes for crypt info */
typedef enum {
	LOG_UNENCRYPTED = 0,
	LOG_CRYPT_KEY_NOT_FOUND = 1,
	LOG_DECRYPT_MAYBE_FAILED = 2
} log_crypt_err_t;

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
	const byte* 		log_block, 	/*!< in: log block */
	log_crypt_err_t* 	err_info); 	/*!< out: error info */

/********************************************************
Print crypt error message to error log */
UNIV_INTERN
void
log_crypt_print_error(
/*==================*/
	log_crypt_err_t 	err_info); 	/*!< out: error info */

/*********************************************************************//**
Print checkpoint no from log block and all encryption keys from
checkpoints if they are present. Used for problem analysis. */
void
log_crypt_print_checkpoint_keys(
/*============================*/
	const byte* log_block);

#endif  // log0crypt.h
