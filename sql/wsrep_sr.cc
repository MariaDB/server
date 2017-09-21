/* Copyright (C) 2013-2016 Codership Oy <info@codership.com>

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

#include "wsrep_applier.h"

#include "wsrep_priv.h"
#include "wsrep_sr.h"
#include "wsrep_thd.h"

#include "transaction.h" // trans_rollback()
#include "debug_sync.h" // DEBUG_SYNC()
#include "rpl_handler.h" // RUN_hOOK()

void Wsrep_SR_rollback_queue::append_SR_rollback(THD *thd)
{
  if (thd->wsrep_SR_rollback_replicated_for_trx == thd->wsrep_trx_id())
  {
    WSREP_DEBUG("SR rollback append skipped for thd: %lld conf %d",
                thd->thread_id, thd->wsrep_conflict_state());
    return;
  }

  thd->wsrep_SR_rollback_replicated_for_trx = thd->wsrep_ws_handle.trx_id;

  mysql_mutex_lock(&mutex_);
  WSREP_DEBUG("SR rollback append for thd: %lld "
              "query %lld  srctrx: %ld trx %ld  conf %d",
              thd->thread_id, thd->query_id, thd->wsrep_trx_meta.stid.trx,
              thd->wsrep_ws_handle.trx_id, thd->wsrep_conflict_state());

  if (map_.find(thd->thread_id) == map_.end())
  {
    map_[thd->thread_id] = new wsrep_SR_rollback_event(thd);

    /* TODO: thd should go for wsrep_rollback later to free all trx allocations
    my_free(thd->wsrep_rbr_buf);
    thd->wsrep_rbr_buf = NULL;
    */
  }
  mysql_mutex_unlock(&mutex_);
}

void Wsrep_SR_rollback_queue::send_SR_rollbacks(THD *thd)
{
  if (thd->wsrep_exec_mode != LOCAL_STATE) return;

  mysql_mutex_lock(&mutex_);

  std::map<my_thread_id, class wsrep_SR_rollback_event*>::iterator i;

  for(i = map_.begin(); i != map_.end(); ++i)
  {
    class wsrep_SR_rollback_event *event = i->second;

    /* replicate event */
    wsrep_ws_handle_t ws_handle = event->get_ws_handle();

    wsrep_status_t const ret= wsrep->rollback(wsrep, ws_handle.trx_id, NULL);

    if (ret != WSREP_OK)
    {
      WSREP_WARN("SR rollback replication failure, thd: %lld, trx_id: %lu SQL: %s",
                 thd->thread_id, thd->wsrep_trx_id(), thd->query());
    }
    delete event;
  }
  map_.clear();
  mysql_mutex_unlock(&mutex_);
}

void wsrep_SR_trx_info::remove(THD* caller, bool persistent)
{
  WSREP_DEBUG("wsrep_SR_trx_info::remove for thd: %lld trx: %lu",
              (thd_) ? thd_->thread_id : -1,
              (thd_) ? thd_->wsrep_trx_id() : -1);

  if (thd_) {
    if (caller) thd_->thread_stack = caller->thread_stack;
    thd_->store_globals();
    /* rollback */
    wsrep_cb_status_t const rcode(trans_rollback_stmt(thd_) ||
                                  trans_rollback(thd_) ?
                                  WSREP_CB_FAILURE : WSREP_CB_SUCCESS);

    if (rcode != WSREP_CB_SUCCESS)
      WSREP_INFO("SR rollback failed, ret: %d, thd: %lld", rcode,thd_->thread_id);

    /* remove persistency records */
    if (wsrep_SR_store && persistent) wsrep_SR_store->rollback_trx(this);

    /* end and delete thd_ */
    wsrep_end_SR_THD(thd_);

    /* retain working THD */
    if (caller) caller->store_globals();
    thd_ = NULL;
  }
}

void wsrep_SR_trx_info::cleanup()
{
  if (wsrep_SR_store) wsrep_SR_store->remove_trx(this);
  get_THD()->store_globals();
}

/* to prepare wsrep_uuid_t usable in std::map */
inline bool operator==(const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return(memcmp(lhs.data, rhs.data, 16) == 0);
}
inline bool operator!=(const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return !operator==(lhs,rhs);
}
inline bool operator< (const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return(memcmp(lhs.data, rhs.data, 16) < 0);
}
inline bool operator> (const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return  operator< (rhs,lhs);
}
inline bool operator<=(const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return !operator> (lhs,rhs);
}
inline bool operator>=(const wsrep_uuid_t& lhs, const wsrep_uuid_t& rhs) {
  return !operator< (lhs,rhs);
}

