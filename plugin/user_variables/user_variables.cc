/* Copyright (C) 2016 MariaDB Foundation and Sergey Vojtovich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1335 USA */

#define MYSQL_SERVER
#include <sql_class.h>
#include <table.h>
#include <sql_show.h>


static const LEX_CSTRING result_types[]=
{
  { STRING_WITH_LEN("VARCHAR") },
  { STRING_WITH_LEN("DOUBLE") },
  { STRING_WITH_LEN("INT") },
  { STRING_WITH_LEN("<IMPOSSIBLE1>") }, // ROW_RESULT
  { STRING_WITH_LEN("DECIMAL") },
  { STRING_WITH_LEN("<IMPOSSIBLE2>")} // TIME_RESULT
};


static const LEX_CSTRING unsigned_result_types[]=
{
  { STRING_WITH_LEN("<IMPOSSIBLE3>") }, // UNSIGNED STRING_RESULT
  { STRING_WITH_LEN("DOUBLE UNSIGNED") },
  { STRING_WITH_LEN("INT UNSIGNED") },
  { STRING_WITH_LEN("<IMPOSSIBLE4>") }, // UNSIGNED ROW_RESULT
  { STRING_WITH_LEN("DECIMAL UNSIGNED") },
  { STRING_WITH_LEN("<IMPOSSIBLE5>") } // UNSIGNED TIME_RESULT
};


static ST_FIELD_INFO user_variables_fields_info[] =
{
  { "VARIABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, "Variable_name", 0 },
  { "VARIABLE_VALUE", 2048, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, "Value", 0 },
  { "VARIABLE_TYPE", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0, 0 },
  { "CHARACTER_SET_NAME", MY_CS_NAME_SIZE, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, 0, 0 },
  { 0, 0, MYSQL_TYPE_NULL, 0, 0, 0, 0 }
};


static int user_variables_fill(THD *thd, TABLE_LIST *tables, COND *cond)
{
  ulong i;
  TABLE *table= tables->table;
  Field **field= table->field;
  String buff;
  bool is_null;

  for (i= 0; i < thd->user_vars.records; i++)
  {
    user_var_entry *var= (user_var_entry*) my_hash_element(&thd->user_vars, i);

    field[0]->store(var->name.str, var->name.length, system_charset_info);

    if (var->val_str(&is_null, &buff, NOT_FIXED_DEC))
    {
      field[1]->store(buff.ptr(), buff.length(), buff.charset());
      field[1]->set_notnull();
    }
    else if (is_null)
      field[1]->set_null();
    else
      return 1;

    const LEX_CSTRING *tmp= var->unsigned_flag ?
                            &unsigned_result_types[var->type] :
                            &result_types[var->type];
    field[2]->store(tmp->str, tmp->length, system_charset_info);

    if (var->charset())
    {
      field[3]->store(var->charset()->csname, strlen(var->charset()->csname),
                      system_charset_info);
      field[3]->set_notnull();
    }
    else
      field[3]->set_null();

    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}


int user_variables_reset(void)
{
  THD *thd= current_thd;
  if (thd)
    my_hash_reset(&thd->user_vars);
  return 0;
}


static int user_variables_init(void *p)
{
  ST_SCHEMA_TABLE *is= (ST_SCHEMA_TABLE *) p;
  is->fields_info= user_variables_fields_info;
  is->fill_table= user_variables_fill;
  is->reset_table= user_variables_reset;
  return 0;
}


static struct st_mysql_information_schema user_variables_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


maria_declare_plugin(user_variables)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &user_variables_descriptor,
  "user_variables",
  "Sergey Vojtovich",
  "User-defined variables",
  PLUGIN_LICENSE_GPL,
  user_variables_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
