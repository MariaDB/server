/*
 Copyright (c) 2020, MariaDB Corporation.

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

#include "mariadb.h"
#include "sql_type.h"
#include "sql_schema.h"
#include "sql_class.h"

class Schema_oracle: public Schema
{
public:
  Schema_oracle(const LEX_CSTRING &name)
   :Schema(name)
  { }
  const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                    const
  {
    if (src == &type_handler_newdate)
      return thd->type_handler_for_datetime();
    return src;
  }
};


class Schema_maxdb: public Schema
{
public:
  Schema_maxdb(const LEX_CSTRING &name)
   :Schema(name)
  { }
  const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                    const
  {
    if (src == &type_handler_timestamp ||
        src == &type_handler_timestamp2)
      return thd->type_handler_for_datetime();
    return src;
  }
};


Schema        mariadb_schema(Lex_cstring(STRING_WITH_LEN("mariadb_schema")));
Schema_oracle oracle_schema(Lex_cstring(STRING_WITH_LEN("oracle_schema")));
Schema_maxdb  maxdb_schema(Lex_cstring(STRING_WITH_LEN("maxdb_schema")));


Schema *Schema::find_by_name(const LEX_CSTRING &name)
{
  DBUG_ASSERT(name.str);
  if (mariadb_schema.eq_name(name))
    return &mariadb_schema;
  if (oracle_schema.eq_name(name))
    return &oracle_schema;
  if (maxdb_schema.eq_name(name))
    return &maxdb_schema;
  return NULL;
}


Schema *Schema::find_implied(THD *thd)
{
  if (thd->variables.sql_mode & MODE_ORACLE)
    return &oracle_schema;
  if (thd->variables.sql_mode & MODE_MAXDB)
    return &maxdb_schema;
  return &mariadb_schema;
}