wsrep_SR_trx_info*  SR_pool::find(const wsrep_uuid_t& nodeID,
                                  const uint64_t& trxID) const
{
  wsrep_SR_trx_info* ret(NULL);
  if (mysql_mutex_lock (&LOCK_wsrep_SR_pool)) abort();

  src_pool_t::const_iterator const si(pool_.find(nodeID));
  if (si != pool_.end())
  {
    trx_pool_t::const_iterator const ti(si->second.find(trxID));
    if (ti != si->second.end()) ret = ti->second;
  }

  if (mysql_mutex_unlock (&LOCK_wsrep_SR_pool)) abort();
  return ret;
}

wsrep_SR_trx_info* SR_pool::add(const wsrep_uuid_t& nodeID, const uint64_t& trxID, THD *thd)
{
  wsrep_SR_trx_info *trx = new wsrep_SR_trx_info(thd);

  if (mysql_mutex_lock (&LOCK_wsrep_SR_pool)) abort();

  assert(NULL == pool_[nodeID][trxID]);

  pool_[nodeID][trxID] = trx;

  if (mysql_mutex_unlock (&LOCK_wsrep_SR_pool)) abort();

  return trx;
}

void SR_pool::remove(THD* caller, const wsrep_uuid_t& nodeID,
                     const uint64_t& trxID, bool persistent)
{
  wsrep_SR_trx_info *trx(NULL);
  if (mysql_mutex_lock (&LOCK_wsrep_SR_pool)) abort();
  WSREP_DEBUG("sr_pool remove for node's trx");
  src_pool_t::iterator const si(pool_.find(nodeID));
  if (si != pool_.end())
  {
    trx_pool_t& trx_pool(si->second);
    trx_pool_t::iterator ti(trx_pool.find(trxID));
    if (ti != trx_pool.end())
    {
      trx = (wsrep_SR_trx_info*)ti->second;
      /* remove transaction persistently */
      trx->remove(caller, persistent);
      WSREP_DEBUG("sr_pool->trx_pool remove for trx: %lu", trxID);
      delete trx;
      trx_pool.erase(ti);
    }

    if (trx_pool.empty()) pool_.erase(si);
  }

  if (mysql_mutex_unlock (&LOCK_wsrep_SR_pool)) abort();
  return;
}

bool SR_pool::remove(THD* caller, THD *victim, bool persistent)
{
  bool ret(false);

  if (mysql_mutex_lock (&LOCK_wsrep_SR_pool)) abort();
  src_pool_t::iterator si;
  WSREP_DEBUG("sr_pool remove for THD %lld, persistent: %d",
              (victim) ? victim->thread_id : -1, persistent);

  for (si = pool_.begin(); si != pool_.end();)
  {
    trx_pool_t& trx_pool(si->second);

    trx_pool_t::iterator ti(trx_pool.begin());

    while (ti != trx_pool.end())
    {
      wsrep_SR_trx_info *trx = (wsrep_SR_trx_info*)ti->second;

      if (!victim || trx->get_THD() == victim)
      {
        WSREP_DEBUG("Found SR transaction to remove: %lld", 
                    (victim) ? victim->thread_id : -1);
        ret = true;

        trx->remove(caller, persistent);
        delete trx;

        /* TODO: transaction to rollback */
        trx_pool_t::iterator prev = ti++;

        trx_pool.erase(prev);
      }
      else
      {
        ti++;
      }

    pool_.erase(si++);
  }
  if (mysql_mutex_unlock (&LOCK_wsrep_SR_pool)) abort();
  return (ret);
  }
}

void SR_pool::trimToNodes (THD* caller, const wsrep_member_info_t nodes[], int nodeCount)
{
  WSREP_DEBUG("SR_pool::trimToNodes");
  src_pool_t::iterator si;

  si = pool_.begin();
  while(si != pool_.end()) {

    bool do_remove(true);
    for (int i = 0; i< nodeCount; i++)
    {
      if (si->first == nodes[i].id)
      {
        do_remove = false;
        continue;
      }
    }

    if (do_remove)
    {
      WSREP_DEBUG("SR_pool::trimToNodes do_remove");

      /* trx_pool erase will not desctruct individual wsrep_SR_trx_info
         objects, must remove them manually here
      */
      trx_pool_t& trx_pool(si->second);
      trx_pool_t::iterator ti(trx_pool.begin());

      while (ti != trx_pool.end())
      {
        wsrep_SR_trx_info *trx = (wsrep_SR_trx_info*)ti->second;

        WSREP_DEBUG("SR transaction to remove: %lld", trx->get_THD()->thread_id);

        trx->remove(caller, true);
        delete trx;
        ti++;
      }

      /* now removing full trx_pool map */
      pool_.erase(si++);
    }
    else
    {
      si++;
    }
  }
}

