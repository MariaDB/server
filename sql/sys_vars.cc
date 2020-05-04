/* Copyright (c) 2002, 2015, Oracle and/or its affiliates.
   Copyright (c) 2012, 2018, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file
  Definitions of all server's session or global variables.

  How to add new variables:

  1. copy one of the existing variables, and edit the declaration.
  2. if you need special behavior on assignment or additional checks
     use ON_CHECK and ON_UPDATE callbacks.
  3. *Don't* add new Sys_var classes or uncle Occam will come
     with his razor to haunt you at nights

  Note - all storage engine variables (for example myisam_whatever)
  should go into the corresponding storage engine sources
  (for example in storage/myisam/ha_myisam.cc) !
*/

#include "sql_plugin.h"
#include "sql_priv.h"
#include "sql_class.h"                          // set_var.h: THD
#include "sys_vars.ic"
#include "my_sys.h"

#include "events.h"
#include <thr_alarm.h>
#include "slave.h"
#include "rpl_mi.h"
#include "transaction.h"
#include "mysqld.h"
#include "lock.h"
#include "sql_time.h"                       // known_date_time_formats
#include "sql_acl.h" // mysql_user_table_is_in_short_password_format
#include "derror.h"  // read_texts
#include "sql_base.h"                           // close_cached_tables
#include "hostname.h"                           // host_cache_size
#include <myisam.h>
#include "debug_sync.h"                         // DEBUG_SYNC
#include "sql_show.h"
#include "opt_trace_context.h"

#include "log_event.h"
#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#include "../storage/perfschema/pfs_server.h"
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */
#include "threadpool.h"
#include "sql_repl.h"
#include "opt_range.h"
#include "rpl_parallel.h"
#include "semisync_master.h"
#include "semisync_slave.h"
#include <ssl_compat.h>

#define PCRE2_STATIC 1             /* Important on Windows */
#include "pcre2.h"                 /* pcre2 header file */

/*
  The rule for this file: everything should be 'static'. When a sys_var
  variable or a function from this file is - in very rare cases - needed
  elsewhere it should be explicitly declared 'export' here to show that it's
  not a mistakenly forgotten 'static' keyword.
*/
#define export /* not static */

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE

static Sys_var_mybool Sys_pfs_enabled(
       "performance_schema", "Enable the performance schema.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_enabled),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_long Sys_pfs_events_waits_history_long_size(
       "performance_schema_events_waits_history_long_size",
       "Number of rows in EVENTS_WAITS_HISTORY_LONG."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_waits_history_long_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_waits_history_size(
       "performance_schema_events_waits_history_size",
       "Number of rows per thread in EVENTS_WAITS_HISTORY."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_waits_history_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_cond_classes(
       "performance_schema_max_cond_classes",
       "Maximum number of condition instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_cond_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_COND_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_cond_instances(
       "performance_schema_max_cond_instances",
       "Maximum number of instrumented condition objects."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_cond_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_program_instances(
       "performance_schema_max_program_instances",
       "Maximum number of instrumented programs."
         " Use 0 to disable, -1 for automated scaling.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_program_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_prepared_stmt_instances(
       "performance_schema_max_prepared_statements_instances",
       "Maximum number of instrumented prepared statements."
         " Use 0 to disable, -1 for automated scaling.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_prepared_stmt_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_file_classes(
       "performance_schema_max_file_classes",
       "Maximum number of file instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_file_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_FILE_CLASS), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_file_handles(
       "performance_schema_max_file_handles",
       "Maximum number of opened instrumented files.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_file_handle_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 1024*1024),
       DEFAULT(PFS_MAX_FILE_HANDLE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_file_instances(
       "performance_schema_max_file_instances",
       "Maximum number of instrumented files."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_file_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_sockets(
       "performance_schema_max_socket_instances",
       "Maximum number of opened instrumented sockets."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_socket_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_socket_classes(
       "performance_schema_max_socket_classes",
       "Maximum number of socket instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_socket_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_SOCKET_CLASS), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_mutex_classes(
       "performance_schema_max_mutex_classes",
       "Maximum number of mutex instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_mutex_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_MUTEX_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_mutex_instances(
       "performance_schema_max_mutex_instances",
       "Maximum number of instrumented MUTEX objects."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_mutex_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 100*1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_rwlock_classes(
       "performance_schema_max_rwlock_classes",
       "Maximum number of rwlock instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_rwlock_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_RWLOCK_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_rwlock_instances(
       "performance_schema_max_rwlock_instances",
       "Maximum number of instrumented RWLOCK objects."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_rwlock_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 100*1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_table_handles(
       "performance_schema_max_table_handles",
       "Maximum number of opened instrumented tables."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_table_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_table_instances(
       "performance_schema_max_table_instances",
       "Maximum number of instrumented tables."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_table_share_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_table_lock_stat(
       "performance_schema_max_table_lock_stat",
       "Maximum number of lock statistics for instrumented tables."
         " Use 0 to disable, -1 for automated scaling.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_table_lock_stat_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_index_stat(
       "performance_schema_max_index_stat",
       "Maximum number of index statistics for instrumented tables."
         " Use 0 to disable, -1 for automated scaling.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_index_stat_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_thread_classes(
       "performance_schema_max_thread_classes",
       "Maximum number of thread instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_thread_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_THREAD_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_thread_instances(
       "performance_schema_max_thread_instances",
       "Maximum number of instrumented threads."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_thread_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_setup_actors_size(
       "performance_schema_setup_actors_size",
       "Maximum number of rows in SETUP_ACTORS.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_setup_actor_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_setup_objects_size(
       "performance_schema_setup_objects_size",
       "Maximum number of rows in SETUP_OBJECTS.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_setup_object_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_accounts_size(
       "performance_schema_accounts_size",
       "Maximum number of instrumented user@host accounts."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_account_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_hosts_size(
       "performance_schema_hosts_size",
       "Maximum number of instrumented hosts."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_host_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_users_size(
       "performance_schema_users_size",
       "Maximum number of instrumented users."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_user_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_stage_classes(
       "performance_schema_max_stage_classes",
       "Maximum number of stage instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_stage_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT(PFS_MAX_STAGE_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_stages_history_long_size(
       "performance_schema_events_stages_history_long_size",
       "Number of rows in EVENTS_STAGES_HISTORY_LONG."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_stages_history_long_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_stages_history_size(
       "performance_schema_events_stages_history_size",
       "Number of rows per thread in EVENTS_STAGES_HISTORY."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_stages_history_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

/**
  Variable performance_schema_max_statement_classes.
  The default number of statement classes is the sum of:
  - SQLCOM_END for all regular "statement/sql/...",
  - SP_PSI_STATEMENT_INFO_COUNT for "statement/sp/...".
  - (COM_END - mariadb gap) for all regular "statement/com/...",
  - 1 for "statement/com/new_packet", for unknown enum_server_command
  - 1 for "statement/com/Error", for invalid enum_server_command
  - 1 for "statement/sql/error", for invalid enum_sql_command
  - 1 for "statement/rpl/relay_log", for replicated statements.
  - 1 for "statement/scheduler/event", for scheduled events.
*/
static Sys_var_ulong Sys_pfs_max_statement_classes(
       "performance_schema_max_statement_classes",
       "Maximum number of statement instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_statement_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 256),
       DEFAULT((ulong) SQLCOM_END + SP_PSI_STATEMENT_INFO_COUNT +
               (ulong) (COM_END -(COM_MDB_GAP_END - COM_MDB_GAP_BEG + 1)) + 5),
       BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_statements_history_long_size(
       "performance_schema_events_statements_history_long_size",
       "Number of rows in EVENTS_STATEMENTS_HISTORY_LONG."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_statements_history_long_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_statements_history_size(
       "performance_schema_events_statements_history_size",
       "Number of rows per thread in EVENTS_STATEMENTS_HISTORY."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_statements_history_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_statement_stack_size(
       "performance_schema_max_statement_stack",
       "Number of rows per thread in EVENTS_STATEMENTS_CURRENT.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_statement_stack_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(1, 256),
       DEFAULT(PFS_STATEMENTS_STACK_SIZE), BLOCK_SIZE(1));

static Sys_var_ulong Sys_pfs_max_memory_classes(
       "performance_schema_max_memory_classes",
       "Maximum number of memory pool instruments.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_memory_class_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 1024),
       DEFAULT(PFS_MAX_MEMORY_CLASS), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_digest_size(
       "performance_schema_digests_size",
       "Size of the statement digest."
       " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_digest_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 200),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_transactions_history_long_size(
       "performance_schema_events_transactions_history_long_size",
       "Number of rows in EVENTS_TRANSACTIONS_HISTORY_LONG."
         " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_transactions_history_long_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024*1024),
       DEFAULT(PFS_AUTOSIZE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_events_transactions_history_size(
       "performance_schema_events_transactions_history_size",
       "Number of rows per thread in EVENTS_TRANSACTIONS_HISTORY."
         " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_events_transactions_history_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024),
       DEFAULT(PFS_AUTOSIZE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_digest_length(
       "performance_schema_max_digest_length",
       "Maximum length considered for digest text, when stored in performance_schema tables.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_max_digest_length),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 1024 * 1024),
       DEFAULT(1024), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_connect_attrs_size(
       "performance_schema_session_connect_attrs_size",
       "Size of session attribute string buffer per thread."
         " Use 0 to disable, -1 for automated sizing.",
       PARSED_EARLY READ_ONLY
       GLOBAL_VAR(pfs_param.m_session_connect_attrs_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 1024 * 1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_metadata_locks(
       "performance_schema_max_metadata_locks",
       "Maximum number of metadata locks."
         " Use 0 to disable, -1 for automated scaling.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_metadata_lock_sizing),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(-1, 100*1024*1024),
       DEFAULT(PFS_AUTOSCALE_VALUE), BLOCK_SIZE(1));

static Sys_var_long Sys_pfs_max_sql_text_length(
       "performance_schema_max_sql_text_length",
       "Maximum length of displayed sql text.",
       PARSED_EARLY READ_ONLY GLOBAL_VAR(pfs_param.m_max_sql_text_length),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 1024 * 1024),
       DEFAULT(1024), BLOCK_SIZE(1));

#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

#ifdef WITH_WSREP

/*
  We need to keep the original values set by the user, as they will
  be lost if wsrep_auto_increment_control set to 'ON':
*/
static bool update_auto_increment_increment (sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_GLOBAL)
    global_system_variables.saved_auto_increment_increment=
      global_system_variables.auto_increment_increment;
  else
    thd->variables.saved_auto_increment_increment=
      thd->variables.auto_increment_increment;
  return false;
}

#endif /* WITH_WSREP */

static Sys_var_double Sys_analyze_sample_percentage(
       "analyze_sample_percentage",
       "Percentage of rows from the table ANALYZE TABLE will sample "
       "to collect table statistics. Set to 0 to let MariaDB decide "
       "what percentage of rows to sample.",
       SESSION_VAR(sample_percentage),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 100),
       DEFAULT(100));

static Sys_var_ulong Sys_auto_increment_increment(
       "auto_increment_increment",
       "Auto-increment columns are incremented by this",
       SESSION_VAR(auto_increment_increment),
       CMD_LINE(OPT_ARG),
       VALID_RANGE(1, 65535), DEFAULT(1), BLOCK_SIZE(1),
#ifdef WITH_WSREP
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_auto_increment_increment));
#else
       NO_MUTEX_GUARD, IN_BINLOG);
#endif /* WITH_WSREP */

#ifdef WITH_WSREP

/*
  We need to keep the original values set by the user, as they will
  be lost if wsrep_auto_increment_control set to 'ON':
*/
static bool update_auto_increment_offset (sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_GLOBAL)
    global_system_variables.saved_auto_increment_offset=
      global_system_variables.auto_increment_offset;
  else
    thd->variables.saved_auto_increment_offset=
      thd->variables.auto_increment_offset;
  return false;
}

#endif /* WITH_WSREP */

static Sys_var_ulong Sys_auto_increment_offset(
       "auto_increment_offset",
       "Offset added to Auto-increment columns. Used when "
       "auto-increment-increment != 1",
       SESSION_VAR(auto_increment_offset),
       CMD_LINE(OPT_ARG),
       VALID_RANGE(1, 65535), DEFAULT(1), BLOCK_SIZE(1),
#ifdef WITH_WSREP
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_auto_increment_offset));
#else
       NO_MUTEX_GUARD, IN_BINLOG);
#endif /* WITH_WSREP */

static Sys_var_mybool Sys_automatic_sp_privileges(
       "automatic_sp_privileges",
       "Creating and dropping stored procedures alters ACLs",
       GLOBAL_VAR(sp_automatic_privileges),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static Sys_var_ulong Sys_back_log(
       "back_log", "The number of outstanding connection requests "
       "MariaDB can have. This comes into play when the main MariaDB thread "
       "gets very many connection requests in a very short time",
       AUTO_SET READ_ONLY GLOBAL_VAR(back_log), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 65535), DEFAULT(150), BLOCK_SIZE(1));

static Sys_var_charptr_fscs Sys_basedir(
       "basedir", "Path to installation directory. All paths are "
       "usually resolved relative to this",
       READ_ONLY GLOBAL_VAR(mysql_home_ptr), CMD_LINE(REQUIRED_ARG, 'b'),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_my_bind_addr(
       "bind_address", "IP address to bind to.",
       READ_ONLY GLOBAL_VAR(my_bind_addr_str), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

const char *Sys_var_vers_asof::asof_keywords[]= {"DEFAULT", NULL};
static Sys_var_vers_asof Sys_vers_asof_timestamp(
       "system_versioning_asof", "Default value for the FOR SYSTEM_TIME AS OF clause",
       SESSION_VAR(vers_asof_timestamp.type), NO_CMD_LINE,
       Sys_var_vers_asof::asof_keywords, DEFAULT(SYSTEM_TIME_UNSPECIFIED));

static const char *vers_alter_history_keywords[]= {"ERROR", "KEEP", NullS};
static Sys_var_enum Sys_vers_alter_history(
       "system_versioning_alter_history", "Versioning ALTER TABLE mode. "
       "ERROR: Fail ALTER with error; " /* TODO: fail only when history non-empty */
       "KEEP: Keep historical system rows and subject them to ALTER",
       SESSION_VAR(vers_alter_history), CMD_LINE(REQUIRED_ARG),
       vers_alter_history_keywords, DEFAULT(VERS_ALTER_HISTORY_ERROR));

static Sys_var_on_access_global<Sys_var_ulonglong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_CACHE_SIZE>
Sys_binlog_cache_size(
       "binlog_cache_size", "The size of the transactional cache for "
       "updates to transactional engines for the binary log. "
       "If you often use transactions containing many statements, "
       "you can increase this to get more performance",
       GLOBAL_VAR(binlog_cache_size),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE, SIZE_T_MAX), DEFAULT(32768), BLOCK_SIZE(IO_SIZE));

static Sys_var_on_access_global<Sys_var_ulonglong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_FILE_CACHE_SIZE>
Sys_binlog_file_cache_size(
       "binlog_file_cache_size", 
       "The size of file cache for the binary log", 
       GLOBAL_VAR(binlog_file_cache_size),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE*2, SIZE_T_MAX), DEFAULT(IO_SIZE*4), BLOCK_SIZE(IO_SIZE));

static Sys_var_on_access_global<Sys_var_ulonglong,
                             PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_STMT_CACHE_SIZE>
Sys_binlog_stmt_cache_size(
       "binlog_stmt_cache_size", "The size of the statement cache for "
       "updates to non-transactional engines for the binary log. "
       "If you often use statements updating a great number of rows, "
       "you can increase this to get more performance.",
       GLOBAL_VAR(binlog_stmt_cache_size),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE, SIZE_T_MAX), DEFAULT(32768), BLOCK_SIZE(IO_SIZE));

/*
  Some variables like @sql_log_bin and @binlog_format change how/if binlogging
  is done. We must not change them inside a running transaction or statement,
  otherwise the event group eventually written to the binlog may become
  incomplete or otherwise garbled.

  This function does the appropriate check.

  It returns true if an error is caused by incorrect usage, false if ok.
*/
static bool
error_if_in_trans_or_substatement(THD *thd, int in_substatement_error,
                                  int in_transaction_error)
{
  if (unlikely(thd->in_sub_stmt))
  {
    my_error(in_substatement_error, MYF(0));
    return true;
  }

  if (unlikely(thd->in_active_multi_stmt_transaction()))
  {
    my_error(in_transaction_error, MYF(0));
    return true;
  }

  return false;
}

bool check_has_super(sys_var *self, THD *thd, set_var *var)
{
  DBUG_ASSERT(self->scope() != sys_var::GLOBAL);// don't abuse check_has_super()
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (!(thd->security_ctx->master_access &
        PRIV_SET_RESTRICTED_SESSION_SYSTEM_VARIABLE))
  {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "SUPER");
    return true;
  }
#endif
  return false;
}

static Sys_var_bit Sys_core_file("core_file", "write a core-file on crashes",
          READ_ONLY GLOBAL_VAR(test_flags), NO_CMD_LINE,
          TEST_CORE_ON_SIGNAL, DEFAULT(IF_WIN(TRUE,FALSE)), NO_MUTEX_GUARD, NOT_IN_BINLOG,
          0,0,0);

static bool binlog_format_check(sys_var *self, THD *thd, set_var *var)
{
  /*
    MariaDB Galera does not support STATEMENT or MIXED binlog format currently.
  */
  if ((WSREP(thd) || opt_support_flashback) &&
      var->save_result.ulonglong_value != BINLOG_FORMAT_ROW)
  {
    // Push a warning to the error log.
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                        "MariaDB Galera and flashback do not support binlog format: %s",
                        binlog_format_names[var->save_result.ulonglong_value]);
    /*
      We allow setting up binlog_format other then ROW for session scope when
      wsrep/flasback is enabled.This is done because of 2 reasons
      1. User might want to run pt-table-checksum.
      2. SuperUser knows what is doing :-)

      For refrence:- MDEV-7322
    */
    if (var->type == OPT_GLOBAL)
    {
      if (WSREP(thd))
        WSREP_ERROR("MariaDB Galera does not support binlog format: %s",
                    binlog_format_names[var->save_result.ulonglong_value]);
      else
        my_error(ER_FLASHBACK_NOT_SUPPORTED,MYF(0),"binlog_format",
                 binlog_format_names[var->save_result.ulonglong_value]);
      return true;
    }
  }

  if (var->type == OPT_GLOBAL)
    return false;

  /*
     If RBR and open temporary tables, their CREATE TABLE may not be in the
     binlog, so we can't toggle to SBR in this connection.

     If binlog_format=MIXED, there are open temporary tables, and an unsafe
     statement is executed, then subsequent statements are logged in row
     format and hence changes to temporary tables may be lost. So we forbid
     switching @@SESSION.binlog_format from MIXED to STATEMENT when there are
     open temp tables and we are logging in row format.
  */
  if (thd->has_thd_temporary_tables() &&
      var->type == OPT_SESSION &&
      var->save_result.ulonglong_value == BINLOG_FORMAT_STMT &&
      ((thd->variables.binlog_format == BINLOG_FORMAT_MIXED &&
        thd->is_current_stmt_binlog_format_row()) ||
       thd->variables.binlog_format == BINLOG_FORMAT_ROW))
  {
    my_error(ER_TEMP_TABLE_PREVENTS_SWITCH_OUT_OF_RBR, MYF(0));
    return true;
  }

  if (unlikely(error_if_in_trans_or_substatement(thd,
                                                 ER_STORED_FUNCTION_PREVENTS_SWITCH_BINLOG_FORMAT,
                                                 ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_BINLOG_FORMAT)))
    return true;

  return false;
}

static bool fix_binlog_format_after_update(sys_var *self, THD *thd,
                                           enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->reset_current_stmt_binlog_format_row();
  return false;
}

static Sys_var_on_access<Sys_var_enum,
                         PRIV_SET_SYSTEM_VAR_BINLOG_FORMAT,
                         PRIV_SET_SYSTEM_VAR_BINLOG_FORMAT>
Sys_binlog_format(
       "binlog_format", "What form of binary logging the master will "
       "use: either ROW for row-based binary logging, STATEMENT "
       "for statement-based binary logging, or MIXED. MIXED is statement-"
       "based binary logging except for those statements where only row-"
       "based is correct: those which involve user-defined functions (i.e. "
       "UDFs) or the UUID() function; for those, row-based binary logging is "
       "automatically used.",
       SESSION_VAR(binlog_format), CMD_LINE(REQUIRED_ARG, OPT_BINLOG_FORMAT),
       binlog_format_names, DEFAULT(BINLOG_FORMAT_MIXED),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(binlog_format_check),
       ON_UPDATE(fix_binlog_format_after_update));

static bool binlog_direct_check(sys_var *self, THD *thd, set_var *var)
{
  if (var->type == OPT_GLOBAL)
    return false;

  if (unlikely(error_if_in_trans_or_substatement(thd,
                                                 ER_STORED_FUNCTION_PREVENTS_SWITCH_BINLOG_DIRECT,
                                                 ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_BINLOG_DIRECT)))
     return true;

  return false;
}

static Sys_var_on_access<Sys_var_mybool,
                PRIV_SET_SYSTEM_VAR_BINLOG_DIRECT_NON_TRANSACTIONAL_UPDATES,
                PRIV_SET_SYSTEM_VAR_BINLOG_DIRECT_NON_TRANSACTIONAL_UPDATES>
Sys_binlog_direct(
       "binlog_direct_non_transactional_updates",
       "Causes updates to non-transactional engines using statement format to "
       "be written directly to binary log. Before using this option make sure "
       "that there are no dependencies between transactional and "
       "non-transactional tables such as in the statement INSERT INTO t_myisam "
       "SELECT * FROM t_innodb; otherwise, slaves may diverge from the master.",
       SESSION_VAR(binlog_direct_non_trans_update),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(binlog_direct_check));


static Sys_var_mybool Sys_explicit_defaults_for_timestamp(
       "explicit_defaults_for_timestamp",
       "This option causes CREATE TABLE to create all TIMESTAMP columns "
       "as NULL with DEFAULT NULL attribute, Without this option, "
       "TIMESTAMP columns are NOT NULL and have implicit DEFAULT clauses.",
       READ_ONLY GLOBAL_VAR(opt_explicit_defaults_for_timestamp),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));


static Sys_var_ulonglong Sys_bulk_insert_buff_size(
       "bulk_insert_buffer_size", "Size of tree cache used in bulk "
       "insert optimisation. Note that this is a limit per thread!",
       SESSION_VAR(bulk_insert_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, SIZE_T_MAX), DEFAULT(8192*1024), BLOCK_SIZE(1));

static Sys_var_charptr_fscs Sys_character_sets_dir(
       "character_sets_dir", "Directory where character sets are",
       READ_ONLY GLOBAL_VAR(charsets_dir), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static bool check_not_null(sys_var *self, THD *thd, set_var *var)
{
  return var->value && var->value->is_null();
}
static bool check_charset(sys_var *self, THD *thd, set_var *var)
{
  if (!var->value)
    return false;

  char buff[STRING_BUFFER_USUAL_SIZE];
  if (var->value->result_type() == STRING_RESULT)
  {
    String str(buff, sizeof(buff), system_charset_info), *res;
    if (!(res= var->value->val_str(&str)))
      var->save_result.ptr= NULL;
    else
    {
      ErrConvString err(res); /* Get utf8 '\0' terminated string */
      if (!(var->save_result.ptr= get_charset_by_csname(err.ptr(),
                                                         MY_CS_PRIMARY,
                                                         MYF(0))) &&
          !(var->save_result.ptr= get_old_charset_by_name(err.ptr())))
      {
        my_error(ER_UNKNOWN_CHARACTER_SET, MYF(0), err.ptr());
        return true;
      }
    }
  }
  else // INT_RESULT
  {
    int csno= (int)var->value->val_int();
    if (!(var->save_result.ptr= get_charset(csno, MYF(0))))
    {
      my_error(ER_UNKNOWN_CHARACTER_SET, MYF(0), llstr(csno, buff));
      return true;
    }
  }
  return false;
}
static bool check_charset_not_null(sys_var *self, THD *thd, set_var *var)
{
  return check_charset(self, thd, var) || check_not_null(self, thd, var);
}
static Sys_var_struct Sys_character_set_system(
       "character_set_system", "The character set used by the server "
       "for storing identifiers",
       READ_ONLY GLOBAL_VAR(system_charset_info), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(0));

static Sys_var_struct Sys_character_set_server(
       "character_set_server", "The default character set",
       SESSION_VAR(collation_server), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_charset_not_null));

static bool check_charset_db(sys_var *self, THD *thd, set_var *var)
{
  if (check_charset_not_null(self, thd, var))
    return true;
  if (!var->value) // = DEFAULT
    var->save_result.ptr= thd->db_charset;
  return false;
}
static Sys_var_struct Sys_character_set_database(
       "character_set_database",
       "The character set used by the default database",
       SESSION_VAR(collation_database), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_charset_db));

static bool check_cs_client(sys_var *self, THD *thd, set_var *var)
{
  if (check_charset_not_null(self, thd, var))
    return true;

  // Currently, UCS-2 cannot be used as a client character set
  if (!is_supported_parser_charset((CHARSET_INFO *)(var->save_result.ptr)))
    return true;

  return false;
}
static bool fix_thd_charset(sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->update_charset();
  return false;
}
static Sys_var_struct Sys_character_set_client(
       "character_set_client", "The character set for statements "
       "that arrive from the client",
       NO_SET_STMT SESSION_VAR(character_set_client), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_cs_client),
       ON_UPDATE(fix_thd_charset));
// for check changing
export sys_var *Sys_character_set_client_ptr= &Sys_character_set_client;

static Sys_var_struct Sys_character_set_connection(
       "character_set_connection", "The character set used for "
       "literals that do not have a character set introducer and for "
       "number-to-string conversion",
       NO_SET_STMT SESSION_VAR(collation_connection), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_charset_not_null),
       ON_UPDATE(fix_thd_charset));
// for check changing
export sys_var *Sys_character_set_connection_ptr= &Sys_character_set_connection;

static Sys_var_struct Sys_character_set_results(
       "character_set_results", "The character set used for returning "
       "query results to the client",
       SESSION_VAR(character_set_results), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_charset));
// for check changing
export sys_var *Sys_character_set_results_ptr= &Sys_character_set_results;

static Sys_var_struct Sys_character_set_filesystem(
       "character_set_filesystem", "The filesystem character set",
       NO_SET_STMT SESSION_VAR(character_set_filesystem), NO_CMD_LINE,
       offsetof(CHARSET_INFO, csname), DEFAULT(&character_set_filesystem),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_charset_not_null),
       ON_UPDATE(fix_thd_charset));

static const char *completion_type_names[]= {"NO_CHAIN", "CHAIN", "RELEASE", 0};
static Sys_var_enum Sys_completion_type(
       "completion_type", "The transaction completion type",
       SESSION_VAR(completion_type), CMD_LINE(REQUIRED_ARG),
       completion_type_names, DEFAULT(0));

static bool check_collation_not_null(sys_var *self, THD *thd, set_var *var)
{
  if (!var->value)
    return false;

  char buff[STRING_BUFFER_USUAL_SIZE];
  if (var->value->result_type() == STRING_RESULT)
  {
    String str(buff, sizeof(buff), system_charset_info), *res;
    if (!(res= var->value->val_str(&str)))
      var->save_result.ptr= NULL;
    else
    {
      ErrConvString err(res); /* Get utf8 '\0'-terminated string */
      if (!(var->save_result.ptr= get_charset_by_name(err.ptr(), MYF(0))))
      {
        my_error(ER_UNKNOWN_COLLATION, MYF(0), err.ptr());
        return true;
      }
    }
  }
  else // INT_RESULT
  {
    int csno= (int)var->value->val_int();
    if (!(var->save_result.ptr= get_charset(csno, MYF(0))))
    {
      my_error(ER_UNKNOWN_COLLATION, MYF(0), llstr(csno, buff));
      return true;
    }
  }
  return check_not_null(self, thd, var);
}
static Sys_var_struct Sys_collation_connection(
       "collation_connection", "The collation of the connection "
       "character set",
       NO_SET_STMT SESSION_VAR(collation_connection), NO_CMD_LINE,
       offsetof(CHARSET_INFO, name), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_collation_not_null),
       ON_UPDATE(fix_thd_charset));

static bool check_collation_db(sys_var *self, THD *thd, set_var *var)
{
  if (check_collation_not_null(self, thd, var))
    return true;
  if (!var->value) // = DEFAULT
    var->save_result.ptr= thd->db_charset;
  return false;
}
static Sys_var_struct Sys_collation_database(
       "collation_database", "The collation of the database "
       "character set",
       SESSION_VAR(collation_database), NO_CMD_LINE,
       offsetof(CHARSET_INFO, name), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_collation_db));

static Sys_var_struct Sys_collation_server(
       "collation_server", "The server default collation",
       SESSION_VAR(collation_server), NO_CMD_LINE,
       offsetof(CHARSET_INFO, name), DEFAULT(&default_charset_info),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_collation_not_null));

static Sys_var_uint Sys_column_compression_threshold(
       "column_compression_threshold",
       "Minimum column data length eligible for compression",
       SESSION_VAR(column_compression_threshold), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(100), BLOCK_SIZE(1));

static Sys_var_uint Sys_column_compression_zlib_level(
       "column_compression_zlib_level",
       "zlib compression level (1 gives best speed, 9 gives best compression)",
       SESSION_VAR(column_compression_zlib_level), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 9), DEFAULT(6), BLOCK_SIZE(1));

/*
  Note that names must correspond to zlib strategy definition. So that we can
  pass column_compression_zlib_strategy directly to deflateInit2().
*/
static const char *column_compression_zlib_strategy_names[]=
{ "DEFAULT_STRATEGY", "FILTERED", "HUFFMAN_ONLY", "RLE", "FIXED", 0 };

static Sys_var_enum Sys_column_compression_zlib_strategy(
       "column_compression_zlib_strategy",
       "The strategy parameter is used to tune the compression algorithm. Use "
       "the value DEFAULT_STRATEGY for normal data, FILTERED for data produced "
       "by a filter (or predictor), HUFFMAN_ONLY to force Huffman encoding "
       "only (no string match), or RLE to limit match distances to one "
       "(run-length encoding). Filtered data consists mostly of small values "
       "with a somewhat random distribution. In this case, the compression "
       "algorithm is tuned to compress them better. The effect of FILTERED is "
       "to force more Huffman coding and less string matching; it is somewhat "
       "intermediate between DEFAULT_STRATEGY and HUFFMAN_ONLY. RLE is "
       "designed to be almost as fast as HUFFMAN_ONLY, but give better "
       "compression for PNG image data. The strategy parameter only affects "
       "the compression ratio but not the correctness of the compressed output "
       "even if it is not set appropriately. FIXED prevents the use of dynamic "
       "Huffman codes, allowing for a simpler decoder for special "
       "applications.",
       SESSION_VAR(column_compression_zlib_strategy), CMD_LINE(REQUIRED_ARG),
       column_compression_zlib_strategy_names, DEFAULT(0));

static Sys_var_mybool Sys_column_compression_zlib_wrap(
       "column_compression_zlib_wrap",
       "Generate zlib header and trailer and compute adler32 check value. "
       "It can be used with storage engines that don't provide data integrity "
       "verification to detect data corruption.",
       SESSION_VAR(column_compression_zlib_wrap), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static const char *concurrent_insert_names[]= {"NEVER", "AUTO", "ALWAYS", 0};
static Sys_var_enum Sys_concurrent_insert(
       "concurrent_insert", "Use concurrent insert with MyISAM",
       GLOBAL_VAR(myisam_concurrent_insert), CMD_LINE(OPT_ARG),
       concurrent_insert_names, DEFAULT(1));

static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_CONNECT_TIMEOUT>
Sys_connect_timeout(
       "connect_timeout",
       "The number of seconds the mysqld server is waiting for a connect "
       "packet before responding with 'Bad handshake'",
       GLOBAL_VAR(connect_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(2, LONG_TIMEOUT), DEFAULT(CONNECT_TIMEOUT), BLOCK_SIZE(1));

static Sys_var_charptr_fscs Sys_datadir(
       "datadir", "Path to the database root directory",
       READ_ONLY GLOBAL_VAR(mysql_real_data_home_ptr),
       CMD_LINE(REQUIRED_ARG, 'h'), DEFAULT(mysql_real_data_home));

#ifndef DBUG_OFF
static Sys_var_dbug Sys_dbug(
       "debug", "Built-in DBUG debugger", sys_var::SESSION,
       CMD_LINE(OPT_ARG, '#'), DEFAULT(""), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_has_super), ON_UPDATE(0),
       DEPRECATED("'@@debug_dbug'")); // since 5.5.37

static Sys_var_dbug Sys_debug_dbug(
       "debug_dbug", "Built-in DBUG debugger", sys_var::SESSION,
       CMD_LINE(OPT_ARG, '#'), DEFAULT(""), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_has_super));
#endif

/**
  @todo
    When updating myisam_delay_key_write, we should do a 'flush tables'
    of all MyISAM tables to ensure that they are reopen with the
    new attribute.
*/
export bool fix_delay_key_write(sys_var *self, THD *thd, enum_var_type type)
{
  switch (delay_key_write_options) {
  case DELAY_KEY_WRITE_NONE:
    myisam_delay_key_write=0;
    ha_open_options&= ~HA_OPEN_DELAY_KEY_WRITE;
    break;
  case DELAY_KEY_WRITE_ON:
    myisam_delay_key_write=1;
    ha_open_options&= ~HA_OPEN_DELAY_KEY_WRITE;
    break;
  case DELAY_KEY_WRITE_ALL:
    myisam_delay_key_write=1;
    ha_open_options|= HA_OPEN_DELAY_KEY_WRITE;
    break;
  }
#ifdef WITH_ARIA_STORAGE_ENGINE
  maria_delay_key_write= myisam_delay_key_write;
#endif
  return false;
}
static const char *delay_key_write_names[]= { "OFF", "ON", "ALL", NullS };
static Sys_var_enum Sys_delay_key_write(
       "delay_key_write", "Specifies how MyISAM tables handles CREATE "
       "TABLE DELAY_KEY_WRITE. If set to ON, the default, any DELAY KEY "
       "WRITEs are honored. The key buffer is then flushed only when the "
       "table closes, speeding up writes. MyISAM tables should be "
       "automatically checked upon startup in this case, and "
       "--external locking should not be used, as it can lead to index "
       "corruption. If set to OFF, DELAY KEY WRITEs are ignored, while if "
       "set to ALL, all new opened tables are treated as if created with "
       "DELAY KEY WRITEs enabled.",
       GLOBAL_VAR(delay_key_write_options), CMD_LINE(OPT_ARG),
       delay_key_write_names, DEFAULT(DELAY_KEY_WRITE_ON),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_delay_key_write));

static Sys_var_ulong Sys_delayed_insert_limit(
       "delayed_insert_limit",
       "After inserting delayed_insert_limit rows, the INSERT DELAYED "
       "handler will check if there are any SELECT statements pending. "
       "If so, it allows these to execute before continuing.",
       GLOBAL_VAR(delayed_insert_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(DELAYED_LIMIT), BLOCK_SIZE(1));

static Sys_var_ulong Sys_delayed_insert_timeout(
       "delayed_insert_timeout",
       "How long a INSERT DELAYED thread should wait for INSERT statements "
       "before terminating",
       GLOBAL_VAR(delayed_insert_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, LONG_TIMEOUT), DEFAULT(DELAYED_WAIT_TIMEOUT),
       BLOCK_SIZE(1));

static Sys_var_ulong Sys_delayed_queue_size(
       "delayed_queue_size",
       "What size queue (in rows) should be allocated for handling INSERT "
       "DELAYED. If the queue becomes full, any client that does INSERT "
       "DELAYED will wait until there is room in the queue again",
       GLOBAL_VAR(delayed_queue_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(DELAYED_QUEUE_SIZE), BLOCK_SIZE(1));

#ifdef HAVE_EVENT_SCHEDULER
static const char *event_scheduler_names[]= { "OFF", "ON", "DISABLED",
                                              "ORIGINAL", NullS };
static bool event_scheduler_check(sys_var *self, THD *thd, set_var *var)
{
  if (Events::opt_event_scheduler == Events::EVENTS_DISABLED)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--event-scheduler=DISABLED or --skip-grant-tables");
    return true;
  }
  /* DISABLED is only accepted on the command line */
  if (var->save_result.ulonglong_value == Events::EVENTS_DISABLED)
    return true;
  return false;
}

static bool event_scheduler_update(sys_var *self, THD *thd, enum_var_type type)
{
  int err_no= 0;
  bool ret;
  uint opt_event_scheduler_value= Events::opt_event_scheduler;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  /*
    Events::start() is heavyweight. In particular it creates a new THD,
    which takes LOCK_global_system_variables internally.
    Thus we have to release it here.
    We need to re-take it before returning, though.

    Note that since we release LOCK_global_system_variables before calling
    start/stop, there is a possibility that the server variable
    can become out of sync with the real event scheduler state.

    This can happen with two concurrent statments if the first gets
    interrupted after start/stop but before retaking
    LOCK_global_system_variables. However, this problem should be quite
    rare and it's difficult to avoid it without opening up possibilities
    for deadlocks. See bug#51160.
  */

  /* EVENTS_ORIGINAL means we should revert back to the startup state */
  if (opt_event_scheduler_value == Events::EVENTS_ORIGINAL)
  {
    opt_event_scheduler_value= Events::opt_event_scheduler=
      Events::startup_state;
  }
 
  /*
    If the scheduler was not properly inited (because of wrong system tables),
    try to init it again. This is needed for mysql_upgrade to work properly if
    the event tables where upgraded.
  */
  if (!Events::inited && (Events::init(thd, 0) || !Events::inited))
    ret= 1;
  else
    ret= opt_event_scheduler_value == Events::EVENTS_ON ?
      Events::start(&err_no) :
      Events::stop();
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (ret)
  {
    Events::opt_event_scheduler= Events::EVENTS_OFF;
    my_error(ER_EVENT_SET_VAR_ERROR, MYF(0), err_no);
  }
  return ret;
}

static Sys_var_enum Sys_event_scheduler(
       "event_scheduler", "Enable the event scheduler. Possible values are "
       "ON, OFF, and DISABLED (keep the event scheduler completely "
       "deactivated, it cannot be activated run-time)",
       GLOBAL_VAR(Events::opt_event_scheduler), CMD_LINE(OPT_ARG),
       event_scheduler_names, DEFAULT(Events::EVENTS_OFF),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(event_scheduler_check), ON_UPDATE(event_scheduler_update));
#endif

static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_EXPIRE_LOGS_DAYS>
Sys_expire_logs_days(
       "expire_logs_days",
       "If non-zero, binary logs will be purged after expire_logs_days "
       "days; possible purges happen at startup and at binary log rotation",
       GLOBAL_VAR(expire_logs_days),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 99), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_mybool Sys_flush(
       "flush", "Flush MyISAM tables to disk between SQL commands",
       GLOBAL_VAR(myisam_flush),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_ulong Sys_flush_time(
       "flush_time",
       "A dedicated thread is created to flush all tables at the "
       "given interval",
       GLOBAL_VAR(flush_time),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, LONG_TIMEOUT),
       DEFAULT(0), BLOCK_SIZE(1));

static bool check_ftb_syntax(sys_var *self, THD *thd, set_var *var)
{
  return ft_boolean_check_syntax_string((uchar*)
                      (var->save_result.string_value.str));
}
static bool query_cache_flush(sys_var *self, THD *thd, enum_var_type type)
{
#ifdef HAVE_QUERY_CACHE
  query_cache.flush();
#endif /* HAVE_QUERY_CACHE */
  return false;
}
/// @todo make SESSION_VAR (usability enhancement and a fix for a race condition)
static Sys_var_charptr Sys_ft_boolean_syntax(
       "ft_boolean_syntax", "List of operators for "
       "MATCH ... AGAINST ( ... IN BOOLEAN MODE)",
       GLOBAL_VAR(ft_boolean_syntax),
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(DEFAULT_FTB_SYNTAX), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(check_ftb_syntax), ON_UPDATE(query_cache_flush));

static Sys_var_ulong Sys_ft_max_word_len(
       "ft_max_word_len",
       "The maximum length of the word to be included in a FULLTEXT index. "
       "Note: FULLTEXT indexes must be rebuilt after changing this variable",
       READ_ONLY GLOBAL_VAR(ft_max_word_len), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(10, HA_FT_MAXCHARLEN), DEFAULT(HA_FT_MAXCHARLEN),
       BLOCK_SIZE(1));

static Sys_var_ulong Sys_ft_min_word_len(
       "ft_min_word_len",
       "The minimum length of the word to be included in a FULLTEXT index. "
       "Note: FULLTEXT indexes must be rebuilt after changing this variable",
       READ_ONLY GLOBAL_VAR(ft_min_word_len), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, HA_FT_MAXCHARLEN), DEFAULT(4), BLOCK_SIZE(1));

/// @todo make it an updatable SESSION_VAR
static Sys_var_ulong Sys_ft_query_expansion_limit(
       "ft_query_expansion_limit",
       "Number of best matches to use for query expansion",
       READ_ONLY GLOBAL_VAR(ft_query_expansion_limit),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 1000), DEFAULT(20), BLOCK_SIZE(1));

static Sys_var_charptr_fscs Sys_ft_stopword_file(
       "ft_stopword_file",
       "Use stopwords from this file instead of built-in list",
       READ_ONLY GLOBAL_VAR(ft_stopword_file), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_mybool Sys_ignore_builtin_innodb(
       "ignore_builtin_innodb",
       "Disable initialization of builtin InnoDB plugin",
       READ_ONLY GLOBAL_VAR(opt_ignore_builtin_innodb),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static bool check_init_string(sys_var *self, THD *thd, set_var *var)
{
  if (var->save_result.string_value.str == 0)
  {
    var->save_result.string_value.str= const_cast<char*>("");
    var->save_result.string_value.length= 0;
  }
  return false;
}
static PolyLock_rwlock PLock_sys_init_connect(&LOCK_sys_init_connect);

static Sys_var_on_access_global<Sys_var_lexstring,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_INIT_CONNECT>
Sys_init_connect(
       "init_connect", "Command(s) that are executed for each "
       "new connection (unless the user has SUPER privilege)",
       GLOBAL_VAR(opt_init_connect), CMD_LINE(REQUIRED_ARG),
       DEFAULT(""), &PLock_sys_init_connect, NOT_IN_BINLOG,
       ON_CHECK(check_init_string));

#ifdef HAVE_REPLICATION
static bool check_master_connection(sys_var *self, THD *thd, set_var *var)
{
  LEX_CSTRING tmp;
  tmp.str= var->save_result.string_value.str;
  tmp.length= var->save_result.string_value.length;
  if (!tmp.str || check_master_connection_name(&tmp))
    return true;

  return false;
}

static Sys_var_session_lexstring Sys_default_master_connection(
       "default_master_connection",
       "Master connection to use for all slave variables and slave commands",
       SESSION_ONLY(default_master_connection),
       NO_CMD_LINE,
       DEFAULT(""), MAX_CONNECTION_NAME, ON_CHECK(check_master_connection));
#endif

static Sys_var_charptr_fscs Sys_init_file(
       "init_file", "Read SQL commands from this file at startup",
       READ_ONLY GLOBAL_VAR(opt_init_file),
#ifdef DISABLE_GRANT_OPTIONS
       NO_CMD_LINE,
#else
       CMD_LINE(REQUIRED_ARG),
#endif
       DEFAULT(0));

static PolyLock_rwlock PLock_sys_init_slave(&LOCK_sys_init_slave);
static Sys_var_on_access_global<Sys_var_lexstring,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_INIT_SLAVE>
Sys_init_slave(
       "init_slave", "Command(s) that are executed by a slave server "
       "each time the SQL thread starts", GLOBAL_VAR(opt_init_slave),
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(""), &PLock_sys_init_slave,
       NOT_IN_BINLOG, ON_CHECK(check_init_string));

static Sys_var_ulong Sys_interactive_timeout(
       "interactive_timeout",
       "The number of seconds the server waits for activity on an interactive "
       "connection before closing it",
       NO_SET_STMT SESSION_VAR(net_interactive_timeout),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, LONG_TIMEOUT), DEFAULT(NET_WAIT_TIMEOUT), BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_join_buffer_size(
       "join_buffer_size",
       "The size of the buffer that is used for joins",
       SESSION_VAR(join_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(128, SIZE_T_MAX), DEFAULT(256*1024), BLOCK_SIZE(128));

static Sys_var_keycache Sys_key_buffer_size(
       "key_buffer_size", "The size of the buffer used for "
       "index blocks for MyISAM tables. Increase this to get better index "
       "handling (for all reads and multiple writes) to as much as you can "
       "afford",
       KEYCACHE_VAR(param_buff_size),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_BUFFER_SIZE),
       VALID_RANGE(0, SIZE_T_MAX), DEFAULT(KEY_CACHE_SIZE),
       BLOCK_SIZE(IO_SIZE), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_buffer_size));

static Sys_var_keycache Sys_key_cache_block_size(
       "key_cache_block_size", "The default size of key cache blocks",
       KEYCACHE_VAR(param_block_size),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_CACHE_BLOCK_SIZE),
       VALID_RANGE(512, 1024*16), DEFAULT(KEY_CACHE_BLOCK_SIZE),
       BLOCK_SIZE(512), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(resize_keycache));

static Sys_var_keycache Sys_key_cache_division_limit(
       "key_cache_division_limit",
       "The minimum percentage of warm blocks in key cache",
       KEYCACHE_VAR(param_division_limit),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_CACHE_DIVISION_LIMIT),
       VALID_RANGE(1, 100), DEFAULT(100),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(change_keycache_param));

static Sys_var_keycache Sys_key_cache_age_threshold(
       "key_cache_age_threshold", "This characterizes the number of "
       "hits a hot block has to be untouched until it is considered aged "
       "enough to be downgraded to a warm block. This specifies the "
       "percentage ratio of that number of hits to the total number of "
       "blocks in key cache",
       KEYCACHE_VAR(param_age_threshold),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_CACHE_AGE_THRESHOLD),
       VALID_RANGE(100, UINT_MAX), DEFAULT(300),
       BLOCK_SIZE(100), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(change_keycache_param));

static Sys_var_keycache Sys_key_cache_file_hash_size(
       "key_cache_file_hash_size",
       "Number of hash buckets for open and changed files.  If you have a lot of MyISAM "
       "files open you should increase this for faster flush of changes. A good "
       "value is probably 1/10 of number of possible open MyISAM files.",
       KEYCACHE_VAR(changed_blocks_hash_size),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_CACHE_CHANGED_BLOCKS_HASH_SIZE),
       VALID_RANGE(128, 16384), DEFAULT(512),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(resize_keycache));

static Sys_var_mybool Sys_large_files_support(
       "large_files_support",
       "Whether mysqld was compiled with options for large file support",
       READ_ONLY GLOBAL_VAR(opt_large_files),
       CMD_LINE_HELP_ONLY, DEFAULT(sizeof(my_off_t) > 4));

static Sys_var_uint Sys_large_page_size(
       "large_page_size",
       "Previously showed the size of large memory pages, unused since "
       "multiple page size support was added",
       READ_ONLY GLOBAL_VAR(opt_large_page_size), NO_CMD_LINE,
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED(""));

static Sys_var_mybool Sys_large_pages(
       "large_pages", "Enable support for large pages",
       READ_ONLY GLOBAL_VAR(opt_large_pages),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_charptr_fscs Sys_language(
       "lc_messages_dir", "Directory where error messages are",
       READ_ONLY GLOBAL_VAR(lc_messages_dir_ptr), CMD_LINE(REQUIRED_ARG, 'L'),
       DEFAULT(0));

static Sys_var_mybool Sys_local_infile(
       "local_infile", "Enable LOAD DATA LOCAL INFILE",
       GLOBAL_VAR(opt_local_infile), CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static Sys_var_ulong Sys_lock_wait_timeout(
       "lock_wait_timeout",
       "Timeout in seconds to wait for a lock before returning an error.",
       SESSION_VAR(lock_wait_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, LONG_TIMEOUT), DEFAULT(24 * 60 * 60), BLOCK_SIZE(1));

#ifdef HAVE_MLOCKALL
static Sys_var_mybool Sys_locked_in_memory(
       "locked_in_memory",
       "Whether mysqld was locked in memory with --memlock",
       READ_ONLY GLOBAL_VAR(locked_in_memory), NO_CMD_LINE, DEFAULT(FALSE));
#endif

/* this says NO_CMD_LINE, as command-line option takes a string, not a bool */
static Sys_var_mybool Sys_log_bin(
       "log_bin", "Whether the binary log is enabled",
       READ_ONLY GLOBAL_VAR(opt_bin_log), NO_CMD_LINE, DEFAULT(FALSE));

static Sys_var_on_access_global<Sys_var_mybool,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_COMPRESS>
Sys_log_bin_compress(
  "log_bin_compress", "Whether the binary log can be compressed",
  GLOBAL_VAR(opt_bin_log_compress), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

/* the min length is 10, means that Begin/Commit/Rollback would never be compressed!   */
static Sys_var_on_access_global<Sys_var_uint,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_COMPRESS_MIN_LEN>
Sys_log_bin_compress_min_len(
  "log_bin_compress_min_len",
  "Minimum length of sql statement(in statement mode) or record(in row mode)"
  "that can be compressed.",
  GLOBAL_VAR(opt_bin_log_compress_min_len),
  CMD_LINE(OPT_ARG), VALID_RANGE(10, 1024), DEFAULT(256), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_mybool,
                    PRIV_SET_SYSTEM_GLOBAL_VAR_LOG_BIN_TRUST_FUNCTION_CREATORS>
Sys_trust_function_creators(
       "log_bin_trust_function_creators",
       "If set to FALSE (the default), then when --log-bin is used, creation "
       "of a stored function (or trigger) is allowed only to users having the "
       "SUPER privilege and only if this stored function (trigger) may not "
       "break binary logging. Note that if ALL connections to this server "
       "ALWAYS use row-based binary logging, the security issues do not "
       "exist and the binary logging cannot break, so you can safely set "
       "this to TRUE",
       GLOBAL_VAR(trust_function_creators),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_charptr_fscs Sys_log_error(
       "log_error",
       "Log errors to file (instead of stdout).  If file name is not specified "
       "then 'datadir'/'log-basename'.err or the 'pid-file' path with extension "
       ".err is used",
       READ_ONLY GLOBAL_VAR(log_error_file_ptr),
       CMD_LINE(OPT_ARG, OPT_LOG_ERROR),
       DEFAULT(disabled_my_option));

static Sys_var_bit Sys_log_queries_not_using_indexes(
       "log_queries_not_using_indexes",
       "Log queries that are executed without benefit of any index to the "
       "slow log if it is open. Same as log_slow_filter='not_using_index'",
       SESSION_VAR(log_slow_filter), CMD_LINE(OPT_ARG), QPLAN_NOT_USING_INDEX,
       DEFAULT(FALSE));

static Sys_var_bit Sys_log_slow_admin_statements(
       "log_slow_admin_statements",
       "Log slow OPTIMIZE, ANALYZE, ALTER and other administrative statements "
       "to the slow log if it is open.  Resets or sets the option 'admin' in "
       "log_slow_disabled_statements",
       SESSION_VAR(log_slow_disabled_statements),
       CMD_LINE(OPT_ARG), REVERSE(LOG_SLOW_DISABLE_ADMIN), DEFAULT(TRUE));

static Sys_var_bit Sys_log_slow_slave_statements(
       "log_slow_slave_statements",
       "Log slow statements executed by slave thread to the slow log if it is "
       "open. Resets or sets the option 'slave' in "
       "log_slow_disabled_statements",
       SESSION_VAR(log_slow_disabled_statements),
       CMD_LINE(OPT_ARG), REVERSE(LOG_SLOW_DISABLE_SLAVE), DEFAULT(TRUE));

static Sys_var_ulong Sys_log_warnings(
       "log_warnings",
       "Log some not critical warnings to the general log file."
       "Value can be between 0 and 11. Higher values mean more verbosity",
       SESSION_VAR(log_warnings),
       CMD_LINE(OPT_ARG, 'W'),
       VALID_RANGE(0, UINT_MAX), DEFAULT(2), BLOCK_SIZE(1));

static bool update_cached_long_query_time(sys_var *self, THD *thd,
                                          enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->variables.long_query_time=
      double2ulonglong(thd->variables.long_query_time_double * 1e6);
  else
    global_system_variables.long_query_time=
      double2ulonglong(global_system_variables.long_query_time_double * 1e6);
  return false;
}

static Sys_var_double Sys_long_query_time(
       "long_query_time",
       "Log all queries that have taken more than long_query_time seconds "
       "to execute to the slow query log file. The argument will be treated "
       "as a decimal value with microsecond precision",
       SESSION_VAR(long_query_time_double),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, LONG_TIMEOUT), DEFAULT(10),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_cached_long_query_time));


static bool update_cached_max_statement_time(sys_var *self, THD *thd,
                                         enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->variables.max_statement_time=
      double2ulonglong(thd->variables.max_statement_time_double * 1e6);
  else
    global_system_variables.max_statement_time=
      double2ulonglong(global_system_variables.max_statement_time_double * 1e6);
  return false;
}

static Sys_var_double Sys_max_statement_time(
       "max_statement_time",
       "A query that has taken more than max_statement_time seconds "
       "will be aborted. The argument will be treated as a decimal value "
       "with microsecond precision. A value of 0 (default) means no timeout",
       SESSION_VAR(max_statement_time_double),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, LONG_TIMEOUT), DEFAULT(0),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_cached_max_statement_time));

static bool fix_low_prio_updates(sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->update_lock_default= (thd->variables.low_priority_updates ?
                               TL_WRITE_LOW_PRIORITY : TL_WRITE);
  else
    thr_upgraded_concurrent_insert_lock=
      (global_system_variables.low_priority_updates ?
       TL_WRITE_LOW_PRIORITY : TL_WRITE);
  return false;
}
static Sys_var_mybool Sys_low_priority_updates(
       "low_priority_updates",
       "INSERT/DELETE/UPDATE has lower priority than selects",
       SESSION_VAR(low_priority_updates),
       CMD_LINE(OPT_ARG),
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_low_prio_updates));

static Sys_var_mybool Sys_lower_case_file_system(
       "lower_case_file_system",
       "Case sensitivity of file names on the file system where the "
       "data directory is located",
       READ_ONLY GLOBAL_VAR(lower_case_file_system),
       CMD_LINE_HELP_ONLY,
       DEFAULT(FALSE));

static Sys_var_uint Sys_lower_case_table_names(
       "lower_case_table_names",
       "If set to 1 table names are stored in lowercase on disk and table "
       "names will be case-insensitive.  Should be set to 2 if you are using "
       "a case insensitive file system",
       READ_ONLY GLOBAL_VAR(lower_case_table_names),
       CMD_LINE(OPT_ARG, OPT_LOWER_CASE_TABLE_NAMES),
       VALID_RANGE(0, 2),
#ifdef FN_NO_CASE_SENSE
    DEFAULT(1),
#else
    DEFAULT(0),
#endif
       BLOCK_SIZE(1));

static bool session_readonly(sys_var *self, THD *thd, set_var *var)
{
  if (var->type == OPT_GLOBAL)
    return false;
  my_error(ER_VARIABLE_IS_READONLY, MYF(0), "SESSION",
           self->name.str, "GLOBAL");
  return true;
}

static bool check_max_allowed_packet(sys_var *self, THD *thd,  set_var *var)
{
  longlong val;
  if (session_readonly(self, thd, var))
    return true;

  val= var->save_result.ulonglong_value;
  if (val < (longlong) global_system_variables.net_buffer_length)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_OPTION_BELOW_LIMIT,
                        ER_THD(thd, WARN_OPTION_BELOW_LIMIT),
                        "max_allowed_packet", "net_buffer_length");
  }
  return false;
}


