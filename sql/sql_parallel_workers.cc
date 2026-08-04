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
        pwt_manager::free_queue
                helper for error conditions
        pwt_manager::init_parallel_workers
                Initialise our parallel worker threads
        pwt_init_psi_keys
                initialize PSI keys
        worker_produce_chunks / worker_scan_table_to_manager
                producer: scan the source table and stream rows to the manager
                a batch at a time through the worker's reused raw row buffer
        pwt_manager::handoff_batch
                producer: hand the filled row buffer to the manager and block
                until it is drained
        parallel_scan_read_next
                consumer: copy raw records from the worker row buffers
        pwt_manager::finalize_parallel_workers
                stop the workers, reap them, surface diagnostics, tear down
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
  { &key_mutex_pwt_LOCK_thread,      "pwt_manager::LOCK_pwt_thread",   0},
  { &key_mutex_pwt_LOCK_worker,      "pwt_worker::LOCK_worker",           0},
  { &key_mutex_pwt_LOCK_data,        "pwt_manager::LOCK_data",         0},
};

static PSI_cond_key key_COND_pwt_data_avail, key_COND_pwt_data_space;

static PSI_cond_info all_pwt_conds[]=
{
  { &key_COND_pwt_data_avail,  "pwt_manager::COND_data_avail",      0},
  { &key_COND_pwt_data_space,  "pwt_manager::COND_data_space",      0},
};

static PSI_memory_info all_pwt_memory[]=
{
  { &key_memory_pwt_queued_event,  "pwt_queued_event",          0},
  { &key_memory_pwt_error_message, "pwt_error_message",         0},
  { &key_memory_pwt_workers,       "pwt_manager::workers",   0},
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
#ifdef WITH_PARTITION_STORAGE_ENGINE
         !table->part_info &&
#endif
         (table->file->ha_table_flags() & HA_CAN_PARALLEL_SCAN);
}


/**
  @brief
    Discount a full-table-scan cost when the table is eligible to be scanned by
    parallel workers.

  @description
    When parallel query is enabled the first non-const table can be scanned by
    N worker threads, each reading a disjoint partition concurrently while the
    manager runs the rest of the join, so the row (full-scan) components of
    'cost' -- I/O, CPU and row-copy -- are divided among them. The index
    components are left untouched: this only ever discounts a full table scan.

    N is not parallel_worker_threads. A chunk cannot be smaller than a leaf
    page, so a table of fewer leaf pages than there are threads cannot occupy
    them all whatever is asked for, and a worker with no chunk reads nothing
    (see init_parallel_workers, which starts no more workers than chunks). The
    engine supplies that ceiling. Taking the request at face value instead is
    the difference between believing a scan of a hundred pages is fifty times
    cheaper and its being at best a hundredth of that off, which is enough to
    prefer a parallel full scan over a perfectly good index.

    Two costs the division does not express are added back. The worker path is
    more expensive per row than the serial one, because rows are copied into a
    batch buffer, handed over under a mutex and re-read by the manager. And
    each worker has to be created, with its own THD, table instances, cloned
    items and row buffer, measured at some 22 microseconds. The setup term is
    what makes the optimizer decline parallelism for a query too small to
    amortise it rather than relying on a threshold.

    Eligibility mirrors the runtime gate in make_join_readinfo() exactly
    (engine support, no blob-backed columns, not fulltext-searched, a real base
    table, not partitioned), so the optimizer never discounts a scan that will
    not actually run in parallel. The caller is responsible for invoking this
    only for the driving table (idx == const_tables), the single position a
    parallel scan applies to, and 'cost' must be the caller's local copy, not
    the cached per-table estimate.

  @return
    true   the cost was scaled (table is parallel-scan eligible)
    false  no change (parallel scan disabled, table not eligible, or the
           table cannot be divided among two or more workers)
*/

/*
  Fewest workers worth using. Below this the parallel path is slower than the
  serial one, and not marginally: measured on a release build, 4000000 rows,
  scan bound by I/O, one worker runs at 0.36 of the serial speed and two at 0.59.
  Three is where it crosses over -- 1.42 -- and it climbs to 1.74 at five, which
  on the six-core machine it was measured on is one worker per core with the
  manager taking the last.

  The cause is not the scan. Every row a worker reads is copied into a batch,
  handed over under a mutex and read again by the manager, which drains one
  worker at a time; with one or two workers there is no slack in that exchange
  and the manager and workers alternate rather than overlap. So this is a floor
  on the number of workers, not on the size of the table, and it is a constant
  because it is a property of the exchange rather than of the machine.
*/
#define PARALLEL_QUERY_MIN_WORKERS 3

uint parallel_scan_worker_count(THD *thd, TABLE *table)
{
  uint n= thd->variables.parallel_worker_threads;
  if (n < 2 ||                                   // disabled, or no speed-up
      !table_can_be_parallel_scanned(table))
    return 0;

  /* No more workers than the engine will have chunks to give them. */
  if (const size_t chunks= table->file->pscan_chunk_count_estimate())
    set_if_smaller(n, (uint) chunks);
  if (n < 2)
    return 0;                        // one chunk: nothing to divide
  return n;
}


uint scale_cost_for_parallel_scan(THD *thd, TABLE *table, ALL_READ_COST *cost)
{
  const uint n= parallel_scan_worker_count(thd, table);
  if (!n)
    return 0;

  /*
    Both factors are session variables rather than constants because they are
    measured quantities, and measuring them wants sweeping them without a
    rebuild: parallel_query_row_cost_ratio and parallel_query_setup_cost.
  */
  const double factor= thd->variables.parallel_query_row_cost_ratio /
                       (double) n;
  cost->row_cost.io  *= factor;
  cost->row_cost.cpu *= factor;
  cost->copy_cost    *= factor;
  cost->row_cost.cpu+= n * thd->variables.parallel_query_setup_cost;
  return n;
}


/*
  @brief
    How much of a joined table's cost the workers divide between them.

  @description
    A worker does not only scan its chunk of the driving table: it runs the whole
    join over that chunk, so the work of every table joined after the driving one
    is divided between the workers just as the scan is. The optimizer costs those
    tables one at a time, after the driving table's access has been chosen, so
    this is asked once per table and answers with the divisor to apply to what
    that table costs.

    It is deliberately not applied while the access method for the table is being
    chosen, only to the cost recorded for the plan. Dividing every candidate by
    the same number cannot change which one is cheapest, so doing it earlier would
    buy nothing and would risk changing a choice by scaling one candidate's cost
    components and not another's. The driving table is the exception and is scaled
    while its access is chosen, because there the division is what can make a full
    scan worth more than an index -- which is a choice, and the point.

  @param  positions     the plan prefix, whose first non-const entry is the
                        driving table and carries the worker count that was used
                        to scale it
  @param  const_tables  index of the driving table in positions

  @return the number of workers the join is being divided between, or 0 if this
          plan's driving table is not being parallel-scanned and nothing is.
*/

