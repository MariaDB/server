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
  - Interprets QB_NAME hints and assigns Opt_hints_qb objects their names.
  - Table-level hints are put into their Query Block's Opt_hints_qb object.
  - Index-level hints are put into their table's Opt_hints_table object.

  Currently, this process is done right after the parsing. It's done before
  the views are opened. This is why hints cannot control anything inside
  VIEWs, either in MariaDB or in MySQL.

  Occurrences of hints are resolved in the order their SELECTs are parsed.
  This is important as query block name declarations like

    / *+ QB_NAME(foo) * /

  must be resolved before we try to resolve any references to foo, like

    / *+ SOME_HINT(table@foo ...) * /

  On the other hand, references that use implicit query block names:

  / *+ SOME_HINT(table@`select#5`) * /

  can only be resolved when select numbers are ready. When one uses CTEs,
  proper select numbers are assigned in LEX::fix_first_select_number()
  which is called after parsing has been finished.

  Beause of that, LEX::handle_parsed_optimizer_hints_in_last_select() builds a
  todo list for resolution and LEX::resolve_optimizer_hints() does the hint
  resolution.

  Query-block level hints can be reached through SELECT_LEX::opt_hints_qb.

  == Hint "fixing" ==

  During Name Resolution, hints are attached the real objects they control:
  - table-level hints find their tables,
  - index-level hints find their indexes.

  This is done in setup_tables() which calls fix_hints_for_table() for
  each table, as a result TABLE_LIST::opt_hints_table points to the table's
  hints.

  Non-index hints may be fixed before TABLE instances are available, by
  calling fix_hints_for_derived_table and using a TABLE_LIST instance; as
  the name implies, this is the case for derived tables (and such tables
  are the only use case at this point).

  == Hint hierarchy ==

  Hints have this hierarchy, less specific to more specific:

    Opt_hints_global
      Opt_hints_qb
        Opt_hints_table
          Opt_hints_key
          Opt_hints_key_bitmap

  Some hints can be specified at a specific level (e.g. per-index) or at a
  more general level (e.g. per-table).  When checking the hint, we need
  to check for per-index commands and then maybe per-table command.

  == API for checking hints ==

  The optimizer checks hints' instructions using these calls for table/index
  level hints:
    hint_table_state()
    hint_table_state_or_fallback()
    hint_key_state()
  For query block-level hints:
    opt_hints_qb->semijoin_enabled()
    opt_hints_qb->sj_enabled_strategies()
    opt_hints_qb->apply_join_order_hints(join) - This adds extra dependencies
      between tables that ensure that the optimizer picks a join order
      prescribed by the hints.
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
#include "opt_trace.h"


struct LEX;
struct TABLE;

using Key_map = Bitmap<MAX_INDEXES>;

struct st_opt_hint_info
{
  LEX_CSTRING hint_type;  // Hint "type", like "BKA" or "MRR".
  bool check_upper_lvl;   // true if upper level hint check is needed (for hints
                          // which can be specified on more than one level).
  bool has_arguments;     // true if hint has additional arguments.
  bool irregular_hint;    // true if hint requires some special handling.
                          // Currently it's used only for join order hints
                          // since they need a special printing procedure.
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

  my_bool is_specified(opt_hints_enum type_arg) const
  {
    return hints_specified.is_set(type_arg);
  }

  void set_switch(opt_hints_enum type_arg,
                  bool switch_state_arg)
  {
    if (switch_state_arg)
      hints.set_bit(type_arg);
    else
      hints.clear_bit(type_arg);
    hints_specified.set_bit(type_arg);
  }

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

  /*
    Hints by default are NOT_FIXED.
    When hints are fixed, during hint resolution, they
    transition from NOT_FIXED to FIXED.
  */
  enum class Fixed_state { NOT_FIXED, FIXED };
private:
  /*
    Parent object. There is no parent for global level,
    for query block level parent is Opt_hints_global object,
    for table level parent is Opt_hints_qb object,
    for key level parent is Opt_hints_key object.
  */
  Opt_hints *parent;

