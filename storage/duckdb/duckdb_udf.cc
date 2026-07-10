/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/*
  Function plugin: run_in_duckdb(sql_string)

  Executes a SQL query directly in DuckDB and returns the result as a string.
  Registered as a MariaDB_FUNCTION_PLUGIN alongside the storage engine plugin
  in ha_duckdb.cc's maria_declare_plugin block.
*/

#define MYSQL_SERVER
#include "mariadb.h"
#include "item.h"
#include <mysql/plugin_function.h>

#undef UNKNOWN

#include "duckdb_query.h"
#include "duckdb_manager.h"

class Item_func_run_in_duckdb : public Item_str_func
{
public:
  Item_func_run_in_duckdb(THD *thd, Item *a) : Item_str_func(thd, a) {}

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("run_in_duckdb")};
    return name;
  }

  bool fix_length_and_dec(THD *) override
  {
    collation.set(&my_charset_bin);
    max_length= 65535;
    set_maybe_null();
    return FALSE;
  }

  String *val_str(String *str) override
  {
    DBUG_ASSERT(fixed());
    String *sql_arg= args[0]->val_str(str);
    if ((null_value= args[0]->null_value))
      return NULL;

    std::string sql(sql_arg->ptr(), sql_arg->length());

    auto conn= myduck::DuckdbManager::CreateConnection();
    if (!conn)
    {
      null_value= 1;
      return NULL;
    }

    auto res= myduck::duckdb_query(*conn, sql);

    if (res->type == duckdb::QueryResultType::STREAM_RESULT)
    {
      auto &stream= res->Cast<duckdb::StreamQueryResult>();
      res= stream.Materialize();
    }

    std::string output= res->HasError() ? res->GetError() : res->ToString();

    if (str->copy(output.c_str(), output.length(), &my_charset_bin))
    {
      null_value= 1;
      return NULL;
    }

    null_value= 0;
    return str;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_run_in_duckdb>(thd, this); }
};


class Create_func_run_in_duckdb : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  { return new (thd->mem_root) Item_func_run_in_duckdb(thd, arg1); }

  static Create_func_run_in_duckdb s_singleton;
};

Create_func_run_in_duckdb Create_func_run_in_duckdb::s_singleton;

Plugin_function plugin_descriptor_function_run_in_duckdb(
    &Create_func_run_in_duckdb::s_singleton);
