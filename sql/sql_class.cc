/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


/*****************************************************************************
**
** This file implements classes defined in sql_class.h
** Especially the classes to handle a result from a select
**
*****************************************************************************/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_cache.h"                          // query_cache_abort
#include "sql_base.h"                           // close_thread_tables
#include "sql_time.h"                         // date_time_format_copy
#include "tztime.h"                           // MYSQL_TIME <-> my_time_t
#include "sql_acl.h"                          // NO_ACL,
                                              // acl_getroot_no_password
#include "sql_base.h"
#include "sql_handler.h"                      // mysql_ha_cleanup
#include "rpl_rli.h"
#include "rpl_filter.h"
#include "rpl_record.h"
#include "slave.h"
#include <my_bitmap.h>
#include "log_event.h"
#include "sql_audit.h"
#include <m_ctype.h>
#include <sys/stat.h>
#include <thr_alarm.h>
#include <mysys_err.h>
#include <limits.h>

#include "sp_head.h"
#include "sp_rcontext.h"
#include "sp_cache.h"
#include "sql_show.h"                           // append_identifier
#include "transaction.h"
#include "sql_select.h" /* declares create_tmp_table() */
#include "debug_sync.h"
#include "sql_parse.h"                          // is_update_query
#include "sql_callback.h"
#include "lock.h"
#include "wsrep_mysqld.h"
#include "sql_connect.h"
#ifdef WITH_WSREP
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"
#else
static inline bool wsrep_is_bf_aborted(THD* thd) { return false; }
#endif /* WITH_WSREP */
#include "opt_trace.h"
#include <mysql/psi/mysql_transaction.h>

#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

/*
  The following is used to initialise Table_ident with a internal
  table name
*/
char internal_table_name[2]= "*";
char empty_c_string[1]= {0};    /* used for not defined db */

const char * const THD::DEFAULT_WHERE= "field list";

/****************************************************************************
** User variables
****************************************************************************/

extern "C" uchar *get_var_key(user_var_entry *entry, size_t *length,
                              my_bool not_used __attribute__((unused)))
{
  *length= entry->name.length;
  return (uchar*) entry->name.str;
}

extern "C" void free_user_var(user_var_entry *entry)
{
  char *pos= (char*) entry+ALIGN_SIZE(sizeof(*entry));
  if (entry->value && entry->value != pos)
    my_free(entry->value);
  my_free(entry);
}

/* Functions for last-value-from-sequence hash */

extern "C" uchar *get_sequence_last_key(SEQUENCE_LAST_VALUE *entry,
                                        size_t *length,
                                        my_bool not_used
                                        __attribute__((unused)))
{
  *length= entry->length;
  return (uchar*) entry->key;
}

extern "C" void free_sequence_last(SEQUENCE_LAST_VALUE *entry)
{
  delete entry;
}


bool Key_part_spec::operator==(const Key_part_spec& other) const
{
  return length == other.length &&
         !lex_string_cmp(system_charset_info, &field_name,
                         &other.field_name);
}


bool Key_part_spec::check_key_for_blob(const handler *file) const
{
  if (!(file->ha_table_flags() & HA_CAN_INDEX_BLOBS))
  {
    my_error(ER_BLOB_USED_AS_KEY, MYF(0), field_name.str, file->table_type());
    return true;
  }
  return false;
}


bool Key_part_spec::check_key_length_for_blob() const
{
  if (!length)
  {
    my_error(ER_BLOB_KEY_WITHOUT_LENGTH, MYF(0), field_name.str);
    return true;
  }
  return false;
}


bool Key_part_spec::init_multiple_key_for_blob(const handler *file)
{
  if (check_key_for_blob(file))
    return true;
  if (!length)
    length= file->max_key_length() + 1;
  return false;
}


/**
  Construct an (almost) deep copy of this key. Only those
  elements that are known to never change are not copied.
  If out of memory, a partial copy is returned and an error is set
  in THD.
*/

Key::Key(const Key &rhs, MEM_ROOT *mem_root)
  :DDL_options(rhs),type(rhs.type),
  key_create_info(rhs.key_create_info),
  columns(rhs.columns, mem_root),
  name(rhs.name),
  option_list(rhs.option_list),
  generated(rhs.generated), invisible(false),
  without_overlaps(rhs.without_overlaps), period(rhs.period)
{
  list_copy_and_replace_each_value(columns, mem_root);
}

/**
  Construct an (almost) deep copy of this foreign key. Only those
  elements that are known to never change are not copied.
  If out of memory, a partial copy is returned and an error is set
  in THD.
*/

Foreign_key::Foreign_key(const Foreign_key &rhs, MEM_ROOT *mem_root)
  :Key(rhs,mem_root),
  constraint_name(rhs.constraint_name),
  ref_db(rhs.ref_db),
  ref_table(rhs.ref_table),
  ref_columns(rhs.ref_columns,mem_root),
  delete_opt(rhs.delete_opt),
  update_opt(rhs.update_opt),
  match_opt(rhs.match_opt)
{
  list_copy_and_replace_each_value(ref_columns, mem_root);
}

/*
  Test if a foreign key (= generated key) is a prefix of the given key
  (ignoring key name, key type and order of columns)

  NOTES:
    This is only used to test if an index for a FOREIGN KEY exists

  IMPLEMENTATION
    We only compare field names

  RETURN
    0	Generated key is a prefix of other key
    1	Not equal
*/

bool foreign_key_prefix(Key *a, Key *b)
{
  /* Ensure that 'a' is the generated key */
  if (a->generated)
  {
    if (b->generated && a->columns.elements > b->columns.elements)
      swap_variables(Key*, a, b);               // Put shorter key in 'a'
  }
  else
  {
    if (!b->generated)
      return TRUE;                              // No foreign key
    swap_variables(Key*, a, b);                 // Put generated key in 'a'
  }

  /* Test if 'a' is a prefix of 'b' */
  if (a->columns.elements > b->columns.elements)
    return TRUE;                                // Can't be prefix

  List_iterator<Key_part_spec> col_it1(a->columns);
  List_iterator<Key_part_spec> col_it2(b->columns);
  const Key_part_spec *col1, *col2;

#ifdef ENABLE_WHEN_INNODB_CAN_HANDLE_SWAPED_FOREIGN_KEY_COLUMNS
  while ((col1= col_it1++))
  {
    bool found= 0;
    col_it2.rewind();
    while ((col2= col_it2++))
    {
      if (*col1 == *col2)
      {
        found= TRUE;
	break;
      }
    }
    if (!found)
      return TRUE;                              // Error
  }
  return FALSE;                                 // Is prefix
#else
  while ((col1= col_it1++))
  {
    col2= col_it2++;
    if (!(*col1 == *col2))
      return TRUE;
  }
  return FALSE;                                 // Is prefix
#endif
}

/*
  @brief
  Check if the foreign key options are compatible with the specification
  of the columns on which the key is created

  @retval
    FALSE   The foreign key options are compatible with key columns
  @retval
    TRUE    Otherwise
*/
bool Foreign_key::validate(List<Create_field> &table_fields)
{
  Create_field  *sql_field;
  Key_part_spec *column;
  List_iterator<Key_part_spec> cols(columns);
  List_iterator<Create_field> it(table_fields);
  DBUG_ENTER("Foreign_key::validate");
  while ((column= cols++))
  {
    it.rewind();
    while ((sql_field= it++) &&
           lex_string_cmp(system_charset_info,
                          &column->field_name,
                          &sql_field->field_name)) {}
    if (!sql_field)
    {
      my_error(ER_KEY_COLUMN_DOES_NOT_EXIST, MYF(0), column->field_name.str);
      DBUG_RETURN(TRUE);
    }
    if (type == Key::FOREIGN_KEY && sql_field->vcol_info)
    {
      if (delete_opt == FK_OPTION_SET_NULL)
      {
        my_error(ER_WRONG_FK_OPTION_FOR_VIRTUAL_COLUMN, MYF(0), 
                 "ON DELETE SET NULL");
        DBUG_RETURN(TRUE);
      }
      if (update_opt == FK_OPTION_SET_NULL)
      {
        my_error(ER_WRONG_FK_OPTION_FOR_VIRTUAL_COLUMN, MYF(0), 
                 "ON UPDATE SET NULL");
        DBUG_RETURN(TRUE);
      }
      if (update_opt == FK_OPTION_CASCADE)
      {
        my_error(ER_WRONG_FK_OPTION_FOR_VIRTUAL_COLUMN, MYF(0), 
                 "ON UPDATE CASCADE");
        DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);
}

/****************************************************************************
** Thread specific functions
****************************************************************************/

extern "C" unsigned long long thd_query_id(const MYSQL_THD thd)
{
  return((unsigned long long)thd->query_id);
}


/**
  Get thread attributes for connection threads

  @retval      Reference to thread attribute for connection threads
*/
pthread_attr_t *get_connection_attrib(void)
{
  return &connection_attrib;
}

/**
  Get max number of connections

  @retval         Max number of connections for MySQL Server
*/
ulong get_max_connections(void)
{
  return max_connections;
}

/*
  The following functions form part of the C plugin API
*/

extern "C" int mysql_tmpfile(const char *prefix)
{
  char filename[FN_REFLEN];
  File fd= create_temp_file(filename, mysql_tmpdir, prefix,
                            O_BINARY | O_SEQUENTIAL,
                            MYF(MY_WME | MY_TEMPORARY));
  return fd;
}


extern "C"
int thd_in_lock_tables(const THD *thd)
{
  return MY_TEST(thd->in_lock_tables);
}


extern "C"
int thd_tablespace_op(const THD *thd)
{
  return MY_TEST(thd->tablespace_op);
}

extern "C"
const char *set_thd_proc_info(THD *thd_arg, const char *info,
                              const char *calling_function,
                              const char *calling_file,
                              const unsigned int calling_line)
{
  PSI_stage_info old_stage;
  PSI_stage_info new_stage;

  new_stage.m_key= 0;
  new_stage.m_name= info;

  set_thd_stage_info(thd_arg, & new_stage, & old_stage,
                     calling_function, calling_file, calling_line);

  return old_stage.m_name;
}

extern "C"
void set_thd_stage_info(void *thd_arg,
                        const PSI_stage_info *new_stage,
                        PSI_stage_info *old_stage,
                        const char *calling_func,
                        const char *calling_file,
                        const unsigned int calling_line)
{
  THD *thd= (THD*) thd_arg;
  if (thd == NULL)
    thd= current_thd;

  if (old_stage)
    thd->backup_stage(old_stage);

  if (new_stage)
    thd->enter_stage(new_stage, calling_func, calling_file, calling_line);
}

void thd_enter_cond(MYSQL_THD thd, mysql_cond_t *cond, mysql_mutex_t *mutex,
                    const PSI_stage_info *stage, PSI_stage_info *old_stage,
                    const char *src_function, const char *src_file,
                    int src_line)
{
  if (!thd)
    thd= current_thd;

  return thd->enter_cond(cond, mutex, stage, old_stage, src_function, src_file,
                         src_line);
}

void thd_exit_cond(MYSQL_THD thd, const PSI_stage_info *stage,
                   const char *src_function, const char *src_file,
                   int src_line)
{
  if (!thd)
    thd= current_thd;

  thd->exit_cond(stage, src_function, src_file, src_line);
  return;
}

extern "C"
void thd_storage_lock_wait(THD *thd, long long value)
{
  thd->utime_after_lock+= value;
}

/**
  Provide a handler data getter to simplify coding
*/
extern "C"
void *thd_get_ha_data(const THD *thd, const struct handlerton *hton)
{
  return thd->ha_data[hton->slot].ha_ptr;
}


/**
  Provide a handler data setter to simplify coding
  @see thd_set_ha_data() definition in plugin.h
*/
extern "C"
void thd_set_ha_data(THD *thd, const struct handlerton *hton,
                     const void *ha_data)
{
  plugin_ref *lock= &thd->ha_data[hton->slot].lock;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  thd->ha_data[hton->slot].ha_ptr= const_cast<void*>(ha_data);
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  if (ha_data && !*lock)
    *lock= ha_lock_engine(NULL, (handlerton*) hton);
  else if (!ha_data && *lock)
  {
    plugin_unlock(NULL, *lock);
    *lock= NULL;
  }
}


/**
  Allow storage engine to wakeup commits waiting in THD::wait_for_prior_commit.
  @see thd_wakeup_subsequent_commits() definition in plugin.h
*/
extern "C"
void thd_wakeup_subsequent_commits(THD *thd, int wakeup_error)
{
  thd->wakeup_subsequent_commits(wakeup_error);
}


extern "C"
long long thd_test_options(const THD *thd, long long test_options)
{
  return thd->variables.option_bits & test_options;
}

extern "C"
int thd_sql_command(const THD *thd)
{
  return (int) thd->lex->sql_command;
}

/*
  Returns options used with DDL's, like IF EXISTS etc...
  Will returns 'nonsense' if the command was not a DDL.
*/

extern "C"
struct DDL_options_st *thd_ddl_options(const THD *thd)
{
  return &thd->lex->create_info;
}


extern "C"
int thd_tx_isolation(const THD *thd)
{
  return (int) thd->tx_isolation;
}

extern "C"
int thd_tx_is_read_only(const THD *thd)
{
  return (int) thd->tx_read_only;
}


extern "C"
{ /* Functions for thd_error_context_service */

  const char *thd_get_error_message(const THD *thd)
  {
    return thd->get_stmt_da()->message();
  }

  uint thd_get_error_number(const THD *thd)
  {
    return thd->get_stmt_da()->sql_errno();
  }

  ulong thd_get_error_row(const THD *thd)
  {
    return thd->get_stmt_da()->current_row_for_warning();
  }

  void thd_inc_error_row(THD *thd)
  {
    thd->get_stmt_da()->inc_current_row_for_warning();
  }
}


#if MARIA_PLUGIN_INTERFACE_VERSION < 0x0200
/**
  TODO: This function is for API compatibility, remove it eventually.
  All engines should switch to use thd_get_error_context_description()
  plugin service function.
*/
extern "C"
char *thd_security_context(THD *thd,
                           char *buffer, unsigned int length,
                           unsigned int max_query_len)
{
  return thd_get_error_context_description(thd, buffer, length, max_query_len);
}
#endif

/**
  Implementation of Drop_table_error_handler::handle_condition().
  The reason in having this implementation is to silence technical low-level
  warnings during DROP TABLE operation. Currently we don't want to expose
  the following warnings during DROP TABLE:
    - Some of table files are missed or invalid (the table is going to be
      deleted anyway, so why bother that something was missed);
    - A trigger associated with the table does not have DEFINER (One of the
      MySQL specifics now is that triggers are loaded for the table being
      dropped. So, we may have a warning that trigger does not have DEFINER
      attribute during DROP TABLE operation).

  @return TRUE if the condition is handled.
*/
bool Drop_table_error_handler::handle_condition(THD *thd,
                                                uint sql_errno,
                                                const char* sqlstate,
                                                Sql_condition::enum_warning_level *level,
                                                const char* msg,
                                                Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  return ((sql_errno == EE_DELETE && my_errno == ENOENT) ||
          sql_errno == ER_TRG_NO_DEFINER);
}


/**
  Handle an error from MDL_context::upgrade_lock() and mysql_lock_tables().
  Ignore ER_LOCK_ABORTED and ER_LOCK_DEADLOCK errors.
*/

bool
MDL_deadlock_and_lock_abort_error_handler::
handle_condition(THD *thd,
                 uint sql_errno,
                 const char *sqlstate,
                 Sql_condition::enum_warning_level *level,
                 const char* msg,
                 Sql_condition **cond_hdl)
{
  *cond_hdl= NULL;
  if (sql_errno == ER_LOCK_ABORTED || sql_errno == ER_LOCK_DEADLOCK)
    m_need_reopen= true;

  return m_need_reopen;
}


/**
   Send timeout to thread.

   Note that this is always safe as the thread will always remove it's
   timeouts at end of query (and thus before THD is destroyed)
*/

extern "C" void thd_kill_timeout(THD* thd)
{
  thd->status_var.max_statement_time_exceeded++;
  /* Kill queries that can't cause data corruptions */
  thd->awake(KILL_TIMEOUT);
}

THD::THD(my_thread_id id, bool is_wsrep_applier)
  :Statement(&main_lex, &main_mem_root, STMT_CONVENTIONAL_EXECUTION,
             /* statement id */ 0),
   rli_fake(0), rgi_fake(0), rgi_slave(NULL),
   protocol_text(this), protocol_binary(this), initial_status_var(0),
   m_current_stage_key(0), m_psi(0),
   in_sub_stmt(0), log_all_errors(0),
   binlog_unsafe_warning_flags(0),
   current_stmt_binlog_format(BINLOG_FORMAT_MIXED),
   bulk_param(0),
   table_map_for_update(0),
   m_examined_row_count(0),
   accessed_rows_and_keys(0),
   m_digest(NULL),
   m_statement_psi(NULL),
   m_transaction_psi(NULL),
   m_idle_psi(NULL),
   col_access(NO_ACL),
   thread_id(id),
   thread_dbug_id(id),
   os_thread_id(0),
   global_disable_checkpoint(0),
   current_backup_stage(BACKUP_FINISHED),
   failed_com_change_user(0),
   is_fatal_error(0),
   transaction_rollback_request(0),
   is_fatal_sub_stmt_error(false),
   rand_used(0),
   time_zone_used(0),
   in_lock_tables(0),
   bootstrap(0),
   derived_tables_processing(FALSE),
   waiting_on_group_commit(FALSE), has_waiter(FALSE),
   spcont(NULL),
   m_parser_state(NULL),
#ifndef EMBEDDED_LIBRARY
   audit_plugin_version(-1),
#endif
#if defined(ENABLED_DEBUG_SYNC)
   debug_sync_control(0),
#endif /* defined(ENABLED_DEBUG_SYNC) */
   wait_for_commit_ptr(0),
   m_internal_handler(0),
   main_da(0, false, false),
   m_stmt_da(&main_da),
   tdc_hash_pins(0),
   xid_hash_pins(0),
   m_tmp_tables_locked(false),
   async_state()
#ifdef HAVE_REPLICATION
   ,
   current_linfo(0),
   slave_info(0),
   is_awaiting_semisync_ack(0)
#endif
#ifdef WITH_WSREP
   ,
   wsrep_applier(is_wsrep_applier),
   wsrep_applier_closing(false),
   wsrep_client_thread(false),
   wsrep_retry_counter(0),
   wsrep_PA_safe(true),
   wsrep_retry_query(NULL),
   wsrep_retry_query_len(0),
   wsrep_retry_command(COM_CONNECT),
   wsrep_consistency_check(NO_CONSISTENCY_CHECK),
   wsrep_mysql_replicated(0),
   wsrep_TOI_pre_query(NULL),
   wsrep_TOI_pre_query_len(0),
   wsrep_po_handle(WSREP_PO_INITIALIZER),
   wsrep_po_cnt(0),
   wsrep_apply_format(0),
   wsrep_rbr_buf(NULL),
   wsrep_sync_wait_gtid(WSREP_GTID_UNDEFINED),
   wsrep_last_written_gtid_seqno(0),
   wsrep_current_gtid_seqno(0),
   wsrep_affected_rows(0),
   wsrep_has_ignored_error(false),
   wsrep_was_on(false),
   wsrep_ignore_table(false),
   wsrep_aborter(0),
   wsrep_delayed_BF_abort(false),

/* wsrep-lib */
   m_wsrep_next_trx_id(WSREP_UNDEFINED_TRX_ID),
   m_wsrep_mutex(&LOCK_thd_data),
   m_wsrep_cond(&COND_wsrep_thd),
   m_wsrep_client_service(this, m_wsrep_client_state),
   m_wsrep_client_state(this,
                        m_wsrep_mutex,
                        m_wsrep_cond,
                        Wsrep_server_state::instance(),
                        m_wsrep_client_service,
                        wsrep::client_id(thread_id)),
   wsrep_applier_service(NULL),
   wsrep_wfc()
#endif /*WITH_WSREP */
{
  ulong tmp;
  bzero(&variables, sizeof(variables));

  /*
    We set THR_THD to temporally point to this THD to register all the
    variables that allocates memory for this THD
  */
  THD *old_THR_THD= current_thd;
  set_current_thd(this);
  status_var.local_memory_used= sizeof(THD);
  status_var.max_local_memory_used= status_var.local_memory_used;
  status_var.global_memory_used= 0;
  variables.pseudo_thread_id= thread_id;
  variables.max_mem_used= global_system_variables.max_mem_used;
  main_da.init();

  mdl_context.init(this);
  mdl_backup_lock= 0;

  /*
    Pass nominal parameters to init_alloc_root only to ensure that
    the destructor works OK in case of an error. The main_mem_root
    will be re-initialized in init_for_queries().
  */
  init_sql_alloc(key_memory_thd_main_mem_root,
                 &main_mem_root, 64, 0, MYF(MY_THREAD_SPECIFIC));

  /*
    Allocation of user variables for binary logging is always done with main
    mem root
  */
  user_var_events_alloc= mem_root;

  stmt_arena= this;
  thread_stack= 0;
  scheduler= thread_scheduler;                 // Will be fixed later
  event_scheduler.data= 0;
  skip_wait_timeout= false;
  catalog= (char*)"std"; // the only catalog we have for now
  main_security_ctx.init();
  security_ctx= &main_security_ctx;
  no_errors= 0;
  password= 0;
  query_start_sec_part_used= 0;
  count_cuted_fields= CHECK_FIELD_IGNORE;
  killed= NOT_KILLED;
  killed_err= 0;
  is_slave_error= thread_specific_used= FALSE;
  my_hash_clear(&handler_tables_hash);
  my_hash_clear(&ull_hash);
  tmp_table=0;
  cuted_fields= 0L;
  m_sent_row_count= 0L;
  limit_found_rows= 0;
  m_row_count_func= -1;
  statement_id_counter= 0UL;
  // Must be reset to handle error with THD's created for init of mysqld
  lex->current_select= 0;
  start_utime= utime_after_query= 0;
  system_time.start.val= system_time.sec= system_time.sec_part= 0;
  utime_after_lock= 0L;
  progress.arena= 0;
  progress.report_to_client= 0;
  progress.max_counter= 0;
  slave_thread = 0;
  connection_name.str= 0;
  connection_name.length= 0;

  file_id = 0;
  query_id= 0;
  query_name_consts= 0;
  semisync_info= 0;
  db_charset= global_system_variables.collation_database;
  bzero((void*) ha_data, sizeof(ha_data));
  mysys_var=0;
  binlog_evt_union.do_union= FALSE;
  binlog_table_maps= FALSE;
  binlog_xid= 0;
  enable_slow_log= 0;
  durability_property= HA_REGULAR_DURABILITY;

#ifdef DBUG_ASSERT_EXISTS
  dbug_sentry=THD_SENTRY_MAGIC;
#endif
  mysql_audit_init_thd(this);
  net.vio=0;
  net.buff= 0;
  net.reading_or_writing= 0;
  client_capabilities= 0;                       // minimalistic client
  system_thread= NON_SYSTEM_THREAD;
  cleanup_done= free_connection_done= abort_on_warning= got_warning= 0;
  peer_port= 0;					// For SHOW PROCESSLIST
  transaction= &default_transaction;
  transaction->m_pending_rows_event= 0;
  transaction->on= 1;
  wt_thd_lazy_init(&transaction->wt,
                   &variables.wt_deadlock_search_depth_short,
                   &variables.wt_timeout_short,
                   &variables.wt_deadlock_search_depth_long,
                   &variables.wt_timeout_long);
#ifdef SIGNAL_WITH_VIO_CLOSE
  active_vio = 0;
#endif
  mysql_mutex_init(key_LOCK_thd_data, &LOCK_thd_data, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wakeup_ready, &LOCK_wakeup_ready, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_thd_kill, &LOCK_thd_kill, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wakeup_ready, &COND_wakeup_ready, 0);
  mysql_mutex_record_order(&LOCK_thd_kill, &LOCK_thd_data);

  /* Variables with default values */
  proc_info="login";
  where= THD::DEFAULT_WHERE;
  slave_net = 0;
  m_command=COM_CONNECT;
  *scramble= '\0';

#ifdef WITH_WSREP
  mysql_cond_init(key_COND_wsrep_thd, &COND_wsrep_thd, NULL);
  wsrep_info[sizeof(wsrep_info) - 1] = '\0'; /* make sure it is 0-terminated */
#endif
  /* Call to init() below requires fully initialized Open_tables_state. */
  reset_open_tables_state();

  init();
  debug_sync_init_thread(this);
#if defined(ENABLED_PROFILING)
  profiling.set_thd(this);
#endif
  user_connect=(USER_CONN *)0;
  my_hash_init(key_memory_user_var_entry, &user_vars, system_charset_info,
               USER_VARS_HASH_SIZE, 0, 0, (my_hash_get_key) get_var_key,
               (my_hash_free_key) free_user_var, HASH_THREAD_SPECIFIC);
  my_hash_init(PSI_INSTRUMENT_ME, &sequences, system_charset_info,
               SEQUENCES_HASH_SIZE, 0, 0, (my_hash_get_key)
               get_sequence_last_key, (my_hash_free_key) free_sequence_last,
               HASH_THREAD_SPECIFIC);

  /* For user vars replication*/
  if (opt_bin_log)
    my_init_dynamic_array(key_memory_user_var_entry, &user_var_events,
			  sizeof(BINLOG_USER_VAR_EVENT *), 16, 16, MYF(0));
  else
    bzero((char*) &user_var_events, sizeof(user_var_events));

  /* Protocol */
  protocol= &protocol_text;			// Default protocol
  protocol_text.init(this);
  protocol_binary.init(this);

  thr_timer_init(&query_timer, (void (*)(void*)) thd_kill_timeout, this);

  tablespace_op=FALSE;

  /*
    Initialize the random generator. We call my_rnd() without a lock as
    it's not really critical if two threads modifies the structure at the
    same time.  We ensure that we have an unique number foreach thread
    by adding the address of the stack.
  */
  tmp= (ulong) (my_rnd(&sql_rand) * 0xffffffff);
  my_rnd_init(&rand, tmp + (ulong)((size_t) &rand), tmp + (ulong) ::global_query_id);
  substitute_null_with_insert_id = FALSE;
  lock_info.mysql_thd= (void *)this;

  m_token_array= NULL;
  if (max_digest_length > 0)
  {
    m_token_array= (unsigned char*) my_malloc(PSI_INSTRUMENT_ME,
                                              max_digest_length,
                                              MYF(MY_WME|MY_THREAD_SPECIFIC));
  }

  m_binlog_invoker= INVOKER_NONE;
  invoker.init();
  prepare_derived_at_open= FALSE;
  create_tmp_table_for_derived= FALSE;
  save_prep_leaf_list= FALSE;
  org_charset= 0;
  /* Restore THR_THD */
  set_current_thd(old_THR_THD);
}


void THD::push_internal_handler(Internal_error_handler *handler)
{
  DBUG_ENTER("THD::push_internal_handler");
  if (m_internal_handler)
  {
    handler->m_prev_internal_handler= m_internal_handler;
    m_internal_handler= handler;
  }
  else
  {
    m_internal_handler= handler;
  }
  DBUG_VOID_RETURN;
}

bool THD::handle_condition(uint sql_errno,
                           const char* sqlstate,
                           Sql_condition::enum_warning_level *level,
                           const char* msg,
                           Sql_condition ** cond_hdl)
{
  if (!m_internal_handler)
  {
    *cond_hdl= NULL;
    return FALSE;
  }

  for (Internal_error_handler *error_handler= m_internal_handler;
       error_handler;
       error_handler= error_handler->m_prev_internal_handler)
  {
    if (error_handler->handle_condition(this, sql_errno, sqlstate, level, msg,
					cond_hdl))
    {
      return TRUE;
    }
  }
  return FALSE;
}


Internal_error_handler *THD::pop_internal_handler()
{
  DBUG_ENTER("THD::pop_internal_handler");
  DBUG_ASSERT(m_internal_handler != NULL);
  Internal_error_handler *popped_handler= m_internal_handler;
  m_internal_handler= m_internal_handler->m_prev_internal_handler;
  DBUG_RETURN(popped_handler);
}


void THD::raise_error(uint sql_errno)
{
  const char* msg= ER_THD(this, sql_errno);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_ERROR, msg);
}

void THD::raise_error_printf(uint sql_errno, ...)
{
  va_list args;
  char ebuff[MYSQL_ERRMSG_SIZE];
  DBUG_ENTER("THD::raise_error_printf");
  DBUG_PRINT("my", ("nr: %d  errno: %d", sql_errno, errno));
  const char* format= ER_THD(this, sql_errno);
  va_start(args, sql_errno);
  my_vsnprintf(ebuff, sizeof(ebuff), format, args);
  va_end(args);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_ERROR, ebuff);
  DBUG_VOID_RETURN;
}

void THD::raise_warning(uint sql_errno)
{
  const char* msg= ER_THD(this, sql_errno);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_WARN, msg);
}

void THD::raise_warning_printf(uint sql_errno, ...)
{
  va_list args;
  char    ebuff[MYSQL_ERRMSG_SIZE];
  DBUG_ENTER("THD::raise_warning_printf");
  DBUG_PRINT("enter", ("warning: %u", sql_errno));
  const char* format= ER_THD(this, sql_errno);
  va_start(args, sql_errno);
  my_vsnprintf(ebuff, sizeof(ebuff), format, args);
  va_end(args);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_WARN, ebuff);
  DBUG_VOID_RETURN;
}

void THD::raise_note(uint sql_errno)
{
  DBUG_ENTER("THD::raise_note");
  DBUG_PRINT("enter", ("code: %d", sql_errno));
  if (!(variables.option_bits & OPTION_SQL_NOTES))
    DBUG_VOID_RETURN;
  const char* msg= ER_THD(this, sql_errno);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_NOTE, msg);
  DBUG_VOID_RETURN;
}

void THD::raise_note_printf(uint sql_errno, ...)
{
  va_list args;
  char    ebuff[MYSQL_ERRMSG_SIZE];
  DBUG_ENTER("THD::raise_note_printf");
  DBUG_PRINT("enter",("code: %u", sql_errno));
  if (!(variables.option_bits & OPTION_SQL_NOTES))
    DBUG_VOID_RETURN;
  const char* format= ER_THD(this, sql_errno);
  va_start(args, sql_errno);
  my_vsnprintf(ebuff, sizeof(ebuff), format, args);
  va_end(args);
  (void) raise_condition(sql_errno, "\0\0\0\0\0",
                         Sql_condition::WARN_LEVEL_NOTE, ebuff);
  DBUG_VOID_RETURN;
}

