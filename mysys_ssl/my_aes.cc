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
