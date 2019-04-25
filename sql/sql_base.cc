/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2019, MariaDB

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


/* Basic functions needed by many modules */

#include "mariadb.h"
#include "sql_base.h"                           // setup_table_map
#include "sql_priv.h"
#include "unireg.h"
#include "debug_sync.h"
#include "lock.h"        // mysql_lock_remove,
                         // mysql_unlock_tables,
                         // mysql_lock_have_duplicate
#include "sql_show.h"    // append_identifier
#include "strfunc.h"     // find_type
#include "sql_view.h"    // mysql_make_view, VIEW_ANY_ACL
#include "sql_parse.h"   // check_table_access
#include "sql_insert.h"  // kill_delayed_threads
#include "sql_acl.h"     // *_ACL, check_grant_all_columns,
                         // check_column_grant_in_table_ref,
                         // get_column_grant
#include "sql_partition.h"               // ALTER_PARTITION_PARAM_TYPE
#include "sql_derived.h" // mysql_derived_prepare,
                         // mysql_handle_derived,
                         // mysql_derived_filling
#include "sql_handler.h" // mysql_ha_flush
#include "sql_test.h"
#include "sql_partition.h"                      // ALTER_PARTITION_PARAM_TYPE
#include "log_event.h"                          // Query_log_event
#include "sql_select.h"
#include "sp_head.h"
#include "sp.h"
#include "sp_cache.h"
#include "sql_trigger.h"
#include "transaction.h"
#include "sql_prepare.h"
#include "sql_statistics.h"
#include "sql_cte.h"
#include <m_ctype.h>
#include <my_dir.h>
#include <hash.h>
#include "rpl_filter.h"
#include "sql_table.h"                          // build_table_filename
#include "datadict.h"   // dd_frm_is_view()
#include "sql_hset.h"   // Hash_set
#include "rpl_rli.h"   // rpl_group_info
#ifdef  __WIN__
#include <io.h>
#endif
#include "wsrep_mysqld.h"
#ifdef WITH_WSREP
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"
#endif /* WITH_WSREP */


bool
No_such_table_error_handler::handle_condition(THD *,
                                              uint sql_errno,
                                              const char*,
                                              Sql_condition::enum_warning_level *level,
                                              const char*,
                                              Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (sql_errno == ER_NO_SUCH_TABLE || sql_errno == ER_NO_SUCH_TABLE_IN_ENGINE)
  {
    m_handled_errors++;
    return TRUE;
  }

  if (*level == Sql_condition::WARN_LEVEL_ERROR)
    m_unhandled_errors++;
  return FALSE;
}


bool No_such_table_error_handler::safely_trapped_errors()
{
  /*
    If m_unhandled_errors != 0, something else, unanticipated, happened,
    so the error is not trapped but returned to the caller.
    Multiple ER_NO_SUCH_TABLE can be raised in case of views.
  */
  return ((m_handled_errors > 0) && (m_unhandled_errors == 0));
}


/**
  This internal handler is used to trap ER_NO_SUCH_TABLE and
  ER_WRONG_MRG_TABLE errors during CHECK/REPAIR TABLE for MERGE
  tables.
*/

class Repair_mrg_table_error_handler : public Internal_error_handler
{
public:
  Repair_mrg_table_error_handler()
    : m_handled_errors(false), m_unhandled_errors(false)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl);

  /**
    Returns TRUE if there were ER_NO_SUCH_/WRONG_MRG_TABLE and there
    were no unhandled errors. FALSE otherwise.
  */
  bool safely_trapped_errors()
  {
    /*
      Check for m_handled_errors is here for extra safety.
      It can be useful in situation when call to open_table()
      fails because some error which was suppressed by another
      error handler (e.g. in case of MDL deadlock which we
      decided to solve by back-off and retry).
    */
    return (m_handled_errors && (! m_unhandled_errors));
  }

private:
  bool m_handled_errors;
  bool m_unhandled_errors;
};


bool
Repair_mrg_table_error_handler::handle_condition(THD *,
                                                 uint sql_errno,
                                                 const char*,
                                                 Sql_condition::enum_warning_level *level,
                                                 const char*,
                                                 Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (sql_errno == ER_NO_SUCH_TABLE ||
      sql_errno == ER_NO_SUCH_TABLE_IN_ENGINE ||
      sql_errno == ER_WRONG_MRG_TABLE)
  {
    m_handled_errors= true;
    return TRUE;
  }

  m_unhandled_errors= true;
  return FALSE;
}


/**
  @defgroup Data_Dictionary Data Dictionary
  @{
*/

static bool check_and_update_table_version(THD *thd, TABLE_LIST *tables,
                                           TABLE_SHARE *table_share);
static bool open_table_entry_fini(THD *thd, TABLE_SHARE *share, TABLE *entry);
static bool auto_repair_table(THD *thd, TABLE_LIST *table_list);


/**
  Get table cache key for a table list element.

  @param table_list[in]  Table list element.
  @param key[out]        On return points to table cache key for the table.

  @note Unlike create_table_def_key() call this function doesn't construct
        key in a buffer provided by caller. Instead it relies on the fact
        that table list element for which key is requested has properly
        initialized MDL_request object and the fact that table definition
        cache key is suffix of key used in MDL subsystem. So to get table
        definition key it simply needs to return pointer to appropriate
        part of MDL_key object nested in this table list element.
        Indeed, this means that lifetime of key produced by this call is
        limited by the lifetime of table list element which it got as
        parameter.

  @return Length of key.
*/

uint get_table_def_key(const TABLE_LIST *table_list, const char **key)
{
  /*
    This call relies on the fact that TABLE_LIST::mdl_request::key object
    is properly initialized, so table definition cache can be produced
    from key used by MDL subsystem.
  */
  DBUG_ASSERT(!strcmp(table_list->get_db_name(),
                      table_list->mdl_request.key.db_name()));
  DBUG_ASSERT(!strcmp(table_list->get_table_name(),
                      table_list->mdl_request.key.name()));

  *key= (const char*)table_list->mdl_request.key.ptr() + 1;
  return table_list->mdl_request.key.length() - 1;
}



/*****************************************************************************
  Functions to handle table definition cache (TABLE_SHARE)
*****************************************************************************/

/*
  Create a list for all open tables matching SQL expression

  SYNOPSIS
    list_open_tables()
    thd			Thread THD
    wild		SQL like expression

  NOTES
    One gets only a list of tables for which one has any kind of privilege.
    db and table names are allocated in result struct, so one doesn't need
    a lock when traversing the return list.

  RETURN VALUES
    NULL	Error (Probably OOM)
    #		Pointer to list of names of open tables.
*/

struct list_open_tables_arg
{
  THD *thd;
  const char *db;
  const char *wild;
  TABLE_LIST table_list;
  OPEN_TABLE_LIST **start_list, *open_list;
};


static my_bool list_open_tables_callback(TDC_element *element,
                                         list_open_tables_arg *arg)
{
  const char *db= (char*) element->m_key;
  size_t db_length= strlen(db);
  const char *table_name= db + db_length + 1;

  if (arg->db && my_strcasecmp(system_charset_info, arg->db, db))
    return FALSE;
  if (arg->wild && wild_compare(table_name, arg->wild, 0))
    return FALSE;

  /* Check if user has SELECT privilege for any column in the table */
  arg->table_list.db.str= db;
  arg->table_list.db.length= db_length;
  arg->table_list.table_name.str= table_name;
  arg->table_list.table_name.length= strlen(table_name);
  arg->table_list.grant.privilege= 0;

  if (check_table_access(arg->thd, SELECT_ACL, &arg->table_list, TRUE, 1, TRUE))
    return FALSE;

  if (!(*arg->start_list= (OPEN_TABLE_LIST *) arg->thd->alloc(
                    sizeof(**arg->start_list) + element->m_key_length)))
    return TRUE;

  strmov((*arg->start_list)->table=
         strmov(((*arg->start_list)->db= (char*) ((*arg->start_list) + 1)),
                db) + 1, table_name);
  (*arg->start_list)->in_use= 0;

  mysql_mutex_lock(&element->LOCK_table_share);
  All_share_tables_list::Iterator it(element->all_tables);
  TABLE *table;
  while ((table= it++))
    if (table->in_use)
      ++(*arg->start_list)->in_use;
  mysql_mutex_unlock(&element->LOCK_table_share);
  (*arg->start_list)->locked= 0;                   /* Obsolete. */
  arg->start_list= &(*arg->start_list)->next;
  *arg->start_list= 0;
  return FALSE;
}


OPEN_TABLE_LIST *list_open_tables(THD *thd, const char *db, const char *wild)
{
  list_open_tables_arg argument;
  DBUG_ENTER("list_open_tables");

  argument.thd= thd;
  argument.db= db;
  argument.wild= wild;
  bzero((char*) &argument.table_list, sizeof(argument.table_list));
  argument.start_list= &argument.open_list;
  argument.open_list= 0;

  if (tdc_iterate(thd, (my_hash_walk_action) list_open_tables_callback,
                  &argument, true))
    DBUG_RETURN(0);

  DBUG_RETURN(argument.open_list);
}


/**
   Close all tables that are not in use in table definition cache

   @param purge_flag  Argument for tc_purge. true if we should force all
                      shares to be deleted. false if it's enough to just
                      evict those that are not in use.
*/

void purge_tables(bool purge_flag)
{
  /*
    Force close of all open tables.

    Note that code in TABLE_SHARE::wait_for_old_version() assumes that
    incrementing of refresh_version is followed by purge of unused table
    shares.
  */
  kill_delayed_threads();
  /*
    Get rid of all unused TABLE and TABLE_SHARE instances. By doing
    this we automatically close all tables which were marked as "old".
  */
  tc_purge(purge_flag);
  /* Free table shares which were not freed implicitly by loop above. */
  tdc_purge(true);
}


/**
   close_cached_tables

   This function has two separate usages:
   1) Close not used tables in the table cache to free memory
   2) Close a list of tables and wait until they are not used anymore. This
      is used mainly when preparing a table for export.

   If there are locked tables, they are closed and reopened before
   function returns. This is done to ensure that table files will be closed
   by all threads and thus external copyable when FLUSH TABLES returns.
*/

bool close_cached_tables(THD *thd, TABLE_LIST *tables,
                         bool wait_for_refresh, ulong timeout)
{
  DBUG_ENTER("close_cached_tables");
  DBUG_ASSERT(thd || (!wait_for_refresh && !tables));
  DBUG_ASSERT(wait_for_refresh || !tables);

  if (!tables)
  {
    /* Free tables that are not used */
    purge_tables(false);
    if (!wait_for_refresh)
      DBUG_RETURN(false);
  }

  DBUG_PRINT("info", ("open table definitions: %d",
                      (int) tdc_records()));

  if (thd->locked_tables_mode)
  {
    /*
      If we are under LOCK TABLES, we need to reopen the tables without
      opening a door for any concurrent threads to sneak in and get
      lock on our tables. To achieve this we use exclusive metadata
      locks.
    */
    TABLE_LIST *tables_to_reopen= (tables ? tables :
                                  thd->locked_tables_list.locked_tables());
    bool result= false;

    /* close open HANDLER for this thread to allow table to be closed */
    mysql_ha_flush_tables(thd, tables_to_reopen);

    for (TABLE_LIST *table_list= tables_to_reopen; table_list;
         table_list= table_list->next_global)
    {
      int err;
      /* A check that the table was locked for write is done by the caller. */
      TABLE *table= find_table_for_mdl_upgrade(thd, table_list->db.str,
                                            table_list->table_name.str, &err);

      /* May return NULL if this table has already been closed via an alias. */
      if (! table)
        continue;

      if (thd->mdl_context.upgrade_shared_lock(table->mdl_ticket, MDL_EXCLUSIVE,
                                               timeout))
      {
        result= true;
        break;
      }
      table->file->extra(HA_EXTRA_PREPARE_FOR_FORCED_CLOSE);
      close_all_tables_for_name(thd, table->s, HA_EXTRA_NOT_USED, NULL);
    }
    /*
      No other thread has the locked tables open; reopen them and get the
      old locks. This should always succeed (unless some external process
      has removed the tables)
    */
    if (thd->locked_tables_list.reopen_tables(thd, false))
      result= true;

    /*
      Since downgrade_lock() won't do anything with shared
      metadata lock it is much simpler to go through all open tables rather
      than picking only those tables that were flushed.
    */
    for (TABLE *tab= thd->open_tables; tab; tab= tab->next)
      tab->mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);

    DBUG_RETURN(result);
  }
  else if (tables)
  {
    /*
      Get an explicit MDL lock for all requested tables to ensure they are
      not used by any other thread
    */
    MDL_request_list mdl_requests;

    DBUG_PRINT("info", ("Waiting for other threads to close their open tables"));
    DEBUG_SYNC(thd, "after_flush_unlock");

    /* close open HANDLER for this thread to allow table to be closed */
    mysql_ha_flush_tables(thd, tables);

    for (TABLE_LIST *table= tables; table; table= table->next_local)
    {
      MDL_request *mdl_request= new (thd->mem_root) MDL_request;
      if (mdl_request == NULL)
        DBUG_RETURN(true);
      mdl_request->init(&table->mdl_request.key, MDL_EXCLUSIVE, MDL_STATEMENT);
      mdl_requests.push_front(mdl_request);
    }

    if (thd->mdl_context.acquire_locks(&mdl_requests, timeout))
      DBUG_RETURN(true);

    for (TABLE_LIST *table= tables; table; table= table->next_local)
      tdc_remove_table(thd, TDC_RT_REMOVE_ALL, table->db.str,
                       table->table_name.str, false);
  }
  DBUG_RETURN(false);
}


/**
  Collect all shares that has open tables
*/

struct tc_collect_arg
{
  DYNAMIC_ARRAY shares;
  flush_tables_type flush_type;
};

static my_bool tc_collect_used_shares(TDC_element *element,
                                      tc_collect_arg *arg)
{
  my_bool result= FALSE;

  DYNAMIC_ARRAY *shares= &arg->shares;
  mysql_mutex_lock(&element->LOCK_table_share);
  if (element->ref_count > 0 && !element->share->is_view)
  {
    DBUG_ASSERT(element->share);
    bool do_flush= 0;
    switch (arg->flush_type) {
    case FLUSH_ALL:
      do_flush= 1;
      break;
    case FLUSH_NON_TRANS_TABLES:
      if (!element->share->online_backup &&
          element->share->table_category == TABLE_CATEGORY_USER)
        do_flush= 1;
      break;
    case FLUSH_SYS_TABLES:
      if (!element->share->online_backup &&
          element->share->table_category != TABLE_CATEGORY_USER)
        do_flush= 1;
    }
    if (do_flush)
    {
      element->ref_count++;                       // Protect against delete
      if (push_dynamic(shares, (uchar*) &element->share))
        result= TRUE;
    }
  }
  mysql_mutex_unlock(&element->LOCK_table_share);
  return result;
}


/**
   Flush cached table as part of global read lock

   @param thd
   @param flag   What type of tables should be flushed

   @return 0  ok
   @return 1  error

   After we get the list of table shares, we will call flush on all
   possible tables, even if some flush fails.
*/

bool flush_tables(THD *thd, flush_tables_type flag)
{
  bool result= TRUE;
  uint open_errors= 0;
  tc_collect_arg collect_arg;
  TABLE *tmp_table;
  DBUG_ENTER("flush_tables");

  purge_tables(false);  /* Flush unused tables and shares */

  /*
    Loop over all shares and collect shares that have open tables
    TODO:
    Optimize this to only collect shares that have been used for
    write after last time all tables was closed.
  */

  if (!(tmp_table= (TABLE*) my_malloc(sizeof(*tmp_table),
                                      MYF(MY_WME | MY_THREAD_SPECIFIC))))
    DBUG_RETURN(1);

  my_init_dynamic_array(&collect_arg.shares, sizeof(TABLE_SHARE*), 100, 100,
                        MYF(0));
  collect_arg.flush_type= flag;
  if (tdc_iterate(thd, (my_hash_walk_action) tc_collect_used_shares,
                  &collect_arg, true))
  {
    /* Release already collected shares */
    for (uint i= 0 ; i < collect_arg.shares.elements ; i++)
    {
      TABLE_SHARE *share= *dynamic_element(&collect_arg.shares, i,
                                           TABLE_SHARE**);
      tdc_release_share(share);
    }
    goto err;
  }

  /* Call HA_EXTRA_FLUSH on all found shares */
  for (uint i= 0 ; i < collect_arg.shares.elements ; i++)
  {
    TABLE_SHARE *share= *dynamic_element(&collect_arg.shares, i,
                                         TABLE_SHARE**);
    TABLE *table= tc_acquire_table(thd, share->tdc);
    if (table)
    {
      (void) table->file->extra(HA_EXTRA_FLUSH);
      tc_release_table(table);
    }
    else
    {
      /*
        HA_OPEN_FOR_ALTER is used to allow us to open the table even if
        TABLE_SHARE::incompatible_version is set.
      */
      if (!open_table_from_share(thd, share, &empty_clex_str,
                                 HA_OPEN_KEYFILE, 0,
                                 HA_OPEN_FOR_ALTER,
                                 tmp_table, FALSE,
                                 NULL))
      {
        (void) tmp_table->file->extra(HA_EXTRA_FLUSH);
        /*
          We don't put the table into the TDC as the table was not fully
          opened (we didn't open triggers)
        */
        closefrm(tmp_table);
      }
      else
        open_errors++;
    }
    tdc_release_share(share);
  }

  result= open_errors ? TRUE : FALSE;
  DBUG_PRINT("note", ("open_errors: %u", open_errors));
err:
  my_free(tmp_table);
  delete_dynamic(&collect_arg.shares);
  DBUG_RETURN(result);
}


/**
  Close all tables which match specified connection string or
  if specified string is NULL, then any table with a connection string.
*/

struct close_cached_connection_tables_arg
{
  THD *thd;
  LEX_CSTRING *connection;
  TABLE_LIST *tables;
};


static my_bool close_cached_connection_tables_callback(
  TDC_element *element, close_cached_connection_tables_arg *arg)
{
  TABLE_LIST *tmp;

  mysql_mutex_lock(&element->LOCK_table_share);
  /* Ignore if table is not open or does not have a connect_string */
  if (!element->share || !element->share->connect_string.length ||
      !element->ref_count)
    goto end;

  /* Compare the connection string */
  if (arg->connection &&
      (arg->connection->length > element->share->connect_string.length ||
       (arg->connection->length < element->share->connect_string.length &&
        (element->share->connect_string.str[arg->connection->length] != '/' &&
         element->share->connect_string.str[arg->connection->length] != '\\')) ||
       strncasecmp(arg->connection->str, element->share->connect_string.str,
                   arg->connection->length)))
    goto end;

  /* close_cached_tables() only uses these elements */
  if (!(tmp= (TABLE_LIST*) alloc_root(arg->thd->mem_root, sizeof(TABLE_LIST))) ||
      !(arg->thd->make_lex_string(&tmp->db, element->share->db.str, element->share->db.length)) ||
      !(arg->thd->make_lex_string(&tmp->table_name, element->share->table_name.str,
                                      element->share->table_name.length)))
  {
    mysql_mutex_unlock(&element->LOCK_table_share);
    return TRUE;
  }

  tmp->next_local= arg->tables;
  arg->tables= tmp;

end:
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


/**
  Close cached connections

  @return false  ok
  @return true   If there was an error from closed_cached_connection_tables or
                 if there was any open connections that we had to force closed
*/

bool close_cached_connection_tables(THD *thd, LEX_CSTRING *connection)
{
  bool res= false;
  close_cached_connection_tables_arg argument;
  DBUG_ENTER("close_cached_connections");
  DBUG_ASSERT(thd);

  argument.thd= thd;
  argument.connection= connection;
  argument.tables= NULL;

  if (tdc_iterate(thd,
                  (my_hash_walk_action) close_cached_connection_tables_callback,
                  &argument))
    DBUG_RETURN(true);

  for (TABLE_LIST *table= argument.tables; table; table= table->next_local)
    res|= tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED,
                           table->db.str,
                           table->table_name.str, TRUE);

  /* Return true if we found any open connections */
  DBUG_RETURN(res);
}


/*
  Mark all tables in the list which were used by current substatement
  as free for reuse.

  SYNOPSIS
    mark_used_tables_as_free_for_reuse()
      thd   - thread context
      table - head of the list of tables

  DESCRIPTION
    Marks all tables in the list which were used by current substatement
    (they are marked by its query_id) as free for reuse.

  NOTE
    The reason we reset query_id is that it's not enough to just test
    if table->query_id != thd->query_id to know if a table is in use.

    For example
    SELECT f1_that_uses_t1() FROM t1;
    In f1_that_uses_t1() we will see one instance of t1 where query_id is
    set to query_id of original query.
*/

static void mark_used_tables_as_free_for_reuse(THD *thd, TABLE *table)
{
  DBUG_ENTER("mark_used_tables_as_free_for_reuse");
  for (; table ; table= table->next)
  {
    DBUG_ASSERT(table->pos_in_locked_tables == NULL ||
                table->pos_in_locked_tables->table == table);
    if (table->query_id == thd->query_id)
    {
      table->query_id= 0;
      table->file->ha_reset();
    }
  }
  DBUG_VOID_RETURN;
}


/**
  Close all open instances of the table but keep the MDL lock.

  Works both under LOCK TABLES and in the normal mode.
  Removes all closed instances of the table from the table cache.

  @param     thd     thread handle
  @param[in] share   table share, but is just a handy way to
                     access the table cache key

  @param[in] extra
                     HA_EXTRA_PREPARE_FOR_DROP
                        - The table is dropped
                     HA_EXTRA_PREPARE_FOR_RENAME
                        - The table is renamed
                     HA_EXTRA_NOT_USED
                        - The table is marked as closed in the
                          locked_table_list but kept there so one can call
                          locked_table_list->reopen_tables() to put it back.

                     In case of drop/rename the documented behavior is to
                     implicitly remove the table from LOCK TABLES
                     list. 

  @pre Must be called with an X MDL lock on the table.
*/

void
close_all_tables_for_name(THD *thd, TABLE_SHARE *share,
                          ha_extra_function extra,
                          TABLE *skip_table)
{
  DBUG_ASSERT(!share->tmp_table);

  char key[MAX_DBKEY_LENGTH];
  size_t key_length= share->table_cache_key.length;
  const char *db= key;
  const char *table_name= db + share->db.length + 1;
  bool remove_from_locked_tables= extra != HA_EXTRA_NOT_USED;

  memcpy(key, share->table_cache_key.str, key_length);

  for (TABLE **prev= &thd->open_tables; *prev; )
  {
    TABLE *table= *prev;

    if (table->s->table_cache_key.length == key_length &&
        !memcmp(table->s->table_cache_key.str, key, key_length) &&
        table != skip_table)
    {
      thd->locked_tables_list.unlink_from_list(thd,
                                               table->pos_in_locked_tables,
                                               remove_from_locked_tables);
      /* Inform handler that there is a drop table or a rename going on */
      if (extra != HA_EXTRA_NOT_USED && table->db_stat)
      {
        table->file->extra(extra);
        extra= HA_EXTRA_NOT_USED;               // Call extra once!
      }

      /*
        Does nothing if the table is not locked.
        This allows one to use this function after a table
        has been unlocked, e.g. in partition management.
      */
      mysql_lock_remove(thd, thd->lock, table);
      close_thread_table(thd, prev);
    }
    else
    {
      /* Step to next entry in open_tables list. */
      prev= &table->next;
    }
  }
  if (skip_table == NULL)
  {
    /* Remove the table share from the cache. */
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, db, table_name,
                     FALSE);
  }
}


/*
  Close all tables used by the current substatement, or all tables
  used by this thread if we are on the upper level.

  SYNOPSIS
    close_thread_tables()
    thd			Thread handler

  IMPLEMENTATION
    Unlocks tables and frees derived tables.
    Put all normal tables used by thread in free list.

    It will only close/mark as free for reuse tables opened by this
    substatement, it will also check if we are closing tables after
    execution of complete query (i.e. we are on upper level) and will
    leave prelocked mode if needed.
*/

void close_thread_tables(THD *thd)
{
  TABLE *table;
  DBUG_ENTER("close_thread_tables");

  THD_STAGE_INFO(thd, stage_closing_tables);

#ifdef EXTRA_DEBUG
  DBUG_PRINT("tcache", ("open tables:"));
  for (table= thd->open_tables; table; table= table->next)
    DBUG_PRINT("tcache", ("table: '%s'.'%s' %p", table->s->db.str,
                          table->s->table_name.str, table));
#endif

#if defined(ENABLED_DEBUG_SYNC)
  /* debug_sync may not be initialized for some slave threads */
  if (thd->debug_sync_control)
    DEBUG_SYNC(thd, "before_close_thread_tables");
#endif

  DBUG_ASSERT(thd->transaction.stmt.is_empty() || thd->in_sub_stmt ||
              (thd->state_flags & Open_tables_state::BACKUPS_AVAIL));

  for (table= thd->open_tables; table; table= table->next)
  {
    if (table->update_handler)
      table->delete_update_handler();

    /* Table might be in use by some outer statement. */
    DBUG_PRINT("tcache", ("table: '%s'  query_id: %lu",
                          table->s->table_name.str, (ulong) table->query_id));

    /* Detach MERGE children after every statement. Even under LOCK TABLES. */
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES ||
        table->query_id == thd->query_id)
    {
      DBUG_ASSERT(table->file);
      table->file->extra(HA_EXTRA_DETACH_CHILDREN);
    }
  }

  /*
    We are assuming here that thd->derived_tables contains ONLY derived
    tables for this substatement. i.e. instead of approach which uses
    query_id matching for determining which of the derived tables belong
    to this substatement we rely on the ability of substatements to
    save/restore thd->derived_tables during their execution.

    TODO: Probably even better approach is to simply associate list of
          derived tables with (sub-)statement instead of thread and destroy
          them at the end of its execution.
  */
  if (thd->derived_tables)
  {
    TABLE *next;
    /*
      Close all derived tables generated in queries like
      SELECT * FROM (SELECT * FROM t1)
    */
    for (table= thd->derived_tables ; table ; table= next)
    {
      next= table->next;
      free_tmp_table(thd, table);
    }
    thd->derived_tables= 0;
  }

  if (thd->rec_tables)
  {
    TABLE *next;
    /*
      Close all temporary tables created for recursive table references.
      This action was postponed because the table could be used in the
      statements like  ANALYZE WITH r AS (...) SELECT * from r
      where r is defined through recursion. 
    */
    for (table= thd->rec_tables ; table ; table= next)
    {
      next= table->next;
      free_tmp_table(thd, table);
    }
    thd->rec_tables= 0;
  }

  /*
    Mark all temporary tables used by this statement as free for reuse.
  */
  thd->mark_tmp_tables_as_free_for_reuse();

  if (thd->locked_tables_mode)
  {

    /* Ensure we are calling ha_reset() for all used tables */
    mark_used_tables_as_free_for_reuse(thd, thd->open_tables);

    /*
      We are under simple LOCK TABLES or we're inside a sub-statement
      of a prelocked statement, so should not do anything else.

      Note that even if we are in LTM_LOCK_TABLES mode and statement
      requires prelocking (e.g. when we are closing tables after
      failing ot "open" all tables required for statement execution)
      we will exit this function a few lines below.
    */
    if (! thd->lex->requires_prelocking())
      DBUG_VOID_RETURN;

    /*
      We are in the top-level statement of a prelocked statement,
      so we have to leave the prelocked mode now with doing implicit
      UNLOCK TABLES if needed.
    */
    if (thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)
      thd->locked_tables_mode= LTM_LOCK_TABLES;

    if (thd->locked_tables_mode == LTM_LOCK_TABLES)
      DBUG_VOID_RETURN;

    thd->leave_locked_tables_mode();

    /* Fallthrough */
  }

  if (thd->lock)
  {
    /*
      For RBR we flush the pending event just before we unlock all the
      tables.  This means that we are at the end of a topmost
      statement, so we ensure that the STMT_END_F flag is set on the
      pending event.  For statements that are *inside* stored
      functions, the pending event will not be flushed: that will be
      handled either before writing a query log event (inside
      binlog_query()) or when preparing a pending event.
     */
    (void)thd->binlog_flush_pending_rows_event(TRUE);
    mysql_unlock_tables(thd, thd->lock);
    thd->lock=0;
  }
  /*
    Closing a MERGE child before the parent would be fatal if the
    other thread tries to abort the MERGE lock in between.
  */
  while (thd->open_tables)
    (void) close_thread_table(thd, &thd->open_tables);

  DBUG_VOID_RETURN;
}


/* move one table to free list */

void close_thread_table(THD *thd, TABLE **table_ptr)
{
  TABLE *table= *table_ptr;
  DBUG_ENTER("close_thread_table");
  DBUG_PRINT("tcache", ("table: '%s'.'%s' %p", table->s->db.str,
                        table->s->table_name.str, table));
  DBUG_ASSERT(!table->file->keyread_enabled());
  DBUG_ASSERT(!table->file || table->file->inited == handler::NONE);

  /*
    The metadata lock must be released after giving back
    the table to the table cache.
  */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE,
                                             table->s->db.str,
                                             table->s->table_name.str,
                                             MDL_SHARED));
  table->mdl_ticket= NULL;

  if (table->file)
  {
    table->file->update_global_table_stats();
    table->file->update_global_index_stats();
  }

  /*
    This look is needed to allow THD::notify_shared_lock() to
    traverse the thd->open_tables list without having to worry that
    some of the tables are removed from under it
  */

  mysql_mutex_lock(&thd->LOCK_thd_data);
  *table_ptr=table->next;
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  if (! table->needs_reopen())
  {
    /* Avoid having MERGE tables with attached children in table cache. */
    table->file->extra(HA_EXTRA_DETACH_CHILDREN);
    /* Free memory and reset for next loop. */
    free_field_buffers_larger_than(table, MAX_TDC_BLOB_SIZE);
    table->file->ha_reset();
  }

  /*
    Do this *before* entering the TABLE_SHARE::tdc.LOCK_table_share
    critical section.
  */
  MYSQL_UNBIND_TABLE(table->file);

  tc_release_table(table);
  DBUG_VOID_RETURN;
}


/*
  Find table in list.

  SYNOPSIS
    find_table_in_list()
    table		Pointer to table list
    offset		Offset to which list in table structure to use
    db_name		Data base name
    table_name		Table name

  NOTES:
    This is called by find_table_in_global_list().

  RETURN VALUES
    NULL	Table not found
    #		Pointer to found table.
*/

TABLE_LIST *find_table_in_list(TABLE_LIST *table,
                               TABLE_LIST *TABLE_LIST::*link,
                               const LEX_CSTRING *db_name,
                               const LEX_CSTRING *table_name)
{
  for (; table; table= table->*link )
  {
    if (cmp(&table->db, db_name) == 0 &&
        cmp(&table->table_name, table_name) == 0)
      break;
  }
  return table;
}


/**
  Test that table is unique (It's only exists once in the table list)

  @param  thd                   thread handle
  @param  table                 table which should be checked
  @param  table_list            list of tables
  @param  check_flag            whether to check tables' aliases
                                Currently this is only used by INSERT

  NOTE: to exclude derived tables from check we use following mechanism:
    a) during derived table processing set THD::derived_tables_processing
    b) JOIN::prepare set SELECT::exclude_from_table_unique_test if
       THD::derived_tables_processing set. (we can't use JOIN::execute
       because for PS we perform only JOIN::prepare, but we can't set this
       flag in JOIN::prepare if we are not sure that we are in derived table
       processing loop, because multi-update call fix_fields() for some its
       items (which mean JOIN::prepare for subqueries) before unique_table
       call to detect which tables should be locked for write).
    c) find_dup_table skip all tables which belong to SELECT with
       SELECT::exclude_from_table_unique_test set.
    Also SELECT::exclude_from_table_unique_test used to exclude from check
    tables of main SELECT of multi-delete and multi-update

    We also skip tables with TABLE_LIST::prelocking_placeholder set,
    because we want to allow SELECTs from them, and their modification
    will rise the error anyway.

    TODO: when we will have table/view change detection we can do this check
          only once for PS/SP

  @retval !=0  found duplicate
  @retval 0 if table is unique
*/

static
TABLE_LIST* find_dup_table(THD *thd, TABLE_LIST *table, TABLE_LIST *table_list,
                           uint check_flag)
{
  TABLE_LIST *res= 0;
  LEX_CSTRING *d_name, *t_name, *t_alias;
  DBUG_ENTER("find_dup_table");
  DBUG_PRINT("enter", ("table alias: %s", table->alias.str));

  /*
    If this function called for query which update table (INSERT/UPDATE/...)
    then we have in table->table pointer to TABLE object which we are
    updating even if it is VIEW so we need TABLE_LIST of this TABLE object
    to get right names (even if lower_case_table_names used).

    If this function called for CREATE command that we have not opened table
    (table->table equal to 0) and right names is in current TABLE_LIST
    object.
  */
  if (table->table)
  {
    /* All MyISAMMRG children are plain MyISAM tables. */
    DBUG_ASSERT(table->table->file->ht->db_type != DB_TYPE_MRG_MYISAM);

    table= table->find_underlying_table(table->table);
    /*
      as far as we have table->table we have to find real TABLE_LIST of
      it in underlying tables
    */
    DBUG_ASSERT(table);
  }
  d_name= &table->db;
  t_name= &table->table_name;
  t_alias= &table->alias;

retry:
  DBUG_PRINT("info", ("real table: %s.%s", d_name->str, t_name->str));
  for (TABLE_LIST *tl= table_list; tl ; tl= tl->next_global, res= 0)
  {
    if (tl->select_lex && tl->select_lex->master_unit() &&
        tl->select_lex->master_unit()->executed)
    {
      /*
        There is no sense to check tables of already executed parts
        of the query
      */
      continue;
    }
    /*
      Table is unique if it is present only once in the global list
      of tables and once in the list of table locks.
    */
    if (! (res= find_table_in_global_list(tl, d_name, t_name)))
      break;
    tl= res;                       // We can continue search after this table

    /* Skip if same underlying table. */
    if (res->table && (res->table == table->table))
      continue;

    /* Skip if table is tmp table */
    if (check_flag & CHECK_DUP_SKIP_TEMP_TABLE &&
        res->table && res->table->s->tmp_table != NO_TMP_TABLE)
    {
      continue;
    }
    if (check_flag & CHECK_DUP_FOR_CREATE)
      DBUG_RETURN(res);

    /* Skip if table alias does not match. */
    if (check_flag & CHECK_DUP_ALLOW_DIFFERENT_ALIAS)
    {
      if (my_strcasecmp(table_alias_charset, t_alias->str, res->alias.str))
        continue;
    }

    /*
      If table is not excluded (could be a derived table) and table is not
      a prelocking placeholder then we found either a duplicate entry
      or a table that is part of a derived table (handled below).
      Examples are:
      INSERT INTO t1 SELECT * FROM t1;
      INSERT INTO t1 SELECT * FROM view_containing_t1;
    */
    if (res->select_lex &&
        !res->select_lex->exclude_from_table_unique_test &&
        !res->prelocking_placeholder)
      break;

    /*
      If we found entry of this table or table of SELECT which already
      processed in derived table or top select of multi-update/multi-delete
      (exclude_from_table_unique_test) or prelocking placeholder.
    */
    DBUG_PRINT("info",
               ("found same copy of table or table which we should skip"));
  }
  if (res && res->belong_to_derived)
  {
    /*
      We come here for queries of type:
      INSERT INTO t1 (SELECT tmp.a FROM (select * FROM t1) as tmp);

      Try to fix by materializing the derived table
    */
    TABLE_LIST *derived=  res->belong_to_derived;
    if (derived->is_merged_derived() && !derived->derived->is_excluded())
    {
      DBUG_PRINT("info",
                 ("convert merged to materialization to resolve the conflict"));
      derived->change_refs_to_fields();
      derived->set_materialized_derived();
      goto retry;
    }
  }
  DBUG_RETURN(res);
}


