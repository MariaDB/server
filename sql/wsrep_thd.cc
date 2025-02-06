/* Copyright (C) 2013-2023 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#include "mariadb.h"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"
#include "wsrep_high_priority_service.h"
#include "wsrep_storage_service.h"
#include "wsrep_server_state.h"
#include "transaction.h"
#include "rpl_rli.h"
#include "log_event.h"
#include "sql_parse.h"
#include "wsrep_mysqld.h"   // start_wsrep_THD();
#include "mysql/service_wsrep.h"
#include "debug_sync.h"
#include "slave.h"
#include "rpl_rli.h"
#include "rpl_mi.h"


static Wsrep_thd_queue* wsrep_rollback_queue= 0;
static Atomic_counter<uint64_t> wsrep_bf_aborts_counter;


int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, void *, system_status_var *,
                          enum enum_var_type scope)
{
  wsrep_local_bf_aborts= wsrep_bf_aborts_counter;
  var->type= SHOW_LONGLONG;
  var->value= (char*)&wsrep_local_bf_aborts;
  return 0;
}

static void wsrep_replication_process(THD *thd,
                                      void* arg __attribute__((unused)))
{
  DBUG_ENTER("wsrep_replication_process");

  Wsrep_applier_service applier_service(thd);

  WSREP_INFO("Starting applier thread %llu", thd->thread_id);
  enum wsrep::provider::status
    ret= Wsrep_server_state::get_provider().run_applier(&applier_service);

  WSREP_INFO("Applier thread exiting ret: %d thd: %llu", ret, thd->thread_id);
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  wsrep_close_applier(thd);
  mysql_cond_broadcast(&COND_wsrep_slave_threads);
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);

  delete thd->wsrep_rgi->rli->mi;
  delete thd->wsrep_rgi->rli;
  
  thd->wsrep_rgi->cleanup_after_session();
  delete thd->wsrep_rgi;
  thd->wsrep_rgi= NULL;


  if(thd->has_thd_temporary_tables())
  {
    WSREP_WARN("Applier %lld has temporary tables at exit.",
               thd->thread_id);
  }
  DBUG_VOID_RETURN;
}

static bool create_wsrep_THD(Wsrep_thd_args* args, bool mutex_protected)
{
  if (!mutex_protected)
    mysql_mutex_lock(&LOCK_wsrep_slave_threads);

  ulong old_wsrep_running_threads= wsrep_running_threads;

  DBUG_ASSERT(args->thread_type() == WSREP_APPLIER_THREAD ||
              args->thread_type() == WSREP_ROLLBACKER_THREAD);

  bool res= mysql_thread_create(args->thread_type() == WSREP_APPLIER_THREAD
                                ? key_wsrep_applier : key_wsrep_rollbacker,
                                args->thread_id(), &connection_attrib,
                                start_wsrep_THD, (void*)args);

  if (res)
    WSREP_ERROR("Can't create wsrep thread");

  /*
    if starting a thread on server startup, wait until the this thread's THD
    is fully initialized (otherwise a THD initialization code might
    try to access a partially initialized server data structure - MDEV-8208).
  */
  if (!mysqld_server_initialized)
  {
    while (old_wsrep_running_threads == wsrep_running_threads)
    {
      mysql_cond_wait(&COND_wsrep_slave_threads, &LOCK_wsrep_slave_threads);
    }
  }

  if (!mutex_protected)
    mysql_mutex_unlock(&LOCK_wsrep_slave_threads);

  return res;
}

bool wsrep_create_appliers(long threads, bool mutex_protected)
{
  /*  Dont' start slave threads if wsrep-provider or wsrep-cluster-address
      is not set.
  */
  if (!WSREP_PROVIDER_EXISTS)
  {
    return false;
  }

  DBUG_ASSERT(wsrep_cluster_address[0]);

  long wsrep_threads=0;

  while (wsrep_threads++ < threads)
  {
    Wsrep_thd_args* args(new Wsrep_thd_args(wsrep_replication_process,
                                            WSREP_APPLIER_THREAD,
                                            pthread_self()));
    if (create_wsrep_THD(args, mutex_protected))
    {
      WSREP_WARN("Can't create thread to manage wsrep replication");
      return true;
    }
  }

  return false;
}

