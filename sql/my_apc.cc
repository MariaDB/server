/*
  TODO: MP AB Copyright
*/


#ifdef MY_APC_STANDALONE

#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>

#include "my_apc.h"

#else

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

  - The caller is responsible for making sure he's not calling to the same
    thread.

  - The caller should have locked target_thread_mutex.


  psergey-todo: Should waits here be KILLable? (it seems one needs 
  to use thd->enter_cond() calls to be killable)
*/

bool Apc_target::make_apc_call(apc_func_t func, void *func_arg, 
                               int timeout_sec, bool *timed_out)
{
  bool res= TRUE;
  *timed_out= FALSE;

  if (enabled)
  {
    /* Create and post the request */
    Call_request apc_request;
    apc_request.func= func;
    apc_request.func_arg= func_arg;
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

    request->func(request->func_arg);
    request->what="func called by process_apc_requests";

#ifndef DBUG_OFF
    n_calls_processed++;
#endif
    mysql_cond_signal(&request->COND_request);
    mysql_mutex_unlock(LOCK_thd_data_ptr);
  }
}

/*****************************************************************************
 * Testing 
 *****************************************************************************/
#ifdef MY_APC_STANDALONE

volatile bool started= FALSE;
volatile bool service_should_exit= FALSE;
volatile bool requestors_should_exit=FALSE;

volatile int apcs_served= 0;
volatile int apcs_missed=0;
volatile int apcs_timed_out=0;

Apc_target apc_target;
mysql_mutex_t target_mutex;

int int_rand(int size)
{
  return round (((double)rand() / RAND_MAX) * size);
}

/* An APC-serving thread */
void *test_apc_service_thread(void *ptr)
{
  my_thread_init();
  mysql_mutex_init(0, &target_mutex, MY_MUTEX_INIT_FAST);
  apc_target.init(&target_mutex);
  apc_target.enable();
  started= TRUE;
  fprintf(stderr, "# test_apc_service_thread started\n");
  while (!service_should_exit)
  {
    //apc_target.disable();
    usleep(10000);
    //apc_target.enable();
    for (int i = 0; i < 10 && !service_should_exit; i++)
    {
      apc_target.process_apc_requests();
      usleep(int_rand(30));
    }
  }
  apc_target.disable();
  apc_target.destroy();
  my_thread_end();
  pthread_exit(0);
}

class Apc_order
{
public:
  int value;   // The value 
  int *where_to;  // Where to write it
  Apc_order(int a, int *b) : value(a), where_to(b) {}
};

void test_apc_func(void *arg)
{
  Apc_order *order=(Apc_order*)arg;
  usleep(int_rand(1000));
  *(order->where_to) = order->value;
  __sync_fetch_and_add(&apcs_served, 1);
}

void *test_apc_requestor_thread(void *ptr)
{
  my_thread_init();
  fprintf(stderr, "# test_apc_requestor_thread started\n");
  while (!requestors_should_exit)
  {
    int dst_value= 0;
    int src_value= int_rand(4*1000*100);
    /* Create APC to do  dst_value= src_value */
    Apc_order apc_order(src_value, &dst_value);
    bool timed_out;

    bool res= apc_target.make_apc_call(test_apc_func, (void*)&apc_order, 60, &timed_out);
    if (res)
    {
      if (timed_out)
        __sync_fetch_and_add(&apcs_timed_out, 1);
      else
        __sync_fetch_and_add(&apcs_missed, 1);

      if (dst_value != 0)
        fprintf(stderr, "APC was done even though return value says it wasnt!\n");
    }
    else
    {
      if (dst_value != src_value)
        fprintf(stderr, "APC was not done even though return value says it was!\n");
    }
    //usleep(300);
  }
  fprintf(stderr, "# test_apc_requestor_thread exiting\n");
  my_thread_end();
}

const int N_THREADS=23;
int main(int args, char **argv)
{
  pthread_t service_thr;
  pthread_t request_thr[N_THREADS];
  int i, j;
  my_thread_global_init();

  pthread_create(&service_thr, NULL, test_apc_service_thread, (void*)NULL);
  while (!started)
    usleep(1000);
  for (i = 0; i < N_THREADS; i++)
    pthread_create(&request_thr[i], NULL, test_apc_requestor_thread, (void*)NULL);
  
  for (i = 0; i < 15; i++)
  {
    usleep(500*1000);
    fprintf(stderr, "# %d APCs served %d missed\n", apcs_served, apcs_missed);
  }
  fprintf(stderr, "# Shutting down requestors\n");
  requestors_should_exit= TRUE;
  for (i = 0; i < N_THREADS; i++)
    pthread_join(request_thr[i], NULL);
  
  fprintf(stderr, "# Shutting down service\n");
  service_should_exit= TRUE;
  pthread_join(service_thr, NULL);
  fprintf(stderr, "# Done.\n");
  my_thread_end();
  my_thread_global_end();
  return 0;
}

#endif // MY_APC_STANDALONE