SR_pool *sr_pool;

void  wsrep_close_SR_transactions(THD *thd)
{

  if (sr_pool)
  {
    WSREP_DEBUG("deleting streaming replication transaction pool");
    (void)sr_pool->remove(thd, NULL, false);
  }
  else
  {
    WSREP_DEBUG("empty streaming replication transaction pool");
  }

  if (wsrep_SR_store) wsrep_SR_store->close();

  delete sr_pool;
  sr_pool = NULL;

  //if (thd) thd->store_globals();
}

void trim_SR_pool(THD* thd, const wsrep_member_info_t nodes[], int nodeCount)
{
  sr_pool->trimToNodes(thd, nodes, nodeCount);
}

void wsrep_init_SR_pool()
{
  WSREP_DEBUG("wsrep_init_SR_pool");

  /* for time being initialize SR pool here */
  if (!sr_pool) sr_pool = new SR_pool();
}

void wsrep_restore_SR_trxs(THD *thd)
{
  if (wsrep_SR_store) wsrep_SR_store->restore(thd);
}

bool wsrep_abort_SR_THD(THD *thd, THD *victim_thd)
{
  bool ret = sr_pool->remove(thd, victim_thd, true);
  //if (thd) thd->store_globals();

  return (ret);
}

void wsrep_prepare_SR_trx_info_for_rollback(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  wsrep_uuid_t node_uuid;
  wsrep_node_uuid(node_uuid);

  DBUG_ASSERT(thd->wsrep_ws_handle.trx_id != WSREP_UNDEFINED_TRX_ID);

  int error= 1;
  THD *sr_thd= wsrep_start_SR_THD(thd->thread_stack);
  if (sr_thd)
  {
    wsrep_SR_trx_info *SR_trx= sr_pool->add(node_uuid,
                                            thd->wsrep_ws_handle.trx_id,
                                            sr_thd);
    if (SR_trx)
    {
      for (wsrep_fragment_set::const_iterator i=
             thd->wsrep_SR_fragments.begin();
           i != thd->wsrep_SR_fragments.end(); ++i)
      {
        SR_trx->append_fragment(&(*i));
      }
      error= 0;
    }
  }
  if (error)
  {
    WSREP_WARN("Could not create SR trx info for rollback, wsrep_schema.SR "
               "table may not be cleaned up for transaction %lu",
               thd->wsrep_ws_handle.trx_id);
  }
  else
  {
    thd->wsrep_SR_fragments.clear();
  }

  thd->store_globals();
}


void wsrep_remove_SR_fragments(THD *thd)
{
  if (wsrep_SR_store) wsrep_SR_store->remove_trx(thd);
}

void wsrep_rollback_SR_trx(THD *thd)
{
  if (wsrep_SR_store) wsrep_SR_store->rollback_trx(thd);
  thd->wsrep_SR_fragments.clear();
}

void wsrep_prepare_SR_for_open_tables(THD *thd, TABLE_LIST **table_list)
{
  if (wsrep_SR_store)
  {
    wsrep_SR_store->prepare_for_open_tables(thd, table_list);
  }
}

void wsrep_handle_SR_rollback(void *BF_thd_ptr, void *victim_thd_ptr)
{
  DBUG_ASSERT(victim_thd_ptr);
  if (!victim_thd_ptr) return;

  THD *victim_thd = (THD*) victim_thd_ptr;
  WSREP_DEBUG("handle SR rollback, for deadlock: thd %lld trx_id %lu frags %lu conf %d",
              victim_thd->thread_id,
              victim_thd->wsrep_trx_id(),
              victim_thd->wsrep_fragments_sent,
              victim_thd->wsrep_conflict_state_unsafe());
  (void)RUN_HOOK(transaction, before_rollback, (victim_thd, true));

  if (BF_thd_ptr) ((THD*) BF_thd_ptr)->store_globals();
}