  /* Bitmap describing switch-type (on/off) hints set at this scope */
  Opt_hints_map hints_map;

  /* Array of child objects. i.e. array of the lower level objects */
  Mem_root_array<Opt_hints *> child_array;

  /* FIXED if hint is connected to the real object (see above) */
  Fixed_state fixed;

  /*
    Number of child hints that are fully fixed, that is, fixed and
    have all their children also fully fixed.
  */
  uint n_fully_fixed_children;

public:
  Opt_hints(const Lex_ident_sys &name_arg,
            Opt_hints *parent_arg,
            MEM_ROOT *mem_root_arg)
    : name(name_arg), parent(parent_arg), child_array(mem_root_arg),
      fixed(Fixed_state::NOT_FIXED), n_fully_fixed_children(0)
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

  /* Collation for comparing the name of this hint */
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

  /*
    The caller is responsible for making sure all hints in this
    hint collection are fixed.
  */
   void set_fixed() { fixed= Fixed_state::FIXED; }

  /*
    @brief
      Check if all elements in this collection are fixed.

      Whether this means children hint collections are fixed depends on
      the collection.
  */
  bool are_all_fixed() const
  {
    return fixed == Fixed_state::FIXED;
  }

  /*
    Check if the individual hint type_arg in this collection is fixed.
  */
  virtual bool is_fixed(opt_hints_enum type_arg)
  {
    return fixed == Fixed_state::FIXED;
  }

  void incr_fully_fixed_children() { n_fully_fixed_children++; }
  Mem_root_array<Opt_hints *> *child_array_ptr() { return &child_array; }

  bool are_children_fully_fixed() const
  {
    return child_array.size() == n_fully_fixed_children;
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
    Check if there are any unfixed hint objects and
    print warnings for them.

    @param thd             Pointer to THD object
  */
  void check_unfixed(THD *thd);

  /*
    Append the name of object(table, index, etc) that hints in this collection
    are attached to.
  */
  virtual void append_name(THD *thd, String *str)= 0;

  virtual void append_hint_arguments(THD *thd, opt_hints_enum hint, String *str)
  {
    DBUG_ASSERT(0);
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
    Print warnings abount unfixed hints in this hint collection

    @param thd             Pointer to THD object
  */
  void print_unfixed_warnings(THD *thd);

protected:
  /**
    Override this function in descendants so that print_unfixed_warnings()
    prints the proper warning text for table/index level unfixed hints
  */
  virtual uint get_unfixed_warning_code() const
  {
    DBUG_ASSERT(0);
    return 0;
  }

  /**
    Function prints hints which are non-standard and don't
    fit into existing hint infrastructure.

   @param thd             pointer to THD object
   @param str             pointer to String object
 */
  virtual void print_irregular_hints(THD *thd, String *str) {}

  /**
    If ignore_print() returns true, hint is not printed
    by Opt_hints::print() function. At the moment used for
    INDEX, JOIN_INDEX, GROUP_INDEX and ORDER_INDEX hints.

    @param type_arg  hint type

    @return  true if the hint should not be printed
    in Opt_hints::print() function, false otherwise.
  */
  virtual bool ignore_print(opt_hints_enum type_arg) const;
};


/**
  Global level hints.  Currently, it's only MAX_EXECUTION_TIME.
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

  virtual void append_name(THD *thd, String *str) override
  {
    /*
      Don't write anything: global hints are not attached to anything named
      like a table/query block/etc
    */
  }

  void append_hint_arguments(THD *thd, opt_hints_enum hint,
                             String *str) override
  {
    if (hint == MAX_EXEC_TIME_HINT_ENUM)
      max_exec_time_hint->append_args(thd, str);
    else
      DBUG_ASSERT(0);
  }

  bool fix_hint(THD *thd);
};


class Opt_hints_table;

