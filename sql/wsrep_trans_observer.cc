/* Copyright 2016 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/*
  Implementation of wsrep transaction observer hooks.

  * wsrep_after_row(): called after each row write/update/delete, will run
    SR step
  * wsrep_before_prepare(): SR table cleanup
  * wsrep_after_prepare(): will run wsrep_pre_commit() which replicates
    and certifies transaction for transactions which have registered
    binlog hton
  * wsrep_before_commit(): run wsrep_pre_commit() for autocommit
    DML when binlog_format = STATEMENT
  * wsrep_after_commit(): release wsrep provider critical section
    after successful commit
  * wsrep_before_rollback(): on SR rollback construct SR_trx_info
    and send rollback event before actual rollback happens. Set
    wsrep_exec_mode to LOCAL_ROLLBACK
  * wsrep_after_rollback(): in case of statement rollback check if
    it is safe for SR and if not, trigger full transaction rollback
  * wsrep_after_command():
    * run wsrep_SR_step()
    * perform post rollback operations for thds which have
      wsrep_exec_mode LOCAL_ROLLBACK
      * perform wsrep_client_rollback() for thds with wsrep_conflict_state
        MUST_ABORT or CERT_FAILURE
      * run wsrep_client_rollback() for thd which has seqno assigned
      * cleanup transaction after rollback
    * do rollback process for threads which have BF aborted or have
      failed certification but have not rolled back yet.
    * replay transactions which need to be replayed


  Note: The rollback processing has been postponed to after_command hook
  because sometimes the rollback process needs to be done for threads
  which have valid wsrep seqno and rollback for such a threads should
  be done after all tables are closed in order to avoid deadlocks.
  Although wsrep rollback steps would be possible to do earlier in
  after_rollback hook, this approach was chosen for simplicity.
*/


#include "wsrep_mysqld.h"
#include "wsrep_xid.h"
#include "wsrep_sr.h"
#include "wsrep_thd.h"

#include "replication.h"
#include "transaction.h"
#include "sql_base.h"   /* close_thread_tables() */
#include "sql_class.h"  /* THD */
#include "sql_parse.h"  /* stmt_causes_implicit_commit() */
#include "rpl_filter.h" /* Rpl_filter */
#include "log.h"
#include "debug_sync.h" /* DEBUG_SYNC */

#include <map>



extern class SR_storage *wsrep_SR_store;
extern Rpl_filter* binlog_filter;




static void wsrep_wait_for_replayers(THD *thd)
{
  mysql_mutex_lock(&LOCK_wsrep_replaying);
  int replay_round= 0;
  while (wsrep_replaying > 0                        &&
         thd->wsrep_conflict_state() == NO_CONFLICT &&
         thd->killed == NOT_KILLED             &&
         !shutdown_in_progress)
  {

    mysql_mutex_unlock(&LOCK_wsrep_replaying);
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd_proc_info(thd, "wsrep waiting on replaying");
    thd->mysys_var->current_mutex= &LOCK_wsrep_replaying;
    thd->mysys_var->current_cond=  &COND_wsrep_replaying;
    mysql_mutex_unlock(&thd->mysys_var->mutex);

    mysql_mutex_lock(&LOCK_wsrep_replaying);
    // Using timedwait is a hack to avoid deadlock in case if BF victim
    // misses the signal.
    struct timespec wtime;
    clock_gettime(CLOCK_REALTIME, &wtime);
    long prev_nsec = wtime.tv_nsec;
    wtime.tv_nsec = (wtime.tv_nsec + 1000000) % 1000000000;
    // If nsecs rolled over, increment seconds.
    wtime.tv_sec += (wtime.tv_nsec < prev_nsec ? 1 : 0);
    mysql_cond_timedwait(&COND_wsrep_replaying, &LOCK_wsrep_replaying,
                         &wtime);

    if (replay_round++ % 100000 == 0)
      WSREP_DEBUG("commit waiting for replaying: replayers %d, thd: (%lld) "
                  "conflict: %d (round: %d)",
                  wsrep_replaying, thd->thread_id,
                  thd->wsrep_conflict_state_unsafe(), replay_round);

    mysql_mutex_unlock(&LOCK_wsrep_replaying);

    mysql_mutex_lock(&thd->mysys_var->mutex);
    thd->mysys_var->current_mutex= 0;
    thd->mysys_var->current_cond=  0;
    mysql_mutex_unlock(&thd->mysys_var->mutex);

    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    mysql_mutex_lock(&LOCK_wsrep_replaying);
  }
  mysql_mutex_unlock(&LOCK_wsrep_replaying);
}