uint parallel_join_divisor(const POSITION *positions, uint const_tables)
{
  const POSITION *driver= positions + const_tables;
  /*
    parallel_workers is only left non-zero when the access finally chosen for the
    driving table was the scan that was costed as parallel, so there is nothing
    further to check here: a driving table that ended up on an index carries 0.
  */
  return driver->parallel_workers;
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

bool pwt_manager::handoff_batch(pwt_worker *worker)
{
  DBUG_ENTER("pwt_manager::handoff_batch");
  mysql_mutex_lock(&LOCK_data);
  if (stop)
  {
    mysql_mutex_unlock(&LOCK_data);
    DBUG_RETURN(true);
  }
  worker->batch_full= true;
  mysql_cond_signal(&COND_data_avail);                  // wake the consumer
  while (worker->batch_full && !stop)
  {
    mysql_cond_wait(&COND_data_space, &LOCK_data);
    DBUG_PRINT("info", ("worker wakes"));
  }
  bool stopped= stop;
  mysql_mutex_unlock(&LOCK_data);
  DBUG_RETURN(stopped);
}


static void close_one_worker_table(TABLE **t)
{
  if (*t)
  {
    (*t)->file->update_global_table_stats();
    closefrm(*t);
    my_free(*t);
    *t= nullptr;
  }
}


/*
  @brief
    Take the engine's per-table counters out of the tables while they are still
    open, so the manager can add them to what ANALYZE reads.
*/

void pwt_worker::snapshot_table_stats()
{
  for (uint i= 0; i < n_tables; i++)
    if (ha_handler_stats *hs= worker_tables[i]->file->handler_stats)
      tab_hstats[i].add(hs);
}


void pwt_worker::close_worker_tables()
{
  if (worker_tables)
    for (uint i= 0; i < n_tables; i++)
      close_one_worker_table(&worker_tables[i]);
  our_scan_table= nullptr;       // == worker_tables[0], closed above
}


/*
  @brief
    Visitor that rebinds an Item_field from a manager join table to the
    worker's private copy of that table.

  @description
    A deep_copy of a WHERE/ref/select-list Item tree carries Item_field nodes
    whose Field* still points into a manager join table. We walk the clone with
    enumerate_field_refs_processor and, for every field that belongs to one of
    the parallel-scanned tables (from_tables[i]), repoint it at the same column
    of the worker's copy (to_tables[i]). Fields of any other table (e.g. a const
    table read once during optimization) are left untouched -- they are
    read-only and safely shared across workers.
*/
/*
  @brief
    Whether 'clone' reaches any item object that 'src' reaches too.

  @description
    A copy is only usable by a worker if it shares no node at all with the item
    it came from. Several classes implement deep_copy() as a shallow copy while
    still holding child items -- every Item_cache, Item_outer_ref and
    Item_copy_string -- so their copy keeps pointing at the original's children.

    Two things then go wrong with a shared node. The worker repoints the
    Item_field leaves of its copy at its own tables, so a shared leaf moves the
    manager's own item onto a worker's table, and the next worker moves it again,
    which is what the in_use assertion in Field::val_int() catches. And a node
    that is not a leaf carries evaluation state, so several workers evaluating
    one shared object at once tear it: a shared Item_cache under LEAST() left a
    MYSQL_TIME half written and tripped the assertion in Time::Time().

    Walking with find_item_processor asks whether a tree reaches one given
    object, so collecting the copy's nodes and asking that of the original
    covers both, and covers the classes not yet met rather than the three above.
*/
static bool pwt_clone_shares_nodes(Item *src, Item *clone)
{
  List<Item> clone_nodes;
  if (clone->walk(&Item::collect_all_items_processor, &clone_nodes, 0))
    return true;                      // could not collect them, assume the worst

  List_iterator_fast<Item> it(clone_nodes);
  Item *node;
  while ((node= it++))
    if (src->walk(&Item::find_item_processor, (void*) node, 0))
      return true;
  return false;
}


/*
  @brief
    Whether this item can be copied into something a worker can own.

  @description
    Clonable, and the copy independent of the original (see
    pwt_clone_shares_nodes). Called from the gate, at optimize time, where a
    query that fails either test still falls back to serial execution for free.
*/
static bool pwt_item_is_clonable(THD *thd, Item *item)
{
  Item *clone= item->deep_copy_with_checks(thd);
  return clone && !pwt_clone_shares_nodes(item, clone);
}


class Pwt_field_rebinder : public Field_enumerator
{
  TABLE **from_tables, **to_tables;
  uint  n;
public:
  Pwt_field_rebinder(TABLE **from, TABLE **to, uint n_arg)
    : from_tables(from), to_tables(to), n(n_arg) {}
  void visit_field(Item_field *item) override
  {
    if (!item->field)
      return;
    for (uint i= 0; i < n; i++)
      if (item->field->table == from_tables[i])
      {
        item->field= to_tables[i]->field[item->field->field_index];
        /*
          Item_field::save_in_field() (used when this field is a top-level
          projection item) copies from result_field, not field. The worker's
          value for this column lives in the rebound source field, so point
          result_field there too. Harmless for fields used only in val_*()
          (conditions, ref values and sub-expressions read field directly).
        */
        item->result_field= item->field;
        return;
      }
  }
};


/*
  @brief
    The whole condition this table has to be filtered by, whatever the plan did
    with it.

  @description
    When the optimizer pushes part of a condition into the engine,
    push_index_cond() leaves tab->select_cond holding only the remainder and
    keeps the original in tab->pre_idx_push_select_cond. The pushed half then
    lives in handler::pushed_idx_cond, which belongs to the manager's handler:
    a worker reads through its own, opened by open_table_from_share() with no
    condition pushed into it, so filtering by select_cond alone would apply the
    pushed half nowhere and return rows the serial plan rejects.

    A worker therefore filters by the pre-pushdown condition and does the whole
    job itself. That gives up what the pushdown was for -- the engine no longer
    rejects an index entry before the row is read -- so a worker does more
    clustered-index work per match than the serial plan does. Correctness first,
    and the alternative is to refuse these plans and lose the parallel scan
    altogether. Pushing a clone onto the worker's own handler would recover it,
    and wants the worker to hold a real JOIN_TAB to hang the key number off.

    There is a second place a condition can go. When an inner table is read
    through a BNL/BNLH join buffer, JOIN_TAB::make_scan_filter() copies the
    conjuncts that need only this table into cache_select->cond, for the buffer
    to apply as it fills, and JOIN_TAB::remove_redundant_bnl_scan_conds() then
    *removes* those conjuncts from select_cond -- it can empty it altogether. A
    worker uses no join buffer, so for a cached table select_cond is not the
    whole condition and filtering by it alone lets through everything the buffer
    would have rejected.

    Both halves are reported together, and the gate and both clone sites go
    through here, so the condition the gate approves is always the condition a
    worker ends up evaluating.
*/

static void pwt_table_conds(JOIN_TAB *tab, Item **cond, Item **cache_cond)
{
  *cond= tab->pre_idx_push_select_cond ? tab->pre_idx_push_select_cond
                                       : tab->select_cond;
  *cache_cond= tab->cache_select ? tab->cache_select->cond : nullptr;
}




/*
  @brief
    Deep-clone an Item tree and rebind its join-table field references to the
    worker's private table copies.

  @return  the clone, or nullptr if the item is not clonable (deep_copy
           declined) or 'src' was nullptr.
*/

static Item *pwt_clone_rebind(THD *thd, Item *src,
                              TABLE **from, TABLE **to, uint n)
{
  if (!src)
    return nullptr;
  Item *clone= src->deep_copy_with_checks(thd);
  if (!clone)
    return nullptr;

  /*
    1. Rebind every field reference to the worker's table copy. The fields stay
       fixed and keep their column position, so no name re-resolution happens.
       The gate rejected anything whose copy is not independent, so the walk
       below can only reach fields this clone owns.
  */
  DBUG_ASSERT(!pwt_clone_shares_nodes(src, clone));
  Pwt_field_rebinder rebinder(from, to, n);
  clone->walk(&Item::enumerate_field_refs_processor, (void*) &rebinder, 0);

  /*
    2. deep_copy duplicates the tree structurally but does not rebuild the
       fix-time caches some predicates hold (e.g. Item_func_in's in_vector,
       a comparator's collation/cmp_item) -- those would still compare against
       the original args and read the wrong table. Reset those caches on the
       non-field items (cleanup_excluding_fields_processor leaves the rebound,
       still-fixed fields alone) and re-fix the tree so they rebuild against the
       rebound args. A bare field clone is already fixed and needs no re-fix.
       (Same protocol the optimizer uses for re-evaluable clones, e.g.
       opt_rewrite_remove_casefold.cc.)
  */
  clone->walk(&Item::cleanup_excluding_fields_processor, (void*) nullptr, 0);
  if (!clone->fixed() && clone->fix_fields(thd, &clone))
    return nullptr;
  return clone;
}


/*
  @brief
    Clone both halves of one table's condition and rebind them to the worker's
    tables, ANDed together when there are two.

  @return  true on error. *out is the clone, or NULL if the table has no
           condition at all.
*/

static bool pwt_clone_table_conds(THD *thd, JOIN_TAB *tab,
                                  TABLE **from, TABLE **to, uint n,
                                  Item **out)
{
  Item *cond, *cache_cond, *c= nullptr, *cc= nullptr;
  pwt_table_conds(tab, &cond, &cache_cond);
  *out= nullptr;

  if (cond && !(c= pwt_clone_rebind(thd, cond, from, to, n)))
    return true;
  if (cache_cond && !(cc= pwt_clone_rebind(thd, cache_cond, from, to, n)))
    return true;
  if (!c || !cc)
  {
    *out= c ? c : cc;
    return false;
  }

  /*
    Both clones are already fixed, so the conjunction needs no more than
    quick_fix_field() -- which is what remove_redundant_bnl_scan_conds() itself
    does when it rebuilds a condition out of fixed conjuncts. The worker only
    ever evaluates this item, so the fix-time caches Item_cond::fix_fields()
    would rebuild (used_tables, not_null_tables) are not read.
  */
  Item_cond_and *both= new (thd->mem_root) Item_cond_and(thd, c, cc);
  if (!both)
    return true;
  both->quick_fix_field();
  *out= both;
  return false;
}


/*
  @brief
    Build a worker-bound clone of a ref-access descriptor (mirrors the store_key
    setup in create_ref_for_key). The cloned ref has its own key buffer and a
    store_key per key part that evaluates the worker-bound key-value items into
    that buffer; cp_buffer_from_ref() then builds the lookup key from it.

  All key parts (even constant ones) get a runtime store_key -- we set
  const_ref_part_map to 0 so cp_buffer_from_ref recomputes every part each
  lookup. Re-evaluating a constant per lookup is cheap and avoids the
  "filled once at setup" bookkeeping.

  @return  true on error.
*/

static bool clone_table_ref(THD *thd, TABLE_REF *src, TABLE *wtable,
                            TABLE **from, TABLE **to, uint n, TABLE_REF *dst)
{
  const uint kp= src->key_parts;
  const uint len= src->key_length;
  KEY *keyinfo= wtable->key_info + src->key;

  dst->key_parts= kp;
  dst->key_length= len;
  dst->key= src->key;
  dst->key_err= 1;
  dst->has_record= FALSE;
  dst->null_rejecting= src->null_rejecting;
  dst->disable_cache= FALSE;
  dst->null_ref_part= NO_REF_PART;
  dst->null_ref_key= nullptr;
  dst->const_ref_part_map= 0;            // recompute every part each lookup
  dst->uses_splitting= FALSE;
  dst->use_count= 0;
  dst->cond_guards= nullptr;             // gated: no subquery trigger guards
  dst->depend_map= src->depend_map;

  if (!(dst->key_buff= thd->calloc<uchar>(ALIGN_SIZE(len) * 2)) ||
      !(dst->key_copy= thd->alloc<store_key*>(kp + 1)) ||
      !(dst->items= thd->alloc<Item*>(kp)))
    return true;
  dst->key_buff2= dst->key_buff + ALIGN_SIZE(len);

  store_key **ref_key= dst->key_copy;
  uchar *key_buff= dst->key_buff;
  for (uint i= 0; i < kp; i++)
  {
    Item *it= pwt_clone_rebind(thd, src->items[i], from, to, n);
    if (!it)
      return true;
    dst->items[i]= it;

    KEY_PART_INFO *kpi= keyinfo->key_part + i;
    const uint maybe_null= MY_TEST(kpi->null_bit);
    Item *real= it->real_item();
    store_key *sk;
    if (real->type() == Item::FIELD_ITEM)
      sk= new store_key_field(thd, kpi->field, key_buff + maybe_null,
                              maybe_null ? key_buff : 0, kpi->length,
                              ((Item_field*) real)->field, real->full_name());
    else
      sk= new store_key_item(thd, kpi->field, key_buff + maybe_null,
                             maybe_null ? key_buff : 0, kpi->length, it, FALSE);
    if (!sk)
      return true;
    *ref_key++= sk;
    key_buff+= kpi->store_length;
  }
  *ref_key= nullptr;                     // end marker
  return false;
}




/*
  The worker whose thread we are on. sub_select() drives the join through
  function pointers whose signatures are fixed -- READ_RECORD::Read_func takes a
  READ_RECORD and Next_select_func takes a JOIN and a JOIN_TAB -- and neither
  reaches the pwt_worker, so the callbacks below find it here. A worker is
  exactly one thread, which is what makes this sound; the pattern is mysqld.cc's
  THR_THD. Set for the length of worker_run_query() and cleared after, so a
  callback reached from anywhere else finds nothing rather than a stale worker.
*/
static thread_local pwt_worker *pwt_self;

/* See the definition below can_run_query_in_workers(). */
static bool pwt_aggregates_supported(THD *thd, JOIN *join);


/* Next row of this worker's chunk of the driving table. */
int pwt_worker::pscan_next_row()
{
  return our_scan_table->file->ha_pscan_get_next_row(engine_ctx);
}


/*
  A fully joined row. Project the worker's clone of the select list into the
  shared record layout and copy that image into the batch: the manager only
  concatenates the images, so the projection happens once, on the thread that
  produced the row.
*/
int pwt_worker::emit_joined_row()
{
  for (uint i= 0; i < proj_count; i++)
    worker_proj[i]->save_in_field(result_table->field[i], false);

  if (manager->fatal_error)                     // projection raised an error
    return 1;

  memcpy(batch_rows + (size_t) batch_count * manager->reclength,
         result_table->record[0], manager->reclength);

  if (++batch_count == PWT_CHUNK_ROWS)
  {
    if (manager->handoff_batch(this))            // manager asked us to stop
      return 2;
    batch_count= 0;                              // drained; refill
  }
  return 0;
}


/*
  @brief
    Read the next row of this worker's chunk of the driving table.

  @description
    The driving table's record source. Every other table in the join keeps the
    one make_join_readinfo() chose, because those read through the worker's own
    handler and its own ref; only this one has to come from the engine's chunk
    reader instead of a plain scan.

  @return  0 a row, -1 end of this worker's chunks, > 0 error. The contract
           READ_RECORD::Read_func has, see rr_handle_error().
*/
static int pwt_pscan_read_record(READ_RECORD *info)
{
  int err= pwt_self->pscan_next_row();
  if (!err)
    return 0;
  if (err == HA_ERR_END_OF_FILE)
    return -1;
  if (info->print_error)
    info->table->file->print_error(err, MYF(0));
  return 1;
}


/*
  @brief
    Point the driving table's JOIN_TAB at the chunk reader.

  @return  0 always: pscan_init_worker() is called once by worker_run_query()
           before the join starts, not here, because a failure to get a chunk
           has to be told apart from a failure to read one.
*/
static int pwt_pscan_init_read_record(JOIN_TAB *tab)
{
  /*
    Only the row-fetching half. setup_worker_tabs() has already put the rest in
    place, unlock_row included, and evaluate_join_record() calls that.
  */
  tab->read_record.read_record_func= pwt_pscan_read_record;
  tab->read_record.read_record_func_and_unpack_calls= pwt_pscan_read_record;

  /*
    And read the first row, which is what read_first_record() means -- see the
    tail of join_init_read_record(). Returning without reading leaves
    evaluate_join_record() to run once on a record buffer nothing has filled,
    which shows up as one extra row per worker.
  */
  return tab->read_record.read_record();
}


/*
  @brief
    What the worker does with a fully joined row: the end of its nested loop.

  @description
    Stands where end_send() stands in a serial plan. The manager, not this, is
    what talks to the client.

  @return  NESTED_LOOP_OK to keep going, NESTED_LOOP_QUERY_LIMIT when the manager
           has asked this worker to stop, NESTED_LOOP_ERROR on a failed
           projection.
*/
static enum_nested_loop_state pwt_end_send(JOIN *join, JOIN_TAB *join_tab,
                                           bool end_of_records)
{
  DBUG_ENTER("pwt_end_send");
  if (end_of_records)
    DBUG_RETURN(NESTED_LOOP_OK);

  switch (pwt_self->emit_joined_row()) {
  case 0:  DBUG_RETURN(NESTED_LOOP_OK);
  case 2:  DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT);
  default: DBUG_RETURN(NESTED_LOOP_ERROR);
  }
}


