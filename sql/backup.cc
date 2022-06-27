/* Copyright (c) 2018, 2022, MariaDB Corporation.
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

/*
  Implementation of BACKUP STAGE, an interface for external backup tools.

  TODO:
  - At backup_start() we call ha_prepare_for_backup() for all active
    storage engines.  If someone tries to load a new storage engine
    that requires prepare_for_backup() for it to work, that storage
    engines has to be blocked from loading until backup finishes.
    As we currently don't have any loadable storage engine that
    requires this and we have not implemented that part.
    This can easily be done by adding a
    PLUGIN_CANT_BE_LOADED_WHILE_BACKUP_IS_RUNNING flag to
    maria_declare_plugin and check this before calling
    plugin_initialize()
*/

#include "mariadb.h"
#include "sql_class.h"
#include "sql_base.h"                           // flush_tables
#include "sql_insert.h"                         // kill_delayed_threads
#include "sql_handler.h"                        // mysql_ha_cleanup_no_free
#include <my_sys.h>
#include <strfunc.h>                           // strconvert()
#include "wsrep_mysqld.h"

static const char *stage_names[]=
{"START", "FLUSH", "BLOCK_DDL", "BLOCK_COMMIT", "END", 0};

TYPELIB backup_stage_names=
{ array_elements(stage_names)-1, "", stage_names, 0 };

static MDL_ticket *backup_flush_ticket;
static File volatile backup_log= -1;
static int backup_log_error= 0;

static bool backup_start(THD *thd);
static bool backup_flush(THD *thd);
static bool backup_block_ddl(THD *thd);
static bool backup_block_commit(THD *thd);
static bool start_ddl_logging();
static void stop_ddl_logging();

/**
  Run next stage of backup
*/

void backup_init()
{
  backup_flush_ticket= 0;
  backup_log= -1;
  backup_log_error= 0;
}

bool run_backup_stage(THD *thd, backup_stages stage)
{
  backup_stages next_stage;
  DBUG_ENTER("run_backup_stage");

  if (thd->current_backup_stage == BACKUP_FINISHED)
  {
    if (stage != BACKUP_START)
    {
      my_error(ER_BACKUP_NOT_RUNNING, MYF(0));
      DBUG_RETURN(1);
    }
    next_stage= BACKUP_START;
  }
  else
  {
    if ((uint) thd->current_backup_stage >= (uint) stage)
    {
      my_error(ER_BACKUP_WRONG_STAGE, MYF(0), stage_names[stage],
               stage_names[thd->current_backup_stage]);
      DBUG_RETURN(1);
    }
    if (stage == BACKUP_END)
    {
      /*
        If end is given, jump directly to stage end. This is to allow one
        to abort backup quickly.
      */
      next_stage= stage;
    }
    else
    {
      /* Go trough all not used stages until we reach 'stage' */
      next_stage= (backup_stages) ((uint) thd->current_backup_stage + 1);
    }
  }

  do
  {
    bool res= false;
    backup_stages previous_stage= thd->current_backup_stage;
    thd->current_backup_stage= next_stage;
    switch (next_stage) {
    case BACKUP_START:
      if (!(res= backup_start(thd)))
        break;
      /* Reset backup stage to start for next backup try */
      previous_stage= BACKUP_FINISHED;
      break;
    case BACKUP_FLUSH:
      res= backup_flush(thd);
      break;
    case BACKUP_WAIT_FOR_FLUSH:
      res= backup_block_ddl(thd);
      break;
    case BACKUP_LOCK_COMMIT:
      res= backup_block_commit(thd);
      break;
    case BACKUP_END:
      res= backup_end(thd);
      break;
    case BACKUP_FINISHED:
      DBUG_ASSERT(0);
    }
    if (res)
    {
      thd->current_backup_stage= previous_stage;
      my_error(ER_BACKUP_STAGE_FAILED, MYF(0), stage_names[(uint) stage]);
      DBUG_RETURN(1);
    }
    next_stage= (backup_stages) ((uint) next_stage + 1);
  } while ((uint) next_stage <= (uint) stage);

  DBUG_RETURN(0);
}