Sql_condition* THD::raise_condition(const Sql_condition *cond)
{
  uint sql_errno= cond->get_sql_errno();
  const char *sqlstate= cond->get_sqlstate();
  Sql_condition::enum_warning_level level= cond->get_level();
  const char *msg= cond->get_message_text();

  Diagnostics_area *da= get_stmt_da();
  Sql_condition *raised= NULL;
  DBUG_ENTER("THD::raise_condition");
  DBUG_ASSERT(level < Sql_condition::WARN_LEVEL_END);

  if (!(variables.option_bits & OPTION_SQL_NOTES) &&
      (level == Sql_condition::WARN_LEVEL_NOTE))
    DBUG_RETURN(NULL);
#ifdef WITH_WSREP
  /*
    Suppress warnings/errors if the wsrep THD is going to replay. The
    deadlock/interrupted errors may be transitient and should not be
    reported to the client.
  */
  if (wsrep_must_replay(this))
    DBUG_RETURN(NULL);
#endif /* WITH_WSREP */

  da->opt_clear_warning_info(query_id);

  /*
    TODO: replace by DBUG_ASSERT(sql_errno != 0) once all bugs similar to
    Bug#36768 are fixed: a SQL condition must have a real (!=0) error number
    so that it can be caught by handlers.
  */
  if (sql_errno == 0)
    sql_errno= ER_UNKNOWN_ERROR;
  if (msg == NULL)
    msg= ER_THD(this, sql_errno);
  if (!*sqlstate)
   sqlstate= mysql_errno_to_sqlstate(sql_errno);

  if ((level == Sql_condition::WARN_LEVEL_WARN) && really_abort_on_warning())
  {
    /* FIXME: push_warning and strict SQL_MODE case. */
    level= Sql_condition::WARN_LEVEL_ERROR;
  }

  if (!is_fatal_error &&
      handle_condition(sql_errno, sqlstate, &level, msg, &raised))
    goto ret;

  switch (level) {
  case Sql_condition::WARN_LEVEL_NOTE:
  case Sql_condition::WARN_LEVEL_WARN:
    got_warning= 1;
    break;
  case Sql_condition::WARN_LEVEL_ERROR:
    break;
  case Sql_condition::WARN_LEVEL_END:
    /* Impossible */
    break;
  }

  if (level == Sql_condition::WARN_LEVEL_ERROR)
  {
    mysql_audit_general(this, MYSQL_AUDIT_GENERAL_ERROR, sql_errno, msg);

    is_slave_error=  1; // needed to catch query errors during replication

#ifdef WITH_WSREP
    /*
      With wsrep we allow converting BF abort error to warning if
      errors are ignored.
     */
    if (!is_fatal_error && no_errors &&
        (wsrep_trx().bf_aborted() || wsrep_retry_counter))
    {
      WSREP_DEBUG("BF abort error converted to warning");
    }
    else
#endif /* WITH_WSREP */
    {
      if (!da->is_error())
      {
	set_row_count_func(-1);
	da->set_error_status(sql_errno, msg, sqlstate, *cond, raised);
      }
    }
  }

  query_cache_abort(this, &query_cache_tls);

  /* 
     Avoid pushing a condition for fatal out of memory errors as this will 
     require memory allocation and therefore might fail. Non fatal out of 
     memory errors can occur if raised by SIGNAL/RESIGNAL statement.
  */
  if (likely(!(is_fatal_error && (sql_errno == EE_OUTOFMEMORY ||
                                  sql_errno == ER_OUTOFMEMORY))))
  {
    raised= da->push_warning(this, sql_errno, sqlstate, level, *cond, msg,
                             cond->m_row_number);
  }
ret:
  if (raised)
    raised->copy_opt_attributes(cond);
  DBUG_RETURN(raised);
}

extern "C"
void *thd_alloc(MYSQL_THD thd, size_t size)
{
  return thd->alloc(size);
}

extern "C"
void *thd_calloc(MYSQL_THD thd, size_t size)
{
  return thd->calloc(size);
}

extern "C"
char *thd_strdup(MYSQL_THD thd, const char *str)
{
  return thd->strdup(str);
}

extern "C"
char *thd_strmake(MYSQL_THD thd, const char *str, size_t size)
{
  return thd->strmake(str, size);
}

extern "C"
LEX_CSTRING *thd_make_lex_string(THD *thd, LEX_CSTRING *lex_str,
                                const char *str, size_t size,
                                int allocate_lex_string)
{
  return allocate_lex_string ? thd->make_clex_string(str, size)
                             : thd->make_lex_string(lex_str, str, size);
}

extern "C"
void *thd_memdup(MYSQL_THD thd, const void* str, size_t size)
{
  return thd->memdup(str, size);
}

extern "C"
void thd_get_xid(const MYSQL_THD thd, MYSQL_XID *xid)
{
  *xid = *(MYSQL_XID *) thd->get_xid();
}

extern "C"
my_time_t thd_TIME_to_gmt_sec(MYSQL_THD thd, const MYSQL_TIME *ltime,
                              unsigned int *errcode)
{
  Time_zone *tz= thd ? thd->variables.time_zone :
                       global_system_variables.time_zone;
  return tz->TIME_to_gmt_sec(ltime, errcode);
}


extern "C"
void thd_gmt_sec_to_TIME(MYSQL_THD thd, MYSQL_TIME *ltime, my_time_t t)
{
  Time_zone *tz= thd ? thd->variables.time_zone :
                       global_system_variables.time_zone;
  tz->gmt_sec_to_TIME(ltime, t);
}


#ifdef _WIN32
extern "C" my_thread_id next_thread_id_noinline()
{
#undef next_thread_id
  return next_thread_id();
}
#endif


const Type_handler *THD::type_handler_for_datetime() const
{
  if (opt_mysql56_temporal_format)
    return &type_handler_datetime2;
  return &type_handler_datetime;
}


/*
  Init common variables that has to be reset on start and on change_user
*/

void THD::init()
{
  DBUG_ENTER("thd::init");
  mysql_mutex_lock(&LOCK_global_system_variables);
  plugin_thdvar_init(this);
  /*
    plugin_thd_var_init() sets variables= global_system_variables, which
    has reset variables.pseudo_thread_id to 0. We need to correct it here to
    avoid temporary tables replication failure.
  */
  variables.pseudo_thread_id= thread_id;

  variables.default_master_connection.str= default_master_connection_buff;
  ::strmake(default_master_connection_buff,
            global_system_variables.default_master_connection.str,
            variables.default_master_connection.length);
  mysql_mutex_unlock(&LOCK_global_system_variables);

  user_time.val= start_time= start_time_sec_part= 0;

  server_status= SERVER_STATUS_AUTOCOMMIT;
  if (variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)
    server_status|= SERVER_STATUS_NO_BACKSLASH_ESCAPES;
  if (variables.sql_mode & MODE_ANSI_QUOTES)
    server_status|= SERVER_STATUS_ANSI_QUOTES;

  transaction->all.modified_non_trans_table=
    transaction->stmt.modified_non_trans_table= FALSE;
  transaction->all.m_unsafe_rollback_flags=
    transaction->stmt.m_unsafe_rollback_flags= 0;

  open_options=ha_open_options;
  update_lock_default= (variables.low_priority_updates ?
			TL_WRITE_LOW_PRIORITY :
			TL_WRITE);
  tx_isolation= (enum_tx_isolation) variables.tx_isolation;
  tx_read_only= variables.tx_read_only;
  update_charset();             // plugin_thd_var() changed character sets
  reset_current_stmt_binlog_format_row();
  reset_binlog_local_stmt_filter();
  set_status_var_init();
  status_var.max_local_memory_used= status_var.local_memory_used;
  bzero((char *) &org_status_var, sizeof(org_status_var));
  status_in_global= 0;
  start_bytes_received= 0;
  m_last_commit_gtid.seq_no= 0;
  last_stmt= NULL;
  /* Reset status of last insert id */
  arg_of_last_insert_id_function= FALSE;
  stmt_depends_on_first_successful_insert_id_in_prev_stmt= FALSE;
  first_successful_insert_id_in_prev_stmt= 0;
  first_successful_insert_id_in_prev_stmt_for_binlog= 0;
  first_successful_insert_id_in_cur_stmt= 0;
  current_backup_stage= BACKUP_FINISHED;
  backup_commit_lock= 0;
#ifdef WITH_WSREP
  wsrep_last_query_id= 0;
  wsrep_xid.null();
  wsrep_skip_locking= FALSE;
  wsrep_converted_lock_session= false;
  wsrep_retry_counter= 0;
  wsrep_rgi= NULL;
  wsrep_PA_safe= true;
  wsrep_consistency_check = NO_CONSISTENCY_CHECK;
  wsrep_mysql_replicated  = 0;
  wsrep_TOI_pre_query     = NULL;
  wsrep_TOI_pre_query_len = 0;
  wsrep_rbr_buf           = NULL;
  wsrep_affected_rows     = 0;
  m_wsrep_next_trx_id     = WSREP_UNDEFINED_TRX_ID;
  wsrep_aborter           = 0;
  wsrep_desynced_backup_stage= false;
#endif /* WITH_WSREP */

  if (variables.sql_log_bin)
    variables.option_bits|= OPTION_BIN_LOG;
  else
    variables.option_bits&= ~OPTION_BIN_LOG;

  select_commands= update_commands= other_commands= 0;
  /* Set to handle counting of aborted connections */
  userstat_running= opt_userstat_running;
  last_global_update_time= current_connect_time= time(NULL);
#ifndef EMBEDDED_LIBRARY
  session_tracker.enable(this);
#endif //EMBEDDED_LIBRARY

  apc_target.init(&LOCK_thd_kill);
  gap_tracker_data.init();
  DBUG_VOID_RETURN;
}


bool THD::restore_from_local_lex_to_old_lex(LEX *oldlex)
{
  DBUG_ASSERT(lex->sphead);
  if (lex->sphead->merge_lex(this, oldlex, lex))
    return true;
  lex= oldlex;
  return false;
}


/* Updates some status variables to be used by update_global_user_stats */

void THD::update_stats(void)
{
  /* sql_command == SQLCOM_END in case of parse errors or quit */
  if (lex->sql_command != SQLCOM_END)
  {
    /* A SQL query. */
    if (lex->sql_command == SQLCOM_SELECT)
      select_commands++;
    else if (sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND)
    {
      /* Ignore 'SHOW ' commands */
    }
    else if (is_update_query(lex->sql_command))
      update_commands++;
    else
      other_commands++;
  }
}


void THD::update_all_stats()
{
  ulonglong end_cpu_time, end_utime;
  double busy_time, cpu_time;

  /* This is set at start of query if opt_userstat_running was set */
  if (!userstat_running)
    return;

  end_cpu_time= my_getcputime();
  end_utime=    microsecond_interval_timer();
  busy_time= (end_utime - start_utime) / 1000000.0;
  cpu_time=  (end_cpu_time - start_cpu_time) / 10000000.0;
  /* In case there are bad values, 2629743 is the #seconds in a month. */
  if (cpu_time > 2629743.0)
    cpu_time= 0;
  status_var_add(status_var.cpu_time, cpu_time);
  status_var_add(status_var.busy_time, busy_time);

  update_global_user_stats(this, TRUE, my_time(0));
  // Has to be updated after update_global_user_stats()
  userstat_running= 0;
}


/*
  Init THD for query processing.
  This has to be called once before we call mysql_parse.
  See also comments in sql_class.h.
*/

void THD::init_for_queries()
{
  DBUG_ASSERT(transaction->on);
  DBUG_ASSERT(m_transaction_psi == NULL);

  /* Set time for --init-file queries */
  set_time();
  reset_root_defaults(mem_root, variables.query_alloc_block_size,
                      variables.query_prealloc_size);
  reset_root_defaults(&transaction->mem_root,
                      variables.trans_alloc_block_size,
                      variables.trans_prealloc_size);
  DBUG_ASSERT(!transaction->xid_state.is_explicit_XA());
  DBUG_ASSERT(transaction->implicit_xid.is_null());
}


/*
  Do what's needed when one invokes change user

  SYNOPSIS
    change_user()

  IMPLEMENTATION
    Reset all resources that are connection specific
*/


void THD::change_user(void)
{
  if (!status_in_global)                        // Reset in init()
    add_status_to_global();

  if (!cleanup_done)
    cleanup();
  cleanup_done= 0;
  reset_killed();
  /* Clear errors from the previous THD */
  my_errno= 0;
  if (mysys_var)
    mysys_var->abort= 0;

  /* Clear warnings. */
  if (!get_stmt_da()->is_warning_info_empty())
    get_stmt_da()->clear_warning_info(0);

  init();
  stmt_map.reset();
  my_hash_init(key_memory_user_var_entry, &user_vars, system_charset_info,
               USER_VARS_HASH_SIZE, 0, 0, (my_hash_get_key) get_var_key,
               (my_hash_free_key) free_user_var, HASH_THREAD_SPECIFIC);
  my_hash_init(key_memory_user_var_entry, &sequences, system_charset_info,
               SEQUENCES_HASH_SIZE, 0, 0, (my_hash_get_key)
               get_sequence_last_key, (my_hash_free_key) free_sequence_last,
               HASH_THREAD_SPECIFIC);
  sp_caches_clear();
  opt_trace.delete_traces();
}

/**
   Change default database

   @note This is coded to have as few instructions as possible under
   LOCK_thd_data
*/

bool THD::set_db(const LEX_CSTRING *new_db)
{
  bool result= 0;
  /*
    Acquiring mutex LOCK_thd_data as we either free the memory allocated
    for the database and reallocating the memory for the new db or memcpy
    the new_db to the db.
  */
  /* Do not reallocate memory if current chunk is big enough. */
  if (db.str && new_db->str && db.length >= new_db->length)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    db.length= new_db->length;
    memcpy((char*) db.str, new_db->str, new_db->length+1);
    mysql_mutex_unlock(&LOCK_thd_data);
  }
  else
  {
    const char *org_db= db.str;
    const char *tmp= NULL;
    if (new_db->str)
    {
      if (!(tmp= my_strndup(key_memory_THD_db, new_db->str, new_db->length,
                            MYF(MY_WME | ME_FATAL))))
        result= 1;
    }

    mysql_mutex_lock(&LOCK_thd_data);
    db.str= tmp;
    db.length= tmp ? new_db->length : 0;
    mysql_mutex_unlock(&LOCK_thd_data);
    my_free((char*) org_db);
  }
  PSI_CALL_set_thread_db(db.str, (int) db.length);
  return result;
}


/**
   Set the current database

   @param new_db     a pointer to the new database name.
   @param new_db_len length of the new database name.

   @note This operation just sets {db, db_length}. Switching the current
   database usually involves other actions, like switching other database
   attributes including security context. In the future, this operation
   will be made private and more convenient interface will be provided.
*/

void THD::reset_db(const LEX_CSTRING *new_db)
{
  if (new_db->str != db.str || new_db->length != db.length)
  {
    if (db.str != 0)
      DBUG_PRINT("QQ", ("Overwriting: %p", db.str));
    mysql_mutex_lock(&LOCK_thd_data);
    db= *new_db;
    mysql_mutex_unlock(&LOCK_thd_data);
    PSI_CALL_set_thread_db(db.str, (int) db.length);
  }
}


/* Do operations that may take a long time */

void THD::cleanup(void)
{
  DBUG_ENTER("THD::cleanup");
  DBUG_ASSERT(cleanup_done == 0);

  set_killed(KILL_CONNECTION);
#ifdef WITH_WSREP
  if (wsrep_cs().state() != wsrep::client_state::s_none)
    wsrep_cs().cleanup();
  wsrep_client_thread= false;
#endif /* WITH_WSREP */

  mysql_ha_cleanup(this);
  locked_tables_list.unlock_locked_tables(this);

  delete_dynamic(&user_var_events);
  close_temporary_tables();

  if (transaction->xid_state.is_explicit_XA())
    trans_xa_detach(this);
  else
    trans_rollback(this);

  DBUG_ASSERT(open_tables == NULL);
  DBUG_ASSERT(m_transaction_psi == NULL);

  /*
    If the thread was in the middle of an ongoing transaction (rolled
    back a few lines above) or under LOCK TABLES (unlocked the tables
    and left the mode a few lines above), there will be outstanding
    metadata locks. Release them.
  */
  mdl_context.release_transactional_locks(this);

  backup_end(this);
  backup_unlock(this);

  /* Release the global read lock, if acquired. */
  if (global_read_lock.is_acquired())
    global_read_lock.unlock_global_read_lock(this);

  if (user_connect)
  {
    decrease_user_connections(user_connect);
    user_connect= 0;                            // Safety
  }
  wt_thd_destroy(&transaction->wt);

  my_hash_free(&user_vars);
  my_hash_free(&sequences);
  sp_caches_clear();
  auto_inc_intervals_forced.empty();
  auto_inc_intervals_in_cur_stmt_for_binlog.empty();

  mysql_ull_cleanup(this);
  stmt_map.reset();
  /* All metadata locks must have been released by now. */
  DBUG_ASSERT(!mdl_context.has_locks());

  apc_target.destroy();
#ifdef HAVE_REPLICATION
  unregister_slave();
#endif
  cleanup_done=1;
  DBUG_VOID_RETURN;
}


/*
  Free all connection related resources associated with a THD.
  This is used when we put a thread into the thread cache.
  After this call should either call ~THD or reset_for_reuse() depending on
  circumstances.
*/

void THD::free_connection()
{
  DBUG_ASSERT(free_connection_done == 0);
  my_free(const_cast<char*>(db.str));
  db= null_clex_str;
#ifndef EMBEDDED_LIBRARY
  if (net.vio)
    vio_delete(net.vio);
  net.vio= nullptr;
  net_end(&net);
#endif
 if (!cleanup_done)
   cleanup();
  ha_close_connection(this);
  plugin_thdvar_cleanup(this);
  mysql_audit_free_thd(this);
  main_security_ctx.destroy();
  /* close all prepared statements, to save memory */
  stmt_map.reset();
  free_connection_done= 1;
#if defined(ENABLED_PROFILING)
  profiling.restart();                          // Reset profiling
#endif
  debug_sync_reset_thread(this);
}

/*
  Reset thd for reuse by another connection
  This is only used for user connections, so the following variables doesn't
  have to be reset:
  - Replication (slave) variables.
  - Variables not reset between each statements. See reset_for_next_command.
*/

void THD::reset_for_reuse()
{
  mysql_audit_init_thd(this);
  change_user();                                // Calls cleanup() & init()
  get_stmt_da()->reset_diagnostics_area();
  main_security_ctx.init();  
  failed_com_change_user= 0;
  is_fatal_error= 0;
  client_capabilities= 0;
  peer_port= 0;
  query_name_consts= 0;                         // Safety
  abort_on_warning= 0;
  free_connection_done= 0;
  m_command= COM_CONNECT;
  transaction->on= 1;
#if defined(ENABLED_PROFILING)
  profiling.reset();
#endif
#ifdef SIGNAL_WITH_VIO_CLOSE
  active_vio = 0;
#endif
#ifdef WITH_WSREP
  wsrep_free_status(this);
#endif /* WITH_WSREP */
}


THD::~THD()
{
  THD *orig_thd= current_thd;
  THD_CHECK_SENTRY(this);
  DBUG_ENTER("~THD()");
  /* Make sure threads are not available via server_threads.  */
  assert_not_linked();
  if (m_psi)
    PSI_CALL_set_thread_THD(m_psi, 0);

  /*
    In error cases, thd may not be current thd. We have to fix this so
    that memory allocation counting is done correctly
  */
  set_current_thd(this);
  if (!status_in_global)
    add_status_to_global();

  /*
    Other threads may have a lock on LOCK_thd_kill to ensure that this
    THD is not deleted while they access it. The following mutex_lock
    ensures that no one else is using this THD and it's now safe to delete
  */
  mysql_mutex_lock(&LOCK_thd_kill);
  mysql_mutex_unlock(&LOCK_thd_kill);

#ifdef WITH_WSREP
  delete wsrep_rgi;
#endif
  if (!free_connection_done)
    free_connection();

#ifdef WITH_WSREP
  mysql_cond_destroy(&COND_wsrep_thd);
#endif
  mdl_context.destroy();

  transaction->free();
  mysql_cond_destroy(&COND_wakeup_ready);
  mysql_mutex_destroy(&LOCK_wakeup_ready);
  mysql_mutex_destroy(&LOCK_thd_data);
  mysql_mutex_destroy(&LOCK_thd_kill);
#ifdef DBUG_ASSERT_EXISTS
  dbug_sentry= THD_SENTRY_GONE;
#endif  
#ifndef EMBEDDED_LIBRARY
  if (rgi_fake)
  {
    delete rgi_fake;
    rgi_fake= NULL;
  }
  if (rli_fake)
  {
    delete rli_fake;
    rli_fake= NULL;
  }
  
  if (rgi_slave)
    rgi_slave->cleanup_after_session();
  my_free(semisync_info);
#endif
  main_lex.free_set_stmt_mem_root();
  free_root(&main_mem_root, MYF(0));
  my_free(m_token_array);
  main_da.free_memory();
  if (tdc_hash_pins)
    lf_hash_put_pins(tdc_hash_pins);
  if (xid_hash_pins)
    lf_hash_put_pins(xid_hash_pins);
  debug_sync_end_thread(this);
  /* Ensure everything is freed */
  status_var.local_memory_used-= sizeof(THD);

  /* trick to make happy memory accounting system */
#ifndef EMBEDDED_LIBRARY
  session_tracker.sysvars.deinit();
#ifdef USER_VAR_TRACKING
  session_tracker.user_variables.deinit();
#endif // USER_VAR_TRACKING
#endif //EMBEDDED_LIBRARY

  if (status_var.local_memory_used != 0)
  {
    DBUG_PRINT("error", ("memory_used: %lld", status_var.local_memory_used));
    SAFEMALLOC_REPORT_MEMORY(thread_id);
    DBUG_ASSERT(status_var.local_memory_used == 0 ||
                !debug_assert_on_not_freed_memory);
  }
  update_global_memory_status(status_var.global_memory_used);
  set_current_thd(orig_thd == this ? 0 : orig_thd);
  DBUG_VOID_RETURN;
}


/*
  Add all status variables to another status variable array

  SYNOPSIS
   add_to_status()
   to_var       add to this array
   from_var     from this array

  NOTES
    This function assumes that all variables at start are long/ulong and
    other types are handled explicitly
*/

void add_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var)
{
  ulong *end= (ulong*) ((uchar*) to_var +
                        offsetof(STATUS_VAR, last_system_status_var) +
			sizeof(ulong));
  ulong *to= (ulong*) to_var, *from= (ulong*) from_var;

  while (to != end)
    *(to++)+= *(from++);

  /* Handle the not ulong variables. See end of system_status_var */
  to_var->bytes_received+=      from_var->bytes_received;
  to_var->bytes_sent+=          from_var->bytes_sent;
  to_var->rows_read+=           from_var->rows_read;
  to_var->rows_sent+=           from_var->rows_sent;
  to_var->rows_tmp_read+=       from_var->rows_tmp_read;
  to_var->binlog_bytes_written+= from_var->binlog_bytes_written;
  to_var->cpu_time+=            from_var->cpu_time;
  to_var->busy_time+=           from_var->busy_time;
  to_var->table_open_cache_hits+= from_var->table_open_cache_hits;
  to_var->table_open_cache_misses+= from_var->table_open_cache_misses;
  to_var->table_open_cache_overflows+= from_var->table_open_cache_overflows;

  /*
    Update global_memory_used. We have to do this with atomic_add as the
    global value can change outside of LOCK_status.
  */
  if (to_var == &global_status_var)
  {
    DBUG_PRINT("info", ("global memory_used: %lld  size: %lld",
                        (longlong) global_status_var.global_memory_used,
                        (longlong) from_var->global_memory_used));
    update_global_memory_status(from_var->global_memory_used);
  }
  else
   to_var->global_memory_used+= from_var->global_memory_used;
}

/*
  Add the difference between two status variable arrays to another one.

  SYNOPSIS
    add_diff_to_status
    to_var       add to this array
    from_var     from this array
    dec_var      minus this array
  
  NOTE
    This function assumes that all variables at start are long/ulong and
    other types are handled explicitly
*/

void add_diff_to_status(STATUS_VAR *to_var, STATUS_VAR *from_var,
                        STATUS_VAR *dec_var)
{
  ulong *end= (ulong*) ((uchar*) to_var + offsetof(STATUS_VAR,
						  last_system_status_var) +
			sizeof(ulong));
  ulong *to= (ulong*) to_var, *from= (ulong*) from_var, *dec= (ulong*) dec_var;

  while (to != end)
    *(to++)+= *(from++) - *(dec++);

  to_var->bytes_received+=       from_var->bytes_received -
                                 dec_var->bytes_received;
  to_var->bytes_sent+=           from_var->bytes_sent - dec_var->bytes_sent;
  to_var->rows_read+=            from_var->rows_read - dec_var->rows_read;
  to_var->rows_sent+=            from_var->rows_sent - dec_var->rows_sent;
  to_var->rows_tmp_read+=        from_var->rows_tmp_read - dec_var->rows_tmp_read;
  to_var->binlog_bytes_written+= from_var->binlog_bytes_written -
                                 dec_var->binlog_bytes_written;
  to_var->cpu_time+=             from_var->cpu_time - dec_var->cpu_time;
  to_var->busy_time+=            from_var->busy_time - dec_var->busy_time;
  to_var->table_open_cache_hits+= from_var->table_open_cache_hits -
                                  dec_var->table_open_cache_hits;
  to_var->table_open_cache_misses+= from_var->table_open_cache_misses -
                                    dec_var->table_open_cache_misses;
  to_var->table_open_cache_overflows+= from_var->table_open_cache_overflows -
                                       dec_var->table_open_cache_overflows;

  /*
    We don't need to accumulate memory_used as these are not reset or used by
    the calling functions.  See execute_show_status().
  */
}

#define SECONDS_TO_WAIT_FOR_KILL 2
#if !defined(_WIN32) && defined(HAVE_SELECT)
/* my_sleep() can wait for sub second times */
#define WAIT_FOR_KILL_TRY_TIMES 20
#else
#define WAIT_FOR_KILL_TRY_TIMES 2
#endif


/**
  Awake a thread.

  @param[in]  state_to_set    value for THD::killed

  This is normally called from another thread's THD object.

  @note Do always call this while holding LOCK_thd_kill.
        NOT_KILLED is used to awake a thread for a slave
*/
extern std::atomic<my_thread_id> shutdown_thread_id;
void THD::awake_no_mutex(killed_state state_to_set)
{
  DBUG_ENTER("THD::awake");
  DBUG_PRINT("enter", ("this: %p current_thd: %p  state: %d",
                       this, current_thd, (int) state_to_set));
  THD_CHECK_SENTRY(this);
  mysql_mutex_assert_owner(&LOCK_thd_data);
  mysql_mutex_assert_owner(&LOCK_thd_kill);

  print_aborted_warning(3, "KILLED");

  /*
    Don't degrade killed state, for example from a KILL_CONNECTION to
    STATEMENT TIMEOUT
  */
  if (killed >= KILL_CONNECTION)
    state_to_set= killed;

  set_killed_no_mutex(state_to_set);

  if (state_to_set >= KILL_CONNECTION || state_to_set == NOT_KILLED)
  {
#ifdef SIGNAL_WITH_VIO_CLOSE
    if (this != current_thd && thread_id != shutdown_thread_id)
    {
      if(active_vio)
        vio_shutdown(active_vio, SHUT_RDWR);
    }
#endif

    /* Mark the target thread's alarm request expired, and signal alarm. */
    thr_alarm_kill(thread_id);

    /* Send an event to the scheduler that a thread should be killed. */
    if (!slave_thread)
      MYSQL_CALLBACK(scheduler, post_kill_notification, (this));
  }

  /* Interrupt target waiting inside a storage engine. */
  if (state_to_set != NOT_KILLED  && !wsrep_is_bf_aborted(this))
    ha_kill_query(this, thd_kill_level(this));

  abort_current_cond_wait(false);
  DBUG_VOID_RETURN;
}

/* Broadcast a condition to kick the target if it is waiting on it. */
void THD::abort_current_cond_wait(bool force)
{
  mysql_mutex_assert_owner(&LOCK_thd_kill);
  if (mysys_var)
  {
    mysql_mutex_lock(&mysys_var->mutex);
    if (!system_thread || force)                 // Don't abort locks
      mysys_var->abort=1;

    /*
      This broadcast could be up in the air if the victim thread
      exits the cond in the time between read and broadcast, but that is
      ok since all we want to do is to make the victim thread get out
      of waiting on current_cond.
      If we see a non-zero current_cond: it cannot be an old value (because
      then exit_cond() should have run and it can't because we have mutex); so
      it is the true value but maybe current_mutex is not yet non-zero (we're
      in the middle of enter_cond() and there is a "memory order
      inversion"). So we test the mutex too to not lock 0.

      Note that there is a small chance we fail to kill. If victim has locked
      current_mutex, but hasn't yet entered enter_cond() (which means that
      current_cond and current_mutex are 0), then the victim will not get
      a signal and it may wait "forever" on the cond (until
      we issue a second KILL or the status it's waiting for happens).
      It's true that we have set its thd->killed but it may not
      see it immediately and so may have time to reach the cond_wait().

      However, where possible, we test for killed once again after
      enter_cond(). This should make the signaling as safe as possible.
      However, there is still a small chance of failure on platforms with
      instruction or memory write reordering.

      We have to do the loop with trylock, because if we would use
      pthread_mutex_lock(), we can cause a deadlock as we are here locking
      the mysys_var->mutex and mysys_var->current_mutex in a different order
      than in the thread we are trying to kill.
      We only sleep for 2 seconds as we don't want to have LOCK_thd_data
      locked too long time.

      There is a small change we may not succeed in aborting a thread that
      is not yet waiting for a mutex, but as this happens only for a
      thread that was doing something else when the kill was issued and
      which should detect the kill flag before it starts to wait, this
      should be good enough.
    */
    if (mysys_var->current_cond && mysys_var->current_mutex)
    {
      uint i;
      for (i= 0; i < WAIT_FOR_KILL_TRY_TIMES * SECONDS_TO_WAIT_FOR_KILL; i++)
      {
        int ret= mysql_mutex_trylock(mysys_var->current_mutex);
        mysql_cond_broadcast(mysys_var->current_cond);
        if (!ret)
        {
          /* Signal is sure to get through */
          mysql_mutex_unlock(mysys_var->current_mutex);
          break;
        }
        my_sleep(1000000L / WAIT_FOR_KILL_TRY_TIMES);
      }
    }
    mysql_mutex_unlock(&mysys_var->mutex);
  }
}


/**
  Close the Vio associated this session.

  @remark LOCK_thd_data is taken due to the fact that
          the Vio might be disassociated concurrently.
*/

void THD::disconnect()
{
  Vio *vio= NULL;

  set_killed(KILL_CONNECTION);

  mysql_mutex_lock(&LOCK_thd_data);

#ifdef SIGNAL_WITH_VIO_CLOSE
  /*
    Since a active vio might might have not been set yet, in
    any case save a reference to avoid closing a inexistent
    one or closing the vio twice if there is a active one.
  */
  vio= active_vio;
  close_active_vio();
#endif

  /* Disconnect even if a active vio is not associated. */
  if (net.vio != vio)
    vio_close(net.vio);
  net.thd= 0;                                   // Don't collect statistics

  mysql_mutex_unlock(&LOCK_thd_data);
}


bool THD::notify_shared_lock(MDL_context_owner *ctx_in_use,
                             bool needs_thr_lock_abort)
{
  THD *in_use= ctx_in_use->get_thd();
  bool signalled= FALSE;
  DBUG_ENTER("THD::notify_shared_lock");
  DBUG_PRINT("enter",("needs_thr_lock_abort: %d", needs_thr_lock_abort));

  if ((in_use->system_thread & SYSTEM_THREAD_DELAYED_INSERT) &&
      !in_use->killed)
  {
    /* This code is similar to kill_delayed_threads() */
    DBUG_PRINT("info", ("kill delayed thread"));
    mysql_mutex_lock(&in_use->LOCK_thd_kill);
    if (in_use->killed < KILL_CONNECTION)
      in_use->set_killed_no_mutex(KILL_CONNECTION);
    in_use->abort_current_cond_wait(true);
    mysql_mutex_unlock(&in_use->LOCK_thd_kill);
    signalled= TRUE;
  }

  if (needs_thr_lock_abort)
  {
    mysql_mutex_lock(&in_use->LOCK_thd_data);
    /* If not already dying */
    if (in_use->killed != KILL_CONNECTION_HARD)
    {
      for (TABLE *thd_table= in_use->open_tables;
           thd_table ;
           thd_table= thd_table->next)
      {
        /*
          Check for TABLE::needs_reopen() is needed since in some
          places we call handler::close() for table instance (and set
          TABLE::db_stat to 0) and do not remove such instances from
          the THD::open_tables for some time, during which other
          thread can see those instances (e.g. see partitioning code).
        */
        if (!thd_table->needs_reopen())
        {
          signalled|= mysql_lock_abort_for_thread(this, thd_table);
        }
      }
    }
    mysql_mutex_unlock(&in_use->LOCK_thd_data);
  }
  DBUG_RETURN(signalled);
}


/*
  Get error number for killed state
  Note that the error message can't have any parameters.
  If one needs parameters, one should use THD::killed_err_msg
  See thd::kill_message()
*/

