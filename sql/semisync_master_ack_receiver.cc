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

my_socket global_ack_signal_fd= -1;

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
  mysql_cond_init(key_COND_ack_receiver, &m_cond_reply, NULL);
  m_pid= 0;

  DBUG_VOID_RETURN;
}

void Ack_receiver::cleanup()
{
  DBUG_ENTER("Ack_receiver::~Ack_receiver");

  stop();
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond);
  mysql_cond_destroy(&m_cond_reply);

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
    signal_listener();            // Signal listener thread to stop
    mysql_cond_broadcast(&m_cond);

    while (m_status == ST_STOPPING)
      mysql_cond_wait(&m_cond, &m_mutex);

    DBUG_ASSERT(m_status == ST_DOWN);
    pthread_join(m_pid, NULL);
    m_pid= 0;
  }
  mysql_mutex_unlock(&m_mutex);

  DBUG_VOID_RETURN;
}

#ifndef DBUG_OFF
void static dbug_verify_no_duplicate_slaves(Slave_ilist *m_slaves, THD *thd)
{
  I_List_iterator<Slave> it(*m_slaves);
  Slave *slave;
  while ((slave= it++))
  {
    DBUG_ASSERT(slave->thd->variables.server_id != thd->variables.server_id);
  }
}
#else
#define dbug_verify_no_duplicate_slaves(A,B) do {} while(0)
#endif


bool Ack_receiver::add_slave(THD *thd)
{
  Slave *slave;
  DBUG_ENTER("Ack_receiver::add_slave");

  if (!(slave= new Slave))
    DBUG_RETURN(true);

  slave->active= 0;
  slave->thd= thd;
  slave->vio= *thd->net.vio;
  slave->vio.mysql_socket.m_psi= NULL;
  slave->vio.read_timeout= 1;                   // 1 ms

  mysql_mutex_lock(&m_mutex);

  dbug_verify_no_duplicate_slaves(&m_slaves, thd);

  m_slaves.push_back(slave);
  m_slaves_changed= true;
  mysql_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);

  signal_listener();  // Inform listener that there are new slaves

  DBUG_RETURN(false);
}

void Ack_receiver::remove_slave(THD *thd)
{
  I_List_iterator<Slave> it(m_slaves);
  Slave *slave;
  bool slaves_changed= 0;
  DBUG_ENTER("Ack_receiver::remove_slave");

  mysql_mutex_lock(&m_mutex);

  while ((slave= it++))
  {
    if (slave->thd == thd)
    {
      delete slave;
      slaves_changed= true;
      break;
    }
  }
  if (slaves_changed)
  {
    m_slaves_changed= true;
    mysql_cond_broadcast(&m_cond);
    /*
      Wait until Ack_receiver::run() acknowledges remove of slave
      As this is only sent under the mutex and after listners has
      been collected, we know that listener has ignored the found
      slave.
    */
    if (m_status != ST_DOWN)
      mysql_cond_wait(&m_cond_reply, &m_mutex);
  }
  mysql_mutex_unlock(&m_mutex);

  DBUG_VOID_RETURN;
}

inline void Ack_receiver::set_stage_info(const PSI_stage_info &stage)
{
  (void)MYSQL_SET_STAGE(stage.m_key, __FILE__, __LINE__);
}

