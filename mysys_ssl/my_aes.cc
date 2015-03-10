/* Copyright (c) 2002, 2012, Oracle and/or its affiliates. All rights reserved.

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
#include <m_string.h>
#include <my_aes.h>
#include <my_crypt.h>

#if defined(HAVE_YASSL)
#include "aes.hpp"
#include "openssl/ssl.h"
#include "crypto_wrapper.hpp"
#elif defined(HAVE_OPENSSL)
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/conf.h>

// Wrap C struct, to ensure resources are released.
struct MyCipherCtx
{
  MyCipherCtx() { memset(&ctx, 0, sizeof(ctx)); }
  ~MyCipherCtx() { EVP_CIPHER_CTX_cleanup(&ctx); }

  EVP_CIPHER_CTX ctx;
};
#endif

enum encrypt_dir { MY_AES_ENCRYPT, MY_AES_DECRYPT };

/**
  This is internal function just keeps joint code of Key generation

  SYNOPSIS
    my_aes_create_key()
    @param key        [in]       Key to use for real key creation
    @param key_length [in]       Length of the key
    @param rkey       [out]      Real key (used by OpenSSL/YaSSL)

  @return
    0         Ok
    -1        Error; Note: The current impementation never returns this
*/

static int my_aes_create_key(const char *key, int key_length, uint8 *rkey)
{
  uint8 *rkey_end= rkey + AES_KEY_LENGTH / 8;   /* Real key boundary */
  uint8 *ptr;                                   /* Start of the real key*/
  const char *sptr;                             /* Start of the working key */
  const char *key_end= key + key_length;        /* Working key boundary*/

  memset(rkey, 0, AES_KEY_LENGTH / 8);          /* Set initial key  */

  for (ptr= rkey, sptr= key; sptr < key_end; ptr ++, sptr ++)
  {
    if (ptr == rkey_end)
      /*  Just loop over tmp_key until we used all key */
      ptr= rkey;
    *ptr ^= (uint8) *sptr;
  }
#ifdef AES_USE_KEY_BITS
  /*
   This block is intended to allow more weak encryption if application
   build with libmysqld needs to correspond to export regulations
   It should be never used in normal distribution as does not give
   any speed improvement.
   To get worse security define AES_USE_KEY_BITS to number of bits
   you want key to be. It should be divisible by 8

   WARNING: Changing this value results in changing of enryption for
   all key lengths  so altering this value will result in impossibility
   to decrypt data encrypted with previous value
  */
#define AES_USE_KEY_BYTES (AES_USE_KEY_BITS/8)
  /*
   To get weaker key we use first AES_USE_KEY_BYTES bytes of created key
   and cyclically copy them until we created all required key length
  */
  for (ptr= rkey+AES_USE_KEY_BYTES, sptr=rkey ; ptr < rkey_end;
       ptr ++, sptr ++)
  {
    if (sptr == rkey + AES_USE_KEY_BYTES)
      sptr= rkey;
    *ptr= *sptr;
  }
#endif
  return 0;
}

/**
   Decode Hexencoded String to uint8[].

   SYNOPSIS
   my_aes_hex2uint()
   @param iv        [in]	Pointer to hexadecimal encoded IV String
   @param dest      [out]	Pointer to output uint8 array. Memory allocated by caller
   @param iv_length [in]  Size of destination array.
 */

void my_aes_hex2uint(const char* in, unsigned char *out, int dest_length)
{
  const char *pos= in;
  int count;
  for (count = 0; count < dest_length; count++)
  {
    uchar res;
    sscanf(pos, "%2hhx", &res);
    out[count] = res;
    pos += 2 * sizeof(char);
  }
}


/**
   Calculate key and iv from a given salt and secret as it is handled
   in openssl encrypted files via console

   SYNOPSIS
   my_bytes_to_key()
   @param salt   [in]  the given salt as extracted from the encrypted file
   @param secret [in]  the given secret as String, provided by the user
   @param key    [out] 32 Bytes of key are written to this pointer
   @param iv     [out] 16 Bytes of iv are written to this pointer
*/