/**
  Query block level hints.  Currently, these can be:
    - QB_NAME (this is interpreted in the parser and is not explicitly
               present after that)
    - [NO_]SEMIJOIN
    - SUBQUERY
    - JOIN_PREFIX
    - JOIN_SUFFIX
    - JOIN_ORDER
    - JOIN_FIXED_ORDER
*/

class Opt_hints_qb : public Opt_hints
{
  uint select_number;     // SELECT_LEX number
  LEX_CSTRING sys_name;   // System QB name
  char buff[32];          // Buffer to hold sys name

  // Array of join order hints
  Mem_root_array<Parser::Join_order_hint *> join_order_hints;
  // Bitmap marking ignored hints
  ulonglong join_order_hints_ignored;
  // Max capacity to avoid overflowing of join_order_hints_ignored bitmap
  static const uint MAX_ALLOWED_JOIN_ORDER_HINTS=
      sizeof(join_order_hints_ignored) * 8;

public:
  Opt_hints_qb(Opt_hints *opt_hints_arg,
               MEM_ROOT *mem_root_arg,
               uint select_number_arg);

  const LEX_CSTRING get_print_name()
  {
    return name.str ? name : sys_name;
  }


  bool is_print_name_default() const
  {
    // If no name given, then it's the sys_name (by default).
    return name.str == nullptr;
  }


  void append_qb_hint(THD *thd, String *str)
  {
    if (name.str)
    {
      str->append(STRING_WITH_LEN("QB_NAME("));
      append_identifier(thd, str, &name);
      str->append(STRING_WITH_LEN(") "));
    }
  }

  virtual void append_name(THD *thd, String *str) override
  {
    /*
      If an explicit name isn't given, then the implicit
      query block name (e.g. `select#X`) will be used.  But
      we don't yet support that for VIEWs, so don't show it.
    */
    if ((thd->lex->sql_command == SQLCOM_CREATE_VIEW ||
         thd->lex->sql_command == SQLCOM_SHOW_CREATE ||
         (thd->lex->derived_tables & DERIVED_VIEW)) &&
        is_print_name_default())
      return;

    /* Append query block name. */
    str->append(STRING_WITH_LEN("@"));
    const LEX_CSTRING print_name= get_print_name();
    append_identifier(thd, str, &print_name);
  }

  void append_hint_arguments(THD *thd, opt_hints_enum hint,
                             String *str) override;

  void fix_hints_for_derived_table(TABLE_LIST *table_list);

  /**
    Function finds Opt_hints_table object corresponding to
    table alias in the query block and attaches corresponding
    key hint objects to appropriate KEY structures.

    @param table      Pointer to TABLE object
    @param alias      Table alias

    @return  pointer Opt_hints_table object if this object is found,
             NULL otherwise.
  */
  Opt_hints_table *fix_hints_for_table(TABLE *table,
                                       const Lex_ident_table &alias);

   /**
     Checks if join order hints are applicable and
     applies table dependencies if possible.

    @param join JOIN object
  */
  void apply_join_order_hints(JOIN *join);

  /**
    Returns whether semi-join is enabled for this query block

    A SEMIJOIN hint will force semi-join regardless of optimizer_switch settings.
    A NO_SEMIJOIN hint will only turn off semi-join if the variant with no
    strategies is used.
    A SUBQUERY hint will turn off semi-join.
    If there is no SEMIJOIN/SUBQUERY hint, optimizer_switch setting determines
    whether SEMIJOIN is used.

    @param thd  Pointer to THD object for session.
                Used to access optimizer_switch

    @return true if semijoin is enabled
  */
  bool semijoin_enabled(THD *thd) const;

  /**
    Returns bit mask of which semi-join strategies are enabled for this query
    block.

    @param opt_switches Bit map of strategies enabled by optimizer_switch

    @return Bit mask of strategies that are enabled
  */
  uint sj_enabled_strategies(uint opt_switches) const;

