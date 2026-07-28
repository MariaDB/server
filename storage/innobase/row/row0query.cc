/*****************************************************************************

Copyright (c) 2025, MariaDB PLC.

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
@file row/row0query.cc
General Query Executor

Created 2025/10/30
*******************************************************/

#include "row0query.h"
#include "pars0pars.h"
#include "dict0dict.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0row.h"
#include "row0vers.h"
#include "row0sel.h"
#include "mem0mem.h"
#include "que0que.h"
#include "lock0lock.h"
#include "rem0rec.h"
#include "btr0pcur.h"
#include "btr0cur.h"

QueryExecutor::QueryExecutor(trx_t *trx) : m_trx(trx), m_mtr(nullptr)
{
  ut_ad(trx);
  ut_ad(trx->state == TRX_STATE_NOT_STARTED
        || trx->state == TRX_STATE_ACTIVE);
  ut_ad(!trx->is_autocommit_non_locking());

  m_heap= mem_heap_create(256);
  m_thr= pars_complete_graph_for_exec(nullptr, trx, m_heap, nullptr);
  btr_pcur_init(&m_pcur);
  m_clust_pcur= nullptr;
  m_prebuilt= nullptr;
}

QueryExecutor::~QueryExecutor()
{
  btr_pcur_close(&m_pcur);
  if (m_clust_pcur)
    btr_pcur_close(m_clust_pcur);

  if (m_prebuilt)
  {
    if (m_prebuilt->old_vers_heap)
      mem_heap_free(m_prebuilt->old_vers_heap);

    ut_ad(!m_prebuilt->blob_heap);
    ut_ad(!m_prebuilt->fetch_cache[0]);
    ut_ad(!m_prebuilt->rtr_info);
    ut_ad(!m_prebuilt->mysql_template);

    m_prebuilt= nullptr;
  }

  if (m_heap) mem_heap_free(m_heap);
}

void QueryExecutor::setup_prebuilt(
       dict_table_t *table, dict_index_t *index,
       const dtuple_t *tuple, page_cur_mode_t mode) noexcept
{
  dict_index_t *clust_index= dict_table_get_first_index(table);
  ulint ref_len= dict_index_get_n_unique(clust_index);

  if (!m_prebuilt)
  {
    m_prebuilt= static_cast<row_prebuilt_t*>(
      mem_heap_zalloc(m_heap, sizeof(*m_prebuilt)));
 
    m_prebuilt->magic_n= ROW_PREBUILT_ALLOCATED;
    m_prebuilt->magic_n2= ROW_PREBUILT_ALLOCATED;

    m_prebuilt->table= table;
    m_prebuilt->heap= m_heap;
 
    m_prebuilt->sel_graph= static_cast<que_fork_t*>(
        que_node_get_parent(m_thr));
    m_prebuilt->sql_stat_start= TRUE;
    m_prebuilt->in_fts_query= true;
    ulint search_tuple_n_fields= 2 * (dict_table_get_n_cols(table)
                                      + dict_table_get_n_v_cols(table));
    m_prebuilt->search_tuple=
      dtuple_create(m_heap, search_tuple_n_fields);

    m_prebuilt->clust_ref= dtuple_create(m_heap, ref_len);
    dict_index_copy_types(m_prebuilt->clust_ref, clust_index, ref_len);
  }
  else if (m_prebuilt->table != table)
  {
    m_prebuilt->table= table;
    m_prebuilt->clust_ref= dtuple_create(m_heap, ref_len);
    dict_index_copy_types(m_prebuilt->clust_ref, clust_index, ref_len);
    btr_pcur_reset(&m_pcur);
    if (m_clust_pcur)
      btr_pcur_reset(m_clust_pcur);
    m_prebuilt->fts_doc_id= 0;
  }

  /* Configure prebuilt for search */
  m_prebuilt->trx= m_trx;
  m_prebuilt->index= index;
  if (!tuple)
    m_prebuilt->search_tuple= nullptr;
  else
    m_prebuilt->search_tuple= const_cast<dtuple_t*>(tuple);

  m_prebuilt->pcur= &m_pcur;
  if (!index->is_clust())
  {
    if (!m_clust_pcur)
    {
      m_clust_pcur= static_cast<btr_pcur_t*>(
        mem_heap_zalloc(m_prebuilt->heap, sizeof(btr_pcur_t)));
      btr_pcur_init(m_clust_pcur);
    }
  }
  m_prebuilt->clust_pcur= m_clust_pcur;
 
  m_prebuilt->mtr= nullptr;
  m_prebuilt->template_type= ROW_MYSQL_NO_TEMPLATE;
  m_prebuilt->select_lock_type= LOCK_NONE;
  m_prebuilt->row_read_type= 0;
  m_prebuilt->n_fetch_cached= 0;
  m_prebuilt->fetch_cache_first= 0;
  m_prebuilt->index_usable= 1;
  m_prebuilt->need_to_access_clustered= index->is_clust() ? 0 : 1;
  m_prebuilt->in_fts_query= true;
  /* Treat every setup_prebuilt() as a fresh statement: force
  row_search_mvcc to re-traverse the tree via search_tuple instead of
  trying to resume from the stored pcur position left over from a
  previous call. */
  m_prebuilt->sql_stat_start= TRUE;
}

