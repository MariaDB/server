/* Copyright (C) 2013 Percona and Sergey Vojtovich
   Copyright (C) 2024 MariaDB Foundation

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
#include <my_global.h>
#include <sql_class.h>
#include <sql_i_s.h>
#include <sql_show.h>
#include <mysql/plugin_audit.h>
#include "query_response_time.h"
#include <sql_parse.h>

ulong opt_query_response_time_range_base= QRT_DEFAULT_BASE;
my_bool opt_query_response_time_stats= 0;
static my_bool opt_query_response_time_flush= 0;
static my_bool inited= 0;

static void query_response_time_flush_update(
              MYSQL_THD thd __attribute__((unused)),
              struct st_mysql_sys_var *var __attribute__((unused)),
              void *tgt __attribute__((unused)),
              const void *save __attribute__((unused)))
{
  query_response_time_flush_all();
}

enum session_stat
{
  session_stat_global,
  session_stat_on,
  session_stat_off
};

static const char *session_stat_names[]= {"GLOBAL", "ON", "OFF", NullS};
static TYPELIB session_stat_typelib= CREATE_TYPELIB_FOR(session_stat_names);

static MYSQL_SYSVAR_ULONG(range_base, opt_query_response_time_range_base,
       PLUGIN_VAR_RQCMDARG,
       "Select base of log for query_response_time ranges. "
       "WARNING: change of this variable take effect only after next "
       "FLUSH QUERY_RESPONSE_TIME execution.  Changing the variable will "
       "flush both read and writes on the next FLUSH",
       NULL, NULL, QRT_DEFAULT_BASE, 2, QRT_MAXIMUM_BASE, 1);
static MYSQL_SYSVAR_BOOL(stats, opt_query_response_time_stats,
       PLUGIN_VAR_OPCMDARG,
       "Enable or disable query response time statistics collecting",
       NULL, NULL, FALSE);
static MYSQL_SYSVAR_BOOL(flush, opt_query_response_time_flush,
       PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_DEPRECATED,
       "Update of this variable flushes statistics and re-reads "
       "query_response_time_range_base. Compatibility variable, "
       "use FLUSH QUERY_RESPONSE_TIME instead",
       NULL, query_response_time_flush_update, FALSE);
#ifndef DBUG_OFF
static MYSQL_THDVAR_ULONGLONG(exec_time_debug, PLUGIN_VAR_NOCMDOPT,
       "Pretend queries take this many microseconds. When 0 (the default) use "
       "the actual execution time. Used only for debugging",
       NULL, NULL, 0, 0, LONG_TIMEOUT, 1);
#endif
static MYSQL_THDVAR_ENUM(session_stats, PLUGIN_VAR_RQCMDARG,
       "Controls query response time statistics collection for the current "
       "session: ON - enable, OFF - disable, GLOBAL (default) - use "
       "query_response_time_stats value", NULL, NULL,
       session_stat_global, &session_stat_typelib);

static struct st_mysql_sys_var *query_response_time_info_vars[]=
{
  MYSQL_SYSVAR(range_base),
  MYSQL_SYSVAR(stats),
  MYSQL_SYSVAR(flush),
#ifndef DBUG_OFF
  MYSQL_SYSVAR(exec_time_debug),
#endif
  MYSQL_SYSVAR(session_stats),
  NULL
};

namespace Show {

ST_FIELD_INFO query_response_time_fields_info[] =
{
  Column("TIME",  Varchar(QRT_TIME_STRING_LENGTH), NOT_NULL, "Time"),
  Column("COUNT", ULong(),                         NOT_NULL, "Count"),
  Column("TOTAL", Varchar(QRT_TIME_STRING_LENGTH), NOT_NULL, "Total"),
  CEnd()
};

ST_FIELD_INFO query_response_time_rw_fields_info[] =
{
  Column("TIME",  Varchar(QRT_TIME_STRING_LENGTH),      NOT_NULL, "Time"),
  Column("READ_COUNT", ULong(),                         NOT_NULL, "Read_count"),
  Column("READ_TOTAL", Varchar(QRT_TIME_STRING_LENGTH), NOT_NULL, "Read_total"),
  Column("WRITE_COUNT", ULong(),                        NOT_NULL,
         "Write_Count"),
  Column("WRITE_TOTAL", Varchar(QRT_TIME_STRING_LENGTH),NOT_NULL,
         "Write_Total"),
  CEnd()
};

} // namespace Show

static int query_response_time_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= Show::query_response_time_fields_info;
  i_s_query_response_time->fill_table= query_response_time_fill;
  i_s_query_response_time->reset_table= query_response_time_flush_all;
  query_response_time_init();
  inited= 1;
  return 0;
}

static int query_response_time_read_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= Show::query_response_time_fields_info;
  i_s_query_response_time->fill_table= query_response_time_fill_read;
  i_s_query_response_time->reset_table= query_response_time_flush_read;
  query_response_time_init();
  return 0;
}

static int query_response_time_write_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= Show::query_response_time_fields_info;
  i_s_query_response_time->fill_table= query_response_time_fill_write;
  i_s_query_response_time->reset_table= query_response_time_flush_write;
  query_response_time_init();
  return 0;
}

static int query_response_time_read_write_init(void *p)
{
  ST_SCHEMA_TABLE *i_s_query_response_time= (ST_SCHEMA_TABLE *) p;
  i_s_query_response_time->fields_info= Show::query_response_time_rw_fields_info;
  i_s_query_response_time->fill_table= query_response_time_fill_read_write;
  i_s_query_response_time->reset_table= query_response_time_flush_all;
  query_response_time_init();
  return 0;
}

static int query_response_time_deinit_main(void *arg __attribute__((unused)))
{
  opt_query_response_time_stats= 0;
  query_response_time_free();
  inited= 0;
  return 0;
}

static int query_response_time_deinit(void *arg __attribute__((unused)))
{
  query_response_time_free();
  return 0;
}


static struct st_mysql_information_schema query_response_time_info_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


static bool query_response_time_should_log(MYSQL_THD thd)
{
  /*
    We don't log sub statements and in this the time will be accounted
    multiple times (both for the sub statement and the real statement.
  */
  if (!inited || thd->in_sub_stmt)
    return 0;

  const enum session_stat session_stat_val=
    static_cast<session_stat>(THDVAR(thd, session_stats));

  if (!((session_stat_val == session_stat_on) ||
        (session_stat_val == session_stat_global &&
         opt_query_response_time_stats)))
    return 0;

  if (!thd->lex)
  {
    /*
      This can only happen for stored procedures/functions that failed
      when calling sp_lex_keeper::validate_lex_and_exec_core(). In this
      case the statement was never executed.
    */
    return 0;
  }
  if (thd->lex->sql_command == SQLCOM_CALL)
  {
    /*
      For stored procedures we have already handled all statements.
      Ignore the call which contains the time for all sub statements.
    */
    return 0;
  }
  if (thd->lex->sql_command == SQLCOM_FLUSH)
  {
    /*
      Ignore FLUSH as we don't want FLUSH query_response_time to affect
      statistics.
    */
    return 0;
  }
  return 1;
}


