#include "my_crypt.h"

#include <openssl/aes.h>
#include <openssl/rand.h>

#include "my_dbug.h"

Crypto::~Crypto() {
  EVP_CIPHER_CTX_cleanup(&ctx);
}

Crypto::Crypto() {
  EVP_CIPHER_CTX_init(&ctx);
}

// WARNING: It is allowed to have output == NULL, for special cases like AAD
// support in AES GCM. output_used however must never be NULL.
CryptResult Crypto::Crypt(const unsigned char* input, int input_size,
                          unsigned char* output, int* output_used) {
  DBUG_ASSERT(input != NULL);
  DBUG_ASSERT(output_used != NULL);
  if (!EVP_CipherUpdate(&ctx, output, output_used, input, input_size)) {
    return OPENSSL_ERROR;
  }

  return CRYPT_OK;
}

CryptResult Aes128CtrCrypto::Init(const unsigned char* key,
                                  const unsigned char* iv,
                                  int iv_size) {
  if (iv_size != 16) {
    DBUG_ASSERT(false);
    return CRYPT_BAD_IV;
  }

  if (!EVP_CipherInit_ex(&ctx, EVP_aes_128_ctr(), NULL, key, iv, mode())) {
    return OPENSSL_ERROR;
  }

  return CRYPT_OK;
}

CryptResult Aes128CtrEncrypter::Encrypt(const unsigned char* plaintext,
                                        int plaintext_size,
                                        unsigned char* ciphertext,
                                        int* ciphertext_used) {
  CryptResult res = Crypt(plaintext, plaintext_size, ciphertext,
                          ciphertext_used);
  DBUG_ASSERT(*ciphertext_used == plaintext_size);
  return res;
}

CryptResult Aes128CtrDecrypter::Decrypt(const unsigned char* ciphertext,
                                        int ciphertext_size,
                                        unsigned char* plaintext,
                                        int* plaintext_used) {
  CryptResult res = Crypt(ciphertext, ciphertext_size, plaintext,
                          plaintext_used);
  DBUG_ASSERT(*plaintext_used == ciphertext_size);
  return res;
}

CryptResult Aes128GcmCrypto::Init(const unsigned char* key,
                                  const unsigned char* iv,
                                  int iv_size) {
  if (!EVP_CipherInit_ex(&ctx, EVP_aes_128_gcm(), NULL, NULL, NULL, mode())) {
    return OPENSSL_ERROR;
  }
  if (!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_SET_IVLEN, iv_size, NULL)) {
    return OPENSSL_ERROR;
  }
  if (!EVP_CipherInit_ex(&ctx, NULL, NULL, key, iv, mode())) {
    return OPENSSL_ERROR;
  }
  return CRYPT_OK;
}

CryptResult Aes128GcmCrypto::AddAAD(const unsigned char* aad, int aad_size) {
  int outlen;
  return Crypt(aad, aad_size, NULL, &outlen);
}

CryptResult Aes128GcmEncrypter::Encrypt(const unsigned char* plaintext,
                                        int plaintext_size,
                                        unsigned char* ciphertext,
                                        int* ciphertext_used) {
  CryptResult res = Crypt(plaintext, plaintext_size, ciphertext,
                          ciphertext_used);
  DBUG_ASSERT(*ciphertext_used == plaintext_size);
  return res;
}

CryptResult Aes128GcmEncrypter::GetTag(unsigned char* tag, int tag_size) {
  unsigned char buffer[AES_128_BLOCK_SIZE];
  int buffer_used;
  if (!EVP_CipherFinal_ex(&ctx, buffer, &buffer_used)) {
    return OPENSSL_ERROR;
  }
  DBUG_ASSERT(buffer_used == 0);
  if (!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_GET_TAG, tag_size, tag)) {
    return OPENSSL_ERROR;
  }
  return CRYPT_OK;
}

CryptResult Aes128GcmDecrypter::Decrypt(const unsigned char* ciphertext,
                                        int ciphertext_size,
                                        unsigned char* plaintext,
                                        int* plaintext_used) {
  CryptResult res = Crypt(ciphertext, ciphertext_size, plaintext,
                          plaintext_used);
  DBUG_ASSERT(*plaintext_used == ciphertext_size);
  return res;
}

CryptResult Aes128GcmDecrypter::SetTag(const unsigned char* tag, int tag_size) {
  if (!EVP_CIPHER_CTX_ctrl(&ctx, EVP_CTRL_GCM_SET_TAG, tag_size,
                           (void*) tag)) {
    return OPENSSL_ERROR;
  }
  return CRYPT_OK;
}

CryptResult Aes128GcmDecrypter::CheckTag() {
  unsigned char buffer[AES_128_BLOCK_SIZE];
  int buffer_used;
  if (!EVP_CipherFinal_ex(&ctx, buffer, &buffer_used)) {
    return OPENSSL_ERROR;
  }
  DBUG_ASSERT(buffer_used == 0);
  return CRYPT_OK;
}

CryptResult Aes128EcbCrypto::Init(const unsigned char* key) {
  if (!EVP_CipherInit_ex(&ctx, EVP_aes_128_ecb(), NULL, key, NULL, mode())) {
    return OPENSSL_ERROR;
  }

  return CRYPT_OK;
}

