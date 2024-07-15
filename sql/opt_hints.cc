/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

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
#include "opt_hints_parser.h"

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
  {"BKA", true, true},
  {"BNL", true, true},
  {"ICP", true, true},
  {"MRR", true, true},
  {"NO_RANGE_OPTIMIZATION", true, true},
  {"QB_NAME", false, false},
  {0, 0, 0}
};

/**
  Prefix for system generated query block name.
  Used in information warning in EXPLAIN oputput.
*/

const LEX_CSTRING sys_qb_prefix=  {"select#", 7};


/*
  Compare LEX_CSTRING objects.

  @param s     Pointer to LEX_CSTRING
  @param t     Pointer to LEX_CSTRING

  @return  0 if strings are equal
           1 if s is greater
          -1 if t is greater
*/

static int cmp_lex_string(const LEX_CSTRING *s,
                          const LEX_CSTRING *t)
{
   return system_charset_info->
     coll->strnncollsp(system_charset_info,
                       (uchar *) s->str, s->length,
                       (uchar *) t->str, t->length);
}


static const Lex_ident_sys null_ident_sys;

static void print_warn(THD *thd, uint err_code, opt_hints_enum hint_type,
                bool hint_state,
                const Lex_ident_sys *qb_name_arg,
                const Lex_ident_sys *table_name_arg,
                const Lex_ident_sys *key_name_arg/*,        PT_hint *hint*/)
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

  /* ER_WARN_CONFLICTING_HINT with one argument */
  str.append('(');

  /* Append table name */
  if (table_name_arg && table_name_arg->length > 0)
    append_identifier(thd, &str, table_name_arg->str, table_name_arg->length);

  /* Append QB name */
  if (qb_name_arg && qb_name_arg->length > 0)
  {
    str.append(STRING_WITH_LEN("@"));
    append_identifier(thd, &str, qb_name_arg->str, qb_name_arg->length);
  }

  /* Append key name */
  if (key_name_arg && key_name_arg->length > 0)
  {
    str.append(' ');
    append_identifier(thd, &str, key_name_arg->str, key_name_arg->length);
  }

  /* Append additional hint arguments if they exist */
  // OLEGS: todo
  // if (hint)
  // {
  //   if (qb_name_arg || table_name_arg || key_name_arg)
  //     str.append(' ');

  //   hint->append_args(thd, &str);
  // }

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
  @param hint        processed hint // OLEGS: amend this

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
               &qb_name, NULL, NULL);
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
    tab= new Opt_hints_table(table_name, qb, pc->thd->mem_root);
    qb->register_child(tab);
  }

  return tab;
}




bool Opt_hints::get_switch(opt_hints_enum type_arg) const
{
  if (is_specified(type_arg))
    return hints_map.switch_on(type_arg);

  if (opt_hint_info[type_arg].check_upper_lvl)
    return parent->get_switch(type_arg);

  return false;
}


Opt_hints* Opt_hints::find_by_name(const LEX_CSTRING &name_arg) const
{
  for (uint i= 0; i < child_array.size(); i++)
  {
    const LEX_CSTRING *name= child_array[i]->get_name();
    if (name && !cmp_lex_string(name, &name_arg))
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
      append_name(thd, str);
      // OLEGS: 
      //if (!opt_hint_info[i].switch_hint)
      //  get_complex_hints(i)->append_args(thd, str);
      str->append(STRING_WITH_LEN(") "));
    }
  }

  for (uint i= 0; i < child_array.size(); i++)
    child_array[i]->print(thd, str);
}