void my_bytes_to_key(const unsigned char *salt, const char *secret, unsigned char *key,
                     unsigned char *iv)
{
#ifdef HAVE_YASSL
  /* the yassl function has no support for SHA1. Reason unknown. */
  int keyLen = 32;
  int ivLen  = 16;
  int EVP_SALT_SZ = 8;
  const int SHA_LEN = 20;
  yaSSL::SHA myMD;
  uint digestSz = myMD.get_digestSize();
  unsigned char digest[SHA_LEN];                   // max size
  int sz = strlen(secret);
  int count = 1;
  int keyLeft   = keyLen;
  int ivLeft    = ivLen;
  int keyOutput = 0;

  while (keyOutput < (keyLen + ivLen))
  {
    int digestLeft = digestSz;
    if (keyOutput)                      // first time D_0 is empty
      myMD.update(digest, digestSz);
    myMD.update((yaSSL::byte* )secret, sz);
    if (salt)
      myMD.update(salt, EVP_SALT_SZ);
    myMD.get_digest(digest);
    for (int j = 1; j < count; j++)
    {
      myMD.update(digest, digestSz);
      myMD.get_digest(digest);
    }

    if (keyLeft)
    {
      int store = MY_MIN(keyLeft, static_cast<int>(digestSz));
      memcpy(&key[keyLen - keyLeft], digest, store);

      keyOutput  += store;
      keyLeft    -= store;
      digestLeft -= store;
    }

    if (ivLeft && digestLeft)
    {
      int store = MY_MIN(ivLeft, digestLeft);
      memcpy(&iv[ivLen - ivLeft], &digest[digestSz - digestLeft], store);

      keyOutput += store;
      ivLeft    -= store;
    }
  }
#elif defined(HAVE_OPENSSL)
  const EVP_CIPHER *type = EVP_aes_256_cbc();
  const EVP_MD *digest = EVP_sha1();
  EVP_BytesToKey(type, digest, salt, (uchar*) secret, strlen(secret), 1, key, iv);
#endif
}

/**
  Crypt buffer with AES CBC encryption algorithm.

  SYNOPSIS
     my_aes_encrypt_cbc()
     @param source         [in]  Pointer to data for encryption
     @param source_length  [in]  Size of encryption data
     @param dest           [out] Buffer to place encrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of encrypted data
     @param key            [in]  Key to be used for encryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  if set to true, no padding is used. if the input length is not a
      	  	  	  	  	  	  	 multiple of the AES block size, trailing bytes are only copied to destination buffer.
      	  	  	  	  	  	  	 This allows currently the same interface for CBC, ECB and CTR encryption.
  @return
    != 0           error
    0             no error
*/

