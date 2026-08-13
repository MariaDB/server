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

  The parallel worker threads: creating the team, the THD each worker runs
  under, the batch channel they ship rows to the manager through, relaying
  their diagnostics, and reaping them.

  What a worker actually runs -- the gate, the per-worker copy of the plan and
  the join over a worker's chunk -- is in sql_parallel_execution.cc.
*/


#include "mariadb.h"
#include "mysqld_error.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_select.h"
#include "sql_parallel_workers.h"
#include "debug_sync.h"
#include "transaction.h"

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_pwt;
static PSI_thread_info all_pwt_threads[]=
{
  { &key_thread_pwt, WORKER_NAME, PSI_FLAG_GLOBAL},
};

static PSI_mutex_key key_mutex_pwt_LOCK_thread,
                     key_mutex_pwt_LOCK_worker,
                     key_mutex_pwt_LOCK_data;
static PSI_mutex_info all_pwt_mutexes[]=
{
  { &key_mutex_pwt_LOCK_thread,      "pwt_manager::LOCK_pwt_thread",      0},
  { &key_mutex_pwt_LOCK_worker,      "pwt_worker::LOCK_worker",           0},
  { &key_mutex_pwt_LOCK_data,        "pwt_manager::LOCK_data",      0},
};

static PSI_cond_key key_COND_pwt_worker,
                    key_COND_pwt_data_avail, key_COND_pwt_data_space;
static PSI_cond_info all_pwt_conds[]=
{
  { &key_COND_pwt_worker,      "pwt_worker::COND_worker",                 0},
  { &key_COND_pwt_data_avail,  "pwt_manager::COND_data_avail",      0},
  { &key_COND_pwt_data_space,  "pwt_manager::COND_data_space",      0},
};

static PSI_memory_info all_pwt_memory[]=
{
  { &key_memory_pwt_queued_event,  "pwt_queued_event",          0},
  { &key_memory_pwt_error_message, "pwt_error_message",         0},
  { &key_memory_pwt_workers,       "pwt_manager::workers",      0},
  { &key_memory_pwt_db,            "pwt_worker::db",            0},
  { &key_memory_pwt_batch_rows,    "pwt_worker::batch.rows",    0},
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
    if (pwt_worker *worker= thd->parallel_worker)
    {
      /*
        A genuine error (not a warning) raised while the worker runs the query
        -- e.g. a WHERE/projection/join evaluation error -- must abort the whole
        query. Trip fatal_error so the worker stops producing and the manager
        aborts instead of sending a truncated result. The error text is still
        relayed to the manager below and surfaced from finalize.

        Exclude a killed worker's own ER_QUERY_INTERRUPTED: a KILL is reported
        separately via kill_signal (which also carries the kill type, so a
        KILL vs KILL QUERY of a worker maps to dropping the manager connection
        vs just its query). Tripping fatal_error here would make the manager
        take the generic-error path and lose that distinction.
      */
      if (*level == Sql_condition::WARN_LEVEL_ERROR && !thd->killed)
      {
        mysql_mutex_lock(&worker->manager->LOCK_data);
        worker->manager->fatal_error= true;
        mysql_cond_broadcast(&worker->manager->COND_data_avail);
        mysql_mutex_unlock(&worker->manager->LOCK_data);
      }
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
        return true;
      }
      mysql_mutex_lock(&worker->manager->LOCK_pwt_thread);
      worker->manager->record_event(event);
      mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
    }
    return true;                // no further processing in worker thread
  }

};


/**
  @brief
    Hand this worker's filled batch buffer to the manager (producer side).

  Marks batch.rows ready and blocks until the manager has drained it
  (clears batch.full) or asks the producers to stop. On return the buffer is
  the worker's again: either ready to refill, or to be abandoned.

  @return
    true   the consumer asked us to stop (stop scanning)
    false  the buffer was drained; refill it
*/

