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

#include "mariadb.h"
#include "wsrep_thd.h"
#include "transaction.h"
#include "rpl_rli.h"
#include "log_event.h"
#include "sql_parse.h"
//#include "global_threads.h" // LOCK_thread_count, etc.
#include "sql_base.h" // close_thread_tables()
#include "mysqld.h"   // start_wsrep_THD();
#include "wsrep_applier.h"   // start_wsrep_THD();
#include "wsrep_sr.h"        // wsrep_abort_SR_THD();

#include "slave.h"    // opt_log_slave_updates
#include "rpl_filter.h"
#include "rpl_rli.h"
#include "rpl_mi.h"

static Wsrep_thd_queue* wsrep_rollback_queue = 0;
static Wsrep_thd_queue* wsrep_post_rollback_queue = 0;

#if (__LP64__)
static volatile int64 wsrep_bf_aborts_counter(0);
#define WSREP_ATOMIC_LOAD_LONG my_atomic_load64
#define WSREP_ATOMIC_ADD_LONG  my_atomic_add64
#else
static volatile int32 wsrep_bf_aborts_counter(0);
#define WSREP_ATOMIC_LOAD_LONG my_atomic_load32
#define WSREP_ATOMIC_ADD_LONG  my_atomic_add32
#endif

int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, char *buff,
                          enum enum_var_type scope)
{
  wsrep_local_bf_aborts = WSREP_ATOMIC_LOAD_LONG(&wsrep_bf_aborts_counter);
  var->type = SHOW_LONGLONG;
  var->value = (char*)&wsrep_local_bf_aborts;
  return 0;
}
void wsrep_cleanup_transaction(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ENTER("wsrep_cleanup_transaction");
  if (thd->wsrep_exec_mode == REPL_RECV)  return;

  DBUG_ASSERT(thd->wsrep_conflict_state() != MUST_REPLAY);
  DBUG_ASSERT(thd->wsrep_SR_fragments.empty());

  if (wsrep_SR_store) wsrep_SR_store->trx_done(thd);
  if (wsrep_emulate_bin_log) wsrep_thd_binlog_trx_reset(thd);

  wsrep_reset_SR_trans(thd);
  thd->wsrep_exec_mode= LOCAL_STATE;
  if (thd->wsrep_conflict_state() != NO_CONFLICT)
  {
    /*
      Catch half finished rollbacks.
     */
    DBUG_ASSERT(thd->wsrep_conflict_state() == ABORTED ||
                thd->wsrep_conflict_state() == CERT_FAILURE);

    thd->killed= NOT_KILLED;
    thd->set_wsrep_conflict_state(NO_CONFLICT);
  }

  if (MUST_REPLAY != thd->wsrep_conflict_state())
  {
    thd->wsrep_PA_safe= true;
    thd->wsrep_ws_handle.trx_id= WSREP_UNDEFINED_TRX_ID;
    thd->set_wsrep_next_trx_id(WSREP_UNDEFINED_TRX_ID);

    if (thd->wsrep_trx_meta.gtid.seqno != WSREP_SEQNO_UNDEFINED)
    {
      thd->wsrep_last_written_gtid= thd->wsrep_trx_meta.gtid;
    }
    thd->wsrep_trx_meta.gtid= WSREP_GTID_UNDEFINED;
    thd->wsrep_trx_meta.depends_on= WSREP_SEQNO_UNDEFINED;
    thd->wsrep_affected_rows= 0;
    thd->wsrep_skip_wsrep_GTID= false;
    thd->wsrep_xid.null();
  }

  DBUG_VOID_RETURN;
}

/*
  Run post rollback actions.

  Assert thd->LOCK_wsrep_thd ownership
 */
void wsrep_post_rollback(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);

  WSREP_LOG_THD(thd, NULL);

  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT || /* voluntary */
              thd->wsrep_conflict_state() == ABORTING); /* BF abort or cert failure  */

  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  if (wsrep_thd_trx_seqno(thd) != WSREP_SEQNO_UNDEFINED)
  {
    /*
      Write set was ordered but it needs to roll back. We need a
      call to post_rollback() to grab critical section.
    */
    if (wsrep->post_rollback(wsrep, &thd->wsrep_ws_handle))
    {
      WSREP_WARN("wsrep::post_rollback fail");
    }
  }
  if (wsrep->release(wsrep, &thd->wsrep_ws_handle))
  {
    WSREP_WARN("wsrep::release fail: %llu %d",
               (long long)thd->thread_id, thd->get_stmt_da()->status());
  }


  mysql_mutex_lock(&thd->LOCK_wsrep_thd);

  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT ||
              thd->wsrep_conflict_state() == ABORTING);

  if (thd->wsrep_conflict_state() == NO_CONFLICT)
  {
    thd->set_wsrep_conflict_state(MUST_ABORT);
    thd->set_wsrep_conflict_state(ABORTING);
    thd->set_wsrep_conflict_state(ABORTED);
  }
  else
  {
    thd->set_wsrep_conflict_state(ABORTED);
  }
}

