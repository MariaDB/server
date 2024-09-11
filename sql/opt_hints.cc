/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2024, MariaDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_global.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_select.h"
#include "opt_hints.h"

/**
  Information about hints. Sould be
  synchronized with opt_hints_enum enum.

  Note: Hint name depends on hint state. 'NO_' prefix is added
  if appropriate hint state bit(see Opt_hints_map::hints) is not
  set. Depending on 'switch_state_arg' argument in 'parse tree
  object' constructors(see parse_tree_h../cnm ints.[h,cc]) implementor
  can control wishful form of the hint name.
*/

struct st_opt_hint_info opt_hint_info[]=
{
  {{STRING_WITH_LEN("BKA")}, true, false},
  {{STRING_WITH_LEN("BNL")}, true, false},
  {{STRING_WITH_LEN("ICP")}, true, false},
  {{STRING_WITH_LEN("MRR")}, true, false},
  {{STRING_WITH_LEN("NO_RANGE_OPTIMIZATION")}, true, false},
  {{STRING_WITH_LEN("QB_NAME")}, false, false},
  {{STRING_WITH_LEN("MAX_EXECUTION_TIME")}, false, true},
  {{STRING_WITH_LEN("SEMIJOIN")}, false, true},
  {{STRING_WITH_LEN("SUBQUERY")}, false, true},
  {null_clex_str, 0, 0}
};

/**
  Prefix for system generated query block name.
  Used in information warning in EXPLAIN oputput.
*/

const LEX_CSTRING sys_qb_prefix=  {"select#", 7};

static const Lex_ident_sys null_ident_sys;

template<typename Hint_type>
static
void print_warn(THD *thd, uint err_code, opt_hints_enum hint_type,
                bool hint_state,
                const Lex_ident_sys *qb_name_arg,
                const Lex_ident_sys *table_name_arg,
                const Lex_ident_sys *key_name_arg,
                Hint_type *hint)
{
  String str;

  /* Append hint name */
  if (!hint_state)
    str.append(STRING_WITH_LEN("NO_"));
  str.append(opt_hint_info[hint_type].hint_name);

  /* ER_WARN_UNKNOWN_QB_NAME with two arguments */
  if (err_code == ER_WARN_UNKNOWN_QB_NAME)
  {
    String qb_name_str;
    append_identifier(thd, &qb_name_str, qb_name_arg->str, qb_name_arg->length);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        err_code, ER_THD(thd, err_code),
                        qb_name_str.c_ptr_safe(), str.c_ptr_safe());
    return;
  }

  /* ER_BAD_OPTION_VALUE with two arguments. hint argument is required here */
  if (err_code == ER_BAD_OPTION_VALUE)
  {
    DBUG_ASSERT(hint);
    String args;
    hint->append_args(thd, &args);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        err_code, ER_THD(thd, err_code),
                        args.c_ptr_safe(), str.c_ptr_safe());
    return;
  }

  /* ER_WARN_CONFLICTING_HINT with one argument */
  str.append('(');

  /* Append table name */
  if (table_name_arg && table_name_arg->length > 0)
    append_identifier(thd, &str, table_name_arg->str, table_name_arg->length);

  /* Append QB name */
  bool got_qb_name= qb_name_arg && qb_name_arg->length > 0;
  if (got_qb_name)
  {
    if (hint_type != QB_NAME_HINT_ENUM)
    {
      /*
        Add the delimiter for warnings like "Hint NO_ICP(`t1`@`q1` is ignored".
        No need for the delimiter for warnings "Hint QB_NAME(qb1) is ignored"
      */
      str.append(STRING_WITH_LEN("@"));
    }
    append_identifier(thd, &str, qb_name_arg->str, qb_name_arg->length);
  }

  /* Append key name */
  if (key_name_arg && key_name_arg->length > 0)
  {
    str.append(' ');
    append_identifier(thd, &str, key_name_arg->str, key_name_arg->length);
  }

  /* Append additional hint arguments if they exist */
  if (hint)
  {
    if (got_qb_name || table_name_arg || key_name_arg)
      str.append(' ');

    hint->append_args(thd, &str);
  }

  str.append(')');

  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      err_code, ER_THD(thd, err_code), str.c_ptr_safe());
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
  {
    lex->opt_hints_global= new (pc->thd->mem_root)
        Opt_hints_global(pc->thd->mem_root);
  }
  return lex->opt_hints_global;
}


