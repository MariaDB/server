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
        table_can_be_parallel_scanned
                table-level eligibility for a parallel worker scan, shared by
                the cost hook and the runtime gate
        scale_cost_for_parallel_scan
                optimizer hook: discount a full-table-scan cost for a table
                that is eligible for parallel worker scan
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
                a batch at a time through the worker's reused raw row buffer
        pwt_management::handoff_batch
                producer: hand the filled row buffer to the manager and block
                until it is drained
        parallel_scan_read_next
                consumer: copy raw records from the worker row buffers
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
  { &key_memory_pwt_batch_rows,    "pwt_worker::batch_rows",    0},
};
#endif /* HAVE_PSI_INTERFACE */


/**
  @brief
    Whether a table's format and engine permit a parallel worker scan.

  @description
    Table-level eligibility shared by the optimizer cost hook
    (scale_cost_for_parallel_scan) and the runtime gate (make_join_readinfo):

      - a real base table (not an internal/temporary table);
      - no blob-backed columns (BLOB/TEXT/GEOMETRY/JSON) -- their payload lives
        off the record buffer and is not reproduced by the by-value row
        transport;
      - not fulltext-searched -- a MATCH ... AGAINST relevance is derived from
        handler state, not a stored column;
      - not partitioned;
      - the engine advertises HA_CAN_PARALLEL_SCAN.

    Caller-specific conditions (parallel_worker_threads, the access method being
    a full scan, the join position) are checked by each caller, not here.
*/

bool table_can_be_parallel_scanned(TABLE *table)
{
  return table->s->tmp_table == NO_TMP_TABLE &&
         table->s->blob_fields == 0 &&
         !table->fulltext_searched &&
         !table->part_info &&
         (table->file->ha_table_flags() & HA_CAN_PARALLEL_SCAN);
}


/**
  @brief
    Discount a full-table-scan cost when the table is eligible to be scanned by
    parallel workers.

  @description
    When parallel query is enabled the first non-const table can be scanned by
    N worker threads, each reading a disjoint partition concurrently while the
    manager runs the rest of the join. The wall-clock cost of reading and
    copying the rows is therefore roughly 1/N of a serial scan, so the row
    (full-scan) components of 'cost' -- I/O, CPU and row-copy -- are scaled by
    1/N. The index components are left untouched: this only ever discounts a
    full table scan.

    Eligibility mirrors the runtime gate in make_join_readinfo() exactly
    (engine support, no blob-backed columns, not fulltext-searched, a real base
    table, not partitioned), so the optimizer never discounts a scan that will
    not actually run in parallel. The caller is responsible for invoking this
    only for the driving table (idx == const_tables), the single position a
    parallel scan applies to, and 'cost' must be the caller's local copy, not
    the cached per-table estimate.

  @return
    true   the cost was scaled (table is parallel-scan eligible)
    false  no change (parallel scan disabled or table not eligible)
*/

bool scale_cost_for_parallel_scan(THD *thd, TABLE *table, ALL_READ_COST *cost)
{
  const uint n= thd->variables.parallel_worker_threads;
  if (n < 2 ||                                   // disabled, or no speed-up
      !table_can_be_parallel_scanned(table))
    return false;

  const double factor= 1.0 / (double) n;
  cost->row_cost.io  *= factor;
  cost->row_cost.cpu *= factor;
  cost->copy_cost    *= factor;
  return true;
}


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


