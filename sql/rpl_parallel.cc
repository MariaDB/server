#include "my_global.h"
#include "rpl_parallel.h"
#include "slave.h"
#include "rpl_mi.h"


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


static int
rpt_handle_event(rpl_parallel_thread::queued_event *qev,
                 struct rpl_parallel_thread *rpt)
{
  int err __attribute__((unused));
  rpl_group_info *rgi= qev->rgi;
  Relay_log_info *rli= rgi->rli;
  THD *thd= rgi->thd;

  thd->rgi_slave= rgi;
  thd->rpl_filter = rli->mi->rpl_filter;

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


static bool
sql_worker_killed(THD *thd, rpl_group_info *rgi, bool in_event_group)
{
  if (!rgi->rli->abort_slave && !abort_loop)
    return false;

  /*
    Do not abort in the middle of an event group that cannot be rolled back.
  */
  if ((thd->transaction.all.modified_non_trans_table ||
       (thd->variables.option_bits & OPTION_KEEP_LOG))
      && in_event_group)
    return false;
  /* ToDo: should we add some timeout like in sql_slave_killed?
       if (rgi->last_event_start_time == 0)
         rgi->last_event_start_time= my_time(0);
  */

  return true;
}


static void
finish_event_group(THD *thd, int err, uint64 sub_id,
                   rpl_parallel_entry *entry, wait_for_commit *wfc)
{
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
  if (err)
    wfc->unregister_wait_for_prior_commit();
  else
    wfc->wait_for_prior_commit();
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
  {
    entry->last_committed_sub_id= sub_id;
    mysql_cond_broadcast(&entry->COND_parallel_entry);
  }
  mysql_mutex_unlock(&entry->LOCK_parallel_entry);

  wfc->wakeup_subsequent_commits(err);
}


pthread_handler_t
handle_rpl_parallel_thread(void *arg)
{
  THD *thd;
  const char* old_msg;
  struct rpl_parallel_thread::queued_event *events;
  bool group_standalone= true;
  bool in_event_group= false;
  rpl_group_info *group_rgi= NULL;
  uint64 event_gtid_sub_id= 0;
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
  thd_proc_info(thd, "Waiting for work from main SQL threads");
  thd->set_time();
  thd->variables.lock_wait_timeout= LONG_TIMEOUT;

  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->thd= thd;

  while (rpt->delay_start)
    mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);

  rpt->running= true;
  mysql_cond_signal(&rpt->COND_rpl_thread);

  while (!rpt->stop && !thd->killed)
  {
    rpl_parallel_thread *list;

    old_msg= thd->proc_info;
    thd->enter_cond(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread,
                    "Waiting for work from SQL thread");
    while (!(events= rpt->event_queue) && !rpt->stop && !thd->killed &&
           !(rpt->current_entry && rpt->current_entry->force_abort))
      mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);
    rpt->dequeue(events);
    thd->exit_cond(old_msg);
    mysql_cond_signal(&rpt->COND_rpl_thread);

  more_events:
    while (events)
    {
      struct rpl_parallel_thread::queued_event *next= events->next;
      Log_event_type event_type;
      rpl_group_info *rgi= events->rgi;
      rpl_parallel_entry *entry= rgi->parallel_entry;
      uint64 wait_for_sub_id;
      uint64 wait_start_sub_id;
      bool end_of_group;

      if (!events->ev)
      {
        handle_queued_pos_update(thd, events);
        my_free(events);
        events= next;
        continue;
      }

      err= 0;
      group_rgi= rgi;
      /* Handle a new event group, which will be initiated by a GTID event. */
      if ((event_type= events->ev->get_type_code()) == GTID_EVENT)
      {
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
          prior groups have committed that are not safe to run in parallel with.
        */
        wait_for_sub_id= rgi->wait_commit_sub_id;
        wait_start_sub_id= rgi->wait_start_sub_id;
        if (wait_for_sub_id || wait_start_sub_id)
        {
          mysql_mutex_lock(&entry->LOCK_parallel_entry);
          if (wait_start_sub_id)
          {
            while (wait_start_sub_id > entry->last_committed_sub_id)
              mysql_cond_wait(&entry->COND_parallel_entry,
                              &entry->LOCK_parallel_entry);
          }
          rgi->wait_start_sub_id= 0;            /* No need to check again. */
          if (wait_for_sub_id > entry->last_committed_sub_id)
          {
            wait_for_commit *waitee=
              &rgi->wait_commit_group_info->commit_orderer;
            rgi->commit_orderer.register_wait_for_prior_commit(waitee);
          }
          mysql_mutex_unlock(&entry->LOCK_parallel_entry);
        }

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
          thd->wait_for_commit_ptr->wakeup_subsequent_commits(err);
        }
        thd->wait_for_commit_ptr= &rgi->commit_orderer;
      }

