/* Copyright (C) 2013-2015 Codership Oy <info@codership.com>

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
#include "wsrep_priv.h"
#include "wsrep_binlog.h" // wsrep_dump_rbr_buf()
#include "wsrep_xid.h"
#include "wsrep_sr.h"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"

#include "slave.h" // opt_log_slave_updates
#include "log_event.h" // class THD, EVENT_LEN_OFFSET, etc.
#include "wsrep_applier.h"
#include "debug_sync.h"

/*
  read the first event from (*buf). The size of the (*buf) is (*buf_len).
  At the end (*buf) is shitfed to point to the following event or NULL and
  (*buf_len) will be changed to account just being read bytes of the 1st event.
*/

static Log_event* wsrep_read_log_event(
    char **arg_buf, size_t *arg_buf_len,
    const Format_description_log_event *description_event)
{
  DBUG_ENTER("wsrep_read_log_event");
  char *head= (*arg_buf);

  uint data_len = uint4korr(head + EVENT_LEN_OFFSET);
  char *buf= (*arg_buf);
  const char *error= 0;
  Log_event *res=  0;

  res= Log_event::read_log_event(buf, data_len, &error, description_event,
                                 true);

  if (!res)
  {
    DBUG_ASSERT(error != 0);
    sql_print_error("Error in Log_event::read_log_event(): "
                    "'%s', data_len: %d, event_type: %d",
                    error,data_len,head[EVENT_TYPE_OFFSET]);
  }
  (*arg_buf)+= data_len;
  (*arg_buf_len)-= data_len;
  DBUG_RETURN(res);
}

#include "transaction.h" // trans_commit(), trans_rollback()
#include "rpl_rli.h"     // class Relay_log_info;

void wsrep_set_apply_format(THD* thd, Format_description_log_event* ev)
{
  if (thd->wsrep_apply_format)
  {
    delete (Format_description_log_event*)thd->wsrep_apply_format;
  }
  thd->wsrep_apply_format= ev;
}

Format_description_log_event* wsrep_get_apply_format(THD* thd)
{
  if (thd->wsrep_apply_format)
  {
    return (Format_description_log_event*) thd->wsrep_apply_format;
  }

  DBUG_ASSERT(thd->wsrep_rgi);

  return thd->wsrep_rgi->rli->relay_log.description_event_for_exec;
}

void wsrep_apply_error::store(const THD* const thd)
{
  Diagnostics_area::Sql_condition_iterator it=
    thd->get_stmt_da()->sql_conditions();
  const Sql_condition* cond;

  static size_t const max_len= 2*MAX_SLAVE_ERRMSG; // 2x so that we have enough

  if (NULL == str_)
  {
    // this must be freeable by standard free()
    str_= static_cast<char*>(malloc(max_len));
    if (NULL == str_)
    {
      WSREP_ERROR("Failed to allocate %zu bytes for error buffer.", max_len);
      len_ = 0;
      return;
    }
  }
  else
  {
    /* This is possible when we invoke rollback after failed applying.
     * In this situation DA should not be reset yet and should contain
     * all previous errors from applying and new ones from rollbacking,
     * so we just overwrite is from scratch */
  }

  char* slider= str_;
  const char* const buf_end= str_ + max_len - 1; // -1: leave space for \0

  for (cond= it++; cond && slider < buf_end; cond= it++)
  {
    uint const err_code= cond->get_sql_errno();
    const char* const err_str= cond->get_message_text();

    slider+= my_snprintf(slider, buf_end - slider, " %s, Error_code: %d;",
                         err_str, err_code);
  }

  *slider= '\0';
  len_= slider - str_ + 1; // +1: add \0

  WSREP_DEBUG("Error buffer for thd %llu seqno %lld, %zu bytes: %s",
              thd->thread_id, (long long)wsrep_thd_trx_seqno(thd),
              len_, str_ ? str_ : "(null)");
}