/**
  @brief
    Run the query over the worker's private chunk of the driving table, joining
    the other tables, and stream the *result* rows to the manager a batch at a
    time through the worker's reused row buffer.

  The worker scans its own copy of the driving table (our_scan_table, opened
  with in_use == this worker's thd) so the workers scan concurrently with no
  shared-scan lock. The join itself is the server's own nested loop:
  sub_select() is driven over this worker's JOIN_TABs, reading the driving table
  through the engine's chunk reader and every other table through this worker's
  handler, and each fully joined row leaves the last table through
  pwt_end_send(), which projects the select list into result_table and ships that
  record image. The manager (manager_collect_and_send) only concatenates these
  final rows.

  @return
  0 on success, or a handler error code. A clean stop requested by the manager
  (handoff_batch -> stop) also returns success: the manager is done, not in
  error.
*/

int pwt_worker::worker_run_query()
{
  DBUG_ENTER("pwt_worker::worker_run_query");
  TABLE *src= our_scan_table;
  pwt_manager *mgr= manager;
  const uint nt= n_tables;
  int err= 0;
  uint i;

  // the source tables were marked in open_worker_tables; this one is ours
  result_table->use_all_columns();         // we write every result column

  /*
    Adopt the manager's snapshot before touching any table. We run in our own
    THD, hence in our own transaction, so without this every worker would open
    its own read view at its first read: the workers, and the manager they work
    for, could each see a different version of the tables, and the chunk
    boundaries the manager's engine computed would not even belong to the
    snapshot we scan. The manager pinned its snapshot in
    pscan_init_coordinator() before any worker was created, and holds it until
    the workers have been reaped (quiesce_workers), which is also what keeps
    purge from removing the versions we still need.

    This covers every table we read, not just the parallel-scanned one: the
    snapshot belongs to the transaction, so the inner tables of the join are
    read at the same point in time as the driving table.
  */
  if (ha_clone_consistent_snapshot(thd, mgr->thd))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "parallel worker: cannot read the manager's snapshot");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }

  /*
    Our handles bypassed lock_tables(), so take the engine-level read lock on
    every table ourselves; InnoDB needs this to register a table with its trx
    before reading it. All-or-nothing: unlock the locked prefix on failure.
  */
  uint locked= 0;
  while (locked < nt)
  {
    if ((err= worker_tables[locked]->file->ha_external_lock(thd, F_RDLCK)))
    {
      while (locked--)
        worker_tables[locked]->file->ha_external_lock(thd, F_UNLCK);
      DBUG_RETURN(err);
    }
    locked++;
  }

  // Initialise the index on each ref/eq_ref inner table; reused across lookups.
  for (i= 1; i < nt; i++)
  {
    JOIN_TAB *it= &join_tabs[i];
    if ((it->type == JT_EQ_REF || it->type == JT_REF) &&
        (err= it->table->file->ha_index_init(it->ref.key, it->sorted)))
      break;
  }

  if (!err && !(err= src->file->pscan_init_worker(engine_ctx)))
  {
    batch_count= 0;

    /*
      Run the join the way do_select() runs it: once to produce the rows, then
      once more to signal end of records, which is what lets an operator that
      buffers flush. The chunk reader, the conditions, the refs and the trackers
      all hang off this worker's own JOIN_TABs, so the executor never touches
      the manager's. pwt_end_send() takes each finished row.
    */
    pwt_self= this;
    enum_nested_loop_state rc= sub_select(worker_join, join_tabs, FALSE);
    if (rc >= NESTED_LOOP_OK && !thd->killed)
      rc= sub_select(worker_join, join_tabs, TRUE);
    pwt_self= nullptr;

    if (rc == NESTED_LOOP_ERROR)
      err= thd->is_error() ? thd->get_stmt_da()->sql_errno() : HA_ERR_GENERIC;
    else if (thd->killed && !thd->is_error())
      my_error(ER_QUERY_INTERRUPTED, MYF(0));

    src->file->pscan_end_worker();

    /*
      Hand off the final partial batch. A stop the manager asked for
      (NESTED_LOOP_QUERY_LIMIT) is not an error -- it is done with us -- and a
      late one here is ignored for the same reason.
    */
    if (!err && !thd->killed && !mgr->fatal_error && batch_count)
      mgr->handoff_batch(this);
  }
  else if (err == HA_ERR_END_OF_FILE)
    err= 0;

  // end any open index/rnd scans (no-op for tables left in NONE state), unlock
  for (i= 1; i < nt; i++)
    join_tabs[i].table->file->ha_index_or_rnd_end();
  for (i= 0; i < nt; i++)
    worker_tables[i]->file->ha_external_lock(thd, F_UNLCK);
  DBUG_RETURN(err);
}


/**
   @brief  Run the query for this worker, ship the result rows to the manager,
           and tidy up. Entry point for worker_run_query.
*/

