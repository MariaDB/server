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
        worker_produce_chunks / worker_scan_table_to_manager
                producer: scan the source table and stream rows to the manager
                a batch at a time through the worker's reused batch table
        pwt_management::handoff_batch
                producer: hand the filled batch table to the manager and block
                until it is drained
        parallel_scan_read_first / parallel_scan_read_next
                consumer: feed the first join_tab from the worker batch tables
        pwt_management::finalize_parallel_workers
                stop the workers, reap them, surface diagnostics, tear down
*/


#include "sql_parallel_workers.h"
#include "sql_select.h"
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
  { &key_mutex_pwt_LOCK_thread,      "pwt_management::LOCK_pwt_thread",   0},
  { &key_mutex_pwt_LOCK_worker,      "pwt_worker::LOCK_worker",           0},
  { &key_mutex_pwt_LOCK_data,        "pwt_management::LOCK_data",         0},
};

static PSI_cond_key key_COND_pwt_data_avail, key_COND_pwt_data_space;
static PSI_cond_info all_pwt_conds[]=
{
  { &key_COND_pwt_data_avail,  "pwt_management::COND_data_avail",      0},
  { &key_COND_pwt_data_space,  "pwt_management::COND_data_space",      0},
};

static PSI_memory_info all_pwt_memory[]=
{
  { &key_memory_pwt_queued_event,  "pwt_queued_event",          0},
  { &key_memory_pwt_error_message, "pwt_error_message",         0},
  { &key_memory_pwt_workers,       "pwt_management::workers",   0},
  { &key_memory_pwt_db,            "pwt_worker::db",            0},
};
#endif /* HAVE_PSI_INTERFACE */


// consumer read function, installed on the first join_tab by init below
static int parallel_scan_read_first(JOIN_TAB *tab);


/**
  @brief
    push an error message onto our queue to send to the manager

  @return
    true      an error occurred
    false     error or warning is queued
*/

bool error_to_queue(pwt_queued_event **event, uint error,
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


/**
   @brief
   An instance of this class is used by our worker threads to capture and
   relay to the manager
*/

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
      if (error_to_queue(&event, sql_errno, *level, msg))
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
      worker->manager->parallel_messages.push_back(event);
      mysql_mutex_unlock(&worker->manager->LOCK_pwt_thread);
    }
    return true;                // no further processing in worker thread
  }

};


/*
  @description
  Copy one field's value from one row buffer to another, propagating the null
  state. field_conv() copies only the value bytes, so the null bit has to be
  transferred separately -- otherwise a NULL source would leave the
  destination non-null (showing stale data) and a non-null source would not
  clear a null bit left over from a previous row. set_null/set_notnull are
  safe no-ops when the field is declared NOT NULL.
*/
static inline void copy_field_with_null(Field *to, Field *from)
{
  if (from->is_null())
    to->set_null();
  else
  {
    to->set_notnull();
    field_conv(to, from);
  }
}


/**
  @brief
    Hand this worker's filled batch table to the manager (producer side).

  @description
  Marks batch_table ready and blocks until the manager has drained it
  (clears batch_full) or asks the producers to stop. On return the table is
  the worker's again: either empty and ready to refill, or to be abandoned.

  @return
    true   the consumer asked us to stop (stop scanning)
    false  the table was drained; refill it
*/

bool pwt_management::handoff_batch(pwt_worker *worker)
{
  mysql_mutex_lock(&LOCK_data);
  if (stop)
  {
    mysql_mutex_unlock(&LOCK_data);
    return true;
  }
  worker->batch_full= true;
  mysql_cond_signal(&COND_data_avail);          // wake the consumer
  while (worker->batch_full && !stop)
    mysql_cond_wait(&COND_data_space, &LOCK_data);
  bool stopped= stop;
  mysql_mutex_unlock(&LOCK_data);
  return stopped;
}


void inline close_worker_scan_table(pwt_worker *worker)
{
  if (worker->our_scan_table)
  {
    closefrm(worker->our_scan_table);
    my_free(worker->our_scan_table);
    worker->our_scan_table= nullptr;
  }
}

