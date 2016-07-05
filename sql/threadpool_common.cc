/* Copyright (C) 2012 Monty Program Ab

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

#include <my_global.h>
#include <violite.h>
#include <sql_priv.h>
#include <sql_class.h>
#include <my_pthread.h>
#include <scheduler.h>
#include <sql_connect.h>
#include <sql_audit.h>
#include <debug_sync.h>
#include <threadpool.h>


/* Threadpool parameters */

uint threadpool_min_threads;
uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_max_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;

/* Stats */
TP_STATISTICS tp_stats;


extern "C" pthread_key(struct st_my_thread_var*, THR_KEY_mysys);
extern bool do_command(THD*);

/*
  Worker threads contexts, and THD contexts.
  =========================================
  
  Both worker threads and connections have their sets of thread local variables 
  At the moment it is mysys_var (this has specific data for dbug, my_error and 
  similar goodies), and PSI per-client structure.

  Whenever query is executed following needs to be done:

  1. Save worker thread context.
  2. Change TLS variables to connection specific ones using thread_attach(THD*).
     This function does some additional work , e.g setting up 
     thread_stack/thread_ends_here pointers.
  3. Process query
  4. Restore worker thread context.

  Connection login and termination follows similar schema w.r.t saving and 
  restoring contexts. 

  For both worker thread, and for the connection, mysys variables are created 
  using my_thread_init() and freed with my_thread_end().

*/
struct Worker_thread_context
{
  PSI_thread *psi_thread;
  st_my_thread_var* mysys_var;

  void save()
  {
#ifdef HAVE_PSI_INTERFACE
    psi_thread=  PSI_server?PSI_server->get_thread():0;
#endif
    mysys_var= (st_my_thread_var *)pthread_getspecific(THR_KEY_mysys);
  }

  void restore()
  {
#ifdef HAVE_PSI_INTERFACE
    if (PSI_server)
      PSI_server->set_thread(psi_thread);
#endif
    pthread_setspecific(THR_KEY_mysys,mysys_var);
    pthread_setspecific(THR_THD, 0);
  }
};


/*
  Attach/associate the connection with the OS thread,
*/
static void thread_attach(THD* thd)
{
  pthread_setspecific(THR_KEY_mysys,thd->mysys_var);
  thd->thread_stack=(char*)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
    PSI_server->set_thread(thd->event_scheduler.m_psi);
#endif
}


THD* threadpool_add_connection(CONNECT *connect, void *scheduler_data)
{
  THD *thd= NULL;
  int error=1;

  Worker_thread_context worker_context;
  worker_context.save();

  /*
    Create a new connection context: mysys_thread_var and PSI thread
    Store them in THD.
  */

  pthread_setspecific(THR_KEY_mysys, 0);
  my_thread_init();
  st_my_thread_var* mysys_var= (st_my_thread_var *)pthread_getspecific(THR_KEY_mysys);
  if (!mysys_var ||!(thd= connect->create_thd(NULL)))
  {
    /* Out of memory? */
    connect->close_and_delete();
    if (mysys_var)
    {
#ifdef HAVE_PSI_INTERFACE
      /*
       current PSI is still from worker thread.
       Set to 0, to avoid premature cleanup by my_thread_end
      */
      if (PSI_server) PSI_server->set_thread(0);
#endif
      my_thread_end();
    }
    worker_context.restore();
    return NULL;
  }
  delete connect;
  add_to_active_threads(thd);
  thd->mysys_var= mysys_var;
  thd->event_scheduler.data= scheduler_data;

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
  {
    thd->event_scheduler.m_psi = 
      PSI_server->new_thread(key_thread_one_connection, thd, thd->thread_id);
  }
#endif


  /* Login. */
  thread_attach(thd);
  ulonglong now= microsecond_interval_timer();
  thd->prior_thr_create_utime= now;
  thd->start_utime= now;
  thd->thr_create_utime= now;

  if (!setup_connection_thread_globals(thd))
  {
    if (!login_connection(thd))
    {
      prepare_new_connection_state(thd);
      
      /* 
        Check if THD is ok, as prepare_new_connection_state()
        can fail, for example if init command failed.
      */
      if (thd_is_connection_alive(thd))
      {
        error= 0;
        thd->net.reading_or_writing= 1;
        thd->skip_wait_timeout= true;
      }
    }
  }
  if (error)
  {
    threadpool_remove_connection(thd);
    thd= NULL;
  }
  worker_context.restore();
  return thd;
}


void threadpool_remove_connection(THD *thd)
{
  Worker_thread_context worker_context;
  worker_context.save();
  thread_attach(thd);

  thd->net.reading_or_writing = 0;
  end_connection(thd);
  close_connection(thd, 0);
  unlink_thd(thd);
  delete thd;

  /*
    Free resources associated with this connection: 
    mysys thread_var and PSI thread.
  */
  my_thread_end();

  worker_context.restore();
}

/**
 Process a single client request or a single batch.
*/
int threadpool_process_request(THD *thd)
{
  int retval= 0;
  Worker_thread_context  worker_context;
  worker_context.save();

  thread_attach(thd);

  if (thd->killed >= KILL_CONNECTION)
  {
    /* 
      killed flag was set by timeout handler 
      or KILL command. Return error.
    */
    retval= 1;
    goto end;
  }


  /*
    In the loop below, the flow is essentially the copy of
    thead-per-connections
    logic, see do_handle_one_connection() in sql_connect.c

    The goal is to execute a single query, thus the loop is normally executed 
    only once. However for SSL connections, it can be executed multiple times 
    (SSL can preread and cache incoming data, and vio->has_data() checks if it 
    was the case).
  */
  for(;;)
  {
    Vio *vio;
    thd->net.reading_or_writing= 0;
    mysql_audit_release(thd);

    if ((retval= do_command(thd)) != 0)
      goto end;

    if (!thd_is_connection_alive(thd))
    {
      retval= 1;
      goto end;
    }

    vio= thd->net.vio;
    if (!vio->has_data(vio))
    { 
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      thd->net.reading_or_writing= 1;
      goto end;
    }
  }

end:
  worker_context.restore();
  return retval;
}



/* Dummy functions, do nothing */

static bool tp_init_new_connection_thread()
{
  return 0;
}

static bool tp_end_thread(THD *, bool)
{
  return 0;
}

static scheduler_functions tp_scheduler_functions=
{
  0,                                  // max_threads
  NULL,
  NULL,
  tp_init,                            // init
  tp_init_new_connection_thread,      // init_new_connection_thread
  tp_add_connection,                  // add_connection
  tp_wait_begin,                      // thd_wait_begin
  tp_wait_end,                        // thd_wait_end
  post_kill_notification,             // post_kill_notification
  tp_end_thread,                      // Dummy function
  tp_end                              // end
};

void pool_of_threads_scheduler(struct scheduler_functions *func,
    ulong *arg_max_connections,
    uint *arg_connection_count)
{
  *func = tp_scheduler_functions;
  func->max_threads= threadpool_max_threads;
  func->max_connections= arg_max_connections;
  func->connection_count= arg_connection_count;
  scheduler_init();
}
