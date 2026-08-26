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

  Result-row transport for parallel query: the row shape the two ends agree on,
  and the batch implementation of the two ends themselves.

  sql_parallel_transport.h says what a transport is and what the interface
  promises; this is the one that exists today. What a worker computes and what
  the manager does with a row once it has arrived are not here -- they are in
  sql_parallel_execution.cc, which reaches the transport only through
  pwt_row_layout, pwt_row_sink and pwt_row_source.
*/

#include "mariadb.h"
#include "mysqld_error.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_class.h"
#include "sql_select.h"
#include "sql_parallel_workers.h"

#ifdef HAVE_PSI_INTERFACE
static PSI_cond_key key_COND_pwt_data_space;
static PSI_cond_info all_pwt_transport_conds[]=
{
  { &key_COND_pwt_data_space, "pwt_batch_source::COND_data_space",  0},
};

void pwt_transport_init_psi_keys(void)
{
  mysql_cond_register("sql", all_pwt_transport_conds,
                      array_elements(all_pwt_transport_conds));
}
#endif /* HAVE_PSI_INTERFACE */


/*****************************************************************************
  pwt_row_layout -- the shape both ends agree on
*****************************************************************************/

/*
  @brief
    Work out what travels, define the columns, and build the manager's
    receiving container.

  @description
    What travels is every base-table column the query reads, in table order.
    See the class comment for why that rather than the projected select list.

    The definition is a list of clones, so the query's own items are never
    bound to a container field, and the manager and every worker build the
    identical layout from it.

  @return  true on error (my_error() has been called).
*/

bool pwt_row_layout::build(THD *thd, JOIN *join_arg, TABLE **tables,
                           uint n_tables)
{
  DBUG_ENTER("pwt_row_layout::build");
  join= join_arg;

  for (uint t= 0; t < n_tables; t++)
  {
    TABLE *tbl= tables[t];
    for (Field **f= tbl->field; *f; f++)
    {
      if (!bitmap_is_set(tbl->read_set, (*f)->field_index))
        continue;
      Item *itf= new (thd->mem_root) Item_field(thd, *f);
      if (!itf || ship_list.push_back(itf, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_field));
        DBUG_RETURN(true);
      }
    }
  }

  /*
    A query reading no column of any table still needs a row shape, because the
    transport measures its rows in record images. "SELECT COUNT(*)" is that
    query: it reads no column, and the count is the number of images that
    arrive.
  */
  if (ship_list.is_empty())
  {
    Item *one= new (thd->mem_root) Item_int(thd, (longlong) 1, 1);
    if (!one || ship_list.push_back(one, thd->mem_root))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item_int));
      DBUG_RETURN(true);
    }
  }

  if (!(copy_back= new (thd->mem_root) Copy_field[ship_list.elements]))
  {
    my_error(ER_OUTOFMEMORY, MYF(0),
             (int) (ship_list.elements * sizeof(Copy_field)));
    DBUG_RETURN(true);
  }

  {
    List_iterator_fast<Item> li(ship_list);
    Item *sel_item;
    while ((sel_item= li++))
    {
      Item *c= sel_item->deep_copy_with_checks(thd);
      if (!c || result_defn.push_back(c, thd->mem_root))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Item));
        DBUG_RETURN(true);
      }
    }
  }

  if (make_container(thd, &recv))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "parallel query: failed to build the result row container");
    DBUG_RETURN(true);
  }
  reclength= recv.table->s->reclength;

  /*
    Pair each container column with the base-table field it was projected from,
    so that receiving a row is a copy per column back into the manager's own
    records. Position i of ship_list is column i of the container, which is how
    make_container() built it. The one item that is not an Item_field is the
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
                                     recv.table->field[i], false);
  }
  DBUG_RETURN(false);
}


/*
  @brief
    Build one container of this layout: the record image both ends copy.

  @description
    A tmp table used for its record buffer and its fields. Whether rows are
    ever written into it through the storage engine is the transport's
    business: the batch transport writes none, and ships images of record[0].

  @return  true on error, false on success (*out set).
*/

