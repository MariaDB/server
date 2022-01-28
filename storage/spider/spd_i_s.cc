/* Copyright (C) 2012-2020 Kentoku Shiba
   Copyright (C) 2020 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_show.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_table.h"

extern pthread_mutex_t spider_mem_calc_mutex;

extern const char *spider_alloc_func_name[SPIDER_MEM_CALC_LIST_NUM];
extern const char *spider_alloc_file_name[SPIDER_MEM_CALC_LIST_NUM];
extern ulong      spider_alloc_line_no[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_total_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
extern longlong   spider_current_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_alloc_mem_count[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_free_mem_count[SPIDER_MEM_CALC_LIST_NUM];

static struct st_mysql_storage_engine spider_i_s_info =
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

namespace Show {
#ifdef SPIDER_I_S_USE_SHOW_FOR_COLUMN
static ST_FIELD_INFO spider_i_s_alloc_mem_fields_info[] =
{
  Column("ID",                ULong(10),     NOT_NULL, "id"),
  Column("FUNC_NAME",         Varchar(64),   NULLABLE, "func_name"),
  Column("FILE_NAME",         Varchar(64),   NULLABLE, "file_name"),
  Column("LINE_NO",           ULong(10),     NULLABLE, "line_no"),
  Column("TOTAL_ALLOC_MEM",   ULonglong(20), NULLABLE, "total_alloc_mem"),
  Column("CURRENT_ALLOC_MEM", SLonglong(20), NULLABLE, "current_alloc_mem"),
  Column("ALLOC_MEM_COUNT",   ULonglong(20), NULLABLE, "alloc_mem_count"),
  Column("FREE_MEM_COUNT",    ULonglong(20), NULLABLE, "free_mem_count"),
  CEnd()
};
#else
static ST_FIELD_INFO spider_i_s_alloc_mem_fields_info[] =
{
  {"ID", 10, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED, "id", SKIP_OPEN_TABLE},
  {"FUNC_NAME", 64, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "func_name", SKIP_OPEN_TABLE},
  {"FILE_NAME", 64, MYSQL_TYPE_STRING, 0,
    MY_I_S_MAYBE_NULL, "file_name", SKIP_OPEN_TABLE},
  {"LINE_NO", 10, MYSQL_TYPE_LONG, 0,
    MY_I_S_UNSIGNED | MY_I_S_MAYBE_NULL, "line_no", SKIP_OPEN_TABLE},
  {"TOTAL_ALLOC_MEM", 20, MYSQL_TYPE_LONGLONG, 0,
    MY_I_S_UNSIGNED | MY_I_S_MAYBE_NULL, "total_alloc_mem", SKIP_OPEN_TABLE},
  {"CURRENT_ALLOC_MEM", 20, MYSQL_TYPE_LONGLONG, 0,
    MY_I_S_MAYBE_NULL, "current_alloc_mem", SKIP_OPEN_TABLE},
  {"ALLOC_MEM_COUNT", 20, MYSQL_TYPE_LONGLONG, 0,
    MY_I_S_UNSIGNED | MY_I_S_MAYBE_NULL, "alloc_mem_count", SKIP_OPEN_TABLE},
  {"FREE_MEM_COUNT", 20, MYSQL_TYPE_LONGLONG, 0,
    MY_I_S_UNSIGNED | MY_I_S_MAYBE_NULL, "free_mem_count", SKIP_OPEN_TABLE},
  {NULL, 0,  MYSQL_TYPE_STRING, 0, 0, NULL, 0}
};
#endif
} // namespace Show

static int spider_i_s_alloc_mem_fill_table(
  THD *thd,
  TABLE_LIST *tables,
  COND *cond
) {
  uint roop_count;
  TABLE *table = tables->table;
  DBUG_ENTER("spider_i_s_alloc_mem_fill_table");
  for (roop_count = 0; roop_count < SPIDER_MEM_CALC_LIST_NUM; roop_count++)
  {
    table->field[0]->store(roop_count, TRUE);
    if (spider_alloc_func_name[roop_count])
    {
      table->field[1]->set_notnull();
      table->field[2]->set_notnull();
      table->field[3]->set_notnull();
      table->field[4]->set_notnull();
      table->field[5]->set_notnull();
      table->field[6]->set_notnull();
      table->field[7]->set_notnull();
      table->field[1]->store(spider_alloc_func_name[roop_count],
        strlen(spider_alloc_func_name[roop_count]), system_charset_info);
      table->field[2]->store(spider_alloc_file_name[roop_count],
        strlen(spider_alloc_file_name[roop_count]), system_charset_info);
      table->field[3]->store(spider_alloc_line_no[roop_count], TRUE);
      pthread_mutex_lock(&spider_mem_calc_mutex);
      table->field[4]->store(spider_total_alloc_mem[roop_count], TRUE);
      table->field[5]->store(spider_current_alloc_mem[roop_count], FALSE);
      table->field[6]->store(spider_alloc_mem_count[roop_count], TRUE);
      table->field[7]->store(spider_free_mem_count[roop_count], TRUE);
      pthread_mutex_unlock(&spider_mem_calc_mutex);
    } else {
      table->field[1]->set_null();
      table->field[2]->set_null();
      table->field[3]->set_null();
      table->field[4]->set_null();
      table->field[5]->set_null();
      table->field[6]->set_null();
      table->field[7]->set_null();
    }
    if (schema_table_store_record(thd, table))
    {
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

static int spider_i_s_alloc_mem_init(
  void *p
) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
  DBUG_ENTER("spider_i_s_alloc_mem_init");
  schema->fields_info = Show::spider_i_s_alloc_mem_fields_info;
  schema->fill_table = spider_i_s_alloc_mem_fill_table;
  schema->idx_field1 = 0;
  DBUG_RETURN(0);
}

static int spider_i_s_alloc_mem_deinit(
  void *p
) {
  DBUG_ENTER("spider_i_s_alloc_mem_deinit");
  DBUG_RETURN(0);
}

struct st_mysql_plugin spider_i_s_alloc_mem =
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &spider_i_s_info,
  "SPIDER_ALLOC_MEM",
  "Kentoku Shiba",
  "Spider memory allocating viewer",
  PLUGIN_LICENSE_GPL,
  spider_i_s_alloc_mem_init,
  spider_i_s_alloc_mem_deinit,
  0x0001,
  NULL,
  NULL,
  NULL,
  0,
};

struct st_maria_plugin spider_i_s_alloc_mem_maria =
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &spider_i_s_info,
  "SPIDER_ALLOC_MEM",
  "Kentoku Shiba",
  "Spider memory allocating viewer",
  PLUGIN_LICENSE_GPL,
  spider_i_s_alloc_mem_init,
  spider_i_s_alloc_mem_deinit,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE,
};

extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

namespace Show {
#ifdef SPIDER_I_S_USE_SHOW_FOR_COLUMN
static ST_FIELD_INFO spider_i_s_wrapper_protocols_fields_info[] =
{
  Column("WRAPPER_NAME",        Varchar(NAME_CHAR_LEN), NOT_NULL, ""),
  Column("WRAPPER_VERSION",     Varchar(20),            NOT_NULL, ""),
  Column("WRAPPER_DESCRIPTION", Longtext(65535),        NULLABLE, ""),
  Column("WRAPPER_MATURITY",    Varchar(12),            NOT_NULL, ""),
  CEnd()
};
#else
static ST_FIELD_INFO spider_i_s_wrapper_protocols_fields_info[] =
{
  {"WRAPPER_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"WRAPPER_VERSION", 20, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {"WRAPPER_DESCRIPTION", 65535, MYSQL_TYPE_STRING, 0, 1, 0, SKIP_OPEN_TABLE},
  {"WRAPPER_MATURITY", 12, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE},
  {0, 0,  MYSQL_TYPE_STRING, 0, 0, 0, 0}
};
#endif
} // namespace Show

static int spider_i_s_wrapper_protocols_fill_table(
  THD *thd,
  TABLE_LIST *tables,
  COND *cond
) {
  uint roop_count;
  SPIDER_DBTON *dbton;
  TABLE *table = tables->table;
  DBUG_ENTER("spider_i_s_wrapper_protocols_fill_table");
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    dbton = &spider_dbton[roop_count];
    if (!dbton->wrapper)
    {
      continue;
    }
    table->field[0]->store(dbton->wrapper,
      strlen(dbton->wrapper), system_charset_info);
    table->field[1]->store(dbton->version_info,
      strlen(dbton->version_info), system_charset_info);
    if (dbton->descr)
    {
      table->field[2]->set_notnull();
      table->field[2]->store(dbton->descr,
        strlen(dbton->descr), system_charset_info);
    } else {
      table->field[2]->set_null();
    }
    if (dbton->maturity <= SPIDER_MATURITY_STABLE)
    {
      table->field[3]->store(maturity_name[dbton->maturity].str,
        maturity_name[dbton->maturity].length, system_charset_info);
    } else {
      table->field[3]->store(maturity_name[0].str,
        maturity_name[0].length, system_charset_info);
    }
    if (schema_table_store_record(thd, table))
    {
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

static int spider_i_s_wrapper_protocols_init(
  void *p
) {
  ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *) p;
  DBUG_ENTER("spider_i_s_wrapper_protocols_init");
  schema->fields_info = Show::spider_i_s_wrapper_protocols_fields_info;
  schema->fill_table = spider_i_s_wrapper_protocols_fill_table;
  schema->idx_field1 = 0;
  DBUG_RETURN(0);
}

static int spider_i_s_wrapper_protocols_deinit(
  void *p
) {
  DBUG_ENTER("spider_i_s_wrapper_protocols_deinit");
  DBUG_RETURN(0);
}

struct st_mysql_plugin spider_i_s_wrapper_protocols =
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &spider_i_s_info,
  "SPIDER_WRAPPER_PROTOCOLS",
  "Kentoku Shiba, MariaDB Corp",
  "Available wrapper protocols of Spider",
  PLUGIN_LICENSE_GPL,
  spider_i_s_wrapper_protocols_init,
  spider_i_s_wrapper_protocols_deinit,
  0x0001,
  NULL,
  NULL,
  NULL,
  0,
};

struct st_maria_plugin spider_i_s_wrapper_protocols_maria =
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &spider_i_s_info,
  "SPIDER_WRAPPER_PROTOCOLS",
  "Kentoku Shiba, MariaDB Corp",
  "Available wrapper protocols of Spider",
  PLUGIN_LICENSE_GPL,
  spider_i_s_wrapper_protocols_init,
  spider_i_s_wrapper_protocols_deinit,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE,
};
