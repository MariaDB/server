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
  */
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
  @brief
    Project the current full row and add its image to the worker's batch,
    handing the batch to the manager when it fills.

  @return  
    -1 = continue with inner join
    0 = row emitted
    1 = stop (manager asked us to stop, or a fatal error).
*/

int pwt_worker::worker_emit_row(uint level)
{
  DBUG_ENTER("pwt_worker::worker_emit_row");
  if (level + 1 < n_tables)
    DBUG_RETURN(-1);
  /*
    Evaluate each cloned select-list item (which reads from the worker's table
    copies, now holding the current matching row of every table) and store it
    into the matching result_table field. Evaluation errors are intercepted by
    PWT_error_handler, which trips fatal_error; the caller checks that.
  */
  for (uint i= 0; i < proj_count; i++)
    worker_proj[i]->save_in_field(result_table->field[i], false);

  if (manager->fatal_error)                       // projection raised an error
    DBUG_RETURN(1);

  memcpy(batch_rows + (size_t) batch_count * manager->reclength,
         result_table->record[0], manager->reclength);
  if (++batch_count == PWT_CHUNK_ROWS)
  {
    if (manager->handoff_batch(this))             // manager asked us to stop
      DBUG_RETURN(1);
    batch_count= 0;                               // buffer drained; refill
  }
  DBUG_RETURN(0);
}


/*
  @brief
    Join the non-driving tables, levels [level .. n_tables-2], against the rows
    of the earlier tables already in their record[0], and emit each full match.

  Inner equi-join only (the gate excludes outer joins and semijoins): for a
  REF/EQ_REF table the key is built from the earlier tables (cp_buffer_from_ref)
  and looked up in the worker's private index; for a JT_ALL table the worker's
  copy is rnd-scanned. Each table's cloned condition is applied as we descend,
  which is where the optimizer left the multi-table predicates, so together
  they reconstitute the whole WHERE.

  @return  0 = continue, 1 = stop (manager stop or fatal error).
*/

