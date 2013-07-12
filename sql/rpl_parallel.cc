#include "my_global.h"
#include "rpl_parallel.h"
#include "slave.h"
#include "rpl_mi.h"


/*
  Code for optional parallel execution of replicated events on the slave.

  ToDo list:

   - Review every field in Relay_log_info, and all code that accesses it.
     Split out the necessary parts into rpl_group_info, to avoid conflicts
     between parallel execution of events. (Such as deferred events ...)

   - Error handling. If we fail in one of multiple parallel executions, we
     need to make a best effort to complete prior transactions and roll back
     following transactions, so slave binlog position will be correct.
     And all the retry logic for temporary errors like deadlock.

   - Stopping the slave needs to handle stopping all parallel executions. And
     the logic in sql_slave_killed() that waits for current event group to
     complete needs to be extended appropriately...

   - Audit the use of Relay_log_info::data_lock. Make sure it is held
     correctly in all needed places also when using parallel replication.

   - We need some user-configurable limit on how far ahead the SQL thread will
     fetch and queue events for parallel execution (otherwise if slave gets
     behind we will fill up memory with pending malloc()'ed events).

   - Fix update of relay-log.info and master.info. In non-GTID replication,
     they must be serialised to preserve correctness. In GTID replication, we
     should not update them at all except at slave thread stop.

   - All the waits (eg. in struct wait_for_commit and in
     rpl_parallel_thread_pool::get_thread()) need to be killable. And on kill,
     everything needs to be correctly rolled back and stopped in all threads,
     to ensure a consistent slave replication state.

   - Handle the case of a partial event group. This occurs when the master
     crashes in the middle of writing the event group to the binlog. The
     slave rolls back the transaction; parallel execution needs to be able
     to deal with this wrt. commit_orderer and such.

   - Relay_log_info::is_in_group(). This needs to be handled correctly in all
     callers. I think it needs to be split into two, one version in
     Relay_log_info to be used from next_event() in slave.cc, one to be used in
     per-transaction stuff.

   - We should fail if we connect to the master with opt_slave_parallel_threads
     greater than zero and master does not support GTID. Just to avoid a bunch
     of potential problems, we won't be able to do any parallel replication
     in this case anyway.
*/

struct rpl_parallel_thread_pool global_rpl_thread_pool;


static void
rpt_handle_event(rpl_parallel_thread::queued_event *qev,
                 struct rpl_parallel_thread *rpt)
{
  int err;
  struct rpl_group_info *rgi= qev->rgi;
  Relay_log_info *rli= rgi->rli;
  THD *thd= rgi->thd;

  thd->rgi_slave= rgi;
  thd->rpl_filter = rli->mi->rpl_filter;
  /* ToDo: Get rid of rli->group_info, it is not thread safe. */
  rli->group_info= rgi;

  /* ToDo: Access to thd, and what about rli, split out a parallel part? */
  mysql_mutex_lock(&rli->data_lock);
  err= apply_event_and_update_pos(qev->ev, thd, rgi, rpt);
  /* ToDo: error handling. */
}


