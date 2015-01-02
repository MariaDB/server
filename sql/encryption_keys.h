#ifndef SQL_CRYPTOKEY_INCLUDED
#define SQL_CRYPTOKEY_INCLUDED

#include "my_global.h"

#ifndef DBUG_OFF
  extern my_bool debug_use_static_encryption_keys;
extern uint opt_debug_encryption_key_version;
#endif /* DBUG_OFF */

#endif // SQL_CRYPTOKEY_INCLUDED