/**
  @brief
    Scan the worker's private source table and stream the rows to the manager,
    a batch at a time, through the worker's reused batch table.

  @description
  Each worker scans its own private copy of the first non-const join table
  (worker->our_scan_table, opened with in_use == this worker's thd) so the
  workers scan truly concurrently -- no shared-scan lock is needed. It copies
  up to PWT_CHUNK_ROWS rows field-by-field into batch_table (built in
  the source-table column format, so field_conv is needed rather than a raw
  record copy), hands that table to the manager, and blocks until the manager
  has drained it (handoff_batch). It then truncates the table and refills it
  for the next batch, until the source scan is exhausted.

  The worker only reproduces the source scan; it does not apply WHERE/JOIN
  conditions or run the rest of the join. The manager consumes these rows and
  drives the join itself as they arrive (see parallel_scan_read_next).

  @return
  0 on success, or the handler error code; HA_ERR_END_OF_FILE is mapped
  to success. A clean stop requested by the manager (handoff_batch -> stop)
  also returns success: the manager is done, not in error.
*/

static int worker_produce_chunks(pwt_worker *worker)
{
  TABLE *src= worker->our_scan_table;
  TABLE *dst= worker->batch_table;
  const uint nfields= src->s->fields;
  int err;

  src->use_all_columns();    // read every column into record[0]
  dst->use_all_columns();    // we write every column of the batch row

  /*
    Our handle bypassed lock_tables(), so take the engine-level read lock
    ourselves; InnoDB needs this to register the table with its trx before
    a scan. Paired with the F_UNLCK below.
  */
  if ((err= src->file->ha_external_lock(worker->thd, F_RDLCK)))
    return err;

  src->file->ha_index_or_rnd_end();      // in case a prior scan was open
  if ((err= src->file->ha_rnd_init_with_error(1)))
  {
    src->file->ha_external_lock(worker->thd, F_UNLCK);
    return err;
  }

  bool eof= false, killed= false;
  while (!eof && !killed)
  {
    /*
      The batch table is ours again (freshly created on the first pass, or
      drained by the manager on later passes). Take ownership and empty it.
    */
    dst->in_use= worker->thd;
    if ((err= dst->file->ha_delete_all_rows()))
      break;

    uint rows= 0;
    while (rows < PWT_CHUNK_ROWS)
    {
      // honour a direct KILL of this worker's thread
      mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
      killed= worker->thd->killed;
      mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);
      if (killed)
      {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        break;                          // stop now; do not hand off
      }

      if ((err= src->file->ha_rnd_next(src->record[0])))
      {
        if (err == HA_ERR_END_OF_FILE)
        {
          err= 0;
          eof= true;
        }
        break;
      }
      for (uint k= 0; k < nfields; k++)
        copy_field_with_null(dst->field[k], src->field[k]);
      if ((err= dst->file->ha_write_tmp_row(dst->record[0])))
        break;
      rows++;
    }

    if (err && err != HA_ERR_END_OF_FILE)
      break;                            // real error; do not hand off
    err= 0;

    if (rows && !killed)                // hand the filled batch to the manager
    {
      if (worker->manager->handoff_batch(worker))  // manager asked us to stop
        break;
    }
  }

  src->file->ha_rnd_end();
  src->file->ha_external_lock(worker->thd, F_UNLCK);
  return err;
}


/**
   @brief  Write rows to our manager, when done, tidy up.  Entry point for
           worker_produce_chunks.

   @return  true on error, false on success
*/

