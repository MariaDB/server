/*
   Copyright (c) 2018, 2019 MariaDB

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

#ifndef SELECT_HANDLER_INCLUDED
#define SELECT_HANDLER_INCLUDED

#include "mariadb.h"
#include "sql_priv.h"

enum class select_pushdown_type {
  SINGLE_SELECT,
  PART_OF_UNIT,
  WHOLE_UNIT
};


/**
  @class pushdown_handler

  A statement that a foreign engine has taken over from the SQL layer.

  The SQL layer builds no query plan for such a statement: the optimizer
  stops after prepare() and JOIN::exec_inner() calls execute() instead of
  running the join. What "execute" means depends on the kind of statement,
  which is what the two derived classes stand for:

  - select_handler       the engine produces a result set, the SQL layer
                         retrieves the rows and sends them to the client
  - multi_upddel_handler the engine performs a whole multi-table
                         UPDATE/DELETE and only reports the row counts

  An engine provides them through the handlerton::create_select /
  create_unit / create_multi_upddel call-back functions.
*/

class pushdown_handler
{
public:
  pushdown_handler(THD *thd_arg, handlerton *ht_arg, SELECT_LEX *sel_lex,
                   select_result *result_arg);

  virtual ~pushdown_handler() = default;

  /*
    Get ready for the execution. Called instead of the optimization of
    the statement. Returns true in case of error.
  */
  virtual bool prepare() { return false; }

  /*
    Execute the statement.

    @retval  0   ok
    @retval -1   error, it has been sent to the client already
  */
  virtual int execute() = 0;

  /*
    The name this statement is reported with in the select_type column of
    EXPLAIN, one of the pushed_*_text constants of sql_explain.h
  */
  virtual const char *explain_type() const = 0;

  /* Single select to be executed. NULL for a whole unit, see select_handler */
  SELECT_LEX *select_lex;

protected:

  /* Report errors */
  virtual void print_error(int error, myf errflag);

  bool send_eof();

  THD *thd;
  handlerton *ht;

  select_result *result;        // Object receiving the retrieved data

  bool is_analyze;
};


/**
  @class select_handler

  This interface class is to be used for execution of select queries
  by foreign engines
*/

class select_handler : public pushdown_handler
{
 public:
   // Constructor for a single SELECT_LEX (not a part of a unit)
  select_handler(THD *thd_arg, handlerton *ht_arg, SELECT_LEX *sel_lex);

  // Constructor for a unit (UNION/EXCEPT/INTERSECT)
  select_handler(THD *thd_arg, handlerton *ht_arg, SELECT_LEX_UNIT *sel_unit);

  /*
    Constructor for a SELECT_LEX which is a part of a unit
    (partial pushdown). Both SELECT_LEX and SELECT_LEX_UNIT are passed
  */
  select_handler(THD *thd_arg, handlerton *ht_arg, SELECT_LEX *sel_lex,
                 SELECT_LEX_UNIT *sel_unit);

  ~select_handler() override;

  int execute() override;

  bool prepare() override;

  const char *explain_type() const override;

  /*
    Select_handler processes these cases:
    - single SELECT
    - whole unit (multiple SELECTs combined with UNION/EXCEPT/INTERSECT)
    - single SELECT that is part of a unit (partial pushdown)

    In the case of single SELECT select_lex is initialized and lex_unit==NULL,
    in the case of whole UNIT select_lex == NULL and lex_unit is initialized,
    in the case of partial pushdown both select_lex and lex_unit
      are initialized
  */
  SELECT_LEX_UNIT *lex_unit;   // Unit to be executed

  /*
    Temporary table where all results should be stored in record[0]
    The table has a field for every item from the select_lex::item_list.
    The table is actually never filled. Only its record buffer is used.
  */
  TABLE *table;

protected:

  /*
    Functions to scan the select result set.
    All these returns 0 if ok, error code in case of error.
  */

  /* Initialize the process of producing rows of result set */
  virtual int init_scan() = 0;

  /*
    Put the next produced row of the result set in table->record[0]
    and return 0. Return HA_ERR_END_OF_FILE if there are no more rows,
    return other error number in case of fatal error.
  */
  virtual int next_row() = 0;

  /* Finish scanning */
  virtual int end_scan() = 0;

  bool send_result_set_metadata();
  bool send_data();

  TABLE *create_tmp_table(THD *thd);

  select_pushdown_type get_pushdown_type();

  List<Item> result_columns;
};


/**
  @class multi_upddel_handler

  This interface class is to be used for execution of multi-table UPDATE
  and DELETE statements by foreign engines.

  A multi-table UPDATE/DELETE has no "primary" ha_something object to call
  handler::direct_update_rows_init()/direct_delete_rows_init() on, so the
  whole statement is handed over to the engine at once, and the engine
  reports only how many rows it has matched and changed.
*/

class multi_upddel_handler : public pushdown_handler
{
public:
  multi_upddel_handler(THD *thd_arg, handlerton *ht_arg, SELECT_LEX *sel_lex);

  int execute() override;

  const char *explain_type() const override;

protected:

  /*
    Perform the whole statement and report how many rows matched the WHERE
    clause (*found_rows) and how many rows were really changed or deleted
    (*affected_rows).

    This is called from multi_upddel_handler::execute().

    @retval 0        ok
    @retval != 0     error code
  */
  virtual int batch_update_delete(ha_rows *found_rows,
                                  ha_rows *affected_rows) = 0;
};


/*
  Look through the tables of a statement for an engine that offers to take
  the statement over. Defined in select_handler.cc together with the classes
  they return.
*/
select_handler *find_single_select_handler(THD *thd, SELECT_LEX *select_lex);

select_handler *find_partial_select_handler(THD *thd, SELECT_LEX *select_lex,
                                            SELECT_LEX_UNIT *select_lex_unit);

multi_upddel_handler *find_multi_upddel_handler(THD *thd,
                                                SELECT_LEX *select_lex);

#endif /* SELECT_HANDLER_INCLUDED */
