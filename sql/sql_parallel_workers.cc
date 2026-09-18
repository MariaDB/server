/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

/**
  @file

  Implementation of parallel worker threads (PWT) management and execution
  logic.

  Contains
        error_to_queue
                push an error message onto our queue to send to the manager
        PWT_error_handler
                intercept error and warnings, queue them to the manager
        parallel_worker_thread_func
                Entry point for our worker threads
        abort_worker
        pwt_management::free_queue
                helper for error conditions
        pwt_management::init_parallel_workers
                Initialise our parallel worker threads
        pwt_init_psi_keys
                initialize PSI keys
        pwt_management::join_parallel_workers
                process information from worker threads until they are finished
*/


#include "sql_parallel_workers.h"
#include "debug_sync.h"

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_pwt;
static PSI_thread_info all_pwt_threads[]=
{
  { &key_thread_pwt, WORKER_NAME, PSI_FLAG_GLOBAL},
};

static PSI_mutex_key key_mutex_pwt_LOCK_manager,
                     key_mutex_pwt_LOCK_thread,
                     key_mutex_pwt_LOCK_worker;
static PSI_mutex_info all_pwt_mutexes[]=
{
  { &key_mutex_pwt_LOCK_manager, "pwt_management::LOCK_pwt_manager", 0},
  { &key_mutex_pwt_LOCK_thread,  "pwt_management::LOCK_pwt_thread",  0},
  { &key_mutex_pwt_LOCK_worker,  "pwt_worker::LOCK_worker",          0},
};

static PSI_cond_key key_COND_pwt_new_message, key_COND_pwt_worker;
static PSI_cond_info all_pwt_conds[]=
{
  { &key_COND_pwt_new_message, "pwt_management::COND_pwt_new_message", 0},
  { &key_COND_pwt_worker,      "pwt_worker::COND_worker",              0},
};

static PSI_memory_info all_pwt_memory[]=
{
  { &key_memory_pwt_queued_event,  "pwt_queued_event",        0},
  { &key_memory_pwt_error_message, "pwt_error_message",       0},
  { &key_memory_pwt_workers,       "pwt_management::workers", 0},
  { &key_memory_pwt_db,            "pwt_worker::db",          0},
};
#endif /* HAVE_PSI_INTERFACE */


/**
  @brief
    push an error message onto our queue to send to the manager

  @return
    true      an error occurred
    false     error or warning is queued
*/

bool error_to_queue(THD *thd, pwt_queued_event **event, uint error,
                     Sql_condition::enum_warning_level level, const char *msg)
{
  DBUG_EXECUTE_IF("pwt_error_to_queue_oom",
                  { *event= nullptr; return true; });
  *event= (pwt_queued_event*) my_malloc(key_memory_pwt_queued_event,
                                        sizeof(pwt_queued_event),
                                        MYF(0));
  if (!*event)
    return true;
  (*event)->error= (pwt_error_message*) my_malloc(key_memory_pwt_error_message,
                                                  sizeof(pwt_error_message),
                                                  MYF(0));
  if (!(*event)->error)
  {
    my_free(*event);
    *event= nullptr;
    return true;
  }
  (*event)->data= nullptr;
  (*event)->error->level= level;
  if (level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR)
    (*event)->error->worker_errno= thd->killed_errno();
  else
    (*event)->error->worker_errno= 0;
  (*event)->error->code= error;
  (*event)->error->message= (char *) my_malloc(key_memory_pwt_error_message,
                                               strlen(msg)+1,
                                               MYF(0));
  if (!(*event)->error->message)
  {
    my_free((*event)->error);
    my_free(*event);
    *event= nullptr;
    return true;
  }
  strmake((*event)->error->message, msg, strlen(msg));
  return false;
}


