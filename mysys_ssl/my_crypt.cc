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
static inline CipherMode aes_ecb(uint8) { return TaoCrypt::ECB; }
static inline CipherMode aes_cbc(uint8) { return TaoCrypt::CBC; }

typedef TaoCrypt::byte KeyByte;

#else
#include <openssl/evp.h>
#include <openssl/aes.h>

typedef int Dir;
static const Dir CRYPT_ENCRYPT = 1;
static const Dir CRYPT_DECRYPT = 0;

typedef const EVP_CIPHER *CipherMode;

#define make_aes_dispatcher(mode)                               \
  static inline CipherMode aes_ ## mode(uint8 key_length)       \
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
#ifdef HAVE_EncryptAes128Ctr
make_aes_dispatcher(ctr)
#endif

typedef uchar KeyByte;

struct MyCTX : EVP_CIPHER_CTX {
  MyCTX()  { EVP_CIPHER_CTX_init(this); }
  ~MyCTX() { EVP_CIPHER_CTX_cleanup(this); }
};
#endif

static int do_crypt(CipherMode cipher, Dir dir,
                    const uchar* source, uint32 source_length,
                    uchar* dest, uint32* dest_length,
                    const KeyByte *key, uint8 key_length,
                    const KeyByte *iv, uint8 iv_length, int no_padding)
{
  int tail= source_length % MY_AES_BLOCK_SIZE;

#ifdef HAVE_YASSL
  TaoCrypt::AES ctx(dir, cipher);

  if (unlikely(key_length != 16 && key_length != 24 && key_length != 32))
    return AES_BAD_KEYSIZE;

  ctx.SetKey(key, key_length);
  if (iv)
  {
    ctx.SetIV(iv);
    DBUG_ASSERT(TaoCrypt::AES::BLOCK_SIZE == iv_length);
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
      int n= dest[source_length - 1];
      if (tail || n == 0 || n > MY_AES_BLOCK_SIZE)
        return AES_OPENSSL_ERROR;
      *dest_length-= n;
    }
  }

#else // HAVE_OPENSSL
  int fin;
  struct MyCTX ctx;

  if (unlikely(!cipher))
    return AES_BAD_KEYSIZE;

  if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, dir))
    return AES_OPENSSL_ERROR;

  EVP_CIPHER_CTX_set_padding(&ctx, !no_padding);

  DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == key_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(&ctx) == iv_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_block_size(&ctx) == MY_AES_BLOCK_SIZE || !no_padding);

  /* use built-in OpenSSL padding, if possible */
  if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length,
                        source, source_length - (no_padding ? tail : 0)))
    return AES_OPENSSL_ERROR;
  if (!EVP_CipherFinal_ex(&ctx, dest + *dest_length, &fin))
    return AES_OPENSSL_ERROR;
  *dest_length += fin;

#endif

  if (no_padding && tail)
  {
    /*
      Not much we can do, block ciphers cannot encrypt data that aren't
      a multiple of the block length. At least not without padding.
      What we do here, we XOR the tail with the previous encrypted block.
    */

    if (unlikely(source_length < MY_AES_BLOCK_SIZE))
      return AES_OPENSSL_ERROR;

    const uchar *s= source + source_length - tail;
    const uchar *e= source + source_length;
    uchar *d= dest + source_length - tail;
    const uchar *m= (dir == CRYPT_ENCRYPT ? d : s) - MY_AES_BLOCK_SIZE;
    while (s < e)
      *d++ = *s++ ^ *m++;
    *dest_length= source_length;
  }

  return AES_OK;
}

C_MODE_START

#ifdef HAVE_EncryptAes128Ctr

int my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  /* CTR is a stream cipher mode, it needs no special padding code */
  return do_crypt(aes_ctr(key_length), CRYPT_ENCRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, 0);
}


int my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(aes_ctr(key_length), CRYPT_DECRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, 0);
}

#endif /* HAVE_EncryptAes128Ctr */

int my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(aes_ecb(key_length), CRYPT_ENCRYPT, source, source_length,
                        dest, dest_length, key, key_length, 0, 0, no_padding);
}

int my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(aes_ecb(key_length), CRYPT_DECRYPT, source, source_length,
                        dest, dest_length, key, key_length, 0, 0, no_padding);
}

int my_aes_encrypt_cbc(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(aes_cbc(key_length), CRYPT_ENCRYPT, source, source_length,
                        dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_decrypt_cbc(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(aes_cbc(key_length), CRYPT_DECRYPT, source, source_length,
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
  return AES_OK;
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
    return AES_OPENSSL_ERROR;
  return AES_OK;
}

C_MODE_END
#endif /* HAVE_YASSL */
