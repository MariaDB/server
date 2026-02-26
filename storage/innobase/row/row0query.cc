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

/** Extract transaction ID from a clustered index record
@param[in] rec          record to extract transaction ID from
@param[in] clust_index  clustered index
@return transaction ID */
static trx_id_t get_record_trx_id(const rec_t* rec, const dict_index_t* clust_index)
{
  if (clust_index->trx_id_offset)
    return trx_read_trx_id(rec + clust_index->trx_id_offset);
  /* Calculate offset to DB_TRX_ID field by iterating through fields */
  ulint trx_id_field_no= clust_index->db_trx_id();
  ulint trx_id_offset= 0;

  if (dict_table_is_comp(clust_index->table))
  {
    const byte* lens=
      rec - REC_N_NEW_EXTRA_BYTES - 1 - clust_index->n_core_null_bytes;
    for (ulint i= 0; i < trx_id_field_no; i++)
    {
      const dict_field_t* field= dict_index_get_nth_field(clust_index, i);
      ulint field_len;

      if (field->fixed_len)
        field_len= field->fixed_len;
      else
      {
        ulint len_byte= *lens--;
        if (UNIV_UNLIKELY(len_byte & 0x80) && DATA_BIG_COL(field->col))
        {
          len_byte<<= 8;
          len_byte|= *lens--;
          field_len= (len_byte & 0x3fff);
        }
        else
          field_len= len_byte;
 
        if (field_len == UNIV_SQL_NULL)
          field_len= 0;
      }
      trx_id_offset+= field_len;
    }
    return trx_read_trx_id(rec + trx_id_offset);
  }
  else
  {
    for (ulint i= 0; i < trx_id_field_no; i++)
    {
      ulint field_len;
      ulint field_offset= rec_get_nth_field_offs_old(rec, i, &field_len);
      if (i == 0)
        trx_id_offset= field_offset;
      if (field_len != UNIV_SQL_NULL)
        trx_id_offset= field_offset + field_len;
      else
        trx_id_offset= field_offset;
    }
    return trx_read_trx_id(rec + trx_id_offset);
  }
}

QueryExecutor::QueryExecutor(trx_t *trx) : m_mtr(trx)
{
  m_heap= mem_heap_create(256);
  m_thr= pars_complete_graph_for_exec(nullptr, trx, m_heap, nullptr);
  btr_pcur_init(&m_pcur);
  m_clust_pcur= nullptr;
  m_version_heap= nullptr;
  m_offsets_heap= nullptr;
}

QueryExecutor::~QueryExecutor()
{
  btr_pcur_close(&m_pcur);
  if (m_clust_pcur)
  {
    btr_pcur_close(m_clust_pcur);
    delete m_clust_pcur;
    m_clust_pcur= nullptr;
  }
  if (m_heap) mem_heap_free(m_heap);
  if (m_version_heap) mem_heap_free(m_version_heap);
}

dberr_t QueryExecutor::insert_record(dict_table_t *table,
                                     dtuple_t *tuple) noexcept
{
  dict_index_t* index= dict_table_get_first_index(table);
  return row_ins_clust_index_entry(index, tuple, m_thr, 0);
}

dberr_t QueryExecutor::lock_table(dict_table_t *table, lock_mode mode) noexcept
{
  ut_ad(m_mtr.trx);
  trx_start_if_not_started(m_mtr.trx, true);
  return ::lock_table(table, nullptr, mode, m_thr);
}