bool worker_scan_table_to_manager(pwt_worker *worker)
{
  pwt_management *mgr= worker->manager;
  int err= worker_produce_chunks(worker);

  /*
    End the worker's read transaction now, while we are still on the worker
    thread. destroy_background_thd() -> THD::cleanup() rolls the transaction
    back and asserts (trans_check) that the statement transaction is already
    empty, so we must close it out here. Any commit failure is captured by the
    installed PWT_error_handler.
  */
  trans_commit_stmt(worker->thd);
  trans_commit(worker->thd);

  /*
    Mark this producer done so the consumer can detect EOF, and wake it in
    case it is blocked waiting for data. A real engine error trips fatal_error
    so the consumer aborts the join instead of returning a truncated result.
    If this worker was killed (e.g. a user KILL aimed at it), record the kill
    so the consumer can propagate it to the manager's THD and abort the join
    with ER_QUERY_INTERRUPTED before any result is sent.
  */
  mysql_mutex_lock(&worker->thd->LOCK_thd_kill);
  killed_state killed= worker->thd->killed;
  mysql_mutex_unlock(&worker->thd->LOCK_thd_kill);

  mysql_mutex_lock(&mgr->LOCK_data);
  if (err)
    mgr->fatal_error= true;
  if (killed && mgr->kill_signal == NOT_KILLED)
    mgr->kill_signal= killed;
  mgr->active_workers--;
  mysql_cond_broadcast(&mgr->COND_data_avail);
  mysql_mutex_unlock(&mgr->LOCK_data);

  if (err)
  {
    worker->our_scan_table->file->print_error(err, MYF(0));
    return true;
  }
  return false;
}


/**
  @brief
    Entry point for our worker threads, arg supplied by manager details what
    needs to be run
*/

