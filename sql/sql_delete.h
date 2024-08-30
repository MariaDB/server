/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_DELETE_INCLUDED
#define SQL_DELETE_INCLUDED

#include "my_base.h"                            /* ha_rows */
#include "sql_class.h"                          /* enum_duplicates */
#include "sql_cmd.h"                            // Sql_cmd_dml
#include "sql_base.h"

class THD;
struct TABLE_LIST;
class Item;
class select_result;

typedef class Item COND;
template <typename T> class SQL_I_List;

/**
   @class Sql_cmd_delete - class used for any DELETE statements

   This class is derived from Sql_cmd_dml and contains implementations
   for abstract virtual function of the latter such as precheck() and
   prepare_inner(). It also overrides the implementation of execute_inner()
   providing a special handling for single-table delete statements that
   are not converted to multi-table delete.
   The class provides an object of the DML_prelocking_strategy class
   for the virtual function get_dml_prelocking_strategy().
*/
class Sql_cmd_delete final : public Sql_cmd_dml
{
public:
  Sql_cmd_delete(bool multitable_arg)
    : orig_multitable(multitable_arg), multitable(multitable_arg),
      save_protocol(NULL)
  {}

  enum_sql_command sql_command_code() const override
  {
    return orig_multitable ? SQLCOM_DELETE_MULTI : SQLCOM_DELETE;
  }

  DML_prelocking_strategy *get_dml_prelocking_strategy() override
  {
    return &dml_prelocking_strategy;
  }

  bool processing_as_multitable_delete_prohibited(THD *thd);

  bool is_multitable() const { return multitable; }

  void set_as_multitable() { multitable= true; }

  void remove_order_by_without_limit(THD *thd);

protected:
  /**
    @brief Perform precheck of table privileges for delete statements
  */
  bool precheck(THD *thd) override;

  /**
    @brief Perform context analysis for delete statements
  */
  bool prepare_inner(THD *thd) override;

  /**
    @brief Perform optimization and execution actions needed for deletes
  */
  bool execute_inner(THD *thd) override;

 private:
  /**
    @biefSpecial handling of single-table deletes after prepare phase
  */
  bool delete_from_single_table(THD *thd);

  /* Original value of the 'multitable' flag set by constructor */
  const bool orig_multitable;

  /*
    True if the statement is a multitable delete or converted to such.
    For a single-table delete this flag is set to true if the statement
    is supposed to be converted to multi-table delete.
  */
  bool multitable;

  /* The prelocking strategy used when opening the used tables */
  DML_prelocking_strategy dml_prelocking_strategy;

  List<Item> empty_list;   /**< auxiliary empty list used by prepare_inner() */
  Protocol *save_protocol; /**< needed for ANALYZE .. DELETE .. RETURNING */
};
#endif /* SQL_DELETE_INCLUDED */
