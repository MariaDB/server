/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/**
  @file

  @brief
  logging of commands

  @todo
    Abort logging when we get an error in reading or writing log files
*/

#include <my_global.h>		/* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "log.h"
#include "sql_base.h"                           // open_log_table
#include "sql_repl.h"
#include "sql_delete.h"                         // mysql_truncate
#include "sql_parse.h"                          // command_name
#include "sql_time.h"           // calc_time_from_sec, my_time_compare
#include "tztime.h"             // my_tz_OFFSET0, struct Time_zone
#include "sql_acl.h"            // SUPER_ACL
#include "log_event.h"          // Query_log_event
#include "rpl_filter.h"
#include "rpl_rli.h"
#include "sql_audit.h"
#include "log_slow.h"
#include "mysqld.h"

#include <my_dir.h>
#include <stdarg.h>
#include <m_ctype.h>				// For test_if_number

#ifdef _WIN32
#include "message.h"
#endif

#include "sql_plugin.h"
#include "rpl_handler.h"
#include "debug_sync.h"
#include "sql_show.h"
#include "my_pthread.h"
#include "wsrep_mysqld.h"

/* max size of the log message */
#define MAX_LOG_BUFFER_SIZE 1024
#define MAX_TIME_SIZE 32
#define MY_OFF_T_UNDEF (~(my_off_t)0UL)

#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

handlerton *binlog_hton;
LOGGER logger;

const char *log_bin_index= 0;
const char *log_bin_basename= 0;

MYSQL_BIN_LOG mysql_bin_log(&sync_binlog_period);

static bool test_if_number(const char *str,
			   ulong *res, bool allow_wildcards);
static int binlog_init(void *p);
static int binlog_close_connection(handlerton *hton, THD *thd);
static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv);
static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv);
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *hton,
                                                      THD *thd);
static int binlog_commit(handlerton *hton, THD *thd, bool all);
static int binlog_rollback(handlerton *hton, THD *thd, bool all);
static int binlog_prepare(handlerton *hton, THD *thd, bool all);
static int binlog_start_consistent_snapshot(handlerton *hton, THD *thd);

static LEX_STRING const write_error_msg=
    { C_STRING_WITH_LEN("error writing to the binary log") };

static my_bool opt_optimize_thread_scheduling= TRUE;
ulong binlog_checksum_options;
#ifndef DBUG_OFF
ulong opt_binlog_dbug_fsync_sleep= 0;
#endif

mysql_mutex_t LOCK_prepare_ordered;
mysql_cond_t COND_prepare_ordered;
mysql_mutex_t LOCK_after_binlog_sync;
mysql_mutex_t LOCK_commit_ordered;

static ulonglong binlog_status_var_num_commits;
static ulonglong binlog_status_var_num_group_commits;
static ulonglong binlog_status_group_commit_trigger_count;
static ulonglong binlog_status_group_commit_trigger_lock_wait;
static ulonglong binlog_status_group_commit_trigger_timeout;
static char binlog_snapshot_file[FN_REFLEN];
static ulonglong binlog_snapshot_position;

static SHOW_VAR binlog_status_vars_detail[]=
{
  {"commits",
    (char *)&binlog_status_var_num_commits, SHOW_LONGLONG},
  {"group_commits",
    (char *)&binlog_status_var_num_group_commits, SHOW_LONGLONG},
  {"group_commit_trigger_count",
    (char *)&binlog_status_group_commit_trigger_count, SHOW_LONGLONG},
  {"group_commit_trigger_lock_wait",
    (char *)&binlog_status_group_commit_trigger_lock_wait, SHOW_LONGLONG},
  {"group_commit_trigger_timeout",
    (char *)&binlog_status_group_commit_trigger_timeout, SHOW_LONGLONG},
  {"snapshot_file",
    (char *)&binlog_snapshot_file, SHOW_CHAR},
  {"snapshot_position",
   (char *)&binlog_snapshot_position, SHOW_LONGLONG},
  {NullS, NullS, SHOW_LONG}
};

/*
  Variables for the binlog background thread.
  Protected by the MYSQL_BIN_LOG::LOCK_binlog_background_thread mutex.
 */
static bool binlog_background_thread_started= false;
static bool binlog_background_thread_stop= false;
static MYSQL_BIN_LOG::xid_count_per_binlog *
    binlog_background_thread_queue= NULL;

static bool start_binlog_background_thread();

static rpl_binlog_state rpl_global_gtid_binlog_state;

/**
   purge logs, master and slave sides both, related error code
   convertor.
   Called from @c purge_error_message(), @c MYSQL_BIN_LOG::reset_logs()

   @param  res  an internal to purging routines error code 

   @return the user level error code ER_*
*/
uint purge_log_get_error_code(int res)
{
  uint errcode= 0;

  switch (res)  {
  case 0: break;
  case LOG_INFO_EOF:	errcode= ER_UNKNOWN_TARGET_BINLOG; break;
  case LOG_INFO_IO:	errcode= ER_IO_ERR_LOG_INDEX_READ; break;
  case LOG_INFO_INVALID:errcode= ER_BINLOG_PURGE_PROHIBITED; break;
  case LOG_INFO_SEEK:	errcode= ER_FSEEK_FAIL; break;
  case LOG_INFO_MEM:	errcode= ER_OUT_OF_RESOURCES; break;
  case LOG_INFO_FATAL:	errcode= ER_BINLOG_PURGE_FATAL_ERR; break;
  case LOG_INFO_IN_USE: errcode= ER_LOG_IN_USE; break;
  case LOG_INFO_EMFILE: errcode= ER_BINLOG_PURGE_EMFILE; break;
  default:		errcode= ER_LOG_PURGE_UNKNOWN_ERR; break;
  }

  return errcode;
}

/**
  Silence all errors and warnings reported when performing a write
  to a log table.
  Errors and warnings are not reported to the client or SQL exception
  handlers, so that the presence of logging does not interfere and affect
  the logic of an application.
*/
class Silence_log_table_errors : public Internal_error_handler
{
  char m_message[MYSQL_ERRMSG_SIZE];
public:
  Silence_log_table_errors()
  {
    m_message[0]= '\0';
  }

  virtual ~Silence_log_table_errors() {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sql_state,
                                Sql_condition::enum_warning_level level,
                                const char* msg,
                                Sql_condition ** cond_hdl);
  const char *message() const { return m_message; }
};

bool
Silence_log_table_errors::handle_condition(THD *,
                                           uint,
                                           const char*,
                                           Sql_condition::enum_warning_level,
                                           const char* msg,
                                           Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  strmake_buf(m_message, msg);
  return TRUE;
}

sql_print_message_func sql_print_message_handlers[3] =
{
  sql_print_information,
  sql_print_warning,
  sql_print_error
};


/**
  Create the name of the log file
  
  @param[OUT] out    a pointer to a new allocated name will go there
  @param[IN] log_ext The extension for the file (e.g .log)
  @param[IN] once    whether to use malloc_once or a normal malloc.
*/
void make_default_log_name(char **out, const char* log_ext, bool once)
{
  char buff[FN_REFLEN+10];
  fn_format(buff, opt_log_basename, "", log_ext, MYF(MY_REPLACE_EXT));
  if (once)
    *out= my_once_strdup(buff, MYF(MY_WME));
  else
  {
    my_free(*out);
    *out= my_strdup(buff, MYF(MY_WME));
  }
}


/*
  Helper classes to store non-transactional and transactional data
  before copying it to the binary log.
*/
class binlog_cache_data
{
public:
  binlog_cache_data(): m_pending(0), before_stmt_pos(MY_OFF_T_UNDEF),
  incident(FALSE), changes_to_non_trans_temp_table_flag(FALSE),
  saved_max_binlog_cache_size(0), ptr_binlog_cache_use(0),
  ptr_binlog_cache_disk_use(0)
  { }
  
  ~binlog_cache_data()
  {
    DBUG_ASSERT(empty());
    close_cached_file(&cache_log);
  }

  bool empty() const
  {
    return pending() == NULL && my_b_tell(&cache_log) == 0;
  }

  Rows_log_event *pending() const
  {
    return m_pending;
  }

  void set_pending(Rows_log_event *const pending_arg)
  {
    m_pending= pending_arg;
  }

  void set_incident(void)
  {
    incident= TRUE;
  }
  
  bool has_incident(void)
  {
    return(incident);
  }

  void set_changes_to_non_trans_temp_table()
  {
    changes_to_non_trans_temp_table_flag= TRUE;    
  }

  bool changes_to_non_trans_temp_table()
  {
    return (changes_to_non_trans_temp_table_flag);    
  }

  void reset()
  {
    compute_statistics();
    truncate(0);
    changes_to_non_trans_temp_table_flag= FALSE;
    incident= FALSE;
    before_stmt_pos= MY_OFF_T_UNDEF;
    /*
      The truncate function calls reinit_io_cache that calls
      my_b_flush_io_cache which may increase disk_writes. This breaks
      the disk_writes use by the binary log which aims to compute the
      ratio between in-memory cache usage and disk cache usage. To
      avoid this undesirable behavior, we reset the variable after
      truncating the cache.
    */
    cache_log.disk_writes= 0;
    DBUG_ASSERT(empty());
  }

  my_off_t get_byte_position() const
  {
    return my_b_tell(&cache_log);
  }

  my_off_t get_prev_position()
  {
     return(before_stmt_pos);
  }

  void set_prev_position(my_off_t pos)
  {
     before_stmt_pos= pos;
  }
  
  void restore_prev_position()
  {
    truncate(before_stmt_pos);
  }

  void restore_savepoint(my_off_t pos)
  {
    truncate(pos);
    if (pos < before_stmt_pos)
      before_stmt_pos= MY_OFF_T_UNDEF;
  }

  void set_binlog_cache_info(my_off_t param_max_binlog_cache_size,
                             ulong *param_ptr_binlog_cache_use,
                             ulong *param_ptr_binlog_cache_disk_use)
  {
    /*
      The assertions guarantee that the set_binlog_cache_info is
      called just once and information passed as parameters are
      never zero.

      This is done while calling the constructor binlog_cache_mngr.
      We cannot set informaton in the constructor binlog_cache_data
      because the space for binlog_cache_mngr is allocated through
      a placement new.

      In the future, we can refactor this and change it to avoid
      the set_binlog_info. 
    */
    DBUG_ASSERT(saved_max_binlog_cache_size == 0 &&
                param_max_binlog_cache_size != 0 &&
                ptr_binlog_cache_use == 0 &&
                param_ptr_binlog_cache_use != 0 &&
                ptr_binlog_cache_disk_use == 0 &&
                param_ptr_binlog_cache_disk_use != 0);

    saved_max_binlog_cache_size= param_max_binlog_cache_size;
    ptr_binlog_cache_use= param_ptr_binlog_cache_use;
    ptr_binlog_cache_disk_use= param_ptr_binlog_cache_disk_use;
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }

  /*
    Cache to store data before copying it to the binary log.
  */
  IO_CACHE cache_log;

private:
  /*
    Pending binrows event. This event is the event where the rows are currently
    written.
   */
  Rows_log_event *m_pending;

  /*
    Binlog position before the start of the current statement.
  */
  my_off_t before_stmt_pos;
 
  /*
    This indicates that some events did not get into the cache and most likely
    it is corrupted.
  */ 
  bool incident;

  /*
    This flag indicates if the cache has changes to temporary tables.
    @TODO This a temporary fix and should be removed after BUG#54562.
  */
  bool changes_to_non_trans_temp_table_flag;

  /**
    This function computes binlog cache and disk usage.
  */
  void compute_statistics()
  {
    if (!empty())
    {
      statistic_increment(*ptr_binlog_cache_use, &LOCK_status);
      if (cache_log.disk_writes != 0)
        statistic_increment(*ptr_binlog_cache_disk_use, &LOCK_status);
    }
  }

  /*
    Stores the values of maximum size of the cache allowed when this cache
    is configured. This corresponds to either
      . max_binlog_cache_size or max_binlog_stmt_cache_size.
  */
  my_off_t saved_max_binlog_cache_size;

  /*
    Stores a pointer to the status variable that keeps track of the in-memory 
    cache usage. This corresponds to either
      . binlog_cache_use or binlog_stmt_cache_use.
  */
  ulong *ptr_binlog_cache_use;

  /*
    Stores a pointer to the status variable that keeps track of the disk
    cache usage. This corresponds to either
      . binlog_cache_disk_use or binlog_stmt_cache_disk_use.
  */
  ulong *ptr_binlog_cache_disk_use;

  /*
    It truncates the cache to a certain position. This includes deleting the
    pending event.
   */
  void truncate(my_off_t pos)
  {
    DBUG_PRINT("info", ("truncating to position %lu", (ulong) pos));
    if (pending())
    {
      delete pending();
      set_pending(0);
    }
    reinit_io_cache(&cache_log, WRITE_CACHE, pos, 0, 0);
    cache_log.end_of_file= saved_max_binlog_cache_size;
  }
 
  binlog_cache_data& operator=(const binlog_cache_data& info);
  binlog_cache_data(const binlog_cache_data& info);
};

class binlog_cache_mngr {
public:
  binlog_cache_mngr(my_off_t param_max_binlog_stmt_cache_size,
                    my_off_t param_max_binlog_cache_size,
                    ulong *param_ptr_binlog_stmt_cache_use,
                    ulong *param_ptr_binlog_stmt_cache_disk_use,
                    ulong *param_ptr_binlog_cache_use,
                    ulong *param_ptr_binlog_cache_disk_use)
    : last_commit_pos_offset(0), using_xa(FALSE), xa_xid(0)
  {
     stmt_cache.set_binlog_cache_info(param_max_binlog_stmt_cache_size,
                                      param_ptr_binlog_stmt_cache_use,
                                      param_ptr_binlog_stmt_cache_disk_use);
     trx_cache.set_binlog_cache_info(param_max_binlog_cache_size,
                                     param_ptr_binlog_cache_use,
                                     param_ptr_binlog_cache_disk_use);
     last_commit_pos_file[0]= 0;
  }

  void reset(bool do_stmt, bool do_trx)
  {
    if (do_stmt)
      stmt_cache.reset();
    if (do_trx)
    {
      trx_cache.reset();
      using_xa= FALSE;
      last_commit_pos_file[0]= 0;
      last_commit_pos_offset= 0;
    }
  }

  binlog_cache_data* get_binlog_cache_data(bool is_transactional)
  {
    return (is_transactional ? &trx_cache : &stmt_cache);
  }

  IO_CACHE* get_binlog_cache_log(bool is_transactional)
  {
    return (is_transactional ? &trx_cache.cache_log : &stmt_cache.cache_log);
  }

  binlog_cache_data stmt_cache;

  binlog_cache_data trx_cache;

  /*
    Binlog position for current transaction.
    For START TRANSACTION WITH CONSISTENT SNAPSHOT, this is the binlog
    position corresponding to the snapshot taken. During (and after) commit,
    this is set to the binlog position corresponding to just after the
    commit (so storage engines can store it in their transaction log).
  */
  char last_commit_pos_file[FN_REFLEN];
  my_off_t last_commit_pos_offset;

  /*
    Flag set true if this transaction is committed with log_xid() as part of
    XA, false if not.
  */
  bool using_xa;
  my_xid xa_xid;
  bool need_unlog;
  /*
    Id of binlog that transaction was written to; only needed if need_unlog is
    true.
  */
  ulong binlog_id;
  /* Set if we get an error during commit that must be returned from unlog(). */
  bool delayed_error;

private:

  binlog_cache_mngr& operator=(const binlog_cache_mngr& info);
  binlog_cache_mngr(const binlog_cache_mngr& info);
};

bool LOGGER::is_log_table_enabled(uint log_table_type)
{
  switch (log_table_type) {
  case QUERY_LOG_SLOW:
    return (table_log_handler != NULL) && global_system_variables.sql_log_slow;
  case QUERY_LOG_GENERAL:
    return (table_log_handler != NULL) && opt_log ;
  default:
    DBUG_ASSERT(0);
    return FALSE;                             /* make compiler happy */
  }
}

/**
   Check if a given table is opened log table

   @param table             Table to check
   @param check_if_opened   Only fail if it's a log table in use
   @param error_msg	    String to put in error message if not ok.
                            No error message if 0
   @return 0 ok
   @return # Type of log file
 */

int check_if_log_table(const TABLE_LIST *table,
                       bool check_if_opened,
                       const char *error_msg)
{
  int result= 0;
  if (table->db_length == 5 &&
      !my_strcasecmp(table_alias_charset, table->db, "mysql"))
  {
    const char *table_name= table->table_name;

    if (table->table_name_length == 11 &&
        !my_strcasecmp(table_alias_charset, table_name, "general_log"))
    {
      result= QUERY_LOG_GENERAL;
      goto end;
    }

    if (table->table_name_length == 8 &&
        !my_strcasecmp(table_alias_charset, table_name, "slow_log"))
    {
      result= QUERY_LOG_SLOW;
      goto end;
    }
  }
  return 0;

end:
  if (!check_if_opened || logger.is_log_table_enabled(result))
  {
    if (error_msg)
      my_error(ER_BAD_LOG_STATEMENT, MYF(0), error_msg);
    return result;
  }
  return 0;
}


Log_to_csv_event_handler::Log_to_csv_event_handler()
{
}


Log_to_csv_event_handler::~Log_to_csv_event_handler()
{
}


void Log_to_csv_event_handler::cleanup()
{
  logger.is_log_tables_initialized= FALSE;
}

/* log event handlers */

/**
  Log command to the general log table

  Log given command to the general log table.

  @param  event_time        command start timestamp
  @param  user_host         the pointer to the string with user@host info
  @param  user_host_len     length of the user_host string. this is computed
                            once and passed to all general log event handlers
  @param  thread_id         Id of the thread, issued a query
  @param  command_type      the type of the command being logged
  @param  command_type_len  the length of the string above
  @param  sql_text          the very text of the query being executed
  @param  sql_text_len      the length of sql_text string


  @return This function attempts to never call my_error(). This is
  necessary, because general logging happens already after a statement
  status has been sent to the client, so the client can not see the
  error anyway. Besides, the error is not related to the statement
  being executed and is internal, and thus should be handled
  internally (@todo: how?).
  If a write to the table has failed, the function attempts to
  write to a short error message to the file. The failure is also
  indicated in the return value. 

  @retval  FALSE   OK
  @retval  TRUE    error occurred
*/

bool Log_to_csv_event_handler::
  log_general(THD *thd, my_hrtime_t event_time, const char *user_host,
              uint user_host_len, int thread_id_arg,
              const char *command_type, uint command_type_len,
              const char *sql_text, uint sql_text_len,
              CHARSET_INFO *client_cs)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= TRUE;
  bool need_close= FALSE;
  bool need_pop= FALSE;
  bool need_rnd_end= FALSE;
  uint field_index;
  Silence_log_table_errors error_handler;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  bool save_time_zone_used;
  DBUG_ENTER("log_general");

  /*
    CSV uses TIME_to_timestamp() internally if table needs to be repaired
    which will set thd->time_zone_used
  */
  save_time_zone_used= thd->time_zone_used;

  save_thd_options= thd->variables.option_bits;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;

  table_list.init_one_table(MYSQL_SCHEMA_NAME.str, MYSQL_SCHEMA_NAME.length,
                            GENERAL_LOG_NAME.str, GENERAL_LOG_NAME.length,
                            GENERAL_LOG_NAME.str,
                            TL_WRITE_CONCURRENT_INSERT);

  /*
    1) open_log_table generates an error of the
    table can not be opened or is corrupted.
    2) "INSERT INTO general_log" can generate warning sometimes.

    Suppress these warnings and errors, they can't be dealt with
    properly anyway.

    QQ: this problem needs to be studied in more detail.
    Comment this 2 lines and run "cast.test" to see what's happening.
  */
  thd->push_internal_handler(& error_handler);
  need_pop= TRUE;

  if (!(table= open_log_table(thd, &table_list, &open_tables_backup)))
    goto err;

  need_close= TRUE;

  if (table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE) ||
      table->file->ha_rnd_init_with_error(0))
    goto err;

  need_rnd_end= TRUE;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  /*
    NOTE: we do not call restore_record() here, as all fields are
    filled by the Logger (=> no need to load default ones).
  */

  /*
    We do not set a value for table->field[0], as it will use
    default value (which is CURRENT_TIMESTAMP).
  */

  /* check that all columns exist */
  if (table->s->fields < 6)
    goto err;

  DBUG_ASSERT(table->field[0]->type() == MYSQL_TYPE_TIMESTAMP);

  ((Field_timestamp*) table->field[0])->store_TIME(
                  hrtime_to_my_time(event_time), hrtime_sec_part(event_time));

  /* do a write */
  if (table->field[1]->store(user_host, user_host_len, client_cs) ||
      table->field[2]->store((longlong) thread_id_arg, TRUE) ||
      table->field[3]->store((longlong) global_system_variables.server_id,
                             TRUE) ||
      table->field[4]->store(command_type, command_type_len, client_cs))
    goto err;

  /*
    A positive return value in store() means truncation.
    Still logging a message in the log in this case.
  */
  table->field[5]->flags|= FIELDFLAG_HEX_ESCAPE;
  if (table->field[5]->store(sql_text, sql_text_len, client_cs) < 0)
    goto err;

  /* mark all fields as not null */
  table->field[1]->set_notnull();
  table->field[2]->set_notnull();
  table->field[3]->set_notnull();
  table->field[4]->set_notnull();
  table->field[5]->set_notnull();

  /* Set any extra columns to their default values */
  for (field_index= 6 ; field_index < table->s->fields ; field_index++)
  {
    table->field[field_index]->set_default();
  }

  /* log table entries are not replicated */
  if (table->file->ha_write_row(table->record[0]))
    goto err;

  result= FALSE;

err:
  if (result && !thd->killed)
    sql_print_error("Failed to write to mysql.general_log: %s",
                    error_handler.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }
  if (need_pop)
    thd->pop_internal_handler();
  if (need_close)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  thd->time_zone_used= save_time_zone_used;
  DBUG_RETURN(result);
}


/*
  Log a query to the slow log table

  SYNOPSIS
    log_slow()
    thd               THD of the query
    current_time      current timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log event handlers
    query_time        Amount of time the query took to execute (in microseconds)
    lock_time         Amount of time the query was locked (in microseconds)
    is_command        The flag, which determines, whether the sql_text is a
                      query or an administrator command (these are treated
                      differently by the old logging routines)
    sql_text          the very text of the query or administrator command
                      processed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log a query to the slow log table

  RETURN
    FALSE - OK
    TRUE - error occurred
*/

bool Log_to_csv_event_handler::
  log_slow(THD *thd, my_hrtime_t current_time,
           const char *user_host, uint user_host_len,
           ulonglong query_utime, ulonglong lock_utime, bool is_command,
           const char *sql_text, uint sql_text_len)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= TRUE;
  bool need_close= FALSE;
  bool need_rnd_end= FALSE;
  Silence_log_table_errors error_handler;
  Open_tables_backup open_tables_backup;
  CHARSET_INFO *client_cs= thd->variables.character_set_client;
  bool save_time_zone_used;
  long query_time= (long) MY_MIN(query_utime/1000000, TIME_MAX_VALUE_SECONDS);
  long lock_time=  (long) MY_MIN(lock_utime/1000000, TIME_MAX_VALUE_SECONDS);
  long query_time_micro= (long) (query_utime % 1000000);
  long lock_time_micro=  (long) (lock_utime % 1000000);

  DBUG_ENTER("Log_to_csv_event_handler::log_slow");

  thd->push_internal_handler(& error_handler);
  /*
    CSV uses TIME_to_timestamp() internally if table needs to be repaired
    which will set thd->time_zone_used
  */
  save_time_zone_used= thd->time_zone_used;

  table_list.init_one_table(MYSQL_SCHEMA_NAME.str, MYSQL_SCHEMA_NAME.length,
                            SLOW_LOG_NAME.str, SLOW_LOG_NAME.length,
                            SLOW_LOG_NAME.str,
                            TL_WRITE_CONCURRENT_INSERT);

  if (!(table= open_log_table(thd, &table_list, &open_tables_backup)))
    goto err;

  need_close= TRUE;

  if (table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE) ||
      table->file->ha_rnd_init_with_error(0))
    goto err;

  need_rnd_end= TRUE;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  restore_record(table, s->default_values);    // Get empty record

  /* check that all columns exist */
  if (table->s->fields < 13)
    goto err;

  /* store the time and user values */
  DBUG_ASSERT(table->field[0]->type() == MYSQL_TYPE_TIMESTAMP);
  ((Field_timestamp*) table->field[0])->store_TIME(
             hrtime_to_my_time(current_time), hrtime_sec_part(current_time));
  if (table->field[1]->store(user_host, user_host_len, client_cs))
    goto err;

  /*
    A TIME field can not hold the full longlong range; query_time or
    lock_time may be truncated without warning here, if greater than
    839 hours (~35 days)
  */
  MYSQL_TIME t;
  t.neg= 0;

  /* fill in query_time field */
  calc_time_from_sec(&t, query_time, query_time_micro);
  if (table->field[2]->store_time(&t))
    goto err;
  /* lock_time */
  calc_time_from_sec(&t, lock_time, lock_time_micro);
  if (table->field[3]->store_time(&t))
    goto err;
  /* rows_sent */
  if (table->field[4]->store((longlong) thd->get_sent_row_count(), TRUE))
    goto err;
  /* rows_examined */
  if (table->field[5]->store((longlong) thd->get_examined_row_count(), TRUE))
    goto err;

  /* fill database field */
  if (thd->db)
  {
    if (table->field[6]->store(thd->db, thd->db_length, client_cs))
      goto err;
    table->field[6]->set_notnull();
  }

  if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
  {
    if (table->
        field[7]->store((longlong)
                        thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                        TRUE))
      goto err;
    table->field[7]->set_notnull();
  }

  /*
    Set value if we do an insert on autoincrement column. Note that for
    some engines (those for which get_auto_increment() does not leave a
    table lock until the statement ends), this is just the first value and
    the next ones used may not be contiguous to it.
  */
  if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
  {
    if (table->
        field[8]->store((longlong)
          thd->auto_inc_intervals_in_cur_stmt_for_binlog.minimum(), TRUE))
      goto err;
    table->field[8]->set_notnull();
  }

  if (table->field[9]->store((longlong)global_system_variables.server_id, TRUE))
    goto err;
  table->field[9]->set_notnull();

  /*
    Column sql_text.
    A positive return value in store() means truncation.
    Still logging a message in the log in this case.
  */
  if (table->field[10]->store(sql_text, sql_text_len, client_cs) < 0)
    goto err;

  if (table->field[11]->store((longlong) thd->thread_id, TRUE))
    goto err;

  /* Rows_affected */
  if (table->field[12]->store(thd->get_stmt_da()->is_ok() ?
                              (longlong) thd->get_stmt_da()->affected_rows() :
                              0, TRUE))
    goto err;

  /* log table entries are not replicated */
  if (table->file->ha_write_row(table->record[0]))
    goto err;

  result= FALSE;

err:
  thd->pop_internal_handler();

  if (result && !thd->killed)
    sql_print_error("Failed to write to mysql.slow_log: %s",
                    error_handler.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }
  if (need_close)
    close_log_table(thd, &open_tables_backup);
  thd->time_zone_used= save_time_zone_used;
  DBUG_RETURN(result);
}

int Log_to_csv_event_handler::
  activate_log(THD *thd, uint log_table_type)
{
  TABLE_LIST table_list;
  TABLE *table;
  LEX_STRING *UNINIT_VAR(log_name);
  int result;
  Open_tables_backup open_tables_backup;

  DBUG_ENTER("Log_to_csv_event_handler::activate_log");

  if (log_table_type == QUERY_LOG_GENERAL)
  {
    log_name= &GENERAL_LOG_NAME;
  }
  else
  {
    DBUG_ASSERT(log_table_type == QUERY_LOG_SLOW);

    log_name= &SLOW_LOG_NAME;
  }
  table_list.init_one_table(MYSQL_SCHEMA_NAME.str, MYSQL_SCHEMA_NAME.length,
                            log_name->str, log_name->length, log_name->str,
                            TL_WRITE_CONCURRENT_INSERT);

  table= open_log_table(thd, &table_list, &open_tables_backup);
  if (table)
  {
    result= 0;
    close_log_table(thd, &open_tables_backup);
  }
  else
    result= 1;

  DBUG_RETURN(result);
}

bool Log_to_csv_event_handler::
  log_error(enum loglevel level, const char *format, va_list args)
{
  /* No log table is implemented */
  DBUG_ASSERT(0);
  return FALSE;
}

bool Log_to_file_event_handler::
  log_error(enum loglevel level, const char *format,
            va_list args)
{
  return vprint_msg_to_log(level, format, args);
}

void Log_to_file_event_handler::init_pthread_objects()
{
  mysql_log.init_pthread_objects();
  mysql_slow_log.init_pthread_objects();
}


/** Wrapper around MYSQL_LOG::write() for slow log. */

bool Log_to_file_event_handler::
  log_slow(THD *thd, my_hrtime_t current_time,
           const char *user_host, uint user_host_len,
           ulonglong query_utime, ulonglong lock_utime, bool is_command,
           const char *sql_text, uint sql_text_len)
{
  Silence_log_table_errors error_handler;
  thd->push_internal_handler(&error_handler);
  bool retval= mysql_slow_log.write(thd, hrtime_to_my_time(current_time),
                                    user_host, user_host_len,
                                    query_utime, lock_utime, is_command,
                                    sql_text, sql_text_len);
  thd->pop_internal_handler();
  return retval;
}


/**
   Wrapper around MYSQL_LOG::write() for general log. We need it since we
   want all log event handlers to have the same signature.
*/

bool Log_to_file_event_handler::
  log_general(THD *thd, my_hrtime_t event_time, const char *user_host,
              uint user_host_len, int thread_id_arg,
              const char *command_type, uint command_type_len,
              const char *sql_text, uint sql_text_len,
              CHARSET_INFO *client_cs)
{
  Silence_log_table_errors error_handler;
  thd->push_internal_handler(&error_handler);
  bool retval= mysql_log.write(hrtime_to_time(event_time), user_host,
                               user_host_len,
                               thread_id_arg, command_type, command_type_len,
                               sql_text, sql_text_len);
  thd->pop_internal_handler();
  return retval;
}


bool Log_to_file_event_handler::init()
{
  if (!is_initialized)
  {
    if (global_system_variables.sql_log_slow)
      mysql_slow_log.open_slow_log(opt_slow_logname);

    if (opt_log)
      mysql_log.open_query_log(opt_logname);

    is_initialized= TRUE;
  }

  return FALSE;
}


void Log_to_file_event_handler::cleanup()
{
  mysql_log.cleanup();
  mysql_slow_log.cleanup();
}

void Log_to_file_event_handler::flush()
{
  /* reopen log files */
  if (opt_log)
    mysql_log.reopen_file();
  if (global_system_variables.sql_log_slow)
    mysql_slow_log.reopen_file();
}

/*
  Log error with all enabled log event handlers

  SYNOPSIS
    error_log_print()

    level             The level of the error significance: NOTE,
                      WARNING or ERROR.
    format            format string for the error message
    args              list of arguments for the format string

  RETURN
    FALSE - OK
    TRUE - error occurred
*/

bool LOGGER::error_log_print(enum loglevel level, const char *format,
                             va_list args)
{
  bool error= FALSE;
  Log_event_handler **current_handler;

  /* currently we don't need locking here as there is no error_log table */
  for (current_handler= error_log_handler_list ; *current_handler ;)
    error= (*current_handler++)->log_error(level, format, args) || error;

  return error;
}


void LOGGER::cleanup_base()
{
  DBUG_ASSERT(inited == 1);
  mysql_rwlock_destroy(&LOCK_logger);
  if (table_log_handler)
  {
    table_log_handler->cleanup();
    delete table_log_handler;
    table_log_handler= NULL;
  }
  if (file_log_handler)
    file_log_handler->cleanup();
}


void LOGGER::cleanup_end()
{
  DBUG_ASSERT(inited == 1);
  if (file_log_handler)
  {
    delete file_log_handler;
    file_log_handler=NULL;
  }
  inited= 0;
}


/**
  Perform basic log initialization: create file-based log handler and
  init error log.
*/
void LOGGER::init_base()
{
  DBUG_ASSERT(inited == 0);
  inited= 1;

  /*
    Here we create file log handler. We don't do it for the table log handler
    here as it cannot be created so early. The reason is THD initialization,
    which depends on the system variables (parsed later).
  */
  if (!file_log_handler)
    file_log_handler= new Log_to_file_event_handler;

  /* by default we use traditional error log */
  init_error_log(LOG_FILE);

  file_log_handler->init_pthread_objects();
  mysql_rwlock_init(key_rwlock_LOCK_logger, &LOCK_logger);
}


void LOGGER::init_log_tables()
{
  if (!table_log_handler)
    table_log_handler= new Log_to_csv_event_handler;

  if (!is_log_tables_initialized &&
      !table_log_handler->init() && !file_log_handler->init())
    is_log_tables_initialized= TRUE;
}


bool LOGGER::flush_logs(THD *thd)
{
  /*
    Now we lock logger, as nobody should be able to use logging routines while
    log tables are closed
  */
  logger.lock_exclusive();

  /* reopen log files */
  file_log_handler->flush();

  /* end of log flush */
  logger.unlock();
  return 0;
}


/**
  Close and reopen the slow log (with locks).
  
  @returns FALSE.
*/
bool LOGGER::flush_slow_log()
{
  /*
    Now we lock logger, as nobody should be able to use logging routines while
    log tables are closed
  */
  logger.lock_exclusive();

  /* Reopen slow log file */
  if (global_system_variables.sql_log_slow)
    file_log_handler->get_mysql_slow_log()->reopen_file();

  /* End of log flush */
  logger.unlock();

  return 0;
}


/**
  Close and reopen the general log (with locks).

  @returns FALSE.
*/
bool LOGGER::flush_general_log()
{
  /*
    Now we lock logger, as nobody should be able to use logging routines while
    log tables are closed
  */
  logger.lock_exclusive();

  /* Reopen general log file */
  if (opt_log)
    file_log_handler->get_mysql_log()->reopen_file();

  /* End of log flush */
  logger.unlock();

  return 0;
}


/*
  Log slow query with all enabled log event handlers

  SYNOPSIS
    slow_log_print()

    thd                 THD of the query being logged
    query               The query being logged
    query_length        The length of the query string
    current_utime       Current time in microseconds (from undefined start)

  RETURN
    FALSE   OK
    TRUE    error occurred
*/

bool LOGGER::slow_log_print(THD *thd, const char *query, uint query_length,
                            ulonglong current_utime)

{
  bool error= FALSE;
  Log_event_handler **current_handler;
  bool is_command= FALSE;
  char user_host_buff[MAX_USER_HOST_SIZE + 1];
  Security_context *sctx= thd->security_ctx;
  uint user_host_len= 0;
  ulonglong query_utime, lock_utime;

  DBUG_ASSERT(thd->enable_slow_log);
  /*
    Print the message to the buffer if we have slow log enabled
  */

  if (*slow_log_handler_list)
  {
    /* do not log slow queries from replication threads */
    if (!thd->variables.sql_log_slow)
      return 0;

    lock_shared();
    if (!global_system_variables.sql_log_slow)
    {
      unlock();
      return 0;
    }

    /* fill in user_host value: the format is "%s[%s] @ %s [%s]" */
    user_host_len= (strxnmov(user_host_buff, MAX_USER_HOST_SIZE,
                             sctx->priv_user ? sctx->priv_user : "", "[",
                             sctx->user ? sctx->user : (thd->slave_thread ? "SQL_SLAVE" : ""), "] @ ",
                             sctx->host ? sctx->host : "", " [",
                             sctx->ip ? sctx->ip : "", "]", NullS) -
                    user_host_buff);

    DBUG_ASSERT(thd->start_utime);
    DBUG_ASSERT(thd->start_time);
    query_utime= (current_utime - thd->start_utime);
    lock_utime=  (thd->utime_after_lock - thd->start_utime);
    my_hrtime_t current_time= { hrtime_from_time(thd->start_time) +
                                thd->start_time_sec_part + query_utime };

    if (!query)
    {
      is_command= TRUE;
      query= command_name[thd->get_command()].str;
      query_length= command_name[thd->get_command()].length;
    }

    for (current_handler= slow_log_handler_list; *current_handler ;)
      error= (*current_handler++)->log_slow(thd, current_time,
                                            user_host_buff, user_host_len,
                                            query_utime, lock_utime, is_command,
                                            query, query_length) || error;

    unlock();
  }
  return error;
}

bool LOGGER::general_log_write(THD *thd, enum enum_server_command command,
                               const char *query, uint query_length)
{
  bool error= FALSE;
  Log_event_handler **current_handler= general_log_handler_list;
  char user_host_buff[MAX_USER_HOST_SIZE + 1];
  uint user_host_len= 0;
  my_hrtime_t current_time;

  DBUG_ASSERT(thd);

  user_host_len= make_user_name(thd, user_host_buff);

  current_time= my_hrtime();

  mysql_audit_general_log(thd, hrtime_to_time(current_time),
                          user_host_buff, user_host_len,
                          command_name[(uint) command].str,
                          command_name[(uint) command].length,
                          query, query_length);
                        
  if (opt_log && log_command(thd, command))
  {
    lock_shared();
    while (*current_handler)
      error|= (*current_handler++)->
        log_general(thd, current_time, user_host_buff,
                    user_host_len, thd->thread_id,
                    command_name[(uint) command].str,
                    command_name[(uint) command].length,
                    query, query_length,
                    thd->variables.character_set_client) || error;
    unlock();
  }

  return error;
}

bool LOGGER::general_log_print(THD *thd, enum enum_server_command command,
                               const char *format, va_list args)
{
  uint message_buff_len= 0;
  char message_buff[MAX_LOG_BUFFER_SIZE];

  /* prepare message */
  if (format)
    message_buff_len= my_vsnprintf(message_buff, sizeof(message_buff),
                                   format, args);
  else
    message_buff[0]= '\0';

  return general_log_write(thd, command, message_buff, message_buff_len);
}

void LOGGER::init_error_log(ulonglong error_log_printer)
{
  if (error_log_printer & LOG_NONE)
  {
    error_log_handler_list[0]= 0;
    return;
  }

  switch (error_log_printer) {
  case LOG_FILE:
    error_log_handler_list[0]= file_log_handler;
    error_log_handler_list[1]= 0;
    break;
    /* these two are disabled for now */
  case LOG_TABLE:
    DBUG_ASSERT(0);
    break;
  case LOG_TABLE|LOG_FILE:
    DBUG_ASSERT(0);
    break;
  }
}

