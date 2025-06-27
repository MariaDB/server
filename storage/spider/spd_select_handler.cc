#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_select.h"
#include "spd_select_handler.h"

extern handlerton *spider_hton_ptr;

spider_select_handler::spider_select_handler(THD *thd, SELECT_LEX *sel_lex)
  : select_handler(thd, spider_hton_ptr, sel_lex)
{}

select_handler *spider_create_select_handler(THD *, SELECT_LEX *,
                                             SELECT_LEX_UNIT *)
{
  return NULL;
}

int spider_select_handler::init_scan()
{
  return 0;
}

int spider_select_handler::next_row()
{
  return 0;
}

int spider_select_handler::end_scan()
{
  return 0;
}
