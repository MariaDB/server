#include <my_global.h>
#include <my_crypt_key_management.h>
#include <cstring>
#ifdef __linux__
#include <arpa/inet.h>
#endif

#ifndef DBUG_OFF
my_bool opt_danger_danger_use_dbug_keys = 0;

#ifdef HAVE_PSI_INTERFACE
PSI_rwlock_key key_LOCK_dbug_crypto_key_version;
#endif
mysql_rwlock_t LOCK_dbug_crypto_key_version;
unsigned int opt_danger_danger_dbug_crypto_key_version = 0;
#endif

/**
 * Default functions
 */
int GetLatestCryptoKeyVersionImpl();
unsigned int HasCryptoKeyImpl(unsigned int version);
int GetCryptoKeySizeImpl(unsigned int version);
int GetCryptoKeyImpl(unsigned int version, unsigned char* key_buffer,
                     unsigned int size);
int GetCryptoIVImpl(unsigned int version, unsigned char* key_buffer,
                    unsigned int size);

/**
 * Function pointers for
 * - GetLatestCryptoKeyVersion
 * - GetCryptoKey
 */
static
struct CryptoKeyFuncs_t cryptoKeyFuncs = {
  GetLatestCryptoKeyVersionImpl,
  HasCryptoKeyImpl,
  GetCryptoKeySizeImpl,
  GetCryptoKeyImpl,
  GetCryptoIVImpl
};

extern "C"
int GetLatestCryptoKeyVersion() {
#ifndef DBUG_OFF
  if (opt_danger_danger_use_dbug_keys) {
    mysql_rwlock_rdlock(&LOCK_dbug_crypto_key_version);
    unsigned int res = opt_danger_danger_dbug_crypto_key_version;
    mysql_rwlock_unlock(&LOCK_dbug_crypto_key_version);
    return res;
  }
#endif

  return (* cryptoKeyFuncs.getLatestCryptoKeyVersionFunc)();
}

extern "C"
unsigned int HasCryptoKey(unsigned int version) {
  return (* cryptoKeyFuncs.hasCryptoKeyFunc)(version);
}

extern "C"
int GetCryptoKeySize(unsigned int version) {
  return (* cryptoKeyFuncs.getCryptoKeySize)(version);
}

extern "C"
int GetCryptoKey(unsigned int version, unsigned char* key, unsigned int size) {
#ifndef DBUG_OFF
  if (opt_danger_danger_use_dbug_keys) {
    memset(key, 0, size);
    // Just don't support tiny keys, no point anyway.
    if (size < sizeof(version)) {
      return 1;
    }

    version = htonl(version);
    memcpy(key, &version, sizeof(version));
    return 0;
  }
#endif

  return (* cryptoKeyFuncs.getCryptoKeyFunc)(version, key, size);
}

extern "C"
int GetCryptoIV(unsigned int version, unsigned char* key, unsigned int size) {
  return (* cryptoKeyFuncs.getCryptoIVFunc)(version, key, size);
}

extern "C"
void
InstallCryptoKeyFunctions(const struct CryptoKeyFuncs_t* _cryptoKeyFuncs)
{
  if (_cryptoKeyFuncs == NULL)
  {
    /* restore defaults wHashhen called with NULL argument */
    cryptoKeyFuncs.getLatestCryptoKeyVersionFunc =
        GetLatestCryptoKeyVersionImpl;
    cryptoKeyFuncs.hasCryptoKeyFunc =
	HasCryptoKeyImpl;
    cryptoKeyFuncs.getCryptoKeySize =
	GetCryptoKeySizeImpl;
    cryptoKeyFuncs.getCryptoKeyFunc =
        GetCryptoKeyImpl;
    cryptoKeyFuncs.getCryptoIVFunc =
        GetCryptoIVImpl;
  }
  else
  {
    cryptoKeyFuncs = *_cryptoKeyFuncs;
  }
}
