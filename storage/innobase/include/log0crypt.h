/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2022, MariaDB Corporation.

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
#pragma once

#include "log0log.h"

/** Initialize the redo log encryption key and random parameters
when creating a new redo log.
The random parameters will be persisted in the log header.
@see log_crypt_write_header()
@see log_crypt_read_header()
@return whether the operation succeeded */
bool log_crypt_init();

/** Add the encryption information to the log header buffer.
@param buf   part of log header buffer */
void log_crypt_write_header(byte *buf);

/** Read the encryption information from a redo log checkpoint buffer.
@param buf   part of checkpoint buffer
@return whether the operation was successful */
bool log_crypt_read_header(const byte *buf);

/** Read the MariaDB 10.1 checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
ATTRIBUTE_COLD bool log_crypt_101_read_checkpoint(const byte* buf);

/** Decrypt a MariaDB 10.1 redo log block.
@param[in,out]	buf		log block
@param[in]	start_lsn	server start LSN
@return	whether the decryption was successful */
ATTRIBUTE_COLD bool log_crypt_101_read_block(byte* buf, lsn_t start_lsn);

/** Read the checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
ATTRIBUTE_COLD bool log_crypt_read_checkpoint_buf(const byte* buf);

/** Decrypt log blocks.
@param[in,out]	buf	log blocks to decrypt
@param[in]	lsn	log sequence number of the start of the buffer
@param[in]	size	size of the buffer, in bytes
@return	whether the operation succeeded */
ATTRIBUTE_COLD bool log_decrypt(byte* buf, lsn_t lsn, ulint size);

/** Decrypt part of a log record.
@param iv    initialization vector
@param buf   buffer for the decrypted data
@param data  the encrypted data
@param len   length of the data, in bytes
@return buf */
byte *log_decrypt_buf(const byte *iv, byte *buf, const byte *data, uint len);

/** Decrypt a log snippet.
@param iv    initialization vector
@param buf   buffer to be replaced with encrypted contents
@param end   pointer past the end of buf */
void log_decrypt_buf(const byte *iv, byte *buf, const byte *const end);

/** Encrypt or decrypt a temporary file block.
@param[in]	src		block to encrypt or decrypt
@param[in]	size		size of the block
@param[out]	dst		destination block
@param[in]	offs		offset to block
@param[in]	encrypt		true=encrypt; false=decrypt
@return whether the operation succeeded */
bool log_tmp_block_encrypt(
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
