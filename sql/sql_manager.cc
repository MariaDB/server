/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.

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
 * sql_manager.cc
 * This thread manages various maintenance tasks.
 *
 *   o Flushing the tables every flush_time seconds.
 *   o Berkeley DB: removing unneeded log files.
 */

#include <my_global.h>
#include "sql_priv.h"
#include "sql_manager.h"
#include "sql_base.h"                           // flush_tables

static bool volatile manager_thread_in_use = 0;
static bool abort_manager = false;

pthread_t manager_thread;
mysql_mutex_t LOCK_manager;
mysql_cond_t COND_manager;

struct handler_cb {
   struct handler_cb *next;
   void (*action)(void *);
   void *data;
};

static struct handler_cb *cb_list; // protected by LOCK_manager

bool mysql_manager_submit(void (*action)(void *), void *data)
{
  bool result= FALSE;
  DBUG_ASSERT(manager_thread_in_use);
  struct handler_cb **cb;
  mysql_mutex_lock(&LOCK_manager);
  cb= &cb_list;
  while (*cb)
    cb= &(*cb)->next;
  *cb= (struct handler_cb *)my_malloc(sizeof(struct handler_cb), MYF(MY_WME));
  if (!*cb)
    result= TRUE;
  else
  {
    (*cb)->next= NULL;
    (*cb)->action= action;
    (*cb)->data= data;
  }
  mysql_cond_signal(&COND_manager);
  mysql_mutex_unlock(&LOCK_manager);
  return result;
}

pthread_handler_t handle_manager(void *arg __attribute__((unused)))
{
  int error = 0;
  struct timespec abstime;
  bool reset_flush_time = TRUE;
  my_thread_init();
  DBUG_ENTER("handle_manager");

  pthread_detach_this_thread();
  manager_thread = pthread_self();
  mysql_mutex_lock(&LOCK_manager);
  while (!abort_manager)
  {
    /* XXX: This will need to be made more general to handle different
     * polling needs. */
    if (flush_time)
    {
      if (reset_flush_time)
      {
	set_timespec(abstime, flush_time);
        reset_flush_time = FALSE;
      }
      while ((!error || error == EINTR) && !abort_manager && !cb_list)
        error= mysql_cond_timedwait(&COND_manager, &LOCK_manager, &abstime);

      if (error == ETIMEDOUT || error == ETIME)
      {
        tc_purge();
        error = 0;
        reset_flush_time = TRUE;
      }
    }
    else
    {
      while ((!error || error == EINTR) && !abort_manager && !cb_list)
        error= mysql_cond_wait(&COND_manager, &LOCK_manager);
    }

    struct handler_cb *cb= cb_list;
    cb_list= NULL;
    mysql_mutex_unlock(&LOCK_manager);

    while (cb)
    {
      struct handler_cb *next= cb->next;
      cb->action(cb->data);
      my_free(cb);
      cb= next;
    }
    mysql_mutex_lock(&LOCK_manager);
  }
  manager_thread_in_use = 0;
  mysql_mutex_unlock(&LOCK_manager);
  mysql_mutex_destroy(&LOCK_manager);
  mysql_cond_destroy(&COND_manager);
  DBUG_LEAVE; // Can't use DBUG_RETURN after my_thread_end
  my_thread_end();
  return (NULL);
}


/* Start handle manager thread */
void start_handle_manager()
{
  DBUG_ENTER("start_handle_manager");
  abort_manager = false;
  {
    pthread_t hThread;
    int err;
    manager_thread_in_use = 1;
    mysql_cond_init(key_COND_manager, &COND_manager,NULL);
    mysql_mutex_init(key_LOCK_manager, &LOCK_manager, NULL);
    if ((err= mysql_thread_create(key_thread_handle_manager, &hThread,
                                  &connection_attrib, handle_manager, 0)))
      sql_print_warning("Can't create handle_manager thread (errno: %M)", err);
  }
  DBUG_VOID_RETURN;
}


/* Initiate shutdown of handle manager thread */
void stop_handle_manager()
{
  DBUG_ENTER("stop_handle_manager");
  if (manager_thread_in_use)
  {
    mysql_mutex_lock(&LOCK_manager);
    abort_manager = true;
    DBUG_PRINT("quit", ("initiate shutdown of handle manager thread: %lu",
                        (ulong)manager_thread));
    mysql_cond_signal(&COND_manager);
    mysql_mutex_unlock(&LOCK_manager);
  }
  DBUG_VOID_RETURN;
}

