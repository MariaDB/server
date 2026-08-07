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
  Number of rows a worker packs into its batch buffer before handing it to the
  manager. The worker hands rows to the manager a batch at a time rather than
  one at a time so the channel mutex is touched once per PWT_CHUNK_ROWS rows
  instead of once per row. Each worker reuses a single row buffer (see
  pwt_worker::batch_rows): it fills the buffer with up to PWT_CHUNK_ROWS
  result-row images (the projected select-list values it computed for the rows
  of its chunk that passed the WHERE filter), hands it to the manager and
  blocks until the manager has drained it, then refills it for the next batch.
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

/*
  Parallel Worker Thread specific attributes
*/
class pwt_worker
{
public:
  void worker_run_query_to_manager();
  pwt_manager  *manager;
  THD             *thd;
  /*
    Hand-off flag for batch_rows, guarded by pwt_manager::LOCK_data.
    The worker sets it true once the buffer is filled and ready for the manager;
    the manager clears it once the buffer is drained, releasing the worker to
    refill. See pwt_manager::handoff_batch / the consumer read function.
  */
  bool            batch_full;
  /*
    The two things the executor's function pointers need to reach on a worker.
    The callbacks in sql_parallel_workers.cc are thin trampolines onto these,
    because READ_RECORD::Read_func and Next_select_func have fixed signatures
    that carry no worker.
  */
  /* Next row of this worker's chunk. Handler error code, 0 on success. */
  int pscan_next_row();
  /* A fully joined row: project it and add it to the batch.
     0 = keep going, 1 = error, 2 = the manager asked us to stop. */
  int emit_joined_row();

  /* Close this worker's private table copies (called by the worker thread). */
  void close_worker_tables();
  void abort_worker();

  void set_engine_ctx(Parallel_scan::Worker_ctx *engine_ctx)
  {

    DBUG_ASSERT(engine_ctx);
    this->engine_ctx= engine_ctx;
  }

  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr.
    See parallel_worker_thread_func.
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

  pthread_t       pthread;

  /*
    This worker's status counters, copied out of its THD just before the THD is
    destroyed, so the manager can add them to the session's own once the workers
    have been joined. See quiesce_workers().
  */
  STATUS_VAR      stats;

  /*
    What this worker did to each of its tables, in the terms ANALYZE reports:
    rows read, rows that passed the table's condition, and scans, plus the
    engine's own counters. Indexed like worker_tables. The manager adds these to
    the JOIN_TAB trackers and handlers the optimizer left for ANALYZE to read,
    once the workers have been joined -- without that, ANALYZE says the
    parallel-scanned table was never read at all. See quiesce_workers().
  */
  Table_access_tracker *tab_stats;
  ha_handler_stats     *tab_hstats;

  /* Copy the engine counters out of the tables before they are closed. */
  void snapshot_table_stats();


  /*
    The JOIN the worker's JOIN_TABs belong to. Not the manager's: sub_select()
    writes join->return_tab as it descends, so two workers sharing one JOIN
    would scribble over each other's backtrack point, and it reads join->thd for
    the diagnostics area, the killed flag and the row counters, which must be
    the worker's own. Built by setup_worker_join(), which carries over only the
    fields the executor reads.
  */
  JOIN          *worker_join;

  /*
    This worker's copy of the join's non-const JOIN_TABs, n_tables of them, in
    join order and indexed the same way pwt_manager::mgr_tabs is: [0] is the
    parallel-scanned driving table and [1..] the tables joined to it. Copied
    from the manager's tabs and then rebound to this worker (its TABLE copies,
    its cloned conditions and refs, its trackers) by setup_worker_tabs().
  */
  JOIN_TAB      *join_tabs;
  /* Pre-aggregation: add a joined row to this worker's own aggregates, and ship
     the chunk's partial values at end of records -- one row, or with a GROUP BY
     one row per group. Called from pwt_end_send(), like emit_joined_row(). */
  int accumulate_partial();
  int emit_partial_row();

private:
  int worker_run_query();

  /* Grouped pre-aggregation: accumulate one row into result_table, keyed on the
     group, and ship every group accumulated so far. */
  int accumulate_group();
  int flush_groups();

