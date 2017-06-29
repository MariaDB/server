#ifndef SQL_TVC_INCLUDED
#define SQL_TVC_INCLUDED
#include "sql_type.h"
#include "item.h"

typedef List<Item> List_item;
class select_result;

/**
  @class table_value_constr
  @brief Definition of a Table Value Construction(TVC)
	
  It contains a list of lists of values that this TVC contains.
*/

class table_value_constr : public Sql_alloc
{
public:
  List<List_item> lists_of_values;
  select_result *result;
  
  bool prepare(THD *thd_arg, SELECT_LEX *sl, 
	       select_result *tmp_result);
  bool exec();
};

#endif /* SQL_TVC_INCLUDED */