class PWT_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sql_state,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    if (pwt_worker *worker= thd->pwt_worker_info)
    {
      pwt_queued_event *event;
      if (error_to_queue(thd, &event, sql_errno, *level, msg))
      {
        /*
          Couldn't allocate the queued event. The worker THD's diagnostics
          area is discarded when the worker exits, so flag the manager so it
          can surface a single ER_OUTOFMEMORY warning to the user instead of
          letting this condition vanish.
        */
        mysql_mutex_lock(&worker->manager->LOCK_pwt_thread);
        worker->manager->messages_dropped= true;
        mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
        mysql_mutex_lock(&worker->manager->LOCK_pwt_manager);
        mysql_cond_signal(&worker->manager->COND_pwt_new_message);
        mysql_mutex_unlock(&worker->manager->LOCK_pwt_manager);
        return true;
      }
      mysql_mutex_lock(&worker->manager->LOCK_pwt_thread);
      worker->manager->parallel_messages.push_back(event);
      mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
    }
    return true;                // no further processing in worker thread
  }

};


/**
  @brief
    Entry point for our worker threads, arg supplied by manager details what
    needs to be run
*/

static void *parallel_worker_thread_func(void *arg)
{
  pwt_worker *worker= (pwt_worker*) arg;
  struct timespec abs_timeout;
  PSI_stage_info old_stage;

  PWT_error_handler error_handler;
  abs_timeout.tv_nsec= 0;
  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(worker->thd);
  my_thread_set_name(worker->thd->connection_name.str);
  THD_STAGE_INFO(worker->thd, stage_sending_data);
  worker->thd->push_internal_handler(&error_handler);

  /*
    START: in lieu of work, wait 1 seconds, push out an error or a warning,
    wait another 1 seconds then exit
  */
  abs_timeout.tv_sec= time(0)+1;
  mysql_mutex_lock(&worker->LOCK_worker);
  worker->thd->ENTER_COND(&worker->COND_worker, &worker->LOCK_worker,
                          &stage_sending_data, &old_stage);
  mysql_cond_timedwait(&worker->COND_worker, &worker->LOCK_worker,
                       &abs_timeout);
  worker->thd->EXIT_COND(&old_stage);

#ifdef ENABLED_DEBUG_SYNC
  /*
    we can't sync on the managers or our THD, spin the whole thing about
    and use the global signal pool, NO_CLEAR_EVENT is needed because we have
    multiple workers and the wrong one will likely consume the signal.
  */
  DBUG_EXECUTE_IF("pwt_worker_pause_before_signal",
    DBUG_ASSERT(!debug_sync_set_action(worker->thd, STRING_WITH_LEN(
      "now SIGNAL pwt_worker_paused WAIT_FOR pwt_worker_continue NO_CLEAR_EVENT"
      ))););
#endif

  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  if (worker->thd->killed)
  {
    mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    goto worker_thread_exit;
  }
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);

  if (worker->parallel_scan_job)
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This is an example warning to show we can push a "
                 "warning from a worker thread to its manager ");
  else
    my_error(ER_ARGUMENT_OUT_OF_RANGE, MYF(0), "worker_busted_function()");

  // signal manager there is something in the queue,
  mysql_mutex_lock(&worker->manager->LOCK_pwt_manager);
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);
  mysql_mutex_unlock(&worker->manager->LOCK_pwt_manager);

  abs_timeout.tv_sec= time(0)+5;
  mysql_mutex_lock(&worker->LOCK_worker);
  worker->thd->ENTER_COND(&worker->COND_worker, &worker->LOCK_worker,
                          &stage_sending_data, &old_stage);
  mysql_cond_timedwait(&worker->COND_worker, &worker->LOCK_worker,
                       &abs_timeout);
  worker->thd->EXIT_COND(&old_stage);

  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  if (worker->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
  }
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);

  // END: in lieu of work

