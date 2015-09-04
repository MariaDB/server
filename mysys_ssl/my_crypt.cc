/*
 Copyright (c) 2014 Google Inc.
 Copyright (c) 2014, 2015 MariaDB Corporation

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
#include <string.h>
#include <my_crypt.h>

#ifdef HAVE_YASSL
#include "yassl.cc"
#else

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/err.h>

#endif

class MyCTX
{
public:
  EVP_CIPHER_CTX ctx;
  MyCTX() { EVP_CIPHER_CTX_init(&ctx); }
  virtual ~MyCTX() { EVP_CIPHER_CTX_cleanup(&ctx); ERR_remove_state(0); }

  virtual int init(const EVP_CIPHER *cipher, int encrypt, const uchar *key,
                   uint klen, const uchar *iv, uint ivlen)
  {
    if (unlikely(!cipher))
      return MY_AES_BAD_KEYSIZE;

    if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, encrypt))
      return MY_AES_OPENSSL_ERROR;

    DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == (int)klen);
    DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(&ctx) <= (int)ivlen);

    return MY_AES_OK;
  }
  virtual int update(const uchar *src, uint slen, uchar *dst, uint *dlen)
  {
    if (!EVP_CipherUpdate(&ctx, dst, (int*)dlen, src, slen))
      return MY_AES_OPENSSL_ERROR;
    return MY_AES_OK;
  }
  virtual int finish(uchar *dst, uint *dlen)
  {
    if (!EVP_CipherFinal_ex(&ctx, dst, (int*)dlen))
      return MY_AES_BAD_DATA;
    return MY_AES_OK;
  }
};

class MyCTX_nopad : public MyCTX
{
public:
  const uchar *key;
  int klen;

  MyCTX_nopad() : MyCTX() { }
  ~MyCTX_nopad() { }

  int init(const EVP_CIPHER *cipher, int encrypt, const uchar *key, uint klen,
           const uchar *iv, uint ivlen)
  {
    compile_time_assert(MY_AES_CTX_SIZE >= sizeof(MyCTX_nopad));
    this->key= key;
    this->klen= klen;
    int res= MyCTX::init(cipher, encrypt, key, klen, iv, ivlen);
    memcpy(ctx.oiv, iv, ivlen); // in ECB mode OpenSSL doesn't do that itself
    EVP_CIPHER_CTX_set_padding(&ctx, 0);
    return res;
  }

  int finish(uchar *dst, uint *dlen)
  {
    if (ctx.buf_len)
    {
      /*
        Not much we can do, block ciphers cannot encrypt data that aren't
        a multiple of the block length. At least not without padding.
        Let's do something CTR-like for the last partial block.
      */
      uchar mask[MY_AES_BLOCK_SIZE];
      uint mlen;

      my_aes_crypt(MY_AES_ECB, ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
                   ctx.oiv, sizeof(mask), mask, &mlen, key, klen, 0, 0);
      DBUG_ASSERT(mlen == sizeof(mask));

      for (int i=0; i < ctx.buf_len; i++)
        dst[i]= ctx.buf[i] ^ mask[i];
    }
    *dlen= ctx.buf_len;
    return MY_AES_OK;
  }
};

#define make_aes_dispatcher(mode)                               \
  static inline const EVP_CIPHER *aes_ ## mode(uint klen)       \
  {                                                             \
    switch (klen) {                                             \
    case 16: return EVP_aes_128_ ## mode();                     \
    case 24: return EVP_aes_192_ ## mode();                     \
    case 32: return EVP_aes_256_ ## mode();                     \
    default: return 0;                                          \
    }                                                           \
  }

make_aes_dispatcher(ecb)
make_aes_dispatcher(cbc)
#ifdef HAVE_EncryptAes128Ctr
make_aes_dispatcher(ctr)
#endif /* HAVE_EncryptAes128Ctr */
#ifdef HAVE_EncryptAes128Gcm
make_aes_dispatcher(gcm)

/*
  special implementation for GCM; to fit OpenSSL AES-GCM into the
  existing my_aes_* API it does the following:
    - IV tail (over 12 bytes) goes to AAD
    - the tag is appended to the ciphertext
*/

class MyCTX_gcm : public MyCTX
{
public:
  const uchar *aad;
  int aadlen;
  MyCTX_gcm() : MyCTX() { }
  ~MyCTX_gcm() { }

  int init(const EVP_CIPHER *cipher, int encrypt, const uchar *key, uint klen,
           const uchar *iv, uint ivlen)
  {
    compile_time_assert(MY_AES_CTX_SIZE >= sizeof(MyCTX_gcm));
    int res= MyCTX::init(cipher, encrypt, key, klen, iv, ivlen);
    int real_ivlen= EVP_CIPHER_CTX_iv_length(&ctx);
    aad= iv + real_ivlen;
    aadlen= ivlen - real_ivlen;
    return res;
  }

