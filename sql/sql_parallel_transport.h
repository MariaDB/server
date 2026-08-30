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

#ifndef SQL_PARALLEL_TRANSPORT_H
#define SQL_PARALLEL_TRANSPORT_H

/**
  @file

  Result-row transport for parallel query: how a row a worker has finished
  with reaches the manager.

  A worker runs the whole select-project[-join] query over its own chunk of the
  driving table and produces finished result rows. It does not send them --
  the client connection belongs to the manager -- so every result row has to
  cross a thread boundary, and what it takes to cross is what lives here.

  Three parts: a shape, and the two ends.

    pwt_row_layout   The row shape both ends agree on: which columns travel,
                     the container whose record image defines their byte
                     layout, and the copies that put them back where the
                     manager's own query expects to read them. Manager-side,
                     built once, shared by reference with every sink.

    pwt_row_sink     The producing end. One per worker. Takes a finished row.

    pwt_row_source   The consuming end. One per manager. Yields the next row
                     of the whole result, in whatever order the transport
                     delivers -- the manager does not choose it and must not
                     depend on it.

  Both ends are abstract.

  What is deliberately NOT here: the state of the worker team as a whole --
  which workers are still running, whether one failed or was killed, whether
  the consumer has had enough. That belongs to pwt_manager, it outlives any
  one transport, and both ends read it through the manager they are handed.
  LOCK_data and COND_data_avail stay the manager's for the same reason: a
  consumer waiting for a row and a consumer waiting for the last worker to
  exit are waiting for the same event and must not be able to miss it.
  COND_data_space, in contrast, IS transport state -- it is the backpressure
  of this particular transport, and the temporary-table one has none.
*/

#include "mariadb.h"
#include "sql_class.h"

class JOIN;
class TMP_TABLE_PARAM;
class pwt_manager;
class pwt_worker;

/*
  Number of rows a worker packs into its batch buffer before handing it to the
  manager, so the channel mutex is touched once per PWT_ROW_GANULARITY rows
  instead of once per row.
  For batch transport it is the size of the hand-off unit.
  For temporary table transport, it is the interval that the worker uses to
  check the stop signal from the manager.
*/
#define PWT_ROW_GANULARITY 128


/*
  What a sink says about a row it was handed. STOP is not an error: the
  consumer has stopped wanting rows -- a satisfied LIMIT, a KILL, a failure in
  another worker -- and this producer should stop producing them.
*/
enum pwt_emit_result
{
  PWT_EMIT_OK=    0,
  PWT_EMIT_ERROR= 1,
  PWT_EMIT_STOP=  2
};


/**
  @brief
    One container of the layout: the record buffer a row is copied through, and
    the column descriptions it was built from.

  @description
    The two are one thing because they cannot be used apart. A heap container
    that fills has to be rebuilt on disk, and the rebuild is driven from
    param->start_recinfo/recinfo -- which create_tmp_table() allocates out of
    the table's own mem_root, so they describe that one table and are freed
    with it. A TMP_TABLE_PARAM shared between containers would describe only
    the last one built, point into a mem_root that may already be gone, and be
    written through by whichever thread rebuilt first. Pairing them is what
    makes that unrepresentable rather than merely avoided.
*/

struct pwt_row_container
{
  TABLE            *table;
  TMP_TABLE_PARAM  *param;

  pwt_row_container(): table(nullptr), param(nullptr) {}
  uchar *record() const { return table->record[0]; }
};


/**
  @brief
    The row shape the producing and consuming ends agree on.

  @description
    What travels is every base-table column the query reads, in table order --
    not the projected select list. The manager copies each column back into the
    field it came from in its own table instances, so that once a row has been
    received the manager's records hold what a serial scan would have left
    there and the query's own items read it directly, with nothing redirected.
    That is required rather than merely tidy: a temp table is filled through the
    Copy_field pairs create_tmp_table() builds, which hold raw Field pointers
    into the base tables, so re-pointing Items could not reach them.

    ship_list holds Item_fields over the manager's own fields. result_defn holds
    clones of them, and that clone list is what defines the column layout; the
    manager and every worker build an identical-layout container from it, which
    is what makes a byte-for-byte record image meaningful at the far end.
    copy_back is one Copy_field per shipped column, container field to the
    base-table field it came from.

    None of this is specific to how the bytes travel, which is why it is not
    part of either end: a temporary-table transport ships the same columns in
    the same order and copies them back the same way. Only reclength's role
    narrows -- see the note at the foot of the file.
*/