pthread_handler_t
handle_rpl_parallel_thread(void *arg)
{
  THD *thd;
  const char* old_msg;
  struct rpl_parallel_thread::queued_event *events;
  bool group_standalone= true;
  bool in_event_group= false;
  uint64 event_gtid_sub_id= 0;

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

  while (!rpt->stop && !thd->killed)
  {
    rpl_parallel_thread *list;

    old_msg= thd->proc_info;
    thd->enter_cond(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread,
                    "Waiting for work from SQL thread");
    while (!rpt->stop && !thd->killed && !(events= rpt->event_queue))
      mysql_cond_wait(&rpt->COND_rpl_thread, &rpt->LOCK_rpl_thread);
    rpt->free= false;
    rpt->event_queue= rpt->last_in_queue= NULL;
    thd->exit_cond(old_msg);

  more_events:
    while (events)
    {
      struct rpl_parallel_thread::queued_event *next= events->next;
      Log_event_type event_type= events->ev->get_type_code();
      rpl_group_info *rgi= events->rgi;
      rpl_parallel_entry *entry= rgi->parallel_entry;
      uint64 wait_for_sub_id;
      uint64 wait_start_sub_id;
      bool end_of_group;

      if (event_type == GTID_EVENT)
      {
        in_event_group= true;
        group_standalone=
          (0 != (static_cast<Gtid_log_event *>(events->ev)->flags2 &
                 Gtid_log_event::FL_STANDALONE));

        /* Save this, as it gets cleared once event group commits. */
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

        DBUG_ASSERT(!thd->wait_for_commit_ptr);
        thd->wait_for_commit_ptr= &rgi->commit_orderer;
      }

      rpt_handle_event(events, rpt);

      end_of_group=
        in_event_group &&
        ((group_standalone && !Log_event::is_part_of_group(event_type)) ||
         event_type == XID_EVENT ||
         (event_type == QUERY_EVENT &&
          (!strcmp("COMMIT", ((Query_log_event *)events->ev)->query) ||
           !strcmp("ROLLBACK", ((Query_log_event *)events->ev)->query))));

      /* ToDo: must use rgi here, not rli, for thread safety. */
      delete_or_keep_event_post_apply(rgi->rli, event_type, events->ev);
      my_free(events);

      if (end_of_group)
      {
        in_event_group= false;

        rgi->commit_orderer.unregister_wait_for_prior_commit();
        thd->wait_for_commit_ptr= NULL;

        /*
          Record that we have finished, so other event groups will no
          longer attempt to wait for us to commit.

          We can race here with the next transactions, but that is fine, as
          long as we check that we do not decrease last_committed_sub_id. If
          this commit is done, then any prior commits will also have been
          done and also no longer need waiting for.
        */
        mysql_mutex_lock(&entry->LOCK_parallel_entry);
        if (entry->last_committed_sub_id < event_gtid_sub_id)
        {
          entry->last_committed_sub_id= event_gtid_sub_id;
          mysql_cond_broadcast(&entry->COND_parallel_entry);
        }
        mysql_mutex_unlock(&entry->LOCK_parallel_entry);

        rgi->commit_orderer.wakeup_subsequent_commits();
        delete rgi;
      }

      events= next;
    }

    mysql_mutex_lock(&rpt->LOCK_rpl_thread);
    if ((events= rpt->event_queue) != NULL)
    {
      rpt->event_queue= rpt->last_in_queue= NULL;
      mysql_mutex_unlock(&rpt->LOCK_rpl_thread);
      goto more_events;
    }

    if (!in_event_group)
    {
      rpt->current_entry= NULL;
      if (!rpt->stop && !rpt->free)
      {
        mysql_mutex_lock(&rpt->pool->LOCK_rpl_thread_pool);
        list= rpt->pool->free_list;
        rpt->next= list;
        rpt->pool->free_list= rpt;
        if (!list)
          mysql_cond_broadcast(&rpt->pool->COND_rpl_thread_pool);
        mysql_mutex_unlock(&rpt->pool->LOCK_rpl_thread_pool);
        rpt->free= true;
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

  /*
    Allocate the new list of threads up-front.
    That way, if we fail half-way, we only need to free whatever we managed
    to allocate, and will not be left with a half-functional thread pool.
  */
  if (new_count &&
      !(new_list= (rpl_parallel_thread **)my_malloc(new_count*sizeof(*new_list),
                                                    MYF(MY_WME))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int(new_count*sizeof(*new_list))));
    goto err;;
  }

  for (i= 0; i < new_count; ++i)
  {
    pthread_t th;

    if (!(new_list[i]= (rpl_parallel_thread *)my_malloc(sizeof(*(new_list[i])),
                                                        MYF(MY_WME))))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), sizeof(*(new_list[i])));
      goto err;
    }
    new_list[i]->delay_start= true;
    new_list[i]->running= false;
    new_list[i]->stop= false;
    new_list[i]->free= false;
    mysql_mutex_init(key_LOCK_rpl_thread, &new_list[i]->LOCK_rpl_thread,
                     MY_MUTEX_INIT_SLOW);
    mysql_cond_init(key_COND_rpl_thread, &new_list[i]->COND_rpl_thread, NULL);
    new_list[i]->pool= pool;
    new_list[i]->current_entry= NULL;
    new_list[i]->event_queue= NULL;
    new_list[i]->last_in_queue= NULL;
    if (mysql_thread_create(key_rpl_parallel_thread, &th, NULL,
                            handle_rpl_parallel_thread, new_list[i]))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      my_free(new_list[i]);
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
    my_free(rpt);
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
      rpl_parallel_thread *next= new_free_list->next;
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
      my_free(new_free_list);
      new_free_list= next;
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


rpl_parallel::rpl_parallel() :
  current(NULL)
{
  my_hash_init(&domain_hash, &my_charset_bin, 32,
               offsetof(rpl_parallel_entry, domain_id), sizeof(uint32),
               NULL, NULL, HASH_UNIQUE);
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

  return e;
}


void
rpl_parallel::wait_for_done()
{
  struct rpl_parallel_entry *e;
  uint32 i;

  for (i= 0; i < domain_hash.records; ++i)
  {
    e= (struct rpl_parallel_entry *)my_hash_element(&domain_hash, i);
    mysql_mutex_lock(&e->LOCK_parallel_entry);
    while (e->current_sub_id > e->last_committed_sub_id)
      mysql_cond_wait(&e->COND_parallel_entry, &e->LOCK_parallel_entry);
    mysql_mutex_unlock(&e->LOCK_parallel_entry);
  }
}


bool
rpl_parallel::do_event(struct rpl_group_info *serial_rgi, Log_event *ev)
{
  rpl_parallel_entry *e;
  rpl_parallel_thread *cur_thread;
  rpl_parallel_thread::queued_event *qev;
  struct rpl_group_info *rgi;
  Relay_log_info *rli= serial_rgi->rli;
  enum Log_event_type typ;

  /* ToDo: what to do with this lock?!? */
  mysql_mutex_unlock(&rli->data_lock);

  if (!(qev= (rpl_parallel_thread::queued_event *)my_malloc(sizeof(*qev),
                                                            MYF(0))))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  qev->ev= ev;
  qev->next= NULL;

  if ((typ= ev->get_type_code()) == GTID_EVENT)
  {
    Gtid_log_event *gtid_ev= static_cast<Gtid_log_event *>(ev);

    if (!(e= find(gtid_ev->domain_id)) ||
        !(rgi= new rpl_group_info(rli)) ||
        event_group_new_gtid(rgi, gtid_ev))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
      return true;
    }
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
      /* Check if we already have a worker thread for this entry. */
      cur_thread= e->rpl_thread;
      if (cur_thread)
      {
        mysql_mutex_lock(&cur_thread->LOCK_rpl_thread);
        if (cur_thread->current_entry != e)
        {
          /* Not ours anymore, we need to grab a new one. */
          mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);
          e->rpl_thread= cur_thread= NULL;
        }
      }

      if (!cur_thread)
      {
        /*
          Nothing else is currently running in this domain. We can spawn a new
          thread to do this event group in parallel with anything else that might
          be running in other domains.
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

    e->current_group_info= rgi;
    e->current_sub_id= rgi->gtid_sub_id;
    current= rgi->parallel_entry= e;
  }
  else if (!Log_event::is_group_event(typ) || !current)
  {
    /*
      Events like ROTATE and FORMAT_DESCRIPTION. Do not run in worker thread.
      Same for events not preceeded by GTID (we should not see those normally,
      but they might be from an old master).
    */
    qev->rgi= serial_rgi;
    rpt_handle_event(qev, NULL);
    delete_or_keep_event_post_apply(rli, typ, qev->ev);

    return false;
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
  }
  qev->rgi= current->current_group_info;

  /*
    Queue the event for processing.
  */
  if (cur_thread->last_in_queue)
    cur_thread->last_in_queue->next= qev;
  else
    cur_thread->event_queue= qev;
  cur_thread->last_in_queue= qev;
  mysql_cond_signal(&cur_thread->COND_rpl_thread);
  mysql_mutex_unlock(&cur_thread->LOCK_rpl_thread);

  return false;
}