static int wsrep_prepare_data_for_replication(THD *thd)
{
  DBUG_ENTER("wsrep_prepare_data_for_replication");
  size_t data_len= 0;
  IO_CACHE* cache= wsrep_get_trans_cache(thd);

  if (cache)
  {
    thd->binlog_flush_pending_rows_event(true);
    enum wsrep_trx_status rcode= wsrep_write_cache(wsrep, thd, cache, &data_len);
    if (rcode != WSREP_TRX_OK)
    {
      WSREP_ERROR("rbr write fail, data_len:: %zu",
                  data_len);
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                           wsrep_trx_status_to_wsrep_status(rcode));
      DBUG_RETURN(1);
    }
  }

  if (data_len == 0)
  {
    if (thd->get_stmt_da()->is_ok()              &&
        thd->get_stmt_da()->affected_rows() > 0  &&
        !binlog_filter->is_on())
    {
      WSREP_DEBUG("empty rbr buffer, query: %s, "
                  "affected rows: %llu, "
                  "changed tables: %d, "
                  "sql_log_bin: %d, "
                  "wsrep status (%d %d %d)",
                  WSREP_QUERY(thd),
                  thd->get_stmt_da()->affected_rows(),
                  stmt_has_updated_trans_table(thd),
                  thd->variables.sql_log_bin,
                  thd->wsrep_exec_mode, thd->wsrep_query_state_unsafe(),
                  thd->wsrep_conflict_state_unsafe());
    }
    else
    {
      WSREP_DEBUG("empty rbr buffer, query: %s", WSREP_QUERY(thd));
    }

    if (!thd->wsrep_is_streaming())
    {
      WSREP_ERROR("I/O error reading from thd's binlog iocache: "
                  "errno=%d, io cache code=%d", my_errno, cache->error);
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

/*
  Run wsrep pre_commit phase

  Asserts thd->LOCK_wsrep_thd ownership
 */
static int wsrep_pre_commit(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ENTER("wsrep_pre_commit");
  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT);
  DBUG_ASSERT(thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID);

  /*
    We must not proceed for pre_commit() if there are threads
    replaying transactions. This is because replaying thread
    may have released some locks which this thread then acquired.

    Now if the replaying ends before write set gets replicated,
    the replayed write set may fall out of this write sets
    certification range, so the conflict won't be detected.
    This will lead to applying error later on.

    Conflict state must be checked out once more after waiting
    for replayers to detect if replaying transaction (or other)
    has BF aborted this.
  */
  wsrep_wait_for_replayers(thd);
  if (thd->wsrep_conflict_state() == MUST_ABORT)
  {
    // Transaction was BF aborted
    wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    DBUG_RETURN(1);
  }

  thd->set_wsrep_query_state(QUERY_COMMITTING);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DEBUG_SYNC(thd, "wsrep_before_replication");

  if (wsrep_prepare_data_for_replication(thd))
  {
    /* Error will be set in function to prepare data */
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_query_state(QUERY_EXEC);
    DBUG_RETURN(1);
  }


  if (thd->killed != NOT_KILLED)
  {
    WSREP_INFO("thd %lld killed with signal %d, skipping replication",
               thd->thread_id, thd->killed);
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    thd->set_wsrep_query_state(QUERY_EXEC);
    DBUG_RETURN(1);
  }

  DBUG_ASSERT(thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID);
  if (WSREP_UNDEFINED_TRX_ID == thd->wsrep_trx_id())
  {
    WSREP_WARN("SQL statement was ineffective, THD: %lld\n"
               "schema: %s \n"
               "QUERY: %s\n"
               " => Skipping replication",
               thd->thread_id,
               (thd->db ? thd->db : "(null)"),
               WSREP_QUERY(thd));
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    wsrep_override_error(thd, ER_ERROR_DURING_COMMIT);
    thd->set_wsrep_query_state(QUERY_EXEC);
    DBUG_RETURN(1);
  }

  /*
    We must make sure that all rollbacks queued in SR rollback queue
    will be replicated before this write set is replicated.
   */
  wsrep_SR_rollback_queue->send_SR_rollbacks(thd);

  uint32_t flags= WSREP_FLAG_TRX_END;
  if (!thd->wsrep_PA_safe || thd->wsrep_is_streaming())
  {
    flags |= WSREP_FLAG_PA_UNSAFE;
  }

  wsrep_status_t rcode=
    wsrep->pre_commit(
                      wsrep,
                      (wsrep_conn_id_t) thd->thread_id,
                      &thd->wsrep_ws_handle,
                      flags,
                      &thd->wsrep_trx_meta);
  WSREP_DEBUG("Trx pre_commit(%lld): rcode %d, seqno %lld, trx %ld, "
              "flags %u, conf %d, SQL: %s",
              thd->thread_id, rcode,(long long)thd->wsrep_trx_meta.gtid.seqno,
              thd->wsrep_trx_meta.stid.trx, flags, thd->wsrep_conflict_state_unsafe(),
              thd->query());

  DBUG_ASSERT((thd->wsrep_trx_meta.depends_on >= 0 &&
               thd->wsrep_trx_meta.depends_on <
               thd->wsrep_trx_meta.gtid.seqno) ||
              WSREP_OK != rcode);

  int ret= 1;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  switch (rcode)
  {
  case WSREP_TRX_MISSING:
    WSREP_WARN("Transaction missing in provider thd: %lld schema: %s SQL: %s",
               thd->thread_id, (thd->db ? thd->db : "(null)"),
               WSREP_QUERY(thd));
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_TRX_MISSING);
    break;
  case WSREP_BF_ABORT:
    DBUG_ASSERT(wsrep_thd_trx_seqno(thd) > 0);
    thd->set_wsrep_conflict_state(MUST_REPLAY);
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    ++wsrep_replaying;
    mysql_mutex_unlock(&LOCK_wsrep_replaying);
    break;
  case WSREP_OK:
    DBUG_ASSERT(wsrep_thd_trx_seqno(thd) > 0);
    /*
      Ignore BF abort from storage engine in commit phase.
      This however requires that the storage engine BF abort
      respects QUERY_COMMITTING query state.

      TODO: Consider removing this and respecting BF abort.
      Also adjust allowed state transitions in
      THD::set_wsrep_conflict_state().
    */
    if (thd->wsrep_conflict_state() == MUST_ABORT)
    {
      thd->killed= NOT_KILLED;
      thd->set_wsrep_conflict_state(NO_CONFLICT);
    }

    thd->wsrep_exec_mode= LOCAL_COMMIT;
    ret= 0;
    wsrep_xid_init(&thd->wsrep_xid, thd->wsrep_trx_meta.gtid.uuid,
                   thd->wsrep_trx_meta.gtid.seqno);
    DBUG_PRINT("wsrep", ("replicating commit success"));
    DBUG_EXECUTE_IF("crash_last_fragment_commit_success",
                    DBUG_SUICIDE(););
    break;
  case WSREP_TRX_FAIL:
    if (thd->wsrep_conflict_state() == NO_CONFLICT)
    {
      thd->set_wsrep_conflict_state(CERT_FAILURE);
      WSREP_LOG_CONFLICT(NULL, thd, FALSE);
    }
    else
    {
      DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_ABORT);
    }
    my_error(ER_LOCK_DEADLOCK, MYF(0), WSREP_TRX_FAIL);
    break;
  case WSREP_SIZE_EXCEEDED:
    WSREP_ERROR("wsrep_run_wsrep_commit(%lld): transaction size exceeded",
                thd->thread_id);
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_SIZE_EXCEEDED);
    break;
  case WSREP_CONN_FAIL:
    WSREP_DEBUG("wsrep_run_wsrep_commit(%lld): replication aborted",
                thd->thread_id);
    my_error(ER_LOCK_DEADLOCK, MYF(0), WSREP_CONN_FAIL);
    break;
  case WSREP_WARNING:
    WSREP_WARN("pre_commit returned warning");
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_WARNING);
    break;
  case WSREP_NODE_FAIL:
    WSREP_ERROR("replication aborted");
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_NODE_FAIL);
    break;
  case WSREP_NOT_IMPLEMENTED:
    WSREP_ERROR("pre_commit not implemented");
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_NOT_IMPLEMENTED);
    break;
  default:
    WSREP_ERROR("wsrep_run_wsrep_commit(%lld): unknown provider failure",
                thd->thread_id);
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), rcode);
    break;
  }

  thd->set_wsrep_query_state(QUERY_EXEC);

  DBUG_RETURN(ret);
}

