/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                         // gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "transaction.h"
#include "debug_sync.h"         // DEBUG_SYNC
#include "sql_acl.h"
#include "semisync_master.h"
#ifdef WITH_WSREP
#include "wsrep_trans_observer.h"
#endif /* WITH_WSREP */

#ifndef EMBEDDED_LIBRARY
/**
  Helper: Tell tracker (if any) that transaction ended.
*/
static void trans_track_end_trx(THD *thd)
{
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
  {
    ((Transaction_state_tracker *)
     thd->session_tracker.get_tracker(TRANSACTION_INFO_TRACKER))->end_trx(thd);
  }
}
#else
#define trans_track_end_trx(A) do{}while(0)
#endif //EMBEDDED_LIBRARY


/**
  Helper: transaction ended, SET TRANSACTION one-shot variables
  revert to session values. Let the transaction state tracker know.
*/
void trans_reset_one_shot_chistics(THD *thd)
{
#ifndef EMBEDDED_LIBRARY
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
  {
    Transaction_state_tracker *tst= (Transaction_state_tracker *)
      thd->session_tracker.get_tracker(TRANSACTION_INFO_TRACKER);

    tst->set_read_flags(thd, TX_READ_INHERIT);
    tst->set_isol_level(thd, TX_ISOL_INHERIT);
  }
#endif //EMBEDDED_LIBRARY
  thd->tx_isolation= (enum_tx_isolation) thd->variables.tx_isolation;
  thd->tx_read_only= thd->variables.tx_read_only;
}

/* Conditions under which the transaction state must not change. */
static bool trans_check(THD *thd)
{
  enum xa_states xa_state= thd->transaction.xid_state.xa_state;
  DBUG_ENTER("trans_check");

  /*
    Always commit statement transaction before manipulating with
    the normal one.
  */
  DBUG_ASSERT(thd->transaction.stmt.is_empty());

  if (unlikely(thd->in_sub_stmt))
    my_error(ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0));
  if (xa_state != XA_NOTR)
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
  else
    DBUG_RETURN(FALSE);

  DBUG_RETURN(TRUE);
}


