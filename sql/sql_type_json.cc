/*
   Copyright (c) 2019, MariaDB

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "sql_type_json.h"
#include "sql_class.h"


Type_handler_json_longtext  type_handler_json_longtext;


/**
   Create JSON_VALID(field_name) expression
*/

Virtual_column_info *
Type_handler_json_longtext::make_json_valid_expr(THD *thd,
                                                 const LEX_CSTRING *field_name)
                                                 const
{
  Lex_ident_sys_st str;
  Item *field, *expr;
  str.set_valid_utf8(field_name);
  if (unlikely(!(field= thd->lex->create_item_ident_field(thd, NullS, NullS,
                                                          &str))))
    return 0;
  if (unlikely(!(expr= new (thd->mem_root) Item_func_json_valid(thd, field))))
    return 0;
  return add_virtual_expression(thd, expr);
}


bool Type_handler_json_longtext::
       Column_definition_validate_check_constraint(THD *thd,
                                                   Column_definition * c) const
{
  if (!c->check_constraint &&
      !(c->check_constraint= make_json_valid_expr(thd, &c->field_name)))
    return true;
  return Type_handler::Column_definition_validate_check_constraint(thd, c);
}
