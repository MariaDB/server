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
  uint worker_errno;
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
  Number of rows a worker packs into its batch buffer before handing it to the
  manager. The worker hands rows to the manager a batch at a time rather than
  one at a time so the channel mutex is touched once per PWT_CHUNK_ROWS rows
  instead of once per row. Each worker reuses a single row buffer (see
  pwt_worker_batch::rows): it fills the buffer with up to PWT_CHUNK_ROWS
  result-row images (the base-table columns it read for the rows of its chunk
  that qualified), hands it to the manager and blocks until the manager has
  drained it, then refills it for the next batch.
*/
//#define PWT_CHUNK_ROWS 2048
#define PWT_CHUNK_ROWS 128

class pwt_manager;
typedef struct st_join_table JOIN_TAB;
class JOIN;
class Item;
class TMP_TABLE_PARAM;

#define WORKER_NAME                    "Parallel Worker"
#define WORKER_ID_LENGTH               3
#define WORKER_NAME_LENGTH             15
#define CONNECTION_NAME_THREAD         "For Thread ID"
#define CONNECTION_NAME_THREAD_LENGTH  13
#define THREAD_ID_LENGTH               20         // ull can occupy 20 chars

struct pwt_worker_batch
{
  /*
    This worker's single reused row buffer, PWT_CHUNK_ROWS * reclength bytes
    (reclength == the manager's result_table record size), allocated up front
    in init_parallel_workers. For each row of its chunk that survives the join
    and the WHERE filter the worker projects the shipped columns into
    exec.result_table->record[0] (see exec.proj) and memcpy's that
    record image into this buffer. The manager and the workers share an
    identical result_table layout, so a byte-for-byte record copy reconstructs
    the result row on the manager side. It hands the buffer to the manager,
    blocks until the manager has drained it, then refills from the top. The
    worker and the manager never touch it at the same time, so it needs no
    per-row locking.
  */
  uchar           *rows;
  uint            count;   // rows the worker placed in batch.rows
  /*
    Hand-off flag for batch.rows, guarded by pwt_manager::drain.LOCK_data.
    The worker sets it true once the buffer is filled and ready for the manager;
    the manager clears it once the buffer is drained, releasing the worker to
    refill. See pwt_manager::handoff_batch / pwt_manager::drain_next_row.
  */
  bool            full;
};