  int update(const uchar *src, uint slen, uchar *dst, uint *dlen)
  {
    /*
      note that this GCM class cannot do streaming decryption, because
      it needs the tag (which is located at the end of encrypted data)
      before decrypting the data. it can encrypt data piecewise, like, first
      half, then the second half, but it must decrypt all at once
    */
    if (!ctx.encrypt)
    {
      slen-= MY_AES_BLOCK_SIZE;
      if(!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_SET_TAG, MY_AES_BLOCK_SIZE,
                              (void*)(src + slen)))
        return MY_AES_OPENSSL_ERROR;
    }
    int unused;
    if (aadlen && !EVP_CipherUpdate(&ctx, NULL, &unused, aad, aadlen))
      return MY_AES_OPENSSL_ERROR;
    aadlen= 0;
    return MyCTX::update(src, slen, dst, dlen);
  }

  int finish(uchar *dst, uint *dlen)
  {
    int fin;
    if (!EVP_CipherFinal_ex(&ctx, dst, &fin))
      return MY_AES_BAD_DATA;
    DBUG_ASSERT(fin == 0);

    if (ctx.encrypt)
    {
      if(!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_GET_TAG, MY_AES_BLOCK_SIZE, dst))
        return MY_AES_OPENSSL_ERROR;
      *dlen= MY_AES_BLOCK_SIZE;
    }
    else
      *dlen= 0;
    return MY_AES_OK;
  }
};

#endif

const EVP_CIPHER *(*ciphers[])(uint)= {
    aes_ecb, aes_cbc
#ifdef HAVE_EncryptAes128Ctr
  , aes_ctr
#ifdef HAVE_EncryptAes128Gcm
  , aes_gcm
#endif
#endif
};

extern "C" {

int my_aes_crypt_init(void *ctx, enum my_aes_mode mode, int flags,
                      const unsigned char* key, unsigned int klen,
                      const unsigned char* iv, unsigned int ivlen)
{
#ifdef HAVE_EncryptAes128Ctr
#ifdef HAVE_EncryptAes128Gcm
  if (mode == MY_AES_GCM)
    if (flags & ENCRYPTION_FLAG_NOPAD)
      return MY_AES_OPENSSL_ERROR;
    else
      new (ctx) MyCTX_gcm();
  else
#endif
  if (mode == MY_AES_CTR)
    new (ctx) MyCTX();
  else
#endif
  if (flags & ENCRYPTION_FLAG_NOPAD)
    new (ctx) MyCTX_nopad();
  else
    new (ctx) MyCTX();
  return ((MyCTX*)ctx)->init(ciphers[mode](klen), flags & 1,
                             key, klen, iv, ivlen);
}

int my_aes_crypt_update(void *ctx, const uchar *src, uint slen,
                        uchar *dst, uint *dlen)
{
  return ((MyCTX*)ctx)->update(src, slen, dst, dlen);
}

int my_aes_crypt_finish(void *ctx, uchar *dst, uint *dlen)
{
  int res= ((MyCTX*)ctx)->finish(dst, dlen);
  ((MyCTX*)ctx)->~MyCTX();
  return res;
}

int my_aes_crypt(enum my_aes_mode mode, int flags,
                 const uchar *src, uint slen, uchar *dst, uint *dlen,
                 const uchar *key, uint klen, const uchar *iv, uint ivlen)
{
  void *ctx= alloca(MY_AES_CTX_SIZE);
  int res1, res2;
  uint d1, d2;
  if ((res1= my_aes_crypt_init(ctx, mode, flags, key, klen, iv, ivlen)))
    return res1;
  res1= my_aes_crypt_update(ctx, src, slen, dst, &d1);
  res2= my_aes_crypt_finish(ctx, dst + d1, &d2);
  *dlen= d1 + d2;
  return res1 ? res1 : res2;
}

#ifdef HAVE_YASSL
#include <random.hpp>
int my_random_bytes(uchar* buf, int num)
{
  TaoCrypt::RandomNumberGenerator rand;
  rand.GenerateBlock((TaoCrypt::byte*) buf, num);
  return MY_AES_OK;
}
#else
#include <openssl/rand.h>

int my_random_bytes(uchar *buf, int num)
{
  /*
    Unfortunately RAND_bytes manual page does not provide any guarantees
    in relation to blocking behavior. Here we explicitly use SSLeay random
    instead of whatever random engine is currently set in OpenSSL. That way
    we are guaranteed to have a non-blocking random.
  */
  RAND_METHOD *rand = RAND_SSLeay();
  if (rand == NULL || rand->bytes(buf, num) != 1)
    return MY_AES_OPENSSL_ERROR;
  return MY_AES_OK;
}
#endif

}