  Parallel_scan::Worker_ctx *engine_ctx;
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
    This worker's single reused row buffer, PWT_CHUNK_ROWS * reclength bytes
    (reclength == the manager's result_table record size), allocated up front
    in init_parallel_workers. For each row of its chunk that passes the WHERE
    filter the worker projects the select list into result_table->record[0]
    (see worker_proj/result_table) and memcpy's that record image into this
    buffer. The manager and the workers share an identical result_table layout,
    so a byte-for-byte record copy reconstructs the result row on the manager
    side. It hands the buffer to the manager, blocks until the manager has
    drained it, then refills from the top. The worker and the manager never
    touch it at the same time, so it needs no per-row locking.
  */
  uchar           *batch_rows;
  uint            batch_count;   // rows the worker placed in batch_rows

  /*
    This worker's private result container: a tmp table whose columns are the
    query's select list (built in init_parallel_workers from the manager's
    result_table layout). The worker only uses its record buffer and fields --
    it projects the cloned select list into result_table->record[0] with
    Item::save_in_field and ships those bytes; it never writes rows through the
    storage engine.
  */
  TABLE           *result_table;
  /*
    Per-worker deep clone of the select list, one item per result_table field,
    with its Item_field leaves rebound to this worker's table copies. Each
    worker owns its own clones so the threads never share mutable Item state
    (null_value, cached results) while they evaluate concurrently. Created on
    the manager thread in init_parallel_workers; evaluated only by this worker.
    Every table's condition, the driving table's included, lives on the matching
    join_tabs[] entry as its select_cond.
  */
  Item            **worker_proj;
  uint            proj_count;

  /*
    Pre-aggregation: this worker's own clones of the query's aggregates, with
    their arguments rebound to its table copies. It accumulates its whole chunk
    into these and ships one row of partial values, which the manager folds into
    the query's own aggregates. Null when this scan ships rows instead; see
    pwt_manager::preagg.

    With a GROUP BY the chunk becomes one partial per group instead of one for
    the whole chunk, and the accumulation happens in result_table rather than in
    the aggregates themselves: each clone's result_field is a column of this
    worker's result_table, and reset_field()/update_field() start and extend the
    group's partial there. That is what the server's own end_update() does, and
    the same four aggregate kinds are accepted either way.
  */
  Item_sum        **worker_sums;

  /*
    Grouped pre-aggregation: this worker's GROUP BY key over its result_table,
    and the key buffer create_tmp_table() built alongside it.

    Every worker needs its own ORDER list rather than sharing the manager's:
    create_tmp_table() writes the key field and its position in the key buffer
    into each entry, so the entries belong to one table, and the worker fills the
    buffer through them from its own thread while the other workers fill theirs.
  */
  ORDER           *group_list;
  uchar           *group_buff;

  /*
    Multi-table join: this worker's private copy of every non-const join table
    (worker_tables[0] == our_scan_table, the parallel-scanned driving table;
    worker_tables[1 .. n_tables-1] are the tables joined after it, in join
    order). join_tabs[] carries the matching JOIN_TAB for each of them.
  */
  TABLE           **worker_tables;
  uint            n_tables;

  friend pwt_manager;
};


/*
  Class to create, manage and eventually destroy a "team" of worker threads.
*/
class pwt_manager : public Sql_alloc
{
public:
  pwt_worker        *workers;
  uint              nworkers;
  I_List<pwt_queued_event> parallel_messages;
  mysql_mutex_t     LOCK_pwt_thread;
  THD               *thd;
  JOIN_TAB          *scan_tab;
  /*
    The non-const join tables, in join order (mgr_tabs[0] == scan_tab, the
    parallel-scanned driving table). mgr_tables holds their TABLEs; together
    with each worker's worker_tables they form the manager->worker table map
    used to rebind the cloned conditions/refs/select list. Set up once, on the
    manager thread, in init_parallel_workers.
  */
  JOIN_TAB          **mgr_tabs;
  TABLE             **mgr_tables;
  uint              n_tables;
  /*
    Set under LOCK_pwt_thread when a worker fails to allocate a queued event.
    The manager surfaces a single ER_OUTOFMEMORY warning so the user sees
    that worker diagnostics were dropped instead of silently disappearing.
  */
  bool              messages_dropped;

