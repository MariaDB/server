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
  // hint_type                             check_upper   has_args     irregular
  {{STRING_WITH_LEN("BKA")},                   true,      false,        false},
  {{STRING_WITH_LEN("BNL")},                   true,      false,        false},
  {{STRING_WITH_LEN("ICP")},                   true,      false,        false},
  {{STRING_WITH_LEN("MRR")},                   true,      false,        false},
  {{STRING_WITH_LEN("NO_RANGE_OPTIMIZATION")}, true,      false,        false},
  {{STRING_WITH_LEN("QB_NAME")},               false,     false,        false},
  {{STRING_WITH_LEN("MAX_EXECUTION_TIME")},    false,     true,         false},
  {{STRING_WITH_LEN("SEMIJOIN")},              false,     true,         false},
  {{STRING_WITH_LEN("SUBQUERY")},              false,     true,         false},
  {{STRING_WITH_LEN("JOIN_PREFIX")},           false,     true,         true},
  {{STRING_WITH_LEN("JOIN_SUFFIX")},           false,     true,         true},
  {{STRING_WITH_LEN("JOIN_ORDER")},            false,     true,         true},
  {{STRING_WITH_LEN("JOIN_FIXED_ORDER")},      false,     true,         false},
  {{STRING_WITH_LEN("DERIVED_CONDITION_PUSHDOWN")}, false, false, false},
  {{STRING_WITH_LEN("MERGE")}, true, false, false},
  {{STRING_WITH_LEN("SPLIT_MATERIALIZED")}, false, false, false},
  {{STRING_WITH_LEN("INDEX")},                 false,     true,         false},
  {{STRING_WITH_LEN("JOIN_INDEX")},            false,     true,         false},
  {{STRING_WITH_LEN("GROUP_INDEX")},           false,     true,         false},
  {{STRING_WITH_LEN("ORDER_INDEX")},           false,     true,         false},
  {{STRING_WITH_LEN("ROWID_FILTER")},          false,     true,         false},
  {{STRING_WITH_LEN("INDEX_MERGE")}, false, false, false},
  {null_clex_str, 0, 0, 0}
};

/**
  Prefix for system generated query block name.
  Used in information warning in EXPLAIN oputput.
*/

const LEX_CSTRING sys_qb_prefix=  {"select#", 7};

/*
  Compare LEX_CSTRING objects.

  @param s     The 1st string
  @param t     The 2nd string
  @param cs    Pointer to character set

  @return  0 if strings are equal
           1 if s is greater
          -1 if t is greater
*/

int cmp_lex_string(const LEX_CSTRING &s, const LEX_CSTRING &t,
                   const CHARSET_INFO *cs)
{
  return cs->coll->strnncollsp(cs, (const uchar*)s.str, s.length,
                                   (const uchar*)t.str, t.length);
}


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

/*
  Function prepares and prints a warning message related to hints parsing or
  resolving.

  @param thd             Pointer to THD object for session.
         err_code        Enumerated code of the warning
         hint_type       Enumerated hint type
         hint_state      true: enabling hint (HINT(...),
                         false: disabling (NO_HINT(...))
         qb_name_arg     optional query block name
         table_name_arg  optional table name
         key_name_arg    optional key (index) name
         hint            optional pointer to the parsed hint object
         add_info        optional string, if given then will be appended to
                         the end of the message embraced in brackets
*/

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
   Helper function to find_qb_hints whereby it matches a qb_name to
   a select number under the presumption that qb_name has a value
   like `select#X` (where X is a select number).

   @return the matching query block hints object, if it exists.
 */