static int my_aes_encrypt_cbc(const uchar* source, uint32 source_length,
                              uchar* dest, uint32* dest_length,
                              const unsigned char* key, uint8 key_length,
                              const unsigned char* iv, uint8 iv_length,
                              uint noPadding)
{
  uint8 remaining_bytes = (noPadding == 0) ? 0 : source_length % MY_AES_BLOCK_SIZE;
  source_length = source_length - remaining_bytes;

#ifdef HAVE_YASSL
  TaoCrypt::AES_CBC_Encryption enc;
  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  int num_blocks;                               /* number of complete blocks */
  int i;
  switch(key_length) {
  case 16:
    break;
  case 24:
    break;
  case 32:
    break;
  default:
    return AES_BAD_KEYSIZE;
  }

  enc.SetKey((const TaoCrypt::byte *) key, key_length, (const TaoCrypt::byte *) iv);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  for (i = num_blocks; i > 0; i--)              /* Encode complete blocks */
  {
    enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  if (noPadding) {
	if (remaining_bytes!=0) {
	   memcpy(dest + source_length, source + source_length, remaining_bytes);
	}
    *dest_length = MY_AES_BLOCK_SIZE * (num_blocks) + remaining_bytes;
    return AES_OK;

  }

  /* Encode the rest. We always have incomplete block */
  char pad_len = MY_AES_BLOCK_SIZE - (source_length -
                                      MY_AES_BLOCK_SIZE * num_blocks);
  memcpy(block, source, 16 - pad_len);
  memset(block + MY_AES_BLOCK_SIZE - pad_len, pad_len,  pad_len);

  enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) block,
              MY_AES_BLOCK_SIZE);

  *dest_length = MY_AES_BLOCK_SIZE * (num_blocks + 1);
  return AES_OK;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;
  /* The real key to be used for encryption */
  const EVP_CIPHER* cipher;
  switch(key_length) {
  case 16:
    cipher = EVP_aes_128_cbc();
    break;
  case 24:
    cipher = EVP_aes_192_cbc();
    break;
  case 32:
    cipher = EVP_aes_256_cbc();
    break;
  default:
    return AES_BAD_KEYSIZE;
  }
  //Initialize Encryption Engine here, default software Engine is default
  ENGINE *engine = NULL;

  if (! EVP_EncryptInit_ex(&ctx.ctx, cipher, engine, key, iv))
    return AES_BAD_DATA;                        /* Error */
  if (noPadding) {
    EVP_CIPHER_CTX_set_padding(&ctx.ctx, 0);
  }
  EVP_CIPHER_CTX_key_length(&ctx.ctx);
  OPENSSL_assert(EVP_CIPHER_CTX_key_length(&ctx.ctx) == key_length);
  OPENSSL_assert(EVP_CIPHER_CTX_iv_length(&ctx.ctx) == iv_length);
  OPENSSL_assert(EVP_CIPHER_CTX_block_size(&ctx.ctx) == 16);
  if (! EVP_EncryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned const char *) source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_EncryptFinal_ex(&ctx.ctx, (unsigned char *) dest + u_len, &f_len))
    return AES_BAD_DATA;                        /* Error */

  if (remaining_bytes!=0) {
 	  memcpy(dest + source_length, source + source_length, remaining_bytes);
   }
  *dest_length = (unsigned long int) (u_len + f_len + remaining_bytes);

  return AES_OK;
#else
  /* currently Open SSL is required */
  return AES_BAD_DATA;
#endif
}


/**
  Crypt buffer with AES ECB encryption algorithm.

  SYNOPSIS
     my_aes_encrypt_ecb()
     @param source         [in]  Pointer to data for encryption
     @param source_length  [in]  Size of encryption data
     @param dest           [out] Buffer to place encrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of encrypted data
     @param key            [in]  Key to be used for encryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  if set to true, no padding is used. if the input length is not a
      	  	  	  	  	  	  	 multiple of the AES block size, trailing bytes are only copied to destination buffer.
      	  	  	  	  	  	  	 This allows currently the same interface for CBC, ECB and CTR encryption.
  @return
    != 0           error
    0             no error
*/