static Sys_var_ulong Sys_max_allowed_packet(
       "max_allowed_packet",
       "Max packet length to send to or receive from the server",
       SESSION_VAR(max_allowed_packet), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, 1024*1024*1024), DEFAULT(16*1024*1024),
       BLOCK_SIZE(1024), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_max_allowed_packet));

static Sys_var_on_access_global<Sys_var_ulong,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_MAX_ALLOWED_PACKET>
Sys_slave_max_allowed_packet(
       "slave_max_allowed_packet",
       "The maximum packet length to sent successfully from the master to slave.",
       GLOBAL_VAR(slave_max_allowed_packet), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, MAX_MAX_ALLOWED_PACKET),
       DEFAULT(MAX_MAX_ALLOWED_PACKET), BLOCK_SIZE(1024));

static Sys_var_on_access_global<Sys_var_ulonglong,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_CACHE_SIZE>
Sys_max_binlog_cache_size(
       "max_binlog_cache_size",
       "Sets the total size of the transactional cache",
       GLOBAL_VAR(max_binlog_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE, SIZE_T_MAX),
       DEFAULT((SIZE_T_MAX/IO_SIZE)*IO_SIZE), BLOCK_SIZE(IO_SIZE));

static Sys_var_on_access_global<Sys_var_ulonglong,
                       PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_STMT_CACHE_SIZE>
Sys_max_binlog_stmt_cache_size(
       "max_binlog_stmt_cache_size",
       "Sets the total size of the statement cache",
       GLOBAL_VAR(max_binlog_stmt_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE, SIZE_T_MAX),
       DEFAULT((SIZE_T_MAX/IO_SIZE)*IO_SIZE), BLOCK_SIZE(IO_SIZE));

static bool fix_max_binlog_size(sys_var *self, THD *thd, enum_var_type type)
{
  mysql_bin_log.set_max_size(max_binlog_size);
  return false;
}
static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_BINLOG_SIZE>
Sys_max_binlog_size(
       "max_binlog_size",
       "Binary log will be rotated automatically when the size exceeds this "
       "value.",
       GLOBAL_VAR(max_binlog_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE, 1024*1024L*1024L), DEFAULT(1024*1024L*1024L),
       BLOCK_SIZE(IO_SIZE), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_max_binlog_size));

static bool fix_max_connections(sys_var *self, THD *thd, enum_var_type type)
{
#ifndef EMBEDDED_LIBRARY
  resize_thr_alarm(max_connections + extra_max_connections +
                   global_system_variables.max_insert_delayed_threads + 10);
#endif
  return false;
}

// Default max_connections of 151 is larger than Apache's default max
// children, to avoid "too many connections" error in a common setup
static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_CONNECTIONS>
Sys_max_connections(
       "max_connections", "The number of simultaneous clients allowed",
       PARSED_EARLY GLOBAL_VAR(max_connections), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(10, 100000),
       DEFAULT(MAX_CONNECTIONS_DEFAULT), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(fix_max_connections));

static Sys_var_uint Sys_default_password_lifetime(
       "default_password_lifetime",
       "This defines the global password expiration policy. 0 means "
       "automatic password expiration is disabled. If the value is a "
       "positive integer N, the passwords must be changed every N days. This "
       "behavior can be overridden using the password expiration options in "
       "ALTER USER.",
       GLOBAL_VAR(default_password_lifetime), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_mybool,
                     PRIV_SET_SYSTEM_GLOBAL_VAR_DISCONNECT_ON_EXPIRED_PASSWORD>
Sys_disconnect_on_expired_password(
       "disconnect_on_expired_password",
       "This variable controls how the server handles clients that are not "
       "aware of the sandbox mode. If enabled, the server disconnects the "
       "client, otherwise the server puts the client in a sandbox mode.",
       GLOBAL_VAR(disconnect_on_expired_password), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_CONNECT_ERRORS>
Sys_max_connect_errors(
       "max_connect_errors",
       "If there is more than this number of interrupted connections from "
       "a host this host will be blocked from further connections",
       GLOBAL_VAR(max_connect_errors), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(MAX_CONNECT_ERRORS),
       BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_MAX_PASSWORD_ERRORS>
Sys_max_password_errors(
       "max_password_errors",
       "If there is more than this number of failed connect attempts "
       "due to invalid password, user will be blocked from further connections until FLUSH_PRIVILEGES.",
       GLOBAL_VAR(max_password_errors), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(UINT_MAX),
       BLOCK_SIZE(1));

static Sys_var_uint Sys_max_digest_length(
       "max_digest_length", "Maximum length considered for digest text.",
       READ_ONLY GLOBAL_VAR(max_digest_length),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 1024 * 1024), DEFAULT(1024), BLOCK_SIZE(1));

static bool check_max_delayed_threads(sys_var *self, THD *thd, set_var *var)
{
  return var->type != OPT_GLOBAL &&
         var->save_result.ulonglong_value != 0 &&
         var->save_result.ulonglong_value !=
                           global_system_variables.max_insert_delayed_threads;
}

// Alias for max_delayed_threads
static Sys_var_ulong Sys_max_insert_delayed_threads(
       "max_insert_delayed_threads",
       "Don't start more than this number of threads to handle INSERT "
       "DELAYED statements. If set to zero INSERT DELAYED will be not used",
       SESSION_VAR(max_insert_delayed_threads),
       NO_CMD_LINE, VALID_RANGE(0, 16384), DEFAULT(20),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_max_delayed_threads), ON_UPDATE(fix_max_connections));

static Sys_var_ulong Sys_max_delayed_threads(
       "max_delayed_threads",
       "Don't start more than this number of threads to handle INSERT "
       "DELAYED statements. If set to zero INSERT DELAYED will be not used",
       SESSION_VAR(max_insert_delayed_threads),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 16384), DEFAULT(20),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_max_delayed_threads), ON_UPDATE(fix_max_connections));

static Sys_var_ulong Sys_max_error_count(
       "max_error_count",
       "Max number of errors/warnings to store for a statement",
       SESSION_VAR(max_error_count), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 65535), DEFAULT(DEFAULT_ERROR_COUNT), BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_max_heap_table_size(
       "max_heap_table_size",
       "Don't allow creation of heap tables bigger than this",
       SESSION_VAR(max_heap_table_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(16384, SIZE_T_MAX), DEFAULT(16*1024*1024),
       BLOCK_SIZE(1024));

static ulong mdl_locks_cache_size;
static Sys_var_ulong Sys_metadata_locks_cache_size(
       "metadata_locks_cache_size", "Unused",
       READ_ONLY GLOBAL_VAR(mdl_locks_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 1024*1024), DEFAULT(1024),
       BLOCK_SIZE(1));

static ulong mdl_locks_hash_partitions;
static Sys_var_ulong Sys_metadata_locks_hash_instances(
       "metadata_locks_hash_instances", "Unused",
       READ_ONLY GLOBAL_VAR(mdl_locks_hash_partitions), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 1024), DEFAULT(8),
       BLOCK_SIZE(1));

static Sys_var_on_access_session<Sys_var_ulonglong,
                                 PRIV_SET_SYSTEM_SESSION_VAR_PSEUDO_THREAD_ID>
Sys_pseudo_thread_id(
       "pseudo_thread_id",
       "This variable is for internal server use",
       SESSION_ONLY(pseudo_thread_id),
       NO_CMD_LINE, VALID_RANGE(0, ULONGLONG_MAX), DEFAULT(0),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, IN_BINLOG);

