
#ifndef INCLUDE_MY_CRYPT_KEY_MANAGMENT_INCLUDED
#define INCLUDE_MY_CRYPT_KEY_MANAGMENT_INCLUDED

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
 * Functions to interact with key management
 */

uint get_latest_encryption_key_version();
uint has_encryption_key(uint version);
uint get_encryption_key_size(uint version);
int get_encryption_key(uint version, uchar* key, uint size);
int get_encryption_iv(uint version, uchar* iv, uint size);

C_MODE_END

#endif // INCLUDE_MY_CRYPT_KEY_MANAGMENT_INCLUDED
