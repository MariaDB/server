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
#include "opt_hints.h"

using Parser= Optimizer_hint_parser;

extern struct st_opt_hint_info opt_hint_info[];

// Forward declaration of functions
void print_warn(THD *thd, uint err_code, opt_hints_enum hint_type,
                bool hint_state,
                const Lex_ident_sys *qb_name_arg,
                const Lex_ident_sys *table_name_arg,
                const Lex_ident_sys *key_name_arg,
                const Printable_parser_rule *hint);

Opt_hints_qb *get_qb_hints(Parse_context *pc);

Opt_hints_qb *find_qb_hints(Parse_context *pc,
                            const Lex_ident_sys &qb_name,
                            opt_hints_enum hint_type,
                            bool hint_state);

Opt_hints_global *get_global_hints(Parse_context *pc);

Opt_hints_table *get_table_hints(Parse_context *pc,
                                 const Lex_ident_sys &table_name,
                                 Opt_hints_qb *qb);

void append_table_name(THD *thd, String *str, const LEX_CSTRING &table_name,
                       const LEX_CSTRING &qb_name);

static const Lex_ident_sys null_ident_sys;

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

  case 5:
    if ("MERGE"_Lex_ident_column.streq(str)) return TokenID::keyword_MERGE;
    if ("INDEX"_Lex_ident_column.streq(str)) return TokenID::keyword_INDEX;
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

  case 8:
    if ("SEMIJOIN"_Lex_ident_column.streq(str))
      return TokenID::keyword_SEMIJOIN;
    if ("SUBQUERY"_Lex_ident_column.streq(str))
      return TokenID::keyword_SUBQUERY;
    if ("NO_MERGE"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_MERGE;
    if ("NO_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_INDEX;
    break;

  case 9:
    if ("LOOSESCAN"_Lex_ident_column.streq(str))
      return TokenID::keyword_LOOSESCAN;
    break;

  case 10:
    if ("FIRSTMATCH"_Lex_ident_column.streq(str))
      return TokenID::keyword_FIRSTMATCH;
    if ("INTOEXISTS"_Lex_ident_column.streq(str))
      return TokenID::keyword_INTOEXISTS;
    if ("JOIN_ORDER"_Lex_ident_column.streq(str))
      return TokenID::keyword_JOIN_ORDER;
    if ("JOIN_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_JOIN_INDEX;
    break;

  case 11:
    if ("NO_SEMIJOIN"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_SEMIJOIN;
    if ("DUPSWEEDOUT"_Lex_ident_column.streq(str))
      return TokenID::keyword_DUPSWEEDOUT;
    if ("JOIN_PREFIX"_Lex_ident_column.streq(str))
      return TokenID::keyword_JOIN_PREFIX;
    if ("JOIN_SUFFIX"_Lex_ident_column.streq(str))
      return TokenID::keyword_JOIN_SUFFIX;
    if ("ORDER_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_ORDER_INDEX;
    if ("GROUP_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_GROUP_INDEX;
    break;

  case 13:
    if ("NO_JOIN_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_JOIN_INDEX;
    break;

  case 14:
    if ("NO_ORDER_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_ORDER_INDEX;
    if ("NO_GROUP_INDEX"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_GROUP_INDEX;
    break;

  case 15:
    if ("MATERIALIZATION"_Lex_ident_column.streq(str))
      return TokenID::keyword_MATERIALIZATION;
    break;

  case 16:
    if ("JOIN_FIXED_ORDER"_Lex_ident_column.streq(str))
      return TokenID::keyword_JOIN_FIXED_ORDER;
    break;

  case 18:
    if ("MAX_EXECUTION_TIME"_Lex_ident_column.streq(str))
      return TokenID::keyword_MAX_EXECUTION_TIME;
    if ("SPLIT_MATERIALIZED"_Lex_ident_column.streq(str))
      return TokenID::keyword_SPLIT_MATERIALIZED;
    break;

  case 21:
    if ("NO_RANGE_OPTIMIZATION"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_RANGE_OPTIMIZATION;
    if ("NO_SPLIT_MATERIALIZED"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_SPLIT_MATERIALIZED;
    break;

  case 26:
    if ("DERIVED_CONDITION_PUSHDOWN"_Lex_ident_column.streq(str))
      return TokenID::keyword_DERIVED_CONDITION_PUSHDOWN;
    break;

  case 29:
    if ("NO_DERIVED_CONDITION_PUSHDOWN"_Lex_ident_column.streq(str))
      return TokenID::keyword_NO_DERIVED_CONDITION_PUSHDOWN;
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
bool Parser::parse_token_list(THD *thd)
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

void Parser::push_warning_syntax_error(THD *thd, uint start_lineno)
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
Parser::Table_name_list_container::add(Optimizer_hint_parser *p,
                                       Table_name &&elem)
{
  Table_name *pe= (Table_name*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool Parser::Hint_param_table_list_container::add(Optimizer_hint_parser *p,
                                                  Hint_param_table &&elem)
{
  Hint_param_table *pe= (Hint_param_table*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool Parser::Hint_param_index_list_container::add(Optimizer_hint_parser *p,
                                                  Hint_param_index &&elem)
{
  Hint_param_index *pe= (Hint_param_index*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool Parser::Hint_list_container::add(Optimizer_hint_parser *p, Hint &&elem)
{
  Hint *pe= new (p->m_thd->mem_root) Hint;
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


bool Parser::Semijoin_strategy_list_container::add(Optimizer_hint_parser *p,
                                                   Semijoin_strategy &&elem)
{
  Semijoin_strategy *pe= (Semijoin_strategy*) p->m_thd->alloc(sizeof(*pe));
  if (!pe)
    return true;
  *pe= std::move(elem);
  return push_back(pe, p->m_thd->mem_root);
}


/*
  Resolve a parsed table level hint, i.e. set up proper Opt_hint_* structures
  which will be used later during query preparation and optimization.

Return value:
- false: no critical errors, warnings on duplicated hints,
       unresolved query block names, etc. are allowed
- true: critical errors detected, break further hints processing
*/
bool Parser::Table_level_hint::resolve(Parse_context *pc) const
{
  const Table_level_hint_type &table_level_hint_type= *this;
  opt_hints_enum hint_type;
  bool hint_state; // ON or OFF

  switch (table_level_hint_type.id())
  {
  case TokenID::keyword_BNL:
    hint_type= BNL_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_BNL:
    hint_type= BNL_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_BKA:
    hint_type= BKA_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_BKA:
    hint_type= BKA_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_DERIVED_CONDITION_PUSHDOWN:
    hint_type= DERIVED_CONDITION_PUSHDOWN_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_DERIVED_CONDITION_PUSHDOWN:
    hint_type= DERIVED_CONDITION_PUSHDOWN_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_MERGE:
    hint_type= MERGE_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_MERGE:
    hint_type= MERGE_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_SPLIT_MATERIALIZED:
    hint_type= SPLIT_MATERIALIZED_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_SPLIT_MATERIALIZED:
    hint_type= SPLIT_MATERIALIZED_HINT_ENUM;
    hint_state= false;
    break;
  default:
    DBUG_ASSERT(0);
    return true;
  }

  if (const At_query_block_name_opt_table_name_list &
          at_query_block_name_opt_table_name_list= *this)
  {
    // this is @ query_block_name opt_table_name_list
    const Lex_ident_sys qb_name_sys= Query_block_name::to_ident_sys(pc->thd);
    Opt_hints_qb *qb= find_qb_hints(pc, qb_name_sys, hint_type, hint_state);
    if (qb == NULL)
      return false;
    if (at_query_block_name_opt_table_name_list.base_list::is_empty())
    {
      // e.g. BKA(@qb1)
      if (qb->set_switch(hint_state, hint_type, false))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &qb_name_sys, nullptr, nullptr, nullptr);
      }
      return false;
    }
    else
    {
      // e.g. BKA(@qb1 t1, t2, t3)
      const Opt_table_name_list &opt_table_name_list= *this;
      for (const Table_name &table : opt_table_name_list)
      {
        const Lex_ident_sys table_name_sys= table.to_ident_sys(pc->thd);
        Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
        if (!tab)
          return false;
        if (tab->set_switch(hint_state, hint_type, true))
        {
          print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                     &qb_name_sys, &table_name_sys, nullptr,
                     nullptr);
        }
      }
    }
  }
  else
  {
    // this is opt_hint_param_table_list
    const Opt_hint_param_table_list &opt_hint_param_table_list= *this;
    Opt_hints_qb *qb= find_qb_hints(pc, Lex_ident_sys(), hint_type, hint_state);
    if (qb == NULL)
      return false;
    if (opt_hint_param_table_list.is_empty())
    {
      // e.g. BKA()
      if (qb->set_switch(hint_state, hint_type, false))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &null_ident_sys, nullptr, nullptr, nullptr);
      }
      return false;
    }
    for (const Hint_param_table &table : opt_hint_param_table_list)
    {
      // e.g. BKA(t1@qb1, t2@qb2, t3)
      const Lex_ident_sys qb_name_sys= table.Query_block_name::
                                       to_ident_sys(pc->thd);
      Opt_hints_qb *qb= find_qb_hints(pc, qb_name_sys, hint_type, hint_state);
      if (qb == NULL)
        return false;
      const Lex_ident_sys table_name_sys= table.Table_name::
                                          to_ident_sys(pc->thd);
      Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
      if (!tab)
        return false;
      if (tab->set_switch(hint_state, hint_type, true))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &qb_name_sys, &table_name_sys, nullptr, nullptr);
      }
    }
  }
  return false;
}

/*
  Resolve a parsed index level hint, i.e. set up proper Opt_hint_* structures
  which will be used later during query preparation and optimization.

  Return value:
  - false: no critical errors, warnings on duplicated hints,
         unresolved query block names, etc. are allowed
  - true: critical errors detected, break further hints processing

  Taxonomy of index hints
  - 2 levels of hints:
    - table level hints: only table name specified but no index names
    - index level hints: both table name and index names specified
  - 2 kinds of hints:
    - global: [NO_]INDEX
    - non-global: [NO_]JOIN_INDEX, [NO_]GROUP_INDEX, [NO_]ORDER_INDEX
  - 4 types of hints:
    - [NO_]JOIN_INDEX
    - [NO_]GROUP_INDEX
    - [NO_]ORDER_INDEX
    - [NO_]INDEX

  Conflict checking
  - A conflict happens if and only if
    - for a table level hint
      - a hint of the same type or opposite kind has already been specified
        for the same table
    - for a index level hint
      - the same type of hint has already been specified for the same
        table or for the same index, OR
      - the opposite kind of hint has already been specified for the
        same index
  - For a multi index hint like JOIN_INDEX(t1 i1, i2, i3), it conflicts
    with a previous hint if any of the JOIN_INDEX(t1 i1), JOIN_INDEX(t1
    i2), JOIN_INDEX(t1 i3) conflicts with a previous hint

   When a hint type is specified for an index, it is also marked as
   specified with the same switch state for the table
*/
bool Parser::Index_level_hint::resolve(Parse_context *pc) const
{
  const Index_level_hint_type &index_level_hint_type= *this;
  opt_hints_enum hint_type;
  bool hint_state; // ON or OFF

  switch (index_level_hint_type.id())
  {
  case TokenID::keyword_NO_ICP:
    hint_type= ICP_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_MRR:
    hint_type= MRR_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_MRR:
    hint_type= MRR_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_NO_RANGE_OPTIMIZATION:
    hint_type= NO_RANGE_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_INDEX:
    hint_type= INDEX_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_INDEX:
    hint_type= INDEX_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_JOIN_INDEX:
    hint_type= JOIN_INDEX_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_JOIN_INDEX:
    hint_type= JOIN_INDEX_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_ORDER_INDEX:
    hint_type= ORDER_INDEX_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_ORDER_INDEX:
    hint_type= ORDER_INDEX_HINT_ENUM;
    hint_state= false;
    break;
  case TokenID::keyword_GROUP_INDEX:
    hint_type= GROUP_INDEX_HINT_ENUM;
    hint_state= true;
    break;
  case TokenID::keyword_NO_GROUP_INDEX:
    hint_type= GROUP_INDEX_HINT_ENUM;
    hint_state= false;
    break;
  default:
    DBUG_ASSERT(0);
    return true;
  }

  const Hint_param_table_ext &table_ext= *this;
  const Lex_ident_sys qb_name_sys= table_ext.Query_block_name::
                                   to_ident_sys(pc->thd);
  const Lex_ident_sys table_name_sys= table_ext.Table_name::
                                      to_ident_sys(pc->thd);
  Opt_hints_qb *qb= find_qb_hints(pc, qb_name_sys, hint_type, hint_state);
  if (qb == NULL)
    return false;

  Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
  if (!tab)
    return false;

  const Lex_ident_sys key_conflict(
      STRING_WITH_LEN("another hint was already specified for this index"));

  /*
    If no index names are given, this is a table level hint, for example:
    GROUP_INDEX(t1), NO_MRR(t2).
    Otherwise this is a group of index-level hints:
    NO_INDEX(t1 idx1, idx2) NO_ICP(t2 idx_a, idx_b, idx_c)
  */
  if (base_list::is_empty())
  {
    uint warn_code= 0;
    if (is_compound_hint(hint_type) &&
        is_index_hint_conflicting(tab, nullptr, hint_type))
    {
      warn_code= ER_WARN_CONFLICTING_COMPOUND_INDEX_HINT_FOR_TABLE;
    }
    else if (tab->set_switch(hint_state, hint_type, false))
    {
      warn_code= ER_WARN_CONFLICTING_INDEX_HINT_FOR_TABLE;
    }

    if (warn_code != 0)
    {
      print_warn(pc->thd, warn_code, hint_type, hint_state, &qb_name_sys,
                  &table_name_sys, nullptr, this);
    }
    else if (is_compound_hint(hint_type))
    {
      tab->get_key_hint_bitmap(hint_type)->parsed_hint= this;
    }
    return false;
  }

  // Key names for a compound hint are first collected into the array:
  Mem_root_array<std::pair<Opt_hints_key *,
                           bool /* whether a new one was created */>>
      key_hints(pc->thd->mem_root);
  bool is_conflicting= false;
  for (const Hint_param_index &index_name : *this)
  {
    const Lex_ident_sys index_name_sys= index_name.to_ident_sys(pc->thd);
    bool new_opt_key_hint_created= false;
    Opt_hints_key *key= (Opt_hints_key *)tab->find_by_name(index_name_sys);
    if (!key)
    {
      key= new (pc->thd->mem_root)
        Opt_hints_key(index_name_sys, tab, pc->thd->mem_root);
      new_opt_key_hint_created= true;
    }

    if (!is_compound_hint(hint_type))
    {
      if (key->set_switch(hint_state, hint_type, true))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_INDEX_HINT_FOR_KEY,
                   hint_type, hint_state, &qb_name_sys, &table_name_sys,
                   &index_name_sys, nullptr);
        continue;
      }
      if (new_opt_key_hint_created)
        tab->register_child(key);
    }
    else
    {
      bool is_specified= tab->is_specified(hint_type) ||
                         key->is_specified(hint_type);
      if (is_specified || is_index_hint_conflicting(tab, key, hint_type))
      {
        is_conflicting= true;
        uint warn_code;
        if (is_specified)
        {
          warn_code= tab->is_specified(hint_type) ?
              ER_WARN_CONFLICTING_INDEX_HINT_FOR_TABLE :
              ER_WARN_CONFLICTING_INDEX_HINT_FOR_KEY;
        }
        else
        {
          warn_code= ER_WARN_CONFLICTING_COMPOUND_INDEX_HINT_FOR_KEY;
        }
        print_warn(pc->thd, warn_code, hint_type, hint_state,
                   &qb_name_sys, &table_name_sys, nullptr, this);
        break;
      }
      key_hints.push_back({ key, new_opt_key_hint_created });
    }
  }

  if (is_compound_hint(hint_type) && !is_conflicting)
  {
    /*
      Process key names collected for a compound hint. They have already been
      checked for conflicts/duplication above, so there is no need to examine
      the `set_switch()` return value
    */
    for (size_t i= 0; i < key_hints.size(); i++)
    {
      std::pair<Opt_hints_key *, bool> key= key_hints.at(i);
      key.first->set_switch(hint_state, hint_type, true);
      if (key.second)
        tab->register_child(key.first);
    }

    tab->get_key_hint_bitmap(hint_type)->parsed_hint= this;
    tab->set_switch(hint_state, hint_type, false);
  }
  return false;
}


void Parser::Index_level_hint::append_args(THD *thd, String *str) const
{
  if (base_list::is_empty())  // Empty list of index names, no additional info
    return;

  bool first_index_name= true;
  for (const Hint_param_index &index_name : *this)
  {
    if (!first_index_name)
      str->append(STRING_WITH_LEN(","));
    append_identifier(thd, str, &index_name);
    first_index_name= false;
  }
}


/*
  Resolve a parsed query block name hint, i.e. set up proper Opt_hint_*
  structures which will be used later during query preparation and optimization.

Return value:
- false: no critical errors, warnings on duplicated hints,
       unresolved query block names, etc. are allowed
- true: critical errors detected, break further hints processing
*/
bool Parser::Qb_name_hint::resolve(Parse_context *pc) const
{
  Opt_hints_qb *qb= pc->select->opt_hints_qb;

  DBUG_ASSERT(qb);

  const Lex_ident_sys qb_name_sys= Query_block_name::to_ident_sys(pc->thd);

  if (qb->get_name().str ||                        // QB name is already set
      qb->get_parent()->find_by_name(qb_name_sys)) // Name is already used
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, QB_NAME_HINT_ENUM, true,
               &qb_name_sys, nullptr, nullptr, nullptr);
    return false;
  }

  qb->set_name(qb_name_sys);
  return false;
}


void Parser::Semijoin_hint::fill_strategies_map(Opt_hints_qb *qb) const
{
  // Loop for hints like SEMIJOIN(firstmatch, dupsweedout)
  const Hint_param_opt_sj_strategy_list &hint_param_strategy_list= *this;
  for (const Semijoin_strategy &strat : hint_param_strategy_list)
    add_strategy_to_map(strat.id(), qb);

  // Loop for hints like SEMIJOIN(@qb1 firstmatch, dupsweedout)
  const Opt_sj_strategy_list &opt_sj_strategy_list= *this;
  for (const Semijoin_strategy &strat : opt_sj_strategy_list)
    add_strategy_to_map(strat.id(), qb);
}


void Parser::Semijoin_hint::add_strategy_to_map(TokenID token_id,
                                                Opt_hints_qb *qb) const
{
  switch(token_id)
  {
  case TokenID::keyword_DUPSWEEDOUT:
    qb->semijoin_strategies_map |= OPTIMIZER_SWITCH_DUPSWEEDOUT;
    break;
  case TokenID::keyword_FIRSTMATCH:
    qb->semijoin_strategies_map |= OPTIMIZER_SWITCH_FIRSTMATCH;
    break;
  case TokenID::keyword_LOOSESCAN:
    qb->semijoin_strategies_map |= OPTIMIZER_SWITCH_LOOSE_SCAN;
    break;
  case TokenID::keyword_MATERIALIZATION:
    qb->semijoin_strategies_map |= OPTIMIZER_SWITCH_MATERIALIZATION;
    break;
  default:
    DBUG_ASSERT(0);
  }
}

/*
  Resolve a parsed semijoin hint, i.e. set up proper Opt_hint_* structures
  which will be used later during query preparation and optimization.

Return value:
- false: no critical errors, warnings on duplicated hints,
       unresolved query block names, etc. are allowed
- true: critical errors detected, break further hints processing
*/
bool Parser::Semijoin_hint::resolve(Parse_context *pc) const
{
  const Semijoin_hint_type &semijoin_hint_type= *this;
  bool hint_state; // true - SEMIJOIN(), false - NO_SEMIJOIN()
  if (semijoin_hint_type.id() == TokenID::keyword_SEMIJOIN)
    hint_state= true;
  else
    hint_state= false;
  Opt_hints_qb *qb;
  if (const At_query_block_name_opt_strategy_list &
          at_query_block_name_opt_strategy_list __attribute__((unused)) = *this)
  {
    /*
      This is @ query_block_name opt_strategy_list,
      e.g. SEMIJOIN(@qb1) or SEMIJOIN(@qb1 firstmatch, loosescan)
    */
    const Lex_ident_sys qb_name= Query_block_name::to_ident_sys(pc->thd);
    qb= resolve_for_qb_name(pc, hint_state, &qb_name);
  }
  else
  {
    //  This is opt_strategy_list, e.g. SEMIJOIN(loosescan, dupsweedout)
    Lex_ident_sys empty_qb_name= Lex_ident_sys();
    qb= resolve_for_qb_name(pc, hint_state, &empty_qb_name);
  }
  if (qb)
    qb->semijoin_hint= this;
  return false;
}


/*
  Helper function to be called by Semijoin_hint::resolve().

Return value:
- pointer to Opt_hints_qb if the hint was resolved successfully
- NULL if the hint was ignored
*/
Opt_hints_qb* Parser::Semijoin_hint::
    resolve_for_qb_name(Parse_context *pc, bool hint_state,
                        const Lex_ident_sys *qb_name) const
{
  Opt_hints_qb *qb= find_qb_hints(pc, *qb_name, SEMIJOIN_HINT_ENUM, hint_state);
  if (!qb)
    return nullptr;
  if (qb->subquery_hint)
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, SEMIJOIN_HINT_ENUM,
               hint_state, qb_name, nullptr, nullptr, this);
    return nullptr;
  }
  if (qb->set_switch(hint_state, SEMIJOIN_HINT_ENUM, false))
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, SEMIJOIN_HINT_ENUM,
               hint_state, qb_name, nullptr, nullptr, this);
    return nullptr;
  }
  fill_strategies_map(qb);
  return qb;
}


void Parser::Semijoin_hint::append_args(THD *thd, String *str) const
{
  // Loop for hints without query block name, e.g. SEMIJOIN(firstmatch, dupsweedout)
  const Hint_param_opt_sj_strategy_list &hint_param_strategy_list= *this;
  uint32 len_before= str->length();
  for (const Semijoin_strategy &strat : hint_param_strategy_list)
  {
    if (str->length() > len_before)
      str->append(STRING_WITH_LEN(", "));
    append_strategy_name(strat.id(), str);
  }

  // Loop for hints with query block name, e.g. SEMIJOIN(@qb1 firstmatch, dupsweedout)
  const Opt_sj_strategy_list &opt_sj_strategy_list= *this;
  for (const Semijoin_strategy &strat : opt_sj_strategy_list)
  {
    if (str->length() > len_before)
      str->append(STRING_WITH_LEN(", "));
    append_strategy_name(strat.id(), str);
  }
}


void Parser::Semijoin_hint::
    append_strategy_name(TokenID token_id, String *str) const
{
  switch(token_id)
  {
  case TokenID::keyword_DUPSWEEDOUT:
    str->append(STRING_WITH_LEN("DUPSWEEDOUT"));
    break;
  case TokenID::keyword_FIRSTMATCH:
    str->append(STRING_WITH_LEN("FIRSTMATCH"));
    break;
  case TokenID::keyword_LOOSESCAN:
    str->append(STRING_WITH_LEN("LOOSESCAN"));
    break;
  case TokenID::keyword_MATERIALIZATION:
    str->append(STRING_WITH_LEN("MATERIALIZATION"));
    break;
  default:
    DBUG_ASSERT(0);
  }
}

/*
  Resolve a parsed subquery hint, i.e. set up proper Opt_hint_* structures
  which will be used later during query preparation and optimization.

Return value:
- false: no critical errors, warnings on duplicated hints,
       unresolved query block names, etc. are allowed
- true: critical errors detected, break further hints processing
*/
bool Parser::Subquery_hint::resolve(Parse_context *pc) const
{
  Opt_hints_qb *qb;
  if (const At_query_block_name_subquery_strategy &
          at_query_block_name_subquery_strategy= *this)
  {
    /*
      This is @ query_block_name subquery_strategy,
      e.g. SUBQUERY(@qb1 INTOEXISTS)
    */
    const Lex_ident_sys qb_name= Query_block_name::to_ident_sys(pc->thd);
    const Subquery_strategy &strat= at_query_block_name_subquery_strategy;
    qb= resolve_for_qb_name(pc, strat.id(), &qb_name);
  }
  else
  {
    //  This is subquery_strategy, e.g. SUBQUERY(MATERIALIZATION)
    Lex_ident_sys empty_qb_name= Lex_ident_sys();
    const Hint_param_subquery_strategy &strat= *this;
    qb= resolve_for_qb_name(pc, strat.id(), &empty_qb_name);
  }
  if (qb)
    qb->subquery_hint= this;
  return false;
}


/*
  Helper function to be called by Subquery_hint::resolve().

Return value:
- pointer to Opt_hints_qb if the hint was resolved successfully
- NULL if the hint was ignored
*/
Opt_hints_qb* Parser::Subquery_hint::
    resolve_for_qb_name(Parse_context *pc, TokenID token_id,
                        const Lex_ident_sys *qb_name) const
{
  Opt_hints_qb *qb= find_qb_hints(pc, *qb_name, SUBQUERY_HINT_ENUM, true);
  if (!qb)
    return nullptr;
  if (qb->semijoin_hint)
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, SUBQUERY_HINT_ENUM,
               true, qb_name, nullptr, nullptr, this);
    return nullptr;
  }
  if (qb->set_switch(true, SUBQUERY_HINT_ENUM, false))
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, SUBQUERY_HINT_ENUM,
               true, qb_name, nullptr, nullptr, this);
    return nullptr;
  }
  set_subquery_strategy(token_id, qb);
  return qb;
}


