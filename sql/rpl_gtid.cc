/* Copyright (c) 2013, Kristian Nielsen and MariaDB Services Ab.
   Copyright (c) 2020, MariaDB Corporation.

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


/* Definitions for MariaDB global transaction ID (GTID). */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "mariadb.h"
#include "sql_base.h"
#include "sql_parse.h"
#include "key.h"
#include "rpl_gtid.h"
#include "rpl_rli.h"
#include "slave.h"
#include "log_event.h"

const LEX_CSTRING rpl_gtid_slave_state_table_name=
  { STRING_WITH_LEN("gtid_slave_pos") };


void
rpl_slave_state::update_state_hash(uint64 sub_id, rpl_gtid *gtid, void *hton,
                                   rpl_group_info *rgi)
{
  int err;
  /*
    Add the gtid to the HASH in the replication slave state.

    We must do this only _after_ commit, so that for parallel replication,
    there will not be an attempt to delete the corresponding table row before
    it is even committed.
  */
  mysql_mutex_lock(&LOCK_slave_state);
  err= update(gtid->domain_id, gtid->server_id, sub_id, gtid->seq_no, hton, rgi);
  mysql_mutex_unlock(&LOCK_slave_state);
  if (err)
  {
    sql_print_warning("Slave: Out of memory during slave state maintenance. "
                      "Some no longer necessary rows in table "
                      "mysql.%s may be left undeleted.",
                      rpl_gtid_slave_state_table_name.str);
    /*
      Such failure is not fatal. We will fail to delete the row for this
      GTID, but it will do no harm and will be removed automatically on next
      server restart.
    */
  }
}


int
rpl_slave_state::record_and_update_gtid(THD *thd, rpl_group_info *rgi)
{
  DBUG_ENTER("rpl_slave_state::record_and_update_gtid");

  /*
    Update the GTID position, if we have it and did not already update
    it in a GTID transaction.
  */
  if (rgi->gtid_pending)
  {
    uint64 sub_id= rgi->gtid_sub_id;
    void *hton= NULL;

    rgi->gtid_pending= false;
    if (rgi->gtid_ignore_duplicate_state!=rpl_group_info::GTID_DUPLICATE_IGNORE)
    {
      if (record_gtid(thd, &rgi->current_gtid, sub_id, false, false, &hton))
        DBUG_RETURN(1);
      update_state_hash(sub_id, &rgi->current_gtid, hton, rgi);
    }
    rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_NULL;
  }
  DBUG_RETURN(0);
}


/*
  Check GTID event execution when --gtid-ignore-duplicates.

  The idea with --gtid-ignore-duplicates is that we allow multiple master
  connections (in multi-source replication) to all receive the same GTIDs and
  event groups. Only one instance of each is applied; we use the sequence
  number in the GTID to decide whether a GTID has already been applied.

  So if the seq_no of a GTID (or a higher sequence number) has already been
  applied, then the event should be skipped. If not then the event should be
  applied.

  To avoid two master connections tring to apply the same event
  simultaneously, only one is allowed to work in any given domain at any point
  in time. The associated Relay_log_info object is called the owner of the
  domain (and there can be multiple parallel worker threads working in that
  domain for that Relay_log_info). Any other Relay_log_info/master connection
  must wait for the domain to become free, or for their GTID to have been
  applied, before being allowed to proceed.

  Returns:
    0  This GTID is already applied, it should be skipped.
    1  The GTID is not yet applied; this rli is now the owner, and must apply
       the event and release the domain afterwards.
   -1  Error (out of memory to allocate a new element for the domain).
*/
int
rpl_slave_state::check_duplicate_gtid(rpl_gtid *gtid, rpl_group_info *rgi)
{
  uint32 domain_id= gtid->domain_id;
  uint64 seq_no= gtid->seq_no;
  rpl_slave_state::element *elem;
  int res;
  bool did_enter_cond= false;
  PSI_stage_info old_stage;
  THD *UNINIT_VAR(thd);
  Relay_log_info *rli= rgi->rli;

  mysql_mutex_lock(&LOCK_slave_state);
  if (!(elem= get_element(domain_id)))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    res= -1;
    goto err;
  }
  /*
    Note that the elem pointer does not change once inserted in the hash. So
    we can re-use the pointer without looking it up again in the hash after
    each lock release and re-take.
  */

  for (;;)
  {
    if (elem->highest_seq_no >= seq_no)
    {
      /* This sequence number is already applied, ignore it. */
      res= 0;
      rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_IGNORE;
      break;
    }
    if (!elem->owner_rli)
    {
      /* The domain became free, grab it and apply the event. */
      elem->owner_rli= rli;
      elem->owner_count= 1;
      rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_OWNER;
      res= 1;
      break;
    }
    if (elem->owner_rli == rli)
    {
      /* Already own this domain, increment reference count and apply event. */
      ++elem->owner_count;
      rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_OWNER;
      res= 1;
      break;
    }
    thd= rgi->thd;
    if (unlikely(thd->check_killed()))
    {
      res= -1;
      break;
    }
    /*
      Someone else is currently processing this GTID (or an earlier one).
      Wait for them to complete (or fail), and then check again.
    */
    if (!did_enter_cond)
    {
      thd->ENTER_COND(&elem->COND_gtid_ignore_duplicates, &LOCK_slave_state,
                      &stage_gtid_wait_other_connection, &old_stage);
      did_enter_cond= true;
    }
    mysql_cond_wait(&elem->COND_gtid_ignore_duplicates,
                    &LOCK_slave_state);
  }

err:
  if (did_enter_cond)
    thd->EXIT_COND(&old_stage);
  else
    mysql_mutex_unlock(&LOCK_slave_state);
  return res;
}


void
rpl_slave_state::release_domain_owner(rpl_group_info *rgi)
{
  element *elem= NULL;

  mysql_mutex_lock(&LOCK_slave_state);
  if (!(elem= get_element(rgi->current_gtid.domain_id)))
  {
    /*
      We cannot really deal with error here, as we are already called in an
      error handling case (transaction failure and rollback).

      However, get_element() only fails if the element did not exist already
      and could not be allocated due to out-of-memory - and if it did not
      exist, then we would not get here in the first place.
    */
    mysql_mutex_unlock(&LOCK_slave_state);
    return;
  }

  if (rgi->gtid_ignore_duplicate_state == rpl_group_info::GTID_DUPLICATE_OWNER)
  {
    uint32 count= elem->owner_count;
    DBUG_ASSERT(count > 0);
    DBUG_ASSERT(elem->owner_rli == rgi->rli);
    --count;
    elem->owner_count= count;
    if (count == 0)
    {
      elem->owner_rli= NULL;
      mysql_cond_broadcast(&elem->COND_gtid_ignore_duplicates);
    }
  }
  rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_NULL;
  mysql_mutex_unlock(&LOCK_slave_state);
}


static void
rpl_slave_state_free_element(void *arg)
{
  struct rpl_slave_state::element *elem= (struct rpl_slave_state::element *)arg;
  mysql_cond_destroy(&elem->COND_wait_gtid);
  mysql_cond_destroy(&elem->COND_gtid_ignore_duplicates);
  my_free(elem);
}


rpl_slave_state::rpl_slave_state()
  : pending_gtid_count(0), last_sub_id(0), gtid_pos_tables(0), loaded(false)
{
  mysql_mutex_init(key_LOCK_slave_state, &LOCK_slave_state,
                   MY_MUTEX_INIT_SLOW);
  my_hash_init(PSI_INSTRUMENT_ME, &hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, rpl_slave_state_free_element, HASH_UNIQUE);
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &gtid_sort_array, sizeof(rpl_gtid),
                        8, 8, MYF(0));
}


rpl_slave_state::~rpl_slave_state()
{
  free_gtid_pos_tables(gtid_pos_tables.load(std::memory_order_relaxed));
  truncate_hash();
  my_hash_free(&hash);
  delete_dynamic(&gtid_sort_array);
  mysql_mutex_destroy(&LOCK_slave_state);
}


void
rpl_slave_state::truncate_hash()
{
  uint32 i;

  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    list_element *l= e->list;
    list_element *next;
    while (l)
    {
      next= l->next;
      my_free(l);
      l= next;
    }
    /* The element itself is freed by the hash element free function. */
  }
  my_hash_reset(&hash);
}


int
rpl_slave_state::update(uint32 domain_id, uint32 server_id, uint64 sub_id,
                        uint64 seq_no, void *hton, rpl_group_info *rgi)
{
  element *elem= NULL;
  list_element *list_elem= NULL;

  DBUG_ASSERT(hton || !loaded);
  if (!(elem= get_element(domain_id)))
    return 1;

  if (seq_no > elem->highest_seq_no)
    elem->highest_seq_no= seq_no;
  if (elem->gtid_waiter && elem->min_wait_seq_no <= seq_no)
  {
    /*
      Someone was waiting in MASTER_GTID_WAIT() for this GTID to appear.
      Signal (and remove) them. The waiter will handle all the processing
      of all pending MASTER_GTID_WAIT(), so we do not slow down the
      replication SQL thread.
    */
    mysql_mutex_assert_owner(&LOCK_slave_state);
    elem->gtid_waiter= NULL;
    mysql_cond_broadcast(&elem->COND_wait_gtid);
  }

  if (rgi)
  {
    if (rgi->gtid_ignore_duplicate_state==rpl_group_info::GTID_DUPLICATE_OWNER)
    {
#ifdef DBUG_ASSERT_EXISTS
      Relay_log_info *rli= rgi->rli;
#endif
      uint32 count= elem->owner_count;
      DBUG_ASSERT(count > 0);
      DBUG_ASSERT(elem->owner_rli == rli);
      --count;
      elem->owner_count= count;
      if (count == 0)
      {
        elem->owner_rli= NULL;
        mysql_cond_broadcast(&elem->COND_gtid_ignore_duplicates);
      }
    }
    rgi->gtid_ignore_duplicate_state= rpl_group_info::GTID_DUPLICATE_NULL;
  }

  if (!(list_elem= (list_element *)my_malloc(PSI_INSTRUMENT_ME,
                                             sizeof(*list_elem), MYF(MY_WME))))
    return 1;
  list_elem->domain_id= domain_id;
  list_elem->server_id= server_id;
  list_elem->sub_id= sub_id;
  list_elem->seq_no= seq_no;
  list_elem->hton= hton;

  elem->add(list_elem);
  if (last_sub_id < sub_id)
    last_sub_id= sub_id;

#ifdef HAVE_REPLICATION
  ++pending_gtid_count;
  if (pending_gtid_count >= opt_gtid_cleanup_batch_size)
  {
    pending_gtid_count = 0;
    slave_background_gtid_pending_delete_request();
  }
#endif

  return 0;
}