/**
  Mark a XA transaction as rollback-only if the RM unilaterally
  rolled back the transaction branch.

  @note If a rollback was requested by the RM, this function sets
        the appropriate rollback error code and transits the state
        to XA_ROLLBACK_ONLY.

  @return TRUE if transaction was rolled back or if the transaction
          state is XA_ROLLBACK_ONLY. FALSE otherwise.
*/
static bool xa_trans_rolled_back(XID_STATE *xid_state)
{
  if (xid_state->rm_error)
  {
    switch (xid_state->rm_error) {
    case ER_LOCK_WAIT_TIMEOUT:
      my_error(ER_XA_RBTIMEOUT, MYF(0));
      break;
    case ER_LOCK_DEADLOCK:
      my_error(ER_XA_RBDEADLOCK, MYF(0));
      break;
    default:
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    xid_state->xa_state= XA_ROLLBACK_ONLY;
  }

  return (xid_state->xa_state == XA_ROLLBACK_ONLY);
}


/**
  Rollback the active XA transaction.

  @note Resets rm_error before calling ha_rollback(), so
        the thd->transaction.xid structure gets reset
        by ha_rollback() / THD::transaction::cleanup().

  @return TRUE if the rollback failed, FALSE otherwise.
*/

static bool xa_trans_force_rollback(THD *thd)
{
  /*
    We must reset rm_error before calling ha_rollback(),
    so thd->transaction.xid structure gets reset
    by ha_rollback()/THD::transaction::cleanup().
  */
  thd->transaction.xid_state.rm_error= 0;
  if (ha_rollback_trans(thd, true))
  {
    my_error(ER_XAER_RMERR, MYF(0));
    return true;
  }
  return false;
}


/**
  Begin a new transaction.

  @note Beginning a transaction implicitly commits any current
        transaction and releases existing locks.

  @param thd     Current thread
  @param flags   Transaction flags

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_begin(THD *thd, uint flags)
{
  int res= FALSE;
#ifndef EMBEDDED_LIBRARY
  Transaction_state_tracker *tst= NULL;
#endif //EMBEDDED_LIBRARY
  DBUG_ENTER("trans_begin");

  if (trans_check(thd))
    DBUG_RETURN(TRUE);

#ifndef EMBEDDED_LIBRARY
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
    tst= (Transaction_state_tracker *)
      thd->session_tracker.get_tracker(TRANSACTION_INFO_TRACKER);
#endif //EMBEDDED_LIBRARY

  thd->locked_tables_list.unlock_locked_tables(thd);

  DBUG_ASSERT(!thd->locked_tables_mode);

  if (thd->in_multi_stmt_transaction_mode() ||
      (thd->variables.option_bits & OPTION_TABLE_LOCK))
  {
    thd->variables.option_bits&= ~OPTION_TABLE_LOCK;
    thd->server_status&=
      ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
    DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
    res= MY_TEST(ha_commit_trans(thd, TRUE));
#ifdef WITH_WSREP
    if (wsrep_thd_is_local(thd))
    {
      res= res || wsrep_after_statement(thd);
    }
#endif /* WITH_WSREP */
  }

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);

  /*
    The following set should not be needed as transaction state should
    already be reset. We should at some point change this to an assert.
  */
  thd->transaction.all.reset();
  thd->has_waiter= false;
  thd->waiting_on_group_commit= false;
  thd->transaction.start_time.reset(thd);

  if (res)
    DBUG_RETURN(TRUE);

  /*
    Release transactional metadata locks only after the
    transaction has been committed.
  */
  thd->mdl_context.release_transactional_locks();

  // The RO/RW options are mutually exclusive.
  DBUG_ASSERT(!((flags & MYSQL_START_TRANS_OPT_READ_ONLY) &&
                (flags & MYSQL_START_TRANS_OPT_READ_WRITE)));
  if (flags & MYSQL_START_TRANS_OPT_READ_ONLY)
  {
    thd->tx_read_only= true;
#ifndef EMBEDDED_LIBRARY
    if (tst)
      tst->set_read_flags(thd, TX_READ_ONLY);
#endif //EMBEDDED_LIBRARY
  }
  else if (flags & MYSQL_START_TRANS_OPT_READ_WRITE)
  {
    /*
      Explicitly starting a RW transaction when the server is in
      read-only mode, is not allowed unless the user has SUPER priv.
      Implicitly starting a RW transaction is allowed for backward
      compatibility.
    */
    const bool user_is_super=
      MY_TEST(thd->security_ctx->master_access & SUPER_ACL);
    if (opt_readonly && !user_is_super)
    {
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
      DBUG_RETURN(true);
    }
    thd->tx_read_only= false;
    /*
      This flags that tx_read_only was set explicitly, rather than
      just from the session's default.
    */
#ifndef EMBEDDED_LIBRARY
    if (tst)
      tst->set_read_flags(thd, TX_READ_WRITE);
#endif //EMBEDDED_LIBRARY
  }

#ifdef WITH_WSREP
  if (wsrep_thd_is_local(thd))
  {
    if (wsrep_sync_wait(thd))
      DBUG_RETURN(TRUE);
    if (!thd->tx_read_only &&
        wsrep_start_transaction(thd, thd->wsrep_next_trx_id()))
      DBUG_RETURN(TRUE);
  }
#endif /* WITH_WSREP */

  thd->variables.option_bits|= OPTION_BEGIN;
  thd->server_status|= SERVER_STATUS_IN_TRANS;
  if (thd->tx_read_only)
    thd->server_status|= SERVER_STATUS_IN_TRANS_READONLY;
  DBUG_PRINT("info", ("setting SERVER_STATUS_IN_TRANS"));

#ifndef EMBEDDED_LIBRARY
  if (tst)
    tst->add_trx_state(thd, TX_EXPLICIT);
#endif //EMBEDDED_LIBRARY

  /* ha_start_consistent_snapshot() relies on OPTION_BEGIN flag set. */
  if (flags & MYSQL_START_TRANS_OPT_WITH_CONS_SNAPSHOT)
  {
#ifndef EMBEDDED_LIBRARY
    if (tst)
      tst->add_trx_state(thd, TX_WITH_SNAPSHOT);
#endif //EMBEDDED_LIBRARY
    res= ha_start_consistent_snapshot(thd);
  }

  DBUG_RETURN(MY_TEST(res));
}


/**
  Commit the current transaction, making its changes permanent.

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_commit(THD *thd)
{
  int res;
  DBUG_ENTER("trans_commit");

  if (trans_check(thd))
    DBUG_RETURN(TRUE);

  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  res= ha_commit_trans(thd, TRUE);

  mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
  mysql_mutex_assert_not_owner(mysql_bin_log.get_log_lock());
  mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
  mysql_mutex_assert_not_owner(&LOCK_commit_ordered);

    /*
      if res is non-zero, then ha_commit_trans has rolled back the
      transaction, so the hooks for rollback will be called.
    */
  if (res)
  {
#ifdef HAVE_REPLICATION
    repl_semisync_master.wait_after_rollback(thd, FALSE);
#endif
  }
  else
  {
#ifdef HAVE_REPLICATION
    repl_semisync_master.wait_after_commit(thd, FALSE);
#endif
  }
  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction.all.reset();
  thd->lex->start_transaction_opt= 0;

  trans_track_end_trx(thd);

  DBUG_RETURN(MY_TEST(res));
}


