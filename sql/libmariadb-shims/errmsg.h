/** @file
  Use the server set of client errors.
  Also avoids letting the `libmariadb` version redefining `ER()` & co..
*/
#include "../../include/errmsg.h"
