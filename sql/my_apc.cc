/*
   Copyright (c) 2011, 2013 Monty Program Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


#ifndef MY_APC_STANDALONE

#include "mariadb.h"
#include "sql_class.h"

#endif

/* For standalone testing of APC system, see unittest/sql/my_apc-t.cc */

/* 
  Initialize the target. 
   
  @note 
  Initialization must be done prior to enabling/disabling the target, or making
  any call requests to it.
  Initial state after initialization is 'disabled'.
*/
void Apc_target::init(mysql_mutex_t *target_mutex)
{
  DBUG_ASSERT(!enabled);
  LOCK_thd_kill_ptr= target_mutex;
#ifndef DBUG_OFF
  n_calls_processed= 0;
#endif
}


/* [internal] Put request qe into the request list */

void Apc_target::enqueue_request(Call_request *qe)
{
  mysql_mutex_assert_owner(LOCK_thd_kill_ptr);
  Call_request *old_apc_calls= apc_calls;
  if (old_apc_calls)
  {
    Call_request *after= old_apc_calls->prev;
    qe->next= old_apc_calls;
    old_apc_calls->prev= qe;
     
    qe->prev= after;
    after->next= qe;
  }
  else
  {
    apc_calls= qe;
    qe->next= qe->prev= qe;
  }
}


/* 
  Remove request qe from the request queue.
  The request is not necessarily first in the queue.
*/

void Apc_target::dequeue_request(Call_request *qe)
{
  mysql_mutex_assert_owner(LOCK_thd_kill_ptr);
  Call_request *old_apc_calls= apc_calls;
  if (old_apc_calls == qe)
  {
    Call_request *next= old_apc_calls->next;
    apc_calls= next;
    if (next == qe)
    {
      apc_calls= NULL;
    }
  }

  qe->prev->next= qe->next;
  qe->next->prev= qe->prev;
}

#ifdef HAVE_PSI_INTERFACE

/* One key for all conds */
PSI_cond_key key_APC_request_COND;
PSI_cond_key key_APC_request_LOCK;

static PSI_cond_info apc_request_psi_conds[]=
{
  { &key_APC_request_COND, "apc_request", 0 /* not using PSI_FLAG_GLOBAL*/ }
};

static PSI_mutex_info apc_request_psi_locks[]=
{
  { &key_APC_request_LOCK, "apc_request", 0 }
};

void init_show_explain_psi_keys(void)
{
  if (PSI_server == NULL)
    return;

  PSI_server->register_cond("sql", apc_request_psi_conds,
                            array_elements(apc_request_psi_conds));
  PSI_server->register_mutex("sql", apc_request_psi_locks,
                            array_elements(apc_request_psi_locks));
}
#endif

/**
  Wait gracefully until the request is completed.
  @retval 0 -- Success
  @retval 1 -- Timeout
 */
int Apc_target::wait_for_completion(THD *caller_thd, Call_request *apc_request,
                                    int timeout_sec)
{
  struct timespec abstime;
  const int timeout= timeout_sec;
  set_timespec(abstime, timeout);

  DBUG_EXECUTE_IF("apc_timeout", set_timespec_nsec(abstime, 1000000););
  int res = 1;
  int wait_res= 0;
  PSI_stage_info old_stage;

  mysql_mutex_lock(&apc_request->LOCK_request);
  mysql_mutex_unlock(LOCK_thd_kill_ptr);

  caller_thd->ENTER_COND(&apc_request->COND_request, &apc_request->LOCK_request,
                         &stage_show_explain, &old_stage);
  /* todo: how about processing other errors here? */
  while (!apc_request->processed && (wait_res != ETIMEDOUT))
  {
    wait_res= mysql_cond_timedwait(&apc_request->COND_request,
                                   &apc_request->LOCK_request, &abstime);
    if (caller_thd->killed)
      break;
  }

  if (!apc_request->processed)
  {
    /*
      The wait has timed out, or this thread was KILLed.
      We can't delete it from the queue, because LOCK_thd_kill_ptr is already
      released. It can't be reacquired because of the ordering with
      apc_request->LOCK_request.
      However, apc_request->processed is guarded by this lock.
      Set processed= TRUE and transfer the ownership to the processor thread.
      It should free the resources itself.
      apc_request cannot be referred after unlock anymore in this case.
    */
    apc_request->processed= TRUE;
    res= 1;
  }
  else
  {
    /* Request was successfully executed and dequeued by the target thread */
    res= 0;
  }

  /* EXIT_COND() will call mysql_mutex_unlock(LOCK_request) for us */
  caller_thd->EXIT_COND(&old_stage);

  return res;
}

/** Create and post the request */
void Apc_target::enqueue_request(Call_request *request_buff, Apc_call *call)
{
  request_buff->call= call;
  request_buff->processed= FALSE;
  enqueue_request(request_buff);
  request_buff->what="enqueued by make_apc_call";
}

/**
  Make an APC (Async Procedure Call) to another thread. 
 
  @detail
  Make an APC call: schedule it for execution and wait until the target
  thread has executed it. 

  - The caller is responsible for making sure he's not posting request
    to the thread he's calling this function from.

  - The caller must have locked target_mutex. The function will release it.

  @retval FALSE - Ok, the call has been made
  @retval TRUE  - Call wasnt made (either the target is in disabled state or
                    timeout occurred)
*/

bool Apc_target::make_apc_call(THD *caller_thd, Apc_call *call,
                               int timeout_sec, bool *timed_out)
{
  bool res= TRUE;
  *timed_out= FALSE;

  if (enabled)
  {
    Call_request *apc_request= new Call_request;
    enqueue_request(apc_request, call);
    res= wait_for_completion(caller_thd, apc_request, timeout_sec);
    *timed_out= res;
    if (!*timed_out)
      delete apc_request;
  }
  else
  {
#ifndef DBUG_OFF
    /* We didn't make the call, because the target is disabled */
    n_calls_processed++;
#endif
    mysql_mutex_unlock(LOCK_thd_kill_ptr);
  }
  return res;
}


/*
  Process all APC requests.
  This should be called periodically by the APC target thread.
*/

void Apc_target::process_apc_requests()
{
  mysql_mutex_lock(LOCK_thd_kill_ptr);

  while (Call_request *request= get_first_in_queue())
  {
    /*
      Remove the request from the queue (we're holding queue lock so we can be
      sure that request owner won't try to remove it)
    */
    request->what= "dequeued by process_apc_requests";
    dequeue_request(request);

    mysql_mutex_lock(&request->LOCK_request);
    if (likely(!request->processed))
    {
      request->processed= TRUE;
      request->call->call_in_target_thread();
      request->what="func called by process_apc_requests";
      mysql_cond_signal(&request->COND_request);
      mysql_mutex_unlock(&request->LOCK_request);
    }
    else
    {
      mysql_mutex_unlock(&request->LOCK_request);
      // The request was timed out.
      // The ownership is passed and it should be freed.
      delete request;
    }

    mysql_mutex_unlock(LOCK_thd_kill_ptr);
    mysql_mutex_lock(LOCK_thd_kill_ptr);

#ifndef DBUG_OFF
    n_calls_processed++;
#endif
  }
  mysql_mutex_unlock(LOCK_thd_kill_ptr);
}

Apc_target::Call_request::Call_request()
{
  mysql_cond_init(key_APC_request_COND, &COND_request, NULL);
  mysql_mutex_init(key_APC_request_LOCK, &LOCK_request, NULL);
}

Apc_target::Call_request::~Call_request()
{
  mysql_cond_destroy(&COND_request);
  mysql_mutex_destroy(&LOCK_request);
}