/**
  Implicitly commit the current transaction.

  @note A implicit commit does not releases existing table locks.

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_commit_implicit(THD *thd)
{
  bool res= FALSE;
  DBUG_ENTER("trans_commit_implicit");

  if (trans_check(thd))
    DBUG_RETURN(TRUE);

  if (thd->variables.option_bits & OPTION_GTID_BEGIN)
    DBUG_PRINT("error", ("OPTION_GTID_BEGIN is set. "
                         "Master and slave will have different GTID values"));

  if (thd->in_multi_stmt_transaction_mode() ||
      (thd->variables.option_bits & OPTION_TABLE_LOCK))
  {
    /* Safety if one did "drop table" on locked tables */
    if (!thd->locked_tables_mode)
      thd->variables.option_bits&= ~OPTION_TABLE_LOCK;
    thd->server_status&=
      ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
    DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
    res= MY_TEST(ha_commit_trans(thd, TRUE));
  }

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction.all.reset();

  /*
    Upon implicit commit, reset the current transaction
    isolation level and access mode. We do not care about
    @@session.completion_type since it's documented
    to not have any effect on implicit commit.
  */
  trans_reset_one_shot_chistics(thd);

  trans_track_end_trx(thd);

  DBUG_RETURN(res);
}


/**
  Rollback the current transaction, canceling its changes.

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_rollback(THD *thd)
{
  int res;
  DBUG_ENTER("trans_rollback");

  if (trans_check(thd))
    DBUG_RETURN(TRUE);

  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  res= ha_rollback_trans(thd, TRUE);
#ifdef HAVE_REPLICATION
  repl_semisync_master.wait_after_rollback(thd, FALSE);
#endif
  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  /* Reset the binlog transaction marker */
  thd->variables.option_bits&= ~OPTION_GTID_BEGIN;
  thd->transaction.all.reset();
  thd->lex->start_transaction_opt= 0;

  trans_track_end_trx(thd);

  DBUG_RETURN(MY_TEST(res));
}


/**
  Implicitly rollback the current transaction, typically
  after deadlock was discovered.

  @param thd     Current thread

  @retval False Success
  @retval True  Failure

  @note ha_rollback_low() which is indirectly called by this
        function will mark XA transaction for rollback by
        setting appropriate RM error status if there was
        transaction rollback request.
*/

bool trans_rollback_implicit(THD *thd)
{
  int res;
  DBUG_ENTER("trans_rollback_implict");

  /*
    Always commit/rollback statement transaction before manipulating
    with the normal one.
    Don't perform rollback in the middle of sub-statement, wait till
    its end.
  */
  DBUG_ASSERT(thd->transaction.stmt.is_empty() && !thd->in_sub_stmt);

  thd->server_status&= ~SERVER_STATUS_IN_TRANS;
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  res= ha_rollback_trans(thd, true);
  /*
    We don't reset OPTION_BEGIN flag below to simulate implicit start
    of new transacton in @@autocommit=1 mode. This is necessary to
    preserve backward compatibility.
  */
  thd->variables.option_bits&= ~(OPTION_KEEP_LOG);
  thd->transaction.all.reset();

  /* Rollback should clear transaction_rollback_request flag. */
  DBUG_ASSERT(! thd->transaction_rollback_request);

  trans_track_end_trx(thd);

  DBUG_RETURN(MY_TEST(res));
}