/*
  must have (&thd->LOCK_wsrep_thd)
  thd->wsrep_conflict_state must be MUST_ABORT
*/
void wsrep_client_rollback(THD *thd, bool rollbacker)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_ABORT ||
              thd->wsrep_conflict_state() == CERT_FAILURE);
  WSREP_DEBUG("client rollback due to BF abort for (%lld %lld), query: %s",
              thd->thread_id, thd->query_id, WSREP_QUERY(thd));

  my_atomic_add64(&wsrep_bf_aborts_counter, 1);

  /*
    Rollback proccess should be fired only for threads which are not
    in the process of committing.
  */
  DBUG_ASSERT(thd->wsrep_query_state() != QUERY_COMMITTING);
  if (rollbacker)
  {
    DBUG_ASSERT(thd->wsrep_trx_meta.gtid.seqno == WSREP_SEQNO_UNDEFINED);
  }

  thd->set_wsrep_conflict_state(ABORTING);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  if (thd->wsrep_is_streaming())
  {
    WSREP_DEBUG("wsrep_client_rollback: thd: %lld fragments %zu",
                thd->thread_id, thd->wsrep_SR_fragments.size());
    wsrep_SR_store->rollback_trx(thd);
    thd->wsrep_SR_fragments.clear();
  }

  if (thd->locked_tables_mode && thd->lock)
  {
    WSREP_DEBUG("unlocking tables for BF abort (%lld)", thd->thread_id);
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
  }

  if (thd->global_read_lock.is_acquired())
  {
    WSREP_DEBUG("unlocking GRL for BF abort (%lld)", thd->thread_id);
    thd->global_read_lock.unlock_global_read_lock(thd);
  }

  /* Release transactional metadata locks. */
  thd->mdl_context.release_transactional_locks();

  /* release explicit MDL locks */
  thd->mdl_context.release_explicit_locks();

  if (thd->get_binlog_table_maps())
  {
    WSREP_DEBUG("clearing binlog table map for BF abort (%lld)", thd->thread_id);
    thd->clear_binlog_table_maps();
  }

  /*
    trans_rolback() must be called after all locks are released since it
    calls ha_rollback_trans() which acquires TO
  */
  if (trans_rollback(thd))
  {
    WSREP_WARN("client rollback failed for: %lld %lld, conf: %d",
               thd->thread_id, thd->query_id,
               thd->wsrep_conflict_state_unsafe());
  }

  if (rollbacker && thd->wsrep_trx_meta.gtid.seqno != WSREP_SEQNO_UNDEFINED)
  {
    /*
      Thd has been assigned seqno and it needs to release provider
      resoureces. Do it in separate thread to avoid deadlocks.
    */
    DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_ROLLBACK);
    if (wsrep_post_rollback_queue->push_back(thd))
    {
      WSREP_WARN("duplicate thd %llu for post-rollbacker",
                 wsrep_thd_thread_id(thd));
    }
  }

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  /*
    If the seqno is not set there is no need for post rollback
    actions.
   */
  if (rollbacker && wsrep_thd_trx_seqno(thd) == WSREP_SEQNO_UNDEFINED)
  {
    wsrep_post_rollback(thd);
  }

  return;
}

#define NUMBER_OF_FIELDS_TO_IDENTIFY_COORDINATOR 1
#define NUMBER_OF_FIELDS_TO_IDENTIFY_WORKER 2
//#include "rpl_info_factory.h"

static rpl_group_info* wsrep_relay_group_init(const char* log_fname)
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
  rli->mi = new Master_info(&connection_name, false);

  struct rpl_group_info *rgi= new rpl_group_info(rli);
  rgi->thd= rli->sql_driver_thd= current_thd;

  if ((rgi->deferred_events_collecting= rli->mi->rpl_filter->is_on()))
  {
    rgi->deferred_events= new Deferred_log_events(rli);
  }

  return rgi;
}