worker_thread_exit:

  // manager needs to see this as atomic
  mysql_mutex_lock(&worker->LOCK_worker);
  /*
    LOCK_thd_kill is the canonical guard for thd->killed; a user-issued
    KILL on this worker's thread_id goes through THD::awake() which holds
    LOCK_thd_kill but not LOCK_worker, so we must nest both to get a
    race-free snapshot for the manager.
    Lock order matches join_parallel_workers().
  */
  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  worker->killed= worker->thd->killed;       // save this flag, THD is destroyed
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);
  worker->thd->pop_internal_handler();       // maybe not needed
  worker->finished= true;
  THD *thd= worker->thd;
  worker->thd= nullptr;
  mysql_mutex_unlock(&worker->LOCK_worker);

  // signal manager again to wake up and end this thread
  mysql_mutex_lock(&worker->manager->LOCK_pwt_manager);
  mysql_cond_signal(&worker->manager->COND_pwt_new_message);
  mysql_mutex_unlock(&worker->manager->LOCK_pwt_manager);

  /*
    executing this sets my_thread_var to null, stopping our ability use
    the normal mutex mechanisms, so we operate this outside the locked region
    on a copy of our THD pointer
  */
  thd_detach_thd(save);
  server_threads.erase(thd);
  destroy_background_thd(thd);

  return nullptr;
}

/**
  @brief
    Abort this worker, called as part of an error condition

  The worker may already be tearing itself down: parallel_worker_thread_func
  nulls worker->thd and destroys the THD under LOCK_worker. Take that lock
  and only awake() if the worker hasn't yet entered its exit section; if
  it has, the worker is on its way out and pthread_join will reap it.
*/

void abort_worker(pwt_worker *worker)
{
  mysql_mutex_lock(&worker->LOCK_worker);
  if (worker->thd)
    worker->thd->awake(ABORT_QUERY);
  mysql_mutex_unlock(&worker->LOCK_worker);
  pthread_join(worker->pthread, nullptr);
  mysql_mutex_destroy(&worker->LOCK_worker);
  mysql_cond_destroy(&worker->COND_worker);
}


/**
   @brief
     Free our message queue, discard the messages
*/

void pwt_management::free_queue()
{
  // process queue
  if (!parallel_messages.head())
    return;

  mysql_mutex_lock(&LOCK_pwt_thread);
  pwt_queued_event *event;
  while ((event= parallel_messages.get()))
  {
    if (pwt_error_message *err= event->error)
    {
      my_free(err->message);
      my_free(err);
    }
    if (event->data)
    {
      // TODO: free associated
    }
    my_free(event);
  }
  mysql_mutex_unlock(&LOCK_pwt_thread);
}


/**
  @brief
    Initialise our parallel worker threads, setting their own new THD objects.
    Set up our mutexs for synchronization.
    Register our new threads in server_threads.

    Called from the management thread for applicable queries at the top level.
*/

