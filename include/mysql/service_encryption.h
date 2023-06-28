#ifndef MYSQL_SERVICE_ENCRYPTION_INCLUDED
/* Copyright (c) 2015, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file
  encryption service

  Functions to support data encryption and encryption key management.
  They are normally implemented in an encryption plugin, so this service
  connects encryption *consumers* (e.g. storage engines) to the encryption
  *provider* (encryption plugin).
*/

#ifndef MYSQL_ABI_CHECK
#include <my_alloca.h>
#ifdef _WIN32
#ifndef __cplusplus
#define inline __inline
#endif
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif
#ifndef MYSQL_ABI_CHECK
#include <assert.h>
#endif

/* returned from encryption_key_get_latest_version() */
#define ENCRYPTION_KEY_VERSION_INVALID        (~(unsigned int)0)
#define ENCRYPTION_KEY_NOT_ENCRYPTED          (0)

#define ENCRYPTION_KEY_SYSTEM_DATA             1
#define ENCRYPTION_KEY_TEMPORARY_DATA          2

/* returned from encryption_key_get()  */
#define ENCRYPTION_KEY_BUFFER_TOO_SMALL    (100)

#define ENCRYPTION_FLAG_DECRYPT     0
#define ENCRYPTION_FLAG_ENCRYPT     1
#define ENCRYPTION_FLAG_NOPAD       2

struct encryption_service_st {
  unsigned int (*encryption_key_get_latest_version_func)(unsigned int key_id);
  unsigned int (*encryption_key_get_func)(unsigned int key_id, unsigned int key_version,
                                          unsigned char* buffer, unsigned int* length);
  unsigned int (*encryption_ctx_size_func)(unsigned int key_id, unsigned int key_version);
  int (*encryption_ctx_init_func)(void *ctx, const unsigned char* key, unsigned int klen,
                                  const unsigned char* iv, unsigned int ivlen,
                                  int flags, unsigned int key_id,
                                  unsigned int key_version);
  int (*encryption_ctx_update_func)(void *ctx, const unsigned char* src, unsigned int slen,
                                    unsigned char* dst, unsigned int* dlen);
  int (*encryption_ctx_finish_func)(void *ctx, unsigned char* dst, unsigned int* dlen);
  unsigned int (*encryption_encrypted_length_func)(unsigned int slen, unsigned int key_id, unsigned int key_version);
};

#ifdef MYSQL_DYNAMIC_PLUGIN

extern struct encryption_service_st *encryption_service;

#define encryption_key_get_latest_version(KI) encryption_service->encryption_key_get_latest_version_func(KI)
#define encryption_key_get(KI,KV,K,S) encryption_service->encryption_key_get_func((KI),(KV),(K),(S))
#define encryption_ctx_size(KI,KV) encryption_service->encryption_ctx_size_func((KI),(KV))
#define encryption_ctx_init(CTX,K,KL,IV,IVL,F,KI,KV) encryption_service->encryption_ctx_init_func((CTX),(K),(KL),(IV),(IVL),(F),(KI),(KV))
#define encryption_ctx_update(CTX,S,SL,D,DL) encryption_service->encryption_ctx_update_func((CTX),(S),(SL),(D),(DL))
#define encryption_ctx_finish(CTX,D,DL) encryption_service->encryption_ctx_finish_func((CTX),(D),(DL))
#define encryption_encrypted_length(SL,KI,KV) encryption_service->encryption_encrypted_length_func((SL),(KI),(KV))
#else

extern struct encryption_service_st encryption_handler;

#define encryption_key_get_latest_version(KI) encryption_handler.encryption_key_get_latest_version_func(KI)
#define encryption_key_get(KI,KV,K,S) encryption_handler.encryption_key_get_func((KI),(KV),(K),(S))
#define encryption_ctx_size(KI,KV) encryption_handler.encryption_ctx_size_func((KI),(KV))
#define encryption_ctx_init(CTX,K,KL,IV,IVL,F,KI,KV) encryption_handler.encryption_ctx_init_func((CTX),(K),(KL),(IV),(IVL),(F),(KI),(KV))
#define encryption_ctx_update(CTX,S,SL,D,DL) encryption_handler.encryption_ctx_update_func((CTX),(S),(SL),(D),(DL))
#define encryption_ctx_finish(CTX,D,DL) encryption_handler.encryption_ctx_finish_func((CTX),(D),(DL))
#define encryption_encrypted_length(SL,KI,KV) encryption_handler.encryption_encrypted_length_func((SL),(KI),(KV))
#endif

static inline unsigned int encryption_key_id_exists(unsigned int id)
{
  return encryption_key_get_latest_version(id) != ENCRYPTION_KEY_VERSION_INVALID;
}

static inline unsigned int encryption_key_version_exists(unsigned int id, unsigned int version)
{
  unsigned int unused;
  return encryption_key_get(id, version, NULL, &unused) != ENCRYPTION_KEY_VERSION_INVALID;
}

/** main entrypoint to perform encryption or decryption
 * @invariant `src` is valid for `slen`
 * @invariant `dst` is valid for `*dlen`, `*dlen` is initialized
 * @invariant `src` and `dst` do not overlap
 */
static inline int encryption_crypt(const unsigned char* src, unsigned int slen,
                                   unsigned char* dst, unsigned int* dlen,
                                   const unsigned char* key, unsigned int klen,
                                   const unsigned char* iv, unsigned int ivlen,
                                   int flags, unsigned int key_id, unsigned int key_version)
{
  void *ctx= alloca(encryption_ctx_size(key_id, key_version));
  int res1, res2;
  unsigned int d1, d2= *dlen;

  // Verify dlen is initialized properly. See MDEV-30389
  assert(*dlen >= slen);
  assert((dst[*dlen - 1]= 1));
  // Verify buffers do not overlap
  if (src < dst)
    assert(src + slen <= dst);
  else
    assert(dst + *dlen <= src);

  if ((res1= encryption_ctx_init(ctx, key, klen, iv, ivlen, flags, key_id, key_version)))
    return res1;
  res1= encryption_ctx_update(ctx, src, slen, dst, &d1);
  d2-= d1;
  res2= encryption_ctx_finish(ctx, dst + d1, &d2);

  *dlen= d1 + d2;
  return res1 ? res1 : res2;
}

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_ENCRYPTION_INCLUDED
#endif