struct rpl_slave_state::element *
rpl_slave_state::get_element(uint32 domain_id)
{
  struct element *elem;

  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (elem)
    return elem;

  if (!(elem= (element *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*elem), MYF(MY_WME))))
    return NULL;
  elem->list= NULL;
  elem->domain_id= domain_id;
  elem->highest_seq_no= 0;
  elem->gtid_waiter= NULL;
  elem->owner_rli= NULL;
  elem->owner_count= 0;
  mysql_cond_init(key_COND_wait_gtid, &elem->COND_wait_gtid, 0);
  mysql_cond_init(key_COND_gtid_ignore_duplicates,
                  &elem->COND_gtid_ignore_duplicates, 0);
  if (my_hash_insert(&hash, (uchar *)elem))
  {
    my_free(elem);
    return NULL;
  }
  return elem;
}


int
rpl_slave_state::put_back_list(list_element *list)
{
  element *e= NULL;
  int err= 0;

  mysql_mutex_lock(&LOCK_slave_state);
  while (list)
  {
    list_element *next= list->next;

    if ((!e || e->domain_id != list->domain_id) &&
        !(e= (element *)my_hash_search(&hash, (const uchar *)&list->domain_id, 0)))
    {
      err= 1;
      goto end;
    }
    e->add(list);
    list= next;
  }

end:
  mysql_mutex_unlock(&LOCK_slave_state);
  return err;
}


int
rpl_slave_state::truncate_state_table(THD *thd)
{
  TABLE_LIST tlist;
  int err= 0;

  tlist.init_one_table(&MYSQL_SCHEMA_NAME, &rpl_gtid_slave_state_table_name,
                       NULL, TL_WRITE);
  tlist.mdl_request.set_type(MDL_EXCLUSIVE);
  if (!(err= open_and_lock_tables(thd, &tlist, FALSE,
                                  MYSQL_OPEN_IGNORE_LOGGING_FORMAT)))
  {
    DBUG_ASSERT(!tlist.table->file->row_logging);
    tlist.table->s->tdc->flush(thd, true);
    err= tlist.table->file->ha_truncate();

    if (err)
    {
      ha_rollback_trans(thd, FALSE);
      close_thread_tables(thd);
      ha_rollback_trans(thd, TRUE);
    }
    else
    {
      ha_commit_trans(thd, FALSE);
      close_thread_tables(thd);
      ha_commit_trans(thd, TRUE);
    }
    thd->mdl_context.release_transactional_locks();
  }
  return err;
}


