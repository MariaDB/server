/*
  Copyright (c) 2016, 2021, MariaDB Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

/**
  All methods pertaining to temporary tables.
*/

#include "mariadb.h"
#include "sql_acl.h"                            /* TMP_TABLE_ACLS */
#include "sql_base.h"                           /* tdc_create_key */
#include "lock.h"                               /* mysql_lock_remove */
#include "log_event.h"                          /* Query_log_event */
#include "sql_show.h"                           /* append_identifier */
#include "sql_handler.h"                        /* mysql_ha_rm_temporary_tables */
#include "sql_table.h"                          // generated_by_server
#include "rpl_rli.h"                            /* rpl_group_info */


bool is_user_tmp_table(TMP_TABLE_SHARE *share)
{
  if (share->global_tmp_table())
  {
    /*
      This is either a global temporary table (GTT) definition table or
      local GTT table created from the GTT definition.
      If (share->tmp_table != NO_TMP_TABLE) then this is local GTT table.
      In this case share->from_share is also set and points to the global
      GTT definition.
    */
    return false;
  }
  /* Return true of the table is non GTT temporary table */
  return share->tmp_table != NO_TMP_TABLE;
}

/**
  Check whether temporary tables exist. The decision is made based on the
  existence of TMP_TABLE_SHAREs in Open_tables_state::temporary_tables list.

  @return false                       Temporary tables exist
          true                        No temporary table exist
*/
bool THD::has_thd_temporary_tables()
{
  DBUG_ENTER("THD::has_thd_temporary_tables");
  bool result= (temporary_tables && !temporary_tables->is_empty());
  DBUG_RETURN(result);
}

/**
   Check if there is any temporary tables that has not been logged to binary
   log.

   If this is the case then statement based binary logging is not safe.

   @result 0  All temporary tables are logged. Statement and row based
              replication are safe.
   @result 1  Some temporary tables are not logged. Statement based replication
              is not safe.
*/

bool THD::has_not_logged_temporary_tables()
{
  TABLE_SHARE *share;
  if (temporary_tables)
  {
    All_tmp_tables_list::Iterator it(*temporary_tables);
    while ((share= it++))
    {
      if (!share->using_binlog())
        return 1;
    }
  }
  return 0;
}

/**
   Check if there is at least one temporary table that is logged to binary log.

   @result 0  No temporary table changes are logged to binary log.
   @result 1  At least one temporary table is logged to binary log.
*/

bool THD::has_logged_temporary_tables()
{
  TABLE_SHARE *share;
  if (temporary_tables)
  {
    All_tmp_tables_list::Iterator it(*temporary_tables);
    while ((share= it++))
    {
      if (share->using_binlog())
        return 1;
    }
  }
  return 0;
}


/**
  Create a temporary table, open it and return the TABLE handle.

  @param frm  [IN]                    Binary frm image
  @param path [IN]                    File path (without extension)
  @param db   [IN]                    Schema name
  @param table_name [IN]              Table name

  @return Success                     A pointer to table object
          Failure                     NULL
*/
TABLE *THD::create_and_open_tmp_table(LEX_CUSTRING *frm,
                                      const char *path,
                                      const Lex_ident_db &db,
                                      const Lex_ident_table &table_name,
                                      bool open_internal_tables)
{
  DBUG_ENTER("THD::create_and_open_tmp_table");

  TMP_TABLE_SHARE *share;
  TABLE *table= NULL;

  if ((share= create_temporary_table(frm, path, db, table_name)))
  {
    open_options|= HA_OPEN_FOR_CREATE;
    table= open_temporary_table(share, table_name);
    open_options&= ~HA_OPEN_FOR_CREATE;

    /*
      Failed to open a temporary table instance. As we are not passing
      the created TMP_TABLE_SHARE to the caller, we must remove it from
      the list and free it here.
    */
    if (!table)
    {
      /* Remove the TABLE_SHARE from the list of temporary tables. */
      temporary_tables->remove(share);

      /* Free the TMP_TABLE_SHARE. */
      free_tmp_table_share(share, false);
      DBUG_RETURN(0);
    }

    /* Open any related tables */
    if (open_internal_tables && table->internal_tables &&
        open_and_lock_internal_tables(table, true))
    {
      drop_temporary_table(table, NULL, false);
      DBUG_RETURN(0);
    }
  }

  DBUG_RETURN(table);
}


/**
  Check whether an open table with db/table name is in use.

  @param db [IN]                      Database name
  @param table_name [IN]              Table name
  @param state [IN]                   State of temp table to open

  @return Success                     Pointer to first used table instance.
          Failure                     NULL
*/
TABLE *THD::find_temporary_table(const Lex_ident_db &db,
                                 const Lex_ident_table &table_name,
                                 Temporary_table_state state,
                                 Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_temporary_table");

  TABLE *table;
  char key[MAX_DBKEY_LENGTH];
  uint key_length;
  bool locked;

  if (!has_temporary_tables())
  {
    DBUG_RETURN(NULL);
  }

  key_length= create_tmp_table_def_key(key, db, table_name);

  locked= lock_temporary_tables();
  table=  find_temporary_table(key, key_length, state, find_kind);
  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(table);
}


/**
  Check whether an open table specified in TABLE_LIST is in use.

  @return tl [IN]                     TABLE_LIST

  @return Success                     Pointer to first used table instance.
          Failure                     NULL
*/
TABLE *THD::find_temporary_table(const TABLE_LIST *tl,
                                 Temporary_table_state state,
                                 Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_temporary_table");
  TABLE *table= find_temporary_table(tl->get_db_name(),
                                     tl->get_table_name(),
                                     state, find_kind);
  DBUG_RETURN(table);
}


/**
  Check whether a temporary table exists with the specified key.
  The key, in this case, is not the usual key used for temporary tables.
  It does not contain server_id & pseudo_thread_id. This function is
  essentially used use to check whether there is any temporary table
  which _shadows_ a base table.
  (see: Query_cache::send_result_to_client())

  @return Success                     A pointer to table share object
          Failure                     NULL
*/
TMP_TABLE_SHARE *THD::find_tmp_table_share_w_base_key(const char *key,
                                                      uint key_length)
{
  DBUG_ENTER("THD::find_tmp_table_share_w_base_key");

  TMP_TABLE_SHARE *share;
  TMP_TABLE_SHARE *result= NULL;
  bool locked;

  if (!has_temporary_tables())
  {
    DBUG_RETURN(NULL);
  }

  locked= lock_temporary_tables();

  All_tmp_tables_list::Iterator it(*temporary_tables);
  while ((share= it++))
  {
    if ((share->table_cache_key.length - TMP_TABLE_KEY_EXTRA) == key_length
        && !memcmp(share->table_cache_key.str, key, key_length))
    {
      result= share;
    }
  }

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(result);
}