static void query_response_time_audit_notify(MYSQL_THD thd,
                                             unsigned int event_class,
                                             const void *event)
{
  const struct mysql_event_general *event_general=
    (const struct mysql_event_general *) event;
  DBUG_ASSERT(event_class == MYSQL_AUDIT_GENERAL_CLASS);

  if (event_general->event_subclass == MYSQL_AUDIT_GENERAL_STATUS &&
      query_response_time_should_log(thd))
  {
    bool stmt_changes_data= is_update_query(thd->last_sql_command)
                         || thd->transaction->stmt.is_trx_read_write();
    QUERY_TYPE query_type= stmt_changes_data ? WRITE : READ;
#ifndef DBUG_OFF
    if (THDVAR(thd, exec_time_debug))
    {
      /* This code is only here for MTR tests */
      query_response_time_collect(query_type,
                                  thd->lex->sql_command != SQLCOM_SET_OPTION ?
                                  THDVAR(thd, exec_time_debug) : 0);
    }
    else
#endif
    {
      DBUG_ASSERT(thd->utime_after_query >= thd->utime_after_lock);
      query_response_time_collect(query_type,
                                  thd->utime_after_query -
                                  thd->utime_after_lock);
    }
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
  query_response_time_init,
  query_response_time_deinit_main,
  0x0100,
  NULL,
  query_response_time_info_vars,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME_READ",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_read_init,
  query_response_time_deinit,
  0x0100,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME_WRITE",
  "Percona and Sergey Vojtovich",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_write_init,
  query_response_time_deinit,
  0x0100,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &query_response_time_info_descriptor,
  "QUERY_RESPONSE_TIME_READ_WRITE",
  "Monty",
  "Query Response Time Distribution INFORMATION_SCHEMA Plugin",
  PLUGIN_LICENSE_GPL,
  query_response_time_read_write_init,
  query_response_time_deinit,
  0x0100,
  NULL,
  NULL,
  "2.0",
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
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