static const TABLE_FIELD_TYPE mysql_rpl_slave_state_coltypes[4]= {
  { { STRING_WITH_LEN("domain_id") },
    { STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { STRING_WITH_LEN("sub_id") },
    { STRING_WITH_LEN("bigint(20) unsigned") },
    {NULL, 0} },
  { { STRING_WITH_LEN("server_id") },
    { STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { STRING_WITH_LEN("seq_no") },
    { STRING_WITH_LEN("bigint(20) unsigned") },
    {NULL, 0} },
};

static const uint mysql_rpl_slave_state_pk_parts[]= {0, 1};

static const TABLE_FIELD_DEF mysql_gtid_slave_pos_tabledef= {
  array_elements(mysql_rpl_slave_state_coltypes),
  mysql_rpl_slave_state_coltypes,
  array_elements(mysql_rpl_slave_state_pk_parts),
  mysql_rpl_slave_state_pk_parts
};

static Table_check_intact_log_error gtid_table_intact;

/*
  Check that the mysql.gtid_slave_pos table has the correct definition.
*/
int
gtid_check_rpl_slave_state_table(TABLE *table)
{
  int err;

  if ((err= gtid_table_intact.check(table, &mysql_gtid_slave_pos_tabledef)))
    my_error(ER_GTID_OPEN_TABLE_FAILED, MYF(0), "mysql",
             rpl_gtid_slave_state_table_name.str);
  return err;
}


/*
  Attempt to find a mysql.gtid_slave_posXXX table that has a storage engine
  that is already in use by the current transaction, if any.
*/
void
rpl_slave_state::select_gtid_pos_table(THD *thd, LEX_CSTRING *out_tablename)
{
  /*
    See comments on rpl_slave_state::gtid_pos_tables for rules around proper
    access to the list.
  */
  auto list= gtid_pos_tables.load(std::memory_order_acquire);

  Ha_trx_info *ha_info;
  uint count = 0;
  for (ha_info= thd->transaction->all.ha_list; ha_info; ha_info= ha_info->next())
  {
    void *trx_hton= ha_info->ht();
    auto table_entry= list;

    if (!ha_info->is_trx_read_write() || trx_hton == binlog_hton)
      continue;
    while (table_entry)
    {
      if (table_entry->table_hton == trx_hton)
      {
        if (likely(table_entry->state == GTID_POS_AVAILABLE))
        {
          *out_tablename= table_entry->table_name;
          /*
            Check if this is a cross-engine transaction, so we can correctly
            maintain the rpl_transactions_multi_engine status variable.
          */
          if (count >= 1)
            statistic_increment(rpl_transactions_multi_engine, LOCK_status);
          else
          {
            for (;;)
            {
              ha_info= ha_info->next();
              if (!ha_info)
                break;
              if (ha_info->is_trx_read_write() && ha_info->ht() != binlog_hton)
              {
                statistic_increment(rpl_transactions_multi_engine, LOCK_status);
                break;
              }
            }
          }
          return;
        }
        /*
          This engine is marked to automatically create the table.
          We cannot easily do this here (possibly in the middle of a
          transaction). But we can request the slave background thread
          to create it, and in a short while it should become available
          for following transactions.
        */
#ifdef HAVE_REPLICATION
        slave_background_gtid_pos_create_request(table_entry);
#endif
        break;
      }
      table_entry= table_entry->next;
    }
    ++count;
  }
  /*
    If we cannot find any table whose engine matches an engine that is
    already active in the transaction, or if there is no current transaction
    engines available, we return the default gtid_slave_pos table.
  */
  *out_tablename=
    default_gtid_pos_table.load(std::memory_order_acquire)->table_name;
  /* Record in status that we failed to find a suitable gtid_pos table. */
  if (count > 0)
  {
    statistic_increment(transactions_gtid_foreign_engine, LOCK_status);
    if (count > 1)
      statistic_increment(rpl_transactions_multi_engine, LOCK_status);
  }
}


/*
  Write a gtid to the replication slave state table.

  Do it as part of the transaction, to get slave crash safety, or as a separate
  transaction if !in_transaction (eg. MyISAM or DDL).

    gtid    The global transaction id for this event group.
    sub_id  Value allocated within the sub_id when the event group was
            read (sub_id must be consistent with commit order in master binlog).

  Note that caller must later ensure that the new gtid and sub_id is inserted
  into the appropriate HASH element with rpl_slave_state.add(), so that it can
  be deleted later. But this must only be done after COMMIT if in transaction.
*/
int
rpl_slave_state::record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                             bool in_transaction, bool in_statement,
                             void **out_hton)
{
  TABLE_LIST tlist;
  int err= 0, not_sql_thread;
  bool table_opened= false;
  TABLE *table;
  ulonglong thd_saved_option= thd->variables.option_bits;
  Query_tables_list lex_backup;
  wait_for_commit* suspended_wfc;
  void *hton= NULL;
  LEX_CSTRING gtid_pos_table_name;
  DBUG_ENTER("record_gtid");

  *out_hton= NULL;
  if (unlikely(!loaded))
  {
    /*
      Probably the mysql.gtid_slave_pos table is missing (eg. upgrade) or
      corrupt.

      We already complained loudly about this, but we can try to continue
      until the DBA fixes it.
    */
    DBUG_RETURN(0);
  }

  if (!in_statement)
    thd->reset_for_next_command();

  /*
    Only the SQL thread can call select_gtid_pos_table without a mutex
    Other threads needs to use a mutex and take into account that the
    result may change during execution, so we have to make a copy.
  */

  if ((not_sql_thread= (thd->system_thread != SYSTEM_THREAD_SLAVE_SQL)))
    mysql_mutex_lock(&LOCK_slave_state);
  select_gtid_pos_table(thd, &gtid_pos_table_name);
  if (not_sql_thread)
  {
    LEX_CSTRING *tmp= thd->make_clex_string(gtid_pos_table_name.str,
                                            gtid_pos_table_name.length);
    mysql_mutex_unlock(&LOCK_slave_state);
    if (!tmp)
      DBUG_RETURN(1);
    gtid_pos_table_name= *tmp;
  }

  DBUG_EXECUTE_IF("gtid_inject_record_gtid",
                  {
                    my_error(ER_CANNOT_UPDATE_GTID_STATE, MYF(0));
                    DBUG_RETURN(1);
                  } );

  /*
    If we are applying a non-transactional event group, we will be committing
    here a transaction, but that does not imply that the event group has
    completed or has been binlogged. So we should not trigger
    wakeup_subsequent_commits() here.

    Note: An alternative here could be to put a call to mark_start_commit() in
    stmt_done() before the call to record_and_update_gtid(). This would
    prevent later calling mark_start_commit() after we have run
    wakeup_subsequent_commits() from committing the GTID update transaction
    (which must be avoided to avoid accessing freed group_commit_orderer
    object). It would also allow following event groups to start slightly
    earlier. And in the cases where record_gtid() is called without an active
    transaction, the current statement should have been binlogged already, so
    binlog order is preserved.

    But this is rather subtle, and potentially fragile. And it does not really
    seem worth it; non-transactional loads are unlikely to benefit much from
    parallel replication in any case. So for now, we go with the simple
    suspend/resume of wakeup_subsequent_commits() here in record_gtid().
  */
  suspended_wfc= thd->suspend_subsequent_commits();
  thd->lex->reset_n_backup_query_tables_list(&lex_backup);
  tlist.init_one_table(&MYSQL_SCHEMA_NAME, &gtid_pos_table_name, NULL, TL_WRITE);
  if ((err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
    goto end;
  table_opened= true;
  table= tlist.table;
  hton= table->s->db_type();
  table->file->row_logging= 0;                  // No binary logging

  if ((err= gtid_check_rpl_slave_state_table(table)))
    goto end;

#ifdef WITH_WSREP
  /*
    Updates in slave state table should not be appended to galera transaction
    writeset.
  */
  thd->wsrep_ignore_table= true;
#endif

  if (!in_transaction)
  {
    DBUG_PRINT("info", ("resetting OPTION_BEGIN"));
    thd->variables.option_bits&=
      ~(ulonglong)(OPTION_NOT_AUTOCOMMIT |OPTION_BEGIN |OPTION_BIN_LOG |
                   OPTION_GTID_BEGIN);
  }
  else
    thd->variables.option_bits&= ~(ulonglong)OPTION_BIN_LOG;

  bitmap_set_all(table->write_set);
  table->rpl_write_set= table->write_set;

  table->field[0]->store((ulonglong)gtid->domain_id, true);
  table->field[1]->store(sub_id, true);
  table->field[2]->store((ulonglong)gtid->server_id, true);
  table->field[3]->store(gtid->seq_no, true);
  DBUG_EXECUTE_IF("inject_crash_before_write_rpl_slave_state", DBUG_SUICIDE(););
  if ((err= table->file->ha_write_row(table->record[0])))
  {
    table->file->print_error(err, MYF(0));
    goto end;
  }
  *out_hton= hton;

  if(opt_bin_log &&
     (err= mysql_bin_log.bump_seq_no_counter_if_needed(gtid->domain_id,
                                                       gtid->seq_no)))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    goto end;
  }
end:

#ifdef WITH_WSREP
  thd->wsrep_ignore_table= false;
#endif

  if (table_opened)
  {
    if (err || (err= ha_commit_trans(thd, FALSE)))
      ha_rollback_trans(thd, FALSE);

    close_thread_tables(thd);
    if (in_transaction)
      thd->mdl_context.release_statement_locks();
    else
      thd->mdl_context.release_transactional_locks();
  }
  thd->lex->restore_backup_query_tables_list(&lex_backup);
  thd->variables.option_bits= thd_saved_option;
  thd->resume_subsequent_commits(suspended_wfc);
  DBUG_EXECUTE_IF("inject_record_gtid_serverid_100_sleep",
    {
      if (gtid->server_id == 100)
        my_sleep(500000);
    });
  DBUG_RETURN(err);
}


/*
  Return a list of all old GTIDs in any mysql.gtid_slave_pos* table that are
  no longer needed and can be deleted from the table.

  Within each domain, we need to keep around the latest GTID (the one with the
  highest sub_id), but any others in that domain can be deleted.
*/
rpl_slave_state::list_element *
rpl_slave_state::gtid_grab_pending_delete_list()
{
  uint32 i;
  list_element *full_list;

  mysql_mutex_lock(&LOCK_slave_state);
  full_list= NULL;
  for (i= 0; i < hash.records; ++i)
  {
    element *elem= (element *)my_hash_element(&hash, i);
    list_element *elist= elem->list;
    list_element *last_elem, **best_ptr_ptr, *cur, *next;
    uint64 best_sub_id;

    if (!elist)
      continue;                                 /* Nothing here */

    /* Delete any old stuff, but keep around the most recent one. */
    cur= elist;
    best_sub_id= cur->sub_id;
    best_ptr_ptr= &elist;
    last_elem= cur;
    while ((next= cur->next)) {
      last_elem= next;
      if (next->sub_id > best_sub_id)
      {
        best_sub_id= next->sub_id;
        best_ptr_ptr= &cur->next;
      }
      cur= next;
    }
    /*
      Append the new elements to the full list. Note the order is important;
      we do it here so that we do not break the list if best_sub_id is the
      last of the new elements.
    */
    last_elem->next= full_list;
    /*
      Delete the highest sub_id element from the old list, and put it back as
      the single-element new list.
    */
    cur= *best_ptr_ptr;
    *best_ptr_ptr= cur->next;
    cur->next= NULL;
    elem->list= cur;

    /*
      Collect the full list so far here. Note that elist may have moved if we
      deleted the first element, so order is again important.
    */
    full_list= elist;
  }
  mysql_mutex_unlock(&LOCK_slave_state);

  return full_list;
}


/* Find the mysql.gtid_slave_posXXX table associated with a given hton. */
LEX_CSTRING *
rpl_slave_state::select_gtid_pos_table(void *hton)
{
  /*
    See comments on rpl_slave_state::gtid_pos_tables for rules around proper
    access to the list.
  */
  auto table_entry= gtid_pos_tables.load(std::memory_order_acquire);

  while (table_entry)
  {
    if (table_entry->table_hton == hton)
    {
      if (likely(table_entry->state == GTID_POS_AVAILABLE))
        return &table_entry->table_name;
    }
    table_entry= table_entry->next;
  }

  return &default_gtid_pos_table.load(std::memory_order_acquire)->table_name;
}


void
rpl_slave_state::gtid_delete_pending(THD *thd,
                                     rpl_slave_state::list_element **list_ptr)
{
  int err= 0;
  ulonglong thd_saved_option;

  if (unlikely(!loaded))
    return;

#ifdef WITH_WSREP
  /*
    Updates in slave state table should not be appended to galera transaction
    writeset.
  */
  thd->wsrep_ignore_table= true;
#endif

  thd_saved_option= thd->variables.option_bits;
  thd->variables.option_bits&=
    ~(ulonglong)(OPTION_NOT_AUTOCOMMIT |OPTION_BEGIN |OPTION_BIN_LOG |
                 OPTION_GTID_BEGIN);

  while (*list_ptr)
  {
    LEX_CSTRING *gtid_pos_table_name, *tmp_table_name;
    Query_tables_list lex_backup;
    TABLE_LIST tlist;
    TABLE *table;
    handler::Table_flags direct_pos= 0;
    list_element *cur, **cur_ptr_ptr;
    bool table_opened= false;
    bool index_inited= false;
    void *hton= (*list_ptr)->hton;

    thd->reset_for_next_command();

    /*
      Only the SQL thread can call select_gtid_pos_table without a mutex
      Other threads needs to use a mutex and take into account that the
      result may change during execution, so we have to make a copy.
    */
    mysql_mutex_lock(&LOCK_slave_state);
    tmp_table_name= select_gtid_pos_table(hton);
    gtid_pos_table_name= thd->make_clex_string(tmp_table_name->str,
                                               tmp_table_name->length);
    mysql_mutex_unlock(&LOCK_slave_state);
    if (!gtid_pos_table_name)
    {
      /* Out of memory - we can try again later. */
      break;
    }

    thd->lex->reset_n_backup_query_tables_list(&lex_backup);
    tlist.init_one_table(&MYSQL_SCHEMA_NAME, gtid_pos_table_name, NULL, TL_WRITE);
    if ((err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
      goto end;
    table_opened= true;
    table= tlist.table;

    if ((err= gtid_check_rpl_slave_state_table(table)))
      goto end;

    direct_pos= table->file->ha_table_flags() & HA_PRIMARY_KEY_REQUIRED_FOR_POSITION;
    bitmap_set_all(table->write_set);
    table->rpl_write_set= table->write_set;

    /* Now delete any already committed GTIDs. */
    bitmap_set_bit(table->read_set, table->field[0]->field_index);
    bitmap_set_bit(table->read_set, table->field[1]->field_index);

    if (!direct_pos)
    {
      if ((err= table->file->ha_index_init(0, 0)))
      {
        table->file->print_error(err, MYF(0));
        goto end;
      }
      index_inited= true;
    }

    cur = *list_ptr;
    cur_ptr_ptr = list_ptr;
    do
    {
      uchar key_buffer[4+8];
      list_element *next= cur->next;

      if (cur->hton == hton)
      {
        int res;

        table->field[0]->store((ulonglong)cur->domain_id, true);
        table->field[1]->store(cur->sub_id, true);
        if (direct_pos)
        {
          res= table->file->ha_rnd_pos_by_record(table->record[0]);
        }
        else
        {
          key_copy(key_buffer, table->record[0], &table->key_info[0], 0, false);
          res= table->file->ha_index_read_map(table->record[0], key_buffer,
                                              HA_WHOLE_KEY, HA_READ_KEY_EXACT);
        }
        DBUG_EXECUTE_IF("gtid_slave_pos_simulate_failed_delete",
              { res= 1;
                err= ENOENT;
                sql_print_error("<DEBUG> Error deleting old GTID row");
              });
        if (res)
          /* We cannot find the row, assume it is already deleted. */
          ;
        else if ((err= table->file->ha_delete_row(table->record[0])))
        {
          sql_print_error("Error deleting old GTID row: %s",
                          thd->get_stmt_da()->message());
          /*
            In case of error, we still discard the element from the list. We do
            not want to endlessly error on the same element in case of table
            corruption or such.
          */
        }
        *cur_ptr_ptr= next;
        my_free(cur);
      }
      else
      {
        /* Leave this one in the list until we get to the table for its hton. */
        cur_ptr_ptr= &cur->next;
      }
      cur= next;
      if (err)
        break;
    } while (cur);
end:
    if (table_opened)
    {
      DBUG_ASSERT(direct_pos || index_inited || err);
      /*
        Index may not be initialized if there was a failure during
        'ha_index_init'. Hence check if index initialization is successful and
        then invoke ha_index_end(). Ending an index which is not initialized
        will lead to assert.
      */
      if (index_inited)
        table->file->ha_index_end();

      if (err || (err= ha_commit_trans(thd, FALSE)))
        ha_rollback_trans(thd, FALSE);
    }
    close_thread_tables(thd);
    thd->mdl_context.release_transactional_locks();
    thd->lex->restore_backup_query_tables_list(&lex_backup);

    if (err)
      break;
  }
  thd->variables.option_bits= thd_saved_option;

#ifdef WITH_WSREP
  thd->wsrep_ignore_table= false;
#endif
}


uint64
rpl_slave_state::next_sub_id(uint32 domain_id)
{
  uint64 sub_id= 0;

  mysql_mutex_lock(&LOCK_slave_state);
  sub_id= ++last_sub_id;
  mysql_mutex_unlock(&LOCK_slave_state);

  return sub_id;
}

/* A callback used in sorting of gtid list based on domain_id. */
static int rpl_gtid_cmp_cb(const void *id1, const void *id2)
{
  uint32 d1= ((rpl_gtid *)id1)->domain_id;
  uint32 d2= ((rpl_gtid *)id2)->domain_id;

  if (d1 < d2)
    return -1;
  else if (d1 > d2)
    return 1;
  return 0;
}

/* Format the specified gtid and store it in the given string buffer. */
bool
rpl_slave_state_tostring_helper(String *dest, const rpl_gtid *gtid, bool *first)
{
  if (*first)
    *first= false;
  else
    if (dest->append(",",1))
      return true;
  return
    dest->append_ulonglong(gtid->domain_id) ||
    dest->append("-",1) ||
    dest->append_ulonglong(gtid->server_id) ||
    dest->append("-",1) ||
    dest->append_ulonglong(gtid->seq_no);
}

/*
  Sort the given gtid list based on domain_id and store them in the specified
  string.
*/
static bool
rpl_slave_state_tostring_helper(DYNAMIC_ARRAY *gtid_dynarr, String *str)
{
  bool first= true, res= true;

  sort_dynamic(gtid_dynarr, rpl_gtid_cmp_cb);

  for (uint i= 0; i < gtid_dynarr->elements; i ++)
  {
    rpl_gtid *gtid= dynamic_element(gtid_dynarr, i, rpl_gtid *);
    if (rpl_slave_state_tostring_helper(str, gtid, &first))
      goto err;
  }
  res= false;

err:
  return res;
}


/* Sort the given gtid list based on domain_id and call cb for each gtid. */
static bool
rpl_slave_state_tostring_helper(DYNAMIC_ARRAY *gtid_dynarr,
                                int (*cb)(rpl_gtid *, void *),
                                void *data)
{
  rpl_gtid *gtid;
  bool res= true;

  sort_dynamic(gtid_dynarr, rpl_gtid_cmp_cb);

  for (uint i= 0; i < gtid_dynarr->elements; i ++)
  {
    gtid= dynamic_element(gtid_dynarr, i, rpl_gtid *);
    if ((*cb)(gtid, data))
      goto err;
  }
  res= false;

err:
  return res;
}

int
rpl_slave_state::iterate(int (*cb)(rpl_gtid *, void *), void *data,
                         rpl_gtid *extra_gtids, uint32 num_extra,
                         bool sort)
{
  uint32 i;
  HASH gtid_hash;
  uchar *rec;
  rpl_gtid *gtid;
  int res= 1;
  bool locked= false;

  my_hash_init(PSI_INSTRUMENT_ME, &gtid_hash, &my_charset_bin, 32,
               offsetof(rpl_gtid, domain_id), sizeof(uint32), NULL, NULL,
               HASH_UNIQUE);
  for (i= 0; i < num_extra; ++i)
    if (extra_gtids[i].server_id == global_system_variables.server_id &&
        my_hash_insert(&gtid_hash, (uchar *)(&extra_gtids[i])))
      goto err;

  mysql_mutex_lock(&LOCK_slave_state);
  locked= true;
  reset_dynamic(&gtid_sort_array);

  for (i= 0; i < hash.records; ++i)
  {
    uint64 best_sub_id;
    rpl_gtid best_gtid;
    element *e= (element *)my_hash_element(&hash, i);
    list_element *l= e->list;

    if (!l)
      continue;                                 /* Nothing here */

    best_gtid.domain_id= e->domain_id;
    best_gtid.server_id= l->server_id;
    best_gtid.seq_no= l->seq_no;
    best_sub_id= l->sub_id;
    while ((l= l->next))
    {
      if (l->sub_id > best_sub_id)
      {
        best_sub_id= l->sub_id;
        best_gtid.server_id= l->server_id;
        best_gtid.seq_no= l->seq_no;
      }
    }

    /* Check if we have something newer in the extra list. */
    rec= my_hash_search(&gtid_hash, (const uchar *)&best_gtid.domain_id, 0);
    if (rec)
    {
      gtid= (rpl_gtid *)rec;
      if (gtid->seq_no > best_gtid.seq_no)
        memcpy(&best_gtid, gtid, sizeof(best_gtid));
      if (my_hash_delete(&gtid_hash, rec))
      {
        goto err;
      }
    }

    if ((res= sort ? insert_dynamic(&gtid_sort_array,
                                    (const void *) &best_gtid) :
         (*cb)(&best_gtid, data)))
    {
      goto err;
    }
  }

  /* Also add any remaining extra domain_ids. */
  for (i= 0; i < gtid_hash.records; ++i)
  {
    gtid= (rpl_gtid *)my_hash_element(&gtid_hash, i);
    if ((res= sort ? insert_dynamic(&gtid_sort_array, (const void *) gtid) :
         (*cb)(gtid, data)))
    {
      goto err;
    }
  }

  if (sort && rpl_slave_state_tostring_helper(&gtid_sort_array, cb, data))
  {
    goto err;
  }

  res= 0;

err:
  if (locked) mysql_mutex_unlock(&LOCK_slave_state);
  my_hash_free(&gtid_hash);

  return res;
}


struct rpl_slave_state_tostring_data {
  String *dest;
  bool first;
};
static int
rpl_slave_state_tostring_cb(rpl_gtid *gtid, void *data)
{
  rpl_slave_state_tostring_data *p= (rpl_slave_state_tostring_data *)data;
  return rpl_slave_state_tostring_helper(p->dest, gtid, &p->first);
}


/*
  Prepare the current slave state as a string, suitable for sending to the
  master to request to receive binlog events starting from that GTID state.

  The state consists of the most recently applied GTID for each domain_id,
  ie. the one with the highest sub_id within each domain_id.

  Optinally, extra_gtids is a list of GTIDs from the binlog. This is used when
  a server was previously a master and now needs to connect to a new master as
  a slave. For each domain_id, if the GTID in the binlog was logged with our
  own server_id _and_ has a higher seq_no than what is in the slave state,
  then this should be used as the position to start replicating at. This
  allows to promote a slave as new master, and connect the old master as a
  slave with MASTER_GTID_POS=AUTO.
*/
int
rpl_slave_state::tostring(String *dest, rpl_gtid *extra_gtids, uint32 num_extra)
{
  struct rpl_slave_state_tostring_data data;
  data.first= true;
  data.dest= dest;

  return iterate(rpl_slave_state_tostring_cb, &data, extra_gtids,
                 num_extra, true);
}


/*
  Lookup a domain_id in the current replication slave state.

  Returns false if the domain_id has no entries in the slave state.
  Otherwise returns true, and fills in out_gtid with the corresponding
  GTID.
*/
bool
rpl_slave_state::domain_to_gtid(uint32 domain_id, rpl_gtid *out_gtid)
{
  element *elem;
  list_element *list;
  uint64 best_sub_id;

  mysql_mutex_lock(&LOCK_slave_state);
  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (!elem || !(list= elem->list))
  {
    mysql_mutex_unlock(&LOCK_slave_state);
    return false;
  }

  out_gtid->domain_id= domain_id;
  out_gtid->server_id= list->server_id;
  out_gtid->seq_no= list->seq_no;
  best_sub_id= list->sub_id;

  while ((list= list->next))
  {
    if (best_sub_id > list->sub_id)
      continue;
    best_sub_id= list->sub_id;
    out_gtid->server_id= list->server_id;
    out_gtid->seq_no= list->seq_no;
  }

  mysql_mutex_unlock(&LOCK_slave_state);
  return true;
}


/*
  Parse a GTID at the start of a string, and update the pointer to point
  at the first character after the parsed GTID.

  Returns 0 on ok, non-zero on parse error.
*/
static int
gtid_parser_helper(const char **ptr, const char *end, rpl_gtid *out_gtid)
{
  char *q;
  const char *p= *ptr;
  uint64 v1, v2, v3;
  int err= 0;

  q= (char*) end;
  v1= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v1 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= (char*) end;
  v2= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v2 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= (char*) end;
  v3= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0)
    return 1;

  out_gtid->domain_id= (uint32) v1;
  out_gtid->server_id= (uint32) v2;
  out_gtid->seq_no= v3;
  *ptr= q;
  return 0;
}


rpl_gtid *
gtid_parse_string_to_list(const char *str, size_t str_len, uint32 *out_len)
{
  const char *p= const_cast<char *>(str);
  const char *end= p + str_len;
  uint32 len= 0, alloc_len= 5;
  rpl_gtid *list= NULL;

  for (;;)
  {
    rpl_gtid gtid;

    if (len >= (((uint32)1 << 28)-1) || gtid_parser_helper(&p, end, &gtid))
    {
      my_free(list);
      return NULL;
    }
    if ((!list || len >= alloc_len) &&
        !(list=
          (rpl_gtid *)my_realloc(PSI_INSTRUMENT_ME, list,
                                 (alloc_len= alloc_len*2) * sizeof(rpl_gtid),
                                 MYF(MY_FREE_ON_ERROR|MY_ALLOW_ZERO_PTR))))
      return NULL;
    list[len++]= gtid;

    if (p == end)
      break;
    if (*p != ',')
    {
      my_free(list);
      return NULL;
    }
    ++p;
  }
  *out_len= len;
  return list;
}


/*
  Update the slave replication state with the GTID position obtained from
  master when connecting with old-style (filename,offset) position.

  If RESET is true then all existing entries are removed. Otherwise only
  domain_ids mentioned in the STATE_FROM_MASTER are changed.

  Returns 0 if ok, non-zero if error.
*/
int
rpl_slave_state::load(THD *thd, const char *state_from_master, size_t len,
                      bool reset, bool in_statement)
{
  const char *end= state_from_master + len;

  if (reset)
  {
    if (truncate_state_table(thd))
      return 1;
    truncate_hash();
  }
  if (state_from_master == end)
    return 0;
  for (;;)
  {
    rpl_gtid gtid;
    uint64 sub_id;
    void *hton= NULL;

    if (gtid_parser_helper(&state_from_master, end, &gtid) ||
        !(sub_id= next_sub_id(gtid.domain_id)) ||
        record_gtid(thd, &gtid, sub_id, false, in_statement, &hton) ||
        update(gtid.domain_id, gtid.server_id, sub_id, gtid.seq_no, hton, NULL))
      return 1;
    if (state_from_master == end)
      break;
    if (*state_from_master != ',')
      return 1;
    ++state_from_master;
  }
  return 0;
}


bool
rpl_slave_state::is_empty()
{
  uint32 i;
  bool result= true;

  mysql_mutex_lock(&LOCK_slave_state);
  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (e->list)
    {
      result= false;
      break;
    }
  }
  mysql_mutex_unlock(&LOCK_slave_state);

  return result;
}


void
rpl_slave_state::free_gtid_pos_tables(struct rpl_slave_state::gtid_pos_table *list)
{
  struct gtid_pos_table *cur, *next;

  cur= list;
  while (cur)
  {
    next= cur->next;
    my_free(cur);
    cur= next;
  }
}


/*
  Replace the list of available mysql.gtid_slave_posXXX tables with a new list.
  The caller must be holding LOCK_slave_state. Additionally, this function
  must only be called while all SQL threads are stopped.
*/
void
rpl_slave_state::set_gtid_pos_tables_list(rpl_slave_state::gtid_pos_table *new_list,
                                          rpl_slave_state::gtid_pos_table *default_entry)
{
  mysql_mutex_assert_owner(&LOCK_slave_state);
  auto old_list= gtid_pos_tables.load(std::memory_order_relaxed);
  gtid_pos_tables.store(new_list, std::memory_order_release);
  default_gtid_pos_table.store(default_entry, std::memory_order_release);
  free_gtid_pos_tables(old_list);
}


void
rpl_slave_state::add_gtid_pos_table(rpl_slave_state::gtid_pos_table *entry)
{
  mysql_mutex_assert_owner(&LOCK_slave_state);
  entry->next= gtid_pos_tables.load(std::memory_order_relaxed);
  gtid_pos_tables.store(entry, std::memory_order_release);
}


struct rpl_slave_state::gtid_pos_table *
rpl_slave_state::alloc_gtid_pos_table(LEX_CSTRING *table_name, void *hton,
                                      rpl_slave_state::gtid_pos_table_state state)
{
  struct gtid_pos_table *p;
  char *allocated_str;

  if (!my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME), &p, sizeof(*p),
                       &allocated_str, table_name->length+1, NULL))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)(sizeof(*p) + table_name->length+1));
    return NULL;
  }
  memcpy(allocated_str, table_name->str, table_name->length+1); // Also copy '\0'
  p->next = NULL;
  p->table_hton= hton;
  p->table_name.str= allocated_str;
  p->table_name.length= table_name->length;
  p->state= state;
  return p;
}


