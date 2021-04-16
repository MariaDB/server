#ifndef SQL_SCHEMA_H_INCLUDED
#define SQL_SCHEMA_H_INCLUDED
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

#include "mysqld.h"
#include "lex_string.h"

class Schema
{
  LEX_CSTRING m_name;
public:
  Schema(const LEX_CSTRING &name)
   :m_name(name)
  { }
  virtual ~Schema() { }
  const LEX_CSTRING &name() const { return m_name; }
  virtual const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                            const
  {
    return src;
  }
  /*
    For now we have *hard-coded* compatibility schemas:
      schema_mariadb, schema_oracle, schema_maxdb.
    But eventually we'll turn then into real databases on disk.
    So the code below compares names according to the filesystem
    case sensitivity, like it is done for regular databases.

    Note, this is different to information_schema, whose name
    is always case insensitive. This is intentional!
    The assymetry will be gone when we'll implement SQL standard
    regular and delimited identifiers.
  */
  bool eq_name(const LEX_CSTRING &name) const
  {
    return !table_alias_charset->strnncoll(m_name.str, m_name.length,
                                           name.str, name.length);
  }
  static Schema *find_by_name(const LEX_CSTRING &name);
  static Schema *find_implied(THD *thd);
};


extern Schema mariadb_schema;

#endif // SQL_SCHEMA_H_INCLUDED
