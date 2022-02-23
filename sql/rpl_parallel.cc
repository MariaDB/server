#include "mariadb.h"
#include "rpl_parallel.h"
#include "slave.h"
#include "rpl_mi.h"
#include "sql_parse.h"
#include "debug_sync.h"
#include "sql_repl.h"
#include "wsrep_mysqld.h"
#ifdef WITH_WSREP
#include "wsrep_trans_observer.h"
#endif

/*
  Code for optional parallel execution of replicated events on the slave.
*/


/*
  Maximum number of queued events to accumulate in a local free list, before
  moving them to the global free list. There is additional a limit of how much
  to accumulate based on opt_slave_parallel_max_queued.
*/
#define QEV_BATCH_FREE 200


struct rpl_parallel_thread_pool global_rpl_thread_pool;

static void signal_error_to_sql_driver_thread(THD *thd, rpl_group_info *rgi,
                                              int err);

static int
rpt_handle_event(rpl_parallel_thread::queued_event *qev,
                 struct rpl_parallel_thread *rpt)
{
  int err;
  rpl_group_info *rgi= qev->rgi;
  Relay_log_info *rli= rgi->rli;
  THD *thd= rgi->thd;
  Log_event *ev;

  DBUG_ASSERT(qev->typ == rpl_parallel_thread::queued_event::QUEUED_EVENT);
  ev= qev->ev;
#ifdef WITH_WSREP
  if (wsrep_before_statement(thd))
  {
    WSREP_WARN("Parallel slave failed at wsrep_before_statement() hook");
    return(1);
  }
#endif /* WITH_WSREP */

  thd->system_thread_info.rpl_sql_info->rpl_filter = rli->mi->rpl_filter;
  ev->thd= thd;

  strcpy(rgi->event_relay_log_name_buf, qev->event_relay_log_name);
  rgi->event_relay_log_name= rgi->event_relay_log_name_buf;
  rgi->event_relay_log_pos= qev->event_relay_log_pos;
  rgi->future_event_relay_log_pos= qev->future_event_relay_log_pos;
  strcpy(rgi->future_event_master_log_name, qev->future_event_master_log_name);
  if (!(ev->is_artificial_event() || ev->is_relay_log_event() ||
        (ev->when == 0)))
    rgi->last_master_timestamp= ev->when + (time_t)ev->exec_time;
  err= apply_event_and_update_pos_for_parallel(ev, thd, rgi);

  rli->executed_entries++;
#ifdef WITH_WSREP
  if (wsrep_after_statement(thd))
  {
    WSREP_WARN("Parallel slave failed at wsrep_after_statement() hook");
    err= 1;
  }
#endif /* WITH_WSREP */
  /* ToDo: error handling. */
  return err;
}


static void
handle_queued_pos_update(THD *thd, rpl_parallel_thread::queued_event *qev)
{
  int cmp;
  Relay_log_info *rli;
  rpl_parallel_entry *e;

  /*
    Events that are not part of an event group, such as Format Description,
    Stop, GTID List and such, are executed directly in the driver SQL thread,
    to keep the relay log state up-to-date. But the associated position update
    is done here, in sync with other normal events as they are queued to
    worker threads.
  */
  if ((thd->variables.option_bits & OPTION_BEGIN) &&
      opt_using_transactions)
    return;

  /* Do not update position if an earlier event group caused an error abort. */
  DBUG_ASSERT(qev->typ == rpl_parallel_thread::queued_event::QUEUED_POS_UPDATE);
  rli= qev->rgi->rli;
  e= qev->entry_for_queued;
  if (e->stop_on_error_sub_id < (uint64)ULONGLONG_MAX ||
      (e->force_abort && !rli->stop_for_until))
    return;

  mysql_mutex_lock(&rli->data_lock);
  cmp= compare_log_name(rli->group_relay_log_name, qev->event_relay_log_name);
  if (cmp < 0)
  {
    rli->group_relay_log_pos= qev->future_event_relay_log_pos;
    strmake_buf(rli->group_relay_log_name, qev->event_relay_log_name);
  } else if (cmp == 0 &&
             rli->group_relay_log_pos < qev->future_event_relay_log_pos)
    rli->group_relay_log_pos= qev->future_event_relay_log_pos;

  cmp= compare_log_name(rli->group_master_log_name, qev->future_event_master_log_name);
  if (cmp < 0)
  {
    strcpy(rli->group_master_log_name, qev->future_event_master_log_name);
    rli->group_master_log_pos= qev->future_event_master_log_pos;
  }
  else if (cmp == 0
           && rli->group_master_log_pos < qev->future_event_master_log_pos)
    rli->group_master_log_pos= qev->future_event_master_log_pos;
  mysql_mutex_unlock(&rli->data_lock);
  mysql_cond_broadcast(&rli->data_cond);
}


/*
  Wait for any pending deadlock kills. Since deadlock kills happen
  asynchronously, we need to be sure they will be completed before starting a
  new transaction. Otherwise the new transaction might suffer a spurious kill.
*/
static void
wait_for_pending_deadlock_kill(THD *thd, rpl_group_info *rgi)
{
  PSI_stage_info old_stage;

  mysql_mutex_lock(&thd->LOCK_wakeup_ready);
  thd->ENTER_COND(&thd->COND_wakeup_ready, &thd->LOCK_wakeup_ready,
                  &stage_waiting_for_deadlock_kill, &old_stage);
  while (rgi->killed_for_retry == rpl_group_info::RETRY_KILL_PENDING)
    mysql_cond_wait(&thd->COND_wakeup_ready, &thd->LOCK_wakeup_ready);
  thd->EXIT_COND(&old_stage);
}


static void
finish_event_group(rpl_parallel_thread *rpt, uint64 sub_id,
                   rpl_parallel_entry *entry, rpl_group_info *rgi)
{
  THD *thd= rpt->thd;
  wait_for_commit *wfc= &rgi->commit_orderer;
  int err;

  if (rgi->get_finish_event_group_called())
    return;

  thd->get_stmt_da()->set_overwrite_status(true);
  /*
    Remove any left-over registration to wait for a prior commit to
    complete. Normally, such wait would already have been removed at
    this point by wait_for_prior_commit() called from within COMMIT
    processing. However, in case of MyISAM and no binlog, we might not
    have any commit processing, and so we need to do the wait here,
    before waking up any subsequent commits, to preserve correct
    order of event execution. Also, in the error case we might have
    skipped waiting and thus need to remove it explicitly.

    It is important in the non-error case to do a wait, not just an
    unregister. Because we might be last in a group-commit that is
    replicated in parallel, and the following event will then wait
    for us to complete and rely on this also ensuring that any other
    event in the group has completed.

    And in the error case, correct GCO lifetime relies on the fact that once
    the last event group in the GCO has executed wait_for_prior_commit(),
    all earlier event groups have also committed; this way no more
    mark_start_commit() calls can be made and it is safe to de-allocate
    the GCO.
  */
  err= wfc->wait_for_prior_commit(thd);
  if (unlikely(err) && !rgi->worker_error)
    signal_error_to_sql_driver_thread(thd, rgi, err);
  thd->wait_for_commit_ptr= NULL;

  mysql_mutex_lock(&entry->LOCK_parallel_entry);
  /*
    We need to mark that this event group started its commit phase, in case we
    missed it before (otherwise we would deadlock the next event group that is
    waiting for this). In most cases (normal DML), it will be a no-op.
  */
  rgi->mark_start_commit_no_lock();

  if (entry->last_committed_sub_id < sub_id)
  {
    /*
      Record that this event group has finished (eg. transaction is
      committed, if transactional), so other event groups will no longer
      attempt to wait for us to commit. Once we have increased
      entry->last_committed_sub_id, no other threads will execute
      register_wait_for_prior_commit() against us. Thus, by doing one
      extra (usually redundant) wakeup_subsequent_commits() we can ensure
      that no register_wait_for_prior_commit() can ever happen without a
      subsequent wakeup_subsequent_commits() to wake it up.

      We can race here with the next transactions, but that is fine, as
      long as we check that we do not decrease last_committed_sub_id. If
      this commit is done, then any prior commits will also have been
      done and also no longer need waiting for.
    */
    entry->last_committed_sub_id= sub_id;
    if (entry->need_sub_id_signal)
      mysql_cond_broadcast(&entry->COND_parallel_entry);

    /* Now free any GCOs in which all transactions have committed. */
    group_commit_orderer *tmp_gco= rgi->gco;
    while (tmp_gco &&
           (!tmp_gco->next_gco || tmp_gco->last_sub_id > sub_id ||
            tmp_gco->next_gco->wait_count > entry->count_committing_event_groups))
    {
      /*
        We must not free a GCO before the wait_count of the following GCO has
        been reached and wakeup has been sent. Otherwise we will lose the
        wakeup and hang (there were several such bugs in the past).

        The intention is that this is ensured already since we only free when
        the last event group in the GCO has committed
        (tmp_gco->last_sub_id <= sub_id). However, if we have a bug, we have
        extra check on next_gco->wait_count to hopefully avoid hanging; we
        have here an assertion in debug builds that this check does not in
        fact trigger.
      */
      DBUG_ASSERT(!tmp_gco->next_gco || tmp_gco->last_sub_id > sub_id);
      tmp_gco= tmp_gco->prev_gco;
    }
    while (tmp_gco)
    {
      group_commit_orderer *prev_gco= tmp_gco->prev_gco;
      tmp_gco->next_gco->prev_gco= NULL;
      rpt->loc_free_gco(tmp_gco);
      tmp_gco= prev_gco;
    }
  }

  /*
    If this event group got error, then any following event groups that have
    not yet started should just skip their group, preparing for stop of the
    SQL driver thread.
  */
  if (unlikely(rgi->worker_error) &&
      entry->stop_on_error_sub_id == (uint64)ULONGLONG_MAX)
    entry->stop_on_error_sub_id= sub_id;
  mysql_mutex_unlock(&entry->LOCK_parallel_entry);
  DBUG_EXECUTE_IF("hold_worker_on_schedule", {
      if (entry->stop_on_error_sub_id < (uint64)ULONGLONG_MAX)
      {
        debug_sync_set_action(thd, STRING_WITH_LEN("now SIGNAL continue_worker"));
      }
    });

  DBUG_EXECUTE_IF("rpl_parallel_simulate_wait_at_retry", {
      if (rgi->current_gtid.seq_no == 1000) {
        DBUG_ASSERT(entry->stop_on_error_sub_id == sub_id);
        debug_sync_set_action(thd,
                              STRING_WITH_LEN("now WAIT_FOR proceed_by_1000"));
      }
    });

  if (rgi->killed_for_retry == rpl_group_info::RETRY_KILL_PENDING)
    wait_for_pending_deadlock_kill(thd, rgi);
  thd->clear_error();
  thd->reset_killed();
  /*
    Would do thd->get_stmt_da()->set_overwrite_status(false) here, but
    reset_diagnostics_area() already does that.
  */
  thd->get_stmt_da()->reset_diagnostics_area();
  wfc->wakeup_subsequent_commits(rgi->worker_error);
  rgi->did_mark_start_commit= false;
  rgi->set_finish_event_group_called(true);
}


static void
signal_error_to_sql_driver_thread(THD *thd, rpl_group_info *rgi, int err)
{
  rgi->worker_error= err;
  /*
    In case we get an error during commit, inform following transactions that
    we aborted our commit.
  */
  rgi->unmark_start_commit();
  rgi->cleanup_context(thd, true);
  rgi->rli->abort_slave= true;
  rgi->rli->stop_for_until= false;
  mysql_mutex_lock(rgi->rli->relay_log.get_log_lock());
  rgi->rli->relay_log.signal_relay_log_update();
  mysql_mutex_unlock(rgi->rli->relay_log.get_log_lock());
}


static void
unlock_or_exit_cond(THD *thd, mysql_mutex_t *lock, bool *did_enter_cond,
                    PSI_stage_info *old_stage)
{
  if (*did_enter_cond)
  {
    thd->EXIT_COND(old_stage);
    *did_enter_cond= false;
  }
  else
    mysql_mutex_unlock(lock);
}


static void
register_wait_for_prior_event_group_commit(rpl_group_info *rgi,
                                           rpl_parallel_entry *entry)
{
  mysql_mutex_assert_owner(&entry->LOCK_parallel_entry);
  if (rgi->wait_commit_sub_id > entry->last_committed_sub_id)
  {
    /*
      Register that the commit of this event group must wait for the
      commit of the previous event group to complete before it may
      complete itself, so that we preserve commit order.
    */
    wait_for_commit *waitee=
      &rgi->wait_commit_group_info->commit_orderer;
    rgi->commit_orderer.register_wait_for_prior_commit(waitee);
  }
}