int wsrep_apply_events(THD*        thd,
                       const void* events_buf,
                       size_t      buf_len)
{
  char *buf= (char *)events_buf;
  int rcode= 0;
  int event= 1;
  Log_event_type typ;

  DBUG_ENTER("wsrep_apply_events");

  if (thd->killed == KILL_CONNECTION &&
      thd->wsrep_conflict_state() != REPLAYING)
  {
    WSREP_INFO("applier has been aborted, skipping apply_rbr: %lld",
               (long long) wsrep_thd_trx_seqno(thd));
    DBUG_RETURN(WSREP_ERR_ABORTED);
  }

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  // thd->set_wsrep_query_state(QUERY_EXEC);
  if (thd->wsrep_conflict_state() !=  REPLAYING)
  {
    DBUG_ASSERT(thd->wsrep_conflict_state() == NO_CONFLICT);
    if (thd->wsrep_conflict_state() != NO_CONFLICT)
    {
      thd->set_wsrep_conflict_state(NO_CONFLICT);
    }
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  if (!buf_len) WSREP_DEBUG("empty rbr buffer to apply: %lld",
                            (long long) wsrep_thd_trx_seqno(thd));

  while(buf_len)
  {
    int exec_res;
    Log_event* ev= wsrep_read_log_event(&buf, &buf_len,
                                        wsrep_get_apply_format(thd));

    if (!ev)
    {
      WSREP_ERROR("applier could not read binlog event, seqno: %lld, len: %zu",
                  (long long)wsrep_thd_trx_seqno(thd), buf_len);
      rcode= WSREP_ERR_BAD_EVENT;
      goto error;
    }

    typ= ev->get_type_code();

    switch (typ) {
    case FORMAT_DESCRIPTION_EVENT:
      wsrep_set_apply_format(thd, (Format_description_log_event*)ev);
      continue;
#ifdef GTID_SUPPORT
    case GTID_LOG_EVENT:
    {
      Gtid_log_event* gev= (Gtid_log_event*)ev;
      if (gev->get_gno() == 0)
      {
        /* Skip GTID log event to make binlog to generate LTID on commit */
        delete ev;
        continue;
      }
    }
#endif /* GTID_SUPPORT */
    default:
      break;
    }

    /* Use the original server id for logging. */
    thd->set_server_id(ev->server_id);
    thd->set_time();                            // time the query
    thd->lex->current_select= 0;
    if (!ev->when)
    {
      my_hrtime_t hrtime= my_hrtime();
      ev->when= hrtime_to_my_time(hrtime);
      ev->when_sec_part= hrtime_sec_part(hrtime);
    }

    thd->variables.option_bits=
      (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
      (ev->flags & LOG_EVENT_SKIP_REPLICATION_F ?  OPTION_SKIP_REPLICATION : 0);

    ev->thd = thd;
    exec_res = ev->apply_event(thd->wsrep_rgi);
    DBUG_PRINT("info", ("exec_event result: %d", exec_res));

    if (exec_res)
    {
      WSREP_WARN("Event %d %s apply failed: %d, seqno %lld",
                 event, ev->get_type_str(), exec_res,
                 (long long) wsrep_thd_trx_seqno(thd));
      rcode= exec_res;
      /* stop processing for the first error */
      delete ev;
      goto error;
    }
    event++;

    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    if (thd->wsrep_conflict_state() != NO_CONFLICT &&
        thd->wsrep_conflict_state() != REPLAYING)
      WSREP_WARN("conflict state after RBR event applying: %d, %lld",
                 thd->wsrep_query_state(), (long long)wsrep_thd_trx_seqno(thd));

    if (thd->wsrep_conflict_state() == MUST_ABORT) {
      WSREP_WARN("Event apply failed, rolling back: %lld",
                 (long long) wsrep_thd_trx_seqno(thd));
      trans_rollback(thd);
      thd->locked_tables_list.unlock_locked_tables(thd);
      /* Release transactional metadata locks. */
      thd->mdl_context.release_transactional_locks();
      thd->set_wsrep_conflict_state(NO_CONFLICT);
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
      DBUG_RETURN(rcode ? rcode : WSREP_ERR_ABORTED);
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

    delete_or_keep_event_post_apply(thd->wsrep_rgi, typ, ev);
  }

 error:
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  assert(thd->wsrep_exec_mode== REPL_RECV);

  if (thd->killed == KILL_CONNECTION)
    WSREP_INFO("applier aborted: %lld", (long long)wsrep_thd_trx_seqno(thd));

  DBUG_RETURN(rcode);
}

static wsrep_SR_trx_info* wsrep_prepare_applier_ctx(
    uint32_t                const flags,
    const wsrep_trx_meta_t* const meta,
    THD**                         thd)
{
  DBUG_ENTER("wsrep_prepare_applier_ctx");

  THD* const orig_thd(*thd);
  wsrep_SR_trx_info* SR_trx(NULL);

  if ((flags & WSREP_FLAG_TRX_START) &&
      (flags & WSREP_FLAG_TRX_END || flags & WSREP_FLAG_ROLLBACK))
  {
    /* non-SR trx */
    (*thd)->store_globals();
  }
  else if (!(flags & WSREP_FLAG_TRX_START))
  {
    /* continuation of SR trx */
    SR_trx = sr_pool->find(meta->stid.node, meta->stid.trx);
    if (NULL == SR_trx)
    {
      WSREP_DEBUG("SR transaction has been aborted already.");
      goto out;
    }
    /* SR trx write sets cannot be applied in parallel */
    assert(SR_trx->get_applier_thread() == 0);

    /* switch THD contexts */
    SR_trx->set_applier_thread(orig_thd->thread_id);
    *thd = SR_trx->get_THD();
    (*thd)->thread_stack = orig_thd->thread_stack;
    WSREP_DEBUG("fragment trx found, thread_id: %lld", (*thd)->thread_id);
    (*thd)->store_globals();
  }
  else
  {
    assert(flags & WSREP_FLAG_TRX_START);
    assert(!(flags & WSREP_FLAG_ROLLBACK));

    /* starting new streaming replication trx, prepare a new context */
    WSREP_DEBUG("WS with begin and not commit flag");
    *thd = wsrep_start_SR_THD(orig_thd->thread_stack);
    SR_trx = sr_pool->add(meta->stid.node, meta->stid.trx, *thd);
  }

  assert(*thd);
  (*thd)->wsrep_trx_meta = *meta;

out:
  DBUG_RETURN(SR_trx);
}

static inline void wsrep_restore_applier_ctx(wsrep_SR_trx_info* const SR_trx,
                                             THD* const               orig_thd)
{
  DBUG_ENTER("wsrep_restore_applier_ctx");
  WSREP_DEBUG("resetting default thd for applier, id: %lld, thd: %p",
              orig_thd->thread_id, orig_thd);
  SR_trx->set_applier_thread(0);
  orig_thd->store_globals();
  DBUG_VOID_RETURN;
}


static wsrep_cb_status_t wsrep_rollback_common(THD* thd, wsrep_apply_error& err)
{
  DBUG_ENTER("wsrep_rollback_common");
  wsrep_cb_status_t rcode(WSREP_CB_SUCCESS);

  WSREP_DEBUG("Slave rolling back %lld", (long long)wsrep_thd_trx_seqno(thd));
//#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "rolling back %lld", (long long)wsrep_thd_trx_seqno(thd));
  thd_proc_info(thd, thd->wsrep_info);
//#else
//  thd_proc_info(thd, "rolling back");
//#endif /* WSREP_PROC_INFO */

  if (thd->wsrep_SR_thd && wsrep_SR_store)
  {
    /* prevent rollbacker to abort the same thd */
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_conflict_state(MUST_ABORT);
    thd->set_wsrep_conflict_state(ABORTING);
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

    /* rollback SR trx */
    if (trans_rollback_stmt(thd) || trans_rollback(thd))
    {
      rcode = WSREP_CB_FAILURE;
      err.store(thd);
    }
  }

//#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "rolled back %lld", (long long)wsrep_thd_trx_seqno(thd));
  thd_proc_info(thd, thd->wsrep_info);
//#else
//  thd_proc_info(thd, "rolled back");
//#endif /* WSREP_PROC_INFO */
  thd->wsrep_rgi->cleanup_context(thd, 0);
#ifdef GTID_SUPPORT
  thd->variables.gtid_next.set_automatic();
#endif
  DBUG_RETURN(rcode);
}


static wsrep_cb_status_t wsrep_rollback_trx(THD*                    thd,
                                            const void* const       buf,
                                            size_t const            buf_len,
                                            uint32_t const          flags,
                                            const wsrep_trx_meta_t* const meta,
                                            wsrep_apply_error&      err)
{
  DBUG_ENTER("wsrep_rollback_trx");

  assert(flags & WSREP_FLAG_ROLLBACK);

  THD* const orig_thd(thd);

  wsrep_SR_trx_info* const SR_trx(wsrep_prepare_applier_ctx(flags, meta, &thd));

  WSREP_DEBUG("Rollback for: %ld", meta->stid.trx);

  // Preempted by rollbacker
  if (!SR_trx && !(flags & WSREP_FLAG_TRX_START))
  {
    WSREP_DEBUG("Pre-empted by rollbacker: %ld",
                meta->stid.trx);
    DBUG_RETURN(WSREP_CB_SUCCESS);
  }

  if (SR_trx)
  {
    /*
      TODO: Must be done atomically with writing dummy event, should be
      moved into wsrep_rollback_comman().
     */
    wsrep_rollback_SR_trx(thd);
  }

  wsrep_cb_status_t const rcode(wsrep_rollback_common(thd, err));

  if (SR_trx)
  {
    wsrep_restore_applier_ctx(SR_trx, orig_thd);
  }

  DBUG_RETURN(rcode);
}

static int wsrep_apply_trx(THD*                    orig_thd,
                           const void*       const buf,
                           size_t            const buf_len,
                           uint32_t          const flags,
                           const wsrep_trx_meta_t* meta,
                           wsrep_apply_error&      err)
{
  THD* thd= orig_thd;
  DBUG_ENTER("wsrep_apply_trx");

  DBUG_ASSERT(thd->wsrep_apply_toi == false);
  DBUG_ASSERT(!(flags & WSREP_FLAG_ROLLBACK));
  DBUG_ASSERT(!(flags & WSREP_FLAG_ISOLATION));
  DBUG_ASSERT(err.is_null());

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_conflict_state() == REPLAYING &&
      (flags & WSREP_FLAG_TRX_END) && !(flags & WSREP_FLAG_TRX_START))
  {
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    /*
      Replaying, must add final frag to SR storage for actual replay
    */
    if (wsrep_SR_store)
    {
      wsrep_SR_store->append_frag_commit(thd,
                                         flags,
                                         (const uchar*)buf,
                                         buf_len);
      DBUG_RETURN(wsrep_replay_from_SR_store(thd, *meta));
    }
    DBUG_RETURN(0);
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  wsrep_SR_trx_info* const SR_trx(wsrep_prepare_applier_ctx(flags, meta, &thd));

  assert(thd);

  /* moved dbug sync point here, after possible THD switch for SR transactions
     has ben done
  */
  // Allow tests to block the applier thread using the DBUG facilities
  DBUG_EXECUTE_IF("sync.wsrep_apply_cb",
                 {
                   const char act[]=
                     "now "
                     "SIGNAL sync.wsrep_apply_cb_reached "
                     "WAIT_FOR signal.wsrep_apply_cb";
                   DBUG_ASSERT(!debug_sync_set_action(thd,
                                                      STRING_WITH_LEN(act)));
                 };);

  /* tune FK and UK checking policy */
  if (wsrep_slave_UK_checks == FALSE)
    thd->variables.option_bits|= OPTION_RELAXED_UNIQUE_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_RELAXED_UNIQUE_CHECKS;

  if (wsrep_slave_FK_checks == FALSE)
    thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;
  else
    thd->variables.option_bits&= ~OPTION_NO_FOREIGN_KEY_CHECKS;

  /*
    Actual applying is done in wsrep_apply_events()
  */
  int rcode(wsrep_apply_events(thd, buf, buf_len));

  if (0 != rcode || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf_with_header(thd, buf, buf_len);
  }

  if (0 != rcode)
  {
    err.store(thd);
    wsrep_rollback_common(thd, err);
  }
  else if (thd->wsrep_SR_thd && wsrep_SR_store)
  {
    /* remove trx from persistent storage on last fragment */
    if ((flags & WSREP_FLAG_TRX_END))
    {
      WSREP_DEBUG("last fragment, cleanup SR table ");
      DBUG_EXECUTE_IF("crash_apply_cb_before_fragment_removal",
                      DBUG_SUICIDE(););
      SR_trx->cleanup();
      DBUG_EXECUTE_IF("crash_apply_cb_after_fragment_removal",
                      DBUG_SUICIDE(););
    }
    else
    {
      DBUG_EXECUTE_IF("crash_apply_cb_before_append_frag",
                      DBUG_SUICIDE(););
      SR_trx->append_fragment(meta);
      /* store the fragment in persistent storage */
      WSREP_DEBUG("append fragment to SR table");
      orig_thd->store_globals();
      wsrep_SR_store->append_frag_apply(orig_thd,
                                        flags,
                                        (const uchar*)buf,
                                        buf_len);
      thd->store_globals();
      DBUG_EXECUTE_IF("crash_apply_cb_after_append_frag",
                      DBUG_SUICIDE(););
    }
  }

  thd->close_temporary_tables();

  if (SR_trx)
  {
    if (rcode)
    {
      WSREP_DEBUG("fragment trx destruction");
//gcf487      trans_rollback(thd); - done in wsrep_rollback_common()
//      close_thread_tables(thd);
//      sr_pool->remove(orig_thd, thd, true);
    }
    wsrep_restore_applier_ctx(SR_trx, orig_thd);
  }
  DBUG_RETURN(rcode);
}


static int wsrep_apply_toi(THD*        const thd,
                           const void* const buf,
                           size_t      const buf_len,
                           uint32_t    const flags,
                           const wsrep_trx_meta_t* const meta,
                           wsrep_apply_error& err)
{
  DBUG_ENTER("wsrep_apply_toi");

  DBUG_ASSERT(flags & WSREP_FLAG_ISOLATION);


  thd->wsrep_trx_meta = *meta;

  thd->wsrep_apply_toi= true;

  /*
    Don't run in transaction mode with TOI actions. These will be
    reset to defaults in commit callback.
  */
  thd->variables.option_bits&= ~OPTION_BEGIN;
  thd->server_status&= ~SERVER_STATUS_IN_TRANS;

  thd->store_globals();

  int const rcode(wsrep_apply_events(thd, buf, buf_len));

  if (0 != rcode || thd->wsrep_has_ignored_error)
  {
    wsrep_dump_rbr_buf_with_header(thd, buf, buf_len);
    thd->wsrep_has_ignored_error= false;

    if (0 != rcode) err.store(thd);
  }

  thd->close_temporary_tables();

  DBUG_RETURN(rcode);
}


static void wsrep_apply_non_blocking(THD* thd, void* args)
{
  /* Go to BF mode */
  wsrep_thd_shadow shadow;
  wsrep_prepare_bf_thd(thd, &shadow);

  Wsrep_nbo_ctx* nbo_ctx= (Wsrep_nbo_ctx*) args;

  DBUG_ASSERT(nbo_ctx);

  /*
    Record context for synchronizing with applier thread once
    MDL locks have been aquired.
  */
  thd->wsrep_nbo_ctx= nbo_ctx;

  /* Must be applier thread */
  assert(thd->wsrep_exec_mode == REPL_RECV);

  /* Set OSU method for non blocking */
  thd->variables.wsrep_OSU_method= WSREP_OSU_NBO;

  /*
    First apply TOI action as usual, wsrep->to_execute_start() will
    be called at stmt commit. Applying is always assumed to succeed here.
    Non-blocking operation end is supposed to sync the end result with
    group and abort if the result does not match.
  */
  wsrep_apply_error err;
  if (wsrep_apply_toi(thd, nbo_ctx->buf(), nbo_ctx->buf_len(), nbo_ctx->flags(),
                      &nbo_ctx->meta(), err) != WSREP_CB_SUCCESS)
  {
    WSREP_DEBUG("Applying NBO failed");
  }

  /*
    Applying did not cause action to signal slave thread. Wake up
    the slave before exit.
  */
  if (!thd->wsrep_nbo_ctx->ready()) thd->wsrep_nbo_ctx->signal();

  /* Non-blocking operation end. No need to take action on error here,
     it is handled by provider. */
  wsrep_buf_t const err_buf= err.get_buf();
  if (thd->wsrep_trx_meta.gtid.seqno != WSREP_SEQNO_UNDEFINED &&
      wsrep->to_execute_end(wsrep, thd->thread_id, &err_buf) != WSREP_OK)
  {
    WSREP_WARN("Non-blocking operation end failed");
  }

  if (wsrep->free_connection(wsrep, thd->thread_id) != WSREP_OK)
  {
    WSREP_WARN("Failed to free connection: %llu",
               (long long unsigned)thd->thread_id);
  }

  thd->wsrep_nbo_ctx= NULL;
  delete nbo_ctx;

  /* Return from BF mode before syncing with group */
  wsrep_return_from_bf_mode(thd, &shadow);
}

static int start_nbo_thd(const void*             const buf,
                         size_t                  const buf_len,
                         uint32_t                const flags,
                         const wsrep_trx_meta_t* const meta)
{
  int rcode= WSREP_RET_SUCCESS;

  // Non-blocking operation start
  Wsrep_nbo_ctx* nbo_ctx= 0;
  Wsrep_thd_args* args= 0;
  try
  {
    nbo_ctx= new Wsrep_nbo_ctx(buf, buf_len, flags, *meta);
    args= new Wsrep_thd_args(wsrep_apply_non_blocking, nbo_ctx);
    pthread_t th;
    int err;
    int max_tries= 1000;
    while ((err= pthread_create(&th, &connection_attrib, start_wsrep_THD,
                                args)) == EAGAIN)
    {
      --max_tries;
      if (max_tries == 0)
      {
        delete nbo_ctx;
        delete args;
        WSREP_ERROR("Failed to create thread for non-blocking operation: %d",
                    err);
        return WSREP_ERR_FAILED;
      }
      else
      {
        usleep(1000);
      }
    }

    // Detach thread and wait until worker signals that it has locked
    // required resources.
    pthread_detach(th);
    nbo_ctx->wait_sync();
    // nbo_ctx will be deleted by worker thread
  }
  catch (...)
  {
    delete nbo_ctx;
    delete args;
    WSREP_ERROR("Caught exception while trying to create thread for "
                "non-blocking operation");
    rcode= WSREP_ERR_FAILED;
  }

  return rcode;
}

int wsrep_apply(void* const              ctx,
                uint32_t const           flags,
                const wsrep_buf_t* const buf,
                const wsrep_trx_meta_t*  meta,
                wsrep_apply_error&       err)
{
  THD* thd= (THD*)ctx;

  DBUG_ENTER("wsrep_apply");

  DBUG_ASSERT(!WSREP_NBO_END(flags) || WSREP_FLAG_ROLLBACK & flags);

  thd->wsrep_trx_meta = *meta;

#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "applying write set %lld: %p, %zu",
           (long long)wsrep_thd_trx_seqno(thd), buf->ptr, buf->len);
  thd_proc_info(thd, thd->wsrep_info);
#else
  thd_proc_info(thd, "Applying write set");
#endif /* WSREP_PROC_INFO */

  WSREP_DEBUG("apply_cb with flags: %u seqno: %ld, srctrx: %ld",
              flags, meta->gtid.seqno, meta->stid.trx);

  bool const rollback(WSREP_FLAG_ROLLBACK & flags);
  int rcode= 0;

  if (!(flags & WSREP_FLAG_ISOLATION))
  {
    // Transaction
    if (!rollback)
    {
      DBUG_ASSERT(meta->stid.trx != uint64_t(-1));
      rcode= wsrep_apply_trx(thd, buf->ptr, buf->len, flags, meta, err);
    }
    else
    {
      DBUG_ASSERT(meta->stid.trx != uint64_t(-1) ||
                  (NULL == buf->ptr && 0 == buf->len));
      rcode= wsrep_rollback_trx(thd, buf->ptr, buf->len, flags, meta, err);
    }
    DBUG_ASSERT(rcode == 0 || !err.is_null());
    DBUG_ASSERT(err.is_null() || rcode != 0);
  }
  else if (WSREP_NBO_START(flags))
  {
    // NBO-start
    if (!rollback)
      rcode = start_nbo_thd(buf->ptr, buf->len, flags, meta);
    else
    {
      WSREP_DEBUG("Failed NBO start, seqno: %lld",
                  (long long)wsrep_thd_trx_seqno(thd));
      rcode= wsrep_rollback_common(thd, err);
    }
  }
  else if (WSREP_NBO_END(flags))
  {
    // ineffective NBO-end - binlog something for it
    assert(rollback);
    rcode= wsrep_rollback_common(thd, err);
  }
  else
  {
    // Regular TOI
    if (!rollback)
      rcode= wsrep_apply_toi(thd, buf->ptr, buf->len, flags, meta, err);
    else
      rcode= wsrep_rollback_common(thd, err);
  }

  
#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "Applied write set %lld", (long long)wsrep_thd_trx_seqno(thd));
  thd_proc_info(thd, thd->wsrep_info);
#else
  thd_proc_info(thd, "Applied write set");
#endif /* WSREP_PROC_INFO */
  WSREP_DEBUG("apply_cb done with rcode: %d", rcode);

  DBUG_RETURN(rcode != 0 ? rcode : err.length());
}

static wsrep_cb_status_t wsrep_commit_thd(THD* const thd,
                                          bool fragment_not_trx_end,
                                          const wsrep_apply_error& err)
{
#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "Committing %lld", (long long)wsrep_thd_trx_seqno(thd));
  thd_proc_info(thd, thd->wsrep_info);
#else
  thd_proc_info(thd, "Committing");
#endif /* WSREP_PROC_INFO */

  wsrep_cb_status_t rcode= WSREP_CB_SUCCESS;
  if (!opt_log_slave_updates &&
      thd->wsrep_apply_toi &&
      wsrep_before_commit(thd, true))
    rcode= WSREP_CB_FAILURE;

  /*
    We can ignore the storage engine durability setting for fragments
    here: Committing a fragment does not cause actual transaction to
    be committed, so it will be enough that the fragment is
    committed in order to be able to recover to consistent state.
   */

  enum durability_properties dur_save= thd->durability_property;
  if (fragment_not_trx_end)
    thd->durability_property= HA_IGNORE_DURABILITY;

  if (rcode == WSREP_CB_SUCCESS && trans_commit(thd))
    rcode= WSREP_CB_FAILURE;

  thd->durability_property= dur_save;

  if (WSREP_CB_SUCCESS == rcode)
  {
    thd->wsrep_rgi->cleanup_context(thd, false);
#ifdef GTID_SUPPORT
    thd->variables.gtid_next.set_automatic();
#endif /* GTID_SUPPORT */
    if (thd->wsrep_apply_toi)
    {
      wsrep_set_SE_checkpoint(thd->wsrep_trx_meta.gtid.uuid,
                              thd->wsrep_trx_meta.gtid.seqno);
    }
    if ((!opt_log_slave_updates || thd->wsrep_apply_toi)
        && wsrep_ordered_commit(thd, true, err))
      rcode= WSREP_CB_FAILURE;

    if (rcode == WSREP_CB_SUCCESS)
    {
      DBUG_ASSERT(fragment_not_trx_end ||
                  thd->wsrep_query_state_unsafe() == QUERY_ORDERED_COMMIT);
      /*
        FIXME: For some reason fragment commits don't go through
        binlog commit group commit if log slave updates is on. This
        effectively disables binlog group commit for dummy events
        which are logged into binlog.
       */
      if (fragment_not_trx_end && opt_log_slave_updates && !wsrep_gtid_mode)
      {
        if (wsrep_before_commit(thd, true) ||
            wsrep_ordered_commit(thd, true, err))
        {
          WSREP_DEBUG("Binlog commit failed (WCT)");
          rcode= WSREP_CB_FAILURE;
        }
      }
    }
  }

#ifdef WSREP_PROC_INFO
  snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
           "Committed %lld", (long long) wsrep_thd_trx_seqno(thd));
  thd_proc_info(thd, thd->wsrep_info);
#else
  thd_proc_info(thd, "Committed");
#endif /* WSREP_PROC_INFO */

  return rcode;
}

static
wsrep_cb_status_t wsrep_commit(void*         const     ctx,
                               uint32_t      const     flags,
                               const wsrep_trx_meta_t* meta,
                               wsrep_bool_t* const     exit,
                               const wsrep_apply_error& err)
{
  DBUG_ENTER("wsrep_commit_cb");
  WSREP_DEBUG("commit_cb with flags: %u seqno: %ld, srctrx: %ld",
              flags, meta->gtid.seqno, meta->stid.trx);

  THD* thd((THD*)ctx);
  wsrep_cb_status_t rcode= WSREP_CB_SUCCESS;
  bool const trx_end(flags & (WSREP_FLAG_TRX_END | WSREP_FLAG_ROLLBACK));

  if (WSREP_NBO_START(flags))
  {
    DBUG_RETURN(WSREP_CB_SUCCESS);
  }

  bool const toi(flags & WSREP_FLAG_ISOLATION); // ineffective NBO end
  bool const fragment(!((flags & WSREP_FLAG_TRX_START) && trx_end) && !toi);
  const wsrep_SR_trx_info* const SR_trx
      (fragment ? sr_pool->find(meta->stid.node, meta->stid.trx) : NULL);

  if (!SR_trx && trx_end && fragment)
  {
    WSREP_DEBUG("SR trxid %ld seqno %lld has been aborted already, skipping "
                "commit_cb", meta->stid.trx, (long long)meta->gtid.seqno);
  }
  else
  {
    assert(0 == SR_trx || fragment);
    assert(!fragment || 0 != SR_trx);
  }

  if (SR_trx && trx_end)
  {
    void* opaque= thd->wsrep_ws_handle.opaque;
    thd = SR_trx->get_THD();
    WSREP_DEBUG("last trx fragment, switching context from %p to %p", ctx, thd);
    thd->wsrep_ws_handle.opaque= opaque;
    thd->store_globals();
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_query_state(QUERY_EXEC);
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    DBUG_EXECUTE_IF("crash_commit_cb_before_last_fragment_commit",
                    DBUG_SUICIDE(););
  }

  thd->wsrep_trx_meta = *meta;
  wsrep_xid_init(&thd->wsrep_xid, meta->gtid.uuid, meta->gtid.seqno);
  assert(meta->gtid.seqno == wsrep_thd_trx_seqno(thd));

  /*
    Failed certification, write a dummy binlog event in wsrep_gtid_mode
    to keep GTID sequence continuous. If wsrep_gtid_mode is off, release
    commit critical section.
  */
  if (!fragment && (flags & WSREP_FLAG_ROLLBACK))
  {
    if (wsrep_gtid_mode)
    {
      wsrep_write_dummy_event(thd, "rollback");
    }
    else
    {
      if (wsrep_before_commit(thd, true) ||
          wsrep_ordered_commit(thd, true, err))
      {
        WSREP_DEBUG("Binlog commit failed (WC)");
        rcode= WSREP_CB_FAILURE;
      }
    }
  }
  else
  {
    /* here if thd == SR_trx->get_thd() we are committing in SR context -
     * that is SR transaction.
     * otherwise we are committing in the usual applier context -
     * that is regular transaction or SR fragment. */
    rcode = wsrep_commit_thd(thd, fragment && !trx_end, err);
  }

  thd->wsrep_ws_handle.opaque= NULL;
  thd->wsrep_xid.null();

  wsrep_set_apply_format(thd, NULL);
  thd->mdl_context.release_transactional_locks();
  thd->reset_query();                           /* Mutex protected */
  free_root(thd->mem_root,MYF(MY_KEEP_PREALLOC));
  thd->tx_isolation= (enum_tx_isolation) thd->variables.tx_isolation;

  if (wsrep_slave_count_change < 0 && trx_end && WSREP_CB_SUCCESS == rcode)
  {
    mysql_mutex_lock(&LOCK_wsrep_slave_threads);
    if (wsrep_slave_count_change < 0)
    {
      wsrep_slave_count_change++;
      *exit = true;
    }
    mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  }

  if (thd->wsrep_applier)
  {
    /* From trans_begin() */
    thd->variables.option_bits|= OPTION_BEGIN;
    thd->server_status|= SERVER_STATUS_IN_TRANS;
    thd->wsrep_apply_toi= false;
  }

  /* Cleanup for streaming replication */
  if (SR_trx && trx_end)
  {
    SR_trx->release();

    /* Cleanup for streaming replication */
    if (trx_end)
    {
      sr_pool->remove((THD*)ctx, meta->stid.node, meta->stid.trx, false);
      DBUG_EXECUTE_IF("crash_commit_cb_last_fragment_commit_success",
                      DBUG_SUICIDE(););
      /* Set thd to null as it is deleted in sr_pool->remove() */
      thd= NULL;
    }
  }

  if (thd)
  {
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    thd->set_wsrep_query_state(QUERY_EXEC);
    if (thd->wsrep_conflict_state() != REPLAYING)
    {
      thd->set_wsrep_query_state(QUERY_IDLE);
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  if (ctx != thd)
  {
    thd= (THD*)ctx;
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    if (thd->wsrep_conflict_state() != REPLAYING)
    {
      thd->set_wsrep_query_state(QUERY_IDLE);
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }

  DBUG_RETURN(rcode);
}

wsrep_cb_status_t wsrep_apply_cb(void* const              ctx,
                                 const wsrep_ws_handle_t* ws_handle,
                                 uint32_t                 flags,
                                 const wsrep_buf_t* const buf,
                                 const wsrep_trx_meta_t*  meta,
                                 wsrep_bool_t*            exit_loop)
{
  assert(meta->gtid.seqno > 0);

  THD *thd= (THD*)ctx;
  void* opaque_save= thd->wsrep_ws_handle.opaque;
  thd->wsrep_ws_handle.opaque= ws_handle->opaque;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_conflict_state() != REPLAYING)
    thd->set_wsrep_query_state(QUERY_EXEC);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  wsrep_apply_error err;
  int const apply_err= wsrep_apply(ctx, flags, buf, meta, err);
  DBUG_ASSERT(0 == apply_err || !err.is_null());
  DBUG_ASSERT(0 != apply_err || err.is_null());

  if (0 != apply_err) flags |= WSREP_FLAG_ROLLBACK;

  wsrep_cb_status_t rcode= wsrep_commit(ctx, flags, meta, exit_loop, err);

  thd->wsrep_ws_handle.opaque= opaque_save;

  DBUG_ASSERT(thd->wsrep_conflict_state_unsafe() == REPLAYING ||
              thd->wsrep_query_state_unsafe() == QUERY_IDLE);

  return rcode;
}

wsrep_cb_status_t wsrep_unordered_cb(void*              const ctx,
                                     const wsrep_buf_t* const data)
{
    return WSREP_CB_SUCCESS;
}
 
