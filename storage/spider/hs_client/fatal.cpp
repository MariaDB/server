
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011-2017 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include <my_global.h>
#include "mysql_version.h"
#include "sql_priv.h"
#include "probes_mysql.h"

#include "fatal.hpp"

namespace dena {

/*
const int opt_syslog = LOG_ERR | LOG_PID | LOG_CONS;
*/

void
fatal_abort(const String& message)
{
  fprintf(stderr, "FATAL_COREDUMP: %s\n", message.ptr());
/*
  syslog(opt_syslog, "FATAL_COREDUMP: %s", message.ptr());
*/
  abort();
}

void
fatal_abort(const char *message)
{
  fprintf(stderr, "FATAL_COREDUMP: %s\n", message);
/*
  syslog(opt_syslog, "FATAL_COREDUMP: %s", message);
*/
  abort();
}

};

