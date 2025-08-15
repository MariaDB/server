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

#ifndef WSREP_THD_H
#define WSREP_THD_H

#include <my_config.h>

#include "mysql/service_wsrep.h"
#include "wsrep/client_state.hpp"
#include "sql_class.h"
#include "wsrep_utils.h"
#include <deque>
class Wsrep_thd_queue
{
public:
  Wsrep_thd_queue(THD* t) : thd(t)
  {
    mysql_mutex_init(key_LOCK_wsrep_thd_queue,
                     &LOCK_wsrep_thd_queue,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_wsrep_thd_queue, &COND_wsrep_thd_queue, NULL);
  }
  ~Wsrep_thd_queue()
  {
    mysql_mutex_destroy(&LOCK_wsrep_thd_queue);
    mysql_cond_destroy(&COND_wsrep_thd_queue);
  }
  bool push_back(THD* thd)
  {
    DBUG_ASSERT(thd);
    wsp::auto_lock lock(&LOCK_wsrep_thd_queue);
    std::deque<THD*>::iterator it = queue.begin();
    while (it != queue.end())
    {
      if (*it == thd)
      {
        return true;
      }
      it++;
    }
    queue.push_back(thd);
    mysql_cond_signal(&COND_wsrep_thd_queue);
    return false;
  }
  THD* pop_front()
  {
    wsp::auto_lock lock(&LOCK_wsrep_thd_queue);
    while (queue.empty())
    {
      if (thd->killed != NOT_KILLED)
        return NULL;

      thd->mysys_var->current_mutex= &LOCK_wsrep_thd_queue;
      thd->mysys_var->current_cond=  &COND_wsrep_thd_queue;

      mysql_cond_wait(&COND_wsrep_thd_queue, &LOCK_wsrep_thd_queue);

      thd->mysys_var->current_mutex= 0;
      thd->mysys_var->current_cond=  0;
    }
    THD* ret= queue.front();
    queue.pop_front();
    return ret;
  }
private:
  THD*             thd;
  std::deque<THD*> queue;
  mysql_mutex_t    LOCK_wsrep_thd_queue;
  mysql_cond_t     COND_wsrep_thd_queue;
};

int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, void *, system_status_var *,
                          enum enum_var_type scope);
bool wsrep_create_appliers(long threads, bool mutex_protected=false);
void wsrep_create_rollbacker();

bool wsrep_bf_abort(THD* bf_thd, THD* victim_thd);
/*
  Abort transaction for victim_thd. This function is called from
  MDL BF abort codepath.
*/
void wsrep_abort_thd(THD *bf_thd,
                     THD *victim_thd,
                     my_bool signal) __attribute__((nonnull(1,2)));

/**
  Kill wsrep connection with kill_signal. Object thd is not
  guaranteed to exist anymore when this function returns.

  Asserts that the caller holds victim_thd->LOCK_thd_kill,
  victim_thd->LOCK_thd_data.

  @param thd THD object for connection that executes the KILL.
  @param victim_thd THD object for connection to be killed.
  @param kill_signal Kill signal.

  @return Zero if the kill was successful, otherwise non-zero error code.
 */
uint wsrep_kill_thd(THD *thd, THD *victim_thd, killed_state kill_signal);

/*
  Backup kill status for commit.
 */
void wsrep_backup_kill_for_commit(THD *);

/*
  Restore KILL status after commit.
 */
void wsrep_restore_kill_after_commit(THD *);

/*
  Helper methods to deal with thread local storage.
  The purpose of these methods is to hide the details of thread
  local storage handling when operating with wsrep storage access
  and streaming applier THDs

  With one-thread-per-connection thread handling thread specific
  variables are allocated when the thread is started and deallocated
  before thread exits (my_thread_init(), my_thread_end()). However,
  with pool-of-threads thread handling new thread specific variables
  are allocated for each THD separately (see threadpool_add_connection()),
  and the variables in thread local storage are assigned from
  currently active thread (see thread_attach()). This must be taken into
  account when storing/resetting thread local storage and when creating
  streaming applier THDs.
*/

/**
   Create new variables for thread local storage. With
   one-thread-per-connection thread handling this is a no op,
   with pool-of-threads new variables are created via my_thread_init().
   It is assumed that the caller has called wsrep_reset_threadvars() to clear
   the thread local storage before this call.

   @return Zero in case of success, non-zero otherwise.
*/
int wsrep_create_threadvars();

/**
   Delete variables which were created by wsrep_create_threadvars().
   The caller must store variables into thread local storage before
   this call via wsrep_store_threadvars().
*/
void wsrep_delete_threadvars();

/**
   Assign variables from current thread local storage into THD.
   This should be called for THDs whose lifetime is limited to single
   thread execution or which may share the operation context with some
   parent THD (e.g. storage access) and thus don't require separately
   allocated globals.

   With one-thread-per-connection thread handling this is a no-op,
   with pool-of-threads the variables which are currently stored into
   thread local storage are assigned to THD.
*/
void wsrep_assign_from_threadvars(THD *);

/**
   Helper struct to save variables from thread local storage.
 */
struct Wsrep_threadvars
{
  THD* cur_thd;
  st_my_thread_var* mysys_var;
};

/**
   Save variables from thread local storage into Wsrep_threadvars struct.
 */
Wsrep_threadvars wsrep_save_threadvars();

