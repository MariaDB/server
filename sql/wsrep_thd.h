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

#include <my_config.h>

#ifndef WSREP_THD_H
#define WSREP_THD_H

#ifdef WITH_WSREP

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

int  wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr,
                                my_bool signal);

/*
  PA = Parallel Applying (on the slave side)
*/
extern void  wsrep_thd_set_PA_safe(void *thd_ptr, my_bool safe);
//extern my_bool  wsrep_thd_is_BF(THD *thd, my_bool sync);
extern my_bool  wsrep_thd_is_BF(void *thd_ptr, my_bool sync);
extern my_bool wsrep_thd_is_wsrep(void *thd_ptr);

//enum wsrep_conflict_state wsrep_thd_conflict_state(void *thd_ptr, my_bool sync);
extern int wsrep_thd_conflict_state(void *thd_ptr, my_bool sync);
extern "C" my_bool  wsrep_thd_is_BF_or_commit(void *thd_ptr, my_bool sync);
extern "C" my_bool  wsrep_thd_is_local(void *thd_ptr, my_bool sync);
int  wsrep_thd_in_locking_session(void *thd_ptr);
THD* wsrep_start_SR_THD(char *thread_stack);
void wsrep_end_SR_THD(THD* thd);
extern my_bool  wsrep_thd_is_SR(void *thd_ptr);
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
                                        wsrep_status_t status)
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
              "    thd: %lld exec_mode: %s query_state: %s conflict_state: %s\n"
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
              wsrep_thd_exec_mode_str(thd),
              wsrep_thd_query_state_str(thd),
              wsrep_thd_conflict_state_str(thd),
              (long long)thd->wsrep_next_trx_id(),
              (long long)thd->wsrep_trx_id(),
              (long long)thd->wsrep_trx_meta.gtid.seqno,
              thd->wsrep_is_streaming(),
              thd->wsrep_SR_fragments.size(),
              (thd->get_stmt_da()->is_error() ? thd->get_stmt_da()->sql_errno() : 0),
              (thd->get_stmt_da()->is_error() ? thd->get_stmt_da()->message() : "")
#ifdef WSREP_THD_LOG_QUERIES
              , thd->lex->sql_command,
              WSREP_QUERY(thd)
#endif /* WSREP_OBSERVER_LOG_QUERIES */
              );
}

#define WSREP_LOG_THD(thd_, message_) wsrep_log_thd(thd_, message_, __FUNCTION__)

#else /* WITH_WSREP */

#define wsrep_thd_is_BF(T, S) (0)
#define wsrep_abort_thd(X,Y,Z) do { } while(0)
#define wsrep_create_appliers(T) do { } while(0)

#endif
#endif /* WSREP_THD_H */