void rpl_binlog_state::init()
{
  my_hash_init(PSI_INSTRUMENT_ME, &hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &gtid_sort_array, sizeof(rpl_gtid), 8, 8, MYF(0));
  mysql_mutex_init(key_LOCK_binlog_state, &LOCK_binlog_state,
                   MY_MUTEX_INIT_SLOW);
  initialized= 1;
}

void
rpl_binlog_state::reset_nolock()
{
  uint32 i;

  for (i= 0; i < hash.records; ++i)
    my_hash_free(&((element *)my_hash_element(&hash, i))->hash);
  my_hash_reset(&hash);
}


void
rpl_binlog_state::reset()
{
  mysql_mutex_lock(&LOCK_binlog_state);
  reset_nolock();
  mysql_mutex_unlock(&LOCK_binlog_state);
}


void rpl_binlog_state::free()
{
  if (initialized)
  {
    initialized= 0;
    reset_nolock();
    my_hash_free(&hash);
    delete_dynamic(&gtid_sort_array);
    mysql_mutex_destroy(&LOCK_binlog_state);
  }
}


bool
rpl_binlog_state::load(struct rpl_gtid *list, uint32 count)
{
  uint32 i;
  bool res= false;

  mysql_mutex_lock(&LOCK_binlog_state);
  reset_nolock();
  for (i= 0; i < count; ++i)
  {
    if (update_nolock(&(list[i]), false))
    {
      res= true;
      break;
    }
  }
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


static int rpl_binlog_state_load_cb(rpl_gtid *gtid, void *data)
{
  rpl_binlog_state *self= (rpl_binlog_state *)data;
  return self->update_nolock(gtid, false);
}


bool
rpl_binlog_state::load(rpl_slave_state *slave_pos)
{
  bool res= false;

  mysql_mutex_lock(&LOCK_binlog_state);
  reset_nolock();
  if (slave_pos->iterate(rpl_binlog_state_load_cb, this, NULL, 0, false))
    res= true;
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


rpl_binlog_state::~rpl_binlog_state()
{
  free();
}


/*
  Update replication state with a new GTID.

  If the (domain_id, server_id) pair already exists, then the new GTID replaces
  the old one for that domain id. Else a new entry is inserted.

  Returns 0 for ok, 1 for error.
*/
int
rpl_binlog_state::update_nolock(const struct rpl_gtid *gtid, bool strict)
{
  element *elem;

  if ((elem= (element *)my_hash_search(&hash,
                                       (const uchar *)(&gtid->domain_id), 0)))
  {
    if (strict && elem->last_gtid && elem->last_gtid->seq_no >= gtid->seq_no)
    {
      my_error(ER_GTID_STRICT_OUT_OF_ORDER, MYF(0), gtid->domain_id,
               gtid->server_id, gtid->seq_no, elem->last_gtid->domain_id,
               elem->last_gtid->server_id, elem->last_gtid->seq_no);
      return 1;
    }
    if (elem->seq_no_counter < gtid->seq_no)
      elem->seq_no_counter= gtid->seq_no;
    if (!elem->update_element(gtid))
      return 0;
  }
  else if (!alloc_element_nolock(gtid))
    return 0;

  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  return 1;
}


int
rpl_binlog_state::update(const struct rpl_gtid *gtid, bool strict)
{
  int res;
  mysql_mutex_lock(&LOCK_binlog_state);
  res= update_nolock(gtid, strict);
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


/*
  Fill in a new GTID, allocating next sequence number, and update state
  accordingly.
*/
int
rpl_binlog_state::update_with_next_gtid(uint32 domain_id, uint32 server_id,
                                        rpl_gtid *gtid)
{
  element *elem;
  int res= 0;

  gtid->domain_id= domain_id;
  gtid->server_id= server_id;

  mysql_mutex_lock(&LOCK_binlog_state);
  if ((elem= (element *)my_hash_search(&hash, (const uchar *)(&domain_id), 0)))
  {
    gtid->seq_no= ++elem->seq_no_counter;
    if (!elem->update_element(gtid))
      goto end;
  }
  else
  {
    gtid->seq_no= 1;
    if (!alloc_element_nolock(gtid))
      goto end;
  }

  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  res= 1;
end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


/* Helper functions for update. */
int
rpl_binlog_state::element::update_element(const rpl_gtid *gtid)
{
  rpl_gtid *lookup_gtid;

  /*
    By far the most common case is that successive events within same
    replication domain have the same server id (it changes only when
    switching to a new master). So save a hash lookup in this case.
  */
  if (likely(last_gtid && last_gtid->server_id == gtid->server_id))
  {
    last_gtid->seq_no= gtid->seq_no;
    return 0;
  }

  lookup_gtid= (rpl_gtid *)
    my_hash_search(&hash, (const uchar *)&gtid->server_id, 0);
  if (lookup_gtid)
  {
    lookup_gtid->seq_no= gtid->seq_no;
    last_gtid= lookup_gtid;
    return 0;
  }

  /* Allocate a new GTID and insert it. */
  lookup_gtid= (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*lookup_gtid),
                                     MYF(MY_WME));
  if (!lookup_gtid)
    return 1;
  memcpy(lookup_gtid, gtid, sizeof(*lookup_gtid));
  if (my_hash_insert(&hash, (const uchar *)lookup_gtid))
  {
    my_free(lookup_gtid);
    return 1;
  }
  last_gtid= lookup_gtid;
  return 0;
}


int
rpl_binlog_state::alloc_element_nolock(const rpl_gtid *gtid)
{
  element *elem;
  rpl_gtid *lookup_gtid;

  /* First time we see this domain_id; allocate a new element. */
  elem= (element *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*elem), MYF(MY_WME));
  lookup_gtid= (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*lookup_gtid),
                                     MYF(MY_WME));
  if (elem && lookup_gtid)
  {
    elem->domain_id= gtid->domain_id;
    my_hash_init(PSI_INSTRUMENT_ME, &elem->hash, &my_charset_bin, 32,
                 offsetof(rpl_gtid, server_id), sizeof(uint32), NULL, my_free,
                 HASH_UNIQUE);
    elem->last_gtid= lookup_gtid;
    elem->seq_no_counter= gtid->seq_no;
    memcpy(lookup_gtid, gtid, sizeof(*lookup_gtid));
    if (0 == my_hash_insert(&elem->hash, (const uchar *)lookup_gtid))
    {
      lookup_gtid= NULL;                        /* Do not free. */
      if (0 == my_hash_insert(&hash, (const uchar *)elem))
        return 0;
    }
    my_hash_free(&elem->hash);
  }

  /* An error. */
  if (elem)
    my_free(elem);
  if (lookup_gtid)
    my_free(lookup_gtid);
  return 1;
}


/*
  Check that a new GTID can be logged without creating an out-of-order
  sequence number with existing GTIDs.
*/
bool
rpl_binlog_state::check_strict_sequence(uint32 domain_id, uint32 server_id,
                                        uint64 seq_no)
{
  element *elem;
  bool res= 0;

  mysql_mutex_lock(&LOCK_binlog_state);
  if ((elem= (element *)my_hash_search(&hash,
                                       (const uchar *)(&domain_id), 0)) &&
      elem->last_gtid && elem->last_gtid->seq_no >= seq_no)
  {
    my_error(ER_GTID_STRICT_OUT_OF_ORDER, MYF(0), domain_id, server_id, seq_no,
             elem->last_gtid->domain_id, elem->last_gtid->server_id,
             elem->last_gtid->seq_no);
    res= 1;
  }
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


/*
  When we see a new GTID that will not be binlogged (eg. slave thread
  with --log-slave-updates=0), then we need to remember to allocate any
  GTID seq_no of our own within that domain starting from there.

  Returns 0 if ok, non-zero if out-of-memory.
*/
int
rpl_binlog_state::bump_seq_no_if_needed(uint32 domain_id, uint64 seq_no)
{
  element *elem;
  int res;

  mysql_mutex_lock(&LOCK_binlog_state);
  if ((elem= (element *)my_hash_search(&hash, (const uchar *)(&domain_id), 0)))
  {
    if (elem->seq_no_counter < seq_no)
      elem->seq_no_counter= seq_no;
    res= 0;
    goto end;
  }

  /* We need to allocate a new, empty element to remember the next seq_no. */
  if (!(elem= (element *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*elem),
                                   MYF(MY_WME))))
  {
    res= 1;
    goto end;
  }

  elem->domain_id= domain_id;
  my_hash_init(PSI_INSTRUMENT_ME, &elem->hash, &my_charset_bin, 32,
               offsetof(rpl_gtid, server_id), sizeof(uint32), NULL, my_free,
               HASH_UNIQUE);
  elem->last_gtid= NULL;
  elem->seq_no_counter= seq_no;
  if (0 == my_hash_insert(&hash, (const uchar *)elem))
  {
    res= 0;
    goto end;
  }

  my_hash_free(&elem->hash);
  my_free(elem);
  res= 1;

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


/*
  Write binlog state to text file, so we can read it in again without having
  to scan last binlog file (normal shutdown/startup, not crash recovery).

  The most recent GTID within each domain_id is written after any other GTID
  within this domain.
*/
int
rpl_binlog_state::write_to_iocache(IO_CACHE *dest)
{
  ulong i, j;
  char buf[21];
  int res= 0;

  mysql_mutex_lock(&LOCK_binlog_state);
  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
    {
      DBUG_ASSERT(e->hash.records == 0);
      continue;
    }
    for (j= 0; j <= e->hash.records; ++j)
    {
      const rpl_gtid *gtid;
      if (j < e->hash.records)
      {
        gtid= (const rpl_gtid *)my_hash_element(&e->hash, j);
        if (gtid == e->last_gtid)
          continue;
      }
      else
        gtid= e->last_gtid;

      longlong10_to_str(gtid->seq_no, buf, 10);
      if (my_b_printf(dest, "%u-%u-%s\n", gtid->domain_id, gtid->server_id,
                      buf))
      {
        res= 1;
        goto end;
      }
    }
  }

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


int
rpl_binlog_state::read_from_iocache(IO_CACHE *src)
{
  /* 10-digit - 10-digit - 20-digit \n \0 */
  char buf[10+1+10+1+20+1+1];
  const char *p, *end;
  rpl_gtid gtid;
  int res= 0;

  mysql_mutex_lock(&LOCK_binlog_state);
  reset_nolock();
  for (;;)
  {
    size_t len= my_b_gets(src, buf, sizeof(buf));
    if (!len)
      break;
    p= buf;
    end= buf + len;
    if (gtid_parser_helper(&p, end, &gtid) ||
        update_nolock(&gtid, false))
    {
      res= 1;
      break;
    }
  }
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


rpl_gtid *
rpl_binlog_state::find_nolock(uint32 domain_id, uint32 server_id)
{
  element *elem;
  if (!(elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0)))
    return NULL;
  return (rpl_gtid *)my_hash_search(&elem->hash, (const uchar *)&server_id, 0);
}

rpl_gtid *
rpl_binlog_state::find(uint32 domain_id, uint32 server_id)
{
  rpl_gtid *p;
  mysql_mutex_lock(&LOCK_binlog_state);
  p= find_nolock(domain_id, server_id);
  mysql_mutex_unlock(&LOCK_binlog_state);
  return p;
}

rpl_gtid *
rpl_binlog_state::find_most_recent(uint32 domain_id)
{
  element *elem;
  rpl_gtid *gtid= NULL;

  mysql_mutex_lock(&LOCK_binlog_state);
  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (elem && elem->last_gtid)
    gtid= elem->last_gtid;
  mysql_mutex_unlock(&LOCK_binlog_state);

  return gtid;
}


uint32
rpl_binlog_state::count()
{
  uint32 c= 0;
  uint32 i;

  mysql_mutex_lock(&LOCK_binlog_state);
  for (i= 0; i < hash.records; ++i)
    c+= ((element *)my_hash_element(&hash, i))->hash.records;
  mysql_mutex_unlock(&LOCK_binlog_state);

  return c;
}


int
rpl_binlog_state::get_gtid_list(rpl_gtid *gtid_list, uint32 list_size)
{
  uint32 i, j, pos;
  int res= 0;

  mysql_mutex_lock(&LOCK_binlog_state);
  pos= 0;
  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
    {
      DBUG_ASSERT(e->hash.records==0);
      continue;
    }
    for (j= 0; j <= e->hash.records; ++j)
    {
      const rpl_gtid *gtid;
      if (j < e->hash.records)
      {
        gtid= (rpl_gtid *)my_hash_element(&e->hash, j);
        if (gtid == e->last_gtid)
          continue;
      }
      else
        gtid= e->last_gtid;

      if (pos >= list_size)
      {
        res= 1;
        goto end;
      }
      memcpy(&gtid_list[pos++], gtid, sizeof(*gtid));
    }
  }

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}


/*
  Get a list of the most recently binlogged GTID, for each domain_id.

  This can be used when switching from being a master to being a slave,
  to know where to start replicating from the new master.

  The returned list must be de-allocated with my_free().

  Returns 0 for ok, non-zero for out-of-memory.
*/
int
rpl_binlog_state::get_most_recent_gtid_list(rpl_gtid **list, uint32 *size)
{
  uint32 i;
  uint32 alloc_size, out_size;
  int res= 0;

  out_size= 0;
  mysql_mutex_lock(&LOCK_binlog_state);
  alloc_size= hash.records;
  if (!(*list= (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME,
                                     alloc_size * sizeof(rpl_gtid), MYF(MY_WME))))
  {
    res= 1;
    goto end;
  }
  for (i= 0; i < alloc_size; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
      continue;
    memcpy(&((*list)[out_size++]), e->last_gtid, sizeof(rpl_gtid));
  }

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  *size= out_size;
  return res;
}

bool
rpl_binlog_state::append_pos(String *str)
{
  uint32 i;

  mysql_mutex_lock(&LOCK_binlog_state);
  reset_dynamic(&gtid_sort_array);

  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (e->last_gtid &&
        insert_dynamic(&gtid_sort_array, (const void *) e->last_gtid))
    {
      mysql_mutex_unlock(&LOCK_binlog_state);
      return true;
    }
  }
  rpl_slave_state_tostring_helper(&gtid_sort_array, str);
  mysql_mutex_unlock(&LOCK_binlog_state);

  return false;
}


