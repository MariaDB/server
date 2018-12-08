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

void wsrep_prepare_bf_thd(THD*, struct wsrep_thd_shadow*);
void wsrep_return_from_bf_mode(THD*, struct wsrep_thd_shadow*);

int wsrep_show_bf_aborts (THD *thd, SHOW_VAR *var, char *buff,
                          enum enum_var_type scope);
void wsrep_client_rollback(THD *thd, bool rollbacker = false);
void wsrep_replay_transaction(THD *thd);
void wsrep_create_appliers(long threads);
void wsrep_create_rollbacker();

bool wsrep_bf_abort(const THD*, THD*);
int  wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr,
                                my_bool signal);
extern void  wsrep_thd_set_PA_safe(void *thd_ptr, my_bool safe);
THD* wsrep_start_SR_THD(char *thread_stack);
void wsrep_end_SR_THD(THD* thd);

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

static inline void wsrep_override_error(THD *thd, uint error)
{
  DBUG_ASSERT(error != ER_ERROR_DURING_COMMIT);
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
    my_error(error, MYF(0));
  }
}

/**
   Override error with additional wsrep status.
 */
static inline void wsrep_override_error(THD *thd, uint error,
                                        enum wsrep::provider::status status)
{
  Diagnostics_area *da= thd->get_stmt_da();
  if (da->is_ok() ||
      !da->is_set() ||
      (da->is_error() &&
       da->sql_errno() != error &&
       da->sql_errno() != ER_ERROR_DURING_COMMIT &&
       da->sql_errno() != ER_LOCK_DEADLOCK))
  {
    da->reset_diagnostics_area();
    my_error(error, MYF(0), status);
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
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT, status);
      break;
    case wsrep::e_deadlock_error:
      wsrep_override_error(thd, ER_LOCK_DEADLOCK);
      break;
    case wsrep::e_interrupted_error:
      wsrep_override_error(thd, ER_QUERY_INTERRUPTED);
      break;
    case wsrep::e_size_exceeded_error:
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT, status);
      break;
    case wsrep::e_append_fragment_error:
      /* TODO: Figure out better error number */
      wsrep_override_error(thd, ER_ERROR_DURING_COMMIT, status);
      break;
    case wsrep::e_not_supported_error:
      wsrep_override_error(thd, ER_NOT_SUPPORTED_YET);
      break;
    case wsrep::e_timeout_error:
      wsrep_override_error(thd, ER_LOCK_WAIT_TIMEOUT);
      break;
    default:
      wsrep_override_error(thd, ER_UNKNOWN_ERROR);
      break;
    }
}

/**
   Helper function to log THD wsrep context.

   @param thd Pointer to THD
   @param message Optional message
   @param function Function where the call was made from
 */
static inline void wsrep_log_thd(THD *thd,
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
              WSREP_QUERY(thd)
#endif /* WSREP_OBSERVER_LOG_QUERIES */
              );
}

#define WSREP_LOG_THD(thd_, message_) wsrep_log_thd(thd_, message_, __FUNCTION__)

#endif /* WSREP_THD_H */