static wsrep_trx_status wsrep_replicate_fragment(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ENTER("wsrep_replicate_fragment");
  DBUG_ASSERT(thd->wsrep_exec_mode != REPL_RECV &&
              thd->wsrep_exec_mode != TOTAL_ORDER);
  DBUG_ASSERT(thd->wsrep_SR_rollback_replicated_for_trx !=
              thd->wsrep_trx_id());
  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT);

  wsrep_wait_for_replayers(thd);
  if (thd->wsrep_conflict_state() == MUST_ABORT)
  {
    // Transaction was BF aborted
    wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    DBUG_RETURN(WSREP_TRX_ERROR);
  }

  thd->set_wsrep_query_state(QUERY_COMMITTING);

  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  bool reset_trx_meta= false;
  IO_CACHE *cache= wsrep_get_trans_cache(thd);
  size_t data_len= 0;
  uint32_t flags= (thd->wsrep_PA_safe ? 0 : WSREP_FLAG_PA_UNSAFE);
  if (thd->wsrep_fragments_sent == 0)
  {
    flags |= WSREP_FLAG_TRX_START;
  }

  /*
    thd->wsrep_fragments_sent must be incremented before wsrep_write_cache
    to get thd->wsrep_rbr_buf populated. This also needs to be decremented
    whenever error is returned from this function.
   */
  ++thd->wsrep_fragments_sent;
  if (cache)
  {
    enum wsrep_trx_status rcode;
    if ((rcode = wsrep_write_cache(wsrep, thd, cache, &data_len)) != WSREP_TRX_OK)
    {
      WSREP_ERROR("SR rbr write fail, data_len: %zu ret: %d",
                  data_len, rcode);
      --thd->wsrep_fragments_sent;
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
      thd->set_wsrep_query_state(QUERY_EXEC);
      DBUG_RETURN(rcode);
    }
  }

#if 0
  WSREP_INFO("wsrep_replicate_fragment: base: %lu bytes: %u data_len %zu fragments_sent: %lu",
             wsrep_get_fragment_base(thd),
             wsrep_get_trans_cache_position(thd),
             data_len,
             thd->wsrep_fragments_sent);
