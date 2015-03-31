/* Copyright (c) 2002, 2006 MySQL AB, 2009 Sun Microsystems, Inc.
   Copyright (c) 2014, 2015 MariaDB Corporation
   Use is subject to license terms.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* Header file for my_aes.c */
/* Wrapper to give simple interface for MySQL to AES standard encryption */

#ifndef MY_AES_INCLUDED
#define MY_AES_INCLUDED

#include <my_global.h>

#define AES_OK 0
#define AES_BAD_DATA  -1
#define AES_BAD_IV -2
#define AES_INVALID -3
#define AES_OPENSSL_ERROR -4
#define AES_BAD_KEYSIZE -5
#define AES_KEY_CREATION_FAILED -10

#define CRYPT_KEY_OK 0
#define CRYPT_BUFFER_TO_SMALL -11
#define CRYPT_KEY_UNKNOWN -48

/* The block size for all supported algorithms */
#define MY_AES_BLOCK_SIZE 16

/* The max key length of all supported algorithms */
#define MY_AES_MAX_KEY_LENGTH 32


#include "rijndael.h"

C_MODE_START

int my_aes_get_size(int source_length);

C_MODE_END

#endif /* MY_AES_INCLUDED */