/*
  Do not start parallel execution of this event group until all prior groups
  have reached the commit phase that are not safe to run in parallel with.
*/
static bool
do_gco_wait(rpl_group_info *rgi, group_commit_orderer *gco,
            bool *did_enter_cond, PSI_stage_info *old_stage)
{
  THD *thd= rgi->thd;
  rpl_parallel_entry *entry= rgi->parallel_entry;
  uint64 wait_count;

  mysql_mutex_assert_owner(&entry->LOCK_parallel_entry);

  if (!gco->installed)
  {
    group_commit_orderer *prev_gco= gco->prev_gco;
    if (prev_gco)
    {
      prev_gco->last_sub_id= gco->prior_sub_id;
      prev_gco->next_gco= gco;
    }
    gco->installed= true;
  }
  wait_count= gco->wait_count;
  if (wait_count > entry->count_committing_event_groups)
  {
    DEBUG_SYNC(thd, "rpl_parallel_start_waiting_for_prior");
    thd->ENTER_COND(&gco->COND_group_commit_orderer,
                    &entry->LOCK_parallel_entry,
                    &stage_waiting_for_prior_transaction_to_start_commit,
                    old_stage);
    *did_enter_cond= true;
    thd->set_time_for_next_stage();
    do
    {
      if (!rgi->worker_error && unlikely(thd->check_killed(1)))
      {
        DEBUG_SYNC(thd, "rpl_parallel_start_waiting_for_prior_killed");
        thd->clear_error();
        thd->get_stmt_da()->reset_diagnostics_area();
        thd->send_kill_message();
        slave_output_error_info(rgi, thd);
        signal_error_to_sql_driver_thread(thd, rgi, 1);
        /*
          Even though we were killed, we need to continue waiting for the
          prior event groups to signal that we can continue. Otherwise we
          mess up the accounting for ordering. However, now that we have
          marked the error, events will just be skipped rather than
          executed, and things will progress quickly towards stop.
        */
      }
      mysql_cond_wait(&gco->COND_group_commit_orderer,
                      &entry->LOCK_parallel_entry);
    } while (wait_count > entry->count_committing_event_groups);
  }

  if (entry->force_abort && wait_count > entry->stop_count)
  {
    /*
      We are stopping (STOP SLAVE), and this event group is beyond the point
      where we can safely stop. So return a flag that will cause us to skip,
      rather than execute, the following events.
    */
    return true;
  }
  else
    return false;
}


static bool
do_ftwrl_wait(rpl_group_info *rgi,
              bool *did_enter_cond, PSI_stage_info *old_stage)
{
  THD *thd= rgi->thd;
  rpl_parallel_entry *entry= rgi->parallel_entry;
  uint64 sub_id= rgi->gtid_sub_id;
  bool aborted= false;
  DBUG_ENTER("do_ftwrl_wait");

  mysql_mutex_assert_owner(&entry->LOCK_parallel_entry);

  /*
    If a FLUSH TABLES WITH READ LOCK (FTWRL) is pending, check if this
    transaction is later than transactions that have priority to complete
    before FTWRL. If so, wait here so that FTWRL can proceed and complete
    first.

    (entry->pause_sub_id is ULONGLONG_MAX if no FTWRL is pending, which makes
    this test false as required).
  */
  if (unlikely(sub_id > entry->pause_sub_id))
  {
    thd->ENTER_COND(&entry->COND_parallel_entry, &entry->LOCK_parallel_entry,
                    &stage_waiting_for_ftwrl, old_stage);
    *did_enter_cond= true;
    thd->set_time_for_next_stage();
    do
    {
      if (entry->force_abort || rgi->worker_error)
      {
        aborted= true;
        break;
      }
      if (unlikely(thd->check_killed()))
      {
        slave_output_error_info(rgi, thd);
        signal_error_to_sql_driver_thread(thd, rgi, 1);
        break;
      }
      mysql_cond_wait(&entry->COND_parallel_entry, &entry->LOCK_parallel_entry);
    } while (sub_id > entry->pause_sub_id);

    /*
      We do not call EXIT_COND() here, as this will be done later by our
      caller (since we set *did_enter_cond to true).
    */
  }

  if (sub_id > entry->largest_started_sub_id)
    entry->largest_started_sub_id= sub_id;

  DBUG_RETURN(aborted);
}


static int
pool_mark_busy(rpl_parallel_thread_pool *pool, THD *thd)
{
  PSI_stage_info old_stage;
  int res= 0;

  /*
    Wait here while the queue is busy. This is done to make FLUSH TABLES WITH
    READ LOCK work correctly, without incuring extra locking penalties in
    normal operation. FLUSH TABLES WITH READ LOCK needs to lock threads in the
    thread pool, and for this we need to make sure the pool will not go away
    during the operation. The LOCK_rpl_thread_pool is not suitable for
    this. It is taken by release_thread() while holding LOCK_rpl_thread; so it
    must be released before locking any LOCK_rpl_thread lock, or a deadlock
    can occur.

    So we protect the infrequent operations of FLUSH TABLES WITH READ LOCK and
    pool size changes with this condition wait.
  */
  DBUG_EXECUTE_IF("mark_busy_mdev_22370",my_sleep(1000000););
  mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
  if (thd)
  {
    thd->ENTER_COND(&pool->COND_rpl_thread_pool, &pool->LOCK_rpl_thread_pool,
                    &stage_waiting_for_rpl_thread_pool, &old_stage);
    thd->set_time_for_next_stage();
  }
  while (pool->busy)
  {
    if (thd && unlikely(thd->check_killed()))
    {
      res= 1;
      break;
    }
    mysql_cond_wait(&pool->COND_rpl_thread_pool, &pool->LOCK_rpl_thread_pool);
  }
  if (!res)
    pool->busy= true;
  if (thd)
    thd->EXIT_COND(&old_stage);
  else
    mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);

  return res;
}


static void
pool_mark_not_busy(rpl_parallel_thread_pool *pool)
{
  mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
  DBUG_ASSERT(pool->busy);
  pool->busy= false;
  mysql_cond_broadcast(&pool->COND_rpl_thread_pool);
  mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
}


void
rpl_unpause_after_ftwrl(THD *thd)
{
  uint32 i;
  rpl_parallel_thread_pool *pool= &global_rpl_thread_pool;
  DBUG_ENTER("rpl_unpause_after_ftwrl");

  DBUG_ASSERT(pool->busy);

  for (i= 0; i < pool->count; ++i)
  {
    rpl_parallel_entry *e;
    rpl_parallel_thread *rpt= pool->threads[i];

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    if (!rpt->current_owner)
    {
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      continue;
    }
    e= rpt->current_entry;
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    rpt->pause_for_ftwrl = false;
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
    /*
      Do not change pause_sub_id if force_abort is set.
      force_abort is set in case of STOP SLAVE.

      Reason: If pause_sub_id is not changed and force_abort_is set,
      any parallel slave thread waiting in do_ftwrl_wait() will
      on wakeup return from do_ftwrl_wait() with 1. This will set
      skip_event_group to 1 in handle_rpl_parallel_thread() and the
      parallel thread will abort at once.

      If pause_sub_id is changed, the code in handle_rpl_parallel_thread()
      would continue to execute the transaction in the queue, which would
      cause some transactions to be lost.
    */
    if (!e->force_abort)
      e->pause_sub_id= (uint64)ULONGLONG_MAX;
    mysql_cond_broadcast(&e->COND_parallel_entry);
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
  }

  pool_mark_not_busy(pool);
  DBUG_VOID_RETURN;
}


/*
  .

  Note: in case of error return, rpl_unpause_after_ftwrl() must _not_ be called.
*/
int
rpl_pause_for_ftwrl(THD *thd)
{
  uint32 i;
  rpl_parallel_thread_pool *pool= &global_rpl_thread_pool;
  int err;
  Dynamic_array<Master_info*> mi_arr(4, 4); // array of replication source mi:s
  DBUG_ENTER("rpl_pause_for_ftwrl");

  /*
    While the count_pending_pause_for_ftwrl counter is non-zero, the pool
    cannot be shutdown/resized, so threads are guaranteed to not disappear.

    This is required to safely be able to access the individual threads below.
    (We cannot lock an individual thread while holding LOCK_rpl_thread_pool,
    as this can deadlock against release_thread()).
  */
  if ((err= pool_mark_busy(pool, thd)))
    DBUG_RETURN(err);

  for (i= 0; i < pool->count; ++i)
  {
    PSI_stage_info old_stage;
    rpl_parallel_entry *e;
    rpl_parallel_thread *rpt= pool->threads[i];

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    if (!rpt->current_owner)
    {
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      continue;
    }
    e= rpt->current_entry;
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    /*
      Setting the rpt->pause_for_ftwrl flag makes sure that the thread will not
      de-allocate itself until signalled to do so by rpl_unpause_after_ftwrl().
    */
    rpt->pause_for_ftwrl = true;
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
    ++e->need_sub_id_signal;
    if (e->pause_sub_id == (uint64)ULONGLONG_MAX)
      e->pause_sub_id= e->largest_started_sub_id;
    thd->ENTER_COND(&e->COND_parallel_entry, &e->LOCK_parallel_entry,
                    &stage_waiting_for_ftwrl_threads_to_pause, &old_stage);
    thd->set_time_for_next_stage();
    while (e->pause_sub_id < (uint64)ULONGLONG_MAX &&
           e->last_committed_sub_id < e->pause_sub_id &&
           !err)
    {
      if (unlikely(thd->check_killed()))
      {
        err= 1;
        break;
      }
      mysql_cond_wait(&e->COND_parallel_entry, &e->LOCK_parallel_entry);
    };
    --e->need_sub_id_signal;

    thd->EXIT_COND(&old_stage);
    if (err)
      break;
    /*
      Notify any source any domain waiting-for-master Start-Alter to give way.
    */
    Master_info *mi= e->rli->mi;
    bool found= false;
    for (uint i= 0; i < mi_arr.elements() && !found; i++)
      found= mi_arr.at(i) == mi;
    if (!found)
    {
      mi_arr.append(mi);
      start_alter_info *info=NULL;
      mysql_mutex_lock(&mi->start_alter_list_lock);
      List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
      while ((info= info_iterator++))
      {
        mysql_mutex_lock(&mi->start_alter_lock);

        DBUG_ASSERT(info->state == start_alter_state::REGISTERED);

        info->state= start_alter_state::ROLLBACK_ALTER;
        info->direct_commit_alter= true;
        mysql_cond_broadcast(&info->start_alter_cond);
        mysql_mutex_unlock(&mi->start_alter_lock);
      }
      mysql_mutex_unlock(&mi->start_alter_list_lock);
    }
  }

  if (err)
    rpl_unpause_after_ftwrl(thd);
  DBUG_RETURN(err);
}


#ifndef DBUG_OFF
static int
dbug_simulate_tmp_error(rpl_group_info *rgi, THD *thd)
{
  if (rgi->current_gtid.domain_id == 0 && rgi->current_gtid.seq_no == 100 &&
      rgi->retry_event_count == 4)
  {
    thd->clear_error();
    thd->get_stmt_da()->reset_diagnostics_area();
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return 1;
  }
  return 0;
}
#endif


/*
  If we detect a deadlock due to eg. storage engine locks that conflict with
  the fixed commit order, then the later transaction will be killed
  asynchroneously to allow the former to complete its commit.

  In this case, we convert the 'killed' error into a deadlock error, and retry
  the later transaction.

  If we are doing optimistic parallel apply of transactions not known to be
  safe, we convert any error to a deadlock error, but then at retry we will
  wait for prior transactions to commit first, so that the retries can be
  done non-speculative.
*/
static void
convert_kill_to_deadlock_error(rpl_group_info *rgi)
{
  THD *thd= rgi->thd;
  int err_code;

  if (!thd->get_stmt_da()->is_error())
    return;
  err_code= thd->get_stmt_da()->sql_errno();
  if ((rgi->speculation == rpl_group_info::SPECULATE_OPTIMISTIC &&
       err_code != ER_PRIOR_COMMIT_FAILED) ||
      ((err_code == ER_QUERY_INTERRUPTED || err_code == ER_CONNECTION_KILLED) &&
       rgi->killed_for_retry))
  {
    thd->clear_error();
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    thd->reset_killed();
  }
}


/*
  Check if an event marks the end of an event group. Returns non-zero if so,
  zero otherwise.

  In addition, returns 1 if the group is committing, 2 if it is rolling back.
*/
static int
is_group_ending(Log_event *ev, Log_event_type event_type)
{
  if (event_type == XID_EVENT || event_type == XA_PREPARE_LOG_EVENT)
    return 1;
  if (event_type == QUERY_EVENT)  // COMMIT/ROLLBACK are never compressed
  {
    Query_log_event *qev = (Query_log_event *)ev;
    if (qev->is_commit() ||
        !strncmp(qev->query, STRING_WITH_LEN("XA COMMIT")) ||
        !strncmp(qev->query, STRING_WITH_LEN("XA ROLLBACK")))
      return 1;
    if (qev->is_rollback())
      return 2;
  }
  return 0;
}


