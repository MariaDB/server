/* Copyright (c) 2000, 2011 Oracle and/or its affiliates.
   Copyright 2008-2011 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  Functions to handle initializating and allocationg of all mysys & debug
  thread variables.
*/

#include "mysys_priv.h"
#include <m_string.h>
#include <signal.h>

pthread_key(struct st_my_thread_var*, THR_KEY_mysys);
mysql_mutex_t THR_LOCK_malloc, THR_LOCK_open,
              THR_LOCK_lock, THR_LOCK_myisam, THR_LOCK_heap,
              THR_LOCK_net, THR_LOCK_charset, THR_LOCK_threads,
              THR_LOCK_myisam_mmap;

mysql_cond_t  THR_COND_threads;
uint            THR_thread_count= 0;
uint 		my_thread_end_wait_time= 5;
#if !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R)
mysql_mutex_t LOCK_localtime_r;
#endif
#ifdef _MSC_VER
static void install_sigabrt_handler();
#endif


static uint get_thread_lib(void);

/** True if @c my_thread_global_init() has been called. */
static my_bool my_thread_global_init_done= 0;
/* True if THR_KEY_mysys is created */
my_bool my_thr_key_mysys_exists= 0;


/*
  These are mutexes not used by safe_mutex or my_thr_init.c

  We want to free these earlier than other mutex so that safe_mutex
  can detect if all mutex and memory is freed properly.
*/

static void my_thread_init_common_mutex(void)
{
  mysql_mutex_init(key_THR_LOCK_open, &THR_LOCK_open, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_lock, &THR_LOCK_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_myisam, &THR_LOCK_myisam, MY_MUTEX_INIT_SLOW);
  mysql_mutex_init(key_THR_LOCK_myisam_mmap, &THR_LOCK_myisam_mmap, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_heap, &THR_LOCK_heap, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_net, &THR_LOCK_net, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_charset, &THR_LOCK_charset, MY_MUTEX_INIT_FAST);
#if !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R)
  mysql_mutex_init(key_LOCK_localtime_r, &LOCK_localtime_r, MY_MUTEX_INIT_SLOW);
#endif
}

void my_thread_destroy_common_mutex(void)
{
  mysql_mutex_destroy(&THR_LOCK_open);
  mysql_mutex_destroy(&THR_LOCK_lock);
  mysql_mutex_destroy(&THR_LOCK_myisam);
  mysql_mutex_destroy(&THR_LOCK_myisam_mmap);
  mysql_mutex_destroy(&THR_LOCK_heap);
  mysql_mutex_destroy(&THR_LOCK_net);
  mysql_mutex_destroy(&THR_LOCK_charset);
#if !defined(HAVE_LOCALTIME_R) || !defined(HAVE_GMTIME_R)
  mysql_mutex_destroy(&LOCK_localtime_r);
#endif
}


/*
  These mutexes are used by my_thread_init() and after
  my_thread_destroy_mutex()
*/

static void my_thread_init_internal_mutex(void)
{
  mysql_mutex_init(key_THR_LOCK_threads, &THR_LOCK_threads, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_THR_LOCK_malloc, &THR_LOCK_malloc, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_THR_COND_threads, &THR_COND_threads, NULL);
}


void my_thread_destroy_internal_mutex(void)
{
  mysql_mutex_destroy(&THR_LOCK_threads);
  mysql_mutex_destroy(&THR_LOCK_malloc);
  mysql_cond_destroy(&THR_COND_threads);
}

static void my_thread_init_thr_mutex(struct st_my_thread_var *var)
{
  mysql_mutex_init(key_my_thread_var_mutex, &var->mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_my_thread_var_suspend, &var->suspend, NULL);
}

static void my_thread_destory_thr_mutex(struct st_my_thread_var *var)
{
  mysql_mutex_destroy(&var->mutex);
  mysql_cond_destroy(&var->suspend);
}


/**
  Re-initialize components initialized early with @c my_thread_global_init.
  Some mutexes were initialized before the instrumentation.
  Destroy + create them again, now that the instrumentation
  is in place.
  This is safe, since this function() is called before creating new threads,
  so the mutexes are not in use.
*/
void my_thread_global_reinit(void)
{
  struct st_my_thread_var *tmp;

  DBUG_ASSERT(my_thread_global_init_done);

#ifdef HAVE_PSI_INTERFACE
  my_init_mysys_psi_keys();
#endif

  my_thread_destroy_common_mutex();
  my_thread_init_common_mutex();

  my_thread_destroy_internal_mutex();
  my_thread_init_internal_mutex();

  tmp= my_pthread_getspecific(struct st_my_thread_var*, THR_KEY_mysys);
  DBUG_ASSERT(tmp);

  my_thread_destory_thr_mutex(tmp);
  my_thread_init_thr_mutex(tmp);
}

