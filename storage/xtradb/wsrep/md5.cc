/*
   Copyright (c) 2014 SkySQL AB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifdef WITH_WSREP

#if defined(HAVE_YASSL)
#include "my_config.h"
#include "md5.hpp"
#elif defined(HAVE_OPENSSL)
#include <openssl/md5.h>
#endif /* HAVE_YASSL */

/* Initialize md5 object. */
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

/**
  Supply message to be hashed.

  @param ctx [IN]    Pointer to MD5 context
  @param buf [IN]    Message to be computed.
  @param len [IN]    Length of the message.
*/
void wsrep_md5_update(void *ctx, char* buf, int len)
{
#if defined(HAVE_YASSL)
  ((TaoCrypt::MD5 *)ctx)->Update((TaoCrypt::byte *) buf, len);
#elif defined(HAVE_OPENSSL)
  MD5_Update((MD5_CTX*)(ctx), buf, len);
#endif /* HAVE_YASSL */
}

/**
  Place computed MD5 digest into the given buffer.

  @param digest [OUT] Computed MD5 digest
  @param ctx    [IN]  Pointer to MD5 context
*/
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

