/*
   Copyright (c) 2025, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef STATEMENT_RCONTEXT_INCLUDED
#define STATEMENT_RCONTEXT_INCLUDED

#include "sp_cursor.h"

/*
  Top level statement-wide run time context for stored routines.
  Unlike sp_rcontext, which contains structures
  (e.g. variables, cursors, condition handlers) belonging to one stored routine,
  Statement_rcontext contains structures shared between all stored routines
  during the top level statement execution.
*/
class Statement_rcontext
{
  /*
    Open cursor counter. It watches OPEN/CLOSE for all kinds of cursors:
    - Static cursors:
        DECLARE c CURSOR FOR SELECT ...;
    - SYS_REFCURSORs:
        DECLARE c SYS_REFCURSOR;
        OPEN c FOR SELECT ...;
    It's used to respect the @@max_open_cursors system variable.
  */
  uint m_open_cursors_counter;

  sp_cursor_array m_statement_cursors;

public:
  Statement_rcontext()
   :m_open_cursors_counter(0)
  { }

  sp_cursor_array *statement_cursors()
  {
    return &m_statement_cursors;
  }

  uint open_cursors_counter() const
  {
    return m_open_cursors_counter;
  }
  void open_cursors_counter_increment()
  {
    m_open_cursors_counter++;
  }
  void open_cursors_counter_decrement()
  {
    DBUG_ASSERT(m_open_cursors_counter > 0);
    m_open_cursors_counter--;
  }

  /*
    Free all top level statement data data and reinit "this"
    for a new top level statement.
    It's called in the very end of the top level statement
    (it's not called for individual stored routune statements).
  */
  void reinit(THD *thd)
  {
    // Close SYS_REFCURSORs which did not have explicit CLOSE statement:
    m_statement_cursors.free(thd);
    // Zero open cursor counter:
    DBUG_ASSERT(open_cursors_counter() == 0);
    m_open_cursors_counter= 0; // Safety
  }
};

#endif // STATEMENT_RCONTEXT_INCLUDED
