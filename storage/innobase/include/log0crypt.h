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
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/
/**************************************************//**
@file include/log0crypt.h
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu
Modified           Jan Lindström jan.lindstrom@mariadb.com
MDEV-11782: Rewritten for MariaDB 10.2 by Marko Mäkelä, MariaDB Corporation.
*******************************************************/
#ifndef log0crypt_h
#define log0crypt_h

#include "log0log.h"

/** innodb_encrypt_log: whether to encrypt the redo log */
extern my_bool srv_encrypt_log;

/** Initialize the redo log encryption key and random parameters
when creating a new redo log.
The random parameters will be persisted in the log checkpoint pages.
@see log_crypt_write_checkpoint_buf()
@see log_crypt_read_checkpoint_buf()
@return whether the operation succeeded */
UNIV_INTERN
bool
log_crypt_init();

/*********************************************************************//**
Writes the crypto (version, msg and iv) info, which has been used for
log blocks with lsn <= this checkpoint's lsn, to a log header's
checkpoint buf. */
UNIV_INTERN
void
log_crypt_write_checkpoint_buf(
/*===========================*/
	byte*	buf);			/*!< in/out: checkpoint buffer */

/** Read the MariaDB 10.1 checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
UNIV_INTERN
bool
log_crypt_101_read_checkpoint(const byte* buf);

/** Decrypt a MariaDB 10.1 redo log block.
@param[in,out]	buf	log block
@return	whether the decryption was successful */
UNIV_INTERN
bool
log_crypt_101_read_block(byte* buf);

/** Read the checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
UNIV_INTERN
bool
log_crypt_read_checkpoint_buf(const byte* buf);

/** Encrypt or decrypt log blocks.
@param[in,out]	buf	log blocks to encrypt or decrypt
@param[in]	lsn	log sequence number of the start of the buffer
@param[in]	size	size of the buffer, in bytes
@param[in]	decrypt	whether to decrypt instead of encrypting */
UNIV_INTERN
void
log_crypt(byte* buf, lsn_t lsn, ulint size, bool decrypt = false);

/** Encrypt or decrypt a temporary file block.
@param[in]	src		block to encrypt or decrypt
@param[in]	size		size of the block
@param[out]	dst		destination block
@param[in]	offs		offset to block
@param[in]	encrypt		true=encrypt; false=decrypt
@return whether the operation succeeded */
UNIV_INTERN
bool
log_tmp_block_encrypt(
	const byte*	src,
	ulint		size,
	byte*		dst,
	uint64_t	offs,
	bool		encrypt = true)
	MY_ATTRIBUTE((warn_unused_result, nonnull));

/** Decrypt a temporary file block.
@param[in]	src		block to decrypt
@param[in]	size		size of the block
@param[out]	dst		destination block
@param[in]	offs		offset to block
@return whether the operation succeeded */
inline
bool
log_tmp_block_decrypt(
	const byte*	src,
	ulint		size,
	byte*		dst,
	uint64_t	offs)
{
	return(log_tmp_block_encrypt(src, size, dst, offs, false));
}

/** @return whether temporary files are encrypted */
inline bool log_tmp_is_encrypted() { return srv_encrypt_log; }
#endif  // log0crypt.h
