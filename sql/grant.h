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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_GRANT_INCLUDED
#define SQL_GRANT_INCLUDED

#include "lex_string.h"
#include "privilege.h"
#include "sql_lex.h"
#include "sql_list.h"

class LEX_COLUMN;
class Lex_ident_sys;
class Table_ident;

/*
  Represents the object name in this standard SQL grammar:
    GRANT <object privileges> ON <object name>
*/
class Grant_object_name
{
public:
  enum Type
  {
    STAR,            // ON *
    IDENT_STAR,      // ON db.*
    STAR_STAR,       // ON *.*
    TABLE_IDENT      // ON db.name
  };
  Lex_cstring m_db;
  Table_ident *m_table_ident;
  Type m_type;
public:
  Grant_object_name(Table_ident *table_ident)
   :m_table_ident(table_ident),
    m_type(TABLE_IDENT)
  { }
  Grant_object_name(const LEX_CSTRING &db, Type type)
   :m_db(db),
    m_table_ident(NULL),
    m_type(type)
  { }
  privilege_t all_privileges_by_type() const;
};



/*
  Represents standard SQL statements described by:
  - <grant privilege statement>
  - <revoke privilege statement>
*/
class Grant_privilege
{
protected:
  List<LEX_COLUMN> m_columns;
  Lex_cstring m_db;
  privilege_t m_object_privilege;
  privilege_t m_column_privilege_total;
  bool m_all_privileges;
public:
  Grant_privilege(privilege_t privileges)
   :m_object_privilege(privileges),
    m_column_privilege_total(NO_ACL),
    m_all_privileges(false)
  { }
  void add_object_privilege(privilege_t privilege)
  {
    m_object_privilege|= privilege;
  }
  bool add_column_privilege(THD *thd, const Lex_ident_sys &col,
                            privilege_t privilege);
  bool add_column_list_privilege(THD *thd, List<Lex_ident_sys> &list,
                                 privilege_t privilege);
  bool set_object_name(THD *thd,
                       const Grant_object_name &ident,
                       SELECT_LEX *sel,
                       privilege_t with_grant_option);
  void set_all_privileges()
  {
    m_object_privilege= GLOBAL_ACLS;
    m_all_privileges= true;
  }
  /* Getters */
  List<LEX_COLUMN> & columns() { return m_columns; }
  const Lex_cstring& db() const { return m_db; }
  privilege_t object_privilege() const { return m_object_privilege; }
  privilege_t column_privilege_total() const { return m_column_privilege_total; }
  bool has_all_privileges() const { return m_all_privileges; }
};


#endif // SQL_GRANT_INCLUDED
