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

#include "my_global.h"
#include "wsrep_server_service.h"
#include "wsrep_server_state.h"
#include "wsrep_client_state.h"
#include "wsrep_client_service.h"
#include "wsrep_storage_service.h"
#include "wsrep_high_priority_service.h"

#include "wsrep_sst.h"
#include "wsrep_xid.h"
#include "wsrep_mysqld.h"
#include "wsrep_schema.h"
#include "wsrep_utils.h"
#include "wsrep_thd.h"

#include "log.h" /* sql_print_xxx() */
#include "sql_class.h" /* system variables */
#include "transaction.h" /* trans_xxx */
#include "sql_base.h" /* close_thread_tables */
#include "debug_sync.h"

static void init_service_thd(THD* thd, char* thread_stack)
{
  thd->thread_stack= thread_stack;
  thd->real_id= pthread_self();
  thd->prior_thr_create_utime= thd->start_utime= microsecond_interval_timer();
  thd->set_command(COM_SLEEP);
  thd->reset_for_next_command(true);
  server_threads.insert(thd); // as wsrep_innobase_kill_one_trx() uses find_thread_by_id()
}

Wsrep_storage_service*
wsrep_create_storage_service(THD* orig_THD, const char* ctx)
{
  THD* thd= new THD(true, true);
  init_service_thd(thd, orig_THD->thread_stack);
  WSREP_DEBUG("Created storage service in %s context with thread id %llu",
              ctx, thd->thread_id);
  /* Use variables from the current thd attached to client_service.
     This is because we need to be able to BF abort storage access
     operations. */
  wsrep_assign_from_threadvars(thd);
  return new Wsrep_storage_service(thd);
}

wsrep::storage_service* Wsrep_server_service::storage_service(
  wsrep::client_service& client_service)
{
  Wsrep_client_service& cs=
    static_cast<Wsrep_client_service&>(client_service);
  return wsrep_create_storage_service(cs.m_thd, "local");
}

wsrep::storage_service* Wsrep_server_service::storage_service(
  wsrep::high_priority_service& high_priority_service)
{
  Wsrep_high_priority_service& hps=
    static_cast<Wsrep_high_priority_service&>(high_priority_service);
  return wsrep_create_storage_service(hps.m_thd, "high priority");
}

void Wsrep_server_service::release_storage_service(
  wsrep::storage_service* storage_service)
{
  Wsrep_storage_service* ss=
    static_cast<Wsrep_storage_service*>(storage_service);
  THD* thd= ss->m_thd;
  wsrep_reset_threadvars(thd);
  server_threads.erase(thd);
  delete ss;
  delete thd;
}

Wsrep_applier_service*
wsrep_create_streaming_applier(THD *orig_thd, const char *ctx)
{
  /* Reset variables to allow creating new variables in thread local
     storage for new THD if needed. Note that reset must be done for
     current_thd, as orig_thd may not be in effect. This may be the case when
     streaming transaction is BF aborted and streaming applier
     is created from BF aborter context. */
  Wsrep_threadvars saved_threadvars(wsrep_save_threadvars());
  if (saved_threadvars.cur_thd)
    wsrep_reset_threadvars(saved_threadvars.cur_thd);
  THD *thd= 0;
  Wsrep_applier_service *ret= 0;
  if (!wsrep_create_threadvars() &&
      (thd= new THD(next_thread_id(), true)))
  {
    init_service_thd(thd, orig_thd->thread_stack);
    wsrep_assign_from_threadvars(thd);
    WSREP_DEBUG("Created streaming applier service in %s context with "
                "thread id %llu", ctx, thd->thread_id);
    if (!(ret= new (std::nothrow) Wsrep_applier_service(thd)))
    {
      delete thd;
    }
  }
  /* Restore original thread local storage state before returning. */
  wsrep_restore_threadvars(saved_threadvars);
  if (saved_threadvars.cur_thd)
    wsrep_store_threadvars(saved_threadvars.cur_thd);
  return ret;
}

wsrep::high_priority_service*
Wsrep_server_service::streaming_applier_service(
  wsrep::client_service& orig_client_service)
{
  Wsrep_client_service& orig_cs=
    static_cast<Wsrep_client_service&>(orig_client_service);
  return wsrep_create_streaming_applier(orig_cs.m_thd, "local");
}

wsrep::high_priority_service*
Wsrep_server_service::streaming_applier_service(
  wsrep::high_priority_service& orig_high_priority_service)
{
  Wsrep_high_priority_service&
    orig_hps(static_cast<Wsrep_high_priority_service&>(orig_high_priority_service));
  return wsrep_create_streaming_applier(orig_hps.m_thd, "high priority");
}