void LOGGER::init_slow_log(ulonglong slow_log_printer)
{
  if (slow_log_printer & LOG_NONE)
  {
    slow_log_handler_list[0]= 0;
    return;
  }

  switch (slow_log_printer) {
  case LOG_FILE:
    slow_log_handler_list[0]= file_log_handler;
    slow_log_handler_list[1]= 0;
    break;
  case LOG_TABLE:
    slow_log_handler_list[0]= table_log_handler;
    slow_log_handler_list[1]= 0;
    break;
  case LOG_TABLE|LOG_FILE:
    slow_log_handler_list[0]= file_log_handler;
    slow_log_handler_list[1]= table_log_handler;
    slow_log_handler_list[2]= 0;
    break;
  }
}

void LOGGER::init_general_log(ulonglong general_log_printer)
{
  if (general_log_printer & LOG_NONE)
  {
    general_log_handler_list[0]= 0;
    return;
  }

  switch (general_log_printer) {
  case LOG_FILE:
    general_log_handler_list[0]= file_log_handler;
    general_log_handler_list[1]= 0;
    break;
  case LOG_TABLE:
    general_log_handler_list[0]= table_log_handler;
    general_log_handler_list[1]= 0;
    break;
  case LOG_TABLE|LOG_FILE:
    general_log_handler_list[0]= file_log_handler;
    general_log_handler_list[1]= table_log_handler;
    general_log_handler_list[2]= 0;
    break;
  }
}


bool LOGGER::activate_log_handler(THD* thd, uint log_type)
{
  MYSQL_QUERY_LOG *file_log;
  bool res= FALSE;
  lock_exclusive();
  switch (log_type) {
  case QUERY_LOG_SLOW:
    if (!global_system_variables.sql_log_slow)
    {
      file_log= file_log_handler->get_mysql_slow_log();

      file_log->open_slow_log(opt_slow_logname);
      if (table_log_handler->activate_log(thd, QUERY_LOG_SLOW))
      {
        /* Error printed by open table in activate_log() */
        res= TRUE;
        file_log->close(0);
      }
      else
      {
        init_slow_log(log_output_options);
        global_system_variables.sql_log_slow= TRUE;
      }
    }
    break;
  case QUERY_LOG_GENERAL:
    if (!opt_log)
    {
      file_log= file_log_handler->get_mysql_log();

      file_log->open_query_log(opt_logname);
      if (table_log_handler->activate_log(thd, QUERY_LOG_GENERAL))
      {
        /* Error printed by open table in activate_log() */
        res= TRUE;
        file_log->close(0);
      }
      else
      {
        init_general_log(log_output_options);
        opt_log= TRUE;
      }
    }
    break;
  default:
    DBUG_ASSERT(0);
  }
  unlock();
  return res;
}


void LOGGER::deactivate_log_handler(THD *thd, uint log_type)
{
  my_bool *tmp_opt= 0;
  MYSQL_LOG *UNINIT_VAR(file_log);

  switch (log_type) {
  case QUERY_LOG_SLOW:
    tmp_opt= &global_system_variables.sql_log_slow;
    file_log= file_log_handler->get_mysql_slow_log();
    break;
  case QUERY_LOG_GENERAL:
    tmp_opt= &opt_log;
    file_log= file_log_handler->get_mysql_log();
    break;
  default:
    MY_ASSERT_UNREACHABLE();
  }

  if (!(*tmp_opt))
    return;

  lock_exclusive();
  file_log->close(0);
  *tmp_opt= FALSE;
  unlock();
}


/* the parameters are unused for the log tables */
bool Log_to_csv_event_handler::init()
{
  return 0;
}

int LOGGER::set_handlers(ulonglong error_log_printer,
                         ulonglong slow_log_printer,
                         ulonglong general_log_printer)
{
  /* error log table is not supported yet */
  DBUG_ASSERT(error_log_printer < LOG_TABLE);

  lock_exclusive();

  if ((slow_log_printer & LOG_TABLE || general_log_printer & LOG_TABLE) &&
      !is_log_tables_initialized)
  {
    slow_log_printer= (slow_log_printer & ~LOG_TABLE) | LOG_FILE;
    general_log_printer= (general_log_printer & ~LOG_TABLE) | LOG_FILE;

    sql_print_error("Failed to initialize log tables. "
                    "Falling back to the old-fashioned logs");
  }

  init_error_log(error_log_printer);
  init_slow_log(slow_log_printer);
  init_general_log(general_log_printer);

  unlock();

  return 0;
}

 /*
  Save position of binary log transaction cache.

  SYNPOSIS
    binlog_trans_log_savepos()

    thd      The thread to take the binlog data from
    pos      Pointer to variable where the position will be stored

  DESCRIPTION

    Save the current position in the binary log transaction cache into
    the variable pointed to by 'pos'
 */

static void
binlog_trans_log_savepos(THD *thd, my_off_t *pos)
{
  DBUG_ENTER("binlog_trans_log_savepos");
  DBUG_ASSERT(pos != NULL);
  binlog_cache_mngr *const cache_mngr= thd->binlog_setup_trx_data();
  DBUG_ASSERT((WSREP(thd) && wsrep_emulate_bin_log) || mysql_bin_log.is_open());
  *pos= cache_mngr->trx_cache.get_byte_position();
  DBUG_PRINT("return", ("*pos: %lu", (ulong) *pos));
  DBUG_VOID_RETURN;
}


/*
  Truncate the binary log transaction cache.

  SYNPOSIS
    binlog_trans_log_truncate()

    thd      The thread to take the binlog data from
    pos      Position to truncate to

  DESCRIPTION

    Truncate the binary log to the given position. Will not change
    anything else.

 */
static void
binlog_trans_log_truncate(THD *thd, my_off_t pos)
{
  DBUG_ENTER("binlog_trans_log_truncate");
  DBUG_PRINT("enter", ("pos: %lu", (ulong) pos));

  DBUG_ASSERT(thd_get_ha_data(thd, binlog_hton) != NULL);
  /* Only true if binlog_trans_log_savepos() wasn't called before */
  DBUG_ASSERT(pos != ~(my_off_t) 0);

  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
  cache_mngr->trx_cache.restore_savepoint(pos);
  DBUG_VOID_RETURN;
}


/*
  this function is mostly a placeholder.
  conceptually, binlog initialization (now mostly done in MYSQL_BIN_LOG::open)
  should be moved here.
*/

int binlog_init(void *p)
{
  binlog_hton= (handlerton *)p;
  binlog_hton->state= (WSREP_ON || opt_bin_log) ? SHOW_OPTION_YES
                                                : SHOW_OPTION_NO;
  binlog_hton->db_type=DB_TYPE_BINLOG;
  binlog_hton->savepoint_offset= sizeof(my_off_t);
  binlog_hton->close_connection= binlog_close_connection;
  binlog_hton->savepoint_set= binlog_savepoint_set;
  binlog_hton->savepoint_rollback= binlog_savepoint_rollback;
  binlog_hton->savepoint_rollback_can_release_mdl=
                                     binlog_savepoint_rollback_can_release_mdl;
  binlog_hton->commit= binlog_commit;
  binlog_hton->rollback= binlog_rollback;
  binlog_hton->prepare= binlog_prepare;
  binlog_hton->start_consistent_snapshot= binlog_start_consistent_snapshot;
  binlog_hton->flags= HTON_NOT_USER_SELECTABLE | HTON_HIDDEN;
  return 0;
}

#ifdef WITH_WSREP
#include "wsrep_binlog.h"
#endif /* WITH_WSREP */
static int binlog_close_connection(handlerton *hton, THD *thd)
{
  DBUG_ENTER("binlog_close_connection");
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
#ifdef WITH_WSREP
  if (cache_mngr && !cache_mngr->trx_cache.empty()) {
    IO_CACHE* cache= get_trans_log(thd);
    uchar *buf;
    size_t len=0;
    wsrep_write_cache_buf(cache, &buf, &len);
    WSREP_WARN("binlog trx cache not empty (%lu bytes) @ connection close %lld",
               len, (longlong) thd->thread_id);
    if (len > 0) wsrep_dump_rbr_buf(thd, buf, len);

    cache = cache_mngr->get_binlog_cache_log(false);
    wsrep_write_cache_buf(cache, &buf, &len);
    WSREP_WARN("binlog stmt cache not empty (%lu bytes) @ connection close %lld",
               len, (longlong) thd->thread_id);
    if (len > 0) wsrep_dump_rbr_buf(thd, buf, len);
  }
#endif /* WITH_WSREP */
  DBUG_ASSERT(cache_mngr->trx_cache.empty() && cache_mngr->stmt_cache.empty());
  thd_set_ha_data(thd, binlog_hton, NULL);
  cache_mngr->~binlog_cache_mngr();
  my_free(cache_mngr);
  DBUG_RETURN(0);
}

/*
  This function flushes a cache upon commit/rollback.

  SYNOPSIS
    binlog_flush_cache()

    thd        The thread whose transaction should be ended
    cache_mngr Pointer to the binlog_cache_mngr to use
    all        True if the entire transaction should be ended, false if
               only the statement transaction should be ended.
    end_ev     The end event to use (COMMIT, ROLLBACK, or commit XID)
    using_stmt True if the statement cache should be flushed
    using_trx  True if the transaction cache should be flushed

  DESCRIPTION

    End the currently transaction or statement. The transaction can be either
    a real transaction or a statement transaction.

    This can be to commit a transaction, with a COMMIT query event or an XA
    commit XID event. But it can also be to rollback a transaction with a
    ROLLBACK query event, used for rolling back transactions which also
    contain updates to non-transactional tables. Or it can be a flush of
    a statement cache.
 */

static int
binlog_flush_cache(THD *thd, binlog_cache_mngr *cache_mngr,
                   Log_event *end_ev, bool all, bool using_stmt,
                   bool using_trx)
{
  int error= 0;
  DBUG_ENTER("binlog_flush_cache");
  DBUG_PRINT("enter", ("end_ev: %p", end_ev));

  if ((using_stmt && !cache_mngr->stmt_cache.empty()) ||
      (using_trx && !cache_mngr->trx_cache.empty()))
  {
    if (using_stmt && thd->binlog_flush_pending_rows_event(TRUE, FALSE))
      DBUG_RETURN(1);
    if (using_trx && thd->binlog_flush_pending_rows_event(TRUE, TRUE))
      DBUG_RETURN(1);

    /*
      Doing a commit or a rollback including non-transactional tables,
      i.e., ending a transaction where we might write the transaction
      cache to the binary log.

      We can always end the statement when ending a transaction since
      transactions are not allowed inside stored functions.  If they
      were, we would have to ensure that we're not ending a statement
      inside a stored function.
    */
    error= mysql_bin_log.write_transaction_to_binlog(thd, cache_mngr,
                                                     end_ev, all,
                                                     using_stmt, using_trx);
  }
  else
  {
    /*
      This can happen in row-format binlog with something like
          BEGIN; INSERT INTO nontrans_table; INSERT IGNORE INTO trans_table;
      The nontrans_table is written directly into the binlog before commit,
      and if the trans_table is ignored there will be no rows to write when
      we get here.

      So there is no work to do. Therefore, we will not increment any XID
      count, so we must not decrement any XID count in unlog().
    */
    cache_mngr->need_unlog= 0;
  }
  cache_mngr->reset(using_stmt, using_trx);

  DBUG_ASSERT((!using_stmt || cache_mngr->stmt_cache.empty()) &&
              (!using_trx || cache_mngr->trx_cache.empty()));
  DBUG_RETURN(error);
}


/**
  This function flushes the stmt-cache upon commit.

  @param thd                The thread whose transaction should be flushed
  @param cache_mngr         Pointer to the cache manager

  @return
    nonzero if an error pops up when flushing the cache.
*/
static inline int
binlog_commit_flush_stmt_cache(THD *thd, bool all,
                               binlog_cache_mngr *cache_mngr)
{
  DBUG_ENTER("binlog_commit_flush_stmt_cache");
#ifdef WITH_WSREP
  if (thd->wsrep_mysql_replicated > 0)
  {
    DBUG_ASSERT(WSREP_ON);
    WSREP_DEBUG("avoiding binlog_commit_flush_trx_cache: %d",
                thd->wsrep_mysql_replicated);
    return 0;
  }
#endif

  Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"),
                          FALSE, TRUE, TRUE, 0);
  DBUG_RETURN(binlog_flush_cache(thd, cache_mngr, &end_evt, all, TRUE, FALSE));
}

/**
  This function flushes the trx-cache upon commit.

  @param thd                The thread whose transaction should be flushed
  @param cache_mngr         Pointer to the cache manager

  @return
    nonzero if an error pops up when flushing the cache.
*/
static inline int
binlog_commit_flush_trx_cache(THD *thd, bool all, binlog_cache_mngr *cache_mngr)
{
  DBUG_ENTER("binlog_commit_flush_trx_cache");
  Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"),
                          TRUE, TRUE, TRUE, 0);
  DBUG_RETURN(binlog_flush_cache(thd, cache_mngr, &end_evt, all, FALSE, TRUE));
}

/**
  This function flushes the trx-cache upon rollback.

  @param thd                The thread whose transaction should be flushed
  @param cache_mngr         Pointer to the cache manager

  @return
    nonzero if an error pops up when flushing the cache.
*/
static inline int
binlog_rollback_flush_trx_cache(THD *thd, bool all,
                                binlog_cache_mngr *cache_mngr)
{
  Query_log_event end_evt(thd, STRING_WITH_LEN("ROLLBACK"),
                          TRUE, TRUE, TRUE, 0);
  return (binlog_flush_cache(thd, cache_mngr, &end_evt, all, FALSE, TRUE));
}

/**
  This function flushes the trx-cache upon commit.

  @param thd                The thread whose transaction should be flushed
  @param cache_mngr         Pointer to the cache manager
  @param xid                Transaction Id

  @return
    nonzero if an error pops up when flushing the cache.
*/
static inline int
binlog_commit_flush_xid_caches(THD *thd, binlog_cache_mngr *cache_mngr,
                               bool all, my_xid xid)
{
  if (xid)
  {
    Xid_log_event end_evt(thd, xid, TRUE);
    return (binlog_flush_cache(thd, cache_mngr, &end_evt, all, TRUE, TRUE));
  }
  else
  {
    /*
      Empty xid occurs in XA COMMIT ... ONE PHASE.
      In this case, we do not have a MySQL xid for the transaction, and the
      external XA transaction coordinator will have to handle recovery if
      needed. So we end the transaction with a plain COMMIT query event.
    */
    Query_log_event end_evt(thd, STRING_WITH_LEN("COMMIT"),
                            TRUE, TRUE, TRUE, 0);
    return (binlog_flush_cache(thd, cache_mngr, &end_evt, all, TRUE, TRUE));
  }
}

/**
  This function truncates the transactional cache upon committing or rolling
  back either a transaction or a statement.

  @param thd        The thread whose transaction should be flushed
  @param cache_mngr Pointer to the cache data to be flushed
  @param all        @c true means truncate the transaction, otherwise the
                    statement must be truncated.

  @return
    nonzero if an error pops up when truncating the transactional cache.
*/
static int
binlog_truncate_trx_cache(THD *thd, binlog_cache_mngr *cache_mngr, bool all)
{
  DBUG_ENTER("binlog_truncate_trx_cache");
  int error=0;
  /*
    This function handles transactional changes and as such this flag
    equals to true.
  */
  bool const is_transactional= TRUE;

  DBUG_PRINT("info", ("thd->options={ %s %s}, transaction: %s",
                      FLAGSTR(thd->variables.option_bits, OPTION_NOT_AUTOCOMMIT),
                      FLAGSTR(thd->variables.option_bits, OPTION_BEGIN),
                      all ? "all" : "stmt"));

  thd->binlog_remove_pending_rows_event(TRUE, is_transactional);
  /*
    If rolling back an entire transaction or a single statement not
    inside a transaction, we reset the transaction cache.
  */
  if (ending_trans(thd, all))
  {
    if (cache_mngr->trx_cache.has_incident())
      error= mysql_bin_log.write_incident(thd);

    thd->clear_binlog_table_maps();

    cache_mngr->reset(false, true);
  }
  /*
    If rolling back a statement in a transaction, we truncate the
    transaction cache to remove the statement.
  */
  else
    cache_mngr->trx_cache.restore_prev_position();

  DBUG_ASSERT(thd->binlog_get_pending_rows_event(is_transactional) == NULL);
  DBUG_RETURN(error);
}

static int binlog_prepare(handlerton *hton, THD *thd, bool all)
{
  /*
    do nothing.
    just pretend we can do 2pc, so that MySQL won't
    switch to 1pc.
    real work will be done in MYSQL_BIN_LOG::log_and_order()
  */
  return 0;
}

/*
  We flush the cache wrapped in a beging/rollback if:
    . aborting a single or multi-statement transaction and;
    . the OPTION_KEEP_LOG is active or;
    . the format is STMT and a non-trans table was updated or;
    . the format is MIXED and a temporary non-trans table was
      updated or;
    . the format is MIXED, non-trans table was updated and
      aborting a single statement transaction;
*/
static bool trans_cannot_safely_rollback(THD *thd, bool all)
{
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  return ((thd->variables.option_bits & OPTION_KEEP_LOG) ||
          (trans_has_updated_non_trans_table(thd) &&
           thd->wsrep_binlog_format() == BINLOG_FORMAT_STMT) ||
          (cache_mngr->trx_cache.changes_to_non_trans_temp_table() &&
           thd->wsrep_binlog_format() == BINLOG_FORMAT_MIXED) ||
          (trans_has_updated_non_trans_table(thd) &&
           ending_single_stmt_trans(thd,all) &&
           thd->wsrep_binlog_format() == BINLOG_FORMAT_MIXED));
}


/**
  This function is called once after each statement.

  It has the responsibility to flush the caches to the binary log on commits.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction commit, and
               @false otherwise.

  @see handlerton::commit
*/
static int binlog_commit(handlerton *hton, THD *thd, bool all)
{
  int error= 0;
  DBUG_ENTER("binlog_commit");
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  if (!cache_mngr)
  {
    DBUG_ASSERT(WSREP(thd));
    DBUG_RETURN(0);
  }

  DBUG_PRINT("debug",
             ("all: %d, in_transaction: %s, all.modified_non_trans_table: %s, stmt.modified_non_trans_table: %s",
              all,
              YESNO(thd->in_multi_stmt_transaction_mode()),
              YESNO(thd->transaction.all.modified_non_trans_table),
              YESNO(thd->transaction.stmt.modified_non_trans_table)));

  if (!cache_mngr->stmt_cache.empty())
  {
    error= binlog_commit_flush_stmt_cache(thd, all, cache_mngr);
  }

  if (cache_mngr->trx_cache.empty())
  {
    /*
      we're here because cache_log was flushed in MYSQL_BIN_LOG::log_xid()
    */
    cache_mngr->reset(false, true);
    DBUG_RETURN(error);
  }

  /*
    We commit the transaction if:
     - We are not in a transaction and committing a statement, or
     - We are in a transaction and a full transaction is committed.
    Otherwise, we accumulate the changes.
  */
  if (!error && ending_trans(thd, all))
    error= binlog_commit_flush_trx_cache(thd, all, cache_mngr);

  /*
    This is part of the stmt rollback.
  */
  if (!all)
    cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);

  DBUG_RETURN(error);
}

/**
  This function is called when a transaction or a statement is rolled back.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction rollback, and
               @false otherwise.

  @see handlerton::rollback
*/
static int binlog_rollback(handlerton *hton, THD *thd, bool all)
{
  DBUG_ENTER("binlog_rollback");
  int error= 0;
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  if (!cache_mngr)
  {
    DBUG_ASSERT(WSREP(thd));
    DBUG_RETURN(0);
  }

  DBUG_PRINT("debug", ("all: %s, all.modified_non_trans_table: %s, stmt.modified_non_trans_table: %s",
                       YESNO(all),
                       YESNO(thd->transaction.all.modified_non_trans_table),
                       YESNO(thd->transaction.stmt.modified_non_trans_table)));

  /*
    If an incident event is set we do not flush the content of the statement
    cache because it may be corrupted.
  */
  if (cache_mngr->stmt_cache.has_incident())
  {
    error= mysql_bin_log.write_incident(thd);
    cache_mngr->reset(true, false);
  }
  else if (!cache_mngr->stmt_cache.empty())
  {
    error= binlog_commit_flush_stmt_cache(thd, all, cache_mngr);
  }

  if (cache_mngr->trx_cache.empty())
  {
    /*
      we're here because cache_log was flushed in MYSQL_BIN_LOG::log_xid()
    */
    cache_mngr->reset(false, true);
    DBUG_RETURN(error);
  }
  if (!wsrep_emulate_bin_log && mysql_bin_log.check_write_error(thd))
  {
    /*
      "all == true" means that a "rollback statement" triggered the error and
      this function was called. However, this must not happen as a rollback
      is written directly to the binary log. And in auto-commit mode, a single
      statement that is rolled back has the flag all == false.
    */
    DBUG_ASSERT(!all);
    /*
      We reach this point if the effect of a statement did not properly get into
      a cache and need to be rolled back.
    */
    error |= binlog_truncate_trx_cache(thd, cache_mngr, all);
  }
  else if (!error)
  {  
    if (ending_trans(thd, all) && trans_cannot_safely_rollback(thd, all))
      error= binlog_rollback_flush_trx_cache(thd, all, cache_mngr);
    /*
      Truncate the cache if:
        . aborting a single or multi-statement transaction or;
        . the OPTION_KEEP_LOG is not active and;
        . the format is not STMT or no non-trans table was
          updated and;
        . the format is not MIXED or no temporary non-trans table
          was updated.
    */
    else if (ending_trans(thd, all) ||
             (!(thd->variables.option_bits & OPTION_KEEP_LOG) &&
              (!stmt_has_updated_non_trans_table(thd) ||
               thd->wsrep_binlog_format() != BINLOG_FORMAT_STMT) &&
              (!cache_mngr->trx_cache.changes_to_non_trans_temp_table() ||
               thd->wsrep_binlog_format() != BINLOG_FORMAT_MIXED)))
      error= binlog_truncate_trx_cache(thd, cache_mngr, all);
  }

  /* 
    This is part of the stmt rollback.
  */
  if (!all)
    cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);

  DBUG_RETURN(error);
}


void binlog_reset_cache(THD *thd)
{
  binlog_cache_mngr *const cache_mngr= opt_bin_log ? 
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton) : 0;
  DBUG_ENTER("binlog_reset_cache");
  if (cache_mngr)
  {
    thd->binlog_remove_pending_rows_event(TRUE, TRUE);
    cache_mngr->reset(true, true);
  }
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::set_write_error(THD *thd, bool is_transactional)
{
  DBUG_ENTER("MYSQL_BIN_LOG::set_write_error");

  write_error= 1;

  if (check_write_error(thd))
    DBUG_VOID_RETURN;

  if (my_errno == EFBIG)
  {
    if (is_transactional)
    {
      my_message(ER_TRANS_CACHE_FULL, ER_THD(thd, ER_TRANS_CACHE_FULL), MYF(MY_WME));
    }
    else
    {
      my_message(ER_STMT_CACHE_FULL, ER_THD(thd, ER_STMT_CACHE_FULL), MYF(MY_WME));
    }
  }
  else
  {
    my_error(ER_ERROR_ON_WRITE, MYF(MY_WME), name, errno);
  }

  DBUG_VOID_RETURN;
}

bool MYSQL_BIN_LOG::check_write_error(THD *thd)
{
  DBUG_ENTER("MYSQL_BIN_LOG::check_write_error");

  bool checked= FALSE;

  if (!thd->is_error())
    DBUG_RETURN(checked);

  switch (thd->get_stmt_da()->sql_errno())
  {
    case ER_TRANS_CACHE_FULL:
    case ER_STMT_CACHE_FULL:
    case ER_ERROR_ON_WRITE:
    case ER_BINLOG_LOGGING_IMPOSSIBLE:
      checked= TRUE;
    break;
  }

  DBUG_RETURN(checked);
}


/**
  @note
  How do we handle this (unlikely but legal) case:
  @verbatim
    [transaction] + [update to non-trans table] + [rollback to savepoint] ?
  @endverbatim
  The problem occurs when a savepoint is before the update to the
  non-transactional table. Then when there's a rollback to the savepoint, if we
  simply truncate the binlog cache, we lose the part of the binlog cache where
  the update is. If we want to not lose it, we need to write the SAVEPOINT
  command and the ROLLBACK TO SAVEPOINT command to the binlog cache. The latter
  is easy: it's just write at the end of the binlog cache, but the former
  should be *inserted* to the place where the user called SAVEPOINT. The
  solution is that when the user calls SAVEPOINT, we write it to the binlog
  cache (so no need to later insert it). As transactions are never intermixed
  in the binary log (i.e. they are serialized), we won't have conflicts with
  savepoint names when using mysqlbinlog or in the slave SQL thread.
  Then when ROLLBACK TO SAVEPOINT is called, if we updated some
  non-transactional table, we don't truncate the binlog cache but instead write
  ROLLBACK TO SAVEPOINT to it; otherwise we truncate the binlog cache (which
  will chop the SAVEPOINT command from the binlog cache, which is good as in
  that case there is no need to have it in the binlog).
*/

static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv)
{
  int error= 1;
  DBUG_ENTER("binlog_savepoint_set");

  if (wsrep_emulate_bin_log)
    DBUG_RETURN(0);

  char buf[1024];

  String log_query(buf, sizeof(buf), &my_charset_bin);
  if (log_query.copy(STRING_WITH_LEN("SAVEPOINT "), &my_charset_bin) ||
      append_identifier(thd, &log_query,
                        thd->lex->ident.str, thd->lex->ident.length))
    DBUG_RETURN(1);
  int errcode= query_error_code(thd, thd->killed == NOT_KILLED);
  Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(),
                        TRUE, FALSE, TRUE, errcode);
  /* 
    We cannot record the position before writing the statement
    because a rollback to a savepoint (.e.g. consider it "S") would
    prevent the savepoint statement (i.e. "SAVEPOINT S") from being
    written to the binary log despite the fact that the server could
    still issue other rollback statements to the same savepoint (i.e. 
    "S"). 
    Given that the savepoint is valid until the server releases it,
    ie, until the transaction commits or it is released explicitly,
    we need to log it anyway so that we don't have "ROLLBACK TO S"
    or "RELEASE S" without the preceding "SAVEPOINT S" in the binary
    log.
  */
  if (!(error= mysql_bin_log.write(&qinfo)))
    binlog_trans_log_savepos(thd, (my_off_t*) sv);

  DBUG_RETURN(error);
}

static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("binlog_savepoint_rollback");

  if (wsrep_emulate_bin_log)
    DBUG_RETURN(0);

  /*
    Write ROLLBACK TO SAVEPOINT to the binlog cache if we have updated some
    non-transactional table. Otherwise, truncate the binlog cache starting
    from the SAVEPOINT command.
  */
  if (unlikely(trans_has_updated_non_trans_table(thd) ||
               (thd->variables.option_bits & OPTION_KEEP_LOG)))
  {
    char buf[1024];
    String log_query(buf, sizeof(buf), &my_charset_bin);
    if (log_query.copy(STRING_WITH_LEN("ROLLBACK TO "), &my_charset_bin) ||
        append_identifier(thd, &log_query,
                          thd->lex->ident.str, thd->lex->ident.length))
      DBUG_RETURN(1);
    int errcode= query_error_code(thd, thd->killed == NOT_KILLED);
    Query_log_event qinfo(thd, log_query.ptr(), log_query.length(),
                          TRUE, FALSE, TRUE, errcode);
    DBUG_RETURN(mysql_bin_log.write(&qinfo));
  }

  binlog_trans_log_truncate(thd, *(my_off_t*)sv);

  /*
    When a SAVEPOINT is executed inside a stored function/trigger we force the
    pending event to be flushed with a STMT_END_F flag and clear the table maps
    as well to ensure that following DMLs will have a clean state to start
    with. ROLLBACK inside a stored routine has to finalize possibly existing
    current row-based pending event with cleaning up table maps. That ensures
    that following DMLs will have a clean state to start with.
   */
  if (thd->in_sub_stmt)
    thd->clear_binlog_table_maps();

  DBUG_RETURN(0);
}


/**
  Check whether binlog state allows to safely release MDL locks after
  rollback to savepoint.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.

  @return true  - It is safe to release MDL locks.
          false - If it is not.
*/
static bool binlog_savepoint_rollback_can_release_mdl(handlerton *hton,
                                                      THD *thd)
{
  DBUG_ENTER("binlog_savepoint_rollback_can_release_mdl");
  /*
    If we have not updated any non-transactional tables rollback
    to savepoint will simply truncate binlog cache starting from
    SAVEPOINT command. So it should be safe to release MDL acquired
    after SAVEPOINT command in this case.
  */
  DBUG_RETURN(!trans_cannot_safely_rollback(thd, true));
}


int check_binlog_magic(IO_CACHE* log, const char** errmsg)
{
  uchar magic[4];
  DBUG_ASSERT(my_b_tell(log) == 0);

  if (my_b_read(log, magic, sizeof(magic)))
  {
    *errmsg = "I/O error reading the header from the binary log";
    sql_print_error("%s, errno=%d, io cache code=%d", *errmsg, my_errno,
		    log->error);
    return 1;
  }
  if (bcmp(magic, BINLOG_MAGIC, sizeof(magic)))
  {
    *errmsg = "Binlog has bad magic number;  It's not a binary log file that can be used by this version of MySQL";
    return 1;
  }
  return 0;
}


