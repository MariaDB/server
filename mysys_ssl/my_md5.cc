/* Copyright (c) 2012, Oracle and/or its affiliates.
   Copyright (c) 2014, SkySQL Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */


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

typedef TaoCrypt::MD5 MD5_CONTEXT;

static void md5_init(MD5_CONTEXT *context)
{
  context= new(context) MD5_CONTEXT;
  context->Init();
}

/*
  this is a variant of md5_init to be used in this file only.
  does nothing for yassl, because the context's constructor was called automatically.
*/
static void md5_init_fast(MD5_CONTEXT *context)
{
}

static void md5_input(MD5_CONTEXT *context, const uchar *buf, unsigned len)
{
  context->Update((const TaoCrypt::byte *) buf, len);
}

static void md5_result(MD5_CONTEXT *context, uchar digest[MD5_HASH_SIZE])
{
    context->Final((TaoCrypt::byte *) digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/evp.h>
typedef EVP_MD_CTX MD5_CONTEXT;

static void md5_init(MD5_CONTEXT *context)
{
  EVP_MD_CTX_init(context);
#ifdef EVP_MD_CTX_FLAG_NON_FIPS_ALLOW
  /* Ok to ignore FIPS: MD5 is not used for crypto here */
  EVP_MD_CTX_set_flags(context, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
  EVP_DigestInit_ex(context, EVP_md5(), NULL);
}

static void md5_init_fast(MD5_CONTEXT *context)
{
  md5_init(context);
}

static void md5_input(MD5_CONTEXT *context, const uchar *buf, unsigned len)
{
  EVP_DigestUpdate(context, buf, len);
}

static void md5_result(MD5_CONTEXT *context, uchar digest[MD5_HASH_SIZE])
{
  EVP_DigestFinal_ex(context, digest, NULL);
  EVP_MD_CTX_cleanup(context);
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
  MD5_CONTEXT md5_context;

  md5_init_fast(&md5_context);
  md5_input(&md5_context, (const uchar *)buf, len);
  md5_result(&md5_context, digest);
}


/**
  Wrapper function to compute MD5 message digest for
  two messages in order to emulate md5(msg1, msg2).

  @param digest [out]  Computed MD5 digest
  @param buf1   [in]   First message
  @param len1   [in]   Length of first message
  @param buf2   [in]   Second message
  @param len2   [in]   Length of second message

  @return              void
*/
void my_md5_multi(uchar *digest, ...)
{
  va_list args;
  va_start(args, digest);

  MD5_CONTEXT md5_context;
  const uchar *str;

  md5_init_fast(&md5_context);
  for (str= va_arg(args, const uchar*); str; str= va_arg(args, const uchar*))
    md5_input(&md5_context, str, va_arg(args, size_t));

  md5_result(&md5_context, digest);
  va_end(args);
}

size_t my_md5_context_size()
{
  return sizeof(MD5_CONTEXT);
}

void my_md5_init(void *context)
{
  md5_init((MD5_CONTEXT *)context);
}

void my_md5_input(void *context, const uchar *buf, size_t len)
{
  md5_input((MD5_CONTEXT *)context, buf, len);
}

void my_md5_result(void *context, uchar *digest)
{
  md5_result((MD5_CONTEXT *)context, digest);
}
