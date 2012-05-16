/*
  TODO: MP AB Copyright
*/


#ifdef MY_APC_STANDALONE

#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>

#else

#include "sql_priv.h"
#include "sql_class.h"

#endif

//#include "my_apc.h"

/*
  Standalone testing:
    g++ -c -DMY_APC_STANDALONE -g -I.. -I../include -o my_apc.o my_apc.cc
    g++ -L../mysys -L../dbug -L../strings my_apc.o -lmysys -ldbug -lmystrings -lpthread -lrt
*/


void Apc_target::init()
{
  // todo: should use my_pthread_... functions instead?
  DBUG_ASSERT(!enabled);
  (void)pthread_mutex_init(&LOCK_apc_queue, MY_MUTEX_INIT_SLOW);

#ifndef DBUG_OFF
  n_calls_processed= 0;
#endif
}


void Apc_target::destroy()
{
  DBUG_ASSERT(!enabled);
  pthread_mutex_destroy(&LOCK_apc_queue);
}


void Apc_target::enable()
{
  pthread_mutex_lock(&LOCK_apc_queue);
  enabled++;
  pthread_mutex_unlock(&LOCK_apc_queue);
}


void Apc_target::disable()
{
  bool process= FALSE;
  pthread_mutex_lock(&LOCK_apc_queue);
  if (!(--enabled))
    process= TRUE;
  pthread_mutex_unlock(&LOCK_apc_queue);
  if (process)
    process_apc_requests();
}

void Apc_target::enqueue_request(Call_request *qe)
{
  //call_queue_size++;
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

void Apc_target::dequeue_request(Call_request *qe)
{
  //call_queue_size--;
  if (apc_calls == qe)
  {
    if ((apc_calls= apc_calls->next) == qe)
    {
      //DBUG_ASSERT(!call_queue_size);
      apc_calls= NULL;
    }
  }

  qe->prev->next= qe->next;
  qe->next->prev= qe->prev;
}


/*
  Make an apc call in another thread. The caller is responsible so 
  that we're not calling to ourselves.
  
  psergey-todo: Should waits here be KILLable? (it seems one needs 
  to use thd->enter_cond() calls to be killable)
*/

bool Apc_target::make_apc_call(apc_func_t func, void *func_arg, 
                               int timeout_sec, bool *timed_out)
{
  bool res= TRUE;
  *timed_out= FALSE;

  pthread_mutex_lock(&LOCK_apc_queue);
  if (enabled)
  {
    /* Create and post the request */
    Call_request apc_request;
    apc_request.func= func;
    apc_request.func_arg= func_arg;
    apc_request.done= FALSE;
    (void)pthread_cond_init(&apc_request.COND_request, NULL);
    (void)pthread_mutex_init(&apc_request.LOCK_request, MY_MUTEX_INIT_SLOW);
    pthread_mutex_lock(&apc_request.LOCK_request);
    enqueue_request(&apc_request);
    apc_request.what="enqueued by make_apc_call";
    pthread_mutex_unlock(&LOCK_apc_queue);
 
    struct timespec abstime;
    const int timeout= timeout_sec;
    set_timespec(abstime, timeout);
    
    int wait_res= 0;
    /* todo: how about processing other errors here? */
    while (!apc_request.done && (wait_res != ETIMEDOUT))
    {
      wait_res= pthread_cond_timedwait(&apc_request.COND_request,
                                       &apc_request.LOCK_request, &abstime);
    }

    if (!apc_request.done)
    {
      /* We timed out */
      apc_request.done= TRUE;
      *timed_out= TRUE;
      pthread_mutex_unlock(&apc_request.LOCK_request);

      pthread_mutex_lock(&LOCK_apc_queue);
      dequeue_request(&apc_request);
      pthread_mutex_unlock(&LOCK_apc_queue);
      res= TRUE;
    }
    else
    {
      /* Request was successfully executed and dequeued by the target thread */
      pthread_mutex_unlock(&apc_request.LOCK_request);
      res= FALSE;
    }

    /* Destroy all APC request data */
    pthread_mutex_destroy(&apc_request.LOCK_request);
    pthread_cond_destroy(&apc_request.COND_request);
  }
  else
  {
    pthread_mutex_unlock(&LOCK_apc_queue);
  }
  return res;
}


/*
  Process all APC requests
*/

void Apc_target::process_apc_requests()
{
  while (1)
  {
    Call_request *request;
    
    pthread_mutex_lock(&LOCK_apc_queue);
    if (!(request= get_first_in_queue()))
    {
      pthread_mutex_unlock(&LOCK_apc_queue);
      break;
    }

    request->what="seen by process_apc_requests";
    pthread_mutex_lock(&request->LOCK_request);

    if (request->done)
    {
      /*
        We can get here when
        - the requestor thread has been waiting for this request
        - the wait has timed out
        - it has set request->done=TRUE
        - it has released LOCK_request, because its next action
          will be to remove the request from the queue, however,
          it could not attempt to lock the queue while holding the lock on
          request, because that would deadlock with this function 
          (we here first lock the queue and then lock the request)
      */
      pthread_mutex_unlock(&request->LOCK_request);
      pthread_mutex_unlock(&LOCK_apc_queue);
      fprintf(stderr, "Whoa rare event #1!\n");
      continue;
    }
    /* 
      Remove the request from the queue (we're holding its lock so we can be 
      sure that request owner won't try to remove it)
    */
    request->what="dequeued by process_apc_requests";
    dequeue_request(request);
    request->done= TRUE;

    pthread_mutex_unlock(&LOCK_apc_queue);

    request->func(request->func_arg);
    request->what="func called by process_apc_requests";

#ifndef DBUG_OFF
    n_calls_processed++;
#endif

    pthread_cond_signal(&request->COND_request);

    pthread_mutex_unlock(&request->LOCK_request);
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

int int_rand(int size)
{
  return round (((double)rand() / RAND_MAX) * size);
}

/* An APC-serving thread */
void *test_apc_service_thread(void *ptr)
{
  my_thread_init();
  apc_target.init();
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