/**
  Test that the subject table of INSERT/UPDATE/DELETE/CREATE
  or (in case of MyISAMMRG) one of its children are not used later
  in the query.

  For MyISAMMRG tables, it is assumed that all the underlying
  tables of @c table (if any) are listed right after it and that
  their @c parent_l field points at the main table.


  @retval non-NULL The table list element for the table that
                   represents the duplicate. 
  @retval NULL     No duplicates found.
*/

TABLE_LIST*
unique_table(THD *thd, TABLE_LIST *table, TABLE_LIST *table_list,
             uint check_flag)
{
  TABLE_LIST *dup;

  table= table->find_table_for_update();

  if (table->table &&
      table->table->file->ha_table_flags() & HA_CAN_MULTISTEP_MERGE)
  {
    TABLE_LIST *child;
    dup= NULL;
    /* Check duplicates of all merge children. */
    for (child= table->next_global; child;
         child= child->next_global)
    {
      if (child->table &&
          child->table->file->ha_table_flags() & HA_CAN_MULTISTEP_MERGE)
        continue;

      /*
        Ensure that the child has one parent that is the table that is
        updated.
      */
      TABLE_LIST *tmp_parent= child;
      while ((tmp_parent= tmp_parent->parent_l))
      {
        if (tmp_parent == table)
          break;
      }
      if (!tmp_parent)
        break;

      if ((dup= find_dup_table(thd, child, child->next_global, check_flag)))
        break;
    }
  }
  else
    dup= find_dup_table(thd, table, table_list, check_flag);
  return dup;
}


/*
  Issue correct error message in case we found 2 duplicate tables which
  prevent some update operation

  SYNOPSIS
    update_non_unique_table_error()
    update      table which we try to update
    operation   name of update operation
    duplicate   duplicate table which we found

  NOTE:
    here we hide view underlying tables if we have them
*/

void update_non_unique_table_error(TABLE_LIST *update,
                                   const char *operation,
                                   TABLE_LIST *duplicate)
{
  update= update->top_table();
  duplicate= duplicate->top_table();
  if (!update->view || !duplicate->view ||
      update->view == duplicate->view ||
      update->view_name.length != duplicate->view_name.length ||
      update->view_db.length != duplicate->view_db.length ||
      lex_string_cmp(table_alias_charset,
                     &update->view_name, &duplicate->view_name) != 0 ||
      lex_string_cmp(table_alias_charset,
                     &update->view_db, &duplicate->view_db) != 0)
  {
    /*
      it is not the same view repeated (but it can be parts of the same copy
      of view), so we have to hide underlying tables.
    */
    if (update->view)
    {
      /* Issue the ER_NON_INSERTABLE_TABLE error for an INSERT */
      if (update->view == duplicate->view)
        my_error(!strncmp(operation, "INSERT", 6) ?
                 ER_NON_INSERTABLE_TABLE : ER_NON_UPDATABLE_TABLE, MYF(0),
                 update->alias.str, operation);
      else
        my_error(ER_VIEW_PREVENT_UPDATE, MYF(0),
                 (duplicate->view ? duplicate->alias.str : update->alias.str),
                 operation, update->alias.str);
      return;
    }
    if (duplicate->view)
    {
      my_error(ER_VIEW_PREVENT_UPDATE, MYF(0), duplicate->alias.str, operation,
               update->alias.str);
      return;
    }
  }
  my_error(ER_UPDATE_TABLE_USED, MYF(0), update->alias.str, operation);
}


/**
   Force all other threads to stop using the table by upgrading
   metadata lock on it and remove unused TABLE instances from cache.

   @param thd      Thread handler
   @param table    Table to remove from cache
   @param function HA_EXTRA_PREPARE_FOR_DROP if table is to be deleted
                   HA_EXTRA_FORCE_REOPEN if table is not be used
                   HA_EXTRA_PREPARE_FOR_RENAME if table is to be renamed
                   HA_EXTRA_NOT_USED             Don't call extra()

   @note When returning, the table will be unusable for other threads
         until metadata lock is downgraded.

   @retval FALSE Success.
   @retval TRUE  Failure (e.g. because thread was killed).
*/

bool wait_while_table_is_used(THD *thd, TABLE *table,
                              enum ha_extra_function function)
{
  DBUG_ENTER("wait_while_table_is_used");
  DBUG_ASSERT(!table->s->tmp_table);
  DBUG_PRINT("enter", ("table: '%s'  share: %p  db_stat: %u  version: %lld",
                       table->s->table_name.str, table->s,
                       table->db_stat, table->s->tdc->version));

  if (thd->mdl_context.upgrade_shared_lock(
             table->mdl_ticket, MDL_EXCLUSIVE,
             thd->variables.lock_wait_timeout))
    DBUG_RETURN(TRUE);

  tdc_remove_table(thd, TDC_RT_REMOVE_NOT_OWN,
                   table->s->db.str, table->s->table_name.str,
                   FALSE);
  /* extra() call must come only after all instances above are closed */
  if (function != HA_EXTRA_NOT_USED)
    (void) table->file->extra(function);
  DBUG_RETURN(FALSE);
}


/**
  Close a and drop a just created table in CREATE TABLE ... SELECT.

  @param  thd         Thread handle
  @param  table       TABLE object for the table to be dropped
  @param  db_name     Name of database for this table
  @param  table_name  Name of this table

  This routine assumes that the table to be closed is open only
  by the calling thread, so we needn't wait until other threads
  close the table. It also assumes that the table is first
  in thd->open_ables and a data lock on it, if any, has been
  released. To sum up, it's tuned to work with
  CREATE TABLE ... SELECT and CREATE TABLE .. SELECT only.
  Note, that currently CREATE TABLE ... SELECT is not supported
  under LOCK TABLES. This function, still, can be called in
  prelocked mode, e.g. if we do CREATE TABLE .. SELECT f1();
*/

void drop_open_table(THD *thd, TABLE *table, const LEX_CSTRING *db_name,
                     const LEX_CSTRING *table_name)
{
  DBUG_ENTER("drop_open_table");
  if (table->s->tmp_table)
    thd->drop_temporary_table(table, NULL, true);
  else
  {
    DBUG_ASSERT(table == thd->open_tables);

    handlerton *table_type= table->s->db_type();
    table->file->extra(HA_EXTRA_PREPARE_FOR_DROP);
    close_thread_table(thd, &thd->open_tables);
    /* Remove the table share from the table cache. */
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL, db_name->str, table_name->str,
                     FALSE);
    /* Remove the table from the storage engine and rm the .frm. */
    quick_rm_table(thd, table_type, db_name, table_name, 0);
 }
  DBUG_VOID_RETURN;
}


/**
  An error handler which converts, if possible, ER_LOCK_DEADLOCK error
  that can occur when we are trying to acquire a metadata lock to
  a request for back-off and re-start of open_tables() process.
*/

class MDL_deadlock_handler : public Internal_error_handler
{
public:
  MDL_deadlock_handler(Open_table_context *ot_ctx_arg)
    : m_ot_ctx(ot_ctx_arg), m_is_active(FALSE)
  {}

  virtual ~MDL_deadlock_handler() {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* msg,
                                Sql_condition ** cond_hdl);

private:
  /** Open table context to be used for back-off request. */
  Open_table_context *m_ot_ctx;
  /**
    Indicates that we are already in the process of handling
    ER_LOCK_DEADLOCK error. Allows to re-emit the error from
    the error handler without falling into infinite recursion.
  */
  bool m_is_active;
};


bool MDL_deadlock_handler::handle_condition(THD *,
                                            uint sql_errno,
                                            const char*,
                                            Sql_condition::enum_warning_level*,
                                            const char*,
                                            Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (! m_is_active && sql_errno == ER_LOCK_DEADLOCK)
  {
    /* Disable the handler to avoid infinite recursion. */
    m_is_active= TRUE;
    (void) m_ot_ctx->request_backoff_action(
             Open_table_context::OT_BACKOFF_AND_RETRY,
             NULL);
    m_is_active= FALSE;
    /*
      If the above back-off request failed, a new instance of
      ER_LOCK_DEADLOCK error was emitted. Thus the current
      instance of error condition can be treated as handled.
    */
    return TRUE;
  }
  return FALSE;
}


/**
  Try to acquire an MDL lock for a table being opened.

  @param[in,out] thd      Session context, to report errors.
  @param[out]    ot_ctx   Open table context, to hold the back off
                          state. If we failed to acquire a lock
                          due to a lock conflict, we add the
                          failed request to the open table context.
  @param[in,out] mdl_request A request for an MDL lock.
                          If we managed to acquire a ticket
                          (no errors or lock conflicts occurred),
                          contains a reference to it on
                          return. However, is not modified if MDL
                          lock type- modifying flags were provided.
  @param[in]    flags flags MYSQL_OPEN_FORCE_SHARED_MDL,
                          MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL or
                          MYSQL_OPEN_FAIL_ON_MDL_CONFLICT
                          @sa open_table().
  @param[out]   mdl_ticket Only modified if there was no error.
                          If we managed to acquire an MDL
                          lock, contains a reference to the
                          ticket, otherwise is set to NULL.

  @retval TRUE  An error occurred.
  @retval FALSE No error, but perhaps a lock conflict, check mdl_ticket.
*/

static bool
open_table_get_mdl_lock(THD *thd, Open_table_context *ot_ctx,
                        MDL_request *mdl_request,
                        uint flags,
                        MDL_ticket **mdl_ticket)
{
  MDL_request mdl_request_shared;

  if (flags & (MYSQL_OPEN_FORCE_SHARED_MDL |
               MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL))
  {
    /*
      MYSQL_OPEN_FORCE_SHARED_MDL flag means that we are executing
      PREPARE for a prepared statement and want to override
      the type-of-operation aware metadata lock which was set
      in the parser/during view opening with a simple shared
      metadata lock.
      This is necessary to allow concurrent execution of PREPARE
      and LOCK TABLES WRITE statement against the same table.

      MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL flag means that we open
      the table in order to get information about it for one of I_S
      queries and also want to override the type-of-operation aware
      shared metadata lock which was set earlier (e.g. during view
      opening) with a high-priority shared metadata lock.
      This is necessary to avoid unnecessary waiting and extra
      ER_WARN_I_S_SKIPPED_TABLE warnings when accessing I_S tables.

      These two flags are mutually exclusive.
    */
    DBUG_ASSERT(!(flags & MYSQL_OPEN_FORCE_SHARED_MDL) ||
                !(flags & MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL));

    mdl_request_shared.init(&mdl_request->key,
                            (flags & MYSQL_OPEN_FORCE_SHARED_MDL) ?
                            MDL_SHARED : MDL_SHARED_HIGH_PRIO,
                            MDL_TRANSACTION);
    mdl_request= &mdl_request_shared;
  }

  if (flags & MYSQL_OPEN_FAIL_ON_MDL_CONFLICT)
  {
    /*
      When table is being open in order to get data for I_S table,
      we might have some tables not only open but also locked (e.g. when
      this happens under LOCK TABLES or in a stored function).
      As a result by waiting on a conflicting metadata lock to go away
      we may create a deadlock which won't entirely belong to the
      MDL subsystem and thus won't be detectable by this subsystem's
      deadlock detector.
      To avoid such situation we skip the trouble-making table if
      there is a conflicting lock.
    */
    if (thd->mdl_context.try_acquire_lock(mdl_request))
      return TRUE;
    if (mdl_request->ticket == NULL)
    {
      my_error(ER_WARN_I_S_SKIPPED_TABLE, MYF(0),
               mdl_request->key.db_name(), mdl_request->key.name());
      return TRUE;
    }
  }
  else
  {
    /*
      We are doing a normal table open. Let us try to acquire a metadata
      lock on the table. If there is a conflicting lock, acquire_lock()
      will wait for it to go away. Sometimes this waiting may lead to a
      deadlock, with the following results:
      1) If a deadlock is entirely within MDL subsystem, it is
         detected by the deadlock detector of this subsystem.
         ER_LOCK_DEADLOCK error is produced. Then, the error handler
         that is installed prior to the call to acquire_lock() attempts
         to request a back-off and retry. Upon success, ER_LOCK_DEADLOCK
         error is suppressed, otherwise propagated up the calling stack.
      2) Otherwise, a deadlock may occur when the wait-for graph
         includes edges not visible to the MDL deadlock detector.
         One such example is a wait on an InnoDB row lock, e.g. when:
         conn C1 gets SR MDL lock on t1 with SELECT * FROM t1
         conn C2 gets a row lock on t2 with  SELECT * FROM t2 FOR UPDATE
         conn C3 gets in and waits on C1 with DROP TABLE t0, t1
         conn C2 continues and blocks on C3 with SELECT * FROM t0
         conn C1 deadlocks by waiting on C2 by issuing SELECT * FROM
         t2 LOCK IN SHARE MODE.
         Such circular waits are currently only resolved by timeouts,
         e.g. @@innodb_lock_wait_timeout or @@lock_wait_timeout.
    */
    MDL_deadlock_handler mdl_deadlock_handler(ot_ctx);

    thd->push_internal_handler(&mdl_deadlock_handler);
    bool result= thd->mdl_context.acquire_lock(mdl_request,
                                               ot_ctx->get_timeout());
    thd->pop_internal_handler();

    if (result && !ot_ctx->can_recover_from_failed_open())
      return TRUE;
  }
  *mdl_ticket= mdl_request->ticket;
  return FALSE;
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
/* Set all [named] partitions as used. */
static int set_partitions_as_used(TABLE_LIST *tl, TABLE *t)
{
  if (t->part_info)
    return t->file->change_partitions_to_open(tl->partition_names);
  return 0;
}
#endif


/**
  Check if the given table is actually a VIEW that was LOCK-ed

  @param thd            Thread context.
  @param t              Table to check.

  @retval TRUE  The 't'-table is a locked view
                needed to remedy problem before retrying again.
  @retval FALSE 't' was not locked, not a VIEW or an error happened.
*/
bool is_locked_view(THD *thd, TABLE_LIST *t)
{
  DBUG_ENTER("check_locked_view");
  /*
   Is this table a view and not a base table?
   (it is work around to allow to open view with locked tables,
   real fix will be made after definition cache will be made)

   Since opening of view which was not explicitly locked by LOCK
   TABLES breaks metadata locking protocol (potentially can lead
   to deadlocks) it should be disallowed.
  */
  if (thd->mdl_context.is_lock_owner(MDL_key::TABLE, t->db.str,
                                     t->table_name.str, MDL_SHARED))
  {
    char path[FN_REFLEN + 1];
    build_table_filename(path, sizeof(path) - 1,
                         t->db.str, t->table_name.str, reg_ext, 0);
    /*
      Note that we can't be 100% sure that it is a view since it's
      possible that we either simply have not found unused TABLE
      instance in THD::open_tables list or were unable to open table
      during prelocking process (in this case in theory we still
      should hold shared metadata lock on it).
    */
    if (dd_frm_is_view(thd, path))
    {
      /*
        If parent_l of the table_list is non null then a merge table
        has this view as child table, which is not supported.
      */
      if (t->parent_l)
      {
        my_error(ER_WRONG_MRG_TABLE, MYF(0));
        DBUG_RETURN(FALSE);
      }

      if (!tdc_open_view(thd, t, CHECK_METADATA_VERSION))
      {
        DBUG_ASSERT(t->view != 0);
        DBUG_RETURN(TRUE); // VIEW
      }
    }
  }

  DBUG_RETURN(FALSE);
}


/**
  Open a base table.

  @param thd            Thread context.
  @param table_list     Open first table in list.
  @param ot_ctx         Context with flags which modify how open works
                        and which is used to recover from a failed
                        open_table() attempt.
                        Some examples of flags:
                        MYSQL_OPEN_IGNORE_FLUSH - Open table even if
                        someone has done a flush. No version number
                        checking is done.
                        MYSQL_OPEN_HAS_MDL_LOCK - instead of acquiring
                        metadata locks rely on that caller already has
                        appropriate ones.

  Uses a cache of open tables to find a TABLE instance not in use.

  If TABLE_LIST::open_strategy is set to OPEN_IF_EXISTS, the table is
  opened only if it exists. If the open strategy is OPEN_STUB, the
  underlying table is never opened. In both cases, metadata locks are
  always taken according to the lock strategy.

  The function used to open temporary tables, but now it opens base tables
  only.

  @retval TRUE  Open failed. "action" parameter may contain type of action
                needed to remedy problem before retrying again.
  @retval FALSE Success. Members of TABLE_LIST structure are filled properly
                (e.g.  TABLE_LIST::table is set for real tables and
                TABLE_LIST::view is set for views).
*/

bool open_table(THD *thd, TABLE_LIST *table_list, Open_table_context *ot_ctx)
{
  TABLE *table;
  const char *key;
  uint	key_length;
  const char *alias= table_list->alias.str;
  uint flags= ot_ctx->get_flags();
  MDL_ticket *mdl_ticket;
  TABLE_SHARE *share;
  uint gts_flags;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int part_names_error=0;
#endif
  DBUG_ENTER("open_table");

  /*
    The table must not be opened already. The table can be pre-opened for
    some statements if it is a temporary table.

    open_temporary_table() must be used to open temporary tables.
  */
  DBUG_ASSERT(!table_list->table);

  /* an open table operation needs a lot of the stack space */
  if (check_stack_overrun(thd, STACK_MIN_SIZE_FOR_OPEN, (uchar *)&alias))
    DBUG_RETURN(TRUE);

  if (!(flags & MYSQL_OPEN_IGNORE_KILLED) && thd->killed)
  {
    thd->send_kill_message();
    DBUG_RETURN(TRUE);
  }

  /*
    Check if we're trying to take a write lock in a read only transaction.

    Note that we allow write locks on log tables as otherwise logging
    to general/slow log would be disabled in read only transactions.
  */
  if (table_list->mdl_request.is_write_lock_request() &&
      thd->tx_read_only &&
      !(flags & (MYSQL_LOCK_LOG_TABLE | MYSQL_OPEN_HAS_MDL_LOCK)))
  {
    my_error(ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION, MYF(0));
    DBUG_RETURN(true);
  }

  if (!table_list->db.str)
  {
    my_error(ER_NO_DB_ERROR, MYF(0));
    DBUG_RETURN(true);
  }

  key_length= get_table_def_key(table_list, &key);

  /*
    If we're in pre-locked or LOCK TABLES mode, let's try to find the
    requested table in the list of pre-opened and locked tables. If the
    table is not there, return an error - we can't open not pre-opened
    tables in pre-locked/LOCK TABLES mode.
    TODO: move this block into a separate function.
  */
  if (thd->locked_tables_mode &&
      ! (flags & MYSQL_OPEN_GET_NEW_TABLE))
  {						// Using table locks
    TABLE *best_table= 0;
    int best_distance= INT_MIN;
    for (table=thd->open_tables; table ; table=table->next)
    {
      if (table->s->table_cache_key.length == key_length &&
	  !memcmp(table->s->table_cache_key.str, key, key_length))
      {
        if (!my_strcasecmp(system_charset_info, table->alias.c_ptr(), alias) &&
            table->query_id != thd->query_id && /* skip tables already used */
            (thd->locked_tables_mode == LTM_LOCK_TABLES ||
             table->query_id == 0))
        {
          int distance= ((int) table->reginfo.lock_type -
                         (int) table_list->lock_type);

          /*
            Find a table that either has the exact lock type requested,
            or has the best suitable lock. In case there is no locked
            table that has an equal or higher lock than requested,
            we us the closest matching lock to be able to produce an error
            message about wrong lock mode on the table. The best_table
            is changed if bd < 0 <= d or bd < d < 0 or 0 <= d < bd.

            distance <  0 - No suitable lock found
            distance >  0 - we have lock mode higher then we require
            distance == 0 - we have lock mode exactly which we need
          */
          if ((best_distance < 0 && distance > best_distance) ||
              (distance >= 0 && distance < best_distance))
          {
            best_distance= distance;
            best_table= table;
            if (best_distance == 0)
            {
              /*
                We have found a perfect match and can finish iterating
                through open tables list. Check for table use conflict
                between calling statement and SP/trigger is done in
                lock_tables().
              */
              break;
            }
          }
        }
      }
    }
    if (best_table)
    {
      table= best_table;
      table->query_id= thd->query_id;
      DBUG_PRINT("info",("Using locked table"));
#ifdef WITH_PARTITION_STORAGE_ENGINE
      part_names_error= set_partitions_as_used(table_list, table);
#endif
      goto reset;
    }

    if (is_locked_view(thd, table_list))
      DBUG_RETURN(FALSE); // VIEW

    /*
      No table in the locked tables list. In case of explicit LOCK TABLES
      this can happen if a user did not include the table into the list.
      In case of pre-locked mode locked tables list is generated automatically,
      so we may only end up here if the table did not exist when
      locked tables list was created.
    */
    if (thd->locked_tables_mode == LTM_PRELOCKED)
      my_error(ER_NO_SUCH_TABLE, MYF(0), table_list->db.str, table_list->alias.str);
    else
      my_error(ER_TABLE_NOT_LOCKED, MYF(0), alias);
    DBUG_RETURN(TRUE);
  }

  /*
    Non pre-locked/LOCK TABLES mode, and the table is not temporary.
    This is the normal use case.
  */

  if (! (flags & MYSQL_OPEN_HAS_MDL_LOCK))
  {
    if (open_table_get_mdl_lock(thd, ot_ctx, &table_list->mdl_request,
                                flags, &mdl_ticket) ||
        mdl_ticket == NULL)
    {
      DEBUG_SYNC(thd, "before_open_table_wait_refresh");
      DBUG_RETURN(TRUE);
    }
    DEBUG_SYNC(thd, "after_open_table_mdl_shared");
  }
  else
  {
    /*
      Grab reference to the MDL lock ticket that was acquired
      by the caller.
    */
    mdl_ticket= table_list->mdl_request.ticket;
  }

  if (table_list->open_strategy == TABLE_LIST::OPEN_IF_EXISTS)
  {
    if (!ha_table_exists(thd, &table_list->db, &table_list->table_name))
      DBUG_RETURN(FALSE);
  }
  else if (table_list->open_strategy == TABLE_LIST::OPEN_STUB)
    DBUG_RETURN(FALSE);

  /* Table exists. Let us try to open it. */

  if (table_list->i_s_requested_object & OPEN_TABLE_ONLY)
    gts_flags= GTS_TABLE;
  else if (table_list->i_s_requested_object &  OPEN_VIEW_ONLY)
    gts_flags= GTS_VIEW;
  else
    gts_flags= GTS_TABLE | GTS_VIEW;

retry_share:

  share= tdc_acquire_share(thd, table_list, gts_flags, &table);

  if (unlikely(!share))
  {
    /*
      Hide "Table doesn't exist" errors if the table belongs to a view.
      The check for thd->is_error() is necessary to not push an
      unwanted error in case the error was already silenced.
      @todo Rework the alternative ways to deal with ER_NO_SUCH TABLE.
    */
    if (thd->is_error())
    {
      if (table_list->parent_l)
      {
        thd->clear_error();
        my_error(ER_WRONG_MRG_TABLE, MYF(0));
      }
      else if (table_list->belong_to_view)
      {
        TABLE_LIST *view= table_list->belong_to_view;
        thd->clear_error();
        my_error(ER_VIEW_INVALID, MYF(0),
                 view->view_db.str, view->view_name.str);
      }
    }
    DBUG_RETURN(TRUE);
  }

  /*
    Check if this TABLE_SHARE-object corresponds to a view. Note, that there is
    no need to check TABLE_SHARE::tdc.flushed as we do for regular tables,
    because view shares are always up to date.
  */
  if (share->is_view)
  {
    /*
      If parent_l of the table_list is non null then a merge table
      has this view as child table, which is not supported.
    */
    if (table_list->parent_l)
    {
      my_error(ER_WRONG_MRG_TABLE, MYF(0));
      goto err_lock;
    }
    if (table_list->sequence)
    {
      my_error(ER_NOT_SEQUENCE, MYF(0), table_list->db.str,
               table_list->alias.str);
      goto err_lock;
    }
    /*
      This table is a view. Validate its metadata version: in particular,
      that it was a view when the statement was prepared.
    */
    if (check_and_update_table_version(thd, table_list, share))
      goto err_lock;

    /* Open view */
    if (mysql_make_view(thd, share, table_list, false))
      goto err_lock;

    /* TODO: Don't free this */
    tdc_release_share(share);

    DBUG_ASSERT(table_list->view);

    DBUG_RETURN(FALSE);
  }

  if (!(flags & MYSQL_OPEN_IGNORE_FLUSH))
  {
    if (share->tdc->flushed)
    {
      DBUG_PRINT("info", ("Found old share version: %lld  current: %lld",
                          share->tdc->version, tdc_refresh_version()));
      /*
        We already have an MDL lock. But we have encountered an old
        version of table in the table definition cache which is possible
        when someone changes the table version directly in the cache
        without acquiring a metadata lock (e.g. this can happen during
        "rolling" FLUSH TABLE(S)).
        Release our reference to share, wait until old version of
        share goes away and then try to get new version of table share.
      */
      if (table)
        tc_release_table(table);
      else
        tdc_release_share(share);

      MDL_deadlock_handler mdl_deadlock_handler(ot_ctx);
      bool wait_result;

      thd->push_internal_handler(&mdl_deadlock_handler);
      wait_result= tdc_wait_for_old_version(thd, table_list->db.str,
                                            table_list->table_name.str,
                                            ot_ctx->get_timeout(),
                                            mdl_ticket->get_deadlock_weight());
      thd->pop_internal_handler();

      if (wait_result)
        DBUG_RETURN(TRUE);

      goto retry_share;
    }

    if (thd->open_tables && thd->open_tables->s->tdc->flushed)
    {
      /*
        If the version changes while we're opening the tables,
        we have to back off, close all the tables opened-so-far,
        and try to reopen them. Note: refresh_version is currently
        changed only during FLUSH TABLES.
      */
      if (table)
        tc_release_table(table);
      else
        tdc_release_share(share);
      (void)ot_ctx->request_backoff_action(Open_table_context::OT_REOPEN_TABLES,
                                           NULL);
      DBUG_RETURN(TRUE);
    }
  }

  if (table)
  {
    DBUG_ASSERT(table->file != NULL);
    MYSQL_REBIND_TABLE(table->file);
#ifdef WITH_PARTITION_STORAGE_ENGINE
    part_names_error= set_partitions_as_used(table_list, table);
#endif
  }
  else
  {
    enum open_frm_error error;
    /* make a new table */
    if (!(table=(TABLE*) my_malloc(sizeof(*table),MYF(MY_WME))))
      goto err_lock;

    error= open_table_from_share(thd, share, &table_list->alias,
                                 HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                                 EXTRA_RECORD,
                                 thd->open_options, table, FALSE,
                                 IF_PARTITIONING(table_list->partition_names,0));

    if (unlikely(error))
    {
      my_free(table);

      if (error == OPEN_FRM_DISCOVER)
        (void) ot_ctx->request_backoff_action(Open_table_context::OT_DISCOVER,
                                              table_list);
      else if (share->crashed)
      {
        if (!(flags & MYSQL_OPEN_IGNORE_REPAIR))
          (void) ot_ctx->request_backoff_action(Open_table_context::OT_REPAIR,
                                                table_list);
        else
          table_list->crashed= 1;  /* Mark that table was crashed */
      }
      goto err_lock;
    }
    if (open_table_entry_fini(thd, share, table))
    {
      closefrm(table);
      my_free(table);
      goto err_lock;
    }

    /* Add table to the share's used tables list. */
    tc_add_table(thd, table);
  }

  if (!(flags & MYSQL_OPEN_HAS_MDL_LOCK) &&
      table->s->table_category < TABLE_CATEGORY_INFORMATION)
  {
    /*
      We are not under LOCK TABLES and going to acquire write-lock/
      modify the base table. We need to acquire protection against
      global read lock until end of this statement in order to have
      this statement blocked by active FLUSH TABLES WITH READ LOCK.

      We don't need to acquire this protection under LOCK TABLES as
      such protection already acquired at LOCK TABLES time and
      not released until UNLOCK TABLES.

      We don't block statements which modify only temporary tables
      as these tables are not preserved by any form of
      backup which uses FLUSH TABLES WITH READ LOCK.

      TODO: The fact that we sometimes acquire protection against
            GRL only when we encounter table to be write-locked
            slightly increases probability of deadlock.
            This problem will be solved once Alik pushes his
            temporary table refactoring patch and we can start
            pre-acquiring metadata locks at the beggining of
            open_tables() call.
    */
    enum enum_mdl_type mdl_type= MDL_BACKUP_DML;

    if (table->s->table_category != TABLE_CATEGORY_USER)
      mdl_type= MDL_BACKUP_SYS_DML;
    else if (table->s->online_backup)
      mdl_type= MDL_BACKUP_TRANS_DML;

    if (table_list->mdl_request.is_write_lock_request() &&
        ! (flags & (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
                    MYSQL_OPEN_FORCE_SHARED_MDL |
                    MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL |
                    MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK)) &&
        ! ot_ctx->has_protection_against_grl(mdl_type))
    {
      MDL_request protection_request;
      MDL_deadlock_handler mdl_deadlock_handler(ot_ctx);

      if (thd->has_read_only_protection())
      {
        MYSQL_UNBIND_TABLE(table->file);
        tc_release_table(table);
        DBUG_RETURN(TRUE);
      }

      protection_request.init(MDL_key::BACKUP, "", "", mdl_type,
                              MDL_STATEMENT);

      /*
        Install error handler which if possible will convert deadlock error
        into request to back-off and restart process of opening tables.
      */
      thd->push_internal_handler(&mdl_deadlock_handler);
      bool result= thd->mdl_context.acquire_lock(&protection_request,
                                                 ot_ctx->get_timeout());
      thd->pop_internal_handler();

      if (result)
      {
        MYSQL_UNBIND_TABLE(table->file);
        tc_release_table(table);
        DBUG_RETURN(TRUE);
      }

      ot_ctx->set_has_protection_against_grl(mdl_type);
    }
  }

  table->mdl_ticket= mdl_ticket;

  table->next= thd->open_tables;		/* Link into simple list */
  thd->set_open_tables(table);

  table->reginfo.lock_type=TL_READ;		/* Assume read */

 reset:
  /*
    Check that there is no reference to a condition from an earlier query
    (cf. Bug#58553). 
  */
  DBUG_ASSERT(table->file->pushed_cond == NULL);
  table_list->updatable= 1; // It is not derived table nor non-updatable VIEW
  table_list->table= table;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (unlikely(table->part_info))
  {
    /* Partitions specified were incorrect.*/
    if (part_names_error)
    {
      table->file->print_error(part_names_error, MYF(0));
      DBUG_RETURN(true);
    }
  }
  else if (table_list->partition_names)
  {
    /* Don't allow PARTITION () clause on a nonpartitioned table */
    my_error(ER_PARTITION_CLAUSE_ON_NONPARTITIONED, MYF(0));
    DBUG_RETURN(true);
  }
#endif
  if (table_list->sequence && table->s->table_type != TABLE_TYPE_SEQUENCE)
  {
    my_error(ER_NOT_SEQUENCE, MYF(0), table_list->db.str, table_list->alias.str);
    DBUG_RETURN(true);
  }

  table->init(thd, table_list);

  DBUG_RETURN(FALSE);

err_lock:
  tdc_release_share(share);

  DBUG_PRINT("exit", ("failed"));
  DBUG_RETURN(TRUE);
}


/**
   Find table in the list of open tables.

   @param list       List of TABLE objects to be inspected.
   @param db         Database name
   @param table_name Table name

   @return Pointer to the TABLE object found, 0 if no table found.
*/

TABLE *find_locked_table(TABLE *list, const char *db, const char *table_name)
{
  char	key[MAX_DBKEY_LENGTH];
  uint key_length= tdc_create_key(key, db, table_name);

  for (TABLE *table= list; table ; table=table->next)
  {
    if (table->s->table_cache_key.length == key_length &&
	!memcmp(table->s->table_cache_key.str, key, key_length))
      return table;
  }
  return(0);
}


/**
   Find instance of TABLE with upgradable or exclusive metadata
   lock from the list of open tables, emit error if no such table
   found.

   @param thd        Thread context
   @param db         Database name.
   @param table_name Name of table.
   @param p_error    In the case of an error (when the function returns NULL)
                     the error number is stored there.
                     If the p_error is NULL, function launches the error itself.

   @note This function checks if the connection holds a global IX
         metadata lock. If no such lock is found, it is not safe to
         upgrade the lock and ER_TABLE_NOT_LOCKED_FOR_WRITE will be
         reported.

   @return Pointer to TABLE instance with MDL_SHARED_UPGRADABLE
           MDL_SHARED_NO_WRITE, MDL_SHARED_NO_READ_WRITE, or
           MDL_EXCLUSIVE metadata lock, NULL otherwise.
*/

TABLE *find_table_for_mdl_upgrade(THD *thd, const char *db,
                                  const char *table_name, int *p_error)
{
  TABLE *tab= find_locked_table(thd->open_tables, db, table_name);
  int error;

  if (unlikely(!tab))
  {
    error= ER_TABLE_NOT_LOCKED;
    goto err_exit;
  }

  /*
    It is not safe to upgrade the metadata lock without a global IX lock.
    This can happen with FLUSH TABLES <list> WITH READ LOCK as we in these
    cases don't take a global IX lock in order to be compatible with
    global read lock.
  */
  if (unlikely(!thd->mdl_context.is_lock_owner(MDL_key::BACKUP, "", "",
                                               MDL_BACKUP_DDL)))
  {
    error= ER_TABLE_NOT_LOCKED_FOR_WRITE;
    goto err_exit;
  }

  while (tab->mdl_ticket != NULL &&
         !tab->mdl_ticket->is_upgradable_or_exclusive() &&
         (tab= find_locked_table(tab->next, db, table_name)))
    continue;

  if (unlikely(!tab))
  {
    error= ER_TABLE_NOT_LOCKED_FOR_WRITE;
    goto err_exit;
  }

  return tab;

err_exit:
  if (p_error)
    *p_error= error;
  else
    my_error(error, MYF(0), table_name);

  return NULL;
}


/***********************************************************************
  class Locked_tables_list implementation. Declared in sql_class.h
************************************************************************/

/**
  Enter LTM_LOCK_TABLES mode.

  Enter the LOCK TABLES mode using all the tables that are
  currently open and locked in this connection.
  Initializes a TABLE_LIST instance for every locked table.

  @param  thd  thread handle

  @return TRUE if out of memory.
*/

bool
Locked_tables_list::init_locked_tables(THD *thd)
{
  DBUG_ASSERT(thd->locked_tables_mode == LTM_NONE);
  DBUG_ASSERT(m_locked_tables == NULL);
  DBUG_ASSERT(m_reopen_array == NULL);
  DBUG_ASSERT(m_locked_tables_count == 0);

  for (TABLE *table= thd->open_tables; table;
       table= table->next, m_locked_tables_count++)
  {
    TABLE_LIST *src_table_list= table->pos_in_table_list;
    LEX_CSTRING db, table_name, alias;

    db.length=         table->s->db.length;
    table_name.length= table->s->table_name.length;
    alias.length=      table->alias.length();
    TABLE_LIST *dst_table_list;

    if (! multi_alloc_root(&m_locked_tables_root,
                           &dst_table_list, sizeof(*dst_table_list),
                           &db.str, (size_t) db.length + 1,
                           &table_name.str, (size_t) table_name.length + 1,
                           &alias.str, (size_t) alias.length + 1,
                           NullS))
    {
      reset();
      return TRUE;
    }

    memcpy((char*) db.str,         table->s->db.str, db.length + 1);
    memcpy((char*) table_name.str, table->s->table_name.str,
           table_name.length + 1);
    memcpy((char*) alias.str,      table->alias.c_ptr(), alias.length + 1);
    dst_table_list->init_one_table(&db, &table_name,
                                   &alias, table->reginfo.lock_type);
    dst_table_list->table= table;
    dst_table_list->mdl_request.ticket= src_table_list->mdl_request.ticket;

    /* Link last into the list of tables */
    *(dst_table_list->prev_global= m_locked_tables_last)= dst_table_list;
    m_locked_tables_last= &dst_table_list->next_global;
    table->pos_in_locked_tables= dst_table_list;
  }
  if (m_locked_tables_count)
  {
    /**
      Allocate an auxiliary array to pass to mysql_lock_tables()
      in reopen_tables(). reopen_tables() is a critical
      path and we don't want to complicate it with extra allocations.
    */
    m_reopen_array= (TABLE**)alloc_root(&m_locked_tables_root,
                                        sizeof(TABLE*) *
                                        (m_locked_tables_count+1));
    if (m_reopen_array == NULL)
    {
      reset();
      return TRUE;
    }
  }

  TRANSACT_TRACKER(add_trx_state(thd, TX_LOCKED_TABLES));

  thd->enter_locked_tables_mode(LTM_LOCK_TABLES);

  return FALSE;
}


/**
  Leave LTM_LOCK_TABLES mode if it's been entered.

  Close all locked tables, free memory, and leave the mode.

  @note This function is a no-op if we're not in LOCK TABLES.
*/

void
Locked_tables_list::unlock_locked_tables(THD *thd)
{
  DBUG_ASSERT(!thd->in_sub_stmt &&
              !(thd->state_flags & Open_tables_state::BACKUPS_AVAIL));
  /*
    Sic: we must be careful to not close open tables if
    we're not in LOCK TABLES mode: unlock_locked_tables() is
    sometimes called implicitly, expecting no effect on
    open tables, e.g. from begin_trans().
  */
  if (thd->locked_tables_mode != LTM_LOCK_TABLES)
    return;

  for (TABLE_LIST *table_list= m_locked_tables;
       table_list; table_list= table_list->next_global)
  {
    /*
      Clear the position in the list, the TABLE object will be
      returned to the table cache.
    */
    if (table_list->table)                    // If not closed
      table_list->table->pos_in_locked_tables= NULL;
  }
  thd->leave_locked_tables_mode();

  TRANSACT_TRACKER(clear_trx_state(thd, TX_LOCKED_TABLES));

  DBUG_ASSERT(thd->transaction.stmt.is_empty());
  close_thread_tables(thd);

  /*
    We rely on the caller to implicitly commit the
    transaction and release transactional locks.
  */

  /*
    After closing tables we can free memory used for storing lock
    request for metadata locks and TABLE_LIST elements.
  */
  reset();
}


/**
  Remove all meta data locks associated with table and release locked
  table mode if there is no locked tables anymore
*/

void
Locked_tables_list::unlock_locked_table(THD *thd, MDL_ticket *mdl_ticket)
{
  /*
    Ensure we are in locked table mode.
    As this function is only called on error condition it's better
    to check this condition here than in the caller.
  */
  if (thd->locked_tables_mode != LTM_LOCK_TABLES)
    return;

  if (mdl_ticket)
  {
    /*
      Under LOCK TABLES we may have several instances of table open
      and locked and therefore have to remove several metadata lock
      requests associated with them.
    */
    thd->mdl_context.release_all_locks_for_name(mdl_ticket);
  }

  if (thd->lock->table_count == 0)
    unlock_locked_tables(thd);
}


/*
  Free memory allocated for storing locks
*/

void Locked_tables_list::reset()
{
  free_root(&m_locked_tables_root, MYF(0));
  m_locked_tables= NULL;
  m_locked_tables_last= &m_locked_tables;
  m_reopen_array= NULL;
  m_locked_tables_count= 0;
}


/**
  Unlink a locked table from the locked tables list, either
  temporarily or permanently.

  @param  thd        thread handle
  @param  table_list the element of locked tables list.
                     The implementation assumes that this argument
                     points to a TABLE_LIST element linked into
                     the locked tables list. Passing a TABLE_LIST
                     instance that is not part of locked tables
                     list will lead to a crash.
  @param  remove_from_locked_tables
                      TRUE if the table is removed from the list
                      permanently.

  This function is a no-op if we're not under LOCK TABLES.

  @sa Locked_tables_list::reopen_tables()
*/


void Locked_tables_list::unlink_from_list(THD *thd,
                                          TABLE_LIST *table_list,
                                          bool remove_from_locked_tables)
{
  /*
    If mode is not LTM_LOCK_TABLES, we needn't do anything. Moreover,
    outside this mode pos_in_locked_tables value is not trustworthy.
  */
  if (thd->locked_tables_mode != LTM_LOCK_TABLES &&
      thd->locked_tables_mode != LTM_PRELOCKED_UNDER_LOCK_TABLES)
    return;

  /*
    table_list must be set and point to pos_in_locked_tables of some
    table.
  */
  DBUG_ASSERT(table_list->table->pos_in_locked_tables == table_list);

  /* Clear the pointer, the table will be returned to the table cache. */
  table_list->table->pos_in_locked_tables= NULL;

  /* Mark the table as closed in the locked tables list. */
  table_list->table= NULL;

  /*
    If the table is being dropped or renamed, remove it from
    the locked tables list (implicitly drop the LOCK TABLES lock
    on it).
  */
  if (remove_from_locked_tables)
  {
    *table_list->prev_global= table_list->next_global;
    if (table_list->next_global == NULL)
      m_locked_tables_last= table_list->prev_global;
    else
      table_list->next_global->prev_global= table_list->prev_global;
    m_locked_tables_count--;
  }
}

/**
  This is an attempt to recover (somewhat) in case of an error.
  If we failed to reopen a closed table, let's unlink it from the
  list and forget about it. From a user perspective that would look
  as if the server "lost" the lock on one of the locked tables.

  @note This function is a no-op if we're not under LOCK TABLES.
*/

void Locked_tables_list::
unlink_all_closed_tables(THD *thd, MYSQL_LOCK *lock, size_t reopen_count)
{
  /* If we managed to take a lock, unlock tables and free the lock. */
  if (lock)
    mysql_unlock_tables(thd, lock);
  /*
    If a failure happened in reopen_tables(), we may have succeeded
    reopening some tables, but not all.
    This works when the connection was killed in mysql_lock_tables().
  */
  if (reopen_count)
  {
    while (reopen_count--)
    {
      /*
        When closing the table, we must remove it
        from thd->open_tables list.
        We rely on the fact that open_table() that was used
        in reopen_tables() always links the opened table
        to the beginning of the open_tables list.
      */
      DBUG_ASSERT(thd->open_tables == m_reopen_array[reopen_count]);

      thd->open_tables->pos_in_locked_tables->table= NULL;

      close_thread_table(thd, &thd->open_tables);
    }
  }
  /* Exclude all closed tables from the LOCK TABLES list. */
  for (TABLE_LIST *table_list= m_locked_tables; table_list; table_list=
       table_list->next_global)
  {
    if (table_list->table == NULL)
    {
      /* Unlink from list. */
      *table_list->prev_global= table_list->next_global;
      if (table_list->next_global == NULL)
        m_locked_tables_last= table_list->prev_global;
      else
        table_list->next_global->prev_global= table_list->prev_global;
      m_locked_tables_count--;
    }
  }

  /* If no tables left, do an automatic UNLOCK TABLES */
  if (thd->lock && thd->lock->table_count == 0)
    unlock_locked_tables(thd);
}


/**
  Reopen the tables locked with LOCK TABLES and temporarily closed
  by a DDL statement or FLUSH TABLES.

  @note This function is a no-op if we're not under LOCK TABLES.

  @return TRUE if an error reopening the tables. May happen in
               case of some fatal system error only, e.g. a disk
               corruption, out of memory or a serious bug in the
               locking.
*/

bool
Locked_tables_list::reopen_tables(THD *thd, bool need_reopen)
{
  Open_table_context ot_ctx(thd, MYSQL_OPEN_REOPEN);
  uint reopen_count= 0;
  MYSQL_LOCK *lock;
  MYSQL_LOCK *merged_lock;
  DBUG_ENTER("Locked_tables_list::reopen_tables");

  for (TABLE_LIST *table_list= m_locked_tables;
       table_list; table_list= table_list->next_global)
  {
    if (need_reopen)
    {
      if (!table_list->table || !table_list->table->needs_reopen())
        continue;
      /* no need to remove the table from the TDC here, thus (TABLE*)1 */
      close_all_tables_for_name(thd, table_list->table->s,
                                HA_EXTRA_NOT_USED, (TABLE*)1);
      DBUG_ASSERT(table_list->table == NULL);
    }
    else
    {
      if (table_list->table)                      /* The table was not closed */
          continue;
    }

    /* Links into thd->open_tables upon success */
    if (open_table(thd, table_list, &ot_ctx))
    {
      unlink_all_closed_tables(thd, 0, reopen_count);
      DBUG_RETURN(TRUE);
    }
    table_list->table->pos_in_locked_tables= table_list;
    /* See also the comment on lock type in init_locked_tables(). */
    table_list->table->reginfo.lock_type= table_list->lock_type;

    DBUG_ASSERT(reopen_count < m_locked_tables_count);
    m_reopen_array[reopen_count++]= table_list->table;
  }
  if (reopen_count)
  {
    thd->in_lock_tables= 1;
    /*
      We re-lock all tables with mysql_lock_tables() at once rather
      than locking one table at a time because of the case
      reported in Bug#45035: when the same table is present
      in the list many times, thr_lock.c fails to grant READ lock
      on a table that is already locked by WRITE lock, even if
      WRITE lock is taken by the same thread. If READ and WRITE
      lock are passed to thr_lock.c in the same list, everything
      works fine. Patching legacy code of thr_lock.c is risking to
      break something else.
    */
    lock= mysql_lock_tables(thd, m_reopen_array, reopen_count,
                            MYSQL_OPEN_REOPEN | MYSQL_LOCK_USE_MALLOC);
    thd->in_lock_tables= 0;
    if (lock == NULL || (merged_lock=
                         mysql_lock_merge(thd->lock, lock)) == NULL)
    {
      unlink_all_closed_tables(thd, lock, reopen_count);
      if (! thd->killed)
        my_error(ER_LOCK_DEADLOCK, MYF(0));
      DBUG_RETURN(TRUE);
    }
    thd->lock= merged_lock;
  }
  DBUG_RETURN(FALSE);
}

/**
  Add back a locked table to the locked list that we just removed from it.
  This is needed in CREATE OR REPLACE TABLE where we are dropping, creating
  and re-opening a locked table.

  @return 0  0k
  @return 1  error
*/

bool Locked_tables_list::restore_lock(THD *thd, TABLE_LIST *dst_table_list,
                                      TABLE *table, MYSQL_LOCK *lock)
{
  MYSQL_LOCK *merged_lock;
  DBUG_ENTER("restore_lock");
  DBUG_ASSERT(!strcmp(dst_table_list->table_name.str, table->s->table_name.str));

  /* Ensure we have the memory to add the table back */
  if (!(merged_lock= mysql_lock_merge(thd->lock, lock)))
    DBUG_RETURN(1);
  thd->lock= merged_lock;

  /* Link to the new table */
  dst_table_list->table= table;
  /*
    The lock type may have changed (normally it should not as create
    table will lock the table in write mode
  */
  dst_table_list->lock_type= table->reginfo.lock_type;
  table->pos_in_locked_tables= dst_table_list;

  add_back_last_deleted_lock(dst_table_list);

  table->mdl_ticket->downgrade_lock(table->reginfo.lock_type >=
                                    TL_WRITE_ALLOW_WRITE ? 
                                    MDL_SHARED_NO_READ_WRITE :
                                    MDL_SHARED_READ);

  DBUG_RETURN(0);
}

/*
  Add back the last deleted lock structure.
  This should be followed by a call to reopen_tables() to
  open the table.
*/

void Locked_tables_list::add_back_last_deleted_lock(TABLE_LIST *dst_table_list)
{
  /* Link the lock back in the locked tables list */
  dst_table_list->prev_global= m_locked_tables_last;
  *m_locked_tables_last= dst_table_list;
  m_locked_tables_last= &dst_table_list->next_global;
  dst_table_list->next_global= 0;
  m_locked_tables_count++;
}


#ifndef DBUG_OFF
/* Cause a spurious statement reprepare for debug purposes. */
static bool inject_reprepare(THD *thd)
{
  if (thd->m_reprepare_observer && thd->stmt_arena->is_reprepared == FALSE)
  {
    thd->m_reprepare_observer->report_error(thd);
    return TRUE;
  }

  return FALSE;
}
#endif

/**
  Compare metadata versions of an element obtained from the table
  definition cache and its corresponding node in the parse tree.

  @details If the new and the old values mismatch, invoke
  Metadata_version_observer.
  At prepared statement prepare, all TABLE_LIST version values are
  NULL and we always have a mismatch. But there is no observer set
  in THD, and therefore no error is reported. Instead, we update
  the value in the parse tree, effectively recording the original
  version.
  At prepared statement execute, an observer may be installed.  If
  there is a version mismatch, we push an error and return TRUE.

  For conventional execution (no prepared statements), the
  observer is never installed.

  @sa Execute_observer
  @sa check_prepared_statement() to see cases when an observer is installed
  @sa TABLE_LIST::is_table_ref_id_equal()
  @sa TABLE_SHARE::get_table_ref_id()

  @param[in]      thd         used to report errors
  @param[in,out]  tables      TABLE_LIST instance created by the parser
                              Metadata version information in this object
                              is updated upon success.
  @param[in]      table_share an element from the table definition cache

  @retval  TRUE  an error, which has been reported
  @retval  FALSE success, version in TABLE_LIST has been updated
*/

static bool
check_and_update_table_version(THD *thd,
                               TABLE_LIST *tables, TABLE_SHARE *table_share)
{
  if (! tables->is_table_ref_id_equal(table_share))
  {
    if (thd->m_reprepare_observer &&
        thd->m_reprepare_observer->report_error(thd))
    {
      /*
        Version of the table share is different from the
        previous execution of the prepared statement, and it is
        unacceptable for this SQLCOM. Error has been reported.
      */
      DBUG_ASSERT(thd->is_error());
      return TRUE;
    }
    /* Always maintain the latest version and type */
    tables->set_table_ref_id(table_share);
  }

  DBUG_EXECUTE_IF("reprepare_each_statement", return inject_reprepare(thd););
  return FALSE;
}


/**
  Compares versions of a stored routine obtained from the sp cache
  and the version used at prepare.

  @details If the new and the old values mismatch, invoke
  Metadata_version_observer.
  At prepared statement prepare, all Sroutine_hash_entry version values
  are NULL and we always have a mismatch. But there is no observer set
  in THD, and therefore no error is reported. Instead, we update
  the value in Sroutine_hash_entry, effectively recording the original
  version.
  At prepared statement execute, an observer may be installed.  If
  there is a version mismatch, we push an error and return TRUE.

  For conventional execution (no prepared statements), the
  observer is never installed.

  @param[in]      thd         used to report errors
  @param[in/out]  rt          pointer to stored routine entry in the
                              parse tree
  @param[in]      sp          pointer to stored routine cache entry.
                              Can be NULL if there is no such routine.
  @retval  TRUE  an error, which has been reported
  @retval  FALSE success, version in Sroutine_hash_entry has been updated
*/

static bool
check_and_update_routine_version(THD *thd, Sroutine_hash_entry *rt,
                                 sp_head *sp)
{
  ulong spc_version= sp_cache_version();
  /* sp is NULL if there is no such routine. */
  ulong version= sp ? sp->sp_cache_version() : spc_version;
  /*
    If the version in the parse tree is stale,
    or the version in the cache is stale and sp is not used,
    we need to reprepare.
    Sic: version != spc_version <--> sp is not NULL.
  */
  if (rt->m_sp_cache_version != version ||
      (version != spc_version && !sp->is_invoked()))
  {
    if (thd->m_reprepare_observer &&
        thd->m_reprepare_observer->report_error(thd))
    {
      /*
        Version of the sp cache is different from the
        previous execution of the prepared statement, and it is
        unacceptable for this SQLCOM. Error has been reported.
      */
      DBUG_ASSERT(thd->is_error());
      return TRUE;
    }
    /* Always maintain the latest cache version. */
    rt->m_sp_cache_version= version;
  }
  return FALSE;
}


/**
   Open view by getting its definition from disk (and table cache in future).

   @param thd               Thread handle
   @param table_list        TABLE_LIST with db, table_name & belong_to_view
   @param flags             Flags which modify how we open the view

   @todo This function is needed for special handling of views under
         LOCK TABLES. We probably should get rid of it in long term.

   @return FALSE if success, TRUE - otherwise.
*/

bool tdc_open_view(THD *thd, TABLE_LIST *table_list, uint flags)
{
  TABLE not_used;
  TABLE_SHARE *share;
  bool err= TRUE;

  if (!(share= tdc_acquire_share(thd, table_list, GTS_VIEW)))
    return TRUE;

  DBUG_ASSERT(share->is_view);

  if (flags & CHECK_METADATA_VERSION)
  {
    /*
      Check TABLE_SHARE-version of view only if we have been instructed to do
      so. We do not need to check the version if we're executing CREATE VIEW or
      ALTER VIEW statements.

      In the future, this functionality should be moved out from
      tdc_open_view(), and  tdc_open_view() should became a part of a clean
      table-definition-cache interface.
    */
    if (check_and_update_table_version(thd, table_list, share))
      goto ret;
  }

  err= mysql_make_view(thd, share, table_list, (flags & OPEN_VIEW_NO_PARSE));
ret:
  tdc_release_share(share);

  return err;
}


/**
   Finalize the process of TABLE creation by loading table triggers
   and taking action if a HEAP table content was emptied implicitly.
*/

static bool open_table_entry_fini(THD *thd, TABLE_SHARE *share, TABLE *entry)
{
  if (Table_triggers_list::check_n_load(thd, &share->db,
                                        &share->table_name, entry, 0))
    return TRUE;

  /*
    If we are here, there was no fatal error (but error may be still
    unitialized).
  */
  if (unlikely(entry->file->implicit_emptied))
  {
    entry->file->implicit_emptied= 0;
    if (mysql_bin_log.is_open())
    {
      char query_buf[2*FN_REFLEN + 21];
      String query(query_buf, sizeof(query_buf), system_charset_info);

      query.length(0);
      query.append("DELETE FROM ");
      append_identifier(thd, &query, &share->db);
      query.append(".");
      append_identifier(thd, &query, &share->table_name);

      /*
        we bypass thd->binlog_query() here,
        as it does a lot of extra work, that is simply wrong in this case
      */
      Query_log_event qinfo(thd, query.ptr(), query.length(),
                            FALSE, TRUE, TRUE, 0);
      if (mysql_bin_log.write(&qinfo))
        return TRUE;
    }
  }
  return FALSE;
}


/**
   Auxiliary routine which is used for performing automatical table repair.
*/

static bool auto_repair_table(THD *thd, TABLE_LIST *table_list)
{
  TABLE_SHARE *share;
  TABLE *entry;
  bool result= TRUE;

  thd->clear_error();

  if (!(entry= (TABLE*)my_malloc(sizeof(TABLE), MYF(MY_WME))))
    return result;

  if (!(share= tdc_acquire_share(thd, table_list, GTS_TABLE)))
    goto end_free;

  DBUG_ASSERT(! share->is_view);

  if (open_table_from_share(thd, share, &table_list->alias,
                            HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                            EXTRA_RECORD,
                            ha_open_options | HA_OPEN_FOR_REPAIR,
                            entry, FALSE) || ! entry->file ||
      (entry->file->is_crashed() && entry->file->ha_check_and_repair(thd)))
  {
    /* Give right error message */
    thd->clear_error();
    my_error(ER_NOT_KEYFILE, MYF(0), share->table_name.str);
    sql_print_error("Couldn't repair table: %s.%s", share->db.str,
                    share->table_name.str);
    if (entry->file)
      closefrm(entry);
  }
  else
  {
    thd->clear_error();			// Clear error message
    closefrm(entry);
    result= FALSE;
  }

  tdc_release_share(share);
  /* Remove the repaired share from the table cache. */
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL,
                   table_list->db.str, table_list->table_name.str,
                   FALSE);
end_free:
  my_free(entry);
  return result;
}


