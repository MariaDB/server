/*
   Copyright (c) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 or later of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Implementation if OS independent timers.
  This is done based on pthread primitives, especially pthread_cond_timedwait()
*/

#include "mysys_priv.h"
#include "thr_timer.h"
#include <m_string.h>
#include <queues.h>
#ifdef HAVE_TIMER_CREATE
#include <sys/syscall.h>
#endif

struct timespec next_timer_expire_time;

static my_bool thr_timer_inited= 0;
static mysql_mutex_t LOCK_timer;
static mysql_cond_t  COND_timer;
static QUEUE timer_queue;
pthread_t timer_thread;

#define set_max_time(abs_time) \
  { (abs_time)->MY_tv_sec= INT_MAX32; (abs_time)->MY_tv_nsec= 0; }


static void *timer_handler(void *arg __attribute__((unused)));

/*
  Compare two timespecs
*/

static int compare_timespec(void *not_used __attribute__((unused)),
                            uchar *a_ptr, uchar *b_ptr)
{
  return cmp_timespec((*(struct timespec*) a_ptr),
                      (*(struct timespec*) b_ptr));
}


/**
  Initialize timer variables and create timer thread

  @param alloc_timers	Init allocation of timers. Will be autoextended
		        if needed
  @return 0 ok
  @return 1 error; Can't create thread
*/

static thr_timer_t max_timer_data;

my_bool init_thr_timer(uint alloc_timers)
{
  pthread_attr_t thr_attr;
  my_bool res= 0;
  DBUG_ENTER("init_thr_timer");

  init_queue(&timer_queue, alloc_timers+2, offsetof(thr_timer_t,expire_time),
             0, compare_timespec, NullS,
             offsetof(thr_timer_t, index_in_queue)+1, 1);
  mysql_mutex_init(key_LOCK_timer, &LOCK_timer, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_timer, &COND_timer, NULL);

  /* Set dummy element with max time into the queue to simplify usage */
  bzero(&max_timer_data, sizeof(max_timer_data));
  set_max_time(&max_timer_data.expire_time);
  queue_insert(&timer_queue, (uchar*) &max_timer_data);
  next_timer_expire_time= max_timer_data.expire_time;

  /* Create a thread to handle timers */
  pthread_attr_init(&thr_attr);
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_PROCESS);
  pthread_attr_setstacksize(&thr_attr,64*1024);
  thr_timer_inited= 1;
  if (mysql_thread_create(key_thread_timer, &timer_thread, &thr_attr,
                          timer_handler, NULL))
  {
    thr_timer_inited= 0;
    res= 1;
    mysql_mutex_destroy(&LOCK_timer);
    mysql_cond_destroy(&COND_timer);
    delete_queue(&timer_queue);
  }
  pthread_attr_destroy(&thr_attr);

  DBUG_RETURN(res);
}


void end_thr_timer(void)
{
  DBUG_ENTER("end_thr_timer");

  if (!thr_timer_inited)
    DBUG_VOID_RETURN;

  mysql_mutex_lock(&LOCK_timer);
  thr_timer_inited= 0;                          /* Signal abort */
  mysql_cond_signal(&COND_timer);
  mysql_mutex_unlock(&LOCK_timer);
  pthread_join(timer_thread, NULL);

  mysql_mutex_destroy(&LOCK_timer);
  mysql_cond_destroy(&COND_timer);
  delete_queue(&timer_queue);
  DBUG_VOID_RETURN;
}


/*
  Initialize a timer object

  @param timer_data	Timer structure
  @param function	Function to be called when getting timeout
  @param argument	Argument for function
*/

void thr_timer_init(thr_timer_t *timer_data, void(*function)(void*),
                    void *arg)
{
  DBUG_ENTER("thr_timer_init");
  bzero(timer_data, sizeof(*timer_data));
  timer_data->func= function;
  timer_data->func_arg= arg;
  timer_data->expired= 1;                       /* Not active */
  DBUG_VOID_RETURN;
}

/*
  Make timer periodic

  @param timer_data     Timer structure
  @param micro_seconds  Period
*/
void thr_timer_set_period(thr_timer_t* timer_data, ulonglong micro_seconds)
{
  DBUG_ENTER("thr_timer_set_period");
  timer_data->period= micro_seconds;
  DBUG_VOID_RETURN;
}