class pwt_row_layout
{
public:
  /*
    The manager's receiving container. Every worker holds an identical-layout
    container of its own, made by make_container() from the same definition.
  */
  pwt_row_container recv;

  List<Item>        ship_list;      // manager's Item_fields, in table order
  List<Item>        result_defn;    // clones of them: the column definition

  /*
    Container field -> the base-table field it was projected from, one per
    shipped column. n_copy_back is ship_list's length less the filler shipped
    by a query that reads no column at all, which has no destination.
  */
  Copy_field        *copy_back;
  uint              n_copy_back;

  uint              reclength;      // record image size, bytes
  JOIN              *join;          // the join this layout serves

  /*
    The partial-aggregate set: what a worker computes per group and the manager
    folds back into the query's own aggregates.

    When the workers pre-aggregate, a shipped row is the base columns *and* one
    column per aggregate holding that worker's partial for the group --
    n_ship_base of the first kind, then n_sums of the second. The base columns
    still travel because the manager's terminal builds the group key from its
    own base-table records, which copy_back fills; the partials travel beside
    them because that is what makes a worker's row worth one group rather than
    one source row.

    mgr_sums are the query's own aggregates, not clones: they are what the
    result is finally read from. partial_items read the shipped partials out of
    recv, for the direct_add() overload that takes an Item.

    n_sums is the partial-aggregate set size, and is what anything that has to
    walk the partials counts with -- the shipped columns after n_ship_base, the
    per-worker aggregate clones, and the merge.
  */
  uint              n_ship_base;
  uint              n_sums;
  Item_sum          **mgr_sums;
  Item              **partial_items;

  /*
    The GROUP BY key definition the workers accumulate on.
    create_tmp_table() writes the key field and its offset in the key buffer
    into each ORDER entry, so an entry belongs to one table and
    every container needs a copy of its own (clone_group_defn).

    group_pos[k] says which shipped column key part k covers letting a copy
    be re-pointed at its own container's fields.

    Determine group_parts, group_length, group_null_parts now rather than
    during create_tmp_table(), avoiding the need to re-derive them from
    re-pointed items.
  */
  ORDER             *group_defn;
  uint              *group_pos;
  uint              n_group;
  uint              group_parts, group_length, group_null_parts;

  /* Whether the workers pre-aggregate per group at all. */
  bool              grouped;

  pwt_row_layout():
    copy_back(nullptr), n_copy_back(0), reclength(0), join(nullptr),
    n_ship_base(0), n_sums(0), mgr_sums(nullptr), partial_items(nullptr),
    group_defn(nullptr), group_pos(nullptr), n_group(0),
    group_parts(0), group_length(0), group_null_parts(0), grouped(false),
    saved_write_set(nullptr)
  {}

  /*
    Work out what travels, build the definition, and build the manager's
    receiving container from it. Returns true on error (my_error() called).
  */
  bool build(THD *thd, JOIN *join_arg, TABLE **tables, uint n_tables,
             ORDER *plan_group);

  /*
    One private copy of group_defn, for one container to key on. Returns
    nullptr on error; only called when there is a key.
  */
  ORDER *clone_group_defn(THD *thd);

  /* Define the partial columns and the key they accumulate per. */
  bool build_aggregates(THD *thd, ORDER *plan_group);

  /*
    Hand each of the query's own aggregates the partial now in recv, so that
    whatever consumes a direct value next -- the plan's terminal, per group --
    folds it in instead of counting the row.
  */
  void direct_add_partials();

  /*
    One more container of this same layout, for a worker to project into, with
    column descriptions of its own. Returns true on error.
  */
  bool make_container(THD *thd, pwt_row_container *out,
                      ORDER *group= nullptr);
  void free_container(THD *thd, pwt_row_container *c);

  /* Where the consumer receives a record image. */
  uchar *recv_record() const { return recv.record(); }

  /*
    Put the columns of the record now in recv_record() back into the manager's
    own base-table records, where the query's items read them.
  */
  void copy_back_row();

  /*
    A record is about to be filled in place of the reader that would normally
    have filled it: clear the flags a reader would have left and make the
    fields writable. Returns true on error; end_receive() undoes it.
  */
  bool begin_receive(THD *thd, TABLE **tables, uint n_tables);
  void end_receive(TABLE **tables, uint n_tables);

  void cleanup(THD *thd);

private:
  /* The manager's own write_sets, saved across a drain by begin_receive(). */
  MY_BITMAP         **saved_write_set;
};