void pwt_worker::worker_run_query_to_manager()
{
  DBUG_ENTER("pwt_worker::worker_run_query_to_manager");

  pwt_manager *mgr= manager;
  int err= worker_run_query();

  /*
    End the worker's read transaction now, while we are still on the worker
    thread. destroy_background_thd() -> THD::cleanup() rolls the transaction
    back and asserts (trans_check) that the statement transaction is already
    empty, so we must close it out here. Any commit failure is captured by the
    installed PWT_error_handler.
  */
  trans_commit_stmt(thd);
  trans_commit(thd);

  /*
    Mark this producer done so the consumer can detect EOF, and wake it in
    case it is blocked waiting for data. A real engine error trips fatal_error
    so the consumer aborts the join instead of returning a truncated result.
    If this worker was killed (e.g. a user KILL aimed at it), record the kill
    so the consumer can propagate it to the manager's THD and abort the join
    with ER_QUERY_INTERRUPTED before any result is sent.
  */
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  killed_state killed= thd->killed;
  mysql_mutex_unlock(&thd->LOCK_thd_kill);

  mysql_mutex_lock(&mgr->LOCK_data);
  if (err)
    mgr->fatal_error= true;
  if (killed && mgr->kill_signal == NOT_KILLED)
    mgr->kill_signal= killed;
  mgr->active_workers--;
  mysql_cond_broadcast(&mgr->COND_data_avail);
  mysql_mutex_unlock(&mgr->LOCK_data);

  if (err)
    our_scan_table->file->print_error(err, MYF(0));

  DBUG_VOID_RETURN;
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

  worker->worker_run_query_to_manager();

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
    Close our private table copies while we are still attached to our THD
    (current_thd == thd) and, crucially, before destroy_background_thd()
    tears down the THD's transaction: the engine handle's close frees state
    that references that transaction (InnoDB's prebuilt). The manager never
    touches a started worker's tables, so no lock is needed here.
  */
  worker->snapshot_table_stats();          // while the tables are still open
  worker->close_worker_tables();

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
  worker->stats= thd->status_var;
  worker->stats.global_memory_used= 0;
  worker->stats.tmp_space_used= 0;
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

int pwt_manager::init_parallel_workers(THD *thd, JOIN *join, JOIN_TAB *scan_tab)
{
  uint i= 0;

  uint n= thd->variables.parallel_worker_threads;
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

  /*
    The engine has now divided the table into chunks, and it divides it by
    the shape of the B-tree rather than by the number of threads asked for.
    A worker beyond the last chunk is handed HA_ERR_END_OF_FILE the first
    time it asks for work and exits without reading a row, so every one of
    them costs a THD, a set of table instances opened from the share, the
    cloned items and the row buffer, and returns nothing. Ask for no more
    workers than there is work, and let the engine decline to answer (0)
    without imposing a bound.
  */
  if (const size_t chunks= file->pscan_chunk_count())
  {
    /*
      One chunk is not a division of labour: the single worker would read the
      whole table by itself, which is a serial scan performed by another thread,
      plus a row copied into the batch buffer, a mutex handed over and a re-read
      by the manager for every row of it. A table the engine cannot divide is a
      table for which the serial path is simply better, so decline and let
      do_select() take it.
    */
    if (chunks < 2)
    {
      file->pscan_end_coordinator();
      return HA_ERR_UNSUPPORTED;
    }
    set_if_smaller(n, (uint) chunks);
  }

  /*
    And decline if what is left is too few workers to be worth the exchange. The
    count tested is the one after the clamp, because a table that divides only
    two ways gives two workers however many were asked for.

    This used to say that one worker was the user's to ask for and was not even
    reliably slower, on the grounds that the worker's scan overlaps the manager's
    sending. Measured on a release build, one worker runs at 0.36 of the serial
    speed and two at 0.59, so that reasoning was wrong -- it came from a debug
    build, where the scan is inflated enough to hide the exchange.
  */
  if (n < PARALLEL_QUERY_MIN_WORKERS)
  {
    file->pscan_end_coordinator();
    return HA_ERR_UNSUPPORTED;
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

  /*
    The non-const join tables in join order (mgr_tabs[0] == scan_tab). These
    plus each worker's table copies form the manager->worker table map used to
    rebind the cloned conditions/refs/select list. No semijoin bushes here (the
    gate excludes them), so the tabs are simply join_tab[const_tables ..].
  */
  n_tables= join->table_count - join->const_tables;
  if (!(mgr_tabs= thd->alloc<JOIN_TAB*>(n_tables)) ||
      !(mgr_tables= thd->alloc<TABLE*>(n_tables)))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) (n_tables * sizeof(void*)));
    goto cleanup_old_workers;
  }
  for (uint t= 0; t < n_tables; t++)
  {
    mgr_tabs[t]= &join->join_tab[join->const_tables + t];
    mgr_tables[t]= mgr_tabs[t]->table;
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
    for (uint t= 0; t < n_tables; t++)
    {
      TABLE *tbl= mgr_tables[t];
      for (Field **f= tbl->field; *f; f++)
      {
        if (!bitmap_is_set(tbl->read_set, (*f)->field_index))
          continue;
        Item *itf= new (thd->mem_root) Item_field(thd, *f);
        if (!itf || ship_list.push_back(itf, thd->mem_root))
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
    if (ship_list.is_empty())
    {
      Item *one= new (thd->mem_root) Item_int(thd, (longlong) 1, 1);
      if (!one || ship_list.push_back(one, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_int));
        goto cleanup_old_workers;
      }
    }
    if (!(copy_back= new (thd->mem_root) Copy_field[ship_list.elements]))
    {
      my_error(ER_OUTOFMEMORY, MYF(0),
               (int) (ship_list.elements * sizeof(Copy_field)));
      goto cleanup_old_workers;
    }
    List_iterator_fast<Item> li(ship_list);
    Item *sel_item;
    while ((sel_item= li++))
    {
      Item *c= sel_item->deep_copy_with_checks(thd);
      if (!c || result_defn.push_back(c, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item));
        goto cleanup_old_workers;
      }
    }
  }
  result_tmp_param= new (thd->mem_root) TMP_TABLE_PARAM;
  if (!result_tmp_param)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(TMP_TABLE_PARAM));
    goto cleanup_old_workers;
  }
  if (make_result_table(thd, result_defn, &result_table))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "init_parallel_workers: failed to build the result table");
    goto cleanup_old_workers;
  }
  reclength= result_table->s->reclength;     // result-row image size

  /*
    Pair each column of result_table with the base-table field it was projected
    from, so that draining a row is a copy per column back into the manager's own
    records. Position i of ship_list is column i of result_table, which is how
    make_result_table() built it. The one item that is not an Item_field is the
    filler shipped for a query that reads no column, and it has nowhere to go
    back to.
  */
  {
    List_iterator_fast<Item> si(ship_list);
    Item *it;
    n_copy_back= 0;
    for (uint i= 0; (it= si++); i++)
      if (it->type() == Item::FIELD_ITEM)
        copy_back[n_copy_back++].set(((Item_field*) it)->field,
                                     result_table->field[i], false);
  }

  cur_cursor= 0;
  fatal_error= false;
  stop= false;
  reaped= false;
  cur_worker= nullptr;
  kill_signal= NOT_KILLED;

  for (i= 0; i < n; i++)
  {
    workers[i].set_engine_ctx(file->pscan_get_worker_context(i));
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
      A worker evaluates this session's expressions, so it needs the session
      variables those expressions read while they are evaluated.
      create_background_thd() starts from the global values, which silently
      changes what the query means: with the session in one time zone and the
      worker in the server's, a condition on a TIMESTAMP column drops rows the
      serial plan keeps, and a projection of one returns a different hour.

      Copied one at a time rather than as a whole struct. system_variables owns
      per-THD allocations (dynamic_variables_ptr and the session tracker), and it
      also carries option_bits, which would tell a worker it is inside the
      session's multi-statement transaction and change how it commits.

      Only the variables an expression reads while it is evaluated belong here.
      Anything read while it is built is already right, because the clones are
      built and fixed on this thread, which is why lc_time_names and
      div_precincrement are absent: DAYNAME() and the division keep the locale and
      the scale they were fixed with. time_zone, sql_mode and default_week_format
      are here because a test showed each of them changing an answer.
      old_behavior is here because the date and time conversions read it as they
      run, the same way sql_mode is read.
    */
    workers[i].thd->variables.time_zone=           thd->variables.time_zone;
    workers[i].thd->variables.sql_mode=            thd->variables.sql_mode;
    workers[i].thd->variables.old_behavior=        thd->variables.old_behavior;
    workers[i].thd->variables.default_week_format= thd->variables.default_week_format;

    /*
      Give this worker its own copy of every non-const join table, opened from
      the shared TABLE_SHARE (open_worker_tables); the driving table is
      worker_tables[0] / our_scan_table. Self-cleans on failure, so on error we
      go to cleanup_db_string (the worker thd is not yet registered).
    */
    if (open_worker_tables(thd, workers + i))
      goto cleanup_db_string;

    server_threads.insert(workers[i].thd);  // +information_schema.processlist

    /*
      Set up how this worker joins the non-driving tables (access method,
      worker-bound ref clone, condition), its result container, and private
      clones of the WHERE condition + select list with field references rebound
      to this worker's table copies. At run time the worker scans the driving
      chunk, joins the inner tables, projects worker_proj into result_table and
      ships that record image.
    */
    if (setup_worker_join(thd, workers + i) ||
        setup_worker_tabs(thd, workers + i) ||
        make_result_table(thd, result_defn, &workers[i].result_table) ||
        clone_worker_exprs(thd, workers + i))
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to set up worker execution");
      goto cleanup_thread_create;
    }
    /*
      This table belongs to the worker, like its table copies do. It is created
      here, on the manager's thread, so create_tmp_table() left the manager in
      in_use, and Field::get_thd() hands out TABLE::in_use: projecting into these
      fields would raise any warning the projection produces on the manager's
      diagnostics area, from the worker's thread, past the worker's own error
      handler and concurrently with the other workers.
    */
    workers[i].result_table->in_use= workers[i].thd;

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
  workers[i].close_worker_tables();

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
    workers[j].abort_worker();
  free_queue();
  free_result_tables(thd);            // workers reaped; result tables now idle
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
  buffer at a time (cur_worker), advancing cur_cursor through its batch_count
  result rows; when the buffer is exhausted it releases the worker to refill
  (clears batch_full, signals COND_data_space) and picks the next ready worker.
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
  const uint reclen= reclength;
  struct timespec wait;
  wait.tv_nsec= 0;

  for (;;)
  {
    if (cur_worker)                               // draining a worker's buffer
    {
      pwt_worker *w= cur_worker;
      if (cur_cursor < w->batch_count)
      {
        memcpy(dst, w->batch_rows + (size_t) cur_cursor * reclen, reclen);
        cur_cursor++;
        DBUG_RETURN(0);
      }
      // buffer drained; release the worker so it can refill
      mysql_mutex_lock(&LOCK_data);
      cur_worker= nullptr;
      w->batch_full= false;                       // buffer is the worker's again
      mysql_cond_broadcast(&COND_data_space);     // wake it to refill
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
        if (workers[i].batch_full)
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
      if (!active_workers)                        // all producers done, drained
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
      mysql_mutex_lock(&LOCK_data);               // re-lock for the next pass
    }
    cur_worker= next;
    cur_cursor= 0;                                // start of next's buffer
    mysql_mutex_unlock(&LOCK_data);
    // loop back to drain next->batch_rows
  }
}


