#ifndef MY_AES_INCLUDED
#define MY_AES_INCLUDED

#define AES_OK 0
#define AES_BAD_DATA  -1
#define AES_BAD_KEYSIZE -5
#define AES_KEY_CREATION_FAILED -10

/* Copyright (c) 2002, 2006 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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


/* Header file for my_aes.c */
/* Wrapper to give simple interface for MySQL to AES standard encryption */

#include "rijndael.h"

C_MODE_START

#define AES_KEY_LENGTH 128		/* Must be 128 192 or 256 */
typedef unsigned long int ulint;


/*
  my_aes_encrypt_cbc- Crypt buffer with AES encryption algorithm using cbc mode.
  source		- Pointer to data for encryption
  source_length		- size of encryption data
  dest			- buffer to place encrypted data (must be large enough)
  key			- Key to be used for encryption
  kel_length		- Length of the key. Will handle keys of any length

  returns  - size of encrypted data, or negative in case of error.
*/
int my_aes_encrypt_cbc(const char* source, ulint source_length,
					char* dest, ulint *dest_length,
					const unsigned char* key, uint8 key_length,
					const unsigned char* iv, uint8 iv_length);


/**
 * Calculate key and iv from a given salt and secret as it is handled in openssl encrypted files via console
 *
 * SYNOPSIS
 * 	my_Bytes_To_Key()
 * 	@param salt   [in]  the given salt as extracted from the encrypted file
 * 	@param secret [in]  the given secret as String, provided by the user
 * 	@param key    [out] 32 Bytes of key are written to this pointer
 * 	@param iv     [out] 16 Bytes of iv are written to this pointer
 */
void my_bytes_to_key(const unsigned char *salt,
		const char *secret, unsigned char *key,
		unsigned char *iv);
/**
 	 Decode Hexencoded String to uint8[].
 	 my_aes_hexToUint()
 	 @param iv		[in]	Pointer to hexadecimal encoded IV String
 	 @param dest	[out]	Pointer to output uint8 array. Memory needs to be allocated by caller
 	 @param iv_length [in]  Size of destination array.
 */
void my_aes_hexToUint(const char* in,
		unsigned char *out,
		int dest_length);

/*
  my_aes_encrypt	- Crypt buffer with AES encryption algorithm.
  source		- Pointer to data for encryption
  source_length		- size of encryption data
  dest			- buffer to place encrypted data (must be large enough)
  key			- Key to be used for encryption
  kel_length		- Length of the key. Will handle keys of any length

  returns  - size of encrypted data, or negative in case of error.
*/

int my_aes_encrypt(const char *source, int source_length, char *dest,
		   const char *key, int key_length);

/*
  my_aes_decrypt_cbc	- DeCrypt buffer with AES encryption algorithm using
  	  	  	  	  cbc Mode.
  source		- Pointer to data for decryption
  source_length		- size of encrypted data
  dest			- buffer to place decrypted data (must be large enough)
  key			- Key to be used for decryption
  kel_length		- Length of the key. Will handle keys of any length

  returns  - size of original data, or negative in case of error.
*/

int my_aes_decrypt_cbc(const char* source, ulint source_length,
					char* dest, ulint *dest_length,
					const unsigned char* key, uint8 key_length,
					const unsigned char* iv, uint8 iv_length);

/*
  my_aes_decrypt	- DeCrypt buffer with AES encryption algorithm.
  source		- Pointer to data for decryption
  source_length		- size of encrypted data
  dest			- buffer to place decrypted data (must be large enough)
  key			- Key to be used for decryption
  kel_length		- Length of the key. Will handle keys of any length

  returns  - size of original data, or negative in case of error.
*/


int my_aes_decrypt(const char *source, int source_length, char *dest,
		   const char *key, int key_length);

/*
  my_aes_get_size - get size of buffer which will be large enough for encrypted
		    data
  source_length   -  length of data to be encrypted

  returns  - size of buffer required to store encrypted data
*/

int my_aes_get_size(int source_length);

C_MODE_END

#endif /* MY_AES_INCLUDED */