static Opt_hints_qb *get_qb_hints(Parse_context *pc)
{
  if (pc->select->opt_hints_qb)
    return pc->select->opt_hints_qb;

  Opt_hints_global *global_hints= get_global_hints(pc);
  if (global_hints == NULL)
    return NULL;

  Opt_hints_qb *qb= new (pc->thd->mem_root)
      Opt_hints_qb(global_hints, pc->thd->mem_root, pc->select->select_number);
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
  @param qb_name     query block name
  @param hint_type   the type of the hint from opt_hints_enum
  @param hint_state  true: hint enables a feature; false: disables it

  @return  pointer to Opt_hints_table object if found,
           NULL otherwise
*/

static Opt_hints_qb *find_qb_hints(Parse_context *pc,
                                   const Lex_ident_sys &qb_name,
                                   opt_hints_enum hint_type,
                                   bool hint_state)
{
  if (qb_name.length == 0) // no QB NAME is used
    return pc->select->opt_hints_qb;

  Opt_hints_qb *qb= static_cast<Opt_hints_qb *>
    (pc->thd->lex->opt_hints_global->find_by_name(qb_name));

  if (qb == NULL)
  {
    print_warn(pc->thd, ER_WARN_UNKNOWN_QB_NAME, hint_type, hint_state,
               &qb_name, NULL, NULL, (Parser::Hint*) NULL);
  }
  return qb;
}


/**
  Returns pointer to Opt_hints_table object,
  create Opt_hints_table object if not exist.

  @param pc          pointer to Parse_context object
  @param table_name  pointer to Hint_param_table object
  @param qb          pointer to Opt_hints_qb object

  @return   pointer to Opt_hints_table object,
            NULL if failed to create the object
*/

static Opt_hints_table *get_table_hints(Parse_context *pc,
                                        const Lex_ident_sys &table_name,
                                        Opt_hints_qb *qb)
{
  Opt_hints_table *tab=
    static_cast<Opt_hints_table *> (qb->find_by_name(table_name));
  if (!tab)
  {
    tab= new (pc->thd->mem_root)
        Opt_hints_table(table_name, qb, pc->thd->mem_root);
    qb->register_child(tab);
  }

  return tab;
}


bool Opt_hints::get_switch(opt_hints_enum type_arg) const
{
  if (is_specified(type_arg))
    return hints_map.is_switched_on(type_arg);

  if (opt_hint_info[type_arg].check_upper_lvl)
    return parent->get_switch(type_arg);

  return false;
}


Opt_hints* Opt_hints::find_by_name(const LEX_CSTRING &name_arg) const
{
  for (uint i= 0; i < child_array.size(); i++)
  {
    const LEX_CSTRING name= child_array[i]->get_name();
    CHARSET_INFO *cs= child_array[i]->charset_info();
    if (name.str && !cs->strnncollsp(name, name_arg))
      return child_array[i];
  }
  return NULL;
}


void Opt_hints::print(THD *thd, String *str)
{
  for (uint i= 0; i < MAX_HINT_ENUM; i++)
  {
    if (is_specified(static_cast<opt_hints_enum>(i)) && is_resolved())
    {
      append_hint_type(str, static_cast<opt_hints_enum>(i));
      str->append(STRING_WITH_LEN("("));
      uint32 len_before_name= str->length();
      append_name(thd, str);
      uint32 len_after_name= str->length();
      if (len_after_name > len_before_name)
        str->append(' ');
      if (opt_hint_info[i].has_arguments)
      {
        std::function<void(THD*, String*)> args_printer= get_args_printer();
        args_printer(thd, str);
      }
      if (str->length() == len_after_name + 1)
      {
        // No additional arguments were printed, trim the space added before
        str->length(len_after_name);
      }
      str->append(STRING_WITH_LEN(") "));
    }
  }

  for (uint i= 0; i < child_array.size(); i++)
    child_array[i]->print(thd, str);
}


void Opt_hints::append_hint_type(String *str, opt_hints_enum type)
{
  if(!hints_map.is_switched_on(type))
    str->append(STRING_WITH_LEN("NO_"));
  str->append(opt_hint_info[type].hint_name);
}


void Opt_hints::print_warn_unresolved(THD *thd)
{
  String hint_name_str, hint_type_str;
  append_name(thd, &hint_name_str);

  for (uint i= 0; i < MAX_HINT_ENUM; i++)
  {
    if (is_specified(static_cast<opt_hints_enum>(i)))
    {
      hint_type_str.length(0);
      append_hint_type(&hint_type_str, static_cast<opt_hints_enum>(i));
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          get_warn_unresolved_code(),
                          ER_THD(thd, get_warn_unresolved_code()),
                          hint_name_str.c_ptr_safe(),
                          hint_type_str.c_ptr_safe());
    }
  }
}


