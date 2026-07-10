/*
  Copyright (c) 2025, MariaDB plc

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <string.h>
#include <openssl/opensslv.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <mysql/plugin.h>

#define SELF "caching_sha2_password"
#define SHA256CRYPT_LEN 43

extern char *private_key_path, *public_key_path, public_key[1024];
extern size_t public_key_len;
extern EVP_PKEY *private_key;

int ssl_decrypt(EVP_PKEY *pkey, unsigned char *src, size_t srclen,
                unsigned char *dst, size_t *dstlen);
int ssl_genkeys();
int ssl_loadkeys();

void sha256_crypt_r(const unsigned char *key, size_t key_len,
                    const unsigned char *salt, size_t salt_len,
                    unsigned char *buffer, size_t rounds);

#if OPENSSL_VERSION_NUMBER < 0x30000000L
EVP_PKEY *EVP_RSA_gen(unsigned int bits);
#endif
