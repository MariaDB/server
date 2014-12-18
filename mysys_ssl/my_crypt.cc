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
#include <openssl/rand.h>

static const int CRYPT_ENCRYPT = 1;
static const int CRYPT_DECRYPT = 0;

class Encrypter {
 public:
  virtual ~Encrypter() {}

  virtual Crypt_result Encrypt(const uchar* plaintext,
                               int plaintext_size,
                               uchar* ciphertext,
                               int* ciphertext_used) = 0;
  virtual Crypt_result GetTag(uchar* tag, int tag_size) = 0;
};

class Decrypter {
 public:
  virtual ~Decrypter() {}

  virtual Crypt_result SetTag(const uchar* tag, int tag_size) = 0;
  virtual Crypt_result Decrypt(const uchar* ciphertext,
                               int ciphertext_size,
                               uchar* plaintext,
                               int* plaintext_used) = 0;
  virtual Crypt_result CheckTag() = 0;
};

class Crypto {
 public:
  virtual ~Crypto();

  Crypt_result Crypt(const uchar* input, int input_size,
                     uchar* output, int* output_used);

 protected:
  Crypto();

  EVP_CIPHER_CTX ctx;
};


/* Various crypto implementations */

class Aes128CtrCrypto : public Crypto {
 public:
  virtual Crypt_result Init(const uchar* key, const uchar* iv,
                            int iv_size);

 protected:
  Aes128CtrCrypto() {}

  virtual int mode() = 0;
};

class Aes128CtrEncrypter : public Aes128CtrCrypto, public Encrypter {
 public:
  Aes128CtrEncrypter() {}
  virtual Crypt_result Encrypt(const uchar* plaintext,
                               int plaintext_size,
                               uchar* ciphertext,
                               int* ciphertext_used);

  virtual Crypt_result GetTag(uchar* tag, int tag_size) {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_ENCRYPT;
  }

 private:
  Aes128CtrEncrypter(const Aes128CtrEncrypter& o);
  Aes128CtrEncrypter& operator=(const Aes128CtrEncrypter& o);
};

class Aes128CtrDecrypter : public Aes128CtrCrypto, public Decrypter {
 public:
  Aes128CtrDecrypter() {}
  virtual Crypt_result Decrypt(const uchar* ciphertext,
                               int ciphertext_size,
                               uchar* plaintext,
                               int* plaintext_used);

  virtual Crypt_result SetTag(const uchar* tag, int tag_size) {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

  virtual Crypt_result CheckTag() {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_DECRYPT;
  }

 private:
  Aes128CtrDecrypter(const Aes128CtrDecrypter& o);
  Aes128CtrDecrypter& operator=(const Aes128CtrDecrypter& o);
};

class Aes128EcbCrypto : public Crypto {
 public:
  virtual Crypt_result Init(const unsigned char* key);

 protected:
  Aes128EcbCrypto() {}

  virtual int mode() = 0;
};

class Aes128EcbEncrypter : public Aes128EcbCrypto, public Encrypter {
 public:
  Aes128EcbEncrypter() {}
  virtual Crypt_result Encrypt(const unsigned char* plaintext,
                               int plaintext_size,
                               unsigned char* ciphertext,
                               int* ciphertext_used);

  virtual Crypt_result GetTag(unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_ENCRYPT;
  }

 private:
  Aes128EcbEncrypter(const Aes128EcbEncrypter& o);
  Aes128EcbEncrypter& operator=(const Aes128EcbEncrypter& o);
};

class Aes128EcbDecrypter : public Aes128EcbCrypto, public Decrypter {
 public:
  Aes128EcbDecrypter() {}
  virtual Crypt_result Decrypt(const unsigned char* ciphertext,
                              int ciphertext_size,
                              unsigned char* plaintext,
                              int* plaintext_used);