/** Open_table_context */

Open_table_context::Open_table_context(THD *thd, uint flags)
  :m_thd(thd),
   m_failed_table(NULL),
   m_start_of_statement_svp(thd->mdl_context.mdl_savepoint()),
   m_timeout(flags & MYSQL_LOCK_IGNORE_TIMEOUT ?
             LONG_TIMEOUT : thd->variables.lock_wait_timeout),
   m_flags(flags),
   m_action(OT_NO_ACTION),
   m_has_locks(thd->mdl_context.has_locks()),
   m_has_protection_against_grl(0)
{}


/**
  Check if we can back-off and set back off action if we can.
  Otherwise report and return error.

  @retval  TRUE if back-off is impossible.
  @retval  FALSE if we can back off. Back off action has been set.
*/

bool
Open_table_context::
request_backoff_action(enum_open_table_action action_arg,
                       TABLE_LIST *table)
{
  /*
    A back off action may be one of three kinds:

    * We met a broken table that needs repair, or a table that
      is not present on this MySQL server and needs re-discovery.
      To perform the action, we need an exclusive metadata lock on
      the table. Acquiring X lock while holding other shared
      locks can easily lead to deadlocks. We rely on MDL deadlock
      detector to discover them. If this is a multi-statement
      transaction that holds metadata locks for completed statements,
      we should keep these locks after discovery/repair.
      The action type in this case is OT_DISCOVER or OT_REPAIR.
    * Our attempt to acquire an MDL lock lead to a deadlock,
      detected by the MDL deadlock detector. The current
      session was chosen a victim. If this is a multi-statement
      transaction that holds metadata locks taken by completed
      statements, restarting locking for the current statement
      may lead to a livelock. Releasing locks of completed
      statements can not be done as will lead to violation
      of ACID. Thus, again, if m_has_locks is set,
      we report an error. Otherwise, when there are no metadata
      locks other than which belong to this statement, we can
      try to recover from error by releasing all locks and
      restarting the pre-locking.
      Similarly, a deadlock error can occur when the
      pre-locking process met a TABLE_SHARE that is being
      flushed, and unsuccessfully waited for the flush to
      complete. A deadlock in this case can happen, e.g.,
      when our session is holding a metadata lock that
      is being waited on by a session which is using
      the table which is being flushed. The only way
      to recover from this error is, again, to close all
      open tables, release all locks, and retry pre-locking.
      Action type name is OT_REOPEN_TABLES. Re-trying
      while holding some locks may lead to a livelock,
      and thus we don't do it.
    * Finally, this session has open TABLEs from different
      "generations" of the table cache. This can happen, e.g.,
      when, after this session has successfully opened one
      table used for a statement, FLUSH TABLES interfered and
      expelled another table used in it. FLUSH TABLES then
      blocks and waits on the table already opened by this
      statement.
      We detect this situation by ensuring that table cache
      version of all tables used in a statement is the same.
      If it isn't, all tables needs to be reopened.
      Note, that we can always perform a reopen in this case,
      even if we already have metadata locks, since we don't
      keep tables open between statements and a livelock
      is not possible.
  */
  if (action_arg == OT_BACKOFF_AND_RETRY && m_has_locks)
  {
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    m_thd->mark_transaction_to_rollback(true);
    return TRUE;
  }
  /*
    If auto-repair or discovery are requested, a pointer to table
    list element must be provided.
  */
  if (table)
  {
    DBUG_ASSERT(action_arg == OT_DISCOVER || action_arg == OT_REPAIR);
    m_failed_table= (TABLE_LIST*) m_thd->alloc(sizeof(TABLE_LIST));
    if (m_failed_table == NULL)
      return TRUE;
    m_failed_table->init_one_table(&table->db, &table->table_name, &table->alias, TL_WRITE);
    m_failed_table->open_strategy= table->open_strategy;
    m_failed_table->mdl_request.set_type(MDL_EXCLUSIVE);
  }
  m_action= action_arg;
  return FALSE;
}


/**
  An error handler to mark transaction to rollback on DEADLOCK error
  during DISCOVER / REPAIR.
*/
class MDL_deadlock_discovery_repair_handler : public Internal_error_handler
{
public:
  virtual bool handle_condition(THD *thd,
                                  uint sql_errno,
                                  const char* sqlstate,
                                  Sql_condition::enum_warning_level *level,
                                  const char* msg,
                                  Sql_condition ** cond_hdl)
  {
    if (sql_errno == ER_LOCK_DEADLOCK)
    {
      thd->mark_transaction_to_rollback(true);
    }
    /*
      We have marked this transaction to rollback. Return false to allow
      error to be reported or handled by other handlers.
    */
    return false;
  }
};

/**
   Recover from failed attempt of open table by performing requested action.

   @pre This function should be called only with "action" != OT_NO_ACTION
        and after having called @sa close_tables_for_reopen().

   @retval FALSE - Success. One should try to open tables once again.
   @retval TRUE  - Error
*/

bool
Open_table_context::recover_from_failed_open()
{
  bool result= FALSE;
  MDL_deadlock_discovery_repair_handler handler;
  /*
    Install error handler to mark transaction to rollback on DEADLOCK error.
  */
  m_thd->push_internal_handler(&handler);

  /* Execute the action. */
  switch (m_action)
  {
    case OT_BACKOFF_AND_RETRY:
      break;
    case OT_REOPEN_TABLES:
      break;
    case OT_DISCOVER:
      {
        if ((result= lock_table_names(m_thd, m_thd->lex->create_info,
                                      m_failed_table, NULL,
                                      get_timeout(), 0)))
          break;

        tdc_remove_table(m_thd, TDC_RT_REMOVE_ALL, m_failed_table->db.str,
                         m_failed_table->table_name.str, FALSE);

        m_thd->get_stmt_da()->clear_warning_info(m_thd->query_id);
        m_thd->clear_error();                 // Clear error message

        No_such_table_error_handler no_such_table_handler;
        bool open_if_exists= m_failed_table->open_strategy == TABLE_LIST::OPEN_IF_EXISTS;

        if (open_if_exists)
          m_thd->push_internal_handler(&no_such_table_handler);
        
        result= !tdc_acquire_share(m_thd, m_failed_table,
                                   GTS_TABLE | GTS_FORCE_DISCOVERY | GTS_NOLOCK);
        if (open_if_exists)
        {
          m_thd->pop_internal_handler();
          if (result && no_such_table_handler.safely_trapped_errors())
            result= FALSE;
        }

        /*
          Rollback to start of the current statement to release exclusive lock
          on table which was discovered but preserve locks from previous statements
          in current transaction.
        */
        m_thd->mdl_context.rollback_to_savepoint(start_of_statement_svp());
        break;
      }
    case OT_REPAIR:
      {
        if ((result= lock_table_names(m_thd, m_thd->lex->create_info,
                                      m_failed_table, NULL,
                                      get_timeout(), 0)))
          break;

        tdc_remove_table(m_thd, TDC_RT_REMOVE_ALL, m_failed_table->db.str,
                         m_failed_table->table_name.str, FALSE);

        result= auto_repair_table(m_thd, m_failed_table);
        /*
          Rollback to start of the current statement to release exclusive lock
          on table which was discovered but preserve locks from previous statements
          in current transaction.
        */
        m_thd->mdl_context.rollback_to_savepoint(start_of_statement_svp());
        break;
      }
    default:
      DBUG_ASSERT(0);
  }
  m_thd->pop_internal_handler();
  /*
    Reset the pointers to conflicting MDL request and the
    TABLE_LIST element, set when we need auto-discovery or repair,
    for safety.
  */
  m_failed_table= NULL;
  /*
    Reset flag indicating that we have already acquired protection
    against GRL. It is no longer valid as the corresponding lock was
    released by close_tables_for_reopen().
  */
  m_has_protection_against_grl= 0;
  /* Prepare for possible another back-off. */
  m_action= OT_NO_ACTION;
  return result;
}


/*
  Return a appropriate read lock type given a table object.

  @param thd              Thread context
  @param prelocking_ctx   Prelocking context.
  @param table_list       Table list element for table to be locked.
  @param routine_modifies_data
                          Some routine that is invoked by statement
                          modifies data.

  @remark Due to a statement-based replication limitation, statements such as
          INSERT INTO .. SELECT FROM .. and CREATE TABLE .. SELECT FROM need
          to grab a TL_READ_NO_INSERT lock on the source table in order to
          prevent the replication of a concurrent statement that modifies the
          source table. If such a statement gets applied on the slave before
          the INSERT .. SELECT statement finishes, data on the master could
          differ from data on the slave and end-up with a discrepancy between
          the binary log and table state.
          This also applies to SELECT/SET/DO statements which use stored
          functions. Calls to such functions are going to be logged as a
          whole and thus should be serialized against concurrent changes
          to tables used by those functions. This is avoided when functions
          do not modify data but only read it, since in this case nothing is
          written to the binary log. Argument routine_modifies_data
          denotes the same. So effectively, if the statement is not a
          update query and routine_modifies_data is false, then
          prelocking_placeholder does not take importance.

          Furthermore, this does not apply to I_S and log tables as it's
          always unsafe to replicate such tables under statement-based
          replication as the table on the slave might contain other data
          (ie: general_log is enabled on the slave). The statement will
          be marked as unsafe for SBR in decide_logging_format().
  @remark Note that even in prelocked mode it is important to correctly
          determine lock type value. In this mode lock type is passed to
          handler::start_stmt() method and can be used by storage engine,
          for example, to determine what kind of row locks it should acquire
          when reading data from the table.
*/

thr_lock_type read_lock_type_for_table(THD *thd,
                                       Query_tables_list *prelocking_ctx,
                                       TABLE_LIST *table_list,
                                       bool routine_modifies_data)
{
  /*
    In cases when this function is called for a sub-statement executed in
    prelocked mode we can't rely on OPTION_BIN_LOG flag in THD::options
    bitmap to determine that binary logging is turned on as this bit can
    be cleared before executing sub-statement. So instead we have to look
    at THD::variables::sql_log_bin member.
  */
  bool log_on= mysql_bin_log.is_open() && thd->variables.sql_log_bin;
  if ((log_on == FALSE) || (thd->wsrep_binlog_format() == BINLOG_FORMAT_ROW) ||
      (table_list->table->s->table_category == TABLE_CATEGORY_LOG) ||
      (table_list->table->s->table_category == TABLE_CATEGORY_PERFORMANCE) ||
      !(is_update_query(prelocking_ctx->sql_command) ||
        (routine_modifies_data && table_list->prelocking_placeholder) ||
        (thd->locked_tables_mode > LTM_LOCK_TABLES)))
    return TL_READ;
  else
    return TL_READ_NO_INSERT;
}


/*
  Extend the prelocking set with tables and routines used by a routine.

  @param[in]  thd                   Thread context.
  @param[in]  rt                    Element of prelocking set to be processed.
  @param[in]  ot_ctx                Context of open_table used to recover from
                                    locking failures.
  @retval false  Success.
  @retval true   Failure (Conflicting metadata lock, OOM, other errors).
*/
static bool
sp_acquire_mdl(THD *thd, Sroutine_hash_entry *rt, Open_table_context *ot_ctx)
{
  DBUG_ENTER("sp_acquire_mdl");
  /*
    Since we acquire only shared lock on routines we don't
    need to care about global intention exclusive locks.
  */
  DBUG_ASSERT(rt->mdl_request.type == MDL_SHARED);

  /*
    Waiting for a conflicting metadata lock to go away may
    lead to a deadlock, detected by MDL subsystem.
    If possible, we try to resolve such deadlocks by releasing all
    metadata locks and restarting the pre-locking process.
    To prevent the error from polluting the diagnostics area
    in case of successful resolution, install a special error
    handler for ER_LOCK_DEADLOCK error.
  */
  MDL_deadlock_handler mdl_deadlock_handler(ot_ctx);

  thd->push_internal_handler(&mdl_deadlock_handler);
  bool result= thd->mdl_context.acquire_lock(&rt->mdl_request,
                                             ot_ctx->get_timeout());
  thd->pop_internal_handler();

  DBUG_RETURN(result);
}


/*
  Handle element of prelocking set other than table. E.g. cache routine
  and, if prelocking strategy prescribes so, extend the prelocking set
  with tables and routines used by it.

  @param[in]  thd                   Thread context.
  @param[in]  prelocking_ctx        Prelocking context.
  @param[in]  rt                    Element of prelocking set to be processed.
  @param[in]  prelocking_strategy   Strategy which specifies how the
                                    prelocking set should be extended when
                                    one of its elements is processed.
  @param[in]  has_prelocking_list   Indicates that prelocking set/list for
                                    this statement has already been built.
  @param[in]  ot_ctx                Context of open_table used to recover from
                                    locking failures.
  @param[out] need_prelocking       Set to TRUE if it was detected that this
                                    statement will require prelocked mode for
                                    its execution, not touched otherwise.
  @param[out] routine_modifies_data Set to TRUE if it was detected that this
                                    routine does modify table data.

  @retval FALSE  Success.
  @retval TRUE   Failure (Conflicting metadata lock, OOM, other errors).
*/

static bool
open_and_process_routine(THD *thd, Query_tables_list *prelocking_ctx,
                         Sroutine_hash_entry *rt,
                         Prelocking_strategy *prelocking_strategy,
                         bool has_prelocking_list,
                         Open_table_context *ot_ctx,
                         bool *need_prelocking, bool *routine_modifies_data)
{
  MDL_key::enum_mdl_namespace mdl_type= rt->mdl_request.key.mdl_namespace();
  DBUG_ENTER("open_and_process_routine");

  *routine_modifies_data= false;

  switch (mdl_type)
  {
  case MDL_key::PACKAGE_BODY:
    DBUG_ASSERT(rt != (Sroutine_hash_entry*)prelocking_ctx->sroutines_list.first);
    /*
      No need to cache the package body itself.
      It gets cached during open_and_process_routine()
      for the first used package routine. See the package related code
      in the "case" below.
    */
    if (sp_acquire_mdl(thd, rt, ot_ctx))
      DBUG_RETURN(TRUE);
    break;
  case MDL_key::FUNCTION:
  case MDL_key::PROCEDURE:
    {
      sp_head *sp;
      /*
        Try to get MDL lock on the routine.
        Note that we do not take locks on top-level CALLs as this can
        lead to a deadlock. Not locking top-level CALLs does not break
        the binlog as only the statements in the called procedure show
        up there, not the CALL itself.
      */
      if (rt != (Sroutine_hash_entry*)prelocking_ctx->sroutines_list.first ||
          mdl_type != MDL_key::PROCEDURE)
      {
        /*
          TODO: If this is a package routine, we should not put MDL
          TODO: on the routine itself. We should put only the package MDL.
        */
        if (sp_acquire_mdl(thd, rt, ot_ctx))
          DBUG_RETURN(TRUE);

        /* Ensures the routine is up-to-date and cached, if exists. */
        if (rt->sp_cache_routine(thd, has_prelocking_list, &sp))
          DBUG_RETURN(TRUE);

        /* Remember the version of the routine in the parse tree. */
        if (check_and_update_routine_version(thd, rt, sp))
          DBUG_RETURN(TRUE);

        /* 'sp' is NULL when there is no such routine. */
        if (sp)
        {
          *routine_modifies_data= sp->modifies_data();

          if (!has_prelocking_list)
          {
            prelocking_strategy->handle_routine(thd, prelocking_ctx, rt, sp,
                                                need_prelocking);
            if (sp->m_parent)
            {
              /*
                If it's a package routine, we need also to handle the
                package body, as its initialization section can use
                some tables and routine calls.
                TODO: Only package public routines actually need this.
                TODO: Skip package body handling for private routines.
              */
              *routine_modifies_data|= sp->m_parent->modifies_data();
              prelocking_strategy->handle_routine(thd, prelocking_ctx, rt,
                                                  sp->m_parent,
                                                  need_prelocking);
            }
          }
        }
      }
      else
      {
        /*
          If it's a top level call, just make sure we have a recent
          version of the routine, if it exists.
          Validating routine version is unnecessary, since CALL
          does not affect the prepared statement prelocked list.
        */
        if (rt->sp_cache_routine(thd, false, &sp))
          DBUG_RETURN(TRUE);
      }
    }
    break;
  case MDL_key::TRIGGER:
    /**
      We add trigger entries to lex->sroutines_list, but we don't
      load them here. The trigger entry is only used when building
      a transitive closure of objects used in a statement, to avoid
      adding to this closure objects that are used in the trigger more
      than once.
      E.g. if a trigger trg refers to table t2, and the trigger table t1
      is used multiple times in the statement (say, because it's used in
      function f1() twice), we will only add t2 once to the list of
      tables to prelock.

      We don't take metadata locks on triggers either: they are protected
      by a respective lock on the table, on which the trigger is defined.

      The only two cases which give "trouble" are SHOW CREATE TRIGGER
      and DROP TRIGGER statements. For these, statement syntax doesn't
      specify the table on which this trigger is defined, so we have
      to make a "dirty" read in the data dictionary to find out the
      table name. Once we discover the table name, we take a metadata
      lock on it, and this protects all trigger operations.
      Of course the table, in theory, may disappear between the dirty
      read and metadata lock acquisition, but in that case we just return
      a run-time error.

      Grammar of other trigger DDL statements (CREATE, DROP) requires
      the table to be specified explicitly, so we use the table metadata
      lock to protect trigger metadata in these statements. Similarly, in
      DML we always use triggers together with their tables, and thus don't
      need to take separate metadata locks on them.
    */
    break;
  default:
    /* Impossible type value. */
    DBUG_ASSERT(0);
  }
  DBUG_RETURN(FALSE);
}