File open_binlog(IO_CACHE *log, const char *log_file_name, const char **errmsg)
{
  File file;
  DBUG_ENTER("open_binlog");

  if ((file= mysql_file_open(key_file_binlog,
                             log_file_name, O_RDONLY | O_BINARY | O_SHARE,
                             MYF(MY_WME))) < 0)
  {
    sql_print_error("Failed to open log (file '%s', errno %d)",
                    log_file_name, my_errno);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (init_io_cache(log, file, IO_SIZE*2, READ_CACHE, 0, 0,
                    MYF(MY_WME|MY_DONT_CHECK_FILESIZE)))
  {
    sql_print_error("Failed to create a cache on log (file '%s')",
                    log_file_name);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (check_binlog_magic(log,errmsg))
    goto err;
  DBUG_RETURN(file);

err:
  if (file >= 0)
  {
    mysql_file_close(file, MYF(0));
    end_io_cache(log);
  }
  DBUG_RETURN(-1);
}

#ifdef _WIN32
static int eventSource = 0;

static void setup_windows_event_source()
{
  HKEY    hRegKey= NULL;
  DWORD   dwError= 0;
  TCHAR   szPath[MAX_PATH];
  DWORD dwTypes;

  if (eventSource)               // Ensure that we are only called once
    return;
  eventSource= 1;

  // Create the event source registry key
  dwError= RegCreateKey(HKEY_LOCAL_MACHINE,
                          "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\MySQL", 
                          &hRegKey);

  /* Name of the PE module that contains the message resource */
  GetModuleFileName(NULL, szPath, MAX_PATH);

  /* Register EventMessageFile */
  dwError = RegSetValueEx(hRegKey, "EventMessageFile", 0, REG_EXPAND_SZ,
                          (PBYTE) szPath, (DWORD) (strlen(szPath) + 1));

  /* Register supported event types */
  dwTypes= (EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
            EVENTLOG_INFORMATION_TYPE);
  dwError= RegSetValueEx(hRegKey, "TypesSupported", 0, REG_DWORD,
                         (LPBYTE) &dwTypes, sizeof dwTypes);

  RegCloseKey(hRegKey);
}

#endif /* _WIN32 */


/**
  Find a unique filename for 'filename.#'.

  Set '#' to the number next to the maximum found in the most
  recent log file extension.

  This function will return nonzero if: (i) the generated name
  exceeds FN_REFLEN; (ii) if the number of extensions is exhausted;
  or (iii) some other error happened while examining the filesystem.

  @return
    nonzero if not possible to get unique filename.
*/

static int find_uniq_filename(char *name, ulong next_log_number)
{
  uint                  i;
  char                  buff[FN_REFLEN], ext_buf[FN_REFLEN];
  struct st_my_dir     *dir_info;
  reg1 struct fileinfo *file_info;
  ulong                 max_found, next, number;
  size_t		buf_length, length;
  char			*start, *end;
  int                   error= 0;
  DBUG_ENTER("find_uniq_filename");

  length= dirname_part(buff, name, &buf_length);
  start=  name + length;
  end=    strend(start);

  *end='.';
  length= (size_t) (end - start + 1);

  if ((DBUG_EVALUATE_IF("error_unique_log_filename", 1, 
      !(dir_info= my_dir(buff,MYF(MY_DONT_SORT))))))
  {						// This shouldn't happen
    strmov(end,".1");				// use name+1
    DBUG_RETURN(1);
  }
  file_info= dir_info->dir_entry;
  max_found= next_log_number ? next_log_number-1 : 0;
  for (i= dir_info->number_of_files ; i-- ; file_info++)
  {
    if (strncmp(file_info->name, start, length) == 0 &&
	test_if_number(file_info->name+length, &number,0))
    {
      set_if_bigger(max_found,(ulong) number);
    }
  }
  my_dirend(dir_info);

  /* check if reached the maximum possible extension number */
  if (max_found >= MAX_LOG_UNIQUE_FN_EXT)
  {
    sql_print_error("Log filename extension number exhausted: %06lu. \
Please fix this by archiving old logs and \
updating the index files.", max_found);
    error= 1;
    goto end;
  }

  next= max_found + 1;
  if (sprintf(ext_buf, "%06lu", next)<0)
  {
    error= 1;
    goto end;
  }
  *end++='.';

  /* 
    Check if the generated extension size + the file name exceeds the
    buffer size used. If one did not check this, then the filename might be
    truncated, resulting in error.
   */
  if (((strlen(ext_buf) + (end - name)) >= FN_REFLEN))
  {
    sql_print_error("Log filename too large: %s%s (%zu). \
Please fix this by archiving old logs and updating the \
index files.", name, ext_buf, (strlen(ext_buf) + (end - name)));
    error= 1;
    goto end;
  }

  if (sprintf(end, "%06lu", next)<0)
  {
    error= 1;
    goto end;
  }

  /* print warning if reaching the end of available extensions. */
  if ((next > (MAX_LOG_UNIQUE_FN_EXT - LOG_WARN_UNIQUE_FN_EXT_LEFT)))
    sql_print_warning("Next log extension: %lu. \
Remaining log filename extensions: %lu. \
Please consider archiving some logs.", next, (MAX_LOG_UNIQUE_FN_EXT - next));

end:
  DBUG_RETURN(error);
}


void MYSQL_LOG::init(enum_log_type log_type_arg,
                     enum cache_type io_cache_type_arg)
{
  DBUG_ENTER("MYSQL_LOG::init");
  log_type= log_type_arg;
  io_cache_type= io_cache_type_arg;
  DBUG_PRINT("info",("log_type: %d", log_type));
  DBUG_VOID_RETURN;
}


bool MYSQL_LOG::init_and_set_log_file_name(const char *log_name,
                                           const char *new_name,
                                           ulong next_log_number,
                                           enum_log_type log_type_arg,
                                           enum cache_type io_cache_type_arg)
{
  init(log_type_arg, io_cache_type_arg);

  if (new_name)
  {
    strmov(log_file_name, new_name);
  }
  else if (!new_name && generate_new_name(log_file_name, log_name,
                                          next_log_number))
    return TRUE;

  return FALSE;
}


/*
  Open a (new) log file.

  SYNOPSIS
    open()

    log_name            The name of the log to open
    log_type_arg        The type of the log. E.g. LOG_NORMAL
    new_name            The new name for the logfile. This is only needed
                        when the method is used to open the binlog file.
    io_cache_type_arg   The type of the IO_CACHE to use for this log file

  DESCRIPTION
    Open the logfile, init IO_CACHE and write startup messages
    (in case of general and slow query logs).

  RETURN VALUES
    0   ok
    1   error
*/

bool MYSQL_LOG::open(
#ifdef HAVE_PSI_INTERFACE
                     PSI_file_key log_file_key,
#endif
                     const char *log_name, enum_log_type log_type_arg,
                     const char *new_name, ulong next_log_number,
                     enum cache_type io_cache_type_arg)
{
  char buff[FN_REFLEN];
  MY_STAT f_stat;
  File file= -1;
  my_off_t seek_offset;
  bool is_fifo = false;
  int open_flags= O_CREAT | O_BINARY;
  DBUG_ENTER("MYSQL_LOG::open");
  DBUG_PRINT("enter", ("log_type: %d", (int) log_type_arg));

  write_error= 0;

  if (!(name= my_strdup(log_name, MYF(MY_WME))))
  {
    name= (char *)log_name; // for the error message
    goto err;
  }

  /*
    log_type is LOG_UNKNOWN if we should not generate a new name
    This is only used when called from MYSQL_BINARY_LOG::open, which
    has already updated log_file_name.
   */
  if (log_type_arg != LOG_UNKNOWN &&
      init_and_set_log_file_name(name, new_name, next_log_number,
                                 log_type_arg, io_cache_type_arg))
    goto err;

  is_fifo = my_stat(log_file_name, &f_stat, MYF(0)) &&
            MY_S_ISFIFO(f_stat.st_mode);

  if (io_cache_type == SEQ_READ_APPEND)
    open_flags |= O_RDWR | O_APPEND;
  else
    open_flags |= O_WRONLY | (log_type == LOG_BIN ? 0 : O_APPEND);

  if (is_fifo)
    open_flags |= O_NONBLOCK;

  db[0]= 0;

#ifdef HAVE_PSI_INTERFACE
  /* Keep the key for reopen */
  m_log_file_key= log_file_key;
#endif

  if ((file= mysql_file_open(log_file_key, log_file_name, open_flags,
                             MYF(MY_WME | ME_WAITTANG))) < 0)
    goto err;

  if (is_fifo)
    seek_offset= 0;
  else if ((seek_offset= mysql_file_tell(file, MYF(MY_WME))))
    goto err;

  if (init_io_cache(&log_file, file, IO_SIZE, io_cache_type, seek_offset, 0,
                    MYF(MY_WME | MY_NABP |
                        ((log_type == LOG_BIN) ? MY_WAIT_IF_FULL : 0))))
    goto err;

  if (log_type == LOG_NORMAL)
  {
    char *end;
    int len=my_snprintf(buff, sizeof(buff), "%s, Version: %s (%s). "
#ifdef EMBEDDED_LIBRARY
                        "embedded library\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT
#elif _WIN32
			"started with:\nTCP Port: %d, Named Pipe: %s\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT,
                        mysqld_port, mysqld_unix_port
#else
			"started with:\nTcp port: %d  Unix socket: %s\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT,
                        mysqld_port, mysqld_unix_port
#endif
                       );
    end= strnmov(buff + len, "Time                 Id Command    Argument\n",
                 sizeof(buff) - len);
    if (my_b_write(&log_file, (uchar*) buff, (uint) (end-buff)) ||
	flush_io_cache(&log_file))
      goto err;
  }

  log_state= LOG_OPENED;
  DBUG_RETURN(0);

err:
  sql_print_error("Could not use %s for logging (error %d). \
Turning logging off for the whole duration of the MySQL server process. \
To turn it on again: fix the cause, \
shutdown the MySQL server and restart it.", name, errno);
  if (file >= 0)
    mysql_file_close(file, MYF(0));
  end_io_cache(&log_file);
  my_free(name);
  name= NULL;
  log_state= LOG_CLOSED;
  DBUG_RETURN(1);
}

MYSQL_LOG::MYSQL_LOG()
  : name(0), write_error(FALSE), inited(FALSE), log_type(LOG_UNKNOWN),
    log_state(LOG_CLOSED)
{
  /*
    We don't want to initialize LOCK_Log here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
  bzero((char*) &log_file, sizeof(log_file));
}

void MYSQL_LOG::init_pthread_objects()
{
  DBUG_ASSERT(inited == 0);
  inited= 1;
  mysql_mutex_init(key_LOG_LOCK_log, &LOCK_log, MY_MUTEX_INIT_SLOW);
}

/*
  Close the log file

  SYNOPSIS
    close()
    exiting     Bitmask. LOG_CLOSE_TO_BE_OPENED is used if we intend to call
                open at once after close. LOG_CLOSE_DELAYED_CLOSE is used for
                binlog rotation, to delay actual close of the old file until
                we have successfully created the new file.

  NOTES
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_LOG::close(uint exiting)
{					// One can't set log_type here!
  DBUG_ENTER("MYSQL_LOG::close");
  DBUG_PRINT("enter",("exiting: %d", (int) exiting));
  if (log_state == LOG_OPENED)
  {
    end_io_cache(&log_file);

    if (log_type == LOG_BIN && mysql_file_sync(log_file.file, MYF(MY_WME)) && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER_THD_OR_DEFAULT(current_thd, ER_ERROR_ON_WRITE), name, errno);
    }

    if (!(exiting & LOG_CLOSE_DELAYED_CLOSE) &&
        mysql_file_close(log_file.file, MYF(MY_WME)) && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER_THD_OR_DEFAULT(current_thd, ER_ERROR_ON_WRITE), name, errno);
    }
  }

  log_state= (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  my_free(name);
  name= NULL;
  DBUG_VOID_RETURN;
}

/** This is called only once. */

void MYSQL_LOG::cleanup()
{
  DBUG_ENTER("cleanup");
  if (inited)
  {
    inited= 0;
    mysql_mutex_destroy(&LOCK_log);
    close(0);
  }
  DBUG_VOID_RETURN;
}


int MYSQL_LOG::generate_new_name(char *new_name, const char *log_name,
                                 ulong next_log_number)
{
  fn_format(new_name, log_name, mysql_data_home, "", 4);
  if (log_type == LOG_BIN)
  {
    if (!fn_ext(log_name)[0])
    {
      if (DBUG_EVALUATE_IF("binlog_inject_new_name_error", TRUE, FALSE) ||
          find_uniq_filename(new_name, next_log_number))
      {
        THD *thd= current_thd;
        if (thd)
          my_printf_error(ER_NO_UNIQUE_LOGFILE,
                          ER_THD(thd, ER_NO_UNIQUE_LOGFILE),
                          MYF(ME_FATALERROR), log_name);
        sql_print_error(ER_DEFAULT(ER_NO_UNIQUE_LOGFILE), log_name);
	return 1;
      }
    }
  }
  return 0;
}


/*
  Reopen the log file

  SYNOPSIS
    reopen_file()

  DESCRIPTION
    Reopen the log file. The method is used during FLUSH LOGS
    and locks LOCK_log mutex
*/


void MYSQL_QUERY_LOG::reopen_file()
{
  char *save_name;

  DBUG_ENTER("MYSQL_LOG::reopen_file");
  if (!is_open())
  {
    DBUG_PRINT("info",("log is closed"));
    DBUG_VOID_RETURN;
  }

  mysql_mutex_lock(&LOCK_log);

  save_name= name;
  name= 0;				// Don't free name
  close(LOG_CLOSE_TO_BE_OPENED);

  /*
     Note that at this point, log_state != LOG_CLOSED (important for is_open()).
  */

  open(
#ifdef HAVE_PSI_INTERFACE
       m_log_file_key,
#endif
       save_name, log_type, 0, 0, io_cache_type);
  my_free(save_name);

  mysql_mutex_unlock(&LOCK_log);

  DBUG_VOID_RETURN;
}


/*
  Write a command to traditional general log file

  SYNOPSIS
    write()

    event_time        command start timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log  event handlers
    thread_id         Id of the thread, issued a query
    command_type      the type of the command being logged
    command_type_len  the length of the string above
    sql_text          the very text of the query being executed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log given command to to normal (not rotable) log file

  RETURN
    FASE - OK
    TRUE - error occurred
*/

bool MYSQL_QUERY_LOG::write(time_t event_time, const char *user_host,
                            uint user_host_len, int thread_id_arg,
                            const char *command_type, uint command_type_len,
                            const char *sql_text, uint sql_text_len)
{
  char buff[32];
  uint length= 0;
  char local_time_buff[MAX_TIME_SIZE];
  struct tm start;
  uint time_buff_len= 0;

  mysql_mutex_lock(&LOCK_log);

  /* Test if someone closed between the is_open test and lock */
  if (is_open())
  {
    /* for testing output of timestamp and thread id */
    DBUG_EXECUTE_IF("reset_log_last_time", last_time= 0;);

    /* Note that my_b_write() assumes it knows the length for this */
      if (event_time != last_time)
      {
        last_time= event_time;

        localtime_r(&event_time, &start);

        time_buff_len= my_snprintf(local_time_buff, MAX_TIME_SIZE,
                                   "%02d%02d%02d %2d:%02d:%02d\t",
                                   start.tm_year % 100, start.tm_mon + 1,
                                   start.tm_mday, start.tm_hour,
                                   start.tm_min, start.tm_sec);

        if (my_b_write(&log_file, (uchar*) local_time_buff, time_buff_len))
          goto err;
      }
      else
        if (my_b_write(&log_file, (uchar*) "\t\t" ,2) < 0)
          goto err;

      /* command_type, thread_id */
      length= my_snprintf(buff, 32, "%5ld ", (long) thread_id_arg);

    if (my_b_write(&log_file, (uchar*) buff, length))
      goto err;

    if (my_b_write(&log_file, (uchar*) command_type, command_type_len))
      goto err;

    if (my_b_write(&log_file, (uchar*) "\t", 1))
      goto err;

    /* sql_text */
    if (my_b_write(&log_file, (uchar*) sql_text, sql_text_len))
      goto err;

    if (my_b_write(&log_file, (uchar*) "\n", 1) ||
        flush_io_cache(&log_file))
      goto err;
  }

  mysql_mutex_unlock(&LOCK_log);
  return FALSE;
err:

  if (!write_error)
  {
    write_error= 1;
    sql_print_error(ER(ER_ERROR_ON_WRITE), name, errno);
  }
  mysql_mutex_unlock(&LOCK_log);
  return TRUE;
}


/*
  Log a query to the traditional slow log file

  SYNOPSIS
    write()

    thd               THD of the query
    current_time      current timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log event handlers
    query_utime       Amount of time the query took to execute (in microseconds)
    lock_utime        Amount of time the query was locked (in microseconds)
    is_command        The flag, which determines, whether the sql_text is a
                      query or an administrator command.
    sql_text          the very text of the query or administrator command
                      processed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log a query to the slow log file.

  RETURN
    FALSE - OK
    TRUE - error occurred
*/

bool MYSQL_QUERY_LOG::write(THD *thd, time_t current_time,
                            const char *user_host,
                            uint user_host_len, ulonglong query_utime,
                            ulonglong lock_utime, bool is_command,
                            const char *sql_text, uint sql_text_len)
{
  bool error= 0;
  DBUG_ENTER("MYSQL_QUERY_LOG::write");

  mysql_mutex_lock(&LOCK_log);

  if (!is_open())
  {
    mysql_mutex_unlock(&LOCK_log);
    DBUG_RETURN(0);
  }

  if (is_open())
  {						// Safety agains reopen
    int tmp_errno= 0;
    char buff[80], *end;
    char query_time_buff[22+7], lock_time_buff[22+7];
    uint buff_len;
    end= buff;

    if (!(specialflag & SPECIAL_SHORT_LOG_FORMAT))
    {
      if (current_time != last_time)
      {
        last_time= current_time;
        struct tm start;
        localtime_r(&current_time, &start);

        buff_len= my_snprintf(buff, sizeof buff,
                              "# Time: %02d%02d%02d %2d:%02d:%02d\n",
                              start.tm_year % 100, start.tm_mon + 1,
                              start.tm_mday, start.tm_hour,
                              start.tm_min, start.tm_sec);

        /* Note that my_b_write() assumes it knows the length for this */
        if (my_b_write(&log_file, (uchar*) buff, buff_len))
          tmp_errno= errno;
      }
      const uchar uh[]= "# User@Host: ";
      if (my_b_write(&log_file, uh, sizeof(uh) - 1))
        tmp_errno= errno;
      if (my_b_write(&log_file, (uchar*) user_host, user_host_len))
        tmp_errno= errno;
      if (my_b_write(&log_file, (uchar*) "\n", 1))
        tmp_errno= errno;
    }
    
    /* For slow query log */
    sprintf(query_time_buff, "%.6f", ulonglong2double(query_utime)/1000000.0);
    sprintf(lock_time_buff,  "%.6f", ulonglong2double(lock_utime)/1000000.0);
    if (my_b_printf(&log_file,
                    "# Thread_id: %lu  Schema: %s  QC_hit: %s\n" \
                    "# Query_time: %s  Lock_time: %s  Rows_sent: %lu  Rows_examined: %lu\n" \
                    "# Rows_affected: %lu\n",
                    (ulong) thd->thread_id, (thd->db ? thd->db : ""),
                    ((thd->query_plan_flags & QPLAN_QC) ? "Yes" : "No"),
                    query_time_buff, lock_time_buff,
                    (ulong) thd->get_sent_row_count(),
                    (ulong) thd->get_examined_row_count(),
                    thd->get_stmt_da()->is_ok() ?
                    (ulong) thd->get_stmt_da()->affected_rows() :
                    0) == (size_t) -1)
      tmp_errno= errno;
     if ((thd->variables.log_slow_verbosity & LOG_SLOW_VERBOSITY_QUERY_PLAN) &&
         (thd->query_plan_flags &
          (QPLAN_FULL_SCAN | QPLAN_FULL_JOIN | QPLAN_TMP_TABLE |
           QPLAN_TMP_DISK | QPLAN_FILESORT | QPLAN_FILESORT_DISK)) &&
         my_b_printf(&log_file,
                     "# Full_scan: %s  Full_join: %s  "
                     "Tmp_table: %s  Tmp_table_on_disk: %s\n"
                     "# Filesort: %s  Filesort_on_disk: %s  Merge_passes: %lu  "
                     "Priority_queue: %s\n",
                     ((thd->query_plan_flags & QPLAN_FULL_SCAN) ? "Yes" : "No"),
                     ((thd->query_plan_flags & QPLAN_FULL_JOIN) ? "Yes" : "No"),
                     ((thd->query_plan_flags & QPLAN_TMP_TABLE) ? "Yes" : "No"),
                     ((thd->query_plan_flags & QPLAN_TMP_DISK) ? "Yes" : "No"),
                     ((thd->query_plan_flags & QPLAN_FILESORT) ? "Yes" : "No"),
                     ((thd->query_plan_flags & QPLAN_FILESORT_DISK) ?
                      "Yes" : "No"),
                     thd->query_plan_fsort_passes,
                     ((thd->query_plan_flags & QPLAN_FILESORT_PRIORITY_QUEUE) ? 
                       "Yes" : "No")
                     ) == (size_t) -1)
       tmp_errno= errno;
    if (thd->variables.log_slow_verbosity & LOG_SLOW_VERBOSITY_EXPLAIN &&
        thd->lex->explain)
    {
      StringBuffer<128> buf;
      DBUG_ASSERT(!thd->free_list);
      if (!print_explain_for_slow_log(thd->lex, thd, &buf))
        my_b_printf(&log_file, "%s", buf.c_ptr_safe());
      thd->free_items();
    }
    if (thd->db && strcmp(thd->db, db))
    {						// Database changed
      if (my_b_printf(&log_file,"use %s;\n",thd->db) == (size_t) -1)
        tmp_errno= errno;
      strmov(db,thd->db);
    }
    if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
    {
      end=strmov(end, ",last_insert_id=");
      end=longlong10_to_str((longlong)
                            thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                            end, -10);
    }
    // Save value if we do an insert.
    if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
    {
      if (!(specialflag & SPECIAL_SHORT_LOG_FORMAT))
      {
        end=strmov(end,",insert_id=");
        end=longlong10_to_str((longlong)
                              thd->auto_inc_intervals_in_cur_stmt_for_binlog.minimum(),
                              end, -10);
      }
    }

    /*
      This info used to show up randomly, depending on whether the query
      checked the query start time or not. now we always write current
      timestamp to the slow log
    */
    end= strmov(end, ",timestamp=");
    end= int10_to_str((long) current_time, end, 10);

    if (end != buff)
    {
      *end++=';';
      *end='\n';
      if (my_b_write(&log_file, (uchar*) "SET ", 4) ||
          my_b_write(&log_file, (uchar*) buff + 1, (uint) (end-buff)))
        tmp_errno= errno;
    }
    if (is_command)
    {
      end= strxmov(buff, "# administrator command: ", NullS);
      buff_len= (ulong) (end - buff);
      DBUG_EXECUTE_IF("simulate_slow_log_write_error",
                      {DBUG_SET("+d,simulate_file_write_error");});
      if(my_b_write(&log_file, (uchar*) buff, buff_len))
        tmp_errno= errno;
    }
    if (my_b_write(&log_file, (uchar*) sql_text, sql_text_len) ||
        my_b_write(&log_file, (uchar*) ";\n",2) ||
        flush_io_cache(&log_file))
      tmp_errno= errno;
    if (tmp_errno)
    {
      error= 1;
      if (! write_error)
      {
        write_error= 1;
        sql_print_error(ER_THD(thd, ER_ERROR_ON_WRITE), name, error);
      }
    }
  }
  mysql_mutex_unlock(&LOCK_log);
  DBUG_RETURN(error);
}


/**
  @todo
  The following should be using fn_format();  We just need to
  first change fn_format() to cut the file name if it's too long.
*/
const char *MYSQL_LOG::generate_name(const char *log_name,
                                     const char *suffix,
                                     bool strip_ext, char *buff)
{
  if (!log_name || !log_name[0])
  {
    strmake(buff, pidfile_name, FN_REFLEN - strlen(suffix) - 1);
    return (const char *)
      fn_format(buff, buff, "", suffix, MYF(MY_REPLACE_EXT|MY_REPLACE_DIR));
  }
  // get rid of extension if the log is binary to avoid problems
  if (strip_ext)
  {
    char *p= fn_ext(log_name);
    uint length= (uint) (p - log_name);
    strmake(buff, log_name, MY_MIN(length, FN_REFLEN-1));
    return (const char*)buff;
  }
  return log_name;
}



MYSQL_BIN_LOG::MYSQL_BIN_LOG(uint *sync_period)
  :reset_master_pending(0), mark_xid_done_waiting(0),
   bytes_written(0), file_id(1), open_count(1),
   group_commit_queue(0), group_commit_queue_busy(FALSE),
   num_commits(0), num_group_commits(0),
   group_commit_trigger_count(0), group_commit_trigger_timeout(0),
   group_commit_trigger_lock_wait(0),
   sync_period_ptr(sync_period), sync_counter(0),
   state_file_deleted(false), binlog_state_recover_done(false),
   is_relay_log(0), signal_cnt(0),
   checksum_alg_reset(BINLOG_CHECKSUM_ALG_UNDEF),
   relay_log_checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF),
   description_event_for_exec(0), description_event_for_queue(0),
   current_binlog_id(0)
{
  /*
    We don't want to initialize locks here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
  index_file_name[0] = 0;
  bzero((char*) &index_file, sizeof(index_file));
  bzero((char*) &purge_index_file, sizeof(purge_index_file));
}

/* this is called only once */

void MYSQL_BIN_LOG::cleanup()
{
  DBUG_ENTER("cleanup");
  if (inited)
  {
    xid_count_per_binlog *b;

    /* Wait for the binlog background thread to stop. */
    if (!is_relay_log && binlog_background_thread_started)
    {
      mysql_mutex_lock(&LOCK_binlog_background_thread);
      binlog_background_thread_stop= true;
      mysql_cond_signal(&COND_binlog_background_thread);
      while (binlog_background_thread_stop)
        mysql_cond_wait(&COND_binlog_background_thread_end,
                        &LOCK_binlog_background_thread);
      mysql_mutex_unlock(&LOCK_binlog_background_thread);
      binlog_background_thread_started= false;
    }

    inited= 0;
    close(LOG_CLOSE_INDEX|LOG_CLOSE_STOP_EVENT);
    delete description_event_for_queue;
    delete description_event_for_exec;

    while ((b= binlog_xid_count_list.get()))
    {
      /*
        There should be no pending XIDs at shutdown, and only one entry (for
        the active binlog file) in the list.
      */
      DBUG_ASSERT(b->xid_count == 0);
      DBUG_ASSERT(!binlog_xid_count_list.head());
      my_free(b);
    }

    mysql_mutex_destroy(&LOCK_log);
    mysql_mutex_destroy(&LOCK_index);
    mysql_mutex_destroy(&LOCK_xid_list);
    mysql_mutex_destroy(&LOCK_binlog_background_thread);
    mysql_mutex_destroy(&LOCK_binlog_end_pos);
    mysql_cond_destroy(&update_cond);
    mysql_cond_destroy(&COND_queue_busy);
    mysql_cond_destroy(&COND_xid_list);
    mysql_cond_destroy(&COND_binlog_background_thread);
    mysql_cond_destroy(&COND_binlog_background_thread_end);
  }

  /*
    Free data for global binlog state.
    We can't do that automaticly as we need to do this before
    safemalloc is shut down
  */
  if (!is_relay_log)
    rpl_global_gtid_binlog_state.free();
  DBUG_VOID_RETURN;
}


/* Init binlog-specific vars */
void MYSQL_BIN_LOG::init(ulong max_size_arg)
{
  DBUG_ENTER("MYSQL_BIN_LOG::init");
  max_size= max_size_arg;
  DBUG_PRINT("info",("max_size: %lu", max_size));
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::init_pthread_objects()
{
  MYSQL_LOG::init_pthread_objects();
  mysql_mutex_init(m_key_LOCK_index, &LOCK_index, MY_MUTEX_INIT_SLOW);
  mysql_mutex_setflags(&LOCK_index, MYF_NO_DEADLOCK_DETECTION);
  mysql_mutex_init(key_BINLOG_LOCK_xid_list,
                   &LOCK_xid_list, MY_MUTEX_INIT_FAST);
  mysql_cond_init(m_key_update_cond, &update_cond, 0);
  mysql_cond_init(m_key_COND_queue_busy, &COND_queue_busy, 0);
  mysql_cond_init(key_BINLOG_COND_xid_list, &COND_xid_list, 0);

  mysql_mutex_init(key_BINLOG_LOCK_binlog_background_thread,
                   &LOCK_binlog_background_thread, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_BINLOG_COND_binlog_background_thread,
                  &COND_binlog_background_thread, 0);
  mysql_cond_init(key_BINLOG_COND_binlog_background_thread_end,
                  &COND_binlog_background_thread_end, 0);

  mysql_mutex_init(m_key_LOCK_binlog_end_pos, &LOCK_binlog_end_pos,
                   MY_MUTEX_INIT_SLOW);
}


bool MYSQL_BIN_LOG::open_index_file(const char *index_file_name_arg,
                                    const char *log_name, bool need_mutex)
{
  File index_file_nr= -1;
  DBUG_ASSERT(!my_b_inited(&index_file));

  /*
    First open of this class instance
    Create an index file that will hold all file names uses for logging.
    Add new entries to the end of it.
  */
  myf opt= MY_UNPACK_FILENAME;
  if (!index_file_name_arg)
  {
    index_file_name_arg= log_name;    // Use same basename for index file
    opt= MY_UNPACK_FILENAME | MY_REPLACE_EXT;
  }
  fn_format(index_file_name, index_file_name_arg, mysql_data_home,
            ".index", opt);
  if ((index_file_nr= mysql_file_open(m_key_file_log_index,
                                      index_file_name,
                                      O_RDWR | O_CREAT | O_BINARY,
                                      MYF(MY_WME))) < 0 ||
       mysql_file_sync(index_file_nr, MYF(MY_WME)) ||
       init_io_cache(&index_file, index_file_nr,
                     IO_SIZE, WRITE_CACHE,
                     mysql_file_seek(index_file_nr, 0L, MY_SEEK_END, MYF(0)),
                                     0, MYF(MY_WME | MY_WAIT_IF_FULL)) ||
      DBUG_EVALUATE_IF("fault_injection_openning_index", 1, 0))
  {
    /*
      TODO: all operations creating/deleting the index file or a log, should
      call my_sync_dir() or my_sync_dir_by_file() to be durable.
      TODO: file creation should be done with mysql_file_create()
      not mysql_file_open().
    */
    if (index_file_nr >= 0)
      mysql_file_close(index_file_nr, MYF(0));
    return TRUE;
  }

#ifdef HAVE_REPLICATION
  /*
    Sync the index by purging any binary log file that is not registered.
    In other words, either purge binary log files that were removed from
    the index but not purged from the file system due to a crash or purge
    any binary log file that was created but not register in the index
    due to a crash.
  */

  if (set_purge_index_file_name(index_file_name_arg) ||
      open_purge_index_file(FALSE) ||
      purge_index_entry(NULL, NULL, need_mutex) ||
      close_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_recovering_index", 1, 0))
  {
    sql_print_error("MYSQL_BIN_LOG::open_index_file failed to sync the index "
                    "file.");
    return TRUE;
  }
#endif

  return FALSE;
}


/**
  Open a (new) binlog file.

  - Open the log file and the index file. Register the new
  file name in it
  - When calling this when the file is in use, you must have a locks
  on LOCK_log and LOCK_index.

  @retval
    0	ok
  @retval
    1	error
*/

bool MYSQL_BIN_LOG::open(const char *log_name,
                         enum_log_type log_type_arg,
                         const char *new_name,
                         ulong next_log_number,
                         enum cache_type io_cache_type_arg,
                         ulong max_size_arg,
                         bool null_created_arg,
                         bool need_mutex)
{
  File file= -1;
  xid_count_per_binlog *new_xid_list_entry= NULL, *b;
  DBUG_ENTER("MYSQL_BIN_LOG::open");
  DBUG_PRINT("enter",("log_type: %d",(int) log_type_arg));

  if (!is_relay_log)
  {
    if (!binlog_state_recover_done)
    {
      binlog_state_recover_done= true;
      if (do_binlog_recovery(opt_bin_logname, false))
        DBUG_RETURN(1);
    }

    if (!binlog_background_thread_started &&
        start_binlog_background_thread())
      DBUG_RETURN(1);
  }

  /* We need to calculate new log file name for purge to delete old */
  if (init_and_set_log_file_name(log_name, new_name, next_log_number,
                                 log_type_arg, io_cache_type_arg))
  {
    sql_print_error("MSYQL_BIN_LOG::open failed to generate new file name.");
    DBUG_RETURN(1);
  }

#ifdef HAVE_REPLICATION
  if (open_purge_index_file(TRUE) ||
      register_create_index_entry(log_file_name) ||
      sync_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_registering_index", 1, 0))
  {
    /**
        TODO:
        Although this was introduced to appease valgrind when
        injecting emulated faults using
        fault_injection_registering_index it may be good to consider
        what actually happens when open_purge_index_file succeeds but
        register or sync fails.

        Perhaps we might need the code below in MYSQL_LOG_BIN::cleanup
        for "real life" purposes as well? 
     */
    DBUG_EXECUTE_IF("fault_injection_registering_index", {
      if (my_b_inited(&purge_index_file))
      {
        end_io_cache(&purge_index_file);
        my_close(purge_index_file.file, MYF(0));
      }
    });

    sql_print_error("MSYQL_BIN_LOG::open failed to sync the index file.");
    DBUG_RETURN(1);
  }
  DBUG_EXECUTE_IF("crash_create_non_critical_before_update_index", DBUG_SUICIDE(););
#endif

  write_error= 0;

  /* open the main log file */
  if (MYSQL_LOG::open(
#ifdef HAVE_PSI_INTERFACE
                      m_key_file_log,
#endif
                      log_name,
                      LOG_UNKNOWN, /* Don't generate new name */
                      0, 0, io_cache_type_arg))
  {
#ifdef HAVE_REPLICATION
    close_purge_index_file();
#endif
    DBUG_RETURN(1);                            /* all warnings issued */
  }

  init(max_size_arg);

  open_count++;

  DBUG_ASSERT(log_type == LOG_BIN);

  {
    bool write_file_name_to_index_file=0;

    if (!my_b_filelength(&log_file))
    {
      /*
	The binary log file was empty (probably newly created)
	This is the normal case and happens when the user doesn't specify
	an extension for the binary log files.
	In this case we write a standard header to it.
      */
      if (my_b_safe_write(&log_file, BINLOG_MAGIC,
			  BIN_LOG_HEADER_SIZE))
        goto err;
      bytes_written+= BIN_LOG_HEADER_SIZE;
      write_file_name_to_index_file= 1;
    }

    {
      /*
        In 4.x we put Start event only in the first binlog. But from 5.0 we
        want a Start event even if this is not the very first binlog.
      */
      Format_description_log_event s(BINLOG_VERSION);
      /*
        don't set LOG_EVENT_BINLOG_IN_USE_F for SEQ_READ_APPEND io_cache
        as we won't be able to reset it later
      */
      if (io_cache_type == WRITE_CACHE)
        s.flags |= LOG_EVENT_BINLOG_IN_USE_F;

      if (is_relay_log)
      {
        if (relay_log_checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF)
          relay_log_checksum_alg=
            opt_slave_sql_verify_checksum ? (enum_binlog_checksum_alg) binlog_checksum_options
                                          : BINLOG_CHECKSUM_ALG_OFF;
        s.checksum_alg= relay_log_checksum_alg;
      }
      else
        s.checksum_alg= (enum_binlog_checksum_alg)binlog_checksum_options;

      crypto.scheme = 0;
      DBUG_ASSERT(s.checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
      if (!s.is_valid())
        goto err;
      s.dont_set_created= null_created_arg;
      if (write_event(&s))
        goto err;
      bytes_written+= s.data_written;

      if (encrypt_binlog)
      {
        uint key_version= encryption_key_get_latest_version(ENCRYPTION_KEY_SYSTEM_DATA);
        if (key_version == ENCRYPTION_KEY_VERSION_INVALID)
        {
          sql_print_error("Failed to enable encryption of binary logs");
          goto err;
        }

        if (key_version != ENCRYPTION_KEY_NOT_ENCRYPTED)
        {
          if (my_random_bytes(crypto.nonce, sizeof(crypto.nonce)))
            goto err;

          Start_encryption_log_event sele(1, key_version, crypto.nonce);
          sele.checksum_alg= s.checksum_alg;
          if (write_event(&sele))
            goto err;

          // Start_encryption_log_event is written, enable the encryption
          if (crypto.init(sele.crypto_scheme, key_version))
            goto err;
        }
      }

      if (!is_relay_log)
      {
        char buf[FN_REFLEN];

        /*
          Output a Gtid_list_log_event at the start of the binlog file.

          This is used to quickly determine which GTIDs are found in binlog
          files earlier than this one, and which are found in this (or later)
          binlogs.

          The list gives a mapping from (domain_id, server_id) -> seq_no (so
          this means that there is at most one entry for every unique pair
          (domain_id, server_id) in the list). It indicates that this seq_no is
          the last one found in an earlier binlog file for this (domain_id,
          server_id) combination - so any higher seq_no should be search for
          from this binlog file, or a later one.

          This allows to locate the binlog file containing a given GTID by
          scanning backwards, reading just the Gtid_list_log_event at the
          start of each file, and scanning only the relevant binlog file when
          found, not all binlog files.

          The existence of a given entry (domain_id, server_id, seq_no)
          guarantees only that this seq_no will not be found in this or any
          later binlog file. It does not guarantee that it can be found it an
          earlier binlog file, for example the file may have been purged.

          If there is no entry for a given (domain_id, server_id) pair, then
          it means that no such GTID exists in any earlier binlog. It is
          permissible to remove such pair from future Gtid_list_log_events
          if all previous binlog files containing such GTIDs have been purged
          (though such optimization is not performed at the time of this
          writing). So if there is no entry for given GTID it means that such
          GTID should be search for in this or later binlog file, same as if
          there had been an entry (domain_id, server_id, 0).
        */

        Gtid_list_log_event gl_ev(&rpl_global_gtid_binlog_state, 0);
        if (write_event(&gl_ev))
          goto err;

        /* Output a binlog checkpoint event at the start of the binlog file. */

        /*
          Construct an entry in the binlog_xid_count_list for the new binlog
          file (we will not link it into the list until we know the new file
          is successfully created; otherwise we would have to remove it again
          if creation failed, which gets tricky since other threads may have
          seen the entry in the meantime - and we do not want to hold
          LOCK_xid_list for long periods of time).

          Write the current binlog checkpoint into the log, so XA recovery will
          know from where to start recovery.
        */
        uint off= dirname_length(log_file_name);
        uint len= strlen(log_file_name) - off;
        char *entry_mem, *name_mem;
        if (!(new_xid_list_entry = (xid_count_per_binlog *)
              my_multi_malloc(MYF(MY_WME),
                              &entry_mem, sizeof(xid_count_per_binlog),
                              &name_mem, len,
                              NULL)))
          goto err;
        memcpy(name_mem, log_file_name+off, len);
        new_xid_list_entry->binlog_name= name_mem;
        new_xid_list_entry->binlog_name_len= len;
        new_xid_list_entry->xid_count= 0;

        /*
          Find the name for the Initial binlog checkpoint.

          Normally this will just be the first entry, as we delete entries
          when their count drops to zero. But we scan the list to handle any
          corner case, eg. for the first binlog file opened after startup, the
          list will be empty.
        */
        mysql_mutex_lock(&LOCK_xid_list);
        I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
        while ((b= it++) && b->xid_count == 0)
          ;
        mysql_mutex_unlock(&LOCK_xid_list);
        if (!b)
          b= new_xid_list_entry;
        strmake(buf, b->binlog_name, b->binlog_name_len);
        Binlog_checkpoint_log_event ev(buf, len);
        DBUG_EXECUTE_IF("crash_before_write_checkpoint_event",
                        flush_io_cache(&log_file);
                        mysql_file_sync(log_file.file, MYF(MY_WME));
                        DBUG_SUICIDE(););
        if (write_event(&ev))
          goto err;
        bytes_written+= ev.data_written;
      }
    }
    if (description_event_for_queue &&
        description_event_for_queue->binlog_version>=4)
    {
      /*
        This is a relay log written to by the I/O slave thread.
        Write the event so that others can later know the format of this relay
        log.
        Note that this event is very close to the original event from the
        master (it has binlog version of the master, event types of the
        master), so this is suitable to parse the next relay log's event. It
        has been produced by
        Format_description_log_event::Format_description_log_event(char* buf,).
        Why don't we want to write the description_event_for_queue if this
        event is for format<4 (3.23 or 4.x): this is because in that case, the
        description_event_for_queue describes the data received from the
        master, but not the data written to the relay log (*conversion*),
        which is in format 4 (slave's).
      */
      /*
        Set 'created' to 0, so that in next relay logs this event does not
        trigger cleaning actions on the slave in
        Format_description_log_event::apply_event_impl().
      */
      description_event_for_queue->created= 0;
      /* Don't set log_pos in event header */
      description_event_for_queue->set_artificial_event();

      if (write_event(description_event_for_queue))
        goto err;
      bytes_written+= description_event_for_queue->data_written;
    }
    if (flush_io_cache(&log_file) ||
        mysql_file_sync(log_file.file, MYF(MY_WME|MY_SYNC_FILESIZE)))
      goto err;

    my_off_t offset= my_b_tell(&log_file);

    if (!is_relay_log)
    {
      /* update binlog_end_pos so that it can be read by after sync hook */
      reset_binlog_end_pos(log_file_name, offset);

      mysql_mutex_lock(&LOCK_commit_ordered);
      strmake_buf(last_commit_pos_file, log_file_name);
      last_commit_pos_offset= offset;
      mysql_mutex_unlock(&LOCK_commit_ordered);
    }

    if (write_file_name_to_index_file)
    {
#ifdef HAVE_REPLICATION
#ifdef ENABLED_DEBUG_SYNC
      if (current_thd)
        DEBUG_SYNC(current_thd, "binlog_open_before_update_index");
#endif
      DBUG_EXECUTE_IF("crash_create_critical_before_update_index", DBUG_SUICIDE(););
#endif

      DBUG_ASSERT(my_b_inited(&index_file) != 0);
      reinit_io_cache(&index_file, WRITE_CACHE,
                      my_b_filelength(&index_file), 0, 0);
      /*
        As this is a new log file, we write the file name to the index
        file. As every time we write to the index file, we sync it.
      */
      if (DBUG_EVALUATE_IF("fault_injection_updating_index", 1, 0) ||
          my_b_write(&index_file, (uchar*) log_file_name,
                     strlen(log_file_name)) ||
          my_b_write(&index_file, (uchar*) "\n", 1) ||
          flush_io_cache(&index_file) ||
          mysql_file_sync(index_file.file, MYF(MY_WME|MY_SYNC_FILESIZE)))
        goto err;

#ifdef HAVE_REPLICATION
      DBUG_EXECUTE_IF("crash_create_after_update_index", DBUG_SUICIDE(););
#endif
    }
  }

  if (!is_relay_log)
  {
    /*
      Now the file was created successfully, so we can link in the entry for
      the new binlog file in binlog_xid_count_list.
    */
    mysql_mutex_lock(&LOCK_xid_list);
    ++current_binlog_id;
    new_xid_list_entry->binlog_id= current_binlog_id;
    /* Remove any initial entries with no pending XIDs.  */
    while ((b= binlog_xid_count_list.head()) && b->xid_count == 0)
      my_free(binlog_xid_count_list.get());
    binlog_xid_count_list.push_back(new_xid_list_entry);
    mysql_mutex_unlock(&LOCK_xid_list);

    /*
      Now that we have synced a new binlog file with an initial Gtid_list
      event, it is safe to delete the binlog state file. We will write out
      a new, updated file at shutdown, and if we crash before we can recover
      the state from the newly written binlog file.

      Since the state file will contain out-of-date data as soon as the first
      new GTID is binlogged, it is better to remove it, to avoid any risk of
      accidentally reading incorrect data later.
    */
    if (!state_file_deleted)
    {
      char buf[FN_REFLEN];
      fn_format(buf, opt_bin_logname, mysql_data_home, ".state",
                MY_UNPACK_FILENAME);
      my_delete(buf, MY_SYNC_DIR);
      state_file_deleted= true;
    }
  }

  log_state= LOG_OPENED;

#ifdef HAVE_REPLICATION
  close_purge_index_file();
#endif

  DBUG_RETURN(0);

err:
#ifdef HAVE_REPLICATION
  if (is_inited_purge_index_file())
    purge_index_entry(NULL, NULL, need_mutex);
  close_purge_index_file();
#endif
  sql_print_error("Could not use %s for logging (error %d). \
Turning logging off for the whole duration of the MySQL server process. \
To turn it on again: fix the cause, \
shutdown the MySQL server and restart it.", name, errno);
  if (new_xid_list_entry)
    my_free(new_xid_list_entry);
  if (file >= 0)
    mysql_file_close(file, MYF(0));
  close(LOG_CLOSE_INDEX);
  DBUG_RETURN(1);
}


int MYSQL_BIN_LOG::get_current_log(LOG_INFO* linfo)
{
  mysql_mutex_lock(&LOCK_log);
  int ret = raw_get_current_log(linfo);
  mysql_mutex_unlock(&LOCK_log);
  return ret;
}

int MYSQL_BIN_LOG::raw_get_current_log(LOG_INFO* linfo)
{
  mysql_mutex_assert_owner(&LOCK_log);
  strmake_buf(linfo->log_file_name, log_file_name);
  linfo->pos = my_b_tell(&log_file);
  return 0;
}

/**
  Move all data up in a file in an filename index file.

    We do the copy outside of the IO_CACHE as the cache buffers would just
    make things slower and more complicated.
    In most cases the copy loop should only do one read.

  @param index_file			File to move
  @param offset			Move everything from here to beginning

  @note
    File will be truncated to be 'offset' shorter or filled up with newlines

  @retval
    0	ok
*/

#ifdef HAVE_REPLICATION

static bool copy_up_file_and_fill(IO_CACHE *index_file, my_off_t offset)
{
  int bytes_read;
  my_off_t init_offset= offset;
  File file= index_file->file;
  uchar io_buf[IO_SIZE*2];
  DBUG_ENTER("copy_up_file_and_fill");

  for (;; offset+= bytes_read)
  {
    mysql_file_seek(file, offset, MY_SEEK_SET, MYF(0));
    if ((bytes_read= (int) mysql_file_read(file, io_buf, sizeof(io_buf),
                                           MYF(MY_WME)))
	< 0)
      goto err;
    if (!bytes_read)
      break;					// end of file
    mysql_file_seek(file, offset-init_offset, MY_SEEK_SET, MYF(0));
    if (mysql_file_write(file, io_buf, bytes_read,
                         MYF(MY_WME | MY_NABP | MY_WAIT_IF_FULL)))
      goto err;
  }
  /* The following will either truncate the file or fill the end with \n' */
  if (mysql_file_chsize(file, offset - init_offset, '\n', MYF(MY_WME)) ||
      mysql_file_sync(file, MYF(MY_WME|MY_SYNC_FILESIZE)))
    goto err;

  /* Reset data in old index cache */
  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 1);
  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}

#endif /* HAVE_REPLICATION */

/**
  Find the position in the log-index-file for the given log name.

  @param linfo		Store here the found log file name and position to
                       the NEXT log file name in the index file.
  @param log_name	Filename to find in the index file.
                       Is a null pointer if we want to read the first entry
  @param need_lock	Set this to 1 if the parent doesn't already have a
                       lock on LOCK_index

  @note
    On systems without the truncate function the file will end with one or
    more empty lines.  These will be ignored when reading the file.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

int MYSQL_BIN_LOG::find_log_pos(LOG_INFO *linfo, const char *log_name,
			    bool need_lock)
{
  int error= 0;
  char *full_fname= linfo->log_file_name;
  char full_log_name[FN_REFLEN], fname[FN_REFLEN];
  uint log_name_len= 0, fname_len= 0;
  DBUG_ENTER("find_log_pos");
  full_log_name[0]= full_fname[0]= 0;

  /*
    Mutex needed because we need to make sure the file pointer does not
    move from under our feet
  */
  if (need_lock)
    mysql_mutex_lock(&LOCK_index);
  mysql_mutex_assert_owner(&LOCK_index);

  // extend relative paths for log_name to be searched
  if (log_name)
  {
    if(normalize_binlog_name(full_log_name, log_name, is_relay_log))
    {
      error= LOG_INFO_EOF;
      goto end;
    }
  }

  log_name_len= log_name ? (uint) strlen(full_log_name) : 0;
  DBUG_PRINT("enter", ("log_name: %s, full_log_name: %s", 
                       log_name ? log_name : "NULL", full_log_name));

  /* As the file is flushed, we can't get an error here */
  (void) reinit_io_cache(&index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  for (;;)
  {
    uint length;
    my_off_t offset= my_b_tell(&index_file);

    DBUG_EXECUTE_IF("simulate_find_log_pos_error",
                    error=  LOG_INFO_EOF; break;);
    /* If we get 0 or 1 characters, this is the end of the file */
    if ((length= my_b_gets(&index_file, fname, FN_REFLEN)) <= 1)
    {
      /* Did not find the given entry; Return not found or error */
      error= !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
      break;
    }
    if (fname[length-1] != '\n')
      continue;                                 // Not a log entry
    fname[length-1]= 0;                         // Remove end \n
    
    // extend relative paths and match against full path
    if (normalize_binlog_name(full_fname, fname, is_relay_log))
    {
      error= LOG_INFO_EOF;
      break;
    }
    fname_len= (uint) strlen(full_fname);

    // if the log entry matches, null string matching anything
    if (!log_name ||
        (log_name_len == fname_len &&
	 !memcmp(full_fname, full_log_name, log_name_len)))
    {
      DBUG_PRINT("info", ("Found log file entry"));
      linfo->index_file_start_offset= offset;
      linfo->index_file_offset = my_b_tell(&index_file);
      break;
    }
  }

end:
  if (need_lock)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


/**
  Find the position in the log-index-file for the given log name.

  @param
    linfo		Store here the next log file name and position to
			the file name after that.
  @param
    need_lock		Set this to 1 if the parent doesn't already have a
			lock on LOCK_index

  @note
    - Before calling this function, one has to call find_log_pos()
    to set up 'linfo'
    - Mutex needed because we need to make sure the file pointer does not move
    from under our feet

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

int MYSQL_BIN_LOG::find_next_log(LOG_INFO* linfo, bool need_lock)
{
  int error= 0;
  uint length;
  char fname[FN_REFLEN];
  char *full_fname= linfo->log_file_name;

  if (need_lock)
    mysql_mutex_lock(&LOCK_index);
  mysql_mutex_assert_owner(&LOCK_index);

  /* As the file is flushed, we can't get an error here */
  (void) reinit_io_cache(&index_file, READ_CACHE, linfo->index_file_offset, 0,
			 0);

  linfo->index_file_start_offset= linfo->index_file_offset;
  if ((length=my_b_gets(&index_file, fname, FN_REFLEN)) <= 1)
  {
    error = !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
    goto err;
  }

  if (fname[0] != 0)
  {
    if(normalize_binlog_name(full_fname, fname, is_relay_log))
    {
      error= LOG_INFO_EOF;
      goto err;
    }
    length= strlen(full_fname);
  }

  full_fname[length-1]= 0;			// kill \n
  linfo->index_file_offset= my_b_tell(&index_file);

err:
  if (need_lock)
    mysql_mutex_unlock(&LOCK_index);
  return error;
}


/**
  Delete all logs refered to in the index file.

  The new index file will only contain this file.

  @param thd		  Thread id. This can be zero in case of resetting 
                          relay logs
  @param create_new_log   1 if we should start writing to a new log file
  @param next_log_number  min number of next log file to use, if possible.

  @note
    If not called from slave thread, write start event to new log

  @retval
    0	ok
  @retval
    1   error
*/

bool MYSQL_BIN_LOG::reset_logs(THD *thd, bool create_new_log,
                               rpl_gtid *init_state, uint32 init_state_len,
                               ulong next_log_number)
{
  LOG_INFO linfo;
  bool error=0;
  int err;
  const char* save_name;
  DBUG_ENTER("reset_logs");

  if (!is_relay_log)
  {
    if (init_state && !is_empty_state())
    {
      my_error(ER_BINLOG_MUST_BE_EMPTY, MYF(0));
      DBUG_RETURN(1);
    }

    /*
      Mark that a RESET MASTER is in progress.
      This ensures that a binlog checkpoint will not try to write binlog
      checkpoint events, which would be useless (as we are deleting the binlog
      anyway) and could deadlock, as we are holding LOCK_log.

      Wait for any mark_xid_done() calls that might be already running to
      complete (mark_xid_done_waiting counter to drop to zero); we need to
      do this before we take the LOCK_log to not deadlock.
    */
    mysql_mutex_lock(&LOCK_xid_list);
    reset_master_pending++;
    while (mark_xid_done_waiting > 0)
      mysql_cond_wait(&COND_xid_list, &LOCK_xid_list);
    mysql_mutex_unlock(&LOCK_xid_list);
  }

  DEBUG_SYNC_C_IF_THD(thd, "reset_logs_after_set_reset_master_pending");
  /*
    We need to get both locks to be sure that no one is trying to
    write to the index log file.
  */
  mysql_mutex_lock(&LOCK_log);
  mysql_mutex_lock(&LOCK_index);

  if (!is_relay_log)
  {
    /*
      We are going to nuke all binary log files.
      Without binlog, we cannot XA recover prepared-but-not-committed
      transactions in engines. So force a commit checkpoint first.

      Note that we take and immediately
      release LOCK_after_binlog_sync/LOCK_commit_ordered. This has
      the effect to ensure that any on-going group commit (in
      trx_group_commit_leader()) has completed before we request the checkpoint,
      due to the chaining of LOCK_log and LOCK_commit_ordered in that function.
      (We are holding LOCK_log, so no new group commit can start).

      Without this, it is possible (though perhaps unlikely) that the RESET
      MASTER could run in-between the write to the binlog and the
      commit_ordered() in the engine of some transaction, and then a crash
      later would leave such transaction not recoverable.
    */

    mysql_mutex_lock(&LOCK_after_binlog_sync);
    mysql_mutex_lock(&LOCK_commit_ordered);
    mysql_mutex_unlock(&LOCK_after_binlog_sync);
    mysql_mutex_unlock(&LOCK_commit_ordered);

    mark_xids_active(current_binlog_id, 1);
    do_checkpoint_request(current_binlog_id);

    /* Now wait for all checkpoint requests and pending unlog() to complete. */
    mysql_mutex_lock(&LOCK_xid_list);
    for (;;)
    {
      if (is_xidlist_idle_nolock())
        break;
      /*
        Wait until signalled that one more binlog dropped to zero, then check
        again.
      */
      mysql_cond_wait(&COND_xid_list, &LOCK_xid_list);
    }

    /*
      Now all XIDs are fully flushed to disk, and we are holding LOCK_log so
      no new ones will be written. So we can proceed to delete the logs.
    */
    mysql_mutex_unlock(&LOCK_xid_list);
  }

  /*
    The following mutex is needed to ensure that no threads call
    'delete thd' as we would then risk missing a 'rollback' from this
    thread. If the transaction involved MyISAM tables, it should go
    into binlog even on rollback.
  */
  mysql_mutex_lock(&LOCK_thread_count);

  /* Save variables so that we can reopen the log */
  save_name=name;
  name=0;					// Protect against free
  close(LOG_CLOSE_TO_BE_OPENED);

  /*
    First delete all old log files and then update the index file.
    As we first delete the log files and do not use sort of logging,
    a crash may lead to an inconsistent state where the index has
    references to non-existent files.

    We need to invert the steps and use the purge_index_file methods
    in order to make the operation safe.
  */

  if ((err= find_log_pos(&linfo, NullS, 0)) != 0)
  {
    uint errcode= purge_log_get_error_code(err);
    sql_print_error("Failed to locate old binlog or relay log files");
    my_message(errcode, ER_THD_OR_DEFAULT(thd, errcode), MYF(0));
    error= 1;
    goto err;
  }

  for (;;)
  {
    if ((error= my_delete(linfo.log_file_name, MYF(0))) != 0)
    {
      if (my_errno == ENOENT) 
      {
        if (thd)
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_LOG_PURGE_NO_FILE,
                              ER_THD(thd, ER_LOG_PURGE_NO_FILE),
                              linfo.log_file_name);

        sql_print_information("Failed to delete file '%s'",
                              linfo.log_file_name);
        my_errno= 0;
        error= 0;
      }
      else
      {
        if (thd)
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with deleting %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              linfo.log_file_name);
        error= 1;
        goto err;
      }
    }
    if (find_next_log(&linfo, 0))
      break;
  }

  if (!is_relay_log)
  {
    if (init_state)
      rpl_global_gtid_binlog_state.load(init_state, init_state_len);
    else
      rpl_global_gtid_binlog_state.reset();
  }

  /* Start logging with a new file */
  close(LOG_CLOSE_INDEX | LOG_CLOSE_TO_BE_OPENED);
  if ((error= my_delete(index_file_name, MYF(0))))	// Reset (open will update)
  {
    if (my_errno == ENOENT) 
    {
      if (thd)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_LOG_PURGE_NO_FILE,
                            ER_THD(thd, ER_LOG_PURGE_NO_FILE),
                            index_file_name);
      sql_print_information("Failed to delete file '%s'",
                            index_file_name);
      my_errno= 0;
      error= 0;
    }
    else
    {
      if (thd)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_BINLOG_PURGE_FATAL_ERR,
                            "a problem with deleting %s; "
                            "consider examining correspondence "
                            "of your binlog index file "
                            "to the actual binlog files",
                            index_file_name);
      error= 1;
      goto err;
    }
  }
  if (create_new_log && !open_index_file(index_file_name, 0, FALSE))
    if ((error= open(save_name, log_type, 0, next_log_number,
                     io_cache_type, max_size, 0, FALSE)))
      goto err;
  my_free((void *) save_name);

err:
  if (error == 1)
    name= const_cast<char*>(save_name);
  mysql_mutex_unlock(&LOCK_thread_count);

  if (!is_relay_log)
  {
    xid_count_per_binlog *b;
    /*
      Remove all entries in the xid_count list except the last.
      Normally we will just be deleting all the entries that we waited for to
      drop to zero above. But if we fail during RESET MASTER for some reason
      then we will not have created any new log file, and we may keep the last
      of the old entries.
    */
    mysql_mutex_lock(&LOCK_xid_list);
    for (;;)
    {
      b= binlog_xid_count_list.head();
      DBUG_ASSERT(b /* List can never become empty. */);
      if (b->binlog_id == current_binlog_id)
        break;
      DBUG_ASSERT(b->xid_count == 0);
      my_free(binlog_xid_count_list.get());
    }
    reset_master_pending--;
    mysql_mutex_unlock(&LOCK_xid_list);
  }

  mysql_mutex_unlock(&LOCK_index);
  mysql_mutex_unlock(&LOCK_log);
  DBUG_RETURN(error);
}


/**
  Delete relay log files prior to rli->group_relay_log_name
  (i.e. all logs which are not involved in a non-finished group
  (transaction)), remove them from the index file and start on next
  relay log.

  IMPLEMENTATION
  - Protects index file with LOCK_index
  - Delete relevant relay log files
  - Copy all file names after these ones to the front of the index file
  - If the OS has truncate, truncate the file, else fill it with \n'
  - Read the next file name from the index file and store in rli->linfo

  @param rli	       Relay log information
  @param included     If false, all relay logs that are strictly before
                      rli->group_relay_log_name are deleted ; if true, the
                      latter is deleted too (i.e. all relay logs
                      read by the SQL slave thread are deleted).

  @note
    - This is only called from the slave-execute thread when it has read
    all commands from a relay log and want to switch to a new relay log.
    - When this happens, we can be in an active transaction as
    a transaction can span over two relay logs
    (although it is always written as a single block to the master's binary
    log, hence cannot span over two master's binary logs).

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_SEEK	Could not allocate IO cache
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

#ifdef HAVE_REPLICATION

int MYSQL_BIN_LOG::purge_first_log(Relay_log_info* rli, bool included)
{
  int error;
  char *to_purge_if_included= NULL;
  inuse_relaylog *ir;
  ulonglong log_space_reclaimed= 0;
  DBUG_ENTER("purge_first_log");

  DBUG_ASSERT(is_open());
  DBUG_ASSERT(rli->slave_running == MYSQL_SLAVE_RUN_NOT_CONNECT);
  DBUG_ASSERT(!strcmp(rli->linfo.log_file_name,rli->event_relay_log_name));

  mysql_mutex_lock(&LOCK_index);

  ir= rli->inuse_relaylog_list;
  while (ir)
  {
    inuse_relaylog *next= ir->next;
    if (!ir->completed || ir->dequeued_count < ir->queued_count)
    {
      included= false;
      break;
    }
    if (!included && !strcmp(ir->name, rli->group_relay_log_name))
      break;
    if (!next)
    {
      rli->last_inuse_relaylog= NULL;
      included= 1;
      to_purge_if_included= my_strdup(ir->name, MYF(0));
    }
    rli->free_inuse_relaylog(ir);
    ir= next;
  }
  rli->inuse_relaylog_list= ir;
  if (ir)
    to_purge_if_included= my_strdup(ir->name, MYF(0));

  /*
    Read the next log file name from the index file and pass it back to
    the caller.
  */
  if((error=find_log_pos(&rli->linfo, rli->event_relay_log_name, 0)) || 
     (error=find_next_log(&rli->linfo, 0)))
  {
    sql_print_error("next log error: %d  offset: %llu  log: %s included: %d",
                    error, rli->linfo.index_file_offset,
                    rli->event_relay_log_name, included);
    goto err;
  }

  /*
    Reset rli's coordinates to the current log.
  */
  rli->event_relay_log_pos= BIN_LOG_HEADER_SIZE;
  strmake_buf(rli->event_relay_log_name,rli->linfo.log_file_name);

  /*
    If we removed the rli->group_relay_log_name file,
    we must update the rli->group* coordinates, otherwise do not touch it as the
    group's execution is not finished (e.g. COMMIT not executed)
  */
  if (included)
  {
    rli->group_relay_log_pos = BIN_LOG_HEADER_SIZE;
    strmake_buf(rli->group_relay_log_name,rli->linfo.log_file_name);
    rli->notify_group_relay_log_name_update();
  }

  /* Store where we are in the new file for the execution thread */
  flush_relay_log_info(rli);

  DBUG_EXECUTE_IF("crash_before_purge_logs", DBUG_SUICIDE(););

  rli->relay_log.purge_logs(to_purge_if_included, included,
                            0, 0, &log_space_reclaimed);

  mysql_mutex_lock(&rli->log_space_lock);
  rli->log_space_total-= log_space_reclaimed;
  mysql_cond_broadcast(&rli->log_space_cond);
  mysql_mutex_unlock(&rli->log_space_lock);

  /*
   * Need to update the log pos because purge logs has been called 
   * after fetching initially the log pos at the begining of the method.
   */
  if((error=find_log_pos(&rli->linfo, rli->event_relay_log_name, 0)))
  {
    sql_print_error("next log error: %d  offset: %llu  log: %s included: %d",
                    error, rli->linfo.index_file_offset,
                    rli->group_relay_log_name, included);
    goto err;
  }

  /* If included was passed, rli->linfo should be the first entry. */
  DBUG_ASSERT(!included || rli->linfo.index_file_start_offset == 0);

err:
  my_free(to_purge_if_included);
  mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}

/**
  Update log index_file.
*/

int MYSQL_BIN_LOG::update_log_index(LOG_INFO* log_info, bool need_update_threads)
{
  if (copy_up_file_and_fill(&index_file, log_info->index_file_start_offset))
    return LOG_INFO_IO;

  // now update offsets in index file for running threads
  if (need_update_threads)
    adjust_linfo_offsets(log_info->index_file_start_offset);
  return 0;
}

/**
  Remove all logs before the given log from disk and from the index file.

  @param to_log	      Delete all log file name before this file.
  @param included            If true, to_log is deleted too.
  @param need_mutex
  @param need_update_threads If we want to update the log coordinates of
                             all threads. False for relay logs, true otherwise.
  @param reclaimeed_log_space If not null, increment this variable to
                              the amount of log space freed

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF		to_log not found
    LOG_INFO_EMFILE             too many files opened
    LOG_INFO_FATAL              if any other than ENOENT error from
                                mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs(const char *to_log, 
                              bool included,
                              bool need_mutex, 
                              bool need_update_threads, 
                              ulonglong *reclaimed_space)
{
  int error= 0;
  bool exit_loop= 0;
  LOG_INFO log_info;
  THD *thd= current_thd;
  DBUG_ENTER("purge_logs");
  DBUG_PRINT("info",("to_log= %s",to_log));

  if (need_mutex)
    mysql_mutex_lock(&LOCK_index);
  if ((error=find_log_pos(&log_info, to_log, 0 /*no mutex*/))) 
  {
    sql_print_error("MYSQL_BIN_LOG::purge_logs was called with file %s not "
                    "listed in the index.", to_log);
    goto err;
  }

  if ((error= open_purge_index_file(TRUE)))
  {
    sql_print_error("MYSQL_BIN_LOG::purge_logs failed to sync the index file.");
    goto err;
  }

  /*
    File name exists in index file; delete until we find this file
    or a file that is used.
  */
  if ((error=find_log_pos(&log_info, NullS, 0 /*no mutex*/)))
    goto err;
  while ((strcmp(to_log,log_info.log_file_name) || (exit_loop=included)) &&
         can_purge_log(log_info.log_file_name))
  {
    if ((error= register_purge_index_entry(log_info.log_file_name)))
    {
      sql_print_error("MYSQL_BIN_LOG::purge_logs failed to copy %s to register file.",
                      log_info.log_file_name);
      goto err;
    }

    if (find_next_log(&log_info, 0) || exit_loop)
      break;
  }

  DBUG_EXECUTE_IF("crash_purge_before_update_index", DBUG_SUICIDE(););

  if ((error= sync_purge_index_file()))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to flush register file.");
    goto err;
  }

  /* We know how many files to delete. Update index file. */
  if ((error=update_log_index(&log_info, need_update_threads)))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to update the index file");
    goto err;
  }

  DBUG_EXECUTE_IF("crash_purge_critical_after_update_index", DBUG_SUICIDE(););

