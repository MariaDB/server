#ifndef SELECT_HANDLER_INCLUDED
#define SELECT_HANDLER_INCLUDED

#include "mariadb.h"
#include "sql_priv.h"

class select_handler
{
 public:
  THD *thd;
  handlerton *ht;

  SELECT_LEX *select;

  /*
    Temporary table where all results should be stored in record[0]
    The table has a field for every item from the select_lex::item_list.
  */
  TABLE *table;

  select_handler(THD *thd_arg, handlerton *ht_arg)
    : thd(thd_arg), ht(ht_arg), table(0) {}
  
  virtual ~select_handler() {} 
 
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
  virtual void print_error(int error, myf errflag) = 0;
};

#endif /* SELECT_HANDLER_INCLUDED */