void Opt_hints::check_unresolved(THD *thd)
{
  if (!is_resolved())
    print_warn_unresolved(thd);

  if (!is_all_resolved())
  {
    for (uint i= 0; i < child_array.size(); i++)
      child_array[i]->check_unresolved(thd);
  }
}


Opt_hints_qb::Opt_hints_qb(Opt_hints *opt_hints_arg,
                           MEM_ROOT *mem_root_arg,
                           uint select_number_arg)
  : Opt_hints(Lex_ident_sys(), opt_hints_arg, mem_root_arg),
    select_number(select_number_arg)
{
  sys_name.str= buff;
  sys_name.length= my_snprintf(buff, sizeof(buff), "%s%lx",
                               sys_qb_prefix.str, select_number);
}


Opt_hints_table *Opt_hints_qb::adjust_hints_for_table(TABLE *table,
                                                  const Lex_ident_table &alias)
{
  Opt_hints_table *tab= static_cast<Opt_hints_table *>(find_by_name(alias));

  table->pos_in_table_list->opt_hints_qb= this;

  if (!tab)                            // Tables not found
    return NULL;

  tab->adjust_key_hints(table);
  return tab;
}


bool Opt_hints_qb::semijoin_enabled(THD *thd) const
{
  if (subquery_hint) // SUBQUERY hint disables semi-join
    return false;

  if (semijoin_hint)
  {
    // SEMIJOIN hint will always force semijoin regardless of optimizer_switch
    if(get_switch(SEMIJOIN_HINT_ENUM))
      return true;
    
    // NO_SEMIJOIN hint.  If strategy list is empty, do not use SEMIJOIN
    if (semijoin_strategies_map == 0)
      return false;

    // Fall through: NO_SEMIJOIN w/ strategies neither turns SEMIJOIN off nor on
  }

  return optimizer_flag(thd, OPTIMIZER_SWITCH_SEMIJOIN);
}


uint Opt_hints_qb::sj_enabled_strategies(uint opt_switches) const
{
  // Hints override switches
  if (semijoin_hint)
  {
    const uint strategies= semijoin_strategies_map;
    if (get_switch(SEMIJOIN_HINT_ENUM))  // SEMIJOIN hint
      return (strategies == 0) ? opt_switches : strategies;

    // NO_SEMIJOIN hint. Hints and optimizer_switch both affect strategies
    return ~strategies & opt_switches;
  }

  return opt_switches;
}


