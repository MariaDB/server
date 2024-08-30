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

#if defined(HAVE_WOLFSSL)
#include <wolfssl/wolfcrypt/md5.h>
#include <ssl_compat.h>
typedef wc_Md5 EVP_MD_CTX;
static void md5_init(EVP_MD_CTX *context)
{
  wc_InitMd5(context);;
}

static void md5_input(EVP_MD_CTX *context, const uchar *buf, unsigned len)
{
  wc_Md5Update(context, buf, len);
}

static void md5_result(EVP_MD_CTX *context, uchar digest[MD5_HASH_SIZE])
{
  wc_Md5Final(context,digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/evp.h>
#include <ssl_compat.h>

static void md5_init(EVP_MD_CTX *context)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_MD *md5;
  EVP_MD_CTX_init(context);
  /* Ok to ignore FIPS: MD5 is not used for crypto here */
  /* In OpenSSL 3.0.0+ it is a different EVP_MD provider */
  md5 = EVP_MD_fetch(NULL, "MD5", "fips=no");
  EVP_DigestInit_ex(context, md5, NULL);
  EVP_MD_free(md5);
#else
  EVP_MD_CTX_init(context);
#ifdef EVP_MD_CTX_FLAG_NON_FIPS_ALLOW
  /* Ok to ignore FIPS: MD5 is not used for crypto here */
  /* In OpenSSL 1.1.1 the non FIPS allowed flag is context specific */
  EVP_MD_CTX_set_flags(context, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
  EVP_DigestInit_ex(context, EVP_md5(), NULL);
#endif
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

#endif /* HAVE_WOLFSSL */

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