void Wsrep_server_service::release_high_priority_service(wsrep::high_priority_service* high_priority_service)
{
  Wsrep_high_priority_service* hps=
    static_cast<Wsrep_high_priority_service*>(high_priority_service);
  THD* thd= hps->m_thd;
  delete hps;
  wsrep_store_threadvars(thd);
  server_threads.erase(thd);
  delete thd;
  wsrep_delete_threadvars();
}

void Wsrep_server_service::background_rollback(wsrep::client_state& client_state)
{
  Wsrep_client_state& cs= static_cast<Wsrep_client_state&>(client_state);
  wsrep_fire_rollbacker(cs.thd());
}

void Wsrep_server_service::bootstrap()
{
  wsrep::log_info()
    << "Bootstrapping a new cluster, setting initial position to "
    << wsrep::gtid::undefined();
  wsrep_set_SE_checkpoint(wsrep::gtid::undefined(), wsrep_gtid_server.undefined());
}

void Wsrep_server_service::log_message(enum wsrep::log::level level,
                                       const char* message)
{
  switch (level)
  {
  case wsrep::log::debug:
    WSREP_DEBUG("%s", message);
    break;
  case wsrep::log::info:
    WSREP_INFO("%s", message);
    break;
  case wsrep::log::warning:
    WSREP_WARN("%s", message);
    break;
  case wsrep::log::error:
    WSREP_ERROR("%s", message);
    break;
  case wsrep::log::unknown:
    WSREP_UNKNOWN("%s", message);
    break;
  }
}

void Wsrep_server_service::log_view(
  wsrep::high_priority_service* high_priority_service,
  const wsrep::view& view)
{
  Wsrep_high_priority_service* applier=
    static_cast<Wsrep_high_priority_service*>(high_priority_service);
  /* Update global system variables */
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (wsrep_auto_increment_control && view.own_index() >= 0)
  {
    global_system_variables.auto_increment_offset= view.own_index() + 1;
    global_system_variables.auto_increment_increment= view.members().size();
    wsrep_protocol_version= view.protocol_version();
  }
  mysql_mutex_unlock(&LOCK_global_system_variables);

  /* Update wsrep status variables */
  mysql_mutex_lock(&LOCK_status);
  wsrep_cluster_size= view.members().size();
  wsrep_local_index= view.own_index();
  std::ostringstream os;
  os << view.state_id().id();
  wsrep_update_cluster_state_uuid(os.str().c_str());
  mysql_mutex_unlock(&LOCK_status);
  wsrep_config_state->set(view);
  wsrep_cluster_conf_id= view.view_seqno().get();

  if (view.status() == wsrep::view::primary)
  {
    if (applier)
    {
      Wsrep_id id;
      Wsrep_view prev_view= wsrep_schema->restore_view(applier->m_thd, id);
      bool checkpoint_was_reset= false;
      if (prev_view.state_id().id() != view.state_id().id())
      {
        WSREP_DEBUG("New cluster UUID was generated, resetting position info");
        wsrep_set_SE_checkpoint(wsrep::gtid::undefined(), wsrep_gtid_server.undefined());
        checkpoint_was_reset= true;
      }

      if (wsrep_debug)
      {
        std::ostringstream os;
        os << "Storing cluster view:\n" << view;
        WSREP_INFO("%s", os.str().c_str());
        DBUG_ASSERT(prev_view.state_id().id() != view.state_id().id() ||
                    view.state_id().seqno().get() >= prev_view.state_id().seqno().get());
      }

      if (trans_begin(applier->m_thd, MYSQL_START_TRANS_OPT_READ_WRITE))
      {
        WSREP_WARN("Failed to start transaction for store view");
      }
      else
      {
        if (wsrep_schema->store_view(applier->m_thd, view))
        {
          WSREP_WARN("Failed to store view");
          trans_rollback_stmt(applier->m_thd);
          if (!trans_rollback(applier->m_thd))
          {
            close_thread_tables(applier->m_thd);
          }
        }
        else
        {
          if (trans_commit(applier->m_thd))
          {
            WSREP_WARN("Failed to commit transaction for store view");
          }
        }
        applier->m_thd->release_transactional_locks();
      }

      /*
        Backwards compatibility: When running in mixed cluster with
        Galera 3.x, the provider does not generate unique sequence numbers
        for views. This condition can be checked by inspecting last
        committed as returned by the provider. If the last_committed
        matches to view state_id seqno, the cluster runs in backwards
        compatibility mode and we skip setting the checkpoint for
        view.
      */
      wsrep::seqno last_committed=
          Wsrep_server_state::instance().provider().last_committed_gtid().seqno();
      if (checkpoint_was_reset || last_committed != view.state_id().seqno())
      {
        wsrep_set_SE_checkpoint(view.state_id(), wsrep_gtid_server.gtid());
      }
      DBUG_ASSERT(wsrep_get_SE_checkpoint<wsrep::gtid>().id() == view.state_id().id());
    }
    else
    {
      WSREP_DEBUG("No applier in Wsrep_server_service::log_view(), "
                  "skipping write to wsrep_schema");
    }
  }
}

