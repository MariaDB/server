/* Copyright 2016-2019 Codership Oy <http://www.codership.com>

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

#ifndef WSREP_TRANS_OBSERVER_H
#define WSREP_TRANS_OBSERVER_H

#include "my_global.h"
#include "mysql/service_wsrep.h"
#include "wsrep_applier.h" /* wsrep_apply_error */
#include "wsrep_xid.h"
#include "wsrep_thd.h"
#include "wsrep_binlog.h" /* register/deregister group commit */
#include "my_dbug.h"

class THD;

void wsrep_commit_empty(THD* thd, bool all);

/*
   Return true if THD has active wsrep transaction.
 */
static inline bool wsrep_is_active(THD* thd)
{
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none  &&
          thd->wsrep_cs().transaction().active());
}

/*
  Return true if transaction is ordered.
 */
static inline bool wsrep_is_ordered(THD* thd)
{
  return thd->wsrep_trx().ordered();
}

/*
  Return true if transaction has been BF aborted but has not been
  rolled back yet.

  It is required that the caller holds thd->LOCK_thd_data.
*/
static inline bool wsrep_must_abort(THD* thd)
{
  mysql_mutex_assert_owner(&thd->LOCK_thd_data);
  return (thd->wsrep_trx().state() == wsrep::transaction::s_must_abort);
}

/*
  Return true if the transaction must be replayed.
 */
static inline bool wsrep_must_replay(THD* thd)
{
  return (thd->wsrep_trx().state() == wsrep::transaction::s_must_replay);
}
/*
  Return true if transaction has not been committed.

  Note that we don't require thd->LOCK_thd_data here. Calling this method
  makes sense only from codepaths which are past ordered_commit state
  and the wsrep transaction is immune to BF aborts at that point.
*/
static inline bool wsrep_not_committed(THD* thd)
{
  return (thd->wsrep_trx().state() != wsrep::transaction::s_committed);
}

/*
  Return true if THD is either committing a transaction or statement
  is autocommit.
 */
static inline bool wsrep_is_real(THD* thd, bool all)
{
  return (all || thd->transaction->all.ha_list == 0);
}

/*
  Check if a transaction has generated changes.
 */
static inline bool wsrep_has_changes(THD* thd)
{
  return (thd->wsrep_trx().is_empty() == false);
}

/*
  Check if an active transaction has been BF aborted.
 */
static inline bool wsrep_is_bf_aborted(THD* thd)
{
  return (thd->wsrep_trx().active() && thd->wsrep_trx().bf_aborted());
}

static inline int wsrep_check_pk(THD* thd)
{
  if (!wsrep_certify_nonPK)
  {
    for (TABLE* table= thd->open_tables; table != NULL; table= table->next)
    {
      if (table->key_info == NULL || table->s->primary_key == MAX_KEY)
      {
        WSREP_DEBUG("No primary key found for table %s.%s",
                    table->s->db.str, table->s->table_name.str);
        wsrep_override_error(thd, ER_LOCK_DEADLOCK);
        return 1;
      }
    }
  }
  return 0;
}

static inline bool wsrep_streaming_enabled(THD* thd)
{
  return (thd->wsrep_sr().fragment_size() > 0);
}

/*
  Return number of fragments successfully certified for the
  current statement.
 */
static inline size_t wsrep_fragments_certified_for_stmt(THD* thd)
{
    return thd->wsrep_trx().fragments_certified_for_statement();
}

static inline int wsrep_start_transaction(THD* thd, wsrep_trx_id_t trx_id)
{
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none  ?
          thd->wsrep_cs().start_transaction(wsrep::transaction_id(trx_id)) :
          0);
}

/**/
static inline int wsrep_start_trx_if_not_started(THD* thd)
{
  int ret= 0;
  DBUG_ASSERT(thd->wsrep_next_trx_id() != WSREP_UNDEFINED_TRX_ID);
  DBUG_ASSERT(thd->wsrep_cs().mode() == Wsrep_client_state::m_local);
  if (thd->wsrep_trx().active() == false)
  {
    ret= wsrep_start_transaction(thd, thd->wsrep_next_trx_id());
  }
  return ret;
}

/*
  Called after each row operation.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_row(THD* thd, bool)
{
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none  &&
      wsrep_thd_is_local(thd))
  {
    if (wsrep_check_pk(thd))
    {
      return 1;
    }
    else if (wsrep_streaming_enabled(thd))
    {
      return thd->wsrep_cs().after_row();
    }
  }
  return 0;
}

/*
  Helper method to determine whether commit time hooks
  should be run for the transaction.

  Commit hooks must be run in the following cases:
  - The transaction is local and has generated write set and is committing.
  - The transaction has been BF aborted
  - Is running in high priority mode and is ordered. This can be replayer,
    applier or storage access.
 */