bool pwt_row_layout::make_container(THD *thd, pwt_row_container *out)
{
  /*
    A param of its own, per container. Two reasons, and the second is the one
    that bites: create_tmp_table() overwrites param->func_count with the number
    of items it actually has to copy, so a second table built from the same
    param allocates fewer fields than the layout needs -- the assertion in
    Create_tmp_table::finalize() catches that. And create_tmp_table() allocates
    param->start_recinfo out of the table's own mem_root, so the column
    descriptions belong to one container and die with it; sharing a param would
    leave every container but the last describing a table that has been freed.
  */
  TMP_TABLE_PARAM *param= new (thd->mem_root) TMP_TABLE_PARAM;
  if (!param)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(TMP_TABLE_PARAM));
    return true;
  }
  param->init();
  count_field_types(join->select_lex, param, result_defn, false);
  param->skip_create_table= true;

  /*
    TMP_TABLE_ALL_COLUMNS makes create_tmp_table() give every item of the list a
    field, including the constant ones. By default it skips constants, which is
    right for a query that materialises its result and can evaluate them once
    outside the table, but wrong here: the layout has to mirror the definition
    one for one, because the worker projects item i into field i and ships the
    record image, and the manager copies field i back to the column it came
    from. Without this a list holding a constant built a table with fewer fields
    than the projection walks over.
  */
  const ulonglong opts= join->select_options | TMP_TABLE_ALL_COLUMNS;
  TABLE *t= create_tmp_table(thd, param, result_defn,
                             nullptr, false, false,
                             opts, HA_POS_ERROR,
                             &empty_clex_str, true, false);
  if (!t)
    return true;
  if (instantiate_tmp_table(t, param->keyinfo, param->start_recinfo,
                            &param->recinfo, opts, true /*cross_thread*/))
  {
    free_tmp_table(thd, t);
    return true;
  }
  /*
    The whole transport is positional, so a layout that does not match the
    definition item for item would have the two ends reading different columns,
    or walking past the end of the field array. Refuse instead, whatever the
    reason turns out to be.
  */
  DBUG_ASSERT(t->s->fields == result_defn.elements);
  if (t->s->fields != result_defn.elements)
  {
    free_tmp_table(thd, t);
    return true;
  }
  out->table= t;
  out->param= param;
  return false;
}


/*
  The param goes with it: start_recinfo lives in the table's mem_root, which
  free_tmp_table() releases, so keeping the param would keep a description of
  memory that is gone.
*/

void pwt_row_layout::free_container(THD *thd, pwt_row_container *c)
{
  if (c->table)
  {
    free_tmp_table(thd, c->table);
    c->table= nullptr;
  }
  c->param= nullptr;
}


void pwt_row_layout::cleanup(THD *thd)
{
  free_container(thd, &recv);
}


/*
  @brief
    Put the row now in recv_record() back where the query expects to read it.

    One copy per shipped column, container field to the base-table field it was
    projected from. After this the manager's records hold what a serial scan
    would have left there.
*/

void pwt_row_layout::copy_back_row()
{
  for (uint i= 0; i < n_copy_back; i++)
    (*copy_back[i].do_copy)(&copy_back[i]);
}


/*
  @brief
    Make the manager's own records fit to receive rows.

  @description
    The manager's tables were opened but never read, so clear the flags a
    reader would have left. Copy_field captures &table->null_row, and a stale
    null_row would make every copied field read as NULL.

    Writing into these records is not something a SELECT's write_set allows,
    and Field::store() asserts on that, so mark the fields writable for the
    duration. A record is being filled here in place of the reader that would
    normally have filled it, which is what the helper is for.

  @return  true on error.
*/

bool pwt_row_layout::begin_receive(THD *thd, TABLE **tables, uint n_tables)
{
  if (!(saved_write_set= (MY_BITMAP**)
                         thd->alloc(n_tables * sizeof(MY_BITMAP*))))
    return true;
  for (uint t= 0; t < n_tables; t++)
  {
    tables[t]->status= 0;
    tables[t]->null_row= false;
    saved_write_set[t]= dbug_tmp_use_all_columns(tables[t],
                                                 &tables[t]->write_set);
  }
  return false;
}


