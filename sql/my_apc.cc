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
  LOCK_thd_data_ptr= target_mutex;
#ifndef DBUG_OFF
  n_calls_processed= 0;
#endif
}


/* [internal] Put request qe into the request list */

void Apc_target::enqueue_request(Call_request *qe)
{
  mysql_mutex_assert_owner(LOCK_thd_data_ptr);
  if (apc_calls)
  {
    Call_request *after= apc_calls->prev;
    qe->next= apc_calls;
    apc_calls->prev= qe;
     
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
  [internal] Remove request qe from the request queue. 
  
  The request is not necessarily first in the queue.
*/

void Apc_target::dequeue_request(Call_request *qe)
{
  mysql_mutex_assert_owner(LOCK_thd_data_ptr);
  if (apc_calls == qe)
  {
    if ((apc_calls= apc_calls->next) == qe)
    {
      apc_calls= NULL;
    }
  }

  qe->prev->next= qe->next;
  qe->next->prev= qe->prev;
}

#ifdef HAVE_PSI_INTERFACE

/* One key for all conds */
PSI_cond_key key_show_explain_request_COND;

static PSI_cond_info show_explain_psi_conds[]=
{
  { &key_show_explain_request_COND, "show_explain", 0 /* not using PSI_FLAG_GLOBAL*/ }
};

void init_show_explain_psi_keys(void)
{
  if (PSI_server == NULL)
    return;

  PSI_server->register_cond("sql", show_explain_psi_conds, 
                            array_elements(show_explain_psi_conds));
}
#endif


/*
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
    /* Create and post the request */
    Call_request apc_request;
    apc_request.call= call;
    apc_request.processed= FALSE;
    mysql_cond_init(key_show_explain_request_COND, &apc_request.COND_request,
                    NULL);
    enqueue_request(&apc_request);
    apc_request.what="enqueued by make_apc_call";
 
    struct timespec abstime;
    const int timeout= timeout_sec;
    set_timespec(abstime, timeout);

    int wait_res= 0;
    PSI_stage_info old_stage;
    caller_thd->ENTER_COND(&apc_request.COND_request, LOCK_thd_data_ptr,
                           &stage_show_explain, &old_stage);
    /* todo: how about processing other errors here? */
    while (!apc_request.processed && (wait_res != ETIMEDOUT))
    {
      /* We own LOCK_thd_data_ptr */
      wait_res= mysql_cond_timedwait(&apc_request.COND_request,
                                     LOCK_thd_data_ptr, &abstime);
                                      // &apc_request.LOCK_request, &abstime);
      if (caller_thd->killed)
        break;
    }

    if (!apc_request.processed)
    {
      /* 
        The wait has timed out, or this thread was KILLed.
        Remove the request from the queue (ok to do because we own
        LOCK_thd_data_ptr)
      */
      apc_request.processed= TRUE;
      dequeue_request(&apc_request);
      *timed_out= TRUE;
      res= TRUE;
    }
    else
    {
      /* Request was successfully executed and dequeued by the target thread */
      res= FALSE;
    }
    /* 
      exit_cond() will call mysql_mutex_unlock(LOCK_thd_data_ptr) for us:
    */
    caller_thd->EXIT_COND(&old_stage);

    /* Destroy all APC request data */
    mysql_cond_destroy(&apc_request.COND_request);
  }
  else
  {
    mysql_mutex_unlock(LOCK_thd_data_ptr);
  }
  return res;
}


/*
  Process all APC requests.
  This should be called periodically by the APC target thread.
*/

void Apc_target::process_apc_requests()
{
  while (1)
  {
    Call_request *request;
 
    mysql_mutex_lock(LOCK_thd_data_ptr);
    if (!(request= get_first_in_queue()))
    {
      /* No requests in the queue */
      mysql_mutex_unlock(LOCK_thd_data_ptr);
      break;
    }

    /* 
      Remove the request from the queue (we're holding queue lock so we can be 
      sure that request owner won't try to remove it)
    */
    request->what="dequeued by process_apc_requests";
    dequeue_request(request);
    request->processed= TRUE;

    request->call->call_in_target_thread();
    request->what="func called by process_apc_requests";

#ifndef DBUG_OFF
    n_calls_processed++;
#endif
    mysql_cond_signal(&request->COND_request);
    mysql_mutex_unlock(LOCK_thd_data_ptr);
  }
}