int pwt_worker::worker_join_inner(uint level)
{
  DBUG_ENTER("pwt_worker::worker_join_inner");
  int ret= worker_emit_row(level);
  if (ret >= 0)
    DBUG_RETURN(ret);

  pwt_jointab *it= &join_tables[level];
  TABLE *t= it->table;
  int err;

  if (it->type == JT_ALL)
  {
    if ((err= t->file->ha_rnd_init(true)))
    {
      manager->fatal_error= true;
      t->file->print_error(err, MYF(0));
      DBUG_RETURN(1);
    }
    while (!(err= t->file->ha_rnd_next(t->record[0])))
    {
      if (it->cond)
      {
        bool pass= it->cond->val_bool();
        if (manager->fatal_error)
        {
          t->file->ha_rnd_end();
          DBUG_RETURN(1);
        }
        if (!pass)
          continue;
      }
      if (worker_join_inner(level+1))
      {
        t->file->ha_rnd_end();
        DBUG_RETURN(1);
      }
    }
    t->file->ha_rnd_end();
    if (err != HA_ERR_END_OF_FILE)
    {
      manager->fatal_error= true;
      t->file->print_error(err, MYF(0));
      DBUG_RETURN(1);
    }
    DBUG_RETURN(0);
  }

  // REF / EQ_REF: build the lookup key from the earlier tables and probe.
  // cp_buffer_from_ref returns true if a null-rejecting key part is NULL,
  // i.e. there can be no match -> just backtrack.
  if (cp_buffer_from_ref(thd, t, &it->ref))
    DBUG_RETURN(0);
  err= t->file->ha_index_read_map(t->record[0], it->ref.key_buff,
                                  make_prev_keypart_map(it->ref.key_parts),
                                  HA_READ_KEY_EXACT);
  while (!err)
  {
    bool pass= !it->cond || it->cond->val_bool();
    if (manager->fatal_error)
      DBUG_RETURN(1);
    if (pass && worker_join_inner(level + 1))
      DBUG_RETURN(1);
    if (it->type == JT_EQ_REF)
      break;                                     // unique key: at most one match
    err= t->file->ha_index_next_same(t->record[0], it->ref.key_buff,
                                     it->ref.key_length);
  }
  if (err && err != HA_ERR_KEY_NOT_FOUND && err != HA_ERR_END_OF_FILE)
  {
    manager->fatal_error= true;
    t->file->print_error(err, MYF(0));
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/**
  @brief
    Run the query over the worker's private chunk of the driving table, joining
    the other tables, and stream the *result* rows to the manager a batch at a
    time through the worker's reused row buffer.

  The worker scans its own copy of the driving table (our_scan_table, opened
  with in_use == this worker's thd) so the workers scan concurrently with no
  shared-scan lock. For each driving row that passes the pushed WHERE it joins
  the remaining tables (worker_join_inner) and, for every full match, projects
  the select list into result_table and ships that record image. The manager
  (manager_collect_and_send) only concatenates these final rows.

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
  for (i= 0; i + 1 < nt; i++)
  {
    pwt_jointab *it= &join_tables[i];
    if ((it->type == JT_EQ_REF || it->type == JT_REF) &&
        (err= it->table->file->ha_index_init(it->ref.key, it->sorted)))
      break;
  }

  if (!err && !(err= src->file->pscan_init_worker(engine_ctx)))
  {
    batch_count= 0;
    bool eof= false, killed= false;
    while (!eof && !killed)
    {
      // honour a direct KILL of this worker's thread
      mysql_mutex_lock(&thd->LOCK_thd_kill);
      killed= thd->killed;
      mysql_mutex_unlock(&thd->LOCK_thd_kill);
      if (killed)
      {
        my_error(ER_QUERY_INTERRUPTED, MYF(0));
        break;
      }

      if ((err= src->file->ha_pscan_get_next_row(engine_ctx)))
      {
        if (err == HA_ERR_END_OF_FILE)
        {
          err= 0;
          eof= true;
        }
        break;
      }

      // apply per worker select_cond
      if (worker_cond)
      {
        bool pass= worker_cond->val_bool();
        if (mgr->fatal_error)
        {
          eof= true;
          break;
        }
        if (!pass)         // skip below
          continue;
      }

      // join the rest of the tables and emit each full match
      if (worker_join_inner(0))
      {
        eof= true;
        break;
      }
    }
    src->file->pscan_end_worker();

    // hand off the final partial batch (ignore a late stop -- we are done)
    if (!err && !killed && !mgr->fatal_error && batch_count)
      mgr->handoff_batch(this);
  }
  else if (err == HA_ERR_END_OF_FILE)
    err= 0;

  // end any open index/rnd scans (no-op for tables left in NONE state), unlock
  for (i= 0; i + 1 < nt; i++)
    join_tables[i].table->file->ha_index_or_rnd_end();
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
  worker->close_worker_tables();

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
    if (setup_worker_inner_tabs(thd, workers + i) ||
        make_result_table(thd, result_defn, &workers[i].result_table) ||
        clone_worker_exprs(thd, workers + i))
    {
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "init_parallel_workers: failed to set up worker execution");
      goto cleanup_thread_create;
    }

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
  int ret= 0;

  /*
    The manager's tables were opened but never read, so clear the flags a reader
    would have left. Copy_field captures &table->null_row, and a stale null_row
    would make every copied field read as NULL.

    Writing into these records is not something a SELECT's write_set allows, and
    Field::store() asserts on that, so mark the fields writable for the drain. A
    record is being filled here in place of the reader that would normally have
    filled it, which is what the helper is for.
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

    /* Put the shipped columns back where the query expects to find them. */
    for (uint i= 0; i < n_copy_back; i++)
      (*copy_back[i].do_copy)(&copy_back[i]);

    int err= join->result->send_data_with_check(join->fields_list, join->unit,
                                                 join->send_records);
    if (unlikely(err))
    {
      if (err > 0)
      {
        ret= 1;
        break;
      }
      join->duplicate_rows++;                     // err < 0: duplicate row
    }
    join->send_records++;
    join->accepted_rows++;
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
  if (!(worker->worker_tables= thd->alloc<TABLE*>(n_tables)))
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
  @brief
    Describe how this worker joins each non-driving table: access method, a
    worker-bound clone of the ref (for REF/EQ_REF), and a cloned + rebound
    per-table condition. Tables are taken in join order; their ref values
    reference earlier (already-read) tables, which is why the worker joins
    them in this order.

  @return  true on error.
*/
bool pwt_manager::setup_worker_inner_tabs(THD *thd, pwt_worker *worker)
{
  const uint m= n_tables - 1;            // number of non-driving tables
  worker->join_tables= nullptr;
  if (!m)
    return false;                        // single table: no inner join

  if (!(worker->join_tables= thd->calloc<pwt_jointab>(m)))
    return true;

  for (uint k= 0; k < m; k++)
  {
    JOIN_TAB *mtab= mgr_tabs[k + 1];     // manager's non-driving tab
    pwt_jointab *it= &worker->join_tables[k];
    it->table= worker->worker_tables[k + 1];
    it->type= mtab->type;
    it->sorted= mtab->sorted;

    if (mtab->select_cond &&
        !(it->cond= pwt_clone_rebind(thd, mtab->select_cond,
                                     mgr_tables, worker->worker_tables,
                                     n_tables)))
      return true;

    if (it->type == JT_EQ_REF || it->type == JT_REF)
    {
      if (clone_table_ref(thd, &mtab->ref, it->table,
                          mgr_tables, worker->worker_tables, n_tables, &it->ref))
        return true;
    }
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

  // possible select_cond pushed to the driving table
  worker->worker_cond= nullptr;
  if (scan_tab->select_cond &&
      !(worker->worker_cond= pwt_clone_rebind(thd, scan_tab->select_cond,
                                              from, to, n_tables)))
    return true;

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
    - no LIMIT/OFFSET, SQL_CALC_FOUND_ROWS, aggregate, or PROCEDURE -- these
      need a global post-pass the workers cannot do independently;
    - every condition, ref value and select-list item can be deep-cloned (so
      each worker gets private, thread-safe copies bound to its own tables).

  Anything else simply runs serially.

  @return
        true  if we can run this query in parallel
        false otherwise
*/
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
  if (join->need_tmp)                             // group/distinct/order/...
  {
    DBUG_PRINT("info", ("group/distinct/order by"));
    DBUG_RETURN(false);
  }
  if (sl->limit_params.explicit_limit)            // LIMIT / OFFSET
  {
    DBUG_PRINT("info", ("limit/offset"));
    DBUG_RETURN(false);
  }
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
  if (sl->agg_func_used() || sl->with_sum_func )
  {
    DBUG_PRINT("info", ("aggregate funcs"));
    DBUG_RETURN(false);
  }

  if (join->group_list || join->group)
  {
    DBUG_PRINT("info", ("group"));
    DBUG_RETURN(false);
  }
  if (join->select_distinct)
  {
    DBUG_PRINT("info", ("distinct"));
    DBUG_RETURN(false);
  }
  if (join->order)
  {
    DBUG_PRINT("info", ("order by"));
    DBUG_RETURN(false);
  }
  if (join->having || join->tmp_having)
  {
    DBUG_PRINT("info", ("having"));
    DBUG_RETURN(false);
  }
  if (join->outer_join)                             // no outer joins
  {
    DBUG_PRINT("info", ("outer_join"));
    DBUG_RETURN(false);
  }

  // every non-const join table must be one the worker can scan / look up itself
  for (uint j= join->const_tables; j < join->table_count; j++)
  {
    JOIN_TAB *tab= &join->join_tab[j];
    if (tab->bush_children)                       // semijoin materialization
    {
      DBUG_PRINT("info", ("SJM on %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
    if (tab->loosescan_match_tab)                 // loose scan
    {
      DBUG_PRINT("info", ("loosescan on %s",tab->table->alias.ptr()));
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
          if (!tab->ref.items[p]->deep_copy_with_checks(thd))   // clonable?
          {
            DBUG_PRINT("info", ("ref unclonable, %s",tab->table->alias.ptr()));
            DBUG_RETURN(false);
          }
        }
    }
    if (tab->select_cond && !tab->select_cond->deep_copy_with_checks(thd))
    {
      DBUG_PRINT("info", ("cond unclonable, %s",tab->table->alias.ptr()));
      DBUG_RETURN(false);
    }
  }

  // select list clonable
  List_iterator_fast<Item> li(join->fields_list);
  Item *item;
  while ((item= li++))
    if (!item->deep_copy_with_checks(thd))
    {
      DBUG_PRINT("info", ("scan table fields unclonable"));
      DBUG_RETURN(false);
    }
  DBUG_RETURN(true);
}


/*
  @brief
    Drive worker-side execution from do_select(): spin up the workers and then
    collect and send their result rows.

  @return  0 = handled (result sent), 1 = error, -1 = the engine declined the
           parallel scan (caller should run the query serially instead).
*/
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
