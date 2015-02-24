#include "my_global.h"
#include "rpl_parallel.h"
#include "slave.h"
#include "rpl_mi.h"
#include "debug_sync.h"

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

  thd->system_thread_info.rpl_sql_info->rpl_filter = rli->mi->rpl_filter;
  ev->thd= thd;

  strcpy(rgi->event_relay_log_name_buf, qev->event_relay_log_name);
  rgi->event_relay_log_name= rgi->event_relay_log_name_buf;
  rgi->event_relay_log_pos= qev->event_relay_log_pos;
  rgi->future_event_relay_log_pos= qev->future_event_relay_log_pos;
  strcpy(rgi->future_event_master_log_name, qev->future_event_master_log_name);
  mysql_mutex_lock(&rli->data_lock);
  /* Mutex will be released in apply_event_and_update_pos(). */
  err= apply_event_and_update_pos(ev, thd, rgi, rpt);

  thread_safe_increment64(&rli->executed_entries,
                          &slave_executed_entries_lock);
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
  e= qev->entry_for_queued;
  if (e->stop_on_error_sub_id < (uint64)ULONGLONG_MAX || e->force_abort)
    return;

  rli= qev->rgi->rli;
  mysql_mutex_lock(&rli->data_lock);
  cmp= strcmp(rli->group_relay_log_name, qev->event_relay_log_name);
  if (cmp < 0)
  {
    rli->group_relay_log_pos= qev->future_event_relay_log_pos;
    strmake_buf(rli->group_relay_log_name, qev->event_relay_log_name);
    rli->notify_group_relay_log_name_update();
  } else if (cmp == 0 &&
             rli->group_relay_log_pos < qev->future_event_relay_log_pos)
    rli->group_relay_log_pos= qev->future_event_relay_log_pos;

  cmp= strcmp(rli->group_master_log_name, qev->future_event_master_log_name);
  if (cmp < 0)
  {
    strcpy(rli->group_master_log_name, qev->future_event_master_log_name);
    rli->notify_group_master_log_name_update();
    rli->group_master_log_pos= qev->future_event_master_log_pos;
  }
  else if (cmp == 0
           && rli->group_master_log_pos < qev->future_event_master_log_pos)
    rli->group_master_log_pos= qev->future_event_master_log_pos;
  mysql_mutex_unlock(&rli->data_lock);
  mysql_cond_broadcast(&rli->data_cond);
}


static void
finish_event_group(rpl_parallel_thread *rpt, uint64 sub_id,
                   rpl_parallel_entry *entry, rpl_group_info *rgi)
{
  THD *thd= rpt->thd;
  wait_for_commit *wfc= &rgi->commit_orderer;
  int err;

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

    But in the error case, we have to abort anyway, and it seems best
    to just complete as quickly as possible with unregister. Anyone
    waiting for us will in any case receive the error back from their
    wait_for_prior_commit() call.
  */
  if (rgi->worker_error)
    wfc->unregister_wait_for_prior_commit();
  else if ((err= wfc->wait_for_prior_commit(thd)))
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

    /* Now free any GCOs in which all transactions have committed. */
    group_commit_orderer *tmp_gco= rgi->gco;
    while (tmp_gco &&
           (!tmp_gco->next_gco || tmp_gco->last_sub_id > sub_id))
      tmp_gco= tmp_gco->prev_gco;
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

  thd->clear_error();
  thd->reset_killed();
  thd->get_stmt_da()->reset_diagnostics_area();
  wfc->wakeup_subsequent_commits(rgi->worker_error);
}