void wsrep_prepare_bf_thd(THD *thd, struct wsrep_thd_shadow* shadow)
{
  shadow->options       = thd->variables.option_bits;
  shadow->server_status = thd->server_status;
  shadow->wsrep_exec_mode = thd->wsrep_exec_mode;
  shadow->vio           = thd->net.vio;

  // Disable general logging on applier threads
  thd->variables.option_bits |= OPTION_LOG_OFF;
  // Enable binlogging if opt_log_slave_updates is set
  if (opt_log_slave_updates)
    thd->variables.option_bits|= OPTION_BIN_LOG;
  else
    thd->variables.option_bits&= ~(OPTION_BIN_LOG);

  if (!thd->wsrep_rgi) thd->wsrep_rgi= wsrep_relay_group_init("wsrep_relay");

  /* thd->system_thread_info.rpl_sql_info isn't initialized. */
  thd->system_thread_info.rpl_sql_info=
    new rpl_sql_thread_info(thd->wsrep_rgi->rli->mi->rpl_filter);

  thd->wsrep_exec_mode= REPL_RECV;
  thd->net.vio= 0;
  thd->clear_error();

  shadow->tx_isolation        = thd->variables.tx_isolation;
  thd->variables.tx_isolation = ISO_READ_COMMITTED;
  thd->tx_isolation           = ISO_READ_COMMITTED;

  shadow->db            = thd->db;
  shadow->db_length     = thd->db_length;
  shadow->user_time     = thd->user_time;
  shadow->row_count_func= thd->get_row_count_func();
  thd->reset_db(NULL, 0);

  shadow->user_time     = thd->user_time;

  shadow->row_count_func= thd->get_row_count_func();
}

void wsrep_return_from_bf_mode(THD *thd, struct wsrep_thd_shadow* shadow)
{
  thd->variables.option_bits  = shadow->options;
  thd->server_status          = shadow->server_status;
  thd->wsrep_exec_mode        = shadow->wsrep_exec_mode;
  thd->net.vio                = shadow->vio;
  thd->variables.tx_isolation = shadow->tx_isolation;
  thd->reset_db(shadow->db, shadow->db_length);
  thd->set_row_count_func(shadow->row_count_func);
  thd->user_time              = shadow->user_time;

  delete thd->system_thread_info.rpl_sql_info;
  delete thd->wsrep_rgi->rli->mi;
  delete thd->wsrep_rgi->rli;

  thd->wsrep_rgi->cleanup_after_session();
  delete thd->wsrep_rgi;
  thd->wsrep_rgi = NULL;
  thd->set_row_count_func(shadow->row_count_func);
}