/**
  Start the backup

  - Wait for previous backup to stop running
  - Start service to log changed tables (TODO)
  - Block purge of redo files (Required at least for Aria)
  - An handler can optionally do a checkpoint of all tables,
    to speed up the recovery stage of the backup.
*/

static bool backup_start(THD *thd)
{
  MDL_request mdl_request;
  DBUG_ENTER("backup_start");

  thd->current_backup_stage= BACKUP_FINISHED;   // For next test
  if (thd->has_read_only_protection())
    DBUG_RETURN(1);

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(1);
  }

  /* this will be reset if this stage fails */
  thd->current_backup_stage= BACKUP_START;

  /*
    Wait for old backup to finish and block ddl's so that we can start the
    ddl logger
  */
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_BLOCK_DDL,
                   MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

  if (start_ddl_logging())
  {
    thd->mdl_context.release_lock(mdl_request.ticket);
    DBUG_RETURN(1);
  }

  DBUG_ASSERT(backup_flush_ticket == 0);
  backup_flush_ticket= mdl_request.ticket;

  /* Downgrade lock to only block other backups */
  backup_flush_ticket->downgrade_lock(MDL_BACKUP_START);

  ha_prepare_for_backup();
  DBUG_RETURN(0);
}

/**
   backup_flush()

   - FLUSH all changes for not active non transactional tables, except
     for statistics and log tables. Close the tables, to ensure they
     are marked as closed after backup.

   - BLOCK all NEW write locks for all non transactional tables
     (except statistics and log tables).  Already granted locks are
     not affected (Running statements with non transaction tables will
     continue running).

   - The following DDL's doesn't have to be blocked as they can't set
     the table in a non consistent state:
     CREATE, RENAME, DROP
*/

