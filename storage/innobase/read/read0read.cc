/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file read/read0read.cc
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#include "read0types.h"

#include "srv0srv.h"
#include "trx0sys.h"
#include "trx0purge.h"

#include "trx0rec.h"
#include "trx0rseg.h"
#include "row0row.h"

/*
-------------------------------------------------------------------------------
FACT A: Cursor read view on a secondary index sees only committed versions
-------
of the records in the secondary index or those versions of rows created
by transaction which created a cursor before cursor was created even
if transaction which created the cursor has changed that clustered index page.

PROOF: We must show that read goes always to the clustered index record
to see that record is visible in the cursor read view. Consider e.g.
following table and SQL-clauses:

create table t1(a int not null, b int, primary key(a), index(b));
insert into t1 values (1,1),(2,2);
commit;

Now consider that we have a cursor for a query

select b from t1 where b >= 1;

This query will use secondary key on the table t1. Now after the first fetch
on this cursor if we do a update:

update t1 set b = 5 where b = 2;

Now second fetch of the cursor should not see record (2,5) instead it should
see record (2,2).

We also should show that if we have delete t1 where b = 5; we still
can see record (2,2).

When we access a secondary key record maximum transaction id is fetched
from this record and this trx_id is compared to up_limit_id in the view.
If trx_id in the record is greater or equal than up_limit_id in the view
cluster record is accessed.  Because trx_id of the creating
transaction is stored when this view was created to the list of
trx_ids not seen by this read view previous version of the
record is requested to be built. This is build using clustered record.
If the secondary key record is delete-marked, its corresponding
clustered record can be already be purged only if records
trx_id < low_limit_no. Purge can't remove any record deleted by a
transaction which was active when cursor was created. But, we still
may have a deleted secondary key record but no clustered record. But,
this is not a problem because this case is handled in
row_sel_get_clust_rec() function which is called
whenever we note that this read view does not see trx_id in the
record. Thus, we see correct version. Q. E. D.

-------------------------------------------------------------------------------
FACT B: Cursor read view on a clustered index sees only committed versions
-------
of the records in the clustered index or those versions of rows created
by transaction which created a cursor before cursor was created even
if transaction which created the cursor has changed that clustered index page.

PROOF:  Consider e.g.following table and SQL-clauses:

create table t1(a int not null, b int, primary key(a));
insert into t1 values (1),(2);
commit;

Now consider that we have a cursor for a query

select a from t1 where a >= 1;

This query will use clustered key on the table t1. Now after the first fetch
on this cursor if we do a update:

update t1 set a = 5 where a = 2;

Now second fetch of the cursor should not see record (5) instead it should
see record (2).

We also should show that if we have execute delete t1 where a = 5; after
the cursor is opened we still can see record (2).

When accessing clustered record we always check if this read view sees
trx_id stored to clustered record. By default we don't see any changes
if record trx_id >= low_limit_id i.e. change was made transaction
which started after transaction which created the cursor. If row
was changed by the future transaction a previous version of the
clustered record is created. Thus we see only committed version in
this case. We see all changes made by committed transactions i.e.
record trx_id < up_limit_id. In this case we don't need to do anything,
we already see correct version of the record. We don't see any changes
made by active transaction except creating transaction. We have stored
trx_id of creating transaction to list of trx_ids when this view was
created. Thus we can easily see if this record was changed by the
creating transaction. Because we already have clustered record we can
access roll_ptr. Using this roll_ptr we can fetch undo record.
We can now check that undo_no of the undo record is less than undo_no of the
trancaction which created a view when cursor was created. We see this
clustered record only in case when record undo_no is less than undo_no
in the view. If this is not true we build based on undo_rec previous
version of the record. This record is found because purge can't remove
records accessed by active transaction. Thus we see correct version. Q. E. D.
-------------------------------------------------------------------------------
FACT C: Purge does not remove any delete-marked row that is visible
-------
in any cursor read view.

PROOF: We know that:
 1: Currently active read views in trx_sys_t::view_list are ordered by
    ReadView::low_limit_no in descending order, that is,
    newest read view first.

 2: Purge clones the oldest read view and uses that to determine whether there
    are any active transactions that can see the to be purged records.

Therefore any joining or active transaction will not have a view older
than the purge view, according to 1.

When purge needs to remove a delete-marked row from a secondary index,
it will first check that the DB_TRX_ID value of the corresponding
record in the clustered index is older than the purge view. It will
also check if there is a newer version of the row (clustered index
record) that is not delete-marked in the secondary index. If such a
row exists and is collation-equal to the delete-marked secondary index
record then purge will not remove the secondary index record.

Delete-marked clustered index records will be removed by
row_purge_remove_clust_if_poss(), unless the clustered index record
(and its DB_ROLL_PTR) has been updated. Every new version of the
clustered index record will update DB_ROLL_PTR, pointing to a new UNDO
log entry that allows the old version to be reconstructed. The
DB_ROLL_PTR in the oldest remaining version in the old-version chain
may be pointing to garbage (an undo log record discarded by purge),
but it will never be dereferenced, because the purge view is older
than any active transaction.

For details see: row_vers_old_has_index_entry() and row_purge_poss_sec()
*/