static Opt_hints_qb *find_hints_by_select_number(Parse_context *pc,
                                                 const Lex_ident_sys &qb_name)
{
  Opt_hints_qb *qb= nullptr;

  for (SELECT_LEX *sl= pc->thd->lex->all_selects_list;
       sl && !qb;  // have select and have not found matching query block hints
       sl= sl->next_select_in_list())
  {
    LEX_CSTRING sys_name;  // System QB name
    char buff[32];         // Buffer to hold sys name
    sys_name.str= buff;
    sys_name.length= snprintf(buff, sizeof(buff), "%s%u", sys_qb_prefix.str,
                              sl->select_number);

    if (cmp_lex_string(sys_name, qb_name, system_charset_info))
      continue;  // not a match, continue to next select

    // Found a matching `select#X` query block, get its attached hints.
    Parse_context sl_ctx(pc, sl);
    qb= get_qb_hints(&sl_ctx);
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

  Opt_hints_qb *qb_by_name= static_cast<Opt_hints_qb *>
    (pc->thd->lex->opt_hints_global->find_by_name(qb_name));

  Opt_hints_qb *qb_by_number= nullptr;
  if (qb_by_name == nullptr)
    qb_by_number= find_hints_by_select_number(pc, qb_name);

  // C++-style comment here, otherwise compiler warns of /* within comment.
  // We don't allow implicit query block names to be specified for hints local
  // to a view (e.g. CREATE VIEW v1 AS SELECT /*+ NO_ICP(@`select#2` t1) ...
  // because of select numbering issues.  When we're ready to fix that, then we
  // can remove this gate.
  if (pc->thd->lex->sql_command == SQLCOM_CREATE_VIEW &&
      qb_by_number)
  {
    print_warn(pc->thd, ER_WARN_NO_IMPLICIT_QB_NAMES_IN_VIEW,
               hint_type, hint_state, &qb_name,
               nullptr, nullptr, nullptr);
    return nullptr;
  }

  Opt_hints_qb *qb= qb_by_name ? qb_by_name : qb_by_number;
  if (qb == nullptr)
    print_warn(pc->thd, ER_WARN_UNKNOWN_QB_NAME, hint_type, hint_state,
               &qb_name, NULL, NULL, NULL);

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


bool Opt_hints::ignore_print(opt_hints_enum type_arg) const
{
  return opt_hint_info[type_arg].irregular_hint;
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
  if (!are_all_fixed())
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


/**
  fix_hints_for_derived_table allows early hint fixing for
  derived tables by linking both *this and the Opt_hints_table
  object to the passed TABLE_LIST instance.

  @param table_list Pointer to TABLE_LIST object
*/

void Opt_hints_qb::fix_hints_for_derived_table(TABLE_LIST *table_list)
{
  DBUG_ASSERT(table_list->is_view_or_derived());
  DBUG_ASSERT(!table_list->opt_hints_qb ||
              (table_list->opt_hints_qb == this));
  table_list->opt_hints_qb= this;

  /*
    This instance will have been marked as fixed on the basis of its
    attachment to a SELECT_LEX (during get_qb_hints).

    We mark the opt_hints_table as 'fixed' here and this means we
    won't try to fix the child hints again later.  They will remain
    unfixed and will eventually produce "Unresolved index name" error
    in opt_hints_qb->check_unfixed().  This is acceptable because
    no child hints apply to derived tables.
  */
  DBUG_ASSERT(!table_list->opt_hints_table);
  table_list->opt_hints_table=
    static_cast<Opt_hints_table *>(find_by_name(table_list->alias));
  if (!table_list->opt_hints_table)
    return;
  table_list->opt_hints_table->set_fixed();
}


Opt_hints_table *Opt_hints_qb::fix_hints_for_table(TABLE *table,
                                                   const Lex_ident_table &alias)
{
  Opt_hints_table *tab=
    static_cast<Opt_hints_table *>(find_by_name(alias));

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
        set_index_hint(*hint, j);
        break;
      }
    }
  }

  /*
    Fixing compound index hints. A compound hint is fixed in two cases:
    - it is a table-level hint, i.e. does not have a list of index names
          (like ORDER_INDEX(t1);
    - it has a list of index names, and at least one of listed
      index names is resolved successfully. So, NO_INDEX(t1 bad_idx) does not
      become a table-level hint NO_INDEX(t1) if `bad_idx` cannnot be resolved.
  */
  for (opt_hints_enum
           hint_type : { INDEX_HINT_ENUM, JOIN_INDEX_HINT_ENUM,
                         GROUP_INDEX_HINT_ENUM, ORDER_INDEX_HINT_ENUM,
                         ROWID_FILTER_HINT_ENUM })
  {
    if (is_specified(hint_type))
    {
      Opt_hints_key_bitmap *bitmap= get_key_hint_bitmap(hint_type);
      if (bitmap->is_table_level() || bitmap->bits_set() > 0)
        bitmap->set_fixed();
    }
  }

  if (are_children_fully_fixed())
    return false;

  return true; // Some children are not fully fixed
}


