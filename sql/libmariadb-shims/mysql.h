#ifndef _mysql_h
  #include "../../libmariadb/include/mysql.h"

  /** @file
    `include/mysql.h` says:
    > We should not define MYSQL_CLIENT when the mysql.h is included
    > by the server or server plugins.
    > Now it is important only for the SQL service to work so we rely on
    > the MYSQL_SERVICE_SQL to check we're compiling the server/plugin
    > related file.

    `libmariadb/include/mysql.h` doesn't care about the mess of our defines.
    This file is a shim to add this condition back.
  */
  #ifdef MYSQL_SERVICE_SQL
    #undef MYSQL_CLIENT
  #endif
#endif