static int
retry_event_group(rpl_group_info *rgi, rpl_parallel_thread *rpt,
                  rpl_parallel_thread::queued_event *orig_qev)
{
  IO_CACHE rlog;
  LOG_INFO linfo;
  File fd= (File)-1;
  const char *errmsg;
  inuse_relaylog *ir= rgi->relay_log;
  uint64 event_count;
  uint64 events_to_execute= rgi->retry_event_count;
  Relay_log_info *rli= rgi->rli;
  int err;
  ulonglong cur_offset, old_offset;
  char log_name[FN_REFLEN];
  THD *thd= rgi->thd;
  rpl_parallel_entry *entry= rgi->parallel_entry;
  ulong retries= 0;
  Format_description_log_event *description_event= NULL;

do_retry:
  event_count= 0;
  err= 0;
  errmsg= NULL;

  /*
    If we already started committing before getting the deadlock (or other
    error) that caused us to need to retry, we have already signalled
    subsequent transactions that we have started committing. This is
    potentially a problem, as now we will rollback, and if subsequent
    transactions would start to execute now, they could see an unexpected
    state of the database and get eg. key not found or duplicate key error.

    However, to get a deadlock in the first place, there must have been
    another earlier transaction that is waiting for us. Thus that other
    transaction has _not_ yet started to commit, and any subsequent
    transactions will still be waiting at this point.

    So here, we decrement back the count of transactions that started
    committing (if we already incremented it), undoing the effect of an
    earlier mark_start_commit(). Then later, when the retry succeeds and we
    commit again, we can do a new mark_start_commit() and eventually wake up
    subsequent transactions at the proper time.

    We need to do the unmark before the rollback, to be sure that the
    transaction we deadlocked with will not signal that it started to commit
    until after the unmark.
  */
  DBUG_EXECUTE_IF("inject_mdev8302", { my_sleep(20000);});
  rgi->unmark_start_commit();
  DEBUG_SYNC(thd, "rpl_parallel_retry_after_unmark");

  /*
    We might get the deadlock error that causes the retry during commit, while
    sitting in wait_for_prior_commit(). If this happens, we will have a
    pending error in the wait_for_commit object. So clear this by
    unregistering (and later re-registering) the wait.
  */
  if(thd->wait_for_commit_ptr)
    thd->wait_for_commit_ptr->unregister_wait_for_prior_commit();
  DBUG_EXECUTE_IF("inject_mdev8031", {
      /* Simulate that we get deadlock killed at this exact point. */
      rgi->killed_for_retry= rpl_group_info::RETRY_KILL_KILLED;
      thd->set_killed(KILL_CONNECTION);
  });
  DBUG_EXECUTE_IF("rpl_parallel_simulate_wait_at_retry", {
      if (rgi->current_gtid.seq_no == 1001) {
        debug_sync_set_action(thd,
                              STRING_WITH_LEN("rpl_parallel_simulate_wait_at_retry WAIT_FOR proceed_by_1001"));
      }
      DEBUG_SYNC(thd, "rpl_parallel_simulate_wait_at_retry");
    });

  rgi->cleanup_context(thd, 1);
  wait_for_pending_deadlock_kill(thd, rgi);
  thd->reset_killed();
  thd->clear_error();
  rgi->killed_for_retry = rpl_group_info::RETRY_KILL_NONE;

  /*
    If we retry due to a deadlock kill that occurred during the commit step, we
    might have already updated (but not committed) an update of table
    mysql.gtid_slave_pos, and cleared the gtid_pending flag. Now we have
    rolled back any such update, so we must set the gtid_pending flag back to
    true so that we will do a new update when/if we succeed with the retry.
  */
  rgi->gtid_pending= true;

  mysql_mutex_lock(&rli->data_lock);
  ++rli->retried_trans;
  ++rpt->last_trans_retry_count;
  statistic_increment(slave_retried_transactions, LOCK_status);
  mysql_mutex_unlock(&rli->data_lock);

  for (;;)
  {
    mysql_mutex_lock(&entry->LOCK_parallel_entry);
    if (entry->stop_on_error_sub_id == (uint64) ULONGLONG_MAX ||
        DBUG_IF("simulate_mdev_12746") ||
        rgi->gtid_sub_id < entry->stop_on_error_sub_id)
    {
      register_wait_for_prior_event_group_commit(rgi, entry);
    }
    else
    {
      /*
        A failure of a preceding "parent" transaction may not be
        seen by the current one through its own worker_error.
        Such induced error gets set by ourselves now.
      */
      err= rgi->worker_error= 1;
      my_error(ER_PRIOR_COMMIT_FAILED, MYF(0));
      mysql_mutex_unlock(&entry->LOCK_parallel_entry);
      goto err;
    }
    mysql_mutex_unlock(&entry->LOCK_parallel_entry);

    /*
      Let us wait for all prior transactions to complete before trying again.
      This way, we avoid repeatedly conflicting with and getting deadlock
      killed by the same earlier transaction.
    */
    if (!(err= thd->wait_for_prior_commit()))
    {
      rgi->speculation = rpl_group_info::SPECULATE_WAIT;
      break;
    }

    convert_kill_to_deadlock_error(rgi);
    if (!has_temporary_error(thd))
      goto err;
    /*
      If we get a temporary error such as a deadlock kill, we can safely
      ignore it, as we already rolled back.

      But we still want to retry the wait for the prior transaction to
      complete its commit.
    */
    thd->clear_error();
    thd->reset_killed();
    if(thd->wait_for_commit_ptr)
      thd->wait_for_commit_ptr->unregister_wait_for_prior_commit();
    DBUG_EXECUTE_IF("inject_mdev8031", {
        /* Inject a small sleep to give prior transaction a chance to commit. */
        my_sleep(100000);
    });
  }

  /*
    Let us clear any lingering deadlock kill one more time, here after
    wait_for_prior_commit() has completed. This should rule out any
    possibility of an old deadlock kill lingering on beyond this point.
  */
  thd->reset_killed();

  strmake_buf(log_name, ir->name);
  if ((fd= open_binlog(&rlog, log_name, &errmsg)) <0)
  {
    err= 1;
    goto err;
  }
  cur_offset= rgi->retry_start_offset;
  delete description_event;
  description_event=
    read_relay_log_description_event(&rlog, cur_offset, &errmsg);
  if (!description_event)
  {
    err= 1;
    goto err;
  }
  DBUG_EXECUTE_IF("inject_mdev8031", {
      /* Simulate pending KILL caught in read_relay_log_description_event(). */
      if (unlikely(thd->check_killed()))
      {
        err= 1;
        goto err;
      }
  });
  my_b_seek(&rlog, cur_offset);

  do
  {
    Log_event_type event_type;
    Log_event *ev;
    rpl_parallel_thread::queued_event *qev;

    /* The loop is here so we can try again the next relay log file on EOF. */
    for (;;)
    {
      old_offset= cur_offset;
      ev= Log_event::read_log_event(&rlog, description_event,
                                    opt_slave_sql_verify_checksum);
      cur_offset= my_b_tell(&rlog);

      if (ev)
        break;
      if (unlikely(rlog.error < 0))
      {
        errmsg= "slave SQL thread aborted because of I/O error";
        err= 1;
        goto check_retry;
      }
      if (unlikely(rlog.error > 0))
      {
        sql_print_error("Slave SQL thread: I/O error reading "
                        "event(errno: %d  cur_log->error: %d)",
                        my_errno, rlog.error);
        errmsg= "Aborting slave SQL thread because of partial event read";
        err= 1;
        goto err;
      }
      /* EOF. Move to the next relay log. */
      end_io_cache(&rlog);
      mysql_file_close(fd, MYF(MY_WME));
      fd= (File)-1;

      /* Find the next relay log file. */
      if((err= rli->relay_log.find_log_pos(&linfo, log_name, 1)) ||
         (err= rli->relay_log.find_next_log(&linfo, 1)))
      {
        char buff[22];
        sql_print_error("next log error: %d  offset: %s  log: %s",
                        err,
                        llstr(linfo.index_file_offset, buff),
                        log_name);
        goto err;
      }
      strmake_buf(log_name ,linfo.log_file_name);

      DBUG_EXECUTE_IF("inject_retry_event_group_open_binlog_kill", {
          if (retries < 2)
          {
            /* Simulate that we get deadlock killed during open_binlog(). */
            thd->reset_for_next_command();
            rgi->killed_for_retry= rpl_group_info::RETRY_KILL_KILLED;
            thd->set_killed(KILL_CONNECTION);
            thd->send_kill_message();
            fd= (File)-1;
            err= 1;
            goto check_retry;
          }
      });
      if ((fd= open_binlog(&rlog, log_name, &errmsg)) <0)
      {
        err= 1;
        goto check_retry;
      }
      description_event->reset_crypto();
      /* Loop to try again on the new log file. */
    }

    event_type= ev->get_type_code();
    if (event_type == FORMAT_DESCRIPTION_EVENT)
    {
      Format_description_log_event *newde= (Format_description_log_event*)ev;
      newde->copy_crypto_data(description_event);
      delete description_event;
      description_event= newde;
      continue;
    }
    else if (event_type == START_ENCRYPTION_EVENT)
    {
      description_event->start_decryption((Start_encryption_log_event*)ev);
      delete ev;
      continue;
    }
    else if (!Log_event::is_group_event(event_type))
    {
      delete ev;
      continue;
    }
    ev->thd= thd;

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    qev= rpt->retry_get_qev(ev, orig_qev, log_name, old_offset,
                            cur_offset - old_offset);
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
    if (!qev)
    {
      delete ev;
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      err= 1;
      goto err;
    }
    if (is_group_ending(ev, event_type) == 1)
      rgi->mark_start_commit();

    err= rpt_handle_event(qev, rpt);
    ++event_count;
    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    rpt->free_qev(qev);
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);

    delete_or_keep_event_post_apply(rgi, event_type, ev);
    DBUG_EXECUTE_IF("rpl_parallel_simulate_double_temp_err_gtid_0_x_100",
                    if (retries == 0) err= dbug_simulate_tmp_error(rgi, thd););
    DBUG_EXECUTE_IF("rpl_parallel_simulate_infinite_temp_err_gtid_0_x_100",
                    err= dbug_simulate_tmp_error(rgi, thd););
    if (!err)
      continue;

check_retry:
    convert_kill_to_deadlock_error(rgi);
    if (has_temporary_error(thd))
    {
      ++retries;
      if (retries < slave_trans_retries)
      {
        if (fd >= 0)
        {
          end_io_cache(&rlog);
          mysql_file_close(fd, MYF(MY_WME));
          fd= (File)-1;
        }
        goto do_retry;
      }
      sql_print_error("Slave worker thread retried transaction %lu time(s) "
                      "in vain, giving up. Consider raising the value of "
                      "the slave_transaction_retries variable.",
                      slave_trans_retries);
    }
    goto err;

  } while (event_count < events_to_execute);

err:

  if (description_event)
    delete description_event;
  if (fd >= 0)
  {
    end_io_cache(&rlog);
    mysql_file_close(fd, MYF(MY_WME));
  }
  if (errmsg)
    sql_print_error("Error reading relay log event: %s", errmsg);
  return err;
}


pthread_handler_t
handle_rpl_parallel_thread(void *arg)
{
  THD *thd;
  PSI_stage_info old_stage;
  struct rpl_parallel_thread::queued_event *events;
  bool group_standalone= true;
  bool in_event_group= false;
  bool skip_event_group= false;
  rpl_group_info *group_rgi= NULL;
  group_commit_orderer *gco;
  uint64 event_gtid_sub_id= 0;
  rpl_sql_thread_info sql_info(NULL);
  int err;

  struct rpl_parallel_thread *rpt= (struct rpl_parallel_thread *)arg;

  my_thread_init();
  thd = new THD(next_thread_id());
  thd->thread_stack = (char*)&thd;
  server_threads.insert(thd);
  set_current_thd(thd);
  pthread_detach_this_thread();
  thd->store_globals();
  thd->init_for_queries();
  thd->variables.binlog_annotate_row_events= 0;
  init_thr_lock();
  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();
  thd->variables.max_allowed_packet= slave_max_allowed_packet;
  /* Ensure that slave can exeute any alter table it gets from master */
  thd->variables.alter_algorithm= (ulong) Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT;
  thd->slave_thread= 1;

  set_slave_thread_options(thd);
  thd->client_capabilities = CLIENT_LOCAL_FILES;
  thd->net.reading_or_writing= 0;
  thd_proc_info(thd, "Waiting for work from main SQL threads");
  thd->variables.lock_wait_timeout= LONG_TIMEOUT;
  thd->system_thread_info.rpl_sql_info= &sql_info;
  /*
    We need to use (at least) REPEATABLE READ isolation level. Otherwise
    speculative parallel apply can run out-of-order and give wrong results
    for statement-based replication.
  */
  thd->variables.tx_isolation= ISO_REPEATABLE_READ;

  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->thd= thd;
  PSI_thread *psi= PSI_CALL_get_thread();
  PSI_CALL_set_thread_os_id(psi);
  PSI_CALL_set_thread_THD(psi, thd);
  PSI_CALL_set_thread_id(psi, thd->thread_id);
  rpt->thd->set_psi(psi);

  while (rpt->delay_start)
    mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);

  rpt->running= true;
  mysql_cond_signal(&rpt->COND_rpl_thread);

  thd->set_command(COM_SLAVE_WORKER);
#ifdef WITH_WSREP
  wsrep_open(thd);
  if (wsrep_before_command(thd))
  {
    WSREP_WARN("Parallel slave failed at wsrep_before_command() hook");
    rpt->stop = true;
  }