void pwt_row_layout::end_receive(TABLE **tables, uint n_tables)
{
  if (!saved_write_set)
    return;
  for (uint t= 0; t < n_tables; t++)
    dbug_tmp_restore_column_map(&tables[t]->write_set, saved_write_set[t]);
  saved_write_set= nullptr;
}


/*****************************************************************************
  pwt_batch_sink -- the producing end of the batch transport
*****************************************************************************/

bool pwt_batch_sink::init(pwt_manager *mgr, pwt_batch_source *peer_arg,
                          uint reclength_arg)
{
  manager=   mgr;
  peer=      peer_arg;
  reclength= reclength_arg;
  count=     0;
  full=      false;
  rows= (uchar*) my_malloc(key_memory_pwt_batch_rows,
                           (size_t) PWT_CHUNK_ROWS * reclength, MYF(MY_WME));
  return rows == nullptr;
}


void pwt_batch_sink::cleanup()
{
  my_free(rows);
  rows= nullptr;
}


/*
  @brief
    Hand this worker's filled buffer to the manager.

  @description
    Marks the buffer ready and blocks until the manager has drained it (clears
    'full') or asks the producers to stop. On return the buffer is the worker's
    again: either to refill, or to abandon.

    This is the only place a worker waits for the manager, and it is what the
    temporary-table transport exists to remove.

  @return
    true   the consumer asked us to stop
    false  the buffer was drained; refill it
*/

bool pwt_batch_sink::handoff()
{
  DBUG_ENTER("pwt_batch_sink::handoff");
  mysql_mutex_lock(&manager->LOCK_data);
  if (manager->stop)
  {
    mysql_mutex_unlock(&manager->LOCK_data);
    DBUG_RETURN(true);
  }
  full= true;
  mysql_cond_signal(&manager->COND_data_avail);          // wake the consumer
  while (full && !manager->stop)
  {
    mysql_cond_wait(&peer->COND_data_space, &manager->LOCK_data);
    DBUG_PRINT("info", ("worker wakes"));
  }
  bool stopped= manager->stop;
  mysql_mutex_unlock(&manager->LOCK_data);
  DBUG_RETURN(stopped);
}


/*
  @brief
    Take one finished row: copy its record image into the buffer, handing the
    buffer over when it fills.
*/

int pwt_batch_sink::emit_row(const uchar *rec)
{
  memcpy(rows + (size_t) count * reclength, rec, reclength);
  if (++count == PWT_CHUNK_ROWS)
  {
    if (handoff())                                // manager asked us to stop
      return PWT_EMIT_STOP;
    count= 0;                                     // buffer drained; refill
  }
  return PWT_EMIT_OK;
}


/*
  Hand over the final partial buffer. A stop arriving now is not interesting:
  this producer has finished anyway.
*/

bool pwt_batch_sink::flush()
{
  if (count)
    handoff();
  return false;
}


/*****************************************************************************
  pwt_batch_source -- the consuming end of the batch transport
*****************************************************************************/

bool pwt_batch_source::init(THD *thd, pwt_manager *mgr, uint n_workers,
                            uint reclength_arg)
{
  manager=   mgr;
  n_sinks=   n_workers;
  reclength= reclength_arg;
  cur=       nullptr;
  cur_cursor= 0;
  if (!(sinks= thd->alloc<pwt_batch_sink*>(n_workers)))
    return true;
  for (uint i= 0; i < n_workers; i++)
    sinks[i]= nullptr;
  mysql_cond_init(key_COND_pwt_data_space, &COND_data_space, nullptr);
  inited= true;
  return false;
}


