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

/**
  @class select_handler

  This interface class is to be used for execution of select queries
  by foreign engines
*/

class select_handler
{
 public:
  THD *thd;
  handlerton *ht;

  SELECT_LEX *select;  // Select to be excuted

  /*
    Temporary table where all results should be stored in record[0]
    The table has a field for every item from the select_lex::item_list.
    The table is actually never filled. Only its record buffer is used.
  */
  TABLE *table;
  List<Item> result_columns;

  bool is_analyze;

  bool send_result_set_metadata();
  bool send_data();

  select_handler(THD *thd_arg, handlerton *ht_arg);

  virtual ~select_handler();

  int execute();

  virtual bool prepare();

  static TABLE *create_tmp_table(THD *thd, SELECT_LEX *sel);

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

  bool send_eof();
};

#endif /* SELECT_HANDLER_INCLUDED */