static void wsrep_remove_streaming_fragments(THD* thd, const char* ctx)
{
  wsrep::transaction_id transaction_id(thd->wsrep_trx().id());
  Wsrep_storage_service* storage_service= wsrep_create_storage_service(thd, ctx);
  storage_service->store_globals();
  storage_service->adopt_transaction(thd->wsrep_trx());
  storage_service->remove_fragments();
  storage_service->commit(wsrep::ws_handle(transaction_id, 0),
                          wsrep::ws_meta());
  Wsrep_server_state::instance().server_service()
    .release_storage_service(storage_service);
  wsrep_store_threadvars(thd);
}

static void wsrep_rollback_high_priority(THD *thd, THD *rollbacker)
{
  WSREP_DEBUG("Rollbacker aborting SR applier thd (%llu %lu)",
              thd->thread_id, thd->real_id);
  void* orig_thread_stack= thd->thread_stack;
  thd->thread_stack= rollbacker->thread_stack;
  DBUG_ASSERT(thd->wsrep_cs().mode() == Wsrep_client_state::m_high_priority);
  /* Must be streaming and must have been removed from the
     server state streaming appliers map. */
  DBUG_ASSERT(thd->wsrep_trx().is_streaming());
  DBUG_ASSERT(!Wsrep_server_state::instance().find_streaming_applier(
                thd->wsrep_trx().server_id(),
                thd->wsrep_trx().id()));
  DBUG_ASSERT(thd->wsrep_applier_service);

  /* Fragment removal should happen before rollback to make
     the transaction non-observable in SR table after the rollback
     completes. For correctness the order does not matter here,
     but currently it is mandated by checks in some MTR tests. */
  wsrep_remove_streaming_fragments(thd, "high priority");
  thd->wsrep_applier_service->rollback(wsrep::ws_handle(),
                                       wsrep::ws_meta());
  thd->wsrep_applier_service->after_apply();
  thd->thread_stack= orig_thread_stack;
  WSREP_DEBUG("rollbacker aborted thd: (%llu %lu)",
              thd->thread_id, thd->real_id);
  /* Will free THD */
  Wsrep_server_state::instance().server_service()
    .release_high_priority_service(thd->wsrep_applier_service);
}

static void wsrep_rollback_local(THD *thd, THD *rollbacker)
{
  WSREP_DEBUG("Rollbacker aborting local thd (%llu %lu)",
              thd->thread_id, thd->real_id);
  void* orig_thread_stack= thd->thread_stack;
  thd->thread_stack= rollbacker->thread_stack;
  if (thd->wsrep_trx().is_streaming())
  {
    wsrep_remove_streaming_fragments(thd, "local");
  }
  /* Set thd->event_scheduler.data temporarily to NULL to avoid
     callbacks to threadpool wait_begin() during rollback. */
  auto saved_esd= thd->event_scheduler.data;
  thd->event_scheduler.data= 0;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  /* prepare THD for rollback processing */
  thd->reset_for_next_command();
  thd->lex->sql_command= SQLCOM_ROLLBACK;
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  /* Perform a client rollback, restore globals and signal
     the victim only when all the resources have been
     released */
  thd->wsrep_cs().client_service().bf_rollback();
  wsrep_reset_threadvars(thd);
  /* Assign saved event_scheduler.data back before letting
     client to continue. */
  thd->event_scheduler.data= saved_esd;
  thd->thread_stack= orig_thread_stack;
  thd->wsrep_cs().sync_rollback_complete();
  WSREP_DEBUG("rollbacker aborted thd: (%llu %lu)",
              thd->thread_id, thd->real_id);
}