static bool table_or_key_hint_type_specified(Opt_hints_table *table_hint,
                                             Opt_hints_key *key_hint,
                                             opt_hints_enum type)
{
  DBUG_ASSERT(table_hint || key_hint );
  return key_hint ? key_hint->is_specified(type) :
                    table_hint->is_specified(type);
}


bool Opt_hints_table::is_fixed(opt_hints_enum type_arg)
{
  if (is_compound_hint(type_arg))
    return Opt_hints::is_fixed(type_arg) &&
           get_key_hint_bitmap(type_arg)->is_fixed();
  return Opt_hints::is_fixed(type_arg);
}


void Opt_hints_table::set_compound_key_hint_map(Opt_hints *hint, uint keynr)
{
  if (hint->is_specified(INDEX_HINT_ENUM))
    global_index_map.set_key_map(keynr);
  if (hint->is_specified(JOIN_INDEX_HINT_ENUM))
    join_index_map.set_key_map(keynr);
  if (hint->is_specified(GROUP_INDEX_HINT_ENUM))
    group_index_map.set_key_map(keynr);
  if (hint->is_specified(ORDER_INDEX_HINT_ENUM))
    order_index_map.set_key_map(keynr);
  if (hint->is_specified(ROWID_FILTER_HINT_ENUM))
    rowid_filter_map.set_key_map(keynr);
}


Opt_hints_key_bitmap *Opt_hints_table::get_key_hint_bitmap(opt_hints_enum type)
{
  switch (type) {
    case INDEX_HINT_ENUM:
      return &global_index_map;
    case JOIN_INDEX_HINT_ENUM:
      return &join_index_map;
    case GROUP_INDEX_HINT_ENUM:
      return &group_index_map;
    case ORDER_INDEX_HINT_ENUM:
      return &order_index_map;
    case ROWID_FILTER_HINT_ENUM:
      return &rowid_filter_map;
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
                                            const Key_map *available_keys_to_use,
                                            opt_hints_enum type_arg)
{
  // Check if hint is resolved.
  if (is_fixed(type_arg))
  {
    // Whitelisting hints: INDEX(), ORDER_INDEX(), etc
    Key_map *keys_specified_in_hint=
        get_key_hint_bitmap(type_arg)->get_key_map();
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
      // Blacklisting hints: NO_INDEX(), NO_JOIN_INDEX(), etc
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
          the specified keys from "keys_to_use"
        */
        keys_to_use->subtract(*keys_specified_in_hint);
      }
    }
  }
}


/**
  @brief
    Set TABLE::keys_in_use_for_XXXX and other members according to the
    specified index hints for this table

  @detail
    For each index hint that is not ignored, include the index in
    - tbl->keys_in_use_for_query if the hint is INDEX or JOIN_INDEX
    - tbl->keys_in_use_for_group_by if the hint is INDEX or
      GROUP_INDEX
    - tbl->keys_in_use_for_order_by if the hint is INDEX or
      ORDER_INDEX
    - tbl->keys_in_use_for_rowid_filter if the hint is ROWID_FILTER
    conversely, subtract the index from the corresponding
    tbl->keys_in_use_for_... map if the hint is prefixed with NO_.
  See also: TABLE_LIST::process_index_hints(), which handles similar logic
  for old-style index hints.

  @param thd            pointer to THD object
  @param tbl            pointer to TABLE object

  @return
    false if no index hint is specified
    true otherwise.
*/

