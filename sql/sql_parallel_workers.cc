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
  under, relaying their diagnostics, and reaping them.

  What a worker actually runs -- the gate, the per-worker copy of the plan and
  the join over a worker's chunk -- is in sql_parallel_execution.cc. How a row
  a worker has finished with reaches the manager is in
  sql_parallel_transport.cc; this file builds that transport and owns the state
  its two ends share about the team as a whole.
*/


#include "mariadb.h"
#include "mysqld_error.h"
#include "sql_class.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_select.h"
#include "sql_parallel_workers.h"
#include "debug_sync.h"
#include "transaction.h"

#ifdef HAVE_PSI_INTERFACE

static PSI_mutex_key key_mutex_pwt_LOCK_data;
static PSI_mutex_info all_pwt_mutexes[]=
{
  { &key_mutex_pwt_LOCK_data,        "pwt_manager::LOCK_data",      0},
};

static PSI_cond_key //key_COND_pwt_worker,
                    key_COND_pwt_data_avail;
static PSI_cond_info all_pwt_conds[]=
{
 // { &key_COND_pwt_worker,      "pwt_worker::COND_worker",                 0},
  { &key_COND_pwt_data_avail,  "pwt_manager::COND_data_avail",      0},
};

static PSI_memory_info all_pwt_memory[]=
{
  { &key_memory_pwt_queued_event,  "pwt_queued_event",          0},
  { &key_memory_pwt_error_message, "pwt_error_message",         0},
  { &key_memory_pwt_db,            "pwt_worker::db",            0},
  { &key_memory_pwt_batch_rows,    "pwt_batch_sink::rows",      0},
};
#endif /* HAVE_PSI_INTERFACE */


// TODO: this should go into pwt_manager_base.
//  but the action here goes beyond pwt_manager_base.
void pwt_manager::notify_fatal_error()
{
  mysql_mutex_lock(&LOCK_data);
  fatal_error= true;
  /* This is to notify other workers too */
  mysql_cond_broadcast(&COND_data_avail);
  mysql_mutex_unlock(&LOCK_data);
}


/**
  @brief
    Entry point for our worker threads, arg supplied by manager details what
    needs to be run
*/

void pwt_worker::thread_func()
{
  execute_and_signal_manager();
 
  /*
    Close our private table copies while we are still attached to our THD
    (current_thd == thd) and, crucially, before destroy_background_thd()
    tears down the THD's transaction: the engine handle's close frees state
    that references that transaction (InnoDB's prebuilt). The manager never
    touches a started worker's tables, so no lock is needed here.
  */
  snapshot_table_stats();          // while the tables are still open
  close_tables();

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
  exec.stats= thd->status_var;
  exec.stats.global_memory_used= 0;
  exec.stats.tmp_space_used= 0;
  thd->set_status_var_init(clear_for_flush_status);
}



/*
  Snapshot the quick select's key intervals into the `ranges` array.
  An empty result means "scan the whole table".
  @return true on error (my_error() has been called)
*/
static bool parallel_build_key_ranges(JOIN_TAB *tab,
                                      Dynamic_array<KEY_MULTI_RANGE> *ranges)
{
  /*
    Only a table scan contributes intervals. A JT_NEXT tab may still carry a
    quick select that the plan discarded in favour of the index scan; its
    ->index need not be the index being scanned, so it must not be harvested.
  */
  if (!tab->use_parallel_scan || tab->type == JT_NEXT ||
      !tab->select || !tab->select->quick)
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

  /*
    Which index the engine is being asked to divide. The intervals, when there
    are any, are over the quick select's index; a bare index scan divides the
    index the plan picked; and MAX_KEY means the whole table.
  */
  const uint keynr= ranges.size() ? scan_tab->select->quick->index
                    : scan_tab->type == JT_NEXT ? scan_tab->index : MAX_KEY;

  // Initialize engine's parallel scan coordinator, ranges copied if reqd.
  int err= file->parallel_init_coordinator(n, keynr, ranges);
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
  
  workers= new pwt_worker[n]; // TODO: does this occur on mem_root this way?
                              // No, this is a normal un-instrumented heap allocation
  if (!workers)
  {
    file->parallel_end_coordinator();
    return HA_ERR_UNSUPPORTED;
  }

  /*
    Publish the team's state before any worker starts: a worker's first act may
    be to hand a row to the transport, whose two ends read this under
    LOCK_data. active_workers must already equal n so the consumer does not
    mistake "not started yet" for EOF.
  */
  mysql_mutex_init(key_mutex_pwt_LOCK_data, &LOCK_data, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_pwt_data_avail, &COND_data_avail, nullptr);
  active_workers= nworkers= n;

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
    Work out the row shape the workers and this thread will agree on, and build
    the transport that carries rows in it. Both are described in
    sql_parallel_transport.h; from here on nothing in this file knows how a row
    travels, only that a worker has a sink to hand one to and we have a source
    to take the next one from.
  */
  if (layout.build(thd, join, exec.tables, exec.n_tables) ||
      setup_transport(thd, n))
    goto cleanup_old_workers;

  fatal_error= false;
  stop= false;
  reaped= false;

  for (i= 0; i < n; i++)
  {
    workers[i].exec.handler_ctx= file->parallel_get_worker_context(i);
    DBUG_ASSERT(workers[i].exec.handler_ctx);

    if (workers[i].init_worker_thd(this, thd, /*worker_nr=*/i+1))
      goto cleanup_old_workers;

    workers[i].thd->userstat_running= thd->userstat_running;

    /*
      Give this worker its own copy of every non-const join table, opened from
      the shared TABLE_SHARE (open_worker_tables); the driving table is
      exec.tables[0] / exec.scan_table. Self-cleans on failure, so on
      error we go to cleanup_db_string (the worker thd is not yet registered).
    */
    if (open_worker_tables(thd, workers + i))
      goto cleanup_db_string;

    /*
      Set up how this worker joins the non-driving tables (access method,
      worker-bound ref clone, condition), its result container, and private
      clones of the WHERE condition + select list with field references rebound
      to this worker's table copies. At run time the worker scans the driving
      chunk, joins the inner tables, projects exec.proj into
      its result container and ships that record image.
    */
    if (setup_worker_join(thd, workers + i) ||
        setup_worker_jointabs(thd, workers + i) ||
        layout.make_container(thd, &workers[i].exec.result) ||
        !(workers[i].sink= source->make_sink(thd, i,
                                             &workers[i].exec.result)) ||
        clone_worker_exprs(thd, workers + i))
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to set up worker execution");
      goto cleanup_thread_create;
    }
    
    /*
      Fail the last worker as if its thread could not be created, so a test can
      reach cleanup_old_workers with the earlier workers already running: that
      is the only path to abort_worker(), and the only one that tears a partly
      built team down. Injected here rather than after create_thread() because
      cleanup_thread_create closes this worker's tables and destroys its THD,
      which is only safe while it has no thread of its own.
    */
    bool inject_create_failure= false;
    DBUG_EXECUTE_IF("pwt_init_fail_last_worker",
                    inject_create_failure= (i + 1 == n););

    if (inject_create_failure || workers[i].create_thread())
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to create worker thread");
      goto cleanup_thread_create;
    }
  }
  return 0;

