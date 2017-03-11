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

/** Initialize the redo log encryption key.
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
@param[in]	size	size of the buffer, in bytes
@param[in]	decrypt	whether to decrypt instead of encrypting */
UNIV_INTERN
void
log_crypt(byte* buf, ulint size, bool decrypt = false);

#endif  // log0crypt.h