int THD::killed_errno()
{
  DBUG_ENTER("killed_errno");
  DBUG_PRINT("enter", ("killed: %d  killed_errno: %d",
                       killed, killed_err ? killed_err->no: 0));

  /* Ensure that killed_err is not set if we are not killed */
  DBUG_ASSERT(!killed_err || killed != NOT_KILLED);

  if (killed_err)
    DBUG_RETURN(killed_err->no);

  switch (killed) {
  case NOT_KILLED:
  case KILL_HARD_BIT:
    DBUG_RETURN(0);                            // Probably wrong usage
  case KILL_BAD_DATA:
  case KILL_BAD_DATA_HARD:
  case ABORT_QUERY_HARD:
  case ABORT_QUERY:
    DBUG_RETURN(0);                             // Not a real error
  case KILL_CONNECTION:
  case KILL_CONNECTION_HARD:
  case KILL_SYSTEM_THREAD:
  case KILL_SYSTEM_THREAD_HARD:
    DBUG_RETURN(ER_CONNECTION_KILLED);
  case KILL_QUERY:
  case KILL_QUERY_HARD:
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  case KILL_TIMEOUT:
  case KILL_TIMEOUT_HARD:
    DBUG_RETURN(ER_STATEMENT_TIMEOUT);
  case KILL_SERVER:
  case KILL_SERVER_HARD:
    DBUG_RETURN(ER_SERVER_SHUTDOWN);
  case KILL_SLAVE_SAME_ID:
    DBUG_RETURN(ER_SLAVE_SAME_ID);
  case KILL_WAIT_TIMEOUT:
  case KILL_WAIT_TIMEOUT_HARD:
    DBUG_RETURN(ER_NET_READ_INTERRUPTED);
  }
  DBUG_RETURN(0);                               // Keep compiler happy
}


void THD::reset_killed()
{
  /*
    Resetting killed has to be done under a mutex to ensure
    its not done during an awake() call.
  */
  DBUG_ENTER("reset_killed");
  if (killed != NOT_KILLED)
  {
    mysql_mutex_assert_not_owner(&LOCK_thd_kill);
    mysql_mutex_lock(&LOCK_thd_kill);
    killed= NOT_KILLED;
    killed_err= 0;
    mysql_mutex_unlock(&LOCK_thd_kill);
  }
#ifdef WITH_WSREP
  mysql_mutex_assert_not_owner(&LOCK_thd_data);
  mysql_mutex_lock(&LOCK_thd_data);
  wsrep_aborter= 0;
  mysql_mutex_unlock(&LOCK_thd_data);
#endif /* WITH_WSREP */

  DBUG_VOID_RETURN;
}

/*
  Remember the location of thread info, the structure needed for
  the structure for the net buffer
*/

void THD::store_globals()
{
  /*
    Assert that thread_stack is initialized: it's necessary to be able
    to track stack overrun.
  */
  DBUG_ASSERT(thread_stack);

  set_current_thd(this);
  /*
    mysys_var is concurrently readable by a killer thread.
    It is protected by LOCK_thd_kill, it is not needed to lock while the
    pointer is changing from NULL not non-NULL. If the kill thread reads
    NULL it doesn't refer to anything, but if it is non-NULL we need to
    ensure that the thread doesn't proceed to assign another thread to
    have the mysys_var reference (which in fact refers to the worker
    threads local storage with key THR_KEY_mysys. 
  */
  mysys_var=my_thread_var;
  /*
    Let mysqld define the thread id (not mysys)
    This allows us to move THD to different threads if needed.
  */
  mysys_var->id=      thread_id;

  /* thread_dbug_id should not change for a THD */
  if (!thread_dbug_id)
    thread_dbug_id= mysys_var->dbug_id;
  else
  {
    /* This only changes if we are using pool-of-threads */
    mysys_var->dbug_id= thread_dbug_id;
  }
#ifdef __NR_gettid
  os_thread_id= (uint32)syscall(__NR_gettid);
#else
  os_thread_id= 0;
#endif
  real_id= pthread_self();                      // For debugging
  mysys_var->stack_ends_here= thread_stack +    // for consistency, see libevent_thread_proc
                              STACK_DIRECTION * (long)my_thread_stack_size;
  if (net.vio)
  {
    net.thd= this;
  }
  /*
    We have to call thr_lock_info_init() again here as THD may have been
    created in another thread
  */
  thr_lock_info_init(&lock_info, mysys_var);
}

/**
   Untie THD from current thread

   Used when using --thread-handling=pool-of-threads
*/

void THD::reset_globals()
{
  mysql_mutex_lock(&LOCK_thd_kill);
  mysys_var= 0;
  mysql_mutex_unlock(&LOCK_thd_kill);

  /* Undocking the thread specific data. */
  set_current_thd(0);
  net.thd= 0;
}

/*
  Cleanup after query.

  SYNOPSIS
    THD::cleanup_after_query()

  DESCRIPTION
    This function is used to reset thread data to its default state.

  NOTE
    This function is not suitable for setting thread data to some
    non-default values, as there is only one replication thread, so
    different master threads may overwrite data of each other on
    slave.
*/

void THD::cleanup_after_query()
{
  DBUG_ENTER("THD::cleanup_after_query");

  thd_progress_end(this);

  /*
    Reset rand_used so that detection of calls to rand() will save random 
    seeds if needed by the slave.

    Do not reset rand_used if inside a stored function or trigger because 
    only the call to these operations is logged. Thus only the calling 
    statement needs to detect rand() calls made by its substatements. These
    substatements must not set rand_used to 0 because it would remove the
    detection of rand() by the calling statement. 
  */
  if (!in_sub_stmt) /* stored functions and triggers are a special case */
  {
    /* Forget those values, for next binlogger: */
    stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;
    auto_inc_intervals_in_cur_stmt_for_binlog.empty();
    rand_used= 0;
#ifndef EMBEDDED_LIBRARY
    /*
      Clean possible unused INSERT_ID events by current statement.
      is_update_query() is needed to ignore SET statements:
        Statements that don't update anything directly and don't
        used stored functions. This is mostly necessary to ignore
        statements in binlog between SET INSERT_ID and DML statement
        which is intended to consume its event (there can be other
        SET statements between them).
    */
    if ((rgi_slave || rli_fake) && is_update_query(lex->sql_command))
      auto_inc_intervals_forced.empty();
#endif
  }
  /*
    Forget the binlog stmt filter for the next query.
    There are some code paths that:
    - do not call THD::decide_logging_format()
    - do call THD::binlog_query(),
    making this reset necessary.
  */
  reset_binlog_local_stmt_filter();
  if (first_successful_insert_id_in_cur_stmt > 0)
  {
    /* set what LAST_INSERT_ID() will return */
    first_successful_insert_id_in_prev_stmt= 
      first_successful_insert_id_in_cur_stmt;
    first_successful_insert_id_in_cur_stmt= 0;
    substitute_null_with_insert_id= TRUE;
  }
  arg_of_last_insert_id_function= 0;
  /* Free Items that were created during this execution */
  free_items();
  /* Reset where. */
  where= THD::DEFAULT_WHERE;
  /* reset table map for multi-table update */
  table_map_for_update= 0;
  m_binlog_invoker= INVOKER_NONE;

#ifndef EMBEDDED_LIBRARY
  if (rgi_slave)
    rgi_slave->cleanup_after_query();
#endif

#ifdef WITH_WSREP
  if (!in_active_multi_stmt_transaction())
    wsrep_affected_rows= 0;
#endif /* WITH_WSREP */

  DBUG_VOID_RETURN;
}


/*
  Convert a string to another character set

  SYNOPSIS
    convert_string()
    to				Store new allocated string here
    to_cs			New character set for allocated string
    from			String to convert
    from_length			Length of string to convert
    from_cs			Original character set

  NOTES
    to will be 0-terminated to make it easy to pass to system funcs

  RETURN
    0	ok
    1	End of memory.
        In this case to->str will point to 0 and to->length will be 0.
*/

bool THD::convert_string(LEX_STRING *to, CHARSET_INFO *to_cs,
			 const char *from, size_t from_length,
			 CHARSET_INFO *from_cs)
{
  DBUG_ENTER("THD::convert_string");
  size_t new_length= to_cs->mbmaxlen * from_length;
  uint errors;
  if (unlikely(alloc_lex_string(to, new_length + 1)))
    DBUG_RETURN(true);                          // EOM
  to->length= copy_and_convert((char*) to->str, new_length, to_cs,
			       from, from_length, from_cs, &errors);
  to->str[to->length]= 0;                       // Safety
  if (unlikely(errors) && lex->parse_vcol_expr)
  {
    my_error(ER_BAD_DATA, MYF(0),
             ErrConvString(from, from_length, from_cs).ptr(),
             to_cs->cs_name.str);
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/*
  Reinterpret a binary string to a character string

  @param[OUT] to    The result will be written here,
                    either the original string as is,
                    or a newly alloced fixed string with
                    some zero bytes prepended.
  @param cs         The destination character set
  @param str        The binary string
  @param length     The length of the binary string

  @return           false on success
  @return           true on error
*/

bool THD::reinterpret_string_from_binary(LEX_CSTRING *to, CHARSET_INFO *cs,
                                         const char *str, size_t length)
{
  /*
    When reinterpreting from binary to tricky character sets like
    UCS2, UTF16, UTF32, we may need to prepend some zero bytes.
    This is possible in scenarios like this:
      SET COLLATION_CONNECTION=utf32_general_ci, CHARACTER_SET_CLIENT=binary;
    This code is similar to String::copy_aligned().
  */
  size_t incomplete= length % cs->mbminlen; // Bytes in an incomplete character
  if (incomplete)
  {
    size_t zeros= cs->mbminlen - incomplete;
    size_t aligned_length= zeros + length;
    char *dst= (char*) alloc(aligned_length + 1);
    if (!dst)
    {
      to->str= NULL; // Safety
      to->length= 0;
      return true;
    }
    bzero(dst, zeros);
    memcpy(dst + zeros, str, length);
    dst[aligned_length]= '\0';
    to->str= dst;
    to->length= aligned_length;
  }
  else
  {
    to->str= str;
    to->length= length;
  }
  return check_string_for_wellformedness(to->str, to->length, cs);
}


/*
  Convert a string between two character sets.
  dstcs and srccs cannot be &my_charset_bin.
*/
bool THD::convert_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                      CHARSET_INFO *srccs, const char *src, size_t src_length,
                      String_copier *status)
{
  DBUG_ENTER("THD::convert_fix");
  size_t dst_length= dstcs->mbmaxlen * src_length;
  if (alloc_lex_string(dst, dst_length + 1))
    DBUG_RETURN(true);                           // EOM
  dst->length= status->convert_fix(dstcs, (char*) dst->str, dst_length,
                                   srccs, src, src_length, src_length);
  dst->str[dst->length]= 0;                      // Safety
  DBUG_RETURN(false);
}


/*
  Copy or convert a string.
*/
bool THD::copy_fix(CHARSET_INFO *dstcs, LEX_STRING *dst,
                   CHARSET_INFO *srccs, const char *src, size_t src_length,
                   String_copier *status)
{
  DBUG_ENTER("THD::copy_fix");
  size_t dst_length= dstcs->mbmaxlen * src_length;
  if (alloc_lex_string(dst, dst_length + 1))
    DBUG_RETURN(true);                          // EOM
  dst->length= status->well_formed_copy(dstcs, dst->str, dst_length,
                                        srccs, src, src_length, src_length);
  dst->str[dst->length]= '\0';
  DBUG_RETURN(false);
}


class String_copier_with_error: public String_copier
{
public:
  bool check_errors(CHARSET_INFO *srccs, const char *src, size_t src_length)
  {
    if (most_important_error_pos())
    {
      ErrConvString err(src, src_length, &my_charset_bin);
      my_error(ER_INVALID_CHARACTER_STRING, MYF(0), srccs->cs_name.str,
               err.ptr());
      return true;
    }
    return false;
  }
};


bool THD::convert_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                             CHARSET_INFO *srccs,
                             const char *src, size_t src_length)
{
  String_copier_with_error status;
  return convert_fix(dstcs, dst, srccs, src, src_length, &status) ||
         status.check_errors(srccs, src, src_length);
}


bool THD::copy_with_error(CHARSET_INFO *dstcs, LEX_STRING *dst,
                          CHARSET_INFO *srccs,
                          const char *src, size_t src_length)
{
  String_copier_with_error status;
  return copy_fix(dstcs, dst, srccs, src, src_length, &status) ||
         status.check_errors(srccs, src, src_length);
}


/*
  Convert string from source character set to target character set inplace.

  SYNOPSIS
    THD::convert_string

  DESCRIPTION
    Convert string using convert_buffer - buffer for character set 
    conversion shared between all protocols.

  RETURN
    0   ok
   !0   out of memory
*/

bool THD::convert_string(String *s, CHARSET_INFO *from_cs, CHARSET_INFO *to_cs)
{
  uint dummy_errors;
  if (unlikely(convert_buffer.copy(s->ptr(), s->length(), from_cs, to_cs,
                                   &dummy_errors)))
    return TRUE;
  /* If convert_buffer >> s copying is more efficient long term */
  if (convert_buffer.alloced_length() >= convert_buffer.length() * 2 ||
      !s->is_alloced())
  {
    return s->copy(convert_buffer);
  }
  s->swap(convert_buffer);
  return FALSE;
}


bool THD::check_string_for_wellformedness(const char *str,
                                          size_t length,
                                          CHARSET_INFO *cs) const
{
  size_t wlen= Well_formed_prefix(cs, str, length).length();
  if (wlen < length)
  {
    ErrConvString err(str, length, &my_charset_bin);
    my_error(ER_INVALID_CHARACTER_STRING, MYF(0), cs->cs_name.str, err.ptr());
    return true;
  }
  return false;
}


bool THD::to_ident_sys_alloc(Lex_ident_sys_st *to, const Lex_ident_cli_st *ident)
{
  if (ident->is_quoted())
  {
    LEX_CSTRING unquoted;
    if (quote_unescape(&unquoted, ident, ident->quote()))
      return true;
    return charset_is_system_charset ?
           to->copy_sys(this, &unquoted) :
           to->convert(this, &unquoted, charset());
  }
  return charset_is_system_charset ?
         to->copy_sys(this, ident) :
         to->copy_or_convert(this, ident, charset());
}


Item_basic_constant *
THD::make_string_literal(const char *str, size_t length,
                         my_repertoire_t repertoire)
{
  if (!length && (variables.sql_mode & MODE_EMPTY_STRING_IS_NULL))
    return new (mem_root) Item_null(this, 0, variables.collation_connection);
  if (!charset_is_collation_connection &&
      (repertoire != MY_REPERTOIRE_ASCII ||
       !my_charset_is_ascii_based(variables.collation_connection)))
  {
    LEX_STRING to;
    if (convert_string(&to, variables.collation_connection,
                       str, length, variables.character_set_client))
      return NULL;
    str= to.str;
    length= to.length;
  }
  return new (mem_root) Item_string(this, str, (uint)length,
                                    variables.collation_connection,
                                    DERIVATION_COERCIBLE, repertoire);
}


Item_basic_constant *
THD::make_string_literal_nchar(const Lex_string_with_metadata_st &str)
{
  DBUG_ASSERT(my_charset_is_ascii_based(national_charset_info));
  if (!str.length && (variables.sql_mode & MODE_EMPTY_STRING_IS_NULL))
    return new (mem_root) Item_null(this, 0, national_charset_info);

  return new (mem_root) Item_string(this, str.str, (uint)str.length,
                                    national_charset_info,
                                    DERIVATION_COERCIBLE,
                                    str.repertoire());
}


Item_basic_constant *
THD::make_string_literal_charset(const Lex_string_with_metadata_st &str,
                                 CHARSET_INFO *cs)
{
  if (!str.length && (variables.sql_mode & MODE_EMPTY_STRING_IS_NULL))
    return new (mem_root) Item_null(this, 0, cs);
  return new (mem_root) Item_string_with_introducer(this, str, cs);
}


/*
  Update some cache variables when character set changes
*/

void THD::update_charset()
{
  uint32 not_used;
  charset_is_system_charset=
    !String::needs_conversion(0,
                              variables.character_set_client,
                              system_charset_info,
                              &not_used);
  charset_is_collation_connection= 
    !String::needs_conversion(0,
                              variables.character_set_client,
                              variables.collation_connection,
                              &not_used);
  charset_is_character_set_filesystem= 
    !String::needs_conversion(0,
                              variables.character_set_client,
                              variables.character_set_filesystem,
                              &not_used);
}

void THD::give_protection_error()
{
  if (current_backup_stage != BACKUP_FINISHED)
    my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
  else
  {
    DBUG_ASSERT(global_read_lock.is_acquired() || mdl_backup_lock);
    my_error(ER_CANT_UPDATE_WITH_READLOCK, MYF(0));
  }
}

/* routings to adding tables to list of changed in transaction tables */

inline static void list_include(CHANGED_TABLE_LIST** prev,
				CHANGED_TABLE_LIST* curr,
				CHANGED_TABLE_LIST* new_table)
{
  if (new_table)
  {
    *prev = new_table;
    (*prev)->next = curr;
  }
}

/* add table to list of changed in transaction tables */

void THD::add_changed_table(TABLE *table)
{
  DBUG_ENTER("THD::add_changed_table(table)");

  DBUG_ASSERT(in_multi_stmt_transaction_mode() &&
              table->file->has_transactions());
  add_changed_table(table->s->table_cache_key.str,
                    (long) table->s->table_cache_key.length);
  DBUG_VOID_RETURN;
}


void THD::add_changed_table(const char *key, size_t key_length)
{
  DBUG_ENTER("THD::add_changed_table(key)");
  CHANGED_TABLE_LIST **prev_changed = &transaction->changed_tables;
  CHANGED_TABLE_LIST *curr = transaction->changed_tables;

  for (; curr; prev_changed = &(curr->next), curr = curr->next)
  {
    int cmp =  (long)curr->key_length - (long)key_length;
    if (cmp < 0)
    {
      list_include(prev_changed, curr, changed_table_dup(key, key_length));
      DBUG_PRINT("info", 
		 ("key_length: %zu  %zu", key_length,
                  (*prev_changed)->key_length));
      DBUG_VOID_RETURN;
    }
    else if (cmp == 0)
    {
      cmp = memcmp(curr->key, key, curr->key_length);
      if (cmp < 0)
      {
	list_include(prev_changed, curr, changed_table_dup(key, key_length));
	DBUG_PRINT("info", 
		   ("key_length:  %zu  %zu", key_length,
		    (*prev_changed)->key_length));
	DBUG_VOID_RETURN;
      }
      else if (cmp == 0)
      {
	DBUG_PRINT("info", ("already in list"));
	DBUG_VOID_RETURN;
      }
    }
  }
  *prev_changed = changed_table_dup(key, key_length);
  DBUG_PRINT("info", ("key_length: %zu  %zu", key_length,
		      (*prev_changed)->key_length));
  DBUG_VOID_RETURN;
}


CHANGED_TABLE_LIST* THD::changed_table_dup(const char *key, size_t key_length)
{
  CHANGED_TABLE_LIST* new_table = 
    (CHANGED_TABLE_LIST*) trans_alloc(ALIGN_SIZE(sizeof(CHANGED_TABLE_LIST))+
				      key_length + 1);
  if (!new_table)
  {
    my_error(EE_OUTOFMEMORY, MYF(ME_FATAL),
             ALIGN_SIZE(sizeof(TABLE_LIST)) + key_length + 1);
    set_killed(KILL_CONNECTION);
    return 0;
  }

  new_table->key= ((char*)new_table)+ ALIGN_SIZE(sizeof(CHANGED_TABLE_LIST));
  new_table->next = 0;
  new_table->key_length = key_length;
  ::memcpy(new_table->key, key, key_length);
  return new_table;
}


int THD::prepare_explain_fields(select_result *result, List<Item> *field_list,
                                 uint8 explain_flags, bool is_analyze)
{
  if (lex->explain_json)
    make_explain_json_field_list(*field_list, is_analyze);
  else
    make_explain_field_list(*field_list, explain_flags, is_analyze);

  return result->prepare(*field_list, NULL);
}


int THD::send_explain_fields(select_result *result,
                             uint8 explain_flags,
                             bool is_analyze)
{
  List<Item> field_list;
  int rc;
  rc= prepare_explain_fields(result, &field_list, explain_flags, is_analyze) ||
      result->send_result_set_metadata(field_list, Protocol::SEND_NUM_ROWS |
                                                   Protocol::SEND_EOF);
  return rc;
}


void THD::make_explain_json_field_list(List<Item> &field_list, bool is_analyze)
{
  Item *item= new (mem_root) Item_empty_string(this, (is_analyze ?
                                                      "ANALYZE" :
                                                      "EXPLAIN"),
                                              78, system_charset_info);
  field_list.push_back(item, mem_root);
}


/*
  Populate the provided field_list with EXPLAIN output columns.
  this->lex->describe has the EXPLAIN flags

  The set/order of columns must be kept in sync with 
  Explain_query::print_explain and co.
*/

void THD::make_explain_field_list(List<Item> &field_list, uint8 explain_flags,
                                  bool is_analyze)
{
  Item *item;
  CHARSET_INFO *cs= system_charset_info;
  field_list.push_back(item= new (mem_root)
                       Item_return_int(this, "id", 3,
                                       MYSQL_TYPE_LONGLONG), mem_root);
  item->set_maybe_null();
  field_list.push_back(new (mem_root)
                       Item_empty_string(this, "select_type", 19, cs),
                       mem_root);
  field_list.push_back(item= new (mem_root)
                       Item_empty_string(this, "table", NAME_CHAR_LEN, cs),
                       mem_root);
  item->set_maybe_null();
  if (explain_flags & DESCRIBE_PARTITIONS)
  {
    /* Maximum length of string that make_used_partitions_str() can produce */
    item= new (mem_root) Item_empty_string(this, "partitions",
                                           MAX_PARTITIONS * (1 + FN_LEN), cs);
    field_list.push_back(item, mem_root);
    item->set_maybe_null();
  }
  field_list.push_back(item= new (mem_root)
                       Item_empty_string(this, "type", 10, cs),
                       mem_root);
  item->set_maybe_null();
  field_list.push_back(item= new (mem_root)
                       Item_empty_string(this, "possible_keys",
                                         NAME_CHAR_LEN*MAX_KEY, cs),
                       mem_root);
  item->set_maybe_null();
  field_list.push_back(item=new (mem_root)
                       Item_empty_string(this, "key", NAME_CHAR_LEN, cs),
                       mem_root);
  item->set_maybe_null();
  field_list.push_back(item=new (mem_root)
                       Item_empty_string(this, "key_len",
                                         NAME_CHAR_LEN*MAX_KEY),
                       mem_root);
  item->set_maybe_null();
  field_list.push_back(item=new (mem_root)
                       Item_empty_string(this, "ref",
                                         NAME_CHAR_LEN*MAX_REF_PARTS, cs),
                       mem_root);
  item->set_maybe_null();
  field_list.push_back(item=new (mem_root)
                       Item_empty_string(this, "rows", NAME_CHAR_LEN, cs),
                       mem_root);
  if (is_analyze)
  {
    field_list.push_back(item= new (mem_root)
                         Item_empty_string(this, "r_rows", NAME_CHAR_LEN, cs),
                         mem_root);
    item->set_maybe_null();
  }

  if (is_analyze || (explain_flags & DESCRIBE_EXTENDED))
  {
    field_list.push_back(item= new (mem_root)
                         Item_float(this, "filtered", 0.1234, 2, 4),
                         mem_root);
    item->set_maybe_null();
  }

  if (is_analyze)
  {
    field_list.push_back(item= new (mem_root)
                         Item_float(this, "r_filtered", 0.1234, 2, 4),
                         mem_root);
    item->set_maybe_null();
  }

  item->set_maybe_null();
  field_list.push_back(new (mem_root)
                       Item_empty_string(this, "Extra", 255, cs),
                       mem_root);
}


#ifdef SIGNAL_WITH_VIO_CLOSE
void THD::close_active_vio()
{
  DBUG_ENTER("close_active_vio");
  mysql_mutex_assert_owner(&LOCK_thd_data);
#ifndef EMBEDDED_LIBRARY
  if (active_vio)
  {
    vio_close(active_vio);
    active_vio = 0;
  }
#endif
  DBUG_VOID_RETURN;
}
#endif


/*
  @brief MySQL parser used for recursive invocations

  @param old_lex  The LEX structure in the state when this parser
                  is called recursively
  @param lex      The LEX structure used to parse a new SQL fragment
  @param str      The SQL fragment to parse
  @param str_len  The length of the SQL fragment to parse
  @param stmt_prepare_mode true <=> when parsing a prepare statement

  @details
    This function is to be used when parsing of an SQL fragment is
    needed within one of the grammar rules.

  @notes
    Currently the function is used only when the specification of a CTE
    is parsed for the not first and not recursive references of the CTE.

  @retval false   On a successful parsing of the fragment
  @retval true    Otherwise
*/

bool THD::sql_parser(LEX *old_lex, LEX *lex,
                     char *str, uint str_len, bool stmt_prepare_mode)
{
  extern int MYSQLparse(THD * thd);
  extern int ORAparse(THD * thd);

  bool parse_status= false;
  Parser_state parser_state;
  Parser_state *old_parser_state= m_parser_state;

  if (parser_state.init(this, str, str_len))
    return true;

  m_parser_state= &parser_state;
  parser_state.m_lip.stmt_prepare_mode= stmt_prepare_mode;
  parser_state.m_lip.multi_statements= false;
  parser_state.m_lip.m_digest= NULL;

  lex->param_list= old_lex->param_list;
  lex->sphead= old_lex->sphead;
  lex->spname= old_lex->spname;
  lex->spcont= old_lex->spcont;
  lex->sp_chistics= old_lex->sp_chistics;
  lex->trg_chistics= old_lex->trg_chistics;

  parse_status= (variables.sql_mode & MODE_ORACLE) ?
                 ORAparse(this) : MYSQLparse(this) != 0;

  m_parser_state= old_parser_state;

  return parse_status;
}


struct Item_change_record: public ilink
{
  Item **place;
  Item *old_value;
  /* Placement new was hidden by `new' in ilink (TODO: check): */
  static void *operator new(size_t size, void *mem) { return mem; }
  static void operator delete(void *ptr, size_t size) {}
  static void operator delete(void *ptr, void *mem) { /* never called */ }
};


/*
  Register an item tree tree transformation, performed by the query
  optimizer. We need a pointer to runtime_memroot because it may be !=
  thd->mem_root (due to possible set_n_backup_active_arena called for thd).
*/

void
Item_change_list::nocheck_register_item_tree_change(Item **place,
                                                    Item *old_value,
                                                    MEM_ROOT *runtime_memroot)
{
  Item_change_record *change;
  DBUG_ENTER("THD::nocheck_register_item_tree_change");
  DBUG_PRINT("enter", ("Register %p <- %p", old_value, (*place)));
  /*
    Now we use one node per change, which adds some memory overhead,
    but still is rather fast as we use alloc_root for allocations.
    A list of item tree changes of an average query should be short.
  */
  void *change_mem= alloc_root(runtime_memroot, sizeof(*change));
  if (change_mem == 0)
  {
    /*
      OOM, thd->fatal_error() is called by the error handler of the
      memroot. Just return.
    */
    DBUG_VOID_RETURN;
  }
  change= new (change_mem) Item_change_record;
  change->place= place;
  change->old_value= old_value;
  change_list.append(change);
  DBUG_VOID_RETURN;
}

/**
  Check and register item change if needed

  @param place           place where we should assign new value
  @param new_value       place of the new value

  @details
    Let C be a reference to an item that changed the reference A
    at the location (occurrence) L1 and this change has been registered.
    If C is substituted for reference A another location (occurrence) L2
    that is to be registered as well than this change has to be
    consistent with the first change in order the procedure that rollback
    changes to substitute the same reference at both locations L1 and L2.
*/

void
Item_change_list::check_and_register_item_tree_change(Item **place,
                                                      Item **new_value,
                                                      MEM_ROOT *runtime_memroot)
{
  Item_change_record *change;
  DBUG_ENTER("THD::check_and_register_item_tree_change");
  DBUG_PRINT("enter", ("Register: %p (%p) <- %p (%p)",
                       *place, place, *new_value, new_value));
  I_List_iterator<Item_change_record> it(change_list);
  while ((change= it++))
  {
    if (change->place == new_value)
      break; // we need only very first value
  }
  if (change)
    nocheck_register_item_tree_change(place, change->old_value,
                                      runtime_memroot);
  DBUG_VOID_RETURN;
}


void Item_change_list::rollback_item_tree_changes()
{
  DBUG_ENTER("THD::rollback_item_tree_changes");
  I_List_iterator<Item_change_record> it(change_list);
  Item_change_record *change;

  while ((change= it++))
  {
    DBUG_PRINT("info", ("Rollback: %p (%p) <- %p",
                        *change->place, change->place, change->old_value));
    *change->place= change->old_value;
  }
  /* We can forget about changes memory: it's allocated in runtime memroot */
  change_list.empty();
  DBUG_VOID_RETURN;
}


/*****************************************************************************
** Functions to provide a interface to select results
*****************************************************************************/

void select_result::cleanup()
{
  /* do nothing */
}

bool select_result::check_simple_select() const
{
  my_error(ER_SP_BAD_CURSOR_QUERY, MYF(0));
  return TRUE;
}


static String default_line_term("\n", 1, default_charset_info);
static String default_escaped("\\", 1, default_charset_info);
static String default_field_term("\t", 1, default_charset_info);
static String default_enclosed_and_line_start("", 0, default_charset_info);
static String default_xml_row_term("<row>", 5, default_charset_info);

sql_exchange::sql_exchange(const char *name, bool flag,
                           enum enum_filetype filetype_arg)
  :file_name(name), opt_enclosed(0), dumpfile(flag), skip_lines(0)
{
  filetype= filetype_arg;
  field_term= &default_field_term;
  enclosed=   line_start= &default_enclosed_and_line_start;
  line_term=  filetype == FILETYPE_CSV ?
              &default_line_term : &default_xml_row_term;
  escaped=    &default_escaped;
  cs= NULL;
}

bool sql_exchange::escaped_given(void) const
{
  return escaped != &default_escaped;
}


bool select_send::send_result_set_metadata(List<Item> &list, uint flags)
{
  bool res;
#ifdef WITH_WSREP
  if (WSREP(thd) && thd->wsrep_retry_query)
  {
    WSREP_DEBUG("skipping select metadata");
    return FALSE;
  }
#endif /* WITH_WSREP */
  if (!(res= thd->protocol->send_result_set_metadata(&list, flags)))
    is_result_set_started= 1;
  return res;
}

void select_send::abort_result_set()
{
  DBUG_ENTER("select_send::abort_result_set");

  if (is_result_set_started && thd->spcont)
  {
    /*
      We're executing a stored procedure, have an open result
      set and an SQL exception condition. In this situation we
      must abort the current statement, silence the error and
      start executing the continue/exit handler if one is found.
      Before aborting the statement, let's end the open result set, as
      otherwise the client will hang due to the violation of the
      client/server protocol.
    */
    thd->spcont->end_partial_result_set= TRUE;
  }
  DBUG_VOID_RETURN;
}


/** 
  Cleanup an instance of this class for re-use
  at next execution of a prepared statement/
  stored procedure statement.
*/

void select_send::cleanup()
{
  is_result_set_started= FALSE;
}

/* Send data to client. Returns 0 if ok */

int select_send::send_data(List<Item> &items)
{
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("select_send::send_data");

  protocol->prepare_for_resend();
  if (protocol->send_result_set_row(&items))
  {
    protocol->remove_last_row();
    DBUG_RETURN(TRUE);
  }

  thd->inc_sent_row_count(1);

  /* Don't return error if disconnected, only if write fails */
  if (likely(thd->vio_ok()))
    DBUG_RETURN(protocol->write());

  DBUG_RETURN(0);
}


bool select_send::send_eof()
{
  /* 
    Don't send EOF if we're in error condition (which implies we've already
    sent or are sending an error)
  */
  if (unlikely(thd->is_error()))
    return TRUE;
  ::my_eof(thd);
  is_result_set_started= 0;
  return FALSE;
}


/************************************************************************
  Handling writing to file
************************************************************************/