/**
  Lookup the TMP_TABLE_SHARE using the given db/table_name.The server_id and
  pseudo_thread_id used to generate table definition key is taken from THD
  (see create_tmp_table_def_key()). Return NULL is none found.

  @return Success                     A pointer to table share object
          Failure                     NULL
*/
TMP_TABLE_SHARE *THD::find_tmp_table_share(const Lex_ident_db &db,
                                           const Lex_ident_table &table_name,
                                           Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_tmp_table_share");

  TMP_TABLE_SHARE *share;
  char key[MAX_DBKEY_LENGTH];
  uint key_length;

  key_length= create_tmp_table_def_key(key, db, table_name);
  share= find_tmp_table_share(key, key_length, find_kind);

  DBUG_RETURN(share);
}


/**
  Lookup TMP_TABLE_SHARE using the specified TABLE_LIST element.
  Return NULL is none found.

  @param tl [IN]                      Table

  @return Success                     A pointer to table share object
          Failure                     NULL
*/
TMP_TABLE_SHARE *THD::find_tmp_table_share(const TABLE_LIST *tl,
                                           Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_tmp_table_share");
  TMP_TABLE_SHARE *share= find_tmp_table_share(tl->get_db_name(),
                                               tl->get_table_name(),
                                               find_kind);
  DBUG_RETURN(share);
}


/**
  Lookup TMP_TABLE_SHARE using the specified table definition key.
  Return NULL is none found.

  @return Success                     A pointer to table share object
          Failure                     NULL
*/
TMP_TABLE_SHARE *THD::find_tmp_table_share(const char *key, size_t key_length,
                                           Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_tmp_table_share");

  TMP_TABLE_SHARE *share;
  TMP_TABLE_SHARE *result= NULL;
  bool locked;

  if (!has_temporary_tables())
  {
    DBUG_RETURN(NULL);
  }

  locked= lock_temporary_tables();

  All_tmp_tables_list::Iterator it(*temporary_tables);
  while ((share= it++))
  {
    if (share->table_cache_key.length == key_length &&
    !memcmp(share->table_cache_key.str, key, key_length) &&
    (find_kind == Tmp_table_kind::ANY ||
     share->global_tmp_table() == (find_kind == Tmp_table_kind::GLOBAL)))
    {
      result= share;
      break;
    }
  }

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(result);
}

void THD::global_tmp_tables_set_explicit_lock_duration()
{
  bool locked= lock_temporary_tables();

  All_tmp_tables_list::Iterator it(*temporary_tables);
  while (TMP_TABLE_SHARE *share= it++)
  {
    if (!share->global_tmp_table())
      continue;
    mdl_context.set_lock_duration(share->mdl_request.ticket, MDL_EXPLICIT);
  }

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }
}

bool THD::use_real_global_temporary_share(const TABLE_LIST *table) const
{
  return table->open_strategy == TABLE_LIST::OPEN_STUB ||
         table->open_strategy == TABLE_LIST::OPEN_FOR_LOCKED_TABLES_LIST ||
         (sql_command_flags() & (CF_ALTER_TABLE   |
                                 CF_SCHEMA_CHANGE |
                                 CF_STATUS_COMMAND)
        && lex->sql_command != SQLCOM_CREATE_TABLE) ||
        lex->sql_command == SQLCOM_CREATE_VIEW      ||
        lex->sql_command == SQLCOM_TRUNCATE         ||
        lex->sql_command == SQLCOM_LOCK_TABLES      ||
        stmt_arena->is_stmt_prepare();
}

/**
  Find a temporary table specified by TABLE_LIST instance in the open table
  list, and open a TABLE handle, without initializing it.

  @param tl [IN]                      TABLE_LIST
  @param table [out]                  TABLE handle found/opened
*/
bool THD::open_temporary_table_impl(TABLE_LIST *tl, TABLE **table,
                                    Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::internal_open_temporary_table");
  /*
    Temporary tables are not safe for parallel replication. They were
    designed to be visible to one thread only, so have no table locking.
    Thus, there is no protection against two conflicting transactions
    committing in parallel and things like that.

    So for now, anything that uses temporary tables will be serialised
    with anything before it, when using parallel replication.
  */

  if (rgi_slave &&
      rgi_slave->is_parallel_exec &&
      find_temporary_table(tl) &&
      wait_for_prior_commit())
    DBUG_RETURN(true);

  /*
    First check if there is a reusable open table available in the
    open table list.
  */
  if (find_and_use_tmp_table(tl, table, find_kind))
  {
    DBUG_RETURN(true);                          /* Error */
  }

  /*
    No reusable table was found. We will have to open a new instance.
  */
  TMP_TABLE_SHARE *tmp_share;
  if (!*table && (tmp_share= find_tmp_table_share(tl, find_kind)))
  {

    *table= open_temporary_table(tmp_share, tl->get_table_name());
    /*
       Temporary tables are not safe for parallel replication. They were
       designed to be visible to one thread only, so have no table locking.
       Thus, there is no protection against two conflicting transactions
       committing in parallel and things like that.

       So for now, anything that uses temporary tables will be serialised
       with anything before it, when using parallel replication.
    */
    if (*table && rgi_slave &&
        rgi_slave->is_parallel_exec &&
        wait_for_prior_commit())
      DBUG_RETURN(true);

    if (!*table && is_error())
      DBUG_RETURN(true);                        // Error when opening table
  }
  DBUG_RETURN(false);
}

