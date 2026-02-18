/*
   Copyright (c) 2019, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>
#include <sql_prepare.h>


// DBMS_SQL_BIND_PARAM_BY_NAME(ps_name, param_name, value)
class Item_func_dbms_sql_bind_param_by_name: public Item_bool_func
{
public:
  using Item_bool_func::Item_bool_func;
  bool val_bool() override
  {
    THD *thd= current_thd;

    StringBuffer<64> ps_name_buffer;
    String *ps_name= args[0]->val_str_ascii(&ps_name_buffer); // TODO: utf8mb3
    if ((null_value= ps_name == nullptr))
      return 0;

    StringBuffer<64> param_name_buffer;
    String *param_name= args[1]->val_str_ascii(&param_name_buffer);//TODO:utf8mb3
    if ((null_value= param_name == nullptr))
      return 0;

    bool rc= mysql_sql_stmt_set_placeholder_by_name(thd,
               Lex_ident_sys(ps_name->ptr(), ps_name->length()),
               Lex_ident_column(param_name->ptr(), param_name->length()),
               args[2]);

    null_value= rc;
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_bind_param_by_name"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_bind_param_by_name>(thd, this);
  }

  class Create_func : public Create_func_arg3
  {
  public:
    using Create_func_arg3::Create_func_arg3;
    Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3) override
    {
      return new (thd->mem_root)
        Item_func_dbms_sql_bind_param_by_name(thd, arg1, arg2, arg3);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


// DBMS_SQL_COLUMN_VALUE(cursor, position, destination)
class Item_func_dbms_sql_column_value: public Item_bool_func
{
  constexpr const char *uppercase_name() const
  {
    return "DBMS_SQL_COLUMN_VALUE";
  }
public:
  using Item_bool_func::Item_bool_func;
  bool check_arguments() const override
  {
    if (Item_bool_func::check_arguments())
      return true;
    Settable_routine_parameter *dst= args[2]->get_settable_routine_parameter();
    if (!dst)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), uppercase_name());
      return true;
    }
    return false;
  }
  bool val_bool() override
  {
    THD *thd= current_thd;
    Longlong_hybrid cursor_idx= args[0]->to_longlong_hybrid();

    if ((null_value= args[0]->null_value ||
         cursor_idx.neg() ||
         thd->statement_cursors()->elements() <= (size_t) cursor_idx.value() ||
         !thd->statement_cursors()->at(cursor_idx.value()).is_open()))
    {
      my_error(ER_SP_CURSOR_MISMATCH, MYF(0), args[0]->null_value ? "NULL" :
                                              ErrConvInteger(cursor_idx).ptr());
      return true;
    }
    sp_cursor_array_element *cursor= &thd->statement_cursors()->
                                       at(cursor_idx.value());

    longlong pos= args[1]->val_int();
    if ((null_value= args[1]->null_value || pos < 1 || pos > cursor->cols()))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), uppercase_name());
      return false;
    }

    Settable_routine_parameter *dst= args[2]->get_settable_routine_parameter();
    DBUG_ASSERT(dst);
    null_value= cursor->column_value(thd, (uint) pos - 1, dst);
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= "dbms_sql_column_value"_LEX_CSTRING;
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_dbms_sql_column_value>(thd, this);
  }

  class Create_func : public Create_func_arg3
  {
  public:
    using Create_func_arg3::Create_func_arg3;
    Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3) override
    {
      return new (thd->mem_root)
        Item_func_dbms_sql_column_value(thd, arg1, arg2, arg3);
    }
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};


/*************************************************************************/

maria_declare_plugin(pkg_dbms_sql)
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_dbms_sql_bind_param_by_name::plugin_descriptor(),
  "dbms_sql_bind_param_by_name",// plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_BIND_PARAM_BY_NAME()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
},
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type
  Item_func_dbms_sql_column_value::plugin_descriptor(),
  "dbms_sql_column_value",      // plugin name
  "MariaDB Corporation",        // plugin author
  "Function DBMS_SQL_COLUMN_VALUE()", // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity
}
maria_declare_plugin_end;
