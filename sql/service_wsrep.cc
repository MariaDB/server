/* Copyright 2018 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */
#include "mariadb.h"

#include "mysql/service_wsrep.h"
#include "wsrep/key.hpp"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"
#include "sql_class.h"
#include "debug_sync.h"

extern "C" my_bool wsrep_global_on()
{
  return WSREP_ON;
}

extern "C" my_bool wsrep_on(const void *thd)
{
  return (int)(WSREP(((const THD*)thd)));
}

extern "C" void wsrep_thd_LOCK(const void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  mysql_mutex_lock(&thd->LOCK_thd_data);
}

extern "C" void wsrep_thd_UNLOCK(const void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}


extern "C" my_bool wsrep_thd_is_wsrep_on(const void *thd)
{
  return ((const THD*)thd)->variables.wsrep_on;
}

extern "C" my_thread_id wsrep_thd_thread_id(const void *thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return thd->thread_id;
}

extern "C" const char* wsrep_thd_client_state_str(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return wsrep::to_c_string(thd->wsrep_cs().state());
}

extern "C" const char* wsrep_thd_client_mode_str(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return wsrep::to_c_string(thd->wsrep_cs().mode());
}

extern "C" const char* wsrep_thd_transaction_state_str(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return wsrep::to_c_string(thd->wsrep_cs().transaction().state());
}


extern "C" const char *wsrep_thd_query(const void *thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  return (thd) ? thd->query() : NULL;
}

extern "C" query_id_t wsrep_thd_query_id(const void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  return thd->query_id;
}

extern "C" query_id_t wsrep_thd_transaction_id(const void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  return thd->wsrep_cs().transaction().id().get();
}

extern "C" long long wsrep_thd_trx_seqno(const void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  const wsrep::client_state& cs= thd->wsrep_cs();
  if (cs.mode() == wsrep::client_state::m_toi)
  {
    return cs.toi_meta().seqno().get();
  }
  else
  {
    return cs.transaction().ws_meta().seqno().get();
  }
}

extern "C" void wsrep_thd_self_abort(void* thd_ptr)
{
  THD* thd= (THD*)thd_ptr;
  thd->wsrep_cs().bf_abort(wsrep::seqno(0));
}

extern "C" my_bool wsrep_thd_is_local(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd->wsrep_cs().mode() == wsrep::client_state::m_local);
}

extern "C" my_bool wsrep_thd_is_applying(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd->wsrep_cs().mode() == wsrep::client_state::m_high_priority);
}

extern "C" my_bool wsrep_thd_is_toi(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd->wsrep_cs().mode() == wsrep::client_state::m_toi);
}

extern "C" my_bool wsrep_thd_is_local_toi(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd->wsrep_cs().mode() == wsrep::client_state::m_toi &&
          thd->wsrep_cs().toi_mode() == wsrep::client_state::m_local);

}

extern "C" my_bool wsrep_thd_is_in_rsu(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd->wsrep_cs().mode() == wsrep::client_state::m_rsu);
}

extern "C" my_bool wsrep_thd_is_BF(const void *thd_ptr, my_bool sync)
{
  THD* thd= (THD*)thd_ptr;
  my_bool status = FALSE;
  if (thd_ptr && WSREP(thd))
  {
    if (sync) mysql_mutex_lock(&thd->LOCK_thd_data);
    status = (wsrep_thd_is_applying(thd) || wsrep_thd_is_toi(thd));
    if (sync) mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return status;
}

extern "C" my_bool wsrep_thd_is_SR(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd && thd->wsrep_cs().transaction().is_streaming());
}

extern "C" void wsrep_handle_SR_rollback(void *bf_thd_ptr,
                                         void *victim_thd_ptr)
{
  DBUG_ASSERT(victim_thd_ptr);
  if (!victim_thd_ptr || (bf_thd_ptr && !wsrep_on(bf_thd_ptr))) return;

  THD* bf_thd= (THD*)bf_thd_ptr;
  THD* victim_thd= (THD*)victim_thd_ptr;
  WSREP_DEBUG("handle rollback, for deadlock: thd %llu trx_id %lu frags %lu conf %s",
              victim_thd->thread_id,
              victim_thd->wsrep_trx_id(),
              victim_thd->wsrep_sr().fragments_certified(),
              wsrep_thd_transaction_state_str(victim_thd));
  if (bf_thd) victim_thd->store_globals();
  if (!bf_thd)
  {
    DEBUG_SYNC(victim_thd, "wsrep_before_SR_rollback");
  }
  if (bf_thd)
  {
    wsrep_bf_abort(bf_thd, victim_thd);
  }
  else
  {
    wsrep_thd_self_abort(victim_thd);
  }
  if (bf_thd) bf_thd->store_globals();
}

