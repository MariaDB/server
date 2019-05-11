/* Copyright (C) 2012-2014 Kentoku Shiba

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
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_show.h"
#endif
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
  schema->fields_info = spider_i_s_alloc_mem_fields_info;
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
#if MYSQL_VERSION_ID >= 50600
  0,
#endif
};

#ifdef MARIADB_BASE_VERSION
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
  MariaDB_PLUGIN_MATURITY_GAMMA,
};
#endif