dberr_t QueryExecutor::handle_wait(dberr_t err, bool table_lock) noexcept
{
  ut_ad(m_mtr.trx);
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
  dict_index_t *clust_index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;
  ulint deleted_count= 0;

retry:
  m_mtr.start();
  m_mtr.set_named_space(table->space);

  m_pcur.btr_cur.page_cur.index= clust_index;
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

    offsets= rec_get_offsets(rec, clust_index, offsets,
                             clust_index->n_core_fields,
                             ULINT_UNDEFINED, &m_heap);

    cmp= cmp_dtuple_rec_with_match(tuple, rec, clust_index,
                                   offsets, &matched_fields);
    if (cmp != 0)
      break;
    err= lock_clust_rec_read_check_and_lock(
           0, btr_pcur_get_block(&m_pcur), rec, clust_index, offsets, LOCK_X,
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
                                        rec, clust_index, offsets, m_thr,
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
  dict_index_t *clust_index= dict_table_get_first_index(table);
  dberr_t err= DB_SUCCESS;
retry:
  m_mtr.start();
  m_mtr.set_named_space(table->space);

  err= m_pcur.open_leaf(true, clust_index, BTR_MODIFY_LEAF, &m_mtr);
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

    offsets= rec_get_offsets(rec, clust_index, nullptr,
                             clust_index->n_core_fields,
                             ULINT_UNDEFINED, &m_heap);
    err= lock_clust_rec_read_check_and_lock(
      0, btr_pcur_get_block(&m_pcur), rec, clust_index, offsets, LOCK_X,
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
                                        const_cast<rec_t*>(rec), clust_index,
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
  ut_ad(m_mtr.trx);
  dict_index_t *clust_index= dict_table_get_first_index(table);
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
  m_pcur.btr_cur.page_cur.index= clust_index;
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
  offsets= rec_get_offsets(rec, clust_index, nullptr,
                           clust_index->n_core_fields,
                           ULINT_UNDEFINED, &m_heap);

  if (trx && trx->read_view.is_open())
  {
    trx_id_t rec_trx_id= row_get_rec_trx_id(rec, clust_index, offsets);
    if (rec_trx_id && !trx->read_view.changes_visible(rec_trx_id))
    {
      err= DB_RECORD_NOT_FOUND;
      goto err_exit;
    }
  }
  cmp= cmp_dtuple_rec_with_match(search_tuple, rec, clust_index,
                                 offsets, &matched_fields);
  if (cmp != 0)
  {
    err= DB_RECORD_NOT_FOUND;
    goto err_exit;
  }

  err= lock_clust_rec_read_check_and_lock(
    0, btr_pcur_get_block(&m_pcur), rec, clust_index, offsets, LOCK_X,
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
      callback->compare_record(search_tuple, rec, clust_index);
    if (action == RecordCompareAction::PROCESS)
    {
      dberr_t proc_err= callback->process_record(rec, clust_index, offsets);
      if (proc_err != DB_SUCCESS)
      {
        err= proc_err;
        goto err_exit;
      }
    }
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
  ut_ad(m_mtr.trx);
  dict_index_t *clust_index= dict_table_get_first_index(table);
  rec_t *rec= btr_pcur_get_rec(&m_pcur);
  mtr_x_lock_index(clust_index, &m_mtr);
  rec_offs *offsets= rec_get_offsets(rec, clust_index, nullptr,
                                     clust_index->n_core_fields,
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
  dict_index_t *clust_index= dict_table_get_first_index(table);

  m_mtr.start();
  ut_ad(m_mtr.trx);
  if (!m_mtr.trx->read_view.is_open())
  {
    trx_start_if_not_started(m_mtr.trx, false);
    m_mtr.trx->read_view.open(m_mtr.trx);
  }
  m_pcur.btr_cur.page_cur.index= clust_index;
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
    err= m_pcur.open_leaf(true, clust_index, BTR_SEARCH_LEAF, &m_mtr);
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
    RecordCompareAction action= callback.compare_record(
      tuple, rec, clust_index);
    if (action == RecordCompareAction::PROCESS)
    {
      dberr_t err= process_record_with_mvcc(clust_index, rec, callback);
      if (err == DB_SUCCESS_LOCKED_REC)
      {
        err= DB_SUCCESS;
        match_count++;
        goto func_exit;
      }
      if (err != DB_SUCCESS)
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
func_exit:
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

  m_mtr.start();
  if (m_mtr.trx && !m_mtr.trx->read_view.is_open())
  {
    trx_start_if_not_started(m_mtr.trx, false);
    m_mtr.trx->read_view.open(m_mtr.trx);
  }
  m_pcur.btr_cur.page_cur.index= sec_index;

  ulint match_count= 0;
  dberr_t err= btr_pcur_open_on_user_rec(
    search_tuple, BTR_SEARCH_LEAF, &m_pcur, &m_mtr);

  if (err != DB_SUCCESS)
    goto func_exit;

  while (btr_pcur_is_on_user_rec(&m_pcur))
  {
    const rec_t *sec_rec= btr_pcur_get_rec(&m_pcur);
    RecordCompareAction action= callback.compare_record(search_tuple, sec_rec,
                                                        sec_index);
    if (action == RecordCompareAction::PROCESS)
    {
      /* Lookup clustered record and process it */
      err= lookup_clustered_record(
        sec_index, sec_rec, callback, match_count);
      if (err != DB_SUCCESS)
        goto func_exit;
    }
    else if (action == RecordCompareAction::STOP)
      break;
    if (!btr_pcur_move_to_next(&m_pcur, &m_mtr)) break;
  }
  err= match_count > 0 ? DB_SUCCESS : DB_RECORD_NOT_FOUND;

func_exit:
  m_mtr.commit();
  return err == DB_SUCCESS_LOCKED_REC ? DB_SUCCESS : err;
}

dberr_t QueryExecutor::lookup_clustered_record(dict_index_t *sec_index,
                                               const rec_t *sec_rec,
                                               RecordCallback& callback,
                                               ulint& match_count) noexcept
{
  ut_ad(sec_index->is_normal_btree());
  ut_ad(!strcmp(sec_index->name(), FTS_DOC_ID_INDEX.str));
  dict_index_t *clust_index= dict_table_get_first_index(sec_index->table);

  /* Build clustered index search tuple from secondary record */
  dtuple_t *clust_ref= row_build_row_ref(ROW_COPY_POINTERS, sec_index,
                                         sec_rec, m_heap);
  if (!m_clust_pcur)
  {
    m_clust_pcur= new btr_pcur_t;
    btr_pcur_init(m_clust_pcur);
  }

  if (!m_offsets_heap)
    m_offsets_heap= mem_heap_create(256);
  /* Use Row_sel_get_clust_rec_for_mysql to get clustered record */
  Row_sel_get_clust_rec_for_mysql row_sel_get_clust_rec_for_mysql;
  const rec_t *clust_rec= nullptr;
  rec_offs *clust_offsets= nullptr;

  ulint savepoint= m_mtr.get_savepoint();

  dberr_t err=
    row_sel_get_clust_rec_for_mysql(clust_ref, m_clust_pcur, LOCK_NONE,
                                    m_mtr.trx, m_version_heap,
				    sec_index, sec_rec, m_thr,
				    &clust_rec, &clust_offsets,
				    &m_offsets_heap, nullptr, &m_mtr);

  if (err == DB_SUCCESS && clust_rec != nullptr)
  {
    /* Process the clustered record with MVCC handling */
    err= callback.process_record(clust_rec, clust_index, clust_offsets);
    if (err == DB_SUCCESS || err == DB_SUCCESS_LOCKED_REC)
      match_count++;
  }

  /* Clean up offset heap */
  if (m_offsets_heap)
    mem_heap_empty(m_offsets_heap);

  m_mtr.rollback_to_savepoint(savepoint, savepoint + 1);
  return err;
}

dberr_t QueryExecutor::process_record_with_mvcc(
   dict_index_t *clust_index, const rec_t *rec,
   RecordCallback &callback) noexcept
{
  ut_ad(m_mtr.trx);
  ut_ad(srv_read_only_mode || m_mtr.trx->read_view.is_open());

  dberr_t error= DB_SUCCESS;
  trx_id_t rec_trx_id= get_record_trx_id(rec, clust_index);
  rec_offs* offsets= nullptr;
  rec_offs* version_offsets= nullptr;

  rec_t *result_rec= const_cast<rec_t*>(rec);
  if (rec_trx_id && !m_mtr.trx->read_view.changes_visible(rec_trx_id))
  {
    /* Compute clustered index offsets */
    if (!m_version_heap)
      m_version_heap= mem_heap_create(128);
    offsets= rec_get_offsets(rec, clust_index, nullptr,
                             clust_index->n_core_fields,
                             ULINT_UNDEFINED, &m_version_heap);
    error= row_vers_build_for_consistent_read(
      rec, &m_mtr, clust_index, &offsets, &m_mtr.trx->read_view,
      &m_version_heap, m_version_heap, &result_rec, nullptr);
    if (error == DB_SUCCESS && result_rec)
      version_offsets= rec_get_offsets(result_rec, clust_index, nullptr,
                                       clust_index->n_core_fields,
                                       ULINT_UNDEFINED, &m_version_heap);
  }

  if (error != DB_SUCCESS || !result_rec)
    goto func_exit;

  if (rec_get_deleted_flag(result_rec, clust_index->table->not_redundant()))
    goto func_exit;

  error= callback.process_record(result_rec, clust_index, version_offsets);
func_exit:
  if (m_version_heap) mem_heap_empty(m_version_heap);
  return error;
}