extern "C" void wsrep_thd_xid(const void *thd_ptr, void *xid, size_t xid_size)
{
  const THD *thd= (const THD*)thd_ptr;
  DBUG_ASSERT(xid_size == sizeof(xid_t));
  if (xid_size == sizeof(xid_t))
  {
    *(xid_t*) xid = thd->wsrep_xid;
  }
}

extern "C" void wsrep_thd_awake(const void* thd_ptr, my_bool signal)
{
  THD* thd= (THD*)thd_ptr;
  if (signal)
  {
    //mysql_mutex_lock(&thd->LOCK_thd_data);
    thd->awake(KILL_QUERY);
    //mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  else
  {
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    mysql_cond_broadcast(&COND_wsrep_replaying);
    mysql_mutex_unlock(&LOCK_wsrep_replaying);
  }
}

extern "C" my_bool wsrep_thd_bf_abort(const void* bf_thd_ptr,
                                      void* victim_thd_ptr,
                                      my_bool signal)
{
  const THD* bf_thd= (const THD*)bf_thd_ptr;
  THD* victim_thd= (THD*)victim_thd_ptr;
  my_bool ret= wsrep_bf_abort(bf_thd, victim_thd);
  /*
    Send awake signal if victim was BF aborted or does not
    have wsrep on. Note that this should never interrupt RSU
    as RSU has paused the provider.
   */
  if ((ret || !wsrep_on(victim_thd)) && signal)
  {
    wsrep_thd_awake((const void*)victim_thd, signal);
  }
  return ret;
}

extern "C" my_bool wsrep_thd_skip_locking(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return thd != NULL && thd->wsrep_skip_locking;
}

extern "C" my_bool wsrep_thd_order_before(const void *left_ptr,
                                          const void *right_ptr)
{
  const THD* left= (const THD*)left_ptr;
  const THD* right= (const THD*)right_ptr;
  if (wsrep_thd_trx_seqno(left) < wsrep_thd_trx_seqno(right)) {
    WSREP_DEBUG("BF conflict, order: %lld %lld\n",
                (long long)wsrep_thd_trx_seqno(left),
                (long long)wsrep_thd_trx_seqno(right));
    return TRUE;
  }
  WSREP_DEBUG("waiting for BF, trx order: %lld %lld\n",
              (long long)wsrep_thd_trx_seqno(left),
              (long long)wsrep_thd_trx_seqno(right));
  return FALSE;
}

extern "C" my_bool wsrep_thd_is_high_priority(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  return (thd != 0 && thd->wsrep_cs().mode() == wsrep::client_state::m_high_priority);
}

extern "C" my_bool wsrep_thd_is_aborting(const void* thd_ptr)
{
  const THD* thd= (const THD*)thd_ptr;
  mysql_mutex_assert_owner(&thd->LOCK_thd_data);
  if (thd != 0)
  {
    const wsrep::client_state& cs(thd->wsrep_cs());
    const enum wsrep::transaction::state tx_state(cs.transaction().state());
    switch (tx_state)
    {
    case wsrep::transaction::s_must_abort:
      return (cs.state() == wsrep::client_state::s_exec ||
              cs.state() == wsrep::client_state::s_result);
    case wsrep::transaction::s_aborting:
    case wsrep::transaction::s_aborted:
      return true;
    default:
      return false;
    }
  }
  return false;
}

static inline enum wsrep::key::type
map_key_type(enum Wsrep_key_type type)
{
  switch (type)
  {
  case wsrep_key_shared: return wsrep::key::shared;
  case wsrep_key_semi_shared: return wsrep::key::semi_shared;
  case wsrep_key_semi_exclusive: return wsrep::key::semi_exclusive;
  case wsrep_key_exclusive: return wsrep::key::exclusive;
  }
  return wsrep::key::exclusive;
}

extern "C" int wsrep_thd_append_key(void* thd_ptr,
                                    const struct wsrep_key* key,
                                    int n_keys,
                                    enum Wsrep_key_type key_type)
{
  THD* thd= (THD*)thd_ptr;
  Wsrep_client_state& client_state(thd->wsrep_cs());
  DBUG_ASSERT(client_state.transaction().active());
  int ret= 0;
  for (int i= 0; i < n_keys && ret == 0; ++i)
  {
    wsrep::key wsrep_key(map_key_type(key_type));
    for (size_t kp= 0; kp < key[i].key_parts_num; ++kp)
    {
      wsrep_key.append_key_part(key[i].key_parts[kp].ptr, key[i].key_parts[kp].len);
    }
    ret= client_state.append_key(wsrep_key);
  }
  return ret;
}