bool pwt_management::init_parallel_workers(THD *thd)
{
  bool result= false;
  uint i= 0;

  if (const uint n= thd->variables.parallel_worker_threads)
  {
    workers= (pwt_worker *) my_malloc(key_memory_pwt_workers,
                                      n * sizeof(pwt_worker),
                                      MYF(MY_WME | MY_ZEROFILL));
    if (!workers)
      return true;

    nworkers= n;

    mysql_mutex_init(key_mutex_pwt_LOCK_manager, &LOCK_pwt_manager,
                     MY_MUTEX_INIT_SLOW);
    mysql_mutex_init(key_mutex_pwt_LOCK_thread, &LOCK_pwt_thread,
                     MY_MUTEX_INIT_SLOW);
    mysql_cond_init(key_COND_pwt_new_message, &COND_pwt_new_message, NULL);
    for (i= 0; i < n; i++)
    {
      workers[i].thd= create_background_thd();
      if (!workers[i].thd)
      {
        result= true;
        goto cleanup_old_workers;
      }

      workers[i].manager= this;
      mysql_mutex_init(key_mutex_pwt_LOCK_worker, &workers[i].LOCK_worker,
                       MY_MUTEX_INIT_FAST);
      mysql_cond_init(key_COND_pwt_worker, &workers[i].COND_worker, nullptr);
      workers[i].thd->system_thread= SYSTEM_THREAD_GENERIC;
      size_t len= my_snprintf(workers[i].conn_name, MAX_THREAD_NAME,
                              WORKER_NAME);
      workers[i].thd->connection_name.str= workers[i].conn_name;
      workers[i].thd->connection_name.length= len;
      workers[i].thd->security_ctx= thd->security_ctx;
      workers[i].thd->set_command(thd->get_command());
      if (thd->db.str)
      {
        // explicit call in ~THD/THD::free_connection()/my_free, so we do this
        workers[i].thd->db.str= (char*)my_malloc(key_memory_pwt_db,
                                                 thd->db.length+1,
                                                 MYF(0));
        if (!workers[i].thd->db.str)
        {
          result= true;
          goto cleanup_db_string;
        }

        strmake(const_cast<char*>(workers[i].thd->db.str), thd->db.str,
                thd->db.length);
        workers[i].thd->db.length= thd->db.length;
      }
      else
      {
        workers[i].thd->db.str= nullptr;
        workers[i].thd->db.length= 0;
      }
      workers[i].thd->start_utime= thd->start_utime;
      workers[i].thd->thread_id= next_thread_id();
      my_snprintf(workers[i].info, sizeof(workers[i].info),
                  WORKER_NAME " %u " CONNECTION_NAME_THREAD " %llu",
                  i+1, thd->thread_id);
      workers[i].thd->query_string= CSET_STRING(workers[i].info,
                                                strlen(workers[i].info),
                                                workers[i].thd->query_charset());
      workers[i].thd->pwt_worker_info= workers+i;
      workers[i].finished= workers[i].joined= false;
      workers[i].killed= NOT_KILLED;
      if ((i+1)%10)        // determines error or warning in a deterministic way
        workers[i].parallel_scan_job= (void*)0x1;
      server_threads.insert(workers[i].thd);  // +information_schema.processlist

      if (mysql_thread_create(key_thread_pwt, &workers[i].pthread, nullptr,
                              parallel_worker_thread_func, &workers[i]))
      {
        result= true;
        goto cleanup_thread_create;
      }
    }
    this->thd= thd;
    return result;
  }
  else
    return false;

cleanup_thread_create:
  server_threads.erase(workers[i].thd);

cleanup_db_string:
  /*
    destroy_background_thd() requires current_thd to be NULL because it
    re-attaches the background THD to this thread's TLS. We are running on
    the user's query thread (current_thd == manager thd), so save/null/
    restore around the call. Mirrors the create_background_thd() pattern.
  */
  {
    THD *save_thd= current_thd;
    set_current_thd(nullptr);
    destroy_background_thd(workers[i].thd);
    set_current_thd(save_thd);
  }
  mysql_mutex_destroy(&workers[i].LOCK_worker);
  mysql_cond_destroy(&workers[i].COND_worker);

cleanup_old_workers:
  for (uint j= 0; j < i; j++)
    abort_worker(workers+j);
  free_queue();
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  mysql_cond_destroy(&COND_pwt_new_message);
  mysql_mutex_destroy(&LOCK_pwt_manager);
  mysql_mutex_destroy(&LOCK_pwt_thread);

  return result;
}

#ifdef HAVE_PSI_INTERFACE
void pwt_init_psi_keys(void)
{
  const char *category= "sql";
  int count;
  count= array_elements(all_pwt_threads);
  PSI_server->register_thread(category, all_pwt_threads, count);
  count= array_elements(all_pwt_mutexes);
  mysql_mutex_register(category, all_pwt_mutexes, count);
  count= array_elements(all_pwt_conds);
  mysql_cond_register(category, all_pwt_conds, count);
  count= array_elements(all_pwt_memory);
  mysql_memory_register(category, all_pwt_memory, count);
}
#endif


