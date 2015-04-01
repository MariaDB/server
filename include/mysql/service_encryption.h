#ifndef MYSQL_SERVICE_ENCRYPTION_INCLUDED
/* Copyright (c) 2015, MariaDB

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

/**
  @file
  encryption service

  Functions to support data encryption and encryption key management.
  They are normally implemented in an encryption plugin, so this service
  connects encryption *consumers* (storage engines) to the encryption
  *provider* (encryption plugin).
*/

#ifdef __cplusplus
extern "C" {
#endif

/* returned from encryption_key_get_latest_version() */
#define ENCRYPTION_KEY_VERSION_INVALID        (~(unsigned int)0)
#define ENCRYPTION_KEY_VERSION_NOT_ENCRYPTED  (0)

/* returned from encryption_key_get()  */
#define ENCRYPTION_KEY_BUFFER_TOO_SMALL    (100)

typedef int (*encrypt_decrypt_func)(const unsigned char* src, unsigned int slen,
                                    unsigned char* dst, unsigned int* dlen,
                                    const unsigned char* key, unsigned int klen,
                                    const unsigned char* iv, unsigned int ivlen,
                                    int no_padding, unsigned int key_version);

struct encryption_service_st {
  unsigned int (*encryption_key_get_latest_version_func)();
  unsigned int (*encryption_key_exists_func)(unsigned int);
  unsigned int (*encryption_key_get_func)(unsigned int, unsigned char*, unsigned int*);
  encrypt_decrypt_func encryption_encrypt_func;
  encrypt_decrypt_func encryption_decrypt_func;
};

#ifdef MYSQL_DYNAMIC_PLUGIN

extern struct encryption_service_st *encryption_service;

#define encryption_key_get_latest_version() encryption_service->encryption_key_get_latest_version_func()
#define encryption_key_exists(V) encryption_service->encryption_key_exists_func(V)
#define encryption_key_get(V,K,S) encryption_service->encryption_key_get_func((V), (K), (S))
#define encryption_encrypt(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_service->encryption_encrypt_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#define encryption_decrypt(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_service->encryption_decrypt_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#else

extern struct encryption_service_st encryption_handler;

#define encryption_key_get_latest_version() encryption_handler.encryption_key_get_latest_version_func()
#define encryption_key_exists(V) encryption_handler.encryption_key_exists_func(V)
#define encryption_key_get(V,K,S) encryption_handler.encryption_key_get_func((V), (K), (S))
#define encryption_encrypt(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_handler.encryption_encrypt_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#define encryption_decrypt(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_handler.encryption_decrypt_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_ENCRYPTION_INCLUDED
#endif