static int my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                              uchar* dest, uint32* dest_length,
                              const unsigned char* key, uint8 key_length,
                              const unsigned char* iv, uint8 iv_length,
                              uint noPadding)
{
  uint8 remaining_bytes = (noPadding == 0) ? 0 : source_length % MY_AES_BLOCK_SIZE;
  source_length = source_length - remaining_bytes;

#ifdef HAVE_YASSL
  TaoCrypt::AES_ECB_Encryption enc;
  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  int num_blocks;                               /* number of complete blocks */
  int i;
  switch(key_length) {
  case 16:
    break;
  case 24:
    break;
  case 32:
    break;
  default:
    return AES_BAD_KEYSIZE;
  }

  enc.SetKey((const TaoCrypt::byte *) key, key_length, (const TaoCrypt::byte *) iv);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  for (i = num_blocks; i > 0; i--)              /* Encode complete blocks */
  {
    enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  if (noPadding) {
	if (remaining_bytes!=0) {
	   memcpy(dest + source_length, source + source_length, remaining_bytes);
	}
	*dest_length = MY_AES_BLOCK_SIZE * (num_blocks) + remaining_bytes;
    return AES_OK;

  }

  /* Encode the rest. We always have incomplete block */
  char pad_len = MY_AES_BLOCK_SIZE - (source_length -
                                      MY_AES_BLOCK_SIZE * num_blocks);
  memcpy(block, source, 16 - pad_len);
  memset(block + MY_AES_BLOCK_SIZE - pad_len, pad_len,  pad_len);

  enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) block,
              MY_AES_BLOCK_SIZE);

  *dest_length = MY_AES_BLOCK_SIZE * (num_blocks + 1);
  return AES_OK;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;
  /* The real key to be used for encryption */
  const EVP_CIPHER* cipher;
  switch(key_length) {
  case 16:
    cipher = EVP_aes_128_ecb();
    break;
  case 24:
    cipher = EVP_aes_192_ecb();
    break;
  case 32:
    cipher = EVP_aes_256_ecb();
    break;
  default:
    return AES_BAD_KEYSIZE;
  }
  //Initialize Encryption Engine here, default software Engine is default
  ENGINE *engine = NULL;

  if (! EVP_EncryptInit_ex(&ctx.ctx, cipher, engine, key, iv))
    return AES_BAD_DATA;                        /* Error */
  if (noPadding) {
    EVP_CIPHER_CTX_set_padding(&ctx.ctx, 0);
  }
  EVP_CIPHER_CTX_key_length(&ctx.ctx);
  OPENSSL_assert(EVP_CIPHER_CTX_key_length(&ctx.ctx) == key_length);
  OPENSSL_assert(EVP_CIPHER_CTX_iv_length(&ctx.ctx) == iv_length);
  OPENSSL_assert(EVP_CIPHER_CTX_block_size(&ctx.ctx) == 16);
  if (! EVP_EncryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned const char *) source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_EncryptFinal_ex(&ctx.ctx, (unsigned char *) dest + u_len, &f_len))
    return AES_BAD_DATA;                        /* Error */

  if (remaining_bytes!=0) {
   	  memcpy(dest + source_length, source + source_length, remaining_bytes);
     }
  *dest_length = (unsigned long int) (u_len + f_len + remaining_bytes);

  return AES_OK;
#else
  /* currently Open SSL is required */
  return AES_BAD_DATA;
#endif
}



/**
  AES decryption - CBC mode

  SYNOPSIS
     my_aes_decrypt_cbc()
     @param source         [in]  Pointer to data to decrypt
     @param source_length  [in]  Size of data
     @param dest           [out] Buffer to place decrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of decrypted data
     @param key            [in]  Key to be used for decryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
 	 @param noPadding	   [in]  if set to true, no padding is used. if the input length is not a
      	  	  	  	  	  	  	 multiple of the AES block size, trailing bytes are only copied to destination buffer.
      	  	  	  	  	  	  	 This allows currently the same interface for CBC, ECB and CTR encryption.

  @return
    != 0           error
    0             no error
*/

