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

/*
  TODO: add support for YASSL
*/

#include <my_global.h>
#include <my_crypt.h>

#ifdef HAVE_EncryptAes128Ctr

#include <openssl/evp.h>
#include <openssl/aes.h>

static const int CRYPT_ENCRYPT = 1;
static const int CRYPT_DECRYPT = 0;

C_MODE_START

static int do_crypt(const EVP_CIPHER *cipher, int encrypt,
                    const uchar* source, uint32 source_length,
                    uchar* dest, uint32* dest_length,
                    const uchar* key, uint8 key_length,
                    const uchar* iv, uint8 iv_length, int no_padding)
{
  int res= AES_OPENSSL_ERROR, fin;
  int tail= no_padding ? source_length % MY_AES_BLOCK_SIZE : 0;

  EVP_CIPHER_CTX ctx;
  EVP_CIPHER_CTX_init(&ctx);
  if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, encrypt))
    goto err;

  EVP_CIPHER_CTX_set_padding(&ctx, !no_padding);

  DBUG_ASSERT(EVP_CIPHER_CTX_key_length(&ctx) == key_length);
  DBUG_ASSERT(EVP_CIPHER_CTX_iv_length(&ctx) == iv_length || !EVP_CIPHER_CTX_iv_length(&ctx));
  DBUG_ASSERT(EVP_CIPHER_CTX_block_size(&ctx) == MY_AES_BLOCK_SIZE || !no_padding);

  if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length, source, source_length - tail))
    goto err;
  if (!EVP_CipherFinal_ex(&ctx, dest + *dest_length, &fin))
    goto err;
  *dest_length += fin;

  if (tail)
  {
    /*
      Not much we can do here, block cyphers cannot encrypt data that aren't
      a multiple of the block length. At least not without padding.
      What we do here, we XOR the tail with the previous encrypted block.
    */

    DBUG_ASSERT(source_length - tail == *dest_length);
    DBUG_ASSERT(source_length - tail > MY_AES_BLOCK_SIZE);
    const uchar *s= source + source_length - tail;
    const uchar *e= source + source_length;
    uchar *d= dest + source_length - tail;
    const uchar *m= (encrypt ? d : s) - MY_AES_BLOCK_SIZE;
    while (s < e)
      *d++ = *s++ ^ *m++;
    *dest_length= source_length;
  }

  res= AES_OK;
err:
  EVP_CIPHER_CTX_cleanup(&ctx);
  return res;
}

/* CTR is a stream cypher mode, it needs no special padding code */

int my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_ctr(), CRYPT_ENCRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, 0);
}


int my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_ctr(), CRYPT_DECRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, 0);
}


int my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_ecb(), CRYPT_ENCRYPT, source, source_length,
                        dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_ecb(), CRYPT_DECRYPT, source, source_length,
                        dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_encrypt_cbc(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_cbc(), CRYPT_ENCRYPT, source, source_length,
                        dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

int my_aes_decrypt_cbc(const uchar* source, uint32 source_length,
                       uchar* dest, uint32* dest_length,
                       const uchar* key, uint8 key_length,
                       const uchar* iv, uint8 iv_length,
                       uint no_padding)
{
  return do_crypt(EVP_aes_128_cbc(), CRYPT_DECRYPT, source, source_length,
                        dest, dest_length, key, key_length, iv, iv_length, no_padding);
}

C_MODE_END

#endif /* HAVE_EncryptAes128Ctr */

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