/*
  @brief
    Drain the workers' result rows and send them to the client.

  @description
  The workers ran the whole select-project query over their disjoint chunks
  and produced the final result rows; the manager just concatenates them (in
  arrival order) and sends each to the client. The select-list metadata was
  already sent (from the query's own field list) before do_select() ran, so we
  only supply the row values here -- the select list is evaluated against the
  manager's own records, which the drain has filled from the shipped columns.
  send_records is advanced so do_select() can set limit_found_rows.

  @return  0 on success (all rows sent), 1 on error.
*/
int pwt_manager::manager_collect_and_send(JOIN *join)
{
  DBUG_ENTER("pwt_manager::manager_collect_and_send");
  uchar *dst= result_table->record[0];

  /*
    The plan's own terminal function, reached exactly as sub_select() would reach
    it: the last real table's next_select, called with the tab after it.
    make_aggr_tables_info() has put setup_end_select_func()'s choice there, which
    for the shapes the gate accepts today is end_send().

    What this buys beyond not keeping a second copy of the server's code: the row
    accounting (send_records, accepted_rows, duplicate_rows), HAVING, and the
    LIMIT checks including WITH TIES are the server's. The gate still refuses
    HAVING and LIMIT, so those paths are unreached today; relaxing either becomes
    a change to the gate rather than more code here, which is the point of
    standing on the plan's terminal at all. A plan whose terminal is some other
    function needs nothing here either.
  */
  JOIN_TAB *last_tab= mgr_tabs[n_tables - 1];
  DBUG_ASSERT(last_tab->next_select);
  int ret= 0;

  /*
    The manager's tables were opened but never read, so clear the flags a reader
    would have left. Copy_field captures &table->null_row, and a stale null_row
    would make every copied field read as NULL.

    Writing into these records is not something a SELECT's write_set allows, and
    Field::store() asserts on that, so mark the fields writable for the drain.
    A record is being filled here in place of the reader that would normally
    have filled it, which is what the helper is for.
  */
  MY_BITMAP **saved_write_set= (MY_BITMAP**)
                                thd->alloc(n_tables * sizeof(MY_BITMAP*));
  if (!saved_write_set)
    DBUG_RETURN(1);
  for (uint t= 0; t < n_tables; t++)
  {
    mgr_tables[t]->status= 0;
    mgr_tables[t]->null_row= false;
    saved_write_set[t]= dbug_tmp_use_all_columns(mgr_tables[t],
                                                 &mgr_tables[t]->write_set);
  }

  for (;;)
  {
    int rc= drain_next_row(dst);
    if (rc < 0)
      break;                                      // -1: all rows drained
    if (rc > 0)
    {
      ret= 1;                                     // killed / worker error
      break;
    }

    /* Put the shipped columns back where the plan expects to find them. */
    for (uint i= 0; i < n_copy_back; i++)
      (*copy_back[i].do_copy)(&copy_back[i]);

    enum_nested_loop_state nls= (*last_tab->next_select)(join, last_tab + 1,
                                                        FALSE);
    if (nls == NESTED_LOOP_QUERY_LIMIT)
    {
      /*
        Enough rows: stop the producers rather than draining the rest. The
        workers see this the next time they hand a batch over.
      */
      mysql_mutex_lock(&LOCK_data);
      stop= true;
      mysql_cond_broadcast(&COND_data_space);
      mysql_mutex_unlock(&LOCK_data);
      break;
    }
    if (nls < NESTED_LOOP_OK)                     // ERROR or KILLED
    {
      ret= 1;
      break;
    }
  }

  if (!ret)
  {
    /*
      End of records. This is the call that sends an aggregate's single row, and
      the one that makes a temp-table stage read back what it accumulated.
    */
    enum_nested_loop_state nls= (*last_tab->next_select)(join, last_tab + 1,
                                                        TRUE);
    if (nls < NESTED_LOOP_OK)
      ret= 1;
  }

  for (uint t= 0; t < n_tables; t++)
    dbug_tmp_restore_column_map(&mgr_tables[t]->write_set, saved_write_set[t]);
  DBUG_PRINT("info", ("join records:%llu", join->send_records));
  DBUG_RETURN(ret);
}

/*
  @brief
    Create + instantiate one result container in result_defn's column layout.

  Only the record buffer and the fields are ever used (the worker projects into
  result_table->record[0] and ships its image; the manager receives images and
  sends from it) -- no rows are written through the storage engine.

  @return  true on error, false on success (*out set).
*/
bool pwt_manager::make_result_table(THD *thd, List<Item> &defn, TABLE **out)
{
  /*
    Start from a freshly counted param every time. create_tmp_table() overwrites
    param->func_count with the number of items it actually has to copy, so a
    second table built from the same param can allocate fewer fields than the
    layout needs, which is what the assertion in Create_tmp_table::finalize()
    catches. The manager and every worker build this same layout, so each of
    them re-counts.
  */
  result_tmp_param->init();
  count_field_types(join->select_lex, result_tmp_param, defn, false);
  result_tmp_param->skip_create_table= true;

  /*
    TMP_TABLE_ALL_COLUMNS makes create_tmp_table() give every item of the list a
    field, including the constant ones. By default it skips constants, which is
    right for a query that materialises its result and can evaluate them once
    outside the table, but wrong here: the layout has to mirror the select list
    one for one, because the worker projects item i into field i and ships the
    record image, and the manager sends one Item_field per field to the client.
    Without this a select list holding a constant, "SELECT 42, a FROM t1", built
    a table with fewer fields than the projection walks over.
  */
  const ulonglong opts= join->select_options | TMP_TABLE_ALL_COLUMNS;
  TABLE *t= create_tmp_table(thd, result_tmp_param, defn,
                             nullptr, false, false,
                             opts, HA_POS_ERROR,
                             &empty_clex_str, true, false);
  if (!t)
    return true;
  if (instantiate_tmp_table(t, result_tmp_param->keyinfo,
                            result_tmp_param->start_recinfo,
                            &result_tmp_param->recinfo,
                            opts, true /*cross_thread*/))
  {
    free_tmp_table(thd, t);
    return true;
  }
  /*
    The whole transport is positional, so a layout that does not match the
    select list item for item would have the worker and the manager reading
    different columns, or walking past the end of the field array. Refuse
    instead, whatever the reason turns out to be.
  */
  DBUG_ASSERT(t->s->fields == defn.elements);
  if (t->s->fields != defn.elements)
  {
    free_tmp_table(thd, t);
    return true;
  }
  *out= t;
  return false;
}


/*
  @brief
    Open this worker's private copy of every non-const join table from the
    shared TABLE_SHARE.

  @description
    open_table_from_share() runs here on the manager thread, so the open must
    happen with in_use == current_thd (handler::ha_thd() asserts that, and
    ha_innobase::open() calls it); we repoint in_use at the worker afterwards.
    InnoDB caches the THD lazily (update_thd() on first use), not at open, so
    each worker gets a private handler and they scan concurrently without a
    shared-scan lock. worker_tables[0] is the parallel-scanned driving table
    (also kept as our_scan_table). Self-cleans on failure.

  @return  true on error.
*/
bool pwt_manager::open_worker_tables(THD *thd, pwt_worker *worker)
{
  worker->n_tables= n_tables;
  /* the table array, plus the ANALYZE counters this worker will fill in */
  if (!(worker->worker_tables= thd->alloc<TABLE*>(n_tables)) ||
      !(worker->tab_stats= thd->calloc<Table_access_tracker>(n_tables)) ||
      !(worker->tab_hstats= thd->calloc<ha_handler_stats>(n_tables)))
    return true;
  for (uint t= 0; t < n_tables; t++)
    worker->worker_tables[t]= nullptr;

  for (uint t= 0; t < n_tables; t++)
  {
    TABLE *src= mgr_tables[t];
    TABLE *st= (TABLE*) my_malloc(key_memory_TABLE, sizeof(TABLE),
                                  MYF(MY_WME | MY_ZEROFILL));
    if (!st)
      goto err;
    if (open_table_from_share(thd, src->s, &src->s->table_name,
                              HA_OPEN_KEYFILE | HA_TRY_READ_ONLY,
                              EXTRA_RECORD, thd->open_options, st,
                              false, nullptr))
    {
      my_free(st);
      goto err;
    }
    st->in_use= worker->thd;
    st->file->ha_handler_stats_reset();
    /*
      Give the copy the manager table's place in the join: its bit in the table
      map and its join position. Items cloned onto this table take used_tables()
      from TABLE::map, so with a zero map a rebound predicate looks like a
      constant -- Item_cond::fix_fields() then evaluates it while we are still
      cloning it (can_eval_in_optimize()), reading a record buffer that no row
      has been read into yet.
    */
    st->map= src->map;
    st->tablenr= src->tablenr;
    /*
      Mirror the manager table's column bitmaps into the copy's own, rather than
      marking every column: the optimizer has already marked exactly the columns
      this query reads, and the engine builds its fetch template from read_set,
      so a blanket all-columns read_set would convert every column of every row
      to MySQL format instead of just the ones the worker evaluates. Copying
      also keeps write_set empty for a table we only read, so a store into a
      source field still trips marked_for_write().

      The copy owns these bitmaps, so nothing is shared with the manager. Done
      here, before the conditions and the select list are cloned onto these
      tables, so the read_set is already in place if anything evaluates a field
      during cloning. open_worker_tables() runs after the optimizer has
      finished, so src's bitmaps are final.
    */
    bitmap_copy(&st->def_read_set, src->read_set);
    bitmap_copy(&st->def_write_set, src->write_set);
    st->column_bitmaps_set(&st->def_read_set, &st->def_write_set);
    worker->worker_tables[t]= st;
  }
  worker->our_scan_table= worker->worker_tables[0];   // the driving table
  return false;

err:
  my_error(ER_INTERNAL_ERROR, MYF(0),
           "init_parallel_workers: failed to open worker table from share");
  worker->close_worker_tables();
  return true;
}


