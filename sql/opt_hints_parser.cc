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


/**
  Returns a pointer to Opt_hints_global object,
  creates Opt_hints object if not exist.

  @param pc   pointer to Parse_context object

  @return  pointer to Opt_hints object,
           NULL if failed to create the object
*/

static Opt_hints_global *get_global_hints(Parse_context *pc)
{
  LEX *lex= pc->thd->lex;

  if (!lex->opt_hints_global)
    lex->opt_hints_global= new Opt_hints_global(pc->thd->mem_root);
  if (lex->opt_hints_global)
    lex->opt_hints_global->set_resolved();
  return lex->opt_hints_global;
}


static Opt_hints_qb *get_qb_hints(Parse_context *pc)
{
  if (pc->select->opt_hints_qb)
    return pc->select->opt_hints_qb;

  Opt_hints_global *global_hints= get_global_hints(pc);
  if (global_hints == NULL)
    return NULL;

  Opt_hints_qb *qb= new Opt_hints_qb(global_hints, pc->thd->mem_root,
                                     pc->select->select_number);
  if (qb)
  {
    global_hints->register_child(qb);
    pc->select->opt_hints_qb= qb;
    qb->set_resolved();
  }
  return qb;
}

/**
  Find existing Opt_hints_qb object, print warning
  if the query block is not found.

  @param pc          pointer to Parse_context object
  @param table_name  query block name
  @param hint        processed hint

  @return  pointer to Opt_hints_table object if found,
           NULL otherwise
*/

static Opt_hints_qb *find_qb_hints(Parse_context *pc,
                                   const LEX_CSTRING *qb_name/*, OLEGS:
                                   PT_hint *hint*/)
{
  if (qb_name->length == 0) // no QB NAME is used
    return pc->select->opt_hints_qb;

  Opt_hints_qb *qb= static_cast<Opt_hints_qb *>
    (pc->thd->lex->opt_hints_global->find_by_name(qb_name));

  // OLEGS:
  // if (qb == NULL)
  // {
  //   hint->print_warn(pc->thd, ER_WARN_UNKNOWN_QB_NAME,
  //                    qb_name, NULL, NULL, NULL);
  // }

  return qb;
}


bool Optimizer_hint_parser::Table_level_hint::resolve(Parse_context *pc) const
{
  const Table_level_hint_type &table_level_hint_type= *this;
  opt_hints_enum __attribute__((unused)) hint_type; // OLEGS:

  switch (table_level_hint_type.id())
  {
  case TokenID::keyword_BNL:
     hint_type= BNL_HINT_ENUM;
     break;
  case TokenID::keyword_NO_BNL:
     hint_type= BNL_HINT_ENUM; // OLEGS: value?
     break;
  case TokenID::keyword_BKA:
     hint_type= BKA_HINT_ENUM;
     break;
  case TokenID::keyword_NO_BKA:
     hint_type= BKA_HINT_ENUM; // OLEGS: value?
     break;
  default:
     DBUG_ASSERT(0);
     return true;
  }

  if (const At_query_block_name_opt_table_name_list &
       at_query_block_name_opt_table_name_list= *this)
  {
    // this is @ query_block_name opt_table_name_list
    const Query_block_name &qb_name= *this;
    if (at_query_block_name_opt_table_name_list.is_empty())
    {
      // e.g. BKA(@qb1)
      Opt_hints_qb *qb= find_qb_hints(pc, &qb_name/*, this*/);
      if (qb == NULL)
        return false;
    }
    else
    {
      // e.g. BKA(@qb1 t1, t2, t3)
      DBUG_ASSERT(100);
    }
  }
  else
  {
    // this is opt_hint_param_table_list
    const Opt_table_name_list &table_name_list= *this;
    const Opt_hint_param_table_list &opt_hint_param_table_list= *this;
    if (table_name_list.is_empty() && opt_hint_param_table_list.is_empty())
    {
      // e.g. BKA()
      Opt_hints_qb *qb= find_qb_hints(pc, &null_clex_str/*, this*/);
      if (qb == NULL)
        return false;
      
    }
    for (const Table_name &table : table_name_list)
    {
      // e.g. BKA(t1, t2)
      // OLEGS: duplicates with above
      Opt_hints_qb *qb= find_qb_hints(pc, &null_clex_str/*, this*/);
      if (qb == NULL)
        return false;
      std::cout << table;
    }
  
    for (const Hint_param_table &table : opt_hint_param_table_list)
    {
      // e.g. BKA(t1@qb1, t2@qb2)
      const Query_block_name &qb_name= table;
      Opt_hints_qb *qb= find_qb_hints(pc, &qb_name/*, this*/);
      if (qb == NULL)
        return false;
      // OLEGS: todo
  //     Opt_hints_table *tab= get_table_hints(pc, table_name, qb);
  //     if (!tab)
  //       return true;
  
  //     if (tab->set_switch(switch_on(), type(), true))
  //       print_warn(pc->thd, ER_WARN_CONFLICTING_HINT,
  //                  &table_name->opt_query_block,
  //                  &table_name->table, NULL, this);
    }
  }
  return false;
}