/* SQL equivalent:
     INSERT INTO <table> VALUES (<tuple>);
Build a dtuple_t in caller-side memory and hand it straight to
row_ins_clust_index_entry(), skipping the parser/graph entirely.

row_ins_clust_index_entry() returns DB_LOCK_WAIT raw for tables with
no foreign-key path (FTS aux/common/CONFIG): the leaf queues the
waiter in trx->lock.wait_lock and returns without calling lock_wait.
We must drain via handle_wait() before retrying; spinning on the goto
would loop on the same unsatisfied wait. */
dberr_t QueryExecutor::insert_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  ut_ad(m_trx);
  ut_ad(!m_mtr || !m_mtr->is_active());
  dict_index_t* index= dict_table_get_first_index(table);
  dberr_t err;

retry:
  err = row_ins_clust_index_entry(index, tuple, m_thr, 0);
  if (err == DB_LOCK_WAIT)
  {
    err= handle_wait(err, false);
    if (err == DB_SUCCESS)
      goto retry;
  }
  ut_ad(err != DB_LOCK_WAIT);
  return err;
}

/* SQL equivalent:
     LOCK TABLES <table> { IS | IX | X };
Acquires an InnoDB table-level lock on `table`, blocking (with the
per-trx innodb_lock_wait_timeout) if a conflicter is present. */
dberr_t QueryExecutor::lock_table(dict_table_t *table, lock_mode mode) noexcept
{
  ut_ad(m_trx);
  m_trx->will_lock= true;
  trx_start_if_not_started(m_trx, mode == LOCK_IX ? true : false);
  ut_ad(m_trx->state == TRX_STATE_ACTIVE);
  ut_ad(m_trx->will_lock);
  ut_ad(!m_trx->is_autocommit_non_locking());
  m_thr->graph->trx= m_trx;

  dberr_t err;
retry:
  err = ::lock_table(table, nullptr, mode, m_thr);
  if (err == DB_LOCK_WAIT)
  {
    err= handle_wait(err, true);
    if (err == DB_SUCCESS)
      goto retry;
  }
  ut_ad(err != DB_LOCK_WAIT);
  return err;
}

/* Drain a DB_LOCK_WAIT from a leaf that does NOT self-drain
(::lock_table, lock_clust_rec_read_check_and_lock).

We do not gate on m_trx->lock.wait_thr: the lock-holder may grant
and clear wait_thr between the leaf's return and our read, which
would skip lock_wait() and propagate a stale DB_LOCK_WAIT.
lock_wait() itself handles a cleared wait_lock by normalising to
DB_SUCCESS, so the call is always safe.

NULL m_trx->mysql_thd is allowed: lock_wait() degrades to a
0-second timeout, which is the intended behaviour for internal
and background transactions. */
dberr_t QueryExecutor::handle_wait(dberr_t err, bool table_lock) noexcept
{
  ut_ad(m_trx);
  ut_ad(err == DB_LOCK_WAIT);
  m_trx->error_state= err;
  m_thr->lock_state= table_lock ? QUE_THR_LOCK_TABLE : QUE_THR_LOCK_ROW;
  dberr_t wait_err= lock_wait(m_thr);
  ut_ad(wait_err == DB_SUCCESS
        || wait_err == DB_LOCK_WAIT_TIMEOUT
        || wait_err == DB_DEADLOCK
        || wait_err == DB_INTERRUPTED);
  ut_ad(wait_err != DB_SUCCESS || !m_trx->lock.wait_lock);
  m_thr->lock_state= QUE_THR_LOCK_NOLOCK;
  return wait_err;
}