static bool
check_gtid_domain_id(sys_var *self, THD *thd, set_var *var)
{
  if (var->type != OPT_GLOBAL &&
      error_if_in_trans_or_substatement(thd,
          ER_STORED_FUNCTION_PREVENTS_SWITCH_GTID_DOMAIN_ID_SEQ_NO,
          ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_GTID_DOMAIN_ID_SEQ_NO))
    return true;

  return false;
}


static Sys_var_on_access<Sys_var_uint,
                         PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_DOMAIN_ID,
                         PRIV_SET_SYSTEM_SESSION_VAR_GTID_DOMAIN_ID>
Sys_gtid_domain_id(
       "gtid_domain_id",
       "Used with global transaction ID to identify logically independent "
       "replication streams. When events can propagate through multiple "
       "parallel paths (for example multiple masters), each independent "
       "source server must use a distinct domain_id. For simple tree-shaped "
       "replication topologies, it can be left at its default, 0.",
       SESSION_VAR(gtid_domain_id),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, UINT_MAX32), DEFAULT(0),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_gtid_domain_id));


static bool check_gtid_seq_no(sys_var *self, THD *thd, set_var *var)
{
  uint32 domain_id, server_id;
  uint64 seq_no;

  if (unlikely(error_if_in_trans_or_substatement(thd,
                                                 ER_STORED_FUNCTION_PREVENTS_SWITCH_GTID_DOMAIN_ID_SEQ_NO,
                                                 ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_GTID_DOMAIN_ID_SEQ_NO)))
    return true;

  domain_id= thd->variables.gtid_domain_id;
  server_id= thd->variables.server_id;
  seq_no= (uint64)var->value->val_uint();
  DBUG_EXECUTE_IF("ignore_set_gtid_seq_no_check", return 0;);
  if (opt_gtid_strict_mode && opt_bin_log &&
      mysql_bin_log.check_strict_gtid_sequence(domain_id, server_id, seq_no))
    return true;

  return false;
}


static Sys_var_on_access_session<Sys_var_ulonglong,
                                PRIV_SET_SYSTEM_SESSION_VAR_GTID_SEQ_NO>
Sys_gtid_seq_no(
       "gtid_seq_no",
       "Internal server usage, for replication with global transaction id. "
       "When set, next event group logged to the binary log will use this "
       "sequence number, not generate a new one, thus allowing to preserve "
       "master's GTID in slave's binlog.",
       SESSION_ONLY(gtid_seq_no),
       NO_CMD_LINE, VALID_RANGE(0, ULONGLONG_MAX), DEFAULT(0),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_gtid_seq_no));


#ifdef HAVE_REPLICATION
static unsigned char opt_gtid_binlog_pos_dummy;
static Sys_var_gtid_binlog_pos Sys_gtid_binlog_pos(
       "gtid_binlog_pos", "Last GTID logged to the binary log, per replication"
       "domain",
       READ_ONLY GLOBAL_VAR(opt_gtid_binlog_pos_dummy), NO_CMD_LINE);


uchar *
Sys_var_gtid_binlog_pos::global_value_ptr(THD *thd, const LEX_CSTRING *base)
{
  char buf[128];
  String str(buf, sizeof(buf), system_charset_info);
  char *p;

  str.length(0);
  if ((opt_bin_log && mysql_bin_log.append_state_pos(&str)) ||
      !(p= thd->strmake(str.ptr(), str.length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }

  return (uchar *)p;
}


static unsigned char opt_gtid_current_pos_dummy;
static Sys_var_gtid_current_pos Sys_gtid_current_pos(
       "gtid_current_pos", "Current GTID position of the server. Per "
       "replication domain, this is either the last GTID replicated by a "
       "slave thread, or the GTID logged to the binary log, whichever is "
       "most recent.",
       READ_ONLY GLOBAL_VAR(opt_gtid_current_pos_dummy), NO_CMD_LINE);


uchar *
Sys_var_gtid_current_pos::global_value_ptr(THD *thd, const LEX_CSTRING *base)
{
  String str;
  char *p;

  str.length(0);
  if (rpl_append_gtid_state(&str, true) ||
      !(p= thd->strmake(str.ptr(), str.length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }

  return (uchar *)p;
}


bool
Sys_var_gtid_slave_pos::do_check(THD *thd, set_var *var)
{
  String str, *res;

  DBUG_ASSERT(var->type == OPT_GLOBAL);

  if (rpl_load_gtid_slave_state(thd))
  {
    my_error(ER_CANNOT_LOAD_SLAVE_GTID_STATE, MYF(0), "mysql",
             rpl_gtid_slave_state_table_name.str);
    return true;
  }

  if (give_error_if_slave_running(0))
    return true;
  if (!(res= var->value->val_str(&str)))
    return true;
  if (thd->in_active_multi_stmt_transaction())
  {
    my_error(ER_CANT_DO_THIS_DURING_AN_TRANSACTION, MYF(0));
    return true;
  }
  if (rpl_gtid_pos_check(thd, &((*res)[0]), res->length()))
    return true;

  if (!(var->save_result.string_value.str=
        thd->strmake(res->ptr(), res->length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  var->save_result.string_value.length= res->length();
  return false;
}


bool
Sys_var_gtid_slave_pos::global_update(THD *thd, set_var *var)
{
  bool err;

  DBUG_ASSERT(var->type == OPT_GLOBAL);

  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }

  mysql_mutex_unlock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_active_mi);
  if (give_error_if_slave_running(1))
    err= true;
  else
    err= rpl_gtid_pos_update(thd, var->save_result.string_value.str,
                             var->save_result.string_value.length);
  mysql_mutex_unlock(&LOCK_active_mi);
  mysql_mutex_lock(&LOCK_global_system_variables);
  return err;
}


uchar *
Sys_var_gtid_slave_pos::global_value_ptr(THD *thd, const LEX_CSTRING *base)
{
  String str;
  char *p;

  str.length(0);
  /*
    If the mysql.rpl_slave_pos table could not be loaded, then we cannot
    easily automatically try to reload it here - we may be inside a statement
    that already has tables locked and so opening more tables is problematic.

    But if the table is not loaded (eg. missing mysql_upgrade_db or some such),
    then the slave state must be empty anyway.
  */
  if ((rpl_global_gtid_slave_state->loaded &&
       rpl_append_gtid_state(&str, false)) ||
      !(p= thd->strmake(str.ptr(), str.length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }

  return (uchar *)p;
}


static unsigned char opt_gtid_slave_pos_dummy;
static Sys_var_gtid_slave_pos Sys_gtid_slave_pos(
       "gtid_slave_pos",
       "The list of global transaction IDs that were last replicated on the "
       "server, one for each replication domain.",
       GLOBAL_VAR(opt_gtid_slave_pos_dummy), NO_CMD_LINE);


static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_STRICT_MODE>
Sys_gtid_strict_mode(
       "gtid_strict_mode",
       "Enforce strict seq_no ordering of events in the binary log. Slave "
       "stops with an error if it encounters an event that would cause it to "
       "generate an out-of-order binlog if executed.",
       GLOBAL_VAR(opt_gtid_strict_mode),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));


struct gtid_binlog_state_data { rpl_gtid *list; uint32 list_len; };

bool
Sys_var_gtid_binlog_state::do_check(THD *thd, set_var *var)
{
  String str, *res;
  struct gtid_binlog_state_data *data;
  rpl_gtid *list;
  uint32 list_len;

  DBUG_ASSERT(var->type == OPT_GLOBAL);

  if (!(res= var->value->val_str(&str)))
    return true;
  if (thd->in_active_multi_stmt_transaction())
  {
    my_error(ER_CANT_DO_THIS_DURING_AN_TRANSACTION, MYF(0));
    return true;
  }
  if (!mysql_bin_log.is_open())
  {
    my_error(ER_FLUSH_MASTER_BINLOG_CLOSED, MYF(0));
    return true;
  }
  if (!mysql_bin_log.is_empty_state())
  {
    my_error(ER_BINLOG_MUST_BE_EMPTY, MYF(0));
    return true;
  }
  if (res->length() == 0)
  {
    list= NULL;
    list_len= 0;
  }
  else if (!(list= gtid_parse_string_to_list(res->ptr(), res->length(),
                                             &list_len)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0));
    return true;
  }
  if (!(data= (gtid_binlog_state_data *)my_malloc(PSI_INSTRUMENT_ME,
                                                  sizeof(*data), MYF(0))))
  {
    my_free(list);
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  data->list= list;
  data->list_len= list_len;
  var->save_result.ptr= data;
  return false;
}


bool
Sys_var_gtid_binlog_state::global_update(THD *thd, set_var *var)
{
  bool res;

  DBUG_ASSERT(var->type == OPT_GLOBAL);

  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }

  struct gtid_binlog_state_data *data=
    (struct gtid_binlog_state_data *)var->save_result.ptr;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  res= (reset_master(thd, data->list, data->list_len, 0) != 0);
  mysql_mutex_lock(&LOCK_global_system_variables);
  my_free(data->list);
  my_free(data);
  return res;
}


uchar *
Sys_var_gtid_binlog_state::global_value_ptr(THD *thd, const LEX_CSTRING *base)
{
  char buf[512];
  String str(buf, sizeof(buf), system_charset_info);
  char *p;

  str.length(0);
  if ((opt_bin_log && mysql_bin_log.append_state(&str)) ||
      !(p= thd->strmake(str.ptr(), str.length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }

  return (uchar *)p;
}


static unsigned char opt_gtid_binlog_state_dummy;
static Sys_var_gtid_binlog_state Sys_gtid_binlog_state(
       "gtid_binlog_state",
       "The internal GTID state of the binlog, used to keep track of all "
       "GTIDs ever logged to the binlog.",
       GLOBAL_VAR(opt_gtid_binlog_state_dummy), NO_CMD_LINE);


static Sys_var_last_gtid Sys_last_gtid(
       "last_gtid", "The GTID of the last commit (if binlogging was enabled), "
       "or the empty string if none.",
       READ_ONLY sys_var::ONLY_SESSION, NO_CMD_LINE);

export sys_var *Sys_last_gtid_ptr= &Sys_last_gtid; // for check changing


uchar *
Sys_var_last_gtid::session_value_ptr(THD *thd, const LEX_CSTRING *base)
{
  char buf[10+1+10+1+20+1];
  String str(buf, sizeof(buf), system_charset_info);
  char *p;
  bool first= true;

  str.length(0);
  rpl_gtid gtid= thd->get_last_commit_gtid();
  if ((gtid.seq_no > 0 &&
       rpl_slave_state_tostring_helper(&str, &gtid, &first)) ||
      !(p= thd->strmake(str.ptr(), str.length())))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }

  return (uchar *)p;
}


static Sys_var_on_access_global<Sys_var_uint,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_CLEANUP_BATCH_SIZE>
Sys_gtid_cleanup_batch_size(
       "gtid_cleanup_batch_size",
       "Normally does not need tuning. How many old rows must accumulate in "
       "the mysql.gtid_slave_pos table before a background job will be run to "
       "delete them. Can be increased to reduce number of commits if "
       "using many different engines with --gtid_pos_auto_engines, or to "
       "reduce CPU overhead if using a huge number of different "
       "gtid_domain_ids. Can be decreased to reduce number of old rows in the "
       "table.",
       GLOBAL_VAR(opt_gtid_cleanup_batch_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,2147483647), DEFAULT(64), BLOCK_SIZE(1));


static bool
check_slave_parallel_threads(sys_var *self, THD *thd, set_var *var)
{
  return give_error_if_slave_running(0);
}

static bool
fix_slave_parallel_threads(sys_var *self, THD *thd, enum_var_type type)
{
  bool err;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  err= give_error_if_slave_running(0);
  mysql_mutex_lock(&LOCK_global_system_variables);

  return err;
}


static Sys_var_on_access_global<Sys_var_ulong,
                             PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_THREADS>
Sys_slave_parallel_threads(
       "slave_parallel_threads",
       "If non-zero, number of threads to spawn to apply in parallel events "
       "on the slave that were group-committed on the master or were logged "
       "with GTID in different replication domains. Note that these threads "
       "are in addition to the IO and SQL threads, which are always created "
       "by a replication slave",
       GLOBAL_VAR(opt_slave_parallel_threads), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,16383), DEFAULT(0), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(check_slave_parallel_threads),
       ON_UPDATE(fix_slave_parallel_threads));

/* Alias for @@slave_parallel_threads to match what MySQL 5.7 uses. */
static Sys_var_on_access_global<Sys_var_ulong,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_WORKERS>
Sys_slave_parallel_workers(
       "slave_parallel_workers",
       "Alias for slave_parallel_threads",
       GLOBAL_VAR(opt_slave_parallel_threads), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,16383), DEFAULT(0), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(check_slave_parallel_threads),
       ON_UPDATE(fix_slave_parallel_threads));


static bool
check_slave_domain_parallel_threads(sys_var *self, THD *thd, set_var *var)
{
  return give_error_if_slave_running(0);
}

static bool
fix_slave_domain_parallel_threads(sys_var *self, THD *thd, enum_var_type type)
{
  bool running;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  running= give_error_if_slave_running(0);
  mysql_mutex_lock(&LOCK_global_system_variables);

  return running;
}


static Sys_var_on_access_global<Sys_var_ulong,
                       PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_DOMAIN_PARALLEL_THREADS>
Sys_slave_domain_parallel_threads(
       "slave_domain_parallel_threads",
       "Maximum number of parallel threads to use on slave for events in a "
       "single replication domain. When using multiple domains, this can be "
       "used to limit a single domain from grabbing all threads and thus "
       "stalling other domains. The default of 0 means to allow a domain to "
       "grab as many threads as it wants, up to the value of "
       "slave_parallel_threads.",
       GLOBAL_VAR(opt_slave_domain_parallel_threads), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,16383), DEFAULT(0), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(check_slave_domain_parallel_threads),
       ON_UPDATE(fix_slave_domain_parallel_threads));


static Sys_var_on_access_global<Sys_var_ulong,
                           PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_MAX_QUEUED>
Sys_slave_parallel_max_queued(
       "slave_parallel_max_queued",
       "Limit on how much memory SQL threads should use per parallel "
       "replication thread when reading ahead in the relay log looking for "
       "opportunities for parallel replication. Only used when "
       "--slave-parallel-threads > 0.",
       GLOBAL_VAR(opt_slave_parallel_max_queued), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,2147483647), DEFAULT(131072), BLOCK_SIZE(1));


bool
Sys_var_slave_parallel_mode::global_update(THD *thd, set_var *var)
{
  enum_slave_parallel_mode new_value=
    (enum_slave_parallel_mode)var->save_result.ulonglong_value;
  LEX_CSTRING *base_name= &var->base;
  Master_info *mi;
  bool res= false;

  if (!base_name->length)
    base_name= &thd->variables.default_master_connection;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_active_mi);

  mi= master_info_index->
    get_master_info(base_name, !base_name->length ?
                    Sql_condition::WARN_LEVEL_ERROR :
                    Sql_condition::WARN_LEVEL_WARN);

  if (mi)
  {
    if (mi->rli.slave_running)
    {
      my_error(ER_SLAVE_MUST_STOP, MYF(0),
               (int) mi->connection_name.length, mi->connection_name.str);
      res= true;
    }
    else
    {
      mi->parallel_mode= new_value;
      if (!base_name->length)
      {
        /* Use as default value for new connections */
        opt_slave_parallel_mode= new_value;
      }
    }
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  mysql_mutex_lock(&LOCK_global_system_variables);

  return res;
}


uchar *
Sys_var_slave_parallel_mode::global_value_ptr(THD *thd,
                                              const LEX_CSTRING *base_name)
{
  Master_info *mi;
  enum_slave_parallel_mode val=
    (enum_slave_parallel_mode)opt_slave_parallel_mode;

  if (!base_name->length)
    base_name= &thd->variables.default_master_connection;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_active_mi);

  mi= master_info_index->
    get_master_info(base_name, !base_name->length ?
                    Sql_condition::WARN_LEVEL_ERROR :
                    Sql_condition::WARN_LEVEL_WARN);
  if (mi)
    val= mi->parallel_mode;

  mysql_mutex_unlock(&LOCK_active_mi);
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (!mi)
    return 0;

  return valptr(thd, val);
}


/* The order here must match enum_slave_parallel_mode in mysqld.h. */
static const char *slave_parallel_mode_names[] = {
  "none", "minimal", "conservative", "optimistic", "aggressive", NULL
};
export TYPELIB slave_parallel_mode_typelib = {
  array_elements(slave_parallel_mode_names)-1,
  "",
  slave_parallel_mode_names,
  NULL
};

static Sys_var_on_access_global<Sys_var_slave_parallel_mode,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_PARALLEL_MODE>
Sys_slave_parallel_mode(
       "slave_parallel_mode",
       "Controls what transactions are applied in parallel when using "
       "--slave-parallel-threads. Possible values: \"optimistic\" tries to "
       "apply most transactional DML in parallel, and handles any conflicts "
       "with rollback and retry. \"conservative\" limits parallelism in an "
       "effort to avoid any conflicts. \"aggressive\" tries to maximise the "
       "parallelism, possibly at the cost of increased conflict rate. "
       "\"minimal\" only parallelizes the commit steps of transactions. "
       "\"none\" disables parallel apply completely.",
       GLOBAL_VAR(opt_slave_parallel_mode), NO_CMD_LINE,
       slave_parallel_mode_names, DEFAULT(SLAVE_PARALLEL_OPTIMISTIC));


static Sys_var_bit Sys_skip_parallel_replication(
       "skip_parallel_replication",
       "If set when a transaction is written to the binlog, parallel apply of "
       "that transaction will be avoided on a slave where slave_parallel_mode "
       "is not \"aggressive\". Can be used to avoid unnecessary rollback and "
       "retry for transactions that are likely to cause a conflict if "
       "replicated in parallel.",
       SESSION_ONLY(option_bits), NO_CMD_LINE, OPTION_RPL_SKIP_PARALLEL,
       DEFAULT(FALSE));


static bool
check_gtid_ignore_duplicates(sys_var *self, THD *thd, set_var *var)
{
  return give_error_if_slave_running(0);
}

static bool
fix_gtid_ignore_duplicates(sys_var *self, THD *thd, enum_var_type type)
{
  bool running;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  running= give_error_if_slave_running(0);
  mysql_mutex_lock(&LOCK_global_system_variables);

  return running;
}


static Sys_var_on_access_global<Sys_var_mybool,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_IGNORE_DUPLICATES>
Sys_gtid_ignore_duplicates(
       "gtid_ignore_duplicates",
       "When set, different master connections in multi-source replication are "
       "allowed to receive and process event groups with the same GTID (when "
       "using GTID mode). Only one will be applied, any others will be "
       "ignored. Within a given replication domain, just the sequence number "
       "will be used to decide whether a given GTID has been already applied; "
       "this means it is the responsibility of the user to ensure that GTID "
       "sequence numbers are strictly increasing.",
       GLOBAL_VAR(opt_gtid_ignore_duplicates), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(check_gtid_ignore_duplicates),
       ON_UPDATE(fix_gtid_ignore_duplicates));
#endif


static Sys_var_on_access_global<Sys_var_ulong,
                           PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_COMMIT_WAIT_COUNT>
Sys_binlog_commit_wait_count(
       "binlog_commit_wait_count",
       "If non-zero, binlog write will wait at most binlog_commit_wait_usec "
       "microseconds for at least this many commits to queue up for group "
       "commit to the binlog. This can reduce I/O on the binlog and provide "
       "increased opportunity for parallel apply on the slave, but too high "
       "a value will decrease commit throughput.",
       GLOBAL_VAR(opt_binlog_commit_wait_count), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, ULONG_MAX), DEFAULT(0), BLOCK_SIZE(1));


static Sys_var_on_access_global<Sys_var_ulong,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_COMMIT_WAIT_USEC>
Sys_binlog_commit_wait_usec(
       "binlog_commit_wait_usec",
       "Maximum time, in microseconds, to wait for more commits to queue up "
       "for binlog group commit. Only takes effect if the value of "
       "binlog_commit_wait_count is non-zero.",
       GLOBAL_VAR(opt_binlog_commit_wait_usec), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, ULONG_MAX), DEFAULT(100000), BLOCK_SIZE(1));


static bool fix_max_join_size(sys_var *self, THD *thd, enum_var_type type)
{
  SV *sv= type == OPT_GLOBAL ? &global_system_variables : &thd->variables;
  if (sv->max_join_size == HA_POS_ERROR)
    sv->option_bits|= OPTION_BIG_SELECTS;
  else
    sv->option_bits&= ~OPTION_BIG_SELECTS;
  return false;
}
static Sys_var_harows Sys_max_join_size(
       "max_join_size",
       "Joins that are probably going to read more than max_join_size "
       "records return an error",
       SESSION_VAR(max_join_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, HA_POS_ERROR), DEFAULT(HA_POS_ERROR), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_max_join_size));

static Sys_var_ulong Sys_max_seeks_for_key(
       "max_seeks_for_key",
       "Limit assumed max number of seeks when looking up rows based on a key",
       SESSION_VAR(max_seeks_for_key), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(UINT_MAX), BLOCK_SIZE(1));

static Sys_var_ulong Sys_max_length_for_sort_data(
       "max_length_for_sort_data",
       "Max number of bytes in sorted records",
       SESSION_VAR(max_length_for_sort_data), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(4, 8192*1024L), DEFAULT(1024), BLOCK_SIZE(1));

static PolyLock_mutex PLock_prepared_stmt_count(&LOCK_prepared_stmt_count);
static Sys_var_uint Sys_max_prepared_stmt_count(
       "max_prepared_stmt_count",
       "Maximum number of prepared statements in the server",
       GLOBAL_VAR(max_prepared_stmt_count), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX32), DEFAULT(16382), BLOCK_SIZE(1),
       &PLock_prepared_stmt_count);

static Sys_var_ulong Sys_max_recursive_iterations(
       "max_recursive_iterations",
       "Maximum number of iterations when executing recursive queries",
       SESSION_VAR(max_recursive_iterations), CMD_LINE(OPT_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(UINT_MAX), BLOCK_SIZE(1));

static Sys_var_ulong Sys_max_sort_length(
       "max_sort_length",
       "The number of bytes to use when sorting BLOB or TEXT values (only "
       "the first max_sort_length bytes of each value are used; the rest "
       "are ignored)",
       SESSION_VAR(max_sort_length), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(4, 8192*1024L), DEFAULT(1024), BLOCK_SIZE(1));

static Sys_var_ulong Sys_max_sp_recursion_depth(
       "max_sp_recursion_depth",
       "Maximum stored procedure recursion depth",
       SESSION_VAR(max_sp_recursion_depth), CMD_LINE(OPT_ARG),
       VALID_RANGE(0, 255), DEFAULT(0), BLOCK_SIZE(1));


static bool if_checking_enabled(sys_var *self, THD *thd,  set_var *var)
{
  if (session_readonly(self, thd, var))
    return true;
  
  if (!max_user_connections_checking)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--max-user-connections=0");
    return true;
  }

  return false;
}
// non-standard session_value_ptr() here
static Sys_var_max_user_conn Sys_max_user_connections(
       "max_user_connections",
       "The maximum number of active connections for a single user "
       "(0 = no limit)",
       SESSION_VAR(max_user_connections), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(-1, INT_MAX), DEFAULT(0), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(if_checking_enabled));

static Sys_var_ulong Sys_max_tmp_tables(
       "max_tmp_tables", "Unused, will be removed.",
       SESSION_VAR(max_tmp_tables), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(32), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("")); // since 10.1.2

static Sys_var_ulong Sys_max_write_lock_count(
       "max_write_lock_count",
       "After this many write locks, allow some read locks to run in between",
       GLOBAL_VAR(max_write_lock_count), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(UINT_MAX), BLOCK_SIZE(1));

static Sys_var_ulong Sys_min_examined_row_limit(
       "min_examined_row_limit",
       "Don't write queries to slow log that examine fewer rows "
       "than that",
       SESSION_VAR(min_examined_row_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1));

#ifdef _WIN32
static Sys_var_mybool Sys_named_pipe(
       "named_pipe", "Enable the named pipe (NT)",
       READ_ONLY GLOBAL_VAR(opt_enable_named_pipe), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));
#endif


static bool check_net_buffer_length(sys_var *self, THD *thd,  set_var *var)
{
  longlong val;
  if (session_readonly(self, thd, var))
    return true;

  val= var->save_result.ulonglong_value;
  if (val > (longlong) global_system_variables.max_allowed_packet)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_OPTION_BELOW_LIMIT,
                        ER_THD(thd, WARN_OPTION_BELOW_LIMIT),
                        "max_allowed_packet", "net_buffer_length");
  }
  return false;
}
static Sys_var_ulong Sys_net_buffer_length(
       "net_buffer_length",
       "Buffer length for TCP/IP and socket communication",
       SESSION_VAR(net_buffer_length), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, 1024*1024), DEFAULT(16384), BLOCK_SIZE(1024),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_net_buffer_length));

static bool fix_net_read_timeout(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    my_net_set_read_timeout(&thd->net, thd->variables.net_read_timeout);
  return false;
}
static Sys_var_ulong Sys_net_read_timeout(
       "net_read_timeout",
       "Number of seconds to wait for more data from a connection before "
       "aborting the read",
       SESSION_VAR(net_read_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, LONG_TIMEOUT), DEFAULT(NET_READ_TIMEOUT), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_net_read_timeout));

static bool fix_net_write_timeout(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    my_net_set_write_timeout(&thd->net, thd->variables.net_write_timeout);
  return false;
}
static Sys_var_ulong Sys_net_write_timeout(
       "net_write_timeout",
       "Number of seconds to wait for a block to be written to a connection "
       "before aborting the write",
       SESSION_VAR(net_write_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, LONG_TIMEOUT), DEFAULT(NET_WRITE_TIMEOUT), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_net_write_timeout));

static bool fix_net_retry_count(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    thd->net.retry_count=thd->variables.net_retry_count;
  return false;
}
static Sys_var_ulong Sys_net_retry_count(
       "net_retry_count",
       "If a read on a communication port is interrupted, retry this "
       "many times before giving up",
       SESSION_VAR(net_retry_count), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(MYSQLD_NET_RETRY_COUNT),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_net_retry_count));

static Sys_var_mybool Sys_old_mode(
       "old", "Use compatible behavior from previous MariaDB version. See also --old-mode",
       SESSION_VAR(old_mode), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static const char *alter_algorithm_modes[]= {"DEFAULT", "COPY", "INPLACE",
"NOCOPY", "INSTANT", NULL};

static Sys_var_enum Sys_alter_algorithm(
	"alter_algorithm", "Specify the alter table algorithm",
	SESSION_VAR(alter_algorithm), CMD_LINE(OPT_ARG),
	alter_algorithm_modes, DEFAULT(0));

static Sys_var_enum Sys_old_alter_table(
       "old_alter_table", "Alias for alter_algorithm. "
       "Deprecated. Use --alter-algorithm instead.",
       SESSION_VAR(alter_algorithm), CMD_LINE(OPT_ARG),
       alter_algorithm_modes, DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("'@@alter_algorithm'")); // Since 10.5.1

static bool check_old_passwords(sys_var *self, THD *thd, set_var *var)
{
  return mysql_user_table_is_in_short_password_format;
}
static Sys_var_mybool Sys_old_passwords(
       "old_passwords",
       "Use old password encryption method (needed for 4.0 and older clients)",
       SESSION_VAR(old_passwords), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_old_passwords));
export sys_var *Sys_old_passwords_ptr= &Sys_old_passwords; // for sql_acl.cc

static Sys_var_ulong Sys_open_files_limit(
       "open_files_limit",
       "If this is not 0, then mysqld will use this value to reserve file "
       "descriptors to use with setrlimit(). If this value is 0 or autoset "
       "then mysqld will reserve max_connections*5 or max_connections + "
       "table_cache*2 (whichever is larger) number of file descriptors",
       AUTO_SET READ_ONLY GLOBAL_VAR(open_files_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, OS_FILE_LIMIT), DEFAULT(0), BLOCK_SIZE(1));

/// @todo change to enum
static Sys_var_ulong Sys_optimizer_prune_level(
       "optimizer_prune_level",
       "Controls the heuristic(s) applied during query optimization to prune "
       "less-promising partial plans from the optimizer search space. "
       "Meaning: 0 - do not apply any heuristic, thus perform exhaustive "
       "search; 1 - prune plans based on number of retrieved rows",
       SESSION_VAR(optimizer_prune_level), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 1), DEFAULT(1), BLOCK_SIZE(1));

static Sys_var_ulong Sys_optimizer_selectivity_sampling_limit(
       "optimizer_selectivity_sampling_limit",
       "Controls number of record samples to check condition selectivity",
       SESSION_VAR(optimizer_selectivity_sampling_limit),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(SELECTIVITY_SAMPLING_THRESHOLD, UINT_MAX),
       DEFAULT(SELECTIVITY_SAMPLING_LIMIT), BLOCK_SIZE(1));

static Sys_var_ulong Sys_optimizer_use_condition_selectivity(
       "optimizer_use_condition_selectivity",
       "Controls selectivity of which conditions the optimizer takes into "
       "account to calculate cardinality of a partial join when it searches "
       "for the best execution plan "
       "Meaning: "
       "1 - use selectivity of index backed range conditions to calculate "
       "the cardinality of a partial join if the last joined table is "
       "accessed by full table scan or an index scan, "
       "2 - use selectivity of index backed range conditions to calculate "
       "the cardinality of a partial join in any case, "
       "3 - additionally always use selectivity of range conditions that are "
       "not backed by any index to calculate the cardinality of a partial join, "
       "4 - use histograms to calculate selectivity of range conditions that "
       "are not backed by any index to calculate the cardinality of "
       "a partial join."
       "5 - additionally use selectivity of certain non-range predicates "
       "calculated on record samples",
       SESSION_VAR(optimizer_use_condition_selectivity), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 5), DEFAULT(4), BLOCK_SIZE(1));

static Sys_var_ulong Sys_optimizer_search_depth(
       "optimizer_search_depth",
       "Maximum depth of search performed by the query optimizer. Values "
       "larger than the number of relations in a query result in better "
       "query plans, but take longer to compile a query. Values smaller "
       "than the number of tables in a relation result in faster "
       "optimization, but may produce very bad query plans. If set to 0, "
       "the system will automatically pick a reasonable value.",
       SESSION_VAR(optimizer_search_depth), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, MAX_TABLES+1), DEFAULT(MAX_TABLES+1), BLOCK_SIZE(1));

/* this is used in the sigsegv handler */
export const char *optimizer_switch_names[]=
{
  "index_merge","index_merge_union","index_merge_sort_union",
  "index_merge_intersection","index_merge_sort_intersection",
  "engine_condition_pushdown",
  "index_condition_pushdown",
  "derived_merge", "derived_with_keys",
  "firstmatch","loosescan","materialization","in_to_exists","semijoin",
  "partial_match_rowid_merge",
  "partial_match_table_scan",
  "subquery_cache",
  "mrr",
  "mrr_cost_based",
  "mrr_sort_keys",
  "outer_join_with_cache",
  "semijoin_with_cache",
  "join_cache_incremental",
  "join_cache_hashed",
  "join_cache_bka",
  "optimize_join_buffer_size",
  "table_elimination",
  "extended_keys",
  "exists_to_in",
  "orderby_uses_equalities",
  "condition_pushdown_for_derived",
  "split_materialized",
  "condition_pushdown_for_subquery",
  "rowid_filter",
  "condition_pushdown_from_having",
  "not_null_range_scan",
  "default", 
  NullS
};
static bool fix_optimizer_switch(sys_var *self, THD *thd,
                                 enum_var_type type)
{
  SV *sv= (type == OPT_GLOBAL) ? &global_system_variables : &thd->variables;
  if (sv->optimizer_switch & deprecated_ENGINE_CONDITION_PUSHDOWN)
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_DEPRECATED_SYNTAX_NO_REPLACEMENT,
                        ER_THD(thd, ER_WARN_DEPRECATED_SYNTAX_NO_REPLACEMENT),
                        "engine_condition_pushdown=on"); // since 10.1.1
  return false;
}
static Sys_var_flagset Sys_optimizer_switch(
       "optimizer_switch",
       "Fine-tune the optimizer behavior",
       SESSION_VAR(optimizer_switch), CMD_LINE(REQUIRED_ARG),
       optimizer_switch_names, DEFAULT(OPTIMIZER_SWITCH_DEFAULT),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_optimizer_switch));