pwt_row_sink *pwt_batch_source::make_sink(THD *thd, uint worker_nr,
                                          pwt_row_container *container)
{
  DBUG_ASSERT(worker_nr < n_sinks);
  /*
    Not used: this transport copies the record out of the container rather than
    keeping it, so it needs only the size, which it has from the layout.
  */
  (void) container;
  pwt_batch_sink *s= new (thd->mem_root) pwt_batch_sink;
  if (!s || s->init(manager, this, reclength))
  {
    my_error(ER_OUTOFMEMORY, MYF(0),
             (int) (PWT_CHUNK_ROWS * reclength));
    return nullptr;
  }
  sinks[worker_nr]= s;
  return s;
}


/*
  Release every producer blocked waiting for its buffer back, so it sees the
  manager's stop request. The caller sets that request; this only wakes them.
*/

void pwt_batch_source::wake_producers()
{
  mysql_mutex_assert_owner(&manager->LOCK_data);
  mysql_cond_broadcast(&COND_data_space);
}


void pwt_batch_source::cleanup()
{
  if (inited)
  {
    mysql_cond_destroy(&COND_data_space);
    inited= false;
  }
}


/*
  @brief
    Copy the next result row's record image into dst.

  @description
    Drains one worker's buffer at a time (cur), advancing cur_cursor through
    its rows; when the buffer is exhausted it releases that worker to refill
    and picks the next ready one. Blocks when no buffer is momentarily ready.

    This is also the manager's only wait, so it is where the team's own state
    is noticed: a worker killed is propagated to the manager's THD, a worker
    error aborts, and "no buffer ready and nobody still running" is end of
    data.

  @return
    0 = row copied into dst,  -1 = end of data,  1 = error.
*/

int pwt_batch_source::next_row(uchar *dst)
{
  DBUG_ENTER("pwt_batch_source::next_row");
  THD *thd= manager->thd;
  struct timespec wait;
  wait.tv_nsec= 0;

  for (;;)
  {
    if (cur)                                      // draining a worker's buffer
    {
      if (cur_cursor < cur->count)
      {
        memcpy(dst, cur->rows + (size_t) cur_cursor * reclength, reclength);
        cur_cursor++;
        DBUG_RETURN(0);
      }
      // buffer drained; release the worker so it can refill
      mysql_mutex_lock(&manager->LOCK_data);
      pwt_batch_sink *drained= cur;
      cur= nullptr;
      drained->full= false;                     // buffer is the worker's again
      mysql_cond_broadcast(&COND_data_space);   // wake it to refill
      mysql_mutex_unlock(&manager->LOCK_data);
      // fall through and look for the next ready worker
    }

    // find the next worker whose buffer is filled and ready
    pwt_batch_sink *next= nullptr;
    PSI_stage_info old_stage;
    mysql_mutex_lock(&manager->LOCK_data);
    for (;;)
    {
      for (uint i= 0; i < n_sinks; i++)
        if (sinks[i] && sinks[i]->full)
        {
          next= sinks[i];
          break;
        }
      if (next)
        break;
      /*
        A worker exited because it was killed: propagate the kill to the
        manager's own THD so the query aborts now with ER_QUERY_INTERRUPTED,
        before any result is sent.
      */
      if (manager->killed_by_worker() != NOT_KILLED && !thd->killed)
      {
        killed_state ks= manager->killed_by_worker();
        mysql_mutex_unlock(&manager->LOCK_data);
        mysql_mutex_lock(&thd->LOCK_thd_kill);
        thd->killed= ks;
        mysql_mutex_unlock(&thd->LOCK_thd_kill);
        DBUG_RETURN(1);
      }
      if (manager->fatal_error)                     // a worker failed
      {
        mysql_mutex_unlock(&manager->LOCK_data);
        DBUG_RETURN(1);
      }
      if (!manager->active_workers)             // all producers done, drained
      {
        mysql_mutex_unlock(&manager->LOCK_data);
        DBUG_RETURN(-1);
      }
      if (thd->killed)
      {
        mysql_mutex_unlock(&manager->LOCK_data);
        DBUG_RETURN(1);
      }
      // wait for a batch, a finishing worker, or a 1s tick to re-check killed.
      // ENTER_COND/EXIT_COND publish the "Reading data from parallel workers"
      // stage and register the cond so a KILL of the manager wakes it.
      wait.tv_sec= time(0) + 1;
      thd->ENTER_COND(&manager->COND_data_avail, &manager->LOCK_data,
                      &stage_reading_data_from_parallel_worker, &old_stage);
      mysql_cond_timedwait(&manager->COND_data_avail, &manager->LOCK_data,
                           &wait);
      thd->EXIT_COND(&old_stage);                 // unlocks LOCK_data
      mysql_mutex_lock(&manager->LOCK_data);      // re-lock for the next pass
    }
    cur= next;
    cur_cursor= 0;                                // start of next's buffer
    mysql_mutex_unlock(&manager->LOCK_data);
    // loop back and drain cur
  }
}