static void wsrep_rollback_process(THD *rollbacker,
                                   void *arg __attribute__((unused)))
{
  DBUG_ENTER("wsrep_rollback_process");

  THD* thd= NULL;
  DBUG_ASSERT(!wsrep_rollback_queue);
  wsrep_rollback_queue= new Wsrep_thd_queue(rollbacker);
  WSREP_INFO("Starting rollbacker thread %llu", rollbacker->thread_id);

  thd_proc_info(rollbacker, "wsrep aborter idle");
  while ((thd= wsrep_rollback_queue->pop_front()) != NULL)
  {
    mysql_mutex_lock(&thd->LOCK_thd_data);
    wsrep::client_state& cs(thd->wsrep_cs());
    const wsrep::transaction& tx(cs.transaction());
    if (tx.state() == wsrep::transaction::s_aborted)
    {
      WSREP_DEBUG("rollbacker thd already aborted: %llu state: %d",
                  (long long)thd->real_id,
                  tx.state());
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      continue;
    }
    mysql_mutex_unlock(&thd->LOCK_thd_data);

    wsrep_reset_threadvars(rollbacker);
    wsrep_store_threadvars(thd);
    thd->wsrep_cs().acquire_ownership();

    thd_proc_info(rollbacker, "wsrep aborter active");

    /* Rollback methods below may free thd pointer. Do not try
       to access it after method returns. */
    if (wsrep_thd_is_applying(thd))
    {
      wsrep_rollback_high_priority(thd, rollbacker);
    }
    else
    {
      wsrep_rollback_local(thd, rollbacker);
    }
    wsrep_store_threadvars(rollbacker);
    thd_proc_info(rollbacker, "wsrep aborter idle");
  }

  delete wsrep_rollback_queue;
  wsrep_rollback_queue= NULL;

  WSREP_INFO("rollbacker thread exiting %llu", rollbacker->thread_id);

  DBUG_ASSERT(rollbacker->killed != NOT_KILLED);
  DBUG_PRINT("wsrep",("wsrep rollbacker thread exiting"));
  DBUG_VOID_RETURN;
}

void wsrep_create_rollbacker()
{
  DBUG_ASSERT(wsrep_cluster_address[0]);
  Wsrep_thd_args* args(new Wsrep_thd_args(wsrep_rollback_process,
                                          WSREP_ROLLBACKER_THREAD,
                                          pthread_self()));

  /* create rollbacker */
  if (create_wsrep_THD(args, false))
    WSREP_WARN("Can't create thread to manage wsrep rollback");
}

/*
  Start async rollback process

  Asserts thd->LOCK_thd_data ownership
 */
void wsrep_fire_rollbacker(THD *thd)
{
  DBUG_ASSERT(thd->wsrep_trx().state() == wsrep::transaction::s_aborting);
  DBUG_PRINT("wsrep",("enqueuing trx abort for %llu", thd->thread_id));
  WSREP_DEBUG("enqueuing trx abort for (%llu)", thd->thread_id);
  if (wsrep_rollback_queue->push_back(thd))
  {
    WSREP_WARN("duplicate thd %llu for rollbacker",
               thd->thread_id);
  }
}

static bool wsrep_bf_abort_low(THD *bf_thd, THD *victim_thd)
{
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);

#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("sync.wsrep_bf_abort",
                  {
                    const char act[]=
                      "now "
                      "SIGNAL sync.wsrep_bf_abort_reached "
                      "WAIT_FOR signal.wsrep_bf_abort";
                    DBUG_ASSERT(!debug_sync_set_action(bf_thd,
                                                       STRING_WITH_LEN(act)));
                  };);
#endif

  wsrep::seqno bf_seqno(bf_thd->wsrep_trx().ws_meta().seqno());
  bool ret;

  {
    /* Adopt the lock, it is being held by the caller. */
    Wsrep_mutex wsm{&victim_thd->LOCK_thd_data};
    wsrep::unique_lock<wsrep::mutex> lock{wsm, std::adopt_lock};

    if (wsrep_thd_is_toi(bf_thd))
    {
      ret= victim_thd->wsrep_cs().total_order_bf_abort(lock, bf_seqno);
    }
    else
    {
      DBUG_ASSERT(WSREP(victim_thd) ? victim_thd->wsrep_trx().active() : 1);
      ret= victim_thd->wsrep_cs().bf_abort(lock, bf_seqno);
    }
    if (ret)
    {
      /* BF abort should be allowed only once by wsrep-lib.*/
      DBUG_ASSERT(victim_thd->wsrep_aborter == 0);
      victim_thd->wsrep_aborter= bf_thd->thread_id;
      wsrep_bf_aborts_counter++;
    }
    lock.release(); /* No unlock at the end of the scope. */
  }

  /* Sanity check for wsrep-lib calls to return with LOCK_thd_data held. */
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);

  return ret;
}