static Sys_var_flagset Sys_optimizer_trace(
    "optimizer_trace",
    "Controls tracing of the Optimizer:"
    " optimizer_trace=option=val[,option=val...], where option is one of"
    " {enabled}"
    " and val is one of {on, off, default}",
    SESSION_VAR(optimizer_trace), CMD_LINE(REQUIRED_ARG),
    Opt_trace_context::flag_names, DEFAULT(Opt_trace_context::FLAG_DEFAULT));
    // @see set_var::is_var_optimizer_trace()
export sys_var *Sys_optimizer_trace_ptr = &Sys_optimizer_trace;

static Sys_var_ulong Sys_optimizer_trace_max_mem_size(
    "optimizer_trace_max_mem_size",
    "Maximum allowed size of an optimizer trace",
    SESSION_VAR(optimizer_trace_max_mem_size), CMD_LINE(REQUIRED_ARG),
    VALID_RANGE(0, ULONG_MAX), DEFAULT(1024 * 1024), BLOCK_SIZE(1));

static Sys_var_charptr_fscs Sys_pid_file(
       "pid_file", "Pid file used by safe_mysqld",
       READ_ONLY GLOBAL_VAR(pidfile_name_ptr), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_plugin_dir(
       "plugin_dir", "Directory for plugins",
       READ_ONLY GLOBAL_VAR(opt_plugin_dir_ptr), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_uint Sys_port(
       "port",
       "Port number to use for connection or 0 to default to, "
       "my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
       "/etc/services, "
#endif
       "built-in default (" STRINGIFY_ARG(MYSQL_PORT) "), whatever comes first",
       READ_ONLY GLOBAL_VAR(mysqld_port), CMD_LINE(REQUIRED_ARG, 'P'),
       VALID_RANGE(0, UINT_MAX32), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_ulong Sys_preload_buff_size(
       "preload_buffer_size",
       "The size of the buffer that is allocated when preloading indexes",
       SESSION_VAR(preload_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, 1024*1024*1024), DEFAULT(32768), BLOCK_SIZE(1));

static Sys_var_uint Sys_protocol_version(
       "protocol_version",
       "The version of the client/server protocol used by the MariaDB server",
       READ_ONLY GLOBAL_VAR(protocol_version), CMD_LINE_HELP_ONLY,
       VALID_RANGE(0, ~0U), DEFAULT(PROTOCOL_VERSION), BLOCK_SIZE(1));

static Sys_var_proxy_user Sys_proxy_user(
       "proxy_user", "The proxy user account name used when logging in");

static Sys_var_external_user Sys_exterenal_user(
       "external_user", "The external user account used when logging in");

static Sys_var_ulong Sys_read_buff_size(
       "read_buffer_size",
       "Each thread that does a sequential scan allocates a buffer of "
       "this size for each table it scans. If you do many sequential scans, "
       "you may want to increase this value",
       SESSION_VAR(read_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE*2, INT_MAX32), DEFAULT(128*1024),
       BLOCK_SIZE(IO_SIZE));

static bool check_read_only(sys_var *self, THD *thd, set_var *var)
{
  /* Prevent self dead-lock */
  if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction() ||
      thd->current_backup_stage != BACKUP_FINISHED)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    return true;
  }
  return false;
}

static bool fix_read_only(sys_var *self, THD *thd, enum_var_type type)
{
  bool result= true;
  my_bool new_read_only= read_only; // make a copy before releasing a mutex
  DBUG_ENTER("sys_var_opt_readonly::update");

  if (read_only == FALSE || read_only == opt_readonly)
  {
    opt_readonly= read_only;
    DBUG_RETURN(false);
  }

  if (check_read_only(self, thd, 0)) // just in case
    goto end;

  if (thd->global_read_lock.is_acquired())
  {
    /*
      This connection already holds the global read lock.
      This can be the case with:
      - FLUSH TABLES WITH READ LOCK
      - SET GLOBAL READ_ONLY = 1
    */
    opt_readonly= read_only;
    DBUG_RETURN(false);
  }

  /*
    READ_ONLY=1 prevents write locks from being taken on tables and
    blocks transactions from committing. We therefore should make sure
    that no such events occur while setting the read_only variable.
    This is a 2 step process:
    [1] lock_global_read_lock()
      Prevents connections from obtaining new write locks on
      tables. Note that we can still have active rw transactions.
    [2] make_global_read_lock_block_commit()
      Prevents transactions from committing.
  */

  read_only= opt_readonly;
  mysql_mutex_unlock(&LOCK_global_system_variables);

  if (thd->global_read_lock.lock_global_read_lock(thd))
    goto end_with_mutex_unlock;

  if ((result= thd->global_read_lock.make_global_read_lock_block_commit(thd)))
    goto end_with_read_lock;

  /* Change the opt_readonly system variable, safe because the lock is held */
  opt_readonly= new_read_only;
  result= false;

 end_with_read_lock:
  /* Release the lock */
  thd->global_read_lock.unlock_global_read_lock(thd);
 end_with_mutex_unlock:
  mysql_mutex_lock(&LOCK_global_system_variables);
 end:
  read_only= opt_readonly;
  DBUG_RETURN(result);
}


/**
  The read_only boolean is always equal to the opt_readonly boolean except
  during fix_read_only(); when that function is entered, opt_readonly is
  the pre-update value and read_only is the post-update value.
  fix_read_only() compares them and runs needed operations for the
  transition (especially when transitioning from false to true) and
  synchronizes both booleans in the end.
*/
static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_READ_ONLY>
Sys_readonly(
       "read_only",
       "Make all non-temporary tables read-only, with the exception for "
       "replication (slave) threads and users with the SUPER privilege",
       GLOBAL_VAR(read_only), CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_read_only), ON_UPDATE(fix_read_only));

// Small lower limit to be able to test MRR
static Sys_var_ulong Sys_read_rnd_buff_size(
       "read_rnd_buffer_size",
       "When reading rows in sorted order after a sort, the rows are read "
       "through this buffer to avoid a disk seeks",
       SESSION_VAR(read_rnd_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, INT_MAX32), DEFAULT(256*1024), BLOCK_SIZE(1));

static Sys_var_ulong Sys_div_precincrement(
       "div_precision_increment", "Precision of the result of '/' "
       "operator will be increased on that value",
       SESSION_VAR(div_precincrement), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, DECIMAL_MAX_SCALE), DEFAULT(4), BLOCK_SIZE(1));

static Sys_var_uint Sys_eq_range_index_dive_limit(
       "eq_range_index_dive_limit",
       "The optimizer will use existing index statistics instead of "
       "doing index dives for equality ranges if the number of equality "
       "ranges for the index is larger than or equal to this number. "
       "If set to 0, index dives are always used.",
       SESSION_VAR(eq_range_index_dive_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX32), DEFAULT(200),
       BLOCK_SIZE(1));

static Sys_var_ulong Sys_range_alloc_block_size(
       "range_alloc_block_size",
       "Allocation block size for storing ranges during optimization",
       SESSION_VAR(range_alloc_block_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(RANGE_ALLOC_BLOCK_SIZE, UINT_MAX),
       DEFAULT(RANGE_ALLOC_BLOCK_SIZE), BLOCK_SIZE(1024));

static bool fix_thd_mem_root(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    reset_root_defaults(thd->mem_root,
                        thd->variables.query_alloc_block_size,
                        thd->variables.query_prealloc_size);
  return false;
}
static Sys_var_ulong Sys_query_alloc_block_size(
       "query_alloc_block_size",
       "Allocation block size for query parsing and execution",
       SESSION_VAR(query_alloc_block_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, UINT_MAX), DEFAULT(QUERY_ALLOC_BLOCK_SIZE),
       BLOCK_SIZE(1024), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_thd_mem_root));

static Sys_var_ulong Sys_query_prealloc_size(
       "query_prealloc_size",
       "Persistent buffer for query parsing and execution",
       SESSION_VAR(query_prealloc_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, UINT_MAX),
       DEFAULT(QUERY_ALLOC_PREALLOC_SIZE),
       BLOCK_SIZE(1024), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_thd_mem_root));


// this has to be NO_CMD_LINE as the command-line option has a different name
static Sys_var_mybool Sys_skip_external_locking(
       "skip_external_locking", "Don't use system (external) locking",
       READ_ONLY GLOBAL_VAR(my_disable_locking), NO_CMD_LINE, DEFAULT(TRUE));

static Sys_var_mybool Sys_skip_networking(
       "skip_networking", "Don't allow connection with TCP/IP",
       READ_ONLY GLOBAL_VAR(opt_disable_networking), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static Sys_var_mybool Sys_skip_name_resolve(
       "skip_name_resolve",
       "Don't resolve hostnames. All hostnames are IP's or 'localhost'.",
       READ_ONLY GLOBAL_VAR(opt_skip_name_resolve),
       CMD_LINE(OPT_ARG, OPT_SKIP_RESOLVE),
       DEFAULT(FALSE));

static Sys_var_mybool Sys_skip_show_database(
       "skip_show_database", "Don't allow 'SHOW DATABASE' commands",
       READ_ONLY GLOBAL_VAR(opt_skip_show_db), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static Sys_var_charptr_fscs Sys_socket(
       "socket", "Socket file to use for connection",
       READ_ONLY GLOBAL_VAR(mysqld_unix_port), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_ulonglong Sys_thread_stack(
       "thread_stack", "The stack size for each thread",
       READ_ONLY GLOBAL_VAR(my_thread_stack_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(128*1024, ULONGLONG_MAX), DEFAULT(DEFAULT_THREAD_STACK),
       BLOCK_SIZE(1024));

static Sys_var_charptr_fscs Sys_tmpdir(
       "tmpdir", "Path for temporary files. Several paths may "
       "be specified, separated by a "
#if defined(__WIN__)
       "semicolon (;)"
#else
       "colon (:)"
#endif
       ", in this case they are used in a round-robin fashion",
       READ_ONLY GLOBAL_VAR(opt_mysql_tmpdir), CMD_LINE(REQUIRED_ARG, 't'),
       DEFAULT(0));

static bool fix_trans_mem_root(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    reset_root_defaults(&thd->transaction->mem_root,
                        thd->variables.trans_alloc_block_size,
                        thd->variables.trans_prealloc_size);
  return false;
}
static Sys_var_ulong Sys_trans_alloc_block_size(
       "transaction_alloc_block_size",
       "Allocation block size for transactions to be stored in binary log",
       SESSION_VAR(trans_alloc_block_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, 128 * 1024 * 1024), DEFAULT(TRANS_ALLOC_BLOCK_SIZE),
       BLOCK_SIZE(1024), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_trans_mem_root));

static Sys_var_ulong Sys_trans_prealloc_size(
       "transaction_prealloc_size",
       "Persistent buffer for transactions to be stored in binary log",
       SESSION_VAR(trans_prealloc_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, 128 * 1024 * 1024), DEFAULT(TRANS_ALLOC_PREALLOC_SIZE),
       BLOCK_SIZE(1024), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_trans_mem_root));

static const char *thread_handling_names[]=
{
  "one-thread-per-connection", "no-threads",
#ifdef HAVE_POOL_OF_THREADS
  "pool-of-threads",
#endif
  0
};

#if defined (_WIN32) && defined (HAVE_POOL_OF_THREADS)
/* Windows is using OS threadpool, so we're pretty sure it works well */
#define DEFAULT_THREAD_HANDLING 2
#else
#define DEFAULT_THREAD_HANDLING 0
#endif

static Sys_var_enum Sys_thread_handling(
       "thread_handling",
       "Define threads usage for handling queries",
       READ_ONLY GLOBAL_VAR(thread_handling), CMD_LINE(REQUIRED_ARG),
       thread_handling_names, 
       DEFAULT(DEFAULT_THREAD_HANDLING)
 );

#ifdef HAVE_QUERY_CACHE
static bool fix_query_cache_size(sys_var *self, THD *thd, enum_var_type type)
{
  size_t new_cache_size= query_cache.resize((size_t)query_cache_size);
  /*
     Note: query_cache_size is a global variable reflecting the
     requested cache size. See also query_cache_size_arg
  */
  if (query_cache_size != new_cache_size)
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WARN_QC_RESIZE, ER_THD(thd, ER_WARN_QC_RESIZE),
                        query_cache_size, (ulong)new_cache_size);

  query_cache_size= new_cache_size;

  return false;
}

static bool fix_query_cache_limit(sys_var *self, THD *thd, enum_var_type type)
{
  query_cache.result_size_limit(query_cache_limit);
  return false;
}
static Sys_var_ulonglong Sys_query_cache_size(
       "query_cache_size",
       "The memory allocated to store results from old queries",
       GLOBAL_VAR(query_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, ULONG_MAX), DEFAULT(1024*1024), BLOCK_SIZE(1024),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, NULL,
       ON_UPDATE(fix_query_cache_size));

static Sys_var_ulong Sys_query_cache_limit(
       "query_cache_limit",
       "Don't cache results that are bigger than this",
       GLOBAL_VAR(query_cache_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(1024*1024), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_query_cache_limit));

static bool fix_qcache_min_res_unit(sys_var *self, THD *thd, enum_var_type type)
{
  query_cache_min_res_unit=
    (ulong)query_cache.set_min_res_unit(query_cache_min_res_unit);
  return false;
}
static Sys_var_ulong Sys_query_cache_min_res_unit(
       "query_cache_min_res_unit",
       "The minimum size for blocks allocated by the query cache",
       GLOBAL_VAR(query_cache_min_res_unit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(QUERY_CACHE_MIN_RESULT_DATA_SIZE),
       BLOCK_SIZE(8), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_qcache_min_res_unit));

static const char *query_cache_type_names[]= { "OFF", "ON", "DEMAND", 0 };

static bool check_query_cache_type(sys_var *self, THD *thd, set_var *var)
{
  if (query_cache.is_disable_in_progress())
  {
    my_error(ER_QUERY_CACHE_IS_DISABLED, MYF(0));
    return true;
  }

  if (var->type != OPT_GLOBAL && global_system_variables.query_cache_type == 0)
  {
    if (var->value)
    {
      if (var->save_result.ulonglong_value != 0)
      {
        my_error(ER_QUERY_CACHE_IS_GLOBALY_DISABLED, MYF(0));
        return true;
      }
    }
  }
  return false;
}


static bool fix_query_cache_type(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
    return false;

  if (global_system_variables.query_cache_type != 0 &&
      query_cache.is_disabled())
  {
    /* if disabling in progress variable will not be set */
    DBUG_ASSERT(!query_cache.is_disable_in_progress());
    /* Enable query cache because it was disabled */
    fix_query_cache_size(0, thd, type);
  }
  else if (global_system_variables.query_cache_type == 0)
    query_cache.disable_query_cache(thd);
  return false;
}
static Sys_var_enum Sys_query_cache_type(
       "query_cache_type",
       "OFF = Don't cache or retrieve results. ON = Cache all results "
       "except SELECT SQL_NO_CACHE ... queries. DEMAND = Cache only "
       "SELECT SQL_CACHE ... queries",
       NO_SET_STMT SESSION_VAR(query_cache_type), CMD_LINE(REQUIRED_ARG),
       query_cache_type_names, DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_query_cache_type),
       ON_UPDATE(fix_query_cache_type));

static Sys_var_mybool Sys_query_cache_wlock_invalidate(
       "query_cache_wlock_invalidate",
       "Invalidate queries in query cache on LOCK for write",
       SESSION_VAR(query_cache_wlock_invalidate), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));
#endif /* HAVE_QUERY_CACHE */

static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SECURE_AUTH>
Sys_secure_auth(
       "secure_auth",
       "Disallow authentication for accounts that have old (pre-4.1) "
       "passwords",
       GLOBAL_VAR(opt_secure_auth), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE));

static bool check_require_secure_transport(sys_var *self, THD *thd, set_var *var)
{
#ifndef _WIN32
  /*
    Always allow require_secure_transport to be enabled on
    Linux, because it always has Unix domain sockets that are secure:
  */
  return false;
#else
  /*
    Check SSL is enabled before turning require_secure_transport ON,
    otherwise no connections will be allowed on Windows:
  */
  if (!var->save_result.ulonglong_value)
    return false;
  if (opt_use_ssl || opt_enable_named_pipe)
    return false;
  /* reject if SSL is disabled: */
  my_error(ER_NO_SECURE_TRANSPORTS_CONFIGURED, MYF(0));
  return true;
#endif
}

static Sys_var_mybool Sys_require_secure_transport(
  "require_secure_transport",
  "When this option is enabled, connections attempted using insecure "
  "transport will be rejected. Secure transports are SSL/TLS, "
  "Unix sockets or named pipes.",
  GLOBAL_VAR(opt_require_secure_transport),
  CMD_LINE(OPT_ARG),
  DEFAULT(FALSE),
  NO_MUTEX_GUARD, NOT_IN_BINLOG,
  ON_CHECK(check_require_secure_transport), ON_UPDATE(0));

static Sys_var_charptr_fscs Sys_secure_file_priv(
       "secure_file_priv",
       "Limit LOAD DATA, SELECT ... OUTFILE, and LOAD_FILE() to files "
       "within specified directory",
       PREALLOCATED READ_ONLY GLOBAL_VAR(opt_secure_file_priv),
       CMD_LINE(REQUIRED_ARG), DEFAULT(0));

static bool fix_server_id(sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_GLOBAL)
  {
    thd->variables.server_id= global_system_variables.server_id;
    /*
      Historically, server_id was a global variable that is exported to
      plugins. Now it is a session variable, and lives in the
      global_system_variables struct, but we still need to export the
      value for reading to plugins for backwards compatibility reasons.
    */
    ::server_id= global_system_variables.server_id;
  }
  return false;
}
static Sys_var_on_access<Sys_var_ulong,
                         PRIV_SET_SYSTEM_GLOBAL_VAR_SERVER_ID,
                         PRIV_SET_SYSTEM_SESSION_VAR_SERVER_ID>
Sys_server_id(
       "server_id",
       "Uniquely identifies the server instance in the community of "
       "replication partners",
       SESSION_VAR(server_id), CMD_LINE(REQUIRED_ARG, OPT_SERVER_ID),
       VALID_RANGE(1, UINT_MAX32), DEFAULT(1), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(fix_server_id));

static Sys_var_on_access_global<Sys_var_mybool,
                          PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_COMPRESSED_PROTOCOL>
Sys_slave_compressed_protocol(
       "slave_compressed_protocol",
       "Use compression on master/slave protocol",
       GLOBAL_VAR(opt_slave_compressed_protocol), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

#ifdef HAVE_REPLICATION
static const char *slave_exec_mode_names[]= {"STRICT", "IDEMPOTENT", 0};
static Sys_var_on_access_global<Sys_var_enum,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_EXEC_MODE>
Slave_exec_mode(
       "slave_exec_mode",
       "How replication events should be executed. Legal values "
       "are STRICT (default) and IDEMPOTENT. In IDEMPOTENT mode, "
       "replication will not stop for operations that are idempotent. "
       "For example, in row based replication attempts to delete rows that "
       "doesn't exist will be ignored. "
       "In STRICT mode, replication will stop on any unexpected difference "
       "between the master and the slave.",
       GLOBAL_VAR(slave_exec_mode_options), CMD_LINE(REQUIRED_ARG),
       slave_exec_mode_names, DEFAULT(SLAVE_EXEC_MODE_STRICT));

static Sys_var_on_access_global<Sys_var_enum,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_DDL_EXEC_MODE>
Slave_ddl_exec_mode(
       "slave_ddl_exec_mode",
       "How replication events should be executed. Legal values "
       "are STRICT and IDEMPOTENT (default). In IDEMPOTENT mode, "
       "replication will not stop for DDL operations that are idempotent. "
       "This means that CREATE TABLE is treated as CREATE TABLE OR REPLACE and "
       "DROP TABLE is treated as DROP TABLE IF EXISTS.",
       GLOBAL_VAR(slave_ddl_exec_mode_options), CMD_LINE(REQUIRED_ARG),
       slave_exec_mode_names, DEFAULT(SLAVE_EXEC_MODE_IDEMPOTENT));

static const char *slave_run_triggers_for_rbr_names[]=
  {"NO", "YES", "LOGGING", "ENFORCE", 0};
static Sys_var_on_access_global<Sys_var_enum,
                          PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_RUN_TRIGGERS_FOR_RBR>
Slave_run_triggers_for_rbr(
       "slave_run_triggers_for_rbr",
       "Modes for how triggers in row-base replication on slave side will be "
       "executed. Legal values are NO (default), YES, LOGGING and ENFORCE. NO means "
       "that trigger for RBR will not be running on slave. YES and LOGGING "
       "means that triggers will be running on slave, if there was not "
       "triggers running on the master for the statement. LOGGING also means "
       "results of that the executed triggers work will be written to "
       "the binlog. ENFORCE means that triggers will always be run on the slave, "
       "even if there are triggers on the master. ENFORCE implies LOGGING.",
       GLOBAL_VAR(slave_run_triggers_for_rbr), CMD_LINE(REQUIRED_ARG),
       slave_run_triggers_for_rbr_names,
       DEFAULT(SLAVE_RUN_TRIGGERS_FOR_RBR_NO));

static const char *slave_type_conversions_name[]= {"ALL_LOSSY", "ALL_NON_LOSSY", 0};
static Sys_var_on_access_global<Sys_var_set,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_TYPE_CONVERSIONS>
Slave_type_conversions(
       "slave_type_conversions",
       "Set of slave type conversions that are enabled."
       " If the variable is empty, no conversions are"
       " allowed and it is expected that the types match exactly",
       GLOBAL_VAR(slave_type_conversions_options), CMD_LINE(REQUIRED_ARG),
       slave_type_conversions_name,
       DEFAULT(0));

static Sys_var_on_access_global<Sys_var_mybool,
                           PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_SQL_VERIFY_CHECKSUM>
Sys_slave_sql_verify_checksum(
       "slave_sql_verify_checksum",
       "Force checksum verification of replication events after reading them "
       "from relay log. Note: Events are always checksum-verified by slave on "
       "receiving them from the network before writing them to the relay log",
       GLOBAL_VAR(opt_slave_sql_verify_checksum), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE));

static Sys_var_on_access_global<Sys_var_mybool,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_MASTER_VERIFY_CHECKSUM>
Sys_master_verify_checksum(
       "master_verify_checksum",
       "Force checksum verification of logged events in the binary log before "
       "sending them to slaves or printing them in the output of "
       "SHOW BINLOG EVENTS",
       GLOBAL_VAR(opt_master_verify_checksum), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

/* These names must match RPL_SKIP_XXX #defines in slave.h. */
static const char *replicate_events_marked_for_skip_names[]= {
  "REPLICATE", "FILTER_ON_SLAVE", "FILTER_ON_MASTER", 0
};

bool
Sys_var_replicate_events_marked_for_skip::global_update(THD *thd, set_var *var)
{
  bool result= true;                            // Assume error
  DBUG_ENTER("Sys_var_replicate_events_marked_for_skip::global_update");

  mysql_mutex_unlock(&LOCK_global_system_variables);
  if (!give_error_if_slave_running(0))
    result= Sys_var_enum::global_update(thd, var);
  mysql_mutex_lock(&LOCK_global_system_variables);
  DBUG_RETURN(result);
}

static Sys_var_on_access_global<Sys_var_replicate_events_marked_for_skip,
                   PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_EVENTS_MARKED_FOR_SKIP>
Replicate_events_marked_for_skip
   ("replicate_events_marked_for_skip",
   "Whether the slave should replicate events that were created with "
   "@@skip_replication=1 on the master. Default REPLICATE (no events are "
   "skipped). Other values are FILTER_ON_SLAVE (events will be sent by the "
   "master but ignored by the slave) and FILTER_ON_MASTER (events marked with "
   "@@skip_replication=1 will be filtered on the master and never be sent to "
   "the slave).",
   GLOBAL_VAR(opt_replicate_events_marked_for_skip), CMD_LINE(REQUIRED_ARG),
   replicate_events_marked_for_skip_names, DEFAULT(RPL_SKIP_REPLICATE));

/* new options for semisync */

static bool fix_rpl_semi_sync_master_enabled(sys_var *self, THD *thd,
                                             enum_var_type type)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  mysql_mutex_lock(&repl_semisync_master.LOCK_rpl_semi_sync_master_enabled);
  if (rpl_semi_sync_master_enabled)
  {
    if (repl_semisync_master.enable_master() != 0)
      rpl_semi_sync_master_enabled= false;
    else if (ack_receiver.start())
    {
      repl_semisync_master.disable_master();
      rpl_semi_sync_master_enabled= false;
    }
  }
  else
  {
    repl_semisync_master.disable_master();
    ack_receiver.stop();
  }
  mysql_mutex_unlock(&repl_semisync_master.LOCK_rpl_semi_sync_master_enabled);
  mysql_mutex_lock(&LOCK_global_system_variables);
  return false;
}

static bool fix_rpl_semi_sync_master_timeout(sys_var *self, THD *thd,
                                             enum_var_type type)
{
  repl_semisync_master.set_wait_timeout(rpl_semi_sync_master_timeout);
  return false;
}

static bool fix_rpl_semi_sync_master_trace_level(sys_var *self, THD *thd,
                                                 enum_var_type type)
{
  repl_semisync_master.set_trace_level(rpl_semi_sync_master_trace_level);
  ack_receiver.set_trace_level(rpl_semi_sync_master_trace_level);
  return false;
}

static bool fix_rpl_semi_sync_master_wait_point(sys_var *self, THD *thd,
                                                enum_var_type type)
{
  repl_semisync_master.set_wait_point(rpl_semi_sync_master_wait_point);
  return false;
}

static bool fix_rpl_semi_sync_master_wait_no_slave(sys_var *self, THD *thd,
                                                   enum_var_type type)
{
  repl_semisync_master.check_and_switch();
  return false;
}

static Sys_var_on_access_global<Sys_var_mybool,
                        PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_ENABLED>
Sys_semisync_master_enabled(
       "rpl_semi_sync_master_enabled",
       "Enable semi-synchronous replication master (disabled by default).",
       GLOBAL_VAR(rpl_semi_sync_master_enabled),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_master_enabled));

static Sys_var_on_access_global<Sys_var_ulong,
                        PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_TIMEOUT>
Sys_semisync_master_timeout(
       "rpl_semi_sync_master_timeout",
       "The timeout value (in ms) for semi-synchronous replication in the "
       "master",
       GLOBAL_VAR(rpl_semi_sync_master_timeout),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,~0L),DEFAULT(10000),BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_master_timeout));

static Sys_var_on_access_global<Sys_var_mybool,
                  PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_WAIT_NO_SLAVE>
Sys_semisync_master_wait_no_slave(
       "rpl_semi_sync_master_wait_no_slave",
       "Wait until timeout when no semi-synchronous replication slave "
       "available (enabled by default).",
       GLOBAL_VAR(rpl_semi_sync_master_wait_no_slave),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_master_wait_no_slave));

static Sys_var_on_access_global<Sys_var_ulong,
                    PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_TRACE_LEVEL>
Sys_semisync_master_trace_level(
       "rpl_semi_sync_master_trace_level",
       "The tracing level for semi-sync replication.",
       GLOBAL_VAR(rpl_semi_sync_master_trace_level),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,~0L),DEFAULT(32),BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_master_trace_level));

static const char *repl_semisync_wait_point[]=
{"AFTER_SYNC", "AFTER_COMMIT", NullS};

static Sys_var_on_access_global<Sys_var_enum,
                     PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_MASTER_WAIT_POINT>
Sys_semisync_master_wait_point(
       "rpl_semi_sync_master_wait_point",
       "Should transaction wait for semi-sync ack after having synced binlog, "
       "or after having committed in storage engine.",
       GLOBAL_VAR(rpl_semi_sync_master_wait_point), CMD_LINE(REQUIRED_ARG),
       repl_semisync_wait_point, DEFAULT(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_master_wait_point));

static bool fix_rpl_semi_sync_slave_enabled(sys_var *self, THD *thd,
                                            enum_var_type type)
{
  repl_semisync_slave.set_slave_enabled(rpl_semi_sync_slave_enabled != 0);
  return false;
}

static bool fix_rpl_semi_sync_slave_trace_level(sys_var *self, THD *thd,
                                                enum_var_type type)
{
  repl_semisync_slave.set_trace_level(rpl_semi_sync_slave_trace_level);
  return false;
}

static bool fix_rpl_semi_sync_slave_delay_master(sys_var *self, THD *thd,
                                                 enum_var_type type)
{
  repl_semisync_slave.set_delay_master(rpl_semi_sync_slave_delay_master);
  return false;
}

static bool fix_rpl_semi_sync_slave_kill_conn_timeout(sys_var *self, THD *thd,
                                                      enum_var_type type)
{
  repl_semisync_slave.
    set_kill_conn_timeout(rpl_semi_sync_slave_kill_conn_timeout);
  return false;
}

static Sys_var_on_access_global<Sys_var_mybool,
                         PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_ENABLED>
Sys_semisync_slave_enabled(
       "rpl_semi_sync_slave_enabled",
       "Enable semi-synchronous replication slave (disabled by default).",
       GLOBAL_VAR(rpl_semi_sync_slave_enabled),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_slave_enabled));

static Sys_var_on_access_global<Sys_var_ulong,
                    PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_TRACE_LEVEL>
Sys_semisync_slave_trace_level(
       "rpl_semi_sync_slave_trace_level",
       "The tracing level for semi-sync replication.",
       GLOBAL_VAR(rpl_semi_sync_slave_trace_level),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0,~0L),DEFAULT(32),BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_slave_trace_level));

static Sys_var_on_access_global<Sys_var_mybool,
                    PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_DELAY_MASTER>
Sys_semisync_slave_delay_master(
       "rpl_semi_sync_slave_delay_master",
       "Only write master info file when ack is needed.",
       GLOBAL_VAR(rpl_semi_sync_slave_delay_master),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_slave_delay_master));

static Sys_var_on_access_global<Sys_var_uint,
               PRIV_SET_SYSTEM_GLOBAL_VAR_RPL_SEMI_SYNC_SLAVE_KILL_CONN_TIMEOUT>
Sys_semisync_slave_kill_conn_timeout(
       "rpl_semi_sync_slave_kill_conn_timeout",
       "Timeout for the mysql connection used to kill the slave io_thread's "
       "connection on master. This timeout comes into play when stop slave "
       "is executed.",
       GLOBAL_VAR(rpl_semi_sync_slave_kill_conn_timeout),
       CMD_LINE(OPT_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(5), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_rpl_semi_sync_slave_kill_conn_timeout));
#endif /* HAVE_REPLICATION */

