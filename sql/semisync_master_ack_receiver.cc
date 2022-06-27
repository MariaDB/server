/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

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

#include <my_global.h>
#include "semisync_master.h"
#include "semisync_master_ack_receiver.h"

#ifdef HAVE_PSI_MUTEX_INTERFACE
extern PSI_mutex_key key_LOCK_ack_receiver;
extern PSI_cond_key key_COND_ack_receiver;
#endif
#ifdef HAVE_PSI_THREAD_INTERFACE
extern PSI_thread_key key_thread_ack_receiver;
#endif
extern Repl_semi_sync_master repl_semisync;

/* Callback function of ack receive thread */
pthread_handler_t ack_receive_handler(void *arg)
{
  Ack_receiver *recv= reinterpret_cast<Ack_receiver *>(arg);

  my_thread_init();
  recv->run();
  my_thread_end();

  return NULL;
}

Ack_receiver::Ack_receiver()
{
  DBUG_ENTER("Ack_receiver::Ack_receiver");

  m_status= ST_DOWN;
  mysql_mutex_init(key_LOCK_ack_receiver, &m_mutex, NULL);
  mysql_cond_init(key_COND_ack_receiver, &m_cond, NULL);
  m_pid= 0;

  DBUG_VOID_RETURN;
}

void Ack_receiver::cleanup()
{
  DBUG_ENTER("Ack_receiver::~Ack_receiver");

  stop();
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond);

  DBUG_VOID_RETURN;
}

bool Ack_receiver::start()
{
  DBUG_ENTER("Ack_receiver::start");

  mysql_mutex_lock(&m_mutex);
  if(m_status == ST_DOWN)
  {
    pthread_attr_t attr;

    m_status= ST_UP;

    if (DBUG_IF("rpl_semisync_simulate_create_thread_failure") ||
        pthread_attr_init(&attr) != 0 ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) != 0 ||
#ifndef _WIN32
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) != 0 ||
#endif
        mysql_thread_create(key_thread_ack_receiver, &m_pid,
                            &attr, ack_receive_handler, this))
    {
      sql_print_error("Failed to start semi-sync ACK receiver thread, "
                      " could not create thread(errno:%d)", errno);

      m_status= ST_DOWN;
      mysql_mutex_unlock(&m_mutex);

      DBUG_RETURN(true);
    }
    (void) pthread_attr_destroy(&attr);
  }
  mysql_mutex_unlock(&m_mutex);

  DBUG_RETURN(false);
}

void Ack_receiver::stop()
{
  DBUG_ENTER("Ack_receiver::stop");

  mysql_mutex_lock(&m_mutex);
  if (m_status == ST_UP)
  {
    m_status= ST_STOPPING;
    mysql_cond_broadcast(&m_cond);

    while (m_status == ST_STOPPING)
      mysql_cond_wait(&m_cond, &m_mutex);

    DBUG_ASSERT(m_status == ST_DOWN);

    m_pid= 0;
  }
  mysql_mutex_unlock(&m_mutex);

  DBUG_VOID_RETURN;
}

bool Ack_receiver::add_slave(THD *thd)
{
  Slave *slave;
  DBUG_ENTER("Ack_receiver::add_slave");

  if (!(slave= new Slave))
    DBUG_RETURN(true);

  slave->thd= thd;
  slave->vio= *thd->net.vio;
  slave->vio.mysql_socket.m_psi= NULL;
  slave->vio.read_timeout= 1;

  mysql_mutex_lock(&m_mutex);
  m_slaves.push_back(slave);
  m_slaves_changed= true;
  mysql_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);

  DBUG_RETURN(false);
}

void Ack_receiver::remove_slave(THD *thd)
{
  I_List_iterator<Slave> it(m_slaves);
  Slave *slave;
  DBUG_ENTER("Ack_receiver::remove_slave");

  mysql_mutex_lock(&m_mutex);

  while ((slave= it++))
  {
    if (slave->thd == thd)
    {
      delete slave;
      m_slaves_changed= true;
      break;
    }
  }
  mysql_mutex_unlock(&m_mutex);

  DBUG_VOID_RETURN;
}