#endif /* WITH_WSREP */
  while (!rpt->stop)
  {
    uint wait_count= 0;
    rpl_parallel_thread::queued_event *qev, *next_qev;

    rpt->start_time_tracker();
    thd->ENTER_COND(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread,
                    &stage_waiting_for_work_from_sql_thread, &old_stage);
    /*
      There are 4 cases that should cause us to wake up:
       - Events have been queued for us to handle.
       - We have an owner, but no events and not inside event group -> we need
         to release ourself to the thread pool
       - SQL thread is stopping, and we have an owner but no events, and we are
         inside an event group; no more events will be queued to us, so we need
         to abort the group (force_abort==1).
       - Thread pool shutdown (rpt->stop==1).
    */
    while (!( (events= rpt->event_queue) ||
              (rpt->current_owner && !in_event_group) ||
              (rpt->current_owner && group_rgi->parallel_entry->force_abort) ||
              rpt->stop))
    {
      if (!wait_count++)
        thd->set_time_for_next_stage();
      mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);
    }
    rpt->dequeue1(events);
    thd->EXIT_COND(&old_stage);
    rpt->add_to_worker_idle_time_and_reset();

  more_events:
    for (qev= events; qev; qev= next_qev)
    {
      Log_event_type event_type;
      rpl_group_info *rgi= qev->rgi;
      rpl_parallel_entry *entry= rgi->parallel_entry;
      bool end_of_group;
      int group_ending;

      next_qev= qev->next;
      if (qev->typ == rpl_parallel_thread::queued_event::QUEUED_POS_UPDATE)
      {
        handle_queued_pos_update(thd, qev);
        rpt->loc_free_qev(qev);
        continue;
      }
      else if (qev->typ ==
               rpl_parallel_thread::queued_event::QUEUED_MASTER_RESTART)
      {
        if (in_event_group)
        {
          /*
            Master restarted (crashed) in the middle of an event group.
            So we need to roll back and discard that event group.
          */
          group_rgi->cleanup_context(thd, 1);
          in_event_group= false;
          finish_event_group(rpt, group_rgi->gtid_sub_id,
                             qev->entry_for_queued, group_rgi);

          rpt->loc_free_rgi(group_rgi);
          thd->rgi_slave= group_rgi= NULL;
        }

        rpt->loc_free_qev(qev);
        continue;
      }
      DBUG_ASSERT(qev->typ==rpl_parallel_thread::queued_event::QUEUED_EVENT);

      thd->rgi_slave= rgi;
      gco= rgi->gco;
      /* Handle a new event group, which will be initiated by a GTID event. */
      if ((event_type= qev->ev->get_type_code()) == GTID_EVENT)
      {
        rpt->last_trans_retry_count= 0;
        rpt->last_seen_gtid= rgi->current_gtid;
        rpt->channel_name_length= (uint)rgi->rli->mi->connection_name.length;
        if (rpt->channel_name_length)
          memcpy(rpt->channel_name, rgi->rli->mi->connection_name.str,
                 rgi->rli->mi->connection_name.length);

        bool did_enter_cond= false;
        PSI_stage_info old_stage;

        DBUG_EXECUTE_IF("hold_worker_on_schedule", {
            if (rgi->current_gtid.domain_id == 0 &&
                rgi->current_gtid.seq_no == 100) {
                  debug_sync_set_action(thd,
                STRING_WITH_LEN("now SIGNAL reached_pause WAIT_FOR continue_worker"));
            }
          });
        DBUG_EXECUTE_IF("rpl_parallel_scheduled_gtid_0_x_100", {
            if (rgi->current_gtid.domain_id == 0 &&
                rgi->current_gtid.seq_no == 100) {
              debug_sync_set_action(thd,
                      STRING_WITH_LEN("now SIGNAL scheduled_gtid_0_x_100"));
            }
          });

        if(unlikely(thd->wait_for_commit_ptr) && group_rgi != NULL)
        {
          /*
            This indicates that we get a new GTID event in the middle of
            a not completed event group. This is corrupt binlog (the master
            will never write such binlog), so it does not happen unless
            someone tries to inject wrong crafted binlog, but let us still
            try to handle it somewhat nicely.
          */
          group_rgi->cleanup_context(thd, true);
          finish_event_group(rpt, group_rgi->gtid_sub_id,
                             group_rgi->parallel_entry, group_rgi);
          rpt->loc_free_rgi(group_rgi);
        }

        thd->tx_isolation= (enum_tx_isolation)thd->variables.tx_isolation;
        in_event_group= true;
        /*
          If the standalone flag is set, then this event group consists of a
          single statement (possibly preceeded by some Intvar_log_event and
          similar), without any terminating COMMIT/ROLLBACK/XID.
        */
        group_standalone=
          (0 != (static_cast<Gtid_log_event *>(qev->ev)->flags2 &
                 Gtid_log_event::FL_STANDALONE));

        event_gtid_sub_id= rgi->gtid_sub_id;
        rgi->thd= thd;

        mysql_mutex_lock(&entry->LOCK_parallel_entry);
        skip_event_group= do_gco_wait(rgi, gco, &did_enter_cond, &old_stage);

        if (unlikely(entry->stop_on_error_sub_id <= rgi->wait_commit_sub_id))
        {
          skip_event_group= true;
          rgi->worker_error= 1;
        }
        if (likely(!skip_event_group))
          skip_event_group= do_ftwrl_wait(rgi, &did_enter_cond, &old_stage);

        /*
          Register ourself to wait for the previous commit, if we need to do
          such registration _and_ that previous commit has not already
          occurred.
        */
        register_wait_for_prior_event_group_commit(rgi, entry);

        unlock_or_exit_cond(thd, &entry->LOCK_parallel_entry,
                            &did_enter_cond, &old_stage);

        thd->wait_for_commit_ptr= &rgi->commit_orderer;

        if (opt_gtid_ignore_duplicates &&
            rgi->rli->mi->using_gtid != Master_info::USE_GTID_NO)
        {
          int res=
            rpl_global_gtid_slave_state->check_duplicate_gtid(&rgi->current_gtid,
                                                             rgi);
          if (res < 0)
          {
            /* Error. */
            slave_output_error_info(rgi, thd);
            signal_error_to_sql_driver_thread(thd, rgi, 1);
          }
          else if (!res)
          {
            /* GTID already applied by another master connection, skip. */
            skip_event_group= true;
          }
          else
          {
            /* We have to apply the event. */
          }
        }
        /*
          If we are optimistically running transactions in parallel, but this
          particular event group should not run in parallel with what came
          before, then wait now for the prior transaction to complete its
          commit.
        */
        if (rgi->speculation == rpl_group_info::SPECULATE_WAIT &&
            (err= thd->wait_for_prior_commit()))
        {
          slave_output_error_info(rgi, thd);
          signal_error_to_sql_driver_thread(thd, rgi, 1);
        }
      }

      group_rgi= rgi;
      group_ending= is_group_ending(qev->ev, event_type);
      /*
        We do not unmark_start_commit() here in case of an explicit ROLLBACK
        statement. Such events should be very rare, there is no real reason
        to try to group commit them - on the contrary, it seems best to avoid
        running them in parallel with following group commits, as with
        ROLLBACK events we are already deep in dangerous corner cases with
        mix of transactional and non-transactional tables or the like. And
        avoiding the mark_start_commit() here allows us to keep an assertion
        in ha_rollback_trans() that we do not rollback after doing
        mark_start_commit().
      */
      if (group_ending == 1 && likely(!rgi->worker_error))
      {
        /*
          Do an extra check for (deadlock) kill here. This helps prevent a
          lingering deadlock kill that occurred during normal DML processing to
          propagate past the mark_start_commit(). If we detect a deadlock only
          after mark_start_commit(), we have to unmark, which has at least a
          theoretical possibility of leaving a window where it looks like all
          transactions in a GCO have started committing, while in fact one
          will need to rollback and retry. This is not supposed to be possible
          (since there is a deadlock, at least one transaction should be
          blocked from reaching commit), but this seems a fragile ensurance,
          and there were historically a number of subtle bugs in this area.
        */
        if (!thd->killed)
        {
          DEBUG_SYNC(thd, "rpl_parallel_before_mark_start_commit");
          rgi->mark_start_commit();
          DEBUG_SYNC(thd, "rpl_parallel_after_mark_start_commit");
        }
      }

      /*
        If the SQL thread is stopping, we just skip execution of all the
        following event groups. We still do all the normal waiting and wakeup
        processing between the event groups as a simple way to ensure that
        everything is stopped and cleaned up correctly.
      */
      if (likely(!rgi->worker_error) && !skip_event_group)
      {
        ++rgi->retry_event_count;
#ifndef DBUG_OFF
        err= 0;
        DBUG_EXECUTE_IF("rpl_parallel_simulate_temp_err_xid",
          if (event_type == XID_EVENT)
          {
            thd->clear_error();
            thd->get_stmt_da()->reset_diagnostics_area();
            my_error(ER_LOCK_DEADLOCK, MYF(0));
            err= 1;
            DEBUG_SYNC(thd, "rpl_parallel_simulate_temp_err_xid");
          });
        if (!err)
#endif
        {
          if (unlikely(thd->check_killed()))
          {
            thd->clear_error();
            thd->get_stmt_da()->reset_diagnostics_area();
            thd->send_kill_message();
            err= 1;
          }
          else
            err= rpt_handle_event(qev, rpt);
        }
        delete_or_keep_event_post_apply(rgi, event_type, qev->ev);
        DBUG_EXECUTE_IF("rpl_parallel_simulate_temp_err_gtid_0_x_100",
                        err= dbug_simulate_tmp_error(rgi, thd););
        if (unlikely(err))
        {
          convert_kill_to_deadlock_error(rgi);
          if (has_temporary_error(thd) && slave_trans_retries > 0)
            err= retry_event_group(rgi, rpt, qev);
        }
      }
      else
      {
        delete qev->ev;
        thd->get_stmt_da()->set_overwrite_status(true);
        err= thd->wait_for_prior_commit();
        thd->get_stmt_da()->set_overwrite_status(false);
      }

      end_of_group=
        in_event_group &&
        ((group_standalone && !Log_event::is_part_of_group(event_type)) ||
         group_ending);

      rpt->loc_free_qev(qev);

      if (unlikely(err))
      {
        if (!rgi->worker_error)
        {
          slave_output_error_info(rgi, thd);
          signal_error_to_sql_driver_thread(thd, rgi, err);
        }
        thd->reset_killed();
      }
      if (end_of_group)
      {
        in_event_group= false;
        finish_event_group(rpt, event_gtid_sub_id, entry, rgi);
        rpt->loc_free_rgi(rgi);
        thd->rgi_slave= group_rgi= rgi= NULL;
        skip_event_group= false;
        DEBUG_SYNC(thd, "rpl_parallel_end_of_group");
      }
    }

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    /*
      Now that we have the lock, we can move everything from our local free
      lists to the real free lists that are also accessible from the SQL
      driver thread.
    */
    rpt->batch_free();

    if ((events= rpt->event_queue) != NULL)
    {
      /*
        Take next group of events from the replication pool.
        This is faster than having to wakeup the pool manager thread to give
        us a new event.
      */
      rpt->dequeue1(events);
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      goto more_events;
    }

    rpt->inuse_relaylog_refcount_update();

    if (in_event_group && group_rgi->parallel_entry->force_abort)
    {
      /*
        We are asked to abort, without getting the remaining events in the
        current event group.

        We have to rollback the current transaction and update the last
        sub_id value so that SQL thread will know we are done with the
        half-processed event group.
      */
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      signal_error_to_sql_driver_thread(thd, group_rgi, 1);
      finish_event_group(rpt, group_rgi->gtid_sub_id,
                         group_rgi->parallel_entry, group_rgi);
      in_event_group= false;
      mysql_mutex_lock(&rpt->LOCK_rpl_thread);
      rpt->free_rgi(group_rgi);
      thd->rgi_slave= group_rgi= NULL;
      skip_event_group= false;
    }
    if (!in_event_group)
    {
      /* If we are in a FLUSH TABLES FOR READ LOCK, wait for it */
      while (rpt->current_entry && rpt->pause_for_ftwrl)
      {
        /*
          We are currently in the delicate process of pausing parallel
          replication while FLUSH TABLES WITH READ LOCK is starting. We must
          not de-allocate the thread (setting rpt->current_owner= NULL) until
          rpl_unpause_after_ftwrl() has woken us up.
        */
        rpl_parallel_entry *e= rpt->current_entry;
        /*
          Wait for rpl_unpause_after_ftwrl() to wake us up.
          Note that rpl_pause_for_ftwrl() may wait for 'e->pause_sub_id'
          to change. This should happen eventually in finish_event_group()
        */
        mysql_mutex_lock(&e->LOCK_parallel_entry);
        mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
        if (rpt->pause_for_ftwrl)
          mysql_cond_wait(&e->COND_parallel_entry, &e->LOCK_parallel_entry);
        mysql_mutex_unlock(&e->LOCK_parallel_entry);
        mysql_mutex_lock(&rpt->LOCK_rpl_thread);
      }

      rpt->current_owner= NULL;
      /* Tell wait_for_done() that we are done, if it is waiting. */
      if (likely(rpt->current_entry) &&
          unlikely(rpt->current_entry->force_abort))
        mysql_cond_broadcast(&rpt->COND_rpl_thread_stop);

      rpt->current_entry= NULL;
      if (!rpt->stop)
        rpt->pool->release_thread(rpt);
    }
  }
#ifdef WITH_WSREP
  wsrep_after_command_before_result(thd);
  wsrep_after_command_after_result(thd);
  wsrep_close(thd);
#endif /* WITH_WSREP */

  rpt->thd= NULL;
  mysql_mutex_unlock(&rpt->LOCK_rpl_thread);

  thd->clear_error();
  thd->catalog= 0;
  thd->reset_query();
  thd->reset_db(&null_clex_str);
  thd_proc_info(thd, "Slave worker thread exiting");
  thd->temporary_tables= 0;

  THD_CHECK_SENTRY(thd);
  server_threads.erase(thd);
  delete thd;

  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->running= false;
  mysql_cond_signal(&rpt->COND_rpl_thread);
  mysql_mutex_unlock(&rpt->LOCK_rpl_thread);

  my_thread_end();

  return NULL;
}


static void
dealloc_gco(group_commit_orderer *gco)
{
  mysql_cond_destroy(&gco->COND_group_commit_orderer);
  my_free(gco);
}

/**
   Change thread count for global parallel worker threads

   @param pool          parallel thread pool
   @param new_count     Number of threads to be in pool. 0 in shutdown
   @param force         Force thread count to new_count even if slave
                        threads are running

   By default we don't resize pool of there are running threads.
   However during shutdown we will always do it.
   This is needed as any_slave_sql_running() returns 1 during shutdown
   as we don't want to access master_info while
   Master_info_index::free_connections are running.
*/