/**
  @brief
    Hand this worker's filled batch buffer to the manager (producer side).

  Marks batch_rows ready and blocks until the manager has drained it
  (clears batch_full) or asks the producers to stop. On return the buffer is
  the worker's again: either ready to refill, or to be abandoned.

  @return
    true   the consumer asked us to stop (stop scanning)
    false  the buffer was drained; refill it
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
    worker->our_scan_table->file->update_global_table_stats();
    closefrm(worker->our_scan_table);
    my_free(worker->our_scan_table);
    worker->our_scan_table= nullptr;
  }
}

/**
  @brief
    Scan the worker's private source-table partition and stream the rows to the
    manager, a batch at a time, through the worker's reused row buffer.

  Each worker scans its own private copy of the source table
  (worker->our_scan_table, opened with in_use == this worker's thd) so the
  workers scan truly concurrently -- no shared-scan lock is needed. It copies
  up to PWT_CHUNK_ROWS raw record[0] images into batch_rows (the worker and
  the manager share the source TABLE_SHARE, so a byte-for-byte record copy
  reconstructs the row on the manager side -- no field_conv, no temp table),
  hands the buffer to the manager, and blocks until the manager has drained it
  (handoff_batch). It then refills the buffer from the top, until the
  partition is exhausted.

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
  const uint reclength= worker->manager->reclength;
  int err;

  src->use_all_columns();    // read every column into record[0]

  /*
    Our handle bypassed lock_tables(), so take the engine-level read lock
    ourselves; InnoDB needs this to register the table with its trx before
    a scan. Paired with the F_UNLCK below.
  */
  if ((err= src->file->ha_external_lock(worker->thd, F_RDLCK)))
    return err;

  err= src->file->pscan_init_worker(worker->engine_ctx);
  if (err != 0)
  {
    src->file->ha_external_lock(worker->thd, F_UNLCK);
    return err == HA_ERR_END_OF_FILE ? 0 : err;
  }

  bool eof= false, killed= false;
  while (!eof && !killed)
  {
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

      if ((err= src->file->ha_pscan_get_next_row(worker->engine_ctx)))
      {
        if (err == HA_ERR_END_OF_FILE)
        {
          err= 0;
          eof= true;
        }
        break;
      }
      memcpy(worker->batch_rows + (size_t) rows * reclength,
             src->record[0], reclength);
      rows++;
    }

    if (err && err != HA_ERR_END_OF_FILE)
      break;                            // real error; do not hand off
    err= 0;

    if (rows && !killed)                // hand the filled batch to the manager
    {
      worker->batch_count= rows;
      if (worker->manager->handoff_batch(worker))  // manager asked us to stop
        break;
    }
  }

  src->file->pscan_end_worker();
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