static bool backup_flush(THD *thd)
{
  DBUG_ENTER("backup_flush");
  /*
    Lock all non transactional normal tables to be used in new DML's
  */
  if (thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                           MDL_BACKUP_FLUSH,
                                           thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

  /*
    Free unused tables and table shares so that mariabackup knows what
    is safe to copy
  */
  tc_purge();
  tdc_purge(true);

  DBUG_RETURN(0);
}

/**
  backup_block_ddl()

  - Kill all insert delay handlers, to ensure that all non transactional
    tables are closed (can be improved in the future).

  - Close handlers as other threads may wait for these, which can cause
    deadlocks.

  - Wait for all statements using write locked non-transactional tables to end.

  - Mark all not used active non transactional tables (except
    statistics and log tables) to be closed with
    handler->extra(HA_EXTRA_FLUSH)

  - Block TRUNCATE TABLE, CREATE TABLE, DROP TABLE and RENAME
    TABLE. Block also start of a new ALTER TABLE and the final rename
    phase of ALTER TABLE.  Running ALTER TABLES are not blocked.  Both normal
    and inline ALTER TABLE'S should be blocked when copying is completed but
    before final renaming of the tables / new table is activated.
    This will probably require a callback from the InnoDB code.
*/

/* Retry to get inital lock for 0.1 + 0.5 + 2.25 + 11.25 + 56.25 = 70.35 sec */
#define MAX_RETRY_COUNT 5

static bool backup_block_ddl(THD *thd)
{
  PSI_stage_info org_stage;
  uint sleep_time;
  DBUG_ENTER("backup_block_ddl");

  kill_delayed_threads();
  mysql_ha_cleanup_no_free(thd);

  thd->backup_stage(&org_stage);
  THD_STAGE_INFO(thd, stage_waiting_for_flush);
  /* Wait until all non trans statements has ended */
  if (thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                           MDL_BACKUP_WAIT_FLUSH,
                                           thd->variables.lock_wait_timeout))
    goto err;

  /*
    Remove not used tables from the table share.  Flush all changes to
    non transaction tables and mark those that are not in use in write
    operations as closed. From backup purposes it's not critical if
    flush_tables() returns an error. It's ok to continue with next
    backup stage even if we got an error.
  */
  (void) flush_tables(thd, FLUSH_NON_TRANS_TABLES);
  thd->clear_error();

#ifdef WITH_WSREP
  /*
    We desync the node for BACKUP STAGE because applier threads
    bypass backup MDL locks (see MDL_lock::can_grant_lock)
  */
  if (WSREP_NNULL(thd))
  {
    Wsrep_server_state &server_state= Wsrep_server_state::instance();
    if (server_state.desync_and_pause().is_undefined()) {
      DBUG_RETURN(1);
    }
    thd->wsrep_desynced_backup_stage= true;
  }
#endif /* WITH_WSREP */

  /*
    block new DDL's, in addition to all previous blocks
    We didn't do this lock above, as we wanted DDL's to be executed while
    we wait for non transactional tables (which may take a while).

    We do this lock in a loop as we can get a deadlock if there are multi-object
    ddl statements like
    RENAME TABLE t1 TO t2, t3 TO t3
    and the MDL happens in the middle of it.
 */
  THD_STAGE_INFO(thd, stage_waiting_for_ddl);
  sleep_time= 100;                              // Start with 0.1 seconds
  for (uint i= 0 ; i <= MAX_RETRY_COUNT ; i++)
  {
    if (!thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                              MDL_BACKUP_WAIT_DDL,
                                              thd->variables.lock_wait_timeout))
      break;
    if (thd->get_stmt_da()->sql_errno() != ER_LOCK_DEADLOCK || thd->killed ||
        i == MAX_RETRY_COUNT)
    {
      /*
        Could be a timeout. Downgrade lock to what is was before this function
        was called so that this function can be called again
      */
      backup_flush_ticket->downgrade_lock(MDL_BACKUP_FLUSH);
      goto err;
    }
    thd->clear_error();                         // Forget the DEADLOCK error
    my_sleep(sleep_time);
    sleep_time*= 5;                             // Wait a bit longer next time
  }

  /* There can't be anything more that needs to be logged to ddl log */
  THD_STAGE_INFO(thd, org_stage);
  stop_ddl_logging();
  DBUG_RETURN(0);
err:
  THD_STAGE_INFO(thd, org_stage);
  DBUG_RETURN(1);
}


/**
   backup_block_commit()

   Block commits, writes to log and statistics tables and binary log
*/