void wsrep_replay_transaction(THD *thd)
{
  DBUG_ENTER("wsrep_replay_transaction");
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  /* checking if BF trx must be replayed */
  if (thd->wsrep_conflict_state() == MUST_REPLAY) {
    DBUG_ASSERT(wsrep_thd_trx_seqno(thd) > 0);
    if (thd->wsrep_exec_mode!= REPL_RECV) {
      if (thd->get_stmt_da()->is_sent())
      {
        WSREP_ERROR("replay issue, thd has reported status already");
      }


      /*
        PS reprepare observer should have been removed already.
        open_table() will fail if we have dangling observer here.
      */
      DBUG_ASSERT(thd->m_reprepare_observer == NULL);

      struct da_shadow
      {
          enum Diagnostics_area::enum_diagnostics_status status;
          ulonglong affected_rows;
          ulonglong last_insert_id;
          char message[MYSQL_ERRMSG_SIZE];
      };
      struct da_shadow da_status;
      da_status.status= thd->get_stmt_da()->status();
      if (da_status.status == Diagnostics_area::DA_OK)
      {
        da_status.affected_rows= thd->get_stmt_da()->affected_rows();
        da_status.last_insert_id= thd->get_stmt_da()->last_insert_id();
        strmake(da_status.message,
                thd->get_stmt_da()->message(),
                sizeof(da_status.message)-1);
      }

      thd->get_stmt_da()->reset_diagnostics_area();

      thd->set_wsrep_conflict_state(REPLAYING);
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

      thd->reset_for_next_command();
      thd->reset_killed();
      close_thread_tables(thd);
      if (thd->locked_tables_mode && thd->lock)
      {
        WSREP_DEBUG("releasing table lock for replaying (%lld)",
                    (longlong) thd->thread_id);
        thd->locked_tables_list.unlock_locked_tables(thd);
        thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
      }
      thd->mdl_context.release_transactional_locks();
      /*
        Replaying will call MYSQL_START_STATEMENT when handling
        BEGIN Query_log_event so end statement must be called before
        replaying.
      */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;
      thd_proc_info(thd, "WSREP replaying trx");
      WSREP_DEBUG("replay trx: %s %lld",
                  thd->query() ? thd->query() : "void",
                  (long long)wsrep_thd_trx_seqno(thd));
      struct wsrep_thd_shadow shadow;
      wsrep_prepare_bf_thd(thd, &shadow);

      /* From trans_begin() */
      thd->variables.option_bits|= OPTION_BEGIN;
      thd->server_status|= SERVER_STATUS_IN_TRANS;

      int rcode = wsrep->replay_trx(wsrep,
                                    &thd->wsrep_ws_handle,
                                    (void *)thd);

      wsrep_return_from_bf_mode(thd, &shadow);

      WSREP_DEBUG("replayed %lld, seqno %lld, rcode %d",
                   thd->thread_id, (long long)wsrep_thd_trx_seqno(thd), rcode);
      DBUG_ASSERT(wsrep_thd_trx_seqno(thd) > 0);

      mysql_mutex_lock(&thd->LOCK_wsrep_thd);

      if (thd->wsrep_conflict_state() != REPLAYING)
        WSREP_WARN("lost replaying mode: %d", thd->wsrep_conflict_state());

      switch (rcode)
      {
      case WSREP_OK:
        thd->killed= NOT_KILLED;
        thd->set_wsrep_conflict_state(NO_CONFLICT);
        wsrep->release(wsrep, &thd->wsrep_ws_handle);
        WSREP_DEBUG("trx_replay successful for: %lld %lld",
                    (longlong) thd->thread_id, (longlong) thd->real_id);
        if (thd->get_stmt_da()->is_sent())
        {
          WSREP_WARN("replay ok, thd has reported status");
        }
        else if (thd->get_stmt_da()->is_set())
        {
          if (thd->get_stmt_da()->status() != Diagnostics_area::DA_OK &&
              thd->get_stmt_da()->status() != Diagnostics_area::DA_OK_BULK)
          {
            WSREP_WARN("replay ok, thd has error status %d",
                       thd->get_stmt_da()->status());
          }
        }
        else
        {
          if (da_status.status == Diagnostics_area::DA_OK)
          {
            my_ok(thd,
                  da_status.affected_rows,
                  da_status.last_insert_id,
                  da_status.message);
          }
          else
          {
            my_ok(thd);
          }
        }
        break;
      case WSREP_TRX_FAIL:
        if (thd->get_stmt_da()->is_sent())
        {
          WSREP_ERROR("replay failed, thd has reported status");
        }
        else
        {
          WSREP_DEBUG("replay failed, rolling back");
          my_error(ER_LOCK_DEADLOCK, MYF(0));
        }
        WSREP_DEBUG("Setting thd to ABORTING, thd %lld conf %d",
                    thd->thread_id, thd->wsrep_conflict_state());
        if (thd->wsrep_conflict_state() != CERT_FAILURE)
          thd->set_wsrep_conflict_state(ABORTING);
        /* We returned out ouf order, trx is rolled back,
         * no locks should remain. Need to do the total order part */
        DBUG_ASSERT(LOCAL_ROLLBACK != thd->wsrep_exec_mode);
        thd->wsrep_exec_mode= LOCAL_ROLLBACK;
        WSREP_DEBUG("replay_transaction(%lld) assigned LOCAL_ROLLBACK to "
                    "seqno %lld, conf %d",
                    thd->thread_id, (long long)wsrep_thd_trx_seqno(thd),
                    thd->wsrep_conflict_state());
        wsrep_post_rollback(thd);

        break;
      default:
        WSREP_ERROR("trx_replay failed for: %d, schema: %s, query: %s",
                    rcode,
                    (thd->db ? thd->db : "(null)"),
                    thd->query() ? thd->query() : "void");
        DBUG_ASSERT(0);
        /* we're now in inconsistent state, must abort */

        /* http://bazaar.launchpad.net/~codership/codership-mysql/5.6/revision/3962#sql/wsrep_thd.cc */
        mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
        unireg_abort(1);
        break;
      }

      wsrep_cleanup_transaction(thd);

      mysql_mutex_lock(&LOCK_wsrep_replaying);
      wsrep_replaying--;
      WSREP_DEBUG("replaying decreased: %d, thd: %lld",
                  wsrep_replaying, (longlong) thd->thread_id);
      mysql_cond_broadcast(&COND_wsrep_replaying);
      mysql_mutex_unlock(&LOCK_wsrep_replaying);
    }
  }
  DBUG_VOID_RETURN;
}