/**
  Commit the single statement transaction.

  @note Note that if the autocommit is on, then the following call
        inside InnoDB will commit or rollback the whole transaction
        (= the statement). The autocommit mechanism built into InnoDB
        is based on counting locks, but if the user has used LOCK
        TABLES then that mechanism does not know to do the commit.

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_commit_stmt(THD *thd)
{
  DBUG_ENTER("trans_commit_stmt");
  int res= FALSE;
  /*
    We currently don't invoke commit/rollback at end of
    a sub-statement.  In future, we perhaps should take
    a savepoint for each nested statement, and release the
    savepoint when statement has succeeded.
  */
  DBUG_ASSERT(! thd->in_sub_stmt);

  thd->merge_unsafe_rollback_flags();

  if (thd->transaction.stmt.ha_list)
  {
    res= ha_commit_trans(thd, FALSE);
    if (! thd->in_active_multi_stmt_transaction())
    {
      trans_reset_one_shot_chistics(thd);
    }
  }

  mysql_mutex_assert_not_owner(&LOCK_prepare_ordered);
  mysql_mutex_assert_not_owner(mysql_bin_log.get_log_lock());
  mysql_mutex_assert_not_owner(&LOCK_after_binlog_sync);
  mysql_mutex_assert_not_owner(&LOCK_commit_ordered);

    /*
      if res is non-zero, then ha_commit_trans has rolled back the
      transaction, so the hooks for rollback will be called.
    */
  if (res)
  {
#ifdef HAVE_REPLICATION
    repl_semisync_master.wait_after_rollback(thd, FALSE);
#endif
  }
  else
  {
#ifdef HAVE_REPLICATION
    repl_semisync_master.wait_after_commit(thd, FALSE);
#endif
  }

  thd->transaction.stmt.reset();

  DBUG_RETURN(MY_TEST(res));
}


/**
  Rollback the single statement transaction.

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/
bool trans_rollback_stmt(THD *thd)
{
  DBUG_ENTER("trans_rollback_stmt");

  /*
    We currently don't invoke commit/rollback at end of
    a sub-statement.  In future, we perhaps should take
    a savepoint for each nested statement, and release the
    savepoint when statement has succeeded.
  */
  DBUG_ASSERT(! thd->in_sub_stmt);

  thd->merge_unsafe_rollback_flags();

  if (thd->transaction.stmt.ha_list)
  {
    ha_rollback_trans(thd, FALSE);
    if (! thd->in_active_multi_stmt_transaction())
      trans_reset_one_shot_chistics(thd);
  }

#ifdef HAVE_REPLICATION
  repl_semisync_master.wait_after_rollback(thd, FALSE);
#endif

  thd->transaction.stmt.reset();

  DBUG_RETURN(FALSE);
}

/* Find a named savepoint in the current transaction. */
static SAVEPOINT **
find_savepoint(THD *thd, LEX_CSTRING name)
{
  SAVEPOINT **sv= &thd->transaction.savepoints;

  while (*sv)
  {
    if (my_strnncoll(system_charset_info, (uchar *) name.str, name.length,
                     (uchar *) (*sv)->name, (*sv)->length) == 0)
      break;
    sv= &(*sv)->prev;
  }

  return sv;
}


/**
  Set a named transaction savepoint.

  @param thd    Current thread
  @param name   Savepoint name

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_savepoint(THD *thd, LEX_CSTRING name)
{
  SAVEPOINT **sv, *newsv;
  DBUG_ENTER("trans_savepoint");

  if (!(thd->in_multi_stmt_transaction_mode() || thd->in_sub_stmt) ||
      !opt_using_transactions)
    DBUG_RETURN(FALSE);

  if (thd->transaction.xid_state.check_has_uncommitted_xa())
    DBUG_RETURN(TRUE);

  sv= find_savepoint(thd, name);

  if (*sv) /* old savepoint of the same name exists */
  {
    newsv= *sv;
    ha_release_savepoint(thd, *sv);
    *sv= (*sv)->prev;
  }
  else if ((newsv= (SAVEPOINT *) alloc_root(&thd->transaction.mem_root,
                                            savepoint_alloc_size)) == NULL)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    DBUG_RETURN(TRUE);
  }

  newsv->name= strmake_root(&thd->transaction.mem_root, name.str, name.length);
  newsv->length= (uint)name.length;

  /*
    if we'll get an error here, don't add new savepoint to the list.
    we'll lose a little bit of memory in transaction mem_root, but it'll
    be free'd when transaction ends anyway
  */
  if (unlikely(ha_savepoint(thd, newsv)))
    DBUG_RETURN(TRUE);

  newsv->prev= thd->transaction.savepoints;
  thd->transaction.savepoints= newsv;

  /*
    Remember locks acquired before the savepoint was set.
    They are used as a marker to only release locks acquired after
    the setting of this savepoint.
    Note: this works just fine if we're under LOCK TABLES,
    since mdl_savepoint() is guaranteed to be beyond
    the last locked table. This allows to release some
    locks acquired during LOCK TABLES.
  */
  newsv->mdl_savepoint= thd->mdl_context.mdl_savepoint();

  DBUG_RETURN(FALSE);
}