bool Opt_hints_table::update_index_hint_maps(THD *thd, TABLE *tbl)
{
  if (!is_fixed(INDEX_HINT_ENUM) && !is_fixed(JOIN_INDEX_HINT_ENUM) &&
      !is_fixed(GROUP_INDEX_HINT_ENUM) && !is_fixed(ORDER_INDEX_HINT_ENUM) &&
      !is_fixed(ROWID_FILTER_HINT_ENUM))
    return false;  // No index hint is specified

  Key_map usable_index_map(tbl->s->usable_indexes(thd));
  tbl->keys_in_use_for_query= tbl->keys_in_use_for_group_by=
      tbl->keys_in_use_for_order_by= tbl->keys_in_use_for_rowid_filter=
        usable_index_map;

  bool is_global_whitelisting= is_whitelisting_index_hint(INDEX_HINT_ENUM);
  tbl->force_index_join= (is_global_whitelisting ||
                          is_whitelisting_index_hint(JOIN_INDEX_HINT_ENUM));
  tbl->force_index_group= (is_global_whitelisting ||
                           is_whitelisting_index_hint(GROUP_INDEX_HINT_ENUM));
  tbl->force_index_order= (is_global_whitelisting ||
                           is_whitelisting_index_hint(ORDER_INDEX_HINT_ENUM));

  if (tbl->force_index_join)
    tbl->keys_in_use_for_query.clear_all();
  if (tbl->force_index_group)
    tbl->keys_in_use_for_group_by.clear_all();
  if (tbl->force_index_order)
    tbl->keys_in_use_for_order_by.clear_all();
  if (is_whitelisting_index_hint(ROWID_FILTER_HINT_ENUM))
    tbl->keys_in_use_for_rowid_filter.clear_all();

  // See comment to the identical code at TABLE_LIST::process_index_hints
  tbl->force_index= (tbl->force_index_order | tbl->force_index_group |
                     tbl->force_index_join);

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
  if (is_fixed(ROWID_FILTER_HINT_ENUM))
  {
    update_index_hint_map(&tbl->keys_in_use_for_rowid_filter, &usable_index_map,
                          ROWID_FILTER_HINT_ENUM);
  }
  else
  {
    /*
      If ROWID_FILTER/NO_ROWID_FILTER hint is not specified, then keys
      for building ROWID filters are the same as for retrieving data
    */
    tbl->keys_in_use_for_rowid_filter= tbl->keys_in_use_for_query;
  }
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
      global_index_map.parsed_hint->append_args(thd, str);
      break;
    case JOIN_INDEX_HINT_ENUM:
      join_index_map.parsed_hint->append_args(thd, str);
      break;
    case GROUP_INDEX_HINT_ENUM:
      group_index_map.parsed_hint->append_args(thd, str);
      break;
    case ORDER_INDEX_HINT_ENUM:
      order_index_map.parsed_hint->append_args(thd, str);
      break;
    case ROWID_FILTER_HINT_ENUM:
      rowid_filter_map.parsed_hint->append_args(thd, str);
      break;
    default:
      DBUG_ASSERT(0);
  }
}