void Ack_receiver::wait_for_slave_connection(THD *thd)
{
  thd->enter_cond(&m_cond, &m_mutex, &stage_waiting_for_semi_sync_slave,
                  0, __func__, __FILE__, __LINE__);

  while (m_status == ST_UP && m_slaves.is_empty())
    mysql_cond_wait(&m_cond, &m_mutex);

  thd->exit_cond(0, __func__, __FILE__, __LINE__);
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
  DBUG_ENTER("Ack_receiver::run");

  my_thread_init();

#ifdef HAVE_POLL
  Poll_socket_listener listener(m_slaves);
#else
  Select_socket_listener listener(m_slaves);
#endif //HAVE_POLL

  if (listener.got_error())
  {
    sql_print_error("Got error %M starting ack receiver thread",
                    listener.got_error());
    DBUG_VOID_RETURN;
  }
  listener.set_global_ack_signal_fd();

  sql_print_information("Starting ack receiver thread");
  thd->system_thread= SYSTEM_THREAD_SEMISYNC_MASTER_BACKGROUND;
  thd->store_globals();
  thd->security_ctx->skip_grants();
  thd->set_command(COM_DAEMON);
  init_net(&net, net_buff, REPLY_MESSAGE_MAX_LENGTH);

  /*
    Mark that we have to setup the listener. Note that only this functions can
    set m_slaves_changed to false
  */
  m_slaves_changed= true;

  while (1)
  {
    int ret, slave_count= 0;
    Slave *slave;

    mysql_mutex_lock(&m_mutex);
    if (unlikely(m_status != ST_UP))
      goto end;

    if (unlikely(m_slaves_changed))
    {
      if (unlikely(m_slaves.is_empty()))
      {
        m_slaves_changed= false;
        mysql_cond_broadcast(&m_cond_reply);      // Signal remove_slave
        wait_for_slave_connection(thd);
        /* Wait for slave unlocks m_mutex */
        continue;
      }

      set_stage_info(stage_waiting_for_semi_sync_ack_from_slave);
      if ((slave_count= listener.init_slave_sockets()) == 0)
      {
        mysql_mutex_unlock(&m_mutex);
        m_slaves_changed= true;
        continue;                               // Retry
      }
      if (slave_count < 0)
        goto end;
      m_slaves_changed= false;
      mysql_cond_broadcast(&m_cond_reply);      // Signal remove_slave
    }

#ifdef HAVE_POLL
      DBUG_PRINT("info", ("fd count %u", slave_count));
#else     
      DBUG_PRINT("info", ("fd count %u, max_fd %d", slave_count,
                          (int) listener.get_max_fd()));
#endif

    mysql_mutex_unlock(&m_mutex);
    ret= listener.listen_on_sockets();

    if (ret <= 0)
    {

      ret= DBUG_IF("rpl_semisync_simulate_select_error") ? -1 : ret;

      if (ret == -1 && errno != EINTR)
        sql_print_information("Failed to wait on semi-sync sockets, "
                              "error: errno=%d", socket_errno);
      continue;
    }

    listener.clear_signal();
    mysql_mutex_lock(&m_mutex);
    set_stage_info(stage_reading_semi_sync_ack);
    Slave_ilist_iterator it(m_slaves);
    while ((slave= it++))
    {
      if (slave->active &&
          ((slave->vio.read_pos < slave->vio.read_end) ||
           listener.is_socket_active(slave)))
      {
        ulong len;

        /* Semi-sync packets will always be sent with pkt_nr == 1 */
        net_clear(&net, 0);
        net.vio= &slave->vio;
        /*
          Set compress flag. This is needed to support
          Slave_compress_protocol flag enabled Slaves
        */
        net.compress= slave->thd->net.compress;

        if (unlikely(listener.is_socket_hangup(slave)))
        {
          if (global_system_variables.log_warnings > 2)
            sql_print_warning("Semisync ack receiver got hangup "
                              "from slave server-id %d",
                              slave->server_id());
          it.remove();
          m_slaves_changed= true;
          continue;
        }

        len= my_net_read(&net);
        if (likely(len != packet_error))
        {
          int res;
          res= repl_semisync_master->report_reply_packet(slave->server_id(),
                                                         net.read_pos, len);
          if (unlikely(res < 0))
          {
            /*
              Slave has sent COM_QUIT or other failure.
              Delete it from listener
            */
            it.remove();
            m_slaves_changed= true;
          }
        }
        else if (net.last_errno == ER_NET_READ_ERROR)
        {
          if (net.last_errno > 0 && global_system_variables.log_warnings > 2)
            sql_print_warning("Semisync ack receiver got error %d \"%s\" "
                              "from slave server-id %d",
                              net.last_errno, ER_DEFAULT(net.last_errno),
                              slave->server_id());
          it.remove();
          m_slaves_changed= true;
        }
      }
    }
    mysql_mutex_unlock(&m_mutex);
  }

end:
  sql_print_information("Stopping ack receiver thread");
  m_status= ST_DOWN;
  mysql_cond_broadcast(&m_cond);
  mysql_cond_broadcast(&m_cond_reply);
  listener.clear_global_ack_signal_fd();
  mysql_mutex_unlock(&m_mutex);

  delete thd;
  DBUG_VOID_RETURN;
}