  const Parser::Semijoin_hint* semijoin_hint= nullptr;

  /**
    Bitmap of strategies listed in the SEMIJOIN/NO_SEMIJOIN hint body, e.g.
    FIRSTMATCH | LOOSESCAN
  */
  uint semijoin_strategies_map= 0;

  const Parser::Subquery_hint *subquery_hint= nullptr;
  uint subquery_strategy= SUBS_NOT_TRANSFORMED;

  /**
    Returns TRUE if the query block has at least one of the hints
    JOIN_PREFIX(), JOIN_SUFFIX(), JOIN_ORDER()  (but not JOIN_FIXED_ORDER()!)
  */
  bool has_join_order_hints() const
  {
    return join_order_hints.size() > 0;
  }

  /*
    Adds a join order hint to the array if the capacity is not exceeded.
    Returns FALSE on success,
            TRUE on failure (capacity exceeded)
  */
  bool add_join_order_hint(Parser::Join_order_hint *hint_arg)
  {
    if (join_order_hints.size() >= MAX_ALLOWED_JOIN_ORDER_HINTS)
      return true;
    join_order_hints.push_back(hint_arg);
    return false;
  }

  const Parser::Join_order_hint *join_prefix= nullptr;
  const Parser::Join_order_hint *join_suffix= nullptr;
  const Parser::Join_order_hint *join_fixed_order= nullptr;

  void trace_hints(THD *thd)
  {
    if (unlikely(thd->trace_started()))
    {
      Json_writer_object obj(thd);
      String str;
      str.length(0);
      print(thd, &str);
      // Eventually we may want to print them as array.
      obj.add("hints", str.c_ptr_safe());
    }
  }

private:
  bool set_join_hint_deps(JOIN *join, const Parser::Join_order_hint *hint);
  void update_nested_join_deps(JOIN *join, const JOIN_TAB *hint_tab,
                               table_map hint_tab_map);
  table_map get_other_dep(JOIN *join, opt_hints_enum type,
                           table_map hint_tab_map, table_map table_map);
  bool compare_table_name(const Parser::Table_name_and_Qb *hint_table_and_qb,
                          const TABLE_LIST *table);
  void print_irregular_hints(THD *thd, String *str) override;
  void print_join_order_warn(THD *thd, opt_hints_enum type,
                             const Parser::Table_name_and_Qb &tbl_name);
};


/**
  Auxiliary class for "compound" index hints: INDEX, JOIN_INDEX,
  GROUP_INDEX and ORDER_INDEX.
  It represents a bitmap of keys specified in the hint along with methods for
  handling it.

  Data structures for compound index hints are stored in two places:
  1) in a Opt_hints_key object(s) for each of the affected indexes;
  2) in a Opt_hints_key_bitmap object which stores a bitmap of all indexes
     specified in the hint.
  These `Opt_hints_key_bitmap` objects themselves are stored
  in the `Opt_hints_table` object.

  `Opt_hints_table->get_switch(hint_type)` tells whether the hint was
  "positive" or "negative".
*/

class Opt_hints_key_bitmap
{
  Key_map key_map;           // Keys specified in the hint.
  bool fixed= false;         // true if all keys of the hint are resolved

public:
  Opt_hints_key_bitmap()
  {
    key_map.clear_all();
  }

  const Parser::Index_level_hint *parsed_hint= nullptr;

  void set_fixed() { fixed= true; }
  bool is_fixed() const { return fixed; }
  bool is_table_level() const { return parsed_hint->is_table_level_hint(); }

  void set_key_map(uint i) { key_map.set_bit(i); }
  bool is_set_key_map(uint i) { return key_map.is_set(i); }
  bool is_key_map_clear_all() { return key_map.is_clear_all(); }
  uint bits_set() { return key_map.bits_set(); }
  Key_map *get_key_map() { return &key_map; }
};


/**
  Table level hints.
  Collection of hints attached to a certain table (and its indexes)
*/

