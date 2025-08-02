/* Copyright (C) 2012 Monty Program Ab.

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

#include <mysql/plugin_audit.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mysql/service_logger.h>

/*
 Disable __attribute__() on non-gcc compilers.
*/
#if !defined(__attribute__) && !defined(__GNUC__)
#define __attribute__(A)
#endif

#ifdef _WIN32
#define localtime_r(a, b) localtime_s(b, a)
#endif /*WIN32*/

/*
  rate 0 means the logging was disabled.
*/


static char *filename;
static unsigned int rate;
static unsigned long long size_limit;
static unsigned int rotations;
static char rotate;
static char warnings;
static char with_db_and_thread_info;

static unsigned int count;
LOGGER_HANDLE *logfile;

static void rotate_log(MYSQL_THD thd, struct st_mysql_sys_var *var,
                       void *var_ptr, const void *save);

static MYSQL_SYSVAR_UINT(rate, rate, PLUGIN_VAR_RQCMDARG,
       "Sampling rate. If set to 0(zero), the logging is disabled", NULL, NULL,
       1, 0, 1000000, 1);

static MYSQL_SYSVAR_ULONGLONG(size_limit, size_limit,
       PLUGIN_VAR_READONLY, "Log file size limit", NULL, NULL,
       1000000, 100, ((long long) 0x7FFFFFFFFFFFFFFFLL), 1);

static MYSQL_SYSVAR_UINT(rotations, rotations,
       PLUGIN_VAR_READONLY, "Number of rotations before log is removed",
       NULL, NULL, 9, 1, 999, 1);

static MYSQL_SYSVAR_BOOL(rotate, rotate,
       PLUGIN_VAR_OPCMDARG, "Force log rotation", NULL, rotate_log,
       0);

static MYSQL_SYSVAR_STR(filename, filename,
       PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
       "The file to log sql errors to", NULL, NULL,
       "sql_errors.log");

static MYSQL_SYSVAR_BOOL(warnings, warnings,
                         PLUGIN_VAR_OPCMDARG,
                         "Warnings. If set to 0, warnings are not logged",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_BOOL(with_db_and_thread_info, with_db_and_thread_info,
       PLUGIN_VAR_READONLY | PLUGIN_VAR_OPCMDARG,
       "Show details about thread id and database name in the log",
       NULL, NULL,
       0);

static struct st_mysql_sys_var* vars[] = {
    MYSQL_SYSVAR(rate),
    MYSQL_SYSVAR(size_limit),
    MYSQL_SYSVAR(rotations),
    MYSQL_SYSVAR(rotate),
    MYSQL_SYSVAR(filename),
    MYSQL_SYSVAR(warnings),
    MYSQL_SYSVAR(with_db_and_thread_info),
    NULL
};


static void log_sql_errors(MYSQL_THD thd __attribute__((unused)),
                           unsigned int event_class __attribute__((unused)),
                           const void *ev)
{
  const struct mysql_event_general *event =
         (const struct mysql_event_general*)ev;

  if (rate &&
      (event->event_subclass == MYSQL_AUDIT_GENERAL_ERROR ||
       (warnings && event->event_subclass == MYSQL_AUDIT_GENERAL_WARNING)))
  {
    const char *type= (event->event_subclass == MYSQL_AUDIT_GENERAL_ERROR ?
                       "ERROR" : "WARNING");
    if (++count >= rate)
    {
      struct tm t;
      time_t event_time = event->general_time;

      count = 0;
      (void) localtime_r(&event_time, &t);
      if (with_db_and_thread_info)
      {
        if (event->database.str)
        {
          logger_printf(logfile, "%04d-%02d-%02d %2d:%02d:%02d %lu "
                      "%s %sQ %s %d: %s : %s \n",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
              t.tm_sec, event->general_thread_id, event->general_user,
              event->database.str, type,
              event->general_error_code, event->general_command, event->general_query);
        }
        else
        {
          logger_printf(logfile, "%04d-%02d-%02d %2d:%02d:%02d %lu "
                      "%s NULL %s %d: %s : %s \n",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
              t.tm_sec, event->general_thread_id, event->general_user, type,
              event->general_error_code, event->general_command, event->general_query);
        }
      }
      else
      {
        logger_printf(logfile, "%04d-%02d-%02d %2d:%02d:%02d "
                      "%s %s %d: %s : %s\n",
              t.tm_year + 1900, t.tm_mon + 1,
              t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
              event->general_user, type, event->general_error_code,
              event->general_command, event->general_query);
      }
    }
  }
}


static int sql_error_log_init(void *p __attribute__((unused)))
{
  logger_init_mutexes();

  logfile= logger_open(filename, size_limit, rotations, 0);
  if (logfile == NULL) {
    fprintf(stderr, "Could not create file '%s'\n",
            filename);
    return 1;
  }
  count = 0;
  return 0;
}


static int sql_error_log_deinit(void *p __attribute__((unused)))
{
  if (logfile)
    logger_close(logfile);
  return 0;
}


static void rotate_log(MYSQL_THD thd  __attribute__((unused)),
                       struct st_mysql_sys_var *var  __attribute__((unused)),
                       void *var_ptr  __attribute__((unused)),
                       const void *save  __attribute__((unused)))
{
  (void) logger_rotate(logfile);
}


static struct st_mysql_audit descriptor =
{
  MYSQL_AUDIT_INTERFACE_VERSION,
  NULL,
  log_sql_errors,
  { MYSQL_AUDIT_GENERAL_CLASSMASK }
};

maria_declare_plugin(sql_errlog)
{
  MYSQL_AUDIT_PLUGIN,
  &descriptor,
  "SQL_ERROR_LOG",
  "Alexey Botchkov",
  "Log SQL level errors to a file with rotation",
  PLUGIN_LICENSE_GPL,
  sql_error_log_init,
  sql_error_log_deinit,
  0x0100,
  NULL,
  vars,
  "1.1",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