static int
rpl_parallel_change_thread_count(rpl_parallel_thread_pool *pool,
                                 uint32 new_count, bool force)
{
  uint32 i;
  rpl_parallel_thread **old_list= NULL;
  rpl_parallel_thread **new_list= NULL;
  rpl_parallel_thread *new_free_list= NULL;
  rpl_parallel_thread *rpt_array= NULL;
  int res;

  if ((res= pool_mark_busy(pool, current_thd)))
    return res;

  /* Protect against parallel pool resizes */
  if (pool->count == new_count)
  {
    pool_mark_not_busy(pool);
    return 0;
  }

  /*
    If we are about to delete pool, do an extra check that there are no new
    slave threads running since we marked pool busy
  */
  if (!new_count && !force)
  {
    if (any_slave_sql_running(false))
    {
      DBUG_PRINT("warning",
                 ("SQL threads running while trying to reset parallel pool"));
      pool_mark_not_busy(pool);
      return 0;                                 // Ok to not resize pool
    }
  }

  /*
    Allocate the new list of threads up-front.
    That way, if we fail half-way, we only need to free whatever we managed
    to allocate, and will not be left with a half-functional thread pool.
  */
  if (new_count &&
      !my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME|MY_ZEROFILL),
                       &new_list, new_count*sizeof(*new_list),
                       &rpt_array, new_count*sizeof(*rpt_array),
                       NULL))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int(new_count*sizeof(*new_list) +
                                          new_count*sizeof(*rpt_array))));
    goto err;
  }

  for (i= 0; i < new_count; ++i)
  {
    pthread_t th;

    new_list[i]= &rpt_array[i];
    new_list[i]->delay_start= true;
    mysql_mutex_init(key_LOCK_rpl_thread, &new_list[i]->LOCK_rpl_thread,
                     MY_MUTEX_INIT_SLOW);
    mysql_cond_init(key_COND_rpl_thread, &new_list[i]->COND_rpl_thread, NULL);
    mysql_cond_init(key_COND_rpl_thread_queue,
                    &new_list[i]->COND_rpl_thread_queue, NULL);
    mysql_cond_init(key_COND_rpl_thread_stop,
                    &new_list[i]->COND_rpl_thread_stop, NULL);
    new_list[i]->pool= pool;
    if (mysql_thread_create(key_rpl_parallel_thread, &th, &connection_attrib,
                            handle_rpl_parallel_thread, new_list[i]))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto err;
    }
    new_list[i]->next= new_free_list;
    new_free_list= new_list[i];
  }

  /*
    Grab each old thread in turn, and signal it to stop.

    Note that since we require all replication threads to be stopped before
    changing the parallel replication worker thread pool, all the threads will
    be already idle and will terminate immediately.
  */
  for (i= 0; i < pool->count; ++i)
  {
    rpl_parallel_thread *rpt;

    mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
    while ((rpt= pool->free_list) == NULL)
      mysql_cond_wait(&pool->COND_rpl_thread_pool, &pool->LOCK_rpl_thread_pool);
    pool->free_list= rpt->next;
    mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);
    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    rpt->stop= true;
    mysql_cond_signal(&rpt->COND_rpl_thread);
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
  }

  for (i= 0; i < pool->count; ++i)
  {
    rpl_parallel_thread *rpt= pool->threads[i];
    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    while (rpt->running)
      mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);
    mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
    mysql_mutex_destroy(&rpt->LOCK_rpl_thread);
    mysql_cond_destroy(&rpt->COND_rpl_thread);
    while (rpt->qev_free_list)
    {
      rpl_parallel_thread::queued_event *next= rpt->qev_free_list->next;
      my_free(rpt->qev_free_list);
      rpt->qev_free_list= next;
    }
    while (rpt->rgi_free_list)
    {
      rpl_group_info *next= rpt->rgi_free_list->next;
      delete rpt->rgi_free_list;
      rpt->rgi_free_list= next;
    }
    while (rpt->gco_free_list)
    {
      group_commit_orderer *next= rpt->gco_free_list->next_gco;
      dealloc_gco(rpt->gco_free_list);
      rpt->gco_free_list= next;
    }
  }

  old_list= pool->threads;
  if (new_count < pool->count)
    pool->count= new_count;
  pool->threads= new_list;
  if (new_count > pool->count)
    pool->count= new_count;
  my_free(old_list);
  pool->free_list= new_free_list;
  for (i= 0; i < pool->count; ++i)
  {
    mysql_mutex_lock(&pool->threads[i]->LOCK_rpl_thread);
    pool->threads[i]->delay_start= false;
    pool->threads[i]->current_start_alter_id= 0;
    pool->threads[i]->current_start_alter_domain_id= 0;
    pool->threads[i]->reserved_start_alter_thread= false;
    mysql_cond_signal(&pool->threads[i]->COND_rpl_thread);
    while (!pool->threads[i]->running)
      mysql_cond_wait(&pool->threads[i]->COND_rpl_thread,
                      &pool->threads[i]->LOCK_rpl_thread);
    mysql_mutex_unlock(&pool->threads[i]->LOCK_rpl_thread);
  }

  pool_mark_not_busy(pool);

  return 0;

err:
  if (new_list)
  {
    while (new_free_list)
    {
      mysql_mutex_lock(&new_free_list->LOCK_rpl_thread);
      new_free_list->delay_start= false;
      new_free_list->stop= true;
      mysql_cond_signal(&new_free_list->COND_rpl_thread);
      while (!new_free_list->running)
        mysql_cond_wait(&new_free_list->COND_rpl_thread,
                        &new_free_list->LOCK_rpl_thread);
      while (new_free_list->running)
        mysql_cond_wait(&new_free_list->COND_rpl_thread,
                        &new_free_list->LOCK_rpl_thread);
      mysql_mutex_unlock(&new_free_list->LOCK_rpl_thread);
      new_free_list= new_free_list->next;
    }
    my_free(new_list);
  }
  pool_mark_not_busy(pool);
  return 1;
}

/*
  Deactivate the parallel replication thread pool, if there are now no more
  SQL threads running.
*/

int rpl_parallel_resize_pool_if_no_slaves(void)
{
  /* master_info_index is set to NULL on shutdown */
  if (opt_slave_parallel_threads > 0 && !any_slave_sql_running(false))
    return rpl_parallel_inactivate_pool(&global_rpl_thread_pool);
  return 0;
}


/**
  Pool activation is preceeded by taking a "lock" of pool_mark_busy
  which guarantees the number of running slaves drops to zero atomicly
  with the number of pool workers.
  This resolves race between the function caller thread and one
  that may be attempting to deactivate the pool.
*/
int
rpl_parallel_activate_pool(rpl_parallel_thread_pool *pool)
{
  int rc= 0;
  struct pool_bkp_for_pfs* bkp= &pool->pfs_bkp;

  if ((rc= pool_mark_busy(pool, current_thd)))
    return rc;   // killed

  if (!pool->count)
  {
    pool_mark_not_busy(pool);
    rc= rpl_parallel_change_thread_count(pool, opt_slave_parallel_threads,
                                         0);
    if (!rc)
    {
      if (pool->count)
      {
        if (bkp->inited)
        {
          if (bkp->count != pool->count)
          {
            bkp->destroy();
            bkp->init(pool->count);
          }
        }
        else
          bkp->init(pool->count);
      }
    }

  }
  else
  {
    pool_mark_not_busy(pool);
  }
  return rc;
}


int
rpl_parallel_inactivate_pool(rpl_parallel_thread_pool *pool)
{
  return rpl_parallel_change_thread_count(pool, 0, 0);
}


void
rpl_parallel_thread::batch_free()
{
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  if (loc_qev_list)
  {
    *loc_qev_last_ptr_ptr= qev_free_list;
    qev_free_list= loc_qev_list;
    loc_qev_list= NULL;
    dequeue2(loc_qev_size);
    /* Signal that our queue can now accept more events. */
    mysql_cond_signal(&COND_rpl_thread_queue);
    loc_qev_size= 0;
    qev_free_pending= 0;
  }
  if (loc_rgi_list)
  {
    *loc_rgi_last_ptr_ptr= rgi_free_list;
    rgi_free_list= loc_rgi_list;
    loc_rgi_list= NULL;
  }
  if (loc_gco_list)
  {
    *loc_gco_last_ptr_ptr= gco_free_list;
    gco_free_list= loc_gco_list;
    loc_gco_list= NULL;
  }
}


void
rpl_parallel_thread::inuse_relaylog_refcount_update()
{
  inuse_relaylog *ir= accumulated_ir_last;
  if (ir)
  {
    ir->dequeued_count+= accumulated_ir_count;
    accumulated_ir_count= 0;
    accumulated_ir_last= NULL;
  }
}


rpl_parallel_thread::queued_event *
rpl_parallel_thread::get_qev_common(Log_event *ev, ulonglong event_size)
{
  queued_event *qev;
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  if ((qev= qev_free_list))
    qev_free_list= qev->next;
  else if(!(qev= (queued_event *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*qev), MYF(0))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*qev));
    return NULL;
  }
  qev->typ= rpl_parallel_thread::queued_event::QUEUED_EVENT;
  qev->ev= ev;
  qev->event_size= (size_t)event_size;
  qev->next= NULL;
  return qev;
}


rpl_parallel_thread::queued_event *
rpl_parallel_thread::get_qev(Log_event *ev, ulonglong event_size,
                             Relay_log_info *rli)
{
  queued_event *qev= get_qev_common(ev, event_size);
  if (!qev)
    return NULL;
  strcpy(qev->event_relay_log_name, rli->event_relay_log_name);
  qev->event_relay_log_pos= rli->event_relay_log_pos;
  qev->future_event_relay_log_pos= rli->future_event_relay_log_pos;
  strcpy(qev->future_event_master_log_name, rli->future_event_master_log_name);
  return qev;
}


rpl_parallel_thread::queued_event *
rpl_parallel_thread::retry_get_qev(Log_event *ev, queued_event *orig_qev,
                                   const char *relay_log_name,
                                   ulonglong event_pos, ulonglong event_size)
{
  queued_event *qev= get_qev_common(ev, event_size);
  if (!qev)
    return NULL;
  qev->rgi= orig_qev->rgi;
  strcpy(qev->event_relay_log_name, relay_log_name);
  qev->event_relay_log_pos= event_pos;
  qev->future_event_relay_log_pos= event_pos+event_size;
  strcpy(qev->future_event_master_log_name,
         orig_qev->future_event_master_log_name);
  return qev;
}


void
rpl_parallel_thread::loc_free_qev(rpl_parallel_thread::queued_event *qev)
{
  inuse_relaylog *ir= qev->ir;
  inuse_relaylog *last_ir= accumulated_ir_last;
  if (ir != last_ir)
  {
    if (last_ir)
      inuse_relaylog_refcount_update();
    accumulated_ir_last= ir;
  }
  ++accumulated_ir_count;
  if (!loc_qev_list)
    loc_qev_last_ptr_ptr= &qev->next;
  else
    qev->next= loc_qev_list;
  loc_qev_list= qev;
  loc_qev_size+= qev->event_size;
  /*
    We want to release to the global free list only occasionally, to avoid
    having to take the LOCK_rpl_thread muted too many times.

    However, we do need to release regularly. If we let the unreleased part
    grow too large, then the SQL driver thread may go to sleep waiting for
    the queue to drop below opt_slave_parallel_max_queued, and this in turn
    can stall all other worker threads for more stuff to do.
  */
  if (++qev_free_pending >= QEV_BATCH_FREE ||
      loc_qev_size >= opt_slave_parallel_max_queued/3)
  {
    mysql_mutex_lock(&LOCK_rpl_thread);
    batch_free();
    mysql_mutex_unlock(&LOCK_rpl_thread);
  }
}


void
rpl_parallel_thread::free_qev(rpl_parallel_thread::queued_event *qev)
{
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  qev->next= qev_free_list;
  qev_free_list= qev;
}


rpl_group_info*
rpl_parallel_thread::get_rgi(Relay_log_info *rli, Gtid_log_event *gtid_ev,
                             rpl_parallel_entry *e, ulonglong event_size)
{
  rpl_group_info *rgi;
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  if ((rgi= rgi_free_list))
  {
    rgi_free_list= rgi->next;
    rgi->reinit(rli);
  }
  else
  {
    if(!(rgi= new rpl_group_info(rli)))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*rgi));
      return NULL;
    }
    rgi->is_parallel_exec = true;
  }
  if ((rgi->deferred_events_collecting= rli->mi->rpl_filter->is_on()) &&
      !rgi->deferred_events)
    rgi->deferred_events= new Deferred_log_events(rli);
  if (event_group_new_gtid(rgi, gtid_ev))
  {
    free_rgi(rgi);
    my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
    return NULL;
  }
  rgi->parallel_entry= e;
  rgi->relay_log= rli->last_inuse_relaylog;
  rgi->retry_start_offset= rli->future_event_relay_log_pos-event_size;
  rgi->retry_event_count= 0;
  rgi->killed_for_retry= rpl_group_info::RETRY_KILL_NONE;
  /* rgi is transaction specific so we need to move this value to rgi */
  rgi->reserved_start_alter_thread= reserved_start_alter_thread;
  rgi->rpt= this;
  rgi->direct_commit_alter= false;
  rgi->finish_event_group_called= false;

  DBUG_ASSERT(!rgi->sa_info);
  /*
    We can remove the reserved_start_alter_thread flag.
    If we get more concurrent alter handle_split_alter will
    automatically set this flag again.
  */
  reserved_start_alter_thread= false;
  return rgi;
}


void
rpl_parallel_thread::loc_free_rgi(rpl_group_info *rgi)
{
  DBUG_ASSERT(rgi->commit_orderer.waitee == NULL);
  rgi->free_annotate_event();
  if (!loc_rgi_list)
    loc_rgi_last_ptr_ptr= &rgi->next;
  else
    rgi->next= loc_rgi_list;
  loc_rgi_list= rgi;
}


void
rpl_parallel_thread::free_rgi(rpl_group_info *rgi)
{
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  DBUG_ASSERT(rgi->commit_orderer.waitee == NULL);
  rgi->free_annotate_event();
  rgi->next= rgi_free_list;
  rgi_free_list= rgi;
}


group_commit_orderer *
rpl_parallel_thread::get_gco(uint64 wait_count, group_commit_orderer *prev,
                             uint64 prior_sub_id)
{
  group_commit_orderer *gco;
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  if ((gco= gco_free_list))
    gco_free_list= gco->next_gco;
  else if(!(gco= (group_commit_orderer *)my_malloc(PSI_INSTRUMENT_ME, sizeof(*gco), MYF(0))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*gco));
    return NULL;
  }
  mysql_cond_init(key_COND_group_commit_orderer,
                  &gco->COND_group_commit_orderer, NULL);
  gco->wait_count= wait_count;
  gco->prev_gco= prev;
  gco->next_gco= NULL;
  gco->prior_sub_id= prior_sub_id;
  gco->installed= false;
  gco->flags= 0;
  return gco;
}


