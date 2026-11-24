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
  Wrapper functions for the OpenSSL/WolfSSL MD5 implementation.
*/

#include <my_global.h>
#include <my_md5.h>
#include <stdarg.h>
#include <ssl_compat.h>

#if defined(HAVE_OPENSSL10) && !defined(HAVE_WOLFSSL) && \
    !defined(LIBRESSL_VERSION_NUMBER)
/*
  OpenSSL 1.0.x in FIPS mode aborts on the low-level MD5 API (md5 is not used
  for crypto here), so go through EVP with the non-FIPS flag.  MDEV-7788.
*/
#include <openssl/evp.h>
typedef EVP_MD_CTX MY_MD5_CTX;
static void md5_init(MY_MD5_CTX *context)
{
  EVP_MD_CTX_init(context);
#ifdef EVP_MD_CTX_FLAG_NON_FIPS_ALLOW
  EVP_MD_CTX_set_flags(context, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
  EVP_DigestInit_ex(context, EVP_md5(), NULL);
}

static void md5_input(MY_MD5_CTX *context, const uchar *buf, unsigned len)
{
  EVP_DigestUpdate(context, buf, len);
}

static void md5_result(MY_MD5_CTX *context, uchar digest[MD5_HASH_SIZE])
{
  EVP_DigestFinal_ex(context, digest, NULL);
  EVP_MD_CTX_cleanup(context);
}

#else
#include <openssl/md5.h>
typedef MD5_CTX MY_MD5_CTX;
static void md5_init(MY_MD5_CTX *context)
{
  MD5_Init(context);
}

static void md5_input(MY_MD5_CTX *context, const uchar *buf, unsigned len)
{
  MD5_Update(context, buf, len);
}

static void md5_result(MY_MD5_CTX *context, uchar digest[MD5_HASH_SIZE])
{
  MD5_Final(digest, context);
}
#endif

/**
  Wrapper function to compute MD5 message digest.

  @param digest [out]  Computed MD5 digest
  @param buf    [in]   Message to be computed
  @param len    [in]   Length of the message

  @return              void
*/
void my_md5(uchar *digest, const char *buf, size_t len)
{
  MY_MD5_CTX context;
  md5_init(&context);
  md5_input(&context, (const uchar *)buf, (uint) len);
  md5_result(&context, digest);
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
  MY_MD5_CTX context;
  va_start(args, digest);

  md5_init(&context);
  for (str= va_arg(args, const uchar*); str; str= va_arg(args, const uchar*))
    md5_input(&context, str, (uint) va_arg(args, size_t));

  md5_result(&context, digest);
  va_end(args);
}

size_t my_md5_context_size()
{
  return sizeof(MY_MD5_CTX);
}

void my_md5_init(void *context)
{
  md5_init((MY_MD5_CTX *)context);
}

void my_md5_input(void *context, const uchar *buf, size_t len)
{
  md5_input((MY_MD5_CTX *)context, buf, (uint) len);
}

void my_md5_result(void *context, uchar *digest)
{
  md5_result((MY_MD5_CTX *)context, digest);
}