/*
  initialize thread environment

  SYNOPSIS
    my_thread_global_init()

  RETURN
    0  ok
    1  error (Couldn't create THR_KEY_mysys)
*/

my_bool my_thread_global_init(void)
{
  int pth_ret;

  /* Normally this should never be called twice */
  DBUG_ASSERT(my_thread_global_init_done == 0);
  if (my_thread_global_init_done)
    return 0;
  my_thread_global_init_done= 1;

  /*
    THR_KEY_mysys is deleted in my_end() as DBUG libraries are using it even
    after my_thread_global_end() is called.
    my_thr_key_mysys_exist is used to protect against application like QT
    that calls my_thread_global_init() + my_thread_global_end() multiple times
    without calling my_init() + my_end().
  */
  if (!my_thr_key_mysys_exists &&
      (pth_ret= pthread_key_create(&THR_KEY_mysys, NULL)) != 0)
  {
    fprintf(stderr, "Can't initialize threads: error %d\n", pth_ret);
    return 1;
  }
  my_thr_key_mysys_exists= 1;

  /* Mutex used by my_thread_init() and after my_thread_destroy_mutex() */
  my_thread_init_internal_mutex();

  if (my_thread_init())
    return 1;

  thd_lib_detected= get_thread_lib();

  my_thread_init_common_mutex();

  return 0;
}


/**
   End the mysys thread system. Called when ending the last thread
*/

void my_thread_global_end(void)
{
  struct timespec abstime;
  my_bool all_threads_killed= 1;

  set_timespec(abstime, my_thread_end_wait_time);
  mysql_mutex_lock(&THR_LOCK_threads);
  while (THR_thread_count > 0)
  {
    int error= mysql_cond_timedwait(&THR_COND_threads, &THR_LOCK_threads,
                                    &abstime);
    if (error == ETIMEDOUT || error == ETIME)
    {
#ifdef HAVE_PTHREAD_KILL
      /*
        We shouldn't give an error here, because if we don't have
        pthread_kill(), programs like mysqld can't ensure that all threads
        are killed when we enter here.
      */
      if (THR_thread_count)
        fprintf(stderr,
                "Error in my_thread_global_end(): %d threads didn't exit\n",
                THR_thread_count);
#endif
      all_threads_killed= 0;
      break;
    }
  }
  mysql_mutex_unlock(&THR_LOCK_threads);

  my_thread_destroy_common_mutex();

  /*
    Only destroy the mutex & conditions if we don't have other threads around
    that could use them.
  */
  if (all_threads_killed)
  {
    my_thread_destroy_internal_mutex();
  }
  my_thread_global_init_done= 0;
}

static my_thread_id thread_id= 0;

/*
  Allocate thread specific memory for the thread, used by mysys and dbug

  SYNOPSIS
    my_thread_init()

  NOTES
    We can't use mutex_locks here if we are using windows as
    we may have compiled the program with SAFE_MUTEX, in which
    case the checking of mutex_locks will not work until
    the pthread_self thread specific variable is initialized.

   This function may called multiple times for a thread, for example
   if one uses my_init() followed by mysql_server_init().

  RETURN
    0  ok
    1  Fatal error; mysys/dbug functions can't be used
*/

my_bool my_thread_init(void)
{
  struct st_my_thread_var *tmp;
  my_bool error=0;

  if (!my_thread_global_init_done)
    return 1; /* cannot proceed with uninitialized library */

#ifdef EXTRA_DEBUG_THREADS
  fprintf(stderr,"my_thread_init(): pthread_self: %p\n", pthread_self());
#endif  

  if (my_pthread_getspecific(struct st_my_thread_var *,THR_KEY_mysys))
  {
#ifdef EXTRA_DEBUG_THREADS
    fprintf(stderr,"my_thread_init() called more than once in thread 0x%lx\n",
            (long) pthread_self());
#endif    
    goto end;
  }

#ifdef _MSC_VER
  install_sigabrt_handler();
#endif

  if (!(tmp= (struct st_my_thread_var *) calloc(1, sizeof(*tmp))))
  {
    error= 1;
    goto end;
  }
  pthread_setspecific(THR_KEY_mysys,tmp);
  tmp->pthread_self= pthread_self();
  my_thread_init_thr_mutex(tmp);

  tmp->stack_ends_here= (char*)&tmp +
                         STACK_DIRECTION * (long)my_thread_stack_size;

  mysql_mutex_lock(&THR_LOCK_threads);
  tmp->id= tmp->dbug_id= ++thread_id;
  ++THR_thread_count;
  mysql_mutex_unlock(&THR_LOCK_threads);
  tmp->init= 1;
#ifndef DBUG_OFF
  /* Generate unique name for thread */
  (void) my_thread_name();
#endif

end:
  return error;
}