bool pwt_worker::handoff_batch()
{
  DBUG_ENTER("pwt_manager::handoff_batch");
  mysql_mutex_lock(&manager->LOCK_data);
  if (manager->drain.stop)
  {
    mysql_mutex_unlock(&manager->LOCK_data);
    DBUG_RETURN(true);
  }
  batch.full= true;
  mysql_cond_signal(&manager->COND_data_avail);          // wake the consumer
  while (batch.full && !manager->drain.stop)
  {
    mysql_cond_wait(&manager->COND_data_space, &manager->LOCK_data);
    DBUG_PRINT("info", ("worker wakes"));
  }
  bool stopped= manager->drain.stop;
  mysql_mutex_unlock(&manager->LOCK_data);
  DBUG_RETURN(stopped);
}


/**
  @brief
    Entry point for our worker threads, arg supplied by manager details what
    needs to be run
*/

static void *parallel_worker_thread_func(void *arg)
{
  DBUG_ENTER("parallel_worker_thread_func");
  pwt_worker *worker= (pwt_worker*) arg;
  PWT_error_handler error_handler;

  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(worker->thd);
  /*
    create_background_thd()'s THD(0) never ran the connection-setup path that
    allocates debug_sync_control, so DEBUG_SYNC actions on this worker (e.g. the
    pwt_worker_pause_before_signal sync point used by the tests) would assert on
    a NULL control block. Initialise it here, on the worker thread, so the
    MY_THREAD_SPECIFIC allocation is charged to this THD and freed symmetrically
    by ~THD (debug_sync_end_thread) when destroy_background_thd() runs below.
    No-op in non-DEBUG_SYNC builds and when debug_sync is inactive.
  */
#ifdef ENABLED_DEBUG_SYNC
  if (!worker->thd->debug_sync_control)
    debug_sync_init_thread(worker->thd);
#endif
  my_thread_set_name(worker->thd->connection_name.str);
  THD_STAGE_INFO(worker->thd, stage_sending_data);
  worker->thd->push_internal_handler(&error_handler);

  DBUG_EXECUTE_IF("pwt_error_to_queue_oom",
  {
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This is an example warning to show we can push a "
                 "warning from a worker thread to its manager ");
  });
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
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
  }
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);

  worker->execute_and_signal_manager();

  // manager needs to see this as atomic
  mysql_mutex_lock(&worker->LOCK_worker);
  /*
    LOCK_thd_kill is the canonical guard for thd->killed; a user-issued
    KILL on this worker's thread_id goes through THD::awake() which holds
    LOCK_thd_kill but not LOCK_worker, so we must nest both to get a
    race-free snapshot for the manager.
  */
  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  worker->state.killed= worker->thd->killed; // save this flag, THD is destroyed
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);
  worker->thd->pop_internal_handler();       // maybe not needed
  worker->state.finished= true;
  THD *thd= worker->thd;
  worker->thd= nullptr;
  mysql_mutex_unlock(&worker->LOCK_worker);

  /*
    Close our private table copies while we are still attached to our THD
    (current_thd == thd) and, crucially, before destroy_background_thd()
    tears down the THD's transaction: the engine handle's close frees state
    that references that transaction (InnoDB's prebuilt). The manager never
    touches a started worker's tables, so no lock is needed here.
  */
  worker->snapshot_table_stats();          // while the tables are still open
  worker->close_tables();

  /*
    Hand our status counters to the manager, which adds them to the session's
    own once every worker has been joined. Everything we counted was done for
    the user's statement, so it belongs in that session, and ~THD would
    otherwise put it straight into the global counters and nowhere else, leaving
    SHOW SESSION STATUS short by whatever the workers did. Clearing them here
    stops ~THD adding the same numbers to the global counters a second time,
    once the manager's session passes them on.

    Only the counters move. Memory accounting stays with this THD, because more
    of this THD's memory is freed after this point and ~THD has to reconcile the
    whole of it with the global counters -- clear_for_flush_status is the offset
    that leaves those fields alone, and the snapshot drops its copies of them.
  */
  worker->exec.stats= thd->status_var;
  worker->exec.stats.global_memory_used= 0;
  worker->exec.stats.tmp_space_used= 0;
  thd->set_status_var_init(clear_for_flush_status);

  /*
    executing thd_detach_thd sets my_thread_var to null, stopping our ability
    use the normal mutex mechanisms, so we operate this outside the locked
    region on a copy of our THD pointer
  */
  thd_detach_thd(save);
  server_threads.erase(thd);
  destroy_background_thd(thd);

  DBUG_RETURN(nullptr);
}


