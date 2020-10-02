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
uint threadpool_mode;
uint threadpool_prio_kickup_timer;

/* Stats */
TP_STATISTICS tp_stats;


static void  threadpool_remove_connection(THD *thd);
static int   threadpool_process_request(THD *thd);
static THD*  threadpool_add_connection(CONNECT *connect, void *scheduler_data);

extern bool do_command(THD*);

static inline TP_connection *get_TP_connection(THD *thd)
{
  return (TP_connection *)thd->event_scheduler.data;
}

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
    mysys_var= my_thread_var;
  }

  void restore()
  {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
    set_mysys_var(mysys_var);
    pthread_setspecific(THR_THD, 0);
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
static void thread_attach(THD* thd)
{
  set_mysys_var(thd->mysys_var);
  thd->thread_stack=(char*)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->event_scheduler.m_psi);
#endif
  mysql_socket_set_thread_owner(thd->net.vio->mysql_socket);
}

/*
  Determine connection priority , using current 
  transaction state and 'threadpool_priority' variable value.
*/
static TP_PRIORITY get_priority(TP_connection *c)
{
  DBUG_ASSERT(c->thd == current_thd);
  TP_PRIORITY prio= (TP_PRIORITY)c->thd->variables.threadpool_priority;
  if (prio == TP_PRIORITY_AUTO)
  {
    return c->thd->transaction.is_active() ? TP_PRIORITY_HIGH : TP_PRIORITY_LOW;
  }
  return prio;
}


void tp_callback(TP_connection *c)
{
  DBUG_ASSERT(c);

  Worker_thread_context worker_context;
  worker_context.save();

  THD *thd= c->thd;

  c->state = TP_STATE_RUNNING;

  if (!thd)
  {
    /* No THD, need to login first. */
    DBUG_ASSERT(c->connect);
    thd= c->thd= threadpool_add_connection(c->connect, c);
    if (!thd)
    {
      /* Bail out on connect error.*/
      goto error;
    }
    c->connect= 0;
  }
  else if (threadpool_process_request(thd))
  {
    /* QUIT or an error occured. */
    goto error;
  }

  /* Set priority */
  c->priority= get_priority(c);

  /* Read next command from client. */
  c->set_io_timeout(thd->variables.net_wait_timeout);
  c->state= TP_STATE_IDLE;
  if (c->start_io())
    goto error;

  worker_context.restore();
  return;

error:
  c->thd= 0;
  delete c;

  if (thd)
  {
    threadpool_remove_connection(thd);
  }
  worker_context.restore();
}


static THD* threadpool_add_connection(CONNECT *connect, void *scheduler_data)
{
  THD *thd= NULL;

  /*
    Create a new connection context: mysys_thread_var and PSI thread
    Store them in THD.
  */

  set_mysys_var(NULL);
  my_thread_init();
  st_my_thread_var* mysys_var= my_thread_var;
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
    return NULL;
  }
  delete connect;
  add_to_active_threads(thd);
  thd->mysys_var= mysys_var;
  thd->event_scheduler.data= scheduler_data;

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

  thd->skip_wait_timeout= true;
  set_thd_idle(thd);
  return thd;

end:
  threadpool_remove_connection(thd);
  return NULL;
}


static void threadpool_remove_connection(THD *thd)
{
  thread_attach(thd);
  thd->event_scheduler.data= 0;
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
}


/*
  Ensure that proper error message is sent to client,
  and "aborted" message appears in the log in case of
  wait timeout.

  See also timeout handling in net_serv.cc
*/
static void handle_wait_timeout(THD *thd)
{
  thd->get_stmt_da()->reset_diagnostics_area();
  thd->reset_killed();
  my_error(ER_NET_READ_INTERRUPTED, MYF(0));
  thd->net.last_errno= ER_NET_READ_INTERRUPTED;
  thd->net.error= 2;
}

/** Check if some client data is cached in thd->net or thd->net.vio */
static bool has_unread_data(THD* thd)
{
  NET *net= &thd->net;
  if (net->compress && net->remain_in_buf)
    return true;
  Vio *vio= net->vio;
  return vio->has_data(vio);
}


/**
 Process a single client request or a single batch.
*/
static int threadpool_process_request(THD *thd)
{
  int retval= 0;
  thread_attach(thd);

  if (thd->killed >= KILL_CONNECTION)
  {
    /* 
      killed flag was set by timeout handler 
      or KILL command. Return error.
    */
    retval= 1;
    if(thd->killed == KILL_WAIT_TIMEOUT)
      handle_wait_timeout(thd);
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

    if (!has_unread_data(thd))
    { 
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      goto end;
    }
  }

end:
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

static TP_pool *pool;

static bool tp_init()
{

#ifdef _WIN32
  if (threadpool_mode == TP_MODE_WINDOWS)
    pool= new (std::nothrow) TP_pool_win;
  else
    pool= new (std::nothrow) TP_pool_generic;
#else
  pool= new (std::nothrow) TP_pool_generic;
#endif
  if (!pool)
    return true;
  if (pool->init())
  {
    delete pool;
    pool= 0;
    return true;
  }
  return false;
}

static void tp_add_connection(CONNECT *connect)
{
  TP_connection *c= pool->new_connection(connect);
  DBUG_EXECUTE_IF("simulate_failed_connection_1", delete c ; c= 0;);
  if (c)
    pool->add(c);
  else
    connect->close_and_delete();
}

int tp_get_idle_thread_count()
{
  return pool? pool->get_idle_thread_count(): 0;
}

int tp_get_thread_count()
{
  return pool ? pool->get_thread_count() : 0;
}

void tp_set_min_threads(uint val)
{
  if (pool)
    pool->set_min_threads(val);
}


void tp_set_max_threads(uint val)
{
  if (pool)
    pool->set_max_threads(val);
}

void tp_set_threadpool_size(uint val)
{
  if (pool)
    pool->set_pool_size(val);
}


void tp_set_threadpool_stall_limit(uint val)
{
  if (pool)
    pool->set_stall_limit(val);
}


void tp_timeout_handler(TP_connection *c)
{
  if (c->state != TP_STATE_IDLE)
    return;
  THD *thd=c->thd;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  thd->set_killed(KILL_WAIT_TIMEOUT);
  c->priority= TP_PRIORITY_HIGH;
  post_kill_notification(thd);
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}


static void tp_wait_begin(THD *thd, int type)
{
  TP_connection *c = get_TP_connection(thd);
  if (c)
    c->wait_begin(type);
}


static void tp_wait_end(THD *thd)
{
  TP_connection *c = get_TP_connection(thd);
  if (c)
    c->wait_end();
}


static void tp_end()
{
  delete pool;
}

static void tp_post_kill_notification(THD *thd)
{
  TP_connection *c= get_TP_connection(thd);
  if (c)
    c->priority= TP_PRIORITY_HIGH;
  post_kill_notification(thd);
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
  tp_post_kill_notification,          // post kill notification
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