err:
  /* Read each entry from purge_index_file and delete the file. */
  if (is_inited_purge_index_file() &&
      (error= purge_index_entry(thd, reclaimed_space, FALSE)))
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to process registered files"
                    " that would be purged.");
  close_purge_index_file();

  DBUG_EXECUTE_IF("crash_purge_non_critical_after_update_index", DBUG_SUICIDE(););

  if (need_mutex)
    mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::set_purge_index_file_name(const char *base_file_name)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::set_purge_index_file_name");
  if (fn_format(purge_index_file_name, base_file_name, mysql_data_home,
                ".~rec~", MYF(MY_UNPACK_FILENAME | MY_SAFE_PATH |
                              MY_REPLACE_EXT)) == NULL)
  {
    error= 1;
    sql_print_error("MYSQL_BIN_LOG::set_purge_index_file_name failed to set "
                      "file name.");
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::open_purge_index_file(bool destroy)
{
  int error= 0;
  File file= -1;

  DBUG_ENTER("MYSQL_BIN_LOG::open_purge_index_file");

  if (destroy)
    close_purge_index_file();

  if (!my_b_inited(&purge_index_file))
  {
    if ((file= my_open(purge_index_file_name, O_RDWR | O_CREAT | O_BINARY,
                       MYF(MY_WME | ME_WAITTANG))) < 0  ||
        init_io_cache(&purge_index_file, file, IO_SIZE,
                      (destroy ? WRITE_CACHE : READ_CACHE),
                      0, 0, MYF(MY_WME | MY_NABP | MY_WAIT_IF_FULL)))
    {
      error= 1;
      sql_print_error("MYSQL_BIN_LOG::open_purge_index_file failed to open register "
                      " file.");
    }
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::close_purge_index_file()
{
  int error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::close_purge_index_file");

  if (my_b_inited(&purge_index_file))
  {
    end_io_cache(&purge_index_file);
    error= my_close(purge_index_file.file, MYF(0));
  }
  my_delete(purge_index_file_name, MYF(0));
  bzero((char*) &purge_index_file, sizeof(purge_index_file));

  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::is_inited_purge_index_file()
{
  return my_b_inited(&purge_index_file);
}

int MYSQL_BIN_LOG::sync_purge_index_file()
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::sync_purge_index_file");

  if ((error= flush_io_cache(&purge_index_file)) ||
      (error= my_sync(purge_index_file.file, MYF(MY_WME|MY_SYNC_FILESIZE))))
    DBUG_RETURN(error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_purge_index_entry(const char *entry)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::register_purge_index_entry");

  if ((error=my_b_write(&purge_index_file, (const uchar*)entry, strlen(entry))) ||
      (error=my_b_write(&purge_index_file, (const uchar*)"\n", 1)))
    DBUG_RETURN (error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_create_index_entry(const char *entry)
{
  DBUG_ENTER("MYSQL_BIN_LOG::register_create_index_entry");
  DBUG_RETURN(register_purge_index_entry(entry));
}

int MYSQL_BIN_LOG::purge_index_entry(THD *thd, ulonglong *reclaimed_space,
                                     bool need_mutex)
{
  DBUG_ENTER("MYSQL_BIN_LOG:purge_index_entry");
  MY_STAT s;
  int error= 0;
  LOG_INFO log_info;
  LOG_INFO check_log_info;

  DBUG_ASSERT(my_b_inited(&purge_index_file));

  if ((error=reinit_io_cache(&purge_index_file, READ_CACHE, 0, 0, 0)))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_index_entry failed to reinit register file "
                    "for read");
    goto err;
  }

  for (;;)
  {
    uint length;

    if ((length=my_b_gets(&purge_index_file, log_info.log_file_name,
                          FN_REFLEN)) <= 1)
    {
      if (purge_index_file.error)
      {
        error= purge_index_file.error;
        sql_print_error("MSYQL_BIN_LOG::purge_index_entry error %d reading from "
                        "register file.", error);
        goto err;
      }

      /* Reached EOF */
      break;
    }

    /* Get rid of the trailing '\n' */
    log_info.log_file_name[length-1]= 0;

    if (!mysql_file_stat(m_key_file_log, log_info.log_file_name, &s, MYF(0)))
    {
      if (my_errno == ENOENT) 
      {
        /*
          It's not fatal if we can't stat a log file that does not exist;
          If we could not stat, we won't delete.
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_LOG_PURGE_NO_FILE, ER_THD(thd, ER_LOG_PURGE_NO_FILE),
                              log_info.log_file_name);
        }
        sql_print_information("Failed to execute mysql_file_stat on file '%s'",
			      log_info.log_file_name);
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'; "
                                "consider examining correspondence "
                                "of your binlog index file "
                                "to the actual binlog files",
                                log_info.log_file_name);
        }
        error= LOG_INFO_FATAL;
        goto err;
      }
    }
    else
    {
      if ((error= find_log_pos(&check_log_info, log_info.log_file_name, need_mutex)))
      {
        if (error != LOG_INFO_EOF)
        {
          if (thd)
          {
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                ER_BINLOG_PURGE_FATAL_ERR,
                                "a problem with deleting %s and "
                                "reading the binlog index file",
                                log_info.log_file_name);
          }
          else
          {
            sql_print_information("Failed to delete file '%s' and "
                                  "read the binlog index file",
                                  log_info.log_file_name);
          }
          goto err;
        }
           
        error= 0;

        DBUG_PRINT("info",("purging %s",log_info.log_file_name));
        if (!my_delete(log_info.log_file_name, MYF(0)))
        {
          if (reclaimed_space)
            *reclaimed_space+= s.st_size;
        }
        else
        {
          if (my_errno == ENOENT)
          {
            if (thd)
            {
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_LOG_PURGE_NO_FILE, ER_THD(thd, ER_LOG_PURGE_NO_FILE),
                                  log_info.log_file_name);
            }
            sql_print_information("Failed to delete file '%s'",
                                  log_info.log_file_name);
            my_errno= 0;
          }
          else
          {
            if (thd)
            {
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_BINLOG_PURGE_FATAL_ERR,
                                  "a problem with deleting %s; "
                                  "consider examining correspondence "
                                  "of your binlog index file "
                                  "to the actual binlog files",
                                  log_info.log_file_name);
            }
            else
            {
              sql_print_information("Failed to delete file '%s'; "
                                    "consider examining correspondence "
                                    "of your binlog index file "
                                    "to the actual binlog files",
                                    log_info.log_file_name);
            }
            if (my_errno == EMFILE)
            {
              DBUG_PRINT("info",
                         ("my_errno: %d, set ret = LOG_INFO_EMFILE", my_errno));
              error= LOG_INFO_EMFILE;
              goto err;
            }
            error= LOG_INFO_FATAL;
            goto err;
          }
        }
      }
    }
  }

err:
  DBUG_RETURN(error);
}

/**
  Remove all logs before the given file date from disk and from the
  index file.

  @param thd		Thread pointer
  @param purge_time	Delete all log files before given date.

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0				ok
  @retval
    LOG_INFO_PURGE_NO_ROTATE	Binary file that can't be rotated
    LOG_INFO_FATAL              if any other than ENOENT error from
                                mysql_file_stat() or mysql_file_delete()
*/

int MYSQL_BIN_LOG::purge_logs_before_date(time_t purge_time)
{
  int error;
  char to_log[FN_REFLEN];
  LOG_INFO log_info;
  MY_STAT stat_area;
  THD *thd= current_thd;
  DBUG_ENTER("purge_logs_before_date");

  mysql_mutex_lock(&LOCK_index);
  to_log[0]= 0;

  if ((error=find_log_pos(&log_info, NullS, 0 /*no mutex*/)))
    goto err;

  while (strcmp(log_file_name, log_info.log_file_name) &&
	 can_purge_log(log_info.log_file_name))
  {
    if (!mysql_file_stat(m_key_file_log,
                         log_info.log_file_name, &stat_area, MYF(0)))
    {
      if (my_errno == ENOENT) 
      {
        /*
          It's not fatal if we can't stat a log file that does not exist.
        */
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'",
                                log_info.log_file_name);
        }
        error= LOG_INFO_FATAL;
        goto err;
      }
    }
    else
    {
      if (stat_area.st_mtime < purge_time) 
        strmake_buf(to_log, log_info.log_file_name);
      else
        break;
    }
    if (find_next_log(&log_info, 0))
      break;
  }

  error= (to_log[0] ? purge_logs(to_log, 1, 0, 1, (ulonglong *) 0) : 0);

err:
  mysql_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


bool
MYSQL_BIN_LOG::can_purge_log(const char *log_file_name_arg)
{
  xid_count_per_binlog *b;

  if (is_active(log_file_name_arg))
    return false;
  mysql_mutex_lock(&LOCK_xid_list);
  {
    I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
    while ((b= it++) &&
           0 != strncmp(log_file_name_arg+dirname_length(log_file_name_arg),
                        b->binlog_name, b->binlog_name_len))
      ;
  }
  mysql_mutex_unlock(&LOCK_xid_list);
  if (b)
    return false;
  return !log_in_use(log_file_name_arg);
}
#endif /* HAVE_REPLICATION */


bool
MYSQL_BIN_LOG::is_xidlist_idle()
{
  bool res;
  mysql_mutex_lock(&LOCK_xid_list);
  res= is_xidlist_idle_nolock();
  mysql_mutex_unlock(&LOCK_xid_list);
  return res;
}


bool
MYSQL_BIN_LOG::is_xidlist_idle_nolock()
{
  xid_count_per_binlog *b;

  I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
  while ((b= it++))
  {
    if (b->xid_count > 0)
      return false;
  }
  return true;
}


/**
  Create a new log file name.

  @param buf		buf of at least FN_REFLEN where new name is stored

  @note
    If file name will be longer then FN_REFLEN it will be truncated
*/

void MYSQL_BIN_LOG::make_log_name(char* buf, const char* log_ident)
{
  uint dir_len = dirname_length(log_file_name); 
  if (dir_len >= FN_REFLEN)
    dir_len=FN_REFLEN-1;
  strnmov(buf, log_file_name, dir_len);
  strmake(buf+dir_len, log_ident, FN_REFLEN - dir_len -1);
}


/**
  Check if we are writing/reading to the given log file.
*/

bool MYSQL_BIN_LOG::is_active(const char *log_file_name_arg)
{
  /**
   * there should/must be mysql_mutex_assert_owner(&LOCK_log) here...
   * but code violates this! (scary monsters and super creeps!)
   *
   * example stacktrace:
   * #8  MYSQL_BIN_LOG::is_active
   * #9  MYSQL_BIN_LOG::can_purge_log
   * #10 MYSQL_BIN_LOG::purge_logs
   * #11 MYSQL_BIN_LOG::purge_first_log
   * #12 next_event
   * #13 exec_relay_log_event
   *
   * I didn't investigate if this is ligit...(i.e if my comment is wrong)
   */
  return !strcmp(log_file_name, log_file_name_arg);
}


/*
  Wrappers around new_file_impl to avoid using argument
  to control locking. The argument 1) less readable 2) breaks
  incapsulation 3) allows external access to the class without
  a lock (which is not possible with private new_file_without_locking
  method).

  @retval
    nonzero - error
*/

int MYSQL_BIN_LOG::new_file()
{
  return new_file_impl(1);
}

/*
  @retval
    nonzero - error
 */
int MYSQL_BIN_LOG::new_file_without_locking()
{
  return new_file_impl(0);
}


/**
  Start writing to a new log file or reopen the old file.

  @param need_lock		Set to 1 if caller has not locked LOCK_log

  @retval
    nonzero - error

  @note
    The new file name is stored last in the index file
*/

int MYSQL_BIN_LOG::new_file_impl(bool need_lock)
{
  int error= 0, close_on_error= FALSE;
  char new_name[FN_REFLEN], *new_name_ptr, *old_name, *file_to_open;
  uint close_flag;
  bool delay_close= false;
  File UNINIT_VAR(old_file);
  DBUG_ENTER("MYSQL_BIN_LOG::new_file_impl");

  if (!is_open())
  {
    DBUG_PRINT("info",("log is closed"));
    DBUG_RETURN(error);
  }

  if (need_lock)
    mysql_mutex_lock(&LOCK_log);
  mysql_mutex_lock(&LOCK_index);

  mysql_mutex_assert_owner(&LOCK_log);
  mysql_mutex_assert_owner(&LOCK_index);

  /* Reuse old name if not binlog and not update log */
  new_name_ptr= name;

  /*
    If user hasn't specified an extension, generate a new log name
    We have to do this here and not in open as we want to store the
    new file name in the current binary log file.
  */
  if ((error= generate_new_name(new_name, name, 0)))
    goto end;
  new_name_ptr=new_name;

  if (log_type == LOG_BIN)
  {
    {
      /*
        We log the whole file name for log file as the user may decide
        to change base names at some point.
      */
      Rotate_log_event r(new_name+dirname_length(new_name), 0, LOG_EVENT_OFFSET,
                         is_relay_log ? Rotate_log_event::RELAY_LOG : 0);
      /* 
         The current relay-log's closing Rotate event must have checksum
         value computed with an algorithm of the last relay-logged FD event.
      */
      if (is_relay_log)
        r.checksum_alg= relay_log_checksum_alg;
      DBUG_ASSERT(!is_relay_log || relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
      if(DBUG_EVALUATE_IF("fault_injection_new_file_rotate_event", (error=close_on_error=TRUE), FALSE) ||
         (error= write_event(&r)))
      {
        DBUG_EXECUTE_IF("fault_injection_new_file_rotate_event", errno=2;);
        close_on_error= TRUE;
        my_printf_error(ER_ERROR_ON_WRITE,
                        ER_THD_OR_DEFAULT(current_thd, ER_CANT_OPEN_FILE),
                        MYF(ME_FATALERROR), name, errno);
        goto end;
      }
      bytes_written += r.data_written;
    }
    /*
      Update needs to be signalled even if there is no rotate event
      log rotation should give the waiting thread a signal to
      discover EOF and move on to the next log.
    */
    signal_update();
  }
  old_name=name;
  name=0;				// Don't free name
  close_flag= LOG_CLOSE_TO_BE_OPENED | LOG_CLOSE_INDEX;
  if (!is_relay_log)
  {
    /*
      We need to keep the old binlog file open (and marked as in-use) until
      the new one is fully created and synced to disk and index. Otherwise we
      leave a window where if we crash, there is no binlog file marked as
      crashed for server restart to detect the need for recovery.
    */
    old_file= log_file.file;
    close_flag|= LOG_CLOSE_DELAYED_CLOSE;
    delay_close= true;
  }
  close(close_flag);
  if (log_type == LOG_BIN && checksum_alg_reset != BINLOG_CHECKSUM_ALG_UNDEF)
  {
    DBUG_ASSERT(!is_relay_log);
    DBUG_ASSERT(binlog_checksum_options != checksum_alg_reset);
    binlog_checksum_options= checksum_alg_reset;
  }
  /*
     Note that at this point, log_state != LOG_CLOSED
     (important for is_open()).
  */

  /*
     new_file() is only used for rotation (in FLUSH LOGS or because size >
     max_binlog_size or max_relay_log_size).
     If this is a binary log, the Format_description_log_event at the
     beginning of the new file should have created=0 (to distinguish with the
     Format_description_log_event written at server startup, which should
     trigger temp tables deletion on slaves.
  */

  /* reopen index binlog file, BUG#34582 */
  file_to_open= index_file_name;
  error= open_index_file(index_file_name, 0, FALSE);
  if (!error)
  {
    /* reopen the binary log file. */
    file_to_open= new_name_ptr;
    error= open(old_name, log_type, new_name_ptr, 0, io_cache_type,
                max_size, 1, FALSE);
  }

  /* handle reopening errors */
  if (error)
  {
    my_printf_error(ER_CANT_OPEN_FILE,
                    ER_THD_OR_DEFAULT(current_thd, ER_CANT_OPEN_FILE), 
                    MYF(ME_FATALERROR), file_to_open, error);
    close_on_error= TRUE;
  }

  my_free(old_name);

end:

  if (delay_close)
  {
    clear_inuse_flag_when_closing(old_file);
    mysql_file_close(old_file, MYF(MY_WME));
  }

  if (error && close_on_error /* rotate or reopen failed */)
  {
    /* 
      Close whatever was left opened.

      We are keeping the behavior as it exists today, ie,
      we disable logging and move on (see: BUG#51014).

      TODO: as part of WL#1790 consider other approaches:
       - kill mysql (safety);
       - try multiple locations for opening a log file;
       - switch server to protected/readonly mode
       - ...
    */
    close(LOG_CLOSE_INDEX);
    sql_print_error("Could not open %s for logging (error %d). "
                     "Turning logging off for the whole duration "
                     "of the MySQL server process. To turn it on "
                     "again: fix the cause, shutdown the MySQL "
                     "server and restart it.", 
                     new_name_ptr, errno);
  }

  if (need_lock)
    mysql_mutex_unlock(&LOCK_log);
  mysql_mutex_unlock(&LOCK_index);

  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::write_event(Log_event *ev, IO_CACHE *file)
{
  Log_event_writer writer(file, &crypto);
  if (crypto.scheme && file == &log_file)
    writer.ctx= alloca(crypto.ctx_size);

  return writer.write(ev);
}

bool MYSQL_BIN_LOG::append(Log_event *ev)
{
  bool res;
  mysql_mutex_lock(&LOCK_log);
  res= append_no_lock(ev);
  mysql_mutex_unlock(&LOCK_log);
  return res;
}


bool MYSQL_BIN_LOG::append_no_lock(Log_event* ev)
{
  bool error = 0;
  DBUG_ENTER("MYSQL_BIN_LOG::append");

  mysql_mutex_assert_owner(&LOCK_log);
  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);

  if (write_event(ev))
  {
    error=1;
    goto err;
  }
  bytes_written+= ev->data_written;
  DBUG_PRINT("info",("max_size: %lu",max_size));
  if (flush_and_sync(0))
    goto err;
  if (my_b_append_tell(&log_file) > max_size)
    error= new_file_without_locking();
err:
  signal_update();				// Safe as we don't call close
  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::write_event_buffer(uchar* buf, uint len)
{
  bool error= 1;
  uchar *ebuf= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::write_event_buffer");

  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);

  mysql_mutex_assert_owner(&LOCK_log);

  if (crypto.scheme != 0)
  {
    DBUG_ASSERT(crypto.scheme == 1);

    uint elen;
    uchar iv[BINLOG_IV_LENGTH];

    ebuf= (uchar*)my_safe_alloca(len);
    if (!ebuf)
      goto err;

    crypto.set_iv(iv, my_b_append_tell(&log_file));

    /*
      we want to encrypt everything, excluding the event length:
      massage the data before the encryption
    */
    memcpy(buf + EVENT_LEN_OFFSET, buf, 4);

    if (encryption_crypt(buf + 4, len - 4,
                         ebuf + 4, &elen,
                         crypto.key, crypto.key_length, iv, sizeof(iv),
                         ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
                         ENCRYPTION_KEY_SYSTEM_DATA, crypto.key_version))
      goto err;

    DBUG_ASSERT(elen == len - 4);

    /* massage the data after the encryption */
    memcpy(ebuf, ebuf + EVENT_LEN_OFFSET, 4);
    int4store(ebuf + EVENT_LEN_OFFSET, len);

    buf= ebuf;
  }
  if (my_b_append(&log_file, buf, len))
    goto err;
  bytes_written+= len;

  error= 0;
  DBUG_PRINT("info",("max_size: %lu",max_size));
  if (flush_and_sync(0))
    goto err;
  if (my_b_append_tell(&log_file) > max_size)
    error= new_file_without_locking();
err:
  my_safe_afree(ebuf, len);
  if (!error)
    signal_update();
  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::flush_and_sync(bool *synced)
{
  int err=0, fd=log_file.file;
  if (synced)
    *synced= 0;
  mysql_mutex_assert_owner(&LOCK_log);
  if (flush_io_cache(&log_file))
    return 1;
  uint sync_period= get_sync_period();
  if (sync_period && ++sync_counter >= sync_period)
  {
    sync_counter= 0;
    err= mysql_file_sync(fd, MYF(MY_WME|MY_SYNC_FILESIZE));
    if (synced)
      *synced= 1;
#ifndef DBUG_OFF
    if (opt_binlog_dbug_fsync_sleep > 0)
      my_sleep(opt_binlog_dbug_fsync_sleep);
#endif
  }
  return err;
}

void MYSQL_BIN_LOG::start_union_events(THD *thd, query_id_t query_id_param)
{
  DBUG_ASSERT(!thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= TRUE;
  thd->binlog_evt_union.unioned_events= FALSE;
  thd->binlog_evt_union.unioned_events_trans= FALSE;
  thd->binlog_evt_union.first_query_id= query_id_param;
}

void MYSQL_BIN_LOG::stop_union_events(THD *thd)
{
  DBUG_ASSERT(thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= FALSE;
}

bool MYSQL_BIN_LOG::is_query_in_union(THD *thd, query_id_t query_id_param)
{
  return (thd->binlog_evt_union.do_union && 
          query_id_param >= thd->binlog_evt_union.first_query_id);
}

/** 
  This function checks if a transactional table was updated by the
  current transaction.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool
trans_has_updated_trans_table(const THD* thd)
{
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  return (cache_mngr ? !cache_mngr->trx_cache.empty() : 0);
}

/** 
  This function checks if a transactional table was updated by the
  current statement.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a transactional table was updated, @c false otherwise.
*/
bool
stmt_has_updated_trans_table(const THD *thd)
{
  Ha_trx_info *ha_info;

  for (ha_info= thd->transaction.stmt.ha_list; ha_info;
       ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write() && ha_info->ht() != binlog_hton)
      return (TRUE);
  }
  return (FALSE);
}

/** 
  This function checks if either a trx-cache or a non-trx-cache should
  be used. If @c bin_log_direct_non_trans_update is active or the format
  is either MIXED or ROW, the cache to be used depends on the flag @c
  is_transactional. 

  On the other hand, if binlog_format is STMT or direct option is
  OFF, the trx-cache should be used if and only if the statement is
  transactional or the trx-cache is not empty. Otherwise, the
  non-trx-cache should be used.

  @param thd              The client thread.
  @param is_transactional The changes are related to a trx-table.
  @return
    @c true if a trx-cache should be used, @c false otherwise.
*/
bool use_trans_cache(const THD* thd, bool is_transactional)
{
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  return
    ((thd->is_current_stmt_binlog_format_row() ||
     thd->variables.binlog_direct_non_trans_update) ? is_transactional :
     (is_transactional || !cache_mngr->trx_cache.empty()));
}

/**
  This function checks if a transaction, either a multi-statement
  or a single statement transaction is about to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if committing a transaction, otherwise @c false.
*/
bool ending_trans(THD* thd, const bool all)
{
  return (all || ending_single_stmt_trans(thd, all));
}

/**
  This function checks if a single statement transaction is about
  to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if committing a single statement transaction, otherwise
    @c false.
*/
bool ending_single_stmt_trans(THD* thd, const bool all)
{
  return (!all && !thd->in_multi_stmt_transaction_mode());
}

/**
  This function checks if a non-transactional table was updated by
  the current transaction.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a non-transactional table was updated, @c false
    otherwise.
*/
bool trans_has_updated_non_trans_table(const THD* thd)
{
  return (thd->transaction.all.modified_non_trans_table ||
          thd->transaction.stmt.modified_non_trans_table);
}

/**
  This function checks if a non-transactional table was updated by the
  current statement.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a non-transactional table was updated, @c false otherwise.
*/
bool stmt_has_updated_non_trans_table(const THD* thd)
{
  return (thd->transaction.stmt.modified_non_trans_table);
}

/*
  These functions are placed in this file since they need access to
  binlog_hton, which has internal linkage.
*/

binlog_cache_mngr *THD::binlog_setup_trx_data()
{
  DBUG_ENTER("THD::binlog_setup_trx_data");
  binlog_cache_mngr *cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);

  if (cache_mngr)
    DBUG_RETURN(cache_mngr);                             // Already set up

  cache_mngr= (binlog_cache_mngr*) my_malloc(sizeof(binlog_cache_mngr), MYF(MY_ZEROFILL));
  if (!cache_mngr ||
      open_cached_file(&cache_mngr->stmt_cache.cache_log, mysql_tmpdir,
                       LOG_PREFIX, binlog_stmt_cache_size, MYF(MY_WME)) ||
      open_cached_file(&cache_mngr->trx_cache.cache_log, mysql_tmpdir,
                       LOG_PREFIX, binlog_cache_size, MYF(MY_WME)))
  {
    my_free(cache_mngr);
    DBUG_RETURN(0);                      // Didn't manage to set it up
  }
  thd_set_ha_data(this, binlog_hton, cache_mngr);

  cache_mngr= new (cache_mngr)
              binlog_cache_mngr(max_binlog_stmt_cache_size,
                                max_binlog_cache_size,
                                &binlog_stmt_cache_use,
                                &binlog_stmt_cache_disk_use,
                                &binlog_cache_use,
                                &binlog_cache_disk_use);
  DBUG_RETURN(cache_mngr);
}

/*
  Function to start a statement and optionally a transaction for the
  binary log.

  SYNOPSIS
    binlog_start_trans_and_stmt()

  DESCRIPTION

    This function does three things:
    - Start a transaction if not in autocommit mode or if a BEGIN
      statement has been seen.

    - Start a statement transaction to allow us to truncate the cache.

    - Save the currrent binlog position so that we can roll back the
      statement by truncating the cache.

      We only update the saved position if the old one was undefined,
      the reason is that there are some cases (e.g., for CREATE-SELECT)
      where the position is saved twice (e.g., both in
      select_create::prepare() and THD::binlog_write_table_map()) , but
      we should use the first. This means that calls to this function
      can be used to start the statement before the first table map
      event, to include some extra events.
 */

void
THD::binlog_start_trans_and_stmt()
{
  binlog_cache_mngr *cache_mngr= (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);
  DBUG_ENTER("binlog_start_trans_and_stmt");
  DBUG_PRINT("enter", ("cache_mngr: %p  cache_mngr->trx_cache.get_prev_position(): %lu",
                       cache_mngr,
                       (cache_mngr ? (ulong) cache_mngr->trx_cache.get_prev_position() :
                        (ulong) 0)));

  if (cache_mngr == NULL ||
      cache_mngr->trx_cache.get_prev_position() == MY_OFF_T_UNDEF)
  {
    this->binlog_set_stmt_begin();
    if (in_multi_stmt_transaction_mode())
      trans_register_ha(this, TRUE, binlog_hton);
    trans_register_ha(this, FALSE, binlog_hton);
    /*
      Mark statement transaction as read/write. We never start
      a binary log transaction and keep it read-only,
      therefore it's best to mark the transaction read/write just
      at the same time we start it.
      Not necessary to mark the normal transaction read/write
      since the statement-level flag will be propagated automatically
      inside ha_commit_trans.
    */
    ha_data[binlog_hton->slot].ha_info[0].set_trx_read_write();
  }
  DBUG_VOID_RETURN;
}

void THD::binlog_set_stmt_begin() {
  binlog_cache_mngr *cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);

  /*
    The call to binlog_trans_log_savepos() might create the cache_mngr
    structure, if it didn't exist before, so we save the position
    into an auto variable and then write it into the transaction
    data for the binary log (i.e., cache_mngr).
  */
  my_off_t pos= 0;
  binlog_trans_log_savepos(this, &pos);
  cache_mngr= (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);
  cache_mngr->trx_cache.set_prev_position(pos);
}

static int
binlog_start_consistent_snapshot(handlerton *hton, THD *thd)
{
  int err= 0;
  DBUG_ENTER("binlog_start_consistent_snapshot");

  binlog_cache_mngr *const cache_mngr= thd->binlog_setup_trx_data();

  /* Server layer calls us with LOCK_commit_ordered locked, so this is safe. */
  mysql_mutex_assert_owner(&LOCK_commit_ordered);
  strmake_buf(cache_mngr->last_commit_pos_file, mysql_bin_log.last_commit_pos_file);
  cache_mngr->last_commit_pos_offset= mysql_bin_log.last_commit_pos_offset;

  trans_register_ha(thd, TRUE, hton);

  DBUG_RETURN(err);
}

/**
  This function writes a table map to the binary log. 
  Note that in order to keep the signature uniform with related methods,
  we use a redundant parameter to indicate whether a transactional table
  was changed or not.

  If with_annotate != NULL and
  *with_annotate = TRUE write also Annotate_rows before the table map.
 
  @param table             a pointer to the table.
  @param is_transactional  @c true indicates a transactional table,
                           otherwise @c false a non-transactional.
  @return
    nonzero if an error pops up when writing the table map event.
*/
int THD::binlog_write_table_map(TABLE *table, bool is_transactional,
                                my_bool *with_annotate)
{
  int error;
  DBUG_ENTER("THD::binlog_write_table_map");
  DBUG_PRINT("enter", ("table: 0x%lx  (%s: #%lu)",
                       (long) table, table->s->table_name.str,
                       table->s->table_map_id));

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_transactional= 1;
  
  /* Pre-conditions */
  DBUG_ASSERT(is_current_stmt_binlog_format_row());
  DBUG_ASSERT(WSREP_EMULATE_BINLOG(this) || mysql_bin_log.is_open());
  DBUG_ASSERT(table->s->table_map_id != ULONG_MAX);

  Table_map_log_event
    the_event(this, table, table->s->table_map_id, is_transactional);

  if (binlog_table_maps == 0)
    binlog_start_trans_and_stmt();

  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);

  IO_CACHE *file=
    cache_mngr->get_binlog_cache_log(use_trans_cache(this, is_transactional));
  Log_event_writer writer(file);
  if (with_annotate && *with_annotate)
  {
    Annotate_rows_log_event anno(table->in_use, is_transactional, false);
    /* Annotate event should be written not more than once */
    *with_annotate= 0;
    if ((error= writer.write(&anno)))
      DBUG_RETURN(error);
  }
  if ((error= writer.write(&the_event)))
    DBUG_RETURN(error);

  binlog_table_maps++;
  DBUG_RETURN(0);
}

/**
  This function retrieves a pending row event from a cache which is
  specified through the parameter @c is_transactional. Respectively, when it
  is @c true, the pending event is returned from the transactional cache.
  Otherwise from the non-transactional cache.

  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
  @return
    The row event if any. 
*/
Rows_log_event*
THD::binlog_get_pending_rows_event(bool is_transactional) const
{
  Rows_log_event* rows= NULL;
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(this, binlog_hton);

  /*
    This is less than ideal, but here's the story: If there is no cache_mngr,
    prepare_pending_rows_event() has never been called (since the cache_mngr
    is set up there). In that case, we just return NULL.
   */
  if (cache_mngr)
  {
    binlog_cache_data *cache_data=
      cache_mngr->get_binlog_cache_data(use_trans_cache(this, is_transactional));

    rows= cache_data->pending();
  }
  return (rows);
}

/**
  This function stores a pending row event into a cache which is specified
  through the parameter @c is_transactional. Respectively, when it is @c
  true, the pending event is stored into the transactional cache. Otherwise
  into the non-transactional cache.

  @param evt               a pointer to the row event.
  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
*/
void
THD::binlog_set_pending_rows_event(Rows_log_event* ev, bool is_transactional)
{
  binlog_cache_mngr *const cache_mngr= binlog_setup_trx_data();

  DBUG_ASSERT(cache_mngr);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(use_trans_cache(this, is_transactional));

  cache_data->set_pending(ev);
}


/**
  This function removes the pending rows event, discarding any outstanding
  rows. If there is no pending rows event available, this is effectively a
  no-op.

  @param thd               a pointer to the user thread.
  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
*/
int
MYSQL_BIN_LOG::remove_pending_rows_event(THD *thd, bool is_transactional)
{
  DBUG_ENTER("MYSQL_BIN_LOG::remove_pending_rows_event");

  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  DBUG_ASSERT(cache_mngr);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(use_trans_cache(thd, is_transactional));

  if (Rows_log_event* pending= cache_data->pending())
  {
    delete pending;
    cache_data->set_pending(NULL);
  }

  DBUG_RETURN(0);
}

/*
  Moves the last bunch of rows from the pending Rows event to a cache (either
  transactional cache if is_transaction is @c true, or the non-transactional
  cache otherwise. Sets a new pending event.

  @param thd               a pointer to the user thread.
  @param evt               a pointer to the row event.
  @param is_transactional  @c true indicates a transactional cache,
                           otherwise @c false a non-transactional.
*/
int
MYSQL_BIN_LOG::flush_and_set_pending_rows_event(THD *thd,
                                                Rows_log_event* event,
                                                bool is_transactional)
{
  DBUG_ENTER("MYSQL_BIN_LOG::flush_and_set_pending_rows_event(event)");
  DBUG_ASSERT(WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open());
  DBUG_PRINT("enter", ("event: 0x%lx", (long) event));

  int error= 0;
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);

  DBUG_ASSERT(cache_mngr);

  binlog_cache_data *cache_data=
    cache_mngr->get_binlog_cache_data(use_trans_cache(thd, is_transactional));

  DBUG_PRINT("info", ("cache_mngr->pending(): 0x%lx", (long) cache_data->pending()));

  if (Rows_log_event* pending= cache_data->pending())
  {
    Log_event_writer writer(&cache_data->cache_log);

    /*
      Write pending event to the cache.
    */
    DBUG_EXECUTE_IF("simulate_disk_full_at_flush_pending",
                    {DBUG_SET("+d,simulate_file_write_error");});
    if (writer.write(pending))
    {
      set_write_error(thd, is_transactional);
      if (check_write_error(thd) && cache_data &&
          stmt_has_updated_non_trans_table(thd))
        cache_data->set_incident();
      delete pending;
      cache_data->set_pending(NULL);
      DBUG_EXECUTE_IF("simulate_disk_full_at_flush_pending",
                      {DBUG_SET("-d,simulate_file_write_error");});
      DBUG_RETURN(1);
    }

    delete pending;
  }

  thd->binlog_set_pending_rows_event(event, is_transactional);

  DBUG_RETURN(error);
}


/* Generate a new global transaction ID, and write it to the binlog */

bool
MYSQL_BIN_LOG::write_gtid_event(THD *thd, bool standalone,
                                bool is_transactional, uint64 commit_id)
{
  rpl_gtid gtid;
  uint32 domain_id;
  uint32 local_server_id;
  uint64 seq_no;
  int err;
  DBUG_ENTER("write_gtid_event");
  DBUG_PRINT("enter", ("standalone: %d", standalone));

#ifdef WITH_WSREP
  if (WSREP(thd) && thd->wsrep_trx_meta.gtid.seqno != -1 && wsrep_gtid_mode)
  {
    domain_id= wsrep_gtid_domain_id;
  } else {
#endif /* WITH_WSREP */
  domain_id= thd->variables.gtid_domain_id;
#ifdef WITH_WSREP
  }
#endif /* WITH_WSREP */
  local_server_id= thd->variables.server_id;
  seq_no= thd->variables.gtid_seq_no;

  if (thd->variables.option_bits & OPTION_GTID_BEGIN)
  {
    DBUG_PRINT("error", ("OPTION_GTID_BEGIN is set. "
                         "Master and slave will have different GTID values"));
    /* Reset the flag, as we will write out a GTID anyway */
    thd->variables.option_bits&= ~OPTION_GTID_BEGIN;
  }

  /*
    Reset the session variable gtid_seq_no, to reduce the risk of accidentally
    producing a duplicate GTID.
  */
  thd->variables.gtid_seq_no= 0;
  if (seq_no != 0)
  {
    /* Use the specified sequence number. */
    gtid.domain_id= domain_id;
    gtid.server_id= local_server_id;
    gtid.seq_no= seq_no;
    err= rpl_global_gtid_binlog_state.update(&gtid, opt_gtid_strict_mode);
    if (err && thd->get_stmt_da()->sql_errno()==ER_GTID_STRICT_OUT_OF_ORDER)
      errno= ER_GTID_STRICT_OUT_OF_ORDER;
  }
  else
  {
    /* Allocate the next sequence number for the GTID. */
    err= rpl_global_gtid_binlog_state.update_with_next_gtid(domain_id,
                                                            local_server_id, &gtid);
    seq_no= gtid.seq_no;
  }
  if (err)
    DBUG_RETURN(true);
  thd->last_commit_gtid= gtid;

  Gtid_log_event gtid_event(thd, seq_no, domain_id, standalone,
                            LOG_EVENT_SUPPRESS_USE_F, is_transactional,
                            commit_id);

  /* Write the event to the binary log. */
  DBUG_ASSERT(this == &mysql_bin_log);
  if (write_event(&gtid_event))
    DBUG_RETURN(true);
  status_var_add(thd->status_var.binlog_bytes_written, gtid_event.data_written);

  DBUG_RETURN(false);
}


int
MYSQL_BIN_LOG::write_state_to_file()
{
  File file_no;
  IO_CACHE cache;
  char buf[FN_REFLEN];
  int err;
  bool opened= false;
  bool log_inited= false;

  fn_format(buf, opt_bin_logname, mysql_data_home, ".state",
            MY_UNPACK_FILENAME);
  if ((file_no= mysql_file_open(key_file_binlog_state, buf,
                                O_RDWR|O_CREAT|O_TRUNC|O_BINARY,
                                MYF(MY_WME))) < 0)
  {
    err= 1;
    goto err;
  }
  opened= true;
  if ((err= init_io_cache(&cache, file_no, IO_SIZE, WRITE_CACHE, 0, 0,
                           MYF(MY_WME|MY_WAIT_IF_FULL))))
    goto err;
  log_inited= true;
  if ((err= rpl_global_gtid_binlog_state.write_to_iocache(&cache)))
    goto err;
  log_inited= false;
  if ((err= end_io_cache(&cache)))
    goto err;
  if ((err= mysql_file_sync(file_no, MYF(MY_WME|MY_SYNC_FILESIZE))))
    goto err;
  goto end;

err:
  sql_print_error("Error writing binlog state to file '%s'.\n", buf);
  if (log_inited)
    end_io_cache(&cache);
end:
  if (opened)
    mysql_file_close(file_no, MYF(0));

  return err;
}


/*
  Initialize the binlog state from the master-bin.state file, at server startup.

  Returns:
    0 for success.
    2 for when .state file did not exist.
    1 for other error.
*/
int
MYSQL_BIN_LOG::read_state_from_file()
{
  File file_no;
  IO_CACHE cache;
  char buf[FN_REFLEN];
  int err;
  bool opened= false;
  bool log_inited= false;

  fn_format(buf, opt_bin_logname, mysql_data_home, ".state",
            MY_UNPACK_FILENAME);
  if ((file_no= mysql_file_open(key_file_binlog_state, buf,
                                O_RDONLY|O_BINARY, MYF(0))) < 0)
  {
    if (my_errno != ENOENT)
    {
      err= 1;
      goto err;
    }
    else
    {
      /*
        If the state file does not exist, this is the first server startup
        with GTID enabled. So initialize to empty state.
      */
      rpl_global_gtid_binlog_state.reset();
      err= 2;
      goto end;
    }
  }
  opened= true;
  if ((err= init_io_cache(&cache, file_no, IO_SIZE, READ_CACHE, 0, 0,
                          MYF(MY_WME|MY_WAIT_IF_FULL))))
    goto err;
  log_inited= true;
  if ((err= rpl_global_gtid_binlog_state.read_from_iocache(&cache)))
    goto err;
  goto end;

err:
  sql_print_error("Error reading binlog GTID state from file '%s'.\n", buf);
end:
  if (log_inited)
    end_io_cache(&cache);
  if (opened)
    mysql_file_close(file_no, MYF(0));

  return err;
}


int
MYSQL_BIN_LOG::get_most_recent_gtid_list(rpl_gtid **list, uint32 *size)
{
  return rpl_global_gtid_binlog_state.get_most_recent_gtid_list(list, size);
}


bool
MYSQL_BIN_LOG::append_state_pos(String *str)
{
  return rpl_global_gtid_binlog_state.append_pos(str);
}


bool
MYSQL_BIN_LOG::append_state(String *str)
{
  return rpl_global_gtid_binlog_state.append_state(str);
}


bool
MYSQL_BIN_LOG::is_empty_state()
{
  return (rpl_global_gtid_binlog_state.count() == 0);
}


bool
MYSQL_BIN_LOG::find_in_binlog_state(uint32 domain_id, uint32 server_id_arg,
                                    rpl_gtid *out_gtid)
{
  rpl_gtid *gtid;
  if ((gtid= rpl_global_gtid_binlog_state.find(domain_id, server_id_arg)))
    *out_gtid= *gtid;
  return gtid != NULL;
}


bool
MYSQL_BIN_LOG::lookup_domain_in_binlog_state(uint32 domain_id,
                                             rpl_gtid *out_gtid)
{
  rpl_gtid *found_gtid;

  if ((found_gtid= rpl_global_gtid_binlog_state.find_most_recent(domain_id)))
  {
    *out_gtid= *found_gtid;
    return true;
  }

  return false;
}


int
MYSQL_BIN_LOG::bump_seq_no_counter_if_needed(uint32 domain_id, uint64 seq_no)
{
  return rpl_global_gtid_binlog_state.bump_seq_no_if_needed(domain_id, seq_no);
}


bool
MYSQL_BIN_LOG::check_strict_gtid_sequence(uint32 domain_id,
                                          uint32 server_id_arg,
                                          uint64 seq_no)
{
  return rpl_global_gtid_binlog_state.check_strict_sequence(domain_id,
                                                            server_id_arg,
                                                            seq_no);
}


/**
  Write an event to the binary log. If with_annotate != NULL and
  *with_annotate = TRUE write also Annotate_rows before the event
  (this should happen only if the event is a Table_map).
*/

bool MYSQL_BIN_LOG::write(Log_event *event_info, my_bool *with_annotate)
{
  THD *thd= event_info->thd;
  bool error= 1;
  binlog_cache_data *cache_data= 0;
  bool is_trans_cache= FALSE;
  bool using_trans= event_info->use_trans_cache();
  bool direct= event_info->use_direct_logging();
  ulong UNINIT_VAR(prev_binlog_id);
  DBUG_ENTER("MYSQL_BIN_LOG::write(Log_event *)");

  /*
    When binary logging is not enabled (--log-bin=0), wsrep-patch partially
    enables it without opening the binlog file (MYSQL_BIN_LOG::open().
    So, avoid writing to binlog file.
  */
  if (direct &&
      (wsrep_emulate_bin_log ||
       (WSREP(thd) && !(thd->variables.option_bits & OPTION_BIN_LOG))))
    DBUG_RETURN(0);

  if (thd->variables.option_bits & OPTION_GTID_BEGIN)
  {
    DBUG_PRINT("info", ("OPTION_GTID_BEGIN was set"));
    /* Wait for commit from binary log before we commit */
    direct= 0;
    using_trans= 1;
  }

  if (thd->binlog_evt_union.do_union)
  {
    /*
      In Stored function; Remember that function call caused an update.
      We will log the function call to the binary log on function exit
    */
    thd->binlog_evt_union.unioned_events= TRUE;
    thd->binlog_evt_union.unioned_events_trans |= using_trans;
    DBUG_RETURN(0);
  }

  /*
    We only end the statement if we are in a top-level statement.  If
    we are inside a stored function, we do not end the statement since
    this will close all tables on the slave. But there can be a special case
    where we are inside a stored function/trigger and a SAVEPOINT is being
    set in side the stored function/trigger. This SAVEPOINT execution will
    force the pending event to be flushed without an STMT_END_F flag. This
    will result in a case where following DMLs will be considered as part of
    same statement and result in data loss on slave. Hence in this case we
    force the end_stmt to be true.
  */
  bool const end_stmt= (thd->in_sub_stmt && thd->lex->sql_command ==
                        SQLCOM_SAVEPOINT) ? true :
    (thd->locked_tables_mode && thd->lex->requires_prelocking());
  if (thd->binlog_flush_pending_rows_event(end_stmt, using_trans))
    DBUG_RETURN(error);

  /*
     In most cases this is only called if 'is_open()' is true; in fact this is
     mostly called if is_open() *was* true a few instructions before, but it
     could have changed since.
  */
  /* applier and replayer can skip writing binlog events */
  if ((WSREP_EMULATE_BINLOG(thd) &&
       IF_WSREP(thd->wsrep_exec_mode != REPL_RECV, 0)) || is_open())
  {
    my_off_t UNINIT_VAR(my_org_b_tell);
#ifdef HAVE_REPLICATION
    /*
      In the future we need to add to the following if tests like
      "do the involved tables match (to be implemented)
      binlog_[wild_]{do|ignore}_table?" (WL#1049)"
    */
    const char *local_db= event_info->get_db();

    bool option_bin_log_flag= (thd->variables.option_bits & OPTION_BIN_LOG);

    /*
      Log all updates to binlog cache so that they can get replicated to other
      nodes. A check has been added to stop them from getting logged into
      binary log files.
    */
    if (WSREP(thd)) option_bin_log_flag= true;

    if ((!(option_bin_log_flag)) ||
	(thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
         thd->lex->sql_command != SQLCOM_SAVEPOINT &&
         !binlog_filter->db_ok(local_db)))
      DBUG_RETURN(0);
#endif /* HAVE_REPLICATION */

    IO_CACHE *file= NULL;

    if (direct)
    {
      int res;
      uint64 commit_id= 0;
      DBUG_PRINT("info", ("direct is set"));
      if ((res= thd->wait_for_prior_commit()))
        DBUG_RETURN(res);
      file= &log_file;
      my_org_b_tell= my_b_tell(file);
      mysql_mutex_lock(&LOCK_log);
      prev_binlog_id= current_binlog_id;
      DBUG_EXECUTE_IF("binlog_force_commit_id",
        {
          const LEX_STRING commit_name= { C_STRING_WITH_LEN("commit_id") };
          bool null_value;
          user_var_entry *entry=
            (user_var_entry*) my_hash_search(&thd->user_vars,
                                             (uchar*) commit_name.str,
                                             commit_name.length);
          commit_id= entry->val_int(&null_value);
        });
      if (write_gtid_event(thd, true, using_trans, commit_id))
        goto err;
    }
    else
    {
      binlog_cache_mngr *const cache_mngr= thd->binlog_setup_trx_data();
      if (!cache_mngr)
        goto err;

      is_trans_cache= use_trans_cache(thd, using_trans);
      file= cache_mngr->get_binlog_cache_log(is_trans_cache);
      cache_data= cache_mngr->get_binlog_cache_data(is_trans_cache);

      if (thd->lex->stmt_accessed_non_trans_temp_table())
        cache_data->set_changes_to_non_trans_temp_table();

      thd->binlog_start_trans_and_stmt();
    }
    DBUG_PRINT("info",("event type: %d",event_info->get_type_code()));

    /*
       No check for auto events flag here - this write method should
       never be called if auto-events are enabled.

       Write first log events which describe the 'run environment'
       of the SQL command. If row-based binlogging, Insert_id, Rand
       and other kind of "setting context" events are not needed.
    */

    if (with_annotate && *with_annotate)
    {
      DBUG_ASSERT(event_info->get_type_code() == TABLE_MAP_EVENT);
      Annotate_rows_log_event anno(thd, using_trans, direct);
      /* Annotate event should be written not more than once */
      *with_annotate= 0;
      if (write_event(&anno, file))
        goto err;
    }

    if (thd)
    {
      if (!thd->is_current_stmt_binlog_format_row())
      {

        if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
        {
          Intvar_log_event e(thd,(uchar) LAST_INSERT_ID_EVENT,
                             thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                             using_trans, direct);
          if (write_event(&e, file))
            goto err;
        }
        if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
        {
          DBUG_PRINT("info",("number of auto_inc intervals: %u",
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             nb_elements()));
          Intvar_log_event e(thd, (uchar) INSERT_ID_EVENT,
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             minimum(), using_trans, direct);
          if (write_event(&e, file))
            goto err;
        }
        if (thd->rand_used)
        {
          Rand_log_event e(thd,thd->rand_saved_seed1,thd->rand_saved_seed2,
                           using_trans, direct);
          if (write_event(&e, file))
            goto err;
        }
        if (thd->user_var_events.elements)
        {
          for (uint i= 0; i < thd->user_var_events.elements; i++)
          {
            BINLOG_USER_VAR_EVENT *user_var_event;
            get_dynamic(&thd->user_var_events,(uchar*) &user_var_event, i);

            /* setting flags for user var log event */
            uchar flags= User_var_log_event::UNDEF_F;
            if (user_var_event->unsigned_flag)
              flags|= User_var_log_event::UNSIGNED_F;

            User_var_log_event e(thd, user_var_event->user_var_event->name.str,
                                 user_var_event->user_var_event->name.length,
                                 user_var_event->value,
                                 user_var_event->length,
                                 user_var_event->type,
                                 user_var_event->charset_number,
                                 flags,
                                 using_trans,
                                 direct);
            if (write_event(&e, file))
              goto err;
          }
        }
      }
    }

    /*
      Write the event.
    */
    if (write_event(event_info, file) ||
        DBUG_EVALUATE_IF("injecting_fault_writing", 1, 0))
      goto err;

    error= 0;
err:
    if (direct)
    {
      my_off_t offset= my_b_tell(file);
      bool check_purge= false;

      if (!error)
      {
        bool synced;

        if ((error= flush_and_sync(&synced)))
        {
        }
        else
        {
          mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
          mysql_mutex_assert_owner(&LOCK_log);
          mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
          mysql_mutex_assert_not_owner(&LOCK_commit_ordered);
          bool first= true;
          bool last= true;
          if ((error= RUN_HOOK(binlog_storage, after_flush,
                               (thd, log_file_name, file->pos_in_file,
                                synced, first, last))))
          {
            sql_print_error("Failed to run 'after_flush' hooks");
            error= 1;
          }
          else
          {
            /* update binlog_end_pos so it can be read by dump thread
             *
             * note: must be _after_ the RUN_HOOK(after_flush) or else
             * semi-sync-plugin might not have put the transaction into
             * it's list before dump-thread tries to send it
             */
            update_binlog_end_pos(offset);

            signal_update();
            if ((error= rotate(false, &check_purge)))
              check_purge= false;
          }
        }
      }

      status_var_add(thd->status_var.binlog_bytes_written,
                     offset - my_org_b_tell);

      mysql_mutex_lock(&LOCK_after_binlog_sync);
      mysql_mutex_unlock(&LOCK_log);

      mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
      mysql_mutex_assert_not_owner(&LOCK_log);
      mysql_mutex_assert_owner(&LOCK_after_binlog_sync);
      mysql_mutex_assert_not_owner(&LOCK_commit_ordered);
      bool first= true;
      bool last= true;
      if (RUN_HOOK(binlog_storage, after_sync,
                   (thd, log_file_name, file->pos_in_file,
                    first, last)))
      {
        error=1;
        /* error is already printed inside hook */
      }

      /*
        Take mutex to protect against a reader seeing partial writes of 64-bit
        offset on 32-bit CPUs.
      */
      mysql_mutex_lock(&LOCK_commit_ordered);
      mysql_mutex_unlock(&LOCK_after_binlog_sync);
      last_commit_pos_offset= offset;
      mysql_mutex_unlock(&LOCK_commit_ordered);

      if (check_purge)
        checkpoint_and_purge(prev_binlog_id);
    }

    if (error)
    {
      set_write_error(thd, is_trans_cache);
      if (check_write_error(thd) && cache_data &&
          stmt_has_updated_non_trans_table(thd))
        cache_data->set_incident();
    }
  }

  DBUG_RETURN(error);
}


int error_log_print(enum loglevel level, const char *format,
                    va_list args)
{
  return logger.error_log_print(level, format, args);
}


bool slow_log_print(THD *thd, const char *query, uint query_length,
                    ulonglong current_utime)
{
  return logger.slow_log_print(thd, query, query_length, current_utime);
}


bool LOGGER::log_command(THD *thd, enum enum_server_command command)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  /*
    Log command if we have at least one log event handler enabled and want
    to log this king of commands
  */
  if (*general_log_handler_list && (what_to_log & (1L << (uint) command)))
  {
    if ((thd->variables.option_bits & OPTION_LOG_OFF)
#ifndef NO_EMBEDDED_ACCESS_CHECKS
         && (sctx->master_access & SUPER_ACL)
#endif
       )
    {
      /* No logging */
      return FALSE;
    }

    return TRUE;
  }

  return FALSE;
}


bool general_log_print(THD *thd, enum enum_server_command command,
                       const char *format, ...)
{
  va_list args;
  uint error= 0;

  /* Print the message to the buffer if we want to log this king of commands */
  if (! logger.log_command(thd, command))
    return FALSE;

  va_start(args, format);
  error= logger.general_log_print(thd, command, format, args);
  va_end(args);

  return error;
}

bool general_log_write(THD *thd, enum enum_server_command command,
                       const char *query, uint query_length)
{
  /* Write the message to the log if we want to log this king of commands */
  if (logger.log_command(thd, command) || mysql_audit_general_enabled())
    return logger.general_log_write(thd, command, query, query_length);

  return FALSE;
}


static void
binlog_checkpoint_callback(void *cookie)
{
  MYSQL_BIN_LOG::xid_count_per_binlog *entry=
    (MYSQL_BIN_LOG::xid_count_per_binlog *)cookie;
  /*
    For every supporting engine, we increment the xid_count and issue a
    commit_checkpoint_request(). Then we can count when all
    commit_checkpoint_notify() callbacks have occurred, and then log a new
    binlog checkpoint event.
  */
  mysql_bin_log.mark_xids_active(entry->binlog_id, 1);
}


/*
  Request a commit checkpoint from each supporting engine.
  This must be called after each binlog rotate, and after LOCK_log has been
  released. The xid_count value in the xid_count_per_binlog entry was
  incremented by 1 and will be decremented in this function; this ensures
  that the entry will not go away early despite LOCK_log not being held.
*/
void
MYSQL_BIN_LOG::do_checkpoint_request(ulong binlog_id)
{
  xid_count_per_binlog *entry;

  /*
    Find the binlog entry, and invoke commit_checkpoint_request() on it in
    each supporting storage engine.
  */
  mysql_mutex_lock(&LOCK_xid_list);
  I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
  do {
    entry= it++;
    DBUG_ASSERT(entry /* binlog_id is always somewhere in the list. */);
  } while (entry->binlog_id != binlog_id);
  mysql_mutex_unlock(&LOCK_xid_list);

  ha_commit_checkpoint_request(entry, binlog_checkpoint_callback);
  /*
    When we rotated the binlog, we incremented xid_count to make sure the
    entry would not go away until this point, where we have done all necessary
    commit_checkpoint_request() calls.
    So now we can (and must) decrease the count - when it reaches zero, we
    will know that both all pending unlog() and all pending
    commit_checkpoint_notify() calls are done, and we can log a new binlog
    checkpoint.
  */
  mark_xid_done(binlog_id, true);
}


/**
  The method executes rotation when LOCK_log is already acquired
  by the caller.

  @param force_rotate  caller can request the log rotation
  @param check_purge   is set to true if rotation took place

  @note
    Caller _must_ check the check_purge variable. If this is set, it means
    that the binlog was rotated, and caller _must_ ensure that
    do_checkpoint_request() is called later with the binlog_id of the rotated
    binlog file. The call to do_checkpoint_request() must happen after
    LOCK_log is released (which is why we cannot simply do it here).
    Usually, checkpoint_and_purge() is appropriate, as it will both handle
    the checkpointing and any needed purging of old logs.

  @note
    If rotation fails, for instance the server was unable 
    to create a new log file, we still try to write an 
    incident event to the current log.

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate(bool force_rotate, bool* check_purge)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::rotate");

  if (wsrep_to_isolation)
  {
    DBUG_ASSERT(WSREP_ON);
    *check_purge= false;
    WSREP_DEBUG("avoiding binlog rotate due to TO isolation: %d", 
                wsrep_to_isolation);
    DBUG_RETURN(0);
  }

  //todo: fix the macro def and restore safe_mutex_assert_owner(&LOCK_log);
  *check_purge= false;

  if (force_rotate || (my_b_tell(&log_file) >= (my_off_t) max_size))
  {
    ulong binlog_id= current_binlog_id;
    /*
      We rotate the binlog, so we need to start a commit checkpoint in all
      supporting engines - when it finishes, we can log a new binlog checkpoint
      event.

      But we cannot start the checkpoint here - there could be a group commit
      still in progress which needs to be included in the checkpoint, and
      besides we do not want to do the (possibly expensive) checkpoint while
      LOCK_log is held.

      On the other hand, we must be sure that the xid_count entry for the
      previous log does not go away until we start the checkpoint - which it
      could do as it is no longer the most recent. So we increment xid_count
      (to count the pending checkpoint request) - this will fix the entry in
      place until we decrement again in do_checkpoint_request().
    */
    mark_xids_active(binlog_id, 1);

    if ((error= new_file_without_locking()))
    {
      /** 
         Be conservative... There are possible lost events (eg, 
         failing to log the Execute_load_query_log_event
         on a LOAD DATA while using a non-transactional
         table)!

         We give it a shot and try to write an incident event anyway
         to the current log. 
      */
      if (!write_incident_already_locked(current_thd))
        flush_and_sync(0);

      /*
        We failed to rotate - so we have to decrement the xid_count back that
        we incremented before attempting the rotate.
      */
      mark_xid_done(binlog_id, false);
    }
    else
      *check_purge= true;
  }
  DBUG_RETURN(error);
}

/**
  The method executes logs purging routine.

  @retval
    nonzero - error in rotating routine.
*/
void MYSQL_BIN_LOG::purge()
{
  mysql_mutex_assert_not_owner(&LOCK_log);
#ifdef HAVE_REPLICATION
  if (expire_logs_days)
  {
    DEBUG_SYNC(current_thd, "at_purge_logs_before_date");
    time_t purge_time= my_time(0) - expire_logs_days*24*60*60;
    if (purge_time >= 0)
    {
      purge_logs_before_date(purge_time);
    }
    DEBUG_SYNC(current_thd, "after_purge_logs_before_date");
  }
#endif
}


void MYSQL_BIN_LOG::checkpoint_and_purge(ulong binlog_id)
{
  do_checkpoint_request(binlog_id);
  purge();
}

/**
  The method is a shortcut of @c rotate() and @c purge().
  LOCK_log is acquired prior to rotate and is released after it.

  @param force_rotate  caller can request the log rotation

  @retval
    nonzero - error in rotating routine.
*/
int MYSQL_BIN_LOG::rotate_and_purge(bool force_rotate)
{
  int error= 0;
  ulong prev_binlog_id;
  DBUG_ENTER("MYSQL_BIN_LOG::rotate_and_purge");
  bool check_purge= false;

  mysql_mutex_lock(&LOCK_log);
  prev_binlog_id= current_binlog_id;
  if ((error= rotate(force_rotate, &check_purge)))
    check_purge= false;
  /*
    NOTE: Run purge_logs wo/ holding LOCK_log because it does not need
          the mutex. Otherwise causes various deadlocks.
  */
  mysql_mutex_unlock(&LOCK_log);

  if (check_purge)
    checkpoint_and_purge(prev_binlog_id);

  DBUG_RETURN(error);
}

uint MYSQL_BIN_LOG::next_file_id()
{
  uint res;
  mysql_mutex_lock(&LOCK_log);
  res = file_id++;
  mysql_mutex_unlock(&LOCK_log);
  return res;
}

class CacheWriter: public Log_event_writer
{
public:
  ulong remains;

  CacheWriter(THD *thd_arg, IO_CACHE *file_arg, bool do_checksum,
              Binlog_crypt_data *cr)
    : Log_event_writer(file_arg, cr), remains(0), thd(thd_arg), first(true)
  { checksum_len= do_checksum ? BINLOG_CHECKSUM_LEN : 0; }

  ~CacheWriter()
  { status_var_add(thd->status_var.binlog_bytes_written, bytes_written); }

  int write(uchar* pos, size_t len)
  {
    if (first)
      write_header(pos, len);
    else
      write_data(pos, len);

    remains -= len;
    if ((first= !remains))
      write_footer();
    return 0;
  }
private:
  THD *thd;
  bool first;
};

/*
  Write the contents of a cache to the binary log.

  SYNOPSIS
    write_cache()
    thd      Current_thread
    cache    Cache to write to the binary log

  DESCRIPTION
    Write the contents of the cache to the binary log. The cache will
    be reset as a READ_CACHE to be able to read the contents from it.

    Reading from the trans cache with possible (per @c binlog_checksum_options) 
    adding checksum value  and then fixing the length and the end_log_pos of 
    events prior to fill in the binlog cache.
*/

int MYSQL_BIN_LOG::write_cache(THD *thd, IO_CACHE *cache)
{
  DBUG_ENTER("MYSQL_BIN_LOG::write_cache");

  mysql_mutex_assert_owner(&LOCK_log);
  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  uint length= my_b_bytes_in_cache(cache), group, carry, hdr_offs;
  long val;
  ulong end_log_pos_inc= 0; // each event processed adds BINLOG_CHECKSUM_LEN 2 t
  uchar header[LOG_EVENT_HEADER_LEN];
  CacheWriter writer(thd, &log_file, binlog_checksum_options, &crypto);

  if (crypto.scheme)
    writer.ctx= alloca(crypto.ctx_size);

  // while there is just one alg the following must hold:
  DBUG_ASSERT(binlog_checksum_options == BINLOG_CHECKSUM_ALG_OFF ||
              binlog_checksum_options == BINLOG_CHECKSUM_ALG_CRC32);

  /*
    The events in the buffer have incorrect end_log_pos data
    (relative to beginning of group rather than absolute),
    so we'll recalculate them in situ so the binlog is always
    correct, even in the middle of a group. This is possible
    because we now know the start position of the group (the
    offset of this cache in the log, if you will); all we need
    to do is to find all event-headers, and add the position of
    the group to the end_log_pos of each event.  This is pretty
    straight forward, except that we read the cache in segments,
    so an event-header might end up on the cache-border and get
    split.
  */

  group= (uint)my_b_tell(&log_file);
  hdr_offs= carry= 0;

  do
  {
    /*
      if we only got a partial header in the last iteration,
      get the other half now and process a full header.
    */
    if (unlikely(carry > 0))
    {
      DBUG_ASSERT(carry < LOG_EVENT_HEADER_LEN);
      uint tail= LOG_EVENT_HEADER_LEN - carry;

      /* assemble both halves */
      memcpy(&header[carry], (char *)cache->read_pos, tail);

      ulong len= uint4korr(header + EVENT_LEN_OFFSET);
      writer.remains= len;

      /* fix end_log_pos */
      end_log_pos_inc += writer.checksum_len;
      val= uint4korr(header + LOG_POS_OFFSET) + group + end_log_pos_inc;
      int4store(header + LOG_POS_OFFSET, val);

      /* fix len */
      len+= writer.checksum_len;
      int4store(header + EVENT_LEN_OFFSET, len);

      if (writer.write(header, LOG_EVENT_HEADER_LEN))
        DBUG_RETURN(ER_ERROR_ON_WRITE);

      cache->read_pos+= tail;
      length-= tail;
      carry= 0;

      /* next event header at ... */
      hdr_offs= len - LOG_EVENT_HEADER_LEN - writer.checksum_len;
    }

    /* if there is anything to write, process it. */

    if (likely(length > 0))
    {
      DBUG_EXECUTE_IF("fail_binlog_write_1",
                      errno= 28; DBUG_RETURN(ER_ERROR_ON_WRITE););
      /*
        process all event-headers in this (partial) cache.
        if next header is beyond current read-buffer,
        we'll get it later (though not necessarily in the
        very next iteration, just "eventually").
      */

      if (hdr_offs >= length)
      {
        if (writer.write(cache->read_pos, length))
          DBUG_RETURN(ER_ERROR_ON_WRITE);
      }

      while (hdr_offs < length)
      {
        /*
          finish off with remains of the last event that crawls
          from previous into the current buffer
        */
        if (writer.remains != 0)
        {
          if (writer.write(cache->read_pos, hdr_offs))
            DBUG_RETURN(ER_ERROR_ON_WRITE);
        }

        /*
          partial header only? save what we can get, process once
          we get the rest.
        */
        if (hdr_offs + LOG_EVENT_HEADER_LEN > length)
        {
          carry= length - hdr_offs;
          memcpy(header, (char *)cache->read_pos + hdr_offs, carry);
          length= hdr_offs;
        }
        else
        {
          /* we've got a full event-header, and it came in one piece */
          uchar *ev= (uchar *)cache->read_pos + hdr_offs;
          uint ev_len= uint4korr(ev + EVENT_LEN_OFFSET); // netto len
          uchar *log_pos= ev + LOG_POS_OFFSET;

          end_log_pos_inc += writer.checksum_len;
          /* fix end_log_pos */
          val= uint4korr(log_pos) + group + end_log_pos_inc;
          int4store(log_pos, val);

          /* fix length */
          int4store(ev + EVENT_LEN_OFFSET, ev_len + writer.checksum_len);

          writer.remains= ev_len;
          if (writer.write(ev, std::min<uint>(ev_len, length - hdr_offs)))
            DBUG_RETURN(ER_ERROR_ON_WRITE);

          /* next event header at ... */
          hdr_offs += ev_len; // incr by the netto len

          DBUG_ASSERT(!writer.checksum_len || writer.remains == 0 || hdr_offs >= length);
        }
      }

      /*
        Adjust hdr_offs. Note that it may still point beyond the segment
        read in the next iteration; if the current event is very long,
        it may take a couple of read-iterations (and subsequent adjustments
        of hdr_offs) for it to point into the then-current segment.
        If we have a split header (!carry), hdr_offs will be set at the
        beginning of the next iteration, overwriting the value we set here:
      */
      hdr_offs -= length;
    }
  } while ((length= my_b_fill(cache)));

  DBUG_ASSERT(carry == 0);
  DBUG_ASSERT(!writer.checksum_len || writer.remains == 0);

  DBUG_RETURN(0);                               // All OK
}

/*
  Helper function to get the error code of the query to be binlogged.
 */
int query_error_code(THD *thd, bool not_killed)
{
  int error;
  
  if (not_killed || (killed_mask_hard(thd->killed) == KILL_BAD_DATA))
  {
    error= thd->is_error() ? thd->get_stmt_da()->sql_errno() : 0;

    /* thd->get_get_stmt_da()->sql_errno() might be ER_SERVER_SHUTDOWN or
       ER_QUERY_INTERRUPTED, So here we need to make sure that error
       is not set to these errors when specified not_killed by the
       caller.
    */
    if (error == ER_SERVER_SHUTDOWN || error == ER_QUERY_INTERRUPTED ||
        error == ER_NEW_ABORTING_CONNECTION || error == ER_CONNECTION_KILLED)
      error= 0;
  }
  else
  {
    /* killed status for DELAYED INSERT thread should never be used */
    DBUG_ASSERT(!(thd->system_thread & SYSTEM_THREAD_DELAYED_INSERT));
    error= thd->killed_errno();
  }

  return error;
}


bool MYSQL_BIN_LOG::write_incident_already_locked(THD *thd)
{
  uint error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::write_incident_already_locked");
  Incident incident= INCIDENT_LOST_EVENTS;
  Incident_log_event ev(thd, incident, write_error_msg);

  if (likely(is_open()))
  {
    error= write_event(&ev);
    status_var_add(thd->status_var.binlog_bytes_written, ev.data_written);
  }

  DBUG_RETURN(error);
}


bool MYSQL_BIN_LOG::write_incident(THD *thd)
{
  uint error= 0;
  my_off_t offset;
  bool check_purge= false;
  ulong prev_binlog_id;
  DBUG_ENTER("MYSQL_BIN_LOG::write_incident");

  mysql_mutex_lock(&LOCK_log);
  if (likely(is_open()))
  {
    prev_binlog_id= current_binlog_id;
    if (!(error= write_incident_already_locked(thd)) &&
        !(error= flush_and_sync(0)))
    {
      signal_update();
      if ((error= rotate(false, &check_purge)))
        check_purge= false;
    }

    offset= my_b_tell(&log_file);

    update_binlog_end_pos(offset);

    /*
      Take mutex to protect against a reader seeing partial writes of 64-bit
      offset on 32-bit CPUs.
    */
    mysql_mutex_lock(&LOCK_commit_ordered);
    last_commit_pos_offset= offset;
    mysql_mutex_unlock(&LOCK_commit_ordered);
    mysql_mutex_unlock(&LOCK_log);

    if (check_purge)
      checkpoint_and_purge(prev_binlog_id);
  }
  else
  {
    mysql_mutex_unlock(&LOCK_log);
  }

  DBUG_RETURN(error);
}

void
MYSQL_BIN_LOG::write_binlog_checkpoint_event_already_locked(const char *name_arg, uint len)
{
  my_off_t offset;
  Binlog_checkpoint_log_event ev(name_arg, len);
  /*
    Note that we must sync the binlog checkpoint to disk.
    Otherwise a subsequent log purge could delete binlogs that XA recovery
    thinks are needed (even though they are not really).
  */
  if (!write_event(&ev) && !flush_and_sync(0))
  {
    signal_update();
  }
  else
  {
    /*
      If we fail to write the checkpoint event, something is probably really
      bad with the binlog. We complain in the error log.

      Note that failure to write binlog checkpoint does not compromise the
      ability to do crash recovery - crash recovery will just have to scan a
      bit more of the binlog than strictly necessary.
    */
    sql_print_error("Failed to write binlog checkpoint event to binary log\n");
  }

  offset= my_b_tell(&log_file);

  update_binlog_end_pos(offset);

  /*
    Take mutex to protect against a reader seeing partial writes of 64-bit
    offset on 32-bit CPUs.
  */
  mysql_mutex_lock(&LOCK_commit_ordered);
  last_commit_pos_offset= offset;
  mysql_mutex_unlock(&LOCK_commit_ordered);
}


/**
  Write a cached log entry to the binary log.
  - To support transaction over replication, we wrap the transaction
  with BEGIN/COMMIT or BEGIN/ROLLBACK in the binary log.
  We want to write a BEGIN/ROLLBACK block when a non-transactional table
  was updated in a transaction which was rolled back. This is to ensure
  that the same updates are run on the slave.

  @param thd
  @param cache		The cache to copy to the binlog
  @param commit_event   The commit event to print after writing the
                        contents of the cache.
  @param incident       Defines if an incident event should be created to
                        notify that some non-transactional changes did
                        not get into the binlog.

  @note
    We only come here if there is something in the cache.
  @note
    The thing in the cache is always a complete transaction.
  @note
    'cache' needs to be reinitialized after this functions returns.
*/

bool
MYSQL_BIN_LOG::write_transaction_to_binlog(THD *thd,
                                           binlog_cache_mngr *cache_mngr,
                                           Log_event *end_ev, bool all,
                                           bool using_stmt_cache,
                                           bool using_trx_cache)
{
  group_commit_entry entry;
  Ha_trx_info *ha_info;
  DBUG_ENTER("MYSQL_BIN_LOG::write_transaction_to_binlog");

  /*
    Control should not be allowed beyond this point in wsrep_emulate_bin_log
    mode. Also, do not write the cached updates to binlog if binary logging is
    disabled (log-bin/sql_log_bin).
  */
  if (wsrep_emulate_bin_log || !(thd->variables.option_bits & OPTION_BIN_LOG))
    DBUG_RETURN(0);

  entry.thd= thd;
  entry.cache_mngr= cache_mngr;
  entry.error= 0;
  entry.all= all;
  entry.using_stmt_cache= using_stmt_cache;
  entry.using_trx_cache= using_trx_cache;
  entry.need_unlog= false;
  ha_info= all ? thd->transaction.all.ha_list : thd->transaction.stmt.ha_list;

  for (; ha_info; ha_info= ha_info->next())
  {
    if (ha_info->is_started() && ha_info->ht() != binlog_hton &&
        !ha_info->ht()->commit_checkpoint_request)
      entry.need_unlog= true;
    break;
  }

  entry.end_event= end_ev;
  if (cache_mngr->stmt_cache.has_incident() ||
      cache_mngr->trx_cache.has_incident())
  {
    Incident_log_event inc_ev(thd, INCIDENT_LOST_EVENTS, write_error_msg);
    entry.incident_event= &inc_ev;
    DBUG_RETURN(write_transaction_to_binlog_events(&entry));
  }
  else
  {
    entry.incident_event= NULL;
    DBUG_RETURN(write_transaction_to_binlog_events(&entry));
  }
}


/*
  Put a transaction that is ready to commit in the group commit queue.
  The transaction is identified by the ENTRY object passed into this function.

  To facilitate group commit for the binlog, we first queue up ourselves in
  this function. Then later the first thread to enter the queue waits for
  the LOCK_log mutex, and commits for everyone in the queue once it gets the
  lock. Any other threads in the queue just wait for the first one to finish
  the commit and wake them up. This way, all transactions in the queue get
  committed in a single disk operation.

  The main work in this function is when the commit in one transaction has
  been marked to wait for the commit of another transaction to happen
  first. This is used to support in-order parallel replication, where
  transactions can execute out-of-order but need to be committed in-order with
  how they happened on the master. The waiting of one commit on another needs
  to be integrated with the group commit queue, to ensure that the waiting
  transaction can participate in the same group commit as the waited-for
  transaction.

  So when we put a transaction in the queue, we check if there were other
  transactions already prepared to commit but just waiting for the first one
  to commit. If so, we add those to the queue as well, transitively for all
  waiters.

  And if a transaction is marked to wait for a prior transaction, but that
  prior transaction is already queued for group commit, then we can queue the
  new transaction directly to participate in the group commit.

  @retval < 0   Error
  @retval > 0   If queued as the first entry in the queue (meaning this
                is the leader)
  @retval   0   Otherwise (queued as participant, leader handles the commit)
*/

int
MYSQL_BIN_LOG::queue_for_group_commit(group_commit_entry *orig_entry)
{
  group_commit_entry *entry, *orig_queue, *last;
  wait_for_commit *cur;
  wait_for_commit *wfc;
  DBUG_ENTER("MYSQL_BIN_LOG::queue_for_group_commit");

  /*
    Check if we need to wait for another transaction to commit before us.

    It is safe to do a quick check without lock first in the case where we do
    not have to wait. But if the quick check shows we need to wait, we must do
    another safe check under lock, to avoid the race where the other
    transaction wakes us up between the check and the wait.
  */
  wfc= orig_entry->thd->wait_for_commit_ptr;
  orig_entry->queued_by_other= false;
  if (wfc && wfc->waitee)
  {
    mysql_mutex_lock(&wfc->LOCK_wait_commit);
    /*
      Do an extra check here, this time safely under lock.

      If waitee->commit_started is set, it means that the transaction we need
      to wait for has already queued up for group commit. In this case it is
      safe for us to queue up immediately as well, increasing the opprtunities
      for group commit. Because waitee has taken the LOCK_prepare_ordered
      before setting the flag, so there is no risk that we can queue ahead of
      it.
    */
    if (wfc->waitee && !wfc->waitee->commit_started)
    {
      PSI_stage_info old_stage;
      wait_for_commit *loc_waitee;

      /*
        By setting wfc->opaque_pointer to our own entry, we mark that we are
        ready to commit, but waiting for another transaction to commit before
        us.

        This other transaction may then take over the commit process for us to
        get us included in its own group commit. If this happens, the
        queued_by_other flag is set.

        Setting this flag may or may not be seen by the other thread, but we
        are safe in any case: The other thread will set queued_by_other under
        its LOCK_wait_commit, and we will not check queued_by_other only after
        we have been woken up.
      */
      wfc->opaque_pointer= orig_entry;
      DEBUG_SYNC(orig_entry->thd, "group_commit_waiting_for_prior");
      orig_entry->thd->ENTER_COND(&wfc->COND_wait_commit,
                                  &wfc->LOCK_wait_commit,
                                  &stage_waiting_for_prior_transaction_to_commit,
                                  &old_stage);
      while ((loc_waitee= wfc->waitee) && !orig_entry->thd->check_killed())
        mysql_cond_wait(&wfc->COND_wait_commit, &wfc->LOCK_wait_commit);
      wfc->opaque_pointer= NULL;
      DBUG_PRINT("info", ("After waiting for prior commit, queued_by_other=%d",
                 orig_entry->queued_by_other));

      if (loc_waitee)
      {
        /* Wait terminated due to kill. */
        mysql_mutex_lock(&loc_waitee->LOCK_wait_commit);
        if (loc_waitee->wakeup_subsequent_commits_running ||
            orig_entry->queued_by_other)
        {
          /* Our waitee is already waking us up, so ignore the kill. */
          mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
          do
          {
            mysql_cond_wait(&wfc->COND_wait_commit, &wfc->LOCK_wait_commit);
          } while (wfc->waitee);
        }
        else
        {
          /* We were killed, so remove us from the list of waitee. */
          wfc->remove_from_list(&loc_waitee->subsequent_commits_list);
          mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
          wfc->waitee= NULL;

          orig_entry->thd->EXIT_COND(&old_stage);
          /* Interrupted by kill. */
          DEBUG_SYNC(orig_entry->thd, "group_commit_waiting_for_prior_killed");
          wfc->wakeup_error= orig_entry->thd->killed_errno();
          if (!wfc->wakeup_error)
            wfc->wakeup_error= ER_QUERY_INTERRUPTED;
          my_message(wfc->wakeup_error,
                     ER_THD(orig_entry->thd, wfc->wakeup_error), MYF(0));
          DBUG_RETURN(-1);
        }
      }
      orig_entry->thd->EXIT_COND(&old_stage);
    }
    else
      mysql_mutex_unlock(&wfc->LOCK_wait_commit);
  }
  /*
    If the transaction we were waiting for has already put us into the group
    commit queue (and possibly already done the entire binlog commit for us),
    then there is nothing else to do.
  */
  if (orig_entry->queued_by_other)
    DBUG_RETURN(0);

  if (wfc && wfc->wakeup_error)
  {
    my_error(ER_PRIOR_COMMIT_FAILED, MYF(0));
    DBUG_RETURN(-1);
  }

  /* Now enqueue ourselves in the group commit queue. */
  DEBUG_SYNC(orig_entry->thd, "commit_before_enqueue");
  orig_entry->thd->clear_wakeup_ready();
  mysql_mutex_lock(&LOCK_prepare_ordered);
  orig_queue= group_commit_queue;

  /*
    Iteratively process everything added to the queue, looking for waiters,
    and their waiters, and so on. If a waiter is ready to commit, we
    immediately add it to the queue, and mark it as queued_by_other.

    This would be natural to do with recursion, but we want to avoid
    potentially unbounded recursion blowing the C stack, so we use the list
    approach instead.

    We keep a list of the group_commit_entry of all the waiters that need to
    be processed. Initially this list contains only the entry passed into this
    function.

    We process entries in the list one by one. The element currently being
    processed is pointed to by `entry`, and the element at the end of the list
    is pointed to by `last` (we do not use NULL to terminate the list).

    As we process an entry, any waiters for that entry are added at the end of
    the list, to be processed in subsequent iterations. The the entry is added
    to the group_commit_queue.  This continues until the list is exhausted,
    with all entries ever added eventually processed.

    The end result is a breath-first traversal of the tree of waiters,
    re-using the `next' pointers of the group_commit_entry objects in place of
    extra stack space in a recursive traversal.

    The temporary list linked through these `next' pointers is not used by the
    caller or any other function; it only exists while doing the iterative
    tree traversal. After, all the processed entries are linked into the
    group_commit_queue.
  */

  cur= wfc;
  last= orig_entry;
  entry= orig_entry;
  for (;;)
  {
    group_commit_entry *next_entry;

    if (entry->cache_mngr->using_xa)
    {
      DEBUG_SYNC(entry->thd, "commit_before_prepare_ordered");
      run_prepare_ordered(entry->thd, entry->all);
      DEBUG_SYNC(entry->thd, "commit_after_prepare_ordered");
    }

    if (cur)
    {
      /*
        Now that we have taken LOCK_prepare_ordered and will queue up in the
        group commit queue, it is safe for following transactions to queue
        themselves. We will grab here any transaction that is now ready to
        queue up, but after that, more transactions may become ready while the
        leader is waiting to start the group commit. So set the flag
        `commit_started', so that later transactions can still participate in
        the group commit..
      */
      cur->commit_started= true;

      /*
        Check if this transaction has other transaction waiting for it to
        commit.

        If so, process the waiting transactions, and their waiters and so on,
        transitively.
      */
      if (cur->subsequent_commits_list)
      {
        wait_for_commit *waiter, **waiter_ptr;

        mysql_mutex_lock(&cur->LOCK_wait_commit);
        /*
          Grab the list, now safely under lock, and process it if still
          non-empty.
        */
        waiter= cur->subsequent_commits_list;
        waiter_ptr= &cur->subsequent_commits_list;
        while (waiter)
        {
          wait_for_commit *next_waiter= waiter->next_subsequent_commit;
          group_commit_entry *entry2=
            (group_commit_entry *)waiter->opaque_pointer;
          if (entry2)
          {
            /*
              This is another transaction ready to be written to the binary
              log. We can put it into the queue directly, without needing a
              separate context switch to the other thread. We just set a flag
              so that the other thread will know when it wakes up that it was
              already processed.

              So remove it from the list of our waiters, and instead put it at
              the end of the list to be processed in a subsequent iteration of
              the outer loop.
            */
            *waiter_ptr= next_waiter;
            entry2->queued_by_other= true;
            last->next= entry2;
            last= entry2;
            /*
              As a small optimisation, we do not actually need to set
              entry2->next to NULL, as we can use the pointer `last' to check
              for end-of-list.
            */
          }
          else
          {
            /*
              This transaction is not ready to participate in the group commit
              yet, so leave it in the waiter list. It might join the group
              commit later, if it completes soon enough to do so (it will see
              our wfc->commit_started flag set), or it might commit later in a
              later group commit.
            */
            waiter_ptr= &waiter->next_subsequent_commit;
          }
          waiter= next_waiter;
        }
        mysql_mutex_unlock(&cur->LOCK_wait_commit);
      }
    }

    /*
      Handle the heuristics that if another transaction is waiting for this
      transaction (or if it does so later), then we want to trigger group
      commit immediately, without waiting for the binlog_commit_wait_usec
      timeout to expire.
    */
    entry->thd->waiting_on_group_commit= true;

    /* Add the entry to the group commit queue. */
    next_entry= entry->next;
    entry->next= group_commit_queue;
    group_commit_queue= entry;
    if (entry == last)
      break;
    /*
      Move to the next entry in the flattened list of waiting transactions
      that still need to be processed transitively.
    */
    entry= next_entry;
    DBUG_ASSERT(entry != NULL);
    cur= entry->thd->wait_for_commit_ptr;
  }

  if (opt_binlog_commit_wait_count > 0 && orig_queue != NULL)
    mysql_cond_signal(&COND_prepare_ordered);
  mysql_mutex_unlock(&LOCK_prepare_ordered);
  DEBUG_SYNC(orig_entry->thd, "commit_after_release_LOCK_prepare_ordered");

  DBUG_PRINT("info", ("Queued for group commit as %s\n",
                      (orig_queue == NULL) ? "leader" : "participant"));
  DBUG_RETURN(orig_queue == NULL);
}

bool
MYSQL_BIN_LOG::write_transaction_to_binlog_events(group_commit_entry *entry)
{
  int is_leader= queue_for_group_commit(entry);

  /*
    The first in the queue handles group commit for all; the others just wait
    to be signalled when group commit is done.
  */
  if (is_leader < 0)
    return true;                                /* Error */
  else if (is_leader)
    trx_group_commit_leader(entry);
  else if (!entry->queued_by_other)
    entry->thd->wait_for_wakeup_ready();
  else
  {
    /*
      If we were queued by another prior commit, then we are woken up
      only when the leader has already completed the commit for us.
      So nothing to do here then.
    */
  }

  if (!opt_optimize_thread_scheduling)
  {
    /* For the leader, trx_group_commit_leader() already took the lock. */
    if (!is_leader)
      mysql_mutex_lock(&LOCK_commit_ordered);

    DEBUG_SYNC(entry->thd, "commit_loop_entry_commit_ordered");
    ++num_commits;
    if (entry->cache_mngr->using_xa && !entry->error)
      run_commit_ordered(entry->thd, entry->all);

    group_commit_entry *next= entry->next;
    if (!next)
    {
      group_commit_queue_busy= FALSE;
      mysql_cond_signal(&COND_queue_busy);
      DEBUG_SYNC(entry->thd, "commit_after_group_run_commit_ordered");
    }
    mysql_mutex_unlock(&LOCK_commit_ordered);
    entry->thd->wakeup_subsequent_commits(entry->error);

    if (next)
    {
      /*
        Wake up the next thread in the group commit.

        The next thread can be waiting in two different ways, depending on
        whether it put itself in the queue, or if it was put in queue by us
        because it had to wait for us to commit first.

        So execute the appropriate wakeup, identified by the queued_by_other
        field.
      */
      if (next->queued_by_other)
        next->thd->wait_for_commit_ptr->wakeup(entry->error);
      else
        next->thd->signal_wakeup_ready();
    }
    else
    {
      /*
        If we rotated the binlog, and if we are using the unoptimized thread
        scheduling where every thread runs its own commit_ordered(), then we
        must do the commit checkpoint and log purge here, after all
        commit_ordered() calls have finished, and locks have been released.
      */
      if (entry->check_purge)
        checkpoint_and_purge(entry->binlog_id);
    }

  }

  if (likely(!entry->error))
    return entry->thd->wait_for_prior_commit();

  switch (entry->error)
  {
  case ER_ERROR_ON_WRITE:
    my_error(ER_ERROR_ON_WRITE, MYF(ME_NOREFRESH), name, entry->commit_errno);
    break;
  case ER_ERROR_ON_READ:
    my_error(ER_ERROR_ON_READ, MYF(ME_NOREFRESH),
             entry->error_cache->file_name, entry->commit_errno);
    break;
  default:
    /*
      There are not (and should not be) any errors thrown not covered above.
      But just in case one is added later without updating the above switch
      statement, include a catch-all.
    */
    my_printf_error(entry->error,
                    "Error writing transaction to binary log: %d",
                    MYF(ME_NOREFRESH), entry->error);
  }

  /*
    Since we return error, this transaction XID will not be committed, so
    we need to mark it as not needed for recovery (unlog() is not called
    for a transaction if log_xid() fails).
  */
  if (entry->cache_mngr->using_xa && entry->cache_mngr->xa_xid &&
      entry->cache_mngr->need_unlog)
    mark_xid_done(entry->cache_mngr->binlog_id, true);

  return 1;
}

/*
  Do binlog group commit as the lead thread.

  This must be called when this statement/transaction is queued at the start of
  the group_commit_queue. It will wait to obtain the LOCK_log mutex, then group
  commit all the transactions in the queue (more may have entered while waiting
  for LOCK_log). After commit is done, all other threads in the queue will be
  signalled.

 */
void
MYSQL_BIN_LOG::trx_group_commit_leader(group_commit_entry *leader)
{
  uint xid_count= 0;
  my_off_t UNINIT_VAR(commit_offset);
  group_commit_entry *current, *last_in_queue;
  group_commit_entry *queue= NULL;
  bool check_purge= false;
  ulong UNINIT_VAR(binlog_id);
  uint64 commit_id;
  DBUG_ENTER("MYSQL_BIN_LOG::trx_group_commit_leader");

  {
    DBUG_EXECUTE_IF("inject_binlog_commit_before_get_LOCK_log",
      DBUG_ASSERT(!debug_sync_set_action(leader->thd, STRING_WITH_LEN
        ("commit_before_get_LOCK_log SIGNAL waiting WAIT_FOR cont TIMEOUT 1")));
    );
    /*
      Lock the LOCK_log(), and once we get it, collect any additional writes
      that queued up while we were waiting.
    */
    DEBUG_SYNC(leader->thd, "commit_before_get_LOCK_log");
    mysql_mutex_lock(&LOCK_log);
    DEBUG_SYNC(leader->thd, "commit_after_get_LOCK_log");

    mysql_mutex_lock(&LOCK_prepare_ordered);
    if (opt_binlog_commit_wait_count)
      wait_for_sufficient_commits();
    /*
      Note that wait_for_sufficient_commits() may have released and
      re-acquired the LOCK_log and LOCK_prepare_ordered if it needed to wait.
    */
    current= group_commit_queue;
    group_commit_queue= NULL;
    mysql_mutex_unlock(&LOCK_prepare_ordered);
    binlog_id= current_binlog_id;

    /* As the queue is in reverse order of entering, reverse it. */
    last_in_queue= current;
    while (current)
    {
      group_commit_entry *next= current->next;
      /*
        Now that group commit is started, we can clear the flag; there is no
        longer any use in waiters on this commit trying to trigger it early.
      */
      current->thd->waiting_on_group_commit= false;
      current->next= queue;
      queue= current;
      current= next;
    }
    DBUG_ASSERT(leader == queue /* the leader should be first in queue */);

    /* Now we have in queue the list of transactions to be committed in order. */
  }
    
  DBUG_ASSERT(is_open());
  if (likely(is_open()))                       // Should always be true
  {
    commit_id= (last_in_queue == leader ? 0 : (uint64)leader->thd->query_id);
    DBUG_EXECUTE_IF("binlog_force_commit_id",
      {
        const LEX_STRING commit_name= { C_STRING_WITH_LEN("commit_id") };
        bool null_value;
        user_var_entry *entry=
          (user_var_entry*) my_hash_search(&leader->thd->user_vars,
                                           (uchar*) commit_name.str,
                                           commit_name.length);
        commit_id= entry->val_int(&null_value);
      });
    /*
      Commit every transaction in the queue.

      Note that we are doing this in a different thread than the one running
      the transaction! So we are limited in the operations we can do. In
      particular, we cannot call my_error() on behalf of a transaction, as
      that obtains the THD from thread local storage. Instead, we must set
      current->error and let the thread do the error reporting itself once
      we wake it up.
    */
    for (current= queue; current != NULL; current= current->next)
    {
      binlog_cache_mngr *cache_mngr= current->cache_mngr;

      /*
        We already checked before that at least one cache is non-empty; if both
        are empty we would have skipped calling into here.
      */
      DBUG_ASSERT(!cache_mngr->stmt_cache.empty() || !cache_mngr->trx_cache.empty());

      if ((current->error= write_transaction_or_stmt(current, commit_id)))
        current->commit_errno= errno;

      strmake_buf(cache_mngr->last_commit_pos_file, log_file_name);
      commit_offset= my_b_write_tell(&log_file);
      cache_mngr->last_commit_pos_offset= commit_offset;
      if (cache_mngr->using_xa && cache_mngr->xa_xid)
      {
        /*
          If all storage engines support commit_checkpoint_request(), then we
          do not need to keep track of when this XID is durably committed.
          Instead we will just ask the storage engine to durably commit all its
          XIDs when we rotate a binlog file.
        */
        if (current->need_unlog)
        {
          xid_count++;
          cache_mngr->need_unlog= true;
          cache_mngr->binlog_id= binlog_id;
        }
        else
          cache_mngr->need_unlog= false;

        cache_mngr->delayed_error= false;
      }
    }

    bool synced= 0;
    if (flush_and_sync(&synced))
    {
      for (current= queue; current != NULL; current= current->next)
      {
        if (!current->error)
        {
          current->error= ER_ERROR_ON_WRITE;
          current->commit_errno= errno;
          current->error_cache= NULL;
        }
      }
    }
    else
    {
      bool any_error= false;
      bool all_error= true;

      mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
      mysql_mutex_assert_owner(&LOCK_log);
      mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
      mysql_mutex_assert_not_owner(&LOCK_commit_ordered);
      bool first= true, last;
      for (current= queue; current != NULL; current= current->next)
      {
        last= current->next == NULL;
        if (!current->error &&
            RUN_HOOK(binlog_storage, after_flush,
                (current->thd,
                 current->cache_mngr->last_commit_pos_file,
                 current->cache_mngr->last_commit_pos_offset, synced,
                 first, last)))
        {
          current->error= ER_ERROR_ON_WRITE;
          current->commit_errno= -1;
          current->error_cache= NULL;
          any_error= true;
        }
        else
          all_error= false;
        first= false;
      }

      /* update binlog_end_pos so it can be read by dump thread
       *
       * note: must be _after_ the RUN_HOOK(after_flush) or else
       * semi-sync-plugin might not have put the transaction into
       * it's list before dump-thread tries to send it
       */
      update_binlog_end_pos(commit_offset);

      if (any_error)
        sql_print_error("Failed to run 'after_flush' hooks");
      if (!all_error)
        signal_update();
    }

    /*
      If any commit_events are Xid_log_event, increase the number of pending
      XIDs in current binlog (it's decreased in ::unlog()). When the count in
      a (not active) binlog file reaches zero, we know that it is no longer
      needed in XA recovery, and we can log a new binlog checkpoint event.
    */
    if (xid_count > 0)
    {
      mark_xids_active(binlog_id, xid_count);
    }

    if (rotate(false, &check_purge))
    {
      /*
        If we fail to rotate, which thread should get the error?
        We give the error to the leader, as any my_error() thrown inside
        rotate() will have been registered for the leader THD.

        However we must not return error from here - that would cause
        ha_commit_trans() to abort and rollback the transaction, which would
        leave an inconsistent state with the transaction committed in the
        binlog but rolled back in the engine.

        Instead set a flag so that we can return error later, from unlog(),
        when the transaction has been safely committed in the engine.
      */
      leader->cache_mngr->delayed_error= true;
      my_error(ER_ERROR_ON_WRITE, MYF(ME_NOREFRESH), name, errno);
      check_purge= false;
    }
    /* In case of binlog rotate, update the correct current binlog offset. */
    commit_offset= my_b_write_tell(&log_file);
  }

  DEBUG_SYNC(leader->thd, "commit_before_get_LOCK_after_binlog_sync");
  mysql_mutex_lock(&LOCK_after_binlog_sync);
  /*
    We cannot unlock LOCK_log until we have locked LOCK_after_binlog_sync;
    otherwise scheduling could allow the next group commit to run ahead of us,
    messing up the order of commit_ordered() calls. But as soon as
    LOCK_after_binlog_sync is obtained, we can let the next group commit start.
  */
  mysql_mutex_unlock(&LOCK_log);

  DEBUG_SYNC(leader->thd, "commit_after_release_LOCK_log");

  /*
    Loop through threads and run the binlog_sync hook
  */
  {
    mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
    mysql_mutex_assert_not_owner(&LOCK_log);
    mysql_mutex_assert_owner(&LOCK_after_binlog_sync);
    mysql_mutex_assert_not_owner(&LOCK_commit_ordered);

    bool first= true, last;
    for (current= queue; current != NULL; current= current->next)
    {
      last= current->next == NULL;
      if (!current->error &&
          RUN_HOOK(binlog_storage, after_sync,
                   (current->thd, log_file_name,
                    current->cache_mngr->last_commit_pos_offset,
                    first, last)))
      {
      /* error is already printed inside hook */
      }
      first= false;
    }
  }

  DEBUG_SYNC(leader->thd, "commit_before_get_LOCK_commit_ordered");
  mysql_mutex_lock(&LOCK_commit_ordered);
  last_commit_pos_offset= commit_offset;

  /*
    Unlock LOCK_after_binlog_sync only *after* LOCK_commit_ordered has been
    acquired so that groups can not reorder for the different stages of
    the group commit procedure.
  */
  mysql_mutex_unlock(&LOCK_after_binlog_sync);
  DEBUG_SYNC(leader->thd, "commit_after_release_LOCK_after_binlog_sync");
  ++num_group_commits;

  if (!opt_optimize_thread_scheduling)
  {
    /*
      If we want to run commit_ordered() each in the transaction's own thread
      context, then we need to mark the queue reserved; we need to finish all
      threads in one group commit before the next group commit can be allowed
      to proceed, and we cannot unlock a simple pthreads mutex in a different
      thread from the one that locked it.
    */

    while (group_commit_queue_busy)
      mysql_cond_wait(&COND_queue_busy, &LOCK_commit_ordered);
    group_commit_queue_busy= TRUE;

    /*
      Set these so parent can run checkpoint_and_purge() in last thread.
      (When using optimized thread scheduling, we run checkpoint_and_purge()
      in this function, so parent does not need to and we need not set these
      values).
    */
    last_in_queue->check_purge= check_purge;
    last_in_queue->binlog_id= binlog_id;

    /* Note that we return with LOCK_commit_ordered locked! */
    DBUG_VOID_RETURN;
  }

  /*
    Wakeup each participant waiting for our group commit, first calling the
    commit_ordered() methods for any transactions doing 2-phase commit.
  */
  current= queue;
  while (current != NULL)
  {
    group_commit_entry *next;

    DEBUG_SYNC(leader->thd, "commit_loop_entry_commit_ordered");
    ++num_commits;
    if (current->cache_mngr->using_xa && !current->error &&
        DBUG_EVALUATE_IF("skip_commit_ordered", 0, 1))
      run_commit_ordered(current->thd, current->all);
    current->thd->wakeup_subsequent_commits(current->error);

    /*
      Careful not to access current->next after waking up the other thread! As
      it may change immediately after wakeup.
    */
    next= current->next;
    if (current != leader)                      // Don't wake up ourself
    {
      if (current->queued_by_other)
        current->thd->wait_for_commit_ptr->wakeup(current->error);
      else
        current->thd->signal_wakeup_ready();
    }
    current= next;
  }
  DEBUG_SYNC(leader->thd, "commit_after_group_run_commit_ordered");
  mysql_mutex_unlock(&LOCK_commit_ordered);
  DEBUG_SYNC(leader->thd, "commit_after_group_release_commit_ordered");

  if (check_purge)
    checkpoint_and_purge(binlog_id);

  DBUG_VOID_RETURN;
}


int
MYSQL_BIN_LOG::write_transaction_or_stmt(group_commit_entry *entry,
                                         uint64 commit_id)
{
  binlog_cache_mngr *mngr= entry->cache_mngr;
  DBUG_ENTER("MYSQL_BIN_LOG::write_transaction_or_stmt");

  if (write_gtid_event(entry->thd, false, entry->using_trx_cache, commit_id))
    DBUG_RETURN(ER_ERROR_ON_WRITE);

  if (entry->using_stmt_cache && !mngr->stmt_cache.empty() &&
      write_cache(entry->thd, mngr->get_binlog_cache_log(FALSE)))
  {
    entry->error_cache= &mngr->stmt_cache.cache_log;
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }

  if (entry->using_trx_cache && !mngr->trx_cache.empty())
  {
    DBUG_EXECUTE_IF("crash_before_writing_xid",
                    {
                      if ((write_cache(entry->thd,
                                       mngr->get_binlog_cache_log(TRUE))))
                        DBUG_PRINT("info", ("error writing binlog cache"));
                      else
                        flush_and_sync(0);

                      DBUG_PRINT("info", ("crashing before writing xid"));
                      DBUG_SUICIDE();
                    });

    if (write_cache(entry->thd, mngr->get_binlog_cache_log(TRUE)))
    {
      entry->error_cache= &mngr->trx_cache.cache_log;
      DBUG_RETURN(ER_ERROR_ON_WRITE);
    }
  }

  DBUG_EXECUTE_IF("inject_error_writing_xid",
                  {
                    entry->error_cache= NULL;
                    errno= 28;
                    DBUG_RETURN(ER_ERROR_ON_WRITE);
                  });

  if (write_event(entry->end_event))
  {
    entry->error_cache= NULL;
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  status_var_add(entry->thd->status_var.binlog_bytes_written,
                 entry->end_event->data_written);

  if (entry->incident_event)
  {
    if (write_event(entry->incident_event))
    {
      entry->error_cache= NULL;
      DBUG_RETURN(ER_ERROR_ON_WRITE);
    }
  }

  if (mngr->get_binlog_cache_log(FALSE)->error) // Error on read
  {
    entry->error_cache= &mngr->stmt_cache.cache_log;
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  if (mngr->get_binlog_cache_log(TRUE)->error)  // Error on read
  {
    entry->error_cache= &mngr->trx_cache.cache_log;
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }

  DBUG_RETURN(0);
}


/*
  Wait for sufficient commits to queue up for group commit, according to the
  values of binlog_commit_wait_count and binlog_commit_wait_usec.

  Note that this function may release and re-acquire LOCK_log and
  LOCK_prepare_ordered if it needs to wait.
*/

void
MYSQL_BIN_LOG::wait_for_sufficient_commits()
{
  size_t count;
  group_commit_entry *e;
  group_commit_entry *last_head;
  struct timespec wait_until;

  mysql_mutex_assert_owner(&LOCK_log);
  mysql_mutex_assert_owner(&LOCK_prepare_ordered);

  for (e= last_head= group_commit_queue, count= 0; e; e= e->next)
  {
    if (++count >= opt_binlog_commit_wait_count)
    {
      group_commit_trigger_count++;
      return;
    }
    if (unlikely(e->thd->has_waiter))
    {
      group_commit_trigger_lock_wait++;
      return;
    }
  }

  mysql_mutex_unlock(&LOCK_log);
  set_timespec_nsec(wait_until, (ulonglong)1000*opt_binlog_commit_wait_usec);

  for (;;)
  {
    int err;
    group_commit_entry *head;

    err= mysql_cond_timedwait(&COND_prepare_ordered, &LOCK_prepare_ordered,
                              &wait_until);
    if (err == ETIMEDOUT)
    {
      group_commit_trigger_timeout++;
      break;
    }
    if (unlikely(last_head->thd->has_waiter))
    {
      group_commit_trigger_lock_wait++;
      break;
    }
    head= group_commit_queue;
    for (e= head; e && e != last_head; e= e->next)
    {
      ++count;
      if (unlikely(e->thd->has_waiter))
      {
        group_commit_trigger_lock_wait++;
        goto after_loop;
      }
    }
    if (count >= opt_binlog_commit_wait_count)
    {
      group_commit_trigger_count++;
      break;
    }
    last_head= head;
  }
after_loop:

  /*
    We must not wait for LOCK_log while holding LOCK_prepare_ordered.
    LOCK_log can be held for long periods (eg. we do I/O under it), while
    LOCK_prepare_ordered must only be held for short periods.

    In addition, waiting for LOCK_log while holding LOCK_prepare_ordered would
    violate locking order of LOCK_log-before-LOCK_prepare_ordered. This could
    cause SAFEMUTEX warnings (even if it cannot actually deadlock with current
    code, as there can be at most one group commit leader thread at a time).

    So release and re-acquire LOCK_prepare_ordered if we need to wait for the
    LOCK_log.
  */
  if (mysql_mutex_trylock(&LOCK_log))
  {
    mysql_mutex_unlock(&LOCK_prepare_ordered);
    mysql_mutex_lock(&LOCK_log);
    mysql_mutex_lock(&LOCK_prepare_ordered);
  }
}


void
MYSQL_BIN_LOG::binlog_trigger_immediate_group_commit()
{
  group_commit_entry *head;
  mysql_mutex_assert_owner(&LOCK_prepare_ordered);
  head= group_commit_queue;
  if (head)
  {
    head->thd->has_waiter= true;
    mysql_cond_signal(&COND_prepare_ordered);
  }
}


/*
  This function is called when a transaction T1 goes to wait for another
  transaction T2. It is used to cut short any binlog group commit delay from
  --binlog-commit-wait-count in the case where another transaction is stalled
  on the wait due to conflicting row locks.

  If T2 is already ready to group commit, any waiting group commit will be
  signalled to proceed immediately. Otherwise, a flag will be set in T2, and
  when T2 later becomes ready, immediate group commit will be triggered.
*/
void
binlog_report_wait_for(THD *thd1, THD *thd2)
{
  if (opt_binlog_commit_wait_count == 0)
    return;
  mysql_mutex_lock(&LOCK_prepare_ordered);
  thd2->has_waiter= true;
  if (thd2->waiting_on_group_commit)
    mysql_bin_log.binlog_trigger_immediate_group_commit();
  mysql_mutex_unlock(&LOCK_prepare_ordered);
}


/**
  Wait until we get a signal that the relay log has been updated.

  @param thd		Thread variable

  @note
    One must have a lock on LOCK_log before calling this function.
    This lock will be released before return! That's required by
    THD::enter_cond() (see NOTES in sql_class.h).
*/

void MYSQL_BIN_LOG::wait_for_update_relay_log(THD* thd)
{
  PSI_stage_info old_stage;
  DBUG_ENTER("wait_for_update_relay_log");

  mysql_mutex_assert_owner(&LOCK_log);
  thd->ENTER_COND(&update_cond, &LOCK_log,
                  &stage_slave_has_read_all_relay_log,
                  &old_stage);
  mysql_cond_wait(&update_cond, &LOCK_log);
  thd->EXIT_COND(&old_stage);
  DBUG_VOID_RETURN;
}

/**
  Wait until we get a signal that the binary log has been updated.
  Applies to master only.
     
  NOTES
  @param[in] thd        a THD struct
  @param[in] timeout    a pointer to a timespec;
                        NULL means to wait w/o timeout.
  @retval    0          if got signalled on update
  @retval    non-0      if wait timeout elapsed
  @note
    LOCK_log must be taken before calling this function.
    LOCK_log is being released while the thread is waiting.
    LOCK_log is released by the caller.
*/

int MYSQL_BIN_LOG::wait_for_update_bin_log(THD* thd,
                                           const struct timespec *timeout)
{
  int ret= 0;
  DBUG_ENTER("wait_for_update_bin_log");

  thd_wait_begin(thd, THD_WAIT_BINLOG);
  mysql_mutex_assert_owner(&LOCK_log);
  if (!timeout)
    mysql_cond_wait(&update_cond, &LOCK_log);
  else
    ret= mysql_cond_timedwait(&update_cond, &LOCK_log,
                              const_cast<struct timespec *>(timeout));
  thd_wait_end(thd);
  DBUG_RETURN(ret);
}

int MYSQL_BIN_LOG::wait_for_update_binlog_end_pos(THD* thd,
                                                  struct timespec *timeout)
{
  int ret= 0;
  DBUG_ENTER("wait_for_update_binlog_end_pos");

  thd_wait_begin(thd, THD_WAIT_BINLOG);
  mysql_mutex_assert_owner(get_binlog_end_pos_lock());
  if (!timeout)
    mysql_cond_wait(&update_cond, get_binlog_end_pos_lock());
  else
    ret= mysql_cond_timedwait(&update_cond, get_binlog_end_pos_lock(),
                              timeout);
  thd_wait_end(thd);
  DBUG_RETURN(ret);
}


/**
  Close the log file.

  @param exiting     Bitmask for one or more of the following bits:
          - LOG_CLOSE_INDEX : if we should close the index file
          - LOG_CLOSE_TO_BE_OPENED : if we intend to call open
                                     at once after close.
          - LOG_CLOSE_STOP_EVENT : write a 'stop' event to the log
          - LOG_CLOSE_DELAYED_CLOSE : do not yet close the file and clear the
                                      LOG_EVENT_BINLOG_IN_USE_F flag

  @note
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_BIN_LOG::close(uint exiting)
{					// One can't set log_type here!
  bool failed_to_save_state= false;

  DBUG_ENTER("MYSQL_BIN_LOG::close");
  DBUG_PRINT("enter",("exiting: %d", (int) exiting));
  if (log_state == LOG_OPENED)
  {
#ifdef HAVE_REPLICATION
    if (log_type == LOG_BIN &&
	(exiting & LOG_CLOSE_STOP_EVENT))
    {
      Stop_log_event s;
      // the checksumming rule for relay-log case is similar to Rotate
        s.checksum_alg= is_relay_log ? relay_log_checksum_alg
                                     : (enum_binlog_checksum_alg)binlog_checksum_options;
      DBUG_ASSERT(!is_relay_log ||
                  relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
      write_event(&s);
      bytes_written+= s.data_written;
      signal_update();

      /*
        When we shut down server, write out the binlog state to a separate
        file so we do not have to scan an entire binlog file to recover it
        at next server start.

        Note that this must be written and synced to disk before marking the
        last binlog file as "not crashed".
      */
      if (!is_relay_log && write_state_to_file())
      {
        sql_print_error("Failed to save binlog GTID state during shutdown. "
                        "Binlog will be marked as crashed, so that crash "
                        "recovery can recover the state at next server "
                        "startup.");
        /*
          Leave binlog file marked as crashed, so we can recover state by
          scanning it now that we failed to write out the state properly.
        */
        failed_to_save_state= true;
      }
    }
#endif /* HAVE_REPLICATION */

    /* don't pwrite in a file opened with O_APPEND - it doesn't work */
    if (log_file.type == WRITE_CACHE && log_type == LOG_BIN
        && !(exiting & LOG_CLOSE_DELAYED_CLOSE))
    {
      my_off_t org_position= mysql_file_tell(log_file.file, MYF(0));
      if (!failed_to_save_state)
        clear_inuse_flag_when_closing(log_file.file);
      /*
        Restore position so that anything we have in the IO_cache is written
        to the correct position.
        We need the seek here, as mysql_file_pwrite() is not guaranteed to keep the
        original position on system that doesn't support pwrite().
      */
      mysql_file_seek(log_file.file, org_position, MY_SEEK_SET, MYF(0));
    }

    /* this will cleanup IO_CACHE, sync and close the file */
    MYSQL_LOG::close(exiting);
  }

  /*
    The following test is needed even if is_open() is not set, as we may have
    called a not complete close earlier and the index file is still open.
  */

  if ((exiting & LOG_CLOSE_INDEX) && my_b_inited(&index_file))
  {
    end_io_cache(&index_file);
    if (mysql_file_close(index_file.file, MYF(0)) < 0 && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER_THD_OR_DEFAULT(current_thd, ER_ERROR_ON_WRITE),
                      index_file_name, errno);
    }
  }
  log_state= (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  my_free(name);
  name= NULL;
  DBUG_VOID_RETURN;
}


/*
  Clear the LOG_EVENT_BINLOG_IN_USE_F; this marks the binlog file as cleanly
  closed and not needing crash recovery.
*/
void MYSQL_BIN_LOG::clear_inuse_flag_when_closing(File file)
{
  my_off_t offset= BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
  uchar flags= 0;            // clearing LOG_EVENT_BINLOG_IN_USE_F
  mysql_file_pwrite(file, &flags, 1, offset, MYF(0));
}


void MYSQL_BIN_LOG::set_max_size(ulong max_size_arg)
{
  /*
    We need to take locks, otherwise this may happen:
    new_file() is called, calls open(old_max_size), then before open() starts,
    set_max_size() sets max_size to max_size_arg, then open() starts and
    uses the old_max_size argument, so max_size_arg has been overwritten and
    it's like if the SET command was never run.
  */
  DBUG_ENTER("MYSQL_BIN_LOG::set_max_size");
  mysql_mutex_lock(&LOCK_log);
  if (is_open())
    max_size= max_size_arg;
  mysql_mutex_unlock(&LOCK_log);
  DBUG_VOID_RETURN;
}


/**
  Check if a string is a valid number.

  @param str			String to test
  @param res			Store value here
  @param allow_wildcards	Set to 1 if we should ignore '%' and '_'

  @note
    For the moment the allow_wildcards argument is not used
    Should be move to some other file.

  @retval
    1	String is a number
  @retval
    0	String is not a number
*/

static bool test_if_number(register const char *str,
			   ulong *res, bool allow_wildcards)
{
  reg2 int flag;
  const char *start;
  DBUG_ENTER("test_if_number");

  flag=0; start=str;
  while (*str++ == ' ') ;
  if (*--str == '-' || *str == '+')
    str++;
  while (my_isdigit(files_charset_info,*str) ||
	 (allow_wildcards && (*str == wild_many || *str == wild_one)))
  {
    flag=1;
    str++;
  }
  if (*str == '.')
  {
    for (str++ ;
	 my_isdigit(files_charset_info,*str) ||
	   (allow_wildcards && (*str == wild_many || *str == wild_one)) ;
	 str++, flag=1) ;
  }
  if (*str != 0 || flag == 0)
    DBUG_RETURN(0);
  if (res)
    *res=atol(start);
  DBUG_RETURN(1);			/* Number ok */
} /* test_if_number */


void sql_perror(const char *message)
{
#if defined(_WIN32)
  char* buf;
  DWORD dw= GetLastError();
  if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |  FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,  NULL, dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, NULL ) > 0)
  {
    sql_print_error("%s: %s",message, buf);
    LocalFree((HLOCAL)buf);
  }
  else
  {
    sql_print_error("%s", message);
  }
#elif defined(HAVE_STRERROR)
  sql_print_error("%s: %s",message, strerror(errno));
#else 
  perror(message);
#endif
}