      /*
        If the SQL thread is stopping, we just skip execution of all the
        following event groups. We still do all the normal waiting and wakeup
        processing between the event groups as a simple way to ensure that
        everything is stopped and cleaned up correctly.
      */
      if (!rgi->is_error && !sql_worker_killed(thd, rgi, in_event_group))
        err= rpt_handle_event(events, rpt);
      else
        err= thd->wait_for_prior_commit();

      end_of_group=
        in_event_group &&
        ((group_standalone && !Log_event::is_part_of_group(event_type)) ||
         event_type == XID_EVENT ||
         (event_type == QUERY_EVENT &&
          (((Query_log_event *)events->ev)->is_commit() ||
           ((Query_log_event *)events->ev)->is_rollback())));

      delete_or_keep_event_post_apply(rgi, event_type, events->ev);
      my_free(events);

      if (err)
      {
        rgi->is_error= true;
        slave_output_error_info(rgi->rli, thd);
        rgi->cleanup_context(thd, true);
        rgi->rli->abort_slave= true;
      }
      if (end_of_group)
      {
        in_event_group= false;
        finish_event_group(thd, err, event_gtid_sub_id, entry,
                           &rgi->commit_orderer);
        delete rgi;
        group_rgi= rgi= NULL;
      }

      events= next;
    }

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    if ((events= rpt->event_queue) != NULL)
    {
      /*
        Take next group of events from the replication pool.
        This is faster than having to wakeup the pool manager thread to give us
        a new event.
      */
      rpt->dequeue(events);
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      mysql_cond_signal(&rpt->COND_rpl_thread);
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
      group_rgi->is_error= true;
      finish_event_group(thd, 1, group_rgi->gtid_sub_id,
                         group_rgi->parallel_entry, &group_rgi->commit_orderer);
      group_rgi->cleanup_context(thd, true);
      group_rgi->rli->abort_slave= true;
      in_event_group= false;
      delete group_rgi;
      group_rgi= NULL;
      mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    }
    if (!in_event_group)
    {
      rpt->current_entry= NULL;
      if (!rpt->stop)
      {
        mysql_mutex_lock(&rpt->pool->LOCK_rpl_thread_pool);
        list= rpt->pool->free_list;
        rpt->next= list;
        rpt->pool->free_list= rpt;
        if (!list)
          mysql_cond_broadcast(&rpt->pool->COND_rpl_thread_pool);
        mysql_mutex_unlock(&rpt->pool->LOCK_rpl_thread_pool);
      }
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
    new_list[i]->pool= pool;
    if (mysql_thread_create(key_rpl_parallel_thread, &th, NULL,
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
    rpl_parallel_thread *rpt= pool->get_thread(NULL);
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
rpl_parallel_thread_pool::get_thread(rpl_parallel_entry *entry)
{
  rpl_parallel_thread *rpt;

  mysql_mutex_lock(&LOCK_rpl_thread_pool);
  while ((rpt= free_list) == NULL)
    mysql_cond_wait(&COND_rpl_thread_pool, &LOCK_rpl_thread_pool);
  free_list= rpt->next;
  mysql_mutex_unlock(&LOCK_rpl_thread_pool);
  mysql_mutex_lock(&rpt->LOCK_rpl_thread);
  rpt->current_entry= entry;

  return rpt;
}


static void
free_rpl_parallel_entry(void *element)
{
  rpl_parallel_entry *e= (rpl_parallel_entry *)element;
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
    if (!(e= (struct rpl_parallel_entry *)my_malloc(sizeof(*e),
                                                    MYF(MY_ZEROFILL))))
      return NULL;
    e->domain_id= domain_id;
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
rpl_parallel::wait_for_done()
{
  struct rpl_parallel_entry *e;
  uint32 i;

  /*
    First signal all workers that they must force quit; no more events will
    be queued to complete any partial event groups executed.
  */
  for (i= 0; i < domain_hash.records; ++i)
  {
    rpl_parallel_thread *rpt;

    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    e->force_abort= true;
    if ((rpt= e->rpl_thread))
    {
      mysql_mutex_lock(&rpt->LOCK_rpl_thread);
      if (rpt->current_entry == e)
        mysql_cond_signal(&rpt->COND_rpl_thread);
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
    }
  }

  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    while (e->current_sub_id > e->last_committed_sub_id)
      mysql_cond_wait(&e->COND_parallel_entry, &e->LOCK_parallel_entry);
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
  }
}


/*
  do_event() is executed by the sql_driver_thd thread.
  It's main purpose is to find a thread that can execute the query.

  @retval false 	ok, event was accepted
  @retval true          error
*/

bool
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

  /* ToDo: what to do with this lock?!? */
  mysql_mutex_unlock(&rli->data_lock);

  /*
    Stop queueing additional event groups once the SQL thread is requested to
    stop.
  */
  if (((typ= ev->get_type_code()) == GTID_EVENT ||
       !(is_group_event= Log_event::is_group_event(typ))) &&
      rli->abort_slave)
    sql_thread_stopping= true;
  if (sql_thread_stopping)
  {
    /* QQ: Need a better comment why we return false here */
    return false;
  }

  if (!(qev= (rpl_parallel_thread::queued_event *)my_malloc(sizeof(*qev),
                                                            MYF(0))))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  qev->ev= ev;
  qev->event_size= event_size;
  qev->next= NULL;
  strcpy(qev->event_relay_log_name, rli->event_relay_log_name);
  qev->event_relay_log_pos= rli->event_relay_log_pos;
  qev->future_event_relay_log_pos= rli->future_event_relay_log_pos;
  strcpy(qev->future_event_master_log_name, rli->future_event_master_log_name);

  if (typ == GTID_EVENT)
  {
    Gtid_log_event *gtid_ev= static_cast<Gtid_log_event *>(ev);
    uint32 domain_id= (rli->mi->using_gtid == Master_info::USE_GTID_NO ?
                       0 : gtid_ev->domain_id);

    if (!(e= find(domain_id)) ||
        !(rgi= new rpl_group_info(rli)) ||
        event_group_new_gtid(rgi, gtid_ev))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
      delete rgi;
      return true;
    }
    rgi->is_parallel_exec = true;
    if ((rgi->deferred_events_collecting= rli->mi->rpl_filter->is_on()))
      rgi->deferred_events= new Deferred_log_events(rli);

    if ((gtid_ev->flags2 & Gtid_log_event::FL_GROUP_COMMIT_ID) &&
        e->last_commit_id == gtid_ev->commit_id)
    {
      /*
        We are already executing something else in this domain. But the two
        event groups were committed together in the same group commit on the
        master, so we can still do them in parallel here on the slave.

        However, the commit of this event must wait for the commit of the prior
        event, to preserve binlog commit order and visibility across all
        servers in the replication hierarchy.
      */
      rpl_parallel_thread *rpt= global_rpl_thread_pool.get_thread(e);
      rgi->wait_commit_sub_id= e->current_sub_id;
      rgi->wait_commit_group_info= e->current_group_info;
      rgi->wait_start_sub_id= e->prev_groupcommit_sub_id;
      e->rpl_thread= cur_thread= rpt;
      /* get_thread() returns with the LOCK_rpl_thread locked. */
    }
    else
    {
      /*
        Check if we already have a worker thread for this entry.

        We continue to queue more events up for the worker thread while it is
        still executing the first ones, to be able to start executing a large
        event group without having to wait for the end to be fetched from the
        master. And we continue to queue up more events after the first group,
        so that we can continue to process subsequent parts of the relay log in
        parallel without having to wait for previous long-running events to
        complete.

        But if the worker thread is idle at any point, it may return to the
        idle list or start servicing a different request. So check this, and
        allocate a new thread if the old one is no longer processing for us.
      */
      cur_thread= e->rpl_thread;
      if (cur_thread)
      {
        mysql_mutex_lock(&cur_thread->LOCK_rpl_thread);
        for (;;)
        {
          if (cur_thread->current_entry != e)
          {
            /*
              The worker thread became idle, and returned to the free list and
              possibly was allocated to a different request. This also means
              that everything previously queued has already been executed,
              else the worker thread would not have become idle. So we should
              allocate a new worker thread.
            */
            mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);
            e->rpl_thread= cur_thread= NULL;
            break;
          }
          else if (cur_thread->queued_size <= opt_slave_parallel_max_queued)
            break;                        // The thread is ready to queue into
          else
          {
            /*
              We have reached the limit of how much memory we are allowed to
              use for queuing events, so wait for the thread to consume some
              of its queue.
            */
            mysql_cond_wait(&cur_thread->COND_rpl_thread,
                            &cur_thread->LOCK_rpl_thread);
          }
        }
      }

      if (!cur_thread)
      {
        /*
          Nothing else is currently running in this domain. We can
          spawn a new thread to do this event group in parallel with
          anything else that might be running in other domains.
        */
        cur_thread= e->rpl_thread= global_rpl_thread_pool.get_thread(e);
        /* get_thread() returns with the LOCK_rpl_thread locked. */
      }
      else
      {
        /*
          We are still executing the previous event group for this replication
          domain, and we have to wait for that to finish before we can start on
          the next one. So just re-use the thread.
        */
      }

      rgi->wait_commit_sub_id= 0;
      rgi->wait_start_sub_id= 0;
      e->prev_groupcommit_sub_id= e->current_sub_id;
    }

    if (gtid_ev->flags2 & Gtid_log_event::FL_GROUP_COMMIT_ID)
    {
      e->last_server_id= gtid_ev->server_id;
      e->last_seq_no= gtid_ev->seq_no;
      e->last_commit_id= gtid_ev->commit_id;
    }
    else
    {
      e->last_server_id= 0;
      e->last_seq_no= 0;
      e->last_commit_id= 0;
    }

    qev->rgi= e->current_group_info= rgi;
    e->current_sub_id= rgi->gtid_sub_id;
    current= rgi->parallel_entry= e;
  }
  else if (!is_group_event || !current)
  {
    my_off_t log_pos;
    int err;
    bool tmp;
    /*
      Events like ROTATE and FORMAT_DESCRIPTION. Do not run in worker thread.
      Same for events not preceeded by GTID (we should not see those normally,
      but they might be from an old master).

      The varuable `current' is NULL for the case where the master did not
      have GTID, like a MariaDB 5.5 or MySQL master.
    */
    qev->rgi= serial_rgi;
    /* Handle master log name change, seen in Rotate_log_event. */
    if (typ == ROTATE_EVENT)
    {
      Rotate_log_event *rev= static_cast<Rotate_log_event *>(qev->ev);
      if ((rev->server_id != global_system_variables.server_id ||
           rli->replicate_same_server_id) &&
          !rev->is_relay_log_event() &&
          !rli->is_in_group())
      {
        memcpy(rli->future_event_master_log_name,
               rev->new_log_ident, rev->ident_len+1);
      }
    }

    tmp= serial_rgi->is_parallel_exec;
    serial_rgi->is_parallel_exec= true;
    err= rpt_handle_event(qev, NULL);
    serial_rgi->is_parallel_exec= tmp;
    log_pos= qev->ev->log_pos;
    delete_or_keep_event_post_apply(serial_rgi, typ, qev->ev);

    if (err)
    {
      my_free(qev);
      return true;
    }
    qev->ev= NULL;
    qev->future_event_master_log_pos= log_pos;
    if (!current)
    {
      rli->event_relay_log_pos= rli->future_event_relay_log_pos;
      handle_queued_pos_update(rli->sql_driver_thd, qev);
      my_free(qev);
      return false;
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
    cur_thread= current->rpl_thread;
    if (cur_thread)
    {
      mysql_mutex_lock(&cur_thread->LOCK_rpl_thread);
      if (cur_thread->current_entry != current)
      {
        /* Not ours anymore, we need to grab a new one. */
        mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);
        cur_thread= NULL;
      }
    }
    if (!cur_thread)
      cur_thread= current->rpl_thread=
        global_rpl_thread_pool.get_thread(current);
  }
  else
  {
    cur_thread= current->rpl_thread;
    if (cur_thread)
    {
      mysql_mutex_lock(&cur_thread->LOCK_rpl_thread);
      if (cur_thread->current_entry != current)
      {
        /* Not ours anymore, we need to grab a new one. */
        mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);
        cur_thread= NULL;
      }
    }
    if (!cur_thread)
    {
      cur_thread= current->rpl_thread=
        global_rpl_thread_pool.get_thread(current);
    }
    qev->rgi= current->current_group_info;
  }

  /*
    Queue the event for processing.
  */
  rli->event_relay_log_pos= rli->future_event_relay_log_pos;
  cur_thread->enqueue(qev);
  mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);
  mysql_cond_signal(&cur_thread->COND_rpl_thread);

  return false;
}
