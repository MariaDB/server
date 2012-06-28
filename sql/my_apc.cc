/*
   Copyright (c) 2009, 2011, Monty Program Ab

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


#ifndef MY_APC_STANDALONE

#include "sql_priv.h"
#include "sql_class.h"

#endif

/*
  Standalone testing:
    g++ -c -DMY_APC_STANDALONE -g -I.. -I../include -o my_apc.o my_apc.cc
    g++ -L../mysys -L../dbug -L../strings my_apc.o -lmysys -ldbug -lmystrings -lpthread -lrt
*/


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


/* 
  Destroy the target. The target must be disabled when this call is made.
*/
void Apc_target::destroy()
{
  DBUG_ASSERT(!enabled);
}


/* 
  Enter ther state where the target is available for serving APC requests
*/
void Apc_target::enable()
{
  /* Ok to do without getting/releasing the mutex: */
  enabled++;
}


/* 
  Make the target unavailable for serving APC requests. 
  
  @note
    This call will serve all requests that were already enqueued
*/

void Apc_target::disable()
{
  bool process= FALSE;
  mysql_mutex_lock(LOCK_thd_data_ptr);
  if (!(--enabled))
    process= TRUE;
  mysql_mutex_unlock(LOCK_thd_data_ptr);
  if (process)
    process_apc_requests();
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
                    timeout occured)

  psergey-todo: Should waits here be KILLable? (it seems one needs 
  to use thd->enter_cond() calls to be killable)
*/

bool Apc_target::make_apc_call(Apc_call *call, int timeout_sec, 
                               bool *timed_out)
{
  bool res= TRUE;
  *timed_out= FALSE;

  if (enabled)
  {
    /* Create and post the request */
    Call_request apc_request;
    apc_request.call= call;
    apc_request.processed= FALSE;
    mysql_cond_init(0 /* do not track in PS */, &apc_request.COND_request, NULL);
    enqueue_request(&apc_request);
    apc_request.what="enqueued by make_apc_call";
 
    struct timespec abstime;
    const int timeout= timeout_sec;
    set_timespec(abstime, timeout);

    int wait_res= 0;
    /* todo: how about processing other errors here? */
    while (!apc_request.processed && (wait_res != ETIMEDOUT))
    {
      /* We own LOCK_thd_data_ptr */
      wait_res= mysql_cond_timedwait(&apc_request.COND_request,
                                     LOCK_thd_data_ptr, &abstime);
                                      // &apc_request.LOCK_request, &abstime);
    }

    if (!apc_request.processed)
    {
      /* 
        The wait has timed out. Remove the request from the queue (ok to do
        because we own LOCK_thd_data_ptr.
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
    mysql_mutex_unlock(LOCK_thd_data_ptr);

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
  if (!get_first_in_queue())
    return;

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

