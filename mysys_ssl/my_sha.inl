/* Copyright (c) 2012, Oracle and/or its affiliates.
   Copyright (c) 2014, 2017, MariaDB

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
  Wrapper functions for OpenSSL, YaSSL implementations. Also provides a
  Compatibility layer to make available YaSSL's SHAn implementation.
*/

#include <my_global.h>
#include <stdarg.h>

#define HASH_SIZE (NUM > 1 ? NUM/8 : 20)

#if defined(HAVE_WOLFSSL)
#define WOLFSSL_SHA512
#define WOLFSSL_SHA384
#define WOLFSSL_SHA224
#include <wolfcrypt/sha.h>
#include <wolfcrypt/sha256.h>
#include <wolfcrypt/sha512.h>
#define xCONTEXT(x)     wc_Sha ## x
#define yCONTEXT(y)     xCONTEXT(y)
#define CONTEXT         yCONTEXT(NUM)
#define wc_InitSha1     wc_InitSha
#define wc_Sha1Final    wc_ShaFinal
#define wc_Sha1Update   wc_ShaUpdate
#define wc_Sha1         wc_Sha
#define SHA224_CTX      SHA256_CTX
#define SHA384_CTX      SHA512_CTX

#define xSHA_Init(x)    wc_InitSha ## x
#define xSHA_Update(x)  wc_Sha ## x ## Update
#define xSHA_Final(x)   wc_Sha ## x ## Final
#define ySHA_Init(y)    xSHA_Init(y)
#define ySHA_Update(y)  xSHA_Update(y)
#define ySHA_Final(y)   xSHA_Final(y)
#define SHA_Init        ySHA_Init(NUM)
#define SHA_Update      ySHA_Update(NUM)
#define SHA_Final       ySHA_Final(NUM)
static void sha_init(CONTEXT *context)
{
  SHA_Init(context);
}

static void sha_init_fast(CONTEXT *context)
{
  sha_init(context);
}

static void sha_input(CONTEXT *context, const uchar *buf, unsigned len)
{
  SHA_Update(context, buf, len);
}

static void sha_result(CONTEXT *context, uchar digest[HASH_SIZE])
{
  SHA_Final(context, digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/sha.h>

#define xCONTEXT(x)     SHA ## x ## _CTX
#define yCONTEXT(y)     xCONTEXT(y)
#define CONTEXT         yCONTEXT(NUM)
#define SHA1_CTX        SHA_CTX
#define SHA224_CTX      SHA256_CTX
#define SHA384_CTX      SHA512_CTX

#define xSHA_Init(x)    SHA ## x ## _Init
#define xSHA_Update(x)  SHA ## x ## _Update
#define xSHA_Final(x)   SHA ## x ## _Final
#define ySHA_Init(y)    xSHA_Init(y)
#define ySHA_Update(y)  xSHA_Update(y)
#define ySHA_Final(y)   xSHA_Final(y)
#define SHA_Init        ySHA_Init(NUM)
#define SHA_Update      ySHA_Update(NUM)
#define SHA_Final       ySHA_Final(NUM)

static void sha_init(CONTEXT *context)
{
  SHA_Init(context);
}

static void sha_init_fast(CONTEXT *context)
{
  sha_init(context);
}

static void sha_input(CONTEXT *context, const uchar *buf, unsigned len)
{
  SHA_Update(context, buf, len);
}

static void sha_result(CONTEXT *context, uchar digest[HASH_SIZE])
{
  SHA_Final(digest, context);
}

#endif /* HAVE_WOLFSSL */

#define xmy_sha_multi(x)         my_sha ## x ## _multi
#define xmy_sha_context_size(x)  my_sha ## x ## _context_size
#define xmy_sha_init(x)          my_sha ## x ## _init
#define xmy_sha_input(x)         my_sha ## x ## _input
#define xmy_sha_result(x)        my_sha ## x ## _result
#define xmy_sha(x)               my_sha ## x
#define ymy_sha_multi(y)         xmy_sha_multi(y)
#define ymy_sha_context_size(y)  xmy_sha_context_size(y)
#define ymy_sha_init(y)          xmy_sha_init(y)
#define ymy_sha_input(y)         xmy_sha_input(y)
#define ymy_sha_result(y)        xmy_sha_result(y)
#define ymy_sha(y)               xmy_sha(y)
#define my_sha_multi             ymy_sha_multi(NUM)
#define my_sha_context_size      ymy_sha_context_size(NUM)
#define my_sha_init              ymy_sha_init(NUM)
#define my_sha_input             ymy_sha_input(NUM)
#define my_sha_result            ymy_sha_result(NUM)
#define my_sha                   ymy_sha(NUM)

/**
  Wrapper function to compute SHAn message digest.

  @param digest [out]  Computed SHAn digest
  @param buf    [in]   Message to be computed
  @param len    [in]   Length of the message

  @return              void
*/
void my_sha(uchar *digest, const char *buf, size_t len)
{
  CONTEXT context;

  sha_init_fast(&context);
  sha_input(&context, (const uchar *)buf, (unsigned int)len);
  sha_result(&context, digest);
}


/**
  Wrapper function to compute SHAn message digest for
  two messages in order to emulate shaN(msg1, msg2).

  @param digest [out]  Computed SHAn digest
  @param buf1   [in]   First message
  @param len1   [in]   Length of first message
  @param buf2   [in]   Second message
  @param len2   [in]   Length of second message

  @return              void
*/
void my_sha_multi(uchar *digest, ...)
{
  va_list args;
  va_start(args, digest);

  CONTEXT context;
  const uchar *str;

  sha_init_fast(&context);
  for (str= va_arg(args, const uchar*); str; str= va_arg(args, const uchar*))
    sha_input(&context, str, (uint) va_arg(args, size_t));

  sha_result(&context, digest);
  va_end(args);
}

size_t my_sha_context_size()
{
  return sizeof(CONTEXT);
}

void my_sha_init(void *context)
{
  sha_init((CONTEXT *)context);
}

void my_sha_input(void *context, const uchar *buf, size_t len)
{
  sha_input((CONTEXT *)context, buf, (uint) len);
}

void my_sha_result(void *context, uchar *digest)
{
  sha_result((CONTEXT *)context, digest);
}