/*
  @brief
    For each index IDX, put its hints into keyinfo_array[IDX]

*/
void Opt_hints_table::adjust_key_hints(TABLE *table)
{
  set_resolved();
  if (child_array_ptr()->size() == 0)  // No key level hints
  {
    get_parent()->incr_resolved_children();
    return;
  }

  /* Make sure that adjustment is called only once. */
  DBUG_ASSERT(keyinfo_array.size() == 0);
  keyinfo_array.resize(table->s->keys, NULL);

  for (Opt_hints** hint= child_array_ptr()->begin();
       hint < child_array_ptr()->end(); ++hint) 
  {
    KEY *key_info= table->key_info;
    for (uint j= 0 ; j < table->s->keys ; j++, key_info++)
    {
      if (key_info->name.streq((*hint)->get_name()))
      {
        (*hint)->set_resolved();
        keyinfo_array[j]= static_cast<Opt_hints_key *>(*hint);
        incr_resolved_children();
      }
    }
  }

  /*
   Do not increase number of resolved tables
   if there are unresolved key objects. It's
   important for check_unresolved() function.
  */
  if (is_all_resolved())
    get_parent()->incr_resolved_children();
}


/**
  Function returns hint value depending on
  the specfied hint level. If hint is specified
  on current level, current level hint value is
  returned, otherwise parent level hint is checked.
  
  @param hint              Pointer to the hint object
  @param parent_hint       Pointer to the parent hint object,
                           should never be NULL
  @param type_arg          hint type
  @param OUT ret_val       hint value depending on
                           what hint level is used

  @return true if hint is specified, false otherwise
*/

static bool get_hint_state(Opt_hints *hint,
                           Opt_hints *parent_hint,
                           opt_hints_enum type_arg,
                           bool *ret_val)
{
  DBUG_ASSERT(parent_hint);

  if (!opt_hint_info[type_arg].has_arguments)
  {
    if (hint && hint->is_specified(type_arg))
    {
      *ret_val= hint->get_switch(type_arg);
      return true;
    }
    else if (opt_hint_info[type_arg].check_upper_lvl &&
             parent_hint->is_specified(type_arg))
    {
      *ret_val= parent_hint->get_switch(type_arg);
      return true;
    }
  }
  else
  {
    /* Complex hint with arguments, not implemented atm */
    DBUG_ASSERT(0);
  }
  return false;
}


/* 
  @brief
    Check whether a given optimization is enabled for table.keyno.
  
  @detail
    First check if a hint is present, then check optimizer_switch
*/

bool hint_key_state(const THD *thd, const TABLE *table,
                    uint keyno, opt_hints_enum type_arg,
                    uint optimizer_switch)
{
  Opt_hints_table *table_hints= table->pos_in_table_list->opt_hints_table;

  /* Parent should always be initialized */
  if (table_hints && keyno != MAX_KEY)
  {
    Opt_hints_key *key_hints= table_hints->keyinfo_array.size() > 0 ?
      table_hints->keyinfo_array[keyno] : NULL;
    bool ret_val= false;
    if (get_hint_state(key_hints, table_hints, type_arg, &ret_val))
      return ret_val;
  }

  return optimizer_flag(thd, optimizer_switch);
}