/*
  Change the file associated with two output streams. Used to
  redirect stdout and stderr to a file. The streams are reopened
  only for appending (writing at end of file).
*/
extern "C" my_bool reopen_fstreams(const char *filename,
                                   FILE *outstream, FILE *errstream)
{
  if (outstream && !my_freopen(filename, "a", outstream))
    return TRUE;

  if (errstream && !my_freopen(filename, "a", errstream))
    return TRUE;

  /* The error stream must be unbuffered. */
  if (errstream)
    setbuf(errstream, NULL);

  return FALSE;
}


/*
  Unfortunately, there seems to be no good way
  to restore the original streams upon failure.
*/
static bool redirect_std_streams(const char *file)
{
  if (reopen_fstreams(file, stdout, stderr))
    return TRUE;

  setbuf(stderr, NULL);
  return FALSE;
}


bool flush_error_log()
{
  bool result= 0;
  if (opt_error_log)
  {
    mysql_mutex_lock(&LOCK_error_log);
    if (redirect_std_streams(log_error_file))
      result= 1;
    mysql_mutex_unlock(&LOCK_error_log);
  }
  return result;
}

void MYSQL_BIN_LOG::signal_update()
{
  DBUG_ENTER("MYSQL_BIN_LOG::signal_update");
  signal_cnt++;
  mysql_cond_broadcast(&update_cond);
  DBUG_VOID_RETURN;
}