/**
  @brief
    Abort this worker, called as part of an error condition

  The worker may already be tearing itself down: parallel_worker_thread_func
  nulls worker->thd and destroys the THD under LOCK_worker. Take that lock
  and only awake() if the worker hasn't yet entered its exit section; if
  it has, the worker is on its way out and pthread_join will reap it.
*/

void pwt_worker::abort_worker()
{
  mysql_mutex_lock(&LOCK_worker);
  if (thd)
    thd->awake(ABORT_QUERY);
  mysql_mutex_unlock(&LOCK_worker);
  pthread_join(pthread, nullptr);
  mysql_mutex_destroy(&LOCK_worker);
  mysql_cond_destroy(&COND_worker);
}


/**
   @brief
     Free our message queue, discard the messages
*/

void pwt_manager::free_queue()
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
    my_free(event);
  }
  mysql_mutex_unlock(&LOCK_pwt_thread);
}


/*
  Snapshot the quick select's key intervals into the `ranges` array.
  An empty result means "scan the whole table".
  @return true on error (my_error() has been called)
*/
static bool parallel_build_key_ranges(JOIN_TAB *tab,
                                      Dynamic_array<KEY_MULTI_RANGE> *ranges)
{
  if (!tab->use_parallel_scan || !tab->select || !tab->select->quick)
    return false;
  QUICK_RANGE_SELECT *quick= (QUICK_RANGE_SELECT*) tab->select->quick;
  range_seq_t seq= quick_range_seq_init(quick, 0, 0);
  KEY_MULTI_RANGE range;
  while (!quick_range_seq_next(seq, &range))
  {
    if (ranges->append(range))
      return true;
  }
  return false;
}


/**
  @brief
    Initialise our parallel worker threads, setting their own new THD objects.
    Set up our mutexs for synchronization.
    Register our new threads in server_threads.

    Called from the management thread for applicable queries at the top level.
  @return
    HA_ERR_UNSUPPORTED  fall back to serial scan
    0                   success
    1                   failure
*/