static void wsrep_replication_process(THD *thd,
                                      void* arg __attribute__((unused)))
{
  int rcode;
  DBUG_ENTER("wsrep_replication_process");

  struct wsrep_thd_shadow shadow;
  wsrep_prepare_bf_thd(thd, &shadow);

  /* From trans_begin() */
  thd->variables.option_bits|= OPTION_BEGIN;
  thd->server_status|= SERVER_STATUS_IN_TRANS;

  rcode = wsrep->recv(wsrep, (void *)thd);
  DBUG_PRINT("wsrep",("wsrep_repl returned: %d", rcode));

  WSREP_INFO("applier thread %lld exiting (code:%d)",
             thd->thread_id, rcode);

  switch (rcode) {
  case WSREP_OK:
  case WSREP_NOT_IMPLEMENTED:
  case WSREP_CONN_FAIL:
    /* provider does not support slave operations / disconnected from group,
     * just close applier thread */
    break;
  case WSREP_NODE_FAIL:
    /* data inconsistency => SST is needed */
    /* Note: we cannot just blindly restart replication here,
     * SST might require server restart if storage engines must be
     * initialized after SST */
    WSREP_ERROR("node consistency compromised, aborting");
    wsrep_kill_mysql(thd);
    break;
  case WSREP_WARNING:
  case WSREP_TRX_FAIL:
  case WSREP_TRX_MISSING:
    /* these suggests a bug in provider code */
    WSREP_WARN("bad return from recv() call: %d", rcode);
    /* Shut down this node. */
    /* fall through */
  case WSREP_FATAL:
    /* Cluster connectivity is lost.
     *
     * If applier was killed on purpose (KILL_CONNECTION), we
     * avoid mysql shutdown. This is because the killer will then handle
     * shutdown processing (or replication restarting)
     */
    if (thd->killed != KILL_CONNECTION)
    {
      wsrep_kill_mysql(thd);
    }
    break;
  }

  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_close_applier(thd);
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  if(thd->has_thd_temporary_tables())
  {
    WSREP_WARN("Applier %lld has temporary tables at exit.",
               thd->thread_id);
  }
  wsrep_return_from_bf_mode(thd, &shadow);
  DBUG_VOID_RETURN;
}

static bool create_wsrep_THD(Wsrep_thd_args* args)
{
  ulong old_wsrep_running_threads= wsrep_running_threads;
  pthread_t unused;
  mysql_mutex_lock(&LOCK_thread_count);

  bool res= pthread_create(&unused, &connection_attrib, start_wsrep_THD,
                           args);
  /*
    if starting a thread on server startup, wait until the this thread's THD
    is fully initialized (otherwise a THD initialization code might
    try to access a partially initialized server data structure - MDEV-8208).
  */
  if (!mysqld_server_initialized)
    while (old_wsrep_running_threads == wsrep_running_threads)
      mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);
  return res;
}

void wsrep_create_appliers(long threads)
{
  if (!wsrep_connected)
  {
    /* see wsrep_replication_start() for the logic */
    if (wsrep_cluster_address && strlen(wsrep_cluster_address) &&
        wsrep_provider && strcasecmp(wsrep_provider, "none"))
    {
      WSREP_ERROR("Trying to launch slave threads before creating "
                  "connection at '%s'", wsrep_cluster_address);
      assert(0);
    }
    return;
  }

  long wsrep_threads=0;
  
  while (wsrep_threads++ < threads)
  {
    Wsrep_thd_args* args(new Wsrep_thd_args(wsrep_replication_process, 0));
    if (create_wsrep_THD(args))
    {
      WSREP_WARN("Can't create thread to manage wsrep replication");
    }
  }
}

