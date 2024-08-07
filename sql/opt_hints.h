/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2024, MariaDB plc.

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

/*
  HintsArchitecture

  == Parsing ==
  Hints have a separate parser, see sql/opt_hint_parser.{h,cc}
  The parser is invoked separately for each occurence of

    SELECT / *+ hint_body * /  ...

  in the query. The result of parsing is saved in
  SELECT_LEX::parsed_optimizer_hints.

  == Hint "resolution" ==

  This is done using "resolve" method of parsed data structures
  This process
  - Creates interpreted hint structures: Opt_hints_global, Opt_hints_qb,
    Opt_hints_table, Opt_hints_key.
  - Interprets QB_NAME hints and assigns Query Block names.
  - Table-level hints are put into their Query Block's Opt_hints_qb object.
  - Index-level hints are put into their table's Opt_hints_table object.

  == Hint "adjustment" ==

  During Name Resolution, setup_tables() calls adjust_table_hints() for each
  table and sets TABLE_LIST::opt_hints_table to point to its Opt_hints_table.

  == Hint hierarchy ==

  Hints have this hierarchy, parent to child:

    Opt_hints_global
      Opt_hints_qb
        Opt_hints_table
          Opt_hints_key

  For some hints, one needs to check the hint's base object and its parent. For
  example, MRR can be disabled on a per-index or a per-table basis.

  == How the optimizer checks hints  ==

  The optimizer checks what hints specify using these calls:
    hint_table_state()
    hint_table_state_or_fallback()
    hint_key_state()
*/


#ifndef OPT_HINTS_INCLUDED
#define OPT_HINTS_INCLUDED

#include <functional>
#include "my_config.h"
#include "sql_alloc.h"
#include "sql_list.h"
#include "mem_root_array.h"
#include "sql_string.h"
#include "sql_bitmap.h"
#include "sql_show.h"
#include "mysqld_error.h"
#include "opt_hints_parser.h"

struct LEX;
struct TABLE;

/**
  Hint types, MAX_HINT_ENUM should be always last.
  This enum should be synchronized with opt_hint_info
  array(see opt_hints.cc).
*/
enum opt_hints_enum
{
  BKA_HINT_ENUM= 0,
  BNL_HINT_ENUM,
  ICP_HINT_ENUM,
  MRR_HINT_ENUM,
  NO_RANGE_HINT_ENUM,
  QB_NAME_HINT_ENUM,
  MAX_EXEC_TIME_HINT_ENUM,
  MAX_HINT_ENUM
};


struct st_opt_hint_info
{
  LEX_CSTRING hint_name;  // Hint name.
  bool check_upper_lvl;   // true if upper level hint check is needed (for hints
                          // which can be specified on more than one level).
  bool has_arguments;     // true if hint has additional arguments.
};

typedef Optimizer_hint_parser Parser;

/**
  Opt_hints_map contains information
  about hint state(specified or not, hint value).
*/

class Opt_hints_map : public Sql_alloc
{
  Bitmap<64> hints;           // hint state
  Bitmap<64> hints_specified; // true if hint is specified

public:
  Opt_hints_map()
  {
    hints.clear_all();
    hints_specified.clear_all();
  }

  /**
     Check if hint is specified.

     @param type_arg   hint type

     @return true if hint is specified,
             false otherwise
  */
  my_bool is_specified(opt_hints_enum type_arg) const
  {
    return hints_specified.is_set(type_arg);
  }
  /**
     Set switch value and set hint into specified state.

     @param type_arg           hint type
     @param switch_state_arg   switch value
  */
  void set_switch(opt_hints_enum type_arg,
                  bool switch_state_arg)
  {
    if (switch_state_arg)
      hints.set_bit(type_arg);
    else
      hints.clear_bit(type_arg);
    hints_specified.set_bit(type_arg);
  }
  /**
     Get switch value.

     @param type_arg    hint type

     @return switch value.
  */
  bool is_switched_on(opt_hints_enum type_arg) const
  {
    return hints.is_set(type_arg);
  }
};


class Opt_hints_key;


/**
  Opt_hints class is used as ancestor for Opt_hints_global,
  Opt_hints_qb, Opt_hints_table, Opt_hints_key classes.

  Opt_hints_global class is hierarchical structure.
  It contains information about global hints and also
  contains array of QUERY BLOCK level objects (Opt_hints_qb class).
  Each QUERY BLOCK level object contains array of TABLE level hints
  (class Opt_hints_table). Each TABLE level hint contains array of
  KEY level hints (Opt_hints_key class).
  Hint information(specified, on|off state) is stored in hints_map object.
*/

class Opt_hints : public Sql_alloc
{
protected:
  /*
    Name of object referred by the hint.
    This name is empty for global level,
    query block name for query block level,
    table name for table level and key name
    for key level.
  */
  Lex_ident_sys name;
private:
  /*
    Parent object. There is no parent for global level,
    for query block level parent is Opt_hints_global object,
    for table level parent is Opt_hints_qb object,
    for key level parent is Opt_hints_key object.
  */
  Opt_hints *parent;

