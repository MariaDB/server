/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2013, Monty Program Ab.

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

static TABLE_LIST *rename_tables(THD *thd, TABLE_LIST *table_list,
                                 bool skip_error, bool if_exits,
                                 bool *force_if_exists,
                                 FK_rename_vector &fk_rename_backup);
static bool do_rename(THD *thd, TABLE_LIST *ren_table,
                      const LEX_CSTRING *new_db,
                      const LEX_CSTRING *new_table_name,
                      const LEX_CSTRING *new_table_alias,
                      bool skip_error, bool if_exists, bool *force_if_exists,
		      FK_rename_vector &fk_rename_backup);

static TABLE_LIST *reverse_table_list(TABLE_LIST *table_list);

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
  FK_rename_vector fk_rename_backup;
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
  /*
    An exclusive lock on table names is satisfactory to ensure
    no other thread accesses this table.
  */
  if ((ren_table= rename_tables(thd, table_list, 0, if_exists,
                                &force_if_exists, fk_rename_backup)))
  {
    /* Rename didn't succeed;  rename back the tables in reverse order */
    TABLE_LIST *table;

    /* Reverse the table list */
    table_list= reverse_table_list(table_list);

    /* Find the last renamed table */
    for (table= table_list;
	 table->next_local != ren_table ;
	 table= table->next_local->next_local) ;
    table= table->next_local->next_local;		// Skip error table
    /* Revert to old names */
    rename_tables(thd, table, 1, if_exists, &force_if_exists, fk_rename_backup);

    /* Revert the table list (for prepared statements) */
    table_list= reverse_table_list(table_list);

    for (FK_rename_backup &bak: fk_rename_backup)
      bak.rollback();

    error= 1;
  }
  else
  {
    for (FK_rename_backup &bak: fk_rename_backup)
    {
      error= fk_install_shadow_frm(bak.old_name, bak.new_name);
      if (error)
        break;
    }
  }

  if (likely(!silent && !error))
  {
    ulonglong save_option_bits= thd->variables.option_bits;
    if (force_if_exists && ! if_exists)
    {
      /* Add IF EXISTS to binary log */
      thd->variables.option_bits|= OPTION_IF_EXISTS;
    }
    binlog_error= write_bin_log(thd, TRUE, thd->query(), thd->query_length());
    thd->variables.option_bits= save_option_bits;

    if (likely(!binlog_error))
      my_ok(thd);
  }

  if (likely(!error))
    query_cache_invalidate3(thd, table_list, 0);

err:
  DBUG_RETURN(error || binlog_error);
}


/*
  reverse table list

  SYNOPSIS
    reverse_table_list()
    table_list pointer to table _list

  RETURN
    pointer to new (reversed) list
*/
static TABLE_LIST *reverse_table_list(TABLE_LIST *table_list)
{
  TABLE_LIST *prev= 0;

  while (table_list)
  {
    TABLE_LIST *next= table_list->next_local;
    table_list->next_local= prev;
    prev= table_list;
    table_list= next;
  }
  return (prev);
}


static bool
do_rename_temporary(THD *thd, TABLE_LIST *ren_table, TABLE_LIST *new_table,
                    bool skip_error)
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


/*
  Rename a single table or a view

  SYNPOSIS
    do_rename()
      thd               Thread handle
      ren_table         A table/view to be renamed
      new_db            The database to which the table to be moved to
      new_table_name    The new table/view name
      new_table_alias   The new table/view alias
      skip_error        Whether to skip error
      if_exists         Skip error, but only if the table didn't exists
      force_if_exists   Set to 1 if we have to log the query with 'IF EXISTS'
                        Otherwise don't touch the value

  DESCRIPTION
    Rename a single table or a view.

  RETURN
    false     Ok
    true      rename failed
*/

