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
  trx_sys.snapshot_ids(trx, &m_ids, &m_low_limit_id, &m_low_limit_no);
  std::sort(m_ids.begin(), m_ids.end());
  m_up_limit_id= m_ids.empty() ? m_low_limit_id : m_ids.front();
  ut_ad(m_up_limit_id <= m_low_limit_id);
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
        low_limit_id() == trx_sys.get_max_trx_id())
      m_open.store(true, std::memory_order_relaxed);
    else
    {
      m_mutex.wr_lock();
      snapshot(trx);
      m_open.store(true, std::memory_order_relaxed);
      m_mutex.wr_unlock();
    }
  }
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
}
