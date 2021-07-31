/* Copyright (c) 2018, 2020, MariaDB Corporation.
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
#include "wsrep_mysqld.h"

static const char *stage_names[]=
{"START", "FLUSH", "BLOCK_DDL", "BLOCK_COMMIT", "END", 0};

TYPELIB backup_stage_names=
{ array_elements(stage_names)-1, "", stage_names, 0 };

static MDL_ticket *backup_flush_ticket;

static bool backup_start(THD *thd);
static bool backup_flush(THD *thd);
static bool backup_block_ddl(THD *thd);
static bool backup_block_commit(THD *thd);

/**
  Run next stage of backup
*/

void backup_init()
{
  backup_flush_ticket= 0;
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
  thd->current_backup_stage= BACKUP_START;

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(1);
  }

  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_START,
                   MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
                                    thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

  backup_flush_ticket= mdl_request.ticket;

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

  - Close handlers as other threads may wait for these, which can cause deadlocks.

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

static bool backup_block_ddl(THD *thd)
{
  DBUG_ENTER("backup_block_ddl");

  kill_delayed_threads();
  mysql_ha_cleanup_no_free(thd);

  /* Wait until all non trans statements has ended */
  if (thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                           MDL_BACKUP_WAIT_FLUSH,
                                           thd->variables.lock_wait_timeout))
    DBUG_RETURN(1);

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
 */
  if (thd->mdl_context.upgrade_shared_lock(backup_flush_ticket,
                                           MDL_BACKUP_WAIT_DDL,
                                           thd->variables.lock_wait_timeout))
  {
    /*
      Could be a timeout. Downgrade lock to what is was before this function
      was called so that this function can be called again
    */
    backup_flush_ticket->downgrade_lock(MDL_BACKUP_FLUSH);
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
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
    ha_end_backup();
    thd->current_backup_stage= BACKUP_FINISHED;
    thd->mdl_context.release_lock(backup_flush_ticket);
#ifdef WITH_WSREP
    if (WSREP_NNULL(thd) && thd->wsrep_desynced_backup_stage)
    {
      Wsrep_server_state &server_state= Wsrep_server_state::instance();
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
 Backup locks
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