CryptResult Aes128EcbEncrypter::Encrypt(const unsigned char* plaintext,
                                        int plaintext_size,
                                        unsigned char* ciphertext,
                                        int* ciphertext_used) {
  CryptResult res = Crypt(plaintext, plaintext_size,
                          ciphertext, ciphertext_used);
  DBUG_ASSERT(*ciphertext_used == plaintext_size);
  return res;
}

CryptResult Aes128EcbDecrypter::Decrypt(const unsigned char* ciphertext,
                                        int ciphertext_size,
                                        unsigned char* plaintext,
                                        int* plaintext_used) {
  CryptResult res = Crypt(ciphertext, ciphertext_size,
                          plaintext, plaintext_used);
  DBUG_ASSERT(*plaintext_used == ciphertext_size);
  return res;
}

extern "C" {

CryptResult EncryptAes128Ctr(const unsigned char* key,
                             const unsigned char* iv, int iv_size,
                             const unsigned char* plaintext, int plaintext_size,
                             unsigned char* ciphertext, int* ciphertext_used) {
  Aes128CtrEncrypter encrypter;

  CryptResult res = encrypter.Init(key, iv, iv_size);

  if (res != CRYPT_OK)
    return res;

  return encrypter.Encrypt(plaintext, plaintext_size, ciphertext,
                           ciphertext_used);
}

CryptResult DecryptAes128Ctr(const unsigned char* key,
                             const unsigned char* iv, int iv_size,
                             const unsigned char* ciphertext,
                             int ciphertext_size,
                             unsigned char* plaintext, int* plaintext_used) {
  Aes128CtrDecrypter decrypter;

  CryptResult res = decrypter.Init(key, iv, iv_size);

  if (res != CRYPT_OK)
    return res;

  return decrypter.Decrypt(ciphertext, ciphertext_size,
                           plaintext, plaintext_used);
}

CryptResult EncryptAes128Gcm(const unsigned char* key,
                             const unsigned char* iv, int iv_size,
                             const unsigned char* aad, int aad_size,
                             const unsigned char* plaintext, int plaintext_size,
                             unsigned char* ciphertext, int* ciphertext_used,
                             unsigned char* tag, int tag_size) {
  Aes128GcmEncrypter encrypter;

  CryptResult res = encrypter.Init(key, iv, iv_size);

  if (res != CRYPT_OK)
    return res;

  if (aad != NULL && aad_size > 0) {
    res = encrypter.AddAAD(aad, aad_size);

    if (res != CRYPT_OK)
      return res;
  }

  res = encrypter.Encrypt(plaintext, plaintext_size, ciphertext, ciphertext_used);

  if (res != CRYPT_OK)
    return res;

  return encrypter.GetTag(tag, tag_size);
}

CryptResult DecryptAes128Gcm(const unsigned char* key,
                             const unsigned char* iv, int iv_size,
                             const unsigned char* aad, int aad_size,
                             const unsigned char* ciphertext, int ciphertext_size,
                             unsigned char* plaintext, int* plaintext_used,
                             const unsigned char* expected_tag, int tag_size) {
  Aes128GcmDecrypter decrypter;

  CryptResult res = decrypter.Init(key, iv, iv_size);

  if (res != CRYPT_OK)
    return res;

  res = decrypter.SetTag(expected_tag, tag_size);

  if (res != CRYPT_OK)
    return res;

  if (aad != NULL && aad_size > 0) {
    res = decrypter.AddAAD(aad, aad_size);

    if (res != CRYPT_OK)
      return res;
  }

  res = decrypter.Decrypt(ciphertext, ciphertext_size,
                          plaintext, plaintext_used);

  if (res != CRYPT_OK)
    return res;

  return decrypter.CheckTag();
}

CryptResult EncryptAes128Ecb(const unsigned char* key,
                             const unsigned char* plaintext,
                             int plaintext_size,
                             unsigned char* ciphertext,
                             int* ciphertext_used) {
  Aes128EcbEncrypter encrypter;

  CryptResult res = encrypter.Init(key);

  if (res != CRYPT_OK)
    return res;

  return encrypter.Encrypt(plaintext, plaintext_size,
                           ciphertext, ciphertext_used);
}

CryptResult DecryptAes128Ecb(const unsigned char* key,
                             const unsigned char* ciphertext,
                             int ciphertext_size,
                             unsigned char* plaintext,
                             int* plaintext_used) {
  Aes128EcbDecrypter decrypter;

  CryptResult res = decrypter.Init(key);

  if (res != CRYPT_OK)
    return res;

  return decrypter.Decrypt(ciphertext, ciphertext_size,
                           plaintext, plaintext_used);
}

CryptResult RandomBytes(unsigned char* buf, int num) {
  // Unfortunately RAND_bytes manual page does not provide any guarantees
  // in relation to blocking behavior. Here we explicitly use SSLeay random
  // instead of whatever random engine is currently set in OpenSSL. That way we
  // are guaranteed to have a non-blocking random.
  RAND_METHOD* rand = RAND_SSLeay();
  if (rand == NULL || rand->bytes(buf, num) != 1) {
    return OPENSSL_ERROR;
  }
  return CRYPT_OK;
}

} // extern "C"
