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
#ifndef SQL_TYPE_DEF_H
#define SQL_TYPE_DEF_H

#include "sql_type.h"
/*
 This class represents a type definition in a stored program.
*/

class sp_type_def : public Sql_alloc,
                    public Type_handler_hybrid_field_type
{
protected:
  /// Name of the type.
  Lex_ident_column m_name;

public:
  sp_type_def(const Lex_ident_column &name_arg, const Type_handler *th)
   :Sql_alloc(),
    Type_handler_hybrid_field_type(th),
    m_name(name_arg)
  { }

  bool eq_name(const LEX_CSTRING &name) const
  {
    return m_name.streq(name);
  }

  const Lex_ident_column &get_name() const
  {
    return m_name;
  }

  Item *make_constructor_item(THD *thd, List<Item> *args) const
  {
    return type_handler()->make_typedef_constructor_item(thd, *this, args);
  }
};


/*
  This class represents 'DECLARE RECORD' statement.
*/
class sp_type_def_record : public sp_type_def
{
public:
  Row_definition_list *field;

public:
  sp_type_def_record(const Lex_ident_column &name_arg,
                     Row_definition_list *prmfield)
   :sp_type_def(name_arg, &type_handler_row),
    field(prmfield)
  { }
};


class sp_type_def_list
{
protected:
    /// Stack of type definitions.
  Dynamic_array<sp_type_def *> m_type_defs;
public:
  sp_type_def_list();
  sp_type_def *find_type_def(const LEX_CSTRING &name) const
  {
    for (uint  i= 0; i < m_type_defs.elements(); i++)
    {
      sp_type_def *p= m_type_defs.at(i);
      if (p->eq_name(name))
        return p;
    }
    return nullptr;
  }

  bool type_defs_add_record(THD *thd, const Lex_ident_column &name,
                            Row_definition_list *field);
};


#endif // SQL_TYPE_DEF_H