#ifdef _WIN32
static void print_buffer_to_nt_eventlog(enum loglevel level, char *buff,
                                        size_t length, size_t buffLen)
{
  HANDLE event;
  char   *buffptr= buff;
  DBUG_ENTER("print_buffer_to_nt_eventlog");

  /* Add ending CR/LF's to string, overwrite last chars if necessary */
  strmov(buffptr+MY_MIN(length, buffLen-5), "\r\n\r\n");

  setup_windows_event_source();
  if ((event= RegisterEventSource(NULL,"MySQL")))
  {
    switch (level) {
      case ERROR_LEVEL:
        ReportEvent(event, EVENTLOG_ERROR_TYPE, 0, MSG_DEFAULT, NULL, 1, 0,
                    (LPCSTR*)&buffptr, NULL);
        break;
      case WARNING_LEVEL:
        ReportEvent(event, EVENTLOG_WARNING_TYPE, 0, MSG_DEFAULT, NULL, 1, 0,
                    (LPCSTR*) &buffptr, NULL);
        break;
      case INFORMATION_LEVEL:
        ReportEvent(event, EVENTLOG_INFORMATION_TYPE, 0, MSG_DEFAULT, NULL, 1,
                    0, (LPCSTR*) &buffptr, NULL);
        break;
    }
    DeregisterEventSource(event);
  }

  DBUG_VOID_RETURN;
}
#endif /* _WIN32 */