static bool
do_rename(THD *thd, TABLE_LIST *ren_table, const LEX_CSTRING *new_db,
          const LEX_CSTRING *new_table_name,
          const LEX_CSTRING *new_table_alias,
          bool skip_error, bool if_exists, bool *force_if_exists,
          FK_rename_vector &fk_rename_backup)
{
  int rc= 1;
  handlerton *hton, *new_hton;
  LEX_CSTRING old_alias, new_alias;
  DBUG_ENTER("do_rename");
  DBUG_PRINT("enter", ("skip_error: %d  if_exists: %d", (int) skip_error,
                       (int) if_exists));

  if (lower_case_table_names == 2)
  {
    old_alias= ren_table->alias;
    new_alias= *new_table_alias;
  }
  else
  {
    old_alias= ren_table->table_name;
    new_alias= *new_table_name;
  }
  DBUG_ASSERT(new_alias.str);

  if (!ha_table_exists(thd, &ren_table->db, &old_alias, &hton) || !hton)
  {
    my_error(ER_NO_SUCH_TABLE, MYF((skip_error | if_exists) ? ME_NOTE : 0),
             ren_table->db.str, old_alias.str);
    DBUG_RETURN(skip_error || if_exists ? 0 : 1);
  }

  if (ha_check_if_updates_are_ignored(thd, hton, "RENAME"))
  {
    /*
      Shared table. Just drop the old .frm as it's not correct anymore
      Discovery will find the old table when it's accessed
     */
    tdc_remove_table(thd, ren_table->db.str, ren_table->table_name.str);
    quick_rm_table(thd, 0, &ren_table->db, &old_alias, FRM_ONLY, 0);
    DBUG_RETURN(0);
  }

  if (ha_table_exists(thd, new_db, &new_alias, &new_hton))
  {
    my_error(ER_TABLE_EXISTS_ERROR, MYF(0), new_alias.str);
    DBUG_RETURN(1);                     // This can't be skipped
  }

  DBUG_ASSERT(!thd->locked_tables_mode);

#ifdef WITH_WSREP
  if (WSREP(thd) && hton && hton != view_pseudo_hton &&
      !wsrep_should_replicate_ddl(thd, hton->db_type))
    DBUG_RETURN(1);
#endif

  tdc_remove_table(thd, ren_table->db.str, ren_table->table_name.str);

  if (hton != view_pseudo_hton)
  {
    if (hton->flags & HTON_TABLE_MAY_NOT_EXIST_ON_SLAVE)
      *force_if_exists= 1;

    if (!skip_error &&
        fk_handle_rename(thd, ren_table, new_db, new_table_name, fk_rename_backup))
      DBUG_RETURN(1);


    thd->replication_flags= 0;
    if (!(rc= mysql_rename_table(hton, &ren_table->db, &old_alias,
                                 new_db, &new_alias, 0)))
    {
      (void) rename_table_in_stat_tables(thd, &ren_table->db,
                                         &ren_table->table_name,
                                         new_db, &new_alias);
      if ((rc= Table_triggers_list::change_table_name(thd, &ren_table->db,
                                                      &old_alias,
                                                      &ren_table->table_name,
                                                      new_db,
                                                      &new_alias)))
      {
        /*
          We've succeeded in renaming table's .frm and in updating
          corresponding handler data, but have failed to update table's
          triggers appropriately. So let us revert operations on .frm
          and handler's data and report about failure to rename table.
        */
        (void) mysql_rename_table(hton, new_db, &new_alias,
                                  &ren_table->db, &old_alias, NO_FK_CHECKS);
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
      my_error(ER_FORBID_SCHEMA_CHANGE, MYF(0), ren_table->db.str, new_db->str);
    else
      rc= mysql_rename_view(thd, new_db, &new_alias, ren_table);
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
    table     pointer to the table list element which rename failed
*/

static TABLE_LIST *
rename_tables(THD *thd, TABLE_LIST *table_list, bool skip_error,
              bool if_exists, bool *force_if_exists,
              FK_rename_vector &fk_rename_backup)
{
  TABLE_LIST *ren_table, *new_table;
  DBUG_ENTER("rename_tables");

  *force_if_exists= 0;

  for (ren_table= table_list; ren_table; ren_table= new_table->next_local)
  {
    new_table= ren_table->next_local;

    if (is_temporary_table(ren_table) ?
          do_rename_temporary(thd, ren_table, new_table, skip_error) :
          do_rename(thd, ren_table, &new_table->db, &new_table->table_name,
                    &new_table->alias, skip_error, if_exists, force_if_exists,
		    fk_rename_backup))
      DBUG_RETURN(ren_table);
  }
  DBUG_RETURN(0);
}


FK_rename_backup::FK_rename_backup(Share_acquire&& _sa) :
  FK_ddl_backup(std::forward<Share_acquire>(_sa)),
  old_name(sa.share->db, sa.share->table_name),
  new_name(sa.share->db, sa.share->table_name)
{}
