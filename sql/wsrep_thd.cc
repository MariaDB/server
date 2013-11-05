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

#include "wsrep_thd.h"

#include "transaction.h"
#include "rpl_rli.h"
#include "log_event.h"
#include "sql_parse.h"
#include "slave.h"    // opt_log_slave_updates
#include "sql_base.h" // close_thread_tables()
#include "mysqld.h"   // start_wsrep_THD();

/* must have (&thd->LOCK_wsrep_thd) */
void wsrep_client_rollback(THD *thd)
{
  WSREP_DEBUG("client rollback due to BF abort for (%ld), query: %s",
              thd->thread_id, thd->query());

  thd->wsrep_conflict_state= ABORTING;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  trans_rollback(thd);

  if (thd->locked_tables_mode && thd->lock)
  {
    WSREP_DEBUG("unlocking tables for BF abort (%ld)", thd->thread_id);
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
  }

  if (thd->global_read_lock.is_acquired())
  {
    WSREP_DEBUG("unlocking GRL for BF abort (%ld)", thd->thread_id);
    thd->global_read_lock.unlock_global_read_lock(thd);
  }

  /* Release transactional metadata locks. */
  thd->mdl_context.release_transactional_locks();

  /* release explicit MDL locks */
  thd->mdl_context.release_explicit_locks();

  if (thd->get_binlog_table_maps())
  {
    WSREP_DEBUG("clearing binlog table map for BF abort (%ld)", thd->thread_id);
    thd->clear_binlog_table_maps();
  }
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  thd->wsrep_conflict_state= ABORTED;
}

static Relay_log_info* wsrep_relay_log_init(const char* log_fname)
{
  Relay_log_info* rli= new Relay_log_info(false);

  rli->no_storage= true;
  if (!rli->relay_log.description_event_for_exec)
  {
    rli->relay_log.description_event_for_exec=
      new Format_description_log_event(4);
  }

  rli->sql_thd= current_thd;
  return rli;
}

static void wsrep_prepare_bf_thd(THD *thd, struct wsrep_thd_shadow* shadow)
{
  shadow->options       = thd->variables.option_bits;
  shadow->wsrep_exec_mode = thd->wsrep_exec_mode;
  shadow->vio           = thd->net.vio;

  if (opt_log_slave_updates)
    thd->variables.option_bits|= OPTION_BIN_LOG;
  else
    thd->variables.option_bits&= ~(OPTION_BIN_LOG);

  if (!thd->wsrep_rli) thd->wsrep_rli= wsrep_relay_log_init("wsrep_relay");

  thd->wsrep_exec_mode= REPL_RECV;
  thd->net.vio= 0;
  thd->clear_error();

  thd->variables.option_bits|= OPTION_NOT_AUTOCOMMIT;

  shadow->tx_isolation        = thd->variables.tx_isolation;
  thd->variables.tx_isolation = ISO_READ_COMMITTED;
  thd->tx_isolation           = ISO_READ_COMMITTED;
}

static void wsrep_return_from_bf_mode(THD *thd, struct wsrep_thd_shadow* shadow)
{
  thd->variables.option_bits  = shadow->options;
  thd->wsrep_exec_mode        = shadow->wsrep_exec_mode;
  thd->net.vio                = shadow->vio;
  thd->variables.tx_isolation = shadow->tx_isolation;
}