int pwt_manager::init_parallel_workers(THD *thd, JOIN *join,
                                          JOIN_TAB *scan_tab)
{
  const uint n= thd->variables.parallel_worker_threads;
  if (n == 0)
    return HA_ERR_UNSUPPORTED;

  Dynamic_array<KEY_MULTI_RANGE> ranges(PSI_INSTRUMENT_MEM, 0, 16);
  if (parallel_build_key_ranges(scan_tab, &ranges))
    return 1;

  uint i= 0;
  TABLE *table= scan_tab->table;
  handler *file= table->file;
  this->exec.join= join;
  this->thd= thd;
  this->exec.scan_tab= scan_tab;

  // Initialize engine's parallel scan coordinator, ranges copied if reqd.
  int err= file->parallel_init_coordinator(n, ranges);
  if (err == HA_ERR_UNSUPPORTED)
  {
    // Signal to fall back to the serial record reader
    return err;
  }
  else if (err)
  {
    // Real error from the engine
    file->print_error(err, MYF(0));
    return err;
  }

  workers= (pwt_worker *) my_malloc(key_memory_pwt_workers,
                                    n * sizeof(pwt_worker),
                                    MYF(MY_WME | MY_ZEROFILL));
  if (!workers)
  {
    file->parallel_end_coordinator();
    return HA_ERR_UNSUPPORTED;
  }

  mysql_mutex_init(key_mutex_pwt_LOCK_thread, &LOCK_pwt_thread,
                    MY_MUTEX_INIT_SLOW);

  /*
    Set up the streaming channel before any worker starts: a worker's first
    action is to hand off a batch through handoff_batch(), which needs
    LOCK_data and the conds live. active_workers must already equal n so the
    consumer does not mistake "not started yet" for EOF.
  */
  mysql_mutex_init(key_mutex_pwt_LOCK_data, &LOCK_data, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_pwt_data_avail, &COND_data_avail, nullptr);
  mysql_cond_init(key_COND_pwt_data_space, &COND_data_space, nullptr);
  drain.active_workers= nworkers= n;

  /*
    The non-const join tables in join order (exec.jointabs[0] == scan_tab). These
    plus each worker's table copies form the manager->worker table map used to
    rebind the cloned conditions/refs/select list. No semijoin bushes here (the
    gate excludes them), so the tabs are simply join_tab[const_tables ..].
  */
  exec.n_tables= join->table_count - join->const_tables;
  if (!(exec.jointabs= thd->alloc<JOIN_TAB*>(exec.n_tables)) ||
      !(exec.tables= thd->alloc<TABLE*>(exec.n_tables)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) (exec.n_tables * sizeof(void*)));
    goto cleanup_old_workers;
  }
  for (uint t= 0; t < exec.n_tables; t++)
  {
    exec.jointabs[t]= &join->join_tab[join->const_tables + t];
    exec.tables[t]= exec.jointabs[t]->table;
  }

  /*
    Build the result containers.

    What a worker ships is every base-table column the query reads, in table
    order. The manager copies each one back into the field it came from in its
    own table instances, so that after a row is drained the manager's records
    hold what a serial scan would have left there and the query's own items read
    it directly. Shipping the projected select list instead would mean every
    expression the manager evaluates had to be re-pointed at a shipped value,
    and re-pointing Items does not reach everything that reads a record --
    create_tmp_table() builds Copy_field pairs holding raw Field pointers into
    the base tables, which no Item indirection can redirect.

    result_defn holds clones of the shipped items, so the query's own items are
    never bound to a tmp field; it defines the columns, and the manager plus
    every worker create an identical-layout copy.
  */
  {
    for (uint t= 0; t < exec.n_tables; t++)
    {
      TABLE *tbl= exec.tables[t];
      for (Field **f= tbl->field; *f; f++)
      {
        if (!bitmap_is_set(tbl->read_set, (*f)->field_index))
          continue;
        Item *itf= new (thd->mem_root) Item_field(thd, *f);
        if (!itf || transport.ship_list.push_back(itf, thd->mem_root))
        {
          my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_field));
          goto cleanup_old_workers;
        }
      }
    }
    /*
      A query reading no column of any table still needs a row shape, because the
      transport measures its batches in record images. "SELECT COUNT(*)" is that
      query: it reads no column, and the count is the number of images that
      arrive.
    */
    if (transport.ship_list.is_empty())
    {
      Item *one= new (thd->mem_root) Item_int(thd, (longlong) 1, 1);
      if (!one || transport.ship_list.push_back(one, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_int));
        goto cleanup_old_workers;
      }
    }
    if (!(transport.copy_back=
            new (thd->mem_root) Copy_field[transport.ship_list.elements]))
    {
      my_error(ER_OUTOFMEMORY, MYF(0),
               (int) (transport.ship_list.elements * sizeof(Copy_field)));
      goto cleanup_old_workers;
    }
    List_iterator_fast<Item> li(transport.ship_list);
    Item *sel_item;
    while ((sel_item= li++))
    {
      Item *c= sel_item->deep_copy_with_checks(thd);
      if (!c || transport.result_defn.push_back(c, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item));
        goto cleanup_old_workers;
      }
    }
  }
  transport.result_tmp_param= new (thd->mem_root) TMP_TABLE_PARAM;
  if (!transport.result_tmp_param)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(TMP_TABLE_PARAM));
    goto cleanup_old_workers;
  }
  if (make_result_table(thd, transport.result_defn, &transport.result_table))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "init_parallel_workers: failed to build the result table");
    goto cleanup_old_workers;
  }
  // result-row image size
  drain.reclength= transport.result_table->s->reclength;

  /*
    Pair each column of result_table with the base-table field it was projected
    from, so that draining a row is a copy per column back into the manager's own
    records. Position i of ship_list is column i of result_table, which is how
    make_result_table() built it. The one item that is not an Item_field is the
    filler shipped for a query that reads no column, and it has nowhere to go
    back to.
  */
  {
    List_iterator_fast<Item> si(transport.ship_list);
    Item *it;
    transport.n_copy_back= 0;
    for (uint i= 0; (it= si++); i++)
      if (it->type() == Item::FIELD_ITEM)
        transport.copy_back[transport.n_copy_back++]
          .set(((Item_field*) it)->field, transport.result_table->field[i],
               false);
  }
  drain.cur_cursor= 0;
  fatal_error= false;
  drain.stop= false;
  reaped= false;
  drain.cur_worker= nullptr;
  kill_signal= NOT_KILLED;

  for (i= 0; i < n; i++)
  {
    workers[i].exec.handler_ctx= file->parallel_get_worker_context(i);
    DBUG_ASSERT(workers[i].exec.handler_ctx);

    workers[i].thd= create_background_thd();
    if (!workers[i].thd)
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
              "init_parallel_workers: failed to create worker thread THD");
      goto cleanup_old_workers;
    }

    workers[i].manager= this;
    mysql_mutex_init(key_mutex_pwt_LOCK_worker, &workers[i].LOCK_worker,
                      MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_pwt_worker, &workers[i].COND_worker, nullptr);
    workers[i].thd->system_thread= SYSTEM_THREAD_GENERIC;
    size_t len= my_snprintf(workers[i].info.conn_name, MAX_THREAD_NAME,
                            WORKER_NAME);
    workers[i].thd->connection_name.str= workers[i].info.conn_name;
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
        my_error(ER_INTERNAL_ERROR, MYF(0),
                "init_parallel_workers: failed to allocate database name");
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
    /*
      A worker evaluates this session's expressions, so it is running this
      session's query and has to say so. Items that hold a value for the
      duration of one statement keep it against the query id they computed it
      under: Item_func_curtime, and the NOW/SYSDATE family with it, recomputes
      only when thd->query_id has moved past its own last_query_id. A
      background THD starts at query id 0, which is also the value that field
      is born with, so a copy that had not been evaluated before it was cloned
      looked up to date to the worker and handed out its uninitialized
      MYSQL_TIME -- what the is_valid_value_slow() assertion in Time::Time()
      catches.
    */
    workers[i].thd->query_id= thd->query_id;
    workers[i].thd->thread_id= next_thread_id();
    my_snprintf(workers[i].info.process_list,
                sizeof(workers[i].info.process_list),
                WORKER_NAME " %u " CONNECTION_NAME_THREAD " %llu",
                i+1, thd->thread_id);
    workers[i].thd->query_string= CSET_STRING(workers[i].info.process_list,
                                          strlen(workers[i].info.process_list),
                                          workers[i].thd->query_charset());
    workers[i].thd->parallel_worker= workers+i;
    workers[i].state.finished= workers[i].state.joined= false;
    workers[i].state.killed= NOT_KILLED;
    workers[i].batch.full= false;
    workers[i].batch.count= 0;
    workers[i].batch.rows= (uchar*) my_malloc(key_memory_pwt_batch_rows,
                                              (size_t) PWT_CHUNK_ROWS*
                                                drain.reclength,
                                              MYF(MY_WME));
    if (!workers[i].batch.rows)
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to allocate worker row buffer");
      goto cleanup_db_string;
    }
    workers[i].thd->userstat_running= thd->userstat_running;

    /*
      Give this worker its own copy of every non-const join table, opened from
      the shared TABLE_SHARE (open_worker_tables); the driving table is
      exec.tables[0] / exec.scan_table. Self-cleans on failure, so on
      error we go to cleanup_db_string (the worker thd is not yet registered).
    */
    if (open_worker_tables(thd, workers + i))
      goto cleanup_db_string;

    server_threads.insert(workers[i].thd);  // +information_schema.processlist

    /*
      Set up how this worker joins the non-driving tables (access method,
      worker-bound ref clone, condition), its result container, and private
      clones of the WHERE condition + select list with field references rebound
      to this worker's table copies. At run time the worker scans the driving
      chunk, joins the inner tables, projects exec.proj into
      result_table and ships that record image.
    */
    if (setup_worker_join(thd, workers + i) ||
        setup_worker_jointabs(thd, workers + i) ||
        make_result_table(thd, transport.result_defn,
                          &workers[i].exec.result_table) ||
        clone_worker_exprs(thd, workers + i))
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to set up worker execution");
      goto cleanup_thread_create;
    }

    if (mysql_thread_create(key_thread_pwt, &workers[i].pthread, nullptr,
                            parallel_worker_thread_func, &workers[i]))
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to create worker thread");
      goto cleanup_thread_create;
    }
  }
  return 0;