#endif

  if (data_len == 0)
  {
    if (thd->get_stmt_da()->is_ok()              &&
        thd->get_stmt_da()->affected_rows() > 0  &&
        !binlog_filter->is_on())
    {
      WSREP_WARN("empty rbr buffer, query: %s, "
                 "affected rows: %llu, "
                 "changed tables: %d, "
                 "sql_log_bin: %d, "
                 "wsrep status (%d %d %d)",
                 thd->query(), thd->get_stmt_da()->affected_rows(),
                 stmt_has_updated_trans_table(thd),
                 thd->variables.sql_log_bin,
                 thd->wsrep_exec_mode, thd->wsrep_query_state(),
                 thd->wsrep_conflict_state_unsafe());
    }
    else
    {
      WSREP_WARN("empty rbr buffer, query: %s", thd->query());
    }
    --thd->wsrep_fragments_sent;
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_query_state(QUERY_EXEC);
    DBUG_RETURN(WSREP_TRX_ERROR);
  }

  DBUG_ASSERT(thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID);
  if (WSREP_UNDEFINED_TRX_ID == thd->wsrep_trx_id())
  {
    WSREP_WARN("SQL statement was ineffective, THD: %lld, buf: %zu\n"
               "QUERY: %s\n"
               " => Skipping fragment replication",
               thd->thread_id, data_len, thd->query());
    --thd->wsrep_fragments_sent;
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_query_state(QUERY_EXEC);
    DBUG_RETURN(WSREP_TRX_ERROR);
  }

  THD *SR_thd= NULL;
  if (wsrep_SR_store)
  {
    SR_thd= wsrep_SR_store->append_frag(thd, flags, thd->wsrep_rbr_buf,
                                        data_len);
    if (!SR_thd)
    {
      my_error(ER_BINLOG_ROW_LOGGING_FAILED, MYF(0));
      --thd->wsrep_fragments_sent;
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
      thd->set_wsrep_query_state(QUERY_EXEC);
      DBUG_RETURN(WSREP_TRX_ERROR);
    }
  }

  my_free(thd->wsrep_rbr_buf);
  thd->wsrep_rbr_buf= NULL;

  DBUG_EXECUTE_IF("crash_replicate_fragment_before_pre_commit",
                  DBUG_SUICIDE(););

  wsrep_SR_rollback_queue->send_SR_rollbacks(thd);
  wsrep_status_t rcode= wsrep->pre_commit(wsrep,
                                          (wsrep_conn_id_t)thd->thread_id,
                                          &thd->wsrep_ws_handle,
                                          flags,
                                          &thd->wsrep_trx_meta);


  WSREP_DEBUG("Fragment pre_commit(%lld): rcode %d, seqno %lld, trx %ld, "
              "flags %u, conf %d, SQL: %s",
              thd->thread_id, rcode,(long long)thd->wsrep_trx_meta.gtid.seqno,
              thd->wsrep_trx_meta.stid.trx, flags,
              thd->wsrep_conflict_state_unsafe(),
              thd->query());
  DBUG_EXECUTE_IF("crash_replicate_fragment_after_pre_commit",
                  DBUG_SUICIDE(););

  if (wsrep_thd_trx_seqno(thd) != WSREP_SEQNO_UNDEFINED)
  {
      wsrep_xid_init(&SR_thd->wsrep_xid, thd->wsrep_trx_meta.gtid.uuid,
                     thd->wsrep_trx_meta.gtid.seqno);
      wsrep_SR_store->update_frag_seqno(SR_thd, thd);
      if (rcode == WSREP_OK)
      {
        rcode = wsrep->release(wsrep, &thd->wsrep_ws_handle);
        reset_trx_meta= true;
      }
  }

  wsrep_trx_status ret= WSREP_TRX_ERROR;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);

  /*
    If the SR transaction was BF aborted at this stage
    we abort whole transaction.
  */
  if (thd->wsrep_conflict_state() == MUST_ABORT)
  {
    rcode= WSREP_BF_ABORT;
  }

  switch (rcode)
  {
  case WSREP_OK:
    DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT);
    DBUG_PRINT("wsrep", ("replicating commit success"));
    if (thd->killed != NOT_KILLED)
    {
      WSREP_DEBUG("thd %lld killed with signal %d, during fragment replication",
                thd->thread_id, thd->killed);
    }
    DBUG_EXECUTE_IF("crash_replicate_fragment_success",
                    DBUG_SUICIDE(););
    ret= WSREP_TRX_OK;
    break;
  case WSREP_BF_ABORT:
    if (thd->wsrep_conflict_state() != MUST_ABORT)
    {
      thd->set_wsrep_conflict_state(MUST_ABORT);
    }
    wsrep_override_error(thd, ER_LOCK_DEADLOCK, WSREP_TRX_FAIL);
    ret= WSREP_TRX_ERROR;
    break;
  case WSREP_TRX_FAIL:
    thd->set_wsrep_conflict_state(CERT_FAILURE);
    WSREP_LOG_CONFLICT(NULL, thd, FALSE);
    wsrep_override_error(thd, ER_LOCK_DEADLOCK, WSREP_TRX_FAIL);
    ret= WSREP_TRX_CERT_FAIL;
    break;
  case WSREP_SIZE_EXCEEDED:
    thd->set_wsrep_conflict_state(MUST_ABORT);
    ret= WSREP_TRX_SIZE_EXCEEDED;
    break;
  case WSREP_CONN_FAIL:
    thd->set_wsrep_conflict_state(MUST_ABORT);
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    ret= WSREP_TRX_ERROR;
    break;
  default:
    thd->set_wsrep_conflict_state(MUST_ABORT);
    ret= WSREP_TRX_ERROR;
    break;
  }

  thd->set_wsrep_query_state(QUERY_EXEC);

  if (wsrep_thd_trx_seqno(thd) == WSREP_SEQNO_UNDEFINED)
  {
    --thd->wsrep_fragments_sent;
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    wsrep_SR_store->release_SR_thd(SR_thd);
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->store_globals();
  }

  /*
    Reset trx meta if pre_commit succeeded (regardless of conflict state),
    new seqno will be used for next fragment.
    In case of failure GTID may be required in rollback process.
  */
  if (reset_trx_meta)
  {
    thd->wsrep_trx_meta.gtid= WSREP_GTID_UNDEFINED;
    thd->wsrep_trx_meta.depends_on= WSREP_SEQNO_UNDEFINED;
  }

  DBUG_RETURN(ret);
}

