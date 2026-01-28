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

#ifndef BACKTRACE_INL_INCLUDED
#define BACKTRACE_INL_INCLUDED

#include "sql_class.h"

inline void Backtrace::reset_backtrace_data(THD *thd)
{
  first_call= TRUE;

  thd->variables.backtrace_str= (char*)"";
  thd->variables.errstack_str= (char*)"";
  errstack_str.set("", 0, system_charset_info);
  backtrace_std_str.set("", 0, system_charset_info);
  sql_condition_handled= FALSE;
}

#endif // BACKTRACE_INL_INCLUDED