/**
  Creates a snapshot where exactly the transactions serialized before this
  point in time are seen in the view.

  @param[in,out] trx transaction
*/
inline void ReadViewBase::snapshot(trx_t *trx)
{
#ifdef WITH_INNODB_SCN
  if (innodb_use_scn)
  {
    m_low_limit_no= scn_mgr.safe_limit_no();
    if (unlikely(m_low_limit_no == 0))
    {
      trx_id_t id= 0;
      trx_id_t no= 0;

      trx_sys.get_min_trx_id_no(id, no);

      m_up_limit_id= id;
      m_low_limit_no= no;

      ut_a(m_low_limit_no > 0);
    }
    else
    {
      m_up_limit_id= scn_mgr.min_active_id();
    }

    if (trx != nullptr && m_up_limit_id == 0)
    {
      m_up_limit_id= trx_sys.get_min_trx_id();
    }

    m_version= trx_sys.get_max_trx_scn();
    m_low_limit_id= trx_sys.get_max_trx_id();
    m_ids.clear();
    m_committing_scns.clear();
    m_committing_ids.clear();

    if (unlikely(m_low_limit_no > m_version))
    {
      m_low_limit_no= m_version;
    }
  }
  else
#endif
  {
    trx_sys.snapshot_ids(trx, &m_ids, &m_low_limit_id, &m_low_limit_no);
    if (m_ids.empty())
    {
      m_up_limit_id= m_low_limit_id;
      return;
    }

    std::sort(m_ids.begin(), m_ids.end());
    m_up_limit_id= m_ids.front();
    ut_ad(m_up_limit_id <= m_low_limit_id);

    if (m_low_limit_no == m_low_limit_id &&
        m_low_limit_id == m_up_limit_id + m_ids.size())
    {
      m_ids.clear();
      m_low_limit_id= m_low_limit_no= m_up_limit_id;
    }
  }
}


/**
  Opens a read view where exactly the transactions serialized before this
  point in time are seen in the view.

  View becomes visible to purge thread.

  @param[in,out] trx transaction

  Reuses closed view if there were no read-write transactions since (and at)
  its creation time.

  Original comment states: there is an inherent race here between purge
  and this thread.

  To avoid this race we should've checked trx_sys.get_max_trx_id() and
  set m_open atomically under ReadView::m_mutex protection. But we're cutting
  edges to achieve greater performance.

  There're at least two types of concurrent threads interested in this
  value: purge coordinator thread (see trx_sys_t::clone_oldest_view()) and
  InnoDB monitor thread (see lock_trx_print_wait_and_mvcc_state()).

  What bad things can happen because we allow this race?

  Speculative execution may reorder state change before get_max_trx_id().
  In this case purge thread has short gap to clone outdated view. Which is
  probably not that bad: it just won't be able to purge things that it was
  actually allowed to purge for a short while.

  This thread may as well get suspended after trx_sys.get_max_trx_id() and
  before m_open is set. New read-write transaction may get started, committed
  and purged meanwhile. It is acceptable as well, since this view doesn't see
  it.
*/
void ReadView::open(trx_t *trx)
{
  ut_ad(this == &trx->read_view);
  if (is_open())
    ut_ad(!srv_read_only_mode);
  else if (likely(!srv_read_only_mode))
  {
    m_creator_trx_id= trx->id;
    if (trx->is_autocommit_non_locking() && empty() &&
        (low_limit_id() == trx_sys.get_max_trx_id()) &&
#ifdef WITH_INNODB_SCN
        (innodb_use_scn ? m_version == trx_sys.get_max_trx_scn() : true)
#else
        true
#endif
    )
      m_open.store(true, std::memory_order_relaxed);
    else
    {
      m_mutex.wr_lock();
      snapshot(trx);
      m_open.store(true, std::memory_order_relaxed);
      m_mutex.wr_unlock();
    }
  }
#ifdef WITH_INNODB_SCN
  set_trx(trx);
#endif
}