/**
  @brief
    Process data {errors, warnings, data, signals} from the workers.

    Currently this is called in the main thread after JOIN::exec_inner, but
    this will need to be disassembled and integrated into the above (or vice
    versa).
*/

void pwt_management::join_parallel_workers(THD *thd)
{
  bool all_done= false, workers_killed= false;
  PSI_stage_info old_stage;
  struct timespec wait_max;
  wait_max.tv_nsec= 0;
  int killed_from= -1;

  while (!all_done)
  {
    wait_max.tv_sec= time(0)+1;                                    // wait 1s
    mysql_mutex_lock(&LOCK_pwt_manager);
    thd->ENTER_COND(&COND_pwt_new_message, &LOCK_pwt_manager,
                    &stage_reading_data_from_parallel_worker, &old_stage);
    mysql_cond_timedwait(&COND_pwt_new_message, &LOCK_pwt_manager, &wait_max);
    thd->EXIT_COND(&old_stage);

    all_done= true;

    // delete worker threads that are finished
    for (uint i= 0; i < nworkers; i++)
    {
      if (workers[i].joined)                                  // already done
        continue;
      mysql_mutex_lock(&workers[i].LOCK_worker);
      if (workers[i].finished)
      {
        mysql_mutex_unlock(&workers[i].LOCK_worker);
        if (workers[i].killed)
        {
          killed_from= i;
          thd->awake(workers[i].killed);
        }
        pthread_join(workers[i].pthread, nullptr);
        mysql_mutex_destroy(&workers[i].LOCK_worker);
        mysql_cond_destroy(&workers[i].COND_worker);
        workers[i].joined= true;
      }
      else
      {
        mysql_mutex_unlock(&workers[i].LOCK_worker);
        all_done= false;
      }
    }

    if (thd->killed && !workers_killed)
    {
      // inform our workers that they are killed
      for (uint i= 0; i < nworkers; i++)
      {
        if (workers[i].joined)
          continue;
        mysql_mutex_lock(&workers[i].LOCK_worker);
        if (workers[i].finished)
        {
          mysql_mutex_unlock(&workers[i].LOCK_worker);
          continue;
        }

        if ((int)i != killed_from)
        {
          mysql_mutex_lock(&workers[i].thd->LOCK_thd_kill);
          workers[i].thd->killed= thd->killed;
          mysql_mutex_unlock(&workers[i].thd->LOCK_thd_kill);
          mysql_cond_signal(&workers[i].COND_worker);
        }
        mysql_mutex_unlock(&workers[i].LOCK_worker);
      }
      workers_killed= true;
    }
    else
    {
      // process queue
      bool surface_drop;
      mysql_mutex_lock(&LOCK_pwt_thread);
      surface_drop= messages_dropped;
      messages_dropped= false;
      pwt_queued_event *event;
      while ((event= parallel_messages.get()))
      {
        if (pwt_error_message *err= event->error)
        {
          /*
            set_overwrite_status to capture a message in our worker THD
            TODO, look at getting rid of this if we can
          */
          // thd->get_stmt_da()->set_overwrite_status(true);
          if (err->level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR)
            my_message_sql(err->code, err->message, MYF(0));
          else
            push_warning(thd, err->level, err->code, err->message);

          my_free(err->message);
          my_free(err);
        }
        if (event->data)
        {
          // process data from our worker thread
        }
        my_free(event);
      }
      mysql_mutex_unlock(&LOCK_pwt_thread);

      if (surface_drop)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_OUTOFMEMORY,
                            "Parallel worker diagnostics were dropped due "
                            "to memory allocation failure");
    }
  }

  if (nworkers)
  {
    mysql_cond_destroy(&COND_pwt_new_message);
    mysql_mutex_destroy(&LOCK_pwt_manager);
    mysql_mutex_destroy(&LOCK_pwt_thread);
    my_free(workers);
    workers= nullptr;
  }
}