/* SQL equivalent:
     DELETE FROM <table> WHERE <pk> = <tuple>;
Two-phase: select_for_update() positions+X-locks the matching
clustered record (graph nodes: sel_node_t + lock acquisition);
btr_cur_del_mark_set_clust_rec() flips the deleted bit + undo
Commit m_mtr here so the caller observes the same lifecycle
as a single DELETE statement */
dberr_t QueryExecutor::delete_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  ut_ad(m_trx);
  ut_ad(!m_mtr || !m_mtr->is_active());

  dict_index_t *clust_index= dict_table_get_first_index(table);

  /* Use select_for_update to find and lock the record with proper MVCC handling */
  dberr_t err= select_for_update(table, tuple, nullptr);
  if (err != DB_SUCCESS)
    return err;

  /* Record is now locked at m_pcur, mark it as deleted */
  rec_t *rec= btr_pcur_get_rec(&m_pcur);
  rec_offs *offsets= rec_get_offsets(rec, clust_index, nullptr,
                                     clust_index->n_core_fields,
                                     ULINT_UNDEFINED, &m_heap);

  err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&m_pcur),
                                      rec, clust_index, offsets, m_thr,
                                      nullptr, m_mtr);

  m_mtr->commit();
  return err;
}

/* SQL equivalent:
     DELETE FROM <table>;
Walk the clustered index leaves directly via btr_pcur and X-lock each
visible record before delete-marking it.
Latch lifetime: BTR_MODIFY_LEAF X-latches the current leaf only;
btr_pcur_move_to_next releases each leaf when crossing pages, so
peak latch holding is one leaf, not the whole scan.
Lock lifetime: LOCK_X record locks accumulate until the caller
commits the trx.  This is intentional - "delete everything" must
be atomic w.r.t. concurrent readers and crash recovery, so we do
not commit/restart mid-scan. */
dberr_t QueryExecutor::delete_all(dict_table_t *table) noexcept
{
  ut_ad(m_trx);
  ut_ad(!m_mtr || !m_mtr->is_active());
  dict_index_t *clust_index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;

  if (!m_mtr)
    m_mtr= new (mem_heap_alloc(m_heap, sizeof(mtr_t))) mtr_t(m_trx);
retry:
  m_mtr->start();
  m_mtr->set_named_space(table->space);

  err= m_pcur.open_leaf(true, clust_index, BTR_MODIFY_LEAF, m_mtr);
  if (err != DB_SUCCESS || !btr_pcur_move_to_next(&m_pcur, m_mtr))
  {
    m_mtr->commit();
    return err;
  }

  while (!btr_pcur_is_after_last_on_page(&m_pcur) &&
         !btr_pcur_is_after_last_in_tree(&m_pcur))
  {
    rec_t* rec= btr_pcur_get_rec(&m_pcur);
    rec_offs *offsets= nullptr;
    if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
      goto next_rec;
    if (rec_get_info_bits(
          rec, dict_table_is_comp(table)) & REC_INFO_MIN_REC_FLAG)
      goto next_rec;

    offsets= rec_get_offsets(rec, clust_index, nullptr,
                             clust_index->n_core_fields,
                             ULINT_UNDEFINED, &m_heap);
    err= lock_clust_rec_read_check_and_lock(
      0, btr_pcur_get_block(&m_pcur), rec, clust_index, offsets, LOCK_X,
      LOCK_REC_NOT_GAP, m_thr);

    if (err == DB_LOCK_WAIT)
    {
      m_mtr->commit();
      err= handle_wait(err, false);
      ut_ad(err != DB_LOCK_WAIT);
      if (err != DB_SUCCESS)
        return err;
      goto retry;
    }
    else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
    {
      m_mtr->commit();
      return err;
    }

    err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&m_pcur),
                                        const_cast<rec_t*>(rec), clust_index,
                                        offsets, m_thr, nullptr, m_mtr);
    if (err)
      break;
next_rec:
    if (!btr_pcur_move_to_next(&m_pcur, m_mtr))
      break;
  }

  m_mtr->commit();
  return err;
}