/**
  Rollback a transaction to the named savepoint.

  @note Modifications that the current transaction made to
        rows after the savepoint was set are undone in the
        rollback.

  @note Savepoints that were set at a later time than the
        named savepoint are deleted.

  @param thd    Current thread
  @param name   Savepoint name

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_rollback_to_savepoint(THD *thd, LEX_CSTRING name)
{
  int res= FALSE;
  SAVEPOINT *sv= *find_savepoint(thd, name);
  DBUG_ENTER("trans_rollback_to_savepoint");

  if (sv == NULL)
  {
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "SAVEPOINT", name.str);
    DBUG_RETURN(TRUE);
  }

  if (thd->transaction.xid_state.check_has_uncommitted_xa())
    DBUG_RETURN(TRUE);

  /**
    Checking whether it is safe to release metadata locks acquired after
    savepoint, if rollback to savepoint is successful.
  
    Whether it is safe to release MDL after rollback to savepoint depends
    on storage engines participating in transaction:
  
    - InnoDB doesn't release any row-locks on rollback to savepoint so it
      is probably a bad idea to release MDL as well.
    - Binary log implementation in some cases (e.g when non-transactional
      tables involved) may choose not to remove events added after savepoint
      from transactional cache, but instead will write them to binary
      log accompanied with ROLLBACK TO SAVEPOINT statement. Since the real
      write happens at the end of transaction releasing MDL on tables
      mentioned in these events (i.e. acquired after savepoint and before
      rollback ot it) can break replication, as concurrent DROP TABLES
      statements will be able to drop these tables before events will get
      into binary log,
  
    For backward-compatibility reasons we always release MDL if binary
    logging is off.
  */
  bool mdl_can_safely_rollback_to_savepoint=
                (!((WSREP_EMULATE_BINLOG_NNULL(thd) || mysql_bin_log.is_open())
                 && thd->variables.sql_log_bin) ||
                 ha_rollback_to_savepoint_can_release_mdl(thd));

  if (ha_rollback_to_savepoint(thd, sv))
    res= TRUE;
  else if (((thd->variables.option_bits & OPTION_KEEP_LOG) ||
            thd->transaction.all.modified_non_trans_table) &&
           !thd->slave_thread)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER_THD(thd, ER_WARNING_NOT_COMPLETE_ROLLBACK));

  thd->transaction.savepoints= sv;

  if (!res && mdl_can_safely_rollback_to_savepoint)
    thd->mdl_context.rollback_to_savepoint(sv->mdl_savepoint);

  DBUG_RETURN(MY_TEST(res));
}


/**
  Remove the named savepoint from the set of savepoints of
  the current transaction.

  @note No commit or rollback occurs. It is an error if the
        savepoint does not exist.

  @param thd    Current thread
  @param name   Savepoint name

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_release_savepoint(THD *thd, LEX_CSTRING name)
{
  int res= FALSE;
  SAVEPOINT *sv= *find_savepoint(thd, name);
  DBUG_ENTER("trans_release_savepoint");

  if (sv == NULL)
  {
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "SAVEPOINT", name.str);
    DBUG_RETURN(TRUE);
  }

  if (ha_release_savepoint(thd, sv))
    res= TRUE;

  thd->transaction.savepoints= sv->prev;

  DBUG_RETURN(MY_TEST(res));
}


/**
  Detach the current XA transaction;

  @param thd     Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_detach(THD *thd)
{
  XID_STATE *xid_s= &thd->transaction.xid_state;
  Ha_trx_info *ha_info, *ha_info_next;

  DBUG_ENTER("trans_xa_detach");

//  #7974 DBUG_ASSERT(xid_s->xa_state == XA_PREPARED &&
//              xid_cache_search(thd, &xid_s->xid));

  xid_cache_delete(thd, xid_s);
  if (xid_cache_insert(&xid_s->xid, XA_PREPARED, xid_s->is_binlogged))
    DBUG_RETURN(TRUE);

  for (ha_info= thd->transaction.all.ha_list;
       ha_info;
       ha_info= ha_info_next)
  {
    ha_info_next= ha_info->next();
    ha_info->reset(); /* keep it conveniently zero-filled */
  }

  thd->transaction.all.ha_list= 0;
  thd->transaction.all.no_2pc= 0;

  DBUG_RETURN(FALSE);
}