/*
  Every JOIN_TAB field the worker's executor could reach that the gate is
  supposed to have made inert. Checked on the manager's tab before it is copied,
  so relaxing a gate without teaching the copy about the field it lets through
  fails here rather than silently executing with the manager's state -- a copy
  keeps the manager's pointer in anything the copy does not overwrite, which is
  the same trap TABLE::map and TABLE::in_use were.

  This is not only a guard. Writing it is what found the BNL scan filter: the
  question "is JOIN_TAB::cache_select inert?" turned out to have the answer no,
  and a wrong result behind it. Anything added here should be *checked* against
  the suite, not assumed.
*/
static void pwt_assert_tab_inert(JOIN_TAB *tab, bool is_driving)
{
#ifndef DBUG_OFF
  /* Outer joins: the gate refuses join->outer_join outright. */
  DBUG_ASSERT(!tab->first_inner);
  DBUG_ASSERT(!tab->last_inner);
  DBUG_ASSERT(!tab->first_upper);
  DBUG_ASSERT(!tab->on_expr_ref || !*tab->on_expr_ref);
  DBUG_ASSERT(!tab->on_precond);

  /* Semijoin strategies and semijoin materialization. */
  DBUG_ASSERT(!tab->bush_children);
  DBUG_ASSERT(tab->sj_strategy == SJ_OPT_NONE);
  DBUG_ASSERT(!tab->loosescan_match_tab);
  DBUG_ASSERT(!tab->do_firstmatch);
  DBUG_ASSERT(!tab->flush_weedout_table);
  DBUG_ASSERT(!tab->check_weed_out_table);
  DBUG_ASSERT(!tab->first_weedout_table);
  DBUG_ASSERT(!tab->emb_sj_nest);

  /* Rowid filters and range/index_merge access. */
  DBUG_ASSERT(!tab->rowid_filter);
  DBUG_ASSERT(!tab->select || !tab->select->quick);

  /* Materialized derived tables, CTEs and split materialization. */
  DBUG_ASSERT(!tab->split_derived_to_update);

  /* DISTINCT and HAVING are refused, so neither shortcut applies. */
  DBUG_ASSERT(!tab->shortcut_for_distinct);
  DBUG_ASSERT(!tab->having);

  /* The driving table is scanned; only the tables after it are looked up. */
  DBUG_ASSERT(is_driving || tab->type == JT_ALL || tab->type == JT_REF ||
              tab->type == JT_EQ_REF);
#else
  (void) tab; (void) is_driving;
#endif
}


/*
  Every JOIN field the worker's executor could reach that the gate is supposed to
  have made inert. Same purpose as pwt_assert_tab_inert(), one level up: relaxing
  a gate without teaching setup_worker_join() about the field it lets through
  fails here rather than running with a field this worker's JOIN does not carry.
*/
static void pwt_assert_join_inert(JOIN *join)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(!join->outer_join);
  /*
    By this point DISTINCT has been turned into something else: either the temp
    table's own uniqueness, or a flag on the aggregation tab for
    remove_duplicates() to act on, and make_aggr_tables_info() clears the JOIN's
    flag in both cases. So this is not a restriction, it is a statement that the
    manager has nothing of its own to do about DISTINCT.
  */
  DBUG_ASSERT(!join->select_distinct);
  /*
    An ORDER BY the gate did not see, because make_aggr_tables_info() had not run
    yet when it looked. It is only sound when there is an aggregation temp table
    for the sort to be applied to, downstream of everything the workers did: then
    the order is established on the manager, after the rows have arrived, and the
    arrival order they had is irrelevant. Without one, the order would have to
    come from the driving scan, which is precisely what a chunked scan does not
    give.
  */
  DBUG_ASSERT(!join->order || join->aggr_tables > 0);
  /*
    HAVING, the select list and the aggregates are evaluated by the plan's own
    terminal function on the manager, over records the drain filled, so none of
    them is a worker's concern.

    need_tmp, group, group_list and sort_and_group are all legitimately set here:
    an aggregate sets sort_and_group, and a GROUP BY sets the rest and gets an
    aggregation temp table that the manager drives the plan into. What must not
    happen is a plan that expects its rows already grouped, and that is checked
    by pwt_plan_needs_group_order() once the terminals are known.
  */
  DBUG_ASSERT(!join->procedure);
  DBUG_ASSERT(!join->group_optimized_away);
  DBUG_ASSERT(!(join->select_options & OPTION_FOUND_ROWS));
#else
  (void) join;
#endif
}


/*
  @brief
    Build the JOIN this worker's JOIN_TABs belong to.

  @description
    Only the fields the executor reads are carried over, by name. The rest are
    whatever JOIN's own constructor makes them, which is why the constructor is
    used rather than a copy of the manager's object: JOIN declares its copy
    constructor and assignment private and unimplemented, so copying it means
    copying bytes past a deliberate prohibition, and every field this code did
    not think about would then hold a value that looks chosen. Built by the
    constructor, an unconsidered field holds JOIN::init()'s default, and the
    static_assert below makes a field added upstream a compile-time prompt to
    decide which of the two it should be.

    What the executor reads, established by walking sub_select() and
    evaluate_join_record() for the shapes the gate allows: thd, join_tab,
    return_tab and found_records. map2table is read only for
    split_derived_to_update, join_tab_execution_startup() only inside its two
    semijoin-materialization branches, and JOIN_TAB::preread_init() returns
    before touching join->thd unless the table is a materialized derived -- all
    of which pwt_assert_tab_inert() asserts away.

    result is deliberately left null. It is the manager's connection to the
    client, and manager_collect_and_send() is the only thing that may send a
    row; a worker that reached for it should crash rather than write to a socket
    two threads share.

  @return  true on error.
*/
bool pwt_manager::setup_worker_join(THD *thd, pwt_worker *worker)
{
  /*
    Adding a field to JOIN does not break this, it just stops the build until
    someone has decided whether a worker needs to carry it. Update the size once
    that decision is made.

    Debug builds only: JOIN carries dbug_join_tab_array_size under
    #ifndef DBUG_OFF, so its size is not the same in the two build types, and
    pinning both would mean two magic numbers of which only one is ever
    checked by whoever changes the class. mtr builds debug, so this is where
    the prompt lands.
  */
#ifndef DBUG_OFF
  static_assert(sizeof(JOIN) == 1552,
                "JOIN has changed shape: review what setup_worker_join() "
                "carries over to a worker");
#endif

  pwt_assert_join_inert(join);

  if (!(worker->worker_join= new (thd->mem_root)
          JOIN(worker->thd, join->fields_list, join->select_options, nullptr)))
    return true;

  /*
    The worker's JOIN describes the worker's array, which holds the join's
    non-const tables and nothing else. Not the manager's counts: the manager
    counts the const tables it resolved before the join started, and
    JOIN_TAB::pfs_batch_update() finds the innermost table by
    join_tab + table_count - 1, which with the manager's count lands past the
    end of a worker's array.
  */
  worker->worker_join->table_count= n_tables;
  worker->worker_join->top_join_tab_count= n_tables;
  worker->worker_join->const_tables= 0;
  worker->worker_join->select_lex= join->select_lex;
  return false;
}


/*
  @brief
    Give this worker its own copy of each of the join's non-const JOIN_TABs,
    rebound to the worker: its private TABLE copies, its cloned conditions and
    refs, its own trackers. Tables are taken in join order, which is the order a
    worker joins them in, because a ref value reads the tables before it.

    The tabs are copied rather than built from nothing so that a field this code
    does not know about holds the value the optimizer chose instead of a zero
    that would look deliberate. What must not survive the copy is anything that
    points into the manager's execution state, so every such field is either
    overwritten below or asserted inert by pwt_assert_tab_inert().

  @return  true on error.
*/
bool pwt_manager::setup_worker_tabs(THD *thd, pwt_worker *worker)
{
  if (!(worker->join_tabs= thd->alloc<JOIN_TAB>(n_tables)))
    return true;
  worker->worker_join->join_tab= worker->join_tabs;

  for (uint k= 0; k < n_tables; k++)
  {
    JOIN_TAB *mtab= mgr_tabs[k];
    JOIN_TAB *wtab= &worker->join_tabs[k];

    pwt_assert_tab_inert(mtab, k == 0);
    *wtab= *mtab;

    wtab->table= worker->worker_tables[k];

    /*
      And the way back. join_read_next_same() and its relatives find the
      JOIN_TAB they are reading for through TABLE::reginfo.join_tab, at execution
      time, so a worker's table has to point at the worker's tab: the optimizer
      set this on the manager's tables (sql_select.cc:5751) and
      open_table_from_share() leaves a copy without it. Third field of TABLE to
      need this after map and in_use -- assume anything the optimizer sets on a
      TABLE is missing on the copy until checked.
    */
    wtab->table->reginfo.join_tab= wtab;

    /*
      Both halves of the condition, ANDed, so select_cond alone is the whole of
      what this table is filtered by. The places the optimizer had moved parts
      of it to are then cleared: they name items belonging to the manager, and a
      worker that read them would be evaluating another thread's Items.
    */
    if (pwt_clone_table_conds(thd, mtab, mgr_tables, worker->worker_tables,
                              n_tables, &wtab->select_cond))
      return true;
    wtab->pre_idx_push_select_cond= nullptr;
    wtab->cache_select= nullptr;
    /*
      SQL_SELECT holds the condition and the quick select. The condition is
      already on select_cond and the gate refuses a quick select, and the record
      sources only ever ask whether select->quick is set, so a worker needs
      neither and reads the manager's neither.
    */
    wtab->select= nullptr;

    /* No join buffer in a worker: it joins a row at a time. */
    wtab->cache= nullptr;
    wtab->use_join_cache= FALSE;
    wtab->jbuf_tracker= nullptr;

    /* ANALYZE reads these back off the manager, see quiesce_workers(). */
    wtab->tracker= &worker->tab_stats[k];

    wtab->join= worker->worker_join;

    /*
      Where a row goes once every table has matched: on to the next table, and
      off the end of the last one into pwt_end_send() rather than end_send().
      That is the whole of the difference between what a worker runs and what a
      serial plan runs.
    */
    wtab->next_select= (k + 1 < n_tables) ? sub_select : pwt_end_send;

    /*
      The driving table comes from the engine's chunk reader. The others keep
      the record source make_join_readinfo() chose, which the struct copy
      brought with it: those functions take the JOIN_TAB they are called with,
      so they read this worker's table through this worker's ref and its own
      read_record. read_record itself is left zeroed for read_first_record() to
      fill, since it is per-scan state rather than plan.
    */
    if (!k)
      wtab->read_first_record= pwt_pscan_init_read_record;

    /*
      READ_RECORD mixes plan and per-scan state. make_join_readinfo() sets the
      row-fetching function and the unlock function at optimize time -- for a ref
      table join_read_next_same, for eq_ref join_no_more_records -- while the
      buffers and cursors are filled by read_first_record() when the scan starts.
      So clear it, which drops any state a previous execution of this statement
      left, and put the plan half back by hand. Zeroing all of it left null
      function pointers for sub_select() to call.
    */
    bzero((char*) &wtab->read_record, sizeof(wtab->read_record));
    wtab->read_record.read_record_func= mtab->read_record.read_record_func;
    wtab->read_record.read_record_func_and_unpack_calls=
      mtab->read_record.read_record_func_and_unpack_calls;
    wtab->read_record.unlock_row= mtab->read_record.unlock_row;
    wtab->read_record.table= wtab->table;
    wtab->read_record.thd= worker->thd;
    wtab->read_record.print_error= TRUE;

    if (k && (wtab->type == JT_EQ_REF || wtab->type == JT_REF))
    {
      if (clone_table_ref(thd, &mtab->ref, wtab->table,
                          mgr_tables, worker->worker_tables, n_tables,
                          &wtab->ref))
        return true;
    }

    /*
      Recomputed, not copied: it answers "is this the innermost table", which
      sub_select() asserts against the live answer, and the worker's array is
      not the manager's. Last, because it reads the condition and the type set
      above.
    */
    wtab->cached_pfs_batch_update= wtab->pfs_batch_update();
  }
  return false;
}