static void *parallel_worker_thread_func(void *arg)
{
  pwt_worker *worker= (pwt_worker*) arg;
  PWT_error_handler error_handler;

  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(worker->thd);
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

  worker_scan_table_to_manager(worker);

  /*
    Null worker->thd under LOCK_worker so abort_worker() -- which takes
    LOCK_worker before deciding whether to awake() -- sees either a live THD or
    nullptr, never a THD mid-teardown.
  */
  mysql_mutex_lock(&worker->LOCK_worker);
  worker->thd->pop_internal_handler();       // maybe not needed
  THD *thd= worker->thd;
  worker->thd= nullptr;
  mysql_mutex_unlock(&worker->LOCK_worker);

  /*
    Close our private source table while we are still attached to our THD
    (current_thd == thd) and, crucially, before destroy_background_thd()
    tears down the THD's transaction: the engine handle's close frees state
    that references that transaction (InnoDB's prebuilt). The manager never
    touches a started worker's our_scan_table, so no lock is needed here.
  */
  close_worker_scan_table(worker);

  /*
    executing thd_detach_thd sets my_thread_var to null, stopping our ability
    use the normal mutex mechanisms, so we operate this outside the locked
    region on a copy of our THD pointer
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

  @return
    false on success
    true on error
*/

bool pwt_management::init_parallel_workers(THD *thd, JOIN *join)
{
  bool result= false;
  uint i= 0;

  if (const uint n= thd->variables.parallel_worker_threads)
  {
    this->join= join;
    this->thd= thd;
    JOIN_TAB *tab= join->parallel_scan_join_tab;
    if (!tab)   // nothing to do
    {
      nworkers= 0;
      return false;
    }

    /*
      The parallel-scanned table is always the first non-const table: the gate
      in get_best_combination() only ever sets use_parallel_scan there. The
      workers reproduce a plain full table scan (ha_rnd_*) of it and feed the
      manager row images, but never position the real handler on a row. Fall
      back to a serial scan when that is unsafe:

        - select->quick: a range/quick access method was chosen (possibly after
          use_parallel_scan was set, e.g. a range built from the WHERE clause);
          a full scan would not honour it.

        - filesort: a sort tied to this table drives its own read of it, which
          the injected read functions do not feed.

        - keep_current_rowid: the plan needs this table's engine rowid via
          handler::position() -- e.g. DuplicateWeedout semijoin elimination or
          a rowid-ordered sort. Injected rows carry no valid handler position,
          so position() returns a stale/duplicate rowid for engines whose rowid
          is positional (MyISAM), and weedout then drops rows. (InnoDB derives
          the rowid from the PK in record[0], so it happens to survive, but we
          must not rely on that.)

      The batch tables already created for this query are left unused (freed at
      cleanup).
    */
    JOIN_TAB *scan_tab= join->join_tab + join->const_tables;
    if ((scan_tab->select && scan_tab->select->quick) ||
        scan_tab->filesort ||
        scan_tab->keep_current_rowid)
    {
      nworkers= 0;
      return false;
    }

    /*
      One batch table per worker was created up front by
      create_parallel_workers_tmp_tables; bail if that count is out of step
      with the worker count we are about to spawn.
    */
    if (n != tab->parallel_tmp_tables.elements())
      return true;

    workers= (pwt_worker *) my_malloc(key_memory_pwt_workers,
                                      n * sizeof(pwt_worker),
                                      MYF(MY_WME | MY_ZEROFILL));
    if (!workers)
      return true;

    nworkers= n;

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
    active_workers= n;
    fatal_error= false;
    stop= false;
    reaped= false;
    cur_worker= nullptr;
    kill_signal= NOT_KILLED;

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
      workers[i].batch_full= false;
      workers[i].batch_table= tab->parallel_tmp_tables.at(i);

      /*
        Give this worker its own TABLE+handler for the manager's first
        non-const source table, opened from the shared TABLE_SHARE.

        open_table_from_share() runs here on the manager thread, so the open
        must happen with in_use == current_thd: handler::ha_thd() asserts
        table->in_use == current_thd, and ha_innobase::open() calls it. So we
        open under the manager's thd, then repoint in_use at the worker.

        That is enough for engines that cache the THD: InnoDB sets m_user_thd
        lazily on the first operation (update_thd()), not at open time, so it
        binds to the worker when the worker scans on its own thread. Each
        worker thus gets a private handler and they scan concurrently without
        a shared-scan lock.

        Arguments to open_table_from_share() below:
          thd          open under the manager's THD (the in_use rule above);
                       in_use is repointed to the worker afterwards.
          src->s       the shared TABLE_SHARE of the first non-const source
                       table -- the worker's TABLE is built from it.
          &src->s->table_name
                       alias (name) for the opened TABLE.
          HA_OPEN_KEYFILE | HA_TRY_READ_ONLY
                       db_stat (handler open mode): open the index/key file,
                       read-only -- the worker only scans the table.
          EXTRA_RECORD
                       prgflag: allocate a second record buffer (record[1])
                       in addition to record[0], as for a normal table open.
          thd->open_options
                       ha_open_flags, the handler open options from the THD.
          st           outparam: the TABLE we just allocated, initialised here
                       as this worker's private table.
          false        is_create_table: this open is not part of CREATE TABLE.
          nullptr      partitions_to_open: open all partitions (no subset).
      */
      {
        TABLE *src= (join->join_tab + join->const_tables)->table;
        TABLE *st= (TABLE*) my_malloc(key_memory_TABLE, sizeof(TABLE),
                                      MYF(MY_WME | MY_ZEROFILL));
        if (!st)
        {
          result= true;
          goto cleanup_db_string;
        }
        if (open_table_from_share(thd, src->s, &src->s->table_name,
                                  HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                                  EXTRA_RECORD, thd->open_options, st,
                                  false, nullptr))
        {
          my_free(st);
          result= true;
          goto cleanup_db_string;
        }
        st->in_use= workers[i].thd;
        workers[i].our_scan_table= st;
      }

      server_threads.insert(workers[i].thd);  // +information_schema.processlist

      if (mysql_thread_create(key_thread_pwt, &workers[i].pthread, nullptr,
                              parallel_worker_thread_func, &workers[i]))
      {
        result= true;
        goto cleanup_thread_create;
      }
    }

    /*
      Feed the first join_tab from the channel instead of scanning the real
      first table. exec_inner() runs after this returns and drives the join
      through these read functions, consuming worker rows as they arrive.
    */
    {
      JOIN_TAB *first= join->join_tab + join->const_tables;
      first->table->reginfo.join_tab= first;
      first->read_first_record= parallel_scan_read_first;
    }
    return result;
  }
  else
    return false;

cleanup_thread_create:
  server_threads.erase(workers[i].thd);
  close_worker_scan_table(workers+i);

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

cleanup_old_workers:
  /*
    A worker spawned before the failure may be blocked in handoff_batch()
    waiting for the manager to drain its batch. Release them (stop + broadcast)
    so abort_worker()'s join can complete.
  */
  mysql_mutex_lock(&LOCK_data);
  stop= true;
  mysql_cond_broadcast(&COND_data_space);
  mysql_mutex_unlock(&LOCK_data);
  for (uint j= 0; j < i; j++)
    abort_worker(workers+j);
  free_queue();
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  mysql_mutex_destroy(&LOCK_pwt_thread);
  mysql_cond_destroy(&COND_data_avail);
  mysql_cond_destroy(&COND_data_space);
  mysql_mutex_destroy(&LOCK_data);

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


/*
  @brief
    function installed into the manager join execution to extract rows from
    the worker threads.

  @description
  Consumer side of the streaming channel. These pluggable read functions feed
  the first join_tab from the worker batch tables instead of scanning the real
  first table: each row a worker wrote into its batch table is copied
  field-by-field into the first table's record[0], so the manager's
  nested-loop join evaluates exactly as if it had scanned the table itself, but
  driven by the worker rows as they arrive. parallel_scan_read_first installs
  the read function and returns the first row; parallel_scan_read_next returns
  each subsequent row, blocking when no worker batch is momentarily ready.

  The manager drains one worker's batch table at a time (mgr->cur_worker): it
  rnd-scans that table, and on EOF releases the worker to refill (clears
  batch_full, signals COND_data_space) and picks the next ready worker.

  @returns
    0 = row produced,
   -1 = end of data
    1 = error (matching report_error()).
*/
static int parallel_scan_read_next(READ_RECORD *info)
{
  TABLE *dst= info->table;                       // real first table
  pwt_management *mgr= dst->reginfo.join_tab->join->parallel_work_manager;
  const uint nfields= dst->s->fields;
  struct timespec wait;
  wait.tv_nsec= 0;

  for (;;)
  {
    if (mgr->cur_worker)                          // draining a worker's batch
    {
      TABLE *bt= mgr->cur_worker->batch_table;
      int err= bt->file->ha_rnd_next(bt->record[0]);
      if (!err)
      {
        /*
          We are injecting a scanned row into the real first table's record[0].
          Some Field::store() paths (e.g. Field_bit_as_char) assert the field
          is marked in write_set, but a plain scanned table has an empty
          write_set. Mark all columns writable for the duration of the copy
          (debug-only; a no-op in release).
        */
        MY_BITMAP *old_map= dbug_tmp_use_all_columns(dst, &dst->write_set);
        for (uint k= 0; k < nfields; k++)
          copy_field_with_null(dst->field[k], bt->field[k]);
        dbug_tmp_restore_column_map(&dst->write_set, old_map);
        return 0;
      }
      // batch drained (EOF) or a read error; end the scan and release worker
      bt->file->ha_rnd_end();
      pwt_worker *w= mgr->cur_worker;
      mysql_mutex_lock(&mgr->LOCK_data);
      mgr->cur_worker= nullptr;
      w->batch_full= false;                       // table is the worker's again
      mysql_cond_broadcast(&mgr->COND_data_space); // wake it to refill
      mysql_mutex_unlock(&mgr->LOCK_data);
      if (err != HA_ERR_END_OF_FILE)
        return 1;
      // fall through and look for the next ready worker
    }

    // find the next worker whose batch table is filled and ready
    pwt_worker *next= nullptr;
    PSI_stage_info old_stage;
    mysql_mutex_lock(&mgr->LOCK_data);
    for (;;)
    {
      for (uint i= 0; i < mgr->nworkers; i++)
        if (mgr->workers[i].batch_full)
        {
          next= &mgr->workers[i];
          break;
        }
      if (next)
        break;
      /*
        A worker exited because it was killed: propagate the kill to the
        manager's own THD so the join aborts now with ER_QUERY_INTERRUPTED,
        before any result is sent. (The join's sub_select kill checks turn
        thd->killed into the error message.)
      */
      if (mgr->kill_signal != NOT_KILLED && !mgr->thd->killed)
      {
        killed_state ks= mgr->kill_signal;
        mysql_mutex_unlock(&mgr->LOCK_data);
        mysql_mutex_lock(&mgr->thd->LOCK_thd_kill);
        mgr->thd->killed= ks;
        mysql_mutex_unlock(&mgr->thd->LOCK_thd_kill);
        return 1;
      }
      if (mgr->fatal_error)                       // a worker failed
      {
        mysql_mutex_unlock(&mgr->LOCK_data);
        return 1;
      }
      if (!mgr->active_workers)                   // all producers done, drained
      {
        mysql_mutex_unlock(&mgr->LOCK_data);
        return -1;
      }
      if (mgr->thd->killed)
      {
        mysql_mutex_unlock(&mgr->LOCK_data);
        return 1;
      }
      // wait for a batch, a finishing worker, or a 1s tick to re-check killed.
      // ENTER_COND/EXIT_COND publish the "Reading data from parallel workers"
      // stage and register the cond so a KILL of the manager wakes it.
      wait.tv_sec= time(0) + 1;
      mgr->thd->ENTER_COND(&mgr->COND_data_avail, &mgr->LOCK_data,
                           &stage_reading_data_from_parallel_worker, &old_stage);
      mysql_cond_timedwait(&mgr->COND_data_avail, &mgr->LOCK_data, &wait);
      mgr->thd->EXIT_COND(&old_stage);          // unlocks LOCK_data
      mysql_mutex_lock(&mgr->LOCK_data);         // re-lock for the next pass
    }
    mgr->cur_worker= next;
    mysql_mutex_unlock(&mgr->LOCK_data);

    // open a scan on the chosen worker's batch table, then loop to read it
    TABLE *bt= next->batch_table;
    bt->in_use= mgr->thd;
    bt->file->ha_index_or_rnd_end();              // in case a scan was open
    if (bt->file->ha_rnd_init_with_error(1))
      return 1;
  }
}


static int parallel_scan_read_first(JOIN_TAB *tab)
{
  tab->table->status= 0;
  tab->read_record.table= tab->table;
  tab->read_record.read_record_func= parallel_scan_read_next;
  return parallel_scan_read_next(&tab->read_record);
}


/**
  @brief
    Stop the producers and pthread_join them.

  @description
    The workers read this join's source and batch tables, so they must be
    reaped before JOIN::join_free()->cleanup() frees those tables -- otherwise a
    worker that has not yet observed the stop request dereferences a freed
    table->file. That is why this is called from join_free(), ahead of the table
    teardown, and again (idempotently, guarded by 'reaped') from finalize.

    On a normal completion the workers have already finished; on an early-out
    (LIMIT) or an abort (KILL/error) we ask them to stop here. A worker
    re-checks 'stop' at each batch hand-off, so it exits within at most one
    batch -- without raising ER_QUERY_INTERRUPTED, which matters for a normal
    early-out.
*/

void pwt_management::quiesce_workers()
{
  if (!workers || reaped)
    return;

  /*
    On an early-out/error the manager may have stopped mid-batch with a scan
    still open on cur_worker's table; end it before that table is freed.
  */
  if (cur_worker)
  {
    cur_worker->batch_table->file->ha_rnd_end();
    cur_worker= nullptr;
  }

  mysql_mutex_lock(&LOCK_data);
  stop= true;
  mysql_cond_broadcast(&COND_data_space);
  mysql_mutex_unlock(&LOCK_data);

  for (uint i= 0; i < nworkers; i++)
  {
    pthread_join(workers[i].pthread, nullptr);
    mysql_mutex_destroy(&workers[i].LOCK_worker);
  }
  reaped= true;
}


/**
  @brief
    Reap the workers (if not already) and tear the channel down.

  @description
    Called from JOIN::exec() once exec_inner() has finished. Worker errors and
    warnings collected by PWT_error_handler are surfaced here, after the join's
    own result has been produced.
*/

void pwt_management::finalize_parallel_workers(THD *thd, JOIN *join)
{
  if (!workers)
    return;

  quiesce_workers();                  // stop + join (no-op if already reaped)

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
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
}
