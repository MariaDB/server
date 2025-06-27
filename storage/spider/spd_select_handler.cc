#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_select.h"
#include "spd_param.h"
#include "spd_select_handler.h"

extern handlerton *spider_hton_ptr;

spider_select_handler::spider_select_handler(THD *thd, SELECT_LEX *select_lex)
  : select_handler(thd, spider_hton_ptr, select_lex)
{}

select_handler *spider_create_select_handler(THD *thd, SELECT_LEX *select_lex,
                                             SELECT_LEX_UNIT *)
{
  if (spider_param_disable_select_handler(thd))
    return NULL;
  return new spider_select_handler(thd, select_lex);
}

int spider_select_handler::init_scan()
{
  first= true;
  return 0;
}

int spider_select_handler::next_row()
{
  if (first)
    first= false;
  else
    return HA_ERR_END_OF_FILE;
  for (Field **field= table->field; *field; field++)
  {
    (*field)->set_null();
  }
  return 0;
}

int spider_select_handler::end_scan()
{
  return 0;
}