void
rpl_parallel_thread::loc_free_gco(group_commit_orderer *gco)
{
  if (!loc_gco_list)
    loc_gco_last_ptr_ptr= &gco->next_gco;
  else
    gco->next_gco= loc_gco_list;
  loc_gco_list= gco;
}

void rpl_group_info::finish_start_alter_event_group()
{
   finish_event_group(rpt, this->gtid_sub_id, this->parallel_entry, this);
}

rpl_parallel_thread::rpl_parallel_thread()
  : channel_name_length(0), last_error_number(0), last_error_timestamp(0),
    worker_idle_time(0), last_trans_retry_count(0), start_time(0)
{
}


rpl_parallel_thread_pool::rpl_parallel_thread_pool()
  : threads(0), free_list(0), count(0), inited(false),current_start_alters(0), busy(false),
    pfs_bkp{0, false, NULL}
{
}


int
rpl_parallel_thread_pool::init(uint32 size)
{
  threads= NULL;
  free_list= NULL;
  count= 0;
  busy= false;

  mysql_mutex_init(key_LOCK_rpl_thread_pool, &LOCK_rpl_thread_pool,
                   MY_MUTEX_INIT_SLOW);
  mysql_cond_init(key_COND_rpl_thread_pool, &COND_rpl_thread_pool, NULL);
  inited= true;

  /*
    The pool is initially empty. Threads will be spawned when a slave SQL
    thread is started.
  */

  return 0;
}


void
rpl_parallel_thread_pool::destroy()
{
  deactivate();
  pfs_bkp.destroy();
  destroy_cond_mutex();
}

void
rpl_parallel_thread_pool::deactivate()
{
  if (!inited)
    return;
  rpl_parallel_change_thread_count(this, 0, 1);
}

void
rpl_parallel_thread_pool::destroy_cond_mutex()
{
  if (!inited)
    return;
  mysql_mutex_destroy(&LOCK_rpl_thread_pool);
  mysql_cond_destroy(&COND_rpl_thread_pool);
  inited= false;
}


/*
  Wait for a worker thread to become idle. When one does, grab the thread for
  our use and return it.

  Note that we return with the worker threads's LOCK_rpl_thread mutex locked.
*/
struct rpl_parallel_thread *
rpl_parallel_thread_pool::get_thread(rpl_parallel_thread **owner,
                                     rpl_parallel_entry *entry)
{
  rpl_parallel_thread *rpt;

  DBUG_ASSERT(count > 0);
  mysql_mutex_lock(&LOCK_rpl_thread_pool);
  while (unlikely(busy) || !(rpt= free_list))
    mysql_cond_wait(&COND_rpl_thread_pool, &LOCK_rpl_thread_pool);
  free_list= rpt->next;
  mysql_mutex_unlock(&LOCK_rpl_thread_pool);
  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->current_owner= owner;
  rpt->current_entry= entry;

  return rpt;
}


/*
  Release a thread to the thread pool.
  The thread should be locked, and should not have any work queued for it.
*/
void
rpl_parallel_thread_pool::release_thread(rpl_parallel_thread *rpt)
{
  rpl_parallel_thread *list;

  mysql_mutex_assert_owner(&rpt->LOCK_rpl_thread);
  DBUG_ASSERT(rpt->current_owner == NULL);
  mysql_mutex_lock(&LOCK_rpl_thread_pool);
  list= free_list;
  rpt->next= list;
  free_list= rpt;
  if (!list)
    mysql_cond_broadcast(&COND_rpl_thread_pool);
  mysql_mutex_unlock(&LOCK_rpl_thread_pool);
}

void
rpl_parallel_thread_pool::copy_pool_for_pfs(Relay_log_info *rli)
{
  if (pfs_bkp.inited)
  {
    for(uint i=0; i<count;i++)
    {
      rpl_parallel_thread *rpt, *pfs_rpt;
      rpt= threads[i];
      pfs_rpt= pfs_bkp.rpl_thread_arr[i];
      if (rpt->channel_name_length)
      {
        pfs_rpt->channel_name_length= rpt->channel_name_length;
        strmake(pfs_rpt->channel_name, rpt->channel_name,
                rpt->channel_name_length);
      }
      pfs_rpt->thd= rpt->thd;
      pfs_rpt->last_seen_gtid= rpt->last_seen_gtid;
      if (rli->err_thread_id && rpt->thd->thread_id == rli->err_thread_id)
      {
        pfs_rpt->last_error_number= rli->last_error().number;
        strmake(pfs_rpt->last_error_message,
            rli->last_error().message, sizeof(rli->last_error().message));
        pfs_rpt->last_error_timestamp= rli->last_error().skr*1000000;
      }
      pfs_rpt->running= false;
      pfs_rpt->worker_idle_time= rpt->get_worker_idle_time();
      pfs_rpt->last_trans_retry_count= rpt->last_trans_retry_count;
    }
  }
}

/* 
  START ALTER , COMMIT ALTER / ROLLBACK ALTER scheduling
  
  Steps:-
    1. (For Gtid_log_event SA). Get the worker thread which is either
      e->rpl_threads[i] is NULL means worker from poll has not been assigned yet
      e->rpl_threads[i]->current_owner != &e->rpl_threads[i] 
          Thread has been released, or about to //same as choose_thread logic
      !e->rpl_threads[i]->current_start_alter_id is 0 , safe to schedule.
      We dont want to schedule on worker which already have been scheduled SA
      but CA/RA has not been scheduled yet. current_start_alter_id will indicate
      this. If we dont do this we will get deadlock.
    2. (For Gtid_log_event SA)
      call choose_thread_internal so that e->rpl_threads[idx] is not null
      update the current_start_alter_id
    3. (For Gtid_log_event SA)
      update local e->pending_start_alters(local) variable and 
       pool->current_start_alters(global)
      We need 2 status variable (global and local) because we can have 
       slave_domain_parallel_threads != pool->threads.
    4. (For CA/RA Gtid_log_event)
      Update e->pending_start_alters and pool->current_start_alters
      while holding mutex lock on pool (if SA is not assigned to 
      reserved thread)


    @returns
      true  Worker allocated (choose_thread_internal called)
      false Worker not allocated (choose_thread_internal not called)
*/
static bool handle_split_alter(rpl_parallel_entry *e,  
                               Gtid_log_event *gtid_ev, uint32 *idx,
                               //choose_thread_internal specific
                               bool *did_enter_cond, rpl_group_info* rgi,
                               PSI_stage_info *old_stage)
{
  uint16 flags_extra= gtid_ev->flags_extra;
  bool thread_allocated= false;
  //Step 1
  if (flags_extra & Gtid_log_event::FL_START_ALTER_E1 ||
      //This will arrange finding threads for CA/RA as well
      //as concurrent DDL
      e->pending_start_alters)
  {
    /*
     j is needed for round robin scheduling, we will start with rpl_thread_idx
     go till rpl_thread_max and then start with 0 to rpl_thread_idx
    */
    int j= e->rpl_thread_idx;
    for(uint i= 0; i < e->rpl_thread_max; i++)
    {
      if (!e->rpl_threads[j] || e->rpl_threads[j]->current_owner
          != &e->rpl_threads[j] || !e->rpl_threads[j]->current_start_alter_id)
      {
        //This condition will hit atleast one time no matter what happens
        *idx= j;
        DBUG_PRINT("info", ("Start alter id %d", j));
        goto idx_found;
      }
      j++;
      j= j % e->rpl_thread_max;
    }
    //We did not find and idx
  DBUG_ASSERT(0);
  return false;
idx_found:
    e->rpl_thread_idx= *idx;
    e->choose_thread_internal(*idx, did_enter_cond, rgi, old_stage);
    thread_allocated= true;
    if (flags_extra & Gtid_log_event::FL_START_ALTER_E1)
    {
      mysql_mutex_assert_owner(&e->rpl_threads[*idx]->LOCK_rpl_thread);
      e->rpl_threads[e->rpl_thread_idx]->current_start_alter_id= gtid_ev->seq_no;
      e->rpl_threads[e->rpl_thread_idx]->current_start_alter_domain_id= 
                                                            gtid_ev->domain_id;
      /*
       We are locking LOCK_rpl_thread_pool becuase we are going to update
       current_start_alters
      */
      mysql_mutex_lock(&global_rpl_thread_pool.LOCK_rpl_thread_pool);
      if (e->pending_start_alters < e->rpl_thread_max - 1 &&
              global_rpl_thread_pool.current_start_alters
              < global_rpl_thread_pool.count - 1)
      {
        e->pending_start_alters++;
        global_rpl_thread_pool.current_start_alters++;
      }
      else
      {
        e->rpl_threads[*idx]->reserved_start_alter_thread= true;
        e->rpl_threads[*idx]->current_start_alter_id= 0;
        e->rpl_threads[*idx]->current_start_alter_domain_id= 0;
      }
      mysql_mutex_unlock(&global_rpl_thread_pool.LOCK_rpl_thread_pool);
    }
  }
  if(flags_extra & (Gtid_log_event::FL_COMMIT_ALTER_E1 |
                    Gtid_log_event::FL_ROLLBACK_ALTER_E1 ))
  {
    //Free the corrosponding rpt current_start_alter_id
    for(uint i= 0; i < e->rpl_thread_max; i++)
    {
      if(e->rpl_threads[i] &&
          e->rpl_threads[i]->current_start_alter_id == gtid_ev->sa_seq_no &&
          e->rpl_threads[i]->current_start_alter_domain_id == gtid_ev->domain_id)
      {
        mysql_mutex_lock(&global_rpl_thread_pool.LOCK_rpl_thread_pool);
        e->rpl_threads[i]->current_start_alter_id= 0;
        e->rpl_threads[i]->current_start_alter_domain_id= 0;
        global_rpl_thread_pool.current_start_alters--;
        e->pending_start_alters--;
        DBUG_PRINT("info", ("Commit/Rollback alter id %d", i));
        mysql_mutex_unlock(&global_rpl_thread_pool.LOCK_rpl_thread_pool);
        break;
      }
    }
  }

  return thread_allocated;

}


/*
  Obtain a worker thread that we can queue an event to.

  Each invocation allocates a new worker thread, to maximise
  parallelism. However, only up to a maximum of
  --slave-domain-parallel-threads workers can be occupied by a single
  replication domain; after that point, we start re-using worker threads that
  are still executing events that were queued earlier for this thread.

  We never queue more than --rpl-parallel-wait-queue_max amount of events
  for one worker, to avoid the SQL driver thread using up all memory with
  queued events while worker threads are stalling.

  Note that this function returns with rpl_parallel_thread::LOCK_rpl_thread
  locked. Exception is if we were killed, in which case NULL is returned.

  The *did_enter_cond flag is set true if we had to wait for a worker thread
  to become free (with mysql_cond_wait()). If so, old_stage will also be set,
  and the LOCK_rpl_thread must be released with THD::EXIT_COND() instead
  of mysql_mutex_unlock.

  When `gtid_ev' is not NULL the last worker thread will be returned again,
  if it is still available. Otherwise a new worker thread is allocated.

  A worker for XA transaction is determined through xid hashing which
  ensure for a XA-complete to be scheduled to the same-xid XA-prepare worker.
*/
rpl_parallel_thread *
rpl_parallel_entry::choose_thread(rpl_group_info *rgi, bool *did_enter_cond,
                                  PSI_stage_info *old_stage,
                                  Gtid_log_event *gtid_ev)
{
  uint32 idx;

  idx= rpl_thread_idx;
  if (gtid_ev)
  {
    if (++idx >= rpl_thread_max)
      idx= 0;
    //rpl_thread_idx will be updated handle_split_alter
    if (handle_split_alter(this, gtid_ev, &idx, did_enter_cond, rgi, old_stage))
      return rpl_threads[idx];
    if (gtid_ev->flags2 &
        (Gtid_log_event::FL_COMPLETED_XA | Gtid_log_event::FL_PREPARED_XA))
    {     
      idx= my_hash_sort(&my_charset_bin, gtid_ev->xid.key(),
                        gtid_ev->xid.key_length()) % rpl_thread_max;
    }
    rpl_thread_idx= idx;
  }
  return choose_thread_internal(idx, did_enter_cond, rgi, old_stage);
}

rpl_parallel_thread * rpl_parallel_entry::choose_thread_internal(uint idx,
                                  bool *did_enter_cond, rpl_group_info *rgi,
                                  PSI_stage_info *old_stage)
{
  rpl_parallel_thread* thr= rpl_threads[idx];
  Relay_log_info *rli= rgi->rli;
  if (thr)
  {
    *did_enter_cond= false;
    mysql_mutex_lock(&thr->LOCK_rpl_thread);
    for (;;)
    {
      if (thr->current_owner != &rpl_threads[idx])
      {
        /*
          The worker thread became idle, and returned to the free list and
          possibly was allocated to a different request. So we should allocate
          a new worker thread.
        */
        unlock_or_exit_cond(rli->sql_driver_thd, &thr->LOCK_rpl_thread,
                            did_enter_cond, old_stage);
        thr= NULL;
        break;
      }
      else if (thr->queued_size <= opt_slave_parallel_max_queued)
      {
        /* The thread is ready to queue into. */
        break;
      }
      else if (unlikely(rli->sql_driver_thd->check_killed(1)))
      {
        unlock_or_exit_cond(rli->sql_driver_thd, &thr->LOCK_rpl_thread,
                            did_enter_cond, old_stage);
        my_error(ER_CONNECTION_KILLED, MYF(0));
        DBUG_EXECUTE_IF("rpl_parallel_wait_queue_max",
          {
            debug_sync_set_action(rli->sql_driver_thd,
                      STRING_WITH_LEN("now SIGNAL wait_queue_killed"));
          };);
        slave_output_error_info(rgi, rli->sql_driver_thd);
        return NULL;
      }
      else
      {
        /*
          We have reached the limit of how much memory we are allowed to use
          for queuing events, so wait for the thread to consume some of its
          queue.
        */
        if (!*did_enter_cond)
        {
          /*
            We need to do the debug_sync before ENTER_COND().
            Because debug_sync changes the thd->mysys_var->current_mutex,
            and this can cause THD::awake to use the wrong mutex.
          */
          DBUG_EXECUTE_IF("rpl_parallel_wait_queue_max",
            {
              debug_sync_set_action(rli->sql_driver_thd,
                        STRING_WITH_LEN("now SIGNAL wait_queue_ready"));
            };);
          rli->sql_driver_thd->ENTER_COND(&thr->COND_rpl_thread_queue,
                                          &thr->LOCK_rpl_thread,
                                          &stage_waiting_for_room_in_worker_thread,
                                          old_stage);
          *did_enter_cond= true;
        }
        mysql_cond_wait(&thr->COND_rpl_thread_queue, &thr->LOCK_rpl_thread);
      }
    }
  }
  if (!thr)
    rpl_threads[idx]= thr= global_rpl_thread_pool.get_thread(&rpl_threads[idx],
                                                             this);

  return thr;
}