/*
  Deallocate memory used by the thread for book-keeping

  SYNOPSIS
    my_thread_end()

  NOTE
    This may be called multiple times for a thread.
    This happens for example when one calls 'mysql_server_init()'
    mysql_server_end() and then ends with a mysql_end().
*/

void my_thread_end(void)
{
  struct st_my_thread_var *tmp;
  tmp= my_pthread_getspecific(struct st_my_thread_var*,THR_KEY_mysys);

#ifdef EXTRA_DEBUG_THREADS
  fprintf(stderr,"my_thread_end(): tmp: %p  pthread_self: %p  thread_id: %ld\n",
	  tmp, pthread_self(), tmp ? (long) tmp->id : 0L);
#endif  

  /*
    Remove the instrumentation for this thread.
    This must be done before trashing st_my_thread_var,
    because the LF_HASH depends on it.
  */
  PSI_CALL_delete_current_thread();

  /*
    We need to disable DBUG early for this thread to ensure that the
    the mutex calls doesn't enable it again
    To this we have to both do DBUG_POP() and also reset THR_KEY_mysys
    as the key is used by DBUG.
  */
  DBUG_POP();
  pthread_setspecific(THR_KEY_mysys,0);

  if (tmp && tmp->init)
  {
#if !defined(DBUG_OFF)
    /* tmp->dbug is allocated inside DBUG library */
    if (tmp->dbug)
    {
      free(tmp->dbug);
      tmp->dbug=0;
    }
#endif
    my_thread_destory_thr_mutex(tmp);

    /*
      Decrement counter for number of running threads. We are using this
      in my_thread_global_end() to wait until all threads have called
      my_thread_end and thus freed all memory they have allocated in
      my_thread_init() and DBUG_xxxx
    */
    mysql_mutex_lock(&THR_LOCK_threads);
    DBUG_ASSERT(THR_thread_count != 0);
    if (--THR_thread_count == 0)
      mysql_cond_signal(&THR_COND_threads);
    mysql_mutex_unlock(&THR_LOCK_threads);

    /* Trash variable so that we can detect false accesses to my_thread_var */
    tmp->init= 2;
    free(tmp);
  }
}

struct st_my_thread_var *_my_thread_var(void)
{
  return  my_pthread_getspecific(struct st_my_thread_var*,THR_KEY_mysys);
}

/****************************************************************************
  Get name of current thread.
****************************************************************************/

my_thread_id my_thread_dbug_id()
{
  /*
    We need to do this test as some system thread may not yet have called
    my_thread_init().
  */
  struct st_my_thread_var *tmp= my_thread_var;
  return tmp ? tmp->dbug_id : 0;
}

#ifdef DBUG_OFF
const char *my_thread_name(void)
{
  return "no_name";
}

#else

const char *my_thread_name(void)
{
  char name_buff[100];
  struct st_my_thread_var *tmp=my_thread_var;
  if (!tmp->name[0])
  {
    my_thread_id id= my_thread_dbug_id();
    sprintf(name_buff,"T@%lu", (ulong) id);
    strmake_buf(tmp->name, name_buff);
  }
  return tmp->name;
}

/* Return pointer to DBUG for holding current state */

extern void **my_thread_var_dbug()
{
  struct st_my_thread_var *tmp;
  if (!my_thread_global_init_done)
    return NULL;
  tmp= my_pthread_getspecific(struct st_my_thread_var*,THR_KEY_mysys);
  return tmp && tmp->init ? &tmp->dbug : 0;
}
#endif /* DBUG_OFF */

/* Return pointer to mutex_in_use */