static void
signal_error_to_sql_driver_thread(THD *thd, rpl_group_info *rgi, int err)
{
  rgi->worker_error= err;
  rgi->cleanup_context(thd, true);
  rgi->rli->abort_slave= true;
  rgi->rli->stop_for_until= false;
  mysql_mutex_lock(rgi->rli->relay_log.get_log_lock());
  mysql_mutex_unlock(rgi->rli->relay_log.get_log_lock());
  rgi->rli->relay_log.signal_update();
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
  the later transaction.  */
static void
convert_kill_to_deadlock_error(rpl_group_info *rgi)
{
  THD *thd= rgi->thd;
  int err_code;

  if (!thd->get_stmt_da()->is_error())
    return;
  err_code= thd->get_stmt_da()->sql_errno();
  if ((err_code == ER_QUERY_INTERRUPTED || err_code == ER_CONNECTION_KILLED) &&
      rgi->killed_for_retry)
  {
    thd->clear_error();
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    rgi->killed_for_retry= false;
    thd->reset_killed();
  }
}


static bool
is_group_ending(Log_event *ev, Log_event_type event_type)
{
  return event_type == XID_EVENT ||
         (event_type == QUERY_EVENT &&
          (((Query_log_event *)ev)->is_commit() ||
           ((Query_log_event *)ev)->is_rollback()));
}


static int
retry_event_group(rpl_group_info *rgi, rpl_parallel_thread *rpt,
                  rpl_parallel_thread::queued_event *orig_qev)
{
  IO_CACHE rlog;
  LOG_INFO linfo;
  File fd= (File)-1;
  const char *errmsg= NULL;
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
  rgi->cleanup_context(thd, 1);

  /*
    If we retry due to a deadlock kill that occured during the commit step, we
    might have already updated (but not committed) an update of table
    mysql.gtid_slave_pos, and cleared the gtid_pending flag. Now we have
    rolled back any such update, so we must set the gtid_pending flag back to
    true so that we will do a new update when/if we succeed with the retry.
  */
  rgi->gtid_pending= true;

  mysql_mutex_lock(&rli->data_lock);
  ++rli->retried_trans;
  statistic_increment(slave_retried_transactions, LOCK_status);
  mysql_mutex_unlock(&rli->data_lock);

  mysql_mutex_lock(&entry->LOCK_parallel_entry);
  register_wait_for_prior_event_group_commit(rgi, entry);
  mysql_mutex_unlock(&entry->LOCK_parallel_entry);

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
      ev= Log_event::read_log_event(&rlog, 0, description_event,
                                    opt_slave_sql_verify_checksum);
      cur_offset= my_b_tell(&rlog);

      if (ev)
        break;
      if (rlog.error < 0)
      {
        errmsg= "slave SQL thread aborted because of I/O error";
        err= 1;
        goto err;
      }
      if (rlog.error > 0)
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

      if ((fd= open_binlog(&rlog, log_name, &errmsg)) <0)
      {
        err= 1;
        goto err;
      }
      /* Loop to try again on the new log file. */
    }

    event_type= ev->get_type_code();
    if (event_type == FORMAT_DESCRIPTION_EVENT)
    {
      delete description_event;
      description_event= (Format_description_log_event *)ev;
      continue;
    } else if (!Log_event::is_group_event(event_type))
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
    if (is_group_ending(ev, event_type))
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
    if (err)
    {
      convert_kill_to_deadlock_error(rgi);
      if (has_temporary_error(thd))
      {
        ++retries;
        if (retries < slave_trans_retries)
        {
          end_io_cache(&rlog);
          mysql_file_close(fd, MYF(MY_WME));
          fd= (File)-1;
          goto do_retry;
        }
        sql_print_error("Slave worker thread retried transaction %lu time(s) "
                        "in vain, giving up. Consider raising the value of "
                        "the slave_transaction_retries variable.",
                        slave_trans_retries);
      }
      goto err;
    }
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
  thd = new THD;
  thd->thread_stack = (char*)&thd;
  mysql_mutex_lock(&LOCK_thread_count);
  thd->thread_id= thd->variables.pseudo_thread_id= thread_id++;
  threads.append(thd);
  mysql_mutex_unlock(&LOCK_thread_count);
  set_current_thd(thd);
  pthread_detach_this_thread();
  thd->init_for_queries();
  thd->variables.binlog_annotate_row_events= 0;
  init_thr_lock();
  thd->store_globals();
  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();
  thd->variables.max_allowed_packet= slave_max_allowed_packet;
  thd->slave_thread= 1;
  thd->enable_slow_log= opt_log_slow_slave_statements;
  thd->variables.log_slow_filter= global_system_variables.log_slow_filter;
  set_slave_thread_options(thd);
  thd->client_capabilities = CLIENT_LOCAL_FILES;
  thd->net.reading_or_writing= 0;
  thd_proc_info(thd, "Waiting for work from main SQL threads");
  thd->set_time();
  thd->variables.lock_wait_timeout= LONG_TIMEOUT;
  thd->system_thread_info.rpl_sql_info= &sql_info;

  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->thd= thd;

  while (rpt->delay_start)
    mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);

  rpt->running= true;
  mysql_cond_signal(&rpt->COND_rpl_thread);

  while (!rpt->stop)
  {
    rpl_parallel_thread::queued_event *qev, *next_qev;

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
      mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);
    rpt->dequeue1(events);
    thd->EXIT_COND(&old_stage);

  more_events:
    for (qev= events; qev; qev= next_qev)
    {
      Log_event_type event_type;
      rpl_group_info *rgi= qev->rgi;
      rpl_parallel_entry *entry= rgi->parallel_entry;
      bool end_of_group, group_ending;

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
        bool did_enter_cond= false;
        PSI_stage_info old_stage;
        uint64 wait_count;

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

        /*
          Register ourself to wait for the previous commit, if we need to do
          such registration _and_ that previous commit has not already
          occured.

          Also do not start parallel execution of this event group until all
          prior groups have reached the commit phase that are not safe to run
          in parallel with.
        */
        mysql_mutex_lock(&entry->LOCK_parallel_entry);
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
                          &old_stage);
          did_enter_cond= true;
          do
          {
            if (thd->check_killed() && !rgi->worker_error)
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
            We are stopping (STOP SLAVE), and this event group is beyond the
            point where we can safely stop. So set a flag that will cause us
            to skip, rather than execute, the following events.
          */
          skip_event_group= true;
        }
        else
          skip_event_group= false;

        if (unlikely(entry->stop_on_error_sub_id <= rgi->wait_commit_sub_id))
          skip_event_group= true;
        else
          register_wait_for_prior_event_group_commit(rgi, entry);

        unlock_or_exit_cond(thd, &entry->LOCK_parallel_entry,
                            &did_enter_cond, &old_stage);

        thd->wait_for_commit_ptr= &rgi->commit_orderer;

        if (opt_gtid_ignore_duplicates)
        {
          int res=
            rpl_global_gtid_slave_state.check_duplicate_gtid(&rgi->current_gtid,
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
      }

      group_rgi= rgi;
      group_ending= is_group_ending(qev->ev, event_type);
      if (group_ending && likely(!rgi->worker_error))
      {
        DEBUG_SYNC(thd, "rpl_parallel_before_mark_start_commit");
        rgi->mark_start_commit();
        DEBUG_SYNC(thd, "rpl_parallel_after_mark_start_commit");
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
        err= rpt_handle_event(qev, rpt);
        delete_or_keep_event_post_apply(rgi, event_type, qev->ev);
        DBUG_EXECUTE_IF("rpl_parallel_simulate_temp_err_gtid_0_x_100",
                        err= dbug_simulate_tmp_error(rgi, thd););
        if (err)
        {
          convert_kill_to_deadlock_error(rgi);
          if (has_temporary_error(thd) && slave_trans_retries > 0)
            err= retry_event_group(rgi, rpt, qev);
        }
      }
      else
      {
        delete qev->ev;
        err= thd->wait_for_prior_commit();
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
        This is faster than having to wakeup the pool manager thread to give us
        a new event.
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
      rpt->current_owner= NULL;
      /* Tell wait_for_done() that we are done, if it is waiting. */
      if (likely(rpt->current_entry) &&
          unlikely(rpt->current_entry->force_abort))
        mysql_cond_broadcast(&rpt->current_entry->COND_parallel_entry);
      rpt->current_entry= NULL;
      if (!rpt->stop)
        rpt->pool->release_thread(rpt);
    }
  }

  rpt->thd= NULL;
  mysql_mutex_unlock(&rpt->LOCK_rpl_thread);

  thd->clear_error();
  thd->catalog= 0;
  thd->reset_query();
  thd->reset_db(NULL, 0);
  thd_proc_info(thd, "Slave worker thread exiting");
  thd->temporary_tables= 0;
  mysql_mutex_lock(&LOCK_thread_count);
  THD_CHECK_SENTRY(thd);
  delete thd;
  mysql_mutex_unlock(&LOCK_thread_count);

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