cleanup_thread_create:
  server_threads.erase(workers[i].thd);
  workers[i].close_tables();

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
  /*
    A worker spawned before the failure may be blocked in handoff_batch()
    waiting for the manager to drain its batch. Release them (stop + broadcast)
    so abort_worker()'s join can complete.
  */
  mysql_mutex_lock(&LOCK_data);
  drain.stop= true;
  mysql_cond_broadcast(&COND_data_space);
  mysql_mutex_unlock(&LOCK_data);
  for (uint j= 0; j < i; j++)
    workers[j].abort_worker();
  free_queue();
  free_result_tables(thd);            // workers reaped; result tables now idle
  // free each worker's row buffer (NULL for those not yet allocated)
  for (uint j= 0; j < n; j++)
    my_free(workers[j].batch.rows);
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  mysql_mutex_destroy(&LOCK_pwt_thread);
  mysql_cond_destroy(&COND_data_avail);
  mysql_cond_destroy(&COND_data_space);
  mysql_mutex_destroy(&LOCK_data);
  file->parallel_end_coordinator();
  return 1;                           // reached only on failure
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

/*
  @brief
    Copy the next worker result-row image into dst (reclength bytes).

  @description
  Consumer side of the streaming channel. The manager drains one worker's
  buffer at a time (cur_worker), advancing cur_cursor through its batch.count
  result rows; when the buffer is exhausted it releases the worker to refill
  (clears batch.full, signals COND_data_space) and picks the next ready worker.
  Blocks when no worker batch is momentarily ready. Kill of a worker is
  propagated to the manager THD; a worker error (fatal_error) aborts.

  @returns
    0 = row produced into dst,
   -1 = end of data,
    1 = error (matching report_error()).
*/
int pwt_manager::drain_next_row(uchar *dst)
{
  DBUG_ENTER("pwt_manager::drain_next_row");
  const uint reclen= drain.reclength;
  struct timespec wait;
  wait.tv_nsec= 0;

  for (;;)
  {
    if (drain.cur_worker)                      // draining a worker's buffer
    {
      pwt_worker *w= drain.cur_worker;
      if (drain.cur_cursor < w->batch.count)
      {
        memcpy(dst, w->batch.rows + (size_t) drain.cur_cursor * reclen, reclen);
        drain.cur_cursor++;
        DBUG_RETURN(0);
      }
      // buffer drained; release the worker so it can refill
      mysql_mutex_lock(&LOCK_data);
      drain.cur_worker= nullptr;
      w->batch.full= false;                     // buffer is the worker's again
      mysql_cond_broadcast(&COND_data_space);   // wake it to refill
      mysql_mutex_unlock(&LOCK_data);
      // fall through and look for the next ready worker
    }

    // find the next worker whose buffer is filled and ready
    pwt_worker *next= nullptr;
    PSI_stage_info old_stage;
    mysql_mutex_lock(&LOCK_data);
    for (;;)
    {
      for (uint i= 0; i < nworkers; i++)
        if (workers[i].batch.full)
        {
          next= &workers[i];
          break;
        }
      if (next)
        break;
      /*
        A worker exited because it was killed: propagate the kill to the
        manager's own THD so the query aborts now with ER_QUERY_INTERRUPTED,
        before any result is sent.
      */
      if (kill_signal != NOT_KILLED && !thd->killed)
      {
        killed_state ks= kill_signal;
        mysql_mutex_unlock(&LOCK_data);
        mysql_mutex_lock(&thd->LOCK_thd_kill);
        thd->killed= ks;
        mysql_mutex_unlock(&thd->LOCK_thd_kill);
        DBUG_RETURN(1);
      }
      if (fatal_error)                            // a worker failed
      {
        mysql_mutex_unlock(&LOCK_data);
        DBUG_RETURN(1);
      }
      if (!drain.active_workers)              // all producers done, drained
      {
        mysql_mutex_unlock(&LOCK_data);
        DBUG_RETURN(-1);
      }
      if (thd->killed)
      {
        mysql_mutex_unlock(&LOCK_data);
        DBUG_RETURN(1);
      }
      // wait for a batch, a finishing worker, or a 1s tick to re-check killed.
      // ENTER_COND/EXIT_COND publish the "Reading data from parallel workers"
      // stage and register the cond so a KILL of the manager wakes it.
      wait.tv_sec= time(0) + 1;
      thd->ENTER_COND(&COND_data_avail, &LOCK_data,
                      &stage_reading_data_from_parallel_worker, &old_stage);
      mysql_cond_timedwait(&COND_data_avail, &LOCK_data, &wait);
      thd->EXIT_COND(&old_stage);                 // unlocks LOCK_data
      mysql_mutex_lock(&LOCK_data);       // re-lock for the next pass
    }
    drain.cur_worker= next;
    drain.cur_cursor= 0;                        // start of next's buffer
    mysql_mutex_unlock(&LOCK_data);
    // loop back to drain next->batch.rows
  }
}