bool select_to_file::send_eof()
{
  int error= MY_TEST(end_io_cache(&cache));
  if (unlikely(mysql_file_close(file, MYF(MY_WME))) ||
      unlikely(thd->is_error()))
    error= true;

  if (likely(!error) && !suppress_my_ok)
  {
    ::my_ok(thd,row_count);
  }
  file= -1;
  return error;
}


void select_to_file::cleanup()
{
  /* In case of error send_eof() may be not called: close the file here. */
  if (file >= 0)
  {
    (void) end_io_cache(&cache);
    mysql_file_close(file, MYF(0));
    file= -1;
  }
  path[0]= '\0';
  row_count= 0;
}


select_to_file::~select_to_file()
{
  if (file >= 0)
  {					// This only happens in case of error
    (void) end_io_cache(&cache);
    mysql_file_close(file, MYF(0));
    file= -1;
  }
}

/***************************************************************************
** Export of select to textfile
***************************************************************************/

select_export::~select_export()
{
  thd->set_sent_row_count(row_count);
}


/*
  Create file with IO cache

  SYNOPSIS
    create_file()
    thd			Thread handle
    path		File name
    exchange		Excange class
    cache		IO cache

  RETURN
    >= 0 	File handle
   -1		Error
*/


static File create_file(THD *thd, char *path, sql_exchange *exchange,
			IO_CACHE *cache)
{
  File file;
  uint option= MY_UNPACK_FILENAME | MY_RELATIVE_PATH;

#ifdef DONT_ALLOW_FULL_LOAD_DATA_PATHS
  option|= MY_REPLACE_DIR;			// Force use of db directory
#endif

  if (!dirname_length(exchange->file_name))
  {
    strxnmov(path, FN_REFLEN-1, mysql_real_data_home, thd->get_db(), NullS);
    (void) fn_format(path, exchange->file_name, path, "", option);
  }
  else
    (void) fn_format(path, exchange->file_name, mysql_real_data_home, "", option);

  if (!is_secure_file_path(path))
  {
    /* Write only allowed to dir or subdir specified by secure_file_priv */
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
    return -1;
  }

  if (!access(path, F_OK))
  {
    my_error(ER_FILE_EXISTS_ERROR, MYF(0), exchange->file_name);
    return -1;
  }
  /* Create the file world readable */
  if ((file= mysql_file_create(key_select_to_file,
                               path, 0644, O_WRONLY|O_EXCL, MYF(MY_WME))) < 0)
    return file;
#ifdef HAVE_FCHMOD
  (void) fchmod(file, 0644);			// Because of umask()
#else
  (void) chmod(path, 0644);
#endif
  if (init_io_cache(cache, file, 0L, WRITE_CACHE, 0L, 1, MYF(MY_WME)))
  {
    mysql_file_close(file, MYF(0));
    /* Delete file on error, it was just created */
    mysql_file_delete(key_select_to_file, path, MYF(0));
    return -1;
  }
  return file;
}


int
select_export::prepare(List<Item> &list, SELECT_LEX_UNIT *u)
{
  bool blob_flag=0;
  bool string_results= FALSE, non_string_results= FALSE;
  unit= u;
  if ((uint) strlen(exchange->file_name) + NAME_LEN >= FN_REFLEN)
    strmake_buf(path,exchange->file_name);

  write_cs= exchange->cs ? exchange->cs : &my_charset_bin;

  if ((file= create_file(thd, path, exchange, &cache)) < 0)
    return 1;
  /* Check if there is any blobs in data */
  {
    List_iterator_fast<Item> li(list);
    Item *item;
    while ((item=li++))
    {
      if (item->max_length >= MAX_BLOB_WIDTH)
      {
	blob_flag=1;
	break;
      }
      if (item->result_type() == STRING_RESULT)
        string_results= TRUE;
      else
        non_string_results= TRUE;
    }
  }
  if (exchange->escaped->numchars() > 1 || exchange->enclosed->numchars() > 1)
  {
    my_error(ER_WRONG_FIELD_TERMINATORS, MYF(0));
    return TRUE;
  }
  if (exchange->escaped->length() > 1 || exchange->enclosed->length() > 1 ||
      !my_isascii(exchange->escaped->ptr()[0]) ||
      !my_isascii(exchange->enclosed->ptr()[0]) ||
      !exchange->field_term->is_ascii() || !exchange->line_term->is_ascii() ||
      !exchange->line_start->is_ascii())
  {
    /*
      Current LOAD DATA INFILE recognizes field/line separators "as is" without
      converting from client charset to data file charset. So, it is supposed,
      that input file of LOAD DATA INFILE consists of data in one charset and
      separators in other charset. For the compatibility with that [buggy]
      behaviour SELECT INTO OUTFILE implementation has been saved "as is" too,
      but the new warning message has been added:

        Non-ASCII separator arguments are not fully supported
    */
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED,
                 ER_THD(thd, WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED));
  }
  field_term_length=exchange->field_term->length();
  field_term_char= field_term_length ?
                   (int) (uchar) (*exchange->field_term)[0] : INT_MAX;
  if (!exchange->line_term->length())
    exchange->line_term=exchange->field_term;	// Use this if it exists
  field_sep_char= (exchange->enclosed->length() ?
                  (int) (uchar) (*exchange->enclosed)[0] : field_term_char);
  if (exchange->escaped->length() && (exchange->escaped_given() ||
      !(thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)))
    escape_char= (int) (uchar) (*exchange->escaped)[0];
  else
    escape_char= -1;
  is_ambiguous_field_sep= MY_TEST(strchr(ESCAPE_CHARS, field_sep_char));
  is_unsafe_field_sep= MY_TEST(strchr(NUMERIC_CHARS, field_sep_char));
  line_sep_char= (exchange->line_term->length() ?
                 (int) (uchar) (*exchange->line_term)[0] : INT_MAX);
  if (!field_term_length)
    exchange->opt_enclosed=0;
  if (!exchange->enclosed->length())
    exchange->opt_enclosed=1;			// A little quicker loop
  fixed_row_size= (!field_term_length && !exchange->enclosed->length() &&
		   !blob_flag);
  if ((is_ambiguous_field_sep && exchange->enclosed->is_empty() &&
       (string_results || is_unsafe_field_sep)) ||
      (exchange->opt_enclosed && non_string_results &&
       field_term_length && strchr(NUMERIC_CHARS, field_term_char)))
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_AMBIGUOUS_FIELD_TERM,
                 ER_THD(thd, ER_AMBIGUOUS_FIELD_TERM));
    is_ambiguous_field_term= TRUE;
  }
  else
    is_ambiguous_field_term= FALSE;

  return 0;
}


#define NEED_ESCAPING(x) ((int) (uchar) (x) == escape_char    || \
                          (enclosed ? (int) (uchar) (x) == field_sep_char      \
                                    : (int) (uchar) (x) == field_term_char) || \
                          (int) (uchar) (x) == line_sep_char  || \
                          !(x))

int select_export::send_data(List<Item> &items)
{

  DBUG_ENTER("select_export::send_data");
  char buff[MAX_FIELD_WIDTH],null_buff[2],space[MAX_FIELD_WIDTH];
  char cvt_buff[MAX_FIELD_WIDTH];
  String cvt_str(cvt_buff, sizeof(cvt_buff), write_cs);
  bool space_inited=0;
  String tmp(buff,sizeof(buff),&my_charset_bin),*res;
  tmp.length(0);

  row_count++;
  Item *item;
  uint used_length=0,items_left=items.elements;
  List_iterator_fast<Item> li(items);

  if (my_b_write(&cache,(uchar*) exchange->line_start->ptr(),
		 exchange->line_start->length()))
    goto err;
  while ((item=li++))
  {
    Item_result result_type=item->result_type();
    bool enclosed = (exchange->enclosed->length() &&
                     (!exchange->opt_enclosed || result_type == STRING_RESULT));
    res=item->str_result(&tmp);
    if (res && !my_charset_same(write_cs, res->charset()) &&
        !my_charset_same(write_cs, &my_charset_bin))
    {
      String_copier copier;
      const char *error_pos;
      uint32 bytes;
      uint64 estimated_bytes=
        ((uint64) res->length() / res->charset()->mbminlen + 1) *
        write_cs->mbmaxlen + 1;
      set_if_smaller(estimated_bytes, UINT_MAX32);
      if (cvt_str.alloc((uint32) estimated_bytes))
      {
        my_error(ER_OUTOFMEMORY, MYF(ME_FATAL), (uint32) estimated_bytes);
        goto err;
      }

      bytes= copier.well_formed_copy(write_cs, (char *) cvt_str.ptr(),
                                     cvt_str.alloced_length(),
                                     res->charset(),
                                     res->ptr(), res->length());
      error_pos= copier.most_important_error_pos();
      if (unlikely(error_pos))
      {
        /*
          TODO: 
             add new error message that will show user this printable_buff

        char printable_buff[32];
        convert_to_printable(printable_buff, sizeof(printable_buff),
                             error_pos, res->ptr() + res->length() - error_pos,
                             res->charset(), 6);
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_TRUNCATED_WRONG_VALUE_FOR_FIELD,
                            ER_THD(thd, ER_TRUNCATED_WRONG_VALUE_FOR_FIELD),
                            "string", printable_buff,
                            item->name.str, static_cast<long>(row_count));
        */
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_TRUNCATED_WRONG_VALUE_FOR_FIELD,
                            ER_THD(thd, WARN_DATA_TRUNCATED),
                            item->name.str, static_cast<long>(row_count));
      }
      else if (copier.source_end_pos() < res->ptr() + res->length())
      { 
        /*
          result is longer than UINT_MAX32 and doesn't fit into String
        */
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            WARN_DATA_TRUNCATED,
                            ER_THD(thd, WARN_DATA_TRUNCATED),
                            item->full_name(), static_cast<long>(row_count));
      }
      cvt_str.length(bytes);
      res= &cvt_str;
    }
    if (res && enclosed)
    {
      if (my_b_write(&cache,(uchar*) exchange->enclosed->ptr(),
		     exchange->enclosed->length()))
	goto err;
    }
    if (!res)
    {						// NULL
      if (!fixed_row_size)
      {
	if (escape_char != -1)			// Use \N syntax
	{
	  null_buff[0]=escape_char;
	  null_buff[1]='N';
	  if (my_b_write(&cache,(uchar*) null_buff,2))
	    goto err;
	}
	else if (my_b_write(&cache,(uchar*) "NULL",4))
	  goto err;
      }
      else
      {
	used_length=0;				// Fill with space
      }
    }
    else
    {
      if (fixed_row_size)
	used_length=MY_MIN(res->length(),item->max_length);
      else
	used_length=res->length();
      if ((result_type == STRING_RESULT || is_unsafe_field_sep) &&
           escape_char != -1)
      {
        char *pos, *start, *end;
        CHARSET_INFO *res_charset= res->charset();
        CHARSET_INFO *character_set_client= thd->variables.
                                            character_set_client;
        bool check_second_byte= (res_charset == &my_charset_bin) &&
                                 character_set_client->
                                 escape_with_backslash_is_dangerous;
        DBUG_ASSERT(character_set_client->mbmaxlen == 2 ||
                    !character_set_client->escape_with_backslash_is_dangerous);
	for (start=pos=(char*) res->ptr(),end=pos+used_length ;
	     pos != end ;
	     pos++)
	{
#ifdef USE_MB
	  if (res_charset->use_mb())
	  {
	    int l;
	    if ((l=my_ismbchar(res_charset, pos, end)))
	    {
	      pos += l-1;
	      continue;
	    }
	  }
#endif

          /*
            Special case when dumping BINARY/VARBINARY/BLOB values
            for the clients with character sets big5, cp932, gbk and sjis,
            which can have the escape character (0x5C "\" by default)
            as the second byte of a multi-byte sequence.
            
            If
            - pos[0] is a valid multi-byte head (e.g 0xEE) and
            - pos[1] is 0x00, which will be escaped as "\0",
            
            then we'll get "0xEE + 0x5C + 0x30" in the output file.
            
            If this file is later loaded using this sequence of commands:
            
            mysql> create table t1 (a varchar(128)) character set big5;
            mysql> LOAD DATA INFILE 'dump.txt' INTO TABLE t1;
            
            then 0x5C will be misinterpreted as the second byte
            of a multi-byte character "0xEE + 0x5C", instead of
            escape character for 0x00.
            
            To avoid this confusion, we'll escape the multi-byte
            head character too, so the sequence "0xEE + 0x00" will be
            dumped as "0x5C + 0xEE + 0x5C + 0x30".
            
            Note, in the condition below we only check if
            mbcharlen is equal to 2, because there are no
            character sets with mbmaxlen longer than 2
            and with escape_with_backslash_is_dangerous set.
            DBUG_ASSERT before the loop makes that sure.
          */

          if ((NEED_ESCAPING(*pos) ||
               (check_second_byte &&
                ((uchar) *pos) > 0x7F /* a potential MB2HEAD */ &&
                pos + 1 < end &&
                NEED_ESCAPING(pos[1]))) &&
              /*
               Don't escape field_term_char by doubling - doubling is only
               valid for ENCLOSED BY characters:
              */
              (enclosed || !is_ambiguous_field_term ||
               (int) (uchar) *pos != field_term_char))
          {
	    char tmp_buff[2];
            tmp_buff[0]= ((int) (uchar) *pos == field_sep_char &&
                          is_ambiguous_field_sep) ?
                          field_sep_char : escape_char;
	    tmp_buff[1]= *pos ? *pos : '0';
	    if (my_b_write(&cache,(uchar*) start,(uint) (pos-start)) ||
		my_b_write(&cache,(uchar*) tmp_buff,2))
	      goto err;
	    start=pos+1;
	  }
	}
	if (my_b_write(&cache,(uchar*) start,(uint) (pos-start)))
	  goto err;
      }
      else if (my_b_write(&cache,(uchar*) res->ptr(),used_length))
	goto err;
    }
    if (fixed_row_size)
    {						// Fill with space
      if (item->max_length > used_length)
      {
	if (!space_inited)
	{
	  space_inited=1;
	  bfill(space,sizeof(space),' ');
	}
	uint length=item->max_length-used_length;
	for (; length > sizeof(space) ; length-=sizeof(space))
	{
	  if (my_b_write(&cache,(uchar*) space,sizeof(space)))
	    goto err;
	}
	if (my_b_write(&cache,(uchar*) space,length))
	  goto err;
      }
    }
    if (res && enclosed)
    {
      if (my_b_write(&cache, (uchar*) exchange->enclosed->ptr(),
                     exchange->enclosed->length()))
        goto err;
    }
    if (--items_left)
    {
      if (my_b_write(&cache, (uchar*) exchange->field_term->ptr(),
                     field_term_length))
        goto err;
    }
  }
  if (my_b_write(&cache,(uchar*) exchange->line_term->ptr(),
		 exchange->line_term->length()))
    goto err;
  DBUG_RETURN(0);
err:
  DBUG_RETURN(1);
}


/***************************************************************************
** Dump  of select to a binary file
***************************************************************************/


int
select_dump::prepare(List<Item> &list __attribute__((unused)),
		     SELECT_LEX_UNIT *u)
{
  unit= u;
  return (int) ((file= create_file(thd, path, exchange, &cache)) < 0);
}


int select_dump::send_data(List<Item> &items)
{
  List_iterator_fast<Item> li(items);
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff),&my_charset_bin),*res;
  tmp.length(0);
  Item *item;
  DBUG_ENTER("select_dump::send_data");

  if (row_count++ > 1) 
  {
    my_message(ER_TOO_MANY_ROWS, ER_THD(thd, ER_TOO_MANY_ROWS), MYF(0));
    goto err;
  }
  while ((item=li++))
  {
    res=item->str_result(&tmp);
    if (!res)					// If NULL
    {
      if (my_b_write(&cache,(uchar*) "",1))
	goto err;
    }
    else if (my_b_write(&cache,(uchar*) res->ptr(),res->length()))
    {
      my_error(ER_ERROR_ON_WRITE, MYF(0), path, my_errno);
      goto err;
    }
  }
  DBUG_RETURN(0);
err:
  DBUG_RETURN(1);
}


int select_singlerow_subselect::send_data(List<Item> &items)
{
  DBUG_ENTER("select_singlerow_subselect::send_data");
  Item_singlerow_subselect *it= (Item_singlerow_subselect *)item;
  if (it->assigned())
  {
    my_message(ER_SUBQUERY_NO_1_ROW, ER_THD(thd, ER_SUBQUERY_NO_1_ROW),
               MYF(current_thd->lex->ignore ? ME_WARNING : 0));
    DBUG_RETURN(1);
  }
  List_iterator_fast<Item> li(items);
  Item *val_item;
  for (uint i= 0; (val_item= li++); i++)
    it->store(i, val_item);
  it->assigned(1);
  DBUG_RETURN(0);
}


void select_max_min_finder_subselect::cleanup()
{
  DBUG_ENTER("select_max_min_finder_subselect::cleanup");
  cache= 0;
  DBUG_VOID_RETURN;
}


int select_max_min_finder_subselect::send_data(List<Item> &items)
{
  DBUG_ENTER("select_max_min_finder_subselect::send_data");
  Item_maxmin_subselect *it= (Item_maxmin_subselect *)item;
  List_iterator_fast<Item> li(items);
  Item *val_item= li++;
  it->register_value();
  if (it->assigned())
  {
    cache->store(val_item);
    if ((this->*op)())
      it->store(0, cache);
  }
  else
  {
    if (!cache)
    {
      cache= val_item->get_cache(thd);
      switch (val_item->cmp_type()) {
      case REAL_RESULT:
	op= &select_max_min_finder_subselect::cmp_real;
	break;
      case INT_RESULT:
	op= &select_max_min_finder_subselect::cmp_int;
	break;
      case STRING_RESULT:
	op= &select_max_min_finder_subselect::cmp_str;
	break;
      case DECIMAL_RESULT:
        op= &select_max_min_finder_subselect::cmp_decimal;
        break;
      case TIME_RESULT:
        if (val_item->field_type() == MYSQL_TYPE_TIME)
          op= &select_max_min_finder_subselect::cmp_time;
        else
          op= &select_max_min_finder_subselect::cmp_str;
        break;
      case ROW_RESULT:
        // This case should never be chosen
	DBUG_ASSERT(0);
	op= 0;
      }
    }
    cache->store(val_item);
    it->store(0, cache);
  }
  it->assigned(1);
  DBUG_RETURN(0);
}

bool select_max_min_finder_subselect::cmp_real()
{
  Item *maxmin= ((Item_singlerow_subselect *)item)->element_index(0);
  double val1= cache->val_real(), val2= maxmin->val_real();

  /* Ignore NULLs for ANY and keep them for ALL subqueries */
  if (cache->null_value)
    return (is_all && !maxmin->null_value) || (!is_all && maxmin->null_value);
  if (maxmin->null_value)
    return !is_all;

  if (fmax)
    return(val1 > val2);
  return (val1 < val2);
}

bool select_max_min_finder_subselect::cmp_int()
{
  Item *maxmin= ((Item_singlerow_subselect *)item)->element_index(0);
  longlong val1= cache->val_int(), val2= maxmin->val_int();

  /* Ignore NULLs for ANY and keep them for ALL subqueries */
  if (cache->null_value)
    return (is_all && !maxmin->null_value) || (!is_all && maxmin->null_value);
  if (maxmin->null_value)
    return !is_all;

  if (fmax)
    return(val1 > val2);
  return (val1 < val2);
}

bool select_max_min_finder_subselect::cmp_time()
{
  Item *maxmin= ((Item_singlerow_subselect *)item)->element_index(0);
  THD *thd= current_thd;
  auto val1= cache->val_time_packed(thd), val2= maxmin->val_time_packed(thd);

  /* Ignore NULLs for ANY and keep them for ALL subqueries */
  if (cache->null_value)
    return (is_all && !maxmin->null_value) || (!is_all && maxmin->null_value);
  if (maxmin->null_value)
    return !is_all;

  if (fmax)
    return(val1 > val2);
  return (val1 < val2);
}

bool select_max_min_finder_subselect::cmp_decimal()
{
  Item *maxmin= ((Item_singlerow_subselect *)item)->element_index(0);
  VDec cvalue(cache), mvalue(maxmin);

  /* Ignore NULLs for ANY and keep them for ALL subqueries */
  if (cvalue.is_null())
    return (is_all && !mvalue.is_null()) || (!is_all && mvalue.is_null());
  if (mvalue.is_null())
    return !is_all;

  return fmax ? cvalue.cmp(mvalue) > 0 : cvalue.cmp(mvalue) < 0;
}

bool select_max_min_finder_subselect::cmp_str()
{
  String *val1, *val2, buf1, buf2;
  Item *maxmin= ((Item_singlerow_subselect *)item)->element_index(0);
  /*
    as far as both operand is Item_cache buf1 & buf2 will not be used,
    but added for safety
  */
  val1= cache->val_str(&buf1);
  val2= maxmin->val_str(&buf2);

  /* Ignore NULLs for ANY and keep them for ALL subqueries */
  if (cache->null_value)
    return (is_all && !maxmin->null_value) || (!is_all && maxmin->null_value);
  if (maxmin->null_value)
    return !is_all;

  if (fmax)
    return (sortcmp(val1, val2, cache->collation.collation) > 0) ;
  return (sortcmp(val1, val2, cache->collation.collation) < 0);
}

int select_exists_subselect::send_data(List<Item> &items)
{
  DBUG_ENTER("select_exists_subselect::send_data");
  Item_exists_subselect *it= (Item_exists_subselect *)item;
  it->value= 1;
  it->assigned(1);
  DBUG_RETURN(0);
}


/***************************************************************************
  Dump of select to variables
***************************************************************************/

int select_dumpvar::prepare(List<Item> &list, SELECT_LEX_UNIT *u)
{
  my_var_sp *mvsp;
  unit= u;
  m_var_sp_row= NULL;

  if (var_list.elements == 1 &&
      (mvsp= var_list.head()->get_my_var_sp()) &&
      mvsp->type_handler() == &type_handler_row)
  {
    // SELECT INTO row_type_sp_variable
    if (mvsp->get_rcontext(thd->spcont)->get_variable(mvsp->offset)->cols() !=
        list.elements)
      goto error;
    m_var_sp_row= mvsp;
    return 0;
  }

  // SELECT INTO variable list
  if (var_list.elements == list.elements)
    return 0;

error:
  my_message(ER_WRONG_NUMBER_OF_COLUMNS_IN_SELECT,
             ER_THD(thd, ER_WRONG_NUMBER_OF_COLUMNS_IN_SELECT), MYF(0));
  return 1;
}


bool select_dumpvar::check_simple_select() const
{
  my_error(ER_SP_BAD_CURSOR_SELECT, MYF(0));
  return TRUE;
}


void select_dumpvar::cleanup()
{
  row_count= 0;
}


Query_arena::Type Query_arena::type() const
{
  return STATEMENT;
}


void Query_arena::free_items()
{
  Item *next;
  DBUG_ENTER("Query_arena::free_items");
  /* This works because items are allocated on THD::mem_root */
  for (; free_list; free_list= next)
  {
    next= free_list->next;
    DBUG_ASSERT(free_list != next);
    DBUG_PRINT("info", ("free item: %p", free_list));
    free_list->delete_self();
  }
  /* Postcondition: free_list is 0 */
  DBUG_VOID_RETURN;
}


void Query_arena::set_query_arena(Query_arena *set)
{
  mem_root=  set->mem_root;
  free_list= set->free_list;
  state= set->state;
}


bool Query_arena::cleanup_stmt(bool /*restore_set_statement_vars*/)
{
  DBUG_ASSERT(! "Query_arena::cleanup_stmt() not implemented");
  return false;
}

/*
  Statement functions
*/

Statement::Statement(LEX *lex_arg, MEM_ROOT *mem_root_arg,
                     enum enum_state state_arg, ulong id_arg)
  :Query_arena(mem_root_arg, state_arg),
  id(id_arg),
  column_usage(MARK_COLUMNS_READ),
  lex(lex_arg),
  db(null_clex_str)
{
  name= null_clex_str;
}


Query_arena::Type Statement::type() const
{
  return STATEMENT;
}


void Statement::set_statement(Statement *stmt)
{
  id=             stmt->id;
  column_usage=   stmt->column_usage;
  lex=            stmt->lex;
  query_string=   stmt->query_string;
}


void
Statement::set_n_backup_statement(Statement *stmt, Statement *backup)
{
  DBUG_ENTER("Statement::set_n_backup_statement");
  backup->set_statement(this);
  set_statement(stmt);
  DBUG_VOID_RETURN;
}


void Statement::restore_backup_statement(Statement *stmt, Statement *backup)
{
  DBUG_ENTER("Statement::restore_backup_statement");
  stmt->set_statement(this);
  set_statement(backup);
  DBUG_VOID_RETURN;
}


void THD::end_statement()
{
  DBUG_ENTER("THD::end_statement");
  /* Cleanup SQL processing state to reuse this statement in next query. */
  lex_end(lex);
  delete lex->result;
  lex->result= 0;
  /* Note that free_list is freed in cleanup_after_query() */

  /*
    Don't free mem_root, as mem_root is freed in the end of dispatch_command
    (once for any command).
  */
  DBUG_VOID_RETURN;
}


/*
  Start using arena specified by @set. Current arena data will be saved to
  *backup.
*/
void THD::set_n_backup_active_arena(Query_arena *set, Query_arena *backup)
{
  DBUG_ENTER("THD::set_n_backup_active_arena");
  DBUG_ASSERT(backup->is_backup_arena == FALSE);

  backup->set_query_arena(this);
  set_query_arena(set);
#ifdef DBUG_ASSERT_EXISTS
  backup->is_backup_arena= TRUE;
#endif
  DBUG_VOID_RETURN;
}


/*
  Stop using the temporary arena, and start again using the arena that is 
  specified in *backup.
  The temporary arena is returned back into *set.
*/

void THD::restore_active_arena(Query_arena *set, Query_arena *backup)
{
  DBUG_ENTER("THD::restore_active_arena");
  DBUG_ASSERT(backup->is_backup_arena);
  set->set_query_arena(this);
  set_query_arena(backup);
#ifdef DBUG_ASSERT_EXISTS
  backup->is_backup_arena= FALSE;
#endif
  DBUG_VOID_RETURN;
}

Statement::~Statement()
{
}

C_MODE_START

static uchar *
get_statement_id_as_hash_key(const uchar *record, size_t *key_length,
                             my_bool not_used __attribute__((unused)))
{
  const Statement *statement= (const Statement *) record; 
  *key_length= sizeof(statement->id);
  return (uchar *) &((const Statement *) statement)->id;
}

static void delete_statement_as_hash_key(void *key)
{
  delete (Statement *) key;
}

static uchar *get_stmt_name_hash_key(Statement *entry, size_t *length,
                                    my_bool not_used __attribute__((unused)))
{
  *length= entry->name.length;
  return (uchar*) entry->name.str;
}

C_MODE_END

Statement_map::Statement_map() :
  last_found_statement(0)
{
  enum
  {
    START_STMT_HASH_SIZE = 16,
    START_NAME_HASH_SIZE = 16
  };
  my_hash_init(key_memory_prepared_statement_map, &st_hash, &my_charset_bin,
               START_STMT_HASH_SIZE, 0, 0, get_statement_id_as_hash_key,
               delete_statement_as_hash_key, MYF(0));
  my_hash_init(key_memory_prepared_statement_map, &names_hash, system_charset_info, START_NAME_HASH_SIZE, 0, 0,
               (my_hash_get_key) get_stmt_name_hash_key,
               NULL, MYF(0));
}


/*
  Insert a new statement to the thread-local statement map.

  DESCRIPTION
    If there was an old statement with the same name, replace it with the
    new one. Otherwise, check if max_prepared_stmt_count is not reached yet,
    increase prepared_stmt_count, and insert the new statement. It's okay
    to delete an old statement and fail to insert the new one.

  POSTCONDITIONS
    All named prepared statements are also present in names_hash.
    Statement names in names_hash are unique.
    The statement is added only if prepared_stmt_count < max_prepard_stmt_count
    last_found_statement always points to a valid statement or is 0

  RETURN VALUE
    0  success
    1  error: out of resources or max_prepared_stmt_count limit has been
       reached. An error is sent to the client, the statement is deleted.
*/

int Statement_map::insert(THD *thd, Statement *statement)
{
  if (my_hash_insert(&st_hash, (uchar*) statement))
  {
    /*
      Delete is needed only in case of an insert failure. In all other
      cases hash_delete will also delete the statement.
    */
    delete statement;
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    goto err_st_hash;
  }
  if (statement->name.str && my_hash_insert(&names_hash, (uchar*) statement))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    goto err_names_hash;
  }
  mysql_mutex_lock(&LOCK_prepared_stmt_count);
  /*
    We don't check that prepared_stmt_count is <= max_prepared_stmt_count
    because we would like to allow to lower the total limit
    of prepared statements below the current count. In that case
    no new statements can be added until prepared_stmt_count drops below
    the limit.
  */
  if (prepared_stmt_count >= max_prepared_stmt_count)
  {
    mysql_mutex_unlock(&LOCK_prepared_stmt_count);
    my_error(ER_MAX_PREPARED_STMT_COUNT_REACHED, MYF(0),
             max_prepared_stmt_count);
    goto err_max;
  }
  prepared_stmt_count++;
  mysql_mutex_unlock(&LOCK_prepared_stmt_count);

  last_found_statement= statement;
  return 0;

err_max:
  if (statement->name.str)
    my_hash_delete(&names_hash, (uchar*) statement);
err_names_hash:
  my_hash_delete(&st_hash, (uchar*) statement);
err_st_hash:
  return 1;
}


void Statement_map::close_transient_cursors()
{
#ifdef TO_BE_IMPLEMENTED
  Statement *stmt;
  while ((stmt= transient_cursor_list.head()))
    stmt->close_cursor();                 /* deletes itself from the list */
#endif
}


void Statement_map::erase(Statement *statement)
{
  if (statement == last_found_statement)
    last_found_statement= 0;
  if (statement->name.str)
    my_hash_delete(&names_hash, (uchar *) statement);

  my_hash_delete(&st_hash, (uchar *) statement);
  mysql_mutex_lock(&LOCK_prepared_stmt_count);
  DBUG_ASSERT(prepared_stmt_count > 0);
  prepared_stmt_count--;
  mysql_mutex_unlock(&LOCK_prepared_stmt_count);
}


void Statement_map::reset()
{
  /* Must be first, hash_free will reset st_hash.records */
  if (st_hash.records)
  {
    mysql_mutex_lock(&LOCK_prepared_stmt_count);
    DBUG_ASSERT(prepared_stmt_count >= st_hash.records);
    prepared_stmt_count-= st_hash.records;
    mysql_mutex_unlock(&LOCK_prepared_stmt_count);
  }
  my_hash_reset(&names_hash);
  my_hash_reset(&st_hash);
  last_found_statement= 0;
}


Statement_map::~Statement_map()
{
  /* Statement_map::reset() should be called prior to destructor. */
  DBUG_ASSERT(!st_hash.records);
  my_hash_free(&names_hash);
  my_hash_free(&st_hash);
}

bool my_var_user::set(THD *thd, Item *item)
{
  Item_func_set_user_var *suv= new (thd->mem_root) Item_func_set_user_var(thd, &name, item);
  suv->save_item_result(item);
  return suv->fix_fields(thd, 0) || suv->update();
}


sp_rcontext *my_var_sp::get_rcontext(sp_rcontext *local_ctx) const
{
  return m_rcontext_handler->get_rcontext(local_ctx);
}


bool my_var_sp::set(THD *thd, Item *item)
{
  return get_rcontext(thd->spcont)->set_variable(thd, offset, &item);
}

bool my_var_sp_row_field::set(THD *thd, Item *item)
{
  return get_rcontext(thd->spcont)->
           set_variable_row_field(thd, offset, m_field_offset, &item);
}