/* SQL equivalent:
     SELECT * FROM <table> WHERE <pk> >= <search_tuple> FOR UPDATE;
Positions m_pcur on the first record at or after the search key,
X-locks it via row_search_mvcc<InnoDBDMLPolicy>(), and leaves the
mtr open so a caller can chain update_record() / delete-mark
before commit_mtr().

On any error (LOCK_WAIT_TIMEOUT / DEADLOCK / RECORD_NOT_FOUND /...)
m_mtr is committed before return, so error paths never leak
an open mtr. */
dberr_t QueryExecutor::select_for_update(dict_table_t *table,
                                         dtuple_t *search_tuple,
                                         RecordCallback *callback) noexcept
{
  ut_ad(m_trx);
  ut_ad(!m_mtr || !m_mtr->is_active());
  dict_index_t *clust_index= dict_table_get_first_index(table);
  setup_prebuilt(table, clust_index, search_tuple, PAGE_CUR_GE);
  m_prebuilt->select_lock_type= LOCK_X;

  if (!m_mtr)
    m_mtr= new (mem_heap_alloc(m_heap, sizeof(mtr_t))) mtr_t(m_trx);

  m_mtr->start();
  m_mtr->set_named_space(table->space);

  if (m_trx && !m_trx->read_view.is_open())
  {
    trx_start_if_not_started(m_trx, false);
    m_trx->read_view.open(m_trx);
  }
  ut_ad(m_trx->state == TRX_STATE_ACTIVE);
  /* Either we are taking row locks (LOCK_X/LOCK_S, latest version,
  no read view needed), or the consistent-read view is open, or we
  are in srv_read_only_mode (where ReadView::open() is intentionally
  a no-op). */
  ut_ad(m_prebuilt->select_lock_type != LOCK_NONE
        || m_trx->read_view.is_open()
        || srv_read_only_mode);

  /* Provide external mtr for DML operation */
  m_prebuilt->mtr= m_mtr;

  dberr_t err= DB_SUCCESS;

  if (callback)
    err= row_search_mvcc_callback_dml(
           callback, PAGE_CUR_GE, m_prebuilt, 0, 0);
  else
    err= row_search_mvcc<InnoDBDMLPolicy>(nullptr, PAGE_CUR_GE,
                                          m_prebuilt, 1, 0);
  ut_ad(err != DB_LOCK_WAIT);

  if (err == DB_END_OF_INDEX || err == DB_RECORD_NOT_FOUND)
    err= DB_RECORD_NOT_FOUND;
  else if (err == DB_SUCCESS_LOCKED_REC)
    err= DB_SUCCESS;

  if (err != DB_SUCCESS)
    m_mtr->commit();
  return err;
}

/* SQL equivalent:
     UPDATE <table> SET <update> WHERE CURRENT OF m_pcur;
Operates on the row currently positioned and X-locked by an
immediately preceding select_for_update(). Falls back from
in-place to optimistic to pessimistic + external storage as the
row size grows.  m_mtr is left open so the caller can issue
commit_mtr() at the right point in its own logic. */
dberr_t QueryExecutor::update_record(dict_table_t *table,
                                     const upd_t *update) noexcept
{
  ut_ad(m_trx);
  ut_ad(m_mtr);
  ut_ad(m_mtr && m_mtr->is_active());
  dict_index_t *clust_index= dict_table_get_first_index(table);
  rec_t *rec= btr_pcur_get_rec(&m_pcur);
  mtr_x_lock_index(clust_index, m_mtr);
  rec_offs *offsets= rec_get_offsets(rec, clust_index, nullptr,
                                     clust_index->n_core_fields,
                                     ULINT_UNDEFINED, &m_heap);

  dberr_t err= DB_SUCCESS;
  ulint cmpl_info= UPD_NODE_NO_ORD_CHANGE | UPD_NODE_NO_SIZE_CHANGE;
  for (ulint i= 0; i < update->n_fields; i++)
  {
    const upd_field_t *upd_field= &update->fields[i];
    ulint field_no= upd_field->field_no;
    if (field_no < rec_offs_n_fields(offsets))
    {
      ulint old_len= rec_offs_nth_size(offsets, field_no);
      ulint new_len= upd_field->new_val.len;
      if (new_len != UNIV_SQL_NULL && new_len != old_len)
      {
        cmpl_info &= ~UPD_NODE_NO_SIZE_CHANGE;
        err= DB_OVERFLOW;
        break;
      }
    }
  }

  if (cmpl_info & UPD_NODE_NO_SIZE_CHANGE)
    err= btr_cur_update_in_place(BTR_NO_LOCKING_FLAG,
                                 btr_pcur_get_btr_cur(&m_pcur),
                                 offsets, const_cast<upd_t*>(update), 0,
                                 m_thr, m_trx->id, m_mtr);
  if (err == DB_OVERFLOW)
  {
    big_rec_t *big_rec= nullptr;
    err= btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG,
                                   btr_pcur_get_btr_cur(&m_pcur),
                                   &offsets, &m_heap,
                                   const_cast<upd_t*>(update),
                                   cmpl_info, m_thr, m_trx->id, m_mtr);

    if (err == DB_OVERFLOW || err == DB_UNDERFLOW)
    {
      mem_heap_t* offsets_heap= nullptr;
      err= btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG,
                                      btr_pcur_get_btr_cur(&m_pcur),
                                      &offsets, &offsets_heap, m_heap,
                                      &big_rec, const_cast<upd_t*>(update),
                                      cmpl_info, m_thr, m_trx->id, m_mtr);

      if (err == DB_SUCCESS && big_rec)
      {
        err= btr_store_big_rec_extern_fields(&m_pcur, offsets, big_rec, m_mtr,
                                             BTR_STORE_UPDATE);
        dtuple_big_rec_free(big_rec);
      }
      if (offsets_heap) mem_heap_free(offsets_heap);
    }
  }
  return err;
}

