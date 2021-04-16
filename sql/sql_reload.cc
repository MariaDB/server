/* Copyright (c) 2010, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

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

#include "mariadb.h"
#include "sql_reload.h"
#include "sql_priv.h"
#include "mysqld.h"      // select_errors
#include "sql_class.h"   // THD
#include "sql_acl.h"     // acl_reload
#include "sql_servers.h" // servers_reload
#include "sql_connect.h" // reset_mqh
#include "thread_cache.h"
#include "sql_base.h"    // close_cached_tables
#include "sql_db.h"      // my_dbopt_cleanup
#include "hostname.h"    // hostname_cache_refresh
#include "sql_repl.h"    // reset_master, reset_slave
#include "rpl_mi.h"      // Master_info::data_lock
#include "sql_show.h"
#include "debug_sync.h"
#include "des_key_file.h"
#include "transaction.h"

static void disable_checkpoints(THD *thd);

/**
  Reload/resets privileges and the different caches.

  @param thd Thread handler (can be NULL!)
  @param options What should be reset/reloaded (tables, privileges, slave...)
  @param tables Tables to flush (if any)
  @param write_to_binlog < 0 if there was an error while interacting with the binary log inside
                         reload_acl_and_cache,
                         0 if we should not write to the binary log,
                         > 0 if we can write to the binlog.

               
  @note Depending on 'options', it may be very bad to write the
    query to the binlog (e.g. FLUSH SLAVE); this is a
    pointer where reload_acl_and_cache() will put 0 if
    it thinks we really should not write to the binlog.
    Otherwise it will put 1.

  @return Error status code
    @retval 0 Ok
    @retval !=0  Error; thd->killed is set or thd->is_error() is true
*/

