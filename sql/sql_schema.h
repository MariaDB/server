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

class Lex_ident_sys;
class Create_func;

class Schema
{
  LEX_CSTRING m_name;
public:
  Schema(const LEX_CSTRING &name)
   :m_name(name)
  { }
  virtual ~Schema() = default;
  const LEX_CSTRING &name() const { return m_name; }
  virtual const Type_handler *map_data_type(THD *thd, const Type_handler *src)
                                            const
  {
    return src;
  }

  /**
    Find a native function builder, return an error if not found,
    build an Item otherwise.
  */
  Item *make_item_func_call_native(THD *thd,
                                   const Lex_ident_sys &name,
                                   List<Item> *args) const;

  /**
    Find the native function builder associated with a given function name.
    @param thd The current thread
    @param name The native function name
    @return The native function builder associated with the name, or NULL
  */
  virtual Create_func *find_native_function_builder(THD *thd,
                                                    const LEX_CSTRING &name)
                                                    const;

  // Builders for native SQL function with a special syntax in sql_yacc.yy
  virtual Item *make_item_func_replace(THD *thd,
                                       Item *subj,
                                       Item *find,
                                       Item *replace) const;
  virtual Item *make_item_func_substr(THD *thd,
                                      const Lex_substring_spec_st &spec) const;

  virtual Item *make_item_func_trim(THD *thd, const Lex_trim_st &spec) const;

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
extern const Schema &oracle_schema_ref;

#endif // SQL_SCHEMA_H_INCLUDED