  Opt_hints_map hints_map;   // Hint map

  /* Array of child objects. i.e. array of the lower level objects */
  Mem_root_array<Opt_hints*, true> child_array;
  /* true if hint is connected to the real object */
  bool resolved;
  /* Number of resolved children */
  uint resolved_children;

public:

  Opt_hints(const Lex_ident_sys &name_arg,
            Opt_hints *parent_arg,
            MEM_ROOT *mem_root_arg)
    : name(name_arg), parent(parent_arg), child_array(mem_root_arg),
      resolved(false), resolved_children(0)
  { }

  bool is_specified(opt_hints_enum type_arg) const
  {
    return hints_map.is_specified(type_arg);
  }

  /**
    Function sets switch hint state.

    @param switch_state_arg  switch hint state
    @param type_arg          hint type
    @param check_parent      true if hint can be on parent level

    @return  true if hint is already specified,
             false otherwise
  */
  bool set_switch(bool switch_state_arg,
                  opt_hints_enum type_arg,
                  bool check_parent)
  {
    if (is_specified(type_arg) ||
        (check_parent && parent->is_specified(type_arg)))
      return true;

    hints_map.set_switch(type_arg, switch_state_arg);
    return false;
  }

  /**
    Function returns switch hint state.

    @param type_arg          hint type

    @return  hint value if hint is specified,
            false otherwise
  */
  bool get_switch(opt_hints_enum type_arg) const;

  virtual CHARSET_INFO *charset_info() const
  {
    return Lex_ident_column::charset_info();
  }

  const LEX_CSTRING get_name() const
  {
    return name;
  }
  void set_name(const Lex_ident_sys &name_arg) { name= name_arg; }
  Opt_hints *get_parent() const { return parent; }
  void set_resolved() { resolved= true; }
  bool is_resolved() const { return resolved; }
  void incr_resolved_children() { resolved_children++; }
  Mem_root_array<Opt_hints*, true> *child_array_ptr() { return &child_array; }

  bool is_all_resolved() const
  {
    return child_array.size() == resolved_children;
  }

  void register_child(Opt_hints* hint_arg)
  {
    child_array.push_back(hint_arg);
  }

  /**
    Find hint among lower-level hint objects.

    @param name_arg        hint name

    @return  hint if found,
             NULL otherwise
  */
  Opt_hints *find_by_name(const LEX_CSTRING &name_arg) const;
  /**
    Print all hints except of QB_NAME hint.

    @param thd             Pointer to THD object
    @param str             Pointer to String object
  */
  void print(THD *thd, String *str);
  /**
    Check if there are any unresolved hint objects and
    print warnings for them.

    @param thd             Pointer to THD object
  */
  void check_unresolved(THD *thd);
  virtual void append_name(THD *thd, String *str)= 0;

  /**
    Get the function appending additional hint arguments to the printed string,
    if the arguments exist. For example, SEMIJOIN and SUBQUERY hints may have
    a list of strategies as additional arguments
  */
  virtual std::function<void(THD*, String*)> get_args_printer() const
  {
    return [](THD*, String*) {};
  }

  virtual ~Opt_hints() {}

private:
  /**
    Append hint type.

    @param str             Pointer to String object
    @param type            Hint type
  */
  void append_hint_type(String *str, opt_hints_enum type);
  /**
    Print warning for unresolved hint name.

    @param thd             Pointer to THD object
  */
  void print_warn_unresolved(THD *thd);

protected:
  /**
    Override this function in descendants so that print_warn_unresolved()
    prints the proper warning text for table/index level unresolved hints
  */
  virtual uint get_warn_unresolved_code() const { return 0; }
};


/**
  Global level hints.
*/

class Opt_hints_global : public Opt_hints
{
public:
  const Parser::Max_execution_time_hint *max_exec_time_hint= nullptr;

  /*
    If MAX_EXECUTION_TIME() hint was provided, this pointer is set to
    the SELECT_LEX which the hint is attached to.
    NULL if MAX_EXECUTION_TIME() hint is missing.
  */
  st_select_lex *max_exec_time_select_lex= nullptr;

  Opt_hints_global(MEM_ROOT *mem_root_arg)
    : Opt_hints(Lex_ident_sys(), NULL, mem_root_arg)
  {}

  virtual void append_name(THD *thd, String *str) override {}

  virtual std::function<void(THD*, String*)> get_args_printer() const override
  {
    using std::placeholders::_1;
    using std::placeholders::_2;
    return std::bind(&Parser::Max_execution_time_hint::append_args,
                     max_exec_time_hint, _1, _2);
  }

  bool resolve(THD *thd);
};


class Opt_hints_table;

/**
  Query block level hints.
*/

class Opt_hints_qb : public Opt_hints
{
  uint select_number;     // SELECT_LEX number
  LEX_CSTRING sys_name;   // System QB name
  char buff[32];          // Buffer to hold sys name

public:

  Opt_hints_qb(Opt_hints *opt_hints_arg,
               MEM_ROOT *mem_root_arg,
               uint select_number_arg);

  const LEX_CSTRING get_print_name()
  {
    return name.str ? name : sys_name;
  }

  /**
    Append query block hint.

    @param thd   pointer to THD object
    @param str   pointer to String object
  */
  void append_qb_hint(THD *thd, String *str)
  {
    if (name.str)
    {
      str->append(STRING_WITH_LEN("QB_NAME("));
      append_identifier(thd, str, &name);
      str->append(STRING_WITH_LEN(") "));
    }
  }
  /**
    Append query block name.

    @param thd   pointer to THD object
    @param str   pointer to String object
  */
  virtual void append_name(THD *thd, String *str) override
  {
    str->append(STRING_WITH_LEN("@"));
    const LEX_CSTRING print_name= get_print_name();
    append_identifier(thd, str, &print_name);
  }

  /**
    Function finds Opt_hints_table object corresponding to
    table alias in the query block and attaches corresponding
    key hint objects to appropriate KEY structures.

    @param table      Pointer to TABLE object
    @param alias      Table alias

    @return  pointer Opt_hints_table object if this object is found,
             NULL otherwise.
  */
  Opt_hints_table *adjust_table_hints(TABLE *table,
                                      const Lex_ident_table &alias);
};


/**
  Table level hints.
*/

class Opt_hints_table : public Opt_hints
{
public:
  Mem_root_array<Opt_hints_key*, true> keyinfo_array;

  Opt_hints_table(const Lex_ident_sys &table_name_arg,
                  Opt_hints_qb *qb_hints_arg,
                  MEM_ROOT *mem_root_arg)
    : Opt_hints(table_name_arg, qb_hints_arg, mem_root_arg),
      keyinfo_array(mem_root_arg)
  { }

  CHARSET_INFO *charset_info() const override
  {
    return Lex_ident_table::charset_info();
  }


  /**
    Append table name.

    @param thd   pointer to THD object
    @param str   pointer to String object
  */
  virtual void append_name(THD *thd, String *str) override
  {
    append_identifier(thd, str, &name);
    get_parent()->append_name(thd, str);
  }
  /**
    Function sets correlation between key hint objects and
    appropriate KEY structures.

    @param table      Pointer to TABLE object
  */
  void adjust_key_hints(TABLE *table);

  virtual uint get_warn_unresolved_code() const override
  {
    return ER_UNRESOLVED_TABLE_HINT_NAME;
  }
};


/**
  Key level hints.
*/

class Opt_hints_key : public Opt_hints
{
public:

  Opt_hints_key(const Lex_ident_sys &key_name_arg,
                Opt_hints_table *table_hints_arg,
                MEM_ROOT *mem_root_arg)
    : Opt_hints(key_name_arg, table_hints_arg, mem_root_arg)
  { }

  /**
    Append key name.

    @param thd   pointer to THD object
    @param str   pointer to String object
  */
  virtual void append_name(THD *thd, String *str) override
  {
    get_parent()->append_name(thd, str);
    str->append(' ');
    append_identifier(thd, str, &name);
  }

  virtual uint get_warn_unresolved_code() const override
  {
    return ER_UNRESOLVED_INDEX_HINT_NAME;
  }
};


/**
  Returns key hint value if hint is specified, returns
  optimizer switch value if hint is not specified.

  @param thd               Pointer to THD object
  @param tab               Pointer to TABLE object
  @param keyno             Key number
  @param type_arg          Hint type
  @param optimizer_switch  Optimizer switch flag

  @return key hint value if hint is specified,
          otherwise optimizer switch value.
*/
bool hint_key_state(const THD *thd, const TABLE *table,
                    uint keyno, opt_hints_enum type_arg,
                    uint optimizer_switch);

/**
  Returns table hint value if hint is specified, returns
  optimizer switch value if hint is not specified.

  @param thd                Pointer to THD object
  @param tab                Pointer to TABLE object
  @param type_arg           Hint type
  @param optimizer_switch   Optimizer switch flag

  @return table hint value if hint is specified,
          otherwise optimizer switch value.
*/
bool hint_table_state(const THD *thd, const TABLE *table,
                      opt_hints_enum type_arg,
                      uint optimizer_switch);

/**
  Returns table hint value if hint is specified, returns
  fallback value if hint is not specified.

  @param thd                Pointer to THD object
  @param tab                Pointer to TABLE object
  @param type_arg           Hint type
  @param fallback_value     Value to be returned if the hint is not set

  @return table hint value if hint is specified,
          otherwise fallback value.
*/
bool hint_table_state_or_fallback(const THD *thd, const TABLE *table,
                                  opt_hints_enum type_arg,
                                  bool fallback_value);
#endif /* OPT_HINTS_INCLUDED */