/*
  @brief
    The producing end of the transport. One per worker.

  @description
    The worker has projected a finished row into the record buffer of its own
    container and calls emit_row() with it. What happens next -- a copy into a
    shared buffer, a write through a storage engine -- is the transport's
    business and the worker does not know which.

    Every method here runs on the worker's own thread.

    Sql_alloc, because both ends are built with new (mem_root), and a class
    without it takes the plain placement new that constructs the object over
    the MEM_ROOT itself.
*/

class pwt_row_sink : public Sql_alloc
{
public:
  virtual ~pwt_row_sink() {}

  /*
    This producer is about to start, and this is the first thing that runs on
    the worker's own thread. A transport that owns a resource per producer
    claims it here rather than when the sink was made, because that happened on
    the manager's thread and some of what a resource needs -- a THD to account
    to, a handler to open -- is the worker's. Returns true on error.
  */
  virtual bool begin() { return false; }

  /* Take one finished row, whose record image is 'rec' (layout reclength
     bytes). Returns a pwt_emit_result. */
  virtual int emit_row(const uchar *rec)= 0;

  /*
    This producer has no more rows. Anything held back for batching goes now.
    Returns true on error.
  */
  virtual bool flush()= 0;

  /* Release what this end holds. Called with the worker joined. */
  virtual void cleanup()= 0;
};


/*
  @brief
    The consuming end of the transport. One per manager.

  @description
    Runs on the manager's thread. next_row() blocks when there is nothing to
    hand back yet and no producer has finished; it is the one place a manager
    waits for its workers, so it is also where a KILL of the manager and the
    failure or kill of a worker are noticed.
*/

class pwt_row_source : public Sql_alloc
{
public:
  virtual ~pwt_row_source() {}

  /*
    Copy the next result row's record image into dst (layout reclength bytes).
      0 = a row,  -1 = end of data,  1 = error (reported).
  */
  virtual int next_row(uchar *dst)= 0;

  /*
    Build the producing end for worker 'worker_nr' and register it with this
    source. A sink only ever makes sense against the source that made it, so
    the source is what makes them rather than a caller pairing the two by
    hand.

    'container' is that worker's own copy of the layout, the record buffer it
    projects a finished row into. A transport that only reads that record
    ignores it; one that materialises the rows makes it the store as well, so
    that projecting and storing are the same write -- and then it needs the
    column descriptions that come with it, to rebuild the store on disk when it
    fills.

    Returns nullptr on error (my_error() called).
  */
  virtual pwt_row_sink *make_sink(THD *thd, uint worker_nr,
                                  pwt_row_container *container)= 0;

  /*
    Give up any position held inside a producer's rows. Called before the
    workers are stopped, so nothing is left pointing into them.
  */
  virtual void release_position()= 0;

  /*
    The consumer wants no more rows. Release any producer blocked inside this
    transport so it can see the manager's stop request and leave. A transport
    in which a producer never waits for the consumer has nothing to do here,
    which is the point of the method: it names what disappears.
  */
  virtual void wake_producers()= 0;

  virtual void cleanup()= 0;
};


/*
    The batch transport: streaming channel from the worker to the manager
*/
class pwt_batch_source;


/*
  @brief  Producing end of the batch transport.

  @description

    One reused buffer of PWT_ROW_GANULARITY record images. The worker fills it,
    hands it to the manager by setting 'full', and blocks until the manager
    clears the flag; then it refills from the top. Because each worker owns one
    buffer and blocks until it is drained, at most one batch per worker is ever
    outstanding: the single buffer IS the backpressure, and there is no other.

    The buffer needs no per-row locking -- the worker and the manager never
    touch it at the same time, which is what 'full' arranges. 'full' itself is
    guarded by the manager's LOCK_data.
*/

class pwt_batch_sink : public pwt_row_sink
{
  pwt_manager      *manager;    // team status, and the lock 'full' is under
  pwt_batch_source *peer;       // the end that drains this buffer
  uchar            *rows;
  uint             count;       // rows placed in 'rows'
  uint             reclength;
  bool             full;        // ready for the manager; under LOCK_data

  /* Hand the filled buffer over and wait for it back.
     true = the consumer asked us to stop. */
  bool handoff();

public:
  pwt_batch_sink():
    manager(nullptr), peer(nullptr), rows(nullptr), count(0), reclength(0),
    full(false)
  {}

  bool init(pwt_manager *mgr, pwt_batch_source *peer_arg, uint reclength_arg);

  int  emit_row(const uchar *rec) override;
  bool flush() override;
  void cleanup() override;

  friend class pwt_batch_source;
};


