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
#include <ssl_compat.h>

#ifdef HAVE_YASSL

int check_openssl_compatibility()
{
  return 0;
}
#else
#include <openssl/evp.h>

#ifdef HAVE_OPENSSL11
typedef void *(*CRYPTO_malloc_t)(size_t, const char *, int);
#endif

#ifdef HAVE_OPENSSL10
typedef void *(*CRYPTO_malloc_t)(size_t);
#define CRYPTO_malloc   malloc
#define CRYPTO_realloc  realloc
#define CRYPTO_free     free
#endif

static uint allocated_size, allocated_count;

static void *coc_malloc(size_t size)
{
  allocated_size+= size;
  allocated_count++;
  return malloc(size);
}

int check_openssl_compatibility()
{
  EVP_CIPHER_CTX *evp_ctx;
  EVP_MD_CTX     *md5_ctx;

  CRYPTO_set_mem_functions((CRYPTO_malloc_t)coc_malloc, CRYPTO_realloc, CRYPTO_free);

  allocated_size= allocated_count= 0;
  evp_ctx= EVP_CIPHER_CTX_new();
  EVP_CIPHER_CTX_free(evp_ctx);
  if (allocated_count > 1 || allocated_size > EVP_CIPHER_CTX_SIZE)
    return 1;

  allocated_size= allocated_count= 0;
  md5_ctx= EVP_MD_CTX_create();
  EVP_MD_CTX_destroy(md5_ctx);
  if (allocated_count > 1 || allocated_size > EVP_MD_CTX_SIZE)
    return 1;

  CRYPTO_set_mem_functions(CRYPTO_malloc, CRYPTO_realloc, CRYPTO_free);
  return 0;
}
#endif
