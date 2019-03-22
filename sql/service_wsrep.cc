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
#include "log.h"

extern "C" my_bool wsrep_on(const THD *thd)
{
  return my_bool(WSREP(thd));
}

extern "C" void wsrep_thd_LOCK(const THD *thd)
{
  mysql_mutex_lock(&thd->LOCK_thd_data);
}

extern "C" void wsrep_thd_UNLOCK(const THD *thd)
{
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}

extern "C" const char* wsrep_thd_client_state_str(const THD *thd)
{
  return wsrep::to_c_string(thd->wsrep_cs().state());
}

extern "C" const char* wsrep_thd_client_mode_str(const THD *thd)
{
  return wsrep::to_c_string(thd->wsrep_cs().mode());
}

extern "C" const char* wsrep_thd_transaction_state_str(const THD *thd)
{
  return wsrep::to_c_string(thd->wsrep_cs().transaction().state());
}


extern "C" const char *wsrep_thd_query(const THD *thd)
{
  return thd ? thd->query() : NULL;
}

extern "C" query_id_t wsrep_thd_transaction_id(const THD *thd)
{
  return thd->wsrep_cs().transaction().id().get();
}

extern "C" long long wsrep_thd_trx_seqno(const THD *thd)
{
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

extern "C" void wsrep_thd_self_abort(THD *thd)
{
  thd->wsrep_cs().bf_abort(wsrep::seqno(0));
}

extern "C" const char* wsrep_get_sr_table_name()
{
  return wsrep_sr_table_name_full;
}

extern "C" my_bool wsrep_get_debug()
{
  return wsrep_debug;
}

extern "C" my_bool wsrep_thd_is_local(const THD *thd)
{
  return thd->wsrep_cs().mode() == wsrep::client_state::m_local;
}

extern "C" my_bool wsrep_thd_is_applying(const THD *thd)
{
  return thd->wsrep_cs().mode() == wsrep::client_state::m_high_priority;
}

extern "C" my_bool wsrep_thd_is_toi(const THD *thd)
{
  return thd->wsrep_cs().mode() == wsrep::client_state::m_toi;
}

extern "C" my_bool wsrep_thd_is_local_toi(const THD *thd)
{
  return thd->wsrep_cs().mode() == wsrep::client_state::m_toi &&
         thd->wsrep_cs().toi_mode() == wsrep::client_state::m_local;

}

extern "C" my_bool wsrep_thd_is_in_rsu(const THD *thd)
{
  return thd->wsrep_cs().mode() == wsrep::client_state::m_rsu;
}

extern "C" my_bool wsrep_thd_is_BF(const THD *thd, my_bool sync)
{
  my_bool status = FALSE;
  if (thd && WSREP(thd))
  {
    if (sync) mysql_mutex_lock(&thd->LOCK_thd_data);
    status = (wsrep_thd_is_applying(thd) || wsrep_thd_is_toi(thd));
    if (sync) mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return status;
}

extern "C" my_bool wsrep_thd_is_SR(const THD *thd)
{
  return thd && thd->wsrep_cs().transaction().is_streaming();
}

extern "C" void wsrep_handle_SR_rollback(THD *bf_thd,
                                         THD *victim_thd)
{
  DBUG_ASSERT(victim_thd);
  if (!victim_thd || !wsrep_on(bf_thd)) return;

  WSREP_DEBUG("handle rollback, for deadlock: thd %llu trx_id %lu frags %lu conf %s",
              victim_thd->thread_id,
              victim_thd->wsrep_trx_id(),
              victim_thd->wsrep_sr().fragments_certified(),
              wsrep_thd_transaction_state_str(victim_thd));
  if (bf_thd && bf_thd != victim_thd)
  {
    victim_thd->store_globals();
  }
  else
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
  if (bf_thd && bf_thd != victim_thd)
  {
    bf_thd->store_globals();
  }
}

extern "C" my_bool wsrep_thd_bf_abort(const THD *bf_thd, THD *victim_thd,
                                      my_bool signal)
{
  if (WSREP(victim_thd) && !victim_thd->wsrep_trx().active())
  {
    WSREP_DEBUG("BF abort for non active transaction");
    wsrep_start_transaction(victim_thd, victim_thd->wsrep_next_trx_id());
  }
  my_bool ret= wsrep_bf_abort(bf_thd, victim_thd);
  /*
    Send awake signal if victim was BF aborted or does not
    have wsrep on. Note that this should never interrupt RSU
    as RSU has paused the provider.
   */
  if ((ret || !wsrep_on(victim_thd)) && signal)
    victim_thd->awake(KILL_QUERY);
  return ret;
}

extern "C" my_bool wsrep_thd_skip_locking(const THD *thd)
{
  return thd && thd->wsrep_skip_locking;
}

extern "C" my_bool wsrep_thd_order_before(const THD *left, const THD *right)
{
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

extern "C" my_bool wsrep_thd_is_aborting(const MYSQL_THD thd)
{
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
map_key_type(enum Wsrep_service_key_type type)
{
  switch (type)
  {
  case WSREP_SERVICE_KEY_SHARED:    return wsrep::key::shared;
  case WSREP_SERVICE_KEY_REFERENCE: return wsrep::key::reference;
  case WSREP_SERVICE_KEY_UPDATE:    return wsrep::key::update;
  case WSREP_SERVICE_KEY_EXCLUSIVE: return wsrep::key::exclusive;
  }
  return wsrep::key::exclusive;
}

extern "C" int wsrep_thd_append_key(THD *thd,
                                    const struct wsrep_key* key,
                                    int n_keys,
                                    enum Wsrep_service_key_type key_type)
{
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

extern "C" void wsrep_commit_ordered(THD *thd)
{
  if (wsrep_is_active(thd) &&
      thd->wsrep_trx().state() == wsrep::transaction::s_committing &&
      !wsrep_commit_will_write_binlog(thd))
  {
    thd->wsrep_cs().ordered_commit();
  }
}
