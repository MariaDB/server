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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

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
#ifdef HAVE_PSI_THREAD_INTERFACE
    psi_thread = PSI_THREAD_CALL(get_thread)();
#endif
    mysys_var= (st_my_thread_var *)pthread_getspecific(THR_KEY_mysys);
  }

  void restore()
  {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
    pthread_setspecific(THR_KEY_mysys,mysys_var);
    pthread_setspecific(THR_THD, 0);
    pthread_setspecific(THR_MALLOC, 0);
  }
};


#ifdef HAVE_PSI_INTERFACE

/*
  The following fixes PSI "idle" psi instrumentation.
  The server assumes that connection  becomes idle
  just before net_read_packet() and switches to active after it.
  In out setup, server becomes idle when async socket io is made.
*/

extern void net_before_header_psi(struct st_net *net, void *user_data, size_t);

static void dummy_before_header(struct st_net *, void *, size_t)
{
}

static void re_init_net_server_extension(THD *thd)
{
  thd->m_net_server_extension.m_before_header = dummy_before_header;
}

#else

#define re_init_net_server_extension(thd)

#endif /* HAVE_PSI_INTERFACE */


static inline void set_thd_idle(THD *thd)
{
  thd->net.reading_or_writing= 1;
#ifdef HAVE_PSI_INTERFACE
  net_before_header_psi(&thd->net, thd, 0);
#endif
}

/*
  Attach/associate the connection with the OS thread,
*/
static bool thread_attach(THD* thd)
{
  pthread_setspecific(THR_KEY_mysys,thd->mysys_var);
  thd->thread_stack=(char*)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->event_scheduler.m_psi);
#endif
  mysql_socket_set_thread_owner(thd->net.vio->mysql_socket);
  return 0;
}


int threadpool_add_connection(THD *thd)
{
  int retval=1;
  Worker_thread_context worker_context;
  worker_context.save();

  /*
    Create a new connection context: mysys_thread_var and PSI thread
    Store them in THD.
  */

  pthread_setspecific(THR_KEY_mysys, 0);
  my_thread_init();
  thd->mysys_var= (st_my_thread_var *)pthread_getspecific(THR_KEY_mysys);
  if (!thd->mysys_var)
  {
    /* Out of memory? */
    worker_context.restore();
    return 1;
  }

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
  thd->event_scheduler.m_psi=
    PSI_THREAD_CALL(new_thread)(key_thread_one_connection, thd, thd->thread_id);
#endif


  /* Login. */
  thread_attach(thd);
  re_init_net_server_extension(thd);
  ulonglong now= microsecond_interval_timer();
  thd->prior_thr_create_utime= now;
  thd->start_utime= now;
  thd->thr_create_utime= now;

  if (setup_connection_thread_globals(thd))
    goto end;

  if (thd_prepare_connection(thd))
    goto end;

  /*
    Check if THD is ok, as prepare_new_connection_state()
    can fail, for example if init command failed.
  */
  if (!thd_is_connection_alive(thd))
    goto end;

  retval= 0;
  thd->skip_wait_timeout= true;
  set_thd_idle(thd);

end:
  worker_context.restore();
  return retval;
}

/*
  threadpool_cleanup_connection() does the bulk of connection shutdown work.
  Usually called from threadpool_remove_connection(), but rarely it might
  be called also in the main polling thread if connection initialization fails.
*/
void threadpool_cleanup_connection(THD *thd)
{
  thd->net.reading_or_writing = 0;
  end_connection(thd);
  close_connection(thd, 0);
  unlink_thd(thd);
  mysql_cond_broadcast(&COND_thread_count);
}


void threadpool_remove_connection(THD *thd)
{
  Worker_thread_context worker_context;
  worker_context.save();
  thread_attach(thd);

  threadpool_cleanup_connection(thd);
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
    In the loop below, the flow is essentially the copy of thead-per-connections
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
    if (mysql_audit_release_required(thd))
      mysql_audit_release(thd);

    if ((retval= do_command(thd)) != 0)
      goto end;

    if (!thd_is_connection_alive(thd))
    {
      retval= 1;
      goto end;
    }

    set_thd_idle(thd);

    vio= thd->net.vio;
    if (!vio->has_data(vio))
    { 
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      goto end;
    }
  }

end:
  worker_context.restore();
  return retval;
}


static scheduler_functions tp_scheduler_functions=
{
  0,                                  // max_threads
  NULL,
  NULL,
  tp_init,                            // init
  NULL,                               // init_new_connection_thread
  tp_add_connection,                  // add_connection
  tp_wait_begin,                      // thd_wait_begin
  tp_wait_end,                        // thd_wait_end
  post_kill_notification,             // post_kill_notification
  NULL,                               // end_thread
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
