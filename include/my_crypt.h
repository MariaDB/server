/*
 Copyright (c) 2014 Google Inc.
 Copyright (c) 2014, 2015 MariaDB Corporation

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

#ifndef MY_CRYPT_INCLUDED
#define MY_CRYPT_INCLUDED

#include <my_global.h>

#ifdef __cplusplus
extern "C" {
#endif

/* return values from my_aes_encrypt/my_aes_decrypt functions */
#define MY_AES_OK               0
#define MY_AES_BAD_DATA         -100
#define MY_AES_OPENSSL_ERROR    -101
#define MY_AES_BAD_KEYSIZE      -102

/* The block size for all supported algorithms */
#define MY_AES_BLOCK_SIZE 16

/* The max key length of all supported algorithms */
#define MY_AES_MAX_KEY_LENGTH 32

#ifdef HAVE_EncryptAes128Ctr

int my_aes_encrypt_ctr(const uchar* source, uint source_length,
                                uchar* dest, uint* dest_length,
                                const uchar* key, uint key_length,
                                const uchar* iv, uint iv_length);

#define my_aes_decrypt_ctr my_aes_encrypt_ctr

#endif

int my_aes_encrypt_cbc(const uchar* source, uint source_length,
                                uchar* dest, uint* dest_length,
                                const uchar* key, uint key_length,
                                const uchar* iv, uint iv_length,
                                int no_padding);

int my_aes_decrypt_cbc(const uchar* source, uint source_length,
                                uchar* dest, uint* dest_length,
                                const uchar* key, uint key_length,
                                const uchar* iv, uint iv_length,
                                int no_padding);

int my_aes_encrypt_ecb(const uchar* source, uint source_length,
                                uchar* dest, uint* dest_length,
                                const uchar* key, uint key_length,
                                const uchar* iv, uint iv_length,
                                int no_padding);

int my_aes_decrypt_ecb(const uchar* source, uint source_length,
                                uchar* dest, uint* dest_length,
                                const uchar* key, uint key_length,
                                const uchar* iv, uint iv_length,
                                int no_padding);

int my_random_bytes(uchar* buf, int num);

int my_aes_get_size(int source_length);

#ifdef __cplusplus
}
#endif

#endif /* MY_CRYPT_INCLUDED */
