#ifndef MYSQL_SERVICE_ENCRYPTION_KEYS_INCLUDED
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
  encryption keys service

  Functions to get encryption keys from the encryption plugin
*/

#ifdef __cplusplus
extern "C" {
#endif

#define BAD_ENCRYPTION_KEY_VERSION (~(unsigned int)0)
#define KEY_BUFFER_TOO_SMALL       (100)

typedef int (*encrypt_decrypt_func)(const unsigned char* src, unsigned int slen,
                                    unsigned char* dst, unsigned int* dlen,
                                    const unsigned char* key, unsigned int klen,
                                    const unsigned char* iv, unsigned int ivlen,
                                    int no_padding, unsigned int key_version);

extern struct encryption_keys_service_st {
  unsigned int (*get_latest_encryption_key_version_func)();
  unsigned int (*has_encryption_key_func)(unsigned int);
  unsigned int (*get_encryption_key_func)(unsigned int, unsigned char*, unsigned int*);
  encrypt_decrypt_func encrypt_data_func;
  encrypt_decrypt_func decrypt_data_func;
} *encryption_keys_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define get_latest_encryption_key_version() encryption_keys_service->get_latest_encryption_key_version_func()
#define has_encryption_key(V) encryption_keys_service->has_encryption_key_func(V)
#define get_encryption_key(V,K,S) encryption_keys_service->get_encryption_key_func((V), (K), (S))
#define encrypt_data(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_keys_service->encrypt_data_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#define decrypt_data(S,SL,D,DL,K,KL,I,IL,NP,KV) encryption_keys_service->decrypt_data_func(S,SL,D,DL,K,KL,I,IL,NP,KV)
#else

unsigned int get_latest_encryption_key_version();
unsigned int has_encryption_key(unsigned int version);
unsigned int get_encryption_key(unsigned int version, unsigned char* key, unsigned int *keybufsize);
int encrypt_data(const unsigned char* src, unsigned int slen,
                 unsigned char* dst, unsigned int* dlen,
                 const unsigned char* key, unsigned int klen,
                 const unsigned char* iv, unsigned int ivlen,
                 int no_padding, unsigned int key_version);
int decrypt_data(const unsigned char* src, unsigned int slen,
                 unsigned char* dst, unsigned int* dlen,
                 const unsigned char* key, unsigned int klen,
                 const unsigned char* iv, unsigned int ivlen,
                 int no_padding, unsigned int key_version);
#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_ENCRYPTION_KEYS_INCLUDED
#endif