static int wsrep_SR_step(THD *thd, uint unit)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  DBUG_ENTER("wsrep_SR_step");
  if (!wsrep_may_produce_SR_step(thd) ||
      unit != thd->variables.wsrep_trx_fragment_unit ||
      thd->variables.wsrep_trx_fragment_size == 0 ||
      thd->get_stmt_da()->is_error())
  {
#if 0
    WSREP_DEBUG("wsrep_SR_step: skip, frag_size: %lu is_error: %d",
                thd->variables.wsrep_trx_fragment_size,
                thd->get_stmt_da()->is_error());
#endif /* 0 */
    DBUG_RETURN(0);
  }

  /*
    Flush pending rows event into IO cache buffer
   */
  thd->binlog_flush_pending_rows_event(true);

  wsrep_trx_status ret= WSREP_TRX_OK;
  uint written= (wsrep_get_trans_cache_position(thd)
                 - wsrep_get_fragment_base(thd));
  bool replicate= false;

  switch (thd->variables.wsrep_trx_fragment_unit)
  {
  case WSREP_FRAG_BYTES:
    if (written >= thd->variables.wsrep_trx_fragment_size)
      replicate= true;
    break;
  case WSREP_FRAG_ROWS:
  case WSREP_FRAG_STATEMENTS:
    wsrep_append_fill_rate(thd, 1);
    if (wsrep_get_fragment_fill(thd) >= thd->variables.wsrep_trx_fragment_size)
      replicate= true;
    break;
  default:
    wsrep_append_fill_rate(thd, 1);
    replicate= true;
    DBUG_ASSERT(0);
  }

  if (replicate)
  {
    WSREP_DEBUG("fragment fill: %lu fragment unit: %lu fragment size: %lu written: %u",
                wsrep_get_fragment_fill(thd),
                thd->variables.wsrep_trx_fragment_unit,
                thd->variables.wsrep_trx_fragment_size,
                written);

    ret= wsrep_replicate_fragment(thd);
    if (ret == WSREP_TRX_OK)
    {
      wsrep_reset_fragment_fill(thd, 0);
      wsrep_step_fragment_base(thd, written);
    }
  }

  if (ret)
  {
    if (!thd->get_stmt_da()->is_error())
    {
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                           wsrep_trx_status_to_wsrep_status(ret));
    }
  }

  DBUG_RETURN(ret);
}

bool wsrep_replicate_GTID(THD *thd)
{
  if (thd->slave_thread)
  {
    WSREP_DEBUG("GTID replication");
    DBUG_ASSERT (WSREP_UNDEFINED_TRX_ID == thd->wsrep_ws_handle.trx_id);
    thd->set_wsrep_next_trx_id(thd->query_id);
    (void)wsrep_ws_handle_for_trx(&thd->wsrep_ws_handle,
                                  thd->wsrep_next_trx_id());
    DBUG_ASSERT (WSREP_UNDEFINED_TRX_ID != thd->wsrep_ws_handle.trx_id);

    int rcode= wsrep_pre_commit(thd);
    if (rcode)
    {
      WSREP_INFO("GTID replication failed: %d", rcode);
      wsrep->post_rollback(wsrep, &thd->wsrep_ws_handle);
      thd->wsrep_replicate_GTID= false;
      my_message(ER_ERROR_DURING_COMMIT,
                 "WSREP GTID replication was interrupted", MYF(0));

      return true;
    }
  }
  thd->wsrep_replicate_GTID= false;
  return false;
}


/*
  Utility methods to be called from hooks
 */

/*
  Log some THD info and called context
 */
static void wsrep_log_thd(Trans_param *param, const char *function)
{
  THD *thd= param->thd;
  char message[10];
  snprintf(message, sizeof(message), "flags: %x", param->flags);
  message[sizeof(message) - 1] = '\0';
  wsrep_log_thd(thd, message, function);
}


/*
  Determine if the hook should be run

  @return 0 Failure
  @return 1 Success
*/
static int wsrep_run_hook(Trans_param *param, bool for_real_trans)
{
  THD *thd= param->thd;

  return (thd &&         /* THD is non NULL */
          WSREP(thd) &&  /* wsrep is enabled for thd and is client thread */
          thd->wsrep_exec_mode != TOTAL_ORDER && /* not TOI execution */
          thd->wsrep_exec_mode != REPL_RECV &&   /* not applier or replayer */
          (for_real_trans == false || param->flags == TRANS_IS_REAL_TRANS)
          );
}

