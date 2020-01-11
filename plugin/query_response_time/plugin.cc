/* Copyright (C) 2013 Percona and Sergey Vojtovich

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

#define MYSQL_SERVER
#include <sql_class.h>
#include <table.h>
#include <sql_show.h>
#include <mysql/plugin_audit.h>
#include "query_response_time.h"


ulong opt_query_response_time_range_base= QRT_DEFAULT_BASE;
my_bool opt_query_response_time_stats= 0;
static my_bool opt_query_response_time_flush= 0;


static void query_response_time_flush_update(
              MYSQL_THD thd __attribute__((unused)),
              struct st_mysql_sys_var *var __attribute__((unused)),
              void *tgt __attribute__((unused)),
              const void *save __attribute__((unused)))
{
  query_response_time_flush();
}


static MYSQL_SYSVAR_ULONG(range_base, opt_query_response_time_range_base,
       PLUGIN_VAR_RQCMDARG,
       "Select base of log for query_response_time ranges. WARNING: variable "
       "change affect only after flush",
       NULL, NULL, QRT_DEFAULT_BASE, 2, QRT_MAXIMUM_BASE, 1);
static MYSQL_SYSVAR_BOOL(stats, opt_query_response_time_stats,
       PLUGIN_VAR_OPCMDARG,
       "Enable or disable query response time statisics collecting",
       NULL, NULL, FALSE);
static MYSQL_SYSVAR_BOOL(flush, opt_query_response_time_flush,
       PLUGIN_VAR_NOCMDOPT,
       "Update of this variable flushes statistics and re-reads "
       "query_response_time_range_base",
       NULL, query_response_time_flush_update, FALSE);
#ifndef DBUG_OFF
static MYSQL_THDVAR_ULONGLONG(exec_time_debug, PLUGIN_VAR_NOCMDOPT,
       "Pretend queries take this many microseconds. When 0 (the default) use "
       "the actual execution time. Used only for debugging.",
       NULL, NULL, 0, 0, LONG_TIMEOUT, 1);
#endif


static struct st_mysql_sys_var *query_response_time_info_vars[]=
{
  MYSQL_SYSVAR(range_base),
  MYSQL_SYSVAR(stats),
  MYSQL_SYSVAR(flush),
#ifndef DBUG_OFF
  MYSQL_SYSVAR(exec_time_debug),
#endif
  NULL
};


ST_FIELD_INFO query_response_time_fields_info[] =
{
  { "TIME",  QRT_TIME_STRING_LENGTH,      MYSQL_TYPE_STRING,  0, 0,               "Time", 0 },
  { "COUNT", MY_INT32_NUM_DECIMAL_DIGITS, MYSQL_TYPE_LONG,    0, MY_I_S_UNSIGNED, "Count", 0 },
  { "TOTAL", QRT_TIME_STRING_LENGTH,      MYSQL_TYPE_STRING,  0, 0,               "Total", 0 },
  { 0, 0, MYSQL_TYPE_NULL, 0, 0, 0, 0 }
};


static int query_response_time_info_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= query_response_time_fields_info;
  i_s_query_response_time->fill_table= query_response_time_fill;
  i_s_query_response_time->reset_table= query_response_time_flush;
  query_response_time_init();
  return 0;
}


static int query_response_time_info_deinit(void *arg __attribute__((unused)))
{
  opt_query_response_time_stats= 0;
  query_response_time_free();
  return 0;
}


static struct st_mysql_information_schema query_response_time_info_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


static void query_response_time_audit_notify(MYSQL_THD thd,
                                             unsigned int event_class,
                                             const void *event)
{
  const struct mysql_event_general *event_general=
    (const struct mysql_event_general *) event;
  DBUG_ASSERT(event_class == MYSQL_AUDIT_GENERAL_CLASS);
  if (event_general->event_subclass == MYSQL_AUDIT_GENERAL_STATUS &&
      opt_query_response_time_stats)
  {
#ifndef DBUG_OFF
    if (THDVAR(thd, exec_time_debug))
      query_response_time_collect(thd->lex->sql_command != SQLCOM_SET_OPTION ?
                                  THDVAR(thd, exec_time_debug) : 0);
    else
#endif
    query_response_time_collect(thd->utime_after_query - thd->utime_after_lock);
  }
}


static struct st_mysql_audit query_response_time_audit_descriptor=
{
  MYSQL_AUDIT_INTERFACE_VERSION, NULL, query_response_time_audit_notify,
  { (unsigned long) MYSQL_AUDIT_GENERAL_CLASSMASK }
};


maria_declare_plugin(query_response_time)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_info_init,
  query_response_time_info_deinit,
  0x0100,
  NULL,
  query_response_time_info_vars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_AUDIT_PLUGIN,
  &query_response_time_audit_descriptor,
  "QUERY_RESPONSE_TIME_AUDIT",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution Audit Plugin",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
