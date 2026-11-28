#ifndef SQL_PARALLEL_WORKERS_H
#define SQL_PARALLEL_WORKERS_H

#include "mariadb.h"
#include "sql_class.h"
#include "mysqld.h"
#include "sql_error.h"

extern MYSQL_THD create_background_thd();
extern void destroy_background_thd(MYSQL_THD thd);
extern void *thd_attach_thd(MYSQL_THD thd);
extern void thd_detach_thd(void *save);

// PWT Parallel Worker Thread


/*
  Message Types
*/
class pwt_error_message
{
public:
  uint code;
  Sql_condition::enum_warning_level level;
  char *message;
};

/*
  Event type. Inherits ilink so it can live in an I_List<pwt_queued_event>.
*/
class pwt_queued_event : public ilink
{
public:
    pwt_error_message   *error;
};


/*
  Number of rows a worker packs into its batch temporary table before handing
  it to the manager. The worker hands rows to the manager a batch at a time
  rather than one at a time so the channel mutex is touched once per
  PWT_CHUNK_ROWS rows instead of once per row. Each worker reuses a single
  batch table (see pwt_worker::batch_table): it fills the table, hands
  it to the manager and blocks until the manager has drained it, then truncates
  and refills it for the next batch.
*/
#define PWT_CHUNK_ROWS 64

class pwt_management;


#define WORKER_NAME                    "Parallel Worker"
#define WORKER_ID_LENGTH               3
#define WORKER_NAME_LENGTH             15
#define CONNECTION_NAME_THREAD         "For Thread ID"
#define CONNECTION_NAME_THREAD_LENGTH  13
#define THREAD_ID_LENGTH               20         // ull can occupy 20 chars

/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker
{
public:
  THD             *thd;
  pwt_management  *manager;
  pthread_t       pthread;
  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr. See parallel_worker_thread_func.
  */
  mysql_mutex_t   LOCK_worker;
  char            conn_name[MAX_THREAD_NAME+1];
  /*
    This is displayed in information_schema.processlist.info
    Currently "Parallel Worker {1..N} For Thread M"
  */
  char            info[WORKER_NAME_LENGTH+
                       1+WORKER_ID_LENGTH+1+
                       CONNECTION_NAME_THREAD_LENGTH+
                       1+THREAD_ID_LENGTH+1];
  /*
    Per-worker copy of the our scan table, opened from the same TABLE_SHARE
    with in_use == this worker's thd. Gives the worker a private handler so
    it can scan concurrently with the other workers and the manager.
    Engines like InnoDB cache the THD pointer (m_user_thd) at open time,
    so a shared handler with a swapped in_use is not enough; each worker needs
    its own. Opened in init_parallel_workers, closed in the worker thread
    before its THD is destroyed.
  */
  TABLE           *our_scan_table;
  /*
    This worker's single batch temporary table, built in our_scan_table column
    format and created up front by JOIN::create_parallel_workers_tmp_tables
    (one per worker, stored in JOIN_TAB::parallel_tmp_tables).
    The worker reuses it for every chunk: fill it with up to PWT_CHUNK_ROWS
    rows, hand it to the manager, block until the manager has drained it,
    then truncate and refill. The worker and the manager never touch it at
    the same time, so it needs no per-row locking.
  */
  TABLE           *batch_table;
  /*
    Hand-off flag for batch_table, guarded by pwt_management::LOCK_data.
    The worker sets it true once the table is filled and ready for the manager;
    the manager clears it once the table is drained, releasing the worker to
    refill. See pwt_management::handoff_batch / the consumer read functions.
  */
  bool            batch_full;
};


/*
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_management : public Sql_alloc
{
public:
  pwt_worker        *workers;
  uint              nworkers;
  I_List<pwt_queued_event> parallel_messages;
  mysql_mutex_t     LOCK_pwt_thread;
  THD               *thd;
  /*
    Set under LOCK_pwt_thread when a worker fails to allocate a queued event.
    The manager surfaces a single ER_OUTOFMEMORY warning so the user sees
    that worker diagnostics were dropped instead of silently disappearing.
  */
  bool              messages_dropped;

  /*
    Streaming channel. Each worker (producer) fills its single reused batch
    table (batch_table) and hands it to the manager (single consumer)
    by setting its batch_full flag; the manager drains the table from its first
    join_tab read function and runs the rest of the join as the batches arrive,
    instead of waiting for every worker to finish first.

    LOCK_data guards cur_worker, the workers' batch_full flags, active_workers
    and the flags below. COND_data_avail wakes the consumer when a worker
    fills its table or finishes; COND_data_space wakes a worker when the
    manager has drained its table so it may refill. Because each worker owns
    one table and blocks until it is drained, at most one batch per worker is
    ever outstanding -- the single table is the natural backpressure bound.
    EOF for the consumer is the state (no worker has batch_full set &&
    active_workers == 0).
  */
  mysql_mutex_t     LOCK_data;
  mysql_cond_t      COND_data_avail;
  mysql_cond_t      COND_data_space;
  pwt_worker        *cur_worker;      // worker whose table the consumer drains
  uint              active_workers;   // producers still running
  bool              fatal_error;      // a producer hit a real engine error
  /*
    Set (under LOCK_data) to a worker's killed_state when that worker exits
    because it was killed -- e.g. a user KILL [QUERY] aimed at a parallel
    worker. The consumer propagates it to the manager's own THD so the join
    aborts with the right error (ER_QUERY_INTERRUPTED) before any result is
    sent, rather than completing and trying to raise the error too late.
  */
  killed_state      kill_signal;

  pwt_management():
    workers(nullptr),
    nworkers(0),
    messages_dropped(false),
    cur_worker(nullptr),
    active_workers(0),
    fatal_error(false),
    kill_signal(NOT_KILLED),
    stop(false),
    reaped(false)
    {}
  ~pwt_management()
  {
    finalize_parallel_workers(current_thd, join);
  }
  bool init_parallel_workers(THD *thd, JOIN *join);
  void quiesce_workers();
  void finalize_parallel_workers(THD *thd, JOIN *join);
  bool handoff_batch(pwt_worker *worker);
  void free_queue();

private:
  JOIN              *join;            // the join these workers serve
  bool              stop;             // consumer wants producers to stop
  /*
    Set once the workers have been stopped and pthread_join'd (quiesce_workers).
    Workers read this join's source and batch tables, so they must be reaped
    before JOIN::join_free()->cleanup() frees those tables; quiesce_workers is
    therefore called from join_free, and again (idempotently) from finalize.
  */
  bool              reaped;
};

#endif
