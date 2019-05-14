/* Copyright (c) 2012, Oracle and/or its affiliates.
   Copyright (c) 2017, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


/**
  @file

  @brief
  Wrapper functions for OpenSSL and YaSSL. Also provides a Compatibility layer
  to make available YaSSL's MD5 implementation.
*/

#include <my_global.h>
#include <my_md5.h>
#include <stdarg.h>

#if defined(HAVE_YASSL)
#include "md5.hpp"
#include <ssl_compat.h>

typedef TaoCrypt::MD5 EVP_MD_CTX;

static void md5_init(EVP_MD_CTX *context)
{
  context= new(context) EVP_MD_CTX;
  context->Init();
}

static void md5_input(EVP_MD_CTX *context, const uchar *buf, unsigned len)
{
  context->Update((const TaoCrypt::byte *) buf, len);
}

static void md5_result(EVP_MD_CTX *context, uchar digest[MD5_HASH_SIZE])
{
    context->Final((TaoCrypt::byte *) digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/evp.h>
#include <ssl_compat.h>

static void md5_init(EVP_MD_CTX *context)
{
  EVP_MD_CTX_init(context);
#ifdef EVP_MD_CTX_FLAG_NON_FIPS_ALLOW
  /* Ok to ignore FIPS: MD5 is not used for crypto here */
  EVP_MD_CTX_set_flags(context, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
  EVP_DigestInit_ex(context, EVP_md5(), NULL);
}

static void md5_input(EVP_MD_CTX *context, const uchar *buf, unsigned len)
{
  EVP_DigestUpdate(context, buf, len);
}

static void md5_result(EVP_MD_CTX *context, uchar digest[MD5_HASH_SIZE])
{
  EVP_DigestFinal_ex(context, digest, NULL);
  EVP_MD_CTX_reset(context);
}

#endif /* HAVE_YASSL */

/**
  Wrapper function to compute MD5 message digest.

  @param digest [out]  Computed MD5 digest
  @param buf    [in]   Message to be computed
  @param len    [in]   Length of the message

  @return              void
*/
void my_md5(uchar *digest, const char *buf, size_t len)
{
  char ctx_buf[EVP_MD_CTX_SIZE];
  EVP_MD_CTX * const ctx= (EVP_MD_CTX*)ctx_buf;
  md5_init(ctx);
  md5_input(ctx, (const uchar *)buf, (uint) len);
  md5_result(ctx, digest);
}


/**
  Wrapper function to compute MD5 message digest for
  many messages, concatenated.

  @param digest [out]  Computed MD5 digest
  @param buf1   [in]   First message
  @param len1   [in]   Length of first message
         ...
  @param bufN   [in]   NULL terminates the list of buf,len pairs.

  @return              void
*/
void my_md5_multi(uchar *digest, ...)
{
  va_list args;
  const uchar *str;
  char ctx_buf[EVP_MD_CTX_SIZE];
  EVP_MD_CTX * const ctx= (EVP_MD_CTX*)ctx_buf;
  va_start(args, digest);

  md5_init(ctx);
  for (str= va_arg(args, const uchar*); str; str= va_arg(args, const uchar*))
    md5_input(ctx, str, (uint) va_arg(args, size_t));

  md5_result(ctx, digest);
  va_end(args);
}

size_t my_md5_context_size()
{
  return EVP_MD_CTX_SIZE;
}

void my_md5_init(void *context)
{
  md5_init((EVP_MD_CTX *)context);
}

void my_md5_input(void *context, const uchar *buf, size_t len)
{
  md5_input((EVP_MD_CTX *)context, buf, (uint) len);
}

void my_md5_result(void *context, uchar *digest)
{
  md5_result((EVP_MD_CTX *)context, digest);
}