/*
  Request timer after X milliseconds

  SYNOPSIS
    thr_timer()
    timer_data		Pointer to timer structure
    micro_seconds;      Number of microseconds until timer

  RETURN VALUES
    0 ok
    1 If no more timers are allowed (aborted by process)

    Stores in first argument a pointer to a non-zero int which is set to 0
    when the timer has been given
*/

my_bool thr_timer_settime(thr_timer_t *timer_data, ulonglong micro_seconds)
{
  int reschedule;
  DBUG_ENTER("thr_timer_settime");
  DBUG_PRINT("enter",("thread: %s  micro_seconds: %llu",my_thread_name(),
                      micro_seconds));

  DBUG_ASSERT(timer_data->expired == 1);

  set_timespec_nsec(timer_data->expire_time, micro_seconds*1000);
  timer_data->expired= 0;

  mysql_mutex_lock(&LOCK_timer);        /* Lock from threads & timers */
  if (queue_insert_safe(&timer_queue,(uchar*) timer_data))
  {
    DBUG_PRINT("info", ("timer queue full"));
    fprintf(stderr,"Warning: thr_timer queue is full\n");
    timer_data->expired= 1;
    mysql_mutex_unlock(&LOCK_timer);
    DBUG_RETURN(1);
  }    

  /* Reschedule timer if the current one has more time left than new one */
  reschedule= cmp_timespec(next_timer_expire_time, timer_data->expire_time);
  mysql_mutex_unlock(&LOCK_timer);
  if (reschedule > 0)
  {
#if defined(MAIN)
  printf("reschedule\n"); fflush(stdout);
#endif
    DBUG_PRINT("info", ("reschedule"));
    mysql_cond_signal(&COND_timer);
  }

  DBUG_RETURN(0);
}


/*
  Remove timer from list of timers

  notes: Timer will be marked as expired
*/

void thr_timer_end(thr_timer_t *timer_data)
{
  DBUG_ENTER("thr_timer_end");

  mysql_mutex_lock(&LOCK_timer);
  if (!timer_data->expired)
  {
    DBUG_ASSERT(timer_data->index_in_queue != 0);
    DBUG_ASSERT(queue_element(&timer_queue, timer_data->index_in_queue) ==
                (uchar*) timer_data);
    queue_remove(&timer_queue, timer_data->index_in_queue);
    /* Mark as expired for asserts to work */
    timer_data->expired= 1;
  }
  mysql_mutex_unlock(&LOCK_timer);
  DBUG_VOID_RETURN;
}


/*
  Come here when some timer in queue is due.
*/

static sig_handler process_timers(struct timespec *now)
{
  thr_timer_t *timer_data;
  DBUG_ENTER("process_timers");
  DBUG_PRINT("info",("active timers: %d", timer_queue.elements - 1));

#if defined(MAIN)
  printf("process_timer\n"); fflush(stdout);
#endif

  /* We can safely remove the first one as it has already expired */
  for (;;) 
  {
    void (*function)(void*);
    void *func_arg;
    my_bool is_periodic;

    timer_data= (thr_timer_t*) queue_top(&timer_queue);
    function=   timer_data->func;
    func_arg=   timer_data->func_arg;
    is_periodic= timer_data->period != 0;
    timer_data->expired= 1;			/* Mark expired */
    /*
      We remove timer before calling timer function to allow thread to
      delete it's timer data any time.

      Deleting timer inside the callback would not work
      for periodic timers, they need to be removed from
      queue prior to destroying timer_data.
    */
    queue_remove_top(&timer_queue);		/* Remove timer */
    (*function)(func_arg);                      /* Inform thread of timeout */

    /*
      If timer is periodic, recalculate next expiration time, and
      reinsert it into the queue.
    */
    if (is_periodic && timer_data->period)
    {
      set_timespec_nsec(timer_data->expire_time, timer_data->period * 1000);
      timer_data->expired= 0;
      queue_insert(&timer_queue, (uchar*)timer_data);
    }

    /* Check if next one has also expired */
    timer_data= (thr_timer_t*) queue_top(&timer_queue);
    if (cmp_timespec(timer_data->expire_time, (*now)) > 0)
      break;                                    /* All data processed */
  }
  DBUG_VOID_RETURN;
}


/*
  set up a timer thread to handle timeouts
  This will be killed when thr_timer_inited is set to false.
*/

