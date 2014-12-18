// TODO: Add Windows support

#ifndef MYSYS_MY_CRYPT_H_
#define MYSYS_MY_CRYPT_H_

/* We expect same result code from encryption functions as in my_aes.h */
#include <my_aes.h>

typedef int Crypt_result;

#if !defined(HAVE_YASSL) && defined(HAVE_OPENSSL)

#define HAVE_EncryptAes128Ctr

C_MODE_START
Crypt_result my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding);

Crypt_result my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding);
C_MODE_END

Crypt_result EncryptAes128Ctr(const uchar* key,
                              const uchar* iv, int iv_size,
                              const uchar* plaintext, int plaintext_size,
                              uchar* ciphertext, int* ciphertext_used);

Crypt_result DecryptAes128Ctr(const uchar* key,
                              const uchar* iv, int iv_size,
                              const uchar* ciphertext, int ciphertext_size,
                              uchar* plaintext, int* plaintext_used);

#endif /* !defined(HAVE_YASSL) && defined(HAVE_OPENSSL) */

C_MODE_START
Crypt_result my_random_bytes(uchar* buf, int num);
C_MODE_END

#endif /* MYSYS_MY_CRYPT_H_ */