  virtual Crypt_result SetTag(const unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

  virtual Crypt_result CheckTag() {
    DBUG_ASSERT(false);
    return AES_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_DECRYPT;
  }

 private:
  Aes128EcbDecrypter(const Aes128EcbDecrypter& o);
  Aes128EcbDecrypter& operator=(const Aes128EcbDecrypter& o);
};


Crypto::~Crypto() {
  EVP_CIPHER_CTX_cleanup(&ctx);
}

Crypto::Crypto() {
  EVP_CIPHER_CTX_init(&ctx);
}

/*
  WARNING: It is allowed to have output == NULL, for special cases like AAD
  support in AES GCM. output_used however must never be NULL.
*/

Crypt_result Crypto::Crypt(const uchar* input, int input_size,
                           uchar* output, int* output_used) {
  DBUG_ASSERT(input != NULL);
  DBUG_ASSERT(output_used != NULL);
  if (!EVP_CipherUpdate(&ctx, output, output_used, input, input_size)) {
    return AES_OPENSSL_ERROR;
  }

  return AES_OK;
}

Crypt_result Aes128CtrCrypto::Init(const uchar* key,
                                   const uchar* iv,
                                   int iv_size) {
  if (iv_size != 16) {
    DBUG_ASSERT(false);
    return AES_BAD_IV;
  }

  if (!EVP_CipherInit_ex(&ctx, EVP_aes_128_ctr(), NULL, key, iv, mode())) {
    return AES_OPENSSL_ERROR;
  }

  return AES_OK;
}

Crypt_result Aes128CtrEncrypter::Encrypt(const uchar* plaintext,
                                         int plaintext_size,
                                         uchar* ciphertext,
                                         int* ciphertext_used) {
  Crypt_result res = Crypt(plaintext, plaintext_size, ciphertext,
                          ciphertext_used);
  DBUG_ASSERT(*ciphertext_used == plaintext_size);
  return res;
}

Crypt_result Aes128CtrDecrypter::Decrypt(const uchar* ciphertext,
                                         int ciphertext_size,
                                         uchar* plaintext,
                                         int* plaintext_used) {
  Crypt_result res = Crypt(ciphertext, ciphertext_size, plaintext,
                           plaintext_used);
  DBUG_ASSERT(*plaintext_used == ciphertext_size);
  return res;
}


Crypt_result Aes128EcbCrypto::Init(const unsigned char* key) {
  if (!EVP_CipherInit_ex(&ctx, EVP_aes_128_ecb(), NULL, key, NULL, mode())) {
    return AES_OPENSSL_ERROR;
  }

  return AES_OK;
}

Crypt_result Aes128EcbEncrypter::Encrypt(const unsigned char* plaintext,
                                        int plaintext_size,
                                        unsigned char* ciphertext,
                                        int* ciphertext_used) {
  Crypt_result res = Crypt(plaintext, plaintext_size,
                           ciphertext, ciphertext_used);
  DBUG_ASSERT(*ciphertext_used == plaintext_size);
  return res;
}

Crypt_result Aes128EcbDecrypter::Decrypt(const unsigned char* ciphertext,
                                         int ciphertext_size,
                                         unsigned char* plaintext,
                                         int* plaintext_used) {
  Crypt_result res = Crypt(ciphertext, ciphertext_size,
                           plaintext, plaintext_used);
  DBUG_ASSERT(*plaintext_used == ciphertext_size);
  return res;
}

C_MODE_START


  /* Encrypt and decrypt according to Aes128Ctr */

Crypt_result my_aes_encrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  Aes128CtrEncrypter encrypter;
  Crypt_result res = encrypter.Init(key, iv, iv_length);
  if (res != AES_OK)
    return res;
  return encrypter.Encrypt(source, source_length, dest, (int*)dest_length);
}


Crypt_result my_aes_decrypt_ctr(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  Aes128CtrDecrypter decrypter;

  Crypt_result res = decrypter.Init(key, iv, iv_length);
  if (res != AES_OK)
    return res;
  return decrypter.Decrypt(source, source_length, dest, (int*)dest_length);
}


Crypt_result my_aes_encrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  Aes128EcbEncrypter encrypter;
  Crypt_result res = encrypter.Init(key);
  if (res != AES_OK)
    return res;
  return encrypter.Encrypt(source, source_length, dest, (int*)dest_length);
}

Crypt_result my_aes_decrypt_ecb(const uchar* source, uint32 source_length,
                                uchar* dest, uint32* dest_length,
                                const unsigned char* key, uint8 key_length,
                                const unsigned char* iv, uint8 iv_length,
                                uint noPadding)
{
  Aes128EcbDecrypter decrypter;

  Crypt_result res = decrypter.Init(key);

  if (res != AES_OK)
    return res;
  return decrypter.Decrypt(source, source_length, dest, (int*)dest_length);
}

C_MODE_END

#endif /* HAVE_EncryptAes128Ctr */

#if defined(HAVE_YASSL)

#include <random.hpp>

C_MODE_START

Crypt_result my_random_bytes(uchar* buf, int num)
{
  TaoCrypt::RandomNumberGenerator rand;
  rand.GenerateBlock((TaoCrypt::byte*) buf, num);
  return AES_OK;
}

C_MODE_END

#else  /* OpenSSL */

C_MODE_START

Crypt_result my_random_bytes(uchar* buf, int num)
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
