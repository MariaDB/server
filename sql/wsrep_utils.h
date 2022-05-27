/* Copyright (C) 2013-2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#ifndef WSREP_UTILS_H
#define WSREP_UTILS_H

#include "wsrep_priv.h"
#include "wsrep_mysqld.h"

unsigned int wsrep_check_ip (const char* const addr, bool *is_ipv6);
size_t wsrep_guess_ip (char* buf, size_t buf_len);
namespace wsp {
class node_status
{
public:
  node_status() : status(wsrep::server_state::s_disconnected) {}
  void set(enum wsrep::server_state::state new_status,
           const wsrep::view* view= 0)
  {
    if (status != new_status || 0 != view)
    {
      wsrep_notify_status(new_status, view);
      status= new_status;
    }
  }
  enum wsrep::server_state::state get() const { return status; }
private:
  enum wsrep::server_state::state status;
};
} /* namespace wsp */

extern wsp::node_status local_status;

/* returns the length of the host part of the address string */
size_t wsrep_host_len(const char* addr, size_t addr_len);

namespace wsp {

class Address {
public:
  Address()
    : m_address_len(0), m_family(UNSPEC), m_port(0), m_valid(false)
  {
    memset(m_address, 0, sizeof(m_address));
  }
  Address(const char *addr_in)
    : m_address_len(0), m_family(UNSPEC), m_port(0), m_valid(false)
  {
    memset(m_address, 0, sizeof(m_address));
    parse_addr(addr_in);
  }
  bool is_valid() { return m_valid; }
  bool is_ipv6() { return (m_family == INET6); }

  const char* get_address() { return m_address; }
  size_t get_address_len() { return m_address_len; }
  int get_port() { return m_port; }
  void set_port(int port) { m_port= port; }

private:
  enum family {
    UNSPEC= 0,
    INET,                                       /* IPv4 */
    INET6,                                      /* IPv6 */
  };

  char   m_address[256];
  size_t m_address_len;
  family m_family;
  int    m_port;
  bool   m_valid;

  void parse_addr(const char *addr_in) {
    const char *start;
    const char *end;
    const char *port;
    const char* open_bracket= strchr(const_cast<char *>(addr_in), '[');
    const char* close_bracket= strchr(const_cast<char *>(addr_in), ']');
    const char* colon= strchr(const_cast<char *>(addr_in), ':');
    const char* dot= strchr(const_cast<char *>(addr_in), '.');

    int cc= colon_count(addr_in);

    if (open_bracket != NULL ||
        dot == NULL ||
        (colon != NULL && (dot == NULL || colon < dot)))
    {
      // This could be an IPv6 address or a hostname
      if (open_bracket != NULL) {
        /* Sanity check: Address with '[' must include ']' */
        if (close_bracket == NULL &&
            open_bracket < close_bracket)       /* Error: malformed address */
        {
          m_valid= false;
          return;
        }

        start= open_bracket + 1;
        end= close_bracket;

        /* Check for port */
        port= strchr(close_bracket, ':');
        if ((port != NULL) && parse_port(port + 1))
        {
          return;                               /* Error: invalid port */
        }
        m_family= INET6;
      }
      else
      {
        switch (cc) {
        case 0:
          /* Hostname with no port */
          start= addr_in;
          end= addr_in + strlen(addr_in);
          break;
        case 1:
          /* Hostname with port (host:port) */
          start= addr_in;
          end= colon;
          if (parse_port(colon + 1))
            return;                             /* Error: invalid port */
          break;
        default:
          /* IPv6 address */
          start= addr_in;
          end= addr_in + strlen(addr_in);
          m_family= INET6;
          break;
        }
      }
    } else {                                    /* IPv4 address or hostname */
      start= addr_in;
      if (colon != NULL) {                      /* Port */
        end= colon;
        if (parse_port(colon + 1))
          return;                               /* Error: invalid port */
      } else {
        end= addr_in + strlen(addr_in);
      }
    }

    size_t len= end - start;

    /* Safety */
    if (len >= sizeof(m_address))
    {
      // The supplied address is too large to fit into the internal buffer.
      m_valid= false;
      return;
    }

    memcpy(m_address, start, len);
    m_address[len]= '\0';
    m_address_len= ++ len;
    m_valid= true;
    return;
  }

  int colon_count(const char *addr) {
    int count= 0, i= 0;

    while(addr[i] != '\0')
    {
      if (addr[i] == ':') ++count;
      ++ i;
    }
    return count;
  }

  bool parse_port(const char *port) {
    errno= 0;                                   /* Reset the errno */
    m_port= strtol(port, NULL, 10);
    if (errno == EINVAL || errno == ERANGE)
    {
      m_port= 0;                                /* Error: invalid port */
      m_valid= false;
      return true;
    }
    return false;
  }
};

class Config_state
{
public:
  Config_state() : view_(), status_(wsrep::server_state::s_disconnected)
  {}