/*****************************************************************************
  pwt_tmp_table_sink -- the producing end of the temporary-table transport
*****************************************************************************/

bool pwt_tmp_table_sink::init(pwt_manager *mgr, pwt_tmp_table_source *peer_arg,
                              pwt_row_container *container_arg)
{
  manager=   mgr;
  peer=      peer_arg;
  container= container_arg;
  return false;
}


/*
  Runs on the worker's thread, before its first row. Take the container: the
  engine accounts a write to TABLE::in_use's status counters, and a projection
  into a field asks in_use for the session's time zone and sql_mode. Until now
  it named the manager, which built the container.
*/

bool pwt_tmp_table_sink::begin()
{
  container->table->in_use= current_thd;
  return false;
}


/*
  @brief  Has the consumer asked us to stop?

  @description
    Read under the lock rather than peeked at, and only once every so many rows
    -- the answer only ever changes once, and reading it costs the same mutex
    the batch transport takes to hand a buffer over. Stopping is not required
    for correctness here: the manager cannot ask before it has drained
    everything it wants, so this only saves a worker from finishing work
    nobody will read.
*/

bool pwt_tmp_table_sink::stop_requested()
{
  mysql_mutex_lock(&manager->LOCK_data);
  bool stop= manager->stop;
  mysql_mutex_unlock(&manager->LOCK_data);
  return stop;
}


/*
  @brief  Keep one finished row.

  @description
    The row is already in the container's record buffer -- the projection put
    it there -- so storing it is one engine write and no copy.
*/

int pwt_tmp_table_sink::emit_row(const uchar *rec)
{
  TABLE *table= container->table;
  DBUG_ASSERT(rec == table->record[0]);
  (void) rec;

  int err= table->file->ha_write_tmp_row(table->record[0]);
  if (unlikely(err))
  {
    if (err == HA_ERR_RECORD_FILE_FULL)
    {
      /*
        Where the heap-to-disk conversion would go, and what it would need is
        now here: create_internal_tmp_table_from_heap() is driven from
        container->param->start_recinfo, which describes this container and no
        other. What is still missing is a thread it can run on -- it re-opens
        the table, and Aria's open binds the handle to the opening thread's
        my_thread_var, which for a worker is freed as soon as that worker's THD
        is destroyed. So this still fails the statement rather than pretending,
        and a result set has to fit the session's tmp table limit.
      */
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "parallel query: the worker's result table is full "
               "(spilling a worker result set to disk is not implemented)");
    }
    else
      table->file->print_error(err, MYF(0));
    return PWT_EMIT_ERROR;
  }

  if (++since_check == PWT_CHUNK_ROWS)
  {
    since_check= 0;
    if (stop_requested())
      return PWT_EMIT_STOP;
  }
  return PWT_EMIT_OK;
}


/*
  This result set is complete. Publishing 'done' is the whole hand-off: from
  here the container is the manager's to read, and this thread does not touch
  it again.
*/

bool pwt_tmp_table_sink::flush()
{
  mysql_mutex_lock(&manager->LOCK_data);
  done= true;
  mysql_cond_signal(&manager->COND_data_avail);
  mysql_mutex_unlock(&manager->LOCK_data);
  return false;
}