/**
   Restore variables into thread local storage from Wsrep_threadvars struct.
*/
void wsrep_restore_threadvars(const Wsrep_threadvars&);

/**
   Store variables into thread local storage.
*/
void wsrep_store_threadvars(THD *);

/**
   Reset thread local storage.
*/
void wsrep_reset_threadvars(THD *);

/**
   Helper functions to override error status

   In many contexts it is desirable to mask the original error status
   set for THD or it is necessary to change OK status to error.
   This function implements the common logic for the most
   of the cases.

   Rules:
   * If the diagnostics are has OK or EOF status, override it unconditionally
   * If the error is either ER_ERROR_DURING_COMMIT or ER_LOCK_DEADLOCK
     it is usually the correct error status to be returned to client,
     so don't override those by default
 */

static inline void wsrep_override_error(THD *thd, uint error, const char *format= 0, ...)
{
  Diagnostics_area *da= thd->get_stmt_da();
  if (da->is_ok() ||
      da->is_eof() ||
      !da->is_set() ||
      (da->is_error() &&
       da->sql_errno() != error &&
       da->sql_errno() != ER_ERROR_DURING_COMMIT &&
       da->sql_errno() != ER_LOCK_DEADLOCK))
  {
    da->reset_diagnostics_area();
    va_list args;
    va_start(args, format);
    if (!format) format= ER_THD(thd, error);
    my_printv_error(error, format, MYF(0), args);
    va_end(args);
  }
}

static inline void wsrep_override_error(THD* thd,
                                        wsrep::client_error ce,
                                        enum wsrep::provider::status status)
{
  DBUG_ASSERT(ce != wsrep::e_success);
  switch (ce)
  {
  case wsrep::e_error_during_commit:
    if (status == wsrep::provider::error_size_exceeded)
      wsrep_override_error(thd, ER_UNKNOWN_ERROR, "Maximum writeset size exceeded");
    else
      /* TODO: Figure out better error number */
      if (status)
        wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                             "Error while appending streaming replication fragment"
                             "(provider status: %s)",
                             wsrep::provider::to_string(status).c_str());
      else
        wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                             "Error while appending streaming replication fragment");
    break;
  case wsrep::e_deadlock_error:
    switch (thd->lex->sql_command)
    {
    case SQLCOM_XA_END:
    case SQLCOM_XA_PREPARE:
      wsrep_override_error(thd, ER_XA_RBDEADLOCK);
      break;
    default:
      wsrep_override_error(thd, ER_LOCK_DEADLOCK);
      break;
    }
    break;
  case wsrep::e_interrupted_error:
    wsrep_override_error(thd, ER_QUERY_INTERRUPTED);
    break;
  case wsrep::e_size_exceeded_error:
    wsrep_override_error(thd, ER_UNKNOWN_ERROR, "Maximum writeset size exceeded");
    break;
  case wsrep::e_append_fragment_error:
    /* TODO: Figure out better error number */
    if (status)
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                           "Error while appending streaming replication fragment"
                           "(provider status: %s)",
                           wsrep::provider::to_string(status).c_str());
    else
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT,
                           "Error while appending streaming replication fragment");
    break;
  case wsrep::e_not_supported_error:
    wsrep_override_error(thd, ER_NOT_SUPPORTED_YET);
    break;
  case wsrep::e_timeout_error:
    wsrep_override_error(thd, ER_LOCK_WAIT_TIMEOUT);
    break;
  default:
    wsrep_override_error(thd, ER_UNKNOWN_ERROR);
  }
}

/**
   Helper function to log THD wsrep context.

   @param thd Pointer to THD
   @param message Optional message
   @param function Function where the call was made from
 */
static inline void wsrep_log_thd(const THD *thd,
                                 const char *message,
                                 const char *function)
{
  WSREP_DEBUG("%s %s\n"
              "    thd: %llu thd_ptr: %p client_mode: %s client_state: %s trx_state: %s\n"
              "    next_trx_id: %lld trx_id: %lld seqno: %lld\n"
              "    is_streaming: %d fragments: %zu\n"
              "    sql_errno: %u message: %s\n"
#define WSREP_THD_LOG_QUERIES
#ifdef WSREP_THD_LOG_QUERIES
              "    command: %d query: %.72s"
#endif /* WSREP_OBSERVER_LOG_QUERIES */
              ,
              function,
              message ? message : "",
              thd->thread_id,
              thd,
              wsrep_thd_client_mode_str(thd),
              wsrep_thd_client_state_str(thd),
              wsrep_thd_transaction_state_str(thd),
              (long long)thd->wsrep_next_trx_id(),
              (long long)thd->wsrep_trx_id(),
              (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_trx().is_streaming(),
              thd->wsrep_sr().fragments().size(),
              (thd->get_stmt_da()->is_error() ? thd->get_stmt_da()->sql_errno() : 0),
              (thd->get_stmt_da()->is_error() ? thd->get_stmt_da()->message() : "")
#ifdef WSREP_THD_LOG_QUERIES
              , thd->lex->sql_command,
              wsrep_thd_query(thd)
#endif /* WSREP_OBSERVER_LOG_QUERIES */
              );
}

#define WSREP_LOG_THD(thd_, message_) wsrep_log_thd(thd_, message_, __FUNCTION__)

#endif /* WSREP_THD_H */