/*
  @brief  Consuming end of the batch transport.

  @description
    Drains one worker's buffer at a time, in arrival order, releasing each
    worker to refill as soon as its buffer is empty. End of data is the state
    "no sink is full and no worker is still running", which is why the source
    has to see the team's active_workers as well as its own sinks.
*/

class pwt_batch_source : public pwt_row_source
{
  pwt_manager     *manager;
  pwt_batch_sink  **sinks;      // one per worker, in worker order
  uint            n_sinks;
  pwt_batch_sink  *cur;         // sink whose buffer we are draining
  uint            cur_cursor;   // row index within cur
  uint            reclength;
  bool            inited;

public:
  /*
    Signalled when the consumer has drained a buffer, so its worker may refill.
    This is the transport's own, not the manager's: it is the backpressure of
    this particular transport and nothing outside it waits on it. Paired with
    the manager's LOCK_data, which is what 'full' is guarded by.
  */
  mysql_cond_t    COND_data_space;

  pwt_batch_source():
    manager(nullptr), sinks(nullptr), n_sinks(0), cur(nullptr),
    cur_cursor(0), reclength(0), inited(false)
  {}

  bool init(THD *thd, pwt_manager *mgr, uint n_workers, uint reclength_arg);

  int  next_row(uchar *dst) override;
  pwt_row_sink *make_sink(THD *thd, uint worker_nr,
                          pwt_row_container *container) override;
  void release_position() override { cur= nullptr; }
  void wake_producers() override;
  void cleanup() override;
};


/*
  The temporary-table transport: a worker materialises its whole result set
  into its own container and the manager reads those containers back.

  The container is the one the worker already projects into, so projecting a
  row and storing it are the same write: save_in_field() fills record[0] and
  ha_write_tmp_row() keeps it. Nothing is copied on the producing side and
  nothing is handed over per row -- a producer never waits for the consumer,
  which is what this transport is for.

  A result set becomes the manager's when its producer flushes. The manager
  then scans each finished container in turn, so rows arrive in whole-worker
  runs rather than interleaved; the interface promises no order, and nothing
  above it depends on one.

  The container's TABLE::in_use follows whoever is using it: the worker takes
  it in begin() (the engine accounts a write to in_use's status, and a
  projection asks it for the session's time zone and sql_mode), and the manager
  takes it back before it scans. The hand-off is ordered by 'done' under the
  manager's LOCK_data, so only one of them ever holds it.

  NOT YET DONE, and known: when a worker's container fills, the server would
  normally convert it from heap to disk, and that conversion is not written to
  run on a worker thread. Nothing here does it, so a result set larger than the
  session's tmp table limit fails the statement instead of spilling. See
  ER_INTERNAL_ERROR in pwt_tmp_table_sink::emit_row().
*/

class pwt_tmp_table_source;


/*
  @brief  Producing end of the temporary-table transport.
*/

class pwt_tmp_table_sink : public pwt_row_sink
{
  pwt_manager           *manager;
  pwt_tmp_table_source  *peer;
  /*
    The worker's own container, which this transport makes its store as well:
    the projection fills its record and the write keeps it. Held as the pair
    rather than the table alone because rebuilding it on disk when it fills is
    driven from the column descriptions beside it.
  */
  pwt_row_container     *container;
  uint                  since_check; // rows written since we last read 'stop'
  /*
    Under the manager's LOCK_data. 'done' is this producer saying its result
    set is complete and may be read; 'taken' is the consumer saying it has
    picked it up, so it is not picked twice. The batch transport's 'full' is
    the same question asked once per buffer instead of once per producer.
  */
  bool                  done;
  bool                  taken;

  /* Has the consumer asked us to stop? Takes LOCK_data. */
  bool stop_requested();

public:
  pwt_tmp_table_sink():
    manager(nullptr), peer(nullptr), container(nullptr), since_check(0),
    done(false), taken(false)
  {}

  bool init(pwt_manager *mgr, pwt_tmp_table_source *peer_arg,
            pwt_row_container *container_arg);

  bool begin() override;
  int  emit_row(const uchar *rec) override;
  bool flush() override;
  void cleanup() override;

  friend class pwt_tmp_table_source;
};


/*
  @brief  Consuming end of the temporary-table transport.
*/

class pwt_tmp_table_source : public pwt_row_source
{
  pwt_manager         *manager;
  pwt_tmp_table_sink  **sinks;    // one per worker, in worker order
  uint                n_sinks;
  pwt_tmp_table_sink  *cur;       // sink whose container we are scanning
  bool                scan_open;  // ... and whether its scan is open
  uint                reclength;