/**
  Handle table list element by obtaining metadata lock, opening table or view
  and, if prelocking strategy prescribes so, extending the prelocking set with
  tables and routines used by it.

  @param[in]     thd                  Thread context.
  @param[in]     lex                  LEX structure for statement.
  @param[in]     tables               Table list element to be processed.
  @param[in,out] counter              Number of tables which are open.
  @param[in]     flags                Bitmap of flags to modify how the tables
                                      will be open, see open_table() description
                                      for details.
  @param[in]     prelocking_strategy  Strategy which specifies how the
                                      prelocking set should be extended
                                      when table or view is processed.
  @param[in]     has_prelocking_list  Indicates that prelocking set/list for
                                      this statement has already been built.
  @param[in]     ot_ctx               Context used to recover from a failed
                                      open_table() attempt.

  @retval  FALSE  Success.
  @retval  TRUE   Error, reported unless there is a chance to recover from it.
*/

static bool
open_and_process_table(THD *thd, LEX *lex, TABLE_LIST *tables,
                       uint *counter, uint flags,
                       Prelocking_strategy *prelocking_strategy,
                       bool has_prelocking_list,
                       Open_table_context *ot_ctx)
{
  bool error= FALSE;
  bool safe_to_ignore_table= FALSE;
  DBUG_ENTER("open_and_process_table");
  DEBUG_SYNC(thd, "open_and_process_table");

  /*
    Ignore placeholders for derived tables. After derived tables
    processing, link to created temporary table will be put here.
    If this is derived table for view then we still want to process
    routines used by this view.
  */
  if (tables->derived)
  {
    if (!tables->view)
      goto end;
    /*
      We restore view's name and database wiped out by derived tables
      processing and fall back to standard open process in order to
      obtain proper metadata locks and do other necessary steps like
      stored routine processing.
    */
    tables->db= tables->view_db;
    tables->table_name= tables->view_name;
  }
  else if (tables->select_lex) 
  {
    /*
      Check whether 'tables' refers to a table defined in a with clause.
      If so set the reference to the definition in tables->with.
    */ 
    if (!tables->with)
      tables->with= tables->select_lex->find_table_def_in_with_clauses(tables);
    /*
      If 'tables' is defined in a with clause set the pointer to the
      specification from its definition in tables->derived.
    */
    if (tables->with)
    {
      if (tables->is_recursive_with_table() &&
          !tables->is_with_table_recursive_reference())
      {
        tables->with->rec_outer_references++;
        With_element *with_elem= tables->with;
        while ((with_elem= with_elem->get_next_mutually_recursive()) !=
               tables->with)
	  with_elem->rec_outer_references++;
      }
      if (tables->set_as_with_table(thd, tables->with))
        DBUG_RETURN(1);
      else
        goto end;
    }
  }

  if (!tables->derived && is_infoschema_db(&tables->db))
  {
    /*
      Check whether the information schema contains a table
      whose name is tables->schema_table_name
    */
    ST_SCHEMA_TABLE *schema_table;
    schema_table= find_schema_table(thd, &tables->schema_table_name);
    if (!schema_table ||
        (schema_table->hidden &&
         ((sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND) == 0 ||
          /*
            this check is used for show columns|keys from I_S hidden table
          */
          lex->sql_command == SQLCOM_SHOW_FIELDS ||
          lex->sql_command == SQLCOM_SHOW_KEYS)))
    {
      my_error(ER_UNKNOWN_TABLE, MYF(0),
               tables->schema_table_name.str, INFORMATION_SCHEMA_NAME.str);
      DBUG_RETURN(1);
    }
  }
  /*
    If this TABLE_LIST object is a placeholder for an information_schema
    table, create a temporary table to represent the information_schema
    table in the query. Do not fill it yet - will be filled during
    execution.
  */
  if (tables->schema_table)
  {
    /*
      If this information_schema table is merged into a mergeable
      view, ignore it for now -- it will be filled when its respective
      TABLE_LIST is processed. This code works only during re-execution.
    */
    if (tables->view)
    {
      MDL_ticket *mdl_ticket;
      /*
        We still need to take a MDL lock on the merged view to protect
        it from concurrent changes.
      */
      if (!open_table_get_mdl_lock(thd, ot_ctx, &tables->mdl_request,
                                   flags, &mdl_ticket) &&
          mdl_ticket != NULL)
        goto process_view_routines;
      /* Fall-through to return error. */
    }
    else if (!mysql_schema_table(thd, lex, tables) &&
             !check_and_update_table_version(thd, tables, tables->table->s))
    {
      goto end;
    }
    error= TRUE;
    goto end;
  }
  DBUG_PRINT("tcache", ("opening table: '%s'.'%s'  item: %p",
                        tables->db.str, tables->table_name.str, tables));
  (*counter)++;

  /*
    Not a placeholder: must be a base/temporary table or a view. Let us open it.
  */
  if (tables->table)
  {
    /*
      If this TABLE_LIST object has an associated open TABLE object
      (TABLE_LIST::table is not NULL), that TABLE object must be a pre-opened
      temporary table or SEQUENCE (see sequence_insert()).
    */
    DBUG_ASSERT(is_temporary_table(tables) || tables->table->s->sequence);
    if (tables->sequence && tables->table->s->table_type != TABLE_TYPE_SEQUENCE)
    {
        my_error(ER_NOT_SEQUENCE, MYF(0), tables->db.str, tables->alias.str);
        DBUG_RETURN(true);
    }
  }
  else if (tables->open_type == OT_TEMPORARY_ONLY)
  {
    /*
      OT_TEMPORARY_ONLY means that we are in CREATE TEMPORARY TABLE statement.
      Also such table list element can't correspond to prelocking placeholder
      or to underlying table of merge table.
      So existing temporary table should have been preopened by this moment
      and we can simply continue without trying to open temporary or base
      table.
    */
    DBUG_ASSERT(tables->open_strategy);
    DBUG_ASSERT(!tables->prelocking_placeholder);
    DBUG_ASSERT(!tables->parent_l);
    DBUG_RETURN(0);
  }

  /* Not a placeholder: must be a base table or a view. Let us open it. */
  if (tables->prelocking_placeholder)
  {
    /*
      For the tables added by the pre-locking code, attempt to open
      the table but fail silently if the table does not exist.
      The real failure will occur when/if a statement attempts to use
      that table.
    */
    No_such_table_error_handler no_such_table_handler;
    thd->push_internal_handler(&no_such_table_handler);

    /*
      We're opening a table from the prelocking list.

      Since this table list element might have been added after pre-opening
      of temporary tables we have to try to open temporary table for it.

      We can't simply skip this table list element and postpone opening of
      temporary table till the execution of substatement for several reasons:
      - Temporary table can be a MERGE table with base underlying tables,
        so its underlying tables has to be properly open and locked at
        prelocking stage.
      - Temporary table can be a MERGE table and we might be in PREPARE
        phase for a prepared statement. In this case it is important to call
        HA_ATTACH_CHILDREN for all merge children.
        This is necessary because merge children remember "TABLE_SHARE ref type"
        and "TABLE_SHARE def version" in the HA_ATTACH_CHILDREN operation.
        If HA_ATTACH_CHILDREN is not called, these attributes are not set.
        Then, during the first EXECUTE, those attributes need to be updated.
        That would cause statement re-preparing (because changing those
        attributes during EXECUTE is caught by THD::m_reprepare_observers).
        The problem is that since those attributes are not set in merge
        children, another round of PREPARE will not help.
    */
    error= thd->open_temporary_table(tables);

    if (!error && !tables->table)
      error= open_table(thd, tables, ot_ctx);

    thd->pop_internal_handler();
    safe_to_ignore_table= no_such_table_handler.safely_trapped_errors();
  }
  else if (tables->parent_l && (thd->open_options & HA_OPEN_FOR_REPAIR))
  {
    /*
      Also fail silently for underlying tables of a MERGE table if this
      table is opened for CHECK/REPAIR TABLE statement. This is needed
      to provide complete list of problematic underlying tables in
      CHECK/REPAIR TABLE output.
    */
    Repair_mrg_table_error_handler repair_mrg_table_handler;
    thd->push_internal_handler(&repair_mrg_table_handler);

    error= thd->open_temporary_table(tables);

    if (!error && !tables->table)
      error= open_table(thd, tables, ot_ctx);

    thd->pop_internal_handler();
    safe_to_ignore_table= repair_mrg_table_handler.safely_trapped_errors();
  }
  else
  {
    if (tables->parent_l)
    {
      /*
        Even if we are opening table not from the prelocking list we
        still might need to look for a temporary table if this table
        list element corresponds to underlying table of a merge table.
      */
      error= thd->open_temporary_table(tables);
    }

    if (!error && !tables->table)
      error= open_table(thd, tables, ot_ctx);
  }

  if (unlikely(error))
  {
    if (! ot_ctx->can_recover_from_failed_open() && safe_to_ignore_table)
    {
      DBUG_PRINT("info", ("open_table: ignoring table '%s'.'%s'",
                          tables->db.str, tables->alias.str));
      error= FALSE;
    }
    goto end;
  }

  /*
    We can't rely on simple check for TABLE_LIST::view to determine
    that this is a view since during re-execution we might reopen
    ordinary table in place of view and thus have TABLE_LIST::view
    set from repvious execution and TABLE_LIST::table set from
    current.
  */
  if (!tables->table && tables->view)
  {
    /* VIEW placeholder */
    (*counter)--;

    /*
      tables->next_global list consists of two parts:
      1) Query tables and underlying tables of views.
      2) Tables used by all stored routines that this statement invokes on
         execution.
      We need to know where the bound between these two parts is. If we've
      just opened a view, which was the last table in part #1, and it
      has added its base tables after itself, adjust the boundary pointer
      accordingly.
    */
    if (lex->query_tables_own_last == &(tables->next_global) &&
        tables->view->query_tables)
      lex->query_tables_own_last= tables->view->query_tables_last;
    /*
      Let us free memory used by 'sroutines' hash here since we never
      call destructor for this LEX.
    */
    my_hash_free(&tables->view->sroutines);
    goto process_view_routines;
  }

  /*
    Special types of open can succeed but still don't set
    TABLE_LIST::table to anything.
  */
  if (tables->open_strategy && !tables->table)
    goto end;

  /*
    If we are not already in prelocked mode and extended table list is not
    yet built we might have to build the prelocking set for this statement.

    Since currently no prelocking strategy prescribes doing anything for
    tables which are only read, we do below checks only if table is going
    to be changed.
  */
  if (thd->locked_tables_mode <= LTM_LOCK_TABLES &&
      ! has_prelocking_list &&
      (tables->lock_type >= TL_WRITE_ALLOW_WRITE || thd->lex->default_used))
  {
    bool need_prelocking= FALSE;
    TABLE_LIST **save_query_tables_last= lex->query_tables_last;
    /*
      Extend statement's table list and the prelocking set with
      tables and routines according to the current prelocking
      strategy.

      For example, for DML statements we need to add tables and routines
      used by triggers which are going to be invoked for this element of
      table list and also add tables required for handling of foreign keys.
    */
    error= prelocking_strategy->handle_table(thd, lex, tables,
                                             &need_prelocking);

    if (need_prelocking && ! lex->requires_prelocking())
      lex->mark_as_requiring_prelocking(save_query_tables_last);

    if (unlikely(error))
      goto end;
  }

  /* Copy grant information from TABLE_LIST instance to TABLE one. */
  tables->table->grant= tables->grant;

  /* Check and update metadata version of a base table. */
  error= check_and_update_table_version(thd, tables, tables->table->s);

  if (unlikely(error))
    goto end;
  /*
    After opening a MERGE table add the children to the query list of
    tables, so that they are opened too.
    Note that placeholders don't have the handler open.
  */
  /* MERGE tables need to access parent and child TABLE_LISTs. */
  DBUG_ASSERT(tables->table->pos_in_table_list == tables);
  /* Non-MERGE tables ignore this call. */
  if (tables->table->file->extra(HA_EXTRA_ADD_CHILDREN_LIST))
  {
    error= TRUE;
    goto end;
  }

  if (get_use_stat_tables_mode(thd) > NEVER && tables->table)
  {
    TABLE_SHARE *table_share= tables->table->s;
    if (table_share && table_share->table_category == TABLE_CATEGORY_USER &&
        table_share->tmp_table == NO_TMP_TABLE)
    {
      if (table_share->stats_cb.stats_can_be_read ||
	  !alloc_statistics_for_table_share(thd, table_share, FALSE))
      {
        if (table_share->stats_cb.stats_can_be_read)
        {   
          KEY *key_info= table_share->key_info;
          KEY *key_info_end= key_info + table_share->keys;
          KEY *table_key_info= tables->table->key_info;
          for ( ; key_info < key_info_end; key_info++, table_key_info++)
            table_key_info->read_stats= key_info->read_stats;
          Field **field_ptr= table_share->field;
          Field **table_field_ptr= tables->table->field;
          for ( ; *field_ptr; field_ptr++, table_field_ptr++)
            (*table_field_ptr)->read_stats= (*field_ptr)->read_stats;
          tables->table->stats_is_read= table_share->stats_cb.stats_is_read;
        }
      }	
    }
  }

process_view_routines:
  /*
    Again we may need cache all routines used by this view and add
    tables used by them to table list.
  */
  if (tables->view &&
      thd->locked_tables_mode <= LTM_LOCK_TABLES &&
      ! has_prelocking_list)
  {
    bool need_prelocking= FALSE;
    TABLE_LIST **save_query_tables_last= lex->query_tables_last;

    error= prelocking_strategy->handle_view(thd, lex, tables,
                                            &need_prelocking);

    if (need_prelocking && ! lex->requires_prelocking())
      lex->mark_as_requiring_prelocking(save_query_tables_last);

    if (unlikely(error))
      goto end;
  }

end:
  DBUG_RETURN(error);
}


static bool upgrade_lock_if_not_exists(THD *thd,
                                       const DDL_options_st &create_info,
                                       TABLE_LIST *create_table,
                                       ulong lock_wait_timeout)
{
  DBUG_ENTER("upgrade_lock_if_not_exists");

  if (thd->lex->sql_command == SQLCOM_CREATE_TABLE ||
      thd->lex->sql_command == SQLCOM_CREATE_SEQUENCE)
  {
    DEBUG_SYNC(thd,"create_table_before_check_if_exists");
    if (!create_info.or_replace() &&
        ha_table_exists(thd, &create_table->db, &create_table->table_name))
    {
      if (create_info.if_not_exists())
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_TABLE_EXISTS_ERROR,
                            ER_THD(thd, ER_TABLE_EXISTS_ERROR),
                            create_table->table_name.str);
      }
      else
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), create_table->table_name.str);
      DBUG_RETURN(true);
    }
    DBUG_RETURN(thd->mdl_context.upgrade_shared_lock(
                                   create_table->mdl_request.ticket,
                                   MDL_EXCLUSIVE,
                                   lock_wait_timeout));
  }
  DBUG_RETURN(false);
}


/**
  Acquire upgradable (SNW, SNRW) metadata locks on tables used by
  LOCK TABLES or by a DDL statement. Under LOCK TABLES, we can't take
  new locks, so use open_tables_check_upgradable_mdl() instead.

  @param thd               Thread context.
  @param options           DDL options.
  @param tables_start      Start of list of tables on which upgradable locks
                           should be acquired.
  @param tables_end        End of list of tables.
  @param lock_wait_timeout Seconds to wait before timeout.
  @param flags             Bitmap of flags to modify how the tables will be
                           open, see open_table() description for details.

  @retval FALSE  Success.
  @retval TRUE   Failure (e.g. connection was killed) or table existed
	         for a CREATE TABLE.

  @notes
  In case of CREATE TABLE we avoid a wait for tables that are in use
  by first trying to do a meta data lock with timeout == 0.  If we get a
  timeout we will check if table exists (it should) and retry with
  normal timeout if it didn't exists.
  Note that for CREATE TABLE IF EXISTS we only generate a warning
  but still return TRUE (to abort the calling open_table() function).
  On must check THD->is_error() if one wants to distinguish between warning
  and error.
*/

bool
lock_table_names(THD *thd, const DDL_options_st &options,
                 TABLE_LIST *tables_start, TABLE_LIST *tables_end,
                 ulong lock_wait_timeout, uint flags)
{
  MDL_request_list mdl_requests;
  TABLE_LIST *table;
  MDL_request global_request;
  MDL_savepoint mdl_savepoint;
  DBUG_ENTER("lock_table_names");

  DBUG_ASSERT(!thd->locked_tables_mode);

  for (table= tables_start; table && table != tables_end;
       table= table->next_global)
  {
    DBUG_PRINT("info", ("mdl_request.type: %d  open_type: %d",
                        table->mdl_request.type, table->open_type));
    if (table->mdl_request.type < MDL_SHARED_UPGRADABLE ||
        table->mdl_request.type == MDL_SHARED_READ_ONLY ||
        table->open_type == OT_TEMPORARY_ONLY ||
        (table->open_type == OT_TEMPORARY_OR_BASE && is_temporary_table(table)))
    {
      continue;
    }

    /* Write lock on normal tables is not allowed in a read only transaction. */
    if (thd->tx_read_only)
    {
      my_error(ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION, MYF(0));
      DBUG_RETURN(true);
    }

    /* Scoped locks: Take intention exclusive locks on all involved schemas. */
    if (!(flags & MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK))
    {
      MDL_request *schema_request= new (thd->mem_root) MDL_request;
      if (schema_request == NULL)
        DBUG_RETURN(TRUE);
      schema_request->init(MDL_key::SCHEMA, table->db.str, "",
                           MDL_INTENTION_EXCLUSIVE,
                           MDL_TRANSACTION);
      mdl_requests.push_front(schema_request);
    }

    mdl_requests.push_front(&table->mdl_request);
  }

  if (mdl_requests.is_empty())
    DBUG_RETURN(FALSE);

  if (flags & MYSQL_OPEN_SKIP_SCOPED_MDL_LOCK)
  {
    DBUG_RETURN(thd->mdl_context.acquire_locks(&mdl_requests,
                                               lock_wait_timeout) ||
                upgrade_lock_if_not_exists(thd, options, tables_start,
                                           lock_wait_timeout));
  }

  /* Protect this statement against concurrent BACKUP STAGE or FTWRL. */
  if (thd->has_read_only_protection())
    DBUG_RETURN(true);

  global_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_DDL, MDL_STATEMENT);
  mdl_savepoint= thd->mdl_context.mdl_savepoint();

  while (!thd->mdl_context.acquire_locks(&mdl_requests, lock_wait_timeout) &&
         !upgrade_lock_if_not_exists(thd, options, tables_start,
                                     lock_wait_timeout) &&
         !thd->mdl_context.try_acquire_lock(&global_request))
  {
    if (global_request.ticket)
    {
      thd->mdl_backup_ticket= global_request.ticket;
      DBUG_RETURN(false);
    }

    /*
      There is ongoing or pending BACKUP STAGE or FTWRL.
      Wait until it finishes and re-try.
    */
    thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
    if (thd->mdl_context.acquire_lock(&global_request, lock_wait_timeout))
      break;
    thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

    /* Reset tickets for all acquired locks */
    global_request.ticket= 0;
    MDL_request_list::Iterator it(mdl_requests);
    while (auto mdl_request= it++)
      mdl_request->ticket= 0;
  }
  DBUG_RETURN(true);
}


/**
  Check for upgradable (SNW, SNRW) metadata locks on tables to be opened
  for a DDL statement. Under LOCK TABLES, we can't take new locks, so we
  must check if appropriate locks were pre-acquired.

  @param thd           Thread context.
  @param tables_start  Start of list of tables on which upgradable locks
                       should be searched for.
  @param tables_end    End of list of tables.
  @param flags         Bitmap of flags to modify how the tables will be
                       open, see open_table() description for details.

  @retval FALSE  Success.
  @retval TRUE   Failure (e.g. connection was killed)
*/

static bool
open_tables_check_upgradable_mdl(THD *thd, TABLE_LIST *tables_start,
                                 TABLE_LIST *tables_end, uint flags)
{
  TABLE_LIST *table;

  DBUG_ASSERT(thd->locked_tables_mode);

  for (table= tables_start; table && table != tables_end;
       table= table->next_global)
  {
    /*
      Check below needs to be updated if this function starts
      called for SRO locks.
    */
    DBUG_ASSERT(table->mdl_request.type != MDL_SHARED_READ_ONLY);
    if (table->mdl_request.type < MDL_SHARED_UPGRADABLE ||
        table->open_type == OT_TEMPORARY_ONLY ||
        (table->open_type == OT_TEMPORARY_OR_BASE && is_temporary_table(table)))
    {
      continue;
    }

    /*
      We don't need to do anything about the found TABLE instance as it
      will be handled later in open_tables(), we only need to check that
      an upgradable lock is already acquired. When we enter LOCK TABLES
      mode, SNRW locks are acquired before all other locks. So if under
      LOCK TABLES we find that there is TABLE instance with upgradeable
      lock, all other instances of TABLE for the same table will have the
      same ticket.

      Note that this works OK even for CREATE TABLE statements which
      request X type of metadata lock. This is because under LOCK TABLES
      such statements don't create the table but only check if it exists
      or, in most complex case, only insert into it.
      Thus SNRW lock should be enough.

      Note that find_table_for_mdl_upgrade() will report an error if
      no suitable ticket is found.
    */
    if (!find_table_for_mdl_upgrade(thd, table->db.str, table->table_name.str,
                                    NULL))
      return TRUE;
  }

  return FALSE;
}


/**
  Open all tables in list

  @param[in]     thd      Thread context.
  @param[in]     options  DDL options.
  @param[in,out] start    List of tables to be open (it can be adjusted for
                          statement that uses tables only implicitly, e.g.
                          for "SELECT f1()").
  @param[out]    counter  Number of tables which were open.
  @param[in]     flags    Bitmap of flags to modify how the tables will be
                          open, see open_table() description for details.
  @param[in]     prelocking_strategy  Strategy which specifies how prelocking
                                      algorithm should work for this statement.

  @note
    Unless we are already in prelocked mode and prelocking strategy prescribes
    so this function will also precache all SP/SFs explicitly or implicitly
    (via views and triggers) used by the query and add tables needed for their
    execution to table list. Statement that uses SFs, invokes triggers or
    requires foreign key checks will be marked as requiring prelocking.
    Prelocked mode will be enabled for such query during lock_tables() call.

    If query for which we are opening tables is already marked as requiring
    prelocking it won't do such precaching and will simply reuse table list
    which is already built.

  @retval  FALSE  Success.
  @retval  TRUE   Error, reported.
*/

bool open_tables(THD *thd, const DDL_options_st &options,
                 TABLE_LIST **start, uint *counter, uint flags,
                 Prelocking_strategy *prelocking_strategy)
{
  /*
    We use pointers to "next_global" member in the last processed
    TABLE_LIST element and to the "next" member in the last processed
    Sroutine_hash_entry element as iterators over, correspondingly,
    the table list and stored routines list which stay valid and allow
    to continue iteration when new elements are added to the tail of
    the lists.
  */
  TABLE_LIST **table_to_open;
  Sroutine_hash_entry **sroutine_to_open;
  TABLE_LIST *tables;
  Open_table_context ot_ctx(thd, flags);
  bool error= FALSE;
  bool some_routine_modifies_data= FALSE;
  bool has_prelocking_list;
  DBUG_ENTER("open_tables");

  /* Data access in XA transaction is only allowed when it is active. */
  if (*start && thd->transaction.xid_state.check_has_uncommitted_xa())
    DBUG_RETURN(true);

  thd->current_tablenr= 0;
restart:
  /*
    Close HANDLER tables which are marked for flush or against which there
    are pending exclusive metadata locks. This is needed both in order to
    avoid deadlocks and to have a point during statement execution at
    which such HANDLERs are closed even if they don't create problems for
    the current session (i.e. to avoid having a DDL blocked by HANDLERs
    opened for a long time).
  */
  if (thd->handler_tables_hash.records)
    mysql_ha_flush(thd);

  has_prelocking_list= thd->lex->requires_prelocking();
  table_to_open= start;
  sroutine_to_open= (Sroutine_hash_entry**) &thd->lex->sroutines_list.first;
  *counter= 0;
  THD_STAGE_INFO(thd, stage_opening_tables);

  /*
    If we are executing LOCK TABLES statement or a DDL statement
    (in non-LOCK TABLES mode) we might have to acquire upgradable
    semi-exclusive metadata locks (SNW or SNRW) on some of the
    tables to be opened.
    When executing CREATE TABLE .. If NOT EXISTS .. SELECT, the
    table may not yet exist, in which case we acquire an exclusive
    lock.
    We acquire all such locks at once here as doing this in one
    by one fashion may lead to deadlocks or starvation. Later when
    we will be opening corresponding table pre-acquired metadata
    lock will be reused (thanks to the fact that in recursive case
    metadata locks are acquired without waiting).
  */
  if (! (flags & (MYSQL_OPEN_HAS_MDL_LOCK |
                  MYSQL_OPEN_FORCE_SHARED_MDL |
                  MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL)))
  {
    if (thd->locked_tables_mode)
    {
      /*
        Under LOCK TABLES, we can't acquire new locks, so we instead
        need to check if appropriate locks were pre-acquired.
      */
      if (open_tables_check_upgradable_mdl(thd, *start,
                                           thd->lex->first_not_own_table(),
                                           flags))
      {
        error= TRUE;
        goto error;
      }
    }
    else
    {
      TABLE_LIST *table;
      if (lock_table_names(thd, options, *start,
                           thd->lex->first_not_own_table(),
                           ot_ctx.get_timeout(), flags))
      {
        error= TRUE;
        goto error;
      }
      for (table= *start; table && table != thd->lex->first_not_own_table();
           table= table->next_global)
      {
        if (table->mdl_request.type >= MDL_SHARED_UPGRADABLE)
          table->mdl_request.ticket= NULL;
      }
    }
  }

  /*
    Perform steps of prelocking algorithm until there are unprocessed
    elements in prelocking list/set.
  */
  while (*table_to_open  ||
         (thd->locked_tables_mode <= LTM_LOCK_TABLES &&
          *sroutine_to_open))
  {
    /*
      For every table in the list of tables to open, try to find or open
      a table.
    */
    for (tables= *table_to_open; tables;
         table_to_open= &tables->next_global, tables= tables->next_global)
    {
      error= open_and_process_table(thd, thd->lex, tables, counter,
                                    flags, prelocking_strategy,
                                    has_prelocking_list, &ot_ctx);

      if (unlikely(error))
      {
        if (ot_ctx.can_recover_from_failed_open())
        {
          /*
            We have met exclusive metadata lock or old version of table.
            Now we have to close all tables and release metadata locks.
            We also have to throw away set of prelocked tables (and thus
            close tables from this set that were open by now) since it
            is possible that one of tables which determined its content
            was changed.

            Instead of implementing complex/non-robust logic mentioned
            above we simply close and then reopen all tables.

            We have to save pointer to table list element for table which we
            have failed to open since closing tables can trigger removal of
            elements from the table list (if MERGE tables are involved),
          */
          close_tables_for_reopen(thd, start, ot_ctx.start_of_statement_svp());

          /*
            Here we rely on the fact that 'tables' still points to the valid
            TABLE_LIST element. Altough currently this assumption is valid
            it may change in future.
          */
          if (ot_ctx.recover_from_failed_open())
            goto error;

          /* Re-open temporary tables after close_tables_for_reopen(). */
          if (thd->open_temporary_tables(*start))
            goto error;

          error= FALSE;
          goto restart;
        }
        goto error;
      }

      DEBUG_SYNC(thd, "open_tables_after_open_and_process_table");
    }

    /*
      If we are not already in prelocked mode and extended table list is
      not yet built for our statement we need to cache routines it uses
      and build the prelocking list for it.
      If we are not in prelocked mode but have built the extended table
      list, we still need to call open_and_process_routine() to take
      MDL locks on the routines.
    */
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
    {
      /*
        Process elements of the prelocking set which are present there
        since parsing stage or were added to it by invocations of
        Prelocking_strategy methods in the above loop over tables.

        For example, if element is a routine, cache it and then,
        if prelocking strategy prescribes so, add tables it uses to the
        table list and routines it might invoke to the prelocking set.
      */
      for (Sroutine_hash_entry *rt= *sroutine_to_open; rt;
           sroutine_to_open= &rt->next, rt= rt->next)
      {
        bool need_prelocking= false;
        bool routine_modifies_data;
        TABLE_LIST **save_query_tables_last= thd->lex->query_tables_last;

        error= open_and_process_routine(thd, thd->lex, rt, prelocking_strategy,
                                        has_prelocking_list, &ot_ctx,
                                        &need_prelocking,
                                        &routine_modifies_data);

        // Remember if any of SF modifies data.
        some_routine_modifies_data|= routine_modifies_data;

        if (need_prelocking && ! thd->lex->requires_prelocking())
          thd->lex->mark_as_requiring_prelocking(save_query_tables_last);

        if (need_prelocking && ! *start)
          *start= thd->lex->query_tables;

        if (unlikely(error))
        {
          if (ot_ctx.can_recover_from_failed_open())
          {
            close_tables_for_reopen(thd, start,
                                    ot_ctx.start_of_statement_svp());
            if (ot_ctx.recover_from_failed_open())
              goto error;

            /* Re-open temporary tables after close_tables_for_reopen(). */
            if (thd->open_temporary_tables(*start))
              goto error;

            error= FALSE;
            goto restart;
          }
          /*
            Serious error during reading stored routines from mysql.proc table.
            Something is wrong with the table or its contents, and an error has
            been emitted; we must abort.
          */
          goto error;
        }
      }
    }
  }

  /*
    After successful open of all tables, including MERGE parents and
    children, attach the children to their parents. At end of statement,
    the children are detached. Attaching and detaching are always done,
    even under LOCK TABLES.

    We also convert all TL_WRITE_DEFAULT and TL_READ_DEFAULT locks to
    appropriate "real" lock types to be used for locking and to be passed
    to storage engine.

    And start wsrep TOI if needed.
  */
  for (tables= *start; tables; tables= tables->next_global)
  {
    TABLE *tbl= tables->table;

    if (!tbl)
      continue;

    /* Schema tables may not have a TABLE object here. */
    if (tbl->file->ha_table_flags() & HA_CAN_MULTISTEP_MERGE)
    {
      /* MERGE tables need to access parent and child TABLE_LISTs. */
      DBUG_ASSERT(tbl->pos_in_table_list == tables);
      if (tbl->file->extra(HA_EXTRA_ATTACH_CHILDREN))
      {
        error= TRUE;
        goto error;
      }
    }

    /* Set appropriate TABLE::lock_type. */
    if (tbl && tables->lock_type != TL_UNLOCK && !thd->locked_tables_mode)
    {
      if (tables->lock_type == TL_WRITE_DEFAULT)
        tbl->reginfo.lock_type= thd->update_lock_default;
      else if (tables->lock_type == TL_READ_DEFAULT)
          tbl->reginfo.lock_type=
            read_lock_type_for_table(thd, thd->lex, tables,
                                     some_routine_modifies_data);
      else
        tbl->reginfo.lock_type= tables->lock_type;
    }
  }

#ifdef WITH_WSREP
  if (WSREP_ON                                         &&
      wsrep_replicate_myisam                           &&
      (*start)                                         &&
      (*start)->table                                  &&
      (*start)->table->file->ht == myisam_hton         &&
      wsrep_thd_is_local(thd)                          &&
      !is_stat_table(&(*start)->db, &(*start)->alias)  &&
      thd->get_command() != COM_STMT_PREPARE           &&
      ((thd->lex->sql_command == SQLCOM_INSERT         ||
        thd->lex->sql_command == SQLCOM_INSERT_SELECT  ||
        thd->lex->sql_command == SQLCOM_REPLACE        ||
        thd->lex->sql_command == SQLCOM_REPLACE_SELECT ||
        thd->lex->sql_command == SQLCOM_UPDATE         ||
        thd->lex->sql_command == SQLCOM_UPDATE_MULTI   ||
        thd->lex->sql_command == SQLCOM_LOAD           ||
        thd->lex->sql_command == SQLCOM_DELETE)))
  {
      wsrep_before_rollback(thd, true);
      wsrep_after_rollback(thd, true);
      wsrep_after_statement(thd);
      WSREP_TO_ISOLATION_BEGIN(NULL, NULL, (*start));
  }
#endif /* WITH_WSREP */

error:
#ifdef WITH_WSREP
wsrep_error_label:
#endif
  THD_STAGE_INFO(thd, stage_after_opening_tables);
  thd_proc_info(thd, 0);

  if (unlikely(error) && *table_to_open)
  {
    (*table_to_open)->table= NULL;
  }
  DBUG_PRINT("open_tables", ("returning: %d", (int) error));
  DBUG_RETURN(error);
}


/**
  Defines how prelocking algorithm for DML statements should handle routines:
  - For CALL statements we do unrolling (i.e. open and lock tables for each
    sub-statement individually). So for such statements prelocking is enabled
    only if stored functions are used in parameter list and only for period
    during which we calculate values of parameters. Thus in this strategy we
    ignore procedure which is directly called by such statement and extend
    the prelocking set only with tables/functions used by SF called from the
    parameter list.
  - For any other statement any routine which is directly or indirectly called
    by statement is going to be executed in prelocked mode. So in this case we
    simply add all tables and routines used by it to the prelocking set.

  @param[in]  thd              Thread context.
  @param[in]  prelocking_ctx   Prelocking context of the statement.
  @param[in]  rt               Prelocking set element describing routine.
  @param[in]  sp               Routine body.
  @param[out] need_prelocking  Set to TRUE if method detects that prelocking
                               required, not changed otherwise.

  @retval FALSE  Success.
  @retval TRUE   Failure (OOM).
*/

bool DML_prelocking_strategy::
handle_routine(THD *thd, Query_tables_list *prelocking_ctx,
               Sroutine_hash_entry *rt, sp_head *sp, bool *need_prelocking)
{
  /*
    We assume that for any "CALL proc(...)" statement sroutines_list will
    have 'proc' as first element (it may have several, consider e.g.
    "proc(sp_func(...)))". This property is currently guaranted by the
    parser.
  */

  if (rt != (Sroutine_hash_entry*)prelocking_ctx->sroutines_list.first ||
      rt->mdl_request.key.mdl_namespace() != MDL_key::PROCEDURE)
  {
    *need_prelocking= TRUE;
    sp_update_stmt_used_routines(thd, prelocking_ctx, &sp->m_sroutines,
                                 rt->belong_to_view);
    (void)sp->add_used_tables_to_table_list(thd,
                                            &prelocking_ctx->query_tables_last,
                                            rt->belong_to_view);
  }
  sp->propagate_attributes(prelocking_ctx);
  return FALSE;
}


