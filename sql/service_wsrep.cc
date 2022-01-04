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

extern "C" void wsrep_thd_kill_LOCK(const THD *thd)
{
  mysql_mutex_lock(&thd->LOCK_thd_kill);
}

extern "C" void wsrep_thd_kill_UNLOCK(const THD *thd)
{
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
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
  if (!thd)
    return "NULL";

  switch(thd->lex->sql_command)
  {
    // Mask away some security related details from error log
    case SQLCOM_CREATE_USER:
      return "CREATE USER";
    case SQLCOM_GRANT:
      return "GRANT";
    case SQLCOM_REVOKE:
      return "REVOKE";
    case SQLCOM_SET_OPTION:
      if (thd->lex->definer)
        return "SET PASSWORD";
      /* fallthrough */
    default:
      return (thd->query() ? thd->query() : "NULL");
  }
  return "NULL";
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

/*
  Test if this connection is a true local (user) connection and not
  a replication or wsrep applier thread.

  Note that this is only usable for galera (as there are other kinds
  of system threads, and only if WSREP_NNULL() is tested by the caller.
 */
extern "C" my_bool wsrep_thd_is_local(const THD *thd)
{
  /*
    async replication IO and background threads have nothing to
    replicate in the cluster, marking them as non-local here to
    prevent write set population and replication

    async replication SQL thread, applies client transactions from
    mariadb master and will be replicated into cluster
  */
  return (
          thd->system_thread != SYSTEM_THREAD_SLAVE_BACKGROUND &&
          thd->system_thread != SYSTEM_THREAD_SLAVE_IO &&
          thd->wsrep_cs().mode() == wsrep::client_state::m_local);
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
  DBUG_ASSERT(wsrep_thd_is_SR(victim_thd));
  if (!victim_thd || !wsrep_on(bf_thd)) return;

  WSREP_DEBUG("handle rollback, for deadlock: thd %llu trx_id %" PRIu64 " frags %zu conf %s",
              victim_thd->thread_id,
              victim_thd->wsrep_trx_id(),
              victim_thd->wsrep_sr().fragments_certified(),
              wsrep_thd_transaction_state_str(victim_thd));

  /* Note: do not store/reset globals before wsrep_bf_abort() call
     to avoid losing BF thd context. */
  if (!(bf_thd && bf_thd != victim_thd))
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
  if (bf_thd)
  {
    wsrep_store_threadvars(bf_thd);
  }
}

extern "C" my_bool wsrep_thd_bf_abort(THD *bf_thd, THD *victim_thd,
                                      my_bool signal)
{
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_kill);
  mysql_mutex_assert_not_owner(&victim_thd->LOCK_thd_data);
  my_bool ret= wsrep_bf_abort(bf_thd, victim_thd);
  /*
    Send awake signal if victim was BF aborted or does not
    have wsrep on. Note that this should never interrupt RSU
    as RSU has paused the provider.
   */
  if ((ret || !wsrep_on(victim_thd)) && signal)
  {
    mysql_mutex_lock(&victim_thd->LOCK_thd_data);

    if (victim_thd->wsrep_aborter && victim_thd->wsrep_aborter != bf_thd->thread_id)
    {
      WSREP_DEBUG("victim is killed already by %llu, skipping awake",
                  victim_thd->wsrep_aborter);
      mysql_mutex_unlock(&victim_thd->LOCK_thd_data);
      return false;
    }

    victim_thd->wsrep_aborter= bf_thd->thread_id;
    victim_thd->awake_no_mutex(KILL_QUERY);
    mysql_mutex_unlock(&victim_thd->LOCK_thd_data);
  } else {
    WSREP_DEBUG("wsrep_thd_bf_abort skipped awake");
  }
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
  /*
    In case of `wsrep_gtid_mode` when WS will be replicated, we need to set
    `server_id` for events that are going to be written in IO, and in case of
    manual SET gtid_seq_no=X we are ignoring value.
   */
  if (!ret && wsrep_gtid_mode && !thd->slave_thread && !wsrep_thd_is_applying(thd))
  {
    thd->variables.server_id= wsrep_gtid_server.server_id;
    thd->variables.gtid_seq_no= 0;
  }
  return ret;
}

extern "C" void wsrep_commit_ordered(THD *thd)
{
  if (wsrep_is_active(thd) &&
      (thd->wsrep_trx().state() == wsrep::transaction::s_committing ||
       thd->wsrep_trx().state() == wsrep::transaction::s_ordered_commit))
  {
    wsrep_gtid_server.signal_waiters(thd->wsrep_current_gtid_seqno, false);
    if (wsrep_thd_is_local(thd))
    {
      thd->wsrep_last_written_gtid_seqno= thd->wsrep_current_gtid_seqno;
    }
    if (thd->wsrep_trx().state() != wsrep::transaction::s_ordered_commit &&
        !wsrep_commit_will_write_binlog(thd))
    {
      DEBUG_SYNC(thd, "before_wsrep_ordered_commit");
      thd->wsrep_cs().ordered_commit();
    }
  }
}

extern "C" my_bool wsrep_thd_has_ignored_error(const THD *thd)
{
  return thd->wsrep_has_ignored_error;
}

extern "C" void wsrep_thd_set_ignored_error(THD *thd, my_bool val)
{
  thd->wsrep_has_ignored_error= val;
}

extern "C" ulong wsrep_OSU_method_get(const MYSQL_THD thd)
{
  if (thd)
    return(thd->variables.wsrep_OSU_method);
  else
    return(global_system_variables.wsrep_OSU_method);
}

extern "C" bool wsrep_thd_set_wsrep_aborter(THD *bf_thd, THD *victim_thd)
{
  WSREP_DEBUG("wsrep_thd_set_wsrep_aborter called");
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);
  if (victim_thd->wsrep_aborter && victim_thd->wsrep_aborter != bf_thd->thread_id)
  {
    return true;
  }
  victim_thd->wsrep_aborter = bf_thd->thread_id;
  return false;
}

extern "C" void wsrep_report_bf_lock_wait(const THD *thd,
                                          unsigned long long trx_id)
{
  if (thd)
  {
    WSREP_ERROR("Thread %s trx_id: %llu thread: %ld "
                "seqno: %lld client_state: %s client_mode: %s transaction_mode: %s "
                "applier: %d toi: %d local: %d "
                "query: %s",
                wsrep_thd_is_BF(thd, false) ? "BF" : "normal",
                trx_id,
                thd_get_thread_id(thd),
                wsrep_thd_trx_seqno(thd),
                wsrep_thd_client_state_str(thd),
                wsrep_thd_client_mode_str(thd),
                wsrep_thd_transaction_state_str(thd),
                wsrep_thd_is_applying(thd),
                wsrep_thd_is_toi(thd),
                wsrep_thd_is_local(thd),
                wsrep_thd_query(thd));
  }
}

extern "C" void  wsrep_thd_set_PA_unsafe(THD *thd)
{
  if (thd && thd->wsrep_cs().mark_transaction_pa_unsafe())
  {
    WSREP_DEBUG("session does not have active transaction, can not mark as PA unsafe");
  }
}