void Wsrep_server_service::recover_streaming_appliers(wsrep::client_service& cs)
{
  Wsrep_client_service& client_service= static_cast<Wsrep_client_service&>(cs);
  wsrep_recover_sr_from_storage(client_service.m_thd);
}

void Wsrep_server_service::recover_streaming_appliers(
  wsrep::high_priority_service& hs)
{
  Wsrep_high_priority_service& high_priority_service=
    static_cast<Wsrep_high_priority_service&>(hs);
  wsrep_recover_sr_from_storage(high_priority_service.m_thd);
}

wsrep::view Wsrep_server_service::get_view(wsrep::client_service& c,
                                           const wsrep::id& own_id)
{
  Wsrep_client_service& cs(static_cast<Wsrep_client_service&>(c));
  wsrep::view v(wsrep_schema->restore_view(cs.m_thd, own_id));
  return v;
}

wsrep::gtid Wsrep_server_service::get_position(wsrep::client_service&)
{
  return wsrep_get_SE_checkpoint<wsrep::gtid>();
}

void Wsrep_server_service::set_position(wsrep::client_service& c WSREP_UNUSED,
                                        const wsrep::gtid& gtid)
{
  Wsrep_client_service& cs WSREP_UNUSED (static_cast<Wsrep_client_service&>(c));
  DBUG_ASSERT(cs.m_client_state.transaction().state()
              == wsrep::transaction::s_aborted);
  // Wait until all prior committers have finished.
  wsrep::gtid wait_for(gtid.id(),
                       wsrep::seqno(gtid.seqno().get() - 1));
  if (auto err = Wsrep_server_state::instance().provider()
      .wait_for_gtid(wait_for, std::numeric_limits<int>::max()))
  {
    WSREP_WARN("Wait for gtid returned error %d while waiting for "
               "prior transactions to commit before setting position", err);
  }
  wsrep_set_SE_checkpoint(gtid, wsrep_gtid_server.gtid());
}

void Wsrep_server_service::log_state_change(
  enum Wsrep_server_state::state prev_state,
  enum Wsrep_server_state::state current_state)
{
  WSREP_INFO("Server status change %s -> %s",
             wsrep::to_c_string(prev_state),
             wsrep::to_c_string(current_state));
  mysql_mutex_lock(&LOCK_status);
  switch (current_state)
  {
  case Wsrep_server_state::s_synced:
    wsrep_ready= TRUE;
    WSREP_INFO("Synchronized with group, ready for connections");
    /* fall through */
  case Wsrep_server_state::s_joined:
  case Wsrep_server_state::s_donor:
    wsrep_cluster_status= "Primary";
    break;
  case Wsrep_server_state::s_connected:
    wsrep_cluster_status= "non-Primary";
    wsrep_ready= FALSE;
    wsrep_connected= TRUE;
    break;
  case Wsrep_server_state::s_disconnected:
    wsrep_ready= FALSE;
    wsrep_connected= FALSE;
    wsrep_cluster_status= "Disconnected";
    break;
  default:
    wsrep_ready= FALSE;
    wsrep_cluster_status= "non-Primary";
    break;
  }
  mysql_mutex_unlock(&LOCK_status);
  wsrep_config_state->set(current_state);
}

bool Wsrep_server_service::sst_before_init() const
{
  return wsrep_before_SE();
}

std::string Wsrep_server_service::sst_request()
{
  return wsrep_sst_prepare();
}

int Wsrep_server_service::start_sst(const std::string& sst_request,
                                    const wsrep::gtid& gtid,
                                    bool bypass)
{
  return wsrep_sst_donate(sst_request, gtid, bypass);
}

int Wsrep_server_service::wait_committing_transactions(int timeout)
{
  return wsrep_wait_committing_connections_close(timeout);
}

void Wsrep_server_service::debug_sync(const char* sync_point)
{
  DBUG_EXECUTE_IF(sync_point, {
      std::stringstream dbug_action;
      dbug_action << "now "
                  << "SIGNAL " << sync_point << "_reached "
                  << "WAIT_FOR " << sync_point << "_continue";
      const std::string& action(dbug_action.str());
      DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                         action.c_str(),
                                         action.length()));
    };);
}