static bool backup_block_commit(THD *thd)
{
  DBUG_ENTER("backup_block_commit");
  if (thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                           MDL_BACKUP_WAIT_COMMIT,
                                           thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

  /* We can ignore errors from flush_tables () */
  (void) flush_tables(thd, FLUSH_SYS_TABLES);

  if (mysql_bin_log.is_open())
  {
    mysql_mutex_lock(mysql_bin_log.get_log_lock());
    mysql_file_sync(mysql_bin_log.get_log_file()->file,
                    MYF(MY_WME|MY_SYNC_FILESIZE));
    mysql_mutex_unlock(mysql_bin_log.get_log_lock());
  }
  thd->clear_error();

  DBUG_RETURN(0);
}


/**
   backup_end()

   Safe to run, even if backup has not been run by this thread.
   This is for example the case when a THD ends.
*/

bool backup_end(THD *thd)
{
  DBUG_ENTER("backup_end");

  if (thd->current_backup_stage != BACKUP_FINISHED)
  {
    DBUG_ASSERT(backup_flush_ticket);
    MDL_ticket *old_ticket= backup_flush_ticket;
    ha_end_backup();
    // This is needed as we may call backup_end without backup_block_commit
    stop_ddl_logging();
    backup_flush_ticket= 0;
    thd->current_backup_stage= BACKUP_FINISHED;
    thd->mdl_context.release_lock(old_ticket);
#ifdef WITH_WSREP
    if (WSREP_NNULL(thd) && thd->wsrep_desynced_backup_stage)
    {
      Wsrep_server_state &server_state= Wsrep_server_state::instance();
      THD_STAGE_INFO(thd, stage_waiting_flow);
      WSREP_DEBUG("backup_end: waiting for flow control for %s",
                  wsrep_thd_query(thd));
      server_state.resume_and_resync();
      thd->wsrep_desynced_backup_stage= false;
    }
#endif /* WITH_WSREP */
  }
  DBUG_RETURN(0);
}


/**
   backup_set_alter_copy_lock()

   @param thd
   @param table  From table that is part of ALTER TABLE. This is only used
                 for the assert to ensure we use this function correctly.

   Downgrades the MDL_BACKUP_DDL lock to MDL_BACKUP_ALTER_COPY to allow
   copy of altered table to proceed under MDL_BACKUP_WAIT_DDL

   Note that in some case when using non transactional tables,
   the lock may be of type MDL_BACKUP_DML.
*/

void backup_set_alter_copy_lock(THD *thd, TABLE *table)
{
  MDL_ticket *ticket= thd->mdl_backup_ticket;

  /* Ticket maybe NULL in case of LOCK TABLES or for temporary tables*/
  DBUG_ASSERT(ticket || thd->locked_tables_mode ||
              table->s->tmp_table != NO_TMP_TABLE);
  if (ticket)
    ticket->downgrade_lock(MDL_BACKUP_ALTER_COPY);
}

/**
   backup_reset_alter_copy_lock

   Upgrade the lock of the original ALTER table MDL_BACKUP_DDL
   Can fail if MDL lock was killed
*/

bool backup_reset_alter_copy_lock(THD *thd)
{
  bool res= 0;
  MDL_ticket *ticket= thd->mdl_backup_ticket;

  /* Ticket maybe NULL in case of LOCK TABLES or for temporary tables*/
  if (ticket)
    res= thd->mdl_context.upgrade_shared_lock(ticket, MDL_BACKUP_DDL,
                                              thd->variables.lock_wait_timeout);
  return res;
}


/*****************************************************************************
 Interfaces for BACKUP LOCK
 These functions are used by maria_backup to ensure that there are no active
 ddl's on the object the backup is going to copy
*****************************************************************************/


bool backup_lock(THD *thd, TABLE_LIST *table)
{
  /* We should leave the previous table unlocked in case of errors */
  backup_unlock(thd);
  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    return 1;
  }
  table->mdl_request.duration= MDL_EXPLICIT;
  if (thd->mdl_context.acquire_lock(&table->mdl_request,
                                    thd->variables.lock_wait_timeout))
    return 1;
  thd->mdl_backup_lock= table->mdl_request.ticket;
  return 0;
}


/* Release old backup lock if it exists */

void backup_unlock(THD *thd)
{
  if (thd->mdl_backup_lock)
    thd->mdl_context.release_lock(thd->mdl_backup_lock);
  thd->mdl_backup_lock= 0;
}


/*****************************************************************************
 Logging of ddl statements to backup log
*****************************************************************************/

static bool start_ddl_logging()
{
  char name[FN_REFLEN];
  DBUG_ENTER("start_ddl_logging");

  fn_format(name, "ddl", mysql_data_home, ".log", 0);

  backup_log_error= 0;
  backup_log= mysql_file_create(key_file_log_ddl, name, CREATE_MODE,
                                O_TRUNC | O_WRONLY | O_APPEND | O_NOFOLLOW,
                                MYF(MY_WME));
  DBUG_RETURN(backup_log < 0);
}

static void stop_ddl_logging()
{
  mysql_mutex_lock(&LOCK_backup_log);
  if (backup_log >= 0)
  {
    mysql_file_close(backup_log, MYF(MY_WME));
    backup_log= -1;
  }
  backup_log_error= 0;
  mysql_mutex_unlock(&LOCK_backup_log);
}