struct pwt_worker_execution
{
  /*
    Per-worker copy of the manager's first non-const source table, the
    parallel-scanned driving table (== tables[0]). Gives the worker a private
    handler so it can scan concurrently with the other workers and the
    manager. Engines like InnoDB cache the THD pointer (m_user_thd) at open
    time, so a shared handler with a swapped in_use is not enough; each worker
    needs its own. Opened in init_parallel_workers, closed in the worker thread
    before its THD is destroyed.
  */
  TABLE                 *scan_table;
  /*
    Multi-table join: this worker's private copy of every non-const join table
    (tables[0] == scan_table, the parallel-scanned driving table; tables[1 ..
    n_tables-1] are the tables joined after it, in join order). jointabs[]
    carries the matching JOIN_TAB for each of them.
  */
  TABLE                 **tables;
  uint                  n_tables;
  /*
    This worker's private result container: a tmp table whose columns are the
    base-table columns the query reads (built in init_parallel_workers from the
    manager's result_table layout). The worker only uses its record buffer and
    fields -- it projects the cloned column list into result_table->record[0]
    with Item::save_in_field and ships those bytes; it never writes rows
    through the storage engine.
  */
  TABLE                 *result_table;
  /*
    Per-worker deep clone of the shipped column list, one item per result_table
    field, with its Item_field leaves rebound to this worker's table copies.
    Each worker owns its own clones so the threads never share mutable Item
    state (null_value, cached results) while they evaluate concurrently.
    Created on the manager thread in init_parallel_workers; evaluated only by
    this worker. Every table's condition, the driving table's included, lives
    on the matching jointabs[] entry as its select_cond.
  */
  Item                  **proj;
  uint                  proj_count;
  /*
    The JOIN the worker's JOIN_TABs belong to. Not the manager's: the executor
    writes join->return_tab as it descends, so two workers sharing one JOIN
    would scribble over each other's backtrack point, and it reads join->thd
    for the diagnostics area, the killed flag and the row counters, which must
    be the worker's own. Built by setup_worker_join(), which carries over only
    the fields the executor reads.
  */
  JOIN                  *join; // The JOIN for the worker's JOIN_TABs
  /*
    This worker's copy of the join's non-const JOIN_TABs, n_tables of them, in
    join order and indexed the same way pwt_manager::exec.jointabs is: [0] is
    the parallel-scanned driving table and [1..] the tables joined to it. Copied
    from the manager's tabs and then rebound to this worker (its TABLE copies,
    its cloned conditions and refs, its trackers) by setup_worker_jointabs().
  */
  JOIN_TAB              *jointabs;   // n_tables x non-const JOIN_TABs in order
  /*
    What this worker did to each of its tables, in the terms ANALYZE reports:
    rows read, rows that passed the table's condition, and scans, plus the
    engine's own counters. Indexed like tables. The manager adds these to the
    JOIN_TAB trackers and handlers the optimizer left for ANALYZE to read,
    once the workers have been joined -- without that, ANALYZE says the
    parallel-scanned table was never read at all. See quiesce_workers().
  */
  Table_access_tracker  *tab_stats;
  ha_handler_stats      *tab_hstats;
  /*
    This worker's status counters, copied out of its THD just before the THD is
    destroyed, so the manager can add them to the session's own once the
    workers have been joined. See quiesce_workers().
  */
  STATUS_VAR            stats;
  Parallel_worker_ctx   *handler_ctx;
};

struct pwt_worker_info
{
  char            conn_name[MAX_THREAD_NAME+1];
  /*
    This is displayed in information_schema.processlist.info
    Currently "Parallel Worker {1..N} For Thread M"
  */
  char            process_list[WORKER_NAME_LENGTH+
                               1+WORKER_ID_LENGTH+1+
                               CONNECTION_NAME_THREAD_LENGTH+
                               1+THREAD_ID_LENGTH+1];
};

struct pwt_worker_state
{
  bool            joined;
  bool            finished;
  killed_state    killed;
};

/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker
{
  int execute_and_handoff();
public:
  /*
    Intialize the worker and create its THD.
    (this is called from the master thread)
  */
  bool init_worker_thd(pwt_manager *manager_arg, THD *parent_thd,
                       int worker_nr);
  /* This is like a destructor. Called after worker is done. */
  void cleanup_worker();

  THD             *thd;
  pwt_manager     *manager;
  pthread_t       pthread;
  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr.
    See parallel_worker_thread_func.
  */
  mysql_mutex_t   LOCK_worker;
  // mysql_cond_t    COND_worker;

  pwt_worker_info       info;
  pwt_worker_state      state;
  pwt_worker_batch      batch;
  pwt_worker_execution  exec;

  /* Run this worker's share of the query and stream the result rows out. */
  void execute_and_signal_manager();
  /* Close this worker's private table copies (called by the worker thread). */
  void close_tables();
  void abort_worker();
  /* Copy the engine counters out of the tables before they are closed. */
  void snapshot_table_stats();

  /*
    The two things the executor's function pointers need to reach on a worker.
    The callbacks in sql_parallel_execution.cc are thin trampolines onto these,
    because READ_RECORD::Read_func and Next_select_func have fixed signatures
    that carry no worker.
  */
  /* Next row of this worker's chunk. Handler error code, 0 on success. */
  int pscan_next_row();
  /* A fully joined row: project it and add it to the batch.
     0 = keep going, 1 = error, 2 = the manager asked us to stop. */
  int emit_joined_row();

  bool handoff_batch();
};


