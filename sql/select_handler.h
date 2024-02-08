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
  @class select_handler

  This interface class is to be used for execution of select queries
  by foreign engines
*/

class select_handler
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

  virtual ~select_handler();

  int execute();

  virtual bool prepare();

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
  SELECT_LEX *select_lex;      // Single select/part of a unit to be executed
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

  /* Report errors */
  virtual void print_error(int error, myf errflag);

  bool send_result_set_metadata();
  bool send_data();
  bool send_eof();

  TABLE *create_tmp_table(THD *thd);

  select_pushdown_type get_pushdown_type();

  THD *thd;
  handlerton *ht;

  select_result *result;        // Object receiving the retrieved data
  List<Item> result_columns;

  bool is_analyze;
};

#endif /* SELECT_HANDLER_INCLUDED */
