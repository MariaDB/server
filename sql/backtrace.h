/* Copyright (c) 2023, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef BACKTRACE_INCLUDED
#define BACKTRACE_INCLUDED

#include <sql_string.h>

#define ERRSTACK_MAX_LEN 2000

class sp_head;
class THD;

struct Backtrace_info
{
  int line_no;
  sp_head* sphead;
  char qname[NAME_LEN * 2 + 2]; // Copy of qualified name (db.name)
  size_t qname_length;
};

struct Error_info
{
  int err_no;
  char msg[MYSQL_ERRMSG_SIZE];
};

class Backtrace
{
public:
  Backtrace()
   :error_stack(PSI_INSTRUMENT_MEM),
    bt_list(PSI_INSTRUMENT_MEM),
    erroring_bt_list(PSI_INSTRUMENT_MEM),
    f1_sphead(NULL),
    first_call(TRUE),
    sql_condition_handled(FALSE)
  {
  }

  ~Backtrace()
  {
  }

  /**
    Reset/clear all backtrace data at every start of command execution.
  */
  void reset_backtrace_data(THD *thd);

  Dynamic_array<struct Error_info> error_stack;
  Dynamic_array<struct Backtrace_info> bt_list;
  Dynamic_array<struct Backtrace_info> erroring_bt_list;
  sp_head* f1_sphead;
  bool first_call;
  bool sql_condition_handled;
  String backtrace_std_str;
  String errstack_str;
};

#endif // BACKTRACE_INCLUDED
