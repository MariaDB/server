/* Copyright (c) 2017, MariaDB

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

#ifndef SQL_TVC_INCLUDED
#define SQL_TVC_INCLUDED
#include "sql_type.h"

typedef List<Item> List_item;
typedef bool (Item::*Item_processor) (void *arg);
class select_result;
class Explain_select;
class Explain_query;
class Item_func_in;
class st_select_lex_unit;
typedef class st_select_lex SELECT_LEX;
class Type_holder;

/**
  @class table_value_constr
  @brief Definition of a Table Value Construction(TVC)
	
  It contains a list of lists of values which this TVC is defined by and
  reference on SELECT where this TVC is defined.
*/
class table_value_constr : public Sql_alloc
{
public:
  List<List_item> lists_of_values;
  select_result *result;
  SELECT_LEX *select_lex;
  Type_holder *type_holders;

  enum { QEP_NOT_PRESENT_YET, QEP_AVAILABLE} have_query_plan;

  Explain_select *explain;
  ulonglong select_options;
  
  table_value_constr(List<List_item> tvc_values, SELECT_LEX *sl,
		     ulonglong select_options_arg) :
    lists_of_values(tvc_values), result(0), select_lex(sl), type_holders(0),
    have_query_plan(QEP_NOT_PRESENT_YET), explain(0),
    select_options(select_options_arg)
  { };

  ha_rows get_records() { return lists_of_values.elements; }
  
  bool prepare(THD *thd_arg, SELECT_LEX *sl, 
	       select_result *tmp_result,
	       st_select_lex_unit *unit_arg);

  bool to_be_wrapped_as_with_tail();

  int save_explain_data_intern(THD *thd_arg,
			       Explain_query *output);
  bool optimize(THD *thd_arg);
  bool exec(SELECT_LEX *sl);

  void print(THD *thd_arg, String *str, enum_query_type query_type);
  bool walk_values(Item_processor processor, bool walk_subquery, void *arg);
};

st_select_lex *wrap_tvc_with_tail(THD *thd, st_select_lex *tvc_sl);

#endif /* SQL_TVC_INCLUDED */
