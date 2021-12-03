/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_UPDATE_INCLUDED
#define SQL_UPDATE_INCLUDED

#include "sql_class.h"                          /* enum_duplicates */
#include "sql_cmd.h"                            // Sql_cmd_dml
#include "sql_base.h"

class Item;
struct TABLE_LIST;
class THD;

typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;

bool mysql_prepare_update(THD *thd, TABLE_LIST *table_list,
                          Item **conds, uint order_num, ORDER *order);
bool check_unique_table(THD *thd, TABLE_LIST *table_list);
int mysql_update(THD *thd,TABLE_LIST *tables,List<Item> &fields,
		 List<Item> &values,COND *conds,
		 uint order_num, ORDER *order, ha_rows limit,
                 bool ignore, ha_rows *found_return, ha_rows *updated_return);
bool mysql_multi_update(THD *thd, TABLE_LIST *table_list,
                        List<Item> *fields, List<Item> *values,
                        COND *conds, ulonglong options,
                        enum enum_duplicates handle_duplicates, bool ignore,
                        SELECT_LEX_UNIT *unit, SELECT_LEX *select_lex,
                        multi_update **result);
bool records_are_comparable(const TABLE *table);
bool compare_record(const TABLE *table);


class Sql_cmd_update final : public Sql_cmd_dml
{
public:
  Sql_cmd_update(bool multitable_arg)
      : multitable(multitable_arg)
  { }

  enum_sql_command sql_command_code() const override
  {
    return multitable ? SQLCOM_UPDATE_MULTI : SQLCOM_UPDATE;
  }

  DML_prelocking_strategy *get_dml_prelocking_strategy()
  {
    return &multiupdate_prelocking_strategy;
  }

protected:
  bool precheck(THD *thd) override;

  bool prepare_inner(THD *thd) override;

  bool execute_inner(THD *thd) override;

private:
  bool update_single_table(THD *thd);

  bool multitable;

  DML_prelocking_strategy dml_prelocking_strategy;
  Multiupdate_prelocking_strategy multiupdate_prelocking_strategy;

 public:
  List<Item> *update_value_list;

};

#endif /* SQL_UPDATE_INCLUDED */
