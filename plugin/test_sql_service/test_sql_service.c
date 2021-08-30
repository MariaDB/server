/* Copyright (C) 2019, Alexey Botchkov and MariaDB Corporation

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


#define PLUGIN_VERSION 0x10000
#define PLUGIN_STR_VERSION "1.0.0"

#include <my_config.h>
#include <my_global.h>
#include <my_base.h>
#include <mysql/plugin_audit.h>
#include <mysql.h>


/* Status variables for SHOW STATUS */
static long test_passed= 0;
static struct st_mysql_show_var test_sql_status[]=
{
  {"test_sql_service_passed", (char *)&test_passed, SHOW_LONG},
  {0,0,0}
};

static my_bool do_test= TRUE;
static void run_test(MYSQL_THD thd, struct st_mysql_sys_var *var,
                     void *var_ptr, const void *save);
static MYSQL_SYSVAR_BOOL(run_test, do_test, PLUGIN_VAR_OPCMDARG,
           "Perform the test now.", NULL, run_test, FALSE);
static struct st_mysql_sys_var* test_sql_vars[]=
{
  MYSQL_SYSVAR(run_test),
  NULL
};


static int do_tests()
{
  MYSQL *mysql;
  MYSQL_RES *res;
  int result= 1;

  mysql= mysql_init(NULL);
  if (mysql_real_connect_local(mysql, NULL, NULL, NULL, 0) == NULL)
    return 1;

  if (mysql_real_query(mysql,
        STRING_WITH_LEN("CREATE TABLE test.ts_table"
          " ( hash varbinary(512),"
          " time timestamp default current_time,"
          " primary key (hash), index tm (time) )")))
    goto exit;

  if (mysql_real_query(mysql, STRING_WITH_LEN("INSERT INTO test.ts_table VALUES('1234567890', NULL)")))
    goto exit;

  if (mysql_real_query(mysql, STRING_WITH_LEN("select * from test.ts_table")))
    goto exit;

  if (!(res= mysql_store_result(mysql)))
    goto exit;

  mysql_free_result(res);

  if (mysql_real_query(mysql, STRING_WITH_LEN("DROP TABLE test.ts_table")))
    goto exit;

  result= 0;
exit:
  mysql_close(mysql);

  return result;
}
 

void auditing(MYSQL_THD thd, unsigned int event_class, const void *ev)
{
}


static void run_test(MYSQL_THD thd  __attribute__((unused)),
                     struct st_mysql_sys_var *var  __attribute__((unused)),
                     void *var_ptr  __attribute__((unused)),
                     const void *save  __attribute__((unused)))
{
  test_passed= do_tests();
}


static int init_done= 0;

static int test_sql_service_plugin_init(void *p __attribute__((unused)))
{
  init_done= 1;
  do_tests();
  return 0;
}


static int test_sql_service_plugin_deinit(void *p __attribute__((unused)))
{
  if (!init_done)
    return 0;

  return 0;
}


static struct st_mysql_audit maria_descriptor =
{
  MYSQL_AUDIT_INTERFACE_VERSION,
  NULL,
  auditing,
  { MYSQL_AUDIT_GENERAL_CLASSMASK |
    MYSQL_AUDIT_TABLE_CLASSMASK |
    MYSQL_AUDIT_CONNECTION_CLASSMASK }
};
maria_declare_plugin(test_sql_service)
{
  MYSQL_AUDIT_PLUGIN,
  &maria_descriptor,
  "TEST_SQL_SERVICE",
  "Alexey Botchkov (MariaDB Corporation)",
  "Test SQL service",
  PLUGIN_LICENSE_GPL,
  test_sql_service_plugin_init,
  test_sql_service_plugin_deinit,
  PLUGIN_VERSION,
  test_sql_status,
  test_sql_vars,
  PLUGIN_STR_VERSION,
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

