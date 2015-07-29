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
#include <my_crypt.h>

#ifdef HAVE_YASSL
#include "aes.hpp"

typedef TaoCrypt::CipherDir Dir;
static const Dir CRYPT_ENCRYPT = TaoCrypt::ENCRYPTION;
static const Dir CRYPT_DECRYPT = TaoCrypt::DECRYPTION;

typedef TaoCrypt::Mode CipherMode;
static inline CipherMode aes_ecb(uint) { return TaoCrypt::ECB; }
static inline CipherMode aes_cbc(uint) { return TaoCrypt::CBC; }

typedef TaoCrypt::byte KeyByte;

#else
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/err.h>

typedef int Dir;
static const Dir CRYPT_ENCRYPT = 1;
static const Dir CRYPT_DECRYPT = 0;

typedef const EVP_CIPHER *CipherMode;

#define make_aes_dispatcher(mode)                               \
  static inline CipherMode aes_ ## mode(uint key_length)        \
  {                                                             \
    switch (key_length) {                                       \
    case 16: return EVP_aes_128_ ## mode();                     \
    case 24: return EVP_aes_192_ ## mode();                     \
    case 32: return EVP_aes_256_ ## mode();                     \
    default: return 0;                                          \
    }                                                           \
  }

make_aes_dispatcher(ecb)
make_aes_dispatcher(cbc)

typedef uchar KeyByte;

struct MyCTX : EVP_CIPHER_CTX {
  MyCTX()  { EVP_CIPHER_CTX_init(this); }
  ~MyCTX() { EVP_CIPHER_CTX_cleanup(this); ERR_remove_state(0); }
};
#endif

static int block_crypt(CipherMode cipher, Dir dir,
                       const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const KeyByte *key, uint key_length,
                       const KeyByte *iv, uint iv_length, int no_padding)
{
  int tail= source_length % MY_AES_BLOCK_SIZE;

  if (likely(source_length >= MY_AES_BLOCK_SIZE || !no_padding))
  {
#ifdef HAVE_YASSL
    TaoCrypt::AES ctx(dir, cipher);

    if (unlikely(key_length != 16 && key_length != 24 && key_length != 32))
      return MY_AES_BAD_KEYSIZE;

    ctx.SetKey(key, key_length);
    if (iv)
    {
      ctx.SetIV(iv);
      DBUG_ASSERT(TaoCrypt::AES::BLOCK_SIZE <= iv_length);
    }
    DBUG_ASSERT(TaoCrypt::AES::BLOCK_SIZE == MY_AES_BLOCK_SIZE);

    ctx.Process(dest, source, source_length - tail);
    *dest_length= source_length - tail;

    /* unlike OpenSSL, YaSSL doesn't support PKCS#7 padding */
    if (!no_padding)
    {
      if (dir == CRYPT_ENCRYPT)
      {
        uchar buf[MY_AES_BLOCK_SIZE];
        memcpy(buf, source + source_length - tail, tail);
        memset(buf + tail, MY_AES_BLOCK_SIZE - tail, MY_AES_BLOCK_SIZE - tail);
        ctx.Process(dest + *dest_length, buf, MY_AES_BLOCK_SIZE);
        *dest_length+= MY_AES_BLOCK_SIZE;
      }
      else
      {
        int n= source_length ? dest[source_length - 1] : 0;
        if (tail || n == 0 || n > MY_AES_BLOCK_SIZE)
          return MY_AES_BAD_DATA;
        *dest_length-= n;
      }
    }

#else // HAVE_OPENSSL
    int fin;
    struct MyCTX ctx;

    if (unlikely(!cipher))
      return MY_AES_BAD_KEYSIZE;

    if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, dir))
      return MY_AES_OPENSSL_ERROR;

    EVP_CIPHER_CTX_set_padding(&ctx, !no_padding);

    DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == (int)key_length);
    DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(&ctx) <= (int)iv_length);
    DBUG_ASSERT(EVP_CIPHER_CTX_block_size(&ctx) == MY_AES_BLOCK_SIZE);

    /* use built-in OpenSSL padding, if possible */
    if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length,
                          source, source_length - (no_padding ? tail : 0)))
      return MY_AES_OPENSSL_ERROR;
    if (!EVP_CipherFinal_ex(&ctx, dest + *dest_length, &fin))
      return MY_AES_BAD_DATA;
    *dest_length += fin;

#endif
  }

  if (no_padding)
  {
    if (tail)
    {
      /*
        Not much we can do, block ciphers cannot encrypt data that aren't
        a multiple of the block length. At least not without padding.
        Let's do something CTR-like for the last partial block.
      */

      uchar mask[MY_AES_BLOCK_SIZE];
      uint mlen;

      DBUG_ASSERT(iv_length >= sizeof(mask));
      my_aes_encrypt_ecb(iv, sizeof(mask), mask, &mlen,
                         key, key_length, 0, 0, 1);
      DBUG_ASSERT(mlen == sizeof(mask));

      const uchar *s= source + source_length - tail;
      const uchar *e= source + source_length;
      uchar *d= dest + source_length - tail;
      const uchar *m= mask;
      while (s < e)
        *d++ = *s++ ^ *m++;
    }
    *dest_length= source_length;
  }

  return MY_AES_OK;
}

C_MODE_START

#ifdef HAVE_EncryptAes128Ctr
make_aes_dispatcher(ctr)

