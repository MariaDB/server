/* Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.

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

#if defined(HAVE_YASSL)
#include "my_config.h"
#include "md5.hpp"

static void my_md5_hash(char *digest, const char *buf, int len)
{
  TaoCrypt::MD5 hasher;
  hasher.Update((TaoCrypt::byte *) buf, len);
  hasher.Final((TaoCrypt::byte *) digest);
}

#elif defined(HAVE_OPENSSL)
#include <openssl/md5.h>

static void my_md5_hash(unsigned char* digest, unsigned const char *buf, int len)
{
  MD5_CTX ctx;
  MD5_Init (&ctx);
  MD5_Update (&ctx, buf, len);
  MD5_Final (digest, &ctx);
}

#endif /* HAVE_YASSL */

/**
    Wrapper function to compute MD5 message digest.

    @param digest [out]  Computed MD5 digest
    @param buf    [in]   Message to be computed
    @param len    [in]   Length of the message

    @return              void
*/
void compute_md5_hash(char *digest, const char *buf, int len)
{
#if defined(HAVE_YASSL)
  my_md5_hash(digest, buf, len);
#elif defined(HAVE_OPENSSL)
  my_md5_hash((unsigned char*)digest, (unsigned const char*)buf, len);
#endif /* HAVE_YASSL */
}

#ifdef WITH_WSREP
void *wsrep_md5_init()
{
#if defined(HAVE_YASSL)
  TaoCrypt::MD5 *hasher= new TaoCrypt::MD5;
  return (void*)hasher;
#elif defined(HAVE_OPENSSL)
  MD5_CTX *ctx = new MD5_CTX();
  MD5_Init (ctx);
  return (void *)ctx;
#endif /* HAVE_YASSL */
}
void wsrep_md5_update(void *ctx, char* buf, int len)
{
#if defined(HAVE_YASSL)
  ((TaoCrypt::MD5 *)ctx)->Update((TaoCrypt::byte *) buf, len);
#elif defined(HAVE_OPENSSL)
  MD5_Update((MD5_CTX*)(ctx), buf, len);
#endif /* HAVE_YASSL */
}
void wsrep_compute_md5_hash(char *digest, void *ctx)
{
#if defined(HAVE_YASSL)
  ((TaoCrypt::MD5*)ctx)->Final((TaoCrypt::byte *) digest);
  delete (TaoCrypt::MD5*)ctx;
#elif defined(HAVE_OPENSSL)
  MD5_Final ((unsigned char*)digest, (MD5_CTX*)ctx);
  delete (MD5_CTX*)ctx;
#endif /* HAVE_YASSL */
}
#endif /* WITH_WSREP */