/**
  Find a temporary table specified by TABLE_LIST instance in the open table
  list and prepare its TABLE instance for use. If

  This function tries to resolve this table in the list of temporary tables
  of this thread. Temporary tables are thread-local and "shadow" base
  tables with the same name.

  @note In most cases one should use THD::open_tables() instead
        of this call.

  @note One should finalize process of opening temporary table for table
        list element by calling open_and_process_table(). This function
        is responsible for table version checking and handling of merge
        tables.

  @note We used to check global_read_lock before opening temporary tables.
        However, that limitation was artificial and is removed now.

  @param tl [IN]                      TABLE_LIST

  @return Error status.
    @retval false                     On success. If a temporary table exists
                                      for the given key, tl->table is set.
    @retval true                      On error. my_error() has been called.
*/
bool THD::open_temporary_table(TABLE_LIST *tl)
{
  DBUG_ENTER("THD::open_temporary_table");
  DBUG_PRINT("enter", ("table: '%s'.'%s'", tl->db.str, tl->table_name.str));

  TABLE *table= NULL;

  /*
    Code in open_table() assumes that TABLE_LIST::table can be non-zero only
    for pre-opened temporary tables.
  */
  DBUG_ASSERT(tl->table == NULL);

  /*
    This function should not be called for cases when derived or I_S
    tables can be met since table list elements for such tables can
    have invalid db or table name.
    Instead THD::open_tables() should be used.
  */
  DBUG_ASSERT(!tl->derived);
  DBUG_ASSERT(!tl->schema_table);
  DBUG_ASSERT(has_temporary_tables() ||
              (rgi_slave && rgi_slave->is_parallel_exec));

  if (tl->open_type == OT_BASE_ONLY)
  {
    DBUG_PRINT("info", ("skip_temporary is set or no temporary tables"));
    DBUG_RETURN(false);
  }

  if (!tl->db.str)
  {
    DBUG_PRINT("info",
               ("Table reference to a temporary table must have database set"));
    DBUG_RETURN(false);
  }

  if (unlikely(open_temporary_table_impl(tl, &table, Tmp_table_kind::TMP)))
    DBUG_RETURN(true);

  if (!table)
  {
    if (tl->open_type == OT_TEMPORARY_ONLY &&
        tl->open_strategy == TABLE_LIST::OPEN_NORMAL)
    {
      my_error(ER_NO_SUCH_TABLE, MYF(0), tl->db.str, tl->table_name.str);
      DBUG_RETURN(true);
    }
    DBUG_RETURN(false);
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (tl->partition_names)
  {
    /* Partitioned temporary tables is not supported. */
    DBUG_ASSERT(!table->part_info);
    my_error(ER_PARTITION_CLAUSE_ON_NONPARTITIONED, MYF(0));
    DBUG_RETURN(true);
  }
#endif

  table->query_id= query_id;
  used|= THREAD_SPECIFIC_USED;

  /* It is neither a derived table nor non-updatable view. */
  tl->updatable= true;
  tl->table= table;

  table->init(this, tl);

  DBUG_PRINT("info", ("Using temporary table"));
  DBUG_RETURN(false);
}


bool THD::check_and_open_tmp_table(TABLE_LIST *tl)
{
  if (!has_temporary_tables() ||
      tl == lex->first_not_own_table() ||
      tl->derived || tl->schema_table)
    return false;

  return open_temporary_table(tl);
}


/**
  Pre-open temporary tables corresponding to table list elements.

  @note One should finalize process of opening temporary tables
        by calling open_tables(). This function is responsible
        for table version checking and handling of merge tables.

  @param tl [IN]                      TABLE_LIST

  @return false                       On success. If a temporary table exists
                                      for the given element, tl->table is set.
          true                        On error. my_error() has been called.
*/
bool THD::open_temporary_tables(TABLE_LIST *tl)
{
  TABLE_LIST *first_not_own;
  DBUG_ENTER("THD::open_temporary_tables");

  if (!has_temporary_tables())
    DBUG_RETURN(0);

  first_not_own= lex->first_not_own_table();
  for (TABLE_LIST *table= tl; table && table != first_not_own;
       table= table->next_global)
  {
    if (table->derived || table->schema_table)
    {
      /*
        Derived and I_S tables will be handled by a later call to open_tables().
      */
      continue;
    }

    if (open_temporary_table(table))
    {
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}


/**
  Close all temporary tables created by 'CREATE TEMPORARY TABLE' for thread
  creates one DROP TEMPORARY TABLE binlog event for each pseudo-thread.

  Temporary tables created in a sql slave is closed by
  Relay_log_info::close_temporary_tables().

  @return false                       Success
          true                        Failure
*/
bool THD::close_temporary_tables()
{
  DBUG_ENTER("THD::close_temporary_tables");

  TMP_TABLE_SHARE *share;
  TABLE *table;

  bool error= false;

  if (!has_thd_temporary_tables())
  {
    if (temporary_tables)
    {
      my_free(temporary_tables);
      temporary_tables= NULL;
    }
    DBUG_RETURN(false);
  }

  DBUG_ASSERT(!rgi_slave);

  /*
    Ensure we don't have open HANDLERs for tables we are about to close.
    This is necessary when THD::close_temporary_tables() is called as
    part of execution of BINLOG statement (e.g. for format description event).
  */
  mysql_ha_rm_temporary_tables(this);

  /* Close all open temporary tables. */
  All_tmp_tables_list::Iterator it(*temporary_tables);
  while ((share= it++))
  {
    /* Traverse the table list. */
    while ((table= share->all_tmp_tables.pop_front()))
    {
      table->file->extra(HA_EXTRA_PREPARE_FOR_DROP);
      free_temporary_table(table);
    }
  }

  // Write DROP TEMPORARY TABLE query log events to binary log.
  if (mysql_bin_log.is_open())
  {
    error= log_events_and_free_tmp_shares();
  }
  else
  {
    while ((share= temporary_tables->pop_front()))
    {
      free_tmp_table_share(share, true);
    }
  }

  /* By now, there mustn't be any elements left in the list. */
  DBUG_ASSERT(temporary_tables->is_empty());

  my_free(temporary_tables);
  temporary_tables= NULL;

  DBUG_RETURN(error);
}


/**
  Rename a temporary table.

  @param table [IN]                   Table handle
  @param db [IN]                      New schema name
  @param table_name [IN]              New table name

  @return false                       Success
          true                        Error
*/
bool THD::rename_temporary_table(TABLE *table,
                                 const LEX_CSTRING *db,
                                 const LEX_CSTRING *table_name)
{
  char *key;
  uint key_length;
  TABLE_SHARE *share= table->s;
  DBUG_ENTER("THD::rename_temporary_table");

  if (!(key= (char *) alloc_root(&share->mem_root, MAX_DBKEY_LENGTH)))
    DBUG_RETURN(true);

  /*
    Temporary tables are renamed by simply changing their table definition key.
  */
  key_length= create_tmp_table_def_key(key, Lex_ident_db(*db),
                                            Lex_ident_table(*table_name));
  share->set_table_cache_key(key, key_length);

  DBUG_RETURN(false);
}


/**
  Drop a temporary table.

  Try to locate the table in the list of open temporary tables.
  If the table is found:
   - If the table is locked with LOCK TABLES or by prelocking,
     unlock it and remove it from the list of locked tables
     (THD::lock). Currently only transactional temporary tables
     are locked.
   - Close the temporary table, remove its .FRM.
   - Remove the table share from the list of temporary table shares.

  This function is used to drop user temporary tables, as well as
  internal tables created in CREATE TEMPORARY TABLE ... SELECT
  or ALTER TABLE.

  @param table [IN]                   Temporary table to be deleted
  @param is_trans [OUT]               Is set to the type of the table:
                                      transactional (e.g. innodb) as true or
                                      non-transactional (e.g. myisam) as false.
  @param delete_table [IN]            Whether to delete the table files?

  @return false                       Table was dropped
          true                        Error
*/
bool THD::drop_temporary_table(TABLE *table, bool *is_trans, bool delete_table)
{
  DBUG_ENTER("THD::drop_temporary_table");

  TMP_TABLE_SHARE *share;

  DBUG_ASSERT(table);
  DBUG_PRINT("tmptable", ("Dropping table: '%s'.'%s'",
                          table->s->db.str, table->s->table_name.str));

  // close all handlers in case it is statement abort and some can be left
  table->file->ha_reset();
  if (is_trans)
    *is_trans= table->file->has_transactions();

  share= tmp_table_share(table);
  DBUG_RETURN(drop_tmp_table_share(table, share, delete_table));
}

bool THD::drop_tmp_table_share(TABLE *table, TMP_TABLE_SHARE *share,
                               bool delete_table)
{
  DBUG_ENTER("THD::drop_tmp_table_share");
  TABLE *tab;
  bool result= false;

  bool locked= lock_temporary_tables();

  if (table)
  {
    /* Table might be in use by some outer statement. */
    All_share_tables_list::Iterator it(share->all_tmp_tables);
    while ((tab= it++))
    {
      if (tab != table && tab->query_id != 0)
      {
        /* Found a table instance in use. This table cannot be dropped. */
        my_error(ER_CANT_REOPEN_TABLE, MYF(0), table->alias.c_ptr());
        result= true;
        goto end;
      }
    }
  }


  /*
    Iterate over the list of open tables and close them.
  */
  while ((tab= share->all_tmp_tables.pop_front()))
  {
    /*
      We need to set the THD as it may be different in case of
      parallel replication
    */
    tab->in_use= this;
    if (delete_table)
      tab->file->extra(HA_EXTRA_PREPARE_FOR_DROP);
    free_temporary_table(tab);
  }

  DBUG_ASSERT(temporary_tables);

  /* Remove the TABLE_SHARE from the list of temporary tables. */
  temporary_tables->remove(share);

  /* Free the TABLE_SHARE and/or delete the files. */
  result= free_tmp_table_share(share, delete_table);

end:
  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(result);
}


/**
  Delete the temporary table files.

  @param base [IN]                    Handlerton for table to be deleted.
  @param path [IN]                    Path to the table to be deleted (i.e. path
                                      to its .frm without an extension).

  @return false                       Success
          true                        Error
*/
bool THD::rm_temporary_table(handlerton *base, const char *path)
{
  DBUG_ENTER("THD::rm_temporary_table");

  bool error= false;
  char frm_path[FN_REFLEN + 1];

  strxnmov(frm_path, sizeof(frm_path) - 1, path, reg_ext, NullS);

  if (base->drop_table(base, path) > 0)
  {
    error= true;
    sql_print_warning("Could not remove temporary table: '%s', error: %d",
                      path, my_errno);
  }

  if (mysql_file_delete(key_file_frm, frm_path,
                        MYF(MY_WME | MY_IGNORE_ENOENT)))
    error= true;

  DBUG_RETURN(error);
}


/**
  Mark all temporary tables which were used by the current statement or
  sub-statement as free for reuse, but only if the query_id can be cleared.

  @remark For temp tables associated with a open SQL HANDLER the query_id
          is not reset until the HANDLER is closed.
*/
void THD::mark_tmp_tables_as_free_for_reuse()
{
  DBUG_ENTER("THD::mark_tmp_tables_as_free_for_reuse");

  TMP_TABLE_SHARE *share;
  TABLE *table;
  bool locked;

  if (query_id == 0)
  {
    /*
      Thread has not executed any statement and has not used any
      temporary tables.
    */
    DBUG_ASSERT(!rgi_slave || !temporary_tables || temporary_tables->committed);
    DBUG_VOID_RETURN;
  }

  if (!has_temporary_tables())
  {
    DBUG_VOID_RETURN;
  }

  locked= lock_temporary_tables();

  All_tmp_tables_list::Iterator it(*temporary_tables);
  while ((share= it++))
  {
    All_share_tables_list::Iterator tables_it(share->all_tmp_tables);
    while ((table= tables_it++))
    {
      if ((table->query_id == query_id) && !table->open_by_handler)
        mark_tmp_table_as_free_for_reuse(table);
    }
  }

  if (temporary_tables->committed)
  {
    temporary_tables->committed= false;
    drop_on_commit_delete_tables();
  }

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  if (rgi_slave)
  {
    /*
      Temporary tables are shared with other by sql execution threads.
      As a safety measure, clear the pointer to the common area.
    */
    temporary_tables= NULL;
  }

  DBUG_VOID_RETURN;
}


/**
  Reset a single temporary table. Effectively this "closes" one temporary
  table in a session.

  @param table              Temporary table

  @return void
*/
void THD::mark_tmp_table_as_free_for_reuse(TABLE *table)
{
  DBUG_ENTER("THD::mark_tmp_table_as_free_for_reuse");

  DBUG_ASSERT(table->s->tmp_table);

  /*
    Ensure that table changes were either binary logged or the table
    is marked as not up to date.
  */
  if (!tmp_table_binlog_handled &&            // Not logged to binlog
      table->s->using_binlog() &&             // Table should be using binlog
      table->file->mark_trx_read_write_done)  // Changes where done
  {
    /* We should only come here is binlog is not open */
    DBUG_ASSERT(!mysql_bin_log.is_open());
    /* Mark the table as not up to date */
    table->mark_as_not_binlogged();
  }

  table->pos_in_table_list= NULL;
  table->query_id= 0;
  table->file->ha_reset();

  /* Detach temporary MERGE children from temporary parent. */
  DBUG_ASSERT(table->file);
  table->file->extra(HA_EXTRA_DETACH_CHILDREN);

  /*
    Reset temporary table lock type to it's default value (TL_WRITE).

    Statements such as INSERT INTO .. SELECT FROM tmp, CREATE TABLE
    .. SELECT FROM tmp and UPDATE may under some circumstances modify
    the lock type of the tables participating in the statement. This
    isn't a problem for non-temporary tables since their lock type is
    reset at every open, but the same does not occur for temporary
    tables for historical reasons.

    Furthermore, the lock type of temporary tables is not really that
    important because they can only be used by one query at a time.
    Nonetheless, it's safer from a maintenance point of view to reset
    the lock type of this singleton TABLE object as to not cause problems
    when the table is reused.

    Even under LOCK TABLES mode its okay to reset the lock type as
    LOCK TABLES is allowed (but ignored) for a temporary table.
  */
  table->reginfo.lock_type= TL_WRITE;
  DBUG_VOID_RETURN;
}


/**
  Remove and return the specified table's TABLE_SHARE from the temporary
  tables list.

  @param table [IN]                   Table

  @return TMP_TABLE_SHARE of the specified table.
*/
TMP_TABLE_SHARE *THD::save_tmp_table_share(TABLE *table)
{
  DBUG_ENTER("THD::save_tmp_table_share");

  TMP_TABLE_SHARE *share;

  lock_temporary_tables();
  DBUG_ASSERT(temporary_tables);
  share= tmp_table_share(table);
  temporary_tables->remove(share);
  unlock_temporary_tables();

  DBUG_RETURN(share);
}


/**
  Add the specified TMP_TABLE_SHARE to the temporary tables list.

  @param share [IN]                   Table share

  @return void
*/
void THD::restore_tmp_table_share(TMP_TABLE_SHARE *share)
{
  DBUG_ENTER("THD::restore_tmp_table_share");

  lock_temporary_tables();
  DBUG_ASSERT(temporary_tables);
  temporary_tables->push_front(share);
  unlock_temporary_tables();

  DBUG_VOID_RETURN;
}


/**
  If its a replication slave, report whether slave temporary tables
  exist (Relay_log_info::save_temporary_tables) or report about THD
  temporary table (Open_tables_state::temporary_tables) otherwise.
  Note start-new-trans context is not about replication transaction
  in which case the function uses the non-slave normal branch.

  @return false                       Temporary tables exist
          true                        No temporary table exist
*/
bool THD::has_temporary_tables()
{
  DBUG_ENTER("THD::has_temporary_tables");
  bool result;
#ifdef HAVE_REPLICATION
  /*
    Slave applier thread may execute an out-of-band "new-transaction"
    and do so in the middle of a replicated transaction processing.
    All functions that open the access to slave temporary table repository
    including the current one have to deny it within the start-new-transaction
    context.
  */
  if (not_new_trans(rgi_slave))
  {
    mysql_mutex_lock(&rgi_slave->rli->data_lock);
    result= rgi_slave->rli->save_temporary_tables &&
      !rgi_slave->rli->save_temporary_tables->is_empty();
    mysql_mutex_unlock(&rgi_slave->rli->data_lock);
  }
  else
#endif
  {
    result= has_thd_temporary_tables();
  }
  DBUG_RETURN(result);
}


/**
  Create a table definition key.

  @param key [OUT]                    Buffer for the key to be created (must
                                      be of size MAX_DBKRY_LENGTH)
  @param db [IN]                      Database name
  @param table_name [IN]              Table name

  @return                             Key length.

  @note
    The table key is create from:
    db + \0
    table_name + \0

    Additionally, we add the following to make each temporary table unique on
    the slave.

    4 bytes of master thread id
    4 bytes of pseudo thread id
*/
uint THD::create_tmp_table_def_key(char *key,
                                   const Lex_ident_db &db,
                                   const Lex_ident_table &table_name)
{
  uint key_length;
  DBUG_ENTER("THD::create_tmp_table_def_key");
  ulonglong server_id= rgi_slave ? variables.server_id : 0;

  key_length= tdc_create_key(key, db.str, table_name.str);
  int4store(key + key_length, server_id);
  int4store(key + key_length + 4, variables.pseudo_thread_id);
  key_length += TMP_TABLE_KEY_EXTRA;

  DBUG_RETURN(key_length);
}


/**
  Create a temporary table.

  @param frm  [IN]                    Binary frm image
  @param path [IN]                    File path (without extension)
  @param db   [IN]                    Schema name
  @param table_name [IN]              Table name

  @return Success                     A pointer to table share object
          Failure                     NULL
*/
TMP_TABLE_SHARE *THD::create_temporary_table(LEX_CUSTRING *frm,
                                             const char *path,
                                             const Lex_ident_db &db,
                                             const Lex_ident_table &table_name)
{
  DBUG_ENTER("THD::create_temporary_table");

  TMP_TABLE_SHARE *share;
  char key_cache[MAX_DBKEY_LENGTH];
  char *saved_key_cache;
  char *tmp_path;
  uint key_length;
  bool locked;
  int res;

  /* Temporary tables are not safe for parallel replication. */
  if (rgi_slave &&
      rgi_slave->is_parallel_exec &&
      wait_for_prior_commit())
    DBUG_RETURN(NULL);

  /* Create the table definition key for the temporary table. */
  key_length= create_tmp_table_def_key(key_cache, db, table_name);

  if (!(share= (TMP_TABLE_SHARE *) my_malloc(key_memory_table_share,
                                             sizeof(TMP_TABLE_SHARE) +
                                             strlen(path) + 1 + key_length,
                                             MYF(MY_WME))))
  {
    DBUG_RETURN(NULL);                          /* Out of memory */
  }

  tmp_path= (char *)(share + 1);
  saved_key_cache= strmov(tmp_path, path) + 1;
  memcpy(saved_key_cache, key_cache, key_length);

  /*
    Temp tables can't be thread specific for slaves as they are freed
    during cleanup() from Relay_log_info::close_temporary_tables()
  */
  init_tmp_table_share(this, share, saved_key_cache, key_length,
                       strend(saved_key_cache) + 1, tmp_path,
		       !not_new_trans(rgi_slave));

  /*
    Prefer using frm image over file. The image might not be available in
    ALTER TABLE, when the discovering engine took over the ownership (see
    TABLE::read_frm_image).
  */
  res= (frm->str)
    ? share->init_from_binary_frm_image(this, false, frm->str, frm->length)
    : open_table_def(this, share, GTS_TABLE | GTS_USE_DISCOVERY);

  if (res)
  {
    /*
      No need to lock share->mutex as this is not needed for temporary tables.
    */
    free_table_share(share);
    my_free(share);
    DBUG_RETURN(NULL);
  }

  share->m_psi= PSI_CALL_get_table_share(true, share);

  locked= lock_temporary_tables();

  /* Initialize the all_tmp_tables list. */
  share->all_tmp_tables.empty();
  share->mdl_request = {};

  /*
    We need to alloc & initialize temporary_tables if this happens
    to be the very first temporary table.
  */
  if (!temporary_tables)
  {
    if ((temporary_tables=
         (All_tmp_tables_list *) my_malloc(key_memory_table_share,
                                           sizeof(All_tmp_tables_list),
                                           MYF(MY_WME))))
    {
      temporary_tables->empty();
    }
    else
    {
      DBUG_RETURN(NULL);                        /* Out of memory */
    }
  }

  /* Add share to the head of the temporary table share list. */
  temporary_tables->push_front(share);

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(share);
}


/**
  Find a table with the specified key.

  @param key [IN]                     Key
  @param key_length [IN]              Key length
  @param state [IN]                   Open table state to look for

  @return Success                     Pointer to the table instance.
          Failure                     NULL
*/
TABLE *THD::find_temporary_table(const char *key, uint key_length,
                                 Temporary_table_state state,
                                 Tmp_table_kind find_kind)
{
  DBUG_ENTER("THD::find_temporary_table");

  TMP_TABLE_SHARE *share;
  TABLE *table;
  TABLE *result= NULL;
  bool locked;

  locked= lock_temporary_tables();

  All_tmp_tables_list::Iterator it(*temporary_tables);
  while ((share= it++))
  {
    if (share->table_cache_key.length == key_length &&
        !memcmp(share->table_cache_key.str, key, key_length) &&
        (find_kind == Tmp_table_kind::ANY ||
         share->global_tmp_table() == (find_kind == Tmp_table_kind::GLOBAL)))
    {
      /* A matching TMP_TABLE_SHARE is found. */

      All_share_tables_list::Iterator tables_it(share->all_tmp_tables);

      bool found= false;
      while (!found && (table= tables_it++))
      {
        switch (state)
        {
        case TMP_TABLE_IN_USE:     found= table->query_id > 0;  break;
        case TMP_TABLE_NOT_IN_USE: found= table->query_id == 0; break;
        case TMP_TABLE_ANY:        found= true;                 break;
        }
      }
      if (table && unlikely(table->needs_reopen()))
      {
        share->all_tmp_tables.remove(table);
        free_temporary_table(table);
        if (share->all_tmp_tables.is_empty())
          table= open_temporary_table(share, share->table_name);
        else
        {
          it.rewind();
          continue;
        }
      }
      result= table;
      break;
    }
  }

  if (locked)
  {
    DBUG_ASSERT(m_tmp_tables_locked);
    unlock_temporary_tables();
  }

  DBUG_RETURN(result);
}



/**
  Open a table from the specified TABLE_SHARE with the given alias.

  @param share [IN]                   Table share
  @param alias [IN]                   Table alias

  @return Success                     A pointer to table object
          Failure                     NULL
*/
TABLE *THD::open_temporary_table(TMP_TABLE_SHARE *share,
                                 const Lex_ident_table &alias)
{
  TABLE *table;
  DBUG_ENTER("THD::open_temporary_table");


  if (!(table= (TABLE *) my_malloc(key_memory_TABLE, sizeof(TABLE),
                                   MYF(MY_WME))))
  {
    DBUG_RETURN(NULL);                          /* Out of memory */
  }

  uint flags= ha_open_options | (open_options & HA_OPEN_FOR_CREATE);
  /*
    In replication, temporary tables are not confined to a single
    thread/THD.
  */
  if (not_new_trans(rgi_slave))
    flags|= HA_OPEN_GLOBAL_TMP_TABLE;
  if (open_table_from_share(this, share, &alias,
                            (uint) HA_OPEN_KEYFILE,
                            EXTRA_RECORD, flags,
                            table, false))
  {
    my_free(table);
    DBUG_RETURN(NULL);
  }

  table->reginfo.lock_type= TL_WRITE;           /* Simulate locked */
  table->grant.privilege= TMP_TABLE_ACLS;
  table->query_id= query_id;
  share->tmp_table= (table->file->has_transaction_manager() ?
                     TRANSACTIONAL_TMP_TABLE : NON_TRANSACTIONAL_TMP_TABLE);
  share->not_usable_by_query_cache= 1;

  /* Add table to the head of table list. */
  share->all_tmp_tables.push_front(table);

  /* Increment Slave_open_temp_table_definitions status variable count. */
  if (not_new_trans(rgi_slave))
    slave_open_temp_tables++;

  DBUG_PRINT("tmptable", ("Opened table: '%s'.'%s  table: %p",
                          table->s->db.str,
                          table->s->table_name.str, table));
  DBUG_RETURN(table);
}


/**
  Find a reusable table in the open table list using the specified TABLE_LIST.

  @param tl [IN]                      Table list
  @param out_table [OUT]              Pointer to the requested TABLE object

  @return Success                     false
          Failure                     true
*/
bool THD::find_and_use_tmp_table(const TABLE_LIST *tl, TABLE **out_table,
                                 Tmp_table_kind find_kind)
{
  char key[MAX_DBKEY_LENGTH];
  uint key_length;
  bool result;
  DBUG_ENTER("THD::find_and_use_tmp_table");

  key_length= create_tmp_table_def_key(key, tl->get_db_name(),
                                       tl->get_table_name());
  result= use_temporary_table(find_temporary_table(key, key_length,
                                                   TMP_TABLE_NOT_IN_USE,
                                                   find_kind),
                              out_table);
  DBUG_RETURN(result);
}

/**
  Mark table as in-use.

  @param table [IN]                   Table to be marked in-use
  @param out_table [OUT]              Pointer to the specified table

  @return false                       Success
          true                        Error
*/
bool THD::use_temporary_table(TABLE *table, TABLE **out_table)
{
  DBUG_ENTER("THD::use_temporary_table");

  *out_table= table;

  /* The following can happen if find_temporary_table() returns NULL */
  if (!table)
    DBUG_RETURN(false);

  /*
    Temporary tables are not safe for parallel replication. They were
    designed to be visible to one thread only, so have no table locking.
    Thus there is no protection against two conflicting transactions
    committing in parallel and things like that.

    So for now, anything that uses temporary tables will be serialised
    with anything before it, when using parallel replication.

    TODO: We might be able to introduce a reference count or something
    on temp tables, and have slave worker threads wait for it to reach
    zero before being allowed to use the temp table. Might not be worth
    it though, as statement-based replication using temporary tables is
    in any case rather fragile.
  */
  if (rgi_slave &&
      rgi_slave->is_parallel_exec &&
      wait_for_prior_commit())
    DBUG_RETURN(true);

  /*
    We need to set the THD as it may be different in case of
    parallel replication
  */
  table->in_use= this;
  if (table->s->global_tmp_table())
    use_global_tmp_table_tp();

  DBUG_RETURN(false);
}


/**
  Close a temporary table.

  @param table [IN]                   Table handle

  @return void
*/
void THD::close_temporary_table(TABLE *table)
{
  DBUG_ENTER("THD::close_temporary_table");

  DBUG_PRINT("tmptable", ("closing table: '%s'.'%s'%p  alias: '%s'",
                          table->s->db.str, table->s->table_name.str,
                          table, table->alias.c_ptr()));

  closefrm(table);
  my_free(table);

  if (rgi_slave)
  {
    /* Natural invariant of temporary_tables */
    DBUG_ASSERT(slave_open_temp_tables || !temporary_tables);
    /* Decrement Slave_open_temp_table_definitions status variable count. */
    slave_open_temp_tables--;
  }

  DBUG_VOID_RETURN;
}

static const char drop_table_stub[]= "DROP TEMPORARY TABLE IF EXISTS ";
static const char rename_table_stub[]= "RENAME TABLE ";

int THD::commit_global_tmp_tables()
{
  DBUG_ASSERT(!rgi_slave);
  if (has_open_global_temporary_tables())
    temporary_tables->committed= true;
  return 0;
}

int THD::drop_on_commit_delete_tables()
{
  int error= 0;
  All_tmp_tables_list::Iterator it(*temporary_tables);
  while (TMP_TABLE_SHARE *share= it++)
  {
    if (!share->on_commit_delete())
      continue;

    All_share_tables_list::Iterator tab_it(share->all_tmp_tables);
    while (TABLE *table= tab_it++)
    {
      if (table->open_by_handler)
      {
        TABLE_LIST tl(table, TL_WRITE);
        mysql_ha_rm_tables(this, &tl);

        push_warning_printf(this, Sql_condition::WARN_LEVEL_NOTE,
                            ER_ILLEGAL_HA,
                            "Global temporary table %s.%s HANDLER is closed.",
                            table->s->db.str, table->s->table_name.str);
      }
    }

    if (int local_error= drop_tmp_table_share(NULL, share, true))
      error= local_error;
  }
  return error;
}



/**
  Write query log events with "DROP TEMPORARY TABLES .." for each pseudo
  thread to the binary log.

  @return false                       Success
          true                        Error
*/
bool THD::log_events_and_free_tmp_shares()
{
  DBUG_ENTER("THD::log_events_and_free_tmp_shares");

  DBUG_ASSERT(!rgi_slave);

  TMP_TABLE_SHARE *share;
  TMP_TABLE_SHARE *sorted;
  TMP_TABLE_SHARE *prev_sorted;
  // Assume thd->variables.option_bits has OPTION_QUOTE_SHOW_CREATE.
  bool was_quote_show= true;
  bool error= false;
  bool found_user_tables= false;
  // Better add "IF EXISTS" in case a RESET MASTER has been done.
  char buf[FN_REFLEN];

  String s_query(buf, sizeof(buf), system_charset_info);
  s_query.copy(drop_table_stub, sizeof(drop_table_stub) - 1, system_charset_info);

  /*
    Insertion sort of temporary tables by pseudo_thread_id to build ordered
    list of sublists of equal pseudo_thread_id.
  */
  All_tmp_tables_list::Iterator it_sorted(*temporary_tables);
  All_tmp_tables_list::Iterator it_unsorted(*temporary_tables);
  uint sorted_count= 0;
  while((share= it_unsorted++))
  {
    if (is_user_tmp_table(share))
    {
      prev_sorted= NULL;

      if (!found_user_tables) found_user_tables= true;

      for (uint i= 0; i < sorted_count; i ++)
      {
        sorted= it_sorted ++;

        if (!is_user_tmp_table(sorted) ||
            (tmpkeyval(sorted) > tmpkeyval(share)))
        {
          /*
            Insert this share before the current element in
            the sorted part of the list.
          */
          temporary_tables->remove(share);

          if (prev_sorted)
          {
            temporary_tables->insert_after(prev_sorted, share);
          }
          else
          {
            temporary_tables->push_front(share);
          }
          break;
        }
        prev_sorted= sorted;
      }
      it_sorted.rewind();
    }
    sorted_count ++;
  }

  /*
    We always quote db & table names.
  */
  if (found_user_tables &&
      !(was_quote_show= MY_TEST(variables.option_bits &
                                OPTION_QUOTE_SHOW_CREATE)))
  {
    variables.option_bits |= OPTION_QUOTE_SHOW_CREATE;
  }

  /*
    Scan sorted temporary tables to generate sequence of DROP.
  */
  share= temporary_tables->pop_front();
  while (share)
  {
    if (is_user_tmp_table(share))
    {
      used_t save_thread_specific_used= used & THREAD_SPECIFIC_USED;
      my_thread_id save_pseudo_thread_id= variables.pseudo_thread_id;
      char db_buf[FN_REFLEN];
      String db(db_buf, sizeof(db_buf), system_charset_info);
      bool at_least_one_create_logged;

      /*
        Set pseudo_thread_id to be that of the processed table.
      */
      variables.pseudo_thread_id= tmpkeyval(share);

      db.copy(share->db.str, share->db.length, system_charset_info);
      /*
        Reset s_query() if changed by previous loop.
      */
      s_query.length(sizeof(drop_table_stub) - 1);

      /*
        Loop forward through all tables that belong to a common database
        within the sublist of common pseudo_thread_id to create single
        DROP query.
      */
      for (at_least_one_create_logged= false;
           share && is_user_tmp_table(share) &&
           tmpkeyval(share) == variables.pseudo_thread_id &&
           share->db.length == db.length() &&
           memcmp(share->db.str, db.ptr(), db.length()) == 0;
           /* Get the next TABLE_SHARE in the list. */
           share= temporary_tables->pop_front())
      {
        if (share->table_creation_was_logged)
        {
          at_least_one_create_logged= true;
          /*
             We are going to add ` around the table names and possible more
             due to special characters.
          */
          append_identifier(this, &s_query, &share->table_name);
          s_query.append(',');
        }
        rm_temporary_table(share->db_type(), share->path.str);
        free_table_share(share);
        my_free(share);
      }

      if (at_least_one_create_logged)
      {
        clear_error();
        CHARSET_INFO *cs_save= variables.character_set_client;
        variables.character_set_client= system_charset_info;
        used|= THREAD_SPECIFIC_USED;

        s_query.length(s_query.length()-1);      // remove trailing ','
        s_query.append(&generated_by_server);

        Query_log_event qinfo(this, s_query.ptr(), s_query.length(),
                              false, true, false, 0);
        qinfo.db= db.ptr();
        qinfo.db_len= db.length();
        variables.character_set_client= cs_save;

        get_stmt_da()->set_overwrite_status(true);
        transaction->stmt.mark_dropped_temp_table();
        bool error2= mysql_bin_log.write(&qinfo);
        if (unlikely(error|= error2))
        {
          /*
             If we're here following THD::cleanup, thence the connection
             has been closed already. So lets print a message to the
             error log instead of pushing yet another error into the
             stmt_da.

             Also, we keep the error flag so that we propagate the error
             up in the stack. This way, if we're the SQL thread we notice
             that THD::close_tables failed. (Actually, the SQL
             thread only calls THD::close_tables while applying
             old Start_log_event_v3 events.)
          */
          sql_print_error("Failed to write the DROP statement for "
              "temporary tables to binary log");
        }

        get_stmt_da()->set_overwrite_status(false);
      }
      variables.pseudo_thread_id= save_pseudo_thread_id;
      used = (used & ~THREAD_SPECIFIC_USED) | save_thread_specific_used;
    }
    else
    {
      free_tmp_table_share(share, true);
      /* Get the next TABLE_SHARE in the list. */
      share= temporary_tables->pop_front();
    }
  }

  if (!was_quote_show)
  {
    /*
      Restore option.
    */
    variables.option_bits&= ~OPTION_QUOTE_SHOW_CREATE;
  }

  DBUG_RETURN(error);
}


/*
  Log drop of renamed temporary table to binary log

  This function is only called by mysql_rename_table() if of there was
  a rename of temporary table that was not in the binary log. These
  tables are removed from the rename list.

  Note that find_temporary_table_for_rename() has ensured that all
  elements in table_list points to the same temporary table even
  if the table exists in several places in the rename list.
*/

bool THD::binlog_renamed_tmp_tables(TABLE_LIST *table_list)
{
  TABLE_LIST *old_table, *new_table;
  char buf[FN_REFLEN];
  String rename_query(buf, sizeof(buf), system_charset_info);
  bool res= 0;
  DBUG_ENTER("binlog_rename_of_changed_tmp_tables_to_binlog");

  rename_query.copy(rename_table_stub, sizeof(rename_table_stub) - 1,
                     system_charset_info);

  for (old_table= table_list; old_table; old_table= new_table->next_local)
  {
    new_table= old_table->next_local;
    if (!old_table->table ||                            // Normal table
        old_table->table->s->table_creation_was_logged) // Normal or logged tmp
    {
      append_identifier(this, &rename_query, &old_table->db);
      rename_query.append('.');
      append_identifier(this, &rename_query, &old_table->table_name);
      rename_query.append(" TO ", 4);
      append_identifier(this, &rename_query, &new_table->db);
      rename_query.append('.');
      append_identifier(this, &rename_query, &new_table->table_name);
      rename_query.append(',');
    }
  }
  if (rename_query.length() > sizeof(rename_table_stub))
  {
    rename_query.length(rename_query.length()-1);
    rename_query.append(generated_by_server);
    res= write_bin_log(this, FALSE, rename_query.ptr(), rename_query.length());
  }
  DBUG_RETURN(res);
}

/**
  Delete the files and free the specified table share.

  @param share [IN]                   TABLE_SHARE to free
  @param delete_table [IN]            Whether to delete the table files?

  @return false                       Success
          true                        Error
*/
bool THD::free_tmp_table_share(TMP_TABLE_SHARE *share, bool delete_table)
{
  bool error= false;
  DBUG_ENTER("THD::free_tmp_table_share");

  if (delete_table)
  {
    error= rm_temporary_table(share->db_type(), share->path.str);

    if (share->hlindexes())
    {
      /* as of now: only one vector index can be here */
      DBUG_ASSERT(share->hlindexes() == 1);
      rm_temporary_table(share->hlindex->db_type(), share->hlindex->path.str);
    }

    if (share->global_tmp_table() && share->mdl_request.ticket)
    {
      mdl_context.release_lock(share->mdl_request.ticket);
      DBUG_ASSERT(temporary_tables->global_temporary_tables_count > 0);
      --temporary_tables->global_temporary_tables_count;
    }
  }
  free_table_share(share);
  my_free(share);
  DBUG_RETURN(error);
}


/**
  Free the specified table object.

  @param table [IN]                   Table object to free.

  @return void
*/
void THD::free_temporary_table(TABLE *table)
{
  DBUG_ENTER("THD::free_temporary_table");

  /*
    If LOCK TABLES list is not empty and contains this table, unlock the table
    and remove the table from this list.
  */
  mysql_lock_remove(this, lock, table);

  close_temporary_table(table);

  DBUG_VOID_RETURN;
}


/**
  On replication slave, acquire the Relay_log_info's data_lock and use slave
  temporary tables.
  Note start-new-trans context is not about replication transaction
  in which case the function returns false.

  @return true                        Lock acquired
          false                       Lock wasn't acquired
*/
bool THD::lock_temporary_tables()
{
  DBUG_ENTER("THD::lock_temporary_tables");

  /* Do not proceed if a lock has already been taken. */
  if (m_tmp_tables_locked)
  {
    DBUG_RETURN(false);
  }

#ifdef HAVE_REPLICATION
  if (not_new_trans(rgi_slave)) /* see has_temporary_tables comments */
  {
    mysql_mutex_lock(&rgi_slave->rli->data_lock);
    temporary_tables= rgi_slave->rli->save_temporary_tables;
    m_tmp_tables_locked= true;
  }
#endif

  DBUG_RETURN(m_tmp_tables_locked);
}


/**
  On replication slave, release the Relay_log_info::data_lock previously
  acquired to use slave temporary tables.

  @return void
*/
void THD::unlock_temporary_tables()
{
  DBUG_ENTER("THD::unlock_temporary_tables");

  if (!m_tmp_tables_locked)
  {
    DBUG_VOID_RETURN;
  }

#ifdef HAVE_REPLICATION
  if (not_new_trans(rgi_slave))                /* ditto lock */
  {
    rgi_slave->rli->save_temporary_tables= temporary_tables;
    temporary_tables= NULL;                     /* Safety */
    mysql_mutex_unlock(&rgi_slave->rli->data_lock);
    m_tmp_tables_locked= false;
  }
#endif

  DBUG_VOID_RETURN;
}


/**
  Close unused TABLE instances for given temporary table.

  @param tl [IN]                      TABLE_LIST

  Initial use case was TRUNCATE, which expects only one instance (which is used
  by TRUNCATE itself) to be open. Most probably some ALTER TABLE variants and
  REPAIR may have similar expectations.
*/

void THD::close_unused_temporary_table_instances(const TABLE_LIST *tl)
{
  DBUG_ASSERT(tl->table);
  TMP_TABLE_SHARE *share= tmp_table_share(tl->table);

  if (share)
  {
    All_share_tables_list::Iterator tables_it(share->all_tmp_tables);

     while (TABLE *table= tables_it++)
     {
       if (table->query_id == 0)
       {
         /* Note: removing current list element doesn't invalidate iterator. */
         share->all_tmp_tables.remove(table);
         /*
           At least one instance should be left (guaranteed by calling this
           function for table which is opened and the table is under processing)
         */
         DBUG_ASSERT(share->all_tmp_tables.front());
         free_temporary_table(table);
       }
     }
  }
}

static int commit_global_tmp_table(THD *thd, bool all)
{
  if (ending_trans(thd, all))
    return thd->commit_global_tmp_tables();
  return 0;
}

static int xa_commit_global_tmp_table(XID *xid)
{

  THD *thd= current_thd;
  if (thd->transaction->xid_state.get_xid() != xid)
    return 0; // Recovery, nothing to do.

  return commit_global_tmp_table(thd, true);
}

static transaction_participant global_temporary_tp=
{
  0, 0,  HTON_NO_ROLLBACK,
  [](THD *) { return 0; },
  NULL, NULL, NULL, NULL,
  commit_global_tmp_table,       // commit
  commit_global_tmp_table,       // rollback
  [](THD *, bool){ return 0; },  // prepare
  NULL, // recover
  xa_commit_global_tmp_table,   // xa_commit
  xa_commit_global_tmp_table,   // xa_rollback
  NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

void THD::use_global_tmp_table_tp()
{
  if (!(sql_command_flags() & CF_STATUS_COMMAND))
  {
    if (in_multi_stmt_transaction_mode())
      trans_register_ha(this, true, &global_temporary_tp, 0);
    else
      trans_register_ha(this, false, &global_temporary_tp, 0);
  }
}

static int init_global_tmp_table(void *p)
{
  st_plugin_int *plugin= (st_plugin_int *)p;
  plugin->data= &global_temporary_tp;
  return setup_transaction_participant(plugin);
}

struct st_mysql_daemon global_temporary_tables_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION  };

maria_declare_plugin(global_temporary_tables)
{
  MYSQL_DAEMON_PLUGIN,
  &global_temporary_tables_plugin,
  "global_temporary_tables",
  "MariaDB Corp.",
  "This is a plugin to represent the global temporary tables in a transaction",
  PLUGIN_LICENSE_GPL,
  init_global_tmp_table, // Plugin Init
  NULL,   // Plugin Deinit
  0x0200, // 2.0
  NULL,   // no status vars
  NULL,   // no sysvars
  "2.0",
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;