/*
  @note this can be changed to use a hash, instead of scanning the linked
  list, if the performance of this function will ever become an issue
*/
bool table_already_fk_prelocked(TABLE_LIST *tl, LEX_CSTRING *db,
                                LEX_CSTRING *table, thr_lock_type lock_type)
{
  for (; tl; tl= tl->next_global )
  {
    if (tl->lock_type >= lock_type &&
        tl->prelocking_placeholder == TABLE_LIST::PRELOCK_FK &&
        strcmp(tl->db.str, db->str) == 0 &&
        strcmp(tl->table_name.str, table->str) == 0)
      return true;
  }
  return false;
}


static bool internal_table_exists(TABLE_LIST *global_list,
                                  const char *table_name)
{
  do
  {
    if (global_list->table_name.str == table_name)
      return 1;
  } while ((global_list= global_list->next_global));
  return 0;
}


static bool
add_internal_tables(THD *thd, Query_tables_list *prelocking_ctx,
                    TABLE_LIST *tables)
{
  TABLE_LIST *global_table_list= prelocking_ctx->query_tables;

  do
  {
    /*
      Skip table if already in the list. Can happen with prepared statements
    */
    if (tables->next_local &&
        internal_table_exists(global_table_list, tables->table_name.str))
      continue;

    TABLE_LIST *tl= (TABLE_LIST *) thd->alloc(sizeof(TABLE_LIST));
    if (!tl)
      return TRUE;
    tl->init_one_table_for_prelocking(&tables->db,
                                      &tables->table_name,
                                      NULL, tables->lock_type,
                                      TABLE_LIST::PRELOCK_NONE,
                                      0, 0,
                                      &prelocking_ctx->query_tables_last);
    /*
      Store link to the new table_list that will be used by open so that
      Item_func_nextval() can find it
    */
    tables->next_local= tl;
  } while ((tables= tables->next_global));
  return FALSE;
}



/**
  Defines how prelocking algorithm for DML statements should handle table list
  elements:
  - If table has triggers we should add all tables and routines
    used by them to the prelocking set.

  We do not need to acquire metadata locks on trigger names
  in DML statements, since all DDL statements
  that change trigger metadata always lock their
  subject tables.

  @param[in]  thd              Thread context.
  @param[in]  prelocking_ctx   Prelocking context of the statement.
  @param[in]  table_list       Table list element for table.
  @param[in]  sp               Routine body.
  @param[out] need_prelocking  Set to TRUE if method detects that prelocking
                               required, not changed otherwise.

  @retval FALSE  Success.
  @retval TRUE   Failure (OOM).
*/

bool DML_prelocking_strategy::
handle_table(THD *thd, Query_tables_list *prelocking_ctx,
             TABLE_LIST *table_list, bool *need_prelocking)
{
  TABLE *table= table_list->table;
  /* We rely on a caller to check that table is going to be changed. */
  DBUG_ASSERT(table_list->lock_type >= TL_WRITE_ALLOW_WRITE ||
              thd->lex->default_used);

  if (table_list->trg_event_map)
  {
    if (table->triggers)
    {
      *need_prelocking= TRUE;

      if (table->triggers->
          add_tables_and_routines_for_triggers(thd, prelocking_ctx, table_list))
        return TRUE;
    }

    if (table->file->referenced_by_foreign_key())
    {
      List <FOREIGN_KEY_INFO> fk_list;
      List_iterator<FOREIGN_KEY_INFO> fk_list_it(fk_list);
      FOREIGN_KEY_INFO *fk;
      Query_arena *arena, backup;

      arena= thd->activate_stmt_arena_if_needed(&backup);

      table->file->get_parent_foreign_key_list(thd, &fk_list);
      if (unlikely(thd->is_error()))
      {
        if (arena)
          thd->restore_active_arena(arena, &backup);
        return TRUE;
      }

      *need_prelocking= TRUE;

      while ((fk= fk_list_it++))
      {
        // FK_OPTION_RESTRICT and FK_OPTION_NO_ACTION only need read access
        uint8 op= table_list->trg_event_map;
        thr_lock_type lock_type;

        if ((op & (1 << TRG_EVENT_DELETE) && fk_modifies_child(fk->delete_method))
         || (op & (1 << TRG_EVENT_UPDATE) && fk_modifies_child(fk->update_method)))
          lock_type= TL_WRITE_ALLOW_WRITE;
        else
          lock_type= TL_READ;

        if (table_already_fk_prelocked(prelocking_ctx->query_tables,
                                       fk->foreign_db, fk->foreign_table,
                                       lock_type))
          continue;

        TABLE_LIST *tl= (TABLE_LIST *) thd->alloc(sizeof(TABLE_LIST));
        tl->init_one_table_for_prelocking(fk->foreign_db,
                                          fk->foreign_table,
                                          NULL, lock_type,
                                          TABLE_LIST::PRELOCK_FK,
                                          table_list->belong_to_view, op,
                                          &prelocking_ctx->query_tables_last);
      }
      if (arena)
        thd->restore_active_arena(arena, &backup);
    }
  }

  /* Open any tables used by DEFAULT (like sequence tables) */
  if (table->internal_tables &&
      ((sql_command_flags[thd->lex->sql_command] & CF_INSERTS_DATA) ||
       thd->lex->default_used))
  {
    Query_arena *arena, backup;
    bool error;
    arena= thd->activate_stmt_arena_if_needed(&backup);
    error= add_internal_tables(thd, prelocking_ctx,
                               table->internal_tables);
    if (arena)
      thd->restore_active_arena(arena, &backup);
    if (unlikely(error))
    {
      *need_prelocking= TRUE;
      return TRUE;
    }
  }
  return FALSE;
}


/**
  Open all tables used by DEFAULT functions.

  This is different from normal open_and_lock_tables() as we may
  already have other tables opened and locked and we have to merge the
  new table with the old ones.
*/

bool open_and_lock_internal_tables(TABLE *table, bool lock_table)
{
  THD *thd= table->in_use;
  TABLE_LIST *tl;
  MYSQL_LOCK *save_lock,*new_lock;
  DBUG_ENTER("open_internal_tables");

  /* remove pointer to old select_lex which is already destroyed */
  for (tl= table->internal_tables ; tl ; tl= tl->next_global)
    tl->select_lex= 0;

  uint counter;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  TABLE_LIST *tmp= table->internal_tables;
  DML_prelocking_strategy prelocking_strategy;

  if (open_tables(thd, thd->lex->create_info, &tmp, &counter, 0,
                  &prelocking_strategy))
    goto err;

  if (lock_table)
  {
    save_lock= thd->lock;
    thd->lock= 0;
    if (lock_tables(thd, table->internal_tables, counter,
                    MYSQL_LOCK_USE_MALLOC))
      goto err;

    if (!(new_lock= mysql_lock_merge(save_lock, thd->lock)))
    {
      thd->lock= save_lock;
      mysql_unlock_tables(thd, save_lock, 1);
      /* We don't have to close tables as caller will do that */
      goto err;
    }
    thd->lock= new_lock;
  }
  DBUG_RETURN(0);

err:
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  DBUG_RETURN(1);
}


/**
  Defines how prelocking algorithm for DML statements should handle view -
  all view routines should be added to the prelocking set.

  @param[in]  thd              Thread context.
  @param[in]  prelocking_ctx   Prelocking context of the statement.
  @param[in]  table_list       Table list element for view.
  @param[in]  sp               Routine body.
  @param[out] need_prelocking  Set to TRUE if method detects that prelocking
                               required, not changed otherwise.

  @retval FALSE  Success.
  @retval TRUE   Failure (OOM).
*/

bool DML_prelocking_strategy::
handle_view(THD *thd, Query_tables_list *prelocking_ctx,
            TABLE_LIST *table_list, bool *need_prelocking)
{
  if (table_list->view->uses_stored_routines())
  {
    *need_prelocking= TRUE;

    sp_update_stmt_used_routines(thd, prelocking_ctx,
                                 &table_list->view->sroutines_list,
                                 table_list->top_table());
  }

  /*
    If a trigger was defined on one of the associated tables then assign the
    'trg_event_map' value of the view to the next table in table_list. When a
    Stored function is invoked, all the associated tables including the tables
    associated with the trigger are prelocked.
  */
  if (table_list->trg_event_map && table_list->next_global)
    table_list->next_global->trg_event_map= table_list->trg_event_map;
  return FALSE;
}


/**
  Defines how prelocking algorithm for LOCK TABLES statement should handle
  table list elements.

  @param[in]  thd              Thread context.
  @param[in]  prelocking_ctx   Prelocking context of the statement.
  @param[in]  table_list       Table list element for table.
  @param[in]  sp               Routine body.
  @param[out] need_prelocking  Set to TRUE if method detects that prelocking
                               required, not changed otherwise.

  @retval FALSE  Success.
  @retval TRUE   Failure (OOM).
*/

bool Lock_tables_prelocking_strategy::
handle_table(THD *thd, Query_tables_list *prelocking_ctx,
             TABLE_LIST *table_list, bool *need_prelocking)
{
  if (DML_prelocking_strategy::handle_table(thd, prelocking_ctx, table_list,
                                            need_prelocking))
    return TRUE;

  /* We rely on a caller to check that table is going to be changed. */
  DBUG_ASSERT(table_list->lock_type >= TL_WRITE_ALLOW_WRITE);

  return FALSE;
}


/**
  Defines how prelocking algorithm for ALTER TABLE statement should handle
  routines - do nothing as this statement is not supposed to call routines.

  We still can end up in this method when someone tries
  to define a foreign key referencing a view, and not just
  a simple view, but one that uses stored routines.
*/

bool Alter_table_prelocking_strategy::
handle_routine(THD *thd, Query_tables_list *prelocking_ctx,
               Sroutine_hash_entry *rt, sp_head *sp, bool *need_prelocking)
{
  return FALSE;
}


/**
  Defines how prelocking algorithm for ALTER TABLE statement should handle
  table list elements.

  Unlike in DML, we do not process triggers here.

  @param[in]  thd              Thread context.
  @param[in]  prelocking_ctx   Prelocking context of the statement.
  @param[in]  table_list       Table list element for table.
  @param[in]  sp               Routine body.
  @param[out] need_prelocking  Set to TRUE if method detects that prelocking
                               required, not changed otherwise.


  @retval FALSE  Success.
  @retval TRUE   Failure (OOM).
*/

bool Alter_table_prelocking_strategy::
handle_table(THD *thd, Query_tables_list *prelocking_ctx,
             TABLE_LIST *table_list, bool *need_prelocking)
{
  return FALSE;
}


/**
  Defines how prelocking algorithm for ALTER TABLE statement
  should handle view - do nothing. We don't need to add view
  routines to the prelocking set in this case as view is not going
  to be materialized.
*/

bool Alter_table_prelocking_strategy::
handle_view(THD *thd, Query_tables_list *prelocking_ctx,
            TABLE_LIST *table_list, bool *need_prelocking)
{
  return FALSE;
}


/**
  Check that lock is ok for tables; Call start stmt if ok

  @param thd             Thread handle.
  @param prelocking_ctx  Prelocking context.
  @param table_list      Table list element for table to be checked.

  @retval FALSE - Ok.
  @retval TRUE  - Error.
*/

static bool check_lock_and_start_stmt(THD *thd,
                                      Query_tables_list *prelocking_ctx,
                                      TABLE_LIST *table_list)
{
  int error;
  thr_lock_type lock_type;
  DBUG_ENTER("check_lock_and_start_stmt");

  /*
    Prelocking placeholder is not set for TABLE_LIST that
    are directly used by TOP level statement.
  */
  DBUG_ASSERT(table_list->prelocking_placeholder == TABLE_LIST::PRELOCK_NONE);

  /*
    TL_WRITE_DEFAULT and TL_READ_DEFAULT are supposed to be parser only
    types of locks so they should be converted to appropriate other types
    to be passed to storage engine. The exact lock type passed to the
    engine is important as, for example, InnoDB uses it to determine
    what kind of row locks should be acquired when executing statement
    in prelocked mode or under LOCK TABLES with @@innodb_table_locks = 0.

    Last argument routine_modifies_data for read_lock_type_for_table()
    is ignored, as prelocking placeholder will never be set here.
  */
  if (table_list->lock_type == TL_WRITE_DEFAULT)
    lock_type= thd->update_lock_default;
  else if (table_list->lock_type == TL_READ_DEFAULT)
    lock_type= read_lock_type_for_table(thd, prelocking_ctx, table_list, true);
  else
    lock_type= table_list->lock_type;

  if ((int) lock_type >= (int) TL_WRITE_ALLOW_WRITE &&
      (int) table_list->table->reginfo.lock_type < (int) TL_WRITE_ALLOW_WRITE)
  {
    my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0),
             table_list->table->alias.c_ptr());
    DBUG_RETURN(1);
  }
  if (unlikely((error= table_list->table->file->start_stmt(thd, lock_type))))
  {
    table_list->table->file->print_error(error, MYF(0));
    DBUG_RETURN(1);
  }

  /*
    Record in transaction state tracking
  */
  TRANSACT_TRACKER(add_trx_state(thd, lock_type,
                                 table_list->table->file->has_transactions()));

  DBUG_RETURN(0);
}


/**
  @brief Open and lock one table

  @param[in]    thd             thread handle
  @param[in]    table_l         table to open is first table in this list
  @param[in]    lock_type       lock to use for table
  @param[in]    flags           options to be used while opening and locking
                                table (see open_table(), mysql_lock_tables())
  @param[in]    prelocking_strategy  Strategy which specifies how prelocking
                                     algorithm should work for this statement.

  @return       table
    @retval     != NULL         OK, opened table returned
    @retval     NULL            Error

  @note
    If ok, the following are also set:
      table_list->lock_type 	lock_type
      table_list->table		table

  @note
    If table_l is a list, not a single table, the list is temporarily
    broken.

  @detail
    This function is meant as a replacement for open_ltable() when
    MERGE tables can be opened. open_ltable() cannot open MERGE tables.

    There may be more differences between open_n_lock_single_table() and
    open_ltable(). One known difference is that open_ltable() does
    neither call thd->decide_logging_format() nor handle some other logging
    and locking issues because it does not call lock_tables().
*/

TABLE *open_n_lock_single_table(THD *thd, TABLE_LIST *table_l,
                                thr_lock_type lock_type, uint flags,
                                Prelocking_strategy *prelocking_strategy)
{
  TABLE_LIST *save_next_global;
  DBUG_ENTER("open_n_lock_single_table");

  /* Remember old 'next' pointer. */
  save_next_global= table_l->next_global;
  /* Break list. */
  table_l->next_global= NULL;

  /* Set requested lock type. */
  table_l->lock_type= lock_type;
  /* Allow to open real tables only. */
  table_l->required_type= TABLE_TYPE_NORMAL;

  /* Open the table. */
  if (open_and_lock_tables(thd, table_l, FALSE, flags,
                           prelocking_strategy))
    table_l->table= NULL; /* Just to be sure. */

  /* Restore list. */
  table_l->next_global= save_next_global;

  DBUG_RETURN(table_l->table);
}


/*
  Open and lock one table

  SYNOPSIS
    open_ltable()
    thd			Thread handler
    table_list		Table to open is first table in this list
    lock_type		Lock to use for open
    lock_flags          Flags passed to mysql_lock_table

  NOTE
    This function doesn't do anything like SP/SF/views/triggers analysis done 
    in open_table()/lock_tables(). It is intended for opening of only one
    concrete table. And used only in special contexts.

  RETURN VALUES
    table		Opened table
    0			Error
  
    If ok, the following are also set:
      table_list->lock_type 	lock_type
      table_list->table		table
*/

TABLE *open_ltable(THD *thd, TABLE_LIST *table_list, thr_lock_type lock_type,
                   uint lock_flags)
{
  TABLE *table;
  Open_table_context ot_ctx(thd, lock_flags);
  bool error;
  DBUG_ENTER("open_ltable");

  /* Ignore temporary tables as they have already been opened. */
  if (table_list->table)
    DBUG_RETURN(table_list->table);

  /* should not be used in a prelocked_mode context, see NOTE above */
  DBUG_ASSERT(thd->locked_tables_mode < LTM_PRELOCKED);

  THD_STAGE_INFO(thd, stage_opening_tables);
  thd->current_tablenr= 0;
  /* open_ltable can be used only for BASIC TABLEs */
  table_list->required_type= TABLE_TYPE_NORMAL;

  /* This function can't properly handle requests for such metadata locks. */
  DBUG_ASSERT(table_list->mdl_request.type < MDL_SHARED_UPGRADABLE);

  while ((error= open_table(thd, table_list, &ot_ctx)) &&
         ot_ctx.can_recover_from_failed_open())
  {
    /*
      Even though we have failed to open table we still need to
      call release_transactional_locks() to release metadata locks which
      might have been acquired successfully.
    */
    thd->mdl_context.rollback_to_savepoint(ot_ctx.start_of_statement_svp());
    table_list->mdl_request.ticket= 0;
    if (ot_ctx.recover_from_failed_open())
      break;
  }

  if (likely(!error))
  {
    /*
      We can't have a view or some special "open_strategy" in this function
      so there should be a TABLE instance.
    */
    DBUG_ASSERT(table_list->table);
    table= table_list->table;
    if (table->file->ha_table_flags() & HA_CAN_MULTISTEP_MERGE)
    {
      /* A MERGE table must not come here. */
      /* purecov: begin tested */
      my_error(ER_WRONG_OBJECT, MYF(0), table->s->db.str,
               table->s->table_name.str, "BASE TABLE");
      table= 0;
      goto end;
      /* purecov: end */
    }

    table_list->lock_type= lock_type;
    table->grant= table_list->grant;
    if (thd->locked_tables_mode)
    {
      if (check_lock_and_start_stmt(thd, thd->lex, table_list))
	table= 0;
    }
    else
    {
      DBUG_ASSERT(thd->lock == 0);	// You must lock everything at once
      if ((table->reginfo.lock_type= lock_type) != TL_UNLOCK)
	if (! (thd->lock= mysql_lock_tables(thd, &table_list->table, 1,
                                            lock_flags)))
        {
          table= 0;
        }
    }
  }
  else
    table= 0;

end:
  if (table == NULL)
  {
    if (!thd->in_sub_stmt)
      trans_rollback_stmt(thd);
    close_thread_tables(thd);
  }
  THD_STAGE_INFO(thd, stage_after_opening_tables);

  thd_proc_info(thd, 0);
  DBUG_RETURN(table);
}


/**
  Open all tables in list, locks them and optionally process derived tables.

  @param thd		      Thread context.
  @param options              DDL options.
  @param tables	              List of tables for open and locking.
  @param derived              Whether to handle derived tables.
  @param flags                Bitmap of options to be used to open and lock
                              tables (see open_tables() and mysql_lock_tables()
                              for details).
  @param prelocking_strategy  Strategy which specifies how prelocking algorithm
                              should work for this statement.

  @note
    The thr_lock locks will automatically be freed by
    close_thread_tables().

  @retval FALSE  OK.
  @retval TRUE   Error
*/

bool open_and_lock_tables(THD *thd, const DDL_options_st &options,
                          TABLE_LIST *tables,
                          bool derived, uint flags,
                          Prelocking_strategy *prelocking_strategy)
{
  uint counter;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  DBUG_ENTER("open_and_lock_tables");
  DBUG_PRINT("enter", ("derived handling: %d", derived));

  if (open_tables(thd, options, &tables, &counter, flags, prelocking_strategy))
    goto err;

  DBUG_EXECUTE_IF("sleep_open_and_lock_after_open", {
                  const char *old_proc_info= thd->proc_info;
                  thd->proc_info= "DBUG sleep";
                  my_sleep(6000000);
                  thd->proc_info= old_proc_info;});

  if (lock_tables(thd, tables, counter, flags))
    goto err;

  (void) read_statistics_for_tables_if_needed(thd, tables);
  
  if (derived)
  {
    if (mysql_handle_derived(thd->lex, DT_INIT))
      goto err;
    if (thd->prepare_derived_at_open &&
        (mysql_handle_derived(thd->lex, DT_PREPARE)))
      goto err;
  }

  DBUG_RETURN(FALSE);
err:
  if (! thd->in_sub_stmt)
    trans_rollback_stmt(thd);  /* Necessary if derived handling failed. */
  close_thread_tables(thd);
  /* Don't keep locks for a failed statement. */
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  DBUG_RETURN(TRUE);
}


/*
  Open all tables in list and process derived tables

  SYNOPSIS
    open_normal_and_derived_tables
    thd		- thread handler
    tables	- list of tables for open
    flags       - bitmap of flags to modify how the tables will be open:
                  MYSQL_LOCK_IGNORE_FLUSH - open table even if someone has
                  done a flush on it.
    dt_phases   - set of flags to pass to the mysql_handle_derived

  RETURN
    FALSE - ok
    TRUE  - error

  NOTE 
    This is to be used on prepare stage when you don't read any
    data from the tables.
*/

bool open_normal_and_derived_tables(THD *thd, TABLE_LIST *tables, uint flags,
                                    uint dt_phases)
{
  DML_prelocking_strategy prelocking_strategy;
  uint counter;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  DBUG_ENTER("open_normal_and_derived_tables");
  DBUG_ASSERT(!thd->fill_derived_tables());
  if (open_tables(thd, &tables, &counter, flags, &prelocking_strategy) ||
      mysql_handle_derived(thd->lex, dt_phases))
    goto end;

  DBUG_RETURN(0);
end:
  /*
    No need to commit/rollback the statement transaction: it's
    either not started or we're filling in an INFORMATION_SCHEMA
    table on the fly, and thus mustn't manipulate with the
    transaction of the enclosing statement.
  */
  DBUG_ASSERT(thd->transaction.stmt.is_empty() ||
              (thd->state_flags & Open_tables_state::BACKUPS_AVAIL));
  close_thread_tables(thd);
  /* Don't keep locks for a failed statement. */
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

  DBUG_RETURN(TRUE); /* purecov: inspected */
}


/**
  Open a table to read its structure, e.g. for:
  - SHOW FIELDS
  - delayed SP variable data type definition: DECLARE a t1.a%TYPE

  The flag MYSQL_OPEN_GET_NEW_TABLE is passed to make %TYPE work
  in stored functions, as during a stored function call
  (e.g. in a SELECT query) the tables referenced in %TYPE can already be locked,
  and attempt to open it again would return an error in open_table().

  The flag MYSQL_OPEN_GET_NEW_TABLE is not really needed for
  SHOW FIELDS or for a "CALL sp()" statement, but it's not harmful,
  so let's pass it unconditionally.
*/

bool open_tables_only_view_structure(THD *thd, TABLE_LIST *table_list,
                                     bool can_deadlock)
{
  DBUG_ENTER("open_tables_only_view_structure");
  /*
    Let us set fake sql_command so views won't try to merge
    themselves into main statement. If we don't do this,
    SELECT * from information_schema.xxxx will cause problems.
    SQLCOM_SHOW_FIELDS is used because it satisfies
    'LEX::only_view_structure()'.
  */
  enum_sql_command save_sql_command= thd->lex->sql_command;
  thd->lex->sql_command= SQLCOM_SHOW_FIELDS;
  bool rc= (thd->open_temporary_tables(table_list) ||
           open_normal_and_derived_tables(thd, table_list,
                                          (MYSQL_OPEN_IGNORE_FLUSH |
                                           MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL |
                                           MYSQL_OPEN_GET_NEW_TABLE |
                                           (can_deadlock ?
                                            MYSQL_OPEN_FAIL_ON_MDL_CONFLICT : 0)),
                                          DT_INIT | DT_PREPARE | DT_CREATE));
  /*
    Restore old value of sql_command back as it is being looked at in
    process_table() function.
  */
  thd->lex->sql_command= save_sql_command;
  DBUG_RETURN(rc);
}


/*
  Mark all real tables in the list as free for reuse.

  SYNOPSIS
    mark_real_tables_as_free_for_reuse()
      thd   - thread context
      table - head of the list of tables

  DESCRIPTION
    Marks all real tables in the list (i.e. not views, derived
    or schema tables) as free for reuse.
*/

static void mark_real_tables_as_free_for_reuse(TABLE_LIST *table_list)
{
  TABLE_LIST *table;
  for (table= table_list; table; table= table->next_global)
    if (!table->placeholder())
    {
      table->table->query_id= 0;
    }
  for (table= table_list; table; table= table->next_global)
    if (!table->placeholder())
    {
      /*
        Detach children of MyISAMMRG tables used in
        sub-statements, they will be reattached at open.
        This has to be done in a separate loop to make sure
        that children have had their query_id cleared.
      */
      table->table->file->extra(HA_EXTRA_DETACH_CHILDREN);
    }
}


static bool fix_all_session_vcol_exprs(THD *thd, TABLE_LIST *tables)
{
  Security_context *save_security_ctx= thd->security_ctx;
  TABLE_LIST *first_not_own= thd->lex->first_not_own_table();
  DBUG_ENTER("fix_session_vcol_expr");

  for (TABLE_LIST *table= tables; table && table != first_not_own;
       table= table->next_global)
  {
    TABLE *t= table->table;
    if (!table->placeholder() && t->s->vcols_need_refixing &&
         table->lock_type >= TL_WRITE_ALLOW_WRITE)
    {
      if (table->security_ctx)
        thd->security_ctx= table->security_ctx;

      for (Field **vf= t->vfield; vf && *vf; vf++)
        if (fix_session_vcol_expr(thd, (*vf)->vcol_info))
          goto err;

      for (Field **df= t->default_field; df && *df; df++)
        if ((*df)->default_value &&
            fix_session_vcol_expr(thd, (*df)->default_value))
          goto err;

      for (Virtual_column_info **cc= t->check_constraints; cc && *cc; cc++)
        if (fix_session_vcol_expr(thd, (*cc)))
          goto err;

      thd->security_ctx= save_security_ctx;
    }
  }
  DBUG_RETURN(0);
err:
  thd->security_ctx= save_security_ctx;
  DBUG_RETURN(1);
}


/**
  Lock all tables in a list.

  @param  thd           Thread handler
  @param  tables        Tables to lock
  @param  count         Number of opened tables
  @param  flags         Options (see mysql_lock_tables() for details)

  You can't call lock_tables() while holding thr_lock locks, as
  this would break the dead-lock-free handling thr_lock gives us.
  You must always get all needed locks at once.

  If the query for which we are calling this function is marked as
  requiring prelocking, this function will change
  locked_tables_mode to LTM_PRELOCKED.

  @retval FALSE         Success. 
  @retval TRUE          A lock wait timeout, deadlock or out of memory.
*/

bool lock_tables(THD *thd, TABLE_LIST *tables, uint count, uint flags)
{
  TABLE_LIST *table;
  DBUG_ENTER("lock_tables");
  /*
    We can't meet statement requiring prelocking if we already
    in prelocked mode.
  */
  DBUG_ASSERT(thd->locked_tables_mode <= LTM_LOCK_TABLES ||
              !thd->lex->requires_prelocking());

  if (!tables && !thd->lex->requires_prelocking())
    DBUG_RETURN(thd->decide_logging_format(tables));

  /*
    Check for thd->locked_tables_mode to avoid a redundant
    and harmful attempt to lock the already locked tables again.
    Checking for thd->lock is not enough in some situations. For example,
    if a stored function contains
    "drop table t3; create temporary t3 ..; insert into t3 ...;"
    thd->lock may be 0 after drop tables, whereas locked_tables_mode
    is still on. In this situation an attempt to lock temporary
    table t3 will lead to a memory leak.
  */
  if (! thd->locked_tables_mode)
  {
    DBUG_ASSERT(thd->lock == 0);	// You must lock everything at once
    TABLE **start,**ptr;

    if (!(ptr=start=(TABLE**) thd->alloc(sizeof(TABLE*)*count)))
      DBUG_RETURN(TRUE);
    for (table= tables; table; table= table->next_global)
    {
      if (!table->placeholder())
	*(ptr++)= table->table;
    }

    DEBUG_SYNC(thd, "before_lock_tables_takes_lock");

    if (! (thd->lock= mysql_lock_tables(thd, start, (uint) (ptr - start),
                                        flags)))
      DBUG_RETURN(TRUE);

    DEBUG_SYNC(thd, "after_lock_tables_takes_lock");

    if (thd->lex->requires_prelocking() &&
        thd->lex->sql_command != SQLCOM_LOCK_TABLES)
    {
      TABLE_LIST *first_not_own= thd->lex->first_not_own_table();
      /*
        We just have done implicit LOCK TABLES, and now we have
        to emulate first open_and_lock_tables() after it.

        When open_and_lock_tables() is called for a single table out of
        a table list, the 'next_global' chain is temporarily broken. We
        may not find 'first_not_own' before the end of the "list".
        Look for example at those places where open_n_lock_single_table()
        is called. That function implements the temporary breaking of
        a table list for opening a single table.
      */
      for (table= tables;
           table && table != first_not_own;
           table= table->next_global)
      {
        if (!table->placeholder())
        {
          table->table->query_id= thd->query_id;
          if (check_lock_and_start_stmt(thd, thd->lex, table))
          {
            mysql_unlock_tables(thd, thd->lock);
            thd->lock= 0;
            DBUG_RETURN(TRUE);
          }
        }
      }
      /*
        Let us mark all tables which don't belong to the statement itself,
        and was marked as occupied during open_tables() as free for reuse.
      */
      mark_real_tables_as_free_for_reuse(first_not_own);
      DBUG_PRINT("info",("locked_tables_mode= LTM_PRELOCKED"));
      thd->enter_locked_tables_mode(LTM_PRELOCKED);
    }
  }
  else
  {
    TABLE_LIST *first_not_own= thd->lex->first_not_own_table();
    /*
      When open_and_lock_tables() is called for a single table out of
      a table list, the 'next_global' chain is temporarily broken. We
      may not find 'first_not_own' before the end of the "list".
      Look for example at those places where open_n_lock_single_table()
      is called. That function implements the temporary breaking of
      a table list for opening a single table.
    */
    for (table= tables;
         table && table != first_not_own;
         table= table->next_global)
    {
      if (table->placeholder())
        continue;

      /*
        In a stored function or trigger we should ensure that we won't change
        a table that is already used by the calling statement.
      */
      if (thd->locked_tables_mode >= LTM_PRELOCKED &&
          table->lock_type >= TL_WRITE_ALLOW_WRITE)
      {
        for (TABLE* opentab= thd->open_tables; opentab; opentab= opentab->next)
        {
          if (table->table->s == opentab->s && opentab->query_id &&
              table->table->query_id != opentab->query_id)
          {
            my_error(ER_CANT_UPDATE_USED_TABLE_IN_SF_OR_TRG, MYF(0),
                     table->table->s->table_name.str);
            DBUG_RETURN(TRUE);
          }
        }
      }

      if (check_lock_and_start_stmt(thd, thd->lex, table))
      {
	DBUG_RETURN(TRUE);
      }
    }
    /*
      If we are under explicit LOCK TABLES and our statement requires
      prelocking, we should mark all "additional" tables as free for use
      and enter prelocked mode.
    */
    if (thd->lex->requires_prelocking())
    {
      mark_real_tables_as_free_for_reuse(first_not_own);
      DBUG_PRINT("info",
                 ("thd->locked_tables_mode= LTM_PRELOCKED_UNDER_LOCK_TABLES"));
      thd->locked_tables_mode= LTM_PRELOCKED_UNDER_LOCK_TABLES;
    }
  }

  bool res= fix_all_session_vcol_exprs(thd, tables);
  if (!res)
    res= thd->decide_logging_format(tables);

  DBUG_RETURN(res);
}


/*
  Restart transaction for tables

  This is used when we had to do an implicit commit after tables are opened
  and want to restart transactions on tables.

  This is used in case of:
  LOCK TABLES xx
  CREATE OR REPLACE TABLE xx;
*/