  void set(const wsrep::view& view)
  {
    wsrep_notify_status(status_, &view);

    lock();
    view_= view;
    unlock();
  }

  void set(enum wsrep::server_state::state status)
  {
    wsrep_notify_status(status);

    lock();
    status_= status;
    unlock();
  }

  const wsrep::view& get_view_info() const
  {
    return view_;
  }

  enum wsrep::server_state::state get_status() const
  {
    return status_;
  }

  int lock()
  {
    return mysql_mutex_lock(&LOCK_wsrep_config_state);
  }

  int unlock()
  {
    return mysql_mutex_unlock(&LOCK_wsrep_config_state);
  }

private:
  wsrep::view view_;
  enum wsrep::server_state::state status_;
};

} /* namespace wsp */

extern wsp::Config_state *wsrep_config_state;

namespace wsp {
/* a class to manage env vars array */
class env
{
private:
    size_t len_;
    char** env_;
    int    errno_;
    bool ctor_common(char** e);
    void dtor();
    env& operator =(env);
public:
    explicit env(char** env);
    explicit env(const env&);
    ~env();
    int append(const char* var); /* add a new env. var */
    int error() const { return errno_; }
    char** operator()() { return env_; }
};

/* A small class to run external programs. */
class process
{
private:
    const char* const str_;
    FILE*       io_;
    int         err_;
    pid_t       pid_;

public:
/*! @arg type is a pointer to a null-terminated string which  must  contain
         either  the  letter  'r'  for  reading  or the letter 'w' for writing.
    @arg env optional null-terminated vector of environment variables
 */
    process  (const char* cmd, const char* type, char** env);
    ~process ();

    FILE* pipe () { return io_;  }
    int   error() { return err_; }
    int   wait ();
    const char* cmd() { return str_; }
};

class thd
{
  class thd_init
  {
  public:
    thd_init()  { my_thread_init(); }
    ~thd_init() { my_thread_end();  }
  }
  init;

  thd (const thd&);
  thd& operator= (const thd&);

public:

  thd(my_bool wsrep_on, bool system_thread=false);
  ~thd();
  THD* const ptr;
};

class string
{
public:
    string() : string_(0) {}
    explicit string(size_t s) : string_(static_cast<char*>(malloc(s))) {}
    char* operator()() { return string_; }
    void set(char* str) { if (string_) free (string_); string_= str; }
    ~string() { set (0); }
private:
    char* string_;
};

/* scope level lock */
class auto_lock
{
public:
  auto_lock(mysql_mutex_t* m) : m_(m) { mysql_mutex_lock(m_); }
  ~auto_lock() { mysql_mutex_unlock(m_); }
private:
  mysql_mutex_t& operator =(mysql_mutex_t&);
  mysql_mutex_t* const m_;
};

#ifdef REMOVED
class lock
{
  pthread_mutex_t* const mtx_;

public:

  lock (pthread_mutex_t* mtx) : mtx_(mtx)
  {
    int err= pthread_mutex_lock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex lock failed: %s", strerror(err));
      abort();
    }
  }

  virtual ~lock ()
  {
    int err= pthread_mutex_unlock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex unlock failed: %s", strerror(err));
      abort();
    }
  }

  inline void wait (pthread_cond_t* cond)
  {
    pthread_cond_wait (cond, mtx_);
  }

private:

  lock (const lock&);
  lock& operator=(const lock&);

};

class monitor
{
  int             mutable refcnt;
  pthread_mutex_t mutable mtx;
  pthread_cond_t  mutable cond;

public:

  monitor() : refcnt(0)
  {
    pthread_mutex_init (&mtx, NULL);
    pthread_cond_init  (&cond, NULL);
  }

  ~monitor()
  {
    pthread_mutex_destroy (&mtx);
    pthread_cond_destroy  (&cond);
  }

  void enter() const
  {
    lock l(&mtx);

    while (refcnt)
    {
      l.wait(&cond);
    }
    refcnt++;
  }

  void leave() const
  {
    lock l(&mtx);

    refcnt--;
    if (refcnt == 0)
    {
      pthread_cond_signal (&cond);
    }
  }

private:

  monitor (const monitor&);
  monitor& operator= (const monitor&);
};

class critical
{
  const monitor& mon;

public:

  critical(const monitor& m) : mon(m) { mon.enter(); }

  ~critical() { mon.leave(); }

private:

  critical (const critical&);
  critical& operator= (const critical&);
};
#endif

} // namespace wsrep

#endif /* WSREP_UTILS_H */
