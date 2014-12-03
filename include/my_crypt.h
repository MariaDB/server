
#ifndef MYSYS_MY_CRYPT_H_
#define MYSYS_MY_CRYPT_H_

#include <assert.h>
#include <openssl/evp.h>

#include "my_global.h"
#include "mysql.h"
#include "my_attribute.h"
#include "my_dbug.h"

enum CryptResult {
  CRYPT_OK = 0,
  CRYPT_BAD_IV,
  CRYPT_INVALID,
  OPENSSL_ERROR
};

static const int AES_128_BLOCK_SIZE = 16;
static const int CRYPT_ENCRYPT = 1;
static const int CRYPT_DECRYPT = 0;

#ifdef __cplusplus

class Encrypter {
 public:
  virtual ~Encrypter() {}

  virtual CryptResult Encrypt(const unsigned char* plaintext,
                              int plaintext_size,
                              unsigned char* ciphertext,
                              int* ciphertext_used) = 0;
  virtual CryptResult GetTag(unsigned char* tag, int tag_size) = 0;
};

class Decrypter {
 public:
  virtual ~Decrypter() {}

  virtual CryptResult SetTag(const unsigned char* tag, int tag_size) = 0;
  virtual CryptResult Decrypt(const unsigned char* ciphertext,
                              int ciphertext_size,
                              unsigned char* plaintext,
                              int* plaintext_used) = 0;
  virtual CryptResult CheckTag() = 0;
};

class Crypto {
 public:
  virtual ~Crypto();

  CryptResult Crypt(const unsigned char* input, int input_size,
                    unsigned char* output, int* output_used);

 protected:
  Crypto();

  EVP_CIPHER_CTX ctx;
};

// Various crypto implementations

class Aes128CtrCrypto : public Crypto {
 public:
  virtual CryptResult Init(const unsigned char* key, const unsigned char* iv,
                           int iv_size);

 protected:
  Aes128CtrCrypto() {}

  virtual int mode() = 0;
};

class Aes128CtrEncrypter : public Aes128CtrCrypto, public Encrypter {
 public:
  Aes128CtrEncrypter() {}
  virtual CryptResult Encrypt(const unsigned char* plaintext,
                              int plaintext_size,
                              unsigned char* ciphertext,
                              int* ciphertext_used);

  virtual CryptResult GetTag(unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
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
  virtual CryptResult Decrypt(const unsigned char* ciphertext,
                              int ciphertext_size,
                              unsigned char* plaintext,
                              int* plaintext_used);

  virtual CryptResult SetTag(const unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
  }

  virtual CryptResult CheckTag() {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_DECRYPT;
  }

 private:
  Aes128CtrDecrypter(const Aes128CtrDecrypter& o);
  Aes128CtrDecrypter& operator=(const Aes128CtrDecrypter& o);
};

class Aes128GcmCrypto : public Crypto {
 public:
  CryptResult Init(const unsigned char* key, const unsigned char* iv,
                   int iv_size);

  virtual CryptResult AddAAD(const unsigned char* aad, int aad_size);

 protected:
  Aes128GcmCrypto() {}

  virtual int mode() = 0;
};

class Aes128GcmEncrypter : public Aes128GcmCrypto, public Encrypter {
 public:
  Aes128GcmEncrypter() {}
  virtual CryptResult Encrypt(const unsigned char* plaintext,
                              int plaintext_size,
                              unsigned char* ciphertext,
                              int* ciphertext_used);

  virtual CryptResult GetTag(unsigned char* tag, int tag_size);

 protected:
  virtual int mode() {
    return CRYPT_ENCRYPT;
  }

 private:
  Aes128GcmEncrypter(const Aes128GcmEncrypter& o);
  Aes128GcmEncrypter& operator=(const Aes128GcmEncrypter& o);
};

class Aes128GcmDecrypter : public Aes128GcmCrypto, public Decrypter {
 public:
  Aes128GcmDecrypter() {}

  virtual CryptResult Decrypt(const unsigned char* ciphertext,
                              int ciphertext_size,
                              unsigned char* plaintext,
                              int* plaintext_used);

  virtual CryptResult SetTag(const unsigned char* tag, int tag_size);

  virtual CryptResult CheckTag();

 protected:
  virtual int mode() {
    return CRYPT_DECRYPT;
  }


 private:
  Aes128GcmDecrypter(const Aes128GcmDecrypter& o);
  Aes128GcmDecrypter& operator=(const Aes128GcmDecrypter& o);
};

class Aes128EcbCrypto : public Crypto {
 public:
  virtual CryptResult Init(const unsigned char* key);

 protected:
  Aes128EcbCrypto() {}

  virtual int mode() = 0;
};

class Aes128EcbEncrypter : public Aes128EcbCrypto, public Encrypter {
 public:
  Aes128EcbEncrypter() {}
  virtual CryptResult Encrypt(const unsigned char* plaintext,
                              int plaintext_size,
                              unsigned char* ciphertext,
                              int* ciphertext_used);

  virtual CryptResult GetTag(unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
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
  virtual CryptResult Decrypt(const unsigned char* ciphertext,
                              int ciphertext_size,
                              unsigned char* plaintext,
                              int* plaintext_used);

  virtual CryptResult SetTag(const unsigned char* tag, int tag_size) {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
  }

  virtual CryptResult CheckTag() {
    DBUG_ASSERT(false);
    return CRYPT_INVALID;
  }

 protected:
  virtual int mode() {
    return CRYPT_DECRYPT;
  }

 private:
  Aes128EcbDecrypter(const Aes128EcbDecrypter& o);
  Aes128EcbDecrypter& operator=(const Aes128EcbDecrypter& o);
};

#endif

C_MODE_START

enum CryptResult EncryptAes128Ctr(const unsigned char* key,
                                  const unsigned char* iv, int iv_size,
                                  const unsigned char* plaintext, int plaintext_size,
                                  unsigned char* ciphertext, int* ciphertext_used);

enum CryptResult DecryptAes128Ctr(const unsigned char* key,
                                  const unsigned char* iv, int iv_size,
                                  const unsigned char* ciphertext, int ciphertext_size,
                                  unsigned char* plaintext, int* plaintext_used);

enum CryptResult EncryptAes128Gcm(const unsigned char* key,
                                  const unsigned char* iv, int iv_size,
                                  const unsigned char* aad, int aad_size,
                                  const unsigned char* plaintext, int plaintext_size,
                                  unsigned char* ciphertext, int* ciphertext_used,
                                  unsigned char* tag, int tag_size);

enum CryptResult DecryptAes128Gcm(const unsigned char* key,
                                  const unsigned char* iv, int iv_size,
                                  const unsigned char* aad, int aad_size,
                                  const unsigned char* ciphertext, int ciphertext_size,
                                  unsigned char* plaintext, int* plaintext_used,
                                  const unsigned char* expected_tag, int tag_size);

enum CryptResult EncryptAes128Ecb(const unsigned char* key,
                                  const unsigned char* plaintext, int plaintext_size,
                                  unsigned char* ciphertext, int* ciphertext_used);

enum CryptResult DecryptAes128Ecb(const unsigned char* key,
                                  const unsigned char* ciphertext, int ciphertext_size,
                                  unsigned char* plaintext, int* plaintext_used);

enum CryptResult RandomBytes(unsigned char* buf, int num);

C_MODE_END

#endif // MYSYS_MY_CRYPT_H_