bool
rpl_binlog_state::append_state(String *str)
{
  uint32 i, j;
  bool res= false;

  mysql_mutex_lock(&LOCK_binlog_state);
  reset_dynamic(&gtid_sort_array);

  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
    {
      DBUG_ASSERT(e->hash.records==0);
      continue;
    }
    for (j= 0; j <= e->hash.records; ++j)
    {
      const rpl_gtid *gtid;
      if (j < e->hash.records)
      {
        gtid= (rpl_gtid *)my_hash_element(&e->hash, j);
        if (gtid == e->last_gtid)
          continue;
      }
      else
        gtid= e->last_gtid;

      if (insert_dynamic(&gtid_sort_array, (const void *) gtid))
      {
        res= true;
        goto end;
      }
    }
  }

  rpl_slave_state_tostring_helper(&gtid_sort_array, str);

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  return res;
}

/**
  Remove domains supplied by the first argument from binlog state.
  Removal is done for any domain whose last gtids (from all its servers) match
  ones in Gtid list event of the 2nd argument.

  @param  ids               gtid domain id sequence, may contain dups
  @param  glev              pointer to Gtid list event describing
                            the match condition
  @param  errbuf [out]      pointer to possible error message array

  @retval NULL              as success when at least one domain is removed
  @retval ""                empty string to indicate ineffective call
                            when no domains removed
  @retval NOT EMPTY string  otherwise an error message
*/
const char*
rpl_binlog_state::drop_domain(DYNAMIC_ARRAY *ids,
                              Gtid_list_log_event *glev,
                              char* errbuf)
{
  DYNAMIC_ARRAY domain_unique; // sequece (unsorted) of unique element*:s
  rpl_binlog_state::element* domain_unique_buffer[16];
  ulong k, l;
  const char* errmsg= NULL;

  DBUG_ENTER("rpl_binlog_state::drop_domain");

  my_init_dynamic_array2(PSI_INSTRUMENT_ME, &domain_unique,
                         sizeof(element*), domain_unique_buffer,
                         sizeof(domain_unique_buffer) / sizeof(element*), 4, 0);

  mysql_mutex_lock(&LOCK_binlog_state);

  /*
    Gtid list is supposed to come from a binlog's Gtid_list event and
    therefore should be a subset of the current binlog state. That is
    for every domain in the list the binlog state contains a gtid with
    sequence number not less than that of the list.
    Exceptions of this inclusion rule are:
      A. the list may still refer to gtids from already deleted domains.
         Files containing them must have been purged whereas the file
         with the list is not yet.
      B. out of order groups were injected
      C. manually build list of binlog files violating the inclusion
         constraint.
    While A is a normal case (not necessarily distinguishable from C though),
    B and C may require the user's attention so any (incl the A's suspected)
    inconsistency is diagnosed and *warned*.
  */
  for (l= 0, errbuf[0]= 0; l < glev->count; l++, errbuf[0]= 0)
  {
    rpl_gtid* rb_state_gtid= find_nolock(glev->list[l].domain_id,
                                         glev->list[l].server_id);
    if (!rb_state_gtid)
      sprintf(errbuf,
              "missing gtids from the '%u-%u' domain-server pair which is "
              "referred to in the gtid list describing an earlier state. Ignore "
              "if the domain ('%u') was already explicitly deleted",
              glev->list[l].domain_id, glev->list[l].server_id,
              glev->list[l].domain_id);
    else if (rb_state_gtid->seq_no < glev->list[l].seq_no)
      sprintf(errbuf,
              "having a gtid '%u-%u-%llu' which is less than "
              "the '%u-%u-%llu' of the gtid list describing an earlier state. "
              "The state may have been affected by manually injecting "
              "a lower sequence number gtid or via replication",
              rb_state_gtid->domain_id, rb_state_gtid->server_id,
              rb_state_gtid->seq_no, glev->list[l].domain_id,
              glev->list[l].server_id, glev->list[l].seq_no);
    if (strlen(errbuf)) // use strlen() as cheap flag
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BINLOG_CANT_DELETE_GTID_DOMAIN,
                          "The current gtid binlog state is incompatible with "
                          "a former one %s.", errbuf);
  }

  /*
    For each domain_id from ids
      when no such domain in binlog state
        warn && continue
      For each domain.server's last gtid
        when not locate the last gtid in glev.list
          error out binlog state can't change
        otherwise continue
  */
  for (ulong i= 0; i < ids->elements; i++)
  {
    rpl_binlog_state::element *elem= NULL;
    uint32 *ptr_domain_id;
    bool not_match;

    ptr_domain_id= (uint32*) dynamic_array_ptr(ids, i);
    elem= (rpl_binlog_state::element *)
      my_hash_search(&hash, (const uchar *) ptr_domain_id, 0);
    if (!elem)
    {
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_BINLOG_CANT_DELETE_GTID_DOMAIN,
                          "The gtid domain being deleted ('%lu') is not in "
                          "the current binlog state", *ptr_domain_id);
      continue;
    }

    for (not_match= true, k= 0; k < elem->hash.records; k++)
    {
      rpl_gtid *d_gtid= (rpl_gtid *)my_hash_element(&elem->hash, k);
      for (ulong l= 0; l < glev->count && not_match; l++)
        not_match= !(*d_gtid == glev->list[l]);
    }

    if (not_match)
    {
      sprintf(errbuf, "binlog files may contain gtids from the domain ('%u') "
              "being deleted. Make sure to first purge those files",
              *ptr_domain_id);
      errmsg= errbuf;
      goto end;
    }
    // compose a sequence of unique pointers to domain object
    for (k= 0; k < domain_unique.elements; k++)
    {
      if ((rpl_binlog_state::element*) dynamic_array_ptr(&domain_unique, k)
          == elem)
        break; // domain_id's elem has been already in
    }
    if (k == domain_unique.elements) // proven not to have duplicates
      insert_dynamic(&domain_unique, (uchar*) &elem);
  }

  // Domain removal from binlog state
  for (k= 0; k < domain_unique.elements; k++)
  {
    rpl_binlog_state::element *elem= *(rpl_binlog_state::element**)
      dynamic_array_ptr(&domain_unique, k);
    my_hash_free(&elem->hash);
    my_hash_delete(&hash, (uchar*) elem);
  }

  DBUG_ASSERT(strlen(errbuf) == 0);

  if (domain_unique.elements == 0)
    errmsg= "";

