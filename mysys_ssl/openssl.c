/*
 Copyright (c) 2017, MariaDB Corporation.

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

#include <my_global.h>

/*
  The check is only done for OpenSSL 1.1.x.
  It could run for OpenSSL 1.0.x but it doesn't make much sense
  and it hits this bug:
  https://bugs.launchpad.net/ubuntu/+source/openssl/+bug/1594748
*/

#ifndef HAVE_OPENSSL11
#include <ssl_compat.h>
int check_openssl_compatibility()
{
  return 0;
}
#else
#include <openssl/evp.h>
#include <ssl_compat.h>
static uint testing, alloc_size, alloc_count;

static void *coc_malloc(size_t size, const char *f __attribute__((unused)),
                                             int l __attribute__((unused)))
{
  if (unlikely(testing))
  {
    alloc_size+= size;
    alloc_count++;
  }
  return malloc(size);
}

int check_openssl_compatibility()
{
  EVP_CIPHER_CTX *evp_ctx;
  EVP_MD_CTX     *md5_ctx;

  if (!CRYPTO_set_mem_functions(coc_malloc, NULL, NULL))
    return 0;

  testing= 1;
  alloc_size= alloc_count= 0;
  evp_ctx= EVP_CIPHER_CTX_new();
  EVP_CIPHER_CTX_free(evp_ctx);
  if (alloc_count != 1 || !alloc_size || alloc_size > EVP_CIPHER_CTX_SIZE)
    return 1;

  alloc_size= alloc_count= 0;
  md5_ctx= EVP_MD_CTX_create();
  EVP_MD_CTX_destroy(md5_ctx);
  if (alloc_count != 1 || !alloc_size || alloc_size > EVP_MD_CTX_SIZE)
    return 1;

  testing= 0;
  return 0;
}
#endif