/*
  special simplified implementation for CTR, because it's a stream cipher
  (doesn't need padding, always encrypts the specified number of bytes), and
  because encrypting and decrypting code is exactly the same (courtesy of XOR)
*/
int my_aes_encrypt_ctr(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length)
{
  CipherMode cipher= aes_ctr(key_length);
  struct MyCTX ctx;
  int fin __attribute__((unused));

  if (unlikely(!cipher))
    return MY_AES_BAD_KEYSIZE;

  if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, CRYPT_ENCRYPT))
    return MY_AES_OPENSSL_ERROR;

  DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == (int)key_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(&ctx) <= (int)iv_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_block_size(&ctx) == 1);

  if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length, source, source_length))
    return MY_AES_OPENSSL_ERROR;

  DBUG_ASSERT(EVP_CipherFinal_ex(&ctx, dest + *dest_length, &fin));
  DBUG_ASSERT(fin == 0);

  return MY_AES_OK;
}

#endif /* HAVE_EncryptAes128Ctr */

#ifdef HAVE_EncryptAes128Gcm
make_aes_dispatcher(gcm)

/*
  special implementation for GCM; to fit OpenSSL AES-GCM into the
  existing my_aes_* API it does the following:
    - IV tail (over 12 bytes) goes to AAD
    - the tag is appended to the ciphertext
*/
int do_gcm(const uchar* source, uint source_length,
           uchar* dest, uint* dest_length,
           const uchar* key, uint key_length,
           const uchar* iv, uint iv_length, Dir dir)
{
  CipherMode cipher= aes_gcm(key_length);
  struct MyCTX ctx;
  int fin;
  uint real_iv_length;

  if (unlikely(!cipher))
    return MY_AES_BAD_KEYSIZE;

  if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, dir))
    return MY_AES_OPENSSL_ERROR;

  real_iv_length= EVP_CIPHER_CTX_iv_length(&ctx);

  DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == (int)key_length);
  DBUG_ASSERT(real_iv_length <= iv_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_block_size(&ctx) == 1);

  if (dir == CRYPT_DECRYPT)
  {
    source_length-= MY_AES_BLOCK_SIZE;
    if(!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_SET_TAG, MY_AES_BLOCK_SIZE,
                            (void*)(source + source_length)))
      return MY_AES_OPENSSL_ERROR;
  }

  if (real_iv_length < iv_length)
  {
    if (!EVP_CipherUpdate(&ctx, NULL, &fin,
                          iv + real_iv_length, iv_length - real_iv_length))
      return MY_AES_OPENSSL_ERROR;
  }

  if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length, source, source_length))
    return MY_AES_OPENSSL_ERROR;

  if (!EVP_CipherFinal_ex(&ctx, dest + *dest_length, &fin))
    return MY_AES_BAD_DATA;
  DBUG_ASSERT(fin == 0);

  if (dir == CRYPT_ENCRYPT)
  {
    if(!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_GET_TAG, MY_AES_BLOCK_SIZE,
                            dest + *dest_length))
      return MY_AES_OPENSSL_ERROR;
    *dest_length+= MY_AES_BLOCK_SIZE;
  }

  return MY_AES_OK;
}

int my_aes_encrypt_gcm(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length)
{
  return do_gcm(source, source_length, dest, dest_length,
                key, key_length, iv, iv_length, CRYPT_ENCRYPT);
}

int my_aes_decrypt_gcm(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length)
{
  return do_gcm(source, source_length, dest, dest_length,
                key, key_length, iv, iv_length, CRYPT_DECRYPT);
}

#endif

int my_aes_encrypt_ecb(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length,
                       int no_padding)
{
  return block_crypt(aes_ecb(key_length), CRYPT_ENCRYPT, source, source_length,
                     dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_decrypt_ecb(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length,
                       int no_padding)
{
  return block_crypt(aes_ecb(key_length), CRYPT_DECRYPT, source, source_length,
                     dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_encrypt_cbc(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length,
                       int no_padding)
{
  return block_crypt(aes_cbc(key_length), CRYPT_ENCRYPT, source, source_length,
                     dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_decrypt_cbc(const uchar* source, uint source_length,
                       uchar* dest, uint* dest_length,
                       const uchar* key, uint key_length,
                       const uchar* iv, uint iv_length,
                       int no_padding)
{
  return block_crypt(aes_cbc(key_length), CRYPT_DECRYPT, source, source_length,
                     dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

C_MODE_END

#if defined(HAVE_YASSL)

#include <random.hpp>

C_MODE_START

int my_random_bytes(uchar* buf, int num)
{
  TaoCrypt::RandomNumberGenerator rand;
  rand.GenerateBlock((TaoCrypt::byte*) buf, num);
  return MY_AES_OK;
}

C_MODE_END

#else  /* OpenSSL */

#include <openssl/rand.h>

C_MODE_START

int my_random_bytes(uchar* buf, int num)
{
  /*
    Unfortunately RAND_bytes manual page does not provide any guarantees
    in relation to blocking behavior. Here we explicitly use SSLeay random
    instead of whatever random engine is currently set in OpenSSL. That way
    we are guaranteed to have a non-blocking random.
  */
  RAND_METHOD* rand = RAND_SSLeay();
  if (rand == NULL || rand->bytes(buf, num) != 1)
    return MY_AES_OPENSSL_ERROR;
  return MY_AES_OK;
}

C_MODE_END
#endif /* HAVE_YASSL */

/**
  Get size of buffer which will be large enough for encrypted data

  The buffer should be sufficiently large to fit encrypted data
  independently from the encryption algorithm and mode. With padding up to
  MY_AES_BLOCK_SIZE bytes can be added. With GCM, exactly MY_AES_BLOCK_SIZE
  bytes are added.

  The actual length of the encrypted data is returned from the encryption
  function (e.g. from my_aes_encrypt_cbc).

  @return required buffer size
*/

uint my_aes_get_size(uint source_length)
{
  return source_length + MY_AES_BLOCK_SIZE;
}