/*
  @brief
    Build this worker's private clones of the driving table's WHERE condition
    and the select list, rebinding their field references to the worker's table
    copies.

  @return  true  error
           false clone built
*/
bool pwt_manager::clone_worker_exprs(THD *thd, pwt_worker *worker)
{
  TABLE **from= mgr_tables;
  TABLE **to= worker->worker_tables;

  // shipped columns -> per-item projection into result_table->field[i]
  worker->proj_count= ship_list.elements;
  worker->worker_proj= (Item**) thd->alloc(worker->proj_count * sizeof(Item*));
  if (!worker->worker_proj)
    return true;

  List_iterator_fast<Item> li(ship_list);
  Item *src;
  uint i= 0;
  while ((src= li++))
  {
    if (!(worker->worker_proj[i++]= pwt_clone_rebind(thd, src, from, to,
                                                     n_tables)))
      return true;
  }
  return false;
}


/*
  @brief  Free the manager and per-worker result containers.
*/
void pwt_manager::free_result_tables(THD *thd)
{
  if (workers)
    for (uint i= 0; i < nworkers; i++)
      if (workers[i].result_table)
      {
        free_tmp_table(thd, workers[i].result_table);
        workers[i].result_table= nullptr;
      }
  if (result_table)
  {
    free_tmp_table(thd, result_table);
    result_table= nullptr;
  }
}


/*
  @brief
    Determine if the parallel workers run this whole query themselves and ship
      final result rows.
      (Called from make_join_readinfo, see JOIN::worker_side_parallel.)

  @description
  True for a streaming select-project[-join] query whose driving table
  'scan_tab' is parallel-scannable and whose remaining tables the workers can
  join themselves:
    - at least one non-const table (the parallel-scanned driving table);
    - all joins are inner (no outer-join null-complementation, no semijoin),
      and every non-driving table is reached by an index ref/eq_ref lookup or a
      plain full scan -- nothing that needs a join buffer, range/quick, rowid
      filter, loose scan, or a subquery trigger guard on the ref;
    - no temporary table (rules out GROUP BY, DISTINCT, ORDER BY, window
      functions, SQL_BUFFER_RESULT, ...): the result is the plain concatenation
      of the per-chunk join results;
    - no SQL_CALC_FOUND_ROWS or PROCEDURE -- these need a global post-pass the
      workers cannot do independently. LIMIT and OFFSET are allowed because the
      manager applies them on its single thread, and COUNT/SUM with no GROUP BY
      because the manager aggregates over the rows the workers ship;
    - every condition, ref value and select-list item can be deep-cloned (so
      each worker gets private, thread-safe copies bound to its own tables).

  Anything else simply runs serially.

  @return
        true  if we can run this query in parallel
        false otherwise
*/
/*
  @brief
    Refuse the aggregates whose value depends on the order the rows arrive in.

  @description
    The manager runs the plan's own terminal function over base-table records the
    drain has filled, so an aggregate computes from every qualifying row exactly
    as it would have serially, whatever kind it is. Nothing here restricts kinds
    on mechanical grounds -- MIN, AVG, and the DISTINCT variants all work.

    What is left is order. Workers finish chunks in whatever order they finish
    them, so the rows reach the aggregate in an order that varies run to run.
    Almost every aggregate is indifferent to that. GROUP_CONCAT is not: its value
    *is* the order, so in the workers it would return a different string each
    time, where serially it returns the scan order. A query whose answer changes
    between identical runs is worse than a query that runs serially, so refuse it
    and let it run serially.

  @return true if every aggregate in the select list is order-independent.
*/
static bool pwt_aggregates_supported(THD *thd, JOIN *join)
{
  List_iterator_fast<Item> li(join->fields_list);
  Item *item;

  while ((item= li++))
  {
    if (item->type() != Item::SUM_FUNC_ITEM)
      continue;
    if (((Item_sum*) item)->sum_func() == Item_sum::GROUP_CONCAT_FUNC)
      return false;
  }
  return true;
}


