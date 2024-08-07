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
#include "sql_show.h"


extern struct st_opt_hint_info opt_hint_info[];


Parse_context::Parse_context(THD *thd, st_select_lex *select)
: thd(thd),
  mem_root(thd->mem_root),
  select(select)
{}


Optimizer_hint_tokenizer::TokenID
Optimizer_hint_tokenizer::find_keyword(const LEX_CSTRING &str)
{
  switch (str.length)
  {
  case 3:
    if ("BKA"_Lex_ident_column.streq(str)) return TokenID::keyword_BKA;
    if ("BNL"_Lex_ident_column.streq(str)) return TokenID::keyword_BNL;
    if ("MRR"_Lex_ident_column.streq(str)) return TokenID::keyword_MRR;
    break;

  case 6:
    if ("NO_BKA"_Lex_ident_column.streq(str)) return TokenID::keyword_NO_BKA;
    if ("NO_BNL"_Lex_ident_column.streq(str)) return TokenID::keyword_NO_BNL;
    if ("NO_ICP"_Lex_ident_column.streq(str)) return TokenID::keyword_NO_ICP;
    if ("NO_MRR"_Lex_ident_column.streq(str)) return TokenID::keyword_NO_MRR;
    break;

  case 7:
    if ("QB_NAME"_Lex_ident_column.streq(str))
      return TokenID::keyword_QB_NAME;
    break;

  case 18:
    if ("MAX_EXECUTION_TIME"_Lex_ident_column.streq(str))
      return TokenID::keyword_MAX_EXECUTION_TIME;
    break;

  case 21:
    if ("NO_RANGE_OPTIMIZATION"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_RANGE_OPTIMIZATION;
    break;
  }

  if (str.length > 0 && (str.str[0] >= '0' && str.str[0] <= '9'))
  {
    /*
      If all characters are digits, qualify the token as a number,
      otherwise as an identifier
    */
    for(size_t i = 1; i < str.length; i++)
    {
      if (str.str[i] < '0' || str.str[i] > '9')
        return TokenID::tIDENT;
    }
    return TokenID::tUNSIGNED_NUMBER;
  }
  return TokenID::tIDENT;
}


Optimizer_hint_tokenizer::Token
Optimizer_hint_tokenizer::get_token(CHARSET_INFO *cs)
{
  get_spaces();
  if (eof())
    return Token(Lex_cstring(m_ptr, m_ptr), TokenID::tEOF);
  const char head= m_ptr[0];
  if (head == '`' || head=='"')
  {
    const Token_with_metadata delimited_ident= get_quoted_string();
    /*
      Consider only non-empty quoted strings as identifiers.
      Table and index names cannot be empty in MariaDB.
      Let's also disallow empty query block names.
      Note, table aliases can actually be empty:
        SELECT ``.a FROM t1 ``;
      But let's disallow them in hints for simplicity, to handle
      all identifiers in the same way in the hint parser.
    */
    if (delimited_ident.length > 2)
      return Token(delimited_ident, TokenID::tIDENT);
    /*
      If the string is empty, "unget" it to have a good
      syntax error position in the message text.
      The point is to include the empty string in the error message:
        EXPLAIN EXTENDED SELECT ... QB_NAME(``) ...;  -->
        Optimizer hint syntax error near '``) ...' at line 1
    */
    m_ptr-= delimited_ident.length;
    return Token(Lex_cstring(m_ptr, m_ptr), TokenID::tNULL);
  }
  const Token_with_metadata ident= get_ident();
  if (ident.length)
    return Token(ident, ident.m_extended_chars ?
                 TokenID::tIDENT : find_keyword(ident));
  if (!get_char(','))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tCOMMA);
  if (!get_char('@'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tAT);
  if (!get_char('('))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tLPAREN);
  if (!get_char(')'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tRPAREN);
  return Token(Lex_cstring(m_ptr, m_ptr), TokenID::tNULL);
}


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

void Optimizer_hint_parser::push_warning_syntax_error(THD *thd,
                                                      uint start_lineno)
{
  DBUG_ASSERT(m_start <= m_ptr);
  DBUG_ASSERT(m_ptr <= m_end);
  const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
  ErrConvString txt(m_look_ahead_token.str, strlen(m_look_ahead_token.str),
                    thd->variables.character_set_client);
  /*
    start_lineno is the line number on which the whole hint started.
    Add the line number of the current tokenizer position inside the hint
    (in case hints are written in multiple lines).
  */
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR),
                      msg, txt.ptr(), start_lineno + lineno());
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
