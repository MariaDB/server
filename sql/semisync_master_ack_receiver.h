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

#ifndef SEMISYNC_MASTER_ACK_RECEIVER_DEFINED
#define SEMISYNC_MASTER_ACK_RECEIVER_DEFINED

#include "my_global.h"
#include "my_pthread.h"
#include "sql_class.h"
#include "semisync.h"
#include "socketpair.h"
#include <vector>

struct Slave :public ilink
{
  THD *thd;
  Vio vio;
#ifdef HAVE_POLL
  uint m_fds_index;
#endif
  bool active;
  my_socket sock_fd() const { return vio.mysql_socket.fd; }
  uint server_id() const { return thd->variables.server_id; }
};

typedef I_List<Slave> Slave_ilist;
typedef I_List_iterator<Slave> Slave_ilist_iterator;

/**
  Ack_receiver is responsible to control ack receive thread and maintain
  slave information used by ack receive thread.

  There are mainly four operations on ack receive thread:
  start: start ack receive thread
  stop: stop ack receive thread
  add_slave: maintain a new semisync slave's information
  remove_slave: remove a semisync slave's information
 */

class Ack_receiver : public Repl_semi_sync_base
{
public:
  Ack_receiver();
  ~Ack_receiver() = default;
  void cleanup();
  /**
     Notify ack receiver to receive acks on the dump session.

     It adds the given dump thread into the slave list and wakes
     up ack thread if it is waiting for any slave coming.

     @param[in] thd  THD of a dump thread.

     @return it return false if succeeds, otherwise true is returned.
  */
  bool add_slave(THD *thd);

  /**
    Notify ack receiver not to receive ack on the dump session.

    it removes the given dump thread from slave list.

    @param[in] thd  THD of a dump thread.
  */
  void remove_slave(THD *thd);

  /**
    Start ack receive thread

    @return it return false if succeeds, otherwise true is returned.
  */
  bool start();

  /**
     Stop ack receive thread
  */
  void stop();

  /**
     The core of ack receive thread.

     It monitors all slaves' sockets and receives acks when they come.
  */
  void run();

  void set_trace_level(unsigned long trace_level)
  {
    m_trace_level= trace_level;
  }
  bool running()
  {
    return m_status != ST_DOWN;
  }

private:
  enum status {ST_UP, ST_DOWN, ST_STOPPING};
  enum status m_status;
  /*
    Protect m_status, m_slaves_changed and m_slaves. ack thread and other
    session may access the variables at the same time.
  */
  mysql_mutex_t m_mutex;
  mysql_cond_t m_cond, m_cond_reply;
  /* If slave list is updated(add or remove). */
  bool m_slaves_changed;

  Slave_ilist m_slaves;
  pthread_t m_pid;

/* Declare them private, so no one can copy the object. */
  Ack_receiver(const Ack_receiver &ack_receiver);
  Ack_receiver& operator=(const Ack_receiver &ack_receiver);

  void set_stage_info(const PSI_stage_info &stage);
  void wait_for_slave_connection(THD *thd);
};


extern my_socket global_ack_signal_fd;

class Ack_listener
{
public:
  my_socket local_read_signal;
  const Slave_ilist &m_slaves;
  int error;

  Ack_listener(const Slave_ilist &slaves)
    :local_read_signal(-1), m_slaves(slaves), error(0)
  {
    my_socket pipes[2];
#ifdef _WIN32
    error= create_socketpair(pipes);
#else
    if (!pipe(pipes))
    {
      fcntl(pipes[0], F_SETFL, O_NONBLOCK);
      fcntl(pipes[1], F_SETFL, O_NONBLOCK);
    }
    else
    {
      pipes[0]= pipes[1]= -1;
    }
#endif /* _WIN32 */
    local_read_signal= pipes[0];
    global_ack_signal_fd= pipes[1];
  }

  virtual ~Ack_listener()
  {
#ifdef _WIN32
    my_socket pipes[2];
    pipes[0]= local_read_signal;
    pipes[1]= global_ack_signal_fd;
    close_socketpair(pipes);
#else
    if (global_ack_signal_fd >= 0)
      close(global_ack_signal_fd);
    if (local_read_signal >= 0)
      close(local_read_signal);
#endif /* _WIN32 */
    global_ack_signal_fd= local_read_signal= -1;
  }

  int got_error()  { return error; }

  virtual bool has_signal_data()= 0;

  /* Clear data sent by signal_listener() to abort read */
  void clear_signal()
  {
    if (has_signal_data())
    {
      char buff[100];
      /* Clear the signal message */
#ifndef _WIN32
      (void) !read(local_read_signal, buff, sizeof(buff));
#else
      recv(local_read_signal, buff, sizeof(buff), 0);
#endif /* _WIN32 */
    }
  }
};