end:
  mysql_mutex_unlock(&LOCK_binlog_state);
  delete_dynamic(&domain_unique);

  DBUG_RETURN(errmsg);
}

slave_connection_state::slave_connection_state()
{
  my_hash_init(PSI_INSTRUMENT_ME, &hash, &my_charset_bin, 32,
               offsetof(entry, gtid) + offsetof(rpl_gtid, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &gtid_sort_array, sizeof(rpl_gtid), 8, 8, MYF(0));
}


slave_connection_state::~slave_connection_state()
{
  my_hash_free(&hash);
  delete_dynamic(&gtid_sort_array);
}


/*
  Create a hash from the slave GTID state that is sent to master when slave
  connects to start replication.

  The state is sent as <GTID>,<GTID>,...,<GTID>, for example:

     0-2-112,1-4-1022

  The state gives for each domain_id the GTID to start replication from for
  the corresponding replication stream. So domain_id must be unique.

  Returns 0 if ok, non-zero if error due to malformed input.

  Note that input string is built by slave server, so it will not be incorrect
  unless bug/corruption/malicious server. So we just need basic sanity check,
  not fancy user-friendly error message.
*/

int
slave_connection_state::load(const char *slave_request, size_t len)
{
  const char *p, *end;
  uchar *rec;
  rpl_gtid *gtid;
  const entry *e;

  reset();
  p= slave_request;
  end= slave_request + len;
  if (p == end)
    return 0;
  for (;;)
  {
    if (!(rec= (uchar *)my_malloc(PSI_INSTRUMENT_ME, sizeof(entry), MYF(MY_WME))))
      return 1;
    gtid= &((entry *)rec)->gtid;
    if (gtid_parser_helper(&p, end, gtid))
    {
      my_free(rec);
      my_error(ER_INCORRECT_GTID_STATE, MYF(0));
      return 1;
    }
    if ((e= (const entry *)
         my_hash_search(&hash, (const uchar *)(&gtid->domain_id), 0)))
    {
      my_error(ER_DUPLICATE_GTID_DOMAIN, MYF(0), gtid->domain_id,
               gtid->server_id, (ulonglong)gtid->seq_no, e->gtid.domain_id,
               e->gtid.server_id, (ulonglong)e->gtid.seq_no, gtid->domain_id);
      my_free(rec);
      return 1;
    }
    ((entry *)rec)->flags= 0;
    if (my_hash_insert(&hash, rec))
    {
      my_free(rec);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return 1;
    }
    if (p == end)
      break;                                         /* Finished. */
    if (*p != ',')
    {
      my_error(ER_INCORRECT_GTID_STATE, MYF(0));
      return 1;
    }
    ++p;
  }

  return 0;
}


int
slave_connection_state::load(const rpl_gtid *gtid_list, uint32 count)
{
  uint32 i;

  reset();
  for (i= 0; i < count; ++i)
    if (update(&gtid_list[i]))
      return 1;
  return 0;
}


static int
slave_connection_state_load_cb(rpl_gtid *gtid, void *data)
{
  slave_connection_state *state= (slave_connection_state *)data;
  return state->update(gtid);
}


/*
  Same as rpl_slave_state::tostring(), but populates a slave_connection_state
  instead.
*/
int
slave_connection_state::load(rpl_slave_state *state,
                             rpl_gtid *extra_gtids, uint32 num_extra)
{
  reset();
  return state->iterate(slave_connection_state_load_cb, this,
                        extra_gtids, num_extra, false);
}


slave_connection_state::entry *
slave_connection_state::find_entry(uint32 domain_id)
{
  return (entry *) my_hash_search(&hash, (const uchar *)(&domain_id), 0);
}


rpl_gtid *
slave_connection_state::find(uint32 domain_id)
{
  entry *e= find_entry(domain_id);
  if (!e)
    return NULL;
  return &e->gtid;
}


int
slave_connection_state::update(const rpl_gtid *in_gtid)
{
  entry *e;
  uchar *rec= my_hash_search(&hash, (const uchar *)(&in_gtid->domain_id), 0);
  if (rec)
  {
    e= (entry *)rec;
    e->gtid= *in_gtid;
    return 0;
  }

  if (!(e= (entry *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*e), MYF(MY_WME))))
    return 1;
  e->gtid= *in_gtid;
  e->flags= 0;
  if (my_hash_insert(&hash, (uchar *)e))
  {
    my_free(e);
    return 1;
  }

  return 0;
}


