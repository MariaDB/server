/*
 Copyright (c) 2014 Google Inc.
 Copyright (c) 2014, 2019, MariaDB Corporation.

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

#include <my_global.h>
#include <string.h>

#define template _template /* bug in WolfSSL 4.4.0, see also violite.h */
#include <openssl/evp.h>
#undef template
#include <openssl/aes.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <my_crypt.h>
#include <ssl_compat.h>
#include <cstdint>

#define CTX_ALIGN 16

class MyCTX
{
public:
  char ctx_buf[EVP_CIPHER_CTX_SIZE + CTX_ALIGN];
  EVP_CIPHER_CTX* ctx;
  MyCTX()
  {
#if CTX_ALIGN > 0
    uintptr_t p= ((uintptr_t)ctx_buf + (CTX_ALIGN - 1)) & ~(CTX_ALIGN - 1);
    ctx = reinterpret_cast<EVP_CIPHER_CTX*>(p);
#else
    ctx = (EVP_CIPHER_CTX*)ctx_buf;
#endif

    EVP_CIPHER_CTX_init(ctx);
  }
  virtual ~MyCTX()
  {
    EVP_CIPHER_CTX_reset(ctx);
    ERR_remove_state(0);
  }

  virtual int init(const EVP_CIPHER *cipher, int encrypt, const uchar *key,
                   uint klen, const uchar *iv, uint ivlen)
  {
    compile_time_assert(MY_AES_CTX_SIZE >= sizeof(MyCTX));
    if (unlikely(!cipher))
      return MY_AES_BAD_KEYSIZE;

    if (EVP_CipherInit_ex(ctx, cipher, NULL, key, iv, encrypt) != 1)
      return MY_AES_OPENSSL_ERROR;

    DBUG_ASSERT(EVP_CIPHER_CTX_key_length(ctx) == (int)klen);
    DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(ctx) <= (int)ivlen);

    return MY_AES_OK;
  }
  virtual int update(const uchar *src, uint slen, uchar *dst, uint *dlen)
  {
#ifdef HAVE_WOLFSSL
    // WolfSSL checks parameters and does not like NULL pointers to be passed to function below.
    if (!src)
    {
      static uchar dummy[MY_AES_BLOCK_SIZE];
      DBUG_ASSERT(!slen);
      src=dummy;
    }
#endif

    if (EVP_CipherUpdate(ctx, dst, (int*)dlen, src, slen) != 1)
      return MY_AES_OPENSSL_ERROR;
    return MY_AES_OK;
  }
  virtual int finish(uchar *dst, uint *dlen)
  {
    if (EVP_CipherFinal_ex(ctx, dst, (int*)dlen) != 1)
      return MY_AES_BAD_DATA;
    return MY_AES_OK;
  }
};

class MyCTX_nopad : public MyCTX
{
public:
  const uchar *key;
  uint klen, source_tail_len;
  uchar oiv[MY_AES_BLOCK_SIZE];
  uchar source_tail[MY_AES_BLOCK_SIZE];

  MyCTX_nopad() : MyCTX() { }
  ~MyCTX_nopad() { }

  int init(const EVP_CIPHER *cipher, int encrypt, const uchar *key, uint klen,
           const uchar *iv, uint ivlen)
  {
    compile_time_assert(MY_AES_CTX_SIZE >= sizeof(MyCTX_nopad));
    this->key= key;
    this->klen= klen;
    this->source_tail_len= 0;
    if (ivlen)
      memcpy(oiv, iv, ivlen);
    DBUG_ASSERT(ivlen == 0 || ivlen == sizeof(oiv));

    int res= MyCTX::init(cipher, encrypt, key, klen, iv, ivlen);

    EVP_CIPHER_CTX_set_padding(ctx, 0);
    return res;
  }

  /** Update last partial source block, stored in source_tail array. */
  void update_source_tail(const uchar* src, uint slen)
  {
    if (!slen)
      return;
    uint new_tail_len= (source_tail_len + slen) % MY_AES_BLOCK_SIZE;
    if (new_tail_len)
    {
      if (slen + source_tail_len < MY_AES_BLOCK_SIZE)
      {
        memcpy(source_tail + source_tail_len, src, slen);
      }
      else
      {
        DBUG_ASSERT(slen > new_tail_len);
        memcpy(source_tail, src + slen - new_tail_len, new_tail_len);
      }
    }
    source_tail_len= new_tail_len;
  }