bool restart_trans_for_tables(THD *thd, TABLE_LIST *table)
{
  DBUG_ENTER("restart_trans_for_tables");

  for (; table; table= table->next_global)
  {
    if (table->placeholder())
      continue;

    if (check_lock_and_start_stmt(thd, thd->lex, table))
    {
      DBUG_ASSERT(0);                           // Should never happen
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  Prepare statement for reopening of tables and recalculation of set of
  prelocked tables.

  @param[in] thd         Thread context.
  @param[in,out] tables  List of tables which we were trying to open
                         and lock.
  @param[in] start_of_statement_svp MDL savepoint which represents the set
                         of metadata locks which the current transaction
                         managed to acquire before execution of the current
                         statement and to which we should revert before
                         trying to reopen tables. NULL if no metadata locks
                         were held and thus all metadata locks should be
                         released.
*/

void close_tables_for_reopen(THD *thd, TABLE_LIST **tables,
                             const MDL_savepoint &start_of_statement_svp)
{
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();
  TABLE_LIST *tmp;

  /*
    If table list consists only from tables from prelocking set, table list
    for new attempt should be empty, so we have to update list's root pointer.
  */
  if (first_not_own_table == *tables)
    *tables= 0;
  thd->lex->chop_off_not_own_tables();
  /* Reset MDL tickets for procedures/functions */
  for (Sroutine_hash_entry *rt=
         (Sroutine_hash_entry*)thd->lex->sroutines_list.first;
       rt; rt= rt->next)
    rt->mdl_request.ticket= NULL;
  sp_remove_not_own_routines(thd->lex);
  for (tmp= *tables; tmp; tmp= tmp->next_global)
  {
    tmp->table= 0;
    tmp->mdl_request.ticket= NULL;
    /* We have to cleanup translation tables of views. */
    tmp->cleanup_items();
  }
  /*
    No need to commit/rollback the statement transaction: it's
    either not started or we're filling in an INFORMATION_SCHEMA
    table on the fly, and thus mustn't manipulate with the
    transaction of the enclosing statement.
  */
  DBUG_ASSERT(thd->transaction.stmt.is_empty() ||
              (thd->state_flags & Open_tables_state::BACKUPS_AVAIL));
  close_thread_tables(thd);
  thd->mdl_context.rollback_to_savepoint(start_of_statement_svp);
}


/*****************************************************************************
* The following find_field_in_XXX procedures implement the core of the
* name resolution functionality. The entry point to resolve a column name in a
* list of tables is 'find_field_in_tables'. It calls 'find_field_in_table_ref'
* for each table reference. In turn, depending on the type of table reference,
* 'find_field_in_table_ref' calls one of the 'find_field_in_XXX' procedures
* below specific for the type of table reference.
******************************************************************************/

/* Special Field pointers as return values of find_field_in_XXX functions. */
Field *not_found_field= (Field*) 0x1;
Field *view_ref_found= (Field*) 0x2; 

#define WRONG_GRANT (Field*) -1

static void update_field_dependencies(THD *thd, Field *field, TABLE *table)
{
  DBUG_ENTER("update_field_dependencies");
  if (should_mark_column(thd->column_usage))
  {
    /*
      We always want to register the used keys, as the column bitmap may have
      been set for all fields (for example for view).
    */
    table->covering_keys.intersect(field->part_of_key);

    if (thd->column_usage == MARK_COLUMNS_READ)
    {
      if (table->mark_column_with_deps(field))
        DBUG_VOID_RETURN; // Field was already marked
    }
    else
    {
      if (bitmap_fast_test_and_set(table->write_set, field->field_index))
      {
        DBUG_PRINT("warning", ("Found duplicated field"));
        thd->dup_field= field;
        DBUG_VOID_RETURN;
      }
    }

    table->used_fields++;
  }
  if (table->get_fields_in_item_tree)
    field->flags|= GET_FIXED_FIELDS_FLAG;
  DBUG_VOID_RETURN;
}


/*
  Find a field by name in a view that uses merge algorithm.

  SYNOPSIS
    find_field_in_view()
    thd				thread handler
    table_list			view to search for 'name'
    name			name of field
    length			length of name
    item_name                   name of item if it will be created (VIEW)
    ref				expression substituted in VIEW should be passed
                                using this reference (return view_ref_found)
    register_tree_change        TRUE if ref is not stack variable and we
                                need register changes in item tree

  RETURN
    0			field is not found
    view_ref_found	found value in VIEW (real result is in *ref)
    #			pointer to field - only for schema table fields
*/

static Field *
find_field_in_view(THD *thd, TABLE_LIST *table_list,
                   const char *name, size_t length,
                   const char *item_name, Item **ref,
                   bool register_tree_change)
{
  DBUG_ENTER("find_field_in_view");
  DBUG_PRINT("enter",
             ("view: '%s', field name: '%s', item name: '%s', ref %p",
              table_list->alias.str, name, item_name, ref));
  Field_iterator_view field_it;
  field_it.set(table_list);
  Query_arena *arena= 0, backup;  

  for (; !field_it.end_of_fields(); field_it.next())
  {
    if (!my_strcasecmp(system_charset_info, field_it.name()->str, name))
    {
      // in PS use own arena or data will be freed after prepare
      if (register_tree_change &&
          thd->stmt_arena->is_stmt_prepare_or_first_stmt_execute())
        arena= thd->activate_stmt_arena_if_needed(&backup);
      /*
        create_item() may, or may not create a new Item, depending on
        the column reference. See create_view_field() for details.
      */
      Item *item= field_it.create_item(thd);
      if (arena)
        thd->restore_active_arena(arena, &backup);
      
      if (!item)
        DBUG_RETURN(0);
      if (!ref)
        DBUG_RETURN((Field*) view_ref_found);
      /*
       *ref != NULL means that *ref contains the item that we need to
       replace. If the item was aliased by the user, set the alias to
       the replacing item.
      */
      if (*ref && !(*ref)->is_autogenerated_name)
        item->set_name(thd, (*ref)->name.str, (*ref)->name.length,
                       system_charset_info);
      if (register_tree_change)
        thd->change_item_tree(ref, item);
      else
        *ref= item;
      DBUG_RETURN((Field*) view_ref_found);
    }
  }
  DBUG_RETURN(0);
}


/*
  Find field by name in a NATURAL/USING join table reference.

  SYNOPSIS
    find_field_in_natural_join()
    thd			 [in]  thread handler
    table_ref            [in]  table reference to search
    name		 [in]  name of field
    length		 [in]  length of name
    ref                  [in/out] if 'name' is resolved to a view field, ref is
                               set to point to the found view field
    register_tree_change [in]  TRUE if ref is not stack variable and we
                               need register changes in item tree
    actual_table         [out] the original table reference where the field
                               belongs - differs from 'table_list' only for
                               NATURAL/USING joins

  DESCRIPTION
    Search for a field among the result fields of a NATURAL/USING join.
    Notice that this procedure is called only for non-qualified field
    names. In the case of qualified fields, we search directly the base
    tables of a natural join.

  RETURN
    NULL        if the field was not found
    WRONG_GRANT if no access rights to the found field
    #           Pointer to the found Field
*/

static Field *
find_field_in_natural_join(THD *thd, TABLE_LIST *table_ref, const char *name, size_t length, Item **ref, bool register_tree_change,
                           TABLE_LIST **actual_table)
{
  List_iterator_fast<Natural_join_column>
    field_it(*(table_ref->join_columns));
  Natural_join_column *nj_col, *curr_nj_col;
  Field *UNINIT_VAR(found_field);
  Query_arena *UNINIT_VAR(arena), backup;
  DBUG_ENTER("find_field_in_natural_join");
  DBUG_PRINT("enter", ("field name: '%s', ref %p",
		       name, ref));
  DBUG_ASSERT(table_ref->is_natural_join && table_ref->join_columns);
  DBUG_ASSERT(*actual_table == NULL);

  for (nj_col= NULL, curr_nj_col= field_it++; curr_nj_col; 
       curr_nj_col= field_it++)
  {
    if (!my_strcasecmp(system_charset_info, curr_nj_col->name()->str, name))
    {
      if (nj_col)
      {
        my_error(ER_NON_UNIQ_ERROR, MYF(0), name, thd->where);
        DBUG_RETURN(NULL);
      }
      nj_col= curr_nj_col;
    }
  }
  if (!nj_col)
    DBUG_RETURN(NULL);

  if (nj_col->view_field)
  {
    Item *item;
    if (register_tree_change)
      arena= thd->activate_stmt_arena_if_needed(&backup);
    /*
      create_item() may, or may not create a new Item, depending on the
      column reference. See create_view_field() for details.
    */
    item= nj_col->create_item(thd);
    if (!item)
      DBUG_RETURN(NULL);

    /*
     *ref != NULL means that *ref contains the item that we need to
     replace. If the item was aliased by the user, set the alias to
     the replacing item.
     */
    if (*ref && !(*ref)->is_autogenerated_name)
      item->set_name(thd, (*ref)->name.str, (*ref)->name.length,
                     system_charset_info);
    if (register_tree_change && arena)
      thd->restore_active_arena(arena, &backup);

    if (!item)
      DBUG_RETURN(NULL);
    DBUG_ASSERT(nj_col->table_field == NULL);
    if (nj_col->table_ref->schema_table_reformed)
    {
      /*
        Translation table items are always Item_fields and fixed
        already('mysql_schema_table' function). So we can return
        ->field. It is used only for 'show & where' commands.
      */
      DBUG_RETURN(((Item_field*) (nj_col->view_field->item))->field);
    }
    if (register_tree_change)
      thd->change_item_tree(ref, item);
    else
      *ref= item;
    found_field= (Field*) view_ref_found;
  }
  else
  {
    /* This is a base table. */
    DBUG_ASSERT(nj_col->view_field == NULL);
    Item *ref= 0;
    /*
      This fix_fields is not necessary (initially this item is fixed by
      the Item_field constructor; after reopen_tables the Item_func_eq
      calls fix_fields on that item), it's just a check during table
      reopening for columns that was dropped by the concurrent connection.
    */
    if (nj_col->table_field->fix_fields_if_needed(thd, &ref))
    {
      DBUG_PRINT("info", ("column '%s' was dropped by the concurrent connection",
                          nj_col->table_field->name.str));
      DBUG_RETURN(NULL);
    }
    DBUG_ASSERT(ref == 0);                      // Should not have changed
    DBUG_ASSERT(nj_col->table_ref->table == nj_col->table_field->field->table);
    found_field= nj_col->table_field->field;
    update_field_dependencies(thd, found_field, nj_col->table_ref->table);
  }

  *actual_table= nj_col->table_ref;
  
  DBUG_RETURN(found_field);
}


/*
  Find field by name in a base table or a view with temp table algorithm.

  The caller is expected to check column-level privileges.

  SYNOPSIS
    find_field_in_table()
    thd				thread handler
    table			table where to search for the field
    name			name of field
    length			length of name
    allow_rowid			do allow finding of "_rowid" field?
    cached_field_index_ptr	cached position in field list (used to speedup
                                lookup for fields in prepared tables)

  RETURN
    0	field is not found
    #	pointer to field
*/

Field *
find_field_in_table(THD *thd, TABLE *table, const char *name, size_t length,
                    bool allow_rowid, uint *cached_field_index_ptr)
{
  Field *field;
  uint cached_field_index= *cached_field_index_ptr;
  DBUG_ENTER("find_field_in_table");
  DBUG_PRINT("enter", ("table: '%s', field name: '%s'", table->alias.c_ptr(),
                       name));

  /* We assume here that table->field < NO_CACHED_FIELD_INDEX = UINT_MAX */
  if (cached_field_index < table->s->fields &&
      !my_strcasecmp(system_charset_info,
                     table->field[cached_field_index]->field_name.str, name))
    field= table->field[cached_field_index];
  else
  {
    LEX_CSTRING fname= {name, length};
    field= table->find_field_by_name(&fname);
  }

  if (field)
  {
    if (field->invisible == INVISIBLE_FULL &&
        DBUG_EVALUATE_IF("test_completely_invisible", 0, 1))
      DBUG_RETURN((Field*)0);

    if (field->invisible == INVISIBLE_SYSTEM &&
        thd->column_usage != MARK_COLUMNS_READ &&
        thd->column_usage != COLUMNS_READ)
      DBUG_RETURN((Field*)0);
  }
  else
  {
    if (!allow_rowid ||
        my_strcasecmp(system_charset_info, name, "_rowid") ||
        table->s->rowid_field_offset == 0)
      DBUG_RETURN((Field*) 0);
    field= table->field[table->s->rowid_field_offset-1];
  }
  *cached_field_index_ptr= field->field_index;

  update_field_dependencies(thd, field, table);

  DBUG_RETURN(field);
}


/*
  Find field in a table reference.

  SYNOPSIS
    find_field_in_table_ref()
    thd			   [in]  thread handler
    table_list		   [in]  table reference to search
    name		   [in]  name of field
    length		   [in]  field length of name
    item_name              [in]  name of item if it will be created (VIEW)
    db_name                [in]  optional database name that qualifies the
    table_name             [in]  optional table name that qualifies the field
                                 0 for non-qualified field in natural joins
    ref		       [in/out] if 'name' is resolved to a view field, ref
                                 is set to point to the found view field
    check_privileges       [in]  check privileges
    allow_rowid		   [in]  do allow finding of "_rowid" field?
    cached_field_index_ptr [in]  cached position in field list (used to
                                 speedup lookup for fields in prepared tables)
    register_tree_change   [in]  TRUE if ref is not stack variable and we
                                 need register changes in item tree
    actual_table           [out] the original table reference where the field
                                 belongs - differs from 'table_list' only for
                                 NATURAL_USING joins.

  DESCRIPTION
    Find a field in a table reference depending on the type of table
    reference. There are three types of table references with respect
    to the representation of their result columns:
    - an array of Field_translator objects for MERGE views and some
      information_schema tables,
    - an array of Field objects (and possibly a name hash) for stored
      tables,
    - a list of Natural_join_column objects for NATURAL/USING joins.
    This procedure detects the type of the table reference 'table_list'
    and calls the corresponding search routine.

    The routine checks column-level privieleges for the found field.

  RETURN
    0			field is not found
    view_ref_found	found value in VIEW (real result is in *ref)
    #			pointer to field
*/

Field *
find_field_in_table_ref(THD *thd, TABLE_LIST *table_list,
                        const char *name, size_t length,
                        const char *item_name, const char *db_name,
                        const char *table_name, Item **ref,
                        bool check_privileges, bool allow_rowid,
                        uint *cached_field_index_ptr,
                        bool register_tree_change, TABLE_LIST **actual_table)
{
  Field *fld;
  DBUG_ENTER("find_field_in_table_ref");
  DBUG_ASSERT(table_list->alias.str);
  DBUG_ASSERT(name);
  DBUG_ASSERT(item_name);
  DBUG_PRINT("enter",
             ("table: '%s'  field name: '%s'  item name: '%s'  ref %p",
              table_list->alias.str, name, item_name, ref));

  /*
    Check that the table and database that qualify the current field name
    are the same as the table reference we are going to search for the field.

    Exclude from the test below nested joins because the columns in a
    nested join generally originate from different tables. Nested joins
    also have no table name, except when a nested join is a merge view
    or an information schema table.

    We include explicitly table references with a 'field_translation' table,
    because if there are views over natural joins we don't want to search
    inside the view, but we want to search directly in the view columns
    which are represented as a 'field_translation'.

    tables->db.str may be 0 if we are preparing a statement
    db_name is 0 if item doesn't have a db name
    table_name is 0 if item doesn't have a specified table_name
  */
  if (db_name && !db_name[0])
    db_name= 0;                                 // Simpler test later

  if (/* Exclude nested joins. */
      (!table_list->nested_join ||
       /* Include merge views and information schema tables. */
       table_list->field_translation) &&
      /*
        Test if the field qualifiers match the table reference we plan
        to search.
      */
      table_name && table_name[0] &&
      (my_strcasecmp(table_alias_charset, table_list->alias.str, table_name) ||
       (db_name && (!table_list->db.str || !table_list->db.str[0])) ||
       (db_name && table_list->db.str && table_list->db.str[0] &&
        (table_list->schema_table ?
         my_strcasecmp(system_charset_info, db_name, table_list->db.str) :
         strcmp(db_name, table_list->db.str)))))
    DBUG_RETURN(0);

  /*
    Don't allow usage of fields in sequence table that is opened as part of
    NEXT VALUE for sequence_name
  */
  if (table_list->sequence)
    DBUG_RETURN(0);

  *actual_table= NULL;

  if (table_list->field_translation)
  {
    /* 'table_list' is a view or an information schema table. */
    if ((fld= find_field_in_view(thd, table_list, name, length, item_name, ref,
                                 register_tree_change)))
      *actual_table= table_list;
  }
  else if (!table_list->nested_join)
  {
    /* 'table_list' is a stored table. */
    DBUG_ASSERT(table_list->table);
    if ((fld= find_field_in_table(thd, table_list->table, name, length,
                                  allow_rowid,
                                  cached_field_index_ptr)))
      *actual_table= table_list;
  }
  else
  {
    /*
      'table_list' is a NATURAL/USING join, or an operand of such join that
      is a nested join itself.

      If the field name we search for is qualified, then search for the field
      in the table references used by NATURAL/USING the join.
    */
    if (table_name && table_name[0])
    {
      List_iterator<TABLE_LIST> it(table_list->nested_join->join_list);
      TABLE_LIST *table;
      while ((table= it++))
      {
        if ((fld= find_field_in_table_ref(thd, table, name, length, item_name,
                                          db_name, table_name, ref,
                                          check_privileges, allow_rowid,
                                          cached_field_index_ptr,
                                          register_tree_change, actual_table)))
          DBUG_RETURN(fld);
      }
      DBUG_RETURN(0);
    }
    /*
      Non-qualified field, search directly in the result columns of the
      natural join. The condition of the outer IF is true for the top-most
      natural join, thus if the field is not qualified, we will search
      directly the top-most NATURAL/USING join.
    */
    fld= find_field_in_natural_join(thd, table_list, name, length, ref,
                                    register_tree_change, actual_table);
  }

  if (fld)
  {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /* Check if there are sufficient access rights to the found field. */
    if (check_privileges &&
        check_column_grant_in_table_ref(thd, *actual_table, name, length, fld))
      fld= WRONG_GRANT;
    else
#endif
      if (should_mark_column(thd->column_usage))
      {
        /*
          Get rw_set correct for this field so that the handler
          knows that this field is involved in the query and gets
          retrieved/updated
         */
        Field *field_to_set= NULL;
        if (fld == view_ref_found)
        {
          if (!ref)
            DBUG_RETURN(fld);
          Item *it= (*ref)->real_item();
          if (it->type() == Item::FIELD_ITEM)
            field_to_set= ((Item_field*)it)->field;
          else
          {
            if (thd->column_usage == MARK_COLUMNS_READ)
              it->walk(&Item::register_field_in_read_map, 0, 0);
            else
              it->walk(&Item::register_field_in_write_map, 0, 0);
          }
        }
        else
          field_to_set= fld;
        if (field_to_set)
        {
          TABLE *table= field_to_set->table;
          DBUG_ASSERT(table);
          if (thd->column_usage == MARK_COLUMNS_READ)
            bitmap_set_bit(table->read_set, field_to_set->field_index);
          else
            bitmap_set_bit(table->write_set, field_to_set->field_index);
        }
      }
  }
  DBUG_RETURN(fld);
}


/*
  Find field in table, no side effects, only purpose is to check for field
  in table object and get reference to the field if found.

  SYNOPSIS
  find_field_in_table_sef()

  table                         table where to find
  name                          Name of field searched for

  RETURN
    0                   field is not found
    #                   pointer to field
*/

Field *find_field_in_table_sef(TABLE *table, const char *name)
{
  Field **field_ptr;
  if (table->s->name_hash.records)
  {
    field_ptr= (Field**)my_hash_search(&table->s->name_hash,(uchar*) name,
                                       strlen(name));
    if (field_ptr)
    {
      /*
        field_ptr points to field in TABLE_SHARE. Convert it to the matching
        field in table
      */
      field_ptr= (table->field + (field_ptr - table->s->field));
    }
  }
  else
  {
    if (!(field_ptr= table->field))
      return (Field *)0;
    for (; *field_ptr; ++field_ptr)
      if (!my_strcasecmp(system_charset_info, (*field_ptr)->field_name.str,
                         name))
        break;
  }
  if (field_ptr)
    return *field_ptr;
  else
    return (Field *)0;
}


/*
  Find field in table list.

  SYNOPSIS
    find_field_in_tables()
    thd			  pointer to current thread structure
    item		  field item that should be found
    first_table           list of tables to be searched for item
    last_table            end of the list of tables to search for item. If NULL
                          then search to the end of the list 'first_table'.
    ref			  if 'item' is resolved to a view field, ref is set to
                          point to the found view field
    report_error	  Degree of error reporting:
                          - IGNORE_ERRORS then do not report any error
                          - IGNORE_EXCEPT_NON_UNIQUE report only non-unique
                            fields, suppress all other errors
                          - REPORT_EXCEPT_NON_UNIQUE report all other errors
                            except when non-unique fields were found
                          - REPORT_ALL_ERRORS
    check_privileges      need to check privileges
    register_tree_change  TRUE if ref is not a stack variable and we
                          to need register changes in item tree

  RETURN VALUES
    0			If error: the found field is not unique, or there are
                        no sufficient access priviliges for the found field,
                        or the field is qualified with non-existing table.
    not_found_field	The function was called with report_error ==
                        (IGNORE_ERRORS || IGNORE_EXCEPT_NON_UNIQUE) and a
			field was not found.
    view_ref_found	View field is found, item passed through ref parameter
    found field         If a item was resolved to some field
*/

Field *
find_field_in_tables(THD *thd, Item_ident *item,
                     TABLE_LIST *first_table, TABLE_LIST *last_table,
		     Item **ref, find_item_error_report_type report_error,
                     bool check_privileges, bool register_tree_change)
{
  Field *found=0;
  const char *db= item->db_name;
  const char *table_name= item->table_name;
  const char *name= item->field_name.str;
  size_t length= item->field_name.length;
  char name_buff[SAFE_NAME_LEN+1];
  TABLE_LIST *cur_table= first_table;
  TABLE_LIST *actual_table;
  bool allow_rowid;

  if (!table_name || !table_name[0])
  {
    table_name= 0;                              // For easier test
    db= 0;
  }

  allow_rowid= table_name || (cur_table && !cur_table->next_local);

  if (item->cached_table)
  {
    DBUG_PRINT("info", ("using cached table"));
    /*
      This shortcut is used by prepared statements. We assume that
      TABLE_LIST *first_table is not changed during query execution (which
      is true for all queries except RENAME but luckily RENAME doesn't
      use fields...) so we can rely on reusing pointer to its member.
      With this optimization we also miss case when addition of one more
      field makes some prepared query ambiguous and so erroneous, but we
      accept this trade off.
    */
    TABLE_LIST *table_ref= item->cached_table;
    /*
      The condition (table_ref->view == NULL) ensures that we will call
      find_field_in_table even in the case of information schema tables
      when table_ref->field_translation != NULL.
      */
    if (table_ref->table && !table_ref->view &&
        (!table_ref->is_merged_derived() ||
         (!table_ref->is_multitable() && table_ref->merged_for_insert)))
    {

      found= find_field_in_table(thd, table_ref->table, name, length,
                                 TRUE, &(item->cached_field_index));
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      /* Check if there are sufficient access rights to the found field. */
      if (found && check_privileges &&
          check_column_grant_in_table_ref(thd, table_ref, name, length, found))
        found= WRONG_GRANT;
#endif
    }
    else
      found= find_field_in_table_ref(thd, table_ref, name, length, item->name.str,
                                     NULL, NULL, ref, check_privileges,
                                     TRUE, &(item->cached_field_index),
                                     register_tree_change,
                                     &actual_table);
    if (found)
    {
      if (found == WRONG_GRANT)
	return (Field*) 0;

      /*
        Only views fields should be marked as dependent, not an underlying
        fields.
      */
      if (!table_ref->belong_to_view &&
          !table_ref->belong_to_derived)
      {
        SELECT_LEX *current_sel= item->context->select_lex;
        SELECT_LEX *last_select= table_ref->select_lex;
        bool all_merged= TRUE;
        for (SELECT_LEX *sl= current_sel; sl && sl!=last_select;
             sl=sl->outer_select())
        {
          Item *subs= sl->master_unit()->item;
          if (subs->type() == Item::SUBSELECT_ITEM && 
              ((Item_subselect*)subs)->substype() == Item_subselect::IN_SUBS &&
              ((Item_in_subselect*)subs)->test_strategy(SUBS_SEMI_JOIN))
          {
            continue;
          }
          all_merged= FALSE;
          break;
        }
        /*
          If the field was an outer referencee, mark all selects using this
          sub query as dependent on the outer query
        */
        if (!all_merged && current_sel != last_select)
        {
          mark_select_range_as_dependent(thd, last_select, current_sel,
                                         found, *ref, item);
        }
      }
      return found;
    }
  }
  else
    item->can_be_depended= TRUE;

  if (db && lower_case_table_names)
  {
    /*
      convert database to lower case for comparison.
      We can't do this in Item_field as this would change the
      'name' of the item which may be used in the select list
    */
    strmake_buf(name_buff, db);
    my_casedn_str(files_charset_info, name_buff);
    db= name_buff;
  }

  if (last_table)
    last_table= last_table->next_name_resolution_table;

  for (; cur_table != last_table ;
       cur_table= cur_table->next_name_resolution_table)
  {
    Field *cur_field= find_field_in_table_ref(thd, cur_table, name, length,
                                              item->name.str, db, table_name, ref,
                                              (thd->lex->sql_command ==
                                               SQLCOM_SHOW_FIELDS)
                                              ? false : check_privileges,
                                              allow_rowid,
                                              &(item->cached_field_index),
                                              register_tree_change,
                                              &actual_table);
    if (cur_field)
    {
      if (cur_field == WRONG_GRANT)
      {
        if (thd->lex->sql_command != SQLCOM_SHOW_FIELDS)
          return (Field*) 0;

        thd->clear_error();
        cur_field= find_field_in_table_ref(thd, cur_table, name, length,
                                           item->name.str, db, table_name, ref,
                                           false,
                                           allow_rowid,
                                           &(item->cached_field_index),
                                           register_tree_change,
                                           &actual_table);
        if (cur_field)
        {
          Field *nf=new Field_null(NULL,0,Field::NONE,
                                   &cur_field->field_name,
                                   &my_charset_bin);
          nf->init(cur_table->table);
          cur_field= nf;
        }
      }

      /*
        Store the original table of the field, which may be different from
        cur_table in the case of NATURAL/USING join.
      */
      item->cached_table= (!actual_table->cacheable_table || found) ?
                          0 : actual_table;

      DBUG_ASSERT(thd->where);
      /*
        If we found a fully qualified field we return it directly as it can't
        have duplicates.
       */
      if (db)
        return cur_field;
      
      if (unlikely(found))
      {
        if (report_error == REPORT_ALL_ERRORS ||
            report_error == IGNORE_EXCEPT_NON_UNIQUE)
          my_error(ER_NON_UNIQ_ERROR, MYF(0),
                   table_name ? item->full_name() : name, thd->where);
        return (Field*) 0;
      }
      found= cur_field;
    }
  }

  if (likely(found))
    return found;
  
  /*
    If the field was qualified and there were no tables to search, issue
    an error that an unknown table was given. The situation is detected
    as follows: if there were no tables we wouldn't go through the loop
    and cur_table wouldn't be updated by the loop increment part, so it
    will be equal to the first table.
  */
  if (table_name && (cur_table == first_table) &&
      (report_error == REPORT_ALL_ERRORS ||
       report_error == REPORT_EXCEPT_NON_UNIQUE))
  {
    char buff[SAFE_NAME_LEN*2 + 2];
    if (db && db[0])
    {
      strxnmov(buff,sizeof(buff)-1,db,".",table_name,NullS);
      table_name=buff;
    }
    my_error(ER_UNKNOWN_TABLE, MYF(0), table_name, thd->where);
  }
  else
  {
    if (report_error == REPORT_ALL_ERRORS ||
        report_error == REPORT_EXCEPT_NON_UNIQUE)
      my_error(ER_BAD_FIELD_ERROR, MYF(0), item->full_name(), thd->where);
    else
      found= not_found_field;
  }
  return found;
}


/*
  Find Item in list of items (find_field_in_tables analog)

  TODO
    is it better return only counter?

  SYNOPSIS
    find_item_in_list()
    find			Item to find
    items			List of items
    counter			To return number of found item
    report_error
      REPORT_ALL_ERRORS		report errors, return 0 if error
      REPORT_EXCEPT_NOT_FOUND	Do not report 'not found' error and
				return not_found_item, report other errors,
				return 0
      IGNORE_ERRORS		Do not report errors, return 0 if error
    resolution                  Set to the resolution type if the item is found 
                                (it says whether the item is resolved 
                                 against an alias name,
                                 or as a field name without alias,
                                 or as a field hidden by alias,
                                 or ignoring alias)
    limit                       How many items in the list to check
                                (if limit==0 then all items are to be checked)
                                
  RETURN VALUES
    0			Item is not found or item is not unique,
			error message is reported
    not_found_item	Function was called with
			report_error == REPORT_EXCEPT_NOT_FOUND and
			item was not found. No error message was reported
                        found field
*/

/* Special Item pointer to serve as a return value from find_item_in_list(). */
Item **not_found_item= (Item**) 0x1;


Item **
find_item_in_list(Item *find, List<Item> &items, uint *counter,
                  find_item_error_report_type report_error,
                  enum_resolution_type *resolution, uint limit)
{
  List_iterator<Item> li(items);
  uint n_items= limit == 0 ? items.elements : limit;
  Item **found=0, **found_unaliased= 0, *item;
  const char *db_name=0;
  const LEX_CSTRING *field_name= 0;
  const char *table_name=0;
  bool found_unaliased_non_uniq= 0;
  /*
    true if the item that we search for is a valid name reference
    (and not an item that happens to have a name).
  */
  bool is_ref_by_name= 0;
  uint unaliased_counter= 0;

  *resolution= NOT_RESOLVED;

  is_ref_by_name= (find->type() == Item::FIELD_ITEM  || 
                   find->type() == Item::REF_ITEM);
  if (is_ref_by_name)
  {
    field_name= &((Item_ident*) find)->field_name;
    table_name= ((Item_ident*) find)->table_name;
    db_name=    ((Item_ident*) find)->db_name;
  }

  for (uint i= 0; i < n_items; i++)
  {
    item= li++;
    if (field_name && field_name->str &&
        (item->real_item()->type() == Item::FIELD_ITEM ||
         ((item->type() == Item::REF_ITEM) &&
          (((Item_ref *)item)->ref_type() == Item_ref::VIEW_REF))))
    {
      Item_ident *item_field= (Item_ident*) item;

      /*
	In case of group_concat() with ORDER BY condition in the QUERY
	item_field can be field of temporary table without item name 
	(if this field created from expression argument of group_concat()),
	=> we have to check presence of name before compare
      */ 
      if (unlikely(!item_field->name.str))
        continue;

      if (table_name)
      {
        /*
          If table name is specified we should find field 'field_name' in
          table 'table_name'. According to SQL-standard we should ignore
          aliases in this case.

          Since we should NOT prefer fields from the select list over
          other fields from the tables participating in this select in
          case of ambiguity we have to do extra check outside this function.

          We use strcmp for table names and database names as these may be
          case sensitive. In cases where they are not case sensitive, they
          are always in lower case.

	  item_field->field_name and item_field->table_name can be 0x0 if
	  item is not fix_field()'ed yet.
        */
        if (item_field->field_name.str && item_field->table_name &&
	    !lex_string_cmp(system_charset_info, &item_field->field_name,
                            field_name) &&
            !my_strcasecmp(table_alias_charset, item_field->table_name, 
                           table_name) &&
            (!db_name || (item_field->db_name &&
                          !strcmp(item_field->db_name, db_name))))
        {
          if (found_unaliased)
          {
            if ((*found_unaliased)->eq(item, 0))
              continue;
            /*
              Two matching fields in select list.
              We already can bail out because we are searching through
              unaliased names only and will have duplicate error anyway.
            */
            if (report_error != IGNORE_ERRORS)
              my_error(ER_NON_UNIQ_ERROR, MYF(0),
                       find->full_name(), current_thd->where);
            return (Item**) 0;
          }
          found_unaliased= li.ref();
          unaliased_counter= i;
          *resolution= RESOLVED_IGNORING_ALIAS;
          if (db_name)
            break;                              // Perfect match
        }
      }
      else
      {
        bool fname_cmp= lex_string_cmp(system_charset_info,
                                       &item_field->field_name,
                                       field_name);
        if (!lex_string_cmp(system_charset_info,
                            &item_field->name, field_name))
        {
          /*
            If table name was not given we should scan through aliases
            and non-aliased fields first. We are also checking unaliased
            name of the field in then next  else-if, to be able to find
            instantly field (hidden by alias) if no suitable alias or
            non-aliased field was found.
          */
          if (found)
          {
            if ((*found)->eq(item, 0))
              continue;                           // Same field twice
            if (report_error != IGNORE_ERRORS)
              my_error(ER_NON_UNIQ_ERROR, MYF(0),
                       find->full_name(), current_thd->where);
            return (Item**) 0;
          }
          found= li.ref();
          *counter= i;
          *resolution= fname_cmp ? RESOLVED_AGAINST_ALIAS:
	                           RESOLVED_WITH_NO_ALIAS;
        }
        else if (!fname_cmp)
        {
          /*
            We will use non-aliased field or react on such ambiguities only if
            we won't be able to find aliased field.
            Again if we have ambiguity with field outside of select list
            we should prefer fields from select list.
          */
          if (found_unaliased)
          {
            if ((*found_unaliased)->eq(item, 0))
              continue;                           // Same field twice
            found_unaliased_non_uniq= 1;
          }
          found_unaliased= li.ref();
          unaliased_counter= i;
        }
      }
    }
    else if (!table_name)
    { 
      if (is_ref_by_name && find->name.str && item->name.str &&
          find->name.length == item->name.length &&
	  !lex_string_cmp(system_charset_info, &item->name, &find->name))
      {
        found= li.ref();
        *counter= i;
        *resolution= RESOLVED_AGAINST_ALIAS;
        break;
      }
      else if (find->eq(item,0))
      {
        found= li.ref();
        *counter= i;
        *resolution= RESOLVED_IGNORING_ALIAS;
        break;
      }
    }
  }

  if (likely(found))
    return found;

  if (unlikely(found_unaliased_non_uniq))
  {
    if (report_error != IGNORE_ERRORS)
      my_error(ER_NON_UNIQ_ERROR, MYF(0),
               find->full_name(), current_thd->where);
    return (Item **) 0;
  }
  if (found_unaliased)
  {
    found= found_unaliased;
    *counter= unaliased_counter;
    *resolution= RESOLVED_BEHIND_ALIAS;
  }

  if (found)
    return found;

  if (report_error != REPORT_EXCEPT_NOT_FOUND)
  {
    if (report_error == REPORT_ALL_ERRORS)
      my_error(ER_BAD_FIELD_ERROR, MYF(0),
               find->full_name(), current_thd->where);
    return (Item **) 0;
  }
  else
    return (Item **) not_found_item;
}


/*
  Test if a string is a member of a list of strings.

  SYNOPSIS
    test_if_string_in_list()
    find      the string to look for
    str_list  a list of strings to be searched

  DESCRIPTION
    Sequentially search a list of strings for a string, and test whether
    the list contains the same string.

  RETURN
    TRUE  if find is in str_list
    FALSE otherwise
*/

static bool
test_if_string_in_list(const char *find, List<String> *str_list)
{
  List_iterator<String> str_list_it(*str_list);
  String *curr_str;
  size_t find_length= strlen(find);
  while ((curr_str= str_list_it++))
  {
    if (find_length != curr_str->length())
      continue;
    if (!my_strcasecmp(system_charset_info, find, curr_str->ptr()))
      return TRUE;
  }
  return FALSE;
}


/*
  Create a new name resolution context for an item so that it is
  being resolved in a specific table reference.

  SYNOPSIS
    set_new_item_local_context()
    thd        pointer to current thread
    item       item for which new context is created and set
    table_ref  table ref where an item showld be resolved

  DESCRIPTION
    Create a new name resolution context for an item, so that the item
    is resolved only the supplied 'table_ref'.

  RETURN
    FALSE  if all OK
    TRUE   otherwise
*/

static bool
set_new_item_local_context(THD *thd, Item_ident *item, TABLE_LIST *table_ref)
{
  Name_resolution_context *context;
  if (!(context= new (thd->mem_root) Name_resolution_context))
    return TRUE;
  context->init();
  context->first_name_resolution_table=
    context->last_name_resolution_table= table_ref;
  item->context= context;
  return FALSE;
}


/*
  Find and mark the common columns of two table references.

  SYNOPSIS
    mark_common_columns()
    thd                [in] current thread
    table_ref_1        [in] the first (left) join operand
    table_ref_2        [in] the second (right) join operand
    using_fields       [in] if the join is JOIN...USING - the join columns,
                            if NATURAL join, then NULL
    found_using_fields [out] number of fields from the USING clause that were
                             found among the common fields

  DESCRIPTION
    The procedure finds the common columns of two relations (either
    tables or intermediate join results), and adds an equi-join condition
    to the ON clause of 'table_ref_2' for each pair of matching columns.
    If some of table_ref_XXX represents a base table or view, then we
    create new 'Natural_join_column' instances for each column
    reference and store them in the 'join_columns' of the table
    reference.

  IMPLEMENTATION
    The procedure assumes that store_natural_using_join_columns() was
    called for the previous level of NATURAL/USING joins.

  RETURN
    TRUE   error when some common column is non-unique, or out of memory
    FALSE  OK
*/

static bool
mark_common_columns(THD *thd, TABLE_LIST *table_ref_1, TABLE_LIST *table_ref_2,
                    List<String> *using_fields, uint *found_using_fields)
{
  Field_iterator_table_ref it_1, it_2;
  Natural_join_column *nj_col_1, *nj_col_2;
  Query_arena *arena, backup;
  bool result= TRUE;
  bool first_outer_loop= TRUE;
  Field *field_1;
  field_visibility_t field_1_invisible, field_2_invisible;
  /*
    Leaf table references to which new natural join columns are added
    if the leaves are != NULL.
  */
  TABLE_LIST *leaf_1= (table_ref_1->nested_join &&
                       !table_ref_1->is_natural_join) ?
                      NULL : table_ref_1;
  TABLE_LIST *leaf_2= (table_ref_2->nested_join &&
                       !table_ref_2->is_natural_join) ?
                      NULL : table_ref_2;

  DBUG_ENTER("mark_common_columns");
  DBUG_PRINT("info", ("operand_1: %s  operand_2: %s",
                      table_ref_1->alias.str, table_ref_2->alias.str));

  *found_using_fields= 0;
  arena= thd->activate_stmt_arena_if_needed(&backup);

  for (it_1.set(table_ref_1); !it_1.end_of_fields(); it_1.next())
  {
    bool found= FALSE;
    const LEX_CSTRING *field_name_1;
    Field *field_2= 0;

    /* true if field_name_1 is a member of using_fields */
    bool is_using_column_1;
    if (!(nj_col_1= it_1.get_or_create_column_ref(thd, leaf_1)))
      goto err;

    field_1= nj_col_1->field();
    field_1_invisible= field_1 ? field_1->invisible : VISIBLE;

    if (field_1_invisible == INVISIBLE_FULL)
      continue;

    field_name_1= nj_col_1->name();
    is_using_column_1= using_fields && 
      test_if_string_in_list(field_name_1->str, using_fields);
    DBUG_PRINT ("info", ("field_name_1=%s.%s", 
                         nj_col_1->safe_table_name(),
                         field_name_1->str));

    if (field_1_invisible && !is_using_column_1)
      continue;

    /*
      Find a field with the same name in table_ref_2.

      Note that for the second loop, it_2.set() will iterate over
      table_ref_2->join_columns and not generate any new elements or
      lists.
    */
    nj_col_2= NULL;
    for (it_2.set(table_ref_2); !it_2.end_of_fields(); it_2.next())
    {
      Natural_join_column *cur_nj_col_2;
      const LEX_CSTRING *cur_field_name_2;
      if (!(cur_nj_col_2= it_2.get_or_create_column_ref(thd, leaf_2)))
        goto err;

      field_2= cur_nj_col_2->field();
      field_2_invisible= field_2 ? field_2->invisible : VISIBLE;

      if (field_2_invisible == INVISIBLE_FULL)
        continue;

      cur_field_name_2= cur_nj_col_2->name();
      DBUG_PRINT ("info", ("cur_field_name_2=%s.%s", 
                           cur_nj_col_2->safe_table_name(),
                           cur_field_name_2->str));

      /*
        Compare the two columns and check for duplicate common fields.
        A common field is duplicate either if it was already found in
        table_ref_2 (then found == TRUE), or if a field in table_ref_2
        was already matched by some previous field in table_ref_1
        (then cur_nj_col_2->is_common == TRUE).
        Note that it is too early to check the columns outside of the
        USING list for ambiguity because they are not actually "referenced"
        here. These columns must be checked only on unqualified reference 
        by name (e.g. in SELECT list).
      */
      if (!lex_string_cmp(system_charset_info, field_name_1,
                          cur_field_name_2))
      {
        DBUG_PRINT ("info", ("match c1.is_common=%d", nj_col_1->is_common));
        if (cur_nj_col_2->is_common || found)
        {
          my_error(ER_NON_UNIQ_ERROR, MYF(0), field_name_1->str, thd->where);
          goto err;
        }
        if ((!using_fields && !field_2_invisible) || is_using_column_1)
        {
          DBUG_ASSERT(nj_col_2 == NULL);
          nj_col_2= cur_nj_col_2;
          found= TRUE;
        }
      }
    }
    if (first_outer_loop && leaf_2)
    {
      /*
        Make sure that the next inner loop "knows" that all columns
        are materialized already.
      */
      leaf_2->is_join_columns_complete= TRUE;
      first_outer_loop= FALSE;
    }
    if (!found)
      continue;                                 // No matching field

    /*
      field_1 and field_2 have the same names. Check if they are in the USING
      clause (if present), mark them as common fields, and add a new
      equi-join condition to the ON clause.
    */
    if (nj_col_2)
    {
      /*
        Create non-fixed fully qualified field and let fix_fields to
        resolve it.
      */
      Item *item_1=   nj_col_1->create_item(thd);
      Item *item_2=   nj_col_2->create_item(thd);
      Item_ident *item_ident_1, *item_ident_2;
      Item_func_eq *eq_cond;

      if (!item_1 || !item_2)
        goto err;                               // out of memory

      /*
        The following assert checks that the two created items are of
        type Item_ident.
      */
      DBUG_ASSERT(!thd->lex->current_select->no_wrap_view_item);
      /*
        In the case of no_wrap_view_item == 0, the created items must be
        of sub-classes of Item_ident.
      */
      DBUG_ASSERT(item_1->type() == Item::FIELD_ITEM ||
                  item_1->type() == Item::REF_ITEM);
      DBUG_ASSERT(item_2->type() == Item::FIELD_ITEM ||
                  item_2->type() == Item::REF_ITEM);

      /*
        We need to cast item_1,2 to Item_ident, because we need to hook name
        resolution contexts specific to each item.
      */
      item_ident_1= (Item_ident*) item_1;
      item_ident_2= (Item_ident*) item_2;
      /*
        Create and hook special name resolution contexts to each item in the
        new join condition . We need this to both speed-up subsequent name
        resolution of these items, and to enable proper name resolution of
        the items during the execute phase of PS.
      */
      if (set_new_item_local_context(thd, item_ident_1, nj_col_1->table_ref) ||
          set_new_item_local_context(thd, item_ident_2, nj_col_2->table_ref))
        goto err;

      if (!(eq_cond= new (thd->mem_root) Item_func_eq(thd, item_ident_1, item_ident_2)))
        goto err;                               /* Out of memory. */

      /*
        Add the new equi-join condition to the ON clause. Notice that
        fix_fields() is applied to all ON conditions in setup_conds()
        so we don't do it here.
      */
      add_join_on(thd, (table_ref_1->outer_join & JOIN_TYPE_RIGHT ?
                        table_ref_1 : table_ref_2),
                  eq_cond);

      nj_col_1->is_common= nj_col_2->is_common= TRUE;
      DBUG_PRINT ("info", ("%s.%s and %s.%s are common", 
                           nj_col_1->safe_table_name(),
                           nj_col_1->name()->str,
                           nj_col_2->safe_table_name(),
                           nj_col_2->name()->str));

      if (field_1)
        update_field_dependencies(thd, field_1, field_1->table);
      if (field_2)
        update_field_dependencies(thd, field_2, field_2->table);

      if (using_fields != NULL)
        ++(*found_using_fields);
    }
  }
  if (leaf_1)
    leaf_1->is_join_columns_complete= TRUE;

  /*
    Everything is OK.
    Notice that at this point there may be some column names in the USING
    clause that are not among the common columns. This is an SQL error and
    we check for this error in store_natural_using_join_columns() when
    (found_using_fields < length(join_using_fields)).
  */
  result= FALSE;

err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(result);
}



/*
  Materialize and store the row type of NATURAL/USING join.

  SYNOPSIS
    store_natural_using_join_columns()
    thd                current thread
    natural_using_join the table reference of the NATURAL/USING join
    table_ref_1        the first (left) operand (of a NATURAL/USING join).
    table_ref_2        the second (right) operand (of a NATURAL/USING join).
    using_fields       if the join is JOIN...USING - the join columns,
                       if NATURAL join, then NULL
    found_using_fields number of fields from the USING clause that were
                       found among the common fields

  DESCRIPTION
    Iterate over the columns of both join operands and sort and store
    all columns into the 'join_columns' list of natural_using_join
    where the list is formed by three parts:
      part1: The coalesced columns of table_ref_1 and table_ref_2,
             sorted according to the column order of the first table.
      part2: The other columns of the first table, in the order in
             which they were defined in CREATE TABLE.
      part3: The other columns of the second table, in the order in
             which they were defined in CREATE TABLE.
    Time complexity - O(N1+N2), where Ni = length(table_ref_i).

  IMPLEMENTATION
    The procedure assumes that mark_common_columns() has been called
    for the join that is being processed.

  RETURN
    TRUE    error: Some common column is ambiguous
    FALSE   OK
*/

static bool
store_natural_using_join_columns(THD *thd, TABLE_LIST *natural_using_join,
                                 TABLE_LIST *table_ref_1,
                                 TABLE_LIST *table_ref_2,
                                 List<String> *using_fields,
                                 uint found_using_fields)
{
  Field_iterator_table_ref it_1, it_2;
  Natural_join_column *nj_col_1, *nj_col_2;
  Query_arena *arena, backup;
  bool result= TRUE;
  List<Natural_join_column> *non_join_columns;
  List<Natural_join_column> *join_columns;
  DBUG_ENTER("store_natural_using_join_columns");

  DBUG_ASSERT(!natural_using_join->join_columns);

  arena= thd->activate_stmt_arena_if_needed(&backup);

  if (!(non_join_columns= new List<Natural_join_column>) ||
      !(join_columns= new List<Natural_join_column>))
    goto err;

  /* Append the columns of the first join operand. */
  for (it_1.set(table_ref_1); !it_1.end_of_fields(); it_1.next())
  {
    nj_col_1= it_1.get_natural_column_ref();
    if (nj_col_1->is_common)
    {
      join_columns->push_back(nj_col_1, thd->mem_root);
      /* Reset the common columns for the next call to mark_common_columns. */
      nj_col_1->is_common= FALSE;
    }
    else
      non_join_columns->push_back(nj_col_1, thd->mem_root);
  }

  /*
    Check that all columns in the USING clause are among the common
    columns. If this is not the case, report the first one that was
    not found in an error.
  */
  if (using_fields && found_using_fields < using_fields->elements)
  {
    String *using_field_name;
    List_iterator_fast<String> using_fields_it(*using_fields);
    while ((using_field_name= using_fields_it++))
    {
      const char *using_field_name_ptr= using_field_name->c_ptr();
      List_iterator_fast<Natural_join_column>
        it(*join_columns);
      Natural_join_column *common_field;

      for (;;)
      {
        /* If reached the end of fields, and none was found, report error. */
        if (!(common_field= it++))
        {
          my_error(ER_BAD_FIELD_ERROR, MYF(0), using_field_name_ptr,
                   current_thd->where);
          goto err;
        }
        if (!my_strcasecmp(system_charset_info,
                           common_field->name()->str, using_field_name_ptr))
          break;                                // Found match
      }
    }
  }

  /* Append the non-equi-join columns of the second join operand. */
  for (it_2.set(table_ref_2); !it_2.end_of_fields(); it_2.next())
  {
    nj_col_2= it_2.get_natural_column_ref();
    if (!nj_col_2->is_common)
      non_join_columns->push_back(nj_col_2, thd->mem_root);
    else
    {
      /* Reset the common columns for the next call to mark_common_columns. */
      nj_col_2->is_common= FALSE;
    }
  }

  if (non_join_columns->elements > 0)
    join_columns->append(non_join_columns);
  natural_using_join->join_columns= join_columns;
  natural_using_join->is_join_columns_complete= TRUE;

  result= FALSE;

  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(result);

err:
  /*
     Actually we failed to build join columns list, so we have to
     clear it to avoid problems with half-build join on next run.
     The list was created in mark_common_columns().
   */
  table_ref_1->remove_join_columns();
  table_ref_2->remove_join_columns();

  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(TRUE);
}


/*
  Precompute and store the row types of the top-most NATURAL/USING joins.

  SYNOPSIS
    store_top_level_join_columns()
    thd            current thread
    table_ref      nested join or table in a FROM clause
    left_neighbor  neighbor table reference to the left of table_ref at the
                   same level in the join tree
    right_neighbor neighbor table reference to the right of table_ref at the
                   same level in the join tree

  DESCRIPTION
    The procedure performs a post-order traversal of a nested join tree
    and materializes the row types of NATURAL/USING joins in a
    bottom-up manner until it reaches the TABLE_LIST elements that
    represent the top-most NATURAL/USING joins. The procedure should be
    applied to each element of SELECT_LEX::top_join_list (i.e. to each
    top-level element of the FROM clause).

  IMPLEMENTATION
    Notice that the table references in the list nested_join->join_list
    are in reverse order, thus when we iterate over it, we are moving
    from the right to the left in the FROM clause.

  RETURN
    TRUE   Error
    FALSE  OK
*/

static bool
store_top_level_join_columns(THD *thd, TABLE_LIST *table_ref,
                             TABLE_LIST *left_neighbor,
                             TABLE_LIST *right_neighbor)
{
  Query_arena *arena, backup;
  bool result= TRUE;

  DBUG_ENTER("store_top_level_join_columns");

  arena= thd->activate_stmt_arena_if_needed(&backup);

  /* Call the procedure recursively for each nested table reference. */
  if (table_ref->nested_join)
  {
    List_iterator_fast<TABLE_LIST> nested_it(table_ref->nested_join->join_list);
    TABLE_LIST *same_level_left_neighbor= nested_it++;
    TABLE_LIST *same_level_right_neighbor= NULL;
    /* Left/right-most neighbors, possibly at higher levels in the join tree. */
    TABLE_LIST *real_left_neighbor, *real_right_neighbor;

    while (same_level_left_neighbor)
    {
      TABLE_LIST *cur_table_ref= same_level_left_neighbor;
      same_level_left_neighbor= nested_it++;
      /*
        The order of RIGHT JOIN operands is reversed in 'join list' to
        transform it into a LEFT JOIN. However, in this procedure we need
        the join operands in their lexical order, so below we reverse the
        join operands. Notice that this happens only in the first loop,
        and not in the second one, as in the second loop
        same_level_left_neighbor == NULL.
        This is the correct behavior, because the second loop sets
        cur_table_ref reference correctly after the join operands are
        swapped in the first loop.
      */
      if (same_level_left_neighbor &&
          cur_table_ref->outer_join & JOIN_TYPE_RIGHT)
      {
        /* This can happen only for JOIN ... ON. */
        DBUG_ASSERT(table_ref->nested_join->join_list.elements == 2);
        swap_variables(TABLE_LIST*, same_level_left_neighbor, cur_table_ref);
      }

      /*
        Pick the parent's left and right neighbors if there are no immediate
        neighbors at the same level.
      */
      real_left_neighbor=  (same_level_left_neighbor) ?
                           same_level_left_neighbor : left_neighbor;
      real_right_neighbor= (same_level_right_neighbor) ?
                           same_level_right_neighbor : right_neighbor;

      if (cur_table_ref->nested_join &&
          store_top_level_join_columns(thd, cur_table_ref,
                                       real_left_neighbor, real_right_neighbor))
        goto err;
      same_level_right_neighbor= cur_table_ref;
    }
  }

  /*
    If this is a NATURAL/USING join, materialize its result columns and
    convert to a JOIN ... ON.
  */
  if (table_ref->is_natural_join)
  {
    DBUG_ASSERT(table_ref->nested_join &&
                table_ref->nested_join->join_list.elements == 2);
    List_iterator_fast<TABLE_LIST> operand_it(table_ref->nested_join->join_list);
    /*
      Notice that the order of join operands depends on whether table_ref
      represents a LEFT or a RIGHT join. In a RIGHT join, the operands are
      in inverted order.
     */
    TABLE_LIST *table_ref_2= operand_it++; /* Second NATURAL join operand.*/
    TABLE_LIST *table_ref_1= operand_it++; /* First NATURAL join operand. */
    List<String> *using_fields= table_ref->join_using_fields;
    uint found_using_fields;

    /*
      The two join operands were interchanged in the parser, change the order
      back for 'mark_common_columns'.
    */
    if (table_ref_2->outer_join & JOIN_TYPE_RIGHT)
      swap_variables(TABLE_LIST*, table_ref_1, table_ref_2);
    if (mark_common_columns(thd, table_ref_1, table_ref_2,
                            using_fields, &found_using_fields))
      goto err;

    /*
      Swap the join operands back, so that we pick the columns of the second
      one as the coalesced columns. In this way the coalesced columns are the
      same as of an equivalent LEFT JOIN.
    */
    if (table_ref_1->outer_join & JOIN_TYPE_RIGHT)
      swap_variables(TABLE_LIST*, table_ref_1, table_ref_2);
    if (store_natural_using_join_columns(thd, table_ref, table_ref_1,
                                         table_ref_2, using_fields,
                                         found_using_fields))
      goto err;

    /*
      Change NATURAL JOIN to JOIN ... ON. We do this for both operands
      because either one of them or the other is the one with the
      natural join flag because RIGHT joins are transformed into LEFT,
      and the two tables may be reordered.
    */
    table_ref_1->natural_join= table_ref_2->natural_join= NULL;

    /* Add a TRUE condition to outer joins that have no common columns. */
    if (table_ref_2->outer_join &&
        !table_ref_1->on_expr && !table_ref_2->on_expr)
      table_ref_2->on_expr= new (thd->mem_root) Item_int(thd, (longlong) 1, 1); // Always true.

    /* Change this table reference to become a leaf for name resolution. */
    if (left_neighbor)
    {
      TABLE_LIST *last_leaf_on_the_left;
      last_leaf_on_the_left= left_neighbor->last_leaf_for_name_resolution();
      last_leaf_on_the_left->next_name_resolution_table= table_ref;
    }
    if (right_neighbor)
    {
      TABLE_LIST *first_leaf_on_the_right;
      first_leaf_on_the_right= right_neighbor->first_leaf_for_name_resolution();
      table_ref->next_name_resolution_table= first_leaf_on_the_right;
    }
    else
      table_ref->next_name_resolution_table= NULL;
  }
  result= FALSE; /* All is OK. */

err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(result);
}


/*
  Compute and store the row types of the top-most NATURAL/USING joins
  in a FROM clause.

  SYNOPSIS
    setup_natural_join_row_types()
    thd          current thread
    from_clause  list of top-level table references in a FROM clause

  DESCRIPTION
    Apply the procedure 'store_top_level_join_columns' to each of the
    top-level table referencs of the FROM clause. Adjust the list of tables
    for name resolution - context->first_name_resolution_table to the
    top-most, lef-most NATURAL/USING join.

  IMPLEMENTATION
    Notice that the table references in 'from_clause' are in reverse
    order, thus when we iterate over it, we are moving from the right
    to the left in the FROM clause.

  NOTES
    We can't run this many times as the first_name_resolution_table would
    be different for subsequent runs when sub queries has been optimized
    away.

  RETURN
    TRUE   Error
    FALSE  OK
*/

static bool setup_natural_join_row_types(THD *thd,
                                         List<TABLE_LIST> *from_clause,
                                         Name_resolution_context *context)
{
  DBUG_ENTER("setup_natural_join_row_types");
  thd->where= "from clause";
  if (from_clause->elements == 0)
    DBUG_RETURN(false); /* We come here in the case of UNIONs. */

  /* 
     Do not redo work if already done:
     1) for stored procedures,
     2) for multitable update after lock failure and table reopening.
  */
  if (!context->select_lex->first_natural_join_processing)
  {
    context->first_name_resolution_table= context->natural_join_first_table;
    DBUG_PRINT("info", ("using cached setup_natural_join_row_types"));
    DBUG_RETURN(false);
  }

  List_iterator_fast<TABLE_LIST> table_ref_it(*from_clause);
  TABLE_LIST *table_ref; /* Current table reference. */
  /* Table reference to the left of the current. */
  TABLE_LIST *left_neighbor;
  /* Table reference to the right of the current. */
  TABLE_LIST *right_neighbor= NULL;

  /* Note that tables in the list are in reversed order */
  for (left_neighbor= table_ref_it++; left_neighbor ; )
  {
    table_ref= left_neighbor;
    do
    {
      left_neighbor= table_ref_it++;
    }
    while (left_neighbor && left_neighbor->sj_subq_pred);

    if (store_top_level_join_columns(thd, table_ref,
                                     left_neighbor, right_neighbor))
      DBUG_RETURN(true);
    if (left_neighbor)
    {
      TABLE_LIST *first_leaf_on_the_right;
      first_leaf_on_the_right= table_ref->first_leaf_for_name_resolution();
      left_neighbor->next_name_resolution_table= first_leaf_on_the_right;
    }
    right_neighbor= table_ref;
  }

  /*
    Store the top-most, left-most NATURAL/USING join, so that we start
    the search from that one instead of context->table_list. At this point
    right_neighbor points to the left-most top-level table reference in the
    FROM clause.
  */
  DBUG_ASSERT(right_neighbor);
  context->first_name_resolution_table=
    right_neighbor->first_leaf_for_name_resolution();
  /*
    This is only to ensure that first_name_resolution_table doesn't
    change on re-execution
  */
  context->natural_join_first_table= context->first_name_resolution_table;
  context->select_lex->first_natural_join_processing= false;
  DBUG_RETURN (false);
}


/****************************************************************************
** Expand all '*' in given fields
****************************************************************************/

int setup_wild(THD *thd, TABLE_LIST *tables, List<Item> &fields,
	       List<Item> *sum_func_list,
	       uint wild_num, uint *hidden_bit_fields)
{
  if (!wild_num)
    return(0);

  Item *item;
  List_iterator<Item> it(fields);
  Query_arena *arena, backup;
  DBUG_ENTER("setup_wild");

  /*
    Don't use arena if we are not in prepared statements or stored procedures
    For PS/SP we have to use arena to remember the changes
  */
  arena= thd->activate_stmt_arena_if_needed(&backup);

  thd->lex->current_select->cur_pos_in_select_list= 0;
  while (wild_num && (item= it++))
  {
    if (item->type() == Item::FIELD_ITEM &&
        ((Item_field*) item)->field_name.str == star_clex_str.str &&
	!((Item_field*) item)->field)
    {
      uint elem= fields.elements;
      bool any_privileges= ((Item_field *) item)->any_privileges;
      Item_subselect *subsel= thd->lex->current_select->master_unit()->item;
      if (subsel &&
          subsel->substype() == Item_subselect::EXISTS_SUBS)
      {
        /*
          It is EXISTS(SELECT * ...) and we can replace * by any constant.

          Item_int do not need fix_fields() because it is basic constant.
        */
        it.replace(new (thd->mem_root) Item_int(thd, "Not_used", (longlong) 1,
                                MY_INT64_NUM_DECIMAL_DIGITS));
      }
      else if (insert_fields(thd, ((Item_field*) item)->context,
                             ((Item_field*) item)->db_name,
                             ((Item_field*) item)->table_name, &it,
                             any_privileges, hidden_bit_fields))
      {
	if (arena)
	  thd->restore_active_arena(arena, &backup);
	DBUG_RETURN(-1);
      }
      if (sum_func_list)
      {
	/*
	  sum_func_list is a list that has the fields list as a tail.
	  Because of this we have to update the element count also for this
	  list after expanding the '*' entry.
	*/
	sum_func_list->elements+= fields.elements - elem;
      }
      wild_num--;
    }
    else
      thd->lex->current_select->cur_pos_in_select_list++;
  }
  thd->lex->current_select->cur_pos_in_select_list= UNDEF_POS;
  if (arena)
  {
    /* make * substituting permanent */
    SELECT_LEX *select_lex= thd->lex->current_select;
    select_lex->with_wild= 0;
#ifdef HAVE_valgrind
    if (&select_lex->item_list != &fields)      // Avoid warning
#endif
    /*   
      The assignment below is translated to memcpy() call (at least on some
      platforms). memcpy() expects that source and destination areas do not
      overlap. That problem was detected by valgrind. 
    */
    if (&select_lex->item_list != &fields)
      select_lex->item_list= fields;

    thd->restore_active_arena(arena, &backup);
  }
  DBUG_RETURN(0);
}

/****************************************************************************
** Check that all given fields exists and fill struct with current data
****************************************************************************/

bool setup_fields(THD *thd, Ref_ptr_array ref_pointer_array,
                  List<Item> &fields, enum_column_usage column_usage,
                  List<Item> *sum_func_list, List<Item> *pre_fix,
                  bool allow_sum_func)
{
  Item *item;
  enum_column_usage saved_column_usage= thd->column_usage;
  nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
  List_iterator<Item> it(fields);
  bool save_is_item_list_lookup;
  bool make_pre_fix= (pre_fix && (pre_fix->elements == 0));
  DBUG_ENTER("setup_fields");
  DBUG_PRINT("enter", ("ref_pointer_array: %p", ref_pointer_array.array()));

  thd->column_usage= column_usage;
  DBUG_PRINT("info", ("thd->column_usage: %d", thd->column_usage));
  if (allow_sum_func)
    thd->lex->allow_sum_func.set_bit(thd->lex->current_select->nest_level);
  thd->where= THD::DEFAULT_WHERE;
  save_is_item_list_lookup= thd->lex->current_select->is_item_list_lookup;
  thd->lex->current_select->is_item_list_lookup= 0;

  /*
    To prevent fail on forward lookup we fill it with zeroes,
    then if we got pointer on zero after find_item_in_list we will know
    that it is forward lookup.

    There is other way to solve problem: fill array with pointers to list,
    but it will be slower.

    TODO: remove it when (if) we made one list for allfields and
    ref_pointer_array
  */
  if (!ref_pointer_array.is_null())
  {
    DBUG_ASSERT(ref_pointer_array.size() >= fields.elements);
    memset(ref_pointer_array.array(), 0, sizeof(Item *) * fields.elements);
  }

  /*
    We call set_entry() there (before fix_fields() of the whole list of field
    items) because:
    1) the list of field items has same order as in the query, and the
       Item_func_get_user_var item may go before the Item_func_set_user_var:
          SELECT @a, @a := 10 FROM t;
    2) The entry->update_query_id value controls constantness of
       Item_func_get_user_var items, so in presence of Item_func_set_user_var
       items we have to refresh their entries before fixing of
       Item_func_get_user_var items.
  */
  List_iterator<Item_func_set_user_var> li(thd->lex->set_var_list);
  Item_func_set_user_var *var;
  while ((var= li++))
    var->set_entry(thd, FALSE);

  Ref_ptr_array ref= ref_pointer_array;
  thd->lex->current_select->cur_pos_in_select_list= 0;
  while ((item= it++))
  {
    if (make_pre_fix)
      pre_fix->push_back(item, thd->stmt_arena->mem_root);

    if (item->fix_fields_if_needed_for_scalar(thd, it.ref()))
    {
      thd->lex->current_select->is_item_list_lookup= save_is_item_list_lookup;
      thd->lex->allow_sum_func= save_allow_sum_func;
      thd->column_usage= saved_column_usage;
      DBUG_PRINT("info", ("thd->column_usage: %d", thd->column_usage));
      DBUG_RETURN(TRUE); /* purecov: inspected */
    }
    item= *(it.ref()); // Item might have changed in fix_fields()
    if (!ref.is_null())
    {
      ref[0]= item;
      ref.pop_front();
    }
    /*
      split_sum_func() must be called for Window Function items, see
      Item_window_func::split_sum_func.
    */
    if (sum_func_list &&
         ((item->with_sum_func() && item->type() != Item::SUM_FUNC_ITEM) ||
          item->with_window_func))
    {
      item->split_sum_func(thd, ref_pointer_array, *sum_func_list,
                           SPLIT_SUM_SELECT);
    }
    thd->lex->current_select->select_list_tables|= item->used_tables();
    thd->lex->used_tables|= item->used_tables();
    thd->lex->current_select->cur_pos_in_select_list++;
  }
  thd->lex->current_select->is_item_list_lookup= save_is_item_list_lookup;
  thd->lex->current_select->cur_pos_in_select_list= UNDEF_POS;

  thd->lex->allow_sum_func= save_allow_sum_func;
  thd->column_usage= saved_column_usage;
  DBUG_PRINT("info", ("thd->column_usage: %d", thd->column_usage));
  DBUG_RETURN(MY_TEST(thd->is_error()));
}


/*
  make list of leaves of join table tree

  SYNOPSIS
    make_leaves_list()
    list    pointer to pointer on list first element
    tables  table list
    full_table_list whether to include tables from mergeable derived table/view.
                    we need them for checks for INSERT/UPDATE statements only.

  RETURN pointer on pointer to next_leaf of last element
*/

void make_leaves_list(THD *thd, List<TABLE_LIST> &list, TABLE_LIST *tables,
                      bool full_table_list, TABLE_LIST *boundary)
 
{
  for (TABLE_LIST *table= tables; table; table= table->next_local)
  {
    if (table == boundary)
      full_table_list= !full_table_list;
    if (full_table_list && table->is_merged_derived())
    {
      SELECT_LEX *select_lex= table->get_single_select();
      /*
        It's safe to use select_lex->leaf_tables because all derived
        tables/views were already prepared and has their leaf_tables
        set properly.
      */
      make_leaves_list(thd, list, select_lex->get_table_list(),
      full_table_list, boundary);
    }
    else
    {
      list.push_back(table, thd->mem_root);
    }
  }
}

/*
  prepare tables

  SYNOPSIS
    setup_tables()
    thd		  Thread handler
    context       name resolution contest to setup table list there
    from_clause   Top-level list of table references in the FROM clause
    tables	  Table list (select_lex->table_list)
    leaves        List of join table leaves list (select_lex->leaf_tables)
    refresh       It is only refresh for subquery
    select_insert It is SELECT ... INSERT command
    full_table_list a parameter to pass to the make_leaves_list function

  NOTE
    Check also that the 'used keys' and 'ignored keys' exists and set up the
    table structure accordingly.
    Create a list of leaf tables. For queries with NATURAL/USING JOINs,
    compute the row types of the top most natural/using join table references
    and link these into a list of table references for name resolution.

    This has to be called for all tables that are used by items, as otherwise
    table->map is not set and all Item_field will be regarded as const items.

  RETURN
    FALSE ok;  In this case *map will includes the chosen index
    TRUE  error
*/

bool setup_tables(THD *thd, Name_resolution_context *context,
                  List<TABLE_LIST> *from_clause, TABLE_LIST *tables,
                  List<TABLE_LIST> &leaves, bool select_insert,
                  bool full_table_list)
{
  uint tablenr= 0;
  List_iterator<TABLE_LIST> ti(leaves);
  TABLE_LIST *table_list;

  DBUG_ENTER("setup_tables");

  DBUG_ASSERT ((select_insert && !tables->next_name_resolution_table) || !tables || 
               (context->table_list && context->first_name_resolution_table));
  /*
    this is used for INSERT ... SELECT.
    For select we setup tables except first (and its underlying tables)
  */
  TABLE_LIST *first_select_table= (select_insert ?
                                   tables->next_local:
                                   0);
  SELECT_LEX *select_lex= select_insert ? thd->lex->first_select_lex() :
                                          thd->lex->current_select;
  if (select_lex->first_cond_optimization)
  {
    leaves.empty();
    if (select_lex->prep_leaf_list_state != SELECT_LEX::SAVED)
    {
      make_leaves_list(thd, leaves, tables, full_table_list, first_select_table);
      select_lex->prep_leaf_list_state= SELECT_LEX::READY;
      select_lex->leaf_tables_exec.empty();
    }
    else
    {
      List_iterator_fast <TABLE_LIST> ti(select_lex->leaf_tables_prep);
      while ((table_list= ti++))
        leaves.push_back(table_list, thd->mem_root);
    }
      
    while ((table_list= ti++))
    {
      TABLE *table= table_list->table;
      if (table)
        table->pos_in_table_list= table_list;
      if (first_select_table &&
          table_list->top_table() == first_select_table)
      {
        /* new counting for SELECT of INSERT ... SELECT command */
        first_select_table= 0;
        thd->lex->first_select_lex()->insert_tables= tablenr;
        tablenr= 0;
      }
      if(table_list->jtbm_subselect)
      {
        table_list->jtbm_table_no= tablenr;
      }
      else if (table)
      {
        table->pos_in_table_list= table_list;
        setup_table_map(table, table_list, tablenr);

        if (table_list->process_index_hints(table))
          DBUG_RETURN(1);
      }
      tablenr++;
    }
    if (tablenr > MAX_TABLES)
    {
      my_error(ER_TOO_MANY_TABLES,MYF(0), static_cast<int>(MAX_TABLES));
      DBUG_RETURN(1);
    }
  }
  else
  { 
    List_iterator_fast <TABLE_LIST> ti(select_lex->leaf_tables_exec);
    select_lex->leaf_tables.empty();
    while ((table_list= ti++))
    {
      if(table_list->jtbm_subselect)
      {
        table_list->jtbm_table_no= table_list->tablenr_exec;
      }
      else
      {
        table_list->table->tablenr= table_list->tablenr_exec;
        table_list->table->map= table_list->map_exec;
        table_list->table->maybe_null= table_list->maybe_null_exec;
        table_list->table->pos_in_table_list= table_list;
        if (table_list->process_index_hints(table_list->table))
          DBUG_RETURN(1);
      }
      select_lex->leaf_tables.push_back(table_list);
    }
  }    

  for (table_list= tables;
       table_list;
       table_list= table_list->next_local)
  {
    if (table_list->merge_underlying_list)
    {
      DBUG_ASSERT(table_list->is_merged_derived());
      Query_arena *arena, backup;
      arena= thd->activate_stmt_arena_if_needed(&backup);
      bool res;
      res= table_list->setup_underlying(thd);
      if (arena)
        thd->restore_active_arena(arena, &backup);
      if (res)
        DBUG_RETURN(1);
    }

    if (table_list->jtbm_subselect)
    {
      Item *item= table_list->jtbm_subselect->optimizer;
      if (table_list->jtbm_subselect->optimizer->fix_fields(thd, &item))
      {
        my_error(ER_TOO_MANY_TABLES,MYF(0), static_cast<int>(MAX_TABLES)); /* psergey-todo: WHY ER_TOO_MANY_TABLES ???*/
        DBUG_RETURN(1);
      }
      DBUG_ASSERT(item == table_list->jtbm_subselect->optimizer);
    }
  }

  /* Precompute and store the row types of NATURAL/USING joins. */
  if (setup_natural_join_row_types(thd, from_clause, context))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/*
  prepare tables and check access for the view tables

  SYNOPSIS
    setup_tables_and_check_access()
    thd		  Thread handler
    context       name resolution contest to setup table list there
    from_clause   Top-level list of table references in the FROM clause
    tables	  Table list (select_lex->table_list)
    conds	  Condition of current SELECT (can be changed by VIEW)
    leaves        List of join table leaves list (select_lex->leaf_tables)
    refresh       It is onle refresh for subquery
    select_insert It is SELECT ... INSERT command
    want_access   what access is needed
    full_table_list a parameter to pass to the make_leaves_list function

  NOTE
    a wrapper for check_tables that will also check the resulting
    table leaves list for access to all the tables that belong to a view

  RETURN
    FALSE ok;  In this case *map will include the chosen index
    TRUE  error
*/
bool setup_tables_and_check_access(THD *thd, 
                                   Name_resolution_context *context,
                                   List<TABLE_LIST> *from_clause,
                                   TABLE_LIST *tables,
                                   List<TABLE_LIST> &leaves,
                                   bool select_insert,
                                   ulong want_access_first,
                                   ulong want_access,
                                   bool full_table_list)
{
  DBUG_ENTER("setup_tables_and_check_access");

  if (setup_tables(thd, context, from_clause, tables,
                   leaves, select_insert, full_table_list))
    DBUG_RETURN(TRUE);

  List_iterator<TABLE_LIST> ti(leaves);
  TABLE_LIST *table_list;
  ulong access= want_access_first;
  while ((table_list= ti++))
  {
    if (table_list->belong_to_view && !table_list->view && 
        check_single_table_access(thd, access, table_list, FALSE))
    {
      tables->hide_view_error(thd);
      DBUG_RETURN(TRUE);
    }
    access= want_access;
  }
  DBUG_RETURN(FALSE);
}


/*
   Create a key_map from a list of index names

   SYNOPSIS
     get_key_map_from_key_list()
     map		key_map to fill in
     table		Table
     index_list		List of index names

   RETURN
     0	ok;  In this case *map will includes the choosed index
     1	error
*/

bool get_key_map_from_key_list(key_map *map, TABLE *table,
                               List<String> *index_list)
{
  List_iterator_fast<String> it(*index_list);
  String *name;
  uint pos;

  map->clear_all();
  while ((name=it++))
  {
    if (table->s->keynames.type_names == 0 ||
        (pos= find_type(&table->s->keynames, name->ptr(),
                        name->length(), 1)) <=
        0)
    {
      my_error(ER_KEY_DOES_NOT_EXITS, MYF(0), name->c_ptr(),
	       table->pos_in_table_list->alias.str);
      map->set_all();
      return 1;
    }
    map->set_bit(pos-1);
  }
  return 0;
}


/*
  Drops in all fields instead of current '*' field

  SYNOPSIS
    insert_fields()
    thd			Thread handler
    context             Context for name resolution
    db_name		Database name in case of 'database_name.table_name.*'
    table_name		Table name in case of 'table_name.*'
    it			Pointer to '*'
    any_privileges	0 If we should ensure that we have SELECT privileges
		          for all columns
                        1 If any privilege is ok
  RETURN
    0	ok     'it' is updated to point at last inserted
    1	error.  Error message is generated but not sent to client
*/

bool
insert_fields(THD *thd, Name_resolution_context *context, const char *db_name,
	      const char *table_name, List_iterator<Item> *it,
              bool any_privileges, uint *hidden_bit_fields)
{
  Field_iterator_table_ref field_iterator;
  bool found;
  char name_buff[SAFE_NAME_LEN+1];
  DBUG_ENTER("insert_fields");
  DBUG_PRINT("arena", ("stmt arena: %p",thd->stmt_arena));

  if (db_name && lower_case_table_names)
  {
    /*
      convert database to lower case for comparison
      We can't do this in Item_field as this would change the
      'name' of the item which may be used in the select list
    */
    strmake_buf(name_buff, db_name);
    my_casedn_str(files_charset_info, name_buff);
    db_name= name_buff;
  }

  found= FALSE;

  /*
    If table names are qualified, then loop over all tables used in the query,
    else treat natural joins as leaves and do not iterate over their underlying
    tables.
  */
  for (TABLE_LIST *tables= (table_name ? context->table_list :
                            context->first_name_resolution_table);
       tables;
       tables= (table_name ? tables->next_local :
                tables->next_name_resolution_table)
       )
  {
    Field *field;
    TABLE *table= tables->table;

    DBUG_ASSERT(tables->is_leaf_for_name_resolution());

    if ((table_name && my_strcasecmp(table_alias_charset, table_name,
                                     tables->alias.str)) ||
        (db_name && strcmp(tables->db.str, db_name)))
      continue;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /* 
       Ensure that we have access rights to all fields to be inserted. Under
       some circumstances, this check may be skipped.

       - If any_privileges is true, skip the check.

       - If the SELECT privilege has been found as fulfilled already for both
         the TABLE and TABLE_LIST objects (and both of these exist, of
         course), the check is skipped.

       - If the SELECT privilege has been found fulfilled for the TABLE object
         and the TABLE_LIST represents a derived table other than a view (see
         below), the check is skipped.

       - If the TABLE_LIST object represents a view, we may skip checking if
         the SELECT privilege has been found fulfilled for it, regardless of
         the TABLE object.

       - If there is no TABLE object, the test is skipped if either 
         * the TABLE_LIST does not represent a view, or
         * the SELECT privilege has been found fulfilled.         

       A TABLE_LIST that is not a view may be a subquery, an
       information_schema table, or a nested table reference. See the comment
       for TABLE_LIST.
    */
    if (!((table && tables->is_non_derived() &&
          (table->grant.privilege & SELECT_ACL)) ||
	  ((!tables->is_non_derived() && 
	    (tables->grant.privilege & SELECT_ACL)))) &&
        !any_privileges)
    {
      field_iterator.set(tables);
      if (check_grant_all_columns(thd, SELECT_ACL, &field_iterator))
        DBUG_RETURN(TRUE);
    }
#endif

    /*
      Update the tables used in the query based on the referenced fields. For
      views and natural joins this update is performed inside the loop below.
    */
    if (table)
    {
      thd->lex->used_tables|= table->map;
      thd->lex->current_select->select_list_tables|= table->map;
    }

    /*
      Initialize a generic field iterator for the current table reference.
      Notice that it is guaranteed that this iterator will iterate over the
      fields of a single table reference, because 'tables' is a leaf (for
      name resolution purposes).
    */
    field_iterator.set(tables);

    for (; !field_iterator.end_of_fields(); field_iterator.next())
    {
      /*
        field() is always NULL for views (see, e.g. Field_iterator_view or
        Field_iterator_natural_join).
        But view fields can never be invisible.
      */
      if ((field= field_iterator.field()) && field->invisible != VISIBLE)
        continue;

      Item *item;

      if (!(item= field_iterator.create_item(thd)))
        DBUG_RETURN(TRUE);

      /* cache the table for the Item_fields inserted by expanding stars */
      if (item->type() == Item::FIELD_ITEM && tables->cacheable_table)
        ((Item_field *)item)->cached_table= tables;

      if (!found)
      {
        found= TRUE;
        it->replace(item); /* Replace '*' with the first found item. */
      }
      else
        it->after(item);   /* Add 'item' to the SELECT list. */

      if (item->type() == Item::FIELD_ITEM && item->field_type() == MYSQL_TYPE_BIT)
        (*hidden_bit_fields)++;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
      /*
        Set privilege information for the fields of newly created views.
        We have that (any_priviliges == TRUE) if and only if we are creating
        a view. In the time of view creation we can't use the MERGE algorithm,
        therefore if 'tables' is itself a view, it is represented by a
        temporary table. Thus in this case we can be sure that 'item' is an
        Item_field.
      */
      if (any_privileges && !tables->is_with_table() && !tables->is_derived())
      {
        DBUG_ASSERT((tables->field_translation == NULL && table) ||
                    tables->is_natural_join);
        DBUG_ASSERT(item->type() == Item::FIELD_ITEM);
        Item_field *fld= (Item_field*) item;
        const char *field_table_name= field_iterator.get_table_name();

        if (!tables->schema_table && 
            !(fld->have_privileges=
              (get_column_grant(thd, field_iterator.grant(),
                                field_iterator.get_db_name(),
                                field_table_name, fld->field_name.str) &
               VIEW_ANY_ACL)))
        {
          my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), "ANY",
                   thd->security_ctx->priv_user,
                   thd->security_ctx->host_or_ip,
                   field_table_name);
          DBUG_RETURN(TRUE);
        }
      }
#endif

      if ((field= field_iterator.field()))
      {
        field->table->mark_column_with_deps(field);
        if (table)
          table->covering_keys.intersect(field->part_of_key);
        if (tables->is_natural_join)
        {
          TABLE *field_table;
          /*
            In this case we are sure that the column ref will not be created
            because it was already created and stored with the natural join.
          */
          Natural_join_column *nj_col;
          if (!(nj_col= field_iterator.get_natural_column_ref()))
            DBUG_RETURN(TRUE);
          DBUG_ASSERT(nj_col->table_field);
          field_table= nj_col->table_ref->table;
          if (field_table)
          {
            thd->lex->used_tables|= field_table->map;
            thd->lex->current_select->select_list_tables|=
              field_table->map;
            field_table->covering_keys.intersect(field->part_of_key);
            field_table->used_fields++;
          }
        }
      }
      else
        thd->lex->used_tables|= item->used_tables();
      thd->lex->current_select->cur_pos_in_select_list++;
    }
    /*
      In case of stored tables, all fields are considered as used,
      while in the case of views, the fields considered as used are the
      ones marked in setup_tables during fix_fields of view columns.
      For NATURAL joins, used_tables is updated in the IF above.
    */
    if (table)
      table->used_fields= table->s->fields;
  }
  if (found)
    DBUG_RETURN(FALSE);

  /*
    TODO: in the case when we skipped all columns because there was a
    qualified '*', and all columns were coalesced, we have to give a more
    meaningful message than ER_BAD_TABLE_ERROR.
  */
  if (!table_name)
    my_error(ER_NO_TABLES_USED, MYF(0));
  else if (!db_name && !thd->db.str)
    my_error(ER_NO_DB_ERROR, MYF(0));
  else
  {
    char name[FN_REFLEN];
    my_snprintf(name, sizeof(name), "%s.%s",
                db_name ? db_name : thd->get_db(), table_name);
    my_error(ER_BAD_TABLE_ERROR, MYF(0), name);
  }

  DBUG_RETURN(TRUE);
}


/**
  Wrap Item_ident

  @param thd             thread handle
  @param conds           pointer to the condition which should be wrapped
*/

void wrap_ident(THD *thd, Item **conds)
{
  Item_direct_ref_to_ident *wrapper;
  DBUG_ASSERT((*conds)->type() == Item::FIELD_ITEM || (*conds)->type() == Item::REF_ITEM);
  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);
  if ((wrapper= new (thd->mem_root) Item_direct_ref_to_ident(thd, (Item_ident *) (*conds))))
    (*conds)= (Item*) wrapper;
  if (arena)
    thd->restore_active_arena(arena, &backup);
}

/**
  Prepare ON expression

  @param thd             Thread handle
  @param table           Pointer to table list
  @param is_update       Update flag

  @retval TRUE error.
  @retval FALSE OK.
*/

bool setup_on_expr(THD *thd, TABLE_LIST *table, bool is_update)
{
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return TRUE;				// Fatal error flag is set!
  for(; table; table= table->next_local)
  {
    TABLE_LIST *embedded; /* The table at the current level of nesting. */
    TABLE_LIST *embedding= table; /* The parent nested table reference. */
    do
    {
      embedded= embedding;
      if (embedded->on_expr)
      {
        thd->where="on clause";
        embedded->on_expr->mark_as_condition_AND_part(embedded);
        if (embedded->on_expr->fix_fields_if_needed_for_bool(thd,
                                                           &embedded->on_expr))
          return TRUE;
      }
      /*
        If it's a semi-join nest, fix its "left expression", as it is used by
        the SJ-Materialization
      */
      if (embedded->sj_subq_pred)
      {
        Item **left_expr= &embedded->sj_subq_pred->left_expr;
        if ((*left_expr)->fix_fields_if_needed(thd, left_expr))
          return TRUE;
      }

      embedding= embedded->embedding;
    }
    while (embedding &&
           embedding->nested_join->join_list.head() == embedded);

    if (table->is_merged_derived())
    {
      SELECT_LEX *select_lex= table->get_single_select();
      setup_on_expr(thd, select_lex->get_table_list(), is_update);
    }

    /* process CHECK OPTION */
    if (is_update)
    {
      TABLE_LIST *view= table->top_table();
      if (view->effective_with_check)
      {
        if (view->prepare_check_option(thd))
          return TRUE;
        thd->change_item_tree(&table->check_option, view->check_option);
      }
    }
  }
  return FALSE;
}

/*
  Fix all conditions and outer join expressions.

  SYNOPSIS
    setup_conds()
    thd     thread handler
    tables  list of tables for name resolving (select_lex->table_list)
    leaves  list of leaves of join table tree (select_lex->leaf_tables)
    conds   WHERE clause

  DESCRIPTION
    TODO

  RETURN
    TRUE  if some error occurred (e.g. out of memory)
    FALSE if all is OK
*/

int setup_conds(THD *thd, TABLE_LIST *tables, List<TABLE_LIST> &leaves,
                COND **conds)
{
  SELECT_LEX *select_lex= thd->lex->current_select;
  TABLE_LIST *table= NULL;	// For HP compilers
  /*
    it_is_update set to TRUE when tables of primary SELECT_LEX (SELECT_LEX
    which belong to LEX, i.e. most up SELECT) will be updated by
    INSERT/UPDATE/LOAD
    NOTE: using this condition helps to prevent call of prepare_check_option()
    from subquery of VIEW, because tables of subquery belongs to VIEW
    (see condition before prepare_check_option() call)
  */
  bool it_is_update= (select_lex == thd->lex->first_select_lex()) &&
    thd->lex->which_check_option_applicable();
  bool save_is_item_list_lookup= select_lex->is_item_list_lookup;
  TABLE_LIST *derived= select_lex->master_unit()->derived;
  DBUG_ENTER("setup_conds");

  select_lex->is_item_list_lookup= 0;

  thd->column_usage= MARK_COLUMNS_READ;
  DBUG_PRINT("info", ("thd->column_usage: %d", thd->column_usage));
  select_lex->cond_count= 0;
  select_lex->between_count= 0;
  select_lex->max_equal_elems= 0;

  for (table= tables; table; table= table->next_local)
  {
    if (select_lex == thd->lex->first_select_lex() &&
        select_lex->first_cond_optimization &&
        table->merged_for_insert &&
        table->prepare_where(thd, conds, FALSE))
      goto err_no_arena;
  }

  if (*conds)
  {
    thd->where="where clause";
    DBUG_EXECUTE("where",
                 print_where(*conds,
                             "WHERE in setup_conds",
                             QT_ORDINARY););
    /*
      Wrap alone field in WHERE clause in case it will be outer field of subquery
      which need persistent pointer on it, but conds could be changed by optimizer
    */
    if ((*conds)->type() == Item::FIELD_ITEM && !derived)
      wrap_ident(thd, conds);
    (*conds)->mark_as_condition_AND_part(NO_JOIN_NEST);
    if ((*conds)->fix_fields_if_needed_for_bool(thd, conds))
      goto err_no_arena;
  }

  /*
    Apply fix_fields() to all ON clauses at all levels of nesting,
    including the ones inside view definitions.
  */
  if (setup_on_expr(thd, tables, it_is_update))
    goto err_no_arena;

  if (!thd->stmt_arena->is_conventional())
  {
    /*
      We are in prepared statement preparation code => we should store
      WHERE clause changing for next executions.

      We do this ON -> WHERE transformation only once per PS/SP statement.
    */
    select_lex->where= *conds;
  }
  thd->lex->current_select->is_item_list_lookup= save_is_item_list_lookup;
  DBUG_RETURN(MY_TEST(thd->is_error()));

err_no_arena:
  select_lex->is_item_list_lookup= save_is_item_list_lookup;
  DBUG_RETURN(1);
}


/******************************************************************************
** Fill a record with data (for INSERT or UPDATE)
** Returns : 1 if some field has wrong type
******************************************************************************/


/**
  Fill the fields of a table with the values of an Item list

  @param thd           thread handler
  @param table_arg     the table that is being modified
  @param fields        Item_fields list to be filled
  @param values        values to fill with
  @param ignore_errors TRUE if we should ignore errors
  @param update        TRUE if update query

  @details
    fill_record() may set table->auto_increment_field_not_null and a
    caller should make sure that it is reset after their last call to this
    function.
    default functions are executed for inserts.
    virtual fields are always updated

  @return Status
  @retval true An error occurred.
  @retval false OK.
*/

bool
fill_record(THD *thd, TABLE *table_arg, List<Item> &fields, List<Item> &values,
            bool ignore_errors, bool update)
{
  List_iterator_fast<Item> f(fields),v(values);
  Item *value, *fld;
  Item_field *field;
  Field *rfield;
  TABLE *table;
  bool only_unvers_fields= update && table_arg->versioned();
  bool save_abort_on_warning= thd->abort_on_warning;
  bool save_no_errors= thd->no_errors;
  DBUG_ENTER("fill_record");

  thd->no_errors= ignore_errors;
  /*
    Reset the table->auto_increment_field_not_null as it is valid for
    only one row.
  */
  if (fields.elements)
    table_arg->auto_increment_field_not_null= FALSE;

  while ((fld= f++))
  {
    if (!(field= fld->field_for_view_update()))
    {
      my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), fld->name.str);
      goto err;
    }
    value=v++;
    DBUG_ASSERT(value);
    rfield= field->field;
    table= rfield->table;
    if (table->next_number_field &&
        rfield->field_index ==  table->next_number_field->field_index)
      table->auto_increment_field_not_null= TRUE;
    Item::Type type= value->type();
    bool vers_sys_field= table->versioned() && rfield->vers_sys_field();
    if ((rfield->vcol_info || vers_sys_field) &&
        type != Item::DEFAULT_VALUE_ITEM &&
        type != Item::NULL_ITEM &&
        table->s->table_category != TABLE_CATEGORY_TEMPORARY)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARNING_NON_DEFAULT_VALUE_FOR_GENERATED_COLUMN,
                          ER_THD(thd, ER_WARNING_NON_DEFAULT_VALUE_FOR_GENERATED_COLUMN),
                          rfield->field_name.str, table->s->table_name.str);
      if (vers_sys_field)
        continue;
    }
    if (only_unvers_fields && !rfield->vers_update_unversioned())
      only_unvers_fields= false;

    if (rfield->stored_in_db())
    {
      if (unlikely(value->save_in_field(rfield, 0) < 0) && !ignore_errors)
      {
        my_message(ER_UNKNOWN_ERROR, ER_THD(thd, ER_UNKNOWN_ERROR), MYF(0));
        goto err;
      }
      /*
        In sql MODE_SIMULTANEOUS_ASSIGNMENT,
        move field pointer on value stored in record[1]
        which contains row before update (see MDEV-13417)
      */
      if (update && thd->variables.sql_mode & MODE_SIMULTANEOUS_ASSIGNMENT)
        rfield->move_field_offset((my_ptrdiff_t) (table->record[1] -
                                                  table->record[0]));
    }
    rfield->set_explicit_default(value);
  }

  if (update && thd->variables.sql_mode & MODE_SIMULTANEOUS_ASSIGNMENT)
  {
    // restore fields pointers on record[0]
    f.rewind();
    while ((fld= f++))
    {
      rfield= fld->field_for_view_update()->field;
      if (rfield->stored_in_db())
      {
        table= rfield->table;
        rfield->move_field_offset((my_ptrdiff_t) (table->record[0] -
                                                  table->record[1]));
      }
    }
  }

  if (!update && table_arg->default_field &&
      table_arg->update_default_fields(0, ignore_errors))
    goto err;
  if (table_arg->versioned() && !only_unvers_fields)
    table_arg->vers_update_fields();
  /* Update virtual fields */
  if (table_arg->vfield &&
      table_arg->update_virtual_fields(table_arg->file, VCOL_UPDATE_FOR_WRITE))
    goto err;
  thd->abort_on_warning= save_abort_on_warning;
  thd->no_errors=        save_no_errors;
  DBUG_RETURN(thd->is_error());
