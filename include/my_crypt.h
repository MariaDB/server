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

// TODO: Add Windows support

#ifndef MYSYS_MY_CRYPT_H_
#define MYSYS_MY_CRYPT_H_

#include <my_aes.h>

#if !defined(HAVE_YASSL) && defined(HAVE_OPENSSL)

C_MODE_START
Crypt_result my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);

Crypt_result my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);

Crypt_result my_aes_encrypt_cbc(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);

Crypt_result my_aes_decrypt_cbc(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);

Crypt_result my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);

Crypt_result my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint no_padding);
C_MODE_END

#endif /* !defined(HAVE_YASSL) && defined(HAVE_OPENSSL) */

C_MODE_START
Crypt_result my_random_bytes(uchar* buf, int num);
C_MODE_END

#endif /* MYSYS_MY_CRYPT_H_ */