static Sys_var_on_access_global<Sys_var_ulong,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SLOW_LAUNCH_TIME>
Sys_slow_launch_time(
       "slow_launch_time",
       "If creating the thread takes longer than this value (in seconds), "
       "the Slow_launch_threads counter will be incremented",
       GLOBAL_VAR(slow_launch_time), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, LONG_TIMEOUT), DEFAULT(2), BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_sort_buffer(
       "sort_buffer_size",
       "Each thread that needs to do a sort allocates a buffer of this size",
       SESSION_VAR(sortbuff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(MIN_SORT_MEMORY, SIZE_T_MAX), DEFAULT(MAX_SORT_MEMORY),
       BLOCK_SIZE(1));

export sql_mode_t expand_sql_mode(sql_mode_t sql_mode)
{
  if (sql_mode & MODE_ANSI)
  {
    /*
      Note that we don't set
      MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS | MODE_NO_FIELD_OPTIONS
      to allow one to get full use of MySQL in this mode.

      MODE_ONLY_FULL_GROUP_BY was removed from ANSI mode because it is
      currently overly restrictive (see BUG#8510).
    */
    sql_mode|= (MODE_REAL_AS_FLOAT | MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE);
  }
  if (sql_mode & MODE_ORACLE)
    sql_mode|= (MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE |
                MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS |
                MODE_NO_FIELD_OPTIONS | MODE_NO_AUTO_CREATE_USER |
                MODE_SIMULTANEOUS_ASSIGNMENT);
  if (sql_mode & MODE_MSSQL)
    sql_mode|= (MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE |
                MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS |
                MODE_NO_FIELD_OPTIONS);
  if (sql_mode & MODE_POSTGRESQL)
    sql_mode|= (MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE |
                MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS |
                MODE_NO_FIELD_OPTIONS);
  if (sql_mode & MODE_DB2)
    sql_mode|= (MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE |
                MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS |
                MODE_NO_FIELD_OPTIONS);
  if (sql_mode & MODE_MAXDB)
    sql_mode|= (MODE_PIPES_AS_CONCAT | MODE_ANSI_QUOTES |
                MODE_IGNORE_SPACE |
                MODE_NO_KEY_OPTIONS | MODE_NO_TABLE_OPTIONS |
                MODE_NO_FIELD_OPTIONS | MODE_NO_AUTO_CREATE_USER);
  if (sql_mode & MODE_MYSQL40)
    sql_mode|= MODE_HIGH_NOT_PRECEDENCE;
  if (sql_mode & MODE_MYSQL323)
    sql_mode|= MODE_HIGH_NOT_PRECEDENCE;
  if (sql_mode & MODE_TRADITIONAL)
    sql_mode|= (MODE_STRICT_TRANS_TABLES | MODE_STRICT_ALL_TABLES |
                MODE_NO_ZERO_IN_DATE | MODE_NO_ZERO_DATE |
                MODE_ERROR_FOR_DIVISION_BY_ZERO | MODE_NO_AUTO_CREATE_USER |
                MODE_NO_ENGINE_SUBSTITUTION);
  return sql_mode;
}
static bool check_sql_mode(sys_var *self, THD *thd, set_var *var)
{
  var->save_result.ulonglong_value=
    (ulonglong) expand_sql_mode(var->save_result.ulonglong_value);
  return false;
}
static bool fix_sql_mode(sys_var *self, THD *thd, enum_var_type type)
{
  if (type != OPT_GLOBAL)
  {
    /* Update thd->server_status */
    if (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)
      thd->server_status|= SERVER_STATUS_NO_BACKSLASH_ESCAPES;
    else
      thd->server_status&= ~SERVER_STATUS_NO_BACKSLASH_ESCAPES;
    if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
      thd->server_status|= SERVER_STATUS_ANSI_QUOTES;
    else
      thd->server_status&= ~SERVER_STATUS_ANSI_QUOTES;
  }
  return false;
}
/*
  WARNING: When adding new SQL modes don't forget to update the
  tables definitions that stores it's value (ie: mysql.event, mysql.proc)
*/
static const char *sql_mode_names[]=
{
  "REAL_AS_FLOAT", "PIPES_AS_CONCAT", "ANSI_QUOTES", "IGNORE_SPACE",
  "IGNORE_BAD_TABLE_OPTIONS",
  "ONLY_FULL_GROUP_BY", "NO_UNSIGNED_SUBTRACTION", "NO_DIR_IN_CREATE",
  "POSTGRESQL", "ORACLE", "MSSQL", "DB2", "MAXDB", "NO_KEY_OPTIONS",
  "NO_TABLE_OPTIONS", "NO_FIELD_OPTIONS", "MYSQL323", "MYSQL40", "ANSI",
  "NO_AUTO_VALUE_ON_ZERO", "NO_BACKSLASH_ESCAPES", "STRICT_TRANS_TABLES",
  "STRICT_ALL_TABLES", "NO_ZERO_IN_DATE", "NO_ZERO_DATE",
  "ALLOW_INVALID_DATES", "ERROR_FOR_DIVISION_BY_ZERO", "TRADITIONAL",
  "NO_AUTO_CREATE_USER", "HIGH_NOT_PRECEDENCE", "NO_ENGINE_SUBSTITUTION",
  "PAD_CHAR_TO_FULL_LENGTH", "EMPTY_STRING_IS_NULL", "SIMULTANEOUS_ASSIGNMENT",
  "TIME_ROUND_FRACTIONAL",
  0
};


const char *sql_mode_string_representation(uint bit_number)
{
  DBUG_ASSERT(bit_number < array_elements(sql_mode_names));
  return sql_mode_names[bit_number];
}


export bool sql_mode_string_representation(THD *thd, sql_mode_t sql_mode,
                                           LEX_CSTRING *ls)
{
  set_to_string(thd, ls, sql_mode, sql_mode_names);
  return ls->str == 0;
}
/*
  sql_mode should *not* be IN_BINLOG: even though it is written to the binlog,
  the slave ignores the MODE_NO_DIR_IN_CREATE variable, so slave's value
  differs from master's (see log_event.cc: Query_log_event::do_apply_event()).
*/
static Sys_var_set Sys_sql_mode(
       "sql_mode",
       "Sets the sql mode",
       SESSION_VAR(sql_mode), CMD_LINE(REQUIRED_ARG),
       sql_mode_names,
       DEFAULT(MODE_STRICT_TRANS_TABLES |
               MODE_ERROR_FOR_DIVISION_BY_ZERO |
               MODE_NO_ENGINE_SUBSTITUTION |
               MODE_NO_AUTO_CREATE_USER),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_sql_mode), ON_UPDATE(fix_sql_mode));

static const char *old_mode_names[]=
{
  "NO_DUP_KEY_WARNINGS_WITH_IGNORE",
  "NO_PROGRESS_INFO",
  "ZERO_DATE_TIME_CAST",
  0
};

/*
  sql_mode should *not* be IN_BINLOG as the slave can't remember this
  anyway on restart.
*/
static Sys_var_set Sys_old_behavior(
       "old_mode",
       "Used to emulate old behavior from earlier MariaDB or MySQL versions",
       SESSION_VAR(old_behavior), CMD_LINE(REQUIRED_ARG),
       old_mode_names, DEFAULT(0));

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
#define SSL_OPT(X) CMD_LINE(REQUIRED_ARG,X)
#else
#define SSL_OPT(X) NO_CMD_LINE
#endif

static Sys_var_charptr_fscs Sys_ssl_ca(
       "ssl_ca",
       "CA file in PEM format (check OpenSSL docs, implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_ca), SSL_OPT(OPT_SSL_CA),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_capath(
       "ssl_capath",
       "CA directory (check OpenSSL docs, implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_capath), SSL_OPT(OPT_SSL_CAPATH),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_cert(
       "ssl_cert", "X509 cert in PEM format (implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_cert), SSL_OPT(OPT_SSL_CERT),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_cipher(
       "ssl_cipher", "SSL cipher to use (implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_cipher), SSL_OPT(OPT_SSL_CIPHER),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_key(
       "ssl_key", "X509 key in PEM format (implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_key), SSL_OPT(OPT_SSL_KEY),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_crl(
       "ssl_crl",
       "CRL file in PEM format (check OpenSSL docs, implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_crl), SSL_OPT(OPT_SSL_CRL),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_ssl_crlpath(
       "ssl_crlpath",
       "CRL directory (check OpenSSL docs, implies --ssl)",
       READ_ONLY GLOBAL_VAR(opt_ssl_crlpath), SSL_OPT(OPT_SSL_CRLPATH),
       DEFAULT(0));

static const char *tls_version_names[]=
{
  "TLSv1.0",
  "TLSv1.1",
  "TLSv1.2",
  "TLSv1.3",
  0
};

export bool tls_version_string_representation(THD *thd, sql_mode_t sql_mode,
                                              LEX_CSTRING *ls)
{
  set_to_string(thd, ls, tls_version, tls_version_names);
  return ls->str == 0;
}

static Sys_var_set Sys_tls_version(
       "tls_version",
       "TLS protocol version for secure connections.",
       READ_ONLY GLOBAL_VAR(tls_version), CMD_LINE(REQUIRED_ARG),
       tls_version_names,
       DEFAULT(VIO_TLSv1_1 | VIO_TLSv1_2 | VIO_TLSv1_3));

static Sys_var_mybool Sys_standard_compliant_cte(
       "standard_compliant_cte",
       "Allow only CTEs compliant to SQL standard",
       SESSION_VAR(only_standard_compliant_cte), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE));


// why ENUM and not BOOL ?
static const char *updatable_views_with_limit_names[]= {"NO", "YES", 0};
static Sys_var_enum Sys_updatable_views_with_limit(
       "updatable_views_with_limit",
       "YES = Don't issue an error message (warning only) if a VIEW without "
       "presence of a key of the underlying table is used in queries with a "
       "LIMIT clause for updating. NO = Prohibit update of a VIEW, which "
       "does not contain a key of the underlying table and the query uses "
       "a LIMIT clause (usually get from GUI tools)",
       SESSION_VAR(updatable_views_with_limit), CMD_LINE(REQUIRED_ARG),
       updatable_views_with_limit_names, DEFAULT(TRUE));

static Sys_var_mybool Sys_sync_frm(
       "sync_frm", "Sync .frm files to disk on creation",
       GLOBAL_VAR(opt_sync_frm), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE));

static char *system_time_zone_ptr;
static Sys_var_charptr Sys_system_time_zone(
       "system_time_zone", "The server system time zone",
       READ_ONLY GLOBAL_VAR(system_time_zone_ptr),
       CMD_LINE_HELP_ONLY,
       DEFAULT(system_time_zone));

/*
  If One use views with prepared statements this should be bigger than
  table_open_cache (now we allow 2 times bigger value)
*/
static Sys_var_ulong Sys_table_def_size(
       "table_definition_cache",
       "The number of cached table definitions",
       GLOBAL_VAR(tdc_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(TABLE_DEF_CACHE_MIN, 2*1024*1024),
       DEFAULT(TABLE_DEF_CACHE_DEFAULT), BLOCK_SIZE(1));


static bool fix_table_open_cache(sys_var *, THD *, enum_var_type)
{
  mysql_mutex_unlock(&LOCK_global_system_variables);
  tc_purge();
  mysql_mutex_lock(&LOCK_global_system_variables);
  return false;
}

/* Check the table_definition_cache comment if makes changes */
static Sys_var_ulong Sys_table_cache_size(
       "table_open_cache", "The number of cached open tables",
       GLOBAL_VAR(tc_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(10, 1024*1024), DEFAULT(TABLE_OPEN_CACHE_DEFAULT),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_table_open_cache));

static Sys_var_uint Sys_table_cache_instances(
       "table_open_cache_instances", "Maximum number of table cache instances",
       READ_ONLY GLOBAL_VAR(tc_instances), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 64), DEFAULT(8), BLOCK_SIZE(1));

static Sys_var_ulong Sys_thread_cache_size(
       "thread_cache_size",
       "How many threads we should keep in a cache for reuse. These are freed after 5 minutes of idle time",
       GLOBAL_VAR(thread_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 16384), DEFAULT(256), BLOCK_SIZE(1));

#ifdef HAVE_POOL_OF_THREADS
static bool fix_tp_max_threads(sys_var *, THD *, enum_var_type)
{
  tp_set_max_threads(threadpool_max_threads);
  return false;
}


#ifdef _WIN32
static bool fix_tp_min_threads(sys_var *, THD *, enum_var_type)
{
  tp_set_min_threads(threadpool_min_threads);
  return false;
}
#endif

static bool check_threadpool_size(sys_var *self, THD *thd, set_var *var)
{
  ulonglong v= var->save_result.ulonglong_value;
  if (v > threadpool_max_size)
  {
    var->save_result.ulonglong_value= threadpool_max_size;
    return throw_bounds_warning(thd, self->name.str, true, true, v);
  }
  return false;
}


static bool fix_threadpool_size(sys_var*, THD*, enum_var_type)
{
  tp_set_threadpool_size(threadpool_size);
  return false;
}


static bool fix_threadpool_stall_limit(sys_var*, THD*, enum_var_type)
{
  tp_set_threadpool_stall_limit(threadpool_stall_limit);
  return false;
}

#ifdef _WIN32
static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_min_threads(
  "thread_pool_min_threads",
  "Minimum number of threads in the thread pool.",
  GLOBAL_VAR(threadpool_min_threads), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(1, 256), DEFAULT(1), BLOCK_SIZE(1),
  NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
  ON_UPDATE(fix_tp_min_threads)
  );

static const char *threadpool_mode_names[]={ "windows", "generic", 0 };
static Sys_var_on_access_global<Sys_var_enum,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_mode(
  "thread_pool_mode",
  "Chose implementation of the threadpool",
  READ_ONLY GLOBAL_VAR(threadpool_mode), CMD_LINE(REQUIRED_ARG),
  threadpool_mode_names, DEFAULT(TP_MODE_WINDOWS)
  );
#endif

static const char *threadpool_priority_names[]={ "high", "low", "auto", 0 };
static Sys_var_on_access_global<Sys_var_enum,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_thread_pool_priority(
  "thread_pool_priority",
  "Threadpool priority. High priority connections usually start executing earlier than low priority."
  "If priority set to 'auto', the the actual priority(low or high) is determined based on whether or not connection is inside transaction.",
  SESSION_VAR(threadpool_priority), CMD_LINE(REQUIRED_ARG),
  threadpool_priority_names, DEFAULT(TP_PRIORITY_AUTO));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_idle_thread_timeout(
  "thread_pool_idle_timeout",
  "Timeout in seconds for an idle thread in the thread pool."
  "Worker thread will be shut down after timeout",
  GLOBAL_VAR(threadpool_idle_timeout), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(1, UINT_MAX), DEFAULT(60), BLOCK_SIZE(1)
);
static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_oversubscribe(
  "thread_pool_oversubscribe",
  "How many additional active worker threads in a group are allowed.",
  GLOBAL_VAR(threadpool_oversubscribe), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(1, 1000), DEFAULT(3), BLOCK_SIZE(1)
);
static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_size(
 "thread_pool_size",
 "Number of thread groups in the pool. "
 "This parameter is roughly equivalent to maximum number of concurrently "
 "executing threads (threads in a waiting state do not count as executing).",
  GLOBAL_VAR(threadpool_size), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(1, MAX_THREAD_GROUPS), DEFAULT(8), BLOCK_SIZE(1),
  NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_threadpool_size),
  ON_UPDATE(fix_threadpool_size)
);
static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_stall_limit(
 "thread_pool_stall_limit",
 "Maximum query execution time in milliseconds,"
 "before an executing non-yielding thread is considered stalled."
 "If a worker thread is stalled, additional worker thread "
 "may be created to handle remaining clients.",
  GLOBAL_VAR(threadpool_stall_limit), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(1, UINT_MAX), DEFAULT(DEFAULT_THREADPOOL_STALL_LIMIT), BLOCK_SIZE(1),
  NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), 
  ON_UPDATE(fix_threadpool_stall_limit)
);

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_max_threads(
  "thread_pool_max_threads",
  "Maximum allowed number of worker threads in the thread pool",
   GLOBAL_VAR(threadpool_max_threads), CMD_LINE(REQUIRED_ARG),
   VALID_RANGE(1, 65536), DEFAULT(65536), BLOCK_SIZE(1),
   NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), 
   ON_UPDATE(fix_tp_max_threads)
);

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_threadpool_prio_kickup_timer(
 "thread_pool_prio_kickup_timer",
 "The number of milliseconds before a dequeued low-priority statement is moved to the high-priority queue",
  GLOBAL_VAR(threadpool_prio_kickup_timer), CMD_LINE(REQUIRED_ARG),
  VALID_RANGE(0, UINT_MAX), DEFAULT(1000), BLOCK_SIZE(1)
);

static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_exact_stats(
  "thread_pool_exact_stats",
  "If set to 1, provides better statistics in information_schema threadpool tables",
  GLOBAL_VAR(threadpool_exact_stats), CMD_LINE(OPT_ARG), DEFAULT(FALSE),
    NO_MUTEX_GUARD, NOT_IN_BINLOG
);

static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_THREAD_POOL>
Sys_threadpool_dedicated_listener(
  "thread_pool_dedicated_listener",
  "If set to 1,listener thread will not pick up queries",
  GLOBAL_VAR(threadpool_dedicated_listener), CMD_LINE(OPT_ARG), DEFAULT(FALSE),
  NO_MUTEX_GUARD, NOT_IN_BINLOG
);
#endif /* HAVE_POOL_OF_THREADS */

/**
  Can't change the 'next' tx_isolation if we are already in a
  transaction.
*/

static bool check_tx_isolation(sys_var *self, THD *thd, set_var *var)
{
  if (var->type == OPT_DEFAULT && thd->in_active_multi_stmt_transaction())
  {
    DBUG_ASSERT(thd->in_multi_stmt_transaction_mode());
    my_error(ER_CANT_CHANGE_TX_CHARACTERISTICS, MYF(0));
    return TRUE;
  }
  return FALSE;
}

// NO_CMD_LINE - different name of the option
static Sys_var_tx_isolation Sys_tx_isolation(
       "tx_isolation", "Default transaction isolation level",
       NO_SET_STMT SESSION_VAR(tx_isolation), NO_CMD_LINE,
       tx_isolation_names, DEFAULT(ISO_REPEATABLE_READ),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_tx_isolation));


/**
  Can't change the tx_read_only state if we are already in a
  transaction.
*/

static bool check_tx_read_only(sys_var *self, THD *thd, set_var *var)
{
  if (var->type == OPT_DEFAULT && thd->in_active_multi_stmt_transaction())
  {
    DBUG_ASSERT(thd->in_multi_stmt_transaction_mode());
    my_error(ER_CANT_CHANGE_TX_CHARACTERISTICS, MYF(0));
    return true;
  }
  return false;
}


bool Sys_var_tx_read_only::session_update(THD *thd, set_var *var)
{
  if (var->type == OPT_SESSION && Sys_var_mybool::session_update(thd, var))
    return true;
  if (var->type == OPT_DEFAULT || !thd->in_active_multi_stmt_transaction())
  {
    // @see Sys_var_tx_isolation::session_update() above for the rules.
    thd->tx_read_only= var->save_result.ulonglong_value;

#ifndef EMBEDDED_LIBRARY
    if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
    {
      if (var->type == OPT_DEFAULT)
        thd->session_tracker.transaction_info.set_read_flags(thd,
                            thd->tx_read_only ? TX_READ_ONLY : TX_READ_WRITE);
      else
        thd->session_tracker.transaction_info.set_read_flags(thd,
                            TX_READ_INHERIT);
    }
#endif //EMBEDDED_LIBRARY
  }
  return false;
}


static Sys_var_tx_read_only Sys_tx_read_only(
       "tx_read_only", "Default transaction access mode. If set to OFF, "
       "the default, access is read/write. If set to ON, access is read-only. "
       "The SET TRANSACTION statement can also change the value of this variable. "
       "See SET TRANSACTION and START TRANSACTION.",
       SESSION_VAR(tx_read_only), NO_CMD_LINE, DEFAULT(0),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_tx_read_only));

static Sys_var_ulonglong Sys_tmp_table_size(
       "tmp_table_size",
       "Alias for tmp_memory_table_size. "
       "If an internal in-memory temporary table exceeds this size, MariaDB "
       "will automatically convert it to an on-disk MyISAM or Aria table.",
       SESSION_VAR(tmp_memory_table_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, (ulonglong)~(intptr)0), DEFAULT(16*1024*1024),
       BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_tmp_memory_table_size(
       "tmp_memory_table_size",
       "If an internal in-memory temporary table exceeds this size, MariaDB "
       "will automatically convert it to an on-disk MyISAM or Aria table. "
       "Same as tmp_table_size.",
       SESSION_VAR(tmp_memory_table_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, (ulonglong)~(intptr)0), DEFAULT(16*1024*1024),
       BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_tmp_disk_table_size(
       "tmp_disk_table_size",
       "Max size for data for an internal temporary on-disk MyISAM or Aria table.",
       SESSION_VAR(tmp_disk_table_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, (ulonglong)~(intptr)0),
       DEFAULT((ulonglong)~(intptr)0), BLOCK_SIZE(1));

static Sys_var_charptr Sys_version(
       "version", "Server version number. It may also include a suffix "
       "with configuration or build information. -debug indicates "
       "debugging support was enabled on the server, and -log indicates "
       "at least one of the binary log, general log or slow query log are "
       "enabled, for example 10.1.1-MariaDB-mariadb1precise-log.",
       READ_ONLY GLOBAL_VAR(server_version_ptr),
       CMD_LINE_HELP_ONLY,
       DEFAULT(server_version));

static char *server_version_comment_ptr;
static Sys_var_charptr Sys_version_comment(
       "version_comment", "Value of the COMPILATION_COMMENT option "
       "specified by CMake when building MariaDB, for example "
       "mariadb.org binary distribution.",
       READ_ONLY GLOBAL_VAR(server_version_comment_ptr),
       CMD_LINE_HELP_ONLY,
       DEFAULT(MYSQL_COMPILATION_COMMENT));

static char *server_version_compile_machine_ptr;
static Sys_var_charptr Sys_version_compile_machine(
       "version_compile_machine", "The machine type or architecture "
       "MariaDB was built on, for example i686.",
       READ_ONLY GLOBAL_VAR(server_version_compile_machine_ptr),
       CMD_LINE_HELP_ONLY, DEFAULT(DEFAULT_MACHINE));

static char *server_version_compile_os_ptr;
static Sys_var_charptr Sys_version_compile_os(
       "version_compile_os", "Operating system that MariaDB was built "
       "on, for example debian-linux-gnu.",
       READ_ONLY GLOBAL_VAR(server_version_compile_os_ptr),
       CMD_LINE_HELP_ONLY,
       DEFAULT(SYSTEM_TYPE));

#include <source_revision.h>
static char *server_version_source_revision;
static Sys_var_charptr Sys_version_source_revision(
       "version_source_revision", "Source control revision id for MariaDB source code",
       READ_ONLY GLOBAL_VAR(server_version_source_revision),
       CMD_LINE_HELP_ONLY,
       DEFAULT(SOURCE_REVISION));

static char *malloc_library;
static Sys_var_charptr Sys_malloc_library(
       "version_malloc_library", "Version of the used malloc library",
       READ_ONLY GLOBAL_VAR(malloc_library), CMD_LINE_HELP_ONLY,
       DEFAULT(guess_malloc_library()));

static char *ssl_library;
static Sys_var_charptr Sys_ssl_library(
       "version_ssl_library", "Version of the used SSL library",
       READ_ONLY GLOBAL_VAR(ssl_library), CMD_LINE_HELP_ONLY,
       DEFAULT(SSL_LIBRARY));

static Sys_var_ulong Sys_net_wait_timeout(
       "wait_timeout",
       "The number of seconds the server waits for activity on a "
       "connection before closing it",
       NO_SET_STMT SESSION_VAR(net_wait_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, IF_WIN(INT_MAX32/1000, LONG_TIMEOUT)),
       DEFAULT(NET_WAIT_TIMEOUT), BLOCK_SIZE(1));

static Sys_var_uint Sys_idle_transaction_timeout(
       "idle_transaction_timeout",
       "The number of seconds the server waits for idle transaction",
       SESSION_VAR(idle_transaction_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, IF_WIN(INT_MAX32/1000, LONG_TIMEOUT)),
       DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_uint Sys_idle_readonly_transaction_timeout(
       "idle_readonly_transaction_timeout",
       "The number of seconds the server waits for read-only idle transaction",
       SESSION_VAR(idle_readonly_transaction_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, IF_WIN(INT_MAX32/1000, LONG_TIMEOUT)),
       DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_uint Sys_idle_write_transaction_timeout(
       "idle_write_transaction_timeout",
       "The number of seconds the server waits for write idle transaction",
       SESSION_VAR(idle_write_transaction_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, IF_WIN(INT_MAX32/1000, LONG_TIMEOUT)),
       DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_plugin Sys_default_storage_engine(
       "default_storage_engine", "The default storage engine for new tables",
       SESSION_VAR(table_plugin), NO_CMD_LINE,
       MYSQL_STORAGE_ENGINE_PLUGIN, DEFAULT(&default_storage_engine),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_not_null));

static Sys_var_plugin Sys_storage_engine(
       "storage_engine", "Alias for @@default_storage_engine. Deprecated",
       SESSION_VAR(table_plugin), NO_CMD_LINE,
       MYSQL_STORAGE_ENGINE_PLUGIN, DEFAULT(&default_storage_engine),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_not_null), ON_UPDATE(0),
       DEPRECATED("'@@default_storage_engine'")); // since 10.5.1

static Sys_var_plugin Sys_default_tmp_storage_engine(
       "default_tmp_storage_engine", "The default storage engine for user-created temporary tables",
       SESSION_VAR(tmp_table_plugin), NO_CMD_LINE,
       MYSQL_STORAGE_ENGINE_PLUGIN, DEFAULT(&default_tmp_storage_engine));

static Sys_var_plugin Sys_enforce_storage_engine(
       "enforce_storage_engine", "Force the use of a storage engine for new tables",
       SESSION_VAR(enforced_table_plugin),
       NO_CMD_LINE, MYSQL_STORAGE_ENGINE_PLUGIN,
       DEFAULT(&enforced_storage_engine), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_has_super));


#ifdef HAVE_REPLICATION
/*
  Check
   1. Value for gtid_pos_auto_engines is not NULL.
   2. No slave SQL thread is running.
*/
static bool
check_gtid_pos_auto_engines(sys_var *self, THD *thd, set_var *var)
{
  bool running;
  bool err= false;

  DBUG_ASSERT(var->type == OPT_GLOBAL);
  if (var->value && var->value->is_null())
    err= true;
  else
  {
    running= give_error_if_slave_running(false);
    if (running)
      err= true;
  }
  return err;
}


static Sys_var_on_access_global<Sys_var_pluginlist,
                               PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_POS_AUTO_ENGINES>
Sys_gtid_pos_auto_engines(
       "gtid_pos_auto_engines",
       "List of engines for which to automatically create a "
       "mysql.gtid_slave_pos_ENGINE table, if a transaction using that engine "
       "is replicated. This can be used to avoid introducing cross-engine "
       "transactions, if engines are used different from that used by table "
       "mysql.gtid_slave_pos",
       GLOBAL_VAR(opt_gtid_pos_auto_plugins), NO_CMD_LINE,
       DEFAULT(&gtid_pos_auto_engines),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_gtid_pos_auto_engines));
#endif


#if defined(ENABLED_DEBUG_SYNC)
/*
  Variable can be set for the session only.

  This could be changed later. Then we need to have a global array of
  actions in addition to the thread local ones. SET GLOBAL would
  manage the global array, SET [SESSION] the local array. A sync point
  would need to look for a local and a global action. Setting and
  executing of global actions need to be protected by a mutex.

  The purpose of global actions could be to allow synchronizing with
  connectionless threads that cannot execute SET statements.
*/
static Sys_var_debug_sync Sys_debug_sync(
       "debug_sync", "Debug Sync Facility",
       NO_SET_STMT sys_var::ONLY_SESSION, NO_CMD_LINE,
       DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_has_super));
#endif /* defined(ENABLED_DEBUG_SYNC) */

/**
 "time_format" "date_format" "datetime_format"

  the following three variables are unused, and the source of confusion
  (bug reports like "I've changed date_format, but date format hasn't changed.
  I've made them read-only, to alleviate the situation somewhat.

  @todo make them NO_CMD_LINE ?
*/
static Sys_var_charptr Sys_date_format(
       "date_format", "The DATE format (ignored)",
       READ_ONLY GLOBAL_VAR(global_date_format.format.str),
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(known_date_time_formats[ISO_FORMAT].date_format),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("")); // since 10.1.2

static Sys_var_charptr Sys_datetime_format(
       "datetime_format", "The DATETIME format (ignored)",
       READ_ONLY GLOBAL_VAR(global_datetime_format.format.str),
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(known_date_time_formats[ISO_FORMAT].datetime_format),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("")); // since 10.1.2

static Sys_var_charptr Sys_time_format(
       "time_format", "The TIME format (ignored)",
       READ_ONLY GLOBAL_VAR(global_time_format.format.str),
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(known_date_time_formats[ISO_FORMAT].time_format),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("")); //  since 10.1.2

static bool fix_autocommit(sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_GLOBAL)
  {
    if (global_system_variables.option_bits & OPTION_AUTOCOMMIT)
      global_system_variables.option_bits&= ~OPTION_NOT_AUTOCOMMIT;
    else
      global_system_variables.option_bits|= OPTION_NOT_AUTOCOMMIT;
    return false;
  }

  if (test_all_bits(thd->variables.option_bits,
                    (OPTION_AUTOCOMMIT | OPTION_NOT_AUTOCOMMIT)))
  {
    // activating autocommit
    if (trans_commit_stmt(thd) || trans_commit(thd))
    {
      thd->variables.option_bits&= ~OPTION_AUTOCOMMIT;
      thd->mdl_context.release_transactional_locks();
      WSREP_DEBUG("autocommit, MDL TRX lock released: %lld",
                  (longlong) thd->thread_id);
      return true;
    }
    /*
      Don't close thread tables or release metadata locks: if we do so, we
      risk releasing locks/closing tables of expressions used to assign
      other variables, as in:
      set @var=my_stored_function1(), @@autocommit=1, @var2=(select MY_MAX(a)
      from my_table), ...
      The locks will be released at statement end anyway, as SET
      statement that assigns autocommit is marked to commit
      transaction implicitly at the end (@sa stmt_causes_implicitcommit()).
    */
    thd->variables.option_bits&=
                 ~(OPTION_BEGIN | OPTION_KEEP_LOG | OPTION_NOT_AUTOCOMMIT |
                   OPTION_GTID_BEGIN);
    thd->transaction->all.modified_non_trans_table= false;
    thd->transaction->all.m_unsafe_rollback_flags&= ~THD_TRANS::DID_WAIT;
    thd->server_status|= SERVER_STATUS_AUTOCOMMIT;
    return false;
  }

  if ((thd->variables.option_bits &
       (OPTION_AUTOCOMMIT |OPTION_NOT_AUTOCOMMIT)) == 0)
  {
    // disabling autocommit
    thd->transaction->all.modified_non_trans_table= false;
    thd->transaction->all.m_unsafe_rollback_flags&= ~THD_TRANS::DID_WAIT;
    thd->server_status&= ~SERVER_STATUS_AUTOCOMMIT;
    thd->variables.option_bits|= OPTION_NOT_AUTOCOMMIT;
    return false;
  }

  return false; // autocommit value wasn't changed
}

static Sys_var_bit Sys_autocommit(
       "autocommit", "If set to 1, the default, all queries are committed "
       "immediately. If set to 0, they are only committed upon a COMMIT statement"
       ", or rolled back with a ROLLBACK statement. If autocommit is set to 0, "
       "and then changed to 1, all open transactions are immediately committed.",
       NO_SET_STMT SESSION_VAR(option_bits), NO_CMD_LINE,
       OPTION_AUTOCOMMIT, DEFAULT(TRUE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(fix_autocommit));
export sys_var *Sys_autocommit_ptr= &Sys_autocommit; // for sql_yacc.yy

static Sys_var_mybool Sys_big_tables(
       "big_tables", "Old variable, which if set to 1, allows large result sets "
       "by saving all temporary sets to disk, avoiding 'table full' errors. No "
       "longer needed, as the server now handles this automatically.",
       SESSION_VAR(big_tables), CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0),
       DEPRECATED("")); // since 10.5.0