bool select_dumpvar::send_data_to_var_list(List<Item> &items)
{
  DBUG_ENTER("select_dumpvar::send_data_to_var_list");
  List_iterator_fast<my_var> var_li(var_list);
  List_iterator<Item> it(items);
  Item *item;
  my_var *mv;
  while ((mv= var_li++) && (item= it++))
  {
    if (mv->set(thd, item))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


int select_dumpvar::send_data(List<Item> &items)
{
  DBUG_ENTER("select_dumpvar::send_data");

  if (row_count++)
  {
    my_message(ER_TOO_MANY_ROWS, ER_THD(thd, ER_TOO_MANY_ROWS), MYF(0));
    DBUG_RETURN(1);
  }
  if (m_var_sp_row ?
      m_var_sp_row->get_rcontext(thd->spcont)->
        set_variable_row(thd, m_var_sp_row->offset, items) :
      send_data_to_var_list(items))
    DBUG_RETURN(1);

  DBUG_RETURN(thd->is_error());
}

bool select_dumpvar::send_eof()
{
  if (! row_count)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_SP_FETCH_NO_DATA, ER_THD(thd, ER_SP_FETCH_NO_DATA));
  /*
    Don't send EOF if we're in error condition (which implies we've already
    sent or are sending an error)
  */
  if (unlikely(thd->is_error()))
    return true;

  if (!suppress_my_ok)
    ::my_ok(thd,row_count);

  return 0;
}



bool
select_materialize_with_stats::
create_result_table(THD *thd_arg, List<Item> *column_types,
                    bool is_union_distinct, ulonglong options,
                    const LEX_CSTRING *table_alias, bool bit_fields_as_long,
                    bool create_table,
                    bool keep_row_order,
                    uint hidden)
{
  DBUG_ASSERT(table == 0);
  tmp_table_param.field_count= column_types->elements;
  tmp_table_param.bit_fields_as_long= bit_fields_as_long;

  if (! (table= create_tmp_table(thd_arg, &tmp_table_param, *column_types,
                                 (ORDER*) 0, is_union_distinct, 1,
                                 options, HA_POS_ERROR, table_alias,
                                 !create_table, keep_row_order)))
    return TRUE;

  col_stat= (Column_statistics*) table->in_use->alloc(table->s->fields *
                                                      sizeof(Column_statistics));
  if (!col_stat)
    return TRUE;

  reset();
  table->file->extra(HA_EXTRA_WRITE_CACHE);
  table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  return FALSE;
}


void select_materialize_with_stats::reset()
{
  memset(col_stat, 0, table->s->fields * sizeof(Column_statistics));
  max_nulls_in_row= 0;
  count_rows= 0;
}


void select_materialize_with_stats::cleanup()
{
  reset();
  select_unit::cleanup();
}


/**
  Override select_unit::send_data to analyze each row for NULLs and to
  update null_statistics before sending data to the client.

  @return TRUE if fatal error when sending data to the client
  @return FALSE on success
*/

int select_materialize_with_stats::send_data(List<Item> &items)
{
  List_iterator_fast<Item> item_it(items);
  Item *cur_item;
  Column_statistics *cur_col_stat= col_stat;
  uint nulls_in_row= 0;
  int res;

  if ((res= select_unit::send_data(items)))
    return res;
  if (table->null_catch_flags & REJECT_ROW_DUE_TO_NULL_FIELDS)
  {
    table->null_catch_flags&= ~REJECT_ROW_DUE_TO_NULL_FIELDS;
    return 0;
  }
  /* Skip duplicate rows. */
  if (write_err == HA_ERR_FOUND_DUPP_KEY ||
      write_err == HA_ERR_FOUND_DUPP_UNIQUE)
    return 0;

  ++count_rows;

  while ((cur_item= item_it++))
  {
    if (cur_item->is_null_result())
    {
      ++cur_col_stat->null_count;
      cur_col_stat->max_null_row= count_rows;
      if (!cur_col_stat->min_null_row)
        cur_col_stat->min_null_row= count_rows;
      ++nulls_in_row;
    }
    ++cur_col_stat;
  }
  if (nulls_in_row > max_nulls_in_row)
    max_nulls_in_row= nulls_in_row;

  return 0;
}


/****************************************************************************
  TMP_TABLE_PARAM
****************************************************************************/

void TMP_TABLE_PARAM::init()
{
  DBUG_ENTER("TMP_TABLE_PARAM::init");
  DBUG_PRINT("enter", ("this: %p", this));
  field_count= sum_func_count= func_count= hidden_field_count= 0;
  group_parts= group_length= group_null_parts= 0;
  quick_group= 1;
  table_charset= 0;
  precomputed_group_by= 0;
  bit_fields_as_long= 0;
  materialized_subquery= 0;
  force_not_null_cols= 0;
  skip_create_table= 0;
  tmp_name= "temptable";                        // Name of temp table on disk
  DBUG_VOID_RETURN;
}


void thd_increment_bytes_sent(void *thd, size_t length)
{
  /* thd == 0 when close_connection() calls net_send_error() */
  if (likely(thd != 0))
  {
    ((THD*) thd)->status_var.bytes_sent+= length;
  }
}

my_bool thd_net_is_killed(THD *thd)
{
  return thd && thd->killed ? 1 : 0;
}


void thd_increment_bytes_received(void *thd, size_t length)
{
  if (thd != NULL) // MDEV-13073 Ack collector having NULL
    ((THD*) thd)->status_var.bytes_received+= length;
}


void THD::set_status_var_init()
{
  bzero((char*) &status_var, offsetof(STATUS_VAR,
                                      last_cleared_system_status_var));
  /*
    Session status for Threads_running is always 1. It can only be queried
    by thread itself via INFORMATION_SCHEMA.SESSION_STATUS or SHOW [SESSION]
    STATUS. And at this point thread is guaranteed to be running.
  */
  status_var.threads_running= 1;
}


void Security_context::init()
{
  host= user= ip= external_user= 0;
  host_or_ip= "connecting host";
  priv_user[0]= priv_host[0]= proxy_user[0]= priv_role[0]= '\0';
  master_access= NO_ACL;
  password_expired= false;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  db_access= NO_ACL;
#endif
}


void Security_context::destroy()
{
  DBUG_PRINT("info", ("freeing security context"));
  // If not pointer to constant
  if (host != my_localhost)
  {
    my_free((char*) host);
    host= NULL;
  }
  if (user != delayed_user)
  {
    my_free((char*) user);
    user= NULL;
  }

  if (external_user)
  {
    my_free(external_user);
    external_user= NULL;
  }

  my_free((char*) ip);
  ip= NULL;
}


void Security_context::skip_grants()
{
  /* privileges for the user are unknown everything is allowed */
  host_or_ip= (char *)"";
  master_access= ALL_KNOWN_ACL;
  *priv_user= *priv_host= '\0';
  password_expired= false;
}


bool Security_context::set_user(char *user_arg)
{
  my_free(const_cast<char*>(user));
  user= my_strdup(key_memory_MPVIO_EXT_auth_info, user_arg, MYF(0));
  return user == 0;
}

bool Security_context::check_access(const privilege_t want_access,
                                    bool match_any)
{
  DBUG_ENTER("Security_context::check_access");
  DBUG_RETURN((match_any ? (master_access & want_access) != NO_ACL
                         : ((master_access & want_access) == want_access)));
}

#ifndef NO_EMBEDDED_ACCESS_CHECKS
/**
  Initialize this security context from the passed in credentials
  and activate it in the current thread.

  @param       thd
  @param       definer_user
  @param       definer_host
  @param       db
  @param[out]  backup  Save a pointer to the current security context
                       in the thread. In case of success it points to the
                       saved old context, otherwise it points to NULL.


  During execution of a statement, multiple security contexts may
  be needed:
  - the security context of the authenticated user, used as the
    default security context for all top-level statements
  - in case of a view or a stored program, possibly the security
    context of the definer of the routine, if the object is
    defined with SQL SECURITY DEFINER option.

  The currently "active" security context is parameterized in THD
  member security_ctx. By default, after a connection is
  established, this member points at the "main" security context
  - the credentials of the authenticated user.

  Later, if we would like to execute some sub-statement or a part
  of a statement under credentials of a different user, e.g.
  definer of a procedure, we authenticate this user in a local
  instance of Security_context by means of this method (and
  ultimately by means of acl_getroot), and make the
  local instance active in the thread by re-setting
  thd->security_ctx pointer.

  Note, that the life cycle and memory management of the "main" and
  temporary security contexts are different.
  For the main security context, the memory for user/host/ip is
  allocated on system heap, and the THD class frees this memory in
  its destructor. The only case when contents of the main security
  context may change during its life time is when someone issued
  CHANGE USER command.
  Memory management of a "temporary" security context is
  responsibility of the module that creates it.

  @retval TRUE  there is no user with the given credentials. The erro
                is reported in the thread.
  @retval FALSE success
*/

bool
Security_context::
change_security_context(THD *thd,
                        LEX_CSTRING *definer_user,
                        LEX_CSTRING *definer_host,
                        LEX_CSTRING *db,
                        Security_context **backup)
{
  bool needs_change;

  DBUG_ENTER("Security_context::change_security_context");

  DBUG_ASSERT(definer_user->str && definer_host->str);

  *backup= NULL;
  needs_change= (strcmp(definer_user->str, thd->security_ctx->priv_user) ||
                 my_strcasecmp(system_charset_info, definer_host->str,
                               thd->security_ctx->priv_host));
  if (needs_change)
  {
    if (acl_getroot(this, definer_user->str, definer_host->str,
                                definer_host->str, db->str))
    {
      my_error(ER_NO_SUCH_USER, MYF(0), definer_user->str,
               definer_host->str);
      DBUG_RETURN(TRUE);
    }
    *backup= thd->security_ctx;
    thd->security_ctx= this;
  }

  DBUG_RETURN(FALSE);
}


void
Security_context::restore_security_context(THD *thd,
                                           Security_context *backup)
{
  if (backup)
    thd->security_ctx= backup;
}
#endif


bool Security_context::user_matches(Security_context *them)
{
  return ((user != NULL) && (them->user != NULL) &&
          !strcmp(user, them->user));
}

bool Security_context::is_priv_user(const char *user, const char *host)
{
  return ((user != NULL) && (host != NULL) &&
          !strcmp(user, priv_user) &&
          !my_strcasecmp(system_charset_info, host,priv_host));
}


/****************************************************************************
  Handling of open and locked tables states.

  This is used when we want to open/lock (and then close) some tables when
  we already have a set of tables open and locked. We use these methods for
  access to mysql.proc table to find definitions of stored routines.
****************************************************************************/

void THD::reset_n_backup_open_tables_state(Open_tables_backup *backup)
{
  DBUG_ENTER("reset_n_backup_open_tables_state");
  backup->set_open_tables_state(this);
  backup->mdl_system_tables_svp= mdl_context.mdl_savepoint();
  reset_open_tables_state();
  state_flags|= Open_tables_state::BACKUPS_AVAIL;
  DBUG_VOID_RETURN;
}


void THD::restore_backup_open_tables_state(Open_tables_backup *backup)
{
  DBUG_ENTER("restore_backup_open_tables_state");
  mdl_context.rollback_to_savepoint(backup->mdl_system_tables_svp);
  /*
    Before we will throw away current open tables state we want
    to be sure that it was properly cleaned up.
  */
  DBUG_ASSERT(open_tables == 0 &&
              temporary_tables == 0 &&
              derived_tables == 0 &&
              lock == 0 &&
              locked_tables_mode == LTM_NONE &&
              m_reprepare_observer == NULL);

  set_open_tables_state(backup);
  DBUG_VOID_RETURN;
}

#if MARIA_PLUGIN_INTERFACE_VERSION < 0x0200
/**
  This is a backward compatibility method, made obsolete
  by the thd_kill_statement service. Keep it here to avoid breaking the
  ABI in case some binary plugins still use it.
*/
#undef thd_killed
extern "C" int thd_killed(const MYSQL_THD thd)
{
  return thd_kill_level(thd) > THD_ABORT_SOFTLY;
}
#else
#error now thd_killed() function can go away
#endif

/*
  return thd->killed status to the client,
  mapped to the API enum thd_kill_levels values.

  @note Since this function is called quite frequently thd_kill_level(NULL) is
  forbidden for performance reasons (saves one conditional branch). If your ever
  need to call thd_kill_level() when THD is not available, you options are (most
  to least preferred):
  - try to pass THD through to thd_kill_level()
  - add current_thd to some service and use thd_killed(current_thd)
  - add thd_killed_current() function to kill statement service
  - add if (!thd) thd= current_thd here
*/
extern "C" enum thd_kill_levels thd_kill_level(const MYSQL_THD thd)
{
  DBUG_ASSERT(thd);

  if (likely(thd->killed == NOT_KILLED))
  {
    Apc_target *apc_target= (Apc_target*) &thd->apc_target;
    if (unlikely(apc_target->have_apc_requests()))
    {
      if (thd == current_thd)
        apc_target->process_apc_requests();
    }
    return THD_IS_NOT_KILLED;
  }

  return thd->killed & KILL_HARD_BIT ? THD_ABORT_ASAP : THD_ABORT_SOFTLY;
}


/**
   Send an out-of-band progress report to the client

   The report is sent every 'thd->...progress_report_time' second,
   however not more often than global.progress_report_time.
   If global.progress_report_time is 0, then don't send progress reports, but
   check every second if the value has changed

  We clear any errors that we get from sending the progress packet to
  the client as we don't want to set an error without the caller knowing
  about it.
*/

static void thd_send_progress(THD *thd)
{
  /* Check if we should send the client a progress report */
  ulonglong report_time= my_interval_timer();
  if (report_time > thd->progress.next_report_time)
  {
    uint seconds_to_next= MY_MAX(thd->variables.progress_report_time,
                              global_system_variables.progress_report_time);
    if (seconds_to_next == 0)             // Turned off
      seconds_to_next= 1;                 // Check again after 1 second

    thd->progress.next_report_time= (report_time +
                                     seconds_to_next * 1000000000ULL);
    if (global_system_variables.progress_report_time &&
        thd->variables.progress_report_time && !thd->is_error())
    {
      net_send_progress_packet(thd);
      if (thd->is_error())
        thd->clear_error();
    }
  }
}


/** Initialize progress report handling **/

extern "C" void thd_progress_init(MYSQL_THD thd, uint max_stage)
{
  DBUG_ASSERT(thd->stmt_arena != thd->progress.arena);
  if (thd->progress.arena)
    return; // already initialized
  /*
    Send progress reports to clients that supports it, if the command
    is a high level command (like ALTER TABLE) and we are not in a
    stored procedure
  */
  thd->progress.report= ((thd->client_capabilities & MARIADB_CLIENT_PROGRESS) &&
                         thd->progress.report_to_client &&
                         !thd->in_sub_stmt);
  thd->progress.next_report_time= 0;
  thd->progress.stage= 0;
  thd->progress.counter= thd->progress.max_counter= 0;
  thd->progress.max_stage= max_stage;
  thd->progress.arena= thd->stmt_arena;
}


/* Inform processlist and the client that some progress has been made */

extern "C" void thd_progress_report(MYSQL_THD thd,
                                    ulonglong progress, ulonglong max_progress)
{
  if (thd->stmt_arena != thd->progress.arena)
    return;
  if (thd->progress.max_counter != max_progress)        // Simple optimization
  {
    /*
      Better to not wait in the unlikely event that LOCK_thd_data is locked
      as Galera can potentially have this locked for a long time.
      Progress counters will fix themselves after the next call.
    */
    if (mysql_mutex_trylock(&thd->LOCK_thd_data))
      return;
    thd->progress.counter= progress;
    thd->progress.max_counter= max_progress;
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  else
    thd->progress.counter= progress;

  if (thd->progress.report)
    thd_send_progress(thd);
}

/**
  Move to next stage in process list handling

  This will reset the timer to ensure the progress is sent to the client
  if client progress reports are activated.
*/

extern "C" void thd_progress_next_stage(MYSQL_THD thd)
{
  if (thd->stmt_arena != thd->progress.arena)
    return;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  thd->progress.stage++;
  thd->progress.counter= 0;
  DBUG_ASSERT(thd->progress.stage < thd->progress.max_stage);
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  if (thd->progress.report)
  {
    thd->progress.next_report_time= 0;          // Send new stage info
    thd_send_progress(thd);
  }
}

/**
  Disable reporting of progress in process list.

  @note
  This function is safe to call even if one has not called thd_progress_init.

  This function should be called by all parts that does progress
  reporting to ensure that progress list doesn't contain 100 % done
  forever.
*/


extern "C" void thd_progress_end(MYSQL_THD thd)
{
  if (thd->stmt_arena != thd->progress.arena)
    return;
  /*
    It's enough to reset max_counter to set disable progress indicator
    in processlist.
  */
  thd->progress.max_counter= 0;
  thd->progress.arena= 0;
}


/**
  Return the thread id of a user thread
  @param thd user thread
  @return thread id
*/
extern "C" unsigned long thd_get_thread_id(const MYSQL_THD thd)
{
  return((unsigned long)thd->thread_id);
}

/**
  Check if THD socket is still connected.
 */
extern "C" int thd_is_connected(MYSQL_THD thd)
{
  return thd->is_connected();
}


extern "C" double thd_rnd(MYSQL_THD thd)
{
  return my_rnd(&thd->rand);
}


/**
  Generate string of printable random characters of requested length.

  @param to[out]      Buffer for generation; must be at least length+1 bytes
                      long; result string is always null-terminated
  @param length[in]   How many random characters to put in buffer
*/
extern "C" void thd_create_random_password(MYSQL_THD thd,
                                           char *to, size_t length)
{
  for (char *end= to + length; to < end; to++)
    *to= (char) (my_rnd(&thd->rand)*94 + 33);
  *to= '\0';
}


extern "C" const char *thd_priv_host(MYSQL_THD thd, size_t *length)
{
  const Security_context *sctx= thd->security_ctx;
  if (!sctx)
  {
    *length= 0;
    return NULL;
  }
  *length= strlen(sctx->priv_host);
  return sctx->priv_host;
}


extern "C" const char *thd_priv_user(MYSQL_THD thd, size_t *length)
{
  const Security_context *sctx= thd->security_ctx;
  if (!sctx)
  {
    *length= 0;
    return NULL;
  }
  *length= strlen(sctx->priv_user);
  return sctx->priv_user;
}


#ifdef INNODB_COMPATIBILITY_HOOKS

/** open a table and add it to thd->open_tables

  @note At the moment this is used in innodb background purge threads
  *only*.There should be no table locks, because the background purge does not
  change the table as far as LOCK TABLES is concerned. MDL locks are
  still needed, though.

  To make sure no table stays open for long, this helper allows the thread to
  have only one table open at any given time.
*/
TABLE *open_purge_table(THD *thd, const char *db, size_t dblen,
                        const char *tb, size_t tblen)
{
  DBUG_ENTER("open_purge_table");
  DBUG_ASSERT(thd->open_tables == NULL);
  DBUG_ASSERT(thd->locked_tables_mode < LTM_PRELOCKED);

  /* Purge already hold the MDL for the table */
  Open_table_context ot_ctx(thd, MYSQL_OPEN_HAS_MDL_LOCK);
  TABLE_LIST *tl= (TABLE_LIST*)thd->alloc(sizeof(TABLE_LIST));
  LEX_CSTRING db_name= {db, dblen };
  LEX_CSTRING table_name= { tb, tblen };

  tl->init_one_table(&db_name, &table_name, 0, TL_READ);
  tl->i_s_requested_object= OPEN_TABLE_ONLY;

  bool error= open_table(thd, tl, &ot_ctx);

  /* we don't recover here */
  DBUG_ASSERT(!error || !ot_ctx.can_recover_from_failed_open());

  if (unlikely(error))
    close_thread_tables(thd);

  DBUG_RETURN(error ? NULL : tl->table);
}

TABLE *get_purge_table(THD *thd)
{
  /* see above, at most one table can be opened */
  DBUG_ASSERT(thd->open_tables == NULL || thd->open_tables->next == NULL);
  return thd->open_tables;
}

/** Find an open table in the list of prelocked tabled

  Used for foreign key actions, for example, in UPDATE t1 SET a=1;
  where a child table t2 has a KB on t1.a.

  But only when virtual columns are involved, otherwise InnoDB
  does not need an open TABLE.
*/
TABLE *find_fk_open_table(THD *thd, const char *db, size_t db_len,
                       const char *table, size_t table_len)
{
  for (TABLE *t= thd->open_tables; t; t= t->next)
  {
    if (t->s->db.length == db_len && t->s->table_name.length == table_len &&
        !strcmp(t->s->db.str, db) && !strcmp(t->s->table_name.str, table) &&
        t->pos_in_table_list->prelocking_placeholder == TABLE_LIST::PRELOCK_FK)
      return t;
  }
  return NULL;
}

/* the following three functions are used in background purge threads */

MYSQL_THD create_thd()
{
  THD *thd= new THD(next_thread_id());
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  thd->set_command(COM_DAEMON);
  thd->system_thread= SYSTEM_THREAD_GENERIC;
  thd->security_ctx->host_or_ip="";
  server_threads.insert(thd);
  return thd;
}

void destroy_thd(MYSQL_THD thd)
{
  thd->add_status_to_global();
  server_threads.erase(thd);
  delete thd;
}

/**
  Create a THD that only has auxilliary functions
  It will never be added to the global connection list
  server_threads. It does not represent any client connection.

  It should never be counted, because it will stall the
  shutdown. It is solely for engine's internal use,
  like for example, evaluation of virtual function in innodb
  purge.
*/
extern "C" pthread_key(struct st_my_thread_var *, THR_KEY_mysys);
MYSQL_THD create_background_thd()
{
  auto save_thd = current_thd;
  set_current_thd(nullptr);

  auto save_mysysvar= pthread_getspecific(THR_KEY_mysys);

  /*
    Allocate new mysys_var specifically new THD,
    so that e.g safemalloc, DBUG etc are happy.
  */
  pthread_setspecific(THR_KEY_mysys, 0);
  my_thread_init();
  auto thd_mysysvar= pthread_getspecific(THR_KEY_mysys);
  auto thd= new THD(0);
  pthread_setspecific(THR_KEY_mysys, save_mysysvar);
  thd->set_psi(nullptr);
  set_current_thd(save_thd);

  /*
    Workaround the adverse effect of incrementing thread_count
    in THD constructor. We do not want these THDs to be counted,
    or waited for on shutdown.
  */
  THD_count::count--;

  thd->mysys_var= (st_my_thread_var *) thd_mysysvar;
  thd->set_command(COM_DAEMON);
  thd->system_thread= SYSTEM_THREAD_GENERIC;
  thd->security_ctx->host_or_ip= "";
  thd->real_id= 0;
  thd->thread_id= 0;
  thd->query_id= 0;
  return thd;
}


/*
  Attach a background THD.

  Changes current value THR_KEY_mysys TLS variable,
  and returns the original value.
*/
void *thd_attach_thd(MYSQL_THD thd)
{
  DBUG_ASSERT(!current_thd);
  DBUG_ASSERT(thd && thd->mysys_var);

  auto save_mysysvar= pthread_getspecific(THR_KEY_mysys);
  pthread_setspecific(THR_KEY_mysys, thd->mysys_var);
  thd->thread_stack= (char *) &thd;
  thd->store_globals();
  return save_mysysvar;
}

/*
  Restore THR_KEY_mysys TLS variable,
  which was changed thd_attach_thd().
*/
void thd_detach_thd(void *mysysvar)
{
  /* Restore mysys_var that is changed when THD was attached.*/
  pthread_setspecific(THR_KEY_mysys, mysysvar);
  /* Restore the THD (we assume it was NULL during attach).*/
  set_current_thd(0);
}

/*
  Destroy a THD that was previously created by
  create_background_thd()
*/
void destroy_background_thd(MYSQL_THD thd)
{
  DBUG_ASSERT(!current_thd);
  auto thd_mysys_var= thd->mysys_var;
  auto save_mysys_var= thd_attach_thd(thd);
  DBUG_ASSERT(thd_mysys_var != save_mysys_var);
  /*
    Workaround the adverse effect decrementing thread_count on THD()
    destructor.
    As we decremented it in create_background_thd(), in order for it
    not to go negative, we have to increment it before destructor.
  */
  THD_count::count++;
  delete thd;

  thd_detach_thd(save_mysys_var);
  /*
     Delete THD-specific my_thread_var, that was
     allocated in create_background_thd().
     Also preserve current PSI context, since my_thread_end()
     would kill it, if we're not careful.
  */
#ifdef HAVE_PSI_THREAD_INTERFACE
  auto save_psi_thread= PSI_CALL_get_thread();
#endif
  PSI_CALL_set_thread(0);
  pthread_setspecific(THR_KEY_mysys, thd_mysys_var);
  my_thread_end();
  pthread_setspecific(THR_KEY_mysys, save_mysys_var);
  PSI_CALL_set_thread(save_psi_thread);
}


void reset_thd(MYSQL_THD thd)
{
  close_thread_tables(thd);
  thd->release_transactional_locks();
  thd->free_items();
  free_root(thd->mem_root, MYF(MY_KEEP_PREALLOC));
}

/**
  This function can be used by storage engine
  to indicate a start of an async operation.

  This asynchronous is such operation needs to be
  finished before we write response to the client
.
  An example of this operation is Innodb's asynchronous
  group commit. Server needs to wait for the end of it
  before writing response to client, to provide durability
  guarantees, in other words, server can't send OK packet
  before modified data is durable in redo log.
*/
extern "C" void thd_increment_pending_ops(MYSQL_THD thd)
{
  thd->async_state.inc_pending_ops();
}

/**
  This function can be used by plugin/engine to indicate
  end of async operation (such as end of group commit
  write flush)

  @param thd THD
*/
extern "C" void thd_decrement_pending_ops(MYSQL_THD thd)
{
  DBUG_ASSERT(thd);
  thd_async_state::enum_async_state state;
  if (thd->async_state.dec_pending_ops(&state) == 0)
  {
    switch(state)
    {
    case thd_async_state::enum_async_state::SUSPENDED:
      DBUG_ASSERT(thd->scheduler->thd_resume);
      thd->scheduler->thd_resume(thd);
      break;
    case thd_async_state::enum_async_state::NONE:
      break;
    default:
      DBUG_ASSERT(0);
    }
  }
}


unsigned long long thd_get_query_id(const MYSQL_THD thd)
{
  return((unsigned long long)thd->query_id);
}

void thd_clear_error(MYSQL_THD thd)
{
  thd->clear_error();
}

extern "C" const struct charset_info_st *thd_charset(MYSQL_THD thd)
{
  return(thd->charset());
}


/**
  Get the current query string for the thread.

  This function is not thread safe and can be used only by thd owner thread.

  @param The MySQL internal thread pointer
  @return query string and length. May be non-null-terminated.
*/
extern "C" LEX_STRING * thd_query_string (MYSQL_THD thd)
{
  DBUG_ASSERT(thd == current_thd);
  return(&thd->query_string.string);
}


/**
  Get the current query string for the thread.

  @param thd     The MySQL internal thread pointer
  @param buf     Buffer where the query string will be copied
  @param buflen  Length of the buffer

  @return Length of the query
  @retval 0 if LOCK_thd_data cannot be acquired without waiting

  @note This function is thread safe as the query string is
        accessed under mutex protection and the string is copied
        into the provided buffer. @see thd_query_string().
*/

extern "C" size_t thd_query_safe(MYSQL_THD thd, char *buf, size_t buflen)
{
  size_t len= 0;
  /* InnoDB invokes this function while holding internal mutexes.
  THD::awake() will hold LOCK_thd_data while invoking an InnoDB
  function that would acquire the internal mutex. Because this
  function is a non-essential part of information_schema view output,
  we will break the deadlock by avoiding a mutex wait here
  and returning the empty string if a wait would be needed. */
  if (!mysql_mutex_trylock(&thd->LOCK_thd_data))
  {
    len= MY_MIN(buflen - 1, thd->query_length());
    if (len)
      memcpy(buf, thd->query(), len);
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  buf[len]= '\0';
  return len;
}


extern "C" const char *thd_user_name(MYSQL_THD thd)
{
  if (!thd->security_ctx)
    return 0;

  return thd->security_ctx->user;
}


extern "C" const char *thd_client_host(MYSQL_THD thd)
{
  if (!thd->security_ctx)
    return 0;

  return thd->security_ctx->host;
}


extern "C" const char *thd_client_ip(MYSQL_THD thd)
{
  if (!thd->security_ctx)
    return 0;

  return thd->security_ctx->ip;
}


extern "C" LEX_CSTRING *thd_current_db(MYSQL_THD thd)
{
  return &thd->db;
}


extern "C" int thd_current_status(MYSQL_THD thd)
{
  Diagnostics_area *da= thd->get_stmt_da();
  if (!da)
    return 0;

  return da->is_error() ? da->sql_errno() : 0;
}


extern "C" enum enum_server_command thd_current_command(MYSQL_THD thd)
{
  return thd->get_command();
}

#ifdef HAVE_REPLICATION /* Working around MDEV-24622 */
/** @return whether the current thread is for applying binlog in a replica */
extern "C" int thd_is_slave(const MYSQL_THD thd)
{
  return thd && thd->slave_thread;
}
#endif /* HAVE_REPLICATION */

/* Returns high resolution timestamp for the start
  of the current query. */
extern "C" unsigned long long thd_start_utime(const MYSQL_THD thd)
{
  return thd->start_time * 1000000 + thd->start_time_sec_part;
}


/*
  This function can optionally be called to check if thd_rpl_deadlock_check()
  needs to be called for waits done by a given transaction.

  If this function returns false for a given thd, there is no need to do
  any calls to thd_rpl_deadlock_check() on that thd.

  This call is optional; it is safe to call thd_rpl_deadlock_check() in
  any case. This call can be used to save some redundant calls to
  thd_rpl_deadlock_check() if desired. (This is unlikely to matter much
  unless there are _lots_ of waits to report, as the overhead of
  thd_rpl_deadlock_check() is small).
*/
extern "C" int
thd_need_wait_reports(const MYSQL_THD thd)
{
  rpl_group_info *rgi;

  if (mysql_bin_log.is_open())
    return true;
  if (!thd)
    return false;
  rgi= thd->rgi_slave;
  if (!rgi)
    return false;
  return rgi->is_parallel_exec;
}

/*
  Used by storage engines (currently InnoDB) to report that
  one transaction THD is about to go to wait for a transactional lock held by
  another transactions OTHER_THD.

  This is used for parallel replication, where transactions are required to
  commit in the same order on the slave as they did on the master. If the
  transactions on the slave encounter lock conflicts on the slave that did not
  exist on the master, this can cause deadlocks. This is primarily used in
  optimistic (and aggressive) modes.

  Normally, such conflicts will not occur in conservative mode, because the
  same conflict would have prevented the two transactions from committing in
  parallel on the master, thus preventing them from running in parallel on the
  slave in the first place. However, it is possible in case when the optimizer
  chooses a different plan on the slave than on the master (eg. table scan
  instead of index scan).

  Storage engines report lock waits using this call. If a lock wait causes a
  deadlock with the pre-determined commit order, we kill the later
  transaction, and later re-try it, to resolve the deadlock.

  This call need only receive reports about waits for locks that will remain
  until the holding transaction commits. InnoDB auto-increment locks,
  for example, are released earlier, and so need not be reported. (Such false
  positives are not harmful, but could lead to unnecessary kill and retry, so
  best avoided).

  Returns 1 if the OTHER_THD will be killed to resolve deadlock, 0 if not. The
  actual kill will happen later, asynchronously from another thread. The
  caller does not need to take any actions on the return value if the
  handlerton kill_query method is implemented to abort the to-be-killed
  transaction.
*/
extern "C" int
thd_rpl_deadlock_check(MYSQL_THD thd, MYSQL_THD other_thd)
{
  rpl_group_info *rgi;
  rpl_group_info *other_rgi;

  if (!thd)
    return 0;
  DEBUG_SYNC(thd, "thd_report_wait_for");
  thd->transaction->stmt.mark_trans_did_wait();
  if (!other_thd)
    return 0;
  binlog_report_wait_for(thd, other_thd);
  rgi= thd->rgi_slave;
  other_rgi= other_thd->rgi_slave;
  if (!rgi || !other_rgi)
    return 0;
  if (!rgi->is_parallel_exec)
    return 0;
  if (rgi->rli != other_rgi->rli)
    return 0;
  if (!rgi->gtid_sub_id || !other_rgi->gtid_sub_id)
    return 0;
  if (rgi->current_gtid.domain_id != other_rgi->current_gtid.domain_id)
    return 0;
  if (rgi->gtid_sub_id > other_rgi->gtid_sub_id)
    return 0;
  if (rgi->finish_event_group_called || other_rgi->finish_event_group_called)
  {
    /*
      If either of two transactions has already performed commit
      (e.g split ALTER, asserted below) there won't be any deadlock.
    */
    DBUG_ASSERT(rgi->sa_info || other_rgi->sa_info);

    return 0;
  }
  /*
    This transaction is about to wait for another transaction that is required
    by replication binlog order to commit after. This would cause a deadlock.

    So send a kill to the other transaction, with a temporary error; this will
    cause replication to rollback (and later re-try) the other transaction,
    releasing the lock for this transaction so replication can proceed.
  */
#ifdef HAVE_REPLICATION
  slave_background_kill_request(other_thd);
#endif
  return 1;
}

/*
  This function is called from InnoDB to check if the commit order of
  two transactions has already been decided by the upper layer. This happens
  in parallel replication, where the commit order is forced to be the same on
  the slave as it was originally on the master.

  If this function returns false, it means that such commit order will be
  enforced. This allows the storage engine to optionally omit gap lock waits
  or similar measures that would otherwise be needed to ensure that
  transactions would be serialised in a way that would cause a commit order
  that is correct for binlogging for statement-based replication.

  Since transactions are only run in parallel on the slave if they ran without
  lock conflicts on the master, normally no lock conflicts on the slave happen
  during parallel replication. However, there are a couple of corner cases
  where it can happen, like these secondary-index operations:

    T1: INSERT INTO t1 VALUES (7, NULL);
    T2: DELETE FROM t1 WHERE b <= 3;

    T1: UPDATE t1 SET secondary=NULL WHERE primary=1
    T2: DELETE t1 WHERE secondary <= 3

  The DELETE takes a gap lock that can block the INSERT/UPDATE, but the row
  locks set by INSERT/UPDATE do not block the DELETE. Thus, the execution
  order of the transactions determine whether a lock conflict occurs or
  not. Thus a lock conflict can occur on the slave where it did not on the
  master.

  If this function returns true, normal locking should be done as required by
  the binlogging and transaction isolation level in effect. But if it returns
  false, the correct order will be enforced anyway, and InnoDB can
  avoid taking the gap lock, preventing the lock conflict.

  Calling this function is just an optimisation to avoid unnecessary
  deadlocks. If it was not used, a gap lock would be set that could eventually
  cause a deadlock; the deadlock would be caught by thd_rpl_deadlock_check()
  and the transaction T2 killed and rolled back (and later re-tried).
*/
extern "C" int
thd_need_ordering_with(const MYSQL_THD thd, const MYSQL_THD other_thd)
{
  rpl_group_info *rgi, *other_rgi;

  DBUG_EXECUTE_IF("disable_thd_need_ordering_with", return 1;);
  if (!thd || !other_thd)
    return 1;
#ifdef WITH_WSREP
  /* wsrep applier, replayer and TOI processing threads are ordered
     by replication provider, relaxed GAP locking protocol can be used
     between high priority wsrep threads.
     Note that wsrep_thd_is_BF() doesn't take LOCK_thd_data for either thd,
     the caller should guarantee that the BF state won't change.
     (e.g. InnoDB does it by keeping lock_sys.mutex locked)
  */
  if (WSREP_ON && wsrep_thd_is_BF(thd, false) &&
      wsrep_thd_is_BF(other_thd, false))
    return 0;
#endif /* WITH_WSREP */
  rgi= thd->rgi_slave;
  other_rgi= other_thd->rgi_slave;
  if (!rgi || !other_rgi)
    return 1;
  if (!rgi->is_parallel_exec)
    return 1;
  if (rgi->rli != other_rgi->rli)
    return 1;
  if (rgi->current_gtid.domain_id != other_rgi->current_gtid.domain_id)
    return 1;
  if (!rgi->commit_id || rgi->commit_id != other_rgi->commit_id)
    return 1;
  DBUG_EXECUTE_IF("thd_need_ordering_with_force", return 1;);
  /*
    Otherwise, these two threads are doing parallel replication within the same
    replication domain. Their commit order is already fixed, so we do not need
    gap locks or similar to otherwise enforce ordering (and in fact such locks
    could lead to unnecessary deadlocks and transaction retry).
  */
  return 0;
}

extern "C" int thd_non_transactional_update(const MYSQL_THD thd)
{
  return(thd->transaction->all.modified_non_trans_table);
}

extern "C" int thd_binlog_format(const MYSQL_THD thd)
{
  if (WSREP(thd))
  {
    /* for wsrep binlog format is meaningful also when binlogging is off */
    return (int) WSREP_BINLOG_FORMAT(thd->variables.binlog_format);
  }

  if (mysql_bin_log.is_open() && (thd->variables.option_bits & OPTION_BIN_LOG))
    return (int) thd->variables.binlog_format;
  return BINLOG_FORMAT_UNSPEC;
}

extern "C" void thd_mark_transaction_to_rollback(MYSQL_THD thd, bool all)
{
  DBUG_ASSERT(thd);
  thd->mark_transaction_to_rollback(all);
}

extern "C" bool thd_binlog_filter_ok(const MYSQL_THD thd)
{
  return binlog_filter->db_ok(thd->db.str);
}

/*
  This is similar to sqlcom_can_generate_row_events, with the expection
  that we only return 1 if we are going to generate row events in a
  transaction.
  CREATE OR REPLACE is always safe to do as this will run in it's own
  transaction.
*/

extern "C" bool thd_sqlcom_can_generate_row_events(const MYSQL_THD thd)
{
  return (sqlcom_can_generate_row_events(thd) && thd->lex->sql_command !=
          SQLCOM_CREATE_TABLE);
}


extern "C" enum durability_properties thd_get_durability_property(const MYSQL_THD thd)
{
  enum durability_properties ret= HA_REGULAR_DURABILITY;
  
  if (thd != NULL)
    ret= thd->durability_property;

  return ret;
}

/** Get the auto_increment_offset auto_increment_increment.
Exposed by thd_autoinc_service.
Needed by InnoDB.
@param thd	Thread object
@param off	auto_increment_offset
@param inc	auto_increment_increment */
extern "C" void thd_get_autoinc(const MYSQL_THD thd, ulong* off, ulong* inc)
{
  *off = thd->variables.auto_increment_offset;
  *inc = thd->variables.auto_increment_increment;
}


/**
  Is strict sql_mode set.
  Needed by InnoDB.
  @param thd	Thread object
  @return True if sql_mode has strict mode (all or trans).
    @retval true  sql_mode has strict mode (all or trans).
    @retval false sql_mode has not strict mode (all or trans).
*/
extern "C" bool thd_is_strict_mode(const MYSQL_THD thd)
{
  return thd->is_strict_mode();
}


/**
  Get query start time as SQL field data.
  Needed by InnoDB.
  @param thd	Thread object
  @param buf	Buffer to hold start time data
*/
void thd_get_query_start_data(THD *thd, char *buf)
{
  Field_timestampf f((uchar *)buf, nullptr, 0, Field::NONE, &empty_clex_str,
                     nullptr, 6);
  f.store_TIME(thd->query_start(), thd->query_start_sec_part());
}


/*
  Interface for MySQL Server, plugins and storage engines to report
  when they are going to sleep/stall.
  
  SYNOPSIS
  thd_wait_begin()
  thd                     Thread object
                          Can be NULL, in this case current THD is used.
  wait_type               Type of wait
                          1 -- short wait (e.g. for mutex)
                          2 -- medium wait (e.g. for disk io)
                          3 -- large wait (e.g. for locked row/table)
  NOTES
    This is used by the threadpool to have better knowledge of which
    threads that currently are actively running on CPUs. When a thread
    reports that it's going to sleep/stall, the threadpool scheduler is
    free to start another thread in the pool most likely. The expected wait
    time is simply an indication of how long the wait is expected to
    become, the real wait time could be very different.

  thd_wait_end MUST be called immediately after waking up again.
*/
extern "C" void thd_wait_begin(MYSQL_THD thd, int wait_type)
{
  if (!thd)
  {
    thd= current_thd;
    if (unlikely(!thd))
      return;
  }
  MYSQL_CALLBACK(thd->scheduler, thd_wait_begin, (thd, wait_type));
}

/**
  Interface for MySQL Server, plugins and storage engines to report
  when they waking up from a sleep/stall.

  @param  thd   Thread handle
  Can be NULL, in this case current THD is used.
*/
extern "C" void thd_wait_end(MYSQL_THD thd)
{
  if (!thd)
  {
    thd= current_thd;
    if (unlikely(!thd))
      return;
  }
  MYSQL_CALLBACK(thd->scheduler, thd_wait_end, (thd));
}

#endif // INNODB_COMPATIBILITY_HOOKS */


/**
  MDL_context accessor
  @param thd   the current session
  @return pointer to thd->mdl_context
*/
extern "C" void *thd_mdl_context(MYSQL_THD thd)
{
  return &thd->mdl_context;
}


/****************************************************************************
  Handling of statement states in functions and triggers.

  This is used to ensure that the function/trigger gets a clean state
  to work with and does not cause any side effects of the calling statement.

  It also allows most stored functions and triggers to replicate even
  if they are used items that would normally be stored in the binary
  replication (like last_insert_id() etc...)

  The following things is done
  - Disable binary logging for the duration of the statement
  - Disable multi-result-sets for the duration of the statement
  - Value of last_insert_id() is saved and restored
  - Value set by 'SET INSERT_ID=#' is reset and restored
  - Value for found_rows() is reset and restored
  - examined_row_count is added to the total
  - cuted_fields is added to the total
  - new savepoint level is created and destroyed

  NOTES:
    Seed for random() is saved for the first! usage of RAND()
    We reset examined_row_count and cuted_fields and add these to the
    result to ensure that if we have a bug that would reset these within
    a function, we are not loosing any rows from the main statement.

    We do not reset value of last_insert_id().
****************************************************************************/

void THD::reset_sub_statement_state(Sub_statement_state *backup,
                                    uint new_state)
{
#ifndef EMBEDDED_LIBRARY
  /* BUG#33029, if we are replicating from a buggy master, reset
     auto_inc_intervals_forced to prevent substatement
     (triggers/functions) from using erroneous INSERT_ID value
   */
  if (rpl_master_erroneous_autoinc(this))
  {
    DBUG_ASSERT(backup->auto_inc_intervals_forced.nb_elements() == 0);
    auto_inc_intervals_forced.swap(&backup->auto_inc_intervals_forced);
  }
#endif
  
  backup->option_bits=     variables.option_bits;
  backup->count_cuted_fields= count_cuted_fields;
  backup->in_sub_stmt=     in_sub_stmt;
  backup->enable_slow_log= enable_slow_log;
  backup->limit_found_rows= limit_found_rows;
  backup->cuted_fields=     cuted_fields;
  backup->client_capabilities= client_capabilities;
  backup->savepoints= transaction->savepoints;
  backup->first_successful_insert_id_in_prev_stmt= 
    first_successful_insert_id_in_prev_stmt;
  backup->first_successful_insert_id_in_cur_stmt= 
    first_successful_insert_id_in_cur_stmt;
  store_slow_query_state(backup);

  if ((!lex->requires_prelocking() || is_update_query(lex->sql_command)) &&
      !is_current_stmt_binlog_format_row())
  {
    variables.option_bits&= ~OPTION_BIN_LOG;
  }

  if ((backup->option_bits & OPTION_BIN_LOG) &&
       is_update_query(lex->sql_command) &&
       !is_current_stmt_binlog_format_row())
    mysql_bin_log.start_union_events(this, this->query_id);

  /* Disable result sets */
  client_capabilities &= ~CLIENT_MULTI_RESULTS;
  in_sub_stmt|= new_state;
  cuted_fields= 0;
  transaction->savepoints= 0;
  first_successful_insert_id_in_cur_stmt= 0;
  reset_slow_query_state();
}

void THD::restore_sub_statement_state(Sub_statement_state *backup)
{
  DBUG_ENTER("THD::restore_sub_statement_state");
#ifndef EMBEDDED_LIBRARY
  /* BUG#33029, if we are replicating from a buggy master, restore
     auto_inc_intervals_forced so that the top statement can use the
     INSERT_ID value set before this statement.
   */
  if (rpl_master_erroneous_autoinc(this))
  {
    backup->auto_inc_intervals_forced.swap(&auto_inc_intervals_forced);
    DBUG_ASSERT(backup->auto_inc_intervals_forced.nb_elements() == 0);
  }
#endif

  /*
    To save resources we want to release savepoints which were created
    during execution of function or trigger before leaving their savepoint
    level. It is enough to release first savepoint set on this level since
    all later savepoints will be released automatically.
  */
  if (transaction->savepoints)
  {
    SAVEPOINT *sv;
    for (sv= transaction->savepoints; sv->prev; sv= sv->prev)
    {}
    /* ha_release_savepoint() never returns error. */
    (void)ha_release_savepoint(this, sv);
  }
  count_cuted_fields= backup->count_cuted_fields;
  transaction->savepoints= backup->savepoints;
  variables.option_bits= backup->option_bits;
  in_sub_stmt=      backup->in_sub_stmt;
  enable_slow_log=  backup->enable_slow_log;
  first_successful_insert_id_in_prev_stmt= 
    backup->first_successful_insert_id_in_prev_stmt;
  first_successful_insert_id_in_cur_stmt= 
    backup->first_successful_insert_id_in_cur_stmt;
  limit_found_rows= backup->limit_found_rows;
  set_sent_row_count(backup->sent_row_count);
  client_capabilities= backup->client_capabilities;

  /* Restore statistic needed for slow log */
  add_slow_query_state(backup);

  /*
    If we've left sub-statement mode, reset the fatal error flag.
    Otherwise keep the current value, to propagate it up the sub-statement
    stack.

    NOTE: is_fatal_sub_stmt_error can be set only if we've been in the
    sub-statement mode.
  */
  if (!in_sub_stmt)
    is_fatal_sub_stmt_error= false;

  if ((variables.option_bits & OPTION_BIN_LOG) && is_update_query(lex->sql_command) &&
       !is_current_stmt_binlog_format_row())
    mysql_bin_log.stop_union_events(this);

  /*
    The following is added to the old values as we are interested in the
    total complexity of the query
  */
  inc_examined_row_count(backup->examined_row_count);
  cuted_fields+=       backup->cuted_fields;
  DBUG_VOID_RETURN;
}

/*
  Store slow query state at start of a stored procedure statment
*/

void THD::store_slow_query_state(Sub_statement_state *backup)
{
  backup->affected_rows=           affected_rows;
  backup->bytes_sent_old=          bytes_sent_old;
  backup->examined_row_count=      m_examined_row_count;
  backup->query_plan_flags=        query_plan_flags;
  backup->query_plan_fsort_passes= query_plan_fsort_passes;
  backup->sent_row_count=          m_sent_row_count;
  backup->tmp_tables_disk_used=    tmp_tables_disk_used;
  backup->tmp_tables_size=         tmp_tables_size;
  backup->tmp_tables_used=         tmp_tables_used;
}

/* Reset variables related to slow query log */

void THD::reset_slow_query_state()
{
  affected_rows=                0;
  bytes_sent_old=               status_var.bytes_sent;
  m_examined_row_count=         0;
  m_sent_row_count=             0;
  query_plan_flags=             QPLAN_INIT;
  query_plan_fsort_passes=      0;
  tmp_tables_disk_used=         0;
  tmp_tables_size=              0;
  tmp_tables_used=              0;
}

/*
  Add back the stored values to the current counters to be able to get
  right status for 'call procedure_name'
*/

void THD::add_slow_query_state(Sub_statement_state *backup)
{
  affected_rows+=                backup->affected_rows;
  bytes_sent_old=                backup->bytes_sent_old;
  m_examined_row_count+=         backup->examined_row_count;
  m_sent_row_count+=             backup->sent_row_count;
  query_plan_flags|=             backup->query_plan_flags;
  query_plan_fsort_passes+=      backup->query_plan_fsort_passes;
  tmp_tables_disk_used+=         backup->tmp_tables_disk_used;
  tmp_tables_size+=              backup->tmp_tables_size;
  tmp_tables_used+=              backup->tmp_tables_used;
}


void THD::set_statement(Statement *stmt)
{
  mysql_mutex_lock(&LOCK_thd_data);
  Statement::set_statement(stmt);
  mysql_mutex_unlock(&LOCK_thd_data);
}

void THD::set_sent_row_count(ha_rows count)
{
  m_sent_row_count= count;
  MYSQL_SET_STATEMENT_ROWS_SENT(m_statement_psi, m_sent_row_count);
}

void THD::set_examined_row_count(ha_rows count)
{
  m_examined_row_count= count;
  MYSQL_SET_STATEMENT_ROWS_EXAMINED(m_statement_psi, m_examined_row_count);
}

void THD::inc_sent_row_count(ha_rows count)
{
  m_sent_row_count+= count;
  MYSQL_SET_STATEMENT_ROWS_SENT(m_statement_psi, m_sent_row_count);
}

void THD::inc_examined_row_count(ha_rows count)
{
  m_examined_row_count+= count;
  MYSQL_SET_STATEMENT_ROWS_EXAMINED(m_statement_psi, m_examined_row_count);
}

void THD::inc_status_created_tmp_disk_tables()
{
  tmp_tables_disk_used++;
  query_plan_flags|= QPLAN_TMP_DISK;
  status_var_increment(status_var.created_tmp_disk_tables_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_created_tmp_disk_tables)(m_statement_psi, 1);
#endif
}

void THD::inc_status_created_tmp_tables()
{
  tmp_tables_used++;
  query_plan_flags|= QPLAN_TMP_TABLE;
  status_var_increment(status_var.created_tmp_tables_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_created_tmp_tables)(m_statement_psi, 1);
#endif
}

void THD::inc_status_select_full_join()
{
  status_var_increment(status_var.select_full_join_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_select_full_join)(m_statement_psi, 1);
#endif
}

void THD::inc_status_select_full_range_join()
{
  status_var_increment(status_var.select_full_range_join_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_select_full_range_join)(m_statement_psi, 1);
#endif
}

void THD::inc_status_select_range()
{
  status_var_increment(status_var.select_range_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_select_range)(m_statement_psi, 1);
#endif
}

void THD::inc_status_select_range_check()
{
  status_var_increment(status_var.select_range_check_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_select_range_check)(m_statement_psi, 1);
#endif
}

void THD::inc_status_select_scan()
{
  status_var_increment(status_var.select_scan_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_select_scan)(m_statement_psi, 1);
#endif
}

void THD::inc_status_sort_merge_passes()
{
  status_var_increment(status_var.filesort_merge_passes_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_sort_merge_passes)(m_statement_psi, 1);
#endif
}

void THD::inc_status_sort_range()
{
  status_var_increment(status_var.filesort_range_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_sort_range)(m_statement_psi, 1);
#endif
}

void THD::inc_status_sort_rows(ha_rows count)
{
  statistic_add(status_var.filesort_rows_, (ulong)count, &LOCK_status);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_sort_rows)(m_statement_psi, (ulong)count);
#endif
}

void THD::inc_status_sort_scan()
{
  status_var_increment(status_var.filesort_scan_count_);
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(inc_statement_sort_scan)(m_statement_psi, 1);
#endif
}

void THD::set_status_no_index_used()
{
  server_status|= SERVER_QUERY_NO_INDEX_USED;
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(set_statement_no_index_used)(m_statement_psi);
#endif
}

void THD::set_status_no_good_index_used()
{
  server_status|= SERVER_QUERY_NO_GOOD_INDEX_USED;
#ifdef HAVE_PSI_STATEMENT_INTERFACE
  PSI_STATEMENT_CALL(set_statement_no_good_index_used)(m_statement_psi);
#endif
}

/** Assign a new value to thd->query and thd->query_id.  */

void THD::set_query_and_id(char *query_arg, uint32 query_length_arg,
                           CHARSET_INFO *cs,
                           query_id_t new_query_id)
{
  mysql_mutex_lock(&LOCK_thd_data);
  set_query_inner(query_arg, query_length_arg, cs);
  mysql_mutex_unlock(&LOCK_thd_data);
  query_id= new_query_id;
#ifdef WITH_WSREP
  set_wsrep_next_trx_id(query_id);
  WSREP_DEBUG("assigned new next query and  trx id: %" PRIu64, wsrep_next_trx_id());
#endif /* WITH_WSREP */
}

/** Assign a new value to thd->mysys_var.  */
void THD::set_mysys_var(struct st_my_thread_var *new_mysys_var)
{
  mysql_mutex_lock(&LOCK_thd_kill);
  mysys_var= new_mysys_var;
  mysql_mutex_unlock(&LOCK_thd_kill);
}

/**
  Leave explicit LOCK TABLES or prelocked mode and restore value of
  transaction sentinel in MDL subsystem.
*/

void THD::leave_locked_tables_mode()
{
  if (locked_tables_mode == LTM_LOCK_TABLES)
  {
    DBUG_ASSERT(current_backup_stage == BACKUP_FINISHED);
    /*
      When leaving LOCK TABLES mode we have to change the duration of most
      of the metadata locks being held, except for HANDLER and GRL locks,
      to transactional for them to be properly released at UNLOCK TABLES.
    */
    mdl_context.set_transaction_duration_for_all_locks();
    /*
      Make sure we don't release the global read lock and commit blocker
      when leaving LTM.
    */
    global_read_lock.set_explicit_lock_duration(this);
    /* Also ensure that we don't release metadata locks for open HANDLERs. */
    if (handler_tables_hash.records)
      mysql_ha_set_explicit_lock_duration(this);
    if (ull_hash.records)
      mysql_ull_set_explicit_lock_duration(this);
  }
  locked_tables_mode= LTM_NONE;
}

void THD::get_definer(LEX_USER *definer, bool role)
{
  binlog_invoker(role);
#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
#ifdef WITH_WSREP
  if ((wsrep_applier || slave_thread) && has_invoker())
#else
  if (slave_thread && has_invoker())
#endif
  {
    definer->user= invoker.user;
    definer->host= invoker.host;
    definer->auth= NULL;
  }
  else
#endif
    get_default_definer(this, definer, role);
}


/**
  Mark transaction to rollback and mark error as fatal to a sub-statement.

  @param  all   TRUE <=> rollback main transaction.
*/

void THD::mark_transaction_to_rollback(bool all)
{
  /*
    There is no point in setting is_fatal_sub_stmt_error unless
    we are actually in_sub_stmt.
  */
  if (in_sub_stmt)
    is_fatal_sub_stmt_error= true;
  transaction_rollback_request= all;
}


/**
  Commit the whole transaction (both statment and all)

  This is used mainly to commit an independent transaction,
  like reading system tables.

  @return  0  0k
  @return <>0 error code. my_error() has been called()
*/

int THD::commit_whole_transaction_and_close_tables()
{
  int error, error2;
  DBUG_ENTER("THD::commit_whole_transaction_and_close_tables");

  /*
    This can only happened if we failed to open any table in the
    new transaction
  */
  DBUG_ASSERT(open_tables);

  if (!open_tables)                             // Safety for production usage
    DBUG_RETURN(0);

  /*
    Ensure table was locked (opened with open_and_lock_tables()). If not
    the THD can't be part of any transactions and doesn't have to call
    this function.
  */
  DBUG_ASSERT(lock);

  error= ha_commit_trans(this, FALSE);
  /* This will call external_lock to unlock all tables */
  if ((error2= mysql_unlock_tables(this, lock)))
  {
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), error2);
    error= error2;
  }
  lock= 0;
  if ((error2= ha_commit_trans(this, TRUE)))
    error= error2;
  close_thread_tables(this);
  DBUG_RETURN(error);
}

