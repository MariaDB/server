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


void sp_cursor::raise_incompatible_row_size(uint sz0, uint sz1)
{
  class RowType: public CharBuffer<6 + MAX_BIGINT_WIDTH>
  {
  public:
    RowType(uint sz)
    { copy("row<"_LEX_CSTRING).append_ulonglong(sz).append_char('>'); }
  };
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
           RowType(sz0).ptr(), RowType(sz1).ptr(), "OPEN .. FOR");
}

/*
  Append a new element into the array.
  @return  NULL Type_ref_null (with is_null()==true) on error (EOM).
  @return  not-NULL Type_ref_null on success
*/
Type_ref_null sp_cursor_array::append(THD *thd)
{
  if (Dynamic_array::append(sp_cursor_array_element()))
  {
    DBUG_ASSERT(thd->is_error());
    return Type_ref_null();
  }
  return Type_ref_null((ulonglong) size() - 1);
}


/*
  Find a cursor by reference.
  @param thd      - the current THD
  @param ref      - the field behind a SYS_REFCURSOR SP variable
  @param for_open - if the cursor is needed for OPEN rather than FETCH/CLOSE,
                    so a new cursor is appended if not found.
*/
sp_cursor_array_element *sp_cursor_array::get_cursor_by_ref(THD *thd,
                                                            Field *ref_field,
                                                            bool for_open)
{
  Type_ref_null ref= ref_field->val_ref(thd);
  if (ref < (ulonglong) elements())
    return &at((size_t) ref.value());// "ref" points to an initialized sp_cursor

  if (!for_open)
    return nullptr;

  /*
    We are here when:
    - The reference ref.is_null() is true, meaning that the
      ref_field's SP variable is not linked to any cursors in "this" array:
      * it is the very first "OPEN .. FOR STMT" command for ref_field.
      * or the ref_field's SP variable was set to NULL explicitly.
    - Or ref_field for some reasons returned a cursor offset outside
      or the range [0..elements()-1].
  */
  if (((ref= find_unused()).is_null() && (ref= append(thd)).is_null()) ||
      ref_field->store_ref(ref, true/*no_conversions*/))
    return nullptr;

  at((size_t) ref.value()).reset(thd, 1/*ref count*/);
  return &at((size_t) ref.value());
}

#endif // MYSQL_SERVER