void attach_native_trx(THD *thd);
/**
  This is a specific to "slave" applier collection of standard cleanup
  actions to reset XA transaction states at the end of XA prepare rather than
  to do it at the transaction commit, see @c ha_commit_one_phase.
  THD of the slave applier is dissociated from a transaction object in engine
  that continues to exist there.

  @param  THD current thread
  @return the value of is_error()
*/

bool applier_reset_xa_trans(THD *thd)
{
  XID_STATE *xid_s= &thd->transaction.xid_state;

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(thd, xid_s);
  if (xid_cache_insert(&xid_s->xid, XA_PREPARED, xid_s->is_binlogged))
    return true;

  attach_native_trx(thd);
  thd->transaction.cleanup();
  thd->transaction.xid_state.xa_state= XA_NOTR;
  thd->mdl_context.release_transactional_locks();

  return thd->is_error();
#ifdef p7974
  Transaction_ctx *trn_ctx= thd->get_transaction();
  XID_STATE *xid_state= trn_ctx->xid_state();
  /*
    In the following the server transaction state gets reset for
    a slave applier thread similarly to xa_commit logics
    except commit does not run.
  */
  thd->variables.option_bits&= ~OPTION_BEGIN;
  trn_ctx->reset_unsafe_rollback_flags(Transaction_ctx::STMT);
  thd->server_status&= ~SERVER_STATUS_IN_TRANS;
  /* Server transaction ctx is detached from THD */
  transaction_cache_detach(trn_ctx);
  xid_state->reset();
  /*
     The current engine transactions is detached from THD, and
     previously saved is restored.
  */
  attach_native_trx(thd);
  trn_ctx->set_ha_trx_info(Transaction_ctx::SESSION, NULL);
  trn_ctx->set_no_2pc(Transaction_ctx::SESSION, false);
  trn_ctx->cleanup();
#ifdef HAVE_PSI_TRANSACTION_INTERFACE
  MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
  thd->m_transaction_psi= NULL;
#endif

#endif /*p7974*/
  return thd->is_error();
}


/**
  The function detaches existing storage engines transaction
  context from thd. Backup area to save it is provided to low level
  storage engine function.

  is invoked by plugin_foreach() after
  trans_xa_start() for each storage engine.

  @param[in,out]     thd     Thread context
  @param             plugin  Reference to handlerton

  @return    FALSE   on success, TRUE otherwise.
*/

my_bool detach_native_trx(THD *thd, plugin_ref plugin, void *unused)
{
  handlerton *hton= plugin_hton(plugin);
  if (hton->replace_native_transaction_in_thd)
    hton->replace_native_transaction_in_thd(thd, NULL,
                                            thd_ha_data_backup(thd, hton));

  return FALSE;

}

/**
  The function restores previously saved storage engine transaction context.

  @param     thd     Thread context
*/
void attach_native_trx(THD *thd)
{
  Ha_trx_info *ha_info= thd->transaction.all.ha_list;
  Ha_trx_info *ha_info_next;

  if (ha_info)
  {
    for (; ha_info; ha_info= ha_info_next)
    {
      handlerton *hton= ha_info->ht();
      if (hton->replace_native_transaction_in_thd)
      {
        /* restore the saved original engine transaction's link with thd */
        void **trx_backup= thd_ha_data_backup(thd, hton);

        hton->
          replace_native_transaction_in_thd(thd, *trx_backup, NULL);
        *trx_backup= NULL;
      }
      ha_info_next= ha_info->next();
      ha_info->reset();
    }
  }
  thd->transaction.all.ha_list= 0;
  thd->transaction.all.no_2pc= 0;
}


/**
  Starts an XA transaction with the given xid value.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_start(THD *thd)
{
  enum xa_states xa_state= thd->transaction.xid_state.xa_state;
  DBUG_ENTER("trans_xa_start");

  if (xa_state == XA_IDLE && thd->lex->xa_opt == XA_RESUME)
  {
    bool not_equal= !thd->transaction.xid_state.xid.eq(thd->lex->xid);
    if (not_equal)
      my_error(ER_XAER_NOTA, MYF(0));
    else
      thd->transaction.xid_state.xa_state= XA_ACTIVE;
    DBUG_RETURN(not_equal);
  }

  /* TODO: JOIN is not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (xa_state != XA_NOTR)
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
  else if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
    my_error(ER_XAER_OUTSIDE, MYF(0));
  else if (!trans_begin(thd))
  {
    DBUG_ASSERT(thd->transaction.xid_state.xid.is_null());
    thd->transaction.xid_state.xa_state= XA_ACTIVE;
    thd->transaction.xid_state.rm_error= 0;
    thd->transaction.xid_state.xid.set(thd->lex->xid);
    if (xid_cache_insert(thd, &thd->transaction.xid_state))
    {
      thd->transaction.xid_state.xa_state= XA_NOTR;
      thd->transaction.xid_state.xid.null();
      trans_rollback(thd);
      DBUG_RETURN(true);
    }

    if (thd->variables.pseudo_slave_mode || thd->slave_thread)
    {
      /*
        In case of slave thread applier or processing binlog by client,
        detach the "native" thd's trx in favor of dynamically created.
      */
      plugin_foreach(thd, detach_native_trx,
                     MYSQL_STORAGE_ENGINE_PLUGIN, NULL);
    }

    DBUG_RETURN(FALSE);
  }
  DBUG_RETURN(TRUE);
}