err:
  DBUG_PRINT("error",("got error"));
  thd->abort_on_warning= save_abort_on_warning;
  thd->no_errors=        save_no_errors;
  if (fields.elements)
    table_arg->auto_increment_field_not_null= FALSE;
  DBUG_RETURN(TRUE);
}


/**
  Prepare Item_field's for fill_record_n_invoke_before_triggers()

  This means redirecting from table->field to
  table->field_to_fill(), if needed.
*/
void switch_to_nullable_trigger_fields(List<Item> &items, TABLE *table)
{
  Field** field= table->field_to_fill();

 /* True if we have NOT NULL fields and BEFORE triggers */
  if (field != table->field)
  {
    List_iterator_fast<Item> it(items);
    Item *item;

    while ((item= it++))
      item->walk(&Item::switch_to_nullable_fields_processor, 1, field);
    table->triggers->reset_extra_null_bitmap();
  }
}


/**
  Prepare Virtual fields and field with default expressions to use
  trigger fields

  This means redirecting from table->field to
  table->field_to_fill(), if needed.
*/

void switch_defaults_to_nullable_trigger_fields(TABLE *table)
{
  if (!table->default_field)
    return; // no defaults

  Field **trigger_field= table->field_to_fill();

 /* True if we have NOT NULL fields and BEFORE triggers */
  if (*trigger_field != *table->field)
  {
    for (Field **field_ptr= table->default_field; *field_ptr ; field_ptr++)
    {
      Field *field= (*field_ptr);
      field->default_value->expr->walk(&Item::switch_to_nullable_fields_processor, 1, trigger_field);
      *field_ptr= (trigger_field[field->field_index]);
    }
  }
}