void Parser::Subquery_hint::set_subquery_strategy(TokenID token_id,
                                                  Opt_hints_qb *qb) const
{
  switch(token_id)
  {
  case TokenID::keyword_INTOEXISTS:
    qb->subquery_strategy= SUBS_IN_TO_EXISTS;
    break;
  case TokenID::keyword_MATERIALIZATION:
    qb->subquery_strategy= SUBS_MATERIALIZATION;
    break;
  default:
    DBUG_ASSERT(0);
  }
}


void Parser::Subquery_hint::append_args(THD *thd, String *str) const
{
  TokenID token_id;
  if (const At_query_block_name_subquery_strategy &
          at_query_block_name_subquery_strategy= *this)
  {
    const Subquery_strategy &strat= at_query_block_name_subquery_strategy;
    token_id= strat.id();
  }
  else
  {
    const Hint_param_subquery_strategy& hint_param_strat= *this;
    token_id= hint_param_strat.id();
  }

  switch(token_id)
  {
  case TokenID::keyword_INTOEXISTS:
    str->append(STRING_WITH_LEN("INTOEXISTS"));
    break;
  case TokenID::keyword_MATERIALIZATION:
    str->append(STRING_WITH_LEN("MATERIALIZATION"));
    break;
  default:
    DBUG_ASSERT(0);
  }
}

