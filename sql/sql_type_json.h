#ifndef SQL_TYPE_JSON_INCLUDED
#define SQL_TYPE_JSON_INCLUDED
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

#include "mariadb.h"
#include "sql_type.h"

class Type_handler_json_longtext: public Type_handler_long_blob
{
  Virtual_column_info *make_json_valid_expr(THD *thd,
                                            const LEX_CSTRING *field_name)
                                            const;
public:
  virtual ~Type_handler_json_longtext() {}
  bool Column_definition_validate_check_constraint(THD *thd,
                                                   Column_definition *c) const;
};

extern MYSQL_PLUGIN_IMPORT
  Type_handler_json_longtext type_handler_json_longtext;

#endif // SQL_TYPE_JSON_INCLUDED