/**
  Put a XA transaction in the IDLE state.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_end(THD *thd)
{
  DBUG_ENTER("trans_xa_end");

  /* TODO: SUSPEND and FOR MIGRATE are not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (thd->transaction.xid_state.xa_state != XA_ACTIVE)
    my_error(ER_XAER_RMFAIL, MYF(0),
             xa_state_names[thd->transaction.xid_state.xa_state]);
  else if (!thd->transaction.xid_state.xid.eq(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (!xa_trans_rolled_back(&thd->transaction.xid_state))
    thd->transaction.xid_state.xa_state= XA_IDLE;

  DBUG_RETURN(thd->is_error() ||
              thd->transaction.xid_state.xa_state != XA_IDLE);
}


/**
  Put a XA transaction in the PREPARED state.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_prepare(THD *thd)
{
  int res= 1;

  DBUG_ENTER("trans_xa_prepare");

  if (thd->transaction.xid_state.xa_state != XA_IDLE)
    my_error(ER_XAER_RMFAIL, MYF(0),
             xa_state_names[thd->transaction.xid_state.xa_state]);
  else if (!thd->transaction.xid_state.xid.eq(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else
  {
    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_request mdl_request;
    mdl_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                     MDL_STATEMENT);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout) ||
        ha_prepare(thd))
    {
      if (!mdl_request.ticket)
        ha_rollback_trans(thd, TRUE);
      thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
      thd->transaction.all.reset();
      thd->server_status&=
        ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
      xid_cache_delete(thd, &thd->transaction.xid_state);
      thd->transaction.xid_state.xa_state= XA_NOTR;
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    else
    {
      res= 0;
      thd->transaction.xid_state.xa_state= XA_PREPARED;
      if (thd->variables.pseudo_slave_mode)
        res= applier_reset_xa_trans(thd);
    }
  }

  DBUG_RETURN(res);
}


/**
  Commit and terminate the a XA transaction.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_commit(THD *thd)
{
  bool res= TRUE;
  enum xa_states xa_state= thd->transaction.xid_state.xa_state;
  DBUG_ENTER("trans_xa_commit");

  if (!thd->transaction.xid_state.xid.eq(thd->lex->xid))
  {
    if (thd->fix_xid_hash_pins())
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      DBUG_RETURN(TRUE);
    }

    XID_STATE *xs= xid_cache_search(thd, thd->lex->xid);
    res= !xs;
    if (res)
      my_error(ER_XAER_NOTA, MYF(0));
    else if (thd->in_multi_stmt_transaction_mode())
    {
      my_error(ER_XAER_RMFAIL, MYF(0),
                xa_state_names[thd->transaction.xid_state.xa_state]);
      res= TRUE;
    }
    else
    {
      res= xa_trans_rolled_back(xs);
      /*
        Acquire metadata lock which will ensure that COMMIT is blocked
        by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
        progress blocks FTWRL).

        We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
      */
      MDL_request mdl_request;
      mdl_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                       MDL_STATEMENT);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
      {
        /*
          We can't rollback an XA transaction on lock failure due to
          Innodb redo log and bin log update is involved in rollback.
          Return error to user for a retry.
        */
        my_error(ER_XAER_RMERR, MYF(0));
        DBUG_RETURN(true);
      }
      ha_commit_or_rollback_by_xid(thd->lex->xid, !res);
      if((WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()) &&
          xs->is_binlogged)
      {
        res= thd->binlog_query(THD::THD::STMT_QUERY_TYPE,
                               thd->query(), thd->query_length(),
                               FALSE, FALSE, FALSE, 0);
      }
      else
        res= 0;
      xid_cache_delete(thd, xs);
    }
    DBUG_RETURN(res);
  }

  if (xa_trans_rolled_back(&thd->transaction.xid_state))
  {
    xa_trans_force_rollback(thd);
    res= thd->is_error();
  }
  else if (xa_state == XA_IDLE && thd->lex->xa_opt == XA_ONE_PHASE)
  {
    int r= ha_commit_trans(thd, TRUE);
    if ((res= MY_TEST(r)))
      my_error(r == 1 ? ER_XA_RBROLLBACK : ER_XAER_RMERR, MYF(0));
  }
  else if (xa_state == XA_PREPARED && thd->lex->xa_opt == XA_NONE)
  {
    MDL_request mdl_request;

    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    mdl_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                     MDL_STATEMENT);

    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
    {
      /*
        We can't rollback an XA transaction on lock failure due to
        Innodb redo log and bin log update is involved in rollback.
        Return error to user for a retry.
      */
      my_error(ER_XAER_RMERR, MYF(0));
      DBUG_RETURN(true);
    }
    else
    {
      DEBUG_SYNC(thd, "trans_xa_commit_after_acquire_commit_lock");

      if((WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()) &&
          thd->transaction.xid_state.is_binlogged)
      {
        res= thd->binlog_query(THD::THD::STMT_QUERY_TYPE,
                               thd->query(), thd->query_length(),
                               FALSE, FALSE, FALSE, 0);
      }
      else
        res= 0;

      if (res || (res= MY_TEST(ha_commit_one_phase(thd, 1))))
        my_error(ER_XAER_RMERR, MYF(0));
    }
  }
  else
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    DBUG_RETURN(TRUE);
  }

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction.all.reset();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(thd, &thd->transaction.xid_state);
  thd->transaction.xid_state.xa_state= XA_NOTR;

  trans_track_end_trx(thd);

  DBUG_RETURN(res);
}