static inline
int wsrep_is_effective_not_to_replay(THD *thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  int ret= (
            /* effective */
            thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID &&
            /* not to replay */
            thd->wsrep_conflict_state()   != MUST_REPLAY
            );
  return ret;
}


static int wsrep_after_row(Trans_param *param)
{
  DBUG_ENTER("wsrep_after_row");

  /*
    We want to run this hook for each row, not just ones which
    end autocommits or transactions.
   */
  if (!wsrep_run_hook(param, false))
  {
    DBUG_RETURN(0);
  }


#if 0
  /*
    Logging this hook is disabled, it gets very verbose.
  */
  wsrep_log_thd(param, "wsrep_after_row enter");
#endif /* 0 */
  THD *thd= param->thd;
  int ret= 0;

  /*
    TODO: Move this before row operation, maybe before_row() hook.
   */
  if (!wsrep_certify_nonPK)
  {
    for (TABLE* table= thd->open_tables; table != NULL; table= table->next)
    {
      if (table->key_info == NULL || table->s->primary_key == MAX_KEY)
      {
        WSREP_DEBUG("No primary key found for table %s.%s",
                    table->s->db.str, table->s->table_name.str);
        ret= 1;
        break;
      }
    }
  }

  if (!ret)
  {
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    if (thd->wsrep_conflict_state() == NO_CONFLICT)
    {
      ret|= wsrep_SR_step(thd, WSREP_FRAG_BYTES);
      ret|= wsrep_SR_step(thd, WSREP_FRAG_ROWS);
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
#if 0
  /*
    Logging this hook is disabled, it gets very verbose.
   */
  wsrep_log_thd(param, "wsrep_after_row leave");
#endif /* 0 */


  DBUG_RETURN(ret);
}

static int wsrep_before_prepare(Trans_param *param)
{
  DBUG_ENTER("wsrep_before_prepare");
  THD *thd= param->thd;

  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_prepare enter");
  }

  int ret= 0;
  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (wsrep_is_effective_not_to_replay(thd))
  {
    if (thd->wsrep_is_streaming())
    {
      DBUG_EXECUTE_IF("crash_last_fragment_commit_before_fragment_removal",
                      DBUG_SUICIDE(););

      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      /*
        We don't support implicit commit for SR transactions.
       */
      if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_BEGIN))
      {
        wsrep_override_error(thd, ER_ERROR_DURING_COMMIT);
        ret= 1;
      }
      else
      {
        /*
          Disable SR temporarily in order to avoid SR step from
          after_row() hook when deleting fragments.
         */
        ulong frag_size_orig= thd->variables.wsrep_trx_fragment_size;
        thd->variables.wsrep_trx_fragment_size= 0;
        wsrep_remove_SR_fragments(thd);
        thd->variables.wsrep_trx_fragment_size= frag_size_orig;
      }
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
      DBUG_EXECUTE_IF("crash_last_fragment_commit_after_fragment_removal",
                      DBUG_SUICIDE(););
    }
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_prepare leave");
  }
  DBUG_RETURN(ret);
}

static int wsrep_after_prepare(Trans_param *param)
{
  DBUG_ENTER("wsrep_after_prepare");

  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_prepare enter");
  }

  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
  /*
    thd->wsrep_exec_mode will be set in wsrep_pre_commit() according
    to outcome
  */
  int ret= 1;

  if (wsrep_is_effective_not_to_replay(thd))
  {
    if (thd->wsrep_conflict_state() == NO_CONFLICT)
    {
      ret= wsrep_pre_commit(thd);
      if (ret)
      {
        DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_REPLAY ||
                    thd->get_stmt_da()->is_error());
      }
    }
    else
    {
      /*
        BF aborted before pre commit, set state to aborting
        and return error to trigger rollback.
      */
      DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_ABORT);
      wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    }
  }
  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_prepare leave");
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(ret);
}

static int wsrep_before_commit(Trans_param *param)
{
  DBUG_ENTER("wsrep_before_commit");

  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_commit enter");
  }

  int ret= 0;
  if (thd->wsrep_exec_mode == LOCAL_STATE)
  {
    /*
      We got here without having prepare phase first. This may
      happen for example via trans_commit_stmt() -> tc_log->commit(thd, false)
      in case of autocommit DML and binlog_format = STATEMENT.
     */
    if (wsrep_is_effective_not_to_replay(thd))
    {
      if (thd->wsrep_conflict_state() == NO_CONFLICT)
      {
        ret= wsrep_pre_commit(thd);
      }
      else
      {
        /*
          BF aborted before pre commit, set state to aborting
          and return error to trigger rollback.
        */
        DBUG_ASSERT(thd->wsrep_conflict_state() == MUST_ABORT);
        wsrep_override_error(thd, ER_LOCK_DEADLOCK);
        ret= 1;
      }
    }
  }

  DBUG_ASSERT(ret ||
              thd->wsrep_trx_id() == WSREP_UNDEFINED_TRX_ID ||
              thd->wsrep_exec_mode == LOCAL_COMMIT);

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_commit leave");
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(ret);
}