static Sys_var_bit Sys_big_selects(
       "sql_big_selects", "If set to 0, MariaDB will not perform large SELECTs."
       " See max_join_size for details. If max_join_size is set to anything but "
       "DEFAULT, sql_big_selects is automatically set to 0. If sql_big_selects "
       "is again set, max_join_size will be ignored.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_BIG_SELECTS,
       DEFAULT(FALSE));

static Sys_var_bit Sys_log_off(
       "sql_log_off", "If set to 1 (0 is the default), no logging to the general "
       "query log is done for the client. Only clients with the SUPER privilege "
       "can update this variable.",
       NO_SET_STMT SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_LOG_OFF,
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_has_super));

/**
  This function sets the session variable thd->variables.sql_log_bin 
  to reflect changes to @@session.sql_log_bin.

  @param[IN] self   A pointer to the sys_var, i.e. Sys_log_binlog.
  @param[IN] type   The type either session or global.

  @return @c FALSE.
*/
static bool fix_sql_log_bin_after_update(sys_var *self, THD *thd,
                                         enum_var_type type)
{
  DBUG_ASSERT(type == OPT_SESSION);

  if (thd->variables.sql_log_bin)
    thd->variables.option_bits |= OPTION_BIN_LOG;
  else
    thd->variables.option_bits &= ~OPTION_BIN_LOG;

  return FALSE;
}

static bool check_session_only_variable(sys_var *self, THD *,set_var *var)
{
  if (unlikely(var->type == OPT_GLOBAL))
  {
    my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0), self->name.str, "SESSION");
    return true;
  }
  return false;
}

/**
  This function checks if the sql_log_bin can be changed,
  what is possible if:
    - the user is a super user;
    - the set is not called from within a function/trigger;
    - there is no on-going transaction.

  @param[IN] self   A pointer to the sys_var, i.e. Sys_log_binlog.
  @param[IN] var    A pointer to the set_var created by the parser.

  @return @c FALSE if the change is allowed, otherwise @c TRUE.
*/
static bool check_sql_log_bin(sys_var *self, THD *thd, set_var *var)
{
  if (check_session_only_variable(self, thd, var))
    return true;

  if (unlikely(error_if_in_trans_or_substatement(thd,
                                                 ER_STORED_FUNCTION_PREVENTS_SWITCH_SQL_LOG_BIN,
                                                 ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_SQL_LOG_BIN)))
    return true;

  return false;
}

static Sys_var_on_access<Sys_var_mybool,
                         PRIV_SET_SYSTEM_VAR_SQL_LOG_BIN,
                         PRIV_SET_SYSTEM_VAR_SQL_LOG_BIN>
Sys_sql_log_bin(
       "sql_log_bin", "If set to 0 (1 is the default), no logging to the binary "
       "log is done for the client. Only clients with the SUPER privilege can "
       "update this variable. Can have unintended consequences if set globally, "
       "see SET SQL_LOG_BIN. Starting MariaDB 10.1.7, this variable does not "
       "affect the replication of events in a Galera cluster.",
       SESSION_VAR(sql_log_bin), NO_CMD_LINE, DEFAULT(TRUE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_sql_log_bin),
       ON_UPDATE(fix_sql_log_bin_after_update));

static Sys_var_bit Sys_sql_warnings(
       "sql_warnings", "If set to 1, single-row INSERTs will produce a string "
       "containing warning information if a warning occurs.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_WARNINGS,
       DEFAULT(FALSE));

static Sys_var_bit Sys_sql_notes(
       "sql_notes", "If set to 1, the default, warning_count is incremented each "
       "time a Note warning is encountered. If set to 0, Note warnings are not "
       "recorded. mysqldump has outputs to set this variable to 0 so that no "
       "unnecessary increments occur when data is reloaded.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_SQL_NOTES,
       DEFAULT(TRUE));

static Sys_var_bit Sys_auto_is_null(
       "sql_auto_is_null", "If set to 1, the query SELECT * FROM table_name WHERE "
       "auto_increment_column IS NULL will return an auto-increment that has just "
       "been successfully inserted, the same as the LAST_INSERT_ID() function. Some"
       " ODBC programs make use of this IS NULL comparison.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_AUTO_IS_NULL,
       DEFAULT(FALSE), NO_MUTEX_GUARD, IN_BINLOG);

static Sys_var_bit Sys_if_exists(
      "sql_if_exists", "If set to 1 adds an implicate IF EXISTS to ALTER, RENAME and DROP of TABLES, VIEWS, FUNCTIONS and PACKAGES",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_IF_EXISTS,
       DEFAULT(FALSE), NO_MUTEX_GUARD, IN_BINLOG);

static Sys_var_bit Sys_safe_updates(
       "sql_safe_updates", "If set to 1, UPDATEs and DELETEs need either a key in "
       "the WHERE clause, or a LIMIT clause, or else they will aborted. Prevents "
       "the common mistake of accidentally deleting or updating every row in a table.",
       SESSION_VAR(option_bits), CMD_LINE(OPT_ARG), OPTION_SAFE_UPDATES,
       DEFAULT(FALSE));

static Sys_var_bit Sys_buffer_results(
       "sql_buffer_result", "If set to 1 (0 is default), results from SELECT "
       "statements are always placed into temporary tables. This can help the "
       "server when it takes a long time to send the results to the client by "
       "allowing the table locks to be freed early.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_BUFFER_RESULT,
       DEFAULT(FALSE));

static Sys_var_bit Sys_quote_show_create(
       "sql_quote_show_create", "If set to 1, the default, the server will "
       "quote identifiers for SHOW CREATE DATABASE, SHOW CREATE TABLE and "
       "SHOW CREATE VIEW statements. Quoting is disabled if set to 0. Enable "
       "to ensure replications works when identifiers require quoting.",
       SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_QUOTE_SHOW_CREATE,
       DEFAULT(TRUE));

static Sys_var_bit Sys_foreign_key_checks(
       "foreign_key_checks", "If set to 1 (the default) foreign key constraints"
       " (including ON UPDATE and ON DELETE behavior) InnoDB tables are checked,"
       " while if set to 0, they are not checked. 0 is not recommended for normal "
       "use, though it can be useful in situations where you know the data is "
       "consistent, but want to reload data in a different order from that that "
       "specified by parent/child relationships. Setting this variable to 1 does "
       "not retrospectively check for inconsistencies introduced while set to 0.",
       SESSION_VAR(option_bits), NO_CMD_LINE,
       REVERSE(OPTION_NO_FOREIGN_KEY_CHECKS),
       DEFAULT(TRUE), NO_MUTEX_GUARD, IN_BINLOG);

static Sys_var_bit Sys_unique_checks(
       "unique_checks", "If set to 1, the default, secondary indexes in InnoDB "
       "tables are performed. If set to 0, storage engines can (but are not "
       "required to) assume that duplicate keys are not present in input data. "
       "Set to 0 to speed up imports of large tables to InnoDB. The storage "
       "engine will still issue a duplicate key error if it detects one, even "
       "if set to 0.",
       SESSION_VAR(option_bits), NO_CMD_LINE,
       REVERSE(OPTION_RELAXED_UNIQUE_CHECKS),
       DEFAULT(TRUE), NO_MUTEX_GUARD, IN_BINLOG);

static Sys_var_bit Sys_no_check_constraint(
       "check_constraint_checks", "check_constraint_checks",
       SESSION_VAR(option_bits), NO_CMD_LINE,
       REVERSE(OPTION_NO_CHECK_CONSTRAINT_CHECKS),
       DEFAULT(TRUE), NO_MUTEX_GUARD, IN_BINLOG);

#ifdef ENABLED_PROFILING
static bool update_profiling(sys_var *self, THD *thd, enum_var_type type)
{
  if (type == OPT_SESSION)
    thd->profiling.reset();
  return false;
}

static Sys_var_bit Sys_profiling(
       "profiling", "If set to 1 (0 is default), statement profiling will be "
       "enabled. See SHOW PROFILES and SHOW PROFILE.",
       NO_SET_STMT SESSION_VAR(option_bits), NO_CMD_LINE, OPTION_PROFILING,
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_profiling));

static Sys_var_ulong Sys_profiling_history_size(
       "profiling_history_size", "Number of statements about which profiling "
       "information is maintained. If set to 0, no profiles are stored. "
       "See SHOW PROFILES.",
       NO_SET_STMT SESSION_VAR(profiling_history_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 100), DEFAULT(15), BLOCK_SIZE(1));
#endif

/*
  When this is set by a connection, binlogged events will be marked with a
  corresponding flag. The slave can be configured to not replicate events
  so marked.
  In the binlog dump thread on the master, this variable is re-used for a
  related purpose: The slave sets this flag when connecting to the master to
  request that the master filter out (ie. not send) any events with the flag
  set, thus saving network traffic on events that would be ignored by the
  slave anyway.
*/
static bool check_skip_replication(sys_var *self, THD *thd, set_var *var)
{
  /*
    We must not change @@skip_replication in the middle of a transaction or
    statement, as that could result in only part of the transaction / statement
    being replicated.
    (This would be particularly serious if we were to replicate eg.
    Rows_log_event without Table_map_log_event or transactional updates without
    the COMMIT).
  */
  if (unlikely(error_if_in_trans_or_substatement(thd,
                                                 ER_STORED_FUNCTION_PREVENTS_SWITCH_SKIP_REPLICATION,
                                                 ER_INSIDE_TRANSACTION_PREVENTS_SWITCH_SKIP_REPLICATION)))
    return 1;

  return 0;
}

static Sys_var_bit Sys_skip_replication(
       "skip_replication", "Changes are logged into the binary log with the "
       "@@skip_replication flag set. Such events will not be replicated by "
       "slaves that run with --replicate-events-marked-for-skip set different "
       "from its default of REPLICATE. See Selectively skipping replication "
       "of binlog events for more information.",
       NO_SET_STMT SESSION_ONLY(option_bits),
       NO_CMD_LINE, OPTION_SKIP_REPLICATION,
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_skip_replication));

static Sys_var_harows Sys_select_limit(
       "sql_select_limit",
       "The maximum number of rows to return from SELECT statements",
       SESSION_VAR(select_limit), NO_CMD_LINE,
       VALID_RANGE(0, HA_POS_ERROR), DEFAULT(HA_POS_ERROR), BLOCK_SIZE(1));

static const char *secure_timestamp_levels[]= {"NO", "SUPER", "REPLICATION", "YES", 0};
bool Sys_var_timestamp::on_check_access_session(THD *thd) const
{
  switch (opt_secure_timestamp) {
  case SECTIME_NO:
    return false;
  case SECTIME_SUPER:
    return check_global_access(thd, SUPER_ACL | BINLOG_REPLAY_ACL);
  case SECTIME_REPL:
    return check_global_access(thd, BINLOG_REPLAY_ACL);
  case SECTIME_YES:
    break;
  }
  char buf[1024];
  strxnmov(buf, sizeof(buf), "--secure-timestamp=",
           secure_timestamp_levels[opt_secure_timestamp], NULL);
  my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), buf);
  return true;
}
static Sys_var_timestamp Sys_timestamp(
       "timestamp", "Set the time for this client",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, TIMESTAMP_MAX_VALUE),
       NO_MUTEX_GUARD, IN_BINLOG);

static bool update_last_insert_id(THD *thd, set_var *var)
{
  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }
  thd->first_successful_insert_id_in_prev_stmt=
    var->save_result.ulonglong_value;
  return false;
}
static ulonglong read_last_insert_id(THD *thd)
{
  return (ulonglong) thd->read_first_successful_insert_id_in_prev_stmt();
}
static Sys_var_session_special Sys_last_insert_id(
       "last_insert_id", "The value to be returned from LAST_INSERT_ID()",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONGLONG_MAX), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_last_insert_id), ON_READ(read_last_insert_id));

// alias for last_insert_id(), Sybase-style
static Sys_var_session_special Sys_identity(
       "identity", "Synonym for the last_insert_id variable",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONGLONG_MAX), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_last_insert_id), ON_READ(read_last_insert_id));

/*
  insert_id should *not* be marked as written to the binlog (i.e., it
  should *not* be IN_BINLOG), because we want any statement that
  refers to insert_id explicitly to be unsafe.  (By "explicitly", we
  mean using @@session.insert_id, whereas insert_id is used
  "implicitly" when NULL value is inserted into an auto_increment
  column).

  We want statements referring explicitly to @@session.insert_id to be
  unsafe, because insert_id is modified internally by the slave sql
  thread when NULL values are inserted in an AUTO_INCREMENT column.
  This modification interfers with the value of the
  @@session.insert_id variable if @@session.insert_id is referred
  explicitly by an insert statement (as is seen by executing "SET
  @@session.insert_id=0; CREATE TABLE t (a INT, b INT KEY
  AUTO_INCREMENT); INSERT INTO t(a) VALUES (@@session.insert_id);" in
  statement-based logging mode: t will be different on master and
  slave).
*/
static bool update_insert_id(THD *thd, set_var *var)
{
  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }
  thd->force_one_auto_inc_interval(var->save_result.ulonglong_value);
  return false;
}

static ulonglong read_insert_id(THD *thd)
{
  return thd->auto_inc_intervals_forced.minimum();
}
static Sys_var_session_special Sys_insert_id(
       "insert_id", "The value to be used by the following INSERT "
       "or ALTER TABLE statement when inserting an AUTO_INCREMENT value",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONGLONG_MAX), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_insert_id), ON_READ(read_insert_id));

static bool update_rand_seed1(THD *thd, set_var *var)
{
  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }
  thd->rand.seed1= (ulong) var->save_result.ulonglong_value;
  return false;
}
static ulonglong read_rand_seed1(THD *thd)
{
  return thd->rand.seed1;
}
static Sys_var_session_special Sys_rand_seed1(
       "rand_seed1", "Sets the internal state of the RAND() "
       "generator for replication purposes",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONG_MAX), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_rand_seed1), ON_READ(read_rand_seed1));

static bool update_rand_seed2(THD *thd, set_var *var)
{
  if (!var->value)
  {
    my_error(ER_NO_DEFAULT, MYF(0), var->var->name.str);
    return true;
  }
  thd->rand.seed2= (ulong) var->save_result.ulonglong_value;
  return false;
}
static ulonglong read_rand_seed2(THD *thd)
{
  return thd->rand.seed2;
}
static Sys_var_session_special Sys_rand_seed2(
       "rand_seed2", "Sets the internal state of the RAND() "
       "generator for replication purposes",
       sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONG_MAX), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_rand_seed2), ON_READ(read_rand_seed2));

static ulonglong read_error_count(THD *thd)
{
  return thd->get_stmt_da()->error_count();
}
// this really belongs to the SHOW STATUS
static Sys_var_session_special Sys_error_count(
       "error_count", "The number of errors that resulted from the "
       "last statement that generated messages",
       READ_ONLY sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONGLONG_MAX), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0), ON_READ(read_error_count));

static ulonglong read_warning_count(THD *thd)
{
  return thd->get_stmt_da()->warn_count();
}
// this really belongs to the SHOW STATUS
static Sys_var_session_special Sys_warning_count(
       "warning_count", "The number of errors, warnings, and notes "
       "that resulted from the last statement that generated messages",
       READ_ONLY sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, ULONGLONG_MAX), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0), ON_READ(read_warning_count));

static Sys_var_ulong Sys_default_week_format(
       "default_week_format",
       "The default week format used by WEEK() functions",
       SESSION_VAR(default_week_format), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 7), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_group_concat_max_len(
       "group_concat_max_len",
       "The maximum length of the result of function GROUP_CONCAT()",
       SESSION_VAR(group_concat_max_len), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(4, SIZE_T_MAX), DEFAULT(1024*1024), BLOCK_SIZE(1));

static char *glob_hostname_ptr;
static Sys_var_charptr Sys_hostname(
       "hostname", "Server host name",
       READ_ONLY GLOBAL_VAR(glob_hostname_ptr), NO_CMD_LINE,
       DEFAULT(glob_hostname));

#ifndef EMBEDDED_LIBRARY
static Sys_var_charptr Sys_repl_report_host(
       "report_host",
       "Hostname or IP of the slave to be reported to the master during "
       "slave registration. Will appear in the output of SHOW SLAVE HOSTS. "
       "Leave unset if you do not want the slave to register itself with the "
       "master. Note that it is not sufficient for the master to simply read "
       "the IP of the slave off the socket once the slave connects. Due to "
       "NAT and other routing issues, that IP may not be valid for connecting "
       "to the slave from the master or other hosts",
       READ_ONLY GLOBAL_VAR(report_host), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_charptr Sys_repl_report_user(
       "report_user",
       "The account user name of the slave to be reported to the master "
       "during slave registration",
       READ_ONLY GLOBAL_VAR(report_user), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_charptr Sys_repl_report_password(
       "report_password",
       "The account password of the slave to be reported to the master "
       "during slave registration",
       READ_ONLY GLOBAL_VAR(report_password), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_uint Sys_repl_report_port(
       "report_port",
       "Port for connecting to slave reported to the master during slave "
       "registration. Set it only if the slave is listening on a non-default "
       "port or if you have a special tunnel from the master or other clients "
       "to the slave. If not sure, leave this option unset",
       READ_ONLY GLOBAL_VAR(report_port), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1));
#endif

static Sys_var_mybool Sys_keep_files_on_create(
       "keep_files_on_create",
       "Don't overwrite stale .MYD and .MYI even if no directory is specified",
       SESSION_VAR(keep_files_on_create), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static char *license;
static Sys_var_charptr Sys_license(
       "license", "The type of license the server has",
       READ_ONLY GLOBAL_VAR(license), NO_CMD_LINE,
       DEFAULT(STRINGIFY_ARG(LICENSE)));

#include <proxy_protocol.h>
char *my_proxy_protocol_networks;
static bool check_proxy_protocol_networks(sys_var *, THD *, set_var *var)
{
  if (!var->value)
    return false;
  return !proxy_protocol_networks_valid(var->save_result.string_value.str);
}


static bool fix_proxy_protocol_networks(sys_var *, THD *, enum_var_type)
{
  return (bool)set_proxy_protocol_networks(my_proxy_protocol_networks);
}


static Sys_var_on_access_global<Sys_var_charptr_fscs,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_PROXY_PROTOCOL_NETWORKS>
Sys_proxy_protocol_networks(
    "proxy_protocol_networks", "Enable proxy protocol for these source "
    "networks. The syntax is a comma separated list of IPv4 and IPv6 "
    "networks. If the network doesn't contain mask, it is considered to be "
    "a single host. \"*\" represents all networks and must the only "
    "directive on the line. String \"localhost\" represents non-TCP "
    "local connections (Unix domain socket, Windows named pipe or shared memory).",
    GLOBAL_VAR(my_proxy_protocol_networks), CMD_LINE(REQUIRED_ARG),
    DEFAULT(""), NO_MUTEX_GUARD, NOT_IN_BINLOG,
    ON_CHECK(check_proxy_protocol_networks), ON_UPDATE(fix_proxy_protocol_networks));


static bool check_log_path(sys_var *self, THD *thd, set_var *var)
{
  if (!var->value)
    return false; // DEFAULT is ok

  if (!var->save_result.string_value.str)
    return true;

  LEX_STRING *val= &var->save_result.string_value;

  if (val->length > FN_REFLEN)
  { // path is too long
    my_error(ER_PATH_LENGTH, MYF(0), self->name.str);
    return true;
  }

  char path[FN_REFLEN];
  size_t path_length= unpack_filename(path, val->str);

  if (!path_length)
    return true;

  if (!is_filename_allowed(var->save_result.string_value.str, 
                           var->save_result.string_value.length, TRUE))
  {
     my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), 
              self->name.str, var->save_result.string_value.str);
     return true;
  }

  static const LEX_CSTRING my_cnf= { STRING_WITH_LEN("my.cnf") };
  static const LEX_CSTRING my_ini= { STRING_WITH_LEN("my.ini") };
  if (path_length >= my_cnf.length)
  {
    if (strcasecmp(path + path_length - my_cnf.length, my_cnf.str) == 0)
      return true; // log file name ends with "my.cnf"
    DBUG_ASSERT(my_cnf.length == my_ini.length);
    if (strcasecmp(path + path_length - my_ini.length, my_ini.str) == 0)
      return true; // log file name ends with "my.ini"
  }

  MY_STAT f_stat;

  if (my_stat(path, &f_stat, MYF(0)))
  {
    if (!MY_S_ISREG(f_stat.st_mode) || !(f_stat.st_mode & MY_S_IWRITE))
      return true; // not a regular writable file
    return false;
  }

  (void) dirname_part(path, val->str, &path_length);

  if (val->length - path_length >= FN_LEN)
  { // filename is too long
      my_error(ER_PATH_LENGTH, MYF(0), self->name.str);
      return true;
  }

  if (!path_length) // no path is good path (remember, relative to datadir)
    return false;

  if (my_access(path, (F_OK|W_OK)))
    return true; // directory is not writable

  return false;
}
static bool fix_log(char** logname, const char* default_logname,
                    const char*ext, bool enabled, void (*reopen)(char*))
{
  if (!*logname) // SET ... = DEFAULT
  {
    make_default_log_name(logname, ext, false);
    if (!*logname)
      return true;
  }
  logger.lock_exclusive();
  mysql_mutex_unlock(&LOCK_global_system_variables);
  if (enabled)
    reopen(*logname);
  logger.unlock();
  mysql_mutex_lock(&LOCK_global_system_variables);
  return false;
}
static void reopen_general_log(char* name)
{
  logger.get_log_file_handler()->close(0);
  logger.get_log_file_handler()->open_query_log(name);
}
static bool fix_general_log_file(sys_var *self, THD *thd, enum_var_type type)
{
  return fix_log(&opt_logname,  opt_log_basename, ".log", opt_log,
                 reopen_general_log);
}
static Sys_var_charptr_fscs Sys_general_log_path(
       "general_log_file", "Log connections and queries to given file",
       PREALLOCATED GLOBAL_VAR(opt_logname), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_log_path), ON_UPDATE(fix_general_log_file));

static void reopen_slow_log(char* name)
{
  logger.get_slow_log_file_handler()->close(0);
  logger.get_slow_log_file_handler()->open_slow_log(name);
}
static bool fix_slow_log_file(sys_var *self, THD *thd, enum_var_type type)
{
  return fix_log(&opt_slow_logname, opt_log_basename, "-slow.log",
                 global_system_variables.sql_log_slow, reopen_slow_log);
}
static Sys_var_charptr_fscs Sys_slow_log_path(
       "slow_query_log_file", "Log slow queries to given log file. "
       "Defaults logging to 'hostname'-slow.log. Must be enabled to activate "
       "other slow log options",
       PREALLOCATED GLOBAL_VAR(opt_slow_logname), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_log_path), ON_UPDATE(fix_slow_log_file));

static Sys_var_have Sys_have_compress(
       "have_compress", "If the zlib compression library is accessible to the "
       "server, this will be set to YES, otherwise it will be NO. The COMPRESS() "
       "and UNCOMPRESS() functions will only be available if set to YES.",
       READ_ONLY GLOBAL_VAR(have_compress), NO_CMD_LINE);

static Sys_var_have Sys_have_crypt(
       "have_crypt", "If the crypt() system call is available this variable will "
       "be set to YES, otherwise it will be set to NO. If set to NO, the "
       "ENCRYPT() function cannot be used.",
       READ_ONLY GLOBAL_VAR(have_crypt), NO_CMD_LINE);

static Sys_var_have Sys_have_dlopen(
       "have_dynamic_loading", "If the server supports dynamic loading of plugins, "
       "will be set to YES, otherwise will be set to NO.",
       READ_ONLY GLOBAL_VAR(have_dlopen), NO_CMD_LINE);

static Sys_var_have Sys_have_geometry(
       "have_geometry", "If the server supports spatial data types, will be set to "
       "YES, otherwise will be set to NO.",
       READ_ONLY GLOBAL_VAR(have_geometry), NO_CMD_LINE);

static Sys_var_have Sys_have_openssl(
       "have_openssl", "Comparing have_openssl with have_ssl will indicate whether "
       "YaSSL or openssl was used. If YaSSL, have_ssl will be YES, but have_openssl "
       "will be NO.",
       READ_ONLY GLOBAL_VAR(have_openssl), NO_CMD_LINE);

static Sys_var_have Sys_have_profiling(
       "have_profiling", "If statement profiling is available, will be set to YES, "
       "otherwise will be set to NO. See SHOW PROFILES and SHOW PROFILE.",
       READ_ONLY GLOBAL_VAR(have_profiling), NO_CMD_LINE);

static Sys_var_have Sys_have_query_cache(
       "have_query_cache", "If the server supports the query cache, will be set to "
       "YES, otherwise will be set to NO.",
       READ_ONLY GLOBAL_VAR(have_query_cache), NO_CMD_LINE);

static Sys_var_have Sys_have_rtree_keys(
       "have_rtree_keys", "If RTREE indexes (used for spatial indexes) "
       "are available, will be set to YES, otherwise will be set to NO.",
       READ_ONLY GLOBAL_VAR(have_rtree_keys), NO_CMD_LINE);

static Sys_var_have Sys_have_ssl(
       "have_ssl", "If the server supports secure connections, will be set to YES, "
       "otherwise will be set to NO. If set to DISABLED, the server was compiled with "
       "TLS support, but was not started with TLS support (see the mysqld options). "
       "See also have_openssl.",
       READ_ONLY GLOBAL_VAR(have_ssl), NO_CMD_LINE);

static Sys_var_have Sys_have_symlink(
       "have_symlink", "If symbolic link support is enabled, will be set to YES, "
       "otherwise will be set to NO. Required for the INDEX DIRECTORY and DATA "
       "DIRECTORY table options (see CREATE TABLE) and Windows symlink support. "
       "Will be set to DISABLED if the server is started with the "
       "--skip-symbolic-links option.",
       READ_ONLY GLOBAL_VAR(have_symlink), NO_CMD_LINE);

#ifdef __SANITIZE_ADDRESS__
static char *have_sanitizer;
static Sys_var_charptr_fscs Sys_have_santitizer(
       "have_sanitizer",
       "If the server is compiled with ASan (Address sanitizer) this will be "
       "set to ASAN",
       READ_ONLY GLOBAL_VAR(have_sanitizer), NO_CMD_LINE,
       DEFAULT("ASAN"));
#endif

static bool fix_log_state(sys_var *self, THD *thd, enum_var_type type);

static Sys_var_mybool Sys_general_log(
       "general_log", "Log connections and queries to a table or log file. "
       "Defaults logging to a file 'hostname'.log or a table mysql.general_log"
       "if --log-output=TABLE is used.",
       GLOBAL_VAR(opt_log), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_log_state));

static Sys_var_mybool Sys_slow_query_log(
       "slow_query_log",
       "Log slow queries to a table or log file. Defaults logging to a file "
       "'hostname'-slow.log or a table mysql.slow_log if --log-output=TABLE is "
       "used. Must be enabled to activate other slow log options.",
       SESSION_VAR(sql_log_slow), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0), ON_UPDATE(fix_log_state));

static bool fix_log_state(sys_var *self, THD *thd, enum_var_type type)
{
  bool res;
  my_bool *UNINIT_VAR(newvalptr), newval, UNINIT_VAR(oldval);
  uint UNINIT_VAR(log_type);

  if (type != OPT_GLOBAL)
    return 0;

  if (self == &Sys_general_log)
  {
    newvalptr= &opt_log;
    oldval=    logger.get_log_file_handler()->is_open();
    log_type=  QUERY_LOG_GENERAL;
  }
  else
  {
    DBUG_ASSERT(self == &Sys_slow_query_log);
    newvalptr= &global_system_variables.sql_log_slow;
    oldval=    logger.get_slow_log_file_handler()->is_open();
    log_type=  QUERY_LOG_SLOW;
  }

  newval= *newvalptr;
  if (oldval == newval)
    return false;

  *newvalptr= oldval; // [de]activate_log_handler works that way (sigh)

  mysql_mutex_unlock(&LOCK_global_system_variables);
  if (!newval)
  {
    logger.deactivate_log_handler(thd, log_type);
    res= false;
  }
  else
    res= logger.activate_log_handler(thd, log_type);
  mysql_mutex_lock(&LOCK_global_system_variables);
  return res;
}


static bool check_not_empty_set(sys_var *self, THD *thd, set_var *var)
{
  return var->save_result.ulonglong_value == 0;
}
static bool fix_log_output(sys_var *self, THD *thd, enum_var_type type)
{
  logger.lock_exclusive();
  logger.init_slow_log(log_output_options);
  logger.init_general_log(log_output_options);
  logger.unlock();
  return false;
}

static const char *log_output_names[] = { "NONE", "FILE", "TABLE", NULL};

static Sys_var_set Sys_log_output(
       "log_output", "How logs should be written",
       GLOBAL_VAR(log_output_options), CMD_LINE(REQUIRED_ARG),
       log_output_names, DEFAULT(LOG_FILE), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_not_empty_set), ON_UPDATE(fix_log_output));