/*
  Streaming channel. Each worker (producer) fills its single reused row
  buffer (batch.rows) with the result rows it computed for its chunk and hands
  it to the manager (single consumer) by setting its batch.full flag; the
  manager (drain_and_send) drains the buffer and sends each result
  row to the client as the batches arrive, instead of waiting for every worker
  to finish first.

  LOCK_data guards cur_worker, the workers' batch.full flags, active_workers
  and the flags below. COND_data_avail wakes the consumer when a worker
  fills its buffer or finishes; COND_data_space wakes a worker when the
  manager has drained its buffer so it may refill. Because each worker owns
  one buffer and blocks until it is drained, at most one batch per worker is
  ever outstanding -- the single buffer is the natural backpressure bound.
  EOF for the consumer is the state (no worker has batch.full set &&
  active_workers == 0).
*/

struct pwt_manager_drain
{
  pwt_worker        *cur_worker;      // worker whose buffer the consumer drains
  uint              cur_cursor;       // consumer's row index within cur_worker
  uint              reclength;        // result_table record image size (bytes)
  uint              active_workers;   // producers still running
  bool              stop;             // consumer wants producers to stop

  pwt_manager_drain():
    cur_worker(nullptr),
    active_workers(0),
    stop(false)
  { }
};

struct pwt_manager_execution
{
  /*
    The non-const join tables, in join order (jointabs[0] == scan_tab, the
    parallel-scanned driving table). tables holds their TABLEs; together
    with each worker's exec.tables they form the manager->worker table
    map used to rebind the cloned conditions/refs/column list. Set up once, on
    the manager thread, in init_parallel_workers.
  */
  JOIN_TAB          **jointabs;
  TABLE             **tables;
  uint              n_tables;
  JOIN              *join;                   // the join these workers serve
  JOIN_TAB          *scan_tab;

  pwt_manager_execution() :
    jointabs(nullptr),
    tables(nullptr),
    n_tables(0)
  { };
};


/*
  How a worker's row reaches the manager's own records: the container the two
  sides agree on, the columns a worker ships in it, and the copies that put
  those columns back where the query's items expect to read them.
*/

struct pwt_manager_transport
{
  /*
    Result container shared (by layout) with every worker's exec.result_table.
    The manager receives each worker result-row image into
    result_table->record[0] and copies its columns back into its own
    base-table records, from where the query's own items read them.
    result_tmp_param backs the create/instantiate/free of result_table and the
    per-worker copies.
  */
  TABLE             *result_table;
  TMP_TABLE_PARAM   *result_tmp_param;

  /*
    What a worker ships per row: every base-table column the query reads, in
    table order. Not the projected select list -- the manager copies each column
    back into the field it came from in its own table instances, so everything
    that reads a record reads what a serial scan would have left there, with
    nothing redirected. That is required rather than merely tidy: a temp table is
    filled through the Copy_field pairs create_tmp_table() builds, which hold raw
    Field pointers into the base tables, so re-pointing Items cannot reach them.

    ship_list holds Item_fields over the manager's own fields. result_defn holds
    clones of them, and that is what defines the column layout the workers and
    the manager agree on. copy_back is one Copy_field per shipped column,
    result_table's field to the base-table field it came from; n_copy_back is
    ship_list's length less the filler a query reading no column ships, which has
    no destination.
  */
  List<Item>        ship_list;
  Copy_field        *copy_back;
  uint              n_copy_back;
  /*
    Clones of the shipped columns that define the result_table columns (kept so
    the manager and every worker build the identical result layout).
  */
  List<Item>        result_defn;

  pwt_manager_transport() :
    result_table(nullptr),
    result_tmp_param(nullptr),
    copy_back(nullptr),
    n_copy_back(0)
  { };
};