/**
   Start a new independent transaction
*/

start_new_trans::start_new_trans(THD *thd)
{
  org_thd= thd;
  mdl_savepoint= thd->mdl_context.mdl_savepoint();
  memcpy(old_ha_data, thd->ha_data, sizeof(old_ha_data));
  thd->reset_n_backup_open_tables_state(&open_tables_state_backup);
  for (auto &data : thd->ha_data)
    data.reset();
  old_transaction= thd->transaction;
  thd->transaction= &new_transaction;
  new_transaction.on= 1;
  in_sub_stmt= thd->in_sub_stmt;
  thd->in_sub_stmt= 0;
  server_status= thd->server_status;
  m_transaction_psi= thd->m_transaction_psi;
  thd->m_transaction_psi= 0;
  wsrep_on= thd->variables.wsrep_on;
  thd->variables.wsrep_on= 0;
  thd->server_status&= ~(SERVER_STATUS_IN_TRANS |
                         SERVER_STATUS_IN_TRANS_READONLY);
  thd->server_status|= SERVER_STATUS_AUTOCOMMIT;
}


void start_new_trans::restore_old_transaction()
{
  org_thd->transaction= old_transaction;
  org_thd->restore_backup_open_tables_state(&open_tables_state_backup);
  ha_close_connection(org_thd);
  memcpy(org_thd->ha_data, old_ha_data, sizeof(old_ha_data));
  org_thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  org_thd->in_sub_stmt= in_sub_stmt;
  org_thd->server_status= server_status;
  if (org_thd->m_transaction_psi)
    MYSQL_COMMIT_TRANSACTION(org_thd->m_transaction_psi);
  org_thd->m_transaction_psi= m_transaction_psi;
  org_thd->variables.wsrep_on= wsrep_on;
  org_thd= 0;
}


/**
  Decide on logging format to use for the statement and issue errors
  or warnings as needed.  The decision depends on the following
  parameters:

  - The logging mode, i.e., the value of binlog_format.  Can be
    statement, mixed, or row.

  - The type of statement.  There are three types of statements:
    "normal" safe statements; unsafe statements; and row injections.
    An unsafe statement is one that, if logged in statement format,
    might produce different results when replayed on the slave (e.g.,
    INSERT DELAYED).  A row injection is either a BINLOG statement, or
    a row event executed by the slave's SQL thread.

  - The capabilities of tables modified by the statement.  The
    *capabilities vector* for a table is a set of flags associated
    with the table.  Currently, it only includes two flags: *row
    capability flag* and *statement capability flag*.

    The row capability flag is set if and only if the engine can
    handle row-based logging. The statement capability flag is set if
    and only if the table can handle statement-based logging.

  Decision table for logging format
  ---------------------------------

  The following table summarizes how the format and generated
  warning/error depends on the tables' capabilities, the statement
  type, and the current binlog_format.

     Row capable        N NNNNNNNNN YYYYYYYYY YYYYYYYYY
     Statement capable  N YYYYYYYYY NNNNNNNNN YYYYYYYYY

     Statement type     * SSSUUUIII SSSUUUIII SSSUUUIII

     binlog_format      * SMRSMRSMR SMRSMRSMR SMRSMRSMR

     Logged format      - SS-S----- -RR-RR-RR SRRSRR-RR
     Warning/Error      1 --2732444 5--5--6-- ---7--6--

  Legend
  ------

  Row capable:    N - Some table not row-capable, Y - All tables row-capable
  Stmt capable:   N - Some table not stmt-capable, Y - All tables stmt-capable
  Statement type: (S)afe, (U)nsafe, or Row (I)njection
  binlog_format:  (S)TATEMENT, (M)IXED, or (R)OW
  Logged format:  (S)tatement or (R)ow
  Warning/Error:  Warnings and error messages are as follows:

  1. Error: Cannot execute statement: binlogging impossible since both
     row-incapable engines and statement-incapable engines are
     involved.

  2. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = ROW and at least one table uses a storage engine
     limited to statement-logging.

  3. Error: Cannot execute statement: binlogging of unsafe statement
     is impossible when storage engine is limited to statement-logging
     and BINLOG_FORMAT = MIXED.

  4. Error: Cannot execute row injection: binlogging impossible since
     at least one table uses a storage engine limited to
     statement-logging.

  5. Error: Cannot execute statement: binlogging impossible since
     BINLOG_FORMAT = STATEMENT and at least one table uses a storage
     engine limited to row-logging.

  6. Warning: Unsafe statement binlogged in statement format since
     BINLOG_FORMAT = STATEMENT.

  In addition, we can produce the following error (not depending on
  the variables of the decision diagram):

  7. Error: Cannot execute statement: binlogging impossible since more
     than one engine is involved and at least one engine is
     self-logging.

  For each error case above, the statement is prevented from being
  logged, we report an error, and roll back the statement.  For
  warnings, we set the thd->binlog_flags variable: the warning will be
  printed only if the statement is successfully logged.

  @see THD::binlog_query

  @param[in] thd    Client thread
  @param[in] tables Tables involved in the query

  @retval 0 No error; statement can be logged.
  @retval -1 One of the error conditions above applies (1, 2, 4, 5, or 6).
*/