static int wsrep_after_commit(Trans_param *param)
{
  DBUG_ENTER("wsrep_after_commit");

  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_commit enter");
  }

  DBUG_ASSERT(thd->wsrep_trx_id() == WSREP_UNDEFINED_TRX_ID ||
              thd->wsrep_exec_mode == LOCAL_COMMIT);
  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT ||
              thd->wsrep_conflict_state() == MUST_ABORT);

  if (wsrep_is_effective_not_to_replay(thd))
  {
    thd->wsrep_SR_fragments.clear();
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    if (wsrep->release(wsrep, &thd->wsrep_ws_handle))
    {
      WSREP_WARN("wsrep::release fail: %lld %d",
                 (long long)thd->thread_id, thd->get_stmt_da()->status());
    }
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  }

  if (thd->wsrep_conflict_state() == MUST_ABORT)
  {
    WSREP_LOG_THD(thd, "BF aborted at commit phase");
    thd->killed= NOT_KILLED;
    thd->set_wsrep_conflict_state(NO_CONFLICT);
  }

  wsrep_cleanup_transaction(thd);

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_commit leave");
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(0);
}

static int wsrep_before_rollback(Trans_param *param)
{
  DBUG_ENTER("wsrep_before_rollback");

  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_trx_id() == WSREP_UNDEFINED_TRX_ID)
  {
      (void)wsrep_ws_handle_for_trx(&thd->wsrep_ws_handle,
                                    thd->wsrep_next_trx_id());

      if (thd->wsrep_trx_id() == WSREP_UNDEFINED_TRX_ID)
      {
          WSREP_DEBUG("wsrep_before_rollback: setting trx_id to undefined, thd %llu %s",
                      thd->thread_id, thd->query());
      }
  }

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_rollback enter");
  }
  if (thd->wsrep_exec_mode != LOCAL_ROLLBACK &&
      wsrep_is_effective_not_to_replay(thd) &&
      ((param->flags & TRANS_IS_REAL_TRANS) ||
       (thd->wsrep_is_streaming() &&
        (!wsrep_stmt_rollback_is_safe(thd) ||
         thd->wsrep_conflict_state() != NO_CONFLICT))))
  {
    if (thd->wsrep_is_streaming() &&
        /*
          Cert failure will generate implicit rollback event on slaves

          TODO: Need to do SR table cleanup on certification failure here.
        */
        thd->wsrep_conflict_state() != CERT_FAILURE &&
        thd->wsrep_SR_rollback_replicated_for_trx != thd->wsrep_trx_id())
    {
      wsrep_prepare_SR_trx_info_for_rollback(thd);
      thd->wsrep_SR_rollback_replicated_for_trx= thd->wsrep_trx_id();
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      DEBUG_SYNC(thd, "wsrep_before_SR_rollback");
      wsrep_status_t rcode= wsrep->rollback(wsrep,
                                            thd->wsrep_trx_id(), NULL);
      if (rcode != WSREP_OK)
      {
        WSREP_WARN("failed to send SR rollback for %lld", thd->thread_id);
      }
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    }
    thd->wsrep_exec_mode= LOCAL_ROLLBACK;
  }

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_before_rollback leave");
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(0);
}

static int wsrep_after_rollback(Trans_param *param)
{
  DBUG_ENTER("wsrep_after_rollback");

  if (!wsrep_run_hook(param, false))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_rollback enter");
  }

  DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT || /* voluntary or stmt rollback */
              thd->wsrep_conflict_state() == MUST_ABORT ||  /* BF abort */
              thd->wsrep_conflict_state() == ABORTING || /* called from wsrep_client_rollback() */
              thd->wsrep_conflict_state() == CERT_FAILURE || /* cert failure */
              thd->wsrep_conflict_state() == MUST_REPLAY || /* BF abort with successful repl */
              (thd->wsrep_conflict_state() == ABORTED) /* trans_rollback_stmt() from mysql_exec_command() */
              );

  if (!(param->flags & TRANS_IS_REAL_TRANS))
  {
    /*
      Statement rollback
    */
    if ((thd->wsrep_is_streaming() && !wsrep_stmt_rollback_is_safe(thd)))
    {
      /*
        Statement rollback is not safe, do full rollback and report to client.
      */
      if (thd->wsrep_conflict_state() == NO_CONFLICT)
      {
        /*
          If the statement rollback is due to an error, such as ER_DUP_ENTRY,
          the client may not expect a full transaction rollback.
          Set the conflict state to must abort here so that after_command()
          hook will override the error to ER_LOCK_DEADLOCK.
        */
        thd->set_wsrep_conflict_state(MUST_ABORT);
      }

      /*
        From trans_rollback()
      */
      thd->server_status&=
        ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      /*
        Calling ha_rollback_trans() here will call rollback hooks recursively.
      */
      ha_rollback_trans(thd, TRUE);
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
      thd->variables.option_bits&= ~OPTION_BEGIN;
      //thd->transaction.all.reset_unsafe_rollback_flags();
      thd->transaction.all.m_unsafe_rollback_flags= 0;
      thd->lex->start_transaction_opt= 0;
    }
  }
  else
  {
    if (thd->wsrep_conflict_state() == ABORTED)
    {
      thd->wsrep_exec_mode= LOCAL_ROLLBACK;
    }
    if (wsrep_is_effective_not_to_replay(thd))
    {
      /*
        Must have gone through before_rollback() hook at least once.
      */
      DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_ROLLBACK);
    }
  }

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_rollback leave");
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(0);
}