/**
  Clones the oldest view and stores it in view.

  No need to call ReadView::close(). The caller owns the view that is passed
  in. This function is called by purge thread to determine whether it should
  purge the delete marked record or not.
*/
void trx_sys_t::clone_oldest_view(ReadViewBase *view) const
{
  view->snapshot(nullptr);
  /* Find oldest view. */
  trx_list.for_each([view](const trx_t &trx) {
                      trx.read_view.append_to(view);
		    });
#ifdef WITH_INNODB_SCN
  if (innodb_use_scn)
  {
    if (view->m_low_limit_no > view->m_version)
    {
      view->m_low_limit_no= view->m_version;
    }
    else
    {
      view->m_version= view->m_low_limit_no;
    }
  }
#endif
}

#ifdef WITH_INNODB_SCN
SCN_Mgr scn_mgr;

bool ReadViewBase::changes_visible(const dict_index_t *index,
                                   buf_block_t *block, const rec_t *rec,
                                   const rec_offs *offsets,
                                   const trx_id_t creator_trx_id) const
{
  ut_a(index->is_clust());
  ut_ad(innodb_use_scn);

  if (index->table->is_temporary())
  {
    return true;
  }

  /* Get transaction id from record */
  ulint offset= scn_mgr.scn_offset(index, offsets);
  trx_id_t id= mach_read_from_6(rec + offset);

  /* If it's scn, direct compare*/
  if (SCN_Mgr::is_scn(id))
  {
    return sees_version(id);
  }

  /* Trx itself */
  if (id == creator_trx_id)
  {
    return true;
  }

  if (id < m_up_limit_id)
  {
    return true;
  }

  if (id >= m_low_limit_id)
  {
    return false;
  }

  if (m_committing_ids.find(id) != m_committing_ids.end())
  {
    /* Not visible to current view */
    return false;
  }

  /* Get SCN from undo log */
  trx_id_t committing_version= 0;
  trx_id_t scn=
      scn_mgr.get_scn(id, index, row_get_rec_roll_ptr(rec, index, offsets),
                      &committing_version);

  if (committing_version != 0 && committing_version < m_version)
  {
    /* Consider such scenario:
    - active trx: get trx->no = 5
    - open read view: version = 7
    - before committing trx completely: not visible
    - after committing trx: visible because it's deregistered
      and scn is written to undo (5 < 7)

    Problem: consistent read is broken, so we must
    record such kind of scn and id */
    ut_a(scn == TRX_ID_MAX);
    m_committing_ids.insert(id);
    m_committing_scns.insert(committing_version);

    ut_a(committing_version >= m_low_limit_no);
  }

  if (scn == TRX_ID_MAX)
  {
    /* Still active */
    return false;
  }

  ut_a(scn > 0);

  if (!srv_read_only_mode && block != nullptr &&
      !index->table->is_temporary() && !index->online_log)
  {
    /* Attch record to block */
    scn_mgr.add_lazy_cursor(block, const_cast<rec_t *>(rec), offset, id, scn,
                            index->table->id);
  }

  return (sees_version(scn));
}

void run_cleanout_task(void *arg);

SCN_Mgr::CleanoutWorker::CleanoutWorker(uint32_t id)
    : m_id(id), m_pages(CLEANOUT_ARRAY_MAX_SIZE),
      m_task(run_cleanout_task, &m_id)
{
  m_thd= innobase_create_background_thd("SCN cleanout worker");
}

SCN_Mgr::CleanoutWorker::~CleanoutWorker()
{
  destroy_background_thd(m_thd);
}

void SCN_Mgr::CleanoutWorker::add_page(uint64_t compact_page_id,
                                       table_id_t table_id)
{
  m_pages.add(compact_page_id, table_id);
}

void SCN_Mgr::CleanoutWorker::take_pages(PageSets &pages)
{
  pages.clear();

  uint64_t page_id= 0;
  table_id_t table_id= 0;
  while ((m_pages.get(page_id, table_id)))
  {
    pages.insert({page_id, table_id});
  }
}

void SCN_Mgr::init_for_background_task()
{
  if (m_cleanout_workers)
  {
    return;
  }
  m_cleanout_workers= new CleanoutWorker *[innodb_cleanout_threads];

  for (uint32_t i= 0; i < innodb_cleanout_threads; i++)
  {
    m_cleanout_workers[i]= new CleanoutWorker(i);
  }
}

