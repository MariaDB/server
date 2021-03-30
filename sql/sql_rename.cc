/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, Monty Program Ab.

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

/*
  Atomic rename of table;  RENAME TABLE t1 to t2, tmp to t1 [,...]
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_rename.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_table.h"                         // write_bin_log
#include "sql_view.h"             // mysql_frm_type, mysql_rename_view
#include "sql_trigger.h"
#include "sql_base.h"   // tdc_remove_table, lock_table_names,
#include "sql_handler.h"                        // mysql_ha_rm_tables
#include "sql_statistics.h" 
#include "ddl_log.h"
#include "debug.h"

/* used to hold table entries for as part of list of renamed temporary tables */
struct TABLE_PAIR
{
  TABLE_LIST *from, *to;
};


static bool rename_tables(THD *thd, TABLE_LIST *table_list,
                          DDL_LOG_STATE *ddl_log_state,
                          bool skip_error, bool if_exits,
                          bool *force_if_exists);

/*
  Every two entries in the table_list form a pair of original name and
  the new name.
*/

bool mysql_rename_tables(THD *thd, TABLE_LIST *table_list, bool silent,
                          bool if_exists)
{
  bool error= 1;
  bool binlog_error= 0, force_if_exists;
  TABLE_LIST *ren_table= 0;
  int to_table;
  const char *rename_log_table[2]= {NULL, NULL};
  DDL_LOG_STATE ddl_log_state;
  DBUG_ENTER("mysql_rename_tables");

  /*
    Avoid problems with a rename on a table that we have locked or
    if the user is trying to to do this in a transcation context
  */

  if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
  {
    my_message(ER_LOCK_OR_ACTIVE_TRANSACTION,
               ER_THD(thd, ER_LOCK_OR_ACTIVE_TRANSACTION), MYF(0));
    DBUG_RETURN(1);
  }

  mysql_ha_rm_tables(thd, table_list);

  if (logger.is_log_table_enabled(QUERY_LOG_GENERAL) ||
      logger.is_log_table_enabled(QUERY_LOG_SLOW))
  {

    /*
      Rules for rename of a log table:

      IF   1. Log tables are enabled
      AND  2. Rename operates on the log table and nothing is being
              renamed to the log table.
      DO   3. Throw an error message.
      ELSE 4. Perform rename.
    */

    for (to_table= 0, ren_table= table_list; ren_table;
         to_table= 1 - to_table, ren_table= ren_table->next_local)
    {
      int log_table_rename;
      if ((log_table_rename= check_if_log_table(ren_table, TRUE, NullS)))
      {
        /*
          as we use log_table_rename as an array index, we need it to start
          with 0, while QUERY_LOG_SLOW == 1 and QUERY_LOG_GENERAL == 2.
          So, we shift the value to start with 0;
        */
        log_table_rename--;
        if (rename_log_table[log_table_rename])
        {
          if (to_table)
            rename_log_table[log_table_rename]= NULL;
          else
          {
            /*
              Two renames of "log_table TO" w/o rename "TO log_table" in
              between.
            */
            my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0),
                     ren_table->table_name.str,
                     ren_table->table_name.str);
            goto err;
          }
        }
        else
        {
          if (to_table)
          {
            /*
              Attempt to rename a table TO log_table w/o renaming
              log_table TO some table.
            */
            my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0),
                     ren_table->table_name.str,
                     ren_table->table_name.str);
            goto err;
          }
          else
          {
            /* save the name of the log table to report an error */
            rename_log_table[log_table_rename]= ren_table->table_name.str;
          }
        }
      }
    }
    if (rename_log_table[0] || rename_log_table[1])
    {
      if (rename_log_table[0])
        my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), rename_log_table[0],
                 rename_log_table[0]);
      else
        my_error(ER_CANT_RENAME_LOG_TABLE, MYF(0), rename_log_table[1],
                 rename_log_table[1]);
      goto err;
    }
  }

  if (lock_table_names(thd, table_list, 0, thd->variables.lock_wait_timeout,
                       0))
    goto err;

  error=0;
  bzero(&ddl_log_state, sizeof(ddl_log_state));

  /*
    An exclusive lock on table names is satisfactory to ensure
    no other thread accesses this table.
  */
  error= rename_tables(thd, table_list, &ddl_log_state,
                       0, if_exists, &force_if_exists);

  if (likely(!silent && !error))
  {
    ulonglong save_option_bits= thd->variables.option_bits;
    if (force_if_exists && ! if_exists)
    {
      /* Add IF EXISTS to binary log */
      thd->variables.option_bits|= OPTION_IF_EXISTS;
    }

    debug_crash_here("ddl_log_rename_before_binlog");
    /*
      Store xid in ddl log and binary log so that we can check on ddl recovery
      if the item is in the binary log (and thus the operation was complete
    */
    thd->binlog_xid= thd->query_id;
    ddl_log_update_xid(&ddl_log_state, thd->binlog_xid);
    binlog_error= write_bin_log(thd, TRUE, thd->query(), thd->query_length());
    if (binlog_error)
      error= 1;
    thd->binlog_xid= 0;
    thd->variables.option_bits= save_option_bits;
    debug_crash_here("ddl_log_rename_after_binlog");

    if (likely(!binlog_error))
      my_ok(thd);
  }

  if (likely(!error))
  {
    query_cache_invalidate3(thd, table_list, 0);
    ddl_log_complete(&ddl_log_state);
  }
  else
  {
    /* Revert the renames of normal tables with the help of the ddl log */
    ddl_log_revert(thd, &ddl_log_state);
  }

