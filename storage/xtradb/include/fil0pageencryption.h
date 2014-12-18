/*****************************************************************************

Copyright (C) 2014 eperi GmbH. All Rights Reserved.

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

#ifndef fil0pageencryption_h
#define fil0pageencryption_h

#define PAGE_ENCRYPTION_WRONG_KEY 1
#define PAGE_ENCRYPTION_WRONG_PAGE_TYPE 2
#define PAGE_ENCRYPTION_ERROR 3
#define PAGE_ENCRYPTION_KEY_MISSING  4
#define PAGE_ENCRYPTION_OK 0
#define PAGE_ENCRYPTION_WILL_NOT_ENCRYPT  5

#include "fsp0fsp.h"
#include "fsp0pageencryption.h"

/******************************************************************//**
@file include/fil0pageencryption.h
Helper functions for encryption/decryption page data on to table space.

Created 08/25/2014
***********************************************************************/


/******************************PAGE_ENCRYPTION_ERROR*************************************//**
Returns the page encryption flag of the space, or false if the space
is not encrypted. The tablespace must be cached in the memory cache.
@return	true if page encrypted, false if not or space not found */
ibool
fil_space_is_page_encrypted(
/*=========================*/
    ulint   id);	/*!< in: space id */


/*******************************************************************//**
Find out whether the page is page encrypted
@return	true if page is page encrypted, false if not */
UNIV_INLINE
ibool
fil_page_is_encrypted(
/*===================*/
    const byte *buf);	/*!< in: page */


/*******************************************************************//**
Find out whether the page can be decrypted
@return	true if page can be decrypted, false if not. */
UNIV_INLINE
ulint
fil_page_encryption_status(
/*===================*/
    const byte *buf);	/*!< in: page */


/****************************************************************//**
For page encrypted pages encrypt the page before actual write
operation.
@return encrypted page to be written*/
byte*
fil_encrypt_page(
/*==============*/
	ulint 		space_id, 	/*!< in: tablespace id of the table. */
	byte* 		buf, 		/*!< in: buffer from which to write; in aio
					this must be appropriately aligned */
	byte* 		out_buf, 	/*!< out: encrypted buffer */
	ulint 		len, 		/*!< in: length of input buffer.*/
	ulint 		encryption_key, /*!< in: encryption key */
	ulint* 		out_len, 	/*!< out: actual length of encrypted page */
	ulint* 		errorCode, 	/*!< out: an error code. set, if page is intentionally not encrypted */
	byte*  		tmp_encryption_buf); /*!< in: temporary buffer or NULL */

/****************************************************************//**
For page encrypted pages decrypt the page after actual read
operation.
@return decrypted page */
ulint
fil_decrypt_page(
/*================*/
	byte* 		page_buf, 	/*!< in: preallocated buffer or NULL */
	byte* 		buf, 		/*!< in/out: buffer from which to read; in aio
					this must be appropriately aligned */
	ulint 		len, 		/*!< in: length buffer, which should be decrypted.*/
	ulint* 		write_size, 	/*!< out: size of the decrypted data. If no error occurred equal to len */
	ibool* 		page_compressed,/*!<out: is page compressed.*/
	byte*  		tmp_encryption_buf); /*!< in: temporary buffer or NULL */

#endif // fil0pageencryption_h
