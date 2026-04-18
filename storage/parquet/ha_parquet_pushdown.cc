#define MYSQL_SERVER 1

#include "ha_parquet_pushdown.h"

select_handler *create_duckdb_select_handler(THD *thd,
                                             SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit)
{
  (void) thd;
  (void) sel_lex;
  (void) sel_unit;

  /*
    Stage 1 pushdown is not implemented in this PR. Returning nullptr keeps
    MariaDB on the normal execution path.
  */
  return nullptr;
}