void wsrep_replay_transaction(THD *thd)
{
  /* checking if BF trx must be replayed */
  if (thd->wsrep_conflict_state== MUST_REPLAY) {
    if (thd->wsrep_exec_mode!= REPL_RECV) {
      if (thd->stmt_da->is_sent)
      {
        WSREP_ERROR("replay issue, thd has reported status already");
      }
      thd->stmt_da->reset_diagnostics_area();

      thd->wsrep_conflict_state= REPLAYING;
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

      mysql_reset_thd_for_next_command(thd);
      thd->killed= THD::NOT_KILLED;
      close_thread_tables(thd);
      if (thd->locked_tables_mode && thd->lock)
      {
        WSREP_DEBUG("releasing table lock for replaying (%ld)",
                    thd->thread_id);
        thd->locked_tables_list.unlock_locked_tables(thd);
        thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
      }
      thd->mdl_context.release_transactional_locks();

      thd_proc_info(thd, "wsrep replaying trx");
      WSREP_DEBUG("replay trx: %s %lld",
                  thd->query() ? thd->query() : "void",
                  (long long)wsrep_thd_trx_seqno(thd));
      struct wsrep_thd_shadow shadow;
      wsrep_prepare_bf_thd(thd, &shadow);
      int rcode = wsrep->replay_trx(wsrep,
                                    &thd->wsrep_ws_handle,
                                    (void *)thd);

      wsrep_return_from_bf_mode(thd, &shadow);
      if (thd->wsrep_conflict_state!= REPLAYING)
        WSREP_WARN("lost replaying mode: %d", thd->wsrep_conflict_state );

      mysql_mutex_lock(&thd->LOCK_wsrep_thd);

      switch (rcode)
      {
      case WSREP_OK:
        thd->wsrep_conflict_state= NO_CONFLICT;
        wsrep->post_commit(wsrep, &thd->wsrep_ws_handle);
        WSREP_DEBUG("trx_replay successful for: %ld %llu",
                    thd->thread_id, (long long)thd->real_id);
        if (thd->stmt_da->is_sent)
        {
          WSREP_WARN("replay ok, thd has reported status");
        }
        else
        {
          my_ok(thd);
        }
        break;
      case WSREP_TRX_FAIL:
        if (thd->stmt_da->is_sent)
        {
          WSREP_ERROR("replay failed, thd has reported status");
        }
        else
        {
          WSREP_DEBUG("replay failed, rolling back");
          my_error(ER_LOCK_DEADLOCK, MYF(0), "wsrep aborted transaction");
        }
        thd->wsrep_conflict_state= ABORTED;
        wsrep->post_rollback(wsrep, &thd->wsrep_ws_handle);
        break;
      default:
        WSREP_ERROR("trx_replay failed for: %d, query: %s",
                    rcode, thd->query() ? thd->query() : "void");
        /* we're now in inconsistent state, must abort */
        unireg_abort(1);
        break;
      }
      mysql_mutex_lock(&LOCK_wsrep_replaying);
      wsrep_replaying--;
      WSREP_DEBUG("replaying decreased: %d, thd: %lu",
                  wsrep_replaying, thd->thread_id);
      mysql_cond_broadcast(&COND_wsrep_replaying);
      mysql_mutex_unlock(&LOCK_wsrep_replaying);
    }
  }
}

static void wsrep_replication_process(THD *thd)
{
  int rcode;
  DBUG_ENTER("wsrep_replication_process");

  struct wsrep_thd_shadow shadow;
  wsrep_prepare_bf_thd(thd, &shadow);

  rcode = wsrep->recv(wsrep, (void *)thd);
  DBUG_PRINT("wsrep",("wsrep_repl returned: %d", rcode));

  WSREP_INFO("applier thread exiting (code:%d)", rcode);

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
    /* fall through to node shutdown */
  case WSREP_FATAL:
    /* Cluster connectivity is lost.
     *
     * If applier was killed on purpose (KILL_CONNECTION), we
     * avoid mysql shutdown. This is because the killer will then handle
     * shutdown processing (or replication restarting)
     */
    if (thd->killed != THD::KILL_CONNECTION)
    {
      wsrep_kill_mysql(thd);
    }
    break;
  }

  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_close_applier(thd);
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  if (thd->temporary_tables)
  {
    WSREP_DEBUG("Applier %lu, has temporary tables at exit", thd->thread_id);
  }
  wsrep_return_from_bf_mode(thd, &shadow);
  DBUG_VOID_RETURN;
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
  pthread_t hThread;
  while (wsrep_threads++ < threads) {
    if (pthread_create(
      &hThread, &connection_attrib,
      start_wsrep_THD, (void*)wsrep_replication_process))
      WSREP_WARN("Can't create thread to manage wsrep replication");
  }
}

