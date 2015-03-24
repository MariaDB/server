/*
  TODO: add support for YASSL
*/

#include <my_global.h>
#include <my_crypt.h>

/* YASSL doesn't support EVP_CIPHER_CTX */
#ifdef HAVE_EncryptAes128Ctr

#include "mysql.h"
#include <openssl/evp.h>
#include <openssl/aes.h>

static const int CRYPT_ENCRYPT = 1;
static const int CRYPT_DECRYPT = 0;

C_MODE_START

static int do_crypt(const EVP_CIPHER *cipher, int mode,
                    const uchar* source, uint32 source_length,
                    uchar* dest, uint32* dest_length,
                    const unsigned char* key, uint8 key_length,
                    const unsigned char* iv, uint8 iv_length,
                    uint noPadding)
{
  int res= AES_OPENSSL_ERROR;
  EVP_CIPHER_CTX ctx;
  EVP_CIPHER_CTX_init(&ctx);
  if (!EVP_CipherInit_ex(&ctx, cipher, NULL, key, iv, mode))
    goto err;
  if (!EVP_CipherUpdate(&ctx, dest, (int*)dest_length, source, source_length))
    goto err;
  res= AES_OK;
err:
  EVP_CIPHER_CTX_cleanup(&ctx);
  return res;
}


int my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  return do_crypt(EVP_aes_128_ctr(), CRYPT_ENCRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, noPadding);
}


int my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  return do_crypt(EVP_aes_128_ctr(), CRYPT_DECRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, noPadding);
}


int my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  return do_crypt(EVP_aes_128_ecb(), CRYPT_ENCRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, noPadding);
}

int my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  return do_crypt(EVP_aes_128_ecb(), CRYPT_DECRYPT, source, source_length,
                  dest, dest_length, key, key_length, iv, iv_length, noPadding);
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