err:
  DBUG_RETURN(error || binlog_error);
}


static bool
do_rename_temporary(THD *thd, TABLE_LIST *ren_table, TABLE_LIST *new_table)
{
  LEX_CSTRING *new_alias;
  DBUG_ENTER("do_rename_temporary");

  new_alias= (lower_case_table_names == 2) ? &new_table->alias :
                                             &new_table->table_name;

  if (thd->find_temporary_table(new_table, THD::TMP_TABLE_ANY))
  {
    my_error(ER_TABLE_EXISTS_ERROR, MYF(0), new_alias->str);
    DBUG_RETURN(1);                     // This can't be skipped
  }

  DBUG_RETURN(thd->rename_temporary_table(ren_table->table,
                                          &new_table->db, new_alias));
}


/**
   Parameters for do_rename
*/

struct rename_param
{
  LEX_CSTRING old_alias, new_alias;
  LEX_CUSTRING old_version;
  handlerton *from_table_hton;
};


/**
  check_rename()

  Check pre-conditions for rename
  - From table should exists
  - To table should not exists.

  SYNOPSIS
  @param new_table_name    The new table/view name
  @param new_table_alias   The new table/view alias
  @param if_exists         If not set, give an error if the table does not
                           exists. If set, just give a warning in this case.
  @return
  @retval 0   ok
  @retval >0  Error (from table doesn't exists or to table exists)
  @retval <0  Can't do rename, but no error
*/

static int
check_rename(THD *thd, rename_param *param,
             TABLE_LIST *ren_table,
             const LEX_CSTRING *new_db,
             const LEX_CSTRING *new_table_name,
             const LEX_CSTRING *new_table_alias,
             bool if_exists)
{
  DBUG_ENTER("check_rename");
  DBUG_PRINT("enter", ("if_exists: %d", (int) if_exists));


  if (lower_case_table_names == 2)
  {
    param->old_alias= ren_table->alias;
    param->new_alias= *new_table_alias;
  }
  else
  {
    param->old_alias= ren_table->table_name;
    param->new_alias= *new_table_name;
  }
  DBUG_ASSERT(param->new_alias.str);

  if (!ha_table_exists(thd, &ren_table->db, &param->old_alias,
                       &param->old_version, NULL,
                       &param->from_table_hton) ||
      !param->from_table_hton)
  {
    my_error(ER_NO_SUCH_TABLE, MYF(if_exists ? ME_NOTE : 0),
             ren_table->db.str, param->old_alias.str);
    DBUG_RETURN(if_exists ? -1 : 1);
  }

  if (param->from_table_hton != view_pseudo_hton &&
      ha_check_if_updates_are_ignored(thd, param->from_table_hton, "RENAME"))
  {
    /*
      Shared table. Just drop the old .frm as it's not correct anymore
      Discovery will find the old table when it's accessed
     */
    tdc_remove_table(thd, ren_table->db.str, ren_table->table_name.str);
    quick_rm_table(thd, 0, &ren_table->db, &param->old_alias, FRM_ONLY, 0);
    DBUG_RETURN(-1);
  }

  if (ha_table_exists(thd, new_db, &param->new_alias, NULL, NULL, 0))
  {
    my_error(ER_TABLE_EXISTS_ERROR, MYF(0), param->new_alias.str);
    DBUG_RETURN(1);                     // This can't be skipped
  }
  DBUG_RETURN(0);
}