static void *timer_handler(void *arg __attribute__((unused)))
{
  my_thread_init();

  mysql_mutex_lock(&LOCK_timer);
  while (likely(thr_timer_inited))
  {
    int error;
    struct timespec *top_time;
    struct timespec now, abstime;

    set_timespec(now, 0);

    top_time= &(((thr_timer_t*) queue_top(&timer_queue))->expire_time);

    if (cmp_timespec((*top_time), now) <= 0)
    {
      process_timers(&now);
      top_time= &(((thr_timer_t*) queue_top(&timer_queue))->expire_time);
    }

    abstime= *top_time;
    next_timer_expire_time= *top_time;
    if ((error= mysql_cond_timedwait(&COND_timer, &LOCK_timer, &abstime)) &&
        error != ETIME && error != ETIMEDOUT)
    {
#ifdef MAIN
      printf("Got error: %d from ptread_cond_timedwait (errno: %d)\n",
             error,errno);
#endif
    }
  }
  mysql_mutex_unlock(&LOCK_timer);
  my_thread_end();
  pthread_exit(0);
  return 0;					/* Impossible */
}


/****************************************************************************
  Testing of thr_timer (when compiled with -DMAIN)
***************************************************************************/

#ifdef MAIN

static mysql_cond_t COND_thread_count;
static mysql_mutex_t LOCK_thread_count;
static uint thread_count, benchmark_runs, test_to_run= 1;

static void send_signal(void *arg)
{
  struct st_my_thread_var *current_my_thread_var= arg;
#if defined(MAIN)
  printf("sending signal\n"); fflush(stdout);
#endif
  mysql_mutex_lock(&current_my_thread_var->mutex);
  mysql_cond_signal(&current_my_thread_var->suspend);
  mysql_mutex_unlock(&current_my_thread_var->mutex);
}


static void run_thread_test(int param)
{
  int i,wait_time,retry;
  my_hrtime_t start_time;
  thr_timer_t timer_data;
  struct st_my_thread_var *current_my_thread_var;
  DBUG_ENTER("run_thread_test");

  current_my_thread_var= my_thread_var;
  thr_timer_init(&timer_data, send_signal, current_my_thread_var);

  for (i=1 ; i <= 10 ; i++)
  {
    wait_time=param ? 11-i : i;
    start_time= my_hrtime();

    mysql_mutex_lock(&current_my_thread_var->mutex);
    if (thr_timer_settime(&timer_data, wait_time * 1000000))
    {
      printf("Thread: %s  timers aborted\n",my_thread_name());
      break;
    }
    if (wait_time == 3)
    {
      printf("Thread: %s  Simulation of no timer needed\n",my_thread_name());
      fflush(stdout);
    }
    else
    {
      for (retry=0 ; !timer_data.expired && retry < 10 ; retry++)
      {
	printf("Thread: %s  Waiting %d sec\n",my_thread_name(),wait_time);
        mysql_cond_wait(&current_my_thread_var->suspend,
                        &current_my_thread_var->mutex);

      }
      if (!timer_data.expired)
      {
	printf("Thread: %s  didn't get an timer. Aborting!\n",
	       my_thread_name());
	break;
      }
    }
    mysql_mutex_unlock(&current_my_thread_var->mutex);
    printf("Thread: %s  Slept for %g (%d) sec\n",my_thread_name(),
	   (int) (my_hrtime().val-start_time.val)/1000000.0, wait_time);
    fflush(stdout);
    thr_timer_end(&timer_data);
    fflush(stdout);
  }
  DBUG_VOID_RETURN;
}


static void run_thread_benchmark(int param)
{
  int i;
  struct st_my_thread_var *current_my_thread_var;
  thr_timer_t timer_data;
  DBUG_ENTER("run_thread_benchmark");

  current_my_thread_var= my_thread_var;
  thr_timer_init(&timer_data, send_signal, current_my_thread_var);

  for (i=1 ; i <= param ; i++)
  {
    if (thr_timer_settime(&timer_data, 1000000))
    {
      printf("Thread: %s  timers aborted\n",my_thread_name());
      break;
    }
    thr_timer_end(&timer_data);
  }
  DBUG_VOID_RETURN;
}


#ifdef HAVE_TIMER_CREATE

/* Test for benchmarking posix timers against thr_timer */

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id   _sigev_un._tid
#endif

