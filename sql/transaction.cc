/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                         // gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "transaction.h"
#include "debug_sync.h"         // DEBUG_SYNC
#include "sql_acl.h"
#include "semisync_master.h"
#include <pfs_transaction_provider.h>
#include <mysql/psi/mysql_transaction.h>
#ifdef WITH_WSREP
#include "wsrep_trans_observer.h"
#endif /* WITH_WSREP */

/**
  Helper: Tell tracker (if any) that transaction ended.
*/
void trans_track_end_trx(THD *thd)
{
#ifndef EMBEDDED_LIBRARY
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
    thd->session_tracker.transaction_info.end_trx(thd);
#endif //EMBEDDED_LIBRARY
}


/**
  Helper: transaction ended, SET TRANSACTION one-shot variables
  revert to session values. Let the transaction state tracker know.
*/
void trans_reset_one_shot_chistics(THD *thd)
{
#ifndef EMBEDDED_LIBRARY
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
  {
    thd->session_tracker.transaction_info.set_read_flags(thd, TX_READ_INHERIT);
    thd->session_tracker.transaction_info.set_isol_level(thd, TX_ISOL_INHERIT);
  }
#endif //EMBEDDED_LIBRARY
  thd->tx_isolation= (enum_tx_isolation) thd->variables.tx_isolation;
  thd->tx_read_only= thd->variables.tx_read_only;
}

/* Conditions under which the transaction state must not change. */
static bool trans_check(THD *thd)
{
  DBUG_ENTER("trans_check");

  /*
    Always commit statement transaction before manipulating with
    the normal one.
  */
  DBUG_ASSERT(thd->transaction->stmt.is_empty());

  if (unlikely(thd->in_sub_stmt))
    my_error(ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0));
  if (!thd->transaction->xid_state.is_explicit_XA())
    DBUG_RETURN(FALSE);

  thd->transaction->xid_state.er_xaer_rmfail();
  DBUG_RETURN(TRUE);
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
  DBUG_ENTER("trans_begin");

  if (trans_check(thd))
    DBUG_RETURN(TRUE);

  if (thd->locked_tables_list.unlock_locked_tables(thd))
    DBUG_RETURN(true);

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
  thd->transaction->all.reset();
  thd->has_waiter= false;
  thd->waiting_on_group_commit= false;
  thd->transaction->start_time.reset(thd);

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
    if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
      thd->session_tracker.transaction_info.set_read_flags(thd, TX_READ_ONLY);
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
      MY_TEST(thd->security_ctx->master_access & PRIV_IGNORE_READ_ONLY);
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
    if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
      thd->session_tracker.transaction_info.set_read_flags(thd, TX_READ_WRITE);
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
  if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
    thd->session_tracker.transaction_info.add_trx_state(thd, TX_EXPLICIT);