static void wsrep_rollback_process(THD *rollbacker,
                                   void *arg __attribute__((unused)))
{
  DBUG_ENTER("wsrep_rollback_process");

  THD* thd= NULL;
  wsrep_rollback_queue= new Wsrep_thd_queue(rollbacker);

  thd_proc_info(rollbacker, "wsrep aborter idle");
  while ((thd= wsrep_rollback_queue->pop_front()) != NULL)
  {
#ifdef OLD_MARIADB
    thd_proc_info(thd, "wsrep aborter idle");
    thd->mysys_var->current_mutex= &LOCK_wsrep_rollback;
    thd->mysys_var->current_cond=  &COND_wsrep_rollback;

    mysql_cond_wait(&COND_wsrep_rollback,&LOCK_wsrep_rollback);

    WSREP_DEBUG("WSREP rollback thread wakes for signal");

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd_proc_info(thd, "WSREP aborter active");
    thd->mysys_var->current_mutex= 0;
    thd->mysys_var->current_cond=  0;
    mysql_mutex_unlock(&thd->mysys_var->mutex);

    /* check for false alarms */
    if (!wsrep_aborting_thd)
    {
      WSREP_DEBUG("WSREP rollback thread has empty abort queue");
    }
    /* process all entries in the queue */
    while (wsrep_aborting_thd) {
      THD *aborting;
      wsrep_aborting_thd_t next = wsrep_aborting_thd->next;
      aborting = wsrep_aborting_thd->aborting_thd;
      my_free(wsrep_aborting_thd);
      wsrep_aborting_thd= next;
      /*
       * must release mutex, appliers my want to add more
       * aborting thds in our work queue, while we rollback
       */
      mysql_mutex_unlock(&LOCK_wsrep_rollback);

      mysql_mutex_lock(&aborting->LOCK_wsrep_thd);
      if (aborting->wsrep_conflict_state()== ABORTED)
      {
        WSREP_DEBUG("WSREP, thd already aborted: %llu state: %d",
                    (long long)aborting->real_id,
                    aborting->wsrep_conflict_state());

        mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);
        mysql_mutex_lock(&LOCK_wsrep_rollback);
        continue;
      }

      mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);

      set_current_thd(aborting); 
      aborting->store_globals();

      if (wsrep_thd_is_SR(aborting))
      {
        WSREP_DEBUG("WSREP rollbacker aborting SR thd: (%lld %llu)",
                    aborting->thread_id, (long long)aborting->real_id);
        wsrep_abort_SR_THD(thd, aborting);
      }
      else
      {
        mysql_mutex_lock(&aborting->LOCK_wsrep_thd);

        /* prepare THD for rollback processing */
        aborting->reset_for_next_command();
        aborting->lex->sql_command= SQLCOM_ROLLBACK;

        wsrep_client_rollback(aborting, true);
        mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);
        WSREP_DEBUG("WSREP rollbacker aborted thd: (%lld %llu)",
                    aborting->thread_id, (long long)aborting->real_id);
      }

      mysql_mutex_lock(&LOCK_wsrep_rollback);
    }
#else
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    if (thd->wsrep_conflict_state() == ABORTED)
    {
      WSREP_DEBUG("rollbacker thd already aborted: %llu state: %d",
                  (long long)thd->real_id,
                  thd->wsrep_conflict_state());

      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      continue;
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

    thd_proc_info(rollbacker, "wsrep aborter active");

    thd->store_globals();
    if (wsrep_thd_is_SR(thd))
    {
      WSREP_DEBUG("rollbacker aborting SR thd: (%lld %llu)",
                  thd->thread_id, (long long)thd->real_id);
      wsrep_abort_SR_THD(rollbacker, thd);
    }
    else
    {
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);

      /* prepare THD for rollback processing */
      thd->reset_for_next_command();
      thd->lex->sql_command= SQLCOM_ROLLBACK;

      wsrep_client_rollback(thd, true);
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      WSREP_DEBUG("rollbacker aborted thd: (%lld %llu)",
                  thd->thread_id, (long long)thd->real_id);
    }

    thd_proc_info(rollbacker, "wsrep aborter idle");
#endif
  }
  
  delete wsrep_rollback_queue;
  wsrep_rollback_queue= NULL;

  sql_print_information("WSREP: rollbacker thread exiting");

  DBUG_ASSERT(rollbacker->killed != NOT_KILLED);
  DBUG_PRINT("wsrep",("wsrep rollbacker thread exiting"));
  DBUG_VOID_RETURN;
}

