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
  Information about hints. Must be in sync with opt_hints_enum.

  Note: Hint name depends on hint state. 'NO_' prefix is added
  if appropriate hint state bit(see Opt_hints_map::hints) is not
  set. Depending on 'switch_state_arg' argument in 'parse tree
  object' constructors(see parse_tree_h../cnm ints.[h,cc]) implementor
  can control wishful form of the hint name.
*/

struct st_opt_hint_info opt_hint_info[]=
{
  {{STRING_WITH_LEN("BKA")}, true, false, false},
  {{STRING_WITH_LEN("BNL")}, true, false, false},
  {{STRING_WITH_LEN("ICP")}, true, false, false},
  {{STRING_WITH_LEN("MRR")}, true, false, false},
  {{STRING_WITH_LEN("NO_RANGE_OPTIMIZATION")}, true, false, false},
  {{STRING_WITH_LEN("QB_NAME")}, false, false, false},
  {{STRING_WITH_LEN("MAX_EXECUTION_TIME")}, false, true, false},
  {{STRING_WITH_LEN("SEMIJOIN")}, false, true, false},
  {{STRING_WITH_LEN("SUBQUERY")}, false, true, false},
  {{STRING_WITH_LEN("JOIN_PREFIX")}, false, true, true},
  {{STRING_WITH_LEN("JOIN_SUFFIX")}, false, true, true},
  {{STRING_WITH_LEN("JOIN_ORDER")}, false, true, true},
  {{STRING_WITH_LEN("JOIN_FIXED_ORDER")}, false, true, false},
  {{STRING_WITH_LEN("INDEX")}, false, true, false},
  {{STRING_WITH_LEN("JOIN_INDEX")}, false, true, false},
  {{STRING_WITH_LEN("GROUP_INDEX")}, false, true, false},
  {{STRING_WITH_LEN("ORDER_INDEX")}, false, true, false},
  {null_clex_str, 0, 0, 0}
};

/**
  Prefix for system generated query block name.
  Used in information warning in EXPLAIN oputput.
*/

const LEX_CSTRING sys_qb_prefix=  {"select#", 7};


/*
  This is a version of push_warning_printf() guaranteeing no escalation of
  the warning to the level of error
*/
void push_warning_safe(THD *thd, Sql_condition::enum_warning_level level,
                         uint code, const char *format, ...)
{
  va_list args;
  DBUG_ENTER("push_warning_safe");
  DBUG_PRINT("enter",("warning: %u", code));

  va_start(args,format);
  bool save_abort_on_warning= thd->abort_on_warning;
  thd->abort_on_warning= false; // Don't escalate to the level of error
  push_warning_printf_va_list(thd, level,code, format, args);
  va_end(args);
  thd->abort_on_warning= save_abort_on_warning;
  DBUG_VOID_RETURN;
}


