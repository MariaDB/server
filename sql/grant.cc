/*
   Copyright (c) 2009, 2020, MariaDB Corporation

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
#include "sql_acl.h"


bool Grant_privilege::add_column_privilege(THD *thd,
                                           const Lex_ident_sys &name,
                                           privilege_t which_grant)
{
  String *new_str= new (thd->mem_root) String((const char*) name.str,
                                              name.length,
                                              system_charset_info);
  if (unlikely(new_str == NULL))
    return true;
  List_iterator <LEX_COLUMN> iter(m_columns);
  class LEX_COLUMN *point;
  while ((point=iter++))
  {
    if (!my_strcasecmp(system_charset_info,
                       point->column.c_ptr(), new_str->c_ptr()))
      break;
  }
  m_column_privilege_total|= which_grant;
  if (point)
  {
    point->rights |= which_grant;
    return false;
  }

  LEX_COLUMN *col= new (thd->mem_root) LEX_COLUMN(*new_str, which_grant);
  if (unlikely(col == NULL))
    return true;
  return m_columns.push_back(col, thd->mem_root);
}


bool Grant_privilege::add_column_list_privilege(THD *thd,
                                                List<Lex_ident_sys> &list,
                                                privilege_t privilege)
{
  Lex_ident_sys *col;
  List_iterator<Lex_ident_sys> it(list);
  while ((col= it++))
  {
    if (add_column_privilege(thd, *col, privilege))
      return true;
  }
  return false;
}


privilege_t Grant_object_name::all_privileges_by_type() const
{
  switch (m_type) {
  case STAR:        return DB_ACLS & ~GRANT_ACL;
  case IDENT_STAR:  return DB_ACLS & ~GRANT_ACL;
  case STAR_STAR:   return GLOBAL_ACLS & ~GRANT_ACL;
  case TABLE_IDENT: return TABLE_ACLS & ~GRANT_ACL;
  }
  return NO_ACL;
}


bool Grant_privilege::set_object_name(THD *thd,
                                      const Grant_object_name &ident,
                                      SELECT_LEX *sel,
                                      privilege_t with_grant_option)
{
  DBUG_ASSERT(!m_all_privileges || !m_columns.elements);

  m_db= ident.m_db;
  if (m_all_privileges)
    m_object_privilege= ident.all_privileges_by_type();
  m_object_privilege|= with_grant_option;
  switch (ident.m_type)
  {
  case Lex_grant_object_name::STAR:
  case Lex_grant_object_name::IDENT_STAR:
  case Lex_grant_object_name::STAR_STAR:
    if (!m_all_privileges && m_columns.elements)
    {
      // e.g. GRANT SELECT (a) ON db.*
      my_error(ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0));
      return true;
    }
    return false;
  case Lex_grant_object_name::TABLE_IDENT:
    m_db= ident.m_table_ident->db;
    return !sel->add_table_to_list(thd, ident.m_table_ident,
                                   NULL, TL_OPTION_UPDATING);
  }
  return false; // Make gcc happy
}
