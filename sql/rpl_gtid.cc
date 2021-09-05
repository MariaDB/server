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

#include <my_global.h>
#include "sql_priv.h"
#include "my_sys.h"
#include "unireg.h"
#include "my_global.h"
#include "sql_base.h"
#include "sql_parse.h"
#include "key.h"
#include "rpl_gtid.h"
#include "rpl_rli.h"
#include "log_event.h"

const LEX_STRING rpl_gtid_slave_state_table_name=
  { C_STRING_WITH_LEN("gtid_slave_pos") };


void
rpl_slave_state::update_state_hash(uint64 sub_id, rpl_gtid *gtid,
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
  err= update(gtid->domain_id, gtid->server_id, sub_id, gtid->seq_no, rgi);
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
    rgi->gtid_pending= false;
    if (rgi->gtid_ignore_duplicate_state!=rpl_group_info::GTID_DUPLICATE_IGNORE)
    {
      if (record_gtid(thd, &rgi->current_gtid, sub_id, NULL, false))
        DBUG_RETURN(1);
      update_state_hash(sub_id, &rgi->current_gtid, rgi);
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
    if (thd->check_killed())
    {
      thd->send_kill_message();
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
  : last_sub_id(0), loaded(false)
{
  mysql_mutex_init(key_LOCK_slave_state, &LOCK_slave_state,
                   MY_MUTEX_INIT_SLOW);
  my_hash_init(&hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, rpl_slave_state_free_element, HASH_UNIQUE);
  my_init_dynamic_array(&gtid_sort_array, sizeof(rpl_gtid), 8, 8, MYF(0));
}


rpl_slave_state::~rpl_slave_state()
{
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
                        uint64 seq_no, rpl_group_info *rgi)
{
  element *elem= NULL;
  list_element *list_elem= NULL;

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
#ifndef DBUG_OFF
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

#ifdef HAVE_REPLICATION
    rgi->pending_gtid_deletes_clear();
#endif
  }

  if (!(list_elem= (list_element *)my_malloc(sizeof(*list_elem), MYF(MY_WME))))
    return 1;
  list_elem->server_id= server_id;
  list_elem->sub_id= sub_id;
  list_elem->seq_no= seq_no;

  elem->add(list_elem);
  if (last_sub_id < sub_id)
    last_sub_id= sub_id;

  return 0;
}


struct rpl_slave_state::element *
rpl_slave_state::get_element(uint32 domain_id)
{
  struct element *elem;

  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (elem)
    return elem;

  if (!(elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME))))
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
rpl_slave_state::put_back_list(uint32 domain_id, list_element *list)
{
  element *e;
  int err= 0;

  mysql_mutex_lock(&LOCK_slave_state);
  if (!(e= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0)))
  {
    err= 1;
    goto end;
  }
  while (list)
  {
    list_element *next= list->next;
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

  tmp_disable_binlog(thd);
  tlist.init_one_table(STRING_WITH_LEN("mysql"),
                       rpl_gtid_slave_state_table_name.str,
                       rpl_gtid_slave_state_table_name.length,
                       NULL, TL_WRITE);
  if (!(err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
  {
    tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED, "mysql",
                     rpl_gtid_slave_state_table_name.str, false);
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
    thd->release_transactional_locks();
  }

  reenable_binlog(thd);
  return err;
}


static const TABLE_FIELD_TYPE mysql_rpl_slave_state_coltypes[4]= {
  { { C_STRING_WITH_LEN("domain_id") },
    { C_STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("sub_id") },
    { C_STRING_WITH_LEN("bigint(20) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("server_id") },
    { C_STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("seq_no") },
    { C_STRING_WITH_LEN("bigint(20) unsigned") },
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
  Write a gtid to the replication slave state table.

    gtid    The global transaction id for this event group.
    sub_id  Value allocated within the sub_id when the event group was
            read (sub_id must be consistent with commit order in master binlog).
    rgi     rpl_group_info context, if we are recording the gtid transactionally
            as part of replicating a transactional event. NULL if called from
            outside of a replicated transaction.

  Note that caller must later ensure that the new gtid and sub_id is inserted
  into the appropriate HASH element with rpl_slave_state.add(), so that it can
  be deleted later. But this must only be done after COMMIT if in transaction.
*/
int
rpl_slave_state::record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                             rpl_group_info *rgi, bool in_statement)
{
  TABLE_LIST tlist;
  int err= 0;
  bool table_opened= false;
  TABLE *table;
  list_element *elist= 0, *cur, *next;
  element *elem;
  ulonglong thd_saved_option= thd->variables.option_bits;
  Query_tables_list lex_backup;
  wait_for_commit* suspended_wfc;
  DBUG_ENTER("record_gtid");

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
  tlist.init_one_table(STRING_WITH_LEN("mysql"),
                       rpl_gtid_slave_state_table_name.str,
                       rpl_gtid_slave_state_table_name.length,
                       NULL, TL_WRITE);
  if ((err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
    goto end;
  table_opened= true;
  table= tlist.table;

  if ((err= gtid_check_rpl_slave_state_table(table)))
    goto end;

#ifdef WITH_WSREP
  /*
    Updates in slave state table should not be appended to galera transaction
    writeset.
  */
  thd->wsrep_ignore_table= true;
#endif

  if (!rgi)
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

  if(opt_bin_log &&
     (err= mysql_bin_log.bump_seq_no_counter_if_needed(gtid->domain_id,
                                                       gtid->seq_no)))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    goto end;
  }

  mysql_mutex_lock(&LOCK_slave_state);
  if ((elem= get_element(gtid->domain_id)) == NULL)
  {
    mysql_mutex_unlock(&LOCK_slave_state);
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    err= 1;
    goto end;
  }
  if ((elist= elem->grab_list()) != NULL)
  {
    /* Delete any old stuff, but keep around the most recent one. */
    uint64 best_sub_id= elist->sub_id;
    list_element **best_ptr_ptr= &elist;
    cur= elist;
    while ((next= cur->next))
    {
      if (next->sub_id > best_sub_id)
      {
        best_sub_id= next->sub_id;
        best_ptr_ptr= &cur->next;
      }
      cur= next;
    }
    /*
      Delete the highest sub_id element from the old list, and put it back as
      the single-element new list.
    */
    cur= *best_ptr_ptr;
    *best_ptr_ptr= cur->next;
    cur->next= NULL;
    elem->list= cur;
  }
  mysql_mutex_unlock(&LOCK_slave_state);

  if (!elist)
    goto end;

  /* Now delete any already committed rows. */
  bitmap_set_bit(table->read_set, table->field[0]->field_index);
  bitmap_set_bit(table->read_set, table->field[1]->field_index);

  if ((err= table->file->ha_index_init(0, 0)))
  {
    table->file->print_error(err, MYF(0));
    goto end;
  }
  cur = elist;
  while (cur)
  {
    uchar key_buffer[4+8];

    DBUG_EXECUTE_IF("gtid_slave_pos_simulate_failed_delete",
                    { err= ENOENT;
                      table->file->print_error(err, MYF(0));
                      /* `break' does not work inside DBUG_EXECUTE_IF */
                      goto dbug_break; });

    next= cur->next;

    table->field[1]->store(cur->sub_id, true);
    /* domain_id is already set in table->record[0] from write_row() above. */
    key_copy(key_buffer, table->record[0], &table->key_info[0], 0, false);
    if (table->file->ha_index_read_map(table->record[1], key_buffer,
                                       HA_WHOLE_KEY, HA_READ_KEY_EXACT))
      /* We cannot find the row, assume it is already deleted. */
      ;
    else if ((err= table->file->ha_delete_row(table->record[1])))
      table->file->print_error(err, MYF(0));
    /*
      In case of error, we still discard the element from the list. We do
      not want to endlessly error on the same element in case of table
      corruption or such.
    */
    cur= next;
    if (err)
      break;
  }
IF_DBUG(dbug_break:, )
  table->file->ha_index_end();

end:

#ifdef WITH_WSREP
  thd->wsrep_ignore_table= false;
#endif

  if (table_opened)
  {
    if (err || (err= ha_commit_trans(thd, FALSE)))
    {
      /*
        If error, we need to put any remaining elist back into the HASH so we
        can do another delete attempt later.
      */
      if (elist)
      {
        put_back_list(gtid->domain_id, elist);
        elist = 0;
      }

      ha_rollback_trans(thd, FALSE);
    }
    close_thread_tables(thd);
    if (rgi)
    {
      thd->mdl_context.release_statement_locks();
      /*
        Save the list of old gtid entries we deleted. If this transaction
        fails later for some reason and is rolled back, the deletion of those
        entries will be rolled back as well, and we will need to put them back
        on the to-be-deleted list so we can re-do the deletion. Otherwise
        redundant rows in mysql.gtid_slave_pos may accumulate if transactions
        are rolled back and retried after record_gtid().
      */
#ifdef HAVE_REPLICATION
      rgi->pending_gtid_deletes_save(gtid->domain_id, elist);
#endif
    }
    else
    {
      thd->release_transactional_locks();
#ifdef HAVE_REPLICATION
      rpl_group_info::pending_gtid_deletes_free(elist);
#endif
    }
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

  my_hash_init(&gtid_hash, &my_charset_bin, 32, offsetof(rpl_gtid, domain_id),
               sizeof(uint32), NULL, NULL, HASH_UNIQUE);
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
gtid_parser_helper(char **ptr, char *end, rpl_gtid *out_gtid)
{
  char *q;
  char *p= *ptr;
  uint64 v1, v2, v3;
  int err= 0;

  q= end;
  v1= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v1 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= end;
  v2= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v2 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= end;
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
  char *p= const_cast<char *>(str);
  char *end= p + str_len;
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
          (rpl_gtid *)my_realloc(list,
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
rpl_slave_state::load(THD *thd, char *state_from_master, size_t len,
                      bool reset, bool in_statement)
{
  char *end= state_from_master + len;

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

    if (gtid_parser_helper(&state_from_master, end, &gtid) ||
        !(sub_id= next_sub_id(gtid.domain_id)) ||
        record_gtid(thd, &gtid, sub_id, NULL, in_statement) ||
        update(gtid.domain_id, gtid.server_id, sub_id, gtid.seq_no, NULL))
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


void rpl_binlog_state::init()
{
  my_hash_init(&hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  my_init_dynamic_array(&gtid_sort_array, sizeof(rpl_gtid), 8, 8, MYF(0));
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
  lookup_gtid= (rpl_gtid *)my_malloc(sizeof(*lookup_gtid), MYF(MY_WME));
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
  elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME));
  lookup_gtid= (rpl_gtid *)my_malloc(sizeof(*lookup_gtid), MYF(MY_WME));
  if (elem && lookup_gtid)
  {
    elem->domain_id= gtid->domain_id;
    my_hash_init(&elem->hash, &my_charset_bin, 32,
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
  if (!(elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME))))
  {
    res= 1;
    goto end;
  }

  elem->domain_id= domain_id;
  my_hash_init(&elem->hash, &my_charset_bin, 32,
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
    size_t res;
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
      res= my_b_printf(dest, "%u-%u-%s\n", gtid->domain_id, gtid->server_id, buf);
      if (res == (size_t) -1)
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
  char *p, *end;
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
  if (!(*list= (rpl_gtid *)my_malloc(alloc_size * sizeof(rpl_gtid),
                                     MYF(MY_WME))))
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

  my_init_dynamic_array2(&domain_unique,
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
                          "the current binlog state", (unsigned long) *ptr_domain_id);
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
  my_hash_init(&hash, &my_charset_bin, 32,
               offsetof(entry, gtid) + offsetof(rpl_gtid, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  my_init_dynamic_array(&gtid_sort_array, sizeof(rpl_gtid), 8, 8, MYF(0));
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
slave_connection_state::load(char *slave_request, size_t len)
{
  char *p, *end;
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
    if (!(rec= (uchar *)my_malloc(sizeof(entry), MYF(MY_WME))))
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

  if (!(e= (entry *)my_malloc(sizeof(*e), MYF(MY_WME))))
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
#ifndef DBUG_OFF
  bool err;
  rpl_gtid *slave_gtid= &((entry *)rec)->gtid;
  DBUG_ASSERT(rec /* We should never try to remove not present domain_id. */);
  DBUG_ASSERT(slave_gtid->server_id == in_gtid->server_id);
  DBUG_ASSERT(slave_gtid->seq_no == in_gtid->seq_no);
#endif

  IF_DBUG(err=, )
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
          if (thd->check_killed())
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
      while (!elem.done && !thd->check_killed())
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
  my_hash_init(&hash, &my_charset_bin, 32,
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

  if (!(e= (hash_element *)my_malloc(sizeof(*e), MYF(MY_WME))))
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