static void
free_rpl_parallel_entry(void *element)
{
  rpl_parallel_entry *e= (rpl_parallel_entry *)element;
  while (e->current_gco)
  {
    group_commit_orderer *prev_gco= e->current_gco->prev_gco;
    dealloc_gco(e->current_gco);
    e->current_gco= prev_gco;
  }
  mysql_cond_destroy(&e->COND_parallel_entry);
  mysql_mutex_destroy(&e->LOCK_parallel_entry);
  my_free(e);
}


rpl_parallel::rpl_parallel() :
  current(NULL), sql_thread_stopping(false)
{
  my_hash_init(PSI_INSTRUMENT_ME, &domain_hash, &my_charset_bin, 32,
               offsetof(rpl_parallel_entry, domain_id), sizeof(uint32),
               NULL, free_rpl_parallel_entry, HASH_UNIQUE);
}


void
rpl_parallel::reset()
{
  my_hash_reset(&domain_hash);
  current= NULL;
  sql_thread_stopping= false;
}


rpl_parallel::~rpl_parallel()
{
  my_hash_free(&domain_hash);
}


rpl_parallel_entry *
rpl_parallel::find(uint32 domain_id, Relay_log_info *rli)
{
  struct rpl_parallel_entry *e;

  if (!(e= (rpl_parallel_entry *)my_hash_search(&domain_hash,
                                                (const uchar *)&domain_id,
                                                sizeof(domain_id))))
  {
    /* Allocate a new, empty one. */
    ulong count= opt_slave_domain_parallel_threads;
    if (count == 0 || count > opt_slave_parallel_threads)
      count= opt_slave_parallel_threads;
    rpl_parallel_thread **p;
    if (!my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME|MY_ZEROFILL),
                         &e, sizeof(*e),
                         &p, count*sizeof(*p),
                         NULL))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int)(sizeof(*e)+count*sizeof(*p)));
      return NULL;
    }
    e->rpl_threads= p;
    e->rpl_thread_max= count;
    e->domain_id= domain_id;
    e->stop_on_error_sub_id= (uint64)ULONGLONG_MAX;
    e->pause_sub_id= (uint64)ULONGLONG_MAX;
    e->pending_start_alters= 0;
    e->rli= rli;
    if (my_hash_insert(&domain_hash, (uchar *)e))
    {
      my_free(e);
      return NULL;
    }
    mysql_mutex_init(key_LOCK_parallel_entry, &e->LOCK_parallel_entry,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_parallel_entry, &e->COND_parallel_entry, NULL);
  }
  else
  {
    DBUG_ASSERT(rli == e->rli);

    e->force_abort= false;
  }

  return e;
}

/**
  Wait until all sql worker threads has stopped processing

  This is called when sql thread has been killed/stopped
*/

void
rpl_parallel::wait_for_done(THD *thd, Relay_log_info *rli)
{
  struct rpl_parallel_entry *e;
  rpl_parallel_thread *rpt;
  uint32 i, j;
  Master_info *mi= rli->mi;
  /*
    First signal all workers that they must force quit; no more events will
    be queued to complete any partial event groups executed.
  */
  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    /*
      We want the worker threads to stop as quickly as is safe. If the slave
      SQL threads are behind, we could have significant amount of events
      queued for the workers, and we want to stop without waiting for them
      all to be applied first. But if any event group has already started
      executing in a worker, we want to be sure that all prior event groups
      are also executed, so that we stop at a consistent point in the binlog
      stream (per replication domain).

      All event groups wait for e->count_committing_event_groups to reach
      the value of group_commit_orderer::wait_count before starting to
      execute. Thus, at this point we know that any event group with a
      strictly larger wait_count are safe to skip, none of them can have
      started executing yet. So we set e->stop_count here and use it to
      decide in the worker threads whether to continue executing an event
      group or whether to skip it, when force_abort is set.

      If we stop due to reaching the START SLAVE UNTIL condition, then we
      need to continue executing any queued events up to that point.
    */
    e->force_abort= true;
    e->stop_count= rli->stop_for_until ?
      e->count_queued_event_groups : e->count_committing_event_groups;
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
    for (j= 0; j < e->rpl_thread_max; ++j)
    {
      if ((rpt= e->rpl_threads[j]))
      {
        mysql_mutex_lock(&rpt->LOCK_rpl_thread);
        if (rpt->current_owner == &e->rpl_threads[j])
          mysql_cond_signal(&rpt->COND_rpl_thread);
        mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      }
    }
  }
  DBUG_EXECUTE_IF("rpl_parallel_wait_for_done_trigger",
  {
    debug_sync_set_action(thd,
                          STRING_WITH_LEN("now SIGNAL wait_for_done_waiting"));
  };);

  global_rpl_thread_pool.copy_pool_for_pfs(rli);
  /*
    Shutdown SA alter threads through marking their execution states
    to force their early post-SA execution exit. Upon that the affected SA threads
    change their state to COMPLETED, notify any waiting CA|RA and this thread.
  */
  start_alter_info *info=NULL;
  mysql_mutex_lock(&mi->start_alter_list_lock);
  List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
  mi->is_shutdown= true;   // a sign to stop in concurrently coming in new SA:s
  while ((info= info_iterator++))
  {
    mysql_mutex_lock(&mi->start_alter_lock);
    if (info->state == start_alter_state::COMPLETED)
    {
      mysql_mutex_unlock(&mi->start_alter_lock);
      continue;
    }
    info->state= start_alter_state::ROLLBACK_ALTER;
    // Any possible CA that is (will be) waiting will complete this ALTER instance
    info->direct_commit_alter= true;
    mysql_cond_broadcast(&info->start_alter_cond); // notify SA:s
    mysql_mutex_unlock(&mi->start_alter_lock);

    // await SA in the COMPLETED state
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state == start_alter_state::ROLLBACK_ALTER)
      mysql_cond_wait(&info->start_alter_cond, &mi->start_alter_lock);

    DBUG_ASSERT(info->state == start_alter_state::COMPLETED);

    mysql_mutex_unlock(&mi->start_alter_lock);
  }
  mysql_mutex_unlock(&mi->start_alter_list_lock);

  DBUG_EXECUTE_IF("rpl_slave_stop_CA_before_binlog",
    {
      debug_sync_set_action(thd, STRING_WITH_LEN("now signal proceed_CA_1"));
    });

  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    for (j= 0; j < e->rpl_thread_max; ++j)
    {
      if ((rpt= e->rpl_threads[j]))
      {
        mysql_mutex_lock(&rpt->LOCK_rpl_thread);
        while (rpt->current_owner == &e->rpl_threads[j])
          mysql_cond_wait(&rpt->COND_rpl_thread_stop, &rpt->LOCK_rpl_thread);
        mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      }
    }
  }
  // Now that all threads are docked, remained alter states are safe to destroy
  mysql_mutex_lock(&mi->start_alter_list_lock);
  info_iterator.rewind();
  while ((info= info_iterator++))
  {
    info_iterator.remove();
    mysql_cond_destroy(&info->start_alter_cond);
    my_free(info);
  }
  mi->is_shutdown= false;
  mysql_mutex_unlock(&mi->start_alter_list_lock);
}


/*
  This function handles the case where the SQL driver thread reached the
  START SLAVE UNTIL position; we stop queueing more events but continue
  processing remaining, already queued events; then use executes manual
  STOP SLAVE; then this function signals to worker threads that they
  should stop the processing of any remaining queued events.
*/
void
rpl_parallel::stop_during_until()
{
  struct rpl_parallel_entry *e;
  uint32 i;

  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    if (e->force_abort)
      e->stop_count= e->count_committing_event_groups;
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
  }
}


bool
rpl_parallel::workers_idle()
{
  struct rpl_parallel_entry *e;
  uint32 i, max_i;

  max_i= domain_hash.records;
  for (i= 0; i < max_i; ++i)
  {
    bool active;
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    active= e->current_sub_id > e->last_committed_sub_id;
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
    if (active)
      break;
  }
  return (i == max_i);
}


int
rpl_parallel_entry::queue_master_restart(rpl_group_info *rgi,
                                         Format_description_log_event *fdev)
{
  uint32 idx;
  rpl_parallel_thread *thr;
  rpl_parallel_thread::queued_event *qev;
  Relay_log_info *rli= rgi->rli;

  /*
    We only need to queue the server restart if we still have a thread working
    on a (potentially partial) event group.

    If the last thread we queued for has finished, then it cannot have any
    partial event group that needs aborting.

    Thus there is no need for the full complexity of choose_thread(). We only
    need to check if we have a current worker thread, and queue for it if so.
  */
  idx= rpl_thread_idx;
  thr= rpl_threads[idx];
  if (!thr)
    return 0;
  mysql_mutex_lock(&thr->LOCK_rpl_thread);
  if (thr->current_owner != &rpl_threads[idx])
  {
    /* No active worker thread, so no need to queue the master restart. */
    mysql_mutex_unlock(&thr->LOCK_rpl_thread);
    return 0;
  }

  if (!(qev= thr->get_qev(fdev, 0, rli)))
  {
    mysql_mutex_unlock(&thr->LOCK_rpl_thread);
    return 1;
  }

  qev->rgi= rgi;
  qev->typ= rpl_parallel_thread::queued_event::QUEUED_MASTER_RESTART;
  qev->entry_for_queued= this;
  qev->ir= rli->last_inuse_relaylog;
  ++qev->ir->queued_count;
  thr->enqueue(qev);
  mysql_cond_signal(&thr->COND_rpl_thread);
  mysql_mutex_unlock(&thr->LOCK_rpl_thread);
  return 0;
}


int
rpl_parallel::wait_for_workers_idle(THD *thd)
{
  uint32 i, max_i;

  /*
    The domain_hash is only accessed by the SQL driver thread, so it is safe
    to iterate over without a lock.
  */
  max_i= domain_hash.records;
  for (i= 0; i < max_i; ++i)
  {
    PSI_stage_info old_stage;
    struct rpl_parallel_entry *e;
    int err= 0;

    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    ++e->need_sub_id_signal;
    thd->ENTER_COND(&e->COND_parallel_entry, &e->LOCK_parallel_entry,
                    &stage_waiting_for_workers_idle, &old_stage);
    while (e->current_sub_id > e->last_committed_sub_id)
    {
      if (unlikely(thd->check_killed()))
      {
        err= 1;
        break;
      }
      mysql_cond_wait(&e->COND_parallel_entry, &e->LOCK_parallel_entry);
    }
    --e->need_sub_id_signal;
    thd->EXIT_COND(&old_stage);
    if (err)
      return err;
  }
  return 0;
}


/*
  Handle seeing a GTID during slave restart in GTID mode. If we stopped with
  different replication domains having reached different positions in the relay
  log, we need to skip event groups in domains that are further progressed.

  Updates the state with the seen GTID, and returns true if this GTID should
  be skipped, false otherwise.
*/
bool
process_gtid_for_restart_pos(Relay_log_info *rli, rpl_gtid *gtid)
{
  slave_connection_state::entry *gtid_entry;
  slave_connection_state *state= &rli->restart_gtid_pos;

  if (likely(state->count() == 0) ||
      !(gtid_entry= state->find_entry(gtid->domain_id)))
    return false;
  if (gtid->server_id == gtid_entry->gtid.server_id)
  {
    uint64 seq_no= gtid_entry->gtid.seq_no;
    if (gtid->seq_no >= seq_no)
    {
      /*
        This domain has reached its start position. So remove it, so that
        further events will be processed normally.
      */
      state->remove(&gtid_entry->gtid);
    }
    return gtid->seq_no <= seq_no;
  }
  else
    return true;
}


/*
  This is used when we get an error during processing in do_event();
  We will not queue any event to the thread, but we still need to wake it up
  to be sure that it will be returned to the pool.
*/
static void
abandon_worker_thread(THD *thd, rpl_parallel_thread *cur_thread,
                      bool *did_enter_cond, PSI_stage_info *old_stage)
{
  unlock_or_exit_cond(thd, &cur_thread->LOCK_rpl_thread,
                      did_enter_cond, old_stage);
  mysql_cond_signal(&cur_thread->COND_rpl_thread);
}


/*
  do_event() is executed by the sql_driver_thd thread.
  It's main purpose is to find a thread that can execute the query.

  @retval  0    ok, event was accepted
  @retval  1    error
  @retval -1    event should be executed serially, in the sql driver thread
*/

