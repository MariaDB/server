/* Copyright (c) 2007, 2013, Oracle and/or its affiliates.
   Copyright (c) 2012, 2014, SkySQL Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Implementation for the thread scheduler
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma implementation
#endif

#include "mariadb.h"
#include "mysqld.h"
#include "scheduler.h"
#include "sql_class.h"
#include "sql_callback.h"
#include <violite.h>

/** @internal
  Helper functions to allow mysys to call the thread scheduler when
  waiting for locks.
*/

/**@{*/
extern "C"
{
static void scheduler_wait_lock_begin(void) { 
  thd_wait_begin(NULL, THD_WAIT_TABLE_LOCK);
}

static void scheduler_wait_lock_end(void) {
  thd_wait_end(NULL);
}

static void scheduler_wait_sync_begin(void) {
  thd_wait_begin(NULL, THD_WAIT_SYNC);
}

static void scheduler_wait_sync_end(void) {
  thd_wait_end(NULL);
}

static void scheduler_wait_net_begin(void) {
   thd_wait_begin(NULL, THD_WAIT_NET);
}

static void scheduler_wait_net_end(void) {
   thd_wait_end(NULL);
}

};
/**@}*/

/**
  Common scheduler init function.

  The scheduler is either initialized by calling
  one_thread_scheduler() or one_thread_per_connection_scheduler() in
  mysqld.cc, so this init function will always be called.
 */

void scheduler_init()
{
  thr_set_lock_wait_callback(scheduler_wait_lock_begin,
                             scheduler_wait_lock_end);
  thr_set_sync_wait_callback(scheduler_wait_sync_begin,
                             scheduler_wait_sync_end);

  vio_set_wait_callback(scheduler_wait_net_begin,
    scheduler_wait_net_end);
}


/**
  Kill notification callback,  used by  one-thread-per-connection
  and threadpool scheduler.

  Wakes up a thread that is stuck in read/poll/epoll/event-poll 
  routines used by threadpool, such that subsequent attempt to 
  read from  client connection will result in IO error.
*/

void post_kill_notification(THD *thd)
{
  DBUG_ENTER("post_kill_notification");
  if (current_thd == thd || thd->system_thread)
    DBUG_VOID_RETURN;

  if (thd->net.vio)
    vio_shutdown(thd->net.vio, SHUT_RD);
  DBUG_VOID_RETURN;
}

/*
  Initialize scheduler for --thread-handling=one-thread-per-connection
*/

#ifndef EMBEDDED_LIBRARY

void one_thread_per_connection_scheduler(scheduler_functions *func,
    ulong *arg_max_connections,
    Atomic_counter<uint> *arg_connection_count)
{
  scheduler_init();
  func->max_threads= *arg_max_connections + 1;
  func->max_connections= arg_max_connections;
  func->connection_count= arg_connection_count;
  func->add_connection= create_thread_to_handle_connection;
  func->post_kill_notification= post_kill_notification;
}
#else
void handle_connection_in_main_thread(CONNECT *connect)
{
}
#endif

/*
  Initialize scheduler for --thread-handling=no-threads
*/

void one_thread_scheduler(scheduler_functions *func,
    Atomic_counter<uint> *arg_connection_count)
{
  scheduler_init();
  func->max_threads= 1;
  func->max_connections= &max_connections;
  func->connection_count= arg_connection_count;
  func->add_connection= handle_connection_in_main_thread;
}