/*
  Rename a single table or a view

  SYNPOSIS
    do_rename()
      thd               Thread handle
      ren_table         A table/view to be renamed
      new_db            The database to which the table to be moved to
      skip_error        Skip error, but only if the table didn't exists
      force_if_exists   Set to 1 if we have to log the query with 'IF EXISTS'
                        Otherwise don't touch the value

  DESCRIPTION
    Rename a single table or a view.
    In case of failure, all changes will be reverted

  RETURN
    false     Ok
    true      rename failed
*/

static bool
do_rename(THD *thd, rename_param *param, DDL_LOG_STATE *ddl_log_state,
          TABLE_LIST *ren_table, const LEX_CSTRING *new_db,
          bool skip_error, bool *force_if_exists)
{
  int rc= 1;
  handlerton *hton;
  LEX_CSTRING *old_alias, *new_alias;
  TRIGGER_RENAME_PARAM rename_param;
  DBUG_ENTER("do_rename");
  DBUG_PRINT("enter", ("skip_error: %d", (int) skip_error));

  old_alias= &param->old_alias;
  new_alias= &param->new_alias;
  hton=      param->from_table_hton;

  DBUG_ASSERT(!thd->locked_tables_mode);

#ifdef WITH_WSREP
  if (WSREP(thd) && hton && hton != view_pseudo_hton &&
      !wsrep_should_replicate_ddl(thd, hton))
    DBUG_RETURN(1);
#endif

  tdc_remove_table(thd, ren_table->db.str, ren_table->table_name.str);

  if (hton != view_pseudo_hton)
  {
    if (hton->flags & HTON_TABLE_MAY_NOT_EXIST_ON_SLAVE)
      *force_if_exists= 1;

    /* Check if we can rename triggers */
    if (Table_triggers_list::prepare_for_rename(thd, &rename_param,
                                                &ren_table->db,
                                                old_alias,
                                                &ren_table->table_name,
                                                new_db,
                                                new_alias))
      DBUG_RETURN(!skip_error);

    thd->replication_flags= 0;

    if (ddl_log_rename_table(thd, ddl_log_state, hton,
                             &ren_table->db, old_alias, new_db, new_alias))
      DBUG_RETURN(1);

    debug_crash_here("ddl_log_rename_before_rename_table");
    if (!(rc= mysql_rename_table(hton, &ren_table->db, old_alias,
                                 new_db, new_alias, &param->old_version, 0)))
    {
      /* Table rename succeded.
         It's safe to start recovery at rename trigger phase
      */
      debug_crash_here("ddl_log_rename_before_phase_trigger");
      ddl_log_update_phase(ddl_log_state, DDL_RENAME_PHASE_TRIGGER);

      debug_crash_here("ddl_log_rename_before_rename_trigger");

      if (!(rc= Table_triggers_list::change_table_name(thd,
                                                       &rename_param,
                                                       &ren_table->db,
                                                       old_alias,
                                                       &ren_table->table_name,
                                                       new_db,
                                                       new_alias)))
      {
        debug_crash_here("ddl_log_rename_before_stat_tables");
        (void) rename_table_in_stat_tables(thd, &ren_table->db,
                                           &ren_table->table_name,
                                           new_db, new_alias);
        debug_crash_here("ddl_log_rename_after_stat_tables");
      }
      else
      {
        /*
          We've succeeded in renaming table's .frm and in updating
          corresponding handler data, but have failed to update table's
          triggers appropriately. So let us revert operations on .frm
          and handler's data and report about failure to rename table.
        */
        debug_crash_here("ddl_log_rename_after_failed_rename_trigger");
        (void) mysql_rename_table(hton, new_db, new_alias,
                                  &ren_table->db, old_alias, &param->old_version,
                                  NO_FK_CHECKS);
        debug_crash_here("ddl_log_rename_after_revert_rename_table");
        ddl_log_disable_entry(ddl_log_state);
        debug_crash_here("ddl_log_rename_after_disable_entry");
      }
    }
    if (thd->replication_flags & OPTION_IF_EXISTS)
      *force_if_exists= 1;
  }
  else
  {
    /*
      Change of schema is not allowed
      except of ALTER ...UPGRADE DATA DIRECTORY NAME command
      because a view has valid internal db&table names in this case.
    */
    if (thd->lex->sql_command != SQLCOM_ALTER_DB_UPGRADE &&
        cmp(&ren_table->db, new_db))
    {
      my_error(ER_FORBID_SCHEMA_CHANGE, MYF(0), ren_table->db.str, new_db->str);
      DBUG_RETURN(1);
    }

    ddl_log_rename_view(thd, ddl_log_state, &ren_table->db,
                        &ren_table->table_name, new_db, new_alias);
    debug_crash_here("ddl_log_rename_before_rename_view");
    rc= mysql_rename_view(thd, new_db, new_alias, &ren_table->db,
                          &ren_table->table_name);
    debug_crash_here("ddl_log_rename_after_rename_view");
    if (rc)
    {
      /*
        On error mysql_rename_view() will leave things as such.
      */
      ddl_log_disable_entry(ddl_log_state);
      debug_crash_here("ddl_log_rename_after_disable_entry");
    }
  }
  DBUG_RETURN(rc && !skip_error ? 1 : 0);
}