bool reload_acl_and_cache(THD *thd, unsigned long long options,
                          TABLE_LIST *tables, int *write_to_binlog)
{
  bool result=0;
  select_errors=0;				/* Write if more errors */
  int tmp_write_to_binlog= *write_to_binlog= 1;

  DBUG_ASSERT(!thd || !thd->in_sub_stmt);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (options & REFRESH_GRANT)
  {
    THD *tmp_thd= 0;
    /*
      If reload_acl_and_cache() is called from SIGHUP handler we have to
      allocate temporary THD for execution of acl_reload()/grant_reload().
    */
    if (unlikely(!thd) && (thd= (tmp_thd= new THD(0))))
    {
      thd->thread_stack= (char*) &tmp_thd;
      thd->store_globals();
    }

    if (likely(thd))
    {
      bool reload_acl_failed= acl_reload(thd);
      bool reload_grants_failed= grant_reload(thd);
      bool reload_servers_failed= servers_reload(thd);

      if (reload_acl_failed || reload_grants_failed || reload_servers_failed)
      {
        result= 1;
        /*
          When an error is returned, my_message may have not been called and
          the client will hang waiting for a response.
        */
        my_error(ER_UNKNOWN_ERROR, MYF(0));
      }
    }
    opt_noacl= 0;

    if (unlikely(tmp_thd))
    {
      delete tmp_thd;
      thd= 0;
    }
    reset_mqh((LEX_USER *)NULL, TRUE);
  }
#endif
  if (options & REFRESH_LOG)
  {
    /*
      Flush the normal query log, the update log, the binary log,
      the slow query log, the relay log (if it exists) and the log
      tables.
    */

    options|= REFRESH_BINARY_LOG;
    options|= REFRESH_RELAY_LOG;
    options|= REFRESH_SLOW_LOG;
    options|= REFRESH_GENERAL_LOG;
    options|= REFRESH_ENGINE_LOG;
    options|= REFRESH_ERROR_LOG;
  }

  if (options & REFRESH_ERROR_LOG)
    if (unlikely(flush_error_log()))
      result= 1;

  if ((options & REFRESH_SLOW_LOG) && global_system_variables.sql_log_slow)
    logger.flush_slow_log();

  if ((options & REFRESH_GENERAL_LOG) && opt_log)
    logger.flush_general_log();

  if (options & REFRESH_ENGINE_LOG)
    if (ha_flush_logs())
      result= 1;

  if (options & REFRESH_BINARY_LOG)
  {
    /*
      Writing this command to the binlog may result in infinite loops
      when doing mysqlbinlog|mysql, and anyway it does not really make
      sense to log it automatically (would cause more trouble to users
      than it would help them)
    */
    tmp_write_to_binlog= 0;
    if (mysql_bin_log.is_open())
    {
      DYNAMIC_ARRAY *drop_gtid_domain=
        (thd && (thd->lex->delete_gtid_domain.elements > 0)) ?
        &thd->lex->delete_gtid_domain : NULL;
      if (mysql_bin_log.rotate_and_purge(true, drop_gtid_domain))
        *write_to_binlog= -1;

      /* Note that WSREP(thd) might not be true here e.g. during
      SST. */
      if (WSREP_ON)
      {
        /* Wait for last binlog checkpoint event to be logged. */
        mysql_bin_log.wait_for_last_checkpoint_event();
      }
    }
  }
  if (options & REFRESH_RELAY_LOG)
  {
#ifdef HAVE_REPLICATION
    LEX_CSTRING connection_name;
    Master_info *mi;
    if (thd)
      connection_name= thd->lex->relay_log_connection_name;
    else
    {
      connection_name.str= (char*) "";
      connection_name.length= 0;
    }

    /*
      Writing this command to the binlog may cause problems as the
      slave is not likely to have the same connection names.
    */
    tmp_write_to_binlog= 0;
    if (connection_name.length == 0)
    {
      if (master_info_index->flush_all_relay_logs())
          *write_to_binlog= -1;
    }
    else if (!(mi= (get_master_info(&connection_name,
                               Sql_condition::WARN_LEVEL_ERROR))))
    {
      result= 1;
    }
    else
    {
      mysql_mutex_lock(&mi->data_lock);
      if (rotate_relay_log(mi))
        *write_to_binlog= -1;
      mysql_mutex_unlock(&mi->data_lock);
      mi->release();
    }
#endif
  }
#ifdef HAVE_QUERY_CACHE
  if (options & REFRESH_QUERY_CACHE_FREE)
  {
    query_cache.pack(thd);              // FLUSH QUERY CACHE
    options &= ~REFRESH_QUERY_CACHE;    // Don't flush cache, just free memory
  }
  if (options & (REFRESH_TABLES | REFRESH_QUERY_CACHE))
  {
    query_cache.flush();			// RESET QUERY CACHE
  }
#endif /*HAVE_QUERY_CACHE*/

  DBUG_ASSERT(!thd || thd->locked_tables_mode ||
              !thd->mdl_context.has_locks() ||
              thd->handler_tables_hash.records ||
              thd->ull_hash.records ||
              thd->global_read_lock.is_acquired() ||
              thd->mdl_backup_lock ||
              thd->current_backup_stage != BACKUP_FINISHED
              );

  /*
    Note that if REFRESH_READ_LOCK bit is set then REFRESH_TABLES is set too
    (see sql_yacc.yy)
  */
  if (options & (REFRESH_TABLES | REFRESH_READ_LOCK)) 
  {
    if ((options & REFRESH_READ_LOCK) && thd)
    {
      DBUG_ASSERT(!(options & REFRESH_FAST) && !tables);
      /*
        On the first hand we need write lock on the tables to be flushed,
        on the other hand we must not try to aspire a global read lock
        if we have a write locked table as this would lead to a deadlock
        when trying to reopen (and re-lock) the table after the flush.
      */
      if (thd->locked_tables_mode)
      {
        my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
        return 1;
      }

      /*
	Writing to the binlog could cause deadlocks, as we don't log
	UNLOCK TABLES
      */
      tmp_write_to_binlog= 0;
      if (thd->global_read_lock.lock_global_read_lock(thd))
	return 1;                               // Killed
      if (flush_tables(thd, FLUSH_ALL))
      {
        /*
          NOTE: my_error() has been already called by reopen_tables() within
          close_cached_tables().
        */
        thd->global_read_lock.unlock_global_read_lock(thd);
        return 1;
      }

      if (thd->global_read_lock.make_global_read_lock_block_commit(thd)) // Killed
      {
        /* Don't leave things in a half-locked state */
        thd->global_read_lock.unlock_global_read_lock(thd);
        return 1;
      }
      if (options & REFRESH_CHECKPOINT)
        disable_checkpoints(thd);
      /*
        We need to do it second time after wsrep appliers were blocked in
        make_global_read_lock_block_commit(thd) above since they could have
        modified the tables too.
      */
      if (WSREP(thd) && flush_tables(thd, FLUSH_ALL))
        result= 1;
    }
    else
    {
      if (thd && thd->locked_tables_mode)
      {
        /*
          If we are under LOCK TABLES we should have a write
          lock on tables which we are going to flush.
        */
        if (tables)
        {
          int err;
          for (TABLE_LIST *t= tables; t; t= t->next_local)
            if (!find_table_for_mdl_upgrade(thd, t->db.str, t->table_name.str, &err))
            {
              if (is_locked_view(thd, t))
                t->next_local= t->next_global;
              else
              {
                my_error(err, MYF(0), t->table_name.str);
                return 1;
              }
            }
        }
        else
        {
          /*
            It is not safe to upgrade the metadata lock without GLOBAL IX lock.
            This can happen with FLUSH TABLES <list> WITH READ LOCK as we in
            these cases don't take a GLOBAL IX lock in order to be compatible
            with global read lock.
          */
          if (thd->open_tables &&
              !thd->mdl_context.is_lock_owner(MDL_key::BACKUP, "", "",
                                              MDL_BACKUP_DDL))
          {
            my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0),
                     thd->open_tables->s->table_name.str);
            return true;
          }

          for (TABLE *tab= thd->open_tables; tab; tab= tab->next)
          {
            if (! tab->mdl_ticket->is_upgradable_or_exclusive())
            {
              my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0),
                       tab->s->table_name.str);
              return 1;
            }
          }
        }
      }