/*
  This is the first step of MAX_EXECUTION_TIME() hint resolution. It is invoked
  during the parsing phase, but at this stage some essential information is
  not yet available, preventing a full validation of the hint.
  Particularly, the type of SQL command, mark of a stored procedure execution
  or whether SELECT_LEX is not top-level (i.e., a subquery) are not yet set.
  However, some basic checks like the numeric argument validation or hint
  duplication check can still be performed.
  The second step of hint validation is performed during the JOIN preparation
  phase, within Opt_hints_global::resolve(). By this point, all necessary
  information is up-to-date, allowing the hint to be fully resolved
*/
bool Parser::Max_execution_time_hint::resolve(Parse_context *pc) const
{
  const Unsigned_Number& hint_arg= *this;
  const ULonglong_null time_ms= hint_arg.get_ulonglong();

  if (time_ms.is_null() || time_ms.value() == 0 || time_ms.value() > INT_MAX32)
  {
    print_warn(pc->thd, ER_BAD_OPTION_VALUE, MAX_EXEC_TIME_HINT_ENUM,
               true, NULL, NULL, NULL, this);
    return false;
  }

  Opt_hints_global *global_hint= get_global_hints(pc);
  if (global_hint->is_specified(MAX_EXEC_TIME_HINT_ENUM))
  {
    // Hint duplication: /*+ MAX_EXECUTION_TIME ... MAX_EXECUTION_TIME */
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, MAX_EXEC_TIME_HINT_ENUM, true,
               NULL, NULL, NULL, this);
    return false;
  }

  global_hint->set_switch(true, MAX_EXEC_TIME_HINT_ENUM, false);
  global_hint->max_exec_time_hint= this;
  global_hint->max_exec_time_select_lex= pc->select;
  return false;
}