void
slave_connection_state::remove(const rpl_gtid *in_gtid)
{
  uchar *rec= my_hash_search(&hash, (const uchar *)(&in_gtid->domain_id), 0);
#ifdef DBUG_ASSERT_EXISTS
  bool err;
  rpl_gtid *slave_gtid= &((entry *)rec)->gtid;
  DBUG_ASSERT(rec /* We should never try to remove not present domain_id. */);
  DBUG_ASSERT(slave_gtid->server_id == in_gtid->server_id);
  DBUG_ASSERT(slave_gtid->seq_no == in_gtid->seq_no);
  err= 
#endif
    my_hash_delete(&hash, rec);
  DBUG_ASSERT(!err);
}


void
slave_connection_state::remove_if_present(const rpl_gtid *in_gtid)
{
  uchar *rec= my_hash_search(&hash, (const uchar *)(&in_gtid->domain_id), 0);
  if (rec)
    my_hash_delete(&hash, rec);
}


int
slave_connection_state::to_string(String *out_str)
{
  out_str->length(0);
  return append_to_string(out_str);
}


int
slave_connection_state::append_to_string(String *out_str)
{
  uint32 i;
  bool first;

  first= true;
  for (i= 0; i < hash.records; ++i)
  {
    const entry *e= (const entry *)my_hash_element(&hash, i);
    if (rpl_slave_state_tostring_helper(out_str, &e->gtid, &first))
      return 1;
  }
  return 0;
}


int
slave_connection_state::get_gtid_list(rpl_gtid *gtid_list, uint32 list_size)
{
  uint32 i, pos;

  pos= 0;
  for (i= 0; i < hash.records; ++i)
  {
    entry *e;
    if (pos >= list_size)
      return 1;
    e= (entry *)my_hash_element(&hash, i);
    memcpy(&gtid_list[pos++], &e->gtid, sizeof(e->gtid));
  }

  return 0;
}


/*
  Check if the GTID position has been reached, for mysql_binlog_send().

  The position has not been reached if we have anything in the state, unless
  it has either the START_ON_EMPTY_DOMAIN flag set (which means it does not
  belong to this master at all), or the START_OWN_SLAVE_POS (which means that
  we start on an old position from when the server was a slave with
  --log-slave-updates=0).
*/
bool
slave_connection_state::is_pos_reached()
{
  uint32 i;

  for (i= 0; i < hash.records; ++i)
  {
    entry *e= (entry *)my_hash_element(&hash, i);
    if (!(e->flags & (START_OWN_SLAVE_POS|START_ON_EMPTY_DOMAIN)))
      return false;
  }

  return true;
}


/*
  Execute a MASTER_GTID_WAIT().
  The position to wait for is in gtid_str in string form.
  The timeout in microseconds is in timeout_us, zero means no timeout.

  Returns:
   1 for error.
   0 for wait completed.
  -1 for wait timed out.
*/
int
gtid_waiting::wait_for_pos(THD *thd, String *gtid_str, longlong timeout_us)
{
  int err;
  rpl_gtid *wait_pos;
  uint32 count, i;
  struct timespec wait_until, *wait_until_ptr;
  ulonglong before;

  /* Wait for the empty position returns immediately. */
  if (gtid_str->length() == 0)
  {
    status_var_increment(thd->status_var.master_gtid_wait_count);
    return 0;
  }

  if (!(wait_pos= gtid_parse_string_to_list(gtid_str->ptr(), gtid_str->length(),
                                            &count)))
  {
    my_error(ER_INCORRECT_GTID_STATE, MYF(0));
    return 1;
  }
  status_var_increment(thd->status_var.master_gtid_wait_count);
  before= microsecond_interval_timer();

  if (timeout_us >= 0)
  {
    set_timespec_nsec(wait_until, (ulonglong)1000*timeout_us);
    wait_until_ptr= &wait_until;
  }
  else
    wait_until_ptr= NULL;
  err= 0;
  for (i= 0; i < count; ++i)
  {
    if ((err= wait_for_gtid(thd, &wait_pos[i], wait_until_ptr)))
      break;
  }
  switch (err)
  {
    case -1:
      status_var_increment(thd->status_var.master_gtid_wait_timeouts);
      /* fall through */
    case 0:
      status_var_add(thd->status_var.master_gtid_wait_time,
                     static_cast<ulong>
                     (microsecond_interval_timer() - before));
  }
  my_free(wait_pos);
  return err;
}


void
gtid_waiting::promote_new_waiter(gtid_waiting::hash_element *he)
{
  queue_element *qe;

  mysql_mutex_assert_owner(&LOCK_gtid_waiting);
  if (queue_empty(&he->queue))
    return;
  qe= (queue_element *)queue_top(&he->queue);
  qe->do_small_wait= true;
  mysql_cond_signal(&qe->thd->COND_wakeup_ready);
}

void
gtid_waiting::process_wait_hash(uint64 wakeup_seq_no,
                                gtid_waiting::hash_element *he)
{
  mysql_mutex_assert_owner(&LOCK_gtid_waiting);

  for (;;)
  {
    queue_element *qe;

    if (queue_empty(&he->queue))
      break;
    qe= (queue_element *)queue_top(&he->queue);
    if (qe->wait_seq_no > wakeup_seq_no)
      break;
    DBUG_ASSERT(!qe->done);
    queue_remove_top(&he->queue);
    qe->done= true;;
    mysql_cond_signal(&qe->thd->COND_wakeup_ready);
  }
}


