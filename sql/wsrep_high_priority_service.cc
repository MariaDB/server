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

#include "wsrep_high_priority_service.h"
#include "wsrep_applier.h"
#include "wsrep_binlog.h"
#include "wsrep_schema.h"
#include "wsrep_xid.h"
#include "wsrep_trans_observer.h"

#include "sql_class.h" /* THD */
#include "transaction.h"
#include "debug_sync.h"
/* RLI */
#include "rpl_rli.h"
#define NUMBER_OF_FIELDS_TO_IDENTIFY_COORDINATOR 1
#define NUMBER_OF_FIELDS_TO_IDENTIFY_WORKER 2
#include "slave.h"
#include "rpl_mi.h"

namespace
{
/*
  Scoped mode for applying non-transactional write sets (TOI)
 */
class Wsrep_non_trans_mode
{
public:
  Wsrep_non_trans_mode(THD* thd, const wsrep::ws_meta& ws_meta)
    : m_thd(thd)
    , m_option_bits(thd->variables.option_bits)
    , m_server_status(thd->server_status)
  {
    m_thd->variables.option_bits&= ~OPTION_BEGIN;
    m_thd->server_status&= ~SERVER_STATUS_IN_TRANS;
    m_thd->wsrep_cs().enter_toi(ws_meta);
  }
  ~Wsrep_non_trans_mode()
  {
    m_thd->variables.option_bits= m_option_bits;
    m_thd->server_status= m_server_status;
    m_thd->wsrep_cs().leave_toi();
  }
private:
  Wsrep_non_trans_mode(const Wsrep_non_trans_mode&);
  Wsrep_non_trans_mode& operator=(const Wsrep_non_trans_mode&);
  THD* m_thd;
  ulonglong m_option_bits;
  uint m_server_status;
};
}

static rpl_group_info* wsrep_relay_group_init(THD* thd, const char* log_fname)
{
  Relay_log_info* rli= new Relay_log_info(false);

  if (!rli->relay_log.description_event_for_exec)
  {
    rli->relay_log.description_event_for_exec=
      new Format_description_log_event(4);
  }

  static LEX_CSTRING connection_name= { STRING_WITH_LEN("wsrep") };

  /*
    Master_info's constructor initializes rpl_filter by either an already
    constructed Rpl_filter object from global 'rpl_filters' list if the
    specified connection name is same, or it constructs a new Rpl_filter
    object and adds it to rpl_filters. This object is later destructed by
    Mater_info's destructor by looking it up based on connection name in
    rpl_filters list.

    However, since all Master_info objects created here would share same
    connection name ("wsrep"), destruction of any of the existing Master_info
    objects (in wsrep_return_from_bf_mode()) would free rpl_filter referenced
    by any/all existing Master_info objects.

    In order to avoid that, we have added a check in Master_info's destructor
    to not free the "wsrep" rpl_filter. It will eventually be freed by
    free_all_rpl_filters() when server terminates.
  */
  rli->mi= new Master_info(&connection_name, false);

  struct rpl_group_info *rgi= new rpl_group_info(rli);
  rgi->thd= rli->sql_driver_thd= thd;

  if ((rgi->deferred_events_collecting= rli->mi->rpl_filter->is_on()))
  {
    rgi->deferred_events= new Deferred_log_events(rli);
  }

  return rgi;
}

static void wsrep_setup_uk_and_fk_checks(THD* thd)
{
  /* Tune FK and UK checking policy. These are reset back to original
     in Wsrep_high_priority_service destructor. */
  if (wsrep_slave_UK_checks == FALSE)
    thd->variables.option_bits|= OPTION_RELAXED_UNIQUE_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_RELAXED_UNIQUE_CHECKS;

  if (wsrep_slave_FK_checks == FALSE)
    thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_NO_FOREIGN_KEY_CHECKS;
}

/****************************************************************************
                         High priority service
*****************************************************************************/