int THD::decide_logging_format(TABLE_LIST *tables)
{
  DBUG_ENTER("THD::decide_logging_format");
  DBUG_PRINT("info", ("Query: %.*s", (uint) query_length(), query()));
  DBUG_PRINT("info", ("binlog_format: %lu", (ulong) variables.binlog_format));
  DBUG_PRINT("info", ("current_stmt_binlog_format: %lu",
                      (ulong) current_stmt_binlog_format));
  DBUG_PRINT("info", ("lex->get_stmt_unsafe_flags(): 0x%x",
                      lex->get_stmt_unsafe_flags()));

  reset_binlog_local_stmt_filter();

  /*
    We should not decide logging format if the binlog is closed or
    binlogging is off, or if the statement is filtered out from the
    binlog by filtering rules.
  */
#ifdef WITH_WSREP
  if (WSREP_CLIENT_NNULL(this) &&
      wsrep_thd_is_local(this) &&
      wsrep_is_active(this) &&
      variables.wsrep_trx_fragment_size > 0)
  {
    if (!is_current_stmt_binlog_format_row())
    {
      my_message(ER_NOT_SUPPORTED_YET,
                 "Streaming replication not supported with "
                 "binlog_format=STATEMENT", MYF(0));
      DBUG_RETURN(-1);
    }
  }
#endif /* WITH_WSREP */

  if (WSREP_EMULATE_BINLOG_NNULL(this) ||
      binlog_table_should_be_logged(&db))
  {
    if (is_bulk_op())
    {
      if (wsrep_binlog_format() == BINLOG_FORMAT_STMT)
      {
        my_error(ER_BINLOG_NON_SUPPORTED_BULK, MYF(0));
        DBUG_PRINT("info",
                   ("decision: no logging since an error was generated"));
        DBUG_RETURN(-1);
      }
    }
    /*
      Compute one bit field with the union of all the engine
      capabilities, and one with the intersection of all the engine
      capabilities.
    */
    handler::Table_flags flags_write_some_set= 0;
    handler::Table_flags flags_access_some_set= 0;
    handler::Table_flags flags_write_all_set=
      HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE;

    /* 
       If different types of engines are about to be updated.
       For example: Innodb and Falcon; Innodb and MyIsam.
    */
    bool multi_write_engine= FALSE;
    /*
       If different types of engines are about to be accessed 
       and any of them is about to be updated. For example:
       Innodb and Falcon; Innodb and MyIsam.
    */
    bool multi_access_engine= FALSE;
    /*
      Identifies if a table is changed.
    */
    bool is_write= FALSE;                        // If any write tables
    bool has_read_tables= FALSE;                 // If any read only tables
    bool has_auto_increment_write_tables= FALSE; // Write with auto-increment
    /* true if it's necessary to switch current statement log format from
    STATEMENT to ROW if binary log format is MIXED and autoincrement values
    are changed in the statement */
    bool has_unsafe_stmt_autoinc_lock_mode= false;
    /* If a write table that doesn't have auto increment part first */
    bool has_write_table_auto_increment_not_first_in_pk= FALSE;
    bool has_auto_increment_write_tables_not_first= FALSE;
    bool found_first_not_own_table= FALSE;
    bool has_write_tables_with_unsafe_statements= FALSE;
    bool blackhole_table_found= 0;

    /*
      A pointer to a previous table that was changed.
    */
    TABLE* prev_write_table= NULL;
    /*
      A pointer to a previous table that was accessed.
    */
    TABLE* prev_access_table= NULL;
    /**
      The number of tables used in the current statement,
      that should be replicated.
    */
    uint replicated_tables_count= 0;
    /**
      The number of tables written to in the current statement,
      that should not be replicated.
      A table should not be replicated when it is considered
      'local' to a MySQL instance.
      Currently, these tables are:
      - mysql.slow_log
      - mysql.general_log
      - mysql.slave_relay_log_info
      - mysql.slave_master_info
      - mysql.slave_worker_info
      - performance_schema.*
      - TODO: information_schema.*
      In practice, from this list, only performance_schema.* tables
      are written to by user queries.
    */
    uint non_replicated_tables_count= 0;

#ifndef DBUG_OFF
    {
      static const char *prelocked_mode_name[] = {
        "NON_PRELOCKED",
        "LOCK_TABLES",
        "PRELOCKED",
        "PRELOCKED_UNDER_LOCK_TABLES",
      };
      compile_time_assert(array_elements(prelocked_mode_name) == LTM_always_last);
      DBUG_PRINT("debug", ("prelocked_mode: %s",
                           prelocked_mode_name[locked_tables_mode]));
    }
#endif

    /*
      Get the capabilities vector for all involved storage engines and
      mask out the flags for the binary log.
    */
    for (TABLE_LIST *tbl= tables; tbl; tbl= tbl->next_global)
    {
      TABLE *table;
      TABLE_SHARE *share;
      handler::Table_flags flags;
      if (tbl->placeholder())
        continue;

      table= tbl->table;
      share= table->s;
      flags= table->file->ha_table_flags();
      if (!share->table_creation_was_logged)
      {
        /*
          This is a temporary table which was not logged in the binary log.
          Disable statement logging to enforce row level logging.
        */
        DBUG_ASSERT(share->tmp_table);
        flags&= ~HA_BINLOG_STMT_CAPABLE;
        /* We can only use row logging */
        set_current_stmt_binlog_format_row();
      }

      DBUG_PRINT("info", ("table: %s; ha_table_flags: 0x%llx",
                          tbl->table_name.str, flags));

      if (share->no_replicate)
      {
        /*
          The statement uses a table that is not replicated.
          The following properties about the table:
          - persistent / transient
          - transactional / non transactional
          - temporary / permanent
          - read or write
          - multiple engines involved because of this table
          are not relevant, as this table is completely ignored.
          Because the statement uses a non replicated table,
          using STATEMENT format in the binlog is impossible.
          Either this statement will be discarded entirely,
          or it will be logged (possibly partially) in ROW format.
        */
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_TABLE);

        if (tbl->lock_type >= TL_FIRST_WRITE)
        {
          non_replicated_tables_count++;
          continue;
        }
      }
      if (tbl == lex->first_not_own_table())
        found_first_not_own_table= true;

      replicated_tables_count++;

      if (tbl->prelocking_placeholder != TABLE_LIST::PRELOCK_FK)
      {
        if (tbl->lock_type < TL_FIRST_WRITE)
          has_read_tables= true;
        else if (table->found_next_number_field &&
                 (tbl->lock_type >= TL_FIRST_WRITE))
        {
          has_auto_increment_write_tables= true;
          has_auto_increment_write_tables_not_first= found_first_not_own_table;
          if (share->next_number_keypart != 0)
            has_write_table_auto_increment_not_first_in_pk= true;
          has_unsafe_stmt_autoinc_lock_mode=
            table->file->autoinc_lock_mode_stmt_unsafe();
        }
      }

      if (tbl->lock_type >= TL_FIRST_WRITE)
      {
        bool trans;
        if (prev_write_table && prev_write_table->file->ht !=
            table->file->ht)
          multi_write_engine= TRUE;

        if (table->file->ht->db_type == DB_TYPE_BLACKHOLE_DB)
          blackhole_table_found= 1;

        if (share->non_determinstic_insert &&
            (sql_command_flags[lex->sql_command] & CF_CAN_GENERATE_ROW_EVENTS
             && !(sql_command_flags[lex->sql_command] & CF_SCHEMA_CHANGE)))
          has_write_tables_with_unsafe_statements= true;

        trans= table->file->has_transactions();

        if (share->tmp_table)
          lex->set_stmt_accessed_table(trans ? LEX::STMT_WRITES_TEMP_TRANS_TABLE :
                                               LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans ? LEX::STMT_WRITES_TRANS_TABLE :
                                               LEX::STMT_WRITES_NON_TRANS_TABLE);

        flags_write_all_set &= flags;
        flags_write_some_set |= flags;
        is_write= TRUE;

        prev_write_table= table;

      }
      flags_access_some_set |= flags;

      if (lex->sql_command != SQLCOM_CREATE_TABLE || lex->tmp_table())
      {
        my_bool trans= table->file->has_transactions();

        if (share->tmp_table)
          lex->set_stmt_accessed_table(trans ? LEX::STMT_READS_TEMP_TRANS_TABLE :
                                               LEX::STMT_READS_TEMP_NON_TRANS_TABLE);
        else
          lex->set_stmt_accessed_table(trans ? LEX::STMT_READS_TRANS_TABLE :
                                               LEX::STMT_READS_NON_TRANS_TABLE);
      }

      if (prev_access_table && prev_access_table->file->ht !=
          table->file->ht)
        multi_access_engine= TRUE;

      prev_access_table= table;
    }

    if (wsrep_binlog_format() != BINLOG_FORMAT_ROW)
    {
      /*
        DML statements that modify a table with an auto_increment
        column based on rows selected from a table are unsafe as the
        order in which the rows are fetched fron the select tables
        cannot be determined and may differ on master and slave.
      */
      if (has_auto_increment_write_tables && has_read_tables)
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_WRITE_AUTOINC_SELECT);

      if (has_write_table_auto_increment_not_first_in_pk)
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_NOT_FIRST);

      if (has_write_tables_with_unsafe_statements)
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);

      if (has_unsafe_stmt_autoinc_lock_mode)
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_LOCK_MODE);

      /*
        A query that modifies autoinc column in sub-statement can make the
        master and slave inconsistent.
        We can solve these problems in mixed mode by switching to binlogging
        if at least one updated table is used by sub-statement
      */
      if (lex->requires_prelocking() &&
          has_auto_increment_write_tables_not_first)
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_AUTOINC_COLUMNS);
    }

    DBUG_PRINT("info", ("flags_write_all_set: 0x%llx", flags_write_all_set));
    DBUG_PRINT("info", ("flags_write_some_set: 0x%llx", flags_write_some_set));
    DBUG_PRINT("info", ("flags_access_some_set: 0x%llx", flags_access_some_set));
    DBUG_PRINT("info", ("multi_write_engine: %d", multi_write_engine));
    DBUG_PRINT("info", ("multi_access_engine: %d", multi_access_engine));

    int error= 0;
    int unsafe_flags;

    bool multi_stmt_trans= in_multi_stmt_transaction_mode();
    bool trans_table= trans_has_updated_trans_table(this);
    bool binlog_direct= variables.binlog_direct_non_trans_update;

    if (lex->is_mixed_stmt_unsafe(multi_stmt_trans, binlog_direct,
                                  trans_table, tx_isolation))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_MIXED_STATEMENT);
    else if (multi_stmt_trans && trans_table && !binlog_direct &&
             lex->stmt_accessed_table(LEX::STMT_WRITES_NON_TRANS_TABLE))
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_NONTRANS_AFTER_TRANS);

    /*
      If more than one engine is involved in the statement and at
      least one is doing it's own logging (is *self-logging*), the
      statement cannot be logged atomically, so we generate an error
      rather than allowing the binlog to become corrupt.
    */
    if (multi_write_engine &&
        (flags_write_some_set & HA_HAS_OWN_BINLOGGING))
      my_error((error= ER_BINLOG_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE),
               MYF(0));
    else if (multi_access_engine && flags_access_some_set & HA_HAS_OWN_BINLOGGING)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE);

    /* both statement-only and row-only engines involved */
    if ((flags_write_all_set & (HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE)) == 0)
    {
      /*
        1. Error: Binary logging impossible since both row-incapable
           engines and statement-incapable engines are involved
      */
      my_error((error= ER_BINLOG_ROW_ENGINE_AND_STMT_ENGINE), MYF(0));
    }
    /* statement-only engines involved */
    else if ((flags_write_all_set & HA_BINLOG_ROW_CAPABLE) == 0)
    {
      if (lex->is_stmt_row_injection())
      {
        /*
          4. Error: Cannot execute row injection since table uses
             storage engine limited to statement-logging
        */
        my_error((error= ER_BINLOG_ROW_INJECTION_AND_STMT_ENGINE), MYF(0));
      }
      else if ((wsrep_binlog_format() == BINLOG_FORMAT_ROW || is_bulk_op()) &&
               sqlcom_can_generate_row_events(this))
      {
        /*
          2. Error: Cannot modify table that uses a storage engine
             limited to statement-logging when BINLOG_FORMAT = ROW
        */
        my_error((error= ER_BINLOG_ROW_MODE_AND_STMT_ENGINE), MYF(0));
      }
      else if ((unsafe_flags= lex->get_stmt_unsafe_flags()) != 0)
      {
        /*
          3. Error: Cannot execute statement: binlogging of unsafe
             statement is impossible when storage engine is limited to
             statement-logging and BINLOG_FORMAT = MIXED.
        */
        for (int unsafe_type= 0;
             unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
             unsafe_type++)
          if (unsafe_flags & (1 << unsafe_type))
            my_error((error= ER_BINLOG_UNSAFE_AND_STMT_ENGINE), MYF(0),
                     ER_THD(this,
                            LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      }
      /* log in statement format! */
    }
    /* no statement-only engines */
    else
    {
      /* binlog_format = STATEMENT */
      if (wsrep_binlog_format() == BINLOG_FORMAT_STMT)
      {
        if (lex->is_stmt_row_injection())
        {
          /*
            We have to log the statement as row or give an error.
            Better to accept what master gives us than stopping replication.
          */
          set_current_stmt_binlog_format_row();
        }
        else if ((flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0 &&
                 sqlcom_can_generate_row_events(this))
        {
          /*
            5. Error: Cannot modify table that uses a storage engine
               limited to row-logging when binlog_format = STATEMENT, except
               if all tables that are updated are temporary tables
          */
          if (!lex->stmt_writes_to_non_temp_table())
          {
            /* As all updated tables are temporary, nothing will be logged */
            set_current_stmt_binlog_format_row();
          }
          else if (IF_WSREP((!WSREP_NNULL(this) ||
                             wsrep_cs().mode() ==
                             wsrep::client_state::m_local),1))
	  {
            my_error((error= ER_BINLOG_STMT_MODE_AND_ROW_ENGINE), MYF(0), "");
	  }
        }
        else if (is_write && (unsafe_flags= lex->get_stmt_unsafe_flags()) != 0)
        {
          /*
            7. Warning: Unsafe statement logged as statement due to
               binlog_format = STATEMENT
          */
          binlog_unsafe_warning_flags|= unsafe_flags;

          DBUG_PRINT("info", ("Scheduling warning to be issued by "
                              "binlog_query: '%s'",
                              ER_THD(this, ER_BINLOG_UNSAFE_STATEMENT)));
          DBUG_PRINT("info", ("binlog_unsafe_warning_flags: 0x%x",
                              binlog_unsafe_warning_flags));
        }
        /* log in statement format (or row if row event)! */
      }
      /* No statement-only engines and binlog_format != STATEMENT.
         I.e., nothing prevents us from row logging if needed. */
      else
      {
        if (lex->is_stmt_unsafe() || lex->is_stmt_row_injection()
            || (flags_write_all_set & HA_BINLOG_STMT_CAPABLE) == 0 ||
            is_bulk_op())
        {
          /* log in row format! */
          set_current_stmt_binlog_format_row_if_mixed();
        }
      }
    }

    if (non_replicated_tables_count > 0)
    {
      if ((replicated_tables_count == 0) || ! is_write)
      {
        DBUG_PRINT("info", ("decision: no logging, no replicated table affected"));
        set_binlog_local_stmt_filter();
      }
      else
      {
        if (! is_current_stmt_binlog_format_row())
        {
          my_error((error= ER_BINLOG_STMT_MODE_AND_NO_REPL_TABLES), MYF(0));
        }
        else
        {
          clear_binlog_local_stmt_filter();
        }
      }
    }
    else
    {
      clear_binlog_local_stmt_filter();
    }

    if (unlikely(error))
    {
      DBUG_PRINT("info", ("decision: no logging since an error was generated"));
      DBUG_RETURN(-1);
    }
    DBUG_PRINT("info", ("decision: logging in %s format",
                        is_current_stmt_binlog_format_row() ?
                        "ROW" : "STATEMENT"));

    if (blackhole_table_found &&
        variables.binlog_format == BINLOG_FORMAT_ROW &&
        (sql_command_flags[lex->sql_command] &
         (CF_UPDATES_DATA | CF_DELETES_DATA)))
    {
      String table_names;
      /*
        Generate a warning for UPDATE/DELETE statements that modify a
        BLACKHOLE table, as row events are not logged in row format.
      */
      for (TABLE_LIST *table= tables; table; table= table->next_global)
      {
        if (table->placeholder())
          continue;
        if (table->table->file->ht->db_type == DB_TYPE_BLACKHOLE_DB &&
            table->lock_type >= TL_FIRST_WRITE)
        {
          table_names.append(&table->table_name);
          table_names.append(',');
        }
      }
      if (!table_names.is_empty())
      {
        bool is_update= MY_TEST(sql_command_flags[lex->sql_command] &
                                CF_UPDATES_DATA);
        /*
          Replace the last ',' with '.' for table_names
        */
        table_names.replace(table_names.length()-1, 1, ".", 1);
        push_warning_printf(this, Sql_condition::WARN_LEVEL_WARN,
                            ER_UNKNOWN_ERROR,
                            "Row events are not logged for %s statements "
                            "that modify BLACKHOLE tables in row format. "
                            "Table(s): '%-.192s'",
                            is_update ? "UPDATE" : "DELETE",
                            table_names.c_ptr());
      }
    }

    if (is_write && is_current_stmt_binlog_format_row())
      binlog_prepare_for_row_logging();
  }
  else
  {
    DBUG_PRINT("info", ("decision: no logging since "
                        "mysql_bin_log.is_open() = %d "
                        "and (options & OPTION_BIN_LOG) = 0x%llx "
                        "and binlog_format = %u "
                        "and binlog_filter->db_ok(db) = %d",
                        mysql_bin_log.is_open(),
                        (variables.option_bits & OPTION_BIN_LOG),
                        (uint) wsrep_binlog_format(),
                        binlog_filter->db_ok(db.str)));
    if (WSREP_NNULL(this) && is_current_stmt_binlog_format_row())
      binlog_prepare_for_row_logging();
  }
  DBUG_RETURN(0);
}


/*
  Reconsider logging format in case of INSERT...ON DUPLICATE KEY UPDATE
  for tables with more than one unique keys in case of MIXED binlog format.

  Unsafe means that a master could execute the statement differently than
  the slave.
  This could can happen in the following cases:
  - The unique check are done in different order on master or slave
    (different engine or different key order).
  - There is a conflict on another key than the first and before the
    statement is committed, another connection commits a row that conflicts
    on an earlier unique key. Example follows:

    Below a and b are unique keys, the table has a row (1,1,0)
    connection 1:
    INSERT INTO t1 set a=2,b=1,c=0 ON DUPLICATE KEY UPDATE c=1;
    connection 2:
    INSERT INTO t1 set a=2,b=2,c=0;

    If 2 commits after 1 has been executed but before 1 has committed
    (and are thus put before the other in the binary log), one will
    get different data on the slave:
    (1,1,1),(2,2,1) instead of (1,1,1),(2,2,0)
*/

void THD::reconsider_logging_format_for_iodup(TABLE *table)
{
  DBUG_ENTER("reconsider_logging_format_for_iodup");
  enum_binlog_format bf= (enum_binlog_format) wsrep_binlog_format();

  DBUG_ASSERT(lex->duplicates == DUP_UPDATE);

  if (bf <= BINLOG_FORMAT_STMT &&
      !is_current_stmt_binlog_format_row())
  {
    KEY *end= table->s->key_info + table->s->keys;
    uint unique_keys= 0;

    for (KEY *keyinfo= table->s->key_info; keyinfo < end ; keyinfo++)
    {
      if (keyinfo->flags & HA_NOSAME)
      {
        /*
          We assume that the following cases will guarantee that the
          key is unique if a key part is not set:
          - The key part is an autoincrement (autogenerated)
          - The key part has a default value that is null and it not
            a virtual field that will be calculated later.
        */
        for (uint j= 0; j < keyinfo->user_defined_key_parts; j++)
        {
          Field *field= keyinfo->key_part[j].field;
          if (!bitmap_is_set(table->write_set, field->field_index))
          {
            /* Check auto_increment */
            if (field == table->next_number_field)
              goto exit;
            if (field->is_real_null() && !field->default_value)
              goto exit;
          }
        }
        if (unique_keys++)
          break;
exit:;
      }
    }
    if (unique_keys > 1)
    {
      if (bf == BINLOG_FORMAT_STMT && !lex->is_stmt_unsafe())
      {
        lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS);
        binlog_unsafe_warning_flags|= lex->get_stmt_unsafe_flags();
      }
      set_current_stmt_binlog_format_row_if_mixed();
      if (is_current_stmt_binlog_format_row())
        binlog_prepare_for_row_logging();
    }
  }
  DBUG_VOID_RETURN;
}

#ifndef MYSQL_CLIENT
/**
  Check if we should log a table DDL to the binlog

  @retval true  yes
  @retval false no
*/

bool THD::binlog_table_should_be_logged(const LEX_CSTRING *db)
{
  return (mysql_bin_log.is_open() &&
          (variables.option_bits & OPTION_BIN_LOG) &&
          (wsrep_binlog_format() != BINLOG_FORMAT_STMT ||
           binlog_filter->db_ok(db->str)));
}

/*
  Template member function for ensuring that there is an rows log
  event of the apropriate type before proceeding.

  PRE CONDITION:
    - Events of type 'RowEventT' have the type code 'type_code'.

  POST CONDITION:
    If a non-NULL pointer is returned, the pending event for thread 'thd' will
    be an event of type 'RowEventT' (which have the type code 'type_code')
    will either empty or have enough space to hold 'needed' bytes.  In
    addition, the columns bitmap will be correct for the row, meaning that
    the pending event will be flushed if the columns in the event differ from
    the columns suppled to the function.

  RETURNS
    If no error, a non-NULL pending event (either one which already existed or
    the newly created one).
    If error, NULL.
 */

template <class RowsEventT> Rows_log_event*
THD::binlog_prepare_pending_rows_event(TABLE* table, uint32 serv_id,
                                       size_t needed,
                                       bool is_transactional,
                                       RowsEventT *hint __attribute__((unused)))
{
  DBUG_ENTER("binlog_prepare_pending_rows_event");
  /* Pre-conditions */
  DBUG_ASSERT(table->s->table_map_id != ~0UL);

  /* Fetch the type code for the RowsEventT template parameter */
  int const general_type_code= RowsEventT::TYPE_CODE;

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_transactional= 1;

  /*
    There is no good place to set up the transactional data, so we
    have to do it here.
  */
  if (binlog_setup_trx_data() == NULL)
    DBUG_RETURN(NULL);

  Rows_log_event* pending= binlog_get_pending_rows_event(is_transactional);

  if (unlikely(pending && !pending->is_valid()))
    DBUG_RETURN(NULL);

  /*
    Check if the current event is non-NULL and a write-rows
    event. Also check if the table provided is mapped: if it is not,
    then we have switched to writing to a new table.
    If there is no pending event, we need to create one. If there is a pending
    event, but it's not about the same table id, or not of the same type
    (between Write, Update and Delete), or not the same affected columns, or
    going to be too big, flush this event to disk and create a new pending
    event.
  */
  if (!pending ||
      pending->server_id != serv_id ||
      pending->get_table_id() != table->s->table_map_id ||
      pending->get_general_type_code() != general_type_code ||
      pending->get_data_size() + needed > opt_binlog_rows_event_max_size ||
      pending->read_write_bitmaps_cmp(table) == FALSE)
  {
    /* Create a new RowsEventT... */
    Rows_log_event* const
        ev= new RowsEventT(this, table, table->s->table_map_id,
                           is_transactional);
    if (unlikely(!ev))
      DBUG_RETURN(NULL);
    ev->server_id= serv_id; // I don't like this, it's too easy to forget.
    /*
      flush the pending event and replace it with the newly created
      event...
    */
    if (unlikely(
        mysql_bin_log.flush_and_set_pending_rows_event(this, ev,
                                                       is_transactional)))
    {
      delete ev;
      DBUG_RETURN(NULL);
    }

    DBUG_RETURN(ev);               /* This is the new pending event */
  }
  DBUG_RETURN(pending);        /* This is the current pending event */
}

/* Declare in unnamed namespace. */
CPP_UNNAMED_NS_START
  /**
     Class to handle temporary allocation of memory for row data.

     The responsibilities of the class is to provide memory for
     packing one or two rows of packed data (depending on what
     constructor is called).

     In order to make the allocation more efficient for "simple" rows,
     i.e., rows that do not contain any blobs, a pointer to the
     allocated memory is of memory is stored in the table structure
     for simple rows.  If memory for a table containing a blob field
     is requested, only memory for that is allocated, and subsequently
     released when the object is destroyed.

   */
  class Row_data_memory {
  public:
    /**
      Build an object to keep track of a block-local piece of memory
      for storing a row of data.

      @param table
      Table where the pre-allocated memory is stored.

      @param length
      Length of data that is needed, if the record contain blobs.
     */
    Row_data_memory(TABLE *table, size_t const len1)
      : m_memory(0)
    {
#ifndef DBUG_OFF
      m_alloc_checked= FALSE;
#endif
      allocate_memory(table, len1);
      m_ptr[0]= has_memory() ? m_memory : 0;
      m_ptr[1]= 0;
    }

    Row_data_memory(TABLE *table, size_t const len1, size_t const len2)
      : m_memory(0)
    {
#ifndef DBUG_OFF
      m_alloc_checked= FALSE;
#endif
      allocate_memory(table, len1 + len2);
      m_ptr[0]= has_memory() ? m_memory        : 0;
      m_ptr[1]= has_memory() ? m_memory + len1 : 0;
    }

    ~Row_data_memory()
    {
      if (m_memory != 0 && m_release_memory_on_destruction)
        my_free(m_memory);
    }

    /**
       Is there memory allocated?

       @retval true There is memory allocated
       @retval false Memory allocation failed
     */
    bool has_memory() const {
#ifndef DBUG_OFF
      m_alloc_checked= TRUE;
#endif
      return m_memory != 0;
    }

    uchar *slot(uint s)
    {
      DBUG_ASSERT(s < sizeof(m_ptr)/sizeof(*m_ptr));
      DBUG_ASSERT(m_ptr[s] != 0);
      DBUG_SLOW_ASSERT(m_alloc_checked == TRUE);
      return m_ptr[s];
    }

  private:
    void allocate_memory(TABLE *const table, size_t const total_length)
    {
      if (table->s->blob_fields == 0)
      {
        /*
          The maximum length of a packed record is less than this
          length. We use this value instead of the supplied length
          when allocating memory for records, since we don't know how
          the memory will be used in future allocations.

          Since table->s->reclength is for unpacked records, we have
          to add two bytes for each field, which can potentially be
          added to hold the length of a packed field.
        */
        size_t const maxlen= table->s->reclength + 2 * table->s->fields;

        /*
          Allocate memory for two records if memory hasn't been
          allocated. We allocate memory for two records so that it can
          be used when processing update rows as well.
        */
        if (table->write_row_record == 0)
          table->write_row_record=
            (uchar *) alloc_root(&table->mem_root, 2 * maxlen);
        m_memory= table->write_row_record;
        m_release_memory_on_destruction= FALSE;
      }
      else
      {
        m_memory= (uchar *) my_malloc(key_memory_Row_data_memory_memory,
                                      total_length, MYF(MY_WME));
        m_release_memory_on_destruction= TRUE;
      }
    }

#ifndef DBUG_OFF
    mutable bool m_alloc_checked;
#endif
    bool m_release_memory_on_destruction;
    uchar *m_memory;
    uchar *m_ptr[2];
  };

CPP_UNNAMED_NS_END

int THD::binlog_write_row(TABLE* table, bool is_trans,
                          uchar const *record)
{

  DBUG_ASSERT(is_current_stmt_binlog_format_row());
  DBUG_ASSERT((WSREP_NNULL(this) && wsrep_emulate_bin_log) ||
              mysql_bin_log.is_open());
  /*
    Pack records into format for transfer. We are allocating more
    memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, max_row_length(table, table->rpl_write_set,
                                               record));
  if (!memory.has_memory())
    return HA_ERR_OUT_OF_MEM;

  uchar *row_data= memory.slot(0);

  size_t const len= pack_row(table, table->rpl_write_set, row_data, record);

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_trans= 1;

  Rows_log_event* ev;
  if (binlog_should_compress(len))
    ev =
    binlog_prepare_pending_rows_event(table, variables.server_id,
                                      len, is_trans,
                                      static_cast<Write_rows_compressed_log_event*>(0));
  else
    ev =
    binlog_prepare_pending_rows_event(table, variables.server_id,
                                      len, is_trans,
                                      static_cast<Write_rows_log_event*>(0));

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;

  return ev->add_row_data(row_data, len);
}

int THD::binlog_update_row(TABLE* table, bool is_trans,
                           const uchar *before_record,
                           const uchar *after_record)
{
  DBUG_ASSERT(is_current_stmt_binlog_format_row());
  DBUG_ASSERT((WSREP_NNULL(this) && wsrep_emulate_bin_log) ||
              mysql_bin_log.is_open());

  /**
    Save a reference to the original read bitmaps
    We will need this to restore the bitmaps at the end as
    binlog_prepare_row_images() may change table->read_set.
    table->read_set is used by pack_row and deep in
    binlog_prepare_pending_events().
  */
  MY_BITMAP *old_read_set= table->read_set;

  /**
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(table);

  size_t const before_maxlen= max_row_length(table, table->read_set,
                                             before_record);
  size_t const after_maxlen=  max_row_length(table, table->rpl_write_set,
                                             after_record);

  Row_data_memory row_data(table, before_maxlen, after_maxlen);
  if (!row_data.has_memory())
    return HA_ERR_OUT_OF_MEM;

  uchar *before_row= row_data.slot(0);
  uchar *after_row= row_data.slot(1);

  size_t const before_size= pack_row(table, table->read_set, before_row,
                                     before_record);
  size_t const after_size= pack_row(table, table->rpl_write_set, after_row,
                                    after_record);

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_trans= 1;

  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
#ifndef HAVE_valgrind
  DBUG_DUMP("before_record", before_record, table->s->reclength);
  DBUG_DUMP("after_record",  after_record, table->s->reclength);
  DBUG_DUMP("before_row",    before_row, before_size);
  DBUG_DUMP("after_row",     after_row, after_size);
#endif

  Rows_log_event* ev;
  if(binlog_should_compress(before_size + after_size))
    ev =
      binlog_prepare_pending_rows_event(table, variables.server_id,
                                      before_size + after_size, is_trans,
                                      static_cast<Update_rows_compressed_log_event*>(0));
  else
    ev =
      binlog_prepare_pending_rows_event(table, variables.server_id,
                                      before_size + after_size, is_trans,
                                      static_cast<Update_rows_log_event*>(0));

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;

  int error=  ev->add_row_data(before_row, before_size) ||
              ev->add_row_data(after_row, after_size);

  /* restore read set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set,
                                      table->write_set);
  return error;

}

int THD::binlog_delete_row(TABLE* table, bool is_trans, 
                           uchar const *record)
{
  DBUG_ASSERT(is_current_stmt_binlog_format_row());
  DBUG_ASSERT((WSREP_NNULL(this) && wsrep_emulate_bin_log) ||
              mysql_bin_log.is_open());
  /**
    Save a reference to the original read bitmaps
    We will need this to restore the bitmaps at the end as
    binlog_prepare_row_images() may change table->read_set.
    table->read_set is used by pack_row and deep in
    binlog_prepare_pending_events().
  */
  MY_BITMAP *old_read_set= table->read_set;

  /** 
     This will remove spurious fields required during execution but
     not needed for binlogging. This is done according to the:
     binlog-row-image option.
   */
  binlog_prepare_row_images(table);

  /*
     Pack records into format for transfer. We are allocating more
     memory than needed, but that doesn't matter.
  */
  Row_data_memory memory(table, max_row_length(table, table->read_set,
                                               record));
  if (unlikely(!memory.has_memory()))
    return HA_ERR_OUT_OF_MEM;

  uchar *row_data= memory.slot(0);

  DBUG_DUMP("table->read_set", (uchar*) table->read_set->bitmap, (table->s->fields + 7) / 8);
  size_t const len= pack_row(table, table->read_set, row_data, record);

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_trans= 1;

  Rows_log_event* ev;
  if(binlog_should_compress(len))
    ev =
      binlog_prepare_pending_rows_event(table, variables.server_id,
                                      len, is_trans,
                                      static_cast<Delete_rows_compressed_log_event*>(0));
  else
    ev =
      binlog_prepare_pending_rows_event(table, variables.server_id,
                                      len, is_trans,
                                      static_cast<Delete_rows_log_event*>(0));

  if (unlikely(ev == 0))
    return HA_ERR_OUT_OF_MEM;


  int error= ev->add_row_data(row_data, len);

  /* restore read set for the rest of execution */
  table->column_bitmaps_set_no_signal(old_read_set,
                                      table->write_set);

  return error;
}


/**
   Remove from read_set spurious columns. The write_set has been
   handled before in table->mark_columns_needed_for_update.
*/

void THD::binlog_prepare_row_images(TABLE *table)
{
  DBUG_ENTER("THD::binlog_prepare_row_images");

  DBUG_PRINT_BITSET("debug", "table->read_set (before preparing): %s",
                    table->read_set);
  THD *thd= table->in_use;

  /**
    if there is a primary key in the table (ie, user declared PK or a
    non-null unique index) and we don't want to ship the entire image,
    and the handler involved supports this.
   */
  if (table->s->primary_key < MAX_KEY &&
      (thd->variables.binlog_row_image < BINLOG_ROW_IMAGE_FULL) &&
      !ha_check_storage_engine_flag(table->s->db_type(),
                                    HTON_NO_BINLOG_ROW_OPT))
  {
    /**
      Just to be sure that tmp_set is currently not in use as
      the read_set already.
    */
    DBUG_ASSERT(table->read_set != &table->tmp_set);

    switch (thd->variables.binlog_row_image)
    {
      case BINLOG_ROW_IMAGE_MINIMAL:
        /* MINIMAL: Mark only PK */
        table->mark_index_columns(table->s->primary_key,
                                  &table->tmp_set);
        break;
      case BINLOG_ROW_IMAGE_NOBLOB:
        /**
          NOBLOB: Remove unnecessary BLOB fields from read_set
                  (the ones that are not part of PK).
         */
        bitmap_copy(&table->tmp_set, table->read_set);
        for (Field **ptr=table->field ; *ptr ; ptr++)
        {
          Field *field= (*ptr);
          if ((field->type() == MYSQL_TYPE_BLOB) &&
              !(field->flags & PRI_KEY_FLAG))
            bitmap_clear_bit(&table->tmp_set, field->field_index);
        }
        break;
      default:
        DBUG_ASSERT(0); // impossible.
    }

    /* set the temporary read_set */
    table->column_bitmaps_set_no_signal(&table->tmp_set,
                                        table->write_set);
  }

  DBUG_PRINT_BITSET("debug", "table->read_set (after preparing): %s",
                    table->read_set);
  DBUG_VOID_RETURN;
}



int THD::binlog_remove_pending_rows_event(bool reset_stmt,
                                          bool is_transactional)
{
  DBUG_ENTER("THD::binlog_remove_pending_rows_event");

  if(!WSREP_EMULATE_BINLOG_NNULL(this) && !mysql_bin_log.is_open())
    DBUG_RETURN(0);

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_transactional= 1;

  mysql_bin_log.remove_pending_rows_event(this, is_transactional);

  if (reset_stmt)
    reset_binlog_for_next_statement();
  DBUG_RETURN(0);
}


