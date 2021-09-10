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


#define PLUGIN_VERSION 0x200

#include <my_config.h>
#include <my_global.h>
#include <my_base.h>
#include <mysql/plugin_audit.h>
#include <mysql.h>


/* Status variables for SHOW STATUS */
static long test_passed= 0;
static char *sql_text_local, *sql_text_global;
static char qwe_res[1024]= "";

static struct st_mysql_show_var test_sql_status[]=
{
  {"test_sql_service_passed", (char *)&test_passed, SHOW_LONG},
  {"test_sql_query_result", qwe_res, SHOW_CHAR},
  {0,0,0}
};

static my_bool do_test= TRUE;
static int run_test(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                    struct st_mysql_value *value);
static int run_sql_local(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                         struct st_mysql_value *value);
static int run_sql_global(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                          struct st_mysql_value *value);

static void noop_update(MYSQL_THD thd, struct st_mysql_sys_var *var,
                        void *var_ptr, const void *save);

static MYSQL_SYSVAR_BOOL(run_test, do_test,
                         PLUGIN_VAR_OPCMDARG,
                         "Perform the test now.",
                         run_test, NULL, FALSE);

static MYSQL_SYSVAR_STR(execute_sql_local, sql_text_local,
                        PLUGIN_VAR_OPCMDARG,
                        "Create the new local connection, execute SQL statement with it.",
                        run_sql_local, noop_update, FALSE);

static MYSQL_SYSVAR_STR(execute_sql_global, sql_text_global,
                        PLUGIN_VAR_OPCMDARG,
                        "Execute SQL statement using the global connection.",
                        run_sql_global, noop_update, FALSE);

static struct st_mysql_sys_var* test_sql_vars[]=
{
  MYSQL_SYSVAR(run_test),
  MYSQL_SYSVAR(execute_sql_local),
  MYSQL_SYSVAR(execute_sql_global),
  NULL
};

static MYSQL *global_mysql;


static int run_queries(MYSQL *mysql)
{
  MYSQL_RES *res;

  if (mysql_real_query(mysql,
        STRING_WITH_LEN("CREATE TABLE test.ts_table"
          " ( hash varbinary(512),"
          " time timestamp default current_time,"
          " primary key (hash), index tm (time) )")))
    return 1;

  if (mysql_real_query(mysql,
       STRING_WITH_LEN("INSERT INTO test.ts_table VALUES('1234567890', NULL)")))
    return 1;

  if (mysql_real_query(mysql, STRING_WITH_LEN("select * from test.ts_table")))
    return 1;

  if (!(res= mysql_store_result(mysql)))
    return 1;

  mysql_free_result(res);

  if (mysql_real_query(mysql, STRING_WITH_LEN("DROP TABLE test.ts_table")))
    return 1;

  return 0;
}


static int do_tests()
{
  MYSQL *mysql;
  int result= 1;

  mysql= mysql_init(NULL);
  if (mysql_real_connect_local(mysql, NULL, NULL, NULL, 0) == NULL)
    return 1;

  if (run_queries(mysql))
    goto exit;

  if (run_queries(global_mysql))
    goto exit;

  result= 0;
exit:
  mysql_close(mysql);

  return result;
}
 

void auditing(MYSQL_THD thd, unsigned int event_class, const void *ev)
{
}


static int run_test(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                    struct st_mysql_value *value)
{
  return (test_passed= (do_tests() == 0)) == 0;
}


static int run_sql(MYSQL *mysql, void *save, struct st_mysql_value *value)
{
  const char *str;
  int len= 0;
  MYSQL_RES *res;

  str= value->val_str(value, NULL, &len);

  if (mysql_real_query(mysql, str, len))
  {
    if (mysql_error(mysql)[0])
    {
      my_snprintf(qwe_res, sizeof(qwe_res), "Error %d returned. %s",
          mysql_errno(mysql), mysql_error(mysql));
      return 0;
    }

    return 1;
  }

  if ((res= mysql_store_result(mysql)))
  {
    my_snprintf(qwe_res, sizeof(qwe_res), "Query returned %lld rows.",
        mysql_num_rows(res));
    mysql_free_result(res);
  }
  else
  {
    if (mysql_error(mysql)[0])
    {
      my_snprintf(qwe_res, sizeof(qwe_res), "Error %d returned. %s",
          mysql_errno(mysql), mysql_error(mysql));
    }
    else
      my_snprintf(qwe_res, sizeof(qwe_res), "Query affected %lld rows.",
          mysql_affected_rows(mysql));
  }

  return 0;
}


static void noop_update(MYSQL_THD thd, struct st_mysql_sys_var *var,
                        void *var_ptr, const void *save)
{
  sql_text_local= sql_text_global= qwe_res;
}

static int run_sql_local(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                         struct st_mysql_value *value)
{
  MYSQL *mysql;
  int result= 1;

  mysql= mysql_init(NULL);
  if (mysql_real_connect_local(mysql, NULL, NULL, NULL, 0) == NULL)
    return 1;

  if (run_sql(mysql, save, value))
    goto exit;

  result= 0;

exit:
  mysql_close(mysql);

  return result;
}


static int run_sql_global(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save,
                          struct st_mysql_value *value)
{
  return run_sql(global_mysql, save, value);
}


static int init_done= 0;

static int test_sql_service_plugin_init(void *p __attribute__((unused)))
{
  global_mysql= mysql_init(NULL);

  if (!global_mysql ||
      mysql_real_connect_local(global_mysql, NULL, NULL, NULL, 0) == NULL)
    return 1;

  init_done= 1;

  test_passed= (do_tests() == 0);

  return 0;
}


static int test_sql_service_plugin_deinit(void *p __attribute__((unused)))
{
  if (!init_done)
    return 0;

  mysql_close(global_mysql);

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
  NULL,
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