bool can_run_query_in_workers(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("can_run_query_in_workers");
  THD *thd= join->thd;
  SELECT_LEX *sl= join->select_lex;

  if (join->table_count - join->const_tables < 1)   // need the driving table
  {
    DBUG_PRINT("info", ("only constant tables"));
    DBUG_RETURN(false);
  }
  /*
    need_tmp is not refused. A GROUP BY needs an aggregation temp table and the
    manager drives the plan into it. The shapes a temp table is otherwise built
    for -- DISTINCT, ORDER BY, window functions, a procedure -- are each refused
    on their own terms below and above, which says what is excluded rather than
    excluding a superset of it. Note it would say little here anyway: this runs
    from make_join_readinfo(), before make_aggr_tables_info() plans the
    aggregation, so need_tmp still holds its pre-planning value.
  */
  /*
    LIMIT and OFFSET are not refused. end_send() applies the limit and
    select_result_sink::send_data_with_check() the offset, both on the manager's
    single thread, so the count of rows sent is exact; reaching the limit returns
    NESTED_LOOP_QUERY_LIMIT, which manager_collect_and_send() turns into a stop
    for the producers. Which rows arrive is another matter: they are whichever
    ones the workers finish first, not the ones a serial scan would reach. For a
    query with no ORDER BY that is a legal answer -- the rows of an unordered
    query have no order to take a prefix of -- and a query that does have ORDER BY
    is refused below, so it runs serially and is unaffected.
  */
  if (join->select_options & OPTION_FOUND_ROWS)  // SQL_CALC_FOUND_ROWS
  {
    DBUG_PRINT("info", ("SQL_CALC_FOUND_ROWS"));
    DBUG_RETURN(false);
  }
  if (join->procedure)
  {
    DBUG_PRINT("info", ("procedure"));
    DBUG_RETURN(false);
  }
  if (sl->have_window_funcs())
  {
    DBUG_PRINT("info", ("window funcs"));
    DBUG_RETURN(false);
  }
  if ((sl->agg_func_used() || sl->with_sum_func) &&
      !pwt_aggregates_supported(thd, join))
  {
    DBUG_PRINT("info", ("order-dependent aggregate"));
    DBUG_RETURN(false);
  }

  /*
    GROUP BY is allowed. The manager hands each drained row to the plan's terminal
    function, so the grouping is done by the server's own aggregation temp table,
    keyed on the group -- exactly as it would be serially. What is not allowed is
    a plan that expects the rows grouped already, and which terminal the optimizer
    picked is not known until after this runs, so that is checked in
    run_worker_side_join() instead (pwt_plan_needs_group_order).
  */
  /*
    DISTINCT is allowed. It is always carried out on a materialised temp table
    downstream of the drain, by one of two routines that are both indifferent to
    the order the rows arrived in: remove_dup_with_hash_index(), or
    remove_dup_with_compare(), which despite the name compares each row against
    all the rows after it rather than against its neighbour. So a chunked scan
    takes nothing away from either.

    Most DISTINCT queries do not reach here as DISTINCT at all. The optimizer
    rewrites them to GROUP BY (sql_select.cc, "Change DISTINCT to GROUP BY"),
    which clears select_distinct before this gate runs, and they are then the
    GROUP BY case. What is left when the rewrite does not happen is DISTINCT with
    a LIMIT, since the rewrite is conditional on there being no row limit.
  */
  if (join->order)
  {
    /*
      ORDER BY of a scan is refused, and this is the check that does it: an order
      the driving table's own filesort produces cannot survive being split into
      chunks that finish in an arbitrary order. Delivering it would need the
      workers to sort their chunks and the manager to merge them, which is not
      built.

      This does not refuse an ORDER BY applied to an aggregation temp table --
      after a GROUP BY, or after a DISTINCT. There join->order is null when this
      runs and is set later by make_aggr_tables_info(), and the sort happens on
      the manager once every row has arrived, so the order the rows arrived in is
      irrelevant. pwt_assert_join_inert() states that as the invariant it is.
    */
    DBUG_PRINT("info", ("order by"));
    DBUG_RETURN(false);
  }
  /*
    HAVING, the select list and the aggregates need nothing from this gate. They
    are evaluated by the plan's own terminal function, on the manager, over base
    table records the drain has filled from the shipped columns -- so they read
    exactly what they would have read serially. Neither the items nor the
    reference array are touched, and no worker evaluates any of them.
  */
  if (join->outer_join)                             // no outer joins
  {
    DBUG_PRINT("info", ("outer_join"));
    DBUG_RETURN(false);
  }

  // every non-const join table must be one the worker can scan / look up itself
  for (uint j= join->const_tables; j < join->table_count; j++)
  {
    JOIN_TAB *tab= &join->join_tab[j];
    if (!tab->table)
    {
      DBUG_PRINT("info", ("join tab %u has no table", j));
      DBUG_RETURN(false);
    }
    /*
      A worker opens its own copy of every one of these tables, not just the one
      it scans in chunks, so each has to pass the same test the driving table
      does. The name reads oddly for a table the worker only looks rows up in,
      but every condition it checks is one a worker-read table needs.

      An internal tmp table, a materialized derived table or subquery, has a
      share built in memory rather than read from a .frm, and
      open_table_from_share() walks off the end of it. Blob payloads live outside
      the record buffer, so they do not survive the by-value row transport
      whichever table they come from. A partitioned table cannot be opened as a
      plain copy. The engine flag is what tells us the engine can also hand the
      worker the manager's snapshot, so requiring it here closes the second half
      of this hole as well, a join whose inner table is in an engine that cannot
      share one and would be read outside the manager's snapshot.
    */
    if (!table_can_be_parallel_scanned(tab->table))
    {
      DBUG_PRINT("info", ("%s cannot be read by a worker",
                          tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (tab->bush_children)                       // semijoin materialization
    {
      DBUG_PRINT("info", ("SJM on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    /*
      The worker runs a plain nested loop, so it implements none of the semijoin
      duplicate-elimination strategies. Nothing in the flat table list shows that
      FirstMatch or DuplicateWeedout is in force, and a worker that ignores them
      emits exactly the duplicates they exist to remove, so refuse a plan that
      uses any of them. bush_children above covers the materialized ones.
    */
    if (tab->sj_strategy != SJ_OPT_NONE || tab->loosescan_match_tab ||
        tab->do_firstmatch || tab->check_weed_out_table ||
        tab->flush_weedout_table || tab->first_weedout_table)
    {
      DBUG_PRINT("info", ("semijoin strategy on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (tab->rowid_filter)                        // rowid filter
    {
      DBUG_PRINT("info", ("rowid_filter on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (tab->select && tab->select->quick)        // range / index_merge
    {
      DBUG_PRINT("info", ("quick select on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (j > join->const_tables)                     // non-driving table access
    {
      if (tab->type != JT_EQ_REF && tab->type != JT_REF && tab->type != JT_ALL)
      {
        DBUG_PRINT("info", ("%s on %s",
                            join_type_str[tab->type],
                            tab->table->alias.ptr()));
        DBUG_RETURN(false);
      }
      if (tab->type == JT_EQ_REF || tab->type == JT_REF)
        for (uint p= 0; p < tab->ref.key_parts; p++)
        {
          if (tab->ref.cond_guards && tab->ref.cond_guards[p])  // subquery trigger
          {
            DBUG_PRINT("info", ("cond_guards, %s",tab->table->alias.ptr()));
            DBUG_RETURN(false);
          }
          if (!pwt_item_is_clonable(thd, tab->ref.items[p]))
          {
            DBUG_PRINT("info", ("ref unclonable, %s",tab->table->alias.ptr()));
            DBUG_RETURN(false);
          }
        }
    }
    Item *tab_cond, *tab_cache_cond;
    pwt_table_conds(tab, &tab_cond, &tab_cache_cond);
    if ((tab_cond && !pwt_item_is_clonable(thd, tab_cond)) ||
        (tab_cache_cond && !pwt_item_is_clonable(thd, tab_cache_cond)))
    {
      DBUG_PRINT("info", ("cond unclonable, %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
  }

  /*
    The select list is deliberately not checked for clonability. Nothing clones
    it: what a worker gets is an Item_field per base-table column the query
    reads, which is always clonable, and the select list is evaluated on the
    manager. Probing it was also actively harmful -- pwt_item_is_clonable()
    copies the item to test it, and copying an Item_sum_min_max crashes, because
    its copy constructor leaves cmp uninitialised while its cleanup() deletes it.
  */
  DBUG_RETURN(true);
}


/*
  @brief
    Drive worker-side execution from do_select(): spin up the workers and then
    collect and send their result rows.

  @return  0 = handled (result sent), 1 = error, -1 = the engine declined the
           parallel scan (caller should run the query serially instead).
*/
/*
  @brief
    Does this plan need its rows in GROUP BY order?

  @description
    Workers finish chunks in whatever order they finish them, so rows reach the
    manager in an order that varies from run to run. Two of the server's group
    terminals detect a new group by comparing the current row's group values with
    the previous row's (test_if_group_changed), which is only correct if the rows
    arrive grouped: end_send_group when the plan needs no temp table, and
    end_write_group when it fills one. Given unordered rows they would emit a
    group per run of equal neighbours and answer with too many rows.

    The other two, end_update and end_unique_update, look the group up in the temp
    table by key for every row, so the arrival order does not matter. Those are
    what the optimizer picks for a GROUP BY that no index resolves -- the case
    worth parallelising.

    With no GROUP BY, end_send_group is safe and is what an aggregate query uses:
    there is one implicit group, group_fields is empty and test_if_group_changed
    never reports a change.

    This has to be asked after make_aggr_tables_info() has planned the aggregation
    and chosen the terminals, which is later than can_run_query_in_workers() runs.
    So it is asked here, and answering yes declines the parallel scan the same way
    the engine can, leaving do_select() to run the query serially.
*/
static bool pwt_plan_needs_group_order(JOIN *join)
{
  if (!join->group_list)
    return false;                     // one implicit group, or none

  for (uint i= 0; i < join->aggr_tables; i++)
  {
    JOIN_TAB *at= join->join_tab + join->top_join_tab_count + i;
    if (at->aggr && at->aggr->get_write_func() == end_write_group)
      return true;
  }
  if (!join->aggr_tables)
  {
    JOIN_TAB *last= join->join_tab + join->top_join_tab_count - 1;
    if (last->next_select == end_send_group)
      return true;
  }
  return false;
}


int run_worker_side_join(JOIN *join, JOIN_TAB *scan_tab)
{
  DBUG_ENTER("run_worker_side_join");
  DBUG_PRINT("info",  ("parallel scan on %s", scan_tab->table->alias.ptr()));
  THD *thd= join->thd;
  pwt_manager *mgr= new (thd->mem_root) pwt_manager;
  if (!mgr)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(pwt_manager));
    DBUG_RETURN(1);
  }
  join->parallel_work_manager= mgr;

  if (pwt_plan_needs_group_order(join))
  {
    DBUG_PRINT("info", ("plan needs group-ordered rows, running serially"));
    join->parallel_work_manager= nullptr;
    DBUG_RETURN(-1);
  }

  /*
    JOIN::optimize_distinct() marks the trailing tables a DISTINCT does not
    select from, so that the join stops looking for further matches once one row
    has been produced -- every later match would only make a duplicate for the
    temp table to remove. It runs from make_aggr_tables_info(), after the gate,
    so like the group order this can only be asked here.

    A worker that ignored the flag would still answer correctly, since the
    duplicates it produced would be removed downstream; it would just read more
    than it had to. It is declined rather than ignored because the workers have
    not been shown to honour it, and quietly doing more work than the plan asked
    for is the kind of thing that should be a decision.
  */
  for (uint t= 0; t < join->table_count; t++)
    if (join->join_tab[t].shortcut_for_distinct)
    {
      DBUG_PRINT("info", ("distinct shortcut on a table, running serially"));
      join->parallel_work_manager= nullptr;
      DBUG_RETURN(-1);
    }

  int err= mgr->init_parallel_workers(thd, join, scan_tab);
  if (err == HA_ERR_UNSUPPORTED)
  {
    /*
      The engine declined the parallel scan -- e.g. this is a locking read, so
      not a consistent read (see pscan_init_coordinator). Nothing was set up
      and no error was raised: return the "declined" code so do_select() runs
      the query serially. Returning an error here would fail the statement with
      an empty diagnostics area.
    */
    DBUG_PRINT("info", ("engine declined the parallel scan, running serially"));
    join->parallel_work_manager= nullptr;
    DBUG_RETURN(-1);
  }
  if (err)
    DBUG_RETURN(1);           // init_parallel_workers has raised the error

  /*
    The workers are running, so this query is being executed in parallel. Count
    it here rather than where the optimizer picks the table: the engine can still
    decline above, and the optimizer's choice is recorded in the optimizer trace
    already. This is the only thing that tells a caller, or a test, that
    execution really went through the workers.
  */
  status_var_increment(thd->status_var.parallel_queries_executed);
  thd->status_var.parallel_workers_started+= mgr->nworkers;

  DBUG_RETURN(mgr->manager_collect_and_send(join));
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

void pwt_manager::quiesce_workers()
{
  DBUG_ENTER("pwt_manager::quiesce_workers");
  if (!workers || reaped)
    DBUG_VOID_RETURN;

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
  /*
    The work the workers did was this session's work, so its statistics are the
    session's too. Each worker left them in its pwt_worker before its THD was
    destroyed, and every worker has now been joined, so this thread is the only
    one touching either side and no locking is needed.
  */
  for (uint i= 0; i < nworkers; i++)
    add_to_status(&thd->status_var, &workers[i].stats);

  /*
    Give ANALYZE what the workers did. The manager never runs the driving table's
    read loop, so the JOIN_TAB trackers the optimizer left for ANALYZE to read
    stay at zero and the report says the table was never touched. The trackers
    and the handlers are the manager's, every worker has been joined, so this
    thread is the only one touching either side.
  */
  for (uint i= 0; i < nworkers; i++)
    for (uint t= 0; t < n_tables; t++)
    {
      if (Table_access_tracker *tr= mgr_tabs[t]->tracker)
      {
        /*
          Not the driving table's: sub_select() counts one scan of it per worker
          and the report wants one between them, added below.
        */
        if (t)
          tr->r_scans+=           workers[i].tab_stats[t].r_scans;
        tr->r_rows+=              workers[i].tab_stats[t].r_rows;
        tr->r_rows_after_where+=  workers[i].tab_stats[t].r_rows_after_where;
      }
      if (ha_handler_stats *hs= mgr_tables[t]->file->handler_stats)
        hs->add(&workers[i].tab_hstats[t]);
    }
  /*
    The chunks are one scan of the driving table between them, so report one,
    which is what the serial plan reports and what makes r_rows per scan comparable.
  */
  if (mgr_tabs[0]->tracker)
    mgr_tabs[0]->tracker->r_scans++;
  reaped= true;
  DBUG_VOID_RETURN;
}


/**
  @brief
    Reap the workers (if not already) and tear the channel down.

  @description
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
  /* Read before ending the coordinator, which resets the reader's counters. */
  thd->status_var.parallel_scan_chunks+=
    scan_tab->table->file->pscan_chunks_created();
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
  free_result_tables(thd);              // workers joined; result tables idle
  for (uint i= 0; i < nworkers; i++)    // workers are joined, buffers idle
    my_free(workers[i].batch_rows);
  my_free(workers);
  workers= nullptr;
  nworkers= 0;
  DBUG_VOID_RETURN;
}
