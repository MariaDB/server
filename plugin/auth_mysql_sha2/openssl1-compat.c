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

#include "mysql_sha2.h"
#include <openssl/rsa.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
EVP_PKEY *EVP_RSA_gen(unsigned int bits)
{
  EVP_PKEY_CTX *ctx;
  EVP_PKEY *pkey = NULL;
  ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  if (!ctx)
    return NULL;

  if (EVP_PKEY_keygen_init(ctx) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0)
    goto err;

  EVP_PKEY_keygen(ctx, &pkey);

err:
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}
#endif
