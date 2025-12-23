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
#include "mem0mem.h"
#include "que0que.h"
#include "lock0lock.h"
#include "rem0rec.h"
#include "btr0pcur.h"
#include "btr0cur.h"

QueryExecutor::QueryExecutor(trx_t *trx) : m_mtr(trx)
{
  m_heap= mem_heap_create(256);
  m_thr= pars_complete_graph_for_exec(nullptr, trx, m_heap, nullptr);
  btr_pcur_init(&m_pcur);
}

QueryExecutor::~QueryExecutor()
{
  btr_pcur_close(&m_pcur);
  if (m_heap) mem_heap_free(m_heap);
}

dberr_t QueryExecutor::insert_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  dict_index_t* index= dict_table_get_first_index(table);
  return row_ins_clust_index_entry(index, tuple, m_thr, 0);
}

dberr_t QueryExecutor::lock_table(dict_table_t *table, lock_mode mode) noexcept
{
  trx_start_if_not_started(m_mtr.trx, true);
  return ::lock_table(table, nullptr, mode, m_thr);
}

dberr_t QueryExecutor::handle_wait(dberr_t err, bool table_lock) noexcept
{
  m_mtr.trx->error_state= err;
  if (table_lock) m_thr->lock_state= QUE_THR_LOCK_TABLE;
  else m_thr->lock_state= QUE_THR_LOCK_ROW;
  if (m_mtr.trx->lock.wait_thr)
  {
    dberr_t wait_err= lock_wait(m_thr);
    if (wait_err == DB_LOCK_WAIT_TIMEOUT) err= wait_err;
    if (wait_err == DB_SUCCESS)
    {
      m_thr->lock_state= QUE_THR_LOCK_NOLOCK;
      return DB_SUCCESS;
    }
  }
  return err;
}

dberr_t QueryExecutor::delete_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;
  ulint deleted_count= 0;

retry:
  m_mtr.start();
  m_mtr.set_named_space(table->space);

  m_pcur.btr_cur.page_cur.index= index;
  err= btr_pcur_open_on_user_rec(tuple, BTR_MODIFY_LEAF, &m_pcur, &m_mtr);
  if (err != DB_SUCCESS)
    goto func_exit;
  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    rec_t *rec= btr_pcur_get_rec(&m_pcur);
    rec_offs *offsets= nullptr;
    uint16_t matched_fields= 0;
    int cmp= 0;
    if (rec_get_deleted_flag(rec, dict_table_is_comp(table)))
      goto next_rec;

    offsets= rec_get_offsets(rec, index, nullptr,
                             index->n_core_fields,
                             ULINT_UNDEFINED, &m_heap);

    cmp= cmp_dtuple_rec_with_match(tuple, rec, index,
                                   offsets, &matched_fields);
    if (cmp != 0)
      break;
    err= lock_clust_rec_read_check_and_lock(
           0, btr_pcur_get_block(&m_pcur), rec, index, offsets, LOCK_X,
           LOCK_REC_NOT_GAP, m_thr);
    if (err == DB_LOCK_WAIT)
    {
      m_mtr.commit();
      err= handle_wait(err, false);
      if (err != DB_SUCCESS)
        return err;
      goto retry;
    }
    else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
      goto func_exit;

    err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&m_pcur),
                                        rec, index, offsets, m_thr,
                                        nullptr, &m_mtr);
    if (err != DB_SUCCESS)
      goto func_exit;
    deleted_count++;
next_rec:
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr))
      break;
  }
func_exit:
  m_mtr.commit();
  if (err)
    return err;
  return (deleted_count > 0) ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

dberr_t QueryExecutor::delete_all(dict_table_t *table) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;
retry:
  m_mtr.start();
  m_mtr.set_named_space(table->space);

  err= m_pcur.open_leaf(true, index, BTR_MODIFY_LEAF, &m_mtr);
  if (err != DB_SUCCESS || !btr_pcur_move_to_next(&m_pcur, &m_mtr))
  {
    m_mtr.commit();
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

    offsets= rec_get_offsets(rec, index, nullptr,
                             index->n_core_fields,
                             ULINT_UNDEFINED, &m_heap);
    err= lock_clust_rec_read_check_and_lock(
      0, btr_pcur_get_block(&m_pcur), rec, index, offsets, LOCK_X,
      LOCK_REC_NOT_GAP, m_thr);

    if (err == DB_LOCK_WAIT)
    {
      m_mtr.commit();
      err= handle_wait(err, false);
      if (err != DB_SUCCESS)
        return err;
      goto retry;
    }
    else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
    {
      m_mtr.commit();
      return err;
    }

    err= btr_cur_del_mark_set_clust_rec(btr_pcur_get_block(&m_pcur),
                                        const_cast<rec_t*>(rec), index,
                                        offsets, m_thr, nullptr, &m_mtr);
    if (err)
      break;
next_rec:
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr))
      break;
  }

  m_mtr.commit();
  return err;
}