static int my_aes_decrypt_cbc(const uchar* source, uint32 source_length,
                              uchar* dest, uint32 *dest_length,
                              const unsigned char* key, uint8 key_length,
                              const unsigned char* iv, uint8 iv_length,
                              uint noPadding)
{
	uint8 remaining_bytes = (noPadding == 0) ? 0 : source_length % MY_AES_BLOCK_SIZE;
	source_length = source_length - remaining_bytes;


#ifdef HAVE_YASSL
  TaoCrypt::AES_CBC_Decryption dec;
  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  uint num_blocks;                               /* Number of complete blocks */
  int i;
  switch(key_length) {
  case 16:
    break;
  case 24:
    break;
  case 32:
    break;
  default:
    return AES_BAD_KEYSIZE;
  }

  dec.SetKey((const TaoCrypt::byte *) key, key_length, iv);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  if ((source_length != num_blocks * MY_AES_BLOCK_SIZE) || num_blocks == 0 )
    /* Input size has to be even and at least one block */
    return AES_BAD_DATA;

  /* Decode all but last blocks */
  for (i = num_blocks - 1; i > 0; i--)
  {
    dec.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  dec.Process((TaoCrypt::byte *) block, (const TaoCrypt::byte *) source,
              MY_AES_BLOCK_SIZE);

  if (noPadding) {
    memcpy(dest, block, MY_AES_BLOCK_SIZE);
    if (remaining_bytes!=0) {
            	  memcpy(dest + source_length, source + source_length, remaining_bytes);
    }
    *dest_length = MY_AES_BLOCK_SIZE * num_blocks + remaining_bytes;
    return AES_OK;
  }

  /* Use last char in the block as size */
  uint pad_len = (uint) (uchar) block[MY_AES_BLOCK_SIZE - 1];

  if (pad_len > MY_AES_BLOCK_SIZE)
    return AES_BAD_DATA;
  /* We could also check whole padding but we do not really need this */

  memcpy(dest, block, MY_AES_BLOCK_SIZE - pad_len);
  *dest_length = MY_AES_BLOCK_SIZE * num_blocks - pad_len;
  return  AES_OK;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;

  const EVP_CIPHER* cipher;
  switch(key_length) {
  case 16:
    cipher = EVP_aes_128_cbc();
    break;
  case 24:
    cipher = EVP_aes_192_cbc();
    break;
  case 32:
    cipher = EVP_aes_256_cbc();
    break;
  default:
    return AES_BAD_KEYSIZE;
  }
  //Initialize Encryption Engine here, default software Engine is default
  ENGINE *engine = NULL;

  if (! EVP_DecryptInit_ex(&ctx.ctx, cipher, engine, key, iv))
    return AES_BAD_DATA;                        /* Error */
  if (noPadding) {
    EVP_CIPHER_CTX_set_padding(&ctx.ctx, 0);
  }
  OPENSSL_assert(EVP_CIPHER_CTX_key_length(&ctx.ctx) == key_length);
  OPENSSL_assert(EVP_CIPHER_CTX_iv_length(&ctx.ctx) == iv_length);
  OPENSSL_assert(EVP_CIPHER_CTX_block_size(&ctx.ctx) == 16);
  if (! EVP_DecryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned char *)source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_DecryptFinal_ex(&ctx.ctx, (unsigned char *) dest + u_len, &f_len)) {
    *dest_length = (unsigned long int) u_len;
    return AES_BAD_DATA;
  }
  if (remaining_bytes!=0) {
      	  memcpy(dest + source_length, source + source_length, remaining_bytes);
  }
  *dest_length = (unsigned long int) (u_len + f_len) + remaining_bytes;
#endif
  return AES_OK;
}

/**
  AES decryption - ECB mode

  SYNOPSIS
     my_aes_decrypt_ecb()
     @param source         [in]  Pointer to data to decrypt
     @param source_length  [in]  Size of data
     @param dest           [out] Buffer to place decrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of decrypted data
     @param key            [in]  Key to be used for decryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  if set to true, no padding is used. if the input length is not a
      	  	  	  	  	  	  	 multiple of the AES block size, trailing bytes are only copied to destination buffer.
      	  	  	  	  	  	  	 This allows currently the same interface for CBC, ECB and CTR encryption.

  @return
    != 0           error
    0             no error
*/