/*
  Rename all tables in list; Return pointer to wrong entry if something goes
  wrong.  Note that the table_list may be empty!
*/

/*
  Rename tables/views in the list

  SYNPOSIS
    rename_tables()
      thd               Thread handle
      table_list        List of tables to rename
      ddl_log_state     ddl logging
      skip_error        Whether to skip errors
      if_exists         Don't give an error if table doesn't exists
      force_if_exists   Set to 1 if we have to log the query with 'IF EXISTS'
                        Otherwise set it to 0

  DESCRIPTION
    Take a table/view name from and odd list element and rename it to a
    the name taken from list element+1. Note that the table_list may be
    empty.

  RETURN
    0         Ok
    1         error
              All tables are reverted to their original names
*/

static bool
rename_tables(THD *thd, TABLE_LIST *table_list, DDL_LOG_STATE *ddl_log_state,
              bool skip_error, bool if_exists, bool *force_if_exists)
{
  TABLE_LIST *ren_table, *new_table;
  List<TABLE_PAIR> tmp_tables;
  DBUG_ENTER("rename_tables");

  *force_if_exists= 0;

  for (ren_table= table_list; ren_table; ren_table= new_table->next_local)
  {
    new_table= ren_table->next_local;

    if (is_temporary_table(ren_table))
    {
      /*
        Store renamed temporary tables into a list.
        We don't store these in the ddl log to avoid writes and syncs
        when only using temporary tables.  We don't need the log as
        all temporary tables will disappear anyway in a crash.
      */
      TABLE_PAIR *pair= (TABLE_PAIR*) thd->alloc(sizeof(*pair));
      if (! pair || tmp_tables.push_front(pair, thd->mem_root))
        goto revert_rename;
      pair->from= ren_table;
      pair->to=   new_table;

      if (do_rename_temporary(thd, ren_table, new_table))
        goto revert_rename;
    }
    else
    {
      int error;
      rename_param param;
      error= check_rename(thd, &param, ren_table, &new_table->db,
                          &new_table->table_name,
                          &new_table->alias, (skip_error || if_exists));
      if (error < 0)
        continue;                               // Ignore rename (if exists)
      if (error > 0)
        goto revert_rename;

      if (do_rename(thd, &param, ddl_log_state,
                    ren_table, &new_table->db,
                    skip_error, force_if_exists))
        goto revert_rename;
    }
  }
  DBUG_RETURN(0);

revert_rename:
  /* Revert temporary tables. Normal tables are reverted in the caller */
  List_iterator_fast<TABLE_PAIR> it(tmp_tables);
  while (TABLE_PAIR *pair= it++)
    do_rename_temporary(thd, pair->to, pair->from);

  DBUG_RETURN(1);
}