/*
  Give the container back to the manager before it frees it. Called on the
  manager's thread with every worker joined, so a container this worker took
  and never returned -- it failed, or was killed -- would otherwise be freed
  naming a THD that no longer exists.
*/

void pwt_tmp_table_sink::cleanup()
{
  if (container)
  {
    if (container->table)
      container->table->in_use= manager->thd;
    container= nullptr;
  }
}


/*****************************************************************************
  pwt_tmp_table_source -- the consuming end of the temporary-table transport
*****************************************************************************/

bool pwt_tmp_table_source::init(THD *thd, pwt_manager *mgr, uint n_workers,
                                uint reclength_arg)
{
  manager=   mgr;
  n_sinks=   n_workers;
  reclength= reclength_arg;
  if (!(sinks= thd->alloc<pwt_tmp_table_sink*>(n_workers)))
    return true;
  for (uint i= 0; i < n_workers; i++)
    sinks[i]= nullptr;
  return false;
}


pwt_row_sink *pwt_tmp_table_source::make_sink(THD *thd, uint worker_nr,
                                              pwt_row_container *container)
{
  DBUG_ASSERT(worker_nr < n_sinks);
#ifndef DBUG_OFF
  /*
    No two containers may share their column descriptions. Rebuilding a full
    container on disk is driven from them, and they are allocated out of the
    table's own mem_root, so one param between containers describes only the
    last one built and points into memory freed with it. Every container is the
    same shape, so nothing about the values would give that away -- this is the
    check that fails on the code this replaced.
  */
  for (uint j= 0; j < n_sinks; j++)
    DBUG_ASSERT(!sinks[j] || sinks[j]->container->param != container->param);
#endif
  pwt_tmp_table_sink *s= new (thd->mem_root) pwt_tmp_table_sink;
  if (!s || s->init(manager, this, container))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(pwt_tmp_table_sink));
    return nullptr;
  }
  sinks[worker_nr]= s;
  return s;
}


/*
  @brief
    Claim the next complete result set, waiting for one if need be.

  @description
    Claimed rather than merely found: the sink is marked taken under the same
    lock that made it visible, so it cannot be scanned twice.

    This is the manager's only wait, so it is also where the team's own state
    is noticed -- a worker killed, a worker failed, and "nothing left to read
    and nobody still running" as end of data. Waiting on active_workers rather
    than on every sink being done is what covers a worker that exits without
    flushing at all.

  @return  0 = *out claimed,  -1 = end of data,  1 = error (reported).
*/

int pwt_tmp_table_source::claim_next_result(pwt_tmp_table_sink **out)
{
  THD *thd= manager->thd;
  PSI_stage_info old_stage;
  struct timespec wait;
  wait.tv_nsec= 0;

  mysql_mutex_lock(&manager->LOCK_data);
  for (;;)
  {
    for (uint i= 0; i < n_sinks; i++)
      if (sinks[i] && sinks[i]->done && !sinks[i]->taken)
      {
        sinks[i]->taken= true;
        mysql_mutex_unlock(&manager->LOCK_data);
        *out= sinks[i];
        return 0;
      }
    /*
      A worker exited because it was killed: propagate the kill to the
      manager's own THD so the query aborts now with ER_QUERY_INTERRUPTED,
      before any result is sent.
    */
    if (manager->killed_by_worker() != NOT_KILLED && !thd->killed)
    {
      killed_state ks= manager->killed_by_worker();
      mysql_mutex_unlock(&manager->LOCK_data);
      mysql_mutex_lock(&thd->LOCK_thd_kill);
      thd->killed= ks;
      mysql_mutex_unlock(&thd->LOCK_thd_kill);
      return 1;
    }
    if (manager->fatal_error)                       // a worker failed
    {
      mysql_mutex_unlock(&manager->LOCK_data);
      return 1;
    }
    if (!manager->active_workers)         // nothing unread, nobody running
    {
      mysql_mutex_unlock(&manager->LOCK_data);
      return -1;
    }
    if (thd->killed)
    {
      mysql_mutex_unlock(&manager->LOCK_data);
      return 1;
    }
    // wait for a result set, a finishing worker, or a 1s tick to re-check
    // killed. ENTER_COND/EXIT_COND publish the "Reading data from parallel
    // workers" stage and register the cond so a KILL of the manager wakes it.
    wait.tv_sec= time(0) + 1;
    thd->ENTER_COND(&manager->COND_data_avail, &manager->LOCK_data,
                    &stage_reading_data_from_parallel_worker, &old_stage);
    mysql_cond_timedwait(&manager->COND_data_avail, &manager->LOCK_data, &wait);
    thd->EXIT_COND(&old_stage);                     // unlocks LOCK_data
    mysql_mutex_lock(&manager->LOCK_data);          // re-lock for the next pass
  }
}