#ifdef WITH_WSREP
      /* In case of applier thread, do not call flush tables */
      if (!thd || !thd->wsrep_applier)
#endif /* WITH_WSREP */
      {
        if (close_cached_tables(thd, tables,
                                ((options & REFRESH_FAST) ?  FALSE : TRUE),
                                (thd ? thd->variables.lock_wait_timeout :
                                 LONG_TIMEOUT)))
        {
          /*
            NOTE: my_error() has been already called by reopen_tables() within
            close_cached_tables().
          */
          result= 1;
        }
      }
    }
    my_dbopt_cleanup();
  }
  if (options & REFRESH_HOSTS)
    hostname_cache_refresh();
  if (thd && (options & REFRESH_STATUS))
    refresh_status(thd);
  if (options & REFRESH_THREADS)
    thread_cache.flush();
#ifdef HAVE_REPLICATION
  if (options & REFRESH_MASTER)
  {
    DBUG_ASSERT(thd);
    tmp_write_to_binlog= 0;
    if (reset_master(thd, NULL, 0, thd->lex->next_binlog_file_number))
    {
      /* NOTE: my_error() has been already called by reset_master(). */
      result= 1;
    }
  }
#endif
#ifdef HAVE_OPENSSL
   if (options & REFRESH_DES_KEY_FILE)
   {
     if (des_key_file && load_des_key_file(des_key_file))
     {
       /* NOTE: my_error() has been already called by load_des_key_file(). */
       result= 1;
     }
   }