void wsrep_abort_thd(THD *bf_thd,
                    THD *victim_thd,
                    my_bool signal)
{
  DBUG_ENTER("wsrep_abort_thd");

  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_kill);
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);

  /* Note that when you use RSU node is desynced from cluster, thus WSREP(thd)
  might not be true.
  */
  if ((WSREP(bf_thd)
       || ((WSREP_ON || bf_thd->variables.wsrep_OSU_method == WSREP_OSU_RSU)
           &&  wsrep_thd_is_toi(bf_thd))
       || bf_thd->lex->sql_command == SQLCOM_KILL)
      && !wsrep_thd_is_aborting(victim_thd) &&
      wsrep_bf_abort_low(bf_thd, victim_thd) &&
      !victim_thd->wsrep_cs().is_rollbacker_active())
  {
    WSREP_DEBUG("wsrep_abort_thd, by: %llu, victim: %llu",
                (long long)bf_thd->real_id, (long long)victim_thd->real_id);
    victim_thd->awake_no_mutex(KILL_QUERY_HARD);
    ha_abort_transaction(bf_thd, victim_thd, signal);
  }
  else
  {
    WSREP_DEBUG("wsrep_abort_thd not effective: bf %llu victim %llu "
                "wsrep %d wsrep_on %d RSU %d TOI %d aborting %d",
                (long long)bf_thd->real_id, (long long)victim_thd->real_id,
                WSREP_NNULL(bf_thd), WSREP_ON,
                bf_thd->variables.wsrep_OSU_method == WSREP_OSU_RSU,
                wsrep_thd_is_toi(bf_thd),
                wsrep_thd_is_aborting(victim_thd));
  }

  DBUG_VOID_RETURN;
}

bool wsrep_bf_abort(THD* bf_thd, THD* victim_thd)
{
  WSREP_LOG_THD(bf_thd, "BF aborter before");
  WSREP_LOG_THD(victim_thd, "victim before");

  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);

  if (WSREP(victim_thd) && !victim_thd->wsrep_trx().active())
  {
    WSREP_DEBUG("wsrep_bf_abort, BF abort for non active transaction."
                " Victim state %s bf state %s",
                wsrep::to_c_string(victim_thd->wsrep_trx().state()),
                wsrep::to_c_string(bf_thd->wsrep_trx().state()));

    switch (victim_thd->wsrep_trx().state()) {
    case wsrep::transaction::s_aborting: /* fall through */
    case wsrep::transaction::s_aborted:
      WSREP_DEBUG("victim is aborting or has aborted");
      break;
    default: break;
    }
    /* victim may not have started transaction yet in wsrep context, but it may
       have acquired MDL locks (due to DDL execution), and this has caused BF conflict.
       such case does not require aborting in wsrep or replication provider state.
    */
    if (victim_thd->current_backup_stage != BACKUP_FINISHED &&
        wsrep_check_mode(WSREP_MODE_BF_MARIABACKUP))
    {
      WSREP_DEBUG("killing connection for non wsrep session");
      victim_thd->awake_no_mutex(KILL_CONNECTION);
    }
    return false;
  }

  return wsrep_bf_abort_low(bf_thd, victim_thd);
}

uint wsrep_kill_thd(THD *thd, THD *victim_thd, killed_state kill_signal)
{
  DBUG_ENTER("wsrep_kill_thd");
  DBUG_ASSERT(WSREP(victim_thd));
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_kill);
  mysql_mutex_assert_owner(&victim_thd->LOCK_thd_data);
  using trans= wsrep::transaction;
  auto trx_state= victim_thd->wsrep_trx().state();
#ifndef DBUG_OFF
  victim_thd->wsrep_killed_state= trx_state;
#endif /* DBUG_OFF */
  /*
    Already killed or in commit codepath. Mark the victim as killed,
    the killed status will be restored in wsrep_after_commit() and
    will be processed after the commit is over. In case of multiple
    KILLs happened on commit codepath, the last one will be effective.
  */
  if (victim_thd->wsrep_abort_by_kill ||
      trx_state == trans::s_preparing ||
      trx_state == trans::s_committing ||
      trx_state == trans::s_ordered_commit)
  {
    victim_thd->wsrep_abort_by_kill= kill_signal;
    DBUG_RETURN(0);
  }
  /*
    Mark killed victim_thd with kill_signal so that awake_no_mutex does
    not dive into storage engine. We use ha_abort_transaction()
    to do the storage engine part for wsrep THDs.
  */
  DEBUG_SYNC(thd, "wsrep_kill_before_awake_no_mutex");
  victim_thd->wsrep_abort_by_kill= kill_signal;
  victim_thd->awake_no_mutex(kill_signal);
  /* ha_abort_transaction() releases tmp->LOCK_thd_kill, so tmp
     is not safe to access anymore. */
  ha_abort_transaction(thd, victim_thd, 1);
  DBUG_RETURN(0);
}

