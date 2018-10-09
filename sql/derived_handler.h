#ifndef DERIVED_HANDLER_INCLUDED
#define DERIVED_HANDLER_INCLUDED

#include "mariadb.h"
#include "sql_priv.h"

class TMP_TABLE_PARAM;

typedef class st_select_lex_unit SELECT_LEX_UNIT;

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

  TMP_TABLE_PARAM *tmp_table_param;

  SELECT_LEX_UNIT *unit;

  SELECT_LEX *select;

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
  virtual void print_error(int error, myf errflag)=0;

  void set_derived(TABLE_LIST *tbl);
};

#endif /* DERIVED_HANDLER_INCLUDED */
