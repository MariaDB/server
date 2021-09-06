/* Copyright (c) 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_class.h"
#include "debug.h"

/**
   Debug utility to do crash after a set number of executions

   The user variable, either @debug_crash_counter or @debug_error_counter,
   is decremented each time debug_crash() or debug_simulate_error is called
   if the keyword is set with @@debug_push, like
   @@debug_push="d+frm_data_type_info_emulate"

   If the variable is not set or is not an integer it will be ignored.
*/

#ifndef DBUG_OFF

static const LEX_CSTRING debug_crash_counter=
{ STRING_WITH_LEN("debug_crash_counter") };
static const LEX_CSTRING debug_error_counter=
{ STRING_WITH_LEN("debug_error_counter") };

static bool debug_decrement_counter(const LEX_CSTRING *name)
{
  THD *thd= current_thd;
  user_var_entry *entry= (user_var_entry*)
    my_hash_search(&thd->user_vars, (uchar*) name->str, name->length);
  if (!entry || entry->type != INT_RESULT || ! entry->value)
    return 0;
  (*(ulonglong*) entry->value)= (*(ulonglong*) entry->value)-1;
  return !*(ulonglong*) entry->value;
}

void debug_crash_here(const char *keyword)
{
  DBUG_ENTER("debug_crash_here");
  DBUG_PRINT("enter", ("keyword: %s", keyword));

  DBUG_EXECUTE_IF(keyword,
                  if (debug_decrement_counter(&debug_crash_counter))
                  {
                    my_printf_error(ER_INTERNAL_ERROR,
                                    "Crashing at %s",
                                    MYF(ME_ERROR_LOG | ME_NOTE), keyword);
                    DBUG_SUICIDE();
                  });
  DBUG_VOID_RETURN;
}

/*
  This can be used as debug_counter to simulate an error at a specific
  position.

  Typical usage would be
  if (debug_simualte_error("keyword"))
    error= 1;
*/

bool debug_simulate_error(const char *keyword, uint error)
{
  DBUG_ENTER("debug_crash_here");
  DBUG_PRINT("enter", ("keyword: %s", keyword));
  DBUG_EXECUTE_IF(keyword,
                  if (debug_decrement_counter(&debug_error_counter))
                  {
                    my_printf_error(error,
                                    "Simulating error for '%s'",
                                    MYF(ME_ERROR_LOG), keyword);
                    DBUG_RETURN(1);
                  });
  DBUG_RETURN(0);
}
#endif /* DBUG_OFF */
