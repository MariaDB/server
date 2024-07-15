/*
   Copyright (c) 2024, MariaDB

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


#include "opt_hints_parser.h"
#include "sql_error.h"
#include "mysqld_error.h"
#include "sql_class.h"


extern struct st_opt_hint_info opt_hint_info[];


Parse_context::Parse_context(THD *thd, st_select_lex *select)
: thd(thd),
  mem_root(thd->mem_root),
  select(select)
{}


// This method is for debug purposes
bool Optimizer_hint_parser::parse_token_list(THD *thd)
{
  for ( ; ; m_look_ahead_token= get_token(m_cs))
  {
    char tmp[200];
    my_snprintf(tmp, sizeof(tmp), "TOKEN: %d %.*s",
               (int) m_look_ahead_token.id(),
               (int) m_look_ahead_token.length,
               m_look_ahead_token.str);
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_UNKNOWN_ERROR, tmp);
    if (m_look_ahead_token.id() == TokenID::tNULL ||
        m_look_ahead_token.id() == TokenID::tEOF)
      break;
  }
  return true; // Success
}


void Optimizer_hint_parser::push_warning_syntax_error(THD *thd)
{
  const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
  ErrConvString txt(m_look_ahead_token.str, strlen(m_look_ahead_token.str),
                    thd->variables.character_set_client);
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR),
                      msg, txt.ptr(), 1);
}


bool
Optimizer_hint_parser::
  Table_name_list_container::add(Optimizer_hint_parser *p,
                                 Table_name &&elem)
{
  Table_name *pe= (Table_name*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_param_table_list_container::add(Optimizer_hint_parser *p,
                                       Hint_param_table &&elem)
{
  Hint_param_table *pe= (Hint_param_table*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_param_index_list_container::add(Optimizer_hint_parser *p,
                                       Hint_param_index &&elem)
{
  Hint_param_index *pe= (Hint_param_index*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool
Optimizer_hint_parser::
  Hint_list_container::add(Optimizer_hint_parser *p,
                           Hint &&elem)
{
  Hint *pe= (Hint*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}