/*
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_manager : public Sql_alloc
{
  pwt_manager_execution    exec;
  pwt_manager_transport    transport;
  pwt_worker               *workers;
  I_List<pwt_queued_event> parallel_messages;

protected:
  pwt_manager_drain        drain;

public:
  uint                     nworkers;
  mysql_mutex_t            LOCK_pwt_thread;
  mysql_mutex_t            LOCK_data;
  mysql_cond_t             COND_data_avail;
  mysql_cond_t             COND_data_space;
  THD                      *thd;
  bool                     fatal_error;   // a producer hit a real engine error

  void notify_message_dropped();
private:
  /*
    Set under LOCK_pwt_thread when a worker fails to allocate a queued event.
    The manager surfaces a single ER_OUTOFMEMORY warning so the user sees
    that worker diagnostics were dropped instead of silently disappearing.
  */
  bool                     messages_dropped;
public:

  /*
    Set once the workers have been stopped and pthread_join'd (quiesce_workers).
    Workers read this join's source tables (via their private handlers), so
    they must be reaped before JOIN::join_free()->cleanup() frees those tables;
    quiesce_workers is called from join_free, and again (idempotently) from
    finalize.
  */
  bool                     reaped;
  /*
    Set (under LOCK_data) to a worker's killed_state when that worker exits
    because it was killed -- e.g. a user KILL [QUERY] aimed at a parallel
    worker. The consumer propagates it to the manager's own THD so the join
    aborts with the right error (ER_QUERY_INTERRUPTED) before any result is
    sent, rather than completing and trying to raise the error too late.
  */
  killed_state             kill_signal;

  pwt_manager():
    workers(nullptr),
    nworkers(0),
    fatal_error(false),
    messages_dropped(false),
    reaped(false),
    kill_signal(NOT_KILLED)
    {}
  ~pwt_manager()
  {
    finalize_parallel_workers(current_thd, exec.join);
  }
  int init_parallel_workers(THD *thd, JOIN *join, JOIN_TAB *scan_tab);
  void quiesce_workers();
  void finalize_parallel_workers(THD *thd, JOIN *join);
  /*
    Consumer: drain result rows from the workers and send them to the client.
    Returns 0 on success (all rows sent), 1 on error.
  */
  int drain_and_send(JOIN *join);
  void free_queue();
  void record_event(pwt_queued_event *event)
  {
    mysql_mutex_lock(&LOCK_pwt_thread);
    parallel_messages.push_back(event);
    mysql_mutex_unlock(&LOCK_pwt_thread);
  }

  /* Copy the next worker result-row image into dst (reclength bytes).
     0 = row produced, -1 = end of data, 1 = error. */
  int drain_next_row(uchar *dst);
  /* Create + instantiate one result container from the column definition list
     'defn'. Returns true on error. */
  bool make_result_table(THD *thd, List<Item> &defn, TABLE **out);
  /* Deep-clone this query's conditions + shipped column list for 'worker',
     rebinding the Item_field leaves to the worker's table copies. Returns
     true on error. */
  bool clone_worker_exprs(THD *thd, pwt_worker *worker);
  /* Open this worker's private copy of every non-const join table (into
     worker->exec.tables / worker->exec.scan_table). Returns true on
     error. */
  bool open_worker_tables(THD *thd, pwt_worker *worker);
  /* Build worker->exec.join: the JOIN the worker's tabs belong to. */
  bool setup_worker_join(THD *thd, pwt_worker *worker);
  /* Build worker->exec.jointabs: a copy of each of the manager's non-const
     JOIN_TABs, rebound to this worker. */
  bool setup_worker_jointabs(THD *thd, pwt_worker *worker);
  /* Free the manager and per-worker result containers. */
  void free_result_tables(THD *thd);

friend  pwt_worker;
};

extern bool table_can_be_parallel_scanned(TABLE *table);
extern bool scale_cost_for_parallel_scan(THD *thd, TABLE *table,
                                        ALL_READ_COST *cost);

extern int parallel_init_read_record(JOIN_TAB *tab);
extern bool parallel_scan_supports_access(JOIN_TAB *tab);
extern bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab);
extern int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab);
#endif
