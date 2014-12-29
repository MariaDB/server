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

/*******************************************************************//**
Find out whether the page is page encrypted
Returns the page encryption flag of the space, or false if the space
is not encrypted. The tablespace must be cached in the memory cache.
@return	true if page encrypted, false if not or space not found */
ibool
fil_space_is_page_encrypted(
/*========================*/
    ulint   id);	/*!< in: space id */

/*******************************************************************//**
Find out whether the page is page encrypted
@return	true if page is page encrypted, false if not */
UNIV_INLINE
ibool
fil_page_is_encrypted(
/*==================*/
    const byte *buf);	/*!< in: page */

/*******************************************************************//**
Find out whether the page is page compressed and then encrypted
@return	true if page is page compressed+encrypted, false if not */
UNIV_INLINE
ibool
fil_page_is_compressed_encrypted(
/*=============================*/
    const byte *buf);	/*!< in: page */

/*******************************************************************//**
Find out whether the page can be decrypted
@return	true if page can be decrypted, false if not. */
UNIV_INLINE
ulint
fil_page_encryption_status(
/*=======================*/
    const byte *buf);	/*!< in: page */

#endif // fil0pageencryption_h