trx_id_t SCN_Mgr::get_scn_fast(trx_id_t id, trx_id_t *version)
{
  if (id < m_startup_id)
  {
    /* Too old transaction */
    return m_startup_scn;
  }
  else
  {
    trx_id_t scn= m_scn_map.read(id);
    if (scn == 0)
    {
      scn= m_random_map.read(id);
    }

    if (scn != 0)
    {
      return scn;
    }

    trx_t *trx= trx_sys.find(nullptr, id, version != nullptr);

    if (trx == nullptr)
    {
      /* Already committed, need to find out scn from undo log */
      return 0;
    }

    if (version == nullptr)
    {
      /* Not calling from changes_visible, but from
      trx_undo_prev_version_build, and if trx is still in
      lf_hash, we treat it as invisible. */
      return TRX_ID_MAX;
    }

    trx->scn_mutex.wr_lock();
    trx_id_t target_scn= trx->scn;
    trx->scn_mutex.wr_unlock();

    trx->release_reference();

    if (target_scn != TRX_ID_MAX)
    {
      /* This scn is not visible even it's larger than
      current versio of read view. */
      *version= target_scn;
    }

    return TRX_ID_MAX;
  }
}

trx_id_t SCN_Mgr::get_scn(trx_id_t id, const dict_index_t *index,
                          roll_ptr_t roll_ptr, trx_id_t *version)
{
  ut_a(innodb_use_scn);
  trx_id_t scn= get_scn_fast(id, version);

  if (scn == TRX_ID_MAX)
  {
    /* Transaction is still active */
    return TRX_ID_MAX;
  }

  if (scn != 0)
  {
    return scn;
  }

  /* Slow path */
  scn= trx_undo_get_scn(index, roll_ptr, id);
  if (scn > 0)
  {
    m_random_map.store(id, scn);
  }

  ut_a(scn < trx_sys.get_max_trx_scn());

  if (scn == 0)
  {
    return TRX_ID_MAX;
  }

  return scn;
}

ulint SCN_Mgr::scn_offset(const dict_index_t *index, const rec_offs *offsets)
{
  ulint offset= index->trx_id_offset;

  if (!offset)
  {
    offset= row_get_trx_id_offset(index, offsets);
  }

  return offset;
}

void SCN_Mgr::set_scn(mtr_t *mtr, buf_block_t *block, rec_t *rec,
                      ulint trx_id_offset, trx_id_t id, trx_id_t scn)
{
  ut_ad(innodb_use_scn);
  byte *trx_id_ptr= rec + trx_id_offset;
  trx_id_t stored_id= mach_read_from_6(trx_id_ptr);

  if (stored_id == 0)
  {
    /* history purged by purge thread, visible to all transactions */
    return;
  }

  if (is_scn(stored_id))
  {
    if (stored_id >= scn)
    {
      /* Never revert back to smaller scn */
      return;
    }
  }
  else if (stored_id != id)
  {
    /* don't match */
    return;
  }

  mach_write_to_6(trx_id_ptr, scn);

  if (!block->page.zip.data)
  {
    mtr->memcpy(*block, trx_id_ptr - block->page.frame, DATA_TRX_ID_LEN);
  }
  else
  {
    page_zip_write_scn(block, rec, trx_id_offset, mtr);
  }
}

void SCN_Mgr::set_scn(trx_id_t current_id, mtr_t *mtr, buf_block_t *block,
                      rec_t *rec, dict_index_t *index, const rec_offs *offsets)
{
  ut_a(innodb_use_scn);
  if (index->table->is_temporary())
  {
    /* No need to set scn for temp table */
    return;
  }

  ulint offset= scn_offset(index, offsets);

  /* Read id */
  trx_id_t id= mach_read_from_6(rec + offset);
  if (id == 0)
  {
    /* history has been purged */
    return;
  }
  if (is_scn(id) || id == current_id)
  {
    /* Already be filled with scn, do nothing */
    return;
  }

  trx_id_t scn= get_scn_fast(id, nullptr);

  if (scn == 0 || scn == TRX_ID_MAX)
  {
    return;
  }

  set_scn(mtr, block, rec, offset, id, scn);
}

void SCN_Mgr::add_lazy_cursor(buf_block_t *block, rec_t *rec,
                              ulint trx_id_offset, trx_id_t id, trx_id_t scn,
                              table_id_t table_id)
{
  ut_ad(innodb_use_scn);
  if (fsp_is_system_temporary(block->get_space_id()))
  {
    return;
  }

  if (block->add_lazy_cursor(rec, trx_id_offset, id, scn) == false)
  {
    return;
  }
  ut_a(rec > block->page.frame);

  /* Add page number to the set */
  page_id_t page_id{block->get_space_id(), block->get_page_no()};
  uint64_t compact_id= page_id.raw();
  uint64_t slot= compact_id % innodb_cleanout_threads;

  m_cleanout_workers[slot]->add_page(compact_id, table_id);
}