safe_mutex_t **my_thread_var_mutex_in_use()
{
  struct st_my_thread_var *tmp;
  if (!my_thread_global_init_done)
    return NULL;
  tmp=  my_pthread_getspecific(struct st_my_thread_var*,THR_KEY_mysys);
  return tmp ? &tmp->mutex_in_use : 0;
}


#define SECONDS_TO_WAIT_FOR_KILL 2
#if !defined(__WIN__) && defined(HAVE_SELECT)
/* my_sleep() can wait for sub second times */
#define WAIT_FOR_KILL_TRY_TIMES 20
#else
#define WAIT_FOR_KILL_TRY_TIMES 2
#endif


void my_thread_interrupt_wait(struct st_my_thread_var *thread_var,
                              my_bool do_abort)
{
  if (!thread_var)
    return;

  mysql_mutex_lock(&thread_var->mutex);
  if (do_abort)         // Don't abort locks
    thread_var->abort= 1;

  /*
    This broadcast could be up in the air if the victim thread
    exits the cond in the time between read and broadcast, but that is
    ok since all we want to do is to make the victim thread get out
    of waiting on current_cond.
    If we see a non-zero current_cond: it cannot be an old value (because
    then exit_cond() should have run and it can't because we have mutex); so
    it is the true value but maybe current_mutex is not yet non-zero (we're
    in the middle of enter_cond() and there is a "memory order
    inversion"). So we test the mutex too to not lock 0.

    Note that there is a small chance we fail to kill. If victim has locked
    current_mutex, but hasn't yet entered enter_cond() (which means that
    current_cond and current_mutex are 0), then the victim will not get
    a signal and it may wait "forever" on the cond (until
    we issue a second KILL or the status it's waiting for happens).
    It's true that we have set its thd->killed but it may not
    see it immediately and so may have time to reach the cond_wait().

    However, where possible, we test for killed once again after
    enter_cond(). This should make the signaling as safe as possible.
    However, there is still a small chance of failure on platforms with
    instruction or memory write reordering.

    We have to do the loop with trylock, because if we would use
    pthread_mutex_lock(), we can cause a deadlock as we are here locking
    the thread_var->mutex and thread_var->current_mutex in a different order
    than in the thread we are trying to kill.
    We only sleep for 2 seconds as we don't want to have LOCK_thd_data
    locked too long time.

    There is a small change we may not succeed in aborting a thread that
    is not yet waiting for a mutex, but as this happens only for a
    thread that was doing something else when the kill was issued and
    which should detect the kill flag before it starts to wait, this
    should be good enough.
  */
  if (thread_var->current_cond && thread_var->current_mutex)
  {
    uint i;
    for (i= 0; i < WAIT_FOR_KILL_TRY_TIMES * SECONDS_TO_WAIT_FOR_KILL; i++)
    {
      int ret= mysql_mutex_trylock(thread_var->current_mutex);
      mysql_cond_broadcast(thread_var->current_cond);
      if (!ret)
      {
        /* Signal is sure to get through */
        mysql_mutex_unlock(thread_var->current_mutex);
        break;
      }
      my_sleep(1000000L / WAIT_FOR_KILL_TRY_TIMES);
    }
  }
  mysql_mutex_unlock(&thread_var->mutex);
}


static uint get_thread_lib(void)
{
#ifdef _CS_GNU_LIBPTHREAD_VERSION
  char buff[64];
    
  confstr(_CS_GNU_LIBPTHREAD_VERSION, buff, sizeof(buff));

  if (!strncasecmp(buff, "NPTL", 4))
    return THD_LIB_NPTL;
  if (!strncasecmp(buff, "linuxthreads", 12))
    return THD_LIB_LT;
#endif
  return THD_LIB_OTHER;
}

#ifdef _WIN32
/*
  In Visual Studio 2005 and later, default SIGABRT handler will overwrite
  any unhandled exception filter set by the application  and will try to
  call JIT debugger. This is not what we want, this we calling __debugbreak
  to stop in debugger, if process is being debugged or to generate 
  EXCEPTION_BREAKPOINT and then handle_segfault will do its magic.
*/

#if (_MSC_VER >= 1400)
static void my_sigabrt_handler(int sig)
{
  __debugbreak();
}
#endif /*_MSC_VER >=1400 */

static void install_sigabrt_handler(void)
{
#if (_MSC_VER >=1400)
  /*abort() should not override our exception filter*/
  _set_abort_behavior(0,_CALL_REPORTFAULT);
  signal(SIGABRT,my_sigabrt_handler);
#endif /* _MSC_VER >=1400 */
}
#endif