void Opt_hints::append_hint_type(String *str, opt_hints_enum type)
{
  const char* hint_name= opt_hint_info[type].hint_name;
  if(!hints_map.switch_on(type))
    str->append(STRING_WITH_LEN("NO_"));
  str->append(hint_name);
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


PT_hint *Opt_hints_global::get_complex_hints(uint type)
{
  DBUG_ASSERT(0);
  return NULL;
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


Opt_hints_table *Opt_hints_qb::adjust_table_hints(TABLE *table,
                                                  const Lex_ident_table &alias)
{
  Opt_hints_table *tab= static_cast<Opt_hints_table *>(find_by_name(alias));

  table->pos_in_table_list->opt_hints_qb= this;

  if (!tab)                            // Tables not found
    return NULL;

  tab->adjust_key_hints(table);
  return tab;
}


void Opt_hints_table::adjust_key_hints(TABLE *table)
{
  set_resolved();
  if (child_array_ptr()->size() == 0)  // No key level hints
  {
    get_parent()->incr_resolved_children();
    return;
  }

  /* Make sure that adjustement is called only once. */
  DBUG_ASSERT(keyinfo_array.size() == 0);
  keyinfo_array.resize(table->s->keys, NULL);

  for (Opt_hints** hint= child_array_ptr()->begin();
       hint < child_array_ptr()->end(); ++hint) 
  {
    KEY *key_info= table->key_info;
    for (uint j= 0 ; j < table->s->keys ; j++, key_info++)
    {
      if (!cmp_lex_string((*hint)->get_name(), &key_info->name))
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

  if (opt_hint_info[type_arg].switch_hint)
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
    /* Complex hint, not implemented atm */
    DBUG_ASSERT(0);
  }
  return false;
}


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
                      uint optimizer_switch)
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

  return optimizer_flag(thd, optimizer_switch);
}


bool hint_table_state_or_fallback(const THD *thd, const TABLE *table,
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


bool Optimizer_hint_parser::Table_level_hint::resolve(Parse_context *pc) const
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
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &qb_name_sys, NULL, NULL/*, this*/);
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
          return true; // OLEGS: why no warning?
        if (tab->set_switch(hint_state, hint_type, true))
          print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                     &qb_name_sys, &table_name_sys, NULL/*, this*/);
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
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &null_ident_sys, NULL, NULL/*, this*/);
      return false;
    }
    for (const Table_name &table : table_name_list)
    {
      // e.g. BKA(t1, t2)
      const Lex_ident_sys table_name_sys= table.to_ident_sys(pc->thd);
      Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
      if (!tab)
        return true; // OLEGS: no warning?
      if (tab->set_switch(hint_state, hint_type, true))
        print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                   &null_ident_sys, &table_name_sys, NULL/*, this*/);
    }
  
    for (const Hint_param_table &table : opt_hint_param_table_list)
    {
      // e.g. BKA(t1@qb1, t2@qb2)
      const Lex_ident_sys qb_name_sys= table.Query_block_name::
                                         to_ident_sys(pc->thd);
      Opt_hints_qb *qb= find_qb_hints(pc, qb_name_sys, hint_type, hint_state);
      if (qb == NULL)
        return false;
      // OLEGS: todo
      const Lex_ident_sys table_name_sys= table.Table_name::
                                            to_ident_sys(pc->thd);
      Opt_hints_table *tab= get_table_hints(pc, table_name_sys, qb);
      if (!tab)
        return true; // OLEGS: why no warning?
  
      if (tab->set_switch(hint_state, hint_type, true))
         print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                    &qb_name_sys, &table_name_sys, NULL/*, this*/);
    }
  }
  return false;
}


bool Optimizer_hint_parser::Index_level_hint::resolve(Parse_context *pc) const
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
    return true; // OLEGS: why no warning?
  
  if (is_empty())  // Table level hint
  {
    if (tab->set_switch(hint_state, hint_type, false))
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                 &qb_name_sys, &table_name_sys, NULL/*, this*/);
    return false;
  }

  for (const Hint_param_index &index_name : *this)
  {
    const Lex_ident_sys index_name_sys= index_name.to_ident_sys(pc->thd);
    Opt_hints_key *idx= (Opt_hints_key *)tab->find_by_name(index_name_sys);
    if (!idx)
    {
      idx= new Opt_hints_key(index_name_sys, tab, pc->thd->mem_root);
      tab->register_child(idx);
    }

    if (idx->set_switch(hint_state, hint_type, true))
      print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, hint_type, hint_state,
                 &qb_name_sys, &table_name_sys, &index_name_sys/*, this*/);
  }

  return false;
}


bool Optimizer_hint_parser::Qb_name_hint::resolve(Parse_context *pc) const
{
  Opt_hints_qb *qb= pc->select->opt_hints_qb;

  DBUG_ASSERT(qb);

  const Lex_ident_sys qb_name_sys= Query_block_name::to_ident_sys(pc->thd);

  if (qb->get_name() ||                            // QB name is already set
      qb->get_parent()->find_by_name(qb_name_sys)) // Name is already used
  {
    print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, QB_NAME_HINT_ENUM, false,
               NULL, NULL, NULL/*, this*/);
    return false;
  }

  qb->set_name(qb_name_sys);
  return false;
}


bool Optimizer_hint_parser::Hint_list::resolve(Parse_context *pc)
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
        return true; // OLEGS: check this result
    }
  }
  return false;
}
