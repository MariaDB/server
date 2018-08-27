/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */


#include "wsrep_priv.h"
#include "wsrep_sr_table.h"
#include "wsrep_schema.h"

#include "sql_class.h"


SR_storage_table::SR_storage_table()
  :
  wsrep_schema_(0)
{
}

SR_storage_table::~SR_storage_table()
{
}

int SR_storage_table::init(const char* cluster_uuid_str,
                           Wsrep_schema* wsrep_schema)
{
  /* Storage tables have been created by Wsrep_schema */
  wsrep_schema_= wsrep_schema;
  return 0;
}


THD* SR_storage_table::append_frag(THD*         thd,
                                   uint32_t     flags,
                                   const uchar* buf,
                                   size_t       buf_len)
{
  /*
    During restore phase fragments are read from storage, don't
    append back.
   */
  if (restored_ == false) {
    return NULL;
  }

  thd->wsrep_trx_meta.stid.trx = thd->wsrep_ws_handle.trx_id;

  THD* ret = wsrep_schema_->append_frag(thd->wsrep_trx_meta, flags,
                                        buf, buf_len);
  if (!ret) {
    WSREP_ERROR("Failed to append frag to persistent storage");
  }
  else {
    WSREP_DEBUG("SR_storage_table::append_frag(): thd %lld, seqno %lld, "
                "trx_id: %ld, thd_ret: %p",
                thd->thread_id, (long long)thd->wsrep_trx_meta.gtid.seqno,
                thd->wsrep_trx_meta.stid.trx, ret);
  }

  thd->store_globals(); /* Restore original thread context */
  return ret;
}

void SR_storage_table::update_frag_seqno(THD* thd, THD* orig_THD)
{
  thd->store_globals();

  if (wsrep_schema_->update_frag_seqno(thd, orig_THD->wsrep_trx_meta)) {
    WSREP_ERROR("Failed to update seqno, must abort");
    unireg_abort(1);
  }

  orig_THD->wsrep_SR_fragments.push_back(orig_THD->wsrep_trx_meta);
  orig_THD->store_globals(); /* Restore original thread context */
}

void SR_storage_table::release_SR_thd(THD* thd)
{
  thd->store_globals();
  wsrep_schema_->release_SR_thd(thd);
}

void SR_storage_table::append_frag_apply(THD*         thd,
                                         uint32_t     flags,
                                         const uchar* buf,
                                         size_t       buf_len)
{
  /*
    During restore phase fragments are read from storage, don't
    append back.
   */
  if (restored_ == false) {
    return;
  }

  if (wsrep_schema_->append_frag_apply(thd, thd->wsrep_trx_meta, flags,
                                       buf, buf_len)) {
    WSREP_ERROR("Failed to append frag to persistent storage, must abort");
    unireg_abort(1);
  }

  thd->store_globals(); /* Restore original thread context */
}

void SR_storage_table::append_frag_commit(THD*         thd,
                                          uint32_t     flags,
                                          const uchar* buf,
                                          size_t       buf_len)
{
  /*
    During restore phase fragments are read from storage, don't
    append back.
   */
  if (restored_ == false) {
    return;
  }

  if (wsrep_schema_->append_frag_commit(thd->wsrep_trx_meta, flags,
                                        buf, buf_len)) {
    WSREP_ERROR("Failed to append frag to persistent storage, must abort");
    unireg_abort(1);
  }

  thd->wsrep_SR_fragments.push_back(thd->wsrep_trx_meta);
  thd->store_globals(); /* Restore original thread context */
}

void SR_storage_table::remove_trx(THD* thd)
{
  WSREP_DEBUG("SR_storage_table::remove_trx(%lld) seqno %lld, trx %ld",
              thd->thread_id, (long long)thd->wsrep_trx_meta.gtid.seqno,
              thd->wsrep_trx_meta.stid.trx);
  int err= wsrep_schema_->remove_trx(thd, &thd->wsrep_SR_fragments);
  if (err == -1) {
    WSREP_DEBUG("SR_storage_table::remove_trx() interrupted");
  } else if (err) {
    WSREP_WARN("Failed to delete fragments from persistent storage");
  }
}

void SR_storage_table::remove_trx(wsrep_SR_trx_info* trx)
{
  remove_trx(trx->get_THD());
}

void SR_storage_table::rollback_trx(THD* thd)
{
  WSREP_DEBUG("SR_storage_table::rollback_trx(%lld) seqno %lld, trx %ld",
              thd->thread_id, (long long)thd->wsrep_trx_meta.gtid.seqno,
              thd->wsrep_trx_meta.stid.trx);
  int err= wsrep_schema_->rollback_trx(thd);
  if (err == -1) {
    WSREP_DEBUG("SR_storage_table::rollback_trx() interrupted");
  } else if (err) {
    WSREP_WARN("Failed to delete fragments from persistent storage");
  }
  thd->store_globals(); /* Restore original thread context */
}

void SR_storage_table::rollback_trx(wsrep_SR_trx_info* trx)
{
  rollback_trx(trx->get_THD());
}

void SR_storage_table::trx_done(THD* thd)
{
  if (thd->wsrep_conflict_state() != MUST_REPLAY)
    thd->wsrep_SR_fragments.clear();
}

int SR_storage_table::replay_trx(THD* thd, const wsrep_trx_meta_t& meta)
{
  return wsrep_schema_->replay_trx(thd, meta);
}

int SR_storage_table::restore(THD* thd)
{
  if (restored_ == true) {
    WSREP_DEBUG("SR_storage_table::restore: Already restored");
    return 0;
  }

  WSREP_INFO("SR_storage_table::restore");
  int ret= wsrep_schema_->restore_frags();
  if (thd) {
    thd->store_globals();
  }
  else {
    my_pthread_setspecific_ptr(THR_THD, NULL);
  }
  restored_= true;
  return ret;
}

void SR_storage_table::prepare_for_open_tables(THD *thd, TABLE_LIST **table_list)
{
  wsrep_schema_->init_SR_table(&thd->wsrep_SR_table);
  TABLE_LIST *ptr= *table_list;
  if (!ptr)
  {
    *table_list= &thd->wsrep_SR_table;
  }
  else
  {
    while (ptr->next_global) ptr = ptr->next_global;
    ptr->next_global = &thd->wsrep_SR_table;
  }
}

void SR_storage_table::close()
{
}