#endif
#ifdef HAVE_REPLICATION
 if (options & REFRESH_SLAVE)
 {
   LEX_MASTER_INFO* lex_mi= &thd->lex->mi;
   Master_info *mi;
   tmp_write_to_binlog= 0;

   if (!(mi= get_master_info(&lex_mi->connection_name,
                             Sql_condition::WARN_LEVEL_ERROR)))
   {
     result= 1;
   }
   else
   {
     /* The following will fail if slave is running */
     if (reset_slave(thd, mi))
     {
       mi->release();
       /* NOTE: my_error() has been already called by reset_slave(). */
       result= 1;
     }
     else if (mi->connection_name.length && thd->lex->reset_slave_info.all)
     {
       /* If not default connection and 'all' is used */
       mi->release();
       mysql_mutex_lock(&LOCK_active_mi);
       if (master_info_index->remove_master_info(mi))
         result= 1;
       mysql_mutex_unlock(&LOCK_active_mi);
     }
     else
       mi->release();
   }
 }
#endif
 if (options & REFRESH_USER_RESOURCES)
   reset_mqh((LEX_USER *) NULL, 0);             /* purecov: inspected */
 if (options & REFRESH_SSL)
 {
   if (reinit_ssl())
     result= 1;
 }
 if (options & REFRESH_GENERIC)
 {
   List_iterator_fast<LEX_CSTRING> li(thd->lex->view_list);
   LEX_CSTRING *ls;
   while ((ls= li++))
   {
     ST_SCHEMA_TABLE *table= find_schema_table(thd, ls);
     if (table->reset_table())
       result= 1;
   }
 }
 if (*write_to_binlog != -1)
   *write_to_binlog= tmp_write_to_binlog;
 /*
   If the query was killed then this function must fail.
 */
 return result || (thd ? thd->killed : 0);
}


/**
  Implementation of FLUSH TABLES <table_list> WITH READ LOCK
  and FLUSH TABLES <table_list> FOR EXPORT

  In brief: take exclusive locks, expel tables from the table
  cache, reopen the tables, enter the 'LOCKED TABLES' mode,
  downgrade the locks.
  Note: the function is written to be called from
  mysql_execute_command(), it is not reusable in arbitrary
  execution context.

  Required privileges
  -------------------
  Since the statement implicitly enters LOCK TABLES mode,
  it requires LOCK TABLES privilege on every table.
  But since the rest of FLUSH commands require
  the global RELOAD_ACL, it also requires RELOAD_ACL.

  Compatibility with the global read lock
  ---------------------------------------
  We don't wait for the GRL, since neither the
  5.1 combination that this new statement is intended to
  replace (LOCK TABLE <list> WRITE; FLUSH TABLES;),
  nor FLUSH TABLES WITH READ LOCK do.
  @todo: this is not implemented, Dmitry disagrees.
  Currently we wait for GRL in another connection,
  but are compatible with a GRL in our own connection.

  Behaviour under LOCK TABLES
  ---------------------------
  Bail out: i.e. don't perform an implicit UNLOCK TABLES.
  This is not consistent with LOCK TABLES statement, but is
  in line with behaviour of FLUSH TABLES WITH READ LOCK, and we
  try to not introduce any new statements with implicit
  semantics.

  Compatibility with parallel updates
  -----------------------------------
  As a result, we will wait for all open transactions
  against the tables to complete. After the lock downgrade,
  new transactions will be able to read the tables, but not
  write to them.

  Differences from FLUSH TABLES <list>
  -------------------------------------
  - you can't flush WITH READ LOCK a non-existent table
  - you can't flush WITH READ LOCK under LOCK TABLES

  Effect on views and temporary tables.
  ------------------------------------
  You can only apply this command to existing base tables.
  If a view with such name exists, ER_WRONG_OBJECT is returned.
  If a temporary table with such name exists, it's ignored:
  if there is a base table, it's used, otherwise ER_NO_SUCH_TABLE
  is returned.

  Handling of MERGE tables
  ------------------------
  For MERGE table this statement will open and lock child tables
  for read (it is impossible to lock parent table without it).
  Child tables won't be flushed unless they are explicitly present
  in the statement's table list.

  Implicit commit
  ---------------
  This statement causes an implicit commit before and
  after it.

  HANDLER SQL
  -----------
  If this connection has HANDLERs open against
  some of the tables being FLUSHed, these handlers
  are implicitly flushed (lose their position).
*/