  /*
    Streaming channel. Each worker (producer) fills its single reused row
    buffer (batch_rows) with the result rows it computed for its chunk and
    hands it to the manager (single consumer) by setting its batch_full flag;
    the manager (manager_collect_and_send) drains the buffer and sends each
    result row to the client as the batches arrive, instead of waiting for
    every worker to finish first.

    LOCK_data guards cur_worker, the workers' batch_full flags, active_workers
    and the flags below. COND_data_avail wakes the consumer when a worker
    fills its buffer or finishes; COND_data_space wakes a worker when the
    manager has drained its buffer so it may refill. Because each worker owns
    one buffer and blocks until it is drained, at most one batch per worker is
    ever outstanding -- the single buffer is the natural backpressure bound.
    EOF for the consumer is the state (no worker has batch_full set &&
    active_workers == 0).
  */
  mysql_mutex_t     LOCK_data;
  mysql_cond_t      COND_data_avail;
  mysql_cond_t      COND_data_space;
  pwt_worker        *cur_worker;      // worker whose buffer the consumer drains
  uint              cur_cursor;       // consumer's row index within cur_worker
  uint              reclength;        // result_table record image size (bytes)
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

  /*
    Result container shared (by layout) with every worker's result_table. The
    manager receives each worker result-row image into result_table->record[0]
    and copies its columns back into its own base-table records, from where the
    query's own items read them. result_tmp_param backs the
    create/instantiate/free of result_table and the per-worker copies.
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
  Copy_field       *copy_back;
  uint              n_copy_back;

  /*
    Pre-aggregation. When the query is an aggregate over the whole scan -- no
    GROUP BY, and every aggregate one Item_sum::direct_add() can fold a partial
    into -- each worker aggregates its own chunk and ships a single row holding
    one partial value per aggregate, rather than shipping every qualifying row
    for the manager to aggregate. The manager folds each partial in and lets the
    plan's own terminal send the result.

    That turns the manager's per-row work, which no number of workers reduces,
    from one row per qualifying row into one row per worker. It is the whole of
    what limits an aggregate query: measured, the drain is 29.6ns of strictly
    serial work per row.

    result_table then holds one column per aggregate instead of the base-table
    columns, and nothing is copied back: mgr_sums are the query's own aggregates
    and partial_items read the shipped values out of result_table for the
    direct_add() overload that takes an Item.

    With a GROUP BY the same thing happens once per group. A worker accumulates
    its chunk into an indexed temp table keyed on the group and ships one row per
    group, so result_table holds the base-table columns *and* the partials: the
    first n_ship_base columns are the shipped base columns, copied back exactly
    as they are for the row transport, and the rest are the partial values. The
    manager primes each aggregate with direct_add() and then calls the plan's own
    terminal per row, which is end_update(): it looks the group up in its own
    aggregation table and folds the partial in, because update_field() consumes
    the direct value instead of counting the row.
  */
  bool              preagg;
  bool              preagg_grouped;
  Item_sum        **mgr_sums;
  Item            **partial_items;
  uint              n_sums;
  uint              n_ship_base;    // shipped base columns before the partials
  bool              any_partial;    // a partial row arrived