static inline char *add_str_to_buffer(char *ptr, const LEX_CSTRING *from)
{
  if (from->length)                           // If length == 0, str may be 0
    memcpy(ptr, from->str, from->length);
  ptr[from->length]= '\t';
  return ptr+ from->length + 1;
}

static char *add_name_to_buffer(char *ptr, const LEX_CSTRING *from)
{
  LEX_CSTRING tmp;
  char buff[NAME_LEN*4];
  uint errors;

  tmp.str= buff;
  tmp.length= strconvert(system_charset_info, from->str, from->length,
                         &my_charset_filename, buff, sizeof(buff), &errors);
  return add_str_to_buffer(ptr, &tmp);
}


static char *add_id_to_buffer(char *ptr, const LEX_CUSTRING *from)
{
  LEX_CSTRING tmp;
  char buff[MY_UUID_STRING_LENGTH];

  if (!from->length)
    return add_str_to_buffer(ptr, (LEX_CSTRING*) from);

  tmp.str= buff;
  tmp.length= MY_UUID_STRING_LENGTH;
  my_uuid2str(from->str, buff, 1);
  return add_str_to_buffer(ptr, &tmp);
}


static char *add_bool_to_buffer(char *ptr, bool value) {
  *(ptr++) = value ? '1' : '0';
  *(ptr++) = '\t';
  return ptr;
}

/*
  Write to backup log

  Sets backup_log_error in case of error.  The backup thread could check this
  to ensure that all logging had succeded
*/

void backup_log_ddl(const backup_log_info *info)
{
  if (backup_log >= 0 && backup_log_error == 0)
  {
    mysql_mutex_lock(&LOCK_backup_log);
    if (backup_log < 0)
    {
      mysql_mutex_unlock(&LOCK_backup_log);
      return;
    }
    /* Enough place for db.table *2 + query + engine_name * 2 + tabs+ uuids */
    char buff[NAME_CHAR_LEN*4+20+40*2+10+MY_UUID_STRING_LENGTH*2], *ptr= buff;
    char timebuff[20];
    struct tm current_time;
    LEX_CSTRING tmp_lex;
    time_t tmp_time= my_time(0);

    localtime_r(&tmp_time, &current_time);
    tmp_lex.str= timebuff;
    tmp_lex.length= snprintf(timebuff, sizeof(timebuff),
                             "%4d-%02d-%02d %2d:%02d:%02d",
                             current_time.tm_year + 1900,
                             current_time.tm_mon+1,
                             current_time.tm_mday,
                             current_time.tm_hour,
                             current_time.tm_min,
                             current_time.tm_sec);
    ptr= add_str_to_buffer(ptr, &tmp_lex);

    ptr= add_str_to_buffer(ptr,  &info->query);
    ptr= add_str_to_buffer(ptr,  &info->org_storage_engine_name);
    ptr= add_bool_to_buffer(ptr, info->org_partitioned);
    ptr= add_name_to_buffer(ptr, &info->org_database);
    ptr= add_name_to_buffer(ptr, &info->org_table);
    ptr= add_id_to_buffer(ptr,   &info->org_table_id);

    /* The following fields are only set in case of rename */
    ptr= add_str_to_buffer(ptr,  &info->new_storage_engine_name);
    ptr= add_bool_to_buffer(ptr, info->new_partitioned);
    ptr= add_name_to_buffer(ptr, &info->new_database);
    ptr= add_name_to_buffer(ptr, &info->new_table);
    ptr= add_id_to_buffer(ptr,   &info->new_table_id);

    ptr[-1]= '\n';                              // Replace last tab with nl
    if (mysql_file_write(backup_log, (uchar*) buff, (size_t) (ptr-buff),
                         MYF(MY_FNABP)))
      backup_log_error= my_errno;
    mysql_mutex_unlock(&LOCK_backup_log);
  }
}