int pwt_management::init_parallel_workers(THD *thd, JOIN *join, JOIN_TAB *scan_tab)
{
  bool result= false;
  uint i= 0;

  const uint n= thd->variables.parallel_worker_threads;
  if (n == 0)
    return HA_ERR_UNSUPPORTED;

  TABLE *table = scan_tab->table;
  handler *file = table->file;
  this->join= join;
  this->thd= thd;
  this->scan_tab= scan_tab;

  // Initialize engine's parallel scan coordinator
  int err= file->pscan_init_coordinator(n);
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
    file->pscan_end_coordinator();
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
  active_workers= nworkers= n;
  reclength= table->s->reclength;     // shared record image size
  cur_cursor= 0;
  fatal_error= false;
  stop= false;
  reaped= false;
  cur_worker= nullptr;
  kill_signal= NOT_KILLED;

  for (i= 0; i < n; i++)
  {
    Parallel_scan::Worker_ctx *engine_ctx= file->pscan_get_worker_context(i);
    DBUG_ASSERT(engine_ctx);
    workers[i].engine_ctx= engine_ctx;

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
    workers[i].thd->thread_id= next_thread_id();
    my_snprintf(workers[i].info, sizeof(workers[i].info),
                WORKER_NAME " %u " CONNECTION_NAME_THREAD " %llu",
                i+1, thd->thread_id);
    workers[i].thd->query_string= CSET_STRING(workers[i].info,
                                              strlen(workers[i].info),
                                              workers[i].thd->query_charset());
    workers[i].thd->pwt_worker_info= workers+i;
    workers[i].batch_full= false;
    workers[i].batch_count= 0;
    workers[i].batch_rows= (uchar*) my_malloc(key_memory_pwt_batch_rows,
                                              (size_t) PWT_CHUNK_ROWS * reclength,
                                              MYF(MY_WME));
    if (!workers[i].batch_rows)
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to allocate worker row buffer");
      goto cleanup_db_string;
    }
    workers[i].thd->userstat_running= thd->userstat_running;

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
      TABLE *src= scan_tab->table;
      TABLE *st= (TABLE*) my_malloc(key_memory_TABLE, sizeof(TABLE),
                                    MYF(MY_WME | MY_ZEROFILL));
      if (!st)
        goto cleanup_db_string;

      if (open_table_from_share(thd, src->s, &src->s->table_name,
                                HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                                EXTRA_RECORD, thd->open_options, st,
                                false, nullptr))
      {
        my_free(st);
        my_error(ER_INTERNAL_ERROR, MYF(0),
                "init_parallel_workers: failed to open table from share");
        goto cleanup_db_string;
      }
      st->in_use= workers[i].thd;
      st->file->ha_handler_stats_reset();
      workers[i].our_scan_table= st;
    }

    server_threads.insert(workers[i].thd);  // +information_schema.processlist

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
  // free each worker's row buffer (NULL for those not yet allocated)
  for (uint j= 0; j < n; j++)
    my_free(workers[j].batch_rows);
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  mysql_mutex_destroy(&LOCK_pwt_thread);
  mysql_cond_destroy(&COND_data_avail);
  mysql_cond_destroy(&COND_data_space);
  mysql_mutex_destroy(&LOCK_data);
  file->pscan_end_coordinator();
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
  Consumer side of the streaming channel. This pluggable read function feeds
  the first join_tab from the worker row buffers instead of scanning the real
  first table: each raw record image a worker placed in its batch_rows buffer
  is memcpy'd straight into the first table's record[0] (worker and manager
  share the source TABLE_SHARE), so the manager's nested-loop join evaluates
  exactly as if it had scanned the table itself, but driven by the worker rows
  as they arrive. parallel_init_read_record installs the read function and
  returns the first row; parallel_scan_read_next returns each subsequent row,
  blocking when no worker batch is momentarily ready.

  The manager drains one worker's buffer at a time (mgr->cur_worker), advancing
  mgr->cur_cursor through its batch_count rows; when the buffer is exhausted it
  releases the worker to refill (clears batch_full, signals COND_data_space)
  and picks the next ready worker.

  @returns
    0 = row produced,
   -1 = end of data
    1 = error (matching report_error()).
*/
int parallel_scan_read_next(READ_RECORD *info)
{
  TABLE *dst= info->table;                       // real first table
  pwt_management *mgr= dst->reginfo.join_tab->join->parallel_work_manager;
  const uint reclength= mgr->reclength;
  struct timespec wait;
  wait.tv_nsec= 0;

  for (;;)
  {
    if (mgr->cur_worker)                          // draining a worker's buffer
    {
      pwt_worker *w= mgr->cur_worker;
      if (mgr->cur_cursor < w->batch_count)
      {
        // raw record image -> first table's record[0]; no Field::store()
        memcpy(dst->record[0],
               w->batch_rows + (size_t) mgr->cur_cursor * reclength,
               reclength);
        mgr->cur_cursor++;
        return 0;
      }
      // buffer drained; release the worker so it can refill
      mysql_mutex_lock(&mgr->LOCK_data);
      mgr->cur_worker= nullptr;
      w->batch_full= false;                       // buffer is the worker's again
      mysql_cond_broadcast(&mgr->COND_data_space); // wake it to refill
      mysql_mutex_unlock(&mgr->LOCK_data);
      // fall through and look for the next ready worker
    }

    // find the next worker whose buffer is filled and ready
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
    mgr->cur_cursor= 0;                            // start of next's buffer
    mysql_mutex_unlock(&mgr->LOCK_data);
    // loop back to drain next->batch_rows
  }
}


/**
  @brief
    Stop the producers and pthread_join them.

  @description
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

void pwt_management::quiesce_workers()
{
  if (!workers || reaped)
    return;

  // the consumer may have stopped mid-batch; drop its position (no open scan)
  cur_worker= nullptr;

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
  scan_tab->table->file->pscan_end_coordinator();
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
  for (uint i= 0; i < nworkers; i++)    // workers are joined, buffers idle
    my_free(workers[i].batch_rows);
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
}