dberr_t QueryExecutor::select_for_update(dict_table_t *table,
                                         dtuple_t *search_tuple,
                                         RecordCallback *callback) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;
  rec_t *rec;
  rec_offs *offsets;
  uint16_t matched_fields= 0;
  int cmp= 0;
  trx_t *trx= m_mtr.trx;
  m_mtr.start();
  m_mtr.set_named_space(table->space);

  if (trx && !trx->read_view.is_open())
  {
    trx_start_if_not_started(trx, false);
    trx->read_view.open(trx);
  }
  m_pcur.btr_cur.page_cur.index= index;
  err= btr_pcur_open_on_user_rec(search_tuple, BTR_MODIFY_LEAF,
                                 &m_pcur, &m_mtr);
  if (err != DB_SUCCESS)
    goto err_exit;

  if (!btr_pcur_is_on_user_rec(&m_pcur))
  {
    err= DB_RECORD_NOT_FOUND;
    goto err_exit;
  }
  rec= btr_pcur_get_rec(&m_pcur);
  offsets= rec_get_offsets(rec, index, nullptr,
                           index->n_core_fields,
                           ULINT_UNDEFINED, &m_heap);

  if (trx && trx->read_view.is_open())
  {
    trx_id_t rec_trx_id= row_get_rec_trx_id(rec, index, offsets);
    if (rec_trx_id && !trx->read_view.changes_visible(rec_trx_id))
    {
      err= DB_RECORD_NOT_FOUND;
      goto err_exit;
    }
  }
  cmp= cmp_dtuple_rec_with_match(search_tuple, rec, index,
                                 offsets, &matched_fields);
  if (cmp != 0)
  {
    err= DB_RECORD_NOT_FOUND;
    goto err_exit;
  }

  err= lock_clust_rec_read_check_and_lock(
    0, btr_pcur_get_block(&m_pcur), rec, index, offsets, LOCK_X,
    LOCK_REC_NOT_GAP, m_thr);

  if (err == DB_LOCK_WAIT)
  {
    m_mtr.commit();
    err= handle_wait(err, false);
    if (err != DB_SUCCESS)
      return err;
    return DB_LOCK_WAIT;
  }
  else if (err != DB_SUCCESS && err != DB_SUCCESS_LOCKED_REC)
  {
err_exit:
    m_mtr.commit();
    return err;
  }

  if (callback)
  {
    RecordCompareAction action=
      callback->compare_record(search_tuple, rec, index, offsets);
    if (action == RecordCompareAction::PROCESS)
      callback->process_record(rec, index, offsets);
    if (action == RecordCompareAction::SKIP)
    {
       err= DB_RECORD_NOT_FOUND;
       goto err_exit;
    }
  }
  return DB_SUCCESS;
}

dberr_t QueryExecutor::update_record(dict_table_t *table,
                                     const upd_t *update) noexcept
{
  dict_index_t *index= dict_table_get_first_index(table);
  rec_t *rec= btr_pcur_get_rec(&m_pcur);
  mtr_x_lock_index(index, &m_mtr);
  rec_offs *offsets= rec_get_offsets(rec, index, nullptr,
                                     index->n_core_fields,
                                     ULINT_UNDEFINED, &m_heap);

  dberr_t err= DB_SUCCESS;
  ulint cmpl_info= UPD_NODE_NO_ORD_CHANGE | UPD_NODE_NO_SIZE_CHANGE;
  for (ulint i = 0; i < update->n_fields; i++)
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
                                 m_thr, m_mtr.trx->id, &m_mtr);
  if (err == DB_OVERFLOW)
  {
    big_rec_t *big_rec= nullptr;
    err= btr_cur_optimistic_update(BTR_NO_LOCKING_FLAG,
                                   btr_pcur_get_btr_cur(&m_pcur),
                                   &offsets, &m_heap,
                                   const_cast<upd_t*>(update),
                                   cmpl_info, m_thr, m_mtr.trx->id, &m_mtr);

    if (err == DB_OVERFLOW || err == DB_UNDERFLOW)
    {
      mem_heap_t* offsets_heap= nullptr;
      err= btr_cur_pessimistic_update(BTR_NO_LOCKING_FLAG,
                                      btr_pcur_get_btr_cur(&m_pcur),
                                      &offsets, &offsets_heap, m_heap,
                                      &big_rec, const_cast<upd_t*>(update),
                                      cmpl_info, m_thr, m_mtr.trx->id, &m_mtr);

      if (err == DB_SUCCESS && big_rec)
      {
        err= btr_store_big_rec_extern_fields(&m_pcur, offsets, big_rec, &m_mtr,
                                             BTR_STORE_UPDATE);
        dtuple_big_rec_free(big_rec);
      }
      if (offsets_heap) mem_heap_free(offsets_heap);
    }
  }
  return err;
}