/*
  @brief
    Copy the next result row's record image into dst.

  @description
    Scans one finished worker's container to its end, then claims the next.
    Rows therefore arrive in whole-worker runs; the interface promises no
    order and the manager's drain loop assumes none.

  @return
    0 = row copied into dst,  -1 = end of data,  1 = error.
*/

int pwt_tmp_table_source::next_row(uchar *dst)
{
  DBUG_ENTER("pwt_tmp_table_source::next_row");

  for (;;)
  {
    if (cur)
    {
      TABLE *t= cur->container->table;
      int err= t->file->ha_rnd_next(t->record[0]);
      if (likely(!err))
      {
        memcpy(dst, t->record[0], reclength);
        DBUG_RETURN(0);
      }
      if (err != HA_ERR_END_OF_FILE)
      {
        t->file->print_error(err, MYF(0));
        DBUG_RETURN(1);
      }
      release_position();                    // this result set is exhausted
    }

    pwt_tmp_table_sink *next;
    int rc= claim_next_result(&next);
    if (rc)
      DBUG_RETURN(rc);                       // -1 end of data, 1 error

    /*
      Take the container back from the worker that filled it before reading:
      the engine accounts what we read to TABLE::in_use. Safe without further
      locking -- claim_next_result() saw 'done', which that worker published
      under LOCK_data as the last thing it did to this container.
    */
    TABLE *t= next->container->table;
    t->in_use= manager->thd;
    if (int err= t->file->ha_rnd_init(true))
    {
      t->file->print_error(err, MYF(0));
      DBUG_RETURN(1);
    }
    cur= next;
    scan_open= true;
  }
}


/*
  End the open scan. The containers are freed just after the workers are
  reaped, and a scan left open would be one the manager still holds on a table
  about to go.
*/

void pwt_tmp_table_source::release_position()
{
  if (cur && scan_open)
    cur->container->table->file->ha_rnd_end();
  cur= nullptr;
  scan_open= false;
}


/*****************************************************************************
  Choosing a transport
*****************************************************************************/

/*
  The temporary-table transport, except where a test asks for the batch one.

  pwt_batch_* is kept while the two are being compared, and the switch is what
  keeps it honest: dead code that cannot be run is code that stops working
  without anyone finding out. Delete the DBUG_EXECUTE_IF and the batch classes
  together when the comparison is over.
*/

pwt_row_source *pwt_create_transport(THD *thd, pwt_manager *mgr,
                                     uint n_workers, uint reclength)
{
  bool use_batch= false;
  DBUG_EXECUTE_IF("pwt_batch_transport", use_batch= true;);

  pwt_row_source *src;
  if (use_batch)
  {
    pwt_batch_source *b= new (thd->mem_root) pwt_batch_source;
    src= b;
    if (b && b->init(thd, mgr, n_workers, reclength))
      src= nullptr;
  }
  else
  {
    pwt_tmp_table_source *t= new (thd->mem_root) pwt_tmp_table_source;
    src= t;
    if (t && t->init(thd, mgr, n_workers, reclength))
      src= nullptr;
  }
  if (!src)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(pwt_tmp_table_source));
    return nullptr;
  }
  return src;
}
