#ifndef SQL_PARALLEL_WORKERS_H
#define SQL_PARALLEL_WORKERS_H

#include "mariadb.h"
#include "sql_class.h"
#include "mysqld.h"
#include "sql_error.h"


#include "sql_parallel_thread.h"
#include "sql_parallel_transport.h"

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
    proj(nullptr), proj_count(0), join(nullptr), jointabs(nullptr),
    tab_stats(nullptr), tab_hstats(nullptr), handler_ctx(nullptr)
  { bzero(&stats, sizeof(stats)); }

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
  /*
    This worker's status counters, copied out of its THD just before the THD is
    destroyed, so the manager can add them to the session's own once the
    workers have been joined. See quiesce_workers().
  */
  STATUS_VAR            stats;
  Parallel_worker_ctx   *handler_ctx;
};


/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker : public pwt_worker_base
{
  int execute_and_handoff();
public:

  /*
    This worker's producing end of the result-row transport, made by the
    manager's source (see pwt_row_source::make_sink). The worker projects a
    finished row into exec.result.record() and hands the image to this;
    how it travels from there is the transport's business, not the worker's.
  */
  pwt_row_sink          *sink;
  pwt_worker_execution  exec;

  /*
    The team is default-constructed as an array before anything is set up, and
    a worker whose setup never got as far as these two is still walked by the
    failure paths (free_result_tables, the sink release in
    init_parallel_workers). Neither is a member the compiler would zero.
  */
  pwt_worker(): sink(nullptr) { }

  void thread_func() override;
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
  pwt_manager_execution    exec;

  /*
    How a worker's finished rows reach us: the row shape both ends agree on,
    and our end of the channel that carries it. Each worker holds the other end
    (pwt_worker::sink), made by this source. See sql_parallel_transport.h.
  */
  pwt_row_layout           layout;
  pwt_row_source           *source;

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
  mysql_cond_t             COND_data_avail;
  uint                     active_workers; // producers still running
  bool                     stop;           // we want the producers to stop
  bool                     fatal_error;    // a producer hit a real engine error
  void notify_fatal_error();
  bool is_fatal_error() { return fatal_error; }
  /* A worker exited because it was killed; NOT_KILLED if none did. */
  killed_state killed_by_worker() const { return kill_signal; }

  pwt_manager():
    source(nullptr), active_workers(0), stop(false), fatal_error(false)
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

private:

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
  /* Free the row containers: the manager's and every worker's. */
  void free_containers(THD *thd);
  /* Build the transport and give every worker its producing end. */
  bool setup_transport(THD *thd, uint n_workers);
  /* Stop the producers: set the request and wake anyone blocked in the
     transport waiting for us. Caller holds LOCK_data. */
  void request_stop();

friend  pwt_worker;
};

extern bool table_can_be_parallel_scanned(TABLE *table);
extern bool scale_cost_for_parallel_scan(THD *thd, TABLE *table,
                                        ALL_READ_COST *cost);

extern int parallel_init_read_record(JOIN_TAB *tab);
extern bool parallel_scan_supports_access(JOIN_TAB *tab);
extern bool table_can_be_parallel_scanned(JOIN_TAB *tab);
extern bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab);
extern int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab);
extern void check_parallel_scan(JOIN *join);
extern void recheck_parallel_scan(JOIN *join);
#endif