static void wsrep_post_rollback_process(THD *post_rollbacker,
                                        void *arg __attribute__((unused)))
{
  DBUG_ENTER("wsrep_post_rollback_process");
  THD* thd= NULL;
  wsrep_post_rollback_queue= new Wsrep_thd_queue(post_rollbacker);

  while ((thd= wsrep_post_rollback_queue->pop_front()) != NULL)
  {
    thd->store_globals();

    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    DBUG_ASSERT(thd->wsrep_conflict_state() == ABORTING);
    DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_ROLLBACK);
    WSREP_DEBUG("post rollbacker calling post rollback for thd %lld, conf %s",
                thd->thread_id, wsrep_thd_conflict_state_str(thd));

    wsrep_post_rollback(thd);
    DBUG_ASSERT(thd->wsrep_conflict_state() == ABORTED);
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }

  delete wsrep_post_rollback_queue;
  wsrep_post_rollback_queue= NULL;

  DBUG_ASSERT(post_rollbacker->killed != NOT_KILLED);
  DBUG_PRINT("wsrep",("wsrep post rollbacker thread exiting"));
  DBUG_VOID_RETURN;
}

void wsrep_create_rollbacker()
{
  if (wsrep_provider && strcasecmp(wsrep_provider, "none"))
  {
    Wsrep_thd_args* args= new Wsrep_thd_args(wsrep_rollback_process, 0);

    /* create rollbacker */
    if (create_wsrep_THD(args))
      WSREP_WARN("Can't create thread to manage wsrep rollback");

    /* create post_rollbacker */
    args= new Wsrep_thd_args(wsrep_post_rollback_process, 0);
    if (create_wsrep_THD(args))
      WSREP_WARN("Can't create thread to manage wsrep post rollback");
   }
}

void wsrep_thd_set_PA_safe(void *thd_ptr, my_bool safe)
{ 
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    thd->wsrep_PA_safe = safe;
  }
}

enum wsrep_conflict_state wsrep_thd_conflict_state(THD *thd, my_bool sync)
{ 
  enum wsrep_conflict_state state = NO_CONFLICT;
  if (thd)
  {
    if (sync) mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    
    state = thd->wsrep_conflict_state();
    if (sync) mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  return state;
}

my_bool wsrep_thd_is_wsrep(THD *thd)
{
  my_bool status = FALSE;
  if (thd)
  {
    status = (WSREP(thd) && WSREP_PROVIDER_EXISTS);
  }
  return status;
}

//my_bool wsrep_thd_is_BF(THD *thd, my_bool sync)
my_bool wsrep_thd_is_BF(void *thd_ptr, my_bool sync)
{
  my_bool status = FALSE;
  if (thd_ptr)
  {
    THD* thd = (THD*)thd_ptr;
    // THD can be BF only if provider exists
    if (wsrep_thd_is_wsrep(thd))
    {
      if (sync)
	mysql_mutex_lock(&thd->LOCK_wsrep_thd);

      status = ((thd->wsrep_exec_mode == REPL_RECV)    ||
      	        (thd->wsrep_exec_mode == TOTAL_ORDER));
      if (sync)
        mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    }
  }
  return status;
}

my_bool wsrep_thd_is_SR(void *thd_ptr)
{
  if (thd_ptr)
  {
    THD* thd = (THD*)thd_ptr;
    return (thd->wsrep_SR_thd);
  }
  return false;
}

my_bool wsrep_thd_skip_locking(void *thd)
{
  return thd != NULL && ((THD*)thd)->wsrep_skip_locking;
}

extern "C"
my_bool wsrep_thd_is_BF_or_commit(void *thd_ptr, my_bool sync)
{
  bool status = FALSE;
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    if (sync) mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    
    status = ((thd->wsrep_exec_mode == REPL_RECV)    ||
	      (thd->wsrep_exec_mode == TOTAL_ORDER)  ||
	      (thd->wsrep_exec_mode == LOCAL_COMMIT));
    if (sync) mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  return status;
}

my_bool wsrep_thd_is_local(void *thd_ptr, my_bool sync)
{
  bool status = FALSE;
  if (thd_ptr) 
  {
    THD* thd = (THD*)thd_ptr;
    if (sync) mysql_mutex_lock(&thd->LOCK_wsrep_thd);

    status = (thd->wsrep_exec_mode == LOCAL_STATE);
    if (sync) mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  return status;
}

/*
  Start async rollback process

  Asserts thd->LOCK_wsrep_thd ownership
 */
void wsrep_fire_rollbacker(THD *thd)
{

  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_ABORT);

  DBUG_PRINT("wsrep",("enqueuing trx abort for %llu", wsrep_thd_thread_id(thd)));
  WSREP_DEBUG("enqueuing trx abort for (%llu)", wsrep_thd_thread_id(thd));

  if (wsrep_rollback_queue->push_back(thd))
  {
    WSREP_WARN("duplicate thd %llu for rollbacker",
               wsrep_thd_thread_id(thd));
  }
}


int wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr, my_bool signal)
{
  THD *victim_thd = (THD *) victim_thd_ptr;
  THD *bf_thd     = (THD *) bf_thd_ptr;
  DBUG_ENTER("wsrep_abort_thd");

  mysql_mutex_lock(&victim_thd->LOCK_wsrep_thd);
  if ( (WSREP(bf_thd) ||
         ( (WSREP_ON || bf_thd->variables.wsrep_OSU_method == WSREP_OSU_RSU) &&
           bf_thd->wsrep_exec_mode == TOTAL_ORDER) )                         &&
       victim_thd &&
       !victim_thd->wsrep_is_rolling_back())
  {
    if (wsrep_thd_is_SR(victim_thd))
    {
      victim_thd->set_wsrep_conflict_state(MUST_ABORT);
      wsrep_fire_rollbacker(victim_thd);
      {
        WSREP_INFO("rollbacker fired for aborting SR transaction");
      }
    }
    else
    {
      WSREP_DEBUG("wsrep_abort_thd, by: %llu, victim: %llu", (bf_thd) ?
                  (long long)bf_thd->real_id : 0, (long long)victim_thd->real_id);
      mysql_mutex_unlock(&victim_thd->LOCK_wsrep_thd);
      ha_abort_transaction(bf_thd, victim_thd, signal);
      mysql_mutex_lock(&victim_thd->LOCK_wsrep_thd);
    }
  }
  else
  {
    WSREP_DEBUG("wsrep_abort_thd not effective: %p %p", bf_thd, victim_thd);
  }
  mysql_mutex_unlock(&victim_thd->LOCK_wsrep_thd);
  DBUG_RETURN(1);
}

