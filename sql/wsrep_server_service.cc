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

#include "log.h" /* sql_print_xxx() */
#include "sql_class.h" /* system variables */
#include "transaction.h" /* trans_xxx */
#include "sql_base.h" /* close_thread_tables */

static void init_service_thd(THD* thd, char* thread_stack)
{
  thd->thread_stack= thread_stack;
  thd->real_id= pthread_self();
  thd->prior_thr_create_utime= thd->start_utime= microsecond_interval_timer();
  thd->set_command(COM_SLEEP);
  thd->reset_for_next_command(true);
}

wsrep::storage_service* Wsrep_server_service::storage_service(
  wsrep::client_service& client_service)
{
  Wsrep_client_service& cs=
    static_cast<Wsrep_client_service&>(client_service);
  THD* thd= new THD(next_thread_id(), true, true);
  init_service_thd(thd, cs.m_thd->thread_stack);
  WSREP_DEBUG("Created storage service with thread id %llu",
              thd->thread_id);
  return new Wsrep_storage_service(thd);
}

wsrep::storage_service* Wsrep_server_service::storage_service(
  wsrep::high_priority_service& high_priority_service)
{
  Wsrep_high_priority_service& hps=
    static_cast<Wsrep_high_priority_service&>(high_priority_service);
  THD* thd= new THD(next_thread_id(), true, true);
  init_service_thd(thd, hps.m_thd->thread_stack);
  WSREP_DEBUG("Created high priority storage service with thread id %llu",
              thd->thread_id);
  return new Wsrep_storage_service(thd);
}

void Wsrep_server_service::release_storage_service(
  wsrep::storage_service* storage_service)
{
  Wsrep_storage_service* ss=
    static_cast<Wsrep_storage_service*>(storage_service);
  THD* thd= ss->m_thd;
  delete ss;
  delete thd;
}

wsrep::high_priority_service*
Wsrep_server_service::streaming_applier_service(
  wsrep::client_service& orig_client_service)
{
  Wsrep_client_service& orig_cs=
    static_cast<Wsrep_client_service&>(orig_client_service);
  THD* thd= new THD(next_thread_id(), true, true);
  init_service_thd(thd, orig_cs.m_thd->thread_stack);
  WSREP_DEBUG("Created streaming applier service in local context with "
              "thread id %llu", thd->thread_id);
  return new Wsrep_applier_service(thd);
}

wsrep::high_priority_service*
Wsrep_server_service::streaming_applier_service(
  wsrep::high_priority_service& orig_high_priority_service)
{
  Wsrep_high_priority_service&
    orig_hps(static_cast<Wsrep_high_priority_service&>(orig_high_priority_service));
  THD* thd= new THD(next_thread_id(), true, true);
  init_service_thd(thd, orig_hps.m_thd->thread_stack);
  WSREP_DEBUG("Created streaming applier service in high priority "
              "context with thread id %llu", thd->thread_id);
  return new Wsrep_applier_service(thd);
}

void Wsrep_server_service::release_high_priority_service(wsrep::high_priority_service* high_priority_service)
{
  Wsrep_high_priority_service* hps=
    static_cast<Wsrep_high_priority_service*>(high_priority_service);
  THD* thd= hps->m_thd;
  delete hps;
  delete thd;
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
  wsrep_set_SE_checkpoint(wsrep::gtid::undefined());
}

void Wsrep_server_service::log_message(enum wsrep::log::level level,
                                       const char* message)
{
  switch (level)
  {
  case wsrep::log::debug:
    sql_print_information("debug: %s", message);
    break;
  case wsrep::log::info:
    sql_print_information("%s", message);
    break;
  case wsrep::log::warning:
    sql_print_warning("%s", message);
    break;
  case wsrep::log::error:
    sql_print_error("%s", message);
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

  if (view.status() == wsrep::view::primary)
  {
    if (applier)
    {
      Wsrep_id id;
      Wsrep_view prev_view= wsrep_schema->restore_view(applier->m_thd, id);
      if (prev_view.state_id().id() != view.state_id().id())
      {
        WSREP_DEBUG("New cluster UUID was generated, resetting position info");
        wsrep_set_SE_checkpoint(wsrep::gtid::undefined());
      }

      if (wsrep_debug)
      {
        std::ostringstream os;
        os << "Storing cluster view:\n" << view;
        WSREP_INFO("%s", os.str().c_str());
        DBUG_ASSERT(prev_view.state_id().id() != view.state_id().id() ||
                    view.state_id().seqno() > prev_view.state_id().seqno());
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
        applier->m_thd->mdl_context.release_transactional_locks();
      }

      wsrep_set_SE_checkpoint(view.state_id());
      DBUG_ASSERT(wsrep_get_SE_checkpoint().id() == view.state_id().id());
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
  return wsrep_get_SE_checkpoint();
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

void Wsrep_server_service::debug_sync(const char*)
{
}

int Wsrep_server_service::do_crypt(void**                ctx,
                                   wsrep::const_buffer&  key,
                                   const char            (*iv)[32],
                                   wsrep::const_buffer&  input,
                                   void*                 output,
                                   bool                  encrypt,
                                   bool                  last)
{

  const void* deserialized_key_ptr;
  size_t deserialized_key_size;
  unsigned int key_version;
  if(wsrep_key_deserialize(key.data(), key.size(), deserialized_key_ptr,
                           deserialized_key_size, key_version))
  {
     throw wsrep::runtime_error("Failed wsrep_key_deserialize()");
  }
  if (*ctx == NULL)
  {
    int mode= encrypt ? ENCRYPTION_FLAG_ENCRYPT : ENCRYPTION_FLAG_DECRYPT;
    *ctx = ::malloc(encryption_ctx_size(ENCRYPTION_KEY_SYSTEM_DATA,
                                        key_version));
    if (*ctx == NULL)
    {
      throw wsrep::runtime_error("Memory not allocated in do_crypt()");
    }
    if(encryption_ctx_init(*ctx,
                           (const unsigned char*)deserialized_key_ptr,
                           deserialized_key_size,
                           (unsigned char*)iv, MY_AES_BLOCK_SIZE,
                           mode | ENCRYPTION_FLAG_NOPAD,
                           ENCRYPTION_KEY_SYSTEM_DATA,
                           key_version))
    {
      throw wsrep::runtime_error("Failed encryption_ctx_init()");
    }
  }
  unsigned int ctx_update_size= 0;
  if(encryption_ctx_update(*ctx, (const unsigned char*)input.data(),
                           input.size(),
                           (unsigned char *)output, &ctx_update_size))
  {
    throw wsrep::runtime_error("Failed encryption_ctx_update()");
  }
  unsigned int ctx_finish_size= 0;
  if (last)
  {
    if(encryption_ctx_finish(*ctx, (unsigned char *)output + ctx_update_size,
                              &ctx_finish_size))
    {
      throw wsrep::runtime_error("Failed encryption_ctx_finish()");
    }
    assert(ctx_update_size + ctx_finish_size == input.size());
    free(*ctx);
  }

  return ctx_update_size + ctx_finish_size;
}