void wsrep_backup_kill_for_commit(THD *thd)
{
  DBUG_ASSERT(WSREP(thd));
  mysql_mutex_assert_owner(&thd->LOCK_thd_kill);
  DBUG_ASSERT(thd->killed != NOT_KILLED);
  mysql_mutex_lock(&thd->LOCK_thd_data);
  /* If the transaction will roll back, keep the killed state.
     For must replay, the replay will happen in different THD context
     which is high priority and cannot be killed. The owning thread will
     pick the killed state in after statement processing. */
  if (thd->wsrep_trx().state() != wsrep::transaction::s_cert_failed &&
      thd->wsrep_trx().state() != wsrep::transaction::s_must_abort &&
      thd->wsrep_trx().state() != wsrep::transaction::s_aborting &&
      thd->wsrep_trx().state() != wsrep::transaction::s_must_replay)
  {
    thd->wsrep_abort_by_kill= thd->killed;
    my_free(thd->wsrep_abort_by_kill_err);
    thd->wsrep_abort_by_kill_err= thd->killed_err;
    thd->killed= NOT_KILLED;
    thd->killed_err= 0;
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}

void wsrep_restore_kill_after_commit(THD *thd)
{
  DBUG_ASSERT(wsrep_is_active(thd));
  mysql_mutex_assert_owner(&thd->LOCK_thd_kill);
  thd->killed= thd->wsrep_abort_by_kill;
  my_free(thd->killed_err);
  thd->killed_err= thd->wsrep_abort_by_kill_err;
  thd->wsrep_abort_by_kill= NOT_KILLED;
  thd->wsrep_abort_by_kill_err= 0;
}

int wsrep_create_threadvars()
{
  int ret= 0;
  if (thread_handling == SCHEDULER_TYPES_COUNT)
  {
    /* Caller should have called wsrep_reset_threadvars() before this
       method. */
    DBUG_ASSERT(!my_thread_var);
    set_mysys_var(0);
    ret= my_thread_init();
  }
  return ret;
}

void wsrep_delete_threadvars()
{
  if (thread_handling == SCHEDULER_TYPES_COUNT)
  {
    /* The caller should have called wsrep_store_threadvars() before
       this method. */
    DBUG_ASSERT(my_thread_var);
    /* Reset psi state to avoid deallocating applier thread
       psi_thread. */
#ifdef HAVE_PSI_INTERFACE
    PSI_thread *psi_thread= PSI_CALL_get_thread();
    if (PSI_server)
    {
      PSI_server->set_thread(0);
    }
#endif /* HAVE_PSI_INTERFACE */
    my_thread_end();
    PSI_CALL_set_thread(psi_thread);
    set_mysys_var(0);
  }
}

void wsrep_assign_from_threadvars(THD *thd)
{
  if (thread_handling == SCHEDULER_TYPES_COUNT)
  {
    thd->set_mysys_var(my_thread_var);
  }
}

Wsrep_threadvars wsrep_save_threadvars()
{
  return Wsrep_threadvars{
    current_thd,
    my_thread_var
  };
}

void wsrep_restore_threadvars(const Wsrep_threadvars& globals)
{
  set_current_thd(globals.cur_thd);
  set_mysys_var(globals.mysys_var);
}

void wsrep_store_threadvars(THD *thd)
{
  if (thread_handling ==  SCHEDULER_TYPES_COUNT)
  {
    set_mysys_var(thd->mysys_var);
  }
  thd->store_globals();
}

void wsrep_reset_threadvars(THD *thd)
{
  if (thread_handling == SCHEDULER_TYPES_COUNT)
  {
    set_mysys_var(0);
  }
  else
  {
    thd->reset_globals();
  }
}
