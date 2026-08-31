#ifndef SQL_PARALLEL_WORKERS_H
#define SQL_PARALLEL_WORKERS_H

#include "mariadb.h"
#include "sql_class.h"
#include "mysqld.h"
#include "sql_error.h"


#include "sql_parallel_thread.h"
#include "sql_parallel_transport.h"

/*
  Manager for a team of workers.

  Worker are threads that work towards some goal.
  If one worker fails or is killed, or the manager is killed, then all workers
  should stop working and be killed.

  When the manager finishes work, it waits for all workers to finish.

  Note that this class doesn't define what "work" is.
  When the workers need the manager's attention, they note it under LOCK_data
  and signal COND_data_avail.
*/

class pwt_manager_base : public pwt_thread_manager
{
  /*
    Set (under LOCK_data) to a worker's killed_state when that worker exits
    because it was killed -- e.g. a user KILL [QUERY] aimed at a parallel
    worker. The consumer propagates it to the manager's own THD so the join
    aborts with the right error (ER_QUERY_INTERRUPTED) before any result is
    sent, rather than completing and trying to raise the error too late.
  */
  killed_state             kill_signal;

  uint                     active_workers; // # workers who haven't finished.
public:
  bool                     fatal_error;    // a producer hit a real engine error
  pwt_manager_base();
  ~pwt_manager_base();

  void register_worker()
  {
    active_workers++;
  }

  int locked__process_manager_wakeup();

  bool is_fatal_error() { return fatal_error; }

  void report_fatal_error();
  void report_worker_final_state(killed_state state, bool err);

private:
  bool locked__no_active_workers()
  {
    mysql_mutex_assert_owner(&LOCK_data);
    return (active_workers == 0);
  }
  /* A worker exited because it was killed; NOT_KILLED if none did. */
  killed_state killed_by_worker() const { return kill_signal; }
public:

  /*
    The worker team's own state, as distinct from the transport's: who is still
    running, whether one of them failed, and whether we have had enough. The
    transport's two ends read it -- a consumer waiting for a row and a consumer
    waiting for the last worker to exit wait for the same event -- so LOCK_data
    and COND_data_avail guard it and belong here rather than to any one
    transport.
  */
  mysql_mutex_t            LOCK_data;

  /* This is to signal the manager to wake up and check its state. */
  mysql_cond_t             COND_data_avail;
};

class pwt_worker_base : public pwt_thread_with_stats
{
public:
  pwt_worker_base(pwt_manager_base *mgr_arg) :
    pwt_thread_with_stats(mgr_arg),
    mgr2(mgr_arg)
  {
    mgr2->register_worker();
  }

  void thread_func_end();
  void on_fatal_error() override;

  int err;
private:
  pwt_manager_base *mgr2;
};



class pwt_manager;
typedef struct st_join_table JOIN_TAB;
class JOIN;
class Item;
class TMP_TABLE_PARAM;


struct pwt_worker_execution
{
  /*
    A team is default-constructed as an array and can be taken apart from any
    stage of its build, so every member a teardown path reads has to mean
    "nothing here" before anything is set up. close_tables() reads tables and
    n_tables, free_containers() reads result, and neither is a member the
    compiler would zero.
  */
  pwt_worker_execution():
    scan_table(nullptr), tables(nullptr), n_tables(0),
    sums(nullptr), aggr_tab(nullptr),
    proj(nullptr), proj_count(0), join(nullptr), jointabs(nullptr),
    tab_stats(nullptr), tab_hstats(nullptr), handler_ctx(nullptr)
  {}

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
    manager's layout), and the column descriptions it was built from. What the
    worker does with it is the transport's business: the batch transport uses
    only its record buffer and fields, projecting the cloned column list into
    record[0] with Item::save_in_field and shipping those bytes, while the
    temporary-table transport keeps the row by writing it through the engine.
  */
  pwt_row_container     result;
  /*
    Pre-aggregation: this worker's own grouping table, keyed on the GROUP BY,
    holding one row per group of its chunk rather than one per source row. The
    projection goes here and the aggregates accumulate into its partial
    columns; at end of records the worker scans it and ships a row per group
    through 'result' like any other. Empty container when the query does not
    pre-aggregate.
  */
  pwt_row_container     group_container;
  /*
    This worker's clones of the query's aggregates, accumulating into the
    partial columns of group_container rather than into themselves, the same
    binding create_tmp_table() makes for the query's own aggregates, and what
    reset_field() and update_field() read and write. layout.n_sums of them.
  */
  Item_sum              **sums;
  /*
    Its own copy of the group key, pointed at group_container's columns,
    and the key buffer create_tmp_table() built alongside.
    Every worker needs a copy: an ORDER entry carries the key field and its
    offset in that buffer, so an entry belongs to one table.
  */
  /*
    The aggregation tab end_update() is called with: the server's own grouped
    aggregation, run over this worker's chunk into this worker's table. It is
    fabricated the way setup_worker_jointabs() fabricates the join tabs, and
    for the same reason -- so the executor runs on the worker's state and not
    the manager's.
  */
  JOIN_TAB              *aggr_tab;
  /*
    Per-worker deep clone of the shipped column list, one item per result
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
  Parallel_worker_ctx   *handler_ctx;
};


/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker : public pwt_worker_base
{
  int execute_and_handoff();
public:
  pwt_manager *manager;

  void thread_func() override;
  /*
    What thread_func() closes at the end of a run. A worker whose thread never
    started still had its tables opened for it, so somebody has to.
  */
  void cleanup_without_run() override { close_tables(); }