int
rpl_parallel::do_event(rpl_group_info *serial_rgi, Log_event *ev,
                       ulonglong event_size)
{
  rpl_parallel_entry *e;
  rpl_parallel_thread *cur_thread;
  rpl_parallel_thread::queued_event *qev;
  rpl_group_info *rgi= NULL;
  Relay_log_info *rli= serial_rgi->rli;
  enum Log_event_type typ;
  bool is_group_event;
  bool did_enter_cond= false;
  PSI_stage_info old_stage;

  DBUG_EXECUTE_IF("slave_crash_if_parallel_apply", DBUG_SUICIDE(););
  /* Handle master log name change, seen in Rotate_log_event. */
  typ= ev->get_type_code();
  if (unlikely(typ == ROTATE_EVENT))
  {
    Rotate_log_event *rev= static_cast<Rotate_log_event *>(ev);
    if ((rev->server_id != global_system_variables.server_id ||
         rli->replicate_same_server_id) &&
        !rev->is_relay_log_event() &&
        !rli->is_in_group())
    {
      memcpy(rli->future_event_master_log_name,
             rev->new_log_ident, rev->ident_len+1);
      rli->notify_group_master_log_name_update();
    }
  }

  /*
    Execute queries non-parallel if slave_skip_counter is set, as it's is
    easier to skip queries in single threaded mode.
  */
  if (rli->slave_skip_counter)
    return -1;

  /* Execute pre-10.0 event, which have no GTID, in single-threaded mode. */
  is_group_event= Log_event::is_group_event(typ);
  if (unlikely(!current) && typ != GTID_EVENT &&
      !(unlikely(rli->gtid_skip_flag != GTID_SKIP_NOT) && is_group_event))
    return -1;

  /* Note: rli->data_lock is released by sql_delay_event(). */
  if (sql_delay_event(ev, rli->sql_driver_thd, serial_rgi))
  {
    /*
      If sql_delay_event() returns non-zero, it means that the wait timed out
      due to slave stop. We should not queue the event in this case, it must
      not be applied yet.
    */
    delete ev;
    return 1;
  }

  if (unlikely(typ == FORMAT_DESCRIPTION_EVENT))
  {
    Format_description_log_event *fdev=
      static_cast<Format_description_log_event *>(ev);
    if (fdev->created)
    {
      /*
        This format description event marks a new binlog after a master server
        restart. We are going to close all temporary tables to clean up any
        possible left-overs after a prior master crash.

        Thus we need to wait for all prior events to execute to completion,
        in case they need access to any of the temporary tables.

        We also need to notify the worker thread running the prior incomplete
        event group (if any), as such event group signifies an incompletely
        written group cut short by a master crash, and must be rolled back.
      */
      if (current->queue_master_restart(serial_rgi, fdev) ||
          wait_for_workers_idle(rli->sql_driver_thd))
      {
        delete ev;
        return 1;
      }
    }
  }
  else if (unlikely(typ == GTID_LIST_EVENT))
  {
    Gtid_list_log_event *glev= static_cast<Gtid_list_log_event *>(ev);
    rpl_gtid *list= glev->list;
    uint32 count= glev->count;
    rli->update_relay_log_state(list, count);
    while (count)
    {
      process_gtid_for_restart_pos(rli, list);
      ++list;
      --count;
    }
  }

  /*
    Stop queueing additional event groups once the SQL thread is requested to
    stop.

    We have to queue any remaining events of any event group that has already
    been partially queued, but after that we will just ignore any further
    events the SQL driver thread may try to queue, and eventually it will stop.
  */
  if ((typ == GTID_EVENT || !is_group_event) && rli->abort_slave)
    sql_thread_stopping= true;
  if (sql_thread_stopping)
  {
    delete ev;
    /*
      Return "no error"; normal stop is not an error, and otherwise the error
      has already been recorded.
    */
    return 0;
  }

  if (unlikely(rli->gtid_skip_flag != GTID_SKIP_NOT) && is_group_event)
  {
    if (typ == GTID_EVENT)
      rli->gtid_skip_flag= GTID_SKIP_NOT;
    else
    {
      if (rli->gtid_skip_flag == GTID_SKIP_STANDALONE)
      {
        if (!Log_event::is_part_of_group(typ))
          rli->gtid_skip_flag= GTID_SKIP_NOT;
      }
      else
      {
        DBUG_ASSERT(rli->gtid_skip_flag == GTID_SKIP_TRANSACTION);
        if (typ == XID_EVENT || typ == XA_PREPARE_LOG_EVENT ||
            (typ == QUERY_EVENT &&  // COMMIT/ROLLBACK are never compressed
             (((Query_log_event *)ev)->is_commit() ||
              ((Query_log_event *)ev)->is_rollback())))
          rli->gtid_skip_flag= GTID_SKIP_NOT;
      }
      delete_or_keep_event_post_apply(serial_rgi, typ, ev);
      return 0;
    }
  }

  Gtid_log_event *gtid_ev= NULL;
  if (typ == GTID_EVENT)
  {
    rpl_gtid gtid;
    gtid_ev= static_cast<Gtid_log_event *>(ev);
    uint32 domain_id= (rli->mi->using_gtid == Master_info::USE_GTID_NO ||
                       rli->mi->parallel_mode <= SLAVE_PARALLEL_MINIMAL ?
                       0 : gtid_ev->domain_id);
    if (!(e= find(domain_id, rli)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
      delete ev;
      return 1;
    }
    current= e;

    gtid.domain_id= gtid_ev->domain_id;
    gtid.server_id= gtid_ev->server_id;
    gtid.seq_no= gtid_ev->seq_no;
    rli->update_relay_log_state(&gtid, 1);
    serial_rgi->gtid_ev_flags_extra= gtid_ev->flags_extra;
    if (process_gtid_for_restart_pos(rli, &gtid))
    {
      /*
        This domain has progressed further into the relay log before the last
        SQL thread restart. So we need to skip this event group to not doubly
        apply it.
      */
      rli->gtid_skip_flag= ((gtid_ev->flags2 & Gtid_log_event::FL_STANDALONE) ?
                            GTID_SKIP_STANDALONE : GTID_SKIP_TRANSACTION);
      delete_or_keep_event_post_apply(serial_rgi, typ, ev);
      return 0;
    }
  }
  else
    e= current;

  /*
    Find a worker thread to queue the event for.
    Prefer a new thread, so we maximise parallelism (at least for the group
    commit). But do not exceed a limit of --slave-domain-parallel-threads;
    instead re-use a thread that we queued for previously.
  */
  cur_thread=
    e->choose_thread(serial_rgi, &did_enter_cond, &old_stage, gtid_ev);
  if (!cur_thread)
  {
    /* This means we were killed. The error is already signalled. */
    delete ev;
    return 1;
  }

  if (!(qev= cur_thread->get_qev(ev, event_size, rli)))
  {
    abandon_worker_thread(rli->sql_driver_thd, cur_thread,
                          &did_enter_cond, &old_stage);
    delete ev;
    return 1;
  }

  if (typ == GTID_EVENT)
  {
    bool new_gco;
    enum_slave_parallel_mode mode= rli->mi->parallel_mode;
    uchar gtid_flags= gtid_ev->flags2;
    group_commit_orderer *gco;
    uint8 force_switch_flag;
    enum rpl_group_info::enum_speculation speculation;

    if (!(rgi= cur_thread->get_rgi(rli, gtid_ev, e, event_size)))
    {
      cur_thread->free_qev(qev);
      abandon_worker_thread(rli->sql_driver_thd, cur_thread,
                            &did_enter_cond, &old_stage);
      delete ev;
      return 1;
    }

    /*
      We queue the event group in a new worker thread, to run in parallel
      with previous groups.

      To preserve commit order within the replication domain, we set up
      rgi->wait_commit_sub_id to make the new group commit only after the
      previous group has committed.

      Event groups that group-committed together on the master can be run
      in parallel with each other without restrictions. But one batch of
      group-commits may not start before all groups in the previous batch
      have initiated their commit phase; we set up rgi->gco to ensure that.
    */
    rgi->wait_commit_sub_id= e->current_sub_id;
    rgi->wait_commit_group_info= e->current_group_info;

    speculation= rpl_group_info::SPECULATE_NO;
    new_gco= true;
    force_switch_flag= 0;
    gco= e->current_gco;
    if (likely(gco))
    {
      uint8 flags= gco->flags;

      if (mode <= SLAVE_PARALLEL_MINIMAL ||
          !(gtid_flags & Gtid_log_event::FL_GROUP_COMMIT_ID) ||
          e->last_commit_id != gtid_ev->commit_id)
        flags|= group_commit_orderer::MULTI_BATCH;
      /* Make sure we do not attempt to run DDL in parallel speculatively. */
      if (gtid_flags & Gtid_log_event::FL_DDL)
        flags|= (force_switch_flag= group_commit_orderer::FORCE_SWITCH);

      if (!(flags & group_commit_orderer::MULTI_BATCH))
      {
        /*
          Still the same batch of event groups that group-committed together
          on the master, so we can run in parallel.
        */
        new_gco= false;
      }
      else if ((mode >= SLAVE_PARALLEL_OPTIMISTIC) &&
               !(flags & group_commit_orderer::FORCE_SWITCH))
      {
        /*
          In transactional parallel mode, we optimistically attempt to run
          non-DDL in parallel. In case of conflicts, we catch the conflict as
          a deadlock or other error, roll back and retry serially.

          The assumption is that only a few event groups will be
          non-transactional or otherwise unsuitable for parallel apply. Those
          transactions are still scheduled in parallel, but we set a flag that
          will make the worker thread wait for everything before to complete
          before starting.
        */
        new_gco= false;
        if (!(gtid_flags & Gtid_log_event::FL_TRANSACTIONAL) ||
            ( (!(gtid_flags & Gtid_log_event::FL_ALLOW_PARALLEL) ||
               (gtid_flags & Gtid_log_event::FL_WAITED)) &&
              (mode < SLAVE_PARALLEL_AGGRESSIVE)))
        {
          /*
            This transaction should not be speculatively run in parallel with
            what came before, either because it cannot safely be rolled back in
            case of a conflict, or because it was marked as likely to conflict
            and require expensive rollback and retry.

            Here we mark it as such, and then the worker thread will do a
            wait_for_prior_commit() before starting it. We do not introduce a
            new group_commit_orderer, since we still want following transactions
            to run in parallel with transactions prior to this one.
          */
          speculation= rpl_group_info::SPECULATE_WAIT;
        }
        else
          speculation= rpl_group_info::SPECULATE_OPTIMISTIC;
      }
      gco->flags= flags;
    }
    else
    {
      if (gtid_flags & Gtid_log_event::FL_DDL)
        force_switch_flag= group_commit_orderer::FORCE_SWITCH;
    }
    rgi->speculation= speculation;

    if (gtid_flags & Gtid_log_event::FL_GROUP_COMMIT_ID)
      e->last_commit_id= gtid_ev->commit_id;
    else
      e->last_commit_id= 0;

    if (new_gco)
    {
      /*
        Do not run this event group in parallel with what came before; instead
        wait for everything prior to at least have started its commit phase, to
        avoid any risk of performing any conflicting action too early.

        Remember the count that marks the end of the previous batch of event
        groups that run in parallel, and allocate a new gco.
      */
      uint64 count= e->count_queued_event_groups;

      if (!(gco= cur_thread->get_gco(count, gco, e->current_sub_id)))
      {
        cur_thread->free_rgi(rgi);
        cur_thread->free_qev(qev);
        abandon_worker_thread(rli->sql_driver_thd, cur_thread,
                              &did_enter_cond, &old_stage);
        delete ev;
        return 1;
      }
      gco->flags|= force_switch_flag;
      e->current_gco= gco;
    }
    rgi->gco= gco;

    qev->rgi= e->current_group_info= rgi;
    e->current_sub_id= rgi->gtid_sub_id;
    ++e->count_queued_event_groups;
  }
  else if (!is_group_event)
  {
    int err;
    bool tmp;
    /*
      Events like ROTATE and FORMAT_DESCRIPTION. Do not run in worker thread.
      Same for events not preceeded by GTID (we should not see those normally,
      but they might be from an old master).
    */
    qev->rgi= serial_rgi;

    tmp= serial_rgi->is_parallel_exec;
    serial_rgi->is_parallel_exec= true;
    err= rpt_handle_event(qev, NULL);
    serial_rgi->is_parallel_exec= tmp;
    if (ev->is_relay_log_event())
      qev->future_event_master_log_pos= 0;
    else if (typ == ROTATE_EVENT)
      qev->future_event_master_log_pos=
        (static_cast<Rotate_log_event *>(ev))->pos;
    else
      qev->future_event_master_log_pos= ev->log_pos;
    delete_or_keep_event_post_apply(serial_rgi, typ, ev);

    if (err)
    {
      cur_thread->free_qev(qev);
      abandon_worker_thread(rli->sql_driver_thd, cur_thread,
                            &did_enter_cond, &old_stage);
      return 1;
    }
    /*
      Queue a position update, so that the position will be updated in a
      reasonable way relative to other events:

       - If the currently executing events are queued serially for a single
         thread, the position will only be updated when everything before has
         completed.

       - If we are executing multiple independent events in parallel, then at
         least the position will not be updated until one of them has reached
         the current point.
    */
    qev->typ= rpl_parallel_thread::queued_event::QUEUED_POS_UPDATE;
    qev->entry_for_queued= e;
  }
  else
  {
    qev->rgi= e->current_group_info;
  }

  /*
    Queue the event for processing.
  */
  qev->ir= rli->last_inuse_relaylog;
  ++qev->ir->queued_count;
  cur_thread->enqueue(qev);
  unlock_or_exit_cond(rli->sql_driver_thd, &cur_thread->LOCK_rpl_thread,
                      &did_enter_cond, &old_stage);
  mysql_cond_signal(&cur_thread->COND_rpl_thread);

  return 0;
}