#endif //EMBEDDED_LIBRARY

  /* ha_start_consistent_snapshot() relies on OPTION_BEGIN flag set. */
  if (flags & MYSQL_START_TRANS_OPT_WITH_CONS_SNAPSHOT)
  {
#ifndef EMBEDDED_LIBRARY
    if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
      thd->session_tracker.transaction_info.add_trx_state(thd, TX_WITH_SNAPSHOT);
#endif //EMBEDDED_LIBRARY
    res= ha_start_consistent_snapshot(thd);
  }
  /*
    Register transaction start in performance schema if not done already.
    We handle explicitly started transactions here, implicitly started
    transactions (and single-statement transactions in autocommit=1 mode)
    are handled in trans_register_ha().
    We can't handle explicit transactions in the same way as implicit
    because we want to correctly attribute statements which follow
    BEGIN but do not touch any transactional tables.
  */
  if (thd->m_transaction_psi == NULL)
  {
    thd->m_transaction_psi= MYSQL_START_TRANSACTION(&thd->m_transaction_state,
                                                 NULL, 0, thd->tx_isolation,
                                                 thd->tx_read_only, false);
    DEBUG_SYNC(thd, "after_set_transaction_psi_before_set_transaction_gtid");
    //gtid_set_performance_schema_values(thd);
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
#ifdef HAVE_REPLICATION
  if (res)
    repl_semisync_master.wait_after_rollback(thd, FALSE);
  else
    repl_semisync_master.wait_after_commit(thd, FALSE);
#endif
  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction->all.reset();
  thd->lex->start_transaction_opt= 0;

  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL);
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
  {
    DBUG_PRINT("error", ("OPTION_GTID_BEGIN is set. "
                         "Master and slave will have different GTID values"));
  }

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
  thd->transaction->all.reset();

  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL);

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
  /* Reset the binlog transaction marker */
  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG |
                                 OPTION_GTID_BEGIN);
  thd->transaction->all.reset();
  thd->lex->start_transaction_opt= 0;

  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL);

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
  DBUG_ASSERT(thd->transaction->stmt.is_empty() && !thd->in_sub_stmt);

  thd->server_status&= ~SERVER_STATUS_IN_TRANS;
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  res= ha_rollback_trans(thd, true);
  /*
    We don't reset OPTION_BEGIN flag below to simulate implicit start
    of new transacton in @@autocommit=1 mode. This is necessary to
    preserve backward compatibility.
  */
  thd->variables.option_bits&= ~(OPTION_KEEP_LOG);
  thd->transaction->all.reset();

  /* Rollback should clear transaction_rollback_request flag. */
  DBUG_ASSERT(!thd->transaction_rollback_request);
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL);

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

  if (thd->transaction->stmt.ha_list)
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

  /* In autocommit=1 mode the transaction should be marked as complete in P_S */
  DBUG_ASSERT(thd->in_active_multi_stmt_transaction() ||
              thd->m_transaction_psi == NULL);

  thd->transaction->stmt.reset();

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

  if (thd->transaction->stmt.ha_list)
  {
    ha_rollback_trans(thd, FALSE);
    if (! thd->in_active_multi_stmt_transaction())
      trans_reset_one_shot_chistics(thd);
  }

#ifdef HAVE_REPLICATION
  repl_semisync_master.wait_after_rollback(thd, FALSE);
#endif

  /* In autocommit=1 mode the transaction should be marked as complete in P_S */
  DBUG_ASSERT(thd->in_active_multi_stmt_transaction() ||
              thd->m_transaction_psi == NULL);

  thd->transaction->stmt.reset();

  DBUG_RETURN(FALSE);
}

/* Find a named savepoint in the current transaction. */
static SAVEPOINT **
find_savepoint(THD *thd, LEX_CSTRING name)
{
  SAVEPOINT **sv= &thd->transaction->savepoints;

  while (*sv)
  {
    if (system_charset_info->strnncoll(
                     (uchar *) name.str, name.length,
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

  if (thd->transaction->xid_state.check_has_uncommitted_xa())
    DBUG_RETURN(TRUE);

  sv= find_savepoint(thd, name);

  if (*sv) /* old savepoint of the same name exists */
  {
    newsv= *sv;
    ha_release_savepoint(thd, *sv);
    *sv= (*sv)->prev;
  }
  else if ((newsv= (SAVEPOINT *) alloc_root(&thd->transaction->mem_root,
                                            savepoint_alloc_size)) == NULL)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    DBUG_RETURN(TRUE);
  }

  newsv->name= strmake_root(&thd->transaction->mem_root, name.str, name.length);
  newsv->length= (uint)name.length;

  /*
    if we'll get an error here, don't add new savepoint to the list.
    we'll lose a little bit of memory in transaction mem_root, but it'll
    be free'd when transaction ends anyway
  */
  if (unlikely(ha_savepoint(thd, newsv)))
    DBUG_RETURN(TRUE);

  newsv->prev= thd->transaction->savepoints;
  thd->transaction->savepoints= newsv;

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

  if (thd->transaction->xid_state.check_has_uncommitted_xa())
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
            thd->transaction->all.modified_non_trans_table) &&
           !thd->slave_thread)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WARNING_NOT_COMPLETE_ROLLBACK,
                 ER_THD(thd, ER_WARNING_NOT_COMPLETE_ROLLBACK));

  thd->transaction->savepoints= sv;

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

  thd->transaction->savepoints= sv->prev;

  DBUG_RETURN(MY_TEST(res));
}