/**
  @brief
    Stop the producers and pthread_join them.

    The workers read this join's source table (their private handler), so they
    must be reaped before JOIN::join_free()->cleanup() frees that table --
    otherwise a worker that has not yet observed the stop request dereferences a
    freed table->file. That is why this is called from join_free(), ahead of the
    table teardown, and again (idempotently, guarded by 'reaped') from finalize.

    On a normal completion the workers have already finished; on an early-out
    (LIMIT) or an abort (KILL/error) we ask them to stop here. A worker
    re-checks 'stop' at each batch hand-off, so it exits within at most one
    batch -- without raising ER_QUERY_INTERRUPTED, which matters for a normal
    early-out.
*/

void pwt_manager::quiesce_workers()
{
  DBUG_ENTER("pwt_manager::quiesce_workers");
  if (!workers || reaped)
    DBUG_VOID_RETURN;

  // the consumer may have stopped mid-batch; drop its position (no open scan)
  drain.cur_worker= nullptr;

  mysql_mutex_lock(&LOCK_data);
  drain.stop= true;
  mysql_cond_broadcast(&COND_data_space);
  mysql_mutex_unlock(&LOCK_data);

  for (uint i= 0; i < nworkers; i++)
  {
    pthread_join(workers[i].pthread, nullptr);
    mysql_mutex_destroy(&workers[i].LOCK_worker);
    mysql_cond_destroy(&workers[i].COND_worker);
  }
  /*
    The work the workers did was this session's work, so its statistics are the
    session's too. Each worker left them in its pwt_worker before its THD was
    destroyed, and every worker has now been joined, so this thread is the only
    one touching either side and no locking is needed.
  */
  for (uint i= 0; i < nworkers; i++)
    add_to_status(&thd->status_var, &workers[i].exec.stats);

  /*
    Give ANALYZE what the workers did. The manager never runs the driving table's
    read loop, so the JOIN_TAB trackers the optimizer left for ANALYZE to read
    stay at zero and the report says the table was never touched. The trackers
    and the handlers are the manager's, every worker has been joined, so this
    thread is the only one touching either side.
 
    Keep the split as well as the sum. Summing tells ANALYZE how much was read,
    which is what a serial plan reports too; the per-worker figures tell it how
    evenly the engine's chunks divided that between the workers, which only a
    parallel plan has to answer for. Allocated on the statement's mem_root,
    which outlives the ANALYZE output the tracker is read for.
  */
  for (uint t= 0; t < exec.n_tables; t++)
  {
    Table_access_tracker *tr= exec.jointabs[t]->tracker;
    if (!tr)
      continue;
    if ((tr->r_rows_per_worker= thd->calloc<ha_rows>(nworkers)))
      tr->n_workers= nworkers;
  }

  for (uint i= 0; i < nworkers; i++)
    for (uint t= 0; t < exec.n_tables; t++)
    {
      if (Table_access_tracker *tr= exec.jointabs[t]->tracker)
      {
        /*
          Not the driving table's: sub_select() counts one scan of it per worker
          and the report wants one between them, added below.
        */
        if (t)
          tr->r_scans+=           workers[i].exec.tab_stats[t].r_scans;
        tr->r_rows+=              workers[i].exec.tab_stats[t].r_rows;
        tr->r_rows_after_where+=
          workers[i].exec.tab_stats[t].r_rows_after_where;
        if (tr->r_rows_per_worker)
          tr->r_rows_per_worker[i]= workers[i].exec.tab_stats[t].r_rows;
      }
      if (ha_handler_stats *hs= exec.tables[t]->file->handler_stats)
        hs->add(&workers[i].exec.tab_hstats[t]);
    }
  /*
    The chunks are one scan of the driving table between them, so report one,
    which is what the serial plan reports and what makes r_rows per scan comparable.
  */
  if (exec.jointabs[0]->tracker)
    exec.jointabs[0]->tracker->r_scans++;
  reaped= true;
  DBUG_VOID_RETURN;
}


