/* Copyright (c) 2009, 2010, Oracle and/or its affiliates.
   Copyright (c) 2012, 2013, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* support for Services */
#include <service_versions.h>

struct st_service_ref {
  const char *name;
  uint version;
  void *service;
};

static struct my_snprintf_service_st my_snprintf_handler = {
  my_snprintf,
  my_vsnprintf
};

static struct thd_alloc_service_st thd_alloc_handler= {
  thd_alloc,
  thd_calloc,
  thd_strdup,
  thd_strmake,
  thd_memdup,
  thd_make_lex_string
};

static struct thd_wait_service_st thd_wait_handler= {
  thd_wait_begin,
  thd_wait_end
};

static struct progress_report_service_st progress_report_handler= {
  thd_progress_init,
  thd_progress_report,
  thd_progress_next_stage,
  thd_progress_end,
  set_thd_proc_info
};

static struct kill_statement_service_st thd_kill_statement_handler= {
  thd_kill_level
};

static struct thd_timezone_service_st thd_timezone_handler= {
  thd_TIME_to_gmt_sec,
  thd_gmt_sec_to_TIME
};

static struct my_sha1_service_st my_sha1_handler = {
  my_sha1,
  my_sha1_multi
};

static struct logger_service_st logger_service_handler= {
  logger_init_mutexes,
  logger_open,
  logger_close,
  logger_vprintf,
  logger_printf,
  logger_write,
  logger_rotate
};

static struct thd_autoinc_service_st thd_autoinc_handler= {
  thd_get_autoinc
};

static struct thd_error_context_service_st thd_error_conext_handler= {
  thd_get_error_message,
  thd_get_error_number,
  thd_get_error_row,
  thd_inc_error_row,
  thd_get_error_context_description
};

static struct st_service_ref list_of_services[]=
{
  { "my_snprintf_service",         VERSION_my_snprintf,         &my_snprintf_handler },
  { "thd_alloc_service",           VERSION_thd_alloc,           &thd_alloc_handler },
  { "thd_wait_service",            VERSION_thd_wait,            &thd_wait_handler },
  { "progress_report_service",     VERSION_progress_report,     &progress_report_handler },
  { "debug_sync_service",          VERSION_debug_sync,          0 }, // updated in plugin_init()
  { "thd_kill_statement_service",  VERSION_kill_statement,      &thd_kill_statement_handler },
  { "thd_timezone_service",        VERSION_thd_timezone,        &thd_timezone_handler },
  { "my_sha1_service",             VERSION_my_sha1,             &my_sha1_handler},
  { "logger_service",              VERSION_logger,              &logger_service_handler },
  { "thd_autoinc_service",         VERSION_thd_autoinc,         &thd_autoinc_handler },
  { "thd_error_context_service",   VERSION_thd_error_context,   &thd_error_conext_handler },
};

