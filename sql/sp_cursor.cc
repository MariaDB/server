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


#ifdef MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"


/*
  Find a cursor available for open.
  @param thd - the current THD
  @param ref - the field behind a SYS_REFCURSOR SP variable
*/
sp_cursor *sp_cursor_array::get_cursor_by_ref(THD *thd,
                                              Field *ref,
                                              bool for_open)
{
  uint pos;
  if (!Sp_rcontext_handler::dereference(ref, &pos, (uint) elements()))
  {
    /*
      "ref" points to an initialized sp_cursor. It can be closed or open.
      Two consequent OPEN (without a CLOSE in between) are allowed
      for SYS_REFCURSORs (unlike for static CURSORs).
      Close the first cursor automatically if it's open, e.g.:
        OPEN c FOR SELECT 1;
        OPEN c FOR SELECT 2;
      Let's also reuse the same sp_cursor instance
      to guarantee cursor aliasing works as expected:
        OPEN c0 FOR SELECT 1;
        SET c1= c0;           -- Creating an alias
        OPEN c0 FOR SELECT 2; -- Reopening affects both c0 and c1
        FETCH c1 INTO a;      -- Fetches "2", from the second "OPEN c0"
    */
    return &at(pos);
  }

  if (!for_open)
    return nullptr;

  /*
    We are here when:
    - The reference ref->is_null() returned true, meaning that
      ref's SP variable is not linked to any curors in this array:
      * this is the very first "OPEN .. FOR STMT" command for "ref"
      * or the ref's SP variable was set to NULL explicitly.
    - Or dereference() for some reasons returned a value greater than
      elements().
    Search for an unused sp_cursor instance inside sp_cursor_array.
  */
  if (!find_unused(&pos))
  {
    /*
      An unused sp_cursor instance has been found at the offset "pos".
      Store the position of the found sp_cursor into the reference Field
      and reset the sp_cursor.
    */
    ref->set_notnull();
    ref->store(pos, true/*unsigned_flag*/);
    at(pos).reset(thd);
    return &at(pos);
  }

  // No unused cursors were found. Append a new one.
  return append(thd, ref);
}


/*
  Append a new cursor into the array.
*/
sp_cursor *sp_cursor_array::append(THD *thd, Field *ref)
{
  if (Dynamic_array::append(sp_cursor()))
    return nullptr;  // The EOM error should already be in DA

  uint pos= (uint) size() - 1;
  ref->set_notnull();
  ref->store(pos, true/*unsigned_flag*/);
  at(pos).reset(thd);
  return &at(pos);
}

#endif