void Parser::Join_order_hint::append_args(THD *thd, String *str) const
{
  bool first_table_name= true;
  for (const Parser::Table_name_and_Qb& tbl : table_names)
  {
    if (!first_table_name)
      str->append(STRING_WITH_LEN(","));
    append_table_name(thd, str, tbl.table_name, tbl.qb_name);
    first_table_name= false;
  }
}

/*
  Resolve a parsed join order hint, i.e. set up proper Opt_hint_* structures
  which will be used later during query preparation and optimization.

  Return value:
  - false: no critical errors, warnings on duplicated hints,
         unresolved query block names, etc. are allowed
  - true: critical errors detected, break further hints processing
*/
bool Parser::Join_order_hint::resolve(Parse_context *pc)
{
  const Join_order_hint_type &join_order_hint_type= *this;

  switch (join_order_hint_type.id())
  {
  case TokenID::keyword_JOIN_FIXED_ORDER:
    hint_type= JOIN_FIXED_ORDER_HINT_ENUM;
    break;
  case TokenID::keyword_JOIN_ORDER:
    hint_type= JOIN_ORDER_HINT_ENUM;
    break;
  case TokenID::keyword_JOIN_PREFIX:
    hint_type= JOIN_PREFIX_HINT_ENUM;
    break;
  case TokenID::keyword_JOIN_SUFFIX:
    hint_type= JOIN_SUFFIX_HINT_ENUM;
    break;
  default:
    DBUG_ASSERT(0);
    return true;
  }

  Opt_hints_qb *qb= nullptr;
  Lex_ident_sys qb_name;
  if (const At_query_block_name_opt_table_name_list &at_qb_tab_list= *this)
  {
    // this is @ query_block_name opt_table_name_list
    qb_name= Query_block_name::to_ident_sys(pc->thd);
    qb= find_qb_hints(pc, qb_name, hint_type, true);
    // Compose `tables_names` list for warnings and final hints resolving
    const Opt_table_name_list &opt_table_name_list= at_qb_tab_list;
    for (const Table_name &table : opt_table_name_list)
    {
      Parser::Table_name_and_Qb *tbl_qb= new (pc->mem_root)
        Parser::Table_name_and_Qb(table.to_ident_sys(pc->thd), Lex_ident_sys());
      if (!tbl_qb)
        return true;
      table_names.push_back(tbl_qb, pc->mem_root);
    }
  }
  else
  {
    // this is opt_hint_param_table_list, query block name is not specified
    qb= find_qb_hints(pc, Lex_ident_sys(), hint_type, true);
    const Opt_hint_param_table_list &opt_hint_param_table_list= *this;
    for (const Hint_param_table &table : opt_hint_param_table_list)
    {
      // e.g. JOIN_ORDER(t1@qb1, t2@qb2, t3)
      Parser::Table_name_and_Qb *tbl_qb=
        new (pc->mem_root) Parser::Table_name_and_Qb(
          table.Table_name::to_ident_sys(pc->thd),
          table.Query_block_name::to_ident_sys(pc->thd));
      table_names.push_back(tbl_qb, pc->mem_root);
    }
  }

  if (qb == nullptr)
    return false;

  if ((hint_type != JOIN_FIXED_ORDER_HINT_ENUM && table_names.is_empty()) ||
      (hint_type == JOIN_FIXED_ORDER_HINT_ENUM && !table_names.is_empty()))
  {
    /*
      Skipping table name(s) only allowed and required for the
      JOIN_FIXED_ORDER hint and is not allowed for other hint types
    */
    print_warn(pc->thd, ER_WARN_MALFORMED_HINT, hint_type, true,
               &qb_name, nullptr, nullptr, this);
    return false;
  }

  if (hint_type == JOIN_FIXED_ORDER_HINT_ENUM)
  {
    /*
      This is JOIN_ORDER_FIXED() or JOIN_ORDER_FIXED(@qb1)
      There can be only one JOIN_ORDER_FIXED hint in a query block,
      other hints are not allowed in this case
    */
    if (qb->has_join_order_hints()|| qb->join_fixed_order)
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, true,
                 &qb_name, nullptr, nullptr, this);
      return false;
    }
    qb->join_fixed_order= this;
    qb->set_switch(true, hint_type, false);
    pc->select->options |= SELECT_STRAIGHT_JOIN;
    return false;
  }

  // Finished with processing of JOIN_FIXED_ORDER()
  DBUG_ASSERT(hint_type != JOIN_FIXED_ORDER_HINT_ENUM);
  /*
    Hints except JOIN_ORDER() must not duplicate. If there is JOIN_ORDER_FIXED()
    already, then other hints are not allowed for this query block
  */
  if ((qb->get_switch(hint_type) && hint_type != JOIN_ORDER_HINT_ENUM) ||
       qb->join_fixed_order)
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, true,
               &qb_name, nullptr, nullptr, this);
    return false;
  }

  switch(hint_type)
  {
  case JOIN_PREFIX_HINT_ENUM:
    if (qb->join_prefix || qb->add_join_order_hint(this))
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, true,
                 &qb_name, nullptr, nullptr, this);
      return false;
    }
    qb->join_prefix= this;
    qb->set_switch(true, JOIN_PREFIX_HINT_ENUM, false);
    break;
  case JOIN_SUFFIX_HINT_ENUM:
    if (qb->join_suffix || qb->add_join_order_hint(this))
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, true,
                 &qb_name, nullptr, nullptr, this);
      return false;
    }
    qb->join_suffix= this;
    qb->set_switch(true, JOIN_SUFFIX_HINT_ENUM, false);
    break;
  case JOIN_ORDER_HINT_ENUM:
    // Multiple JOIN_ORDER() hints are allowed
    if (qb->add_join_order_hint(this))
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, true,
                 &qb_name, nullptr, nullptr, this);
      return false;
    }
    qb->set_switch(true, JOIN_ORDER_HINT_ENUM, false);
    break;
  default:
    DBUG_ASSERT(0);
  }
  return false;
}