int
rpl_parallel_change_thread_count(rpl_parallel_thread_pool *pool,
                                 uint32 new_count, bool skip_check)
{
  uint32 i;
  rpl_parallel_thread **new_list= NULL;
  rpl_parallel_thread *new_free_list= NULL;
  rpl_parallel_thread *rpt_array= NULL;

  /*
    Allocate the new list of threads up-front.
    That way, if we fail half-way, we only need to free whatever we managed
    to allocate, and will not be left with a half-functional thread pool.
  */
  if (new_count &&
      !my_multi_malloc(MYF(MY_WME|MY_ZEROFILL),
                       &new_list, new_count*sizeof(*new_list),
                       &rpt_array, new_count*sizeof(*rpt_array),
                       NULL))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int(new_count*sizeof(*new_list) +
                                          new_count*sizeof(*rpt_array))));
    goto err;;
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

  if (!skip_check)
  {
    mysql_mutex_lock(&LOCK_active_mi);
    if (master_info_index->give_error_if_slave_running())
    {
      mysql_mutex_unlock(&LOCK_active_mi);
      goto err;
    }
    if (pool->changing)
    {
      mysql_mutex_unlock(&LOCK_active_mi);
      my_error(ER_CHANGE_SLAVE_PARALLEL_THREADS_ACTIVE, MYF(0));
      goto err;
    }
    pool->changing= true;
    mysql_mutex_unlock(&LOCK_active_mi);
  }

  /*
    Grab each old thread in turn, and signal it to stop.

    Note that since we require all replication threads to be stopped before
    changing the parallel replication worker thread pool, all the threads will
    be already idle and will terminate immediately.
  */
  for (i= 0; i < pool->count; ++i)
  {
    rpl_parallel_thread *rpt= pool->get_thread(NULL, NULL);
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

  my_free(pool->threads);
  pool->threads= new_list;
  pool->free_list= new_free_list;
  pool->count= new_count;
  for (i= 0; i < pool->count; ++i)
  {
    mysql_mutex_lock(&pool->threads[i]->LOCK_rpl_thread);
    pool->threads[i]->delay_start= false;
    mysql_cond_signal(&pool->threads[i]->COND_rpl_thread);
    while (!pool->threads[i]->running)
      mysql_cond_wait(&pool->threads[i]->COND_rpl_thread,
                      &pool->threads[i]->LOCK_rpl_thread);
    mysql_mutex_unlock(&pool->threads[i]->LOCK_rpl_thread);
  }

  if (!skip_check)
  {
    mysql_mutex_lock(&LOCK_active_mi);
    pool->changing= false;
    mysql_mutex_unlock(&LOCK_active_mi);
  }

  mysql_mutex_lock(&pool->LOCK_rpl_thread_pool);
  mysql_cond_broadcast(&pool->COND_rpl_thread_pool);
  mysql_mutex_unlock(&pool->LOCK_rpl_thread_pool);

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
  if (!skip_check)
  {
    mysql_mutex_lock(&LOCK_active_mi);
    pool->changing= false;
    mysql_mutex_unlock(&LOCK_active_mi);
  }
  return 1;
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
    my_atomic_rwlock_wrlock(&ir->inuse_relaylog_atomic_lock);
    my_atomic_add64(&ir->dequeued_count, accumulated_ir_count);
    my_atomic_rwlock_wrunlock(&ir->inuse_relaylog_atomic_lock);
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
  else if(!(qev= (queued_event *)my_malloc(sizeof(*qev), MYF(0))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*qev));
    return NULL;
  }
  qev->typ= rpl_parallel_thread::queued_event::QUEUED_EVENT;
  qev->ev= ev;
  qev->event_size= event_size;
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
  rgi->killed_for_retry= false;

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
  else if(!(gco= (group_commit_orderer *)my_malloc(sizeof(*gco), MYF(0))))
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