  pwt_manager():
    workers(nullptr),
    nworkers(0),
    mgr_tabs(nullptr),
    mgr_tables(nullptr),
    n_tables(0),
    messages_dropped(false),
    cur_worker(nullptr),
    active_workers(0),
    fatal_error(false),
    kill_signal(NOT_KILLED),
    result_table(nullptr),
    result_tmp_param(nullptr),
    copy_back(nullptr),
    n_copy_back(0),
    preagg(false),
    preagg_grouped(false),
    mgr_sums(nullptr),
    partial_items(nullptr),
    n_sums(0),
    n_ship_base(0),
    any_partial(false),
    group_defn(nullptr),
    group_pos(nullptr),
    n_group(0),
    group_parts(0),
    group_length(0),
    group_null_parts(0),
    reaped(false)
    {}
  ~pwt_manager()
  {
    finalize_parallel_workers(current_thd, join);
  }
  int init_parallel_workers(THD *thd, JOIN *join, JOIN_TAB *scan_tab);
  void quiesce_workers();
  void finalize_parallel_workers(THD *thd, JOIN *join);
  bool handoff_batch(pwt_worker *worker);
  /*
    Consumer: drain result rows from the workers and send them to the client.
    Returns 0 on success (all rows sent), 1 on error.
  */
  int manager_collect_and_send(JOIN *join);
  void free_queue();

private:
  /* Copy the next worker result-row image into dst (reclength bytes).
     0 = row produced, -1 = end of data, 1 = error. */
  int drain_next_row(uchar *dst);
  /* Create + instantiate one result container from the column definition list
     'defn', keyed on 'group' when there is one. Returns true on error. */
  bool make_result_table(THD *thd, List<Item> &defn, ORDER *group, TABLE **out);
  /* Deep-clone this query's WHERE + select list for 'worker', rebinding the
     Item_field leaves to the worker's table copies. Returns true on error. */
  bool clone_worker_exprs(THD *thd, pwt_worker *worker);
  /* Open this worker's private copy of every non-const join table (into
     worker->worker_tables / our_scan_table). Returns true on error. */
  bool open_worker_tables(THD *thd, pwt_worker *worker);
  /* Build worker->worker_join: the JOIN the worker's tabs belong to. */
  bool setup_worker_join(THD *thd, pwt_worker *worker);
  /* Build worker->join_tabs: a copy of each of the manager's non-const
     JOIN_TABs, rebound to this worker. */
  bool setup_worker_tabs(THD *thd, pwt_worker *worker);
  /* Free the manager and per-worker result containers. */
  void free_result_tables(THD *thd);
  /* Decide what a worker ships and build the manager's result container. */
  bool build_result_layout(THD *thd, JOIN *join, ORDER *plan_group);
  /* Collect this query's aggregates into mgr_sums and give each of them a
     result_table column. */
  bool setup_preagg(THD *thd, JOIN *join);
  /* Give 'worker' its own clones of the aggregates to accumulate into. */
  bool clone_worker_sums(THD *thd, pwt_worker *worker);
  /* Hand each aggregate the partial value now in result_table, for whatever
     consumes a direct value next -- an aggregator_add() here, or update_field()
     inside the plan's terminal. */
  bool direct_add_partials();
  /* Fold the partial row now in result_table into the query's aggregates. */
  bool merge_partial_row();
  /* Build group_defn: the GROUP BY key expressed over the shipped layout. */
  bool make_group_defn(THD *thd, JOIN *join, ORDER *plan_group);
  /* One private copy of group_defn, for one result table. */
  ORDER *clone_group_defn(THD *thd);
  /* Point a worker's key at the columns of its own grouping table. */
  bool bind_worker_group(THD *thd, pwt_worker *worker);

  /* Clones of the shipped columns that define the result_table columns (kept so
     the manager and every worker build the identical result layout). */
  List<Item>        result_defn;

  /*
    Grouped pre-aggregation: the GROUP BY key over result_defn's items, and the
    key sizes create_tmp_table() needs for it. Every result table is built from a
    private copy of this list (clone_group_defn), because create_tmp_table()
    writes each table's own key fields into the entries it is given.

    The sizes are computed once, before any table exists, and re-applied to the
    param for every build. calc_group_buffer() sizes a group item that already
    has a temp-table field from the field and one that does not from the item, so
    asking it again after the first table was built -- which leaves a field on
    each item -- would answer differently for the second.
  */
  ORDER            *group_defn;
  uint             *group_pos;      // each key part's column in the layout
  uint              n_group;
  uint              group_parts, group_length, group_null_parts;

  JOIN              *join;            // the join these workers serve
  bool              stop;             // consumer wants producers to stop
  /*
    Set once the workers have been stopped and pthread_join'd (quiesce_workers).
    Workers read this join's source table (via their private handler), so they
    must be reaped before JOIN::join_free()->cleanup() frees that table;
    quiesce_workers is therefore called from join_free, and again (idempotently)
    from finalize.
  */
  bool              reaped;
};

/*
  Gate (sql_parallel_workers.cc): true when 'join' is a select-project query
  over the single non-const, parallel-scannable table 'scan_tab' that the
  workers can run themselves and ship final result rows for. Called from
  make_join_readinfo(); see JOIN::worker_side_parallel.
*/
extern bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab);

/*
  Drive worker-side execution from do_select(): spin up the workers (which run
  the query over their chunks), then collect and send the result rows.
  Returns 0 = handled (result sent), 1 = error, -1 = engine declined the
  parallel scan (caller should run the query serially instead).
*/
extern int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab);

#endif