  /* Find the next complete, unclaimed result set, waiting for one if the
     workers are still running. 0 = *out set, -1 = end of data, 1 = error. */
  int claim_next_result(pwt_tmp_table_sink **out);

public:
  pwt_tmp_table_source():
    manager(nullptr), sinks(nullptr), n_sinks(0), cur(nullptr),
    scan_open(false), reclength(0)
  {}

  bool init(THD *thd, pwt_manager *mgr, uint n_workers, uint reclength_arg);

  int  next_row(uchar *dst) override;
  pwt_row_sink *make_sink(THD *thd, uint worker_nr,
                          pwt_row_container *container) override;
  void release_position() override;
  /* Nothing to wake: a producer here never waits for the consumer. */
  void wake_producers() override {}
  void cleanup() override {}
};


/*
  @brief
    Build the transport a team of n_workers will use.

  @description
    The one place the implementation is chosen. There is one today, so there is
    nothing to choose between; when the temporary-table transport lands, this
    is where a query picks -- and the only place, which is what keeps the rest
    of the parallel-query code from learning which one it got.

  @return  the consuming end, or nullptr on error (my_error() called). Its
           sinks are made from it, one per worker, with make_sink().
*/
pwt_row_source *pwt_create_transport(THD *thd, pwt_manager *mgr,
                                     uint n_workers, uint reclength);

#ifdef HAVE_PSI_INTERFACE
void pwt_transport_init_psi_keys(void);
#else
static inline void pwt_transport_init_psi_keys(void) {}
#endif


/*
  ---------------------------------------------------------------------------
  Why the two ends are abstract, and what is still open
  ---------------------------------------------------------------------------

  The shape of the interface above is not arbitrary. Each of these is something
  the two transports answer differently, and the reason it is a method rather
  than an assumption:

  - emit_row() may return PWT_EMIT_ERROR for reasons that have nothing to do
    with the consumer. Writing a row can fail on its own -- the store filling,
    out of space, a duplicate key once the container carries a group index --
    where a memcpy into a buffer cannot.

  - flush() is only a partial buffer to the batch transport. To a materialising
    one it is the point where the result set becomes the manager's at all,
    which is the whole hand-off.

  - next_row() is free to return rows in any order and to block for as long as
    it likes. The batch transport blocks for one batch; the temporary-table one
    blocks until a whole worker has finished. The manager's drain loop makes no
    assumption either way, and must not be given one.

  - release_position() exists because the consumer can stop early (a KILL, a
    failed send) while positioned inside a producer's rows, and the producers
    are about to be reaped. For the batch transport that is dropping a pointer;
    for the temporary-table one it also ends an open scan on a container that
    is about to be freed.

  - wake_producers() names what disappears: it releases a producer blocked
    waiting for the consumer, and the temporary-table transport has none to
    release.

  - reclength stays in the layout rather than in the batch classes. A
    materialising transport does not measure a hand-off in record images, but
    it still moves one record image from a worker's container into the
    manager's, so the size is still the layout's business.

  Still open for the temporary-table transport:

  - Spilling a container to disk. The column descriptions needed to rebuild one
    are in place now, one set per container (pwt_row_container). What is not is
    a thread to do it on: create_internal_tmp_table_from_heap() re-opens the
    table, and Aria's maria_open() binds the handle to the opening thread's
    my_thread_var, which destroy_background_thd() frees when that worker's THD
    goes. Run on a worker, it leaves the manager scanning through a dangling
    pointer. The candidates are to delegate the rebuild to the manager (which
    also wants a cross_thread flag threaded through open_tmp_table()), to give
    each worker a second container built on disk from the start, or to choose
    the on-disk engine up front from the optimizer's row estimate.

  - Accounting. tmp_space_used and the created-tmp-table counters are per-THD,
    and a worker's THD is destroyed before the manager reports anything; they
    will have to travel the way the status counters already do.

  What the temporary-table transport gives up, and what it buys:

  - Gives up streaming. Nothing reaches the client until a worker has finished,
    and an early-out cannot stop work that has already been done.

  - Buys the removal of the only place a worker blocks on the manager. A worker
    that never waits for the consumer can be released the moment its chunk is
    done, which is what a worker pool would need, and it removes the coupling
    that makes a slow client hold worker threads.

  - Buys a result set the manager can read more than once, in an order of its
    choosing, which is what merging per-worker aggregates or sorted runs needs.
*/

#endif /* SQL_PARALLEL_TRANSPORT_H */