bool hint_table_state(const THD *thd, const TABLE *table,
                                  opt_hints_enum type_arg,
                                  bool fallback_value)
{
  TABLE_LIST *table_list= table->pos_in_table_list;
  if (table_list->opt_hints_qb)
  {
    bool ret_val= false;
    if (get_hint_state(table_list->opt_hints_table,
                       table_list->opt_hints_qb,
                       type_arg, &ret_val))
      return ret_val;
  }

  return fallback_value;
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
    if (at_query_block_name_opt_table_name_list.is_empty())
    {
      // e.g. BKA(@qb1)
      if (qb->set_switch(hint_state, hint_type, false))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &qb_name_sys, nullptr, nullptr, (Parser::Hint*) nullptr);
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
                     (Parser::Hint*) nullptr);
        }
      }
    }
  }
  else
  {
    // this is opt_hint_param_table_list
    const Opt_table_name_list &table_name_list= *this;
    const Opt_hint_param_table_list &opt_hint_param_table_list= *this;
    Opt_hints_qb *qb= find_qb_hints(pc, Lex_ident_sys(), hint_type, hint_state);
    if (qb == NULL)
      return false;
    if (table_name_list.is_empty() && opt_hint_param_table_list.is_empty())
    {
      // e.g. BKA()
      if (qb->set_switch(hint_state, hint_type, false))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &null_ident_sys, nullptr, nullptr, (Parser::Hint*) nullptr);
      }
      return false;
    }
    for (const Table_name &table : table_name_list)
    {
      // e.g. BKA(t1, t2)
      const Lex_ident_sys table_name_sys= table.to_ident_sys(pc->thd);
      Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
      if (!tab)
        return false;
      if (tab->set_switch(hint_state, hint_type, true))
      {
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &null_ident_sys, &table_name_sys, nullptr,
                   (Parser::Hint*) nullptr);
      }
    }
  
    for (const Hint_param_table &table : opt_hint_param_table_list)
    {
      // e.g. BKA(t1@qb1, t2@qb2)
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
                    &qb_name_sys, &table_name_sys, nullptr,
                    (Parser::Hint*) nullptr);
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

  if (is_empty())  // Table level hint
  {
    if (tab->set_switch(hint_state, hint_type, false))
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                 &qb_name_sys, &table_name_sys, nullptr,
                 (Parser::Hint*) nullptr);
    }
    return false;
  }

  for (const Hint_param_index &index_name : *this)
  {
    const Lex_ident_sys index_name_sys= index_name.to_ident_sys(pc->thd);
    Opt_hints_key *idx= (Opt_hints_key *)tab->find_by_name(index_name_sys);
    if (!idx)
    {
      idx= new (pc->thd->mem_root)
          Opt_hints_key(index_name_sys, tab, pc->thd->mem_root);
      tab->register_child(idx);
    }

    if (idx->set_switch(hint_state, hint_type, true))
    {
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                 &qb_name_sys, &table_name_sys, &index_name_sys,
                 (Parser::Hint*) nullptr);
    }
  }
  return false;
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
               &qb_name_sys, nullptr, nullptr, (Parser::Hint*) nullptr);
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
Opt_hints_qb* Parser::Semijoin_hint::resolve_for_qb_name(Parse_context *pc,
                      bool hint_state, const Lex_ident_sys *qb_name) const
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
Opt_hints_qb* Parser::Subquery_hint::resolve_for_qb_name(Parse_context *pc,
                      TokenID token_id, const Lex_ident_sys *qb_name) const
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


bool Opt_hints_global::resolve(THD *thd)
{
  if (thd->lex->is_ps_or_view_context_analysis())
    return false;

  if (!max_exec_time_hint)
  {
    /* No possible errors */
    set_resolved();
    return false;
  }

  /*
    2nd step of MAX_EXECUTION_TIME() hint validation. Some checks were already
    performed during the parsing stage (Max_execution_time_hint::resolve()),
    but the following checks can only be performed during the JOIN preparation
    because thd->lex variables are not available during parsing
  */
  if (thd->lex->sql_command != SQLCOM_SELECT || // not a SELECT statement
      thd->lex->sphead || thd->in_sub_stmt != 0 || // or a SP/trigger/event
      max_exec_time_select_lex->master_unit() != &thd->lex->unit || // or a subquery
      max_exec_time_select_lex->select_number != 1) // not a top-level select
  {
    print_warn(thd, ER_NOT_ALLOWED_IN_THIS_CONTEXT, MAX_EXEC_TIME_HINT_ENUM,
               true, NULL, NULL, NULL, max_exec_time_hint);
  }
  else
  {
    thd->reset_query_timer();
    thd->set_query_timer_force(max_exec_time_hint->get_milliseconds() * 1000);
  }
  set_resolved();
  return false;
}


bool Parser::Hint_list::resolve(Parse_context *pc) const
{
  if (pc->thd->lex->create_view)
  {
    // we're creating or modifying a view, hints are not allowed here
    push_warning_printf(pc->thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_HINTS_INSIDE_VIEWS_NOT_SUPPORTED,
                        ER_THD(pc->thd, ER_HINTS_INSIDE_VIEWS_NOT_SUPPORTED));
    return false;
  }

  if (!get_qb_hints(pc))
    return true;

  for (Hint_list::iterator li= this->begin(); li != this->end(); ++li)
  {
    const Optimizer_hint_parser::Hint &hint= *li;
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
    else {
      DBUG_ASSERT(0);
    }
  }
  return false;
}