static int my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                              uchar* dest, uint32 *dest_length,
                              const unsigned char* key, uint8 key_length,
                              const unsigned char* iv, uint8 iv_length,
                              uint noPadding)
{
  uint8 remaining_bytes = (noPadding == 0) ? 0 : source_length % MY_AES_BLOCK_SIZE;
  source_length = source_length - remaining_bytes;


#ifdef HAVE_YASSL
  TaoCrypt::AES_ECB_Decryption dec;
  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  uint num_blocks;                               /* Number of complete blocks */
  int i;
  switch(key_length) {
  case 16:
    break;
  case 24:
    break;
  case 32:
    break;
  default:
    return AES_BAD_KEYSIZE;
  }

  dec.SetKey((const TaoCrypt::byte *) key, key_length, iv);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  if ((source_length != num_blocks * MY_AES_BLOCK_SIZE) || num_blocks == 0 )
    /* Input size has to be even and at least one block */
    return AES_BAD_DATA;

  /* Decode all but last blocks */
  for (i = num_blocks - 1; i > 0; i--)
  {
    dec.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  dec.Process((TaoCrypt::byte *) block, (const TaoCrypt::byte *) source,
              MY_AES_BLOCK_SIZE);

  if (noPadding) {
    memcpy(dest, block, MY_AES_BLOCK_SIZE);
    if (remaining_bytes!=0) {
      memcpy(dest + source_length, source + source_length, remaining_bytes);
    }
    *dest_length = MY_AES_BLOCK_SIZE * num_blocks + remaining_bytes;
    return AES_OK;
  }

  /* Use last char in the block as size */
  uint pad_len = (uint) (uchar) block[MY_AES_BLOCK_SIZE - 1];

  if (pad_len > MY_AES_BLOCK_SIZE)
    return AES_BAD_DATA;
  /* We could also check whole padding but we do not really need this */

  memcpy(dest, block, MY_AES_BLOCK_SIZE - pad_len);
  *dest_length = MY_AES_BLOCK_SIZE * num_blocks - pad_len;
  return  AES_OK;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;

  const EVP_CIPHER* cipher;
  switch(key_length) {
  case 16:
    cipher = EVP_aes_128_ecb();
    break;
  case 24:
    cipher = EVP_aes_192_ecb();
    break;
  case 32:
    cipher = EVP_aes_256_ecb();
    break;
  default:
    return AES_BAD_KEYSIZE;
  }
  //Initialize Encryption Engine here, default software Engine is default
  ENGINE *engine = NULL;

  if (! EVP_DecryptInit_ex(&ctx.ctx, cipher, engine, key, iv))
    return AES_BAD_DATA;                        /* Error */
  if (noPadding) {
    EVP_CIPHER_CTX_set_padding(&ctx.ctx, 0);
  }
  OPENSSL_assert(EVP_CIPHER_CTX_key_length(&ctx.ctx) == key_length);
  OPENSSL_assert(EVP_CIPHER_CTX_iv_length(&ctx.ctx) == iv_length);
  OPENSSL_assert(EVP_CIPHER_CTX_block_size(&ctx.ctx) == 16);
  if (! EVP_DecryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned char *)source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_DecryptFinal_ex(&ctx.ctx, (unsigned char *) dest + u_len, &f_len)) {
    *dest_length = (unsigned long int) u_len;
    return AES_BAD_DATA;
  }
  if (remaining_bytes!=0) {
    memcpy(dest + source_length, source + source_length, remaining_bytes);
  }
  *dest_length = (unsigned long int) (u_len + f_len) + remaining_bytes;

#endif
  return AES_OK;
}





/**
   Encryption interface that doesn't do anything (for testing)

   SYNOPSIS
     my_aes_encrypt_none()
     @param source         [in]  Pointer to data for encryption
     @param source_length  [in]  Size of encryption data
     @param dest           [out] Buffer to place encrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of encrypted data
     @param key            [in]  Key to be used for encryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  unused
  @return
    != 0           error
    0             no error
*/

static int my_aes_encrypt_none(const uchar* source, uint32 source_length,
                               uchar* dest, uint32* dest_length,
                               const unsigned char* key, uint8 key_length,
                               const unsigned char* iv, uint8 iv_length,
                               uint noPadding)
{
  memcpy(dest, source, source_length);
  *dest_length= source_length;
  return 0;
}


/**
   Decryption interface that doesn't do anything (for testing)

   SYNOPSIS
     my_aes_decrypt_none()
     @param source         [in]  Pointer to data to decrypt
     @param source_length  [in]  Size of data
     @param dest           [out] Buffer to place decrypted data (must be large enough)
     @param dest_length    [out] Pointer to size of decrypted data
     @param key            [in]  Key to be used for decryption
     @param key_length     [in]  Length of the key. 16, 24 or 32
     @param iv             [in]  Iv to be used for encryption
     @param iv_length      [in]  Length of the iv. should be 16.
     @param noPadding	   [in]  unused

  @return
    != 0           error
    0             no error
*/

int my_aes_decrypt_none(const uchar* source, uint32 source_length,
                        uchar* dest, uint32 *dest_length,
                        const unsigned char* key, uint8 key_length,
                        const unsigned char* iv, uint8 iv_length,
                        uint noPadding)
{
  memcpy(dest, source, source_length);
  *dest_length= source_length;
  return 0;
}

/**
  Initialize encryption methods
*/

my_aes_decrypt_dynamic_type my_aes_decrypt_dynamic= my_aes_decrypt_none;
my_aes_encrypt_dynamic_type my_aes_encrypt_dynamic= my_aes_encrypt_none;
enum_my_aes_encryption_algorithm current_aes_dynamic_method= MY_AES_ALGORITHM_NONE;