bool Optimizer_hint_parser::Index_level_hint::resolve(Parse_context *pc) const
{
  const Index_level_hint_type &index_level_hint_type= *this;
  opt_hints_enum __attribute__((unused)) hint_type; // OLEGS:

  switch (index_level_hint_type.id())
  {
  case TokenID::keyword_NO_ICP:
     hint_type= ICP_HINT_ENUM;
     break;
  case TokenID::keyword_MRR:
     hint_type= MRR_HINT_ENUM;
     break;
  case TokenID::keyword_NO_MRR:
     hint_type= MRR_HINT_ENUM; // OLEGS: value?
     break;
  case TokenID::keyword_NO_RANGE_OPTIMIZATION:
     hint_type= NO_RANGE_HINT_ENUM;
     break;
  default:
     DBUG_ASSERT(0);
     return true;
  }
  
  const Hint_param_table_ext* table= static_cast<const Hint_param_table_ext*>(
      static_cast<const Index_level_hint_body*>(
        static_cast<const Index_level_hint*>(this)));
          
  //const Hint_param_table_ext &table= *this;
  const Query_block_name *qb_name= static_cast<const Query_block_name *>(table);
  
  Opt_hints_qb *qb= find_qb_hints(pc, qb_name/*, this*/);
  if (qb == NULL)
    return false;
  
  for (const Hint_param_index &ind : *this)
  {
    std::cout <<ind;
  }
  
  return false;
}


bool Optimizer_hint_parser::Qb_name_hint::resolve(Parse_context *pc) const
{
  // OLEGS: todo
  Opt_hints_qb *qb= pc->select->opt_hints_qb;

  DBUG_ASSERT(qb);

  const Query_block_name &qb_name= *this;

  if (qb->get_name() ||                         // QB name is already set
      qb->get_parent()->find_by_name(&qb_name)) // Name is already used
  {
    // OLEGS: todo
    // print_warn(pc->thd, ER_WARN_CONFLICTING_HINT,
    //            NULL, NULL, NULL, this);
    return false;
  }

  qb->set_name(&qb_name);
  return false;
}

bool
Optimizer_hint_parser::
  Hint_list::resolve(Parse_context *pc)
{
  if (!get_qb_hints(pc))
    return true;

  List_iterator_fast<Optimizer_hint_parser::Hint> li(*this);
  while(Optimizer_hint_parser::Hint *hint= li++)
  {
    if (const Table_level_hint &table_hint=
        static_cast<const Table_level_hint &>(*hint))
    {
      if (table_hint.resolve(pc))
        return true;
    }
    else if (const Index_level_hint &index_hint=
             static_cast<const Index_level_hint &>(*hint))
    {
      if (index_hint.resolve(pc))
        return true;
    }
    else if (const Qb_name_hint &qb_hint=
             static_cast<const Qb_name_hint &>(*hint))
    {
      if (qb_hint.resolve(pc))
        return true;
    }
  }
  // for (PT_hint **h= hints.begin(), **end= hints.end(); h < end; h++)
  // {
  //   if (*h != NULL && (*h)->contextualize(pc))
  //     return true;
  // }
  return false;
}
