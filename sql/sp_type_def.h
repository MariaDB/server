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
                    public Type_generic_attributes
{
protected:
  /// Name of the type.
  Lex_ident_column m_name;

public:
  sp_type_def(const Lex_ident_column &name_arg)
   :Sql_alloc(),
    m_name(name_arg)
  { }

  virtual ~sp_type_def() { }

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
  A reference data type, e.g. REF CURSOR.
*/
class sp_type_def_ref : public sp_type_def,
                        public Type_handler_hybrid_field_type
{
  Spvar_definition m_def;
public:
  /*
    m_def - The definition (e.g. the structure) of the referenced data type.
            a) Can be nullptr when the structure of the referenced type
               is not set:
                 TYPE cur0_t IS REF CURSOR;
            b) Can be non-nullptr when the structure of the referenced type
               is set:
                 TYPE rec0_t IS RECORD (a INT, b VARCHAR(10));
                 TYPE cur0_t IS REF CURSOR RETURNS rec0_t;
  */
  sp_type_def_ref(const Lex_ident_column &name_arg,
                  const Type_handler *th,
                  const Spvar_definition &def)
   :sp_type_def(name_arg),
    Type_handler_hybrid_field_type(th),
    m_def(def)
  { }
  const Type_handler *type_handler() const override
  {
    return Type_handler_hybrid_field_type::type_handler();
  }
  const Spvar_definition & def() const
  {
    return m_def;
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
   :sp_type_def(name_arg),
    field(prmfield)
  { }

  const Type_handler *type_handler() const override
  {
    return &type_handler_row;
  }
};


/*
  This class represents 'DECLARE TYPE .. TABLE OF' statement.
*/
class sp_type_def_composite2 : public sp_type_def,
                               public Type_handler_hybrid_field_type
{
  Spvar_definition m_def[2];

public:
  sp_type_def_composite2(const Lex_ident_column &name_arg,
                         const Type_handler *th,
                         const Spvar_definition *key_def_arg,
                         const Spvar_definition *value_def_arg)
   :sp_type_def(name_arg),
    Type_handler_hybrid_field_type(th),
    m_def{*key_def_arg, *value_def_arg}
  { }

  const Type_handler *type_handler() const override
  {
    return Type_handler_hybrid_field_type::type_handler();
  }

  const Spvar_definition & def(uint idx) const
  {
    DBUG_ASSERT(idx < 2);
    return m_def[idx];
  }
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
  bool type_defs_add(sp_type_def *def)
  {
    return m_type_defs.append(def);
  }
};


#endif // SQL_TYPE_DEF_H