my_bool my_aes_init_dynamic_encrypt(enum_my_aes_encryption_algorithm method)
{
  switch (method)
  {
    /* used for encrypting tables */
  case MY_AES_ALGORITHM_ECB:
    my_aes_encrypt_dynamic= my_aes_encrypt_ecb;
    my_aes_decrypt_dynamic= my_aes_decrypt_ecb;
    break;
  case MY_AES_ALGORITHM_CBC:
    my_aes_encrypt_dynamic= my_aes_encrypt_cbc;
    my_aes_decrypt_dynamic= my_aes_decrypt_cbc;
    break;
#ifdef HAVE_EncryptAes128Ctr
    /* encrypt everything, with a set of keys */
  case MY_AES_ALGORITHM_CTR:
    my_aes_encrypt_dynamic= my_aes_encrypt_ctr;
    my_aes_decrypt_dynamic= my_aes_decrypt_ctr;
    break;
#endif
    /* Simulate encrypting interface */
  case MY_AES_ALGORITHM_NONE:
    my_aes_encrypt_dynamic= my_aes_encrypt_none;
    my_aes_decrypt_dynamic= my_aes_decrypt_none;
    break;
  default:
    return 1;
  }
  current_aes_dynamic_method= method;
  return 0;
}

my_aes_decrypt_dynamic_type
get_aes_decrypt_func(enum_my_aes_encryption_algorithm method)
{
  switch (method)
  {
    /* used for encrypting tables */
  case MY_AES_ALGORITHM_ECB:
    return my_aes_decrypt_ecb;
    break;
  case MY_AES_ALGORITHM_CBC:
    return my_aes_decrypt_cbc;
    break;
#ifdef HAVE_EncryptAes128Ctr
    /* encrypt everything, with a set of keys */
  case MY_AES_ALGORITHM_CTR:
    return my_aes_decrypt_ctr;
    break;
#endif
    /* Simulate encrypting interface */
  case MY_AES_ALGORITHM_NONE:
    return my_aes_decrypt_none;
    break;
  default:
    return NULL;
  }
  return NULL;
}

my_aes_encrypt_dynamic_type
get_aes_encrypt_func(enum_my_aes_encryption_algorithm method)
{
  switch (method)
  {
    /* used for encrypting tables */
  case MY_AES_ALGORITHM_ECB:
    return my_aes_encrypt_ecb;
    break;
  case MY_AES_ALGORITHM_CBC:
    return my_aes_encrypt_cbc;
    break;
#ifdef HAVE_EncryptAes128Ctr
    /* encrypt everything, with a set of keys */
  case MY_AES_ALGORITHM_CTR:
    return my_aes_encrypt_ctr;
    break;
#endif
    /* Simulate encrypting interface */
  case MY_AES_ALGORITHM_NONE:
    return my_aes_encrypt_none;
    break;
  default:
    return NULL;
  }
  return NULL;
}


/****************************************************************
  Encryption function visible to MariaDB users
****************************************************************/

int my_aes_encrypt(const uchar* source, int source_length, uchar* dest,
                   const char* key, int key_length)
{
#if defined(HAVE_YASSL)
  TaoCrypt::AES_ECB_Encryption enc;

  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  int num_blocks;                               /* number of complete blocks */
  int i;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;
#endif

  /* The real key to be used for encryption */
  uint8 rkey[AES_KEY_LENGTH / 8];
  int rc;                                       /* result codes */

  if ((rc= my_aes_create_key(key, key_length, rkey)))
    return rc;

#if defined(HAVE_YASSL)
  enc.SetKey((const TaoCrypt::byte *) rkey, MY_AES_BLOCK_SIZE);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  for (i = num_blocks; i > 0; i--)              /* Encode complete blocks */
  {
    enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  /* Encode the rest. We always have incomplete block */
  char pad_len = MY_AES_BLOCK_SIZE - (source_length -
                                      MY_AES_BLOCK_SIZE * num_blocks);
  memcpy(block, source, 16 - pad_len);
  memset(block + MY_AES_BLOCK_SIZE - pad_len, pad_len,  pad_len);

  enc.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) block,
              MY_AES_BLOCK_SIZE);

  return MY_AES_BLOCK_SIZE * (num_blocks + 1);