/**
  @brief
    Reap the workers (if not already) and tear the channel down.

    Called from JOIN::exec() once exec_inner() has finished. Worker errors and
    warnings collected by PWT_error_handler are surfaced here, after the join's
    own result has been produced.
*/

void pwt_manager::finalize_parallel_workers(THD *thd, JOIN *join)
{
  DBUG_ENTER("pwt_manager::finalize_parallel_workers");
  if (!workers)
    DBUG_VOID_RETURN;

  quiesce_workers();                  // stop + join (no-op if already reaped)
  exec.scan_tab->table->file->parallel_end_coordinator();
  /*
    Surface errors/warnings the workers queued via PWT_error_handler. A worker
    error that mattered to the result has already aborted the join during
    execution (fatal_error or a propagated kill), so thd is already in error by
    the time we get here; raising another error would trip the "can't overwrite
    status" assertion in the diagnostics area. So only raise a queued ERROR
    when thd is not already in error -- otherwise keep it as a warning. Plain
    warnings are always safe to add.
  */
  bool surface_drop;
  mysql_mutex_lock(&LOCK_pwt_thread);
  surface_drop= messages_dropped;
  messages_dropped= false;
  pwt_queued_event *event;
  while ((event= parallel_messages.get()))
  {
    if (pwt_error_message *err= event->error)
    {
      if (err->level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR &&
          !thd->is_error())
        my_message_sql(err->code, err->message, MYF(0));
      else
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, err->code,
                     err->message);
      my_free(err->message);
      my_free(err);
    }
    my_free(event);
  }
  mysql_mutex_unlock(&LOCK_pwt_thread);

  if (surface_drop)
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_OUTOFMEMORY,
                        "Parallel worker diagnostics were dropped due to "
                        "memory allocation failure");

  mysql_cond_destroy(&COND_data_avail);
  mysql_cond_destroy(&COND_data_space);
  mysql_mutex_destroy(&LOCK_data);
  mysql_mutex_destroy(&LOCK_pwt_thread);
  free_result_tables(thd);              // workers joined; result tables idle
  for (uint i= 0; i < nworkers; i++)    // workers are joined, buffers idle
    my_free(workers[i].batch.rows);
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  DBUG_VOID_RETURN;
}
