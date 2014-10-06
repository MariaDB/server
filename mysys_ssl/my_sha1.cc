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
  Wrapper functions for OpenSSL, YaSSL implementations. Also provides a
  Compatibility layer to make available YaSSL's SHA1 implementation.
*/

#include <my_global.h>
#include <sha1.h>
#include <stdarg.h>

#if defined(HAVE_YASSL)
#include "sha.hpp"

typedef TaoCrypt::SHA SHA_CTX;

static void sha1_init(SHA_CTX *context)
{
  context->Init();
}

/*
  this is a variant of sha1_init to be used in this file only.
  does nothing for yassl, because the context's constructor was called automatically.
*/
static void sha1_init_fast(SHA_CTX *context)
{
}

static void sha1_input(SHA_CTX *context, const uchar *buf, unsigned len)
{
  context->Update((const TaoCrypt::byte *) buf, len);
}

static void sha1_result(SHA_CTX *context, uchar digest[SHA1_HASH_SIZE])
{
    context->Final((TaoCrypt::byte *) digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/sha.h>

static void sha1_init(SHA_CTX *context)
{
  SHA1_Init(context);
}

static void sha1_init_fast(SHA_CTX *context)
{
  sha1_init(context);
}

static void sha1_input(SHA_CTX *context, const uchar *buf, unsigned len)
{
  SHA1_Update(context, buf, len);
}

static void sha1_result(SHA_CTX *context, uchar digest[SHA1_HASH_SIZE])
{
  SHA1_Final(digest, context);
}

#endif /* HAVE_YASSL */

/**
  Wrapper function to compute SHA1 message digest.

  @param digest [out]  Computed SHA1 digest
  @param buf    [in]   Message to be computed
  @param len    [in]   Length of the message

  @return              void
*/
void my_sha1(uchar *digest, const char *buf, size_t len)
{
  SHA_CTX sha1_context;

  sha1_init_fast(&sha1_context);
  sha1_input(&sha1_context, (const uchar *)buf, len);
  sha1_result(&sha1_context, digest);
}


/**
  Wrapper function to compute SHA1 message digest for
  two messages in order to emulate sha1(msg1, msg2).

  @param digest [out]  Computed SHA1 digest
  @param buf1   [in]   First message
  @param len1   [in]   Length of first message
  @param buf2   [in]   Second message
  @param len2   [in]   Length of second message

  @return              void
*/
void my_sha1_multi(uchar *digest, ...)
{
  va_list args;
  va_start(args, digest);

  SHA_CTX sha1_context;
  const uchar *str;

  sha1_init_fast(&sha1_context);
  for (str= va_arg(args, const uchar*); str; str= va_arg(args, const uchar*))
    sha1_input(&sha1_context, str, va_arg(args, size_t));

  sha1_result(&sha1_context, digest);
  va_end(args);
}

size_t my_sha1_context_size()
{
  return sizeof(SHA_CTX);
}

void my_sha1_init(void *context)
{
  sha1_init((SHA_CTX *)context);
}

void my_sha1_input(void *context, const uchar *buf, size_t len)
{
  sha1_input((SHA_CTX *)context, buf, len);
}

void my_sha1_result(void *context, uchar *digest)
{
  sha1_result((SHA_CTX *)context, digest);
}
