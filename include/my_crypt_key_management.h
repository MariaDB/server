
#ifndef MYSYS_MY_CRYPT_KEY_MANAGMENT_H_
#define MYSYS_MY_CRYPT_KEY_MANAGMENT_H_

#include "my_global.h"
#include "my_pthread.h"
#include "mysql/psi/psi.h"

#ifndef DBUG_OFF
extern my_bool debug_use_static_encryption_keys;

#ifdef HAVE_PSI_INTERFACE
extern PSI_rwlock_key key_LOCK_dbug_encryption_key_version;
#endif

extern mysql_rwlock_t LOCK_dbug_encryption_key_version;
extern uint opt_debug_encryption_key_version;
#endif /* DBUG_OFF */

C_MODE_START
/**
 * function returning latest key version
 */
typedef int (* GetLatestCryptoKeyVersionFunc_t)();

/**
 * function returning if the key exists
 */
typedef unsigned int (* HasKeyVersionFunc_t)(unsigned int version);

/**
 * function returning the key size
 */
typedef int (* GetKeySizeFunc_t)(unsigned int version);

/**
 * function returning a key for a key version
 */
typedef int (* GetCryptoKeyFunc_t)(unsigned int version,
                                   unsigned char* key,
                                   unsigned keybufsize);

/**
 * function returning an iv for a key version
 */
typedef int (* GetCryptoIVFunc_t)(unsigned int version,
                                   unsigned char* iv,
                                   unsigned ivbufsize);


struct CryptoKeyFuncs_t
{
  GetLatestCryptoKeyVersionFunc_t getLatestCryptoKeyVersionFunc;
  HasKeyVersionFunc_t hasCryptoKeyFunc;
  GetKeySizeFunc_t getCryptoKeySize;
  GetCryptoKeyFunc_t getCryptoKeyFunc;
  GetCryptoIVFunc_t getCryptoIVFunc;
};

/**
 * Install functions to use for key management
 */
void
InstallCryptoKeyFunctions(const struct CryptoKeyFuncs_t* cryptoKeyFuncs);

/**
 * Functions to interact with key management
 */

int GetLatestCryptoKeyVersion();
unsigned int HasCryptoKey(unsigned int version);
int GetCryptoKeySize(unsigned int version);
int GetCryptoKey(unsigned int version, unsigned char* key_buffer,
                 unsigned int size);
int GetCryptoIV(unsigned int version, unsigned char* key_buffer,
                unsigned int size);

C_MODE_END

#endif // MYSYS_MY_CRYPT_KEY_MANAGMENT_H_
