/*
   Copyright (c) 2012, Monty Program Ab

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
  This file does standalone APC system tests.
*/
#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>

#include <stdio.h>

#include <tap.h>

/*
  A fake THD with enter_cond/exit_cond and some other members.
*/
PSI_stage_info stage_show_explain;
class THD 
{
  mysql_mutex_t* thd_mutex; 
public:
  bool killed;

  THD() : killed(FALSE) {}
  inline const char* ENTER_COND(mysql_cond_t *cond, mysql_mutex_t* mutex,
                                PSI_stage_info*, PSI_stage_info*)
  {
    mysql_mutex_assert_owner(mutex);
    thd_mutex= mutex;
    return NULL;
  }
  inline void EXIT_COND(PSI_stage_info*)
  {
    mysql_mutex_unlock(thd_mutex);
  }
};

#include "../sql/my_apc.h"

#define MY_APC_STANDALONE 1
#include "../sql/my_apc.cc"

volatile bool started= FALSE;
volatile bool service_should_exit= FALSE;
volatile bool requestors_should_exit=FALSE;

/*  Counters for APC calls */
int apcs_served= 0;
int apcs_missed=0;
int apcs_timed_out=0;
mysql_mutex_t apc_counters_mutex;

inline void increment_counter(int *var)
{
  mysql_mutex_lock(&apc_counters_mutex);
  *var= *var+1;
  mysql_mutex_unlock(&apc_counters_mutex);
}

volatile bool have_errors= false;

Apc_target apc_target;
mysql_mutex_t target_mutex;

int int_rand(int size)
{
  return (int) (0.5 + ((double)rand() / RAND_MAX) * size);
}

/* 
  APC target thread (the one that will serve the APC requests). We will have
  one target.
*/
void *test_apc_service_thread(void *ptr)
{
  my_thread_init();
  mysql_mutex_init(0, &target_mutex, MY_MUTEX_INIT_FAST);
  apc_target.init(&target_mutex);
  apc_target.enable();
  started= TRUE;
  diag("test_apc_service_thread started");
  while (!service_should_exit)
  {
    //apc_target.disable();
    my_sleep(10000);
    //apc_target.enable();
    for (int i = 0; i < 10 && !service_should_exit; i++)
    {
      apc_target.process_apc_requests();
      my_sleep(int_rand(30));
    }
  }
  apc_target.disable();
  apc_target.destroy();
  mysql_mutex_destroy(&target_mutex);
  my_thread_end();
  pthread_exit(0);
  return NULL;
}


/*
  One APC request (to write 'value' into *where_to)
*/
class Apc_order : public Apc_target::Apc_call
{
public:
  int value;   // The value 
  int *where_to;  // Where to write it
  Apc_order(int a, int *b) : value(a), where_to(b) {}

  void call_in_target_thread()
  {
    my_sleep(int_rand(1000));
    *where_to = value;
    increment_counter(&apcs_served);
  }
};


/*
  APC requestor thread. It makes APC requests, and checks if they were actually
  executed.
*/
void *test_apc_requestor_thread(void *ptr)
{
  my_thread_init();
  diag("test_apc_requestor_thread started");
  THD my_thd;

  while (!requestors_should_exit)
  {
    int dst_value= 0;
    int src_value= int_rand(4*1000*100);
    /* Create an APC to do "dst_value= src_value" assignment */
    Apc_order apc_order(src_value, &dst_value);
    bool timed_out;

    mysql_mutex_lock(&target_mutex);
    bool res= apc_target.make_apc_call(&my_thd, &apc_order, 60, &timed_out);
    if (res)
    {
      if (timed_out)
        increment_counter(&apcs_timed_out);
      else
        increment_counter(&apcs_missed);

      if (dst_value != 0)
      {
        diag("APC was done even though return value says it wasnt!");
        have_errors= true;
      }
    }
    else
    {
      if (dst_value != src_value)
      {
        diag("APC was not done even though return value says it was!");
        have_errors= true;
      }
    }
    //my_sleep(300);
  }
  diag("test_apc_requestor_thread exiting");
  my_thread_end();
  return NULL;
}

/* Number of APC requestor threads */
const int N_THREADS=23;


int main(int args, char **argv)
{
  pthread_t service_thr;
  pthread_t request_thr[N_THREADS];
  int i;

  my_thread_global_init();

  mysql_mutex_init(0, &apc_counters_mutex, MY_MUTEX_INIT_FAST);

  plan(1);
  diag("Testing APC delivery and execution");

  pthread_create(&service_thr, NULL, test_apc_service_thread, (void*)NULL);
  while (!started)
    my_sleep(1000);
  for (i = 0; i < N_THREADS; i++)
    pthread_create(&request_thr[i], NULL, test_apc_requestor_thread, (void*)NULL);
  
  for (i = 0; i < 15; i++)
  {
    my_sleep(500*1000);
    diag("%d APCs served %d missed", apcs_served, apcs_missed);
  }
  diag("Shutting down requestors");
  requestors_should_exit= TRUE;
  for (i = 0; i < N_THREADS; i++)
    pthread_join(request_thr[i], NULL);
  
  diag("Shutting down service");
  service_should_exit= TRUE;
  pthread_join(service_thr, NULL);

  mysql_mutex_destroy(&apc_counters_mutex);

  diag("Done");
  my_thread_end();
  my_thread_global_end();

  ok1(!have_errors);
  return exit_status();
}

