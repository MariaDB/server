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
  Find a cursor by reference.
  @param thd      - the current THD
  @param ref      - the field behind a SYS_REFCURSOR SP variable
  @param for_open - if the cursor is needed for OPEN rather than FETCH/CLOSE,
                    so a new cursor is appended if not found.
*/
sp_cursor_array_element *sp_cursor_array::get_cursor_by_ref(THD *thd,
                                                            Field *ref,
                                                            bool for_open)
{
  uint pos;
  if (!ref->dereference(&pos, (uint) elements()))
    return &at(pos); // "ref" points to an already initialized sp_cursor.

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
  */
  if (find_unused(&pos))
  {
    // An unused array element has not been found, append a new one
    if (Dynamic_array::append(sp_cursor_array_element()))
    {
      DBUG_ASSERT(thd->is_error());
      return nullptr;
    }
    pos= (uint) size() - 1;
  }

  /*
    Store the position of the unused/appended array element
    into the reference Field and reset the sp_cursor instance.
  */
  at(pos).reset_and_update_ref(thd, ref, pos);
  return &at(pos);
}

#endif // MYSQL_SERVER