/*
  Execute a MASTER_GTID_WAIT() for one specific domain.

  The implementation is optimised primarily for (1) minimal performance impact
  on the slave replication threads, and secondarily for (2) quick performance
  of MASTER_GTID_WAIT() on a single GTID, which can be useful for consistent
  read to clients in an async replication read-scaleout scenario.

  To achieve (1), we have a "small" wait and a "large" wait. The small wait
  contends with the replication threads on the lock on the gtid_slave_pos, so
  only minimal processing is done under that lock, and only a single waiter at
  a time does the small wait.

  If there is already a small waiter, a new thread will either replace the
  small waiter (if it needs to wait for an earlier sequence number), or
  instead do a "large" wait.

  Once awoken on the small wait, the waiting thread releases the lock shared
  with the SQL threads quickly, and then processes all waiters currently doing
  the large wait using a different lock that does not impact replication.

  This way, the SQL threads only need to do a single check + possibly a
  pthread_cond_signal() when updating the gtid_slave_state, and the time that
  non-SQL threads contend for the lock on gtid_slave_state is minimized.

  There is always at least one thread that has the responsibility to ensure
  that there is a small waiter; this thread has queue_element::do_small_wait
  set to true. This thread will do the small wait until it is done, at which
  point it will make sure to pass on the responsibility to another thread.
  Normally only one thread has do_small_wait==true, but it can occasionally
  happen that there is more than one, when threads race one another for the
  lock on the small wait (this results in slightly increased activity on the
  small lock but is otherwise harmless).

  Returns:
     0  Wait completed normally
    -1  Wait completed due to timeout
     1  An error (my_error() will have been called to set the error in the da)
*/
int
gtid_waiting::wait_for_gtid(THD *thd, rpl_gtid *wait_gtid,
                            struct timespec *wait_until)
{
  bool timed_out= false;
#ifdef HAVE_REPLICATION
  queue_element elem;
  uint32 domain_id= wait_gtid->domain_id;
  uint64 seq_no= wait_gtid->seq_no;
  hash_element *he;
  rpl_slave_state::element *slave_state_elem= NULL;
  PSI_stage_info old_stage;
  bool did_enter_cond= false;

  elem.wait_seq_no= seq_no;
  elem.thd= thd;
  elem.done= false;

  mysql_mutex_lock(&LOCK_gtid_waiting);
  if (!(he= get_entry(wait_gtid->domain_id)))
  {
    mysql_mutex_unlock(&LOCK_gtid_waiting);
    return 1;
  }
  /*
    If there is already another waiter with seq_no no larger than our own,
    we are sure that there is already a small waiter that will wake us up
    (or later pass the small wait responsibility to us). So in this case, we
    do not need to touch the small wait lock at all.
  */
  elem.do_small_wait=
    (queue_empty(&he->queue) ||
     ((queue_element *)queue_top(&he->queue))->wait_seq_no > seq_no);

  if (register_in_wait_queue(thd, wait_gtid, he, &elem))
  {
    mysql_mutex_unlock(&LOCK_gtid_waiting);
    return 1;
  }
  /*
    Loop, doing either the small or large wait as appropriate, until either
    the position waited for is reached, or we get a kill or timeout.
  */
  for (;;)
  {
    mysql_mutex_assert_owner(&LOCK_gtid_waiting);

    if (elem.do_small_wait)
    {
      uint64 wakeup_seq_no;
      queue_element *cur_waiter;

      mysql_mutex_lock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      /*
        The elements in the gtid_slave_state_hash are never re-allocated once
        they enter the hash, so we do not need to re-do the lookup after releasing
        and re-aquiring the lock.
      */
      if (!slave_state_elem &&
          !(slave_state_elem= rpl_global_gtid_slave_state->get_element(domain_id)))
      {
        mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
        remove_from_wait_queue(he, &elem);
        promote_new_waiter(he);
        if (did_enter_cond)
          thd->EXIT_COND(&old_stage);
        else
          mysql_mutex_unlock(&LOCK_gtid_waiting);
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        return 1;
      }

      if ((wakeup_seq_no= slave_state_elem->highest_seq_no) >= seq_no)
      {
        /*
          We do not have to wait. (We will be removed from the wait queue when
          we call process_wait_hash() below.
        */
        mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      }
      else if ((cur_waiter= slave_state_elem->gtid_waiter) &&
               slave_state_elem->min_wait_seq_no <= seq_no)
      {
        /*
          There is already a suitable small waiter, go do the large wait.
          (Normally we would not have needed to check the small wait in this
          case, but it can happen if we race with another thread for the small
          lock).
        */
        elem.do_small_wait= false;
        mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      }
      else
      {
        /*
          We have to do the small wait ourselves (stealing it from any thread
          that might already be waiting for a later seq_no).
        */
        slave_state_elem->gtid_waiter= &elem;
        slave_state_elem->min_wait_seq_no= seq_no;
        if (cur_waiter)
        {
          /* We stole the wait, so wake up the old waiting thread. */
          mysql_cond_signal(&slave_state_elem->COND_wait_gtid);
        }

        /* Release the large lock, and do the small wait. */
        if (did_enter_cond)
        {
          thd->EXIT_COND(&old_stage);
          did_enter_cond= false;
        }
        else
          mysql_mutex_unlock(&LOCK_gtid_waiting);
        thd->ENTER_COND(&slave_state_elem->COND_wait_gtid,
                        &rpl_global_gtid_slave_state->LOCK_slave_state,
                        &stage_master_gtid_wait_primary, &old_stage);
        do
        {
          if (unlikely(thd->check_killed(1)))
            break;
          else if (wait_until)
          {
            int err=
              mysql_cond_timedwait(&slave_state_elem->COND_wait_gtid,
                                   &rpl_global_gtid_slave_state->LOCK_slave_state,
                                   wait_until);
            if (err == ETIMEDOUT || err == ETIME)
            {
              timed_out= true;
              break;
            }
          }
          else
            mysql_cond_wait(&slave_state_elem->COND_wait_gtid,
                            &rpl_global_gtid_slave_state->LOCK_slave_state);
        } while (slave_state_elem->gtid_waiter == &elem);
        wakeup_seq_no= slave_state_elem->highest_seq_no;
        /*
          If we aborted due to timeout or kill, remove us as waiter.

          If we were replaced by another waiter with a smaller seq_no, then we
          no longer have responsibility for the small wait.
        */
        if ((cur_waiter= slave_state_elem->gtid_waiter))
        {
          if (cur_waiter == &elem)
            slave_state_elem->gtid_waiter= NULL;
          else if (slave_state_elem->min_wait_seq_no <= seq_no)
            elem.do_small_wait= false;
        }
        thd->EXIT_COND(&old_stage);

        mysql_mutex_lock(&LOCK_gtid_waiting);
      }

      /*
        Note that hash_entry pointers do not change once allocated, so we do
        not need to lookup `he' again after re-aquiring LOCK_gtid_waiting.
      */
      process_wait_hash(wakeup_seq_no, he);
    }
    else
    {
      /* Do the large wait. */
      if (!did_enter_cond)
      {
        thd->ENTER_COND(&thd->COND_wakeup_ready, &LOCK_gtid_waiting,
                        &stage_master_gtid_wait, &old_stage);
        did_enter_cond= true;
      }
      while (!elem.done && likely(!thd->check_killed(1)))
      {
        thd_wait_begin(thd, THD_WAIT_BINLOG);
        if (wait_until)
        {
          int err= mysql_cond_timedwait(&thd->COND_wakeup_ready,
                                        &LOCK_gtid_waiting, wait_until);
          if (err == ETIMEDOUT || err == ETIME)
            timed_out= true;
        }
        else
          mysql_cond_wait(&thd->COND_wakeup_ready, &LOCK_gtid_waiting);
        thd_wait_end(thd);
        if (elem.do_small_wait || timed_out)
          break;
      }
    }

    if ((thd->killed || timed_out) && !elem.done)
    {
      /* Aborted, so remove ourselves from the hash. */
      remove_from_wait_queue(he, &elem);
      elem.done= true;
    }
    if (elem.done)
    {
      /*
        If our wait is done, but we have (or were passed) responsibility for
        the small wait, then we need to pass on that task to someone else.
      */
      if (elem.do_small_wait)
        promote_new_waiter(he);
      break;
    }
  }

  if (did_enter_cond)
    thd->EXIT_COND(&old_stage);
  else
    mysql_mutex_unlock(&LOCK_gtid_waiting);
  if (thd->killed)
    thd->send_kill_message();
#endif  /* HAVE_REPLICATION */
  return timed_out ? -1 : 0;
}


static void
free_hash_element(void *p)
{
  gtid_waiting::hash_element *e= (gtid_waiting::hash_element *)p;
  delete_queue(&e->queue);
  my_free(e);
}


void
gtid_waiting::init()
{
  my_hash_init(PSI_INSTRUMENT_ME, &hash, &my_charset_bin, 32,
               offsetof(hash_element, domain_id), sizeof(uint32), NULL,
               free_hash_element, HASH_UNIQUE);
  mysql_mutex_init(key_LOCK_gtid_waiting, &LOCK_gtid_waiting, 0);
}


void
gtid_waiting::destroy()
{
  mysql_mutex_destroy(&LOCK_gtid_waiting);
  my_hash_free(&hash);
}


static int
cmp_queue_elem(void *, uchar *a, uchar *b)
{
  uint64 seq_no_a= *(uint64 *)a;
  uint64 seq_no_b= *(uint64 *)b;
  if (seq_no_a < seq_no_b)
    return -1;
  else if (seq_no_a == seq_no_b)
    return 0;
  else
    return 1;
}


gtid_waiting::hash_element *
gtid_waiting::get_entry(uint32 domain_id)
{
  hash_element *e;

  if ((e= (hash_element *)my_hash_search(&hash, (const uchar *)&domain_id, 0)))
    return e;

  if (!(e= (hash_element *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*e), MYF(MY_WME))))
    return NULL;

  if (init_queue(&e->queue, 8, offsetof(queue_element, wait_seq_no), 0,
                 cmp_queue_elem, NULL, 1+offsetof(queue_element, queue_idx), 1))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    my_free(e);
    return NULL;
  }
  e->domain_id= domain_id;
  if (my_hash_insert(&hash, (uchar *)e))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    delete_queue(&e->queue);
    my_free(e);
    return NULL;
  }
  return e;
}


int
gtid_waiting::register_in_wait_queue(THD *thd, rpl_gtid *wait_gtid,
                                     gtid_waiting::hash_element *he,
                                     gtid_waiting::queue_element *elem)
{
  mysql_mutex_assert_owner(&LOCK_gtid_waiting);

  if (queue_insert_safe(&he->queue, (uchar *)elem))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return 1;
  }

  return 0;
}


void
gtid_waiting::remove_from_wait_queue(gtid_waiting::hash_element *he,
                                     gtid_waiting::queue_element *elem)
{
  mysql_mutex_assert_owner(&LOCK_gtid_waiting);

  queue_remove(&he->queue, elem->queue_idx);
}
