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

typedef int Crypt_result;

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

/**
  Crypt buffer with AES dynamic (defined at startup) encryption algorithm.

  SYNOPSIS
     my_aes_encrypt_dynamic()
     @param source         [in]  Pointer to data for encryption
     @param source_length  [in]  Size of encryption data
     @param dest           [out] Buffer to place encrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of encrypted data
     @param key            [in]  Key to be used for encryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  if set, algorithm specific padding behaviour is used

  Method used defined by calling my_aes_init_dynamic_encrypt() at startup.

  @return
    != 0           error
    0             no error
*/

typedef int (*my_aes_encrypt_dynamic_type)(const uchar* source, uint32 source_length,
				           uchar* dest, uint32* dest_length,
                                           const uchar* key, uint8 key_length,
                                           const uchar* iv, uint8 iv_length,
                                           uint noPadding);

extern MYSQL_PLUGIN_IMPORT my_aes_encrypt_dynamic_type my_aes_encrypt_dynamic;

/**
  AES decryption AES dynamic (defined at startup) encryption algorithm.

  SYNOPSIS
     my_aes_decrypt_dynamic()
     @param source         [in]  Pointer to data to decrypt
     @param source_length  [in]  Size of data
     @param dest           [out] Buffer to place decrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of decrypted data
     @param key            [in]  Key to be used for decryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  if set, algorithm specific padding behaviour is used

  @return
    != 0           error
    0             no error

  Method used defined by calling my_aes_init_dynamic_encrypt() at startup.
*/

typedef int (*my_aes_decrypt_dynamic_type)(const uchar *source,
                                           uint32 source_length,
					   uchar *dest, uint32 *dest_length,
                                           const uchar *key, uint8 key_length,
                                           const uchar *iv, uint8 iv_length,
                                           uint noPadding);
extern MYSQL_PLUGIN_IMPORT my_aes_decrypt_dynamic_type my_aes_decrypt_dynamic;

/**
   Initialize dynamic crypt functions
*/

enum enum_my_aes_encryption_algorithm
{
  MY_AES_ALGORITHM_NONE, MY_AES_ALGORITHM_ECB, MY_AES_ALGORITHM_CBC,
  MY_AES_ALGORITHM_CTR
};

my_aes_decrypt_dynamic_type get_aes_decrypt_func(enum enum_my_aes_encryption_algorithm method);
my_aes_encrypt_dynamic_type get_aes_encrypt_func(enum enum_my_aes_encryption_algorithm method);


my_bool my_aes_init_dynamic_encrypt(enum enum_my_aes_encryption_algorithm method);

extern MYSQL_PLUGIN_IMPORT enum enum_my_aes_encryption_algorithm current_aes_dynamic_method;

int my_aes_get_size(int source_length);

C_MODE_END

#endif /* MY_AES_INCLUDED */
