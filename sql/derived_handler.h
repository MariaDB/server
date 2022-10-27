/*
   Copyright (c) 2016, 2017 MariaDB

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

#ifndef DERIVED_HANDLER_INCLUDED
#define DERIVED_HANDLER_INCLUDED

#include "mariadb.h"
#include "sql_priv.h"

class TMP_TABLE_PARAM;

typedef class st_select_lex_unit SELECT_LEX_UNIT;

/**
  @class derived_handler

  This interface class is to be used for execution of queries that specify
  derived table by foreign engines
*/

class derived_handler
{
public:
  THD *thd;
  handlerton *ht;

  TABLE_LIST *derived;

  /*
    Temporary table where all results should be stored in record[0]
    The table has a field for every item from the select list of
    the specification of derived.
  */
  TABLE *table;

  /* The parameters if the temporary table used at its creation */
  TMP_TABLE_PARAM *tmp_table_param;

  SELECT_LEX_UNIT *unit;   // Specifies the derived table

  SELECT_LEX *select;      // The first select of the specification

  derived_handler(THD *thd_arg, handlerton *ht_arg)
    : thd(thd_arg), ht(ht_arg), derived(0),table(0), tmp_table_param(0),
    unit(0), select(0) {}
  virtual ~derived_handler() {}

  /*
    Functions to scan data. All these returns 0 if ok, error code in case
    of error
  */

  /* Initialize the process of producing rows of the derived table */
  virtual int init_scan()= 0;

  /*
    Put the next produced row of the derived in table->record[0] and return 0.
    Return HA_ERR_END_OF_FILE if there are no more rows, return other error
    number in case of fatal error.
   */
  virtual int next_row()= 0;

  /* End prodicing rows */
  virtual int end_scan()=0;

  /* Report errors */
  virtual void print_error(int error, myf errflag);

  void set_derived(TABLE_LIST *tbl);
};

#endif /* DERIVED_HANDLER_INCLUDED */