Wsrep_high_priority_service::Wsrep_high_priority_service(THD* thd)
  : wsrep::high_priority_service(Wsrep_server_state::instance())
  , wsrep::high_priority_context(thd->wsrep_cs())
  , m_thd(thd)
  , m_rli()
{
  LEX_CSTRING db_str= { NULL, 0 };
  m_shadow.option_bits  = thd->variables.option_bits;
  m_shadow.server_status= thd->server_status;
  m_shadow.vio          = thd->net.vio;
  m_shadow.tx_isolation = thd->variables.tx_isolation;
  m_shadow.db           = (char *)thd->db.str;
  m_shadow.db_length    = thd->db.length;
  m_shadow.user_time    = thd->user_time;
  m_shadow.row_count_func= thd->get_row_count_func();
  m_shadow.wsrep_applier= thd->wsrep_applier;

  /* Disable general logging on applier threads */
  thd->variables.option_bits |= OPTION_LOG_OFF;
  /* Enable binlogging if opt_log_slave_updates is set */
  if (opt_log_slave_updates)
    thd->variables.option_bits|= OPTION_BIN_LOG;
  else
    thd->variables.option_bits&= ~(OPTION_BIN_LOG);

  thd->net.vio= 0;
  thd->reset_db(&db_str);
  thd->clear_error();
  thd->variables.tx_isolation= ISO_READ_COMMITTED;
  thd->tx_isolation          = ISO_READ_COMMITTED;

  /* From trans_begin() */
  thd->variables.option_bits|= OPTION_BEGIN;
  thd->server_status|= SERVER_STATUS_IN_TRANS;

  /* Make THD wsrep_applier so that it cannot be killed */
  thd->wsrep_applier= true;

  if (!thd->wsrep_rgi) thd->wsrep_rgi= wsrep_relay_group_init(thd, "wsrep_relay");

  m_rgi= thd->wsrep_rgi;
  m_rgi->thd= thd;
  m_rli= m_rgi->rli;
  thd_proc_info(thd, "wsrep applier idle");
}

Wsrep_high_priority_service::~Wsrep_high_priority_service()
{
  THD* thd= m_thd;
  thd->variables.option_bits = m_shadow.option_bits;
  thd->server_status         = m_shadow.server_status;
  thd->net.vio               = m_shadow.vio;
  thd->variables.tx_isolation= m_shadow.tx_isolation;
  LEX_CSTRING db_str= { m_shadow.db, m_shadow.db_length };
  thd->reset_db(&db_str);
  thd->user_time             = m_shadow.user_time;
  
  if (thd->wsrep_rgi && thd->wsrep_rgi->rli)
    delete thd->wsrep_rgi->rli->mi;
  if (thd->wsrep_rgi)
    delete thd->wsrep_rgi->rli;
  delete thd->wsrep_rgi;
  thd->wsrep_rgi= NULL;
  
  thd->set_row_count_func(m_shadow.row_count_func);
  thd->wsrep_applier         = m_shadow.wsrep_applier;
}

int Wsrep_high_priority_service::start_transaction(
  const wsrep::ws_handle& ws_handle, const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER(" Wsrep_high_priority_service::start_transaction");
  DBUG_RETURN(m_thd->wsrep_cs().start_transaction(ws_handle, ws_meta) ||
              trans_begin(m_thd));
}

const wsrep::transaction& Wsrep_high_priority_service::transaction() const
{
  DBUG_ENTER(" Wsrep_high_priority_service::transaction");
  DBUG_RETURN(m_thd->wsrep_trx());
}