static void wsrep_rollback_process(THD *thd)
{
  DBUG_ENTER("wsrep_rollback_process");

  mysql_mutex_lock(&LOCK_wsrep_rollback);
  wsrep_aborting_thd= NULL;

  while (thd->killed == THD::NOT_KILLED) {
    thd_proc_info(thd, "wsrep aborter idle");
    thd->mysys_var->current_mutex= &LOCK_wsrep_rollback;
    thd->mysys_var->current_cond=  &COND_wsrep_rollback;

    mysql_cond_wait(&COND_wsrep_rollback,&LOCK_wsrep_rollback);

    WSREP_DEBUG("WSREP rollback thread wakes for signal");

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd_proc_info(thd, "wsrep aborter active");
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
      if (aborting->wsrep_conflict_state== ABORTED)
      {
        WSREP_DEBUG("WSREP, thd already aborted: %llu state: %d",
                    (long long)aborting->real_id,
                    aborting->wsrep_conflict_state);

        mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);
        mysql_mutex_lock(&LOCK_wsrep_rollback);
        continue;
      }
      aborting->wsrep_conflict_state= ABORTING;

      mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);

      aborting->store_globals();

      mysql_mutex_lock(&aborting->LOCK_wsrep_thd);
      wsrep_client_rollback(aborting);
      WSREP_DEBUG("WSREP rollbacker aborted thd: (%lu %llu)",
                  aborting->thread_id, (long long)aborting->real_id);
      mysql_mutex_unlock(&aborting->LOCK_wsrep_thd);

      mysql_mutex_lock(&LOCK_wsrep_rollback);
    }
  }

  mysql_mutex_unlock(&LOCK_wsrep_rollback);
  sql_print_information("WSREP: rollbacker thread exiting");

  DBUG_PRINT("wsrep",("wsrep rollbacker thread exiting"));
  DBUG_VOID_RETURN;
}

void wsrep_create_rollbacker()
{
  if (wsrep_provider && strcasecmp(wsrep_provider, "none"))
  {
    pthread_t hThread;
    /* create rollbacker */
    if (pthread_create( &hThread, &connection_attrib,
                        start_wsrep_THD, (void*)wsrep_rollback_process))
      WSREP_WARN("Can't create thread to manage wsrep rollback");
  }
}

extern "C"
int wsrep_thd_is_brute_force(void *thd_ptr)
{
  if (thd_ptr) {
    switch (((THD *)thd_ptr)->wsrep_exec_mode) {
    case LOCAL_STATE:
    {
      if (((THD *)thd_ptr)->wsrep_conflict_state== REPLAYING)
      {
        return 1;
      }
      return 0;
    }
    case REPL_RECV:    return 1;
    case TOTAL_ORDER:  return 2;
    case LOCAL_COMMIT: return 3;
    }
  }
  return 0;
}

extern "C"
int wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr, my_bool signal)
{
  THD *victim_thd = (THD *) victim_thd_ptr;
  THD *bf_thd     = (THD *) bf_thd_ptr;
  DBUG_ENTER("wsrep_abort_thd");

  if ( (WSREP(bf_thd) ||
         ( (WSREP_ON || wsrep_OSU_method_options == WSREP_OSU_RSU) &&
           bf_thd->wsrep_exec_mode == TOTAL_ORDER) )               &&
       victim_thd)
  {
    WSREP_DEBUG("wsrep_abort_thd, by: %llu, victim: %llu", (bf_thd) ?
                (long long)bf_thd->real_id : 0, (long long)victim_thd->real_id);
    ha_wsrep_abort_transaction(bf_thd, victim_thd, signal);
  }
  else
  {
    WSREP_DEBUG("wsrep_abort_thd not effective: %p %p", bf_thd, victim_thd);
  }

  DBUG_RETURN(1);
}

extern "C"
int wsrep_thd_in_locking_session(void *thd_ptr)
{
  if (thd_ptr && ((THD *)thd_ptr)->in_lock_tables) {
    return 1;
  }
  return 0;
}