static inline bool wsrep_run_commit_hook(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_run_commit_hook");
  DBUG_PRINT("wsrep", ("Is_active: %d is_real %d has_changes %d is_applying %d "
                       "is_ordered: %d",
                       wsrep_is_active(thd), wsrep_is_real(thd, all),
                       wsrep_has_changes(thd), wsrep_thd_is_applying(thd),
                       wsrep_is_ordered(thd)));
  /* Is MST commit or autocommit? */
  bool ret= wsrep_is_active(thd) && wsrep_is_real(thd, all);
  /* Do not commit if we are aborting */
  ret= ret && (thd->wsrep_trx().state() != wsrep::transaction::s_aborting);
  if (ret && !(wsrep_has_changes(thd) ||  /* Has generated write set */
               /* Is high priority (replay, applier, storage) and the
                  transaction is scheduled for commit ordering */
               (wsrep_thd_is_applying(thd) && wsrep_is_ordered(thd))))
  {
    mysql_mutex_lock(&thd->LOCK_thd_data);
    DBUG_PRINT("wsrep", ("state: %s",
                         wsrep::to_c_string(thd->wsrep_trx().state())));
    /* Transaction is local but has no changes, the commit hooks will
       be skipped and the wsrep transaction is terminated in
       wsrep_commit_empty() */
    if (thd->wsrep_trx().state() == wsrep::transaction::s_executing)
    {
      ret= false;
    }
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  DBUG_PRINT("wsrep", ("return: %d", ret));
  DBUG_RETURN(ret);
}

/*
  Called before the transaction is prepared.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_prepare(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_before_prepare");
  WSREP_DEBUG("wsrep_before_prepare: %d", wsrep_is_real(thd, all));
  int ret= 0;
  DBUG_ASSERT(wsrep_run_commit_hook(thd, all));
  if ((ret= thd->wsrep_cs().before_prepare()) == 0)
  {
    DBUG_ASSERT(!thd->wsrep_trx().ws_meta().gtid().is_undefined());
    wsrep_xid_init(&thd->wsrep_xid,
                   thd->wsrep_trx().ws_meta().gtid(),
                   wsrep_gtid_server.gtid());
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been prepared.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_prepare(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_after_prepare");
  WSREP_DEBUG("wsrep_after_prepare: %d", wsrep_is_real(thd, all));
  DBUG_ASSERT(wsrep_run_commit_hook(thd, all));
  int ret= thd->wsrep_cs().after_prepare();
  DBUG_ASSERT(ret == 0 || thd->wsrep_cs().current_error() ||
              thd->wsrep_cs().transaction().state() == wsrep::transaction::s_must_replay);
  DBUG_RETURN(ret);
}

/*
  Called before the transaction is committed.

  This function must be called from both client and
  applier contexts before commit.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_commit(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_before_commit");
  WSREP_DEBUG("wsrep_before_commit: %d, %lld",
              wsrep_is_real(thd, all),
              (long long)wsrep_thd_trx_seqno(thd));
  int ret= 0;
  DBUG_ASSERT(wsrep_run_commit_hook(thd, all));
  if ((ret= thd->wsrep_cs().before_commit()) == 0)
  {
    DBUG_ASSERT(!thd->wsrep_trx().ws_meta().gtid().is_undefined());
    if (!thd->variables.gtid_seq_no && 
        (thd->wsrep_trx().ws_meta().flags() & wsrep::provider::flag::commit))
    {
        uint64 seqno= 0;
        if (thd->variables.wsrep_gtid_seq_no &&
            thd->variables.wsrep_gtid_seq_no > wsrep_gtid_server.seqno())
        {
          seqno= thd->variables.wsrep_gtid_seq_no;
          wsrep_gtid_server.seqno(thd->variables.wsrep_gtid_seq_no);
        }
        else
        {
          seqno= wsrep_gtid_server.seqno_inc();
        }
        thd->variables.wsrep_gtid_seq_no= 0;
        thd->wsrep_current_gtid_seqno= seqno;
        if (mysql_bin_log.is_open() && wsrep_gtid_mode)
        {
          thd->variables.gtid_seq_no= seqno;
          thd->variables.gtid_domain_id= wsrep_gtid_server.domain_id;
          thd->variables.server_id= wsrep_gtid_server.server_id;
        }
    }

    wsrep_xid_init(&thd->wsrep_xid,
                   thd->wsrep_trx().ws_meta().gtid(),
                   wsrep_gtid_server.gtid());
    wsrep_register_for_group_commit(thd);
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been ordered for commit.

  This function must be called from both client and
  applier contexts after the commit has been ordered.

  @param thd Pointer to THD
  @param all 
  @param err Error buffer in case of applying error

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_ordered_commit(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_ordered_commit");
  WSREP_DEBUG("wsrep_ordered_commit: %d", wsrep_is_real(thd, all));
  DBUG_ASSERT(wsrep_run_commit_hook(thd, all));
  DBUG_RETURN(thd->wsrep_cs().ordered_commit());
}

/*
  Called after the transaction has been committed.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_commit(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_after_commit");
  WSREP_DEBUG("wsrep_after_commit: %d, %d, %lld, %d",
              wsrep_is_real(thd, all),
              wsrep_is_active(thd),
              (long long)wsrep_thd_trx_seqno(thd),
              wsrep_has_changes(thd));
  DBUG_ASSERT(wsrep_run_commit_hook(thd, all));
  int ret= 0;
  if (thd->wsrep_trx().state() == wsrep::transaction::s_committing)
  {
    ret= thd->wsrep_cs().ordered_commit();
  }
  wsrep_unregister_from_group_commit(thd);
  thd->wsrep_xid.null();
  DBUG_RETURN(ret || thd->wsrep_cs().after_commit());
}

/*
  Called before the transaction is rolled back.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_rollback(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_before_rollback");
  int ret= 0;
  if (wsrep_is_active(thd))
  {
    if (!all && thd->in_active_multi_stmt_transaction())
    {
      if (wsrep_emulate_bin_log)
      {
        wsrep_thd_binlog_stmt_rollback(thd);
      }

      if (thd->wsrep_trx().is_streaming() &&
          !wsrep_stmt_rollback_is_safe(thd))
      {
        /* Non-safe statement rollback during SR multi statement
           transasction. Self abort the transaction, the actual rollback
           and error handling will be done in after statement phase. */
        wsrep_thd_self_abort(thd);
        ret= 0;
      }
    }
    else if (wsrep_is_real(thd, all) &&
             thd->wsrep_trx().state() != wsrep::transaction::s_aborted)
    {
      /* Real transaction rolling back and wsrep abort not completed
         yet */
      /* Reset XID so that it does not trigger writing serialization
         history in InnoDB. This needs to be avoided because rollback
         may happen out of order and replay may follow. */
      thd->wsrep_xid.null();
      ret= thd->wsrep_cs().before_rollback();
    }
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been rolled back.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_rollback(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_after_rollback");
  DBUG_RETURN((wsrep_is_real(thd, all) && wsrep_is_active(thd) &&
               thd->wsrep_cs().transaction().state() !=
               wsrep::transaction::s_aborted) ?
              thd->wsrep_cs().after_rollback() : 0);
}