#ifndef EMBEDDED_LIBRARY
static void print_buffer_to_file(enum loglevel level, const char *buffer,
                                 size_t length)
{
  time_t skr;
  struct tm tm_tmp;
  struct tm *start;
  THD *thd;
  int tag_length= 0;
  char tag[NAME_LEN];
  DBUG_ENTER("print_buffer_to_file");
  DBUG_PRINT("enter",("buffer: %s", buffer));

  if (mysqld_server_initialized && (thd= current_thd))
  {
    if (thd->connection_name.length)
    {
      /*
        Add tag for slaves so that the user can see from which connection
        the error originates.
      */
      tag_length= my_snprintf(tag, sizeof(tag),
                              ER_THD(thd, ER_MASTER_LOG_PREFIX),
                              (int) thd->connection_name.length,
                              thd->connection_name.str);
    }
  }

  mysql_mutex_lock(&LOCK_error_log);

  skr= my_time(0);
  localtime_r(&skr, &tm_tmp);
  start=&tm_tmp;

  fprintf(stderr, "%d-%02d-%02d %2d:%02d:%02d %lu [%s] %.*s%.*s\n",
          start->tm_year + 1900,
          start->tm_mon+1,
          start->tm_mday,
          start->tm_hour,
          start->tm_min,
          start->tm_sec,
          (unsigned long) pthread_self(),
          (level == ERROR_LEVEL ? "ERROR" : level == WARNING_LEVEL ?
           "Warning" : "Note"),
          tag_length, tag,
          (int) length, buffer);

  fflush(stderr);

  mysql_mutex_unlock(&LOCK_error_log);
  DBUG_VOID_RETURN;
}

/**
  Prints a printf style message to the error log and, under NT, to the
  Windows event log.

  This function prints the message into a buffer and then sends that buffer
  to other functions to write that message to other logging sources.

  @param level          The level of the msg significance
  @param format         Printf style format of message
  @param args           va_list list of arguments for the message

  @returns
    The function always returns 0. The return value is present in the
    signature to be compatible with other logging routines, which could
    return an error (e.g. logging to the log tables)
*/
int vprint_msg_to_log(enum loglevel level, const char *format, va_list args)
{
  char   buff[1024];
  size_t length;
  DBUG_ENTER("vprint_msg_to_log");

  length= my_vsnprintf(buff, sizeof(buff), format, args);
  print_buffer_to_file(level, buff, length);

#ifdef _WIN32
  print_buffer_to_nt_eventlog(level, buff, length, sizeof(buff));
#endif

  DBUG_RETURN(0);
}
#endif /* EMBEDDED_LIBRARY */