static void run_timer_benchmark(int param)
{
  int i;
  timer_t timerid;
  struct sigevent sigev;
  pid_t thread_id= (pid_t) syscall(SYS_gettid);
  DBUG_ENTER("run_timer_benchmark");

  /* Setup a signal that will never be signaled */
  sigev.sigev_value.sival_ptr= 0;
  sigev.sigev_signo= SIGRTMIN;                  /* First free signal */
  sigev.sigev_notify= SIGEV_SIGNAL | SIGEV_THREAD_ID;
  sigev.sigev_notify_thread_id= thread_id;

  if (timer_create(CLOCK_MONOTONIC, &sigev, &timerid))
  {
    printf("Could not create timer\n");
    exit(1);
  }

  for (i=1 ; i <= param ; i++)
  {
    struct itimerspec abstime;
    abstime.it_interval.tv_sec= 0;
    abstime.it_interval.tv_nsec= 0;
    abstime.it_value.tv_sec= 1;
    abstime.it_value.tv_nsec= 0;

    if (timer_settime(timerid, 0, &abstime, NULL))
    {
      printf("Thread: %s  timers aborted\n",my_thread_name());
      break;
    }
    abstime.it_interval.tv_sec= 0;
    abstime.it_interval.tv_nsec= 0;
    abstime.it_value.tv_sec= 0;
    abstime.it_value.tv_nsec= 0;
    timer_settime(timerid, 0, &abstime, NULL);
  }
  timer_delete(timerid);
  DBUG_VOID_RETURN;
}
#endif /* HAVE_TIMER_CREATE */


static void *start_thread(void *arg)
{
  my_thread_init();
  printf("Thread %d (%s) started\n",*((int*) arg),my_thread_name());
  fflush(stdout);

  switch (test_to_run) {
  case 1:
    run_thread_test(*((int*) arg));
    break;
  case 2:
    run_thread_benchmark(benchmark_runs);
    break;
  case 3:
#ifdef HAVE_TIMER_CREATE
    run_timer_benchmark(benchmark_runs);
#endif
    break;
  }
  free((uchar*) arg);
  mysql_mutex_lock(&LOCK_thread_count);
  thread_count--;
  mysql_cond_signal(&COND_thread_count); /* Tell main we are ready */
  mysql_mutex_unlock(&LOCK_thread_count);
  my_thread_end();
  return 0;
}


/* Start a lot of threads that will run with timers */

static void run_test()
{
  pthread_t tid;
  pthread_attr_t thr_attr;
  int i,*param,error;
  DBUG_ENTER("run_test");

  if (init_thr_timer(5))
  {
    printf("Can't initialize timers\n");
    exit(1);
  }

  mysql_mutex_init(0, &LOCK_thread_count, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &COND_thread_count, NULL);

  thr_setconcurrency(3);
  pthread_attr_init(&thr_attr);
  pthread_attr_setscope(&thr_attr,PTHREAD_SCOPE_PROCESS);
  printf("Main thread: %s\n",my_thread_name());
  for (i=0 ; i < 2 ; i++)
  {
    param=(int*) malloc(sizeof(int));
    *param= i;
    mysql_mutex_lock(&LOCK_thread_count);
    if ((error= mysql_thread_create(0,
                                    &tid, &thr_attr, start_thread,
                                    (void*) param)))
    {
      printf("Can't create thread %d, error: %d\n",i,error);
      exit(1);
    }
    thread_count++;
    mysql_mutex_unlock(&LOCK_thread_count);
  }

  pthread_attr_destroy(&thr_attr);
  mysql_mutex_lock(&LOCK_thread_count);
  while (thread_count)
  {
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  DBUG_ASSERT(timer_queue.elements == 1);
  end_thr_timer();
  printf("Test succeeded\n");
  DBUG_VOID_RETURN;
}


int main(int argc __attribute__((unused)),char **argv __attribute__((unused)))
{
  MY_INIT(argv[0]);

  if (argc > 1 && argv[1][0] == '-')
  {
    switch (argv[1][1]) {
    case '#':
      test_to_run= 1;
      DBUG_PUSH(argv[1]+2);
      break;
    case 'b':
      test_to_run= 2;
      benchmark_runs= atoi(argv[1]+2);
      break;
    case 't':
      test_to_run= 3;
      benchmark_runs= atoi(argv[1]+2);
      break;
    }
  }
  if (!benchmark_runs)
    benchmark_runs= 1000000;

  run_test();
  my_end(1);
  return 0;
}

#endif /* MAIN */