  int update(const uchar *src, uint slen, uchar *dst, uint *dlen)
  {
    update_source_tail(src, slen);
    return MyCTX::update(src, slen, dst, dlen);
  }

  int finish(uchar *dst, uint *dlen)
  {
    if (source_tail_len)
    {
      /*
        Not much we can do, block ciphers cannot encrypt data that aren't
        a multiple of the block length. At least not without padding.
        Let's do something CTR-like for the last partial block.
      */
      uchar mask[MY_AES_BLOCK_SIZE];
      uint mlen;

      int rc= my_aes_crypt(MY_AES_ECB, ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
                   oiv, sizeof(mask), mask, &mlen, key, klen, 0, 0);
      DBUG_ASSERT(rc == MY_AES_OK);
      if (rc)
        return rc;
      DBUG_ASSERT(mlen == sizeof(mask));

      for (uint i=0; i < source_tail_len; i++)
        dst[i]= source_tail[i] ^ mask[i];
    }
    *dlen= source_tail_len;
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
    int real_ivlen= EVP_CIPHER_CTX_iv_length(ctx);
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
    if (!EVP_CIPHER_CTX_encrypting(ctx))
    {
      /* encrypted string must contain authenticaton tag (see MDEV-11174) */
      if (slen < MY_AES_BLOCK_SIZE)
        return MY_AES_BAD_DATA;
      slen-= MY_AES_BLOCK_SIZE;
      if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, MY_AES_BLOCK_SIZE,
                              (void*)(src + slen)))
        return MY_AES_OPENSSL_ERROR;
    }
    int unused;
    if (aadlen && !EVP_CipherUpdate(ctx, NULL, &unused, aad, aadlen))
      return MY_AES_OPENSSL_ERROR;
    aadlen= 0;
    return MyCTX::update(src, slen, dst, dlen);
  }

  int finish(uchar *dst, uint *dlen)
  {
    int fin;
    if (!EVP_CipherFinal_ex(ctx, dst, &fin))
      return MY_AES_BAD_DATA;
    DBUG_ASSERT(fin == 0);

    if (EVP_CIPHER_CTX_encrypting(ctx))
    {
      if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, MY_AES_BLOCK_SIZE, dst))
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
  uint d1= 0, d2;
  if ((res1= my_aes_crypt_init(ctx, mode, flags, key, klen, iv, ivlen)))
    return res1;
  res1= my_aes_crypt_update(ctx, src, slen, dst, &d1);
  res2= my_aes_crypt_finish(ctx, dst + d1, &d2);
  if (res1 || res2)
    ERR_remove_state(0); /* in case of failure clear error queue */
  else
    *dlen= d1 + d2;
  return res1 ? res1 : res2;
}


/*
  calculate the length of the cyphertext from the length of the plaintext
  for different AES encryption modes with padding enabled.
  Without padding (ENCRYPTION_FLAG_NOPAD) cyphertext has the same length
  as the plaintext
*/
unsigned int my_aes_get_size(enum my_aes_mode mode __attribute__((unused)), unsigned int source_length)
{
#ifdef HAVE_EncryptAes128Ctr
  if (mode == MY_AES_CTR)
    return source_length;
#ifdef HAVE_EncryptAes128Gcm
  if (mode == MY_AES_GCM)
    return source_length + MY_AES_BLOCK_SIZE;
#endif
#endif
  return (source_length / MY_AES_BLOCK_SIZE + 1) * MY_AES_BLOCK_SIZE;
}


unsigned int my_aes_ctx_size(enum my_aes_mode)
{
  return MY_AES_CTX_SIZE;
}

int my_random_bytes(uchar *buf, int num)
{
  if (RAND_bytes(buf, num) != 1)
    return MY_AES_OPENSSL_ERROR;
  return MY_AES_OK;
}

}