int wsrep_thd_in_locking_session(void *thd_ptr)
{
  if (thd_ptr && ((THD *)thd_ptr)->in_lock_tables) {
    return 1;
  }
  return 0;
}

THD* wsrep_start_SR_THD(char *thread_stack)
{  
  THD* thd;
  if (!(thd= new THD(0)))
  {
    WSREP_ERROR("Could not create THD for Streaming Replication");
    goto err;
  }

  thd->thread_stack= thread_stack;
  if (thd->store_globals())
  {
    WSREP_ERROR("Could not create THD for Streaming Replication");
    delete thd;
    goto err;
  }

  thd->real_id=pthread_self(); // Keep purify happy
  mysql_mutex_lock(&LOCK_thread_count);
  //add_global_thread(thd);
  add_to_active_threads(thd);
  
  thd->thread_id= thd->variables.pseudo_thread_id= next_thread_id();
  (void)mysql_mutex_unlock(&LOCK_thread_count);

  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();
  thd->proc_info= 0;
  thd->set_command(COM_SLEEP);
  thd->set_time();
  thd->init_for_queries();

  struct wsrep_thd_shadow shadow;

  wsrep_prepare_bf_thd(thd, &shadow);

  thd->wsrep_SR_thd = true;
  WSREP_DEBUG("SR thread created, id: %lld thd: %p", thd->thread_id, thd);
 err:
  return thd;
}

void wsrep_end_SR_THD(THD *thd)
{
  WSREP_DEBUG("Stopping Streaming Replication thd: %lld", thd->thread_id);

  close_thread_tables(thd);
  mysql_mutex_lock(&LOCK_thread_count);
  thd->unlink();
  mysql_mutex_unlock(&LOCK_thread_count);

  delete thd;
  /* Remember that we don't have a THD */
  //my_pthread_setspecific_ptr(THR_THD,  0);

  my_thread_end();
}

bool wsrep_thd_has_explicit_locks(THD *thd)
{
  assert(thd);
  return thd->mdl_context.has_explicit_locks();
}