rpl_parallel_thread_pool::rpl_parallel_thread_pool()
  : count(0), threads(0), free_list(0), changing(false), inited(false)
{
}


int
rpl_parallel_thread_pool::init(uint32 size)
{
  count= 0;
  threads= NULL;
  free_list= NULL;

  mysql_mutex_init(key_LOCK_rpl_thread_pool, &LOCK_rpl_thread_pool,
                   MY_MUTEX_INIT_SLOW);
  mysql_cond_init(key_COND_rpl_thread_pool, &COND_rpl_thread_pool, NULL);
  changing= false;
  inited= true;

  return rpl_parallel_change_thread_count(this, size, true);
}


void
rpl_parallel_thread_pool::destroy()
{
  if (!inited)
    return;
  rpl_parallel_change_thread_count(this, 0, true);
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

  mysql_mutex_lock(&LOCK_rpl_thread_pool);
  while ((rpt= free_list) == NULL)
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

  If the flag `reuse' is set, the last worker thread will be returned again,
  if it is still available. Otherwise a new worker thread is allocated.
*/
rpl_parallel_thread *
rpl_parallel_entry::choose_thread(rpl_group_info *rgi, bool *did_enter_cond,
                                  PSI_stage_info *old_stage, bool reuse)
{
  uint32 idx;
  Relay_log_info *rli= rgi->rli;
  rpl_parallel_thread *thr;

  idx= rpl_thread_idx;
  if (!reuse)
  {
    ++idx;
    if (idx >= rpl_thread_max)
      idx= 0;
    rpl_thread_idx= idx;
  }
  thr= rpl_threads[idx];
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
      else if (rli->sql_driver_thd->check_killed())
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
  my_hash_init(&domain_hash, &my_charset_bin, 32,
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
rpl_parallel::find(uint32 domain_id)
{
  struct rpl_parallel_entry *e;

  if (!(e= (rpl_parallel_entry *)my_hash_search(&domain_hash,
                                                (const uchar *)&domain_id, 0)))
  {
    /* Allocate a new, empty one. */
    ulong count= opt_slave_domain_parallel_threads;
    if (count == 0 || count > opt_slave_parallel_threads)
      count= opt_slave_parallel_threads;
    rpl_parallel_thread **p;
    if (!my_multi_malloc(MYF(MY_WME|MY_ZEROFILL),
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
    e->force_abort= false;

  return e;
}


void
rpl_parallel::wait_for_done(THD *thd, Relay_log_info *rli)
{
  struct rpl_parallel_entry *e;
  rpl_parallel_thread *rpt;
  uint32 i, j;

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

  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    for (j= 0; j < e->rpl_thread_max; ++j)
    {
      if ((rpt= e->rpl_threads[j]))
      {
        mysql_mutex_lock(&rpt->LOCK_rpl_thread);
        while (rpt->current_owner == &e->rpl_threads[j])
          mysql_cond_wait(&e->COND_parallel_entry, &rpt->LOCK_rpl_thread);
        mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      }
    }
  }
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
    bool active;
    wait_for_commit my_orderer;
    struct rpl_parallel_entry *e;

    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    if ((active= (e->current_sub_id > e->last_committed_sub_id)))
    {
      wait_for_commit *waitee= &e->current_group_info->commit_orderer;
      my_orderer.register_wait_for_prior_commit(waitee);
      thd->wait_for_commit_ptr= &my_orderer;
    }
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
    if (active)
    {
      int err= my_orderer.wait_for_prior_commit(thd);
      thd->wait_for_commit_ptr= NULL;
      if (err)
        return err;
    }
  }
  return 0;
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
    }
  }

  /*
    Execute queries non-parallel if slave_skip_counter is set, as it's is
    easier to skip queries in single threaded mode.
  */
  if (rli->slave_skip_counter)
    return -1;

  /* Execute pre-10.0 event, which have no GTID, in single-threaded mode. */
  if (unlikely(!current) && typ != GTID_EVENT)
    return -1;

  /* ToDo: what to do with this lock?!? */
  mysql_mutex_unlock(&rli->data_lock);

  if (typ == FORMAT_DESCRIPTION_EVENT)
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

  /*
    Stop queueing additional event groups once the SQL thread is requested to
    stop.

    We have to queue any remaining events of any event group that has already
    been partially queued, but after that we will just ignore any further
    events the SQL driver thread may try to queue, and eventually it will stop.
  */
  is_group_event= Log_event::is_group_event(typ);
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

  if (typ == GTID_EVENT)
  {
    Gtid_log_event *gtid_ev= static_cast<Gtid_log_event *>(ev);
    uint32 domain_id= (rli->mi->using_gtid == Master_info::USE_GTID_NO ?
                       0 : gtid_ev->domain_id);
    if (!(e= find(domain_id)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
      delete ev;
      return 1;
    }
    current= e;
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
    e->choose_thread(serial_rgi, &did_enter_cond, &old_stage,
                     typ != GTID_EVENT);
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
    Gtid_log_event *gtid_ev= static_cast<Gtid_log_event *>(ev);

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

    if (!((gtid_ev->flags2 & Gtid_log_event::FL_GROUP_COMMIT_ID) &&
          e->last_commit_id == gtid_ev->commit_id))
    {
      /*
        A new batch of transactions that group-committed together on the master.

        Remember the count that marks the end of the previous group committed
        batch, and allocate a new gco.
      */
      uint64 count= e->count_queued_event_groups;
      group_commit_orderer *gco;

      if (!(gco= cur_thread->get_gco(count, e->current_gco, e->current_sub_id)))
      {
        cur_thread->free_rgi(rgi);
        cur_thread->free_qev(qev);
        abandon_worker_thread(rli->sql_driver_thd, cur_thread,
                              &did_enter_cond, &old_stage);
        delete ev;
        return 1;
      }
      e->current_gco= rgi->gco= gco;
    }
    else
      rgi->gco= e->current_gco;
    if (gtid_ev->flags2 & Gtid_log_event::FL_GROUP_COMMIT_ID)
      e->last_commit_id= gtid_ev->commit_id;
    else
      e->last_commit_id= 0;
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
  rli->event_relay_log_pos= rli->future_event_relay_log_pos;
  qev->ir= rli->last_inuse_relaylog;
  ++qev->ir->queued_count;
  cur_thread->enqueue(qev);
  unlock_or_exit_cond(rli->sql_driver_thd, &cur_thread->LOCK_rpl_thread,
                      &did_enter_cond, &old_stage);
  mysql_cond_signal(&cur_thread->COND_rpl_thread);

  return 0;
}