void sql_print_error(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_error");

  va_start(args, format);
  error_log_print(ERROR_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


void sql_print_warning(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_warning");

  va_start(args, format);
  error_log_print(WARNING_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


void sql_print_information(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_information");

  if (disable_log_notes)
    DBUG_VOID_RETURN;                 // Skip notes during start/shutdown
  
  va_start(args, format);
  error_log_print(INFORMATION_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


void
TC_LOG::run_prepare_ordered(THD *thd, bool all)
{
  Ha_trx_info *ha_info=
    all ? thd->transaction.all.ha_list : thd->transaction.stmt.ha_list;

  mysql_mutex_assert_owner(&LOCK_prepare_ordered);
  for (; ha_info; ha_info= ha_info->next())
  {
    handlerton *ht= ha_info->ht();
    if (!ht->prepare_ordered)
      continue;
    ht->prepare_ordered(ht, thd, all);
  }
}


void
TC_LOG::run_commit_ordered(THD *thd, bool all)
{
  Ha_trx_info *ha_info=
    all ? thd->transaction.all.ha_list : thd->transaction.stmt.ha_list;

  mysql_mutex_assert_owner(&LOCK_commit_ordered);
  for (; ha_info; ha_info= ha_info->next())
  {
    handlerton *ht= ha_info->ht();
    if (!ht->commit_ordered)
      continue;
    ht->commit_ordered(ht, thd, all);
    DEBUG_SYNC(thd, "commit_after_run_commit_ordered");
  }
}


int TC_LOG_MMAP::log_and_order(THD *thd, my_xid xid, bool all,
                               bool need_prepare_ordered,
                               bool need_commit_ordered)
{
  int cookie;
  struct commit_entry entry;
  bool UNINIT_VAR(is_group_commit_leader);

  if (need_prepare_ordered)
  {
    mysql_mutex_lock(&LOCK_prepare_ordered);
    run_prepare_ordered(thd, all);
    if (need_commit_ordered)
    {
      /*
        Must put us in queue so we can run_commit_ordered() in same sequence
        as we did run_prepare_ordered().
      */
      thd->clear_wakeup_ready();
      entry.thd= thd;
      commit_entry *previous_queue= commit_ordered_queue;
      entry.next= previous_queue;
      commit_ordered_queue= &entry;
      is_group_commit_leader= (previous_queue == NULL);
    }
    mysql_mutex_unlock(&LOCK_prepare_ordered);
  }

  if (thd->wait_for_prior_commit())
    return 0;

  cookie= 0;
  if (xid)
    cookie= log_one_transaction(xid);

  if (need_commit_ordered)
  {
    if (need_prepare_ordered)
    {
      /*
        We did the run_prepare_ordered() serialised, then ran the log_xid() in
        parallel. Now we have to do run_commit_ordered() serialised in the
        same sequence as run_prepare_ordered().

        We do this starting from the head of the queue, each thread doing
        run_commit_ordered() and signalling the next in queue.
      */
      if (is_group_commit_leader)
      {
        /* The first in queue starts the ball rolling. */
        mysql_mutex_lock(&LOCK_prepare_ordered);
        while (commit_ordered_queue_busy)
          mysql_cond_wait(&COND_queue_busy, &LOCK_prepare_ordered);
        commit_entry *queue= commit_ordered_queue;
        commit_ordered_queue= NULL;
        /*
          Mark the queue busy while we bounce it from one thread to the
          next.
        */
        commit_ordered_queue_busy= true;
        mysql_mutex_unlock(&LOCK_prepare_ordered);

        /* Reverse the queue list so we get correct order. */
        commit_entry *prev= NULL;
        while (queue)
        {
          commit_entry *next= queue->next;
          queue->next= prev;
          prev= queue;
          queue= next;
        }
        DBUG_ASSERT(prev == &entry && prev->thd == thd);
      }
      else
      {
        /* Not first in queue; just wait until previous thread wakes us up. */
        thd->wait_for_wakeup_ready();
      }
    }

    /* Only run commit_ordered() if log_xid was successful. */
    if (cookie)
    {
      mysql_mutex_lock(&LOCK_commit_ordered);
      run_commit_ordered(thd, all);
      mysql_mutex_unlock(&LOCK_commit_ordered);
    }

    if (need_prepare_ordered)
    {
      commit_entry *next= entry.next;
      if (next)
      {
        next->thd->signal_wakeup_ready();
      }
      else
      {
        mysql_mutex_lock(&LOCK_prepare_ordered);
        commit_ordered_queue_busy= false;
        mysql_cond_signal(&COND_queue_busy);
        mysql_mutex_unlock(&LOCK_prepare_ordered);
      }
    }
  }

  return cookie;
}


/********* transaction coordinator log for 2pc - mmap() based solution *******/

/*
  the log consists of a file, mapped to memory.
  file is divided into pages of tc_log_page_size size.
  (usable size of the first page is smaller because of the log header)
  there is a PAGE control structure for each page
  each page (or rather its PAGE control structure) can be in one of
  the three states - active, syncing, pool.
  there could be only one page in the active or syncing state,
  but many in pool - pool is a fifo queue.
  the usual lifecycle of a page is pool->active->syncing->pool.
  the "active" page is a page where new xid's are logged.
  the page stays active as long as the syncing slot is taken.
  the "syncing" page is being synced to disk. no new xid can be added to it.
  when the syncing is done the page is moved to a pool and an active page
  becomes "syncing".

  the result of such an architecture is a natural "commit grouping" -
  If commits are coming faster than the system can sync, they do not
  stall. Instead, all commits that came since the last sync are
  logged to the same "active" page, and they all are synced with the next -
  one - sync. Thus, thought individual commits are delayed, throughput
  is not decreasing.

  when an xid is added to an active page, the thread of this xid waits
  for a page's condition until the page is synced. when syncing slot
  becomes vacant one of these waiters is awaken to take care of syncing.
  it syncs the page and signals all waiters that the page is synced.
  PAGE::waiters is used to count these waiters, and a page may never
  become active again until waiters==0 (that is all waiters from the
  previous sync have noticed that the sync was completed)

  note, that the page becomes "dirty" and has to be synced only when a
  new xid is added into it. Removing a xid from a page does not make it
  dirty - we don't sync xid removals to disk.
*/

ulong tc_log_page_waits= 0;

#ifdef HAVE_MMAP

#define TC_LOG_HEADER_SIZE (sizeof(tc_log_magic)+1)

static const uchar tc_log_magic[]={(uchar) 254, 0x23, 0x05, 0x74};

ulong opt_tc_log_size;
ulong tc_log_max_pages_used=0, tc_log_page_size=0, tc_log_cur_pages_used=0;

int TC_LOG_MMAP::open(const char *opt_name)
{
  uint i;
  bool crashed=FALSE;
  PAGE *pg;

  DBUG_ASSERT(total_ha_2pc > 1);
  DBUG_ASSERT(opt_name && opt_name[0]);

  tc_log_page_size= my_getpagesize();

  fn_format(logname,opt_name,mysql_data_home,"",MY_UNPACK_FILENAME);
  if ((fd= mysql_file_open(key_file_tclog, logname, O_RDWR, MYF(0))) < 0)
  {
    if (my_errno != ENOENT)
      goto err;
    if (using_heuristic_recover())
      return 1;
    if ((fd= mysql_file_create(key_file_tclog, logname, CREATE_MODE,
                               O_RDWR, MYF(MY_WME))) < 0)
      goto err;
    inited=1;
    file_length= opt_tc_log_size;
    if (mysql_file_chsize(fd, file_length, 0, MYF(MY_WME)))
      goto err;
  }
  else
  {
    inited= 1;
    crashed= TRUE;
    sql_print_information("Recovering after a crash using %s", opt_name);
    if (tc_heuristic_recover)
    {
      sql_print_error("Cannot perform automatic crash recovery when "
                      "--tc-heuristic-recover is used");
      goto err;
    }
    file_length= mysql_file_seek(fd, 0L, MY_SEEK_END, MYF(MY_WME+MY_FAE));
    if (file_length == MY_FILEPOS_ERROR || file_length % tc_log_page_size)
      goto err;
  }

  data= (uchar *)my_mmap(0, (size_t)file_length, PROT_READ|PROT_WRITE,
                        MAP_NOSYNC|MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
  {
    my_errno=errno;
    goto err;
  }
  inited=2;

  npages=(uint)file_length/tc_log_page_size;
  if (npages < 3)             // to guarantee non-empty pool
    goto err;
  if (!(pages=(PAGE *)my_malloc(npages*sizeof(PAGE), MYF(MY_WME|MY_ZEROFILL))))
    goto err;
  inited=3;
  for (pg=pages, i=0; i < npages; i++, pg++)
  {
    pg->next=pg+1;
    pg->waiters=0;
    pg->state=PS_POOL;
    mysql_mutex_init(key_PAGE_lock, &pg->lock, MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_PAGE_cond, &pg->cond, 0);
    pg->ptr= pg->start=(my_xid *)(data + i*tc_log_page_size);
    pg->size=pg->free=tc_log_page_size/sizeof(my_xid);
    pg->end=pg->start + pg->size;
  }
  pages[0].size=pages[0].free=
                (tc_log_page_size-TC_LOG_HEADER_SIZE)/sizeof(my_xid);
  pages[0].start=pages[0].end-pages[0].size;
  pages[npages-1].next=0;
  inited=4;

  if (crashed && recover())
      goto err;

  memcpy(data, tc_log_magic, sizeof(tc_log_magic));
  data[sizeof(tc_log_magic)]= (uchar)total_ha_2pc;
  my_msync(fd, data, tc_log_page_size, MS_SYNC);
  inited=5;

  mysql_mutex_init(key_LOCK_sync, &LOCK_sync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_active, &LOCK_active, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_pool, &LOCK_pool, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_pending_checkpoint, &LOCK_pending_checkpoint,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_active, &COND_active, 0);
  mysql_cond_init(key_COND_pool, &COND_pool, 0);
  mysql_cond_init(key_TC_LOG_MMAP_COND_queue_busy, &COND_queue_busy, 0);

  inited=6;

  syncing= 0;
  active=pages;
  DBUG_ASSERT(npages >= 2);
  pool=pages+1;
  pool_last_ptr= &((pages+npages-1)->next);
  commit_ordered_queue= NULL;
  commit_ordered_queue_busy= false;

  return 0;

err:
  close();
  return 1;
}

/**
  there is no active page, let's got one from the pool.

  Two strategies here:
    -# take the first from the pool
    -# if there're waiters - take the one with the most free space.

  @todo
    page merging. try to allocate adjacent page first,
    so that they can be flushed both in one sync
*/

void TC_LOG_MMAP::get_active_from_pool()
{
  PAGE **p, **best_p=0;
  int best_free;

  mysql_mutex_lock(&LOCK_pool);

  do
  {
    best_p= p= &pool;
    if ((*p)->waiters == 0 && (*p)->free > 0) // can the first page be used ?
      break;                                  // yes - take it.

    best_free=0;            // no - trying second strategy
    for (p=&(*p)->next; *p; p=&(*p)->next)
    {
      if ((*p)->waiters == 0 && (*p)->free > best_free)
      {
        best_free=(*p)->free;
        best_p=p;
      }
    }
  }
  while ((*best_p == 0 || best_free == 0) && overflow());

  mysql_mutex_assert_owner(&LOCK_active);
  active=*best_p;

  /* Unlink the page from the pool. */
  if (!(*best_p)->next)
    pool_last_ptr= best_p;
  *best_p=(*best_p)->next;
  mysql_mutex_unlock(&LOCK_pool);

  mysql_mutex_lock(&active->lock);
  if (active->free == active->size) // we've chosen an empty page
  {
    tc_log_cur_pages_used++;
    set_if_bigger(tc_log_max_pages_used, tc_log_cur_pages_used);
  }
}

/**
  @todo
  perhaps, increase log size ?
*/
int TC_LOG_MMAP::overflow()
{
  /*
    simple overflow handling - just wait
    TODO perhaps, increase log size ?
    let's check the behaviour of tc_log_page_waits first
  */
  tc_log_page_waits++;
  mysql_cond_wait(&COND_pool, &LOCK_pool);
  return 1; // always return 1
}

/**
  Record that transaction XID is committed on the persistent storage.

    This function is called in the middle of two-phase commit:
    First all resources prepare the transaction, then tc_log->log() is called,
    then all resources commit the transaction, then tc_log->unlog() is called.

    All access to active page is serialized but it's not a problem, as
    we're assuming that fsync() will be a main bottleneck.
    That is, parallelizing writes to log pages we'll decrease number of
    threads waiting for a page, but then all these threads will be waiting
    for a fsync() anyway

   If tc_log == MYSQL_LOG then tc_log writes transaction to binlog and
   records XID in a special Xid_log_event.
   If tc_log = TC_LOG_MMAP then xid is written in a special memory-mapped
   log.

  @retval
    0  - error
  @retval
    \# - otherwise, "cookie", a number that will be passed as an argument
    to unlog() call. tc_log can define it any way it wants,
    and use for whatever purposes. TC_LOG_MMAP sets it
    to the position in memory where xid was logged to.
*/

int TC_LOG_MMAP::log_one_transaction(my_xid xid)
{
  int err;
  PAGE *p;
  ulong cookie;

  mysql_mutex_lock(&LOCK_active);

  /*
    if the active page is full - just wait...
    frankly speaking, active->free here accessed outside of mutex
    protection, but it's safe, because it only means we may miss an
    unlog() for the active page, and we're not waiting for it here -
    unlog() does not signal COND_active.
  */
  while (unlikely(active && active->free == 0))
    mysql_cond_wait(&COND_active, &LOCK_active);

  /* no active page ? take one from the pool */
  if (active == 0)
    get_active_from_pool();
  else
    mysql_mutex_lock(&active->lock);

  p=active;

  /*
    p->free is always > 0 here because to decrease it one needs
    to take p->lock and before it one needs to take LOCK_active.
    But checked that active->free > 0 under LOCK_active and
    haven't release it ever since
  */

  /* searching for an empty slot */
  while (*p->ptr)
  {
    p->ptr++;
    DBUG_ASSERT(p->ptr < p->end);               // because p->free > 0
  }

  /* found! store xid there and mark the page dirty */
  cookie= (ulong)((uchar *)p->ptr - data);      // can never be zero
  *p->ptr++= xid;
  p->free--;
  p->state= PS_DIRTY;
  mysql_mutex_unlock(&p->lock);

  mysql_mutex_lock(&LOCK_sync);
  if (syncing)
  {                                          // somebody's syncing. let's wait
    mysql_mutex_unlock(&LOCK_active);
    mysql_mutex_lock(&p->lock);
    p->waiters++;
    while (p->state == PS_DIRTY && syncing)
    {
      mysql_mutex_unlock(&p->lock);
      mysql_cond_wait(&p->cond, &LOCK_sync);
      mysql_mutex_lock(&p->lock);
    }
    p->waiters--;
    err= p->state == PS_ERROR;
    if (p->state != PS_DIRTY)                   // page was synced
    {
      mysql_mutex_unlock(&LOCK_sync);
      if (p->waiters == 0)
        mysql_cond_signal(&COND_pool);     // in case somebody's waiting
      mysql_mutex_unlock(&p->lock);
      goto done;                             // we're done
    }
    DBUG_ASSERT(!syncing);
    mysql_mutex_unlock(&p->lock);
    syncing = p;
    mysql_mutex_unlock(&LOCK_sync);

    mysql_mutex_lock(&LOCK_active);
    active=0;                                  // page is not active anymore
    mysql_cond_broadcast(&COND_active);
    mysql_mutex_unlock(&LOCK_active);
  }
  else
  {
    syncing = p;                               // place is vacant - take it
    mysql_mutex_unlock(&LOCK_sync);
    active = 0;                                // page is not active anymore
    mysql_cond_broadcast(&COND_active);
    mysql_mutex_unlock(&LOCK_active);
  }
  err= sync();

done:
  return err ? 0 : cookie;
}

int TC_LOG_MMAP::sync()
{
  int err;

  DBUG_ASSERT(syncing != active);

  /*
    sit down and relax - this can take a while...
    note - no locks are held at this point
  */
  err= my_msync(fd, syncing->start, syncing->size * sizeof(my_xid), MS_SYNC);

  /* page is synced. let's move it to the pool */
  mysql_mutex_lock(&LOCK_pool);
  (*pool_last_ptr)=syncing;
  pool_last_ptr=&(syncing->next);
  syncing->next=0;
  syncing->state= err ? PS_ERROR : PS_POOL;
  mysql_cond_signal(&COND_pool);           // in case somebody's waiting
  mysql_mutex_unlock(&LOCK_pool);

  /* marking 'syncing' slot free */
  mysql_mutex_lock(&LOCK_sync);
  mysql_cond_broadcast(&syncing->cond);    // signal "sync done"
  syncing=0;
  /*
    we check the "active" pointer without LOCK_active. Still, it's safe -
    "active" can change from NULL to not NULL any time, but it
    will take LOCK_sync before waiting on active->cond. That is, it can never
    miss a signal.
    And "active" can change to NULL only by the syncing thread
    (the thread that will send a signal below)
  */
  if (active)
    mysql_cond_signal(&active->cond);      // wake up a new syncer
  mysql_mutex_unlock(&LOCK_sync);
  return err;
}

static void
mmap_do_checkpoint_callback(void *data)
{
  TC_LOG_MMAP::pending_cookies *pending=
    static_cast<TC_LOG_MMAP::pending_cookies *>(data);
  ++pending->pending_count;
}

int TC_LOG_MMAP::unlog(ulong cookie, my_xid xid)
{
  pending_cookies *full_buffer= NULL;
  uint32 ncookies= tc_log_page_size / sizeof(my_xid);
  DBUG_ASSERT(*(my_xid *)(data+cookie) == xid);

  /*
    Do not delete the entry immediately, as there may be participating storage
    engines which implement commit_checkpoint_request(), and thus have not yet
    flushed the commit durably to disk.

    Instead put it in a queue - and periodically, we will request a checkpoint
    from all engines and delete a whole batch at once.
  */
  mysql_mutex_lock(&LOCK_pending_checkpoint);
  if (pending_checkpoint == NULL)
  {
    uint32 size= sizeof(*pending_checkpoint) + sizeof(ulong) * (ncookies - 1);
    if (!(pending_checkpoint=
          (pending_cookies *)my_malloc(size, MYF(MY_ZEROFILL))))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), size);
      mysql_mutex_unlock(&LOCK_pending_checkpoint);
      return 1;
    }
  }

  pending_checkpoint->cookies[pending_checkpoint->count++]= cookie;
  if (pending_checkpoint->count == ncookies)
  {
    full_buffer= pending_checkpoint;
    pending_checkpoint= NULL;
  }
  mysql_mutex_unlock(&LOCK_pending_checkpoint);

  if (full_buffer)
  {
    /*
      We do an extra increment and notify here - this ensures that
      things work also if there are no engines at all that support
      commit_checkpoint_request.
    */
    ++full_buffer->pending_count;
    ha_commit_checkpoint_request(full_buffer, mmap_do_checkpoint_callback);
    commit_checkpoint_notify(full_buffer);
  }
  return 0;
}


void
TC_LOG_MMAP::commit_checkpoint_notify(void *cookie)
{
  uint count;
  pending_cookies *pending= static_cast<pending_cookies *>(cookie);
  mysql_mutex_lock(&LOCK_pending_checkpoint);
  DBUG_ASSERT(pending->pending_count > 0);
  count= --pending->pending_count;
  mysql_mutex_unlock(&LOCK_pending_checkpoint);
  if (count == 0)
  {
    uint i;
    for (i= 0; i < tc_log_page_size / sizeof(my_xid); ++i)
      delete_entry(pending->cookies[i]);
    my_free(pending);
  }
}


/**
  erase xid from the page, update page free space counters/pointers.
  cookie points directly to the memory where xid was logged.
*/

int TC_LOG_MMAP::delete_entry(ulong cookie)
{
  PAGE *p=pages+(cookie/tc_log_page_size);
  my_xid *x=(my_xid *)(data+cookie);

  DBUG_ASSERT(x >= p->start && x < p->end);

  mysql_mutex_lock(&p->lock);
  *x=0;
  p->free++;
  DBUG_ASSERT(p->free <= p->size);
  set_if_smaller(p->ptr, x);
  if (p->free == p->size)              // the page is completely empty
    statistic_decrement(tc_log_cur_pages_used, &LOCK_status);
  if (p->waiters == 0)                 // the page is in pool and ready to rock
    mysql_cond_signal(&COND_pool);     // ping ... for overflow()
  mysql_mutex_unlock(&p->lock);
  return 0;
}

void TC_LOG_MMAP::close()
{
  uint i;
  switch (inited) {
  case 6:
    mysql_mutex_destroy(&LOCK_sync);
    mysql_mutex_destroy(&LOCK_active);
    mysql_mutex_destroy(&LOCK_pool);
    mysql_mutex_destroy(&LOCK_pending_checkpoint);
    mysql_cond_destroy(&COND_pool);
    mysql_cond_destroy(&COND_active);
    mysql_cond_destroy(&COND_queue_busy);
  case 5:
    data[0]='A'; // garble the first (signature) byte, in case mysql_file_delete fails
  case 4:
    for (i=0; i < npages; i++)
    {
      if (pages[i].ptr == 0)
        break;
      mysql_mutex_destroy(&pages[i].lock);
      mysql_cond_destroy(&pages[i].cond);
    }
  case 3:
    my_free(pages);
  case 2:
    my_munmap((char*)data, (size_t)file_length);
  case 1:
    mysql_file_close(fd, MYF(0));
  }
  if (inited>=5) // cannot do in the switch because of Windows
    mysql_file_delete(key_file_tclog, logname, MYF(MY_WME));
  if (pending_checkpoint)
    my_free(pending_checkpoint);
  inited=0;
}


int TC_LOG_MMAP::recover()
{
  HASH xids;
  PAGE *p=pages, *end_p=pages+npages;

  if (bcmp(data, tc_log_magic, sizeof(tc_log_magic)))
  {
    sql_print_error("Bad magic header in tc log");
    goto err1;
  }

  /*
    the first byte after magic signature is set to current
    number of storage engines on startup
  */
  if (data[sizeof(tc_log_magic)] > total_ha_2pc)
  {
    sql_print_error("Recovery failed! You must enable "
                    "all engines that were enabled at the moment of the crash");
    goto err1;
  }

  if (my_hash_init(&xids, &my_charset_bin, tc_log_page_size/3, 0,
                   sizeof(my_xid), 0, 0, MYF(0)))
    goto err1;

  for ( ; p < end_p ; p++)
  {
    for (my_xid *x=p->start; x < p->end; x++)
      if (*x && my_hash_insert(&xids, (uchar *)x))
        goto err2; // OOM
  }

  if (ha_recover(&xids))
    goto err2;

  my_hash_free(&xids);
  bzero(data, (size_t)file_length);
  return 0;

err2:
  my_hash_free(&xids);
err1:
  sql_print_error("Crash recovery failed. Either correct the problem "
                  "(if it's, for example, out of memory error) and restart, "
                  "or delete tc log and start mysqld with "
                  "--tc-heuristic-recover={commit|rollback}");
  return 1;
}
#endif

TC_LOG *tc_log;
TC_LOG_DUMMY tc_log_dummy;
TC_LOG_MMAP  tc_log_mmap;

/**
  Perform heuristic recovery, if --tc-heuristic-recover was used.

  @note
    no matter whether heuristic recovery was successful or not
    mysqld must exit. So, return value is the same in both cases.

  @retval
    0	no heuristic recovery was requested
  @retval
    1   heuristic recovery was performed
*/

int TC_LOG::using_heuristic_recover()
{
  if (!tc_heuristic_recover)
    return 0;

  sql_print_information("Heuristic crash recovery mode");
  if (ha_recover(0))
    sql_print_error("Heuristic crash recovery failed");
  sql_print_information("Please restart mysqld without --tc-heuristic-recover");
  return 1;
}

/****** transaction coordinator log for 2pc - binlog() based solution ******/
#define TC_LOG_BINLOG MYSQL_BIN_LOG

int TC_LOG_BINLOG::open(const char *opt_name)
{
  int      error= 1;

  DBUG_ASSERT(total_ha_2pc > 1);
  DBUG_ASSERT(opt_name && opt_name[0]);

  if (!my_b_inited(&index_file))
  {
    /* There was a failure to open the index file, can't open the binlog */
    cleanup();
    return 1;
  }

  if (using_heuristic_recover())
  {
    /* generate a new binlog to mask a corrupted one */
    open(opt_name, LOG_BIN, 0, 0, WRITE_CACHE, max_binlog_size, 0, TRUE);
    cleanup();
    return 1;
  }

  error= do_binlog_recovery(opt_name, true);
  binlog_state_recover_done= true;
  return error;
}

/** This is called on shutdown, after ha_panic. */
void TC_LOG_BINLOG::close()
{
}

/*
  Do a binlog log_xid() for a group of transactions, linked through
  thd->next_commit_ordered.
*/
int
TC_LOG_BINLOG::log_and_order(THD *thd, my_xid xid, bool all,
                             bool need_prepare_ordered __attribute__((unused)),
                             bool need_commit_ordered __attribute__((unused)))
{
  int err;
  DBUG_ENTER("TC_LOG_BINLOG::log_and_order");

  binlog_cache_mngr *cache_mngr= thd->binlog_setup_trx_data();
  if (!cache_mngr)
  {
    WSREP_DEBUG("Skipping empty log_xid: %s", thd->query());
    DBUG_RETURN(0);
  }

  cache_mngr->using_xa= TRUE;
  cache_mngr->xa_xid= xid;
  err= binlog_commit_flush_xid_caches(thd, cache_mngr, all, xid);

  DEBUG_SYNC(thd, "binlog_after_log_and_order");

  if (err)
    DBUG_RETURN(0);
  /*
    If using explicit user XA, we will not have XID. We must still return a
    non-zero cookie (as zero cookie signals error).
  */
  if (!xid || !cache_mngr->need_unlog)
    DBUG_RETURN(BINLOG_COOKIE_DUMMY(cache_mngr->delayed_error));
  else
    DBUG_RETURN(BINLOG_COOKIE_MAKE(cache_mngr->binlog_id,
                                   cache_mngr->delayed_error));
}

/*
  After an XID is logged, we need to hold on to the current binlog file until
  it is fully committed in the storage engine. The reason is that crash
  recovery only looks at the latest binlog, so we must make sure there are no
  outstanding prepared (but not committed) transactions before rotating the
  binlog.

  To handle this, we keep a count of outstanding XIDs. This function is used
  to increase this count when committing one or more transactions to the
  binary log.
*/
void
TC_LOG_BINLOG::mark_xids_active(ulong binlog_id, uint xid_count)
{
  xid_count_per_binlog *b;

  DBUG_ENTER("TC_LOG_BINLOG::mark_xids_active");
  DBUG_PRINT("info", ("binlog_id=%lu xid_count=%u", binlog_id, xid_count));

  mysql_mutex_lock(&LOCK_xid_list);
  I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
  while ((b= it++))
  {
    if (b->binlog_id == binlog_id)
    {
      b->xid_count += xid_count;
      break;
    }
  }
  /*
    As we do not delete elements until count reach zero, elements should always
    be found.
  */
  DBUG_ASSERT(b);
  mysql_mutex_unlock(&LOCK_xid_list);
  DBUG_VOID_RETURN;
}

/*
  Once an XID is committed, it can no longer be needed during crash recovery,
  as it has been durably recorded on disk as "committed".

  This function is called to mark an XID this way. It needs to decrease the
  count of pending XIDs in the corresponding binlog. When the count reaches
  zero (for an "old" binlog that is not the active one), that binlog file no
  longer need to be scanned during crash recovery, so we can log a new binlog
  checkpoint.
*/
void
TC_LOG_BINLOG::mark_xid_done(ulong binlog_id, bool write_checkpoint)
{
  xid_count_per_binlog *b;
  bool first;
  ulong current;

  DBUG_ENTER("TC_LOG_BINLOG::mark_xid_done");

  mysql_mutex_lock(&LOCK_xid_list);
  current= current_binlog_id;
  I_List_iterator<xid_count_per_binlog> it(binlog_xid_count_list);
  first= true;
  while ((b= it++))
  {
    if (b->binlog_id == binlog_id)
    {
      --b->xid_count;
      break;
    }
    first= false;
  }
  /* Binlog is always found, as we do not remove until count reaches 0 */
  DBUG_ASSERT(b);
  /*
    If a RESET MASTER is pending, we are about to remove all log files, and
    the RESET MASTER thread is waiting for all pending unlog() calls to
    complete while holding LOCK_log. In this case we should not log a binlog
    checkpoint event (it would be deleted immediately anyway and we would
    deadlock on LOCK_log) but just signal the thread.
  */
  if (unlikely(reset_master_pending))
  {
    mysql_cond_signal(&COND_xid_list);
    mysql_mutex_unlock(&LOCK_xid_list);
    DBUG_VOID_RETURN;
  }

  if (likely(binlog_id == current) || b->xid_count != 0 || !first ||
      !write_checkpoint)
  {
    /* No new binlog checkpoint reached yet. */
    mysql_mutex_unlock(&LOCK_xid_list);
    DBUG_VOID_RETURN;
  }

  /*
    Now log a binlog checkpoint for the first binlog file with a non-zero count.

    Note that it is possible (though perhaps unlikely) that when count of
    binlog (N-2) drops to zero, binlog (N-1) is already at zero. So we may
    need to skip several entries before we find the one to log in the binlog
    checkpoint event.

    We chain the locking of LOCK_xid_list and LOCK_log, so that we ensure that
    Binlog_checkpoint_events are logged in order. This simplifies recovery a
    bit, as it can just take the last binlog checkpoint in the log, rather
    than compare all found against each other to find the one pointing to the
    most recent binlog.

    Note also that we need to first release LOCK_xid_list, then aquire
    LOCK_log, then re-aquire LOCK_xid_list. If we were to take LOCK_log while
    holding LOCK_xid_list, we might deadlock with other threads that take the
    locks in the opposite order.
  */

  ++mark_xid_done_waiting;
  mysql_mutex_unlock(&LOCK_xid_list);
  mysql_mutex_lock(&LOCK_log);
  mysql_mutex_lock(&LOCK_xid_list);
  --mark_xid_done_waiting;
  if (unlikely(reset_master_pending))
    mysql_cond_signal(&COND_xid_list);
  /* We need to reload current_binlog_id due to release/re-take of lock. */
  current= current_binlog_id;

  for (;;)
  {
    /* Remove initial element(s) with zero count. */
    b= binlog_xid_count_list.head();
    /*
      We must not remove all elements in the list - the entry for the current
      binlog must be present always.
    */
    DBUG_ASSERT(b);
    if (b->binlog_id == current || b->xid_count > 0)
      break;
    my_free(binlog_xid_count_list.get());
  }

  mysql_mutex_unlock(&LOCK_xid_list);
  write_binlog_checkpoint_event_already_locked(b->binlog_name,
                                               b->binlog_name_len);
  mysql_mutex_unlock(&LOCK_log);
  DBUG_VOID_RETURN;
}

int TC_LOG_BINLOG::unlog(ulong cookie, my_xid xid)
{
  DBUG_ENTER("TC_LOG_BINLOG::unlog");
  if (!xid)
    DBUG_RETURN(0);

  if (!BINLOG_COOKIE_IS_DUMMY(cookie))
    mark_xid_done(BINLOG_COOKIE_GET_ID(cookie), true);
  /*
    See comment in trx_group_commit_leader() - if rotate() gave a failure,
    we delay the return of error code to here.
  */
  DBUG_RETURN(BINLOG_COOKIE_GET_ERROR_FLAG(cookie));
}

void
TC_LOG_BINLOG::commit_checkpoint_notify(void *cookie)
{
  xid_count_per_binlog *entry= static_cast<xid_count_per_binlog *>(cookie);
  mysql_mutex_lock(&LOCK_binlog_background_thread);
  entry->next_in_queue= binlog_background_thread_queue;
  binlog_background_thread_queue= entry;
  mysql_cond_signal(&COND_binlog_background_thread);
  mysql_mutex_unlock(&LOCK_binlog_background_thread);
}

/*
  Binlog background thread.

  This thread is used to log binlog checkpoints in the background, rather than
  in the context of random storage engine threads that happen to call
  commit_checkpoint_notify_ha() and may not like the delays while syncing
  binlog to disk or may not be setup with all my_thread_init() and other
  necessary stuff.

  In the future, this thread could also be used to do log rotation in the
  background, which could elimiate all stalls around binlog rotations.
*/
pthread_handler_t
binlog_background_thread(void *arg __attribute__((unused)))
{
  bool stop;
  MYSQL_BIN_LOG::xid_count_per_binlog *queue, *next;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("binlog_background_thread");

  thd= new THD;
  thd->system_thread= SYSTEM_THREAD_BINLOG_BACKGROUND;
  thd->thread_stack= (char*) &thd;           /* Set approximate stack start */
  thd->thread_id= next_thread_id();
  thd->store_globals();
  thd->security_ctx->skip_grants();
  thd->set_command(COM_DAEMON);

  /*
    Load the slave replication GTID state from the mysql.gtid_slave_pos
    table.

    This is mostly so that we can start our seq_no counter from the highest
    seq_no seen by a slave. This way, we have a way to tell if a transaction
    logged by ourselves as master is newer or older than a replicated
    transaction.
  */
#ifdef HAVE_REPLICATION
  if (rpl_load_gtid_slave_state(thd))
    sql_print_warning("Failed to load slave replication state from table "
                      "%s.%s: %u: %s", "mysql",
                      rpl_gtid_slave_state_table_name.str,
                      thd->get_stmt_da()->sql_errno(),
                      thd->get_stmt_da()->message());
#endif

  mysql_mutex_lock(&mysql_bin_log.LOCK_binlog_background_thread);
  binlog_background_thread_started= true;
  mysql_cond_signal(&mysql_bin_log.COND_binlog_background_thread_end);
  mysql_mutex_unlock(&mysql_bin_log.LOCK_binlog_background_thread);

  for (;;)
  {
    /*
      Wait until there is something in the queue to process, or we are asked
      to shut down.
    */
    THD_STAGE_INFO(thd, stage_binlog_waiting_background_tasks);
    mysql_mutex_lock(&mysql_bin_log.LOCK_binlog_background_thread);
    for (;;)
    {
      stop= binlog_background_thread_stop;
      queue= binlog_background_thread_queue;
      if (stop && !mysql_bin_log.is_xidlist_idle())
      {
        /*
          Delay stop until all pending binlog checkpoints have been processed.
        */
        stop= false;
      }
      if (stop || queue)
        break;
      mysql_cond_wait(&mysql_bin_log.COND_binlog_background_thread,
                      &mysql_bin_log.LOCK_binlog_background_thread);
    }
    /* Grab the queue, if any. */
    binlog_background_thread_queue= NULL;
    mysql_mutex_unlock(&mysql_bin_log.LOCK_binlog_background_thread);

    /* Process any incoming commit_checkpoint_notify() calls. */
    DBUG_EXECUTE_IF("inject_binlog_background_thread_before_mark_xid_done",
      DBUG_ASSERT(!debug_sync_set_action(
        thd,
        STRING_WITH_LEN("binlog_background_thread_before_mark_xid_done "
                        "SIGNAL injected_binlog_background_thread "
                        "WAIT_FOR something_that_will_never_happen "
                        "TIMEOUT 2")));
      );
    while (queue)
    {
      THD_STAGE_INFO(thd, stage_binlog_processing_checkpoint_notify);
      DEBUG_SYNC(thd, "binlog_background_thread_before_mark_xid_done");
      /* Grab next pointer first, as mark_xid_done() may free the element. */
      next= queue->next_in_queue;
      mysql_bin_log.mark_xid_done(queue->binlog_id, true);
      queue= next;

      DBUG_EXECUTE_IF("binlog_background_checkpoint_processed",
        DBUG_ASSERT(!debug_sync_set_action(
          thd,
          STRING_WITH_LEN("now SIGNAL binlog_background_checkpoint_processed")));
        );
    }

    if (stop)
      break;
  }

  THD_STAGE_INFO(thd, stage_binlog_stopping_background_thread);

  /* No need to use mutex as thd is not linked into other threads */
  delete thd;

  my_thread_end();

  /* Signal that we are (almost) stopped. */
  mysql_mutex_lock(&mysql_bin_log.LOCK_binlog_background_thread);
  binlog_background_thread_stop= false;
  mysql_cond_signal(&mysql_bin_log.COND_binlog_background_thread_end);
  mysql_mutex_unlock(&mysql_bin_log.LOCK_binlog_background_thread);

  DBUG_RETURN(0);
}

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_binlog;

static PSI_thread_info all_binlog_threads[]=
{
  { &key_thread_binlog, "binlog_background", PSI_FLAG_GLOBAL},
};
#endif /* HAVE_PSI_INTERFACE */

static bool
start_binlog_background_thread()
{
  pthread_t th;

#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
    PSI_server->register_thread("sql", all_binlog_threads,
                                array_elements(all_binlog_threads));
#endif

  if (mysql_thread_create(key_thread_binlog, &th, &connection_attrib,
                          binlog_background_thread, NULL))
    return 1;

  /*
    Wait for the thread to have started (so we know that the slave replication
    state is loaded and we have correct global_gtid_counter).
  */
  mysql_mutex_lock(&mysql_bin_log.LOCK_binlog_background_thread);
  while (!binlog_background_thread_started)
    mysql_cond_wait(&mysql_bin_log.COND_binlog_background_thread_end,
                    &mysql_bin_log.LOCK_binlog_background_thread);
  mysql_mutex_unlock(&mysql_bin_log.LOCK_binlog_background_thread);

  return 0;
}


int TC_LOG_BINLOG::recover(LOG_INFO *linfo, const char *last_log_name,
                           IO_CACHE *first_log,
                           Format_description_log_event *fdle, bool do_xa)
{
  Log_event *ev= NULL;
  HASH xids;
  MEM_ROOT mem_root;
  char binlog_checkpoint_name[FN_REFLEN];
  bool binlog_checkpoint_found;
  bool first_round;
  IO_CACHE log;
  File file= -1;
  const char *errmsg;
#ifdef HAVE_REPLICATION
  rpl_gtid last_gtid;
  bool last_gtid_standalone= false;
  bool last_gtid_valid= false;
#endif

  if (! fdle->is_valid() ||
      (do_xa && my_hash_init(&xids, &my_charset_bin, TC_LOG_PAGE_SIZE/3, 0,
                             sizeof(my_xid), 0, 0, MYF(0))))
    goto err1;

  if (do_xa)
    init_alloc_root(&mem_root, TC_LOG_PAGE_SIZE, TC_LOG_PAGE_SIZE, MYF(0));

  fdle->flags&= ~LOG_EVENT_BINLOG_IN_USE_F; // abort on the first error

  /*
    Scan the binlog for XIDs that need to be committed if still in the
    prepared stage.

    Start with the latest binlog file, then continue with any other binlog
    files if the last found binlog checkpoint indicates it is needed.
  */

  binlog_checkpoint_found= false;
  first_round= true;
  for (;;)
  {
    while ((ev= Log_event::read_log_event(first_round ? first_log : &log,
                                          0, fdle, opt_master_verify_checksum))
           && ev->is_valid())
    {
      enum Log_event_type typ= ev->get_type_code();
      switch (typ)
      {
      case XID_EVENT:
      {
        if (do_xa)
        {
          Xid_log_event *xev=(Xid_log_event *)ev;
          uchar *x= (uchar *) memdup_root(&mem_root, (uchar*) &xev->xid,
                                          sizeof(xev->xid));
          if (!x || my_hash_insert(&xids, x))
            goto err2;
          break;
        }
      }
      case BINLOG_CHECKPOINT_EVENT:
        if (first_round && do_xa)
        {
          uint dir_len;
          Binlog_checkpoint_log_event *cev= (Binlog_checkpoint_log_event *)ev;
          if (cev->binlog_file_len >= FN_REFLEN)
            sql_print_warning("Incorrect binlog checkpoint event with too "
                              "long file name found.");
          else
          {
            /*
              Note that we cannot use make_log_name() here, as we have not yet
              initialised MYSQL_BIN_LOG::log_file_name.
            */
            dir_len= dirname_length(last_log_name);
            strmake(strnmov(binlog_checkpoint_name, last_log_name, dir_len),
                    cev->binlog_file_name, FN_REFLEN - 1 - dir_len);
            binlog_checkpoint_found= true;
          }
        }
        break;
      case GTID_LIST_EVENT:
        if (first_round)
        {
          Gtid_list_log_event *glev= (Gtid_list_log_event *)ev;

          /* Initialise the binlog state from the Gtid_list event. */
          if (rpl_global_gtid_binlog_state.load(glev->list, glev->count))
            goto err2;
        }
        break;

#ifdef HAVE_REPLICATION
      case GTID_EVENT:
        if (first_round)
        {
          Gtid_log_event *gev= (Gtid_log_event *)ev;

          /* Update the binlog state with any GTID logged after Gtid_list. */
          last_gtid.domain_id= gev->domain_id;
          last_gtid.server_id= gev->server_id;
          last_gtid.seq_no= gev->seq_no;
          last_gtid_standalone=
            ((gev->flags2 & Gtid_log_event::FL_STANDALONE) ? true : false);
          last_gtid_valid= true;
        }
        break;
#endif

      case START_ENCRYPTION_EVENT:
        {
          if (fdle->start_decryption((Start_encryption_log_event*) ev))
            goto err2;
        }
        break;

      default:
        /* Nothing. */
        break;
      }

#ifdef HAVE_REPLICATION
      if (last_gtid_valid &&
          ((last_gtid_standalone && !ev->is_part_of_group(typ)) ||
           (!last_gtid_standalone &&
            (typ == XID_EVENT ||
             (typ == QUERY_EVENT &&
              (((Query_log_event *)ev)->is_commit() ||
               ((Query_log_event *)ev)->is_rollback()))))))
      {
        if (rpl_global_gtid_binlog_state.update_nolock(&last_gtid, false))
          goto err2;
        last_gtid_valid= false;
      }
#endif

      delete ev;
      ev= NULL;
    }

    if (!do_xa)
      break;
    /*
      If the last binlog checkpoint event points to an older log, we have to
      scan all logs from there also, to get all possible XIDs to recover.

      If there was no binlog checkpoint event at all, this means the log was
      written by an older version of MariaDB (or MySQL) - these always have an
      (implicit) binlog checkpoint event at the start of the last binlog file.
    */
    if (first_round)
    {
      if (!binlog_checkpoint_found)
        break;
      first_round= false;
      DBUG_EXECUTE_IF("xa_recover_expect_master_bin_000004",
          if (0 != strcmp("./master-bin.000004", binlog_checkpoint_name) &&
              0 != strcmp(".\\master-bin.000004", binlog_checkpoint_name))
            DBUG_SUICIDE();
        );
      if (find_log_pos(linfo, binlog_checkpoint_name, 1))
      {
        sql_print_error("Binlog file '%s' not found in binlog index, needed "
                        "for recovery. Aborting.", binlog_checkpoint_name);
        goto err2;
      }
    }
    else
    {
      end_io_cache(&log);
      mysql_file_close(file, MYF(MY_WME));
      file= -1;
    }

    if (!strcmp(linfo->log_file_name, last_log_name))
      break;                                    // No more files to do
    if ((file= open_binlog(&log, linfo->log_file_name, &errmsg)) < 0)
    {
      sql_print_error("%s", errmsg);
      goto err2;
    }
    /*
      We do not need to read the Format_description_log_event of other binlog
      files. It is not possible for a binlog checkpoint to span multiple
      binlog files written by different versions of the server. So we can use
      the first one read for reading from all binlog files.
    */
    if (find_next_log(linfo, 1))
    {
      sql_print_error("Error reading binlog files during recovery. Aborting.");
      goto err2;
    }
    fdle->reset_crypto();
  }

  if (do_xa)
  {
    if (ha_recover(&xids))
      goto err2;

    free_root(&mem_root, MYF(0));
    my_hash_free(&xids);
  }
  return 0;

err2:
  delete ev;
  if (file >= 0)
  {
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));
  }
  if (do_xa)
  {
    free_root(&mem_root, MYF(0));
    my_hash_free(&xids);
  }
err1:
  sql_print_error("Crash recovery failed. Either correct the problem "
                  "(if it's, for example, out of memory error) and restart, "
                  "or delete (or rename) binary log and start mysqld with "
                  "--tc-heuristic-recover={commit|rollback}");
  return 1;
}


int
MYSQL_BIN_LOG::do_binlog_recovery(const char *opt_name, bool do_xa_recovery)
{
  LOG_INFO log_info;
  const char *errmsg;
  IO_CACHE    log;
  File        file;
  Log_event  *ev= 0;
  Format_description_log_event fdle(BINLOG_VERSION);
  char        log_name[FN_REFLEN];
  int error;

  if ((error= find_log_pos(&log_info, NullS, 1)))
  {
    /*
      If there are no binlog files (LOG_INFO_EOF), then we still try to read
      the .state file to restore the binlog state. This allows to copy a server
      to provision a new one without copying the binlog files (except the
      master-bin.state file) and still preserve the correct binlog state.
    */
    if (error != LOG_INFO_EOF)
      sql_print_error("find_log_pos() failed (error: %d)", error);
    else
    {
      error= read_state_from_file();
      if (error == 2)
      {
        /*
          No binlog files and no binlog state is not an error (eg. just initial
          server start after fresh installation).
        */
        error= 0;
      }
    }
    return error;
  }

  if (! fdle.is_valid())
    return 1;

  do
  {
    strmake_buf(log_name, log_info.log_file_name);
  } while (!(error= find_next_log(&log_info, 1)));

  if (error !=  LOG_INFO_EOF)
  {
    sql_print_error("find_log_pos() failed (error: %d)", error);
    return error;
  }

  if ((file= open_binlog(&log, log_name, &errmsg)) < 0)
  {
    sql_print_error("%s", errmsg);
    return 1;
  }

  if ((ev= Log_event::read_log_event(&log, 0, &fdle,
                                     opt_master_verify_checksum)) &&
      ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
  {
    if (ev->flags & LOG_EVENT_BINLOG_IN_USE_F)
    {
      sql_print_information("Recovering after a crash using %s", opt_name);
      error= recover(&log_info, log_name, &log,
                     (Format_description_log_event *)ev, do_xa_recovery);
    }
    else
    {
      error= read_state_from_file();
      if (error == 2)
      {
        /*
          The binlog exists, but the .state file is missing. This is normal if
          this is the first master start after a major upgrade to 10.0 (with
          GTID support).

          However, it could also be that the .state file was lost somehow, and
          in this case it could be a serious issue, as we would set the wrong
          binlog state in the next binlog file to be created, and GTID
          processing would be corrupted. A common way would be copying files
          from an old server to a new one and forgetting the .state file.

          So in this case, we want to try to recover the binlog state by
          scanning the last binlog file (but we do not need any XA recovery).

          ToDo: We could avoid one scan at first start after major upgrade, by
          detecting that there is no GTID_LIST event at the start of the
          binlog file, and stopping the scan in that case.
        */
        error= recover(&log_info, log_name, &log,
                       (Format_description_log_event *)ev, false);
      }
    }
  }

  delete ev;
  end_io_cache(&log);
  mysql_file_close(file, MYF(MY_WME));

  return error;
}


#ifdef INNODB_COMPATIBILITY_HOOKS
/**
  Get the file name of the MySQL binlog.
  @return the name of the binlog file
*/
extern "C"
const char* mysql_bin_log_file_name(void)
{
  return mysql_bin_log.get_log_fname();
}
/**
  Get the current position of the MySQL binlog.
  @return byte offset from the beginning of the binlog
*/
extern "C"
ulonglong mysql_bin_log_file_pos(void)
{
  return (ulonglong) mysql_bin_log.get_log_file()->pos_in_file;
}
/*
  Get the current position of the MySQL binlog for transaction currently being
  committed.

  This is valid to call from within storage engine commit_ordered() and
  commit() methods only.

  Since it stores the position inside THD, it is safe to call without any
  locking.
*/
void
mysql_bin_log_commit_pos(THD *thd, ulonglong *out_pos, const char **out_file)
{
  binlog_cache_mngr *cache_mngr;
  if (opt_bin_log &&
      (cache_mngr= (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton)))
  {
    *out_file= cache_mngr->last_commit_pos_file;
    *out_pos= (ulonglong)(cache_mngr->last_commit_pos_offset);
  }
  else
  {
    *out_file= NULL;
    *out_pos= 0;
  }
}
#endif /* INNODB_COMPATIBILITY_HOOKS */


static void
binlog_checksum_update(MYSQL_THD thd, struct st_mysql_sys_var *var,
                       void *var_ptr, const void *save)
{
  ulong value=  *((ulong *)save);
  bool check_purge= false;
  ulong UNINIT_VAR(prev_binlog_id);

  mysql_mutex_lock(mysql_bin_log.get_log_lock());
  if(mysql_bin_log.is_open())
  {
    prev_binlog_id= mysql_bin_log.current_binlog_id;
    if (binlog_checksum_options != value)
      mysql_bin_log.checksum_alg_reset= (enum_binlog_checksum_alg)value;
    if (mysql_bin_log.rotate(true, &check_purge))
      check_purge= false;
  }
  else
  {
    binlog_checksum_options= value;
  }
  DBUG_ASSERT(binlog_checksum_options == value);
  mysql_bin_log.checksum_alg_reset= BINLOG_CHECKSUM_ALG_UNDEF;
  mysql_mutex_unlock(mysql_bin_log.get_log_lock());
  if (check_purge)
    mysql_bin_log.checkpoint_and_purge(prev_binlog_id);
}


static int show_binlog_vars(THD *thd, SHOW_VAR *var, char *buff)
{
  mysql_bin_log.set_status_variables(thd);
  var->type= SHOW_ARRAY;
  var->value= (char *)&binlog_status_vars_detail;
  return 0;
}

static SHOW_VAR binlog_status_vars_top[]= {
  {"Binlog", (char *) &show_binlog_vars, SHOW_FUNC},
  {NullS, NullS, SHOW_LONG}
};

static MYSQL_SYSVAR_BOOL(
  optimize_thread_scheduling,
  opt_optimize_thread_scheduling,
  PLUGIN_VAR_READONLY,
  "Run fast part of group commit in a single thread, to optimize kernel "
  "thread scheduling. On by default. Disable to run each transaction in group "
  "commit in its own thread, which can be slower at very high concurrency. "
  "This option is mostly for testing one algorithm versus the other, and it "
  "should not normally be necessary to change it.",
  NULL,
  NULL,
  1);

static MYSQL_SYSVAR_ENUM(
  checksum,
  binlog_checksum_options,
  PLUGIN_VAR_RQCMDARG,
  "Type of BINLOG_CHECKSUM_ALG. Include checksum for "
  "log events in the binary log",
  NULL,
  binlog_checksum_update,
  BINLOG_CHECKSUM_ALG_OFF,
  &binlog_checksum_typelib);

static struct st_mysql_sys_var *binlog_sys_vars[]=
{
  MYSQL_SYSVAR(optimize_thread_scheduling),
  MYSQL_SYSVAR(checksum),
  NULL
};


/*
  Copy out the non-directory part of binlog position filename for the
  `binlog_snapshot_file' status variable, same way as it is done for
  SHOW MASTER STATUS.
*/
static void
set_binlog_snapshot_file(const char *src)
{
  int dir_len = dirname_length(src);
  strmake_buf(binlog_snapshot_file, src + dir_len);
}

/*
  Copy out current values of status variables, for SHOW STATUS or
  information_schema.global_status.

  This is called only under LOCK_show_status, so we can fill in a static array.
*/
void
TC_LOG_BINLOG::set_status_variables(THD *thd)
{
  binlog_cache_mngr *cache_mngr;

  if (thd && opt_bin_log)
    cache_mngr= (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
  else
    cache_mngr= 0;

  bool have_snapshot= (cache_mngr && cache_mngr->last_commit_pos_file[0] != 0);
  mysql_mutex_lock(&LOCK_commit_ordered);
  binlog_status_var_num_commits= this->num_commits;
  binlog_status_var_num_group_commits= this->num_group_commits;
  if (!have_snapshot)
  {
    set_binlog_snapshot_file(last_commit_pos_file);
    binlog_snapshot_position= last_commit_pos_offset;
  }
  mysql_mutex_unlock(&LOCK_commit_ordered);
  mysql_mutex_lock(&LOCK_prepare_ordered);
  binlog_status_group_commit_trigger_count= this->group_commit_trigger_count;
  binlog_status_group_commit_trigger_timeout= this->group_commit_trigger_timeout;
  binlog_status_group_commit_trigger_lock_wait= this->group_commit_trigger_lock_wait;
  mysql_mutex_unlock(&LOCK_prepare_ordered);

  if (have_snapshot)
  {
    set_binlog_snapshot_file(cache_mngr->last_commit_pos_file);
    binlog_snapshot_position= cache_mngr->last_commit_pos_offset;
  }
}

struct st_mysql_storage_engine binlog_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(binlog)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &binlog_storage_engine,
  "binlog",
  "MySQL AB",
  "This is a pseudo storage engine to represent the binlog in a transaction",
  PLUGIN_LICENSE_GPL,
  binlog_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  binlog_status_vars_top,     /* status variables                */
  binlog_sys_vars,            /* system variables                */
  "1.0",                      /* string version */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
}
maria_declare_plugin_end;

#ifdef WITH_WSREP
IO_CACHE * get_trans_log(THD * thd)
{
  DBUG_ASSERT(binlog_hton->slot != HA_SLOT_UNDEF);
  binlog_cache_mngr *cache_mngr = (binlog_cache_mngr*)
    thd_get_ha_data(thd, binlog_hton);
  if (cache_mngr)
    return cache_mngr->get_binlog_cache_log(true);

  WSREP_DEBUG("binlog cache not initialized, conn: %lld",
              (longlong) thd->thread_id);
  return NULL;
}


bool wsrep_trans_cache_is_empty(THD *thd)
{
  binlog_cache_mngr *const cache_mngr=
      (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
  return (!cache_mngr || cache_mngr->trx_cache.empty());
}


void thd_binlog_trx_reset(THD * thd)
{
  /*
    todo: fix autocommit select to not call the caller
  */
  if (thd_get_ha_data(thd, binlog_hton) != NULL)
  {
    binlog_cache_mngr *const cache_mngr=
      (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
    if (cache_mngr)
    {
      cache_mngr->reset(false, true);
      if (!cache_mngr->stmt_cache.empty())
      {
        WSREP_DEBUG("pending events in stmt cache, sql: %s", thd->query());
        cache_mngr->stmt_cache.reset();
      }
    }
  }
  thd->clear_binlog_table_maps();
}


void thd_binlog_rollback_stmt(THD * thd)
{
  WSREP_DEBUG("thd_binlog_rollback_stmt connection: %lld",
              (longlong) thd->thread_id);
  binlog_cache_mngr *const cache_mngr=
    (binlog_cache_mngr*) thd_get_ha_data(thd, binlog_hton);
  if (cache_mngr)
    cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF);
}
#endif /* WITH_WSREP */