void Parser::Max_execution_time_hint::append_args(THD *thd, String *str) const
{
  const Unsigned_Number& hint_arg= *this;
  str->append(ErrConvString(hint_arg.str, hint_arg.length,
                            &my_charset_latin1).lex_cstring());
}


ulonglong Parser::Max_execution_time_hint::get_milliseconds() const
{
  const Unsigned_Number& hint_arg= *this;
  return hint_arg.get_ulonglong().value();
}


bool Parser::Hint_list::resolve(Parse_context *pc) const
{
  if (pc->thd->lex->create_view)
  {
    // we're creating or modifying a view, hints are not allowed here
    push_warning(pc->thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_HINTS_INSIDE_VIEWS_NOT_SUPPORTED,
                 ER_THD(pc->thd, ER_HINTS_INSIDE_VIEWS_NOT_SUPPORTED));
    return false;
  }

  if (!get_qb_hints(pc))
    return true;

  for (Hint_list::iterator li= this->begin(); li != this->end(); ++li)
  {
    Parser::Hint &hint= *li;
    if (const Table_level_hint &table_hint= hint)
    {
      if (table_hint.resolve(pc))
        return true;
    }
    else if (const Index_level_hint &index_hint= hint)
    {
      if (index_hint.resolve(pc))
        return true;
    }
    else if (const Qb_name_hint &qb_hint= hint)
    {
      if (qb_hint.resolve(pc))
        return true;
    }
    else if (const Max_execution_time_hint &max_hint= hint)
    {
      if (max_hint.resolve(pc))
        return true;
    }
    else if (const Semijoin_hint &sj_hint= hint)
    {
      if (sj_hint.resolve(pc))
        return true;
    }
    else if (const Subquery_hint &subq_hint= hint)
    {
      if (subq_hint.resolve(pc))
        return true;
    }
    else if (Join_order_hint &join_order_hint= hint)
    {
      if (join_order_hint.resolve(pc))
        return true;
    }
    else {
      DBUG_ASSERT(0);
    }
  }
  return false;
}