#elif defined(HAVE_OPENSSL)
  if (! EVP_EncryptInit(&ctx.ctx, EVP_aes_128_ecb(),
                        (const unsigned char *) rkey, NULL))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_EncryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned const char *) source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_EncryptFinal(&ctx.ctx, (unsigned char *) dest + u_len, &f_len))
    return AES_BAD_DATA;                        /* Error */

  return u_len + f_len;
#endif
}


/**
  DeCrypt buffer with AES encryption algorithm.

  SYNOPSIS
    my_aes_decrypt()
    @param source        [in]   Pointer to data for decryption
    @param source_length [in]   Size of encrypted data
    @param dest          [out]  Buffer to place decrypted data (must
                                be large enough)
    @param key           [in]   Key to be used for decryption
    @param key_length    [in]   Length of the key. Will handle keys of any length

  @return
    >= 0             Size of encrypted data
    < 0              Error
*/

int my_aes_decrypt(const uchar *source, int source_length, uchar *dest,
                   const char *key, int key_length)
{
#if defined(HAVE_YASSL)
  TaoCrypt::AES_ECB_Decryption dec;
  /* 128 bit block used for padding */
  uint8 block[MY_AES_BLOCK_SIZE];
  int num_blocks;                               /* Number of complete blocks */
  int i;
#elif defined(HAVE_OPENSSL)
  MyCipherCtx ctx;
  int u_len, f_len;
#endif

  /* The real key to be used for decryption */
  uint8 rkey[AES_KEY_LENGTH / 8];
  int rc;                                       /* Result codes */

  if ((rc= my_aes_create_key(key, key_length, rkey)))
    return rc;

#if defined(HAVE_YASSL)
  dec.SetKey((const TaoCrypt::byte *) rkey, MY_AES_BLOCK_SIZE);

  num_blocks = source_length / MY_AES_BLOCK_SIZE;

  if ((source_length != num_blocks * MY_AES_BLOCK_SIZE) || num_blocks == 0 )
    /* Input size has to be even and at least one block */
    return AES_BAD_DATA;

  /* Decode all but last blocks */
  for (i = num_blocks - 1; i > 0; i--)
  {
    dec.Process((TaoCrypt::byte *) dest, (const TaoCrypt::byte *) source,
                MY_AES_BLOCK_SIZE);
    source += MY_AES_BLOCK_SIZE;
    dest += MY_AES_BLOCK_SIZE;
  }

  dec.Process((TaoCrypt::byte *) block, (const TaoCrypt::byte *) source,
              MY_AES_BLOCK_SIZE);

  /* Use last char in the block as size */
  uint pad_len = (uint) (uchar) block[MY_AES_BLOCK_SIZE - 1];

  if (pad_len > MY_AES_BLOCK_SIZE)
    return AES_BAD_DATA;
  /* We could also check whole padding but we do not really need this */

  memcpy(dest, block, MY_AES_BLOCK_SIZE - pad_len);
  return MY_AES_BLOCK_SIZE * num_blocks - pad_len;
#elif defined(HAVE_OPENSSL)
  if (! EVP_DecryptInit(&ctx.ctx, EVP_aes_128_ecb(),
                        (const unsigned char *) rkey, NULL))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_DecryptUpdate(&ctx.ctx, (unsigned char *) dest, &u_len,
                          (unsigned const char *) source, source_length))
    return AES_BAD_DATA;                        /* Error */
  if (! EVP_DecryptFinal(&ctx.ctx, (unsigned char *) dest + u_len, &f_len))
    return AES_BAD_DATA;                        /* Error */
  return u_len + f_len;
#endif
}


/**
  Get size of buffer which will be large enough for encrypted data

  SYNOPSIS
    my_aes_get_size()
    @param source_length  [in] Length of data to be encrypted

  @return
    Size of buffer required to store encrypted data
*/

int my_aes_get_size(int source_length)
{
  return MY_AES_BLOCK_SIZE * (source_length / MY_AES_BLOCK_SIZE)
    + MY_AES_BLOCK_SIZE;
}