int THD::binlog_flush_pending_rows_event(bool stmt_end, bool is_transactional)
{
  DBUG_ENTER("THD::binlog_flush_pending_rows_event");
  /*
    We shall flush the pending event even if we are not in row-based
    mode: it might be the case that we left row-based mode before
    flushing anything (e.g., if we have explicitly locked tables).
   */
  if (!WSREP_EMULATE_BINLOG_NNULL(this) && !mysql_bin_log.is_open())
    DBUG_RETURN(0);

  /* Ensure that all events in a GTID group are in the same cache */
  if (variables.option_bits & OPTION_GTID_BEGIN)
    is_transactional= 1;

  /*
    Mark the event as the last event of a statement if the stmt_end
    flag is set.
  */
  int error= 0;
  if (Rows_log_event *pending= binlog_get_pending_rows_event(is_transactional))
  {
    if (stmt_end)
    {
      pending->set_flags(Rows_log_event::STMT_END_F);
      reset_binlog_for_next_statement();
    }
    error= mysql_bin_log.flush_and_set_pending_rows_event(this, 0,
                                                          is_transactional);
  }

  DBUG_RETURN(error);
}


/*
  DML that doesn't change the table normally is not logged,
  but it needs to be logged if it auto-created a partition as a side effect.
*/
bool THD::binlog_for_noop_dml(bool transactional_table)
{
  if (log_current_statement())
  {
    reset_unsafe_warnings();
    if (binlog_query(THD::STMT_QUERY_TYPE, query(), query_length(),
                      transactional_table, FALSE, FALSE, 0) > 0)
    {
      my_error(ER_ERROR_ON_WRITE, MYF(0), "binary log", -1);
      return true;
    }
  }
  return false;
}


#if !defined(DBUG_OFF) && !defined(_lint)
static const char *
show_query_type(THD::enum_binlog_query_type qtype)
{
  switch (qtype) {
  case THD::ROW_QUERY_TYPE:
    return "ROW";
  case THD::STMT_QUERY_TYPE:
    return "STMT";
  case THD::QUERY_TYPE_COUNT:
  default:
    DBUG_ASSERT(0 <= qtype && qtype < THD::QUERY_TYPE_COUNT);
  }
  static char buf[64];
  sprintf(buf, "UNKNOWN#%d", qtype);
  return buf;
}
#endif

/*
  Constants required for the limit unsafe warnings suppression
*/
//seconds after which the limit unsafe warnings suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT 5*60
//number of limit unsafe warnings after which the suppression will be activated
#define LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT 10

static ulonglong unsafe_suppression_start_time= 0;
static bool unsafe_warning_suppression_active[LEX::BINLOG_STMT_UNSAFE_COUNT];
static ulong unsafe_warnings_count[LEX::BINLOG_STMT_UNSAFE_COUNT];
static ulong total_unsafe_warnings_count;

/**
  Auxiliary function to reset the limit unsafety warning suppression.
  This is done without mutex protection, but this should be good
  enough as it doesn't matter if we loose a couple of suppressed
  messages or if this is called multiple times.
*/

static void reset_binlog_unsafe_suppression(ulonglong now)
{
  uint i;
  DBUG_ENTER("reset_binlog_unsafe_suppression");

  unsafe_suppression_start_time= now;
  total_unsafe_warnings_count= 0;

  for (i= 0 ; i < LEX::BINLOG_STMT_UNSAFE_COUNT ; i++)
  {
    unsafe_warnings_count[i]= 0;
    unsafe_warning_suppression_active[i]= 0;
  }
  DBUG_VOID_RETURN;
}

/**
  Auxiliary function to print warning in the error log.
*/
static void print_unsafe_warning_to_log(THD *thd, int unsafe_type, char* buf,
                                        char* query)
{
  DBUG_ENTER("print_unsafe_warning_in_log");
  sprintf(buf, ER_THD(thd, ER_BINLOG_UNSAFE_STATEMENT),
          ER_THD(thd, LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
  sql_print_warning(ER_THD(thd, ER_MESSAGE_AND_STATEMENT), buf, query);
  DBUG_VOID_RETURN;
}

/**
  Auxiliary function to check if the warning for unsafe repliction statements
  should be thrown or suppressed.

  Logic is:
  - If we get more than LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT errors
    of one type, that type of errors will be suppressed for
    LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT.
  - When the time limit has been reached, all suppression is reset.

  This means that if one gets many different types of errors, some of them
  may be reset less than LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT. However at
  least one error is disable for this time.

  SYNOPSIS:
  @params
   unsafe_type - The type of unsafety.

  RETURN:
    0   0k to log
    1   Message suppressed
*/

static bool protect_against_unsafe_warning_flood(int unsafe_type)
{
  ulong count;
  ulonglong now= my_interval_timer()/1000000000ULL;
  DBUG_ENTER("protect_against_unsafe_warning_flood");

  count= ++unsafe_warnings_count[unsafe_type];
  total_unsafe_warnings_count++;

  /*
    INITIALIZING:
    If this is the first time this function is called with log warning
    enabled, the monitoring the unsafe warnings should start.
  */
  if (unsafe_suppression_start_time == 0)
  {
    reset_binlog_unsafe_suppression(now);
    DBUG_RETURN(0);
  }

  /*
    The following is true if we got too many errors or if the error was
    already suppressed
  */
  if (count >= LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT)
  {
    ulonglong diff_time= (now - unsafe_suppression_start_time);

    if (!unsafe_warning_suppression_active[unsafe_type])
    {
      /*
        ACTIVATION:
        We got LIMIT_UNSAFE_WARNING_ACTIVATION_THRESHOLD_COUNT warnings in
        less than LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT we activate the
        suppression.
      */
      if (diff_time <= LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT)
      {
        unsafe_warning_suppression_active[unsafe_type]= 1;
        sql_print_information("Suppressing warnings of type '%s' for up to %d seconds because of flooding",
                              ER(LEX::binlog_stmt_unsafe_errcode[unsafe_type]),
                              LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT);
      }
      else
      {
        /*
          There is no flooding till now, therefore we restart the monitoring
        */
        reset_binlog_unsafe_suppression(now);
      }
    }
    else
    {
      /* This type of warnings was suppressed */
      if (diff_time > LIMIT_UNSAFE_WARNING_ACTIVATION_TIMEOUT)
      {
        ulong save_count= total_unsafe_warnings_count;
        /* Print a suppression note and remove the suppression */
        reset_binlog_unsafe_suppression(now);
        sql_print_information("Suppressed %lu unsafe warnings during "
                              "the last %d seconds",
                              save_count, (int) diff_time);
      }
    }
  }
  DBUG_RETURN(unsafe_warning_suppression_active[unsafe_type]);
}

MYSQL_TIME THD::query_start_TIME()
{
  MYSQL_TIME res;
  variables.time_zone->gmt_sec_to_TIME(&res, query_start());
  res.second_part= query_start_sec_part();
  time_zone_used= 1;
  return res;
}

/**
  Auxiliary method used by @c binlog_query() to raise warnings.

  The type of warning and the type of unsafeness is stored in
  THD::binlog_unsafe_warning_flags.
*/
void THD::issue_unsafe_warnings()
{
  char buf[MYSQL_ERRMSG_SIZE * 2];
  uint32 unsafe_type_flags;
  DBUG_ENTER("issue_unsafe_warnings");
  /*
    Ensure that binlog_unsafe_warning_flags is big enough to hold all
    bits.  This is actually a constant expression.
  */
  DBUG_ASSERT(LEX::BINLOG_STMT_UNSAFE_COUNT <=
              sizeof(binlog_unsafe_warning_flags) * CHAR_BIT);
  
  if (!(unsafe_type_flags= binlog_unsafe_warning_flags))
    DBUG_VOID_RETURN;                           // Nothing to do

  /*
    For each unsafe_type, check if the statement is unsafe in this way
    and issue a warning.
  */
  for (int unsafe_type=0;
       unsafe_type < LEX::BINLOG_STMT_UNSAFE_COUNT;
       unsafe_type++)
  {
    if ((unsafe_type_flags & (1 << unsafe_type)) != 0)
    {
      push_warning_printf(this, Sql_condition::WARN_LEVEL_NOTE,
                          ER_BINLOG_UNSAFE_STATEMENT,
                          ER_THD(this, ER_BINLOG_UNSAFE_STATEMENT),
                          ER_THD(this, LEX::binlog_stmt_unsafe_errcode[unsafe_type]));
      if (global_system_variables.log_warnings > 0 &&
          !protect_against_unsafe_warning_flood(unsafe_type))
        print_unsafe_warning_to_log(this, unsafe_type, buf, query());
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Log the current query.

  The query will be logged in either row format or statement format
  depending on the value of @c current_stmt_binlog_format_row field and
  the value of the @c qtype parameter.

  This function must be called:

  - After the all calls to ha_*_row() functions have been issued.

  - After any writes to system tables. Rationale: if system tables
    were written after a call to this function, and the master crashes
    after the call to this function and before writing the system
    tables, then the master and slave get out of sync.

  - Before tables are unlocked and closed.

  @see decide_logging_format

  @retval < 0 No logging of query (ok)
  @retval 0 Success
  @retval > 0  If there is a failure when writing the query (e.g.,
               write failure), then the error code is returned.
*/

int THD::binlog_query(THD::enum_binlog_query_type qtype, char const *query_arg,
                      ulong query_len, bool is_trans, bool direct, 
                      bool suppress_use, int errcode)
{
  DBUG_ENTER("THD::binlog_query");
  DBUG_PRINT("enter", ("qtype: %s  query: '%-.*s'",
                       show_query_type(qtype), (int) query_len, query_arg));

  DBUG_ASSERT(query_arg);
  DBUG_ASSERT(WSREP_EMULATE_BINLOG_NNULL(this) || mysql_bin_log.is_open());

  /* If this is withing a BEGIN ... COMMIT group, don't log it */
  if (variables.option_bits & OPTION_GTID_BEGIN)
  {
    direct= 0;
    is_trans= 1;
  }
  DBUG_PRINT("info", ("is_trans: %d  direct: %d", is_trans, direct));

  if (get_binlog_local_stmt_filter() == BINLOG_FILTER_SET)
  {
    /*
      The current statement is to be ignored, and not written to
      the binlog. Do not call issue_unsafe_warnings().
    */
    DBUG_RETURN(-1);
  }

  /*
    If we are not in prelocked mode, mysql_unlock_tables() will be
    called after this binlog_query(), so we have to flush the pending
    rows event with the STMT_END_F set to unlock all tables at the
    slave side as well.

    If we are in prelocked mode, the flushing will be done inside the
    top-most close_thread_tables().
  */
  if (this->locked_tables_mode <= LTM_LOCK_TABLES)
  {
    int error;
    if (unlikely(error= binlog_flush_pending_rows_event(TRUE, is_trans)))
    {
      DBUG_ASSERT(error > 0);
      DBUG_RETURN(error);
    }
  }

  /*
    Warnings for unsafe statements logged in statement format are
    printed in three places instead of in decide_logging_format().
    This is because the warnings should be printed only if the statement
    is actually logged. When executing decide_logging_format(), we cannot
    know for sure if the statement will be logged:

    1 - sp_head::execute_procedure which prints out warnings for calls to
    stored procedures.

    2 - sp_head::execute_function which prints out warnings for calls
    involving functions.

    3 - THD::binlog_query (here) which prints warning for top level
    statements not covered by the two cases above: i.e., if not insided a
    procedure and a function.

    Besides, we should not try to print these warnings if it is not
    possible to write statements to the binary log as it happens when
    the execution is inside a function, or generaly speaking, when
    the variables.option_bits & OPTION_BIN_LOG is false.
    
  */
  if ((variables.option_bits & OPTION_BIN_LOG) &&
      spcont == NULL && !binlog_evt_union.do_union)
    issue_unsafe_warnings();

  switch (qtype) {
    /*
      ROW_QUERY_TYPE means that the statement may be logged either in
      row format or in statement format.  If
      current_stmt_binlog_format is row, it means that the
      statement has already been logged in row format and hence shall
      not be logged again.
    */
  case THD::ROW_QUERY_TYPE:
    DBUG_PRINT("debug",
               ("is_current_stmt_binlog_format_row: %d",
                is_current_stmt_binlog_format_row()));
    if (is_current_stmt_binlog_format_row())
      DBUG_RETURN(-1);
    /* Fall through */

    /*
      STMT_QUERY_TYPE means that the query must be logged in statement
      format; it cannot be logged in row format.  This is typically
      used by DDL statements.  It is an error to use this query type
      if current_stmt_binlog_format_row is row.

      @todo Currently there are places that call this method with
      STMT_QUERY_TYPE and current_stmt_binlog_format is row.  Fix those
      places and add assert to ensure correct behavior. /Sven
    */
  case THD::STMT_QUERY_TYPE:
    /*
      The MYSQL_LOG::write() function will set the STMT_END_F flag and
      flush the pending rows event if necessary.
    */
    {
      int error = 0;

      /*
        Binlog table maps will be irrelevant after a Query_log_event
        (they are just removed on the slave side) so after the query
        log event is written to the binary log, we pretend that no
        table maps were written.
      */
      if (binlog_should_compress(query_len))
      {
        Query_compressed_log_event qinfo(this, query_arg, query_len, is_trans,
                                         direct, suppress_use, errcode);
        error= mysql_bin_log.write(&qinfo);
      }
      else
      {
        Query_log_event qinfo(this, query_arg, query_len, is_trans, direct,
                              suppress_use, errcode);
        error= mysql_bin_log.write(&qinfo);
      }
      /*
        row logged binlog may not have been reset in the case of locked tables
      */
      reset_binlog_for_next_statement();

      DBUG_RETURN(error >= 0 ? error : 1);
    }

  case THD::QUERY_TYPE_COUNT:
  default:
    DBUG_ASSERT(qtype < QUERY_TYPE_COUNT);
  }
  DBUG_RETURN(0);
}


/**
  Binlog current query as a statement, ignoring the binlog filter setting.

  The filter is in decide_logging_format() to mark queries to not be stored
  in the binary log, for example by a shared distributed engine like S3.
  This function resets the filter to ensure the the query is logged if
  the binlog is active.

  Note that 'direct' is set to false, which means that the query will
  not be directly written to the binary log but instead to the cache.

  @retval false   ok
  @retval true    error
*/


bool THD::binlog_current_query_unfiltered()
{
  if (!mysql_bin_log.is_open())
    return 0;

  reset_binlog_local_stmt_filter();
  clear_binlog_local_stmt_filter();
  return binlog_query(THD::STMT_QUERY_TYPE, query(), query_length(),
                      /* is_trans */     FALSE,
                      /* direct */       FALSE,
                      /* suppress_use */ FALSE,
                      /* Error */        0) > 0;
}


void
THD::wait_for_wakeup_ready()
{
  mysql_mutex_lock(&LOCK_wakeup_ready);
  while (!wakeup_ready)
    mysql_cond_wait(&COND_wakeup_ready, &LOCK_wakeup_ready);
  mysql_mutex_unlock(&LOCK_wakeup_ready);
}

void
THD::signal_wakeup_ready()
{
  mysql_mutex_lock(&LOCK_wakeup_ready);
  wakeup_ready= true;
  mysql_mutex_unlock(&LOCK_wakeup_ready);
  mysql_cond_signal(&COND_wakeup_ready);
}

void THD::set_last_commit_gtid(rpl_gtid &gtid)
{
#ifndef EMBEDDED_LIBRARY
  bool changed_gtid= (m_last_commit_gtid.seq_no != gtid.seq_no);
#endif
  m_last_commit_gtid= gtid;
#ifndef EMBEDDED_LIBRARY
  if (changed_gtid)
  {
    DBUG_ASSERT(current_thd == this);
    session_tracker.sysvars.mark_as_changed(this, Sys_last_gtid_ptr);
  }
#endif
}

void
wait_for_commit::reinit()
{
  subsequent_commits_list= NULL;
  next_subsequent_commit= NULL;
  waitee.store(NULL, std::memory_order_relaxed);
  opaque_pointer= NULL;
  wakeup_error= 0;
  wakeup_subsequent_commits_running= false;
  commit_started= false;
#ifdef SAFE_MUTEX
  /*
    When using SAFE_MUTEX, the ordering between taking the LOCK_wait_commit
    mutexes is checked. This causes a problem when we re-use a mutex, as then
    the expected locking order may change.

    So in this case, do a re-init of the mutex. In release builds, we want to
    avoid the overhead of a re-init though.

    To ensure that no one is locking the mutex, we take a lock of it first.
    For full explanation, see wait_for_commit::~wait_for_commit()
  */
  mysql_mutex_lock(&LOCK_wait_commit);
  mysql_mutex_unlock(&LOCK_wait_commit);

  mysql_mutex_destroy(&LOCK_wait_commit);
  mysql_mutex_init(key_LOCK_wait_commit, &LOCK_wait_commit, MY_MUTEX_INIT_FAST);
#endif
}


wait_for_commit::wait_for_commit()
{
  mysql_mutex_init(key_LOCK_wait_commit, &LOCK_wait_commit, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wait_commit, &COND_wait_commit, 0);
  reinit();
}


wait_for_commit::~wait_for_commit()
{
  /*
    Since we do a dirty read of the waiting_for_commit flag in
    wait_for_prior_commit() and in unregister_wait_for_prior_commit(), we need
    to take extra care before freeing the wait_for_commit object.

    It is possible for the waitee to be pre-empted inside wakeup(), just after
    it has cleared the waiting_for_commit flag and before it has released the
    LOCK_wait_commit mutex. And then it is possible for the waiter to find the
    flag cleared in wait_for_prior_commit() and go finish up things and
    de-allocate the LOCK_wait_commit and COND_wait_commit objects before the
    waitee has time to be re-scheduled and finish unlocking the mutex and
    signalling the condition. This would lead to the waitee accessing no
    longer valid memory.

    To prevent this, we do an extra lock/unlock of the mutex here before
    deallocation; this makes certain that any waitee has completed wakeup()
    first.
  */
  mysql_mutex_lock(&LOCK_wait_commit);
  mysql_mutex_unlock(&LOCK_wait_commit);

  mysql_mutex_destroy(&LOCK_wait_commit);
  mysql_cond_destroy(&COND_wait_commit);
}


void
wait_for_commit::wakeup(int wakeup_error)
{
  /*
    We signal each waiter on their own condition and mutex (rather than using
    pthread_cond_broadcast() or something like that).

    Otherwise we would need to somehow ensure that they were done
    waking up before we could allow this THD to be destroyed, which would
    be annoying and unnecessary.

    Note that wakeup_subsequent_commits2() depends on this function being a
    full memory barrier (it is, because it takes a mutex lock).

  */
  mysql_mutex_lock(&LOCK_wait_commit);
  this->wakeup_error= wakeup_error;
  /* Memory barrier to make wakeup_error visible to the waiter thread. */
  waitee.store(NULL, std::memory_order_release);
  /*
    Note that it is critical that the mysql_cond_signal() here is done while
    still holding the mutex. As soon as we release the mutex, the waiter might
    deallocate the condition object.
  */
  mysql_cond_signal(&COND_wait_commit);
  mysql_mutex_unlock(&LOCK_wait_commit);
}


/*
  Register that the next commit of this THD should wait to complete until
  commit in another THD (the waitee) has completed.

  The wait may occur explicitly, with the waiter sitting in
  wait_for_prior_commit() until the waitee calls wakeup_subsequent_commits().

  Alternatively, the TC (eg. binlog) may do the commits of both waitee and
  waiter at once during group commit, resolving both of them in the right
  order.

  Only one waitee can be registered for a waiter; it must be removed by
  wait_for_prior_commit() or unregister_wait_for_prior_commit() before a new
  one is registered. But it is ok for several waiters to register a wait for
  the same waitee. It is also permissible for one THD to be both a waiter and
  a waitee at the same time.
*/
void
wait_for_commit::register_wait_for_prior_commit(wait_for_commit *waitee)
{
  DBUG_ASSERT(!this->waitee.load(std::memory_order_relaxed)
              /* No prior registration allowed */);
  wakeup_error= 0;
  this->waitee.store(waitee, std::memory_order_relaxed);

  mysql_mutex_lock(&waitee->LOCK_wait_commit);
  /*
    If waitee is in the middle of wakeup, then there is nothing to wait for,
    so we need not register. This is necessary to avoid a race in unregister,
    see comments on wakeup_subsequent_commits2() for details.
  */
  if (waitee->wakeup_subsequent_commits_running)
    this->waitee.store(NULL, std::memory_order_relaxed);
  else
  {
    /*
      Put ourself at the head of the waitee's list of transactions that must
      wait for it to commit first.
     */
    this->next_subsequent_commit= waitee->subsequent_commits_list;
    waitee->subsequent_commits_list= this;
  }
  mysql_mutex_unlock(&waitee->LOCK_wait_commit);
}


/**
  Waits for commit of another transaction to complete, as already registered
  with register_wait_for_prior_commit(). If the commit already completed,
  returns immediately.

  If thd->backup_commit_lock is set, release it while waiting for other threads
*/

int
wait_for_commit::wait_for_prior_commit2(THD *thd)
{
  PSI_stage_info old_stage;
  wait_for_commit *loc_waitee;
  bool backup_lock_released= 0;

  /*
    Release MDL_BACKUP_COMMIT LOCK while waiting for other threads to commit
    This is needed to avoid deadlock between the other threads (which not
    yet have the MDL_BACKUP_COMMIT_LOCK) and any threads using
    BACKUP LOCK BLOCK_COMMIT.
  */
  if (thd->backup_commit_lock && thd->backup_commit_lock->ticket)
  {
    backup_lock_released= 1;
    thd->mdl_context.release_lock(thd->backup_commit_lock->ticket);
    thd->backup_commit_lock->ticket= 0;
  }

  mysql_mutex_lock(&LOCK_wait_commit);
  DEBUG_SYNC(thd, "wait_for_prior_commit_waiting");
  thd->ENTER_COND(&COND_wait_commit, &LOCK_wait_commit,
                  &stage_waiting_for_prior_transaction_to_commit,
                  &old_stage);
  while ((loc_waitee= this->waitee.load(std::memory_order_relaxed)) &&
         likely(!thd->check_killed(1)))
    mysql_cond_wait(&COND_wait_commit, &LOCK_wait_commit);
  if (!loc_waitee)
  {
    if (wakeup_error)
      my_error(ER_PRIOR_COMMIT_FAILED, MYF(0));
    goto end;
  }
  /*
    Wait was interrupted by kill. We need to unregister our wait and give the
    error. But if a wakeup is already in progress, then we must ignore the
    kill and not give error, otherwise we get inconsistency between waitee and
    waiter as to whether we succeed or fail (eg. we may roll back but waitee
    might attempt to commit both us and any subsequent commits waiting for us).
  */
  mysql_mutex_lock(&loc_waitee->LOCK_wait_commit);
  if (loc_waitee->wakeup_subsequent_commits_running)
  {
    /* We are being woken up; ignore the kill and just wait. */
    mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
    do
    {
      mysql_cond_wait(&COND_wait_commit, &LOCK_wait_commit);
    } while (this->waitee.load(std::memory_order_relaxed));
    if (wakeup_error)
      my_error(ER_PRIOR_COMMIT_FAILED, MYF(0));
    goto end;
  }
  remove_from_list(&loc_waitee->subsequent_commits_list);
  mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
  this->waitee.store(NULL, std::memory_order_relaxed);

  wakeup_error= thd->killed_errno();
  if (!wakeup_error)
    wakeup_error= ER_QUERY_INTERRUPTED;
  my_message(wakeup_error, ER_THD(thd, wakeup_error), MYF(0));
  thd->EXIT_COND(&old_stage);
  /*
    Must do the DEBUG_SYNC() _after_ exit_cond(), as DEBUG_SYNC is not safe to
    use within enter_cond/exit_cond.
  */
  DEBUG_SYNC(thd, "wait_for_prior_commit_killed");
  if (backup_lock_released)
    thd->mdl_context.acquire_lock(thd->backup_commit_lock,
                                  thd->variables.lock_wait_timeout);
  return wakeup_error;

end:
  thd->EXIT_COND(&old_stage);
  if (backup_lock_released)
    thd->mdl_context.acquire_lock(thd->backup_commit_lock,
                                  thd->variables.lock_wait_timeout);
  return wakeup_error;
}


/*
  Wakeup anyone waiting for us to have committed.

  Note about locking:

  We have a potential race or deadlock between wakeup_subsequent_commits() in
  the waitee and unregister_wait_for_prior_commit() in the waiter.

  Both waiter and waitee needs to take their own lock before it is safe to take
  a lock on the other party - else the other party might disappear and invalid
  memory data could be accessed. But if we take the two locks in different
  order, we may end up in a deadlock.

  The waiter needs to lock the waitee to delete itself from the list in
  unregister_wait_for_prior_commit(). Thus wakeup_subsequent_commits() can not
  hold its own lock while locking waiters, as this could lead to deadlock.

  So we need to prevent unregister_wait_for_prior_commit() running while wakeup
  is in progress - otherwise the unregister could complete before the wakeup,
  leading to incorrect spurious wakeup or accessing invalid memory.

  However, if we are in the middle of running wakeup_subsequent_commits(), then
  there is no need for unregister_wait_for_prior_commit() in the first place -
  the waiter can just do a normal wait_for_prior_commit(), as it will be
  immediately woken up.

  So the solution to the potential race/deadlock is to set a flag in the waitee
  that wakeup_subsequent_commits() is in progress. When this flag is set,
  unregister_wait_for_prior_commit() becomes just wait_for_prior_commit().

  Then also register_wait_for_prior_commit() needs to check if
  wakeup_subsequent_commits() is running, and skip the registration if
  so. This is needed in case a new waiter manages to register itself and
  immediately try to unregister while wakeup_subsequent_commits() is
  running. Else the new waiter would also wait rather than unregister, but it
  would not be woken up until next wakeup, which could be potentially much
  later than necessary.
*/

void
wait_for_commit::wakeup_subsequent_commits2(int wakeup_error)
{
  wait_for_commit *waiter;

  mysql_mutex_lock(&LOCK_wait_commit);
  wakeup_subsequent_commits_running= true;
  waiter= subsequent_commits_list;
  subsequent_commits_list= NULL;
  mysql_mutex_unlock(&LOCK_wait_commit);

  while (waiter)
  {
    /*
      Important: we must grab the next pointer before waking up the waiter;
      once the wakeup is done, the field could be invalidated at any time.
    */
    wait_for_commit *next= waiter->next_subsequent_commit;
    waiter->wakeup(wakeup_error);
    waiter= next;
  }

  /*
    We need a full memory barrier between walking the list above, and clearing
    the flag wakeup_subsequent_commits_running below. This barrier is needed
    to ensure that no other thread will start to modify the list pointers
    before we are done traversing the list.

    But wait_for_commit::wakeup() does a full memory barrier already (it locks
    a mutex), so no extra explicit barrier is needed here.
  */
  wakeup_subsequent_commits_running= false;
  DBUG_EXECUTE_IF("inject_wakeup_subsequent_commits_sleep", my_sleep(21000););
}


/* Cancel a previously registered wait for another THD to commit before us. */
void
wait_for_commit::unregister_wait_for_prior_commit2()
{
  wait_for_commit *loc_waitee;

  mysql_mutex_lock(&LOCK_wait_commit);
  if ((loc_waitee= this->waitee.load(std::memory_order_relaxed)))
  {
    mysql_mutex_lock(&loc_waitee->LOCK_wait_commit);
    if (loc_waitee->wakeup_subsequent_commits_running)
    {
      /*
        When a wakeup is running, we cannot safely remove ourselves from the
        list without corrupting it. Instead we can just wait, as wakeup is
        already in progress and will thus be immediate.

        See comments on wakeup_subsequent_commits2() for more details.
      */
      mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
      while (this->waitee.load(std::memory_order_relaxed))
        mysql_cond_wait(&COND_wait_commit, &LOCK_wait_commit);
    }
    else
    {
      /* Remove ourselves from the list in the waitee. */
      remove_from_list(&loc_waitee->subsequent_commits_list);
      mysql_mutex_unlock(&loc_waitee->LOCK_wait_commit);
      this->waitee.store(NULL, std::memory_order_relaxed);
    }
  }
  wakeup_error= 0;
  mysql_mutex_unlock(&LOCK_wait_commit);
}


bool Discrete_intervals_list::append(ulonglong start, ulonglong val,
                                 ulonglong incr)
{
  DBUG_ENTER("Discrete_intervals_list::append");
  /* first, see if this can be merged with previous */
  if ((head == NULL) || tail->merge_if_contiguous(start, val, incr))
  {
    /* it cannot, so need to add a new interval */
    Discrete_interval *new_interval= new Discrete_interval(start, val, incr);
    DBUG_RETURN(append(new_interval));
  }
  DBUG_RETURN(0);
}

bool Discrete_intervals_list::append(Discrete_interval *new_interval)
{
  DBUG_ENTER("Discrete_intervals_list::append");
  if (unlikely(new_interval == NULL))
    DBUG_RETURN(1);
  DBUG_PRINT("info",("adding new auto_increment interval"));
  if (head == NULL)
    head= current= new_interval;
  else
    tail->next= new_interval;
  tail= new_interval;
  elements++;
  DBUG_RETURN(0);
}


void AUTHID::copy(MEM_ROOT *mem_root, const LEX_CSTRING *user_name,
                                      const LEX_CSTRING *host_name)
{
  user.str= strmake_root(mem_root, user_name->str, user_name->length);
  user.length= user_name->length;

  host.str= strmake_root(mem_root, host_name->str, host_name->length);
  host.length= host_name->length;
}


/*
  Set from a string in 'user@host' format.
  This method resebmles parse_user(),
  but does not need temporary buffers.
*/
void AUTHID::parse(const char *str, size_t length)
{
  const char *p= strrchr(str, '@');
  if (!p)
  {
    user.str= str;
    user.length= length;
    host= null_clex_str;
  }
  else
  {
    user.str= str;
    user.length= (size_t) (p - str);
    host.str= p + 1;
    host.length= (size_t) (length - user.length - 1);
    if (user.length && !host.length)
      host= host_not_specified; // 'user@' -> 'user@%'
  }
  if (user.length > USERNAME_LENGTH)
    user.length= USERNAME_LENGTH;
  if (host.length > HOSTNAME_LENGTH)
    host.length= HOSTNAME_LENGTH;
}


void Database_qualified_name::copy(MEM_ROOT *mem_root,
                                   const LEX_CSTRING &db,
                                   const LEX_CSTRING &name)
{
  m_db.length= db.length;
  m_db.str= strmake_root(mem_root, db.str, db.length);
  m_name.length= name.length;
  m_name.str= strmake_root(mem_root, name.str, name.length);
}


bool Table_ident::append_to(THD *thd, String *str) const
{
  return (db.length &&
          (append_identifier(thd, str, db.str, db.length) ||
           str->append('.'))) ||
         append_identifier(thd, str, table.str, table.length);
}


bool Qualified_column_ident::append_to(THD *thd, String *str) const
{
  return Table_ident::append_to(thd, str) || str->append('.') ||
         append_identifier(thd, str, m_column.str, m_column.length);
}


#endif /* !defined(MYSQL_CLIENT) */


Query_arena_stmt::Query_arena_stmt(THD *_thd) :
  thd(_thd)
{
  arena= thd->activate_stmt_arena_if_needed(&backup);
}

Query_arena_stmt::~Query_arena_stmt()
{
  if (arena)
    thd->restore_active_arena(arena, &backup);
}


bool THD::timestamp_to_TIME(MYSQL_TIME *ltime, my_time_t ts,
                            ulong sec_part, date_mode_t fuzzydate)
{
  time_zone_used= 1;
  if (ts == 0 && sec_part == 0)
  {
    if (fuzzydate & TIME_NO_ZERO_DATE)
      return 1;
    set_zero_time(ltime, MYSQL_TIMESTAMP_DATETIME);
  }
  else
  {
    variables.time_zone->gmt_sec_to_TIME(ltime, ts);
    ltime->second_part= sec_part;
  }
  return 0;
}

THD_list_iterator *THD_list_iterator::iterator()
{
  return &server_threads;
}
