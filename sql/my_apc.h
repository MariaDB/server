#ifndef SQL_MY_APC_INCLUDED
#define SQL_MY_APC_INCLUDED
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

/*
  Interface
  ~~~~~~~~~
   (
    - This is an APC request queue
    - We assume there is a particular owner thread which periodically calls
      process_apc_requests() to serve the call requests.
    - Other threads can post call requests, and block until they are exectued.
  )

  Implementation
  ~~~~~~~~~~~~~~
  - The target has a mutex-guarded request queue.

  - After the request has been put into queue, the requestor waits for request
    to be satisfied. The worker satisifes the request and signals the
    requestor.
*/

class THD;

/*
  Target for asynchronous procedure calls (APCs). 
   - A target is running in some particular thread, 
   - One can make calls to it from other threads.
*/
class Apc_target
{
  mysql_mutex_t *LOCK_thd_kill_ptr;
public:
  Apc_target() : enabled(0), apc_calls(NULL) {} 
  ~Apc_target() { DBUG_ASSERT(!enabled && !apc_calls);}

  void init(mysql_mutex_t *target_mutex);

  /* Destroy the target. The target must be disabled when this call is made. */
  void destroy() { DBUG_ASSERT(!enabled); }

  /* Enter ther state where the target is available for serving APC requests */
  void enable() { enabled++; }

  /*
    Make the target unavailable for serving APC requests.

    @note
      This call will serve all requests that were already enqueued
  */
  void disable()
  {
    DBUG_ASSERT(enabled);
    mysql_mutex_lock(LOCK_thd_kill_ptr);
    bool process= !--enabled && have_apc_requests();
    mysql_mutex_unlock(LOCK_thd_kill_ptr);
    if (unlikely(process))
      process_apc_requests();
  }

  void process_apc_requests();
  /* 
    A lightweight function, intended to be used in frequent checks like this:

      if (apc_target.have_requests()) apc_target.process_apc_requests()
  */
  inline bool have_apc_requests()
  {
    return MY_TEST(apc_calls);
  }

  inline bool is_enabled() { return enabled; }
  
  /* Functor class for calls you can schedule */
  class Apc_call
  {
  public:
    /* This function will be called in the target thread */
    virtual void call_in_target_thread()= 0;
    virtual ~Apc_call() {}
  };
  
  /* Make a call in the target thread (see function definition for details) */
  bool make_apc_call(THD *caller_thd, Apc_call *call, int timeout_sec, bool *timed_out);

#ifndef DBUG_OFF
  int n_calls_processed; /* Number of calls served by this target */
#endif
private:
  class Call_request;

  /* 
    Non-zero value means we're enabled. It's an int, not bool, because one can
    call enable() N times (and then needs to call disable() N times before the 
    target is really disabled)
  */
  int enabled;

  /* 
    Circular, double-linked list of all enqueued call requests. 
    We use this structure, because we 
     - process requests sequentially: requests are added at the end of the 
       list and removed from the front. With circular list, we can keep one
       pointer, and access both front an back of the list with it.
     - a thread that has posted a request may time out (or be KILLed) and 
       cancel the request, which means we need a fast request-removal
       operation.
  */
  Call_request *apc_calls;
 
  class Call_request
  {
  public:
    Apc_call *call; /* Functor to be called */

    /* The caller will actually wait for "processed==TRUE" */
    bool processed;

    /* Condition that will be signalled when the request has been served */
    mysql_cond_t COND_request;
    
    /* Double linked-list linkage */
    Call_request *next;
    Call_request *prev;
    
    const char *what; /* (debug) state of the request */
  };

  void enqueue_request(Call_request *qe);
  void dequeue_request(Call_request *qe);

  /* return the first call request in queue, or NULL if there are none enqueued */
  Call_request *get_first_in_queue()
  {
    return apc_calls;
  }
};

#ifdef HAVE_PSI_INTERFACE
void init_show_explain_psi_keys(void);
#else
#define init_show_explain_psi_keys() /* no-op */
#endif

#endif //SQL_MY_APC_INCLUDED