  /*
    This worker's producing end of the result-row transport, made by the
    manager's source (see pwt_row_source::make_sink). The worker projects a
    finished row into exec.result.record() and hands the image to this;
    how it travels from there is the transport's business, not the worker's.
  */
  pwt_row_sink          *sink;
  pwt_worker_execution  exec;

  pwt_worker(pwt_manager *manager_arg);

  /* Run this worker's share of the query and stream the result rows out. */
  void execute_and_signal_manager();
  /* Close this worker's private table copies (called by the worker thread). */
  void close_tables();
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
  /* A fully joined row: project it and hand it to the transport.
     0 = keep going, 1 = error, 2 = the manager asked us to stop
     (pwt_emit_result). */
  int emit_joined_row();
  /* Ship one row per group once the chunk is done. */
  int flush_groups();
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
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_manager : public pwt_manager_base
{
  /*
    The worker team, one entry per worker, held by pointer rather than by
    value: a worker's thread, the sinks the transport made for it and the
    JOIN_TABs it runs all keep pointers into its pwt_worker, so the object
    must not move once the team is built -- which an array of objects cannot
    promise, as growing it relocates every element. Filled one worker at a
    time by init_parallel_workers and emptied by destroy_workers(); an empty
    array means "no team", which is what the teardown paths test for.
  */
  Dynamic_array<pwt_worker*> workers;
  uint nworkers() const { return (uint) workers.elements(); }

  pwt_manager_execution    exec;

  /*
    How a worker's finished rows reach us: the row shape both ends agree on,
    and our end of the channel that carries it. Each worker holds the other end
    (pwt_worker::sink), made by this source. See sql_parallel_transport.h.
  */
  pwt_row_layout           layout;
  pwt_row_source           *source;


  /*
    Set once the workers have been stopped and pthread_join'd (quiesce_workers).
    Workers read this join's source tables (via their private handlers), so
    they must be reaped before JOIN::join_free()->cleanup() frees those tables;
    quiesce_workers is called from join_free, and again (idempotently) from
    finalize.
  */
  bool                     reaped;
public:
  bool                     stop;           // we want the producers to stop

  pwt_manager():
    workers(PSI_INSTRUMENT_MEM, 0, 8),
    source(nullptr), reaped(false),
    stop(false)
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
  /*
    The row shape both ends agree on; the worker paths read the partial set
     and the group key out of it.
  */
  pwt_row_layout &row_layout() { return layout; }

private:

  /* Delete the team's pwt_worker objects and empty workers[]. */
  void destroy_workers();

  /* Build the transport and give every worker its producing end. */
  bool setup_transport(THD *thd, uint n_workers);
  /* Undo setup_transport() and the team's state: release both ends of the
     transport, free the containers, destroy LOCK_data/COND_data_avail and the
     worker objects. Callers have already reaped or aborted every worker. */
  void destroy_transport();

  /* Free the row containers: the manager's and every worker's. */
  void free_containers(THD *thd);
  /* Stop the producers: set the request and wake anyone blocked in the
     transport waiting for us. Caller holds LOCK_data. */
  void request_stop();

  /*
    Give this worker its grouping table, its own copies of the query's
    aggregates, and a group key pointed at that table. Returns true on
    error.
  */
  bool setup_worker_preagg(THD *thd, pwt_worker *worker);
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

friend  pwt_worker;
};

extern bool table_can_be_parallel_scanned(TABLE *table);
extern bool scale_cost_for_parallel_scan(THD *thd, TABLE *table,
                                        ALL_READ_COST *cost);

extern bool parallel_scan_supports_access(JOIN_TAB *tab);
extern bool table_can_be_parallel_scanned(JOIN_TAB *tab);
extern bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab);
extern ORDER *pwt_preagg_group(JOIN *join);
extern int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab);
extern void check_parallel_scan(JOIN *join);
extern void recheck_parallel_scan(JOIN *join);
#endif