dberr_t QueryExecutor::replace_record(
   dict_table_t *table, dtuple_t *search_tuple,
   const upd_t *update, dtuple_t *insert_tuple) noexcept
{
retry_again:
  dberr_t err= select_for_update(table, search_tuple);
  if (err == DB_SUCCESS)
  {
    err= update_record(table, update);
    m_mtr.commit();
    return err;
  }
  else if (err == DB_RECORD_NOT_FOUND)
  {
    err= insert_record(table, insert_tuple);
    return err;
  }
  else if (err == DB_LOCK_WAIT)
    goto retry_again;
  return err;
}

dberr_t QueryExecutor::read(dict_table_t *table, const dtuple_t *tuple,
                            page_cur_mode_t mode,
                            RecordCallback& callback) noexcept
{
  ut_ad(table);
  dict_index_t *index= dict_table_get_first_index(table);

  m_mtr.start();
  if (m_mtr.trx && !m_mtr.trx->read_view.is_open())
  {
    trx_start_if_not_started(m_mtr.trx, false);
    m_mtr.trx->read_view.open(m_mtr.trx);
  }
  m_pcur.btr_cur.page_cur.index= index;
  dberr_t err= DB_SUCCESS;
  if (tuple)
  {
    err= btr_pcur_open_on_user_rec(tuple, BTR_SEARCH_LEAF, &m_pcur, &m_mtr);
    if (err != DB_SUCCESS)
    {
      m_mtr.commit();
      return err;
    }
  }
  else
  {
    err= m_pcur.open_leaf(true, index, BTR_SEARCH_LEAF, &m_mtr);
    if (err != DB_SUCCESS || !btr_pcur_move_to_next(&m_pcur, &m_mtr))
    {
      m_mtr.commit();
      return err;
    }
  }
  ulint match_count= 0;
  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    const rec_t *rec= btr_pcur_get_rec(&m_pcur);
    rec_offs* offsets= rec_get_offsets(rec, index, nullptr,
                                       index->n_core_fields,
                                       ULINT_UNDEFINED, &m_heap);
    RecordCompareAction action= callback.compare_record(
      tuple, rec, index, offsets);
    if (action == RecordCompareAction::PROCESS)
    {
      bool continue_processing= true;
      dberr_t err= process_record_with_mvcc(table, index, rec,
                                            offsets, callback,
					    continue_processing);
      if (err || !continue_processing)
      {
        m_mtr.commit();
	return err;
      }
      match_count++;
    }
    else if (action == RecordCompareAction::STOP)
      break;
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr))
      break;
  }
  m_mtr.commit();
  return (match_count > 0 || !tuple) ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

dberr_t QueryExecutor::read_by_index(dict_table_t *table,
                                     dict_index_t *sec_index,
                                     const dtuple_t *search_tuple,
                                     page_cur_mode_t mode,
                                     RecordCallback& callback) noexcept
{
  ut_ad(table);
  ut_ad(sec_index);
  ut_ad(sec_index->table == table);
  ut_ad(!dict_index_is_clust(sec_index));

  dict_index_t *clust_index= dict_table_get_first_index(table);

  m_mtr.start();
  if (m_mtr.trx && !m_mtr.trx->read_view.is_open())
  {
    trx_start_if_not_started(m_mtr.trx, false);
    m_mtr.trx->read_view.open(m_mtr.trx);
  }
  m_pcur.btr_cur.page_cur.index= sec_index;

  dberr_t err= btr_pcur_open_on_user_rec(
    search_tuple, BTR_SEARCH_LEAF, &m_pcur, &m_mtr);

  if (err != DB_SUCCESS)
  {
    m_mtr.commit();
    return err;
  }

  ulint match_count= 0;
  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    const rec_t *sec_rec= btr_pcur_get_rec(&m_pcur);
    rec_offs* sec_offsets= rec_get_offsets(sec_rec, sec_index, nullptr,
                                           sec_index->n_core_fields,
                                           ULINT_UNDEFINED, &m_heap);
    /* Check if secondary record matches our search criteria */
    RecordCompareAction action= callback.compare_record(search_tuple, sec_rec,
                                                        sec_index, sec_offsets);
    if (action == RecordCompareAction::PROCESS)
    {
      /* Lookup clustered record and process it */
      bool continue_processing= true;
      dberr_t err= lookup_clustered_record(
        table, sec_index, clust_index, sec_rec, callback, match_count,
	continue_processing);
      if (err || !continue_processing)
      {
        m_mtr.commit();
	return err;
      }
    }
    else if (action == RecordCompareAction::STOP)
      break;
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr)) break;
  }
  m_mtr.commit();
  return match_count > 0 ? DB_SUCCESS : DB_RECORD_NOT_FOUND;
}

