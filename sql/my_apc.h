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

/*
  Target for asynchronous procedue calls (APCs).
*/
class Apc_target
{
  mysql_mutex_t *LOCK_thd_data_ptr;
public:
  Apc_target() : enabled(0), apc_calls(NULL) {} 
  ~Apc_target() { DBUG_ASSERT(!enabled && !apc_calls);}

  void init(mysql_mutex_t *target_mutex);
  void destroy();
  void enable();
  void disable();
  
  void process_apc_requests();

  class Apc_call
  {
  public:
    virtual void call_in_target_thread()= 0;
    virtual ~Apc_call() {}
  };
  /*
    Make an APC call: schedule it for execution and wait until the target
    thread has executed it. This function must not be called from a thread
    that's different from the target thread.

    @retval FALSE - Ok, the call has been made
    @retval TRUE  - Call wasnt made (either the target is in disabled state or
                    timeout occured)
  */
  bool make_apc_call(Apc_call *call, int timeout_sec, bool *timed_out);

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
     - process requests sequentially (i.e. they are removed from the front)
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

///////////////////////////////////////////////////////////////////////