/**
  Test NOT NULL constraint after BEFORE triggers
*/
static bool not_null_fields_have_null_values(TABLE *table)
{
  Field **orig_field= table->field;
  Field **filled_field= table->field_to_fill();

  if (filled_field != orig_field)
  {
    THD *thd=table->in_use;
    for (uint i=0; i < table->s->fields; i++)
    {
      Field *of= orig_field[i];
      Field *ff= filled_field[i];
      if (ff != of)
      {
        // copy after-update flags to of, copy before-update flags to ff
        swap_variables(uint32, of->flags, ff->flags);
        if (ff->is_real_null())
        {
          ff->set_notnull(); // for next row WHERE condition in UPDATE
          if (convert_null_to_field_value_or_error(of) || thd->is_error())
            return true;
        }
      }
    }
  }

  return false;
}

/**
  Fill fields in list with values from the list of items and invoke
  before triggers.

  @param thd           thread context
  @param table         the table that is being modified
  @param fields        Item_fields list to be filled
  @param values        values to fill with
  @param ignore_errors TRUE if we should ignore errors
  @param event         event type for triggers to be invoked

  @detail
    This function assumes that fields which values will be set and triggers
    to be invoked belong to the same table, and that TABLE::record[0] and
    record[1] buffers correspond to new and old versions of row respectively.

  @return Status
  @retval true An error occurred.
  @retval false OK.
*/

bool
fill_record_n_invoke_before_triggers(THD *thd, TABLE *table,
                                     List<Item> &fields,
                                     List<Item> &values, bool ignore_errors,
                                     enum trg_event_type event)
{
  int result;
  Table_triggers_list *triggers= table->triggers;

  result= fill_record(thd, table, fields, values, ignore_errors,
                      event == TRG_EVENT_UPDATE);

  if (!result && triggers)
  {
    if (triggers->process_triggers(thd, event, TRG_ACTION_BEFORE,
                                    TRUE) ||
        not_null_fields_have_null_values(table))
      return TRUE;

    /*
      Re-calculate virtual fields to cater for cases when base columns are
      updated by the triggers.
    */
    if (table->vfield && fields.elements)
    {
      Item *fld= (Item_field*) fields.head();
      Item_field *item_field= fld->field_for_view_update();
      if (item_field)
      {
        DBUG_ASSERT(table == item_field->field->table);
        result|= table->update_virtual_fields(table->file,
                                              VCOL_UPDATE_FOR_WRITE);
      }
    }
  }
  return result;
}


/**
  Fill the field buffer of a table with the values of an Item list
  All fields are given a value

  @param thd           thread handler
  @param table_arg     the table that is being modified
  @param ptr           pointer on pointer to record of fields
  @param values        values to fill with
  @param ignore_errors TRUE if we should ignore errors
  @param use_value     forces usage of value of the items instead of result

  @details
    fill_record() may set table->auto_increment_field_not_null and a
    caller should make sure that it is reset after their last call to this
    function.

  @return Status
  @retval true An error occurred.
  @retval false OK.
*/

bool
fill_record(THD *thd, TABLE *table, Field **ptr, List<Item> &values,
            bool ignore_errors, bool use_value)
{
  List_iterator_fast<Item> v(values);
  List<TABLE> tbl_list;
  bool all_fields_have_values= true;
  Item *value;
  Field *field;
  bool abort_on_warning_saved= thd->abort_on_warning;
  uint autoinc_index= table->next_number_field
                        ? table->next_number_field->field_index
                        : ~0U;
  DBUG_ENTER("fill_record");
  if (!*ptr)
  {
    /* No fields to update, quite strange!*/
    DBUG_RETURN(0);
  }

  /*
    On INSERT or UPDATE fields are checked to be from the same table,
    thus we safely can take table from the first field.
  */
  DBUG_ASSERT((*ptr)->table == table);

  /*
    Reset the table->auto_increment_field_not_null as it is valid for
    only one row.
  */
  table->auto_increment_field_not_null= FALSE;
  while ((field = *ptr++) && ! thd->is_error())
  {
    /* Ensure that all fields are from the same table */
    DBUG_ASSERT(field->table == table);

    if (unlikely(field->invisible))
    {
      all_fields_have_values= false;
      continue;
    }
    else
      value=v++;

    bool vers_sys_field= table->versioned() && field->vers_sys_field();

    if (field->field_index == autoinc_index)
      table->auto_increment_field_not_null= TRUE;
    if (unlikely(field->vcol_info) || (vers_sys_field && !ignore_errors))
    {
      Item::Type type= value->type();
      if (type != Item::DEFAULT_VALUE_ITEM &&
          type != Item::NULL_ITEM &&
          table->s->table_category != TABLE_CATEGORY_TEMPORARY)
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_WARNING_NON_DEFAULT_VALUE_FOR_GENERATED_COLUMN,
                            ER_THD(thd, ER_WARNING_NON_DEFAULT_VALUE_FOR_GENERATED_COLUMN),
                            field->field_name.str, table->s->table_name.str);
        if (vers_sys_field)
          continue;
      }
    }

    if (use_value)
      value->save_val(field);
    else
      if (value->save_in_field(field, 0) < 0)
        goto err;
    all_fields_have_values &= field->set_explicit_default(value);
  }
  if (!all_fields_have_values && table->default_field &&
      table->update_default_fields(0, ignore_errors))
    goto err;
  /* Update virtual fields */
  thd->abort_on_warning= FALSE;
  if (table->versioned())
    table->vers_update_fields();
  if (table->vfield &&
      table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE))
    goto err;
  thd->abort_on_warning= abort_on_warning_saved;
  DBUG_RETURN(thd->is_error());

err:
  thd->abort_on_warning= abort_on_warning_saved;
  table->auto_increment_field_not_null= FALSE;
  DBUG_RETURN(TRUE);
}


/*
  Fill fields in an array with values from the list of items and invoke
  before triggers.

  @param thd           thread context
  @param table         the table that is being modified
  @param ptr        the fields to be filled
  @param values        values to fill with
  @param ignore_errors TRUE if we should ignore errors
  @param event         event type for triggers to be invoked

  @detail
    This function assumes that fields which values will be set and triggers
    to be invoked belong to the same table, and that TABLE::record[0] and
    record[1] buffers correspond to new and old versions of row respectively.

  @return Status
  @retval true An error occurred.
  @retval false OK.
*/

bool
fill_record_n_invoke_before_triggers(THD *thd, TABLE *table, Field **ptr,
                                     List<Item> &values, bool ignore_errors,
                                     enum trg_event_type event)
{
  bool result;
  Table_triggers_list *triggers= table->triggers;

  result= fill_record(thd, table, ptr, values, ignore_errors, FALSE);

  if (!result && triggers && *ptr)
    result= triggers->process_triggers(thd, event, TRG_ACTION_BEFORE, TRUE) ||
            not_null_fields_have_null_values(table);
  /*
    Re-calculate virtual fields to cater for cases when base columns are
    updated by the triggers.
  */
  if (!result && triggers && *ptr)
  {
    DBUG_ASSERT(table == (*ptr)->table);
    if (table->vfield)
      result= table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE);
  }
  return result;

}


my_bool mysql_rm_tmp_tables(void)
{
  uint i, idx;
  char	filePath[FN_REFLEN], *tmpdir, filePathCopy[FN_REFLEN];
  MY_DIR *dirp;
  FILEINFO *file;
  TABLE_SHARE share;
  THD *thd;
  DBUG_ENTER("mysql_rm_tmp_tables");

  if (!(thd= new THD(0)))
    DBUG_RETURN(1);
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  for (i=0; i<=mysql_tmpdir_list.max; i++)
  {
    tmpdir=mysql_tmpdir_list.list[i];
    /* See if the directory exists */
    if (!(dirp = my_dir(tmpdir,MYF(MY_WME | MY_DONT_SORT))))
      continue;

    /* Remove all SQLxxx tables from directory */

    for (idx=0 ; idx < (uint) dirp->number_of_files ; idx++)
    {
      file=dirp->dir_entry+idx;

      if (!memcmp(file->name, tmp_file_prefix,
                  tmp_file_prefix_length))
      {
        char *ext= fn_ext(file->name);
        size_t ext_len= strlen(ext);
        size_t filePath_len= my_snprintf(filePath, sizeof(filePath),
                                       "%s%c%s", tmpdir, FN_LIBCHAR,
                                       file->name);
        if (!strcmp(reg_ext, ext))
        {
          handler *handler_file= 0;
          /* We should cut file extention before deleting of table */
          memcpy(filePathCopy, filePath, filePath_len - ext_len);
          filePathCopy[filePath_len - ext_len]= 0;
          init_tmp_table_share(thd, &share, "", 0, "", filePathCopy);
          if (!open_table_def(thd, &share) &&
              ((handler_file= get_new_handler(&share, thd->mem_root,
                                              share.db_type()))))
          {
            handler_file->ha_delete_table(filePathCopy);
            delete handler_file;
          }
          free_table_share(&share);
        }
        /*
          File can be already deleted by tmp_table.file->delete_table().
          So we hide error messages which happnes during deleting of these
          files(MYF(0)).
        */
        (void) mysql_file_delete(key_file_misc, filePath, MYF(0));
      }
    }
    my_dirend(dirp);
  }
  delete thd;
  DBUG_RETURN(0);
}


/*****************************************************************************
	unireg support functions
*****************************************************************************/

int setup_ftfuncs(SELECT_LEX *select_lex)
{
  List_iterator<Item_func_match> li(*(select_lex->ftfunc_list)),
                                 lj(*(select_lex->ftfunc_list));
  Item_func_match *ftf, *ftf2;

  while ((ftf=li++))
  {
    if (ftf->fix_index())
      return 1;
    lj.rewind();
    while ((ftf2=lj++) != ftf)
    {
      if (ftf->eq(ftf2,1) && !ftf2->master)
        ftf2->master=ftf;
    }
  }

  return 0;
}


void cleanup_ftfuncs(SELECT_LEX *select_lex)
{
  List_iterator<Item_func_match> li(*(select_lex->ftfunc_list)),
                                 lj(*(select_lex->ftfunc_list));
  Item_func_match *ftf;

  while ((ftf=li++))
  {
    ftf->cleanup();
  }
}


int init_ftfuncs(THD *thd, SELECT_LEX *select_lex, bool no_order)
{
  if (select_lex->ftfunc_list->elements)
  {
    List_iterator<Item_func_match> li(*(select_lex->ftfunc_list));
    Item_func_match *ifm;

    while ((ifm=li++))
      if (unlikely(!ifm->is_fixed()))
        /*
          it mean that clause where was FT function was removed, so we have
          to remove the function from the list.
        */
        li.remove();
      else if (ifm->init_search(thd, no_order))
	return 1;
  }
  return 0;
}


bool is_equal(const LEX_CSTRING *a, const LEX_CSTRING *b)
{
  return a->length == b->length && !strncmp(a->str, b->str, a->length);
}

/*
  Open and lock system tables for read.

  SYNOPSIS
    open_system_tables_for_read()
      thd         Thread context.
      table_list  List of tables to open.
      backup      Pointer to Open_tables_state instance where
                  information about currently open tables will be
                  saved, and from which will be restored when we will
                  end work with system tables.

  NOTES
    Thanks to restrictions which we put on opening and locking of
    system tables for writing, we can open and lock them for reading
    even when we already have some other tables open and locked.  One
    must call close_system_tables() to close systems tables opened
    with this call.

  NOTES
   In some situations we  use this function to open system tables for
   writing. It happens, for examples, with statistical tables when
   they are updated by an ANALYZE command. In these cases we should
   guarantee that system tables will not be deadlocked.

  RETURN
    FALSE   Success
    TRUE    Error
*/

bool
open_system_tables_for_read(THD *thd, TABLE_LIST *table_list,
                            Open_tables_backup *backup)
{
  Query_tables_list query_tables_list_backup;
  LEX *lex= thd->lex;
  DBUG_ENTER("open_system_tables_for_read");

  /*
    Besides using new Open_tables_state for opening system tables,
    we also have to backup and reset/and then restore part of LEX
    which is accessed by open_tables() in order to determine if
    prelocking is needed and what tables should be added for it.
    close_system_tables() doesn't require such treatment.
  */
  lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
  thd->reset_n_backup_open_tables_state(backup);
  thd->lex->sql_command= SQLCOM_SELECT;

  /*
    Only use MYSQL_LOCK_IGNORE_TIMEOUT for tables opened for read.
    This is to ensure that lock_wait_timeout is honored when trying
    to update stats tables.
  */
  if (open_and_lock_tables(thd, table_list, FALSE,
                           (MYSQL_OPEN_IGNORE_FLUSH |
                            (table_list->lock_type < TL_WRITE_ALLOW_WRITE ?
                             MYSQL_LOCK_IGNORE_TIMEOUT : 0))))
  {
    lex->restore_backup_query_tables_list(&query_tables_list_backup);
    thd->restore_backup_open_tables_state(backup);
    DBUG_RETURN(TRUE);
  }

  for (TABLE_LIST *tables= table_list; tables; tables= tables->next_global)
  {
    DBUG_ASSERT(tables->table->s->table_category == TABLE_CATEGORY_SYSTEM);
    tables->table->use_all_columns();
  }
  lex->restore_backup_query_tables_list(&query_tables_list_backup);

  DBUG_RETURN(FALSE);
}


/*
  Close system tables, opened with open_system_tables_for_read().

  SYNOPSIS
    close_system_tables()
      thd     Thread context
      backup  Pointer to Open_tables_backup instance which holds
              information about tables which were open before we
              decided to access system tables.
*/

void
close_system_tables(THD *thd, Open_tables_backup *backup)
{
  /*
    Inform the transaction handler that we are closing the
    system tables and we don't need the read view anymore.
  */
  for (TABLE *table= thd->open_tables ; table ; table= table->next)
    table->file->extra(HA_EXTRA_PREPARE_FOR_FORCED_CLOSE);

  close_thread_tables(thd);
  thd->restore_backup_open_tables_state(backup);
}


/**
  A helper function to close a mysql.* table opened
  in an auxiliary THD during bootstrap or in the main
  connection, when we know that there are no locks
  held by the connection due to a preceding implicit
  commit.

  We need this function since we'd like to not
  just close the system table, but also release
  the metadata lock on it.

  Note, that in LOCK TABLES mode this function
  does not release the metadata lock. But in this
  mode the table can be opened only if it is locked
  explicitly with LOCK TABLES.
*/

void
close_mysql_tables(THD *thd)
{
  if (! thd->in_sub_stmt)
    trans_commit_stmt(thd);
  close_thread_tables(thd);
  thd->mdl_context.release_transactional_locks();
}

/*
  Open and lock one system table for update.

  SYNOPSIS
    open_system_table_for_update()
      thd        Thread context.
      one_table  Table to open.

  NOTES
    Table opened with this call should closed using close_thread_tables().

  RETURN
    0	Error
    #	Pointer to TABLE object of system table
*/

TABLE *
open_system_table_for_update(THD *thd, TABLE_LIST *one_table)
{
  DBUG_ENTER("open_system_table_for_update");

  TABLE *table= open_ltable(thd, one_table, one_table->lock_type,
                            MYSQL_LOCK_IGNORE_TIMEOUT);
  if (table)
  {
    DBUG_ASSERT(table->s->table_category == TABLE_CATEGORY_SYSTEM);
    table->use_all_columns();
  }

  DBUG_RETURN(table);
}

/**
  Open a log table.
  Opening such tables is performed internally in the server
  implementation, and is a 'nested' open, since some tables
  might be already opened by the current thread.
  The thread context before this call is saved, and is restored
  when calling close_log_table().
  @param thd The current thread
  @param one_table Log table to open
  @param backup [out] Temporary storage used to save the thread context
*/
TABLE *
open_log_table(THD *thd, TABLE_LIST *one_table, Open_tables_backup *backup)
{
  uint flags= ( MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
                MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
                MYSQL_OPEN_IGNORE_FLUSH |
                MYSQL_LOCK_IGNORE_TIMEOUT |
                MYSQL_LOCK_LOG_TABLE);
  TABLE *table;
  /* Save value that is changed in mysql_lock_tables() */
  ulonglong save_utime_after_lock= thd->utime_after_lock;
  DBUG_ENTER("open_log_table");

  thd->reset_n_backup_open_tables_state(backup);

  if ((table= open_ltable(thd, one_table, one_table->lock_type, flags)))
  {
    DBUG_ASSERT(table->s->table_category == TABLE_CATEGORY_LOG);
    /* Make sure all columns get assigned to a default value */
    table->use_all_columns();
    DBUG_ASSERT(table->s->no_replicate);
  }
  else
    thd->restore_backup_open_tables_state(backup);

  thd->utime_after_lock= save_utime_after_lock;
  DBUG_RETURN(table);
}

/**
  Close a log table.
  The last table opened by open_log_table()
  is closed, then the thread context is restored.
  @param thd The current thread
  @param backup [in] the context to restore.
*/
void close_log_table(THD *thd, Open_tables_backup *backup)
{
  close_system_tables(thd, backup);
}


/**
  @brief
  Remove 'fixed' flag from items in a list

  @param items list of items to un-fix

  @details
  This function sets to 0 the 'fixed' flag for items in the 'items' list.
  It's needed to force correct marking of views' fields for INSERT/UPDATE
  statements.
*/

void unfix_fields(List<Item> &fields)
{
  List_iterator<Item> li(fields);
  Item *item;
  while ((item= li++))
    item->unfix_fields();
}


/**
  Check result of dynamic column function and issue error if it is needed

  @param rc              The result code of dynamic column function

  @return the result code which was get as an argument\
*/

int dynamic_column_error_message(enum_dyncol_func_result rc)
{
  switch (rc) {
  case ER_DYNCOL_YES:
  case ER_DYNCOL_OK:
  case ER_DYNCOL_TRUNCATED:
    break; // it is not an error
  case ER_DYNCOL_FORMAT:
    my_error(ER_DYN_COL_WRONG_FORMAT, MYF(0));
    break;
  case ER_DYNCOL_LIMIT:
    my_error(ER_DYN_COL_IMPLEMENTATION_LIMIT, MYF(0));
    break;
  case ER_DYNCOL_RESOURCE:
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    break;
  case ER_DYNCOL_DATA:
    my_error(ER_DYN_COL_DATA, MYF(0));
    break;
  case ER_DYNCOL_UNKNOWN_CHARSET:
    my_error(ER_DYN_COL_WRONG_CHARSET, MYF(0));
    break;
  }
  return rc;
}

/**
  @} (end of group Data_Dictionary)
*/