#ifdef HAVE_REPLICATION
static Sys_var_mybool Sys_log_slave_updates(
       "log_slave_updates", "Tells the slave to log the updates from "
       "the slave thread to the binary log. You will need to turn it on if "
       "you plan to daisy-chain the slaves.",
       READ_ONLY GLOBAL_VAR(opt_log_slave_updates), CMD_LINE(OPT_ARG),
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_relay_log(
       "relay_log", "The location and name to use for relay logs.",
       READ_ONLY GLOBAL_VAR(opt_relay_logname), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

/*
  Uses NO_CMD_LINE since the --relay-log-index option set
  opt_relaylog_index_name variable and computes a value for the
  relay_log_index variable.
*/
static Sys_var_charptr_fscs Sys_relay_log_index(
       "relay_log_index", "The location and name to use for the file "
       "that keeps a list of the last relay logs.",
       READ_ONLY GLOBAL_VAR(relay_log_index), NO_CMD_LINE,
       DEFAULT(0));

/*
  Uses NO_CMD_LINE since the --log-bin-index option set
  opt_binlog_index_name variable and computes a value for the
  log_bin_index variable.
*/
static Sys_var_charptr_fscs Sys_binlog_index(
       "log_bin_index", "File that holds the names for last binary log files.",
       READ_ONLY GLOBAL_VAR(log_bin_index), NO_CMD_LINE,
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_relay_log_basename(
       "relay_log_basename",
       "The full path of the relay log file names, excluding the extension.",
       READ_ONLY GLOBAL_VAR(relay_log_basename), NO_CMD_LINE,
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_log_bin_basename(
       "log_bin_basename",
       "The full path of the binary log file names, excluding the extension.",
       READ_ONLY GLOBAL_VAR(log_bin_basename), NO_CMD_LINE,
       DEFAULT(0));

static Sys_var_charptr_fscs Sys_relay_log_info_file(
       "relay_log_info_file", "The location and name of the file that "
       "remembers where the SQL replication thread is in the relay logs.",
       READ_ONLY GLOBAL_VAR(relay_log_info_file), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_RELAY_LOG_PURGE>
Sys_relay_log_purge(
       "relay_log_purge", "if disabled - do not purge relay logs. "
       "if enabled - purge them as soon as they are no more needed.",
       GLOBAL_VAR(relay_log_purge), CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static Sys_var_on_access_global<Sys_var_mybool,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_RELAY_LOG_RECOVERY>
Sys_relay_log_recovery(
       "relay_log_recovery", "Enables automatic relay log recovery "
       "right after the database startup, which means that the IO Thread "
       "starts re-fetching from the master right after the last transaction "
       "processed.",
       GLOBAL_VAR(relay_log_recovery), CMD_LINE(OPT_ARG), DEFAULT(FALSE));


bool Sys_var_rpl_filter::global_update(THD *thd, set_var *var)
{
  bool result= true;                            // Assume error
  LEX_CSTRING *base_name= &var->base;

  if (!base_name->length)
    base_name= &thd->variables.default_master_connection;

  mysql_mutex_unlock(&LOCK_global_system_variables);

  if (Master_info *mi= get_master_info(base_name, !var->base.length ?
                                       Sql_condition::WARN_LEVEL_ERROR :
                                       Sql_condition::WARN_LEVEL_WARN))
  {
    if (mi->rli.slave_running)
    {
      my_error(ER_SLAVE_MUST_STOP, MYF(0), 
               (int) mi->connection_name.length,
               mi->connection_name.str);
      result= true;
    }
    else
    {
      result= set_filter_value(var->save_result.string_value.str, mi);
    }
    mi->release();
  }

  mysql_mutex_lock(&LOCK_global_system_variables);
  return result;
}

bool Sys_var_rpl_filter::set_filter_value(const char *value, Master_info *mi)
{
  bool status= true;
  Rpl_filter* rpl_filter= mi->rpl_filter;

  /* Proctect against other threads */
  mysql_mutex_lock(&LOCK_active_mi);
  switch (opt_id) {
  case OPT_REPLICATE_DO_DB:
    status= rpl_filter->set_do_db(value);
    break;
  case OPT_REPLICATE_DO_TABLE:
    status= rpl_filter->set_do_table(value);
    break;
  case OPT_REPLICATE_IGNORE_DB:
    status= rpl_filter->set_ignore_db(value);
    break;
  case OPT_REPLICATE_IGNORE_TABLE:
    status= rpl_filter->set_ignore_table(value);
    break;
  case OPT_REPLICATE_WILD_DO_TABLE:
    status= rpl_filter->set_wild_do_table(value);
    break;
  case OPT_REPLICATE_WILD_IGNORE_TABLE:
    status= rpl_filter->set_wild_ignore_table(value);
    break;
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  return status;
}

uchar *Sys_var_rpl_filter::global_value_ptr(THD *thd,
                                            const LEX_CSTRING *base_name)
{
  char buf[256];
  String tmp(buf, sizeof(buf), &my_charset_bin);
  uchar *ret;
  Master_info *mi;
  Rpl_filter *rpl_filter;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  mi= get_master_info(base_name, !base_name->length ?
                      Sql_condition::WARN_LEVEL_ERROR :
                      Sql_condition::WARN_LEVEL_WARN);

  if (!mi)
  {
    mysql_mutex_lock(&LOCK_global_system_variables);
    return 0;
  }

  rpl_filter= mi->rpl_filter;
  tmp.length(0);

  mysql_mutex_lock(&LOCK_active_mi);
  switch (opt_id) {
  case OPT_REPLICATE_DO_DB:
    rpl_filter->get_do_db(&tmp);
    break;
  case OPT_REPLICATE_DO_TABLE:
    rpl_filter->get_do_table(&tmp);
    break;
  case OPT_REPLICATE_IGNORE_DB:
    rpl_filter->get_ignore_db(&tmp);
    break;
  case OPT_REPLICATE_IGNORE_TABLE:
    rpl_filter->get_ignore_table(&tmp);
    break;
  case OPT_REPLICATE_WILD_DO_TABLE:
    rpl_filter->get_wild_do_table(&tmp);
    break;
  case OPT_REPLICATE_WILD_IGNORE_TABLE:
    rpl_filter->get_wild_ignore_table(&tmp);
    break;
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  mysql_mutex_lock(&LOCK_global_system_variables);

  mi->release();

  ret= (uchar *) thd->strmake(tmp.ptr(), tmp.length());

  return ret;
}

static Sys_var_rpl_filter Sys_replicate_do_db(
       "replicate_do_db", OPT_REPLICATE_DO_DB,
       "Tell the slave to restrict replication to updates of tables "
       "whose names appear in the comma-separated list. For "
       "statement-based replication, only the default database (that "
       "is, the one selected by USE) is considered, not any explicitly "
       "mentioned tables in the query. For row-based replication, the "
       "actual names of table(s) being updated are checked.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_DO_DB);

static Sys_var_rpl_filter Sys_replicate_do_table(
       "replicate_do_table", OPT_REPLICATE_DO_TABLE,
       "Tells the slave to restrict replication to tables in the "
       "comma-separated list.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_DO_TABLE);

static Sys_var_rpl_filter Sys_replicate_ignore_db(
       "replicate_ignore_db", OPT_REPLICATE_IGNORE_DB,
       "Tell the slave to restrict replication to updates of tables "
       "whose names do not appear in the comma-separated list. For "
       "statement-based replication, only the default database (that "
       "is, the one selected by USE) is considered, not any explicitly "
       "mentioned tables in the query. For row-based replication, the "
       "actual names of table(s) being updated are checked.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_IGNORE_DB);

static Sys_var_rpl_filter Sys_replicate_ignore_table(
       "replicate_ignore_table", OPT_REPLICATE_IGNORE_TABLE,
       "Tells the slave thread not to replicate any statement that "
       "updates the specified table, even if any other tables might be "
       "updated by the same statement.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_IGNORE_TABLE);

static Sys_var_rpl_filter Sys_replicate_wild_do_table(
       "replicate_wild_do_table", OPT_REPLICATE_WILD_DO_TABLE,
       "Tells the slave thread to restrict replication to statements "
       "where any of the updated tables match the specified database "
       "and table name patterns.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_WILD_DO_TABLE);

static Sys_var_rpl_filter Sys_replicate_wild_ignore_table(
       "replicate_wild_ignore_table", OPT_REPLICATE_WILD_IGNORE_TABLE,
       "Tells the slave thread to not replicate to the tables that "
       "match the given wildcard pattern.",
       PRIV_SET_SYSTEM_GLOBAL_VAR_REPLICATE_WILD_IGNORE_TABLE);

static Sys_var_charptr_fscs Sys_slave_load_tmpdir(
       "slave_load_tmpdir", "The location where the slave should put "
       "its temporary files when replicating a LOAD DATA INFILE command",
       READ_ONLY GLOBAL_VAR(slave_load_tmpdir), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_NET_TIMEOUT>
Sys_slave_net_timeout(
       "slave_net_timeout", "Number of seconds to wait for more data "
       "from any master/slave connection before aborting the read",
       GLOBAL_VAR(slave_net_timeout), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, LONG_TIMEOUT), DEFAULT(SLAVE_NET_TIMEOUT), BLOCK_SIZE(1));


/*
  Access a multi_source variable
  Return 0 + warning if it doesn't exist
*/

ulonglong Sys_var_multi_source_ulonglong::
get_master_info_ulonglong_value(THD *thd, ptrdiff_t offset)
{
  Master_info *mi;
  ulonglong res= 0;                                  // Default value
  mysql_mutex_unlock(&LOCK_global_system_variables);
  if ((mi= get_master_info(&thd->variables.default_master_connection,
                           Sql_condition::WARN_LEVEL_WARN)))
  {
    res= *((ulonglong*) (((uchar*) mi) + master_info_offset));
    mi->release();
  }
  mysql_mutex_lock(&LOCK_global_system_variables);
  return res;
}
  

bool update_multi_source_variable(sys_var *self_var, THD *thd,
                                  enum_var_type type)
{
  Sys_var_multi_source_ulonglong *self= (Sys_var_multi_source_ulonglong*) self_var;
  bool result= true;
  Master_info *mi;

  if (type == OPT_GLOBAL)
    mysql_mutex_unlock(&LOCK_global_system_variables);
  if ((mi= (get_master_info(&thd->variables.default_master_connection,
                            Sql_condition::WARN_LEVEL_ERROR))))
  {
    mysql_mutex_lock(&mi->rli.run_lock);
    mysql_mutex_lock(&mi->rli.data_lock);
    result= self->update_variable(thd, mi);
    mysql_mutex_unlock(&mi->rli.data_lock);
    mysql_mutex_unlock(&mi->rli.run_lock);
    mi->release();
  }
  if (type == OPT_GLOBAL)
    mysql_mutex_lock(&LOCK_global_system_variables);
  return result;
}

static bool update_slave_skip_counter(sys_var *self, THD *thd, Master_info *mi)
{
  if (mi->rli.slave_running)
  {
    my_error(ER_SLAVE_MUST_STOP, MYF(0), (int) mi->connection_name.length,
             mi->connection_name.str);
    return true;
  }
  if (mi->using_gtid != Master_info::USE_GTID_NO && mi->using_parallel())
  {
    ulong domain_count;
    mysql_mutex_lock(&rpl_global_gtid_slave_state->LOCK_slave_state);
    domain_count= rpl_global_gtid_slave_state->count();
    mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
    if (domain_count > 1)
    {
      /*
        With domain-based parallel replication, the slave position is
        multi-dimensional, so the relay log position is not very meaningful.
        It might not even correspond to the next GTID to execute in _any_
        domain (the case after error stop). So slave_skip_counter will most
        likely not do what the user intends. Instead give an error, with a
        suggestion to instead set @@gtid_slave_pos past the point of error;
        this works reliably also in the case of multiple domains.
      */
      my_error(ER_SLAVE_SKIP_NOT_IN_GTID, MYF(0));
      return true;
    }
  }

  /* The value was stored temporarily in thd */
  mi->rli.slave_skip_counter= thd->variables.slave_skip_counter;
  return false;
}

static Sys_var_multi_source_ulonglong Sys_slave_skip_counter(
       "sql_slave_skip_counter", "Skip the next N events from the master log",
       SESSION_VAR(slave_skip_counter), NO_CMD_LINE,
       MASTER_INFO_VAR(rli.slave_skip_counter),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1),
       ON_UPDATE(update_slave_skip_counter));

static bool update_max_relay_log_size(sys_var *self, THD *thd, Master_info *mi)
{
  mi->rli.max_relay_log_size= thd->variables.max_relay_log_size;
  mi->rli.relay_log.set_max_size((ulong)mi->rli.max_relay_log_size);
  return false;
}

static Sys_var_multi_source_ulonglong Sys_max_relay_log_size(
       "max_relay_log_size",
       "relay log will be rotated automatically when the size exceeds this "
       "value.  If 0 at startup, it's set to max_binlog_size",
       SESSION_VAR(max_relay_log_size), CMD_LINE(REQUIRED_ARG),
       MASTER_INFO_VAR(rli.max_relay_log_size),
       VALID_RANGE(0, 1024L*1024*1024), DEFAULT(0), BLOCK_SIZE(IO_SIZE),
       ON_UPDATE(update_max_relay_log_size));

static Sys_var_charptr Sys_slave_skip_errors(
       "slave_skip_errors", "Tells the slave thread to continue "
       "replication when a query event returns an error from the "
       "provided list",
       READ_ONLY GLOBAL_VAR(opt_slave_skip_errors), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_on_access_global<Sys_var_ulonglong,
                            PRIV_SET_SYSTEM_GLOBAL_VAR_READ_BINLOG_SPEED_LIMIT>
Sys_read_binlog_speed_limit(
       "read_binlog_speed_limit", "Maximum speed(KB/s) to read binlog from"
       " master (0 = no limit)",
       GLOBAL_VAR(opt_read_binlog_speed_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, ULONG_MAX), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_charptr Sys_slave_transaction_retry_errors(
       "slave_transaction_retry_errors", "Tells the slave thread to retry "
       "transaction for replication when a query event returns an error from "
       "the provided list. Deadlock error, elapsed lock wait timeout, "
       "net read error, net read timeout, net write error, net write timeout, "
       "connect error and 2 types of lost connection error are automatically "
       "added to this list",
       READ_ONLY GLOBAL_VAR(opt_slave_transaction_retry_errors), CMD_LINE(REQUIRED_ARG),
       DEFAULT(0));

static Sys_var_ulonglong Sys_relay_log_space_limit(
       "relay_log_space_limit", "Maximum space to use for all relay logs",
       READ_ONLY GLOBAL_VAR(relay_log_space_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, ULONGLONG_MAX), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_RELAY_LOG>
Sys_sync_relaylog_period(
       "sync_relay_log", "Synchronously flush relay log to disk after "
       "every #th event. Use 0 to disable synchronous flushing",
       GLOBAL_VAR(sync_relaylog_period), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(10000), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_RELAY_LOG_INFO>
Sys_sync_relayloginfo_period(
       "sync_relay_log_info", "Synchronously flush relay log info "
       "to disk after every #th transaction. Use 0 to disable "
       "synchronous flushing",
       GLOBAL_VAR(sync_relayloginfo_period), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(10000), BLOCK_SIZE(1));
#endif

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_BINLOG>
Sys_sync_binlog_period(
       "sync_binlog", "Synchronously flush binary log to disk after "
       "every #th event. Use 0 (default) to disable synchronous flushing",
       GLOBAL_VAR(sync_binlog_period), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_uint,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_SYNC_MASTER_INFO>
Sys_sync_masterinfo_period(
       "sync_master_info", "Synchronously flush master info to disk "
       "after every #th event. Use 0 to disable synchronous flushing",
       GLOBAL_VAR(sync_masterinfo_period), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(10000), BLOCK_SIZE(1));

#ifdef HAVE_REPLICATION
static Sys_var_ulong Sys_slave_trans_retries(
       "slave_transaction_retries", "Number of times the slave SQL "
       "thread will retry a transaction in case it failed with a deadlock, "
       "elapsed lock wait timeout or listed in "
       "slave_transaction_retry_errors, before giving up and stopping",
       GLOBAL_VAR(slave_trans_retries), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(10), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_ulong,
                    PRIV_SET_SYSTEM_GLOBAL_VAR_SLAVE_TRANSACTION_RETRY_INTERVAL>
Sys_slave_trans_retry_interval(
       "slave_transaction_retry_interval", "Interval of the slave SQL "
       "thread will retry a transaction in case it failed with a deadlock "
       "or elapsed lock wait timeout or listed in "
       "slave_transaction_retry_errors",
       GLOBAL_VAR(slave_trans_retry_interval), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 3600), DEFAULT(0), BLOCK_SIZE(1));
#endif

static bool check_locale(sys_var *self, THD *thd, set_var *var)
{
  if (!var->value)
    return false;

  MY_LOCALE *locale;
  char buff[STRING_BUFFER_USUAL_SIZE];
  if (var->value->result_type() == INT_RESULT)
  {
    int lcno= (int)var->value->val_int();
    if (!(locale= my_locale_by_number(lcno)))
    {
      my_error(ER_UNKNOWN_LOCALE, MYF(0), llstr(lcno, buff));
      return true;
    }
    if (check_not_null(self, thd, var))
      return true;
  }
  else // STRING_RESULT
  {
    String str(buff, sizeof(buff), system_charset_info), *res;
    if (!(res=var->value->val_str(&str)))
      return true;
    else if (!(locale= my_locale_by_name(res->c_ptr_safe())))
    {
      ErrConvString err(res);
      my_error(ER_UNKNOWN_LOCALE, MYF(0), err.ptr());
      return true;
    }
  }

  var->save_result.ptr= locale;

  if (!locale->errmsgs->errmsgs)
  {
    bool res;
    mysql_mutex_lock(&LOCK_error_messages);
    res= (!locale->errmsgs->errmsgs &&
          read_texts(ERRMSG_FILE, locale->errmsgs->language,
                     &locale->errmsgs->errmsgs));
    mysql_mutex_unlock(&LOCK_error_messages);
    if (res)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                          "Can't process error message file for locale '%s'",
                          locale->name);
      return true;
    }
  }
  status_var_increment(thd->status_var.feature_locale);
  return false;
}

static bool update_locale(sys_var *self, THD* thd, enum_var_type type)
{
  /* Cache pointer to error messages */
  if (type == OPT_SESSION)
    thd->variables.errmsgs= thd->variables.lc_messages->errmsgs->errmsgs;
  else
    global_system_variables.errmsgs=
      global_system_variables.lc_messages->errmsgs->errmsgs;
  return false;
}
  
static Sys_var_struct Sys_lc_messages(
       "lc_messages", "Set the language used for the error messages",
       SESSION_VAR(lc_messages), NO_CMD_LINE,
       my_offsetof(MY_LOCALE, name), DEFAULT(&my_default_lc_messages),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_locale), ON_UPDATE(update_locale));

static Sys_var_struct Sys_lc_time_names(
       "lc_time_names", "Set the language used for the month "
       "names and the days of the week",
       SESSION_VAR(lc_time_names), NO_CMD_LINE,
       my_offsetof(MY_LOCALE, name), DEFAULT(&my_default_lc_time_names),
       NO_MUTEX_GUARD, IN_BINLOG, ON_CHECK(check_locale));

static Sys_var_tz Sys_time_zone(
       "time_zone", "The current time zone, used to initialize the time "
       "zone for a client when it connects. Set to SYSTEM by default, in "
       "which the client uses the system time zone value.",
       SESSION_VAR(time_zone), NO_CMD_LINE,
       DEFAULT(&default_tz), NO_MUTEX_GUARD, IN_BINLOG);

#ifdef WITH_WSREP
#include "wsrep_var.h"
#include "wsrep_sst.h"
#include "wsrep_binlog.h"

static Sys_var_charptr_fscs Sys_wsrep_provider(
       "wsrep_provider", "Path to replication provider library",
       PREALLOCATED GLOBAL_VAR(wsrep_provider), CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_NONE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_provider_check), ON_UPDATE(wsrep_provider_update));

static Sys_var_charptr Sys_wsrep_provider_options(
       "wsrep_provider_options", "Semicolon (;) separated list of wsrep "
       "options (see wsrep_provider_options documentation).",
       PREALLOCATED GLOBAL_VAR(wsrep_provider_options), 
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(""), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_provider_options_check), 
       ON_UPDATE(wsrep_provider_options_update));

static Sys_var_charptr_fscs Sys_wsrep_data_home_dir(
       "wsrep_data_home_dir", "home directory for wsrep provider",
       READ_ONLY GLOBAL_VAR(wsrep_data_home_dir), CMD_LINE(REQUIRED_ARG),
       DEFAULT(mysql_real_data_home));

static Sys_var_charptr Sys_wsrep_cluster_name(
       "wsrep_cluster_name", "Name for the cluster",
       PREALLOCATED GLOBAL_VAR(wsrep_cluster_name), CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_CLUSTER_NAME),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_cluster_name_check),
       ON_UPDATE(wsrep_cluster_name_update));

static PolyLock_mutex PLock_wsrep_cluster_config(&LOCK_wsrep_cluster_config);
static Sys_var_charptr Sys_wsrep_cluster_address (
       "wsrep_cluster_address", "Address to initially connect to cluster",
       PREALLOCATED GLOBAL_VAR(wsrep_cluster_address), 
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(""),
       &PLock_wsrep_cluster_config, NOT_IN_BINLOG,
       ON_CHECK(wsrep_cluster_address_check), 
       ON_UPDATE(wsrep_cluster_address_update));

static Sys_var_charptr Sys_wsrep_node_name (
       "wsrep_node_name", "Name of this node. This name can be used in "
       "wsrep_sst_donor as a preferred donor. Note that multiple nodes "
       "in a cluster can have the same name.",
       PREALLOCATED GLOBAL_VAR(wsrep_node_name), CMD_LINE(REQUIRED_ARG),
       DEFAULT(glob_hostname), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       wsrep_node_name_check, wsrep_node_name_update);

static Sys_var_charptr Sys_wsrep_node_address (
       "wsrep_node_address", "Specifies the node's network address, in "
       "the format ip address[:port]. Used in situations where autoguessing "
       "is not reliable. As of MariaDB 10.1.8, supports IPv6.",
       PREALLOCATED GLOBAL_VAR(wsrep_node_address), CMD_LINE(REQUIRED_ARG),
       DEFAULT(""),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_node_address_check),
       ON_UPDATE(wsrep_node_address_update));

static Sys_var_charptr Sys_wsrep_node_incoming_address(
       "wsrep_node_incoming_address", "Client connection address",
       PREALLOCATED GLOBAL_VAR(wsrep_node_incoming_address),CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_NODE_INCOMING_AUTO));

static Sys_var_ulong Sys_wsrep_slave_threads(
       "wsrep_slave_threads", "Number of slave appliers to launch",
       GLOBAL_VAR(wsrep_slave_threads), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 512), DEFAULT(1), BLOCK_SIZE(1),
       &PLock_wsrep_cluster_config, NOT_IN_BINLOG,
       ON_CHECK(0),
       ON_UPDATE(wsrep_slave_threads_update));

static Sys_var_charptr Sys_wsrep_dbug_option(
       "wsrep_dbug_option", "DBUG options to provider library",
       GLOBAL_VAR(wsrep_dbug_option),CMD_LINE(REQUIRED_ARG),
       DEFAULT(""));

static const char *wsrep_debug_names[]=
{ "NONE", "SERVER", "TRANSACTION", "STREAMING", "CLIENT", NullS };
static Sys_var_enum Sys_wsrep_debug(
       "wsrep_debug", "WSREP debug level logging",
       GLOBAL_VAR(wsrep_debug), CMD_LINE(REQUIRED_ARG),
       wsrep_debug_names, DEFAULT(0),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0), ON_UPDATE(wsrep_debug_update));

static Sys_var_mybool Sys_wsrep_convert_LOCK_to_trx(
       "wsrep_convert_LOCK_to_trx", "To convert locking sessions "
       "into transactions",
       GLOBAL_VAR(wsrep_convert_LOCK_to_trx), 
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_ulong Sys_wsrep_retry_autocommit(
      "wsrep_retry_autocommit", "Max number of times to retry "
      "a failed autocommit statement",
       SESSION_VAR(wsrep_retry_autocommit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 10000), DEFAULT(1), BLOCK_SIZE(1));

static bool update_wsrep_auto_increment_control (sys_var *self, THD *thd, enum_var_type type)
{
  if (wsrep_auto_increment_control)
  {
    /*
      The variables that control auto increment shall be calculated
      automaticaly based on the size of the cluster. This usually done
      within the wsrep_view_handler_cb callback. However, if the user
      manually sets the value of wsrep_auto_increment_control to 'ON',
      then we should to re-calculate these variables again (because
      these values may be required before wsrep_view_handler_cb will
      be re-invoked, which is rarely invoked if the cluster stays in
      the stable state):
    */
    global_system_variables.auto_increment_increment=
       wsrep_cluster_size ? wsrep_cluster_size : 1;
    global_system_variables.auto_increment_offset=
       wsrep_local_index >= 0 ? wsrep_local_index + 1 : 1;
    thd->variables.auto_increment_increment=
      global_system_variables.auto_increment_increment;
    thd->variables.auto_increment_offset=
      global_system_variables.auto_increment_offset;
  }
  else
  {
    /*
      We must restore the last values of the variables that
      are explicitly specified by the user:
    */
    global_system_variables.auto_increment_increment=
      global_system_variables.saved_auto_increment_increment;
    global_system_variables.auto_increment_offset=
      global_system_variables.saved_auto_increment_offset;
    thd->variables.auto_increment_increment=
      thd->variables.saved_auto_increment_increment;
    thd->variables.auto_increment_offset=
      thd->variables.saved_auto_increment_offset;
  }
  return false;
}

static Sys_var_mybool Sys_wsrep_auto_increment_control(
       "wsrep_auto_increment_control", "To automatically control the "
       "assignment of autoincrement variables",
       GLOBAL_VAR(wsrep_auto_increment_control), 
       CMD_LINE(OPT_ARG), DEFAULT(TRUE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_wsrep_auto_increment_control));

static Sys_var_mybool Sys_wsrep_drupal_282555_workaround(
       "wsrep_drupal_282555_workaround", "Enable a workaround to handle the "
       "cases where inserting a DEFAULT value into an auto-increment column "
       "could fail with duplicate key error",
       GLOBAL_VAR(wsrep_drupal_282555_workaround),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_charptr sys_wsrep_sst_method(
       "wsrep_sst_method", "State snapshot transfer method",
       GLOBAL_VAR(wsrep_sst_method),CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_SST_DEFAULT), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_sst_method_check));

static Sys_var_charptr Sys_wsrep_sst_receive_address( 
       "wsrep_sst_receive_address", "Address where node is waiting for "
       "SST contact", 
       GLOBAL_VAR(wsrep_sst_receive_address),CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_SST_ADDRESS_AUTO), NO_MUTEX_GUARD,
       NOT_IN_BINLOG,
       ON_CHECK(wsrep_sst_receive_address_check),
       ON_UPDATE(wsrep_sst_receive_address_update)); 

static Sys_var_charptr Sys_wsrep_sst_auth(
       "wsrep_sst_auth", "Authentication for SST connection",
       PREALLOCATED GLOBAL_VAR(wsrep_sst_auth), CMD_LINE(REQUIRED_ARG),
       DEFAULT(NULL), NO_MUTEX_GUARD,
       NOT_IN_BINLOG,
       ON_CHECK(wsrep_sst_auth_check),
       ON_UPDATE(wsrep_sst_auth_update)); 

static Sys_var_charptr Sys_wsrep_sst_donor(
       "wsrep_sst_donor", "preferred donor node for the SST",
       GLOBAL_VAR(wsrep_sst_donor),CMD_LINE(REQUIRED_ARG),
       DEFAULT(""), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_sst_donor_check),
       ON_UPDATE(wsrep_sst_donor_update)); 

static Sys_var_mybool Sys_wsrep_sst_donor_rejects_queries(
       "wsrep_sst_donor_rejects_queries", "Reject client queries "
       "when donating state snapshot transfer", 
       GLOBAL_VAR(wsrep_sst_donor_rejects_queries), 
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_wsrep_on (
       "wsrep_on", "To enable wsrep replication ",
       SESSION_VAR(wsrep_on), 
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_on_check),
       ON_UPDATE(wsrep_on_update));

static Sys_var_charptr Sys_wsrep_start_position (
       "wsrep_start_position", "global transaction position to start from ",
       PREALLOCATED GLOBAL_VAR(wsrep_start_position), 
       CMD_LINE(REQUIRED_ARG),
       DEFAULT(WSREP_START_POSITION_ZERO),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_start_position_check), 
       ON_UPDATE(wsrep_start_position_update));

static Sys_var_ulong Sys_wsrep_max_ws_size (
       "wsrep_max_ws_size", "Max write set size (bytes)",
       GLOBAL_VAR(wsrep_max_ws_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, WSREP_MAX_WS_SIZE), DEFAULT(WSREP_MAX_WS_SIZE),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_max_ws_size_check), ON_UPDATE(wsrep_max_ws_size_update));

static Sys_var_ulong Sys_wsrep_max_ws_rows (
       "wsrep_max_ws_rows", "Max number of rows in write set",
       GLOBAL_VAR(wsrep_max_ws_rows), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 1048576), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_charptr Sys_wsrep_notify_cmd(
       "wsrep_notify_cmd", "",
       GLOBAL_VAR(wsrep_notify_cmd),CMD_LINE(REQUIRED_ARG),
       DEFAULT(""));

static Sys_var_mybool Sys_wsrep_certify_nonPK(
       "wsrep_certify_nonPK", "Certify tables with no primary key",
       GLOBAL_VAR(wsrep_certify_nonPK), 
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static const char *wsrep_certification_rules_names[]= { "strict", "optimized", NullS };
static Sys_var_enum Sys_wsrep_certification_rules(
       "wsrep_certification_rules",
       "Certification rules to use in the cluster. Possible values are: "
       "\"strict\": stricter rules that could result in more certification "
       "failures. "
       "\"optimized\": relaxed rules that allow more concurrency and "
       "cause less certification failures.",
       GLOBAL_VAR(wsrep_certification_rules), CMD_LINE(REQUIRED_ARG),
       wsrep_certification_rules_names, DEFAULT(WSREP_CERTIFICATION_RULES_STRICT),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(0));

static Sys_var_mybool Sys_wsrep_causal_reads(
       "wsrep_causal_reads", "Setting this variable is equivalent "
       "to setting wsrep_sync_wait READ flag",
       SESSION_VAR(wsrep_causal_reads),
       CMD_LINE(OPT_ARG, OPT_WSREP_CAUSAL_READS), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(wsrep_causal_reads_update),
       DEPRECATED("'@@wsrep_sync_wait=1'")); // since 10.1.3

static Sys_var_uint Sys_wsrep_sync_wait(
       "wsrep_sync_wait", "Ensure \"synchronous\" read view before executing "
       "an operation of the type specified by bitmask: 1 - READ(includes "
       "SELECT, SHOW and BEGIN/START TRANSACTION); 2 - UPDATE and DELETE; 4 - "
       "INSERT and REPLACE",
       SESSION_VAR(wsrep_sync_wait), CMD_LINE(OPT_ARG, OPT_WSREP_SYNC_WAIT),
       VALID_RANGE(WSREP_SYNC_WAIT_NONE, WSREP_SYNC_WAIT_MAX),
       DEFAULT(WSREP_SYNC_WAIT_NONE), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(wsrep_sync_wait_update));

static const char *wsrep_OSU_method_names[]= { "TOI", "RSU", NullS };
static Sys_var_enum Sys_wsrep_OSU_method(
       "wsrep_OSU_method", "Method for Online Schema Upgrade",
       SESSION_VAR(wsrep_OSU_method), CMD_LINE(OPT_ARG),
       wsrep_OSU_method_names, DEFAULT(WSREP_OSU_TOI));

static PolyLock_mutex PLock_wsrep_desync(&LOCK_wsrep_desync);
static Sys_var_mybool Sys_wsrep_desync (
       "wsrep_desync", "To desynchronize the node from the cluster",
       GLOBAL_VAR(wsrep_desync),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       &PLock_wsrep_desync, NOT_IN_BINLOG,
       ON_CHECK(wsrep_desync_check),
       ON_UPDATE(wsrep_desync_update));

static Sys_var_mybool Sys_wsrep_strict_ddl (
       "wsrep_strict_ddl", "If set, reject DDL on affected tables not supporting Galera replication",
       GLOBAL_VAR(wsrep_strict_ddl),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0),
       ON_UPDATE(0));

static const char *wsrep_reject_queries_names[]= { "NONE", "ALL", "ALL_KILL", NullS };
static Sys_var_enum Sys_wsrep_reject_queries(
       "wsrep_reject_queries", "Variable to set to reject queries",
       GLOBAL_VAR(wsrep_reject_queries), CMD_LINE(OPT_ARG),
       wsrep_reject_queries_names, DEFAULT(WSREP_REJECT_NONE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(wsrep_reject_queries_update));

static const char *wsrep_binlog_format_names[]=
       {"MIXED", "STATEMENT", "ROW", "NONE", NullS};
static Sys_var_enum Sys_wsrep_forced_binlog_format(
       "wsrep_forced_binlog_format", "binlog format to take effect over user's choice",
       GLOBAL_VAR(wsrep_forced_binlog_format), CMD_LINE(REQUIRED_ARG),
       wsrep_binlog_format_names, DEFAULT(BINLOG_FORMAT_UNSPEC));

static Sys_var_mybool Sys_wsrep_recover_datadir(
       "wsrep_recover", "Recover database state after crash and exit",
       READ_ONLY GLOBAL_VAR(wsrep_recovery),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_wsrep_replicate_myisam(
       "wsrep_replicate_myisam", "To enable myisam replication",
       GLOBAL_VAR(wsrep_replicate_myisam), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_wsrep_log_conflicts(
       "wsrep_log_conflicts", "To log multi-master conflicts",
       GLOBAL_VAR(wsrep_log_conflicts), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_ulong Sys_wsrep_mysql_replication_bundle(
      "wsrep_mysql_replication_bundle", "mysql replication group commit ",
       GLOBAL_VAR(wsrep_mysql_replication_bundle), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 1000), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_mybool Sys_wsrep_load_data_splitting(
       "wsrep_load_data_splitting", "To commit LOAD DATA "
       "transaction after every 10K rows inserted (deprecated)",
       GLOBAL_VAR(wsrep_load_data_splitting), 
       CMD_LINE(OPT_ARG), DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0), ON_UPDATE(0), DEPRECATED("")); // since 10.4.3

static Sys_var_mybool Sys_wsrep_slave_FK_checks(
       "wsrep_slave_FK_checks", "Should slave thread do "
       "foreign key constraint checks",
       GLOBAL_VAR(wsrep_slave_FK_checks), 
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static Sys_var_mybool Sys_wsrep_slave_UK_checks(
       "wsrep_slave_UK_checks", "Should slave thread do "
       "secondary index uniqueness checks",
       GLOBAL_VAR(wsrep_slave_UK_checks), 
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_wsrep_restart_slave(
       "wsrep_restart_slave", "Should MariaDB slave be restarted automatically, when node joins back to cluster",
       GLOBAL_VAR(wsrep_restart_slave), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_ulonglong Sys_wsrep_trx_fragment_size(
      "wsrep_trx_fragment_size",
      "Size of transaction fragments for streaming replication (measured in "
      "units of 'wsrep_trx_fragment_unit')",
      SESSION_VAR(wsrep_trx_fragment_size), CMD_LINE(REQUIRED_ARG),
      VALID_RANGE(0, WSREP_MAX_WS_SIZE), DEFAULT(0), BLOCK_SIZE(1),
      NO_MUTEX_GUARD, NOT_IN_BINLOG,
      ON_CHECK(wsrep_trx_fragment_size_check),
      ON_UPDATE(wsrep_trx_fragment_size_update));

extern const char *wsrep_fragment_units[];

static Sys_var_enum Sys_wsrep_trx_fragment_unit(
      "wsrep_trx_fragment_unit",
      "Unit for streaming replication transaction fragments' size: bytes, "
      "rows, statements",
      SESSION_VAR(wsrep_trx_fragment_unit), CMD_LINE(REQUIRED_ARG),
      wsrep_fragment_units,
      DEFAULT(WSREP_FRAG_BYTES),
      NO_MUTEX_GUARD, NOT_IN_BINLOG,
      ON_CHECK(0),
      ON_UPDATE(wsrep_trx_fragment_unit_update));

extern const char *wsrep_SR_store_types[];
static Sys_var_enum Sys_wsrep_SR_store(
       "wsrep_SR_store", "Storage for streaming replication fragments",
       READ_ONLY GLOBAL_VAR(wsrep_SR_store_type), CMD_LINE(REQUIRED_ARG),
       wsrep_SR_store_types, DEFAULT(WSREP_SR_STORE_TABLE));

static Sys_var_mybool Sys_wsrep_dirty_reads(
       "wsrep_dirty_reads",
       "Allow reads even when the node is not in the primary component.",
       SESSION_VAR(wsrep_dirty_reads), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static Sys_var_uint Sys_wsrep_ignore_apply_errors (
       "wsrep_ignore_apply_errors", "Ignore replication errors",
       GLOBAL_VAR(wsrep_ignore_apply_errors), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(WSREP_IGNORE_ERRORS_NONE, WSREP_IGNORE_ERRORS_MAX),
       DEFAULT(7), BLOCK_SIZE(1));

static Sys_var_uint Sys_wsrep_gtid_domain_id(
       "wsrep_gtid_domain_id", "When wsrep_gtid_mode is set, this value is "
       "used as gtid_domain_id for galera transactions and also copied to the "
       "joiner nodes during state transfer. It is ignored, otherwise.",
       GLOBAL_VAR(wsrep_gtid_server.domain_id), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX32), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_ulonglong Sys_wsrep_gtid_seq_no(
       "wsrep_gtid_seq_no",
       "Internal server usage, manually set WSREP GTID seqno.",
       SESSION_ONLY(wsrep_gtid_seq_no),
       NO_CMD_LINE, VALID_RANGE(0, ULONGLONG_MAX), DEFAULT(0),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(wsrep_gtid_seq_no_check));

static Sys_var_mybool Sys_wsrep_gtid_mode(
       "wsrep_gtid_mode", "Automatically update the (joiner) node's "
       "wsrep_gtid_domain_id value with that of donor's (received during "
       "state transfer) and use it in place of gtid_domain_id for all galera "
       "transactions. When OFF (default), wsrep_gtid_domain_id is simply "
       "ignored (backward compatibility).",
       GLOBAL_VAR(wsrep_gtid_mode), CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static char *wsrep_patch_version_ptr;
static Sys_var_charptr Sys_wsrep_patch_version(
       "wsrep_patch_version", "Wsrep patch version, for example wsrep_25.10.",
       READ_ONLY GLOBAL_VAR(wsrep_patch_version_ptr), CMD_LINE_HELP_ONLY,
       DEFAULT(WSREP_PATCH_VERSION));

#endif /* WITH_WSREP */

static bool fix_host_cache_size(sys_var *, THD *, enum_var_type)
{
  hostname_cache_resize((uint) host_cache_size);
  return false;
}

static Sys_var_ulong Sys_host_cache_size(
       "host_cache_size",
       "How many host names should be cached to avoid resolving.",
       AUTO_SET GLOBAL_VAR(host_cache_size),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, 65536),
       DEFAULT(HOST_CACHE_SIZE), BLOCK_SIZE(1),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(fix_host_cache_size));

vio_keepalive_opts opt_vio_keepalive;

static Sys_var_int Sys_keepalive_time(
       "tcp_keepalive_time",
       "Timeout, in seconds, with no activity until the first TCP keep-alive packet is sent."
       "If set to 0, system dependent default is used.",
       AUTO_SET GLOBAL_VAR(opt_vio_keepalive.idle),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, INT_MAX32/1000), DEFAULT(0),
       BLOCK_SIZE(1));

static Sys_var_int Sys_keepalive_interval(
       "tcp_keepalive_interval",
       "The interval, in seconds, between when successive keep-alive packets are sent if no acknowledgement is received."
       "If set to 0, system dependent default is used.",
       AUTO_SET GLOBAL_VAR(opt_vio_keepalive.interval),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, INT_MAX32/1000), DEFAULT(0),
       BLOCK_SIZE(1));

static Sys_var_int Sys_keepalive_probes(
       "tcp_keepalive_probes",
       "The number of unacknowledged probes to send before considering the connection dead and notifying the application layer."
       "If set to 0, system dependent default is used.",
       AUTO_SET GLOBAL_VAR(opt_vio_keepalive.probes),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(0, INT_MAX32/1000), DEFAULT(0),
       BLOCK_SIZE(1));


static bool update_tcp_nodelay(sys_var *self, THD *thd,
  enum_var_type type)
{
  DBUG_ASSERT(thd);

  Vio *vio = thd->net.vio;
  if (vio)
    return (MY_TEST(vio_nodelay(vio, thd->variables.tcp_nodelay)));

  return false;
}

static Sys_var_mybool Sys_tcp_nodelay(
       "tcp_nodelay",
       "Set option TCP_NODELAY (disable Nagle's algorithm) on socket",
       SESSION_VAR(tcp_nodelay), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE),NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(check_session_only_variable),
       ON_UPDATE(update_tcp_nodelay));

static Sys_var_charptr_fscs Sys_ignore_db_dirs(
       "ignore_db_dirs",
       "Specifies a directory to add to the ignore list when collecting "
       "database names from the datadir. Put a blank argument to reset "
       "the list accumulated so far.",
       READ_ONLY GLOBAL_VAR(opt_ignore_db_dirs), 
       CMD_LINE(REQUIRED_ARG, OPT_IGNORE_DB_DIRECTORY),
       DEFAULT(0));

static Sys_var_ulong Sys_sp_cache_size(
       "stored_program_cache",
       "The soft upper limit for number of cached stored routines for "
       "one connection.",
       GLOBAL_VAR(stored_program_cache_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 512 * 1024), DEFAULT(256), BLOCK_SIZE(1));

export const char *plugin_maturity_names[]=
{ "unknown", "experimental", "alpha", "beta", "gamma", "stable", 0 };
static Sys_var_enum Sys_plugin_maturity(
       "plugin_maturity",
       "The lowest desirable plugin maturity. "
       "Plugins less mature than that will not be installed or loaded",
       READ_ONLY GLOBAL_VAR(plugin_maturity), CMD_LINE(REQUIRED_ARG),
       plugin_maturity_names,
       DEFAULT(SERVER_MATURITY_LEVEL > 0 ?
               SERVER_MATURITY_LEVEL - 1 : SERVER_MATURITY_LEVEL));

static Sys_var_ulong Sys_deadlock_search_depth_short(
       "deadlock_search_depth_short",
       "Short search depth for the two-step deadlock detection",
       SESSION_VAR(wt_deadlock_search_depth_short), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 32), DEFAULT(4), BLOCK_SIZE(1));

static Sys_var_ulong Sys_deadlock_search_depth_long(
       "deadlock_search_depth_long",
       "Long search depth for the two-step deadlock detection",
       SESSION_VAR(wt_deadlock_search_depth_long), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 33), DEFAULT(15), BLOCK_SIZE(1));

static Sys_var_ulong Sys_deadlock_timeout_depth_short(
       "deadlock_timeout_short",
       "Short timeout for the two-step deadlock detection (in microseconds)",
       SESSION_VAR(wt_timeout_short), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(10000), BLOCK_SIZE(1));

static Sys_var_ulong Sys_deadlock_timeout_depth_long(
       "deadlock_timeout_long",
       "Long timeout for the two-step deadlock detection (in microseconds)",
       SESSION_VAR(wt_timeout_long), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(50000000), BLOCK_SIZE(1));

static Sys_var_uint Sys_extra_port(
       "extra_port",
       "Extra port number to use for tcp connections in a "
       "one-thread-per-connection manner. 0 means don't use another port",
       READ_ONLY GLOBAL_VAR(mysqld_extra_port), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX32), DEFAULT(0), BLOCK_SIZE(1));

static Sys_var_on_access_global<Sys_var_ulong,
                              PRIV_SET_SYSTEM_GLOBAL_VAR_EXTRA_MAX_CONNECTIONS>
Sys_extra_max_connections(
       "extra_max_connections", "The number of connections on extra-port",
       GLOBAL_VAR(extra_max_connections), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, 100000), DEFAULT(1), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(fix_max_connections));

#ifdef SAFE_MUTEX
static Sys_var_mybool Sys_mutex_deadlock_detector(
       "debug_mutex_deadlock_detector", "Enable checking of wrong mutex usage",
       READ_ONLY GLOBAL_VAR(safe_mutex_deadlock_detector),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));
#endif

static Sys_var_keycache Sys_key_cache_segments(
       "key_cache_segments", "The number of segments in a key cache",
       KEYCACHE_VAR(param_partitions),
       CMD_LINE(REQUIRED_ARG, OPT_KEY_CACHE_PARTITIONS),
       VALID_RANGE(0, MAX_KEY_CACHE_PARTITIONS),
       DEFAULT(DEFAULT_KEY_CACHE_PARTITIONS),
       BLOCK_SIZE(1), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(repartition_keycache));

static const char *log_slow_filter_names[]= 
{
  "admin", "filesort", "filesort_on_disk", "filesort_priority_queue",
  "full_join", "full_scan", "not_using_index", "query_cache",
  "query_cache_miss", "tmp_table", "tmp_table_on_disk", 0
};


static Sys_var_set Sys_log_slow_filter(
       "log_slow_filter",
       "Log only certain types of queries to the slow log. If variable empty alll kind of queries are logged.  All types are bound by slow_query_time, except 'not_using_index' which is always logged if enabled",
       SESSION_VAR(log_slow_filter), CMD_LINE(REQUIRED_ARG),
       log_slow_filter_names,
       /* by default we log all queries except 'not_using_index' */
       DEFAULT(my_set_bits(array_elements(log_slow_filter_names)-1) &
               ~QPLAN_NOT_USING_INDEX));

static const char *log_slow_disabled_statements_names[]=
{ "admin", "call", "slave", "sp", 0 };

static const char *log_disabled_statements_names[]=
{ "slave", "sp", 0 };

static Sys_var_set Sys_log_slow_disabled_statements(
       "log_slow_disabled_statements",
       "Don't log certain types of statements to slow log",
       SESSION_VAR(log_slow_disabled_statements), CMD_LINE(REQUIRED_ARG),
       log_slow_disabled_statements_names,
       DEFAULT(LOG_SLOW_DISABLE_SP));

static Sys_var_set Sys_log_disabled_statements(
       "log_disabled_statements",
       "Don't log certain types of statements to general log",
       SESSION_VAR(log_disabled_statements), CMD_LINE(REQUIRED_ARG),
       log_disabled_statements_names,
       DEFAULT(LOG_DISABLE_SP),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_has_super));