dberr_t QueryExecutor::lookup_clustered_record(dict_table_t *table,
                                               dict_index_t *sec_index,
                                               dict_index_t *clust_index,
                                               const rec_t *sec_rec,
                                               RecordCallback& callback,
                                               ulint& match_count,
                                               bool& continue_processing) noexcept
{
  /* Extract primary key from secondary index record */
  dtuple_t *clust_tuple= row_build_row_ref(ROW_COPY_DATA, sec_index,
                                           sec_rec, m_heap);
  /* Now lookup the complete row using clustered index */
  btr_pcur_t clust_pcur;
  clust_pcur.btr_cur.page_cur.index= clust_index;

  ulint savepoint= m_mtr.get_savepoint();
  dberr_t clust_err= btr_pcur_open(clust_tuple, PAGE_CUR_LE,
                                   BTR_SEARCH_LEAF, &clust_pcur,
                                   &m_mtr);
  if (clust_err == DB_SUCCESS)
  {
    const rec_t *clust_rec= btr_pcur_get_rec(&clust_pcur);
    rec_offs* clust_offsets= rec_get_offsets(clust_rec, clust_index,
                                             nullptr,
                                             clust_index->n_core_fields,
                                             ULINT_UNDEFINED, &m_heap);
    /* Verify this is the exact record we want */
    if (!cmp_dtuple_rec(clust_tuple, clust_rec, clust_index, clust_offsets))
    {
      dberr_t err= process_record_with_mvcc(
        table, clust_index, clust_rec, clust_offsets, callback,
        continue_processing);
      if (err != DB_SUCCESS)
      {
        m_mtr.rollback_to_savepoint(savepoint, savepoint + 1);
        return err;
      }
      match_count++;
    }
  }
  m_mtr.rollback_to_savepoint(savepoint, savepoint + 1);
  return clust_err;
}

dberr_t QueryExecutor::process_record_with_mvcc(
   dict_table_t *table, dict_index_t *index, const rec_t *rec,
   rec_offs *offsets, RecordCallback &callback,
   bool &continue_processing) noexcept
{
  bool is_deleted= rec_get_deleted_flag(rec, dict_table_is_comp(table));
  rec_t* version_rec= const_cast<rec_t*>(rec);
  rec_offs* version_offsets= offsets;
  mem_heap_t* version_heap= nullptr;
  bool should_process_record= false;
  dberr_t error= DB_SUCCESS;

  if (m_mtr.trx && m_mtr.trx->read_view.is_open())
  {
    trx_id_t rec_trx_id= row_get_rec_trx_id(rec, index, offsets);
    if (rec_trx_id && !m_mtr.trx->read_view.changes_visible(rec_trx_id))
    {
      version_heap= mem_heap_create(1024);
      error= row_vers_build_for_consistent_read(
        rec, &m_mtr, index, &offsets, &m_mtr.trx->read_view, &version_heap,
        version_heap, &version_rec, nullptr);
      if (error == DB_SUCCESS && version_rec)
      {
        version_offsets= rec_get_offsets(version_rec, index, nullptr,
                                         index->n_core_fields,
                                         ULINT_UNDEFINED, &version_heap);
        is_deleted= rec_get_deleted_flag(version_rec, dict_table_is_comp(table));
        should_process_record= !is_deleted;
      }
      else if (error != DB_SUCCESS)
      {
        if (version_heap) mem_heap_free(version_heap);
        continue_processing= false;
        return error;
      }
    }
    else
      should_process_record= !is_deleted;
  }
  else
    should_process_record= !is_deleted && version_rec;

  if (should_process_record)
    continue_processing= callback.process_record(
      version_rec, index, version_offsets);
  if (version_heap) mem_heap_free(version_heap);
  return error;
}
