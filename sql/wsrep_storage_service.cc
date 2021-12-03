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
#include "wsrep_storage_service.h"
#include "wsrep_trans_observer.h" /* wsrep_open() */
#include "wsrep_schema.h"
#include "wsrep_binlog.h"

#include "sql_class.h"
#include "mysqld.h" /* next_query_id() */
#include "slave.h" /* opt_log_slave_updates() */
#include "transaction.h" /* trans_commit(), trans_rollback() */

/*
  Temporarily enable wsrep on thd
 */
class Wsrep_on
{
public:
  Wsrep_on(THD* thd)
    : m_thd(thd)
    , m_wsrep_on(thd->variables.wsrep_on)
  {
    thd->variables.wsrep_on= TRUE;
  }
  ~Wsrep_on()
  {
    m_thd->variables.wsrep_on= m_wsrep_on;
  }
private:
  THD* m_thd;
  my_bool m_wsrep_on;
};

Wsrep_storage_service::Wsrep_storage_service(THD* thd)
  : wsrep::storage_service()
  , wsrep::high_priority_context(thd->wsrep_cs())
  , m_thd(thd)
{
  thd->security_ctx->skip_grants();
  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;

  /* No binlogging */

  /* No general log */
  thd->variables.option_bits |= OPTION_LOG_OFF;

  /* Read committed isolation to avoid gap locking */
  thd->variables.tx_isolation = ISO_READ_COMMITTED;

  /* Keep wsrep on to enter commit ordering hooks */
  thd->variables.wsrep_on= 1;
  thd->wsrep_skip_locking= true;

  wsrep_open(thd);
  wsrep_before_command(thd);
}

Wsrep_storage_service::~Wsrep_storage_service()
{
  wsrep_after_command_ignore_result(m_thd);
  wsrep_close(m_thd);
  m_thd->wsrep_skip_locking= false;
}

int Wsrep_storage_service::start_transaction(const wsrep::ws_handle& ws_handle)
{
  DBUG_ENTER("Wsrep_storage_service::start_transaction");
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_PRINT("info", ("Wsrep_storage_service::start_transcation(%llu, %p)",
                      m_thd->thread_id, m_thd));
  m_thd->set_wsrep_next_trx_id(ws_handle.transaction_id().get());
  DBUG_RETURN(m_thd->wsrep_cs().start_transaction(
                wsrep::transaction_id(m_thd->wsrep_next_trx_id())) ||
              trans_begin(m_thd, MYSQL_START_TRANS_OPT_READ_WRITE));
}

void Wsrep_storage_service::adopt_transaction(const wsrep::transaction& transaction)
{
  DBUG_ENTER("Wsrep_Storage_server::adopt_transaction");
  DBUG_ASSERT(m_thd == current_thd);
  m_thd->wsrep_cs().adopt_transaction(transaction);
  trans_begin(m_thd, MYSQL_START_TRANS_OPT_READ_WRITE);
  DBUG_VOID_RETURN;
}

int Wsrep_storage_service::append_fragment(const wsrep::id& server_id,
                                           wsrep::transaction_id transaction_id,
                                           int flags,
                                           const wsrep::const_buffer& data,
                                           const wsrep::xid& xid WSREP_UNUSED)
{
  DBUG_ENTER("Wsrep_storage_service::append_fragment");
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_PRINT("info", ("Wsrep_storage_service::append_fragment(%llu, %p)",
                      m_thd->thread_id, m_thd));
  int ret= wsrep_schema->append_fragment(m_thd,
                                         server_id,
                                         transaction_id,
                                         wsrep::seqno(-1),
                                         flags,
                                         data);
  DBUG_RETURN(ret);
}

int Wsrep_storage_service::update_fragment_meta(const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_storage_service::update_fragment_meta");
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_PRINT("info", ("Wsrep_storage_service::update_fragment_meta(%llu, %p)",
                      m_thd->thread_id, m_thd));
  int ret= wsrep_schema->update_fragment_meta(m_thd, ws_meta);
  DBUG_RETURN(ret);
}

int Wsrep_storage_service::remove_fragments()
{
  DBUG_ENTER("Wsrep_storage_service::remove_fragments");
  DBUG_ASSERT(m_thd == current_thd);

  int ret= wsrep_schema->remove_fragments(m_thd,
                                          m_thd->wsrep_trx().server_id(),
                                          m_thd->wsrep_trx().id(),
                                          m_thd->wsrep_sr().fragments());
  DBUG_RETURN(ret);
}

int Wsrep_storage_service::commit(const wsrep::ws_handle& ws_handle,
                                  const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_storage_service::commit");
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_PRINT("info", ("Wsrep_storage_service::commit(%llu, %p)",
                      m_thd->thread_id, m_thd));
  WSREP_DEBUG("Storage service commit: %llu, %lld",
              ws_meta.transaction_id().get(), ws_meta.seqno().get());
  int ret= 0;
  const bool is_ordered= !ws_meta.seqno().is_undefined();

  if (is_ordered)
  {
    ret= m_thd->wsrep_cs().prepare_for_ordering(ws_handle, ws_meta, true);
  }

  ret= ret || trans_commit(m_thd);

  if (!is_ordered)
  {
    /* Wsrep commit was not ordered so it does not go through commit time
       hooks and remains active. Roll it back to make cleanup happen
       in after_applying() call. */
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }
  else if (ret)
  {
    /* Commit failed, this probably means that the parent SR transaction
       was BF aborted. Roll back out of order, the parent
       transaction will release commit order after it has rolled back. */
    m_thd->wsrep_cs().prepare_for_ordering(wsrep::ws_handle(),
                                           wsrep::ws_meta(),
                                           false);
    trans_rollback(m_thd);
  }
  m_thd->wsrep_cs().after_applying();
  m_thd->release_transactional_locks();
  DBUG_RETURN(ret);
}

int Wsrep_storage_service::rollback(const wsrep::ws_handle& ws_handle,
                                    const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_storage_service::rollback");
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_PRINT("info", ("Wsrep_storage_service::rollback(%llu, %p)",
                      m_thd->thread_id, m_thd));
  int ret= (m_thd->wsrep_cs().prepare_for_ordering(
            ws_handle, ws_meta, false) ||
            trans_rollback(m_thd));
  m_thd->wsrep_cs().after_applying();
  m_thd->release_transactional_locks();
  DBUG_RETURN(ret);
}

void Wsrep_storage_service::store_globals()
{
  wsrep_store_threadvars(m_thd);
}

void Wsrep_storage_service::reset_globals()
{
  wsrep_reset_threadvars(m_thd);
}