#define NOT_SUPPORTED_YET -2
#ifndef PCRE2_EXTENDED_MORE
#define PCRE2_EXTENDED_MORE NOT_SUPPORTED_YET
#endif

static const char *default_regex_flags_names[]= 
{
  "DOTALL",    // (?s)  . matches anything including NL
  "DUPNAMES",  // (?J)  Allow duplicate names for subpatterns
  "EXTENDED",  // (?x)  Ignore white space and # comments
  "EXTENDED_MORE",//(?xx)  Ignore white space and # comments inside cheracter
  "EXTRA",     // means nothing since PCRE2
  "MULTILINE", // (?m)  ^ and $ match newlines within data
  "UNGREEDY",  // (?U)  Invert greediness of quantifiers
  0
};
static const int default_regex_flags_to_pcre[]=
{
  PCRE2_DOTALL,
  PCRE2_DUPNAMES,
  PCRE2_EXTENDED,
  PCRE2_EXTENDED_MORE,
  -1, /* EXTRA flag not available since PCRE2 */
  PCRE2_MULTILINE,
  PCRE2_UNGREEDY,
  0
};
int default_regex_flags_pcre(THD *thd)
{
  ulonglong src= thd->variables.default_regex_flags;
  int i, res;
  for (i= res= 0; default_regex_flags_to_pcre[i]; i++)
  {
    if (src & (1ULL << i))
    {
      if (default_regex_flags_to_pcre[i] < 0)
      {
        const char *msg= default_regex_flags_to_pcre[i] == NOT_SUPPORTED_YET
          ? "Your version of PCRE2 does not support the %s flag. Ignored."
          : "PCRE2 doesn't support the %s flag. Ignored.";
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_UNKNOWN_ERROR, msg, default_regex_flags_names[i]);
        continue;
      }
      res|= default_regex_flags_to_pcre[i];
    }
  }
  return res;
}
static Sys_var_set Sys_default_regex_flags(
       "default_regex_flags",
       "Default flags for the regex library",
       SESSION_VAR(default_regex_flags), CMD_LINE(REQUIRED_ARG),
       default_regex_flags_names,
       DEFAULT(0));

static Sys_var_ulong Sys_log_slow_rate_limit(
       "log_slow_rate_limit",
       "Write to slow log every #th slow query. Set to 1 to log everything. "
       "Increase it to reduce the size of the slow or the performance impact "
       "of slow logging",
       SESSION_VAR(log_slow_rate_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1, UINT_MAX), DEFAULT(1), BLOCK_SIZE(1));

static const char *log_slow_verbosity_names[]= { "innodb", "query_plan", 
                                                 "explain", 0 };
static Sys_var_set Sys_log_slow_verbosity(
       "log_slow_verbosity",
       "Verbosity level for the slow log",
       SESSION_VAR(log_slow_verbosity), CMD_LINE(REQUIRED_ARG),
       log_slow_verbosity_names, DEFAULT(LOG_SLOW_VERBOSITY_INIT));

static Sys_var_ulong Sys_join_cache_level(
       "join_cache_level",
       "Controls what join operations can be executed with join buffers. Odd "
       "numbers are used for plain join buffers while even numbers are used "
       "for linked buffers",
       SESSION_VAR(join_cache_level), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 8), DEFAULT(2), BLOCK_SIZE(1));

static Sys_var_ulong Sys_mrr_buffer_size(
       "mrr_buffer_size",
       "Size of buffer to use when using MRR with range access",
       SESSION_VAR(mrr_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(IO_SIZE*2, INT_MAX32), DEFAULT(256*1024), BLOCK_SIZE(1));

static Sys_var_ulong Sys_rowid_merge_buff_size(
       "rowid_merge_buff_size",
       "The size of the buffers used [NOT] IN evaluation via partial matching",
       SESSION_VAR(rowid_merge_buff_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, LONG_MAX), DEFAULT(8*1024*1024),
       BLOCK_SIZE(1));

static Sys_var_mybool Sys_userstat(
       "userstat",
       "Enables statistics gathering for USER_STATISTICS, CLIENT_STATISTICS, "
       "INDEX_STATISTICS and TABLE_STATISTICS tables in the INFORMATION_SCHEMA",
       GLOBAL_VAR(opt_userstat_running),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_on_access<Sys_var_mybool,
                         PRIV_SET_SYSTEM_VAR_BINLOG_ANNOTATE_ROW_EVENTS,
                         PRIV_SET_SYSTEM_VAR_BINLOG_ANNOTATE_ROW_EVENTS>
Sys_binlog_annotate_row_events(
       "binlog_annotate_row_events",
       "Tells the master to annotate RBR events with the statement that "
       "caused these events",
       SESSION_VAR(binlog_annotate_row_events), CMD_LINE(OPT_ARG),
       DEFAULT(TRUE));

#ifdef HAVE_REPLICATION
static Sys_var_mybool Sys_replicate_annotate_row_events(
       "replicate_annotate_row_events",
       "Tells the slave to write annotate rows events received from the master "
       "to its own binary log. Ignored if log_slave_updates is not set",
       READ_ONLY GLOBAL_VAR(opt_replicate_annotate_row_events),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));
#endif

static Sys_var_ulonglong Sys_join_buffer_space_limit(
       "join_buffer_space_limit",
       "The limit of the space for all join buffers used by a query",
       SESSION_VAR(join_buff_space_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(2048, ULONGLONG_MAX), DEFAULT(16*128*1024),
       BLOCK_SIZE(2048));

static Sys_var_ulong Sys_progress_report_time(
       "progress_report_time",
       "Seconds between sending progress reports to the client for "
       "time-consuming statements. Set to 0 to disable progress reporting.",
       SESSION_VAR(progress_report_time), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(5), BLOCK_SIZE(1));

const char *use_stat_tables_modes[] =
           {"NEVER", "COMPLEMENTARY", "PREFERABLY",
           "COMPLEMENTARY_FOR_QUERIES", "PREFERABLY_FOR_QUERIES", 0};
static Sys_var_enum Sys_optimizer_use_stat_tables(
       "use_stat_tables",
       "Specifies how to use system statistics tables",
       SESSION_VAR(use_stat_tables), CMD_LINE(REQUIRED_ARG),
       use_stat_tables_modes, DEFAULT(4));

static Sys_var_ulong Sys_histogram_size(
       "histogram_size",
       "Number of bytes used for a histogram. "
       "If set to 0, no histograms are created by ANALYZE.",
       SESSION_VAR(histogram_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, 255), DEFAULT(254), BLOCK_SIZE(1));

extern const char *histogram_types[];
static Sys_var_enum Sys_histogram_type(
       "histogram_type",
       "Specifies type of the histograms created by ANALYZE. "
       "Possible values are: "
       "SINGLE_PREC_HB - single precision height-balanced, "
       "DOUBLE_PREC_HB - double precision height-balanced.",
       SESSION_VAR(histogram_type), CMD_LINE(REQUIRED_ARG),
       histogram_types, DEFAULT(1));

static Sys_var_mybool Sys_no_thread_alarm(
       "debug_no_thread_alarm",
       "Disable system thread alarm calls. Disabling it may be useful "
       "in debugging or testing, never do it in production",
       READ_ONLY GLOBAL_VAR(my_disable_thr_alarm), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static Sys_var_mybool Sys_query_cache_strip_comments(
       "query_cache_strip_comments",
       "Strip all comments from a query before storing it "
       "in the query cache",
       SESSION_VAR(query_cache_strip_comments), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static ulonglong in_transaction(THD *thd)
{
  return MY_TEST(thd->in_active_multi_stmt_transaction());
}
static Sys_var_session_special Sys_in_transaction(
       "in_transaction", "Whether there is an active transaction",
       READ_ONLY sys_var::ONLY_SESSION, NO_CMD_LINE,
       VALID_RANGE(0, 1), BLOCK_SIZE(1), NO_MUTEX_GUARD,
       NOT_IN_BINLOG, ON_CHECK(0), ON_UPDATE(0), ON_READ(in_transaction));

#ifndef DBUG_OFF
static Sys_var_ulong Sys_debug_binlog_fsync_sleep(
       "debug_binlog_fsync_sleep",
       "Extra sleep (in microseconds) to add to binlog fsync(), for debugging",
       GLOBAL_VAR(opt_binlog_dbug_fsync_sleep),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(0), BLOCK_SIZE(1));
#endif

static Sys_var_harows Sys_expensive_subquery_limit(
       "expensive_subquery_limit",
       "The maximum number of rows a subquery may examine in order to be "
       "executed during optimization and used for constant optimization",
       SESSION_VAR(expensive_subquery_limit), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, HA_POS_ERROR), DEFAULT(100), BLOCK_SIZE(1));

static Sys_var_mybool Sys_encrypt_tmp_disk_tables(
       "encrypt_tmp_disk_tables",
       "Encrypt temporary on-disk tables (created as part of query execution)",
       GLOBAL_VAR(encrypt_tmp_disk_tables),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_encrypt_tmp_files(
       "encrypt_tmp_files",
       "Encrypt temporary files (created for filesort, binary log cache, etc)",
       READ_ONLY GLOBAL_VAR(encrypt_tmp_files),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE));

static Sys_var_mybool Sys_binlog_encryption(
       "encrypt_binlog", "Encrypt binary logs (including relay logs)",
       READ_ONLY GLOBAL_VAR(encrypt_binlog), CMD_LINE(OPT_ARG),
       DEFAULT(FALSE));

static const char *binlog_row_image_names[]= {"MINIMAL", "NOBLOB", "FULL", NullS};
static Sys_var_on_access<Sys_var_enum,
                         PRIV_SET_SYSTEM_VAR_BINLOG_ROW_IMAGE,
                         PRIV_SET_SYSTEM_VAR_BINLOG_ROW_IMAGE>
Sys_binlog_row_image(
       "binlog_row_image",
       "Controls whether rows should be logged in 'FULL', 'NOBLOB' or "
       "'MINIMAL' formats. 'FULL', means that all columns in the before "
       "and after image are logged. 'NOBLOB', means that mysqld avoids logging "
       "blob columns whenever possible (eg, blob column was not changed or "
       "is not part of primary key). 'MINIMAL', means that a PK equivalent (PK "
       "columns or full row if there is no PK in the table) is logged in the "
       "before image, and only changed columns are logged in the after image. "
       "(Default: FULL).",
       SESSION_VAR(binlog_row_image), CMD_LINE(REQUIRED_ARG),
       binlog_row_image_names, DEFAULT(BINLOG_ROW_IMAGE_FULL));

static const char *binlog_row_metadata_names[]= {"NO_LOG", "MINIMAL", "FULL", NullS};
static Sys_var_on_access_global<Sys_var_enum,
                                PRIV_SET_SYSTEM_GLOBAL_VAR_BINLOG_ROW_METADATA>
Sys_binlog_row_metadata(
       "binlog_row_metadata",
       "Controls whether metadata is logged using FULL , MINIMAL format and NO_LOG."
       "FULL causes all metadata to be logged; MINIMAL means that only "
       "metadata actually required by slave is logged; NO_LOG NO metadata will be logged."
       "Default: NO_LOG.",
       GLOBAL_VAR(binlog_row_metadata), CMD_LINE(REQUIRED_ARG),
       binlog_row_metadata_names, DEFAULT(Table_map_log_event::BINLOG_ROW_METADATA_NO_LOG),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(NULL),
       ON_UPDATE(NULL));


static bool check_pseudo_slave_mode(sys_var *self, THD *thd, set_var *var)
{
  longlong previous_val= thd->variables.pseudo_slave_mode;
  longlong val= (longlong) var->save_result.ulonglong_value;
  bool rli_fake= false;

#ifndef EMBEDDED_LIBRARY
  rli_fake= thd->rli_fake ? true : false;
#endif

  if (rli_fake)
  {
    if (!val)
    {
#ifndef EMBEDDED_LIBRARY
      delete thd->rli_fake;
      thd->rli_fake= NULL;
      delete thd->rgi_fake;
      thd->rgi_fake= NULL;
#endif
    }
    else if (previous_val && val)
      goto ineffective;
    else if (!previous_val && val)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "'pseudo_slave_mode' is already ON.");
  }
  else
  {
    if (!previous_val && !val)
      goto ineffective;
    else if (previous_val && !val)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "Slave applier execution mode not active, "
                   "statement ineffective.");
  }
  goto end;

ineffective:
  push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
               ER_WRONG_VALUE_FOR_VAR,
               "'pseudo_slave_mode' change was ineffective.");

end:
  return FALSE;
}
static Sys_var_mybool Sys_pseudo_slave_mode(
       "pseudo_slave_mode",
       "SET pseudo_slave_mode= 0,1 are commands that mysqlbinlog "
       "adds to beginning and end of binary log dumps. While zero "
       "value indeed disables, the actual enabling of the slave "
       "applier execution mode is done implicitly when a "
       "Format_description_event is sent through the session.",
       SESSION_ONLY(pseudo_slave_mode), NO_CMD_LINE, DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(check_pseudo_slave_mode));

static Sys_var_mybool Sys_mysql56_temporal_format(
       "mysql56_temporal_format",
       "Use MySQL-5.6 (instead of MariaDB-5.3) format for TIME, DATETIME, TIMESTAMP columns.",
       GLOBAL_VAR(opt_mysql56_temporal_format),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));

static Sys_var_mybool Sys_strict_password_validation(
       "strict_password_validation",
       "When password validation plugins are enabled, reject passwords "
       "that cannot be validated (passwords specified as a hash)",
       GLOBAL_VAR(strict_password_validation),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE));

#ifdef HAVE_MMAP
static Sys_var_ulong Sys_log_tc_size(
       "log_tc_size",
       "Size of transaction coordinator log.",
       READ_ONLY GLOBAL_VAR(opt_tc_log_size),
       CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(my_getpagesize() * 3, ULONG_MAX),
       DEFAULT(my_getpagesize() * 6), BLOCK_SIZE(my_getpagesize()));
#endif

static Sys_var_ulonglong Sys_max_thread_mem(
       "max_session_mem_used", "Amount of memory a single user session "
       "is allowed to allocate. This limits the value of the "
       "session variable MEM_USED", SESSION_VAR(max_mem_used),
       CMD_LINE(REQUIRED_ARG), VALID_RANGE(8192,  ULONGLONG_MAX),
       DEFAULT(LONGLONG_MAX), BLOCK_SIZE(1));

#ifndef EMBEDDED_LIBRARY

static Sys_var_sesvartrack Sys_track_session_sys_vars(
       "session_track_system_variables",
       "Track changes in registered system variables. ",
       CMD_LINE(REQUIRED_ARG),
       DEFAULT("autocommit,character_set_client,character_set_connection,"
       "character_set_results,time_zone"));

static bool update_session_track_schema(sys_var *self, THD *thd,
                                        enum_var_type type)
{
  DBUG_ENTER("update_session_track_schema");
  DBUG_RETURN(thd->session_tracker.current_schema.update(thd, NULL));
}

static Sys_var_mybool Sys_session_track_schema(
       "session_track_schema",
       "Track changes to the default schema.",
       SESSION_VAR(session_track_schema),
       CMD_LINE(OPT_ARG), DEFAULT(TRUE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0),
       ON_UPDATE(update_session_track_schema));


static bool update_session_track_tx_info(sys_var *self, THD *thd,
                                         enum_var_type type)
{
  DBUG_ENTER("update_session_track_tx_info");
  DBUG_RETURN(thd->session_tracker.transaction_info.update(thd, NULL));
}

static const char *session_track_transaction_info_names[]=
  { "OFF", "STATE", "CHARACTERISTICS", NullS };

static Sys_var_enum Sys_session_track_transaction_info(
       "session_track_transaction_info",
       "Track changes to the transaction attributes. OFF to disable; "
       "STATE to track just transaction state (Is there an active transaction? "
       "Does it have any data? etc.); CHARACTERISTICS to track transaction "
       "state and report all statements needed to start a transaction with "
       "the same characteristics (isolation level, read only/read write,"
       "snapshot - but not any work done / data modified within the "
       "transaction).",
       SESSION_VAR(session_track_transaction_info),
       CMD_LINE(REQUIRED_ARG), session_track_transaction_info_names,
       DEFAULT(0), NO_MUTEX_GUARD, NOT_IN_BINLOG, ON_CHECK(0),
       ON_UPDATE(update_session_track_tx_info));


static bool update_session_track_state_change(sys_var *self, THD *thd,
                                              enum_var_type type)
{
  DBUG_ENTER("update_session_track_state_change");
  DBUG_RETURN(thd->session_tracker.state_change.update(thd, NULL));
}

static Sys_var_mybool Sys_session_track_state_change(
       "session_track_state_change",
       "Track changes to the session state.",
       SESSION_VAR(session_track_state_change),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0),
       ON_UPDATE(update_session_track_state_change));


static bool update_session_track_user_variables(sys_var *self, THD *thd,
                                                enum_var_type type)
{
  return thd->session_tracker.user_variables.update(thd, 0);
}

static Sys_var_mybool Sys_session_track_user_variables(
       "session_track_user_variables",
       "Track changes to user variables.",
       SESSION_VAR(session_track_user_variables),
       CMD_LINE(OPT_ARG), DEFAULT(FALSE),
       NO_MUTEX_GUARD, NOT_IN_BINLOG,
       ON_CHECK(0),
       ON_UPDATE(update_session_track_user_variables));

#endif //EMBEDDED_LIBRARY

static Sys_var_uint Sys_in_subquery_conversion_threshold(
       "in_predicate_conversion_threshold",
       "The minimum number of scalar elements in the value list of "
       "IN predicate that triggers its conversion to IN subquery. Set to "
       "0 to disable the conversion.",
       SESSION_VAR(in_subquery_conversion_threshold), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(0, UINT_MAX), DEFAULT(IN_SUBQUERY_CONVERSION_THRESHOLD), BLOCK_SIZE(1));

static Sys_var_enum Sys_secure_timestamp(
       "secure_timestamp", "Restricts direct setting of a session "
       "timestamp. Possible levels are: YES - timestamp cannot deviate from "
       "the system clock, REPLICATION - replication thread can adjust "
       "timestamp to match the master's, SUPER - a user with this "
       "privilege and a replication thread can adjust timestamp, NO - "
       "historical behavior, anyone can modify session timestamp",
       READ_ONLY GLOBAL_VAR(opt_secure_timestamp), CMD_LINE(REQUIRED_ARG),
       secure_timestamp_levels, DEFAULT(SECTIME_NO));

static Sys_var_ulonglong Sys_max_rowid_filter_size(
       "max_rowid_filter_size",
       "The maximum size of the container of a rowid filter",
       SESSION_VAR(max_rowid_filter_size), CMD_LINE(REQUIRED_ARG),
       VALID_RANGE(1024, (ulonglong)~(intptr)0), DEFAULT(128*1024),
       BLOCK_SIZE(1));