void print_warn(THD *thd, uint err_code, opt_hints_enum hint_type,
                bool hint_state,
                const Lex_ident_sys *qb_name_arg,
                const Lex_ident_sys *table_name_arg,
                const Lex_ident_sys *key_name_arg,
                const Printable_parser_rule *hint)
{
  String str;

  /* Append hint name */
  if (!hint_state)
    str.append(STRING_WITH_LEN("NO_"));
  str.append(opt_hint_info[hint_type].hint_type);

  /* ER_WARN_UNKNOWN_QB_NAME with two arguments */
  if (err_code == ER_WARN_UNKNOWN_QB_NAME)
  {
    String qb_name_str;
    append_identifier(thd, &qb_name_str, qb_name_arg->str, qb_name_arg->length);
    push_warning_safe(thd, Sql_condition::WARN_LEVEL_WARN,
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
    push_warning_safe(thd, Sql_condition::WARN_LEVEL_WARN,
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
  push_warning_safe(thd, Sql_condition::WARN_LEVEL_WARN,
                    err_code, ER_THD(thd, err_code), str.c_ptr_safe());
}


/**
  Returns a pointer to Opt_hints_global object,
  creates Opt_hints object if not exist.

  @param pc   pointer to Parse_context object

  @return  pointer to Opt_hints object,
           NULL if failed to create the object
*/

Opt_hints_global *get_global_hints(Parse_context *pc)
{
  LEX *lex= pc->thd->lex;

  if (!lex->opt_hints_global)
  {
    lex->opt_hints_global= new (pc->thd->mem_root)
        Opt_hints_global(pc->thd->mem_root);
  }
  return lex->opt_hints_global;
}


Opt_hints_qb *get_qb_hints(Parse_context *pc)
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
    /*
      Mark the query block as resolved as we know which SELECT_LEX it is
      attached to.
      Note that children (indexes, tables) are probably not resolved, yet.
    */
    qb->set_fixed();
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

Opt_hints_qb *find_qb_hints(Parse_context *pc,
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
               &qb_name, NULL, NULL, NULL);
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

Opt_hints_table *get_table_hints(Parse_context *pc,
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
  // Print the hints stored in the bitmap
  for (uint i= 0; i < MAX_HINT_ENUM; i++)
  {
    if (opt_hint_info[i].irregular_hint)
      continue;
    opt_hints_enum hint= static_cast<opt_hints_enum>(i);
    if (is_specified(hint) && !ignore_print(hint) && is_fixed(hint))
    {
      append_hint_type(str, hint);
      str->append(STRING_WITH_LEN("("));
      uint32 len_before_name= str->length();
      append_name(thd, str);
      uint32 len_after_name= str->length();
      if (len_after_name > len_before_name)
        str->append(' ');
      if (opt_hint_info[i].has_arguments)
        append_hint_arguments(thd, hint, str);
      if (str->length() == len_after_name + 1)
      {
        // No additional arguments were printed, trim the space added before
        str->length(len_after_name);
      }
      str->append(STRING_WITH_LEN(") "));
    }
  }

  print_irregular_hints(thd, str);

  for (uint i= 0; i < child_array.size(); i++)
    child_array[i]->print(thd, str);
}


/*
  @brief
    Append hint "type", for example, "NO_RANGE_OPTIMIZATION" or "BKA"
*/

void Opt_hints::append_hint_type(String *str, opt_hints_enum type)
{
  if(!hints_map.is_switched_on(type))
    str->append(STRING_WITH_LEN("NO_"));
  str->append(opt_hint_info[type].hint_type);
}


void Opt_hints::print_unfixed_warnings(THD *thd)
{
  String hint_name_str, hint_type_str;
  append_name(thd, &hint_name_str);

  for (uint i= 0; i < MAX_HINT_ENUM; i++)
  {
    if (is_specified(static_cast<opt_hints_enum>(i)))
    {
      hint_type_str.length(0);
      append_hint_type(&hint_type_str, static_cast<opt_hints_enum>(i));
      push_warning_safe(thd, Sql_condition::WARN_LEVEL_WARN,
                        get_unfixed_warning_code(),
                        ER_THD(thd, get_unfixed_warning_code()),
                        hint_name_str.c_ptr_safe(),
                        hint_type_str.c_ptr_safe());
    }
  }
}

/*
  @brief
    Recursively walk the descendant hints and emit warnings for any
    unresolved hints
*/

void Opt_hints::check_unfixed(THD *thd)
{
  if (!is_fixed())
    print_unfixed_warnings(thd);

  if (!are_children_fully_fixed())
  {
    for (uint i= 0; i < child_array.size(); i++)
      child_array[i]->check_unfixed(thd);
  }
}


Opt_hints_qb::Opt_hints_qb(Opt_hints *opt_hints_arg,
                           MEM_ROOT *mem_root_arg,
                           uint select_number_arg)
  : Opt_hints(Lex_ident_sys(), opt_hints_arg, mem_root_arg),
    select_number(select_number_arg),
    join_order_hints(mem_root_arg),
      join_order_hints_ignored(0)
{
  sys_name.str= buff;
  sys_name.length= my_snprintf(buff, sizeof(buff), "%s%x",
                               sys_qb_prefix.str, select_number);
}


Opt_hints_table *Opt_hints_qb::fix_hints_for_table(TABLE *table,
                                                   const Lex_ident_table &alias)
{
  Opt_hints_table *tab= static_cast<Opt_hints_table *>(find_by_name(alias));

  table->pos_in_table_list->opt_hints_qb= this;

  if (!tab)                            // Tables not found
    return NULL;

  if (!tab->fix_key_hints(table))
    incr_fully_fixed_children();

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


void Opt_hints_qb::append_hint_arguments(THD *thd, opt_hints_enum hint,
                                         String *str)
{
  switch (hint)
  {
  case SUBQUERY_HINT_ENUM:
    subquery_hint->append_args(thd, str);
    break;
  case SEMIJOIN_HINT_ENUM:
    semijoin_hint->append_args(thd, str);
    break;
  case JOIN_FIXED_ORDER_HINT_ENUM:
    join_fixed_order->append_args(thd, str);
    break;
  default:
    DBUG_ASSERT(0);
  }
}


/*
  @brief
    For each index IDX, put its hints into keyinfo_array[IDX]
*/

bool Opt_hints_table::fix_key_hints(TABLE *table)
{
  /*
    Ok, there's a table we attach to. Mark this hint as fixed and proceed to
    fixing the child objects.
  */
  set_fixed();

  if (child_array_ptr()->size() == 0)  // No key level hints
    return false; // Ok, fully fixed

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
        (*hint)->set_fixed();
        keyinfo_array[j]= static_cast<Opt_hints_key *>(*hint);
        incr_fully_fixed_children();
        set_compound_key_hint_map(*hint, j);
        break;
      }
    }
  }

  if (are_children_fully_fixed())
    return false;

  return true; // Some children are not fully fixed
}


bool Opt_hints_table::is_hint_conflicting(Opt_hints_key *key_hint,
                                          opt_hints_enum type) const
{
  if ((key_hint == nullptr) && is_specified(type))
    return true;
  return (key_hint && key_hint->is_specified(type));
}


void Opt_hints_table::set_fixed()
{
  Opt_hints::set_fixed();
  if (is_specified(INDEX_HINT_ENUM))
    global_index.set_fixed(true);
  if (is_specified(JOIN_INDEX_HINT_ENUM))
    join_index.set_fixed(true);
  if (is_specified(GROUP_INDEX_HINT_ENUM))
    group_index.set_fixed(true);
  if (is_specified(ORDER_INDEX_HINT_ENUM))
    order_index.set_fixed(true);
}


bool Opt_hints_table::is_fixed(opt_hints_enum type_arg)
{
  if (is_compound_hint(type_arg))
    return Opt_hints::is_fixed(type_arg) &&
           get_compound_key_hint(type_arg)->is_fixed();
  return Opt_hints::is_fixed(type_arg);
}


void Opt_hints_table::set_compound_key_hint_map(Opt_hints *hint, uint arg)
{
  if (hint->is_specified(INDEX_HINT_ENUM))
    global_index.set_key_map(arg);
  if (hint->is_specified(JOIN_INDEX_HINT_ENUM))
    join_index.set_key_map(arg);
  if (hint->is_specified(GROUP_INDEX_HINT_ENUM))
    group_index.set_key_map(arg);
  if (hint->is_specified(ORDER_INDEX_HINT_ENUM))
    order_index.set_key_map(arg);
}


Compound_key_hint *Opt_hints_table::get_compound_key_hint(opt_hints_enum type)
{
  switch (type) {
    case INDEX_HINT_ENUM:
      return &global_index;
    case JOIN_INDEX_HINT_ENUM:
      return &join_index;
    case GROUP_INDEX_HINT_ENUM:
      return &group_index;
    case ORDER_INDEX_HINT_ENUM:
      return &order_index;
    default:
      DBUG_ASSERT(0);
      return nullptr;
  }
}


/**
  Function updates key_to_use key map depending on index hint state.

  @param keys_to_use            key to use
  @param available_keys_to_use  available keys to use
  @param type_arg               hint type
*/

void Opt_hints_table::update_index_hint_map(Key_map *keys_to_use,
                                            Key_map *available_keys_to_use,
                                            opt_hints_enum type_arg)
{
  // Check if hint is resolved.
  if (is_fixed(type_arg))
  {
    Key_map *keys_specified_in_hint=
        get_compound_key_hint(type_arg)->get_key_map();
    if (get_switch(type_arg))
    {
      if (keys_specified_in_hint->is_clear_all())
      {
        /*
          If the hint is on and no keys are specified in the hint,
          then set "keys_to_use" to all the available keys.
        */
        keys_to_use->merge(*available_keys_to_use);
      }
      else
      {
        /*
          If the hint is on and there are keys specified in the hint, then add
          the specified keys to "keys_to_use" taking care of the disabled keys
          (available_keys_to_use).
        */
        keys_to_use->merge(*keys_specified_in_hint);
        keys_to_use->intersect(*available_keys_to_use);
      }
    }
    else
    {
      if (keys_specified_in_hint->is_clear_all())
      {
        /*
          If the hint is off and there are no keys specified in the hint, then
          we clear "keys_to_use".
        */
        keys_to_use->clear_all();
      }
      else
      {
        /*
          If hint is off and some keys are specified in the hint, then remove
          the specified keys from "keys_to_use.
        */
        keys_to_use->subtract(*keys_specified_in_hint);
      }
    }
  }
}


/**
  Function updates keys_in_use_for_query, keys_in_use_for_group_by,
  keys_in_use_for_order_by depending on INDEX, JOIN_INDEX, GROUP_INDEX,
  ORDER_INDEX hints.

  @param thd            pointer to THD object
  @param tbl            pointer to TABLE object

  @return false if no index hint is specified, true otherwise.
*/

bool Opt_hints_table::update_index_hint_maps(THD *thd, TABLE *tbl)
{
  if (!is_fixed(INDEX_HINT_ENUM) && !is_fixed(JOIN_INDEX_HINT_ENUM) &&
      !is_fixed(GROUP_INDEX_HINT_ENUM) && !is_fixed(ORDER_INDEX_HINT_ENUM))
    return false;  // No index hint is specified

  Key_map usable_index_map(tbl->s->usable_indexes(thd));
  tbl->keys_in_use_for_query= tbl->keys_in_use_for_group_by=
      tbl->keys_in_use_for_order_by= usable_index_map;

  const bool force_index= is_force_index_hint(INDEX_HINT_ENUM);
  tbl->force_index= (force_index || is_force_index_hint(JOIN_INDEX_HINT_ENUM));
  tbl->force_index_group=
      (force_index || is_force_index_hint(GROUP_INDEX_HINT_ENUM));
  tbl->force_index_order=
      (force_index || is_force_index_hint(ORDER_INDEX_HINT_ENUM));

  if (tbl->force_index || tbl->force_index_group || tbl->force_index_order)
  {
    tbl->keys_in_use_for_query.clear_all();
    tbl->keys_in_use_for_group_by.clear_all();
    tbl->keys_in_use_for_order_by.clear_all();
  }

  update_index_hint_map(&tbl->keys_in_use_for_query, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_group_by, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_order_by, &usable_index_map,
                        INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_query, &usable_index_map,
                        JOIN_INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_group_by, &usable_index_map,
                        GROUP_INDEX_HINT_ENUM);
  update_index_hint_map(&tbl->keys_in_use_for_order_by, &usable_index_map,
                        ORDER_INDEX_HINT_ENUM);
  /* Make sure "covering_keys" does not include indexes disabled with a hint */
  Key_map covering_keys(tbl->keys_in_use_for_query);
  covering_keys.merge(tbl->keys_in_use_for_group_by);
  covering_keys.merge(tbl->keys_in_use_for_order_by);
  tbl->covering_keys.intersect(covering_keys);
  return true;
}


void Opt_hints_table::append_hint_arguments(THD *thd, opt_hints_enum hint,
                                            String *str)
{
  switch (hint)
  {
    case INDEX_HINT_ENUM:
      global_index.parsed_hint->append_args(thd, str);
      break;
    case JOIN_INDEX_HINT_ENUM:
      join_index.parsed_hint->append_args(thd, str);
      break;
    case GROUP_INDEX_HINT_ENUM:
      group_index.parsed_hint->append_args(thd, str);
      break;
    case ORDER_INDEX_HINT_ENUM:
      order_index.parsed_hint->append_args(thd, str);
      break;
    default:
      DBUG_ASSERT(0);
  }
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


void append_table_name(THD *thd, String *str, const LEX_CSTRING &table_name,
                       const LEX_CSTRING &qb_name)
{
  /* Append table name */
  append_identifier(thd, str, &table_name);

  /* Append QB name */
  if (qb_name.length > 0)
  {
    str->append(STRING_WITH_LEN("@"));
    append_identifier(thd, str, &qb_name);
  }
}


void Opt_hints_qb::apply_join_order_hints(JOIN *join)
{
  if (join_fixed_order)
  {
    DBUG_ASSERT(join_order_hints.size() == 0 && !join_prefix && !join_suffix);
    // The hint is already applied at Parser::Join_order_hint::resolve()
    return;
  }
  DBUG_ASSERT(!join_fixed_order);

  // Apply hints in the same order they were specified in the query
  for (uint hint_idx= 0; hint_idx < join_order_hints.size(); hint_idx++)
  {
    Parser::Join_order_hint* hint= join_order_hints[hint_idx];
    if (join->select_options & SELECT_STRAIGHT_JOIN)
    {
      // Only mark as ignored and print the warning
      join_order_hints_ignored |= 1ULL << hint_idx;
      print_warn(join->thd, ER_WARN_CONFLICTING_HINT, hint->hint_type, true,
                 nullptr, nullptr, nullptr, hint);
    }
    else if (set_join_hint_deps(join, hint))
    {
      // Mark as ignored
      join_order_hints_ignored |= 1ULL << hint_idx;
    }
  }
}


/**
  @brief
    Resolve hint tables, check and set table dependencies according to one 
    JOIN_ORDER, JOIN_PREFIX, or JOIN_SUFFIX hint.

  @param join  pointer to JOIN object
  @param hint  The hint

  @detail
    If the hint is ignored due to circular table dependencies, original
    dependencies are restored and a warning is generated.

    == Dependencies that we add ==
    For any JOIN_HINT(t1, t2, t3, t4) we add the these dependencies:

    t2.dependent|= {t1}
    t3.dependent|= {t1,t2}
    t4.dependent|= {t1,t2,t3}
      and so forth.

    This makes sure that the listed tables occur in the join order in the order
    they are listed in the hint.

    For JOIN_ORDER, this is all what we need.
    For JOIN_PREFIX(t1, t2, ...) we also add dependencies on {t1,t2,...}
       for all tables not listed in the hint.
    For JOIN_SUFFIX(t1, t2, ...) dependencies on all tables that are NOT listed
       in the hint are added to all tables LISTED in the hint: {t1, t2, ...}

  @return false if hint is applied, true otherwise.
*/

bool Opt_hints_qb::set_join_hint_deps(JOIN *join,
                                      const Parser::Join_order_hint *hint)
{
  /*
    Make a copy of original table dependencies. If an error occurs
    when applying the hint, the original dependencies will be restored.
  */
  table_map *orig_dep_array= join->export_table_dependencies();

  // Map of the tables, specified in the hint
  table_map hint_tab_map= 0;

  for (const Parser::Table_name_and_Qb& tbl_name_and_qb : hint->table_names)
  {
    bool hint_table_found= false;
    for (uint i= 0; i < join->table_count; i++)
    {
      TABLE_LIST *table= join->join_tab[i].tab_list;
      if (!compare_table_name(&tbl_name_and_qb, table))
      {
        hint_table_found= true;
        /*
          Const tables are excluded from the process of dependency setting
          since they are always first in the table order. Note that it
          does not prevent the hint from being applied to the non-const tables
        */
        if (join->const_table_map & table->get_map())
          break;

        JOIN_TAB *join_tab= &join->join_tab[i];
        // Hint tables are always dependent on preceding tables
        join_tab->dependent |= hint_tab_map;
        update_nested_join_deps(join, join_tab, hint_tab_map);
        hint_tab_map |= join_tab->tab_list->get_map();
        break;
      }
    }

    if (!hint_table_found)
    {
      print_join_order_warn(join->thd, hint->hint_type, tbl_name_and_qb);
      join->restore_table_dependencies(orig_dep_array);
      return true;
    }
  }

  // Add dependencies that are related to non-hint tables
  for (uint i= 0; i < join->table_count; i++)
  {
    JOIN_TAB *join_tab= &join->join_tab[i];
    const table_map dependent_tables=
        get_other_dep(join, hint->hint_type, hint_tab_map,
                      join_tab->tab_list->get_map());
    update_nested_join_deps(join, join_tab, dependent_tables);
    join_tab->dependent |= dependent_tables;
  }

  if (join->propagate_dependencies(join->join_tab))
  {
    join->restore_table_dependencies(orig_dep_array);
    print_warn(join->thd, ER_WARN_CONFLICTING_HINT, hint->hint_type, true,
               nullptr, nullptr, nullptr, hint);
    return true;
  }
  return false;
}


/**
  Function updates dependencies for nested joins. If a table
  specified in the hint belongs to a nested join, we need
  to update dependencies of all tables of the nested join
  with the same dependency as for the hint table. It is also
  necessary to update all tables of the nested joins this table
  is part of.

  @param join             pointer to JOIN object
  @param hint_tab         pointer to JOIN_TAB object
  @param hint_tab_map     map of the tables, specified in the hint


  @detail
   This function is called when the caller has added a dependency:

      hint_tab now also depends on hint_tab_map.

   For example:

      FROM t0 ... LEFT JOIN ( ... t1 ... t2 ... ) ON ...

   hint_tab=t1,  hint_tab_map={t0}.

   We want to avoid the situation where the optimizer has constructed a join
   prefix with table t2 and without table t0:

    ... t2

   and now it needs to add t1 to the join prefix (it must do so, see
   add_table_function_dependencies, check_interleaving_with_nj) but it can't
   do that because t0 is not in the join prefix, and it's not possible to add
   t0 as that would break the NO-INTERLEAVING rule (see mentioned functions)

   In order to avoid this situation, we make t2 also depend t0 (that is, also
   depend on any tables outside the join nest that we've made t1 to depend on)

   Note that inside the join nest

      LEFT JOIN  ( ... t1 ... t2 ... )

   t1 and t2 may not be direct children but rather occur inside children join
   nests:

      LEFT JOIN  ( ... LEFT JOIN (...t1...) ... LEFT JOIN (...t2...) ... )
*/

void Opt_hints_qb::update_nested_join_deps(JOIN *join, const JOIN_TAB *hint_tab,
                                           table_map hint_tab_map)
{
  const TABLE_LIST *table= hint_tab->tab_list;
  if (table->embedding)
  {
    for (uint i= 0; i < join->table_count; i++)
    {
      JOIN_TAB *tab= &join->join_tab[i];
      /* Walk up the nested joins that tab->table is a part of */
      for (TABLE_LIST *emb= tab->tab_list->embedding; emb; emb=emb->embedding)
      {
        /*
          Apply the rule only for outer joins. Semi-joins do not impose such
          limitation
        */
        if (emb->on_expr)
        {
          const NESTED_JOIN *const nested_join= emb->nested_join;
          /* Is hint_tab somewhere inside this nested join, too? */
          if (hint_tab->embedding_map & nested_join->nj_map)
          {
            /*
              Yes, it is. Then, tab->table be also dependent on all outside
              tables that hint_tab is dependent on:
            */
            tab->dependent |= (hint_tab_map & ~nested_join->used_tables);
          }
        }
      }
    }
  }
}


/**
  Function returns a map of dependencies which must be applied to the
  particular table of a JOIN, according to the join order hint

  @param join          JOIN to which the hints are being applied
  @param type          hint type
  @param hint_tab_map  Bitmap of all tables listed in the hint.
  @param table_map     Bit of the table that we're setting extra dependencies
                       for.

  @detail
  This returns extra dependencies between tables listed in the Hint and tables
  that are not listed. Depending on hint type, these are:

  JOIN_PREFIX(t1, t2, ...) - all not listed tables depend on {t1,t2,...}.

  JOIN_SUFFIX(t1, t2, ...) - all tables listed in the hint depend on all tables
                             that are not listed in the hint
  JOIN_ORDER(t1, t2, ...)  - No extra dependencies needed.

  @return bitmap of dependencies to apply
*/

table_map Opt_hints_qb::
    get_other_dep(JOIN *join, opt_hints_enum type, table_map hint_tab_map,
                  table_map table_map)
{
  switch (type)
  {
    case JOIN_PREFIX_HINT_ENUM:
      if (hint_tab_map & table_map)  // Hint table: No additional dependencies
        return 0;
      // Other tables: depend on all hint tables
      return hint_tab_map;
    case JOIN_SUFFIX_HINT_ENUM:
      if (hint_tab_map & table_map)  // Hint table: depends on all other tables
        return join->all_tables_map() & ~hint_tab_map;
      return 0;
    case JOIN_ORDER_HINT_ENUM:
      return 0;  // No additional dependencies
    default:
      DBUG_ASSERT(0);
      break;
  }
  return 0;
}


/**
  Function compares hint table name and TABLE_LIST table name.
  Query block name is taken into account also.

  @param hint_table_and_qb         table/query block names given in the hint
  @param table                     pointer to TABLE_LIST object

  @return false if table names are equal, true otherwise.
*/

bool Opt_hints_qb::compare_table_name(
    const Parser::Table_name_and_Qb *hint_table_and_qb,
    const TABLE_LIST *table)
{
  const LEX_CSTRING &join_tab_qb_name=
      table->opt_hints_qb ? table->opt_hints_qb->get_name() : Lex_ident_sys();

  /*
    If QB name is not specified explicitly for a table name int hint,
    for example `JOIN_PREFIX(t2)` or `JOIN_SUFFIX(@q1 t3)` then QB name is
    considered to be equal to `Opt_hints_qb::get_name()`
  */
  const LEX_CSTRING &hint_tab_qb_name=
      hint_table_and_qb->qb_name.length > 0 ? hint_table_and_qb->qb_name :
                                              this->get_name();

  CHARSET_INFO *cs= charset_info();
  // Compare QB names
  if (cs->strnncollsp(join_tab_qb_name, hint_tab_qb_name))
      return true;
  // Compare table names
  return cs->strnncollsp(table->alias, hint_table_and_qb->table_name);
}


void Opt_hints_qb::print_irregular_hints(THD *thd, String *str)
{
  /* Print join order hints */
  for (uint i= 0; i < join_order_hints.size(); i++)
  {
    if (join_order_hints_ignored & (1ULL << i))
      continue;
    const Parser::Join_order_hint *hint= join_order_hints[i];
    str->append(opt_hint_info[hint->hint_type].hint_type);
    str->append(STRING_WITH_LEN("("));
    append_name(thd, str);
    str->append(STRING_WITH_LEN(" "));
    hint->append_args(thd, str);
    str->append(STRING_WITH_LEN(") "));
  }
}


void Opt_hints_qb::print_join_order_warn(THD *thd, opt_hints_enum type,
                                  const Parser::Table_name_and_Qb &tbl_name)
{
  String tbl_name_str, hint_type_str;
  hint_type_str.append(opt_hint_info[type].hint_type);
  append_table_name(thd, &tbl_name_str, tbl_name.table_name, tbl_name.qb_name);
  uint err_code= ER_UNRESOLVED_TABLE_HINT_NAME;

  push_warning_safe(thd, Sql_condition::WARN_LEVEL_WARN,
                    err_code, ER_THD(thd, err_code),
                    tbl_name_str.c_ptr_safe(), hint_type_str.c_ptr_safe());
}


/*
  @brief
    Fix global-level hints (and only them)
*/

bool Opt_hints_global::fix_hint(THD *thd)
{
  if (thd->lex->is_ps_or_view_context_analysis())
    return false;

  if (!max_exec_time_hint)
  {
    /* No possible errors */
    set_fixed();
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
  set_fixed();
  return false;
}


/**
  Function checks if INDEX hint is conflicting with
  already specified JOIN_INDEX, GROUP_INDEX, ORDER_INDEX
  hints.

  @param table_hint         pointer to table hint
  @param key_hint           pointer to key hint

  @return false if no conflict, true otherwise.
*/

bool Global_index_key_hint::is_hint_conflicting(Opt_hints_table *table_hint,
                                                Opt_hints_key *key_hint) const
{
  return (table_hint->is_hint_conflicting(key_hint, JOIN_INDEX_HINT_ENUM) ||
          table_hint->is_hint_conflicting(key_hint, GROUP_INDEX_HINT_ENUM) ||
          table_hint->is_hint_conflicting(key_hint, ORDER_INDEX_HINT_ENUM));
}


/**
  Function checks if JOIN_INDEX|GROUP_INDEX|ORDER_INDEX
  hint is conflicting with already specified INDEX hint.

  @param table_hint         pointer to table hint
  @param key_hint           pointer to key hint

  @return false if no conflict, true otherwise.
*/

bool Index_key_hint::is_hint_conflicting(Opt_hints_table *table_hint,
                                         Opt_hints_key *key_hint) const
{
  return table_hint->is_hint_conflicting(key_hint, INDEX_HINT_ENUM);
}


bool is_compound_hint(opt_hints_enum type_arg)
{
  return (
      type_arg == INDEX_HINT_ENUM || type_arg == JOIN_INDEX_HINT_ENUM ||
      type_arg == GROUP_INDEX_HINT_ENUM || type_arg == ORDER_INDEX_HINT_ENUM);
}


#ifndef DBUG_OFF
static char dbug_print_hint_buf[64];

const char *dbug_print_hints(Opt_hints_qb *hint)
{
  char *buf= dbug_print_hint_buf;
  THD *thd= current_thd;
  String str(buf, sizeof(dbug_print_hint_buf), &my_charset_bin);
  str.length(0);
  if (!hint)
    return "(Opt_hints_qb*)NULL";

  hint->print(thd, &str);

  if (str.c_ptr_safe() == buf)
    return buf;
  else
    return "Couldn't fit into buffer";
}
#endif
