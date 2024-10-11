/*
   Copyright (c) 2025, Rakuten Securities
   Copyright (c) 2025, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#include "sql_type.h"
#include "sql_type_row.h"
#include "item.h"
#include "sql_select.h"

bool Type_handler_row::get_item_index(THD *thd,
                                      const Item_field *item,
                                      const LEX_CSTRING& name,
                                      uint& idx) const
{
  auto item_row=
    dynamic_cast<Item_field_row *>(const_cast<Item_field *> (item));
  DBUG_ASSERT(item_row);

  auto vtable= item_row->field->virtual_tmp_table();
  if (!vtable)
    return true;
  
  return vtable->sp_find_field_by_name_or_error(&idx,
                                                item_row->field->field_name,
                                                name);
}


Item_field *Type_handler_row::get_item(THD *thd,
                                       const Item_field *item,
                                       const LEX_CSTRING& name) const
{
  auto item_row=
    dynamic_cast<Item_field_row *>(const_cast<Item_field *> (item));
  DBUG_ASSERT(item_row);

  uint field_idx;
  if (get_item_index(thd, item_row, name, field_idx))
    return nullptr;

  return item_row->element_index(field_idx)->field_for_view_update();
}

Named_type_handler<Type_handler_row> type_handler_row_internal("row");
const Type_handler_composite &type_handler_row=
  type_handler_row_internal;