cleanup_thread_create:
  workers[i].close_tables();

cleanup_db_string:
  workers[i].destroy_worker_thd();
  workers[i].cleanup_worker();

cleanup_old_workers:
  /*
    A worker spawned before the failure may be blocked inside the transport
    waiting for the manager to take its rows. Release them so abort_worker()'s
    join can complete.
  */
  mysql_mutex_lock(&LOCK_data);
  request_stop();
  mysql_mutex_unlock(&LOCK_data);
  for (uint j= 0; j < i; j++)
    workers[j].abort_worker();
  discard_pending_warnings();
  /*
    Release the transport's hold on the containers before freeing them: a sink
    may still be naming a worker THD that is now gone. Null for a worker whose
    setup never got that far.
  */
  for (uint j= 0; j < n; j++)
    if (workers[j].sink)
      workers[j].sink->cleanup();
  if (source)
    source->cleanup();
  free_containers(thd);            // workers reaped; containers now idle
  delete[] workers;
  workers= nullptr;
  nworkers= 0;
  mysql_cond_destroy(&COND_data_avail);
  mysql_mutex_destroy(&LOCK_data);
  file->parallel_end_coordinator();
  return 1;                           // reached only on failure
}

#ifdef HAVE_PSI_INTERFACE
void pwt_init_psi_keys(void)
{
  const char *category= "sql";
  int count;
  count= array_elements(all_pwt_mutexes);
  mysql_mutex_register(category, all_pwt_mutexes, count);
  count= array_elements(all_pwt_conds);
  mysql_cond_register(category, all_pwt_conds, count);
  count= array_elements(all_pwt_memory);
  mysql_memory_register(category, all_pwt_memory, count);
  pwt_transport_init_psi_keys();
}
#endif


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

  /*
    The consumer may have stopped part-way through a producer's rows: drop that
    position before the producers are reaped, so nothing is left pointing into
    them.
  */
  if (source)
    source->release_position();

  mysql_mutex_lock(&LOCK_data);
  request_stop();
  mysql_mutex_unlock(&LOCK_data);

  for (uint i= 0; i < nworkers; i++)
  {
    workers[i].join_worker_thread();
    workers[i].cleanup_worker();
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
  process_pending_warnings();
  for (uint i= 0; i < nworkers; i++)    // workers are joined; both ends idle
    if (workers[i].sink)
      workers[i].sink->cleanup();
  if (source)
    source->cleanup();
  free_containers(thd);              // the transport has let go of them
  mysql_cond_destroy(&COND_data_avail);
  mysql_mutex_destroy(&LOCK_data);
  delete [] workers;
  workers= nullptr;
  nworkers= 0;
  DBUG_VOID_RETURN;
}


/*
  @brief
    Build the result-row transport: the manager's consuming end of it.

  @description
    One place makes the choice (pwt_create_transport), and the source it
    returns is what makes the matching sinks, so a worker is never paired with
    an end of a different kind. Called once the row layout is known; each
    worker's sink is made later, with the rest of that worker's setup, because
    a sink may need the container that setup builds.

  @return  true on error (my_error() has been called).
*/

bool pwt_manager::setup_transport(THD *thd, uint n_workers)
{
  return (source= pwt_create_transport(thd, this, n_workers,
                                       layout.reclength)) == nullptr;
}


/*
  @brief
    Ask the producers to stop, and wake any that are waiting on us.

    The request is the team's (a satisfied LIMIT, a KILL, an error, a team that
    failed to start); the waking is the transport's, and is a no-op for a
    transport in which a producer never waits for the consumer. Caller holds
    LOCK_data.
*/

void pwt_manager::request_stop()
{
  mysql_mutex_assert_owner(&LOCK_data);
  stop= true;
  if (source)
    source->wake_producers();
}