void SCN_Mgr::view_task()
{
  ut_a(innodb_use_scn);
  trx_id_t id= 0;
  trx_id_t no= 0;
  trx_sys.get_min_trx_id_no(id, no);
  m_safe_limit_no= no;
  m_min_active_id= id;
}

void SCN_Mgr::batch_write(buf_block_t *block, mtr_t *mtr)
{
  ut_ad(innodb_use_scn);
  LazyCleanoutRecs lrecs;
  lrecs.clear();
  block->copy_and_free(lrecs);

  for (auto &lrec : lrecs)
  {
    rec_t *rec= lrec.first;
    ulint trx_id_offset= std::get<0>(lrec.second);
    trx_id_t trx_id= std::get<1>(lrec.second);
    trx_id_t scn= std::get<2>(lrec.second);
    set_scn(mtr, block, rec, trx_id_offset, trx_id, scn);
  }
}

void SCN_Mgr::cleanout_task(uint32_t slot)
{
  ut_a(innodb_use_scn);
  CleanoutWorker *worker= m_cleanout_workers[slot];
  MDL_ticket *mdl_ticket;
  THD *thd= worker->get_thd();
  while (!m_abort.load(std::memory_order_acquire))
  {
    PageSets pages;
    pages.clear();
    worker->take_pages(pages);
    if (pages.empty())
    {
      break;
    }
    /* Process pages that need to be modified */
    for (auto &val : pages)
    {
      if (m_abort.load(std::memory_order_acquire))
      {
        break;
      }
      page_id_t page_id(val.first);
      table_id_t table_id= val.second;
      mdl_ticket= nullptr;
      /* Prevents the tablespace from being dropped during the operation */
      dict_table_t *table= dict_table_open_on_id(
          table_id, false, DICT_TABLE_OP_NORMAL, thd, &mdl_ticket);
      if (table == nullptr)
      {
        continue;
      }
      fil_space_t *space= fil_space_get(page_id.space());
      mtr_t mtr;
      buf_block_t *block= nullptr;
      mtr_start(&mtr);
      mtr.set_in_scn_cleanout();
      block= buf_page_get_gen(page_id, 0, RW_X_LATCH, nullptr,
                              BUF_GET_IF_IN_POOL, &mtr, nullptr);
      if (block != nullptr)
      {
        if (!table->is_active_ddl())
        {
          mtr.set_named_space(space);
          batch_write(block, &mtr);
        }
        else
        {
          block->clear_cursor();
        }
      }
      mtr_commit(&mtr);
      dict_table_close(table, false, thd, mdl_ticket);
    }
  }
}

void run_cleanout_task(void *arg)
{
  uint32_t slot= *(uint32_t *) arg;
  scn_mgr.cleanout_task(slot);
}

void SCN_Mgr::cleanout_task_monitor()
{
  for (uint32_t i= 0; i < innodb_cleanout_threads; i++)
  {
    auto cleanout_work= m_cleanout_workers[i];
    if (!cleanout_work->is_empty() && !cleanout_work->is_running())
    {
      srv_thread_pool->submit_task(cleanout_work->get_task());
    }
  }
}

void cleanout_task_monitor(void *) { scn_mgr.cleanout_task_monitor(); }

void run_view_task(void *) { scn_mgr.view_task(); }

void SCN_Mgr::start()
{
  if (!innodb_use_scn || !m_cleanout_workers)
  {
    return;
  }
  m_abort= false;

  m_cleanout_task_timer.reset(
      srv_thread_pool->create_timer(::cleanout_task_monitor, nullptr));
  m_cleanout_task_timer->set_time(0, 1000);

  m_view_task_timer.reset(
      srv_thread_pool->create_timer(run_view_task, nullptr));
  m_view_task_timer->set_time(0, 1000);
}

void SCN_Mgr::stop()
{
  if (!innodb_use_scn || !m_cleanout_workers)
  {
    return;
  }
  m_abort= true;
  if (m_cleanout_task_timer.get())
  {
    m_cleanout_task_timer->disarm();
    m_cleanout_task_timer.reset();
  }
  if (m_view_task_timer.get())
  {
    m_view_task_timer->disarm();
    m_view_task_timer.reset();
  }
  for (uint32_t i= 0; i < innodb_cleanout_threads; i++)
  {
    auto task= m_cleanout_workers[i]->get_task();
    task->wait();
    delete m_cleanout_workers[i];
  }
  delete[] m_cleanout_workers;
  m_cleanout_workers= nullptr;
}
#endif /* WITH_INNODB_SCN */