static inline void signal_listener()
{
#ifndef _WIN32
  my_write(global_ack_signal_fd, (uchar*) "a", 1, MYF(0));
#else
  send(global_ack_signal_fd, "a", 1, 0);
#endif /* _WIN32 */
}

#ifdef HAVE_POLL
#include <sys/poll.h>

class Poll_socket_listener final : public Ack_listener
{
private:
  std::vector<pollfd> m_fds;

public:
  Poll_socket_listener(const Slave_ilist &slaves)
    :Ack_listener(slaves)
  {}

  virtual ~Poll_socket_listener() = default;

  bool listen_on_sockets()
  {
    return poll(m_fds.data(), m_fds.size(), -1);
  }

  bool is_socket_active(const Slave *slave)
  {
    return m_fds[slave->m_fds_index].revents & POLLIN;
  }

  bool is_socket_hangup(const Slave *slave)
  {
    return m_fds[slave->m_fds_index].revents & POLLHUP;
  }

  void clear_socket_info(const Slave *slave)
  {
    m_fds[slave->m_fds_index].fd= -1;
    m_fds[slave->m_fds_index].events= 0;
  }

  bool has_signal_data() override
  {
    /* The signal fd is always first */
    return (m_fds[0].revents & POLLIN);
  }

  int init_slave_sockets()
  {
    Slave_ilist_iterator it(const_cast<Slave_ilist&>(m_slaves));
    Slave *slave;
    uint fds_index= 0;
    pollfd poll_fd;

    m_fds.clear();
    /* First put in the signal socket */
    poll_fd.fd= local_read_signal;
    poll_fd.events= POLLIN;
    m_fds.push_back(poll_fd);
    fds_index++;

    while ((slave= it++))
    {
      slave->active= 1;
      pollfd poll_fd;
      poll_fd.fd= slave->sock_fd();
      poll_fd.events= POLLIN;
      m_fds.push_back(poll_fd);
      slave->m_fds_index= fds_index++;
    }
    return fds_index;
  }
};

#else //NO POLL

class Select_socket_listener final : public Ack_listener
{
private:
  my_socket m_max_fd;
  fd_set m_init_fds;
  fd_set m_fds;

public:
  Select_socket_listener(const Slave_ilist &slaves)
    :Ack_listener(slaves), m_max_fd(INVALID_SOCKET)
  {}

  virtual ~Select_socket_listener() = default;

  bool listen_on_sockets()
  {
    /* Reinitialize the fds with active fds before calling select */
    m_fds= m_init_fds;
    /* select requires max fd + 1 for the first argument */
    return select((int) m_max_fd+1, &m_fds, NULL, NULL, NULL);
  }

  bool is_socket_active(const Slave *slave)
  {
    return FD_ISSET(slave->sock_fd(), &m_fds);
  }

  bool is_socket_hangup(const Slave *slave)
  {
    return 0;
  }

  bool has_signal_data() override
  {
    return FD_ISSET(local_read_signal, &m_fds);
  }

  void clear_socket_info(const Slave *slave)
  {
    FD_CLR(slave->sock_fd(), &m_init_fds);
  }

  int init_slave_sockets()
  {
    Slave_ilist_iterator it(const_cast<Slave_ilist&>(m_slaves));
    Slave *slave;
    uint fds_index= 0;

    FD_ZERO(&m_init_fds);
    m_max_fd= -1;

    /* First put in the signal socket */
    FD_SET(local_read_signal, &m_init_fds);
    fds_index++;
    set_if_bigger(m_max_fd, local_read_signal);
#ifndef _WIN32
    if (local_read_signal > FD_SETSIZE)
    {
      int socket_id= local_read_signal;
      sql_print_error("Semisync slave socket fd is %u. "
                      "select() cannot handle if the socket fd is "
                      "greater than %u (FD_SETSIZE).", socket_id, FD_SETSIZE);
      return -1;
    }
#endif

    while ((slave= it++))
    {
      my_socket socket_id= slave->sock_fd();
      set_if_bigger(m_max_fd, socket_id);
#ifndef _WIN32
      if (socket_id > FD_SETSIZE)
      {
        sql_print_error("Semisync slave socket fd is %u. "
                        "select() cannot handle if the socket fd is "
                        "greater than %u (FD_SETSIZE).", socket_id, FD_SETSIZE);
        it.remove();
        continue;
      }
#endif //_WIN32
      FD_SET(socket_id, &m_init_fds);
      fds_index++;
      slave->active= 1;
    }
    return fds_index;
  }
  my_socket get_max_fd() { return m_max_fd; }
};

#endif //HAVE_POLL

extern Ack_receiver ack_receiver;
#endif