int Wsrep_high_priority_service::adopt_transaction(
  const wsrep::transaction& transaction)
{
  DBUG_ENTER(" Wsrep_high_priority_service::adopt_transaction");
  /* Adopt transaction first to set up transaction meta data for
     trans begin. If trans_begin() fails for some reason, roll back
     the wsrep transaction before return. */
  m_thd->wsrep_cs().adopt_transaction(transaction);
  int ret= trans_begin(m_thd);
  if (ret)
  {
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::append_fragment_and_commit(
  const wsrep::ws_handle& ws_handle,
  const wsrep::ws_meta& ws_meta,
  const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_high_priority_service::append_fragment_and_commit");
  int ret= start_transaction(ws_handle, ws_meta);
  /*
    Start transaction explicitly to avoid early commit via
    trans_commit_stmt() in append_fragment()
  */
  ret= ret || trans_begin(m_thd);
  ret= ret || wsrep_schema->append_fragment(m_thd,
                                            ws_meta.server_id(),
                                            ws_meta.transaction_id(),
                                            ws_meta.seqno(),
                                            ws_meta.flags(),
                                            data);

  /*
    Note: The commit code below seems to be identical to
    Wsrep_storage_service::commit(). Consider implementing
    common utility function to deal with commit.
   */
  const bool do_binlog_commit= (opt_log_slave_updates &&
				wsrep_gtid_mode       &&
				m_thd->variables.gtid_seq_no);
   /*
    Write skip event into binlog if gtid_mode is on. This is to
    maintain gtid continuity.
  */
  if (do_binlog_commit)
  {
    ret= wsrep_write_skip_event(m_thd);
  }

  if (!ret)
  {
    ret= m_thd->wsrep_cs().prepare_for_ordering(ws_handle,
                                                ws_meta, true);
  }

  ret= ret || trans_commit(m_thd);

  m_thd->wsrep_cs().after_applying();
  m_thd->mdl_context.release_transactional_locks();

  thd_proc_info(m_thd, "wsrep applier committed");

  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::remove_fragments(const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::remove_fragments");
  int ret= wsrep_schema->remove_fragments(m_thd,
                                          ws_meta.server_id(),
                                          ws_meta.transaction_id(),
                                          m_thd->wsrep_sr().fragments());
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::commit(const wsrep::ws_handle& ws_handle,
                                        const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::commit");
  THD* thd= m_thd;
  DBUG_ASSERT(thd->wsrep_trx().active());
  thd->wsrep_cs().prepare_for_ordering(ws_handle, ws_meta, true);
  thd_proc_info(thd, "committing");

  const bool is_ordered= !ws_meta.seqno().is_undefined();
  int ret= trans_commit(thd);

  if (ret == 0)
  {
    m_rgi->cleanup_context(thd, 0);
  }

  m_thd->mdl_context.release_transactional_locks();

  thd_proc_info(thd, "wsrep applier committed");

  if (!is_ordered)
  {
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }
  else if (m_thd->wsrep_trx().state() == wsrep::transaction::s_executing)
  {
    /*
      Wsrep commit was ordered but it did not go through commit time
      hooks and remains active. Cycle through commit hooks to release
      commit order and to make cleanup happen in after_applying() call.

      This is a workaround for CTAS with empty result set.
    */
    WSREP_DEBUG("Commit not finished for applier %llu", thd->thread_id);
    ret= ret || m_thd->wsrep_cs().before_commit() ||
      m_thd->wsrep_cs().ordered_commit() ||
      m_thd->wsrep_cs().after_commit();
  }

  thd->lex->sql_command= SQLCOM_END;

  must_exit_= check_exit_status();
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::rollback(const wsrep::ws_handle& ws_handle,
                                          const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::rollback");
  m_thd->wsrep_cs().prepare_for_ordering(ws_handle, ws_meta, false);
  int ret= (trans_rollback_stmt(m_thd) || trans_rollback(m_thd));
  m_thd->mdl_context.release_transactional_locks();
  m_thd->mdl_context.release_explicit_locks();
  DBUG_RETURN(ret);
}

int Wsrep_high_priority_service::apply_toi(const wsrep::ws_meta& ws_meta,
                                           const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_high_priority_service::apply_toi");
  THD* thd= m_thd;
  Wsrep_non_trans_mode non_trans_mode(thd, ws_meta);

  wsrep::client_state& client_state(thd->wsrep_cs());
  DBUG_ASSERT(client_state.in_toi());

  thd_proc_info(thd, "wsrep applier toi");

  WSREP_DEBUG("Wsrep_high_priority_service::apply_toi: %lld",
              client_state.toi_meta().seqno().get());

  int ret= wsrep_apply_events(thd, m_rli, data.data(), data.size());
  if (ret != 0 || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf_with_header(thd, data.data(), data.size());
    thd->wsrep_has_ignored_error= false;
    /* todo: error voting */
  }
  trans_commit(thd);

  thd->close_temporary_tables();
  thd->lex->sql_command= SQLCOM_END;

  wsrep_set_SE_checkpoint(client_state.toi_meta().gtid());

  must_exit_= check_exit_status();

  DBUG_RETURN(ret);
}

void Wsrep_high_priority_service::store_globals()
{
  DBUG_ENTER("Wsrep_high_priority_service::store_globals");
  /* In addition to calling THD::store_globals(), call
     wsrep::client_state::store_globals() to gain ownership of
     the client state */
  m_thd->store_globals();
  m_thd->wsrep_cs().store_globals();
  DBUG_VOID_RETURN;
}

void Wsrep_high_priority_service::reset_globals()
{
  DBUG_ENTER("Wsrep_high_priority_service::reset_globals");
  m_thd->reset_globals();
  DBUG_VOID_RETURN;
}

void Wsrep_high_priority_service::switch_execution_context(wsrep::high_priority_service& orig_high_priority_service)
{
  DBUG_ENTER("Wsrep_high_priority_service::switch_execution_context");
  Wsrep_high_priority_service&
    orig_hps= static_cast<Wsrep_high_priority_service&>(orig_high_priority_service);
  m_thd->thread_stack= orig_hps.m_thd->thread_stack;
  DBUG_VOID_RETURN;
}

int Wsrep_high_priority_service::log_dummy_write_set(const wsrep::ws_handle& ws_handle,
                                                     const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_high_priority_service::log_dummy_write_set");
  int ret= 0;
  DBUG_PRINT("info",
             ("Wsrep_high_priority_service::log_dummy_write_set: seqno=%lld",
              ws_meta.seqno().get()));
  m_thd->wsrep_cs().start_transaction(ws_handle, ws_meta);
  WSREP_DEBUG("Log dummy write set %lld", ws_meta.seqno().get());
  if (!(opt_log_slave_updates && wsrep_gtid_mode && m_thd->variables.gtid_seq_no))
  {
    m_thd->wsrep_cs().before_rollback();
    m_thd->wsrep_cs().after_rollback();
  }
  m_thd->wsrep_cs().after_applying();
  DBUG_RETURN(ret);
}

void Wsrep_high_priority_service::debug_crash(const char* crash_point)
{
  DBUG_ASSERT(m_thd == current_thd);
  DBUG_EXECUTE_IF(crash_point, DBUG_SUICIDE(););
}

/****************************************************************************
                           Applier service
*****************************************************************************/

Wsrep_applier_service::Wsrep_applier_service(THD* thd)
  : Wsrep_high_priority_service(thd)
{
  thd->wsrep_applier_service= this;
  thd->wsrep_cs().open(wsrep::client_id(thd->thread_id));
  thd->wsrep_cs().before_command();
  thd->wsrep_cs().debug_log_level(wsrep_debug);

}

Wsrep_applier_service::~Wsrep_applier_service()
{
  m_thd->wsrep_cs().after_command_before_result();
  m_thd->wsrep_cs().after_command_after_result();
  m_thd->wsrep_cs().close();
  m_thd->wsrep_cs().cleanup();
  m_thd->wsrep_applier_service= NULL;
}

int Wsrep_applier_service::apply_write_set(const wsrep::ws_meta& ws_meta,
                                           const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_applier_service::apply_write_set");
  THD* thd= m_thd;

  thd->variables.option_bits |= OPTION_BEGIN;
  thd->variables.option_bits |= OPTION_NOT_AUTOCOMMIT;
  DBUG_ASSERT(thd->wsrep_trx().active());
  DBUG_ASSERT(thd->wsrep_trx().state() == wsrep::transaction::s_executing);

  thd_proc_info(thd, "applying write set");
  /* moved dbug sync point here, after possible THD switch for SR transactions
     has ben done
  */
  /* Allow tests to block the applier thread using the DBUG facilities */
  DBUG_EXECUTE_IF("sync.wsrep_apply_cb",
                 {
                   const char act[]=
                     "now "
                     "SIGNAL sync.wsrep_apply_cb_reached "
                     "WAIT_FOR signal.wsrep_apply_cb";
                   DBUG_ASSERT(!debug_sync_set_action(thd,
                                                      STRING_WITH_LEN(act)));
                 };);

  wsrep_setup_uk_and_fk_checks(thd);

  int ret= wsrep_apply_events(thd, m_rli, data.data(), data.size());

  if (ret || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf_with_header(thd, data.data(), data.size());
  }

  thd->close_temporary_tables();
  if (!ret && !(ws_meta.flags() & wsrep::provider::flag::commit))
  {
    thd->wsrep_cs().fragment_applied(ws_meta.seqno());
  }
  thd_proc_info(thd, "wsrep applied write set");
  DBUG_RETURN(ret);
}

void Wsrep_applier_service::after_apply()
{
  DBUG_ENTER("Wsrep_applier_service::after_apply");
  wsrep_after_apply(m_thd);
  DBUG_VOID_RETURN;
}

bool Wsrep_applier_service::check_exit_status() const
{
  bool ret= false;
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  if (wsrep_slave_count_change < 0)
  {
    ++wsrep_slave_count_change;
    ret= true;
  }
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  return ret;
}

/****************************************************************************
                           Replayer service
*****************************************************************************/

Wsrep_replayer_service::Wsrep_replayer_service(THD* thd)
  : Wsrep_high_priority_service(thd)
  , m_da_shadow()
  , m_replay_status()
{
  /* Response must not have been sent to client */
  DBUG_ASSERT(!thd->get_stmt_da()->is_sent());
  /* PS reprepare observer should have been removed already
     open_table() will fail if we have dangling observer here */
  DBUG_ASSERT(!thd->m_reprepare_observer);
  /* Replaying should happen always from after_statement() hook
     after rollback, which should guarantee that there are no
     transactional locks */
  DBUG_ASSERT(!thd->mdl_context.has_transactional_locks());

  /* Make a shadow copy of diagnostics area and reset */
  m_da_shadow.status= thd->get_stmt_da()->status();
  if (m_da_shadow.status == Diagnostics_area::DA_OK)
  {
    m_da_shadow.affected_rows= thd->get_stmt_da()->affected_rows();
    m_da_shadow.last_insert_id= thd->get_stmt_da()->last_insert_id();
    strmake(m_da_shadow.message, thd->get_stmt_da()->message(),
            sizeof(m_da_shadow.message) - 1);
  }
  thd->get_stmt_da()->reset_diagnostics_area();

  /* Release explicit locks */
  if (thd->locked_tables_mode && thd->lock)
  {
    WSREP_WARN("releasing table lock for replaying (%llu)",
               thd->thread_id);
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
  }

  /*
    Replaying will call MYSQL_START_STATEMENT when handling
    BEGIN Query_log_event so end statement must be called before
    replaying.
  */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;
  thd_proc_info(thd, "wsrep replaying trx");
}

Wsrep_replayer_service::~Wsrep_replayer_service()
{
  THD* thd= m_thd;
  DBUG_ASSERT(!thd->get_stmt_da()->is_sent());
  DBUG_ASSERT(!thd->get_stmt_da()->is_set());
  if (m_replay_status == wsrep::provider::success)
  {
    DBUG_ASSERT(thd->wsrep_cs().current_error() == wsrep::e_success);
    thd->killed= NOT_KILLED;
    if (m_da_shadow.status == Diagnostics_area::DA_OK)
    {
      my_ok(thd,
            m_da_shadow.affected_rows,
            m_da_shadow.last_insert_id,
            m_da_shadow.message);
    }
    else
    {
      my_ok(thd);
    }
  }
  else if (m_replay_status == wsrep::provider::error_certification_failed)
  {
    wsrep_override_error(thd, ER_LOCK_DEADLOCK);
  }
  else
  {
    DBUG_ASSERT(0);
    WSREP_ERROR("trx_replay failed for: %d, schema: %s, query: %s",
                m_replay_status,
                thd->db.str, WSREP_QUERY(thd));
    unireg_abort(1);
  }
}

int Wsrep_replayer_service::apply_write_set(const wsrep::ws_meta& ws_meta,
                                                 const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_replayer_service::apply_write_set");
  THD* thd= m_thd;

  DBUG_ASSERT(thd->wsrep_trx().active());
  DBUG_ASSERT(thd->wsrep_trx().state() == wsrep::transaction::s_replaying);

  wsrep_setup_uk_and_fk_checks(thd);

  int ret= 0;
  if (!wsrep::starts_transaction(ws_meta.flags()))
  {
    DBUG_ASSERT(thd->wsrep_trx().is_streaming());
    ret= wsrep_schema->replay_transaction(thd,
                                          m_rli,
                                          ws_meta,
                                          thd->wsrep_sr().fragments());
  }

  ret= ret || wsrep_apply_events(thd, m_rli, data.data(), data.size());

  if (ret || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf_with_header(thd, data.data(), data.size());
  }

  thd->close_temporary_tables();
  if (!ret && !(ws_meta.flags() & wsrep::provider::flag::commit))
  {
    thd->wsrep_cs().fragment_applied(ws_meta.seqno());
  }

  thd_proc_info(thd, "wsrep replayed write set");
  DBUG_RETURN(ret);
}