/**
  Roll back and terminate a XA transaction.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_rollback(THD *thd)
{
  bool res= TRUE;
  enum xa_states xa_state= thd->transaction.xid_state.xa_state;
  DBUG_ENTER("trans_xa_rollback");

  if (!thd->transaction.xid_state.xid.eq(thd->lex->xid))
  {
    if (thd->fix_xid_hash_pins())
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      DBUG_RETURN(TRUE);
    }

    XID_STATE *xs= xid_cache_search(thd, thd->lex->xid);
    if (!xs)
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      MDL_request mdl_request;
      mdl_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                       MDL_STATEMENT);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
      {
        /*
          We can't rollback an XA transaction on lock failure due to
          Innodb redo log and bin log update is involved in rollback.
          Return error to user for a retry.
        */
        my_error(ER_XAER_RMERR, MYF(0));
        DBUG_RETURN(true);
      }

      xa_trans_rolled_back(xs);
      if (ha_commit_or_rollback_by_xid(thd->lex->xid, 0) == 0 &&
          xs->is_binlogged &&
          (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()))
        thd->binlog_query(THD::THD::STMT_QUERY_TYPE,
                          thd->query(), thd->query_length(),
                          FALSE, FALSE, FALSE, 0);
      xid_cache_delete(thd, xs);
    }
    DBUG_RETURN(thd->get_stmt_da()->is_error());
  }

  if (xa_state != XA_IDLE && xa_state != XA_PREPARED &&
      xa_state != XA_ROLLBACK_ONLY)
  {
    my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
    DBUG_RETURN(TRUE);
  }

  MDL_request mdl_request;
  mdl_request.init(MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
      MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
        thd->variables.lock_wait_timeout))
  {
    /*
       We can't rollback an XA transaction on lock failure due to
       Innodb redo log and bin log update is involved in rollback.
       Return error to user for a retry.
       */
    my_error(ER_XAER_RMERR, MYF(0));
    DBUG_RETURN(true);
  }

  if(xa_state == XA_PREPARED && thd->transaction.xid_state.is_binlogged &&
     (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()))
  {
    res= thd->binlog_query(THD::THD::STMT_QUERY_TYPE,
        thd->query(), thd->query_length(),
        FALSE, FALSE, FALSE, 0);
  }
  else
    res= 0;

  res= res || xa_trans_force_rollback(thd);
  if (res || (res= MY_TEST(xa_trans_force_rollback(thd))))
    my_error(ER_XAER_RMERR, MYF(0));

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction.all.reset();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(thd, &thd->transaction.xid_state);
  thd->transaction.xid_state.xa_state= XA_NOTR;

  trans_track_end_trx(thd);

  DBUG_RETURN(res);
}
