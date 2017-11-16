/* Copyright (c) 2015, 2017 Tencent.
Use is subject to license terms

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#ifndef REPL_SPEEDLIMIT_H
#define REPL_SPEEDLIMIT_H

#define MYSQL_SERVER
#define HAVE_REPLICATION

#include "unireg.h"
#include <my_global.h>
#include <mysql/plugin.h>
#include <replication.h>
#include "log.h"                                /* sql_print_information */



typedef struct st_mysql_show_var SHOW_VAR;
typedef struct st_mysql_sys_var SYS_VAR;


uint64 get_current_ms();
void sleep_ms(int64 micro_second);

/**
   This class is used to trace function calls and other process
   information
*/
class Trace {
public:
  static const unsigned long kTraceFunction;
  static const unsigned long kTraceGeneral;
  static const unsigned long kTraceDetail;
  static const unsigned long kTraceTimer;

  unsigned long           trace_level_;                      /* the level for tracing */

  inline void function_enter(const char *func_name)
  {
    if (trace_level_ & kTraceFunction)
      sql_print_information("---> %s enter", func_name);
  }

  inline int  function_exit(const char *func_name, int exit_code)
  {
    if (trace_level_ & kTraceFunction)
      sql_print_information("<--- %s exit (%d)", func_name, exit_code);
    return exit_code;
  }

  inline void function_exit(const char *func_name)
  {
    if (trace_level_ & kTraceFunction)
      sql_print_information("<--- %s exit", func_name);
  }

  Trace()
    :trace_level_(0L)
  {}

  Trace(unsigned long trace_level)
    :trace_level_(trace_level)
  {}
};


#endif /* REPL_SPEEDLIMIT_H */