static int wsrep_after_command(Trans_param *param)
{
  DBUG_ENTER("wsrep_after_command");

  /*
    We want to run this hook for each command, not just ones which
    end autocommits or transactions.
  */
  if (!wsrep_run_hook(param, false))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
    wsrep_log_thd(param, "wsrep_after_command enter");
  }

  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE ||
              thd->wsrep_exec_mode == LOCAL_ROLLBACK);

  int ret= 0;

  switch (thd->wsrep_exec_mode)
  {
  case LOCAL_STATE:
    /*
      Run SR step if
      - No conflict detected
      - Trnsaction is active, it has acquired trx_id
      - Not read-only command
    */
    if (thd->wsrep_conflict_state() == NO_CONFLICT  &&
        thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID &&
        thd->lex->sql_command != SQLCOM_SELECT)
    {
      ret|= wsrep_SR_step(thd, WSREP_FRAG_BYTES);
      ret|= wsrep_SR_step(thd, WSREP_FRAG_STATEMENTS);
    }
    break;
  case LOCAL_ROLLBACK:
    {
      bool should_retry= false;
      const bool forced_rollback(thd->wsrep_conflict_state() == MUST_ABORT ||
                                 thd->wsrep_conflict_state() == CERT_FAILURE);

      DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT ||
                  thd->wsrep_conflict_state() == MUST_ABORT ||
                  thd->wsrep_conflict_state() == CERT_FAILURE);
      /*
        If conflict state is NO_CONFLICT the transaction was either
        voluntary or done due to deadlock.
      */
      if (forced_rollback)
      {
        should_retry= !(
          thd->wsrep_is_streaming() || // SR transactions do not retry
          thd->spcont                  // SP code not patched to handle retry
          );
        wsrep_client_rollback(thd, false);
      }
      wsrep_post_rollback(thd);
      if (forced_rollback)
      {
        wsrep_override_error(thd, ER_LOCK_DEADLOCK);
      }
      wsrep_cleanup_transaction(thd);
      DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
      DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT);
      /*
        Retry autocommit in case of deadlock error, usually seen
        as ER_LOCK_DEADLOCK, sometimes as ER_QUERY_INTERRUPTED.
       */
      if (should_retry && thd->get_stmt_da()->is_error() &&
          (thd->get_stmt_da()->sql_errno() == ER_LOCK_DEADLOCK ||
           thd->get_stmt_da()->sql_errno() == ER_QUERY_INTERRUPTED))
      {
        thd->set_wsrep_conflict_state(RETRY_AUTOCOMMIT);
      }
    }
    break;
  default:
    break;
  }

  if (thd->wsrep_conflict_state() == MUST_ABORT ||
      thd->wsrep_conflict_state() == CERT_FAILURE)
  {
    wsrep_client_rollback(thd);
    wsrep_post_rollback(thd);
    wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    wsrep_cleanup_transaction(thd);
  }

  if (thd->wsrep_conflict_state() == MUST_REPLAY &&
      !thd->spcont)
  {
    /*
      BF aborted during commit, must replay
    */
    wsrep_replay_transaction(thd);
  }

  if (thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID)
  {
      wsrep_log_thd(param, "wsrep_after_command leave");
  }


  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(ret);
}

static int wsrep_before_GTID_binlog(Trans_param *param)
{
  DBUG_ENTER("wsrep_before_GTID_binlogt");
  int ret= 0;
  
  if (!wsrep_run_hook(param, true))
  {
    DBUG_RETURN(0);
  }

  THD* thd= param->thd;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (wsrep_replicate_GTID(thd)) ret = 1;
  
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  if (wsrep->release(wsrep, &thd->wsrep_ws_handle))
  {
    WSREP_WARN("wsrep::release fail: %lld %d",
               (long long)thd->thread_id, thd->get_stmt_da()->status());
  }
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  wsrep_cleanup_transaction(thd);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_RETURN(ret);
}

Trans_observer wsrep_trans_observer =
{
  sizeof(Trans_observer),
  wsrep_before_prepare,
  wsrep_before_commit,
  wsrep_before_rollback,
  wsrep_after_row,
  wsrep_after_prepare,
  wsrep_after_commit,
  wsrep_after_rollback,
  wsrep_after_command,
  wsrep_before_GTID_binlog
};

int wsrep_register_trans_observer(void *p)
{
  int ret= register_trans_observer(&wsrep_trans_observer, p);
  WSREP_INFO("wsrep_register_trans_observer: %d", ret);
  return ret;
}

int wsrep_unregister_trans_observer(void *p)
{
  int ret= unregister_trans_observer(&wsrep_trans_observer, p);
  WSREP_INFO("wsrep_unregister_trans_observer: %d", ret);
  if (ret)
  {
    WSREP_ERROR("wsrep_unregister_trans_observer failed");
  }
  return ret;
}
