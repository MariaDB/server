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

#ifndef SQL_DELETE_INCLUDED
#define SQL_DELETE_INCLUDED

#include "my_base.h"                            /* ha_rows */
#include "sql_class.h"                          /* enum_duplicates */
#include "sql_cmd.h"                            // Sql_cmd_dml
#include "sql_base.h"

class THD;
struct TABLE_LIST;
class Item;
class select_result;

typedef class Item COND;
template <typename T> class SQL_I_List;

class Sql_cmd_delete final : public Sql_cmd_dml
{
public:
  Sql_cmd_delete(bool multitable_arg)
    :  multitable(multitable_arg), save_protocol(NULL) {}

  enum_sql_command sql_command_code() const override
  {
    return multitable ? SQLCOM_DELETE_MULTI : SQLCOM_DELETE;
  }

  DML_prelocking_strategy *get_dml_prelocking_strategy()
  {
    return &dml_prelocking_strategy;
  }

protected:
  bool precheck(THD *thd) override;

  bool prepare_inner(THD *thd) override;

  bool execute_inner(THD *thd) override;

 private:
  bool delete_from_single_table(THD *thd);

  bool multitable;

  DML_prelocking_strategy dml_prelocking_strategy;
  List<Item> empty_list;
  Protocol *save_protocol;
};
#endif /* SQL_DELETE_INCLUDED */