static inline int wsrep_before_statement(THD* thd)
{
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none ?
	  thd->wsrep_cs().before_statement() : 0);
}

static inline
int wsrep_after_statement(THD* thd)
{
  DBUG_ENTER("wsrep_after_statement");
  DBUG_RETURN(thd->wsrep_cs().state() != wsrep::client_state::s_none ?
              thd->wsrep_cs().after_statement() : 0);
}

static inline void wsrep_after_apply(THD* thd)
{
  DBUG_ASSERT(wsrep_thd_is_applying(thd));
  WSREP_DEBUG("wsrep_after_apply %lld", thd->thread_id);
  thd->wsrep_cs().after_applying();
}

static inline void wsrep_open(THD* thd)
{
  DBUG_ENTER("wsrep_open");
  if (WSREP(thd))
  {
    thd->wsrep_cs().open(wsrep::client_id(thd->thread_id));
    thd->wsrep_cs().debug_log_level(wsrep_debug);
    if (!thd->wsrep_applier && thd->variables.wsrep_trx_fragment_size)
    {
      thd->wsrep_cs().enable_streaming(
        wsrep_fragment_unit(thd->variables.wsrep_trx_fragment_unit),
        size_t(thd->variables.wsrep_trx_fragment_size));
    }
  }
  DBUG_VOID_RETURN;
}

static inline void wsrep_close(THD* thd)
{
  DBUG_ENTER("wsrep_close");
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none)
  {
    thd->wsrep_cs().close();
  }
  DBUG_VOID_RETURN;
}

static inline void
wsrep_wait_rollback_complete_and_acquire_ownership(THD *thd)
{
  DBUG_ENTER("wsrep_wait_rollback_complete_and_acquire_ownership");
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none)
  {
    thd->wsrep_cs().wait_rollback_complete_and_acquire_ownership();
  }
  DBUG_VOID_RETURN;
}

static inline int wsrep_before_command(THD* thd)
{
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none ?
	  thd->wsrep_cs().before_command() : 0);
}
/*
  Called after each command.

  Return zero on success, non-zero on failure.
*/
static inline void wsrep_after_command_before_result(THD* thd)
{
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none)
  {
    thd->wsrep_cs().after_command_before_result();
  }
}

static inline void wsrep_after_command_after_result(THD* thd)
{
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none)
  {
    thd->wsrep_cs().after_command_after_result();
  }
}

static inline void wsrep_after_command_ignore_result(THD* thd)
{
  wsrep_after_command_before_result(thd);
  DBUG_ASSERT(!thd->wsrep_cs().current_error());
  wsrep_after_command_after_result(thd);
}

static inline enum wsrep::client_error wsrep_current_error(THD* thd)
{
  return thd->wsrep_cs().current_error();
}

static inline enum wsrep::provider::status
wsrep_current_error_status(THD* thd)
{
  return thd->wsrep_cs().current_error_status();
}

#endif /* WSREP_TRANS_OBSERVER */