bool flush_tables_with_read_lock(THD *thd, TABLE_LIST *all_tables)
{
  Lock_tables_prelocking_strategy lock_tables_prelocking_strategy;

  /*
    This is called from SQLCOM_FLUSH, the transaction has
    been committed implicitly.
  */

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    goto error;
  }

  if (thd->current_backup_stage != BACKUP_FINISHED)
  {
    my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
    goto error;
  }

  /* Should not flush tables while BACKUP LOCK is active */
  if (thd->mdl_backup_lock)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    goto error;
  }

  if (thd->lex->type & REFRESH_READ_LOCK)
  {
    /*
      Acquire SNW locks on tables to be flushed. Don't acquire global
      IX and database-scope IX locks on the tables as this will make
      this statement incompatible with FLUSH TABLES WITH READ LOCK.
    */
    if (lock_table_names(thd, all_tables, NULL,
                         thd->variables.lock_wait_timeout,
                         MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK))
      goto error;

    DEBUG_SYNC(thd,"flush_tables_with_read_lock_after_acquire_locks");

    /* Reset ticket to satisfy asserts in open_tables(). */
    for (auto table_list= all_tables; table_list;
         table_list= table_list->next_global)
      table_list->mdl_request.ticket= NULL;
  }

  thd->variables.option_bits|= OPTION_TABLE_LOCK;

  /*
    Before opening and locking tables the below call also waits
    for old shares to go away, so the fact that we don't pass
    MYSQL_OPEN_IGNORE_FLUSH flag to it is important.
    Also we don't pass MYSQL_OPEN_HAS_MDL_LOCK flag as we want
    to open underlying tables if merge table is flushed.
    For underlying tables of the merge the below call has to
    acquire SNW locks to ensure that they can be locked for
    read without further waiting.
  */
  if (open_and_lock_tables(thd, all_tables, FALSE,
                           MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK,
                           &lock_tables_prelocking_strategy))
    goto error_reset_bits;

  if (thd->lex->type & REFRESH_FOR_EXPORT)
  {
    // Check if all storage engines support FOR EXPORT.
    for (TABLE_LIST *table_list= all_tables; table_list;
         table_list= table_list->next_global)
    {
      if (!(table_list->table->file->ha_table_flags() & HA_CAN_EXPORT))
      {
        my_error(ER_ILLEGAL_HA, MYF(0),table_list->table->file->table_type(),
                 table_list->db.str, table_list->table_name.str);
        goto error_reset_bits;
      }
    }
  }

  if (thd->lex->type & REFRESH_READ_LOCK)
  {
    for (auto table_list= all_tables; table_list;
         table_list= table_list->next_global)
    {
      if (table_list->table->file->extra(HA_EXTRA_FLUSH))
        goto error_reset_bits;
    }
  }

  if (thd->locked_tables_list.init_locked_tables(thd))
    goto error_reset_bits;


  /*
    We don't downgrade MDL_SHARED_NO_WRITE here as the intended
    post effect of this call is identical to LOCK TABLES <...> READ,
    and we didn't use thd->in_lock_talbes and
    thd->sql_command= SQLCOM_LOCK_TABLES hacks to enter the LTM.
  */

  return FALSE;

error_reset_bits:
  trans_rollback_stmt(thd);
  close_thread_tables(thd);
  thd->variables.option_bits&= ~OPTION_TABLE_LOCK;
error:
  return TRUE;
}


/**
   Disable checkpoints for all handlers
   This is released in unlock_global_read_lock()
*/

static void disable_checkpoints(THD *thd)
{
  if (!thd->global_disable_checkpoint)
  {
    thd->global_disable_checkpoint= 1;
    if (!global_disable_checkpoint++)
      ha_checkpoint_state(1);                   // Disable checkpoints
  }
}