inline void Ack_receiver::set_stage_info(const PSI_stage_info &stage)
{
  (void)MYSQL_SET_STAGE(stage.m_key, __FILE__, __LINE__);
}

inline void Ack_receiver::wait_for_slave_connection()
{
  set_stage_info(stage_waiting_for_semi_sync_slave);
  mysql_cond_wait(&m_cond, &m_mutex);
}

/* Auxilary function to initialize a NET object with given net buffer. */
static void init_net(NET *net, unsigned char *buff, unsigned int buff_len)
{
  memset(net, 0, sizeof(NET));
  net->max_packet= buff_len;
  net->buff= buff;
  net->buff_end= buff + buff_len;
  net->read_pos= net->buff;
}

void Ack_receiver::run()
{
  THD *thd= new THD(next_thread_id());
  NET net;
  unsigned char net_buff[REPLY_MESSAGE_MAX_LENGTH];

  my_thread_init();

  DBUG_ENTER("Ack_receiver::run");

#ifdef HAVE_POLL
  Poll_socket_listener listener(m_slaves);
#else
  Select_socket_listener listener(m_slaves);
#endif //HAVE_POLL

  sql_print_information("Starting ack receiver thread");
  thd->system_thread= SYSTEM_THREAD_SEMISYNC_MASTER_BACKGROUND;
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  thd->security_ctx->skip_grants();
  thd->set_command(COM_DAEMON);
  init_net(&net, net_buff, REPLY_MESSAGE_MAX_LENGTH);

  mysql_mutex_lock(&m_mutex);
  m_slaves_changed= true;
  mysql_mutex_unlock(&m_mutex);

  while (1)
  {
    int ret;
    uint slave_count __attribute__((unused))= 0;
    Slave *slave;

    mysql_mutex_lock(&m_mutex);
    if (unlikely(m_status == ST_STOPPING))
      goto end;

    set_stage_info(stage_waiting_for_semi_sync_ack_from_slave);
    if (unlikely(m_slaves_changed))
    {
      if (unlikely(m_slaves.is_empty()))
      {
        wait_for_slave_connection();
        mysql_mutex_unlock(&m_mutex);
        continue;
      }

      if ((slave_count= listener.init_slave_sockets()) == 0)
        goto end;
      m_slaves_changed= false;
#ifdef HAVE_POLL
      DBUG_PRINT("info", ("fd count %u", slave_count));
#else     
      DBUG_PRINT("info", ("fd count %u, max_fd %d", slave_count,
                          (int) listener.get_max_fd()));
#endif
    }

    ret= listener.listen_on_sockets();
    if (ret <= 0)
    {
      mysql_mutex_unlock(&m_mutex);

      ret= DBUG_IF("rpl_semisync_simulate_select_error") ? -1 : ret;

      if (ret == -1 && errno != EINTR)
        sql_print_information("Failed to wait on semi-sync sockets, "
                              "error: errno=%d", socket_errno);
      /* Sleep 1us, so other threads can catch the m_mutex easily. */
      my_sleep(1);
      continue;
    }

    set_stage_info(stage_reading_semi_sync_ack);
    Slave_ilist_iterator it(m_slaves);
    while ((slave= it++))
    {
      if (listener.is_socket_active(slave))
      {
        ulong len;

        net_clear(&net, 0);
        net.vio= &slave->vio;
        /*
          Set compress flag. This is needed to support
          Slave_compress_protocol flag enabled Slaves
        */
        net.compress= slave->thd->net.compress;

        len= my_net_read(&net);
        if (likely(len != packet_error))
          repl_semisync_master.report_reply_packet(slave->server_id(),
                                                   net.read_pos, len);
        else if (net.last_errno == ER_NET_READ_ERROR)
          listener.clear_socket_info(slave);
      }
    }
    mysql_mutex_unlock(&m_mutex);
  }
end:
  sql_print_information("Stopping ack receiver thread");
  m_status= ST_DOWN;
  delete thd;
  mysql_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);
  DBUG_VOID_RETURN;
}
