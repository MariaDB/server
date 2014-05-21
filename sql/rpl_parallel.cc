#include "my_global.h"
#include "rpl_parallel.h"
#include "slave.h"
#include "rpl_mi.h"
#include "debug_sync.h"


/*
  Code for optional parallel execution of replicated events on the slave.

  ToDo list:

   - Retry of failed transactions is not yet implemented for the parallel case.

   - All the waits (eg. in struct wait_for_commit and in
     rpl_parallel_thread_pool::get_thread()) need to be killable. And on kill,
     everything needs to be correctly rolled back and stopped in all threads,
     to ensure a consistent slave replication state.
*/

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

  thd->rgi_slave= rgi;
  thd->system_thread_info.rpl_sql_info->rpl_filter = rli->mi->rpl_filter;

  /* ToDo: Access to thd, and what about rli, split out a parallel part? */
  mysql_mutex_lock(&rli->data_lock);
  qev->ev->thd= thd;
  strcpy(rgi->event_relay_log_name_buf, qev->event_relay_log_name);
  rgi->event_relay_log_name= rgi->event_relay_log_name_buf;
  rgi->event_relay_log_pos= qev->event_relay_log_pos;
  rgi->future_event_relay_log_pos= qev->future_event_relay_log_pos;
  strcpy(rgi->future_event_master_log_name, qev->future_event_master_log_name);
  err= apply_event_and_update_pos(qev->ev, thd, rgi, rpt);
  thd->rgi_slave= NULL;

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
finish_event_group(THD *thd, uint64 sub_id, rpl_parallel_entry *entry,
                   rpl_group_info *rgi)
{
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
  mysql_mutex_lock(&entry->LOCK_parallel_entry);
  if (entry->last_committed_sub_id < sub_id)
    entry->last_committed_sub_id= sub_id;

  /*
    If this event group got error, then any following event groups that have
    not yet started should just skip their group, preparing for stop of the
    SQL driver thread.
  */
  if (unlikely(rgi->worker_error) &&
      entry->stop_on_error_sub_id == (uint64)ULONGLONG_MAX)
    entry->stop_on_error_sub_id= sub_id;
  /*
    We need to mark that this event group started its commit phase, in case we
    missed it before (otherwise we would deadlock the next event group that is
    waiting for this). In most cases (normal DML), it will be a no-op.
  */
  rgi->mark_start_commit_no_lock();
  mysql_mutex_unlock(&entry->LOCK_parallel_entry);

  thd->clear_error();
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
  group_commit_orderer *gco, *tmp_gco;
  uint64 event_gtid_sub_id= 0;
  rpl_parallel_thread::queued_event *qevs_to_free;
  rpl_group_info *rgis_to_free;
  group_commit_orderer *gcos_to_free;
  rpl_sql_thread_info sql_info(NULL);
  size_t total_event_size;
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
  /*
    For now, we need to run the replication parallel worker threads in
    READ COMMITTED. This is needed because gap locks are not symmetric.
    For example, a gap lock from a DELETE blocks an insert intention lock,
    but not vice versa. So an INSERT followed by DELETE can group commit
    on the master, but if we are unlucky with thread scheduling we can
    then deadlock on the slave because the INSERT ends up waiting for a
    gap lock from the DELETE (and the DELETE in turn waits for the INSERT
    in wait_for_prior_commit()). See also MDEV-5914.

    It should be mostly safe to run in READ COMMITTED in the slave anyway.
    The commit order is already fixed from on the master, so we do not
    risk logging into the binlog in an incorrect order between worker
    threads (one that would cause different results if executed on a
    lower-level slave that uses this slave as a master). The only
    potential problem is with transactions run in a different master
    connection (using multi-source replication), or run directly on the
    slave by an application; when using READ COMMITTED we are not
    guaranteed serialisability of binlogged statements.

    In practice, this is unlikely to be an issue. In GTID mode, such
    parallel transactions from multi-source or application must in any
    case use a different replication domain, in which case binlog order
    by definition must be independent between the different domain. Even
    in non-GTID mode, normally one will assume that the external
    transactions are not conflicting with those applied by the slave, so
    that isolation level should make no difference. It would be rather
    strange if the result of applying query events from one master would
    depend on the timing and nature of other queries executed from
    different multi-source connections or done directly on the slave by
    an application. Still, something to be aware of.
  */
  thd->variables.tx_isolation= ISO_READ_COMMITTED;

  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->thd= thd;

  while (rpt->delay_start)
    mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);

  rpt->running= true;
  mysql_cond_signal(&rpt->COND_rpl_thread);

  while (!rpt->stop)
  {
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
    qevs_to_free= NULL;
    rgis_to_free= NULL;
    gcos_to_free= NULL;
    total_event_size= 0;
    while (events)
    {
      struct rpl_parallel_thread::queued_event *next= events->next;
      Log_event_type event_type;
      rpl_group_info *rgi= events->rgi;
      rpl_parallel_entry *entry= rgi->parallel_entry;
      bool end_of_group, group_ending;

      total_event_size+= events->event_size;
      if (!events->ev)
      {
        handle_queued_pos_update(thd, events);
        events->next= qevs_to_free;
        qevs_to_free= events;
        events= next;
        continue;
      }

      group_rgi= rgi;
      gco= rgi->gco;
      /* Handle a new event group, which will be initiated by a GTID event. */
      if ((event_type= events->ev->get_type_code()) == GTID_EVENT)
      {
        bool did_enter_cond= false;
        PSI_stage_info old_stage;
        uint64 wait_count;

        thd->tx_isolation= (enum_tx_isolation)thd->variables.tx_isolation;
        in_event_group= true;
        /*
          If the standalone flag is set, then this event group consists of a
          single statement (possibly preceeded by some Intvar_log_event and
          similar), without any terminating COMMIT/ROLLBACK/XID.
        */
        group_standalone=
          (0 != (static_cast<Gtid_log_event *>(events->ev)->flags2 &
                 Gtid_log_event::FL_STANDALONE));

        /* Save this, as it gets cleared when the event group commits. */
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
          if (gco->prev_gco)
            gco->prev_gco->next_gco= gco;
          gco->installed= true;
        }
        wait_count= gco->wait_count;
        if (wait_count > entry->count_committing_event_groups)
        {
          DEBUG_SYNC(thd, "rpl_parallel_start_waiting_for_prior");
          thd->ENTER_COND(&gco->COND_group_commit_orderer,
                          &entry->LOCK_parallel_entry,
                          &stage_waiting_for_prior_transaction_to_commit,
                          &old_stage);
          did_enter_cond= true;
          do
          {
            if (thd->check_killed() && !rgi->worker_error)
            {
              DEBUG_SYNC(thd, "rpl_parallel_start_waiting_for_prior_killed");
              thd->send_kill_message();
              slave_output_error_info(rgi->rli, thd);
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

        if ((tmp_gco= gco->prev_gco))
        {
          /*
            Now all the event groups in the previous batch have entered their
            commit phase, and will no longer access their gco. So we can free
            it here.
          */
          DBUG_ASSERT(!tmp_gco->prev_gco);
          gco->prev_gco= NULL;
          tmp_gco->next_gco= gcos_to_free;
          gcos_to_free= tmp_gco;
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
        else if (rgi->wait_commit_sub_id > entry->last_committed_sub_id)
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
        unlock_or_exit_cond(thd, &entry->LOCK_parallel_entry,
                            &did_enter_cond, &old_stage);

        if(thd->wait_for_commit_ptr)
        {
          /*
            This indicates that we get a new GTID event in the middle of
            a not completed event group. This is corrupt binlog (the master
            will never write such binlog), so it does not happen unless
            someone tries to inject wrong crafted binlog, but let us still
            try to handle it somewhat nicely.
          */
          rgi->cleanup_context(thd, true);
          thd->wait_for_commit_ptr->unregister_wait_for_prior_commit();
          thd->wait_for_commit_ptr->wakeup_subsequent_commits(rgi->worker_error);
        }
        thd->wait_for_commit_ptr= &rgi->commit_orderer;

        if (opt_gtid_ignore_duplicates)
        {
          int res=
            rpl_global_gtid_slave_state.check_duplicate_gtid(&rgi->current_gtid,
                                                             rgi);
          if (res < 0)
          {
            /* Error. */
            slave_output_error_info(rgi->rli, thd);
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

      group_ending= event_type == XID_EVENT ||
         (event_type == QUERY_EVENT &&
          (((Query_log_event *)events->ev)->is_commit() ||
           ((Query_log_event *)events->ev)->is_rollback()));
      if (group_ending)
      {
        DEBUG_SYNC(thd, "rpl_parallel_before_mark_start_commit");
        rgi->mark_start_commit();
      }

      /*
        If the SQL thread is stopping, we just skip execution of all the
        following event groups. We still do all the normal waiting and wakeup
        processing between the event groups as a simple way to ensure that
        everything is stopped and cleaned up correctly.
      */
      if (!rgi->worker_error && !skip_event_group)
        err= rpt_handle_event(events, rpt);
      else
        err= thd->wait_for_prior_commit();

      end_of_group=
        in_event_group &&
        ((group_standalone && !Log_event::is_part_of_group(event_type)) ||
         group_ending);

      delete_or_keep_event_post_apply(rgi, event_type, events->ev);
      events->next= qevs_to_free;
      qevs_to_free= events;

      if (unlikely(err) && !rgi->worker_error)
      {
        slave_output_error_info(rgi->rli, thd);
        signal_error_to_sql_driver_thread(thd, rgi, err);
      }
      if (end_of_group)
      {
        in_event_group= false;
        finish_event_group(thd, event_gtid_sub_id, entry, rgi);
        rgi->next= rgis_to_free;
        rgis_to_free= rgi;
        group_rgi= rgi= NULL;
        skip_event_group= false;
        DEBUG_SYNC(thd, "rpl_parallel_end_of_group");
      }

      events= next;
    }

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    /* Signal that our queue can now accept more events. */
    rpt->dequeue2(total_event_size);
    mysql_cond_signal(&rpt->COND_rpl_thread_queue);
    /* We need to delay the free here, to when we have the lock. */
    while (gcos_to_free)
    {
      group_commit_orderer *next= gcos_to_free->next_gco;
      rpt->free_gco(gcos_to_free);
      gcos_to_free= next;
    }
    while (rgis_to_free)
    {
      rpl_group_info *next= rgis_to_free->next;
      rpt->free_rgi(rgis_to_free);
      rgis_to_free= next;
    }
    while (qevs_to_free)
    {
      rpl_parallel_thread::queued_event *next= qevs_to_free->next;
      rpt->free_qev(qevs_to_free);
      qevs_to_free= next;
    }

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
      finish_event_group(thd, group_rgi->gtid_sub_id,
                         group_rgi->parallel_entry, group_rgi);
      in_event_group= false;
      mysql_mutex_lock(&rpt->LOCK_rpl_thread);
      rpt->free_rgi(group_rgi);
      group_rgi= NULL;
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
  DBUG_ASSERT(!gco->prev_gco /* Must only free after dealloc previous */);
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


rpl_parallel_thread::queued_event *
rpl_parallel_thread::get_qev(Log_event *ev, ulonglong event_size,
                             Relay_log_info *rli)
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
  qev->ev= ev;
  qev->event_size= event_size;
  qev->next= NULL;
  strcpy(qev->event_relay_log_name, rli->event_relay_log_name);
  qev->event_relay_log_pos= rli->event_relay_log_pos;
  qev->future_event_relay_log_pos= rli->future_event_relay_log_pos;
  strcpy(qev->future_event_master_log_name, rli->future_event_master_log_name);
  return qev;
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
                             rpl_parallel_entry *e)
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

  return rgi;
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
rpl_parallel_thread::get_gco(uint64 wait_count, group_commit_orderer *prev)
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
  gco->installed= false;
  return gco;
}


void
rpl_parallel_thread::free_gco(group_commit_orderer *gco)
{
  mysql_mutex_assert_owner(&LOCK_rpl_thread);
  DBUG_ASSERT(!gco->prev_gco /* Must not free until wait has completed. */);
  gco->next_gco= gco_free_list;
  gco_free_list= gco;
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
rpl_parallel_entry::choose_thread(Relay_log_info *rli, bool *did_enter_cond,
                                  PSI_stage_info *old_stage, bool reuse)
{
  uint32 idx;
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
        slave_output_error_info(rli, rli->sql_driver_thd);
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
  if (e->current_gco)
    dealloc_gco(e->current_gco);
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
    uint32 domain_id;
    if (likely(typ == GTID_EVENT))
    {
      Gtid_log_event *gtid_ev= static_cast<Gtid_log_event *>(ev);
      domain_id= (rli->mi->using_gtid == Master_info::USE_GTID_NO ?
                  0 : gtid_ev->domain_id);
    }
    else
      domain_id= 0;
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
    e->choose_thread(rli, &did_enter_cond, &old_stage, typ != GTID_EVENT);
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

    if (!(rgi= cur_thread->get_rgi(rli, gtid_ev, e)))
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

      if (!(gco= cur_thread->get_gco(count, e->current_gco)))
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
      Queue an empty event, so that the position will be updated in a
      reasonable way relative to other events:

       - If the currently executing events are queued serially for a single
         thread, the position will only be updated when everything before has
         completed.

       - If we are executing multiple independent events in parallel, then at
         least the position will not be updated until one of them has reached
         the current point.
    */
    qev->ev= NULL;
  }
  else
  {
    qev->rgi= e->current_group_info;
  }

  /*
    Queue the event for processing.
  */
  rli->event_relay_log_pos= rli->future_event_relay_log_pos;
  cur_thread->enqueue(qev);
  unlock_or_exit_cond(rli->sql_driver_thd, &cur_thread->LOCK_rpl_thread,
                      &did_enter_cond, &old_stage);
  mysql_cond_signal(&cur_thread->COND_rpl_thread);

  return 0;
}