class Opt_hints_table : public Opt_hints
{
public:
  /*
    keyinfo_array[IDX] has all hint prescriptions for index number IDX.
  */
  Mem_root_array<Opt_hints_key *> keyinfo_array;

  /*
    Bitmaps for "compound" index hints.
    Check get_switch(opt_hint_enum) to see if it's INDEX or NO_INDEX, etc.
  */
  Opt_hints_key_bitmap global_index_map; // INDEX(), NO_INDEX()
  Opt_hints_key_bitmap join_index_map;   // JOIN_INDEX(), NO_JOIN_INDEX()
  Opt_hints_key_bitmap group_index_map;  // GROUP_INDEX(), NO_GROUP_INDEX()
  Opt_hints_key_bitmap order_index_map;  // ORDER_INDEX(), NO_ORDER_INDEX()

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
  bool fix_key_hints(TABLE *table);

  /**
    Returns `true` if a particular hint has been attached to an object
    (table or key) and `false` otherwise

    @param type_arg  hint type
  */
  bool is_fixed(opt_hints_enum type_arg) override;

  virtual uint get_unfixed_warning_code() const override
  {
    return ER_UNRESOLVED_TABLE_HINT_NAME;
  }

  /**
    Return bitmap object corresponding to the hint type passed

    @param type  hint_type
  */
  Opt_hints_key_bitmap *get_key_hint_bitmap(opt_hints_enum type);

  void append_hint_arguments(THD *thd, opt_hints_enum hint,
                             String *str) override;


  bool update_index_hint_maps(THD *thd, TABLE *tbl);

private:
  bool is_force_index_hint(opt_hints_enum type_arg)
  {
    return (get_key_hint_bitmap(type_arg)->is_fixed() &&
            get_switch(type_arg));
  }

  void update_index_hint_map(Key_map *keys_to_use,
                             const Key_map *available_keys_to_use,
                             opt_hints_enum type_arg);
  void set_compound_key_hint_map(Opt_hints *hint, uint keynr);

};

bool is_index_hint_conflicting(Opt_hints_table *table_hint,
                               Opt_hints_key *key_hint,
                               opt_hints_enum hint_type);

bool is_compound_hint(opt_hints_enum type_arg);


/**
  Key level hints.
  A set of hints attached to a particular index.
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

  virtual uint get_unfixed_warning_code() const override
  {
    return ER_UNRESOLVED_INDEX_HINT_NAME;
  }

  /**
    Compound index hints are present at both index-level (`Opt_hints_key`) and
    table-level (`Opt_hints_table`) but must be printed only once (at the
    table level). That is why they must be skipped when printing
    `Opt_hints_key` hints
  */
  bool ignore_print(opt_hints_enum type_arg) const override
  {
    if (is_compound_hint(type_arg))
      return true;
    return Opt_hints::ignore_print(type_arg);
  }
};


enum class hint_state
{
  NOT_PRESENT,  // Hint is not specified
  ENABLED,      // Hint is specified as enabled
  DISABLED      // Hint is specified as disabled
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
  fallback value if hint is not specified.

  @param thd                Pointer to THD object
  @param table_lsit         Pointer to TABLE_LIST object
  @param type_arg           Hint type
  @param fallback_value     Value to be returned if the hint is not set

  @return table hint value if hint is specified,
          otherwise fallback value.
*/
bool hint_table_state(const THD *thd, const TABLE_LIST *table_list,
                      opt_hints_enum type_arg, bool fallback_value);

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
bool hint_table_state(const THD *thd, const TABLE *table,
                      opt_hints_enum type_arg, bool fallback_value);


/*
  Similar to above but returns hint_state enum
*/
hint_state hint_table_state(const THD *thd,
                            const TABLE_LIST *table_list,
                            opt_hints_enum type_arg);

#ifndef DBUG_OFF
const char *dbug_print_hints(Opt_hints_qb *hint);
#endif

#endif /* OPT_HINTS_INCLUDED */