/* SQL equivalent:
     INSERT INTO <table> VALUES (<insert_tuple>)
       ON DUPLICATE KEY UPDATE <update applied to <search_tuple>>;
Upsert: try select_for_update on the search key; if found, run
update_record; if not, run insert_record.  Both branches commit
their own mtr so the caller observes the same single-statement
lifecycle (m_mtr state NONE on entry and on exit). */
dberr_t QueryExecutor::replace_record(
   dict_table_t *table, dtuple_t *search_tuple,
   const upd_t *update, dtuple_t *insert_tuple) noexcept
{
  ut_ad(m_trx);
  ut_ad(!m_mtr || !m_mtr->is_active());
  dberr_t err= select_for_update(table, search_tuple);
  if (err == DB_RECORD_NOT_FOUND)
    return insert_record(table, insert_tuple);
  if (err == DB_SUCCESS)
  {
    err= update_record(table, update);
    m_mtr->commit();
  }
  return err;
}

/* SQL equivalent:
     SELECT <implicit clust columns> FROM <table> [USE INDEX (<index>)]
       WHERE <index columns> <mode> <search_tuple>
with the WHERE refined per-record by callback.compare_record,
and each visible row handed to callback.process_record.

If `index` is the clustered index, this is a direct scan; otherwise
row_search_mvcc walks the secondary index and resolves to the
matching clustered record before invoking the callback.

read_all=true keeps iterating to EOF / STOP / pagination signal;
read_all=false returns after the first matching row. */
dberr_t QueryExecutor::read_by_index(dict_table_t *table,
                                     dict_index_t *index,
                                     const dtuple_t *search_tuple,
                                     page_cur_mode_t mode,
                                     RecordCallback& callback,
			             bool read_all) noexcept
{
  ut_ad(m_trx);
  ut_ad(table);
  ut_ad(index);
  ut_ad(callback.compare_record);

  setup_prebuilt(table, index, search_tuple, mode);

  if (m_trx && !m_trx->read_view.is_open())
  {
    trx_start_if_not_started(m_trx, false);
    m_trx->read_view.open(m_trx);
  }
  ut_ad(m_trx->state == TRX_STATE_ACTIVE);
  ut_ad(m_prebuilt->select_lock_type != LOCK_NONE
        || m_trx->read_view.is_open()
        || srv_read_only_mode);
  dberr_t err= row_search_mvcc_callback(&callback, mode, m_prebuilt, 0, 0);
  ut_ad(err != DB_LOCK_WAIT);
  if (read_all)
  {
    while (err == DB_SUCCESS)
      err= row_search_mvcc_callback(&callback, mode, m_prebuilt, 0,
                                    ROW_SEL_NEXT);
    ut_ad(err != DB_LOCK_WAIT);
  }
  if (err == DB_END_OF_INDEX || err == DB_SUCCESS_LOCKED_REC)
    err= DB_SUCCESS;
  return err;
}

/* SQL equivalent:
     SELECT <implicit clust columns> FROM <table>
       WHERE <pk> <mode> <tuple>
       LIMIT 1;
Single-record convenience wrapper over read_by_index() pinned to
the clustered index, with read_all=false. */
dberr_t QueryExecutor::read(dict_table_t *table, const dtuple_t *tuple,
                            page_cur_mode_t mode,
                            RecordCallback& callback) noexcept
{
  ut_ad(m_trx);
  ut_ad(table);
  ut_ad(callback.compare_record);

  dict_index_t *clust_index= dict_table_get_first_index(table);
  return read_by_index(table, clust_index, tuple, mode, callback, false);
}