// See comment in header file.
void Opt_hints_table::set_index_hint(Opt_hints *hint, uint arg)
{
  hint->set_fixed();
  keyinfo_array[arg]= static_cast<Opt_hints_key *>(hint);
  incr_fully_fixed_children();

  /*
    Update the index_merge_map to note that the key
    is referenced by a [NO_]INDEX_HINT associated with
    the table.
  */
  if (hint->is_specified(INDEX_MERGE_HINT_ENUM))
    index_merge_map.set_key(arg);

  set_compound_key_hint_map(hint, arg);

  // In the future, other hint types can be managed here.
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
  In addition to indicating the state of a hint, also indicates
  if the hint is present or not.  Serves to disambiguate cases
  that the other version of hint_table_state cannot, such as
  when a hint is forcing a behavior in the optimizer that it
  would not normally do and the corresponding optimizer switch
  is enabled.

  @param thd        Current thread connection state
  @param table_list Table having the hint
  @param type_arg   The hint kind in question

  @return appropriate value from hint_state enumeration
          indicating hint enabled/disabled (if present) or
          if the hint was not present.
 */

hint_state hint_table_state(const THD *thd,
                            const TABLE_LIST *table_list,
                            opt_hints_enum type_arg)
{
  if (!table_list->opt_hints_qb)
    return hint_state::NOT_PRESENT;

  DBUG_ASSERT(!opt_hint_info[type_arg].has_arguments);

  Opt_hints *hint= table_list->opt_hints_table;
  Opt_hints *parent_hint= table_list->opt_hints_qb;

  if (hint && hint->is_specified(type_arg))
  {
    const bool hint_value= hint->get_switch(type_arg);
    return hint_value ? hint_state::ENABLED :
                        hint_state::DISABLED;
  }

  if (opt_hint_info[type_arg].check_upper_lvl &&
      parent_hint->is_specified(type_arg))
  {
    const bool hint_value= parent_hint->get_switch(type_arg);
    return hint_value ? hint_state::ENABLED :
                        hint_state::DISABLED;
  }

  return hint_state::NOT_PRESENT;
}


/*
  Inspects the table and corresponding index_merge_map to
  interpret index merge hint state.

  @param table          The table indicated by the hint
  @param keyno          The particular key for the index hint
  @param has_key_hint   [OUT] true if the hint is specified for keyno
  @param other_key_hint [OUT] true if the hint is not specified for
                        keyno but is specified for some other key on
                        the table
  @param has_table_hint [OUT] true if the hint is not specified for any
                        specific key, but is specified for the table
  @parma hint_value     [OUT] true if the hint is specified and enabled
                        or false if: (1) the hint is specified and false
                        or (2) the hint is not specified (in the case of
                        (2), has_key_hint, other_key_hint, and
                        has_table_hint will be false).
*/
static void index_merge_hint_impl(const TABLE *table,
                                  uint keyno,
                                  bool &has_key_hint,
                                  bool &other_key_hint,
                                  bool &has_table_hint,
                                  bool &hint_value)
{
  Opt_hints_table *table_hints= table->pos_in_table_list->opt_hints_table;
  has_key_hint= false;
  other_key_hint= false;
  has_table_hint= false;
  hint_value= false;

  // Parent should always be initialized
  if (!table_hints || keyno == MAX_KEY)
    return;

  const opt_hints_enum type_arg= INDEX_MERGE_HINT_ENUM;

  // Get the hint state for the specific key, if named.
  if (table_hints->keyinfo_array.size() > 0 &&
      table_hints->keyinfo_array[keyno] &&
      table_hints->keyinfo_array[keyno]->is_specified(type_arg))
  {
    has_key_hint= true;
    hint_value= table_hints->keyinfo_array[keyno]->get_switch(type_arg);
    return;
  }

  /*
    The passed keyno doesn't have the hint specified, but see if another
    has the [NO_]INDEX_MERGE hint specified.  If not, then see if the table
    as a whole has the hint specified (implying all keys are affected).
    There can't be a mix of NO_INDEX_MERGE and INDEX_MERGE hints for the
    same table, so inspecting the first other specified key is enough.
  */
  const uint other_keyno= table_hints->index_merge_map.get_first_keyno();
  if (table_hints->index_merge_map.has_key_specified() &&
      table_hints->keyinfo_array[other_keyno] &&
      table_hints->keyinfo_array[other_keyno]->is_specified(type_arg))
  {
    other_key_hint= true;
    hint_value= table_hints->keyinfo_array[other_keyno]->get_switch(type_arg);
    return;
  }

  // No specific key named, see if the table has the hint specified.
  if (table_hints->is_specified(type_arg))
  {
    has_table_hint= true;
    hint_value= table_hints->get_switch(type_arg);
  }
}


// See comment in header file.
index_merge_behavior index_merge_hint(const TABLE *table,
                                      uint keyno,
                                      bool *force_index_merge,
                                      bool *use_cheapest_index_merge)
{
  bool has_key_hint= false,
       other_has_hint= false,
       has_table_hint= false,
       hint_value= false;

  index_merge_hint_impl(table,
                        keyno,
                        has_key_hint,
                        other_has_hint,
                        has_table_hint,
                        hint_value);

  if (has_key_hint && hint_value)
  {
    // Index merge is allowed for this key, so use it.
    *force_index_merge= true;
    return index_merge_behavior::USE_KEY;
  }

  if (other_has_hint && hint_value)
    // keyno isn't the one with the hint, another key on the table has the hint.
    return index_merge_behavior::SKIP_KEY;

  if (has_key_hint && !hint_value)
    // This key is not allowed, so skip it.
    return index_merge_behavior::SKIP_KEY;

  if (other_has_hint && !hint_value)
    // Another key is disallowed by the hint, this key is allowed.
    return index_merge_behavior::USE_KEY;

  if (has_table_hint && hint_value)
  {
    // No specific keys mentioned in the hint, so all are implied for the table.
    *force_index_merge= true;
    *use_cheapest_index_merge= true;
    return index_merge_behavior::TABLE_ENABLED;
  }

  if (has_table_hint && !hint_value)
    // Merging is disabled for all keys on the table.
    return index_merge_behavior::TABLE_DISABLED;

  // No hint specified for the table.
  return index_merge_behavior::NO_HINT;
}


// See comment in header file.
index_merge_behavior index_merge_hint(const TABLE *table,
                                      uint keyno)
{
  bool force_index_merge_IGNORED= false,
       use_cheapest_index_merge_IGNORED= false;
  return index_merge_hint(table,
                          keyno,
                          &force_index_merge_IGNORED,
                          &use_cheapest_index_merge_IGNORED);
}


bool hint_key_state(const THD *thd, const TABLE *table,
                    uint keyno, opt_hints_enum type_arg,
                    bool fallback_value)
{
  Opt_hints_table *table_hints= table->pos_in_table_list->opt_hints_table;

  if (table_hints && keyno != MAX_KEY)
  {
    if (!is_compound_hint(type_arg))
    {
      // Simple index hint
      Opt_hints_key *key_hints= table_hints->keyinfo_array.size() > 0 ?
                                     table_hints->keyinfo_array[keyno] : NULL;
      bool ret_val= false;
      if (get_hint_state(key_hints, table_hints, type_arg, &ret_val))
        return ret_val;
    }
    else if (table_hints->is_fixed(type_arg))
    {
      // Compound index hint
      Key_map *keys_specified_in_hint=
          table_hints->get_key_hint_bitmap(type_arg)->get_key_map();
      if (keys_specified_in_hint->is_clear_all())
      {
        /*
          No keys are specified (i.e., it is a table-level hint).
          This means either all or no keys can be used, depending on whether
          the hint is whitelisting (INDEX, GROUP_INDEX) or blacklisting
          (NO_INDEX, NO_ORDER_INDEX)
        */
        return table_hints->get_switch(type_arg);
      }
      else
      {
        bool is_specified= keys_specified_in_hint->is_set(keyno);
        bool is_on= table_hints->get_switch(type_arg);
        return (is_on && is_specified) || (!is_on && !is_specified);
      }
    }
  }
  return fallback_value;
}


bool hint_table_state(const THD *thd, const TABLE_LIST *table_list,
                      opt_hints_enum type_arg, bool fallback_value)
{
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


bool hint_table_state(const THD *thd, const TABLE *table,
                                  opt_hints_enum type_arg,
                                  bool fallback_value)
{
  return hint_table_state(thd, table->pos_in_table_list, type_arg,
                          fallback_value);
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
      TABLE_LIST *table= join->join_tab[i].get_tab_list();
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
        hint_tab_map |= join_tab->get_tab_list()->get_map();
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
                      join_tab->get_tab_list()->get_map());
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
  const TABLE_LIST *table= hint_tab->get_tab_list();
  if (table->embedding)
  {
    for (uint i= 0; i < join->table_count; i++)
    {
      JOIN_TAB *tab= &join->join_tab[i];
      /* Walk up the nested joins that tab->table is a part of */
      for (TABLE_LIST *emb= tab->get_tab_list()->embedding; emb; emb=emb->embedding)
      {
        /*
          Apply the rule only for outer joins. Semi-joins do not impose such
          limitation
        */
        if (emb->on_expr)
        {
          const NESTED_JOIN *const nested_join= emb->nested_join;
          /* Is hint_tab somewhere inside this nested join, too? */
          if (hint_tab->embedding_map & nested_join->get_nj_map())
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
      hint_table_and_qb->qb_name.length > 0 || this->get_name().length == 0
        ? hint_table_and_qb->qb_name : this->get_name();

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
  if (thd->lex->context_analysis_only &
      (CONTEXT_ANALYSIS_ONLY_PREPARE |
       CONTEXT_ANALYSIS_ONLY_VCOL_EXPR))
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
  Function checks if an INDEX (resp. JOIN_INDEX, GROUP_INDEX or
  ORDER_INDEX) hint conflicts with any JOIN_INDEX, GROUP_INDEX or
  ORDER_INDEX (resp. INDEX) hints, by checking if any of the latter is
  already specified at table level or index level.

  @param table_hint         pointer to table hint
  @param key_hint           pointer to key hint
  @param hint_type          enumerated hint type

  @return false if no conflict, true otherwise.
*/

bool is_index_hint_conflicting(Opt_hints_table *table_hint,
                               Opt_hints_key *key_hint,
                               opt_hints_enum hint_type)
{
  if (hint_type == ROWID_FILTER_HINT_ENUM)
    return table_or_key_hint_type_specified(table_hint, key_hint,
                                            ROWID_FILTER_HINT_ENUM);
  if (hint_type != INDEX_HINT_ENUM)
    return table_or_key_hint_type_specified(table_hint, key_hint,
                                            INDEX_HINT_ENUM);
  return (table_or_key_hint_type_specified(table_hint, key_hint,
                                           JOIN_INDEX_HINT_ENUM) ||
          table_or_key_hint_type_specified(table_hint, key_hint,
                                           ORDER_INDEX_HINT_ENUM) ||
          table_or_key_hint_type_specified(table_hint, key_hint,
                                           GROUP_INDEX_HINT_ENUM));
}


bool is_compound_hint(opt_hints_enum type_arg)
{
  return (
      type_arg == INDEX_HINT_ENUM || type_arg == JOIN_INDEX_HINT_ENUM ||
      type_arg == GROUP_INDEX_HINT_ENUM || type_arg == ORDER_INDEX_HINT_ENUM ||
      type_arg == ROWID_FILTER_HINT_ENUM);
}


/*
  @brief
    Perform "Hint Resolution" for Optimizer Hints (see opt_hints.h for
    definition)

  @detail
    Hints use "Explain select numbering", so this must be called after the
    call to LEX::fix_first_select_number().

    On the other hand, this must be called before the first attempt to check
    any hint.
*/

void LEX::resolve_optimizer_hints()
{
  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);
  SCOPE_EXIT([&] () mutable {
    selects_for_hint_resolution.empty();
    if (arena)
      thd->restore_active_arena(arena, &backup);
  });

  List_iterator<SELECT_LEX> it(selects_for_hint_resolution);
  SELECT_LEX *sel;
  while ((sel= it++))
  {
    if (!sel->parsed_optimizer_hints)
      continue;
    Parse_context pc(thd, sel);
    sel->parsed_optimizer_hints->resolve(&pc);
  }
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
